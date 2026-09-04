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
// last ring, 8,192 m) out to a 65.5 km half-extent, so a summit-level or
// airborne view sees terrain to the horizon instead of the ring cascade's
// hard edge. (Retired 2026-08-30 when the cascade briefly reached 65 km;
// restored as the default far field 2026-09-02 when the cascade was cut
// back to 8 rings -- see VoxelEarthGameMode.cpp's spawn site.)
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
// derivation): NumLevels levels (3), each a fixed 65x65-vertex grid (64x64
// quads). Every level uses the SAME local topology (vertex/index layout never
// changes -- only world placement and sampled heights do), doubling vertex
// spacing per level starting from a spacing derived from the ring cascade's
// own outer radius, so level 0's inner hole lands exactly on the ring edge
// and each level's inner hole exactly matches the previous level's outer edge
// (classic clipmap "hole = quarter area" ratio, held by construction). Total
// coverage: the cascade edge (8,192 m) to an outer half-extent of
// 2 x ringEdge x 2^(NumLevels-1) = 8 x ringEdge = 65.5 km (92.7 km corner).
// Everything here derives from GetRingPresets()[GetMaxRingLevel()], so the
// extent follows the cascade with no code change; the level COUNT is the only
// lever, and it trades per-rebuild vertex work against far-field spread.
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
	// THREE SINCE 2026-08-30, down from four, and this SHRINKS the far field on
	// purpose.
	//
	// The extent is forced arithmetic, not a choice: level 0's hole must land on
	// the ring cascade's outer edge, so spacing0 = ringEdge / HoleHalfIndex, and
	// L doubling levels then reach 32 * spacing0 * 2^(L-1). At four levels that
	// is 16 x ringEdge -- which was 65 km at the 4 km cascade and became 131 km
	// the moment the cascade doubled. Nothing is visible at 131 km: this scene's
	// fog and aerial perspective take terrain to the sky's own colour tens of
	// kilometres before that (measured: the far bands read (93,109,125), which is
	// fog, not ground).
	//
	// AND IT IS FREE TO CUT, because the clipmap's cost does NOT scale with its
	// extent -- every level is a fixed 65x65 grid however far it reaches, so a
	// wider extent just spreads the same vertices thinner. Dropping a level
	// halves the reach to 8 x ringEdge (65 km at the 8 km cascade) AND removes a
	// quarter of the per-rebuild vertex work, which matters more than it used to:
	// each vertex now costs a climate sample and a vxc::classifyBiome call, on
	// the GAME THREAD, and the p99 tail here is game-thread-owned.
	static constexpr int32 NumLevels = 3;

	// Half-extent of the OUTERMOST level's square grid, in world UU, measured
	// from the shared camera-snapped origin. The farthest drawn point of this
	// actor is the grid CORNER, i.e. this times sqrt(2).
	//
	// PUBLIC FOR THE SAME REASON NumLevels IS: so that code which has to be
	// bigger than the clipmap can ask instead of guessing. AVoxelSkyDomeActor is
	// the first such caller -- M_NightSky depth-tests, so a star dome inside this
	// radius is occluded by distant terrain (Tools/create_sky_material.py, "DEPTH
	// TEST STAYS ON") -- and a hard-coded number there would silently stop being
	// true the first time -VoxelRingOuterMeters moved.
	//
	// NOT constexpr, and that is the whole point: this scales with the ring
	// cascade's runtime outer radius (see SpacingUUForLevel), so it changes
	// with -VoxelRingOuterMeters and -VoxelMaxRingLevel. At the shipped
	// defaults (cascade edge 8,192 m, NumLevels 3) it is 6,553,600 UU =
	// 65.5 km. The compile-time restatement AVoxelOceanActor sizes its plane
	// from is asserted against NumLevels in VoxelOceanActor.cpp.
	static double OuterHalfExtentUU();

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

	// Unbound (whole-world) post-process whose ONLY overrides are the
	// auto-exposure method and bias, pinning eye adaptation to a fixed stop
	// instead of letting the histogram hunt for 18% grey in a scene that has no
	// 18% grey in it. See the .cpp for the measurement.
	//
	// IT IS NOT THE ONLY UNBOUND EXPOSURE VOLUME ANY MORE. UVoxelSkySubsystem
	// owns a second one (the day/night EV curve) and two unbound volumes fight
	// silently -- highest Priority takes every field both override. The
	// ownership rule between them is written out in full beside
	// `CaveExposurePP->Priority = 100.f` in the .cpp, and again in
	// VoxelSkySubsystem.cpp. Read one of them before touching either volume.
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

	// ---- SNOW OVERRIDE (why this exists, and why it is off by default) -----
	//
	// THE EFFECT: the far clipmap draws flat white over its high ground. Above
	// 3000 m, 100.0% of this world's land is at full snow.
	//
	// M_VoxelClipmap's snow term is the ONLY white in the whole graph -- every
	// other colour it can produce is a biome, rock grey, or beach tan, none of
	// them above 0.72 in any channel. The term fires two ways
	// (Tools/terrain_material_common.py):
	//
	//   snow_from_temp = 1 - ramp(VertexColor.B, SnowTempMax, +SnowTempFeather)
	//   snow_from_z    =     ramp(worldZ_m,  SnowlineLowMeters, SnowlineHighMeters)
	//   snow           = saturate(max(the two))
	//
	// and its four constants were fitted to a world that no longer exists.
	// terrain_material_common says so in as many words: "the world Z term only
	// bites above ~2700 m, and THIS WORLD'S HIGHEST POINT IS 2897 m, so it is a
	// cap on the very tops". The world configured today (DefaultGame.ini
	// DefaultTileDir, provider 80b9ca451a23eae4, 289 tiles) reaches 6331 m. A
	// snowline at 93% of the old world's maximum height sits at 43% of this
	// one's, and "a cap on the very tops" has become most of a mountain range.
	//
	// MEASURED on that world's tiles, replaying the graph over all 33,234,574
	// land pixels (snow weight >= 0.9, i.e. essentially pure snow_color):
	//
	//   land overall     12.4%
	//   land >= 1500 m   44.2%
	//   land >= 2500 m   87.7%
	//   land >= 3000 m  100.0%
	//
	// WHY THIS IS A RUNTIME OVERRIDE AND NOT AN EDIT TO THE MATERIAL. All four
	// constants are ScalarParameters on the shipped M_VoxelClipmap.uasset
	// (verified in its name table: SnowlineLowMeters, SnowlineHighMeters,
	// SnowTempMax, SnowTempFeather), so a MID can move them with no asset
	// regeneration at all -- and regenerating a .uasset in this project breaks
	// dependents silently and has its own procedure.
	//
	// WHY IT DEFAULTS TO DOING NOTHING. Where the snowline BELONGS on a 6331 m
	// world is a judgement about how the world should look, and this is judged
	// by eye by the owner, not by me. With no switch passed, no MID is created
	// and no parameter is set, so the frame is byte-identical to one taken
	// before this change. Pass any of the four and the pair of captures is an
	// A/B on one binary.
	//
	//   -VoxelClipmapSnowlineLowM=<m>      -VoxelClipmapSnowlineHighM=<m>
	//   -VoxelClipmapSnowTempMax=<0..1>    -VoxelClipmapSnowTempFeather=<0..1>
	//
	// A CANDIDATE THAT WAS CHECKED AND IS WRONG, so nobody spends a capture on
	// it: scaling the old constants by the height ratio (6331/2897) gives about
	// 5900/6300 m, and this world has only 0.005% of its land above 5900 m --
	// that is not a higher snowline, it is no snow anywhere. The scaling fails
	// because the two worlds have different SHAPES, not just different maxima:
	// this one's p99.9 is 4898 m, so its top 1400 m is a handful of summits.
	//
	// The hypsometry, so a number can be picked instead of derived (fraction of
	// this world's land above each height, all 289 tiles):
	//
	//   2700 m  3.18%     3500 m  0.59%     4500 m  0.20%
	//   3000 m  1.79%     4000 m  0.35%     5000 m  0.08%
	//
	// AND WHATEVER IS PICKED MUST MOVE IN TWO PLACES. 2700 m was chosen to match
	// voxel-core's MAT_SNOW threshold (amplifier.cpp, voxel-core/shaders/
	// worldgen.ush); moving the material without moving that splits the vista
	// from the near field at the snowline, which is precisely the class of
	// divergence terrain_material_common.py was written to end. These switches
	// are for finding the number by eye, not for shipping it.
	bool bSnowOverrideActive = false;

	// -VoxelClipmapVertexAlbedo / -VoxelClipmapAlbedoSrgb. See the parse site
	// in BeginPlay for what this changes and why one switch arms both the
	// vertex encoding and the material parameter.
	// DEFAULT 1.0 SINCE 2026-08-30 -- ONE COLOUR AUTHORITY IS NOW THE SHIPPING
	// BEHAVIOUR. The far field paints from vxc::kMaterialPalette via the same
	// vxc::classifyBiome the voxels use, instead of a biome LUT whose colours
	// were hand-authored independently. -VoxelClipmapVertexAlbedo=0 is the
	// control arm and restores the LUT path exactly (no MID is created at 0).
	float VertexAlbedoWeightOverride = 1.0f;
	bool bVertexAlbedoActive = false;
	bool bAlbedoAsSrgb = true;
	// Virtual-ring voxelization strength (2026-09-02, owner-accepted): the far
	// field continues the voxel cascade's cell law in the material's pixel
	// shader -- terraced steps, axis-quantized normals, per-cell colour,
	// hemisphere-ambient parity. 1.0 = voxelized (default, matches the
	// generated material's own default); -VoxelClipmapVoxelize=0 is the smooth
	// control arm for the seam A/B.
	//
	// V3 (2026-09-04, owner: "even more voxelized, with its own LODs"): the
	// SAME switch now also gates GEOMETRIC voxelization -- RebuildLevel
	// quantizes every vertex height to the band ladder's cell (real 3D
	// terraces, so the SILHOUETTE steps too, which the v2 pixel shader could
	// not do). One switch arms both halves on purpose, same doctrine as
	// -VoxelClipmapVertexAlbedo: a frame where the mesh terraced but the
	// shader read smooth cells (or vice versa) is a picture nobody can
	// interpret. At 0 both halves are byte-identical to the smooth arm.
	float VoxelizeWeightOverride = 1.0f;

	// ---- THE ONE BAND LADDER (v3, 2026-09-04) ------------------------------
	// Cell size is VoxelizeBaseCellUU at the cascade seam, doubling per
	// distance band: cell(b) = base * 2^b for Chebyshev distance in
	// [seam*2^b, seam*2^(b+1)), clamped at band 3. BOTH consumers -- the
	// vertex quantization in RebuildLevel and M_VoxelClipmap's pixel shader --
	// take these two numbers from HERE: RebuildLevel uses them directly, and
	// ApplyLevelMaterial pushes them onto every level MID as the
	// VoxelizeSeamUU / VoxelizeBaseCellUU scalar parameters the shader's
	// ladder is written against. Derived in BeginPlay from the ring cascade
	// (seam = active outer edge; base cell = the coarsest ring's voxel size,
	// VoxelCoords::VoxelSizeUU << MaxRingLevel), and checked there against the
	// generated material asset's fallback defaults (819,200 / 1,280 UU) so a
	// cascade override cannot silently split the two derivations. Declaration
	// defaults match the shipped cascade for the pre-BeginPlay window.
	double VoxelizeSeamUU = 819200.0;    // 8,192 m
	double VoxelizeBaseCellUU = 1280.0;  // 12.8 m = R7's voxel size
	float SnowlineLowMetersOverride = 2700.f;
	float SnowlineHighMetersOverride = 2900.f;
	float SnowTempMaxOverride = 0.16f;
	float SnowTempFeatherOverride = 0.10f;

	// Per-level MID, created only when something actually needs one (the snow
	// override at BeginPlay, or voxel.Debug.Rings on a transition). Null entries
	// mean "this level is drawing the plain shared material", which is the
	// shipped path and stays the shipped path unless a switch is passed.
	//
	// ONE ARRAY FOR BOTH USERS, and that is the point: the debug tint used to
	// create its own MID and then throw it away by calling SetMaterial(0,
	// ClipmapMaterial) when the layer turned off. With a second parameter source
	// on the same material that would have silently reverted the snowline every
	// time the ring debug layer was toggled off. ApplyLevelMaterial is now the
	// single place that decides what a level draws with.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> LevelMIDs;

	// Points a level's PMC at its MID if it needs one, at the plain shared
	// material if it does not, and re-pushes every parameter the MID owns.
	// Called from the first build of a level and from every debug-tint
	// transition, so no path can leave a level holding a material with half its
	// parameters set.
	void ApplyLevelMaterial(int32 LevelIndex);

	// -VoxelClipmapColorCensus: after each level rebuild, log what the vertex
	// colours this actor just wrote will DO in the material -- the fraction
	// inside the magenta marker gate, the fraction at full snow by each of the
	// two routes, and how much of each biome-LUT axis is pinned at an end.
	//
	// WHY AN INSTRUMENT AND NOT A DESCRIPTION. "The vista is white" and "the
	// vista is magenta" are the same sentence for four different mechanisms, and
	// every one of them was a live candidate here (a failed material load, an
	// unbound parameter, a UV running out of range, a per-band difference). This
	// turns the screenshot into numbers taken from the ACTUAL run, with the
	// actual tiles and the actual seed, on the same data the material reads --
	// which is the only kind of reading that has ever settled one of these.
	//
	// The thresholds it applies are copied from Tools/terrain_material_common.py
	// and are named beside their use in RebuildLevel. If that graph moves and
	// this does not, the census lies -- so it prints the thresholds it used.
	bool bColorCensus = false;

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> ClipmapRoot;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Clipmap")
	TArray<TObjectPtr<UProceduralMeshComponent>> LevelMeshes;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> ClipmapMaterial;

	// Shared across all levels (see BuildSharedTopology); built once.
	TArray<int32> SharedTriangles;

	// Level 0's topology, identical to SharedTriangles except its hole is a
	// DISC rather than a square -- level 0 abuts the voxel ring cascade, which
	// is admitted radially, while every other level abuts a square clipmap
	// level. See BuildSharedTopology for the hole this difference closed.
	TArray<int32> SharedTrianglesLevel0;
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
