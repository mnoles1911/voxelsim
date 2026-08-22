#pragma once
// Module header for the front end. Owns nothing but the log category and the
// lifetime of FVoxelUIStyle -- see VoxelEarthUI.Build.cs for why this module
// exists at all.

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

VOXELEARTHUI_API DECLARE_LOG_CATEGORY_EXTERN(LogVoxelUI, Log, All);

class FVoxelEarthUIModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
