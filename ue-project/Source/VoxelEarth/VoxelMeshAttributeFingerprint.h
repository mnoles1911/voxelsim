#pragma once
#include "CoreMinimal.h"
class FStaticMeshRenderData;
struct FVoxelMeshLodFingerprint {
    uint32 Vertices=0,TexCoords=0,Sections=0;
    uint32 MainIndices=0,DepthIndices=0,ReversedIndices=0,ReversedDepthIndices=0,WireframeIndices=0;
    float ScreenSize=0;
    FString Sha256;
};
struct FVoxelMeshAttributeFingerprint {
    static constexpr uint32 SchemaVersion=1;
    FString Sha256;
    TArray<FVoxelMeshLodFingerprint> Lods;
};
// Offline verification only. Caller must retain CPU-readable, resident buffers
// and prevent concurrent mutation/stream-out. CPU-access ownership is required
// even before RHI initialization; cached pointers alone may survive CleanUp.
// Fails closed on unavailable data.
// Schema1 hashes decoded attributes in explicit little-endian IEEE32/u32/RGBA
// order; -0 is canonical +0, nonfinite floats are refused. Index width/padding
// and UObject addresses are excluded. Material indices/section flags are included;
// actual material/shader contents must be pinned separately by the bake manifest.
// Equality proves buffer persistence, not source-geometry or visual equivalence.
bool VoxelFingerprintMeshAttributes(const FStaticMeshRenderData& Data,
    FVoxelMeshAttributeFingerprint& Out,FString& Error);
