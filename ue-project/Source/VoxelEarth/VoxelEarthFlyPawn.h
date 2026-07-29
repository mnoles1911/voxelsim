#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "VoxelMovementTuning.h"
#include "VoxelEarthFlyPawn.generated.h"

class UCameraComponent;
class UFloatingPawnMovement;
class UVoxelCharacterMovementComponent;
class UVoxelProxyBodyComponent;

// docs/m1-plan.md Stage 2 decisions table: "Spectator fly pawn + legacy input
// bindings (Enhanced Input assets deferred)." A minimal from-scratch fly pawn
// rather than ADefaultPawn/ASpectatorPawn: those bind to NAMED axis mappings
// ("MoveForward" etc.) that must come from an Input Settings config this project
// doesn't have; binding raw FKeys directly here needs zero ini/config and stays
// fully "legacy input, no assets."
//
// Two movement modes, toggled with G. FLY (default) is unchanged
// UFloatingPawnMovement flight with a 9-step speed table. WALK delegates
// entirely to UVoxelCharacterMovementComponent -- a kinematic mover sweeping an
// AABB against the voxel world (plan SS3.3 "no Chaos for terrain").
//
// This pawn's remaining job after that extraction is INPUT and CAMERAS:
//   * legacy key/axis bindings, forwarded to whichever mover is active;
//   * the first-person camera and the manually-placed over-the-shoulder
//     third-person boom (no USpringArmComponent -- terrain carries no Chaos
//     collision, so a spring arm's sphere-sweep probe would never hit anything;
//     the boom uses the subsystem's voxel DDA raycast for pull-in instead);
//   * camera FEEL -- crouch eye height, step smoothing, landing punch, head bob
//     and the high-tier FOV kick.
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
	bool IsRightShoulder() const { return bRightShoulder; }

	// Raw LeftShift key state. Deliberately NOT "is the character sprinting":
	// the key means boost in fly mode and sprint in walk mode, and the walk
	// sprint additionally has a forward-only gate. Use IsFlyBoostActive() or
	// IsSprintEngaged() to report either honestly -- the HUD used to print
	// "(boost)" while walking because this one flag served both.
	bool IsShiftHeld() const { return bShiftHeld; }

	// Fly-mode boost is actually applying (Shift held AND in fly mode).
	bool IsFlyBoostActive() const { return bShiftHeld && !bWalkMode; }

	// Walk-mode sprint is actually applying: Shift held, moving forward, not
	// crouched. False whenever the forward-only gate suppresses it.
	bool IsSprintEngaged() const;

	// Effective fly speed (UU/s) INCLUDING the boost/precision modifiers --
	// i.e. what the pawn would actually top out at right now.
	double GetEffectiveFlySpeedUU() const;

	// Base fly speed step (UU/s) before modifiers, and its index/count in the
	// speed table so the overlay can render "step 5/9".
	double GetFlyBaseSpeedUU() const;
	int32 GetFlySpeedIndex() const { return FlySpeedIndex; }
	static int32 GetFlySpeedStepCount() { return kNumFlySpeedSteps; }

	// --- Walk-mode queries (forwarded to the movement component) -----------

	// True while the pawn is holding position because the terrain under it has
	// not streamed in yet. The HUD surfaces this -- an unexplained hover is
	// otherwise indistinguishable from a physics bug.
	bool IsWaitingForTerrain() const;
	bool IsGroundedNow() const;
	bool IsSwimmingNow() const;
	bool IsCrouched() const;

	// True when the crouch key is released but a ceiling is still keeping the
	// character crouched; the HUD says so rather than leaving it a mystery.
	bool IsCrouchBlocked() const;

	// Walk speed dial (the mouse wheel): current tier index, its name
	// ("Jog", "Sprint", ...) and the speed it would actually top out at now.
	int32 GetSpeedTierIndex() const;
	const TCHAR* GetSpeedTierName() const;
	static int32 GetSpeedTierCount() { return VoxelMovementTuning::kNumSpeedTiers; }
	double GetEffectiveWalkSpeedUU() const;

	// --- Scripted input (unattended movement fixture, -VoxelWalkTest) -------
	//
	// Overrides the cached WASD axis values so an unattended run can exercise
	// the mover with no keyboard attached.
	//
	// A SEPARATE channel rather than writing CurrentForwardInput/
	// CurrentRightInput directly, because that would silently do nothing: UE
	// polls input before actor tick, so the axis bindings re-set those fields
	// to 0 every frame before TickWalkMode reads them. Only the AXIS values
	// need this treatment -- sprint, crouch and jump are pushed to the mover on
	// key EVENTS rather than per frame, so a fixture drives those through
	// UVoxelCharacterMovementComponent's own API (see GetWalkMovement).
	void SetScriptedInput(float Forward, float Right);
	void ClearScriptedInput() { bScriptedInputActive = false; }
	bool IsScriptedInputActive() const { return bScriptedInputActive; }

	// The mover itself, so a fixture can push jump/crouch/sprint/tier and read
	// grounded/crouched/velocity back without duplicating the forwarders.
	UVoxelCharacterMovementComponent* GetWalkMovement() const { return WalkMovement; }

	// Steps the fly speed table (Delta = +1 / -1), clamped. Public so the debug
	// overlay can drive it as well as the ']' / '[' keys.
	void AdjustFlySpeed(int32 Delta);

	// Steps whichever speed control the CURRENT mode owns -- the walk gait tier
	// or the fly speed step. This is what the mouse wheel and the debug
	// overlay's speed row both drive, so the row always adjusts the same value
	// it is displaying.
	void AdjustSpeedDial(int32 Delta);

	// Public so the debug overlay's "Movement mode" row can flip it.
	void SetWalkMode(bool bInWalkMode);

