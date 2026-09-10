#pragma once
#include "CoreMinimal.h"
#include "VoxelBrickPool.h"
struct FVoxelBrickAppearanceRay { FVector3f Origin=FVector3f::ZeroVector;float Reach=0;FVector3f Direction=FVector3f(0,0,-1);uint32 Level=0; };
struct FVoxelBrickAppearanceHit { uint32 Hit=0,Material=0;int32 X=0,Y=0,Z=0,Axis=0,Sign=0;float Distance=0; };
static_assert(sizeof(FVoxelBrickAppearanceRay)==32);
static_assert(sizeof(FVoxelBrickAppearanceHit)==32);
// Bounded test instrument. Origins/reach are in the selected level's local UU
// frame (10 UU per cell), matching production levelled shadow traversal.
VOXELEARTHSHADERS_API bool VoxelRunBrickAppearanceProbe(FVoxelBrickPool& Pool,const TArray<FVoxelBrickChunkKey>& Keys,
    const TArray<FVoxelBrickAppearanceRay>& Rays,TArray<FVoxelBrickAppearanceHit>& Flat,TArray<FVoxelBrickAppearanceHit>& Hier,FString& Error);
