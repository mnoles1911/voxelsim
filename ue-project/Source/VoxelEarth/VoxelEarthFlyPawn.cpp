#include "VoxelEarthFlyPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
// VoxelCoords.h is deliberately absent: the voxel lattice maths it provided
// moved out with the mover (VoxelCharacterMovement.cpp), and this file now only
// needs the subsystem for the third-person boom's DDA raycast.
#include "VoxelCharacterMovement.h"
#include "VoxelMovementTuning.h"
#include "VoxelProxyBody.h"
#include "VoxelWorldSubsystem.h"

namespace VMT = VoxelMovementTuning;

AVoxelEarthFlyPawn::AVoxelEarthFlyPawn()
{
	// Tick always runs (not just in walk mode): the walk mover is gated on
	// bWalkMode, but camera smoothing / the third-person boom and the proxy
	// body's animation need to update every frame in fly mode too.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SceneRoot);
	Camera->SetRelativeLocation(FVector(0.0, 0.0, VMT::StandEyeOffsetUU));
	Camera->SetFieldOfView(VMT::BaseFOVDegrees);
	Camera->bAutoActivate = true;

	// Over-the-shoulder third-person camera; its transform is recomputed every
	// frame in UpdateThirdPersonCamera rather than via a spring arm -- see the
	// class comment on the field. Starts inactive; BeginPlay makes the initial
	// FP/TP state explicit.
	ThirdPersonCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("ThirdPersonCamera"));
	ThirdPersonCamera->SetupAttachment(SceneRoot);
	ThirdPersonCamera->SetFieldOfView(VMT::BaseFOVDegrees);
	ThirdPersonCamera->bAutoActivate = false;

	// Blocky voxel proxy body; its own origin is the walk-mode collision box's
	// centre (VoxelMovementTuning::StandHalfExtentZ -- see VoxelProxyBody.h),
	// same as this pawn's root, so no extra offset is needed here.
	ProxyBody = CreateDefaultSubobject<UVoxelProxyBodyComponent>(TEXT("ProxyBody"));
	ProxyBody->SetupAttachment(SceneRoot);

	WalkMovement = CreateDefaultSubobject<UVoxelCharacterMovementComponent>(TEXT("WalkMovement"));

	Movement = CreateDefaultSubobject<UFloatingPawnMovement>(TEXT("Movement"));
	// Fly speed comes from the kFlySpeedStepsUU table (ApplyFlySpeedToMovement,
	// called every fly-mode Tick); these initial values are the default step so
	// a pawn that never ticks still behaves exactly as it did before the speed
	// control existed.
	Movement->MaxSpeed = (float)kFlySpeedStepsUU[kDefaultFlySpeedIndex]; // 30 m/s
	Movement->Acceleration = (float)(kFlySpeedStepsUU[kDefaultFlySpeedIndex] * kFlyAccelPerMaxSpeed);
	Movement->Deceleration = Movement->Acceleration;

	// FLY MODE CLIPS THROUGH EVERYTHING. Two things already guaranteed most of
	// it and one did not:
	//   * the root is a bare USceneComponent, so UFloatingPawnMovement's swept
	//     MoveUpdatedComponent has no collision shape to sweep and passes
	//     through terrain regardless;
	//   * voxel terrain and water chunk components carry no Chaos collision at
	//     all (docs/m1-plan.md SS3.3 "no Chaos for terrain"), and the proxy
	//     body's meshes are explicitly NoCollision;
	//   * but DEBRIS (AVoxelDebris) and explosives are ordinary physics actors
	//     WITH collision, and an overlap against those is the one way this pawn
	//     could ever be stopped or shoved.
	// Disabling actor collision outright makes "no collision at all in fly mode"
	// true by construction rather than by three separate coincidences. Walk mode
	// does not need it either: its collision is the DDA sweep against
	// UVoxelWorldSubsystem::IsSolidAtVoxel, which never consults Chaos -- so
	// this stays off in both modes and there is no mode-dependent collision
	// state to get wrong.
	SetActorEnableCollision(false);

	// Actor rotation follows the controller (mouse look), and movement input is
	// applied in actor-local axes -- the standard "fly cam" setup. Walk mode
	// keeps this too (mouse look still pitches the camera); the mover only
	// re-derives a yaw-only horizontal basis for movement.
	bUseControllerRotationYaw = true;
	bUseControllerRotationPitch = true;
	bUseControllerRotationRoll = false;
}

