#include "VoxelWorldSubsystem.h"

#include "VoxelChunkComponent.h"
#include "VoxelCoords.h"
#include "VoxelDebug.h"
#include "VoxelEarth.h"
#include "VoxelMeshTypes.h"
// M3 wave 1 (docs/m3-plan.md): role split needs the relay actor (broadcast
// target) and the player controller (owns the client->server intent RPCs --
// see VoxelEditRelay.h's class comment for why those can't live on the
// shared relay actor itself). Both are ordinary same-module UE classes, not
// the voxel-core boundary, so including them from this .cpp is fine.
#include "VoxelEarthPlayerController.h"
#include "VoxelDebris.h" // M5 destruction: cosmetic falling-debris actor spawned for each detached island
#include "VoxelEditRelay.h"
// W2 dig-breach hook (docs/voxel-earth-implementation-plan.md SS3.7 item 2):
// TryDig/CarveSphere notify the water subsystem of newly-cleared voxels so
// it can seed Reservoir v0 boundary cells. Same-module UE include, not a
// voxel-core boundary concern -- VoxelWaterSubsystem.h is itself
// voxel-core-free (PImpl), exactly like this header.
#include "VoxelWaterSubsystem.h"

// voxel-core is UE-header-free C++20; safe to include directly from a UE
// module .cpp (doctrine: never from a header UHT parses -- see
// VoxelWorldSubsystem.h / VoxelChunkComponent.h, both voxel-core-free).
#include "voxelcore/bytes.h"
#include "voxelcore/collapse.h"     // M5 large-edit structural collapse: differential coarse support
#include "voxelcore/connectivity.h" // M5 destruction: findDisconnectedIslands over the edited region
#include "voxelcore/counters.h"
#include "voxelcore/editcompact.h" // M3 wave 2: compactLog for save-to-disk + join-sync compaction
#include "voxelcore/hash.h"
#include "voxelcore/mesher.h"
#include "voxelcore/mips.h"
#include "voxelcore/raycast.h"
#include "voxelcore/tiles.h"
// Track B2: vxc::TileGridSampler (parses real .vxtl terrain-service tiles)
// -- only referenced from this .cpp's file-local MakeTileSampler factory and
// FVoxelWorldImpl's Tiles/GridTiles members, never from the UHT-parsed
// header (same PImpl doctrine as every other voxel-core include here).
#include "voxelcore/tilestore.h"
#include "voxelcore/world.h"

#include "Components/SceneComponent.h"
#include "Containers/Queue.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h" // TActorIterator (M3: locating the world's single AVoxelEditRelay)
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h" // M3 wave 2: atomic save-file rename (IFileManager::Move)
#include "HAL/IConsoleManager.h" // M3: voxel.DumpEditedDigest / voxel.SaveWorld
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeCounter.h"
#include "LocalVertexFactory.h" // hitch isolation: FLocalVertexFactory::StaticType for the BeginPlay PSO precache warmup
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h" // M3 wave 2: Saved/VoxelWorlds/<seed>.vxlog read/write
#include "Misc/Parse.h"
#include "Misc/Paths.h" // M3 wave 2: FPaths::ProjectSavedDir
#include "RenderTimer.h" // M1 gate: per-thread frame timers (GRenderThreadTime/GRHIThreadTime/GGameThreadWaitTime) for hitch attribution
#include "Misc/ScopeRWLock.h"
#include "Materials/MaterialInterface.h"
#include "PSOPrecache.h" // hitch isolation: FPSOPrecacheParams for the BeginPlay terrain-material PSO precache warmup
#include "Stats/Stats.h"
#include "Tasks/Task.h"
#include "UObject/UObjectGlobals.h"

#include <algorithm>
#include <atomic>
#include <filesystem> // Track B2: directory_iterator over -VoxelTileDir
#include <memory>     // Track B2: std::unique_ptr<vxc::ITileSampler>
#include <unordered_map>
#include <vector>

// VoxelCoords.h intentionally duplicates this constant (in UU) so it stays
// voxel-core-free; check the two never drift apart.
static_assert(vxc::kVoxelSizeMm == int32(VoxelCoords::VoxelSizeUU) * 10,
              "VoxelCoords::VoxelSizeUU (UE units) must track vxc::kVoxelSizeMm (mm)");

// M5 destruction (first slice, docs/voxel-earth-implementation-plan.md SS3.5):
// gates the connectivity-flood-fill "did this edit detach a floating island?"
// pass that runs after every solid-removing dig/carve on the authority. On by
// default -- with no trees/structures in the world yet it is a near-no-op
// (normal terrain digs never detach an island; see PromoteDetachedIslands), so
// standalone terrain/edit RESULTS are byte-identical whether it is on or off;
// this switch exists to disable the extra per-edit scan cost if ever needed.
static TAutoConsoleVariable<bool> CVarVoxelDestructionEnabled(
	TEXT("voxel.Destruction.Enabled"), true,
	TEXT("M5: run connectivity island-detection after solid-removing edits and promote detached islands to falling debris."),
	ECVF_Default);

// M5 large-edit structural collapse, mirroring voxel.Destruction.Enabled one
// level down: this gates ONLY the brick-resolution differential-support pass
// that runs when the voxel-resolution island region clamps (large/explosive
// edits). Default on. Turning it off restores the previous v0 behavior exactly
// -- large edits detach nothing -- without touching ordinary digs, which never
// reach this path at all. voxel.Destruction.Enabled=0 disables both.
static TAutoConsoleVariable<bool> CVarVoxelCollapseEnabled(
	TEXT("voxel.Destruction.Collapse"), true,
	TEXT("M5: run brick-resolution differential-support structural collapse for edits too large for voxel-resolution island detection."),
	ECVF_Default);

namespace
{
// Meshes one render chunk's 4x4x4 bricks via MaterialAt and bakes each
// brick's quad coordinates into chunk-local ones. Shared, unchanged, by both
// halves of the lock-free split (docs/m1-plan.md Stage 2 decisions table,
// "Worker threading" row): the worker-job path calls this with a sampler
// closed over GeneratedWorld only (pure function of seed); the game-thread
// edited-chunk path calls it with a sampler closed over World::materialAt
// (overlay-aware). Only the sampler differs -- the mesh-and-bake logic never
// drifts between the two.
template <typename MaterialFn>
void MeshChunkBricks(const VoxelCoords::FVoxelChunkKey& ChunkKey, const MaterialFn& MaterialAt, TArray<FVoxelChunkQuad>& OutQuads,
                      vxc::Counters* PerfCounters = nullptr)
{
	using namespace vxc;
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;
	constexpr int32 BricksPerChunk = VoxelCoords::ChunkEdgeBricks;

	std::vector<Quad> BrickQuads;
	for (int32 Dz = 0; Dz < BricksPerChunk; ++Dz)
	{
		for (int32 Dy = 0; Dy < BricksPerChunk; ++Dy)
		{
			for (int32 Dx = 0; Dx < BricksPerChunk; ++Dx)
			{
				const int64 OriginVX = (int64(ChunkKey.X) * BricksPerChunk + Dx) * int64(B);
				const int64 OriginVY = (int64(ChunkKey.Y) * BricksPerChunk + Dy) * int64(B);
				const int64 OriginVZ = (int64(ChunkKey.Z) * BricksPerChunk + Dz) * int64(B);

				// Sampler valid on [-1,B]^3 (mesher.h contract): MaterialAt
				// reads straight across brick AND render-chunk borders via
				// the same deterministic function, so no neighbor data needs
				// to be materialized just to mesh this one brick.
				const auto Sampler = [&](int X, int Y, int Z) -> MaterialId
				{ return MaterialAt(OriginVX + X, OriginVY + Y, OriginVZ + Z); };

				BrickQuads.clear();
				meshBrick<B>(Sampler, BrickQuads);

				// docs/debug-tooling-plan.md P1 "vxc::Counters": counted here
				// (around the UE layer's call into meshBrick), not inside
				// voxel-core itself -- bricksGenerated/cellsWritten count
				// every brick attempted (whether or not it emits quads: an
				// interior-solid brick still gets sampled/meshed), quads
				// counted separately below only when non-empty.
				if (PerfCounters)
				{
					PerfCounters->incBricksGenerated();
					PerfCounters->incCellsWritten(uint64_t(B) * B * B);
				}

				if (BrickQuads.empty())
				{
					continue;
				}

				if (PerfCounters)
				{
					PerfCounters->incQuadsEmitted(uint64_t(BrickQuads.size()));
				}

				// Bake this brick's position within the chunk into the
				// (already chunk-scale, uint8-safe: max 31) quad fields so
				// the scene proxy never needs to know about bricks.
				const int32 AxisOffset[3] = {Dx * B, Dy * B, Dz * B};
				OutQuads.Reserve(OutQuads.Num() + (int32)BrickQuads.size());
				for (const Quad& Q : BrickQuads)
				{
					const int32 U = (Q.axis + 1) % 3;
					const int32 V = (Q.axis + 2) % 3;
					FVoxelChunkQuad CQ;
					CQ.Axis = Q.axis;
					CQ.Positive = Q.positive;
					CQ.Slice = (uint8)(Q.slice + AxisOffset[Q.axis]);
					CQ.U0 = (uint8)(Q.u0 + AxisOffset[U]);
					CQ.V0 = (uint8)(Q.v0 + AxisOffset[V]);
					CQ.W = Q.w;
					CQ.H = Q.h;
					CQ.Ao = Q.ao;
					CQ.Mat = Q.mat;
					OutQuads.Add(CQ);
				}
			}
		}
	}
}

// M2 mip sourcing (docs/m2-plan.md "Mip sourcing" row): a level-L (L>=1)
// chunk job builds its bricks via FCachedMipBuilder (below -- a
// vxc::MipChain<8>-equivalent recursion over vxc::downsampleBricks, wired to
// the cross-job FSharedMipCache as of M2 wave 2 item 1) over a PURE-generated
// level-0 source -- the lock-free rule holds unchanged (workers never touch
// World's overlay, only GeneratedWorld). A naive MipChain source that calls
// GeneratedWorld::makeBrick(key) fresh on every request would recompute the
// (8x8) amplifier column grid for every level-0 brick queried; within one
// job, a tall stack of level-0 bricks at the same (bx,by) XY footprint
// shares one column grid, so this small per-job LRU (capped, not one grid
// covering the whole job footprint -- that would front-load amplifier work
// for chunk regions the recursion may not even need) caches ColumnGrids by
// (bx,by) and is the thing that actually avoids the redundant recompute.
class FJobColumnGridCache
{
public:
	using GeneratedWorldT = vxc::GeneratedWorld<VoxelCoords::BrickEdgeVoxels>;

	FJobColumnGridCache(const GeneratedWorldT& InGen, vxc::Counters* InPerfCounters) : Gen(InGen), PerfCounters(InPerfCounters) {}

	const GeneratedWorldT::ColumnGrid& Get(int32 Bx, int32 By)
	{
		const uint64 PackedKey = (uint64(uint32(Bx)) << 32) | uint64(uint32(By));
		if (GeneratedWorldT::ColumnGrid* Found = Grids.Find(PackedKey))
		{
			Touch(PackedKey);
			return *Found;
		}
		if (Grids.Num() >= kMaxEntries)
		{
			Evict();
		}
		// docs/debug-tooling-plan.md P1 "vxc::Counters": one increment per
		// actual cache miss (LRU hits, the whole point of this cache, don't
		// recount) -- B*B amplifier column evaluations per miss, same unit as
		// the level-0 job's columnEvals counter.
		if (PerfCounters)
		{
			PerfCounters->incColumnEvals(uint64_t(VoxelCoords::BrickEdgeVoxels) * VoxelCoords::BrickEdgeVoxels);
		}
		GeneratedWorldT::ColumnGrid& NewGrid = Grids.Add(PackedKey, Gen.columns(Bx, By));
		RecencyOrder.Add(PackedKey);
		return NewGrid;
	}

private:
	void Touch(uint64 Key)
	{
		RecencyOrder.RemoveSingle(Key);
		RecencyOrder.Add(Key);
	}
	void Evict()
	{
		if (RecencyOrder.Num() == 0)
		{
			return;
		}
		const uint64 Oldest = RecencyOrder[0];
		RecencyOrder.RemoveAt(0);
		Grids.Remove(Oldest);
	}

	static constexpr int32 kMaxEntries = 64;
	const GeneratedWorldT& Gen;
	vxc::Counters* PerfCounters;
	TMap<uint64, GeneratedWorldT::ColumnGrid> Grids;
	TArray<uint64> RecencyOrder;
};

// M2 wave 2 item 1 ("Cross-job mip caching", docs/m2-plan.md): a shared,
// thread-safe, read-mostly cache of PURE-GENERATED level>=1 mip bricks
// (vxc::downsampleBricks output), keyed by (level, BrickKey) via
// vxc::MipKey/vxc::MipKeyHash (voxelcore/mips.h, read-only this wave -- its
// public key/hash types are reused as-is rather than duplicated). Wave 1
// rebuilt every level from scratch inside a fresh PER-JOB vxc::MipChain<8>
// (measured worker p95 ~296ms on high-level jobs); the same (level,key) mip
// brick is frequently rebuilt by MULTIPLE jobs (adjacent chunks at the same
// level share intermediate ancestors, and a job building one level-4 chunk
// recomputes every level-1..3 ancestor a sibling job needs too) -- this
// cache amortizes that across jobs AND across levels within one job.
//
// Safe under the lock-free doctrine's "workers never touch the overlay"
// rule: every entry here is a DETERMINISTIC function of (seed, level, key)
// computed via GeneratedWorld only -- see FCachedMipBuilder below, whose two
// call sites are MakeLevelSampler (worker path, SharedCache=this) and
// MakeOverlayAwareLevelSampler (game-thread edited-mip path,
// SharedCache=nullptr BY CONSTRUCTION so an edited region's mip bricks can
// NEVER be written here). Edits explicitly invalidate (remove) affected
// entries instead (FVoxelWorldImpl::PropagateEditToMips) rather than this
// cache ever being allowed to hold an overlay-tainted value.
//
// Sharded (FRWLock-guarded std::unordered_map per shard) so concurrent
// worker jobs don't serialize on one global lock. Find() returns a COPY
// under the read lock rather than a pointer/reference: the target shard's
// map can rehash on a concurrent writer, which would invalidate anything
// crossing the lock boundary by reference. Brick<8> copies are cheap in the
// overwhelmingly common homogeneous case (no heap allocation -- see
// Brick<B>::isHomogeneous); non-homogeneous bricks pay a small heap copy,
// acceptable given this replaces a full recursive re-downsample on a hit.
//
// M2 task "Mip cache eviction": bounded by voxel.MipCacheBudgetMB (bytes via
// VoxelDebug::GetMipCacheBudgetBytes(), default 512MB) via a per-shard
// GENERATION-STAMPED APPROXIMATE LRU -- each entry carries a relaxed atomic
// "last touched" stamp (FEntry::LastTouch), bumped on every Find hit and
// fresh Insert from one cache-wide monotonic counter (Generation). Eviction
// (EvictIfOverBudgetLocked, called at the end of Insert while that shard's
// write lock is already held -- no extra locking) samples a small, FIXED
// number of entries from the shard's map (kEvictSampleSize) and evicts
// whichever sampled entry has the lowest stamp, looping until back under
// budget or the shard runs dry. This is deliberately approximate rather than
// a true global LRU (an intrusive doubly-linked list, sharded or not, would
// be exact but is more machinery than this cache's access pattern needs --
// see the task's own framing, "keep it simple"): the O(kEvictSampleSize)
// sample cost per eviction step is independent of shard size, so a single
// over-budget Insert never pays an O(shard size) scan even at the scale
// docs/status.md's wave-2 measurement recorded (4.9M bricks / ~716MB with no
// eviction at all, pre this task). Racing the LastTouch stamp under a shared
// READ lock (multiple Find calls on the same entry) is safe in the C++
// memory-model sense (std::atomic tolerates unsynchronized concurrent
// writers by design) -- worst case is a lost/overwritten touch, which only
// ever biases WHICH near-tied entry gets evicted next, never correctness.
class FSharedMipCache
{
public:
	using BrickT = vxc::Brick<VoxelCoords::BrickEdgeVoxels>;

	bool Find(int32 Level, const vxc::BrickKey& Key, BrickT& OutBrick) const
	{
		const vxc::MipKey MK{Level, Key};
		const FShard& Shard = ShardFor(MK);
		FRWScopeLock Lock(Shard.Lock, SLT_ReadOnly);
		const auto Found = Shard.Map.find(MK);
		if (Found == Shard.Map.end())
		{
			return false;
		}
		OutBrick = Found->second.Brick;
		Found->second.LastTouch.store(NextGeneration(), std::memory_order_relaxed);
		return true;
	}

	void Insert(int32 Level, const vxc::BrickKey& Key, const BrickT& Value)
	{
		const vxc::MipKey MK{Level, Key};
		FShard& Shard = ShardFor(MK);
		const uint64 Gen = NextGeneration();
		FRWScopeLock Lock(Shard.Lock, SLT_Write);
		auto [It, bInserted] = Shard.Map.try_emplace(MK, Value, Gen);
		if (bInserted)
		{
			BrickCount.fetch_add(1, std::memory_order_relaxed);
			BytesUsed.fetch_add(EstimateBytes(Value), std::memory_order_relaxed);
		}
		else
		{
			// Two jobs racing a miss on the same (level,key) both build and
			// insert -- harmless (same deterministic inputs => byte-identical
			// output), just an overwrite rather than a true race on content.
			const int64 OldBytes = EstimateBytes(It->second.Brick);
			It->second.Brick = Value;
			It->second.LastTouch.store(Gen, std::memory_order_relaxed);
			BytesUsed.fetch_add(EstimateBytes(Value) - OldBytes, std::memory_order_relaxed);
		}
		EvictIfOverBudgetLocked(Shard);
	}

	// M2 wave 2 item 2 ("Distant-edit mip propagation"): removes a
	// (level,key) entry if present -- called when an edit lands under this
	// mip chunk's footprint, since the pure-generated value cached here is
	// now stale for rendering (the chunk must take the overlay-aware
	// game-thread path from now on, exactly like a level-0 edited chunk;
	// see FVoxelWorldImpl::PropagateEditToMips).
	void Invalidate(int32 Level, const vxc::BrickKey& Key)
	{
		const vxc::MipKey MK{Level, Key};
		FShard& Shard = ShardFor(MK);
		FRWScopeLock Lock(Shard.Lock, SLT_Write);
		const auto Found = Shard.Map.find(MK);
		if (Found != Shard.Map.end())
		{
			BytesUsed.fetch_sub(EstimateBytes(Found->second.Brick), std::memory_order_relaxed);
			Shard.Map.erase(Found);
			BrickCount.fetch_sub(1, std::memory_order_relaxed);
		}
	}

	int64 GetBrickCount() const { return BrickCount.load(std::memory_order_relaxed); }
	int64 GetBytesUsed() const { return BytesUsed.load(std::memory_order_relaxed); }
	int64 GetEvictionsCount() const { return Evictions.load(std::memory_order_relaxed); }

private:
	static constexpr int32 kNumShards = 16;
	// Bounded sample size for approximate-LRU eviction (class comment) --
	// small enough that even a large batch of evictions (e.g. a runtime
	// voxel.MipCacheBudgetMB drop) stays cheap per step.
	static constexpr int32 kEvictSampleSize = 8;

	struct FEntry
	{
		BrickT Brick;
		// mutable: Find() is a const method but still needs to bump the
		// touch stamp on a hit (see class comment) -- same "logically const,
		// bookkeeping mutates" doctrine as FRWLock members being mutable.
		mutable std::atomic<uint64> LastTouch;

		FEntry(const BrickT& InBrick, uint64 InTouch) : Brick(InBrick), LastTouch(InTouch) {}
	};

	struct FShard
	{
		mutable FRWLock Lock;
		std::unordered_map<vxc::MipKey, FEntry, vxc::MipKeyHash> Map;
	};

	FShard& ShardFor(const vxc::MipKey& Key) { return Shards[vxc::MipKeyHash{}(Key) % uint64(kNumShards)]; }
	const FShard& ShardFor(const vxc::MipKey& Key) const { return Shards[vxc::MipKeyHash{}(Key) % uint64(kNumShards)]; }

	// const: called from the const Find() method too (touch-on-hit) -- Generation
	// is `mutable` for the same "logically const, bookkeeping mutates" reason
	// FEntry::LastTouch is.
	uint64 NextGeneration() const { return Generation.fetch_add(1, std::memory_order_relaxed); }

	// Called from Insert with Shard's write lock already held. Loops
	// evicting one sampled-oldest entry at a time until BytesUsed is back
	// under voxel.MipCacheBudgetMB or this shard is empty -- see class
	// comment for why sampling (not a full scan) is used.
	void EvictIfOverBudgetLocked(FShard& Shard)
	{
		const int64 Budget = VoxelDebug::GetMipCacheBudgetBytes();
		if (Budget <= 0)
		{
			return; // <=0 means "unbounded" (defensive; eviction disabled)
		}
		while (BytesUsed.load(std::memory_order_relaxed) > Budget && !Shard.Map.empty())
		{
			auto OldestIt = Shard.Map.end();
			uint64 OldestTouch = 0;
			int32 Sampled = 0;
			for (auto It = Shard.Map.begin(); It != Shard.Map.end() && Sampled < kEvictSampleSize; ++It, ++Sampled)
			{
				const uint64 Touch = It->second.LastTouch.load(std::memory_order_relaxed);
				if (OldestIt == Shard.Map.end() || Touch < OldestTouch)
				{
					OldestIt = It;
					OldestTouch = Touch;
				}
			}
			if (OldestIt == Shard.Map.end())
			{
				break; // defensive; unreachable given the while condition's !empty() check
			}
			BytesUsed.fetch_sub(EstimateBytes(OldestIt->second.Brick), std::memory_order_relaxed);
			Shard.Map.erase(OldestIt);
			BrickCount.fetch_sub(1, std::memory_order_relaxed);
			Evictions.fetch_add(1, std::memory_order_relaxed);
		}
	}

	static int64 EstimateBytes(const BrickT& Value)
	{
		// Approximate (public-API-only, since Brick<B>'s storage is private):
		// base struct size, plus one byte per dense cell + one byte per
		// palette entry for the non-homogeneous case (mirrors Brick<B>'s
		// actual cells_/palette_ layout closely enough for an HUD memory row).
		constexpr int64 Base = sizeof(BrickT);
		if (Value.isHomogeneous())
		{
			return Base;
		}
		return Base + BrickT::kCells + int64(Value.paletteSize());
	}

	FShard Shards[kNumShards];
	std::atomic<int64> BrickCount{0};
	std::atomic<int64> BytesUsed{0};
	std::atomic<int64> Evictions{0};
	mutable std::atomic<uint64> Generation{1};
};

// M2 wave 2 item 1: recursive level>=1 mip builder shared by BOTH the
// pure-generated worker path and the overlay-aware game-thread path (only
// the Level0Source callback and whether a FSharedMipCache is wired in
// differ). Reimplements vxc::MipChain<B>'s recursion (voxelcore/mips.h,
// read-only this wave: its brick()/cache_ are usable but cache_ is private
// with no seeding hook, so a per-job-only cache can't be shared across jobs
// without this wrapper) using mips.h's PUBLIC downsampleBricks<B> directly,
// so every level of the recursion -- not just the job's own target level --
// consults/populates FSharedMipCache. A job-local map still backs every
// pointer this returns (both cache hits and freshly-built bricks are copied
// in here) so pointers stay valid for the sampler's whole lifetime, exactly
// like vxc::MipChain<B>::cache_ did per-job in wave 1.
class FCachedMipBuilder
{
public:
	using BrickT = vxc::Brick<VoxelCoords::BrickEdgeVoxels>;
	using Level0SourceFn = std::function<const BrickT*(const vxc::BrickKey&)>;

	// SharedCache == nullptr disables cross-job caching entirely (both read
	// and write) -- this is how the overlay-aware game-thread path
	// (MakeOverlayAwareLevelSampler) structurally guarantees it can never
	// pollute the pure shared cache with an edited value, rather than relying
	// on a caller convention.
	//
	// GlobalEditEpoch/EpochSnapshot close a narrow cross-thread race: a
	// worker job dispatched BEFORE an edit lands may still be mid-recursion
	// when PropagateEditToMips (game thread) invalidates the very
	// (level,key) entries this job is about to Insert -- without a check,
	// the job's Insert would land AFTER the invalidate and silently
	// resurrect a now-stale pure value. GlobalEditEpoch is
	// FVoxelWorldImpl::EditEpoch (bumped once per edit batch, BEFORE
	// invalidating, in PropagateEditToMips); EpochSnapshot is its value at
	// job-dispatch time (DispatchJobs, same "snapshot at dispatch" idiom
	// FChunkRecord::GenerationId already uses for stale-worker-RESULT
	// discarding). If the epoch has moved by the time this job is ready to
	// Insert, some edit landed mid-flight -- INSTEAD OF checking machinery
	// to know whether it actually overlapped this specific brick, the whole
	// Insert is conservatively skipped (a dropped cache-population
	// opportunity, not a correctness issue: recomputed fresh next time).
	// nullptr GlobalEditEpoch disables the check (used when SharedCache is
	// also nullptr -- the overlay path never inserts here regardless).
	FCachedMipBuilder(Level0SourceFn InLevel0Source, FSharedMipCache* InSharedCache, const std::atomic<uint64>* InGlobalEditEpoch,
	                   uint64 InEpochSnapshot, int32 InThreshold = 4)
		: Level0Source(std::move(InLevel0Source))
		, SharedCache(InSharedCache)
		, GlobalEditEpoch(InGlobalEditEpoch)
		, EpochSnapshot(InEpochSnapshot)
		, Threshold(InThreshold)
	{
	}

	const BrickT* Brick(int32 Level, const vxc::BrickKey& Key)
	{
		if (Level <= 0)
		{
			return Level0Source(Key);
		}

		const vxc::MipKey MK{Level, Key};
		const auto LocalFound = LocalCache.find(MK);
		if (LocalFound != LocalCache.end())
		{
			return &LocalFound->second;
		}

		if (SharedCache)
		{
			BrickT Cached;
			if (SharedCache->Find(Level, Key, Cached))
			{
				auto [It, Inserted] = LocalCache.emplace(MK, MoveTemp(Cached));
				(void)Inserted;
				return &It->second;
			}
		}

		const BrickT* Children[8] = {};
		bool bAnyChild = false;
		for (int32 Cz = 0; Cz < 2; ++Cz)
		{
			for (int32 Cy = 0; Cy < 2; ++Cy)
			{
				for (int32 Cx = 0; Cx < 2; ++Cx)
				{
					const vxc::BrickKey ChildKey{Key.x * 2 + Cx, Key.y * 2 + Cy, Key.z * 2 + Cz};
					const BrickT* Child = Brick(Level - 1, ChildKey);
					if (Child)
					{
						bAnyChild = true;
					}
					Children[Cx + 2 * Cy + 4 * Cz] = Child;
				}
			}
		}
		if (!bAnyChild)
		{
			return nullptr; // whole 2x2x2 group is air; matches MipChain's convention (don't materialize).
		}

		BrickT Built = vxc::downsampleBricks<VoxelCoords::BrickEdgeVoxels>(Children, Threshold);
		if (SharedCache && (!GlobalEditEpoch || GlobalEditEpoch->load() == EpochSnapshot))
		{
			SharedCache->Insert(Level, Key, Built);
		}
		auto [It, Inserted] = LocalCache.emplace(MK, MoveTemp(Built));
		(void)Inserted;
		return &It->second;
	}

private:
	Level0SourceFn Level0Source;
	FSharedMipCache* SharedCache;
	const std::atomic<uint64>* GlobalEditEpoch;
	uint64 EpochSnapshot;
	int32 Threshold;
	std::unordered_map<vxc::MipKey, BrickT, vxc::MipKeyHash> LocalCache;
};

// Builds a per-job level-L sampler (level-L absolute voxel coords ->
// MaterialId, valid on [-1,B]^3 across brick AND chunk borders, matching
// meshBrick's contract) whose level-0 source lazily materializes bricks from
// GeneratedWorld via the column-grid cache above, and whose level>=1 bricks
// go through FCachedMipBuilder wired to SharedCache (M2 wave 2 item 1: the
// cross-job cache). The returned std::function keeps everything alive via a
// shared_ptr capture, so the caller can pass the sampler straight into
// MeshChunkBricks without worrying about lifetime -- freed once the sampler
// itself goes out of scope at the end of the worker job. Worker path only
// (pure GeneratedWorld source -- never touches World's overlay, per the
// lock-free doctrine); see MakeOverlayAwareLevelSampler for the game-thread
// edited-mip counterpart.
std::function<vxc::MaterialId(int64, int64, int64)> MakeLevelSampler(const vxc::GeneratedWorld<VoxelCoords::BrickEdgeVoxels>& Gen,
                                                                       int32 Level, vxc::Counters* PerfCounters,
                                                                       FSharedMipCache* SharedCache,
                                                                       const std::atomic<uint64>* GlobalEditEpoch, uint64 EpochSnapshot)
{
	using namespace vxc;
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;

	struct FJobMipState
	{
		FJobMipState(const GeneratedWorld<B>& InGen, Counters* InPerfCounters, FSharedMipCache* InSharedCache,
		             const std::atomic<uint64>* InGlobalEditEpoch, uint64 InEpochSnapshot)
			: ColumnCache(InGen, InPerfCounters)
			, Gen(InGen)
			, Builder(
				  [this](const BrickKey& Key) -> const Brick<B>*
				  {
					  auto Found = Level0Bricks.find(Key);
					  if (Found != Level0Bricks.end())
					  {
						  return &Found->second;
					  }
					  const auto& Grid = ColumnCache.Get(Key.x, Key.y);
					  auto [It, Inserted] = Level0Bricks.emplace(Key, Gen.makeBrick(Key, Grid));
					  (void)Inserted;
					  return &It->second;
				  },
				  InSharedCache, InGlobalEditEpoch, InEpochSnapshot)
		{
		}

		FJobColumnGridCache ColumnCache;
		const GeneratedWorld<B>& Gen;
		std::unordered_map<BrickKey, Brick<B>, BrickKeyHash> Level0Bricks;
		FCachedMipBuilder Builder;
	};

	TSharedPtr<FJobMipState> State = MakeShared<FJobMipState>(Gen, PerfCounters, SharedCache, GlobalEditEpoch, EpochSnapshot);

	return [State, Level](int64 X, int64 Y, int64 Z) -> MaterialId
	{
		const BrickKey BKey{int32_t(floorDiv(X, B)), int32_t(floorDiv(Y, B)), int32_t(floorDiv(Z, B))};
		const Brick<B>* B0 = State->Builder.Brick(Level, BKey);
		if (!B0)
		{
			return MAT_AIR;
		}
		const int Lx = int(floorMod(X, B));
		const int Ly = int(floorMod(Y, B));
		const int Lz = int(floorMod(Z, B));
		return B0->get(Lx, Ly, Lz);
	};
}

// M2 wave 2 item 2 ("Distant-edit mip propagation", docs/m2-plan.md): the
// game-thread, overlay-aware counterpart to MakeLevelSampler -- sources
// level-0 bricks from World::brickAt (edited version if present, else
// generated; voxelcore/world.h) instead of a pure GeneratedWorld, so a mip
// chunk built through this sampler reflects dig/place/carve edits underneath
// it. Game thread only (World's overlay is not thread-safe, same constraint
// as Voxels.materialAt elsewhere in this file). Deliberately constructs its
// FCachedMipBuilder with SharedCache=nullptr: this is what makes it
// structurally impossible for an edited mip brick to leak into the pure
// cross-job FSharedMipCache (see that class's doc comment) -- caching here
// is job-local (this single re-mesh call) only, same cost profile a per-job
// vxc::MipChain<8> had in wave 1.
std::function<vxc::MaterialId(int64, int64, int64)> MakeOverlayAwareLevelSampler(const vxc::World<VoxelCoords::BrickEdgeVoxels>& Voxels,
                                                                                   int32 Level)
{
	using namespace vxc;
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;

	struct FOverlayMipState
	{
		FOverlayMipState(const World<B>& InVoxels)
			: Voxels(InVoxels)
			, Builder(
				  [this](const BrickKey& Key) -> const Brick<B>*
				  {
					  auto Found = Level0Bricks.find(Key);
					  if (Found != Level0Bricks.end())
					  {
						  return &Found->second;
					  }
					  auto [It, Inserted] = Level0Bricks.emplace(Key, Voxels.brickAt(Key));
					  (void)Inserted;
					  return &It->second;
				  },
				  /*SharedCache*/ nullptr, /*GlobalEditEpoch*/ nullptr, /*EpochSnapshot*/ 0)
		{
		}

		const World<B>& Voxels;
		std::unordered_map<BrickKey, Brick<B>, BrickKeyHash> Level0Bricks;
		FCachedMipBuilder Builder;
	};

	TSharedPtr<FOverlayMipState> State = MakeShared<FOverlayMipState>(Voxels);

	return [State, Level](int64 X, int64 Y, int64 Z) -> MaterialId
	{
		const BrickKey BKey{int32_t(floorDiv(X, B)), int32_t(floorDiv(Y, B)), int32_t(floorDiv(Z, B))};
		const Brick<B>* B0 = State->Builder.Brick(Level, BKey);
		if (!B0)
		{
			return MAT_AIR;
		}
		const int Lx = int(floorMod(X, B));
		const int Ly = int(floorMod(Y, B));
		const int Lz = int(floorMod(Z, B));
		return B0->get(Lx, Ly, Lz);
	};
}

// Track B2 ("real .vxtl terrain tiles as a selectable tile source"): builds
// the ITileSampler FVoxelWorldImpl::Tiles owns for this run.
//
// TileDir empty/absent (the default -- no -VoxelTileDir on the command line)
// => a bare vxc::SyntheticTileSampler(Seed), zero filesystem access,
// UNCONDITIONALLY: this is what keeps default behavior byte-identical to
// pre-Track-B2 (the cross-vendor determinism goldens depend on exactly this
// path running with no new switches present).
//
// Non-empty TileDir => construct a vxc::TileGridSampler(Seed, TileScale) and
// load every *.vxtl file directly inside TileDir (non-recursive: the
// terrain-service cache leaf directory this switch names, e.g.
// tile-cache/<provider_id>/<seed hex>/s1, is already flat). Parsed manually
// here (vxc::readFileBytes + vxc::TileData::parse) rather than via
// TileGridSampler::loadTileFile so the tile's own (x, y) is available for the
// bounding-box log below -- loadTile(TileData) still enforces the exact same
// seed/scale rejection loadTileFile would (doctrine-required: a tile
// generated for a different seed/scale must never silently leak into this
// run's terrain).
//
// Missing-tile policy (doctrine, see voxelcore/tilestore.h's own comment):
// once a grid IS in use, any individual query landing outside every loaded
// tile's footprint returns TileGridSampler's built-in deterministic flat-sea-
// level default (elevation 0, default climate) rather than fabricating data
// -- intended and fine (see FVoxelWorldImpl::MaybeLogCounters' missing-tile
// telemetry). But if the directory as a WHOLE produces zero loaded tiles (bad
// path, empty directory, or -VoxelTileDir paired with the wrong -VoxelSeed so
// every file's seed check fails), that is very likely operator error rather
// than an intentionally sparse tile set -- silently booting an entirely-empty
// flat world on a bad path would be confusing and hard to diagnose, so this
// case is rejected loudly (UE_LOG Error) and falls back to the synthetic
// sampler instead, exactly like the invalid-TileScale case below.
std::unique_ptr<vxc::ITileSampler> MakeTileSampler(uint64 Seed, const FString& TileDir, int32 TileScale, bool& bOutUsingTileGrid)
{
	bOutUsingTileGrid = false;

	if (TileDir.IsEmpty())
	{
		return std::make_unique<vxc::SyntheticTileSampler>(Seed);
	}

	if (vxc::tilePixelSizeMm((uint8)TileScale) == 0)
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("-VoxelTileScale=%d is not a supported tile scale (only 1 [30m/px] or 8 [11.25m/px] are valid) -- ")
		       TEXT("falling back to the synthetic sampler."),
		       TileScale);
		return std::make_unique<vxc::SyntheticTileSampler>(Seed);
	}

	// TCHAR is wchar_t on Windows (this module's only supported platform per
	// its build docs), which is std::filesystem::path's native windows encoding
	// -- constructing directly from *TileDir needs no UTF8 round-trip.
	const std::filesystem::path DirPath(*TileDir);
	std::error_code DirEc;
	if (!std::filesystem::is_directory(DirPath, DirEc) || DirEc)
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("-VoxelTileDir=%s does not exist or is not a directory -- falling back to the synthetic sampler."), *TileDir);
		return std::make_unique<vxc::SyntheticTileSampler>(Seed);
	}

	auto Grid = std::make_unique<vxc::TileGridSampler>(Seed, (uint8)TileScale);

	int32 Loaded = 0;
	int32 Rejected = 0;
	bool bAnyBox = false;
	int32 MinTx = 0, MaxTx = 0, MinTy = 0, MaxTy = 0;

	std::error_code IterEc;
	for (auto It = std::filesystem::directory_iterator(DirPath, IterEc);
	     !IterEc && It != std::filesystem::directory_iterator(); It.increment(IterEc))
	{
		const std::filesystem::directory_entry& Entry = *It;
		std::error_code FileEc;
		if (!Entry.is_regular_file(FileEc) || FileEc)
		{
			continue;
		}
		if (Entry.path().extension() != ".vxtl")
		{
			continue;
		}

		std::optional<std::vector<uint8_t>> Bytes = vxc::readFileBytes(Entry.path());
		if (!Bytes)
		{
			++Rejected;
			continue;
		}
		std::optional<vxc::TileData> Parsed = vxc::TileData::parse(Bytes->data(), Bytes->size());
		if (!Parsed)
		{
			++Rejected; // malformed / truncated / wrong-version .vxtl
			continue;
		}
		const int32 Tx = Parsed->x;
		const int32 Ty = Parsed->y;
		if (!Grid->loadTile(std::move(*Parsed)))
		{
			++Rejected; // parsed fine but this file's seed/scale != (Seed, TileScale)
			continue;
		}

		++Loaded;
		if (!bAnyBox)
		{
			MinTx = MaxTx = Tx;
			MinTy = MaxTy = Ty;
			bAnyBox = true;
		}
		else
		{
			MinTx = FMath::Min(MinTx, Tx);
			MaxTx = FMath::Max(MaxTx, Tx);
			MinTy = FMath::Min(MinTy, Ty);
			MaxTy = FMath::Max(MaxTy, Ty);
		}
	}

	UE_LOG(LogVoxelEarth, Log, TEXT("Voxel tile grid: dir=%s loaded=%d rejected=%d seed=%llu scale=%d"), *TileDir, Loaded, Rejected,
	       (unsigned long long)Seed, TileScale);

	if (Loaded == 0)
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("-VoxelTileDir=%s produced zero loaded tiles (bad path, empty directory, or every tile's seed/scale ")
		       TEXT("mismatched -VoxelSeed=%llu / -VoxelTileScale=%d) -- falling back to the synthetic sampler rather than ")
		       TEXT("silently booting an empty flat world."),
		       *TileDir, (unsigned long long)Seed, TileScale);
		return std::make_unique<vxc::SyntheticTileSampler>(Seed);
	}

	UE_LOG(LogVoxelEarth, Log, TEXT("Voxel tile grid: loaded tile coords bounding box x=[%d,%d] y=[%d,%d]"), MinTx, MaxTx, MinTy, MaxTy);

	bOutUsingTileGrid = true;
	return Grid;
}

