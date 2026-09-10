#pragma once
#include "CoreMinimal.h"
struct FVoxelApprovedAppearanceProbeQuery {
    uint32 PackedMaterialRGB=0,WorldAxis=0,Positive=0,Yaw=0;
    int32 SourceZeroX=0,SourceZeroY=0,SourceZeroZ=0;uint32 Needle=0;
    float FractionX=0,FractionY=0,FractionZ=0,PitchMm=100;
    float PatternFootprint=0;uint32 Foliage=1,Reserved0=0,Reserved1=0;
};
struct FVoxelApprovedAppearanceProbeResult {
    float R=0,G=0,B=0,U=0,V=0,Coverage=0;uint32 Valid=0,Reserved=0;
};
static_assert(sizeof(FVoxelApprovedAppearanceProbeQuery)==64);
static_assert(sizeof(FVoxelApprovedAppearanceProbeResult)==32);
// GT diagnostic only, synchronous GPU readback, <=65536 queries. Footprint is
// max(length(ddx(UV/scale)),length(ddy(UV/scale))) or equivalent ray differential.
// SourceZero is original source zero-based; Fraction is world-cell relative.
VOXELEARTHSHADERS_API bool VoxelRunApprovedAppearanceProbe(
    const TArray<FVoxelApprovedAppearanceProbeQuery>& Queries,
    TArray<FVoxelApprovedAppearanceProbeResult>& Output,FString& Error);
