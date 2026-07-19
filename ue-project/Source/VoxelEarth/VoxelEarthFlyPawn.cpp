#include "VoxelEarthFlyPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"

AVoxelEarthFlyPawn::AVoxelEarthFlyPawn()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SceneRoot);

	Movement = CreateDefaultSubobject<UFloatingPawnMovement>(TEXT("Movement"));
	// Flying speed tuned for exploring a streaming radius measured in tens
	// of meters (docs/m1-plan.md Stage 2: 64m load / 80m unload rings).
	Movement->MaxSpeed = 3000.f;   // 30 m/s
	Movement->Acceleration = 8000.f;
	Movement->Deceleration = 8000.f;

	// Actor rotation follows the controller (mouse look), and movement input
	// is applied in actor-local axes -- the standard "fly cam" setup.
	bUseControllerRotationYaw = true;
	bUseControllerRotationPitch = true;
	bUseControllerRotationRoll = false;
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
}

void AVoxelEarthFlyPawn::MoveForward(float Value) { AddMovementInput(GetActorForwardVector(), Value); }
void AVoxelEarthFlyPawn::MoveRight(float Value) { AddMovementInput(GetActorRightVector(), Value); }
void AVoxelEarthFlyPawn::MoveUp(float Value) { AddMovementInput(FVector::UpVector, Value); }

void AVoxelEarthFlyPawn::Turn(float Value) { AddControllerYawInput(Value * MouseLookSpeed); }
void AVoxelEarthFlyPawn::LookUp(float Value) { AddControllerPitchInput(-Value * MouseLookSpeed); }
