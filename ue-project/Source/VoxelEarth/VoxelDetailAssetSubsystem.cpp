// TASK #7: render the invisible 85% -- detail-lattice (L3) ground cover as
// instanced static meshes.
//
// ---------------------------------------------------------------------------
// WHAT THIS IS, IN ONE PARAGRAPH
// ---------------------------------------------------------------------------
//
// voxel-core already resolves every grass tuft, fern, flower, reed and small
// rock deterministically (vxc::AssetField::instancesForRect with
// terrainOnly=false) -- through the same biome/elevation/slope/anchor gates as
// the trees -- and until this file existed nothing consumed the answer
// (docs/asset-placement-audit.md section 5.3: 85% of all instances, 266
// species, invisible). This subsystem walks a ring of 2x2 level-0 chunk
// groups around the streaming anchor, resolves each group's detail instances
// ON A WORKER (the same vxc::Amplifier::column the meshing workers already
// call concurrently -- never the render thread, never the game thread),
// converts each (species, seed) grid ONCE into a small vertex-coloured static
// mesh (naive visible-face quads: the grids are 100-5000 voxels and per-voxel
// colour jitter would defeat greedy merging anyway), and draws the instances
// through one HISM component per (species, seed), transformed by anchor
// position + yawQuarter.
//
// ---------------------------------------------------------------------------
// WHAT THIS DELIBERATELY DOES NOT DO
// ---------------------------------------------------------------------------
//
//  * NO ANIMALS (owner: they need animation first). Structural, not a filter:
//    fish/bird/quadruped/cetacean species carry layer 255
//    (vxc::kAssetLayerNotScattered), never enter the folded species table, and
//    therefore can never come out of instancesForRect. The worker still skips
//    any grid with rig parts (hasParts()) so the exclusion holds even if a
//    plant were ever baked with a rig by mistake.
//  * NO PLACEMENT LOGIC. Same seed + tiles => same instances, because this
//    file only consumes the resolver. It computes nothing about where
//    anything stands.
//  * NO WORLD-LATTICE VOXELS, NO COLLISION, NO EDITS. Detail grids never
//    enter the world grid by design (vxc::AssetGrid::onTerrainLattice() is
//    false for them, and the streaming bounds deliberately exclude detail
//    layers -- assetplacement.h). Presentation only: worldgen digests,
//    admission bounds and multiplayer determinism are untouched by
//    construction. Ground cover is walk-through (the same "no Chaos for
//    terrain" doctrine; a grass tuft with a collision body would also be a
//    desync risk, since clients do not agree on WHEN cover is resolved).
//  * NO REACTION TO DIG/PLACE EDITS (v1): a dug-out column keeps its resolved
//    cover until the group leaves the ring and re-resolves. Terrain-lattice
//    assets get this right through the overlay; cover accepting the same
//    staleness window as its ring is the v1 trade, recorded in
//    docs/detail-asset-rendering.md.
//
// ---------------------------------------------------------------------------
// THREADING, AND WHY IT IS SOUND
// ---------------------------------------------------------------------------
//
// The worker calls exactly what the level-0 meshing jobs already call from
// many threads at once: Amplifier::column (safe -- see VoxelFineTileStreamer.h
// "THREADING" note: resident tiles are fully decoded at load and the tile map
// is rwlocked) and IAssetBankSource::bankGrid (loads serialised under the
// library's own mutex; returned grids are immutable for the process
// lifetime). The one rule that makes worker column queries legal is the
// fine-tier residency gate: a worker query into a non-resident fine tile is a
// GATE LEAK and stops an unattended run. So a group is only dispatched after
// IsFootprintResident over its rect dilated by the layer table's maximum
// reach -- the same shape FootprintChunkZRangeCached uses for exact
// admission, with the dilation taken over ALL layers because
// instancesForRect(terrainOnly=false) evaluates terrain-layer anchors too.
//
// Job lifetime: every launched task is tracked and waited on in
// Deinitialize(), and Initialize() declares InitializeDependency on
// UVoxelWorldSubsystem, so this subsystem deinitializes FIRST and the
// borrowed AssetField/Amplifier/bank pointers outlive every job by
// construction.
//
// ---------------------------------------------------------------------------
// COLOUR
// ---------------------------------------------------------------------------
//
// One flat colour per voxel face from vxc::kMaterialPalette (the same table
// the terrain's asset voxels are shaded from -- one definition, every
// consumer), face-classed top/side/bottom, plus the palette's own per-voxel
// lightness jitter hashed from the voxel's LOCAL coordinates (deterministic
// per (species, seed) mesh; instances of the same seed are identical, exactly
// as terrain-lattice trees are). Colours are authored sRGB; they are handed
// to the mesh build as LINEAR floats because UStaticMesh::BuildFromMeshDescription
// re-encodes vertex colours with ToFColor(true) (verified in the 5.8 source),
// and the GPU reads the colour buffer as raw UNORM -- so the material
// (M_VoxelDetailAsset, Tools/create_detail_asset_material.py) applies the
// sRGB decode. If that asset is missing this renders grey cover with correct
// shapes and says so loudly, never crashes.
//
// See docs/detail-asset-rendering.md for the architecture write-up and what a
// capture must show.

#include "VoxelDetailAssetSubsystem.h"

#include "VoxelCoords.h"
#include "VoxelDebug.h" // VoxelDebug::kHitchThresholdMs -- the adaptive budgets' bar
#include "VoxelEarth.h"
#include "VoxelEofDirtyLedger.h" // EndOfFrameUpdates attribution -- one count per HISM dirtied
#include "VoxelFineTileStreamer.h"
#include "VoxelWorldSubsystem.h"

// PHASE 6: the cover funnel does not end at the producer. The index is the other
// half of it, and "the producer packed 12,000 chunks" against "the index holds
// 0" is a routing answer that neither counter can give alone -- which is the
// offered/resident/dropped ownership split that answered the L1 question in one
// grep instead of a whole leg.
#include "VoxelBrickPool.h"            // Phase 6: cover is published into the same pool as terrain, at level 7
#include "VoxelBrickCpuPackFromCore.h" // ...through the ONE vxc::ChunkBrickPack -> FVoxelBrickCpuPack copy
#include "VoxelMarchChunkIndex.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "MeshDescription.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "StaticMeshAttributes.h"
#include "Tasks/Task.h"
#include "UObject/Package.h"

// voxel-core: allowed here (this .cpp is not UHT-parsed), same doctrine as
// VoxelWorldSubsystem.cpp.
#include "voxelcore/amplifier.h"
#include "voxelcore/assetfield.h"
#include "voxelcore/assetgrid.h"
#include "voxelcore/assetplacement.h"
#include "voxelcore/assetpolicy.h"
#include "voxelcore/core.h"
#include "voxelcore/covervolume.h"
#include "voxelcore/hash.h"
#include "voxelcore/materialpalette.h"

#include <atomic>

