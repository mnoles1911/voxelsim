#include "VoxelPerfRunSubsystem.h"

#include "VoxelDebug.h"
// P7: the GI arm this run was measured on. VoxelGI.h for the three switches and
// the anchored outcome, VoxelGIVolume.h for voxel.GI.Volume's reader -- the two
// live in different modules and VoxelEarth already depends on VoxelEarthShaders.
#include "VoxelGI.h"
#include "VoxelGIVolume.h"
#include "VoxelSkySubsystem.h" // FVoxelSkyState / VoxelSky::GetTimeScale -- the sun pose this run was measured at
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
#include "UnrealClient.h" // FScreenshotRequest -- the ONE shutter this project uses; see MaybeFireMovingShot

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

// ---------------------------------------------------------------------------
// SKY READ-BACK HELPERS.
//
// FVoxelSkyState reports the clock as LocalHours (0..24) and DayOfYear (0..365)
// because those are what the ephemeris computes. Both the periodic log line and
// the summary JSON want the two forms a HUMAN pins a leg with -- HH:MM and
// MM-DD -- so a recorded leg can be replayed by pasting its own numbers back
// into -VoxelTimeOfDay= / -VoxelDate= without arithmetic in between.
//
// THE MONTH TABLE THAT USED TO BE COPIED HERE IS GONE. It was a deliberate,
// documented duplicate of VoxelSkySubsystem.cpp's kDaysBeforeMonth/kDaysInMonth,
// copied only because that file was under concurrent edit at the time, with a
// note naming the correct end state: export it as VoxelSky::MonthDayFromDayOfYear
// and delete the copy. That is now done -- the single definition lives in
// VoxelSkySubsystem.cpp beside VoxelSky::kDaysBeforeMonth and is declared in
// VoxelSkySubsystem.h -- so this file calls it instead. The failure it avoided:
// two tables disagreeing shows up as a JSON date one day off from the one the sky
// log printed, which is exactly the size of discrepancy that gets rationalised
// rather than fixed. (REFERENCE YEAR 2000, A LEAP YEAR, February has 29 days --
// stated at the definition, not here.)

// Truncates rather than rounds, so 11:59.9 never prints as 12:00 -- a capture
// labelled "noon" that was taken a minute either side of it is the sort of
// rounding that makes two legs look identical when their sun angles are not.
void PerfClockFromLocalHours(double LocalHours, int32& OutHour, int32& OutMinute)
{
	const double Hours = FMath::Clamp(LocalHours, 0.0, 24.0);
	OutHour = FMath::Clamp((int32)Hours, 0, 23);
	OutMinute = FMath::Clamp((int32)((Hours - (double)OutHour) * 60.0), 0, 59);
}

