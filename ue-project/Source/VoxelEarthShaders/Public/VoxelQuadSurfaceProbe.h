#pragma once
#include "CoreMinimal.h"
struct FVoxelQuadSurfaceProbeQuery {
    float X=0,Y=0,Z=0,Identity=0;
    float DxX=0,DxY=0,DxZ=0;uint32 Material=0;
    float DyX=0,DyY=0,DyZ=0;uint32 Reserved=0;
};
struct FVoxelQuadSurfaceProbeResult {float R=0,G=0,B=0,Coverage=1;uint32 Approved=0,DecodedIdentity=0,Reserved0=0,Reserved1=0;};
static_assert(sizeof(FVoxelQuadSurfaceProbeQuery)==48);
static_assert(sizeof(FVoxelQuadSurfaceProbeResult)==32);
// Diagnostic actual quad helper, with explicit pixel derivatives. Water uses
// the vertex factory's slot-zero identity. Does not exercise raster depth.
VOXELEARTHSHADERS_API bool VoxelRunQuadSurfaceProbe(const TArray<uint32>& Pages,
    const TArray<uint32>& Sources,const TArray<FUintVector2>& Ranges,const TArray<FUintVector4>& Slots,
    const TArray<FVoxelQuadSurfaceProbeQuery>& Queries,TArray<FVoxelQuadSurfaceProbeResult>& Output,FString& Error);