namespace
{
// One 2x2 block of level-0 chunks: 64 voxels = 6.4 m on a side. Small enough
// that a group's resolve is ~100 amplifier columns (a level-0 mesh job pays
// ~1,150), large enough that the ring is ~1,000 groups rather than ~4,000.
// ---------------------------------------------------------------------------
// THE COVER VOLUME ARM (voxel.Cover.Produce) -- default OFF.
// ---------------------------------------------------------------------------
//
// docs/detail-assets-in-the-volume-2026-08-19.md is the design and the census.
// The producer itself is engine-free (voxelcore/covervolume.h): it is
// packChunkBricksCanonical over the cover composition reference, so a GPU cover
// stamp is checkable against exactly what runs here.
//
// WHAT THIS ARM DOES AND DOES NOT DO TODAY, STATED PLAINLY SO IT IS NOT
// MISTAKEN FOR A RENDERER. There is no cover STORE yet -- the sparse store is
// blocked on the brick pool / marcher seam, which is mid-revert after a 188x
// traversal regression. So with this cvar on, every group packs its cover
// chunks and THROWS THE PACK AWAY. That is deliberate and it is a measurement,
// not an oversight: it is the "generate, pack, publication stubbed" arm the
// ray-marching plan section 8 asks for, and it prices the producer and sizes
// the volume on real ground without waiting on a consumer. NOTHING RENDERS
// FROM IT. The HISM path is untouched and still draws the cover.
//
// 0 (default) = this file behaves exactly as it did before the arm existed.
int32 GVoxelCoverProduce = 0;
FAutoConsoleVariableRef CVarVoxelCoverProduce(
	TEXT("voxel.Cover.Produce"),
	GVoxelCoverProduce,
	TEXT("1 = each detail resolve job ALSO packs its group into cover-volume bricks ")
	TEXT("(voxelcore/covervolume.h) and discards them -- a producer-cost and volume-size ")
	TEXT("arm, nothing renders from it. 0 (default) = untouched. Latched per dispatch."),
	ECVF_Default);

// THE MUTATION ARM FOR THE INDEX'S CONSERVATION LAW.
//
// offered == admitted + droppedOutOfBand has never failed, which is exactly when
// a reader starts relying on it. At 1 the index refuses the FIRST cover chunk it
// is ever offered and counts it NOWHERE, so the law must read VIOLATED. Its
// precondition is identical to the law's -- a cover chunk was offered -- so if
// voxel.Cover.Stats does not say NOT EXERCISED, this arm has fired.
//
// If the law still reads CONSERVED under this, the law is decorative and every
// cover funnel it has blessed is unverified. NEVER leave it on.
int32 GVoxelCoverMutateIndex = 0;
FAutoConsoleVariableRef CVarVoxelCoverMutateIndex(
	TEXT("voxel.Cover.MutateIndex"),
	GVoxelCoverMutateIndex,
	TEXT("MUTATION ARM, default 0. 1 = the march chunk index refuses the first cover chunk it is ")
	TEXT("offered and counts it nowhere, so voxel.Cover.Stats MUST report the conservation law ")
	TEXT("VIOLATED. A check that has never failed is not yet known to be a check. Never ship at 1."),
	ECVF_Default);

// THE PUBLICATION GATE, SEPARATE FROM THE PRODUCTION GATE.
//
// voxel.Cover.Produce 1 alone is the pack-and-discard measurement arm that has
// existed since 2026-08-20: it prices the producer and sizes the volume without
// a consumer. Adding voxel.Cover.Resident 1 is what makes those packs reach
// FVoxelBrickPool at level 7, become visible to the march chunk index, and cost
// VRAM.
//
// TWO CVARS AND NOT ONE, for the reason voxel.GPU.BrickPack and
// voxel.GPU.BrickPackResident are two: with one, "the producer never ran" and
// "the producer ran and nothing was stored" are the same reading, and this
// project has spent three separate diagnoses on exactly that ambiguity.
//
// SUBORDINATE: with Produce 0 this does nothing at all, because there is no pack
// to publish. Latched per dispatch, like Produce.
int32 GVoxelCoverResident = 0;
FAutoConsoleVariableRef CVarVoxelCoverResident(
	TEXT("voxel.Cover.Resident"),
	GVoxelCoverResident,
	TEXT("1 = cover packs are published into the brick pool at level 7 and indexed by the march ")
	TEXT("chunk index; 0 (default) = packed and discarded, which is the producer-cost arm and the ")
	TEXT("control for what publication costs. Requires voxel.Cover.Produce 1 -- with that off there ")
	TEXT("is no pack to publish and this is ignored. Nothing RENDERS from the pool until the cover ")
	TEXT("march segment is enabled separately."),
	ECVF_Default);

// Process-lifetime cumulative, incremented from workers. Same doctrine as
// vxc::Counters: the UE layer owns the instrument, voxel-core stays clean.
vxc::CoverProducerCounters GCoverCounters;

// REFUSED, NOT SILENTLY DEGRADED. A null channel source is the sentinel world
// (fail-closed water gates), and a cover volume built from it is a DIFFERENT
// WORLD -- the binding rule of 2026-08-17. Counted so "cover produced nothing"
// can never be confused with "cover was never allowed to run".
std::atomic<uint64> GCoverGroupsRefusedSentinel{0};
// ---- Phase 6: the publication funnel, and every term can move -------------
//
// FILE SCOPE, like the two counters below, so voxel.Cover.Stats can print them:
// the impl is per-subsystem and that command is static. Game thread only, but
// atomic for the same reason its neighbours are -- one doctrine per file.
//
// resident and released are the two halves of a lifetime; REFUSED is the pool
// declining an add (arenas full after eviction); MISSING is a release finding
// the pool no longer holds a chunk this subsystem believes it owns. The last two
// must NOT be folded into one "cover chunks" number: "the pool is full" and "the
// pool evicted behind our back" have different owners and identical visible
// symptoms -- cover absent from ground the player is standing on.
std::atomic<int64> GCoverChunksResident{0};
std::atomic<int64> GCoverChunksReleased{0};
std::atomic<int64> GCoverPublishRefused{0};
std::atomic<int64> GCoverReleaseMissing{0};
// A group whose instances span more z than any plausible cover shell. Capped
// rather than walked, and COUNTED, because an uncounted cap is the silent
// no-op this project keeps paying for.
std::atomic<uint64> GCoverGroupsZClamped{0};
constexpr int32 kCoverMaxChunkLayers = 32;

// The pitch the land detail library is baked at. The 7 species at 20 mm are the
// reef set (ocean-weighted, zero on land) and are refused BY NAME at init.
constexpr uint32 kCoverPitchMm = 50;

// The INDEX half of the funnel, printed by voxel.Cover.Stats beside the
// producer half. Separate line, separate verdict, on purpose: "the producer
// found nothing" and "the producer found plenty and none of it reached the
// index" are different owners, and a single combined number cannot tell them
// apart. That ownership split is what answered the L1 residency question in one
// grep instead of a whole leg.
// The PUBLICATION half of the funnel: producer -> pool. Printed between the
// producer's funnel and the index's, because that is the order the bytes travel
// and because a gap between any two adjacent lines names its own owner.
void PrintCoverPublication()
{
	const int64 Resident = GCoverChunksResident.load(std::memory_order_relaxed);
	const int64 Released = GCoverChunksReleased.load(std::memory_order_relaxed);
	const int64 Refused = GCoverPublishRefused.load(std::memory_order_relaxed);
	const int64 Missing = GCoverReleaseMissing.load(std::memory_order_relaxed);
	if (Resident == 0 && Released == 0 && Refused == 0)
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelCoverPool: NOTHING WAS EVER PUBLISHED -- voxel.Cover.Resident is %d "
		            "(needs 1, and voxel.Cover.Produce 1 with it). This is not a statement about "
		            "the pool."),
		       GVoxelCoverResident);
		return;
	}
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelCoverPool: %lld cover chunks resident, %lld released, %lld REFUSED by the "
	            "pool (arenas full after eviction -- read voxel.Brick stats for allocFailures), "
	            "%lld release-missing."),
	       (long long)Resident, (long long)Released, (long long)Refused, (long long)Missing);
	if (Missing > 0)
	{
		// NOT BENIGN, and it is an Error because the symptom is invisible: the
		// pool evicted cover this subsystem still believes it owns, so the
		// group's list and residency have diverged and cover goes missing from
		// ground the player is standing on with every other counter healthy.
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("VoxelCoverPool: %lld cover chunks were gone from the pool by the time their "
		            "group released them. The pool is evicting cover behind this subsystem's back, "
		            "so group ownership and residency have diverged. Cover lifetime is supposed to "
		            "be owned by the detail ring, not by eviction pressure."),
		       (long long)Missing);
	}
}

void PrintCoverIndexFunnel()
{
	FVoxelMarchChunkIndex& Idx = GetGlobalVoxelMarchChunkIndex();
	// Re-armed here only so the flag reflects the cvar when the detail tick is
	// inert (no assets installed). THE ARMING THAT MATTERS IS IN THE TICK -- by
	// the time this runs, every cover chunk this leg will ever publish has
	// already been offered to the index. Both sites read the same global and set
	// the same value, so they cannot disagree; deleting the tick one because this
	// exists would silently disarm the mutation.
	Idx.SetMutateCoverConservation(GVoxelCoverMutateIndex != 0);

	// THE SINK MAY NOT EVEN BE CONNECTED, and that is a different answer from
	// "no cover was offered". The index attaches from VoxelMarchPublishSource,
	// which the FLUID subsystem drives -- so a leg without fluids leaves every
	// counter below at zero for a reason that has nothing to do with cover.
	if (!Idx.IsAttached())
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelCoverIndex: THE INDEX IS NOT ATTACHED TO THE POOL -- it is hooked up by "
		            "VoxelMarchPublishSource (driven by the fluid subsystem), which has not run in "
		            "this session. Cover may well have been published into the pool; nothing was "
		            "listening. The zeroes below are about the sink, not about the ground."));
		return;
	}

	FString Verdict;
	const FVoxelMarchChunkIndex::ECoverConservation Result = Idx.CheckCoverConservation(Verdict);
	// NOT EXERCISED IS LOGGED AT Log AND SAYS SO IN WORDS. It is not a pass, and
	// the one thing it must never do is look like one. Only a genuine violation
	// is an Error, so a leg grep for Error is not poisoned by "cover was off".
	//
	// Two UE_LOG sites rather than a verbosity expression: UE_LOG's verbosity is
	// a compile-time token, not a value.
	if (Result == FVoxelMarchChunkIndex::ECoverConservation::Violated)
	{
		UE_LOG(LogVoxelEarth, Error, TEXT("VoxelCoverIndex: %s"), *Verdict);
	}
	else
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelCoverIndex: %s"), *Verdict);
	}
	if (Result == FVoxelMarchChunkIndex::ECoverConservation::NotExercised)
	{
		return;
	}
	const FIntVector Span = Idx.GetCumulativeCoordSpan(FVoxelMarchChunkIndex::kCoverLevel);
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelCoverIndex: resident %d cover chunks in grid slot %u; alias collisions %d "
	            "(non-zero means one cover chunk is shadowing another and the shadowed one is a "
	            "HOLE the marcher invented); cumulative cover coord span (%d,%d,%d) against a "
	            "band radius of %d chunks -- THE SPAN IS A TRAVEL LOG, NOT AN ALIASING CLAIM."),
	       Idx.GetCoverEntries(), FVoxelMarchChunkIndex::kCoverGridSlot,
	       Idx.GetCoverAliasCollisions(), Span.X, Span.Y, Span.Z,
	       FVoxelMarchChunkIndex::kCoverBandRadiusChunks);

	// ---- THE RECONCILIATION, SUBTRACTED HERE RATHER THAN BY A READER --------
	//
	// Publication is bounded by the DETAIL RING (256 m); the index band is
	// +/-40 cover chunks (64 m). They are deliberately mismatched, so cover
	// between the two radii is RESIDENT IN THE POOL, costing VRAM, and INVISIBLE
	// TO THE MARCHER. That gap is the number below.
	//
	// AND IT DOES NOT CLOSE BY ITSELF AS THE CAMERA APPROACHES. Admission is
	// decided ONCE, when the pool emits the Added delta; there is no re-offer
	// when the band moves. So a cover chunk published at 200 m and dropped stays
	// dropped even when the camera walks to 10 m from it -- until its group
	// leaves the unload ring and re-resolves. The symptom would be a hole in
	// cover that travels with the camera, and NOTHING ELSE WOULD LOOK WRONG.
	//
	// Printed, not fixed: the fix is to bound publication to the band, and that
	// would change the admitted/offered ratio this leg was pre-registered
	// against. Decide it against this number rather than against my estimate.
	{
		const int64 PoolResident = GCoverChunksResident.load(std::memory_order_relaxed);
		const int64 IndexResident = int64(Idx.GetCoverEntries());
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelCoverReconcile: pool holds %lld cover chunks, the index can see %lld -- "
		            "%lld are RESIDENT BUT UNMARCHABLE (published outside the +/-%d-chunk band and "
		            "never re-offered). Admission is decided once, at publish time; it does not "
		            "reopen as the camera approaches."),
		       (long long)PoolResident, (long long)IndexResident,
		       (long long)(PoolResident - IndexResident),
		       FVoxelMarchChunkIndex::kCoverBandRadiusChunks);
	}
}

void PrintCoverStats()
{
	const uint64 Attempted = GCoverCounters.attempted();
	if (Attempted == 0)
	{
		// THE WORDING MATTERS. Zeroes are what a broken instrument and an empty
		// world have in common, and that ambiguity has cost this project three
		// separate diagnoses. Say which one this is.
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelCover: DID NOT RUN -- no chunk was attempted. "
		            "voxel.Cover.Produce is %d (needs 1), or no detail group has "
		            "resolved yet. This is not a result about the ground."),
		       GVoxelCoverProduce);
		PrintCoverPublication();
		PrintCoverIndexFunnel();
		return;
	}
	const uint64 Packed = GCoverCounters.packed();
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelCover: attempted %llu -> resolved %llu -> PACKED %llu; "
	            "%llu instances anchored, %llu solid cover voxels, %.2f MiB packed "
	            "(discarded -- no store yet). Refused sentinel-world groups %llu, "
	            "z-clamped groups %llu."),
	       (unsigned long long)Attempted,
	       (unsigned long long)GCoverCounters.resolved(),
	       (unsigned long long)Packed,
	       (unsigned long long)GCoverCounters.anchored(),
	       (unsigned long long)GCoverCounters.solid(),
	       double(GCoverCounters.bytes()) / (1024.0 * 1024.0),
	       (unsigned long long)GCoverGroupsRefusedSentinel.load(std::memory_order_relaxed),
	       (unsigned long long)GCoverGroupsZClamped.load(std::memory_order_relaxed));
	if (Packed == 0)
	{
		// The other half of the funnel, and it is a REAL answer: measured at the
		// alpine census site, only three species place there and none of them is
		// ground cover.
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelCover: RAN AND PRODUCED NOTHING -- %llu chunks attempted, none "
		            "held cover. That is a statement about this ground, not about the "
		            "producer."),
		       (unsigned long long)Attempted);
	}
	PrintCoverPublication();
	PrintCoverIndexFunnel();
}

