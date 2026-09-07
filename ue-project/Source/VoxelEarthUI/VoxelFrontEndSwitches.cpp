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

	// -VoxelPauseShot / -VoxelPauseShot=<s>, both forms like every other
	// capture switch here.
	if (FParse::Value(Cmd, TEXT("VoxelPauseShot="), Seconds))
	{
		S.bPauseShot = true;
		S.PauseShotSeconds = FMath::Max(Seconds, 0.f);
	}
	else if (FParse::Param(Cmd, TEXT("VoxelPauseShot")))
	{
		S.bPauseShot = true;
	}
	FParse::Value(Cmd, TEXT("VoxelPausePanel="), S.PausePanel);
	S.PausePanel = S.PausePanel.TrimStartAndEnd().ToLower();

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

	// --- The 2026-09-07 in-game screens -------------------------------------
	// Both forms of each, on the -VoxelPauseShot pattern above.
	if (FParse::Value(Cmd, TEXT("VoxelScreenShot="), Seconds))
	{
		S.bScreenShot = true;
		S.ScreenShotSeconds = FMath::Max(Seconds, 0.f);
	}
	else if (FParse::Param(Cmd, TEXT("VoxelScreenShot")))
	{
		S.bScreenShot = true;
	}
	FParse::Value(Cmd, TEXT("VoxelScreenPanel="), S.ScreenPanel);
	S.ScreenPanel = S.ScreenPanel.TrimStartAndEnd().ToLower();

	if (FParse::Value(Cmd, TEXT("VoxelDeathShot="), Seconds))
	{
		S.bDeathShot = true;
		S.DeathShotSeconds = FMath::Max(Seconds, 0.f);
	}
	else if (FParse::Param(Cmd, TEXT("VoxelDeathShot")))
	{
		S.bDeathShot = true;
	}

	if (FParse::Value(Cmd, TEXT("VoxelDialogueShot="), Seconds))
	{
		S.bDialogueShot = true;
		S.DialogueShotSeconds = FMath::Max(Seconds, 0.f);
	}
	else if (FParse::Param(Cmd, TEXT("VoxelDialogueShot")))
	{
		S.bDialogueShot = true;
	}

	// THROUGH ParseFloatList, not FParse::Value: the value is a comma-separated
	// list and FParse::Value's default terminator set includes the comma, which
	// has already silently truncated -VoxelSpawnAt and -VoxelTimeOfDay once
	// each. See ParseFloatList's own comment.
	{
		TArray<float> Vitals;
		if (ParseFloatList(TEXT("VoxelDemoVitals="), Vitals) && Vitals.Num() > 0)
		{
			S.bDemoVitals = true;
			S.DemoHealth = FMath::Clamp(Vitals[0], 0.f, 100.f);
			if (Vitals.Num() > 1) { S.DemoHunger = FMath::Clamp(Vitals[1], 0.f, 100.f); }
			if (Vitals.Num() > 2) { S.DemoWound = FMath::Clamp(Vitals[2], 0.f, 100.f); }
		}
		else if (FParse::Param(Cmd, TEXT("VoxelDemoVitals")))
		{
			// Bare form: the HUD mock's own TWEAK_DEFAULTS.
			S.bDemoVitals = true;
		}
	}

	if (FParse::Value(Cmd, TEXT("VoxelHudShot="), Seconds))
	{
		S.bHudShot = true;
		S.HudShotSeconds = FMath::Max(Seconds, 0.f);
	}
	else if (FParse::Param(Cmd, TEXT("VoxelHudShot")))
	{
		S.bHudShot = true;
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

	// -VoxelLoadingShot IMPLIES -VoxelMenuAutoStart. The loading screen only
	// exists after somebody presses NEW GAME, and an unattended capture run has
	// nobody to press it -- without this the switch would photograph the main
	// menu and the watchdog would eventually kill the run, which is a
	// confusing way to learn you needed a second flag.
	if (S.bLoadingShot && !S.bAutoStart)
	{
		S.bAutoStart = true;
		S.AutoStartSeconds = 0.5f;
	}

	// -VoxelPauseShot IMPLIES IT TOO, one step further along: the pause overlay
	// only exists after the world has been handed to the player, so an
	// unattended capture has to press NEW GAME *and* sit through the loading
	// theatre. Without the first, the shot is of a title screen; without the
	// second, of 30-60 s of hourglass.
	if (S.bPauseShot && !S.bAutoStart)
	{
		S.bAutoStart = true;
		S.AutoStartSeconds = 0.5f;
	}

	// The four in-game-screen shots imply it for exactly the same reason: every
	// one of them needs a world with a pawn in it, and an unattended run has
	// nobody to press NEW GAME.
	if ((S.bScreenShot || S.bDeathShot || S.bDialogueShot || S.bHudShot) && !S.bAutoStart)
	{
		S.bAutoStart = true;
		S.AutoStartSeconds = 0.5f;
	}

	S.bNoAssets = FParse::Param(Cmd, TEXT("VoxelUINoAssets"));
	S.bDemoSaves = FParse::Param(Cmd, TEXT("VoxelDemoSaves"));
	S.bReadyProbeLog = FParse::Param(Cmd, TEXT("VoxelReadyProbeLog"));

	FParse::Value(Cmd, TEXT("VoxelLoadGateMaxRing="), S.LoadGateMaxRing);
	{
		int32 FineRingGate = 1;
		FParse::Value(Cmd, TEXT("VoxelLoadGateFineRing="), FineRingGate);
		S.bLoadGateFineRing = (FineRingGate != 0);
	}
	FParse::Value(Cmd, TEXT("VoxelLoadMinHold="), S.LoadMinHoldSeconds);
	FParse::Value(Cmd, TEXT("VoxelLoadMaxHold="), S.LoadMaxHoldSeconds);
	FParse::Value(Cmd, TEXT("VoxelLoadGateMaxWait="), S.LoadGateMaxWaitSeconds);
	FParse::Value(Cmd, TEXT("VoxelMenuWatchdog="), S.MenuWatchdogSeconds);

	// -VoxelLoadingScreenThread=0|1. Int-valued rather than a bare Param
	// because the DEFAULT IS ON, so the useful form is the one that turns it
	// off, and FParse::Param has no "off".
	{
		int32 CurtainThread = 1;
		FParse::Value(Cmd, TEXT("VoxelLoadingScreenThread="), CurtainThread);
		S.bLoadingScreenThread = (CurtainThread != 0);
	}

	// -VoxelLoadTheatre=<min>[,<max>]: the artificial load duration's range.
	// One value pins the duration; 0 disables the theatre (the arm unattended
	// hand-off parity legs should pass). Through ParseFloatList for the same
	// comma-terminator reason as -VoxelLoadingShotAt above.
	{
		TArray<float> Theatre;
		if (ParseFloatList(TEXT("VoxelLoadTheatre="), Theatre) && Theatre.Num() > 0)
		{
			S.LoadTheatreMinSeconds = FMath::Max(Theatre[0], 0.f);
			S.LoadTheatreMaxSeconds = Theatre.Num() > 1 ? FMath::Max(Theatre[1], 0.f)
			                                            : S.LoadTheatreMinSeconds;
		}
		// An inverted range is a typo. FRandRange would quietly interpolate
		// between the two anyway, but a warned clamp says which end wins.
		if (S.LoadTheatreMinSeconds > S.LoadTheatreMaxSeconds)
		{
			UE_LOG(LogVoxelUI, Warning,
			       TEXT("-VoxelLoadTheatre min %.1f exceeds max %.1f; clamping the maximum up."),
			       S.LoadTheatreMinSeconds, S.LoadTheatreMaxSeconds);
			S.LoadTheatreMaxSeconds = S.LoadTheatreMinSeconds;
		}
	}

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
