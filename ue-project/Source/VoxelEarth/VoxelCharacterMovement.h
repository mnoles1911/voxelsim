#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "VoxelMovementTuning.h"
#include "VoxelCharacterMovement.generated.h"

class UVoxelWorldSubsystem;

// Kinematic character mover for the voxel world (docs/m1-plan.md stage 3b,
// plan SS3.3 "no Chaos for terrain").
//
// Extracted from AVoxelEarthFlyPawn, which previously owned this logic inline.
// The split exists because the mover has three future consumers the pawn could
// never serve: AVoxelAgent (NPCs walking the same terrain), the M3
// authoritative server, and any future player pawn -- and because the pawn was
// already 800+ lines of two unrelated concerns welded together.
//
// WHY NOT UCharacterMovementComponent: it resolves collision through Chaos
// primitives, and voxel terrain carries no Chaos collision at all by design.
// Collision here is an axis-separated swept AABB against
// UVoxelWorldSubsystem::IsSolidAtVoxel -- the same solidity query dig/place
// uses -- so the mover and the world agree by construction.
//
// This is CLIENT PRESENTATION ONLY. The determinism boundary covers world
// DERIVATION, not player motion, so plain doubles are correct here and nothing
// in this file feeds a digest.
//
// The owning actor drives it: push input with SetMoveInput/SetSprintHeld/
// SetCrouchHeld/RequestJump, then call TickMovement once per frame. The
// component moves its owner directly via SetActorLocation and never ticks
// itself, so all movement timing stays in one place in the pawn's Tick.
UCLASS(ClassGroup = (VoxelEarth), meta = (BlueprintSpawnableComponent))
class VOXELEARTH_API UVoxelCharacterMovementComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UVoxelCharacterMovementComponent();

	// --- Input (pushed by the owning pawn) ---------------------------------

	// Cached WASD / Space-Ctrl axis values. Up is only consulted while
	// swimming; on land Space is a jump event, not an axis.
	void SetMoveInput(float Forward, float Right, float Up);
	void SetSprintHeld(bool bHeld) { bSprintHeld = bHeld; }
	void SetCrouchHeld(bool bHeld) { bCrouchHeld = bHeld; }

	// Jump press/release. Press is BUFFERED (see JumpBufferSeconds) rather
	// than consumed immediately, so a jump entered just before landing still
	// fires; release cuts a rising jump short (variable jump height).
	void RequestJump();
	void ReleaseJump();

	// Steps the speed dial (Delta = +1 / -1), clamped to the tier table.
	void AdjustSpeedTier(int32 Delta);

	// --- Simulation ---------------------------------------------------------

	// One kinematic step. Moves the owning actor. Safe to call before the
	// voxel world exists (holds position rather than falling through nothing).
	void TickMovement(float DeltaTime);

	// Clears velocities and jump state (but NOT stance -- see the definition).
	// Called when the pawn enters or leaves walk mode so stale state never
	// leaks across a mode toggle.
	void ResetState();

	// Stands up unconditionally, skipping the CanStandAt check, and moves the
	// owner to match. ONLY valid where collision does not apply -- specifically
	// leaving walk mode for collision-free fly mode, where a crouched box left
	// behind would otherwise keep the camera at crouch eye height and the proxy
	// body squashed for the whole flight.
	void ForceStand();

	// --- Queries (HUD, cameras, proxy body) --------------------------------

	bool IsGrounded() const { return bGroundedLastTick; }
	bool IsSwimming() const { return bSwimmingLastTick; }
	bool IsWaitingForTerrain() const { return bWaitingForTerrain; }

	// True once the box has actually shrunk -- NOT merely "the key is held".
	// A crouch key released under a ceiling keeps this true until there is
	// room to stand (see CanStandAt).
	bool IsCrouched() const { return bCrouched; }

	// True when a stand-up was actually ATTEMPTED and refused for lack of
	// headroom. Deliberately not "crouched with the key released", which is
	// briefly true for one frame during every ordinary stand and would flash a
	// warning on the HUD each time.
	bool IsCrouchBlocked() const { return bStandBlocked; }

	double GetHorizontalSpeedUU() const { return HorizontalVelocity.Size(); }
	double GetVerticalVelocityUU() const { return VerticalVelocity; }

	int32 GetSpeedTierIndex() const { return SpeedTierIndex; }
	static int32 GetSpeedTierCount() { return VoxelMovementTuning::kNumSpeedTiers; }
	const TCHAR* GetSpeedTierName() const;

	// Dialled pace before any modifier.
	double GetDialSpeedUU() const { return VoxelMovementTuning::SpeedForTier(SpeedTierIndex); }

	// What the mover will actually top out at right now: the dial, capped by
	// crouch or overridden by an engaged sprint.
	double GetEffectiveMaxSpeedUU() const;

	// True only while sprint is genuinely applying -- Shift held AND moving
	// forward AND not crouched. Distinct from the raw key state so the HUD
	// never claims a sprint that the forward-only gate is suppressing.
	bool IsSprintEngaged() const { return bSprintEngagedLastTick; }

	// Current collision half-extents (Z changes with crouch).
	double GetHalfExtentZ() const { return bCrouched ? VoxelMovementTuning::CrouchHalfExtentZ
	                                                 : VoxelMovementTuning::StandHalfExtentZ; }

	// Eye offset above the box CENTER for the current stance. The pawn smooths
	// toward this rather than snapping (see AVoxelEarthFlyPawn::EyeOffsetUU).
	double GetTargetEyeOffsetUU() const { return bCrouched ? VoxelMovementTuning::CrouchEyeOffsetUU
	                                                       : VoxelMovementTuning::StandEyeOffsetUU; }

	// Gait phase in radians, advanced by distance travelled. Owned here so the
	// camera bob and the proxy body's limb swing share ONE phase instead of
	// two copies of the same formula drifting apart.
	float GetGaitPhase() const { return GaitPhase; }

	// Abrupt Z change (UU) the mover applied this tick that the camera should
	// NOT follow rigidly -- a step-up snap or a crouch/stand transition.
	// Returns the accumulated value and clears it; the pawn folds it into its
	// decay-to-zero smoothing channel.
	double ConsumeAbruptZJumpUU();

	// Impact speed (UU/s) of a landing that happened this tick, or 0. Returns
	// the value and clears it. Drives the first-person landing view punch.
	double ConsumeLandingImpactUU();

	// --- Debug visualization ------------------------------------------------

	// Draws the player's scale reference: the collision box wireframe, a marker
	// at the current eye height, and the voxel cells IsGroundedAt is probing
	// (green where solid, red where not). Non-persistent, so the caller must
	// re-issue it every frame -- same contract as
	// FVoxelWorldImpl::DrawDebugBoundsLayer.
	//
	// Lives HERE rather than on the pawn because it needs this file's private
	// AxisVoxelRange, subsystem handle and live half-extents. Drawing it from
	// the pawn would mean a second copy of the probe maths, which is exactly how
	// a debug view drifts out of sync with the logic it exists to explain. NPCs
	// sharing this mover simply never call it.
	//
	// EyeWorldZ is the caller's actual camera height (world Z), so the marker
	// reflects crouch blending and step smoothing rather than a nominal
	// constant. Self-gating: checks voxel.Debug.PlayerBox and suppresses itself
	// in unattended fixture runs.
	void DebugDrawVolume(double EyeWorldZ) const;

