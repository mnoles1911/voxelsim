#include "VoxelEarthFlyPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "VoxelCoords.h"
#include "VoxelDebug.h"
#include "VoxelProxyBody.h"
#include "VoxelWorldSubsystem.h"

namespace
{
// Converts a world-space interval [Lo, Hi] (UU) into the inclusive integer
// voxel-index range it overlaps. KINDA_SMALL_NUMBER keeps a box face that
// lands exactly on a voxel boundary from spuriously pulling in the next
// voxel over.
void AxisVoxelRange(double Lo, double Hi, int64& OutMin, int64& OutMax)
{
	OutMin = (int64)FMath::FloorToDouble(Lo / VoxelCoords::VoxelSizeUU);
	OutMax = (int64)FMath::FloorToDouble((Hi - KINDA_SMALL_NUMBER) / VoxelCoords::VoxelSizeUU);
	if (OutMax < OutMin)
	{
		OutMax = OutMin;
	}
}
} // namespace

AVoxelEarthFlyPawn::AVoxelEarthFlyPawn()
{
	// Tick now always runs (not just in walk mode): TickWalkMode is still
	// gated on bWalkMode, but camera smoothing/the third-person boom
	// (UpdateCameraSmoothing) and proxy body animation need to update every
	// frame in fly mode too (task 5: fly mode keeps TP camera + visible
	// proxy).
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SceneRoot);
	Camera->SetRelativeLocation(FVector(0.0, 0.0, HeadOffsetUU));
	Camera->bAutoActivate = true;

	// Over-the-shoulder third-person camera (Player experience decisions
	// table, "Cameras" row); its transform is recomputed every frame in
	// UpdateThirdPersonCamera rather than via a spring arm -- see class
	// comment on the field. Starts inactive; BeginPlay makes the initial
	// FP/TP state explicit.
	ThirdPersonCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("ThirdPersonCamera"));
	ThirdPersonCamera->SetupAttachment(SceneRoot);
	ThirdPersonCamera->bAutoActivate = false;

	// Blocky voxel proxy body (Player experience decisions table,
	// "Character proxy" row); its own origin is the walk-mode collision
	// box's center (matches WalkBoxHalfExtentZ/XY -- see VoxelProxyBody.h),
	// same as this pawn's root, so no extra offset is needed here.
	ProxyBody = CreateDefaultSubobject<UVoxelProxyBodyComponent>(TEXT("ProxyBody"));
	ProxyBody->SetupAttachment(SceneRoot);

	Movement = CreateDefaultSubobject<UFloatingPawnMovement>(TEXT("Movement"));
	// Fly speed now comes from the kFlySpeedStepsUU table (ApplyFlySpeedToMovement,
	// called every fly-mode Tick); these initial values are the default step
	// (index 4 == 3000 UU/s) so a pawn that never ticks still behaves exactly
	// as it did before the speed control existed.
	Movement->MaxSpeed = (float)kFlySpeedStepsUU[kDefaultFlySpeedIndex]; // 30 m/s
	Movement->Acceleration = (float)(kFlySpeedStepsUU[kDefaultFlySpeedIndex] * kFlyAccelPerMaxSpeed);
	Movement->Deceleration = Movement->Acceleration;

	// FLY MODE CLIPS THROUGH EVERYTHING (task requirement). Two things already
	// guaranteed most of it and one did not:
	//   * the root is a bare USceneComponent, so UFloatingPawnMovement's swept
	//     MoveUpdatedComponent has no collision shape to sweep and passes
	//     through terrain regardless;
	//   * voxel terrain and water chunk components carry no Chaos collision at
	//     all (docs/m1-plan.md SS3.3 "no Chaos for terrain"), and the proxy
	//     body's meshes are explicitly NoCollision;
	//   * but DEBRIS (AVoxelDebris) and explosives are ordinary physics actors
	//     WITH collision, and an overlap against those is the one way this pawn
	//     could ever be stopped or shoved.
	// Disabling actor collision outright makes "no collision at all in fly
	// mode" true by construction rather than by three separate coincidences.
	// Walk mode does not need it either: its collision is the DDA sweep against
	// UVoxelWorldSubsystem::IsSolidAtVoxel (SweepAxis), which never consults
	// Chaos -- so this stays off in both modes and there is no mode-dependent
	// collision state to get wrong.
	SetActorEnableCollision(false);

	// Actor rotation follows the controller (mouse look), and movement input
	// is applied in actor-local axes -- the standard "fly cam" setup. Walk
	// mode keeps this too (mouse look still pitches the camera); it only
	// re-derives a yaw-only horizontal basis for movement (see
	// TickWalkMode) instead of using GetActorForwardVector() directly.
	bUseControllerRotationYaw = true;
	bUseControllerRotationPitch = true;
	bUseControllerRotationRoll = false;
}

