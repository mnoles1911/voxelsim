#pragma once
#include "VoxelDebris.h"
#include "VoxelAppearanceBankBinding.h"
#include "voxelcore/world.h"
namespace VoxelDebrisCapture {
// Synchronous pre-removal snapshot of the actual world overlay. The caller
// must not edit the world during capture. False leaves Out empty and means
// the removal/handoff must be refused, rather than re-reading after removal.
bool Capture(const vxc::World<8>& World,const FVoxelAppearanceBankBinding* Binding,
    const TArray<VoxelCoords::FVoxelCoord>& Cells,TArray<FVoxelDebrisCellAppearance>& Out);
}
