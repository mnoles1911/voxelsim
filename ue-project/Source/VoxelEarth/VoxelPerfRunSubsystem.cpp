#include "VoxelPerfRunSubsystem.h"

#include "VoxelDebug.h"
#include "VoxelWorldSubsystem.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace
{
// ---------------------------------------------------------------------------
// THE EXIT WATCHDOG (2026-07-29).
//
// FinishRun asks the engine to exit and then has no further say: bFinished
// makes IsTickable() false from that frame on, so this subsystem cannot even
// observe whether the exit happened. Twice now it did not -- the harness
// reported "ok (290s)" and returned, and UnrealEditor-Cmd stayed alive
// indefinitely at ~4.5 GB, idle, until the NEXT leg died on the one-editor
// guard and the wedge was attributed to the wrong run.
//
// The known-suspect list for that wedge is in
// FVoxelWorldImpl::WaitForInFlightTasks, and the bounded wait added there
// addresses the top of it. This is the backstop for the rest, and it exists
// because of WHERE those wedges live: inside FEngineLoop::Exit. By then the
// game thread is the thing that is stuck, and neither FTSTicker (only pumped
// from FEngineLoop::Tick) nor a UObject tick nor an FCoreDelegate can fire.
// Only a thread that is not the game thread can act, so this is a plain
// FRunnable that sleeps and then terminates the process.
//
// SCOPE, deliberately narrow: armed only when -VoxelPerfRun= asked for a
// scripted headless run, only once the run has ALREADY written its JSON and
// requested exit, and it does nothing at all if exit completes first (the
// process is gone). It is not a general "kill the editor" facility and must
// never become one -- an interactive editor that hangs should be debugged,
// not shot.
//
// bForce=true is TerminateProcess on Windows. That is the point: it is the
// only thing that beats a game thread blocked on a task event, and a leg whose
// results JSON is already on disk has nothing left to lose by dying hard. The
// log line before it is what tells the next reader this happened -- an exit
// code from a terminated process is otherwise indistinguishable from a crash.
// ---------------------------------------------------------------------------
class FVoxelExitWatchdog final : public FRunnable
{
public:
	explicit FVoxelExitWatchdog(double InTimeoutSec) : TimeoutSec(InTimeoutSec) {}

	uint32 Run() override
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSec;
		while (FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(0.25f);
		}
		UE_LOG(LogVoxelPerf, Error,
		       TEXT("VoxelPerfRun exit watchdog: the process did not exit within %.0fs of RequestExit. Terminating. ")
		       TEXT("The run's JSON was already written, so its numbers are valid -- but SHUTDOWN IS BROKEN: read the ")
		       TEXT("last WaitForInFlightTasks line in this log to see whether worker tasks failed to drain."),
		       TimeoutSec);
		if (GLog)
		{
			GLog->Flush(); // the message above is the whole value of this thread; do not lose it to buffering
		}
		FPlatformMisc::RequestExit(/*bForce*/ true);
		return 0;
	}

private:
	double TimeoutSec;
};

// Deliberately leaked, both of them: the process is either exiting normally
// (in which case joining a sleeping thread would only slow that down, and is
// precisely the class of shutdown join this whole mechanism exists to
// survive) or being terminated by the watchdog itself.
void ArmExitWatchdog(double TimeoutSec)
{
	static bool bArmed = false;
	if (bArmed || TimeoutSec <= 0.0)
	{
		return;
	}
	bArmed = true;
	FVoxelExitWatchdog* Watchdog = new FVoxelExitWatchdog(TimeoutSec);
	FRunnableThread::Create(Watchdog, TEXT("VoxelPerfExitWatchdog"), 0, TPri_Normal);
}
} // namespace

void UVoxelPerfRunSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// docs/debug-tooling-plan.md P1 "-VoxelPerfRun=<seconds> harness". Absent
	// (or non-positive) => this subsystem stays completely inert (IsTickable
	// returns false below), matching the zero-cost-when-off constraint.
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfRun="), DurationSeconds) && DurationSeconds > 0.f)
	{
		bRequested = true;

		// UVoxelWorldSubsystem only populates FVoxelPerfSnapshot (chunks
		// loaded, budget saturation, ...) while voxel.Debug >= 1 (constraint:
		// "keep all debug work zero-cost when voxel.Debug=0"). A perf run's
		// entire purpose is to collect that data, so force mode >=1 here
		// rather than silently reporting zeros for chunksLoaded/budgetSat in
		// the JSON summary.
		if (VoxelDebug::GetDebugMode() < 1)
		{
			VoxelDebug::SetDebugMode(1);
		}

		// -VoxelPerfFlight=<surface|underground>. A command-line switch, not a
		// cvar, for the same reason the sky-band and buried-skip switches are:
		// the path shape decides where the anchor is on the FIRST tick, and an
		// -ExecCmds cvar lands after streaming has already begun building a
		// desired set around the surface spawn.
		FString FlightName;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfFlight="), FlightName))
		{
			if (FlightName.Equals(TEXT("underground"), ESearchCase::IgnoreCase))
			{
				Flight = EVoxelPerfFlight::Underground;
			}
			else if (FlightName.Equals(TEXT("static"), ESearchCase::IgnoreCase))
			{
				Flight = EVoxelPerfFlight::Static;
				// Overridable so the pose can be aimed at a dense view rather
				// than whatever the spawn happens to face -- the measurement is
				// only as good as the amount of world in frame.
				FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfYaw="), StaticYawDeg);
				FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfPitch="), StaticPitchDeg);

				// -VoxelPerfStaticAt=X,Y,Z in UU (world), optional.
				//
				// Without it, static mode pins wherever the pawn spawned, and the
				// spawn is always a SURFACE column: RestartPlayer grounds the pawn
				// on the highest solid voxel with air above it
				// (VoxelEarthGameMode.cpp), and -VoxelCameraHigh only moves it
				// further UP (it rejects values <= 0). So there was no way to pin
				// this fixture inside a cave or on a cavern lake shore, which is
				// exactly the scene the water A/B needs -- the only voxel water in
				// the default world is underground, and the fixtures that DO go
				// there (-VoxelFloodTest, -VoxelCavernShot) move the camera on
				// their own timers, which static mode then overwrites every tick.
				//
				// UU rather than metres deliberately: the pose this is meant to
				// reproduce is the one the flood-test fixture prints, and that line
				// is in UU. Copying it verbatim is the point.
				// bShouldStopOnSeparator=false for the reason
				// VoxelEarthGameMode.cpp's ParseSpawnColumnUU spells out for
				// -VoxelSpawnAt=X,Y: FParse::Value's default terminator set
				// includes ',', so it truncates "X,Y,Z" at the first comma and
				// hands back "X". This switch's value is the whole triple, so
				// read to the next whitespace instead.
				//
				// Reproduced here before the flag was added -- the first run
				// aborted with "-VoxelPerfStaticAt=42030 is not X,Y,Z in UU",
				// which is precisely what the abort-rather-than-fall-back
				// branch below exists for: without it the fixture would have
				// pinned at the surface spawn and produced a perfectly
				// plausible cavern-lake measurement of the wrong scene.
				FString StaticAt;
				if (FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfStaticAt="), StaticAt,
				                  /*bShouldStopOnSeparator=*/false))
				{
					TArray<FString> Parts;
					StaticAt.ParseIntoArray(Parts, TEXT(","), /*InCullEmpty*/ true);
					if (Parts.Num() == 3)
					{
						StaticLocationOverrideUU = FVector(FCString::Atod(*Parts[0]),
						                                   FCString::Atod(*Parts[1]),
						                                   FCString::Atod(*Parts[2]));
						bStaticLocationOverride = true;
					}
					else
					{
						// Same reasoning as the unknown-flight-name branch below:
						// silently falling back to the spawn pose would produce a
						// plausible JSON summary for a scene nobody asked for.
						UE_LOG(LogVoxelPerf, Error,
						       TEXT("VoxelPerfRun: -VoxelPerfStaticAt=%s is not X,Y,Z in UU. Aborting run."),
						       *StaticAt);
						bRequested = false;
						return;
					}
				}
			}
			else if (FlightName.Equals(TEXT("line"), ESearchCase::IgnoreCase))
			{
				Flight = EVoxelPerfFlight::Line;
				// -VoxelPerfHeading=<degrees>, 0 = +X, 90 = +Y. Parsed the same
				// shape as -VoxelPerfSpeed below (parse into a local, only
				// overwrite the member on success) but WITHOUT a positivity
				// gate: unlike a speed override, 0 deg and negative degrees are
				// both perfectly meaningful headings, not a "no override"
				// sentinel, so there is nothing to reject a malformed-but-parsed
				// value against. A missing/unparsable switch simply leaves the
				// +X default in place, same as -VoxelPerfYaw/-VoxelPerfPitch
				// above.
				float HeadingDegValue = 0.f;
				if (FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfHeading="), HeadingDegValue))
				{
					HeadingDeg = HeadingDegValue;
				}
			}
			else if (!FlightName.Equals(TEXT("surface"), ESearchCase::IgnoreCase))
			{
				// Refuse rather than silently flying the default path: a typo'd
				// flight name that quietly ran the surface circle would produce
				// a perfectly plausible JSON summary labelled as the run you
				// thought you asked for.
				UE_LOG(LogVoxelPerf, Error,
				       TEXT("VoxelPerfRun: unknown -VoxelPerfFlight=%s (expected 'surface', 'underground', 'static' or 'line'). Aborting run."),
				       *FlightName);
				bRequested = false;
				return;
			}
		}

		float DepthM = float(DefaultDepthM);
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfDepth="), DepthM) && DepthM > 0.f)
		{
			DepthUU = double(DepthM) * 100.0;
		}

		float SpeedMPerSec = 0.f;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfSpeed="), SpeedMPerSec) && SpeedMPerSec > 0.f)
		{
			LinearSpeedUUPerSecOverride = double(SpeedMPerSec) * 100.0;
		}

		// -VoxelPerfPreflightSec=<seconds>. Same guarded-scalar shape as
		// -VoxelPerfSpeed/-VoxelPerfDepth above -- 0 or a malformed value both
		// mean "no preflight", which is already the member's default, so
		// there is nothing to abort over. See PreflightSec's doc comment for
		// what this switch is for and its frame-metrics caveat.
		float PreflightSecValue = 0.f;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfPreflightSec="), PreflightSecValue) && PreflightSecValue > 0.f)
		{
			PreflightSec = PreflightSecValue;
		}

		// -VoxelPerfLingerSec=<seconds>. Same guarded-scalar shape as
		// -VoxelPerfSpeed/-VoxelPerfDepth above (parse into a local, only
		// overwrite the member if present AND positive) -- 0 or a malformed
		// value both mean "no linger", which is already the member's default,
		// so there is nothing to abort over. See LingerSec's doc comment for
		// what this switch is for and its frame-metrics caveat.
		float LingerSecValue = 0.f;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfLingerSec="), LingerSecValue) && LingerSecValue > 0.f)
		{
			LingerSec = LingerSecValue;
		}

		// Four-way, not the two-way it used to be: with only
		// underground-vs-"everything else" the static flight silently logged
		// itself as "surface" here (though never in the FinishRun JSON, which
		// already got this right) -- fixed in passing while adding line's own
		// label rather than perpetuating the same gap for a third mode.
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("VoxelPerfRun: scripted %.1fs flight requested (flight=%s, depth=%.0fm, speed=%.0fm/s, ")
		       TEXT("preflight=%.1fs, linger=%.1fs)."),
		       DurationSeconds,
		       Flight == EVoxelPerfFlight::Underground ? TEXT("underground")
		           : Flight == EVoxelPerfFlight::Static ? TEXT("static")
		           : Flight == EVoxelPerfFlight::Line   ? TEXT("line")
		                                                 : TEXT("surface"),
		       DepthUU / 100.0, LinearSpeedUUPerSecOverride / 100.0, PreflightSec, LingerSec);
	}
}