void AVoxelEarthFlyPawn::BeginPlay()
{
	Super::BeginPlay();

	// Explicit initial camera-mode / proxy-visibility state (first person,
	// proxy hidden) -- doesn't rely on component default-activation order.
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

	// Legacy input, no Enhanced Input assets (docs/m1-plan.md Stage 2
	// decisions table). BindAxisKey only accepts true 1D axis keys (MouseX/
	// MouseY) — digital keys (W, A, SpaceBar, ...) must go through named
	// engine-defined axis mappings or they trip the AxisKey.IsAxis1D()
	// ensure. Registration is process-global and idempotent-enough for a
	// dev pawn (duplicate registrations of identical mappings are no-ops).
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

	// Stage 3b: G toggles walk mode; Space is repurposed from fly-up (the
	// VoxelFly_Up axis above) to jump while walking -- MoveUp ignores that
	// axis in walk mode, so the two bindings never fight over the key.
	PlayerInputComponent->BindKey(EKeys::G, IE_Pressed, this, &AVoxelEarthFlyPawn::ToggleWalkMode);
	PlayerInputComponent->BindKey(EKeys::SpaceBar, IE_Pressed, this, &AVoxelEarthFlyPawn::OnJumpPressed);

	// Player experience decisions table: sprint (LeftShift held) and camera
	// mode toggle ('C').
	PlayerInputComponent->BindKey(EKeys::LeftShift, IE_Pressed, this, &AVoxelEarthFlyPawn::OnSprintPressed);
	PlayerInputComponent->BindKey(EKeys::LeftShift, IE_Released, this, &AVoxelEarthFlyPawn::OnSprintReleased);
	PlayerInputComponent->BindKey(EKeys::C, IE_Pressed, this, &AVoxelEarthFlyPawn::ToggleCameraMode);

	// Fly speed step (usability task): ']' faster, '[' slower, LeftAlt held =
	// precision. Brackets rather than the mouse wheel because the wheel is
	// already the dig/place size selector (AVoxelEarthPlayerController), and a
	// key that silently does two things depending on mode is exactly the kind
	// of thing this task exists to remove.
	PlayerInputComponent->BindKey(EKeys::RightBracket, IE_Pressed, this, &AVoxelEarthFlyPawn::OnFlySpeedUp);
	PlayerInputComponent->BindKey(EKeys::LeftBracket, IE_Pressed, this, &AVoxelEarthFlyPawn::OnFlySpeedDown);
	PlayerInputComponent->BindKey(EKeys::LeftAlt, IE_Pressed, this, &AVoxelEarthFlyPawn::OnPrecisionPressed);
	PlayerInputComponent->BindKey(EKeys::LeftAlt, IE_Released, this, &AVoxelEarthFlyPawn::OnPrecisionReleased);
}

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
	// Cached unconditionally: walk mode's swim branch (TickWalkMode) reads
	// this for vertical fly-style movement while submerged. Walk mode
	// otherwise ignores it -- Space/LeftControl don't fly up/down on land
	// (Space is jump instead, via OnJumpPressed).
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

void AVoxelEarthFlyPawn::OnFlySpeedUp() { AdjustFlySpeed(+1); }
void AVoxelEarthFlyPawn::OnFlySpeedDown() { AdjustFlySpeed(-1); }
void AVoxelEarthFlyPawn::OnPrecisionPressed() { bPrecisionHeld = true; }
void AVoxelEarthFlyPawn::OnPrecisionReleased() { bPrecisionHeld = false; }

