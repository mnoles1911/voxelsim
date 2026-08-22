#pragma once
// "Is enough of the world here to show it to the player?"
//
// THE PORT OF A TWO-GATE RULE. CopperIslesTestBootstrap.gd answers this with
// two independent tests that must BOTH hold for three consecutive polls:
//
//   Gate 1, spatial: 7 radii x 16 directions = 112 downward probes around the
//   spawn, every one of which must find ground.
//   Gate 2, streamer idle: the queue must be near empty.
//
// Both gates matter, and for different reasons. Gate 2 alone passes during the
// brief lull before the streamer has been ASKED for anything -- which on a
// cold start is the very first frame. Gate 1 alone passes as soon as the
// surface exists even if half the visible ring is still meshing. Together they
// mean "the ground is there AND nothing is still being made".
//
// WHAT CHANGED IN TRANSLATION. Gate 1 in Godot is 112 physics raycasts. That
// does not port: terrain in THIS project has no Chaos collision, so a physics
// ray would hit nothing at all, and the voxel-space raycast that does exist
// warns in its own comments that it reports a clean miss "straight through
// solid rock" when nothing is streamed -- i.e. it cannot distinguish absent
// ground from absent DATA, which is the entire question here.
// UVoxelWorldSubsystem::IsChunkPresentableAt can, so the probe asks that
// instead, at a point just above the analytic surface height of each column.
//
// Gate 2 becomes "pending and in-flight are both zero on every gated ring",
// which is the settle rule docs/manual-verification-checklist.md already
// states in prose.

#include "CoreMinimal.h"

class UVoxelWorldSubsystem;

struct VOXELEARTHUI_API FVoxelReadyProbeConfig
{
	// Verbatim from CopperIslesTestBootstrap.gd's PROBE_RADII_M. All seven lie
	// inside R2's 256 m outer edge, so gate 1 is an R0-R2 statement by
	// construction -- which is why GateMaxRingLevel below is about gate 2.
	TArray<double> ProbeRadiiMeters = {20.0, 50.0, 90.0, 130.0, 170.0, 210.0, 250.0};
	int32 ProbeDirectionCount = 16;   // 7 x 16 = 112 probes
	// How far above the analytic surface each probe sits. Far enough not to
	// land inside the surface voxel itself, close enough to be in the same
	// chunk as the ground.
	double ProbeHeightAboveSurfaceM = 1.0;

	// WHICH RINGS GATE 2 REQUIRES. Three is a HYPOTHESIS, not a measurement,
	// and the difference matters in this codebase.
	//
	// The reasoning: a full 4 km cascade settles in 80-86 s cold at 39,020
	// chunks (VoxelWorldSubsystem.h records both numbers), so gating on R5
	// against a 60 s maximum hold would ALWAYS take the timeout path -- a gate
	// that never passes is not a gate. R4 and R5 cover 1-4 km, ground
	// AVoxelClipmapActor already draws as a heightfield out to ~30 km, so the
	// visual difference from waiting on them is distant detail and the cost is
	// a minute of staring. Gate 1's radii stop inside R2, so R3 is one ring of
	// margin, which keeps the first thing a player sees from being a ring
	// boundary popping in.
	//
	// -VoxelLoadGateMaxRing sweeps it; the measurement belongs in
	// docs/measurements/ before this comment claims anything stronger.
	int32 GateMaxRingLevel = 3;

	// 3 polls at 0.4 s = 1.2 s sustained. Filters the single-frame races that
	// a bare "is it zero right now" test would trip over.
	int32 RequiredGoodSamples = 3;
	float PollIntervalSeconds = 0.4f;
	float MaxWaitSeconds = 60.0f;
};

struct VOXELEARTHUI_API FVoxelReadyProbeStatus
{
	int32 ProbeHits = 0;
	int32 ProbeTotal = 0;
	int32 PendingInGate = 0;
	int32 JobsInGate = 0;
	int32 ConsecutiveGood = 0;
	float ElapsedSeconds = 0.f;
	// 0..1, how much of the gated rings is drawn. The progress bar's work term.
	float RingFillFraction = 0.f;
	// What the poll itself cost. Logged rather than assumed, because filling
	// FVoxelStreamingProgress walks every chunk record and that number is
	// 39,020 at settle.
	float LastPollMs = 0.f;
	bool bReady = false;
	bool bTimedOut = false;
};

// The loading bar's progress model, as a pure function of its four inputs.
//
// FREE AND PURE SO IT CAN BE TESTED. It has three invariants that are easy to
// state, easy to break, and invisible in a screenshot -- monotone, floored by
// elapsed time, and never 1.0 -- and each of them exists because the Godot
// original's bar lied in a specific way. See VoxelFrontEndTests.cpp.
//
//   TimeFraction   elapsed / MaxHold, clamped. The floor: a work-only bar can
//                  sit on one number for tens of seconds while an R3 tail
//                  drains, and a bar that never moves reads as a hang.
//   SpatialFraction probe hits / probe total.
//   RingFillFraction weighted loaded/(loaded+outstanding) over the gated rings.
//   PreviousProgress the last value returned. The monotone clamp: the desired
//                  set GROWS as the anchor settles, so the raw ratio genuinely
//                  decreases, and a bar going backwards reads worse than one
//                  standing still.
//
// Returns at most 0.995. Reaching 1.0 is the caller's job, and only when the
// gate actually passes -- "100% because a timer expired" is the one lie this
// whole model exists to avoid.
VOXELEARTHUI_API float ComputeLoadProgress(float TimeFraction, float SpatialFraction, float RingFillFraction,
                                           float PreviousProgress);

class VOXELEARTHUI_API FVoxelWorldReadyProbe
{
public:
	// AnchorUU is the spawn column the probes ring. Passed in rather than read
	// from the pawn, because the pawn does not exist yet when the first poll
	// runs -- that is the same reason
	// UVoxelWorldSubsystem::SetStreamingAnchorOverride exists.
	void Start(const FVector& AnchorUU, const FVoxelReadyProbeConfig& InConfig);
	// Internally rate-limited to PollIntervalSeconds; safe to call every frame.
	void Tick(float DeltaSeconds, const UVoxelWorldSubsystem& World);

	const FVoxelReadyProbeStatus& GetStatus() const { return Status; }
	bool IsReady() const { return Status.bReady; }
	bool HasTimedOut() const { return Status.bTimedOut; }

private:
	void Poll(const UVoxelWorldSubsystem& World);

	FVoxelReadyProbeConfig Config;
	FVoxelReadyProbeStatus Status;
	FVector Anchor = FVector::ZeroVector;
	float PollAccumulator = 0.f;
	bool bStarted = false;
};
