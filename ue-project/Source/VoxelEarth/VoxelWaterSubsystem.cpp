#include "VoxelWaterSubsystem.h"

#include "VoxelDebug.h"
#include "VoxelEarth.h"
#include "VoxelEditRelay.h"
#include "VoxelWaterChunkComponent.h"
#include "VoxelWorldSubsystem.h"

// ADR-0006 water pool (voxel.Water.GPU): a SECOND INSTANCE of the terrain
// geometry pool, not a copy of it. See GetOrCreateWaterPool below for what had
// to be parameterised and what did not.
#include "VoxelGpuPoolComponent.h"

// voxel-core is UE-header-free C++20; safe to include directly from a UE
// module .cpp (same doctrine note as VoxelWorldSubsystem.cpp).
#include "voxelcore/amplifier.h"
#include "voxelcore/bytes.h"
#include "voxelcore/caverns.h"
#include "voxelcore/mesher.h"
// W4 shallow water (docs/adr/0004-swe-fixed-point-coupling.md). This include is
// the first one anywhere in the repo: swe.h shipped complete, tested and
// GOLDEN-PINNED but entirely unreferenced -- "nothing in voxel-core or the
// engine constructs a SweGrid" -- and this file is where that stops being true.
// Nothing in voxel-core changed to make that possible; the header was always
// callable, it was only ever waiting on ADR-0004 item 3.
#include "voxelcore/swe.h"
#include "voxelcore/tiles.h"
#include "voxelcore/waterca.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h" // TActorIterator (locating the world's single AVoxelEditRelay)
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"  // ADR-0005 water persistence: IFileManager (atomic rename, mkdir)
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h" // ADR-0005 water persistence: FFileHelper (blob read/write)
#include "Misc/Paths.h"      // ADR-0005 water persistence: FPaths::ProjectSavedDir (mirror the .vxlog path)
#include "TimerManager.h"    // voxel.Water.SpawnIn's deferred pour
#include "UObject/UObjectGlobals.h"

#include <memory>
#include <vector>

// ADR-0003 item 2/4 (docs/adr/0003-hydrostatic-persistent-body.md): toggles
// vxc::WaterCA's cross-tick terrain-solidity memo. Checked every fixed step
// (see StepFixed below), same "read the cvar every call, no restart needed"
// pattern as VoxelWorldSubsystem.cpp's CVarVoxelDestructionEnabled/
// CVarVoxelCollapseEnabled -- so it can be flipped off in the field without a
// relaunch (task item 4) if the invalidation contract is ever suspected of a
// gap. Byte-identical for as long as UVoxelWorldSubsystem's edit paths honour
// the invalidation contract (NotifyTerrainRegionEdited, wired into
// TryDig/TryPlace/CarveSphere/island-and-collapse-removal -- see
// VoxelWorldSubsystem.cpp).
//
// DEFAULT ON as of ADR-0003 item 2 resolution: an in-engine cross-process A/B
// (-VoxelWaterMemoTest, VoxelEarthGameMode.cpp) ran the full dig/place/carve/
// M5-collapse edit vocabulary beneath and around a settled basin pool, once
// with this cvar forced 0 and once forced 1 (same seed, same edit sequence),
// and diffed EVERY logged checkpoint digest, not just the final one: pour,
// pre-edit settle, post-dig/-place/-carve/-collapse (each immediately after
// its edit AND again once re-settled). All of them matched byte-for-byte
// between the two runs, including intermediate values, confirming the
// invalidation wiring is complete for every edit path this cvar's contract
// depends on. See docs/status.md's "Water edit-notification completeness +
// memo enablement" entry for the full digest table and the perf numbers.
static TAutoConsoleVariable<bool> CVarVoxelWaterSolidCache(
	TEXT("voxel.Water.SolidCacheEnabled"), true,
	TEXT("ADR-0003: enable WaterCA's cross-tick terrain-solidity memo (~2.8-3.0x hydrostatic pass speedup). ")
	TEXT("Byte-identical for as long as every terrain edit invalidates it -- see docs/adr/0003-hydrostatic-persistent-body.md. ")
	TEXT("Set to 0 to force the uncached path (e.g. if an edit path is ever suspected of a missed invalidation)."),
	ECVF_Default);

namespace
{
// BrickKey <-> VoxelCoords::FVoxelCoord: this subsystem carries brick-grid
// coordinates (NOT voxel coordinates) through UE-visible plumbing as
// FVoxelCoord, per that struct's W2 doc comment (VoxelCoords.h) -- these two
// tiny helpers are the one place the reinterpretation happens.
VoxelCoords::FVoxelCoord ToCoord(const vxc::BrickKey& K)
{
	return VoxelCoords::FVoxelCoord{K.x, K.y, K.z};
}
vxc::BrickKey ToBrickKey(const VoxelCoords::FVoxelCoord& C)
{
	return vxc::BrickKey{static_cast<int32_t>(C.X), static_cast<int32_t>(C.Y), static_cast<int32_t>(C.Z)};
}

// --- Replication wire format (task item 1) ----------------------------------
//
// Brick-granularity raw-fill snapshots (u32 count, then per brick: i32 x,y,z
// + 512 raw fill bytes in the SAME fixed cell order WaterBrick8::digest uses)
// -- NOT a per-cell delta. Deliberately simple (task spec: "basic diff
// encoding -- full compression is W2-polish"); built from vxc::ByteWriter/
// ByteReader, the same primitives UVoxelWorldSubsystem's edit-log wire format
// uses (VoxelWorldSubsystem.cpp's SerializeEntries/ParseEntries).
//
// Processes Keys in order, stopping once MaxBytes would be exceeded (but
// always encoding at least one brick, guaranteeing forward progress even if
// a single brick's 524 bytes alone exceeds a pathologically small cap).
// OutConsumedKeyCount reports how many of the FRONT of Keys were fully
// handled (encoded OR determined-absent-so-nothing-to-send) -- the caller
// drops exactly those from its "dirty since last broadcast" set, leaving any
// remainder (past the byte cap) for the next ~5Hz round.
void SerializeWaterDiffs(const std::vector<vxc::BrickKey>& Keys, const vxc::WaterCA& CA, int32 MaxBytes, TArray<uint8>& OutBytes,
                          int32& OutEncodedBrickCount, size_t& OutConsumedKeyCount)
{
	std::vector<uint8_t> Bytes;
	vxc::ByteWriter W(Bytes);
	W.u32(0); // patched below once the final encoded count is known
	uint32_t Encoded = 0;
	size_t Consumed = 0;
	constexpr size_t EntrySize = 12 + size_t(vxc::WaterBrick8::kCells); // i32 x,y,z + 512 raw fill bytes

	for (; Consumed < Keys.size(); ++Consumed)
	{
		const vxc::BrickKey& K = Keys[Consumed];
		const vxc::WaterBrick8* Brick = CA.findBrick(K);
		if (!Brick)
		{
			continue; // emptied since being marked dirty -- nothing to send, but fully handled
		}
		if (Encoded > 0 && Bytes.size() + EntrySize > size_t(MaxBytes))
		{
			break; // cap reached -- this (and later) keys wait for next round
		}
		W.i32(K.x);
		W.i32(K.y);
		W.i32(K.z);
		for (int z = 0; z < vxc::WaterBrick8::kEdge; ++z)
			for (int y = 0; y < vxc::WaterBrick8::kEdge; ++y)
				for (int x = 0; x < vxc::WaterBrick8::kEdge; ++x) W.u8(Brick->get(x, y, z));
		++Encoded;
	}

	Bytes[0] = uint8_t(Encoded);
	Bytes[1] = uint8_t(Encoded >> 8);
	Bytes[2] = uint8_t(Encoded >> 16);
	Bytes[3] = uint8_t(Encoded >> 24);
	OutBytes.SetNumUninitialized((int32)Bytes.size());
	if (!Bytes.empty())
	{
		FMemory::Memcpy(OutBytes.GetData(), Bytes.data(), Bytes.size());
	}
	OutEncodedBrickCount = int32(Encoded);
	OutConsumedKeyCount = Consumed;
}
} // namespace

// FVoxelWaterImpl -- the voxel-core side of the subsystem, defined only here
// so VoxelWaterSubsystem.h (UHT-parsed) never sees a voxel-core header (same
// pattern as FVoxelWorldImpl in VoxelWorldSubsystem.cpp).
struct FVoxelWaterImpl
{
	explicit FVoxelWaterImpl(UVoxelWorldSubsystem& InTerrain)
		: Terrain(InTerrain)
		, Tiles(InTerrain.GetSeed())
		, Amp(InTerrain.GetSeed(), Tiles)
		, Mob(
			  // The implicit static flood field (C7, docs/cavern-design.md SS5.1):
			  // worldgen-owned, deterministic, ZERO storage. caverns.h's
			  // cavernFloodedAt is exactly half the predicate -- "is this below
			  // the column's flood level" -- and WaterMobilizer applies the other
			  // half (is the cell actually open air) itself, so this callback
			  // stays a pure worldgen query.
			  [this](int64_t vx, int64_t vy, int64_t vz) -> uint8_t
			  { return vxc::cavernFloodedAt(Amp.columnCached(vx, vy).cavern, vz) ? uint8_t(255) : uint8_t(0); },
			  [this](int64_t vx, int64_t vy, int64_t vz) -> vxc::MaterialId
			  { return Terrain.IsSolidAtVoxel(vx, vy, vz) ? vxc::MAT_ROCK : vxc::MAT_AIR; })
		// NOT the bare terrain query: makeSolidFn() layers the implicit-water
		// WALL on top, which is what makes the implicit/CA ownership partition
		// structural rather than a matter of call ordering (waterca.h).
		, CA(Mob.makeSolidFn())
	{
	}

	UVoxelWorldSubsystem& Terrain;

	// OUR OWN worldgen sampler, not UVoxelWorldSubsystem's. That subsystem is
	// another agent's file and exposes no column/cavern accessor, so — exactly
	// as AVoxelClipmapActor already does for terrain height — we build a second
	// Amplifier over the same seed. Worldgen is a pure function of (seed, tile
	// sampler), so this is bit-identical to the terrain's own for every run
	// that uses the synthetic sampler (the default).
	//
	// CAVEAT, and the reason for the warning logged in OnWorldBeginPlay: a run
	// launched with -VoxelTileDir uses a real tile-grid sampler over there and
	// this synthetic one over here, so the surfaces disagree and cavern flood
	// levels would be computed against the wrong terrain. Follow-up: a public
	// column accessor on UVoxelWorldSubsystem, which its owner must add.
	vxc::SyntheticTileSampler Tiles;
	vxc::Amplifier Amp;

	// MUST be declared before CA: makeSolidFn() hands the CA a callable that
	// captures the mobilizer, so the mobilizer has to outlive it.
	vxc::WaterMobilizer Mob;

	vxc::WaterCA CA;

	// One UWaterChunkComponent per rendered IMPLICIT (not-yet-mobilized) water
	// brick, keyed the same way ChunkComponents is. Kept separate from the CA's
	// map so mobilization is a clean handover: the implicit component is
	// destroyed and the CA's is created, and because implicitFillAt() returns 0
	// for a mobilized brick the two can never both draw the same water.
	TMap<VoxelCoords::FVoxelCoord, TObjectPtr<UWaterChunkComponent>> ImplicitChunkComponents;

	// Brick coordinate the implicit-water refresh last centred on, so a
	// stationary camera costs nothing. Set once the first refresh has run.
	VoxelCoords::FVoxelCoord LastImplicitCenterBrick = {0, 0, 0};
	bool bImplicitCenterValid = false;

	// Bricks the implicit pass still owes a mesh, drained under a per-tick
	// budget exactly like DirtyBricks.
	TArray<VoxelCoords::FVoxelCoord> PendingImplicitBricks;

	// Reservoir v0 (docs/voxel-earth-implementation-plan.md SS3.7): breach-
	// boundary voxel coordinates that continuously top up to 255 fill units
	// every fixed step -- see UVoxelWaterSubsystem::NotifyTerrainVoxelsCleared
	// for the seeding rule and UVoxelWaterSubsystem::StepFixed for the top-up.
	TSet<VoxelCoords::FVoxelCoord> ReservoirCells;

	// Fixed 10Hz accumulator (task item 3).
	float TickAccumSeconds = 0.f;
	static constexpr float FixedStepSeconds = 0.1f; // 10Hz
	// Fixed-timestep spiral-of-death guard: a frame hitch accumulates debt
	// but never demands more than this many catch-up steps in one Tick.
	static constexpr int32 MaxStepsPerFrame = 4;

	// One UWaterChunkComponent per resident vxc::WaterBrick8, keyed by its
	// BrickKey (carried as VoxelCoords::FVoxelCoord -- brick-grid
	// coordinates; see the file-scope ToCoord/ToBrickKey doc comment).
	TMap<VoxelCoords::FVoxelCoord, TObjectPtr<UWaterChunkComponent>> ChunkComponents;

	// --- ADR-0006 water pool (voxel.Water.GPU) ------------------------------
	//
	// The pooled alternative to the two component maps above: a brick's geometry
	// is a RANGE in a shared quad buffer rather than a scene primitive of its
	// own.
	//
	// W5: A HANDFUL OF PRIMITIVES, BUCKETED SPATIALLY, NOT ONE.
	//
	// This used to be one pool component and therefore one primitive, and
	// docs/gpu-water-pool-design.md argued at length why that was sound: the
	// renderer sorts translucent geometry per PRIMITIVE, so pooled water had one
	// sort key for the whole world -- but with a constant base colour, a constant
	// 0.55 opacity and no scene read, two water fragments differed only in their
	// lighting and a stack of N surfaces transmitted (1-0.55)^N in ANY order.
	// The doc also named exactly what would end that: "per-fill shading, foam or
	// caustics", and named the fallback: "per-region sort keys (several
	// primitives, one per spatial bucket)".
	//
	// Depth-tinted absorption and foam land in this same wave, so this is that
	// fallback, built FIRST and deliberately so -- a tint applied over a broken
	// sort makes a sorting artefact and a shading artefact indistinguishable.
	//
	// Each pool is created lazily on the first brick that wants it, on its OWN
	// actor as that actor's ROOT component -- attached as a child of ChunkRoot
	// the primitive never enters the visible set at all
	// (docs/gpu-pool-rendering-notes.md invariant 1).
	struct FPoolBucket
	{
		// Bucket coordinate = floorDiv(brick coordinate, kWaterPoolBucketBricks).
		VoxelCoords::FVoxelCoord Key{0, 0, 0};
		TWeakObjectPtr<UVoxelGpuPoolComponent> Pool;
		// The pool's float32 chunk table cannot hold this world's ~8.4M UU
		// coordinates (invariant 4), so the component carries the offset in its
		// double-precision transform and the table stores brick origins relative
		// to it. DERIVED FROM Key, not from the first brick that happened to
		// arrive: a bucket's rebase is then a pure function of where it is, which
		// is what makes re-homing an emptied bucket (below) a one-line move
		// rather than a migration.
		FVector Rebase = FVector::ZeroVector;
	};
	TArray<FPoolBucket> PoolBuckets;
	TMap<VoxelCoords::FVoxelCoord, int32> PoolBucketIndex;

	// Set while a batched re-mesh loop is running, so a bucket created MID-LOOP
	// can be folded into the same publication scope as the buckets that existed
	// when the loop started. Without it the first tick that reaches a new region
	// would publish once per brick on the new bucket -- the exact per-brick
	// publication tax S1-1 exists to remove, arriving precisely when a burst of
	// bricks is landing.
	struct FVoxelWaterPoolBatch* ActiveBatch = nullptr;

	// Pool handles, keyed exactly like the component maps they replace. Two
	// maps for the same reason there are two component maps: the implicit
	// (worldgen) and CA (simulated) halves own disjoint bricks and hand over
	// between themselves, and MarkMobilizedBricksDirty has to be able to drop
	// one without touching the other.
	//
	// The value is (bucket, handle) rather than a bare handle now: a pool handle
	// is only meaningful against the pool that issued it, and there are several.
	// Storing the bucket beside it -- rather than re-deriving it from the brick
	// coordinate at release time -- is what makes a brick releasable even after
	// its bucket has been re-homed to a different region, which is the one
	// ordering this scheme has to survive.
	struct FPooledBrick
	{
		int32 Bucket = INDEX_NONE;
		int32 Handle = INDEX_NONE;
	};
	TMap<VoxelCoords::FVoxelCoord, FPooledBrick> PoolSlots;
	TMap<VoxelCoords::FVoxelCoord, FPooledBrick> ImplicitPoolSlots;

	// W5 foam input: the bricks vxc::WaterCA reported ACTIVE at the end of the
	// last fixed step. Read at mesh time and written into vertex colour A (via
	// ChunkParams.y on the pooled path, directly on the component path), where
	// M_WaterVoxel turns it into foam.
	//
	// A MEMBER RATHER THAN A LIVE CA QUERY because it is also what drives the
	// SETTLE EDGE: a brick that stops being active is, by definition, no longer
	// marked dirty, so without a record of what WAS active nothing would ever
	// re-mesh it to turn its foam back off. StepFixed diffs this against the new
	// active set and marks the difference dirty exactly once.
	TSet<VoxelCoords::FVoxelCoord> ActiveBricks;

	// 1 Hz status line for the pooled path. Budgeted before it was needed: one
	// primitive drawing thousands of bricks has no per-brick state to inspect,
	// so "what does the pool hold" has to come from a log line or from nowhere.
	float PoolLogAccumSeconds = 0.f;

	// Bricks touched since the last re-mesh pass (task item 4): unioned from
	// vxc::WaterCA::activeBricks() after every fixed step, PLUS any brick an
	// addWater() call (spawn/breach/reservoir top-up) touched directly --
	// the latter needs marking immediately rather than waiting for the next
	// step() to fold it into the CA's own active set, so a fresh pour is
	// visible the same frame it's placed.
	TSet<VoxelCoords::FVoxelCoord> DirtyBricks;

	// --- Perf / HUD bookkeeping (task item 3) -------------------------------
	float PerfRefreshAccumSeconds = 0.f;
	int32 StepsThisWindow = 0;
	int32 LastSteppedBrickCount = 0;
	int32 ReplicatedBytesThisWindow = 0;
	FVoxelWaterPerfSnapshot LastSnapshot;

	// Log-throttle for the voxel.Water.MaxActiveBricks budget warning (task
	// item 3: "log-throttle warning (do not explode)").
	double LastBudgetWarnWorldSeconds = -1000.0;

	// --- Replication v1 (task item 1) ---------------------------------------
	float ReplicationAccumSeconds = 0.f;
	static constexpr float ReplicationIntervalSeconds = 0.2f; // ~5Hz
	static constexpr int32 MaxDiffBytesPerBroadcast = 32 * 1024; // documented cap (task item 1)
	TSet<VoxelCoords::FVoxelCoord> DirtySinceLastBroadcast;

	// --- W4 shallow water, voxel.Water.SWE (ADR-0004) -----------------------
	//
	// BOTH NULL unless the cvar has been armed on a standalone world, and that
	// is the whole safety story for this feature: an unarmed run allocates
	// nothing, queries nothing and branches once per fixed step on a null
	// pointer. swe.h's inertness argument -- "a world in which nothing ever
	// mobilizes is bit-for-bit a world in which this class does not exist" --
	// is inherited verbatim, which is what lets W4 come alive without a version
	// bump or a moved golden.
	//
	// DECLARATION ORDER IS LOAD-BEARING, exactly like Mob-before-CA above:
	// SweCaCoupler holds SweGrid& and WaterCA&, so the grid must outlive the
	// coupler. Members destruct in reverse declaration order, so coupler-last
	// is coupler-first-destroyed.
	//
	// A SINGLE DENSE 128x128 REGION, not a tiled/sparse residency scheme. swe.h
	// says outright that the dense rectangle is the reference core and "a
	// production sheet tiles this; the tick rules do not change" -- so tiling is
	// a later, separable concern that would only obscure this first wiring.
	// 128x128 is also the size ADR-0004 measured end-to-end at 0.364 ms/tick
	// (16,384 columns, linear in columns), i.e. ~3.6% of the 10Hz step's own
	// budget and comfortably inside the subsystem's <2ms/frame target even
	// though it is paid every step rather than every frame.
	std::unique_ptr<vxc::SweGrid> SweSheet;
	std::unique_ptr<vxc::SweCaCoupler> SweCoupler;

	// The conservation ledger for the ENGINE WIRING, which is the part with no
	// tests. swe.h's numerics and the coupler's three transfer channels are
	// golden-pinned and covered by test_swe.cpp's headline invariant; what is
	// entirely new here is the code in this file that decides when to construct
	// a grid, where to put it, which columns to seed, and when to flush it back.
	// That code can lose water in ways voxel-core's tests can never see, so it
	// carries its own runtime check -- see the assert in StepFixed.
	//
	// SweInjected is the running "total injected" of ADR-0004's invariant
	// `ca.totalVolume() + grid.totalVolume() == total injected`. It cannot be a
	// constant here the way it is in a unit test: the live CA is injected into
	// continuously and legitimately from OUTSIDE the coupled window (reservoir
	// top-ups, voxel.SpawnWater, breach seeding, C8 mobilization crediting
	// implicit cavern water, and placements destroying water). So every fixed
	// step folds the change observed OUTSIDE the window into SweInjected, and
	// then requires the window itself -- coupler.step(); grid.step(); ca.step()
	// -- to move the sum by exactly zero. That is the same claim, checked once
	// per tick instead of once per run, and it localises any failure to the
	// three calls that could have caused it.
	int64_t SweInjected = 0;
	int64_t SweLastCoupledTotal = 0;
	int64_t SweConservationFailures = 0;

	// Per-column depth as of the last tick, row-major over the sheet, for
	// dirty-marking the render bricks above a column whose depth moved.
	//
	// WHY THIS IS NEEDED AT ALL. The CA drives re-meshing through its own
	// changed-brick set, and sheet water is not in the CA -- once a column
	// promotes, its depth can change every tick while the CA reports nothing
	// dirty at all, so the surface would simply freeze at whatever it looked
	// like when the water left the CA. Diffing 16,384 ints is negligible next
	// to the 0.364 ms/tick the sheet step itself costs.
	std::vector<int32_t> SweLastDepth;

	// One-shot latches so a refused arm request (wrong net mode, nothing to
	// centre on) says so once rather than once per frame for the whole session.
	bool bSweRefusalLogged = false;
	double SweLastStatusWorldSeconds = -1000.0;
};

namespace
{
// Marks every water brick spanning [VzStart, VzStart + ceil(PlacedAmount/255)]
// at (Vx,Vy) dirty for re-mesh AND replication -- addWater() stacks straight
// up a column, so this is the full set of bricks a single addWater() call
// could possibly have touched (a one-brick apron above the theoretical span
// costs nothing: findBrick() returning null there is a harmless no-op in
// RemeshDirtyBricks/SerializeWaterDiffs).
void MarkColumnDirty(FVoxelWaterImpl& Impl, int64_t Vx, int64_t Vy, int64_t VzStart, uint32_t PlacedAmount)
{
	const int64_t CellsSpan = int64_t(PlacedAmount) / 255 + 2;
	for (int64_t Dz = 0; Dz <= CellsSpan; ++Dz)
	{
		const VoxelCoords::FVoxelCoord C = ToCoord(vxc::waterKeyForVoxel(Vx, Vy, VzStart + Dz));
		Impl.DirtyBricks.Add(C);
		Impl.DirtySinceLastBroadcast.Add(C);
	}
}
} // namespace

// ADR-0005 water persistence: forward declarations. The definitions live in
// the big anonymous namespace below (they lean on MarkMobilizedBricksDirty /
// ToCoord, defined there), but OnWorldBeginPlay / Deinitialize / SaveWaterState
// above them need to call them.
namespace
{
FString GetWaterSaveFilePath(uint64 Seed);
bool SaveWaterStateToDisk(const FVoxelWaterImpl& Impl, uint64 Seed);
void LoadWaterStateFromDisk(FVoxelWaterImpl& Impl, uint64 Seed);

// W4 (ADR-0004). Same forward-declaration reason as the three above: these are
// defined down beside StepFixed (they lean on MarkColumnDirty and on the
// meshing helpers' brick bookkeeping), but Tick() and SaveWaterState() are
// above them and both have to call them.
void MaybeArmSwe(FVoxelWaterImpl& Impl, UWorld* World);
void FlushSweIntoCA(FVoxelWaterImpl& Impl, const TCHAR* Reason);
void MarkSweDepthChangesDirty(FVoxelWaterImpl& Impl);
} // namespace

// UVoxelWaterSubsystem ------------------------------------------------------

UVoxelWaterSubsystem::UVoxelWaterSubsystem() = default;
UVoxelWaterSubsystem::~UVoxelWaterSubsystem() = default;
UVoxelWaterSubsystem::UVoxelWaterSubsystem(FVTableHelper& Helper)
	: Super(Helper)
{
}

void UVoxelWaterSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Direct dependency on the terrain subsystem (task spec item 1): both are
	// UWorldSubsystem-derived, so InitializeDependency handles ordering --
	// UVoxelWorldSubsystem::Impl is guaranteed constructed by the time this
	// call returns, so IsSolidAtVoxel is safe to bind into the CA's SolidFn
	// right away (it isn't actually CALLED until streaming/breach/spawn
	// touches a voxel, well after both subsystems' Initialize has run).
	UVoxelWorldSubsystem* Terrain = Collection.InitializeDependency<UVoxelWorldSubsystem>();
	if (!Terrain)
	{
		UE_LOG(LogVoxelWater, Error,
		       TEXT("UVoxelWaterSubsystem::Initialize: no UVoxelWorldSubsystem -- water CA cannot bridge to terrain ")
		       TEXT("solidity; water is disabled for this run."));
		return;
	}

	Impl = MakeUnique<FVoxelWaterImpl>(*Terrain);
}

void UVoxelWaterSubsystem::Deinitialize()
{
	// ADR-0005: autosave the water blob on shutdown, mirroring
	// UVoxelWorldSubsystem::Deinitialize's edit-log autosave lifetime exactly --
	// authority only, and only for a world that genuinely began play (see
	// bWorldBegunPlay). Additionally gated on there being real water state to
	// persist (stored or mobilized bricks) so a run that never disturbed
	// underground water leaves no sibling .vxwater to shadow a later same-seed
	// session -- the edit log has no such gate because every world has a log,
	// but an all-implicit world has nothing to serialize and an empty blob would
	// only add a spurious file.
	if (Impl && bWorldBegunPlay)
	{
		UWorld* World = GetWorld();
		const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
		// W4 (ADR-0004): the sheet counts as "real water state to persist" too,
		// and it has to be named explicitly here. Promotion MOVES units out of
		// CA cells, so a session whose whole pour has been promoted can have
		// storedBrickCount() == 0 with a full sheet -- and the pre-W4 gate would
		// then skip the autosave, drop the grid with Impl.Reset() below, and
		// lose the lot on shutdown. SaveWaterState flushes the sheet back into
		// the CA before serialising, so once we decide to save, the blob is
		// complete.
		const bool bHaveSheetWater = Impl->SweSheet && Impl->SweSheet->totalVolume() > 0;
		if (NetMode != NM_Client &&
		    (Impl->CA.storedBrickCount() > 0 || !Impl->Mob.mobilizedBricks().empty() || bHaveSheetWater))
		{
			SaveWaterState();
		}
	}

	ChunkRoot = nullptr;
	ChunkOwner = nullptr;
	WaterMaterial = nullptr;
	Impl.Reset();

	Super::Deinitialize();
}

