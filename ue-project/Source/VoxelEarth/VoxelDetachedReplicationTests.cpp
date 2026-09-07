#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelDetachedReplication.h"
#include "Misc/AutomationTest.h"
#include "Misc/Crc.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelDetachedWireTest,"Voxel.Objects.NetworkAssembly",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelDetachedWireTest::RunTest(const FString&)
{
    using namespace VoxelDetachedWire;
    FAssembly A;const FGuid Id=FGuid::NewGuid();TArray<uint8> Bytes;
    for(int32 I=0;I<ChunkBytes+7;++I)Bytes.Add(uint8(I%251));
    const uint32 Crc=FCrc::MemCrc32(Bytes.GetData(),Bytes.Num());
    TArray<uint8> First(Bytes.GetData(),ChunkBytes),Last(Bytes.GetData()+ChunkBytes,7);
    TestFalse(TEXT("Reject missing first fragment"),A.Add(Id,1,Bytes.Num(),Crc,ChunkBytes,Last));
    TestFalse(TEXT("Reject oversized allocation"),A.Add(Id,1,MaxSnapshotBytes+1,Crc,0,First));
    TestTrue(TEXT("Accept first bounded chunk"),A.Add(Id,1,Bytes.Num(),Crc,0,First));
    TestFalse(TEXT("Incomplete data cannot publish"),A.Complete());
    TestFalse(TEXT("Duplicate chunk rejected"),A.Add(Id,1,Bytes.Num(),Crc,0,First));
    TestFalse(TEXT("Cross object chunks rejected"),A.Add(FGuid::NewGuid(),1,Bytes.Num(),Crc,ChunkBytes,Last));
    TestFalse(TEXT("Cross revision chunks rejected"),A.Add(Id,2,Bytes.Num(),Crc,ChunkBytes,Last));
    TestFalse(TEXT("Overlapping fragment rejected"),A.Add(Id,1,Bytes.Num(),Crc,ChunkBytes-1,Last));
    TestTrue(TEXT("Final fragment completes exact bytes"),A.Add(Id,1,Bytes.Num(),Crc,ChunkBytes,Last));
    TestTrue(TEXT("Checksum verified before install"),A.Complete());
    TestTrue(TEXT("Assembled bytes identical"),A.Data==Bytes);
    A.Reset();Last[0]^=1;
    A.Add(Id,1,Bytes.Num(),Crc,0,First);A.Add(Id,1,Bytes.Num(),Crc,ChunkBytes,Last);
    TestFalse(TEXT("Corrupt snapshot cannot publish"),A.Complete());
    return true;
}
#endif
