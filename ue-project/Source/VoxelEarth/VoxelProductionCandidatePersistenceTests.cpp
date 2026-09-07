#include "VoxelProductionCandidatePreparation.h"
#include "VoxelEnvironmentLODPrototype.h"
#include "VoxelDetachedPersistence.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace
{
struct FCandidatePersistenceFlags { bool CandidateDone=false,CandidateValid=false,RestoreDone=false,RestoreValid=false; };
class FProductionCandidatePersistenceProbe final : public IAutomationLatentCommand
{
    FAutomationTestBase* Test;
    UWorld* World=nullptr;
    AVoxelEnvironmentLODPrototype* Candidate=nullptr;
    AVoxelEnvironmentLODPrototype* Restoring=nullptr;
    FVoxelImmutableGeometry Geometry;
    FGuid SourceId,RestoringId;
    TSharedPtr<FCandidatePersistenceFlags> Flags=MakeShared<FCandidatePersistenceFlags>();
    double Started=0;
    void Cleanup(){if(World){VoxelObjects::Forget(World);World->DestroyWorld(false);World=nullptr;}}
    bool CheckSnapshot(const TCHAR* Stage)
    {
        VoxelDetachedPersistence::RefreshObjects(World);
        if(!Test->TestEqual(Stage,VoxelObjects::Get(World).Num(),2))return false;
        VoxelDetachedPersistence::FSnapshot Snapshot;
        if(!Test->TestTrue(TEXT("preparation does not block whole-world snapshot"),VoxelDetachedPersistence::CaptureSnapshot(World,Snapshot)))return false;
        if(!Test->TestEqual(TEXT("only existing authoritative objects saved"),Snapshot.Num(),2))return false;
        const auto* Source=Snapshot.FindByPredicate([&](const auto& E){return E.Id==SourceId;});
        const auto* Pending=Snapshot.FindByPredicate([&](const auto& E){return E.Id==RestoringId;});
        if(!Test->TestTrue(TEXT("ordinary hidden live object remains saved"),Source!=nullptr))return false;
        if(!Test->TestTrue(TEXT("normal restoring record retains existing geometry and revision"),Pending&&Pending->Geometry==Geometry&&Pending->Revision==42&&Pending->Residency==VoxelObjects::EResidency::Restoring))return false;
        return Test->TestTrue(TEXT("unpublished candidate has no registry binding"),VoxelObjects::Get(World).Find(Candidate)==nullptr);
    }
public:
    explicit FProductionCandidatePersistenceProbe(FAutomationTestBase* In):Test(In){}
    virtual bool Update() override
    {
        if(!World)
        {
            Started=FPlatformTime::Seconds();
            World=UWorld::CreateWorld(EWorldType::Game,false,FName(*FString::Printf(TEXT("CandidatePersistence_%s"),*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
            if(!World){Test->AddError(TEXT("create isolated candidate persistence world"));return true;}
            std::vector<uint8> Vxa;
            const auto Word=[&](uint32 V){for(int I=0;I<4;++I)Vxa.push_back(uint8(V>>(I*8)));};
            for(uint32 V:{vxc::kVxaMagic,3u,0u,0u,0u,2u,2u,2u,100u,1u,0u,0u})Word(V);
            Vxa.push_back(16);Word(8);
            VoxelProductionCandidate::FWork Input;
            Input.Descriptor.SpecId=TEXT("candidate-persistence-tree");Input.Descriptor.Kind=TEXT("tree");Input.Descriptor.Category=TEXT("environment");
            if(!VoxelProductionCandidate::BuildImmutableSnapshot(Input,Vxa)){Test->AddError(TEXT("build test geometry"));Cleanup();return true;}
            Geometry=Input.Geometry;
            auto Source=World->SpawnActor<AVoxelEnvironmentLODPrototype>();
            if(!Source||!Source->RestoreObjectState(Geometry,Input.Dynamic)){Test->AddError(TEXT("prepare authoritative source actor"));Cleanup();return true;}
            Source->SetActorHiddenInGame(true); // visibility alone must never exclude persistence
            VoxelDetachedPersistence::RefreshObjects(World);
            auto SourceEntry=VoxelObjects::Get(World).Find(Source);
            if(!SourceEntry||!SourceEntry->Geometry){Test->AddError(TEXT("source registry capture"));Cleanup();return true;}
            SourceId=SourceEntry->Id;
            Restoring=World->SpawnActor<AVoxelEnvironmentLODPrototype>();
            if(!Restoring){Test->AddError(TEXT("spawn normal staged restore"));Cleanup();return true;}
            Restoring->Tags.AddUnique(TEXT("ObjectRestorePending"));
            auto Existing=*SourceEntry;Existing.Id=FGuid::NewGuid();Existing.Revision=42;Existing.Actor=Restoring;Existing.Residency=VoxelObjects::EResidency::Restoring;
            Existing.Geometry=Geometry;RestoringId=Existing.Id;
            if(!VoxelObjects::Get(World).Import(Existing)){Test->AddError(TEXT("install existing restoring data record"));Cleanup();return true;}
            // Import normalizes actor-backed records to Live. The streaming
            // scheduler enters Restoring after admission; mirror that state
            // transition without changing the imported geometry/revision.
            VoxelObjects::Get(World).Find(RestoringId)->Residency=VoxelObjects::EResidency::Restoring;
            const auto Shared=Flags;
            Restoring->BeginStagedObjectRestore(Geometry,Input.Dynamic,[Shared](bool Valid){Shared->RestoreDone=true;Shared->RestoreValid=Valid;});
            Test->TestFalse(TEXT("ordinary restore is not a new production preparation"),Restoring->IsUnpublishedPreparation());
            Candidate=World->SpawnActor<AVoxelEnvironmentLODPrototype>();
            if(!Candidate){Test->AddError(TEXT("spawn unpublished candidate"));Cleanup();return true;}
            Candidate->MarkUnpublishedPreparation();
            if(!CheckSnapshot(TEXT("spawning empty hidden candidate does not bind it"))){Cleanup();return true;}
            Candidate->BeginStagedObjectRestore(Geometry,Input.Dynamic,[Shared](bool Valid){Shared->CandidateDone=true;Shared->CandidateValid=Valid;});
            if(!CheckSnapshot(TEXT("starting candidate worker does not bind it"))){Cleanup();return true;}
            return false;
        }
        if(FPlatformTime::Seconds()-Started>60){Test->AddError(TEXT("candidate persistence staging timed out"));Cleanup();return true;}
        Candidate->AdvanceStagedObjectRestore();Restoring->AdvanceStagedObjectRestore();
        if(!CheckSnapshot(TEXT("advancing components does not bind candidate"))){Cleanup();return true;}
        if(!Flags->CandidateDone||!Flags->RestoreDone)return false;
        Test->TestTrue(TEXT("candidate reaches hidden ready state"),Flags->CandidateValid);
        Test->TestTrue(TEXT("normal restoring actor still prepares successfully"),Flags->RestoreValid);
        Test->TestTrue(TEXT("ready candidate retains explicit unpublished status"),Candidate->IsUnpublishedPreparation());
        Test->TestTrue(TEXT("ready candidate stays hidden"),Candidate->IsHidden());
        Test->TestFalse(TEXT("ready candidate remains non-colliding"),Candidate->GetActorEnableCollision());
        Candidate->CancelStagedObjectRestore();
        Test->TestTrue(TEXT("cancellation does not make empty candidate discoverable"),Candidate->IsUnpublishedPreparation());
        if(!CheckSnapshot(TEXT("cancelled actor cannot poison snapshot"))){Cleanup();return true;}
        VoxelDetachedPersistence::OnEndPlay(Candidate,EEndPlayReason::LevelTransition);
        Test->TestFalse(TEXT("unpublished teardown does not poison legacy save fallback"),VoxelDetachedPersistence::HasLoadFailure(World));
        if(!CheckSnapshot(TEXT("legacy end-play exclusion preserves snapshots"))){Cleanup();return true;}
        Candidate->Destroy();
        if(!CheckSnapshot(TEXT("destroying candidate leaves prior records intact"))){Cleanup();return true;}
        Cleanup();return true;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelProductionCandidatePersistenceTest,
    "Voxel.Objects.ProductionCandidatePersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FVoxelProductionCandidatePersistenceTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FProductionCandidatePersistenceProbe(this));return true;
}
#endif