void AVoxelEarthFlyPawn::BeginPlay()
{
	Super::BeginPlay();

	// Explicit initial camera-mode / proxy-visibility state (first person, proxy
	// hidden) -- doesn't rely on component default-activation order.
	if (Camera)
	{
		Camera->SetActive(true);
	}
	if (ThirdPersonCamera)
	{
		ThirdPersonCamera->SetActive(false);
	}
	if (ProxyBody)
	{
		ProxyBody->SetVisibility(false, true);
	}
}

void AVoxelEarthFlyPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	if (!PlayerInputComponent)
	{
		return;
	}

	// Legacy input, no Enhanced Input assets (docs/m1-plan.md Stage 2 decisions
	// table). BindAxisKey only accepts true 1D axis keys (MouseX/MouseY) --
	// digital keys (W, A, SpaceBar, ...) must go through named engine-defined
	// axis mappings or they trip the AxisKey.IsAxis1D() ensure. Registration is
	// process-global and idempotent-enough for a dev pawn (duplicate
	// registrations of identical mappings are no-ops).
	UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping(TEXT("VoxelFly_Forward"), EKeys::W, 1.f));
	UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping(TEXT("VoxelFly_Forward"), EKeys::S, -1.f));
	UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping(TEXT("VoxelFly_Right"), EKeys::D, 1.f));
	UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping(TEXT("VoxelFly_Right"), EKeys::A, -1.f));
	UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping(TEXT("VoxelFly_Up"), EKeys::SpaceBar, 1.f));
	UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping(TEXT("VoxelFly_Up"), EKeys::LeftControl, -1.f));

	PlayerInputComponent->BindAxis(TEXT("VoxelFly_Forward"), this, &AVoxelEarthFlyPawn::MoveForward);
	PlayerInputComponent->BindAxis(TEXT("VoxelFly_Right"), this, &AVoxelEarthFlyPawn::MoveRight);
	PlayerInputComponent->BindAxis(TEXT("VoxelFly_Up"), this, &AVoxelEarthFlyPawn::MoveUp);

	PlayerInputComponent->BindAxisKey(EKeys::MouseX, this, &AVoxelEarthFlyPawn::Turn);
	PlayerInputComponent->BindAxisKey(EKeys::MouseY, this, &AVoxelEarthFlyPawn::LookUp);

	// G toggles walk mode. Space is repurposed from fly-up (the VoxelFly_Up axis
	// above) to jump while walking -- MoveUp ignores that axis in walk mode, so
	// the two bindings never fight over the key. The RELEASE binding is what
	// makes the jump variable-height (a tap gives a short hop).
	PlayerInputComponent->BindKey(EKeys::G, IE_Pressed, this, &AVoxelEarthFlyPawn::ToggleWalkMode);
	PlayerInputComponent->BindKey(EKeys::SpaceBar, IE_Pressed, this, &AVoxelEarthFlyPawn::OnJumpPressed);
	PlayerInputComponent->BindKey(EKeys::SpaceBar, IE_Released, this, &AVoxelEarthFlyPawn::OnJumpReleased);

	// LeftShift: fly boost / walk sprint (the walk side additionally gates on
	// heading forward -- see UVoxelCharacterMovementComponent).
	PlayerInputComponent->BindKey(EKeys::LeftShift, IE_Pressed, this, &AVoxelEarthFlyPawn::OnSprintPressed);
	PlayerInputComponent->BindKey(EKeys::LeftShift, IE_Released, this, &AVoxelEarthFlyPawn::OnSprintReleased);

	// 'C' is CROUCH (hold), so the camera FP/TP toggle moved to 'V' and the
	// shoulder swap took 'Q'. LeftControl deliberately stays the fly-down half
	// of VoxelFly_Up rather than doubling as crouch: crouch is walk-only, and a
	// key whose meaning depends on the mode is the pattern the fly-speed
	// bindings were written to avoid.
	PlayerInputComponent->BindKey(EKeys::C, IE_Pressed, this, &AVoxelEarthFlyPawn::OnCrouchPressed);
	PlayerInputComponent->BindKey(EKeys::C, IE_Released, this, &AVoxelEarthFlyPawn::OnCrouchReleased);
	PlayerInputComponent->BindKey(EKeys::V, IE_Pressed, this, &AVoxelEarthFlyPawn::ToggleCameraMode);
	PlayerInputComponent->BindKey(EKeys::Q, IE_Pressed, this, &AVoxelEarthFlyPawn::ToggleShoulder);

	// The mouse wheel is the SPEED DIAL in both modes: the walk-mode gait tier
	// (creep .. mad dash) and the fly-mode speed step are the same concept, so
	// the wheel never silently means two different things. It previously cycled
	// dig size, which kept its existing 1/2/3 shortcuts in
	// AVoxelEarthPlayerController and lost nothing. Brackets stay as keyboard
	// aliases for the fly step.
	PlayerInputComponent->BindKey(EKeys::MouseScrollUp, IE_Pressed, this, &AVoxelEarthFlyPawn::OnSpeedDialUp);
	PlayerInputComponent->BindKey(EKeys::MouseScrollDown, IE_Pressed, this, &AVoxelEarthFlyPawn::OnSpeedDialDown);
	PlayerInputComponent->BindKey(EKeys::RightBracket, IE_Pressed, this, &AVoxelEarthFlyPawn::OnFlySpeedUp);
	PlayerInputComponent->BindKey(EKeys::LeftBracket, IE_Pressed, this, &AVoxelEarthFlyPawn::OnFlySpeedDown);
	PlayerInputComponent->BindKey(EKeys::LeftAlt, IE_Pressed, this, &AVoxelEarthFlyPawn::OnPrecisionPressed);
	PlayerInputComponent->BindKey(EKeys::LeftAlt, IE_Released, this, &AVoxelEarthFlyPawn::OnPrecisionReleased);
}

