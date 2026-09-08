#pragma once
// The loading curtain, painted on the engine's Slate LOADING THREAD while the
// game thread is inside a long world tick.
//
// ===========================================================================
// THE OWNER'S REQUIREMENT, AND WHY THE OBVIOUS ANSWER DOES NOT EXIST
// ===========================================================================
//
// Owner, 2026-09-07: "I want the main menu and loading screen to be buttery
// smooth and suffer no hitches. Happy to have player sit on loading screen for
// more than a minute if that time is needed to load the tiles and game world
// in. However, the loading screen should not feel chunky or hitching."
//
// SVoxelLoadingScreen is a viewport widget (VoxelFrontEndSubsystem's
// AddViewportWidgetContent) ticked and painted on the game thread. Every long
// game-thread frame under the curtain freezes it: a fine-tile decode (~300 ms),
// the ring-5 entry recompute (~500 ms), and the raster-atlas fill phase, whose
// FILL frames measure p50 6.3 s. The hourglass simply stops.
//
// THE OBVIOUS FIX -- "put the loading screen on the loading thread and leave it
// there" -- IS NOT AVAILABLE IN UE 5.8, AND THIS IS THE FIRST THING IN THIS
// FILE SO NOBODY SPENDS AN AFTERNOON REDISCOVERING IT. Four independent engine
// facts, each one sufficient on its own:
//
//   1. IsMoviePlayerEnabled() requires !GIsEditor
//      (MoviePlayer.cpp:123). GetMoviePlayer() hands back FNullGameMoviePlayer
//      otherwise (MoviePlayer.cpp:107), whose every method is an empty body.
//      In PIE there is no movie player at all -- so this whole file degrades to
//      nothing there, on purpose, and says so in its log line.
//
//   2. FEngineLoop::Tick OPENS by force-finishing any playing movie and then
//      ensures none is playing:
//         FMoviePlayerProxy::BlockingForceFinished();
//         ensure(!IsMoviePlayerEnabled() || (GetMoviePlayer()->IsLoadingFinished()
//                && !GetMoviePlayer()->IsMovieCurrentlyPlaying()));
//      -- LaunchEngineLoop.cpp:5616-5617. A loading screen cannot span engine
//      frames. It is a WITHIN-FRAME mechanism by the engine's own contract.
//
//   3. bAllowEngineTick DOES NOT GIVE YOU BOTH. It is read only inside
//      WaitForMovieToFinish's loop (DefaultGameMoviePlayer.cpp:527), and the
//      FIRST thing that function does is destroy the Slate loading thread
//      (:452-458). So in bAllowEngineTick mode the world tick and the Slate
//      tick are back on one thread in one loop -- exactly today's topology, and
//      exactly today's freeze.
//
//   4. There is ONE Slate loading thread and it has one owner:
//      "Only one system can use the SlateThread at the same time. GetMoviePlayer
//      is not compatible with PreLoadScreen." (MoviePlayerThreading.cpp:195-199,
//      guarding GSlateLoadingThreadId, which PreLoadSlateThreading.cpp:26 also
//      claims.) And FPreLoadScreenManager is destroyed at
//      LaunchEngineLoop.cpp:5896, before the first engine tick, so it is a
//      startup mechanism and cannot serve a NEW GAME inside a running world.
//
// ===========================================================================
// WHAT IS SUPPORTED, AND IS WHAT THIS FILE DOES
// ===========================================================================
//
// The engine's own answer to "the game thread is about to block for a long
// time; keep the loading screen alive" is the MoviePlayer PROXY, play-on-
// blocking path:
//
//   GetMoviePlayer()->SetIsPlayOnBlockingEnabled(true)   MoviePlayer.h:288
//   FMoviePlayerProxy::BlockingStarted()                 MoviePlayerProxy.h:21
//       -> FDefaultGameMoviePlayer::BlockingStarted      :648  -> PlayMovie()
//       -> FSlateLoadingSynchronizationMechanism         PlayMovie :406
//          spawns "SlateLoadingThread", which ticks and paints the widget at
//          60 Hz (MoviePlayerThreading.cpp:126-176) and hands each draw pass to
//          the render thread, which presents it from
//          FDefaultGameMoviePlayer::Tick (render thread, :520).
//   FMoviePlayerProxy::BlockingFinished()                -> :683
//
// So: ARM THE BLOCK AROUND THE WORLD TICK, one game-thread frame at a time.
// FWorldDelegates::OnWorldPreActorTick (LevelTick.cpp:1675) and
// OnWorldPostActorTick (:1906) bracket FTickableGameObject::TickObjects
// (:1821), which is where UVoxelWorldSubsystem::Tick runs -- and therefore
// where the tile decodes, the ring recompute and the raster-atlas fills
// (VoxelWorldSubsystem.cpp:11504) all live. Nothing Slate-related happens on
// the game thread between those two points, so the loading thread has Slate to
// itself for the whole span. That is the invariant this file rests on; if a
// future change makes the game thread paint inside the world tick, this file
// must be revisited before that change ships.
//
// WIDGET-ONLY. No movie streamer is registered and MoviePaths stays empty, so
// nothing here plays or decodes video (owner, 2026-09-07: "No need for video").
// The module dependency buys the loading THREAD, not a player.
//
// ===========================================================================
// WHY IT IS ARMED PER FRAME AND NOT LEFT ON
// ===========================================================================
//
// Arming is not free: PlayMovie creates a thread and swaps the main window's
// content; BlockingFinished routes to WaitForMovieToFinish, which joins the
// thread and calls FlushRenderingCommands twice
// (DefaultGameMoviePlayer.cpp:565, :590). Paying that on every one of 3,600
// frames of a smooth minute-long load would be absurd, and it would serialise
// the game and render threads while doing it.
//
// So the block is armed only when the frame is expected to be long:
//   * unconditionally for the first kPrimeFrames frames after the curtain goes
//     up -- StartWorldAndPawn's first tick builds the desired set for a whole
//     cascade and is the single most expensive frame of the session, and it is
//     known to be long BEFORE it runs; and
//   * afterwards by hysteresis on the previous frame's own length: one frame
//     over kArmThresholdMs arms the next kCoolDownFrames frames. The chunky
//     phases of a load are runs, not singletons (the FILL window is seconds of
//     multi-second frames), so one long frame is a good predictor of the next.
//     The cost of the arm during such a run is a rounding error against a 6.3 s
//     frame; during a smooth run the arm never happens at all.
//
// ===========================================================================
// IT HUNG A LIVE SESSION ON ITS FIRST DAY. READ THIS BEFORE RE-ENABLING IT.
// ===========================================================================
//
// 2026-09-07, owner's interactive load, default -VoxelLoadingScreenThread=1.
// The process went silent mid-load and never came back: Responding=True, CPU
// flat, 61 threads, loading screen frozen on screen, killed after 14 minutes.
// docs/evidence/2026-09-07-live-curtain-hang-VoxelEarth.log.
//
// THE TIMING IS THE EVIDENCE. The curtain armed its six prime frames at
// 23:27:20.18 and then did not arm again for two minutes -- every frame was
// under the 40 ms threshold. The first two frames in the whole load to cross
// it are 54.63 ms at 23:29:28.826 and 67.22 ms at :28.874. The log's last
// line is :29.063, five frames later. The hang is on the first arm/disarm
// cycle after two minutes of not arming, and there is no other candidate in
// the log: no ensure, no Fatal, no refusal, no summary line.
//
// THE MECHANISM, AND WHY IT CANNOT BE BOUNDED FROM HERE:
//
//   DisarmBlock()
//     -> FMoviePlayerProxy::BlockingFinished()        MoviePlayerProxy.cpp:32
//     -> FDefaultGameMoviePlayer::BlockingFinished()  DefaultGameMoviePlayer.cpp:683
//     -> WaitForMovieToFinish()                       :445
//     -> SyncMechanism->DestroySlateThread()          :452
//     -> while (bMainLoopRunning) { PumpMessages(false); Sleep(0.001f); }
//                                                     MoviePlayerThreading.cpp:75-81
//
// That loop has NO TIMEOUT AND NO CANCEL, and it is entered from inside the
// world tick. It exits only when the Slate thread sets bMainLoopRunning=false
// (MoviePlayerThreading.cpp:179) -- which it cannot do until
// IsSlateDrawPassEnqueued() goes false (:174-177), which happens in EXACTLY
// ONE place: FDefaultGameMoviePlayer::Tick, ON THE RENDER THREAD
// (DefaultGameMoviePlayer.cpp:520-537).
//
// And that render-thread tick is not guaranteed:
//   * in a threaded build the game thread never calls TickRenderingTickables
//     -- LaunchEngineLoop.cpp:5610 guards it with `if (!GUseThreadedRendering)`;
//   * the sole driver is FRenderingThreadTickHeartbeat
//     (RenderingThread.cpp:466-486), which skips the tick entirely while
//     GSuspendRenderingTickables != 0, i.e. during any FlushRenderingCommands
//     (:433-440) -- and WaitForMovieToFinish itself calls FlushRenderingCommands
//     twice;
//   * even when it runs, TickRenderingTickables returns early without ticking
//     anything if less than 1/GRenderingThreadMaxIdleTickFrequency (25 ms) has
//     passed, off a shared function-local static
//     (TickableObjectRenderThread.cpp:34-45).
//
// So the game thread's exit from a disarm depends on a render-thread heartbeat
// the engine itself suppresses during rendering flushes. WE DO NOT OWN THE
// RELEASE CONDITION. Every frame of that load was already render-bound
// (gameWaitMs ~31 ms of a 38 ms frame -- GGameThreadWaitTime, the render fence,
// VoxelWorldSubsystem.cpp:12066), which is exactly the state in which the
// heartbeat is most starved.
//
// WHAT CHANGED AS A RESULT:
//   1. -VoxelLoadingScreenThread DEFAULTS TO 0. Opt-in only, for a leg with
//      somebody watching. A loading screen that can hang is worse than one
//      that stutters.
//   2. bEndRequested: once End() has been asked for, no block may EVER arm
//      again on this instance -- the hand-off's widget move and an arm can no
//      longer race.
//   3. The front end calls End() AT THE REVEAL, before the curtain fade
//      starts, not at the end of teardown -- see VoxelFrontEndSubsystem.cpp.
//   4. The unattended/alt-tab path is refused: no block arms while the
//      application does not have focus. tools/voxel-editor.ps1 sets
//      t.IdleWhenNotForeground, the owner was alt-tabbing, and the gate-sweep
//      leg that validated this never lost focus -- so that state was NEVER
//      exercised before it shipped.
//   5. A hard cap on blocks per load, and a log line on BOTH SIDES of the
//      join, so a future hang names itself in the log instead of being a
//      silent stop.
//
// THE STANDING RULE THIS COST: never call an engine wait with no timeout from
// inside the world tick. If the release condition belongs to another thread
// and the engine gives you no deadline, you do not get to enter it.
//
// ===========================================================================
//
// THE ARM PROVES ITSELF OR TURNS ITSELF OFF. After BlockingStarted the code
// asks IsMovieCurrentlyPlaying() (which is `SyncMechanism != NULL`,
// DefaultGameMoviePlayer.cpp:638 -- i.e. "the loading thread is actually
// running"). If it is false the attempt is counted as a REFUSAL, the widget
// goes straight back into the viewport, and after kMaxRefusals the mechanism
// disables itself for the rest of the load with one Warning naming the count.
// A silent arm that paints nothing is the house failure mode on this project;
// this one cannot be silent, and the counters are printed whether it worked or
// not.

