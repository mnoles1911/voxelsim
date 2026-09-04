#include "VoxelGraphicsUserSettings.h"

#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"

namespace
{
constexpr const TCHAR* kSection = TEXT("VoxelGraphics");
constexpr const TCHAR* kFineDetailKey = TEXT("FineDetailSmoothing");
constexpr const TCHAR* kFasterTerrainKey = TEXT("FasterTerrainDrawing");

// The one place the setting's cvar spelling and its polarity live. The
// setting is named for what the PLAYER gets (smoothing of fine silhouettes),
// which maps 1:1 onto the engine cvar -- no inversion to mis-remember.
void ApplyFineDetail(bool bEnabled)
{
	if (IConsoleVariable* Var =
	        IConsoleManager::Get().FindConsoleVariable(TEXT("r.TSR.ThinGeometryDetection")))
	{
		// SetByGameSetting: below SetByConsole, so a developer poking the cvar
		// from the console for an A/B still wins over the persisted setting
		// for that session -- the exact precedence the perf legs rely on.
		Var->Set(bEnabled ? 1 : 0, ECVF_SetByGameSetting);
	}
}

void ApplyFasterTerrain(bool bEnabled)
{
	if (IConsoleVariable* Var =
	        IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.March.TemporalPrime")))
	{
		Var->Set(bEnabled ? 1 : 0, ECVF_SetByGameSetting);
	}
}
} // namespace

namespace VoxelGraphicsUserSettings
{
bool GetFineDetailSmoothing()
{
	// Default FALSE = the shipped DefaultEngine.ini value (see the ini's
	// comment block for the A/B receipts). A missing key reads as the
	// shipped default rather than the engine default, so a fresh install and
	// a pre-feature save behave identically.
	bool bEnabled = false;
	if (GConfig)
	{
		GConfig->GetBool(kSection, kFineDetailKey, bEnabled, GGameUserSettingsIni);
	}
	return bEnabled;
}

void SetFineDetailSmoothing(bool bEnabled)
{
	if (GConfig)
	{
		GConfig->SetBool(kSection, kFineDetailKey, bEnabled, GGameUserSettingsIni);
		// Flush now, not at shutdown: a crash between toggle and exit must not
		// silently revert a choice the player watched take effect.
		GConfig->Flush(false, GGameUserSettingsIni);
	}
	ApplyFineDetail(bEnabled);
}

bool GetFasterTerrainDrawing()
{
	// Default TRUE = the shipped cvar default; see the header's ship record.
	bool bEnabled = true;
	if (GConfig)
	{
		GConfig->GetBool(kSection, kFasterTerrainKey, bEnabled, GGameUserSettingsIni);
	}
	return bEnabled;
}

void SetFasterTerrainDrawing(bool bEnabled)
{
	if (GConfig)
	{
		GConfig->SetBool(kSection, kFasterTerrainKey, bEnabled, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}
	ApplyFasterTerrain(bEnabled);
}

void ApplyAll()
{
	ApplyFineDetail(GetFineDetailSmoothing());
	ApplyFasterTerrain(GetFasterTerrainDrawing());
	// Engagement line, once per apply: "the switch is on" and "the key was
	// misspelled and nothing latched" must not produce identical logs -- the
	// eleven-inert-features lesson, applied to a settings screen.
	UE_LOG(LogTemp, Log,
	       TEXT("VoxelGraphicsUserSettings: applied FineDetailSmoothing=%d FasterTerrainDrawing=%d"),
	       GetFineDetailSmoothing() ? 1 : 0, GetFasterTerrainDrawing() ? 1 : 0);
}
} // namespace VoxelGraphicsUserSettings