FAutoConsoleCommand GCoverStatsCmd(
	TEXT("voxel.Cover.Stats"),
	TEXT("Cover-volume producer funnel: attempted -> resolved -> packed, and which of "
	     "'did not run' / 'ran and found nothing' applies."),
	FConsoleCommandDelegate::CreateStatic(&PrintCoverStats));

constexpr int32 kGroupEdgeChunks = 2;
constexpr int32 kGroupEdgeVoxels = kGroupEdgeChunks * VoxelCoords::ChunkEdgeVoxels; // 64
constexpr double kGroupEdgeUU = double(kGroupEdgeVoxels) * VoxelCoords::VoxelSizeUU; // 640

// 112 -> 256 m, 2026-08-18 (owner: "extend the detail ring"). The old value
// sat inside R0's 128 m level-0 ring so every group stood on already-admitted
// ground; past it, groups can land on ground the streamer has not admitted
// yet -- which is SAFE here and always was, because dispatch is gated on
// IsFootprintResident (dilated by the full layer reach) and a group outside
// residency simply defers to a later tick instead of dispatching. The gate is
// the invariant; 128 m was belt-and-braces.
//
// Cost: instance count scales with area, 5.2x at this radius -- but the same
// day cut L3 density 1000 -> 300 per-mille (the owner walked into ground cover
// that blocked movement), so the net is ~1.6x the old resident count for 2.3x
// the visible radius. That pairing is the point: THINNER, but visible much
// further, which is what "the slopes look bare from the vista" actually was --
// 94% of a hillside's vegetation is detail-lattice and used to vanish at 112 m.
constexpr double kDefaultRingMeters = 256.0;

// Hysteresis on release, same shape as UVoxelWorldSubsystem's
// UnloadRingMultiplier: a group loads inside RingMeters and unloads past
// RingMeters * this, so the boundary does not thrash under small anchor
// motion.
constexpr double kUnloadMultiplier = 1.15;

// Per-tick budgets, ADAPTIVE since 2026-08-18 (the ten-minute first-arrival
// debt: 5,028 groups / 311 first-encounter meshes at the alpine lake pushed
// capture settle from ~350 s to 591-656 s under the old fixed 4/8/4/4).
//
// The original hitch reasoning still stands and is preserved, just made
// proportional: mesh builds and instance-table rebuilds run on the game
// thread and are the ones that could hitch a frame, so they are governed by a
// measured TIME budget per tick rather than a count (grids span 100-5000
// voxels; four big builds and four small ones are very different frames).
// Resolve jobs run on background workers and are bounded by an in-flight
// count (worker-side cost -- they share the task pool with the level-0
// meshing jobs).
//
// Both budgets scale with the headroom the LAST frame had against
// VoxelDebug::kHitchThresholdMs (the same 33.3 ms bar the "Hitch frame" log
// fires on): a fast frame gets the max values, a frame at 85% of the
// threshold gets the min values, linear in between. Convergence therefore
// runs hard exactly when the frame can afford it and backs off to roughly
// the old fixed budgets when it cannot -- it never ADDS a hitch to a frame
// already near the bar.
constexpr int32 kMinJobsInFlight = 4;        // the old fixed cap; floor under load
constexpr int32 kMaxJobsInFlight = 24;       // ceiling with full frame headroom
constexpr int32 kMaxDispatchPerTick = 32;    // refill limit per tick
// Game-thread time budget shared by mesh builds and HISM rebuilds each tick.
// 6 ms fits inside a 16.6 ms frame's idle time (the game thread idles ~75% at
// 2K -- voxelsim-draw-path-2k); 1.5 ms is roughly what the old 4-mesh budget
// spent on typical grids, so the busy floor is the old behaviour.
constexpr double kMinBuildBudgetMs = 1.5;
constexpr double kMaxBuildBudgetMs = 6.0;
// Hard ceilings under the time budget so a pathological run of tiny grids
// cannot spin the loops unboundedly inside one tick.
constexpr int32 kMaxMeshBuildsPerTick = 64;
constexpr int32 kMaxRebuildsPerTick = 64;
// Headroom window, as fractions of the hitch threshold: at or below 60%
// (20 ms) the frame is healthy and budgets are maxed; at or above 85%
// (28.3 ms) budgets are floored.
constexpr double kHealthyFrameFrac = 0.60;
constexpr double kBusyFrameFrac = 0.85;

// Refuse to dense-decode an absurd grid (nothing baked approaches this; the
// detail library is 100-5000 voxels per grid).
constexpr int64 kMaxGridCells = 8 * 1024 * 1024;

// (bankId << 16) | seedIndex -- the identity of one baked grid, and therefore
// of one static mesh and one HISM batch.
FORCEINLINE uint32 MakeMeshKey(uint16 BankId, uint16 SeedIndex)
{
	return (uint32(BankId) << 16) | uint32(SeedIndex);
}

struct FGroupKey
{
	int64 X = 0;
	int64 Y = 0;
	friend bool operator==(const FGroupKey&, const FGroupKey&) = default;
	friend uint32 GetTypeHash(const FGroupKey& K)
	{
		return HashCombine(::GetTypeHash(K.X), ::GetTypeHash(K.Y));
	}
};

// One resolved detail instance, in world space. Doubles deliberately: at
// -400 km from the origin a float has ~3 cm of resolution, and the HISM
// instance buffer's float precision is recovered by giving each component a
// nearby world origin and storing instances relative to it (see
// FMeshEntry::OriginUU).
struct FDetailInstanceRec
{
	uint32 MeshKey = 0;
	FVector3d PosUU = FVector3d::ZeroVector;
	uint8 YawQuarter = 0;
};

// Worker-built geometry for one (species, seed): parallel per-vertex arrays +
// an index list, converted to a MeshDescription on the game thread. Built off
// the game thread because the face walk over a few thousand voxels is the
// expensive half; the MeshDescription fill is a linear copy.
struct FMeshGeometry
{
	uint32 MeshKey = 0;
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector3f> TangentsX;
	TArray<FVector4f> Colors; // LINEAR floats -- see the colour note up top
	TArray<FVector2f> UVs;
	TArray<uint32> Indices;
	uint64 SolidVoxels = 0;
};

// One cover chunk on its way from a worker to the pool.
//
// The key is in COVER CHUNK coordinates (32 cover cells of 50 mm = 1.6 m each),
// and it goes into the pool at FVoxelBrickPool::kCoverLevel. That level is the
// only thing that tells the pool, the index and the marcher that this chunk's
// cells are 50 mm rather than 100 mm.
struct FCoverChunkPublish
{
	FIntVector CoverChunkCoord = FIntVector::ZeroValue;
	FVoxelBrickCpuPackRef Pack;
};

struct FGroupResult
{
	FGroupKey Group;
	TArray<FDetailInstanceRec> Instances;
	TArray<TUniquePtr<FMeshGeometry>> NewGeometry;
	int32 SitesTotal = 0;      // everything instancesForRect resolved
	int32 DetailKept = 0;      // detail-lattice instances anchored in this group
	int32 BankMisses = 0;      // detail instances whose grid the library refused
	int32 CoverChunksPacked = 0;   // voxel.Cover.Produce arm; 0 when it is off
	// THE PACKS THEMSELVES, carried back to the game thread rather than published
	// from the worker: FVoxelBrickPool::AddChunkFromCpu is GAME THREAD ONLY (its
	// header says so, and it mutates the suballocator and the pending-write
	// lists). Empty unless voxel.Cover.Resident is on, and empty for every chunk
	// the producer answered anyCover=false for -- requirement C1's "store
	// nothing" is enforced by never creating the carrier at all.
	TArray<FCoverChunkPublish> CoverPacks;
};

// Everything a resolve job needs, captured by value at dispatch. The three
// borrowed pointers outlive every job -- Deinitialize() waits on all tasks
// and runs before UVoxelWorldSubsystem's own teardown (InitializeDependency).
struct FResolveJobInput
{
	const vxc::AssetField* Field = nullptr;
	const vxc::Amplifier* Amp = nullptr;
	const vxc::IAssetBankSource* Banks = nullptr;
	// The engine's ONE channel binding (UVoxelWorldSubsystem::
	// GetAssetChannelSource) -- ground cover must gate on the same water
	// distance / standing water / treeline the tree meshers gate on, or reeds
	// resolve where no lake is and grass resolves under it. Null (no fine
	// tier) means sentinel channels: fail-closed, the pre-channel world.
	// Thread-safe from workers; internally serialized.
	vxc::IAssetChannelSource* Channels = nullptr;
	FGroupKey Group;
	vxc::AssetVoxelRect Rect;
	// LATCHED AT DISPATCH, never read from the cvar on the worker: the same
	// reason VoxelGpuMeshJobManager latches voxel.GPU.BrickPack at Submit --
	// a value that changes under a job in flight makes two jobs of one leg
	// disagree about what was measured.
	bool bProduceCover = false;
	// LATCHED TOO, and SEPARATE from bProduceCover on purpose. Produce alone is
	// the pack-and-discard measurement arm that has existed since 2026-08-20;
	// Resident is what makes the pack reach the pool. The same two-cvar shape as
	// voxel.GPU.BrickPack / voxel.GPU.BrickPackResident, so "the producer is off"
	// and "the producer ran and nothing was stored" stay distinguishable.
	bool bPublishCover = false;
	// MeshKeys whose geometry the game thread already has (or has in flight
	// from an earlier job). A stale snapshot only means a duplicate geometry
	// build, which the drain discards -- never a missing one: the first job
	// to see a key always carries its geometry.
	TSet<uint32> KnownGeometry;
};

// sRGB palette -> linear, once, both faces classes and all materials. The
// bank loader guarantees every material id in a served grid is
// < vxc::kMaterialCount (materialsWithinEngine is a load-time refusal), so
// indexing this table with a served grid's bytes cannot go out of range.
struct FPaletteLinear
{
	FLinearColor Face[vxc::kMaterialCount][vxc::kFaceClassCount];
	uint8 Jitter[vxc::kMaterialCount];
	FPaletteLinear()
	{
		for (uint32 M = 0; M < uint32(vxc::kMaterialCount); ++M)
		{
			const vxc::MaterialAppearance& A = vxc::kMaterialPalette[M];
			for (uint32 F = 0; F < uint32(vxc::kFaceClassCount); ++F)
			{
				// FLinearColor(FColor) is the exact sRGB decode.
				Face[M][F] = FLinearColor(FColor(A.face[F].r, A.face[F].g, A.face[F].b));
			}
			Jitter[M] = A.voxelJitter;
		}
	}
};

const FPaletteLinear& PaletteLinear()
{
	static const FPaletteLinear P; // thread-safe magic-static init
	return P;
}

