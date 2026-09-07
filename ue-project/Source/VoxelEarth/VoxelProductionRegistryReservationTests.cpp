#include "VoxelObjectRegistry.h"
#include "VoxelProductionCandidatePreparation.h"
#include "VoxelProductionEnvironmentAdapter.h"
#include "VoxelEnvironmentLODPrototype.h"
#include "VoxelDetachedPersistence.h"
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include <type_traits>
#if WITH_DEV_AUTOMATION_TESTS
namespace {
struct FReservationFinish : IAutomationLatentCommand {
    FAutomationTestBase* Test;UWorld* World;TWeakObjectPtr<AVoxelEnvironmentLODPrototype> Actor;
    VoxelObjects::FProductionReservation Ticket;TSharedRef<bool> Done=MakeShared<bool>(false),Ready=MakeShared<bool>(false);
    double Started=FPlatformTime::Seconds();
    FReservationFinish(FAutomationTestBase* InTest,UWorld* W,AVoxelEnvironmentLODPrototype* A,VoxelObjects::FProductionReservation T):Test(InTest),World(W),Actor(A),Ticket(T){}
    bool Update() override {
        auto A=Actor.Get();if(A&&!*Done){A->AdvanceStagedObjectRestore();if(!*Done&&FPlatformTime::Seconds()-Started<60.)return false;}
        auto& Registry=VoxelObjects::Get(World);
        if(Test->TestTrue(TEXT("hidden staged actor completes before timeout"),A&&*Done&&*Ready)){
            Test->TestTrue(TEXT("explicit staged actor publication succeeds"),A->PublishStagedObjectRestore());
            Test->TestFalse(TEXT("reservation blocks random ID after preparation flag clears"),Registry.Bind(A,3).IsValid());
            Test->TestTrue(TEXT("capturable actor commits exact reserved identity"),Registry.CommitProduction(Ticket));
            Test->TestTrue(TEXT("live actor has reserved stable ID"),Registry.Find(A)&&Registry.Find(A)->Id==Ticket.Id);
            TArray<VoxelObjects::FEntry> Saved;Test->TestTrue(TEXT("committed entry is saveable"),VoxelDetachedPersistence::CaptureSnapshot(World,Saved));
        }
        Registry.RollbackProduction(Ticket);if(A){A->CancelStagedObjectRestore();A->Destroy();}
        VoxelObjects::Forget(World);World->DestroyWorld(false);return true;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelProductionRegistryReservationTest,"Voxel.Objects.ProductionRegistryReservation",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelProductionRegistryReservationTest::RunTest(const FString&)
{
    using namespace VoxelObjects;
    static_assert(!std::is_copy_constructible_v<FRegistry> && !std::is_move_constructible_v<FRegistry>);
    static_assert(!std::is_copy_assignable_v<FRegistry> && !std::is_move_assignable_v<FRegistry>);
    std::vector<uint8> Vxa;auto Word=[&](uint32 V){for(int I=0;I<4;++I)Vxa.push_back(uint8(V>>(8*I)));};
    for(uint32 V:{vxc::kVxaMagic,3u,0u,0u,0u,1u,1u,1u,100u,1u,0u,0u})Word(V);Vxa.push_back(16);Word(1);
    VoxelProductionCandidate::FWork Work;Work.Provenance={19,123,456,-13,4,10,5,1,2,3};
    Work.Descriptor.SpecId=TEXT("reserved-canonical-rock");Work.Descriptor.Kind=TEXT("rock");Work.Descriptor.Category=TEXT("environment");Work.Descriptor.SeedIndex=1;
    Work.CanonicalSourceHash=TEXT("22222222222222222222222222222222");
    if(!TestTrue(TEXT("prepared fixture snapshot"),VoxelProductionCandidate::BuildImmutableSnapshot(Work,Vxa)))return false;
    FEntry Prepared;Prepared.Kind=3;Prepared.GeometryRevision=1;Prepared.Geometry=Work.Geometry;Prepared.Dynamic=Work.Dynamic;Prepared.Transform=FTransform(FVector(-130.,40.,100.));Prepared.bRetained=true;Prepared.Lifetime.Kind=EVoxelDebrisLifetime::Retained;
    const auto Stable=VoxelProductionEnvironment::StableId(Work.Provenance);
    FRegistry Registry,Other;
    auto Invalid=Prepared;Invalid.GeometryRevision=0;
    TestFalse(TEXT("zero geometry revision refused"),Registry.ReserveProduction(Work.Descriptor,Invalid).IsValid());
    for(int32 N=0;N<Prepared.Geometry->Num();++N){
        Invalid=Prepared;auto Cut=MakeShared<TArray<uint8>,ESPMode::ThreadSafe>();Cut->Append(Prepared.Geometry->GetData(),N);Invalid.Geometry=Cut;
        if(Registry.ReserveProduction(Work.Descriptor,Invalid).IsValid()){AddError(FString::Printf(TEXT("Truncated geometry admitted at %d"),N));return false;}
    }
    for(int32 N=0;N<Prepared.Dynamic.Num();++N){
        Invalid=Prepared;Invalid.Dynamic.SetNum(N);
        if(Registry.ReserveProduction(Work.Descriptor,Invalid).IsValid()){AddError(FString::Printf(TEXT("Truncated dynamic state admitted at %d"),N));return false;}
    }
    Invalid=Prepared;Invalid.Dynamic.Add(0);TestFalse(TEXT("extra dynamic tail refused"),Registry.ReserveProduction(Work.Descriptor,Invalid).IsValid());
    Invalid=Prepared;auto Extra=MakeShared<TArray<uint8>,ESPMode::ThreadSafe>(*Prepared.Geometry);Extra->Add(0);Invalid.Geometry=Extra;
    TestFalse(TEXT("extra geometry tail refused"),Registry.ReserveProduction(Work.Descriptor,Invalid).IsValid());
    auto Ticket=Registry.ReserveProduction(Work.Descriptor,Prepared);
    if(!TestTrue(TEXT("valid canonical identity reserved"),Ticket.IsValid()))return false;
    TestEqual(TEXT("reservation carries provenance stable ID"),Ticket.Id,Stable);
    TestEqual(TEXT("reserved identity is not registered"),Registry.Num(),0);TestTrue(TEXT("Find excludes reservations"),Registry.Find(Stable)==nullptr);
    TestTrue(TEXT("save and replication snapshot excludes reservation"),Registry.Snapshot().IsEmpty());
    int32 Cursor=0;TestTrue(TEXT("sliced snapshot excludes reservation"),Registry.SnapshotSlice(Cursor).IsEmpty());
    TestTrue(TEXT("regional snapshot excludes reservation"),Registry.SnapshotRegions().IsEmpty());
    TestEqual(TEXT("streaming never inspects reservations"),Registry.Tick({},1.,{}).Inspected,0);
    TestFalse(TEXT("duplicate reservation refused"),Registry.ReserveProduction(Work.Descriptor,Prepared).IsValid());
    auto Aliased=Prepared;Aliased.Id=Stable;TestFalse(TEXT("ordinary import cannot steal reserved ID"),Registry.Import(Aliased));
    const auto OtherTicket=Other.ReserveProduction(Work.Descriptor,Prepared);
    TestFalse(TEXT("cross registry commit refused"),Other.CommitProduction(Ticket));TestFalse(TEXT("cross registry rollback refused"),Other.RollbackProduction(Ticket));
    TestTrue(TEXT("other reservation remains intact"),Other.RollbackProduction(OtherTicket));
    TestTrue(TEXT("rollback succeeds"),Registry.RollbackProduction(Ticket));TestEqual(TEXT("rollback creates no tombstone"),Registry.Num(),0);
    const auto Replacement=Registry.ReserveProduction(Work.Descriptor,Prepared);
    TestFalse(TEXT("stale rollback cannot release replacement"),Registry.RollbackProduction(Ticket));
    TestFalse(TEXT("stale commit cannot publish replacement"),Registry.CommitProduction(Ticket));
    TestTrue(TEXT("explicit data-only commit succeeds"),Registry.CommitProduction(Replacement));
    TestEqual(TEXT("commit publishes exactly one stable entry"),Registry.Num(),1);
    TestTrue(TEXT("committed entry is dormant and preserves geometry"),Registry.Find(Stable)&&Registry.Find(Stable)->Residency==EResidency::Dormant&&Registry.Find(Stable)->Geometry==Prepared.Geometry);
    TestFalse(TEXT("existing stable record is not overwritten"),Registry.ReserveProduction(Work.Descriptor,Prepared).IsValid());
    Registry.Remove(Stable);TestFalse(TEXT("tombstone is not resurrected"),Registry.ReserveProduction(Work.Descriptor,Prepared).IsValid());
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,FName(*FString::Printf(TEXT("ProductionReservation_%s"),*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
    if(!World)return false;
    auto Actor=World->SpawnActor<AVoxelEnvironmentLODPrototype>();
    if(!Actor){World->DestroyWorld(false);return false;}
    Actor->MarkUnpublishedPreparation();Prepared.Actor=Actor;
    auto& Live=Get(World);const auto LiveTicket=Live.ReserveProduction(Work.Descriptor,Prepared);
    TestTrue(TEXT("hidden candidate identity reserved"),LiveTicket.IsValid());
    TestFalse(TEXT("random-ID Bind cannot discover reserved candidate"),Live.Bind(Actor,3).IsValid());
    TestFalse(TEXT("explicit Bind cannot bypass reservation"),Live.Bind(Actor,3,Stable).IsValid());
    auto WrongId=Prepared;WrongId.Id=FGuid::NewGuid();TestFalse(TEXT("import cannot alias reserved actor"),Live.Import(WrongId));
    FEntry Existing;Existing.Id=FGuid::NewGuid();Existing.Kind=1;Existing.bRetained=true;Live.Import(Existing);
    const auto ExistingRevision=Live.Find(Existing.Id)->Revision;
    TestFalse(TEXT("Update cannot publish reserved actor under another ID"),Live.Update(Existing.Id,[&](FEntry& E){E.Actor=Actor;}));
    TestEqual(TEXT("rejected update leaves revision unchanged"),Live.Find(Existing.Id)->Revision,ExistingRevision);
    Live.Remove(Existing.Id);
    TestFalse(TEXT("unpublished candidate cannot commit"),Live.CommitProduction(LiveTicket));
    TestEqual(TEXT("failed commit preserves reservation"),Live.NumProductionReservations(),1);
    TArray<FEntry> Saved;TestTrue(TEXT("reservation cannot poison whole-world capture"),VoxelDetachedPersistence::CaptureSnapshot(World,Saved));TestTrue(TEXT("hidden reservation absent from save"),!Saved.ContainsByPredicate([&](const FEntry& E){return E.Id==Stable;}));
    TestTrue(TEXT("hidden rollback succeeds"),Live.RollbackProduction(LiveTicket));
    const auto DestroyedTicket=Live.ReserveProduction(Work.Descriptor,Prepared);Actor->Destroy();
    TestFalse(TEXT("destroyed candidate cannot become data-only commit"),Live.CommitProduction(DestroyedTicket));
    TestTrue(TEXT("destroyed candidate reservation can roll back"),Live.RollbackProduction(DestroyedTicket));
    TestTrue(TEXT("cancellation leaves no candidate entry"),Live.Find(Stable)==nullptr);
    auto Published=World->SpawnActor<AVoxelEnvironmentLODPrototype>();
    if(Published){
        Published->MarkUnpublishedPreparation();Prepared.Actor=Published;
        const auto PublishedTicket=Live.ReserveProduction(Work.Descriptor,Prepared);
        TestTrue(TEXT("published-path fixture reserved"),PublishedTicket.IsValid());
        auto Finish=MakeShared<FReservationFinish>(this,World,Published,PublishedTicket);
        const auto Done=Finish->Done,Ready=Finish->Ready;
        Published->BeginStagedObjectRestore(Work.Geometry,Work.Dynamic,[Done,Ready](bool Success){*Ready=Success;*Done=true;});
        FAutomationTestFramework::Get().EnqueueLatentCommand(Finish);return true;
    }
    AddError(TEXT("spawn published-path fixture"));
    Forget(World);World->DestroyWorld(false);return true;
}
#endif