bool UVoxelWaterSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Same scope as UVoxelWorldSubsystem: game worlds only.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::GamePreview;
}

void UVoxelWaterSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (!InWorld.IsGameWorld() || !Impl)
	{
		return;
	}

	// ADR-0005: from here on this is a genuine gameplay world (same reasoning as
	// UVoxelWorldSubsystem's bWorldBegunPlay) -- gates Deinitialize's autosave.
	bWorldBegunPlay = true;

	// ADR-0005 water persistence: the authority (server/listen/standalone) loads
	// Saved/VoxelWorlds/<seed>.vxwater, if present, before the first fixed step
	// ever runs (Tick() below). This is the earliest point OnWorldBeginPlay
	// reaches after the game-world/Impl guard, and it runs BEFORE the
	// dedicated-server early-return below so it applies to every authority role,
	// exactly mirroring UVoxelWorldSubsystem's LoadEditLogFromDisk placement. The
	// live CA/mob are still fresh here (no addWater/mobilize has run yet), which
	// is vxc::WaterState::applyTo's precondition. Fills-first load, three
	// failure modes handled loudly -- see LoadWaterStateFromDisk. NM_Client never
	// loads its own file: a joining client mirrors state via replication instead.
	if (InWorld.GetNetMode() != NM_Client)
	{
		LoadWaterStateFromDisk(*Impl, Impl->Terrain.GetSeed());
	}

	// Dedicated server: no viewport, so no render chunks -- but the CA still
	// ticks authoritatively regardless (Tick() below doesn't check
	// ChunkOwner for the simulation half, only for the re-mesh half). Same
	// reasoning as UVoxelWorldSubsystem's identical dedicated-server branch.
	if (InWorld.GetNetMode() == NM_DedicatedServer)
	{
		UE_LOG(LogVoxelWater, Log,
		       TEXT("Water rendering DISABLED (NM_DedicatedServer has no viewport): the pressure CA still ticks ")
		       TEXT("authoritatively at %.0fHz."),
		       1.f / FVoxelWaterImpl::FixedStepSeconds);
		return;
	}

	WaterMaterial = Cast<UMaterialInterface>(
		StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/Game/Voxel/M_WaterVoxel.M_WaterVoxel")));
	if (WaterMaterial == nullptr)
	{
		WaterMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		UE_LOG(LogVoxelWater, Warning, TEXT("M_WaterVoxel not found at /Game/Voxel/M_WaterVoxel -- using engine default material."));
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags |= RF_Transient;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ChunkOwner = InWorld.SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	if (ChunkOwner == nullptr)
	{
		UE_LOG(LogVoxelWater, Error, TEXT("Failed to spawn the water chunk owner actor; water rendering will not start."));
		return;
	}

	ChunkRoot = NewObject<USceneComponent>(ChunkOwner, TEXT("VoxelWaterChunkRoot"));
	ChunkOwner->SetRootComponent(ChunkRoot);
	ChunkRoot->RegisterComponent();
#if WITH_EDITOR
	ChunkOwner->SetActorLabel(TEXT("VoxelWaterChunkOwner"));
#endif

	UE_LOG(LogVoxelWater, Log, TEXT("Water subsystem initialized: fixed step %.0fHz, MaxActiveBricks budget=%d"),
	       1.f / FVoxelWaterImpl::FixedStepSeconds, VoxelDebug::GetWaterMaxActiveBricks());
}

namespace
{
// --- ADR-0006 water pool (voxel.Water.GPU) ---------------------------------
//
// WHY A SECOND INSTANCE OF UVoxelGpuPoolComponent RATHER THAN A WATER-SPECIFIC
// COPY. The pool is generic in everything water needed it to be:
//
//   * The quad packing is shared already. PackVoxelChunkQuad's fields are all
//     <= 8 bits and a water brick is 8 voxels on an edge, so brick-local
//     coordinates fit the same encoding terrain's 32-voxel chunk-local ones do
//     -- with 2 bits to spare rather than 3. No second decode path.
//   * The vertex factory writes exactly the one channel M_WaterVoxel reads.
//     That material's only input from the geometry is VertexColor.G (AO), and
//     VoxelQuadVertexFactory.ush writes AO into .G. Channels R/B/A carry
//     terrain-specific biome-tint and climate values, which this material does
//     not sample -- so they are inert here, not wrong.
//   * Translucency is a MATERIAL property, and the proxy already derives its
//     view relevance from FMaterialRelevance. Nothing in the draw path had to
//     learn about blend modes.
//
// Three things did have to be parameterised, and all three are one-line
// setters on the shared component rather than forks of it: the log prefix
// (two pools printing the same line is worse than no line), the per-entry edge
// length used to grow bounds (8 voxels, not 32), and the chunk-table floor.
//
// The one genuine BEHAVIOURAL fix the water instance forced is in the pool
// itself, not here: chunk-table entries were never recycled. See
// UVoxelGpuPoolComponent::FreeChunkIds.
//
// --- W5: SEVERAL INSTANCES, BUCKETED BY WORLD SPACE ------------------------
//
// WHY THE COUNT CHANGED. UE sorts translucent draw commands by a key whose
// distance field is filled from `PrimitiveBounds[PrimitiveIndex]
// .BoxSphereBounds.Origin` (UpdateTranslucentMeshSortKeys, MeshDrawCommands.cpp)
// -- the PRIMITIVE's bounds centre, one value for every command the primitive
// emits. Within a primitive the remaining tie-break is the 16-bit
// MeshIdInPrimitive, which is a STABLE id and not a per-view depth order. So a
// single water primitive genuinely cannot sort its own contents against itself
// or against anything else at better than whole-pool granularity, however many
// mesh batches it emits. Splitting the primitive is the only lever the renderer
// offers short of an OIT path.
//
// WHY 64 BRICKS (51.2 m) ON A SIDE. Sized from the largest water body this
// system actually produces, which is the implicit cavern-lake shell:
// RefreshImplicitWater sweeps a 65 x 65 x 33 BRICK disc, i.e. 52 x 52 x 26 m.
// A bucket larger than that disc gives back nothing at all -- the whole lake
// would land in one bucket and we would be exactly where we started. 64 makes
// the disc span 2 x 2 buckets horizontally and 1-2 vertically, so a typical
// scene draws 4-8 water primitives instead of 1. That is the "handful" this is
// aiming at, and it is three orders of magnitude below the per-brick component
// path's 2,231 primitives at the same anchor (docs/gpu-water-pool-design.md).
//
// REJECTED, AND WHY:
//
//   * 16 bricks (12.8 m). Sorts better, and the implicit disc alone would then
//     want up to 5x5x3 = 75 primitives. Not fatal -- still far below the
//     component path -- but it re-opens the FScene::AddPrimitive funnel ADR-0006
//     exists to close, in exchange for a granularity that STILL does not fix the
//     dominant artefact (see the next point). Bad trade.
//   * Trying to fix the artefact properly with buckets at all. Water surfaces
//     stack most often at ONE-BRICK range: the near side wall, the surface and
//     the far side wall of a single stepped pool. No bucket size above a brick
//     orders those, and a bucket per brick is the per-brick component path with
//     extra steps. What bucketing DOES fix is long-range compositing -- one lake
//     seen through another, water above a cavern lake, and water against a
//     foreign translucent primitive -- and that is the honest claim.
//   * Bucketing only in XY, with Z unbounded. Cheaper (fewer buckets), and
//     wrong for the case this project has most of: a surface lake directly above
//     a cavern lake is the textbook water-over-water stack, and a Z-unbounded
//     column puts both in one bucket.
//   * Keeping one component and ranking its FMeshBatches with MeshIdInPrimitive
//     per view. It would order water against water for free, with no extra
//     primitive -- but MeshIdInPrimitive sits BELOW Distance in the sort key, so
//     it can only break ties WITHIN one primitive. Foreign translucent geometry
//     would still sort against the whole pool as one unit, which is the third
//     break condition gpu-water-pool-design.md names. Kept in reserve as an
//     intra-bucket refinement.
//
// WHAT A BUCKET COSTS. Its own quad buffer (kWaterBucketCapacityQuads, 4 MB),
// its own chunk table, its own proxy and its own cull walk. That is why buckets
// are RECYCLED rather than accumulated: water follows the camera, so buckets
// empty out behind it, and an emptied bucket is re-homed to a new region
// instead of allocating a new one. The rebase is a pure function of the bucket
// key, so re-homing is a SetWorldLocation and nothing else.

// Edge length of one sort bucket, in water bricks. See the essay above.
constexpr int32 kWaterPoolBucketBricks = 64;

// Ceiling on live bucket primitives. Not a tuning knob: past this, bricks are
// routed to their NEAREST live bucket, which draws them in the right place with
// the wrong sort key -- i.e. it degrades to exactly the pre-W5 behaviour for
// the overflow rather than dropping geometry. 12 is ~1.5x the 4-8 a typical
// scene wants, so reaching it at all means something unusual is on screen.
constexpr int32 kMaxWaterPoolBuckets = 12;

// Per-bucket quad capacity. 4 MB each; 12 buckets is a 48 MB ceiling against
// the single pool's 32 MB, and the typical 4-8 live buckets sit at 16-32 MB,
// i.e. at or below where this started.
//
// SIZED AGAINST THE WHOLE WORLD'S WATER, NOT A BUCKET'S SHARE, deliberately.
// The measured peak is 28,862 quads across 2,231 implicit bricks (~13 quads per
// brick). Even if EVERY implicit brick and the CA's entire 4,096-brick budget
// landed in ONE bucket at a pessimistic 80 quads each, that is ~500k quads. A
// per-bucket pool cannot borrow from its neighbours the way one big pool could,
// so the headroom has to cover the degenerate distribution rather than the
// expected one.
constexpr uint32 kWaterBucketCapacityQuads = 512u * 1024u;

// Per-bucket chunk-table floor. Crossing it is not an error but it IS a full
// render-state rebuild and a full quad re-upload, so it wants to sit above the
// bucket's steady-state entry count rather than near it. 8,192 is ~3.7x the
// 2,231 implicit bricks measured for a whole scene.
constexpr int32 kWaterBucketTableCapacity = 8192;

// Which bucket a brick belongs to. floorDiv, not integer division: brick
// coordinates go negative and C++ truncates toward zero, which would fold
// bricks at -1 and +1 into the same bucket and put a sort-bucket seam through
// the world origin.
VoxelCoords::FVoxelCoord WaterPoolBucketKey(const VoxelCoords::FVoxelCoord& BrickCoord)
{
	return VoxelCoords::FVoxelCoord{
		int64(vxc::floorDiv(int64_t(BrickCoord.X), int64_t(kWaterPoolBucketBricks))),
		int64(vxc::floorDiv(int64_t(BrickCoord.Y), int64_t(kWaterPoolBucketBricks))),
		int64(vxc::floorDiv(int64_t(BrickCoord.Z), int64_t(kWaterPoolBucketBricks)))};
}

// A bucket's rebase: the world-space corner of the bucket itself. Derived, not
// remembered -- see FPoolBucket::Rebase.
FVector WaterPoolBucketRebase(const VoxelCoords::FVoxelCoord& BucketKey)
{
	constexpr double kBucketUU =
		double(kWaterPoolBucketBricks) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU;
	return FVector(double(BucketKey.X) * kBucketUU, double(BucketKey.Y) * kBucketUU,
	               double(BucketKey.Z) * kBucketUU);
}

UVoxelGpuPoolComponent* SpawnWaterPoolPrimitive(AActor* ChunkOwner, UMaterialInterface* Material,
                                                const VoxelCoords::FVoxelCoord& BucketKey)
{
	UWorld* World = ChunkOwner ? ChunkOwner->GetWorld() : nullptr;
	if (World == nullptr)
	{
		return nullptr;
	}

	// ITS OWN ACTOR, WITH THE POOL AS ROOT. Not attached under ChunkRoot beside
	// the per-brick components: as a child the primitive never enters the
	// visible set -- GetDynamicMeshElements is never called, with a live proxy,
	// valid bounds and no warning anywhere (gpu-pool-rendering-notes.md
	// invariant 1). The terrain pool learned this the expensive way; there was
	// no reason to learn it twice.
	FActorSpawnParameters PoolSpawnParams;
	PoolSpawnParams.ObjectFlags |= RF_Transient;
	PoolSpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* PoolOwner = World->SpawnActor<AActor>(
		AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, PoolSpawnParams);
	if (PoolOwner == nullptr)
	{
		return nullptr;
	}
#if WITH_EDITOR
	PoolOwner->SetActorLabel(FString::Printf(TEXT("VoxelWaterPoolOwner_%lld_%lld_%lld"), (long long)BucketKey.X,
	                                         (long long)BucketKey.Y, (long long)BucketKey.Z));
#endif

	UVoxelGpuPoolComponent* Pool = NewObject<UVoxelGpuPoolComponent>(PoolOwner);
	// NAMED PER BUCKET. Every pool diagnostic in this project is a log line
	// keyed on the pool name (the pool has no per-brick state to inspect), and
	// several buckets all printing "VoxelWaterPool draw SUBMITTED" would make
	// the one number that matters unattributable -- which is the same reason
	// the water pool got a name distinct from terrain's in the first place.
	Pool->SetPoolName(FString::Printf(TEXT("VoxelWaterPool[%lld,%lld,%lld]"), (long long)BucketKey.X,
	                                  (long long)BucketKey.Y, (long long)BucketKey.Z));
	// vxc::WaterBrick8::kEdge. Terrain's 32 would still be CORRECT here (a
	// larger bounds box is a looser cull, never a lost draw), but 8 is what the
	// geometry actually spans and this pool's bricks are 1/64th the volume.
	Pool->SetChunkEdgeVoxels(vxc::WaterBrick8::kEdge);
	// Above this BUCKET's plausible resident-brick count, so an ordinary camera
	// move does not oscillate across the table capacity and pay a full
	// render-state rebuild -- and a full re-upload of the quad buffer -- each
	// time it does. See kWaterBucketTableCapacity.
	Pool->SetChunkTableCapacity(kWaterBucketTableCapacity);
	// Makes the pooled path reproduce water's vertex-colour convention rather
	// than terrain's: R = the CA fill fraction this subsystem's meshing sampler
	// puts in the quad's `mat` byte (terrain puts a binary sky-facing biome
	// flag there), B = the per-vertex top-boundary flag (terrain puts per-chunk
	// climate there). M_WaterVoxel's World Position Offset consumes both to
	// lower a partial cell's surface to its fill height.
	//
	// WITHOUT this the pooled path would hand the material terrain's biome flag
	// and climate, drawing every water surface at full height -- silently
	// agreeing with the old >=128 behaviour while the component path, which is
	// the default, showed the stepped surface. Must precede proxy creation
	// (RegisterComponent below); the proxy takes a copy.
	Pool->SetWaterMode(true);
	// M_WaterVoxel. Without it the proxy silently falls back to the engine
	// default material, which is opaque -- water would render as grey boxes and
	// look like a geometry bug rather than a material one.
	Pool->SetChunkMaterial(Material);
	PoolOwner->SetRootComponent(Pool);
	Pool->RegisterComponent();

	// SetWorldLocation AFTER RegisterComponent, never SetRelativeLocation
	// before: SetRootComponent on a freshly NewObject'd component installs an
	// identity transform and redefines the actor's location as the world
	// origin, 84 km from anything worth drawing (invariant 2).
	//
	// THE REBASE IS THE BUCKET'S OWN CORNER, not the first brick that arrived.
	// That is what makes it derivable rather than remembered, and therefore what
	// makes an emptied bucket re-homable by moving the component and nothing
	// else. It also bounds every chunk-table origin to [0, 5120) UU on each
	// axis -- a bucket edge -- which is a far tighter float32 range than the
	// old first-brick rebase gave, since that one could be anywhere in the world
	// relative to the water that later joined it.
	Pool->SetWorldLocation(WaterPoolBucketRebase(BucketKey));

	Pool->InitPool(kWaterBucketCapacityQuads);
	return Pool;
}

UVoxelGpuPoolComponent* GetBucketPool(FVoxelWaterImpl& Impl, int32 BucketIndex)
{
	return Impl.PoolBuckets.IsValidIndex(BucketIndex) ? Impl.PoolBuckets[BucketIndex].Pool.Get() : nullptr;
}

} // namespace

// AT GLOBAL SCOPE, NOT IN THE ANONYMOUS NAMESPACE ABOVE, and that is forced
// rather than stylistic: FVoxelWaterImpl declares `struct FVoxelWaterPoolBatch*
// ActiveBatch`, and an elaborated type specifier inside a class declares the
// name in the nearest ENCLOSING namespace -- the global one, since
// FVoxelWaterImpl is a global-scope struct. Defining it in the anonymous
// namespace instead would create a second, unrelated type with the same
// spelling, and the assignment in the constructor would not compile.
//
// Opens one publication scope per live bucket, and keeps accepting new buckets
// for as long as it is alive. See FVoxelWaterImpl::ActiveBatch for why the
// second half matters.
//
// The scopes are held by pointer in an array purely because
// UVoxelGpuPoolComponent::FScopedBatch is deliberately non-copyable and
// non-movable (its depth counter is the whole mechanism), so it cannot live in
// a TArray by value.
struct FVoxelWaterPoolBatch
{
	explicit FVoxelWaterPoolBatch(FVoxelWaterImpl& InImpl)
		: Impl(InImpl)
	{
		for (const FVoxelWaterImpl::FPoolBucket& Bucket : Impl.PoolBuckets)
		{
			if (UVoxelGpuPoolComponent* Pool = Bucket.Pool.Get())
			{
				Scopes.Emplace(MakeUnique<UVoxelGpuPoolComponent::FScopedBatch>(Pool));
			}
		}
		// LAST, so a bucket created by anything above cannot be adopted twice.
		Impl.ActiveBatch = this;
	}

	~FVoxelWaterPoolBatch()
	{
		// Cleared BEFORE the scopes close: ~FScopedBatch flushes, a flush can in
		// principle reach code that creates geometry, and adopting a bucket into
		// a batch that is already unwinding would open a scope nothing closes.
		Impl.ActiveBatch = nullptr;
		Scopes.Empty();
	}

	FVoxelWaterPoolBatch(const FVoxelWaterPoolBatch&) = delete;
	FVoxelWaterPoolBatch& operator=(const FVoxelWaterPoolBatch&) = delete;

	void Adopt(UVoxelGpuPoolComponent* Pool)
	{
		if (Pool != nullptr)
		{
			Scopes.Emplace(MakeUnique<UVoxelGpuPoolComponent::FScopedBatch>(Pool));
		}
	}

	FVoxelWaterImpl& Impl;
	TArray<TUniquePtr<UVoxelGpuPoolComponent::FScopedBatch>> Scopes;
};

namespace
{

// Routes a brick to its sort bucket, creating or re-homing a pool primitive if
// it needs one. Returns INDEX_NONE only if no primitive could be produced at
// all, which the caller treats exactly like a full pool: the brick goes
// undrawn rather than half-drawn.
int32 GetOrCreateWaterPoolBucket(FVoxelWaterImpl& Impl, AActor* ChunkOwner, UMaterialInterface* Material,
                                 const VoxelCoords::FVoxelCoord& BrickCoord)
{
	const VoxelCoords::FVoxelCoord BucketKey = WaterPoolBucketKey(BrickCoord);
	if (const int32* Existing = Impl.PoolBucketIndex.Find(BucketKey))
	{
		if (GetBucketPool(Impl, *Existing) != nullptr)
		{
			return *Existing;
		}
		// The component was garbage collected out from under us (world teardown
		// races, an editor PIE stop). Drop the mapping and fall through to make a
		// new one rather than handing back an index whose pool is gone.
		Impl.PoolBucketIndex.Remove(BucketKey);
	}

	// RE-HOME AN EMPTY BUCKET BEFORE ALLOCATING A NEW ONE. Water follows the
	// camera; buckets behind it drain to zero live entries and would otherwise
	// sit there holding 4 MB and a scene primitive for the rest of the session.
	// An empty pool has no chunk-table entry, no run and no live quad naming its
	// old rebase, so moving it is a transform change and nothing else.
	for (int32 I = 0; I < Impl.PoolBuckets.Num(); ++I)
	{
		UVoxelGpuPoolComponent* Pool = Impl.PoolBuckets[I].Pool.Get();
		if (Pool == nullptr || Pool->GetNumChunks() != 0)
		{
			continue;
		}
		Impl.PoolBucketIndex.Remove(Impl.PoolBuckets[I].Key);
		Impl.PoolBuckets[I].Key = BucketKey;
		Impl.PoolBuckets[I].Rebase = WaterPoolBucketRebase(BucketKey);
		Pool->SetWorldLocation(Impl.PoolBuckets[I].Rebase);
		Impl.PoolBucketIndex.Add(BucketKey, I);
		UE_LOG(LogVoxelWater, Verbose,
		       TEXT("voxel.Water.GPU: re-homed empty water bucket %d to (%lld,%lld,%lld)"),
		       I, (long long)BucketKey.X, (long long)BucketKey.Y, (long long)BucketKey.Z);
		return I;
	}

	if (Impl.PoolBuckets.Num() < kMaxWaterPoolBuckets)
	{
		UVoxelGpuPoolComponent* Pool = SpawnWaterPoolPrimitive(ChunkOwner, Material, BucketKey);
		if (Pool == nullptr)
		{
			return INDEX_NONE;
		}
		FVoxelWaterImpl::FPoolBucket Bucket;
		Bucket.Key = BucketKey;
		Bucket.Pool = Pool;
		Bucket.Rebase = WaterPoolBucketRebase(BucketKey);
		const int32 Index = Impl.PoolBuckets.Add(Bucket);
		Impl.PoolBucketIndex.Add(BucketKey, Index);
		// A bucket born mid-loop joins the loop's publication scope. Without
		// this, the first tick that reaches a new region publishes once per
		// brick on it -- see FVoxelWaterImpl::ActiveBatch.
		if (Impl.ActiveBatch != nullptr)
		{
			Impl.ActiveBatch->Adopt(Pool);
		}
		UE_LOG(LogVoxelWater, Log,
		       TEXT("voxel.Water.GPU: water bucket %d up at (%lld,%lld,%lld), %u quad capacity (%.1f MB), "
		            "rebase (%.0f,%.0f,%.0f). %d/%d buckets live."),
		       Index, (long long)BucketKey.X, (long long)BucketKey.Y, (long long)BucketKey.Z,
		       kWaterBucketCapacityQuads, double(kWaterBucketCapacityQuads) * 8.0 / (1024.0 * 1024.0),
		       Bucket.Rebase.X, Bucket.Rebase.Y, Bucket.Rebase.Z, Impl.PoolBuckets.Num(), kMaxWaterPoolBuckets);
		return Index;
	}

	// AT THE CAP. Fall back to the NEAREST live bucket rather than dropping the
	// brick. It draws in the right place with the wrong sort key, which is
	// precisely the pre-W5 behaviour for that brick and no worse.
	//
	// The chunk-table origin is then relative to a FOREIGN bucket's rebase, so
	// this does spend float32 precision it would not otherwise: nearest-bucket
	// keeps that offset as small as the live set allows, but after a teleport it
	// could be kilometres, where float32 resolves to ~centimetres rather than the
	// sub-millimetre a same-bucket brick enjoys. That is a visible-at-nothing
	// error on 10 cm voxels and is accepted; it is also self-correcting, since
	// the buckets behind the camera drain and the next brick re-homes one.
	int32 Best = INDEX_NONE;
	int64 BestDistSq = MAX_int64;
	for (int32 I = 0; I < Impl.PoolBuckets.Num(); ++I)
	{
		if (Impl.PoolBuckets[I].Pool.Get() == nullptr)
		{
			continue;
		}
		const int64 Dx = int64(Impl.PoolBuckets[I].Key.X) - int64(BucketKey.X);
		const int64 Dy = int64(Impl.PoolBuckets[I].Key.Y) - int64(BucketKey.Y);
		const int64 Dz = int64(Impl.PoolBuckets[I].Key.Z) - int64(BucketKey.Z);
		const int64 DistSq = Dx * Dx + Dy * Dy + Dz * Dz;
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = I;
		}
	}
	UE_LOG(LogVoxelWater, Warning,
	       TEXT("voxel.Water.GPU: %d water sort buckets live (cap %d); brick bucket (%lld,%lld,%lld) folded into "
	            "bucket %d. Translucent sorting degrades to pre-W5 for that region -- geometry is unaffected."),
	       Impl.PoolBuckets.Num(), kMaxWaterPoolBuckets, (long long)BucketKey.X, (long long)BucketKey.Y,
	       (long long)BucketKey.Z, Best);
	return Best;
}

// The pooled equivalent of "destroy this brick's component". Safe to call for a
// brick that was never pooled.
void ReleaseWaterBrickPooled(FVoxelWaterImpl& Impl,
                             TMap<VoxelCoords::FVoxelCoord, FVoxelWaterImpl::FPooledBrick>& Slots,
                             const VoxelCoords::FVoxelCoord& BrickCoord)
{
	FVoxelWaterImpl::FPooledBrick Slot;
	if (!Slots.RemoveAndCopyValue(BrickCoord, Slot) || Slot.Handle == INDEX_NONE)
	{
		return;
	}
	// Released through the bucket the handle was ISSUED BY, which the record
	// carries. Re-deriving it from BrickCoord would be wrong the moment a bucket
	// is re-homed: the brick's key would name a bucket that is now somewhere
	// else, and RemoveChunk would free a stranger's range.
	if (UVoxelGpuPoolComponent* Pool = GetBucketPool(Impl, Slot.Bucket))
	{
		Pool->RemoveChunk(Slot.Handle);
	}
}

// Puts one brick's quads into the pool, adding or re-meshing in place.
//
// Returns false if the pool refused the geometry, in which case the brick is
// left undrawn rather than half-drawn -- and the caller must NOT then fall back
// to a component for it, because a brick drawn twice is worse than a brick
// drawn once.
bool ApplyWaterBrickPooled(FVoxelWaterImpl& Impl, AActor* ChunkOwner, UMaterialInterface* Material,
                           TMap<VoxelCoords::FVoxelCoord, FVoxelWaterImpl::FPooledBrick>& Slots,
                           const VoxelCoords::FVoxelCoord& BrickCoord,
                           const FVector& BrickOriginUU,
                           const TArray<FVoxelChunkQuad>& Quads,
                           const TArray<uint32>& CornerHeights,
                           float Activity)
{
	// One packed corner word per quad, always -- EmitWaterQuads fills both arrays
	// in lockstep, including on the split path. The pool indexes them by the same
	// slot offset, so a length mismatch would silently shift every corner height
	// by however many quads had been dropped.
	check(CornerHeights.Num() == Quads.Num());

	// A RESIDENT BRICK STAYS IN THE BUCKET THAT ISSUED ITS HANDLE, even if that
	// bucket has since been re-homed elsewhere. Moving it would mean a free in
	// one pool and an alloc in another with the geometry visible in neither for
	// a frame, to fix a sort key -- and a brick only ends up in a foreign bucket
	// via the at-cap fallback, which is a degraded case by construction.
	FVoxelWaterImpl::FPooledBrick* ExistingSlot = Slots.Find(BrickCoord);
	const bool bResident = ExistingSlot != nullptr && ExistingSlot->Handle != INDEX_NONE
		&& GetBucketPool(Impl, ExistingSlot->Bucket) != nullptr;

	const int32 BucketIndex = bResident
		? ExistingSlot->Bucket
		: GetOrCreateWaterPoolBucket(Impl, ChunkOwner, Material, BrickCoord);
	UVoxelGpuPoolComponent* Pool = GetBucketPool(Impl, BucketIndex);
	if (Pool == nullptr)
	{
		return false;
	}

	// The CPU mesher's quads in the 8 bytes the pool stores. Water bricks are
	// already 0..7 on every axis, so unlike terrain there is no brick-to-chunk
	// rebase to bake in first -- brick-local IS entry-local here.
	TArray<uint64> Packed;
	Packed.SetNumUninitialized(Quads.Num());
	for (int32 I = 0; I < Quads.Num(); ++I)
	{
		Packed[I] = PackVoxelChunkQuad(Quads[I]);
	}

	// Params: xy = terrain's climate pair, z = terrain's surface gate, w spare.
	// Water uses exactly ONE of them.
	//
	// .y IS THE FOAM ACTIVITY (W5). The vertex factory writes ChunkParams.y
	// straight into vertex colour A under every mode
	// (VoxelQuadVertexFactory.ush: `Intermediates.Color = half4(..., ChunkClimate.y)`),
	// so this reaches M_WaterVoxel with ZERO shader plumbing -- no new uniform
	// member, and therefore no exposure to the loose-FShaderParameter trap that
	// silently no-ops in this vertex factory (docs/gpu-g2-draw-path.md).
	// .x stays at terrain's neutral midpoint and .z at kNoSurfaceGate
	// ("everything counts as surface"), which is the same well-defined fallback
	// the pool gives its own hidden entry; neither reaches this material.
	const FVector4f Params(0.5f, FMath::Clamp(Activity, 0.0f, 1.0f),
	                       UVoxelGpuPoolComponent::kNoSurfaceGate, 0.0f);

	int32 NewSlot;
	if (bResident)
	{
		// PARAMS FIRST, THEN THE RE-MESH. SetChunkParams only marks the chunk
		// table dirty and defers; UpdateChunk's own PushUpdatesToProxy then
		// carries the new row in the same publication. The other order would
		// leave activity trailing the geometry by one flush -- which, for a
		// brick that just went still, means one tick of foam on settled water.
		Pool->SetChunkParams(ExistingSlot->Handle, Params);
		// Re-mesh in place. This is water's COMMON case, not its rare one: every
		// active brick re-meshes at the 10 Hz CA cadence, so free+realloc on each
		// tick would fragment the pool far faster than terrain digging does.
		NewSlot = Pool->UpdateChunk(ExistingSlot->Handle, Packed, CornerHeights);
	}
	else
	{
		NewSlot = Pool->AddChunk(
			Packed, FVector3f(BrickOriginUU - Impl.PoolBuckets[BucketIndex].Rebase), /*Level=*/0, Params,
			&CornerHeights);
	}

	if (NewSlot == INDEX_NONE)
	{
		// Out of contiguous room. Drop the mapping so the brick is honestly
		// undrawn rather than pointing at a range that is no longer its own.
		Slots.Remove(BrickCoord);
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("voxel.Water.GPU: no room for %d quads in bucket %d (%u free, largest run %u). "
		            "Brick left undrawn."),
		       Packed.Num(), BucketIndex, Pool->GetFreeQuads(), Pool->GetLargestFreeRun());
		return false;
	}

	Slots.Add(BrickCoord, FVoxelWaterImpl::FPooledBrick{BucketIndex, NewSlot});
	return true;
}

