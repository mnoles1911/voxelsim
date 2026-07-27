#pragma once
// VoxelMovementTuning.h -- the ONE place for player character movement numbers.
//
// Same spirit as VoxelCoords.h (which owns voxel<->world placement): a plain
// constants header with no UObject types, safe to include from UHT-parsed
// headers, the movement component, the proxy body and the HUD alike. It exists
// because the numbers were previously spread across AVoxelEarthFlyPawn's
// private section and hand-copied into UVoxelProxyBodyComponent -- the old
// VoxelProxyBody.h carried its own `RefWalkSpeedUU = 450.0` with a comment
// admitting it mirrored the pawn's WalkSpeedUU and had to be kept in sync by
// hand. One header removes that class of drift.
//
// Everything here is client-side PRESENTATION tuning. Player movement is
// deliberately outside the determinism boundary (that covers world DERIVATION;
// docs/voxel-earth-implementation-plan.md), so doubles are fine and none of
// these values feed a digest.
//
// Units: UU (Unreal units = cm) throughout, matching VoxelCoords::VoxelSizeUU
// (10 UU = 1 voxel = 10 cm). A comment gives the metric value wherever the
// number is a design decision rather than an implementation detail.

#include "CoreMinimal.h"

namespace VoxelMovementTuning
{
	// --- Character volume (docs/m1-plan.md "Character proxy" row) -----------
	//
	// The collision box is axis-aligned with the actor's location at its
	// CENTER, so a half-extent of 90 UU is a 1.8 m tall character. Standing
	// height is unchanged from the original walk-mode prototype -- it is now
	// simply an explicit decision rather than a literal buried in the pawn.

	inline constexpr double BoxHalfExtentXY = 30.0;    // 0.6 m wide (6 voxels)
	inline constexpr double StandHalfExtentZ = 90.0;   // 1.8 m tall (18 voxels)
	inline constexpr double CrouchHalfExtentZ = 60.0;  // 1.2 m tall (12 voxels)

	// How far the box CENTER drops when crouching. Feet stay planted: the box
	// shrinks from the top only, so bottom = center - halfZ is invariant across
	// the transition and a crouching character never sinks into the floor.
	inline constexpr double CrouchCenterDropUU = StandHalfExtentZ - CrouchHalfExtentZ; // 30

	// Vertical slab (UU) that must be clear ABOVE the crouched box before the
	// character may stand up -- exactly the space the taller box would newly
	// occupy. See UVoxelCharacterMovementComponent::CanStandAt.
	inline constexpr double StandClearanceUU = 2.0 * CrouchCenterDropUU; // 60 (6 voxels)

	// --- Eye height ---------------------------------------------------------
	//
	// Offsets from the box CENTER, not the feet. Standing: 90 + 70 = 1.6 m eye
	// height. Crouched: 60 + 40 = 1.0 m. Both sit 20 UU below the box top, so
	// the view never pokes through a ceiling the box is cleared against.

	inline constexpr double StandEyeOffsetUU = 70.0;   // 1.6 m above the feet
	inline constexpr double CrouchEyeOffsetUU = 40.0;  // 1.0 m above the feet

	// --- Speed dial (8 tiers, mouse wheel) ----------------------------------
	//
	// A single walk/sprint pair cannot serve a world that is kilometres across
	// and made of 10 cm voxels: placing a voxel on a ledge and crossing a
	// valley want different speeds, and neither is "walk" or "run". The wheel
	// selects a sustained pace instead, Star-Citizen style, and Shift is a
	// momentary override on top of it (see SprintTierIndex below).
	//
	// The wheel used to cycle dig size; dig size kept its existing 1/2/3
	// shortcuts (AVoxelEarthPlayerController::SelectDigSize*) and the wheel now
	// means "speed" in BOTH walk and fly mode, so it is never mode-ambiguous.

	inline constexpr int32 kNumSpeedTiers = 8;

	inline constexpr double kSpeedTiersUU[kNumSpeedTiers] = {
		70.0,  // 0.7 m/s -- Creep    : voxel-precision placement near a ledge
		140.0, // 1.4 m/s -- Walk     : real human walking pace
		220.0, // 2.2 m/s -- Stride
		320.0, // 3.2 m/s -- Trot
		450.0, // 4.5 m/s -- Jog      : DEFAULT, the pre-dial WalkSpeedUU exactly
		600.0, // 6.0 m/s -- Run
		750.0, // 7.5 m/s -- Sprint
		950.0, // 9.5 m/s -- Mad dash : Shift's momentary override target
	};

