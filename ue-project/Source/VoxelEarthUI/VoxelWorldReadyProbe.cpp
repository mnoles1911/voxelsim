#include "VoxelWorldReadyProbe.h"

#include "VoxelEarthUI.h"
#include "VoxelFrontEndSwitches.h"

#include "VoxelDebug.h"          // FVoxelStreamingProgress
#include "VoxelWorldSubsystem.h"

#include "HAL/PlatformTime.h"

namespace VoxelReadyProbeDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// Per-ring weights for the progress bar's work term, normalised over whatever
// rings are actually gated. Front-loaded because that is where the player is
// standing: R0 finishing is most of what "the world is here" feels like, and
// R3 finishing is the last few percent of a bar nobody watches to the end.
const float kRingWeights[] = {0.35f, 0.25f, 0.20f, 0.20f, 0.10f, 0.10f};
} // namespace VoxelReadyProbeDetail

void FVoxelWorldReadyProbe::Start(const FVector& AnchorUU, const FVoxelReadyProbeConfig& InConfig)
{
	Config = InConfig;
	Anchor = AnchorUU;
	Status = FVoxelReadyProbeStatus();
	Status.ProbeTotal = Config.ProbeRadiiMeters.Num() * Config.ProbeDirectionCount;
	PollAccumulator = 0.f;
	bStarted = true;

	UE_LOG(LogVoxelUI, Log,
	       TEXT("VoxelLoadGate: started at (%.0f, %.0f) UU -- %d probes, rings R0..R%d, %d good sample(s) at %.1fs, ")
	       TEXT("max wait %.0fs."),
	       Anchor.X, Anchor.Y, Status.ProbeTotal, Config.GateMaxRingLevel, Config.RequiredGoodSamples,
	       Config.PollIntervalSeconds, Config.MaxWaitSeconds);
}

void FVoxelWorldReadyProbe::Tick(float DeltaSeconds, const UVoxelWorldSubsystem& World)
{
	if (!bStarted || Status.bReady || Status.bTimedOut)
	{
		return;
	}
	Status.ElapsedSeconds += DeltaSeconds;

	// RATE LIMITED, and not for tidiness. Each poll fills
	// FVoxelStreamingProgress, which walks every chunk record -- 39,020 of them
	// at a settled 4 km cascade. At 0.4 s that is negligible; per frame it
	// would be a self-inflicted hitch on the one screen whose whole job is to
	// hide hitches.
	PollAccumulator += DeltaSeconds;
	if (PollAccumulator < Config.PollIntervalSeconds)
	{
		return;
	}
	PollAccumulator = 0.f;
	Poll(World);

	if (Status.ElapsedSeconds >= Config.MaxWaitSeconds && !Status.bReady)
	{
		Status.bTimedOut = true;
		// A WARNING, DELIBERATELY, and greppable. This is the port of the
		// GDScript's push_warning on the same condition: the curtain is about
		// to lift on a world that never reported itself ready, and in a
		// headless run the log line is the only witness.
		UE_LOG(LogVoxelUI, Warning,
		       TEXT("VoxelLoadGate: TIMED OUT after %.1fs -- hits %d/%d, pending %d, jobs %d on R0..R%d. ")
		       TEXT("Lifting the curtain anyway."),
		       Status.ElapsedSeconds, Status.ProbeHits, Status.ProbeTotal, Status.PendingInGate, Status.JobsInGate,
		       Config.GateMaxRingLevel);
	}
}