// Doctrine SS2.5 ("everything expensive is budgeted, never demand-driven"):
// per-brick vxc::meshBrick<8> (re-mesh of an existing component OR meshing
// for a brand-new one -- both cost the same greedy-mesh pass) is the
// expensive per-tick operation, NOT just NewObject+RegisterComponent as
// first assumed. Verification testing measured this progressively:
//   1. Every apron sample routed through vxc::WaterCA::fillAt()'s full
//      hash+unordered_map lookup: ~90 active bricks cost 10-40ms/tick.
//   2. Fixed by indexing the already-fetched WaterBrick8 directly for
//      in-bounds samples (see the Sampler below) -- but a NEW-component
//      creation cap alone (kMaxNewComponentsPerTick, tried at 32 then 4)
//      still left 15-69ms spikes: re-meshing ~90-150 ALREADY-EXISTING
//      components every tick (uncapped) was the remaining cost, not
//      creation.
// Fix: ONE unified per-tick budget over the actual expensive operation
// (the meshBrick<8> call itself), covering both re-mesh and create alike;
// only cheap destroy-on-empty is unbudgeted (see below). A sustained flood
// larger than this budget spreads its update cost over several 10Hz ticks
// (each dirty-but-deferred brick keeps its last-known mesh on screen for
// one extra tick at most) instead of spiking one of them.
constexpr int32 kMaxBrickMeshesPerTick = 8;

// The smallest CA fill a cell must hold before it is meshed at all.
//
// The meshing sampler returns the fill fraction itself as the vxc::MaterialId
// (see the Sampler lambda below), and vxc::MAT_AIR is 0, so ANY nonzero fill
// would otherwise produce geometry. At the extreme that means a cell holding
// 1/255 units emits a full quad whose top face M_WaterVoxel then pushes down
// to 0.4 mm above the floor -- six vertices and a translucent draw for
// something thinner than the mesher's own coordinate resolution.
//
// This is a RENDERING floor only. The CA still simulates, conserves and
// replicates those units exactly as before; they are simply not drawn until
// there is enough to see. That distinction matters because the alternative --
// clamping the fill itself -- would destroy volume, which is the one thing
// waterca.h's ledger is built to make impossible.
//
// 8/255 is ~3% of a voxel, i.e. ~3 mm at kVoxelSizeMm = 100 -- below what a
// player can notice missing, above the sliver regime described here.
//
// ONE THING TO REVISIT IF THE SWE COUPLER IS EVER ENABLED (ADR-0004): a
// settled SWE surface is flat only to its derived +/-16-unit deadband, so a
// cell sitting near this floor could cross it repeatedly and flicker in and
// out of the mesh. Hysteresis (or a floor below the deadband) would be the fix.
// The CA alone settles to +/-1 unit, so the problem does not arise today.
constexpr uint8_t kMinVisibleFill = 8;

// Height quantisation for the MERGE byte only (see QuantiseFillForMesh).
//
// Fill-as-material makes the mesher's merge key carry the fill value, which is
// exactly what splits faces of differing depth -- the property that produces a
// stepped surface. The cost is that it also refuses to merge cells that differ
// by a single unit, and the CA settles "flat within +/-1", so a settled pool
// fragments toward one quad per surface cell. MEASURED on the standard
// 30,000-unit pour: 365 quads through the old fill>=128 binarisation vs 984
// with raw fill-as-material, i.e. 2.7x, and that multiple scales with the
// water body rather than being a fixed cost.
//
// Snapping the merge byte to 8-unit buckets makes those +/-1 neighbours agree
// again while costing ~3.2 mm of height resolution (8/255 of a 10 cm voxel) --
// still ~30x finer than the 10 cm lattice, and far below anything visible.
// Occupancy is deliberately NOT quantised: the kMinVisibleFill test upstream
// still sees the exact fill, so thin water does not change visibility.
constexpr uint8_t kFillMergeBucket = 8;

// Near-full cells snap UP to 255 rather than down to a bucket edge, so the
// interior of a settled body (which sits at 254-255) merges into one large
// quad instead of splitting against a 248 bucket.
constexpr uint8_t kFillFullSnap = 250;

inline vxc::MaterialId QuantiseFillForMesh(uint8_t Fill)
{
	if (Fill >= kFillFullSnap)
	{
		return vxc::MaterialId(255);
	}
	const uint8_t Bucketed = uint8_t(Fill & ~(kFillMergeBucket - 1));
	// Never quantise a visible cell down below the visibility floor -- that
	// would delete thin water the exact-fill test just admitted.
	return vxc::MaterialId(Bucketed < kMinVisibleFill ? kMinVisibleFill : Bucketed);
}

// --- C0 BILINEAR WATER SURFACE ----------------------------------------------
//
// WHAT THIS REPLACES. The stepped surface above seats each surface cell's top
// face at ITS OWN fill height, which is correct per cell and wrong per body:
// the height is constant across a face and jumps at every cell boundary, so a
// settled pool reads as a field of 10 cm plateaus rather than a water surface.
// At the CA's own +/-1-unit settle tolerance those jumps are invisible, but a
// pour, a wave front or a draining lake routinely holds neighbours 30-80 fill
// units apart, and that is 1-3 cm of vertical step every 10 cm laterally.
//
// AND IT CLOSES AN ACTUAL HOLE, not only a cosmetic one. voxel-core's mesher
// emits NO face between two occupied cells (mesher.h: the face mask requires
// !solidAt(neighbour)), so two adjacent partial cells of different fill get
// tops at different heights with no riser between them -- an open slit up to a
// whole voxel tall, straight through to the water's interior, on exactly the
// active cascades where the fill difference is largest. That is not fixable by
// tuning the step size; it needs the two tops to MEET.
//
// THE FIX IS PER-CORNER HEIGHTS, SHARED BETWEEN NEIGHBOURS. Each vertex of a
// water face sits on a LATTICE CORNER of the (x,y) grid, where four columns
// meet. Give that corner the average of those four columns' surface heights and
// two adjacent cells necessarily agree along their shared edge, because they
// average the same four columns -- so the surface is C0 by construction and the
// slit cannot exist. Interpolation across the quad then does the rest: the top
// is a bilinear patch instead of a plateau.
//
// The height still rides VertexColor.R through M_WaterVoxel's existing World
// Position Offset ((1 - R) * one voxel down, gated on the per-vertex
// top-boundary flag in B). Nothing about that formula changes; R simply becomes
// PER-CORNER rather than per-face. So the material asset is untouched, and both
// render paths keep the encoding they already share.
//
// WHAT IS DELIBERATELY NOT DONE: voxel-core is not modified. The mesher still
// merges by (material | ao | visible) and knows nothing about corners; where a
// merged quad cannot represent the corner field (see EmitWaterQuads) it is
// split back into unit quads HERE, on the presentation side, so determinism and
// the CPU/GPU mesher parity contract are untouched.

// The padded fill block one brick's mesh is built from: the brick's own 8^3
// cells plus the 1-voxel apron vxc::meshBrick documents as its sampler domain
// ([-1, B] on every axis).
//
// MATERIALISED ONCE PER BRICK, and that costs nothing extra. meshBrick ALREADY
// materialises exactly this block before its first face scan (see its own
// "Performance note"), calling the sampler once per padded cell -- so building
// it one level up and handing the mesher a lambda that reads it back is the
// same 1,000 source reads per brick, not a second pass over the CA.
//
// It has to exist because the corner heights below need the SAME cells the
// mesher needed, in a different order (per lattice corner, not per face) and
// several times each. Re-deriving them through a live sampler would put
// vxc::WaterCA::fillAt()'s hash + unordered_map lookup back on every apron
// sample -- the exact cost the sampler's own "index the already-fetched brick"
// comment records as 10-40 ms/tick over ~90 active bricks.
// --- SWE union sampling (ADR-0004 "Renderer") -------------------------------
//
// THE REQUIREMENT. Once the coupler promotes a column, that column's water
// lives in the SHEET, not in CA cells -- measured: a 30,000-unit pour ends up
// 29,780 in the sheet and 220 in the CA. The CA sampler alone therefore draws
// 0.7% of the water and the pour looks like it evaporated. ADR-0004 states the
// visible surface must be the UNION of sheet depth and CA fill.
//
// WHY THIS IS A TERM IN THE CA SAMPLER AND NOT A THIRD RENDER SOURCE. The
// implicit-cavern-water path is a genuine second source with its own component
// and pool-slot maps, and that works because implicit-vs-CA is disjoint BY
// BRICK (mobilization is per brick). SWE-vs-CA is disjoint BY VOXEL WITHIN A
// COLUMN: an SWE column's sheet occupies [bed+1, bed+sheetScanVoxels] while the
// CA still owns everything at or below the bed, and may hold a rate-limited
// residue inside that same range. "This brick belongs to source X" cannot
// express that boundary, so a third parallel source would draw two translucent
// surfaces over one brick -- exactly the doubling MarkMobilizedBricksDirty
// exists to prevent on the implicit path.
//
// WHY THE SUM, NOT max(CA, SWE). The coupler's CA-side evacuation is rate
// limited (absorbPerTick), so a fast source leaves a bounded standing residue
// inside the sheet's z-range. Drawing max() under-draws by up to that residue;
// drawing both double-draws. Summing the residue into the depth and then
// suppressing the CA's own contribution over exactly that range is exact.
struct FSweColumnDepths
{
	static constexpr int32 kPad = vxc::WaterBrick8::kEdge + 2; // 10 columns per axis

	// Effective depth (fill units) and bed voxel z, per padded column. Bed is
	// only meaningful where bHasSheet.
	int32 EffDepth[kPad * kPad] = {};
	int32 Bed[kPad * kPad] = {};
	bool bHasSheet[kPad * kPad] = {};
	int32 ScanVoxels = 0;
	bool bAnySheet = false;

	int32 Index(int32 X, int32 Y) const { return (X + 1) + kPad * (Y + 1); }
};

// Resolves the sheet for the 10x10 columns a brick's padded block spans.
//
// PER COLUMN, NOT PER VOXEL. The union needs a sum over the sheet's whole
// z-range, and the pad has 1,000 cells across only 100 columns -- computing it
// per sample would repeat the same scan ten times per column. Building it once
// costs 100 columns x sheetScanVoxels (8) CA reads, the same order as the pad
// itself, and every pad cell then resolves in O(1).
void BuildSweColumnDepths(FSweColumnDepths& Out, const FVoxelWaterImpl& Impl, const vxc::BrickKey& Key)
{
	if (!Impl.SweSheet || !Impl.SweCoupler)
	{
		return;
	}
	// sheetScanVoxels is the COUPLER's knob (it bounds how far above the bed
	// the coupler scans for CA fill to absorb), not the grid's -- and it must
	// be read from there rather than duplicated, because the union's
	// suppression range has to be exactly the range the coupler absorbs over.
	// A local copy that drifted would either double-draw or lose a layer.
	Out.ScanVoxels = Impl.SweCoupler->config().sheetScanVoxels;

	for (int32 Cy = -1; Cy <= vxc::WaterBrick8::kEdge; ++Cy)
	{
		for (int32 Cx = -1; Cx <= vxc::WaterBrick8::kEdge; ++Cx)
		{
			const int64_t Vx = int64_t(Key.x) * vxc::WaterBrick8::kEdge + Cx;
			const int64_t Vy = int64_t(Key.y) * vxc::WaterBrick8::kEdge + Cy;
			if (!Impl.SweSheet->inBounds(Vx, Vy) || !Impl.SweCoupler->isSweColumn(Vx, Vy))
			{
				continue; // CA owns this column outright
			}

			const int32 Bed = Impl.SweSheet->bedAt(Vx, Vy);
			int64_t Eff = Impl.SweSheet->depthAt(Vx, Vy);
			// The CA residue inside the sheet's own range, folded in.
			for (int32 K = 1; K <= Out.ScanVoxels; ++K)
			{
				Eff += Impl.CA.fillAt(Vx, Vy, int64_t(Bed) + K);
			}

			const int32 I = Out.Index(Cx, Cy);
			Out.bHasSheet[I] = true;
			Out.Bed[I] = Bed;
			Out.EffDepth[I] = int32(FMath::Min<int64_t>(Eff, int64_t(Out.ScanVoxels) * 255));
			Out.bAnySheet = true;
		}
	}
}

// The per-voxel union. CaFill is what the CA sampler would have returned.
//
// Stacking convention is WaterCA::addWater's and SweCaCoupler::demote()'s, and
// deliberately so: a demotion writes the sheet back into CA cells with exactly
// this layout, so hand-back is invisible instead of a pop.
uint8 UnionSweFill(const FSweColumnDepths& Sheet, int32 PadX, int32 PadY, int64_t Vz, uint8 CaFill)
{
	if (!Sheet.bAnySheet)
	{
		return CaFill;
	}
	const int32 I = Sheet.Index(PadX, PadY);
	if (!Sheet.bHasSheet[I])
	{
		return CaFill;
	}

	const int64_t K = Vz - int64_t(Sheet.Bed[I]); // 1 == first voxel above the bed
	if (K < 1 || K > Sheet.ScanVoxels)
	{
		// At or below the bed, or above the sheet's reach: CA owns it outright.
		return CaFill;
	}
	// Inside the sheet's range the CA's own contribution is SUPPRESSED -- it was
	// already summed into EffDepth, and returning it here as well is the
	// double-draw this whole structure exists to avoid.
	const int64_t Remaining = int64_t(Sheet.EffDepth[I]) - 255 * (K - 1);
	const uint8 Slice = uint8(FMath::Clamp<int64_t>(Remaining, 0, 255));

	// THE FLICKER FIX (the revisit kMinVisibleFill's comment asks for).
	//
	// A settled SWE surface is flat only to the coupler's derived +/-16-unit
	// deadband, against the CA's +/-1. Thresholding the SLICE at kMinVisibleFill
	// = 8 therefore has a cell whose remainder sits near 8 crossing the floor
	// every tick, appearing and vanishing -- and no floor value fixes that,
	// because an oscillation of +/-16 crosses ANY single threshold it straddles.
	// Per-cell hysteresis would need a 512-bit "currently drawn" mask per brick
	// and a band wider than the deadband to actually be stable.
	//
	// The cheaper and more honest fix is to move the DECISION to the quantity
	// that is stable. The unstable number is the top slice's remainder, which is
	// a modulus and lives in [0,255) no matter how much water the column holds.
	// The column's total depth is not: a column holding 100 units still reads
	// 84-116 under the same deadband, nowhere near the floor. So visibility is
	// decided ONCE PER COLUMN on EffDepth, and a column that qualifies draws its
	// top slice at the floor rather than dropping it.
	//
	// Costs at most 8/255 of a voxel (~3 mm) of over-draw on that one slice, and
	// only where the true remainder was already below what a player can see. No
	// volume is touched: this is the render path, and the ledger never sees it.
	if (Slice > 0 && Slice < kMinVisibleFill && Sheet.EffDepth[I] >= kMinVisibleFill)
	{
		return kMinVisibleFill;
	}
	return Slice;
}

struct FWaterFillPad
{
	static constexpr int32 kEdge = vxc::WaterBrick8::kEdge;   // 8
	static constexpr int32 kPad = kEdge + 2;                  // 10: [-1 .. 8]

	uint8 Fill[kPad * kPad * kPad];

	// Brick-local, and -1 / kEdge (the apron) are legal on every axis.
	uint8 At(int32 X, int32 Y, int32 Z) const
	{
		checkSlow(X >= -1 && X <= kEdge && Y >= -1 && Y <= kEdge && Z >= -1 && Z <= kEdge);
		return Fill[(X + 1) + kPad * ((Y + 1) + kPad * (Z + 1))];
	}
};

// Fills a pad from any per-cell RAW fill source, exactly once per padded cell.
// The two producers (the CA's WaterBrick8 + apron, and the mobilizer's implicit
// flood field) differ only in this functor.
template <typename FillFn>
void BuildWaterFillPad(FWaterFillPad& OutPad, const FillFn& Fn)
{
	int32 Idx = 0;
	for (int32 Z = -1; Z <= FWaterFillPad::kEdge; ++Z)
	{
		for (int32 Y = -1; Y <= FWaterFillPad::kEdge; ++Y)
		{
			for (int32 X = -1; X <= FWaterFillPad::kEdge; ++X)
			{
				// Same index order FWaterFillPad::At computes, so this is a
				// straight append rather than a scattered write.
				OutPad.Fill[Idx++] = Fn(X, Y, Z);
			}
		}
	}
}

// The water surface height at every LATTICE CORNER of the brick, for every cell
// layer, as a 0..255 fraction of THAT layer's own cell.
//
// 9 x 9 x 8 = 648 bytes, built once per brick and then read by every quad. The
// alternative -- computing four corners per quad on demand -- re-derives the
// same lattice point up to four times (once per touching face) and, worse,
// makes the split test below quadratic in the merge size.
//
// WHY THE HEIGHT IS EXPRESSED WITHIN A CELL rather than as an absolute Z. The
// quad packing cannot carry a fractional face position (VoxelQuadDecode.ush
// computes FaceCoordVox = Slice + Positive in integers), so the height must
// reach the material as the 0..1 WPO drop it already understands, which is
// cell-relative by definition. That clamp is also what keeps the surface
// watertight where it crosses a cell boundary: a corner whose true height is in
// the cell ABOVE clamps to 255 (this cell's ceiling) while the cell above
// clamps it to its own fraction, and the riser between them is a real side face
// -- the mesher emits one there precisely because the cell beside it is air.
struct FWaterCornerField
{
	static constexpr int32 kLat = vxc::WaterBrick8::kEdge + 1;      // 9 corners per axis
	static constexpr int32 kLayers = vxc::WaterBrick8::kEdge;       // 8 cell layers

	uint8 H[kLat * kLat * kLayers];

	uint8 At(int32 Lx, int32 Ly, int32 Cz) const
	{
		checkSlow(Lx >= 0 && Lx < kLat && Ly >= 0 && Ly < kLat && Cz >= 0 && Cz < kLayers);
		return H[Lx + kLat * (Ly + kLat * Cz)];
	}
};

void BuildWaterCornerField(const FWaterFillPad& Pad, FWaterCornerField& Out)
{
	for (int32 Cz = 0; Cz < FWaterCornerField::kLayers; ++Cz)
	{
		for (int32 Ly = 0; Ly < FWaterCornerField::kLat; ++Ly)
		{
			for (int32 Lx = 0; Lx < FWaterCornerField::kLat; ++Lx)
			{
				// The four columns meeting at this lattice corner. Every read is
				// in [-1, 8] on each axis (Lx-1 >= -1, Lx <= 8, Cz+1 <= 8), i.e.
				// inside the pad the mesher already required.
				int32 Sum = 0;
				int32 Count = 0;
				for (int32 Dy = -1; Dy <= 0; ++Dy)
				{
					for (int32 Dx = -1; Dx <= 0; ++Dx)
					{
						const int32 Cx = Lx + Dx;
						const int32 Cy = Ly + Dy;

						// This column continues above the layer, so its surface is
						// not here: it is full to this cell's ceiling.
						if (Pad.At(Cx, Cy, Cz + 1) >= kMinVisibleFill)
						{
							Sum += 255;
							++Count;
							continue;
						}

						const uint8 F = Pad.At(Cx, Cy, Cz);
						if (F >= kMinVisibleFill)
						{
							Sum += int32(F);
							++Count;
						}
						// ELSE: A DRY COLUMN CONTRIBUTES NOTHING -- it does not
						// average in as height 0. This is the difference between a
						// pool that meets its containing wall flat and one whose
						// surface droops to the floor at every boundary. Water
						// against rock has no lower neighbour to blend toward: the
						// absent column is absent, not empty. A genuine shoreline
						// still tapers, because there the neighbouring columns hold
						// real, smaller fills and DO average in.
					}
				}

				// Count is >= 1 for every corner any quad can reference (a quad's
				// own cell is always one of the four columns of each of its
				// corners), so the fallback is unreachable from the emit path. 255
				// -- "full height, do not move this vertex" -- is the harmless
				// direction if it ever is reached.
				Out.H[Lx + FWaterCornerField::kLat * (Ly + FWaterCornerField::kLat * Cz)] =
					Count > 0 ? uint8((Sum + Count / 2) / Count) : uint8(255);
			}
		}
	}
}

// Which lattice corner of the surface field a point on a face plane maps to.
//
// Uc/Vc are the quad's own in-plane coordinates (u = (axis+1)%3, v =
// (axis+2)%3). Only ever called for a TOP-boundary point, which is what makes
// the Vc-1 / Uc-1 below safe: those subtract 1 from a Z LATTICE coordinate to
// name the cell layer underneath it, and a top-boundary point always has that
// coordinate >= 1.
uint8 CornerHeightAtFacePoint(const FWaterCornerField& Field, int32 Axis, int32 Positive, int32 Slice,
                              int32 Uc, int32 Vc)
{
	if (Axis == 2)
	{
		// u = x, v = y; the whole quad lies in one cell layer.
		return Field.At(Uc, Vc, Slice);
	}
	if (Axis == 0)
	{
		// u = y, v = z. The face plane is at x = Slice + Positive.
		return Field.At(Slice + Positive, Uc, Vc - 1);
	}
	// Axis == 1: u = z, v = x. The face plane is at y = Slice + Positive.
	return Field.At(Vc, Slice + Positive, Uc - 1);
}

// Which of a quad's four corners sit on the TOP (+Z) boundary of the cell the
// face belongs to, as a bitmask over the corner order the two proxies and
// VoxelQuadDecode.ush all share: 0=(u0,v0) 1=(u0,v1) 2=(u1,v1) 3=(u1,v0).
//
// This is the same question FWaterChunkSceneProxy's bTopCorner and
// DecodeVoxelQuadVertex's TopBoundary answer, expressed against the quad fields
// instead of against decoded positions -- all three must agree, because B (the
// gate) comes from those two and R (the height) comes from this one.
uint8 TopCornerMaskFor(int32 Axis, int32 Positive)
{
	if (Axis == 2)
	{
		return Positive != 0 ? 0xFu : 0x0u;   // one plane: all four, or none
	}
	if (Axis == 0)
	{
		return 0x6u;   // v carries z -> corners 1 (u0,v1) and 2 (u1,v1)
	}
	return 0xCu;       // axis 1: u carries z -> corners 2 (u1,v1) and 3 (u1,v0)
}