private:
	// --- Voxel collision primitives (moved verbatim from the pawn) ---------

	// Sweeps the collision box along a single world axis (0=X, 1=Y, 2=Z) by
	// Delta UU from InOutPos; on a blocking voxel, clamps InOutPos[Axis] to the
	// hit face (CollisionEpsilonUU clearance) instead of applying the full
	// Delta. Returns true if the sweep was blocked.
	bool SweepAxis(int32 Axis, double Delta, FVector& InOutPos) const;

	// True if a voxel is solid immediately (1 voxel) under the box footprint
	// at Pos. Uses the CURRENT half-extents, so a crouched probe is narrower
	// vertically but identical horizontally.
	bool IsGroundedAt(const FVector& Pos) const;

	// True when the voxel world under Pos has actually streamed in, so gravity
	// may safely run. See the definition for the "queued, not here yet" rule
	// and why falling is never the right answer for an unstreamed chunk.
	bool IsTerrainReadyAt(const FVector& Pos) const;

	// True if the taller standing box would fit at Pos -- i.e. the slab from
	// the crouched box's top up to the standing box's top
	// (StandClearanceUU) is clear across the footprint. Pos is the CROUCHED
	// box centre. This is what makes releasing crouch under a ceiling a no-op
	// instead of embedding the character in rock.
	bool CanStandAt(const FVector& CrouchCenterPos) const;

	// Applies a crouch/stand transition at Pos, keeping the feet planted: the
	// box centre moves down on crouch and up on stand by CrouchCenterDropUU.
	// Refuses to stand when CanStandAt fails. Returns true if the stance
	// changed.
	bool UpdateCrouchState(FVector& InOutPos);

	UVoxelWorldSubsystem* GetVoxelWorldSubsystem() const;

	// --- State --------------------------------------------------------------

	float CurrentForwardInput = 0.f;
	float CurrentRightInput = 0.f;
	float CurrentUpInput = 0.f;

	bool bSprintHeld = false;
	bool bCrouchHeld = false;
	bool bCrouched = false;
	// Set by UpdateCrouchState when a stand-up was refused by CanStandAt.
	bool bStandBlocked = false;

	// Accel-based horizontal velocity, X/Y only (Z always 0). Integrated
	// toward the wish direction rather than snapped to it, which is what makes
	// direction changes feel weighted.
	FVector HorizontalVelocity = FVector::ZeroVector;
	double VerticalVelocity = 0.0;

	int32 SpeedTierIndex = VoxelMovementTuning::kDefaultSpeedTierIndex;

	// Jump feel timers. TimeSinceGroundedSeconds is set huge on a successful
	// jump so the coyote window cannot be spent twice in one airtime.
	double TimeSinceGroundedSeconds = 0.0;
	double JumpBufferRemainingSeconds = 0.0;
	bool bJumpKeyHeld = false;

	// Known-floor memory (see the known-floor rule in TickMovement). Set the
	// first time the character actually rests on a voxel, and CLEARED by
	// ResetState -- a mode toggle may have flown the pawn kilometres away, and
	// a remembered floor from before the flight would clamp it in mid-air.
	bool bHadGroundContact = false;
	double LastGroundedFeetZ = 0.0;

	bool bWaitingForTerrain = false;
	bool bGroundedLastTick = false;
	bool bSwimmingLastTick = false;
	bool bSprintEngagedLastTick = false;

	float GaitPhase = 0.f;

	double PendingAbruptZJumpUU = 0.0;
	double PendingLandingImpactUU = 0.0;
};