#include "CoreMinimal.h"

class SWidget;
class UGameViewportClient;
class UWorld;

// ---------------------------------------------------------------------------
// The curtain's PAINT CLOCK -- the interval between successive paints of the
// loading screen, whichever thread painted it.
//
// THIS, NOT THE GAME FRAME, IS WHAT THE OWNER'S REQUIREMENT IS ABOUT. The game
// thread's frames under the curtain are allowed to be seconds long -- he said
// so. What must stay under 33 ms is how often the hourglass gets redrawn. Those
// are the same number today (one painter, one thread) and are deliberately
// different numbers once the loading thread is arming, which is exactly why the
// instrument measures the paint and not the frame: seg=LOADING p99 < 33 ms
// while the same log's seg=FILL still shows multi-second frames is the Phase 4
// gate, and with -VoxelLoadingScreenThread=0 the same instrument reads seconds.
// A confirmation that cannot come out the other way is not one.
//
// NotePaint is called from SVoxelLoadingScreen::Tick, which SWidget::Paint
// invokes for any widget with bCanTick (SWidget.cpp, the NeedsTick branch of
// Paint) -- so it fires on the game thread on ordinary frames and on the Slate
// loading thread inside an armed block, with no separate hook for either.
// Intervals are queued and drained on the game thread rather than pushed
// straight into VoxelFramePhase, because that file's buckets and its 5 s flush
// are game-thread-only state and must stay that way.
namespace VoxelLoadingCurtain
{
// Any thread. Records the interval since the previous NotePaint.
VOXELEARTHUI_API void NotePaint();

// Game thread. Forgets the previous timestamp so the next interval is not
// measured across a gap when nothing was on screen.
VOXELEARTHUI_API void ResetPaintClock();

// Game thread. Hands every interval recorded since the last drain to Sink, in
// order, and empties the queue. Sink is called with the lock released.
VOXELEARTHUI_API void DrainPaintIntervals(TFunctionRef<void(double IntervalMs)> Sink);

// Intervals dropped because the queue was full between two drains. A nonzero
// value means the game thread went a very long time without draining and the
// LOADING distribution is missing samples from precisely the worst window --
// read it before quoting a p99.
VOXELEARTHUI_API int64 PaintOverflowCount();

// Every paint counted since the last ResetPaintClock, drained or not.
VOXELEARTHUI_API int64 PaintCount();
} // namespace VoxelLoadingCurtain