// The quad's four corner heights, packed one byte per corner in that same
// position order (byte 0 = corner 0). NOT the AO order -- see
// VoxelQuadDecode.ush's AoShiftForCorner, which exists precisely because
// vxc::detail::aoCorner packs (0,0),(1,0),(0,1),(1,1). Choosing position order
// here means the shader's shift is Corner*8 with no remap table, and the one
// place the two orders could be confused is this comment.
//
// A corner that is NOT on the top boundary keeps the quad's own `mat` byte,
// which is exactly what R held for every vertex before this change. The WPO is
// gated to zero on those vertices, so the value is unused either way -- but
// preserving it means a bottom or lower-side vertex's R is byte-identical to
// the stepped-surface build, and any future reader of R sees no new case.
uint32 PackQuadCornerHeights(const FWaterCornerField& Field, const FVoxelChunkQuad& Q)
{
	const uint8 TopMask = TopCornerMaskFor(Q.Axis, Q.Positive);
	const int32 U0 = int32(Q.U0), V0 = int32(Q.V0);
	const int32 U1 = U0 + int32(Q.W), V1 = V0 + int32(Q.H);
	const int32 Us[4] = {U0, U0, U1, U1};
	const int32 Vs[4] = {V0, V1, V1, V0};

	uint32 Packed = 0;
	for (int32 C = 0; C < 4; ++C)
	{
		const uint8 Byte = (TopMask & (1u << C)) != 0
			? CornerHeightAtFacePoint(Field, Q.Axis, Q.Positive, Q.Slice, Us[C], Vs[C])
			: Q.Mat;
		Packed |= uint32(Byte) << (8 * C);
	}
	return Packed;
}

// How far, in 1/255ths of a voxel, an interior lattice corner may deviate from
// what a merged quad's own interpolation produces before the quad is split.
//
// 2/255 of a 10 cm voxel is 0.8 mm. The merge itself already accepts 8/255
// (3.2 mm) of height error by quantising the merge key (kFillMergeBucket), so
// this is well inside the error budget the stepped surface already spends -- it
// exists to catch REAL discontinuities (a pool edge, a wall, a wave front),
// not the +/-1-unit noise of a settled body, which would otherwise split every
// quad in a still pool and cost the merge for nothing.
constexpr int32 kCornerPlanarToleranceUnits = 2;

// Can this quad's interpolation reproduce the corner field across its whole
// extent?
//
// THE QUESTION MATTERS BECAUSE A MERGED QUAD HAS NO INTERIOR VERTICES. Its four
// corners are its only samples, so a 4-wide surface quad draws a straight line
// between the lattice heights at its two ends and ignores the three in between
// -- while the neighbouring quads along that edge (a +Z face of the next cell
// layer, or the side face of the cell beyond it) DO have vertices there. Where
// the field is not planar over the merge, that is a crack: the exact defect
// per-corner heights are here to remove.
//
// Two triangles interpolate a PLANE exactly and a general bilinear patch only
// when the corners happen to be coplanar (a triangle pair is linear on each
// half of the A-C diagonal), so the test is planarity, evaluated in integers
// against every lattice point the quad spans:
//
//   Pred(i,j) = A + (D - A) * i/w + (B - A) * j/h        [A=c0 B=c1 C=c2 D=c3]
//
// scaled by w*h to stay exact. Corner C is itself one of the lattice points
// tested (i=w, j=h), so 4-corner coplanarity is included rather than assumed.
bool QuadInterpolationCoversCornerField(const FWaterCornerField& Field, const FVoxelChunkQuad& Q)
{
	const int32 U0 = int32(Q.U0), V0 = int32(Q.V0);
	const int32 W = int32(Q.W), H = int32(Q.H);

	if (Q.Axis == 2)
	{
		if (Q.Positive == 0 || (W == 1 && H == 1))
		{
			return true;   // no top-boundary corners, or nothing between them
		}
		const int32 A = int32(Field.At(U0, V0, Q.Slice));
		const int32 B = int32(Field.At(U0, V0 + H, Q.Slice));
		const int32 D = int32(Field.At(U0 + W, V0, Q.Slice));
		const int32 Den = W * H;
		for (int32 J = 0; J <= H; ++J)
		{
			for (int32 I = 0; I <= W; ++I)
			{
				const int32 Actual = int32(Field.At(U0 + I, V0 + J, Q.Slice));
				const int32 Pred = A * Den + (D - A) * I * H + (B - A) * J * W;
				if (FMath::Abs(Actual * Den - Pred) > kCornerPlanarToleranceUnits * Den)
				{
					return false;
				}
			}
		}
		return true;
	}

	// A side face's top boundary is a single EDGE, so the test is 1D along
	// whichever in-plane axis is horizontal. The other axis carries z, and the
	// corners below the top edge never move at all -- a merged wall spanning
	// several cells vertically is still exact, which is what keeps a deep pool's
	// walls as cheap as they are today.
	if (Q.Axis == 0)
	{
		if (W == 1)
		{
			return true;
		}
		const int32 P0 = int32(CornerHeightAtFacePoint(Field, 0, Q.Positive, Q.Slice, U0, V0 + H));
		const int32 P1 = int32(CornerHeightAtFacePoint(Field, 0, Q.Positive, Q.Slice, U0 + W, V0 + H));
		for (int32 I = 0; I <= W; ++I)
		{
			const int32 Actual = int32(CornerHeightAtFacePoint(Field, 0, Q.Positive, Q.Slice, U0 + I, V0 + H));
			if (FMath::Abs(Actual * W - (P0 * W + (P1 - P0) * I)) > kCornerPlanarToleranceUnits * W)
			{
				return false;
			}
		}
		return true;
	}

	if (H == 1)
	{
		return true;
	}
	const int32 P0 = int32(CornerHeightAtFacePoint(Field, 1, Q.Positive, Q.Slice, U0 + W, V0));
	const int32 P1 = int32(CornerHeightAtFacePoint(Field, 1, Q.Positive, Q.Slice, U0 + W, V0 + H));
	for (int32 J = 0; J <= H; ++J)
	{
		const int32 Actual = int32(CornerHeightAtFacePoint(Field, 1, Q.Positive, Q.Slice, U0 + W, V0 + J));
		if (FMath::Abs(Actual * H - (P0 * H + (P1 - P0) * J)) > kCornerPlanarToleranceUnits * H)
		{
			return false;
		}
	}
	return true;
}

// Turns one brick's greedy-mesh output into the render arrays, splitting any
// quad whose merge the corner field cannot survive.
//
// OutCorners is PARALLEL to OutQuads, one packed uint32 per quad, and both
// paths carry it beside the quads rather than inside them: FVoxelChunkQuad and
// the 8-byte packed form are a contract shared with the GPU mesher and the
// terrain renderer (VoxelMeshTypes.h, VoxelQuadDecode.ush), and the packed word
// is full. A parallel array costs 4 bytes per quad on the water path only and
// changes nothing terrain touches.
//
// SPLIT SHAPES, and why they are not all "unit quads":
//   * +Z surface faces split both ways -- the corner field varies in x and y
//     and only a unit cell can carry all four of its corners.
//   * side faces split only along their HORIZONTAL axis. Their vertical extent
//     has no top-boundary corners below the top row, so a 1x8 wall strip is as
//     exact as eight 1x1 ones and eight times cheaper.
//
// AO SURVIVES THE SPLIT UNCHANGED IN PRACTICE, which is worth stating because
// splitting a greedy quad usually does not: the mesher's merge key includes the
// 4-corner AO byte, so every cell of a merged quad carried the SAME byte, and
// each sub-quad inherits it. The one real difference is that a non-uniform AO
// byte's corner-to-corner ramp is then repeated per cell instead of stretched
// across the merge. For water surfaces that cannot bite: AO here is occlusion
// by OTHER WATER (the mesher's solidAt reads the fill-as-material array), and a
// free surface has air above it, so a +Z water face's AO byte is 0xFF -- four
// equal corners, no ramp to repeat.
void EmitWaterQuads(const std::vector<vxc::Quad>& RawQuads, const FWaterCornerField& Field,
                    TArray<FVoxelChunkQuad>& OutQuads, TArray<uint32>& OutCorners)
{
	OutQuads.Reserve(int32(RawQuads.size()));
	OutCorners.Reserve(int32(RawQuads.size()));

	for (const vxc::Quad& RQ : RawQuads)
	{
		FVoxelChunkQuad CQ;
		CQ.Axis = RQ.axis;
		CQ.Positive = RQ.positive;
		CQ.Slice = RQ.slice;
		CQ.U0 = RQ.u0;
		CQ.V0 = RQ.v0;
		CQ.W = RQ.w;
		CQ.H = RQ.h;
		CQ.Ao = RQ.ao;
		CQ.Mat = RQ.mat;

		if (QuadInterpolationCoversCornerField(Field, CQ))
		{
			OutQuads.Add(CQ);
			OutCorners.Add(PackQuadCornerHeights(Field, CQ));
			continue;
		}

		const int32 SubW = (CQ.Axis == 1) ? int32(CQ.W) : 1;
		const int32 SubH = (CQ.Axis == 0) ? int32(CQ.H) : 1;
		for (int32 J = 0; J < int32(CQ.H); J += SubH)
		{
			for (int32 I = 0; I < int32(CQ.W); I += SubW)
			{
				FVoxelChunkQuad Sub = CQ;
				Sub.U0 = uint8(int32(CQ.U0) + I);
				Sub.V0 = uint8(int32(CQ.V0) + J);
				Sub.W = uint8(SubW);
				Sub.H = uint8(SubH);
				OutQuads.Add(Sub);
				OutCorners.Add(PackQuadCornerHeights(Field, Sub));
			}
		}
	}
}

// Rebuilds (or destroys) the render component for every brick in
// Impl.DirtyBricks, then clears that set (except for any budget-deferred
// entries, left in place for the next call). Shared by the authority tick
// path and the client replication-receive path (task item 4's mesher applies
// identically regardless of which side produced the fill data).
void RemeshDirtyBricks(FVoxelWaterImpl& Impl, AActor* ChunkOwner, USceneComponent* ChunkRoot, UMaterialInterface* Material)
{
	if (!ChunkOwner || !ChunkRoot)
	{
		// No viewport (NM_DedicatedServer) -- simulate only, never render;
		// skip the meshBrick<8> work entirely rather than pay its cost for
		// output nobody will ever see, and drop the backlog so it can't grow
		// unbounded over a long-running dedicated server session.
		Impl.DirtyBricks.Reset();
		return;
	}

	const TArray<VoxelCoords::FVoxelCoord> ToProcess = Impl.DirtyBricks.Array();
	Impl.DirtyBricks.Reset();
	int32 MeshesThisTick = 0;
	int32 DeferredCount = 0;

	// S1-1: the water pool is batched too, and this is a DELIBERATE decision
	// rather than an inherited one (docs/speculative-generation-plan.md asks for
	// it to be stated either way).
	//
	// BATCHED, for the same reason terrain is: this loop calls AddChunk /
	// UpdateChunk / RemoveChunk once per dirty brick, each of which ends in its
	// own PushUpdatesToProxy, and water re-meshes at 10 Hz with bricks appearing
	// and vanishing continuously -- the churn that made FreeChunkIds necessary in
	// the first place. Per-brick publication is exactly the tax S0 measured on
	// the terrain side.
	//
	// AND IT IS STRICTLY SAFER HERE. The whole hazard batching introduces is the
	// same-frame free-then-GPU-write race that UnmarkQuadsDirty exists to close,
	// and it needs a PENDING GPU WRITE to occur at all. Water only ever reaches
	// the pool through AddChunk/UpdateChunk -- the CPU path, which writes the
	// shadow and marks it dirty. Nothing in this subsystem calls AddChunkFromGpu,
	// so PendingGpuWrites is always empty for this instance and the race has no
	// way to happen. If that ever changes, this scope inherits the terrain fix
	// automatically, because the subtract lives in AddChunkFromGpu itself.
	//
	// The component path below (the non-pooled arm) is untouched by this: it owns
	// UWaterChunkComponents, not pool ranges.
	//
	// W5: ONE SCOPE PER BUCKET, and one that adopts buckets created inside the
	// loop. Splitting the pool into several primitives split the publication
	// with it, so a single scope over a single pool would silently have stopped
	// batching most of the work -- the failure mode being a perf regression with
	// no counter moving, since every brick would still land correctly.
	FVoxelWaterPoolBatch WaterPoolBatch(Impl);

	for (const VoxelCoords::FVoxelCoord& BrickCoord : ToProcess)
	{
		const vxc::BrickKey Key = ToBrickKey(BrickCoord);
		const vxc::WaterBrick8* Brick = Impl.CA.findBrick(Key);

		if (!Brick)
		{
			// Brick fully drained/collapsed (WaterMap's homogeneous-empty
			// collapse == absence, per waterca.h) -- drop its component
			// entirely. Cheap (no meshBrick call) -- unbudgeted, and capping
			// it would leak stale geometry.
			if (TObjectPtr<UWaterChunkComponent>* Existing = Impl.ChunkComponents.Find(BrickCoord))
			{
				if (*Existing)
				{
					(*Existing)->DestroyComponent();
				}
				Impl.ChunkComponents.Remove(BrickCoord);
			}
			// Dispatch on what the brick HOLDS, not on the cvar: a brick drawn
			// before a mid-session voxel.Water.GPU flip must unload the way it
			// loaded. Both calls are no-ops for a brick the other path owns.
			ReleaseWaterBrickPooled(Impl, Impl.PoolSlots, BrickCoord);
			continue;
		}

		if (MeshesThisTick >= kMaxBrickMeshesPerTick)
		{
			// Budget reached -- defer to the NEXT RemeshDirtyBricks call
			// rather than pay for every dirty brick's meshBrick<8> pass in
			// one frame. Whatever this brick's component currently shows
			// (its previous mesh, or nothing if brand new) stays as-is for
			// one extra tick at most (10Hz CA cadence >> this budget's
			// drain rate for any v0-scale event).
			Impl.DirtyBricks.Add(BrickCoord);
			++DeferredCount;
			continue;
		}
		++MeshesThisTick;

		// STEPPED FILL-FRACTION SURFACES (was: "fill>=128 solid for meshing
		// v0; surface cells with partial fill render at full cube v0 --
		// partial-height mesh is W5 polish").
		//
		// The sampler now returns THE FILL FRACTION ITSELF as the
		// vxc::MaterialId, which does three things at once:
		//
		//   1. OCCUPANCY. vxc::MAT_AIR is 0, so any cell at or above
		//      kMinVisibleFill is solid to the mesher and anything below it is
		//      air. This replaces the old >=128 threshold, under which a cell
		//      less than half full was invisible and a brick sitting at a
		//      uniform 100 fill meshed to zero quads and had its component
		//      destroyed outright (the "Fully solid or fully empty" branch
		//      below). Water popped in and out at the 50% mark.
		//   2. HEIGHT. The value rides the quad's `mat` byte through
		//      PackVoxelChunkQuad into VertexColor.R -- directly on the
		//      component path (FWaterChunkSceneProxy writes FColor(Q.Mat, ...)),
		//      and via FVoxelQuadVertexFactoryParameters::WaterMode on the
		//      pooled one. M_WaterVoxel's World Position Offset reads R and
		//      seats each surface cell's top face at its own fill height. The
		//      packing itself cannot carry a fractional height: the decode
		//      computes FaceCoordVox = Slice + (Positive ? 1 : 0) in integers
		//      (VoxelQuadDecode.ush), so WPO is the mechanism.
		//   3. MERGE CORRECTNESS, FOR FREE. vxc::meshBrick's greedy mask key is
		//      `material | ao<<8 | visible<<16`, so two faces merge only when
		//      their materials match. Putting fill in the material slot
		//      therefore makes the mesher refuse to merge cells of DIFFERENT
		//      fill automatically -- no new merge predicate, no mesher change.
		//      Interior and side faces (all at 255) keep merging exactly as
		//      before; only the surface layer fragments, which is precisely
		//      where the extra quads buy the stepped waterline.
		//
		// The old placeholder was MAT_ROCK, chosen because "water's translucent
		// material doesn't branch on it". That is still true -- M_WaterVoxel
		// reads R as a scalar height, never as a categorical id -- but the byte
		// is no longer arbitrary, so it must not be repurposed again without
		// updating the material.
		// Perf: meshBrick<8> samples a padded [-1,8] range per axis, but the
		// overwhelming majority of those samples land INSIDE this brick
		// (0..7 on every axis) -- for those, index straight into the
		// already-fetched Brick pointer instead of paying vxc::WaterCA::
		// fillAt()'s full waterKeyForVoxel-hash + unordered_map-lookup cost
		// per cell. Only the apron (a neighbor brick, or empty space) falls
		// back to fillAt(). Measured necessary in verification testing: with
		// every sample going through fillAt(), a single RemeshDirtyBricks
		// pass over ~90 active bricks cost 10-40ms/tick -- far over the v0
		// <2ms/frame budget -- purely from redundant hashmap lookups within
		// a brick whose contents were already in hand.
		//
		// The raw fill is materialised into the pad FIRST and the mesher reads it
		// back, rather than the sampler reaching into the brick directly. Same
		// number of source reads (meshBrick materialises this exact block itself
		// before its first face scan), and it is what lets the corner field below
		// re-read the same cells per lattice corner without touching the CA again.
		FWaterFillPad Pad;
		// The sheet for this brick's columns, resolved once (see the union
		// comment above). No sheet armed, or no promoted column here, and this
		// is inert: bAnySheet stays false and UnionSweFill returns CA fill
		// unchanged, so the un-armed path is byte-identical to before.
		FSweColumnDepths Sheet;
		BuildSweColumnDepths(Sheet, Impl, Key);

		BuildWaterFillPad(Pad, [&Impl, &Key, Brick, &Sheet](int32 x, int32 y, int32 z) -> uint8
		{
			uint8 CaFill;
			if (x >= 0 && x < vxc::WaterBrick8::kEdge && y >= 0 && y < vxc::WaterBrick8::kEdge && z >= 0 && z < vxc::WaterBrick8::kEdge)
			{
				CaFill = Brick->get(x, y, z);
			}
			else
			{
				const int64_t Vx = int64_t(Key.x) * vxc::WaterBrick8::kEdge + x;
				const int64_t Vy = int64_t(Key.y) * vxc::WaterBrick8::kEdge + y;
				const int64_t Vz = int64_t(Key.z) * vxc::WaterBrick8::kEdge + z;
				CaFill = Impl.CA.fillAt(Vx, Vy, Vz);
			}
			// Folding the union in HERE, at the pad, rather than at the mesher's
			// sampler, is what gives SWE water the bilinear corner surface for
			// free -- the corner field reads this same pad.
			const int64_t Vz = int64_t(Key.z) * vxc::WaterBrick8::kEdge + z;
			return UnionSweFill(Sheet, x, y, Vz, CaFill);
		});

		std::vector<vxc::Quad> RawQuads;
		const auto Sampler = [&Pad](int x, int y, int z) -> vxc::MaterialId
		{
			const uint8 Fill = Pad.At(x, y, z);
			return Fill >= kMinVisibleFill ? QuantiseFillForMesh(Fill) : vxc::MAT_AIR;
		};
		vxc::meshBrick<vxc::WaterBrick8::kEdge>(Sampler, RawQuads);

		// Per-corner surface heights, and the merge splits they force. Skipped
		// entirely for a brick that meshed to nothing -- which is most of them in
		// a large body, since interior bricks emit no faces at all.
		TArray<FVoxelChunkQuad> Quads;
		TArray<uint32> CornerHeights;
		if (!RawQuads.empty())
		{
			FWaterCornerField CornerField;
			BuildWaterCornerField(Pad, CornerField);
			EmitWaterQuads(RawQuads, CornerField, Quads, CornerHeights);
		}

		if (Quads.Num() == 0)
		{
			// No visible faces: the brick is fully enclosed by other water
			// (an interior brick of a large body emits nothing, which is
			// exactly what keeps a big lake affordable) or every cell sits
			// below kMinVisibleFill. Drop any stale component rather than
			// register a proxy-less one.
			//
			// This branch used to fire far more often, and wrongly: under the
			// old >=128 occupancy rule a brick whose cells all sat at, say,
			// 100 fill meshed to zero quads and had its component destroyed,
			// so genuinely-present water rendered as nothing at all. That is
			// the case the fill-fraction sampler above fixes, and a brick held
			// at uniform sub-half fill now rendering is the cheapest single
			// proof this change works.
			if (TObjectPtr<UWaterChunkComponent>* Existing = Impl.ChunkComponents.Find(BrickCoord))
			{
				if (*Existing)
				{
					(*Existing)->DestroyComponent();
				}
				Impl.ChunkComponents.Remove(BrickCoord);
			}
			ReleaseWaterBrickPooled(Impl, Impl.PoolSlots, BrickCoord);
			continue;
		}

		const FVector BrickOriginUU(double(Key.x) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU,
		                            double(Key.y) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU,
		                            double(Key.z) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU);

		// W5 FOAM ACTIVITY, and the ONE place both render paths read it, so they
		// cannot drift. 1 while vxc::WaterCA still calls this brick active,
		// 0 the moment it settles -- and the settle transition reaches here at
		// all only because StepFixed marks the bricks that LEFT the active set
		// dirty exactly once. See FVoxelWaterImpl::ActiveBricks.
		const float Activity = Impl.ActiveBricks.Contains(BrickCoord) ? 1.0f : 0.0f;

		// A brick already holding a component keeps the component path even if
		// the cvar flipped underneath it, and vice versa: mixing representations
		// for ONE brick would draw it twice. New bricks take whichever path is
		// current. Same rule ApplyMeshResult uses for terrain.
		const bool bAlreadyPooled = Impl.PoolSlots.Contains(BrickCoord);
		const bool bAlreadyComponent = Impl.ChunkComponents.Contains(BrickCoord);
		if (bAlreadyPooled || (!bAlreadyComponent && VoxelDebug::GetWaterGpu()))
		{
			ApplyWaterBrickPooled(Impl, ChunkOwner, Material, Impl.PoolSlots, BrickCoord, BrickOriginUU, Quads,
			                      CornerHeights, Activity);
			continue;
		}

		TObjectPtr<UWaterChunkComponent>* ExistingPtr = Impl.ChunkComponents.Find(BrickCoord);
		UWaterChunkComponent* Comp = ExistingPtr ? ExistingPtr->Get() : nullptr;
		if (!Comp)
		{
			// Already inside this tick's kMaxBrickMeshesPerTick budget (the
			// meshBrick<8> call above already happened) -- NewObject +
			// RegisterComponent is comparatively cheap next to that, so no
			// separate cap needed here.
			Comp = NewObject<UWaterChunkComponent>(ChunkOwner);
			Comp->SetupAttachment(ChunkRoot);
			// ChunkRoot sits at the world origin, so the brick's world origin is
			// also its relative location.
			Comp->SetRelativeLocation(BrickOriginUU);
			Comp->SetMaterial(0, Material);
			Comp->RegisterComponent();
			Impl.ChunkComponents.Add(BrickCoord, Comp);
		}
		Comp->SetChunkQuads(MoveTemp(Quads), MoveTemp(CornerHeights), Activity);
	}

	if (DeferredCount > 0)
	{
		UE_LOG(LogVoxelWater, Verbose, TEXT("RemeshDirtyBricks: deferred %d brick mesh(es) to next tick (budget %d/tick)."),
		       DeferredCount, kMaxBrickMeshesPerTick);
	}
}

// --- C7/C8: implicit cavern water ------------------------------------------

// Drains the mobilizer's newly-converted-brick queue. A mobilized brick must
// stop drawing as implicit water and start drawing as CA water in the SAME
// frame or the handover visibly flickers, so this both marks it dirty for the
// CA re-mesh and destroys its implicit component.
void MarkMobilizedBricksDirty(FVoxelWaterImpl& Impl)
{
	for (const vxc::BrickKey& K : Impl.Mob.takeRecentlyMobilized())
	{
		const VoxelCoords::FVoxelCoord C = ToCoord(K);
		Impl.DirtyBricks.Add(C);
		Impl.DirtySinceLastBroadcast.Add(C);
		if (TObjectPtr<UWaterChunkComponent>* Existing = Impl.ImplicitChunkComponents.Find(C))
		{
			if (*Existing)
			{
				(*Existing)->DestroyComponent();
			}
			Impl.ImplicitChunkComponents.Remove(C);
		}
		// Same handover under voxel.Water.GPU: the implicit range must stop
		// drawing in the SAME frame the CA's starts, or the two briefly draw the
		// same water and a translucent surface doubled on itself is obvious.
		ReleaseWaterBrickPooled(Impl, Impl.ImplicitPoolSlots, C);
	}
}

// --- ADR-0005 water persistence -------------------------------------------
//
// The blob is a SIBLING of the terrain edit log, written into the same
// directory with the same per-seed naming and the same atomic tmp+rename write
// UVoxelWorldSubsystem uses (GetWorldSaveFilePath / WriteBytesAtomic in
// VoxelWorldSubsystem.cpp). Only the extension differs: <seed>.vxwater beside
// <seed>.vxlog. See waterca.h's WaterState comment for why it is a separate
// file rather than a section of the log (it invalidates on kWaterCAVersion and
// is discardable without discarding terrain edits).
FString GetWaterSaveFilePath(uint64 Seed)
{
	return FPaths::ProjectSavedDir() / TEXT("VoxelWorlds") / FString::Printf(TEXT("%llu.vxwater"), (unsigned long long)Seed);
}

// The .vxlog the terrain edit log saves to -- used only to decide how LOUD a
// missing water blob should be (a missing blob beside an existing terrain save
// means a world that had edits, some possibly drains, will refill every cavern;
// a missing blob with no terrain save is just a fresh world).
FString GetTerrainSaveFilePath(uint64 Seed)
{
	return FPaths::ProjectSavedDir() / TEXT("VoxelWorlds") / FString::Printf(TEXT("%llu.vxlog"), (unsigned long long)Seed);
}

// Atomic tmp+rename write, identical shape to VoxelWorldSubsystem.cpp's
// WriteBytesAtomic: a process dying mid-write leaves only the .tmp behind,
// never a truncated/corrupt blob a later load would have to reject.
bool WriteWaterBytesAtomic(const FString& Path, const TArray<uint8>& Bytes)
{
	const FString TmpPath = Path + TEXT(".tmp");
	if (!FFileHelper::SaveArrayToFile(Bytes, *TmpPath))
	{
		UE_LOG(LogVoxelWater, Error, TEXT("SaveWaterState: failed to write temp file %s"), *TmpPath);
		return false;
	}
	if (!IFileManager::Get().Move(*Path, *TmpPath, /*Replace*/ true))
	{
		UE_LOG(LogVoxelWater, Error, TEXT("SaveWaterState: failed to rename %s -> %s"), *TmpPath, *Path);
		IFileManager::Get().Delete(*TmpPath);
		return false;
	}
	return true;
}

