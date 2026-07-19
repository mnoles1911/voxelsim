#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * Primary game module for VoxelEarth.
 *
 * M1 scaffold: proves that a UE 5.7 module can include voxel-core headers
 * and link against the prebuilt voxelcore.lib static library, and that the
 * linked determinism goldens (docs/determinism.md) match.
 */
class FVoxelEarthModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
