#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelEnvironmentPageBarrier.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPageBarrierLifetimeTest, "Voxel.Objects.PageBarrierLifetime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVoxelPageBarrierLifetimeTest::RunTest(const FString&)
{
    using namespace VoxelEnvironmentPages;
    const VoxelEnvironmentPages::FKey A{0,{1,2,3}}, B{1,{4,5,6}};
    FBarrier Ledger;
    auto Cpu = Ledger.TryAcquire(A,EProducer::Cpu);
    auto SpecGpu = Ledger.TryAcquire(A,EProducer::Gpu);
    auto Other = Ledger.TryAcquire(B,EProducer::Cpu);
    TestTrue(TEXT("Both producers may hold the same key"),Cpu.IsValid() && SpecGpu.IsValid());
    // ChunkRecords may be removed here and the CPU result may be queued. Neither
    // event retires the token: simulate its copy from worker into the result queue.
    const FLease QueuedResult = Cpu;
    auto Ticket = Ledger.TryFreeze({A});
    TestTrue(TEXT("Freeze accepts an active key"),Ticket.IsValid());
    TestFalse(TEXT("Unloaded/queued CPU and speculative GPU still block"),Ledger.IsQuiescent(Ticket));
    TestFalse(TEXT("New ordinary work cannot enter frozen key"),Ledger.TryAcquire(A,EProducer::Cpu).IsValid());
    TestTrue(TEXT("Final GPU discard retires its lease"),Ledger.Retire(SpecGpu));
    TestFalse(TEXT("Worker completion is not result consumption"),Ledger.IsQuiescent(Ticket));
    TestTrue(TEXT("Final queued-result consumption retires CPU"),Ledger.Retire(QueuedResult));
    TestFalse(TEXT("Duplicate worker token cannot retire twice"),Ledger.Retire(Cpu));
    TestTrue(TEXT("Unrelated active work does not prevent key quiescence"),Ledger.IsQuiescent(Ticket));
    TestTrue(TEXT("Unrelated key still admits new work"),Ledger.TryAcquire(B,EProducer::Gpu).IsValid());
    TestTrue(TEXT("Release completed barrier"),Ledger.Release(Ticket));
    TestFalse(TEXT("Released ticket is not quiescent authority"),Ledger.IsQuiescent(Ticket));
    TestTrue(TEXT("Unrelated lease remains active"),Ledger.Retire(Other));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPageBarrierCancellationTest, "Voxel.Objects.PageBarrierCancellation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVoxelPageBarrierCancellationTest::RunTest(const FString&)
{
    using namespace VoxelEnvironmentPages;
    const VoxelEnvironmentPages::FKey A{0,{1,2,3}};
    FBarrier Ledger, Foreign;
    auto Old = Ledger.TryAcquire(A,EProducer::Cpu);
    auto Cancelled = Ledger.TryFreeze({A});
    TestTrue(TEXT("Cancel releases freeze"),Ledger.Release(Cancelled));
    TestEqual(TEXT("Cancel leaves pending work alive"),Ledger.ActiveLeases(),1);
    auto New = Ledger.TryAcquire(A,EProducer::Gpu);
    auto Retry = Ledger.TryFreeze({A});
    TestFalse(TEXT("Stale cancel cannot release retry"),Ledger.Release(Cancelled));
    TestTrue(TEXT("Retry remains frozen"),Ledger.IsFrozen(A));
    auto ForeignLease = Foreign.TryAcquire(A,EProducer::Cpu);
    auto ForeignTicket = Foreign.TryFreeze({A});
    TestFalse(TEXT("Same-serial foreign lease rejected"),Ledger.Retire(ForeignLease));
    TestFalse(TEXT("Same-serial foreign ticket rejected"),Ledger.Release(ForeignTicket));
    TestTrue(TEXT("Original work can finish after retry"),Ledger.Retire(Old));
    TestFalse(TEXT("Old token cannot release new lease"),Ledger.Retire(Old));
    TestFalse(TEXT("New work still blocks retry"),Ledger.IsQuiescent(Retry));
    TestTrue(TEXT("New work drains exactly once"),Ledger.Retire(New));
    TestTrue(TEXT("Retry becomes quiescent only after both generations drain"),Ledger.IsQuiescent(Retry));
    TestFalse(TEXT("Default ticket rejected"),Ledger.Release({}));
    TestFalse(TEXT("Default lease rejected"),Ledger.Retire({}));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPageBarrierBoundsTest, "Voxel.Objects.PageBarrierBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVoxelPageBarrierBoundsTest::RunTest(const FString&)
{
    using namespace VoxelEnvironmentPages;
    const VoxelEnvironmentPages::FKey A{0,{1,2,3}}, B{1,{4,5,6}}, C{2,{7,8,9}};
    FBarrier Ledger(2,2,2);
    TestFalse(TEXT("Empty footprint rejected"),Ledger.TryFreeze({}).IsValid());
    TestFalse(TEXT("Duplicate footprint rejected atomically"),Ledger.TryFreeze({A,A}).IsValid());
    TestFalse(TEXT("Invalid level rejects complete request"),Ledger.TryFreeze({A,VoxelEnvironmentPages::FKey{VoxelCoords::kNumLevels,{}}}).IsValid());
    TestFalse(TEXT("Over-capacity request rejected"),Ledger.TryFreeze({A,B,C}).IsValid());
    TestEqual(TEXT("Rejected batches freeze no prefix"),Ledger.FrozenKeys(),0);
    auto T = Ledger.TryFreeze({A});
    TestFalse(TEXT("Overlapping request rejected atomically"),Ledger.TryFreeze({B,A}).IsValid());
    TestFalse(TEXT("Failed overlap does not freeze unrelated prefix"),Ledger.IsFrozen(B));
    auto U = Ledger.TryFreeze({B});
    TestTrue(TEXT("Disjoint bounded tickets coexist"),T.IsValid() && U.IsValid());
    TestFalse(TEXT("Combined key capacity enforced"),Ledger.TryFreeze({C}).IsValid());
    Ledger.Release(T); Ledger.Release(U);
    auto L = Ledger.TryAcquire(A,EProducer::Cpu);
    auto M = Ledger.TryAcquire(B,EProducer::Gpu);
    TestFalse(TEXT("Lease capacity failure"),Ledger.TryAcquire(C,EProducer::Cpu).IsValid());
    TestEqual(TEXT("Lease failure preserves existing work"),Ledger.ActiveLeases(),2);
    Ledger.Retire(L);
    TestTrue(TEXT("Capacity reused after final retirement"),Ledger.TryAcquire(C,EProducer::Gpu).IsValid());
    TestTrue(TEXT("Unrelated lease survived failed admission"),Ledger.Retire(M));
    FBarrier TicketLimited(3,3,1);
    TicketLimited.TryFreeze({A});
    TestFalse(TEXT("Independent ticket bound enforced"),TicketLimited.TryFreeze({B}).IsValid());
    TestFalse(TEXT("Ticket capacity failure freezes nothing"),TicketLimited.IsFrozen(B));
    FBarrier Disabled(0,0,0);
    TestFalse(TEXT("Zero-capacity ledger admits no lease"),Disabled.TryAcquire(A,EProducer::Cpu).IsValid());
    TestFalse(TEXT("Zero-capacity ledger admits no ticket"),Disabled.TryFreeze({A}).IsValid());
    return true;
}
#endif
