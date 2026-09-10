#include "VoxelMeshAttributeFingerprint.h"
#include "StaticMeshResources.h"
#include <openssl/sha.h>
namespace {
struct FWriter {
    SHA256_CTX State;bool Valid=true;
    FWriter(){SHA256_Init(&State);}
    void U32(uint32 V){uint8 B[4]={uint8(V),uint8(V>>8),uint8(V>>16),uint8(V>>24)};SHA256_Update(&State,B,4);}
    void F32(float V){if(!FMath::IsFinite(V)){Valid=false;return;}if(V==0)V=0;uint32 B;FMemory::Memcpy(&B,&V,4);U32(B);}
    void V3(const FVector3f& V){F32(V.X);F32(V.Y);F32(V.Z);}
    void V4(const FVector4f& V){F32(V.X);F32(V.Y);F32(V.Z);F32(V.W);}
    void Color(FColor C){uint8 B[4]={C.R,C.G,C.B,C.A};SHA256_Update(&State,B,4);}
    FString Finish(){uint8 B[32];SHA256_Final(B,&State);return BytesToHex(B,32).ToLower();}
};
bool Indices(const FRawStaticIndexBuffer& B,uint32 Vertices,FWriter& H,uint32& Count){
    const int32 N=B.GetNumIndices();if(N<0||N>60000000)return false;
    Count=uint32(N);H.U32(Count);if(!N)return true;
    if(B.IsInitialized()&&!B.GetAllowCPUAccess())return false;
    // The array view reflects owned storage, unlike cached index counts.
    // Validate its live length and allocation before reading any element.
    const auto V=B.GetArrayView();
    const uint64 Required=uint64(N)*(B.Is32Bit()?4u:2u);
    if(V.Num()!=N||uint64(B.GetAllocatedSize())<Required)return false;
    for(int32 I=0;I<N;++I){if(V[I]>=Vertices)return false;H.U32(V[I]);}return true;
}
}
bool VoxelFingerprintMeshAttributes(const FStaticMeshRenderData& Data,FVoxelMeshAttributeFingerprint& Out,FString& Error){
    auto Fail=[&](const TCHAR* Why){Error=Why;return false;};
    if(Data.LODResources.Num()<1||Data.LODResources.Num()>MAX_STATIC_MESH_LODS)return Fail(TEXT("Invalid LOD count"));
    FVoxelMeshAttributeFingerprint Result;FWriter Whole;Whole.U32(0x31464d56);Whole.U32(Result.SchemaVersion);Whole.U32(Data.LODResources.Num());
    for(int32 L=0;L<Data.LODResources.Num();++L){
        const auto& R=Data.LODResources[L];const auto& V=R.VertexBuffers;FVoxelMeshLodFingerprint F;
        F.Vertices=V.PositionVertexBuffer.GetNumVertices();F.TexCoords=V.StaticMeshVertexBuffer.GetNumTexCoords();F.Sections=R.Sections.Num();F.ScreenSize=Data.ScreenSize[L].Default;
        if(!F.Vertices||F.Vertices>10000000||!F.TexCoords||F.TexCoords>MAX_STATIC_TEXCOORDS||F.Sections>65536||
           V.StaticMeshVertexBuffer.GetNumVertices()!=F.Vertices||V.ColorVertexBuffer.GetNumVertices()!=F.Vertices)
            return Fail(TEXT("Incomplete vertex channels or exceeded fingerprint bounds"));
        // CleanUp can leave the cached data pointers and counts nonzero in
        // UE5.8. GetAllowCPUAccess checks the owning allocation objects, so it
        // must be checked even before RHI initialization. Pointer-only checks
        // would read freed storage after an offline cleanup/stream transition.
        if(!V.PositionVertexBuffer.GetAllowCPUAccess()||!V.StaticMeshVertexBuffer.GetAllowCPUAccess()||!V.ColorVertexBuffer.GetAllowCPUAccess())
            return Fail(TEXT("CPU vertex allocation ownership/retention unavailable"));
        if(!V.PositionVertexBuffer.GetVertexData()||!V.StaticMeshVertexBuffer.GetTangentData()||!V.StaticMeshVertexBuffer.GetTexCoordData()||!V.ColorVertexBuffer.GetVertexData()||
           uint64(V.PositionVertexBuffer.GetAllocatedSize())<uint64(F.Vertices)*sizeof(FVector3f)||
           uint64(V.ColorVertexBuffer.GetAllocatedSize())<uint64(F.Vertices)*sizeof(FColor)||
           V.StaticMeshVertexBuffer.GetTangentSize()<=0||V.StaticMeshVertexBuffer.GetTexCoordSize()<=0)
            return Fail(TEXT("Vertex CPU data unavailable or undersized; retain CPU data and resident LODs"));
        FWriter H;H.U32(Result.SchemaVersion);H.U32(L);H.U32(F.Vertices);H.U32(F.TexCoords);H.U32(F.Sections);H.F32(F.ScreenSize);
        for(uint32 I=0;I<F.Vertices;++I){
            H.V3(V.PositionVertexBuffer.VertexPosition(I));
            H.V4(V.StaticMeshVertexBuffer.VertexTangentX(I));H.V3(V.StaticMeshVertexBuffer.VertexTangentY(I));H.V4(V.StaticMeshVertexBuffer.VertexTangentZ(I));
            H.Color(V.ColorVertexBuffer.VertexColor(I));
            for(uint32 T=0;T<F.TexCoords;++T){const auto UV=V.StaticMeshVertexBuffer.GetVertexUV(I,T);H.F32(UV.X);H.F32(UV.Y);}
        }
        if(!Indices(R.IndexBuffer,F.Vertices,H,F.MainIndices)||!Indices(R.DepthOnlyIndexBuffer,F.Vertices,H,F.DepthIndices))return Fail(TEXT("Primary/depth index CPU data unavailable or invalid"));
        H.U32(R.AdditionalIndexBuffers?1:0);
        if(R.AdditionalIndexBuffers){const auto& A=*R.AdditionalIndexBuffers;
            if(!Indices(A.ReversedIndexBuffer,F.Vertices,H,F.ReversedIndices)||!Indices(A.ReversedDepthOnlyIndexBuffer,F.Vertices,H,F.ReversedDepthIndices)||!Indices(A.WireframeIndexBuffer,F.Vertices,H,F.WireframeIndices))return Fail(TEXT("Additional index CPU data unavailable or invalid"));}
        for(const auto& S:R.Sections){
            if(S.MaterialIndex<0||uint64(S.FirstIndex)+uint64(S.NumTriangles)*3>F.MainIndices||S.MinVertexIndex>S.MaxVertexIndex||S.MaxVertexIndex>=F.Vertices)return Fail(TEXT("Invalid material section"));
            H.U32(uint32(S.MaterialIndex));H.U32(S.FirstIndex);H.U32(S.NumTriangles);H.U32(S.MinVertexIndex);H.U32(S.MaxVertexIndex);
            H.U32(S.bEnableCollision);H.U32(S.bCastShadow);H.U32(S.bVisibleInRayTracing);H.U32(S.bAffectDistanceFieldLighting);H.U32(S.bForceOpaque);
        }
        if(!H.Valid||F.ScreenSize<0)return Fail(TEXT("Nonfinite attribute or invalid screen size"));
        F.Sha256=H.Finish();uint8 Digest[32];HexToBytes(F.Sha256,Digest);SHA256_Update(&Whole.State,Digest,32);Result.Lods.Add(MoveTemp(F));
    }
    Result.Sha256=Whole.Finish();Out=MoveTemp(Result);Error.Reset();return true;
}
