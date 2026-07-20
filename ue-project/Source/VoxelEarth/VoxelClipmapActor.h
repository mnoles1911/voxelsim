#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelClipmapActor.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;

// M2 Band 3 first slice (docs/m2-plan.md Band 3 row; doctrine
// docs/voxel-earth-implementation-plan.md SS3.3 "Band 3 -- heightmap
// clipmap"): a CPU-built concentric clipmap that extends terrain from the
// voxel ring cascade's outer edge (UVoxelWorldSubsystem::RingPresets'
// R4 outer, ~1km) out to ~30km, so a summit-level or airborne view sees
// terrain to the horizon instead of the ring cascade's hard edge.
//
// PRAGMATIC EXCEPTION to the "no ProceduralMeshComponent" doctrine (plan
// SS3.3 Band 1: custom FPrimitiveSceneProxy, NOT PMC -- see
// VoxelChunkComponent.h): that rule targets the VOXEL rendering path
// (per-quad material id/orientation/AO, GPU greedy meshing). Band 3 is a
// conventional CDLOD-style heightmap clipmap, not voxel terrain -- there is
// no voxel data here, just 4 flat vertex/index buffers rebuilt on the CPU as
// the camera moves. UProceduralMeshComponent is the doctrine-clean choice
// for that (same reasoning AVoxelOceanActor's header gives for choosing a
// plain UStaticMeshComponent over a PMC for its cosmetic plane -- here the
// content genuinely IS procedural mesh data, so PMC is the right tool, not
// a doctrine violation). ADR-worthy per the M2 task spec; the plan's CDLOD
// polish pass (dithered cross-fades, screen-space error budgets) can replace
// this v1 with a proper clipmap renderer later without touching callers.
//
// Geometry (see VoxelClipmapActor.cpp SpacingUUForLevel/RebuildLevel for the
// derivation): 4 levels, each a fixed 65x65-vertex grid (64x64 quads). Every
// level uses the SAME local topology (vertex/index layout never changes --
// only world placement and sampled heights do), doubling vertex spacing
// per level starting from a spacing derived from the ring cascade's own
// outer radius, so level 0's inner hole lands exactly on the ring edge and
// each level's inner hole exactly matches the previous level's outer edge
// (classic clipmap "hole = quarter area" ratio, held constant across all 4
// levels by construction). Total coverage: ring edge (~1km) to ~16.4km
// radius (~32.8km diameter) -- see the .cpp for why this differs from the
// task spec's illustrative 16m->128m/vertex numbers (they don't reconcile
// with a fixed 65-vertex grid; this is the corrected, self-consistent
// version of the same doubling-annulus idea, extending
// UVoxelWorldSubsystem::RingPresets' own pattern outward).
//
// Height source (m2-plan.md "Height source" row): TILE elevation directly
// (30m/px bilinear), NOT the full Amplifier (that is sub-voxel-ring detail,
// invisible at these scales and not worth the cost here) -- sampled through
// a file-local vxc::SyntheticTileSampler in the .cpp using the same
// UVoxelWorldSubsystem::DefaultSeed the voxel world uses, so clipmap terrain
// lines up with the ring cascade's terrain at the shared seam. This header
// stays voxel-core-free by doctrine (see VoxelWorldSubsystem.h's PImpl
// comment) -- the sampling helper is a plain free function in the .cpp, no
// voxel-core type ever appears in a UHT-parsed signature here.
//
// Recenter/rebuild (m2-plan.md "Recenter" row): each level's grid snaps to
// its OWN vertex-spacing grid as the camera moves; a level only rebuilds its
// heights when its snapped origin actually changes, and at most one level
// rebuilds per tick (round-robin across levels) once the initial four-level
// bootstrap (first tick a camera is available) has happened -- a 65x65
// bilinear height fill is cheap enough that the one-time bootstrap building
// all 4 at once is a non-issue, and it avoids a 4-frame terrain pop-in at
// spawn.
//
// Cracks/overlap (m2-plan.md "Cracks/overlap" row): v1 uses skirts (the
// outer grid edge and the inner hole boundary both drop 2x that level's
// vertex spacing) plus per-level annulus culling (quads entirely inside the
// hole are simply not emitted) to hide most seams; z-fighting/visible seams
// against the ring cascade and between adjacent clipmap levels are accepted
// v1 artifacts (plan's CDLOD polish item fixes this properly later).
UCLASS()
class VOXELEARTH_API AVoxelClipmapActor : public AActor
{
	GENERATED_BODY()

public:
	AVoxelClipmapActor();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	// Fixed level count (m2-plan.md binding decision: "4 levels"). Public so
	// the game mode / verification code can reference it without a magic
	// number if ever needed.
	static constexpr int32 NumLevels = 4;

private:
	// 65x65 vertices per level (m2-plan.md binding decision), i.e. 64x64
	// quads -- fixed for every level; only spacing/origin/heights differ.
	static constexpr int32 NumVertsPerSide = 65;
	static constexpr int32 NumVertsTotal = NumVertsPerSide * NumVertsPerSide;
	// (NumVertsPerSide - 1) / 2: the center vertex index on each axis (grid
	// is centred on the camera-snapped origin).
	static constexpr int32 HalfIndex = 32;
	// Hole half-extent in GRID INDICES (not world units): constant across
	// every level by construction (see SpacingUUForLevel in the .cpp) --
	// the inner square hole is always exactly half of the grid's own half
	// extent, i.e. a quarter of the grid's area, matching every level's
	// finer neighbor (the ring cascade for level 0, level L-1 for level
	// L>=1) exactly.
	static constexpr int32 HoleHalfIndex = 16;

