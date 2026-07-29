#include "VoxelSkyLadderFixture.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "UnrealClient.h" // FScreenshotRequest
#include "VoxelDebug.h"   // FVoxelPerfSnapshot -- the streaming read-back the preflight polls
#include "VoxelEarth.h"
#include "VoxelSkySubsystem.h"
#include "VoxelWorldSubsystem.h"

// ===========================================================================
// -VoxelSkyLadder=<N> -- N captures across one simulated day, one process
// ===========================================================================
//
// The header carries the argument for why this exists and why it must not be a
// shell loop. This file is the mechanism, and there are five ideas in it worth
// reading before the code:
//
// 1. THE CLOCK IS SET, NEVER ADVANCED. voxel.Sky.TimeScale is pinned to 0 at arm
//    time and every rung writes an absolute epoch through
//    UVoxelSkySubsystem::SetEpochSeconds. See ComputeRungEpochSeconds for the
//    one arithmetic subtlety (the game-day index is LATCHED, not re-solved).
//
// 2. THE LOG STATES WHAT THE FRAME USED, NOT WHAT IT ASKED FOR. Every rung reads
//    the sun altitude, azimuth, intensity, exposure and resolved hour back out of
//    UVoxelSkySubsystem::GetSkyState() -- after the settle, immediately before
//    the shutter -- and names the FILE with the read-back hour, so a filename can
//    never disagree with the frame behind it. VoxelGpuVerify.cpp:2118-2126 states
//    the rule: the log has to say what the run actually used or it is not
//    evidence. VoxelSkySubsystem.cpp:628-636 already applies it to this same
//    clock at startup; this is the same rule at capture time.
//
// 3. THE CAMERA IS LATCHED AND RE-ASSERTED. The entire value of the ladder is
//    that consecutive rungs differ ONLY in the light. A pawn that settles a few
//    centimetres down a slope over the minute the ladder takes would add parallax
//    to every comparison, and parallax and a lighting change look nothing alike
//    in a diff but everything alike to the eye. The pose is captured once, at the
//    first rung, and re-asserted before every shutter -- the same thing
//    VoxelSweBreachFixture::PoseCamera does for the same reason.
//
// 4. IT REFUSES TO ARM RATHER THAN MISLABEL. Three preconditions are checked by
//    READ-BACK at arm time (clock frozen, sky enabled, no competing capture
//    fixture) and each failure aborts the run with an Error naming the fix. The
//    precedent is VoxelGpuVerify.cpp:2104-2110 refusing to schedule an A/B
//    against a misspelt cvar: "This would have run an A/B against itself."
//
// 5. IT ALWAYS DIES. Watchdog armed before anything that can stall, one Finish()
//    that every path funnels through, timers cleared before the exit is armed.
//    -VoxelWaterParityTest shipped without a self-quit once and left an editor
//    running for 2h40m.

namespace
{
// --- Tunables, all overridable and all logged ------------------------------

// Rungs. 8 is the bare-flag default because 24/8 = 3 h lands exactly on
// midnight, sunrise-ish, noon and sunset-ish at any latitude, and eight frames
// still fit on one screen for review. Any N is legal.
constexpr int32 kDefaultSteps = 8;

// An upper bound, for two reasons that are both about the run rather than about
// taste: the watchdog is sized from N (see StartFromCommandLine), and every rung
// is a PNG on disk. 96 rungs is a 15-minute capture at a quarter-hour of
// simulated resolution, which is past the point where anyone reviews the ladder
// frame by frame.
constexpr int32 kMaxSteps = 96;

// SETTLE BEFORE EACH SHUTTER, AND WHY IT IS SECONDS RATHER THAN FRAMES.
//
// The sun does not merely rotate between rungs -- it TELEPORTS, by up to three
// hours of arc, and three separate temporally-amortised systems have to catch up
// before the frame is of the new sky rather than a blend of two:
//
//   (a) THE SKY LIGHT. UVoxelSkySubsystem::SpawnRig sets
//       SetRealTimeCaptureEnabled(true), and UE's real-time capture amortises the
//       cubemap capture and its convolution across several frames. This is the
//       binding constraint, and it is exactly the thing that makes the ambient
//       term follow the sky down at dusk -- so a short settle does not produce a
//       subtly wrong frame, it produces a frame lit by the PREVIOUS rung.
//   (b) THE SKY ATMOSPHERE's own transmittance/scattering LUTs, which are
//       likewise refreshed against the light directions over a few frames.
//   (c) EYE ADAPTATION, if voxel.Sky.ExposureMode is 1 (clamped auto). Mode 2,
//       the default, is AEM_Manual and adapts instantly -- but the ladder must
//       not be correct only in the default mode, since comparing the modes is one
//       of the things it is for.
//
// 4 s is ~240 frames at 60 fps and ~120 at this project's heavier 2K numbers,
// which is an order of magnitude more than (a) and (b) need and covers UE's
// default ~1 s eye-adaptation time constant several times over for (c). It is
// deliberately generous: the cost is 4 s x N of wall clock in a headless leg
// nobody is watching, and the failure it buys off is a stale-sky frame that
// looks entirely plausible and is wrong.
constexpr float kDefaultSettleSeconds = 4.f;
constexpr float kMinSettleSeconds = 0.5f;
constexpr float kMaxSettleSeconds = 60.f;

// HOLD AFTER THE SHUTTER, BEFORE THE NEXT CLOCK JUMP.
//
// FScreenshotRequest::RequestScreenshot only RAISES A FLAG; the viewport services
// it at the end of a subsequent draw. Jumping the clock in the next tick would
// therefore race the shutter and could write rung i's filename over rung i+1's
// lighting -- a mislabelled frame, which is the one failure mode this fixture
// exists to prevent. 1.5 s is many frames of margin for a request that is
// normally serviced in one.
constexpr float kPostCaptureHoldSeconds = 1.5f;

// --- Preflight -------------------------------------------------------------
//
// THE WORLD MUST BE STREAMED IN BEFORE RUNG 0 OR THE LADDER PHOTOGRAPHS HOLES,
// and this project has already paid for that lesson once: an unstreamed frame
// and a night frame are BOTH mostly black, so "the terrain did not load" and
// "the sky works" are indistinguishable in exactly the artifact this fixture
// produces. (The same collision is why VoxelSkySubsystem.cpp:254-266 refuses to
// default the clock to midnight.)
//
// WHAT WAS CHOSEN AND WHY IT IS NOT THE PERF HARNESS'S 90 s. -VoxelPerfRun uses a
// FIXED 90 s preflight because a perf measurement needs the machine in a
// STEADY STATE, not merely a populated one -- a fixed wait is part of its
// protocol. A ladder only needs the chunks to be there, and "are they there" has
// a direct read-back: UVoxelWorldSubsystem::GetPerfSnapshot's queue depths.
// So: a short fixed settle, then POLL to quiet, capped at 90 s -- the perf
// harness's number kept as the CEILING rather than as the wait. A fast machine
// starts the ladder in ~25 s; a slow one gets the full 90 and says so in the log
// instead of silently photographing a partial world.
//
// The preflight is paid ONCE for the whole ladder, not per rung, because the
// camera never moves and a frozen clock streams nothing new -- which is another
// thing N separate processes would have got wrong, N times over.
constexpr float kDefaultPreflightSeconds = 20.f;
constexpr float kPollIntervalSeconds = 2.f;
constexpr int32 kMaxTerrainPolls = 45; // 90 s ceiling, the perf harness's number

// --- Run state -------------------------------------------------------------
//
// ON THE HEAP, owned by the timer delegates. See VoxelSkyLadderFixture.h for the
// argument; the short form is that N is a RUNTIME value, so "one FTimerHandle
// member per stage on the game mode" cannot express this sequence at all.
struct FSkyLadderRun
{
	TWeakObjectPtr<UWorld> World;

