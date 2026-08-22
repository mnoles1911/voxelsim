#include "VoxelFrontEndSwitches.h"

#include "VoxelEarthUI.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace VoxelFrontEndSwitchesDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// Reads "0.5,6,20" into an array. Returns false (leaving Out untouched) when
// the switch is absent, so the caller can distinguish "not passed" from
// "passed empty".
//
// bShouldStopOnSeparator=false, for the reason VoxelEarthSpawn::
// ParseSpawnColumnUU records: FParse::Value's default terminator set includes
// ',', which would silently truncate a comma-separated list at its first
// element. That trap has now bitten -VoxelSpawnAt and -VoxelTimeOfDay in this
// codebase; it is not going to bite a third switch.
bool ParseFloatList(const TCHAR* Key, TArray<float>& Out)
{
	FString Raw;
	if (!FParse::Value(FCommandLine::Get(), Key, Raw, /*bShouldStopOnSeparator=*/false))
	{
		return false;
	}
	TArray<FString> Parts;
	Raw.ParseIntoArray(Parts, TEXT(","), /*InCullEmpty=*/true);
	Out.Reset();
	for (const FString& Part : Parts)
	{
		Out.Add(FCString::Atof(*Part.TrimStartAndEnd()));
	}
	return true;
}

FVoxelFrontEndSwitches Parse()
{
	FVoxelFrontEndSwitches S;
	const TCHAR* Cmd = FCommandLine::Get();

	// -VoxelMenuShot / -VoxelMenuShot=<s>. Both forms, because every capture
	// switch in this project accepts both and a reader should not have to
	// remember which ones do.
	float Seconds = 0.f;
	if (FParse::Value(Cmd, TEXT("VoxelMenuShot="), Seconds))
	{
		S.bMenuShot = true;
		S.MenuShotSeconds = FMath::Max(Seconds, 0.f);
	}
	else if (FParse::Param(Cmd, TEXT("VoxelMenuShot")))
	{
		S.bMenuShot = true;
	}

	FParse::Value(Cmd, TEXT("VoxelMenuPanel="), S.MenuPanel);
	S.MenuPanel = S.MenuPanel.TrimStartAndEnd().ToLower();

	if (ParseFloatList(TEXT("VoxelLoadingShotAt="), S.LoadingShotSeconds))
	{
		S.bLoadingShot = true;
	}
	else if (FParse::Value(Cmd, TEXT("VoxelLoadingShot="), Seconds))
	{
		S.bLoadingShot = true;
		S.LoadingShotSeconds = {FMath::Max(Seconds, 0.f)};
	}
	else if (FParse::Param(Cmd, TEXT("VoxelLoadingShot")))
	{
		S.bLoadingShot = true;
		S.LoadingShotSeconds = {6.0f};
	}
	// Offsets are consumed in order, so an out-of-order list would silently
	// skip captures. Sorting is friendlier than refusing, and the log line
	// below reports what was actually used.
	S.LoadingShotSeconds.Sort();

	if (ParseFloatList(TEXT("VoxelHourglassShot="), S.HourglassProgress))
	{
		S.bHourglassShot = true;
		for (float& P : S.HourglassProgress)
		{
			P = FMath::Clamp(P, 0.f, 1.f);
		}
	}
	else if (FParse::Param(Cmd, TEXT("VoxelHourglassShot")))
	{
		S.bHourglassShot = true;
		S.HourglassProgress = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
	}

	if (FParse::Value(Cmd, TEXT("VoxelMenuAutoStart="), Seconds))
	{
		S.bAutoStart = true;
		S.AutoStartSeconds = FMath::Max(Seconds, 0.f);
	}
	else if (FParse::Param(Cmd, TEXT("VoxelMenuAutoStart")))
	{
		S.bAutoStart = true;
	}

	S.bNoAssets = FParse::Param(Cmd, TEXT("VoxelUINoAssets"));
	S.bReadyProbeLog = FParse::Param(Cmd, TEXT("VoxelReadyProbeLog"));

	FParse::Value(Cmd, TEXT("VoxelLoadGateMaxRing="), S.LoadGateMaxRing);
	FParse::Value(Cmd, TEXT("VoxelLoadMinHold="), S.LoadMinHoldSeconds);
	FParse::Value(Cmd, TEXT("VoxelLoadMaxHold="), S.LoadMaxHoldSeconds);
	FParse::Value(Cmd, TEXT("VoxelMenuWatchdog="), S.MenuWatchdogSeconds);

	// A min hold longer than the max hold is a typo that would otherwise show
	// up as "the loading screen never closes early", which reads like a bug in
	// the gate rather than in the flags.
	if (S.LoadMinHoldSeconds > S.LoadMaxHoldSeconds)
	{
		UE_LOG(LogVoxelUI, Warning,
		       TEXT("-VoxelLoadMinHold=%.1f exceeds -VoxelLoadMaxHold=%.1f; clamping the minimum to the maximum."),
		       S.LoadMinHoldSeconds, S.LoadMaxHoldSeconds);
		S.LoadMinHoldSeconds = S.LoadMaxHoldSeconds;
	}
	return S;
}
} // namespace VoxelFrontEndSwitchesDetail

const FVoxelFrontEndSwitches& FVoxelFrontEndSwitches::Get()
{
	static const FVoxelFrontEndSwitches Switches = VoxelFrontEndSwitchesDetail::Parse();
	return Switches;
}