// ---------------------------------------------------------------------------
// The block driver. One instance, owned by UVoxelFrontEndSubsystem for the
// lifetime of the loading curtain. Game thread only.
class VOXELEARTHUI_API FVoxelLoadingCurtainThread
{
public:
	FVoxelLoadingCurtainThread() = default;
	~FVoxelLoadingCurtainThread();

	FVoxelLoadingCurtainThread(const FVoxelLoadingCurtainThread&) = delete;
	FVoxelLoadingCurtainThread& operator=(const FVoxelLoadingCurtainThread&) = delete;

	// Why the mechanism cannot run in this process, or an empty string when it
	// can. Answered from the engine's own predicates rather than guessed, so
	// the log line names the real reason (PIE, null RHI, one core, ...).
	static FString UnavailableReason();

	// Starts driving. bRequested is the -VoxelLoadingScreenThread switch,
	// passed in rather than read here so that this translation unit does not
	// depend on the front end's switch struct. Curtain is the SAME widget
	// instance the viewport holds; Viewport and ZOrder are what it is put back
	// into after every block.
	//
	// Returns true if the mechanism is live. False (with a log line) when it is
	// switched off or unavailable, in which case nothing else here does
	// anything and the viewport-widget path is unchanged.
	bool Begin(UWorld* InWorld, UGameViewportClient* InViewport, TSharedRef<SWidget> InCurtain,
	           int32 InZOrder, bool bRequested);