// ---------------------------------------------------------------------------
// Input callbacks
// ---------------------------------------------------------------------------

void AVoxelEarthFlyPawn::MoveForward(float Value)
{
	CurrentForwardInput = Value;
	if (!bWalkMode)
	{
		AddMovementInput(GetActorForwardVector(), Value);
	}
}

void AVoxelEarthFlyPawn::MoveRight(float Value)
{
	CurrentRightInput = Value;
	if (!bWalkMode)
	{
		AddMovementInput(GetActorRightVector(), Value);
	}
}

void AVoxelEarthFlyPawn::MoveUp(float Value)
{
	// Cached unconditionally: the mover's swim branch reads this for
	// fly-style vertical movement while submerged. Walk mode otherwise ignores
	// it -- Space/LeftControl don't fly up/down on land (Space is jump instead,
	// via OnJumpPressed).
	CurrentUpInput = Value;
	if (!bWalkMode)
	{
		AddMovementInput(FVector::UpVector, Value);
	}
}

void AVoxelEarthFlyPawn::Turn(float Value) { AddControllerYawInput(Value * MouseLookSpeed); }
void AVoxelEarthFlyPawn::LookUp(float Value) { AddControllerPitchInput(-Value * MouseLookSpeed); }

void AVoxelEarthFlyPawn::ToggleWalkMode()
{
	SetWalkMode(!bWalkMode);
}

void AVoxelEarthFlyPawn::OnSpeedDialUp() { AdjustSpeedDial(+1); }
void AVoxelEarthFlyPawn::OnSpeedDialDown() { AdjustSpeedDial(-1); }