	// --- resolved switches ---
	int32 Steps = kDefaultSteps;
	float SettleSeconds = kDefaultSettleSeconds;
	float PreflightSeconds = kDefaultPreflightSeconds;
	double StartHour = 0.0;
	float WatchdogSeconds = 0.f;

	// --- latched at the first rung ---
	//
	// THE GAME-DAY INDEX, LATCHED ONCE. Not re-read per rung: it is derived from
	// the epoch, and the epoch is what each rung overwrites, so re-deriving it
	// would let the ladder walk. See ComputeRungEpochSeconds.
	bool bLatched = false;
	double DayLengthSeconds = 1200.0;
	double BaseDayIndex = 0.0;
	double BaseEpochSeconds = 0.0;

	// The pose the whole ladder is shot from, latched at the same moment.
	bool bPoseLatched = false;
	FVector PawnLocation = FVector::ZeroVector;
	FRotator PawnRotation = FRotator::ZeroRotator;
	FRotator ControlRotation = FRotator::ZeroRotator;

	// --- progress ---
	int32 StepIndex = 0;
	int32 TerrainPolls = 0;
	int32 CapturesTaken = 0;

	// --- the result table, for the closing summary --------------------------
	//
	// Kept per rung rather than only streamed to the log so the final line can
	// present the whole cycle in one block. A reviewer reading a 90 s log wants
	// the altitude sweep in one place to see that it is monotone up then down;
	// scattered across N lines fifteen seconds apart it is not readable as a
	// curve.
	struct FRung
	{
		int32 Index = 0;
		double RequestedHours = 0.0;
		double ResolvedHours = 0.0;
		int32 DayOfYear = 0;
		double SunAltitudeDeg = 0.0;
		double SunAzimuthDeg = 0.0;
		double MoonAltitudeDeg = 0.0;
		double MoonIlluminatedFraction = 0.0;
		float SunIntensity = 0.f;
		float ExposureBiasEV = 0.f;
		int32 ExposureMode = 0;
		FString ShotName;
	};
	TArray<FRung> Rungs;

	bool bFinishing = false;

	FTimerHandle StartHandle;
	FTimerHandle TerrainPollHandle;
	FTimerHandle StageHandle;
	FTimerHandle WatchdogHandle;
	FTimerHandle QuitHandle;
};

using FRunRef = TSharedRef<FSkyLadderRun, ESPMode::ThreadSafe>;

// Stage forward declarations -- the sequence, in order.
void StageBegin(FRunRef Run);
void StageTerrainPoll(FRunRef Run);
void StageSetClock(FRunRef Run);
void StageCapture(FRunRef Run);
void StageAdvance(FRunRef Run);
void StageSummary(FRunRef Run);
void Finish(FRunRef Run, const TCHAR* Reason);

FORCEINLINE UWorld* WorldOf(const FRunRef& Run)
{
	return Run->World.Get();
}

// Timer helpers, verbatim in shape from VoxelSweBreachFixture.cpp:392-410. The
// Run ref is captured BY VALUE into the lambda, which is what keeps the run
// state alive exactly as long as some timer can still reach a stage.
void SetTimerOnce(FRunRef Run, FTimerHandle& Handle, float Delay, TFunction<void(FRunRef)> Fn)
{
	UWorld* W = WorldOf(Run);
	if (!W)
	{
		return;
	}
	W->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([Run, Fn]() { Fn(Run); }), FMath::Max(Delay, 0.01f),
	                              false);
}