	// Stops driving, disarms any in-flight block, restores the widget to the
	// viewport and prints the engagement counters. Idempotent; called from the
	// curtain teardown and from the destructor.
	void End();

	bool IsActive() const { return bActive; }
	// True only while the Slate loading thread is actually painting for us.
	bool IsBlockArmed() const { return bBlockArmed; }

private:
	void OnPreActorTick(UWorld* TickWorld, ELevelTick TickType, float DeltaSeconds);
	void OnPostActorTick(UWorld* TickWorld, ELevelTick TickType, float DeltaSeconds);

	void ArmBlock();
	void DisarmBlock();

	// --- Tuning, deliberately constants and not switches --------------------
	// One switch (-VoxelLoadingScreenThread) is the A/B; three more knobs would
	// be three more things to classify and none of them is a question anybody
	// has asked. They are named here so a reader can find them.
	//
	// 40 ms: a frame over ~2.5 display frames at 60 Hz is already a visible
	// stutter on the curtain, and it is well clear of the ordinary 8-13 ms
	// loading frame so the arm does not fire on noise.
	static constexpr double kArmThresholdMs = 40.0;
	// ~0.5 s of cover after one long frame.
	static constexpr int32 kCoolDownFrames = 30;
	// The first frames after the curtain goes up: BeginLoad's two paint frames
	// and then StartWorldAndPawn's cascade frame, plus margin.
	static constexpr int32 kPrimeFrames = 6;
	// Consecutive failures to actually start the loading thread before the
	// mechanism gives up for this load.
	static constexpr int32 kMaxRefusals = 3;
	// A HARD CEILING ON HOW MANY TIMES THIS MAY ENTER THE ENGINE'S UNBOUNDED
	// JOIN IN ONE LOAD. Every arm is one throw of the dice described at the top
	// of this file; a pathological load must not throw it thousands of times.
	// 240 is ~4 s of continuously chunky frames, well past the point where the
	// evidence would show whether it is helping.
	static constexpr int32 kMaxBlocksPerLoad = 240;
	// A disarm that takes longer than this is the hang, caught early. Logged as
	// a Warning AFTER the fact -- the game thread is inside the engine's loop
	// while it happens and nothing of ours can run -- and it disables the
	// mechanism for the rest of the load.
	static constexpr double kDisarmWarnMs = 250.0;

