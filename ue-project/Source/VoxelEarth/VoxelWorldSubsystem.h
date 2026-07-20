#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelCoords.h" // VoxelCoords::kNumLevels (ring preset table sizing) -- UE-only, voxel-core-free
#include "VoxelDebug.h" // FVoxelPerfSnapshot -- plain POD, voxel-core-free (see VoxelDebug.h doctrine note)
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

	// Fallback seed (docs/m1-plan.md decisions table: "seed from config
	// (default 20260719)"). M2 task "Config-driven seed": the RUNTIME seed
	// actually in effect is resolved once in Initialize() from -VoxelSeed=<u64>
	// on the command line (falling back to this constant) and stored in Seed
	// below -- GetSeed() is the value every voxel-core access, and every other
	// actor that needs seed-matched sampling (e.g. AVoxelClipmapActor's
	// heightmap), should read. DefaultSeed itself stays a compile-time
	// constant only for the fallback value and any log/comment referring to
	// "the default".
	static constexpr uint64 DefaultSeed = 20260719;

	// The seed actually in effect this run (-VoxelSeed=<u64> override, else
	// DefaultSeed) -- resolved once in Initialize(), before Impl is
	// constructed, so it is valid for the subsystem's entire lifetime.
	uint64 GetSeed() const { return Seed; }

	// M2 first implementation wave (docs/m2-plan.md decisions table, "Ring
	// structure" / "Ring streaming" rows): default ring preset, one annulus
	// [InnerMeters, OuterMeters) per mip level -- R0 = true voxels (level 0,
	// same radii as the original single-ring M1 behavior), R1-R4 = mip
	// levels 1-4. A chunk leaves its level's desired set once it crosses
	// OuterMeters*UnloadRingMultiplier (hysteresis on the outer edge only --
	// see FVoxelWorldImpl::RecomputeDesiredSet doc comment for why the inner
	// edge has none in this wave).
	struct FRingPreset
	{
		double InnerMeters = 0.0;
		double OuterMeters = 0.0;
	};
	static constexpr FRingPreset RingPresets[VoxelCoords::kNumLevels] = {
		{0.0, 64.0},
		{64.0, 128.0},
		{128.0, 256.0},
		{256.0, 512.0},
		{512.0, 1024.0},
	};
	static constexpr double UnloadRingMultiplier = 1.25;

	// Back-compat aliases (R0's radii; a handful of log lines still reference
	// these by name -- unchanged numeric values from the pre-M2 single ring).
	static constexpr double LoadRadiusMeters = RingPresets[0].OuterMeters;
	static constexpr double UnloadRadiusMeters = RingPresets[0].OuterMeters * UnloadRingMultiplier;

	// Stage 2 decisions table: dig/place raycast range.
	static constexpr double DigPlaceRangeMeters = 8.0;

	// m1-plan.md "Player experience decisions" (Matt sign-off): dig/place
	// cube edge lengths, in voxels, selectable via AVoxelEarthPlayerController
	// (scroll wheel / number keys).
	static constexpr int32 MinCubeSizeVoxels = 1;
	static constexpr int32 MaxCubeSizeVoxels = 4;

	// Digs a grid-aligned SizeVoxels^3 cube (MAT_AIR) anchored on the first
	// solid voxel hit by a ray from CameraWorldLocation along
	// CameraWorldDirection (need not be normalized), out to
	// DigPlaceRangeMeters. The cube is centred on the hit voxel on the two
	// axes tangent to the hit face and biased ~SizeVoxels/2 along the
	// negative hit-face normal (into the terrain) on the face axis, so it
	// bites into solid material rather than mostly digging air (m1-plan.md
	// "Dig sizes" row -- replaces the old r=3 sphere dig). Submits one
	// World::applyEdit per touched brick (the edit-log authority path) and
	// re-meshes every dirty render chunk (including chunk-border neighbors)
	// budgeted on subsequent ticks. Game thread only. Returns true if
	// anything was edited.
	bool TryDig(const FVector& CameraWorldLocation, const FVector& CameraWorldDirection, int32 SizeVoxels);

	// Places a grid-aligned SizeVoxels^3 cube of MaterialId, grid-snapped
	// against the face of the first solid voxel hit by the same ray (biased
	// away from the surface, mirroring TryDig's bias) -- no-op if nothing is
	// hit within range, or the ray starts inside solid geometry (no valid
	// face to place against). Rejected (logged, no edit) if the placement
	// cube would overlap the player's collision box at PlayerActorLocation
	// (m1-plan.md "Place" row). Game thread only. Returns true if the cube
	// was placed.
	bool TryPlace(const FVector& CameraWorldLocation, const FVector& CameraWorldDirection, int32 SizeVoxels,
	              uint8 MaterialId, const FVector& PlayerActorLocation);

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

	// Deterministic voxel DDA raycast (voxelcore/raycast.h) from StartUU along
	// DirUU (need not be normalized -- normalized internally), out to
	// MaxDistUU. Used by AVoxelEarthFlyPawn for the over-the-shoulder camera's
	// collision-aware pull-in (Player experience decisions table, "Cameras"
	// row): terrain has no Chaos collision, so USpringArmComponent's probe
	// can't be used -- this gives the same DDA the dig/place raycast uses
	// instead. On hit, OutHitVoxelCenterUU is the first solid voxel's center
	// and OutPrevVoxelCenterUU is the center of the last empty voxel before
	// it (project the segment head->OutPrevVoxelCenterUU to know how far the
	// camera can safely sit along the ray). Returns false (outputs
	// untouched) if nothing solid is hit within MaxDistUU. Game thread only
	// (same constraint as IsSolidAtVoxel).
	bool RaycastVoxelWorld(const FVector& StartUU, const FVector& DirUU, double MaxDistUU, FVector& OutHitVoxelCenterUU, FVector& OutPrevVoxelCenterUU) const;

	// --- Explosives v1 (m1-plan.md "Explosives v1" row) ---------------------

	// Carves (MAT_AIR) every voxel whose center lies within RadiusUU +
	// per-voxel jitter of CenterUU (both UU), where the jitter is a
	// deterministic per-voxel hash (vxc::hash3, world seed, channel 40)
	// scaled into [-JitterUU, +JitterUU] -- a ragged, reproducible blast edge
	// rather than a perfect sphere. Same edit-log authority path as
	// TryDig/TryPlace (one World::applyEdit per touched brick; dirties every
	// overlapping render chunk incl. neighbors, budgeted re-mesh on
	// subsequent ticks). Called by AVoxelExplosive on fuse detonation. Game
	// thread only. Returns the number of voxels actually removed (were
	// non-air before the carve).
	int32 CarveSphere(const FVector& CenterUU, double RadiusUU, double JitterUU);

	// docs/debug-tooling-plan.md P1 "Perf HUD": a snapshot refreshed at 1Hz
	// (per-frame collection, see FVoxelWorldImpl::UpdatePerfSnapshot), read by
	// AVoxelEarthHUD every frame when voxel.Debug >= 1. Cheap struct copy;
	// safe to call from the game thread at any time after Initialize.
	FVoxelPerfSnapshot GetPerfSnapshot() const;

private:
	// M2 task "Config-driven seed": resolved in Initialize() from -VoxelSeed=
	// (default DefaultSeed); see GetSeed() above.
	uint64 Seed = DefaultSeed;

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
