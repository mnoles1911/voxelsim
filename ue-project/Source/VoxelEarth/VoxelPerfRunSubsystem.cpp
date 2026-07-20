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

		UE_LOG(LogVoxelPerf, Log, TEXT("VoxelPerfRun: scripted %.1fs flight requested."), DurationSeconds);
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

	// docs/debug-tooling-plan.md P1: "circle radius 100m around spawn at
	// 20 m/s at surface+30m, constant yaw sweep."
	const double AngularSpeedRadPerSec = LinearSpeedUUPerSec / CircleRadiusUU;
	const double Angle = double(ElapsedSeconds) * AngularSpeedRadPerSec;

	FVector NewLocation = CircleCenterUU;
	NewLocation.X += CircleRadiusUU * FMath::Cos(Angle);
	NewLocation.Y += CircleRadiusUU * FMath::Sin(Angle);
	NewLocation.Z = FixedHeightUU;

	const float Yaw = FMath::Fmod(ElapsedSeconds * float(YawSweepDegPerSec), 360.f);
	const FRotator NewRotation(-10.f, Yaw, 0.f);

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
		TEXT("  \"postWarmupHitchCount\": %d\n")
		TEXT("}\n"),
		DurationSeconds, N, P50, P95, Max, HitchCount, HitchThresholdMs, (long long)ChunksLoaded, AvgChunksPerSec,
		AvgBudgetSaturationPct, WarmupExcludeSeconds, PostWarmupN, PostWarmupP50, PostWarmupP95, PostWarmupMax, PostWarmupHitchCount);

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
