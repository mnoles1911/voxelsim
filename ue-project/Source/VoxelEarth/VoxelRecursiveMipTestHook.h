#pragma once
#include "CoreMinimal.h"
#include "voxelcore/brick.h"
#include <functional>
#if WITH_DEV_AUTOMATION_TESTS
struct FVoxelRecursiveMipSeed { int32 Level=1;vxc::BrickKey Key;vxc::Brick<8> Brick; };
struct FVoxelRecursiveMipTestResult {
    bool Valid=false;int64 X=0,Y=0,Z=0;vxc::MaterialId Material=vxc::MAT_AIR;
};
// Test-only bridge to the actual private builder. Seeded entries model cache
// hits from earlier jobs; WarmJob populates a real separate builder first.
FVoxelRecursiveMipTestResult VoxelTestRecursiveMipTrace(
    std::function<const vxc::Brick<8>*(const vxc::BrickKey&)> Source,
    const TArray<FVoxelRecursiveMipSeed>& Seeds,int32 Level,int64 X,int64 Y,int64 Z,
    bool Shared,bool WarmJob=false,bool AdvanceEpoch=false,int32 Threshold=4);
#endif