bool UVoxelPerfRunSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Same scope as UVoxelWorldSubsystem: game worlds only, never the editor.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::GamePreview;
}

bool UVoxelPerfRunSubsystem::IsTickable() const
{
	return bRequested && !bFinished;
}

TStatId UVoxelPerfRunSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelPerfRunSubsystem, STATGROUP_Tickables);
}

void UVoxelPerfRunSubsystem::Tick(float DeltaTime)
{
	if (!bRequested || bFinished)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (!bPathInitialized)
	{
		// Lazily wait for the pawn to exist (GameMode::RestartPlayer may not
		// have run yet on the very first tick) -- same pattern
		// UVoxelWorldSubsystem::Tick uses for its streaming anchor.
		APlayerController* PC = World->GetFirstPlayerController();
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (!Pawn)
		{
			return; // try again next tick; this run's clock hasn't started yet
		}

		CircleCenterUU = Pawn->GetActorLocation();
		if (UVoxelWorldSubsystem* Subsystem = World->GetSubsystem<UVoxelWorldSubsystem>())
		{
			FixedHeightUU = Subsystem->GetSurfaceHeightUU(CircleCenterUU.X, CircleCenterUU.Y) + HeightAboveSurfaceUU;
		}
		else
		{
			FixedHeightUU = CircleCenterUU.Z;
		}
		// Line flight's ground-following Z (StepFlightPath) is rate-limited
		// relative to the LAST placed Z, so it needs a frame-1 seed that
		// already matches "surface at the origin + clearance" -- otherwise
		// the very first tick would ramp up from 0 instead of starting level.
		LineLastZUU = FixedHeightUU;

		// Preflight's pinned pose, captured HERE -- the same frame, and from
		// the same Pawn, as CircleCenterUU above -- rather than lazily inside
		// StepFlightPath's preflight branch. That is deliberate: it is what
		// guarantees nothing ever re-captures a pose when the flight begins,
		// so a preflight-then-line run still starts its traverse from the
		// true spawn rather than from wherever the pawn happened to be sitting
		// at the moment preflight ended.
		PreflightLocationUU = CircleCenterUU;
		PreflightRotation = Pawn->GetActorRotation();

		bPathInitialized = true;
		UE_LOG(LogVoxelPerf, Log, TEXT("VoxelPerfRun: path centered at (%.0f, %.0f), height %.0f UU."),
		       CircleCenterUU.X, CircleCenterUU.Y, FixedHeightUU);
	}

	StepFlightPath(DeltaTime);

	// Per-frame sampling (docs/debug-tooling-plan.md P1: "per-frame samples
	// of frame ms").
	const float FrameMs = DeltaTime * 1000.f;
	FrameMsSamples.Add(FrameMs);
	if (FrameMs > HitchThresholdMs)
	{
		++HitchCount;
	}

	// Post-warmup window (see WarmupExcludeSeconds's doc comment): gated on
	// ElapsedSeconds as of the START of this frame (before it's advanced at
	// the bottom of this function) -- a one-frame fuzziness on a 10s cutoff
	// is immaterial.
	if (ElapsedSeconds >= WarmupExcludeSeconds)
	{
		PostWarmupFrameMsSamples.Add(FrameMs);
		if (FrameMs > HitchThresholdMs)
		{
			++PostWarmupHitchCount;
		}
	}

	if (UVoxelWorldSubsystem* Subsystem = World->GetSubsystem<UVoxelWorldSubsystem>())
	{
		BudgetSaturationAccum += Subsystem->GetPerfSnapshot().BudgetSaturationPct;
		++BudgetSaturationSamples;
	}

	ElapsedSeconds += DeltaTime;
	// Bracketed by PreflightSec and LingerSec (both default 0, i.e. no change
	// from before): the flight only advances for ElapsedSeconds in
	// [PreflightSec, PreflightSec + DurationSeconds) (see StepFlightPath's
	// preflight- and linger-pin branches), but FinishRun/RequestExit wait
	// until PreflightSec + DurationSeconds + LingerSec, so a warmup-then-fly-
	// then-stop leg gets both a pre-motion cascade-warm window and a
	// post-motion observation window instead of the process exiting the
	// instant the flight itself starts or stops.
	if (ElapsedSeconds >= PreflightSec + DurationSeconds + LingerSec)
	{
		FinishRun();
	}
}

