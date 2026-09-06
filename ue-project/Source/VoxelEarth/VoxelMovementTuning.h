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
	// Offsets from the box CENTER, not the feet. Standing: 90 + 77 = 1.67 m eye
	// height. Crouched: 60 + 51 = 1.11 m.
	//
	// RAISED from 70/40 (1.60 / 1.00 m) on 2026-07-28. 1.60 m was ~7 cm below
	// anthropometric eye height for a 1.80 m person (~1.67 m), and measured
	// against the proxy body it was worse than that sounds: the head mesh spans
	// 1.52-1.80 m, so the view was coming from 29% up the head -- roughly jaw
	// level on the body you can see in third person. 1.67 m lands at 54% up the
	// head, where eyes actually sit. The crouched value keeps the same
	// eye-height-to-stature ratio (0.928) rather than being picked separately.
	//
	// Both still sit below the box top (13 UU standing, 9 UU crouched), which is
	// the property that matters: any ceiling the box clears, the view clears
	// too, so the camera can never poke through geometry the collision passed.
	inline constexpr double StandEyeOffsetUU = 77.0;   // 1.67 m above the feet
	inline constexpr double CrouchEyeOffsetUU = 51.0;  // 1.11 m above the feet

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

	// v = sqrt(2 * g * apex). Raised from 442.7 (a 1.0 m apex) to 495.0 (1.25 m)
	// on 2026-07-29: the 1.0 m jump read as STICKY in play-test -- Matt asked
	// for 20-30% more height and 25% is the middle of that.
	//
	// Note this was reported alongside a separate "jump gets stuck in mid-air"
	// bug, which was NOT a tuning problem (the gravity veto latching on an
	// all-air chunk, fixed by the known-floor rule in VoxelCharacterMovement).
	// Worth keeping distinct: a jump that felt weak and a jump that froze had
	// one shared symptom -- not going where you expected -- and only one of
	// them was about this number.
	//
	// 1.25 m still clears 12 voxels, so the "taller rises need a jump" rule in
	// m1-plan's Slope feel row (auto-step absorbs <= 3 voxels) is unchanged in
	// character; it just makes the gap between step and jump less punishing.
	inline constexpr double JumpSpeedUU = 495.0;

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

// ============================================================================
// VoxelBoatTuning -- AVoxelBoat (Phase D2)
// ============================================================================
//
// docs/water-ocean-tides-plan-2026-09-04.md Phase D. Same spirit as the block
// above and the same reason for existing: these numbers were going to be spread
// across AVoxelBoat's private section and hand-copied into whatever spawns it.
//
// WHAT THE PROVENANCE IS, since the W-phase blocks above can each cite a
// play-test: NONE OF THESE HAVE BEEN JUDGED YET. They are a first pass sized
// from a real canoe (about 4 m x 0.9 m, 30 kg empty, 180 kg loaded, floating on
// roughly 12 cm of draft) and from the reference game's own stated ambition,
// which is "fairly basic physics". Every one of them is expected to move once
// the owner has driven it, and the honest thing is to say so here rather than
// let a plausible-looking constant read as a measured one.
//
// UNITS. UU (cm) and seconds, matching the block above -- with ONE deliberate
// exception: the buoyancy spring is written as the physical relation
// K = Mass * g / (NumProbes * RestDraft), so it is not a number here at all.
// A boat that floats at the wrong height is then a wrong RestDraftUU, which is
// a measurable fact about a hull, rather than a spring constant nobody can
// check against anything.
namespace VoxelBoatTuning
{
	// --- the hull ------------------------------------------------------------
	//
	// The DEFAULTS. AVoxelBoat overrides length/beam/height from the loaded
	// asset's own bounds the moment a real .vxa arrives, because a boat whose
	// probes sit outside its own hull is the one failure that looks like a
	// physics bug and is not.
	inline constexpr double HullHalfLengthUU = 200.0; // 4.0 m stem to stern
	inline constexpr double HullHalfBeamUU = 45.0;    // 0.9 m beam
	inline constexpr double HullHeightUU = 45.0;      // 0.45 m gunwale above keel

