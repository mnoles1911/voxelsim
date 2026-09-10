#pragma once
#include "CoreMinimal.h"
struct FVoxelSparseAppearanceProbeQuery {uint32 X=0,Y=0,Z=0,Material=0;};
struct FVoxelSparseAppearanceProbeResult {
    uint32 Hit=0,Resource=0,PackedMaterialRGB=0,Needle=0;
    int32 SourceX=0,SourceY=0,SourceZ=0;uint32 Yaw=0;
    uint32 ZeroX=0,ZeroY=0,ZeroZ=0,Reserved=0;
};
static_assert(sizeof(FVoxelSparseAppearanceProbeQuery)==16);
static_assert(sizeof(FVoxelSparseAppearanceProbeResult)==48);
// Explicit diagnostic only. GT synchronous render-command + GPU readback.
// SourceRanges[resourceID]=(wordBase,wordLength), index0 reserved. No activation.
// Bounded to64MiB perwordbuffer and65536queries; failure preserves output.
VOXELEARTHSHADERS_API bool VoxelRunSparseAppearanceProbe(const TArray<uint32>& PageWords,
    const TArray<uint32>& SourceWords,const TArray<FUintVector2>& SourceRanges,
    const TArray<FVoxelSparseAppearanceProbeQuery>& Queries,
    TArray<FVoxelSparseAppearanceProbeResult>& Output,FString& Error);