	// World-space (UU) vertex spacing for a given level; derived from
	// UVoxelWorldSubsystem::RingPresets so level 0's inner hole always lands
	// exactly on the ring cascade's outer edge (see .cpp).
	static double SpacingUUForLevel(int32 LevelIndex);

	// Builds the triangle index buffer + UV0 array shared by every level
	// (identical local topology for all 4 -- see class comment): the
	// annulus quad mask (skip quads fully inside the [-HoleHalfIndex,
	// HoleHalfIndex) hole) plus front-facing winding. Computed once,
	// lazily, on first use.
	void BuildSharedTopology();

	// Samples heights for level LevelIndex's grid centred at SnappedOriginUU
	// (world XY, UU), computes normals/slope/snow vertex colors, applies
	// inner/outer skirts, and pushes the result via
	// CreateMeshSection/UpdateMeshSection (Create only the very first time a
	// level is built, Update every rebuild after that -- topology never
	// changes, so Update is strictly cheaper: no scene proxy recreation).
	void RebuildLevel(int32 LevelIndex, const FVector2D& SnappedOriginUU);

	// Same camera-lookup fallback chain AVoxelOceanActor::UpdateFollowPlane
	// uses (PlayerCameraManager, falling back to the pawn) -- returns false
	// (Out untouched) if neither is available yet.
	bool GetCameraLocationUU(FVector& OutCameraLocationUU) const;

	// voxel.Debug.Rings (m2-plan.md "Debug" row): cyan tint on every
	// clipmap level, applied/cleared only on a mode transition (never
	// creates a MID while the layer is off, matching the doctrine every
	// other debug-tint call site in this module follows -- see
	// UVoxelChunkComponent::SetDebugTint/ClearDebugTint).
	void UpdateDebugTint();

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> ClipmapRoot;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Clipmap")
	TArray<TObjectPtr<UProceduralMeshComponent>> LevelMeshes;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> ClipmapMaterial;

	// Shared across all levels (see BuildSharedTopology); built once.
	TArray<int32> SharedTriangles;
	TArray<FVector2D> SharedUV0;
	bool bTopologyBuilt = false;

	// Per-level recenter/rebuild bookkeeping.
	FVector2D LastSnappedOriginUU[NumLevels] = {};
	bool bLevelBuilt[NumLevels] = {};

	// True once the first-available-camera bootstrap (all 4 levels built
	// immediately) has run; round-robin (<=1 rebuild/tick) governs every
	// rebuild after that.
	bool bBootstrapped = false;
	int32 RoundRobinCursor = 0;

	bool bLastRingsEnabled = false;
};
