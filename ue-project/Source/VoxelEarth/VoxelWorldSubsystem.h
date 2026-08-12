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
// Forward declaration only -- this header is UHT-parsed and deliberately sees no
// voxel-core types, so InstallWaterMarker takes the pointer opaquely.
//
// IT MUST STAY ABOVE UCLASS(). UHT requires the class declaration to follow
// UCLASS() immediately; anything in between is "Found 'namespace' when
// expecting class while parsing class", which does not name the real problem.
namespace vxc { class IWaterSampler; }

UCLASS()
class VOXELEARTH_API UVoxelWorldSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// DEBUG WATER MARKER (-VoxelWaterMarker=1). Makes every column between its
	// ground and the baked water surface voxelise as MAT_WATERMARK -- solid
	// magenta -- so water placement can be judged at full clipmap range instead
	// of through the near-field renderer's 25.6 m horizontal / 12.8 m vertical
	// bubble.
	//
	// CALL ONCE, DURING BRING-UP, BEFORE ANY WORKER TOUCHES THE WORLD. The
	// brick caches are session-lifetime, so installing later would leave
	// pre-marker bricks resident beside marked ones. `Sampler` is BORROWED and
	// must outlive this subsystem; it must also be safe to call from many
	// threads, because Amplifier::column runs on the mesher pool --
	// vxc::LockedWaterSampler is what makes an ordinary sampler safe to pass.
	// `bIncludeOcean` composes the sea datum in, as lakes.h's
	// implicitWaterDatumMm does for the near-field sweep -- the sampler carries
	// only baked lakes and rivers, because the ocean is not on the wire. Pass
	// false to mark inland water only; near a coast a marked ocean fills the
	// frame and a river shot is unreadable. Pass nullptr to uninstall.
	//
	// RETURNS FALSE IF THE MARKER WAS REFUSED, which happens when the GPU mesh
	// fork is on -- see the refusal in the .cpp for why that pairing cannot be
	// allowed to produce a frame. Callers must not announce a marked world
	// without checking: a log line saying water is drawn as magenta when it is
	// not is the same silent-wrong-evidence failure the refusal exists to stop.
	// Uninstalling (nullptr) always returns true.
	bool InstallWaterMarker(vxc::IWaterSampler* Sampler, bool bIncludeOcean = true);
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
	// Compile-time DEFAULTS only -- unchanged numeric values from the pre-M2
	// single ring. Nothing outside GetRingPresets() below should read this
	// table directly any more: a runtime override
	// (-VoxelRingInnerMeters=/-VoxelRingOuterMeters=) must be visible to every
	// reader of ring radii, and GetRingPresets() is the only place that knows
	// both the defaults and the override switches.
	// ADOPTED 2026-07-27 (owner decision): R0 = 128 m, full cascade to 4 km --
	// the Wave F configuration, promoted from override to default. Measured on
	// real terrain the same day (docs/measurements/gpu-throughput-wave-2026-07-27
	// .txt): settles at 39,020 chunks / 35,205,733 resident quads, identically
	// on both meshers across four legs, in 80-86 s cold. The pool default in
	// GetOrCreateGpuPool was resized from the same measurement. The annuli must
	// still abut exactly (Outer[L] == Inner[L+1]) -- GetRingPresets() enforces
	// it for overrides; for these defaults it holds by construction.
	static constexpr FRingPreset kDefaultRingPresets[VoxelCoords::kNumLevels] = {
		{0.0, 128.0},
		{128.0, 256.0},
		{256.0, 512.0},
		{512.0, 1024.0},
		{1024.0, 2048.0},
		{2048.0, 4096.0}, // R5: the 4 km cascade edge
	};
	// A short initializer list here is silently zero-filled by C++, and the
	// consequences are runtime-silent rather than loud: a {0,0} annulus admits no
	// chunks at that level, and AVoxelClipmapActor derives its ENTIRE vertex
	// spacing from RingPresets[kNumLevels-1].OuterMeters, so a zero there
	// collapses the whole 30 km heightmap to a degenerate zero-extent mesh.
	static_assert(UE_ARRAY_COUNT(kDefaultRingPresets) == VoxelCoords::kNumLevels, "kDefaultRingPresets must have one entry per level");
	static constexpr double UnloadRingMultiplier = 1.25;

	// Runtime accessor for the ring radii (docs/streaming-handoff.md: this
	// table was static constexpr and had to become a runtime accessor before
	// R0 could move to 128 m -- this is that accessor; R0 itself is NOT moved
	// by this change, only the storage). Returns kDefaultRingPresets unless
	// overridden via -VoxelRingInnerMeters=<L0>,<L1>,.../-VoxelRingOuterMeters=
	// <L0>,<L1>,... (comma lists, one entry per level, trailing levels left at
	// default if the list is short -- same convention as
	// VoxelStreamAdmission::GetRingSlotFloors's -VoxelRingFloors=).
	//
	// Command-line rather than a cvar: ring radii decide the shape of the
	// FIRST desired set. FVoxelWorldImpl::RecomputeDesiredSet runs on the
	// very first tick, before any -ExecCmds console command has had a chance
	// to run, so a cvar set that way would only affect chunks streamed in
	// AFTER the command executes and would leave the initial set built from
	// stale radii -- same reasoning as -VoxelMaxRingLevel and -VoxelPerfFlight
	// (see their doc comments). Resolved once, on first call, and cached.
	static const FRingPreset* GetRingPresets();

	// Outermost ring level actually streaming this run (-VoxelMaxRingLevel=<N>,
	// default kNumLevels-1). GetRingPresets()[GetMaxRingLevel()].OuterMeters is
	// therefore where the voxel world really ends, which is what
	// AVoxelClipmapActor has to butt its inner hole against -- see
	// SpacingUUForLevel. Resolved once from the command line at first use.
	static int32 GetMaxRingLevel();

	// Back-compat aliases (R0's radii; a handful of log lines still reference
	// these by name -- unchanged numeric values from the pre-M2 single ring).
	// Functions, not constants, now that level 0's radii can be overridden at
	// runtime (-VoxelRingOuterMeters=).
	static double GetLoadRadiusMeters() { return GetRingPresets()[0].OuterMeters; }
	static double GetUnloadRadiusMeters() { return GetRingPresets()[0].OuterMeters * UnloadRingMultiplier; }

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

	// Track B2 ("real .vxtl terrain tiles as a selectable tile source"):
	// bilinear-samples RAW TILE elevation (not the full Amplifier -- see
	// GetSurfaceHeightUU above for that) at the given world XY, through
	// whichever ITileSampler this run is actually using (the synthetic
	// sampler by default, or a loaded vxc::TileGridSampler under
	// -VoxelTileDir=<path> -- see VoxelWorldSubsystem.cpp's FVoxelWorldImpl
	// for the selection policy). This is the SAME sampler the ring cascade's
	// Amplifier reads underneath it, which is the point: AVoxelClipmapActor's
	// heightmap calls this instead of constructing its own sampler, so
	// clipmap terrain and voxel terrain agree at their shared seam whether or
	// not real tiles are loaded. Plain double signature (no vxc types) so this
	// UHT-parsed header stays voxel-core-free by doctrine. Returns 0.0 if Impl
	// is null (the transient "Entry"/loading world's subsystem instance --
	// see bWorldBegunPlay's doc comment -- never gets a real Impl to sample).
	double SampleTerrainHeightUU(double WorldXUU, double WorldYUU) const;

	// The fine-tier streamer this run is using, or null when there is none
	// (no -VoxelFineTileDir=, or the transient loading world's subsystem
	// instance, which never gets an Impl). BORROWED and owned by FVoxelWorldImpl
	// -- valid only while this subsystem is alive.
	//
	// EXISTS FOR ONE CALLER, UVoxelBathyFieldSubsystem, which needs the resident
	// tile set's baked bathymetry planes and nothing else about the world. A raw
	// forward-declared pointer rather than a wrapper method per query because the
	// thing it needs is FVoxelFineTileStreamer::ReadBathyRect, whose signature
	// carries a vxc type -- and this header is UHT-parsed, so by the doctrine at
	// VoxelFineTileStreamer.h:8-14 it must stay voxel-core-free. A forward
	// declaration of a non-UObject class is invisible to UHT; the vxc types stay
	// behind it in the .cpp that dereferences this.
	class FVoxelFineTileStreamer* GetFineTileStreamer() const;

	// True if the voxel at the given integer voxel-lattice coordinate is
	// solid (overlay-aware World::materialAt != MAT_AIR -- edits are
	// reflected immediately). Game thread only (same constraint as
	// TryDig/TryPlace: Voxels' overlay is not thread-safe). Stage 3b (plan
	// SS3.3, "no Chaos for terrain"): the walk-mode custom kinematic
	// collision in AVoxelEarthFlyPawn queries this per-voxel instead of using
	// a physics engine.
	bool IsSolidAtVoxel(int64 Vx, int64 Vy, int64 Vz) const;

	// --- Water re-architecture Phase 3: the fluid occupancy edit-dirty hook --
	//
	// Registers (unset TFunction clears) the listener UVoxelFluidSubsystem
	// uses to keep its GPU collision volume in step with terrain. Fired, with
	// an INCLUSIVE world-voxel box, from exactly the two call sites
	// VoxelFluidOccupancy.h documents:
	//   * FVoxelWorldImpl::MarkChunkDirtyForRemesh, LEVEL-0 KEYS ONLY (a mip
	//     key would fill the volume at the wrong grain) -- terrain EDITS,
	//     reached from ApplyGroupedEdits;
	//   * FVoxelWorldImpl::ApplyMeshResult, level 0 -- terrain ARRIVING, so a
	//     chunk streaming in inside the volume replaces the unbuilt-solid
	//     placeholder bits.
	// The listener runs on the game thread, must be cheap (it is called per
	// dirtied/delivered chunk), and must tolerate boxes far outside any fluid
	// volume -- it does its own clipping. This is a NOTIFICATION hook only;
	// nothing about meshing or streaming reads anything back from it.
	void SetFluidTerrainDirtyListener(
		TFunction<void(int64 MinVx, int64 MinVy, int64 MinVz, int64 MaxVx, int64 MaxVy, int64 MaxVz)> Listener);

	// A PROVABLE UPPER BOUND, in absolute mm, on the amplified surface over
	// every column in the inclusive voxel rectangle [Vx0, Vx1] x [Vy0, Vy1] --
	// i.e. every voxel whose bottom sits at or above the returned value is
	// guaranteed to be worldgen AIR, because no amplifier pass turns air into
	// solid (the cave and cavern passes only carve). Returns INT64_MIN when the
	// amplifier declines to bound the footprint, which callers must treat as
	// "no information", never as a low bound.
	//
	// This is vxc::Amplifier::surfaceUpperBoundMm, which is where the
	// derivation and its static_asserts live, exposed with plain types so this
	// UHT-parsed header stays voxel-core-free by the same doctrine
	// SampleTerrainHeightUU follows. It exists because callers that sweep a
	// VOLUME -- the implicit-water candidate sweep is the first -- need to
	// reject whole bricks without paying GetSurfaceHeightUU per column: this
	// costs at most one 16x16 block of tile reads and evaluates no detail
	// octave at all, so it is cheaper than a single amplifier column.
	//
	// Pure worldgen: it does NOT see the edit overlay. A caller using it to
	// prove "all air" is proving it about the generated world, so any caller
	// that must also respect placed voxels has to exclude edited bricks itself.
	int64 GetSurfaceUpperBoundMm(int64 Vx0, int64 Vy0, int64 Vx1, int64 Vy1) const;

	// The MIRROR of the above: a guaranteed LOWER bound on the amplified
	// surface over the rectangle, so a caller can prove a brick is entirely
	// BURIED as cheaply as it can prove one is entirely above ground.
	//
	// THE IMPLICIT-WATER SWEEP IS WHAT NEEDED IT. That sweep offers every brick
	// from its box floor up to the flood level with no lower bound from the
	// ground at all, so in shallow water most of what it offers is rock.
	// Measured on the bv14 braided reach: 46,475 of 67,600 bricks offered at
	// one camera (68.8%) are provably underground, and only 12.5% of what the
	// drain managed to mesh emitted a single quad. That -- not the disc rebuild
	// -- is what stopped the water disc finishing
	// (docs/measurements/water-refresh-2026-08-05.txt).
	//
	// Same no-information convention as GetSurfaceUpperBoundMm: MIN_int64 means
	// "declined". For a buried-rejection test that value admits everything,
	// which is the safe direction -- an unbounded floor offers a brick that
	// meshes to nothing, a wrong floor deletes real water.
	//
	// Pure worldgen: it does NOT see the edit overlay, same as its sibling.
	int64 GetSurfaceLowerBoundMm(int64 Vx0, int64 Vy0, int64 Vx1, int64 Vy1) const;

	// ONE amplifier column, both of the worldgen facts a water client needs
	// about it: the amplified surface in absolute mm, and the CAVERN FLOOD
	// LEVEL for that column (INT32_MIN = "no site in reach", the sentinel
	// vxc::CavernColumn::floodZMm uses). Returns false, leaving the outputs at
	// 0 / INT32_MIN, when there is no world yet.
	//
	// THIS EXISTS BECAUSE THE ALTERNATIVE COST A WEEK. UVoxelWaterSubsystem
	// could not reach a cavern column here and so built a SECOND vxc::Amplifier
	// over a SyntheticTileSampler -- which, on a baked run, is a different world
	// from the one on screen. On 2026-08-04 that put its cavern flood levels at
	// 606.166 m over ground the renderer draws at 77.6 m, and the near-field
	// water sweep dutifully offered a 52 m disc of water 528 m up, following the
	// camera. That file's own comment had predicted it and named this accessor
	// as the fix ("a public column accessor on UVoxelWorldSubsystem, which its
	// owner must add"); the lake half had already been moved onto
	// GetSurfaceHeightUU for the same reason a day earlier.
	//
	// BOTH FROM ONE CALL, not two accessors, and that is the point rather than
	// an optimisation: a caller that took the ground from here and the cavern
	// from anywhere else could reintroduce exactly the defect this repairs.
	//
	// Plain int types so this UHT-parsed header stays voxel-core-free by the
	// same doctrine GetSurfaceUpperBoundMm and SampleTerrainHeightUU follow --
	// vxc::cavernWaterAt/cavernWaterCeilingMm take the flood LEVEL for this
	// reason and callers hand it straight over.
	//
	// Game thread only, like GetSurfaceHeightUU, and for the same reason: it
	// prefetches the fine-tile footprint before evaluating the column.
	bool GetWorldgenSurfaceAndCavernFloodMm(int64 Vx, int64 Vy, int32& OutSurfaceMm,
	                                        int32& OutCavernFloodZMm) const;

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

	// --- M5 destruction (first slice, docs/m4-plan.md Round 2 reframe) --------

	// Places a hand-authored blocky voxel "tree" TEST FIXTURE (a solid trunk
	// column + a canopy blob, MAT_ROCK) rooted on the surface at world column
	// (WorldX, WorldY), via the edit-log authority path. This is NOT M4
	// vegetation -- it exists solely to exercise the M5 chop -> island-detect ->
	// fall pipeline (a stand-in that a chop severs). Authority only; behind the
	// GameMode's -VoxelTreeTest switch. Returns the number of voxels stamped.
	int32 SpawnTreeFixtureAt(double WorldX, double WorldY);

	// Places a hand-authored wall + roof-slab + far-pillars TEST FIXTURE rooted
	// at world column (WorldX, WorldY), via the edit-log authority path. Exists
	// solely to exercise M5 LARGE-EDIT structural collapse: blowing out only the
	// far pillars leaves the roof still 6-connected to the ground through the
	// wall, so connectivity alone would (correctly, and uselessly) report zero
	// detached islands, while the support model brings down everything past the
	// cantilever budget. Authority only; behind the GameMode's
	// -VoxelStructureTest switch. Returns the number of voxels stamped.
	int32 SpawnStructureFixtureAt(double WorldX, double WorldY);

	// Underground streaming diagnostic: reports what the streaming system
	// currently holds for the LEVEL-0 render chunk containing WorldPos --
	// whether it is in the desired set at all (bOutTracked), whether it owns a
	// live component, and how many quads it meshed to. Zero quads with
	// bOutTracked true is the expected, correct reading for a fully-solid
	// interior chunk (it has no visible faces, so it holds no component and no
	// GPU memory); bOutTracked FALSE is the "there is no world here" case this
	// task exists to fix. Game thread only; returns false if Impl is null.
	//
	// bOutSettled is FChunkRecord::bMeshSettled: this chunk has finished meshing
	// and its answer is final. It is the ONLY way to tell "queued, not here yet"
	// (tracked, no component, NOT settled) from "meshed to nothing, and always
	// will be" (tracked, no component, SETTLED -- an all-air or fully-buried
	// chunk, which ApplyChunkMesh deliberately leaves component-less). Callers
	// that treat every component-less tracked chunk as "not ready" hang forever
	// on the second case; see UVoxelCharacterMovementComponent::IsTerrainReadyAt.
	bool DebugChunkStatusAt(const FVector& WorldPos, bool& bOutTracked, bool& bOutHasComponent, int32& OutQuads,
	                        bool& bOutSettled) const;

	// docs/debug-tooling-plan.md P1 "Perf HUD": a snapshot refreshed at 1Hz
	// (per-frame collection, see FVoxelWorldImpl::UpdatePerfSnapshot), read by
	// AVoxelEarthHUD every frame when voxel.Debug >= 1. Cheap struct copy;
	// safe to call from the game thread at any time after Initialize.
	FVoxelPerfSnapshot GetPerfSnapshot() const;

	// --- M3 wave 1: multiplayer role split (docs/m3-plan.md) -----------------
	//
	// TryDig/TryPlace/CarveSphere above are role-aware internally (see the
	// .cpp): NM_Standalone applies directly with zero extra work (byte-
	// identical to pre-M3 behavior); NM_DedicatedServer/NM_ListenServer apply
	// directly (today's authority behavior) and then broadcast the newly
	// appended entries via AVoxelEditRelay::MulticastAppliedEntries;
	// NM_Client applies the SAME cells locally as a prediction (tracked
	// internally, reconciled against confirmed entries) and forwards the
	// intent to the server through AVoxelEarthPlayerController's Server RPCs
	// (a shared, unowned relay actor cannot receive client-called Server
	// RPCs -- see VoxelEditRelay.h). Everything below is the plumbing those
	// two actor classes ride on; plain TArray<uint8>/uint64 signatures keep
	// this UHT-parsed header voxel-core-free by doctrine.

	// Wire-format helpers (vxc::ByteWriter/ByteReader-based, NOT
	// vxc::EditLog::serialize's self-describing full-log format -- see the
	// .cpp's SerializeEntries/ParseEntries doc comment): entries with seq in
	// [FromSeq, GetLogSize()) serialized into OutBytes.
	void SerializeLogEntriesFrom(uint64 FromSeq, TArray<uint8>& OutBytes) const;
	uint64 GetLogSize() const;

	// M3 wave 2 "Join-sync compaction" (docs/m3-plan.md): same flat wire
	// format as SerializeLogEntriesFrom, but built from
	// vxc::compactLog(Impl->Voxels.log()) instead of the raw log -- one
	// last-write-wins entry per touched brick, so a join-syncing client
	// receives fewer/smaller chunks for the exact same replayed overlay
	// state (compactLog is proven digest-equal to its source by
	// voxelcore/editcompact.h's own tests). The server's live in-memory log
	// is never mutated -- compaction only ever produces this outgoing copy
	// (append-only doctrine). Used by AVoxelEarthPlayerController::
	// ServerRequestJoinSync in place of the old SerializeLogEntriesFrom(0, ...)
	// call.
	void SerializeCompactedLogEntries(TArray<uint8>& OutBytes) const;

	// Parses OutBytes (SerializeLogEntriesFrom's format) and applies every
	// entry through the same World::applyEdit path TryDig/TryPlace/
	// CarveSphere use (one call per brick), reconciling any matching pending
	// client predictions and marking every touched chunk dirty for re-mesh.
	// Returns false on a corrupt/short buffer (nothing applied).
	bool ApplyReplicatedEntries(const TArray<uint8>& Bytes);

	// Join-sync client-side state machine (m3-plan.md "Join sync"): call
	// BeginJoinSync before issuing the join-sync request RPC, feed every
	// received chunk through ReceiveJoinSyncChunk (bFinal=true triggers the
	// replay), and route every live MulticastAppliedEntries batch through
	// ReceiveLiveEntries -- it automatically buffers live batches that
	// arrive while a sync is still in flight and flushes them, in order,
	// right after the historical replay completes.
	void BeginJoinSync();
	bool ReceiveJoinSyncChunk(const TArray<uint8>& Bytes, bool bFinal);
	bool ReceiveLiveEntries(const TArray<uint8>& Bytes);

	// Determinism-guard handshake probe (m3-plan.md "Determinism guard"):
	// digest of seed + vxc::kWorldGenVersion + 16 fixed Amplifier columns.
	// Computed identically wherever this subsystem's Impl exists; AVoxelEditRelay
	// compares the server's value (replicated) against each client's own
	// locally-computed value at join and hard-disconnects on mismatch.
	uint64 ComputeHandshakeDigest() const;
	uint32 GetWorldGenVersion() const;

	// Deterministic digest over the edit overlay (vxc::World::editedDigest)
	// -- the M3 gate's cross-process comparison value. Also surfaced via the
	// voxel.DumpEditedDigest console command and the -VoxelDumpDigestAfter=<s>
	// command-line switch (AVoxelEarthGameMode / AVoxelEarthPlayerController).
	uint64 GetEditedDigest() const;

	// --- M3 wave 2: persistence (docs/m3-plan.md "Save/load") -----------------
	//
	// Saves the edit log to Saved/VoxelWorlds/<seed>.vxlog (EditLog::serialize
	// format -- the same format voxel-core/bench/editlog_tool.cpp's `stats`/
	// `verify` commands already read), compacting first
	// (voxelcore/editcompact.h's compactLog) when the raw log has more than
	// 2x its compacted entry count -- only the on-disk copy is ever
	// compacted; Impl->Voxels.log() itself (the live append-only log) is
	// never mutated. Atomic tmp+rename write (never leaves a truncated file
	// if the process dies mid-write). Authority only (server/listen/
	// standalone): returns false (logged warning, no-op) if called on
	// NM_Client, since a client has no authoritative log of its own to save
	// -- it relies on join-sync from the server instead. Also invoked by the
	// voxel.SaveWorld console command and automatically from Deinitialize
	// (autosave-on-shutdown). Returns true on success.
	bool SaveWorld() const;