	inline constexpr double MassKg = 180.0; // hull + one paddler + kit

	// Draft at rest: how deep the keel sits when the boat is floating still.
	// THIS IS THE ONE NUMBER THE SPRING IS DERIVED FROM -- K is chosen so that
	// exactly this much submersion holds exactly the boat's weight, so if it
	// floats too low, this is the number that is wrong.
	inline constexpr double RestDraftUU = 12.0; // 12 cm

	// How far a probe may be submerged before the spring stops growing. Past the
	// gunwale the hull is swamped, not more buoyant, and an unclamped spring
	// launches a boat that has been pushed under. 3x rest draft is generous
	// enough that ordinary bobbing never reaches it.
	inline constexpr double MaxDraftUU = 36.0;

	// Four probes at the hull corners (plan D2). Four is the smallest number
	// that gives pitch AND roll from pure vertical forces -- three would leave a
	// degenerate axis and two is a see-saw.
	inline constexpr int32 NumProbes = 4;

	// Fraction of critical damping on each probe's spring, so the damper is
	// derived from the spring rather than being a second free number that can
	// silently disagree with it: D = 2 * Ratio * sqrt(K * MassPerProbe).
	// Under 1 the hull settles with a visible bob (right for a boat); at 1 it
	// sinks to its waterline like a lift, which reads as dead.
	inline constexpr double BuoyancyDampingRatio = 0.85;

	// --- drag, and the keel is the whole point -------------------------------
	//
	// Per-second linear drag rates in the BODY frame. A hull is not
	// isotropic: it is built to go one way. The lateral figure being ~9x the
	// longitudinal one is what makes a boat track instead of drifting sideways
	// like a crate, and it is the cheapest possible stand-in for a keel.
	inline constexpr double DragLongPerSec = 0.35;
	inline constexpr double DragLatPerSec = 3.20;
	inline constexpr double DragVertPerSec = 1.20;

	// Angular damping, degrees-free (per second, applied to angular velocity).
	// Yaw is loose so the rudder can turn the boat; roll and pitch are stiff so
	// it does not wallow.
	inline constexpr double AngularDampYawPerSec = 1.20;
	inline constexpr double AngularDampRollPitchPerSec = 3.50;

	// --- drive ---------------------------------------------------------------

	// Forward acceleration at full throttle, applied along the hull's forward
	// vector PROJECTED ONTO THE WATER PLANE -- a boat pitched bow-up by a wave
	// must not be able to thrust itself into the sky.
	inline constexpr double ThrustAccelUUPerSec2 = 260.0; // 2.6 m/s^2

	// Reverse is weaker than forward on anything with a stern.
	inline constexpr double ReverseThrustScale = 0.45;

	// Rudder authority, as a yaw acceleration per unit of steering input per
	// UU/s of forward speed. A rudder is a control surface: it does NOTHING at
	// rest, which is correct and is also the first thing that reads as broken if
	// it is not explained. Hold throttle to turn.
	inline constexpr double RudderYawAccelPerSpeed = 0.0022;

	// Speed at which the rudder's authority saturates, so a fast boat does not
	// spin on the spot.
	inline constexpr double RudderSaturationSpeedUU = 500.0;

	// --- ground, and the unstreamed-tile rule --------------------------------

	// Extra clearance under the keel before the boat counts as beached.
	inline constexpr double GroundClearanceUU = 3.0;

	// Linear drag rate applied while beached. High: a hull on gravel stops.
	inline constexpr double GroundFrictionPerSec = 6.0;

	// How far ahead of the bow the cliff probe reaches, and the speed below
	// which running into rock stops being worth cancelling.
	inline constexpr double BowProbeUU = 90.0;
	inline constexpr double BowProbeMinSpeedUU = 30.0;

