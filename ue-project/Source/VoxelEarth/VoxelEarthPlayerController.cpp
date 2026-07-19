#include "VoxelEarthPlayerController.h"

#include "Components/InputComponent.h"
#include "Engine/World.h"
#include "InputCoreTypes.h"
#include "VoxelWorldSubsystem.h"

void AVoxelEarthPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	if (!InputComponent)
	{
		return;
	}

	// Legacy raw-key bindings (docs/m1-plan.md Stage 2 decisions table:
	// "legacy input bindings, no Enhanced Input assets").
	InputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &AVoxelEarthPlayerController::OnDig);
	InputComponent->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &AVoxelEarthPlayerController::OnPlace);
}

void AVoxelEarthPlayerController::OnDig()
{
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Subsystem)
	{
		return;
	}

	FVector CameraLocation;
	FRotator CameraRotation;
	GetPlayerViewPoint(CameraLocation, CameraRotation);

	Subsystem->TryDig(CameraLocation, CameraRotation.Vector());
}

void AVoxelEarthPlayerController::OnPlace()
{
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Subsystem)
	{
		return;
	}

	FVector CameraLocation;
	FRotator CameraRotation;
	GetPlayerViewPoint(CameraLocation, CameraRotation);

	Subsystem->TryPlace(CameraLocation, CameraRotation.Vector());
}
