#include "VoxelEnvironmentLODPrototype.h"
#include "VoxelProductionCandidatePreparation.h"
#include "VoxelObjectRegistry.h"
#include "VoxelDetachedPersistence.h"
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "Misc/App.h"
#include "RHI.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace VisualOnlyFixture
{
struct FCommand final : IAutomationLatentCommand
{
    FAutomationTestBase* Test; UWorld* World; TWeakObjectPtr<AVoxelEnvironmentLODPrototype> Actor;
    VoxelObjects::FRegistry PrivateRegistry;
    VoxelObjects::FProductionReservation Reservation;
    TSharedRef<bool> Done=MakeShared<bool>(false),Success=MakeShared<bool>(false);
    double Started=FPlatformTime::Seconds();
    FCommand(FAutomationTestBase* T,UWorld* W,AVoxelEnvironmentLODPrototype* A):Test(T),World(W),Actor(A){}
    bool Cleanup(){if(auto A=Actor.Get()){A->CancelStagedObjectRestore();A->Destroy();}VoxelObjects::Forget(World);World->DestroyWorld(false);return true;}
    bool Update() override
    {
        auto A=Actor.Get();
        if(!A||FPlatformTime::Seconds()-Started>60.){Test->AddError(TEXT("visual-only fixture failed/timed out"));return Cleanup();}
        if(!*Done){A->AdvanceStagedObjectRestore();return false;}
        if(!Test->TestTrue(TEXT("hidden resources finish preparation"),*Success))return Cleanup();
        Test->TestFalse(TEXT("visual reveal requires logical commit"),A->ValidatePreparedVisualReveal());
        const auto Token=PrivateRegistry.PrepareProductionCommit(Reservation);
        if(!Test->TestTrue(TEXT("private registry prepares hidden commit"),PrivateRegistry.ValidatePreparedProduction(Token)))return Cleanup();
        const auto IntendedMaterial=A->Materials[0];
        auto ReplacementMaterial=UMaterialInstanceDynamic::Create(IntendedMaterial->Parent,A);
        A->Materials[0]=ReplacementMaterial;
        if(FApp::CanEverRender()&&!GUsingNullRHI)
            Test->TestFalse(TEXT("different MID with same parent refuses prepared material evidence"),PrivateRegistry.ValidatePreparedProduction(Token));
        Test->TestTrue(TEXT("material refusal leaves actor hidden"),A->IsHidden());
        A->Materials[0]=IntendedMaterial;
        Test->TestTrue(TEXT("exact intended material identity restores token validity"),PrivateRegistry.ValidatePreparedProduction(Token));
        if(FApp::CanEverRender()&&!GUsingNullRHI){
            IntendedMaterial->SetScalarParameterValue(TEXT("Fade"),0.f);
            Test->TestFalse(TEXT("same MID fade mutation refuses visual token"),PrivateRegistry.ValidatePreparedProduction(Token));
            Test->TestTrue(TEXT("fade refusal keeps preparation hidden"),A->IsHidden());
            IntendedMaterial->SetScalarParameterValue(TEXT("Fade"),1.f);
            IntendedMaterial->SetScalarParameterValue(TEXT("Reverse"),1.f);
            Test->TestFalse(TEXT("same MID reverse mutation refuses visual token"),PrivateRegistry.ValidatePreparedProduction(Token));
            IntendedMaterial->SetScalarParameterValue(TEXT("Reverse"),0.f);
            Test->TestTrue(TEXT("restored exact visibility parameters validate"),PrivateRegistry.ValidatePreparedProduction(Token));
            auto Section=A->FindComponentByClass<UProceduralMeshComponent>();
            if(!Test->TestNotNull(TEXT("prepared fixture has material-bearing section"),Section))return Cleanup();
            auto Assigned=Section->GetMaterial(0);
            Section->SetMaterial(0,ReplacementMaterial);Section->DoDeferredRenderUpdates_Concurrent();
            Test->TestFalse(TEXT("clean component with substituted material refuses token"),PrivateRegistry.ValidatePreparedProduction(Token));
            Test->TestTrue(TEXT("substituted material never becomes visible"),A->IsHidden());
            Section->SetMaterial(0,Assigned);Section->DoDeferredRenderUpdates_Concurrent();
            Test->TestTrue(TEXT("restored exact section material validates"),PrivateRegistry.ValidatePreparedProduction(Token));
        }
        auto Extra=NewObject<USceneComponent>(A);A->AddInstanceComponent(Extra);Extra->RegisterComponent();
        Test->TestFalse(TEXT("extra registered component refuses precommit visual contract"),PrivateRegistry.ValidatePreparedProduction(Token));
        Extra->DestroyComponent();
        A->GetRootComponent()->MarkRenderTransformDirty();
        Test->TestFalse(TEXT("dirty transform refuses precommit visual contract"),PrivateRegistry.ValidatePreparedProduction(Token));
        A->GetRootComponent()->DoDeferredRenderUpdates_Concurrent();
        if(!Test->TestTrue(TEXT("restored hidden component contract validates"),PrivateRegistry.ValidatePreparedProduction(Token)))return Cleanup();
        PrivateRegistry.CommitPreparedProduction(Token);
        Test->TestTrue(TEXT("logical commit still excluded from public discovery"),A->IsUnpublishedPreparation());
        Test->TestTrue(TEXT("committed visual actor remains hidden"),A->IsHidden());
        if(!Test->TestTrue(TEXT("committed preparation validates visual latch"),A->ValidatePreparedVisualReveal()))return Cleanup();
        A->PublishPreparedVisualOnly();
        Test->TestFalse(TEXT("visual latch unhides actor"),A->IsHidden());
        Test->TestTrue(TEXT("visual-only exclusion remains explicit"),A->IsVisualOnlyPreparation()&&A->IsUnpublishedPreparation());
        Test->TestFalse(TEXT("visual latch cannot replay"),A->ValidatePreparedVisualReveal());
        Test->TestFalse(TEXT("legacy reveal cannot enroll diagnostic actor"),A->PublishStagedObjectRestore());
        Test->TestFalse(TEXT("visual actor has no collision"),A->GetActorEnableCollision());
        Test->TestFalse(TEXT("visual actor has no gameplay tick"),A->IsActorTickEnabled());
        TInlineComponentArray<UActorComponent*> Components(A);
        for(auto C:Components)if(C->IsRegistered())Test->TestFalse(TEXT("registered component visibility updates submitted"),C->IsRenderStateDirty());
        FVoxelImmutableGeometry Geometry;TArray<uint8> Dynamic;
        Test->TestFalse(TEXT("public capture excludes visual actor"),A->CaptureObjectState(Geometry,Dynamic));
        Test->TestFalse(TEXT("visual actor cannot be carved"),A->Carve(A->GetActorLocation(),1));
        Test->TestFalse(TEXT("visual actor cannot be chopped"),A->CanChop(A->GetActorLocation()));
        Test->TestFalse(TEXT("visual actor supplies no terrain solidity"),A->SolidAt(A->GetActorLocation()));
        TArray<VoxelObjects::FEntry> Saved;
        Test->TestTrue(TEXT("visual latch does not poison global save"),VoxelDetachedPersistence::CaptureSnapshot(World,Saved));
        Test->TestEqual(TEXT("global registry never discovers visual actor"),VoxelObjects::Get(World).Num(),0);
        Test->TestTrue(TEXT("global snapshot stays empty"),Saved.IsEmpty());
        Test->TestEqual(TEXT("only private diagnostic registry owns stable ID"),PrivateRegistry.Num(),1);
        A->CancelStagedObjectRestore();
        Test->TestFalse(TEXT("cancelled committed state cannot reveal again"),A->ValidatePreparedVisualReveal());
        Test->TestTrue(TEXT("cancel hides diagnostic actor"),A->IsHidden());
        Test->TestTrue(TEXT("cancel retains exclusion"),A->IsUnpublishedPreparation());
        return Cleanup();
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEnvironmentVisualOnlyTest,"Voxel.Objects.EnvironmentVisualOnly",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelEnvironmentVisualOnlyTest::RunTest(const FString&)
{
    std::vector<uint8> Vxa;auto Word=[&](uint32 V){for(int I=0;I<4;++I)Vxa.push_back(uint8(V>>(8*I)));};
    for(uint32 V:{vxc::kVxaMagic,3u,0u,0u,0u,1u,1u,1u,100u,1u,0u,0u})Word(V);Vxa.push_back(16);Word(1);
    VoxelProductionCandidate::FWork Work;Work.Provenance={19,123,456,-13,4,10,5,1,2,3};
    Work.Descriptor.SpecId=TEXT("visual-only-canonical-cobble");Work.Descriptor.Kind=TEXT("rock");Work.Descriptor.Category=TEXT("environment");Work.Descriptor.SeedIndex=1;
    Work.CanonicalSourceHash=TEXT("22222222222222222222222222222222");
    if(!VoxelProductionCandidate::BuildImmutableSnapshot(Work,Vxa))return false;
    auto World=UWorld::CreateWorld(EWorldType::Game,false,FName(*FString::Printf(TEXT("VisualOnly_%s"),*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
    if(!World)return false;auto Actor=World->SpawnActor<AVoxelEnvironmentLODPrototype>();if(!Actor){World->DestroyWorld(false);return false;}
    Actor->MarkVisualOnlyPreparation();
    auto Command=MakeShared<VisualOnlyFixture::FCommand>(this,World,Actor);
    VoxelObjects::FEntry Entry;Entry.Kind=3;Entry.GeometryRevision=1;Entry.Geometry=Work.Geometry;Entry.Dynamic=Work.Dynamic;
    Entry.Transform=FTransform(FVector(-130.,40.,100.));Entry.bRetained=true;Entry.Lifetime.Kind=EVoxelDebrisLifetime::Retained;Entry.Actor=Actor;
    Command->Reservation=Command->PrivateRegistry.ReserveProduction(Work.Descriptor,Entry);
    if(!Command->Reservation.IsValid()){Command->Cleanup();return false;}
    const auto Done=Command->Done,Success=Command->Success;
    Actor->BeginStagedObjectRestore(Work.Geometry,Work.Dynamic,[Done,Success](bool Ready){*Done=true;*Success=Ready;});
    FAutomationTestFramework::Get().EnqueueLatentCommand(Command);return true;
}
#endif