void AVoxelEarthFlyPawn::AdjustFlySpeed(int32 Delta)
{
	FlySpeedIndex = FMath::Clamp(FlySpeedIndex + Delta, 0, kNumFlySpeedSteps - 1);
	ApplyFlySpeedToMovement();
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
	if (bSprintHeld)
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
	// Proportional accel: a fixed 8000 UU/s^2 would take ~25 s to reach the
	// top step and would make the slowest step feel teleporty.
	Movement->Acceleration = (float)(MaxSpeed * kFlyAccelPerMaxSpeed);
	Movement->Deceleration = Movement->Acceleration;
}

void AVoxelEarthFlyPawn::SetWalkMode(bool bInWalkMode)
{
	if (bWalkMode == bInWalkMode)
	{
		return;
	}
	bWalkMode = bInWalkMode;
	VerticalVelocity = 0.f;
	bJumpRequested = false;
	HorizontalVelocity = FVector::ZeroVector;
	// Only ever meaningful in walk mode; clear it so the HUD never shows a
	// stale "waiting for terrain" after switching back to fly.
	bWaitingForTerrain = false;
	bGroundedLastTick = false;
	bSwimmingLastTick = false;

	if (Movement)
	{
		// Fly mode (default, unchanged behavior) uses UFloatingPawnMovement;
		// walk mode drives position entirely from TickWalkMode below, so the
		// movement component is parked (ticking it too would fight the
		// kinematic mover for SceneRoot's transform).
		Movement->StopMovementImmediately();
		Movement->SetComponentTickEnabled(!bWalkMode);
	}

	// Actor tick itself now stays on unconditionally in both modes (see
	// constructor comment) so camera smoothing / the third-person boom /
	// proxy animation keep updating in fly mode too; only TickWalkMode's
	// kinematic step is gated on bWalkMode (see Tick).
}

void AVoxelEarthFlyPawn::OnJumpPressed()
{
	if (bWalkMode)
	{
		bJumpRequested = true;
	}
	// Fly mode: SpaceBar already flies up via the VoxelFly_Up axis; no
	// separate jump action there.
}

void AVoxelEarthFlyPawn::OnSprintPressed() { bSprintHeld = true; }
void AVoxelEarthFlyPawn::OnSprintReleased() { bSprintHeld = false; }

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

UVoxelWorldSubsystem* AVoxelEarthFlyPawn::GetVoxelWorldSubsystem() const
{
	UWorld* World = GetWorld();
	return World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
}

bool AVoxelEarthFlyPawn::IsTerrainReadyAt(const FVector& Pos) const
{
	// THE PROBLEM. Terrain streams in asynchronously, so a walking pawn can
	// exist before the ground beneath it does. IsSolidAtVoxel answers "is
	// there a solid voxel here" and CANNOT distinguish "empty air" from "not
	// generated yet" -- both are false. Left alone, a pawn that spawns or
	// toggles into walk mode a fraction of a second early falls forever, and
	// (worse) it does so silently and looks like a collision bug.
	//
	// THE RULE. Gravity runs unless the pawn's OWN chunk is TRACKED (in the
	// streaming desired set) but does not yet own a COMPONENT -- precisely
	// "queued, not here yet", the boot case and the reason the pawn must not
	// fall the moment it spawns. Anything else counts as ready:
	//
	//   * tracked WITH a component -- the ground is really there (or the chunk
	//     is genuinely empty air, a legitimate reason to fall);
	//   * not tracked at all -- outside the streaming footprint, e.g. an
	//     all-air chunk the sky-band optimisation never admits. Vetoing there
	//     would leave the pawn hovering wherever it stepped into open sky.
	//
	// ONLY the pawn's own chunk is probed. An earlier version also probed the
	// chunk one below, to stop a fall into terrain a frame away -- but that
	// held a pawn hovering a metre above real ground whenever the solid rock
	// directly beneath it was un-meshed (underground rock is never meshed, so
	// its chunk is legitimately "tracked, no component"). The genuine
	// fall-through case -- dropping into space that was never generated -- is
	// caught instead by the analytic-surface backstop at the end of
	// TickWalkMode, which needs no streaming at all. So this probe stays local
	// and the caller additionally short-circuits it when actually grounded.
	//
	// NOT snapshot-based. FVoxelPerfSnapshot looked like the obvious global
	// "has anything loaded yet" signal, but the subsystem only refreshes it
	// while voxel.Debug >= 1 (a deliberate zero-cost-at-mode-0 gate), so a
	// snapshot test would read all-zero in a normal session and hold the pawn
	// in the air forever. DebugChunkStatusAt is a live query with no such gate.
	UVoxelWorldSubsystem* Subsystem = GetVoxelWorldSubsystem();
	if (!Subsystem)
	{
		return false;
	}

	bool bTracked = false;
	bool bHasComponent = false;
	int32 Quads = 0;
	if (Subsystem->DebugChunkStatusAt(Pos, bTracked, bHasComponent, Quads) && bTracked && !bHasComponent)
	{
		return false;
	}
	return true;
}

