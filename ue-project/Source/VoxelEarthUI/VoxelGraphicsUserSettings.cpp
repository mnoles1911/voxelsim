#include "VoxelGraphicsUserSettings.h"

#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"

namespace
{
constexpr const TCHAR* kSection = TEXT("VoxelGraphics");
constexpr const TCHAR* kFineDetailKey = TEXT("FineDetailSmoothing");
constexpr const TCHAR* kFasterTerrainKey = TEXT("FasterTerrainDrawing");
constexpr const TCHAR* kWaterWaveKey = TEXT("WaterWaveDetail");
constexpr const TCHAR* kOceanDetailKey = TEXT("OceanMeshDetail");

// The cvar each row fronts, spelled once. A misspelling here does not fail to
// compile and does not warn: FindConsoleVariable returns null and the toggle
// becomes decoration. That is why every apply below RETURNS whether it found
// its variable, and why ApplyAll names the ones it could not -- an apply that
// set nothing used to log exactly the same line as one that worked.
constexpr const TCHAR* kFineDetailVar = TEXT("r.TSR.ThinGeometryDetection");
constexpr const TCHAR* kFasterTerrainVar = TEXT("voxel.March.TemporalPrime");
constexpr const TCHAR* kWaterWaveVar = TEXT("voxel.Water.WaveTessRadiusM");
constexpr const TCHAR* kOceanDetailVar = TEXT("voxel.Ocean.HalfDetail");

// SetByGameSetting: below SetByConsole, so a developer poking the cvar from the
// console for an A/B still wins over the persisted setting for that session --
// the exact precedence the perf legs rely on. It is also, deliberately, the
// priority AVoxelWaterSheetActor::BeginPlay uses for -VoxelWaveTessM, so a leg
// flag and this cannot silently outrank each other; see that call site.
bool SetVar(const TCHAR* Name, int32 Value)
{
	IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
	if (!Var)
	{
		return false;
	}
	Var->Set(Value, ECVF_SetByGameSetting);
	return true;
}

// For the rows whose ON state is the CVAR'S OWN DEFAULT rather than a number
// this file knows. Spelling the ON value here too would make this a second
// authority on a value that belongs with the feature, and the two would drift
// the first time the feature was retuned -- silently, because nothing compares
// them. GetDefaultValue() is the constructor default and is unaffected by
// anything that has since Set() the variable.
bool SetVarDefaultOr(const TCHAR* Name, bool bWantDefault, const TCHAR* OffValue)
{
	IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
	if (!Var)
	{
		return false;
	}
	const FString OnValue = Var->GetDefaultValue();
	Var->Set(bWantDefault ? *OnValue : OffValue, ECVF_SetByGameSetting);
	return true;
}

// The one place each setting's cvar spelling and its polarity live.
//
// The first two are named for what the PLAYER gets and map 1:1 onto their cvar
// -- no inversion to mis-remember. The last one does NOT: see its note.
bool ApplyFineDetail(bool bEnabled)
{
	return SetVar(kFineDetailVar, bEnabled ? 1 : 0);
}

bool ApplyFasterTerrain(bool bEnabled)
{
	return SetVar(kFasterTerrainVar, bEnabled ? 1 : 0);
}

// ON restores the cvar's default disc radius (80 m today); OFF is 0, which is
// the feature's own documented OFF control -- no tessellated vertex is emitted
// at all. The radius is read at every sheet rebuild, so a flip lands as the
// round-robin revisits basins rather than at the next launch.
bool ApplyWaterWaveDetail(bool bEnabled)
{
	return SetVarDefaultOr(kWaterWaveVar, bEnabled, TEXT("0"));
}

// INVERTED, AND THIS IS THE PLACE THAT INVERTS IT. The setting says how much
// mesh the player gets; the cvar says whether the renderer builds half of it.
// ON = the cvar's default (0, full detail); OFF = 1. Written as "restore the
// default" rather than as "set 0" for the same reason as the wave row above:
// the default is the feature's to choose, not this file's.
bool ApplyOceanMeshDetail(bool bEnabled)
{
	return SetVarDefaultOr(kOceanDetailVar, bEnabled, TEXT("1"));
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

bool GetWaterWaveDetail()
{
	// Default TRUE = the shipped cvar default (an 80 m tessellation disc).
	bool bEnabled = true;
	if (GConfig)
	{
		GConfig->GetBool(kSection, kWaterWaveKey, bEnabled, GGameUserSettingsIni);
	}
	return bEnabled;
}

void SetWaterWaveDetail(bool bEnabled)
{
	if (GConfig)
	{
		GConfig->SetBool(kSection, kWaterWaveKey, bEnabled, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}
	ApplyWaterWaveDetail(bEnabled);
}

bool GetOceanMeshDetail()
{
	// Default TRUE = full detail = voxel.Ocean.HalfDetail's default of 0. The
	// bool and the cvar disagree in VALUE and agree in MEANING; the inversion
	// lives at the apply site and nowhere else.
	bool bEnabled = true;
	if (GConfig)
	{
		GConfig->GetBool(kSection, kOceanDetailKey, bEnabled, GGameUserSettingsIni);
	}
	return bEnabled;
}

void SetOceanMeshDetail(bool bEnabled)
{
	if (GConfig)
	{
		GConfig->SetBool(kSection, kOceanDetailKey, bEnabled, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}
	ApplyOceanMeshDetail(bEnabled);
}

void ApplyAll()
{
	const bool bFineOk = ApplyFineDetail(GetFineDetailSmoothing());
	const bool bTerrainOk = ApplyFasterTerrain(GetFasterTerrainDrawing());
	const bool bWaveOk = ApplyWaterWaveDetail(GetWaterWaveDetail());
	const bool bOceanOk = ApplyOceanMeshDetail(GetOceanMeshDetail());

	// Engagement line, once per apply: "the switch is on" and "the key was
	// misspelled and nothing latched" must not produce identical logs -- the
	// eleven-inert-features lesson, applied to a settings screen. The values
	// alone were never enough for that, because they are read back out of the
	// ini and are correct whether or not a cvar was ever found; naming the
	// MISSING variables is the half that can actually fail.
	FString Missing;
	auto Note = [&Missing](bool bOk, const TCHAR* Name)
	{
		if (!bOk)
		{
			if (!Missing.IsEmpty())
			{
				Missing += TEXT(", ");
			}
			Missing += Name;
		}
	};
	Note(bFineOk, kFineDetailVar);
	Note(bTerrainOk, kFasterTerrainVar);
	Note(bWaveOk, kWaterWaveVar);
	Note(bOceanOk, kOceanDetailVar);

	UE_LOG(LogTemp, Log,
	       TEXT("VoxelGraphicsUserSettings: applied FineDetailSmoothing=%d FasterTerrainDrawing=%d "
	            "WaterWaveDetail=%d OceanMeshDetail=%d"),
	       GetFineDetailSmoothing() ? 1 : 0, GetFasterTerrainDrawing() ? 1 : 0, GetWaterWaveDetail() ? 1 : 0,
	       GetOceanMeshDetail() ? 1 : 0);
	if (!Missing.IsEmpty())
	{
		// WARNING, NOT LOG. A cvar that is not registered means the module that
		// owns the feature did not load, or the name moved -- either way that
		// row is a dead switch and the player has no way to tell.
		UE_LOG(LogTemp, Warning,
		       TEXT("VoxelGraphicsUserSettings: SET NOTHING for %s -- those console variables are not "
		            "registered, so the settings rows fronting them are inert this session."),
		       *Missing);
	}
}
} // namespace VoxelGraphicsUserSettings
