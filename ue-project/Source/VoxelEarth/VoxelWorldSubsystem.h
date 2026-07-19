#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelWorldSubsystem.generated.h"

// voxel-core owns the deterministic world + edit overlay (doctrine SS2.1 /
// SS2.4: vxc::World<8> + its sampler). Kept behind a PImpl so this header
// never includes a voxel-core header -- UHT-parsed headers stay
// voxel-core-free by doctrine; see VoxelWorldSubsystem.cpp for the bridge.
// Stage 2: FVoxelWorldImpl also owns ALL streaming bookkeeping (chunk
// records, pending-work queues, the worker-result MPSC queue, in-flight task
// handles) for the same reason -- none of it needs to leak into this header.
struct FVoxelWorldImpl;

// Owns the voxel world (docs/m1-plan.md decisions table: "All voxel-core
// access via this subsystem") and streams render chunks around a moving
// anchor (docs/m1-plan.md Stage 2 decisions table): background UE::Tasks
// jobs generate+mesh chunks from the deterministic GeneratedWorld only
// (lock-free), while chunks touched by edits are meshed on the game thread
// via the overlay-aware World::materialAt. Also hosts dig/place (edit-log
// authority path) queried by AVoxelEarthPlayerController.
UCLASS()
class VOXELEARTH_API UVoxelWorldSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UVoxelWorldSubsystem();
	// Declared (not defaulted) here and defined in the .cpp: TUniquePtr<FVoxelWorldImpl>'s
	// destructor needs FVoxelWorldImpl's full definition, which this
	// UHT-parsed header must not see (voxel-core stays out of it).
	virtual ~UVoxelWorldSubsystem() override;
	// UHT auto-generates this hot-reload constructor unless one is already
	// declared; the auto-generated version lives in VoxelWorldSubsystem.gen.cpp,
	// which cannot see FVoxelWorldImpl's definition either (same PImpl reason
	// as the destructor above).
	UVoxelWorldSubsystem(FVTableHelper& Helper);

	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	//~ End UWorldSubsystem Interface

	//~ Begin FTickableGameObject / UTickableWorldSubsystem Interface
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject / UTickableWorldSubsystem Interface

	// Dev-fixed seed (docs/m1-plan.md decisions table: "seed from config
	// (default 20260719)" -- still fixed; config-driven seed selection is a
	// later milestone).
	static constexpr uint64 DefaultSeed = 20260719;

	// Stage 2 decisions table: streaming hysteresis ring radii (meters).
	// Render chunks enter the desired set inside LoadRadiusMeters, leave it
	// only once they cross UnloadRadiusMeters (the gap prevents load/unload
	// flicker for an anchor sitting near a single threshold).
	static constexpr double LoadRadiusMeters = 64.0;
	static constexpr double UnloadRadiusMeters = 80.0;

	// Stage 2 decisions table: dig/place raycast range and dig sphere radius.
	static constexpr double DigPlaceRangeMeters = 8.0;
	static constexpr int32 DigSphereRadiusVoxels = 3;

	// Digs a sphere (radius DigSphereRadiusVoxels, MAT_AIR) centred on the
	// first solid voxel hit by a ray from CameraWorldLocation along
	// CameraWorldDirection (need not be normalized), out to
	// DigPlaceRangeMeters. Submits one World::applyEdit per touched brick
	// (the edit-log authority path) and re-meshes every dirty render chunk
	// (including chunk-border neighbors) budgeted on subsequent ticks. Game
	// thread only. Returns true if anything was edited.
	bool TryDig(const FVector& CameraWorldLocation, const FVector& CameraWorldDirection);

	// Places a single MAT_ROCK voxel on the face of the first solid voxel hit
	// by the same ray (no-op if nothing is hit within range, or the ray
	// starts inside solid geometry -- no valid face to place against). Game
	// thread only. Returns true if a voxel was placed.
	bool TryPlace(const FVector& CameraWorldLocation, const FVector& CameraWorldDirection);

	// Amplifier column surface elevation, in UE units (cm), at the given
	// world XY. A pure query (no streaming state touched) -- safe to call as
	// soon as Initialize has run, e.g. for GameMode spawn placement before
	// any chunk has streamed in.
	double GetSurfaceHeightUU(double WorldX, double WorldY) const;

	// True if the voxel at the given integer voxel-lattice coordinate is
	// solid (overlay-aware World::materialAt != MAT_AIR -- edits are
	// reflected immediately). Game thread only (same constraint as
	// TryDig/TryPlace: Voxels' overlay is not thread-safe). Stage 3b (plan
	// SS3.3, "no Chaos for terrain"): the walk-mode custom kinematic
	// collision in AVoxelEarthFlyPawn queries this per-voxel instead of using
	// a physics engine.
	bool IsSolidAtVoxel(int64 Vx, int64 Vy, int64 Vz) const;

private:
	TUniquePtr<FVoxelWorldImpl> Impl;

	// Single actor hosting every render-chunk component (unchanged from
	// stage 1: one UVoxelChunkComponent per non-empty render chunk).
	UPROPERTY(Transient)
	TObjectPtr<AActor> ChunkOwner;

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> ChunkRoot;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> ChunkMaterial;
};
