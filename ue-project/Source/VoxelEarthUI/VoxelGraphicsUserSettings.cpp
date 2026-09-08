#include "VoxelGraphicsUserSettings.h"

#include "Framework/Application/SlateApplication.h" // SetApplicationScale -- the UI-size row
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"

namespace
{
constexpr const TCHAR* kSection = TEXT("VoxelGraphics");
constexpr const TCHAR* kFineDetailKey = TEXT("FineDetailSmoothing");
constexpr const TCHAR* kFasterTerrainKey = TEXT("FasterTerrainDrawing");
constexpr const TCHAR* kWaterWaveKey = TEXT("WaterWaveDetail");
constexpr const TCHAR* kOceanDetailKey = TEXT("OceanMeshDetail");
constexpr const TCHAR* kUIScaleKey = TEXT("UIScale");
constexpr const TCHAR* kHideMusicUIKey = TEXT("HideMusicUI");
constexpr const TCHAR* kHideCompassUIKey = TEXT("HideCompassUI");

// The interface-size row's bounds. 1.00 is the default: the engine's own
// ShortestSide curve reaching the screen unmodified, which is the framing
// ADR-0011 chose. 0.05 is coarse enough to cross the range in fifteen presses
// and fine enough to land on a size.
constexpr float kUIScaleDefault = 1.00f;
constexpr float kUIScaleMin = 0.75f;
constexpr float kUIScaleMax = 1.50f;
constexpr float kUIScaleStep = 0.05f;

// SNAPPED AND CLAMPED IN ONE PLACE. The slider hands over a continuous value
// and the ini can hold anything a text editor put there, so both routes go
// through this -- otherwise the stored number and the sixteen the row can
// display drift apart and the knob never sits where it was left.
float SnapUIScale(float Scale)
{
	const float Snapped = FMath::RoundToFloat(Scale / kUIScaleStep) * kUIScaleStep;
	return FMath::Clamp(Snapped, kUIScaleMin, kUIScaleMax);
}

// Applies to Slate. Returns false when there is no Slate application at all --
// a commandlet or a server -- which is a real state on this project (the
// dedicated server links no Slate) and not an error.
bool ApplyUIScale(float Scale)
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}
	FSlateApplication::Get().SetApplicationScale(SnapUIScale(Scale));
	return true;
}

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

bool GetHideMusicUI()
{
	// FALSE on a missing key: nothing is hidden until the player hides it.
	bool bHidden = false;
	if (GConfig)
	{
		GConfig->GetBool(kSection, kHideMusicUIKey, bHidden, GGameUserSettingsIni);
	}
	return bHidden;
}

void SetHideMusicUI(bool bHidden)
{
	if (GConfig)
	{
		GConfig->SetBool(kSection, kHideMusicUIKey, bHidden, GGameUserSettingsIni);
		// Flushed now, on this file's own standing reason: a crash between the
		// toggle and exit must not silently revert a choice the player watched
		// take effect.
		GConfig->Flush(false, GGameUserSettingsIni);
	}
	// NO ApplyAll(). There is no cvar and no apply step -- SVoxelGameHud reads
	// this every frame through a bound visibility attribute, so the row is live
	// the instant it is clicked. The log line is the engagement evidence that
	// takes ApplyAll's place for these two rows.
	UE_LOG(LogTemp, Log, TEXT("VoxelHud: music UI hidden=%d"), bHidden ? 1 : 0);
}

bool GetHideCompassUI()
{
	bool bHidden = false;
	if (GConfig)
	{
		GConfig->GetBool(kSection, kHideCompassUIKey, bHidden, GGameUserSettingsIni);
	}
	return bHidden;
}

void SetHideCompassUI(bool bHidden)
{
	if (GConfig)
	{
		GConfig->SetBool(kSection, kHideCompassUIKey, bHidden, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}
	UE_LOG(LogTemp, Log, TEXT("VoxelHud: compass UI hidden=%d"), bHidden ? 1 : 0);
}

float UIScaleMin() { return kUIScaleMin; }
float UIScaleMax() { return kUIScaleMax; }
float UIScaleStep() { return kUIScaleStep; }

float GetUIScale()
{
	float Scale = kUIScaleDefault;
	if (GConfig)
	{
		GConfig->GetFloat(kSection, kUIScaleKey, Scale, GGameUserSettingsIni);
	}
	// Snapped on the way OUT as well as in: a hand-edited ini holding 1.37 must
	// not make the slider sit between two of its own stops.
	return SnapUIScale(Scale);
}

void SetUIScale(float Scale)
{
	const float Snapped = SnapUIScale(Scale);
	if (GConfig)
	{
		GConfig->SetFloat(kSection, kUIScaleKey, Snapped, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}
	// LIVE, like the volume sliders and unlike FULLSCREEN: the only way to judge
	// an interface size is to watch it change under the cursor.
	ApplyUIScale(Snapped);
}

void ApplyAll()
{
	const bool bFineOk = ApplyFineDetail(GetFineDetailSmoothing());
	const bool bTerrainOk = ApplyFasterTerrain(GetFasterTerrainDrawing());
	const bool bWaveOk = ApplyWaterWaveDetail(GetWaterWaveDetail());
	const bool bOceanOk = ApplyOceanMeshDetail(GetOceanMeshDetail());
	// Not folded into the Missing list below: that list names CVARS that were
	// not registered, and "no Slate application" is a different fact about a
	// different kind of build. ApplyAll runs from
	// UVoxelFrontEndSubsystem::Initialize, which only exists where Slate does,
	// so a false here would be news.
	const bool bScaleOk = ApplyUIScale(GetUIScale());

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
	            "WaterWaveDetail=%d OceanMeshDetail=%d UIScale=%.2f%s "
	            "HideMusicUI=%d HideCompassUI=%d"),
	       GetFineDetailSmoothing() ? 1 : 0, GetFasterTerrainDrawing() ? 1 : 0, GetWaterWaveDetail() ? 1 : 0,
	       GetOceanMeshDetail() ? 1 : 0, GetUIScale(),
	       bScaleOk ? TEXT("") : TEXT(" (NOT APPLIED -- no Slate application)"),
	       // REPORTED, NOT APPLIED, and the distinction is the point of this
	       // line. These two have no cvar to push: the HUD reads them every
	       // frame. Naming them here means a leg can still see at boot what the
	       // player has chosen, without implying an apply that never happened.
	       GetHideMusicUI() ? 1 : 0, GetHideCompassUI() ? 1 : 0);
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