void SetTimerLooping(FRunRef Run, FTimerHandle& Handle, float Interval, TFunction<void(FRunRef)> Fn)
{
	UWorld* W = WorldOf(Run);
	if (!W)
	{
		return;
	}
	W->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([Run, Fn]() { Fn(Run); }),
	                              FMath::Max(Interval, 0.01f), true);
}

// --- Terminal path ---------------------------------------------------------
//
// EVERY exit goes through here, including the watchdog and every "subsystem
// missing" bail. Clears the whole timer set FIRST so no stage can re-enter after
// the final line, then arms the one timer that ends the process.
//
// The 5 s before RequestExit is not a round number picked for comfort: the
// screenshot write is ASYNC (the -VoxelScreenshotAfter block at
// VoxelEarthGameMode.cpp:1200-1203 waits ~3 s for exactly this), and quitting
// on top of a pending write is how a ladder loses its last rung.
void Finish(FRunRef Run, const TCHAR* Reason)
{
	if (Run->bFinishing)
	{
		return;
	}
	Run->bFinishing = true;

	UE_LOG(LogVoxelEarth, Log, TEXT("VoxelSkyLadder: FINISHED (%s) after %d/%d capture(s). Quitting in 5s."), Reason,
	       Run->CapturesTaken, Run->Steps);

	UWorld* W = WorldOf(Run);
	if (!W)
	{
		FPlatformMisc::RequestExit(/*bForce*/ false);
		return;
	}
	FTimerManager& TM = W->GetTimerManager();
	TM.ClearTimer(Run->StartHandle);
	TM.ClearTimer(Run->TerrainPollHandle);
	TM.ClearTimer(Run->StageHandle);
	TM.ClearTimer(Run->WatchdogHandle);
	TM.SetTimer(Run->QuitHandle, FTimerDelegate::CreateLambda([]() { FPlatformMisc::RequestExit(/*bForce*/ false); }), 5.f,
	            false);
}

// --- Pose ------------------------------------------------------------------

void LatchPose(FRunRef Run)
{
	UWorld* W = WorldOf(Run);
	APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
	APawn* P = PC ? PC->GetPawn() : nullptr;
	if (!P)
	{
		// Not fatal. A run with no pawn still has a view (the ladder composes with
		// whatever framing switch aimed it), it simply cannot be re-asserted -- and
		// saying so is better than silently not doing it.
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelSkyLadder: no pawn to latch a pose from; rungs will be shot from wherever the camera ")
		       TEXT("happens to be. If anything moves the view mid-ladder the rungs stop being comparable."));
		return;
	}
	Run->PawnLocation = P->GetActorLocation();
	Run->PawnRotation = P->GetActorRotation();
	Run->ControlRotation = PC->GetControlRotation();
	Run->bPoseLatched = true;
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSkyLadder: pose LATCHED at (%.0f,%.0f,%.0f) rot=(%.1f,%.1f,%.1f). Re-asserted before every ")
	       TEXT("shutter so consecutive rungs differ only in the light."),
	       Run->PawnLocation.X, Run->PawnLocation.Y, Run->PawnLocation.Z, Run->PawnRotation.Pitch,
	       Run->PawnRotation.Yaw, Run->PawnRotation.Roll);
}

void ReassertPose(FRunRef Run)
{
	if (!Run->bPoseLatched)
	{
		return;
	}
	UWorld* W = WorldOf(Run);
	APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
	APawn* P = PC ? PC->GetPawn() : nullptr;
	if (!P)
	{
		return;
	}
	P->SetActorLocation(Run->PawnLocation, false, nullptr, ETeleportType::TeleportPhysics);
	P->SetActorRotation(Run->PawnRotation);
	PC->SetViewTarget(P);
	PC->SetControlRotation(Run->ControlRotation);
}

// --- Clock arithmetic ------------------------------------------------------

// The absolute epoch, in game seconds, that rung i is shot at.
//
// WHY THIS IS NOT UVoxelSkySubsystem::SetTimeOfDay, WHICH IS THE OBVIOUS CALL.
//
// SetTimeOfDay(h) re-solves the epoch through SolveEpochFor against the CURRENT
// day of the year (VoxelSkySubsystem.cpp:1345-1357), and the current day of the
// year is itself a function of the day FRACTION -- which is the one thing every
// rung changes. Concretely: DayOfYear = floor(((K + f) / DaysPerYear) *
// 365.2425), so walking f from 0 to 1 can move the reported day of the year, and
// the next SetTimeOfDay would then solve against that moved date and may pick a
// different whole-game-day index K. The ladder would drift its own season, one
// rung at a time, for reasons that have nothing to do with the time of day it is
// supposed to be isolating.
//
// So the game-day index K is LATCHED ONCE and only the fraction moves:
//
//     Epoch(i) = DayLength * (K + frac(StartHour/24 + i/N))
//
// which is exactly "one game day, sampled N times" and nothing else. Keeping K
// an integer is also what makes frac(Epoch / DayLength) come out EXACTLY equal
// to the requested day fraction -- the same property SolveEpochFor's
// integer-K search is built around (VoxelSkySubsystem.cpp:344-352) -- so the
// resolved hour a rung reads back matches the hour it asked for to the bit.
//
// WHAT THIS STILL CANNOT HOLD FIXED, stated plainly because it will show up in
// the frames: the SEASON. At the shipped voxel.Sky.DaysPerYear=48 one game day
// spans 365.2425/48 = 7.6 REAL days of solar declination, so the sun's arc is
// measurably different at the top of the ladder and the bottom of it. That is
// the compressed calendar working as designed (VoxelEphemeris.h:130-138 and the
// DaysPerYear cvar help both call it out as the point of a compressed year, not
// a wrapping bug), not something this fixture can or should correct -- so every
// rung LOGS its day of the year and the arm-time line states the total drift.
double ComputeRungEpochSeconds(const FRunRef& Run, int32 StepIndex)
{
	const double N = FMath::Max(1.0, (double)Run->Steps);
	const double Fraction = FMath::Frac(Run->StartHour / 24.0 + (double)StepIndex / N);
	const double Wrapped = Fraction < 0.0 ? Fraction + 1.0 : Fraction;
	return Run->DayLengthSeconds * (Run->BaseDayIndex + Wrapped);
}

