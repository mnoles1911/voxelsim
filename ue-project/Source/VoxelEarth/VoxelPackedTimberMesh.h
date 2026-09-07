#pragma once
#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"

namespace VoxelPackedTimberMesh
{
inline const FGuid VersionKey(0x7AE01123,0x8C3E42A1,0x9BF09917,0xD30C0612);
struct FVertex
{
    FVector3f Position,Normal,Tangent;
    FVector2f UV0,UV1;
    FColor Color;
    uint32 Flip;
};
static_assert(sizeof(FVertex)==60,"Packed mesh must have no unwritten padding");
inline bool Serialize(FArchive& Ar,FProcMeshSection& Out,const FProcMeshSection* Source)
{
    int32 Count=Ar.IsSaving()?Source->ProcVertexBuffer.Num():0;Ar<<Count;
    if(Ar.IsError()||Count<0||Count>262144)return false;
    const int64 Size=int64(Count)*sizeof(FVertex);
    if(Ar.IsLoading()&&Size>Ar.TotalSize()-Ar.Tell())return false;
    TArray<FVertex> Packed;Packed.SetNumUninitialized(Count);
    if(Ar.IsSaving())for(int32 I=0;I<Count;++I){
        const auto& V=Source->ProcVertexBuffer[I];auto& P=Packed[I];
        P.Position=FVector3f(V.Position);P.Normal=FVector3f(V.Normal);P.Tangent=FVector3f(V.Tangent.TangentX);
        P.UV0=FVector2f(V.UV0);P.UV1=FVector2f(V.UV1);P.Color=V.Color;P.Flip=V.Tangent.bFlipTangentY?1:0;
        // Renderer-authored local voxel positions are float-representable.
        // Refuse unsupported precision rather than moving an authored voxel.
        if(FVector(P.Position)!=V.Position)return false;
    }
    if(Count)Ar.Serialize(Packed.GetData(),Size);if(Ar.IsError())return false;
    if(Ar.IsLoading()){
        Out.ProcVertexBuffer.SetNum(Count);
        for(int32 I=0;I<Count;++I){auto& V=Out.ProcVertexBuffer[I];const auto& P=Packed[I];if(P.Flip>1)return false;
            V.Position=FVector(P.Position);V.Normal=FVector(P.Normal);V.Tangent=FProcMeshTangent(FVector(P.Tangent),P.Flip!=0);
            V.UV0=FVector2D(P.UV0);V.UV1=FVector2D(P.UV1);V.Color=P.Color;
        }
    }
    int32 Indices=Ar.IsSaving()?Source->ProcIndexBuffer.Num():0;Ar<<Indices;
    if(Ar.IsError()||Indices<0||Indices>393216||Indices%3)return false;
    const int64 IndexBytes=int64(Indices)*sizeof(uint32);
    if(Ar.IsLoading()){
        if(IndexBytes>Ar.TotalSize()-Ar.Tell())return false;
        Out.ProcIndexBuffer.SetNumUninitialized(Indices);if(Indices)Ar.Serialize(Out.ProcIndexBuffer.GetData(),IndexBytes);
    }else if(Indices)Ar.Serialize(const_cast<uint32*>(Source->ProcIndexBuffer.GetData()),IndexBytes);
    return !Ar.IsError();
}
}
