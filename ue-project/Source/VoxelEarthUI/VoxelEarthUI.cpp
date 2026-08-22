#include "VoxelEarthUI.h"

#include "VoxelUIStyle.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogVoxelUI);

void FVoxelEarthUIModule::StartupModule()
{
	// FVoxelUIStyle holds FSlateBrush and FSlateFontInfo instances, which are
	// Slate resources. Building them here, at module startup, rather than
	// lazily on first widget construction, means the font failure (if the .ttf
	// is missing) is reported once at boot in the log rather than at the
	// moment the menu appears -- which, in an unattended capture run, is after
	// the only human who could have noticed has gone home.
	FVoxelUIStyle::Startup();
}

void FVoxelEarthUIModule::ShutdownModule()
{
	// Slate resources must not outlive the renderer. Nothing enforces that for
	// a plain singleton, so the module does it explicitly.
	FVoxelUIStyle::Shutdown();
}

IMPLEMENT_MODULE(FVoxelEarthUIModule, VoxelEarthUI)