// Sized dig/place cube anchoring (m1-plan.md "Player experience decisions",
// "Dig sizes" / "Place" rows): the SizeVoxels^3 cube is centred on Anchor on
// the two axes tangent to the hit face (FaceAxis), and biased along the face
// axis by GrowAxisSign so the whole cube sits on one side of Anchor instead
// of straddling it -- dig grows INTO the terrain (GrowAxisSign ==
// hit.faceSign), place grows AWAY from it (GrowAxisSign == -hit.faceSign).
// FaceAxis == -1 (ray started inside solid; dig-only, TryPlace already
// rejects this case) falls back to centring on all three axes since there is
// no face to bias against.
void ComputeCubeMinCorner(int64 AnchorX, int64 AnchorY, int64 AnchorZ, int32 FaceAxis, int32 GrowAxisSign, int32 SizeVoxels,
                           int64& OutMinX, int64& OutMinY, int64& OutMinZ)
{
	const int64 Anchor[3] = {AnchorX, AnchorY, AnchorZ};
	int64 Min[3];
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (Axis == FaceAxis && GrowAxisSign != 0)
		{
			Min[Axis] = (GrowAxisSign > 0) ? Anchor[Axis] : Anchor[Axis] - (int64)(SizeVoxels - 1);
		}
		else
		{
			Min[Axis] = Anchor[Axis] - (int64)(SizeVoxels / 2);
		}
	}
	OutMinX = Min[0];
	OutMinY = Min[1];
	OutMinZ = Min[2];
}
} // namespace

// Streaming bookkeeping types. File-scope (not exposed to the UHT-parsed
// header -- see VoxelWorldSubsystem.h comment on FVoxelWorldImpl).
namespace VoxelStreaming
{
struct FChunkRecord
{
	// Weak: once registered, the component is owned (GC-rooted) by
	// ChunkOwner's component list; this is a lookup handle, not ownership.
	TWeakObjectPtr<UVoxelChunkComponent> Component;

	// Bumped by MarkChunkDirtyForRemesh whenever an edit dirties this chunk.
	// A worker job snapshots the id at dispatch time; if it no longer
	// matches this value when the result is drained, the result is stale
	// (superseded by an edit while the job was in flight) and is discarded
	// rather than applied -- this IS the stale-result discard mechanism.
	uint64 GenerationId = 1;

	bool bJobInFlight = false;

	// --- Chunk-state debug tint bookkeeping (docs/debug-tooling-plan.md P1,
	// mode 2 + voxel.Debug.ChunkStates) -- all in FVoxelWorldImpl::ElapsedSeconds
	// terms (a free-running clock since Initialize, not GetWorld()'s time).
	// Far-in-the-past defaults so a chunk that has never flashed reads as
	// fully decayed on the very first tint update.
	float LoadedAtSeconds = -1000.f;   // set when Component transitions null -> non-null (first apply)
	float RemeshedAtSeconds = -1000.f; // set on a genuine re-mesh (Component already existed, overlay-aware path)
	bool bHasOverlayBricks = false;    // this exact chunk (no border) currently owns >=1 edited brick
	int32 LastQuadCount = 0;           // for FVoxelWorldImpl::ResidentQuads bookkeeping on replace/unload

	// Underground streaming (docs/status.md "Underground streaming (vertical
	// footprint)"): true iff this chunk entered the desired set ONLY because
	// the anchor was underground and this chunk fell inside the anchor-relative
	// deep box -- i.e. it sits strictly below its footprint's surface band +
	// depth skirt. Those two are pure functions of the footprint, so the
	// existing XY-only hysteresis-exit scan evicts them correctly; the deep box
	// is a function of the anchor's Z as well, so chunks it (and only it) added
	// need the extra vertical keep-test in RecomputeDesiredSet's exit pass.
	// Chunks in the surface band or the skirt are NEVER flagged, so they keep
	// exactly their pre-change lifetime.
	bool bDeepAnchorRelative = false;
};

struct FJobResult
{
	VoxelCoords::FVoxelLevelChunkKey Key;
	uint64 GenerationId = 0;
	TArray<FVoxelChunkQuad> Quads;
	float JobMs = 0.f; // wall time inside the worker task body (docs/debug-tooling-plan.md P1 "Worker timings")
};
} // namespace VoxelStreaming

// M3 wave 1 (docs/m3-plan.md "Transport"): the wire format AVoxelEditRelay's
// MulticastAppliedEntries and AVoxelEarthPlayerController's join-sync RPCs
// carry. Built from vxc::ByteWriter/ByteReader (the SAME primitives
// vxc::EditLog::serialize/parse use) but framed as a flat entry list rather
// than EditLog::serialize's self-describing whole-log format (magic/version/
// seed header, contiguous-from-0 seq requirement) -- broadcasts need to send
// arbitrary TAIL slices of the log (new entries since the last broadcast),
// not always the whole thing from seq 0. Sparse-only cell encoding (no RLE):
// batches here are always small (one player's dig/place/carve, or the
// occasional full join-sync replay), so EditLog's RLE-for-full-brick-
// rewrites optimization isn't worth the complexity. Deliberately does NOT
// touch voxel-core (doctrine: avoid modifying voxel-core unless truly
// unavoidable) -- entries replay through the existing public
// World::applyEdit, one call per brick, exactly like a local dig (see
// FVoxelWorldImpl::ApplyReplicatedEntries below).
namespace
{
using FEditsByBrick = std::unordered_map<vxc::BrickKey, std::vector<vxc::EditCell>, vxc::BrickKeyHash>;

void SerializeEntries(const std::vector<vxc::EditEntry>& Entries, TArray<uint8>& OutBytes)
{
	std::vector<uint8_t> Bytes;
	vxc::ByteWriter W(Bytes);
	W.u64(Entries.size());
	for (const vxc::EditEntry& E : Entries)
	{
		W.i32(E.key.x);
		W.i32(E.key.y);
		W.i32(E.key.z);
		W.u16(static_cast<uint16_t>(E.cells.size()));
		for (const vxc::EditCell& C : E.cells)
		{
			W.u16(C.cell);
			W.u8(C.mat);
		}
	}
	OutBytes.SetNumUninitialized((int32)Bytes.size());
	if (!Bytes.empty())
	{
		FMemory::Memcpy(OutBytes.GetData(), Bytes.data(), Bytes.size());
	}
}

bool ParseEntries(const TArray<uint8>& InBytes, std::vector<vxc::EditEntry>& OutEntries)
{
	OutEntries.clear();
	vxc::ByteReader R(InBytes.GetData(), static_cast<size_t>(InBytes.Num()));
	uint64_t Count = 0;
	if (!R.u64(Count))
	{
		return InBytes.Num() == 0; // an empty batch is valid (nothing to apply)
	}
	OutEntries.reserve((size_t)Count);
	for (uint64_t i = 0; i < Count; ++i)
	{
		vxc::EditEntry E;
		int32_t X = 0, Y = 0, Z = 0;
		uint16_t NumCells = 0;
		if (!R.i32(X) || !R.i32(Y) || !R.i32(Z) || !R.u16(NumCells))
		{
			return false;
		}
		E.key = vxc::BrickKey{X, Y, Z};
		E.cells.resize(NumCells);
		for (uint16_t c = 0; c < NumCells; ++c)
		{
			if (!R.u16(E.cells[c].cell) || !R.u8(E.cells[c].mat))
			{
				return false;
			}
		}
		OutEntries.push_back(std::move(E));
	}
	return true;
}

// W2 dig-breach hook: converts a TryDig/CarveSphere batch's brick+cell edits
// into plain voxel coordinates, filtered to MAT_AIR writes only (dig-style
// clears -- TryPlace's arbitrary materials never breach). UVoxelWaterSubsystem
// itself decides which of these actually border pre-existing open water
// (see NotifyTerrainVoxelsCleared's doc comment); this helper only unpacks
// the coordinates.
void ExtractClearedVoxelCoords(const FEditsByBrick& Edits, TArray<VoxelCoords::FVoxelCoord>& OutCoords)
{
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;
	for (const auto& Entry : Edits)
	{
		const vxc::BrickKey& Key = Entry.first;
		for (const vxc::EditCell& Cell : Entry.second)
		{
			if (Cell.mat != vxc::MAT_AIR)
			{
				continue;
			}
			const int LocalX = Cell.cell % B;
			const int LocalY = (Cell.cell / B) % B;
			const int LocalZ = Cell.cell / (B * B);
			OutCoords.Add(VoxelCoords::FVoxelCoord{int64(Key.x) * B + LocalX, int64(Key.y) * B + LocalY, int64(Key.z) * B + LocalZ});
		}
	}
}

// ADR-0003 item 2 (docs/adr/0003-hydrostatic-persistent-body.md): the
// water-memo invalidation companion to ExtractClearedVoxelCoords above --
// but UNFILTERED by material, since the memo cares about BOTH solidity
// directions (a placed solid matters exactly as much as a dug-out one; see
// UVoxelWaterSubsystem::NotifyTerrainRegionEdited's doc comment). Returns the
// INCLUSIVE voxel-coordinate bounding box spanning every cell in Edits,
// regardless of what material each cell was written to -- one box per edit,
// not one invalidation call per voxel (the "batch per edit" efficiency the
// task calls for). Returns false (Out* untouched) if Edits is empty.
bool ComputeEditVoxelBounds(const FEditsByBrick& Edits, VoxelCoords::FVoxelCoord& OutMin, VoxelCoords::FVoxelCoord& OutMax)
{
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;
	bool bAny = false;
	int64 MinX = 0, MinY = 0, MinZ = 0, MaxX = 0, MaxY = 0, MaxZ = 0;
	for (const auto& Entry : Edits)
	{
		const vxc::BrickKey& Key = Entry.first;
		for (const vxc::EditCell& Cell : Entry.second)
		{
			const int LocalX = Cell.cell % B;
			const int LocalY = (Cell.cell / B) % B;
			const int LocalZ = Cell.cell / (B * B);
			const int64 Vx = int64(Key.x) * B + LocalX;
			const int64 Vy = int64(Key.y) * B + LocalY;
			const int64 Vz = int64(Key.z) * B + LocalZ;
			if (!bAny)
			{
				MinX = MaxX = Vx;
				MinY = MaxY = Vy;
				MinZ = MaxZ = Vz;
				bAny = true;
			}
			else
			{
				MinX = FMath::Min(MinX, Vx); MaxX = FMath::Max(MaxX, Vx);
				MinY = FMath::Min(MinY, Vy); MaxY = FMath::Max(MaxY, Vy);
				MinZ = FMath::Min(MinZ, Vz); MaxZ = FMath::Max(MaxZ, Vz);
			}
		}
	}
	if (bAny)
	{
		OutMin = VoxelCoords::FVoxelCoord{MinX, MinY, MinZ};
		OutMax = VoxelCoords::FVoxelCoord{MaxX, MaxY, MaxZ};
	}
	return bAny;
}

// Same idea as ComputeEditVoxelBounds, for the StampVoxels-based test fixture
// paths (SpawnTreeFixtureAt/SpawnStructureFixtureAt) whose callers already
// hold a plain TArray<FVoxelCoord> rather than an FEditsByBrick -- these are
// ALSO non-air (MAT_ROCK) edits the memo must be told about (ADR-0003 item 2:
// "make water learn about every solidity change... and non-air edits").
bool ComputeVoxelCoordBounds(const TArray<VoxelCoords::FVoxelCoord>& Coords, VoxelCoords::FVoxelCoord& OutMin,
                              VoxelCoords::FVoxelCoord& OutMax)
{
	if (Coords.Num() == 0)
	{
		return false;
	}
	int64 MinX = Coords[0].X, MinY = Coords[0].Y, MinZ = Coords[0].Z;
	int64 MaxX = MinX, MaxY = MinY, MaxZ = MinZ;
	for (const VoxelCoords::FVoxelCoord& V : Coords)
	{
		MinX = FMath::Min(MinX, V.X); MaxX = FMath::Max(MaxX, V.X);
		MinY = FMath::Min(MinY, V.Y); MaxY = FMath::Max(MaxY, V.Y);
		MinZ = FMath::Min(MinZ, V.Z); MaxZ = FMath::Max(MaxZ, V.Z);
	}
	OutMin = VoxelCoords::FVoxelCoord{MinX, MinY, MinZ};
	OutMax = VoxelCoords::FVoxelCoord{MaxX, MaxY, MaxZ};
	return true;
}
} // namespace

// Bounded admission switch (docs/status.md "Streaming pipeline re-measure +
// rework"; see FVoxelWorldImpl::AdmissionCutoffDistSq for what it does).
//
// Deliberately a COMMAND-LINE switch resolved once at first use, not a cvar:
// `-ExecCmds` cvars are applied after streaming has already begun, so an
// -ExecCmds A/B silently measures the same warmed-up state twice (this project
// has been bitten by exactly that -- see -VoxelNoUnderground, which exists for
// the same reason). `-VoxelPendingJobCap=0` restores the unbounded,
// pre-this-wave behaviour for A/B measurement.
namespace VoxelStreamAdmission
{
// Max chunks that may sit queued for worker meshing at once. At the measured
// ~250 chunks/s drain rate this is ~8 seconds of queued work -- deep enough
// that the worker pool never runs dry between recomputes (which happen ~8x/s),
// shallow enough that the queue costs a fraction of what an uncapped ~28k-deep
// one does in the per-recompute sort, filter and exit scan.
constexpr int32 kDefaultPendingJobCap = 2048;

int32 GetPendingJobCap()
{
	static const int32 Cap = []
	{
		int32 Value = kDefaultPendingJobCap;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelPendingJobCap="), Value);
		return FMath::Max(0, Value); // 0 = unbounded (pre-wave behaviour)
	}();
	return Cap;
}
} // namespace VoxelStreamAdmission

// FVoxelWorldImpl -- the voxel-core side of the subsystem, defined only here
// so VoxelWorldSubsystem.h (UHT-parsed) never sees a voxel-core header. Also
// owns ALL Stage 2 streaming state (chunk records, pending-work queues, the
// worker-result MPSC queue, in-flight task handles): none of it is
// UE-reflection-visible, so it belongs behind the same PImpl boundary.
struct FVoxelWorldImpl
{
	// Track B2: TileDir/TileScale select the tile source (see MakeTileSampler
	// above for the full policy) -- TileDir empty is the pre-Track-B2 default
	// (SyntheticTileSampler, byte-identical to before).
	explicit FVoxelWorldImpl(uint64 Seed, const FString& TileDir, int32 TileScale)
		: Tiles(MakeTileSampler(Seed, TileDir, TileScale, bUsingTileGrid))
		, Voxels(Seed, *Tiles)
	{
		// bUsingTileGrid (declared, and thus constructed via its NSDMI, BEFORE
		// Tiles below -- member init order follows DECLARATION order, not this
		// list's order) is already valid by the time MakeTileSampler's
		// out-param write above runs, so GridTiles can safely depend on it here.
		if (bUsingTileGrid)
		{
			// Non-owning: Tiles (unique_ptr<ITileSampler>) is the sole owner.
			// static_cast, not dynamic_cast: voxel-core is RTTI-agnostic by
			// design (Counters/mesher etc. never assume it's enabled), and
			// bUsingTileGrid being true is exactly MakeTileSampler's own
			// guarantee that Tiles.get() really does point at a TileGridSampler.
			GridTiles = static_cast<vxc::TileGridSampler*>(Tiles.get());
		}

		// Fixed-size ring buffer (docs/debug-tooling-plan.md P1 "Worker
		// timings"): sized once here so DrainResults never touches TArray
		// growth on its hot path.
		WorkerJobMsWindow.Init(0.f, WorkerJobMsWindowSize);
		// M2 wave 2 item 1: same fixed-size-ring-buffer treatment, one window
		// per ring level (report requirement: "worker p50/p95 per level").
		for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
		{
			LevelWorkerJobMsWindow[Level].Init(0.f, WorkerJobMsWindowSize);
		}
	}

	// Track B2: bUsingTileGrid MUST be declared (and default-constructed via
	// its NSDMI) before Tiles below -- see the ctor's mem-initializer-order
	// comment. Tiles itself must be declared before Voxels: Voxels holds a
	// live ITileSampler& into whatever Tiles owns, so Tiles has to already be
	// a fully-formed object by the time Voxels' own constructor runs.
	bool bUsingTileGrid = false;
	std::unique_ptr<vxc::ITileSampler> Tiles;
	// Non-owning view of Tiles when bUsingTileGrid (set in the ctor body,
	// after Tiles exists) -- telemetry only (MaybeLogCounters' missing-tile
	// warning below); nullptr whenever the synthetic sampler is in use.
	//
	// THREAD-SAFETY: meshing worker jobs (DispatchJobs) query the active
	// sampler concurrently via Voxels' amplifier/generated-world path. That is
	// safe here because TileGridSampler's tile_ map is READ-ONLY after this
	// ctor returns -- every loadTile call above happens before Initialize()
	// hands Impl back to the subsystem, i.e. strictly before any job is ever
	// dispatched. missingTileQueries is the one field worker jobs and this
	// (game-thread) telemetry both touch concurrently; a parallel voxel-core
	// change is making it atomic, so it's read here through an implicit
	// conversion into a plain uint64 local (see MaybeLogCounters) rather than
	// captured by reference/pointer -- that compiles whether the member is
	// today's plain uint64_t or the incoming std::atomic<uint64_t>.
	vxc::TileGridSampler* GridTiles = nullptr;
	vxc::World<VoxelCoords::BrickEdgeVoxels> Voxels;

	// MaybeLogCounters' missing-tile delta tracking (bUsingTileGrid only).
	uint64 LastMissingTileQueries = 0;

	// --- Stage 2 streaming state (docs/m1-plan.md Stage 2 decisions table);
	// M2 (docs/m2-plan.md "Ring streaming" row) generalizes every key here
	// from FVoxelChunkKey to FVoxelLevelChunkKey (level, chunk) -- level 0
	// keys behave identically to the pre-M2 single-ring scheme. ---

	TMap<VoxelCoords::FVoxelLevelChunkKey, VoxelStreaming::FChunkRecord> ChunkRecords;

	// M1 hitch-gap wave (component pooling, docs/status.md M1 gate row): a
	// component that leaves the desired set (DrainUnloads) or goes empty on
	// re-mesh (ApplyMeshResult's zero-quads branch) is hidden + parked here
	// instead of DestroyComponent()'d; AcquireChunkComponent (ApplyMeshResult's
	// first-load branch) pops from here before ever falling back to
	// NewObject+RegisterComponent. The hitch-attribution instrumentation
	// (docs/status.md "Perf-run hitches") pinned render-thread scene-mutation
	// backlog -- not this subsystem's own tick cost -- as the dominant cost at
	// ring-boundary crossings; reusing a UPrimitiveComponent (and, just as
	// importantly, its already-created UMaterialInstanceDynamic -- see
	// ApplyRingFadeParams) avoids the AddPrimitive/RemovePrimitive churn a
	// fresh Unregister+destroy / NewObject+Register pair costs. Capped at
	// voxel.Stream.ComponentPoolMax (default 512); unloads past the cap still
	// destroy the old way, so this never grows unboundedly. TWeakObjectPtr for
	// the same reason FChunkRecord::Component is weak -- the actual GC root is
	// ChunkOwner's OwnedComponents list; pooled components stay
	// registered/attached (never Unregister/DestroyComponent'd) the whole time
	// they're parked, so the weak pointer should never actually go stale in
	// practice (AcquireChunkComponent guards against it anyway).
	TArray<TWeakObjectPtr<UVoxelChunkComponent>> ComponentPool;

	// Nearest-first-within-level, lower-level-wins-ties priority queues
	// (docs/m2-plan.md item 1: "Budgets shared across levels, nearest-first
	// within level, lower level (finer) wins priority at equal distance").
	// Sorted so Pop() (O(1), removes from the back) always yields the
	// highest-priority pending chunk -- see SortPendingQueues.
	TArray<VoxelCoords::FVoxelLevelChunkKey> PendingJobKeys;        // worker-dispatch pending (any level, minus edited-ancestor chunks)
	TArray<VoxelCoords::FVoxelLevelChunkKey> PendingGameThreadKeys; // overlay-aware game-thread mesh pending -- ANY level as of
	                                                                 // M2 wave 2 (see MarkChunkDirtyForRemesh/PropagateEditToMips)
	// SortPendingQueues' decorate-sort-undecorate scratch (see its body): kept
	// as members purely to reuse the allocation across the ~8 calls/second.
	struct FSortEntry
	{
		double DistSq;
		VoxelCoords::FVoxelLevelChunkKey Key;
	};
	TArray<FSortEntry> SortScratchJob;
	TArray<FSortEntry> SortScratchGameThread;

	// RecomputeDesiredSet's per-call eviction set (see its batched queue-filter
	// pass); a member only to reuse the allocation.
	TSet<VoxelCoords::FVoxelLevelChunkKey> EvictedThisCall;

	TArray<VoxelCoords::FVoxelLevelChunkKey> PendingUnloadKeys;
	TSet<VoxelCoords::FVoxelLevelChunkKey> PendingUnloadSet; // de-dupes PendingUnloadKeys across recomputes

	// M2 wave 2 item 1 ("Cross-job mip caching"): shared cross-job cache of
	// pure-generated level>=1 mip bricks, consulted/populated by every
	// worker job's FCachedMipBuilder (see MakeLevelSampler). Owned here
	// (outlives every job -- Deinitialize's WaitForInFlightTasks guarantees
	// no job survives Impl, same lifetime assumption DispatchJobs already
	// makes for GenPtr/QueuePtr/CounterPtr).
	FSharedMipCache SharedMipCache;

	// M2 wave 2 item 1: bumped once per edit batch, BEFORE SharedMipCache
	// invalidation, in PropagateEditToMips -- lets an in-flight worker job
	// (dispatched before the edit landed) detect that an edit raced its
	// build and skip re-inserting a now-stale value (see FCachedMipBuilder's
	// GlobalEditEpoch/EpochSnapshot doc comment). Same "snapshot at
	// dispatch, compare later" idiom FChunkRecord::GenerationId already uses
	// for stale-RESULT discarding, applied here to stale-CACHE-INSERT
	// discarding instead.
	std::atomic<uint64> EditEpoch{1};

	// M2 wave 2 item 2 ("Distant-edit mip propagation"): per-level set of
	// chunk keys known to have at least one edited level-0 voxel somewhere
	// in their footprint (populated by PropagateEditToMips, consulted by
	// RecomputeDesiredSet/DispatchJobs exactly like level 0's
	// ChunkHasEditedBrick scan) -- index 0 is unused (level 0 keeps its
	// existing live overlay scan, which is already exact; this table exists
	// because scanning the overlay directly at level>=1 would mean visiting
	// up to (ChunkEdgeBricks*2^Level)^3 level-0 brick keys per CANDIDATE
	// chunk, prohibitive at L3/L4 -- this set is maintained incrementally
	// instead, sized by edit count rather than footprint size).
	TSet<VoxelCoords::FVoxelChunkKey> EditedAncestorChunks[VoxelCoords::kNumLevels];

	TQueue<VoxelStreaming::FJobResult, EQueueMode::Mpsc> ResultsQueue;
	FThreadSafeCounter JobsInFlightCounter;
	TArray<UE::Tasks::TTask<void>> InFlightTasks; // only for a clean Deinitialize() wait; see WaitForInFlightTasks

	bool bHasRecomputed = false;
	VoxelCoords::FVoxelChunkKey LastAnchorChunk{}; // level 0 anchor chunk; gates the whole RecomputeDesiredSet call
	FVector LastAnchorLocation = FVector::ZeroVector;

	// Underground streaming (see namespace VoxelUnderground): whether the
	// anchor is currently below the terrain surface, with enter/exit
	// hysteresis. Recomputed once per TickStreaming (one amplifier column
	// sample), NOT per footprint. While true, the anchor's chunk Z is part of
	// what gates a recompute -- otherwise digging straight down would never
	// trigger one, since the existing gate is XY-only.
	bool bAnchorUnderground = false;
	// Returns the underground state for this anchor, applying hysteresis
	// against the current bAnchorUnderground value.
	bool EvaluateAnchorUnderground(const FVector& Anchor) const;

	// Per-level entry-scan gating (docs/m2-plan.md "Perf budget" row: "Ring
	// levels ... are ~constant cost by construction"): re-running the O(candidates)
	// entry scan for every level on every level-0 chunk crossing (every 3.2m
	// of movement) would waste work for outer levels whose own chunk edge is
	// much larger -- level L only re-scans once the anchor has crossed into a
	// new LEVEL-L chunk. The hysteresis/unload pass (cheap: iterates existing
	// ChunkRecords, no candidate generation) still runs every call.
	bool bHasRecomputedLevel[VoxelCoords::kNumLevels] = {};
	VoxelCoords::FVoxelChunkKey LastAnchorChunkPerLevel[VoxelCoords::kNumLevels] = {};

	// ComputeFootprintChunkZRange memo (docs/status.md "R3/R4 recompute
	// amortization"). That function is a PURE function of (Level, ChunkX,
	// ChunkY) and the amplifier -- and the amplifier is a pure function of the
	// seed, fixed for the subsystem's lifetime; edits live in the overlay and
	// never move terrain elevation. So caching it cannot change which chunks
	// are desired, only how long it takes to decide. It is worth caching
	// because consecutive entry scans re-visit almost the same annulus: a
	// one-chunk anchor step changes only the annulus MARGIN (~40 of ~1250
	// level-0 footprints), yet the uncached scan re-samples the amplifier at 4
	// corners for every footprint, every time.
	//
	// Keyed with Z=0 (the value IS the Z range, so Z is not part of the input).
	struct FFootprintZRange
	{
		int32 ChunkZMin = 0;
		int32 ChunkZMax = 0;
	};
	mutable TMap<VoxelCoords::FVoxelLevelChunkKey, FFootprintZRange> FootprintZRangeCache;

	// --- Bounded admission (docs/status.md "Streaming pipeline re-measure +
	// rework") ---------------------------------------------------------------
	//
	// The desired set is produced far faster than the worker pool can drain it
	// (measured: ~28-29k chunks queued, ~24 jobs in flight pinned at the cap,
	// ~250 chunks/s consumed, and 207k evictions per 15k loads over a 60s
	// flight -- i.e. ~93% of everything admitted left the desired set before a
	// worker ever looked at it). Every one of those admissions cost a
	// ChunkRecords TMap insert, a slot in the O(n log n) pending-queue sort
	// (8x/second), a slot in the O(evictions) queue filter, AND a slot in the
	// O(tracked) exit hysteresis scan -- all to describe work that was never
	// going to happen.
	//
	// So the queue is capped at what can actually be consumed. Two gates, both
	// keyed on the SAME 3D chunk-centre distance SortPendingQueues already
	// prioritises by, so neither changes the ORDER work is done in -- only how
	// much never-to-be-done work is materialised:
	//  (a) AdmissionCutoffDistSq -- while the queue is full, a candidate farther
	//      than the farthest currently-queued chunk is not made a record at all
	//      (it would only be dropped again by (b) this same call). Candidates
	//      NEARER than the cutoff are always admitted, so nothing near the
	//      player ever waits behind a full queue of far work.
	//  (b) TruncatePendingJobQueue -- after the sort, the farthest entries past
	//      the cap are dropped, record and all. Only ever entries that have no
	//      component and no job in flight (a queued chunk by definition has
	//      never meshed), so a drop costs nothing and loses nothing: the entry
	//      scan re-enumerates any untracked in-annulus footprint the next time
	//      that level scans, which is exactly when its distance ranking can have
	//      changed (the per-level gate fires on the anchor crossing a level-L
	//      chunk boundary).
	//
	// Determinism is untouched -- this changes only WHICH chunks are queued,
	// never what a chunk contains.
	double AdmissionCutoffDistSq = DBL_MAX; // farthest queued chunk's distSq when the queue is at cap; DBL_MAX = not full

	// True while the cap is holding work back (something was rejected or
	// dropped by the two gates above). This is what makes the cap safe for a
	// player who STOPS: RecomputeDesiredSet is triggered by anchor movement, so
	// with an unbounded queue a stationary player kept a ~26k backlog feeding
	// the workers for minutes, whereas a capped queue drains in seconds and --
	// without this -- would then sit idle with unloaded chunks still in range,
	// because nothing would ever re-enumerate them. TickStreaming therefore also
	// recomputes when a deferring queue has drained (see its trigger), and that
	// recompute clears the per-level scan gate so the entry scans actually run
	// with the anchor sitting still.
	bool bAdmissionDeferredWork = false;
	// Per-RecomputeDesiredSet-call bookkeeping for the flag above (reset at the
	// top of every call): how many levels actually ran their entry scan, and how
	// many candidates gate (a) declined.
	int32 LevelsScannedThisCall = 0;
	int32 CandidatesRejectedThisCall = 0;
	// Admission budget, per LEVEL per RecomputeDesiredSet call. The distance
	// cutoff alone does not bound how much a single call admits: DispatchJobs
	// pops from the NEAR end, so the queue steadily fills with the farthest
	// candidates, the cutoff drifts out with it, and every call was admitting
	// ~3k newly-near chunks and dropping ~3k newly-far ones (each an insert +
	// an erase, and a slot in that call's sort). Bounding admissions bounds all
	// three at once. Sized well above the measured per-call dispatch rate
	// (~160), so it never starves the workers -- what it removes is churn, not
	// throughput.
	//
	// Per level, not per call, because levels are scanned in ascending order
	// and level 0 rescans on EVERY call while level 1 rescans on every second
	// one: one shared budget let R0 spend it before R1 was even looked at, and
	// R1 residency measured ~8% lower for it (620-664 loaded vs 699-705). A
	// budget each keeps the ring mix where it was.
	int32 AdmissionsThisLevel = 0;

	// Bound on the memo above. The live working set is the union of all five
	// annuli, ~5k footprints; this cap leaves an order of magnitude of slack
	// for drift before PruneFootprintZRangeCache drops the entries that are
	// far from the anchor (a distance-based prune rather than a full clear, so
	// pruning never costs a re-sample burst).
	static constexpr int32 FootprintZRangeCacheMaxEntries = 65536;

	// Cumulative counters, logged periodically (LogVoxelPerf, every ~5s) --
	// never per-chunk (docs/m1-plan.md: "log cumulative ... periodically").
	int64 TotalChunksLoaded = 0;
	int64 TotalChunksUnloaded = 0;
	int64 TotalQuadsLoaded = 0;
	float LogTimerAccumSeconds = 0.f;

	// --- Debug-tooling instrumentation (docs/debug-tooling-plan.md P1) -------

	// Engine-free perf counters (voxel-core/include/voxelcore/counters.h),
	// incremented around this file's own calls into voxel-core (worker mesh
	// jobs, the edit-log apply path) -- voxel-core's internals stay untouched.
	vxc::Counters PerfCounters;

	// Free-running clock since Initialize (NOT GetWorld()'s time -- this impl
	// has no UWorld& handy outside Tick/TickStreaming's parameters), used only
	// for chunk-state tint flash decay timing.
	float ElapsedSeconds = 0.f;

	int64 ResidentQuads = 0;         // sum of FChunkRecord::LastQuadCount across every tracked record with a live component
	int64 StaleResultsDiscarded = 0; // cumulative worker results dropped (chunk left the desired set, or superseded by an edit)

	// Rolling window of per-chunk worker mesh-job milliseconds (P1 "Worker
	// timings"), filled on the game thread in DrainResults as results are
	// drained -- no cross-thread access, so a plain ring buffer suffices.
	static constexpr int32 WorkerJobMsWindowSize = 256;
	TArray<float> WorkerJobMsWindow;
	int32 WorkerJobMsWindowNext = 0;
	int32 WorkerJobMsWindowCount = 0;

	// M2 wave 2 item 1: same rolling window, split per ring level (the perf
	// report this wave asks for is "worker p50/p95 per level" -- the global
	// window above averages every level together and would hide the wave-1
	// regression this item fixes).
	TArray<float> LevelWorkerJobMsWindow[VoxelCoords::kNumLevels];
	int32 LevelWorkerJobMsWindowNext[VoxelCoords::kNumLevels] = {};
	int32 LevelWorkerJobMsWindowCount[VoxelCoords::kNumLevels] = {};

	// This-tick budget-saturation fractions (P1 "budget saturation (% of
	// per-frame apply/unload/re-mesh budgets used)"), set by the three Drain*
	// functions and blended together once per tick in UpdatePerfSnapshot.
	float LastAppliedFrac = 0.f;
	float LastRemeshFrac = 0.f;
	float LastUnloadFrac = 0.f;
	float BudgetSaturationAccum = 0.f;
	int32 BudgetSaturationSamples = 0;

	// --- Hitch attribution (docs/status.md "Perf-run hitches" isolation task)
	// -----------------------------------------------------------------------
	// Reset to 0 at the top of every TickStreaming call, filled by
	// DispatchJobs/DrainResults/DrainGameThreadMesh/DrainUnloads as they run,
	// and logged (LogVoxelPerf) at the bottom of TickStreaming only when this
	// frame's DeltaTime exceeded VoxelDebug::kHitchThresholdMs -- "measure
	// before fixing" diagnostic breakdown of what a hitch frame actually did.
	int32 ThisFrameAppliesFromWorker = 0; // DrainResults: worker-mesh-result applies (ApplyMeshResult calls, any outcome)
	int32 ThisFrameProxiesCreated = 0;    // ApplyMeshResult calls (either path) that were a genuine first load (pooled reuse OR NewObject+RegisterComponent)
	int32 ThisFrameEditRemeshes = 0;      // DrainGameThreadMesh: overlay-aware re-mesh/first-load applies
	int32 ThisFrameUnloads = 0;           // DrainUnloads: pool-park-or-destroy events
	float ThisFrameDispatchMs = 0.f;
	float ThisFrameApplyMs = 0.f;
	float ThisFrameRemeshMs = 0.f;
	float ThisFrameUnloadMs = 0.f;

	// M1 steady-state-hitch wave (docs/status.md "R3/R4 recompute
	// amortization"): the four Drain*/Dispatch timers above cover everything
	// TickStreaming does EXCEPT RecomputeDesiredSet, which is why the M1 gate's
	// residual ~14-15ms `subsystemTickMs` hitches showed up as unattributed
	// tick time. These break that call down into its three stages (exit
	// hysteresis scan / per-level entry scan / pending-queue sort) so the cost
	// can be attributed to a specific ring level before anything is changed.
	// Same always-collected, log-only-on-hitch policy as the timers above.
	float ThisFrameRecomputeMs = 0.f;
	float ThisFrameExitScanMs = 0.f;
	float ThisFrameSortMs = 0.f;
	float ThisFrameLevelEntryMs[VoxelCoords::kNumLevels] = {};
	int32 ThisFrameLevelFootprints[VoxelCoords::kNumLevels] = {}; // in-annulus XY footprints visited (= ComputeFootprintChunkZRange calls)

	// Running maxima since the last periodic counter log (MaybeLogCounters
	// resets them): a recompute burst that lands on a frame which does NOT
	// cross the hitch threshold is invisible to the per-hitch log above, so
	// these give the cost distribution independent of whether it hitched.
	float MaxRecomputeMs = 0.f;
	float MaxExitScanMs = 0.f;
	float MaxSortMs = 0.f;
	float MaxLevelEntryMs[VoxelCoords::kNumLevels] = {};
	int32 LevelEntryScans[VoxelCoords::kNumLevels] = {}; // how many times each level's entry scan actually ran
	int32 RecomputeCalls = 0;

	// --- Streaming pipeline re-measure (docs/status.md "Streaming pipeline
	// re-measure + rework") -------------------------------------------------
	// The hitch log above only fires on frames over the 33.3ms threshold, and a
	// -nullrhi throughput run has none -- so "where does per-tick time go" and
	// "how much worker output is actually used" were both unmeasurable. These
	// accumulate every tick and are reported (then reset) by the 5s periodic
	// log. All are plain counter adds on a path that already calls
	// FPlatformTime::Seconds four times per tick; nothing new is timed.
	double AccumDispatchMs = 0.0;
	double AccumApplyMs = 0.0;
	double AccumRemeshMs = 0.0;
	double AccumUnloadMs = 0.0;
	double AccumRecomputeMs = 0.0;
	double AccumTickMs = 0.0;
	int32 AccumTicks = 0;
	int64 JobsDispatchedSinceLog = 0;   // DispatchJobs: worker jobs actually launched
	// R2-R4 ring starvation wave: the per-ring question the logs could not
	// answer. "Voxel rings" reports per-level RESIDENCY and QUEUE DEPTH, which
	// cannot distinguish "this ring is queued and dispatching but every result
	// is buried rock meshing to zero quads" from "this ring never gets a worker
	// at all" -- both read as loaded=0 with thousands pending. These are the
	// dispatch side: jobs launched per level, and of those, results that came
	// back with zero quads. Cumulative counters (never reset) alongside the
	// since-log ones, so a run's total per-ring dispatch is readable from the
	// final log line alone.
	int64 LevelJobsDispatchedSinceLog[VoxelCoords::kNumLevels] = {};
	int64 LevelJobsDispatchedTotal[VoxelCoords::kNumLevels] = {};
	int64 LevelZeroQuadTotal[VoxelCoords::kNumLevels] = {};
	int64 LevelChunksLoadedTotal[VoxelCoords::kNumLevels] = {}; // component creations per level, cumulative
	int64 ResultsDrainedSinceLog = 0;   // DrainResults: results dequeued (live + stale)
	int64 StaleDiscardsSinceLog = 0;    // ...of which discarded (record gone / superseded)
	int64 ZeroQuadAppliesSinceLog = 0;  // ...of which meshed to zero quads (buried: real work, no component)
	int64 RecordsAddedSinceLog = 0;     // RecomputeDesiredSet: ChunkRecords.Add calls (admission)
	int64 RecordsEvictedSinceLog = 0;   // DrainUnloads: records erased (component-less + parked)
	int64 CandidatesRejectedSinceLog = 0; // bounded admission: in-annulus candidates NOT admitted (never became records)
	int64 RecordsDroppedSinceLog = 0;     // bounded admission: queued-but-never-meshed records displaced by nearer work