// Naive visible-face quads over one baked grid. NOT the greedy mesher, on
// purpose: vxc::meshBrick is shaped around 8^3 bricks with a sampler apron,
// and -- decisive -- a greedy quad must be attribute-uniform, while the look
// doctrine (materialpalette.h) wants per-voxel colour jitter, which makes
// almost every merge illegal anyway. At 100-5000 voxels per grid the naive
// walk emits a few thousand faces and is nowhere near any budget.
//
// Winding and corner order copied from FVoxelChunkSceneProxy's proven
// convention (VoxelChunkComponent.cpp, the 2026-07-21 winding fix): corners
// (u0,v0)->(u0,v1)->(u1,v1)->(u1,v0) with U=(Axis+1)%3, V=(Axis+2)%3;
// positive faces index (0,1,2)(0,2,3), negative faces (0,2,1)(0,3,2).
void BuildNaiveFaceGeometry(const vxc::AssetGrid& Grid, uint32 MeshKey, FMeshGeometry& Out)
{
	const int32 SX = Grid.sizeX(), SY = Grid.sizeY(), SZ = Grid.sizeZ();
	const int64 Cells = int64(SX) * int64(SY) * int64(SZ);
	if (Cells <= 0 || Cells > kMaxGridCells)
	{
		return;
	}

	// Dense material grid, z fastest (matches columnRuns' delivery order).
	TArray<uint8> Mat;
	Mat.SetNumZeroed(int32(Cells));
	uint64 Solid = 0;
	for (int32 X = 0; X < SX; ++X)
	{
		for (int32 Y = 0; Y < SY; ++Y)
		{
			const int64 Base = (int64(X) * SY + Y) * SZ;
			Grid.columnRuns(X, Y,
			                [&](int32 Z0, int32 Len, vxc::MaterialId M)
			                {
				                if (M == vxc::MAT_AIR)
				                {
					                return;
				                }
				                Solid += uint64(Len);
				                for (int32 Z = Z0; Z < Z0 + Len; ++Z)
				                {
					                Mat[int32(Base + Z)] = uint8(M);
				                }
			                });
		}
	}
	Out.SolidVoxels = Solid;
	if (Solid == 0)
	{
		return;
	}

	auto MatAt = [&](int32 X, int32 Y, int32 Z) -> uint8
	{
		if (X < 0 || Y < 0 || Z < 0 || X >= SX || Y >= SY || Z >= SZ)
		{
			return 0;
		}
		return Mat[int32((int64(X) * SY + Y) * SZ + Z)];
	};

	// THE DETAIL GRID'S OWN PITCH, not the world's. assetgrid.h: "Read it
	// before placing anything ... at() deliberately does NOT scale by it."
	// This is the scale read; a 5 cm tuft renders at 5 cm.
	const float PitchUU = float(double(Grid.voxelSizeMm()) * 0.1);
	const int32 Origin[3] = {Grid.originX(), Grid.originY(), Grid.originZ()};

	const FPaletteLinear& Pal = PaletteLinear();
	const uint64 JitterSeed = (uint64(MeshKey) << 1) | 1u;

	static const FVector3f AxisDir[3] = {FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, 0, 1)};

	// Rough reserve: ~4.5 visible faces per solid voxel is a generous upper
	// bound for sparse foliage; TArray growth handles the rest.
	const int32 ReserveFaces = int32(FMath::Min<uint64>(Solid * 5u, 200000u));
	Out.Positions.Reserve(ReserveFaces * 4);
	Out.Normals.Reserve(ReserveFaces * 4);
	Out.TangentsX.Reserve(ReserveFaces * 4);
	Out.Colors.Reserve(ReserveFaces * 4);
	Out.UVs.Reserve(ReserveFaces * 4);
	Out.Indices.Reserve(ReserveFaces * 6);

	int32 Cell[3];
	for (Cell[0] = 0; Cell[0] < SX; ++Cell[0])
	{
		for (Cell[1] = 0; Cell[1] < SY; ++Cell[1])
		{
			for (Cell[2] = 0; Cell[2] < SZ; ++Cell[2])
			{
				const uint8 M = MatAt(Cell[0], Cell[1], Cell[2]);
				if (M == 0)
				{
					continue;
				}

				// Per-voxel lightness jitter, hashed from LOCAL coordinates +
				// the (species, seed) identity -- deterministic, and keyed to
				// the VOXEL so all six faces of one cube agree
				// (materialpalette.h's "keyed to the voxel and not the face").
				const double J =
					double(vxc::hashToSigned16(vxc::hash3(JitterSeed, Cell[0], Cell[1], Cell[2], 0))) /
					32768.0;
				const float Gain =
					1.0f + float(J) * 0.35f * (float(Pal.Jitter[M]) / 255.0f);

				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					for (int32 Positive = 0; Positive < 2; ++Positive)
					{
						int32 N[3] = {Cell[0], Cell[1], Cell[2]};
						N[Axis] += Positive ? 1 : -1;
						if (MatAt(N[0], N[1], N[2]) != 0)
						{
							continue; // face culled against a solid neighbour
						}

						const int32 U = (Axis + 1) % 3;
						const int32 V = (Axis + 2) % 3;
						const vxc::FaceClass FC =
							(Axis == 2) ? (Positive ? vxc::kFaceTop : vxc::kFaceBottom)
							            : vxc::kFaceSide;
						FLinearColor C = Pal.Face[M][FC] * Gain;
						C.R = FMath::Clamp(C.R, 0.0f, 1.0f);
						C.G = FMath::Clamp(C.G, 0.0f, 1.0f);
						C.B = FMath::Clamp(C.B, 0.0f, 1.0f);
						C.A = 1.0f;

						const float FaceCoord = float(Cell[Axis] + Positive);
						const float U0 = float(Cell[U]), U1 = float(Cell[U] + 1);
						const float V0 = float(Cell[V]), V1 = float(Cell[V] + 1);
						const float CornerU[4] = {U0, U0, U1, U1};
						const float CornerV[4] = {V0, V1, V1, V0};

						const FVector3f Normal = AxisDir[Axis] * (Positive ? 1.0f : -1.0f);
						const FVector3f TangentX = AxisDir[U];
						const uint32 Base = uint32(Out.Positions.Num());

						for (int32 Corner = 0; Corner < 4; ++Corner)
						{
							FVector3f P;
							P[Axis] = (FaceCoord + float(Origin[Axis])) * PitchUU;
							P[U] = (CornerU[Corner] + float(Origin[U])) * PitchUU;
							P[V] = (CornerV[Corner] + float(Origin[V])) * PitchUU;
							Out.Positions.Add(P);
							Out.Normals.Add(Normal);
							Out.TangentsX.Add(TangentX);
							Out.Colors.Add(FVector4f(C.R, C.G, C.B, 1.0f));
							// The material samples no texture; a stable planar
							// UV keeps every downstream assumption (non-zero
							// UV channel, finite derivatives) honest.
							Out.UVs.Add(FVector2f(CornerU[Corner], CornerV[Corner]) *
							            (PitchUU / 100.0f));
						}

						if (Positive)
						{
							Out.Indices.Append({Base + 0, Base + 1, Base + 2, Base + 0, Base + 2, Base + 3});
						}
						else
						{
							Out.Indices.Append({Base + 0, Base + 2, Base + 1, Base + 0, Base + 3, Base + 2});
						}
					}
				}
			}
		}
	}
}

