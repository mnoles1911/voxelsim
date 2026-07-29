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
	// Straight-line traverse, not a loop. Surface (and underground) walk a
	// closed 100 m circle, so after one lap every column the flight visits is
	// already loaded -- neither can reproduce a bug that only shows up on
	// CONTINUOUS entry into virgin terrain (a chunk boundary crossed once,
	// streamed in, and never crossed again). Line exists for exactly that: fly
	// a straight heading from the captured origin for the whole run, so the
	// desired set keeps admitting new chunks instead of eventually re-treading
	// ground the streamer already holds resident.
	Line,
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

	// Emits the periodic "Voxel sky (Ns window)" line on the -VoxelPerfLogInterval
	// cadence. See SkyLogIntervalSec below for why this line lives in the perf
	// subsystem rather than in UVoxelSkySubsystem::Tick.
	void MaybeLogSky(float DeltaTime);

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

	// -VoxelPerfStaticAt=X,Y,Z (UU). Pins somewhere other than the spawn column.
	// The spawn is always on the surface, so without this the static fixture
	// cannot be aimed at the only voxel water this world has, which is
	// underground. See the parse site for the full reasoning.
	bool bStaticLocationOverride = false;
	FVector StaticLocationOverrideUU = FVector::ZeroVector;

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

	// -VoxelPerfHeading=<degrees>, line flight only. 0 = +X, 90 = +Y -- the
	// ordinary FRotator::Yaw convention, so the pawn's facing (yaw = this
	// value, see StepFlightPath) and its direction of travel agree by
	// construction rather than needing to be kept in sync by hand. Defaults to
	// +X: an arbitrary but stable choice that needs no per-world knowledge
	// (unlike e.g. "toward the nearest unexplored region").
	float HeadingDeg = 0.f;

	// Line flight only: rate-limits how fast the ground-following Z target
	// (see StepFlightPath) is allowed to move the pawn, expressed as a
	// multiple of horizontal speed rather than an absolute m/s figure so the
	// steepest "slope" the fixture can track scales with -VoxelPerfSpeed
	// instead of going stale relative to it. Without this a cliff or a chunk
	// seam's height discontinuity would teleport the pawn's Z in one frame --
	// exactly the kind of frame the p95/hitch stats would then (wrongly)
	// blame on streaming rather than on the fixture's own path math.
	static constexpr double LineMaxZSpeedMultiplier = 2.0;

	// Line flight only: the Z the pawn was actually placed at last tick, so
	// this tick's ground-following target can be approached at the
	// LineMaxZSpeedMultiplier-clamped rate instead of jumping straight to it.
	// Seeded from FixedHeightUU (surface-at-origin + HeightAboveSurfaceUU) on
	// path init, so frame 1 does not itself pop.
	double LineLastZUU = 0.0;

	// -VoxelPerfPreflightSec=<seconds>, default 0 (no change from prior
	// behaviour: the flight starts advancing on the very first tick a pawn
	// exists). The smoke run showed the flight starting on a completely COLD
	// world, which confounds the thing being investigated (streaming-lag
	// holes) with plain cold-fill for the first ~40s -- there was no way to
	// let the cascade warm up around a stationary pawn before the flight
	// itself starts moving and generating fresh crossings. PreflightSec>0
	// holds the pawn at its captured spawn pose (PreflightLocationUU/
	// PreflightRotation below) for that many seconds before the flight path
	// clock starts.
	//
	// The path clock genuinely starts at zero when preflight ends -- Step-
	// FlightPath computes path position from (ElapsedSeconds - PreflightSec),
	// not from ElapsedSeconds directly -- so e.g. a line flight's start point
	// is still exactly the captured origin, not partway down its heading.
	//
	// Same frame-time-samples caveat as LingerSec: preflight frames are still
	// folded into FrameMsSamples/PostWarmupFrameMsSamples (the world is
	// ticking, so frame time is still frame time), so two legs compared on
	// frame-time metrics must use the same PreflightSec or the comparison is
	// contaminated by however much warmup got averaged in on one side.
	float PreflightSec = 0.f;

	// One-time "entering preflight" log guard -- see StepFlightPath. Position/
	// rotation themselves are NOT captured lazily like this: they are set
	// once in Tick's path-init block, in the exact same frame CircleCenterUU/
	// FixedHeightUU are, specifically so nothing re-captures a pose when the
	// flight begins and the traverse still starts from the true spawn.
	bool bPreflightLogged = false;
	FVector PreflightLocationUU = FVector::ZeroVector;
	FRotator PreflightRotation = FRotator::ZeroRotator;

	// -VoxelPerfLingerSec=<seconds>, default 0 (no change from prior
	// behaviour: FinishRun fires the instant ElapsedSeconds reaches
	// PreflightSec + DurationSeconds, i.e. as soon as the flight itself ends).
	// The ring-gap investigation's signature is "holes
	// appear while moving, fill in once you stop", and with LingerSec==0
	// there is no way to observe that: RequestExit fires the moment the
	// flight itself stops. LingerSec>0 keeps the process alive (StepFlightPath
	// pins the pose, re-asserted every tick like EVoxelPerfFlight::Static)
	// for that many extra seconds AFTER the flight ends, so the periodic
	// LogVoxelPerf streaming lines -- which come from
	// FVoxelWorldImpl::TickStreaming, not this subsystem, and keep flowing
	// regardless of what this subsystem does -- are on the record across the
	// fly-then-stop transition.
	//
	// CAVEAT, applies to whoever next reuses this switch: linger frames are
	// still sampled into FrameMsSamples/PostWarmupFrameMsSamples same as any
	// other frame (see Tick) -- the pawn isn't moving, but the world is still
	// ticking and frame time is still frame time. Two legs being compared on
	// FRAME-TIME metrics (p50/p95/hitchCount/...) must use the same
	// LingerSec or the comparison is contaminated by how much quiescent-pinned
	// time got averaged in on one side and not the other. The ring-gap legs
	// this switch was built for only read the periodic streaming log lines,
	// not this summary's frame stats, so they don't care -- the next use
	// might.
	float LingerSec = 0.f;

	// Linger-phase pinned pose -- same re-assert-every-tick pattern as
	// EVoxelPerfFlight::Static's StaticLocationUU/bStaticPoseCaptured, just
	// captured once at the moment the flight ENDS rather than at run start.
	bool bLingerPoseCaptured = false;
	FVector LingerLocationUU = FVector::ZeroVector;
	FRotator LingerRotation = FRotator::ZeroRotator;

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

	// Cadence for the periodic "Voxel sky (Ns window)" line, read once from
	// -VoxelPerfLogInterval= in Initialize so it lands in the SAME windows
	// FVoxelWorldImpl::MaybeLogCounters (VoxelWorldSubsystem.cpp:5005-5024)
	// puts chunksPerSec= in -- tools/voxel-leg-summary.ps1 reads both out of
	// one log and a second, unrelated cadence would make "which window" an
	// extra thing to get wrong.
	//
	// WHY THIS LINE IS EMITTED HERE AND NOT FROM UVoxelSkySubsystem::Tick. It
	// is a perf-harness instrument: its cadence is a perf-harness switch, it
	// only has a reader while -VoxelPerfRun= is on the command line, and the
	// sky subsystem has no business knowing about either. Emitting it here also
	// makes it exactly zero cost in a normal session, since IsTickable() is
	// false without -VoxelPerfRun=.
	float SkyLogIntervalSec = 5.f;
	float SkyLogAccumSec = 0.f;

	TArray<float> FrameMsSamples;
	int32 HitchCount = 0;
	TArray<float> PostWarmupFrameMsSamples;
	int32 PostWarmupHitchCount = 0;
	float BudgetSaturationAccum = 0.f;
	int32 BudgetSaturationSamples = 0;
};