	// The 60fps bar is 16.6ms, but the hitch log/`UVoxelPerfRunSubsystem`'s
	// hitch count both use the much looser 33.3ms threshold -- a recurring
	// ~20-30ms burst is a hard 60fps violation that is completely invisible to
	// both. Counted here (cumulative + since-last-log) so the gate's actual
	// criterion can be reported, not just the 33.3ms proxy for it.
	int64 TotalFramesOver60FpsBar = 0;
	int32 FramesOver60FpsBarSinceLog = 0;

	// M1 hitch-gap wave (component pooling): AcquireChunkComponent pool hits
	// (a first-load that reused a parked component instead of NewObject'ing a
	// fresh one) this frame / cumulative since startup.
	int32 ThisFramePoolReuses = 0;
	int64 TotalPoolReuses = 0;

	// 1Hz refresh window (P1 "1Hz refresh of text, per-frame collection"):
	// per-frame accumulation happens continuously above; the published
	// snapshot itself only updates once this timer rolls over.
	float PerfRefreshAccumSeconds = 0.f;
	int64 LoadedAtLastPerfRefresh = 0;
	int64 UnloadedAtLastPerfRefresh = 0;
	int64 PoolReusesAtLastPerfRefresh = 0;
	FVoxelPerfSnapshot LastPerfSnapshot;

	// Tracks the previous tick's VoxelDebug::IsChunkStatesEnabled() /
	// IsRingsEnabled() so the off-transition can drop every component's MID
	// exactly once (constraint: zero MID cost once both layers are off
	// again).
	bool bChunkStatesWasEnabled = false;
	bool bRingsWasEnabled = false;

	void TickStreaming(const FVector& Anchor, AActor& Owner, USceneComponent& Root, UMaterialInterface* Material, float DeltaTime);
	void WaitForInFlightTasks();

	// Dig/place (edit-log authority path). Game thread only. OutPredicted,
	// when non-null, receives a COPY of the per-brick cell groups actually
	// applied (M3 wave 1 client-prediction tracking, docs/m3-plan.md
	// "Prediction") -- populated before ApplyGroupedEdits moves the cells
	// out of the local EditsByBrick map, so it reflects exactly what this
	// call wrote to the overlay.
	bool TryDig(const FVector& CameraLoc, const FVector& CameraDir, int32 SizeVoxels, FEditsByBrick* OutPredicted = nullptr);
	bool TryPlace(const FVector& CameraLoc, const FVector& CameraDir, int32 SizeVoxels, uint8 MaterialId,
	              const FVector& PlayerActorLocation, FEditsByBrick* OutPredicted = nullptr);

	// Explosives v1 (edit-log authority path). Game thread only. OutPredicted: see TryDig.
	int32 CarveSphere(const FVector& CenterUU, double RadiusUU, double JitterUU, FEditsByBrick* OutPredicted = nullptr);

	// M3 wave 1 (docs/m3-plan.md): applies a batch of server-authoritative
	// entries (parsed off the wire by UVoxelWorldSubsystem::ApplyReplicatedEntries)
	// through the SAME ApplyGroupedEdits tail TryDig/TryPlace/CarveSphere use
	// -- one World::applyEdit per touched brick, dirty-chunk marking, mip
	// propagation, all identical to a local edit. Also reconciles any
	// pending local predictions touching the same bricks (see
	// ReconcilePrediction below) before applying. Game thread only (same
	// constraint as every other edit-log entry point).
	void ApplyReplicatedEntries(const std::vector<vxc::EditEntry>& Entries);

	// M3 wave 1 client-prediction bookkeeping: brick -> the cells this
	// client's OWN not-yet-confirmed local prediction wrote there (see
	// TryDig's OutPredicted doc comment). Reconciled/erased by
	// ReconcilePrediction as confirming (or superseding) server entries
	// arrive. Deliberately public, all-fields-public struct convention
	// (matches ChunkRecords etc. above) -- read/written by the free
	// role-split helper functions in this file (TryDigReplica et al).
	FEditsByBrick PendingPredictedCellsByBrick;

	// M3 wave 1 authority-side broadcast bookkeeping: log size already sent
	// via AVoxelEditRelay::MulticastAppliedEntries (see BroadcastNewEntries
	// in this file). Authority only in practice (NM_Client never appends
	// via this path, only via ApplyReplicatedEntries); harmless if unused.
	uint64 LastBroadcastSeq = 0;

	FVoxelPerfSnapshot GetPerfSnapshot() const { return LastPerfSnapshot; }

private:
	// M3 wave 1 (docs/m3-plan.md "Prediction reconcile v1"): drops
	// PendingPredictedCellsByBrick[Key] if it exact-matches ConfirmedCells
	// (silent confirmation -- deterministic derivation means byte-identical
	// results, so this is the common case for a client's own edits). A
	// present-but-different entry is a v1-acceptable brick-granularity
	// mismatch: logs a warning and drops the stale prediction: the caller
	// (ApplyReplicatedEntries) already applies ConfirmedCells' exact values
	// to the overlay and marks the chunk dirty for re-mesh regardless of
	// match, so the overlay always converges to server truth for the cells
	// the confirmed entry actually names -- only cells the client predicted
	// but the server DIDN'T confirm could remain locally wrong past this
	// point (accepted v1 limitation, see docs/m3-plan.md wave 2 note on
	// prediction/reconcile polish). A no-op if Key has no pending prediction
	// (either not ours, or already confirmed).
	void ReconcilePrediction(const vxc::BrickKey& Key, const std::vector<vxc::EditCell>& ConfirmedCells);


	void RecomputeDesiredSet(const FVector& Anchor);
	// Bounded admission gate (b) -- see AdmissionCutoffDistSq. Called only from
	// RecomputeDesiredSet, immediately after SortPendingQueues.
	void TruncatePendingJobQueue();
	void ComputeFootprintChunkZRange(int32 ChunkX, int32 ChunkY, int32 Level, int32& OutChunkZMin, int32& OutChunkZMax) const;
	// Memoized wrapper around ComputeFootprintChunkZRange -- see
	// FootprintZRangeCache's doc comment for why memoizing is exact.
	void FootprintChunkZRangeCached(int32 ChunkX, int32 ChunkY, int32 Level, int32& OutChunkZMin, int32& OutChunkZMax) const;
	void PruneFootprintZRangeCache(const FVector& Anchor);
	void SortPendingQueues(const FVector& Anchor);
	void DispatchJobs();
	void DrainResults(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material);
	void DrainGameThreadMesh(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material);
	void DrainUnloads();
	// Returns true iff this call created a brand-new UVoxelChunkComponent
	// (NewObject+RegisterComponent) -- i.e. a genuine "proxy created" event as
	// opposed to SetChunkQuads on an already-resident component (still a proxy
	// RE-create via MarkRenderStateDirty, but without the RegisterComponent/
	// scene-attach overhead). Hitch-attribution callers (DrainResults,
	// DrainGameThreadMesh) sum this into ThisFrameProxiesCreated.
	bool ApplyMeshResult(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material,
	                      const VoxelCoords::FVoxelLevelChunkKey& Key, VoxelStreaming::FChunkRecord& Rec, TArray<FVoxelChunkQuad>&& Quads,
	                      bool bIsGameThreadMesh);
	// M1 hitch-gap wave (component pooling): returns a pooled component
	// (popped + counted as a reuse) if ComponentPool is non-empty, else falls
	// back to the pre-pooling NewObject+SetupAttachment+RegisterComponent
	// path. Never returns null. Caller (ApplyMeshResult) is responsible for
	// re-applying every per-load property (level, position, material,
	// visibility) regardless of which path this took -- a pooled component's
	// stale level/position/quads are exactly the things about to be
	// overwritten, and a fresh component needs them set for the first time.
	UVoxelChunkComponent& AcquireChunkComponent(AActor& Owner, USceneComponent& Root);
	// M1 hitch-gap wave: hides InComp, drops its render proxy (empty quads),
	// resets its debug tint to identity, and parks it in ComponentPool --
	// unless the pool is already at voxel.Stream.ComponentPoolMax, in which
	// case this falls back to the pre-pooling DestroyComponent() (no
	// unbounded pool growth). Deliberately leaves ChunkLevel/ChunkMaterial/
	// ChunkMID untouched (see the .cpp definition) -- reusing that MID
	// without recreating it is a real part of the pooling win, not just the
	// register/unregister avoidance.
	void ReturnChunkComponentToPool(UVoxelChunkComponent& InComp);
	// M2 wave 2: generalized from a level-0-only helper (wave 1) to any
	// level -- both level-0 edited chunks AND their level>=1 mip ancestors
	// (see PropagateEditToMips) route through here identically.
	void MarkChunkDirtyForRemesh(const VoxelCoords::FVoxelLevelChunkKey& LevelKey);
	// M2 wave 2 item 2 ("Distant-edit mip propagation"): for every level-0
	// chunk key in DirtyLevel0Chunks (already apron-extended across
	// render-chunk borders by CollectDirtyChunks), marks dirty the ancestor
	// mip chunk at every level 1..kNumLevels-1 (cheap key math, see
	// VoxelCoords::AncestorChunkKey), invalidates that footprint's entries
	// in SharedMipCache (stale pure values), and records the ancestor in
	// EditedAncestorChunks so it takes the overlay-aware path on any FUTURE
	// load too (not just chunks that happen to be resident right now).
	void PropagateEditToMips(const TSet<VoxelCoords::FVoxelChunkKey>& DirtyLevel0Chunks);
	void CollectDirtyChunks(int64 Vx, int64 Vy, int64 Vz, TSet<VoxelCoords::FVoxelChunkKey>& Out) const;
	bool ChunkHasEditedBrick(const VoxelCoords::FVoxelChunkKey& ChunkKey) const;
	// M2 wave 2: "does this (level,chunk) currently need the overlay-aware
	// game-thread mesh path" -- level 0 via ChunkHasEditedBrick's live scan,
	// level>=1 via EditedAncestorChunks membership. Used by
	// RecomputeDesiredSet (initial routing) and DispatchJobs (defensive
	// re-check for an edit landing between recompute and dispatch).
	bool NeedsOverlayAwarePath(const VoxelCoords::FVoxelLevelChunkKey& LevelKey) const;
	// Strict (no 1-brick border) variant used only for the "edited-chunk
	// orange tint" test -- ChunkHasEditedBrick's border scan is correct for
	// MESH ROUTING (a pristine chunk beside an edited one must also take the
	// overlay-aware path) but would tint chunks that merely neighbor an edit,
	// not own one.
	bool ChunkOwnsEditedBrick(const VoxelCoords::FVoxelChunkKey& ChunkKey) const;
	vxc::RaycastHit CastFromCamera(const FVector& CameraLoc, const FVector& Dir) const;
	void MaybeLogCounters(float DeltaTime);

	// Shared tail of TryDig/TryPlace/CarveSphere (edit-log authority path):
	// applies every brick's grouped edits, marks every dirty chunk for
	// re-mesh, and re-sorts the pending queues. No-op if EditsByBrick is
	// empty.
	void ApplyGroupedEdits(std::unordered_map<vxc::BrickKey, std::vector<vxc::EditCell>, vxc::BrickKeyHash>& EditsByBrick,
	                        const TSet<VoxelCoords::FVoxelChunkKey>& DirtyChunks);

	// --- M5 destruction (first slice, docs/voxel-earth-implementation-plan.md SS3.5) ---
	// public: called by the subsystem's SpawnTreeFixtureAt and the file-scope
	// PromoteDetachedIslands chop hook (same access shape as Voxels/log() reached
	// by BroadcastNewEntries).
public:
	// Writes Coords (world voxel-lattice) as solid Material through the exact
	// same edit-log authority path a dig/place uses (one World::applyEdit per
	// brick + dirty-chunk re-mesh). Used to stamp the stand-in tree TEST
	// FIXTURE (docs/m4-plan.md Round 2 -- NOT M4 vegetation). Returns the count
	// actually written. Authority side; caller replicates via BroadcastNewEntries.
	int32 StampVoxels(const TArray<VoxelCoords::FVoxelCoord>& Coords, uint8 Material);

	// After a solid-removing edit whose cleared voxels are ClearedVoxels, runs
	// vxc::findDisconnectedIslands over a bounded region around them (see the
	// .cpp for the region/anchor policy) and, for every detached island,
	// REMOVES its voxels from the authoritative grid via the edit-log path
	// (deterministic + replicated -- the authoritative half of the split). The
	// removed islands' world coords are returned in OutIslands so the caller
	// (subsystem, which has UWorld) can spawn the COSMETIC AVoxelDebris bodies.
	// bOutRegionClamped is set true if the region hit the size cap. Returns the
	// island count. No-op returning 0 if ClearedVoxels is empty.
	int32 DetectAndRemoveIslands(const TArray<VoxelCoords::FVoxelCoord>& ClearedVoxels,
	                             TArray<TArray<VoxelCoords::FVoxelCoord>>& OutIslands, bool& bOutRegionClamped);

	// M5 LARGE-EDIT structural collapse (docs/status.md "Structural collapse
	// (M5, large-edit)"; model + soundness argument in
	// voxel-core/include/voxelcore/collapse.h). This is the path taken when
	// DetectAndRemoveIslands' bounded voxel region CLAMPS -- i.e. exactly the
	// explosive-scale edits whose boundary-anchor policy it cannot reason
	// about, which previously did nothing at all and left impossible floating
	// geometry standing.
	//
	// Runs vxc::findCollapsingCells over a BRICK-resolution region (one coarse
	// cell == one 8-voxel/0.8m brick, so cell occupancy comes straight off the
	// edit overlay's brick bitset or the heightfield, with no per-voxel terrain
	// evaluation) twice: once against pre-edit occupancy (post-edit occupancy
	// plus the bricks the just-cleared voxels sat in) and once against
	// post-edit occupancy. Mass whose support THIS edit destroyed is removed
	// from the authoritative grid through the same edit-log path
	// DetectAndRemoveIslands uses, and the removed voxel sets come back in
	// OutPieces for the caller to hand to cosmetic AVoxelDebris bodies.
	// Returns the number of collapsed pieces. Authority side, game thread.
	int32 DetectAndRemoveCollapse(const TArray<VoxelCoords::FVoxelCoord>& ClearedVoxels,
	                              TArray<TArray<VoxelCoords::FVoxelCoord>>& OutPieces);
private:

	// --- Debug-tooling helpers (docs/debug-tooling-plan.md P1) ----------------
	void UpdatePerfSnapshot(float DeltaTime, float TickMs);
	void UpdateChunkStateTints();
	// M2 item 4 (docs/m2-plan.md): tints every loaded chunk by its ring
	// level instead of chunk state; see VoxelDebug::IsRingsEnabled.
	void UpdateRingTints();
	void DrawDebugBoundsLayer(UWorld& World, const FVector& Anchor) const;
};

// --- streaming tick orchestration -------------------------------------------

void FVoxelWorldImpl::TickStreaming(const FVector& Anchor, AActor& Owner, USceneComponent& Root, UMaterialInterface* Material, float DeltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_VoxelSubsystemTick);
	const double TickStartSeconds = FPlatformTime::Seconds();

	using namespace VoxelCoords;

	LastAnchorLocation = Anchor;
	ElapsedSeconds += DeltaTime;

	// Hitch attribution (docs/status.md "Perf-run hitches" isolation task):
	// reset this frame's counters up front; Dispatch/Drain* below fill them in
	// as they run, and the bottom of this function logs the breakdown iff this
	// frame overran the hitch threshold.
	ThisFrameAppliesFromWorker = 0;
	ThisFrameProxiesCreated = 0;
	ThisFrameEditRemeshes = 0;
	ThisFrameUnloads = 0;
	ThisFramePoolReuses = 0;
	ThisFrameRecomputeMs = 0.f;
	ThisFrameExitScanMs = 0.f;
	ThisFrameSortMs = 0.f;
	for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
	{
		ThisFrameLevelEntryMs[Level] = 0.f;
		ThisFrameLevelFootprints[Level] = 0;
	}

	// Recompute the desired set only when the anchor crosses into a new
	// render-chunk XY column (every ChunkEdgeUU = 3.2m of movement), not
	// every tick -- the ring-membership test itself (docs/m1-plan.md: "render
	// chunks (XY footprints) within 64m") is cheap per-candidate but touches
	// ~1300+ candidate footprints at a 64m/3.2m ratio, so gating it behind
	// anchor movement is what keeps this off the hot per-frame path.
	//
	// Underground streaming (see namespace VoxelUnderground) adds two more
	// triggers to that XY-crossing test, both cheap and both necessary: the
	// anchor crossing into a new chunk in Z WHILE UNDERGROUND (otherwise
	// digging or falling straight down never recomputes at all -- the deep box
	// would stay pinned where the player entered the ground), and the
	// underground flag itself flipping (which is what makes surfacing evict the
	// deep box, and entering the ground build it). Above ground the Z of the
	// anchor still does not matter, so flying keeps exactly its M1 recompute
	// cadence.
	const FVoxelChunkKey AnchorChunk = ChunkKeyForVoxel(WorldToVoxel(Anchor));
	const bool bUndergroundNow = EvaluateAnchorUnderground(Anchor);
	const bool bUndergroundChanged = bUndergroundNow != bAnchorUnderground;
	bAnchorUnderground = bUndergroundNow;

	// Bounded admission (see bAdmissionDeferredWork): a third trigger, for the
	// case the movement gates cannot see -- the queue has drained below a
	// quarter of its cap while the cap is still holding candidates back. That
	// only happens when the workers have caught up, which is exactly when more
	// work should be admitted, and it is the standing-still case in particular
	// (no movement trigger will ever fire). The per-level scan gate is cleared
	// for this call, since it would otherwise skip every level for the same
	// "anchor hasn't moved" reason.
	const int32 AdmissionCap = VoxelStreamAdmission::GetPendingJobCap();
	const bool bAdmissionRefill =
		bHasRecomputed && bAdmissionDeferredWork && AdmissionCap > 0 && PendingJobKeys.Num() * 4 < AdmissionCap;
	if (!bHasRecomputed || AnchorChunk.X != LastAnchorChunk.X || AnchorChunk.Y != LastAnchorChunk.Y ||
	    bUndergroundChanged || (bAnchorUnderground && AnchorChunk.Z != LastAnchorChunk.Z) || bAdmissionRefill)
	{
		if (bAdmissionRefill)
		{
			for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
			{
				bHasRecomputedLevel[Level] = false;
			}
		}
		RecomputeDesiredSet(Anchor);
		LastAnchorChunk = AnchorChunk;
		bHasRecomputed = true;
	}

	// Budgets: jobs in flight <=2xLogicalCores (docs/m1-plan.md Stage 2
	// decisions table); chunk component applies/unloads/edit-re-meshes are
	// now voxel.Stream.Max{Applies,Unloads,Remeshes}PerFrame cvars (default
	// 3/2/2, tightened from the original 8/4/4 -- see VoxelDebug.cpp's cvar
	// comments for the docs/status.md "Perf-run hitches" measurement this
	// tightening is based on). Edit re-meshes also cover first load of an
	// edited chunk -- see PendingGameThreadKeys comment above.
	{
		const double T0 = FPlatformTime::Seconds();
		DispatchJobs();
		const double T1 = FPlatformTime::Seconds();
		DrainResults(Owner, Root, Material);
		const double T2 = FPlatformTime::Seconds();
		DrainGameThreadMesh(Owner, Root, Material);
		const double T3 = FPlatformTime::Seconds();
		DrainUnloads();
		const double T4 = FPlatformTime::Seconds();

		// Hitch attribution timing (docs/status.md "Perf-run hitches"
		// isolation task): four cheap FPlatformTime::Seconds() calls (already
		// established practice in this function -- see TickStartSeconds
		// above), always collected so the per-frame log below has real
		// numbers the instant a hitch happens, not just from the next frame.
		ThisFrameDispatchMs = float((T1 - T0) * 1000.0);
		ThisFrameApplyMs = float((T2 - T1) * 1000.0);
		ThisFrameRemeshMs = float((T3 - T2) * 1000.0);
		ThisFrameUnloadMs = float((T4 - T3) * 1000.0);

		// Streaming re-measure: same four samples, also summed into the 5s
		// window so the periodic log can report where tick time goes on runs
		// that never cross the hitch threshold at all.
		AccumDispatchMs += (T1 - T0) * 1000.0;
		AccumApplyMs += (T2 - T1) * 1000.0;
		AccumRemeshMs += (T3 - T2) * 1000.0;
		AccumUnloadMs += (T4 - T3) * 1000.0;
		AccumRecomputeMs += ThisFrameRecomputeMs;
		++AccumTicks;
	}

	InFlightTasks.RemoveAllSwap([](const UE::Tasks::TTask<void>& T) { return T.IsCompleted(); }, EAllowShrinking::No);

	// docs/debug-tooling-plan.md P1 "Stats group": always-on DWORD counters
	// (STATS macros already compile to nothing in Shipping; no extra gate).
	SET_DWORD_STAT(STAT_VoxelChunksLoaded, ChunkRecords.Num());
	SET_DWORD_STAT(STAT_VoxelChunksInFlight, JobsInFlightCounter.GetValue());

	MaybeLogCounters(DeltaTime);

	// Wall-clock ms this streaming tick has spent so far (one more
	// FPlatformTime::Seconds() call -- negligible; matches the doctrine that
	// only the O(n) work below, not this measurement itself, needs the
	// debug-mode gate). Both UpdatePerfSnapshot (below) and the hitch
	// attribution log want it.
	const float TickMsSoFar = float((FPlatformTime::Seconds() - TickStartSeconds) * 1000.0);
	// Streaming re-measure: whole-tick cost into the same 5s window as the four
	// stage timers above (MaybeLogCounters has already run for this tick, so a
	// given window's ticks are all summed before the log that reports them --
	// consistent, not lagged, because the reset happens in the same call).
	AccumTickMs += TickMsSoFar;

	// Constraint: "keep all debug work zero-cost when voxel.Debug=0 (branch
	// out early)" -- FVoxelPerfSnapshot collection (array iteration, once/sec
	// sort) is real work beyond the always-on STATS macros above, so it's
	// gated on mode >= 1 (the perf HUD's own activation threshold) rather
	// than running unconditionally.
	if (VoxelDebug::GetDebugMode() >= 1)
	{
		UpdatePerfSnapshot(DeltaTime, TickMsSoFar);
	}

	// docs/debug-tooling-plan.md P1 / docs/status.md "Perf-run hitches"
	// isolation task: "when a frame exceeds the hitch threshold, log that
	// frame's breakdown" -- the diagnostic deliverable ("measure before
	// fixing"). Uses DeltaTime (the same per-frame sample
	// UVoxelPerfRunSubsystem sums into its own hitch count, via the same
	// VoxelDebug::kHitchThresholdMs) so the two never disagree about which
	// frames count as a hitch. Deliberately NOT gated behind voxel.Debug --
	// a real hitch in normal play is exactly when this diagnostic is most
	// wanted, and the cost when there is no hitch is one float compare.
	const float FrameMs = DeltaTime * 1000.f;
	if (FrameMs > 16.6f) // the actual 60fps gate bar (see TotalFramesOver60FpsBar)
	{
		++TotalFramesOver60FpsBar;
		++FramesOver60FpsBarSinceLog;
	}
	if (FrameMs > VoxelDebug::kHitchThresholdMs)
	{
		const float ElsewhereMs = FMath::Max(0.f, FrameMs - TickMsSoFar);
		// M1 gate attribution (docs/status.md M1 gate row): the engine's own
		// per-thread frame timers, set once/frame in FViewport::Draw (so they
		// describe the PREVIOUS frame -- a one-frame lag is immaterial for
		// correlating which thread a hitch lived on). This is what
		// disambiguates "elsewhereMs" between (a)/(b) render-thread scene
		// mutation (renderMs high), (d) GPU/RHI submission (rhiMs high), and a
		// pure game-thread stall on the render fence (gameWaitMs high, renderMs
		// low). Cycles -> ms via the platform's seconds-per-cycle.
		const double CyToMs = FPlatformTime::GetSecondsPerCycle() * 1000.0;
		const float RenderMs = float(GRenderThreadTime * CyToMs);
		const float RenderWaitMs = float(GRenderThreadWaitTime * CyToMs);
		const float RHIMs = float(GRHIThreadTime * CyToMs);
		const float GameWaitMs = float(GGameThreadWaitTime * CyToMs);
		UE_LOG(LogVoxelPerf, Warning,
		       TEXT("Hitch frame: frameMs=%.2f (threshold %.1f) | subsystemTickMs=%.2f elsewhereMs=%.2f | ")
		       TEXT("renderMs=%.2f renderWaitMs=%.2f rhiMs=%.2f gameWaitMs=%.2f | ")
		       TEXT("dispatchMs=%.2f applyMs=%.2f remeshMs=%.2f unloadMs=%.2f | ")
		       TEXT("componentsApplied=%d proxiesCreated=%d editRemeshes=%d unloads=%d poolReuses=%d poolSize=%d"),
		       FrameMs, VoxelDebug::kHitchThresholdMs, TickMsSoFar, ElsewhereMs, RenderMs, RenderWaitMs, RHIMs, GameWaitMs,
		       ThisFrameDispatchMs, ThisFrameApplyMs, ThisFrameRemeshMs, ThisFrameUnloadMs, ThisFrameAppliesFromWorker,
		       ThisFrameProxiesCreated, ThisFrameEditRemeshes, ThisFrameUnloads, ThisFramePoolReuses, ComponentPool.Num());

		// Recompute breakdown (M1 steady-state-hitch wave): the one stage of
		// TickStreaming the line above does not cover. Split out rather than
		// widened into that format string so the existing line stays greppable.
		UE_LOG(LogVoxelPerf, Warning,
		       TEXT("Hitch frame recompute: recomputeMs=%.2f exitScanMs=%.2f sortMs=%.2f | ")
		       TEXT("entryMs R0=%.2f R1=%.2f R2=%.2f R3=%.2f R4=%.2f | ")
		       TEXT("footprints R0=%d R1=%d R2=%d R3=%d R4=%d | tracked=%d"),
		       ThisFrameRecomputeMs, ThisFrameExitScanMs, ThisFrameSortMs, ThisFrameLevelEntryMs[0], ThisFrameLevelEntryMs[1],
		       ThisFrameLevelEntryMs[2], ThisFrameLevelEntryMs[3], ThisFrameLevelEntryMs[4], ThisFrameLevelFootprints[0],
		       ThisFrameLevelFootprints[1], ThisFrameLevelFootprints[2], ThisFrameLevelFootprints[3], ThisFrameLevelFootprints[4],
		       ChunkRecords.Num());
	}

	// Visualizations (mode 2 only; every helper early-outs on its own cvar
	// check so this call is a single cheap branch when debug is off). M2
	// item 4: voxel.Debug.Rings tints by mip level instead of chunk state --
	// the two layers share one MID per component (UVoxelChunkComponent::
	// SetDebugTint has no concept of "which layer"), so Rings takes priority
	// when both happen to be enabled at once rather than fighting over the
	// tint every tick.
	const bool bRingsNow = VoxelDebug::IsRingsEnabled();
	const bool bChunkStatesNow = VoxelDebug::IsChunkStatesEnabled();
	if (bRingsNow)
	{
		UpdateRingTints();
	}
	else if (bChunkStatesNow)
	{
		UpdateChunkStateTints();
	}
	if (!bRingsNow && !bChunkStatesNow && (bRingsWasEnabled || bChunkStatesWasEnabled))
	{
		// Off-transition (both layers now off): drop every MID once
		// (constraint: zero MID cost with the layer off) rather than paying
		// for one until the chunk happens to re-mesh naturally.
		for (auto& Pair : ChunkRecords)
		{
			if (UVoxelChunkComponent* Comp = Pair.Value.Component.Get())
			{
				Comp->ClearDebugTint();
			}
		}
	}
	bRingsWasEnabled = bRingsNow;
	bChunkStatesWasEnabled = bChunkStatesNow;

	if (UWorld* World = Owner.GetWorld())
	{
		DrawDebugBoundsLayer(*World, Anchor);
	}
}

void FVoxelWorldImpl::WaitForInFlightTasks()
{
	// Called from Deinitialize before Impl is torn down: worker jobs capture
	// raw pointers into Voxels.generated() and the results queue (see
	// DispatchJobs) rather than a ref-counted handle, on the assumption that
	// no job outlives Impl. This is what makes that assumption true.
	for (UE::Tasks::TTask<void>& Task : InFlightTasks)
	{
		Task.Wait();
	}
	InFlightTasks.Empty();
}

void FVoxelWorldImpl::MaybeLogCounters(float DeltaTime)
{
	LogTimerAccumSeconds += DeltaTime;
	if (LogTimerAccumSeconds < 5.0f)
	{
		return;
	}
	LogTimerAccumSeconds = 0.f;

	// docs/debug-tooling-plan.md P1 "Log split": periodic counter line moved
	// off LogVoxelEarth (module/general) onto LogVoxelPerf.
	// Underground streaming: how many tracked chunks exist only because of the
	// anchor-relative deep box, and how many of those actually produced visible
	// geometry. A full solid interior chunk meshes to ZERO quads and therefore
	// holds no component and no GPU memory (see ApplyMeshResult's Quads.Num()==0
	// branch), so this pair is what tells us whether depth is costing RAM or
	// merely bookkeeping + worker throughput. An O(tracked) scan at the periodic
	// 5s log cadence only; never per-frame.
	int32 DeepTracked = 0;
	int32 DeepWithGeometry = 0;
	for (const auto& Pair : ChunkRecords)
	{
		if (Pair.Value.bDeepAnchorRelative)
		{
			++DeepTracked;
			DeepWithGeometry += (Pair.Value.LastQuadCount > 0) ? 1 : 0;
		}
	}

	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel streaming: loaded=%lld unloaded=%lld quads=%lld tracked=%d jobsInFlight=%d pendingJobs=%d ")
	       TEXT("pendingGameThread=%d pendingUnload=%d underground=%d deepTracked=%d deepWithGeometry=%d residentQuads=%lld"),
	       (long long)TotalChunksLoaded, (long long)TotalChunksUnloaded, (long long)TotalQuadsLoaded, ChunkRecords.Num(),
	       JobsInFlightCounter.GetValue(), PendingJobKeys.Num(), PendingGameThreadKeys.Num(), PendingUnloadKeys.Num(),
	       bAnchorUnderground ? 1 : 0, DeepTracked, DeepWithGeometry, (long long)ResidentQuads);

	// M2 item 1: "Per-level loaded/pending counters into the perf snapshot/
	// HUD" -- also into this periodic log line, so a headless run's log file
	// alone (no HUD to screenshot) is enough to verify every ring level is
	// actually loading chunks.
	int32 LevelLoaded[VoxelCoords::kNumLevels] = {};
	int32 LevelPending[VoxelCoords::kNumLevels] = {};
	for (const auto& Pair : ChunkRecords)
	{
		if (Pair.Value.Component.IsValid())
		{
			++LevelLoaded[FMath::Clamp(Pair.Key.Level, 0, VoxelCoords::kNumLevels - 1)];
		}
	}
	for (const VoxelCoords::FVoxelLevelChunkKey& K : PendingJobKeys)
	{
		++LevelPending[FMath::Clamp(K.Level, 0, VoxelCoords::kNumLevels - 1)];
	}
	for (const VoxelCoords::FVoxelLevelChunkKey& K : PendingGameThreadKeys)
	{
		++LevelPending[FMath::Clamp(K.Level, 0, VoxelCoords::kNumLevels - 1)];
	}
	UE_LOG(LogVoxelPerf, Log, TEXT("Voxel rings: R0 loaded=%d pending=%d | R1 loaded=%d pending=%d | R2 loaded=%d pending=%d | ")
	                          TEXT("R3 loaded=%d pending=%d | R4 loaded=%d pending=%d"),
	       LevelLoaded[0], LevelPending[0], LevelLoaded[1], LevelPending[1], LevelLoaded[2], LevelPending[2], LevelLoaded[3],
	       LevelPending[3], LevelLoaded[4], LevelPending[4]);

	// R2-R4 ring starvation wave: the dispatch side of the same picture. Read
	// with the line above, "loaded=0 pending=4000 dispatched=0" (starved: no
	// worker ever ran) is now distinguishable from "loaded=0 dispatched=4000
	// zeroQuad=4000" (served, but every chunk was buried rock). disp= is this
	// 5s window, total= and load= are cumulative over the whole run.
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel ring dispatch: R0 disp=%lld total=%lld load=%lld zq=%lld | R1 disp=%lld total=%lld load=%lld zq=%lld | ")
	       TEXT("R2 disp=%lld total=%lld load=%lld zq=%lld | R3 disp=%lld total=%lld load=%lld zq=%lld | ")
	       TEXT("R4 disp=%lld total=%lld load=%lld zq=%lld"),
	       (long long)LevelJobsDispatchedSinceLog[0], (long long)LevelJobsDispatchedTotal[0],
	       (long long)LevelChunksLoadedTotal[0], (long long)LevelZeroQuadTotal[0],
	       (long long)LevelJobsDispatchedSinceLog[1], (long long)LevelJobsDispatchedTotal[1],
	       (long long)LevelChunksLoadedTotal[1], (long long)LevelZeroQuadTotal[1],
	       (long long)LevelJobsDispatchedSinceLog[2], (long long)LevelJobsDispatchedTotal[2],
	       (long long)LevelChunksLoadedTotal[2], (long long)LevelZeroQuadTotal[2],
	       (long long)LevelJobsDispatchedSinceLog[3], (long long)LevelJobsDispatchedTotal[3],
	       (long long)LevelChunksLoadedTotal[3], (long long)LevelZeroQuadTotal[3],
	       (long long)LevelJobsDispatchedSinceLog[4], (long long)LevelJobsDispatchedTotal[4],
	       (long long)LevelChunksLoadedTotal[4], (long long)LevelZeroQuadTotal[4]);
	for (int64& V : LevelJobsDispatchedSinceLog)
	{
		V = 0;
	}

	// M2 wave 2 item 1 ("Cross-job mip caching"): per-level worker mesh-job
	// ms (rolling-window p50/p95, LastPerfSnapshot -- refreshed at 1Hz
	// whenever voxel.Debug >= 1, which -VoxelPerfRun forces on) plus the
	// shared cross-job cache's memory footprint. This is the log-only
	// evidence for the "report worker p50/p95 per level" measurement (a
	// -VoxelPerfRun run has no HUD to screenshot).
	const FVoxelPerfSnapshot& Snap = LastPerfSnapshot;
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel worker ms/level: R0 p50=%.2f p95=%.2f | R1 p50=%.2f p95=%.2f | R2 p50=%.2f p95=%.2f | ")
	       TEXT("R3 p50=%.2f p95=%.2f | R4 p50=%.2f p95=%.2f | mipCache bricks=%lld bytes=%lld evictions=%lld"),
	       Snap.LevelWorkerMsP50[0], Snap.LevelWorkerMsP95[0], Snap.LevelWorkerMsP50[1], Snap.LevelWorkerMsP95[1],
	       Snap.LevelWorkerMsP50[2], Snap.LevelWorkerMsP95[2], Snap.LevelWorkerMsP50[3], Snap.LevelWorkerMsP95[3],
	       Snap.LevelWorkerMsP50[4], Snap.LevelWorkerMsP95[4], (long long)Snap.MipCacheBrickCount, (long long)Snap.MipCacheBytes,
	       (long long)Snap.MipCacheEvictions);

	// M1 steady-state-hitch wave: worst RecomputeDesiredSet cost seen since the
	// last periodic log, per level. Independent of the hitch log (a burst that
	// lands on a frame under the 33.3ms threshold is invisible there), so this
	// is what quantifies "what does R3/R4 recompute cost when it fires".
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel recompute (max since last log): totalMs=%.2f exitScanMs=%.2f sortMs=%.2f calls=%d | ")
	       TEXT("entryMs R0=%.2f R1=%.2f R2=%.2f R3=%.2f R4=%.2f | scans R0=%d R1=%d R2=%d R3=%d R4=%d | ")
	       TEXT("tracked=%d pendingJob=%d pendingGT=%d pendingUnload=%d | framesOver16.6ms=%d (total %lld)"),
	       MaxRecomputeMs, MaxExitScanMs, MaxSortMs, RecomputeCalls, MaxLevelEntryMs[0], MaxLevelEntryMs[1], MaxLevelEntryMs[2],
	       MaxLevelEntryMs[3], MaxLevelEntryMs[4], LevelEntryScans[0], LevelEntryScans[1], LevelEntryScans[2], LevelEntryScans[3],
	       LevelEntryScans[4], ChunkRecords.Num(), PendingJobKeys.Num(), PendingGameThreadKeys.Num(), PendingUnloadKeys.Num(),
	       FramesOver60FpsBarSinceLog, (long long)TotalFramesOver60FpsBar);
	// Streaming pipeline re-measure (docs/status.md "Streaming pipeline
	// re-measure + rework"): the two questions the existing logs could not
	// answer. (1) Where does per-tick time go -- the hitch log breaks a tick
	// down but only fires above 33.3ms, so a clean run reports nothing at all;
	// these are the same four stage timers summed over the whole 5s window,
	// which is what makes "streaming costs X% of the frame" answerable. (2) How
	// much worker output is actually USED -- dispatched vs drained vs discarded
	// as stale vs meshed-to-zero-quads. `loaded` in the line above counts only
	// component creations, so it understates worker throughput by every buried
	// chunk and overstates efficiency by every wasted job.
	// The window is the log cadence itself (LogTimerAccumSeconds has already
	// been reset to 0 above), i.e. 5s plus at most one frame.
	constexpr double WindowMs = 5000.0;
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel tick budget (5s window): ticks=%d tickMs=%.1f (%.2f%% of wall) | recompute=%.1f dispatch=%.1f ")
	       TEXT("apply=%.1f remesh=%.1f unload=%.1f | perTick tick=%.3f recompute=%.3f"),
	       AccumTicks, AccumTickMs, WindowMs > 0.0 ? 100.0 * AccumTickMs / WindowMs : 0.0, AccumRecomputeMs, AccumDispatchMs,
	       AccumApplyMs, AccumRemeshMs, AccumUnloadMs, AccumTicks > 0 ? AccumTickMs / AccumTicks : 0.0,
	       AccumTicks > 0 ? AccumRecomputeMs / AccumTicks : 0.0);
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel job flow (5s window): dispatched=%lld drained=%lld stale=%lld (%.1f%%) zeroQuad=%lld ")
	       TEXT("recordsAdded=%lld recordsEvicted=%lld candidatesRejected=%lld"),
	       (long long)JobsDispatchedSinceLog, (long long)ResultsDrainedSinceLog, (long long)StaleDiscardsSinceLog,
	       ResultsDrainedSinceLog > 0 ? 100.0 * double(StaleDiscardsSinceLog) / double(ResultsDrainedSinceLog) : 0.0,
	       (long long)ZeroQuadAppliesSinceLog, (long long)RecordsAddedSinceLog, (long long)RecordsEvictedSinceLog,
	       (long long)CandidatesRejectedSinceLog);
	UE_LOG(LogVoxelPerf, Log, TEXT("Voxel admission (5s window): cap=%d cutoffM=%.0f rejected=%lld dropped=%lld"),
	       VoxelStreamAdmission::GetPendingJobCap(),
	       AdmissionCutoffDistSq < DBL_MAX ? FMath::Sqrt(AdmissionCutoffDistSq) / 100.0 : -1.0,
	       (long long)CandidatesRejectedSinceLog, (long long)RecordsDroppedSinceLog);
	AccumDispatchMs = AccumApplyMs = AccumRemeshMs = AccumUnloadMs = AccumRecomputeMs = AccumTickMs = 0.0;
	AccumTicks = 0;
	JobsDispatchedSinceLog = ResultsDrainedSinceLog = StaleDiscardsSinceLog = ZeroQuadAppliesSinceLog = 0;
	RecordsAddedSinceLog = RecordsEvictedSinceLog = CandidatesRejectedSinceLog = RecordsDroppedSinceLog = 0;

	MaxRecomputeMs = 0.f;
	MaxExitScanMs = 0.f;
	MaxSortMs = 0.f;
	RecomputeCalls = 0;
	FramesOver60FpsBarSinceLog = 0;
	for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
	{
		MaxLevelEntryMs[Level] = 0.f;
		LevelEntryScans[Level] = 0;
	}

	// Track B2 missing-tile telemetry: only meaningful once a real tile grid
	// is in use (bUsingTileGrid) -- the synthetic sampler has no concept of a
	// "missing" tile, so this is silent (no log spam) for today's default
	// path. See GridTiles' doc comment above for why this reads through an
	// implicit conversion into a plain uint64 local rather than holding a
	// reference to the counter.
	if (bUsingTileGrid && GridTiles)
	{
		const uint64 MissingNow = GridTiles->missingTileQueries;
		if (MissingNow > LastMissingTileQueries)
		{
			UE_LOG(LogVoxelPerf, Warning,
			       TEXT("Voxel tile grid: +%llu missing-tile queries since last log (total %llu) -- player/clipmap sampling ")
			       TEXT("beyond loaded tile radius -- terrain is deterministic flat sea-level fallback there."),
			       (unsigned long long)(MissingNow - LastMissingTileQueries), (unsigned long long)MissingNow);
		}
		LastMissingTileQueries = MissingNow;
	}
}

