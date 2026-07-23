#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelWorldSubsystem.h" // UVoxelWorldSubsystem::DefaultSeed/GetSeed() -- voxel-core-free, safe here (see doctrine note below)
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
// a file-local vxc::SyntheticTileSampler in the .cpp seeded from
// UVoxelWorldSubsystem::GetSeed() (the RUNTIME seed, -VoxelSeed=<u64> or
// DefaultSeed -- see TerrainSeed below), so clipmap terrain lines up with
// the ring cascade's terrain at the shared seam even when -VoxelSeed
// overrides the default. This header stays voxel-core-free by doctrine (see
// VoxelWorldSubsystem.h's PImpl comment) -- the sampling helper is a plain
// free function in the .cpp, no voxel-core type ever appears in a
// UHT-parsed signature here.
//
// Recenter/rebuild (m2-plan.md "Recenter" row): all levels share ONE
// camera-snapped origin (snapped to the FINEST level's vertex spacing) so the
// rings are truly CONCENTRIC -- each level's outer boundary then lands on
// exactly the same world-space square as the next-coarser level's inner hole
// boundary (both +-32*SpacingL == +-16*Spacing(L+1) from the shared centre),
// which is what closes the inter-ring seam (see Tick() for the full
// diagnosis: independent per-level snapping used to give each ring a different
// origin and open a gap between them). A level rebuilds only when the shared
// origin actually changes; at most one level rebuilds per tick (round-robin
// across levels) once the initial four-level bootstrap (first tick a camera is
// available) has happened -- a 65x65 bilinear height fill is cheap enough that
// the one-time bootstrap building all 4 at once is a non-issue, and it avoids
// a 4-frame terrain pop-in at spawn.
//
// Cracks/overlap (m2-plan.md "Cracks/overlap" row): with the rings concentric
// (above) the only inter-level discontinuity left is the T-junction crack (a
// finer ring's edge has 2x the coarser ring's vertex density along the shared
// boundary). The v1 skirts -- dropping BOTH the outer grid edge AND the inner
// hole boundary by 2x spacing -- did NOT hide it once the rings were concentric:
// the two coincident dropped rings dived away from each other into an open
// V-trench (the "dark slab" artifact; see RebuildLevel pass 3). Internal seams
// are now closed by a T-junction STITCH instead (RebuildLevel pass 3): each
// finer level's odd-offset outer-edge vertices are snapped to the average of
// their even neighbours, which coincide with the coarser hole-edge vertices, so
// the shared boundary becomes an identical watertight polyline -- no gap, no
// step, no trench. Quads entirely inside a level's hole are still not emitted
// (annulus culling), and the OUTERMOST level keeps a real downward skirt on its
// true world-edge perimeter. Residual: a per-coarse-cell slope crease at each
// seam (the finer detail meets the coarser chord at a different gradient) --
// cosmetic LOD shimmer, most visible on flat sea-level terrain under a low sun;
// the full per-vertex CDLOD morph (ADR-0002 tripwire) remains the M2-polish item
// that removes it. Residual z-fighting against the ring cascade at the near seam
// is an accepted v1 artifact.
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

	// ---- Underground veil (see the "UNDERGROUND VEIL" block in the .cpp for
	// the full diagnosis this implements) ----------------------------------
	//
	// The world is a SHELL, not a solid: the voxel mesher emits a quad only
	// where solid meets air (voxel-core mesher.h), and the 1-voxel apron is
	// re-derived from the deterministic generator rather than read from
	// neighbor residency -- so underground rock is surrounded by rock and
	// meshes to ZERO geometry, everywhere except the thin band around a
	// carved/natural void. M_VoxelTerrain is one-sided, so the surface skin
	// that DOES exist is backface-culled when viewed from below. Net effect
	// from inside a cave: past the underground streaming radius there is
	// simply nothing, and the two-sided distant actors (this clipmap, the
	// ocean plane) plus the SkyAtmosphere paint a sunlit vista over what
	// should be solid rock.
	//
	// No material or winding change can fix that -- there is no geometry down
	// there to shade. Instead, when the camera actually has rock overhead we
	// hide the clipmap and draw a single inward-facing dark box far outside
	// the voxel cascade, which occludes sky/ocean/clipmap in one draw call
	// while leaving every real cave surface in front of it untouched.

	// Probes straight up from the camera for solid voxels (this is the
	// physically honest test -- "is there rock over my head", not "is my Z
	// below some heightfield sample", which mis-fires by tens of meters on an
	// amplified peak and would switch the veil on ABOVE ground). Costs one
	// amplifier column plus a handful of analytic-in-Z material lookups.
	bool IsCameraUnderRock(const FVector& CameraLocUU) const;

	// Applies the current veil state to the level meshes + shell. Only ever
	// touches components on a state TRANSITION.
	void SetVeilActive(bool bActive);

	// Builds the 8-vertex / 12-triangle inward-facing box once, lazily.
	void EnsureVeilShell();

	// ---- Underground presentation rig -------------------------------------
	//
	// WHY THIS LIVES HERE. It is driven by exactly one predicate --
	// IsCameraUnderRock -- which this actor already evaluates every tick for
	// the veil. A second "am I underground" test would be a second thing to
	// keep in sync, and the brief was explicit about reusing this one.
	//
	// WHY IT CANNOT REGRESS THE SURFACE. Both components below are created
	// lazily on the first underground transition and are switched off
	// (UPostProcessComponent::bEnabled = false, light visibility = false)
	// by the SAME latch that hides the clipmap. Above ground they either do
	// not exist or contribute nothing -- there is no code path in which a
	// surface frame sees a different post-process stack than it did before
	// this change. The M1 flight never goes underground, so it never even
	// constructs them.
	void EnsureCaveRig();

	// Applies the rig's on/off state. Called only from SetVeilActive, i.e.
	// only on a transition.
	void SetCaveRigActive(bool bActive);

	// Unbound (whole-world) post-process whose ONLY overrides are the two
	// auto-exposure brightness clamps. Setting min == max pins eye adaptation
	// to a fixed EV100 instead of letting the histogram hunt for 18% grey in a
	// scene that has no 18% grey in it. See the .cpp for the measurement.
	UPROPERTY(Transient)
	TObjectPtr<class UPostProcessComponent> CaveExposurePP;

	// Camera-mounted lamp. Inverse-square falloff is what turns a flat
	// uniformly-lit box into a cave: near rock reads, far rock falls off into
	// the dark the veil is there to provide.
	UPROPERTY(Transient)
	TObjectPtr<class UPointLightComponent> CaveLamp;

	// -VoxelCaveLight=0 disables the whole rig (exposure lock + lamp) on the
	// same binary, for the A/B. Read once at BeginPlay for the same reason
	// bVeilEnabled is: -ExecCmds lands after init.
	bool bCaveRigEnabled = true;
	bool bCaveRigActive = false;

	// Tunables, all overridable from the command line so a screenshot pass
	// does not need a rebuild. Defaults are the measured values (see .cpp).
	// +10 stops is not a taste value, it is a calibration. AEM_Manual's
	// default physical camera (f/4, 1/60 s, ISO 100) is EV100 ~9.9, i.e. a
	// bright-daylight stop, and the cave is lit by an 8-lux sun. Sweeping the
	// bias at 8 / 10 / 12 against the r.EyeAdaptationQuality 0 reference
	// frame, +10 reproduces it to within a percent of mean luminance
	// (86.3 vs 84.8 over the whole frame, p50 33 vs 31, zero clipped pixels
	// either way). +8 is 4 stops of crush, +12 puts the median back at 104.
	float CaveExposureEV100 = 10.f;  // -VoxelCaveEV=
	// Measured against that same fixed stop: 6000 lm and 2500 lm clip 15% and
	// 6.5% of the frame respectively, 800 lm lifts the frame mean from 86 to
	// 162 (flat, flash-lit), 250 lm lands at 135 with 0.01% clipped. 150 is
	// the low end of that usable band, chosen so the lamp SHAPES the near
	// field rather than replacing the falloff into darkness that makes the
	// image read as a cave at all. With voxel GI on (-VoxelGIOn), whose AO
	// term buys back a lot of headroom, 400 lm is the better-looking number
	// and is what the hero capture uses.
	float CaveLampLumens = 150.f;    // -VoxelCaveLampLumens= (0 disables)
	float CaveLampRadiusUU = 6000.f; // -VoxelCaveLampRadiusM= (x100)

	// -VoxelVeilExtentM=<metres>: shrinks the veil box so it can be
	// PHOTOGRAPHED directly (at 300 m it is always behind real geometry, which
	// is exactly why "the veil looks pale" went undiagnosed for three agents).
	// Diagnostic only; the default is the shipped 300 m.
	double VeilHalfExtentUU = 30000.0;

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> VeilShell;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> VeilShellMID;

	// -VoxelUndergroundVeil=0 disables the whole feature (read once at
	// BeginPlay, NOT a cvar: -ExecCmds applies after systems initialise and
	// can silently measure the same state twice).
	bool bVeilEnabled = true;

	bool bVeilActive = false;
	// False until SetVeilActive has run once, so the very first evaluation
	// always applies (and logs) even though it agrees with bVeilActive's
	// initialiser.
	bool bVeilStateKnown = false;

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

	// M2 task "Config-driven seed": the voxel world's RUNTIME seed
	// (UVoxelWorldSubsystem::GetSeed(), read once in BeginPlay), used to
	// seed SampleHeightUU's vxc::SyntheticTileSampler so clipmap terrain
	// stays lined up with the ring cascade even under a -VoxelSeed override.
	// Defaults to UVoxelWorldSubsystem::DefaultSeed until BeginPlay resolves
	// it (matches the subsystem's own pre-Initialize default; VoxelWorldSubsystem.h
	// is UHT-parsed but voxel-core-free by its own doctrine, so it's safe to
	// include from this UHT-parsed header too).
	uint64 TerrainSeed = UVoxelWorldSubsystem::DefaultSeed;
};