protected:
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth")
	TObjectPtr<USceneComponent> SceneRoot;

	// First-person camera -- active by default.
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth")
	TObjectPtr<UCameraComponent> Camera;

	// Over-the-shoulder third-person camera (Player experience decisions table,
	// "Cameras" row). Not attached via USpringArmComponent (terrain has no Chaos
	// collision, so the spring arm's sphere-sweep probe would never hit anything
	// useful) -- instead its world transform is computed and set manually every
	// frame in UpdateThirdPersonCamera, using the subsystem voxel DDA raycast
	// for collision-aware pull-in. 'V' toggles which of Camera /
	// ThirdPersonCamera is active; AActor::CalcCamera's default "first active
	// UCameraComponent" behavior then picks it up with no further plumbing.
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth")
	TObjectPtr<UCameraComponent> ThirdPersonCamera;

	// Blocky voxel proxy body -- visible only in third person.
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth")
	TObjectPtr<UVoxelProxyBodyComponent> ProxyBody;

	// Walk-mode kinematic mover (voxel AABB sweep, crouch, jump feel).
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth")
	TObjectPtr<UVoxelCharacterMovementComponent> WalkMovement;

	// Fly-mode mover. Parked (tick disabled) whenever walk mode is active so
	// the two never fight over the root transform.
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth")
	TObjectPtr<UFloatingPawnMovement> Movement;