// --- underground streaming policy --------------------------------------------
//
// docs/status.md "Underground streaming (vertical footprint)". Before this,
// ComputeFootprintChunkZRange below was the ONLY thing deciding vertical
// extent, and it is keyed purely on the terrain SURFACE at (Level, X, Y): a
// level-0 chunk is 3.2m tall and it returns surfaceChunkZ-1, so the world was
// meshed only 3.2-6.4m deep everywhere. A camera 9m down rendered open sky.
//
// Two additions, both applied OUTSIDE the FootprintZRangeCache memo so that
// memo keeps its correctness argument intact (it caches a pure function of
// (Level, X, Y) and the amplifier; nothing anchor-dependent may go inside it):
//
//  1. DEPTH SKIRT -- unconditional, anchor-Z-independent. Extra chunks of
//     meshed rock below the surface band, shrinking with horizontal distance,
//     so a shaft dug from the surface has walls and a floor instead of opening
//     into nothing. Because it is a pure function of the footprint, skirt
//     chunks need no new eviction rule: they leave with their footprint.
//
//  2. ANCHOR-RELATIVE DEEP BOX -- only while the anchor is underground. A
//     vertical band centred on the anchor's own chunk Z, again shrinking with
//     horizontal distance, so being underground streams chunks around you in
//     all directions rather than a surface-relative crust far overhead.
//
// Depth budget. Cost is measured in chunks-per-footprint, and every ring holds
// roughly the same footprint count by construction (~940: each ring doubles
// both its radius and its chunk edge), so "+N chunks of depth on ring R" costs
// ~940*N chunks there regardless of R. The budget therefore shrinks with
// horizontal distance, expressed as a fraction of the ring's OWN outer radius:
//
//   - Level 0's annulus is [0, 64m), so it is the only ring whose footprints
//     can land in the <0.25 and <0.5 bands. Levels 1-4 have Inner == Outer/2
//     (see RingPresets), so their fraction is always >= 0.5 and they get the
//     far-band budget -- i.e. R2/R3/R4 keep their pre-change vertical extent
//     EXACTLY. That is deliberate: an R3/R4 chunk is already 25.6m/51.2m tall,
//     so its existing -1 chunk of margin is already tens of metres of depth,
//     and deep columns out at 256-1024m are invisible rock.
//   - The deep box additionally stops at level 1. Beyond ~128m horizontally
//     there is no line of sight underground that a deeper column would serve.
//
// Nothing here touches worldgen, the edit log, or chunk CONTENTS -- only which
// chunk keys enter the desired set.
namespace VoxelUnderground
{
// Master switch, for A/B measurement against the M1 gate without a rebuild.
// Turning it off mid-run stops NEW deep chunks entering the desired set; the
// exit pass's vertical keep-test is deliberately NOT gated on it, so already-
// resident deep chunks still drain out through the normal unload path.
static int32 GEnabled = 1;
static FAutoConsoleVariableRef CVarUndergroundEnabled(
	TEXT("voxel.Stream.Underground"),
	GEnabled,
	TEXT("1 (default): extend the streaming footprint below the terrain surface (depth skirt + underground anchor box). 0: pre-M4 surface-band-only behaviour."),
	ECVF_Default);

// -VoxelNoUnderground: the same off-switch as the cvar, but readable BEFORE
// the first RecomputeDesiredSet. This matters for A/B measurement: -ExecCmds
// cvars are applied after the world has already begun streaming, and by then
// the first recomputes have added deep chunks that nothing subsequently
// evicts (skirt chunks are pure functions of their footprint and live as long
// as it does), so an -ExecCmds A/B silently measures the SAME desired set
// twice. Same "don't depend on -ExecCmds cvar-parsing timing" reasoning the
// GameMode already applies to -VoxelGIOn / -VoxelMipCacheBudgetMB.
static bool UndergroundDisabled()
{
	static const bool bDisabledOnCommandLine = FParse::Param(FCommandLine::Get(), TEXT("VoxelNoUnderground"));
	return bDisabledOnCommandLine || GEnabled == 0;
}

// Ring-fraction band edges (fraction of the level's own OuterMeters).
static constexpr double NearFrac = 0.25;
static constexpr double MidFrac = 0.50;

// (1) Depth skirt: EXTRA level-L chunks below ComputeFootprintChunkZRange's
// ChunkZMin, per band. At level 0 (3.2m chunks) that is ~41.6m / ~16.0m /
// unchanged-3.2m of guaranteed rock under the surface at <16m / <32m / >=32m
// from the anchor.
//
// The near-band figure is set by the M4 CAVE PASS rather than by digging.
// voxelcore/caves.h places tunnel axes kCaveNodeDepthMinMm..+SpanMm =
// 9m..34m below the surface, with radius up to kCaveRadiusMaxMm = 2.8m, so
// the deepest cave voxel sits ~36.8m down (the header's own static_assert
// bounds it under 40m). 12 level-0 chunks = 38.4m clears that whole band, so
// a cave -- and a sinkhole shaft leading into one -- is meshed BEFORE the
// player is underground, which is what makes an entrance visible from
// outside it rather than popping in once you have already fallen through.
// It also covers digging by a wide margin (MaxCubeSizeVoxels = 4 = 0.4m).
static constexpr int32 SkirtChunksNear = 12;
static constexpr int32 SkirtChunksMid = 5;
static constexpr int32 SkirtChunksFar = 0;

// (2) Underground deep box: vertical RADIUS in level-L chunks around the
// anchor's chunk Z, per band. Radius 0 still means one chunk layer (the
// anchor's own). Level 0 near-band 8 => +-25.6m, which overlaps the 19.2m
// skirt above it, so a shaft up to ~45m deep stays vertically CONTIGUOUS
// (skirt bottom meets box top) with no unmeshed gap in its walls.
static constexpr int32 BoxRadiusChunksL0[3] = {8, 4, 0}; // near / mid / far
static constexpr int32 BoxRadiusChunksL1 = 0;            // whole ring (fraction is always >= 0.5 there)

// Vertical keep-distance for the deep box, in level-L chunks, used by the
// exit scan. Mirrors the XY hysteresis: keep out to KeepChunks * chunkEdge *
// UnloadRingMultiplier. Generous relative to the near-band radius that created
// them (8 -> 9) so that a player moving HORIZONTALLY between bands does not
// thrash chunks in and out; surfacing still evicts the whole box, because the
// anchor's Z leaves the keep window entirely.
static constexpr int32 KeepChunks[VoxelCoords::kNumLevels] = {9, 2, 0, 0, 0};

// The anchor counts as underground once it is this far below the amplifier
// surface, and stops counting as underground only once it comes back above the
// (smaller) exit depth -- plain hysteresis so standing at the lip of a pit
// cannot flap the whole desired set. Enter depth is well clear of the ~1m a
// pawn's actor origin floats above the ground it is standing on.
static constexpr double EnterDepthUU = 200.0; // 2.0m
static constexpr double ExitDepthUU = 100.0;  // 1.0m

// Which band a footprint at squared XY distance DistSq falls in (0 near,
// 1 mid, 2 far), against the level's own outer radius.
static int32 BandForDistance(int32 Level, double DistSq)
{
	const double OuterUU = UVoxelWorldSubsystem::RingPresets[Level].OuterMeters * 100.0;
	if (DistSq < FMath::Square(OuterUU * NearFrac))
	{
		return 0;
	}
	if (DistSq < FMath::Square(OuterUU * MidFrac))
	{
		return 1;
	}
	return 2;
}

static int32 SkirtDepthChunks(int32 Level, double DistSq)
{
	if (UndergroundDisabled())
	{
		return 0;
	}
	switch (BandForDistance(Level, DistSq))
	{
	case 0: return SkirtChunksNear;
	case 1: return SkirtChunksMid;
	default: return SkirtChunksFar;
	}
}

// Returns false if this level gets no deep box at all (levels 2-4).
static bool BoxRadiusChunks(int32 Level, double DistSq, int32& OutRadius)
{
	if (UndergroundDisabled())
	{
		return false;
	}
	if (Level == 0)
	{
		OutRadius = BoxRadiusChunksL0[BandForDistance(0, DistSq)];
		return true;
	}
	if (Level == 1)
	{
		OutRadius = BoxRadiusChunksL1;
		return true;
	}
	return false;
}
} // namespace VoxelUnderground

// --- desired-set / hysteresis ------------------------------------------------

void FVoxelWorldImpl::ComputeFootprintChunkZRange(int32 ChunkX, int32 ChunkY, int32 Level, int32& OutChunkZMin, int32& OutChunkZMax) const
{
	using namespace VoxelCoords;

	// Cheap proxy for stage 1's exhaustive per-brick surfaceBrickRange (which
	// would cost B*B amplifier samples per candidate -- prohibitive across
	// ~1300+ candidate footprints per recompute): sample the amplifier
	// column at this footprint's 4 corners and take a +-1 render-chunk
	// margin around the resulting elevation range as slope safety. Generous
	// on purpose; the Z-extent algorithm isn't pinned by the decisions table
	// (only the XY radius test is), and this keeps recompute cost bounded.
	//
	// M2: the amplifier always operates in LEVEL-0 (10cm) voxel units, so a
	// level-L footprint's corners are converted up to level-0 voxel units
	// before querying it, and the resulting level-0 top-voxel range is
	// converted back down to level-L chunk units afterward.
	const int64 LevelScale = int64(1) << Level;
	const int64 Vx0 = (int64(ChunkX) * ChunkEdgeVoxels) * LevelScale;
	const int64 Vx1 = Vx0 + ChunkEdgeVoxels * LevelScale - 1;
	const int64 Vy0 = (int64(ChunkY) * ChunkEdgeVoxels) * LevelScale;
	const int64 Vy1 = Vy0 + ChunkEdgeVoxels * LevelScale - 1;

	int64 TopVoxelMin = INT64_MAX, TopVoxelMax = INT64_MIN;
	const int64 CornersX[2] = {Vx0, Vx1};
	const int64 CornersY[2] = {Vy0, Vy1};
	for (int64 Cx : CornersX)
	{
		for (int64 Cy : CornersY)
		{
			const vxc::ColumnSample Col = Voxels.amplifier().column(Cx, Cy);
			const int64 Top = vxc::floorDiv(Col.surfaceMm, vxc::kVoxelSizeMm);
			TopVoxelMin = FMath::Min(TopVoxelMin, Top);
			TopVoxelMax = FMath::Max(TopVoxelMax, Top);
		}
	}

	// Corner-only sampling under-estimates interior extremes; the amplifier's
	// slope-scaled detail can add metres between corners. Missing chunks
	// BELOW the range are invisible (buried), so -1 suffices; missing chunks
	// ABOVE the range are holes in peaks, so take +2 headroom (in level-L
	// chunks -- generous by construction, since a level-L chunk is (1<<L)
	// times taller than a level-0 one). The exact fix (workers compute their
	// own z-range per footprint) is a stage 3 refactor.
	// Qualified: VoxelLightField.cpp declares its own anonymous-namespace
	// FloorDiv, and in a unity blob that contains both files the unqualified
	// name is ambiguous against `using namespace VoxelCoords`.
	const int64 TopVoxelMinAtLevel = VoxelCoords::FloorDiv(TopVoxelMin, LevelScale);
	const int64 TopVoxelMaxAtLevel = VoxelCoords::FloorDiv(TopVoxelMax, LevelScale);
	OutChunkZMin = (int32)VoxelCoords::FloorDiv(TopVoxelMinAtLevel, (int64)ChunkEdgeVoxels) - 1;
	OutChunkZMax = (int32)VoxelCoords::FloorDiv(TopVoxelMaxAtLevel, (int64)ChunkEdgeVoxels) + 2;
}

void FVoxelWorldImpl::FootprintChunkZRangeCached(int32 ChunkX, int32 ChunkY, int32 Level, int32& OutChunkZMin,
                                                 int32& OutChunkZMax) const
{
	const VoxelCoords::FVoxelLevelChunkKey CacheKey{Level, VoxelCoords::FVoxelChunkKey{ChunkX, ChunkY, 0}};
	if (const FFootprintZRange* Hit = FootprintZRangeCache.Find(CacheKey))
	{
		OutChunkZMin = Hit->ChunkZMin;
		OutChunkZMax = Hit->ChunkZMax;
		return;
	}
	ComputeFootprintChunkZRange(ChunkX, ChunkY, Level, OutChunkZMin, OutChunkZMax);
	FootprintZRangeCache.Add(CacheKey, FFootprintZRange{OutChunkZMin, OutChunkZMax});
}

void FVoxelWorldImpl::PruneFootprintZRangeCache(const FVector& Anchor)
{
	if (FootprintZRangeCache.Num() <= FootprintZRangeCacheMaxEntries)
	{
		return;
	}

	// Drop everything outside twice its level's unload ring: those footprints
	// cannot re-enter the annulus without a long journey back, and re-sampling
	// them then costs the same as it did the first time. Cheap relative to what
	// it protects (a pointer-chasing pass over the map, no amplifier sampling).
	for (auto It = FootprintZRangeCache.CreateIterator(); It; ++It)
	{
		const VoxelCoords::FVoxelLevelChunkKey& Key = It.Key();
		const UVoxelWorldSubsystem::FRingPreset& Preset = UVoxelWorldSubsystem::RingPresets[Key.Level];
		const double ChunkEdge = VoxelCoords::ChunkEdgeUUForLevel(Key.Level);
		const double CenterX = (double(Key.Key.X) + 0.5) * ChunkEdge;
		const double CenterY = (double(Key.Key.Y) + 0.5) * ChunkEdge;
		const double DistSq = FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y);
		const double KeepRadiusUU = Preset.OuterMeters * 100.0 * UVoxelWorldSubsystem::UnloadRingMultiplier * 2.0;
		if (DistSq > FMath::Square(KeepRadiusUU))
		{
			It.RemoveCurrent();
		}
	}
}

bool FVoxelWorldImpl::ChunkHasEditedBrick(const VoxelCoords::FVoxelChunkKey& ChunkKey) const
{
	// docs/m1-plan.md Stage 2 decisions table: "Chunks that contain ANY
	// edited brick (query World's editedBricks map by chunk range) are never
	// meshed by workers." The scan extends 1 brick beyond the chunk on every
	// axis because the mesher's apron reads 1 voxel across the chunk border:
	// a pristine chunk NEXT TO an edited brick must also take the
	// overlay-aware path, or its border faces cull against pre-edit data
	// (visible as a seam after unload/reload of a neighbor-of-edit chunk).
	constexpr int32 BricksPerChunk = VoxelCoords::ChunkEdgeBricks;
	const vxc::ChunkMap<VoxelCoords::BrickEdgeVoxels>& Overlay = Voxels.editedBricks();
	if (Overlay.size() == 0)
	{
		return false; // fast path: nothing has ever been edited
	}
	for (int32 Dz = -1; Dz <= BricksPerChunk; ++Dz)
	{
		for (int32 Dy = -1; Dy <= BricksPerChunk; ++Dy)
		{
			for (int32 Dx = -1; Dx <= BricksPerChunk; ++Dx)
			{
				const vxc::BrickKey Key{ChunkKey.X * BricksPerChunk + Dx, ChunkKey.Y * BricksPerChunk + Dy,
				                         ChunkKey.Z * BricksPerChunk + Dz};
				if (Overlay.find(Key) != nullptr)
				{
					return true;
				}
			}
		}
	}
	return false;
}

bool FVoxelWorldImpl::ChunkOwnsEditedBrick(const VoxelCoords::FVoxelChunkKey& ChunkKey) const
{
	// docs/debug-tooling-plan.md P1 chunk-state tint: "edited-chunk orange
	// (persistent while it has overlay bricks)" -- unlike ChunkHasEditedBrick
	// (mesh-routing correctness, needs the 1-brick border), this is purely a
	// tint decision, so it only asks whether THIS chunk owns an edited brick.
	constexpr int32 BricksPerChunk = VoxelCoords::ChunkEdgeBricks;
	const vxc::ChunkMap<VoxelCoords::BrickEdgeVoxels>& Overlay = Voxels.editedBricks();
	if (Overlay.size() == 0)
	{
		return false;
	}
	for (int32 Dz = 0; Dz < BricksPerChunk; ++Dz)
	{
		for (int32 Dy = 0; Dy < BricksPerChunk; ++Dy)
		{
			for (int32 Dx = 0; Dx < BricksPerChunk; ++Dx)
			{
				const vxc::BrickKey Key{ChunkKey.X * BricksPerChunk + Dx, ChunkKey.Y * BricksPerChunk + Dy,
				                         ChunkKey.Z * BricksPerChunk + Dz};
				if (Overlay.find(Key) != nullptr)
				{
					return true;
				}
			}
		}
	}
	return false;
}

void FVoxelWorldImpl::SortPendingQueues(const FVector& Anchor)
{
	const auto DistSq = [&Anchor](const VoxelCoords::FVoxelLevelChunkKey& LevelKey)
	{
		const double ChunkEdge = VoxelCoords::ChunkEdgeUUForLevel(LevelKey.Level);
		const double CenterX = (double(LevelKey.Key.X) + 0.5) * ChunkEdge;
		const double CenterY = (double(LevelKey.Key.Y) + 0.5) * ChunkEdge;
		const double CenterZ = (double(LevelKey.Key.Z) + 0.5) * ChunkEdge;
		return FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y) + FMath::Square(CenterZ - Anchor.Z);
	};
	// docs/m2-plan.md item 1: "Budgets shared across levels, nearest-first
	// within level, lower level (finer) wins priority at equal distance."
	// Distance is the PRIMARY key -- "nearest-first" is the dominant clause,
	// and "lower level wins ... at equal distance" is explicitly a tie-break
	// ("at equal distance"), not an override. Level-as-primary-key was tried
	// first and measured wrong: it dispatches every level-0 job before a
	// single level-1 job, every level-1 before level-2, etc., so outer rings
	// never get a turn until the (much larger, in absolute chunk count)
	// inner rings fully drain -- R3/R4 stayed at 0 loaded chunks through a
	// 90s verification run despite thousands queued. Distance-primary lets
	// every ring populate outward from the camera roughly together, which is
	// both the visually-correct LOD behavior and the literal reading of the
	// decisions-table wording. Sort() wants a "less than" predicate
	// producing ascending order, and Pop() removes from the back, so the
	// LOWEST-priority element must sort first (front) and the
	// HIGHEST-priority element (nearest, level 0 on an exact tie) must sort
	// last (back) -- this predicate returns true when A has lower priority
	// than B.
	// Decorate-sort-undecorate. The obvious form -- Sort() with a predicate that
	// calls DistSq on both operands -- recomputes the distance on every
	// COMPARISON, i.e. ~2*n*log2(n) times (measured: 3.2-3.5ms per call on a
	// ~19k-deep pending queue, ~8 times a second, see docs/status.md "R3/R4
	// recompute amortization"). Computing each key exactly once instead makes it
	// n distance computes plus a sort over cached doubles. The ordering is
	// bit-identical: same primary key (distance, descending), same tie-break
	// (higher level first), and Sort() was already unstable, so elements equal
	// under BOTH keys were never in a defined order to begin with.
	const auto SortQueue = [&DistSq](TArray<VoxelCoords::FVoxelLevelChunkKey>& Queue, TArray<FSortEntry>& Scratch)
	{
		Scratch.Reset(Queue.Num());
		for (const VoxelCoords::FVoxelLevelChunkKey& Key : Queue)
		{
			Scratch.Add(FSortEntry{DistSq(Key), Key});
		}
		Scratch.Sort(
		    [](const FSortEntry& A, const FSortEntry& B)
		    {
			    if (A.DistSq != B.DistSq)
			    {
				    return A.DistSq > B.DistSq; // farther = lower priority = sorts toward the front
			    }
			    return A.Key.Level > B.Key.Level; // exact-distance tie: higher level number = lower priority
		    });
		for (int32 Index = 0; Index < Scratch.Num(); ++Index)
		{
			Queue[Index] = Scratch[Index].Key;
		}
	};
	// Member scratch buffers (not locals): this runs ~8 times a second with
	// ~19k elements, and reusing the allocation keeps it off the heap churn.
	SortQueue(PendingJobKeys, SortScratchJob);
	SortQueue(PendingGameThreadKeys, SortScratchGameThread);
}

bool FVoxelWorldImpl::EvaluateAnchorUnderground(const FVector& Anchor) const
{
	// One amplifier column per TickStreaming call -- the same query
	// UVoxelWorldSubsystem::GetSurfaceHeightUU makes, inlined here because this
	// runs on the streaming path and must not depend on the outer subsystem.
	// Deliberately the GENERATED surface, not an overlay-aware one: standing in
	// a dug-out chamber whose roof is edited-away should still read as
	// underground by elevation, and the overlay is not a height field anyway.
	const int64 Vx = (int64)FMath::FloorToDouble(Anchor.X / VoxelCoords::VoxelSizeUU);
	const int64 Vy = (int64)FMath::FloorToDouble(Anchor.Y / VoxelCoords::VoxelSizeUU);
	const double SurfaceUU = double(Voxels.amplifier().column(Vx, Vy).surfaceMm) / 10.0;
	const double DepthUU = SurfaceUU - Anchor.Z;
	// Hysteresis: deeper than EnterDepthUU to become underground, shallower
	// than ExitDepthUU to stop being underground.
	return bAnchorUnderground ? (DepthUU > VoxelUnderground::ExitDepthUU)
	                          : (DepthUU > VoxelUnderground::EnterDepthUU);
}

void FVoxelWorldImpl::RecomputeDesiredSet(const FVector& Anchor)
{
	using namespace VoxelCoords;

	// Stage timing (see the ThisFrameRecomputeMs member's comment): always
	// collected, a handful of FPlatformTime::Seconds() calls on a path that
	// only runs on an anchor chunk crossing, never per-frame.
	const double RecomputeT0 = FPlatformTime::Seconds();
	EvictedThisCall.Reset();
	LevelsScannedThisCall = 0;
	CandidatesRejectedThisCall = 0;

	// Bounded admission: the cutoff was computed at the end of the PREVIOUS
	// call, when the queue was at cap; workers have been draining it since.
	// Relax it only once the queue has drained MEANINGFULLY (below 3/4 cap),
	// not on the handful of slots one inter-recompute interval frees: relaxing
	// on every call would re-admit (and re-drop) the whole rejected annulus
	// every call, which is the exact TMap add/erase churn the cap exists to
	// remove -- measured at ~68k candidate chunks/second produced by the entry
	// scans. Below 3/4 the queue genuinely has room (cold start, a teleport, or
	// a throughput burst) and should refill without waiting for the anchor to
	// move.
	{
		const int32 Cap = VoxelStreamAdmission::GetPendingJobCap();
		if (Cap <= 0 || PendingJobKeys.Num() * 4 < Cap * 3)
		{
			AdmissionCutoffDistSq = DBL_MAX;
		}
	}

	// 1. Hysteresis exit: currently-tracked chunks (any level) that drifted
	// beyond their level's unload ring, OR drifted inside their level's inner
	// edge (the next finer level has taken over -- no hysteresis on this
	// side in wave 1, see the doc comment on RingPresets in
	// VoxelWorldSubsystem.h: "hard boundary" per m2-plan.md's v0 decision),
	// get queued for unload. Chunks that merely left the load ring but are
	// still inside the unload ring stay tracked untouched -- this is the
	// same hysteresis gap the original single-ring code had, now evaluated
	// per level.
	for (const auto& Pair : ChunkRecords)
	{
		const FVoxelLevelChunkKey& LevelKey = Pair.Key;
		if (PendingUnloadSet.Contains(LevelKey))
		{
			continue;
		}
		const UVoxelWorldSubsystem::FRingPreset& Preset = UVoxelWorldSubsystem::RingPresets[LevelKey.Level];
		const double ChunkEdge = ChunkEdgeUUForLevel(LevelKey.Level);
		const double CenterX = (double(LevelKey.Key.X) + 0.5) * ChunkEdge;
		const double CenterY = (double(LevelKey.Key.Y) + 0.5) * ChunkEdge;
		const double DistSq = FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y);
		const double UnloadOuterUU = Preset.OuterMeters * 100.0 * UVoxelWorldSubsystem::UnloadRingMultiplier;
		const double InnerUU = Preset.InnerMeters * 100.0;
		const bool bBeyondOuter = DistSq > FMath::Square(UnloadOuterUU);
		const bool bInsideInner = LevelKey.Level > 0 && DistSq < FMath::Square(InnerUU);
		// Underground streaming (see namespace VoxelUnderground): the two tests
		// above are XY-only, which is correct for every chunk whose desired-ness
		// is a pure function of its footprint (the surface band and the depth
		// skirt both are). Chunks that entered ONLY via the anchor-relative deep
		// box are not -- without this they would stay resident forever once the
		// player surfaced, since their footprint is still well inside its ring.
		// Same hysteresis shape as the outer XY edge (UnloadRingMultiplier).
		bool bBeyondVertical = false;
		if (Pair.Value.bDeepAnchorRelative)
		{
			const int32 KeepChunks = VoxelUnderground::KeepChunks[LevelKey.Level];
			const double KeepUU = double(KeepChunks + 1) * ChunkEdge * UVoxelWorldSubsystem::UnloadRingMultiplier;
			const double CenterZ = (double(LevelKey.Key.Z) + 0.5) * ChunkEdge;
			bBeyondVertical = FMath::Abs(CenterZ - Anchor.Z) > KeepUU;
		}
		if (bBeyondOuter || bInsideInner || bBeyondVertical)
		{
			PendingUnloadKeys.Add(LevelKey);
			PendingUnloadSet.Add(LevelKey);
			EvictedThisCall.Add(LevelKey);
		}
	}

	// Drop the just-evicted keys from the two pending queues in ONE filtering
	// pass instead of a TArray::RemoveSingle per eviction. RemoveSingle is a
	// linear scan, and these queues run ~19k deep during a perf flight (the
	// worker throughput, not the desired-set size, is what bounds them), so the
	// old form was O(evictions * queue depth) -- measured as the single largest
	// component of this function's cost. Same result: at most one copy of a key
	// can be in a queue (a key is only ever queued right after ChunkRecords.Add,
	// which is guarded by ChunkRecords.Contains), so "remove all copies" and
	// "remove one copy" are the same removal here. Queue ORDER is irrelevant --
	// SortPendingQueues re-sorts both at the end of this function.
	// Filtered against only the keys evicted by THIS call (not the whole
	// PendingUnloadSet, which also holds not-yet-drained evictions from earlier
	// calls -- those were already removed from the queues when they happened,
	// and a chunk CAN legitimately be re-queued while still in that set, see
	// DrainUnloads' "raced with a re-add/remesh" branch).
	if (EvictedThisCall.Num() > 0)
	{
		PendingJobKeys.RemoveAllSwap([this](const FVoxelLevelChunkKey& K) { return EvictedThisCall.Contains(K); },
		                             EAllowShrinking::No);
		PendingGameThreadKeys.RemoveAllSwap([this](const FVoxelLevelChunkKey& K) { return EvictedThisCall.Contains(K); },
		                                    EAllowShrinking::No);
	}

	// 2. Hysteresis entry, per level: XY footprints within level L's annulus
	// [Inner(L), Outer(L)) that aren't already tracked become newly tracked
	// chunks. Level 0 routes to the worker queue or the game-thread queue
	// depending on whether it already contains an edited brick (unchanged
	// M1 behavior); levels >=1 always take the worker/MipChain path (m2-plan.md
	// "Distant edits" wave-1 limitation: higher levels render pure-generated
	// even where level-0 edits exist underneath them -- see MakeLevelSampler).
	//
	// Gated per level (bHasRecomputedLevel/LastAnchorChunkPerLevel): a level-L
	// chunk is (1<<L) times larger, so re-running this O(candidates) scan on
	// every level-0 chunk crossing (every 3.2m) would be wasted work for
	// outer levels once their own chunk hasn't actually changed.
	const double RecomputeT1 = FPlatformTime::Seconds();
	ThisFrameExitScanMs = float((RecomputeT1 - RecomputeT0) * 1000.0);

	int32 ScratchBoxRadius = 0; // BoxRadiusChunks out-param, used only for its bool return in the level gate

	for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
	{
		const double LevelT0 = FPlatformTime::Seconds();
		const FVoxelCoord AnchorVoxel = WorldToVoxelForLevel(Anchor, Level);
		const FVoxelChunkKey AnchorChunk = ChunkKeyForVoxel(AnchorVoxel);
		// Underground streaming: while underground the anchor's LEVEL-L chunk Z
		// is an input to this level's candidate set (the deep box), so it joins
		// X and Y in the gate -- but only for levels that actually get a deep
		// box, and only while underground. Above ground this is byte-for-byte
		// the M1/M2 gate, so the perf-gate flight's scan cadence is unchanged.
		const bool bZMatters = bAnchorUnderground && VoxelUnderground::BoxRadiusChunks(Level, 0.0, ScratchBoxRadius);
		if (bHasRecomputedLevel[Level] && AnchorChunk.X == LastAnchorChunkPerLevel[Level].X &&
		    AnchorChunk.Y == LastAnchorChunkPerLevel[Level].Y &&
		    (!bZMatters || AnchorChunk.Z == LastAnchorChunkPerLevel[Level].Z))
		{
			continue; // nothing new can have entered this level's annulus
		}
		LastAnchorChunkPerLevel[Level] = AnchorChunk;
		bHasRecomputedLevel[Level] = true;
		++LevelEntryScans[Level];
		++LevelsScannedThisCall;
		AdmissionsThisLevel = 0; // per-level admission budget (see its doc comment)

		const UVoxelWorldSubsystem::FRingPreset& Preset = UVoxelWorldSubsystem::RingPresets[Level];
		const double InnerUU = Preset.InnerMeters * 100.0;
		const double OuterUU = Preset.OuterMeters * 100.0;
		const double ChunkEdge = ChunkEdgeUUForLevel(Level);
		const int32 ChunkSpan = FMath::CeilToInt32(OuterUU / ChunkEdge) + 1;

		for (int32 Cy = AnchorChunk.Y - ChunkSpan; Cy <= AnchorChunk.Y + ChunkSpan; ++Cy)
		{
			for (int32 Cx = AnchorChunk.X - ChunkSpan; Cx <= AnchorChunk.X + ChunkSpan; ++Cx)
			{
				const double CenterX = (double(Cx) + 0.5) * ChunkEdge;
				const double CenterY = (double(Cy) + 0.5) * ChunkEdge;
				const double DistSq = FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y);
				if (DistSq >= FMath::Square(OuterUU) || (Level > 0 && DistSq < FMath::Square(InnerUU)))
				{
					continue; // outside this level's annulus
				}

				++ThisFrameLevelFootprints[Level];
				int32 ChunkZMin, ChunkZMax;
				FootprintChunkZRangeCached(Cx, Cy, Level, ChunkZMin, ChunkZMax);

				// Underground streaming (see namespace VoxelUnderground), step
				// 1: widen the memoized SURFACE band downward by this band's
				// depth skirt. Applied here rather than inside
				// ComputeFootprintChunkZRange precisely so the memo's inputs
				// stay (Level, X, Y) only -- the skirt depends on the anchor's
				// horizontal distance, which the memo does not key on.
				ChunkZMin -= VoxelUnderground::SkirtDepthChunks(Level, DistSq);

				const auto AddCandidate = [this, ChunkEdge, CenterX, CenterY, &Anchor](const FVoxelLevelChunkKey& LevelKey,
				                                                                     bool bDeepAnchorRelative)
				{
					if (ChunkRecords.Contains(LevelKey))
					{
						return;
					}

					// Bounded admission gate (a): while the WORKER queue is at
					// cap, a candidate farther than the farthest already-queued
					// chunk would be dropped again by TruncatePendingJobQueue at
					// the bottom of this very call -- so never make it a record.
					// Same 3D chunk-centre distance the queue is sorted by.
					// Applies only to the worker path: the game-thread queue is
					// edit-driven (always near the player, never a backlog) and
					// is not what the cap exists to bound.
					const bool bOverlayAware = NeedsOverlayAwarePath(LevelKey);
					const int32 Cap = VoxelStreamAdmission::GetPendingJobCap();
					if (!bOverlayAware && Cap > 0 && AdmissionsThisLevel >= Cap / 4)
					{
						// Per-level admission budget (see AdmissionsThisLevel).
						++CandidatesRejectedSinceLog;
						++CandidatesRejectedThisCall;
						bAdmissionDeferredWork = true;
						return;
					}
					if (!bOverlayAware && AdmissionCutoffDistSq < DBL_MAX)
					{
						const double CenterZ = (double(LevelKey.Key.Z) + 0.5) * ChunkEdge;
						const double DistSq3D = FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y) +
						                        FMath::Square(CenterZ - Anchor.Z);
						if (DistSq3D >= AdmissionCutoffDistSq)
						{
							++CandidatesRejectedSinceLog;
							++CandidatesRejectedThisCall;
							bAdmissionDeferredWork = true;
							return;
						}
					}

					VoxelStreaming::FChunkRecord& NewRecord = ChunkRecords.Add(LevelKey);
					++RecordsAddedSinceLog;
					AdmissionsThisLevel += bOverlayAware ? 0 : 1;
					NewRecord.bDeepAnchorRelative = bDeepAnchorRelative;
					if (bOverlayAware)
					{
						PendingGameThreadKeys.Add(LevelKey);
					}
					else
					{
						PendingJobKeys.Add(LevelKey);
					}
				};

				for (int32 Cz = ChunkZMin; Cz <= ChunkZMax; ++Cz)
				{
					AddCandidate(FVoxelLevelChunkKey{Level, FVoxelChunkKey{Cx, Cy, Cz}}, /*bDeepAnchorRelative*/ false);
				}

				// Step 2: the anchor-relative deep box, only while underground.
				// Chunks the surface band + skirt already cover are skipped so
				// they keep their unflagged (footprint-lifetime) status -- only
				// chunks that exist SOLELY because of the anchor's Z get the
				// flag, and therefore only those are subject to the exit pass's
				// vertical keep-test.
				int32 BoxRadius = 0;
				if (bAnchorUnderground && VoxelUnderground::BoxRadiusChunks(Level, DistSq, BoxRadius))
				{
					for (int32 Cz = AnchorChunk.Z - BoxRadius; Cz <= AnchorChunk.Z + BoxRadius; ++Cz)
					{
						if (Cz >= ChunkZMin && Cz <= ChunkZMax)
						{
							continue;
						}
						AddCandidate(FVoxelLevelChunkKey{Level, FVoxelChunkKey{Cx, Cy, Cz}}, /*bDeepAnchorRelative*/ true);
					}
				}
			}
		}

		// Levels that early-`continue`d above (anchor still inside the same
		// level-L chunk) leave their entry at 0 -- exactly the "did no work"
		// reading we want.
		ThisFrameLevelEntryMs[Level] = float((FPlatformTime::Seconds() - LevelT0) * 1000.0);
		MaxLevelEntryMs[Level] = FMath::Max(MaxLevelEntryMs[Level], ThisFrameLevelEntryMs[Level]);
	}

	PruneFootprintZRangeCache(Anchor);

	const double SortT0 = FPlatformTime::Seconds();
	SortPendingQueues(Anchor);
	// Bounded admission gate (b) -- must run immediately after the sort, which
	// is what puts the farthest (lowest-priority) entries at the front and
	// leaves SortScratchJob's distances aligned with the queue. Timed inside
	// sortMs: it is part of the same "get the queue into shape" stage, and
	// keeping it there means the existing sortMs metric still accounts for
	// 100% of the queue-maintenance cost.
	TruncatePendingJobQueue();
	const double SortT1 = FPlatformTime::Seconds();
	ThisFrameSortMs = float((SortT1 - SortT0) * 1000.0);
	ThisFrameRecomputeMs = float((SortT1 - RecomputeT0) * 1000.0);
	MaxRecomputeMs = FMath::Max(MaxRecomputeMs, ThisFrameRecomputeMs);
	MaxExitScanMs = FMath::Max(MaxExitScanMs, ThisFrameExitScanMs);
	MaxSortMs = FMath::Max(MaxSortMs, ThisFrameSortMs);
	++RecomputeCalls;
}

