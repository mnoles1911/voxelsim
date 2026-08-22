#include "VoxelEarthUI.h"

#include "VoxelUIAssetLibrary.h"
#include "VoxelUIStyle.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogVoxelUI);

FRandomStream MakeVoxelUIRandomStream()
{
	if (FApp::IsUnattended())
	{
		// Any constant would do; this one is the project's default world seed,
		// so a reader who greps for it finds an explanation rather than a
		// magic number.
		return FRandomStream(20260719);
	}
	return FRandomStream(int32(FPlatformTime::Cycles()));
}

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
	// a plain singleton, so the module does it explicitly. The asset library
	// goes first: its brushes reference the textures its cache holds, and a
	// brush outliving its texture is the one ordering that crashes.
	FVoxelUIAssetLibrary::Shutdown();
	FVoxelUIStyle::Shutdown();
}

IMPLEMENT_MODULE(FVoxelEarthUIModule, VoxelEarthUI)