void AVoxelEarthFlyPawn::OnFlySpeedUp() { AdjustFlySpeed(+1); }
void AVoxelEarthFlyPawn::OnFlySpeedDown() { AdjustFlySpeed(-1); }
void AVoxelEarthFlyPawn::OnPrecisionPressed() { bPrecisionHeld = true; }
void AVoxelEarthFlyPawn::OnPrecisionReleased() { bPrecisionHeld = false; }

void AVoxelEarthFlyPawn::OnJumpPressed()
{
	if (bWalkMode && WalkMovement)
	{
		WalkMovement->RequestJump();
	}
	// Fly mode: SpaceBar already flies up via the VoxelFly_Up axis; no separate
	// jump action there.
}

void AVoxelEarthFlyPawn::OnJumpReleased()
{
	if (bWalkMode && WalkMovement)
	{
		WalkMovement->ReleaseJump();
	}
}

void AVoxelEarthFlyPawn::OnSprintPressed()
{
	bShiftHeld = true;
	if (WalkMovement)
	{
		WalkMovement->SetSprintHeld(true);
	}
}

void AVoxelEarthFlyPawn::OnSprintReleased()
{
	bShiftHeld = false;
	if (WalkMovement)
	{
		WalkMovement->SetSprintHeld(false);
	}
}

void AVoxelEarthFlyPawn::OnCrouchPressed()
{
	bCrouchHeld = true;
	if (WalkMovement)
	{
		WalkMovement->SetCrouchHeld(true);
	}
}

void AVoxelEarthFlyPawn::OnCrouchReleased()
{
	bCrouchHeld = false;
	if (WalkMovement)
	{
		// Not necessarily an immediate stand: the mover refuses while a ceiling
		// is in the way and stands automatically once clear.
		WalkMovement->SetCrouchHeld(false);
	}
}

void AVoxelEarthFlyPawn::ToggleCameraMode()
{
	bThirdPerson = !bThirdPerson;
	if (Camera)
	{
		Camera->SetActive(!bThirdPerson);
	}
	if (ThirdPersonCamera)
	{
		ThirdPersonCamera->SetActive(bThirdPerson);
	}
	if (ProxyBody)
	{
		// Character proxy row: "Visible in TP, hidden in FP."
		ProxyBody->SetVisibility(bThirdPerson, true);
	}
}

void AVoxelEarthFlyPawn::ToggleShoulder()
{
	bRightShoulder = !bRightShoulder;
	// Re-seed the lag so the boom sweeps across to the new shoulder rather than
	// snapping -- the lag term below does the rest.
}

// ---------------------------------------------------------------------------
// Fly speed
// ---------------------------------------------------------------------------

void AVoxelEarthFlyPawn::AdjustFlySpeed(int32 Delta)
{
	FlySpeedIndex = FMath::Clamp(FlySpeedIndex + Delta, 0, kNumFlySpeedSteps - 1);
	ApplyFlySpeedToMovement();
}

void AVoxelEarthFlyPawn::AdjustSpeedDial(int32 Delta)
{
	const int32 Step = Delta >= 0 ? +1 : -1;
	if (bWalkMode)
	{
		if (WalkMovement)
		{
			WalkMovement->AdjustSpeedTier(Step);
		}
	}
	else
	{
		AdjustFlySpeed(Step);
	}
}

double AVoxelEarthFlyPawn::GetFlyBaseSpeedUU() const
{
	return kFlySpeedStepsUU[FMath::Clamp(FlySpeedIndex, 0, kNumFlySpeedSteps - 1)];
}

double AVoxelEarthFlyPawn::GetEffectiveFlySpeedUU() const
{
	double Speed = GetFlyBaseSpeedUU();
	// Boost and precision compose (both held = 0.6x); that is the useful
	// behaviour, not an accident -- there is no reason to make one win.
	if (bShiftHeld)
	{
		Speed *= kFlyBoostScale;
	}
	if (bPrecisionHeld)
	{
		Speed *= kFlyPrecisionScale;
	}
	return Speed;
}