// The worker body: resolve one group's sites (terrain AND detail -- the
// resolver has no detail-only mode, and terrain sites are the cheap 15%),
// keep the detail-lattice instances ANCHORED IN THIS GROUP (instancesForRect
// dilates by layer reach, so an instance can be returned by up to four
// neighbouring groups; anchor ownership is the dedup), and build geometry for
// any (species, seed) the game thread did not know at dispatch.
FGroupResult RunResolveJob(const FResolveJobInput& In)
{
	FGroupResult R;
	R.Group = In.Group;

	const std::vector<vxc::AssetInstance> Insts = In.Field->instancesForRect(
		In.Rect,
		[Amp = In.Amp, Ch = In.Channels](int64_t Vx, int64_t Vy)
		{
			// Channel-sourced facts, same binding as the terrain meshers; a
			// null source composes the sentinel (fail-closed) channels.
			return vxc::assetColumnFactsFromSample(
				Amp->column(Vx, Vy),
				Ch != nullptr ? Ch->channelsAt(Vx, Vy) : vxc::AssetColumnChannels{});
		},
		/*terrainOnly*/ false);
	R.SitesTotal = int32(Insts.size());

	const std::vector<vxc::AssetLayer>& Layers = In.Field->layers();
	TSet<uint32> BuiltHere;

	for (const vxc::AssetInstance& Inst : Insts)
	{
		if (size_t(Inst.layer) >= Layers.size() || Layers[Inst.layer].terrainLattice)
		{
			continue; // terrain-lattice: composed into world voxels already
		}
		const int64 AnchorVx = vxc::floorDiv(Inst.anchorXMm, int64(vxc::kVoxelSizeMm));
		const int64 AnchorVy = vxc::floorDiv(Inst.anchorYMm, int64(vxc::kVoxelSizeMm));
		if (AnchorVx < In.Rect.vx0 || AnchorVx > In.Rect.vx1 || AnchorVy < In.Rect.vy0 ||
		    AnchorVy > In.Rect.vy1)
		{
			continue; // owned by a neighbouring group
		}

		const vxc::AssetGrid* Grid = In.Banks->bankGrid(Inst.bankId, Inst.seedIndex);
		if (Grid == nullptr || !Grid->valid())
		{
			++R.BankMisses; // refused/missing bank: composes as nothing, counted
			continue;
		}
		if (Grid->onTerrainLattice())
		{
			continue; // category guard: a world-lattice grid is not ours to draw
		}
		if (Grid->hasParts())
		{
			continue; // animals are rig-parted and EXCLUDED (owner decision)
		}

		FDetailInstanceRec Rec;
		Rec.MeshKey = MakeMeshKey(Inst.bankId, Inst.seedIndex);
		// Anchor point (the jittered in-cell position), standing on the TOP
		// surface of the verified-solid anchor voxel. The terrain-lattice
		// convention (base slab shares the ground voxel) would bury a 5 cm
		// tuft entirely inside the 10 cm ground cube; cover sits ON the
		// ground it resolved against.
		Rec.PosUU.X = double(Inst.anchorXMm) * 0.1;
		Rec.PosUU.Y = double(Inst.anchorYMm) * 0.1;
		Rec.PosUU.Z = double(Inst.anchorVz + 1) * VoxelCoords::VoxelSizeUU;
		Rec.YawQuarter = Inst.yawQuarter & 3u;
		R.Instances.Add(Rec);
		++R.DetailKept;

		if (!In.KnownGeometry.Contains(Rec.MeshKey) && !BuiltHere.Contains(Rec.MeshKey))
		{
			TUniquePtr<FMeshGeometry> G = MakeUnique<FMeshGeometry>();
			G->MeshKey = Rec.MeshKey;
			BuildNaiveFaceGeometry(*Grid, Rec.MeshKey, *G);
			if (G->Indices.Num() > 0)
			{
				R.NewGeometry.Add(MoveTemp(G));
				BuiltHere.Add(Rec.MeshKey);
			}
		}
	}
	// --- the cover-volume arm, off unless voxel.Cover.Produce 1 -------------
	if (In.bProduceCover)
	{
		if (In.Channels == nullptr)
		{
			// The sentinel world is a different world; refuse rather than
			// produce a volume nobody can compare to anything.
			GCoverGroupsRefusedSentinel.fetch_add(1, std::memory_order_relaxed);
		}
		else
		{
			// ONE RESOLVE PER GROUP, not per chunk. resolveForCoverCompose is
			// the complement of resolveForCompose and reuses the instance list
			// the HISM path already paid for above.
			const std::vector<vxc::AssetField::ResolvedCoverInstance> Cover =
				In.Field->resolveForCoverCompose(Insts, kCoverPitchMm);
			if (!Cover.empty())
			{
				const int64 CellsPerVoxel = int64(vxc::kVoxelSizeMm) / int64(kCoverPitchMm);
				const int64 BaseCx = In.Rect.vx0 * CellsPerVoxel;
				const int64 BaseCy = In.Rect.vy0 * CellsPerVoxel;
				const int64 E = int64(vxc::kCoverChunkEdgeCells);

				// TNumericLimits, not INT64_MAX: this translation unit includes
				// no <cstdint> of its own and CoreMinimal's reach for the C
				// macros is a transitive accident, not a contract.
				int64 ZMin = TNumericLimits<int64>::Max();
				int64 ZMax = TNumericLimits<int64>::Lowest();
				for (const vxc::AssetField::ResolvedCoverInstance& CI : Cover)
				{
					const int64 Lo = CI.anchorCz + int64(CI.grid->originZ());
					const int64 Hi = Lo + int64(CI.grid->sizeZ()) - 1;
					ZMin = FMath::Min(ZMin, Lo);
					ZMax = FMath::Max(ZMax, Hi);
				}
				int64 Cz0 = vxc::floorDiv(ZMin, E);
				int64 Cz1 = vxc::floorDiv(ZMax, E);
				if (Cz1 - Cz0 + 1 > int64(kCoverMaxChunkLayers))
				{
					// Counted, never silent. See kCoverMaxChunkLayers.
					GCoverGroupsZClamped.fetch_add(1, std::memory_order_relaxed);
					Cz1 = Cz0 + int64(kCoverMaxChunkLayers) - 1;
				}

				// The group is kGroupEdgeVoxels level-0 voxels across, so this
				// many cover chunks per axis. Derived, not spelled: at 50 mm it
				// is 4, and it must move with the pitch rather than be a 4.
				const int64 ChunksPerAxis = (int64(kGroupEdgeVoxels) * CellsPerVoxel) / E;
				for (int64 Cz = Cz0; Cz <= Cz1; ++Cz)
				{
					for (int64 Jy = 0; Jy < ChunksPerAxis; ++Jy)
					{
						for (int64 Jx = 0; Jx < ChunksPerAxis; ++Jx)
						{
							const int64 Ccx = vxc::floorDiv(BaseCx, E) + Jx;
							const int64 Ccy = vxc::floorDiv(BaseCy, E) + Jy;
							const vxc::CoverChunkResult Packed = vxc::packCoverChunk(
								Cover, kCoverPitchMm, Ccx, Ccy, Cz, GCoverCounters);
							// anyCover FALSE MEANS STORE NOTHING -- requirement
							// C1, enforced by never building a carrier rather
							// than by building one and skipping it later. No
							// zeroed pack, no reserved slot, no dense entry.
							if (!Packed.anyCover)
							{
								continue;
							}
							++R.CoverChunksPacked;
							if (!In.bPublishCover)
							{
								continue;   // the pack-and-discard arm, unchanged
							}
							// THE ORIGIN IS IN COVER CELLS, not level-0 voxels.
							// The chunk record stores it and the marcher
							// validates the index against it as
							// (chunkCoord * 32) in the chunk's OWN units, so
							// passing level-0 voxels here would make every cover
							// lookup fail the record check and read as air --
							// silently, as missing cover.
							FCoverChunkPublish Pub;
							Pub.CoverChunkCoord =
								FIntVector(int32(Ccx), int32(Ccy), int32(Cz));
							Pub.Pack = VoxelBrickCpuPackFromCore(
								Packed.pack, Ccx * E, Ccy * E, Cz * E);
							R.CoverPacks.Add(MoveTemp(Pub));
						}
					}
				}
			}
		}
	}

	return R;
}

} // namespace

// ---------------------------------------------------------------------------
// The PImpl
// ---------------------------------------------------------------------------

struct FVoxelDetailAssetImpl
{
	// Config, resolved once in Initialize.
	double RingMeters = kDefaultRingMeters;
	bool bDisabled = false;
	bool bCastShadow = false;

	// Set true once the asset field was seen installed and the owner actor +
	// material exist. Until then Tick idles cheaply.
	bool bStarted = false;
	bool bMaterialFallbackWarned = false;

	// Cached once at start (immutable for the session): which layers are
	// detail, and the widest reach any layer dilates a rect by (residency
	// gate must cover the columns the resolve will actually touch).
	int64 MaxReachMm = 0;

	struct FGroupRecord
	{
		TArray<FDetailInstanceRec> Instances;
		// WHAT THIS GROUP PUT IN THE POOL, so releasing the group can take it
		// back out. Without this the cover volume grows monotonically as the
		// camera moves and the pool evicts cover under pressure instead of the
		// group owning its own lifetime -- and eviction is ranked by distance,
		// which would silently drop the cover the player is standing in when a
		// distant group happened to be added later.
		//
		// This is also the first caller FVoxelBrickPool::RemoveChunk has ever
		// had. Its header notes that GetEvictions() reads zero today ONLY
		// because nothing calls it, so the index's Removed path and the
		// marcher's stale-index record check are, from here on, live code rather
		// than code that has never run.
		TArray<FIntVector> CoverChunks;
	};

	struct FMeshEntry
	{
		UStaticMesh* Mesh = nullptr; // GC-rooted via the subsystem's BuiltMeshes
		UHierarchicalInstancedStaticMeshComponent* Hism = nullptr; // rooted via HismComponents
		// Component world origin: instances are stored in the HISM buffer
		// relative to this (float precision at planet coordinates -- see
		// FDetailInstanceRec). Snapped to the voxel grid purely for tidiness.
		FVector3d OriginUU = FVector3d::ZeroVector;
		// Full rebuild wanted (mesh newly built, or a contributing group was
		// released -- HISM instance removal reshuffles ids, so release is a
		// budgeted rebuild-from-truth rather than index bookkeeping).
		bool bDirty = false;
		uint64 SolidVoxels = 0;
	};

	// Game-thread state. Groups are the single source of truth for what is
	// resolved; KeyGroups is the reverse index a rebuild reads.
	TSet<FGroupKey> InFlight;
	TMap<FGroupKey, FGroupRecord> Groups;
	TMap<uint32, TSet<FGroupKey>> KeyGroups;
	TSet<uint32> GeometryKnown;                      // geometry seen (built or pending)
	TMap<uint32, TUniquePtr<FMeshGeometry>> PendingGeometry; // awaiting budgeted mesh build
	TMap<uint32, FMeshEntry> Meshes;

	// Worker plumbing.
	TQueue<TUniquePtr<FGroupResult>, EQueueMode::Mpsc> Results;
	TArray<UE::Tasks::TTask<void>> Tasks;

	// Telemetry.
	uint64 StatGroupsResolved = 0;
	uint64 StatInstancesLive = 0;
	uint64 StatMeshesBuilt = 0;
	uint64 StatBankMisses = 0;
	uint64 StatSitesResolved = 0;
	double LogTimer = 0.0;
	bool bFirstApplyLogged = false;

	// Convergence telemetry (2026-08-18). The convergence curve was previously
	// unmeasurable from the log -- the only signal was the capture harness's
	// settle time. These make it a first-class measurement:
	//  * StartTimeSeconds anchors time-to-first-full-ring;
	//  * bConvergedLogged latches the one CONVERGED line (first full ring:
	//    every group in the ring resolved, every encountered mesh built, no
	//    dirty HISM rebuilds outstanding);
	//  * the Window* pair measure mesh-build throughput per progress window;
	//  * StatPendingGroups is the dispatch step's last candidate count (groups
	//    inside the ring not yet resolved or in flight -- includes groups
	//    deferred by the residency gate).
	double StartTimeSeconds = 0.0;
	bool bConvergedLogged = false;
	double ProgressLogTimer = 0.0;
	uint64 WindowMeshBuilds = 0;
	double WindowBuildMs = 0.0;
	double StatBuildMsTotal = 0.0;
	int32 StatPendingGroups = 0;

	void WaitForAllJobs()
	{
		for (UE::Tasks::TTask<void>& T : Tasks)
		{
			T.Wait();
		}
		Tasks.Reset();
		// Drain and drop anything the jobs produced after the last tick.
		TUniquePtr<FGroupResult> R;
		while (Results.Dequeue(R))
		{
			R.Reset();
		}
	}
};

// ---------------------------------------------------------------------------
// Subsystem boilerplate
// ---------------------------------------------------------------------------

UVoxelDetailAssetSubsystem::UVoxelDetailAssetSubsystem() = default;
UVoxelDetailAssetSubsystem::~UVoxelDetailAssetSubsystem() = default;
UVoxelDetailAssetSubsystem::UVoxelDetailAssetSubsystem(FVTableHelper& Helper)
	: Super(Helper)
{
}

bool UVoxelDetailAssetSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Same scope as UVoxelWorldSubsystem: game worlds only.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE ||
	       WorldType == EWorldType::GamePreview;
}

TStatId UVoxelDetailAssetSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelDetailAssetSubsystem, STATGROUP_Tickables);
}

void UVoxelDetailAssetSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	// ORDERING, AND IT IS LOAD-BEARING: this makes UVoxelWorldSubsystem
	// initialize BEFORE this subsystem, and therefore deinitialize AFTER it --
	// so Deinitialize()'s WaitForAllJobs() always runs while the AssetField,
	// Amplifier and bank library the jobs borrow are still alive.
	Collection.InitializeDependency(UVoxelWorldSubsystem::StaticClass());
	Super::Initialize(Collection);

	Impl = MakeUnique<FVoxelDetailAssetImpl>();

	Impl->bDisabled = FParse::Param(FCommandLine::Get(), TEXT("VoxelNoDetailAssets"));
	Impl->bCastShadow = FParse::Param(FCommandLine::Get(), TEXT("VoxelDetailShadows"));
	double Ring = kDefaultRingMeters;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelDetailRingMeters="), Ring))
	{
		Impl->RingMeters = FMath::Clamp(Ring, 16.0, 512.0);
	}
}

void UVoxelDetailAssetSubsystem::Deinitialize()
{
	if (Impl)
	{
		Impl->WaitForAllJobs();
		Impl.Reset();
	}
	Super::Deinitialize();
}

// ---------------------------------------------------------------------------
// Game-thread helpers
// ---------------------------------------------------------------------------