// Hours -> (HH, MM), rounded to the nearest minute WITH the carry handled.
//
// THIS IS A LABEL, AND THE LABEL HAS TO SURVIVE THE ARITHMETIC. Rung i's epoch
// is DayLength * (K + i/N) and the hour comes back as frac(Epoch / DayLength) *
// 24 -- a multiply and a divide, so for any N that does not divide a power of
// two the exact value comes back a few ulps LOW. Flooring, which is what
// VoxelSkySubsystem.cpp:649-650 does for its own log line, then renders an exact
// 08:00 as "07h59". In a log line that is a harmless off-by-a-nanosecond; in a
// FILENAME it reads as a fixture bug and, worse, it makes the ladder's rungs
// look unevenly spaced when they are exactly even.
//
// So: round to nearest, and carry properly rather than clamping the carry away.
// Clamping 60 to 59 (which is what "so it can never read 23h60" would buy) turns
// midnight into "23h59", which is the same lie in a different place. The
// unrounded value is in the log line beside the name to three decimals of an
// hour, so nothing is lost -- this rounds a LABEL by at most 30 simulated
// seconds, it does not round the clock.
void SplitHours(double Hours, int32& OutHour, int32& OutMinute)
{
	const double Wrapped = FMath::Fmod(FMath::Fmod(Hours, 24.0) + 24.0, 24.0);
	int32 TotalMinutes = (int32)FMath::RoundToDouble(Wrapped * 60.0);
	// 23:59:31..23:59:59 rounds to 1440, which is 24:00 and therefore 00:00.
	TotalMinutes = ((TotalMinutes % 1440) + 1440) % 1440;
	OutHour = TotalMinutes / 60;
	OutMinute = TotalMinutes % 60;
}

// ===========================================================================
// STAGES
// ===========================================================================

// Stage 1: resolve the sky subsystem, then wait for the world to stream in.
void StageBegin(FRunRef Run)
{
	UWorld* W = WorldOf(Run);
	UVoxelSkySubsystem* Sky = W ? W->GetSubsystem<UVoxelSkySubsystem>() : nullptr;
	UVoxelWorldSubsystem* Terrain = W ? W->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Sky || !Terrain)
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("VoxelSkyLadder: sky=%d terrain=%d -- one of the subsystems this needs is absent, so there is ")
		       TEXT("nothing to photograph a day of."),
		       Sky ? 1 : 0, Terrain ? 1 : 0);
		Finish(Run, TEXT("subsystems missing"));
		return;
	}

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSkyLadder: preflight done (%.1fs); polling the streamer to quiet, at most %d x %.1fs = %.0fs."),
	       Run->PreflightSeconds, kMaxTerrainPolls, kPollIntervalSeconds, kMaxTerrainPolls * kPollIntervalSeconds);
	Run->TerrainPolls = 0;
	SetTimerLooping(Run, Run->TerrainPollHandle, kPollIntervalSeconds, &StageTerrainPoll);
}

// Stage 2: poll until the streamer is quiet, then start the ladder.
//
// Same read-back and same field set as VoxelSweBreachFixture::StageTerrainPoll,
// deliberately: two fixtures that mean different things by "the world is ready"
// are two different worlds.
void StageTerrainPoll(FRunRef Run)
{
	UWorld* W = WorldOf(Run);
	UVoxelWorldSubsystem* Terrain = W ? W->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Terrain)
	{
		Finish(Run, TEXT("terrain subsystem lost"));
		return;
	}
	++Run->TerrainPolls;

	const FVoxelPerfSnapshot Snap = Terrain->GetPerfSnapshot();
	const bool bQuiet = Snap.JobsInFlight == 0 && Snap.PendingJobQueueDepth == 0 &&
	                    Snap.PendingGameThreadQueueDepth == 0 && Snap.PendingUnloadQueueDepth == 0 &&
	                    Snap.ChunksLoadedPerSec <= 0.f;

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSkyLadder terrain-poll %d/%d: inFlight=%d pendingJob=%d pendingGT=%d pendingUnload=%d ")
	       TEXT("loaded/s=%.1f -> %s"),
	       Run->TerrainPolls, kMaxTerrainPolls, Snap.JobsInFlight, Snap.PendingJobQueueDepth,
	       Snap.PendingGameThreadQueueDepth, Snap.PendingUnloadQueueDepth, Snap.ChunksLoadedPerSec,
	       bQuiet ? TEXT("QUIET") : TEXT("still streaming"));

	if (!bQuiet && Run->TerrainPolls < kMaxTerrainPolls)
	{
		return;
	}
	W->GetTimerManager().ClearTimer(Run->TerrainPollHandle);
	if (!bQuiet)
	{
		// Loud, and specific about what it does to the ARTIFACT rather than to the
		// run: a partially streamed ladder is mostly-black frames that look exactly
		// like the night rungs working correctly.
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelSkyLadder: the streamer did NOT go quiet within %.0fs; shooting anyway. Holes in the frames ")
		       TEXT("will read as night rungs -- check the poll line above before concluding anything about the sky."),
		       kMaxTerrainPolls * kPollIntervalSeconds);
	}
	SetTimerOnce(Run, Run->StageHandle, 0.5f, &StageSetClock);
}

