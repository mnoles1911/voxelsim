#pragma once

#include "Camera/CameraShakeBase.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelExplosive.generated.h"

class UStaticMeshComponent;
class UProjectileMovementComponent;

// Brief camera kick on detonation (m1-plan.md "Explosives v1" row: "add a
// brief camera shake"). A perlin-noise rotation/FOV shake (EngineCameras
// plugin, enabled by default) configured entirely in C++ -- no Blueprint
// asset needed.
UCLASS()
class VOXELEARTH_API UVoxelBlastCameraShake : public UCameraShakeBase
{
	GENERATED_BODY()

public:
	UVoxelBlastCameraShake(const FObjectInitializer& ObjectInitializer);
};

// m1-plan.md "Player experience decisions" (Matt sign-off), "Explosives v1"
// row: thrown from AVoxelEarthPlayerController (hold 'F' to charge, release
// to throw with a 30-degree upward arc). Flight is cosmetic only
// (UProjectileMovementComponent: gravity + bounce) -- there is no Chaos
// collision against voxel terrain (plan SS3.3: "no Chaos for terrain"), so
// the projectile mostly just arcs through the air. The CARVE is authoritative
// and fires on a fixed 3s fuse from the moment Launch() is called, independent
// of where the mesh visually ends up (UVoxelWorldSubsystem::CarveSphere, the
// edit-log authority path).
UCLASS()
class VOXELEARTH_API AVoxelExplosive : public AActor
{
	GENERATED_BODY()

public:
	AVoxelExplosive();

	virtual void BeginPlay() override;

	// Called by the throwing controller immediately after SpawnActor: sets
	// the projectile's initial velocity (already includes the upward-arc
	// pitch and charge-scaled speed -- see AVoxelEarthPlayerController::
	// OnChargeRelease) and starts the fuse timer. An explicit setup call
	// (rather than doing everything in BeginPlay) so the thrower controls
	// the exact launch velocity.
	void Launch(const FVector& InitialVelocityUUPerSec);

	// Blast tuning (m1-plan.md "Explosives v1" row): 3.0s fuse; carves every
	// voxel within ~3.0m (+-0.3m deterministic per-voxel jitter) of the
	// detonation point.
	static constexpr float FuseSeconds = 3.0f;
	static constexpr double BlastRadiusUU = 300.0; // 3.0 m
	static constexpr double BlastJitterUU = 30.0;  // +-0.3 m

private:
	void Detonate();

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Explosive")
	TObjectPtr<UStaticMeshComponent> BlastMesh;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Explosive")
	TObjectPtr<UProjectileMovementComponent> Projectile;

	FTimerHandle FuseTimerHandle;
};
