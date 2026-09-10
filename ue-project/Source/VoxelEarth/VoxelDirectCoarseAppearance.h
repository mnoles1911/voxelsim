#pragma once
#include "VoxelTerrainAppearancePage.h"
#include "voxelcore/generator.h"
#include <functional>
// Direct makeCoarseBrick fallback ONLY. Recursive downsampleBricks has a
// different source-selection rule and must not be attributed with this helper.
namespace VoxelDirectCoarseAppearance {
// makeCoarseBrick is terrain-only. Compose the immutable canonical ordered
// asset list at the SAME representative, only where that terrain is air.
std::function<vxc::MaterialId(int64,int64,int64)> MakeSampler(
    std::function<vxc::MaterialId(int64,int64,int64)> Terrain,
    std::vector<vxc::AssetField::ResolvedAssetInstance> Ordered,uint32 Level);
TSharedPtr<const FVoxelTerrainAppearancePage,ESPMode::ThreadSafe> Prepare(
    const vxc::GeneratedWorld<8>& Generated,FIntVector Key,uint32 Level,uint64 Generation,
    const std::vector<vxc::AssetField::ResolvedAssetInstance>& Ordered,
    const FVoxelAppearanceBankBinding& Binding,bool SurfacePreserve,
    TFunctionRef<vxc::MaterialId(int64,int64,int64)> ActualGeometry,FString& Error);
}
