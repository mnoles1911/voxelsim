#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelEnvironmentSparseGrid.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEnvironmentSparseCodecTest,"Voxel.Objects.EnvironmentSparseCodec",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelEnvironmentSparseCodecTest::RunTest(const FString&){
    FVoxelEnvironmentSparseGrid Grid;Grid.Size=FIntVector(1200,1200,1200);Grid.Origin=FIntVector(-601,-3,-77);Grid.Mm=25;
    TestTrue(TEXT("Large bounds accepted without dense allocation"),Grid.Init());
    TestTrue(TEXT("Run crosses chunks"),Grid.SetRun(3,4,7,21,16));
    TestTrue(TEXT("Far occupied cell"),Grid.Set(1199,1199,1199,2));
    TArray<uint8> Bytes;FMemoryWriter W(Bytes);TestTrue(TEXT("Sparse encode"),Grid.Serialize(W));
    TestTrue(TEXT("Wire proportional to occupied chunks"),Bytes.Num()<4096);
    FVoxelEnvironmentSparseGrid Copy;Copy.Size=Grid.Size;Copy.Origin=Grid.Origin;Copy.Mm=Grid.Mm;
    FMemoryReader R(Bytes);TestTrue(TEXT("Sparse decode"),Copy.Serialize(R));TestTrue(TEXT("Exact roundtrip"),Copy.Equals(Grid));
    TestEqual(TEXT("Origin-aware query"),Copy.LocalAt(-598,1,-70),uint8(16));
    Copy.MaxDataZ=10;Copy.Data.clearAbove(10);TestEqual(TEXT("Upper cell reclaimed"),Copy.At(1199,1199,1199),uint8(0));
    TestEqual(TEXT("Remaining lower run"),Copy.At(3,4,9),uint8(16));
    auto Truncated=Bytes;Truncated.SetNum(Truncated.Num()-1);FMemoryReader Bad(Truncated);
    TestFalse(TEXT("Truncated decode rejected"),Copy.Serialize(Bad));TestEqual(TEXT("Failed decode leaves contents"),Copy.At(3,4,9),uint8(16));
    // Legacy dense body uses Z-fastest ordering and is read without a dense copy.
    FVoxelEnvironmentSparseGrid Legacy;Legacy.Size=FIntVector(2,2,3);Legacy.Origin=FIntVector(-1,0,0);Legacy.Mm=50;
    TArray<uint8> Dense;FMemoryWriter DW(Dense);int32 N=12;DW<<N;uint8 Cells[12]={0,2,0,0,0,0,0,0,0,16,16,0};DW.Serialize(Cells,12);
    FMemoryReader DR(Dense);TestTrue(TEXT("Legacy dense accepted"),Legacy.Serialize(DR));
    TestEqual(TEXT("Legacy layout"),Legacy.At(1,1,1),uint8(16));TestEqual(TEXT("Legacy air"),Legacy.At(0,1,1),uint8(0));
    return true;
}
#endif