	bool bActive = false;
	bool bBlockArmed = false;
	bool bDisabledByRefusals = false;
	// Set by End() BEFORE it does anything else. A one-way latch: no block may
	// arm after it, so the hand-off's widget move cannot race an arm.
	bool bEndRequested = false;
	// Set when a disarm took longer than kDisarmWarnMs, i.e. the engine's join
	// is going slowly. Stops arming for the rest of this load.
	bool bDisabledBySlowDisarm = false;

	UWorld* World = nullptr;
	UGameViewportClient* Viewport = nullptr;
	TSharedPtr<SWidget> Curtain;
	int32 ZOrder = 0;

	FDelegateHandle PreTickHandle;
	FDelegateHandle PostTickHandle;

	double LastPreTickSeconds = 0.0;
	double BlockStartedSeconds = 0.0;
	int32 FramesSeen = 0;
	int32 CoolDownRemaining = 0;
	int32 ConsecutiveRefusals = 0;

	// --- Engagement counters, all printed by End() --------------------------
	int64 BlocksArmed = 0;
	int64 BlockRefusals = 0;
	// Arms declined before touching the movie player at all. Printed beside
	// blocks= so "it never fired" and "it was never allowed to fire" are
	// different readings.
	int64 FocusRefusals = 0;
	int64 CapRefusals = 0;
	double LongestDisarmMs = 0.0;
	double CoveredSeconds = 0.0;
	double LongestBlockMs = 0.0;
	double LongestUncoveredFrameMs = 0.0;
};
