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
#include "VoxelEditRelay.h"

// voxel-core is UE-header-free C++20; safe to include directly from a UE
// module .cpp (doctrine: never from a header UHT parses -- see
// VoxelWorldSubsystem.h / VoxelChunkComponent.h, both voxel-core-free).
#include "voxelcore/bytes.h"
#include "voxelcore/counters.h"
#include "voxelcore/editcompact.h" // M3 wave 2: compactLog for save-to-disk + join-sync compaction
#include "voxelcore/hash.h"
#include "voxelcore/mesher.h"
#include "voxelcore/mips.h"
#include "voxelcore/raycast.h"
#include "voxelcore/tiles.h"
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
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h" // M3 wave 2: Saved/VoxelWorlds/<seed>.vxlog read/write
#include "Misc/Parse.h"
#include "Misc/Paths.h" // M3 wave 2: FPaths::ProjectSavedDir
#include "Misc/ScopeRWLock.h"
#include "Materials/MaterialInterface.h"
#include "Stats/Stats.h"
#include "Tasks/Task.h"
#include "UObject/UObjectGlobals.h"

#include <algorithm>
#include <atomic>
#include <unordered_map>
#include <vector>

// VoxelCoords.h intentionally duplicates this constant (in UU) so it stays
// voxel-core-free; check the two never drift apart.
static_assert(vxc::kVoxelSizeMm == int32(VoxelCoords::VoxelSizeUU) * 10,
              "VoxelCoords::VoxelSizeUU (UE units) must track vxc::kVoxelSizeMm (mm)");

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
} // namespace