void AVoxelEarthFlyPawn::ApplyFlySpeedToMovement()
{
	if (!Movement)
	{
		return;
	}
	const double MaxSpeed = GetEffectiveFlySpeedUU();
	Movement->MaxSpeed = (float)MaxSpeed;
	// Proportional accel: a fixed 8000 UU/s^2 would take ~25 s to reach the top
	// step and would make the slowest step feel teleporty.
	Movement->Acceleration = (float)(MaxSpeed * kFlyAccelPerMaxSpeed);
	Movement->Deceleration = Movement->Acceleration;
}

// ---------------------------------------------------------------------------
// Walk-mode forwarding
// ---------------------------------------------------------------------------

bool AVoxelEarthFlyPawn::IsWaitingForTerrain() const
{
	return bWalkMode && WalkMovement && WalkMovement->IsWaitingForTerrain();
}

bool AVoxelEarthFlyPawn::IsGroundedNow() const
{
	return bWalkMode && WalkMovement && WalkMovement->IsGrounded();
}

bool AVoxelEarthFlyPawn::IsSwimmingNow() const
{
	return bWalkMode && WalkMovement && WalkMovement->IsSwimming();
}

bool AVoxelEarthFlyPawn::IsCrouched() const
{
	return WalkMovement && WalkMovement->IsCrouched();
}

bool AVoxelEarthFlyPawn::IsCrouchBlocked() const
{
	return WalkMovement && WalkMovement->IsCrouchBlocked();
}

bool AVoxelEarthFlyPawn::IsSprintEngaged() const
{
	return bWalkMode && WalkMovement && WalkMovement->IsSprintEngaged();
}

int32 AVoxelEarthFlyPawn::GetSpeedTierIndex() const
{
	return WalkMovement ? WalkMovement->GetSpeedTierIndex() : VMT::kDefaultSpeedTierIndex;
}

const TCHAR* AVoxelEarthFlyPawn::GetSpeedTierName() const
{
	return WalkMovement ? WalkMovement->GetSpeedTierName() : VMT::NameForTier(VMT::kDefaultSpeedTierIndex);
}

double AVoxelEarthFlyPawn::GetEffectiveWalkSpeedUU() const
{
	return WalkMovement ? WalkMovement->GetEffectiveMaxSpeedUU() : VMT::SpeedForTier(VMT::kDefaultSpeedTierIndex);
}

void AVoxelEarthFlyPawn::SetWalkMode(bool bInWalkMode)
{
	if (bWalkMode == bInWalkMode)
	{
		return;
	}
	bWalkMode = bInWalkMode;

	if (WalkMovement)
	{
		WalkMovement->ResetState();
		// Re-sync held keys: a mode toggle while Shift or C is down must not
		// leave the mover believing the opposite.
		WalkMovement->SetSprintHeld(bShiftHeld);
		WalkMovement->SetCrouchHeld(bCrouchHeld);

		if (!bWalkMode)
		{
			// Leaving walk mode: the mover stops ticking, so a crouch entered
			// on the ground would otherwise persist for the entire flight --
			// camera stuck at crouch eye height, proxy body stuck squashed,
			// with no tick left to undo it. Fly mode has no collision at all
			// (SetActorEnableCollision(false) plus no Chaos on terrain), so
			// standing unconditionally here is always safe.
			WalkMovement->ForceStand();
		}
	}

	if (Movement)
	{
		// Fly mode uses UFloatingPawnMovement; walk mode drives position
		// entirely from the walk mover, so the movement component is parked
		// (ticking it too would fight the kinematic mover for SceneRoot's
		// transform).
		Movement->StopMovementImmediately();
		Movement->SetComponentTickEnabled(!bWalkMode);
	}

	// Actor tick itself stays on unconditionally in both modes (see the
	// constructor comment) so camera smoothing / the third-person boom / proxy
	// animation keep updating in fly mode too.
}

