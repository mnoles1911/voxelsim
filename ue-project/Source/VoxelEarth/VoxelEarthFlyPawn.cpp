#include "VoxelEarthFlyPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/FloatingPawnMovement.h"
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

	// Legacy raw-key bindings (docs/m1-plan.md Stage 2 decisions table:
	// "legacy input bindings, no Enhanced Input assets"). BindAxisKey needs
	// no named AxisMapping config -- each key drives its handler directly
	// with the key's raw value (1.0 while a digital key like W is held).
	PlayerInputComponent->BindAxisKey(EKeys::W, this, &AVoxelEarthFlyPawn::MoveForward);
	PlayerInputComponent->BindAxisKey(EKeys::S, this, &AVoxelEarthFlyPawn::MoveBackward);
	PlayerInputComponent->BindAxisKey(EKeys::D, this, &AVoxelEarthFlyPawn::MoveRight);
	PlayerInputComponent->BindAxisKey(EKeys::A, this, &AVoxelEarthFlyPawn::MoveLeft);
	PlayerInputComponent->BindAxisKey(EKeys::SpaceBar, this, &AVoxelEarthFlyPawn::MoveUp);
	PlayerInputComponent->BindAxisKey(EKeys::LeftControl, this, &AVoxelEarthFlyPawn::MoveDown);

	PlayerInputComponent->BindAxisKey(EKeys::MouseX, this, &AVoxelEarthFlyPawn::Turn);
	PlayerInputComponent->BindAxisKey(EKeys::MouseY, this, &AVoxelEarthFlyPawn::LookUp);
}

void AVoxelEarthFlyPawn::MoveForward(float Value) { AddMovementInput(GetActorForwardVector(), Value); }
void AVoxelEarthFlyPawn::MoveBackward(float Value) { AddMovementInput(GetActorForwardVector(), -Value); }
void AVoxelEarthFlyPawn::MoveRight(float Value) { AddMovementInput(GetActorRightVector(), Value); }
void AVoxelEarthFlyPawn::MoveLeft(float Value) { AddMovementInput(GetActorRightVector(), -Value); }
void AVoxelEarthFlyPawn::MoveUp(float Value) { AddMovementInput(FVector::UpVector, Value); }
void AVoxelEarthFlyPawn::MoveDown(float Value) { AddMovementInput(FVector::UpVector, -Value); }

void AVoxelEarthFlyPawn::Turn(float Value) { AddControllerYawInput(Value * MouseLookSpeed); }
void AVoxelEarthFlyPawn::LookUp(float Value) { AddControllerPitchInput(-Value * MouseLookSpeed); }
