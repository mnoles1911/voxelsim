#include "VoxelEnvironmentLODPrototype.h"
#include "VoxelProductionCandidatePreparation.h"
#include "VoxelObjectRegistry.h"
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace {
struct FRenderReadinessResult {int32 Calls=0;bool Success=false;};
struct FEnvironmentRenderReadinessCommand : IAutomationLatentCommand {
    FAutomationTestBase* Test;UWorld* World;TWeakObjectPtr<AVoxelEnvironmentLODPrototype> Actor;
    FVoxelImmutableGeometry Geometry;TArray<uint8> Dynamic;
    TArray<TSharedRef<FRenderReadinessResult>> Results;
    bool SawFinalFence=false;double Started=FPlatformTime::Seconds();
    FEnvironmentRenderReadinessCommand(FAutomationTestBase* T,UWorld* W,AVoxelEnvironmentLODPrototype* A,FVoxelImmutableGeometry G,TArray<uint8> D):Test(T),World(W),Actor(A),Geometry(MoveTemp(G)),Dynamic(MoveTemp(D)){}
    void Begin(){auto Result=MakeShared<FRenderReadinessResult>();Results.Add(Result);Actor->BeginStagedObjectRestore(Geometry,Dynamic,[Result](bool Success){++Result->Calls;Result->Success=Success;});}
    bool Cleanup(){if(auto A=Actor.Get()){A->CancelStagedObjectRestore();A->Destroy();}VoxelObjects::Forget(World);World->DestroyWorld(false);return true;}
    bool Update() override {
        auto A=Actor.Get();if(!A||FPlatformTime::Seconds()-Started>60.){Test->AddError(TEXT("render readiness fixture failed/timed out"));return Cleanup();}
        if(Results.IsEmpty()){
            Begin();Test->TestFalse(TEXT("unprepared actor cannot publish"),A->PublishStagedObjectRestore());
            A->CancelStagedObjectRestore();
            Test->TestEqual(TEXT("early cancellation completes once"),Results[0]->Calls,1);
            Test->TestFalse(TEXT("early cancellation reports failure"),Results[0]->Success);
            Begin();return false;
        }
        A->AdvanceStagedObjectRestore();
        Test->TestTrue(TEXT("preparation remains hidden"),A->IsHidden());
        Test->TestFalse(TEXT("preparation collision remains disabled"),A->GetActorEnableCollision());
        Test->TestEqual(TEXT("obsolete worker cannot complete old request again"),Results[0]->Calls,1);
        if(A->IsStagedRenderResourcesPending()){
            Test->TestEqual(TEXT("fence stage cannot report premature completion"),Results.Last()->Calls,0);
            Test->TestFalse(TEXT("publish cannot bypass render acknowledgment"),A->PublishStagedObjectRestore());
            if(Results.Num()==2){
                A->CancelStagedObjectRestore();
                Test->TestEqual(TEXT("fence cancellation completes once"),Results[1]->Calls,1);
                Test->TestFalse(TEXT("fence cancellation fails preparation"),Results[1]->Success);
                Test->TestFalse(TEXT("cancelled fence cannot publish"),A->PublishStagedObjectRestore());
                Begin();return false;
            }
            SawFinalFence=true;return false;
        }
        if(Results.Num()==3&&Results.Last()->Calls){
            Test->TestTrue(TEXT("retry passed through explicit render acknowledgment"),SawFinalFence);
            Test->TestTrue(TEXT("retry completes after acknowledgment"),Results.Last()->Success);
            Test->TestEqual(TEXT("retry completes exactly once"),Results.Last()->Calls,1);
            Test->TestEqual(TEXT("cancelled old fence cannot complete replacement"),Results[1]->Calls,1);
            Test->TestTrue(TEXT("acknowledged preparation can explicitly publish"),A->PublishStagedObjectRestore());
            return Cleanup();
        }
        return false;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEnvironmentRenderReadinessTest,"Voxel.Objects.EnvironmentRenderReadiness",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelEnvironmentRenderReadinessTest::RunTest(const FString&){
    std::vector<uint8> Vxa;auto Word=[&](uint32 V){for(int I=0;I<4;++I)Vxa.push_back(uint8(V>>(8*I)));};
    for(uint32 V:{vxc::kVxaMagic,3u,0u,0u,0u,1u,1u,1u,100u,1u,0u,0u})Word(V);Vxa.push_back(16);Word(1);
    VoxelProductionCandidate::FWork Work;Work.Descriptor.SpecId=TEXT("render-ready-cobble");Work.Descriptor.Kind=TEXT("rock");Work.Descriptor.Category=TEXT("environment");
    if(!VoxelProductionCandidate::BuildImmutableSnapshot(Work,Vxa))return false;
    auto World=UWorld::CreateWorld(EWorldType::Game,false,FName(*FString::Printf(TEXT("RenderReadiness_%s"),*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
    if(!World)return false;auto Actor=World->SpawnActor<AVoxelEnvironmentLODPrototype>();if(!Actor){World->DestroyWorld(false);return false;}
    Actor->MarkUnpublishedPreparation();
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FEnvironmentRenderReadinessCommand>(this,World,Actor,Work.Geometry,Work.Dynamic));return true;
}
#endif