// Stage 3 (per rung): latch on the first pass, set the clock ABSOLUTELY, settle.
void StageSetClock(FRunRef Run)
{
	UWorld* W = WorldOf(Run);
	UVoxelSkySubsystem* Sky = W ? W->GetSubsystem<UVoxelSkySubsystem>() : nullptr;
	if (!Sky)
	{
		Finish(Run, TEXT("sky subsystem lost"));
		return;
	}

	if (!Run->bLatched)
	{
		const FVoxelSkyState& S = Sky->GetSkyState();

		// HAS THE SUBSYSTEM ACTUALLY TICKED? GetSkyState returns a ZEROED state
		// before the first tick and whenever Impl is null (VoxelSkySubsystem.cpp:1321-1328),
		// and a zeroed state is indistinguishable from a legitimate "midnight on
		// 1 January" epoch of 0. LightUpdates is the discriminator: it is
		// incremented on the first tick that drives the rig and never reset, so
		// zero here means the clock has never run -- which would silently discard
		// whatever -VoxelDate resolved to and walk day 0 instead.
		if (S.LightUpdates <= 0)
		{
			UE_LOG(LogVoxelEarth, Error,
			       TEXT("VoxelSkyLadder: UVoxelSkySubsystem has never driven the rig (LightUpdates=0) after %.0fs, so ")
			       TEXT("its state is zeroed and the base date cannot be read. Most likely there is no ")
			       TEXT("UVoxelWorldSubsystem, so the sky subsystem left its impl null (VoxelSkySubsystem.cpp:521-528). ")
			       TEXT("Refusing to shoot a ladder whose date would be a fabrication."),
			       Run->PreflightSeconds);
			Finish(Run, TEXT("sky clock never ticked"));
			return;
		}

		Run->DayLengthSeconds = VoxelSky::GetDayLengthSeconds();
		Run->BaseEpochSeconds = S.EpochSeconds;
		Run->BaseDayIndex = FMath::FloorToDouble(S.EpochSeconds / Run->DayLengthSeconds);
		Run->bLatched = true;

		const double DaysPerYear = VoxelSky::GetDaysPerYear();
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelSkyLadder: LATCHED game-day index %.0f from epoch %.3f s (dayLength=%.1f s, the clock stood ")
		       TEXT("at %05.2f h on day-of-year %d). Every rung sets DayLength*(K + frac) with K held, so only the ")
		       TEXT("hour moves. SEASONAL DRIFT ACROSS THE LADDER: %.1f real days of solar declination at ")
		       TEXT("DaysPerYear=%.1f -- inherent to the compressed calendar, not a fixture bug."),
		       Run->BaseDayIndex, Run->BaseEpochSeconds, Run->DayLengthSeconds, S.LocalHours, S.DayOfYear,
		       365.2425 / FMath::Max(1.0, DaysPerYear), DaysPerYear);

		LatchPose(Run);
	}

	// THE ABSOLUTE SET. Never "advance the clock by 24/N hours": voxel.Sky.TimeScale
	// is 0 precisely so that the epoch is a value this fixture owns rather than a
	// quantity that also moves on its own between here and the shutter.
	const double Epoch = ComputeRungEpochSeconds(Run, Run->StepIndex);
	Sky->SetEpochSeconds(Epoch);

	const double RequestedHours = FMath::Frac(Epoch / Run->DayLengthSeconds) * 24.0;
	int32 ReqH = 0, ReqM = 0;
	SplitHours(RequestedHours, ReqH, ReqM);
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSkyLadder rung %d/%d: clock SET to epoch %.3f s (requested %02dh%02d); settling %.1fs before ")
	       TEXT("the shutter (SkyLight real-time capture has to reconverge after a %0.1f-hour jump)."),
	       Run->StepIndex, Run->Steps - 1, Epoch, ReqH, ReqM, Run->SettleSeconds, 24.0 / FMath::Max(1.0, (double)Run->Steps));

	SetTimerOnce(Run, Run->StageHandle, Run->SettleSeconds, &StageCapture);
}

