#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelBrickPool.h"
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelAppearancePageLifetimeTest,"Voxel.Appearance.PageHostLifetime",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelAppearancePageLifetimeTest::RunTest(const FString&) {
    FVoxelBrickPool Pool;FVoxelBrickPoolConfig Config;Config.ChunkCapacity=2;Config.OccWordCapacity=1024;Config.MatWordCapacity=1024;Pool.Init(Config);
    auto Air=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>();Air->Desc.SetNumZeroed(128);
    const FVoxelBrickChunkKey Key{-2,3,-1,0};
    TSharedPtr<FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe> Page=MakeShared<FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe>();Page->PageKey=FIntVector(-2,3,-1);Page->Generation=19;
    TSharedPtr<FVoxelTerrainAppearanceSources,ESPMode::ThreadSafe> Sources=MakeShared<FVoxelTerrainAppearanceSources,ESPMode::ThreadSafe>();Page->Sources=Sources;
    TWeakPtr<const FVoxelTerrainAppearanceSources,ESPMode::ThreadSafe> WeakSources=Sources;
    TestTrue(TEXT("valid empty appearance accepted"),Pool.AddChunkFromCpu(Air,Key,FVoxelBrickChunkShading::Neutral(),Page)!=INDEX_NONE);
    FVoxelBrickPool::FResidentChunk Before;TestTrue(TEXT("resident metadata available"),Pool.DebugGetResidentChunk(Key,Before));
    TestTrue(TEXT("same page retained with geometry"),Before.Appearance==Page);
    auto Wrong=MakeShared<FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe>();Wrong->Generation=20;Wrong->PageKey=FIntVector(8,3,-1);
    TestEqual(TEXT("mismatched page rejected before replacement"),Pool.AddChunkFromCpu(Air,Key,FVoxelBrickChunkShading::Neutral(),Wrong),INDEX_NONE);
    FVoxelBrickPool::FResidentChunk After;Pool.DebugGetResidentChunk(Key,After);
    TestEqual(TEXT("rejected admission preserves revision"),After.AddSequence,Before.AddSequence);
    TestTrue(TEXT("rejected admission preserves source mapping"),After.Appearance==Page);
    Before.Appearance.Reset();After.Appearance.Reset();Page.Reset();Sources.Reset();
    TestTrue(TEXT("resident retains source snapshot"),WeakSources.IsValid());
    TestTrue(TEXT("ordinary replacement succeeds"),Pool.AddChunkFromCpu(Air,Key,FVoxelBrickChunkShading::Neutral())!=INDEX_NONE);
    Pool.DebugGetResidentChunk(Key,After);TestFalse(TEXT("ordinary replacement clears appearance"),After.Appearance.IsValid());
    TestFalse(TEXT("replaced pending and resident references retired"),WeakSources.IsValid());
    Pool.RemoveChunk(Key);return true;
}
#endif