bool AVoxelEarthFlyPawn::IsGroundedAt(const FVector& Pos) const
{
	UVoxelWorldSubsystem* Subsystem = GetVoxelWorldSubsystem();
	if (!Subsystem)
	{
		return false;
	}

	// Downward 1-voxel probe under the box: solid anywhere in the voxel
	// layer immediately beneath the box's footprint counts as grounded.
	const double BottomZ = Pos.Z - WalkBoxHalfExtentZ;
	const int64 ProbeVZ = (int64)FMath::FloorToDouble(BottomZ / VoxelCoords::VoxelSizeUU) - 1;

	int64 VXMin, VXMax, VYMin, VYMax;
	AxisVoxelRange(Pos.X - WalkBoxHalfExtentXY, Pos.X + WalkBoxHalfExtentXY, VXMin, VXMax);
	AxisVoxelRange(Pos.Y - WalkBoxHalfExtentXY, Pos.Y + WalkBoxHalfExtentXY, VYMin, VYMax);

	for (int64 VX = VXMin; VX <= VXMax; ++VX)
	{
		for (int64 VY = VYMin; VY <= VYMax; ++VY)
		{
			if (Subsystem->IsSolidAtVoxel(VX, VY, ProbeVZ))
			{
				return true;
			}
		}
	}
	return false;
}

