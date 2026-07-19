#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

// Shared module-wide log category (used by UVoxelWorldSubsystem to report
// generation totals -- docs/m1-plan.md stage 1 verification).
VOXELEARTH_API DECLARE_LOG_CATEGORY_EXTERN(LogVoxelEarth, Log, All);

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