	inline constexpr const TCHAR* kSpeedTierNames[kNumSpeedTiers] = {
		TEXT("Creep"), TEXT("Walk"), TEXT("Stride"), TEXT("Trot"),
		TEXT("Jog"), TEXT("Run"), TEXT("Sprint"), TEXT("Mad dash"),
	};

	// Index 4 (450 UU/s) is byte-identical to the pre-dial WalkSpeedUU, so a
	// session that never touches the wheel moves exactly as it did before.
	inline constexpr int32 kDefaultSpeedTierIndex = 4;

	// Shift jumps straight to the top tier rather than nudging one step, so
	// "hold Shift" always produces a KNOWN speed regardless of where the dial
	// happens to sit.
	inline constexpr int32 kSprintTierIndex = kNumSpeedTiers - 1;

	// Sprint only engages while actually heading forward: strafing and
	// backpedalling stay at the dialled pace. Compared against the dot product
	// of the wish direction and the actor's yaw-only forward vector, so this is
	// cos(~45 deg) -- a generous forward cone, not a strict straight-ahead test.
	inline constexpr double SprintForwardDot = 0.7;

	// Crouch CLAMPS the dial rather than replacing it (1.5 m/s). Crouching
	// while dialled below that stays at the dialled speed -- a crouching creep
	// must not speed up.
	inline constexpr double CrouchSpeedCapUU = 150.0; // 1.5 m/s

	// --- Ground/air kinematics ----------------------------------------------

	inline constexpr double GroundAccelUUPerSec2 = 4000.0; // accel AND friction when input is zero
	inline constexpr double AirControlFactor = 0.30;       // fraction of ground accel while airborne
	inline constexpr double GravityUUPerSec2 = 980.0;
	inline constexpr double StepUpHeightUU = 30.0;         // 3 voxels, absorbed silently
	inline constexpr double CollisionEpsilonUU = 0.1;      // face-clamp clearance

	// W1 swimming placeholder (real buoyancy/currents are W4): below sea level
	// gravity is off and movement is fly-style at this speed on all axes.
	inline constexpr double SwimSpeedUU = 300.0;

	// Slack below the analytic surface before the unstreamed-terrain backstop
	// engages. Generous enough that voxel quantisation or a step-down never
	// trips it, far smaller than the multi-metre fall it exists to stop.
	inline constexpr double SurfaceBackstopToleranceUU = 300.0; // 3 m

	// --- Jump ---------------------------------------------------------------

	// v = sqrt(2 * g * 100) UU/s, sized for a ~1.0 m (10 voxel) apex.
	inline constexpr double JumpSpeedUU = 442.7;

	// Coyote time: a jump is still allowed this long after walking off an edge.
	// With 10 cm voxels the world is nothing but edges, so without this the
	// most common input outcome is a dropped jump that reads as a bug.
	inline constexpr double CoyoteTimeSeconds = 0.10;

	// Jump buffering: a jump pressed this long BEFORE landing fires on
	// touchdown instead of being discarded.
	inline constexpr double JumpBufferSeconds = 0.15;

	// Variable jump height: releasing the key while still rising scales the
	// remaining upward velocity by this, giving a short hop for 1-3 voxel
	// rises without a second jump button.
	inline constexpr double JumpReleaseCutScale = 0.45;

	// --- Cameras ------------------------------------------------------------

	inline constexpr double ThirdPersonBoomBackUU = 250.0;      // 2.5 m back
	inline constexpr double ThirdPersonBoomRightUU = 40.0;      // 0.4 m to the active shoulder
	inline constexpr double ThirdPersonPullInEpsilonUU = 10.0;  // clearance before the first solid voxel

	// Exponential rate (per second) at which the third-person boom eases toward
	// its target position. Applied BEFORE the collision pull-in so a lagging
	// camera can never smooth itself into rock.
	inline constexpr double ThirdPersonLagRatePerSec = 12.0;

