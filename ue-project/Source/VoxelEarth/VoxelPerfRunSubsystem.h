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

	// Fires the shutter if this frame's PLACED pose crossed a distance boundary
	// along the line flight. Called from StepFlightPath's Line branch with the
	// pose the pawn was ACTUALLY placed at this tick -- not the pose the path
	// math wanted -- because the frame about to be drawn is the frame at the
	// pose the pawn is at now. Disarmed (ShotEveryUU == 0) it is one compare
	// and a return. See its definition for why the trigger is distance and not
	// the clock.
	void MaybeFireMovingShot(const FVector& PlacedLocationUU, const FRotator& PlacedRotation);

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
	// CAMERA HEIGHT ABOVE THE SURFACE, and it is a member rather than a constant
	// because the pose decides the measurement.
	//
	// 30 m was fine while every leg measured the DRAW path, where what matters is
	// how much geometry is in frame. It is not fine for the ray-march spike: at
	// 30 m up, 58.7% of rays MISS (they cross the volume into sky), and
	// empty-space skipping helps misses 7.8x against 4.6x for hits -- so an
	// elevated pose flatters the skip ratio that the whole marcher decision rests
	// on. A player's camera sits near eye height, where the miss population
	// collapses. -VoxelPerfHeightM= exists so that pose can be measured.
	//
	// NOTE it is NOT -VoxelPerfDepth (flight-path depth, no effect on a pinned
	// static pose) and NOT -VoxelSpawnAltM (spawn altitude, which this overwrites
	// on the first path-init tick). Both were tried; both silently produced the
	// 30 m pose again, with an identical census, which is exactly the kind of
	// null result that reads as "the experiment ran".
	double HeightAboveSurfaceUU = 3000.0; // surface + 30m unless overridden
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

	// ========================================================================
	// MOVING CAPTURES -- -VoxelPerfShotEveryM=<metres>, 0 = off, the default.
	// ========================================================================
	//
	// WHY THIS EXISTS. Every capture this project takes settles first: the
	// camera is parked, the cascade is allowed to converge for ~120 s, and only
	// then does the shutter fire. That protocol is what makes two arms
	// comparable -- and it is also, structurally, why two live decisions cannot
	// be settled at all:
	//
	//   * docs/outer-ring-stagger-2026-08-28.md, "WHAT NO INSTRUMENT HERE CAN
	//     SHOW": the stagger arm trades a transient far-field lag WHILE MOVING
	//     for a better worst frame. A settled capture lets both arms converge
	//     on the same world well before the shutter, so the parked A/B is
	//     "structurally incapable of showing the thing this change trades
	//     away". That arm is parked on exactly this.
	//   * The half-res marcher was rejected for being "grainy at distance while
	//     moving" -- a verdict nobody could photograph, so nobody could check
	//     it, argue with it, or measure a cure against it.
	//
	// The project's own limitation note reads "no moving-capture capability
	// exists". This is that capability.
	//
	// WHY THE SHUTTER TRIGGERS ON DISTANCE TRAVELLED AND NOT ON THE CLOCK: see
	// UVoxelPerfRunSubsystem::MaybeFireMovingShot, where the trigger lives. It
	// is the one design decision that makes these frames evidence rather than
	// pictures, so the argument is written at the site that acts on it.
	//
	// A LEG CARRYING MOVING CAPTURES IS AN IMAGE LEG, AND ITS FRAME TIMES ARE
	// NOT A TIMING RESULT. A shutter stalls the frame it is serviced on, so a
	// moving capture perturbs the very timing it is flying through. That is a
	// rule, not a caveat -- see the arming Warning in Initialize, the
	// end-of-run restatement in FinishRun, and frameTimingAdmissible in the
	// summary JSON.
	//
	// LINE FLIGHT ONLY. Armed on any other flight this ABORTS the run rather
	// than flying it unshot; the refusal and its reasoning are in Initialize.

	// The step, in UU. 0 means disarmed, which is the default, and is why every
	// existing leg is byte-identical: nothing below this line runs.
	double ShotEveryUU = 0.0;

	// -VoxelPerfShotStartM=<metres>, default 0 -- the distance the FIRST shot
	// fires at. Exists so a leg can skip the opening stretch, where the pawn is
	// still flying out of the bubble -VoxelPerfPreflightSec= warmed around the
	// spawn and the far field is not yet in the regime being judged.
	double ShotStartUU = 0.0;

	// -VoxelPerfShotMaxCount=<n>, default 32. A 300 s line leg at 20 m/s covers
	// 6 km; at a 128 m step that is 47 shots, and a 2560x1440 PNG is a few MB.
	// The cap is here so an over-eager step cannot quietly fill the disk partway
	// through a leg that then dies for an unrelated-looking reason.
	int32 ShotMaxCount = 32;

	// -VoxelPerfShotName=<tag>, sanitised at the parse site. Goes in the
	// filename so two arms' shots can share one directory and still be told
	// apart by a comparer. Sanitised for the reason
	// VoxelSkyLadderFixture.cpp:743 sanitises its arm value -- a screenshot
	// basename with a '.' in it is an extension waiting to be misparsed by
	// every tool downstream -- and, additionally, because this is the ONE
	// operator-controlled string that reaches the summary JSON, whose writer
	// documents itself as having "no user-controlled strings to escape".
	FString ShotTag;

	// Counted at the site that acts (MaybeFireMovingShot) and reported in
	// FinishRun. "Armed every 512 m and fired 0" is a loud line there, not
	// silence: an armed-but-inert image leg that says nothing gets read as
	// "the change is invisible", which is the most expensive way to be wrong
	// about a renderer.
	int32 ShotsFired = 0;

	// Index of the NEXT nominal boundary to shoot; boundary i is at
	// ShotStartUU + i * ShotEveryUU. Monotone and never rewound, so no boundary
	// can be shot twice however the path's Z-follower jitters.
	int32 NextShotIndex = 0;

	// Loud-inertness evidence: how far the anchor actually got from the flight
	// origin. If shots are armed every 512 m and this says the leg never passed
	// 300 m, that is the ANSWER to "why did nothing fire", and it belongs in the
	// log rather than in the head of whoever launched the leg.
	double MaxDistanceReachedUU = 0.0;

	// Boundaries the path stepped straight over with no frame landing in them.
	// One frame at 20 m/s covers 0.3-2 m, so this is only reachable with a step
	// smaller than a frame's travel or with a multi-second hitch. Counted
	// because a boundary SKIPPED on one arm and HIT on the other is precisely
	// the asymmetry that makes two shot lists disagree -- and the slower arm is
	// the one that skips, which is the direction that would flatter it.
	int32 ShotBoundariesSkipped = 0;

	// THE ONE-FRAME BRACKET.
	//
	// FScreenshotRequest only RAISES A FLAG; the viewport services it at the end
	// of a subsequent draw (VoxelSkyLadderFixture.cpp:115-121). So the pose
	// logged when the shutter is REQUESTED is a LOWER BOUND on the pose actually
	// drawn, and the true one lies between it and the pose one frame later.
	// Rather than assume that gap is zero -- a join computed instead of checked,
	// which is the shape of a well-worn family of bugs in this tree -- the tick
	// after a request logs the pose again, so the bracket is on the record and a
	// reader can see how wide it was on THIS leg instead of trusting that it is
	// narrow.
	bool bShotPending = false;
	int32 PendingShotNominalM = 0;
	FVector PendingShotLocationUU = FVector::ZeroVector;

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