// Stage 4 (per rung): read the state BACK, name the file from it, shoot.
void StageCapture(FRunRef Run)
{
	UWorld* W = WorldOf(Run);
	UVoxelSkySubsystem* Sky = W ? W->GetSubsystem<UVoxelSkySubsystem>() : nullptr;
	if (!Sky)
	{
		Finish(Run, TEXT("sky subsystem lost"));
		return;
	}

	ReassertPose(Run);

	// EVERYTHING BELOW IS READ BACK, NOTHING IS ECHOED. The altitude and azimuth
	// are the ones the rig was actually posed at, the hour is the one the clock
	// actually resolved to, and the FILENAME is built from the read-back hour --
	// so a frame whose label disagrees with its content is not expressible here.
	// VoxelGpuVerify.cpp:2118-2126.
	const FVoxelSkyState& S = Sky->GetSkyState();

	const double RequestedEpoch = ComputeRungEpochSeconds(Run, Run->StepIndex);
	const double RequestedHours = FMath::Frac(RequestedEpoch / Run->DayLengthSeconds) * 24.0;

	// A drift guard, because "the clock did not take" has no other symptom. If the
	// epoch that came back is not the epoch that went in, something is still
	// advancing it -- which is the exact failure the frozen time scale exists to
	// prevent, and it would put every rung at an hour its filename denies.
	const double EpochError = S.EpochSeconds - RequestedEpoch;
	if (FMath::Abs(EpochError) > 0.5)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelSkyLadder rung %d: the clock MOVED %+.3f s between the set and the shutter (asked %.3f, read ")
		       TEXT("%.3f). voxel.Sky.TimeScale is %.3f. Every filename in this ladder is now approximate; trust the ")
		       TEXT("logged read-back, not the name."),
		       Run->StepIndex, EpochError, RequestedEpoch, S.EpochSeconds, VoxelSky::GetTimeScale());
	}

	int32 Hour = 0, Minute = 0;
	SplitHours(S.LocalHours, Hour, Minute);

	// %02d index first so the ladder sorts in ladder order in a directory listing,
	// then the hour so each frame states what time it is without opening a log.
	const FString ShotName = FString::Printf(TEXT("VoxelSkyLadder_%02d_%02dh%02d"), Run->StepIndex, Hour, Minute);

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSkyLadderShot n=%d/%d name=%s requestedHours=%06.3f resolvedHours=%06.3f (%02dh%02d) ")
	       TEXT("dayOfYear=%d epoch=%.3f lat=%.4f lon=%.4f sunAltDeg=%+.3f sunAzDeg=%.3f sunUp=%d sunIntensity=%.4f ")
	       TEXT("sunTempK=%.0f moonAltDeg=%+.3f moonIllum=%.3f moonIntensity=%.4f exposureMode=%d exposureBiasEV=%+.3f ")
	       TEXT("lightUpdates=%lld"),
	       Run->StepIndex, Run->Steps - 1, *ShotName, RequestedHours, S.LocalHours, Hour, Minute, S.DayOfYear,
	       S.EpochSeconds, S.LatitudeDeg, S.LongitudeDeg, S.SunAltitudeDeg, S.SunAzimuthDeg, S.bSunUp ? 1 : 0,
	       S.SunIntensity, S.SunTemperatureK, S.MoonAltitudeDeg, S.MoonIlluminatedFraction, S.MoonIntensity,
	       S.ExposureMode, S.ExposureBiasEV, (long long)S.LightUpdates);

	FSkyLadderRun::FRung Rung;
	Rung.Index = Run->StepIndex;
	Rung.RequestedHours = RequestedHours;
	Rung.ResolvedHours = S.LocalHours;
	Rung.DayOfYear = S.DayOfYear;
	Rung.SunAltitudeDeg = S.SunAltitudeDeg;
	Rung.SunAzimuthDeg = S.SunAzimuthDeg;
	Rung.MoonAltitudeDeg = S.MoonAltitudeDeg;
	Rung.MoonIlluminatedFraction = S.MoonIlluminatedFraction;
	Rung.SunIntensity = S.SunIntensity;
	Rung.ExposureBiasEV = S.ExposureBiasEV;
	Rung.ExposureMode = S.ExposureMode;
	Rung.ShotName = ShotName;
	Run->Rungs.Add(MoveTemp(Rung));

	// FScreenshotRequest, NOT HighResShot. HighResShot re-renders through a tiled
	// path with its own screen-percentage and post-process behaviour, so its output
	// is not comparable to the rest of this project's capture archive -- and
	// comparability across frames is the entire product here. Same call and same
	// arguments as every other fixture (VoxelSweBreachFixture.cpp:1381-1386):
	// bShowUI=false, bAddFilenameSuffix=true. Lands in
	// ue-project/Saved/Screenshots/WindowsEditor/.
	FScreenshotRequest::RequestScreenshot(*ShotName, /*bShowUI*/ false, /*bAddFilenameSuffix*/ true);
	++Run->CapturesTaken;

	SetTimerOnce(Run, Run->StageHandle, kPostCaptureHoldSeconds, &StageAdvance);
}

// Stage 5 (per rung): advance, or fall through to the summary.
void StageAdvance(FRunRef Run)
{
	++Run->StepIndex;
	if (Run->StepIndex < Run->Steps)
	{
		StageSetClock(Run);
		return;
	}
	StageSummary(Run);
}