void FVoxelWorldImpl::TruncatePendingJobQueue()
{
	// Bounded admission gate (b) -- see AdmissionCutoffDistSq's doc comment.
	// Called only from RecomputeDesiredSet, immediately after SortPendingQueues:
	// PendingJobKeys is sorted lowest-priority-first (farthest at index 0), and
	// SortScratchJob[i].DistSq is the distance of PendingJobKeys[i].
	const int32 Cap = VoxelStreamAdmission::GetPendingJobCap();
	if (Cap <= 0 || PendingJobKeys.Num() <= Cap)
	{
		AdmissionCutoffDistSq = DBL_MAX; // not full: admit everything in range
		// Nothing was held back by THIS call. Only clear the deferral flag if
		// every level actually re-enumerated on this call (a movement-triggered
		// call scans only the levels whose own chunk the anchor crossed, so a
		// level gated out of it may still have candidates waiting -- clearing
		// then would lose the refill trigger for them). A refill call always
		// satisfies this, since it clears the per-level gate first.
		if (LevelsScannedThisCall == VoxelCoords::kNumLevels && CandidatesRejectedThisCall == 0)
		{
			bAdmissionDeferredWork = false;
		}
		return;
	}
	bAdmissionDeferredWork = true;

	const int32 OldNum = PendingJobKeys.Num();
	int32 ToDrop = OldNum - Cap;
	int32 Write = 0;
	for (int32 Read = 0; Read < OldNum; ++Read)
	{
		const VoxelCoords::FVoxelLevelChunkKey Key = PendingJobKeys[Read];
		if (ToDrop > 0)
		{
			VoxelStreaming::FChunkRecord* Rec = ChunkRecords.Find(Key);
			if (!Rec)
			{
				--ToDrop; // already untracked: a queue entry with no record is dead weight anyway
				continue;
			}
			// A queued chunk has never meshed, so it has no component and no job
			// in flight -- both are asserted rather than assumed, because
			// dropping a record that DOES own either would leak a component or
			// strand an in-flight result. Anything already queued for unload is
			// left to DrainUnloads (it owns that record's removal).
			if (!Rec->Component.IsValid() && !Rec->bJobInFlight && !PendingUnloadSet.Contains(Key))
			{
				ChunkRecords.Remove(Key);
				++RecordsDroppedSinceLog;
				--ToDrop;
				continue;
			}
		}
		PendingJobKeys[Write++] = Key;
	}
	PendingJobKeys.SetNum(Write, EAllowShrinking::No);

	// The cutoff is the distance of a surviving entry a headroom band IN FROM
	// the farthest survivor. Entries are dropped from the front (farthest) in
	// order, so survivors occupy [OldNum - Write, OldNum) of the still-sorted
	// scratch, with distance decreasing as the index rises.
	//
	// Why not simply "distance of the farthest survivor": candidates cluster
	// just inside it, so every call would admit a few thousand chunks barely
	// nearer than the cutoff and then drop most of them again on this same pass
	// -- a thrash costing one TMap insert + one erase per chunk per call
	// (measured at ~46k/second before this band existed). The band admits only
	// chunks that are clearly nearer, sized so a call's admissions are on the
	// order of what a call's dispatches drain (~160 at the measured rate),
	// which is the whole point of the exercise: make production track drain.
	const int32 Headroom = FMath::Max(1, Cap / 8);
	const int32 CutoffIndex = FMath::Min(OldNum - Write + Headroom, OldNum - 1);
	AdmissionCutoffDistSq =
		(Write > 0 && SortScratchJob.Num() == OldNum && CutoffIndex >= 0) ? SortScratchJob[CutoffIndex].DistSq : DBL_MAX;
}

// --- budgeted drains ----------------------------------------------------

void FVoxelWorldImpl::DispatchJobs()
{
	// docs/m1-plan.md Stage 2 decisions table: "<=2xLogicalCores jobs in
	// flight."
	const int32 MaxJobsInFlight = 2 * FPlatformMisc::NumberOfCoresIncludingHyperthreads();

	while (JobsInFlightCounter.GetValue() < MaxJobsInFlight && PendingJobKeys.Num() > 0)
	{
		const VoxelCoords::FVoxelLevelChunkKey LevelKey = PendingJobKeys.Pop(EAllowShrinking::No); // highest priority (see SortPendingQueues)

		VoxelStreaming::FChunkRecord* Rec = ChunkRecords.Find(LevelKey);
		if (!Rec)
		{
			continue; // left the desired set between recompute and dispatch
		}

		// Defensive re-check (any level as of M2 wave 2 -- see
		// NeedsOverlayAwarePath): an edit landing in this same tick, between
		// recompute and dispatch, may have made this chunk (or one of its
		// mip ancestors) edited-only.
		if (NeedsOverlayAwarePath(LevelKey))
		{
			PendingGameThreadKeys.Add(LevelKey);
			continue;
		}

		Rec->bJobInFlight = true;
		JobsInFlightCounter.Increment();
		++JobsDispatchedSinceLog;
		{
			const int32 L = FMath::Clamp(LevelKey.Level, 0, VoxelCoords::kNumLevels - 1);
			++LevelJobsDispatchedSinceLog[L];
			++LevelJobsDispatchedTotal[L];
		}

		const uint64 GenId = Rec->GenerationId;
		// Worker threading (decisions table): the job touches ONLY
		// GeneratedWorld (pure function of seed, lock-free) -- never World
		// (the overlay is not thread-safe and workers must never touch it).
		// GenPtr, QueuePtr and CounterPtr are raw pointers into Impl-owned
		// data; safe because Deinitialize waits for every in-flight task
		// (WaitForInFlightTasks) before Impl is destroyed.
		const vxc::GeneratedWorld<VoxelCoords::BrickEdgeVoxels>* GenPtr = &Voxels.generated();
		TQueue<VoxelStreaming::FJobResult, EQueueMode::Mpsc>* QueuePtr = &ResultsQueue;
		FThreadSafeCounter* CounterPtr = &JobsInFlightCounter;
		vxc::Counters* PerfCountersPtr = &PerfCounters;
		// M2 wave 2 item 1: shared cross-job mip cache -- see FSharedMipCache's
		// doc comment for the lifetime/thread-safety argument (same "Impl
		// outlives every job" guarantee as the three pointers above).
		FSharedMipCache* SharedMipCachePtr = &SharedMipCache;
		// M2 wave 2 item 1 race fix: snapshot the edit epoch at dispatch time
		// (see FCachedMipBuilder's GlobalEditEpoch/EpochSnapshot doc comment,
		// and EditEpoch's doc comment above) -- EditEpochPtr is read live at
		// insert time, EditEpochSnapshot is this moment's value.
		const std::atomic<uint64>* EditEpochPtr = &EditEpoch;
		const uint64 EditEpochSnapshot = EditEpoch.load();

		UE::Tasks::TTask<void> Task = UE::Tasks::Launch(
			TEXT("VoxelChunkMeshJob"),
			[GenPtr, LevelKey, GenId, QueuePtr, CounterPtr, PerfCountersPtr, SharedMipCachePtr, EditEpochPtr, EditEpochSnapshot]()
			{
				SCOPE_CYCLE_COUNTER(STAT_VoxelWorkerJob);
				const double JobStartSeconds = FPlatformTime::Seconds();

				VoxelStreaming::FJobResult Result;
				Result.Key = LevelKey;
				Result.GenerationId = GenId;

				const VoxelCoords::FVoxelChunkKey& Key = LevelKey.Key;
				if (LevelKey.Level == 0)
				{
					// Column-cache the whole job (docs/m1-plan.md stage 3a): a
					// naive GeneratedWorld::materialAt sampler recomputes the
					// full amplifier column per VOXEL query (~130k column
					// evaluations per chunk); this grid computes each of the
					// (32+2)^2 columns exactly once (~100x less amplifier work,
					// measured ~5 -> ~50+ chunks/s). The +1 apron matches the
					// mesher's [-1,B] sampler contract across chunk borders.
					// Level 0 keeps this exact, already-tuned fast path
					// unchanged rather than routing it through MipChain too
					// (a level-0 MipChain::brick(0,key) is just source_(key)
					// directly, so it would be equivalent but strictly slower
					// -- an extra Brick<8> materialization + get() indirection
					// per voxel instead of Amplifier::materialAt directly).
					constexpr int32 ChunkVox = VoxelCoords::ChunkEdgeVoxels;
					constexpr int32 GridEdge = ChunkVox + 2;
					const int64 BaseVX = int64(Key.X) * ChunkVox;
					const int64 BaseVY = int64(Key.Y) * ChunkVox;
					TArray<vxc::ColumnSample> Columns;
					Columns.SetNumUninitialized(GridEdge * GridEdge);
					const vxc::Amplifier& Amp = GenPtr->amplifier();
					for (int32 LY = 0; LY < GridEdge; ++LY)
					{
						for (int32 LX = 0; LX < GridEdge; ++LX)
						{
							Columns[LX + GridEdge * LY] =
								Amp.column(BaseVX + LX - 1, BaseVY + LY - 1);
						}
					}
					const auto GridSampler = [&Columns, BaseVX, BaseVY](int64 X, int64 Y, int64 Z)
					{
						const int32 LX = int32(X - BaseVX) + 1;
						const int32 LY = int32(Y - BaseVY) + 1;
						checkSlow(LX >= 0 && LX < GridEdge && LY >= 0 && LY < GridEdge);
						return vxc::Amplifier::materialAt(Columns[LX + GridEdge * LY], Z);
					};
					// docs/debug-tooling-plan.md P1 "vxc::Counters": columnEvals
					// counts the explicit Amp.column() calls this cache-build loop
					// makes -- the ~100x-cheaper number the doc comment above
					// references, NOT a count of voxel-core-internal column work
					// (that stays uninstrumented this pass, per P1 scope).
					PerfCountersPtr->incColumnEvals(uint64_t(GridEdge) * GridEdge);
					MeshChunkBricks(Key, GridSampler, Result.Quads, PerfCountersPtr);
				}
				else
				{
					// M2 mip sourcing (docs/m2-plan.md "Mip sourcing" row):
					// level-L (L>=1) bricks come from a per-job FCachedMipBuilder
					// over a pure-GeneratedWorld level-0 source, wired to the
					// cross-job SharedMipCachePtr (M2 wave 2 item 1) -- see
					// MakeLevelSampler's doc comment for the column-grid LRU
					// that avoids the naive-recompute perf trap. Still lock-free
					// (GenPtr only, never World/the overlay); by the time
					// DispatchJobs pops this key it has already been routed here
					// specifically BECAUSE it is not a known edited-ancestor
					// chunk (NeedsOverlayAwarePath / EditedAncestorChunks, M2
					// wave 2 item 2), so a pure-generated mesh is correct here.
					const auto LevelSampler =
						MakeLevelSampler(*GenPtr, LevelKey.Level, PerfCountersPtr, SharedMipCachePtr, EditEpochPtr, EditEpochSnapshot);
					MeshChunkBricks(Key, LevelSampler, Result.Quads, PerfCountersPtr);
				}

				Result.JobMs = float((FPlatformTime::Seconds() - JobStartSeconds) * 1000.0);
				QueuePtr->Enqueue(MoveTemp(Result));
				CounterPtr->Decrement();
			},
			UE::Tasks::ETaskPriority::BackgroundNormal);
		InFlightTasks.Add(MoveTemp(Task));
	}
}

UVoxelChunkComponent& FVoxelWorldImpl::AcquireChunkComponent(AActor& Owner, USceneComponent& Root)
{
	while (ComponentPool.Num() > 0)
	{
		const TWeakObjectPtr<UVoxelChunkComponent> Pooled = ComponentPool.Pop(EAllowShrinking::No);
		if (UVoxelChunkComponent* Comp = Pooled.Get())
		{
			++ThisFramePoolReuses;
			++TotalPoolReuses;
			return *Comp;
		}
		// Stale weak pointer: shouldn't happen in practice (pooled components
		// stay registered/attached the whole time they're parked -- see
		// ComponentPool's doc comment) but skip it and keep looking rather
		// than ever hand back a dangling reference.
	}

	UVoxelChunkComponent* Comp = NewObject<UVoxelChunkComponent>(&Owner);
	Comp->SetupAttachment(&Root);
	Comp->RegisterComponent();
	return *Comp;
}

void FVoxelWorldImpl::ReturnChunkComponentToPool(UVoxelChunkComponent& InComp)
{
	const int32 PoolMax = VoxelDebug::GetComponentPoolMax();
	if (ComponentPool.Num() >= PoolMax)
	{
		// Already at cap: fall back to the pre-pooling path rather than grow
		// this array unboundedly.
		InComp.DestroyComponent();
		return;
	}

	// Reset every piece of per-residency state EXCEPT ChunkLevel/ChunkMaterial/
	// ChunkMID -- those are overwritten on the next acquire (SetLevel/
	// SetMaterial, called unconditionally in ApplyMeshResult's first-load
	// branch below) before anything reads them again, and leaving ChunkMID
	// alive is itself a real part of the pooling win: ApplyRingFadeParams
	// reuses the SAME UMaterialInstanceDynamic and just updates its scalar
	// params instead of calling UMaterialInstanceDynamic::Create again (a
	// non-trivial render-thread cost of its own).
	//  - SetVisibility(false): hides it. Coalesces with the MarkRenderStateDirty
	//    below into a single end-of-frame proxy recreate (UE batches same-frame
	//    dirty-render-state requests, it does not recreate per call), so doing
	//    both here costs the same as doing either one alone.
	//  - SetChunkQuads({}, ...): drops the stale geometry (CreateSceneProxy
	//    returns null once ChunkQuads is empty) so a parked component holds no
	//    GPU-side vertex/index buffers and has no live scene proxy at all --
	//    the cheapest possible "parked" state, and correctness-required (a
	//    hidden-but-still-meshed component would otherwise flash its OLD
	//    chunk's geometry for one frame if something ever force-showed it).
	//  - ClearDebugTint(): a chunk-state/ring tint from this component's
	//    PREVIOUS residency must never bleed into whichever new chunk reuses
	//    it -- this is the "indistinguishable from a fresh component"
	//    correctness bar for the debug-tint layers specifically.
	// No SetComponentTickEnabled(false) call: UVoxelChunkComponent's
	// constructor already sets PrimaryComponentTick.bCanEverTick = false
	// unconditionally (this primitive never ticks, pooled or not), so there
	// is nothing for it to disable.
	InComp.SetVisibility(false);
	InComp.SetChunkQuads({}, VoxelCoords::ChunkEdgeVoxels);
	InComp.ClearDebugTint();
	ComponentPool.Add(&InComp);
}

bool FVoxelWorldImpl::ApplyMeshResult(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material,
                                       const VoxelCoords::FVoxelLevelChunkKey& Key, VoxelStreaming::FChunkRecord& Rec,
                                       TArray<FVoxelChunkQuad>&& Quads, bool bIsGameThreadMesh)
{
	// docs/debug-tooling-plan.md P1 "Memory" row: ResidentQuads tracks
	// currently-loaded quads (not the cumulative TotalQuadsLoaded below), so
	// it must be decremented by whatever this record held before, regardless
	// of which branch below runs.
	ResidentQuads -= Rec.LastQuadCount;
	Rec.LastQuadCount = 0;

	if (Quads.Num() == 0)
	{
		// No visible geometry (fully buried chunk, or an edit carved away
		// the last exposed faces): park (not destroy) any stale component --
		// M1 hitch-gap wave, same pooling path DrainUnloads uses -- rather
		// than spawning/keeping an empty one. The record stays in
		// ChunkRecords (this chunk key is still in the desired set; it might
		// gain quads again on a future edit), it just has no live component
		// until then.
		if (UVoxelChunkComponent* Existing = Rec.Component.Get())
		{
			ReturnChunkComponentToPool(*Existing);
		}
		Rec.Component = nullptr;
		return false;
	}

	UVoxelChunkComponent* Comp = Rec.Component.Get();
	const bool bWasFirstLoad = (Comp == nullptr);
	if (!Comp)
	{
		// M1 hitch-gap wave: prefer a pooled component (hidden, no proxy,
		// still registered/attached) over NewObject+RegisterComponent -- see
		// AcquireChunkComponent. Every per-load property below is
		// unconditionally re-applied regardless of which path AcquireChunkComponent
		// took, so a reused component ends up byte-for-byte indistinguishable
		// from a fresh one at this (level, key): SetLevel resets the level
		// (and, via ApplyRingFadeParams, the ring cross-fade params) even
		// though a pooled component may have held a DIFFERENT level in its
		// previous residency; SetRelativeLocation/SetMaterial/SetVisibility
		// likewise never assume anything about prior state.
		Comp = &AcquireChunkComponent(Owner, Root);
		Comp->SetLevel(Key.Level);
		Comp->SetRelativeLocation(VoxelCoords::ChunkOriginWorldForLevel(Key.Key, Key.Level));
		Comp->SetMaterial(0, Material);
		Comp->SetVisibility(true); // undo ReturnChunkComponentToPool's hide; no-op for a genuinely fresh component (visible by default)
		Rec.Component = Comp;
		++TotalChunksLoaded;
		++LevelChunksLoadedTotal[FMath::Clamp(Key.Level, 0, VoxelCoords::kNumLevels - 1)];
	}
	TotalQuadsLoaded += Quads.Num();
	Rec.LastQuadCount = Quads.Num();
	ResidentQuads += Rec.LastQuadCount;
	Comp->SetChunkQuads(MoveTemp(Quads), VoxelCoords::ChunkEdgeVoxels);

	// docs/debug-tooling-plan.md P1 chunk-state tints: "just-loaded" fires on
	// any first component creation (worker or game-thread path alike);
	// "re-mesh" only fires when this was a genuine re-mesh of an
	// already-resident chunk via the overlay-aware path (never the worker
	// path -- workers only ever run a chunk's FIRST mesh).
	if (bWasFirstLoad)
	{
		Rec.LoadedAtSeconds = ElapsedSeconds;
	}
	else if (bIsGameThreadMesh)
	{
		Rec.RemeshedAtSeconds = ElapsedSeconds;
	}
	// Overlay ownership: level 0 uses the exact live-overlay scan
	// (ChunkOwnsEditedBrick); level>=1 uses the EditedAncestorChunks
	// membership PropagateEditToMips maintains (M2 wave 2 item 2) -- both
	// answer "does this chunk currently render through the overlay-aware
	// path," just via different exact-vs-tracked-set mechanisms (see
	// ChunkHasEditedBrick vs NeedsOverlayAwarePath).
	Rec.bHasOverlayBricks =
		(Key.Level == 0) ? ChunkOwnsEditedBrick(Key.Key) : EditedAncestorChunks[Key.Level].Contains(Key.Key);
	return bWasFirstLoad;
}

void FVoxelWorldImpl::DrainResults(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material)
{
	// docs/m1-plan.md Stage 2 decisions table: "<=8 chunk component
	// applies/frame" -- now a cvar (voxel.Stream.MaxAppliesPerFrame, default
	// 8) so the ramp's apply rate can be smoothed/tightened without a
	// recompile (docs/status.md "Perf-run hitches" isolation task).
	const int32 MaxApplies = VoxelDebug::GetStreamMaxAppliesPerFrame();
	// M1 gate (docs/status.md M1 gate row): MaxApplies gates only the
	// RENDER-THREAD-FACING applies (ApplyMeshResult -> SetChunkQuads ->
	// MarkRenderStateDirty -> FScene::AddPrimitive + GPU buffer upload). A
	// STALE result (chunk left the desired set, or was superseded by an edit
	// re-mesh while its job was in flight) creates no proxy and costs only a
	// TMap::Find + a counter bump -- it must NOT consume the render-facing
	// budget, or under the heavy ring-crossing churn a perf-flight generates
	// the budget is spent entirely discarding stale results and live chunks
	// near the player never get applied at all (measured: R0 resident collapsed
	// to 0 under aggressive unloading before this split). The cheap discard
	// work is instead bounded by a much larger per-frame drain cap so a huge
	// stale backlog can't stall the game thread either.
	constexpr int32 kMaxResultDrainsPerFrame = 1024;
	int32 Applied = 0; // render-thread-facing applies (live results) this frame -- gated by MaxApplies
	int32 Drains = 0;  // total results dequeued incl. stale discards (game-thread-cheap) -- gated by kMaxResultDrainsPerFrame
	int32 ProxiesCreated = 0;
	VoxelStreaming::FJobResult Result;
	while (Applied < MaxApplies && Drains < kMaxResultDrainsPerFrame && ResultsQueue.Dequeue(Result))
	{
		++Drains;
		++ResultsDrainedSinceLog;

		// docs/debug-tooling-plan.md P1 "Worker timings": recorded for every
		// drained result, even ones about to be discarded as stale below --
		// the worker did real, representative work regardless of whether the
		// result is still wanted.
		WorkerJobMsWindow[WorkerJobMsWindowNext] = Result.JobMs;
		WorkerJobMsWindowNext = (WorkerJobMsWindowNext + 1) % WorkerJobMsWindowSize;
		WorkerJobMsWindowCount = FMath::Min(WorkerJobMsWindowCount + 1, WorkerJobMsWindowSize);

		// M2 wave 2 item 1: same recording, split into this result's level's
		// own window (see LevelWorkerJobMsWindow doc comment).
		{
			const int32 Lvl = FMath::Clamp(Result.Key.Level, 0, VoxelCoords::kNumLevels - 1);
			LevelWorkerJobMsWindow[Lvl][LevelWorkerJobMsWindowNext[Lvl]] = Result.JobMs;
			LevelWorkerJobMsWindowNext[Lvl] = (LevelWorkerJobMsWindowNext[Lvl] + 1) % WorkerJobMsWindowSize;
			LevelWorkerJobMsWindowCount[Lvl] = FMath::Min(LevelWorkerJobMsWindowCount[Lvl] + 1, WorkerJobMsWindowSize);
		}

		VoxelStreaming::FChunkRecord* Rec = ChunkRecords.Find(Result.Key);
		// Stale-result discard: the chunk left the desired set entirely (no
		// record any more), or an edit re-mesh superseded this job while it
		// was in flight (GenerationId no longer matches the id the job was
		// dispatched with -- MarkChunkDirtyForRemesh bumped it). Free (no
		// proxy) -- does NOT consume the render-facing MaxApplies budget.
		if (!Rec || Rec->GenerationId != Result.GenerationId)
		{
			++StaleResultsDiscarded;
			++StaleDiscardsSinceLog;
			continue;
		}

		Rec->bJobInFlight = false;
		ZeroQuadAppliesSinceLog += (Result.Quads.Num() == 0) ? 1 : 0;
		if (Result.Quads.Num() == 0)
		{
			++LevelZeroQuadTotal[FMath::Clamp(Result.Key.Level, 0, VoxelCoords::kNumLevels - 1)];
		}
		++Applied; // a live result: this IS a render-thread-facing apply
		if (ApplyMeshResult(Owner, Root, Material, Result.Key, *Rec, MoveTemp(Result.Quads), /*bIsGameThreadMesh*/ false))
		{
			++ProxiesCreated;
		}
	}
	LastAppliedFrac = MaxApplies > 0 ? float(Applied) / float(MaxApplies) : 0.f;
	ThisFrameAppliesFromWorker = Applied;
	ThisFrameProxiesCreated += ProxiesCreated;
}

void FVoxelWorldImpl::DrainGameThreadMesh(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material)
{
	// docs/m1-plan.md Stage 2 decisions table: "<=4 edit re-meshes/frame" --
	// now a cvar (voxel.Stream.MaxRemeshesPerFrame, default 4; docs/status.md
	// "Perf-run hitches" isolation task). This budget covers both first-time
	// load of a chunk that already contains an edited brick
	// (ChunkHasEditedBrick routed it here in RecomputeDesiredSet/DispatchJobs)
	// and a post-edit dirty re-mesh (MarkChunkDirtyForRemesh) -- both use the
	// identical overlay-aware game-thread mesh path, so they share one budget
	// rather than two.
	const int32 MaxRemeshes = VoxelDebug::GetStreamMaxRemeshesPerFrame();
	int32 Count = 0;
	int32 ProxiesCreated = 0;
	while (Count < MaxRemeshes && PendingGameThreadKeys.Num() > 0)
	{
		// M2 wave 2: ANY level can be on this queue now -- level 0 (unchanged
		// from wave 1) and level>=1 mip ancestors of an edit (see
		// PropagateEditToMips / MarkChunkDirtyForRemesh).
		const VoxelCoords::FVoxelLevelChunkKey LevelKey = PendingGameThreadKeys.Pop(EAllowShrinking::No); // nearest

		VoxelStreaming::FChunkRecord* Rec = ChunkRecords.Find(LevelKey);
		if (!Rec)
		{
			continue; // left the desired set; doesn't consume the budget
		}
		++Count;

		SCOPE_CYCLE_COUNTER(STAT_VoxelGameThreadMesh);
		TArray<FVoxelChunkQuad> Quads;
		if (LevelKey.Level == 0)
		{
			MeshChunkBricks(
				LevelKey.Key, [this](int64 X, int64 Y, int64 Z) { return Voxels.materialAt(X, Y, Z); }, Quads, &PerfCounters);
		}
		else
		{
			// M2 wave 2 item 2 ("Distant-edit mip propagation"): overlay-aware
			// level>=1 sampler over World::brickAt, game-thread only (see
			// MakeOverlayAwareLevelSampler's doc comment) -- this is what
			// closes the wave-1 "R1+ never shows edits" limitation. Logged
			// (not Verbose): this is a rare, edit-triggered event, and the
			// log line is the headless-run evidence that a distant edit
			// actually re-meshed a mip ring chunk.
			const auto OverlaySampler = MakeOverlayAwareLevelSampler(Voxels, LevelKey.Level);
			MeshChunkBricks(LevelKey.Key, OverlaySampler, Quads, &PerfCounters);
			UE_LOG(LogVoxelEdit, Log, TEXT("Distant-edit mip re-mesh: level=%d chunk=(%d,%d,%d) quads=%d"), LevelKey.Level,
			       LevelKey.Key.X, LevelKey.Key.Y, LevelKey.Key.Z, Quads.Num());
		}
		if (ApplyMeshResult(Owner, Root, Material, LevelKey, *Rec, MoveTemp(Quads), /*bIsGameThreadMesh*/ true))
		{
			++ProxiesCreated;
		}
	}
	LastRemeshFrac = float(Count) / float(MaxRemeshes);
	ThisFrameEditRemeshes = Count;
	ThisFrameProxiesCreated += ProxiesCreated;
}

void FVoxelWorldImpl::DrainUnloads()
{
	// docs/m1-plan.md Stage 2 decisions table: "<=4 unloads/frame" -- now a
	// cvar (voxel.Stream.MaxUnloadsPerFrame, default 4; docs/status.md
	// "Perf-run hitches" isolation task).
	const int32 MaxUnloads = VoxelDebug::GetStreamMaxUnloadsPerFrame();
	// M1 gate (docs/status.md M1 gate row): MaxUnloads gates only the
	// RENDER-THREAD-FACING unloads -- a chunk that has a live component, whose
	// pool-park (ReturnChunkComponentToPool -> SetVisibility(false) +
	// SetChunkQuads({}) -> MarkRenderStateDirty) enqueues an
	// FScene::RemovePrimitive. A component-LESS record (an outer-ring R2/R3/R4
	// candidate that was queued as a job but left the desired set before it
	// ever meshed -- by far the bulk of the unload traffic during a perf
	// flight: measured 130k+ component-less evictions vs ~7k real loads) costs
	// only a TMap erase and must NOT consume the render-facing budget, or the
	// real R0/R1 unloads starve behind the far-ring flood and resident R0
	// chunks that have long left view range pile up unbounded (measured R0
	// resident bloating to ~6000, ~4x its ~1600 desired size, which is exactly
	// what loaded the render thread into the hitch). Cheap evictions flow
	// freely under a generous per-frame pop cap (so a huge backlog still can't
	// stall the game thread); component-bearing unloads over budget this frame
	// are deferred (kept tracked + re-queued) for the next frame rather than
	// forced through.
	constexpr int32 kMaxUnloadPopsPerFrame = 1024;
	int32 ComponentUnloads = 0; // render-thread-facing pool-parks this frame -- gated by MaxUnloads
	int32 Pops = 0;             // total queue pops incl. free component-less evictions -- gated by kMaxUnloadPopsPerFrame
	TArray<VoxelCoords::FVoxelLevelChunkKey> Deferred; // component-bearing unloads beyond MaxUnloads this frame
	while (Pops < kMaxUnloadPopsPerFrame && PendingUnloadKeys.Num() > 0)
	{
		const VoxelCoords::FVoxelLevelChunkKey Key = PendingUnloadKeys.Pop(EAllowShrinking::No);
		++Pops;

		VoxelStreaming::FChunkRecord* Rec = ChunkRecords.Find(Key);
		if (!Rec)
		{
			PendingUnloadSet.Remove(Key); // already gone (raced with a re-add/remesh); nothing to do
			continue;
		}

		// A live component here means this pop costs a render-thread
		// RemovePrimitive -- gate it. Over budget this frame: keep the record
		// tracked and re-queue it for the next frame (do NOT drop the chunk).
		if (Rec->Component.IsValid())
		{
			if (ComponentUnloads >= MaxUnloads)
			{
				Deferred.Add(Key); // stays in PendingUnloadSet, re-added below
				continue;
			}
			// Any worker job still in flight for this key keeps running to
			// completion (it can't be cancelled); when its result arrives,
			// DrainResults finds no record for the key and discards it.
			// M1 hitch-gap wave: park (not destroy) -- see
			// ReturnChunkComponentToPool / docs/status.md M1 gate row.
			ReturnChunkComponentToPool(*Rec->Component.Get());
			++ComponentUnloads;
		}

		PendingUnloadSet.Remove(Key);
		ResidentQuads -= Rec->LastQuadCount;
		ChunkRecords.Remove(Key);
		++TotalChunksUnloaded;
		++RecordsEvictedSinceLog;
	}
	// Re-queue the deferred component-bearing unloads (still in PendingUnloadSet,
	// still tracked) so they're retried next frame under the render budget.
	PendingUnloadKeys.Append(Deferred);
	LastUnloadFrac = MaxUnloads > 0 ? float(ComponentUnloads) / float(MaxUnloads) : 0.f;
	ThisFrameUnloads = ComponentUnloads;
}

// --- dig / place (edit-log authority path) -----------------------------

vxc::RaycastHit FVoxelWorldImpl::CastFromCamera(const FVector& CameraLoc, const FVector& Dir) const
{
	const int64 OxMm = VoxelCoords::WorldToMm(CameraLoc.X);
	const int64 OyMm = VoxelCoords::WorldToMm(CameraLoc.Y);
	const int64 OzMm = VoxelCoords::WorldToMm(CameraLoc.Z);
	const double RangeMm = UVoxelWorldSubsystem::DigPlaceRangeMeters * 1000.0;
	const int64 DxMm = (int64)FMath::RoundToDouble(Dir.X * RangeMm);
	const int64 DyMm = (int64)FMath::RoundToDouble(Dir.Y * RangeMm);
	const int64 DzMm = (int64)FMath::RoundToDouble(Dir.Z * RangeMm);

	// World::materialAt is overlay-aware (docs/m1-plan.md Stage 2 decisions
	// table: "call vxc::raycastVoxels against the subsystem's
	// World::materialAt (game thread)"). Game thread only -- Voxels.overlay_
	// is not thread-safe.
	const auto MaterialFn = [this](int64 X, int64 Y, int64 Z) { return Voxels.materialAt(X, Y, Z); };
	return vxc::raycastVoxels(MaterialFn, OxMm, OyMm, OzMm, DxMm, DyMm, DzMm);
}

void FVoxelWorldImpl::CollectDirtyChunks(int64 Vx, int64 Vy, int64 Vz, TSet<VoxelCoords::FVoxelChunkKey>& Out) const
{
	using namespace VoxelCoords;

	// The mesher apron reads 1 voxel across a brick's (and therefore a
	// render chunk's) boundary. An edited voxel sitting on a render-chunk
	// border therefore also invalidates whichever neighbor chunk(s) share
	// that border -- including diagonal neighbors when the voxel sits on an
	// edge or corner of the chunk, since meshBrick's AO sampling reads the
	// diagonal apron cell too (voxelcore/mesher.h planeSolid(-1,-1) etc).
	const FVoxelChunkKey Base = ChunkKeyForVoxel(FVoxelCoord{Vx, Vy, Vz});
	const int64 Lx = FloorMod(Vx, ChunkEdgeVoxels);
	const int64 Ly = FloorMod(Vy, ChunkEdgeVoxels);
	const int64 Lz = FloorMod(Vz, ChunkEdgeVoxels);
	const int32 Ox = (Lx == 0) ? -1 : (Lx == ChunkEdgeVoxels - 1 ? 1 : 0);
	const int32 Oy = (Ly == 0) ? -1 : (Ly == ChunkEdgeVoxels - 1 ? 1 : 0);
	const int32 Oz = (Lz == 0) ? -1 : (Lz == ChunkEdgeVoxels - 1 ? 1 : 0);

	const int32 XSteps = Ox != 0 ? 2 : 1;
	const int32 YSteps = Oy != 0 ? 2 : 1;
	const int32 ZSteps = Oz != 0 ? 2 : 1;
	for (int32 Sx = 0; Sx < XSteps; ++Sx)
	{
		for (int32 Sy = 0; Sy < YSteps; ++Sy)
		{
			for (int32 Sz = 0; Sz < ZSteps; ++Sz)
			{
				FVoxelChunkKey Key = Base;
				if (Sx == 1) Key.X += Ox;
				if (Sy == 1) Key.Y += Oy;
				if (Sz == 1) Key.Z += Oz;
				Out.Add(Key);
			}
		}
	}
}

bool FVoxelWorldImpl::NeedsOverlayAwarePath(const VoxelCoords::FVoxelLevelChunkKey& LevelKey) const
{
	// Level 0: exact live-overlay scan (unchanged from wave 1). Level>=1
	// (M2 wave 2 item 2): the tracked EditedAncestorChunks membership set
	// PropagateEditToMips maintains -- see that function's doc comment for
	// why this is a maintained set rather than a live scan at this level
	// (scanning the overlay directly here would touch up to
	// (ChunkEdgeBricks*2^Level)^3 level-0 brick keys PER CANDIDATE chunk).
	if (LevelKey.Level == 0)
	{
		return ChunkHasEditedBrick(LevelKey.Key);
	}
	return EditedAncestorChunks[LevelKey.Level].Contains(LevelKey.Key);
}

void FVoxelWorldImpl::MarkChunkDirtyForRemesh(const VoxelCoords::FVoxelLevelChunkKey& LevelKey)
{
	// M2 wave 2: generalized from a level-0-only helper (wave 1) to any
	// level -- see the doc comment on the declaration. Level-0 callers
	// (ApplyGroupedEdits, below) and level>=1 callers (PropagateEditToMips)
	// share this identical bump/requeue logic.
	VoxelStreaming::FChunkRecord* Rec = ChunkRecords.Find(LevelKey);
	if (!Rec)
	{
		// Not currently streamed/tracked: nothing to re-mesh right now. On a
		// later load, NeedsOverlayAwarePath routes it correctly -- level 0's
		// ChunkHasEditedBrick scan extends one brick beyond the chunk, so
		// border-neighbors of edits also take the overlay-aware path (no
		// stale-seam-on-reload case); level>=1's EditedAncestorChunks
		// membership was already recorded by PropagateEditToMips before this
		// call even for untracked chunks (see that function).
		return;
	}
	++Rec->GenerationId; // invalidates any in-flight/queued worker result for this chunk
	Rec->bJobInFlight = false;
	PendingJobKeys.RemoveSingle(LevelKey); // a dirtied chunk is never worker-dispatched
	PendingGameThreadKeys.AddUnique(LevelKey);
}

