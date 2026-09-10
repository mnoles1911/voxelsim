#include "VoxelAssetColumnAppend.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelAssetColumnAppendTest,
    "Voxel.Worklist.AssetColumnAppend",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVoxelAssetColumnAppendTest::RunTest(const FString&)
{
    TArray<uint32> Actual{19u, 27u};
    TArray<uint32> Reference = Actual;
    const int32 Sizes[] = {0, 1, 4097, 0, 65537, 3};
    const uint32 Offsets[] = {0u, 13u, 900u, 0u, 0xfffffff0u, 7u};
    for (int32 Batch = 0; Batch < UE_ARRAY_COUNT(Sizes); ++Batch)
    {
        TArray<uint32> Source;
        for (int32 I = 0; I < Sizes[Batch]; ++I) Source.Add(uint32(I) * 31u);
        const TArray<uint32> Original = Source;
        for (uint32 Column : Source) Reference.Add(Column + Offsets[Batch]);
        VoxelAssetColumns::AppendRebased(Actual, Source, Offsets[Batch]);
        TestTrue(TEXT("rebased payload preserves prefix, order and unsigned offsets"), Actual == Reference);
        TestTrue(TEXT("source payload unchanged"), Source == Original);
    }
    return true;
}
#endif