private:
	// Join-sync buffering state (client only; see BeginJoinSync above).
	TArray<uint8> JoinSyncAccumulator;
	bool bJoinSyncInProgress = false;
	TArray<TArray<uint8>> BufferedLiveEntryBatches;


	// M2 task "Config-driven seed": resolved in Initialize() from -VoxelSeed=
	// (default DefaultSeed); see GetSeed() above.
	uint64 Seed = DefaultSeed;

	// M3 wave 2 persistence (docs/m3-plan.md "Save/load"): set true once
	// OnWorldBeginPlay actually runs its game-world/Impl-present body (i.e.
	// this instance's UWorld is a genuine gameplay world, not the transient
	// "Entry"/loading UWorld that -game/-server launches also construct a
	// subsystem instance for -- that phantom instance's Impl is a freshly-
	// constructed EMPTY world that never sees OnWorldBeginPlay before being
	// torn down, and Deinitialize's autosave must NOT write that empty state
	// over a real save file. Checked (not just Impl-non-null) before
	// Deinitialize calls SaveWorld().
	bool bWorldBegunPlay = false;

	TUniquePtr<FVoxelWorldImpl> Impl;

	// Single actor hosting every render-chunk component (unchanged from
	// stage 1: one UVoxelChunkComponent per non-empty render chunk).
	UPROPERTY(Transient)
	TObjectPtr<AActor> ChunkOwner;

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> ChunkRoot;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> ChunkMaterial;

	// --- -VoxelCavernShot: unattended cavern-vista capture --------------------
	//
	// See "Cavern vista capture" in VoxelWorldSubsystem.cpp for what this does
	// and, more to the point, why the framing is computed the way it is: two
	// previous attempts at an underground screenshot were defeated not by the
	// streamer but by parking the camera somewhere useless (on a sunlit
	// hillside; 0.9 m from a wall). This one measures the room it is standing
	// in and reports the measurement alongside the image, so a bad frame is
	// visible in the log rather than only in the PNG.
	void TickCavernShot(float DeltaSeconds);
	bool FindCavernPose(FVector& OutCameraUU, FRotator& OutLookRot, FString& OutReport) const;
	void PoseCavernCamera() const;

	double CavernShotElapsed = -1.0; // < 0 means -VoxelCavernShot was not passed
	double CavernShotSettleSeconds = 45.0;
	bool bCavernShotPosed = false;
	bool bCavernShotCaptured = false;
	bool bCavernShotFailed = false;
	FVector CavernShotCameraUU = FVector::ZeroVector;
	FRotator CavernShotLookRot = FRotator::ZeroRotator;
	// Distance from the camera to the far wall, as MEASURED by FindCavernPose
	// against the voxel world. The residency probe at capture reports how much
	// of it was actually meshed, which is the whole claim under test.
	double CavernShotSightlineUU = 0.0;
};
