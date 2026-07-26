#include "VoxelPerfRunSubsystem.h"

#include "VoxelDebug.h"
#include "VoxelWorldSubsystem.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

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
			else if (!FlightName.Equals(TEXT("surface"), ESearchCase::IgnoreCase))
			{
				// Refuse rather than silently flying the default path: a typo'd
				// flight name that quietly ran the surface circle would produce
				// a perfectly plausible JSON summary labelled as the run you
				// thought you asked for.
				UE_LOG(LogVoxelPerf, Error,
				       TEXT("VoxelPerfRun: unknown -VoxelPerfFlight=%s (expected 'surface', 'underground' or 'static'). Aborting run."),
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

		UE_LOG(LogVoxelPerf, Log, TEXT("VoxelPerfRun: scripted %.1fs flight requested (flight=%s, depth=%.0fm, speed=%.0fm/s)."),
		       DurationSeconds, Flight == EVoxelPerfFlight::Underground ? TEXT("underground") : TEXT("surface"),
		       DepthUU / 100.0, LinearSpeedUUPerSecOverride / 100.0);
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
	if (ElapsedSeconds >= DurationSeconds)
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

	// docs/debug-tooling-plan.md P1: "circle radius 100m around spawn at
	// 20 m/s at surface+30m, constant yaw sweep."
	const double AngularSpeedRadPerSec = LinearSpeedUUPerSecOverride / CircleRadiusUU;
	const double Angle = double(ElapsedSeconds) * AngularSpeedRadPerSec;

	FVector NewLocation = CircleCenterUU;
	NewLocation.X += CircleRadiusUU * FMath::Cos(Angle);
	NewLocation.Y += CircleRadiusUU * FMath::Sin(Angle);
	NewLocation.Z = FixedHeightUU;

	const float Yaw = FMath::Fmod(ElapsedSeconds * float(YawSweepDegPerSec), 360.f);
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
		    : (Flight == EVoxelPerfFlight::Static ? TEXT("static") : TEXT("surface")), DepthUU / 100.0,
		LinearSpeedUUPerSecOverride / 100.0, StaticYawDeg, StaticPitchDeg, UndergroundFraction);

	FFileHelper::SaveStringToFile(Json, *OutPath);

	UE_LOG(LogVoxelPerf, Log,
	       TEXT("VoxelPerfRun complete: frames=%d p50=%.2fms p95=%.2fms max=%.2fms hitches=%d chunksLoaded=%lld ")
	       TEXT("avgChunks/s=%.2f budgetSat=%.1f%% -- wrote %s"),
	       N, P50, P95, Max, HitchCount, (long long)ChunksLoaded, AvgChunksPerSec, AvgBudgetSaturationPct, *OutPath);

	UE_LOG(LogVoxelPerf, Log,
	       TEXT("VoxelPerfRun post-warmup (t>=%.0fs): frames=%d p50=%.2fms p95=%.2fms max=%.2fms hitches=%d"), WarmupExcludeSeconds,
	       PostWarmupN, PostWarmupP50, PostWarmupP95, PostWarmupMax, PostWarmupHitchCount);

	FPlatformMisc::RequestExit(/*bForce*/ false);
}