	// Physics sleeps beyond this distance from the camera (plan D2's
	// "unstreamed-tile rule": every query this actor makes is datum-only and
	// therefore valid over unstreamed ground, but a boat simulating a hundred
	// metres away is simulating for nobody).
	inline constexpr double SleepRadiusUU = 10000.0; // 100 m

	// --- wake (plan D4) ------------------------------------------------------
	//
	// Strengths are METRES of ripple height and are deliberately small: the
	// swept helper passes StrengthM to every sub-splat and overlapping raised
	// cosines sum, so a wake tuned to the one-off splash figures in
	// VoxelRippleField.h would be a wall of water.

	// Below this the boat is drifting and leaves nothing.
	inline constexpr double WakeMinSpeedUU = 55.0; // 0.55 m/s

	inline constexpr double BowWakeWidthM = 0.55;
	inline constexpr double BowWakeStrengthM = 0.030;
	inline constexpr double TransomWakeWidthM = 0.95;
	inline constexpr double TransomWakeStrengthM = 0.022;

	// Hull slam: a probe descending faster than this INTO water makes a splash
	// on top of the wake. 2.5 m/s is a hull dropping off a wave, not a hull
	// settling.
	inline constexpr double SlamProbeSpeedUU = 250.0;
	inline constexpr double SlamRadiusM = 0.8;
	inline constexpr double SlamStrengthM = 0.07;

	// --- possession ----------------------------------------------------------

	// How close the player must be for the interact key to find a boat.
	inline constexpr double InteractRangeUU = 400.0; // 4 m

	// Where the player is put back down on exit: beside the hull, clear of the
	// beam, at the waterline. Sideways rather than astern so stepping out of a
	// beached boat does not put you in the rock it is resting against.
	inline constexpr double ExitSideClearanceUU = 90.0;
	inline constexpr double ExitLiftUU = 100.0;

	// --- camera --------------------------------------------------------------

	inline constexpr double CameraBackUU = 460.0;
	inline constexpr double CameraUpUU = 190.0;
	inline constexpr double CameraPitchMinDeg = -70.0;
	inline constexpr double CameraPitchMaxDeg = 25.0;
	inline constexpr double CameraDefaultPitchDeg = -12.0;
}

// ============================================================================
// VoxelGliderTuning -- AVoxelGlider (Phase E)
// ============================================================================
//
// A POINT MASS WITH A WING, not a Chaos body: the plan says kinematic, which is
// also this project's house pattern for anything the player steers
// (AVoxelEarthFlyPawn and UVoxelCharacterMovementComponent are both kinematic
// against a voxel DDA, because terrain carries no Chaos collision at all).
//
// UNITS ARE SI HERE AND ONLY HERE, and it is worth saying why rather than
// converting for the sake of matching the header. Lift is q*S*CL with
// q = 0.5*rho*V^2; rho, S and every CL/CD coefficient are tabulated in SI in
// every source anyone would check these against, and a coefficient rewritten in
// kg/cm^3 is a coefficient nobody can check. So the AERO is SI, the conversion
// to UU happens once at the force-application site in VoxelGlider.cpp, and the
// two are never mixed inside one expression.
//
// THE GLIDE RATIO IS THE GATE. Max L/D = 1 / (2*sqrt(CD0 * InducedK)) =
// 1 / (2*sqrt(0.045 * 0.0833)) = 8.16, at CL = sqrt(CD0/k) = 0.735. The plan
// asks for "~1:8", so these two numbers are not independently tunable knobs --
// they ARE the glide ratio, and moving either moves it. Anyone retuning should
// pick the ratio first and solve back.
namespace VoxelGliderTuning
{
	inline constexpr double MassKg = 100.0;      // pilot + wing, hang-glider class
	inline constexpr double WingAreaM2 = 15.0;   // a big slow wing; forgiving to fly
	inline constexpr double AirDensityKgM3 = 1.225; // sea level ISA, not altitude-varied in v1

