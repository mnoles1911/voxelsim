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

	// Blast tuning. Radius dropped 3.0 m -> 2.0 m and jitter raised 0.3 -> 0.5 m
	// on 2026-07-29 (Matt, play-test): a 2 m crater with visibly ragged edges,
	// rather than a larger and rounder one.
	//
	// Jitter is a DETERMINISTIC per-voxel hash inside CarveSphere, not a random
	// draw -- the carve goes through the edit-log authority path and every
	// client must derive the identical crater from the same entry. "Randomness"
	// here means the edge is hash-ragged, not that two charges at the same spot
	// differ.
	//
	// PerCharge radius variation IS a real random draw, but it is applied once
	// at spawn and travels in the edit as a concrete radius, so it is decided in
	// one place rather than re-rolled per client.
	static constexpr float FuseSeconds = 3.0f;
	static constexpr double BlastRadiusUU = 200.0;      // 2.0 m
	static constexpr double BlastJitterUU = 50.0;       // +-0.5 m ragged edge
	static constexpr double BlastRadiusVarianceUU = 25.0; // +-0.25 m per charge

private:
	void Detonate();

	// Per-tick voxel-space collision. UProjectileMovementComponent resolves
	// against Chaos, and voxel terrain has NO Chaos collision at all by design
	// (plan SS3.3) -- so a thrown charge fell straight through the world and
	// detonated three seconds later somewhere underground, carving a crater
	// nobody could see. Reported from play-test as "charges do nothing".
	//
	// Same fix the over-the-shoulder camera already uses for the same reason:
	// step the DDA raycast (UVoxelWorldSubsystem::RaycastVoxelWorld) along the
	// segment actually travelled this frame. Stepping the SEGMENT rather than
	// testing the endpoint is what stops a fast charge tunnelling through a
	// thin roof between two frames.
	virtual void Tick(float DeltaSeconds) override;

	// Where the charge was last frame, for the segment test above.
	FVector LastTickLocationUU = FVector::ZeroVector;

	// Set once the charge has come to rest on terrain: the projectile is
	// deactivated and the fuse simply runs out where it landed.
	bool bLanded = false;

	// Rolled at spawn (see BlastRadiusVarianceUU).
	double ThisChargeRadiusUU = BlastRadiusUU;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Explosive")
	TObjectPtr<UStaticMeshComponent> BlastMesh;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Explosive")
	TObjectPtr<UProjectileMovementComponent> Projectile;

	FTimerHandle FuseTimerHandle;
};