bool AVoxelEarthFlyPawn::SweepAxis(int32 Axis, double Delta, FVector& InOutPos) const
{
	if (FMath::IsNearlyZero(Delta))
	{
		return false;
	}

	UVoxelWorldSubsystem* Subsystem = GetVoxelWorldSubsystem();
	if (!Subsystem)
	{
		// World not ready (shouldn't happen once walking, defensive only):
		// move freely rather than getting stuck against nothing.
		InOutPos[Axis] += Delta;
		return false;
	}

	const double HalfExtent[3] = {WalkBoxHalfExtentXY, WalkBoxHalfExtentXY, WalkBoxHalfExtentZ};

	const double OldCenter = InOutPos[Axis];
	const double NewCenter = OldCenter + Delta;

	// Voxel range the box overlaps across its full path along Axis this
	// step (covers both the start and end box, not just the destination).
	const double SweptLo = FMath::Min(OldCenter, NewCenter) - HalfExtent[Axis];
	const double SweptHi = FMath::Max(OldCenter, NewCenter) + HalfExtent[Axis];
	int64 VMin[3], VMax[3];
	AxisVoxelRange(SweptLo, SweptHi, VMin[Axis], VMax[Axis]);

	// The other two axes use the box's static extent at the (already
	// axis-resolved) position -- this is what makes the sweep
	// axis-separated: each axis is tested independently, in turn.
	const int32 Other1 = (Axis + 1) % 3;
	const int32 Other2 = (Axis + 2) % 3;
	AxisVoxelRange(InOutPos[Other1] - HalfExtent[Other1], InOutPos[Other1] + HalfExtent[Other1], VMin[Other1], VMax[Other1]);
	AxisVoxelRange(InOutPos[Other2] - HalfExtent[Other2], InOutPos[Other2] + HalfExtent[Other2], VMin[Other2], VMax[Other2]);

	// Walk the moving axis from the near side (closest to OldCenter) toward
	// the far side so the first hit found is the nearest blocking voxel.
	const int32 Step = Delta > 0 ? 1 : -1;
	const int64 AxisStart = Step > 0 ? VMin[Axis] : VMax[Axis];
	const int64 AxisEnd = Step > 0 ? VMax[Axis] : VMin[Axis];

	int64 V[3];
	bool bBlocked = false;
	int64 BlockingVoxel = 0;
	for (int64 AV = AxisStart; Step > 0 ? AV <= AxisEnd : AV >= AxisEnd; AV += Step)
	{
		V[Axis] = AV;
		bool bSliceSolid = false;
		for (int64 O1 = VMin[Other1]; O1 <= VMax[Other1] && !bSliceSolid; ++O1)
		{
			V[Other1] = O1;
			for (int64 O2 = VMin[Other2]; O2 <= VMax[Other2] && !bSliceSolid; ++O2)
			{
				V[Other2] = O2;
				if (Subsystem->IsSolidAtVoxel(V[0], V[1], V[2]))
				{
					bSliceSolid = true;
				}
			}
		}
		if (bSliceSolid)
		{
			bBlocked = true;
			BlockingVoxel = AV;
			break;
		}
	}

	if (!bBlocked)
	{
		InOutPos[Axis] = NewCenter;
		return false;
	}

	// Clamp the box's leading face to the blocking voxel's near face, with
	// CollisionEpsilonUU clearance so the box doesn't sit flush (and
	// potentially re-trigger a hit next step due to floating point noise).
	double ClampedCenter;
	if (Step > 0)
	{
		const double FaceMin = double(BlockingVoxel) * VoxelCoords::VoxelSizeUU;
		ClampedCenter = FMath::Min(NewCenter, FaceMin - HalfExtent[Axis] - CollisionEpsilonUU);
	}
	else
	{
		const double FaceMax = double(BlockingVoxel + 1) * VoxelCoords::VoxelSizeUU;
		ClampedCenter = FMath::Max(NewCenter, FaceMax + HalfExtent[Axis] + CollisionEpsilonUU);
	}
	InOutPos[Axis] = ClampedCenter;
	return true;
}

void AVoxelEarthFlyPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bWalkMode)
	{
		TickWalkMode(DeltaTime);
	}
	else
	{
		// Fly speed step + held modifiers (LeftShift boost / LeftAlt
		// precision) -> UFloatingPawnMovement. Done per-frame rather than only
		// on key events because the modifiers are HELD keys: releasing Shift
		// must drop the top speed immediately.
		ApplyFlySpeedToMovement();
	}

	// Runs in both modes (see constructor comment on PrimaryActorTick):
	// step-smoothing decay + the third-person boom, and the proxy body's
	// code-driven animation (task 5: fly mode keeps the TP camera/proxy).
	UpdateCameraSmoothing(DeltaTime);

	if (ProxyBody)
	{
		const double HorizSpeedUU = bWalkMode ? HorizontalVelocity.Size() : GetVelocity().Size();
		ProxyBody->UpdateAnimation(DeltaTime, HorizSpeedUU);
	}
}

FVector AVoxelEarthFlyPawn::GetHeadWorldLocation() const
{
	return GetActorLocation() + FVector(0.0, 0.0, HeadOffsetUU + CameraZOffsetUU);
}

void AVoxelEarthFlyPawn::UpdateCameraSmoothing(float DeltaTime)
{
	// Step-smoothing (decisions table, "Slope feel"): TickWalkMode's
	// step-up snap pushes the abrupt part of the actor's Z jump into
	// CameraZOffsetUU instead of letting the camera teleport with it; here
	// that offset exponentially decays back to 0 (rate CameraSmoothRatePerSec,
	// ~10/s) so steps read as a smooth ramp. Runs every frame (not just
	// while walking) so a step-up finishes smoothing out even across a
	// G-toggle to fly mode.
	const double Alpha = 1.0 - FMath::Exp(-CameraSmoothRatePerSec * (double)DeltaTime);
	CameraZOffsetUU = FMath::Lerp(CameraZOffsetUU, 0.0, Alpha);
	if (FMath::Abs(CameraZOffsetUU) < 0.01)
	{
		CameraZOffsetUU = 0.0;
	}

	if (Camera)
	{
		Camera->SetRelativeLocation(FVector(0.0, 0.0, HeadOffsetUU + CameraZOffsetUU));
	}

	UpdateThirdPersonCamera();
}

