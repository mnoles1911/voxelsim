#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "VoxelEarthFlyPawn.generated.h"

class UCameraComponent;
class UFloatingPawnMovement;
class UVoxelWorldSubsystem;
class UVoxelProxyBodyComponent;

// docs/m1-plan.md Stage 2 decisions table: "Spectator fly pawn + legacy
// input bindings (Enhanced Input assets deferred)." A minimal from-scratch
// fly pawn rather than ADefaultPawn/ASpectatorPawn: those bind to NAMED axis
// mappings ("MoveForward" etc.) that must come from an Input Settings config
// this project doesn't have; binding raw FKeys directly (BindAxisKey) here
// needs zero ini/config and stays fully "legacy input, no assets."
//
// Stage 3b (docs/m1-plan.md stage 3b, plan SS3.3 "no Chaos for terrain")
// adds a walk mode toggled with G: fly mode (default) is unchanged
// UFloatingPawnMovement flight; walk mode is a from-scratch kinematic
// mover -- gravity, jump, and an axis-separated swept-AABB sweep against
// UVoxelWorldSubsystem::IsSolidAtVoxel -- driven from this pawn's own Tick
// (only enabled while walking). No CharacterMovementComponent, no Chaos:
// this is client-side presentation only (the doctrine determinism boundary
// covers world DERIVATION; authoritative movement arrives with the M3
// server), so plain floats are fine here.
UCLASS()
class VOXELEARTH_API AVoxelEarthFlyPawn : public APawn
{
	GENERATED_BODY()

public:
	AVoxelEarthFlyPawn();

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void Tick(float DeltaTime) override;

	// --- HUD queries (AVoxelEarthHUD mode line + debug overlay) -------------
	//
	// Read-only accessors; the HUD never mutates pawn state.

	bool IsWalkMode() const { return bWalkMode; }
	bool IsThirdPerson() const { return bThirdPerson; }
	bool IsSprintHeld() const { return bSprintHeld; }

	// Effective fly speed (UU/s) INCLUDING the boost/precision modifiers --
	// i.e. what the pawn would actually top out at right now.
	double GetEffectiveFlySpeedUU() const;

	// Base fly speed step (UU/s) before modifiers, and its index/count in the
	// speed table so the overlay can render "step 5/9".
	double GetFlyBaseSpeedUU() const;
	int32 GetFlySpeedIndex() const { return FlySpeedIndex; }
	static int32 GetFlySpeedStepCount() { return kNumFlySpeedSteps; }

	// Walk mode only: true while the pawn is holding position because the
	// terrain under it has not streamed in yet (see TickWalkMode). The HUD
	// surfaces this -- an unexplained hover is otherwise indistinguishable
	// from a physics bug.
	bool IsWaitingForTerrain() const { return bWaitingForTerrain; }

	// Walk mode only: last tick's grounded / submerged state.
	bool IsGroundedNow() const { return bGroundedLastTick; }
	bool IsSwimmingNow() const { return bSwimmingLastTick; }

	// Steps the fly speed table (Delta = +1 / -1), clamped. Public so the
	// debug overlay can drive it as well as the ']' / '[' keys.
	void AdjustFlySpeed(int32 Delta);

	// Public so the debug overlay's "Movement mode" row can flip it.
	void SetWalkMode(bool bInWalkMode);

protected:
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth")
	TObjectPtr<USceneComponent> SceneRoot;

	// First-person camera (unchanged from stage 2/3b) -- active by default.
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth")
	TObjectPtr<UCameraComponent> Camera;

	// Over-the-shoulder third-person camera (Player experience decisions
	// table, "Cameras" row). Not attached via USpringArmComponent (terrain
	// has no Chaos collision, so the spring arm's sphere-sweep probe would
	// never hit anything useful) -- instead its world transform is computed
	// and set manually every frame in UpdateThirdPersonCamera, using the
	// subsystem voxel DDA raycast for collision-aware pull-in. 'C' toggles
	// which of Camera / ThirdPersonCamera is active; AActor::CalcCamera's
	// default "first active UCameraComponent" behavior (bFindCameraComponentWhenViewTarget)
	// then picks it up with no further plumbing.
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth")
	TObjectPtr<UCameraComponent> ThirdPersonCamera;

	// Blocky voxel proxy body (Player experience decisions table,
	// "Character proxy" row) -- visible only in third person.
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth")
	TObjectPtr<UVoxelProxyBodyComponent> ProxyBody;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth")
	TObjectPtr<UFloatingPawnMovement> Movement;

private:
	void MoveForward(float Value);
	void MoveRight(float Value);
	void MoveUp(float Value);
	void Turn(float Value);
	void LookUp(float Value);
	void ToggleWalkMode();
	void OnFlySpeedUp();
	void OnFlySpeedDown();
	void OnPrecisionPressed();
	void OnPrecisionReleased();
	void OnJumpPressed();
	void OnSprintPressed();
	void OnSprintReleased();
	void ToggleCameraMode();

