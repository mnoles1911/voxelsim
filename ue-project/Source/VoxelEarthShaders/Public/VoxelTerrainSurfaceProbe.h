#pragma once
#include "CoreMinimal.h"
struct FVoxelTerrainSurfaceProbeQuery {
    uint32 PageBase=0,PageCount=0,RangeBase=0,RangeCount=0;
    int32 X=0,Y=0,Z=0;uint32 Axis=2;
    float FractionX=.5f,FractionY=.5f,FractionZ=1.f;uint32 Positive=1;
    uint32 MaterialsLow=0,MaterialsHigh=0,Steps=2;float PatternFootprint=0;
    float PitchMm=100;uint32 Reserved0=0,Reserved1=0,Reserved2=0;
};
struct FVoxelTerrainSurfaceProbeResult {
    uint32 Hit=0,Step=0,Material=0,Rejected=0;
    uint32 Approved=0,FirstApproved=0;float FirstCoverage=1,Coverage=1;
    float R=0,G=0,B=0,U=0;
    float V=0;int32 X=0,Y=0,Z=0;
};
static_assert(sizeof(FVoxelTerrainSurfaceProbeQuery)==80);
static_assert(sizeof(FVoxelTerrainSurfaceProbeResult)==64);
// Diagnostic front-to-back cell walk, at most8cells per ray; material bytes
// low-word first. Calls the ACTUAL surface helper and continues coverage<.5.
// Does not substitute for production BrickTraverse/shadow/GI entrypoint tests.
VOXELEARTHSHADERS_API bool VoxelRunTerrainSurfaceProbe(const TArray<uint32>& Pages,
    const TArray<uint32>& Sources,const TArray<FUintVector2>& Ranges,
    const TArray<FVoxelTerrainSurfaceProbeQuery>& Queries,
    TArray<FVoxelTerrainSurfaceProbeResult>& Output,FString& Error);
