#include "VoxelMeshAttributeFingerprint.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "StaticMeshResources.h"
#include <limits>
namespace {
TUniquePtr<FStaticMeshRenderData> FingerprintFixture(int Mutation=0){
    auto D=MakeUnique<FStaticMeshRenderData>();D->AllocateLODResources(2);
    for(int L=0;L<2;++L){auto& R=D->LODResources[L];auto& V=R.VertexBuffers;
        D->ScreenSize[L].Default=L?.25f:1.f;
        V.PositionVertexBuffer.Init(3);V.StaticMeshVertexBuffer.Init(3,2);V.ColorVertexBuffer.InitFromSingleColor(FColor(30,90,150,180),3);
        for(uint32 I=0;I<3;++I){V.PositionVertexBuffer.VertexPosition(I)=FVector3f(I==1?1.f:0.f,I==2?1.f:0.f,float(L));
            V.StaticMeshVertexBuffer.SetVertexTangents(I,FVector3f(1,0,0),FVector3f(0,1,0),FVector3f(0,0,1));
            V.StaticMeshVertexBuffer.SetVertexUV(I,0,FVector2f(.25f*I,.5f));V.StaticMeshVertexBuffer.SetVertexUV(I,1,FVector2f(.125f*I,.75f));}
        R.IndexBuffer.SetIndices(TArray<uint32>{0,1,2},EIndexBufferStride::Force16Bit);
        R.DepthOnlyIndexBuffer.SetIndices(TArray<uint32>{0,1,2},EIndexBufferStride::Force16Bit);
        auto& S=R.Sections.AddDefaulted_GetRef();S.NumTriangles=1;S.MinVertexIndex=0;S.MaxVertexIndex=2;
        R.AdditionalIndexBuffers=new FAdditionalStaticMeshIndexBuffers();
        R.AdditionalIndexBuffers->ReversedIndexBuffer.SetIndices(TArray<uint32>{2,1,0},EIndexBufferStride::Force16Bit);
        R.AdditionalIndexBuffers->ReversedDepthOnlyIndexBuffer.SetIndices(TArray<uint32>{2,1,0},EIndexBufferStride::Force16Bit);
        R.AdditionalIndexBuffers->WireframeIndexBuffer.SetIndices(TArray<uint32>{0,1,1,2,2,0},EIndexBufferStride::Force16Bit);
    }
    auto& R=D->LODResources[0];auto& V=R.VertexBuffers;
    switch(Mutation){
        case 1:V.PositionVertexBuffer.VertexPosition(0).X=.125f;break;
        case 2:V.StaticMeshVertexBuffer.SetVertexTangents(0,FVector3f(0,1,0),FVector3f(-1,0,0),FVector3f(0,0,1));break;
        case 3:V.ColorVertexBuffer.VertexColor(0).R++;break;
        case 4:V.ColorVertexBuffer.VertexColor(0).A++;break;
        case 5:V.StaticMeshVertexBuffer.SetVertexUV(0,0,FVector2f(.5f,.5f));break;
        case 6:V.StaticMeshVertexBuffer.SetVertexUV(0,1,FVector2f(.5f,.75f));break;
        case 7:R.IndexBuffer.SetIndices(TArray<uint32>{0,2,1},EIndexBufferStride::Force16Bit);break;
        case 8:R.Sections[0].MaterialIndex=1;break;
        case 9:D->ScreenSize[1].Default=.3f;break;
        case 10:D->LODResources[1].VertexBuffers.PositionVertexBuffer.VertexPosition(0).Z=2;break;
        case 11:R.DepthOnlyIndexBuffer.SetIndices(TArray<uint32>{0,2,1},EIndexBufferStride::Force16Bit);break;
        case 12:R.AdditionalIndexBuffers->ReversedIndexBuffer.SetIndices(TArray<uint32>{0,1,2},EIndexBufferStride::Force16Bit);break;
        case 13:R.AdditionalIndexBuffers->ReversedDepthOnlyIndexBuffer.SetIndices(TArray<uint32>{0,1,2},EIndexBufferStride::Force16Bit);break;
        case 14:R.AdditionalIndexBuffers->WireframeIndexBuffer.SetIndices(TArray<uint32>{0,2},EIndexBufferStride::Force16Bit);break;
        case 15:R.Sections[0].bCastShadow=false;break;
        case 16:V.StaticMeshVertexBuffer.SetVertexTangents(0,FVector3f(1,0,0),FVector3f(0,0,1),FVector3f(0,-1,0));break;
    }
    return D;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMeshAttributeFingerprintTest,"Voxel.Appearance.MeshAttributeFingerprint",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelMeshAttributeFingerprintTest::RunTest(const FString&){
    auto Base=FingerprintFixture();FVoxelMeshAttributeFingerprint A,B;FString Error;
    if(!TestTrue(TEXT("CPU fixture fingerprints"),VoxelFingerprintMeshAttributes(*Base,A,Error)))return false;
    TestEqual(TEXT("two explicit LOD records"),A.Lods.Num(),2);TestEqual(TEXT("descriptive vertex count"),A.Lods[0].Vertices,uint32(3));TestEqual(TEXT("both UV channels counted"),A.Lods[0].TexCoords,uint32(2));
    auto Repeat=FingerprintFixture();TestTrue(TEXT("fresh allocation fingerprints"),VoxelFingerprintMeshAttributes(*Repeat,B,Error));TestEqual(TEXT("same semantic buffers stable across allocations"),A.Sha256,B.Sha256);
    Repeat->LODResources[0].IndexBuffer.SetIndices(TArray<uint32>{0,1,2},EIndexBufferStride::Force32Bit);
    TestTrue(TEXT("different index storage fingerprints"),VoxelFingerprintMeshAttributes(*Repeat,B,Error));TestEqual(TEXT("16/32 bit storage encodes identical index values"),A.Sha256,B.Sha256);
    for(int Mutation=1;Mutation<=16;++Mutation){auto Changed=FingerprintFixture(Mutation);
        TestTrue(TEXT("changed valid buffer fingerprints"),VoxelFingerprintMeshAttributes(*Changed,B,Error));TestTrue(FString::Printf(TEXT("mutation%d changes digest"),Mutation),B.Sha256!=A.Sha256);}
    auto Invalid=FingerprintFixture();Invalid->LODResources[0].VertexBuffers.PositionVertexBuffer.VertexPosition(0).X=std::numeric_limits<float>::quiet_NaN();
    B=A;TestFalse(TEXT("nonfinite attributes refused"),VoxelFingerprintMeshAttributes(*Invalid,B,Error));TestEqual(TEXT("failed fingerprint leaves output unchanged"),B.Sha256,A.Sha256);
    auto Missing=FingerprintFixture();Missing->LODResources[0].VertexBuffers.ColorVertexBuffer.CleanUp();
    TestFalse(TEXT("missing CPU color data refused"),VoxelFingerprintMeshAttributes(*Missing,B,Error));
    auto MissingPosition=FingerprintFixture();MissingPosition->LODResources[0].VertexBuffers.PositionVertexBuffer.CleanUp();
    TestFalse(TEXT("cleaned position allocation refused despite stale cached pointer/count"),VoxelFingerprintMeshAttributes(*MissingPosition,B,Error));
    auto MissingTangents=FingerprintFixture();MissingTangents->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.CleanUp();
    TestFalse(TEXT("cleaned tangent/UV ownership refused despite cached metadata"),VoxelFingerprintMeshAttributes(*MissingTangents,B,Error));
    // TResourceArray::Discard is intentionally a no-op in editor/commandlet;
    // explicitly remove storage to exercise a missing primary index payload.
    auto MissingIndices=FingerprintFixture();MissingIndices->LODResources[0].IndexBuffer.SetIndices(TArray<uint32>{},EIndexBufferStride::Force16Bit);
    TestFalse(TEXT("missing main index storage refused"),VoxelFingerprintMeshAttributes(*MissingIndices,B,Error));
    auto BadIndex=FingerprintFixture();BadIndex->LODResources[0].IndexBuffer.SetIndices(TArray<uint32>{0,1,9},EIndexBufferStride::Force16Bit);
    TestFalse(TEXT("out of range indices refused"),VoxelFingerprintMeshAttributes(*BadIndex,B,Error));
    return true;
}
#endif
