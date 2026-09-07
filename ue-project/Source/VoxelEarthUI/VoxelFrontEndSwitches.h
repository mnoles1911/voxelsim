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

	// -VoxelMenuPanel=load|help|credits|settings: open that panel before the
	// capture. Empty means the main column.
	FString MenuPanel;

	// -VoxelPauseShot[=<seconds>]: play for N seconds, open the PAUSE overlay,
	// settle another N, capture with the UI on, quit. Two settles because the
	// overlay is built at the first and its glyphs rasterise during the second.
	//
	// IT NEEDS A WORLD, unlike every switch above it, which is why it is driven
	// by UVoxelPauseUISubsystem rather than by the front end: the front end has
	// stopped ticking by the time there is anything to photograph. Pair it with
	// -VoxelMenuAutoStart (and -VoxelLoadTheatre=0) to reach Playing without a
	// human, and with -VoxelSpawnAt to avoid the origin's missing fine tiles.
	bool bPauseShot = false;
	float PauseShotSeconds = 2.0f;

	// -VoxelPausePanel=pause|settings|save|load: which of the overlay's four
	// screens to open before the shutter. Empty means the pause list.
	FString PausePanel;

	// --- The 2026-09-07 in-game screens ------------------------------------
	// All four follow -VoxelPauseShot exactly: play for N seconds, open the
	// thing, settle another N so its glyphs rasterise, photograph with the UI
	// on, quit. They need a world for the same reason it does, and are driven
	// by UVoxelScreensUISubsystem rather than by the front end, which has
	// stopped ticking by the time there is anything to photograph.
	//
	// PAIR EVERY ONE WITH -VoxelSpawnAt. The world origin has no fine tiles and
	// the spawn gate is fatal there; tools/voxel-ui-capture.ps1 adds it for
	// these shots the same way it does for -Shot Pause.

	// -VoxelScreenShot[=<seconds>]: open the five-tab stack and photograph it.
	bool bScreenShot = false;
	float ScreenShotSeconds = 2.0f;
	// -VoxelScreenPanel=map|journal|inventory|player|codex. Empty means
	// inventory, which is the tab with real data behind it.
	FString ScreenPanel;

	// -VoxelDeathShot[=<seconds>]. CAPTURE-ONLY BY NECESSITY: nothing in this
	// project can kill the player (no health, no damage, no death delegate), so
	// this switch is the death screen's only caller.
	bool bDeathShot = false;
	float DeathShotSeconds = 2.0f;

	// -VoxelDialogueShot[=<seconds>]. Also capture-only: there is no
	// conversation system, and design/CONVERSATION_SYSTEM.md -- which the
	// dialogue CSS names as its spec -- is not in this repository.
	bool bDialogueShot = false;
	float DialogueShotSeconds = 2.0f;

	// -VoxelDemoVitals[=<hp>,<hunger>,<wound>]: draw the HUD's health and hunger
	// bars, and its interaction prompt, at fabricated values.
	//
	// SAME JOB AS -VoxelDemoSaves, AND THE SAME JUSTIFICATION. This project has
	// no health, no hunger and no interaction system, so SVoxelGameHud gates
	// all three off -- a health bar drawn full is a CLAIM, not a decoration, and
	// it would go on telling the player they were unhurt after something could
	// hurt them. But that leaves every picture of the HUD a picture of its
	// empty state, with no way to review the layout the mock was drawn for.
	//
	// So this is capture-only, exactly as the demo save rows are: it fabricates
	// what no system can yet supply, it is off by default, and nothing in
	// ordinary play can turn it on. Values are 0..100 and default to the HUD
	// mock's own TWEAK_DEFAULTS (100 / 100 / 0).
	bool bDemoVitals = false;
	float DemoHealth = 100.f;
	float DemoHunger = 100.f;
	float DemoWound = 0.f;

	// -VoxelHudShot[=<seconds>]: photograph the world with ONLY the HUD on it.
	// Opens nothing, because the HUD is installed as soon as the player has a
	// pawn; the settle is there so the compass tape and the hotbar glyphs have
	// rasterised.
	bool bHudShot = false;
	float HudShotSeconds = 2.0f;

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

	// -VoxelDemoSaves: put a fabricated set of saves in front of the LOAD list
	// and the save dialog's collision check.
	//
	// THE ROW STRUCT WAS DESIGNED FOR THIS -- FVoxelSaveRowInfo's own comment
	// says it is "kept as a plain struct so the widget ... can be
	// screenshot-tested with fabricated rows" -- and this is the switch that
	// finally uses it. A capture box has no saves, so without it every picture
	// of the LOAD dialog is a picture of its empty state: no tags, no filter
	// chips doing anything, no rich rows, and no way to see the overwrite band
	// at all. CAPTURE ONLY: the rows name no world on disk, so LOADING one
	// fails the way loading a deleted save does.
	bool bDemoSaves = false;

	// -VoxelReadyProbeLog: one line per readiness poll, with hit counts,
	// per-ring pending/in-flight, and the poll's own cost in ms.
	bool bReadyProbeLog = false;

	// Loading-gate tuning, so the GateMaxRing measurement is one flag rather
	// than a rebuild. Defaults are the ported Godot contract.
	int32 LoadGateMaxRing = 3;
	// -VoxelLoadGateFineRing=0|1 (default 1): gate 3, "the fine tier's
	// prefetch ring has settled" -- see FVoxelReadyProbeConfig::bRequireFineRing.
	// 0 restores the two-gate rule exactly (the A/B control arm).
	bool bLoadGateFineRing = true;
	// ---- THE MINIMUM HOLD: 15.0 -> 2.0, 2026-08-27 ------------------------
	//
	// THE MINIMUM IS NOT THE THING THAT MAKES THE PLAYER WAIT. The contract in
	// TickLoading is `wait at least MinHold, then leave as soon as the world
	// reports ready`, so what the player actually waits is
	// max(MinHold, timeToReady) + HandOff. Time-to-ready is ~6.1 s (the settle,
	// which is at its own concurrency floor: 34.5 CPU-seconds of worker work
	// over 5.6 effective threads = 6.16 s modelled against 6.10 s observed).
	//
	// So at 15.0 the MINIMUM was binding and readiness was not: the world had
	// been playable for ~9 seconds behind a curtain that would not lift. The
	// player's cold start was 15.4 s against a standing <5 s target, and
	// **none of it was streaming's fault**.
	//
	// WHY IT WAS INVISIBLE. Every leg this project has ever run passes
	// -unattended, which suppresses the front end entirely (`VoxelFrontEnd:
	// suppressed` in every log). So no measurement has ever included the hold,
	// and `settleT` -- the number the <5 s target is tracked against -- both
	// EXCLUDES engine bring-up and never sees this gate at all.
	//
	// WHY 2.0 AND NOT 0. The minimum has a real job, stated at the enforcement
	// site: it stops a warm cache flashing the loading screen for a third of a
	// second, "which reads as a glitch rather than as speed". Two seconds
	// serves that comfortably. Fifteen served it 7.5x over.
	//
	// WHY NOT LOWER THAN THE SETTLE. Below ~6 s readiness binds instead, so
	// dropping this further buys nothing until the settle itself moves --
	// at which point this constant is already out of the way. 15.4 s -> ~7.5 s
	// is the whole prize here and it is entirely in this line.
	//
	// The old value came from `max(60 * 0.25, 5.0)` in the ported
	// TransitionManager and is a port artifact, not a tuned number.
	// -VoxelLoadMinHold= overrides it; set 15 to reproduce every build before
	// this change.
	float LoadMinHoldSeconds = 2.0f;
	// NO LONGER THE READINESS GATE'S CEILING (2026-09-07). Until Phase 4 this
	// one number did two jobs: it was the curtain's maximum hold AND it was
	// passed straight into FVoxelReadyProbeConfig::MaxWaitSeconds. Those are
	// different questions and the owner's directive separates them; the gate's
	// patience is LoadGateMaxWaitSeconds below.
	float LoadMaxHoldSeconds = 60.0f; // the value both menu call sites pass

	// -VoxelLoadGateMaxWait=<s>: how long the readiness probe waits before it
	// gives up and lets the curtain lift on a world that is not ready.
	//
	// 60 -> 300, OWNER DIRECTIVE 2026-09-07: "Happy to have player sit on
	// loading screen for more than a minute if that time is needed to load the
	// tiles and game world in." At 60 s a cold 8-ring cascade took the TIMEOUT
	// path -- the curtain lifted on a world that was still landing, which is
	// the one thing the gate exists to prevent, and the probe's own Warning was
	// the only trace of it.
	//
	// 300 RATHER THAN NO CEILING. A gate with no ceiling is a hang, and the
	// timeout arm has to stay distinguishable from a pass: FVoxelWorldReadyProbe
	// logs a Warning and the front end prints "world NOT ready (gate timed
	// out)", so a run that took it can be told from one that did not.
	float LoadGateMaxWaitSeconds = 300.0f;

	// -VoxelLoadingScreenThread=0|1 (default 1): paint the loading curtain on
	// the engine's Slate loading thread across long world ticks, so a 6.3 s
	// fill frame no longer freezes the hourglass. 0 is the CONTROL ARM -- the
	// curtain stays a plain viewport widget on the game thread, exactly as it
	// was before 2026-09-07, and the seg=LOADING row then reads seconds instead
	// of milliseconds.
	//
	// The mechanism, and the four engine facts that rule out the more obvious
	// versions of it, are in VoxelLoadingCurtainThread.h. Short version: UE 5.8
	// has no persistent loading thread while the game thread ticks a world, so
	// this arms the supported per-blocking-section path around each world tick.
	bool bLoadingScreenThread = true;

	// ---- THE ARTIFICIAL LOAD DURATION (owner directive, 2026-09-05) --------
	//
	// On entering the loading screen the front end rolls a uniform random
	// duration in [Min, Max] seconds and plays the progress bar out against it
	// as THEATRE -- see ComputeTheatreProgress. The reveal is
	// max(timer, world ready), so this range, not LoadMinHoldSeconds, is what
	// ordinarily sets how long a player watches the screen; the minimum hold
	// still matters only when the theatre is overridden shorter than it.
	//
	// -VoxelLoadTheatre=<min>[,<max>] overrides both ends. One value pins the
	// duration exactly; 0 disables the theatre entirely and restores the
	// pre-directive reveal-on-ready timing, which is the arm the unattended
	// -VoxelMenuAutoStart parity legs should pass so a hand-off capture does
	// not grow 30-60 s of curtain time.
	float LoadTheatreMinSeconds = 30.0f;
	float LoadTheatreMaxSeconds = 60.0f;

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
	bool IsCaptureRun() const
	{
		return bMenuShot || bLoadingShot || bHourglassShot || bPauseShot
		    || bScreenShot || bDeathShot || bDialogueShot || bHudShot;
	}
};

namespace VoxelFrontEndSwitches
{
inline const FVoxelFrontEndSwitches& Get() { return FVoxelFrontEndSwitches::Get(); }
}