namespace
{
// The engine-facing mesh from worker geometry. bFastBuild is the documented
// runtime path (BuildFromMeshDescriptions asserts it in non-editor builds and
// takes the direct render-data route in editor builds); no source model, no
// DDC, no collision.
UStaticMesh* CreateDetailStaticMesh(const FMeshGeometry& G, UMaterialInterface* Material)
{
	FMeshDescription MeshDesc;
	FStaticMeshAttributes Attributes(MeshDesc);
	Attributes.Register();

	const FPolygonGroupID PolyGroup = MeshDesc.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[PolyGroup] = FName(TEXT("Detail"));

	const int32 NumVerts = G.Positions.Num();
	const int32 NumTris = G.Indices.Num() / 3;
	MeshDesc.ReserveNewVertices(NumVerts);
	MeshDesc.ReserveNewVertexInstances(G.Indices.Num());
	MeshDesc.ReserveNewTriangles(NumTris);

	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> InstNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> InstTangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<FVector4f> InstColors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> InstUVs = Attributes.GetVertexInstanceUVs();

	TArray<FVertexID> VertexIds;
	VertexIds.Reserve(NumVerts);
	for (int32 I = 0; I < NumVerts; ++I)
	{
		const FVertexID V = MeshDesc.CreateVertex();
		Positions[V] = G.Positions[I];
		VertexIds.Add(V);
	}

	for (int32 T = 0; T < NumTris; ++T)
	{
		FVertexInstanceID Corner[3];
		for (int32 C = 0; C < 3; ++C)
		{
			const uint32 SrcVert = G.Indices[T * 3 + C];
			const FVertexInstanceID VI = MeshDesc.CreateVertexInstance(VertexIds[int32(SrcVert)]);
			InstNormals[VI] = G.Normals[int32(SrcVert)];
			InstTangents[VI] = G.TangentsX[int32(SrcVert)];
			InstColors[VI] = G.Colors[int32(SrcVert)];
			InstUVs[VI] = G.UVs[int32(SrcVert)];
			Corner[C] = VI;
		}
		MeshDesc.CreateTriangle(PolyGroup, Corner);
	}

	UStaticMesh* Mesh = NewObject<UStaticMesh>(
		GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(),
		                     *FString::Printf(TEXT("VoxelDetail_%08x"), G.MeshKey)),
		RF_Transient);
	Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, FName(TEXT("Detail"))));

	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bFastBuild = true;
	Params.bCommitMeshDescription = false;
	Params.bMarkPackageDirty = false;
	Params.bBuildSimpleCollision = false;
	Params.bAllowCpuAccess = false;

	TArray<const FMeshDescription*> Descs;
	Descs.Add(&MeshDesc);
	if (!Mesh->BuildFromMeshDescriptions(Descs, Params))
	{
		return nullptr;
	}
	return Mesh;
}

FTransform DetailInstanceTransform(const FDetailInstanceRec& Rec, const FVector3d& ComponentOrigin)
{
	// Quarter turns about +Z through the anchor point. UE's positive yaw maps
	// +X toward +Y, which is exactly assetgrid.cpp's yaw-1 forward map
	// ((u,v) -> (-v,u)), so yawQuarter * 90 degrees reproduces the baked
	// rotation convention.
	return FTransform(FRotator(0.0, 90.0 * double(Rec.YawQuarter), 0.0),
	                  FVector(Rec.PosUU - ComponentOrigin));
}
} // namespace

// ---------------------------------------------------------------------------
// The tick pipeline
// ---------------------------------------------------------------------------