void AVoxelEarthFlyPawn::UpdateThirdPersonCamera()
{
	if (!ThirdPersonCamera)
	{
		return;
	}

	const FVector HeadPos = GetHeadWorldLocation();
	const FVector Forward = GetActorForwardVector();
	const FVector Right = GetActorRightVector();
	const FVector DesiredPos = HeadPos - Forward * ThirdPersonBoomBackUU + Right * ThirdPersonBoomRightUU;

	FVector FinalPos = DesiredPos;
	const FVector ToDesired = DesiredPos - HeadPos;
	const double DesiredDist = ToDesired.Size();
	if (DesiredDist > KINDA_SMALL_NUMBER)
	{
		const FVector Dir = ToDesired / DesiredDist;
		if (UVoxelWorldSubsystem* Subsystem = GetVoxelWorldSubsystem())
		{
			FVector HitCenter, PrevCenter;
			// Collision-aware pull-in (decisions table, "Cameras" row): no
			// USpringArmComponent probes -- terrain has no Chaos collision --
			// so the boom is pulled in with the same voxel DDA raycast
			// dig/place uses, cast from the head toward the desired camera
			// position.
			if (Subsystem->RaycastVoxelWorld(HeadPos, Dir, DesiredDist, HitCenter, PrevCenter))
			{
				// Project the last-empty-voxel center back onto the ray so
				// the pulled-in camera always stays exactly on the
				// head->desired line (PrevCenter itself is a voxel center,
				// not necessarily on that line).
				const double PrevDistAlongDir = (PrevCenter - HeadPos) | Dir;
				const double PulledDist = FMath::Max(0.0, PrevDistAlongDir - ThirdPersonPullInEpsilonUU);
				FinalPos = HeadPos + Dir * PulledDist;
			}
		}
	}

	ThirdPersonCamera->SetWorldLocation(FinalPos);
	ThirdPersonCamera->SetWorldRotation(GetActorRotation());
}

