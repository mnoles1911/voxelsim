#include "VoxelPackedTimberMesh.h"
#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPackedTimberMeshTest,"Voxel.Save.PackedTimber",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelPackedTimberMeshTest::RunTest(const FString&)
{
    FProcMeshSection Original,Restored;
    for(int32 I=0;I<4;++I){FProcMeshVertex V;V.Position=FVector(I*1.25,-I*2.5,10000+I*5.);
        V.Normal=FVector(0,0,1);V.Tangent=FProcMeshTangent(FVector(1,0,0),I%2!=0);
        V.UV0=FVector2D(I*.125,I*.25);V.UV1=FVector2D(I*.5,100);V.Color=FColor(17,29,43,128);
        Original.ProcVertexBuffer.Add(V);}
    Original.ProcIndexBuffer={0,1,2,0,2,3};TArray<uint8> Bytes;
    {FMemoryWriter Ar(Bytes);TestTrue(TEXT("pack valid voxel mesh"),VoxelPackedTimberMesh::Serialize(Ar,Restored,&Original));}
    TestEqual(TEXT("fixed bulk layout"),Bytes.Num(),272);
    {FMemoryReader Ar(Bytes);TestTrue(TEXT("unpack valid voxel mesh"),VoxelPackedTimberMesh::Serialize(Ar,Restored,nullptr));}
    TestTrue(TEXT("indices preserved"),Restored.ProcIndexBuffer==Original.ProcIndexBuffer);
    for(int32 I=0;I<4;++I){const auto& A=Original.ProcVertexBuffer[I];const auto& B=Restored.ProcVertexBuffer[I];
        TestTrue(TEXT("voxel coordinates exact"),A.Position==B.Position);
        TestTrue(TEXT("normal and tangent preserved"),A.Normal==B.Normal&&A.Tangent.TangentX==B.Tangent.TangentX&&A.Tangent.bFlipTangentY==B.Tangent.bFlipTangentY);
        TestTrue(TEXT("UVs and foliage/color metadata preserved"),A.UV0==B.UV0&&A.UV1==B.UV1&&A.Color==B.Color);}
    auto Truncated=Bytes;Truncated.SetNum(12);
    {FMemoryReader Ar(Truncated);TestFalse(TEXT("truncated bulk rejected before allocation"),VoxelPackedTimberMesh::Serialize(Ar,Restored,nullptr));}
    TArray<uint8> Oversized;{FMemoryWriter Ar(Oversized);int32 Count=MAX_int32;Ar<<Count;}
    {FMemoryReader Ar(Oversized);TestFalse(TEXT("oversized count rejected"),VoxelPackedTimberMesh::Serialize(Ar,Restored,nullptr));}
    Original.ProcVertexBuffer[0].Position.X=1./3.;Bytes.Reset();
    {FMemoryWriter Ar(Bytes);TestFalse(TEXT("unsupported coordinate precision refused"),VoxelPackedTimberMesh::Serialize(Ar,Restored,&Original));}
    return true;
}
#endif