void AVoxelEarthFlyPawn::TickWalkMode(float DeltaTime)
{
	if (!WalkMovement)
	{
		return;
	}
	WalkMovement->SetMoveInput(CurrentForwardInput, CurrentRightInput, CurrentUpInput);
	WalkMovement->TickMovement(DeltaTime);
}

// ---------------------------------------------------------------------------
// Tick + cameras
// ---------------------------------------------------------------------------

void AVoxelEarthFlyPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bWalkMode)
	{
		TickWalkMode(DeltaTime);
	}
	else
	{
		// Fly speed step + held modifiers (LeftShift boost / LeftAlt precision)
		// -> UFloatingPawnMovement. Done per-frame rather than only on key
		// events because the modifiers are HELD keys: releasing Shift must drop
		// the top speed immediately.
		ApplyFlySpeedToMovement();
	}

	UpdateCameraSmoothing(DeltaTime);

	// Player scale reference (voxel.Debug.PlayerBox, default ON). Walk mode
	// only -- fly mode has no collision volume to describe, so drawing one
	// there would be fiction. Issued AFTER UpdateCameraSmoothing so the eye
	// height passed in is this frame's blended value rather than last frame's;
	// the marker is meant to show the crouch ease and step-smoothing lag, which
	// a frame-stale value would misreport by exactly the amount of interest.
	if (bWalkMode && WalkMovement)
	{
		WalkMovement->DebugDrawVolume(GetHeadWorldLocation().Z);
	}

	if (ProxyBody)
	{
		// KEEP THE BODY UPRIGHT. This pawn pitches its ROOT with the controller
		// (bUseControllerRotationPitch, set in the constructor) because fly mode
		// steers along GetActorForwardVector and the third-person boom needs the
		// pitched view basis. The proxy body is attached to that same root, so
		// without this it pitched too: looking down tipped the character forward
		// and a full -90 degrees laid it flat on its face. First person hides the
		// body, which is why this survived unnoticed -- it is only ever visible
		// in third person.
		//
		// Yaw-only in WORLD space, so the body turns with you (which is correct
		// and expected) and nothing else. This also fixes the crouch squash,
		// which is applied along the component's local Z and was therefore
		// leaning with the pitch instead of compressing straight down.
		//
		// Not fixed by dropping the root pitch instead: fly mode genuinely needs
		// the actor to pitch, and walk mode already re-derives its own yaw-only
		// basis in the mover, so the root's pitch is load-bearing for exactly one
		// consumer and wrong for exactly one other.
		ProxyBody->SetWorldRotation(FRotator(0.0, GetActorRotation().Yaw, 0.0));

		const double HorizSpeedUU = bWalkMode && WalkMovement ? WalkMovement->GetHorizontalSpeedUU() : GetVelocity().Size();
		const float GaitPhase = WalkMovement ? WalkMovement->GetGaitPhase() : 0.f;
		const float CrouchAlpha = IsCrouched() ? 1.f : 0.f;
		ProxyBody->UpdateAnimation(DeltaTime, HorizSpeedUU, GaitPhase, CrouchAlpha);
	}
}

FVector AVoxelEarthFlyPawn::GetHeadWorldLocation() const
{
	return GetActorLocation() + FVector(0.0, 0.0, EyeOffsetUU + CameraZOffsetUU);
}

