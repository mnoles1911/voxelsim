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
// Which scripted path -VoxelPerfRun drives.
//
// Surface is the shipped M1 gate flight and its numbers are the ones every
// perf baseline in docs/status.md was taken against, so it stays the default
// and its geometry is not touched by the underground addition.
//
// Underground exists because PR #80 shipped a 64 m sight sphere whose cost
// under a MOVING anchor was never measured. The exit scan in
// RecomputeDesiredSet is O(ChunkRecords) and runs on every level-0 anchor
// chunk crossing; underground that record set is ~2.7x the surface flight's,
// and no fixture in the tree moved an underground anchor at all (the cavern
// shot settles and stands still). This is that fixture.
enum class EVoxelPerfFlight : uint8
{
	Surface,
	Underground,
	// Neither a flight nor a fixture that merely "stands still" -- it PINS the
	// pose, every frame, and logs it.
	//
	// Exists because the surface and underground flights cannot answer "is
	// renderer A faster than renderer B". They deliberately move, so the
	// streaming load differs run to run, and the resulting spread swamps the
	// difference being looked for: twelve 60 s legs comparing the pooled and
	// component renderers produced overlapping ranges and a retracted result
	// (docs/streaming-handoff.md, "CORRECTION: the G5 frame-time numbers were
	// noise").
	//
	// The obvious repair -- stand still and let the cascade settle, so
	// streaming is quiescent -- was tried WITHOUT pinning the pose, and failed
	// the same way: the component path rendered an identical settled scene at
	// 43 fps in one run and 103 fps in another. The anchor is a POSITION; an
	// unattended pawn's yaw and pitch are neither pinned nor recorded, and a
	// camera facing sky versus facing down a valley is exactly that size of
	// effect.
	//
	// So this mode fixes position AND rotation, holds them against anything
	// else that might move the pawn, and logs the pose in the summary so the
	// assumption is checkable rather than assumed. That makes the renderer the
	// only variable between two runs, which is the whole point.
	Static,
};

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

	// -VoxelPerfFlight=surface|underground. Surface is the default so every
	// existing invocation in docs/status.md keeps meaning what it meant.
	EVoxelPerfFlight Flight = EVoxelPerfFlight::Surface;

	// Static-mode pose. -VoxelPerfYaw= / -VoxelPerfPitch=. The defaults look
	// slightly down and along +X, which at the reference spawn puts terrain
	// across the whole frame rather than sky.
	float StaticYawDeg = 0.f;
	float StaticPitchDeg = -15.f;
	bool bStaticPoseCaptured = false;
	FVector StaticLocationUU = FVector::ZeroVector;

	// -VoxelPerfDepth=<metres>, underground flight only. 60 m is chosen to
	// match the documented test cavern's depth (60.7 m down), which is the one
	// underground scene in the tree with a measured residency result to compare
	// against. Deep enough that the surface band and the depth skirt are far
	// overhead, so the deep box is genuinely carrying the record set rather
	// than overlapping something the surface path would have admitted anyway.
	static constexpr double DefaultDepthM = 60.0;
	double DepthUU = DefaultDepthM * 100.0;

	// -VoxelPerfSpeed=<m/s>. Defaults to the surface flight's own 20 m/s
	// rather than a walking pace, deliberately: the point of this fixture is an
	// A/B against the surface flight where DEPTH is the only variable. A slower
	// underground run would confound "underground is cheaper/dearer" with
	// "fewer chunk crossings per second", since the exit scan's frequency is
	// set by crossings and its cost by record count. A realistic walk is a
	// separate run at -VoxelPerfSpeed=6.
	double LinearSpeedUUPerSecOverride = LinearSpeedUUPerSec;

	// Underground flight: the last Z we actually placed the pawn at, logged
	// alongside the surface height so a run can be checked for having been
	// underground at all rather than trusted to have been. A fixture that
	// silently surfaces would report the surface flight's numbers under an
	// underground label, which is the exact false-pass shape two sibling
	// probes in this tree already produced.
	double LastPlacedZUU = 0.0;
	double LastSurfaceZUU = 0.0;
	int32 UndergroundFrames = 0;
	int32 TotalPathFrames = 0;

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
