#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelBrickPool.h"
#include "VoxelPreparedIndexTestSupport.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPreparedBatchTokenTest,"Voxel.Objects.PreparedBatchToken",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelPreparedBatchTokenTest::RunTest(const FString&)
{
    FVoxelBrickPoolConfig Config; Config.ChunkCapacity=8; Config.OccWordCapacity=2048; Config.MatWordCapacity=2048;
    FVoxelBrickPool Pool; Pool.Init(Config);
    int32 Deliveries=0; TArray<FVoxelBrickIndexEntry> Initial;
    const FVoxelBrickIndexSink Sink=[&](const auto&){++Deliveries;};
    VoxelPreparedIndexTestSupport::SetSink(Pool,Sink,Initial);
    auto Air=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>(); Air->Desc.SetNumZeroed(128);
    const FVoxelBrickChunkKey A{0,0,0,0},Absent{1,0,0,0};
    Pool.AddChunkFromCpu(Air,A,{}); Pool.Flush(); const auto Old=Pool.SnapshotAllocation(A);
    auto Pages=[&]() { FVoxelBrickPreparedReplacement P; P.Key=A; P.CpuPack=Air;
        const auto T=Pool.SnapshotAllocation(A); P.ExpectedSlot=T.Slot; P.ExpectedSequence=T.AddSequence;
        return TArray<FVoxelBrickPreparedReplacement>{P}; };
    const int32 Before=Deliveries;
    TestFalse(TEXT("zero byte budget refuses"),Pool.PreparePreparedBatch(Pages(),{}, {},0).IsValid());
    auto Token=Pool.PreparePreparedBatch(Pages());
    TestTrue(TEXT("opaque reservation prepared"),Token.IsValid()); if(!Token)return false;
    TestTrue(TEXT("reservation validates"),Pool.ValidatePreparedBatch(Token));
    TestTrue(TEXT("CPU copy and metadata charged"),Token->GetRetainedBytes()>=512);
    TestEqual(TEXT("prepare preserves resident identity"),Pool.SnapshotAllocation(A).AddSequence,Old.AddSequence);
    TestEqual(TEXT("prepare does not deliver index"),Deliveries,Before);
    TestFalse(TEXT("second reservation bounded"),Pool.PreparePreparedBatch(Pages()).IsValid());
    Air->Desc.Empty(); // mutable caller copy cannot alter the private snapshot
    TestTrue(TEXT("private CPU snapshot survives caller mutation"),Pool.ValidatePreparedBatch(Token));
    Air->Desc.SetNumZeroed(128);
    TestTrue(TEXT("explicit cancellation succeeds"),Pool.CancelPreparedBatch(Token));
    TestEqual(TEXT("retained cancelled handle has no arrays"),Token->GetRetainedBytes(),uint64(0));
    TestFalse(TEXT("cancelled token invalid"),Pool.ValidatePreparedBatch(Token));
    TestEqual(TEXT("cancellation preserves old slot"),Pool.SnapshotAllocation(A).Slot,Old.Slot);
    TArray<FVoxelBrickPreparedBatchRef> RetainedCancelled;
    for(int32 I=0;I<12;++I)
    {
        auto R=Pool.PreparePreparedBatch(Pages());
        if(!TestTrue(TEXT("cancel returns arena reservation for reuse"),R.IsValid()))return false;
        Pool.CancelPreparedBatch(R); RetainedCancelled.Add(R);
        TestEqual(TEXT("retained cancelled handles cannot accumulate array storage"),R->GetRetainedBytes(),uint64(0));
    }
    Token=Pool.PreparePreparedBatch(Pages());
    Pool.AddChunkFromCpu(Air,Absent,{});
    TestFalse(TEXT("ordinary mutation stales token"),Pool.ValidatePreparedBatch(Token));
    TestEqual(TEXT("stale validation leaves target untouched"),Pool.SnapshotAllocation(A).AddSequence,Old.AddSequence);
    Pool.CancelPreparedBatch(Token); Pool.RemoveChunk(Absent); Pool.Flush();
    const TArray<FVoxelBrickChunkKey> Keys{A,Absent}, Absences{Absent};
    auto Pins=Pool.AcquireEvictionPins(Keys);
    TestFalse(TEXT("replacement and absence overlap refused"),Pool.PreparePreparedBatch(Pages(),Pins,TArray<FVoxelBrickChunkKey>{A}).IsValid());
    Token=Pool.PreparePreparedBatch(Pages(),Pins,Absences);
    TestTrue(TEXT("full present/absent footprint reserved"),Token.IsValid() && Pool.ValidatePreparedBatch(Token));
    TestTrue(TEXT("valid token proves exact absence"),Pool.PreparedBatchCoversAbsent(Token,Absent));
    TestFalse(TEXT("token does not assert absence for replacement"),Pool.PreparedBatchCoversAbsent(Token,A));
    Pool.ReleaseEvictionPins(Pins);
    TestFalse(TEXT("stale token cannot prove absence"),Pool.PreparedBatchCoversAbsent(Token,Absent));
    TestFalse(TEXT("released pressure ticket stales reservation"),Pool.ValidatePreparedBatch(Token));
    Pool.CancelPreparedBatch(Token);
    Pins=Pool.AcquireEvictionPins(Keys); Token=Pool.PreparePreparedBatch(Pages(),Pins,Absences);
    if(!TestTrue(TEXT("fresh footprint validates"),Pool.ValidatePreparedBatch(Token)))return false;
    Pool.CommitPreparedBatch(Token);
    TestFalse(TEXT("commit consumes token"),Pool.ValidatePreparedBatch(Token));
    TestEqual(TEXT("retained consumed token has no arrays"),Token->GetRetainedBytes(),uint64(0));
    TestTrue(TEXT("commit advances resident identity"),Pool.SnapshotAllocation(A).AddSequence!=Old.AddSequence);
    TestFalse(TEXT("absence remains absent"),Pool.SnapshotAllocation(Absent).bPresent);
    Pool.ReleaseEvictionPins(Pins);
    Token=Pool.PreparePreparedBatch(Pages()); Pool.Reset();
    TestFalse(TEXT("reset invalidates outstanding token"),Pool.ValidatePreparedBatch(Token));
    TestEqual(TEXT("reset releases retained token storage"),Token->GetRetainedBytes(),uint64(0));
    FVoxelBrickPreparedBatchRef Orphan;
    {
        auto Temporary=MakeUnique<FVoxelBrickPool>(); Temporary->Init(Config);
        VoxelPreparedIndexTestSupport::SetSink(*Temporary,Sink,Initial);
        FVoxelBrickPreparedReplacement P;P.Key=A;P.CpuPack=Air;
        Orphan=Temporary->PreparePreparedBatch(TArray<FVoxelBrickPreparedReplacement>{P});
        TestTrue(TEXT("temporary pool has reservation"),Orphan.IsValid());
    }
    if(!Orphan)return false;
    TestEqual(TEXT("pool destruction releases orphan storage"),Orphan->GetRetainedBytes(),uint64(0));
    TestFalse(TEXT("cross-pool orphan cannot validate"),Pool.ValidatePreparedBatch(Orphan));
    Orphan.Reset(); Token.Reset(); FlushRenderingCommands();
    return true;
}
#endif