void AVoxelEarthFlyPawn::UpdateCameraSmoothing(float DeltaTime)
{
	// --- Transient channel: always decays to zero -------------------------
	//
	// The mover reports abrupt Z changes it applied this tick -- a step-up snap
	// or a crouch/stand centre shift. SUBTRACTING them here keeps the camera's
	// world height continuous while the actor's Z jumps; the decay below then
	// eases it out so steps read as ramps rather than teleports.
	if (WalkMovement)
	{
		CameraZOffsetUU -= WalkMovement->ConsumeAbruptZJumpUU();

		// Landing view punch: dip the camera by an impact-scaled amount into
		// the same decaying channel. Below the minimum impact speed nothing
		// happens at all, so walking down a step never punches.
		const double ImpactUU = WalkMovement->ConsumeLandingImpactUU();
		if (ImpactUU > VMT::LandingPunchMinSpeedUU)
		{
			const double Strength = FMath::Clamp(
				(ImpactUU - VMT::LandingPunchMinSpeedUU) / (VMT::LandingPunchRefSpeedUU - VMT::LandingPunchMinSpeedUU), 0.0, 1.0);
			CameraZOffsetUU -= VMT::LandingPunchMaxUU * Strength;
		}
	}

	const double DecayAlpha = 1.0 - FMath::Exp(-VMT::CameraSmoothRatePerSec * (double)DeltaTime);
	CameraZOffsetUU = FMath::Lerp(CameraZOffsetUU, 0.0, DecayAlpha);
	if (FMath::Abs(CameraZOffsetUU) < 0.01)
	{
		CameraZOffsetUU = 0.0;
	}

	// --- Held channel: tracks the stance's eye height ----------------------
	//
	// Separate from the decay channel above because it must SETTLE at its
	// target (70 UU standing, 40 UU crouched) and stay there, not return to
	// zero.
	const double TargetEyeUU = WalkMovement ? WalkMovement->GetTargetEyeOffsetUU() : VMT::StandEyeOffsetUU;
	const double CrouchAlpha = 1.0 - FMath::Exp(-VMT::CrouchCameraRatePerSec * (double)DeltaTime);
	EyeOffsetUU = FMath::Lerp(EyeOffsetUU, TargetEyeUU, CrouchAlpha);

	// --- Head bob ----------------------------------------------------------
	//
	// Driven by the mover's gait phase, the SAME phase the proxy body's limb
	// swing uses, so the camera dips in step with the visible footfalls.
	// Vertical runs at twice the gait frequency (one dip per foot), lateral at
	// the gait frequency (one sway per stride). Only while genuinely walking on
	// the ground -- bobbing mid-air or mid-swim reads as a bug.
	double TargetBobZ = 0.0;
	double TargetBobY = 0.0;
	if (bWalkMode && WalkMovement && WalkMovement->IsGrounded() && !WalkMovement->IsSwimming())
	{
		const double SpeedFraction =
			FMath::Clamp(WalkMovement->GetHorizontalSpeedUU() / VMT::SpeedForTier(VMT::kSprintTierIndex), 0.0, 1.0);
		const float Phase = WalkMovement->GetGaitPhase();
		TargetBobZ = FMath::Sin(Phase * 2.f) * VMT::HeadBobVerticalUU * SpeedFraction;
		TargetBobY = FMath::Sin(Phase) * VMT::HeadBobLateralUU * SpeedFraction;
	}
	// Eased rather than applied raw so the bob fades out when movement stops
	// instead of freezing at whatever phase it was in.
	HeadBobOffsetZUU = FMath::Lerp(HeadBobOffsetZUU, TargetBobZ, DecayAlpha);
	HeadBobOffsetYUU = FMath::Lerp(HeadBobOffsetYUU, TargetBobY, DecayAlpha);

	if (Camera)
	{
		Camera->SetRelativeLocation(
			FVector(0.0, HeadBobOffsetYUU, EyeOffsetUU + CameraZOffsetUU + HeadBobOffsetZUU));
	}

	// --- FOV kick ----------------------------------------------------------
	//
	// Widens across the top tiers only (kFOVKickStartTier and above), so the
	// ordinary walking range is completely unaffected and inspecting terrain
	// never has a moving FOV.
	float TargetFOV = VMT::BaseFOVDegrees;
	if (bWalkMode && WalkMovement)
	{
		const int32 Tier = WalkMovement->GetSpeedTierIndex();
		const int32 TopTier = VMT::kNumSpeedTiers - 1;
		if (Tier >= VMT::kFOVKickStartTier && TopTier > VMT::kFOVKickStartTier)
		{
			// Scaled by how fast the character is ACTUALLY moving, not just the
			// dial: standing still at the top tier should not widen the view.
			const double SpeedFraction =
				FMath::Clamp(WalkMovement->GetHorizontalSpeedUU() / VMT::SpeedForTier(VMT::kSprintTierIndex), 0.0, 1.0);
			const float TierFraction = float(Tier - VMT::kFOVKickStartTier) / float(TopTier - VMT::kFOVKickStartTier);
			TargetFOV += VMT::MaxFOVKickDegrees * TierFraction * (float)SpeedFraction;
		}
	}
	const float FOVAlpha = 1.f - FMath::Exp(-(float)VMT::FOVKickRatePerSec * DeltaTime);
	CurrentFOVDegrees = FMath::Lerp(CurrentFOVDegrees, TargetFOV, FOVAlpha);
	if (Camera)
	{
		Camera->SetFieldOfView(CurrentFOVDegrees);
	}
	if (ThirdPersonCamera)
	{
		ThirdPersonCamera->SetFieldOfView(CurrentFOVDegrees);
	}

	UpdateThirdPersonCamera(DeltaTime);
}