private:
	void MoveForward(float Value);
	void MoveRight(float Value);
	void MoveUp(float Value);
	void Turn(float Value);
	void LookUp(float Value);
	void ToggleWalkMode();
	void OnSpeedDialUp();
	void OnSpeedDialDown();
	void OnFlySpeedUp();
	void OnFlySpeedDown();
	void OnPrecisionPressed();
	void OnPrecisionReleased();
	void OnJumpPressed();
	void OnJumpReleased();
	void OnSprintPressed();
	void OnSprintReleased();
	void OnCrouchPressed();
	void OnCrouchReleased();
	void ToggleCameraMode();
	void ToggleShoulder();

	// Pushes the cached axis/modifier state into the walk mover, then steps it.
	void TickWalkMode(float DeltaTime);

	// Pushes the current fly speed step + modifiers onto UFloatingPawnMovement.
	// Called every Tick in fly mode (the modifiers are held keys, so the
	// effective speed can change between two presses of nothing).
	void ApplyFlySpeedToMovement();

	// Runs every frame regardless of walk/fly mode: advances the two camera
	// height channels, the head bob and the FOV kick, then recomputes the
	// third-person boom.
	void UpdateCameraSmoothing(float DeltaTime);

	// Over-the-shoulder boom placement + collision-aware pull-in: desired
	// position is the head anchor, 250 UU back along the view and 40 UU toward
	// the active shoulder, eased toward with exponential lag; a voxel DDA
	// raycast from the head then pulls the camera in to 10 UU before the first
	// solid hit.
	void UpdateThirdPersonCamera(float DeltaTime);

	// Head anchor used by both the first-person camera's height and the
	// third-person boom's origin: actor location plus the two camera height
	// channels (see EyeOffsetUU / CameraZOffsetUU).
	FVector GetHeadWorldLocation() const;

	// Degrees/sec applied per unit of raw mouse-delta axis value.
	static constexpr float MouseLookSpeed = 2.5f;

	bool bWalkMode = false;

	// Cached WASD axis state. Fly mode applies these via AddMovementInput
	// directly in the callback; walk mode forwards them to the mover each Tick.
	float CurrentForwardInput = 0.f;
	float CurrentRightInput = 0.f;
	float CurrentUpInput = 0.f;

	// Scripted-input override (see SetScriptedInput). Walk mode only -- fly
	// mode applies its axes in the input callback itself, which a fixture has
	// no reason to drive.
	bool bScriptedInputActive = false;
	float ScriptedForwardInput = 0.f;
	float ScriptedRightInput = 0.f;

	// LeftShift held. One key, two meanings -- see IsShiftHeld().
	bool bShiftHeld = false;
	// LeftAlt held: fly-mode precision modifier (see kFlyPrecisionScale).
	bool bPrecisionHeld = false;
	bool bCrouchHeld = false;

	// --- Fly speed --------------------------------------------------------
	//
	// The world is kilometres across and a voxel is 10 cm, so one flat speed
	// cannot serve both "read the material seam on this cliff face" and "cross
	// four rings to the next tile". Three controls compose:
	//   * a base STEP table below, mouse wheel (also ']' / '[', and settable
	//     from the overlay),
	//   * LeftShift held  -> kFlyBoostScale (fast traverse),
	//   * LeftAlt held    -> kFlyPrecisionScale (10 cm inspection).
	// Combined range is 0.075 m/s .. 8000 m/s. Step 4 (3000 UU/s) is the
	// default and is byte-identical to the pre-task constant.
	static constexpr int32 kNumFlySpeedSteps = 9;
	static constexpr double kFlySpeedStepsUU[kNumFlySpeedSteps] = {
		50.0,     // 0.5 m/s  -- voxel-scale inspection
		150.0,    // 1.5 m/s
		400.0,    // 4 m/s    -- walking pace
		1000.0,   // 10 m/s
		3000.0,   // 30 m/s   -- DEFAULT
		8000.0,   // 80 m/s
		20000.0,  // 200 m/s
		60000.0,  // 600 m/s
		200000.0, // 2 km/s   -- cross the streamed world in a breath
	};
	static constexpr int32 kDefaultFlySpeedIndex = 4;
	static constexpr double kFlyBoostScale = 4.0;
	static constexpr double kFlyPrecisionScale = 0.15;
	// Acceleration as a multiple of max speed. 2.6667 keeps the default step at
	// exactly the pre-task 8000 UU/s^2 while staying proportional.
	static constexpr double kFlyAccelPerMaxSpeed = 8000.0 / 3000.0;

	int32 FlySpeedIndex = kDefaultFlySpeedIndex;

	// --- Camera height channels -------------------------------------------
	//
	// TWO channels, because they have genuinely different shapes and one cannot
	// serve both:
	//
	//   EyeOffsetUU tracks a HELD target -- the stance's eye height above the
	//   box centre (70 UU standing, 40 UU crouched). It must settle AT that
	//   value and stay there.
	//
	//   CameraZOffsetUU always decays to ZERO. It absorbs transients: the
	//   step-up snap and the crouch/stand centre shift (the mover reports both
	//   via ConsumeAbruptZJumpUU, and subtracting them here keeps the camera's
	//   world height continuous while the actor's Z jumps), plus the landing
	//   view punch.
	double EyeOffsetUU = VoxelMovementTuning::StandEyeOffsetUU;
	double CameraZOffsetUU = 0.0;

	// Head-bob offsets, applied to the first-person camera only. Kept in their
	// own terms rather than folded into CameraZOffsetUU, which must stay a pure
	// decay-to-zero channel. Named ...Offset... to keep them distinct from the
	// VoxelMovementTuning amplitude constants they are computed from.
	double HeadBobOffsetZUU = 0.0;
	double HeadBobOffsetYUU = 0.0;

	// Smoothed FOV so the high-tier kick eases in and out.
	float CurrentFOVDegrees = VoxelMovementTuning::BaseFOVDegrees;

	// Lagged third-person boom position (world space), eased toward the desired
	// boom point BEFORE the collision pull-in is applied.
	FVector ThirdPersonLaggedPos = FVector::ZeroVector;
	bool bThirdPersonPosInitialized = false;

	bool bThirdPerson = false;
	// Which shoulder the boom sits over; 'Q' mirrors it.
	bool bRightShoulder = true;
};