void FVoxelWorldReadyProbe::Poll(const UVoxelWorldSubsystem& World)
{
	const double PollStart = FPlatformTime::Seconds();

	// --- Gate 1: is there ground all round the spawn? -----------------------
	int32 Hits = 0;
	for (const double RadiusMeters : Config.ProbeRadiiMeters)
	{
		const double RadiusUU = RadiusMeters * 100.0;
		for (int32 Direction = 0; Direction < Config.ProbeDirectionCount; ++Direction)
		{
			const double Angle = (2.0 * PI * double(Direction)) / double(Config.ProbeDirectionCount);
			const double X = Anchor.X + FMath::Cos(Angle) * RadiusUU;
			const double Y = Anchor.Y + FMath::Sin(Angle) * RadiusUU;
			// The analytic surface needs no streaming at all, so this asks
			// "where WOULD the ground be" and then asks whether the chunk
			// containing that point is actually drawn yet. Those are two
			// genuinely different questions and only the second is the gate.
			const double SurfaceZ = World.GetSurfaceHeightUU(X, Y);
			const FVector Probe(X, Y, SurfaceZ + Config.ProbeHeightAboveSurfaceM * 100.0);
			if (World.IsChunkPresentableAt(Probe))
			{
				++Hits;
			}
		}
	}
	Status.ProbeHits = Hits;

	// --- Gate 2: has the streamer stopped making things? --------------------
	const FVoxelStreamingProgress Progress = World.GetStreamingProgress();
	const int32 MaxRing = FMath::Clamp(Config.GateMaxRingLevel, 0, VoxelCoords::kNumLevels - 1);

	int32 Pending = 0;
	int32 Jobs = 0;
	float WeightedFill = 0.f;
	float WeightTotal = 0.f;
	for (int32 Level = 0; Level <= MaxRing; ++Level)
	{
		Pending += Progress.LevelPendingCount[Level];
		Jobs += Progress.LevelJobsInFlight[Level];

		const float Weight = Level < int32(UE_ARRAY_COUNT(VoxelReadyProbeDetail::kRingWeights))
		                         ? VoxelReadyProbeDetail::kRingWeights[Level]
		                         : 0.1f;
		const int32 Loaded = Progress.LevelLoadedCount[Level];
		const int32 Outstanding = Progress.LevelPendingCount[Level] + Progress.LevelJobsInFlight[Level];
		const int32 Denominator = FMath::Max(1, Loaded + Outstanding);
		WeightedFill += Weight * (float(Loaded) / float(Denominator));
		WeightTotal += Weight;
	}
	Status.PendingInGate = Pending;
	Status.JobsInGate = Jobs;
	Status.RingFillFraction = WeightTotal > 0.f ? WeightedFill / WeightTotal : 0.f;

	// --- Both gates, sustained ----------------------------------------------
	const bool bSpatialOk = (Status.ProbeTotal > 0) && (Hits >= Status.ProbeTotal);
	const bool bStreamerIdle = Progress.bSessionStarted && Pending == 0 && Jobs == 0;
	if (bSpatialOk && bStreamerIdle)
	{
		++Status.ConsecutiveGood;
	}
	else
	{
		// RESET, not decrement. The rule is "quiet for 1.2 s continuously",
		// and a streamer that goes idle, wakes, and goes idle again has not
		// been quiet at all.
		Status.ConsecutiveGood = 0;
	}

	Status.LastPollMs = float((FPlatformTime::Seconds() - PollStart) * 1000.0);

	if (Status.ConsecutiveGood >= Config.RequiredGoodSamples)
	{
		Status.bReady = true;
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelLoadGate: READY after %.2fs (hits %d/%d, %d chunk(s) tracked)."),
		       Status.ElapsedSeconds, Hits, Status.ProbeTotal, Progress.TrackedChunks);
		return;
	}

	if (FVoxelFrontEndSwitches::Get().bReadyProbeLog)
	{
		UE_LOG(LogVoxelUI, Log,
		       TEXT("VoxelLoadGate: t=%.1fs hits=%d/%d pending(R0..R%d)=%d jobs=%d fill=%.2f consec=%d/%d poll=%.2fms"),
		       Status.ElapsedSeconds, Hits, Status.ProbeTotal, MaxRing, Pending, Jobs, Status.RingFillFraction,
		       Status.ConsecutiveGood, Config.RequiredGoodSamples, Status.LastPollMs);
	}
}