void FVoxelWorldImpl::PropagateEditToMips(const TSet<VoxelCoords::FVoxelChunkKey>& DirtyLevel0Chunks)
{
	using namespace VoxelCoords;
	constexpr int32 BricksPerChunk = ChunkEdgeBricks;

	// M2 wave 2 item 1 race fix: bump BEFORE any SharedMipCache::Invalidate
	// call below, so any worker job whose EpochSnapshot was taken before
	// this point (game thread; all edit application + dispatch is game-
	// thread-only, so this is a clean sequence point) will detect the
	// mismatch and skip a stale re-Insert if it races to finish after this
	// edit's invalidations (see FCachedMipBuilder's GlobalEditEpoch doc
	// comment and EditEpoch's doc comment).
	EditEpoch.fetch_add(1);

	for (int32 Level = 1; Level < kNumLevels; ++Level)
	{
		// De-duped per level: several DirtyLevel0Chunks (e.g. a dig's 6
		// chunk-border-neighbor apron cells) commonly share one ancestor at
		// higher levels, since the ancestor's footprint is (1<<Level) times
		// larger -- avoid invalidating/logging the same ancestor repeatedly.
		TSet<FVoxelChunkKey> AncestorsThisLevel;
		AncestorsThisLevel.Reserve(DirtyLevel0Chunks.Num());
		for (const FVoxelChunkKey& Level0Chunk : DirtyLevel0Chunks)
		{
			AncestorsThisLevel.Add(AncestorChunkKey(Level0Chunk, Level));
		}

		for (const FVoxelChunkKey& Ancestor : AncestorsThisLevel)
		{
			const bool bAlreadyEdited = EditedAncestorChunks[Level].Contains(Ancestor);
			EditedAncestorChunks[Level].Add(Ancestor);

			// SharedMipCache stores PURE bricks only (see its doc comment);
			// this chunk's footprint at this level is now stale for
			// rendering, so drop every brick this chunk owns from the shared
			// cache -- BricksPerChunk^3 lookups (64 at BricksPerChunk=4),
			// cheap and bounded regardless of Level.
			for (int32 Dz = 0; Dz < BricksPerChunk; ++Dz)
			{
				for (int32 Dy = 0; Dy < BricksPerChunk; ++Dy)
				{
					for (int32 Dx = 0; Dx < BricksPerChunk; ++Dx)
					{
						const vxc::BrickKey BKey{Ancestor.X * BricksPerChunk + Dx, Ancestor.Y * BricksPerChunk + Dy,
						                          Ancestor.Z * BricksPerChunk + Dz};
						SharedMipCache.Invalidate(Level, BKey);
					}
				}
			}

			// Dirty it for re-mesh if it happens to be resident right now
			// (MarkChunkDirtyForRemesh no-ops if untracked); EditedAncestorChunks
			// membership above is what guarantees correct routing on any
			// FUTURE load too, tracked or not.
			MarkChunkDirtyForRemesh(FVoxelLevelChunkKey{Level, Ancestor});

			UE_LOG(LogVoxelEdit, Log,
			       TEXT("Distant-edit mip propagation: level=%d chunk=(%d,%d,%d) tracked=%d %s"), Level, Ancestor.X, Ancestor.Y,
			       Ancestor.Z, ChunkRecords.Contains(FVoxelLevelChunkKey{Level, Ancestor}) ? 1 : 0,
			       bAlreadyEdited ? TEXT("(already edited-ancestor)") : TEXT("(newly marked edited-ancestor)"));
		}
	}
}

void FVoxelWorldImpl::ApplyGroupedEdits(std::unordered_map<vxc::BrickKey, std::vector<vxc::EditCell>, vxc::BrickKeyHash>& EditsByBrick,
                                         const TSet<VoxelCoords::FVoxelChunkKey>& DirtyChunks)
{
	if (EditsByBrick.empty())
	{
		return;
	}

	SCOPE_CYCLE_COUNTER(STAT_VoxelEditApply);
	for (auto& Entry : EditsByBrick)
	{
		Voxels.applyEdit(Entry.first, std::move(Entry.second));
		// docs/debug-tooling-plan.md P1 "vxc::Counters": one increment per
		// World::applyEdit call, matching the doc comment's existing "one
		// World::applyEdit per touched brick" invariant exactly.
		PerfCounters.incEditsApplied();
	}

	for (const VoxelCoords::FVoxelChunkKey& Key : DirtyChunks)
	{
		MarkChunkDirtyForRemesh(VoxelCoords::FVoxelLevelChunkKey{0, Key});
	}
	// M2 wave 2 item 2 ("Distant-edit mip propagation"): closes the wave-1
	// "R1+ never shows edits" limitation -- dirty every ancestor mip chunk
	// at every level whose footprint contains one of these level-0 edits.
	PropagateEditToMips(DirtyChunks);
	SortPendingQueues(LastAnchorLocation);
}

// --- M5 destruction (first slice, docs/voxel-earth-implementation-plan.md SS3.5) ---

int32 FVoxelWorldImpl::StampVoxels(const TArray<VoxelCoords::FVoxelCoord>& Coords, uint8 Material)
{
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;
	std::unordered_map<vxc::BrickKey, std::vector<vxc::EditCell>, vxc::BrickKeyHash> EditsByBrick;
	TSet<VoxelCoords::FVoxelChunkKey> DirtyChunks;
	for (const VoxelCoords::FVoxelCoord& V : Coords)
	{
		const vxc::BrickKey BKey = vxc::ChunkMap<B>::keyForVoxel(V.X, V.Y, V.Z);
		const int LocalX = (int)vxc::floorMod(V.X, B);
		const int LocalY = (int)vxc::floorMod(V.Y, B);
		const int LocalZ = (int)vxc::floorMod(V.Z, B);
		const uint16_t Cell = (uint16_t)vxc::Brick<B>::cellIndex(LocalX, LocalY, LocalZ);
		EditsByBrick[BKey].push_back(vxc::EditCell{Cell, (vxc::MaterialId)Material});
		CollectDirtyChunks(V.X, V.Y, V.Z, DirtyChunks);
	}
	if (EditsByBrick.empty())
	{
		return 0;
	}
	const int32 Count = Coords.Num();
	ApplyGroupedEdits(EditsByBrick, DirtyChunks);
	return Count;
}

int32 FVoxelWorldImpl::DetectAndRemoveIslands(const TArray<VoxelCoords::FVoxelCoord>& ClearedVoxels,
                                              TArray<TArray<VoxelCoords::FVoxelCoord>>& OutIslands, bool& bOutRegionClamped)
{
	bOutRegionClamped = false;
	if (ClearedVoxels.Num() == 0)
	{
		return 0;
	}

	// --- Analysis region ----------------------------------------------------
	// AABB of the just-cleared voxels, expanded into the volume where a
	// detached piece could live. A chop severs a piece ABOVE the cut, so we
	// grow generously upward (MarginZUp) to capture the whole piece and
	// modestly outward (MarginXY) so the piece sits INTERIOR to the region --
	// which is what keeps the anchor policy below safe. MarginZDown reaches
	// one layer below the cut, where the still-grounded stump/terrain sits.
	constexpr int64 MarginXY = 10;
	constexpr int64 MarginZDown = 3;
	constexpr int64 MarginZUp = 48;
	// Hard caps so a giant edit can never scan the world (connectivity.h is a
	// dense O(volume) scan). Clamping only ever SHRINKS the region, which under
	// the boundary anchor below can only anchor MORE voxels -- it can miss a
	// far-flung island but never falsely promote grounded terrain.
	constexpr int64 RegionMaxXY = 48;
	constexpr int64 RegionMaxZ = 128;

	int64 MinX = INT64_MAX, MinY = INT64_MAX, MinZ = INT64_MAX;
	int64 MaxX = INT64_MIN, MaxY = INT64_MIN, MaxZ = INT64_MIN;
	for (const VoxelCoords::FVoxelCoord& V : ClearedVoxels)
	{
		MinX = FMath::Min(MinX, V.X);
		MinY = FMath::Min(MinY, V.Y);
		MinZ = FMath::Min(MinZ, V.Z);
		MaxX = FMath::Max(MaxX, V.X);
		MaxY = FMath::Max(MaxY, V.Y);
		MaxZ = FMath::Max(MaxZ, V.Z);
	}

	vxc::VoxelCoord RegionMin{MinX - MarginXY, MinY - MarginXY, MinZ - MarginZDown};
	vxc::VoxelCoord RegionMax{MaxX + MarginXY, MaxY + MarginXY, MaxZ + MarginZUp};

	// Clamp each axis to its cap, keeping the cleared-AABB centre in view.
	auto ClampAxis = [&bOutRegionClamped](int64& Lo, int64& Hi, int64 Cap)
	{
		if (Hi - Lo + 1 > Cap)
		{
			const int64 Centre = (Lo + Hi) / 2;
			Lo = Centre - Cap / 2;
			Hi = Lo + Cap - 1;
			bOutRegionClamped = true;
		}
	};
	ClampAxis(RegionMin.x, RegionMax.x, RegionMaxXY);
	ClampAxis(RegionMin.y, RegionMax.y, RegionMaxXY);
	ClampAxis(RegionMin.z, RegionMax.z, RegionMaxZ);

	// If the edit is larger than the cap, the bounded region can no longer
	// CONTAIN the affected piece with margin -- the boundary-except-top anchor
	// below is only SOUND when the region fully surrounds the candidate island
	// (otherwise a big ragged carve produces hundreds of interior fragments and
	// terrain whose support merely exits the clamped box gets mis-flagged). So
	// a clamped region SKIPS detection entirely rather than run it unsoundly.
	// Consequence (documented v0 limitation, docs/status.md): large edits
	// (explosive craters) do NOT detach islands this slice -- structural
	// collapse of big spans is later M5 work. A controlled chop (the tree test,
	// a normal small dig) stays well under the cap and is fully handled.
	if (bOutRegionClamped)
	{
		UE_LOG(LogVoxelEdit, Log,
		       TEXT("Destruction: edit region exceeded the voxel-resolution cap (%lldx%lldx%lld) -- handing off to large-edit ")
		       TEXT("structural collapse (brick-resolution differential support)"),
		       (long long)RegionMaxXY, (long long)RegionMaxXY, (long long)RegionMaxZ);
		return DetectAndRemoveCollapse(ClearedVoxels, OutIslands);
	}

	// Ragged carves (jittered CarveSphere) can genuinely isolate a handful of
	// single voxels at the blast edge; promoting each to its own debris body is
	// noise, not gameplay. Only islands of at least this many voxels become
	// debris (smaller floating chips are left in the grid for this slice).
	constexpr int32 MinIslandVoxels = 16;
	// Safety valve so one pathological edit can never spawn an unbounded number
	// of debris actors.
	constexpr int32 MaxIslandsPerEdit = 16;

	// --- Connectivity flood-fill -------------------------------------------
	// solidFn: overlay-aware materialAt (edits already applied by the caller's
	// dig/carve, so the just-removed voxels read as air here).
	const auto SolidFn = [this](int64_t x, int64_t y, int64_t z) { return Voxels.materialAt(x, y, z) != vxc::MAT_AIR; };
	// anchorFn: a solid voxel is "grounded" if it touches ANY region boundary
	// face EXCEPT the top -- reaching a side/bottom face means it plausibly
	// continues into the standing world outside this bounded box (connectivity.h
	// warns that out-of-box voxels are never consulted). Only a piece fully
	// interior to the region -- exactly a severed chunk with margin around it --
	// counts as a floating island. This is strictly safer than the header's
	// bottomFaceAnchor: it never deletes terrain whose support exits sideways.
	const vxc::VoxelCoord RMin = RegionMin, RMax = RegionMax;
	const auto AnchorFn = [RMin, RMax](int64_t x, int64_t y, int64_t z)
	{
		return x == RMin.x || x == RMax.x || y == RMin.y || y == RMax.y || z == RMin.z; // NOT z == RMax.z (top)
	};

	const vxc::IslandAnalysis Analysis = vxc::findDisconnectedIslands(SolidFn, RegionMin, RegionMax, AnchorFn);

	UE_LOG(LogVoxelEdit, Log,
	       TEXT("Destruction: region=[(%lld,%lld,%lld)..(%lld,%lld,%lld)]%s components=%d anchored=%d islands=%d"),
	       (long long)RegionMin.x, (long long)RegionMin.y, (long long)RegionMin.z, (long long)RegionMax.x,
	       (long long)RegionMax.y, (long long)RegionMax.z, bOutRegionClamped ? TEXT(" (CLAMPED)") : TEXT(""),
	       Analysis.connectivity.componentCount, (int)Analysis.anchoredComponentIndices.size(),
	       (int)Analysis.islandComponentIndices.size());

	if (Analysis.islandComponentIndices.empty())
	{
		return 0;
	}

	// --- Remove each island from the authoritative grid (edit-log path) -----
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;
	int32 IslandCount = 0;
	int32 SkippedSmall = 0;
	for (int32 CompIdx : Analysis.islandComponentIndices)
	{
		const vxc::Component& Comp = Analysis.connectivity.components[CompIdx];
		if ((int32)Comp.voxels.size() < MinIslandVoxels)
		{
			++SkippedSmall;
			continue; // too small to be worth a debris body -- leave it in the grid
		}
		if (IslandCount >= MaxIslandsPerEdit)
		{
			UE_LOG(LogVoxelEdit, Log, TEXT("Destruction: island cap (%d) reached -- remaining islands left in place this edit"),
			       MaxIslandsPerEdit);
			break;
		}
		std::unordered_map<vxc::BrickKey, std::vector<vxc::EditCell>, vxc::BrickKeyHash> EditsByBrick;
		TSet<VoxelCoords::FVoxelChunkKey> DirtyChunks;
		TArray<VoxelCoords::FVoxelCoord> IslandCoords;
		IslandCoords.Reserve((int32)Comp.voxels.size());
		for (const vxc::VoxelCoord& V : Comp.voxels)
		{
			const vxc::BrickKey BKey = vxc::ChunkMap<B>::keyForVoxel(V.x, V.y, V.z);
			const int LocalX = (int)vxc::floorMod(V.x, B);
			const int LocalY = (int)vxc::floorMod(V.y, B);
			const int LocalZ = (int)vxc::floorMod(V.z, B);
			const uint16_t Cell = (uint16_t)vxc::Brick<B>::cellIndex(LocalX, LocalY, LocalZ);
			EditsByBrick[BKey].push_back(vxc::EditCell{Cell, vxc::MAT_AIR});
			CollectDirtyChunks(V.x, V.y, V.z, DirtyChunks);
			IslandCoords.Add(VoxelCoords::FVoxelCoord{V.x, V.y, V.z});
		}
		if (EditsByBrick.empty())
		{
			continue;
		}
		ApplyGroupedEdits(EditsByBrick, DirtyChunks);
		UE_LOG(LogVoxelEdit, Log, TEXT("Destruction: island %d promoted -> %d voxels removed from static grid (edit-log)"),
		       IslandCount, IslandCoords.Num());
		OutIslands.Add(MoveTemp(IslandCoords));
		++IslandCount;
	}
	if (SkippedSmall > 0)
	{
		UE_LOG(LogVoxelEdit, Log, TEXT("Destruction: %d sub-%d-voxel chip(s) left in grid (below MinIslandVoxels)"), SkippedSmall,
		       MinIslandVoxels);
	}
	return IslandCount;
}

// M5 LARGE-EDIT structural collapse. See the declaration above for what this
// is, and voxel-core/include/voxelcore/collapse.h for the model itself (coarse
// cells, support-with-a-lateral-budget, differential before/after, closure)
// plus the argument for why it stays sound at any region size where the
// voxel-resolution bounded box did not.
int32 FVoxelWorldImpl::DetectAndRemoveCollapse(const TArray<VoxelCoords::FVoxelCoord>& ClearedVoxels,
                                               TArray<TArray<VoxelCoords::FVoxelCoord>>& OutPieces)
{
	if (!CVarVoxelCollapseEnabled.GetValueOnGameThread())
	{
		UE_LOG(LogVoxelEdit, Log, TEXT("Collapse: voxel.Destruction.Collapse=0 -- large-edit collapse skipped"));
		return 0;
	}
	if (ClearedVoxels.Num() == 0)
	{
		return 0;
	}
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels; // 8 voxels = 0.8m = ONE coarse cell
	const double StartSeconds = FPlatformTime::Seconds();

	// --- Tunables (all documented in docs/status.md) -------------------------
	// Region margins, in BRICKS. Generous laterally and upward because the mass
	// that loses its support can extend well past the blast itself (a roof
	// whose pillars were blown out reaches far beyond the crater). Unlike the
	// voxel-resolution path, over-shrinking here is not a correctness problem,
	// only a recall problem -- see the clamp note below.
	constexpr int64 MarginBricksXY = 20;   // 16m
	constexpr int64 MarginBricksDown = 4;  // 3.2m
	constexpr int64 MarginBricksUp = 16;   // 12.8m
	// Hard region caps. 48x48x40 bricks = 38.4 x 38.4 x 32 m, 92160 cells --
	// two byte arrays + two int32 arrays over that is ~0.9MB, and the
	// heightfield prepass below is 48*48 brick columns * 64 amplifier column
	// evaluations = ~147k, the dominant cost (logged as prepassMs).
	constexpr int64 RegionMaxBricksXY = 48;
	constexpr int64 RegionMaxBricksZ = 40;
	// Cantilever budget: 6 cells = 4.8m of horizontal span from the nearest
	// thing carrying load. See collapse.h; larger is more conservative.
	constexpr int32 MaxLateralCells = 6;
	// Work valves. Cells first (bounds the support scan's output), then voxels
	// (bounds edit-log churn + re-mesh), both taken as a deterministic prefix
	// in VoxelCoordLess order so a truncated collapse is still identical on
	// every machine.
	constexpr int32 MaxCollapsingCells = 2048;   // 2048 bricks = up to ~1M voxels before the voxel cap bites
	constexpr int32 MaxCollapseVoxels = 150000;
	// Chips smaller than this are left in the grid rather than promoted --
	// same policy (and same number) as DetectAndRemoveIslands' MinIslandVoxels.
	constexpr int32 MinPieceVoxels = 16;

	// --- Analysis region, in brick coords -----------------------------------
	int64 MinVx = INT64_MAX, MinVy = INT64_MAX, MinVz = INT64_MAX;
	int64 MaxVx = INT64_MIN, MaxVy = INT64_MIN, MaxVz = INT64_MIN;
	for (const VoxelCoords::FVoxelCoord& V : ClearedVoxels)
	{
		MinVx = FMath::Min(MinVx, V.X); MaxVx = FMath::Max(MaxVx, V.X);
		MinVy = FMath::Min(MinVy, V.Y); MaxVy = FMath::Max(MaxVy, V.Y);
		MinVz = FMath::Min(MinVz, V.Z); MaxVz = FMath::Max(MaxVz, V.Z);
	}
	vxc::VoxelCoord MinCell{vxc::floorDiv(MinVx, B) - MarginBricksXY, vxc::floorDiv(MinVy, B) - MarginBricksXY,
	                        vxc::floorDiv(MinVz, B) - MarginBricksDown};
	vxc::VoxelCoord MaxCell{vxc::floorDiv(MaxVx, B) + MarginBricksXY, vxc::floorDiv(MaxVy, B) + MarginBricksXY,
	                        vxc::floorDiv(MaxVz, B) + MarginBricksUp};

	// Clamping is SAFE here, which is the whole point of this path. Shrinking
	// the region turns interior cells into boundary cells, and boundary cells
	// are anchored in BOTH the before pass and the after pass -- so they are
	// supported in both and can never satisfy the collapse predicate. Clamping
	// can therefore only ever REMOVE collapse decisions, never invent one.
	// (The voxel-resolution path could not say this: its anchor policy was only
	// sound while the box CONTAINED the candidate piece, which is exactly why
	// it had to bail out when it clamped.)
	bool bRegionClamped = false;
	auto ClampAxis = [&bRegionClamped](int64& Lo, int64& Hi, int64 Cap)
	{
		if (Hi - Lo + 1 > Cap)
		{
			const int64 Centre = (Lo + Hi) / 2;
			Lo = Centre - Cap / 2;
			Hi = Lo + Cap - 1;
			bRegionClamped = true;
		}
	};
	ClampAxis(MinCell.x, MaxCell.x, RegionMaxBricksXY);
	ClampAxis(MinCell.y, MaxCell.y, RegionMaxBricksXY);
	ClampAxis(MinCell.z, MaxCell.z, RegionMaxBricksZ);

	const int64 CX = MaxCell.x - MinCell.x + 1;
	const int64 CY = MaxCell.y - MinCell.y + 1;
	const int64 CZ = MaxCell.z - MinCell.z + 1;

	// --- Heightfield prepass ------------------------------------------------
	// The base (unedited) world is a pure heightfield: voxel (x,y,z) is solid
	// iff its centre z*kVoxelSizeMm + kVoxelSizeMm/2 is at or below that
	// column's surfaceMm (amplifier.h). So one Amplifier::column() evaluation
	// per voxel COLUMN answers occupancy for the whole vertical brick stack
	// above it -- 64 evaluations per brick column, reused for all CZ bricks and
	// again for the per-voxel extraction below, instead of the ~O(volume)
	// uncached column() calls a World::materialAt-per-voxel scan would cost.
	TArray<int32> ColumnSurfaceMm;
	ColumnSurfaceMm.SetNumUninitialized((int32)(CX * CY * B * B));
	TArray<int64> BrickColumnTopVz; // topmost solid voxel z over each brick column's 8x8 footprint
	BrickColumnTopVz.SetNumUninitialized((int32)(CX * CY));
	for (int64 Ly = 0; Ly < CY; ++Ly)
	{
		for (int64 Lx = 0; Lx < CX; ++Lx)
		{
			int64 TopMax = INT64_MIN;
			for (int32 Cy = 0; Cy < B; ++Cy)
			{
				for (int32 Cx = 0; Cx < B; ++Cx)
				{
					const int64 Vx = (MinCell.x + Lx) * B + Cx;
					const int64 Vy = (MinCell.y + Ly) * B + Cy;
					const int32 SurfMm = Voxels.amplifier().column(Vx, Vy).surfaceMm;
					ColumnSurfaceMm[(int32)(((Ly * CX) + Lx) * B * B + Cy * B + Cx)] = SurfMm;
					TopMax = FMath::Max(TopMax, vxc::floorDiv((int64)SurfMm - vxc::kVoxelSizeMm / 2, vxc::kVoxelSizeMm));
				}
			}
			BrickColumnTopVz[(int32)(Ly * CX + Lx)] = TopMax;
		}
	}
	const double PrepassSeconds = FPlatformTime::Seconds();

	// --- Occupancy predicates ----------------------------------------------
	// AFTER: the edit overlay wins where it exists (its brick bitset is exact
	// and already includes this edit's removals), otherwise the heightfield.
	const auto OccupiedAfter = [&](int64_t cx, int64_t cy, int64_t cz) -> bool
	{
		const vxc::BrickKey Key{(int32)cx, (int32)cy, (int32)cz};
		if (const vxc::Brick<B>* Br = Voxels.editedBricks().find(Key))
		{
			return !Br->empty();
		}
		const int64 Lx = cx - MinCell.x, Ly = cy - MinCell.y;
		if (Lx < 0 || Lx >= CX || Ly < 0 || Ly >= CY)
		{
			return false; // outside the prepass; never queried by computeSupport
		}
		return (int64)cz * B <= BrickColumnTopVz[(int32)(Ly * CX + Lx)];
	};
	// BEFORE: for a removal-only edit, pre-edit occupancy is exactly post-edit
	// occupancy PLUS every brick that contained one of the just-cleared voxels
	// (those bricks certainly had at least one solid voxel a moment ago). No
	// snapshot of the world is needed, only the cleared list the caller already
	// has -- this is what makes the differential rule cheap.
	TSet<FIntVector> ClearedBricks;
	ClearedBricks.Reserve(FMath::Min(ClearedVoxels.Num(), 4096));
	for (const VoxelCoords::FVoxelCoord& V : ClearedVoxels)
	{
		ClearedBricks.Add(FIntVector((int32)vxc::floorDiv(V.X, B), (int32)vxc::floorDiv(V.Y, B),
		                             (int32)vxc::floorDiv(V.Z, B)));
	}
	const auto OccupiedBefore = [&](int64_t cx, int64_t cy, int64_t cz) -> bool
	{
		if (ClearedBricks.Contains(FIntVector((int32)cx, (int32)cy, (int32)cz)))
		{
			return true;
		}
		return OccupiedAfter(cx, cy, cz);
	};

	// --- The decision -------------------------------------------------------
	vxc::CollapseParams Params;
	Params.maxLateralCells = MaxLateralCells;
	Params.maxCollapsingCells = MaxCollapsingCells;
	const vxc::CollapseAnalysis Analysis =
		vxc::findCollapsingCells(OccupiedBefore, OccupiedAfter, MinCell, MaxCell, Params);
	const double AnalysisSeconds = FPlatformTime::Seconds();

	vxc::Digest CellDigest;
	Analysis.digest(CellDigest);
	UE_LOG(LogVoxelEdit, Log,
	       TEXT("Collapse: region=[(%lld,%lld,%lld)..(%lld,%lld,%lld)] bricks (%lldx%lldx%lld)%s budget=%d ")
	       TEXT("occupiedAfter=%d supportedBefore=%d supportedAfter=%d collapsingCells=%d%s digest=0x%016llX"),
	       (long long)MinCell.x, (long long)MinCell.y, (long long)MinCell.z, (long long)MaxCell.x,
	       (long long)MaxCell.y, (long long)MaxCell.z, (long long)CX, (long long)CY, (long long)CZ,
	       bRegionClamped ? TEXT(" (CLAMPED - recall only, still sound)") : TEXT(""), MaxLateralCells,
	       Analysis.occupiedCellsAfter, Analysis.supportedCellsBefore, Analysis.supportedCellsAfter,
	       (int32)Analysis.collapsingCells.size(), Analysis.bTruncated ? TEXT(" (CELL CAP)") : TEXT(""),
	       (unsigned long long)CellDigest.h);

	if (Analysis.collapsingCells.empty())
	{
		UE_LOG(LogVoxelEdit, Log,
		       TEXT("Collapse: nothing lost support -- 0 pieces (prepass %.1fms, support %.1fms)"),
		       (PrepassSeconds - StartSeconds) * 1000.0, (AnalysisSeconds - PrepassSeconds) * 1000.0);
		return 0;
	}

	// --- Coarse cells -> the actual voxels to remove ------------------------
	// Only cells collapse; the voxels that come down are every still-solid
	// voxel inside them. Iterated in the cells' own VoxelCoordLess order so the
	// MaxCollapseVoxels cut is a deterministic prefix.
	std::vector<vxc::VoxelCoord> CollapsingVoxels;
	bool bVoxelCapHit = false;
	for (const vxc::VoxelCoord& Cell : Analysis.collapsingCells)
	{
		if ((int32)CollapsingVoxels.size() >= MaxCollapseVoxels)
		{
			bVoxelCapHit = true;
			break;
		}
		const vxc::BrickKey Key{(int32)Cell.x, (int32)Cell.y, (int32)Cell.z};
		const vxc::Brick<B>* Br = Voxels.editedBricks().find(Key);
		const int64 Lx = Cell.x - MinCell.x, Ly = Cell.y - MinCell.y;
		for (int32 Cz = 0; Cz < B; ++Cz)
		{
			for (int32 Cy = 0; Cy < B; ++Cy)
			{
				for (int32 Cx = 0; Cx < B; ++Cx)
				{
					const int64 Vz = Cell.z * B + Cz;
					bool bSolid;
					if (Br)
					{
						bSolid = Br->get(Cx, Cy, Cz) != vxc::MAT_AIR;
					}
					else
					{
						const int32 SurfMm = ColumnSurfaceMm[(int32)(((Ly * CX) + Lx) * B * B + Cy * B + Cx)];
						bSolid = (Vz * vxc::kVoxelSizeMm + vxc::kVoxelSizeMm / 2) <= (int64)SurfMm;
					}
					if (bSolid)
					{
						CollapsingVoxels.push_back(vxc::VoxelCoord{Cell.x * B + Cx, Cell.y * B + Cy, Vz});
					}
				}
			}
		}
	}

	// Split into physically separate falling pieces (a collapse usually breaks
	// into several). Hash-set BFS over the SET, not a dense mask over its
	// bounding box -- the set is sparse and can be large.
	const int32 CollapsingVoxelCount = (int32)CollapsingVoxels.size();
	std::vector<vxc::Component> Pieces = vxc::splitIntoComponents(std::move(CollapsingVoxels));
	vxc::Digest PieceDigest;
	vxc::digestComponents(Pieces, PieceDigest);

	// --- Authoritative removal (edit-log path) ------------------------------
	// ONE grouped apply for the whole collapse rather than one per piece: a big
	// collapse can touch hundreds of bricks and each ApplyGroupedEdits triggers
	// re-mesh bookkeeping.
	std::unordered_map<vxc::BrickKey, std::vector<vxc::EditCell>, vxc::BrickKeyHash> EditsByBrick;
	TSet<VoxelCoords::FVoxelChunkKey> DirtyChunks;
	int32 PieceCount = 0, SkippedSmall = 0, RemovedVoxels = 0;
	for (const vxc::Component& Piece : Pieces)
	{
		if ((int32)Piece.voxels.size() < MinPieceVoxels)
		{
			++SkippedSmall;
			continue; // chip -- leave it in the grid, same policy as MinIslandVoxels
		}
		TArray<VoxelCoords::FVoxelCoord> PieceCoords;
		PieceCoords.Reserve((int32)Piece.voxels.size());
		for (const vxc::VoxelCoord& V : Piece.voxels)
		{
			const vxc::BrickKey BKey = vxc::ChunkMap<B>::keyForVoxel(V.x, V.y, V.z);
			const int LocalX = (int)vxc::floorMod(V.x, B);
			const int LocalY = (int)vxc::floorMod(V.y, B);
			const int LocalZ = (int)vxc::floorMod(V.z, B);
			EditsByBrick[BKey].push_back(vxc::EditCell{(uint16_t)vxc::Brick<B>::cellIndex(LocalX, LocalY, LocalZ), vxc::MAT_AIR});
			CollectDirtyChunks(V.x, V.y, V.z, DirtyChunks);
			PieceCoords.Add(VoxelCoords::FVoxelCoord{V.x, V.y, V.z});
		}
		RemovedVoxels += PieceCoords.Num();
		OutPieces.Add(MoveTemp(PieceCoords));
		++PieceCount;
	}
	if (EditsByBrick.empty())
	{
		UE_LOG(LogVoxelEdit, Log, TEXT("Collapse: %d chip(s) below %d voxels only -- nothing removed"), SkippedSmall,
		       MinPieceVoxels);
		OutPieces.Reset();
		return 0;
	}
	ApplyGroupedEdits(EditsByBrick, DirtyChunks);

	const double EndSeconds = FPlatformTime::Seconds();
	UE_LOG(LogVoxelEdit, Log,
	       TEXT("Collapse: %d piece(s) from %d unsupported voxels, %d voxels removed from the static grid (edit-log)%s%s; ")
	       TEXT("%d chip(s) left; pieceDigest=0x%016llX; prepass %.1fms support %.1fms total %.1fms"),
	       PieceCount, CollapsingVoxelCount, RemovedVoxels, bVoxelCapHit ? TEXT(" (VOXEL CAP)") : TEXT(""),
	       Analysis.bTruncated ? TEXT(" (CELL CAP)") : TEXT(""), SkippedSmall,
	       (unsigned long long)PieceDigest.h, (PrepassSeconds - StartSeconds) * 1000.0,
	       (AnalysisSeconds - PrepassSeconds) * 1000.0, (EndSeconds - StartSeconds) * 1000.0);
	return PieceCount;
}

// --- debug-tooling helpers (docs/debug-tooling-plan.md P1) -----------------

void FVoxelWorldImpl::UpdatePerfSnapshot(float DeltaTime, float TickMs)
{
	// Per-frame collection into accumulators ("1Hz refresh of text, per-frame
	// collection"); the published snapshot (LastPerfSnapshot, read via
	// GetPerfSnapshot) only updates once the 1Hz window below rolls over.
	const float ThisTickSaturation = (LastAppliedFrac + LastRemeshFrac + LastUnloadFrac) / 3.f;
	BudgetSaturationAccum += ThisTickSaturation;
	++BudgetSaturationSamples;

	PerfRefreshAccumSeconds += DeltaTime;
	LastPerfSnapshot.SubsystemTickMs = TickMs; // always fresh; cheap, no reason to gate behind 1Hz

	if (PerfRefreshAccumSeconds < 1.0f)
	{
		return;
	}
	const float Window = PerfRefreshAccumSeconds;
	PerfRefreshAccumSeconds = 0.f;

	const int32 MaxJobsInFlight = 2 * FPlatformMisc::NumberOfCoresIncludingHyperthreads();

	LastPerfSnapshot.TotalChunksLoaded = TotalChunksLoaded;
	LastPerfSnapshot.TotalChunksUnloaded = TotalChunksUnloaded;
	LastPerfSnapshot.ChunksLoadedPerSec = float(TotalChunksLoaded - LoadedAtLastPerfRefresh) / Window;
	LastPerfSnapshot.ChunksUnloadedPerSec = float(TotalChunksUnloaded - UnloadedAtLastPerfRefresh) / Window;
	LoadedAtLastPerfRefresh = TotalChunksLoaded;
	UnloadedAtLastPerfRefresh = TotalChunksUnloaded;

	// M1 hitch-gap wave (component pooling): parked-pool size (current) +
	// reuse rate (events/sec over this window) + cumulative reuses since
	// startup -- same ChunksLoadedPerSec-style windowed-rate convention as
	// the streaming counters just above.
	LastPerfSnapshot.PooledComponents = ComponentPool.Num();
	LastPerfSnapshot.PoolReusesPerSec = float(TotalPoolReuses - PoolReusesAtLastPerfRefresh) / Window;
	LastPerfSnapshot.TotalPoolReuses = TotalPoolReuses;
	PoolReusesAtLastPerfRefresh = TotalPoolReuses;

	LastPerfSnapshot.JobsInFlight = JobsInFlightCounter.GetValue();
	LastPerfSnapshot.JobsInFlightCap = MaxJobsInFlight;
	LastPerfSnapshot.PendingJobQueueDepth = PendingJobKeys.Num();
	LastPerfSnapshot.PendingGameThreadQueueDepth = PendingGameThreadKeys.Num();
	LastPerfSnapshot.PendingUnloadQueueDepth = PendingUnloadKeys.Num();
	LastPerfSnapshot.StaleResultsDiscarded = StaleResultsDiscarded;

	LastPerfSnapshot.BudgetSaturationPct =
		BudgetSaturationSamples > 0 ? (BudgetSaturationAccum / float(BudgetSaturationSamples)) * 100.f : 0.f;
	BudgetSaturationAccum = 0.f;
	BudgetSaturationSamples = 0;

	// Worker mesh-job ms percentiles over the rolling window (sort a small
	// <=256-float copy once/sec -- negligible even though this runs whenever
	// voxel.Debug >= 1, not just under mode 2).
	if (WorkerJobMsWindowCount > 0)
	{
		TArray<float> Sorted;
		Sorted.Append(WorkerJobMsWindow.GetData(), WorkerJobMsWindowCount);
		Sorted.Sort();
		const int32 P50Index = FMath::Clamp(int32(Sorted.Num() * 0.50f), 0, Sorted.Num() - 1);
		const int32 P95Index = FMath::Clamp(int32(Sorted.Num() * 0.95f), 0, Sorted.Num() - 1);
		LastPerfSnapshot.WorkerMsP50 = Sorted[P50Index];
		LastPerfSnapshot.WorkerMsP95 = Sorted[P95Index];
		LastPerfSnapshot.WorkerMsMax = Sorted.Last();
	}
	else
	{
		LastPerfSnapshot.WorkerMsP50 = LastPerfSnapshot.WorkerMsP95 = LastPerfSnapshot.WorkerMsMax = 0.f;
	}

	// M2 wave 2 item 1: identical percentile computation, per level (report
	// requirement: "worker p50/p95 per level" -- this is the number the
	// cross-job mip cache targets).
	for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
	{
		if (LevelWorkerJobMsWindowCount[Level] > 0)
		{
			TArray<float> Sorted;
			Sorted.Append(LevelWorkerJobMsWindow[Level].GetData(), LevelWorkerJobMsWindowCount[Level]);
			Sorted.Sort();
			const int32 P50Index = FMath::Clamp(int32(Sorted.Num() * 0.50f), 0, Sorted.Num() - 1);
			const int32 P95Index = FMath::Clamp(int32(Sorted.Num() * 0.95f), 0, Sorted.Num() - 1);
			LastPerfSnapshot.LevelWorkerMsP50[Level] = Sorted[P50Index];
			LastPerfSnapshot.LevelWorkerMsP95[Level] = Sorted[P95Index];
		}
		else
		{
			LastPerfSnapshot.LevelWorkerMsP50[Level] = LastPerfSnapshot.LevelWorkerMsP95[Level] = 0.f;
		}
	}

	// M2 wave 2 item 1: shared cross-job mip cache memory (FSharedMipCache).
	// MipCacheEvictions: M2 task "Mip cache eviction" LRU-budget counter.
	LastPerfSnapshot.MipCacheBrickCount = SharedMipCache.GetBrickCount();
	LastPerfSnapshot.MipCacheBytes = SharedMipCache.GetBytesUsed();
	LastPerfSnapshot.MipCacheEvictions = SharedMipCache.GetEvictionsCount();

	int32 ResidentComponents = 0;
	// M2 item 1: "Per-level loaded/pending counters into the perf
	// snapshot/HUD." Loaded = has a live component right now; pending =
	// queued across all three pending arrays (job dispatch, game-thread
	// mesh, unload) for that level.
	int32 LevelLoaded[VoxelCoords::kNumLevels] = {};
	int32 LevelPending[VoxelCoords::kNumLevels] = {};
	for (const auto& Pair : ChunkRecords)
	{
		if (Pair.Value.Component.IsValid())
		{
			++ResidentComponents;
			++LevelLoaded[FMath::Clamp(Pair.Key.Level, 0, VoxelCoords::kNumLevels - 1)];
		}
	}
	for (const VoxelCoords::FVoxelLevelChunkKey& K : PendingJobKeys)
	{
		++LevelPending[FMath::Clamp(K.Level, 0, VoxelCoords::kNumLevels - 1)];
	}
	for (const VoxelCoords::FVoxelLevelChunkKey& K : PendingGameThreadKeys)
	{
		++LevelPending[FMath::Clamp(K.Level, 0, VoxelCoords::kNumLevels - 1)];
	}
	for (const VoxelCoords::FVoxelLevelChunkKey& K : PendingUnloadKeys)
	{
		++LevelPending[FMath::Clamp(K.Level, 0, VoxelCoords::kNumLevels - 1)];
	}
	for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
	{
		LastPerfSnapshot.LevelLoadedCount[Level] = LevelLoaded[Level];
		LastPerfSnapshot.LevelPendingCount[Level] = LevelPending[Level];
	}
	LastPerfSnapshot.ResidentComponents = ResidentComponents;
	LastPerfSnapshot.ResidentQuads = ResidentQuads;
	LastPerfSnapshot.OverlayBrickCount = int64(Voxels.editedBricks().size());
	LastPerfSnapshot.EditLogEntries = int64(Voxels.log().size());

	LastPerfSnapshot.BricksGenerated = PerfCounters.getBricksGenerated();
	LastPerfSnapshot.CellsWritten = PerfCounters.getCellsWritten();
	LastPerfSnapshot.QuadsEmitted = PerfCounters.getQuadsEmitted();
	LastPerfSnapshot.EditsApplied = PerfCounters.getEditsApplied();
	LastPerfSnapshot.ColumnEvals = PerfCounters.getColumnEvals();
}

