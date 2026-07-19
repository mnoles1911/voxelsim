#include "VoxelEarthGameMode.h"

#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "VoxelEarthFlyPawn.h"
#include "VoxelEarthPlayerController.h"
#include "VoxelWorldSubsystem.h"

AVoxelEarthGameMode::AVoxelEarthGameMode()
{
	DefaultPawnClass = AVoxelEarthFlyPawn::StaticClass();
	PlayerControllerClass = AVoxelEarthPlayerController::StaticClass();
}

void AVoxelEarthGameMode::RestartPlayer(AController* NewPlayer)
{
	// docs/m1-plan.md Stage 2 decisions table item 3: spawn above the
	// terrain surface (Amplifier column at 0,0), +5m -- rather than via
	// FindPlayerStart/APlayerStart, since no level in this repo places one
	// yet. Falls back to the default PlayerStart-based flow if the voxel
	// world subsystem isn't available for some reason.
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!NewPlayer || !Subsystem)
	{
		Super::RestartPlayer(NewPlayer);
		return;
	}

	constexpr double SpawnHeightAboveSurfaceUU = 500.0; // +5m (1 UU = 1 cm)
	const double SurfaceUU = Subsystem->GetSurfaceHeightUU(0.0, 0.0);
	const FVector SpawnLocation(0.0, 0.0, SurfaceUU + SpawnHeightAboveSurfaceUU);
	const FTransform SpawnTransform(FRotator::ZeroRotator, SpawnLocation);

	RestartPlayerAtTransform(NewPlayer, SpawnTransform);
}
