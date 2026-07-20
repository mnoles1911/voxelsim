#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelDebug.h" // VoxelDebug::kHitchThresholdMs -- shared hitch-frame threshold
#include "VoxelPerfRunSubsystem.generated.h"

// docs/debug-tooling-plan.md P1 "Regression harness (the gates, automated)":
// -VoxelPerfRun=<seconds> command line switch drives a scripted flight (fly
// mode, fixed circle around spawn) for the given duration, sampling
// per-frame frame time, then writes an end-of-run JSON summary to
// Saved/PerfRuns/perf_<timestamp>.json and exits -- this is the artifact a
// checker script (tools/check-perf-run.py) asserts thresholds against, and
// the mechanism a future CI/-game headless run uses as the M1/M2 perf gate.
//
// Entirely inert (early-outs in Initialize, IsTickable() false) unless
// -VoxelPerfRun is present on the command line -- zero cost in normal play.
UCLASS()
class VOXELEARTH_API UVoxelPerfRunSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	//~ End USubsystem Interface

	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

	//~ Begin FTickableGameObject / UTickableWorldSubsystem Interface
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;
	//~ End FTickableGameObject / UTickableWorldSubsystem Interface

private:
	// Moves/orients the pawn along the scripted circle path this tick, once
	// the pawn (and hence the circle's center) has been captured.
	void StepFlightPath(float DeltaTime);

	// Computes final stats, writes the JSON summary, logs it, and requests
	// process exit.
	void FinishRun();

	bool bRequested = false; // -VoxelPerfRun was present on the command line
	bool bFinished = false;
	float DurationSeconds = 0.f;
	float ElapsedSeconds = 0.f;

	bool bPathInitialized = false;
	FVector CircleCenterUU = FVector::ZeroVector;
	double FixedHeightUU = 0.0;

	// docs/debug-tooling-plan.md P1: "circle radius 100m ... at 20 m/s ...
	// constant yaw sweep."
	static constexpr double CircleRadiusUU = 10000.0;      // 100m
	static constexpr double LinearSpeedUUPerSec = 2000.0;  // 20 m/s
	static constexpr double HeightAboveSurfaceUU = 3000.0; // surface + 30m
	static constexpr double YawSweepDegPerSec = 60.0;

	// Shared with FVoxelWorldImpl::TickStreaming's per-frame hitch-attribution
	// log (VoxelDebug.h) so both never disagree about which frames count as a
	// hitch (docs/status.md "Perf-run hitches" isolation task).
	static constexpr float HitchThresholdMs = VoxelDebug::kHitchThresholdMs;

	// docs/status.md "Perf-run hitches" isolation task / M1 gate note: "if the
	// ramp itself still hitches but steady-state is clean, report that
	// honestly ... with the steady-state window numbers proving it." Frames
	// with ElapsedSeconds >= this are also folded into a SEPARATE
	// post-warmup sample set (below), reported as its own p95/hitch-count in
	// the JSON summary/log -- this is what lets a "cold-start-only" hitch
	// pattern be told apart from a genuine, ongoing steady-state one without
	// re-running anything.
	static constexpr float WarmupExcludeSeconds = 10.0f;

	TArray<float> FrameMsSamples;
	int32 HitchCount = 0;
	TArray<float> PostWarmupFrameMsSamples;
	int32 PostWarmupHitchCount = 0;
	float BudgetSaturationAccum = 0.f;
	int32 BudgetSaturationSamples = 0;
};
