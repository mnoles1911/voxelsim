#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelBrickPool.h"
#include "VoxelPreparedIndexTestSupport.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBrickAllocationTokenTest,
    "Voxel.Objects.BrickAllocationToken", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVoxelBrickAllocationTokenTest::RunTest(const FString&)
{
    FVoxelBrickPool Pool;
    const FVoxelBrickChunkKey Key{7,-3,2,1};
    auto CheckAbsent=[&](const FVoxelBrickAllocationToken& Token){
        TestFalse(TEXT("Absent token is explicit"),Token.bPresent);
        TestEqual(TEXT("Absent slot is canonical"),Token.Slot,int32(INDEX_NONE));
        TestEqual(TEXT("Absent sequence is canonical"),Token.AddSequence,uint64(0));
    };
    CheckAbsent(Pool.SnapshotAllocation(Key));
    FVoxelBrickPoolConfig Config;Config.ChunkCapacity=3;Config.OccWordCapacity=1024;Config.MatWordCapacity=1024;Pool.Init(Config);
    int32 Publications=0;
    TArray<FVoxelBrickIndexEntry> Initial;
    VoxelPreparedIndexTestSupport::SetSink(Pool,[&](const auto&){++Publications;},Initial);
    auto Air=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>();Air->Desc.SetNumZeroed(128);
    auto Replacement=[&](const FVoxelBrickAllocationToken& Token){
        FVoxelBrickPreparedReplacement Page;Page.Key=Key;Page.ExpectedSlot=Token.Slot;Page.ExpectedSequence=Token.AddSequence;Page.CpuPack=Air;
        TArray<FVoxelBrickPreparedReplacement> Pages;Pages.Add(Page);return Pages;
    };
    const auto Absent=Pool.SnapshotAllocation(Key);CheckAbsent(Absent);
    Pool.AddChunkFromCpu(Air,Key,FVoxelBrickChunkShading{});
    const auto First=Pool.SnapshotAllocation(Key);
    TestTrue(TEXT("Allocated token is present"),First.bPresent);
    TestEqual(TEXT("Token names the resident slot"),First.Slot,Pool.FindChunkSlot(Key));
    TestTrue(TEXT("Allocated sequence is nonzero"),First.AddSequence!=0);
    Pool.Flush();Publications=0;
    TestFalse(TEXT("Old absence cannot overwrite new resident"),Pool.PublishPreparedBatch(Replacement(Absent)));
    TestEqual(TEXT("Rejected absence emits no publication"),Publications,0);
    Pool.RemoveChunk(Key);CheckAbsent(Pool.SnapshotAllocation(Key));
    Pool.AddChunkFromCpu(Air,Key,FVoxelBrickChunkShading{});
    const auto Reinserted=Pool.SnapshotAllocation(Key);
    TestTrue(TEXT("Reinserted identity has a new sequence"),Reinserted.AddSequence!=First.AddSequence);
    Pool.Flush();Publications=0;
    TestFalse(TEXT("Removed allocation cannot replace reinserted resident"),Pool.PublishPreparedBatch(Replacement(First)));
    TestEqual(TEXT("Stale token emits no publication"),Publications,0);
    TestEqual(TEXT("Stale rejection preserves current identity"),Pool.SnapshotAllocation(Key).AddSequence,Reinserted.AddSequence);
    TestTrue(TEXT("Current resident token admits replacement"),Pool.PublishPreparedBatch(Replacement(Reinserted)));
    TestEqual(TEXT("Successful replacement publishes once"),Publications,1);
    Pool.RemoveChunk(Key);Pool.Flush();Publications=0;
    TestTrue(TEXT("Current absence admits insertion"),Pool.PublishPreparedBatch(Replacement(Pool.SnapshotAllocation(Key))));
    TestEqual(TEXT("Absent insertion publishes once"),Publications,1);
    TArray<FVoxelBrickIndexEntry> Ignored;Pool.SetIndexSink(nullptr,Ignored);
    return true;
}
#endif
