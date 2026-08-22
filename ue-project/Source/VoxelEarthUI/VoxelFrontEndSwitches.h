#pragma once
// Every new -Voxel* switch the front end adds, parsed once, in one place.
//
// WHY ONE STRUCT AND NOT FParse CALLS AT THE USE SITES. The gameplay module's
// forty-odd verification switches are parsed where they are used, which is
// idiomatic and, at this point, means nobody can list them without grepping.
// tools/lint-frontend-switch-coverage.py has to be able to see every switch
// this module adds in order to classify it against VoxelFrontEndPolicy's
// self-driving rule, so the front end's are collected here instead. It also
// means the resolved values appear in one log line at boot, which is what
// makes a capture that came out wrong diagnosable from its own log.
//
// Every one of these follows the -VoxelOverlayShot precedent: capture WITH the
// UI on screen, then quit. -VoxelScreenshotAfter cannot serve, because it
// captures with bShowUI=false and would photograph an empty world behind the
// menu.

#include "CoreMinimal.h"

struct VOXELEARTHUI_API FVoxelFrontEndSwitches
{
	// -VoxelMenuShot[=<seconds>]: settle N seconds on the main menu, capture
	// with UI, quit. Default 2 s -- long enough for the background decode to
	// land, short enough that a capture sweep is not a coffee break.
	bool bMenuShot = false;
	float MenuShotSeconds = 2.0f;

	// -VoxelMenuPanel=load|help|credits: open that panel before the capture.
	// Empty means the main column.
	FString MenuPanel;

	// -VoxelLoadingShot[=<seconds>] / -VoxelLoadingShotAt=<s,s,s>: press NEW
	// GAME immediately, then capture at each offset. The default single offset
	// is 6 s, which on a cold cascade puts the bar mid-fill with the sand
	// mound formed and grains falling -- an empty hourglass proves nothing.
	bool bLoadingShot = false;
	TArray<float> LoadingShotSeconds;

	// -VoxelHourglassShot=<p>[,<p>...]: draw ONLY the hourglass, at each fixed
	// progress, on a flat field. Isolated from everything else because it is
	// the densest drawing in the front end and the most likely to need
	// iteration; a strip at 0/0.25/0.5/0.75/1.0 is one comparable image.
	bool bHourglassShot = false;
	TArray<float> HourglassProgress;

	// -VoxelMenuAutoStart[=<seconds>]: press NEW GAME after N seconds and then
	// get out of the way -- no capture, no quit. THE COMPATIBILITY SWITCH:
	// pairing it with -VoxelFrontEnd=1 lets any existing -Voxel* capture run
	// through the front end and prove the hand-off lands in the same world the
	// archive photographs.
	bool bAutoStart = false;
	float AutoStartSeconds = 0.5f;

	// -VoxelUINoAssets: pretend the font and background art are missing. Makes
	// the degraded path screenshot-testable instead of theoretical.
	bool bNoAssets = false;

	// -VoxelReadyProbeLog: one line per readiness poll, with hit counts,
	// per-ring pending/in-flight, and the poll's own cost in ms.
	bool bReadyProbeLog = false;

	// Loading-gate tuning, so the GateMaxRing measurement is one flag rather
	// than a rebuild. Defaults are the ported Godot contract.
	int32 LoadGateMaxRing = 3;
	float LoadMinHoldSeconds = 15.0f; // max(60 * 0.25, 5.0) from TransitionManager
	float LoadMaxHoldSeconds = 60.0f; // the value both menu call sites pass

	// -VoxelMenuWatchdog=<seconds>: under -unattended, refuse to sit on the
	// menu past N seconds and exit with an error. Same shape as
	// -VoxelPerfExitWatchdog, and for the same reason: a mis-flagged headless
	// run must not hang a machine until somebody notices.
	float MenuWatchdogSeconds = 300.0f;

	// Parsed once on first call.
	static const FVoxelFrontEndSwitches& Get();

	// True when any switch here drives the run itself. Each capture path arms
	// its own quit, so nothing consults this yet; it is the predicate a future
	// caller wanting "is this a capture run at all" should use rather than
	// re-deriving the disjunction.
	bool IsCaptureRun() const { return bMenuShot || bLoadingShot || bHourglassShot; }
};

namespace VoxelFrontEndSwitches
{
inline const FVoxelFrontEndSwitches& Get() { return FVoxelFrontEndSwitches::Get(); }
}