	// Step-smoothing / landing-punch decay rate: this channel always decays
	// toward ZERO, which is what makes it right for transient offsets.
	inline constexpr double CameraSmoothRatePerSec = 10.0;

	// Crouch eye-height rate: this channel tracks a HELD target (stand vs
	// crouch eye offset) rather than decaying to zero, which is why it cannot
	// share the step-smoothing channel above.
	inline constexpr double CrouchCameraRatePerSec = 12.0;

	// --- First-person feel --------------------------------------------------

	// Landing view punch: the camera dips by up to this much, scaled by impact
	// speed against the reference below, then decays out through the ordinary
	// step-smoothing channel.
	inline constexpr double LandingPunchMaxUU = 22.0;
	inline constexpr double LandingPunchRefSpeedUU = 900.0; // impact speed producing a full-strength punch
	inline constexpr double LandingPunchMinSpeedUU = 180.0; // below this, stepping down should not punch at all

	// Head bob: vertical at twice the gait frequency (one dip per footfall),
	// lateral at the gait frequency (one sway per stride).
	inline constexpr double HeadBobVerticalUU = 3.0;
	inline constexpr double HeadBobLateralUU = 2.0;

	// One full gait cycle per this many UU travelled (~1.4 m stride). Shared
	// with the proxy body's limb swing so the camera bob and the visible legs
	// are driven by the SAME phase rather than two drifting copies.
	inline constexpr double StrideLengthUU = 140.0;

	// FOV kick: widens the field of view across the top tiers to convey speed.
	// Nothing happens below kFOVKickStartTier, so the ordinary walking range is
	// completely unaffected.
	inline constexpr float BaseFOVDegrees = 90.f;
	inline constexpr float MaxFOVKickDegrees = 12.f;
	inline constexpr int32 kFOVKickStartTier = 5; // "Run" and above
	inline constexpr double FOVKickRatePerSec = 6.0;

	// --- Proxy body ---------------------------------------------------------

	// Speed at which the limb swing reaches full amplitude. Now genuinely
	// shared rather than duplicated: this IS the default dial tier.
	inline constexpr double ProxyRefSpeedUU = kSpeedTiersUU[kDefaultSpeedTierIndex];
	inline constexpr float ProxyMaxSwingDegrees = 35.f;
	inline constexpr float ProxyBobFrequencyHz = 0.8f;
	inline constexpr double ProxyBobAmplitudeUU = 1.5;

	// Vertical squash applied to the whole proxy body while crouched. Equal to
	// the box half-extent ratio, so the body fills the collision volume exactly
	// in both stances instead of merely suggesting a crouch.
	inline constexpr double ProxyCrouchScaleZ = CrouchHalfExtentZ / StandHalfExtentZ; // 2/3

	// --- Player volume debug draw -------------------------------------------
	//
	// A scale reference for play-testing (voxel.Debug.PlayerBox, default ON in
	// walk mode). Colours deliberately avoid CYAN, which the chunk-bounds layer
	// already owns (FVoxelWorldImpl::DrawDebugBoundsLayer) -- with both layers
	// up, two wireframe boxes in the same colour would be unreadable.

	inline constexpr float DebugBoxThickness = 2.0f;
	inline constexpr float DebugMarkerThickness = 1.5f;

	// Flat markers are drawn as boxes with a near-zero extent on one axis.
	inline constexpr double DebugFlatHalfThicknessUU = 0.5;

	// Ground-probe cells are drawn on the TOP face of the probed voxel layer --
	// i.e. the surface being stood on -- lifted by this much so they do not
	// z-fight with the terrain they sit on.
	inline constexpr double DebugGroundCellLiftUU = 0.5;

	// --- Helpers ------------------------------------------------------------

	inline constexpr int32 ClampTierIndex(int32 Index)
	{
		return Index < 0 ? 0 : (Index >= kNumSpeedTiers ? kNumSpeedTiers - 1 : Index);
	}

	inline constexpr double SpeedForTier(int32 Index)
	{
		return kSpeedTiersUU[ClampTierIndex(Index)];
	}

	inline const TCHAR* NameForTier(int32 Index)
	{
		return kSpeedTierNames[ClampTierIndex(Index)];
	}
}