// Stage 6: the whole cycle in one block, then the exit.
void StageSummary(FRunRef Run)
{
	// The curve, in one place. Scattered across N log lines separated by fifteen
	// seconds of streaming chatter, an altitude sweep is not readable AS a sweep --
	// and "does the altitude rise then fall, and is the exposure monotone against
	// it" is the thing the reviewer is actually checking.
	FString Table;
	for (const FSkyLadderRun::FRung& R : Run->Rungs)
	{
		int32 H = 0, M = 0;
		SplitHours(R.ResolvedHours, H, M);
		Table += FString::Printf(
			TEXT("\n  %02d  %02dh%02d  doy=%3d  sunAlt=%+7.2f  sunAz=%7.2f  sunI=%6.3f  moonAlt=%+7.2f  moonIllum=%.2f  ")
			TEXT("expEV=%+6.2f (mode %d)  %s"),
			R.Index, H, M, R.DayOfYear, R.SunAltitudeDeg, R.SunAzimuthDeg, R.SunIntensity, R.MoonAltitudeDeg,
			R.MoonIlluminatedFraction, R.ExposureBiasEV, R.ExposureMode, *R.ShotName);
	}

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSkyLadder SUMMARY: %d rung(s) across one simulated day from %05.2f h, all in ONE process (the ")
	       TEXT("cross-session screenshot floor is 1.81%% and the within-session floor is 0.00%%, so only same-process ")
	       TEXT("rungs are comparable -- VoxelGpuVerify.cpp:2074-2084). Every row below is READ BACK from ")
	       TEXT("GetSkyState() at the shutter, not requested.%s"),
	       Run->Rungs.Num(), Run->StartHour, *Table);

	// The one thing a reviewer should check before believing any of it, stated
	// where they will read it. VoxelSkySubsystem.cpp's exposure anchors are
	// explicitly unmeasured guesses that this leg exists to calibrate.
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSkyLadder: read the rungs against ExposureBiasForSunAltitude's anchors ")
	       TEXT("(VoxelSkySubsystem.cpp:428-461) -- those numbers are UNMEASURED first guesses and this ladder is the ")
	       TEXT("leg that corrects them. Screenshots are in Saved/Screenshots/WindowsEditor/ as VoxelSkyLadder_*."));

	Finish(Run, TEXT("ladder complete"));
}

// --- Arm-time cvar pinning -------------------------------------------------

// Force a cvar owned by another file and REPORT WHAT IT ACTUALLY BECAME.
//
// Drives the cvar object through IConsoleManager rather than adding a setter,
// which is the pattern VoxelEarthHUD.cpp:279-287 uses for voxel.GI.Enabled: the
// cvar stays the one shared source of truth instead of acquiring a parallel flag.
//
// ECVF_SetByCode is a LOW priority and can be REFUSED outright -- anything set
// via -ExecCmds arrives at ECVF_SetByConsole and outranks it (VoxelSkySubsystem.cpp:145-149
// spells this out for SetTimeScale). Which is exactly why this returns the value
// it read back rather than the value it wrote.
bool PinCVar(const TCHAR* Name, const TCHAR* Value, FString& OutActual)
{
	IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
	if (!Var)
	{
		OutActual = TEXT("<missing>");
		return false;
	}
	Var->Set(Value, ECVF_SetByCode);
	OutActual = Var->GetString();
	UE_LOG(LogVoxelEarth, Log, TEXT("VoxelSkyLadder: %s <- %s, read back as %s."), Name, Value, *OutActual);
	return true;
}

} // namespace