void UVoxelPerfRunSubsystem::StepFlightPath(float DeltaTime)
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return;
	}

	if (PreflightSec > 0.f && ElapsedSeconds < PreflightSec)
	{
		// Cold-cascade warmup window, before the flight path clock starts at
		// all -- hold at the pose captured in Tick's path-init block
		// (PreflightLocationUU/PreflightRotation, set once, the SAME frame as
		// CircleCenterUU/FixedHeightUU -- not re-captured here) instead of
		// advancing any flight path, so the first streaming observation isn't
		// confounded with plain cold-fill. See PreflightSec's doc comment.
		// Checked ahead of the per-flight branches below (and ahead of the
		// linger gate, since the two windows are time-disjoint by
		// construction) so it applies uniformly regardless of which flight
		// was requested.
		if (!bPreflightLogged)
		{
			bPreflightLogged = true;
			UE_LOG(LogVoxelPerf, Log,
			       TEXT("VoxelPerfRun: preflight warmup for %.1fs at spawn pose (%.0f, %.0f, %.0f) yaw=%.1f -- ")
			       TEXT("world ticking/streaming, flight path clock starts at zero once this ends."),
			       PreflightSec, PreflightLocationUU.X, PreflightLocationUU.Y, PreflightLocationUU.Z,
			       PreflightRotation.Yaw);
		}

		Pawn->SetActorLocationAndRotation(PreflightLocationUU, PreflightRotation,
		                                  /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
		if (PC)
		{
			PC->SetControlRotation(PreflightRotation);
		}
		// Deliberately NOT folded into TotalPathFrames/UndergroundFrames, same
		// reasoning as the linger window below: these counters describe the
		// FLOWN path, and a parked warmup window isn't part of it.
		return;
	}

	if (LingerSec > 0.f && ElapsedSeconds >= PreflightSec + DurationSeconds)
	{
		// Flight has ended (t >= PreflightSec + DurationSeconds); hold this
		// pose for the configured linger window instead of advancing further,
		// so the world keeps streaming (and logging) around a STOPPED pawn --
		// see LingerSec's doc comment. Checked ahead of the per-flight
		// branches below so it applies uniformly regardless of which flight
		// was running; for EVoxelPerfFlight::Static this is a no-op in effect
		// (already pinned) but harmless to fall into.
		//
		// Same re-assert-every-tick pin pattern as Static mode, just captured
		// once at the moment motion stops rather than at run start.
		if (!bLingerPoseCaptured)
		{
			bLingerPoseCaptured = true;
			LingerLocationUU = Pawn->GetActorLocation();
			LingerRotation = Pawn->GetActorRotation();
			UE_LOG(LogVoxelPerf, Log,
			       TEXT("VoxelPerfRun: flight ended at t=%.1fs, lingering %.1fs at (%.0f, %.0f, %.0f) yaw=%.1f -- ")
			       TEXT("pose pinned, world left ticking/streaming underneath it."),
			       ElapsedSeconds, LingerSec, LingerLocationUU.X, LingerLocationUU.Y, LingerLocationUU.Z,
			       LingerRotation.Yaw);
		}

		Pawn->SetActorLocationAndRotation(LingerLocationUU, LingerRotation,
		                                  /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
		if (PC)
		{
			PC->SetControlRotation(LingerRotation);
		}
		// Deliberately NOT folded into TotalPathFrames/UndergroundFrames: those
		// describe the FLOWN path (FinishRun's fixture-validity check divides
		// by TotalPathFrames), and a parked observation window isn't part of
		// the path -- counting it in would make undergroundFrameFraction drift
		// with however long LingerSec happened to run for, for a flight that
		// had already stopped moving.
		return;
	}

	// Path-local time: zero at the moment the flight itself starts, not at
	// the moment this subsystem started ticking. Both preflight-gate branches
	// above return before reaching here, so by construction ElapsedSeconds >=
	// PreflightSec whenever this line runs (PreflightSec defaults to 0, so
	// this is a no-op subtraction for every existing invocation). Used
	// instead of ElapsedSeconds below so e.g. a line flight's start point is
	// still exactly the captured origin -- not partway down its heading --
	// regardless of how long preflight ran.
	const float FlightSeconds = ElapsedSeconds - PreflightSec;

	if (Flight == EVoxelPerfFlight::Static)
	{
		// Capture the pose ONCE, then re-assert it every tick.
		//
		// Re-asserting rather than simply not moving is the point: an
		// unattended pawn does drift -- one run in this session travelled 254 m
		// and climbed 66 m unprompted -- and a run that quietly moved would
		// report a plausible number for a different scene. Holding it makes
		// that impossible instead of unlikely.
		if (!bStaticPoseCaptured)
		{
			bStaticPoseCaptured = true;
			StaticLocationUU = bStaticLocationOverride ? StaticLocationOverrideUU : Pawn->GetActorLocation();
			UE_LOG(LogVoxelPerf, Log,
			       TEXT("VoxelPerfRun: STATIC pose pinned at (%.0f, %.0f, %.0f) yaw=%.1f pitch=%.1f (%s) -- "
			            "position AND rotation held for the whole run, so the renderer is the only variable."),
			       StaticLocationUU.X, StaticLocationUU.Y, StaticLocationUU.Z, StaticYawDeg, StaticPitchDeg,
			       bStaticLocationOverride ? TEXT("-VoxelPerfStaticAt") : TEXT("spawn pose"));
		}

		const FRotator StaticRotation(StaticPitchDeg, StaticYawDeg, 0.f);
		Pawn->SetActorLocationAndRotation(StaticLocationUU, StaticRotation,
		                                  /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
		if (PC)
		{
			PC->SetControlRotation(StaticRotation);
		}
		++TotalPathFrames;
		return;
	}

	if (Flight == EVoxelPerfFlight::Line)
	{
		// Straight traverse from the captured origin (CircleCenterUU -- same
		// pawn-location-on-first-tick capture the circle path uses; the name
		// predates this flight but the origin is exactly what "start" means
		// here too) along a fixed heading. See the enum's doc comment for why:
		// this is the one flight that keeps admitting virgin terrain for the
		// whole run instead of looping back over ground already streamed in.
		const double HeadingRad = FMath::DegreesToRadians(double(HeadingDeg));
		const FVector Dir(FMath::Cos(HeadingRad), FMath::Sin(HeadingRad), 0.0);
		const double DistanceUU = LinearSpeedUUPerSecOverride * double(FlightSeconds);

		FVector NewLocation = CircleCenterUU + Dir * DistanceUU;

		// Ground-following Z, NOT the circle path's once-computed FixedHeightUU
		// -- a line run can travel for minutes over real terrain, and a Z
		// pinned once at the origin's +30m clearance flies straight into the
		// first hillside taller than that. GetSurfaceHeightUU is documented as
		// a pure query that touches no streaming state (VoxelWorldSubsystem.h)
		// and the underground flight above already calls it every tick for
		// the same reason, so doing so here is established practice, not a
		// new per-tick cost.
		double TargetZUU = FixedHeightUU;
		if (UVoxelWorldSubsystem* Subsystem = World->GetSubsystem<UVoxelWorldSubsystem>())
		{
			TargetZUU = Subsystem->GetSurfaceHeightUU(NewLocation.X, NewLocation.Y) + HeightAboveSurfaceUU;
		}

		// Rate-limit the Z step (see LineMaxZSpeedMultiplier's doc comment):
		// a cliff or a chunk seam's height discontinuity moves the pawn
		// smoothly instead of popping it there in one frame.
		const double MaxZStepUU = LinearSpeedUUPerSecOverride * LineMaxZSpeedMultiplier * double(DeltaTime);
		const double ClampedDeltaZ = FMath::Clamp(TargetZUU - LineLastZUU, -MaxZStepUU, MaxZStepUU);
		NewLocation.Z = LineLastZUU + ClampedDeltaZ;
		LineLastZUU = NewLocation.Z;

		// Face the direction of travel -- yaw = heading -- with the same
		// -10 deg "terrain across the frame, not sky" pitch convention the
		// static fixture's default pose uses.
		const FRotator NewRotation(-10.f, HeadingDeg, 0.f);

		Pawn->SetActorLocationAndRotation(NewLocation, NewRotation, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
		if (PC)
		{
			PC->SetControlRotation(NewRotation);
		}
		++TotalPathFrames;
		return;
	}

	// docs/debug-tooling-plan.md P1: "circle radius 100m around spawn at
	// 20 m/s at surface+30m, constant yaw sweep."
	const double AngularSpeedRadPerSec = LinearSpeedUUPerSecOverride / CircleRadiusUU;
	const double Angle = double(FlightSeconds) * AngularSpeedRadPerSec;

	FVector NewLocation = CircleCenterUU;
	NewLocation.X += CircleRadiusUU * FMath::Cos(Angle);
	NewLocation.Y += CircleRadiusUU * FMath::Sin(Angle);
	NewLocation.Z = FixedHeightUU;

	const float Yaw = FMath::Fmod(FlightSeconds * float(YawSweepDegPerSec), 360.f);
	FRotator NewRotation(-10.f, Yaw, 0.f);

	if (Flight == EVoxelPerfFlight::Underground)
	{
		// Same circle, same speed -- only Z differs, so an A/B against the
		// surface run isolates depth. Z tracks the terrain rather than being
		// pinned once at the spawn column: over a 100 m circle the surface can
		// easily move more than 60 m, and a fixed Z would surface partway
		// round and hand back the surface flight's numbers under an
		// underground label. Following the surface at a constant offset is
		// also the honest shape of the player action being modelled -- walking
		// a tunnel that was dug at a roughly constant depth.
		//
		// The extra Z motion this introduces is not a distortion to apologise
		// for: it is the underground recompute trigger (anchor chunk Z changes
		// while underground) doing exactly what it exists to do, and leaving it
		// out would measure a strictly easier case than a real tunnel walk.
		double SurfaceZ = FixedHeightUU - HeightAboveSurfaceUU;
		if (UWorld* W = GetWorld())
		{
			if (UVoxelWorldSubsystem* Subsystem = W->GetSubsystem<UVoxelWorldSubsystem>())
			{
				SurfaceZ = Subsystem->GetSurfaceHeightUU(NewLocation.X, NewLocation.Y);
			}
		}
		NewLocation.Z = SurfaceZ - DepthUU;

		// Look along the direction of travel, level. A walking player faces
		// down the tunnel; the surface flight's decoupled yaw sweep exists to
		// stress frustum churn against a distant horizon, which underground is
		// a wall two metres away.
		const double TangentDeg = FMath::RadiansToDegrees(Angle) + 90.0;
		NewRotation = FRotator(0.f, float(FMath::Fmod(TangentDeg, 360.0)), 0.f);

		LastPlacedZUU = NewLocation.Z;
		LastSurfaceZUU = SurfaceZ;
		++TotalPathFrames;
		if (NewLocation.Z < SurfaceZ)
		{
			++UndergroundFrames;
		}
	}
	else
	{
		++TotalPathFrames;
	}

	// Teleport (not a swept move): this is a scripted perf-measurement path,
	// not gameplay movement, and terrain has no Chaos collision to sweep
	// against anyway (doctrine: no Chaos for terrain, plan SS3.3).
	Pawn->SetActorLocationAndRotation(NewLocation, NewRotation, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
	if (PC)
	{
		PC->SetControlRotation(NewRotation);
	}
}

void UVoxelPerfRunSubsystem::FinishRun()
{
	bFinished = true;

	TArray<float> Sorted = FrameMsSamples;
	Sorted.Sort();
	const int32 N = Sorted.Num();
	const float P50 = N > 0 ? Sorted[FMath::Clamp(int32(N * 0.50f), 0, N - 1)] : 0.f;
	const float P95 = N > 0 ? Sorted[FMath::Clamp(int32(N * 0.95f), 0, N - 1)] : 0.f;
	const float Max = N > 0 ? Sorted.Last() : 0.f;

	// Post-warmup window (see WarmupExcludeSeconds's doc comment) -- same
	// percentile math, over only the samples from ElapsedSeconds >=
	// WarmupExcludeSeconds onward.
	TArray<float> PostWarmupSorted = PostWarmupFrameMsSamples;
	PostWarmupSorted.Sort();
	const int32 PostWarmupN = PostWarmupSorted.Num();
	const float PostWarmupP50 = PostWarmupN > 0 ? PostWarmupSorted[FMath::Clamp(int32(PostWarmupN * 0.50f), 0, PostWarmupN - 1)] : 0.f;
	const float PostWarmupP95 = PostWarmupN > 0 ? PostWarmupSorted[FMath::Clamp(int32(PostWarmupN * 0.95f), 0, PostWarmupN - 1)] : 0.f;
	const float PostWarmupMax = PostWarmupN > 0 ? PostWarmupSorted.Last() : 0.f;

	int64 ChunksLoaded = 0;
	if (UWorld* World = GetWorld())
	{
		if (UVoxelWorldSubsystem* Subsystem = World->GetSubsystem<UVoxelWorldSubsystem>())
		{
			ChunksLoaded = Subsystem->GetPerfSnapshot().TotalChunksLoaded;
		}
	}
	const float AvgChunksPerSec = DurationSeconds > 0.f ? float(ChunksLoaded) / DurationSeconds : 0.f;
	const float AvgBudgetSaturationPct = BudgetSaturationSamples > 0 ? BudgetSaturationAccum / float(BudgetSaturationSamples) : 0.f;

	// Fixture-validity evidence, reported rather than assumed. An underground
	// run whose fraction is not 1.0 did not measure what its label claims, and
	// the number is in the artifact so that is not something a reader has to
	// take on trust from the flight code.
	const float UndergroundFraction = TotalPathFrames > 0 ? float(UndergroundFrames) / float(TotalPathFrames) : 0.f;
	if (Flight == EVoxelPerfFlight::Underground)
	{
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("VoxelPerfRun fixture check: undergroundFrames=%d/%d (%.1f%%) lastZ=%.0f lastSurfaceZ=%.0f depth=%.0fm"),
		       UndergroundFrames, TotalPathFrames, 100.f * UndergroundFraction, LastPlacedZUU, LastSurfaceZUU, DepthUU / 100.0);
		if (UndergroundFraction < 0.999f)
		{
			UE_LOG(LogVoxelPerf, Warning,
			       TEXT("VoxelPerfRun: underground flight spent %.1f%% of frames ABOVE the surface -- these numbers are not an ")
			       TEXT("underground measurement."),
			       100.f * (1.f - UndergroundFraction));
		}
	}

	const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString OutDir = FPaths::ProjectSavedDir() / TEXT("PerfRuns");
	IFileManager::Get().MakeDirectory(*OutDir, /*Tree*/ true);
	const FString OutPath = OutDir / FString::Printf(TEXT("perf_%s.json"), *Timestamp);

	// Hand-rolled JSON (every field here is a number or a fixed key -- no
	// user-controlled strings to escape, so a full JSON library dependency
	// isn't warranted). tools/check-perf-run.py reads this back.
	const FString Json = FString::Printf(
		TEXT("{\n")
		TEXT("  \"durationSeconds\": %.2f,\n")
		TEXT("  \"frameCount\": %d,\n")
		TEXT("  \"p50FrameMs\": %.3f,\n")
		TEXT("  \"p95FrameMs\": %.3f,\n")
		TEXT("  \"maxFrameMs\": %.3f,\n")
		TEXT("  \"hitchCount\": %d,\n")
		TEXT("  \"hitchThresholdMs\": %.1f,\n")
		TEXT("  \"chunksLoaded\": %lld,\n")
		TEXT("  \"avgChunksPerSec\": %.2f,\n")
		TEXT("  \"avgBudgetSaturationPct\": %.2f,\n")
		TEXT("  \"warmupExcludeSeconds\": %.1f,\n")
		TEXT("  \"postWarmupFrameCount\": %d,\n")
		TEXT("  \"postWarmupP50FrameMs\": %.3f,\n")
		TEXT("  \"postWarmupP95FrameMs\": %.3f,\n")
		TEXT("  \"postWarmupMaxFrameMs\": %.3f,\n")
		TEXT("  \"postWarmupHitchCount\": %d,\n")
		TEXT("  \"flight\": \"%s\",\n")
		TEXT("  \"flightDepthM\": %.1f,\n")
		TEXT("  \"flightSpeedMPerSec\": %.1f,\n")
		// Recorded unconditionally (like staticYaw/PitchDeg below) so a line
		// run's numbers can never be read without the heading they were flown
		// at -- same reasoning as the static pose fields: two line runs at
		// different headings over real (non-flat) terrain are not comparable.
		TEXT("  \"flightHeadingDeg\": %.1f,\n")
		// PreflightSec/LingerSec's own doc comments: recorded so anyone
		// reading frame-time metrics off this JSON can tell a
		// preflight/linger-inflated run apart from one with both at 0, rather
		// than silently comparing the two.
		TEXT("  \"preflightSec\": %.1f,\n")
		TEXT("  \"lingerSec\": %.1f,\n")
		// Recorded so a static run's numbers can never be read without the pose
		// they were taken at. Two static runs at different poses are not
		// comparable, and that is exactly the mistake this mode exists to stop.
		TEXT("  \"staticYawDeg\": %.1f,\n")
		TEXT("  \"staticPitchDeg\": %.1f,\n")
		TEXT("  \"undergroundFrameFraction\": %.4f\n")
		TEXT("}\n"),
		DurationSeconds, N, P50, P95, Max, HitchCount, HitchThresholdMs, (long long)ChunksLoaded, AvgChunksPerSec,
		AvgBudgetSaturationPct, WarmupExcludeSeconds, PostWarmupN, PostWarmupP50, PostWarmupP95, PostWarmupMax, PostWarmupHitchCount,
		Flight == EVoxelPerfFlight::Underground ? TEXT("underground")
		    : Flight == EVoxelPerfFlight::Static ? TEXT("static")
		    : Flight == EVoxelPerfFlight::Line   ? TEXT("line")
		                                          : TEXT("surface"),
		DepthUU / 100.0, LinearSpeedUUPerSecOverride / 100.0, HeadingDeg, PreflightSec, LingerSec, StaticYawDeg, StaticPitchDeg,
		UndergroundFraction);

	FFileHelper::SaveStringToFile(Json, *OutPath);

	UE_LOG(LogVoxelPerf, Log,
	       TEXT("VoxelPerfRun complete: frames=%d p50=%.2fms p95=%.2fms max=%.2fms hitches=%d chunksLoaded=%lld ")
	       TEXT("avgChunks/s=%.2f budgetSat=%.1f%% -- wrote %s"),
	       N, P50, P95, Max, HitchCount, (long long)ChunksLoaded, AvgChunksPerSec, AvgBudgetSaturationPct, *OutPath);

	UE_LOG(LogVoxelPerf, Log,
	       TEXT("VoxelPerfRun post-warmup (t>=%.0fs): frames=%d p50=%.2fms p95=%.2fms max=%.2fms hitches=%d"), WarmupExcludeSeconds,
	       PostWarmupN, PostWarmupP50, PostWarmupP95, PostWarmupMax, PostWarmupHitchCount);

	// Arm the backstop BEFORE asking to exit, so a shutdown that wedges inside
	// FEngineLoop::Exit is still covered -- see FVoxelExitWatchdog's comment.
	// -VoxelPerfExitWatchdogSec=<seconds>, 0 disables (for a debugging session
	// where you WANT the hung process to sit there and be attached to).
	float WatchdogSec = 120.f;
	FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfExitWatchdogSec="), WatchdogSec);
	if (WatchdogSec > 0.f)
	{
		UE_LOG(LogVoxelPerf, Log, TEXT("VoxelPerfRun: arming exit watchdog (%.0fs) and requesting exit."), WatchdogSec);
		ArmExitWatchdog(double(WatchdogSec));
	}

	// Flush now rather than trusting shutdown to do it: if the exit does wedge,
	// every line above this point is the evidence, and a wedged process never
	// closes its log.
	if (GLog)
	{
		GLog->Flush();
	}

	FPlatformMisc::RequestExit(/*bForce*/ false);
}