bool SaveWaterStateToDisk(const FVoxelWaterImpl& Impl, uint64 Seed)
{
	// vxc::WaterState::serialize appends the whole blob (magic + kFormatVersion +
	// kWaterCAVersion + totalVolume integrity check + per-brick fill + active set
	// + mobilized keys) into a caller-owned buffer, exactly like EditLog.
	std::vector<uint8_t> Bytes;
	vxc::WaterState::serialize(Impl.CA, Impl.Mob, Bytes);

	TArray<uint8> OutBytes;
	OutBytes.SetNumUninitialized((int32)Bytes.size());
	if (!Bytes.empty())
	{
		FMemory::Memcpy(OutBytes.GetData(), Bytes.data(), Bytes.size());
	}

	const FString Dir = FPaths::ProjectSavedDir() / TEXT("VoxelWorlds");
	IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
	const FString Path = GetWaterSaveFilePath(Seed);
	if (!WriteWaterBytesAtomic(Path, OutBytes))
	{
		return false;
	}

	vxc::Digest D;
	Impl.CA.digest(D);
	UE_LOG(LogVoxelWater, Log,
	       TEXT("SaveWaterState: wrote %d bytes (%llu fill units, %llu stored brick(s), %llu mobilized brick(s)) to %s -- waterDigest=0x%016llX"),
	       OutBytes.Num(), (unsigned long long)Impl.CA.totalVolume(), (unsigned long long)Impl.CA.storedBrickCount(),
	       (unsigned long long)Impl.Mob.mobilizedBricks().size(), *Path, (unsigned long long)D.h);
	return true;
}

void LoadWaterStateFromDisk(FVoxelWaterImpl& Impl, uint64 Seed)
{
	// Honor -VoxelNoLoad exactly as LoadEditLogFromDisk does: a fresh-start
	// verification aid must start the water field fully implicit too, or terrain
	// would start fresh while water came back from a stale blob.
	if (FParse::Param(FCommandLine::Get(), TEXT("VoxelNoLoad")))
	{
		UE_LOG(LogVoxelWater, Log,
		       TEXT("LoadWaterState: -VoxelNoLoad passed -- skipping water-state load; underground water starts fully implicit."));
		return;
	}

	const FString Path = GetWaterSaveFilePath(Seed);
	if (!FPaths::FileExists(Path))
	{
		// FAILURE MODE 1 -- MISSING. Coherent (the world reverts to the implicit,
		// full field) but every drained cavern refills, so it is LOUD whenever a
		// terrain save exists (that world had edits; some may have been drains)
		// and merely informational for a genuinely fresh world with no log either.
		if (FPaths::FileExists(GetTerrainSaveFilePath(Seed)))
		{
			UE_LOG(LogVoxelWater, Warning,
			       TEXT("LoadWaterState: a terrain save exists but there is NO water blob at %s -- falling back to a fully ")
			       TEXT("implicit world. EVERY drained cavern in this world WILL REFILL (ADR-0005). Expected only for a save ")
			       TEXT("that predates water persistence; otherwise the water blob was lost."),
			       *Path);
		}
		else
		{
			UE_LOG(LogVoxelWater, Log,
			       TEXT("LoadWaterState: no water blob at %s and no terrain save -- fresh world, underground water is fully implicit."),
			       *Path);
		}
		return;
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("LoadWaterState: water blob %s exists but could NOT be READ -- falling back to a fully implicit world; ")
		       TEXT("every drained cavern WILL REFILL (ADR-0005)."),
		       *Path);
		return;
	}

	// FAILURE MODES 2 (stale kWaterCAVersion) and 3 (refused: corrupt/truncated,
	// integrity cross-check mismatch, non-fresh target) collapse into the single
	// signal the ADR-0005 handoff names: vxc::WaterState::load returning false.
	// All three take the SAME loud fallback, because handling any of them quietly
	// silently loses every drained lake. load() applies fills FIRST, then the
	// active set, then markMobilized (waterca.h) -- so the world is never even
	// momentarily in the both-accountants-report-zero state that reads as open air.
	if (!vxc::WaterState::load(Bytes.GetData(), (size_t)Bytes.Num(), Impl.CA, Impl.Mob))
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("LoadWaterState: water blob %s (%d bytes) was REFUSED by vxc::WaterState::load -- stale kWaterCAVersion ")
		       TEXT("(engine is now v%u), corrupt/truncated, or a failed totalVolume integrity cross-check. Falling back to a ")
		       TEXT("fully implicit world; EVERY drained cavern in this world WILL REFILL (ADR-0005)."),
		       *Path, Bytes.Num(), vxc::kWaterCAVersion);
		return;
	}

	// Success. Every restored brick needs meshing on the first frame: the
	// mobilized ones must stop drawing as implicit water and start drawing as CA
	// water (drain takeRecentlyMobilized via MarkMobilizedBricksDirty, tearing
	// down any implicit component -- there are none yet this early, so this just
	// queues the CA re-mesh), and every stored CA brick must mesh its fill.
	MarkMobilizedBricksDirty(Impl);
	for (const auto& Entry : Impl.CA.bricks())
	{
		Impl.DirtyBricks.Add(ToCoord(Entry.first));
	}

	vxc::Digest D;
	Impl.CA.digest(D);
	UE_LOG(LogVoxelWater, Log,
	       TEXT("LoadWaterState: restored %llu fill units across %llu stored brick(s), %llu mobilized brick(s) from %s -- waterDigest=0x%016llX"),
	       (unsigned long long)Impl.CA.totalVolume(), (unsigned long long)Impl.CA.storedBrickCount(),
	       (unsigned long long)Impl.Mob.mobilizedBricks().size(), *Path, (unsigned long long)D.h);
}

// How far around the camera implicit lake surfaces are built, in water bricks
// (8 voxels = 0.8 m each). 32 bricks is ~25 m, comfortably more than a cavern
// room's half-width, so standing in a flooded chamber shows its whole surface.
constexpr int32 kImplicitRadiusBricks = 32;
constexpr int32 kImplicitRadiusBricksZ = 16;
// Same budget discipline as kMaxBrickMeshesPerTick: the candidate box is large
// and a first refresh must never land as one frame's worth of work.
constexpr int32 kMaxImplicitMeshesPerTick = 192;

// Rebuilds the implicit-water candidate list when the camera crosses into a new
// brick, then meshes a budgeted slice of it every tick.
//
// WHY THIS MESHES BRICKS RATHER THAN DROPPING IN A FLAT PLANE. A plane at
// floodZ (the AVoxelOceanActor trick) would be cheaper and the design doc even
// suggests the same material family — but mobilization would then be a visible
// seam, a flat sheet abruptly becoming voxel water. Going through the very same
// meshBrick<8> path the CA uses means an implicit brick and a mobilized brick
// are pixel-identical, so a draining lake reads as one continuous body with a
// hole appearing in it. That is precisely the shot this feature has to sell.
void RefreshImplicitWater(FVoxelWaterImpl& Impl, const FVector& CameraUU, AActor* ChunkOwner,
                          USceneComponent* ChunkRoot, UMaterialInterface* Material)
{
	if (!ChunkOwner || !ChunkRoot)
	{
		return; // dedicated server: never render
	}

	const int64 CamVx = int64(FMath::FloorToDouble(CameraUU.X / VoxelCoords::VoxelSizeUU));
	const int64 CamVy = int64(FMath::FloorToDouble(CameraUU.Y / VoxelCoords::VoxelSizeUU));
	const int64 CamVz = int64(FMath::FloorToDouble(CameraUU.Z / VoxelCoords::VoxelSizeUU));
	const VoxelCoords::FVoxelCoord Center{
		int32(vxc::floorDiv(CamVx, vxc::WaterBrick8::kEdge)), int32(vxc::floorDiv(CamVy, vxc::WaterBrick8::kEdge)),
		int32(vxc::floorDiv(CamVz, vxc::WaterBrick8::kEdge))};

	if (!Impl.bImplicitCenterValid || Center != Impl.LastImplicitCenterBrick)
	{
		Impl.LastImplicitCenterBrick = Center;
		Impl.bImplicitCenterValid = true;
		Impl.PendingImplicitBricks.Reset();

		// Cheap reject, one column query per brick COLUMN rather than 512 per
		// brick: the flood level is a per-site constant across its whole reach
		// disc, so a column that is dry has no flooded brick anywhere in its
		// stack and the entire vertical run is skipped.
		for (int32 By = Center.Y - kImplicitRadiusBricks; By <= Center.Y + kImplicitRadiusBricks; ++By)
		{
			for (int32 Bx = Center.X - kImplicitRadiusBricks; Bx <= Center.X + kImplicitRadiusBricks; ++Bx)
			{
				const int64 Vx = int64(Bx) * vxc::WaterBrick8::kEdge;
				const int64 Vy = int64(By) * vxc::WaterBrick8::kEdge;
				const int32 FloodZMm = Impl.Amp.columnCached(Vx, Vy).cavern.floodZMm;
				if (FloodZMm == INT32_MIN)
				{
					continue; // dry column
				}
				// Only bricks whose bottom sits below the flood level can hold
				// any water at all.
				const int64 FloodBrickZ = vxc::floorDiv(int64(FloodZMm) / vxc::kVoxelSizeMm, vxc::WaterBrick8::kEdge);
				for (int32 Bz = Center.Z - kImplicitRadiusBricksZ; Bz <= Center.Z + kImplicitRadiusBricksZ; ++Bz)
				{
					if (int64(Bz) > FloodBrickZ)
					{
						continue;
					}
					Impl.PendingImplicitBricks.Add(VoxelCoords::FVoxelCoord{Bx, By, Bz});
				}
			}
		}

		// Farthest first, because the drain below Pop()s from the BACK: the
		// water actually in front of the camera has to mesh in the first few
		// ticks, not after the whole 25 m disc has been walked.
		UE_LOG(LogVoxelWater, Log,
		       TEXT("RefreshImplicitWater: rebuilt at brick (%d,%d,%d) [cam (%.0f,%.0f,%.0f) UU] -- %d candidate brick(s)"),
		       Center.X, Center.Y, Center.Z, CameraUU.X, CameraUU.Y, CameraUU.Z, Impl.PendingImplicitBricks.Num());
		Impl.PendingImplicitBricks.Sort(
			[Center](const VoxelCoords::FVoxelCoord& A, const VoxelCoords::FVoxelCoord& B)
			{
				const int64 Da = int64(A.X - Center.X) * (A.X - Center.X) + int64(A.Y - Center.Y) * (A.Y - Center.Y) +
				                 int64(A.Z - Center.Z) * (A.Z - Center.Z);
				const int64 Db = int64(B.X - Center.X) * (B.X - Center.X) + int64(B.Y - Center.Y) * (B.Y - Center.Y) +
				                 int64(B.Z - Center.Z) * (B.Z - Center.Z);
				return Da > Db;
			});
	}

	int32 MeshesThisTick = 0;
	int32 Built = 0;
	while (Impl.PendingImplicitBricks.Num() > 0 && MeshesThisTick < kMaxImplicitMeshesPerTick)
	{
		const VoxelCoords::FVoxelCoord BrickCoord = Impl.PendingImplicitBricks.Pop(EAllowShrinking::No);
		const vxc::BrickKey Key = ToBrickKey(BrickCoord);
		const int64_t Ox = int64_t(Key.x) * vxc::WaterBrick8::kEdge;
		const int64_t Oy = int64_t(Key.y) * vxc::WaterBrick8::kEdge;
		const int64_t Oz = int64_t(Key.z) * vxc::WaterBrick8::kEdge;

		++MeshesThisTick;

		// implicitFillAt() already returns 0 for a mobilized brick, so a
		// converted brick simply meshes to nothing here and the CA's own
		// component takes over — the ownership partition does the bookkeeping.
		std::vector<vxc::Quad> RawQuads;
		// Carries the fill fraction as the material id, exactly like the CA
		// sampler in RemeshDirtyBricks -- see the long comment there for what
		// the three uses of that byte are.
		//
		// THIS ONE IS NOT OPTIONAL, and it is not merely for consistency. The
		// byte now drives M_WaterVoxel's World Position Offset via
		// VertexColor.R, so returning the old fixed MAT_ROCK placeholder (= 2)
		// would hand the material R = 2/255 and push the top face of every
		// implicit cavern lake down by ~99% of a voxel -- collapsing every
		// static underground lake in the world to a sub-millimetre film. The
		// implicit field is a static flood level, so in practice this returns
		// 255 below floodZ and 0 above it, which the material reads as full
		// height. Same kMinVisibleFill floor for the same reason.
		//
		// Padded-block-first, for the same two reasons the CA path is (see the
		// FWaterFillPad comment): identical read count, and the corner field below
		// then costs no further implicitFillAt calls.
		FWaterFillPad Pad;
		BuildWaterFillPad(Pad, [&Impl, Ox, Oy, Oz](int32 x, int32 y, int32 z) -> uint8
		{
			return Impl.Mob.implicitFillAt(Ox + x, Oy + y, Oz + z);
		});

		const auto Sampler = [&Pad](int x, int y, int z) -> vxc::MaterialId
		{
			const uint8 Fill = Pad.At(x, y, z);
			return Fill >= kMinVisibleFill ? QuantiseFillForMesh(Fill) : vxc::MAT_AIR;
		};
		vxc::meshBrick<vxc::WaterBrick8::kEdge>(Sampler, RawQuads);

		if (RawQuads.empty())
		{
			// Dry, fully submerged (interior bricks emit no faces — which is
			// exactly what keeps a large lake affordable), or mobilized.
			if (TObjectPtr<UWaterChunkComponent>* Existing = Impl.ImplicitChunkComponents.Find(BrickCoord))
			{
				if (*Existing)
				{
					(*Existing)->DestroyComponent();
				}
				Impl.ImplicitChunkComponents.Remove(BrickCoord);
			}
			ReleaseWaterBrickPooled(Impl, Impl.ImplicitPoolSlots, BrickCoord);
			continue;
		}

		// Same corner-height treatment as the CA path, and it costs this path very
		// little: the implicit field is a per-site FLOOD LEVEL, so within one
		// cavern's reach it is 255 below floodZ and 0 above it. Every lattice
		// corner of a submerged layer then averages to 255 ("full to this cell's
		// ceiling") and a flat lake's top layer agrees corner for corner, so
		// nothing is non-planar and nothing splits -- an implicit lake meshes to
		// the quad count it did before. Where two sites' flood levels meet, or
		// where a lake laps a cavern wall, the same rule applies as anywhere else
		// and a few quads split; that is the point rather than a cost.
		//
		// AND IT IS WHAT KEEPS THE MOBILIZATION HANDOVER INVISIBLE. C8 exists so a
		// brick converting from implicit to CA is pixel-identical across the swap
		// (see this function's own header comment). The moment CA water grew a
		// bilinear surface, an implicit neighbour still drawing flat tops would
		// have put a step exactly on the boundary the handover is meant to hide.
		// Routing both through the same corner field is not consistency for its
		// own sake here -- it is the feature.
		FWaterCornerField CornerField;
		BuildWaterCornerField(Pad, CornerField);

		TArray<FVoxelChunkQuad> Quads;
		TArray<uint32> CornerHeights;
		EmitWaterQuads(RawQuads, CornerField, Quads, CornerHeights);

		const FVector BrickOriginUU(double(Ox) * VoxelCoords::VoxelSizeUU, double(Oy) * VoxelCoords::VoxelSizeUU,
		                            double(Oz) * VoxelCoords::VoxelSizeUU);

		// Same "dispatch on what the brick holds" rule as the CA path above.
		//
		// ACTIVITY IS ZERO HERE AND ALWAYS WILL BE, and that is a statement about
		// what implicit water IS rather than a placeholder: the implicit field is
		// a static worldgen flood level, so no implicit brick is ever in the CA's
		// active set. A brick that starts moving is MOBILIZED, which hands it to
		// the CA half and destroys the implicit range in the same frame
		// (MarkMobilizedBricksDirty) -- so foam appears exactly at the handover
		// and never on a still cavern lake.
		const bool bAlreadyPooled = Impl.ImplicitPoolSlots.Contains(BrickCoord);
		const bool bAlreadyComponent = Impl.ImplicitChunkComponents.Contains(BrickCoord);
		if (bAlreadyPooled || (!bAlreadyComponent && VoxelDebug::GetWaterGpu()))
		{
			ApplyWaterBrickPooled(Impl, ChunkOwner, Material, Impl.ImplicitPoolSlots, BrickCoord, BrickOriginUU, Quads,
			                      CornerHeights, /*Activity=*/0.0f);
			++Built;
			continue;
		}

		TObjectPtr<UWaterChunkComponent>* ExistingPtr = Impl.ImplicitChunkComponents.Find(BrickCoord);
		UWaterChunkComponent* Comp = ExistingPtr ? ExistingPtr->Get() : nullptr;
		if (!Comp)
		{
			Comp = NewObject<UWaterChunkComponent>(ChunkOwner);
			Comp->SetupAttachment(ChunkRoot);
			Comp->SetRelativeLocation(BrickOriginUU);
			Comp->SetMaterial(0, Material);
			Comp->RegisterComponent();
			Impl.ImplicitChunkComponents.Add(BrickCoord, Comp);
		}
		Comp->SetChunkQuads(MoveTemp(Quads), MoveTemp(CornerHeights), /*Activity=*/0.0f);
		++Built;
	}

	if (MeshesThisTick > 0 && Impl.PendingImplicitBricks.Num() == 0)
	{
		UE_LOG(LogVoxelWater, Verbose, TEXT("RefreshImplicitWater: candidate list drained; %d implicit water component(s) live."),
		       Impl.ImplicitChunkComponents.Num());
	}
	(void)Built;
}

// ===========================================================================
// W4 SHALLOW WATER -- voxel.Water.SWE (docs/adr/0004-swe-fixed-point-coupling.md)
// ===========================================================================
//
// WHAT THIS IS FOR, IN ONE SENTENCE FROM THE ADR: "today a lake is a level, not
// a body of water. It has no current, no waves, no direction." waterca.h's
// Phase C is explicit that it "only ever computes a static equilibrium level,
// instantly-ish over a few ticks, never a surge", and that flow momentum is NOT
// MODELLED. voxelcore/swe.h is the layer that has the momentum -- the face
// fluxes ARE the force field -- and it has been sitting in the tree complete,
// tested and golden-pinned, with `SweCoupleConfig::enabled` defaulting false
// and nothing anywhere constructing an SweGrid. This block is the wiring that
// lets a standalone session turn it on.
//
// WHY THIS IS ALLOWED TO EXIST AT ALL, given ADR-0004 item 3 says DEFER.
// The ADR's deferral is argued entirely from replication: the coupler "is a
// second simulation whose state (membership, dwell counters, sheet depth) must
// replicate or be derived identically on every client, exactly like WaterCA ...
// it is not yet wired into the replication path at all. Enabling it before that
// is a guaranteed desync." Every clause of that is about a peer that could
// disagree. In NM_Standalone there is no mirror and no wire -- the subsystem
// does not even broadcast (Tick's BroadcastWaterDiffs is skipped on
// NM_Standalone) -- so the failure the deferral names cannot occur. MaybeArmSwe
// below therefore REFUSES every other net mode outright and says why. That is
// keeping the ADR's deferral, not overriding it: the moment a client exists,
// this feature is off, and it stays off until someone lands the replication
// plumbing the ADR asks for. Plumbing first, flag second, as separate commits.
//
// WHAT IS DELIBERATELY NOT HERE, AND IT IS THE ONE THING A LOOKER WILL NOTICE
// FIRST: THE RENDERER. ADR-0004's own enablement checklist ends with "the
// visible water surface becomes the UNION of sheet depth and CA fill. A
// renderer drawing only one of them will show water vanishing at a boundary."
// That is exactly what happens today: RemeshDirtyBricks samples the CA, and
// RefreshImplicitWater samples the mobilizer's implicit field, and neither
// knows the sheet exists. So a promoted column's water is simulated, conserved,
// ledgered and INVISIBLE. This is called out in the arming log line, in the
// cvar help text, and in the 1Hz status line, because a tester who pours water
// with this flag on and watches it disappear must be able to tell "correctly
// promoted, not yet drawn" from "destroyed". The meshing/sampler region of this
// file is owned elsewhere and is being rewritten concurrently; the precise
// query the renderer needs is specified in the handoff notes rather than
// guessed at here.

// Columns on a side of the single dense sheet.
//
// ONE dense rectangle, not a tiled residency scheme, because swe.h says the
// dense grid IS the reference core and "a production sheet tiles this; the tick
// rules do not change" -- so tiling is a separable follow-up, and doing it now
// would mean debugging residency and first-light wiring in the same change.
//
// 128 is the footprint ADR-0004 measured: 16,384 columns at 0.364 ms/tick,
// scaling linearly in columns (0.083 ms at 64x64, 1.48 ms at 256x256). At 10Hz
// that is ~3.6 ms/s of game-thread time, well inside this subsystem's <2 ms per
// FRAME budget, and it is paid only while armed. 12.8 m on a side at
// kVoxelSizeMm = 100 covers a poured test basin with room around it; 256 would
// cover more and cost 4x, which is a decision for after the renderer exists.
constexpr int32 kSweSheetColumns = 128;

// Bed seating window, in voxels, relative to the terrain's reported surface
// height for each column. Start a little ABOVE the surface (placed blocks, and
// the surface-height query's own quantisation, can both sit above it) and scan
// down far enough to find the floor of a dug basin.
//
// This window is a COST bound, not a correctness one: a column whose search
// finds no solid voxel simply keeps a non-solid bed, which fails the coupler's
// eligibility predicate (`solidAt(vx, vy, bed)` is its first test) forever, so
// the column stays CA-owned and the CA behaves exactly as it does today. The
// failure mode of too small a window is therefore "less sheet", never "wrong
// sheet". 64 voxels is 6.4 m of downward search.
constexpr int32 kSweBedScanAboveVoxels = 8;
constexpr int32 kSweBedScanVoxels = 64;

// Voxel z of the topmost solid voxel in this column's search window.
//
// Uses the MOBILIZER-WRAPPED solidity function, not the bare terrain query, for
// the same reason the CA is given the wrapped one (waterca.h, "SO WE MAKE IT
// STRUCTURALLY IMPOSSIBLE"): an unmobilized implicit cavern lake reads as SOLID
// through the wrapper. Seating the bed with the bare query would put the
// ownership boundary UNDER water the implicit field still owns, and the
// coupler's eligibility predicate -- which only asks whether the bed is solid
// and whether there is clearance above it -- would then happily promote a
// column over an implicit lake and start moving units the implicit accountant
// still believes it holds. That is the double-occupancy bug waterca.h spends a
// page making structurally impossible, and it would be reintroduced by using
// the wrong callback here. Wrapped everywhere, or nowhere.
int32 SeatSweBedZ(const vxc::WaterCA::SolidFn& Solid, int64 Vx, int64 Vy, int64 SurfaceVz, bool& bOutFound)
{
	const int64 TopZ = SurfaceVz + kSweBedScanAboveVoxels;
	for (int32 K = 0; K < kSweBedScanVoxels + kSweBedScanAboveVoxels; ++K)
	{
		const int64 Z = TopZ - K;
		if (Solid(Vx, Vy, Z) != vxc::MAT_AIR)
		{
			bOutFound = true;
			return int32(Z);
		}
	}
	bOutFound = false;
	return int32(TopZ - (kSweBedScanVoxels + kSweBedScanAboveVoxels));
}

// Flushes every SWE-owned column's sheet depth back into the CA through the
// coupler's ledgered demotion channel, leaving the grid empty and every column
// CA-owned.
//
// THIS IS WHAT MAKES PERSISTENCE A NO-OP RATHER THAN A FORMAT CHANGE, and it is
// the single most destructive thing to get wrong in this whole change. Sheet
// depth is REAL VOLUME -- swe.h §2: depth is a column total in the same fill
// units the CA uses, 255 == one voxel -- and vxc::WaterState::serialize walks
// the CA's bricks and the mobilizer's key set and knows nothing about a grid.
// So serialising while the sheet holds water writes a blob that is quietly
// short by exactly the sheet's contents, and the loss is invisible until
// someone reloads and finds their lake shallower. Flushing first means the
// units are back in CA cells before the serializer looks, ADR-0005's blob
// format is untouched, kWaterCAVersion is untouched, and a save written with
// this cvar armed is byte-comparable with one written without it.
//
// It goes through forceDemote rather than draining the grid by hand precisely
// because forceDemote runs the REAL transfer channel: demote() pre-flights the
// CA's available capacity, moves the whole column or not one unit of it (a
// half-demoted column would hold sheet depth AND CA fill in the same z-range,
// which swe.h §5's partition forbids), and books the move into the coupler's
// toCA_ ledger. A hand-rolled drain would have to re-derive all of that and
// would be the obvious place for this change to lose water.
//
// ALL-OR-NOTHING MEANS THIS CAN LEGITIMATELY FAIL, on a column whose CA cells
// have no room this instant. There is nothing useful to retry against without
// ticking the CA, so a residue is reported LOUDLY rather than papered over: the
// water is still conserved (it is in the grid, and the ledger still balances),
// it is simply not in the blob, which is precisely the case that must never be
// discovered later from a screenshot.
// Dirty every render brick sitting over a column whose sheet depth moved.
//
// The CA's own changed-brick set cannot cover this: sheet water is not in the
// CA, so a promoted column can change depth every tick while the CA reports
// nothing dirty and the surface freezes at whatever it looked like when the
// water left. This is the sheet's equivalent of that signal.
//
// A depth change dirties the whole z-span the sheet can occupy for that column
// ([bed+1, bed+sheetScanVoxels]) rather than trying to work out which single
// layer moved -- the span is at most sheetScanVoxels tall (8 voxels, so one or
// two bricks), and getting it wrong in the cheap direction means a stale
// surface, which is the failure this function exists to prevent.
void MarkSweDepthChangesDirty(FVoxelWaterImpl& Impl)
{
	if (!Impl.SweSheet || !Impl.SweCoupler)
	{
		return;
	}

	const vxc::SweGrid& Grid = *Impl.SweSheet;
	const int32 Cols = kSweSheetColumns;
	const int32 ScanVoxels = Impl.SweCoupler->config().sheetScanVoxels;

	if (Impl.SweLastDepth.size() != size_t(Cols) * size_t(Cols))
	{
		Impl.SweLastDepth.assign(size_t(Cols) * size_t(Cols), 0);
	}

	const int64 OriginVx = Grid.originVx();
	const int64 OriginVy = Grid.originVy();

	for (int32 Cy = 0; Cy < Cols; ++Cy)
	{
		for (int32 Cx = 0; Cx < Cols; ++Cx)
		{
			const int64 Vx = OriginVx + Cx;
			const int64 Vy = OriginVy + Cy;
			const int32 Depth = Grid.depthAt(Vx, Vy);
			int32& Last = Impl.SweLastDepth[size_t(Cy) * size_t(Cols) + size_t(Cx)];
			if (Depth == Last)
			{
				continue;
			}
			Last = Depth;

			const int32 Bed = Grid.bedAt(Vx, Vy);
			const int64 BrickX = vxc::floorDiv(Vx, int64(vxc::WaterBrick8::kEdge));
			const int64 BrickY = vxc::floorDiv(Vy, int64(vxc::WaterBrick8::kEdge));
			const int64 ZLo = vxc::floorDiv(int64(Bed) + 1, int64(vxc::WaterBrick8::kEdge));
			const int64 ZHi = vxc::floorDiv(int64(Bed) + ScanVoxels, int64(vxc::WaterBrick8::kEdge));
			for (int64 BrickZ = ZLo; BrickZ <= ZHi; ++BrickZ)
			{
				Impl.DirtyBricks.Add(VoxelCoords::FVoxelCoord{BrickX, BrickY, BrickZ});
			}
		}
	}
}

