#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelProductionEnvironmentAdapter.h"
#include "VoxelPreparedIndexTestSupport.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelProductionPageBatchTest,"Voxel.Objects.ProductionPageBatch",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelProductionPageBatchTest::RunTest(const FString&)
{
    auto Air=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>();Air->Desc.SetNumZeroed(128);
    for(uint32 Capacity:{3u,4u}){
        int32 Publications=0;FVoxelBrickIndexDelta Published;
        FVoxelBrickPool Pool;FVoxelBrickPoolConfig Config;Config.ChunkCapacity=Capacity;Config.OccWordCapacity=1024;Config.MatWordCapacity=1024;Pool.Init(Config);
        TArray<FVoxelBrickIndexEntry> Initial;VoxelPreparedIndexTestSupport::SetSink(Pool,[&](const auto& Delta){++Publications;Published=Delta;},Initial);
        TArray<FVoxelBrickPreparedReplacement> Pages;
        for(int32 I=0;I<2;++I){FVoxelBrickChunkKey Key{I,0,0,0};Pool.AddChunkFromCpu(Air,Key,FVoxelBrickChunkShading{});FVoxelBrickPool::FResidentChunk Old;
            if(!Pool.DebugGetResidentChunk(Key,Old)){AddError(TEXT("Failed fixture allocation"));return false;}
            auto& Page=Pages.AddDefaulted_GetRef();Page.Key=Key;Page.ExpectedSlot=int32(Old.ChunkSlot);Page.ExpectedSequence=Old.AddSequence;Page.CpuPack=Air;
        }
        Pool.Flush();Publications=0;Published={};
        auto Stale=Pages;Stale[0].ExpectedSequence++;
        TestFalse(TEXT("Concurrent remesh rejects whole prepared batch"),Pool.PublishPreparedBatch(Stale));
        TestEqual(TEXT("Rejected revision publishes no delta"),Publications,0);
        const bool Success=Pool.PublishPreparedBatch(Pages);
        TestEqual(TEXT("Reserve requires space for all old and new pages"),Success,Capacity==4);
        if(Capacity==3){
            TestEqual(TEXT("Partial reservation failure publishes nothing"),Publications,0);
            for(const auto& Page:Pages)TestEqual(TEXT("Rollback retains old visible slot"),Pool.FindChunkSlot(Page.Key),Page.ExpectedSlot);
            TestEqual(TEXT("Rollback retains resident count"),Pool.GetNumResidentChunks(),2);
        }else{
            TestEqual(TEXT("Complete replacement emits one index batch"),Publications,1);
            TestEqual(TEXT("Both new pages in batch"),Published.Added.Num(),2);
            TestEqual(TEXT("Both old slots retired in batch"),Published.Removed.Num(),2);
            for(const auto& Page:Pages)TestTrue(TEXT("Replacement retains old ranges until new reservations exist"),Pool.FindChunkSlot(Page.Key)!=Page.ExpectedSlot);
        }
        TArray<FVoxelBrickIndexEntry> Ignored;Pool.SetIndexSink(nullptr,Ignored);
    }
    return true;
}
#endif