// THE EFFECTIVE clock rate, which is not the same question as "what is
// voxel.Sky.TimeScale". voxel.Sky.Enabled 0 freezes the clock no matter what
// TimeScale says (UVoxelSkySubsystem::Tick, VoxelSkySubsystem.cpp:1042-1053),
// so reporting the raw cvar would put "timeScale: 1.0" in the artifact for a
// run whose sun never moved -- a reader would then throw out a perfectly good
// frozen-sun leg. FVoxelSkyState::bClockRunning is the state's own answer to
// "did the clock advance this frame", so it is what gates the number, per
// VoxelGpuVerify.cpp:2118-2126's rule that an instrument reports state and
// never intent.
float EffectiveSkyTimeScale(const FVoxelSkyState& State)
{
	return State.bClockRunning ? VoxelSky::GetTimeScale() : 0.f;
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

		// -VoxelPerfHeightM=<metres above surface>. Same guarded-scalar shape as
		// -VoxelPerfDepth above. See HeightAboveSurfaceUU for why the pose is a
		// parameter and which two switches do NOT move it.
		float HeightM = 0.f;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfHeightM="), HeightM) && HeightM > 0.f)
		{
			HeightAboveSurfaceUU = double(HeightM) * 100.0;
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

		// ====================================================================
		// MOVING CAPTURES: -VoxelPerfShotEveryM= (plus -VoxelPerfShotStartM=,
		// -VoxelPerfShotMaxCount=, -VoxelPerfShotName=)
		// ====================================================================
		//
		// COMMAND-LINE SWITCHES AND NOT CVARS, for the same reason
		// -VoxelPerfFlight= above is one and the same reason -VoxelRiverRibbons
		// is: this has to be live before BeginPlay. An -ExecCmds cvar lands
		// after streaming has already begun building a desired set, and the
		// first shot of a line flight can be at distance 0 -- i.e. before any
		// cvar this run will ever see.
		//
		// PARSED AS A STRING AND THEN CONVERTED, not straight into a float.
		// FParse::Value into a float cannot tell "-VoxelPerfShotEveryM=oops"
		// from "the switch was absent": both leave the member at 0, i.e.
		// DISARMED. An image leg that silently took no images is the exact
		// failure this whole feature exists to make impossible, so the two
		// cases are told apart here and only one of them is survivable.
		//
		// PARSED AFTER the flight has been resolved above, because the refusal
		// below depends on which flight this is.
		FString ShotEveryRaw;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfShotEveryM="), ShotEveryRaw))
		{
			const double ShotEveryM = FCString::Atod(*ShotEveryRaw);
			if (ShotEveryM <= 0.0)
			{
				// Abort rather than fall back to disarmed -- the same choice the
				// -VoxelPerfStaticAt branch above makes, for the same reason. A
				// leg that flew the whole traverse, wrote a perfectly plausible
				// summary JSON and produced no images at all would be read as
				// "the change is invisible" rather than as "the camera was never
				// loaded".
				UE_LOG(LogVoxelPerf, Error,
				       TEXT("VoxelPerfRun: -VoxelPerfShotEveryM=%s is not a positive number of metres. Aborting run ")
				       TEXT("rather than flying an image leg that takes no images."),
				       *ShotEveryRaw);
				bRequested = false;
				return;
			}

			// ================================================================
			// LINE FLIGHT ONLY -- AND THIS REFUSES RATHER THAN DEGRADES.
			// ================================================================
			//
			// The trigger is horizontal distance from the flight origin, and
			// that is a usable path parameter for exactly one of the four
			// flights:
			//
			//   line        -- leaves the origin on a fixed heading and never
			//                  returns, so distance-from-origin is strictly
			//                  increasing and indexes the ground one-to-one.
			//                  This is the case the feature is for.
			//   surface     -- a CLOSED 100 m circle. Distance-from-origin is
			//   underground    pinned at the radius from the first frame
			//                  onward, so a distance trigger fires once and
			//                  then never again -- or, with a step under 100 m,
			//                  fires a burst in the first second and labels it
			//                  a multi-kilometre traverse.
			//   static      -- never leaves the origin at all.
			//
			// Degrading to "shoot every N seconds instead" on those was
			// considered and rejected: it would hand back frames that LOOK like
			// a matched pair and are not, which is strictly worse than handing
			// back no frames. Arc length along the circle would be an honest
			// parameter, but no decision is waiting on a circling capture, so
			// it is not built and this refuses instead of guessing.
			if (Flight != EVoxelPerfFlight::Line)
			{
				UE_LOG(LogVoxelPerf, Error,
				       TEXT("VoxelPerfRun: -VoxelPerfShotEveryM= requires -VoxelPerfFlight=line. The shutter triggers ")
				       TEXT("on distance from the flight origin, and that only indexes the ground on a straight ")
				       TEXT("traverse -- the circle flights sit at a constant radius and the static fixture never ")
				       TEXT("moves. Aborting run rather than producing frames that look like a matched pair and ")
				       TEXT("are not."));
				bRequested = false;
				return;
			}

			ShotEveryUU = ShotEveryM * 100.0;

			// Same guarded-scalar shape as -VoxelPerfSpeed/-VoxelPerfDepth
			// above: parse into a local, only overwrite the member on a
			// present-and-positive value. 0 means "start at the origin", which
			// is already the default, so there is nothing to abort over.
			float ShotStartM = 0.f;
			if (FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfShotStartM="), ShotStartM) && ShotStartM > 0.f)
			{
				ShotStartUU = double(ShotStartM) * 100.0;
			}

			int32 MaxCountValue = 0;
			if (FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfShotMaxCount="), MaxCountValue) && MaxCountValue > 0)
			{
				ShotMaxCount = MaxCountValue;
			}

			FString TagValue;
			if (FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfShotName="), TagValue) && !TagValue.IsEmpty())
			{
				// SANITISED TO [A-Za-z0-9_], and the whitelist is deliberate
				// rather than a blacklist of the characters that bit us last
				// time. This one string reaches three places that each have a
				// different way of being wrong about it: a screenshot basename
				// (where a '.' is an extension waiting to be misparsed --
				// VoxelSkyLadderFixture.cpp:743), the comparer in
				// tools/voxel-pair-moving-shots.py (which splits these names on
				// '-'), and the summary JSON, whose writer documents itself as
				// having "no user-controlled strings to escape". A whitelist is
				// the only version of this that is still true after the next
				// person adds a fourth reader.
				FString Safe;
				Safe.Reserve(TagValue.Len());
				for (const TCHAR C : TagValue)
				{
					Safe.AppendChar(FChar::IsAlnum(C) ? C : TEXT('_'));
				}
				ShotTag = Safe;
			}

			// ================================================================
			// THIS IS AN IMAGE LEG. ITS FRAME TIMES ARE NOT A TIMING RESULT.
			// ================================================================
			//
			// A rule, not a caveat, and it is stated at the moment of arming so
			// nobody can launch this without being told.
			//
			// FScreenshotRequest stalls the frame it is serviced on: the
			// viewport reads back the render target and the PNG encode runs off
			// it. So a moving capture PERTURBS THE VERY TIMING IT IS FLYING
			// THROUGH. A 32-shot leg puts 32 stalls into FrameMsSamples, and
			// they land at p95 and max -- which is exactly where every streaming
			// verdict in this project is read.
			//
			// Worse for an A/B: the stalls are NOT symmetric between arms. They
			// fire at fixed DISTANCES, so the slower arm takes the same number
			// of stalls over fewer frames and wears a larger fraction of them.
			// A timing comparison drawn from two image legs would therefore be
			// biased against the slower arm by the instrument itself.
			//
			// So: shoot images on one leg, take timings on another, and never
			// quote the two out of the same run. The summary JSON carries
			// frameTimingAdmissible: 0 for this leg so the rule survives the
			// walk from this log line to whoever reads the artifact next month.
			UE_LOG(LogVoxelPerf, Warning,
			       TEXT("VoxelPerfShots ARMED: every %.0fm from %.0fm, at most %d shot(s), tag='%s'. THIS IS AN ")
			       TEXT("IMAGE LEG -- the shutter stalls the frame it is serviced on, so THE FRAME-TIME NUMBERS IN ")
			       TEXT("THIS RUN'S SUMMARY (p50/p95/max/hitches) ARE NOT ADMISSIBLE AS TIMING. Take timings on a ")
			       TEXT("leg with no -VoxelPerfShotEveryM=."),
			       ShotEveryUU / 100.0, ShotStartUU / 100.0, ShotMaxCount,
			       ShotTag.IsEmpty() ? TEXT("<none>") : *ShotTag);
		}

		// -VoxelPerfLogInterval=, the SAME switch and the SAME 5 s default and
		// 0.25 s floor FVoxelWorldImpl::MaybeLogCounters applies
		// (VoxelWorldSubsystem.cpp:5019-5024). Read here rather than shared
		// through a header because it is four lines and a cross-subsystem
		// accessor for a command-line scalar is more coupling than the
		// duplication costs -- but the two MUST stay equal, because
		// tools/voxel-leg-summary.ps1 pairs sky windows with streaming windows
		// positionally and a sky line on its own cadence would silently
		// misalign that pairing.
		float LogIntervalValue = 5.f;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfLogInterval="), LogIntervalValue))
		{
			SkyLogIntervalSec = FMath::Max(0.25f, LogIntervalValue);
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

	// The sun, per window, for the WHOLE leg -- preflight and linger included,
	// exactly like the streaming counters, because the sun does not stop moving
	// when the flight path does and a shadow cache busted during preflight is
	// busted for the frames after it too.
	//
	// WHY A PER-WINDOW LINE AND NOT JUST THE SUMMARY JSON. FinishRun records ONE
	// sun pose (the last one). That is sufficient to compare two FROZEN legs and
	// insufficient to characterise a moving one: a leg that swept 40 degrees of
	// altitude and a leg that never moved can finish at the same angle, and the
	// artifact would then say the two runs were taken at the same sun. This line
	// is the sweep itself, on the record, and tools/voxel-leg-summary.ps1 turns
	// it into a first->last range so "the sun moved" is a column rather than an
	// assumption.
	MaybeLogSky(DeltaTime);

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

		// MOVING CAPTURES. Called HERE -- at the site that actually places the
		// pawn, in the same tick as the placement, and handed the pose the pawn
		// was ACTUALLY placed at rather than the pose the path math wanted --
		// because the frame that is about to be drawn is the frame at the pose
		// the pawn is at now. Disarmed by default (ShotEveryUU == 0), in which
		// case this is one compare and a return, so every existing leg is
		// byte-identical. See MaybeFireMovingShot for why the shutter triggers
		// on distance travelled and not on the clock.
		MaybeFireMovingShot(NewLocation, NewRotation);
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

// ---------------------------------------------------------------------------
// THE SHUTTER TRIGGERS ON DISTANCE TRAVELLED, NOT ON THE CLOCK.
//
// This is the single decision that makes these frames evidence instead of
// pictures, so the argument is written here, at the trigger, rather than in a
// document that the next person editing this function will not have open.
//
// Two arms are compared by putting their frames side by side. That means
// nothing unless the two frames are OF THE SAME GROUND FROM THE SAME POSE. The
// line flight is deterministic -- fixed -VoxelSpawnAt, fixed -VoxelPerfHeading,
// fixed -VoxelPerfSpeed, sun pinned with -VoxelTimeScale=0 -- so the ground in
// frame is a function of ONE number: how far down the path the anchor is.
// Trigger on that number and the two arms photograph the same ground by
// construction.
//
// TRIGGER ON WALL-CLOCK TIME INSTEAD AND THE ARMS DIVERGE the moment they
// differ in frame rate, in exactly the direction that poisons the result: the
// SLOWER arm -- the one under suspicion, the one an armed change is supposed to
// be judged on -- would be photographed over different ground, and any
// difference in the picture could then be blamed on the change when it was
// really a different hillside. An A/B whose confound points at the hypothesis
// is worse than no A/B.
//
// (Honest note on what is and is not load-bearing TODAY. The line path's
// position is currently a closed form in flight time --
// LinearSpeedUUPerSecOverride * FlightSeconds, see StepFlightPath -- so for
// THIS path, as it stands, a time trigger and a distance trigger would in fact
// land on the same ground. That is a property of the current path math, not of
// the method, and it is not something to build an archive on. The distance
// formulation is the one that stays correct if the path is ever integrated per
// frame, which any physics-driven, terrain-braked or variable-speed flight
// would be; it is the one that still pairs two arms flown at different
// -VoxelPerfSpeed; and it is the one that makes the FILENAME mean the ground
// rather than the clock, which is what a comparer needs.)
//
// AND THE DISTANCE IS MEASURED, NOT RE-DERIVED. It comes from the pose the pawn
// was actually placed at this frame, not from re-evaluating the path formula.
// A join computed instead of checked is the shape of a well-worn family of bugs
// in this tree, and here it would produce a filename stating a distance the
// camera was never at -- an archive that lies quietly, forever.
// ---------------------------------------------------------------------------
void UVoxelPerfRunSubsystem::MaybeFireMovingShot(const FVector& PlacedLocationUU, const FRotator& PlacedRotation)
{
	// THE ONE-FRAME BRACKET (see bShotPending's doc comment in the header).
	// Last frame asked for a shutter; the viewport services the request at the
	// end of a subsequent draw. Log where the pawn is NOW, so the pose the frame
	// was actually drawn at is BRACKETED by two logged poses instead of assumed
	// to equal the first. This is cheap and it is the difference between "the
	// lag is probably negligible" and a number on the record for this leg.
	if (bShotPending)
	{
		bShotPending = false;
		const double AdvanceUU = FVector::Dist2D(PlacedLocationUU, PendingShotLocationUU);
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("VoxelPerfShotBracket nominalM=%d advancedSinceRequestM=%.3f -- the drawn frame is somewhere in ")
		       TEXT("[request pose, this pose]. FScreenshotRequest only raises a flag; the viewport services it at ")
		       TEXT("the end of a subsequent draw, so this is the width of the uncertainty on that shot's pose."),
		       PendingShotNominalM, AdvanceUU / 100.0);
	}

	if (ShotEveryUU <= 0.0)
	{
		// Disarmed -- the default, and the reason every existing leg is
		// byte-identical: this is the whole cost of the feature when off.
		return;
	}

	// Measured HORIZONTALLY from the flight origin, because the ground is
	// indexed by XY: the line flight's Z is a rate-limited ground-follower
	// (see StepFlightPath) whose vertical wander over a hillside is not
	// progress along the path and must not count as any.
	const double DistanceUU = FVector::Dist2D(PlacedLocationUU, CircleCenterUU);
	MaxDistanceReachedUU = FMath::Max(MaxDistanceReachedUU, DistanceUU);

	if (ShotsFired >= ShotMaxCount)
	{
		return; // cap already reached and already announced, once, below
	}
	if (DistanceUU < ShotStartUU + double(NextShotIndex) * ShotEveryUU)
	{
		return; // not at the next boundary yet
	}

	// WHICH boundary we are at, not merely that we passed one.
	//
	// A frame at 20 m/s covers 0.3-2 m, so normally ReachedIndex ==
	// NextShotIndex and nothing below the FloorToInt32 does anything. A
	// multi-second hitch (or a step smaller than one frame's travel) can step
	// over one or more boundaries entirely. Shoot the NEAREST boundary behind
	// the current pose -- the one with the smallest residual, i.e. the most
	// honest filename -- and say out loud which ones were stepped over, because
	// a boundary skipped on one arm and hit on the other is exactly the
	// asymmetry that makes two shot lists disagree. Note which arm skips: the
	// SLOWER one, which is the direction that would quietly flatter it.
	const int32 ReachedIndex = FMath::FloorToInt32((DistanceUU - ShotStartUU) / ShotEveryUU);
	const int32 Skipped = FMath::Max(0, ReachedIndex - NextShotIndex);
	if (Skipped > 0)
	{
		ShotBoundariesSkipped += Skipped;
		UE_LOG(LogVoxelPerf, Warning,
		       TEXT("VoxelPerfShot: the path stepped over %d boundary/boundaries (nominal %.0fm..%.0fm) with no ")
		       TEXT("frame landing in them -- those distances will be MISSING from this arm's shot list. If the ")
		       TEXT("other arm HAS them they are not a comparable pair; drop them rather than pairing across."),
		       Skipped,
		       (ShotStartUU + double(NextShotIndex) * ShotEveryUU) / 100.0,
		       (ShotStartUU + double(ReachedIndex - 1) * ShotEveryUU) / 100.0);
	}

	const double NominalUU = ShotStartUU + double(ReachedIndex) * ShotEveryUU;
	const int32 NominalM = FMath::RoundToInt32(NominalUU / 100.0);

	// THE NAME CARRIES THE ARM AND THE NOMINAL DISTANCE, because that is what
	// makes two arms pairable at all: both arms name their 512 m shot "d00512"
	// whatever their few-centimetre residuals were, so a comparer pairs by
	// filename and never has to guess. The residual is NOT thrown away -- it is
	// in the log line below, and checking that the two arms' ACTUAL distances
	// agree is the comparer's job before anyone looks at a pixel.
	//
	// %05d so a directory listing sorts in flight order out to 99.9 km.
	//
	// The trailing '_' is not decoration. RequestScreenshot with
	// bAddFilenameSuffix=true appends its own %05i uniqueness suffix, and
	// without a separator "...d00512" + "00000" reads as one ten-digit run --
	// see this project's own archive, which contains
	// VoxelSkyLadder_00_00h0000000.png with exactly that problem.
	const FString ShotName = ShotTag.IsEmpty()
	                             ? FString::Printf(TEXT("VoxelMove-d%05d_"), NominalM)
	                             : FString::Printf(TEXT("VoxelMove_%s-d%05d_"), *ShotTag, NominalM);

	// LOGGED BEFORE THE SHUTTER, AND LOGGED IN FULL, so two arms' shot lists can
	// be checked for agreement BEFORE any pixels are compared -- which is the
	// order that keeps this an instrument rather than a slideshow. Every field a
	// comparer needs in order to prove the two frames are of the same ground is
	// on this one line: nominalM pairs them, actualM/residualM say how closely
	// they really line up, and pos/yaw/pitch is the pose itself. A mismatch is
	// therefore loud and mechanical, not a judgement call.
	//
	// GREP: "VoxelPerfShot n="
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("VoxelPerfShot n=%d/%d nominalM=%d actualM=%.3f residualM=%+.3f pos=(%.1f, %.1f, %.1f) ")
	       TEXT("yaw=%.3f pitch=%.3f headingDeg=%.1f speedMPerSec=%.1f pathFrame=%d engineFrame=%llu ")
	       TEXT("flightSec=%.3f name=%s"),
	       ShotsFired + 1, ShotMaxCount, NominalM, DistanceUU / 100.0, (DistanceUU - NominalUU) / 100.0,
	       PlacedLocationUU.X, PlacedLocationUU.Y, PlacedLocationUU.Z,
	       PlacedRotation.Yaw, PlacedRotation.Pitch, HeadingDeg, LinearSpeedUUPerSecOverride / 100.0,
	       TotalPathFrames, (unsigned long long)GFrameCounter,
	       ElapsedSeconds - PreflightSec, *ShotName);

	// THE SAME CALL AS EVERY OTHER FIXTURE IN THIS TREE, deliberately:
	// FScreenshotRequest and NOT HighResShot, bShowUI=false,
	// bAddFilenameSuffix=true. HighResShot re-renders through a tiled path with
	// its own screen-percentage and post-process behaviour, so its output is not
	// comparable to the rest of this project's capture archive -- and
	// comparability across frames is the entire product here.
	// VoxelSkyLadderFixture.cpp:779-786 states the same rule for the same
	// reason. Lands in ue-project/Saved/Screenshots/WindowsEditor/.
	FScreenshotRequest::RequestScreenshot(*ShotName, /*bShowUI*/ false, /*bAddFilenameSuffix*/ true);

	++ShotsFired;
	NextShotIndex = ReachedIndex + 1;
	bShotPending = true;
	PendingShotNominalM = NominalM;
	PendingShotLocationUU = PlacedLocationUU;

	if (ShotsFired >= ShotMaxCount)
	{
		// Announced ONCE, at the moment the cap bites, naming the distance it
		// bit at -- so "the second half of the traverse has no images" is a line
		// in the log rather than something noticed later from a short directory
		// listing and mistaken for a crash.
		UE_LOG(LogVoxelPerf, Warning,
		       TEXT("VoxelPerfShot: -VoxelPerfShotMaxCount=%d reached at %.0fm from the origin. THE REST OF THIS ")
		       TEXT("FLIGHT IS UNSHOT -- if the far end of the traverse is what you meant to look at, raise the cap ")
		       TEXT("or widen -VoxelPerfShotEveryM=."),
		       ShotMaxCount, DistanceUU / 100.0);
	}
}

void UVoxelPerfRunSubsystem::MaybeLogSky(float DeltaTime)
{
	SkyLogAccumSec += DeltaTime;
	if (SkyLogAccumSec < SkyLogIntervalSec)
	{
		return;
	}
	SkyLogAccumSec = 0.f;

	UWorld* World = GetWorld();
	UVoxelSkySubsystem* Sky = World ? World->GetSubsystem<UVoxelSkySubsystem>() : nullptr;
	if (!Sky)
	{
		// No line at all rather than a zeroed one. A "tod=00:00 sunAlt=0.00"
		// line from a world with no sky subsystem is indistinguishable from a
		// real midnight-at-the-horizon reading, and the summariser would report
		// it as a sun pose. An ABSENT column reads as "this leg predates the
		// instrument", which is the truth.
		return;
	}

	const FVoxelSkyState& S = Sky->GetSkyState();
	int32 Hour = 0, Minute = 0;
	PerfClockFromLocalHours(S.LocalHours, Hour, Minute);
	int32 Month = 1, Day = 1;
	VoxelSky::MonthDayFromDayOfYear(S.DayOfYear, Month, Day);

	// lightUpdates is the count of times the rig's rotation was ACTUALLY
	// written (voxel.Sky.ShadowUpdateHz caps it -- FVoxelSkyState:108-112), and
	// it is the number that matters for the frozen-vs-moving question: it is
	// the rate at which UE's cached whole-scene shadow setup was invalidated.
	// A leg with timeScale != 0 but lightUpdates flat across the whole log has
	// a broken cadence cap, not a frozen sun, and those two are otherwise
	// indistinguishable from the frame times alone.
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel sky (%.0fs window): tod=%02d:%02d sunAlt=%.2f sunAz=%.2f timeScale=%.3f date=%02d-%02d ")
	       TEXT("sunUp=%d lightUpdates=%lld"),
	       SkyLogIntervalSec, Hour, Minute, S.SunAltitudeDeg, S.SunAzimuthDeg, EffectiveSkyTimeScale(S), Month, Day,
	       S.bSunUp ? 1 : 0, (long long)S.LightUpdates);
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

	// --- MOVING CAPTURES: THE ARMED-BUT-INERT CASE MUST BE LOUD --------------
	//
	// "Shots armed every 512 m and 0 fired" is a line, not silence. An image leg
	// that produced no images and said nothing about it gets read as "the change
	// is invisible", and a false null on a renderer is the most expensive way
	// this tree has been wrong.
	//
	// This block only speaks when the feature was ARMED, so a leg that never
	// asked for shots is byte-identical here too.
	if (ShotEveryUU > 0.0)
	{
		// Named locals rather than inline expressions inside the UE_LOG, for the
		// reason VoxelSkyLadderFixture.cpp:655 gives for the same choice: one of
		// these is a temporary FString, and a temporary's `*` inside a log macro
		// is exactly the kind of lifetime question nobody should have to answer
		// while reading a report line.
		const FString TagLabel = ShotTag.IsEmpty() ? FString(TEXT("<none>")) : ShotTag;
		const FString FilePrefix = ShotTag.IsEmpty() ? FString(TEXT("VoxelMove"))
		                                            : FString(TEXT("VoxelMove_")) + ShotTag;
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("VoxelPerfShots: armed every %.0fm from %.0fm (cap %d, tag='%s') -- fired=%d ")
		       TEXT("skippedBoundaries=%d maxDistanceReachedM=%.1f. Images in Saved/Screenshots/WindowsEditor/ ")
		       TEXT("as %s-d<metres>_*.png."),
		       ShotEveryUU / 100.0, ShotStartUU / 100.0, ShotMaxCount, *TagLabel,
		       ShotsFired, ShotBoundariesSkipped, MaxDistanceReachedUU / 100.0, *FilePrefix);

		if (ShotsFired == 0)
		{
			// TWO-SIDED BY CONSTRUCTION: this cannot fire on a leg that shot
			// anything, so it is a statement about THIS leg rather than a
			// warning that appears in every run and is therefore read by
			// nobody. And it names the reading that EXPLAINS the zero -- how far
			// the anchor actually got -- instead of leaving "why did nothing
			// fire" to be guessed at from a directory listing.
			UE_LOG(LogVoxelPerf, Error,
			       TEXT("VoxelPerfShots: ARMED EVERY %.0fm AND FIRED 0. THIS LEG PRODUCED NO IMAGES. The anchor ")
			       TEXT("reached %.1fm from the flight origin and the first boundary is at %.0fm, so either the ")
			       TEXT("flight was too short for the step (-VoxelPerfRun= / -VoxelPerfSpeed=) or ")
			       TEXT("-VoxelPerfShotStartM= is beyond the end of the traverse. DO NOT read this leg as 'no ")
			       TEXT("visible difference'."),
			       ShotEveryUU / 100.0, MaxDistanceReachedUU / 100.0, ShotStartUU / 100.0);
		}
		else
		{
			// Restated at the END of the run as well as at arm time. The arming
			// Warning is thousands of lines up the log by now, and the numbers
			// it warns about are printed a few lines BELOW this one -- so this
			// is where a reader scrolling to the result actually meets it.
			UE_LOG(LogVoxelPerf, Warning,
			       TEXT("VoxelPerfShots: %d shutter(s) fired during this flight. THE FRAME-TIME NUMBERS BELOW ARE ")
			       TEXT("NOT ADMISSIBLE AS TIMING -- a screenshot stalls the frame it is serviced on, and those ")
			       TEXT("stalls land in p95 and max. This is an IMAGE leg (frameTimingAdmissible: 0 in the ")
			       TEXT("summary JSON); take timings on a leg with no -VoxelPerfShotEveryM=."),
			       ShotsFired);
		}
	}

	// --- SKY STATE, READ BACK FROM THE SUBSYSTEM ------------------------------
	//
	// Read from UVoxelSkySubsystem::GetSkyState() rather than from the command
	// line, so the artifact records what the run USED rather than what it was
	// asked for. That distinction is not pedantic here: the calendar quantises
	// the request (VoxelSkySubsystem.cpp:317-331 -- reachable dates are 7.6 real
	// days apart at the default DaysPerYear), so -VoxelDate=06-21 does not
	// generally produce 21 June, and a leg recorded from its switches would
	// claim a date the sun was never at.
	FVoxelSkyState SkyState;
	bool bHaveSky = false;
	if (UWorld* World = GetWorld())
	{
		if (UVoxelSkySubsystem* Sky = World->GetSubsystem<UVoxelSkySubsystem>())
		{
			SkyState = Sky->GetSkyState();
			bHaveSky = true;
		}
	}
	// --- GI ARM, READ BACK FROM THE SUBSYSTEM AND FROM THE CVARS -------------
	//
	// Same reasoning as the sky block above, and the same failure it prevents:
	// a frame-time number read without the arm it was taken on. GI's arm is
	// three switches and one RUNTIME OUTCOME, and it is the outcome that makes
	// this worth reading back rather than reconstructing from a command line.
	//
	// THIS EXISTS BECAUSE THE PROJECT ALREADY MADE THE MISTAKE. Every leg on
	// record ran with voxel.GI.Enabled 1 and voxel.GI.Volume 1 while three
	// documents and the cvar's own help text said both shipped at 0, so a
	// month of budgeting was done against a baseline that already contained
	// GI's cost. A leg that stamps its own arm cannot be misread that way.
	//
	// giVolumeAnchored IS THE ONE NO CVAR REPORTS. A leg can have every switch
	// on and still have sampled nothing all run, because EnsureVolumeOrigin
	// refused to identify the terrain pool (P7-a). Its p95 then describes GI
	// being off while the command line says it is on -- which is precisely the
	// class of silent arm failure -VoxelGIOff's own re-check was added for.
	int32 GiEnabled = 0, GiVolume = 0, GiSourceBricks = 0, GiAnchored = 0, GiIdentityRefusals = 0;
	int32 GiMarchBricks = 0;
	bool bHaveGI = false;
	if (UWorld* World = GetWorld())
	{
		if (const UVoxelGISubsystem* GI = World->GetSubsystem<UVoxelGISubsystem>())
		{
			GiAnchored = GI->IsVolumeAnchored() ? 1 : 0;
			GiIdentityRefusals = GI->GetPoolIdentityRefusals();
			bHaveGI = true;
		}
	}
	GiEnabled = VoxelGI::IsEnabled() ? 1 : 0;
	GiVolume = VoxelGIVolume::IsEnabled() ? 1 : 0;
	GiMarchBricks = VoxelGI::IsMarchBricksEnabled() ? 1 : 0;
	// The two arms are reported SEPARATELY even though IsQuadIngestRetired()
	// is true on both. Folding them would make a march leg and a fed-by-nothing
	// leg produce identical JSON, and those two describe opposite things: one
	// has GI content produced on the GPU, the other has no GI content at all.
	GiSourceBricks = (VoxelGI::IsQuadIngestRetired() && GiMarchBricks == 0) ? 1 : 0;

	const float SkyTimeScale = EffectiveSkyTimeScale(SkyState);
	int32 SkyHour = 0, SkyMinute = 0;
	PerfClockFromLocalHours(SkyState.LocalHours, SkyHour, SkyMinute);
	int32 SkyMonth = 1, SkyDay = 1;
	VoxelSky::MonthDayFromDayOfYear(SkyState.DayOfYear, SkyMonth, SkyDay);

	// Fixture-validity evidence, same shape and same purpose as the
	// underground-fraction warning above: state the way this run could fail to
	// mean what its label says, in the run's own log, rather than leaving it to
	// be noticed.
	//
	// A MOVING SUN IS NOT A SMALL EFFECT ON THIS DRAW PATH. A movable
	// directional light that has not rotated since spawn lets the renderer keep
	// its cached whole-scene shadow setup; a sun that rotates re-renders the
	// resident geometry into every cascade it touches. docs/backlog.md §0's
	// shadowGather=0 / ~1.03 gathers-per-frame baseline was taken with a sun
	// frozen since spawn, so it does not transfer to a day/night run -- and
	// worse, a leg that spans a sunrise is not even comparable to ITSELF
	// end-to-end, because the cost changes underneath the average.
	if (SkyTimeScale != 0.f)
	{
		UE_LOG(LogVoxelPerf, Warning,
		       TEXT("VoxelPerfRun: voxel.Sky.TimeScale=%.3f -- THE SUN MOVED DURING THIS LEG (%02d:%02d at exit, ")
		       TEXT("sunAlt=%.2f deg, %lld light updates). These frame times are an average over a CHANGING ")
		       TEXT("shadow-cache state and are not comparable against a frozen-sun leg, nor against another ")
		       TEXT("moving-sun leg that started at a different hour. Pin -VoxelTimeScale=0 for any comparison ")
		       TEXT("where the sun is not the variable under test; see the per-window 'Voxel sky' lines above ")
		       TEXT("for the sweep this leg actually covered."),
		       SkyTimeScale, SkyHour, SkyMinute, SkyState.SunAltitudeDeg, (long long)SkyState.LightUpdates);
	}
	if (!bHaveSky)
	{
		// The fields below will be a zeroed state's -- midnight, sun at 0/0.
		// Say so, because "midnight" and "there was no sky subsystem" produce
		// byte-identical JSON.
		UE_LOG(LogVoxelPerf, Warning,
		       TEXT("VoxelPerfRun: no UVoxelSkySubsystem in this world -- the sky fields in the summary JSON are ")
		       TEXT("a ZEROED state, not a measurement. Do not read them as a sun pose."));
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
		TEXT("  \"undergroundFrameFraction\": %.4f,\n")
		// THE SUN, RECORDED UNCONDITIONALLY, for exactly the reason
		// staticYawDeg/staticPitchDeg two lines up are: "a static run's numbers
		// can never be read without the pose they were taken at. Two static
		// runs at different poses are not comparable, and that is exactly the
		// mistake this mode exists to stop."
		//
		// TWO LEGS AT DIFFERENT SUN ANGLES ARE THE SAME CLASS OF MISTAKE. The
		// sun is a pose too -- it decides how much geometry lands in each
		// shadow cascade and whether the renderer's cached whole-scene shadow
		// setup survives the frame -- and unlike the camera pose it now moves
		// on its own unless something pins it. Every perf number this project
		// has on record predates the day/night clock and was therefore taken at
		// one frozen pose (docs/backlog.md §0); with these fields absent, the
		// first leg run after the clock landed would have been silently
		// comparable-looking against all of them.
		//
		// timeOfDay/dateMMDD are strings in the exact shape of the switches
		// that set them, so a leg can be reproduced by pasting its own artifact
		// back into -VoxelTimeOfDay= / -VoxelDate= with no arithmetic in
		// between -- and sunAltitudeDeg is the ground truth to check that
		// reproduction against, since the calendar may not be able to land on
		// the requested date exactly.
		//
		// WITH timeScale != 0 THESE ARE END-OF-RUN VALUES, not the leg's. That
		// leg is not comparable to anything (see the Warning logged above), and
		// the per-window "Voxel sky" lines are where its sweep lives.
		TEXT("  \"timeOfDay\": \"%02d:%02d\",\n")
		TEXT("  \"dateMMDD\": \"%02d-%02d\",\n")
		TEXT("  \"timeScale\": %.3f,\n")
		TEXT("  \"sunAltitudeDeg\": %.2f,\n")
		TEXT("  \"sunAzimuthDeg\": %.2f,\n")
		// P7. The GI arm, recorded unconditionally, for the sun block's reason.
		// giSubsystemPresent distinguishes "GI was off" from "there was no GI
		// subsystem in this world" -- both of which would otherwise write
		// giVolumeAnchored: 0, exactly as a zeroed sky state and midnight write
		// identical JSON above.
		TEXT("  \"giSubsystemPresent\": %d,\n")
		TEXT("  \"giEnabled\": %d,\n")
		TEXT("  \"giVolume\": %d,\n")
		TEXT("  \"giSourceBricks\": %d,\n")
		TEXT("  \"giMarchBricks\": %d,\n")
		TEXT("  \"giVolumeAnchored\": %d,\n")
		TEXT("  \"giPoolIdentityRefusals\": %d,\n")
		// --- MOVING CAPTURES ------------------------------------------------
		//
		// frameTimingAdmissible is the field that survives the walk from this
		// run's log to whoever reads the artifact next month. A leg that fired
		// shutters has stalls in its frame samples; EVERY frame-time number
		// above it in this same file is contaminated by them, and a reader who
		// was not present when the leg was launched has no other way to know
		// that. It is 1 on every leg that took no shots, which is every leg
		// this project has ever run, so no historical artifact changes meaning.
		//
		// shotTag is the ONE operator-supplied string in this file; it is
		// whitelisted to [A-Za-z0-9_] at its parse site precisely so this
		// hand-rolled writer's "no user-controlled strings to escape" claim
		// stays true.
		TEXT("  \"shotEveryM\": %.1f,\n")
		TEXT("  \"shotStartM\": %.1f,\n")
		TEXT("  \"shotsFired\": %d,\n")
		TEXT("  \"shotBoundariesSkipped\": %d,\n")
		TEXT("  \"shotMaxDistanceReachedM\": %.1f,\n")
		TEXT("  \"shotTag\": \"%s\",\n")
		TEXT("  \"frameTimingAdmissible\": %d\n")
		TEXT("}\n"),
		DurationSeconds, N, P50, P95, Max, HitchCount, HitchThresholdMs, (long long)ChunksLoaded, AvgChunksPerSec,
		AvgBudgetSaturationPct, WarmupExcludeSeconds, PostWarmupN, PostWarmupP50, PostWarmupP95, PostWarmupMax, PostWarmupHitchCount,
		Flight == EVoxelPerfFlight::Underground ? TEXT("underground")
		    : Flight == EVoxelPerfFlight::Static ? TEXT("static")
		    : Flight == EVoxelPerfFlight::Line   ? TEXT("line")
		                                          : TEXT("surface"),
		DepthUU / 100.0, LinearSpeedUUPerSecOverride / 100.0, HeadingDeg, PreflightSec, LingerSec, StaticYawDeg, StaticPitchDeg,
		UndergroundFraction,
		SkyHour, SkyMinute, SkyMonth, SkyDay, SkyTimeScale, SkyState.SunAltitudeDeg, SkyState.SunAzimuthDeg,
		bHaveGI ? 1 : 0, GiEnabled, GiVolume, GiSourceBricks, GiMarchBricks, GiAnchored,
		GiIdentityRefusals,
		ShotEveryUU / 100.0, ShotStartUU / 100.0, ShotsFired, ShotBoundariesSkipped,
		MaxDistanceReachedUU / 100.0, *ShotTag, ShotsFired == 0 ? 1 : 0);

	FFileHelper::SaveStringToFile(Json, *OutPath);

	UE_LOG(LogVoxelPerf, Log,
	       TEXT("VoxelPerfRun complete: frames=%d p50=%.2fms p95=%.2fms max=%.2fms hitches=%d chunksLoaded=%lld ")
	       TEXT("avgChunks/s=%.2f budgetSat=%.1f%% -- wrote %s"),
	       N, P50, P95, Max, HitchCount, (long long)ChunksLoaded, AvgChunksPerSec, AvgBudgetSaturationPct, *OutPath);

	UE_LOG(LogVoxelPerf, Log,
	       TEXT("VoxelPerfRun post-warmup (t>=%.0fs): frames=%d p50=%.2fms p95=%.2fms max=%.2fms hitches=%d"), WarmupExcludeSeconds,
	       PostWarmupN, PostWarmupP50, PostWarmupP95, PostWarmupMax, PostWarmupHitchCount);

	// --- THE HITCH COUNT SILENTLY CHANGED MEANING, SO SAY WHEN IT HAS --------
	//
	// hitchThresholdMs is a FIXED 33.3 (VoxelDebug::kHitchThresholdMs). When
	// this threshold was chosen, p50 was 23.3 ms and a hitch genuinely meant a
	// spike. Since shadows started rendering on 2026-08-19 the shadowed
	// baseline runs p50 ~34.7, so the MEDIAN frame now exceeds the threshold
	// and roughly 65% of frames count as "hitches" for the entirely mundane
	// reason that the scene runs at 28.8 fps.
	//
	// A hitch count taken in that regime is not comparable to any historical
	// one -- including the x3.2 that governed voxel GI for a month -- and
	// re-basing the threshold would in turn re-base every historical figure.
	// So the number is kept as it is and its meaning is stated, once, in the
	// run's own log, immediately after it is printed. Judge such a leg on p95
	// and postWarmupMaxFrameMs instead.
	//
	// TWO-SIDED BY CONSTRUCTION: this fires only when p50 has actually crossed
	// the bar, and stays silent otherwise, so it is a statement about this leg
	// rather than a warning that must appear in every long-enough run.
	//
	// GREP: "HITCH COUNT IS NOT A SPIKE COUNT"
	if (PostWarmupN > 0 && PostWarmupP50 >= HitchThresholdMs)
	{
		UE_LOG(LogVoxelPerf, Warning,
		       TEXT("VoxelPerfRun: HITCH COUNT IS NOT A SPIKE COUNT ON THIS LEG. post-warmup p50=%.2fms is at ")
		       TEXT("or above hitchThresholdMs=%.1f, so the MEDIAN frame counts as a hitch and hitches=%d of ")
		       TEXT("%d frames mostly measures the frame rate, not spikes. NOT comparable to any hitch count ")
		       TEXT("taken when p50 was below the threshold. Judge this leg on p95 (%.2fms) and ")
		       TEXT("postWarmupMaxFrameMs (%.2fms)."),
		       PostWarmupP50, HitchThresholdMs, PostWarmupHitchCount, PostWarmupN, PostWarmupP95, PostWarmupMax);
	}

	// --- THE GI ARM, NEXT TO THE NUMBERS IT QUALIFIES -----------------------
	//
	// Same placement rule as the sky line below and for the same stated reason:
	// "a reader who has scrolled to the bottom for the p95 should not have to
	// scroll back up to find out which sun it was measured under".
	//
	// GREP: "VoxelPerfRun GI arm:"
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("VoxelPerfRun GI arm: subsystem=%d enabled=%d volume=%d sourceBricks=%d marchBricks=%d ")
	       TEXT("anchored=%d identityRefusals=%d -- %s"),
	       bHaveGI ? 1 : 0, GiEnabled, GiVolume, GiSourceBricks, GiMarchBricks, GiAnchored,
	       GiIdentityRefusals,
	       GiMarchBricks != 0
	           ? TEXT("P7 GPU CONE MARCH over the resident bricks. The CPU voxelize/solve/encode/upload ")
	             TEXT("chain is retired, so this leg and a control-arm leg differ in WHAT PRODUCED THE ")
	             TEXT("LIGHTING as well as in cost -- read the pair as a whole-frame A/B and nothing ")
	             TEXT("finer. v1 has no bounce and binary cones, so caves read darker; judge the picture ")
	             TEXT("on screenshots, never on this number")
	           : (GiSourceBricks != 0
	                  ? TEXT("P7-c MEASUREMENT ARM: the light field is fed by nothing. These frame times ")
	                    TEXT("describe a run with NO GI CONTENT; read them for streaming, never as a ")
	                    TEXT("lighting arm")
	                  : (GiEnabled != 0 && GiVolume != 0 && GiAnchored == 0
	                         ? TEXT("GI IS SWITCHED ON BUT WAS NEVER ANCHORED -- EnsureVolumeOrigin never ")
	                           TEXT("identified the terrain pool (P7-a), so this leg measures GI OFF while ")
	                           TEXT("its switches say on")
	                         : TEXT("arm as configured"))));

	// The arm this leg belongs to, in one line, next to the numbers it
	// qualifies. The per-window lines above already carry the same state, but a
	// reader who has scrolled to the bottom for the p95 should not have to
	// scroll back up to find out which sun it was measured under -- that is the
	// exact reading failure the JSON fields are there to prevent, and the log
	// is read far more often than the JSON.
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("VoxelPerfRun sky: tod=%02d:%02d date=%02d-%02d sunAlt=%.2f sunAz=%.2f timeScale=%.3f (sun %s)"),
	       SkyHour, SkyMinute, SkyMonth, SkyDay, SkyState.SunAltitudeDeg, SkyState.SunAzimuthDeg, SkyTimeScale,
	       SkyTimeScale == 0.f ? TEXT("FROZEN -- comparable to other frozen legs at the same tod/date")
	                           : TEXT("MOVING -- see the warning above"));

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