void FlushSweIntoCA(FVoxelWaterImpl& Impl, const TCHAR* Reason)
{
	if (!Impl.SweCoupler || !Impl.SweSheet)
	{
		return;
	}
	vxc::SweGrid& Grid = *Impl.SweSheet;
	const int64_t SheetBefore = Grid.totalVolume();
	const int32 ColumnsBefore = Impl.SweCoupler->sweColumnCount();
	if (ColumnsBefore == 0 && SheetBefore == 0)
	{
		return; // armed but holding nothing -- no work, and no log noise either
	}

	int32 Demoted = 0;
	for (int32 Cy = 0; Cy < Grid.sizeY(); ++Cy)
	{
		for (int32 Cx = 0; Cx < Grid.sizeX(); ++Cx)
		{
			const int64 Vx = Grid.originVx() + Cx;
			const int64 Vy = Grid.originVy() + Cy;
			if (!Impl.SweCoupler->isSweColumn(Vx, Vy))
			{
				continue;
			}
			const int32_t Depth = Grid.depthAt(Vx, Vy);
			const int32_t Bed = Grid.bedAt(Vx, Vy);
			Impl.SweCoupler->forceDemote(Vx, Vy);
			++Demoted;
			if (Depth > 0 && Grid.depthAt(Vx, Vy) == 0)
			{
				// The units landed in CA cells stacking up from the bed, so the
				// same span MarkColumnDirty computes for an addWater() covers
				// them -- re-mesh and (on a listen server, which cannot arm this
				// anyway) replication both need them marked.
				MarkColumnDirty(Impl, Vx, Vy, Bed, uint32_t(Depth));
			}
		}
	}

	const int64_t SheetAfter = Grid.totalVolume();
	if (SheetAfter != 0)
	{
		UE_LOG(LogVoxelWater, Error,
		       TEXT("voxel.Water.SWE flush (%s): %lld of %lld sheet fill unit(s) could NOT be demoted into the CA ")
		       TEXT("(%d column(s) attempted, %d still SWE-owned). Demotion is all-or-nothing per column (swe.h S5): a ")
		       TEXT("column whose CA cells have no room this tick keeps its depth rather than half-moving. The water is ")
		       TEXT("NOT lost -- it is still in the grid and the coupled ledger still balances -- but it is NOT in the ")
		       TEXT("save blob either, so a reload will be short by that amount."),
		       Reason, (long long)SheetAfter, (long long)SheetBefore, Demoted, Impl.SweCoupler->sweColumnCount());
	}
	else
	{
		UE_LOG(LogVoxelWater, Log,
		       TEXT("voxel.Water.SWE flush (%s): demoted %d column(s), %lld fill unit(s) returned to the CA through the ")
		       TEXT("ledgered demotion channel. Sheet is empty; the ADR-0005 blob format is untouched."),
		       Reason, Demoted, (long long)SheetBefore);
	}

	// The coupler's own hysteresis re-forms the sheet from here: every column
	// that still passes the eligibility predicate promotes again after
	// promoteDwellTicks (8 ticks = 0.8 s at this subsystem's fixed step). A
	// save is therefore a brief, self-healing interruption of the sheet rather
	// than a teardown, and no re-seeding code is needed on this side.
	Impl.SweLastCoupledTotal = int64_t(Impl.CA.totalVolume()) + Grid.totalVolume();
}

// Constructs (or tears down) the sheet to match voxel.Water.SWE. Called once
// per Tick, before the fixed-step loop, so an arm/disarm always lands on a
// clean step boundary rather than between the coupler and the CA.
void MaybeArmSwe(FVoxelWaterImpl& Impl, UWorld* World)
{
	const bool bWant = VoxelDebug::GetWaterSwe();
	const bool bArmed = Impl.SweCoupler != nullptr;

	if (!bWant)
	{
		Impl.bSweRefusalLogged = false; // re-arm may legitimately re-refuse and should say so again
		if (bArmed)
		{
			// DISARM IS A FLUSH, NOT A DELETE. Dropping the grid would silently
			// destroy every unit of sheet depth -- the same loss the save path
			// guards against -- so the off-switch runs the identical ledgered
			// return path and only then releases the objects. That is what makes
			// this cvar genuinely reversible mid-session in both directions.
			FlushSweIntoCA(Impl, TEXT("voxel.Water.SWE 0"));
			Impl.SweCoupler.reset();
			Impl.SweSheet.reset();
			UE_LOG(LogVoxelWater, Log, TEXT("voxel.Water.SWE 0: shallow-water sheet disarmed; water is pure WaterCA again."));
		}
		return;
	}

	if (bArmed)
	{
		return;
	}

	// --- The ADR-0004 item 3 gate -----------------------------------------
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
	if (NetMode != NM_Standalone)
	{
		if (!Impl.bSweRefusalLogged)
		{
			Impl.bSweRefusalLogged = true;
			UE_LOG(LogVoxelWater, Warning,
			       TEXT("voxel.Water.SWE REFUSED: net mode is %d, not NM_Standalone. ADR-0004 (accepted 2026-07-21) item 3 ")
			       TEXT("defers enabling the CA<->SWE coupler until M3 networked water, because the coupler is a second ")
			       TEXT("simulation whose membership, dwell counters and sheet depth are 'not yet wired into the ")
			       TEXT("replication path at all' -- 'enabling it before that is a guaranteed desync'. A standalone world ")
			       TEXT("has no mirror to desync, which is the ONLY case this gate lets through. Land the replication ")
			       TEXT("plumbing first, then widen this gate -- plumbing first, flag second, as separate commits."),
			       int32(NetMode));
		}
		return;
	}

	// --- Where to put it ---------------------------------------------------
	//
	// One dense region has to be centred on something, and the honest choice is
	// wherever the water actually is: the centroid of the CA's stored bricks,
	// which for a test session is the pour. With no water stored yet, fall back
	// to the player's own viewpoint so that arming the cvar and then pouring
	// works in either order. With neither, do not guess -- retry silently next
	// frame, because a sheet centred on the world origin is 84 km from anything
	// and would only produce a confusing "armed but nothing ever promotes".
	int64 AnchorVx = 0, AnchorVy = 0;
	bool bHaveAnchor = false;
	if (Impl.CA.storedBrickCount() > 0)
	{
		double SumX = 0.0, SumY = 0.0;
		int64 Count = 0;
		for (const auto& Entry : Impl.CA.bricks())
		{
			SumX += (double(Entry.first.x) + 0.5) * double(vxc::WaterBrick8::kEdge);
			SumY += (double(Entry.first.y) + 0.5) * double(vxc::WaterBrick8::kEdge);
			++Count;
		}
		AnchorVx = int64(FMath::FloorToDouble(SumX / double(Count)));
		AnchorVy = int64(FMath::FloorToDouble(SumY / double(Count)));
		bHaveAnchor = true;
	}
	else if (APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr)
	{
		FVector ViewUU = FVector::ZeroVector;
		FRotator UnusedRot = FRotator::ZeroRotator;
		PC->GetPlayerViewPoint(ViewUU, UnusedRot);
		const VoxelCoords::FVoxelCoord Vc = VoxelCoords::WorldToVoxel(ViewUU);
		AnchorVx = Vc.X;
		AnchorVy = Vc.Y;
		bHaveAnchor = true;
	}
	if (!bHaveAnchor)
	{
		if (!Impl.bSweRefusalLogged)
		{
			Impl.bSweRefusalLogged = true;
			UE_LOG(LogVoxelWater, Warning,
			       TEXT("voxel.Water.SWE: nothing to centre the sheet on yet (no stored water, no player controller). ")
			       TEXT("Retrying every frame -- pour water or wait for a pawn."));
		}
		return;
	}

	const double ArmStartSeconds = FPlatformTime::Seconds();

	// ticksPerSecond exists ONLY to scale velocityAt()'s mm/s output (swe.h S6);
	// it enters no tick rule and moves no digest. It must match this subsystem's
	// fixed step or the force field W5 eventually consumes would be reported at
	// twice its real speed, since swe.h's default assumes a 20Hz caller.
	vxc::SweConfig SheetCfg;
	SheetCfg.ticksPerSecond = int32_t(FMath::RoundToInt(1.f / FVoxelWaterImpl::FixedStepSeconds));

	const int64 OriginVx = AnchorVx - kSweSheetColumns / 2;
	const int64 OriginVy = AnchorVy - kSweSheetColumns / 2;
	Impl.SweSheet = std::make_unique<vxc::SweGrid>(OriginVx, OriginVy, kSweSheetColumns, kSweSheetColumns, SheetCfg);

	// The wrapped solidity callback, built ONCE and shared by the bed seating
	// below and by the coupler itself, so the two can never disagree about what
	// "solid" means (see SeatSweBedZ's comment for why that matters).
	vxc::WaterCA::SolidFn Solid = Impl.Mob.makeSolidFn();

	int32 SeatedColumns = 0;
	for (int32 Cy = 0; Cy < kSweSheetColumns; ++Cy)
	{
		for (int32 Cx = 0; Cx < kSweSheetColumns; ++Cx)
		{
			const int64 Vx = OriginVx + Cx;
			const int64 Vy = OriginVy + Cy;
			const double SurfaceZUU = Impl.Terrain.GetSurfaceHeightUU(double(Vx) * VoxelCoords::VoxelSizeUU,
			                                                          double(Vy) * VoxelCoords::VoxelSizeUU);
			const int64 SurfaceVz = int64(FMath::FloorToDouble(SurfaceZUU / VoxelCoords::VoxelSizeUU));
			bool bFound = false;
			const int32 BedZ = SeatSweBedZ(Solid, Vx, Vy, SurfaceVz, bFound);
			Impl.SweSheet->setBed(Vx, Vy, BedZ);
			if (bFound)
			{
				++SeatedColumns;
			}
		}
	}

	vxc::SweCoupleConfig CoupleCfg;
	CoupleCfg.enabled = true; // the master flag swe.h ships false; this is the only place it is ever set
	Impl.SweCoupler = std::make_unique<vxc::SweCaCoupler>(*Impl.SweSheet, Impl.CA, Solid, CoupleCfg);

	// --- Seed the body that is already there -------------------------------
	//
	// forcePromote rather than waiting out promoteDwellTicks, exactly as swe.h
	// documents it for "a caller seeding a known-open body (e.g. a generated
	// lake) at world load": it runs the same promote() path, which absorbs the
	// column's CA fill at full rate through the ledgered channel, so it is
	// ledger-exact -- it is only the DWELL that is skipped, not the transfer.
	//
	// SEEDED SET = columns that have a real bed AND already hold CA water above
	// it. Note what is deliberately NOT done: the coupler's `eligible()`
	// predicate is private, and re-implementing it engine-side to pre-filter
	// this set would fork a rule that lives in golden-pinned code, which is
	// exactly how two copies of a predicate drift. Instead this seeds the water
	// -bearing columns and lets the machinery correct itself: a seeded column
	// that in fact fails the predicate (a lidded pocket, a one-voxel pipe)
	// accumulates the ineligible dwell and demotes all-or-nothing after
	// demoteDwellTicks (32 ticks = 3.2 s), returning its units through the same
	// ledger. The cost of being wrong is a few seconds of a column being
	// simulated by the wrong solver; the cost of a forked predicate is silent
	// disagreement forever.
	int32 SeededColumns = 0;
	int64_t SeededVolume = 0;
	const int32 ScanVoxels = Impl.SweCoupler->config().sheetScanVoxels;
	for (int32 Cy = 0; Cy < kSweSheetColumns; ++Cy)
	{
		for (int32 Cx = 0; Cx < kSweSheetColumns; ++Cx)
		{
			const int64 Vx = OriginVx + Cx;
			const int64 Vy = OriginVy + Cy;
			const int32_t BedZ = Impl.SweSheet->bedAt(Vx, Vy);
			if (Solid(Vx, Vy, BedZ) == vxc::MAT_AIR)
			{
				continue; // no bed was found in the search window: not a sheet column
			}
			bool bHasWater = false;
			for (int32 K = 1; K <= ScanVoxels && !bHasWater; ++K)
			{
				bHasWater = Impl.CA.fillAt(Vx, Vy, int64(BedZ) + K) != 0;
			}
			if (!bHasWater)
			{
				continue;
			}
			Impl.SweCoupler->forcePromote(Vx, Vy);
			if (Impl.SweCoupler->isSweColumn(Vx, Vy))
			{
				++SeededColumns;
				SeededVolume += Impl.SweSheet->depthAt(Vx, Vy);
				// Those cells just emptied on the CA side; the mesh has to lose
				// them in the same tick or the water renders twice.
				MarkColumnDirty(Impl, Vx, Vy, int64(BedZ), uint32_t(Impl.SweSheet->depthAt(Vx, Vy)));
			}
		}
	}

	// Seat the conservation ledger. Promotion is a TRANSFER, so the sum across
	// both solvers is unchanged by the seeding above -- which is itself the
	// first thing this ledger asserts, on the very next fixed step.
	Impl.SweLastCoupledTotal = int64_t(Impl.CA.totalVolume()) + Impl.SweSheet->totalVolume();
	Impl.SweInjected = Impl.SweLastCoupledTotal;
	Impl.SweConservationFailures = 0;
	Impl.bSweRefusalLogged = false;

	const double ArmMs = (FPlatformTime::Seconds() - ArmStartSeconds) * 1000.0;
	UE_LOG(LogVoxelWater, Log,
	       TEXT("voxel.Water.SWE ARMED (ADR-0004, NM_Standalone only): %dx%d sheet at voxel origin (%lld,%lld), ")
	       TEXT("%d/%d column(s) found a bed, %d column(s) seeded by forcePromote holding %lld fill unit(s). ")
	       TEXT("Arming took %.1f ms (bed seating). kSweVersion=%u, kWaterCAVersion=%u -- neither is bumped and no ")
	       TEXT("water golden moves. The renderer draws the UNION of sheet depth and CA fill (ADR-0004 'Renderer', ")
	       TEXT("see UnionSweFill), so promoted water is visible. NOTE it will not look identical to the CA-only ")
	       TEXT("case: the sheet spreads a pour laterally as a thin film where Phase C pools it into basins. That ")
	       TEXT("is a real difference in the physics, not a rendering artifact -- watch the conservation line."),
	       kSweSheetColumns, kSweSheetColumns, (long long)OriginVx, (long long)OriginVy, SeatedColumns,
	       kSweSheetColumns * kSweSheetColumns, SeededColumns, (long long)SeededVolume, ArmMs, vxc::kSweVersion,
	       vxc::kWaterCAVersion);
}

// One fixed 10Hz step: Reservoir v0 top-up, then vxc::WaterCA::step(),
// folding the CA's own post-step active set into the dirty-for-remesh /
// dirty-for-replication sets (task items 2 & 3).
void StepFixed(FVoxelWaterImpl& Impl, double NowWorldSeconds)
{
	// ADR-0003 item 4: re-check the cvar every fixed step (cheap -- a bool
	// compare, plus a memo clear only on the OFF-transition, see
	// WaterCA::setSolidCacheEnabled) so voxel.Water.SolidCacheEnabled can be
	// flipped live without restarting. Must happen before step() below --
	// hydrostaticPass reads solidCacheEnabled_ at the top of its own call.
	Impl.CA.setSolidCacheEnabled(CVarVoxelWaterSolidCache.GetValueOnGameThread());

	// Reservoir v0 (SS3.7: "the implicit ocean acts as an infinite
	// reservoir: boundary cells refill to 255 each tick while exposed"). v0
	// simplification: tops up unconditionally once registered (no support
	// for detecting a plugged/re-solidified breach), documented in
	// NotifyTerrainVoxelsCleared's own doc comment.
	for (const VoxelCoords::FVoxelCoord& Cell : Impl.ReservoirCells)
	{
		const uint8_t Cur = Impl.CA.fillAt(Cell.X, Cell.Y, Cell.Z);
		if (Cur < 255)
		{
			Impl.CA.addWater(Cell.X, Cell.Y, Cell.Z, uint32_t(255 - Cur));
			MarkColumnDirty(Impl, Cell.X, Cell.Y, Cell.Z, uint32_t(255 - Cur));
		}
	}

	// C8 mobilize-on-approach: advance the implicit->CA front BEFORE stepping,
	// so any brick this tick's water could flow into is already CA-owned. Over
	// budget simply defers — a deferred brick is still a wall to the CA, so
	// deferring can never leak water (waterca.h). Cheap when nothing is active:
	// a settled or empty CA has no active bricks, so no front to advance.
	if (Impl.Mob.advanceFront(Impl.CA) > 0)
	{
		MarkMobilizedBricksDirty(Impl);
	}

	// --- W4: the coupled shallow-water half of the step (ADR-0004) ---------
	//
	// TICK ORDER IS coupler.step(); grid.step(); ca.step(); AND IT IS COPIED,
	// NOT REASONED OUT. Every coupling test in voxel-core/tests/test_swe.cpp --
	// swe_coupler_conserves_volume_across_the_boundary,
	// swe_coupler_puncture_meters_the_inrush_then_hands_the_column_over,
	// swe_coupler_ownership_partition_walls_off_and_evacuates -- drives exactly
	// this sequence, so it is the only order the golden-pinned behaviour has
	// ever been observed under. It is also the order the design wants: decide
	// ownership and move water across the boundary FIRST (so no cell is in two
	// domains for any part of the tick), then advance each solver over a
	// partition that is already settled for this tick.
	//
	// PLACED HERE, immediately before the existing CA step, rather than at the
	// top of StepFixed: the reservoir top-up and the C8 mobilize-on-approach
	// front above both INJECT into the CA, and running them before the coupler
	// means an injection is available to be absorbed on the same tick it lands
	// instead of sitting inside a sheet's z-range for a tick (the "bounded
	// standing residue" swe.h S5 describes, which a renderer would have to draw).
	// It also puts every external injection outside the coupled window, which is
	// what makes the ledger below able to tell an injection from a leak.
	const bool bSweArmed = Impl.SweCoupler != nullptr && Impl.SweSheet != nullptr;
	if (bSweArmed)
	{
		const int64_t Before = int64_t(Impl.CA.totalVolume()) + Impl.SweSheet->totalVolume();
		// Everything that moved the coupled total since the last window closed
		// came from outside the coupler by definition: spawn, breach, reservoir
		// top-up, mobilization crediting implicit cavern water, or a placement
		// destroying some. Fold it in as injection so the invariant below is
		// about THIS tick's three calls and nothing else.
		Impl.SweInjected += Before - Impl.SweLastCoupledTotal;

		Impl.SweCoupler->step();
		Impl.SweSheet->step();

		MarkSweDepthChangesDirty(Impl);
	}

	Impl.CA.step();
	++Impl.StepsThisWindow;
	Impl.LastSteppedBrickCount = int32(Impl.CA.steppedBrickCount());

	if (bSweArmed)
	{
		// ADR-0004's headline invariant, checked every tick:
		//     ca.totalVolume() + grid.totalVolume() == total injected
		//
		// WHY THIS EXISTS WHEN THE NUMERICS ARE ALREADY GOLDEN-PINNED. swe.h's
		// conservation is structural and test_swe.cpp asserts it per tick with
		// an independent re-sum. None of that covers a single line of the code
		// in THIS file, which is where the grid gets built, where the beds get
		// seated, which columns get seeded, and when the sheet gets flushed --
		// all of it new, none of it under test, and every one of those the kind
		// of thing that loses water quietly. A wrong bed or a double-counted
		// seed shows up here on the first tick rather than as a shallower lake
		// after a reload.
		//
		// ensureMsgf, not check: this is a dev cvar and a conservation slip is a
		// bug to investigate, not a reason to take an editor session down with
		// it. Fires once per unique site by construction, and the Error log
		// below carries the numbers on every occurrence.
		const int64_t After = int64_t(Impl.CA.totalVolume()) + Impl.SweSheet->totalVolume();
		Impl.SweLastCoupledTotal = After;
		if (After != Impl.SweInjected)
		{
			++Impl.SweConservationFailures;
			const int64_t Delta = After - Impl.SweInjected;
			ensureMsgf(false,
			           TEXT("voxel.Water.SWE conservation FAILED: ca+grid=%lld, injected=%lld, delta=%+lld"),
			           (long long)After, (long long)Impl.SweInjected, (long long)Delta);
			UE_LOG(LogVoxelWater, Error,
			       TEXT("voxel.Water.SWE conservation FAILED on the coupled step (ADR-0004 invariant ca.totalVolume() + ")
			       TEXT("grid.totalVolume() == injected): ca=%llu + sheet=%lld = %lld, expected %lld, delta %+lld ")
			       TEXT("(failure #%lld). The coupled window is exactly coupler.step(); grid.step(); ca.step() -- every ")
			       TEXT("other injection is folded in outside it -- so this is the engine wiring in ")
			       TEXT("VoxelWaterSubsystem.cpp, not swe.h's numerics."),
			       (unsigned long long)Impl.CA.totalVolume(), (long long)Impl.SweSheet->totalVolume(), (long long)After,
			       (long long)Impl.SweInjected, (long long)Delta, (long long)Impl.SweConservationFailures);
			// Re-seat so the next tick reports its OWN delta rather than
			// re-reporting this one forever. The running total stays honest
			// about where the water actually is.
			Impl.SweInjected = After;
		}

		// 1Hz status, on the water log rather than the perf log: this is a
		// correctness/diagnostic line for a dev flag, not a budget number, and
		// while the renderer cannot draw sheet depth it is the ONLY way to see
		// that promoted water still exists.
		if (NowWorldSeconds - Impl.SweLastStatusWorldSeconds >= 1.0)
		{
			Impl.SweLastStatusWorldSeconds = NowWorldSeconds;
			const vxc::SweVelocity V = Impl.SweSheet->velocityAt(
				Impl.SweSheet->originVx() + Impl.SweSheet->sizeX() / 2,
				Impl.SweSheet->originVy() + Impl.SweSheet->sizeY() / 2);
			UE_LOG(LogVoxelWater, Log,
			       TEXT("SwePerf: sweColumns=%d sheetVolume=%lld caVolume=%llu punctured=%d toCA=%lld toSWE=%lld ")
			       TEXT("centreVel=(%d,%d) mm/s conservationFailures=%lld (sheet volume IS drawn -- ADR-0004 ")
			       TEXT("'Renderer' union is wired; see UnionSweFill)"),
			       Impl.SweCoupler->sweColumnCount(), (long long)Impl.SweSheet->totalVolume(),
			       (unsigned long long)Impl.CA.totalVolume(), Impl.SweCoupler->lastPuncturedCount(),
			       (long long)Impl.SweCoupler->transferredToCA(), (long long)Impl.SweCoupler->transferredToSWE(),
			       V.xMmPerSec, V.yMmPerSec, (long long)Impl.SweConservationFailures);
		}
	}

	// W5: the active set is now a RENDER input as well as a re-mesh trigger --
	// it is the foam signal, written into vertex colour A by both paths (see
	// FVoxelWaterImpl::ActiveBricks and ApplyWaterBrickPooled's Params).
	//
	// THE SETTLE EDGE IS THE WHOLE REASON THIS IS A DIFF AND NOT A LOOP. A brick
	// that stops being active stops being marked dirty, so it stops being
	// re-meshed -- and its last re-mesh was one where it WAS active. Foam would
	// therefore switch on correctly and then never switch off, freezing whitewater
	// onto water that has been perfectly still for minutes. Marking the bricks
	// that LEFT the set dirty costs exactly one extra re-mesh per brick per
	// activity episode, and it is the cheapest possible fix: the alternative is a
	// per-brick decay timer that has to re-publish on a schedule of its own.
	//
	// Deliberately NOT added to DirtySinceLastBroadcast: a settle carries no fill
	// change, so replicating it would spend bandwidth on a diff that is empty.
	TSet<VoxelCoords::FVoxelCoord> NowActive;
	NowActive.Reserve(int32(Impl.CA.activeBricks().size()));
	for (const vxc::BrickKey& K : Impl.CA.activeBricks())
	{
		const VoxelCoords::FVoxelCoord C = ToCoord(K);
		NowActive.Add(C);
		Impl.DirtyBricks.Add(C);
		Impl.DirtySinceLastBroadcast.Add(C);
	}
	for (const VoxelCoords::FVoxelCoord& C : Impl.ActiveBricks)
	{
		if (!NowActive.Contains(C))
		{
			Impl.DirtyBricks.Add(C);
		}
	}
	Impl.ActiveBricks = MoveTemp(NowActive);

	// Task item 3: "if steppedBrickCount exceeds a cvar cap ... log-throttle
	// warning (do not explode)". The CA's tick contract (waterca.h) is
	// atomic over its whole active-set snapshot -- there is no safe mid-step
	// cutoff that wouldn't break volume conservation/determinism -- so this
	// is purely a monitoring signal, not a clamp.
	const int32 Cap = VoxelDebug::GetWaterMaxActiveBricks();
	if (Cap > 0 && Impl.LastSteppedBrickCount > Cap && (NowWorldSeconds - Impl.LastBudgetWarnWorldSeconds) > 5.0)
	{
		Impl.LastBudgetWarnWorldSeconds = NowWorldSeconds;
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("WaterCA over budget: steppedBrickCount=%d > voxel.Water.MaxActiveBricks=%d (tick ran in full -- ")
		       TEXT("no safe mid-step cutoff exists; raise the cap or investigate runaway spread)."),
		       Impl.LastSteppedBrickCount, Cap);
	}
}