void UVoxelDetailAssetSubsystem::Tick(float DeltaTime)
{
	if (!Impl || Impl->bDisabled)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	UVoxelWorldSubsystem* VoxelWorld = World->GetSubsystem<UVoxelWorldSubsystem>();
	if (VoxelWorld == nullptr)
	{
		return;
	}

	// All three are null until -VoxelAssetDir installed a field (or forever,
	// on a run without assets) -- in which case there is, correctly, nothing
	// to render and this subsystem stays inert.
	const vxc::AssetField* Field = VoxelWorld->GetAssetField();
	const vxc::Amplifier* Amp = VoxelWorld->GetWorldgenAmplifier();
	const vxc::IAssetBankSource* Banks = VoxelWorld->GetAssetBankSource();
	if (Field == nullptr || Amp == nullptr || Banks == nullptr)
	{
		return;
	}

	FVoxelDetailAssetImpl& S = *Impl;

	// --- one-time start ----------------------------------------------------
	if (!S.bStarted)
	{
		bool bAnyDetailLayer = false;
		for (const vxc::AssetLayer& L : Field->layers())
		{
			if (L.cellMm > 0 && !L.terrainLattice)
			{
				bAnyDetailLayer = true;
			}
			if (L.cellMm > 0 && L.maxRadiusMm > S.MaxReachMm)
			{
				S.MaxReachMm = L.maxRadiusMm;
			}
		}
		if (!bAnyDetailLayer)
		{
			UE_LOG(LogVoxelEarth, Warning,
			       TEXT("VoxelDetailAssets: asset field installed but carries no detail layer -- "
			            "nothing to render; subsystem stays inert."));
			S.bDisabled = true;
			return;
		}

		if (!FParse::Param(FCommandLine::Get(), TEXT("VoxelDefaultMaterial")))
		{
			DetailMaterial = Cast<UMaterialInterface>(StaticLoadObject(
				UMaterialInterface::StaticClass(), nullptr,
				TEXT("/Game/Voxel/M_VoxelDetailAsset.M_VoxelDetailAsset")));
		}
		if (DetailMaterial == nullptr)
		{
			DetailMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
			if (!S.bMaterialFallbackWarned)
			{
				S.bMaterialFallbackWarned = true;
				UE_LOG(LogVoxelEarth, Warning,
				       TEXT("VoxelDetailAssets: M_VoxelDetailAsset not found at "
				            "/Game/Voxel/M_VoxelDetailAsset -- ground cover renders with the engine "
				            "default material (grey, correct shapes). Author it once with "
				            "Tools/create_detail_asset_material.py."));
			}
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transient;
		SpawnParams.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		DetailOwner = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector,
		                                        FRotator::ZeroRotator, SpawnParams);
		if (DetailOwner == nullptr)
		{
			UE_LOG(LogVoxelEarth, Error,
			       TEXT("VoxelDetailAssets: could not spawn the detail owner actor -- detail "
			            "rendering disabled this run."));
			S.bDisabled = true;
			return;
		}
		DetailRoot = NewObject<USceneComponent>(DetailOwner, TEXT("VoxelDetailRoot"));
		DetailOwner->SetRootComponent(DetailRoot);
		DetailRoot->RegisterComponent();
		VoxelEofLedger::Count(VoxelEofLedger::ESource::Detail);
		VoxelEofLedger::CountRegister();
#if WITH_EDITOR
		DetailOwner->SetActorLabel(TEXT("VoxelDetailAssetOwner"));
#endif

		S.bStarted = true;
		S.StartTimeSeconds = FPlatformTime::Seconds();
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelDetailAssets: STARTED -- ring %.0f m, group %.1f m, reach dilation "
		            "%lld mm, shadows %s. Rendering detail-lattice plants/rocks only; detail "
		            "ENTITIES (animals) are excluded by design."),
		       S.RingMeters, kGroupEdgeUU / 100.0, (long long)S.MaxReachMm,
		       S.bCastShadow ? TEXT("on") : TEXT("off"));
	}

	// --- anchor (same rule as UVoxelWorldSubsystem::Tick) -------------------
	FVector Anchor = FVector::ZeroVector;
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (APawn* Pawn = PC->GetPawn())
		{
			Anchor = Pawn->GetActorLocation();
		}
	}

	const double RingUU = S.RingMeters * 100.0;
	const double UnloadUU = RingUU * kUnloadMultiplier;

	// --- the cover band centre ---------------------------------------------
	//
	// The march chunk index admits cover only within kCoverBandRadiusChunks of
	// this point, which is what bounds the simultaneous cover span to 80 chunks
	// on every axis and makes the toroidal wrap provably alias-free -- the same
	// 80 < 128 argument that covers ring 0, reached by bounding what ENTERS
	// rather than by hoping.
	//
	// FROM THE SAME ANCHOR THE RING USES, deliberately. Residency (the pool's
	// eviction focus), publication (the ring test below) and indexing (this) must
	// agree about where the camera is, or a chunk can be published, resident, and
	// refused by the index -- which reads as missing cover with a healthy pool.
	{
		// UU -> level-0 voxels -> cover cells -> cover chunks. Each step is an
		// exact integer floor; FloorToInt is the right rounding for negative
		// coordinates and a divide is not.
		const int64 Vx = int64(FMath::FloorToDouble(Anchor.X / VoxelCoords::VoxelSizeUU));
		const int64 Vy = int64(FMath::FloorToDouble(Anchor.Y / VoxelCoords::VoxelSizeUU));
		const int64 Vz = int64(FMath::FloorToDouble(Anchor.Z / VoxelCoords::VoxelSizeUU));
		const int64 E = int64(vxc::kCoverChunkEdgeCells);
		const int64 Cpv = FVoxelBrickPool::kCoverCellsPerVoxel0;
		GetGlobalVoxelMarchChunkIndex().SetCoverBandCentreChunk(
			FIntVector(int32(vxc::floorDiv(Vx * Cpv, E)), int32(vxc::floorDiv(Vy * Cpv, E)),
			           int32(vxc::floorDiv(Vz * Cpv, E))));

		// THE MUTATION ARM IS ARMED HERE, IN THE TICK, AND NOT AT STATS TIME.
		//
		// It was armed inside PrintCoverIndexFunnel, which only runs when
		// voxel.Cover.Stats is typed -- so the order in a real leg was: set the
		// cvar, publish every cover chunk with the flag still FALSE, then type
		// Stats and arm a mutation with nothing left to bite. The law would have
		// read CONSERVED on a leg whose whole purpose was to make it read
		// VIOLATED, and a CONSERVED reading there is indistinguishable from a
		// working check -- which would have certified the law using a run that
		// never tested it.
		//
		// This tick sets the band centre BEFORE the result drain publishes
		// anything (section 2 below), so the flag is live for the first cover
		// chunk the index is ever offered -- which is precisely the entry
		// AdmitToSlot refuses. Same call, moved to where it can still act.
		GetGlobalVoxelMarchChunkIndex().SetMutateCoverConservation(GVoxelCoverMutateIndex != 0);
	}

	// --- adaptive budgets (see the constants' comment) -----------------------
	// DeltaTime is the LAST frame's duration -- the standard one-frame-lagged
	// headroom proxy, and the same quantity the "Hitch frame" log judges.
	const double FrameMs = double(DeltaTime) * 1000.0;
	const double HealthyMs = double(VoxelDebug::kHitchThresholdMs) * kHealthyFrameFrac;
	const double BusyMs = double(VoxelDebug::kHitchThresholdMs) * kBusyFrameFrac;
	const double Headroom =
		FMath::Clamp((BusyMs - FrameMs) / (BusyMs - HealthyMs), 0.0, 1.0);
	const int32 JobsInFlightCap =
		kMinJobsInFlight +
		FMath::RoundToInt32(Headroom * double(kMaxJobsInFlight - kMinJobsInFlight));
	const double BuildBudgetMs =
		kMinBuildBudgetMs + Headroom * (kMaxBuildBudgetMs - kMinBuildBudgetMs);
	// Shared by the mesh-build and rebuild steps below; each step is
	// guaranteed one unit of progress per tick so a long stretch of busy
	// frames stalls convergence to the old fixed-budget rate, never to zero.
	double BudgetSpentMs = 0.0;

	// --- 1. drain worker results -------------------------------------------
	{
		TUniquePtr<FGroupResult> R;
		while (S.Results.Dequeue(R))
		{
			S.InFlight.Remove(R->Group);
			S.StatSitesResolved += uint64(R->SitesTotal);
			S.StatBankMisses += uint64(R->BankMisses);

			// New geometry first (even if the group itself is dropped below --
			// the mesh is position-independent and the next group will want it).
			for (TUniquePtr<FMeshGeometry>& G : R->NewGeometry)
			{
				if (!S.GeometryKnown.Contains(G->MeshKey))
				{
					S.GeometryKnown.Add(G->MeshKey);
					S.PendingGeometry.Add(G->MeshKey, MoveTemp(G));
				}
				// else: duplicate from a concurrent job -- dropped.
			}

			// A group that left the ring while its job ran: drop the
			// instances; it will re-resolve (identically -- determinism) if
			// the anchor comes back.
			const FVector3d Centre((double(R->Group.X) + 0.5) * kGroupEdgeUU,
			                       (double(R->Group.Y) + 0.5) * kGroupEdgeUU, 0.0);
			const double DistSq = FMath::Square(Centre.X - Anchor.X) + FMath::Square(Centre.Y - Anchor.Y);
			if (DistSq > FMath::Square(UnloadUU))
			{
				continue;
			}

			FVoxelDetailAssetImpl::FGroupRecord& Rec = S.Groups.Add(R->Group);
			Rec.Instances = MoveTemp(R->Instances);

			// ---- publish this group's cover into the brick pool -------------
			//
			// GAME THREAD, AFTER THE RING CHECK ABOVE. A group that left the ring
			// while its job ran has already `continue`d, so its packs are dropped
			// with the result and never reach the pool -- which is what keeps the
			// resident cover set inside the reach the index's aliasing proof is
			// stated against.
			//
			// NOTHING IS DISPATCHED HERE. FVoxelBrickPool::Flush batches the
			// writes and is driven once a tick from UVoxelWorldSubsystem, and the
			// index sink fires at the END of that flush -- so the GPU never sees
			// an index entry for a slot the pool has not written yet. That
			// ordering is the pool's published seam, not a coincidence of this
			// call site.
			for (FCoverChunkPublish& Pub : R->CoverPacks)
			{
				FVoxelBrickChunkKey Key;
				Key.X = Pub.CoverChunkCoord.X;
				Key.Y = Pub.CoverChunkCoord.Y;
				Key.Z = Pub.CoverChunkCoord.Z;
				Key.Level = FVoxelBrickPool::kCoverLevel;
				const int32 Slot = GetGlobalVoxelBrickPool().AddChunkFromCpu(
					Pub.Pack, Key,
					// NEUTRAL, and stated rather than defaulted: cover is 50 mm
					// vegetation at the cover level. It has no biome tint and the
					// surface-proximity gate is meaningless for it.
					FVoxelBrickChunkShading::Neutral());
				if (Slot == INDEX_NONE)
				{
					// REFUSED, AND COUNTED. The pool is full and could not evict
					// enough; GetAllocFailures() has moved. Recording the key
					// anyway would make the release path call RemoveChunk on a
					// chunk that was never added.
					GCoverPublishRefused.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				Rec.CoverChunks.Add(Pub.CoverChunkCoord);
				GCoverChunksResident.fetch_add(1, std::memory_order_relaxed);
			}
			++S.StatGroupsResolved;
			S.StatInstancesLive += uint64(Rec.Instances.Num());

			// Register + append to already-built components.
			TMap<uint32, TArray<FTransform>> Appends;
			for (const FDetailInstanceRec& Inst : Rec.Instances)
			{
				S.KeyGroups.FindOrAdd(Inst.MeshKey).Add(R->Group);
				if (FVoxelDetailAssetImpl::FMeshEntry* Entry = S.Meshes.Find(Inst.MeshKey);
				    Entry != nullptr && Entry->Hism != nullptr && !Entry->bDirty)
				{
					Appends.FindOrAdd(Inst.MeshKey)
						.Add(DetailInstanceTransform(Inst, Entry->OriginUU));
				}
			}
			for (TPair<uint32, TArray<FTransform>>& A : Appends)
			{
				S.Meshes[A.Key].Hism->AddInstances(A.Value, /*bShouldReturnIndices*/ false,
				                                   /*bWorldSpace*/ false);
				// One count per COMPONENT dirtied, not per instance: AddInstances
				// marks the HISM once for the whole array.
				VoxelEofLedger::Count(VoxelEofLedger::ESource::Detail);
			}

			if (!S.bFirstApplyLogged && Rec.Instances.Num() > 0)
			{
				S.bFirstApplyLogged = true;
				UE_LOG(LogVoxelEarth, Log,
				       TEXT("VoxelDetailAssets: first group applied -- %d detail instances of "
				            "%d resolved sites at group (%lld, %lld)."),
				       Rec.Instances.Num(), R->SitesTotal, (long long)R->Group.X,
				       (long long)R->Group.Y);
			}
		}
	}

	// --- 2. budgeted mesh builds -------------------------------------------
	{
		int32 Built = 0;
		for (auto It = S.PendingGeometry.CreateIterator();
		     It && Built < kMaxMeshBuildsPerTick &&
		     (Built == 0 || BudgetSpentMs < BuildBudgetMs);
		     ++It)
		{
			const uint32 Key = It->Key;
			TUniquePtr<FMeshGeometry> Geometry = MoveTemp(It->Value);
			It.RemoveCurrent();
			++Built;
			const double BuildStart = FPlatformTime::Seconds();

			UStaticMesh* Mesh = CreateDetailStaticMesh(*Geometry, DetailMaterial);
			if (Mesh == nullptr)
			{
				UE_LOG(LogVoxelEarth, Warning,
				       TEXT("VoxelDetailAssets: mesh build FAILED for key %08x (%d verts) -- "
				            "instances of this (species, seed) will not render this session."),
				       Key, Geometry->Positions.Num());
				BudgetSpentMs += (FPlatformTime::Seconds() - BuildStart) * 1000.0;
				continue;
			}
			BuiltMeshes.Add(Mesh);

			UHierarchicalInstancedStaticMeshComponent* Hism =
				NewObject<UHierarchicalInstancedStaticMeshComponent>(DetailOwner);
			Hism->SetStaticMesh(Mesh);
			Hism->SetMobility(EComponentMobility::Movable);
			Hism->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Hism->SetCanEverAffectNavigation(false);
			Hism->SetCastShadow(Impl->bCastShadow);
			// Per-instance distance cull inside the ring: instances vanish at
			// the ring edge (10 cm cover at 112 m is subpixel; the pop is
			// invisible) rather than living until the 1.15x release boundary.
			Hism->SetCullDistances(int32(RingUU * 0.85), int32(RingUU));
			Hism->SetupAttachment(DetailRoot);
			// Component origin near the anchor: instance transforms stay small
			// (<= ring radius) so the instance buffer's float precision is
			// spent on centimetres, not on the 400 km to the world origin.
			const FVector3d Origin(FMath::GridSnap(Anchor.X, VoxelCoords::VoxelSizeUU),
			                       FMath::GridSnap(Anchor.Y, VoxelCoords::VoxelSizeUU), 0.0);
			Hism->SetWorldLocation(FVector(Origin));
			Hism->RegisterComponent();
			VoxelEofLedger::Count(VoxelEofLedger::ESource::Detail);
			VoxelEofLedger::CountRegister();
			HismComponents.Add(Hism);

			FVoxelDetailAssetImpl::FMeshEntry& Entry = S.Meshes.Add(Key);
			Entry.Mesh = Mesh;
			Entry.Hism = Hism;
			Entry.OriginUU = Origin;
			Entry.SolidVoxels = Geometry->SolidVoxels;
			Entry.bDirty = true; // full rebuild picks up every already-applied group
			++S.StatMeshesBuilt;

			const double BuildMs = (FPlatformTime::Seconds() - BuildStart) * 1000.0;
			BudgetSpentMs += BuildMs;
			S.WindowBuildMs += BuildMs;
			S.StatBuildMsTotal += BuildMs;
			++S.WindowMeshBuilds;
		}
	}

	// --- 3. release groups past the unload ring ----------------------------
	{
		TArray<FGroupKey> ToRemove;
		for (const TPair<FGroupKey, FVoxelDetailAssetImpl::FGroupRecord>& G : S.Groups)
		{
			const double CX = (double(G.Key.X) + 0.5) * kGroupEdgeUU;
			const double CY = (double(G.Key.Y) + 0.5) * kGroupEdgeUU;
			const double DistSq = FMath::Square(CX - Anchor.X) + FMath::Square(CY - Anchor.Y);
			if (DistSq > FMath::Square(UnloadUU))
			{
				ToRemove.Add(G.Key);
			}
		}
		for (const FGroupKey& G : ToRemove)
		{
			FVoxelDetailAssetImpl::FGroupRecord Rec;
			S.Groups.RemoveAndCopyValue(G, Rec);
			S.StatInstancesLive -= uint64(Rec.Instances.Num());

			// The cover this group put in the pool comes back out with it. The
			// record's own clear is what makes the slot read "nothing here" the
			// instant it lands, and the index's Removed-before-Added rule is what
			// keeps a slot retired and re-used in one flush from being resurrected
			// under the old key.
			for (const FIntVector& C : Rec.CoverChunks)
			{
				FVoxelBrickChunkKey Key;
				Key.X = C.X;
				Key.Y = C.Y;
				Key.Z = C.Z;
				Key.Level = FVoxelBrickPool::kCoverLevel;
				if (GetGlobalVoxelBrickPool().RemoveChunk(Key))
				{
					GCoverChunksResident.fetch_sub(1, std::memory_order_relaxed);
					GCoverChunksReleased.fetch_add(1, std::memory_order_relaxed);
				}
				else
				{
					// The pool did not have it. That is NOT benign: it means the
					// pool evicted a cover chunk behind this subsystem's back, so
					// the group's list and residency have diverged. Counted
					// rather than ignored, because the visible symptom would be
					// cover missing from ground the player is standing on with
					// every other counter reading healthy.
					GCoverReleaseMissing.fetch_add(1, std::memory_order_relaxed);
				}
			}
			for (const FDetailInstanceRec& Inst : Rec.Instances)
			{
				if (TSet<FGroupKey>* Members = S.KeyGroups.Find(Inst.MeshKey))
				{
					Members->Remove(G);
				}
				if (FVoxelDetailAssetImpl::FMeshEntry* Entry = S.Meshes.Find(Inst.MeshKey))
				{
					// HISM instance removal reshuffles ids; rebuilding the
					// component from the surviving groups (budgeted, below) is
					// the correctness-by-construction path.
					Entry->bDirty = true;
				}
			}
		}
	}

	// --- 4. budgeted full rebuilds of dirty components ----------------------
	{
		int32 Rebuilt = 0;
		for (TPair<uint32, FVoxelDetailAssetImpl::FMeshEntry>& Pair : S.Meshes)
		{
			// Shares the tick's time budget with the mesh builds above (and the
			// same guaranteed-progress floor: at least one rebuild per tick).
			if (Rebuilt >= kMaxRebuildsPerTick ||
			    (Rebuilt > 0 && BudgetSpentMs >= BuildBudgetMs))
			{
				break;
			}
			FVoxelDetailAssetImpl::FMeshEntry& Entry = Pair.Value;
			if (!Entry.bDirty || Entry.Hism == nullptr)
			{
				continue;
			}
			++Rebuilt;
			Entry.bDirty = false;
			const double RebuildStart = FPlatformTime::Seconds();

			TArray<FTransform> Transforms;
			if (const TSet<FGroupKey>* Members = S.KeyGroups.Find(Pair.Key))
			{
				for (const FGroupKey& G : *Members)
				{
					if (const FVoxelDetailAssetImpl::FGroupRecord* Rec = S.Groups.Find(G))
					{
						for (const FDetailInstanceRec& Inst : Rec->Instances)
						{
							if (Inst.MeshKey == Pair.Key)
							{
								Transforms.Add(DetailInstanceTransform(Inst, Entry.OriginUU));
							}
						}
					}
				}
			}
			Entry.Hism->ClearInstances();
			if (Transforms.Num() > 0)
			{
				Entry.Hism->AddInstances(Transforms, /*bShouldReturnIndices*/ false,
				                         /*bWorldSpace*/ false);
			}
			// The clear and the refill dirty the SAME component in the same
			// frame, so this rebuild is ONE unit of EndOfFrameUpdates work.
			VoxelEofLedger::Count(VoxelEofLedger::ESource::Detail);
			BudgetSpentMs += (FPlatformTime::Seconds() - RebuildStart) * 1000.0;
		}
	}

	// --- 5. dispatch new resolve jobs --------------------------------------
	{
		// Prune finished task handles so the array (and Deinitialize's wait
		// list) stays small.
		S.Tasks.RemoveAll([](const UE::Tasks::TTask<void>& T) { return T.IsCompleted(); });

		const int32 Capacity =
			FMath::Min(kMaxDispatchPerTick, JobsInFlightCap - S.InFlight.Num());
		if (Capacity > 0)
		{
			FVoxelFineTileStreamer* Streamer = VoxelWorld->GetFineTileStreamer();

			struct FCandidate
			{
				double DistSq;
				FGroupKey Key;
			};
			TArray<FCandidate> Candidates;

			const int64 G0X = int64(FMath::FloorToDouble((Anchor.X - RingUU) / kGroupEdgeUU));
			const int64 G1X = int64(FMath::FloorToDouble((Anchor.X + RingUU) / kGroupEdgeUU));
			const int64 G0Y = int64(FMath::FloorToDouble((Anchor.Y - RingUU) / kGroupEdgeUU));
			const int64 G1Y = int64(FMath::FloorToDouble((Anchor.Y + RingUU) / kGroupEdgeUU));
			for (int64 GY = G0Y; GY <= G1Y; ++GY)
			{
				for (int64 GX = G0X; GX <= G1X; ++GX)
				{
					const FGroupKey Key{GX, GY};
					if (S.Groups.Contains(Key) || S.InFlight.Contains(Key))
					{
						continue;
					}
					const double CX = (double(GX) + 0.5) * kGroupEdgeUU;
					const double CY = (double(GY) + 0.5) * kGroupEdgeUU;
					const double DistSq =
						FMath::Square(CX - Anchor.X) + FMath::Square(CY - Anchor.Y);
					if (DistSq > FMath::Square(RingUU))
					{
						continue;
					}
					Candidates.Add({DistSq, Key});
				}
			}
			Candidates.Sort([](const FCandidate& A, const FCandidate& B)
			                { return A.DistSq < B.DistSq; });
			S.StatPendingGroups = Candidates.Num();

			// Time-to-first-full-ring, latched once. "Converged" is the full
			// conjunction: every group in the ring resolved (no candidates
			// left, none in flight), every first-encounter mesh built, and no
			// HISM still waiting on a rebuild -- i.e. everything the resolver
			// placed is actually on screen.
			if (!S.bConvergedLogged && S.Groups.Num() > 0 && Candidates.Num() == 0 &&
			    S.InFlight.Num() == 0 && S.PendingGeometry.Num() == 0)
			{
				bool bAnyDirty = false;
				for (const TPair<uint32, FVoxelDetailAssetImpl::FMeshEntry>& Pair : S.Meshes)
				{
					if (Pair.Value.bDirty && Pair.Value.Hism != nullptr)
					{
						bAnyDirty = true;
						break;
					}
				}
				if (!bAnyDirty)
				{
					S.bConvergedLogged = true;
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelDetailAssets: CONVERGED -- full ring resolved and built "
					            "%.1f s after start. %d groups, %llu instances, %d distinct "
					            "meshes (%.0f ms game-thread mesh-build time total)."),
					       FPlatformTime::Seconds() - S.StartTimeSeconds, S.Groups.Num(),
					       (unsigned long long)S.StatInstancesLive, S.Meshes.Num(),
					       S.StatBuildMsTotal);
				}
			}

			int32 Dispatched = 0;
			for (const FCandidate& C : Candidates)
			{
				if (Dispatched >= Capacity)
				{
					break;
				}

				const vxc::AssetVoxelRect Rect{C.Key.X * kGroupEdgeVoxels,
				                               C.Key.Y * kGroupEdgeVoxels,
				                               C.Key.X * kGroupEdgeVoxels + kGroupEdgeVoxels - 1,
				                               C.Key.Y * kGroupEdgeVoxels + kGroupEdgeVoxels - 1};

				// THE RESIDENCY GATE. A worker column into a non-resident fine
				// tile is a gate leak (fatal in unattended runs) -- defer the
				// group until the streamer has the tiles, dilated by the
				// widest layer reach because instancesForRect evaluates
				// anchors up to that far outside the rect.
				if (Streamer != nullptr)
				{
					const int64 X0Mm = Rect.vx0 * vxc::kVoxelSizeMm - S.MaxReachMm;
					const int64 Y0Mm = Rect.vy0 * vxc::kVoxelSizeMm - S.MaxReachMm;
					const int64 X1Mm = (Rect.vx1 + 1) * vxc::kVoxelSizeMm + S.MaxReachMm;
					const int64 Y1Mm = (Rect.vy1 + 1) * vxc::kVoxelSizeMm + S.MaxReachMm;
					if (!Streamer->IsFootprintResident(X0Mm, Y0Mm, X1Mm, Y1Mm))
					{
						continue; // retried automatically on a later tick
					}
				}

				FResolveJobInput Input;
				Input.Field = Field;
				Input.Amp = Amp;
				Input.Banks = Banks;
				Input.Channels = VoxelWorld->GetAssetChannelSource();
				Input.Group = C.Key;
				Input.Rect = Rect;
				Input.bProduceCover = (GVoxelCoverProduce != 0);
				// SUBORDINATE, AND SPELLED AS AN AND rather than left to the
				// worker to notice: publishing with nothing produced is not a
				// state this file should be able to reach.
				Input.bPublishCover = (GVoxelCoverProduce != 0) && (GVoxelCoverResident != 0);
				Input.KnownGeometry = S.GeometryKnown; // snapshot; stale => dup, not miss

				S.InFlight.Add(C.Key);
				++Dispatched;

				// The queue outlives the job by the Deinitialize wait; plain
				// pointer capture of the impl-owned queue is safe under that
				// contract (WaitForAllJobs runs before Impl is destroyed).
				auto* Queue = &S.Results;
				S.Tasks.Add(UE::Tasks::Launch(
					TEXT("VoxelDetailResolve"),
					[Input = MoveTemp(Input), Queue]()
					{
						TUniquePtr<FGroupResult> R = MakeUnique<FGroupResult>(RunResolveJob(Input));
						Queue->Enqueue(MoveTemp(R));
					},
					UE::Tasks::ETaskPriority::BackgroundNormal));
			}
		}
	}

	// --- 6. periodic telemetry ---------------------------------------------
	// Convergence-curve log: every 5 s until the first full ring, so the curve
	// (groups resolved / pending, meshes built / pending, build throughput) is
	// readable straight off the capture log rather than inferred from settle
	// times. Goes quiet once converged; the 30 s census below continues.
	if (!S.bConvergedLogged)
	{
		S.ProgressLogTimer += DeltaTime;
		if (S.ProgressLogTimer >= 5.0 && S.StatGroupsResolved > 0)
		{
			S.ProgressLogTimer = 0.0;
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("VoxelDetailAssets: converging -- %.1f s elapsed: %d groups live, "
			            "%d pending, %d in flight; %d meshes built, %d builds pending; "
			            "%llu instances; window: %llu builds in %.1f ms; headroom %.2f "
			            "(frame %.1f ms), jobsCap %d, budget %.1f ms"),
			       FPlatformTime::Seconds() - S.StartTimeSeconds, S.Groups.Num(),
			       S.StatPendingGroups, S.InFlight.Num(), S.Meshes.Num(),
			       S.PendingGeometry.Num(), (unsigned long long)S.StatInstancesLive,
			       (unsigned long long)S.WindowMeshBuilds, S.WindowBuildMs, Headroom,
			       FrameMs, JobsInFlightCap, BuildBudgetMs);
			S.WindowMeshBuilds = 0;
			S.WindowBuildMs = 0.0;
		}
	}

	S.LogTimer += DeltaTime;
	if (S.LogTimer >= 30.0)
	{
		S.LogTimer = 0.0;
		if (S.StatGroupsResolved > 0)
		{
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("VoxelDetailAssets: %llu groups live (%d pending, %d in flight), %llu "
			            "instances, %d distinct (species, seed) meshes (%d builds pending), "
			            "%d HISM components, %llu sites resolved total, %llu bank misses, "
			            "%.0f ms mesh-build time total. ZERO INSTANCES WITH GROUPS LIVE MEANS "
			            "THE RESOLVER IS GATING EVERYTHING OUT HERE (biome/elevation/slope), "
			            "not a wiring fault."),
			       (unsigned long long)S.Groups.Num(), S.StatPendingGroups, S.InFlight.Num(),
			       (unsigned long long)S.StatInstancesLive,
			       S.Meshes.Num(), S.PendingGeometry.Num(), HismComponents.Num(),
			       (unsigned long long)S.StatSitesResolved,
			       (unsigned long long)S.StatBankMisses, S.StatBuildMsTotal);
		}
	}
}
