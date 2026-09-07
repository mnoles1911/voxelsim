#include "VoxelObjectGeometrySnapshot.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelObjectSnapshotEnvelopeTest,
    "Voxel.Objects.GeometryEnvelope",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelObjectSnapshotEnvelopeTest::RunTest(const FString&){
    TArray<uint8> Geometry,Dynamic,Combined;
    {FMemoryWriter W(Geometry);VoxelObjectGeometrySnapshot::WriteVersion(W);uint32 G=0x12345678;W<<G;}
    {FMemoryWriter W(Dynamic);VoxelObjectGeometrySnapshot::WriteVersion(W);uint32 D=0x87654321;W<<D;}
    TestTrue(TEXT("valid split record combines"),VoxelObjectGeometrySnapshot::Combine(Geometry,Dynamic,Combined));
    TestEqual(TEXT("version prefixes excluded"),Combined.Num(),8);
    FMemoryReader Reader(Combined);uint32 D=0,G=0;Reader<<D<<G;
    TestEqual(TEXT("dynamic header comes first"),D,uint32(0x87654321));
    TestEqual(TEXT("geometry tail follows intact"),G,uint32(0x12345678));
    auto Bad=Geometry;Bad[0]=2;
    TestFalse(TEXT("unknown geometry version rejected"),VoxelObjectGeometrySnapshot::Combine(Bad,Dynamic,Combined));
    Bad=Dynamic;Bad[0]=2;
    TestFalse(TEXT("unknown dynamic version rejected"),VoxelObjectGeometrySnapshot::Combine(Geometry,Bad,Combined));
    Bad.SetNum(3);
    TestFalse(TEXT("truncated dynamic header rejected"),VoxelObjectGeometrySnapshot::Combine(Geometry,Bad,Combined));
    Bad.SetNumZeroed(4097);
    TestFalse(TEXT("unbounded dynamic payload rejected"),VoxelObjectGeometrySnapshot::Combine(Geometry,Bad,Combined));
    return true;
}
#endif
