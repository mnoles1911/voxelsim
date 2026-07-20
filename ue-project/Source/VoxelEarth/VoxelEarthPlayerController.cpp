#include "VoxelEarthPlayerController.h"

#include "Components/InputComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "InputCoreTypes.h"
#include "VoxelExplosive.h"
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

	// Dig/place size selection (m1-plan.md "Dig sizes" row).
	InputComponent->BindKey(EKeys::MouseScrollUp, IE_Pressed, this, &AVoxelEarthPlayerController::CycleDigSizeUp);
	InputComponent->BindKey(EKeys::MouseScrollDown, IE_Pressed, this, &AVoxelEarthPlayerController::CycleDigSizeDown);
	InputComponent->BindKey(EKeys::One, IE_Pressed, this, &AVoxelEarthPlayerController::SelectDigSize1);
	InputComponent->BindKey(EKeys::Two, IE_Pressed, this, &AVoxelEarthPlayerController::SelectDigSize2);
	InputComponent->BindKey(EKeys::Three, IE_Pressed, this, &AVoxelEarthPlayerController::SelectDigSize4);

	// Creative placement palette cycle (m1-plan.md "Place" row).
	InputComponent->BindKey(EKeys::T, IE_Pressed, this, &AVoxelEarthPlayerController::CyclePaletteMaterial);

	// Explosive charge/throw (m1-plan.md "Explosives v1" row).
	InputComponent->BindKey(EKeys::F, IE_Pressed, this, &AVoxelEarthPlayerController::OnChargeStart);
	InputComponent->BindKey(EKeys::F, IE_Released, this, &AVoxelEarthPlayerController::OnChargeRelease);
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

	Subsystem->TryDig(CameraLocation, CameraRotation.Vector(), DigSizeVoxels);
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

	const APawn* PlayerPawn = GetPawn();
	const FVector PlayerLocation = PlayerPawn ? PlayerPawn->GetActorLocation() : CameraLocation;

	Subsystem->TryPlace(CameraLocation, CameraRotation.Vector(), DigSizeVoxels, PaletteMaterialId, PlayerLocation);
}

void AVoxelEarthPlayerController::CycleDigSizeUp()
{
	// 1 -> 2 -> 4 -> 1 (m1-plan.md "Dig sizes" row).
	DigSizeVoxels = (DigSizeVoxels >= 4) ? 1 : DigSizeVoxels * 2;
}

void AVoxelEarthPlayerController::CycleDigSizeDown()
{
	DigSizeVoxels = (DigSizeVoxels <= 1) ? 4 : DigSizeVoxels / 2;
}

void AVoxelEarthPlayerController::SelectDigSize1() { DigSizeVoxels = 1; }
void AVoxelEarthPlayerController::SelectDigSize2() { DigSizeVoxels = 2; }
void AVoxelEarthPlayerController::SelectDigSize4() { DigSizeVoxels = 4; }

void AVoxelEarthPlayerController::CyclePaletteMaterial()
{
	// rock(2) -> soil/topsoil(6) -> sand(4) -> rock ... (m1-plan.md "Place"
	// row: "cycle material (rock/soil/sand)"). Numeric vxc::MaterialId
	// values (voxelcore/core.h) -- kept literal since this header/translation
	// unit intentionally avoids depending on the voxel-core enum type here.
	switch (PaletteMaterialId)
	{
	case 2: // MAT_ROCK -> MAT_TOPSOIL
		PaletteMaterialId = 6;
		break;
	case 6: // MAT_TOPSOIL -> MAT_SAND
		PaletteMaterialId = 4;
		break;
	default: // MAT_SAND (or anything unexpected) -> MAT_ROCK
		PaletteMaterialId = 2;
		break;
	}
}

void AVoxelEarthPlayerController::OnChargeStart()
{
	if (bChargingExplosive)
	{
		return;
	}
	bChargingExplosive = true;
	ChargeStartTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
}

void AVoxelEarthPlayerController::OnChargeRelease()
{
	if (!bChargingExplosive)
	{
		return;
	}
	bChargingExplosive = false;

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 0.3s->1.5s hold maps to 600->1600 UU/s throw speed; a shorter tap still
	// throws at the minimum speed rather than being a no-op.
	const float HeldSeconds = World->GetTimeSeconds() - ChargeStartTimeSeconds;
	const float Alpha = FMath::Clamp((HeldSeconds - MinChargeSeconds) / (MaxChargeSeconds - MinChargeSeconds), 0.f, 1.f);
	const float ThrowSpeedUU = FMath::Lerp(MinThrowSpeedUU, MaxThrowSpeedUU, Alpha);

	FVector CameraLocation;
	FRotator CameraRotation;
	GetPlayerViewPoint(CameraLocation, CameraRotation);

	// 30-degree upward arc (m1-plan.md "Explosives v1" row).
	FRotator ThrowRotation = CameraRotation;
	ThrowRotation.Pitch = FMath::Clamp(CameraRotation.Pitch + ThrowUpwardArcDegrees, -89.f, 89.f);
	const FVector ThrowDirection = ThrowRotation.Vector();

	// Spawn a short distance in front of the camera so the explosive doesn't
	// immediately collide with the throwing pawn.
	const FVector ExplosiveSpawnLocation = CameraLocation + ThrowDirection * 80.0;

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = this;
	SpawnParams.Instigator = GetPawn();
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	if (AVoxelExplosive* Explosive = World->SpawnActor<AVoxelExplosive>(AVoxelExplosive::StaticClass(), ExplosiveSpawnLocation, ThrowRotation, SpawnParams))
	{
		Explosive->Launch(ThrowDirection * ThrowSpeedUU);
	}
}

float AVoxelEarthPlayerController::GetExplosiveChargeAlpha() const
{
	if (!bChargingExplosive)
	{
		return 0.f;
	}
	const UWorld* World = GetWorld();
	if (!World)
	{
		return 0.f;
	}
	const float HeldSeconds = World->GetTimeSeconds() - ChargeStartTimeSeconds;
	return FMath::Clamp(HeldSeconds / MaxChargeSeconds, 0.f, 1.f);
}
