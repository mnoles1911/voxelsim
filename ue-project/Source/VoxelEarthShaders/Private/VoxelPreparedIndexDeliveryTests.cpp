#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelBrickPool.h"
#include "VoxelMarchChunkIndex.h"
#include "Misc/AutomationTest.h"
#include "HAL/PlatformProcess.h"
#include "RenderingThread.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPreparedIndexDeliveryTest,
    "Voxel.Objects.PreparedIndexDelivery",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelPreparedIndexDeliveryTest::RunTest(const FString&)
{
    FVoxelBrickPool Pool;FVoxelBrickPoolConfig Config;
    Config.ChunkCapacity=6;Config.OccWordCapacity=1024;Config.MatWordCapacity=1024;Pool.Init(Config);
    FVoxelMarchChunkIndex Index;TArray<FVoxelBrickIndexEntry> Empty;
    Index.DebugSeedOrderedForTest(Empty);
    FVoxelBrickIndexSink Sink=[&](const auto& D){Index.DebugApplyOrderedForTest(D);};
    TArray<FVoxelBrickIndexEntry> Snapshot;
    FVoxelBrickIndexDelta Prospective;
    FVoxelBrickPreparedIndexDeliveryRef RetainedDelivery;
    auto Install=[&](){Pool.SetIndexSink(Sink,Snapshot,[&](const auto& D){Prospective=D;RetainedDelivery=Index.DebugPrepareIndexForTest(D);return RetainedDelivery;});};
    auto Air=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>();Air->Desc.SetNumZeroed(128);
    const FVoxelBrickChunkKey A{0,0,0,0},B{1,0,0,0},C{2,0,0,0};
    Pool.SetIndexSink(Sink,Snapshot);
    Pool.AddChunkFromCpu(Air,A,{});Pool.AddChunkFromCpu(Air,C,{});Pool.Flush();
    const auto Old=Pool.SnapshotAllocation(A);
    auto Pages=[&](){FVoxelBrickPreparedReplacement P;P.Key=A;P.CpuPack=Air;
        const auto T=Pool.SnapshotAllocation(A);P.ExpectedSlot=T.Slot;P.ExpectedSequence=T.AddSequence;
        TArray<FVoxelBrickPreparedReplacement> R;R.Add(P);return R;};
    TestFalse(TEXT("Strict prepared publication refuses legacy-only sink"),Pool.PublishPreparedBatch(Pages()));
    Pool.AddChunkFromCpu(Air,B,{});Pool.RemoveChunk(C);
    const int32 Pending=Pool.DebugGetPendingWriteCount();
    Install();Index.DebugSetPilotBudgetForTest(0);
    TestFalse(TEXT("Budget refusal happens before resident mutation"),Pool.PublishPreparedBatch(Pages()));
    TestEqual(TEXT("Refusal preserves exact resident sequence"),Pool.SnapshotAllocation(A).AddSequence,Old.AddSequence);
    TestEqual(TEXT("Refusal preserves exact resident slot"),Pool.SnapshotAllocation(A).Slot,Old.Slot);
    TestEqual(TEXT("Refusal preserves pending ordinary write"),Pool.DebugGetPendingWriteCount(),Pending);
    TestEqual(TEXT("Refusal retains no credits"),Index.GetReservedPilotIndexBytes(),uint64(0));
    struct FRead{uint64 Gen=0;uint32 Value=0,Block=0;};
    auto Unchanged=MakeShared<FRead,ESPMode::ThreadSafe>();
    ENQUEUE_RENDER_COMMAND(PreparedIndexRefusalView)([&Index,Unchanged](FRHICommandListImmediate&){Index.DebugReadOrderedForTest(0,Unchanged->Gen,Unchanged->Value,Unchanged->Block);});
    FlushRenderingCommands();
    TestEqual(TEXT("Refusal did not enqueue a future index generation"),Unchanged->Gen,uint64(2));
    TestEqual(TEXT("Refusal retains old render-index slot"),Unchanged->Value & FVoxelMarchChunkIndex::kSlotMask,uint32(Old.Slot));
    Index.DebugSetPilotBudgetForTest(128ull*1024*1024);
    Pool.SetIndexSink(Sink,Snapshot,[&](const auto& D){auto Lease=Index.DebugPrepareIndexForTest(D);Pool.SetIndexSink(Sink,Snapshot);return Lease;});
    TestFalse(TEXT("Sink replacement invalidates preflight"),Pool.PublishPreparedBatch(Pages()));
    TestEqual(TEXT("Wrong sink leaves resident unchanged"),Pool.SnapshotAllocation(A).AddSequence,Old.AddSequence);
    TestEqual(TEXT("Wrong sink releases reserved credit"),Index.GetReservedPilotIndexBytes(),uint64(0));
    Install();
    auto PoolToken=Pool.PreparePreparedBatch(Pages());
    TestTrue(TEXT("Pool token reserves real index delivery"),PoolToken.IsValid() && Pool.ValidatePreparedBatch(PoolToken));
    TestTrue(TEXT("Pool reservation holds index credits"),Index.GetReservedPilotIndexBytes()>0);
    RetainedDelivery.Reset(); // fixture must not separately retain the index lease
    Pool.CancelPreparedBatch(PoolToken);
    TestEqual(TEXT("Pool cancellation returns index credits"),Index.GetReservedPilotIndexBytes(),uint64(0));
    if(PoolToken) TestEqual(TEXT("Retained cancelled pool token releases snapshots"),PoolToken->GetRetainedBytes(),uint64(0));
    auto Cancel=Index.DebugPrepareIndexForTest(Prospective);
    TestTrue(TEXT("Explicit reservation is valid"),Cancel.IsValid() && Cancel->ValidateForCommit());
    const uint64 Reserved=Index.GetReservedPilotIndexBytes();
    TestTrue(TEXT("Reservation charges allocated packet storage"),Reserved>0);
    Index.DebugSetPilotBudgetForTest(Reserved);
    TestFalse(TEXT("Outstanding reservation consumes pilot budget"),Index.DebugPrepareIndexForTest(Prospective).IsValid());
    Cancel.Reset();TestEqual(TEXT("Cancellation returns storage credit"),Index.GetReservedPilotIndexBytes(),uint64(0));
    Index.DebugSetPilotBudgetForTest(128ull*1024*1024);
    auto Stale=Index.DebugPrepareIndexForTest(Prospective);
    Index.DebugResetOrderedForTest();
    TestFalse(TEXT("Changed index epoch rejects reserved delivery"),Stale->ValidateForCommit());
    Stale.Reset();TestEqual(TEXT("Stale cancellation returns credit"),Index.GetReservedPilotIndexBytes(),uint64(0));
    // Snapshot includes current pool residents; seed before retrying pending deltas.
    Install();Index.DebugSeedOrderedForTest(Snapshot);
    Index.NoteChunkAdmitted(FIntVector(3,0,0),0); // pending annotation included in reserved capacity
    FEvent* Published=FPlatformProcess::GetSynchEventFromPool(true);
    ENQUEUE_RENDER_COMMAND(PreparedIndexHoldPublication)([Published](FRHICommandListImmediate&){Published->Wait(10000);});
    const bool Success=Pool.PublishPreparedBatch(Pages());
    TestTrue(TEXT("Reserved publication succeeds"),Success);
    TestTrue(TEXT("Enqueued packet retains credits until execution"),Index.GetReservedPilotIndexBytes()>0);
    TestEqual(TEXT("Prospective includes pending B and replacement A"),Prospective.Added.Num(),2);
    TestEqual(TEXT("Prospective includes prior C removal and A retirement"),Prospective.Removed.Num(),2);
    auto After=MakeShared<FRead,ESPMode::ThreadSafe>();
    ENQUEUE_RENDER_COMMAND(PreparedIndexCommittedView)([&Index,After](FRHICommandListImmediate&){Index.DebugReadOrderedForTest(0,After->Gen,After->Value,After->Block);});
    Published->Trigger();FlushRenderingCommands();FPlatformProcess::ReturnSynchEventToPool(Published);
    TestEqual(TEXT("Executed packet returns pilot credits"),Index.GetReservedPilotIndexBytes(),uint64(0));
    TestTrue(TEXT("Caller retained actual committed reservation"),RetainedDelivery.IsValid());
    TestEqual(TEXT("Retained queued delivery releases expected delta and all local arrays"),RetainedDelivery->DebugRetainedArrayBytes(),uint64(0));
    TestEqual(TEXT("Committed view names actual replacement slot"),After->Value & FVoxelMarchChunkIndex::kSlotMask,uint32(Pool.SnapshotAllocation(A).Slot));
    TestTrue(TEXT("Successful replacement changes identity"),Pool.SnapshotAllocation(A).AddSequence!=Old.AddSequence);
    Pool.SetIndexSink(nullptr,Snapshot);
    auto Disposable=MakeUnique<FVoxelMarchChunkIndex>();Disposable->DebugSeedOrderedForTest(Empty);
    auto CreditProbe=Disposable->DebugPilotCreditProbeForTest();
    FVoxelBrickIndexDelta NoChanges;
    auto Noop=Disposable->DebugPrepareIndexForTest(NoChanges);
    TestTrue(TEXT("No-op reservation is concrete"),Noop.IsValid() && CreditProbe()>0);
    Noop->Commit(NoChanges);
    TestEqual(TEXT("Commit that queues nothing retires credits"),CreditProbe(),uint64(0));
    TestEqual(TEXT("Retained no-op token owns no uncharged arrays"),Noop->DebugRetainedArrayBytes(),uint64(0));
    TArray<FVoxelBrickPreparedIndexDeliveryRef> ConsumedTokens;
    for(int32 I=0;I<8;++I)
    {
        auto Lease=Disposable->DebugPrepareIndexForTest(NoChanges);
        TestTrue(TEXT("Repeated no-op reserves real storage"),Lease->DebugRetainedArrayBytes()>0);
        Lease->Commit(NoChanges);ConsumedTokens.Add(Lease);
        TestEqual(TEXT("Retained consumed tokens cannot accumulate arrays"),Lease->DebugRetainedArrayBytes(),uint64(0));
        TestEqual(TEXT("Repeated no-op returns credit"),CreditProbe(),uint64(0));
    }
    Noop.Reset();
    auto Orphan=Disposable->DebugPrepareIndexForTest(NoChanges);
    Disposable.Reset();
    TestFalse(TEXT("Destroyed owner invalidates outstanding reservation"),Orphan->ValidateForCommit());
    Orphan.Reset();
    TestEqual(TEXT("Reservation destruction releases retired owner's credits"),CreditProbe(),uint64(0));
    FlushRenderingCommands();
    return true;
}
#endif