bool VoxelSkyLadderFixture::StartFromCommandLine(UWorld* World)
{
	// The Value-or-Param idiom every switch in this module uses
	// (VoxelEarthGameMode.cpp:2147-2150): bare -VoxelSkyLadder takes the default,
	// -VoxelSkyLadder=<N> overrides it. FParse::Param requires a delimiter after
	// the token, so -VoxelSkyLadderSettle= cannot accidentally satisfy it.
	int32 Steps = kDefaultSteps;
	if (!FParse::Value(FCommandLine::Get(), TEXT("VoxelSkyLadder="), Steps) &&
	    !FParse::Param(FCommandLine::Get(), TEXT("VoxelSkyLadder")))
	{
		return false;
	}
	if (!World)
	{
		return false;
	}

	FRunRef Run = MakeShared<FSkyLadderRun, ESPMode::ThreadSafe>();
	Run->World = World;
	Run->Steps = FMath::Clamp(Steps, 1, kMaxSteps);

	float Settle = kDefaultSettleSeconds;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelSkyLadderSettle="), Settle))
	{
		Run->SettleSeconds = FMath::Clamp(Settle, kMinSettleSeconds, kMaxSettleSeconds);
	}
	float Preflight = kDefaultPreflightSeconds;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelSkyLadderPreflight="), Preflight))
	{
		Run->PreflightSeconds = FMath::Clamp(Preflight, 0.5f, 600.f);
	}
	float StartHour = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelSkyLadderStartHour="), StartHour))
	{
		Run->StartHour = FMath::Fmod(FMath::Fmod((double)StartHour, 24.0) + 24.0, 24.0);
	}

	// --- Precondition 1: nothing else may be driving a capture ---------------
	//
	// -VoxelScreenshotAfter does not merely take an extra picture: it captures and
	// then QUITS ~3 s later (VoxelEarthGameMode.cpp:1200-1203). Combined with this
	// fixture it would kill the process partway up the ladder, leaving however many
	// rungs had been shot -- a truncated artifact that looks like a completed one
	// unless someone counts the files.
	//
	// The other fixtures handle the collision by DEFERRING their capture to the
	// shared framing chain (the bCaveTestSelfCapture idiom,
	// VoxelEarthGameMode.cpp:2662-2672). That option does not exist here: this
	// fixture's whole product is N captures at N different clock settings, and the
	// shared chain takes exactly one, at whatever the clock happened to be. So the
	// only honest answers are "refuse" or "silently truncate", and this project's
	// rule for that choice is already written down at VoxelGpuVerify.cpp:2104-2110.
	{
		float Throwaway = 0.f;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelScreenshotAfter="), Throwaway) ||
		    FParse::Param(FCommandLine::Get(), TEXT("VoxelScreenshotAfter")))
		{
			UE_LOG(LogVoxelEarth, Error,
			       TEXT("VoxelSkyLadder: -VoxelScreenshotAfter is also present. That switch captures once and then quits ")
			       TEXT("the process, which would truncate this ladder partway up and leave an artifact that looks ")
			       TEXT("complete. This fixture cannot defer to it either -- it owns N captures at N clock settings and ")
			       TEXT("the shared framing chain takes exactly one. NOT ARMING; drop -VoxelScreenshotAfter and re-run."));
			return false;
		}
	}

	// --- Precondition 2: the clock must be frozen, by READ-BACK --------------
	FString ActualTimeScale, ActualEnabled;
	const bool bHaveTimeScale = PinCVar(TEXT("voxel.Sky.TimeScale"), TEXT("0"), ActualTimeScale);
	const bool bHaveEnabled = PinCVar(TEXT("voxel.Sky.Enabled"), TEXT("1"), ActualEnabled);
	if (!bHaveTimeScale || !bHaveEnabled)
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("VoxelSkyLadder: voxel.Sky.TimeScale=%s voxel.Sky.Enabled=%s -- a sky cvar is missing, so this is ")
		       TEXT("not a build with UVoxelSkySubsystem in it. NOT ARMING."),
		       *ActualTimeScale, *ActualEnabled);
		return false;
	}
	if (FMath::Abs(VoxelSky::GetTimeScale()) > KINDA_SMALL_NUMBER)
	{
		// The pin was refused. ECVF_SetByCode loses to ECVF_SetByConsole and
		// ECVF_SetByCommandLine, so the usual cause is the same value being set from
		// -ExecCmds. A running clock drifts between the settle and the shutter --
		// at the shipped 1200 s day and a 4 s settle that is ~4.8 simulated minutes
		// per rung, so every filename would be a small lie and the ladder would
		// still look completely plausible.
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("VoxelSkyLadder: asked for voxel.Sky.TimeScale 0 and read back %.3f -- the ECVF_SetByCode write was ")
		       TEXT("REFUSED, almost certainly by an -ExecCmds/-VoxelTimeScale set at a higher priority. The clock would ")
		       TEXT("drift between each settle and each shutter and every filename would be wrong by minutes. NOT ")
		       TEXT("ARMING; remove the competing set."),
		       VoxelSky::GetTimeScale());
		return false;
	}

	// --- Precondition 3: the rig must be driven by the clock -----------------
	if (!VoxelSky::IsEnabled())
	{
		// With voxel.Sky.Enabled 0 the rig is frozen at the pre-W4 static pose
		// (VoxelSkySubsystem.cpp:952-958) and the clock drives nothing -- so all N
		// rungs would be pixel-identical frames with N different hours in their
		// filenames. That is the single most misleading artifact this fixture could
		// produce.
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("VoxelSkyLadder: asked for voxel.Sky.Enabled 1 and read back %s. With the clock off the rig is ")
		       TEXT("frozen at the static pose, so all %d rungs would be identical frames with different hours in their ")
		       TEXT("names. NOT ARMING."),
		       *ActualEnabled, Run->Steps);
		return false;
	}

	// --- Watchdog sizing -----------------------------------------------------
	//
	// Computed from N rather than a constant, because N is a runtime value and a
	// fixed watchdog would either kill a long ladder or leave a short one holding
	// the process for minutes after it finished. Nominal = preflight + the full
	// poll ceiling + N x (settle + hold) + the closing 5 s exit, doubled and given
	// 30 s of slack -- the same "worst case the stages can actually reach, roughly
	// doubled" sizing VoxelSweBreachFixture.cpp:131-137 uses.
	const float NominalSeconds = Run->PreflightSeconds + kMaxTerrainPolls * kPollIntervalSeconds +
	                             (float)Run->Steps * (Run->SettleSeconds + kPostCaptureHoldSeconds) + 10.f;
	Run->WatchdogSeconds = NominalSeconds * 2.f + 30.f;

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSkyLadder: ARMED. %d rung(s) every %.2f simulated hours from %05.2f h, settle %.1fs, preflight ")
	       TEXT("%.1fs. voxel.Sky.TimeScale=%s voxel.Sky.Enabled=%s (both read back, not echoed). Nominal run %.0fs; ")
	       TEXT("the watchdog quits this process unconditionally %.0fs from now. Captures -> ")
	       TEXT("Saved/Screenshots/WindowsEditor/VoxelSkyLadder_*.png"),
	       Run->Steps, 24.0 / (double)Run->Steps, Run->StartHour, Run->SettleSeconds, Run->PreflightSeconds,
	       *ActualTimeScale, *ActualEnabled, NominalSeconds, Run->WatchdogSeconds);

	// THE WATCHDOG GOES ON FIRST, before anything that could stall
	// (VoxelSweBreachFixture.cpp:1638-1641). Note it is armed even though every
	// stage below already funnels into Finish(): the stages can only cover the
	// failures they anticipated, and this is the one that covers the rest.
	World->GetTimerManager().SetTimer(
		Run->WatchdogHandle, FTimerDelegate::CreateLambda([Run]() { Finish(Run, TEXT("WATCHDOG -- a stage stalled")); }),
		Run->WatchdogSeconds, false);

	SetTimerOnce(Run, Run->StartHandle, Run->PreflightSeconds, &StageBegin);
	return true;
}