void AVoxelEarthFlyPawn::TickWalkMode(float DeltaTime)
{
	UVoxelWorldSubsystem* Subsystem = GetVoxelWorldSubsystem();
	if (!Subsystem)
	{
		bWaitingForTerrain = true;
		return; // world not streamed in yet; hold position rather than fall through nothing
	}

	FVector Pos = GetActorLocation();

	// Being GROUNDED on a real solid voxel is proof the terrain here has
	// arrived -- it trumps the streaming-readiness probe unconditionally. This
	// ordering matters: IsTerrainReadyAt also inspects the chunk one below the
	// pawn, and when you are standing on the floor of a resident chunk that
	// lower chunk is solid underground rock, which is legitimately never meshed
	// (so "tracked, no component"). Without this short-circuit that would read
	// as "not ready" and hold a pawn that is plainly standing on the ground.
	const bool bGroundedNow = IsGroundedAt(Pos);

	// Terrain not streamed under the pawn (see IsTerrainReadyAt): suspend
	// gravity and hold Z. Horizontal movement is deliberately still allowed --
	// freezing completely would strand you with no way to walk back to loaded
	// ground -- but with no voxels to sweep against it is unobstructed, which
	// is the honest behaviour: there is nothing there yet. The HUD shows
	// "WAITING FOR TERRAIN" so this reads as a state, not a bug.
	bWaitingForTerrain = !bGroundedNow && !IsTerrainReadyAt(Pos);
	if (bWaitingForTerrain)
	{
		VerticalVelocity = 0.f;
		bJumpRequested = false;
		bGroundedLastTick = false;
		bSwimmingLastTick = false;

		const FRotator HoldYawOnly(0.0, GetActorRotation().Yaw, 0.0);
		const FRotationMatrix HoldYawMatrix(HoldYawOnly);
		FVector HoldWish =
			HoldYawMatrix.GetUnitAxis(EAxis::X) * CurrentForwardInput + HoldYawMatrix.GetUnitAxis(EAxis::Y) * CurrentRightInput;
		if (HoldWish.SizeSquared() > 1.0)
		{
			HoldWish.Normalize();
		}
		HorizontalVelocity = HoldWish * (bSprintHeld ? SprintSpeedUU : WalkSpeedUU);
		SetActorLocation(Pos + HorizontalVelocity * DeltaTime);
		return;
	}

	// Water track W1 swimming placeholder (docs/voxel-earth-implementation-
	// plan.md SS3.7): the walk-mode box counts as "in the water" once it is
	// entirely below sea level (z=0, matching AVoxelOceanActor's implicit
	// ocean -- VoxelCoords.h: voxel z=0 == UE world z=0). This is a binary
	// swim/walk switch only -- no buoyancy, drag, or currents (those are
	// W4).
	const bool bSwimming = (Pos.Z + WalkBoxHalfExtentZ) < 0.0;
	const bool bGrounded = !bSwimming && bGroundedNow;
	bSwimmingLastTick = bSwimming;
	bGroundedLastTick = bGrounded;

	if (bSwimming)
	{
		// Fly-style while submerged: no gravity, no jump, no step-up --
		// just reduced-speed 3-axis movement, still swept against voxel
		// collision (below the DDA-swept ground, e.g. a lakebed).
		VerticalVelocity = 0.0;
		bJumpRequested = false;
	}
	else
	{
		if (bJumpRequested && bGrounded)
		{
			VerticalVelocity = (float)JumpSpeedUU;
		}
		bJumpRequested = false;

		// Client-presentation kinematics only (see class comment): plain float
		// gravity integration, not part of the deterministic world derivation
		// and not authoritative -- M3's server owns real player movement.
		VerticalVelocity -= GravityUUPerSec2 * DeltaTime;
	}

	// Horizontal move from the existing WASD axis inputs, projected onto the
	// yaw-only horizontal plane -- ignores the actor's pitch (mouse look
	// still pitches the camera; it must not tilt walking into the ground).
	const FRotator YawOnly(0.0, GetActorRotation().Yaw, 0.0);
	const FRotationMatrix YawMatrix(YawOnly);
	FVector WishDir = YawMatrix.GetUnitAxis(EAxis::X) * CurrentForwardInput + YawMatrix.GetUnitAxis(EAxis::Y) * CurrentRightInput;
	if (WishDir.SizeSquared() > 1.0)
	{
		WishDir.Normalize();
	}

	FVector HorizDelta;
	if (bSwimming)
	{
		// Fly-style while submerged (W1 swimming placeholder, unchanged):
		// instant velocity from input, no accel/friction model. Kept synced
		// into HorizontalVelocity so ground movement resumes smoothly from
		// this value on exit instead of snapping.
		HorizontalVelocity = WishDir * SwimSpeedUU;
		HorizDelta = HorizontalVelocity * DeltaTime;
	}
	else
	{
		// Player experience decisions table ("Movement feel"): accel-based
		// ground movement (~4000 UU/s^2) with friction-style damping when
		// there's no input (WishDir == 0 pulls HorizontalVelocity toward
		// zero the same way), and air control at 30% of ground accel while
		// airborne -- this is what makes direction changes feel weighted
		// instead of the old instant-velocity snap.
		const double MaxSpeed = bSprintHeld ? SprintSpeedUU : WalkSpeedUU;
		const double Accel = bGrounded ? GroundAccelUUPerSec2 : (GroundAccelUUPerSec2 * AirControlFactor);

		const FVector TargetVelocity = WishDir * MaxSpeed;
		const FVector VelDelta = TargetVelocity - HorizontalVelocity;
		const double MaxStep = Accel * DeltaTime;
		if (VelDelta.SizeSquared() <= FMath::Square(MaxStep))
		{
			HorizontalVelocity = TargetVelocity;
		}
		else
		{
			HorizontalVelocity += VelDelta.GetSafeNormal() * MaxStep;
		}
		HorizDelta = HorizontalVelocity * DeltaTime;
	}

	// Swimming: vertical motion comes directly from the Space/LeftControl
	// axis (fly-style), not integrated velocity -- there's no "falling"
	// underwater in this placeholder.
	const double VertDelta = bSwimming ? (CurrentUpInput * SwimSpeedUU * DeltaTime) : (VerticalVelocity * DeltaTime);

	FVector NewPos = Pos;
	const bool bBlockedX = SweepAxis(0, HorizDelta.X, NewPos);
	const bool bBlockedY = SweepAxis(1, HorizDelta.Y, NewPos);

	bool bDidStepSnap = false;
	if (!bSwimming && (bBlockedX || bBlockedY) && bGrounded)
	{
		// Step-up: retry the same horizontal move 30 UU (3 voxels) higher;
		// if that clears, snap back down onto the ground with a downward
		// sweep. Handles ledges/stairs up to 3 voxels without a full
		// physics-style step solver.
		FVector StepPos = Pos + FVector(0.0, 0.0, StepUpHeightUU);
		const bool bStepBlockedX = SweepAxis(0, HorizDelta.X, StepPos);
		const bool bStepBlockedY = SweepAxis(1, HorizDelta.Y, StepPos);
		if (!bStepBlockedX && !bStepBlockedY)
		{
			SweepAxis(2, -(StepUpHeightUU + CollisionEpsilonUU * 2.0), StepPos);
			NewPos = StepPos;
			bDidStepSnap = true;
			VerticalVelocity = 0.0;
		}
		// Else: the raised retry also hit something (a wall, not a step) --
		// keep the normal (blocked/clamped) horizontal result in NewPos.
	}

	if (!bDidStepSnap)
	{
		if (SweepAxis(2, VertDelta, NewPos))
		{
			VerticalVelocity = 0.0;
		}
	}
	else
	{
		// Step smoothing (decisions table, "Slope feel"): the actor's Z just
		// jumped abruptly (up to StepUpHeightUU = 30 UU, well within the
		// "<=35 UU" abrupt-change band); absorb that jump into the camera
		// offset instead of letting the camera teleport with it --
		// UpdateCameraSmoothing decays this back to 0 every frame.
		const double AbruptZJump = NewPos.Z - Pos.Z;
		CameraZOffsetUU -= AbruptZJump;
	}

	// --- Backstop: never fall through the surface into unstreamed space -----
	//
	// IsTerrainReadyAt (above) catches "the chunk I am in is queued", but it
	// cannot catch this: the streaming set does not ADMIT underground chunks
	// until the anchor itself goes underground (FVoxelWorldImpl's deep-set
	// policy). So a pawn that starts falling from a few metres up can outrun
	// the surface chunk, cross into space that was never generated at all --
	// where IsSolidAtVoxel honestly answers "air" for every voxel -- and keep
	// accelerating downwards forever. Measured directly: entering walk mode 1 s
	// after launch put the pawn 14.9 m BELOW the surface and still airborne.
	//
	// The fix uses the one height that needs no streaming at all:
	// GetSurfaceHeightUU is the analytic amplifier surface, available
	// everywhere, immediately. If the box has sunk below it and the chunk it
	// is in has no component, the pawn is falling through terrain that does
	// not exist yet -- so park it on the analytic surface and report
	// "waiting for terrain" until the real voxels arrive.
	//
	// This deliberately does NOT fire when the chunk IS resident, which is
	// what makes caves, dug holes and cave mouths work normally: those are all
	// below the analytic surface, but their chunks are loaded, so the real
	// voxel sweep stays in charge and this never intervenes.
	if (!bSwimming)
	{
		const double SurfaceUU = Subsystem->GetSurfaceHeightUU(NewPos.X, NewPos.Y);
		if ((NewPos.Z - WalkBoxHalfExtentZ) < (SurfaceUU - SurfaceBackstopToleranceUU))
		{
			bool bTracked = false;
			bool bHasComponent = false;
			int32 Quads = 0;
			const bool bResident = Subsystem->DebugChunkStatusAt(NewPos, bTracked, bHasComponent, Quads) && bHasComponent;
			if (!bResident)
			{
				NewPos.Z = SurfaceUU + WalkBoxHalfExtentZ;
				VerticalVelocity = 0.f;
				bWaitingForTerrain = true;
				bGroundedLastTick = false;
			}
		}
	}

	SetActorLocation(NewPos);
}