void AVoxelEarthFlyPawn::UpdateThirdPersonCamera(float DeltaTime)
{
	if (!ThirdPersonCamera)
	{
		return;
	}

	const FVector HeadPos = GetHeadWorldLocation();
	const FVector Forward = GetActorForwardVector();
	const FVector Right = GetActorRightVector();
	const double ShoulderSign = bRightShoulder ? 1.0 : -1.0;
	const FVector DesiredPos =
		HeadPos - Forward * VMT::ThirdPersonBoomBackUU + Right * (VMT::ThirdPersonBoomRightUU * ShoulderSign);

	// Exponential lag toward the desired boom point. Applied BEFORE the
	// collision pull-in below, never after: a camera that eased toward its
	// target after being pulled in would smooth itself straight back into the
	// rock the pull-in just rescued it from.
	if (!bThirdPersonPosInitialized)
	{
		ThirdPersonLaggedPos = DesiredPos;
		bThirdPersonPosInitialized = true;
	}
	else
	{
		const double LagAlpha = 1.0 - FMath::Exp(-VMT::ThirdPersonLagRatePerSec * (double)DeltaTime);
		ThirdPersonLaggedPos = FMath::Lerp(ThirdPersonLaggedPos, DesiredPos, LagAlpha);
	}

	FVector FinalPos = ThirdPersonLaggedPos;
	const FVector ToLagged = ThirdPersonLaggedPos - HeadPos;
	const double LaggedDist = ToLagged.Size();
	if (LaggedDist > KINDA_SMALL_NUMBER)
	{
		const FVector Dir = ToLagged / LaggedDist;
		if (UWorld* World = GetWorld())
		{
			if (UVoxelWorldSubsystem* Subsystem = World->GetSubsystem<UVoxelWorldSubsystem>())
			{
				FVector HitCenter, PrevCenter;
				// Collision-aware pull-in: no USpringArmComponent probes --
				// terrain has no Chaos collision -- so the boom is pulled in
				// with the same voxel DDA raycast dig/place uses, cast from the
				// head toward the (lagged) camera position.
				if (Subsystem->RaycastVoxelWorld(HeadPos, Dir, LaggedDist, HitCenter, PrevCenter))
				{
					// Project the last-empty-voxel centre back onto the ray so
					// the pulled-in camera always stays exactly on the
					// head->desired line (PrevCenter itself is a voxel centre,
					// not necessarily on that line).
					const double PrevDistAlongDir = (PrevCenter - HeadPos) | Dir;
					const double PulledDist = FMath::Max(0.0, PrevDistAlongDir - VMT::ThirdPersonPullInEpsilonUU);
					FinalPos = HeadPos + Dir * PulledDist;
				}
			}
		}
	}

	ThirdPersonCamera->SetWorldLocation(FinalPos);
	ThirdPersonCamera->SetWorldRotation(GetActorRotation());
}