// Server -> client replication (task item 1), fixed ~5Hz cadence,
// byte-capped per broadcast. No-op (and clears nothing) if there's nothing
// dirty this round.
void BroadcastWaterDiffs(FVoxelWaterImpl& Impl, UWorld& World, float DeltaTime)
{
	Impl.ReplicationAccumSeconds += DeltaTime;
	if (Impl.ReplicationAccumSeconds < FVoxelWaterImpl::ReplicationIntervalSeconds)
	{
		return;
	}
	Impl.ReplicationAccumSeconds = 0.f;

	if (Impl.DirtySinceLastBroadcast.Num() == 0)
	{
		return;
	}

	std::vector<vxc::BrickKey> Keys;
	Keys.reserve(Impl.DirtySinceLastBroadcast.Num());
	for (const VoxelCoords::FVoxelCoord& C : Impl.DirtySinceLastBroadcast) Keys.push_back(ToBrickKey(C));

	TArray<uint8> Bytes;
	int32 EncodedBrickCount = 0;
	size_t ConsumedKeyCount = 0;
	SerializeWaterDiffs(Keys, Impl.CA, FVoxelWaterImpl::MaxDiffBytesPerBroadcast, Bytes, EncodedBrickCount, ConsumedKeyCount);

	for (size_t i = 0; i < ConsumedKeyCount; ++i)
	{
		Impl.DirtySinceLastBroadcast.Remove(ToCoord(Keys[i]));
	}

	if (EncodedBrickCount == 0)
	{
		return; // every dirty brick this round had already emptied again -- nothing to actually tell clients
	}

	if (ConsumedKeyCount < Keys.size())
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("BroadcastWaterDiffs: byte cap reached -- sent %d/%d dirty bricks this round (%d bytes); remainder ")
		       TEXT("deferred to next ~5Hz broadcast."),
		       EncodedBrickCount, (int32)Keys.size(), Bytes.Num());
	}

	AVoxelEditRelay* Relay = nullptr;
	for (TActorIterator<AVoxelEditRelay> It(&World); It; ++It)
	{
		Relay = *It;
		break;
	}
	if (!Relay)
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("BroadcastWaterDiffs: no AVoxelEditRelay in the world -- %d brick diffs not broadcast."),
		       EncodedBrickCount);
		return;
	}

	Relay->MulticastWaterDiffs(Bytes);
	Impl.ReplicatedBytesThisWindow += Bytes.Num();
}
} // namespace

void UVoxelWaterSubsystem::Tick(float DeltaTime)
{
	if (!Impl)
	{
		return;
	}
	// docs/debug-tooling-plan.md P3 / task item 5c perf budget ("<2ms/frame
	// at v0 scale"): wall-clock this whole Tick() body, published unthrottled
	// (same convention as UVoxelWorldSubsystem::FVoxelPerfSnapshot::SubsystemTickMs).
	const double TickStartSeconds = FPlatformTime::Seconds();

	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;

	if (NetMode != NM_Client)
	{
		// W4 (ADR-0004): construct or tear down the shallow-water sheet to match
		// voxel.Water.SWE. Deliberately OUTSIDE the fixed-step loop below, so an
		// arm or a disarm always lands on a step boundary and never between the
		// coupler and the CA -- a grid appearing or vanishing mid-window would
		// break the conservation ledger's before/after pairing for that tick.
		// Cheap when nothing changed: two bool reads and a pointer compare.
		MaybeArmSwe(*Impl, World);

		Impl->TickAccumSeconds += DeltaTime;
		const double NowWorldSeconds = World ? World->GetTimeSeconds() : 0.0;
		int32 StepsThisFrame = 0;
		while (Impl->TickAccumSeconds >= FVoxelWaterImpl::FixedStepSeconds && StepsThisFrame < FVoxelWaterImpl::MaxStepsPerFrame)
		{
			Impl->TickAccumSeconds -= FVoxelWaterImpl::FixedStepSeconds;
			StepFixed(*Impl, NowWorldSeconds);
			++StepsThisFrame;
		}

		if (World && NetMode != NM_Standalone)
		{
			BroadcastWaterDiffs(*Impl, *World, DeltaTime);
		}
	}
	// NM_Client: authority-only simulation (task item 1) -- never steps its
	// own CA; rendering is kept current purely by ApplyReplicatedWaterDiffs.

	RemeshDirtyBricks(*Impl, ChunkOwner, ChunkRoot, WaterMaterial);

	// C7 static cavern lakes. Runs on clients too: the implicit field is pure
	// worldgen, so a client derives WHERE the water is straight from the seed
	// with no replication at all. What does have to replicate is which bricks
	// have MOBILIZED, because that is simulation state -- see
	// ApplyReplicatedWaterDiffs.
	if (World)
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector CameraUU = FVector::ZeroVector;
			FRotator UnusedRot = FRotator::ZeroRotator;
			PC->GetPlayerViewPoint(CameraUU, UnusedRot);
			RefreshImplicitWater(*Impl, CameraUU, ChunkOwner, ChunkRoot, WaterMaterial);
		}
	}

	// --- voxel.Water.GPU pool status, 1Hz -----------------------------------
	//
	// Budgeted before it was needed. A pooled primitive drawing thousands of
	// bricks has no component to select, no per-brick proxy to break on, and no
	// per-brick state of any kind -- so if the pooled water renders wrong or not
	// at all, this line and the pool's own "VoxelWaterPool[x,y,z] draw
	// SUBMITTED/SKIPPED" are the entire first rung of the diagnostic ladder.
	//
	// W5 ADDS THE BUCKET CENSUS, and it is the line to read first when the sort
	// A/B looks wrong. `buckets=1` means every water surface in the world is
	// still sharing one sort key and the split did not take effect -- which is
	// the pre-W5 picture exactly, and would otherwise be indistinguishable from
	// "the split worked and the sort was never the problem".
	// Deliberately mirrors the terrain pool's shape (live entries, quads,
	// free-vs-largest-run, which is the number that tells "genuinely full" from
	// "merely fragmented").
	Impl->PoolLogAccumSeconds += DeltaTime;
	if (Impl->PoolLogAccumSeconds >= 1.0f)
	{
		Impl->PoolLogAccumSeconds = 0.f;
		int32 LiveBuckets = 0;
		int32 DrawingBuckets = 0;
		int32 LiveEntries = 0;
		uint32 UsedQuads = 0;
		uint32 FreeQuads = 0;
		uint32 SmallestLargestRun = MAX_uint32;
		int32 MaxFreeRuns = 0;
		for (const FVoxelWaterImpl::FPoolBucket& Bucket : Impl->PoolBuckets)
		{
			const UVoxelGpuPoolComponent* Pool = Bucket.Pool.Get();
			if (Pool == nullptr)
			{
				continue;
			}
			++LiveBuckets;
			DrawingBuckets += (Pool->GetNumChunks() > 0) ? 1 : 0;
			LiveEntries += Pool->GetNumChunks();
			// GetNumQuads() is the pool's CAPACITY, not its occupancy -- the
			// whole buffer is allocated up front because incremental writes
			// address absolute offsets. Occupancy is capacity minus free.
			UsedQuads += uint32(Pool->GetNumQuads()) - Pool->GetFreeQuads();
			FreeQuads += Pool->GetFreeQuads();
			// The WORST bucket's largest free run, not the sum: fragmentation is
			// a per-allocator property and summing it across pools would report a
			// comfortable number for a set in which one pool is about to start
			// refusing geometry. Same argument for taking the max free-run count.
			SmallestLargestRun = FMath::Min(SmallestLargestRun, Pool->GetLargestFreeRun());
			MaxFreeRuns = FMath::Max(MaxFreeRuns, Pool->GetFreeRunCount());
		}
		if (LiveBuckets > 0)
		{
			UE_LOG(LogVoxelWater, Log,
			       TEXT("VoxelWaterPool: %d buckets (%d drawing, cap %d), %d live entries (%d CA + %d implicit), "
			            "%u quads used, %u free, worst largest run %u, worst free runs %d"),
			       LiveBuckets, DrawingBuckets, kMaxWaterPoolBuckets, LiveEntries, Impl->PoolSlots.Num(),
			       Impl->ImplicitPoolSlots.Num(), UsedQuads, FreeQuads, SmallestLargestRun, MaxFreeRuns);
		}
	}

	// --- Perf snapshot (task item 3), 1Hz -----------------------------------
	Impl->PerfRefreshAccumSeconds += DeltaTime;
	if (Impl->PerfRefreshAccumSeconds >= 1.0f)
	{
		const float Window = Impl->PerfRefreshAccumSeconds;
		Impl->PerfRefreshAccumSeconds = 0.f;

		FVoxelWaterPerfSnapshot Snap;
		Snap.ActiveBricks = int64(Impl->CA.activeBrickCount());
		Snap.StoredBricks = int64(Impl->CA.storedBrickCount());
		Snap.TotalVolume = Impl->CA.totalVolume();
		Snap.StepsPerSec = float(Impl->StepsThisWindow) / Window;
		Snap.LastSteppedBrickCount = Impl->LastSteppedBrickCount;
		Snap.ReplicatedBytesPerSec = float(Impl->ReplicatedBytesThisWindow) / Window;
		Snap.ReservoirCells = Impl->ReservoirCells.Num();

		Snap.TickMs = Impl->LastSnapshot.TickMs; // preserve the always-fresh field across this reassignment
		Impl->LastSnapshot = Snap;
		Impl->StepsThisWindow = 0;
		Impl->ReplicatedBytesThisWindow = 0;

		// Task item 5c perf-budget evidence ("water tick must stay <2ms/frame
		// at v0 scale; report actual"): a 1Hz log-line trail while water is
		// actually active, readable alongside -VoxelPerfRun's own p50/p95
		// frame-time JSON summary. Skipped while the sim is fully idle (no
		// stored bricks, zero volume) so a normal no-water run stays quiet.
		if (Snap.StoredBricks > 0 || Snap.TotalVolume > 0)
		{
			UE_LOG(LogVoxelPerf, Log,
			       TEXT("WaterPerf: activeBricks=%lld stored=%lld volume=%llu steps/s=%.1f tickMs=%.3f replKB/s=%.2f"),
			       Snap.ActiveBricks, Snap.StoredBricks, (unsigned long long)Snap.TotalVolume, Snap.StepsPerSec, Snap.TickMs,
			       Snap.ReplicatedBytesPerSec / 1024.0);
		}
	}

	Impl->LastSnapshot.TickMs = float((FPlatformTime::Seconds() - TickStartSeconds) * 1000.0);
}

TStatId UVoxelWaterSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelWaterSubsystem, STATGROUP_Tickables);
}

uint32 UVoxelWaterSubsystem::SpawnWaterAt(const FVector& WorldLocationUU, uint32 Amount)
{
	if (!Impl)
	{
		return 0;
	}
	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
	if (NetMode == NM_Client)
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("SpawnWaterAt refused on NM_Client -- water sim is authority-only (task item 1)."));
		return 0;
	}

	const VoxelCoords::FVoxelCoord Vc = VoxelCoords::WorldToVoxel(WorldLocationUU);
	const uint32 Placed = Impl->CA.addWater(Vc.X, Vc.Y, Vc.Z, Amount);
	if (Placed > 0)
	{
		MarkColumnDirty(*Impl, Vc.X, Vc.Y, Vc.Z, Placed);
		UE_LOG(LogVoxelWater, Log, TEXT("SpawnWaterAt: placed %u/%u fill units at voxel (%lld,%lld,%lld)"), Placed, Amount,
		       (long long)Vc.X, (long long)Vc.Y, (long long)Vc.Z);
	}
	return Placed;
}

void UVoxelWaterSubsystem::NotifyTerrainVoxelsCleared(const TArray<VoxelCoords::FVoxelCoord>& ClearedVoxels)
{
	if (!Impl || ClearedVoxels.Num() == 0)
	{
		return;
	}
	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
	if (NetMode == NM_Client)
	{
		return; // breach seeding is simulation state, authority-only (same rule as the CA tick)
	}

	TSet<VoxelCoords::FVoxelCoord> ClearedSet(ClearedVoxels);

	static const VoxelCoords::FVoxelCoord kNeighborOffsets[6] = {
		VoxelCoords::FVoxelCoord{1, 0, 0}, VoxelCoords::FVoxelCoord{-1, 0, 0}, VoxelCoords::FVoxelCoord{0, 1, 0},
		VoxelCoords::FVoxelCoord{0, -1, 0}, VoxelCoords::FVoxelCoord{0, 0, 1}, VoxelCoords::FVoxelCoord{0, 0, -1}};

	int32 NewlyBreachedCount = 0;
	for (const VoxelCoords::FVoxelCoord& V : ClearedVoxels)
	{
		if (V.Z >= 0)
		{
			continue; // Reservoir v0/implicit ocean is a below-sea-level concept only (SS3.7)
		}

		// "adjacent cell is implicit-ocean (i.e., non-solid, z<0, outside
		// active water)" -- crucially, a neighbor that's ALSO part of
		// ClearedSet is newly-dug-by-this-same-edit, not pre-existing ocean;
		// excluding it is what stops an isolated dry pocket (all neighbors
		// solid before the dig, hence also cleared-by-this-dig, hence
		// excluded here) from spontaneously flooding itself.
		bool bTouchesOcean = false;
		for (const VoxelCoords::FVoxelCoord& Off : kNeighborOffsets)
		{
			const VoxelCoords::FVoxelCoord N{V.X + Off.X, V.Y + Off.Y, V.Z + Off.Z};
			if (ClearedSet.Contains(N)) continue;
			if (N.Z >= 0) continue;
			if (Impl->Terrain.IsSolidAtVoxel(N.X, N.Y, N.Z)) continue;
			if (Impl->CA.fillAt(N.X, N.Y, N.Z) != 0) continue; // already-tracked active water, not "implicit ocean"
			bTouchesOcean = true;
			break;
		}
		if (!bTouchesOcean)
		{
			continue;
		}

		if (!Impl->ReservoirCells.Contains(V))
		{
			Impl->ReservoirCells.Add(V);
			++NewlyBreachedCount;
		}
		const uint8 Cur = Impl->CA.fillAt(V.X, V.Y, V.Z);
		if (Cur < 255)
		{
			Impl->CA.addWater(V.X, V.Y, V.Z, uint32(255 - Cur));
		}
		MarkColumnDirty(*Impl, V.X, V.Y, V.Z, 255);
	}

	if (NewlyBreachedCount > 0)
	{
		UE_LOG(LogVoxelWater, Log,
		       TEXT("Dig breach: seeded %d Reservoir v0 boundary cell(s) (full fill, continuous top-up) out of %d cleared ")
		       TEXT("voxel(s) examined."),
		       NewlyBreachedCount, ClearedVoxels.Num());
	}
}

void UVoxelWaterSubsystem::NotifyTerrainRegionEdited(const VoxelCoords::FVoxelCoord& MinVoxelIncl,
                                                       const VoxelCoords::FVoxelCoord& MaxVoxelIncl)
{
	if (!Impl)
	{
		return;
	}
	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
	if (NetMode == NM_Client)
	{
		return; // a client's local mirror never runs step()/hydrostaticPass -- nothing to invalidate
	}
	Impl->CA.invalidateSolidRegion(MinVoxelIncl.X, MinVoxelIncl.Y, MinVoxelIncl.Z, MaxVoxelIncl.X, MaxVoxelIncl.Y,
	                               MaxVoxelIncl.Z);

	// ...and then actually WAKE the water this edit can affect. The memo
	// invalidation above only fixes what the CA believes about terrain; without
	// this call a settled body never ticks again and simply ignores the edit
	// (see the header comment on this function, and voxelcore/waterca.h
	// "Terrain-edit reactivation"). Scheduling only: no fill is written, so the
	// conservation ledger is untouched. Water-bearing bricks only, within one
	// brick of the edit -- an edit nowhere near water costs one bounded probe
	// and wakes nothing.
	// C8 mobilize-on-approach, THE SEED (docs/cavern-design.md SS5.2). This has
	// to happen here and not only in the CA-activity front, because digging into
	// the wall of a static lake produces no CA activity whatsoever -- there is no
	// CA water yet -- so without an edit hook the advancing front would have
	// nothing to grow from and the player would face a frozen wall of water.
	// Costs a bounded scan of the edit's own bricks when they hold no implicit
	// water, which is every edit outside a flooded cavern.
	const size_t Converted = Impl->Mob.mobilizeEditRegion(Impl->CA, MinVoxelIncl.X, MinVoxelIncl.Y, MinVoxelIncl.Z,
	                                                      MaxVoxelIncl.X, MaxVoxelIncl.Y, MaxVoxelIncl.Z);
	if (Converted > 0)
	{
		MarkMobilizedBricksDirty(*Impl);
		UE_LOG(LogVoxelWater, Log,
		       TEXT("Mobilized %d cavern water brick(s) on edit [%d,%d,%d]..[%d,%d,%d] (implicit -> CA; ledger %llu debited / %llu credited, shortfall %llu)"),
		       static_cast<int32>(Converted), MinVoxelIncl.X, MinVoxelIncl.Y, MinVoxelIncl.Z, MaxVoxelIncl.X,
		       MaxVoxelIncl.Y, MaxVoxelIncl.Z, (unsigned long long)Impl->Mob.debitedVolume(),
		       (unsigned long long)Impl->Mob.creditedVolume(), (unsigned long long)Impl->Mob.shortfallVolume());
	}

	const size_t Woken = Impl->CA.wakeRegion(MinVoxelIncl.X, MinVoxelIncl.Y, MinVoxelIncl.Z, MaxVoxelIncl.X,
	                                         MaxVoxelIncl.Y, MaxVoxelIncl.Z);
	if (Woken > 0)
	{
		UE_LOG(LogVoxelWater, Verbose,
		       TEXT("NotifyTerrainRegionEdited: woke %d water brick(s) for edit [%d,%d,%d]..[%d,%d,%d]"),
		       static_cast<int32>(Woken), MinVoxelIncl.X, MinVoxelIncl.Y, MinVoxelIncl.Z, MaxVoxelIncl.X,
		       MaxVoxelIncl.Y, MaxVoxelIncl.Z);
	}
}

bool UVoxelWaterSubsystem::IsSolidCacheEnabled() const
{
	return Impl && Impl->CA.solidCacheEnabled();
}

FVoxelWaterPerfSnapshot UVoxelWaterSubsystem::GetPerfSnapshot() const
{
	return Impl ? Impl->LastSnapshot : FVoxelWaterPerfSnapshot{};
}

uint64 UVoxelWaterSubsystem::GetWaterDigest() const
{
	if (!Impl)
	{
		return 0;
	}
	vxc::Digest D;
	Impl->CA.digest(D);
	return D.h;
}

bool UVoxelWaterSubsystem::GetStoredWaterCentroidUU(FVector& OutCenterUU) const
{
	if (!Impl || Impl->CA.storedBrickCount() == 0)
	{
		return false;
	}
	double SumX = 0.0, SumY = 0.0, SumZ = 0.0;
	int64 Count = 0;
	for (const auto& Entry : Impl->CA.bricks())
	{
		const vxc::BrickKey& Key = Entry.first;
		// Brick-center world position (UU): brick origin + half an edge.
		SumX += (double(Key.x) + 0.5) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU;
		SumY += (double(Key.y) + 0.5) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU;
		SumZ += (double(Key.z) + 0.5) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU;
		++Count;
	}
	OutCenterUU = FVector(SumX / double(Count), SumY / double(Count), SumZ / double(Count));
	return true;
}

uint8 UVoxelWaterSubsystem::GetMaxStoredFill() const
{
	if (!Impl)
	{
		return 0;
	}
	uint8_t Max = 0;
	for (const auto& Entry : Impl->CA.bricks())
	{
		const vxc::WaterBrick8& B = Entry.second;
		for (int z = 0; z < vxc::WaterBrick8::kEdge; ++z)
			for (int y = 0; y < vxc::WaterBrick8::kEdge; ++y)
				for (int x = 0; x < vxc::WaterBrick8::kEdge; ++x) Max = FMath::Max(Max, B.get(x, y, z));
	}
	return Max;
}

// --- W4 SWE diagnostics (VoxelWaterSubsystem.h's three PODs) -----------------
//
// Read-only, added for -VoxelSweBreachTest. See the header for why they exist
// at all; the short version is that the sheet lives inside FVoxelWaterImpl and
// the difference between "spreading correctly" and "seated wrong" is a
// per-column comparison, not a total.

bool UVoxelWaterSubsystem::GetSweProbe(FVoxelSweProbe& Out) const
{
	Out = FVoxelSweProbe{};
	if (!Impl)
	{
		return false;
	}
	// CA volume is reported whether or not the sheet is armed -- a CA-only run
	// of the fixture needs exactly this number to compare against.
	Out.CaVolume = int64(Impl->CA.totalVolume());
	if (!Impl->SweSheet || !Impl->SweCoupler)
	{
		return false;
	}

	const vxc::SweGrid& Grid = *Impl->SweSheet;
	Out.bArmed = true;
	Out.SheetVolume = Grid.totalVolume();
	Out.Injected = Impl->SweInjected;
	Out.ConservationFailures = Impl->SweConservationFailures;
	Out.SweColumns = Impl->SweCoupler->sweColumnCount();
	Out.LastPunctured = Impl->SweCoupler->lastPuncturedCount();
	Out.TransferredToCA = Impl->SweCoupler->transferredToCA();
	Out.TransferredToSWE = Impl->SweCoupler->transferredToSWE();
	Out.OriginVx = Grid.originVx();
	Out.OriginVy = Grid.originVy();
	Out.SizeX = Grid.sizeX();
	Out.SizeY = Grid.sizeY();
	Out.SheetScanVoxels = Impl->SweCoupler->config().sheetScanVoxels;
	Out.SettleToleranceFill = vxc::sweSettleTolerance(Grid.config());
	return true;
}

bool UVoxelWaterSubsystem::GetWaterColumnProbe(int64 Vx, int64 Vy, int64 ScanFromVz, int32 ScanVoxels,
                                                FVoxelWaterColumnProbe& Out) const
{
	Out = FVoxelWaterColumnProbe{};
	Out.Vx = Vx;
	Out.Vy = Vy;
	if (!Impl)
	{
		return false;
	}
	ScanVoxels = FMath::Max(ScanVoxels, 1);

	// CA side and terrain side in ONE downward walk, so the two answers are
	// read from the same instant and the same window.
	for (int32 K = 0; K < ScanVoxels; ++K)
	{
		const int64 Vz = ScanFromVz - K;
		const uint8 Fill = Impl->CA.fillAt(Vx, Vy, Vz);
		if (Fill != 0)
		{
			Out.CaColumnFill += int32(Fill);
			if (Out.CaTopFill == 0)
			{
				Out.CaTopFill = Fill;
				Out.CaTopVz = Vz;
			}
		}
		if (!Out.bTerrainTopFound && Impl->Terrain.IsSolidAtVoxel(Vx, Vy, Vz))
		{
			Out.bTerrainTopFound = true;
			Out.TerrainTopSolidVz = Vz;
		}
	}

	if (!Impl->SweSheet || !Impl->SweCoupler)
	{
		return true;
	}
	const vxc::SweGrid& Grid = *Impl->SweSheet;
	Out.bSweArmed = true;
	if (!Grid.inBounds(Vx, Vy))
	{
		return true;
	}
	Out.bInSheet = true;
	Out.bSweOwned = Impl->SweCoupler->isSweColumn(Vx, Vy);
	Out.Bed = Grid.bedAt(Vx, Vy);
	Out.Depth = Grid.depthAt(Vx, Vy);
	const vxc::SweVelocity V = Grid.velocityAt(Vx, Vy);
	Out.VelXMmPerSec = V.xMmPerSec;
	Out.VelYMmPerSec = V.yMmPerSec;
	Out.FluxXFillPerTick = Grid.faceFluxX(Vx, Vy);
	Out.FluxYFillPerTick = Grid.faceFluxY(Vx, Vy);
	return true;
}

bool UVoxelWaterSubsystem::AuditSweBedSeating(FVoxelSweBedAudit& Out) const
{
	Out = FVoxelSweBedAudit{};
	if (!Impl || !Impl->SweSheet || !Impl->SweCoupler)
	{
		return false;
	}
	Out.bArmed = true;

	const vxc::SweGrid& Grid = *Impl->SweSheet;

	// THE SAME callback the arming path built and handed to both SeatSweBedZ
	// and the coupler. Re-deriving it here (rather than using the bare terrain
	// query) is the whole point: the audit must ask the question the seating
	// code asked, or a "mismatch" would just be the mobilizer wrapper.
	vxc::WaterCA::SolidFn Solid = Impl->Mob.makeSolidFn();

	for (int32 Cy = 0; Cy < Grid.sizeY(); ++Cy)
	{
		for (int32 Cx = 0; Cx < Grid.sizeX(); ++Cx)
		{
			const int64 Vx = Grid.originVx() + Cx;
			const int64 Vy = Grid.originVy() + Cy;
			++Out.Columns;

			const int32 StoredBed = Grid.bedAt(Vx, Vy);
			const bool bSweOwned = Impl->SweCoupler->isSweColumn(Vx, Vy);
			if (bSweOwned)
			{
				++Out.SweOwned;
			}
			if (Solid(Vx, Vy, StoredBed) != vxc::MAT_AIR)
			{
				++Out.Seated;
			}

			// (a) Re-seat with the identical function and the live world.
			const double SurfaceZUU = Impl->Terrain.GetSurfaceHeightUU(double(Vx) * VoxelCoords::VoxelSizeUU,
			                                                           double(Vy) * VoxelCoords::VoxelSizeUU);
			const int64 SurfaceVz = int64(FMath::FloorToDouble(SurfaceZUU / VoxelCoords::VoxelSizeUU));
			bool bFound = false;
			const int32 Reseated = SeatSweBedZ(Solid, Vx, Vy, SurfaceVz, bFound);
			if (bFound && Reseated != StoredBed)
			{
				++Out.Mismatched;
				if (bSweOwned)
				{
					++Out.SweOwnedMismatched;
				}
				const int32 AbsDelta = FMath::Abs(Reseated - StoredBed);
				Out.SumAbsDeltaVoxels += AbsDelta;
				if (AbsDelta > Out.MaxAbsDeltaVoxels)
				{
					Out.MaxAbsDeltaVoxels = AbsDelta;
					Out.WorstVx = Vx;
					Out.WorstVy = Vy;
					Out.WorstStoredBed = StoredBed;
					Out.WorstReseatedBed = Reseated;
				}
			}

			// (b) The bare-terrain cross-check, over the same window, with no
			// mobilizer wrapper. Differences here are EXPECTED over implicit
			// cavern water and are reported separately for that reason.
			const int64 TopZ = SurfaceVz + kSweBedScanAboveVoxels;
			bool bTerrainFound = false;
			int64 TerrainTop = 0;
			for (int32 K = 0; K < kSweBedScanVoxels + kSweBedScanAboveVoxels; ++K)
			{
				if (Impl->Terrain.IsSolidAtVoxel(Vx, Vy, TopZ - K))
				{
					bTerrainFound = true;
					TerrainTop = TopZ - K;
					break;
				}
			}
			if (bTerrainFound && TerrainTop != int64(StoredBed))
			{
				++Out.MismatchedVsTerrain;
			}
		}
	}
	return true;
}

