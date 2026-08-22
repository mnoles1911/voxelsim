#pragma once
// Module header for the front end. Owns nothing but the log category and the
// lifetime of FVoxelUIStyle -- see VoxelEarthUI.Build.cs for why this module
// exists at all.

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "Modules/ModuleInterface.h"

VOXELEARTHUI_API DECLARE_LOG_CATEGORY_EXTERN(LogVoxelUI, Log, All);

// The front end's one source of randomness: which background is showing, and
// which quip and tip come up first.
//
// DETERMINISTIC UNDER -unattended, ON PURPOSE. Every capture switch runs
// unattended, and a capture strip is only worth anything if two runs of the
// same command produce comparable images -- a randomly chosen background would
// make every -VoxelMenuShot diff a false positive. Interactive launches get a
// clock-seeded stream, so a player still gets a different opening every time.
VOXELEARTHUI_API FRandomStream MakeVoxelUIRandomStream();

class FVoxelEarthUIModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
