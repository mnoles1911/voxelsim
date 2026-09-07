#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelObjectRegistry.h"
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelObjectRegistryTest,"Voxel.Objects.Registry",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelObjectRegistryTest::RunTest(const FString&)
{
    using namespace VoxelObjects;
    FRegistry Registry;FEntry Initial;Initial.Id=FGuid::NewGuid();Initial.Kind=1;
    Initial.Transform=FTransform(FVector(-1.,25600.,0.));Initial.Lifetime.Kind=EVoxelDebrisLifetime::Harvestable;
    Initial.Lifetime.RemainingSeconds=123.;
    TestTrue(TEXT("Import stable identity"),Registry.Import(Initial));
    TestFalse(TEXT("Reject duplicate revision"),Registry.Import(Initial));
    TestEqual(TEXT("Negative region uses floor"),FRegistry::RegionFor(Initial.Transform.GetLocation()),FIntVector(-1,1,0));
    TArray<uint8> Bytes;Bytes.Add(17);TestTrue(TEXT("Publish geometry"),Registry.PublishGeometry(Initial.Id,MoveTemp(Bytes)));
    const auto Frozen=Registry.Snapshot();const uint64 Before=Frozen[0].Revision;
    TArray<uint8> Replacement;Replacement.Add(29);Registry.PublishGeometry(Initial.Id,MoveTemp(Replacement));
    TestEqual(TEXT("Old snapshot bytes remain immutable"),int32((*Frozen[0].Geometry)[0]),17);
    TestEqual(TEXT("Geometry revision advances"),Registry.Find(Initial.Id)->GeometryRevision,uint64(2));
    TestTrue(TEXT("Record revision advances"),Registry.Find(Initial.Id)->Revision>Before);
    TestEqual(TEXT("Snapshot preserves identity"),Registry.Snapshot()[0].Id,Initial.Id);
    FCallbacks Callbacks;TArray<FView> Views;
    Registry.Tick(Views,10.,Callbacks);
    TestEqual(TEXT("Dormant timer advances"),Registry.Find(Initial.Id)->Lifetime.RemainingSeconds,113.);
    Views.Add(FView{Initial.Transform.GetLocation(),FVector::ForwardVector});Registry.Tick(Views,10.,Callbacks);
    TestEqual(TEXT("Nearby player protects dormant resource"),Registry.Find(Initial.Id)->Lifetime.RemainingSeconds,113.);
    Registry.Update(Initial.Id,[](FEntry& E){E.bRetained=true;});Views.Reset();Registry.Tick(Views,1000.,Callbacks);
    TestEqual(TEXT("Retained timer preserved"),Registry.Find(Initial.Id)->Lifetime.RemainingSeconds,113.);
    Registry.Update(Initial.Id,[](FEntry& E){E.bRetained=false;});Registry.Tick(Views,113.,Callbacks);
    TestTrue(TEXT("Expired entry tombstoned"),Registry.Find(Initial.Id)->Residency==EResidency::Tombstone);
    Initial.Revision=100000;TestFalse(TEXT("Tombstone blocks stale resurrection even with newer revision"),Registry.Import(Initial));
    TestEqual(TEXT("Normal snapshots omit tombstones"),Registry.Snapshot(false).Num(),0);
    TestEqual(TEXT("Replication snapshots retain tombstones"),Registry.Snapshot().Num(),1);
    TestFalse(TEXT("Tombstone releases registry geometry"),Registry.Find(Initial.Id)->Geometry.IsValid());
    TestEqual(TEXT("Tombstone releases dynamic capacity"),Registry.Find(Initial.Id)->Dynamic.GetAllocatedSize(),SIZE_T(0));

    FEntry Near;Near.Id=FGuid::NewGuid();Near.Kind=2;Near.Transform=FTransform(FVector(16000.,0.,0.));
    Views.Add(FView{});
    TestTrue(TEXT("Load inclusive boundary"),FRegistry::ShouldLoad(Near,Views,16000.));
    TestFalse(TEXT("Hysteresis retains resident at 160m"),FRegistry::ShouldEvict(Near,Views,20000.));
    Near.Transform.SetLocation(FVector(20001.,0.,0.));
    TestTrue(TEXT("Evict beyond boundary"),FRegistry::ShouldEvict(Near,Views,20000.));
    Near.bActivePhysics=true;TestFalse(TEXT("Falling body cannot evict"),FRegistry::ShouldEvict(Near,Views,20000.));
    Near.bActivePhysics=false;Near.bPinned=true;TestFalse(TEXT("Ownership dependency pins source"),FRegistry::ShouldEvict(Near,Views,20000.));

    FRegistry Budget;for(int32 I=0;I<5;++I){FEntry E;E.Id=FGuid::NewGuid();E.Kind=1;E.Geometry=MakeShared<const TArray<uint8>,ESPMode::ThreadSafe>();Budget.Import(E);}
    int32 Attempts=0;Callbacks.Restore=[&](const FEntry& E)->AActor*{++Attempts;Budget.Remove(E.Id);return nullptr;};
    FSettings Settings;Settings.MaxTransitions=2;Settings.MaxInspections=3;
    const auto Result=Budget.Tick(Views,1.,Callbacks,Settings);
    TestEqual(TEXT("Inspection cursor stops after attempt budget"),Result.Inspected,2);TestEqual(TEXT("Bounded restore attempts"),Attempts,2);
    TestEqual(TEXT("Callback removal survives stale restore"),Budget.Snapshot(false).Num(),3);
    FRegistry Fair;for(int32 I=0;I<5;++I){FEntry Candidate;Candidate.Id=FGuid::NewGuid();Candidate.Kind=1;Candidate.Geometry=MakeShared<const TArray<uint8>,ESPMode::ThreadSafe>();Fair.Import(Candidate);}
    TSet<FGuid> Attempted;FCallbacks Failures;Failures.BeginRestore=[&](const FEntry& Candidate){Attempted.Add(Candidate.Id);return false;};
    // Default 128 inspections exceeds the five entries; failed starts must not
    // reset the cursor and repeatedly monopolize both transition attempts.
    for(int32 I=0;I<3;++I)Fair.Tick(Views,.1,Failures);
    TestEqual(TEXT("Every entry gets an attempt despite persistent failures"),Attempted.Num(),5);
    FRegistry Staged;FEntry Pending;Pending.Id=FGuid::NewGuid();Pending.Kind=2;Pending.Geometry=MakeShared<const TArray<uint8>,ESPMode::ThreadSafe>();
    Pending.Lifetime.Kind=EVoxelDebrisLifetime::Harvestable;Pending.Lifetime.RemainingSeconds=5.;Staged.Import(Pending);
    FCallbacks AsyncCallbacks;int32 Began=0;AsyncCallbacks.BeginRestore=[&](const FEntry&){++Began;return true;};
    Staged.Tick(Views,0.,AsyncCallbacks);Staged.Tick(Views,1.,AsyncCallbacks);
    TestEqual(TEXT("Async restore starts exactly once"),Began,1);
    TestTrue(TEXT("Async restore stays pending"),Staged.Find(Pending.Id)->Residency==EResidency::Restoring);
    Views.Reset();Staged.Tick(Views,5.,AsyncCallbacks);
    TestTrue(TEXT("Pending restore expires without resurrection"),Staged.Find(Pending.Id)->Residency==EResidency::Tombstone);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelObjectIdentityTest,"Voxel.Objects.Identity",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelObjectIdentityTest::RunTest(const FString&)
{
    using namespace VoxelObjects;
    UWorld* W=UWorld::CreateWorld(EWorldType::Game,false,FName(*FString::Printf(TEXT("RegistryIdentity_%s"),*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
    if(!W){AddError(TEXT("create identity world"));return false;}
    AActor* A=W->SpawnActor<AActor>();AActor* B=W->SpawnActor<AActor>();
    if(!A||!B){AddError(TEXT("spawn identity actors"));W->DestroyWorld(false);return false;}
    FRegistry Registry;const FGuid First=Registry.Bind(A,1),Second=Registry.Bind(B,2);
    TestTrue(TEXT("Distinct actor IDs"),First.IsValid()&&Second.IsValid()&&First!=Second);
    TestEqual(TEXT("Repeated registration is exactly once"),Registry.Bind(A,1),First);
    TestEqual(TEXT("Repeated registration does not add record"),Registry.Num(),2);
    TestFalse(TEXT("Reject rebinding actor under second ID"),Registry.Bind(A,1,FGuid::NewGuid()).IsValid());
    TestFalse(TEXT("Reject mutation of actor kind"),Registry.Bind(A,2).IsValid());
    auto Duplicate=*Registry.Find(First);Duplicate.Id=FGuid::NewGuid();
    TestFalse(TEXT("Import cannot alias an existing actor"),Registry.Import(Duplicate));
    Registry.Find(First)->Dynamic.SetNumUninitialized(4096);
    Registry.Find(First)->Residency=EResidency::Restoring;
    TestFalse(TEXT("Staged completion rejects stale geometry generation"),Registry.CompleteRestore(First,99,A));
    TestTrue(TEXT("Staged completion accepts current geometry generation"),Registry.CompleteRestore(First,0,A));
    A->Destroy();B->Destroy();
    auto FoundA=Registry.Find(A);auto FoundB=Registry.Find(B);
    TestTrue(TEXT("Pending-kill actor lookup keeps exact identity"),FoundA&&FoundA->Id==First&&FoundB&&FoundB->Id==Second);
    Registry.Remove(First);
    TestEqual(TEXT("Removing actor frees dynamic allocation"),Registry.Find(First)->Dynamic.GetAllocatedSize(),SIZE_T(0));
    TestTrue(TEXT("Other destroyed actor identity stays distinct"),Registry.Find(B)&&Registry.Find(B)->Id==Second);
    W->DestroyWorld(false);return true;
}
#endif