	// CL = CL0 + CLAlpha * alpha, clamped. CLAlpha is ~0.8 of the thin-aerofoil
	// 2*pi, which is a normal finite-wing figure.
	inline constexpr double CL0 = 0.25;
	inline constexpr double CLAlphaPerRad = 5.0;
	// THE STALL, and it is a CLAMP not a break. A real wing loses lift past the
	// stall angle; clamping instead just stops it gaining any. That is the
	// forgiving choice on purpose -- a v1 glider that departs controlled flight
	// is a glider nobody photographs. Stated so the limitation is a decision.
	inline constexpr double CLMax = 1.35;
	inline constexpr double CLMin = -0.45;

	// Drag polar. See the glide-ratio note above the namespace before touching.
	inline constexpr double CD0 = 0.045;
	inline constexpr double InducedK = 0.0833;

	// Below this airspeed the aero terms are faded out entirely. Not a physical
	// stall: q -> 0 makes every coefficient meaningless and the direction of
	// "forward" numerically garbage, and a NaN attitude is unrecoverable.
	inline constexpr double MinAirspeedMS = 3.0;
	// ...and the speed over which the fade completes.
	inline constexpr double AeroFadeBandMS = 3.0;

	// --- controls ------------------------------------------------------------
	//
	// RATE controls, not force controls: the stick commands a pitch/roll RATE
	// directly. That is the arcade choice and it is deliberate for the same
	// reason as the stall clamp.
	inline constexpr double PitchRateDegPerSec = 38.0;
	inline constexpr double RollRateDegPerSec = 65.0;
	inline constexpr double MaxPitchDeg = 60.0;
	inline constexpr double MaxRollDeg = 65.0;

	// Yaw FOLLOWS roll -- a coordinated turn, yawRate = g*tan(roll)/V. This is
	// the real relation, not an approximation of one, which is why there is no
	// gain here to tune: a banked wing turns at that rate or it is slipping.
	inline constexpr double GravityMS2 = 9.80665;
	// Cap so a near-90-degree bank at low speed cannot ask for an infinite rate.
	inline constexpr double MaxYawRateDegPerSec = 90.0;

	// Self-levelling toward wings-level when the stick is centred. Small: enough
	// that a released stick recovers, not so much that the glider flies itself.
	inline constexpr double RollLevelPerSec = 0.8;

	// --- terrain and water ----------------------------------------------------

	// Height above ground at which the flare begins: pitch up, bleed speed.
	inline constexpr double FlareAltitudeUU = 250.0; // 2.5 m
	inline constexpr double FlarePitchDeg = 12.0;

	// Below this clearance the glider is DOWN: it settles, the player is put
	// back on their feet and the glider despawns.
	inline constexpr double TouchdownClearanceUU = 40.0;

	// How far down the terrain probe reaches. Generous -- a glider over a valley
	// is legitimately hundreds of metres up, and a probe that gave up would read
	// as "no ground" and fly straight through the far wall.
	inline constexpr double GroundProbeUU = 200000.0;

	// A ditching makes ONE splash. Bigger than a boat's wake and smaller than
	// the field's own strength ceiling.
	inline constexpr double DitchSplashRadiusM = 1.6;
	inline constexpr double DitchSplashStrengthM = 0.11;

	// --- launch ---------------------------------------------------------------

	// Deploy gives this much airspeed along the view direction, so a glider
	// opened at terminal velocity does not need a second of freefall to become
	// controllable.
	inline constexpr double DeployMinSpeedMS = 14.0;

	// Minimum clearance above ground to deploy. Opening a wing 3 m up is a
	// crash, and refusing it with a log line is better than simulating one.
	inline constexpr double DeployMinClearanceUU = 600.0; // 6 m

	// --- camera ---------------------------------------------------------------

	inline constexpr double CameraBackUU = 700.0;
	inline constexpr double CameraUpUU = 220.0;
}