// --- C7/C8 underground water ------------------------------------------------

bool UVoxelWaterSubsystem::GetCavernFloodZUU(double WorldXUU, double WorldYUU, double& OutFloodZUU) const
{
	if (!Impl)
	{
		return false;
	}
	const int64 Vx = int64(FMath::FloorToDouble(WorldXUU / VoxelCoords::VoxelSizeUU));
	const int64 Vy = int64(FMath::FloorToDouble(WorldYUU / VoxelCoords::VoxelSizeUU));
	const int32 FloodZMm = Impl->Amp.columnCached(Vx, Vy).cavern.floodZMm;
	if (FloodZMm == INT32_MIN)
	{
		return false;
	}
	OutFloodZUU = double(FloodZMm) / 10.0; // mm -> UU (1 UU = 10 mm)
	return true;
}

bool UVoxelWaterSubsystem::FindFloodedCavernNear(const FVector& OriginUU, double SearchRadiusUU,
                                                  FVector& OutWaterSurfaceUU) const
{
	if (!Impl)
	{
		return false;
	}
	// 30 m grid: under a cavern site's ~36 m reach radius, so a site inside the
	// search radius cannot be stepped over.
	constexpr double StepUU = 3000.0;
	const int32 Steps = FMath::Max(1, int32(SearchRadiusUU / StepUU));

	// Contiguous open voxels spanning the waterline at this column: how much
	// lake there is below and how much headroom above. This, not a two-point
	// probe, is what tells the middle of a chamber from its tapering edge.
	constexpr int32 kProbeVoxels = 60; // +/- 6 m
	constexpr int32 kMinAirSpanVoxels = 40; // 4 m of open water + headroom
	// Scored as 2*min(headroom, depth) rather than the plain sum: a column with
	// 6 m of water and no roof clearance scores the same as one with 6 m of
	// roof and no water under the plain sum, and only the BALANCED one gives a
	// camera both a lake to look at and somewhere to stand.
	const auto AirSpanAround = [this](int64 Vx, int64 Vy, int64 Vz) -> int32
	{
		int32 Above = 0, Below = 0;
		for (int32 D = 0; D < kProbeVoxels; ++D)
		{
			if (Impl->Terrain.IsSolidAtVoxel(Vx, Vy, Vz + D)) break;
			++Above;
		}
		for (int32 D = 1; D < kProbeVoxels; ++D)
		{
			if (Impl->Terrain.IsSolidAtVoxel(Vx, Vy, Vz - D)) break;
			++Below;
		}
		return 2 * FMath::Min(Above, Below);
	};

	// Expanding rings, so the NEAREST flooded cavern wins rather than whichever
	// one a raster scan happens to reach first.
	for (int32 Ring = 0; Ring <= Steps; ++Ring)
	{
		for (int32 Iy = -Ring; Iy <= Ring; ++Iy)
		{
			for (int32 Ix = -Ring; Ix <= Ring; ++Ix)
			{
				if (Ring > 0 && FMath::Abs(Ix) != Ring && FMath::Abs(Iy) != Ring)
				{
					continue; // interior of the ring: already tested on an earlier ring
				}
				const double Wx = OriginUU.X + double(Ix) * StepUU;
				const double Wy = OriginUU.Y + double(Iy) * StepUU;
				const int64 Vx = int64(FMath::FloorToDouble(Wx / VoxelCoords::VoxelSizeUU));
				const int64 Vy = int64(FMath::FloorToDouble(Wy / VoxelCoords::VoxelSizeUU));

				// BY VALUE, not by reference. Amplifier::columnCached returns a
				// reference into a per-thread memo that the NEXT columnCached
				// call overwrites, and the refinement loop below makes hundreds
				// of them -- reading Col afterwards is a dangling read (it was
				// silently reporting the wrong cavern seg count before this).
				const vxc::ColumnSample Col = Impl->Amp.columnCached(Vx, Vy);
				if (Col.cavern.floodZMm == INT32_MIN || Col.cavern.count == 0)
				{
					continue; // dry, or no room actually reaches this column
				}

				// A column can carry a site's flood level while its own rooms are
				// truncated away by the roof/bedrock clamps, so require the
				// column to be genuinely open here: water below the flood level
				// and real headroom above it.
				const int64 FloodVz = int64(Col.cavern.floodZMm) / vxc::kVoxelSizeMm;
				if (AirSpanAround(Vx, Vy, FloodVz) < kMinAirSpanVoxels)
				{
					continue;
				}

				// REFINE. The search grid is 30 m and a room is 24-56 m across,
				// so the first hit is usually somewhere on the reach disc's edge,
				// where the roof closes down and a camera placed "just above the
				// water" ends up inside rock -- which renders as a see-through
				// world, because the surrounding geometry is all backfaces.
				// Walk a local grid and take the column with the most open air
				// around the waterline, i.e. the middle of the chamber.
				double BestWx = Wx, BestWy = Wy;
				int32 BestSpan = AirSpanAround(Vx, Vy, FloodVz);
				constexpr double RefineStepUU = 200.0; // 2 m
				constexpr int32 RefineSteps = 12;      // +/- 24 m
				for (int32 Ry = -RefineSteps; Ry <= RefineSteps; ++Ry)
				{
					for (int32 Rx = -RefineSteps; Rx <= RefineSteps; ++Rx)
					{
						const double Cx = Wx + double(Rx) * RefineStepUU;
						const double Cy = Wy + double(Ry) * RefineStepUU;
						const int64 Cvx = int64(FMath::FloorToDouble(Cx / VoxelCoords::VoxelSizeUU));
						const int64 Cvy = int64(FMath::FloorToDouble(Cy / VoxelCoords::VoxelSizeUU));
						// Must belong to the SAME lake, not a neighbouring site.
						if (Impl->Amp.columnCached(Cvx, Cvy).cavern.floodZMm != Col.cavern.floodZMm)
						{
							continue;
						}
						const int32 Span = AirSpanAround(Cvx, Cvy, FloodVz);
						if (Span > BestSpan)
						{
							BestSpan = Span;
							BestWx = Cx;
							BestWy = Cy;
						}
					}
				}

				OutWaterSurfaceUU = FVector(BestWx, BestWy, double(Col.cavern.floodZMm) / 10.0);
				UE_LOG(LogVoxelWater, Log,
				       TEXT("FindFloodedCavernNear: flooded cavern at (%.0f, %.0f) UU, water surface z=%.0f UU (floodZ %d mm), %d seg(s), open air span %d voxels (%.1f m) around the waterline"),
				       BestWx, BestWy, OutWaterSurfaceUU.Z, Col.cavern.floodZMm, Col.cavern.count, BestSpan,
				       double(BestSpan) * VoxelCoords::VoxelSizeUU / 100.0);
				return true;
			}
		}
	}
	UE_LOG(LogVoxelWater, Warning, TEXT("FindFloodedCavernNear: no flooded cavern within %.0f UU of (%.0f, %.0f)."),
	       SearchRadiusUU, OriginUU.X, OriginUU.Y);
	return false;
}

int32 UVoxelWaterSubsystem::CarveCavernOutflow(const FVector& LakeSurfaceUU)
{
	if (!Impl)
	{
		return 0;
	}

	// Walk +X until the flood field runs out -- that is the edge of this site's
	// reach disc, and the first place water can actually go.
	constexpr double ProbeStepUU = 200.0;  // 2 m
	constexpr double MaxProbeUU = 8000.0;  // 80 m, comfortably past a site's reach
	double ExitX = LakeSurfaceUU.X;
	bool bFoundExit = false;
	for (double D = ProbeStepUU; D <= MaxProbeUU; D += ProbeStepUU)
	{
		double Unused = 0.0;
		if (!GetCavernFloodZUU(LakeSurfaceUU.X + D, LakeSurfaceUU.Y, Unused))
		{
			ExitX = LakeSurfaceUU.X + D;
			bFoundExit = true;
			break;
		}
	}
	if (!bFoundExit)
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("CarveCavernOutflow: no dry column within %.0f UU -- lake has nowhere to drain."), MaxProbeUU);
		return 0;
	}

	// A tunnel just under the water surface, so it takes the top of the lake
	// and the level visibly drops, then a shaft down past the dry edge so the
	// water keeps running away instead of backing up.
	constexpr double TunnelRadiusUU = 250.0; // 2.5 m
	constexpr double StepUU = 150.0;         // 1.5 m, well under the radius: no gaps
	const double TunnelZ = LakeSurfaceUU.Z - 100.0; // 1 m below the surface

	int32 Removed = 0;
	int32 Spheres = 0, ProductiveSpheres = 0;
	for (double X = LakeSurfaceUU.X; X <= ExitX + 400.0; X += StepUU)
	{
		// Three stacked passes rather than one: a single-radius tunnel at a
		// fixed z can thread a chamber that is open at that exact height for
		// its whole length and remove NOTHING, which is a silent no-drain.
		// Spanning 5 m vertically guarantees it meets the chamber's rock.
		for (double DZ = -200.0; DZ <= 200.0; DZ += 200.0)
		{
			const int32 R = Impl->Terrain.CarveSphere(FVector(X, LakeSurfaceUU.Y, TunnelZ + DZ), TunnelRadiusUU, 0.0);
			++Spheres;
			if (R > 0) ++ProductiveSpheres;
			Removed += R;
		}
	}
	// Drop shaft at the dry end.
	for (double Z = TunnelZ; Z >= TunnelZ - 1500.0; Z -= StepUU)
	{
		Removed += Impl->Terrain.CarveSphere(FVector(ExitX + 400.0, LakeSurfaceUU.Y, Z), TunnelRadiusUU, 0.0);
	}

	int32 MobBricks = 0;
	uint64 Deb = 0, Cred = 0, Short = 0;
	GetMobilizationStats(MobBricks, Deb, Cred, Short);
	UE_LOG(LogVoxelWater, Log,
	       TEXT("CarveCavernOutflow: removed %d voxels via %d/%d productive sphere(s), exit at x=%.0f UU. Mobilized %d brick(s), ledger %llu debited / %llu credited, shortfall %llu."),
	       Removed, ProductiveSpheres, Spheres, ExitX, MobBricks, (unsigned long long)Deb, (unsigned long long)Cred,
	       (unsigned long long)Short);
	if (Removed == 0)
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("CarveCavernOutflow: carved nothing -- every target cell was already air, so no edit fired and nothing mobilized."));
	}
	return Removed;
}

uint8 UVoxelWaterSubsystem::GetImplicitFillAtWorld(const FVector& WorldUU) const
{
	if (!Impl)
	{
		return 0;
	}
	return Impl->Mob.implicitFillAt(int64(FMath::FloorToDouble(WorldUU.X / VoxelCoords::VoxelSizeUU)),
	                                 int64(FMath::FloorToDouble(WorldUU.Y / VoxelCoords::VoxelSizeUU)),
	                                 int64(FMath::FloorToDouble(WorldUU.Z / VoxelCoords::VoxelSizeUU)));
}

void UVoxelWaterSubsystem::GetMobilizationStats(int32& OutMobilizedBricks, uint64& OutDebited, uint64& OutCredited,
                                                 uint64& OutShortfall) const
{
	OutMobilizedBricks = Impl ? int32(Impl->Mob.mobilizedBricks().size()) : 0;
	OutDebited = Impl ? Impl->Mob.debitedVolume() : 0;
	OutCredited = Impl ? Impl->Mob.creditedVolume() : 0;
	OutShortfall = Impl ? Impl->Mob.shortfallVolume() : 0;
}

bool UVoxelWaterSubsystem::ApplyReplicatedWaterDiffs(const TArray<uint8>& Bytes)
{
	if (!Impl)
	{
		return false;
	}
	vxc::ByteReader R(Bytes.GetData(), size_t(Bytes.Num()));
	uint32_t Count = 0;
	if (!R.u32(Count))
	{
		return Bytes.Num() == 0; // an empty batch is valid (nothing to apply)
	}

	for (uint32_t i = 0; i < Count; ++i)
	{
		int32_t X = 0, Y = 0, Z = 0;
		if (!R.i32(X) || !R.i32(Y) || !R.i32(Z))
		{
			return false;
		}
		const vxc::BrickKey Key{X, Y, Z};

		// HOW A CLIENT AGREES WITH THE SERVER ON WHAT HAS MOBILIZED (C8).
		// Mobilization is authority-only: it depends on where players dug and
		// when, so a client that ran its own front off its replication-mirror
		// CA would drift the instant a packet was late. Instead the authority
		// mobilizes and the client learns it from THIS stream, at zero extra
		// bytes -- mobilizing a brick writes fill into it, which dirties it,
		// which is exactly what puts it in this batch. So "the server sent me
		// authoritative fill for this brick" IS "this brick has mobilized",
		// and marking it here flips the client's own ownership partition in
		// the same packet that delivers the water.
		//
		// markMobilized credits nothing, which is the point: those units are
		// already in the replicated fill below, and crediting them again would
		// duplicate them (waterca.h).
		Impl->Mob.markMobilized(Key);

		for (int z = 0; z < vxc::WaterBrick8::kEdge; ++z)
		{
			for (int y = 0; y < vxc::WaterBrick8::kEdge; ++y)
			{
				for (int x = 0; x < vxc::WaterBrick8::kEdge; ++x)
				{
					uint8_t Fill = 0;
					if (!R.u8(Fill))
					{
						return false;
					}
					const int64_t Vx = int64_t(Key.x) * vxc::WaterBrick8::kEdge + x;
					const int64_t Vy = int64_t(Key.y) * vxc::WaterBrick8::kEdge + y;
					const int64_t Vz = int64_t(Key.z) * vxc::WaterBrick8::kEdge + z;
					Impl->CA.setReplicatedFill(Vx, Vy, Vz, Fill);
				}
			}
		}
		Impl->DirtyBricks.Add(ToCoord(Key));
	}

	// Tear down any implicit-water components for bricks this batch converted,
	// so the client's handover looks identical to the authority's.
	MarkMobilizedBricksDirty(*Impl);

	// Clients render too (v0: same re-mesh path, driven by replicated state
	// instead of a locally-stepped CA).
	RemeshDirtyBricks(*Impl, ChunkOwner, ChunkRoot, WaterMaterial);
	return true;
}

// --- ADR-0005 water persistence: public save + round-trip verification ------

bool UVoxelWaterSubsystem::SaveWaterState() const
{
	if (!Impl)
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("SaveWaterState: no water Impl -- nothing to save."));
		return false;
	}
	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
	if (NetMode == NM_Client)
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("SaveWaterState: refused on NM_Client -- only the authority (server/listen/standalone) has an authoritative CA to persist."));
		return false;
	}

	// W4 (ADR-0004): flush the shallow-water sheet back into the CA BEFORE the
	// serializer looks at it. Sheet depth is real volume in the same fill units
	// (swe.h S2, "255 fill units == one full voxel"), and vxc::WaterState::
	// serialize walks the CA's bricks and the mobilizer's key set -- it has
	// never heard of an SweGrid and must not have to. Flushing here is what
	// keeps ADR-0005's blob format, kWaterCAVersion and the whole load path
	// completely untouched by this feature: a save written with voxel.Water.SWE
	// armed is byte-comparable with one written without it, because by the time
	// the bytes are produced the sheet is empty and every unit is back in a CA
	// cell. No-op (and silent) when nothing is armed. Total no-op cost on the
	// untouched path: one null pointer test.
	//
	// A const method mutating simulation state deserves a word: TUniquePtr's
	// constness is shallow, so *Impl is a mutable FVoxelWaterImpl& here, and the
	// flush is genuinely part of "produce a correct save" rather than a side
	// effect of it. The alternative -- serialising and losing the sheet -- is
	// silent data loss, which is not a trade const-correctness wins.
	FlushSweIntoCA(*Impl, TEXT("SaveWaterState"));

	return SaveWaterStateToDisk(*Impl, Impl->Terrain.GetSeed());
}

bool UVoxelWaterSubsystem::VerifyWaterDiskRoundTrip(uint64& OutLiveDigest, uint64& OutReloadedDigest, uint64& OutLiveVolume,
                                                     uint64& OutReloadedVolume, int32& OutLiveMobilized, int32& OutReloadedMobilized)
{
	OutLiveDigest = OutReloadedDigest = 0;
	OutLiveVolume = OutReloadedVolume = 0;
	OutLiveMobilized = OutReloadedMobilized = 0;
	if (!Impl)
	{
		return false;
	}

	// Live side.
	{
		vxc::Digest D;
		Impl->CA.digest(D);
		OutLiveDigest = D.h;
		OutLiveVolume = Impl->CA.totalVolume();
		OutLiveMobilized = int32(Impl->Mob.mobilizedBricks().size());
	}

	// Read back the ACTUAL on-disk blob (proving the file wiring, not just the
	// in-memory serializer) and apply it into a FRESH CA + mobilizer pair built
	// over the same implicit-flood / terrain-solidity callbacks the live pair
	// uses (FVoxelWaterImpl's constructor) -- this is the load path a genuine
	// reload runs, isolated so it never touches live state.
	const FString Path = GetWaterSaveFilePath(Impl->Terrain.GetSeed());
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("VerifyWaterDiskRoundTrip: could not read %s -- run SaveWaterState first."), *Path);
		return false;
	}

	FVoxelWaterImpl& I = *Impl;
	vxc::WaterMobilizer FreshMob(
		[&I](int64_t vx, int64_t vy, int64_t vz) -> uint8_t
		{ return vxc::cavernFloodedAt(I.Amp.columnCached(vx, vy).cavern, vz) ? uint8_t(255) : uint8_t(0); },
		[&I](int64_t vx, int64_t vy, int64_t vz) -> vxc::MaterialId
		{ return I.Terrain.IsSolidAtVoxel(vx, vy, vz) ? vxc::MAT_ROCK : vxc::MAT_AIR; });
	vxc::WaterCA FreshCA(FreshMob.makeSolidFn());

	if (!vxc::WaterState::load(Bytes.GetData(), (size_t)Bytes.Num(), FreshCA, FreshMob))
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("VerifyWaterDiskRoundTrip: vxc::WaterState::load REFUSED the on-disk blob %s."), *Path);
		return false;
	}

	vxc::Digest RD;
	FreshCA.digest(RD);
	OutReloadedDigest = RD.h;
	OutReloadedVolume = FreshCA.totalVolume();
	OutReloadedMobilized = int32(FreshMob.mobilizedBricks().size());
	return true;
}

// --- voxel.SpawnWater console command (task item 2, dev tool) --------------

namespace
{
// ADR-0005 dev/verification convenience: force a water-state save without
// waiting for shutdown. Mirrors the voxel.SaveWorld console command
// (VoxelWorldSubsystem.cpp) -- same manual-save-alongside-autosave shape.
FAutoConsoleCommandWithWorld CVarVoxelSaveWater(
	TEXT("voxel.SaveWater"),
	TEXT("ADR-0005: write the water-state blob (Saved/VoxelWorlds/<seed>.vxwater) now, beside the terrain edit log."),
	FConsoleCommandWithWorldDelegate::CreateLambda(
		[](UWorld* World)
		{
			if (!World)
			{
				return;
			}
			if (UVoxelWaterSubsystem* Water = World->GetSubsystem<UVoxelWaterSubsystem>())
			{
				Water->SaveWaterState();
			}
			else
			{
				UE_LOG(LogVoxelWater, Warning, TEXT("voxel.SaveWater: no UVoxelWaterSubsystem in this world."));
			}
		}));

// Dumps water at the local player's crosshair. Shared by voxel.SpawnWater and
// voxel.Water.SpawnIn.
void SpawnWaterAtCrosshair(UWorld* World, uint32 Amount, const TCHAR* Caller)
{
	UVoxelWaterSubsystem* WaterSubsystem = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	UVoxelWorldSubsystem* Terrain = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!WaterSubsystem || !Terrain || !PC || !PC->PlayerCameraManager)
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("%s: missing subsystem/camera; nothing spawned."), Caller);
		return;
	}

	const FVector CamLoc = PC->PlayerCameraManager->GetCameraLocation();
	const FVector CamDir = PC->PlayerCameraManager->GetCameraRotation().Vector();

	FVector HitVoxelCenterUU, PrevVoxelCenterUU;
	constexpr double MaxDistUU = 6400.0; // 64m, generous crosshair reach
	FVector TargetUU;
	if (Terrain->RaycastVoxelWorld(CamLoc, CamDir, MaxDistUU, HitVoxelCenterUU, PrevVoxelCenterUU))
	{
		TargetUU = PrevVoxelCenterUU; // last empty voxel before the hit surface -- mirrors TryPlace's aim
	}
	else
	{
		TargetUU = CamLoc + CamDir * 500.0; // no hit: 5m in front of the camera
	}

	const uint32 Placed = WaterSubsystem->SpawnWaterAt(TargetUU, Amount);
	UE_LOG(LogVoxelWater, Log, TEXT("%s: requested %u, placed %u at (%.0f,%.0f,%.0f)"), Caller, Amount, Placed,
	       TargetUU.X, TargetUU.Y, TargetUU.Z);
}

FAutoConsoleCommandWithWorldAndArgs CVarVoxelSpawnWater(
	TEXT("voxel.SpawnWater"),
	TEXT("voxel.SpawnWater <amount> -- dev tool: dumps <amount> water fill units at the local player's crosshair (W2)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (!World)
			{
				return;
			}
			if (Args.Num() < 1)
			{
				UE_LOG(LogVoxelWater, Warning, TEXT("voxel.SpawnWater <amount> -- missing amount argument."));
				return;
			}
			SpawnWaterAtCrosshair(World, uint32(FMath::Max(0, FCString::Atoi(*Args[0]))), TEXT("voxel.SpawnWater"));
		}));

// The headless-verification form of voxel.SpawnWater.
//
// A water A/B screenshot needs water actually in frame, and at a surface anchor
// there may be none: UWaterChunkComponent geometry comes only from the CA
// (player-poured or breached) and from implicit CAVERN lakes, which are
// underground. AVoxelOceanActor's plane is a different primitive with a
// different material and is unaffected by voxel.Water.GPU either way -- so
// screenshotting an anchor that merely has the OCEAN in it would compare two
// images in which no water-brick geometry appears at all, and pass regardless
// of whether the pool works.
//
// -ExecCmds fires at startup, when there is no pawn, no camera and no streamed
// terrain to raycast against, so the spawn has to be deferred. Pairs with
// voxel.Debug.ShotIn: "voxel.Water.GPU 1, voxel.Water.SpawnIn 20 400000,
// voxel.Debug.ShotIn 30" pours at 20 s and shoots at 30 s, by which time the
// cascade has filled and the CA has settled the pour.
FAutoConsoleCommandWithWorldAndArgs CVarVoxelWaterSpawnIn(
	TEXT("voxel.Water.SpawnIn"),
	TEXT("voxel.Water.SpawnIn <seconds> <amount> -- run voxel.SpawnWater <amount> N seconds from now. Headless ")
	TEXT("verification aid: -ExecCmds runs before any pawn or streamed terrain exists, so an immediate pour has ")
	TEXT("nothing to aim at."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (!World)
			{
				return;
			}
			const float Delay = (Args.Num() > 0) ? FMath::Max(0.1f, FCString::Atof(*Args[0])) : 20.0f;
			const uint32 Amount = (Args.Num() > 1) ? uint32(FMath::Max(0, FCString::Atoi(*Args[1]))) : 400000u;

			TWeakObjectPtr<UWorld> WeakWorld(World);
			FTimerHandle Handle;
			World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([WeakWorld, Amount]()
			{
				UWorld* W = WeakWorld.Get();
				UVoxelWaterSubsystem* WaterSubsystem = W ? W->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
				UVoxelWorldSubsystem* Terrain = W ? W->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
				APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
				if (!WaterSubsystem || !Terrain || !PC || !PC->PlayerCameraManager)
				{
					UE_LOG(LogVoxelWater, Warning, TEXT("voxel.Water.SpawnIn: missing subsystem/camera; nothing spawned."));
					return;
				}

				// Aim at the GROUND, not at the crosshair.
				//
				// voxel.SpawnWater's crosshair raycast is the right tool in
				// play and the wrong one here. The default spawn pose is +5m
				// above the surface looking horizontally (VoxelEarthGameMode's
				// RestartPlayer), and at a summit anchor like
				// -VoxelSpawnAt=-84480,53760 that aims at the horizon: the
				// 64 m raycast hits nothing, the pour lands in mid-air, and
				// the water falls out of the bottom of the frame while the
				// screenshot photographs empty sky. Sampling the surface
				// height a fixed distance ahead puts the pour on solid ground
				// every time regardless of terrain, and pointing the camera
				// at it makes the A/B frame the water rather than the horizon.
				const FVector CamLoc = PC->PlayerCameraManager->GetCameraLocation();
				const FRotator CamRot = PC->PlayerCameraManager->GetCameraRotation();
				FVector Ahead = CamRot.Vector();
				Ahead.Z = 0.0;
				Ahead = Ahead.GetSafeNormal();
				if (Ahead.IsNearlyZero())
				{
					Ahead = FVector(1, 0, 0);
				}

				constexpr double AheadUU = 1200.0;   // 12 m: clear of the pawn, well inside the near ring
				constexpr double AboveUU = 300.0;    // pour from 3 m up so it lands rather than clipping into rock
				const FVector GroundXY = CamLoc + Ahead * AheadUU;
				const double SurfaceZUU = Terrain->GetSurfaceHeightUU(GroundXY.X, GroundXY.Y);
				const FVector TargetUU(GroundXY.X, GroundXY.Y, SurfaceZUU + AboveUU);

				const uint32 Placed = WaterSubsystem->SpawnWaterAt(TargetUU, Amount);
				PC->SetControlRotation((TargetUU - CamLoc).Rotation());
				UE_LOG(LogVoxelWater, Log,
				       TEXT("voxel.Water.SpawnIn: requested %u, placed %u at (%.0f,%.0f,%.0f); camera now looks at it"),
				       Amount, Placed, TargetUU.X, TargetUU.Y, TargetUU.Z);
			}), Delay, false);
			UE_LOG(LogVoxelWater, Log, TEXT("voxel.Water.SpawnIn: %u fill units scheduled in %.1f s"), Amount, Delay);
		}));
} // namespace