// FVoxelWorldImpl -- the voxel-core side of the subsystem, defined only here
// so VoxelWorldSubsystem.h (UHT-parsed) never sees a voxel-core header. Also
// owns ALL Stage 2 streaming state (chunk records, pending-work queues, the
// worker-result MPSC queue, in-flight task handles): none of it is
// UE-reflection-visible, so it belongs behind the same PImpl boundary.
struct FVoxelWorldImpl
{
	explicit FVoxelWorldImpl(uint64 Seed)
		: Tiles(Seed)
		, Voxels(Seed, Tiles)
	{
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

	vxc::SyntheticTileSampler Tiles;
	vxc::World<VoxelCoords::BrickEdgeVoxels> Voxels;

	// --- Stage 2 streaming state (docs/m1-plan.md Stage 2 decisions table);
	// M2 (docs/m2-plan.md "Ring streaming" row) generalizes every key here
	// from FVoxelChunkKey to FVoxelLevelChunkKey (level, chunk) -- level 0
	// keys behave identically to the pre-M2 single-ring scheme. ---

	TMap<VoxelCoords::FVoxelLevelChunkKey, VoxelStreaming::FChunkRecord> ChunkRecords;

	// Nearest-first-within-level, lower-level-wins-ties priority queues
	// (docs/m2-plan.md item 1: "Budgets shared across levels, nearest-first
	// within level, lower level (finer) wins priority at equal distance").
	// Sorted so Pop() (O(1), removes from the back) always yields the
	// highest-priority pending chunk -- see SortPendingQueues.
	TArray<VoxelCoords::FVoxelLevelChunkKey> PendingJobKeys;        // worker-dispatch pending (any level, minus edited-ancestor chunks)
	TArray<VoxelCoords::FVoxelLevelChunkKey> PendingGameThreadKeys; // overlay-aware game-thread mesh pending -- ANY level as of
	                                                                 // M2 wave 2 (see MarkChunkDirtyForRemesh/PropagateEditToMips)
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

	// Per-level entry-scan gating (docs/m2-plan.md "Perf budget" row: "Ring
	// levels ... are ~constant cost by construction"): re-running the O(candidates)
	// entry scan for every level on every level-0 chunk crossing (every 3.2m
	// of movement) would waste work for outer levels whose own chunk edge is
	// much larger -- level L only re-scans once the anchor has crossed into a
	// new LEVEL-L chunk. The hysteresis/unload pass (cheap: iterates existing
	// ChunkRecords, no candidate generation) still runs every call.
	bool bHasRecomputedLevel[VoxelCoords::kNumLevels] = {};
	VoxelCoords::FVoxelChunkKey LastAnchorChunkPerLevel[VoxelCoords::kNumLevels] = {};

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

	// 1Hz refresh window (P1 "1Hz refresh of text, per-frame collection"):
	// per-frame accumulation happens continuously above; the published
	// snapshot itself only updates once this timer rolls over.
	float PerfRefreshAccumSeconds = 0.f;
	int64 LoadedAtLastPerfRefresh = 0;
	int64 UnloadedAtLastPerfRefresh = 0;
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
	void ComputeFootprintChunkZRange(int32 ChunkX, int32 ChunkY, int32 Level, int32& OutChunkZMin, int32& OutChunkZMax) const;
	void SortPendingQueues(const FVector& Anchor);
	void DispatchJobs();
	void DrainResults(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material);
	void DrainGameThreadMesh(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material);
	void DrainUnloads();
	void ApplyMeshResult(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material,
	                      const VoxelCoords::FVoxelLevelChunkKey& Key, VoxelStreaming::FChunkRecord& Rec, TArray<FVoxelChunkQuad>&& Quads,
	                      bool bIsGameThreadMesh);
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

	// Recompute the desired set only when the anchor crosses into a new
	// render-chunk XY column (every ChunkEdgeUU = 3.2m of movement), not
	// every tick -- the ring-membership test itself (docs/m1-plan.md: "render
	// chunks (XY footprints) within 64m") is cheap per-candidate but touches
	// ~1300+ candidate footprints at a 64m/3.2m ratio, so gating it behind
	// anchor movement is what keeps this off the hot per-frame path.
	const FVoxelChunkKey AnchorChunk = ChunkKeyForVoxel(WorldToVoxel(Anchor));
	if (!bHasRecomputed || AnchorChunk.X != LastAnchorChunk.X || AnchorChunk.Y != LastAnchorChunk.Y)
	{
		RecomputeDesiredSet(Anchor);
		LastAnchorChunk = AnchorChunk;
		bHasRecomputed = true;
	}

	// Budgets (docs/m1-plan.md Stage 2 decisions table): jobs in flight
	// <=2xLogicalCores, chunk component applies <=8/frame, unloads <=4/frame,
	// edit re-meshes <=4/frame (also covers first load of an edited chunk --
	// see PendingGameThreadKeys comment above).
	DispatchJobs();
	DrainResults(Owner, Root, Material);
	DrainGameThreadMesh(Owner, Root, Material);
	DrainUnloads();

	InFlightTasks.RemoveAllSwap([](const UE::Tasks::TTask<void>& T) { return T.IsCompleted(); }, EAllowShrinking::No);

	// docs/debug-tooling-plan.md P1 "Stats group": always-on DWORD counters
	// (STATS macros already compile to nothing in Shipping; no extra gate).
	SET_DWORD_STAT(STAT_VoxelChunksLoaded, ChunkRecords.Num());
	SET_DWORD_STAT(STAT_VoxelChunksInFlight, JobsInFlightCounter.GetValue());

	MaybeLogCounters(DeltaTime);

	// Constraint: "keep all debug work zero-cost when voxel.Debug=0 (branch
	// out early)" -- FVoxelPerfSnapshot collection (array iteration, once/sec
	// sort) is real work beyond the always-on STATS macros above, so it's
	// gated on mode >= 1 (the perf HUD's own activation threshold) rather
	// than running unconditionally.
	if (VoxelDebug::GetDebugMode() >= 1)
	{
		const float TickMs = float((FPlatformTime::Seconds() - TickStartSeconds) * 1000.0);
		UpdatePerfSnapshot(DeltaTime, TickMs);
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
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel streaming: loaded=%lld unloaded=%lld quads=%lld tracked=%d jobsInFlight=%d pendingJobs=%d ")
	       TEXT("pendingGameThread=%d pendingUnload=%d"),
	       (long long)TotalChunksLoaded, (long long)TotalChunksUnloaded, (long long)TotalQuadsLoaded, ChunkRecords.Num(),
	       JobsInFlightCounter.GetValue(), PendingJobKeys.Num(), PendingGameThreadKeys.Num(), PendingUnloadKeys.Num());

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
}

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
	const int64 TopVoxelMinAtLevel = FloorDiv(TopVoxelMin, LevelScale);
	const int64 TopVoxelMaxAtLevel = FloorDiv(TopVoxelMax, LevelScale);
	OutChunkZMin = (int32)FloorDiv(TopVoxelMinAtLevel, ChunkEdgeVoxels) - 1;
	OutChunkZMax = (int32)FloorDiv(TopVoxelMaxAtLevel, ChunkEdgeVoxels) + 2;
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
	const auto Farthest = [&](const VoxelCoords::FVoxelLevelChunkKey& A, const VoxelCoords::FVoxelLevelChunkKey& B)
	{
		const double DistA = DistSq(A);
		const double DistB = DistSq(B);
		if (DistA != DistB)
		{
			return DistA > DistB; // farther = lower priority = sorts toward the front
		}
		return A.Level > B.Level; // exact-distance tie: higher level number = lower priority
	};
	PendingJobKeys.Sort(Farthest);
	PendingGameThreadKeys.Sort(Farthest);
}