void FVoxelWorldImpl::UpdateChunkStateTints()
{
	// docs/debug-tooling-plan.md P1 chunk-state tints: just-loaded flash blue
	// (1s decay), edited-chunk orange (persistent while it owns overlay
	// bricks), game-thread re-mesh flash purple (1s decay). Whichever flash
	// is more recent (higher decay alpha) wins over the persistent base tint;
	// the two flashes are mutually exclusive in practice (LoadedAtSeconds only
	// fires on first creation, RemeshedAtSeconds only on an already-resident
	// chunk) but a fresh chunk that's edited again within the same second
	// could see both windows open at once, so pick explicitly rather than
	// relying on ordering.
	static const FLinearColor BlueFlash(0.1f, 0.4f, 1.0f, 1.0f);
	static const FLinearColor PurpleFlash(0.6f, 0.05f, 0.9f, 1.0f);
	static const FLinearColor OrangePersistent(1.0f, 0.45f, 0.05f, 1.0f);
	static const FLinearColor White(1.0f, 1.0f, 1.0f, 1.0f);
	constexpr float FlashDecaySeconds = 1.0f;

	for (auto& Pair : ChunkRecords)
	{
		UVoxelChunkComponent* Comp = Pair.Value.Component.Get();
		if (!Comp)
		{
			continue;
		}
		const VoxelStreaming::FChunkRecord& Rec = Pair.Value;
		const FLinearColor BaseTint = Rec.bHasOverlayBricks ? OrangePersistent : White;

		const float SinceLoaded = ElapsedSeconds - Rec.LoadedAtSeconds;
		const float SinceRemeshed = ElapsedSeconds - Rec.RemeshedAtSeconds;
		const float LoadedAlpha = (SinceLoaded >= 0.f && SinceLoaded < FlashDecaySeconds) ? (1.f - SinceLoaded / FlashDecaySeconds) : 0.f;
		const float RemeshedAlpha = (SinceRemeshed >= 0.f && SinceRemeshed < FlashDecaySeconds) ? (1.f - SinceRemeshed / FlashDecaySeconds) : 0.f;

		FLinearColor Tint = BaseTint;
		if (LoadedAlpha > 0.f || RemeshedAlpha > 0.f)
		{
			const bool bLoadedWins = LoadedAlpha >= RemeshedAlpha;
			Tint = FMath::Lerp(BaseTint, bLoadedWins ? BlueFlash : PurpleFlash, bLoadedWins ? LoadedAlpha : RemeshedAlpha);
		}

		Comp->SetDebugTint(Tint);
	}
}

void FVoxelWorldImpl::UpdateRingTints()
{
	// docs/m2-plan.md first implementation wave item 4: "when voxel.Debug
	// >= 2 and voxel.Debug.Rings is set, tint components by level." Simple
	// and unconditional -- unlike UpdateChunkStateTints there is no flash/
	// decay here, just a flat per-level color.
	for (auto& Pair : ChunkRecords)
	{
		if (UVoxelChunkComponent* Comp = Pair.Value.Component.Get())
		{
			Comp->SetDebugTint(VoxelDebug::RingLevelTint(Pair.Key.Level));
		}
	}
}

void FVoxelWorldImpl::DrawDebugBoundsLayer(UWorld& World, const FVector& Anchor) const
{
	if (!VoxelDebug::IsBoundsEnabled())
	{
		return;
	}

	// Cap ~200 nearest tracked chunks (docs/debug-tooling-plan.md P1 "Bounds
	// layer"). ChunkRecords can hold several thousand entries at M1 streaming
	// radii; the full sort below only runs while this layer is switched on.
	struct FEntry
	{
		VoxelCoords::FVoxelLevelChunkKey Key;
		double DistSq;
	};
	TArray<FEntry> Entries;
	Entries.Reserve(ChunkRecords.Num());
	for (const auto& Pair : ChunkRecords)
	{
		const double ChunkEdge = VoxelCoords::ChunkEdgeUUForLevel(Pair.Key.Level);
		const FVector Center = VoxelCoords::ChunkOriginWorldForLevel(Pair.Key.Key, Pair.Key.Level) + FVector(ChunkEdge * 0.5);
		Entries.Add(FEntry{Pair.Key, FVector::DistSquared(Center, Anchor)});
	}
	Entries.Sort([](const FEntry& A, const FEntry& B) { return A.DistSq < B.DistSq; });

	constexpr int32 MaxBoxes = 200;
	const int32 Count = FMath::Min(MaxBoxes, Entries.Num());
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const VoxelCoords::FVoxelLevelChunkKey& LevelKey = Entries[Index].Key;
		const float Extent = float(VoxelCoords::ChunkEdgeUUForLevel(LevelKey.Level)) * 0.5f;
		const FVector Center = VoxelCoords::ChunkOriginWorldForLevel(LevelKey.Key, LevelKey.Level) + FVector(Extent);
		DrawDebugBox(&World, Center, FVector(Extent), FColor::Cyan, /*bPersistentLines*/ false, /*LifeTime*/ -1.f,
		             /*DepthPriority*/ 0, /*Thickness*/ 1.5f);
	}
}

bool FVoxelWorldImpl::TryDig(const FVector& CameraLoc, const FVector& CameraDir, int32 SizeVoxels, FEditsByBrick* OutPredicted)
{
	const FVector Dir = CameraDir.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		return false;
	}

	const vxc::RaycastHit Hit = CastFromCamera(CameraLoc, Dir);
	if (!Hit.hit)
	{
		return false;
	}

	const int32 N = FMath::Clamp(SizeVoxels, UVoxelWorldSubsystem::MinCubeSizeVoxels, UVoxelWorldSubsystem::MaxCubeSizeVoxels);
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;

	// Cube biased into the terrain (m1-plan.md "Dig sizes" row): grow along
	// the hit-face axis in the SAME direction the ray was travelling when it
	// entered the solid voxel (Hit.faceSign) -- that direction points deeper
	// into the surface, away from the camera.
	int64 MinX, MinY, MinZ;
	ComputeCubeMinCorner(Hit.vx, Hit.vy, Hit.vz, Hit.faceAxis, Hit.faceSign, N, MinX, MinY, MinZ);

	// One World::applyEdit per touched brick (docs/m1-plan.md Stage 2
	// decisions table): collect every voxel in the cube, grouped by brick.
	std::unordered_map<vxc::BrickKey, std::vector<vxc::EditCell>, vxc::BrickKeyHash> EditsByBrick;
	TSet<VoxelCoords::FVoxelChunkKey> DirtyChunks;

	for (int64 Dz = 0; Dz < N; ++Dz)
	{
		for (int64 Dy = 0; Dy < N; ++Dy)
		{
			for (int64 Dx = 0; Dx < N; ++Dx)
			{
				const int64 Vx = MinX + Dx, Vy = MinY + Dy, Vz = MinZ + Dz;
				const vxc::BrickKey BKey = vxc::ChunkMap<B>::keyForVoxel(Vx, Vy, Vz);
				const int LocalX = (int)vxc::floorMod(Vx, B);
				const int LocalY = (int)vxc::floorMod(Vy, B);
				const int LocalZ = (int)vxc::floorMod(Vz, B);
				const uint16_t Cell = (uint16_t)vxc::Brick<B>::cellIndex(LocalX, LocalY, LocalZ);
				EditsByBrick[BKey].push_back(vxc::EditCell{Cell, vxc::MAT_AIR});

				CollectDirtyChunks(Vx, Vy, Vz, DirtyChunks);
			}
		}
	}

	if (EditsByBrick.empty())
	{
		return false;
	}
	if (OutPredicted)
	{
		*OutPredicted = EditsByBrick; // copy before ApplyGroupedEdits moves the cells out (M3 wave 1 prediction tracking)
	}
	ApplyGroupedEdits(EditsByBrick, DirtyChunks);
	return true;
}

bool FVoxelWorldImpl::TryPlace(const FVector& CameraLoc, const FVector& CameraDir, int32 SizeVoxels, uint8 MaterialId,
                                const FVector& PlayerActorLocation, FEditsByBrick* OutPredicted)
{
	const FVector Dir = CameraDir.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		return false;
	}

	const vxc::RaycastHit Hit = CastFromCamera(CameraLoc, Dir);
	if (!Hit.hit || Hit.faceAxis < 0)
	{
		return false; // no hit, or the ray started inside solid (no valid face)
	}

	const int32 N = FMath::Clamp(SizeVoxels, UVoxelWorldSubsystem::MinCubeSizeVoxels, UVoxelWorldSubsystem::MaxCubeSizeVoxels);
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;

	// Cube grid-snapped against the hit face, biased AWAY from the surface
	// (m1-plan.md "Place" row): grow along the face axis opposite the ray's
	// entry direction (-Hit.faceSign), starting from Hit.px/py/pz (already
	// the empty voxel adjacent to the face) -- mirrors TryDig's bias.
	int64 MinX, MinY, MinZ;
	ComputeCubeMinCorner(Hit.px, Hit.py, Hit.pz, Hit.faceAxis, -Hit.faceSign, N, MinX, MinY, MinZ);

	// Reject if the placement cube would overlap the player's collision box
	// (m1-plan.md "Place" row: "Placement must not intersect the player's
	// collision box"). Box half-extents mirror AVoxelEarthFlyPawn's walk-mode
	// collision box (60x60x180 UU) -- duplicated here (rather than reaching
	// into the pawn) since the subsystem must not depend on a specific pawn
	// class.
	constexpr double PlayerHalfExtentXYUU = 30.0;
	constexpr double PlayerHalfExtentZUU = 90.0;
	const FVector CubeMinUU = VoxelCoords::VoxelToWorldCorner(VoxelCoords::FVoxelCoord{MinX, MinY, MinZ});
	const FVector CubeMaxUU =
		VoxelCoords::VoxelToWorldCorner(VoxelCoords::FVoxelCoord{MinX + N, MinY + N, MinZ + N});
	const FVector PlayerMinUU = PlayerActorLocation - FVector(PlayerHalfExtentXYUU, PlayerHalfExtentXYUU, PlayerHalfExtentZUU);
	const FVector PlayerMaxUU = PlayerActorLocation + FVector(PlayerHalfExtentXYUU, PlayerHalfExtentXYUU, PlayerHalfExtentZUU);
	const bool bOverlapsPlayer = CubeMinUU.X < PlayerMaxUU.X && CubeMaxUU.X > PlayerMinUU.X && CubeMinUU.Y < PlayerMaxUU.Y &&
	                              CubeMaxUU.Y > PlayerMinUU.Y && CubeMinUU.Z < PlayerMaxUU.Z && CubeMaxUU.Z > PlayerMinUU.Z;
	if (bOverlapsPlayer)
	{
		// docs/debug-tooling-plan.md P1 "Log split": edit-log rejections move
		// to LogVoxelEdit.
		UE_LOG(LogVoxelEdit, Log, TEXT("TryPlace rejected: %dx%dx%d cube at (%lld,%lld,%lld) would intersect the player."),
		       N, N, N, (long long)MinX, (long long)MinY, (long long)MinZ);
		return false;
	}

	// One World::applyEdit per touched brick, same grouping as TryDig.
	std::unordered_map<vxc::BrickKey, std::vector<vxc::EditCell>, vxc::BrickKeyHash> EditsByBrick;
	TSet<VoxelCoords::FVoxelChunkKey> DirtyChunks;

	for (int64 Dz = 0; Dz < N; ++Dz)
	{
		for (int64 Dy = 0; Dy < N; ++Dy)
		{
			for (int64 Dx = 0; Dx < N; ++Dx)
			{
				const int64 Vx = MinX + Dx, Vy = MinY + Dy, Vz = MinZ + Dz;
				const vxc::BrickKey BKey = vxc::ChunkMap<B>::keyForVoxel(Vx, Vy, Vz);
				const int LocalX = (int)vxc::floorMod(Vx, B);
				const int LocalY = (int)vxc::floorMod(Vy, B);
				const int LocalZ = (int)vxc::floorMod(Vz, B);
				const uint16_t Cell = (uint16_t)vxc::Brick<B>::cellIndex(LocalX, LocalY, LocalZ);
				EditsByBrick[BKey].push_back(vxc::EditCell{Cell, (vxc::MaterialId)MaterialId});

				CollectDirtyChunks(Vx, Vy, Vz, DirtyChunks);
			}
		}
	}

	if (EditsByBrick.empty())
	{
		return false;
	}
	if (OutPredicted)
	{
		*OutPredicted = EditsByBrick; // see TryDig's identical comment above
	}
	ApplyGroupedEdits(EditsByBrick, DirtyChunks);
	return true;
}

int32 FVoxelWorldImpl::CarveSphere(const FVector& CenterUU, double RadiusUU, double JitterUU, FEditsByBrick* OutPredicted)
{
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;
	const double VoxelSize = VoxelCoords::VoxelSizeUU;
	const uint64 Seed = Voxels.amplifier().seed();
	const double MaxExtentUU = RadiusUU + FMath::Abs(JitterUU);

	// Bounding box of voxels that could possibly be inside RadiusUU +
	// JitterUU of CenterUU, in voxel-lattice coordinates.
	const int64 VXMin = (int64)FMath::FloorToDouble((CenterUU.X - MaxExtentUU) / VoxelSize);
	const int64 VXMax = (int64)FMath::CeilToDouble((CenterUU.X + MaxExtentUU) / VoxelSize);
	const int64 VYMin = (int64)FMath::FloorToDouble((CenterUU.Y - MaxExtentUU) / VoxelSize);
	const int64 VYMax = (int64)FMath::CeilToDouble((CenterUU.Y + MaxExtentUU) / VoxelSize);
	const int64 VZMin = (int64)FMath::FloorToDouble((CenterUU.Z - MaxExtentUU) / VoxelSize);
	const int64 VZMax = (int64)FMath::CeilToDouble((CenterUU.Z + MaxExtentUU) / VoxelSize);

	// One World::applyEdit per touched brick, same grouping as TryDig/TryPlace.
	std::unordered_map<vxc::BrickKey, std::vector<vxc::EditCell>, vxc::BrickKeyHash> EditsByBrick;
	TSet<VoxelCoords::FVoxelChunkKey> DirtyChunks;
	int32 RemovedCount = 0;

	for (int64 Vz = VZMin; Vz <= VZMax; ++Vz)
	{
		for (int64 Vy = VYMin; Vy <= VYMax; ++Vy)
		{
			for (int64 Vx = VXMin; Vx <= VXMax; ++Vx)
			{
				const FVector VoxelCenterUU = VoxelCoords::VoxelToWorldCenter(VoxelCoords::FVoxelCoord{Vx, Vy, Vz});
				const double DistUU = (VoxelCenterUU - CenterUU).Size();

				// Deterministic per-voxel jitter (m1-plan.md "Explosives v1"
				// row): vxc::hash3(seed, vx,vy,vz, channel 40), top 16 bits
				// signed and scaled into [-JitterUU, +JitterUU] -- gives a
				// ragged, reproducible blast edge instead of a perfect sphere.
				const uint64 H = vxc::hash3(Seed, Vx, Vy, Vz, /*channel*/ 40);
				const double JitterHereUU = (double(vxc::hashToSigned16(H)) / 32768.0) * JitterUU;
				if (DistUU >= RadiusUU + JitterHereUU)
				{
					continue;
				}

				if (Voxels.materialAt(Vx, Vy, Vz) == vxc::MAT_AIR)
				{
					continue; // nothing to remove; also keeps the edit-log entry minimal
				}

				const vxc::BrickKey BKey = vxc::ChunkMap<B>::keyForVoxel(Vx, Vy, Vz);
				const int LocalX = (int)vxc::floorMod(Vx, B);
				const int LocalY = (int)vxc::floorMod(Vy, B);
				const int LocalZ = (int)vxc::floorMod(Vz, B);
				const uint16_t Cell = (uint16_t)vxc::Brick<B>::cellIndex(LocalX, LocalY, LocalZ);
				EditsByBrick[BKey].push_back(vxc::EditCell{Cell, vxc::MAT_AIR});

				CollectDirtyChunks(Vx, Vy, Vz, DirtyChunks);
				++RemovedCount;
			}
		}
	}

	if (OutPredicted)
	{
		*OutPredicted = EditsByBrick;
	}
	ApplyGroupedEdits(EditsByBrick, DirtyChunks);
	return RemovedCount;
}

void FVoxelWorldImpl::ReconcilePrediction(const vxc::BrickKey& Key, const std::vector<vxc::EditCell>& ConfirmedCells)
{
	auto It = PendingPredictedCellsByBrick.find(Key);
	if (It == PendingPredictedCellsByBrick.end())
	{
		return; // not one of our own predictions (another player's edit, or already confirmed)
	}
	if (It->second == ConfirmedCells)
	{
		// Exact match: deterministic derivation means byte-identical results
		// -- silent confirmation, no correction needed.
		PendingPredictedCellsByBrick.erase(It);
		return;
	}
	UE_LOG(LogVoxelEdit, Warning,
	       TEXT("Prediction reconcile: brick (%d,%d,%d) server entry differs from local prediction -- applying ")
	       TEXT("server truth for the confirmed cells (brick-granularity v1, docs/m3-plan.md 'Prediction reconcile v1')."),
	       Key.x, Key.y, Key.z);
	PendingPredictedCellsByBrick.erase(It);
}

void FVoxelWorldImpl::ApplyReplicatedEntries(const std::vector<vxc::EditEntry>& Entries)
{
	if (Entries.empty())
	{
		return;
	}

	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;
	FEditsByBrick EditsByBrick;
	TSet<VoxelCoords::FVoxelChunkKey> DirtyChunks;

	for (const vxc::EditEntry& E : Entries)
	{
		ReconcilePrediction(E.key, E.cells);

		std::vector<vxc::EditCell>& Cells = EditsByBrick[E.key];
		Cells.insert(Cells.end(), E.cells.begin(), E.cells.end());

		for (const vxc::EditCell& C : E.cells)
		{
			const int LocalX = C.cell % B, LocalY = (C.cell / B) % B, LocalZ = C.cell / (B * B);
			const int64 Vx = int64(E.key.x) * B + LocalX;
			const int64 Vy = int64(E.key.y) * B + LocalY;
			const int64 Vz = int64(E.key.z) * B + LocalZ;
			CollectDirtyChunks(Vx, Vy, Vz, DirtyChunks);
		}
	}
	// Reuses the exact same tail every local edit uses (docs/m3-plan.md
	// doctrine: "one authority path for world changes") -- World::applyEdit
	// per brick, dirty-chunk marking, mip propagation, pending-queue sort.
	ApplyGroupedEdits(EditsByBrick, DirtyChunks);
}

// M3 wave 1 role-split helpers (docs/m3-plan.md "Authority flow"/"Prediction").
// Free functions (not FVoxelWorldImpl/UVoxelWorldSubsystem members) since they
// need UWorld&/actor access FVoxelWorldImpl deliberately never has, and the
// UHT-parsed UVoxelWorldSubsystem header stays untouched (the role split is
// entirely an implementation detail behind TryDig/TryPlace/CarveSphere's
// unchanged public signatures).
namespace
{
// NM_Client path: apply the SAME cells locally right now (today's overlay-
// apply path, so digging/placing/carving still feels instant), remember them
// as a pending prediction, then forward the identical intent to the server
// through the local player's PlayerController -- the ONE actor guaranteed to
// be owned by this client's connection (see VoxelEditRelay.h's class comment
// for why the shared, unowned AVoxelEditRelay can't receive client-called
// Server RPCs itself).
bool TryDigReplica(FVoxelWorldImpl& Impl, UWorld& World, const FVector& CameraLoc, const FVector& CameraDir, int32 SizeVoxels)
{
	FEditsByBrick Predicted;
	const bool bApplied = Impl.TryDig(CameraLoc, CameraDir, SizeVoxels, &Predicted);
	if (!bApplied)
	{
		return false;
	}
	for (auto& Entry : Predicted)
	{
		Impl.PendingPredictedCellsByBrick[Entry.first] = Entry.second;
	}
	if (AVoxelEarthPlayerController* PC = Cast<AVoxelEarthPlayerController>(World.GetFirstPlayerController()))
	{
		PC->ServerSubmitDigIntent(CameraLoc, CameraDir, SizeVoxels);
	}
	return true;
}

bool TryPlaceReplica(FVoxelWorldImpl& Impl, UWorld& World, const FVector& CameraLoc, const FVector& CameraDir, int32 SizeVoxels,
                      uint8 MaterialId, const FVector& PlayerActorLocation)
{
	FEditsByBrick Predicted;
	const bool bApplied = Impl.TryPlace(CameraLoc, CameraDir, SizeVoxels, MaterialId, PlayerActorLocation, &Predicted);
	if (!bApplied)
	{
		return false;
	}
	for (auto& Entry : Predicted)
	{
		Impl.PendingPredictedCellsByBrick[Entry.first] = Entry.second;
	}
	if (AVoxelEarthPlayerController* PC = Cast<AVoxelEarthPlayerController>(World.GetFirstPlayerController()))
	{
		PC->ServerSubmitPlaceIntent(CameraLoc, CameraDir, SizeVoxels, MaterialId, PlayerActorLocation);
	}
	return true;
}

int32 CarveSphereReplica(FVoxelWorldImpl& Impl, UWorld& World, const FVector& CenterUU, double RadiusUU, double JitterUU)
{
	FEditsByBrick Predicted;
	const int32 Removed = Impl.CarveSphere(CenterUU, RadiusUU, JitterUU, &Predicted);
	if (Removed <= 0)
	{
		return Removed;
	}
	for (auto& Entry : Predicted)
	{
		Impl.PendingPredictedCellsByBrick[Entry.first] = Entry.second;
	}
	if (AVoxelEarthPlayerController* PC = Cast<AVoxelEarthPlayerController>(World.GetFirstPlayerController()))
	{
		PC->ServerSubmitCarveIntent(CenterUU, (float)RadiusUU, (float)JitterUU);
	}
	return Removed;
}

// The relay is a single, world-scoped, always-relevant actor (see
// VoxelEditRelay.h) -- a plain iterator lookup is fine at dig/place/carve
// rate (not a per-frame hot path).
AVoxelEditRelay* FindEditRelay(UWorld& World)
{
	for (TActorIterator<AVoxelEditRelay> It(&World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

// Authority path tail: after a local (or client-forwarded) authoritative
// edit lands, broadcast whatever is new in the log since the last broadcast
// (usually exactly what the edit just appended) to every client.
void BroadcastNewEntries(FVoxelWorldImpl& Impl, UWorld& World)
{
	const uint64 CurrentSize = Impl.Voxels.log().size();
	if (CurrentSize <= Impl.LastBroadcastSeq)
	{
		return;
	}
	const auto& AllEntries = Impl.Voxels.log().entries();
	std::vector<vxc::EditEntry> Slice(AllEntries.begin() + static_cast<std::ptrdiff_t>(Impl.LastBroadcastSeq), AllEntries.end());
	Impl.LastBroadcastSeq = CurrentSize;

	TArray<uint8> Bytes;
	SerializeEntries(Slice, Bytes);

	if (AVoxelEditRelay* Relay = FindEditRelay(World))
	{
		Relay->MulticastAppliedEntries(Bytes);
	}
	else
	{
		UE_LOG(LogVoxelEdit, Warning, TEXT("BroadcastNewEntries: no AVoxelEditRelay in the world -- %d entries not broadcast."),
		       (int32)Slice.size());
	}
}

// M5 destruction (first slice): the chop hook. Runs after an authoritative
// solid-removing edit. Detects disconnected islands in the affected region and
// removes each from the authoritative grid (deterministic + edit-log, done
// inside DetectAndRemoveIslands -- the AUTHORITATIVE half), then spawns one
// cosmetic AVoxelDebris body per island (the COSMETIC half; skipped on a
// dedicated server, which has no viewport). MUST be called BEFORE
// BroadcastNewEntries so the island-removal entries replicate together with the
// triggering dig. See docs/status.md "M5 -- Destruction" for the split.
void PromoteDetachedIslands(FVoxelWorldImpl& Impl, UWorld& World, const TArray<VoxelCoords::FVoxelCoord>& ClearedVoxels)
{
	if (!CVarVoxelDestructionEnabled.GetValueOnGameThread())
	{
		return;
	}
	TArray<TArray<VoxelCoords::FVoxelCoord>> Islands;
	bool bRegionClamped = false;
	const int32 IslandCount = Impl.DetectAndRemoveIslands(ClearedVoxels, Islands, bRegionClamped);
	if (IslandCount <= 0)
	{
		return;
	}

	// W2 fix (ADR-0003 item 2 / this task's collapse-notification gap):
	// island promotion AND large-edit structural collapse (DetectAndRemoveCollapse
	// funnels its own removed pieces into this SAME Islands array -- see its
	// call site inside DetectAndRemoveIslands) both remove voxels from the
	// authoritative grid via the edit-log path, exactly like a dig, but water
	// previously never learned about it -- only the triggering dig/carve's own
	// ClearedVoxels was ever notified. Flatten every piece's coordinates once
	// and notify BOTH hooks: the solid->air reservoir/breach path (these
	// voxels are always MAT_AIR writes, same as a dig) and the general
	// solidity-memo invalidation path (one bounding-box call, not one per
	// voxel -- a collapse can remove up to 150,000 voxels). Authority-only by
	// construction: PromoteDetachedIslands is only ever called from TryDig/
	// CarveSphere's own NetMode!=NM_Client branches.
	if (UVoxelWaterSubsystem* WaterSubsystem = World.GetSubsystem<UVoxelWaterSubsystem>())
	{
		TArray<VoxelCoords::FVoxelCoord> RemovedVoxels;
		int64 MinX = INT64_MAX, MinY = INT64_MAX, MinZ = INT64_MAX;
		int64 MaxX = INT64_MIN, MaxY = INT64_MIN, MaxZ = INT64_MIN;
		for (const TArray<VoxelCoords::FVoxelCoord>& Piece : Islands)
		{
			for (const VoxelCoords::FVoxelCoord& V : Piece)
			{
				RemovedVoxels.Add(V);
				MinX = FMath::Min(MinX, V.X); MaxX = FMath::Max(MaxX, V.X);
				MinY = FMath::Min(MinY, V.Y); MaxY = FMath::Max(MaxY, V.Y);
				MinZ = FMath::Min(MinZ, V.Z); MaxZ = FMath::Max(MaxZ, V.Z);
			}
		}
		if (RemovedVoxels.Num() > 0)
		{
			WaterSubsystem->NotifyTerrainVoxelsCleared(RemovedVoxels);
			WaterSubsystem->NotifyTerrainRegionEdited(VoxelCoords::FVoxelCoord{MinX, MinY, MinZ},
			                                          VoxelCoords::FVoxelCoord{MaxX, MaxY, MaxZ});
		}
	}

	// Cosmetic debris bodies: only where there is something to look at. A
	// dedicated server has already done the authoritative half above (the
	// voxels ARE gone from the grid and those removals replicate); it just
	// spawns no Chaos body, so there is nothing here that could desync.
	if (World.GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	// --- Debris caps (docs/status.md "Structural collapse (M5, large-edit)") -
	// A chop detaches one canopy; a large collapse can shatter into dozens of
	// pieces totalling six figures of voxels, and a thousand Chaos bodies (or a
	// million ISM instances) would tank the frame for a purely decorative
	// effect. Two caps, both applied HERE on the cosmetic side only -- the
	// authoritative removal above is never capped by them, so world state is
	// identical whatever these are set to (and identical on a dedicated server,
	// which spawns none of this).
	//   * MaxDebrisActors: at most this many Chaos bodies per edit.
	//   * MaxDebrisInstancesPerEdit: a shared budget of visible cubes across
	//     all of them, so many-small-pieces and one-huge-piece both stay bounded.
	// Pieces are taken LARGEST FIRST so the visually significant mass always
	// gets a body and it is the invisible confetti that gets dropped. Ties
	// break on the piece's first (VoxelCoordLess-minimum) voxel, so the choice
	// is deterministic even though it is only cosmetic.
	// Pieces past the cap are simply not shown; they have already been removed
	// from the grid, which is the part that matters.
	constexpr int32 MaxDebrisActors = 24;
	constexpr int32 MaxDebrisInstancesPerEdit = 40000;

	TArray<int32> Order;
	Order.Reserve(Islands.Num());
	for (int32 I = 0; I < Islands.Num(); ++I)
	{
		if (Islands[I].Num() > 0)
		{
			Order.Add(I);
		}
	}
	Order.Sort([&Islands](int32 A, int32 Bi)
	{
		const TArray<VoxelCoords::FVoxelCoord>& Pa = Islands[A];
		const TArray<VoxelCoords::FVoxelCoord>& Pb = Islands[Bi];
		if (Pa.Num() != Pb.Num())
		{
			return Pa.Num() > Pb.Num(); // largest first
		}
		const VoxelCoords::FVoxelCoord& Va = Pa[0];
		const VoxelCoords::FVoxelCoord& Vb = Pb[0];
		if (Va.Z != Vb.Z) return Va.Z < Vb.Z; // VoxelCoordLess tie-break
		if (Va.Y != Vb.Y) return Va.Y < Vb.Y;
		return Va.X < Vb.X;
	});

	int32 Spawned = 0, InstanceBudget = MaxDebrisInstancesPerEdit, Skipped = 0, TotalInstances = 0;
	for (int32 I : Order)
	{
		if (Spawned >= MaxDebrisActors || InstanceBudget <= 0)
		{
			++Skipped;
			continue;
		}
		AVoxelDebris* Debris = World.SpawnActor<AVoxelDebris>();
		if (!Debris)
		{
			continue;
		}
		const int32 Used = Debris->InitFromIsland(Islands[I], InstanceBudget);
		InstanceBudget -= Used;
		TotalInstances += Used;
		++Spawned;
	}
	UE_LOG(LogVoxelEdit, Log,
	       TEXT("Destruction: %d piece(s) promoted, %d cosmetic debris actor(s) spawned (%d instances, %d piece(s) over the ")
	       TEXT("cap shown as none)"),
	       IslandCount, Spawned, TotalInstances, Skipped);
}

// M3 wave 2 persistence (docs/m3-plan.md "Save/load") -----------------------
//
// Saved/VoxelWorlds/<seed>.vxlog, using vxc::EditLog::serialize's
// self-describing whole-log format (magic/version/seed/brickEdge header) --
// the SAME format voxel-core/bench/editlog_tool.cpp's `stats`/`compact`/
// `verify` commands already read/write, so an on-disk save from this game
// process is directly usable by that offline tool too.
FString GetWorldSaveFilePath(uint64 Seed)
{
	return FPaths::ProjectSavedDir() / TEXT("VoxelWorlds") / FString::Printf(TEXT("%llu.vxlog"), (unsigned long long)Seed);
}

// Atomic tmp+rename write (mirrors voxel-core/bench/editlog_tool.cpp's own
// writeFileAtomic, UE-side: FFileHelper for the write, IFileManager::Move for
// the rename-over-destination) -- a process dying mid-write leaves only the
// .tmp file behind, never a truncated/corrupt Path.
bool WriteBytesAtomic(const FString& Path, const TArray<uint8>& Bytes)
{
	const FString TmpPath = Path + TEXT(".tmp");
	if (!FFileHelper::SaveArrayToFile(Bytes, *TmpPath))
	{
		UE_LOG(LogVoxelEdit, Error, TEXT("SaveWorld: failed to write temp file %s"), *TmpPath);
		return false;
	}
	if (!IFileManager::Get().Move(*Path, *TmpPath, /*Replace*/ true))
	{
		UE_LOG(LogVoxelEdit, Error, TEXT("SaveWorld: failed to rename %s -> %s"), *TmpPath, *Path);
		IFileManager::Get().Delete(*TmpPath);
		return false;
	}
	return true;
}

// UVoxelWorldSubsystem::SaveWorld's real body (kept as a free function so it
// only needs FVoxelWorldImpl&, matching this file's existing role-split
// convention -- see TryDigReplica et al above). Compacts the outgoing copy
// (never the live log_) when the raw log has more than 2x its compacted
// entry count (docs/m3-plan.md wave 2 item 1: "COMPACT on save when the log
// has >2x the entries of its compacted form").
bool SaveEditLogToDisk(const FVoxelWorldImpl& Impl, uint64 Seed)
{
	const vxc::EditLog& Log = Impl.Voxels.log();
	const vxc::EditLog Compacted = vxc::compactLog(Log);
	const bool bUseCompacted = Log.size() > 2 * Compacted.size();
	const vxc::EditLog& ToSave = bUseCompacted ? Compacted : Log;

	std::vector<uint8_t> Bytes;
	ToSave.serialize(Bytes);
	TArray<uint8> OutBytes;
	OutBytes.SetNumUninitialized((int32)Bytes.size());
	if (!Bytes.empty())
	{
		FMemory::Memcpy(OutBytes.GetData(), Bytes.data(), Bytes.size());
	}

	const FString Dir = FPaths::ProjectSavedDir() / TEXT("VoxelWorlds");
	IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
	const FString Path = GetWorldSaveFilePath(Seed);

	if (!WriteBytesAtomic(Path, OutBytes))
	{
		return false;
	}

	if (bUseCompacted)
	{
		UE_LOG(LogVoxelEdit, Log,
		       TEXT("SaveWorld: compacted %llu -> %llu entries before saving (raw log had more than 2x the compacted size)."),
		       (unsigned long long)Log.size(), (unsigned long long)Compacted.size());
	}
	UE_LOG(LogVoxelEdit, Log,
	       TEXT("SaveWorld: wrote %llu entries (%d bytes%s) to %s -- editedDigest=0x%016llX"),
	       (unsigned long long)ToSave.size(), OutBytes.Num(), bUseCompacted ? TEXT(", compacted") : TEXT(""), *Path,
	       (unsigned long long)Impl.Voxels.editedDigest());
	return true;
}

// Loads Saved/VoxelWorlds/<seed>.vxlog (if present) via vxc::World::replay,
// called once from OnWorldBeginPlay before any streaming/chunk work reads
// from Impl.Voxels -- see the call site for the authority-only gating.
// -VoxelNoLoad bypasses this entirely (fresh-start verification aid).
void LoadEditLogFromDisk(FVoxelWorldImpl& Impl, uint64 Seed)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("VoxelNoLoad")))
	{
		UE_LOG(LogVoxelEdit, Log, TEXT("LoadWorld: -VoxelNoLoad passed -- skipping saved-world load, starting fresh."));
		return;
	}

	const FString Path = GetWorldSaveFilePath(Seed);
	if (!FPaths::FileExists(Path))
	{
		UE_LOG(LogVoxelEdit, Log, TEXT("LoadWorld: no saved world at %s -- starting fresh."), *Path);
		return;
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogVoxelEdit, Error, TEXT("LoadWorld: failed to read %s -- starting fresh."), *Path);
		return;
	}

	const std::optional<vxc::EditLog> ParsedLog = vxc::EditLog::parse(Bytes.GetData(), (size_t)Bytes.Num());
	if (!ParsedLog)
	{
		UE_LOG(LogVoxelEdit, Error, TEXT("LoadWorld: %s is corrupt or unrecognized -- starting fresh."), *Path);
		return;
	}

	if (!Impl.Voxels.replay(*ParsedLog))
	{
		UE_LOG(LogVoxelEdit, Error, TEXT("LoadWorld: seed/brickEdge mismatch replaying %s -- starting fresh."), *Path);
		return;
	}

	UE_LOG(LogVoxelEdit, Log, TEXT("LoadWorld: restored %llu entries from %s -- editedDigest=0x%016llX"),
	       (unsigned long long)ParsedLog->size(), *Path, (unsigned long long)Impl.Voxels.editedDigest());
}
} // namespace

// UVoxelWorldSubsystem ----------------------------------------------------

UVoxelWorldSubsystem::UVoxelWorldSubsystem() = default;
UVoxelWorldSubsystem::~UVoxelWorldSubsystem() = default;
UVoxelWorldSubsystem::UVoxelWorldSubsystem(FVTableHelper& Helper)
	: Super(Helper)
{
}

void UVoxelWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// M2 task "Config-driven seed": -VoxelSeed=<u64> command-line override,
	// falling back to DefaultSeed (docs/m1-plan.md decisions table "seed from
	// config (default 20260719)"). Command-line only, no ini fallback --
	// an ini-driven override would be overkill for a dev/verification-only
	// knob (see the task's own framing). Resolved once, here, before Impl is
	// constructed, so every voxel-core access this run (Impl's World/Tiles)
	// and every other actor that samples seed-matched terrain (e.g.
	// AVoxelClipmapActor's heightmap, via GetSeed()) agree on one value.
	uint64 ParsedSeed = DefaultSeed;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelSeed="), ParsedSeed))
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelSeed override: using seed %llu (default %llu)"),
		       (unsigned long long)ParsedSeed, (unsigned long long)DefaultSeed);
	}
	Seed = ParsedSeed;

	// Track B2 ("real .vxtl terrain tiles as a selectable tile source"):
	// -VoxelTileDir=<path> selects a terrain-service tile-cache directory as
	// the world's tile source instead of the synthetic sampler; empty/absent
	// (the default -- no new switch on the command line) keeps today's exact
	// behavior, unconditionally (see MakeTileSampler). Same one-shot dev/
	// config-switch convention as -VoxelSeed above: command-line only, no ini
	// fallback, resolved once here before Impl is constructed.
	FString TileDir;
	FParse::Value(FCommandLine::Get(), TEXT("VoxelTileDir="), TileDir);
	if (!TileDir.IsEmpty() && FPaths::IsRelative(TileDir))
	{
		TileDir = FPaths::Combine(FPaths::ProjectContentDir(), TileDir);
	}

	// -VoxelTileScale=<int>: which tile scale to load (1 => 30m/px, the
	// default; 8 => 11.25m/px). Only meaningful alongside -VoxelTileDir --
	// MakeTileSampler validates it (vxc::tilePixelSizeMm returns 0 for
	// anything else) and falls back to the synthetic sampler with an Error
	// log if it's invalid, rather than silently misinterpreting tile pixels.
	int32 TileScale = 1;
	FParse::Value(FCommandLine::Get(), TEXT("VoxelTileScale="), TileScale);

	Impl = MakeUnique<FVoxelWorldImpl>(Seed, TileDir, TileScale);
}

