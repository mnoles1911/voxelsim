#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelBrickPool.h"
#include "VoxelPreparedIndexTestSupport.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBrickEvictionPinsTest,
    "Voxel.Objects.BrickEvictionPins", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVoxelBrickEvictionPinsTest::RunTest(const FString&)
{
    FVoxelBrickPool Pool, Other;
    FVoxelBrickPoolConfig Config; Config.ChunkCapacity=3; Config.OccWordCapacity=1024; Config.MatWordCapacity=1024;
    Pool.Init(Config); Other.Init(Config);
    TArray<FVoxelBrickIndexEntry> Initial; VoxelPreparedIndexTestSupport::SetSink(Pool,[](const auto&){},Initial);
    auto Air=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>(); Air->Desc.SetNumZeroed(128);
    const FVoxelBrickChunkKey A{100,0,0,0}, B{50,0,0,0}, C{1,0,0,0}, D{2,0,0,0};
    auto Add=[&](const auto& Key){return Pool.AddChunkFromCpu(Air,Key,FVoxelBrickChunkShading{});};
    auto Pages=[&](const auto& Key){
        const auto Token=Pool.SnapshotAllocation(Key); FVoxelBrickPreparedReplacement Page;
        Page.Key=Key; Page.ExpectedSlot=Token.Slot; Page.ExpectedSequence=Token.AddSequence; Page.CpuPack=Air;
        TArray<FVoxelBrickPreparedReplacement> Result; Result.Add(Page); return Result;
    };
    TArray<FVoxelBrickChunkKey> Keys{A};
    const auto Ticket=Pool.AcquireEvictionPins(Keys);
    TestTrue(TEXT("Absent key can be pressure protected"),Ticket.IsValid());
    TestFalse(TEXT("Overlapping acquisition is refused"),Pool.AcquireEvictionPins(Keys).IsValid());
    TestFalse(TEXT("Cross-pool ticket cannot release"),Other.ReleaseEvictionPins(Ticket));
    TArray<FVoxelBrickChunkKey> Partial{B,A};
    TestFalse(TEXT("Late overlap rejects entire acquisition"),Pool.AcquireEvictionPins(Partial).IsValid());
    TArray<FVoxelBrickChunkKey> OnlyB{B}; const auto Probe=Pool.AcquireEvictionPins(OnlyB);
    TestTrue(TEXT("Rejected acquisition leaves earlier keys unclaimed"),Probe.IsValid());
    Pool.ReleaseEvictionPins(Probe);
    TArray<FVoxelBrickChunkKey> Huge; Huge.SetNum(8193);
    TestFalse(TEXT("Oversized acquisition rejected before processing keys"),Pool.AcquireEvictionPins(Huge).IsValid());
    Add(A); Add(B); Add(C); Pool.SetEvictionFocusVoxel0(0,0,0);
    AddExpectedError(TEXT("BRICK POOL EVICTED FOR THE FIRST TIME"),EAutomationExpectedErrorFlags::Contains,1);
    Add(D);
    TestTrue(TEXT("Farthest protected page survives unrelated pressure"),Pool.SnapshotAllocation(A).bPresent);
    TestFalse(TEXT("Next farthest unprotected page is evicted"),Pool.SnapshotAllocation(B).bPresent);
    TestTrue(TEXT("Incoming page allocated"),Pool.SnapshotAllocation(D).bPresent);
    const auto Old=Pool.SnapshotAllocation(A); Add(A);
    TestTrue(TEXT("Same-key draining replacement stays legal"),Pool.SnapshotAllocation(A).AddSequence!=Old.AddSequence);
    TestFalse(TEXT("Unticketed batch cannot replace protected page"),Pool.PublishPreparedBatch(Pages(A)));
    TestFalse(TEXT("Reservation failure does not evict pages"),Pool.PublishPreparedBatch(Pages(A),Ticket));
    TestTrue(TEXT("Failed batch preserves protected page"),Pool.SnapshotAllocation(A).bPresent);
    TestTrue(TEXT("Explicit draining removal remains legal"),Pool.RemoveChunk(A));
    Add(A);
    TArray<FVoxelBrickChunkKey> More{C,D}; const auto All=Pool.AcquireEvictionPins(More);
    TestTrue(TEXT("Remaining residents protected"),All.IsValid());
    AddExpectedError(TEXT("Brick pool REFUSED chunk"),EAutomationExpectedErrorFlags::Contains,1);
    TestEqual(TEXT("All-pinned pressure terminates with refusal"),Add(B),int32(INDEX_NONE));
    TestTrue(TEXT("Capacity refusal preserves resident"),Pool.SnapshotAllocation(C).bPresent);
    TestTrue(TEXT("Release makes skipped pages eligible again"),Pool.ReleaseEvictionPins(All));
    Add(B); TestTrue(TEXT("Allocation resumes after unpin"),Pool.SnapshotAllocation(B).bPresent);
    Pool.RemoveChunk(B); // room for reserve-before-replace
    TestTrue(TEXT("Matching ticket permits prepared replacement"),Pool.PublishPreparedBatch(Pages(A),Ticket));
    TestFalse(TEXT("Publication keeps pressure protection until explicit release"),Pool.PublishPreparedBatch(Pages(A)));
    TestTrue(TEXT("Release original ticket"),Pool.ReleaseEvictionPins(Ticket));
    TestFalse(TEXT("Released ticket is stale"),Pool.ReleaseEvictionPins(Ticket));
    TestFalse(TEXT("Stale ticket cannot publish"),Pool.PublishPreparedBatch(Pages(A),Ticket));
    TestTrue(TEXT("Unticketed unprotected publication remains compatible"),Pool.PublishPreparedBatch(Pages(A)));
    TArray<FVoxelBrickChunkKey> Duplicate{A,A};
    TestFalse(TEXT("Duplicate key request refused atomically"),Pool.AcquireEvictionPins(Duplicate).IsValid());
    const auto ResetTicket=Pool.AcquireEvictionPins(Keys);
    TestTrue(TEXT("Rejected duplicate left no pin"),ResetTicket.IsValid());
    Pool.Reset();
    TestFalse(TEXT("Reset invalidates outstanding tickets"),Pool.ReleaseEvictionPins(ResetTicket));
    const auto Fresh=Pool.AcquireEvictionPins(Keys);
    TestTrue(TEXT("Reset cleared old key ownership"),Fresh.IsValid());
    TestTrue(TEXT("New epoch ticket works"),Pool.ReleaseEvictionPins(Fresh));
    TArray<FVoxelBrickIndexEntry> Ignored; Pool.SetIndexSink(nullptr,Ignored);
    return true;
}
#endif