void FVoxelWorldImpl::RecomputeDesiredSet(const FVector& Anchor)
{
	using namespace VoxelCoords;

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
		if (bBeyondOuter || bInsideInner)
		{
			PendingUnloadKeys.Add(LevelKey);
			PendingUnloadSet.Add(LevelKey);
			PendingJobKeys.RemoveSingle(LevelKey);
			PendingGameThreadKeys.RemoveSingle(LevelKey);
		}
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
	for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
	{
		const FVoxelCoord AnchorVoxel = WorldToVoxelForLevel(Anchor, Level);
		const FVoxelChunkKey AnchorChunk = ChunkKeyForVoxel(AnchorVoxel);
		if (bHasRecomputedLevel[Level] && AnchorChunk.X == LastAnchorChunkPerLevel[Level].X &&
		    AnchorChunk.Y == LastAnchorChunkPerLevel[Level].Y)
		{
			continue; // nothing new can have entered this level's annulus
		}
		LastAnchorChunkPerLevel[Level] = AnchorChunk;
		bHasRecomputedLevel[Level] = true;

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

				int32 ChunkZMin, ChunkZMax;
				ComputeFootprintChunkZRange(Cx, Cy, Level, ChunkZMin, ChunkZMax);

				for (int32 Cz = ChunkZMin; Cz <= ChunkZMax; ++Cz)
				{
					const FVoxelLevelChunkKey LevelKey{Level, FVoxelChunkKey{Cx, Cy, Cz}};
					if (ChunkRecords.Contains(LevelKey))
					{
						continue;
					}

					ChunkRecords.Add(LevelKey);
					if (NeedsOverlayAwarePath(LevelKey))
					{
						PendingGameThreadKeys.Add(LevelKey);
					}
					else
					{
						PendingJobKeys.Add(LevelKey);
					}
				}
			}
		}
	}

	SortPendingQueues(Anchor);
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

void FVoxelWorldImpl::ApplyMeshResult(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material,
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
		// the last exposed faces): drop any stale component instead of
		// spawning/keeping an empty one.
		if (UVoxelChunkComponent* Existing = Rec.Component.Get())
		{
			Existing->DestroyComponent();
		}
		Rec.Component = nullptr;
		return;
	}

	UVoxelChunkComponent* Comp = Rec.Component.Get();
	const bool bWasFirstLoad = (Comp == nullptr);
	if (!Comp)
	{
		Comp = NewObject<UVoxelChunkComponent>(&Owner);
		Comp->SetupAttachment(&Root);
		Comp->SetLevel(Key.Level); // M2: fixed for the component's lifetime, see SetLevel doc comment
		Comp->SetRelativeLocation(VoxelCoords::ChunkOriginWorldForLevel(Key.Key, Key.Level));
		Comp->SetMaterial(0, Material);
		Comp->RegisterComponent();
		Rec.Component = Comp;
		++TotalChunksLoaded;
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
}