	// Walk-mode kinematic step, called from Tick only while bWalkMode is set.
	void TickWalkMode(float DeltaTime);

	// Pushes the current fly speed step + modifiers onto UFloatingPawnMovement.
	// Called every Tick in fly mode (the modifiers are held keys, so the
	// effective speed can change between two presses of nothing).
	void ApplyFlySpeedToMovement();

	// True when the voxel world under Pos has actually streamed in, so walk
	// mode's gravity may safely run. See the definition for the two-part rule
	// (global "anything loaded at all" + local "this chunk is tracked but has
	// no component yet") and why falling is never the right answer here.
	bool IsTerrainReadyAt(const FVector& Pos) const;

	// Runs every frame regardless of walk/fly mode: decays CameraZOffsetUU
	// (step smoothing, see class comment on that field) toward 0 and applies
	// it to the first-person camera's height, then recomputes the
	// third-person boom (UpdateThirdPersonCamera).
	void UpdateCameraSmoothing(float DeltaTime);

	// Over-the-shoulder boom placement + collision-aware pull-in (Player
	// experience decisions table, "Cameras" row): desired position is the
	// head anchor, 250 UU back along the view and 40 UU right; a voxel DDA
	// raycast (UVoxelWorldSubsystem::RaycastVoxelWorld) from the head toward
	// that point pulls the camera in to 10 UU before the first solid hit.
	void UpdateThirdPersonCamera();

	// Head anchor used by both the first-person camera's height and the
	// third-person boom's origin: actor location + HeadOffsetUU, plus the
	// step-smoothing CameraZOffsetUU so a step-up jump is absorbed
	// identically in both camera modes.
	FVector GetHeadWorldLocation() const;

	// Sweeps the walk-mode collision box along a single world axis (0=X,
	// 1=Y, 2=Z) by Delta UU, starting from InOutPos; on a blocking voxel,
	// clamps InOutPos[Axis] to the hit face (CollisionEpsilonUU clearance)
	// instead of the full Delta. Returns true if the sweep was blocked.
	bool SweepAxis(int32 Axis, double Delta, FVector& InOutPos) const;

	// True if a voxel is solid immediately (1 voxel) under the walk-mode
	// collision box's footprint at Pos.
	bool IsGroundedAt(const FVector& Pos) const;

	UVoxelWorldSubsystem* GetVoxelWorldSubsystem() const;

	// Degrees/sec applied per unit of raw mouse-delta axis value.
	static constexpr float MouseLookSpeed = 2.5f;

	// --- Walk mode (stage 3b) -----------------------------------------

	bool bWalkMode = false;

	// Cached WASD axis state (fly mode applies these via AddMovementInput
	// directly in the callback; walk mode instead re-projects them onto the
	// horizontal plane once per Tick -- see TickWalkMode).
	float CurrentForwardInput = 0.f;
	float CurrentRightInput = 0.f;

	// Cached Space/LeftControl axis value (unused in walk mode outside of
	// swimming -- see class comment on MoveUp and TickWalkMode's swim
	// branch, W1 swimming placeholder).
	float CurrentUpInput = 0.f;

	float VerticalVelocity = 0.f;
	bool bJumpRequested = false;
	bool bSprintHeld = false;
	// LeftAlt held: fly-mode precision modifier (see kFlyPrecisionScale).
	bool bPrecisionHeld = false;

	// Set by TickWalkMode; read by the HUD (IsWaitingForTerrain / grounded /
	// swimming rows).
	bool bWaitingForTerrain = false;
	bool bGroundedLastTick = false;
	bool bSwimmingLastTick = false;

	// --- Fly speed (usability task) ---------------------------------------
	//
	// The world is kilometres across and a voxel is 10 cm, so one flat speed
	// cannot serve both "read the material seam on this cliff face" and "cross
	// four rings to the next tile". Three controls compose:
	//   * a base STEP table below, ']' / '[' (also settable from the overlay),
	//   * LeftShift held  -> kFlyBoostScale (fast traverse),
	//   * LeftAlt held    -> kFlyPrecisionScale (10 cm inspection).
	// Combined range is 0.075 m/s .. 8000 m/s. Step 4 (3000 UU/s) is the
	// default and is byte-identical to the pre-task constant, so nothing that
	// does not touch these keys changes behaviour.
	static constexpr int32 kNumFlySpeedSteps = 9;
	static constexpr double kFlySpeedStepsUU[kNumFlySpeedSteps] = {
		50.0,     // 0.5 m/s  -- voxel-scale inspection
		150.0,    // 1.5 m/s
		400.0,    // 4 m/s    -- walking pace
		1000.0,   // 10 m/s
		3000.0,   // 30 m/s   -- DEFAULT (pre-task MaxSpeed)
		8000.0,   // 80 m/s
		20000.0,  // 200 m/s
		60000.0,  // 600 m/s
		200000.0, // 2 km/s   -- cross the streamed world in a breath
	};
	static constexpr int32 kDefaultFlySpeedIndex = 4;
	static constexpr double kFlyBoostScale = 4.0;
	static constexpr double kFlyPrecisionScale = 0.15;
	// Acceleration as a multiple of max speed. 2.6667 keeps the default step
	// at exactly the pre-task 8000 UU/s^2 while staying proportional (a 2 km/s
	// step with 8000 UU/s^2 accel would take half a minute to reach speed).
	static constexpr double kFlyAccelPerMaxSpeed = 8000.0 / 3000.0;

