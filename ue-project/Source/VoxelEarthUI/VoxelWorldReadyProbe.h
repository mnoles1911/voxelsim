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
	// 60 -> 300, 2026-09-07 (Phase 4). Set by the front end from
	// -VoxelLoadGateMaxWait; see that switch's comment for the owner directive
	// and for why the ceiling is no longer -VoxelLoadMaxHold. A gate that
	// always times out is not a gate, and at 60 s against a cold 8-ring
	// cascade this one always did.
	float MaxWaitSeconds = 300.0f;

	// GATE 3 (2026-09-07): "the fine tier's prefetch ring has settled". With
	// the async tile loader (-VoxelFineTileAsync=1) the ring's tiles arrive on
	// workers over the first seconds of the world, and gates 1 and 2 can both
	// pass while a neighbour tile is still in flight -- the curtain would lift
	// onto a world whose first step across a tile edge is a blocking load.
	// Asks UVoxelWorldSubsystem::IsFineRingSettled, which is true when there
	// is no fine tier at all and cannot be held by an unbaked neighbour (a
	// known-absent tile counts as settled). -VoxelLoadGateFineRing=0 switches
	// it off; MaxWaitSeconds still bounds it either way.
	bool bRequireFineRing = true;
};

struct VOXELEARTHUI_API FVoxelReadyProbeStatus
{
	int32 ProbeHits = 0;
	int32 ProbeTotal = 0;
	int32 PendingInGate = 0;
	int32 JobsInGate = 0;
	// Gate 3's n/m: ring tiles settled (resident, absent or refused) over the
	// ring's size. 0/0 until the residency tick has run once.
	int32 FineRingSettled = 0;
	int32 FineRingTotal = 0;
	bool bFineRingOk = false;
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

// Where the bar eases to and HOLDS while the artificial timer has run out but
// the world has not reported ready. Part of the reveal contract below, and
// deliberately under the hourglass grain emitter's 0.995 cut-off, so the sand
// keeps falling through the hold instead of freezing with the bar.
inline constexpr float kVoxelTheatreHoldProgress = 0.97f;

// The loading bar's progress model, as a pure function of its three inputs.
//
// THE BAR IS THEATRE, BY OWNER DIRECTIVE (2026-09-05). It no longer reports
// streaming work at all: on entering the loading screen the front end rolls an
// artificial 30-60 s duration, and the bar plays that duration out as a show
// -- SMOOTHLY, which the honest work-driven bar it replaces could not be,
// because ring fill moves in lurches and a warm cache pinned it at 99% for the
// whole hold. The world's actual readiness enters this model as exactly one
// bit, and only to gate the ending.
//
// THE REVEAL SEMANTICS, stated honestly: the world is revealed at
// max(artificial timer elapsed, world actually ready). A world that beats the
// timer waits behind the curtain while the theatre plays out -- the owner's
// explicit intent. A world SLOWER than the timer holds the bar at
// kVoxelTheatreHoldProgress (~97%) with the hourglass still animating, then
// completes when the gate opens. The bar never sits frozen at 100% and never
// moves backwards.
//
//   TheatreFraction  elapsed / rolled duration, clamped.
//   bWorldGateOpen   the ready probe passed (or timed out and the curtain is
//                    lifting anyway). While false, the return is capped at the
//                    hold value; 1.0 is reachable only once this is true --
//                    "100% while the world is still landing" stays the one lie
//                    this model refuses to tell, same as its predecessor.
//   PreviousProgress the last value returned. The monotone clamp, kept from
//                    the old model: a bar going backwards reads worse than one
//                    standing still.
//
// THE EASING IS SMOOTHSTEP (3t^2 - 2t^3), and the choice is load-bearing: its
// slope is ZERO at both ends, so the bar leaves 0% gently, lands on the ~97%
// hold with no visible speed discontinuity, and finishes without a snap --
// an eased curve reads better than linear, and this one makes the hold
// invisible as a transition. FREE AND PURE SO IT CAN BE TESTED; see
// VoxelFrontEndTests.cpp.
VOXELEARTHUI_API float ComputeTheatreProgress(float TheatreFraction, bool bWorldGateOpen, float PreviousProgress);

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