void FVoxelWorldImpl::DrainResults(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material)
{
	// docs/m1-plan.md Stage 2 decisions table: "<=8 chunk component
	// applies/frame."
	constexpr int32 MaxApplies = 8;
	int32 Applied = 0;
	VoxelStreaming::FJobResult Result;
	while (Applied < MaxApplies && ResultsQueue.Dequeue(Result))
	{
		++Applied;

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
		// dispatched with -- MarkChunkDirtyForRemesh bumped it).
		if (!Rec || Rec->GenerationId != Result.GenerationId)
		{
			++StaleResultsDiscarded;
			continue;
		}

		Rec->bJobInFlight = false;
		ApplyMeshResult(Owner, Root, Material, Result.Key, *Rec, MoveTemp(Result.Quads), /*bIsGameThreadMesh*/ false);
	}
	LastAppliedFrac = float(Applied) / float(MaxApplies);
}

void FVoxelWorldImpl::DrainGameThreadMesh(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material)
{
	// docs/m1-plan.md Stage 2 decisions table: "<=4 edit re-meshes/frame."
	// This budget covers both first-time load of a chunk that already
	// contains an edited brick (ChunkHasEditedBrick routed it here in
	// RecomputeDesiredSet/DispatchJobs) and a post-edit dirty re-mesh
	// (MarkChunkDirtyForRemesh) -- both use the identical overlay-aware
	// game-thread mesh path, so they share one budget rather than two.
	constexpr int32 MaxRemeshes = 4;
	int32 Count = 0;
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
		ApplyMeshResult(Owner, Root, Material, LevelKey, *Rec, MoveTemp(Quads), /*bIsGameThreadMesh*/ true);
	}
	LastRemeshFrac = float(Count) / float(MaxRemeshes);
}

void FVoxelWorldImpl::DrainUnloads()
{
	// docs/m1-plan.md Stage 2 decisions table: "<=4 unloads/frame."
	constexpr int32 MaxUnloads = 4;
	int32 Count = 0;
	while (Count < MaxUnloads && PendingUnloadKeys.Num() > 0)
	{
		const VoxelCoords::FVoxelLevelChunkKey Key = PendingUnloadKeys.Pop(EAllowShrinking::No);
		PendingUnloadSet.Remove(Key);
		++Count;

		VoxelStreaming::FChunkRecord Rec;
		if (ChunkRecords.RemoveAndCopyValue(Key, Rec))
		{
			// Any worker job still in flight for this key keeps running to
			// completion (it can't be cancelled); when its result arrives,
			// DrainResults finds no record for the key and discards it.
			if (UVoxelChunkComponent* Comp = Rec.Component.Get())
			{
				Comp->DestroyComponent();
			}
			ResidentQuads -= Rec.LastQuadCount;
			++TotalChunksUnloaded;
		}
	}
	LastUnloadFrac = float(Count) / float(MaxUnloads);
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

	Impl = MakeUnique<FVoxelWorldImpl>(Seed);
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

	const bool bApplied = Impl->TryDig(CameraWorldLocation, CameraWorldDirection, SizeVoxels);
	if (bApplied && World && (NetMode == NM_DedicatedServer || NetMode == NM_ListenServer))
	{
		BroadcastNewEntries(*Impl, *World);
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

	const bool bApplied = Impl->TryPlace(CameraWorldLocation, CameraWorldDirection, SizeVoxels, MaterialId, PlayerActorLocation);
	if (bApplied && World && (NetMode == NM_DedicatedServer || NetMode == NM_ListenServer))
	{
		BroadcastNewEntries(*Impl, *World);
	}
	return bApplied;
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

	const int32 Removed = Impl->CarveSphere(CenterUU, RadiusUU, JitterUU);
	if (Removed > 0 && World && (NetMode == NM_DedicatedServer || NetMode == NM_ListenServer))
	{
		BroadcastNewEntries(*Impl, *World);
	}
	return Removed;
}

FVoxelPerfSnapshot UVoxelWorldSubsystem::GetPerfSnapshot() const
{
	return Impl ? Impl->GetPerfSnapshot() : FVoxelPerfSnapshot{};
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