	int32 FlySpeedIndex = kDefaultFlySpeedIndex;

	// Accel-based horizontal velocity (Player experience decisions table,
	// "Movement feel"): X/Y only (Z always 0), integrated in TickWalkMode via
	// GroundAccelUUPerSec2 / AirControlFactor instead of snapping straight to
	// the wish direction -- this is what makes direction changes feel
	// weighted instead of binary. Swimming keeps the old instant-velocity
	// model (see TickWalkMode's swim branch); this field is simply
	// overwritten each frame while swimming so ground movement resumes
	// smoothly from the last swim velocity on exit.
	FVector HorizontalVelocity = FVector::ZeroVector;

	// 60x60x180 UU box, actor location at box center.
	static constexpr double WalkBoxHalfExtentXY = 30.0;
	static constexpr double WalkBoxHalfExtentZ = 90.0;

	static constexpr double WalkSpeedUU = 450.0;         // UU/s, horizontal (4.5 m/s)
	static constexpr double SprintSpeedUU = 700.0;       // UU/s, horizontal, LeftShift held (7 m/s)
	static constexpr double GroundAccelUUPerSec2 = 4000.0; // UU/s^2, grounded horizontal accel/friction
	static constexpr double AirControlFactor = 0.30;     // fraction of ground accel applied while airborne
	// Water track W1 swimming placeholder (docs/voxel-earth-implementation-
	// plan.md SS3.7 -- real buoyancy/currents are W4): when the walk-mode
	// box is below sea level (z=0, see TickWalkMode), gravity is disabled
	// and movement becomes fly-style (horizontal + vertical from WASD +
	// Space/LeftControl) at this reduced speed instead.
	static constexpr double SwimSpeedUU = 300.0;         // UU/s, swimming (all axes)
	static constexpr double GravityUUPerSec2 = 980.0;    // UU/s^2
	// v = sqrt(2 * GravityUUPerSec2 * 100) UU/s, sized for a ~1.0m (10 voxel) apex.
	static constexpr double JumpSpeedUU = 442.7;         // UU/s, upward
	static constexpr double StepUpHeightUU = 30.0;       // 3 voxels
	static constexpr double CollisionEpsilonUU = 0.1;    // face clamp clearance
	// Slack allowed below the analytic surface before the unstreamed-terrain
	// backstop (see the end of TickWalkMode) engages. Generous enough that
	// voxel quantisation, a step-down, or standing in a shallow dip never trip
	// it; far smaller than the multi-metre free fall it exists to stop.
	static constexpr double SurfaceBackstopToleranceUU = 300.0; // 3 m

	// --- Cameras + step smoothing (Player experience decisions table) -----

	// Height of the "head" anchor above the walk-box center; shared by the
	// first-person camera's rest height and the third-person boom's origin.
	static constexpr double HeadOffsetUU = 70.0;

	// Third-person boom offsets from the head anchor, along the actor's view
	// basis (docs/m1-plan.md decisions table: "boom 2.5m back, 0.4m right").
	static constexpr double ThirdPersonBoomBackUU = 250.0;
	static constexpr double ThirdPersonBoomRightUU = 40.0;
	// Clearance kept between the pulled-in camera and the first solid voxel
	// hit by the boom's collision raycast.
	static constexpr double ThirdPersonPullInEpsilonUU = 10.0;

	// Step-smoothing camera offset (Player experience decisions table,
	// "Slope feel": "camera Z is smoothed (spring toward target) so steps
	// read as ramps"). TickWalkMode's step-up snap pushes the actor's Z
	// instantly; instead of also snapping the camera, it subtracts that jump
	// into this offset, and UpdateCameraSmoothing exponentially decays it
	// back to 0 (~10/s) every frame, added to both cameras' head height.
	double CameraZOffsetUU = 0.0;
	static constexpr double CameraSmoothRatePerSec = 10.0;

	bool bThirdPerson = false;
};