void UVoxelWorldSubsystem::Deinitialize()
{
	if (Impl)
	{
		// Worker jobs hold raw pointers into Impl-owned data (DispatchJobs);
		// block until every in-flight job has finished before freeing it.
		Impl->WaitForInFlightTasks();

		// M3 wave 2 persistence (docs/m3-plan.md "Save/load"): autosave-on-
		// shutdown, but ONLY for a world that actually began play (see the
		// header's doc comment on bWorldBegunPlay) -- the transient "Entry"
		// loading world -game/-server also constructs a subsystem instance
		// for has an empty Impl and must never overwrite a real save file
		// with that emptiness. SaveWorld() itself already no-ops (with a
		// logged warning) on NM_Client, so this is safe to call
		// unconditionally beyond that -- only the authority (server/listen/
		// standalone) actually writes a file.
		if (bWorldBegunPlay)
		{
			SaveWorld();
		}
	}

	ChunkRoot = nullptr;
	ChunkOwner = nullptr;
	ChunkMaterial = nullptr;
	Impl.Reset();

	Super::Deinitialize();
}

bool UVoxelWorldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Stage 1 scope (deliverable 2): "On world begin play (game worlds
	// only)" -- skip Editor/Inactive/EditorPreview worlds so simply opening
	// the level editor never triggers generation or ticking.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::GamePreview;
}

void UVoxelWorldSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (!InWorld.IsGameWorld() || !Impl)
	{
		return;
	}

	// M3 wave 2 persistence: from here on, this is a genuine gameplay world
	// (see the header's doc comment on bWorldBegunPlay for why this can't
	// just be "Impl is non-null") -- gates Deinitialize's autosave.
	bWorldBegunPlay = true;

	// M3 wave 2 persistence (docs/m3-plan.md "Save/load"): authority
	// (server/listen/standalone) loads Saved/VoxelWorlds/<seed>.vxlog, if
	// present, before ANY streaming/chunk work below reads from Impl->Voxels
	// -- this is the earliest point OnWorldBeginPlay runs (right after the
	// game-world/Impl guard above), and it runs before the dedicated-server
	// early-return further down so it applies uniformly across every
	// authority role. NM_Client never loads its own file -- a joining client
	// gets its state from the server's join-sync reply instead (see
	// AVoxelEarthPlayerController::ServerRequestJoinSync).
	if (InWorld.GetNetMode() != NM_Client)
	{
		LoadEditLogFromDisk(*Impl, Seed);
	}

	// M3 wave 1 (docs/m3-plan.md): a dedicated server has no viewport and
	// never renders -- render-chunk streaming (mesh generation, worker
	// jobs, UVoxelChunkComponent spawning) is pure client/listen-server
	// concern that would otherwise burn CPU every tick for nothing on a
	// true NM_DedicatedServer (this WAS observed: the M3 gate's first run
	// showed multi-second tick stalls server-side from exactly this work,
	// stretching FTimerManager-scheduled verification switches well past
	// their nominal delay). ChunkOwner/ChunkRoot are deliberately left
	// null; Tick() already no-ops without them, so nothing else needs to
	// change -- Impl->Voxels (the authoritative World + edit log) is fully
	// usable via TryDig/TryPlace/CarveSphere regardless.
	if (InWorld.GetNetMode() == NM_DedicatedServer)
	{
		UE_LOG(LogVoxelStream, Log,
		       TEXT("Voxel streaming DISABLED (seed %llu): NM_DedicatedServer has no viewport -- only the authoritative ")
		       TEXT("World + edit log run here."),
		       (unsigned long long)Seed);
		return;
	}

	// Resolve the terrain material once (deliverable 4: load by path,
	// fallback to the engine default material, never crash).
	// -VoxelDefaultMaterial: diagnostic switch — skip the authored material
	// and use the engine default, to isolate material bugs from geometry
	// bugs (an invisible-terrain failure with the authored material and a
	// visible one with the default indicts the asset, not the mesh).
	if (!FParse::Param(FCommandLine::Get(), TEXT("VoxelDefaultMaterial")))
	{
		ChunkMaterial = Cast<UMaterialInterface>(
			StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/Game/Voxel/M_VoxelTerrain.M_VoxelTerrain")));
	}
	if (ChunkMaterial == nullptr)
	{
		ChunkMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("M_VoxelTerrain not found at /Game/Voxel/M_VoxelTerrain -- using engine default material."));
	}

	// Hitch isolation (docs/status.md "Perf-run hitches" / M1 gate blocker,
	// fix (b) "PSO precache"): this hand-rolled FLocalVertexFactory +
	// M_VoxelTerrain combination has never been drawn yet at this point in a
	// fresh session -- absent a precache request, the engine compiles its
	// pipeline-state-object the first time a real chunk actually reaches
	// GetDynamicMeshElements, synchronously stalling whichever frame that
	// lands on. UPrimitiveComponent's automatic PrecachePSOs-on-register path
	// only fires for components that override it (UStaticMeshComponent etc);
	// UVoxelChunkComponent never has, so this material+vertex-factory pair
	// was never precached at all before this fix. Kicking it off here, once,
	// at BeginPlay -- before ChunkOwner/ChunkRoot even exist, let alone any
	// real chunk -- gives the async compile the whole loading-time window to
	// finish before the streaming ramp's first frame. Non-blocking:
	// PrecachePSOs only enqueues async shader-compile graph events, it never
	// waits on them.
	// -VoxelNoPSOPrecache: diagnostic switch (same convention as
	// -VoxelDefaultMaterial above) -- skip the warmup request to isolate its
	// effect on the hitch-attribution A/B measurement (docs/status.md
	// "Perf-run hitches"). Never set in normal play.
	if (ChunkMaterial && !FParse::Param(FCommandLine::Get(), TEXT("VoxelNoPSOPrecache")))
	{
		FPSOPrecacheParams TerrainPrecacheParams;
		// UVoxelChunkComponent never calls SetMobility away from its
		// UPrimitiveComponent Super default (Movable) -- match that here so
		// the precached PSO variant is the one actually requested at draw
		// time, not a Static-mobility variant that would go unused.
		TerrainPrecacheParams.SetMobility(EComponentMobility::Movable);
		ChunkMaterial->PrecachePSOs(&FLocalVertexFactory::StaticType, TerrainPrecacheParams);
	}

	// Single actor hosting every render-chunk component (unchanged from
	// stage 1); streaming now spawns/destroys UVoxelChunkComponents on it
	// every tick instead of a one-shot fixed-radius pass.
	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags |= RF_Transient;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ChunkOwner = InWorld.SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	if (ChunkOwner == nullptr)
	{
		// docs/debug-tooling-plan.md P1 "Log split": streaming-lifecycle lines
		// move to LogVoxelStream (module/general lines stay on LogVoxelEarth).
		UE_LOG(LogVoxelStream, Error, TEXT("Failed to spawn the voxel chunk owner actor; streaming will not start."));
		return;
	}

	ChunkRoot = NewObject<USceneComponent>(ChunkOwner, TEXT("VoxelChunkRoot"));
	ChunkOwner->SetRootComponent(ChunkRoot);
	ChunkRoot->RegisterComponent();
#if WITH_EDITOR
	ChunkOwner->SetActorLabel(TEXT("VoxelChunkOwner"));
#endif

	UE_LOG(LogVoxelStream, Log,
	       TEXT("Voxel streaming initialized (seed %llu): load=%.0fm unload=%.0fm digRange=%.0fm"),
	       (unsigned long long)Seed, LoadRadiusMeters, UnloadRadiusMeters, DigPlaceRangeMeters);
}

void UVoxelWorldSubsystem::Tick(float DeltaTime)
{
	if (!Impl || !ChunkOwner || !ChunkRoot)
	{
		return;
	}

	// Streaming anchor (decisions table: "Track a streaming anchor each
	// tick"): the first local player's possessed pawn, falling back to the
	// world origin if there is none yet (e.g. before RestartPlayer runs).
	FVector Anchor = FVector::ZeroVector;
	if (UWorld* World = GetWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (APawn* Pawn = PC->GetPawn())
			{
				Anchor = Pawn->GetActorLocation();
			}
		}
	}

	Impl->TickStreaming(Anchor, *ChunkOwner, *ChunkRoot, ChunkMaterial, DeltaTime);
}

TStatId UVoxelWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelWorldSubsystem, STATGROUP_Tickables);
}

// M3 wave 1 role split (docs/m3-plan.md "Authority flow"): NM_Standalone
// takes EXACTLY the pre-M3 code path (Impl->TryDig, nothing else) --
// byte-identical single-player behavior. NM_DedicatedServer/NM_ListenServer
// (this world IS the authority) also apply directly, then broadcast
// whatever the edit newly appended to every client. NM_Client predicts
// locally and forwards the intent to the server instead of applying
// authoritatively (see TryDigReplica above).
bool UVoxelWorldSubsystem::TryDig(const FVector& CameraWorldLocation, const FVector& CameraWorldDirection, int32 SizeVoxels)
{
	if (!Impl)
	{
		return false;
	}
	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;

	if (NetMode == NM_Client)
	{
		return World ? TryDigReplica(*Impl, *World, CameraWorldLocation, CameraWorldDirection, SizeVoxels) : false;
	}

	FEditsByBrick DugCells;
	const bool bApplied = Impl->TryDig(CameraWorldLocation, CameraWorldDirection, SizeVoxels, &DugCells);
	if (bApplied && World && NetMode != NM_Client)
	{
		// Voxels this dig cleared (dig-style MAT_AIR writes), shared by the M5
		// chop hook and the W2 water-breach hook below.
		TArray<VoxelCoords::FVoxelCoord> ClearedVoxels;
		ExtractClearedVoxelCoords(DugCells, ClearedVoxels);

		// M5 chop hook (docs/voxel-earth-implementation-plan.md SS3.5): detach +
		// remove any floating island this dig severed. Runs BEFORE the broadcast
		// so the island-removal edit entries replicate together with the dig
		// (they are appended to the same log). Also spawns cosmetic debris.
		PromoteDetachedIslands(*Impl, *World, ClearedVoxels);

		// Same NetMode gate as before, but now AFTER the chop hook so the
		// broadcast covers both the dig and any island removals it triggered.
		if (NetMode == NM_DedicatedServer || NetMode == NM_ListenServer)
		{
			BroadcastNewEntries(*Impl, *World);
		}

		// W2 dig-breach hook (task item 2): fires on any authority role incl.
		// NM_Standalone (single-player has no relay to broadcast to, but water
		// breach seeding still applies).
		if (UVoxelWaterSubsystem* WaterSubsystem = World->GetSubsystem<UVoxelWaterSubsystem>())
		{
			WaterSubsystem->NotifyTerrainVoxelsCleared(ClearedVoxels);

			// ADR-0003 item 2: memo invalidation, batched over this whole
			// dig's own bounding box (one call, not one per voxel).
			VoxelCoords::FVoxelCoord EditMin, EditMax;
			if (ComputeEditVoxelBounds(DugCells, EditMin, EditMax))
			{
				WaterSubsystem->NotifyTerrainRegionEdited(EditMin, EditMax);
			}
		}
	}
	return bApplied;
}

bool UVoxelWorldSubsystem::TryPlace(const FVector& CameraWorldLocation, const FVector& CameraWorldDirection, int32 SizeVoxels,
                                     uint8 MaterialId, const FVector& PlayerActorLocation)
{
	if (!Impl)
	{
		return false;
	}
	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;

	if (NetMode == NM_Client)
	{
		return World ? TryPlaceReplica(*Impl, *World, CameraWorldLocation, CameraWorldDirection, SizeVoxels, MaterialId,
		                                PlayerActorLocation)
		             : false;
	}

	// W2 fix (task item 1): TryPlace previously sent NO water notification at
	// all -- capture the placed cells (mirrors TryDig's DugCells) so both
	// water hooks below can see exactly what changed.
	FEditsByBrick PlacedCells;
	const bool bApplied =
		Impl->TryPlace(CameraWorldLocation, CameraWorldDirection, SizeVoxels, MaterialId, PlayerActorLocation, &PlacedCells);
	if (bApplied && World)
	{
		if (NetMode == NM_DedicatedServer || NetMode == NM_ListenServer)
		{
			BroadcastNewEntries(*Impl, *World);
		}

		if (UVoxelWaterSubsystem* WaterSubsystem = World->GetSubsystem<UVoxelWaterSubsystem>())
		{
			// Reservoir/breach path: normally a no-op for TryPlace (the
			// placed material is virtually never MAT_AIR), but handled
			// generically via the same ExtractClearedVoxelCoords filter a dig
			// uses rather than assumed-never-relevant, in case a future
			// material picker ever offers MAT_AIR.
			TArray<VoxelCoords::FVoxelCoord> ClearedVoxels;
			ExtractClearedVoxelCoords(PlacedCells, ClearedVoxels);
			if (ClearedVoxels.Num() > 0)
			{
				WaterSubsystem->NotifyTerrainVoxelsCleared(ClearedVoxels);
			}

			// ADR-0003 item 2: the fix that actually matters here -- air->solid
			// invalidates the memo exactly like solid->air does.
			VoxelCoords::FVoxelCoord EditMin, EditMax;
			if (ComputeEditVoxelBounds(PlacedCells, EditMin, EditMax))
			{
				WaterSubsystem->NotifyTerrainRegionEdited(EditMin, EditMax);
			}
		}
	}
	return bApplied;
}

bool UVoxelWorldSubsystem::DebugChunkStatusAt(const FVector& WorldPos, bool& bOutTracked, bool& bOutHasComponent,
                                              int32& OutQuads) const
{
	if (!Impl)
	{
		return false;
	}
	const VoxelCoords::FVoxelLevelChunkKey Key{0, VoxelCoords::ChunkKeyForVoxel(VoxelCoords::WorldToVoxel(WorldPos))};
	const VoxelStreaming::FChunkRecord* Rec = Impl->ChunkRecords.Find(Key);
	bOutTracked = (Rec != nullptr);
	bOutHasComponent = Rec && Rec->Component.IsValid();
	OutQuads = Rec ? Rec->LastQuadCount : 0;
	return true;
}

double UVoxelWorldSubsystem::GetSurfaceHeightUU(double WorldX, double WorldY) const
{
	if (!Impl)
	{
		return 0.0;
	}
	const int64 Vx = (int64)FMath::FloorToDouble(WorldX / VoxelCoords::VoxelSizeUU);
	const int64 Vy = (int64)FMath::FloorToDouble(WorldY / VoxelCoords::VoxelSizeUU);
	const vxc::ColumnSample Col = Impl->Voxels.amplifier().column(Vx, Vy);
	return double(Col.surfaceMm) / 10.0; // mm -> UU (1 UU = 10 mm)
}

double UVoxelWorldSubsystem::SampleTerrainHeightUU(double WorldXUU, double WorldYUU) const
{
	if (!Impl)
	{
		// Transient "Entry"/loading world (see bWorldBegunPlay's doc comment
		// in the header) -- Impl exists but has never streamed anything, and
		// nothing here needs a real height for it; 0.0 matches
		// GetSurfaceHeightUU's own Impl==nullptr fallback above.
		return 0.0;
	}

	// Track B2: sample whichever ITileSampler this run is actually using
	// (synthetic by default, or a loaded vxc::TileGridSampler under
	// -VoxelTileDir=<path> -- see FVoxelWorldImpl::Tiles/MakeTileSampler) --
	// moved here (from AVoxelClipmapActor's file-local SampleHeightUU) so
	// clipmap terrain and voxel-ring terrain read the SAME tiles and keep
	// lining up at their shared seam whether or not real tiles are loaded.
	vxc::ITileSampler& Sampler = *Impl->Tiles;

	// mm -> UU is /10 (VoxelCoords::WorldToMm's inverse: 1 UU = 10 mm).
	const double PixelSizeUU = double(Sampler.pixelSizeMm()) / 10.0;
	const double Px = WorldXUU / PixelSizeUU;
	const double Py = WorldYUU / PixelSizeUU;
	const int64 Px0 = (int64)FMath::FloorToDouble(Px);
	const int64 Py0 = (int64)FMath::FloorToDouble(Py);
	const double Fx = Px - double(Px0);
	const double Fy = Py - double(Py0);

	auto ElevUU = [&Sampler](int64 X, int64 Y) { return double(Sampler.elevationMm(X, Y)) / 10.0; };
	const double H00 = ElevUU(Px0, Py0);
	const double H10 = ElevUU(Px0 + 1, Py0);
	const double H01 = ElevUU(Px0, Py0 + 1);
	const double H11 = ElevUU(Px0 + 1, Py0 + 1);
	const double Hx0 = FMath::Lerp(H00, H10, Fx);
	const double Hx1 = FMath::Lerp(H01, H11, Fx);
	return FMath::Lerp(Hx0, Hx1, Fy);
}

bool UVoxelWorldSubsystem::IsSolidAtVoxel(int64 Vx, int64 Vy, int64 Vz) const
{
	if (!Impl)
	{
		return false;
	}
	// Overlay-aware (World::materialAt, not GeneratedWorld::materialAt): a
	// dug voxel must read back as non-solid immediately, and a placed one as
	// solid, for walk-mode collision to agree with what dig/place just did.
	return Impl->Voxels.materialAt(Vx, Vy, Vz) != vxc::MAT_AIR;
}

bool UVoxelWorldSubsystem::RaycastVoxelWorld(const FVector& StartUU, const FVector& DirUU, double MaxDistUU,
                                              FVector& OutHitVoxelCenterUU, FVector& OutPrevVoxelCenterUU) const
{
	if (!Impl || MaxDistUU <= 0.0)
	{
		return false;
	}

	const FVector Dir = DirUU.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		return false;
	}

	// Same mm conversion as the dig/place raycast (CastFromCamera above);
	// kept independent (not shared) so this method stays self-contained for
	// the parallel file-ownership split noted in docs/m1-plan.md.
	const int64 OxMm = VoxelCoords::WorldToMm(StartUU.X);
	const int64 OyMm = VoxelCoords::WorldToMm(StartUU.Y);
	const int64 OzMm = VoxelCoords::WorldToMm(StartUU.Z);
	const double RangeMm = MaxDistUU * 10.0; // UU -> mm (1 UU = 10 mm)
	const int64 DxMm = (int64)FMath::RoundToDouble(Dir.X * RangeMm);
	const int64 DyMm = (int64)FMath::RoundToDouble(Dir.Y * RangeMm);
	const int64 DzMm = (int64)FMath::RoundToDouble(Dir.Z * RangeMm);

	const auto MaterialFn = [this](int64 X, int64 Y, int64 Z) { return Impl->Voxels.materialAt(X, Y, Z); };
	const vxc::RaycastHit Hit = vxc::raycastVoxels(MaterialFn, OxMm, OyMm, OzMm, DxMm, DyMm, DzMm);
	if (!Hit.hit)
	{
		return false;
	}

	OutHitVoxelCenterUU = VoxelCoords::VoxelToWorldCenter(VoxelCoords::FVoxelCoord{Hit.vx, Hit.vy, Hit.vz});
	OutPrevVoxelCenterUU = VoxelCoords::VoxelToWorldCenter(VoxelCoords::FVoxelCoord{Hit.px, Hit.py, Hit.pz});
	return true;
}

int32 UVoxelWorldSubsystem::CarveSphere(const FVector& CenterUU, double RadiusUU, double JitterUU)
{
	if (!Impl)
	{
		return 0;
	}
	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;

	if (NetMode == NM_Client)
	{
		return World ? CarveSphereReplica(*Impl, *World, CenterUU, RadiusUU, JitterUU) : 0;
	}

	FEditsByBrick CarvedCells;
	const int32 Removed = Impl->CarveSphere(CenterUU, RadiusUU, JitterUU, &CarvedCells);
	if (Removed > 0 && World && NetMode != NM_Client)
	{
		TArray<VoxelCoords::FVoxelCoord> ClearedVoxels;
		ExtractClearedVoxelCoords(CarvedCells, ClearedVoxels);

		// M5 chop hook: CarveSphere is the explosive/crater carve path AND the
		// -VoxelChopTest trunk cut -- a carve that severs a piece detaches it
		// here (runs before the broadcast, same as TryDig).
		PromoteDetachedIslands(*Impl, *World, ClearedVoxels);

		if (NetMode == NM_DedicatedServer || NetMode == NM_ListenServer)
		{
			BroadcastNewEntries(*Impl, *World);
		}

		// W2 dig-breach hook (task item 2): a crater opened below sea level
		// adjacent to open ocean floods.
		if (UVoxelWaterSubsystem* WaterSubsystem = World->GetSubsystem<UVoxelWaterSubsystem>())
		{
			WaterSubsystem->NotifyTerrainVoxelsCleared(ClearedVoxels);

			// ADR-0003 item 2: memo invalidation, batched over this carve's
			// own bounding box.
			VoxelCoords::FVoxelCoord EditMin, EditMax;
			if (ComputeEditVoxelBounds(CarvedCells, EditMin, EditMax))
			{
				WaterSubsystem->NotifyTerrainRegionEdited(EditMin, EditMax);
			}
		}
	}
	return Removed;
}

FVoxelPerfSnapshot UVoxelWorldSubsystem::GetPerfSnapshot() const
{
	return Impl ? Impl->GetPerfSnapshot() : FVoxelPerfSnapshot{};
}

int32 UVoxelWorldSubsystem::SpawnTreeFixtureAt(double WorldX, double WorldY)
{
	if (!Impl)
	{
		return 0;
	}

	// TEST FIXTURE geometry (docs/m4-plan.md Round 2 -- NOT M4 vegetation): a
	// 2x2 trunk column rooted on the surface + a spherical canopy blob above
	// it, both solid MAT_ROCK, all written through the edit-log authority path.
	// Deliberately blocky/simple -- its only job is to be a connected solid the
	// chop test can sever so the canopy detaches as an island.
	const double SurfaceUU = GetSurfaceHeightUU(WorldX, WorldY);
	const int64 BaseVx = (int64)FMath::FloorToDouble(WorldX / VoxelCoords::VoxelSizeUU);
	const int64 BaseVy = (int64)FMath::FloorToDouble(WorldY / VoxelCoords::VoxelSizeUU);
	const int64 BaseVz = (int64)FMath::FloorToDouble(SurfaceUU / VoxelCoords::VoxelSizeUU) + 1; // first voxel above the surface

	constexpr int64 TrunkHeight = 24; // 2.4m
	constexpr int64 CanopyRadius = 6; // ~1.2m blob

	TSet<VoxelCoords::FVoxelCoord> TreeVoxels; // set dedupes the trunk/canopy overlap
	for (int64 Dz = 0; Dz < TrunkHeight; ++Dz)
	{
		for (int64 Dy = 0; Dy < 2; ++Dy)
		{
			for (int64 Dx = 0; Dx < 2; ++Dx)
			{
				TreeVoxels.Add(VoxelCoords::FVoxelCoord{BaseVx + Dx, BaseVy + Dy, BaseVz + Dz});
			}
		}
	}
	const int64 Ccx = BaseVx, Ccy = BaseVy, Ccz = BaseVz + TrunkHeight; // canopy centre overlaps the trunk top (stays connected)
	const int64 R2 = CanopyRadius * CanopyRadius;
	for (int64 Dz = -CanopyRadius; Dz <= CanopyRadius; ++Dz)
	{
		for (int64 Dy = -CanopyRadius; Dy <= CanopyRadius; ++Dy)
		{
			for (int64 Dx = -CanopyRadius; Dx <= CanopyRadius; ++Dx)
			{
				if (Dx * Dx + Dy * Dy + Dz * Dz <= R2)
				{
					TreeVoxels.Add(VoxelCoords::FVoxelCoord{Ccx + Dx, Ccy + Dy, Ccz + Dz});
				}
			}
		}
	}

	const TArray<VoxelCoords::FVoxelCoord> Coords = TreeVoxels.Array();
	const int32 Count = Impl->StampVoxels(Coords, (uint8)vxc::MAT_ROCK);

	// Replicate the fixture to clients on a networked authority (single-player
	// has no relay -- BroadcastNewEntries no-ops without one anyway).
	UWorld* World = GetWorld();
	if (World && (World->GetNetMode() == NM_DedicatedServer || World->GetNetMode() == NM_ListenServer))
	{
		BroadcastNewEntries(*Impl, *World);
	}

	// ADR-0003 item 2: this is a non-air (MAT_ROCK) edit-log write like any
	// other -- the memo must be told, exactly like TryPlace.
	if (World)
	{
		if (UVoxelWaterSubsystem* WaterSubsystem = World->GetSubsystem<UVoxelWaterSubsystem>())
		{
			VoxelCoords::FVoxelCoord EditMin, EditMax;
			if (ComputeVoxelCoordBounds(Coords, EditMin, EditMax))
			{
				WaterSubsystem->NotifyTerrainRegionEdited(EditMin, EditMax);
			}
		}
	}

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelTreeTest: stamped stand-in tree FIXTURE -- %d voxels at column (%.0f,%.0f), baseVz=%lld (surface %.0fUU)"),
	       Count, WorldX, WorldY, (long long)BaseVz, SurfaceUU);
	return Count;
}

int32 UVoxelWorldSubsystem::SpawnStructureFixtureAt(double WorldX, double WorldY)
{
	if (!Impl)
	{
		return 0;
	}

	// M5 LARGE-EDIT COLLAPSE test fixture (docs/status.md "Structural collapse
	// (M5, large-edit)"). Deliberately shaped so that pure connectivity is NOT
	// enough to explain the right answer:
	//
	//     wall                                        far pillars
	//     ####|=====================================|####|
	//     ####|             roof slab               |####|
	//     ####                                       ####
	//     ####                                       ####
	//   ~~~~~~~~~~~~~~~~ terrain ~~~~~~~~~~~~~~~~~~~~~~~~~~
	//
	// A short ground-rooted WALL at one end, two PILLARS 12.8m away at the
	// other, and a thin ROOF SLAB spanning between them. Blow out only the far
	// pillars and the roof is STILL 6-connected to the ground through the wall
	// -- findDisconnectedIslands would report zero islands and nothing would
	// fall -- but everything past the 4.8m cantilever budget from the wall has
	// nothing carrying it and must come down. That is the whole point of a
	// support model as opposed to a connectivity model.
	//
	// Sizes are in voxels (1 voxel = 10cm). All members run 16 voxels BELOW the
	// surface so uneven terrain can never leave the fixture floating.
	constexpr int64 StructLenX = 128;   // 12.8m span, wall face to pillar face
	constexpr int64 StructWidY = 32;    // 3.2m
	constexpr int64 WallThickX = 4;
	constexpr int64 PillarSize = 8;     // 0.8m square == exactly one brick footprint
	constexpr int64 EmbedDepth = 16;    // 1.6m of footing buried in the terrain
	constexpr int64 ClearHeight = 32;   // 3.2m from the surface to the underside of the roof
	constexpr int64 RoofThick = 2;      // 20cm slab

	const double SurfaceUU = GetSurfaceHeightUU(WorldX, WorldY);
	const int64 BaseVx = (int64)FMath::FloorToDouble(WorldX / VoxelCoords::VoxelSizeUU);
	const int64 BaseVy = (int64)FMath::FloorToDouble(WorldY / VoxelCoords::VoxelSizeUU);
	const int64 BaseVz = (int64)FMath::FloorToDouble(SurfaceUU / VoxelCoords::VoxelSizeUU) + 1;

	TSet<VoxelCoords::FVoxelCoord> Voxels;
	auto AddBox = [&Voxels, BaseVx, BaseVy, BaseVz](int64 X0, int64 X1, int64 Y0, int64 Y1, int64 Z0, int64 Z1)
	{
		for (int64 Z = Z0; Z <= Z1; ++Z)
			for (int64 Y = Y0; Y <= Y1; ++Y)
				for (int64 X = X0; X <= X1; ++X)
					Voxels.Add(VoxelCoords::FVoxelCoord{BaseVx + X, BaseVy + Y, BaseVz + Z});
	};

	AddBox(0, WallThickX - 1, 0, StructWidY - 1, -EmbedDepth, ClearHeight - 1); // wall
	AddBox(StructLenX - PillarSize, StructLenX - 1, 0, PillarSize - 1, -EmbedDepth, ClearHeight - 1); // -y pillar
	AddBox(StructLenX - PillarSize, StructLenX - 1, StructWidY - PillarSize, StructWidY - 1, -EmbedDepth,
	       ClearHeight - 1); // +y pillar
	AddBox(0, StructLenX - 1, 0, StructWidY - 1, ClearHeight, ClearHeight + RoofThick - 1); // roof slab

	const TArray<VoxelCoords::FVoxelCoord> Coords = Voxels.Array();
	const int32 Count = Impl->StampVoxels(Coords, (uint8)vxc::MAT_ROCK);

	UWorld* World = GetWorld();
	if (World && (World->GetNetMode() == NM_DedicatedServer || World->GetNetMode() == NM_ListenServer))
	{
		BroadcastNewEntries(*Impl, *World);
	}

	// ADR-0003 item 2: same fix as SpawnTreeFixtureAt above -- a non-air
	// (MAT_ROCK) edit-log write the memo must be told about.
	if (World)
	{
		if (UVoxelWaterSubsystem* WaterSubsystem = World->GetSubsystem<UVoxelWaterSubsystem>())
		{
			VoxelCoords::FVoxelCoord EditMin, EditMax;
			if (ComputeVoxelCoordBounds(Coords, EditMin, EditMax))
			{
				WaterSubsystem->NotifyTerrainRegionEdited(EditMin, EditMax);
			}
		}
	}

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelStructureTest: stamped wall+roof+pillars FIXTURE -- %d voxels at column (%.0f,%.0f), baseVz=%lld ")
	       TEXT("(surface %.0fUU); roof spans %.1fm, pillars at +%.1fm"),
	       Count, WorldX, WorldY, (long long)BaseVz, SurfaceUU, double(StructLenX) / 10.0,
	       double(StructLenX - PillarSize) / 10.0);
	return Count;
}

// --- M3 wave 1: multiplayer plumbing (docs/m3-plan.md) ----------------------

void UVoxelWorldSubsystem::SerializeLogEntriesFrom(uint64 FromSeq, TArray<uint8>& OutBytes) const
{
	OutBytes.Reset();
	if (!Impl)
	{
		return;
	}
	const std::vector<vxc::EditEntry>& AllEntries = Impl->Voxels.log().entries();
	std::vector<vxc::EditEntry> Slice;
	if (FromSeq < AllEntries.size())
	{
		Slice.assign(AllEntries.begin() + static_cast<std::ptrdiff_t>(FromSeq), AllEntries.end());
	}
	SerializeEntries(Slice, OutBytes);
}

uint64 UVoxelWorldSubsystem::GetLogSize() const
{
	return Impl ? Impl->Voxels.log().size() : 0;
}

void UVoxelWorldSubsystem::SerializeCompactedLogEntries(TArray<uint8>& OutBytes) const
{
	OutBytes.Reset();
	if (!Impl)
	{
		return;
	}
	const vxc::EditLog& RawLog = Impl->Voxels.log();
	const vxc::EditLog Compacted = vxc::compactLog(RawLog);
	SerializeEntries(Compacted.entries(), OutBytes);
	UE_LOG(LogVoxelEdit, Log,
	       TEXT("SerializeCompactedLogEntries (join-sync): %llu raw entries -> %llu compacted entries (%d bytes)."),
	       (unsigned long long)RawLog.size(), (unsigned long long)Compacted.size(), OutBytes.Num());
}

bool UVoxelWorldSubsystem::ApplyReplicatedEntries(const TArray<uint8>& Bytes)
{
	if (!Impl)
	{
		return false;
	}
	std::vector<vxc::EditEntry> Entries;
	if (!ParseEntries(Bytes, Entries))
	{
		UE_LOG(LogVoxelEdit, Error, TEXT("ApplyReplicatedEntries: failed to parse %d bytes -- dropping batch."), Bytes.Num());
		return false;
	}
	Impl->ApplyReplicatedEntries(Entries);
	return true;
}

void UVoxelWorldSubsystem::BeginJoinSync()
{
	bJoinSyncInProgress = true;
	JoinSyncAccumulator.Reset();
	BufferedLiveEntryBatches.Reset();
	UE_LOG(LogVoxelEdit, Log, TEXT("BeginJoinSync: waiting for the server's full edit-log replay."));
}

bool UVoxelWorldSubsystem::ReceiveJoinSyncChunk(const TArray<uint8>& Bytes, bool bFinal)
{
	JoinSyncAccumulator.Append(Bytes);
	if (!bFinal)
	{
		return true;
	}

	const bool bOk = ApplyReplicatedEntries(JoinSyncAccumulator);
	UE_LOG(LogVoxelEdit, Log,
	       TEXT("ReceiveJoinSyncChunk: join-sync log replay %s (%d bytes total; %d live batch(es) buffered mid-sync to flush now)."),
	       bOk ? TEXT("OK") : TEXT("FAILED"), JoinSyncAccumulator.Num(), BufferedLiveEntryBatches.Num());
	JoinSyncAccumulator.Reset();
	bJoinSyncInProgress = false;

	// Flush anything that arrived mid-sync, in arrival order (m3-plan.md
	// "Join sync": "client replays before accepting live entries (buffer
	// live ones meanwhile)").
	TArray<TArray<uint8>> Buffered = MoveTemp(BufferedLiveEntryBatches);
	BufferedLiveEntryBatches.Reset();
	for (const TArray<uint8>& Batch : Buffered)
	{
		ApplyReplicatedEntries(Batch);
	}
	return bOk;
}

bool UVoxelWorldSubsystem::ReceiveLiveEntries(const TArray<uint8>& Bytes)
{
	if (bJoinSyncInProgress)
	{
		BufferedLiveEntryBatches.Add(Bytes);
		return true;
	}
	return ApplyReplicatedEntries(Bytes);
}

uint32 UVoxelWorldSubsystem::GetWorldGenVersion() const
{
	return vxc::kWorldGenVersion;
}

uint64 UVoxelWorldSubsystem::ComputeHandshakeDigest() const
{
	if (!Impl)
	{
		return 0;
	}
	// 16 fixed Amplifier columns (m3-plan.md "Determinism guard"): spread
	// across a few km (arbitrary-but-fixed, not axis-aligned/round-number
	// spacing) so the probe is sensitive to amplifier/tile drift generally,
	// not just one column that happens to be lucky.
	vxc::Digest D;
	D.u64(Seed);
	D.u32(vxc::kWorldGenVersion);
	for (int32 i = 0; i < 16; ++i)
	{
		const int64 Vx = int64(i) * 4001;
		const int64 Vy = int64(i) * 2003;
		const vxc::ColumnSample Col = Impl->Voxels.amplifier().column(Vx, Vy);
		D.u32(static_cast<uint32_t>(Col.surfaceMm));
		D.u32(static_cast<uint32_t>(Col.topsoilMm));
		D.u32(static_cast<uint32_t>(Col.subsoilMm));
		D.u32(static_cast<uint32_t>(Col.bedrockDepthMm));
		D.u8(Col.surfaceMat);
	}
	return D.h;
}

uint64 UVoxelWorldSubsystem::GetEditedDigest() const
{
	return Impl ? Impl->Voxels.editedDigest() : 0;
}

bool UVoxelWorldSubsystem::SaveWorld() const
{
	if (!Impl)
	{
		return false;
	}
	const UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
	if (NetMode == NM_Client)
	{
		UE_LOG(LogVoxelEdit, Warning,
		       TEXT("SaveWorld: refused on NM_Client -- only the authority (server/listen/standalone) has a log to save."));
		return false;
	}
	return SaveEditLogToDisk(*Impl, Seed);
}

namespace
{
// M3 gate ("two clients dig the same hole", docs/m3-plan.md): a console
// command alternative to the -VoxelDumpDigestAfter=<s> command-line switch
// (AVoxelEarthGameMode / AVoxelEarthPlayerController) for interactive
// verification (PIE/editor console) -- same underlying value either way.
FAutoConsoleCommandWithWorld CVarVoxelDumpEditedDigest(
    TEXT("voxel.DumpEditedDigest"),
    TEXT("Logs this process's seed + World::editedDigest() (M3 gate: compare this value across server/client log files)."),
    FConsoleCommandWithWorldDelegate::CreateLambda(
        [](UWorld* World)
        {
	        UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	        if (!Subsystem)
	        {
		        UE_LOG(LogVoxelEarth, Warning, TEXT("voxel.DumpEditedDigest: no UVoxelWorldSubsystem in this world."));
		        return;
	        }
	        UE_LOG(LogVoxelEarth, Log, TEXT("VoxelDigestDump: role=Console seed=%llu editedDigest=0x%016llX"),
	               (unsigned long long)Subsystem->GetSeed(), (unsigned long long)Subsystem->GetEditedDigest());
        }));

// M3 wave 2 persistence (docs/m3-plan.md "Save/load"): interactive
// alternative to the -VoxelSaveWorldAfter=<s> command-line switch
// (AVoxelEarthGameMode) -- same UVoxelWorldSubsystem::SaveWorld() call
// either way.
FAutoConsoleCommandWithWorld CVarVoxelSaveWorld(
    TEXT("voxel.SaveWorld"),
    TEXT("Saves the authoritative edit log to Saved/VoxelWorlds/<seed>.vxlog (M3 wave 2 persistence; no-op with a ")
    TEXT("warning on NM_Client)."),
    FConsoleCommandWithWorldDelegate::CreateLambda(
        [](UWorld* World)
        {
	        UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	        if (!Subsystem)
	        {
		        UE_LOG(LogVoxelEarth, Warning, TEXT("voxel.SaveWorld: no UVoxelWorldSubsystem in this world."));
		        return;
	        }
	        Subsystem->SaveWorld();
        }));
} // namespace
