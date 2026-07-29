#include "VoxelWorldSubsystem.h"

#include "VoxelChunkComponent.h"
// MeshChunkBricks + ERingSkirtFace used to live in this file's anonymous
// namespace. They moved to their own header so the GPU/CPU parity harnesses can
// call the shipping mesher instead of a transcription of it; nothing about the
// function changed.
#include "VoxelChunkMesher.h"
#include "VoxelClimateProbe.h"
#include "VoxelGpuPoolComponent.h"
#include "VoxelGpuMeshJobManager.h" // Wave D / D4: the async GPU mesh runner, forked into DispatchJobs
#include "VoxelGpuRegionBuild.h"    // FillRasterWindow -- shared with both GPU verify harnesses
#include "VoxelCoords.h"
#include "VoxelDebug.h"
#include "VoxelEarth.h"
#include "VoxelFineTileStreamer.h" // Phase 2 fine-tier residency/prefetch/eviction gate (-VoxelFineTileDir=)
#include "VoxelFootprintBand.h" // FFootprintBand + the two per-column reductions (lifted so the GPU gate can call them)
#include "VoxelGI.h" // pooled-path light field ingest (the component path reaches it via SetChunkQuads)
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
// -VoxelCavernShot's unattended capture (FScreenshotRequest) and camera
// readback (APlayerCameraManager). Ordinary engine headers, not the voxel-core
// boundary.
#include "Camera/PlayerCameraManager.h"
#include "UnrealClient.h"

#include "voxelcore/bytes.h"
// -VoxelCavernShot searches for natural cavern rooms via ColumnSample::cavern
// (CavernSeg::marginSq); amplifier.h already pulls this in, included
// explicitly because this file names the type.
#include "voxelcore/caverns.h"
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
#include "Misc/ConfigCacheIni.h" // DefaultTileDir ini fallback (GConfig/GGameIni) in Initialize()
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

// QueryThreadCycleTime for the worker-job CPU-cycle probe (FJobResult::JobCycles
// -- see its doc comment for why WALL time alone cannot tell contention from
// work). The only Windows API this module calls; everything else goes through
// FPlatformTime. Guarded and stubbed below so a non-Windows build simply
// reports zero cycles and the wall-time numbers are unaffected.
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <processthreadsapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
// This thread's retired CPU cycles, or 0 where the platform cannot answer.
// Cheap (a user-mode read of the thread's own counter), so it can sit on the
// worker hot path without changing what it measures.
inline uint64 VoxelThreadCycles()
{
#if PLATFORM_WINDOWS
	uint64 Cycles = 0;
	if (::QueryThreadCycleTime(::GetCurrentThread(), &Cycles))
	{
		return Cycles;
	}
#endif
	return 0;
}
} // namespace

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
// MeshChunkBricks and ERingSkirtFace now live in VoxelChunkMesher.h (included
// at the top of this file). Nothing about them changed; they moved so the
// GPU/CPU parity harnesses can call the shipping mesher rather than a copy.

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

	// CoarseLevel == 0 is the historical behaviour (level-0 column grids for the
	// mip recursion's level-0 source). CoarseLevel > 0 makes this a cache of
	// COARSE column grids at that level (vxc::GeneratedWorld::coarseColumns),
	// which is what MakeCoarseLevelSampler needs -- same B*B amplifier column
	// evaluations per miss at ANY level, which is the whole point of the coarse
	// path. voxel-core guarantees coarseColumns(0,...) == columns(...)
	// bit-identically, but the branch is kept explicit so the level-0 mip source
	// provably takes the exact same call it always did.
	FJobColumnGridCache(const GeneratedWorldT& InGen, vxc::Counters* InPerfCounters, int32 InCoarseLevel = 0)
		: Gen(InGen), PerfCounters(InPerfCounters), CoarseLevel(InCoarseLevel)
	{
	}

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
		GeneratedWorldT::ColumnGrid& NewGrid =
			Grids.Add(PackedKey, CoarseLevel == 0 ? Gen.columns(Bx, By) : Gen.coarseColumns(CoarseLevel, Bx, By));
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
	int32 CoarseLevel = 0;
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

// Direct COARSE level-L sampler -- the reason distant rings can exist at all.
//
// MakeLevelSampler above is correct but its cost is structural: a level-L brick
// is folded from 8^L level-0 bricks, so it scales 8x per level. Measured on this
// box (60s flight, real diffusion tiles, seed 20260719, before this function
// existed): R0 p50 1.32ms, R1 2.44ms, R2 23.88ms, R3 1543ms, R4 15901ms. At R4 a
// single chunk holds a worker slot for SIXTEEN SECONDS, so R3/R4 reported
// loaded=0 at every 5s sample of the whole flight -- the nominal 1024m cascade
// actually terminated near 250m (the admission cutoff collapsed to ~200m under
// the resulting queue backlog) and everything beyond was clipmap. That is the
// "low poly vista".
//
// vxc::GeneratedWorld::makeCoarseBrick/coarseColumns (voxelcore/generator.h)
// generate a level-L brick DIRECTLY: coarseColumns does B*B amplifier column
// evaluations and makeCoarseBrick does B^3 materialAt calls, at ANY level. So a
// level-L chunk costs what a level-0 chunk costs, independent of L. That is what
// makes rings past R2 viable.
//
// Semantics differ deliberately and this is not a bug: coarse is nearest-
// neighbour sampling of the generator at the coarse cell centre, mip is a
// majority vote over children (generator.h documents them as separate paths with
// separate goldens; coarse != downsample in general). For distant LOD the coarse
// rule is arguably the better one -- it tracks the true surface instead of
// eroding thin features through repeated majority votes.
//
// No FSharedMipCache here, on purpose. That cache's key is (level, BrickKey) and
// its documented contract is "vxc::downsampleBricks output"; a coarse brick is a
// DIFFERENT function of the same key, so sharing the cache would mix two rules
// under one key. Coarse bricks are cheap enough not to need cross-job caching --
// the job-local map below is sufficient. Worker path only (pure GeneratedWorld,
// never World's overlay), same lock-free rule as MakeLevelSampler.
std::function<vxc::MaterialId(int64, int64, int64)> MakeCoarseLevelSampler(const vxc::GeneratedWorld<VoxelCoords::BrickEdgeVoxels>& Gen,
                                                                             int32 Level, vxc::Counters* PerfCounters)
{
	using namespace vxc;
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;

	struct FJobCoarseState
	{
		FJobCoarseState(const GeneratedWorld<B>& InGen, Counters* InPerfCounters, int32 InLevel)
			: ColumnCache(InGen, InPerfCounters, InLevel), Gen(InGen), Level(InLevel)
		{
		}

		const Brick<B>* Brick(const BrickKey& Key)
		{
			auto Found = Bricks.find(Key);
			if (Found != Bricks.end())
			{
				return &Found->second;
			}
			const auto& Grid = ColumnCache.Get(Key.x, Key.y);
			auto [It, Inserted] = Bricks.emplace(Key, Gen.makeCoarseBrick(Level, Key, Grid));
			(void)Inserted;
			return &It->second;
		}

		FJobColumnGridCache ColumnCache;
		const GeneratedWorld<B>& Gen;
		int32 Level;
		std::unordered_map<BrickKey, vxc::Brick<B>, BrickKeyHash> Bricks;
	};

	TSharedPtr<FJobCoarseState> State = MakeShared<FJobCoarseState>(Gen, PerfCounters, Level);

	return [State](int64 X, int64 Y, int64 Z) -> MaterialId
	{
		const BrickKey BKey{int32_t(floorDiv(X, B)), int32_t(floorDiv(Y, B)), int32_t(floorDiv(Z, B))};
		const vxc::Brick<B>* Br = State->Brick(BKey);
		if (!Br)
		{
			return MAT_AIR;
		}
		return Br->get(int(floorMod(X, B)), int(floorMod(Y, B)), int(floorMod(Z, B)));
	};
}

// COARSE flat-grid sampler -- MakeCoarseLevelSampler's output with the
// indirection removed. Same rule, same numbers, ~3x cheaper per chunk.
//
// Unfold what the sampler above actually computes for absolute level-L voxel
// (X, Y, Z). Its brick key is (floorDiv(X,B), floorDiv(Y,B), floorDiv(Z,B)) and
// its local index is (floorMod(X,B), ...), so key*B + local == X on every axis.
// coarseColumns(L, bx, by) fills grid.at(lx,ly) with
// amp.column(coarseRep(bx*B+lx, L), coarseRep(by*B+ly, L)); makeCoarseBrick
// writes cell (lx,ly,lz) = Amplifier::materialAt(grid.at(lx,ly),
// coarseRep(bz*B+lz, L)) (generator.h:117-142), and Brick::set/get round-trip a
// MaterialId exactly (palette lookup, no quantization -- brick.h:56-76).
// Substituting: the sampler is EXACTLY
//
//     materialAt(amp.column(coarseRep(X,L), coarseRep(Y,L)), coarseRep(Z,L))
//
// with no dependence on brick boundaries at all. So the whole Brick<8> layer is
// a cache of a function that is cheaper to call than to cache: one (32+2)^2
// column grid covers a render chunk plus its mesher apron, and every voxel query
// is then an array index and one materialAt. What this deletes per chunk is the
// std::function dispatch, three floorDiv + three floorMod, an unordered_map
// probe per voxel, and the eager 512-cell fill + palette + tryCollapse of up to
// ~216 bricks (~2,300 amplifier columns) -- which is why coarse jobs cost
// 4.4-5.3 ms while a level-0 job doing the same amount of GENERATION costs 1.55.
//
// Not a std::function: this is passed to MeshChunkBricks by reference as a
// concrete functor, so the per-voxel call inlines the way the level-0 lambda
// does. -VoxelCoarseGridVerify meshes both ways and compares the quads.
struct FCoarseChunkGridSampler
{
	using GenT = vxc::GeneratedWorld<VoxelCoords::BrickEdgeVoxels>;
	static constexpr int32 ChunkVox = VoxelCoords::ChunkEdgeVoxels;
	static constexpr int32 GridEdge = ChunkVox + 2; // +1 apron each side (mesher's [-1,B] contract)

	FCoarseChunkGridSampler(const GenT& Gen, int32 InLevel, const VoxelCoords::FVoxelChunkKey& Key,
	                        vxc::Counters* PerfCounters)
		: Level(InLevel)
		, BaseVX(int64(Key.X) * ChunkVox)
		, BaseVY(int64(Key.Y) * ChunkVox)
	{
		Columns.SetNumUninitialized(GridEdge * GridEdge);
		const vxc::Amplifier& Amp = Gen.amplifier();
		for (int32 LY = 0; LY < GridEdge; ++LY)
		{
			const int64 CY = GenT::coarseRep(BaseVY + LY - 1, Level);
			for (int32 LX = 0; LX < GridEdge; ++LX)
			{
				Columns[LX + GridEdge * LY] = Amp.column(GenT::coarseRep(BaseVX + LX - 1, Level), CY);
			}
		}
		// Same unit the level-0 job and FJobColumnGridCache report: explicit
		// Amplifier::column() calls made by this job.
		if (PerfCounters)
		{
			PerfCounters->incColumnEvals(uint64_t(GridEdge) * GridEdge);
		}
	}

	vxc::MaterialId operator()(int64 X, int64 Y, int64 Z) const
	{
		const int32 LX = int32(X - BaseVX) + 1;
		const int32 LY = int32(Y - BaseVY) + 1;
		checkSlow(LX >= 0 && LX < GridEdge && LY >= 0 && LY < GridEdge);
		return vxc::Amplifier::materialAt(Columns[LX + GridEdge * LY], GenT::coarseRep(Z, Level));
	}

	int32 Level;
	int64 BaseVX;
	int64 BaseVY;
	TArray<vxc::ColumnSample> Columns;
};

// Field-by-field quad equality for -VoxelCoarseGridVerify. Compares the emitted
// arrays in ORDER: both paths run the identical MeshChunkBricks traversal, so a
// pure refactor must reproduce the same quads in the same sequence, not merely
// the same set.
bool VoxelChunkQuadsIdentical(const TArray<FVoxelChunkQuad>& A, const TArray<FVoxelChunkQuad>& Bq, int32& OutFirstDiff)
{
	OutFirstDiff = -1;
	if (A.Num() != Bq.Num())
	{
		OutFirstDiff = FMath::Min(A.Num(), Bq.Num());
		return false;
	}
	for (int32 I = 0; I < A.Num(); ++I)
	{
		const FVoxelChunkQuad& P = A[I];
		const FVoxelChunkQuad& Q = Bq[I];
		if (P.Axis != Q.Axis || P.Positive != Q.Positive || P.Slice != Q.Slice || P.U0 != Q.U0 || P.V0 != Q.V0 ||
		    P.W != Q.W || P.H != Q.H || P.Ao != Q.Ao || P.Mat != Q.Mat)
		{
			OutFirstDiff = I;
			return false;
		}
	}
	return true;
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
std::unique_ptr<vxc::ITileSampler> MakeTileSampler(uint64 Seed, const FString& TileDir, int32 TileScale,
                                                  bool& bOutUsingTileGrid, FBox2D& bOutCoverageUU)
{
	bOutUsingTileGrid = false;
	bOutCoverageUU = FBox2D(ForceInit);

	if (TileDir.IsEmpty())
	{
		return std::make_unique<vxc::SyntheticTileSampler>(Seed);
	}

	if (vxc::tilePixelSizeMm((uint8)TileScale) == 0)
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("-VoxelTileScale=%d is not a supported tile scale (only 1 [30m/px] or 8 [3.75m/px] are valid) -- ")
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

	// WHERE THE REAL TERRAIN ACTUALLY IS, in world UU.
	//
	// The bounding box above was already logged in TILE coordinates, which is
	// unreadable next to a spawn position and is why nobody noticed that the
	// default spawn sits outside it. A tile is kTileSize pixels and a pixel is
	// tilePixelSizeMm at this scale, so the conversion is exact.
	{
		const double TilePx = double(vxc::TileData::kTileSize);
		const double PxUU = double(vxc::tilePixelSizeMm((uint8)TileScale)) / 10.0;  // mm -> UU (1 UU = 1 cm)
		bOutCoverageUU = FBox2D(
			FVector2D(double(MinTx) * TilePx * PxUU, double(MinTy) * TilePx * PxUU),
			FVector2D(double(MaxTx + 1) * TilePx * PxUU, double(MaxTy + 1) * TilePx * PxUU));
		const FVector2D CentreUU = bOutCoverageUU.GetCenter();
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("Voxel tile grid: real terrain covers world X [%.0f, %.0f] m, Y [%.0f, %.0f] m "
		            "-- centre (%.0f, %.0f) m. Outside this box every elevation query returns the "
		            "missing-tile sea-level default."),
		       bOutCoverageUU.Min.X / 100.0, bOutCoverageUU.Max.X / 100.0,
		       bOutCoverageUU.Min.Y / 100.0, bOutCoverageUU.Max.Y / 100.0,
		       CentreUU.X / 100.0, CentreUU.Y / 100.0);
	}

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
	//
	// Null under voxel.Stream.GPU (ADR-0006 G3) -- there is no per-chunk
	// component on that path. Ask HoldsGeometry(), never Component directly,
	// anywhere the question is "is this chunk currently drawn".
	TWeakObjectPtr<UVoxelChunkComponent> Component;

	// This chunk's range in the GPU geometry pool, or INDEX_NONE. The pooled
	// counterpart of Component: exactly one of the two is ever set, decided by
	// voxel.Stream.GPU at the moment the chunk loaded. Storing which one is
	// what lets a mid-session cvar flip stay harmless -- a chunk unloads
	// through the same path it loaded through.
	int32 PoolSlot = INDEX_NONE;

	// Is this chunk currently holding drawn geometry, on either path? The
	// unload budget, the retention gate and the empty-chunk release sites all
	// turn on this and must not care which renderer is in play.
	bool HoldsGeometry() const { return Component.IsValid() || PoolSlot != INDEX_NONE; }

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

	// Load-before-unload (voxel.Stream.LodRetentionMs). When an LOD-ring
	// transition evicts this chunk while it is still VISIBLE (has an applied
	// component with geometry), RecomputeDesiredSet stamps this to
	// ElapsedSeconds + retention. DrainUnloads then keeps the chunk drawn (defers
	// the pool-park) until ElapsedSeconds passes it, giving the replacement LOD
	// time to stream in so no hole opens. -1000 = not retained (park immediately,
	// the pre-change behaviour and the path for genuinely-gone chunks once their
	// grace elapses).
	float RetainUntilSeconds = -1000.f;

	// Which LOD replaced this chunk when it was retained: RetainDir_Finer (a
	// finer ring took over from the inside) or RetainDir_Coarser (a coarser ring
	// from the outside). Tells ReplacementCovered which neighbour columns to
	// check. 0 (RetainDir_None) = not an LOD-transition retention. RetainUntil-
	// Seconds above is now a SAFETY CAP; the primary release is coverage-based.
	uint8 RetainReplaceDir = 0;

	// Load-before-unload coverage, Z-DESIREDNESS of the replacement keys
	// (ring-gap fix, 2026-07-27). Which of the replacement chunks
	// ReplacementCovered consults are actually WANTED in Z, decided ONCE here at
	// retention-stamp time and never re-derived per frame.
	//
	// WHY IT HAS TO BE PRE-COMPUTED. The fix makes an ABSENT replacement record
	// BLOCK the stand-in's release (see ReplacementCovered) -- but only when the
	// absent chunk was genuinely desired. Desiredness splits in two, and the two
	// halves have opposite cost profiles:
	//   Z   -- a pure function of the TERRAIN (ComputeFootprintChunkZRange), so
	//          it is anchor-INDEPENDENT and can be stamped once per eviction.
	//          It is also the expensive half (amplifier corner samples).
	//   XY  -- a function of the ANCHOR, which moves every frame, so it must be
	//          live. It is pure arithmetic, so that is affordable.
	// Stamping Z here and evaluating XY live in ReplacementCovered keeps the
	// amplifier off the per-frame DrainUnloads path entirely.
	//
	// LAYOUT. RetainDir_Finer: bit (dx + dy*2 + dz*4) is set iff child chunk
	// (2X+dx, 2Y+dy, 2Z+dz) at level L-1 lies inside that child FOOTPRINT's
	// desired chunk-Z range. RetainDir_Coarser: bit 0 only, for the single L+1
	// parent (X>>1, Y>>1, Z>>1).
	//
	// DEFAULTS TO ALL BITS SET, not 0, and the distinction is load-bearing.
	// "Never stamped" must read as "block on any absence" (the conservative
	// answer), while a genuine stamp of 0 -- every consulted chunk provably out
	// of Z range -- must read as "nothing to wait for" and release. A 0 default
	// would collide those two, and the collision is reachable: the coarser
	// direction consults exactly one key, so its honest mask is one bit. Making
	// the DEFAULT the conservative value leaves 0 free to mean what it says.
	// (Every retention stamp overwrites this unconditionally, in both
	// directions, so a re-evicted record never reuses a stale mask.)
	uint8 RetainChildZMask = 0xFF;

	// Load-before-unload coverage (ReplacementCovered). True once this chunk's
	// geometry is FINAL for its current generation: either ApplyMeshResult ran
	// (with or without quads), or a pre-dispatch skip proved it empty and it
	// will never be dispatched at all.
	//
	// "Settled", deliberately not "has geometry". A chunk that meshes to zero
	// quads still COVERS its footprint -- there is correctly nothing to draw
	// there -- so a replacement made of empty chunks must be able to release a
	// retained stand-in. Keying coverage on geometry (the 2026-07-24 first cut)
	// meant empty replacements never reported covered and always fell through
	// to the safety cap. See ReplacementCovered for the other half of that bug.
	bool bMeshSettled = false;
};

// --- Buried-chunk pre-dispatch skip -----------------------------------------
//
// FFootprintBand, BandProvesChunkEmpty, MakeFootprintBand and the two
// per-column reductions the band is a max/min of now live in
// VoxelFootprintBand.h, included above. They were lifted out of this file for
// the same reason MeshChunkBricks was: voxel.GPU.VerifyRegion gates
// BandReduceMain (Wave D / D6) against the SHIPPING reduction rather than a
// transcription of it, and it cannot do that while the reduction is buried in
// a .cpp. See that header for the full commentary.

struct FJobResult
{
	VoxelCoords::FVoxelLevelChunkKey Key;
	uint64 GenerationId = 0;
	TArray<FVoxelChunkQuad> Quads;
	float JobMs = 0.f; // wall time inside the worker task body (docs/debug-tooling-plan.md P1 "Worker timings")

	// --- Buried-chunk pre-dispatch skip, MEASUREMENT ONLY (-VoxelMeasureEmpty).
	// docs/status.md "Buried-chunk pre-dispatch skip" step 1: before building
	// any skip we must know what fraction of worker output meshes to zero
	// quads, split by ring level, and WHAT those chunks are -- all-air, all-
	// solid, or something subtler. Two prior leads on this project turned out
	// to be misdiagnoses, so nothing here is assumed.
	//
	// GridMs is the level-0 column-grid build (the (32+2)^2 Amplifier::column
	// calls) measured separately from the mesh, because which of the two
	// dominates decides what a skip can possibly save: if the mesher's own
	// early-outs already make a buried chunk nearly free, the only thing left
	// to reclaim is the column grid.
	float GridMs = 0.f;
	// CPU CYCLES this worker thread actually retired inside the job body, and
	// inside the column-grid build specifically (0 off Windows, or when the
	// query fails). Paired with JobMs/GridMs, which are WALL time, these are
	// what separate the two candidate explanations for level-0 job time nearly
	// doubling under motion (docs/status.md "Streaming under motion"):
	//
	//   * cycles/job roughly CONSTANT while ms/job doubles  -> the thread is not
	//     doing more work; it is being descheduled, or running at a lower clock.
	//     Contention/scheduling, and the fix is a concurrency or ordering one.
	//   * cycles/job RISING with ms/job                     -> the thread really
	//     is burning more cycles for the same 1156 columns, i.e. cache/memory
	//     pressure. The fix is a working-set one.
	//
	// The grid build is the ideal probe because its work is EXACTLY FIXED: every
	// level-0 job evaluates 34x34 = 1156 Amplifier::column calls, no more and no
	// fewer, whatever the chunk contains. Cycles per column is therefore a
	// like-for-like number across runs in a way that ms/job is not (chunk mix
	// moves the mesh half).
	//
	// QueryThreadCycleTime is a cheap user-mode read of the thread's own cycle
	// counter (two calls per job) and cannot perturb the number it reports the
	// way a sampling profiler would.
	uint64 JobCycles = 0;
	uint64 GridCycles = 0;
	// Per-brick skip census (level 0, -VoxelL0BrickSkip): of this chunk's 64
	// bricks, how many were proven to emit nothing before meshBrick was called,
	// split by which of the two proofs fired. See FNeverSkipBrick.
	uint16 BricksSkippedAir = 0;
	uint16 BricksSkippedSolid = 0;
	// 0 = produced quads, 1 = all air, 2 = all solid (no AIR in chunk+apron),
	// 3 = zero quads but neither (mixed with no visible face). Only filled in
	// when the measurement switch is on; otherwise stays 0.
	uint8 EmptyClass = 0;

	// Buried-chunk pre-dispatch skip: the level-0 footprint band this job
	// derived as a by-product of the column grid it had to build anyway (see
	// ComputeFootprintBand). Only level-0 jobs set this.
	bool bBandValid = false;
	FFootprintBand Band;

	// -VoxelVerifyBuriedSkip: this chunk's band verdict said "provably no
	// geometry", but it was dispatched anyway so the verdict can be checked
	// against the real mesh. Any result with bPredictedEmpty && QuadCount()>0
	// is a soundness violation -- geometry the skip would have deleted.
	bool bPredictedEmpty = false;

	// --- Wave D / D1: quads that never came to the CPU ----------------------
	//
	// Non-null when the GPU fork left this chunk's geometry in GPU memory. Quads
	// is then EMPTY and this handle is what ApplyMeshResult passes to
	// UVoxelGpuPoolComponent::AddChunkFromGpu. Always null on the worker path.
	//
	// Dropping it is always safe: a stale result, a full pool or a teardown
	// simply destroys it, which releases the GPU memory on the render thread.
	FVoxelGpuQuadPayloadRef GpuQuads;
	uint32 GpuQuadCount = 0;

	// HOW MUCH GEOMETRY THIS CHUNK MESHED TO. Use this, not Quads.Num(), for
	// every question that is about the AMOUNT of geometry rather than the
	// geometry itself -- the zero-quad census, the buried/solid/sky skip
	// soundness checks, ResidentQuads, LastQuadCount.
	//
	// The reason is a silent one. On the direct path Quads is empty for a chunk
	// that meshed to thousands of quads, so every one of those sites would read
	// it as an EMPTY chunk: the census would report the fork producing nothing,
	// the skip verifiers would stop being able to catch an unsound skip (they
	// only fire on a non-zero count), and LastQuadCount/ResidentQuads would
	// under-report residency while the terrain drew correctly. All
	// self-consistent, all wrong, and none of it visible on screen.
	int32 QuadCount() const
	{
		return GpuQuads.IsValid() ? int32(GpuQuadCount) : Quads.Num();
	}

	// S0-3 (docs/speculative-generation-plan.md Wave S0 / T0-2): DELIVER time --
	// the moment this result was handed to ResultsQueue by whichever producer
	// made it (OnGpuMeshJobComplete for the fork, the worker task body for the
	// CPU arm, both right before their ResultsQueue.Enqueue). DrainResults reads it once
	// it has decided to apply this result, right after the ApplyMeshResult()
	// call, to get DeliverToApplyMs: how long a finished result sat on
	// ResultsQueue before DrainResults's own per-frame budget let it through.
	// Deliberately NOT read inside ApplyMeshResult -- that function is being
	// edited concurrently elsewhere in this wave. 0.0 unless
	// voxel.Stream.LatencyStats is on (both producers gate the
	// FPlatformTime::Seconds() call that fills this), in which case it stays
	// "not measured" rather than a false zero -- same convention as
	// FVoxelGpuMeshJobResult::QueuedMs.
	double DeliverSeconds = 0.0;

	// S0-3: true when this result came from the GPU fork (OnGpuMeshJobComplete)
	// rather than the CPU worker task body. The two arms' latency windows are
	// kept separate (VoxelWorldSubsystem.cpp's GpuSubmitToDeliverMsWindow vs
	// CpuWorkerEndToEndMsWindow etc.) because JobMs means something different
	// on each arm -- the fork's SubmitToDeliverMs vs the worker's own task-body
	// wall time -- and the pre-existing WorkerJobMsWindow already mixes both
	// with no way to tell them apart after the fact.
	bool bFromGpuMesh = false;
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

// Wave D / D4: fork level-0, unedited chunk meshing off the worker pool and
// onto FVoxelGpuMeshJobManager.
//
// Command line rather than a cvar, for the same reason -VoxelPendingJobCap and
// -VoxelCoarseMinLevel are: -ExecCmds lands after streaming has already begun,
// so a cvar would fork a run half way through and make its cold-fill number a
// blend of two producers. This has to be decided before the first dispatch or
// the A/B it exists to serve cannot be taken.
//
// ON BY DEFAULT since 2026-07-27, which is why the switch below is the NEGATIVE
// -VoxelNoGpuMesh rather than an opt-in. It was off, "until the byte-equality
// gate and a motion measurement both pass"; both have since passed.
// voxel.GPU.VerifyCoarse proves the fork bit-exact against vxc::coarseColumns +
// makeCoarseBrick on columns, cells AND quads at every level it takes, and the
// D5.3b budget fix turned the whole cascade from 3.4x WORSE than the CPU path
// into ~32-38% faster -- see GpuMeshMaxLevel below for those numbers and the
// retraction that came with them.
//
// The pooled RENDERER is still a separate switch (voxel.Stream.GPU) and must
// stay one: this fork changes only WHO PRODUCES the quads, not who draws them,
// so the two are independent and either can be measured alone.
bool GpuMeshEnabled()
{
	// 2026-07-27: DEFAULT ON, and inverted to -VoxelNoGpuMesh so a PIE session
	// gets it. A PIE run launched from the editor has no command line of its
	// own, so an opt-IN FParse::Param switch can never reach it -- the fork
	// would have been unreachable in exactly the session that is meant to
	// evaluate it. Same idiom as -VoxelNoRingSkirt and -VoxelNoUnderground.
	static const bool bDisabled = FParse::Param(FCommandLine::Get(), TEXT("VoxelNoGpuMesh"));
	return !bDisabled;
}

// How many GPU mesh jobs may be outstanding at once, budgeted separately from
// the CPU worker cap. See the HasDispatchBudget comment in DispatchJobs for why
// this is a different quantity from JobsInFlightPerCore and not a bigger one,
// and GpuMeshMaxLevel below for the measurement that made the separation the
// difference between the fork being 3.4x slower than the CPU path and ~38%
// faster.
//
// 256 matches the manager's own cap, so neither binds before the other -- two
// caps in series make a throughput number unattributable to either, which is
// the same trap the ring floor sweep fell into.
int32 GpuMeshInFlight()
{
	// 256, TESTED AND KEPT (2026-07-27). The depth hypothesis -- fork
	// throughput = in-flight depth / round-trip latency, so 1024 should lift
	// ~600 chunks/s toward ~2,400 -- was tried at the adopted 128 m / 4 km
	// cascade and FALSIFIED: 590 chunks/s at 1024 (vs 602 at 256), with the
	// fork idling at ~11 in flight (never depth-bound at all) and the
	// submit->deliver MAX ballooning to 13,146 ms -- past the 10 s retention
	// cap, i.e. a correctness hazard, not just waste. Whatever pins the
	// new-stack loading rate at ~600/s at that cascade (the old
	// component-renderer arm reaches ~1,080/s), it is not this knob, not
	// MeshBatchCap (4/8 vs 16/32 moved 0.0), and not the mesher choice
	// (-VoxelNoGpuMesh arm also ~593/s). That plateau is the open P0 in
	// docs/measurements/gpu-throughput-wave-2026-07-27.txt.
	static const int32 N = []
	{
		int32 Value = 256;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuMeshInFlight="), Value);
		return FMath::Clamp(Value, 1, 4096);
	}();
	return N;
}

// Highest chunk level the fork may take (D5).
//
// FIVE BY DEFAULT -- THE WHOLE CASCADE -- AND THAT IS A MEASUREMENT, NOT
// CONFIDENCE. It was ZERO, on a measurement, and the history is kept here
// because the number that overturned it is small and easy to lose.
//
// Correctness was never the limit at any point: voxel.GPU.VerifyCoarse proves
// the coarse path bit-exact against vxc::coarseColumns + makeCoarseBrick on
// columns, cells AND quads -- levels 0-4 first, then L5 on [origin] and on the
// [high-relief] fixture found by voxel.GPU.FindBandFixture (2,025 voxels / 202 m
// of surface spread, against the three coarse bricks of z 768 require).
// [far-negative] cannot express L5 and is reported as SKIP rather than FAIL,
// because a fixture too thin to hold the level is not the coarse path
// disagreeing. A coarse run reports failed=0 across 105 log windows with no
// stranded columns. THROUGHPUT was the limit, and it has moved.
//
// WHAT WAS MEASURED (R0 = 128 m, same anchor, same harness, static pose,
// docs/measurements/wave-d5-budget-fix.txt):
//
//     128 m, CPU producer                      60.4 / 58.4 s to settle
//     128 m, fork L0-L4, SHARED job budget    205.4 s, second leg never settled
//     128 m, fork L0 only, separate budget     58.5 / 62.4 s  (~7,370 forked)
//     128 m, fork L0-L4,  separate budget      36.3 / 38.3 s  (~34,365 forked)
//
// 3.4x WORSE on the middle row, with the fork taking 51,720 chunks spread
// properly across the levels (L0 9201, L1 10280, L2 10464, L3 10892, L4 10883).
// It was latency, not error: submit->deliver max went from 90 ms at level 0 to
// 4,025 ms, and the fork sat at 19-20 in flight against a cap of 256 -- waiting,
// not saturated.
//
// THE CAUSE WAS A UNIT ERROR IN THE BUDGET, AND THE BATCHING DIAGNOSIS IS
// RETRACTED. The earlier text on this comment blamed BATCHING: a region dispatch
// is a brick-aligned slab with a one-brick halo, so meshing a single 32^3 chunk
// dispatches 48^3 voxels -- 3.4x waste -- and D3's split readback adds a second
// round trip. That waste is real and batching may still help, but it was not the
// blocker. MaxJobsInFlight is JobsInFlightPerCore x logical cores (24 on that
// box) and that number sizes a pool of WORKERS; a GPU job is not a thread, it is
// a round trip, so the queue has to be sized against latency x rate. Giving the
// fork its own budget (D5.3b) cost one line.
//
// BOTH CHANGES WERE REQUIRED AND NEITHER WORKS ALONE, which is what makes this a
// result rather than a coincidence: coarse levels without the budget fix is
// 205 s; the budget fix without coarse levels is 58.5/62.4 s against a CPU
// baseline of 58.4/60.4 s, i.e. no change at all. Levels 1-5 are ~80% of
// resident chunks, so a level-0-only fork cannot move the number however much
// budget it is given.
//
// RE-MEASURED AFTER THE RING-BOUNDARY SKIRT (D5.3) REMOVED THE BOUNDARY-CHUNK
// CARVE-OUT, i.e. with the fork taking strictly MORE work than the numbers above
// describe (docs/measurements/wave-d5-skirt-remeasure.txt):
//
//     128 m, CPU producer     62.5 / 62.4 s
//     128 m, fork L0-L5       42.4 / 42.3 / 42.4 s, then 40.4 / 42.4 s
//
// ~32% faster, spread 0.1 s within both configs. Compare WITHIN a session only:
// the CPU baseline itself moved between the two runs, so the honest statement is
// the ratio (0.62 pre-skirt, 0.68 post-skirt) -- still clearly ahead while doing
// more work.
//
// STILL OPEN: one 7,395 ms submit->deliver outlier, above the 5 s
// kBlindJobMarkTimeoutSeconds, seen once and not since across four further legs.
// A level-4 dispatch covers 16x the world area of a level-0 one, so the coarse
// chunks the skirt newly admits are the plausible source. The `slow(>=5s)`
// counter on the GPU-fork perf line now exists precisely to say so if it
// returns; it reads 0.
//
// -VoxelGpuMeshMaxLevel=<n> still lowers the cap for measurement (0 restores the
// level-0-only fork, and the clamp's upper bound is the highest level
// VerifyCoarse has proven).
int32 GpuMeshMaxLevel()
{
	static const int32 MaxLevel = []
	{
		int32 Value = 5;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuMeshMaxLevel="), Value);
		return FMath::Clamp(Value, 0, 5);
	}();
	return MaxLevel;
}

// Lowest chunk level that generates via the DIRECT COARSE path
// (MakeCoarseLevelSampler / vxc::GeneratedWorld::makeCoarseBrick) instead of the
// 8^L mip recursion (MakeLevelSampler). Levels below this keep the mip path;
// level 0 always has its own dedicated path and is unaffected either way.
//
// Default 1 = every mip level goes coarse, because the mip recursion's cost is
// what capped the effective voxel radius at ~250m (see MakeCoarseLevelSampler's
// comment for the measured per-level numbers). `-VoxelCoarseMinLevel=99`
// disables the coarse path entirely and restores the pre-change mip behaviour,
// which is how the A/B for this change is measured.
//
// Command-line, not a cvar, for the reason documented on -VoxelPendingJobCap
// above: -ExecCmds lands after streaming has already begun.
constexpr int32 kDefaultCoarseMinLevel = 1;

int32 GetCoarseMinLevel()
{
	static const int32 MinLevel = []
	{
		int32 Value = kDefaultCoarseMinLevel;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelCoarseMinLevel="), Value);
		return FMath::Max(1, Value); // level 0 has its own path; never route it here
	}();
	return MinLevel;
}

// Flat per-chunk column grid for the COARSE path (default ON), the coarse
// counterpart of the level-0 fast path in DispatchJobs.
//
// The coarse SAMPLING RULE was designed to cost what level 0 costs at any level
// (generator.h: B*B amplifier columns per brick footprint, B^3 materialAt calls,
// independent of L), and docs/gpu-g0-sizing.md measures the generator itself
// flat at ~2.6-3.0 ms across levels. The shipped coarse chunks nevertheless cost
// 4.4-5.3 ms against level 0's 1.55 ms. That gap is PLUMBING, not generation:
// MakeCoarseLevelSampler answers each of the ~64k per-chunk voxel queries
// through a std::function, three floorDiv/floorMod pairs and an unordered_map
// lookup, and it eagerly materializes a whole Brick<8> (512 materialAt calls,
// palette writes, tryCollapse) per brick key touched -- up to ~216 bricks for a
// chunk's 4x4x4 interior plus apron, i.e. ~2,300 amplifier columns against level
// 0's 1,156.
//
// makeCoarseBrick(L, key, coarseColumns(L, key.x, key.y)) stores exactly
// materialAt(amp.column(coarseRep(vx,L), coarseRep(vy,L)), coarseRep(vz,L)) at
// each cell (generator.h:117-142; Brick::set/get round-trips a MaterialId
// exactly through its palette), so building the chunk's (32+2)^2 coarse columns
// once and sampling that composition directly is the SAME function with the
// indirection and the eager brick materialization removed -- byte-identical
// output by construction, which -VoxelCoarseGridVerify below exists to hold to.
//
// `-VoxelCoarseGrid=0` restores the MakeCoarseLevelSampler path as the A/B
// control. Command-line, not a cvar, for the reason documented on
// -VoxelCoarseMinLevel above: -ExecCmds lands after streaming has begun.
bool CoarseGridEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelCoarseGrid="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// Per-worker-thread scratch buffer for the LEVEL-0 34x34 column grid, replacing
// a per-job heap allocation (default ON). See the block comment at the
// allocation site in DispatchJobs for the sizes and the byte-identity argument.
//
// `-VoxelL0GridScratch=0` restores the per-job TArray as the A/B control.
// Command line, not a cvar, for the reason documented on -VoxelCoarseMinLevel
// above: -ExecCmds lands after streaming has begun, so a cvar A/B would measure
// the same warmed-up state twice and would miss the cascade fill entirely --
// which is precisely the phase where concurrent allocation is heaviest.
bool L0GridScratchEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelL0GridScratch="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// Per-BRICK emptiness skip inside a level-0 job (default ON).
//
// The existing buried-chunk skip works on a whole 32-voxel-tall chunk, so it can
// only ever fire on chunks entirely above the terrain or entirely inside it --
// and it already has. What it leaves behind is measured at 40% of level-0 worker
// time spent on chunks that still mesh to ZERO quads, plus every surface chunk,
// where typically one of the four brick layers in z straddles the surface and
// the other three do not.
//
// The same two proofs the band uses are decisive brick by brick, from the SAME
// per-column numbers the band is a reduction of -- no new generation, no new
// amplifier work, ~164 integer compares per brick against the 1,000 sampler
// calls meshBrick makes before its own early-out can fire. See FNeverSkipBrick
// for the soundness argument and the level-0 job for the two tests.
//
// `-VoxelL0BrickSkip=0` restores the unconditional mesh as the A/B control, and
// -VoxelL0GridVerify checks the two paths quad-for-quad. Command line, not a
// cvar, for the reason documented on -VoxelCoarseMinLevel above.
bool L0BrickSkipEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelL0BrickSkip="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// Equivalence harness for the LEVEL-0 path, the exact counterpart of
// -VoxelCoarseGridVerify below and held to the same bar: every level-0 chunk is
// meshed TWICE -- once through whatever the level-0 fast path currently does,
// and once through a freshly allocated per-job TArray filled by a plain
// `Amp.column` loop, which is the pre-change reference implementation
// character-for-character -- and the two quad arrays are compared field by field
// IN ORDER. Both sides run the identical MeshChunkBricks traversal, so a change
// that only moves storage around must reproduce the same quads in the same
// sequence, not merely the same set.
//
// Measurement switch: it more than doubles level-0 job cost, so never on in a
// throughput run.
bool L0GridVerifyEnabled()
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("VoxelL0GridVerify"));
	return bEnabled;
}

// Verify tallies (worker threads write, the 1 Hz perf log reads).
std::atomic<int64> GL0GridVerifyChecked{0};
std::atomic<int64> GL0GridVerifyMismatches{0};

// Cross-job level-0 column-grid cache: MEASUREMENT ONLY, and deliberately so.
//
// A level-0 job's 34x34 grid depends only on the footprint (X,Y) -- the whole
// vertical stack of chunks at one (X,Y) builds the identical 1156 columns. A
// shared cross-job cache of those grids is the obvious next move once the grid
// is 41% of level-0 job time, and the estimate going in was that it is worth
// ~15%. That estimate rests entirely on how much REUSE there actually is in
// dispatch order, and that is measurable without writing a single line of cache.
//
// `-VoxelL0GridCacheProbe=<entries>` simulates an LRU of that capacity over the
// level-0 dispatch stream on the game thread: one packed-key probe per dispatch,
// no grids stored, no locks, no worker-side change at all. It reports hits,
// misses and the DISTINCT footprint count per window, so the ceiling (distinct /
// dispatches) and the achievable rate at a given capacity can be read off
// separately -- the difference between them is what capacity buys, and it is the
// number that decides whether a real cache is worth its memory (a stored grid is
// 208 KB, so 256 entries is 53 MB) and its lock.
//
// 0 = off. Command line for the usual reason: the reuse pattern during the
// cascade fill is part of what is being measured.
int32 L0GridCacheProbeEntries()
{
	static const int32 Entries = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelL0GridCacheProbe="), Value);
		return FMath::Max(0, Value);
	}();
	return Entries;
}

// Equivalence harness for the switch above: every coarse chunk is meshed TWICE,
// once through the flat grid and once through MakeCoarseLevelSampler, and the
// two quad arrays are compared field-by-field. The claim "pure refactor" is only
// worth anything as a count of chunks that agreed, so this converts it into one
// -- a mismatch would be terrain that silently changed shape in the outer rings,
// which is exactly the class of bug nobody notices until it ships.
//
// Measurement switch: it roughly doubles coarse job cost, so never on in a
// throughput run.
bool CoarseGridVerifyEnabled()
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("VoxelCoarseGridVerify"));
	return bEnabled;
}

// Verify tallies (worker threads write, the 1 Hz perf log reads).
std::atomic<int64> GCoarseGridVerifyChecked{0};
std::atomic<int64> GCoarseGridVerifyMismatches{0};

// Ring-boundary skirt (see MeshChunkBricks' ERingSkirtFace comment): default ON.
// -VoxelNoRingSkirt is the A/B control that reverts to the pre-fix open-edge
// seams (the concentric see-through cracks). Command-line, not a cvar, for the
// same "lands after streaming starts" reason as the switches above.
bool RingSkirtEnabled()
{
	static const bool bDisabled = FParse::Param(FCommandLine::Get(), TEXT("VoxelNoRingSkirt"));
	return !bDisabled;
}

// docs/status.md "Buried-chunk pre-dispatch skip", step 1 (measure before
// changing anything). Turns on the per-level zero-quad census in the worker
// job: every job additionally classifies its chunk as all-air / all-solid /
// mixed-no-face, and times the level-0 column-grid build separately from the
// mesh. The census costs a 34^3 sampler sweep per job, so it is a MEASUREMENT
// switch and never on in a throughput or gate run -- the numbers it reports
// describe the pipeline, but the run it reports them from is slower than the
// pipeline it describes.
//
// Read at first use from the command line rather than as a cvar, because
// -ExecCmds cvars apply only AFTER streaming has initialised and would
// silently census nothing for the first seconds of a run (the established
// -VoxelPendingJobCap= / -VoxelRingQuota= pattern, for the same reason).
bool MeasureEmptyEnabled()
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("VoxelMeasureEmpty"));
	return bEnabled;
}

// Soundness harness for the buried-chunk skip. With this on, the band verdict
// is computed exactly as it would be in production but the job is dispatched
// ANYWAY, and every result whose verdict said "provably no geometry" is
// checked against the real mesh. It converts "I believe the bound is
// conservative" into "N chunks agreed", which is the only form of that claim
// worth having: a false 'definitely empty' is a hole in the world, and unlike
// a wasted job it is invisible until someone flies past it.
bool VerifyBuriedSkipEnabled()
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("VoxelVerifyBuriedSkip"));
	return bEnabled;
}

// Buried-chunk pre-dispatch skip master switch, for A/B against the M1 gate
// and the throughput run without a rebuild. `-VoxelBuriedSkip=0` restores the
// pre-wave behaviour exactly (every candidate is dispatched). Command line
// rather than cvar for the reason above.
// Cold-band throttle kill switch (`-VoxelNoColdBandThrottle`), so the throttle
// can be attributed on ONE binary. Added because a hitch regression (1 -> ~85
// post-warmup hitches) appeared across two commits -- the ring-seam admission
// fix and the throttle -- and guessing which is not good enough.
bool ColdBandThrottleEnabled()
{
	static const bool bEnabled = !FParse::Param(FCommandLine::Get(), TEXT("VoxelNoColdBandThrottle"));
	return bEnabled;
}

bool BuriedSkipEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelBuriedSkip="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// Ring quota switch (docs/status.md "R2-R4 ring starvation fix").
//
// Both of the bounded-admission gates above, and the dispatch order they were
// built on top of, rank purely by 3D distance from the anchor. Ring MEMBERSHIP,
// however, is an XY annulus per mip level (RingPresets), and the rings differ in
// candidate count by orders of magnitude. Two independent consequences, both
// measured on this build (see the status.md section):
//
//  * With the cap OFF, dispatch is globally nearest-first, and R0 alone -- whose
//    footprint is a deep COLUMN reaching 38.4 m below the anchor since
//    underground streaming landed -- out-produces the entire worker pool
//    forever. R3/R4 sat at 5,560 + 5,185 chunks queued and 0 dispatched across a
//    whole 60 s run.
//  * With the cap ON (the default), the single global admission cutoff settles
//    around 246 m, and R3's annulus starts at 256 m. The outer rings are not
//    starved at dispatch any more -- they are never admitted at all, so they do
//    not even appear in the queue. R3/R4 pending = 0.
//
// The fix gives every ring its own queue, its own share of the admission cap and
// its own guaranteed floor of worker slots. `-VoxelRingQuota=0` restores the
// single-global-ordering behaviour (one shared cap, one global cutoff, strictly
// nearest-first dispatch across all levels) for A/B measurement, on the same
// binary, for the same reason -VoxelPendingJobCap is a switch and not a cvar.
// Admission refill: per level (1, default) or against the summed queue depth
// across every level (0, the pre-change form). A cvar rather than a
// command-line switch on purpose -- unlike -VoxelNoUnderground, this one is
// read fresh on every tick and changes no latched state, so flipping it from
// -ExecCmds mid-run genuinely measures both behaviours on one binary.
static int32 GPerLevelRefill = 1;
static FAutoConsoleVariableRef CVarPerLevelRefill(
	TEXT("voxel.Stream.PerLevelRefill"),
	GPerLevelRefill,
	TEXT("1 (default): a ring's admission refill triggers on ITS OWN queue draining below its share of the cap. 0: the pre-change form, one threshold over the summed queue depth -- which let R4's multi-second jobs hold R0's refill hostage. A/Bs the ADMISSION-DEFERRED refill only; the stale-scan refill added by the 2026-07-27 ring-gap fix is a separate trigger and runs either way."),
	ECVF_Default);

bool GetPerLevelRefillEnabled()
{
	return GPerLevelRefill != 0;
}

// All-solid ADMISSION skip (PR #80 follow-up #2, its author's own top-ranked
// item). The buried-chunk skip already declines to DISPATCH an all-solid chunk,
// so those cost almost no worker time -- but they are all still tracked
// records, and the record count is what both the exit scan and R0's entry scan
// are O(). Measured on the moving-underground flight added in this same wave:
// 52-53 k tracked of which 43-44 k deep, streaming subsystem at 19-21% of wall
// against the surface flight's 6.3-6.6%, and 96% of the deep set never produces
// geometry.
//
// 1 (default): a deep candidate whose apron-inclusive top is provably below
// vxc::Amplifier::solidBelowBoundMm never becomes a record at all.
// 0: the shipped form -- admit it, then decline to dispatch it later.
//
// Safe to drive from -ExecCmds for the same reason voxel.Stream.UndergroundSightM
// is: the deep set does not exist until the anchor first goes underground,
// which on any scripted flight is long after cvars have been applied.
static int32 GSolidSkip = 1;
static FAutoConsoleVariableRef CVarSolidSkip(
	TEXT("voxel.Stream.AdmissionSolidSkip"),
	GSolidSkip,
	TEXT("1 (default): skip ADMITTING anchor-relative deep chunks that are provably all solid rock, rather than "
	     "admitting them and declining to dispatch them later. 0: the pre-change form. The proof is "
	     "vxc::Amplifier::solidBelowBoundMm; it is vetoed by any edit in the footprint at or below the chunk."),
	ECVF_Default);

bool SolidSkipEnabled()
{
	return GSolidSkip != 0;
}

// Buried-chunk band skip at ADMISSION time (docs/streaming-handoff.md "Open,
// not blocking G3" -> "Deep-column waste"). The band skip in DispatchJobs
// already declines to MESH a buried level-0 chunk, but by then the chunk is a
// record and has spent one of R0's Cap/4 admission slots and one of its 512
// queue slots -- so the chunks that DO have geometry queue behind rock.
// Evaluating the SAME verdict (VoxelStreaming::BandProvesChunkEmpty) one stage
// earlier costs one TMap lookup per footprint per scan and never creates the
// record at all.
//
// 0: off (the pre-change form -- admit, queue, then decline to dispatch).
// 1: skip admitting.
// 2: MEASURE ONLY -- compute and count the verdict, admit anyway. This is the
//    control experiment for "can this predicate fire at admission at all",
//    which is a real question and not a rhetorical one: the band does not
//    exist until some level-0 job in that footprint has completed AND drained,
//    so a footprint being scanned for the first time has nothing to consult.
static int32 GAdmissionBandSkip = 0;
static FAutoConsoleVariableRef CVarAdmissionBandSkip(
	TEXT("voxel.Stream.AdmissionBandSkip"),
	GAdmissionBandSkip,
	TEXT("0 (default): the buried-chunk band skip runs at dispatch time only. 1: also refuse ADMISSION to level-0 "
	     "chunks the cached footprint band proves empty. 2: measure the verdict without acting on it. Vetoed by "
	     "EditedFootprintMinZ, exactly like voxel.Stream.AdmissionSolidSkip."),
	ECVF_Default);

// 0 off / 1 skip / 2 measure-only.
//
// -VoxelAdmissionBandSkip=<n> overrides the cvar and WINS, for the reason
// -VoxelNoUnderground documents: an -ExecCmds cvar is applied after the world
// has already begun streaming, and by then the first recomputes have admitted
// the near footprints this switch is about. An A/B driven through -ExecCmds
// would measure the same admitted set twice.
inline int32 AdmissionBandSkipMode()
{
	static const int32 CmdLineOverride = []
	{
		int32 Value = -1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelAdmissionBandSkip="), Value);
		return Value;
	}();
	return CmdLineOverride >= 0 ? CmdLineOverride : GAdmissionBandSkip;
}

// -VoxelNoEditFloorHatch: turn the DOWNWARD escape hatch off (see the
// EditedFootprintMinZ block in RecomputeDesiredSet). Exists only so the
// control experiment can show what a shaft dug past the depth skirt's floor
// looks like WITHOUT it -- which, on every build before this one, is a hole.
// A command-line switch and not a cvar, same reasoning as
// -VoxelAdmissionBandSkip below.
inline bool EditFloorHatchEnabled()
{
	static const bool bDisabled = FParse::Param(FCommandLine::Get(), TEXT("VoxelNoEditFloorHatch"));
	return !bDisabled;
}

// -VoxelVerifySolidSkip: admit the chunks the skip would have dropped, and
// record them so DrainResults can check they really did mesh to zero quads.
// voxel-core's tests prove the BOUND against the real amplifier; this proves
// the UE-side chunk-Z arithmetic that turns the bound into a per-chunk verdict,
// which is the half an off-by-one would actually live in.
inline bool VerifySolidSkipEnabled()
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("VoxelVerifySolidSkip"));
	return bEnabled;
}

bool GetRingQuotaEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelRingQuota="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// Guaranteed floor of in-flight worker slots per ring level, out of the
// 2xLogicalCores total (24 on this box). A FLOOR, not a fixed partition: a level
// under its floor with work pending is dispatched ahead of nearer work from any
// other level, but once every level is at or above its floor the remaining slots
// are filled strictly nearest-first, exactly as before. So when the outer rings
// are idle (cold start, or fully resident) R0 still gets every worker, and the
// reservation costs nothing.
//
// Sized from the measured per-level worker cost: R0 jobs are ~0.8 ms p50 and R1
// ~2 ms, so R0/R1 convert slots into chunks fast and need rate, not slots; R2
// jobs measured 8-44 ms p50 (they build mip bricks), so a handful of slots is
// already a meaningful share of throughput for them, and the coarse rings are
// exactly the ones a 50 km vista is made of.
// MEASURED CAVEAT, and the reason these are small: per-level worker cost is not
// remotely uniform. On this box R0 jobs are 0.7 ms p50, R1 4.5 ms, R2 18 ms, R3
// 150-198 ms and R4 **2,850-2,913 ms** -- a level-4 chunk covers 16x the linear
// extent of a level-0 one, so its mip build is ~4096x the volume, and the cost
// tracks that almost exactly. A reserved slot is therefore not a small loan for
// the coarse rings; an R4 slot holds a real worker thread for ~3 seconds. Sizing
// the floors by "share of the rings" rather than by cost was measured
// catastrophic: floors {0,2,3,4,4} reserved 13 of 24 slots, and because the task
// graph has ~12 actual background threads, the long jobs occupied the pool
// itself -- whole-run throughput collapsed from 49,179 chunks to 558 (819 -> 9.3
// chunks/s) and R0 residency from ~3,000 to ~10. The floors below cost at most 4
// of 24 slots and are the largest that did not measurably move the near field.
// R5 gets the same floor of 1 as every other coarse ring. The measured
// catastrophe above came from LONG jobs monopolising the ~12 real background
// threads; since the coarse path made per-chunk cost flat in level (~4 ms at R5
// exactly as at R1), an R5 slot is now no more expensive to reserve than an R1
// one, so the "at most 4 of 24 slots" reasoning extends unchanged to 5 of 24.
constexpr int32 kRingSlotFloorDefault[VoxelCoords::kNumLevels] = {0, 1, 1, 1, 1, 1};
static_assert(UE_ARRAY_COUNT(kRingSlotFloorDefault) == VoxelCoords::kNumLevels,
              "kRingSlotFloorDefault must have one entry per level (a short list zero-fills silently, starving the tail rings)");

// Overridable for tuning/measurement: `-VoxelRingFloors=0,1,1,1,1`.
//
// bShouldStopOnSeparator=false: FParse::Value's default terminator set is
// ",) \r\n\t" (Parse.cpp:299), so the default overload truncates a comma list
// at the FIRST comma and hands back one entry. This call site shipped without
// the flag and so only ever applied Floors[0] -- see the long note on
// GetRingPresets below, which had the same defect and where it was caught.
const int32* GetRingSlotFloors()
{
	static int32 Floors[VoxelCoords::kNumLevels];
	static const bool bInit = []
	{
		FMemory::Memcpy(Floors, kRingSlotFloorDefault, sizeof(Floors));
		FString Spec;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelRingFloors="), Spec,
		                  /*bShouldStopOnSeparator=*/false))
		{
			TArray<FString> Parts;
			Spec.ParseIntoArray(Parts, TEXT(","), true);
			for (int32 I = 0; I < Parts.Num() && I < VoxelCoords::kNumLevels; ++I)
			{
				Floors[I] = FMath::Max(0, FCString::Atoi(*Parts[I]));
			}
			// Log the RESOLVED floors, not the requested string: the whole
			// point of the bug above is that those two can differ silently,
			// and a tuning sweep whose legs were all secretly the default
			// reads as "the knob does nothing" rather than as a parse fault.
			FString Resolved;
			for (int32 I = 0; I < VoxelCoords::kNumLevels; ++I)
			{
				Resolved += FString::Printf(TEXT("%s%d"), I ? TEXT(",") : TEXT(""), Floors[I]);
			}
			UE_LOG(LogVoxelEarth, Log, TEXT("RingSlotFloors = {%s} (requested '%s')"), *Resolved, *Spec);
		}
		return true;
	}();
	(void)bInit;
	return Floors;
}

// Share of the pending-job cap reserved for each ring level. Sums to 1.0. R0
// keeps the largest single share (it has by far the highest churn -- it both
// produces and drains the most), but every ring is guaranteed a share it cannot
// be crowded out of, which is the whole point: with one shared cap R0+R1 filled
// it inside 246 m and R3/R4 never got a record.
// R5 (1024-2048 m) takes a share out of the existing ones rather than being
// bolted on. The ring geometry makes this cheap to reason about: every annulus
// is sized so its COLUMN count is roughly equal (the chunk edge doubles per
// level at the same time the annulus area quadruples), so rings want broadly
// similar shares, and R0 keeps the largest only because of its churn.
//
// A short list here does not merely mistune -- it breaks. A 0.0 share rounds to
// a LevelCap of 1 via the Max(1,...) below, so the ring holds one queued chunk,
// permanently re-arms bAdmissionDeferredWork, and spins the recompute every tick
// while never streaming.
constexpr double kRingCapShare[VoxelCoords::kNumLevels] = {0.25, 0.18, 0.17, 0.15, 0.13, 0.12};
static_assert(UE_ARRAY_COUNT(kRingCapShare) == VoxelCoords::kNumLevels, "kRingCapShare must have one entry per level");

constexpr double SumRingCapShare()
{
	double Sum = 0.0;
	for (double Share : kRingCapShare)
	{
		Sum += Share;
	}
	return Sum;
}
static_assert(SumRingCapShare() > 0.999 && SumRingCapShare() < 1.001, "kRingCapShare must sum to 1.0");
} // namespace VoxelStreamAdmission

// --- sky band: never mesh chunks that are provably above the terrain ---------
//
// docs/status.md "Zero-quad census": ~69% of all worker wall time on this box
// produces no geometry, and the coarse rings are the worst offenders per job
// (R3 460 ms p50, R4 6,022 ms p50 -- a level-4 chunk covers 4096x the volume of
// a level-0 one and its mip build tracks that). A chunk entirely ABOVE the
// terrain is the cheapest of those to recognise, because Amplifier::materialAt
// is unconditionally MAT_AIR whenever the voxel centre is above `surfaceMm`:
// caves and caverns only ever CARVE, and there is no fill of any kind above the
// surface. So an UPPER BOUND on the surface over a footprint is all it takes --
// no cave data, no cavern data, no bedrock, no level-0 bricks.
//
// The bound is analytic, from Amplifier::evalSurface's own structure:
//
//   surfaceMm = clamp(baseMm + detailMm, ...)
//
//   * baseMm is the BILINEAR interpolation of four ITileSampler::elevationMm
//     reads at the corners of the tile pixel the column falls in. A bilinear
//     patch is a convex combination of its four corners, so over any region its
//     maximum is bounded by the largest elevationMm at any pixel corner the
//     region touches -- an EXACT bound on the tile-scale term, from a handful
//     of reads (<=16 at level 4 with 30 m pixels), and the term that 4-corner
//     column sampling was missing.
//   * detailMm is bounded absolutely: each octave contributes
//     valueNoise2 * amplitudeMm / 32768 with |valueNoise2| <= 32768, so the sum
//     is bounded by the amplitude sum, and the slope scale is clamped to
//     [0.25, 4.0] q10. That is kMaxDetailMm below.
//
// Both switches are COMMAND-LINE switches resolved once at first use, not
// cvars, for the reason -VoxelPendingJobCap and -VoxelNoUnderground are: an
// -ExecCmds cvar lands after streaming has already built its desired set, so an
// -ExecCmds A/B silently measures the same state twice.
namespace VoxelSkyBand
{
// Amplifier detail-octave amplitude sum (kDetailOctaves in
// voxel-core/src/amplifier.cpp: 1800 + 700 + 260 + 100) times the maximum
// slope scale (slopeScaleQ10 clamps to 4096 q10 == 4.0). This is the only
// worldgen constant mirrored here, and it is mirrored CONSERVATIVELY: the bound
// is only ever used to widen an upper bound, so over-stating it costs a few
// wasted chunks and can never hide geometry. It is also checked continuously in
// production by -VoxelVerifySkyBand (see below), which cleared every chunk of a
// full perf run against the real mesh output.
//
// FOLLOW-UP (reported upstream): this belongs in voxel-core as a
// `Amplifier::surfaceUpperBoundMm(vx0, vy0, vx1, vy1)` next to the constants it
// depends on, so a change to kDetailOctaves cannot silently invalidate it.
// REMOVED (worldgen v9). kDetailAmplitudeSumMm / kMaxSlopeScaleQ10 /
// kMaxDetailMm mirrored the amplifier's octave table by hand, and the FOLLOW-UP
// above is what happened to them: kDetailOctaves changed at v6 and silently
// invalidated the copy, which sat at the v1 amplitudes (1800+700+260+100)
// against a real table of 2600+1100+500+190+60 for three worldgen versions.
//
// The allowance now comes from vxc::Amplifier::surfaceUpperBoundMm, which
// DERIVES it from the live table inside voxel-core and static_asserts the
// couplings the derivation needs. Do not reintroduce a local copy.

// Defensive cap on tile-corner reads per footprint. At level 4 (51.2 m
// footprint) a 30 mm-class pixel needs 4x4 and the 11.25 m scale-8 pixel 6x6;
// anything past this means an unexpected pixel size, and the bound is simply
// declined (INT64_MAX) rather than made expensive.
//
// Level 5 (102.4 m + 6.4 m apron = 108.8 m) needs ~6x6 at 30 m and ~12x12 at
// 11.25 m, so the 2 km cascade stays inside this cap and the bound is still
// computed at every level in use. See the matching note on
// vxc::kSurfaceBoundMaxCornersPerAxis for why a looser bound at a bigger
// footprint is the safe direction, and why level 6 would start declining.
constexpr int64 kMaxPixelCornersPerAxis = 16;

// -VoxelSkyTrim=0: restore ComputeFootprintChunkZRange's unconditional
// +2-level-L-chunk headroom (the pre-wave behaviour) for A/B on one binary.
bool GetTrimEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelSkyTrim="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// -VoxelSkySkip=0: keep the trim but disable the per-chunk pre-dispatch all-air
// skip. Separate from the trim so the two halves can be attributed apart.
bool GetSkipEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelSkySkip="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// -VoxelVerifySkyBand: compute the all-air verdict for every chunk but DISPATCH
// ANYWAY, and check the verdict against the mesh the worker actually produced.
// The only verification method strong enough for a "never hide geometry" claim:
// it exercises the real streaming path over hundreds of thousands of real
// chunks instead of a hand-picked unit fixture.
bool GetVerifyEnabled()
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("VoxelVerifySkyBand"));
	return bEnabled;
}
} // namespace VoxelSkyBand

// FVoxelWorldImpl -- the voxel-core side of the subsystem, defined only here
// so VoxelWorldSubsystem.h (UHT-parsed) never sees a voxel-core header. Also
// owns ALL Stage 2 streaming state (chunk records, pending-work queues, the
// worker-result MPSC queue, in-flight task handles): none of it is
// UE-reflection-visible, so it belongs behind the same PImpl boundary.
// Per-level diagnostic fields are built by LOOP rather than written out as
// R0=... R1=... literals. The hand-listed form was load-bearing in the worst
// way: it compiled fine when kNumLevels grew but silently omitted every level
// past R4 from every diagnostic -- precisely when a newly added outer ring is
// the thing you most need to see. Joins one formatted field per level with " | ".
// Highest ring level that participates in streaming, i.e. where the voxel
// cascade ends. kNumLevels-1 (the default) is the full cascade; a lower value
// retires the outer rings at runtime.
//
// This exists so the cascade radius is an A/B-able knob on ONE binary rather
// than a recompile: -VoxelMaxRingLevel=4 gives the 1024 m cascade and =5 the
// 2048 m one, interleaved, which is the only honest way to attribute a frame-time
// delta to the extra ring rather than to run-to-run drift. It is also the
// mechanism for shipping the largest radius that actually holds the budget.
//
// Retiring a level here suppresses only its ENTRY scan (nothing new is admitted
// at that level). The exit scan is left completely alone on purpose: any chunk
// already tracked at a retired level must still be able to leave by the normal
// hysteresis path, and teaching the exit scan about this switch would mean
// stranding resident chunks the moment it changed.
//
// Command-line rather than cvar, per the -VoxelPendingJobCap precedent: it has
// to be known before the first RecomputeDesiredSet and before the clipmap sizes
// its inner hole against it.
int32 UVoxelWorldSubsystem::GetMaxRingLevel()
{
	static const int32 MaxLevel = []
	{
		int32 Value = VoxelCoords::kNumLevels - 1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelMaxRingLevel="), Value);
		return FMath::Clamp(Value, 0, VoxelCoords::kNumLevels - 1);
	}();
	return MaxLevel;
}

// docs/streaming-handoff.md: RingPresets was static constexpr and had to
// become a runtime accessor before R0 could move to 128 m. See the header
// doc comment on GetRingPresets for why the override below is a command-line
// switch and not a cvar.
const UVoxelWorldSubsystem::FRingPreset* UVoxelWorldSubsystem::GetRingPresets()
{
	static FRingPreset Presets[VoxelCoords::kNumLevels];
	static const bool bInit = []
	{
		FMemory::Memcpy(Presets, kDefaultRingPresets, sizeof(Presets));
		bool bOverrideRequested = false;

		// -VoxelRingInnerMeters=<L0>,<L1>,... : overrides InnerMeters for as
		// many leading levels as values are given; trailing levels keep the
		// default. Same comma-list convention as -VoxelRingFloors
		// (VoxelStreamAdmission::GetRingSlotFloors, above).
		//
		// bShouldStopOnSeparator=false, AND THIS IS THE WHOLE SWITCH.
		// FParse::Value's FString overload defaults that flag to true, and its
		// terminator set is then ",) \r\n\t" (Parse.cpp:299) -- so the default
		// overload truncates the value at the FIRST COMMA. Every comma list in
		// this file was read as its first entry alone.
		//
		// Measured, not reasoned: Wave E ran
		//   -VoxelRingInnerMeters=0,32,64,128,256,512
		//   -VoxelRingOuterMeters=32,64,128,256,512,1024
		// -- a full six-entry cascade -- and the log below printed
		//   RingPresets[0] = [0.0, 32.0) m [OVERRIDDEN]
		//   RingPresets[1] = [64.0, 128.0) m        <- default, NOT overridden
		//   ... levels 2-5 likewise default.
		// Only OuterSpec="32" survived the parse. The result was a cascade with
		// 32-64 m covered by NO ring at all, which still runs, still renders and
		// still produces a plausible number.
		//
		// Wave E's write-up concluded the switch "appears to require all six
		// entries". It does not -- six entries fail identically. The arity was
		// never the variable; the comma was. Their OTHER leg passed four entries
		// beginning 0,64.../64,128... whose first entries equal the defaults, so
		// nothing changed at all and the run matched the full cascade frame for
		// frame, which is what made it look like an arity rule.
		FString InnerSpec;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelRingInnerMeters="), InnerSpec,
		                  /*bShouldStopOnSeparator=*/false))
		{
			TArray<FString> Parts;
			InnerSpec.ParseIntoArray(Parts, TEXT(","), true);
			for (int32 I = 0; I < Parts.Num() && I < VoxelCoords::kNumLevels; ++I)
			{
				Presets[I].InnerMeters = FCString::Atod(*Parts[I]);
			}
			bOverrideRequested = true;
		}

		// -VoxelRingOuterMeters=<L0>,<L1>,... : same shape, for OuterMeters.
		// This is the switch the verification for the runtime-accessor change
		// uses to shrink R0 (e.g. -VoxelRingOuterMeters=16).
		FString OuterSpec;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelRingOuterMeters="), OuterSpec,
		                  /*bShouldStopOnSeparator=*/false))
		{
			TArray<FString> Parts;
			OuterSpec.ParseIntoArray(Parts, TEXT(","), true);
			for (int32 I = 0; I < Parts.Num() && I < VoxelCoords::kNumLevels; ++I)
			{
				Presets[I].OuterMeters = FCString::Atod(*Parts[I]);
			}
			bOverrideRequested = true;
		}

		for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
		{
			UE_LOG(LogVoxelEarth, Log, TEXT("RingPresets[%d] = [%.1f, %.1f) m%s"), Level,
			       Presets[Level].InnerMeters, Presets[Level].OuterMeters,
			       (Presets[Level].InnerMeters != kDefaultRingPresets[Level].InnerMeters ||
			        Presets[Level].OuterMeters != kDefaultRingPresets[Level].OuterMeters)
			           ? TEXT(" [OVERRIDDEN]")
			           : TEXT(""));
		}

		// Validate the cascade the run is ACTUALLY going to use, and refuse to
		// start on a malformed one. Only reachable when an override switch was
		// supplied, so a default run cannot trip it.
		//
		// Abort rather than fall back to defaults, for the reason
		// VoxelPerfRunSubsystem's -VoxelPerfStaticAt branch spells out: a
		// silent fallback yields a perfectly plausible measurement of a scene
		// nobody asked for. That is not hypothetical here -- it is exactly what
		// the truncation bug above did to Wave E, and the cost was a whole
		// session's legs plus a wrong published explanation.
		//
		// The invariants are the ones the rest of the file already assumes:
		// RecomputeDesiredSet's ring-seam padding is derived from annuli that
		// ABUT (Outer[L] == Inner[L+1], zero overlap), and AVoxelClipmapActor
		// derives its entire vertex spacing from the outermost OuterMeters, so
		// a degenerate annulus there collapses the 30 km heightmap.
		if (bOverrideRequested)
		{
			// Tolerance, not equality: these are doubles round-tripped through
			// text. 1 micron is far below any radius anyone would type and far
			// above any parse error.
			constexpr double kAbutToleranceMeters = 1e-6;
			FString Faults;
			if (Presets[0].InnerMeters > kAbutToleranceMeters)
			{
				Faults += FString::Printf(
				    TEXT("\n  R0 inner is %.3f m, not 0 -- that leaves a hole centred on the player."),
				    Presets[0].InnerMeters);
			}
			for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
			{
				if (Presets[Level].OuterMeters <= Presets[Level].InnerMeters)
				{
					Faults += FString::Printf(
					    TEXT("\n  R%d = [%.3f, %.3f) is empty or inverted -- that ring admits no chunks."),
					    Level, Presets[Level].InnerMeters, Presets[Level].OuterMeters);
				}
				if (Level + 1 < VoxelCoords::kNumLevels &&
				    FMath::Abs(Presets[Level].OuterMeters - Presets[Level + 1].InnerMeters) > kAbutToleranceMeters)
				{
					Faults += FString::Printf(
					    TEXT("\n  R%d outer (%.3f m) != R%d inner (%.3f m) -- the annuli must abut exactly; "
					         "%s is covered by no ring."),
					    Level, Presets[Level].OuterMeters, Level + 1, Presets[Level + 1].InnerMeters,
					    Presets[Level].OuterMeters < Presets[Level + 1].InnerMeters
					        ? TEXT("the band between them")
					        : TEXT("the overlap is double-resident and"));
				}
			}
			if (!Faults.IsEmpty())
			{
				UE_LOG(LogVoxelEarth, Fatal,
				       TEXT("Ring cascade override is malformed and the run would measure a world nobody asked for.%s"
				            "\nRequested: -VoxelRingInnerMeters=%s -VoxelRingOuterMeters=%s"
				            "\nBoth switches take ONE ENTRY PER LEVEL (%d), inner[L+1] == outer[L]."),
				       *Faults, InnerSpec.IsEmpty() ? TEXT("<absent>") : *InnerSpec,
				       OuterSpec.IsEmpty() ? TEXT("<absent>") : *OuterSpec, VoxelCoords::kNumLevels);
			}
		}
		return true;
	}();
	(void)bInit;
	return Presets;
}

template <typename FormatOneLevelFn>
static FString JoinPerLevel(FormatOneLevelFn&& FormatOneLevel)
{
	FString Out;
	for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
	{
		if (Level > 0)
		{
			Out += TEXT(" | ");
		}
		Out += FormatOneLevel(Level);
	}
	return Out;
}

struct FVoxelWorldImpl
{
	// Track B2: TileDir/TileScale select the tile source (see MakeTileSampler
	// above for the full policy) -- TileDir empty is the pre-Track-B2 default
	// (SyntheticTileSampler, byte-identical to before).
	//
	// Phase 2 fine-tier streaming (docs/terrain-amplification-plan.md,
	// VoxelFineTileStreamer.h): FineTileDir empty (the default -- no
	// -VoxelFineTileDir on the command line) leaves FineStreamer null and
	// every fine-tier gate below a no-op, so behavior is byte-identical to
	// pre-Phase-2 unless the switch is explicitly passed. Same opt-in shape
	// as TileDir/MakeTileSampler above, deliberately: this is new,
	// unverified-in-engine glue (see the task report), and the existing
	// -VoxelTileDir precedent is exactly the risk profile it should match.
	explicit FVoxelWorldImpl(uint64 Seed, const FString& TileDir, int32 TileScale, const FString& FineTileDir,
	                         const FString& FineProviderId, uint64 FineBudgetBytes)
		: Tiles(MakeTileSampler(Seed, TileDir, TileScale, bUsingTileGrid, TileCoverageUU))
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

		// Phase 2: construct the fine-tier streamer only when a directory was
		// actually given. No directory-existence check here (unlike
		// MakeTileSampler's TileDir): FVoxelFineTileStreamer's own
		// EnsureTileResident already treats "file not found" as an ordinary,
		// expected "not resident yet" outcome (a frontier tile the bake
		// hasn't produced), never an error to fall back from -- see its
		// class-level doc comment on "block until ready".
		if (!FineTileDir.IsEmpty())
		{
			FineStreamer = MakeUnique<FVoxelFineTileStreamer>(
				FineTileDir, FineProviderId, Seed, FineBudgetBytes != 0 ? FineBudgetBytes : FVoxelFineTileStreamer::kDefaultBudgetBytes);
		}

		// "Admit everything at this level" sentinel. Set by loop rather than by a
		// braced initializer on the member, because a braced list that falls
		// short of kNumLevels zero-fills its tail -- and 0.0 here is not a
		// permissive cutoff but an absolute one that rejects every candidate at
		// those levels, forever, silently. See the member's doc comment.
		for (double& Cutoff : LevelAdmissionCutoffDistSq)
		{
			Cutoff = DBL_MAX;
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

		// S0-3: same treatment, one window per (producer, stage). Sized
		// unconditionally -- cheap (7 * 256 floats) -- even though nothing
		// writes into them unless voxel.Stream.LatencyStats is on.
		GpuQueuedMsWindow.Init(0.f, WorkerJobMsWindowSize);
		GpuDispatchToReadyMsWindow.Init(0.f, WorkerJobMsWindowSize);
		GpuReadyToDeliverMsWindow.Init(0.f, WorkerJobMsWindowSize);
		GpuSubmitToDeliverMsWindow.Init(0.f, WorkerJobMsWindowSize);
		GpuDeliverToApplyMsWindow.Init(0.f, WorkerJobMsWindowSize);
		CpuWorkerEndToEndMsWindow.Init(0.f, WorkerJobMsWindowSize);
		CpuDeliverToApplyMsWindow.Init(0.f, WorkerJobMsWindowSize);
	}

	// Track B2: bUsingTileGrid MUST be declared (and default-constructed via
	// its NSDMI) before Tiles below -- see the ctor's mem-initializer-order
	// comment. Tiles itself must be declared before Voxels: Voxels holds a
	// live ITileSampler& into whatever Tiles owns, so Tiles has to already be
	// a fully-formed object by the time Voxels' own constructor runs.
	// World-UU box the loaded tiles actually cover; empty under the synthetic
	// sampler. DECLARED HERE, BEFORE Tiles, AND THAT IS LOAD-BEARING: members
	// initialise in DECLARATION order, so a declaration after Tiles would have
	// its FBox2D(ForceInit) run AFTER MakeTileSampler wrote through the
	// out-param and silently erase the answer. Same hazard the Tiles/Voxels
	// ordering comment above documents.
	//
	// It exists because the DEFAULT SPAWN IS (0,0) and the tiles need not
	// contain it. When they do not, every elevation query returns the
	// missing-tile sea-level default and the world looks FLAT AND GENERIC
	// rather than broken -- which is exactly how it presented.
	FBox2D TileCoverageUU = FBox2D(ForceInit);
	// One-shot latch for the spawn-vs-coverage check below. Once, not per log
	// window: an Error repeated every 5 s trains people to ignore it.
	bool bTileCoverageChecked = false;

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

	// Phase 2 fine-tier residency/prefetch/eviction gate (VoxelFineTileStreamer.h);
	// null unless -VoxelFineTileDir was passed, in which case every fine-tier
	// gate this file adds (RecomputeDesiredSet's AddCandidate check, and the
	// per-call TickResidencyAndEviction below) is a no-op and behavior is
	// unchanged from before this feature existed. Independent of Tiles/Voxels
	// above by design -- see VoxelFineTileStreamer.h's "SCOPE NOTE": it does
	// NOT replace Tiles as the Amplifier's live sampler in this pass.
	TUniquePtr<FVoxelFineTileStreamer> FineStreamer;

	// MaybeLogCounters' missing-tile delta tracking (bUsingTileGrid only).
	uint64 LastMissingTileQueries = 0;

	// --- Stage 2 streaming state (docs/m1-plan.md Stage 2 decisions table);
	// M2 (docs/m2-plan.md "Ring streaming" row) generalizes every key here
	// from FVoxelChunkKey to FVoxelLevelChunkKey (level, chunk) -- level 0
	// keys behave identically to the pre-M2 single-ring scheme. ---

	TMap<VoxelCoords::FVoxelLevelChunkKey, VoxelStreaming::FChunkRecord> ChunkRecords;

	// --- S2-3: parked geometry (docs/speculative-generation-plan.md Wave S2) --
	//
	// Chunks whose records are gone but whose POOL RANGE is kept, hidden, so a
	// re-admit is a table write instead of a full re-mesh round trip. Under
	// motion the ring boundaries oscillate and the same ground is re-meshed
	// seconds after it was evicted; this is what stops that. It is also the
	// mechanism T4-1 parks speculatively generated terrain in.
	//
	// A SEPARATE MAP, NOT A FIELD ON FChunkRecord, and that is forced rather
	// than stylistic: DrainUnloads does ChunkRecords.Remove(Key) unconditionally
	// once the geometry is released, so anything living on the record is gone at
	// exactly the moment parking needs to remember something. A speculative chunk
	// (Wave S4) has no record at all yet, so the same map serves both.
	struct FParkedGeometry
	{
		int32 PoolHandle = INDEX_NONE;
		// Everything UnparkChunk needs to re-stamp the table entry. Held here
		// rather than re-derived so unpark cannot disagree with what was parked.
		FVector3f OriginInPool = FVector3f::ZeroVector;
		FVector4f Params = FVector4f(0.f, 0.f, 0.f, 0.f);
		int32 Level = 0;
		int32 QuadCount = 0;
		// INVALIDATION. GenerationId is bumped by MarkChunkDirtyForRemesh and
		// EditEpoch by PropagateEditToMips; either moving means this geometry
		// describes a world that no longer exists. Checked on adopt, because a
		// parked chunk has no record for the normal staleness path to catch --
		// MarkChunkDirtyForRemesh does not release geometry, it only bumps and
		// re-queues, so without this an edited chunk would silently un-park with
		// its pre-edit shape.
		uint64 GenerationId = 0;
		uint64 EditEpoch = 0;
		float ParkedAtSeconds = 0.f;
		// T4-1: was this geometry SPECULATED, or parked from a real eviction?
		// They share the map and the adopt path deliberately -- an adopt is an
		// adopt -- but they are capped separately and counted separately,
		// because demand parking caches geometry that WAS wanted and speculation
		// caches geometry that MIGHT be. Conflating their hit rates would hide
		// exactly the number that decides whether speculation is worth running.
		bool bSpeculative = false;
	};
	TMap<VoxelCoords::FVoxelLevelChunkKey, FParkedGeometry> ParkedGeometry;

	// Insertion order, for O(1) oldest-first eviction.
	//
	// THE FIRST VERSION SCANNED THE WHOLE MAP FOR THE OLDEST ENTRY ON EVERY PARK,
	// and the comment justifying it -- "linear is fine, the overshoot is one
	// entry in the steady state" -- was true about how MANY entries get evicted
	// and silently wrong about what it costs to FIND each one. At 58,676 parks
	// against a 12,000-entry cap that is ~700 million comparisons per leg, and it
	// was roughly half of parking's remaining cost after the publication fix.
	//
	// ParkedAtSeconds is ElapsedSeconds, which only increases, so insertion order
	// IS age order and a queue is exact rather than approximate. Entries adopted
	// out of the map leave stale keys behind here; the evictor skips any key the
	// map no longer holds, which is why this is a plain array and not a second
	// source of truth.
	TArray<VoxelCoords::FVoxelLevelChunkKey> ParkedInsertionOrder;

	// --- T4-1 speculative generation (Wave S4) ------------------------------
	//
	// Keys enumerated ahead of the anchor that nothing has asked for yet. Its OWN
	// queue, deliberately not PendingJobKeysByLevel: sharing that queue would put
	// speculative work under the admission cap and the ring quotas, where it
	// would either be starved by demand or -- worse -- displace it. The
	// "genuine-room guard" comment on the refill trigger documents a churn loop
	// that cost ~237,600 rejections/s when admission and dispatch shared state.
	TArray<VoxelCoords::FVoxelLevelChunkKey> SpeculativeKeys;
	TSet<VoxelCoords::FVoxelLevelChunkKey> SpeculativeInFlight;

	int64 SpecDispatchedSinceLog = 0;
	// One unconditional sample per frame. ~28 B each; the cap bounds a long
	// session at ~1.1 MB and a standard leg produces ~16,000.
	struct FFrameSample
	{
		float FrameMs = 0.f;
		float VoxelTickMs = 0.f;
		float RenderMs = 0.f;
		float RenderWaitMs = 0.f;
		float RHIMs = 0.f;
		float GameWaitMs = 0.f;
	};
	static constexpr int32 kMaxFrameSamples = 40000;
	TArray<FFrameSample> FrameSamples;

	int64 SpecParkedSinceLog = 0;
	// Why a DEMAND eviction did not park. Sums with ChunksParkedSinceLog to the
	// number of evictions that reached ParkChunkGeometry, so a shortfall in the
	// total says the function is not being called rather than refusing.
	int64 ParkRefusedNoPoolGeomSinceLog = 0;
	int64 ParkRefusedUnsettledSinceLog = 0;
	int64 ParkRefusedNotFinerSinceLog = 0;
	int64 ParkRefusedEditedSinceLog = 0;
	// Why a speculative dispatch did not become a park. These three plus
	// SpecParkedSinceLog account for every delivered speculative result.
	int64 SpecDroppedEmptySinceLog = 0;
	// Where in the surface band an empty speculative chunk sat. See task #19.
	int64 SpecEmptyTopSinceLog = 0;
	int64 SpecEmptyMidSinceLog = 0;
	int64 SpecEmptyBotSinceLog = 0;
	// Candidates the shared buried skip removed before dispatch.
	int64 SpecBandSkippedSinceLog = 0;
	int64 SpecDroppedOvertakenSinceLog = 0;
	int64 SpecDroppedPoolFullSinceLog = 0;
	int64 SpecAdoptedSinceLog = 0;
	int64 SpecEvictedUnusedSinceLog = 0;
	int64 SpecDispatchedTotal = 0;
	int64 SpecParkedTotal = 0;
	int64 SpecAdoptedTotal = 0;
	int64 SpecEvictedUnusedTotal = 0;
	int32 SpecParkedNow = 0;

	// Enumerate the leading-edge annulus from the PREDICTED anchor and queue what
	// demand has not asked for. Per tick, cheap, and bounded.
	void EnumerateSpeculativeCandidates();
	// Submit from SpeculativeKeys into whatever fork budget demand left. Called
	// at the END of DispatchJobs.
	void DispatchSpeculativeJobs();
	// Put a speculative mesh into the pool HIDDEN, and register it for adoption.
	void ParkSpeculativeResult(const VoxelCoords::FVoxelLevelChunkKey& Key,
	                           const FVoxelGpuMeshJobResult& GpuResult);
	void EvictOldestSpeculative();

	// Park bookkeeping for the 5 s line and the eviction policy.
	int64 ChunksParkedSinceLog = 0;
	int64 ChunksAdoptedSinceLog = 0;
	int64 ParkEvictedStaleSinceLog = 0;   // generation/edit-epoch mismatch on adopt
	int64 ParkEvictedCapSinceLog = 0;     // pushed out by the cap, oldest first
	int64 ChunksParkedTotal = 0;
	int64 ChunksAdoptedTotal = 0;

	// Park the oldest entries until the map is within Cap. LRU by ParkedAtSeconds.
	//
	// THE CAP IS A CORRECTNESS BOUNDARY, NOT A TUNING KNOB. A parked chunk still
	// owns its pool range, its Allocations slot and its chunk-table entry, so it
	// counts against pool capacity and the chunk-table floor exactly as a drawn
	// chunk does. S1 measured what happens when residency is allowed to expand
	// into available capacity: it does, all of it, and then the pool starts
	// refusing DEMAND allocations while looking half empty
	// (docs/measurements/s1-close-2026-07-27.txt). Parked geometry has weaker
	// back pressure than resident geometry, and speculative geometry (Wave S4)
	// has none at all.
	// Returns true if the geometry was PARKED rather than freed. A true return
	// means the record no longer holds geometry, exactly as ReleaseChunkGeometry
	// does -- callers must not treat it as "nothing happened".
	bool ParkChunkGeometry(const VoxelCoords::FVoxelLevelChunkKey& Key, VoxelStreaming::FChunkRecord& Rec);
	void EvictParkedOverCap(int32 Cap);
	void EvictParkedKey(const VoxelCoords::FVoxelLevelChunkKey& Key);

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

	// ADR-0006 G3. Weak for the same reason as FChunkRecord::Component: once
	// registered it is owned by ChunkOwner's component list. Null until the
	// first chunk loads under voxel.Stream.GPU.
	TWeakObjectPtr<UVoxelGpuPoolComponent> GpuPool;
	// Set alongside GpuPool. Raw because it is the streaming root, which outlives
	// every path that reads it, and a weak pointer here would imply a lifetime
	// question that does not exist.
	USceneComponent* GpuPoolRootComponent = nullptr;

	// Nearest-first-within-level, lower-level-wins-ties priority queues
	// (docs/m2-plan.md item 1: "Budgets shared across levels, nearest-first
	// within level, lower level (finer) wins priority at equal distance").
	// Sorted so Pop() (O(1), removes from the back) always yields the
	// highest-priority pending chunk -- see SortPendingQueues.
	// SortPendingQueues' decorate-sort-undecorate entry (see its body). The
	// worker queues STORE these rather than bare keys: the cached distance is
	// what lets DispatchJobs compare the heads of five separate per-level queues
	// (and TruncatePendingJobQueue read a cutoff distance) without recomputing a
	// chunk centre per comparison.
	struct FSortEntry
	{
		double DistSq;
		VoxelCoords::FVoxelLevelChunkKey Key;
	};

	// Worker-dispatch pending, ONE QUEUE PER RING LEVEL (R2-R4 starvation fix --
	// see VoxelStreamAdmission::GetRingQuotaEnabled). Each is sorted
	// lowest-priority-first so Pop() (O(1), from the back) yields that ring's
	// nearest pending chunk. Splitting by level is what makes a per-ring
	// admission share and a per-ring worker floor expressible at all; with one
	// flat queue every bound that could be written was necessarily global, and a
	// global bound on a radial quantity always cuts the outer rings first.
	TArray<FSortEntry> PendingJobKeysByLevel[VoxelCoords::kNumLevels];
	TArray<VoxelCoords::FVoxelLevelChunkKey> PendingGameThreadKeys; // overlay-aware game-thread mesh pending -- ANY level as of
	                                                                 // M2 wave 2 (see MarkChunkDirtyForRemesh/PropagateEditToMips)
	// Kept as a member purely to reuse the allocation across the ~8 calls/second.
	TArray<FSortEntry> SortScratchGameThread;

	// Total depth across every level's worker queue -- the quantity the old flat
	// PendingJobKeys.Num() reported, kept for the logs, the perf snapshot and the
	// drain-triggered refill test.
	int32 PendingJobNum() const
	{
		int32 Total = 0;
		for (const TArray<FSortEntry>& Q : PendingJobKeysByLevel)
		{
			Total += Q.Num();
		}
		return Total;
	}

	// TruncatePendingJobQueue's -VoxelRingQuota=0 path only: the union of every
	// level's queue, re-sorted globally so the pre-fix single-cap behaviour can
	// be reproduced exactly on the same binary. Never touched with the quota on.
	TArray<FSortEntry> TruncateMergeScratch;

	// Widest per-ring admission cutoff, in metres, for the periodic log -- -1 if
	// no ring is capping. The WIDEST is the informative one: it is the outer
	// rings' cutoff, and the whole bug was that a single global cutoff sat at
	// ~246 m while R3's annulus starts at 256 m.
	double WidestAdmissionCutoffM() const
	{
		double Widest = -1.0;
		for (const double Cutoff : LevelAdmissionCutoffDistSq)
		{
			if (Cutoff < DBL_MAX)
			{
				Widest = FMath::Max(Widest, FMath::Sqrt(Cutoff) / 100.0);
			}
		}
		return Widest;
	}

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

	// Highest edited chunk Z per (level, footprint XY) -- the sky-band trim's
	// escape hatch (see RecomputeDesiredSet and FFootprintZRange::
	// ChunkZMaxUntrimmed). Maintained in exactly one place, PropagateEditToMips,
	// which already receives the apron-extended level-0 dirty set and already
	// walks its ancestors at every level; this map is that same walk's XY->maxZ
	// reduction. Sized by edited footprints, not by world extent.
	TMap<FIntPoint, int32> EditedFootprintMaxZ[VoxelCoords::kNumLevels];

	// LOWEST edited chunk Z per (level, footprint XY) -- the mirror of the map
	// above, and the correctness crux of the all-solid admission skip (see
	// VoxelStreamAdmission::SolidSkipEnabled and RecomputeDesiredSet).
	//
	// The analytic floor from Amplifier::solidBelowBoundMm is a statement about
	// WORLDGEN, and worldgen is not the only thing that puts air underground: a
	// player digs. A chunk 50 m down that the bound correctly calls all-solid
	// stops being all-solid the moment someone tunnels into it, and if that
	// chunk was never ADMITTED there is no record, no component, and nothing to
	// re-mesh -- strictly worse than the sky-band trim's failure mode, which at
	// least still had a tracked record. So the skip is vetoed for any footprint
	// with an edit at or below the candidate's Z.
	//
	// Lives OUTSIDE the FootprintZRangeCache memo for exactly the reason the
	// sky-band escape hatch and the depth skirt do: the memo is keyed on
	// (Level, X, Y) and must stay a pure function of them, which the M1 hitch
	// fix depends on. Edits are not a function of (Level, X, Y) over time.
	//
	// Maintained in the same one place and by the same walk as EditedFootprintMaxZ.
	TMap<FIntPoint, int32> EditedFootprintMinZ[VoxelCoords::kNumLevels];

	// -VoxelVerifySkyBand only: chunk keys IsChunkProvablyAllAir called air,
	// dispatched anyway so DrainResults can check the verdict against the mesh
	// the worker really produced. Empty (and never touched) in normal runs.
	TSet<VoxelCoords::FVoxelLevelChunkKey> VerifyPredictedAirKeys;
	int64 VerifyAirPredictions = 0;  // chunks predicted all-air AND checked against a real result
	int64 VerifyAirViolations = 0;   // ...of which the worker produced quads for. Must stay 0.
	int64 VerifyChunksChecked = 0;   // every non-stale result the verifier saw, air or not
	int64 VerifyAirPredictionsByLevel[VoxelCoords::kNumLevels] = {};
	int64 VerifyResultsByLevel[VoxelCoords::kNumLevels] = {};

	// Chunks never dispatched because IsChunkProvablyAllAir proved them empty,
	// per level (the headline saving) -- reported on the ring dispatch line.
	int64 LevelSkySkippedTotal[VoxelCoords::kNumLevels] = {};

	TQueue<VoxelStreaming::FJobResult, EQueueMode::Mpsc> ResultsQueue;
	FThreadSafeCounter JobsInFlightCounter;
	TArray<UE::Tasks::TTask<void>> InFlightTasks; // only for a clean Deinitialize() wait; see WaitForInFlightTasks

	// --- GPU meshing (ADR-0006, Wave D / D4) --------------------------------
	//
	// The async runner, owned here so it lives exactly as long as the streaming
	// state its results feed. Created lazily on the first GPU-forked dispatch
	// (so a session that never enables the fork never constructs it) and
	// destroyed in WaitForInFlightTasks, which is also where its outstanding
	// jobs are cancelled -- see there for the teardown-ordering argument, which
	// is the one genuinely load-bearing part of this wiring.
	//
	// NOT in InFlightTasks: a GPU job has no UE::Tasks::TTask to wait on.
	TUniquePtr<FVoxelGpuMeshJobManager> GpuMeshJobs;
	// Cleared before CancelAll at teardown, so cancellations delivered while
	// Impl is being destroyed are absorbed instead of enqueued onto a
	// ResultsQueue nothing will ever drain again.
	bool bAcceptingGpuResults = true;
	// Counted rather than silent: a delivery arriving after the fork is
	// disarmed is expected at teardown and would be a bug at any other time, so
	// the number is worth having in the log rather than inferring from silence.
	int64 GpuResultsAbsorbedAtTeardown = 0;

	// Per-log-window fork telemetry. dispatched vs delivered is the pair that
	// says whether the fork is keeping up; the status breakdown exists because
	// every non-Success outcome still produces a chunk with zero quads, which is
	// indistinguishable on screen from terrain that is genuinely empty.
	int64 GpuMeshJobsDispatchedSinceLog = 0;
	// Per level, because "the fork is running" and "the fork is running where it
	// matters" are different claims. Wave F measured a level-0-only fork
	// changing cold fill by nothing, and the reason was that levels 1-5 are ~80%
	// of resident chunks -- so the split is the number that says whether D5
	// actually moved the population, not the total.
	int64 GpuMeshDispatchedByLevel[VoxelCoords::kNumLevels] = {};
	int64 GpuMeshJobsDeliveredSinceLog = 0;
	int64 GpuMeshJobsFailedSinceLog = 0;
	double GpuMeshSubmitToDeliverMsSinceLog = 0.0;
	double GpuMeshSubmitToDeliverMaxMs = 0.0;
	// Deliveries that took at least kBlindJobMarkTimeoutSeconds. See
	// OnGpuMeshJobComplete for why gpuLatencyTimeouts cannot stand in for this.
	int64 GpuMeshSlowDeliveriesSinceLog = 0;
	int64 GpuMeshSlowDeliveriesTotal = 0;

	// Stage-0 measure-first census for the chunk-tile GPU-batching design
	// (docs/measurements/gpu-throughput-wave-2026-07-27.txt): tallies, per 5s
	// log window, how many GPU-fork-bound dispatches would have landed in
	// each cell of the proposed fixed world-aligned 4x4 lattice (tile key =
	// Level, ChunkX>>2, ChunkY>>2 -- Z is deliberately NOT split, since a
	// tile may span multiple chunk-Z and the design accepts that union).
	// Only chunks that actually take the GPU fork branch in DispatchJobs are
	// counted (never the CPU-worker path, never the game-thread path), so
	// this cannot overcount what a real tile could combine. Read and reset
	// alongside the other GpuMesh...SinceLog counters in MaybeLogCounters.
	// This map and its logging are expected to be REMOVED or gated once tile
	// batching ships and is measured by its own real occupancy counters.
	TMap<FIntVector, int32> TileCensusSinceLog;

	// What the fork needs back when a job lands, keyed by the runner's job id.
	//
	// Kept here rather than packed into FVoxelGpuMeshJobResult::UserTag because
	// a level chunk key plus a generation id does not fit in one uint64, and a
	// lossy pack is precisely how a result gets applied to the wrong chunk. The
	// map is erased on delivery, and the runner guarantees exactly one delivery
	// per submitted job, so it cannot leak.
	struct FGpuPendingJob
	{
		VoxelCoords::FVoxelLevelChunkKey Key;
		uint64 GenerationId = 0;
		// T4-1: a speculative result is PARKED on arrival, not applied -- nothing
		// has asked for it, so it must not create a record or become visible.
		bool bSpeculative = false;
	};
	TMap<uint64, FGpuPendingJob> GpuJobsPending;

	// Creates the runner on first use and returns it. Null only if the RHI
	// cannot support it, which is checked once.
	FVoxelGpuMeshJobManager* EnsureGpuMeshJobs();
	// Builds the region request for one level-0 chunk and hands it to the
	// runner. Returns false if the fork could not take the chunk, in which case
	// the caller MUST fall through to the CPU path -- the counters have already
	// been incremented and something owes a result.
	bool SubmitGpuMeshJob(const VoxelCoords::FVoxelLevelChunkKey& LevelKey, uint64 GenId,
	                      uint8 RingSkirtMask, bool bSpeculative = false);
	// Delivery callback. Game thread, from inside Tick().
	void OnGpuMeshJobComplete(FVoxelGpuMeshJobResult&& GpuResult);

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
	// STALE-SCAN REFILL (ring-gap fix, 2026-07-27). The exact anchor position the
	// last entry scan for this level actually ran at -- not quantized to the
	// level's chunk lattice the way LastAnchorChunkPerLevel is. That quantization
	// is the whole point: a level-L scan decides admission with the anchor at one
	// POINT, and everything it skipped on distance (the `Level > 0` inner test,
	// the seam-padding parent test) was skipped against THAT point. Once the
	// anchor stops moving, nothing re-asks -- the movement gates are quantized
	// coarser than the seam padding and the admission-deferred flag is only set by
	// budget/cutoff rejections, so distance-skipped columns are never revisited.
	// TickStreaming's refill block uses this to re-arm a drained level whose
	// anchor has moved at all since its last scan. See that block's comment for
	// the measurement (63-123 byte-identical permanent holes at r=65 m and
	// r=124-128 m across four independent instrumented legs).
	FVector2D LastEntryScanAnchorXY[VoxelCoords::kNumLevels] = {};

	// Quiescence detector for the stale-scan refill (rev B): the trigger above
	// only fires once the anchor has been effectively still for half a second,
	// because firing it while MOVING fed re-admission churn without buying any
	// coverage (see the trigger's comment for the measured numbers).
	FVector2D LastQuiescenceAnchorXY = FVector2D(DBL_MAX, DBL_MAX);
	float AnchorStillSeconds = 0.f;

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
		// The sky-band trim's escape hatch. ChunkZMax is the TRIMMED top (see
		// ComputeFootprintChunkZRange); this is what the pre-trim +2-chunk rule
		// would have returned. RecomputeDesiredSet re-widens up to -- never past
		// -- this value for footprints that contain an edited chunk above the
		// trimmed top, so a placed block on a hilltop cannot be orphaned by the
		// trim while the pre-trim envelope stays exactly what it always was.
		// Both fields are pure functions of (Level, X, Y), so the memo's
		// correctness argument is untouched: nothing anchor- or edit-dependent
		// lives in here.
		int32 ChunkZMaxUntrimmed = 0;
	};
	mutable TMap<VoxelCoords::FVoxelLevelChunkKey, FFootprintZRange> FootprintZRangeCache;

	// All-solid admission skip: the analytic floor (absolute mm, sea-level
	// datum) below which every voxel of this level-L footprint is provably
	// solid, from vxc::Amplifier::solidBelowBoundMm. INT64_MIN means it
	// declined, which callers must read as "no information", never as solid.
	//
	// A SEPARATE memo from FootprintZRangeCache rather than a fourth field on
	// FFootprintZRange, because it is consulted only for anchor-relative deep
	// candidates -- a small minority of the calls the z-range memo serves, and
	// none at all on a surface flight. Same key, same purity argument (a pure
	// function of (Level, X, Y) and the immutable tile raster: no anchor, no
	// edits), same prune.
	mutable TMap<VoxelCoords::FVoxelLevelChunkKey, int64> FootprintSolidFloorCache;

	// Buried-chunk pre-dispatch skip: level-0 footprint bands, keyed by the
	// level-0 footprint (X,Y). Like FootprintZRangeCache this memoizes a PURE
	// function of (X, Y) and the amplifier -- nothing anchor-relative and
	// nothing edit-relative goes in here, so an entry never needs invalidating
	// and is reusable for the whole session (edits are vetoed at the point of
	// use instead, see DispatchJobs). Populated by level-0 worker results,
	// which derive the band from the column grid they had to build anyway;
	// never computed on the game thread, where 1156 Amplifier::column calls
	// per footprint would be an M1-gate disaster. Pruned by distance alongside
	// FootprintZRangeCache.
	TMap<FIntPoint, VoxelStreaming::FFootprintBand> FootprintBandCache;

	// Cold-band throttle (DispatchJobs). Level-0 XY footprints that have a
	// band-seeding job in flight but no band yet: exactly one blind job per
	// footprint is allowed, and its ~15 column-mates wait for the answer rather
	// than each guessing. Added in DispatchJobs, removed in DrainResults on any
	// level-0 result. Game-thread only, like FootprintBandCache itself.
	// Value is ElapsedSeconds at the moment the mark was set, so it can AGE OUT.
	//
	// The mark is meant to be released by DrainResults on any level-0 result for
	// the column. Relying on that alone has now failed twice: once by setting it
	// at pop time (a popped chunk can still leave via NeedsOverlayAwarePath or
	// either pre-dispatch skip without launching anything), and once again here,
	// where a column was measured stranded for an entire run -- two chunks 44 m
	// from the anchor, deferred on every dispatch pass forever, which starved
	// their whole ring of ~1,700 chunks.
	//
	// Both times the bug was a launch path that produced no result. Enumerating
	// those paths correctly is exactly what keeps being got wrong, so the mark no
	// longer depends on it: a mark older than kBlindJobMarkTimeoutSeconds is
	// treated as absent. Releasing early costs at most one extra blind job for
	// that column -- the throttle exists to save work, not for correctness --
	// while holding one forever costs the ring.
	TMap<FIntPoint, double> FootprintBlindJobInFlight;
	// Which of those marks was seeded by a GPU-meshed job (Wave D / D4). Kept
	// beside the map rather than folded into its value so the mark's own
	// lifetime rules stay literally unchanged -- added and removed in exactly
	// the same two places, and empty whenever the GPU fork is off.
	//
	// It exists only to SPLIT the age-out counter below. See there.
	TSet<FIntPoint> FootprintBlindJobIsGpu;
	// Generous next to a worker job (measured p50 well under 100 ms) so a healthy
	// column is never released early, but short enough that a stranded one
	// recovers in seconds rather than never.
	//
	// DELIBERATELY NOT RAISED FOR GPU JOBS (Wave D / D4). A GPU job's
	// submit->deliver time is longer and, at time of writing, unmeasured -- so
	// the obvious move is to raise this. That is the wrong trade: raising it
	// slows recovery from a genuine strand, and this throttle exists to SAVE
	// WORK, not for correctness. An extra blind job costs one job; a stranded
	// (X,Y) column costs its whole ring, and has done, twice. The mark stays at
	// 5 s and the counter is split instead.
	static constexpr double kBlindJobMarkTimeoutSeconds = 5.0;
	int64 ColdBandDefersSinceLog = 0; // column-mates held back, per 5s log window
	// Marks released by the age-out above WITH NOTHING IN FLIGHT. Should be ZERO
	// on a healthy run -- any non-zero value means a launch path produced no
	// result, which is the bug this backstop exists to survive, and is worth
	// chasing. That sentence is the only diagnostic for a bug this project has
	// now fixed twice, which is why the GPU case is counted separately below
	// rather than being allowed to blunt it.
	int64 ColdBandMarkTimeoutsSinceLog = 0;
	// Marks released by the age-out while a GPU-meshed job for that footprint
	// was still legitimately in flight (Wave D / D4). NOT a bug: it means GPU
	// delivery outran the 5 s mark, and the cost is one redundant blind job for
	// that column. PRE-EMPTIVE, and recorded as such -- the only datapoint in
	// hand is a 921 ms dispatch->ready max on a warmup leg, comfortably under
	// the mark, so this is expected to read zero today. It is here so that if
	// GPU latency ever does cross 5 s, the fact lands in its own counter
	// instead of turning the line above into noise.
	int64 ColdBandGpuLatencyTimeoutsSinceLog = 0;
	int32 ColdBandHeldThisFrame = 0;  // held on the most recent DispatchJobs pass

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
	// Per RING LEVEL, not global (see VoxelStreamAdmission::GetRingQuotaEnabled).
	// A single global cutoff is a radius, and a radius applied to a set of
	// concentric annuli excludes the outermost ones completely: measured, the
	// global cutoff settled at ~246 m while R3's annulus begins at 256 m, so R3
	// and R4 never admitted a single chunk. With the quota off, every entry here
	// holds the same global value and the behaviour is exactly the old one.
	// DBL_MAX = that level's queue is not full, admit everything in its annulus.
	//
	// Filled by loop, NOT by a literal list, and that is deliberate. The sentinel
	// here means "admit everything", so a hand-written list that falls short of
	// kNumLevels zero-fills the tail to 0.0 -- which is not a lax cutoff but the
	// strictest possible one, rejecting every candidate at those levels forever.
	// It is the single most dangerous of the per-level tables and the one least
	// likely to be noticed, because the symptom is a ring that silently never
	// streams rather than any error.
	// (set to the DBL_MAX sentinel by loop in FVoxelWorldImpl's constructor)
	double LevelAdmissionCutoffDistSq[VoxelCoords::kNumLevels] = {};

	// Worker jobs currently in flight per ring level. JobsInFlightCounter is the
	// total (and must stay atomic -- workers decrement it); this breakdown is
	// only ever touched on the game thread, incremented in DispatchJobs and
	// decremented in DrainResults, and is what DispatchJobs tests the per-ring
	// slot floor against.
	int32 LevelJobsInFlight[VoxelCoords::kNumLevels] = {};

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
	// PER LEVEL. It was one global flag, which was correct while the refill
	// trigger was also global; now that a refill re-enumerates only the levels
	// whose own queue drained (see TickStreaming), a global flag could never be
	// cleared again -- its clear condition requires that EVERY level scanned on
	// the call, and a partial refill by construction scans only some. The flag
	// would then latch on, and since a converged level's queue is empty (and so
	// always under its share of the cap), RecomputeDesiredSet would run every
	// single tick forever. Splitting it per level keeps the clear reachable:
	// each level's deferral is set and cleared by that level's own scan.
	bool bAdmissionDeferredWork[VoxelCoords::kNumLevels] = {};
	// Per-RecomputeDesiredSet-call bookkeeping for the flags above (reset at the
	// top of every call): which levels actually ran their entry scan, and how
	// many candidates gate (a) declined in each.
	bool bLevelScannedThisCall[VoxelCoords::kNumLevels] = {};
	int32 LevelsScannedThisCall = 0;
	int32 CandidatesRejectedThisCall = 0;
	int32 LevelCandidatesRejectedThisCall[VoxelCoords::kNumLevels] = {};
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
	// S0-2: TotalChunksLoaded as of the previous MaybeLogCounters window, so
	// the periodic log can report a per-window rate (apply rate decaying
	// across a leg is the §2.2 prediction this counter tests) rather than
	// only the leg-long mean TotalChunksLoaded already gives.
	int64 ChunksLoadedAtLastLog = 0;

	// --- Debug-tooling instrumentation (docs/debug-tooling-plan.md P1) -------

	// Engine-free perf counters (voxel-core/include/voxelcore/counters.h),
	// incremented around this file's own calls into voxel-core (worker mesh
	// jobs, the edit-log apply path) -- voxel-core's internals stay untouched.
	vxc::Counters PerfCounters;

	// Free-running clock since Initialize (NOT GetWorld()'s time -- this impl
	// has no UWorld& handy outside Tick/TickStreaming's parameters), used only
	// for chunk-state tint flash decay timing.
	float ElapsedSeconds = 0.f;

	// Load-before-unload retention telemetry (see the counter block in
	// DrainUnloads for how to read these). Game-thread only.
	int32 RetainHeldThisFrame = 0;             // stand-ins still held at the end of the last DrainUnloads
	// The covered releases, SPLIT BY WHAT THE COVERED VERDICT RESTED ON (ring-gap
	// wave). ReplacementCovered USED to treat an ABSENT replacement record as
	// covered, on the argument that a key with no record was never desired -- an
	// argument that only holds when absence means "the replacing ring's own
	// footprint trim declined it". Absence has four OTHER causes on this path
	// (the per-level admission budget, the per-level distance cutoff, the
	// pending-queue drop, and the entry-scan gate that skips a level entirely
	// while the anchor has not left its level-L chunk), and under any of them the
	// stand-in was released over ground nothing was drawing yet -- the ring-gap
	// defect, measured at ~1700 uncovered columns in flight.
	//
	// Since the 2026-07-27 fix an absent-but-desired replacement BLOCKS, so the
	// split now separates "all present and settled" from "some absent and proven
	// undesired". Both are sound; the unsound population moved to RetainHeldThisFrame.
	int64 RetainCoveredSettledReleasesSinceLog = 0; // every replacement record consulted existed AND had settled
	int64 RetainCoveredAbsentReleasesSinceLog = 0;  // some consulted record was absent and provably not desired
	int64 RetainCapReleasesSinceLog = 0;            // parked because LodRetentionMs expired first
	int64 ResurrectionsSinceLog = 0;                // pending unloads cancelled because the footprint re-entered the desired set
	// Per-level breakouts of the two suspicious releases. Which RING releases on
	// an absent replacement is the whole question -- a coarse ring released at its
	// inner edge leaves a hole the finer ring has not filled, and that is a
	// different bug from a fine ring released at its outer edge.
	int64 LevelRetainCoveredAbsent[VoxelCoords::kNumLevels] = {};
	int64 LevelRetainCapReleases[VoxelCoords::kNumLevels] = {};

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

	// S0-3 (docs/speculative-generation-plan.md Wave S0 / T0-2): per-producer,
	// per-stage submit->apply latency, same fixed-size-ring-buffer idiom as
	// WorkerJobMsWindow above (sized with WorkerJobMsWindowSize -- not a second
	// window size, the same one). Filled only under voxel.Stream.LatencyStats
	// (VoxelDebug::GetStreamLatencyStats) -- see OnGpuMeshJobComplete and
	// DrainResults for the write sites, MaybeLogCounters for where these are
	// read back as p50/p95/max.
	//
	// GPU fork arm, written in OnGpuMeshJobComplete straight from
	// FVoxelGpuMeshJobResult's own fields:
	TArray<float> GpuQueuedMsWindow;
	int32 GpuQueuedMsWindowNext = 0;
	int32 GpuQueuedMsWindowCount = 0;
	TArray<float> GpuDispatchToReadyMsWindow;
	int32 GpuDispatchToReadyMsWindowNext = 0;
	int32 GpuDispatchToReadyMsWindowCount = 0;
	TArray<float> GpuReadyToDeliverMsWindow;
	int32 GpuReadyToDeliverMsWindowNext = 0;
	int32 GpuReadyToDeliverMsWindowCount = 0;
	TArray<float> GpuSubmitToDeliverMsWindow;
	int32 GpuSubmitToDeliverMsWindowNext = 0;
	int32 GpuSubmitToDeliverMsWindowCount = 0;
	// Deliver (ResultsQueue.Enqueue) to ApplyMeshResult, written in DrainResults
	// from FJobResult::DeliverSeconds -- the ONE stage that is a property of
	// the CONSUMER (DrainResults's own per-frame apply budget), not either
	// producer, which is why it exists on both arms below as well as this one.
	TArray<float> GpuDeliverToApplyMsWindow;
	int32 GpuDeliverToApplyMsWindowNext = 0;
	int32 GpuDeliverToApplyMsWindowCount = 0;

	// CPU worker arm. One end-to-end figure -- the worker task has no separate
	// queue/dispatch/ready stages the way the GPU fork does, so this mirrors
	// FJobResult::JobMs (already stamped JobStartSeconds..enqueue in the task
	// body; nothing new added there) rather than inventing a stage split that
	// does not exist. Isolates the CPU-only population that WorkerJobMsWindow
	// above mixes with the GPU arm's SubmitToDeliverMs.
	TArray<float> CpuWorkerEndToEndMsWindow;
	int32 CpuWorkerEndToEndMsWindowNext = 0;
	int32 CpuWorkerEndToEndMsWindowCount = 0;
	TArray<float> CpuDeliverToApplyMsWindow;
	int32 CpuDeliverToApplyMsWindowNext = 0;
	int32 CpuDeliverToApplyMsWindowCount = 0;

	// Per-level (0..5) quad-count-per-delivered-chunk distribution (S0-3's
	// other half -- "free here", filled alongside the census just above it in
	// DrainResults). Mean is SumQuads/Count; the bucket histogram is read in
	// MaybeLogCounters. Buckets: see the bucket boundary comment at the log
	// site -- chosen against a measured mean of ~902 quads/chunk and the hard
	// per-chunk bound of 98,304 quads.
	static constexpr int32 kNumQuadHistBuckets = 6; // 0, 1-255, 256-1023, 1024-4095, 4096-16383, 16384+
	int64 LevelQuadHistSinceLog[VoxelCoords::kNumLevels][kNumQuadHistBuckets] = {};
	int64 LevelQuadCountSinceLog[VoxelCoords::kNumLevels] = {};
	int64 LevelQuadSumSinceLog[VoxelCoords::kNumLevels] = {};

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
	// --- Anchor motion (HUD row now; Wave S3's velocity source) -------------
	//
	// FINITE-DIFFERENCED FROM THE ANCHOR, DELIBERATELY, NOT Pawn->GetVelocity().
	// -VoxelPerfFlight=line repositions the pawn with
	// SetActorLocationAndRotation(..., TeleportPhysics) every tick, which never
	// touches UFloatingPawnMovement, so the movement component's Velocity is
	// zero for the entire flight. Anything reading GetVelocity() reports a
	// stationary camera while the world streams past at 20 m/s -- and it reports
	// it on exactly the runs this is meant to explain
	// (docs/speculative-generation-plan.md §2.4).
	//
	// The anchor is also the RIGHT point to measure: it is what every ring
	// radius, admission cutoff and retention decision is computed against, so
	// its motion is what the streaming numbers are responding to.
	FVector PrevAnchorLocation = FVector::ZeroVector;
	bool bHasPrevAnchor = false;
	// EMA over ~0.25 s. Raw per-tick delta is unreadable at 60+ fps, and a
	// single repositioning frame would spike it into nonsense.
	double SmoothedAnchorSpeedUUPerSec = 0.0;
	// The DIRECTION half, smoothed the same way. T4-1 needs a cone, not a speed:
	// speculation is only worth anything if it is aimed where the camera is
	// going. Kept as a velocity rather than a normalised heading so a stationary
	// anchor produces a zero vector and the lead collapses to nothing on its own,
	// with no special case.
	FVector SmoothedAnchorVelocity = FVector::ZeroVector;

	// Where the anchor will be in VelocityLeadSec, clamped. This is what
	// speculative enumeration measures its annulus from -- NOT what admission,
	// eviction or retention use, which all stay on the true anchor. Evicting
	// against a forward-shifted centre would delete ground behind the camera that
	// is still on screen (docs/speculative-generation-plan.md Wave S3).
	FVector PredictedAnchorLocation = FVector::ZeroVector;

	double AccumDispatchMs = 0.0;
	// Sub-total of AccumDispatchMs: the speculative half only. See task #21.
	double AccumSpecDispatchMs = 0.0;
	// Enumeration runs BEFORE the dispatch timer's T0, so it has never had a
	// bucket at all -- it lands in tickMs and nowhere else. Counted here so the
	// annulus walk can be ruled in or out rather than guessed at.
	double AccumSpecEnumerateMs = 0.0;
	// Speculative park work, done inline in the completion callback.
	double AccumSpecParkMs = 0.0;
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

	// WHICH EXIT DrainResults TOOK, per 5s window. Wave S0
	// (docs/speculative-generation-plan.md §4, executing T0-1).
	//
	// This exists because the open P0's headline reading may be an artifact of a
	// metric. The published claim is "apply budget only 8.5% saturated -- results
	// are not ARRIVING", and it rests on LastAppliedFrac, which divides Applied by
	// the COUNT ceiling (MaxApplies) while the loop actually breaks on a 6ms
	// WALL CLOCK. If the wall-clock exit dominates, the loop is exiting on TIME,
	// not on an empty queue, and the entire "results are not arriving" diagnosis
	// inverts -- the consumer is the wall, which is what the whole T4-1
	// re-sequencing is predicated on.
	//
	// Nothing currently logs which exit fired, so both readings are consistent
	// with every number ever taken. Four unconditional increments settle it.
	//
	// A frame that drains the queue completely takes the QueueEmpty exit, so in
	// steady state that SHOULD dominate; the question is what happens during the
	// cascade fill and under flight.
	int64 DrainExitQueueEmptySinceLog = 0;  // Dequeue returned false -- nothing left to apply
	int64 DrainExitWallClockSinceLog = 0;   // ApplyBudgetMs elapsed (past the kMinAppliesPerFrame floor)
	int64 DrainExitCountCapSinceLog = 0;    // Applied hit MaxAppliesPerFrame
	int64 DrainExitDrainCapSinceLog = 0;    // Drains hit kMaxResultDrainsPerFrame (stale backlog)

	// WHERE PER-APPLY TIME GOES, per 5s window. §1a prices the table push as the
	// dominant term and the batching wave is built on that, but the split has
	// never been measured. Milliseconds accumulated across the window; divide by
	// AppliesTimedSinceLog for a per-apply figure.
	//
	// POOLED BRANCH ONLY -- the component branch is the control arm and is not
	// split; see the note at the top of it in ApplyMeshResult. The fourth stage
	// §1a names, the table push, is not here either: it happens inside the pool
	// add, and splitting it needs the pool's own clocks. That is
	// UVoxelGpuPoolComponent::GetAndResetPushStats, and poolAdd below is its
	// total, so the two lines add up.
	//
	// Gated on voxel.Stream.ApplyStageStats -- these are FPlatformTime::Seconds
	// pairs on the hot apply path, which is exactly the kind of instrument that
	// can become what it measures. AppliesTimedSinceLog counts only applies that
	// were actually timed, so the per-apply divide stays honest when the gate is
	// flipped mid-run.
	double ApplyStagePackMs = 0.0;     // CPU-form quad repack (skipped entirely for GPU-resident chunks)
	double ApplyStageParamsMs = 0.0;   // SampleChunkParamsForPool: a full Amplifier::column on the game thread
	double ApplyStagePoolAddMs = 0.0;  // Pool->AddChunk / AddChunkFromGpu / UpdateChunk, INCLUDING their PushUpdatesToProxy
	int64 AppliesTimedSinceLog = 0;
	int64 RecordsAddedSinceLog = 0;     // RecomputeDesiredSet: ChunkRecords.Add calls (admission)
	int64 RecordsEvictedSinceLog = 0;   // DrainUnloads: records erased (component-less + parked)
	int64 CandidatesRejectedSinceLog = 0; // bounded admission: in-annulus candidates NOT admitted (never became records)
	int64 RecordsDroppedSinceLog = 0;     // bounded admission: queued-but-never-meshed records displaced by nearer work

	// Per-level breakouts of the three above. The global tallies cannot answer
	// "this ring keeps admitting records that never mesh -- who is removing
	// them", because every ring's adds and removes land in the same number.
	// Added when R0 was measured admitting ~512 records per pass while its
	// tracked count stayed at about one pass worth.
	int64 LevelRecordsAdded[VoxelCoords::kNumLevels] = {};
	int64 LevelRecordsDropped[VoxelCoords::kNumLevels] = {};
	int64 LevelRecordsEvicted[VoxelCoords::kNumLevels] = {};

	// WHICH exit test evicted, per level (ring-gap wave). LevelRecordsEvicted
	// above counts removals and every exit path lands in the same number, but
	// the three tests mean entirely different things: `out` is the ring's outer
	// hysteresis edge (normal, the world moving away), `in` is the INNER edge --
	// a finer ring claiming the footprint, which is the eviction the
	// load-before-unload stand-in exists for and the one the ring-gap hypothesis
	// is about -- and `vert` is the underground deep box's vertical keep-test,
	// which has no replacement at all.
	//
	// Counted in the same in/out/vert priority the eviction condition reads, one
	// bucket per evicted chunk, so the three sum to that level's exit-pass
	// evictions rather than double-counting a chunk that fails two tests.
	// Filled in RecomputeDesiredSet's exit pass, the only place all three are
	// known; reset with the rest of the 5s window in MaybeLogCounters.
	int64 LevelEvictInner[VoxelCoords::kNumLevels] = {};
	int64 LevelEvictOuter[VoxelCoords::kNumLevels] = {};
	int64 LevelEvictVertical[VoxelCoords::kNumLevels] = {};

	// --- Buried-chunk pre-dispatch skip, step 1 census (docs/status.md).
	// Per RING LEVEL, over the same 5s window as everything above: results
	// drained, of which zero-quad, split by composition. The existing
	// ZeroQuadAppliesSinceLog is a single world-wide number and cannot answer
	// "is this an R0 deep-column problem or an R3/R4 problem", which is the
	// question that decides where a skip should be aimed. The class split is
	// only populated under -VoxelMeasureEmpty (see MeasureEmptyEnabled).
	int64 LevelResultsSinceLog[VoxelCoords::kNumLevels] = {};
	int64 LevelZeroQuadSinceLog[VoxelCoords::kNumLevels] = {};
	int64 LevelAllAirSinceLog[VoxelCoords::kNumLevels] = {};
	int64 LevelAllSolidSinceLog[VoxelCoords::kNumLevels] = {};
	int64 LevelMixedEmptySinceLog[VoxelCoords::kNumLevels] = {};
	// Level-0 column-grid build time vs total job time, summed over the window:
	// the "what could a skip possibly reclaim" ratio.
	double AccumLevel0GridMs = 0.0;
	double AccumLevel0JobMs = 0.0;
	// Same two intervals in RETIRED CPU CYCLES rather than wall time, plus the
	// job count they are averaged over. See FJobResult::JobCycles: cycles/job
	// against ms/job is what distinguishes "the thread was descheduled or
	// downclocked" from "the thread burned more cycles on the same work", and
	// the grid build is the fixed-work probe (always exactly 1156 columns).
	uint64 AccumLevel0GridCycles = 0;
	uint64 AccumLevel0JobCycles = 0;
	int64 AccumLevel0Jobs = 0;
	// Per-brick skip census (see FJobResult::BricksSkippedAir). Denominator is
	// AccumLevel0Jobs * 64.
	int64 AccumLevel0BricksSkippedAir = 0;
	int64 AccumLevel0BricksSkippedSolid = 0;

	// -VoxelL0GridCacheProbe: simulated cross-job level-0 column-grid cache.
	// Game thread only, one packed-key probe per level-0 dispatch, no grids
	// stored -- see VoxelStreamAdmission::L0GridCacheProbeEntries for why the
	// measurement is worth taking before the cache is worth building.
	// L0ProbeRecency is least-recent-first; L0ProbeDistinct is unbounded and so
	// gives the ceiling an infinite-capacity cache would reach.
	TArray<uint64> L0ProbeRecency;
	TSet<uint64> L0ProbeResident;
	TSet<uint64> L0ProbeDistinct;
	int64 L0ProbeHitsSinceLog = 0;
	int64 L0ProbeMissesSinceLog = 0;
	int64 L0ProbeColdMissesSinceLog = 0; // misses on a footprint never seen this window
	// THE decisive number for this wave, and the one the job-count census
	// cannot give: worker WALL TIME split by outcome. 77% of jobs meshing to
	// zero quads only justifies a skip if those jobs are also a large share of
	// worker time -- the mesher early-outs on a uniform chunk, so a zero-quad
	// job is already cheaper than a surface one, and the honest ceiling on
	// this wave is LevelZeroQuadMs / (LevelZeroQuadMs + LevelQuadMs), not 77%.
	// Always accumulated (two float adds, no census sweep) so it can be read
	// from a run with -VoxelMeasureEmpty OFF -- the census's own 34^3 sweep
	// lands inside JobMs and would inflate the zero-quad side badly.
	double LevelZeroQuadMsSinceLog[VoxelCoords::kNumLevels] = {};
	double LevelQuadMsSinceLog[VoxelCoords::kNumLevels] = {};

	// Buried-chunk pre-dispatch skip: chunks proven featureless before any job
	// was launched, over the same 5s window. Split by which half of the band
	// proved it, because the two halves have very different causes (air =
	// footprint above the terrain, solid = fully-buried interior) and the
	// measured mix is the check that the band is behaving as the census said
	// it should.
	int64 BuriedSkipsSinceLog = 0;
	int64 BuriedSkipsByLevelSinceLog[VoxelCoords::kNumLevels] = {};
	int64 BuriedSkipAirSinceLog = 0;
	int64 BuriedSkipSolidSinceLog = 0;
	// -VoxelVerifyBuriedSkip: predicted-empty chunks actually meshed, and how
	// many of them turned out to have geometry. The second must be 0.
	int64 BuriedVerifyCheckedSinceLog = 0;
	int64 BuriedVerifyCheckedTotal = 0;
	int64 BuriedVerifyViolations = 0;

	// All-solid ADMISSION skip census (voxel.Stream.AdmissionSolidSkip): deep
	// candidates that never became records at all, which is the whole point --
	// the dispatch-time buried skip already stopped these from costing worker
	// time, but they still cost a record in every O(ChunkRecords) pass.
	int64 SolidSkippedAtAdmissionSinceLog = 0;
	int64 SolidSkippedAtAdmissionTotal = 0;
	int64 LevelSolidSkippedAtAdmission[VoxelCoords::kNumLevels] = {};
	// -VoxelVerifySolidSkip only: chunk keys IsChunkProvablyAllSolid claimed,
	// admitted and dispatched anyway so DrainResults can hold the claim against
	// the mesh the worker really produced. Empty and never touched otherwise.
	TSet<VoxelCoords::FVoxelLevelChunkKey> SolidSkipVerifyKeys;
	int64 SolidVerifyCheckedSinceLog = 0;
	int64 SolidVerifyCheckedTotal = 0;
	int64 SolidVerifyViolations = 0;

	// Buried-band ADMISSION skip census (voxel.Stream.AdmissionBandSkip). The
	// three numbers that decide whether this lever exists at all, counted over
	// level-0 surface-band candidates that were not already records:
	//   Warm    -- the footprint had a cached band to consult.
	//   Cold    -- it did not, so the candidate had to be admitted blind. A band
	//              only exists after some level-0 job in that (X,Y) has completed
	//              AND drained, so this is the ceiling on what the lever can
	//              never reach, and it is the number to read first.
	//   Skipped -- warm AND the band proved the chunk empty AND no edit vetoed
	//              it. Under mode 2 these are counted but still admitted.
	int64 BandAdmitWarmSinceLog = 0;
	int64 BandAdmitColdSinceLog = 0;
	int64 BandSkippedAtAdmissionSinceLog = 0;
	int64 BandSkippedAtAdmissionTotal = 0;
	int64 BandAdmitEditVetoSinceLog = 0;
	// Downward escape hatch: footprints whose ChunkZMin was widened by an edit
	// below the worldgen floor, and the deepest such widening seen, in chunks.
	int64 EditFloorWidenedSinceLog = 0;
	int32 EditFloorWidestChunks = 0;
	// Entry-scan gates cleared because an edit moved a footprint's recorded
	// edit range (see PropagateEditToMips). Without these the hatches are
	// dead code on a stationary anchor.
	int64 EditForcedRescansSinceLog = 0;

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
	void ComputeFootprintChunkZRange(int32 ChunkX, int32 ChunkY, int32 Level, int32& OutChunkZMin, int32& OutChunkZMax,
	                                  int32& OutChunkZMaxUntrimmed) const;
	// Upper bound (mm, sea-level datum) on Amplifier::column().surfaceMm over
	// EVERY column of this level-L footprint -- see namespace VoxelSkyBand for
	// the derivation. Returns INT64_MAX if it declines to bound (unexpected tile
	// pixel size); callers must treat that as "no information", never as air.
	//
	// Like ComputeFootprintChunkZRange this is a PURE function of
	// (Level, ChunkX, ChunkY) and the immutable tile raster -- no anchor, no
	// edits, nothing per-frame -- which is what lets its result live inside the
	// FootprintZRangeCache memo alongside the z-range it feeds.
	int64 FootprintSurfaceUpperBoundMm(int32 Level, int32 ChunkX, int32 ChunkY) const;
	// True iff every voxel of this chunk is provably above the terrain surface,
	// and therefore MAT_AIR, and therefore meshes to zero quads. Conservative:
	// false means "not proven", never "has geometry". Does NOT consider edits --
	// callers must veto on NeedsOverlayAwarePath first (a placed block is solid
	// material in what worldgen calls sky).
	bool IsChunkProvablyAllAir(const VoxelCoords::FVoxelLevelChunkKey& LevelKey) const;
	// Memoized vxc::Amplifier::solidBelowBoundMm for this level-L footprint:
	// absolute mm below which every voxel of the footprint is provably solid,
	// or INT64_MIN if the bound declined. Pure in (Level, ChunkX, ChunkY).
	int64 FootprintSolidFloorMmCached(int32 Level, int32 ChunkX, int32 ChunkY) const;
	// True iff every voxel of this chunk, apron included, is provably solid
	// rock and therefore meshes to zero quads (voxelcore/mesher.h emits a face
	// only where a solid voxel has an AIR neighbour). Conservative: false means
	// "not proven", never "has geometry". Does NOT consider edits -- callers
	// must veto on the footprint's lowest edited Z first, since a player can
	// dig into rock this says is solid.
	bool IsChunkProvablyAllSolid(const VoxelCoords::FVoxelLevelChunkKey& LevelKey) const;
	// Memoized wrapper around ComputeFootprintChunkZRange -- see
	// FootprintZRangeCache's doc comment for why memoizing is exact.
	void FootprintChunkZRangeCached(int32 ChunkX, int32 ChunkY, int32 Level, int32& OutChunkZMin, int32& OutChunkZMax,
	                                 int32& OutChunkZMaxUntrimmed) const;
	void PruneFootprintZRangeCache(const FVector& Anchor);
	// Load-before-unload, ring-gap fix: the Z half of "is the replacement chunk
	// this stand-in is waiting on actually DESIRED" -- see
	// FChunkRecord::RetainChildZMask for the bit layout and why Z is stamped once
	// while XY is re-evaluated live. Called only from RecomputeDesiredSet's exit
	// pass, at most once per LOD-transition eviction.
	uint8 ComputeRetainReplacementZMask(const VoxelCoords::FVoxelLevelChunkKey& Key, uint8 Dir,
	                                     const FVector& Anchor) const;
	void SortPendingQueues(const FVector& Anchor);
	// Drops the farthest entries of an ALREADY-SORTED (farthest-first) queue
	// until it fits EntryCap, removing their chunk records too, and writes the
	// re-admission cutoff distance. Returns true if anything was held back.
	bool DropFarthestOverCap(TArray<FSortEntry>& Entries, int32 EntryCap, double& OutCutoffDistSq);
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
	// GpuQuads (Wave D / D1) is the alternative to Quads: a handle to geometry
	// that is already in GPU memory, with GpuQuadCount describing it. Exactly one
	// of the two is ever populated; passing a payload FORCES the pooled branch,
	// because that is the only renderer that can consume it. Defaulted so every
	// CPU caller is unchanged.
	bool ApplyMeshResult(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material,
	                      const VoxelCoords::FVoxelLevelChunkKey& Key, VoxelStreaming::FChunkRecord& Rec, TArray<FVoxelChunkQuad>&& Quads,
	                      bool bIsGameThreadMesh,
	                      const FVoxelGpuQuadPayloadRef& GpuQuads = FVoxelGpuQuadPayloadRef(),
	                      int32 GpuQuadCount = 0);
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

	// --- ADR-0006 G3: the GPU-pool geometry path (voxel.Stream.GPU) --------

	// The one pool component every chunk's geometry lives in when
	// voxel.Stream.GPU is on. Created on first use and never destroyed per
	// chunk -- that is the entire point: streaming stops touching FScene.
	UVoxelGpuPoolComponent* GetOrCreateGpuPool(AActor& Owner, USceneComponent& Root,
	                                           UMaterialInterface* Material,
	                                           const FVector& FirstChunkOrigin);

	// Origin the pooled chunk table is expressed relative to. See
	// GetOrCreateGpuPool for why this exists at all.
	FVector GpuPoolRebase = FVector::ZeroVector;

	// The chunk root the pool was created against (Wave D / D1).
	//
	// SubmitGpuMeshJob needs a chunk's WORLD origin to ask GI whether it wants
	// that chunk's quads, and it must compose it the SAME way ApplyMeshResult
	// does -- Root.GetComponentLocation() + ChunkOriginWorldForLevel(...) -- or
	// the two would disagree about which chunks are inside the GI radius, which
	// is silent in both directions. DispatchJobs has no Root parameter, so it is
	// captured here at pool creation instead of re-derived. Only ever read on the
	// direct-to-pool path, which already requires the pool to exist.
	TWeakObjectPtr<USceneComponent> GpuPoolRoot;

	// Gives back whatever geometry a record is holding, on either path, and
	// leaves the record geometry-less.
	//
	// Every "this chunk should stop being drawn" site routes through here.
	// Before G3 there were four of them, each open-coding the same
	// component-park-and-null dance; with two renderers that is four places to
	// forget the pooled case. It dispatches on what the record HOLDS, not on
	// the cvar, so a chunk loaded before a mid-session flip still releases
	// correctly.
	void ReleaseChunkGeometry(VoxelStreaming::FChunkRecord& Rec);
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

public:
	// Rebuild the edit-derived ADMISSION state (EditedFootprintMinZ/MaxZ,
	// EditedAncestorChunks) from the restored overlay after a saved world is
	// replayed. Public because the only caller is LoadWorld, a free function.
	// See the definition for why a load cannot go through PropagateEditToMips.
	void RebuildEditedFootprintsFromOverlay();

private:
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

	// voxel.Stream.CoverageVerify (ring-gap wave): turn "I can see holes" into a
	// logged number. Called once per periodic-log window from MaybeLogCounters,
	// and READ-ONLY -- it inspects ChunkRecords and re-derives ring geometry, and
	// changes no streaming state whatsoever. Its own function rather than a block
	// inside MaybeLogCounters because it must be defined below namespace
	// VoxelUnderground, whose depth-skirt rule it mirrors. See its definition for
	// what "visibly covered" means and why.
	void LogCoverageVerify();

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

	// Anchor motion, before LastAnchorLocation is overwritten below. See the
	// PrevAnchorLocation declaration for why this is finite-differenced rather
	// than read from the pawn's movement component.
	if (bHasPrevAnchor && DeltaTime > SMALL_NUMBER)
	{
		const FVector InstantVelocity = (Anchor - PrevAnchorLocation) / double(DeltaTime);
		// EMA with a ~0.25 s time constant, frame-rate independent. Smoothing the
		// VECTOR rather than speed-and-heading separately means a direction change
		// shortens the lead while it settles, which is the conservative direction:
		// speculation aimed at where the camera WAS is waste, and a shorter lead
		// while turning is exactly what should happen.
		constexpr double kSpeedTauSeconds = 0.25;
		const double Alpha = FMath::Clamp(double(DeltaTime) / kSpeedTauSeconds, 0.0, 1.0);
		SmoothedAnchorVelocity += (InstantVelocity - SmoothedAnchorVelocity) * Alpha;
		SmoothedAnchorSpeedUUPerSec = SmoothedAnchorVelocity.Size();
	}

	// Recomputed every tick so it tracks a cvar change without a restart.
	{
		const double LeadSec = double(VoxelDebug::GetStreamVelocityLeadSec());
		const double MaxLeadUU = double(VoxelDebug::GetStreamVelocityLeadMaxUU());
		const FVector Lead = SmoothedAnchorVelocity * LeadSec;
		PredictedAnchorLocation =
			Anchor + (Lead.SizeSquared() > MaxLeadUU * MaxLeadUU ? Lead.GetSafeNormal() * MaxLeadUU : Lead);
	}
	PrevAnchorLocation = Anchor;
	bHasPrevAnchor = true;

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
	// PER LEVEL, not against the summed queue depth. The summed form was a
	// priority inversion severe enough to be the single thing standing between
	// this project and an underground screenshot, and it is worth spelling out
	// because nothing about it is visible from the surface flight:
	//
	// Standing still underground, `loaded` climbed at ~12 chunks/s and stalled
	// with R0 holding 162 chunks and `pending=0` -- its queue completely
	// EMPTY, workers idle for it -- while ~20,000 level-0 candidates sat
	// rejected by the admission budget. The refill that would have let them in
	// is gated on the TOTAL pending count falling under a quarter of the cap,
	// and the total was 500-800 the whole time: R3 and R4 entries, which cost
	// ~1 s and ~10 s each. So R0's refill waited on R4's queue. On a surface
	// flight the movement triggers hide this completely (the anchor crosses a
	// level-0 chunk every 3.2 m and recomputes anyway); it only bites when the
	// player stands still, which is exactly what you do to look at a view.
	//
	// A level's queue draining is a statement about THAT level's workers, so
	// the trigger and the rescan are both per level now. Each ring is measured
	// against its own share of the cap -- the same kRingCapShare the truncate
	// pass already divides the cap by, so the two halves agree on what "this
	// ring's queue is empty" means.
	// voxel.Stream.PerLevelRefill=0 restores the summed-queue form exactly, so
	// the A/B for this change runs on ONE binary -- the same reason
	// -VoxelRingQuota exists. It is safe from -ExecCmds because it is read
	// fresh on every tick rather than latched at startup.
	const int32 AdmissionCap = VoxelStreamAdmission::GetPendingJobCap();
	bool bLevelWantsRefill[VoxelCoords::kNumLevels] = {};
	bool bAdmissionRefill = false;
	if (bHasRecomputed && AdmissionCap > 0)
	{
		bool bAnyDeferred = false;
		for (const bool bDeferred : bAdmissionDeferredWork)
		{
			bAnyDeferred |= bDeferred;
		}
		if (!VoxelStreamAdmission::GetPerLevelRefillEnabled())
		{
			// Pre-change behaviour: one summed threshold, and every level
			// re-enumerates when it fires.
			if (bAnyDeferred && PendingJobNum() * 4 < AdmissionCap)
			{
				bAdmissionRefill = true;
				for (bool& bWants : bLevelWantsRefill)
				{
					bWants = true;
				}
			}
		}
		else if (bAnyDeferred)
		{
			for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
			{
				if (!bAdmissionDeferredWork[Level])
				{
					continue; // this ring has nothing waiting; an empty queue is just convergence
				}
				// EMPTY, not "under a quarter of its share". The quarter
				// threshold is what the summed form used, and carrying it over
				// per level was measured as a ~3 ms p95 regression on the M1
				// surface flight: R0's queue there sits at 146-314 against a
				// share whose quarter is ~180, so the trigger was satisfied on
				// most frames and R0 re-ran its full entry scan (1.6-2.4 ms)
				// almost every tick, for nothing -- on a moving anchor the
				// movement trigger was already going to rescan it.
				//
				// An EMPTY queue is a different statement, and it is exactly
				// the pathology this fix exists for: workers with nothing left
				// to do for this ring while candidates sit rejected. It cannot
				// fire on a flight that is keeping the ring fed, and it fires
				// immediately when a stationary underground anchor starves it.
				// EMPTY is not the property wanted here -- "has no work this ring
				// can actually dispatch" is.
				//
				// A ring only re-enumerates when the anchor crosses one of its
				// chunks or when this trigger fires. Gating the trigger on an
				// empty queue lets a BOUNDED number of permanently-undispatchable
				// entries starve the whole ring: measured, two cold-band-deferred
				// chunks held R0 at 255 drawn against ~1,950 desired, on a
				// stationary anchor, with the trigger armed the entire time.
				//
				// So discount the entries the last dispatch pass put straight
				// back. If nothing else remains, this ring is drained as far as it
				// can act on, and refilling is exactly right. The two bugs are
				// independent -- the stranded mark is fixed separately -- but this
				// is what stops the NEXT stuck entry doing the same thing.
				const int32 Undispatchable = (Level == 0) ? ColdBandHeldThisFrame : 0;
				// GENUINE-ROOM GUARD (2026-07-27, the R0 churn loop). The discount
				// above has a degenerate case: when the cold-band throttle holds
				// R0's ENTIRE queue, Num() <= ColdBandHeldThisFrame is an identity
				// (DrainUnloads re-appends the held entries to the same queue), so
				// this trigger fired EVERY TICK of a 110 s flight -- each refill
				// re-enumerated the annulus, admitted Cap/4 = 512, and the
				// truncation pass threw ~500 straight back out because the queue
				// was already AT its ring-share cap. Measured on the B1 leg:
				// ~237,600 rejections/s, ~30,000 record adds + erases/s sustained,
				// and R0 residency collapsing 2,024 -> 92 during the burst. A
				// refill into a queue with no room is pure churn: require genuine
				// room below the ring's own truncation cap before firing. The
				// legitimate case this trigger exists for (a drained-but-for-
				// undispatchables queue) still fires: a drained queue is far
				// below its cap by definition.
				const int32 LevelCap = FMath::Max(
				    1, FMath::RoundToInt(double(AdmissionCap) * VoxelStreamAdmission::kRingCapShare[Level]));
				if (PendingJobKeysByLevel[Level].Num() <= Undispatchable &&
				    PendingJobKeysByLevel[Level].Num() < LevelCap)
				{
					bLevelWantsRefill[Level] = true;
					bAdmissionRefill = true;
				}
			}
		}
	}

	// STALE-SCAN REFILL (ring-gap fix, 2026-07-27) -- a SECOND, independent
	// per-level trigger, OR'd with the admission-deferred one above.
	//
	// THE DEFECT. Every refill trigger that existed before this one is gated on
	// bAdmissionDeferredWork, which is set in exactly two places: the per-level
	// admission budget rejecting a candidate, and the per-level distance cutoff
	// rejecting one. Those are the ways a level KNOWS it left work behind. There
	// are two ways it does not:
	//
	//  1. A column skipped by a DISTANCE test at a stale anchor. The entry pass's
	//     `Level > 0` inner skip and its seam-padding parent test both decide
	//     against the anchor position of the scan that ran; neither records that
	//     it declined anything. Move the anchor a few metres and the answer
	//     changes -- but nothing re-asks.
	//  2. A column missed because the SCAN GATE quantizes coarser than the seam
	//     padding. Level L only rescans once the anchor leaves its level-L chunk
	//     (51.2 m at L4), while the padded admit radius moves continuously with
	//     the anchor. The band between the two is never enumerated.
	//
	// Neither path sets bAdmissionDeferredWork, so when the anchor STOPS, the
	// residue is permanent. Measured on the 2026-07-27 instrumented line flights:
	// 63-123 uncovered columns at r=65 m and r=124-128 m -- the R0/R1 and R1/R2
	// seams -- surviving indefinitely, at BYTE-IDENTICAL coordinates across four
	// independent runs. Deterministic, which is what says it is a predicate bug
	// and not a throughput one: no amount of waiting or worker headroom fills it.
	//
	// THE TRIGGER. A level wants a rescan when its queue holds nothing it can
	// dispatch (same expression the deferred path uses -- there is no point
	// re-enumerating a ring whose workers are still busy) AND the anchor has
	// moved at all since that level's entry pass last actually ran. 1 UU is
	// "moved at all"; the threshold exists only to keep float noise on a
	// perfectly-still anchor from re-arming it. It is self-quiescing: the rescan
	// updates LastEntryScanAnchorXY to the current anchor, so a pinned anchor
	// fires each level exactly once and then goes quiet -- which is the whole
	// point, since the pinned anchor is the case that was broken.
	//
	// Deliberately NOT gated on AdmissionCap or on voxel.Stream.PerLevelRefill:
	// both of those A/B the admission-budget mechanism, and neither of the two
	// paths above has anything to do with the admission budget.
	//
	// QUIESCENT-ANCHOR GATE (rev B, same day). The first cut fired this trigger
	// whenever a level's queue drained, and the prediction two paragraphs up --
	// that a flight keeps every ring's queue fed -- was measured WRONG the same
	// night: R1-R5 queues empty routinely mid-flight, so the trigger fired at
	// exactly the edge/4 rate all flight long (R1-R5 entry scans x3.9, matched
	// to <2%). The scans themselves were cheap (+0.1% recompute on the GPU leg)
	// but the RE-ADMISSION CHURN they fed was not: coarse fork dispatches +17%
	// (L3 +271%), CPU stale drains +143%, R0 mean residency crowded from 864
	// down to 653, and it seeded the pre-existing R4 deferred-refill oscillation
	// on 83% of flight ticks (10.3x its baseline). None of that bought coverage
	// -- the residue this trigger exists to heal is a PINNED-anchor defect, and
	// while moving, the crossing gates already rescan every level.
	//
	// So the trigger now requires the anchor to have been QUIESCENT for
	// kStaleRescanStillSeconds first: the deterministic residue heals within a
	// second of stopping (the trigger then fires each stale level exactly once
	// and goes quiet), and a moving anchor gets byte-identical behavior to the
	// pre-trigger build. A slow CREEP (below the quiescence threshold per tick
	// but never still) keeps the crossing gates as its only rescan source --
	// acceptable, because creep also keeps firing them.
	{
		const FVector2D AnchorXY(Anchor.X, Anchor.Y);
		const double kStillEpsilonUU = 1.0; // float noise, not movement
		if (FVector2D::DistSquared(AnchorXY, LastQuiescenceAnchorXY) > FMath::Square(kStillEpsilonUU))
		{
			LastQuiescenceAnchorXY = AnchorXY;
			AnchorStillSeconds = 0.f;
		}
		else
		{
			AnchorStillSeconds += DeltaTime;
		}
	}
	if (bHasRecomputed && AnchorStillSeconds >= 0.5f /*kStaleRescanStillSeconds*/)
	{
		const FVector2D AnchorXY(Anchor.X, Anchor.Y);
		for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
		{
			if (bLevelWantsRefill[Level] || !bHasRecomputedLevel[Level])
			{
				continue; // already re-arming, or has never scanned (no stale anchor to compare)
			}
			const int32 Undispatchable = (Level == 0) ? ColdBandHeldThisFrame : 0;
			if (PendingJobKeysByLevel[Level].Num() > Undispatchable)
			{
				continue; // this ring still has work in hand
			}
			const double RescanThresholdUU = ChunkEdgeUUForLevel(Level) * 0.25;
			if (FVector2D::DistSquared(AnchorXY, LastEntryScanAnchorXY[Level]) > FMath::Square(RescanThresholdUU))
			{
				bLevelWantsRefill[Level] = true;
				bAdmissionRefill = true;
			}
		}
	}
	if (!bHasRecomputed || AnchorChunk.X != LastAnchorChunk.X || AnchorChunk.Y != LastAnchorChunk.Y ||
	    bUndergroundChanged || (bAnchorUnderground && AnchorChunk.Z != LastAnchorChunk.Z) || bAdmissionRefill)
	{
		if (bAdmissionRefill)
		{
			// Only the drained levels re-enumerate. Clearing the gate for all
			// five would make a refill for R0 drag R3 and R4 through a full
			// candidate scan they have no room for, which is the cost the
			// per-level gate exists to avoid in the first place. This property
			// is what lets the stale-scan trigger above simply OR into
			// bLevelWantsRefill: a level re-arming for a stale anchor cannot
			// drag its neighbours through a scan they did not ask for.
			for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
			{
				if (bLevelWantsRefill[Level])
				{
					bHasRecomputedLevel[Level] = false;
				}
			}
		}
		// NOTE: batching the recompute pass was tried and REVERTED. The idea was
		// sound -- adoption unparks, UnparkChunk publishes, and S1-1's scope
		// covers the drains rather than the recompute, so ~23,700 adopts were
		// each paying a full publication. But wrapping this call in an
		// FScopedBatch took parking from 28,117 parks/84% hit to ZERO parks and
		// chunks/s 1,040 -> 540, and the world was still churning at the end of
		// the linger. Cause not established; the scope is the only delta between
		// the two legs. Do not re-apply without diagnosing that first.
		// TASK #17, BEHIND A GATE. Wrapping this call in an FScopedBatch took
		// parking from 28,117 parks (84% hit) to EXACTLY ZERO, reproducibly, and
		// was reverted undiagnosed. "Exactly zero" means either ParkChunkGeometry
		// stopped being called or one of its refusals became universal, and the
		// park census now counts all four refusals separately, so one leg with
		// this on distinguishes them.
		//
		// Default 0. This is an experiment, not a feature: it made things
		// measurably worse and nothing here claims to have fixed that.
		if (VoxelDebug::GetStreamBatchRecompute() != 0)
		{
			UVoxelGpuPoolComponent::FScopedBatch RecomputeBatch(GpuPool.Get());
			RecomputeDesiredSet(Anchor);
		}
		else
		{
			RecomputeDesiredSet(Anchor);
		}
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
		// T4-1: enumerate the leading edge BEFORE dispatch, so anything queued
		// this tick can be submitted this tick into whatever budget demand
		// leaves. Cheap and a no-op while voxel.Stream.VelocityLeadSec is 0.
		const double SpecEnumT0 = FPlatformTime::Seconds();
		EnumerateSpeculativeCandidates();
		AccumSpecEnumerateMs += (FPlatformTime::Seconds() - SpecEnumT0) * 1000.0;

		// T0/T1 are declared out here and assigned inside the batch block for the
		// same reason T2/T3/T3b already are: the scope has to CLOSE before T4 so
		// the flush is inside the measured tick, but these are read by the
		// attribution below it.
		double T0 = 0.0;
		double T1 = 0.0;
		double T2 = 0.0;
		double T3 = 0.0;
		double T3b = 0.0;
		{
		T0 = FPlatformTime::Seconds();
		// THE BATCH SCOPE STARTS HERE, NOT BELOW, AND T4-1 IS WHY.
		//
		// The comment on the scope below used to say it deliberately excluded
		// this DispatchJobs because "that submits jobs and touches no pool
		// state". That was true when it was written and T4-1 made it false:
		// GpuMeshJobs->Tick() calls OnGpuMeshJobComplete, and a SPECULATIVE
		// completion runs ParkSpeculativeResult inline -- AddChunkFromGpu plus
		// ParkChunk, both pool mutations, on this side of the old scope.
		//
		// Outside the batch, each one published the whole chunk table on its own.
		// Measured: specPark was 334.7 ms of a 343.9 ms dispatch bucket -- 97% of
		// T4-1's entire game-thread cost, and precisely the per-mutation
		// publication S1-1 removed for demand. Speculation was paying the bill
		// S1-1 had already settled, because it entered the pool through a door
		// the scope did not cover.
		//
		// The hazard argument is unchanged: AddChunkFromGpu still does the
		// UnmarkQuadsDirty interval subtract that guards the
		// free-then-reallocate-within-one-frame race, which is what makes any
		// widening of this scope safe.
		UVoxelGpuPoolComponent::FScopedBatch PoolBatch(GpuPool.Get());
		DispatchJobs();
		// Poll the GPU runner BETWEEN dispatch and drain (Wave D / D4).
		//
		// Order matters and this is the only correct slot. Tick() is what calls
		// OnGpuMeshJobComplete, which enqueues onto ResultsQueue -- so running
		// it here means a job that finished during the frame is drained in the
		// SAME frame, exactly as a worker job that finished mid-frame is.
		// Ticking after DrainResults would add a guaranteed one-frame delay to
		// every GPU chunk and would show up as the fork being slower than it is.
		//
		// Cheap and safe with nothing outstanding, and the pointer is null until
		// the first forked dispatch, so a run without -VoxelGpuMesh pays a null
		// check per frame.
		if (GpuMeshJobs.IsValid())
		{
			GpuMeshJobs->Tick();
		}
		T1 = FPlatformTime::Seconds();

		// S1-1 (docs/speculative-generation-plan.md Wave S1): ONE pool
		// publication for this whole tick instead of one per mutated chunk.
		//
		// SCOPE BOUNDS ARE THE DESIGN. It opens here, before DrainResults, and
		// closes after DrainUnloads -- so every add this tick makes and every
		// remove it makes ride the same render command. It deliberately does NOT
		// wrap the first DispatchJobs (above, at T0): that submits jobs and
		// touches no pool state, so including it would widen the scope for
		// nothing. The SECOND DispatchJobs falls inside these bounds by position
		// and that is harmless for the same reason.
		//
		// It also deliberately does not wrap the whole tick. ApplyMeshResult is
		// reachable from the edit path (ApplyGroupedEdits -> the game-thread
		// re-mesh), which must keep publishing on its own -- an edit is a
		// user-visible single event, not part of a streaming batch.
		//
		// Null pool is fine and is the ordinary case on the first frames: the
		// pool is created lazily inside ApplyMeshResult, so a scope opened before
		// it exists simply does not engage and those applies publish per-chunk,
		// exactly as they did before. FScopedBatch holds a weak pointer.
		//
		// WITH THE GATE OFF this costs one increment, one decrement and one bool
		// test per tick, and every mutator flushes exactly as it always has.
		//
		// COST ATTRIBUTION, so the tick-budget line is not read wrongly: the
		// flush runs at the close, so with batching ON the tick's single
		// publication lands in ThisFrameUnloadMs rather than being spread across
		// ApplyMs. The publication's real cost, split across both threads, is the
		// `Voxel pool publish` line from S0-1 -- read that, not the unload bucket.
		T2 = T1;
		T3 = T1;
		T3b = T1;
		{
			// The batch is already open (see T0 above); this block keeps its
			// original bounds for readability and for the T2/T3 timing points.
			DrainResults(Owner, Root, Material);
			T2 = FPlatformTime::Seconds();
			DrainGameThreadMesh(Owner, Root, Material);
			T3 = FPlatformTime::Seconds();

		// Second DispatchJobs() pass (voxel.Stream.DispatchAfterDrain, default
		// 1) -- refills the slots DrainResults just freed IN THIS FRAME rather
		// than leaving them idle until next frame's single dispatch. See
		// CVarVoxelStreamDispatchAfterDrain's comment in VoxelDebug.cpp for the
		// ~9% worker-utilisation measurement this addresses and how it
		// complements JobsInFlightPerCore=8 (which widens the buffer but does
		// not touch the once-per-frame cadence).
		//
		// SLOT CHOICE: after DrainGameThreadMesh, before DrainUnloads -- not
		// straight after DrainResults. Two reasons. First, DrainResults is what
		// decrements LevelJobsInFlight (see its ring-quota bookkeeping), so the
		// ring-quota pass inside a second DispatchJobs only sees accurate
		// per-ring deficits once DrainResults has actually run; parking it here
		// keeps that ordering trivially satisfied regardless of exactly where
		// in [after DrainResults, before DrainUnloads] it lands. Second, and
		// decisively, DispatchJobs has always run BEFORE DrainUnloads within a
		// tick (dispatch against this frame's desired set, then prune it) --
		// this preserves that same invariant for the second pass instead of
		// having a same-frame dispatch race a same-frame unload.
		//
		// GATED ON QUEUE PRESSURE: PendingJobNum() sums every per-level queue
		// (see its doc comment), so this is the "nothing pending" case called
		// out above -- if the first dispatch already drained every ring dry,
		// a second pass has nothing to do and costs only this scan.
		//
		// TIMING: folded into ThisFrameDispatchMs/AccumDispatchMs (not
		// RemeshMs or UnloadMs) so the tick-budget line still attributes all
		// dispatch cost to the dispatch bucket. T3b collapses to T3 (zero
		// delta) when the cvar is off or the gate skips, so UnloadMs stays
		// exactly (T4 - T3) in that case -- byte-identical to the old
		// single-dispatch behaviour.
			T3b = T3;
			if (VoxelDebug::GetStreamDispatchAfterDrain() != 0 && PendingJobNum() > 0)
			{
				DispatchJobs();
				T3b = FPlatformTime::Seconds();
			}

			DrainUnloads();
		}
		} // PoolBatch closes here -- the tick's single publication happens now.
		const double T4 = FPlatformTime::Seconds();

		// Hitch attribution timing (docs/status.md "Perf-run hitches"
		// isolation task): four cheap FPlatformTime::Seconds() calls (already
		// established practice in this function -- see TickStartSeconds
		// above), always collected so the per-frame log below has real
		// numbers the instant a hitch happens, not just from the next frame.
		ThisFrameDispatchMs = float((T1 - T0) * 1000.0 + (T3b - T3) * 1000.0);
		ThisFrameApplyMs = float((T2 - T1) * 1000.0);
		ThisFrameRemeshMs = float((T3 - T2) * 1000.0);
		ThisFrameUnloadMs = float((T4 - T3b) * 1000.0);

		// Streaming re-measure: same four samples, also summed into the 5s
		// window so the periodic log can report where tick time goes on runs
		// that never cross the hitch threshold at all.
		AccumDispatchMs += (T1 - T0) * 1000.0 + (T3b - T3) * 1000.0;
		AccumApplyMs += (T2 - T1) * 1000.0;
		AccumRemeshMs += (T3 - T2) * 1000.0;
		AccumUnloadMs += (T4 - T3b) * 1000.0;
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

	// --- FRAME ATTRIBUTION (voxel.Stream.FrameAttribution) ------------------
	//
	// SAMPLED ON EVERY FRAME, WHICH IS THE ENTIRE POINT. The hitch block below
	// captures the same per-thread timers, but only above 33.3 ms -- so it can
	// describe the tail and can say nothing at all about a typical frame. That
	// asymmetry produced a whole wrong analysis on 2026-07-28 (lesson 17: every
	// frame-time figure grepped out of the Hitch line was a median of frames
	// that had ALREADY exceeded the threshold), and then a second wrong one: the
	// frame tail was attributed to the streaming tick because perTick happened to
	// match the p50-to-p95 gap, a third of the tick was removed, and p50/p95 did
	// not move.
	//
	// So this collects the unconditional series and the periodic report below
	// splits it into FAST frames and SLOW frames and prints the per-component
	// delta. The component that differs most between those two buckets IS the
	// tail, measured rather than inferred.
	if (VoxelDebug::GetStreamFrameAttribution() != 0)
	{
		const double CyToMs = FPlatformTime::GetSecondsPerCycle() * 1000.0;
		FFrameSample Sample;
		Sample.FrameMs      = FrameMs;
		Sample.VoxelTickMs  = TickMsSoFar;
		Sample.RenderMs     = float(GRenderThreadTime * CyToMs);
		Sample.RenderWaitMs = float(GRenderThreadWaitTime * CyToMs);
		Sample.RHIMs        = float(GRHIThreadTime * CyToMs);
		Sample.GameWaitMs   = float(GGameThreadWaitTime * CyToMs);
		// NO DIRECT GPU TIME. It needs FRHIGPUFrameTimeHistory, a pull-based API
		// with its own state, which is more plumbing than this pass warrants --
		// and it is not needed to answer the question: a GPU-bound frame shows up
		// here as the CPU WAITING for it, i.e. renderWait or gameWait rising. If
		// those dominate the slow-frame delta, the answer is "GPU", and only then
		// is it worth wiring the real counter to find out which pass.
		if (FrameSamples.Num() < kMaxFrameSamples)
		{
			FrameSamples.Add(Sample);
		}
	}

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
		       TEXT("entryMs %s | footprints %s | tracked=%d"),
		       ThisFrameRecomputeMs, ThisFrameExitScanMs, ThisFrameSortMs,
		       *JoinPerLevel([&](int32 L) { return FString::Printf(TEXT("R%d=%.2f"), L, ThisFrameLevelEntryMs[L]); }),
		       *JoinPerLevel([&](int32 L) { return FString::Printf(TEXT("R%d=%d"), L, ThisFrameLevelFootprints[L]); }),
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

	// --- GPU-meshed jobs (Wave D / D4) --------------------------------------
	//
	// A GPU job adds NOTHING to InFlightTasks -- there is no UE::Tasks::TTask,
	// only a ref-counted job object inside FVoxelGpuMeshJobManager -- so the
	// loop above does not cover it and this function's whole guarantee would
	// have a hole in it exactly the size of the wave.
	//
	// WHY CancelAll AND NOT A WAIT. The manager cannot be waited on: its
	// completion path is a render-thread readback polled from Tick(), and Tick
	// is not going to run again during teardown. CancelAll() is the manager's
	// own answer -- it delivers Cancelled for every queued and in-flight job,
	// which is what satisfies the exactly-one-outcome invariant its header
	// states. It deliberately does NOT flush the render thread: any render
	// command still in flight holds its own reference to the job it touches, so
	// those objects stay alive after this returns and die with the manager.
	//
	// WHY THE CALLBACK MUST BE NEUTRALISED FIRST, and this is the subtle half.
	// CancelAll delivers results, and our delivery callback enqueues onto
	// ResultsQueue and decrements JobsInFlightCounter -- i.e. it TOUCHES Impl
	// state, during Impl teardown, for chunks whose records are about to be
	// destroyed. Nothing would ever drain that queue. So the fork is disarmed
	// first: bAcceptingGpuResults is cleared, the callback becomes a counted
	// no-op, and the cancellations are absorbed rather than acted on.
	//
	// The invariant is not weakened by that. "Exactly one outcome per job" is a
	// statement about the MANAGER, and CancelAll still satisfies it for every
	// job. What changes is only what the subsystem chooses to do with an
	// outcome that arrives after it has stopped streaming, which is nothing --
	// the correct action for a world that is being destroyed.
	bAcceptingGpuResults = false;
	if (GpuMeshJobs.IsValid())
	{
		const int32 Outstanding = GpuMeshJobs->NumQueued() + GpuMeshJobs->NumInFlight();
		GpuMeshJobs->CancelAll();
		if (Outstanding > 0)
		{
			UE_LOG(LogVoxelStream, Log,
			       TEXT("WaitForInFlightTasks: cancelled %d outstanding GPU mesh job(s) at teardown "
			            "(%lld absorbed after the fork was disarmed)"),
			       Outstanding, (long long)GpuResultsAbsorbedAtTeardown);
		}
		GpuMeshJobs.Reset();
	}
}

namespace
{
// S0-3 (docs/speculative-generation-plan.md Wave S0 / T0-2). Same idiom as the
// pre-existing WorkerJobMsWindow percentile computation in UpdatePerfSnapshot
// (sort a small copy of the ring buffer, index by rank) -- factored out here
// because S0-3 adds seven of these windows (one per producer per stage) and
// this is the read side for all of them. Window may hold more slots than
// Count (ring not yet full after startup); only the first Count entries were
// ever written by PushLatencyMsSample below.
struct FMsPercentiles
{
	float P50 = 0.f;
	float P95 = 0.f;
	float Max = 0.f;
};

FMsPercentiles ComputeMsPercentiles(const TArray<float>& Window, int32 Count)
{
	FMsPercentiles Out;
	if (Count <= 0)
	{
		return Out;
	}
	TArray<float> Sorted;
	Sorted.Append(Window.GetData(), Count);
	Sorted.Sort();
	const int32 P50Index = FMath::Clamp(int32(Sorted.Num() * 0.50f), 0, Sorted.Num() - 1);
	const int32 P95Index = FMath::Clamp(int32(Sorted.Num() * 0.95f), 0, Sorted.Num() - 1);
	Out.P50 = Sorted[P50Index];
	Out.P95 = Sorted[P95Index];
	Out.Max = Sorted.Last();
	return Out;
}

// The write side: same overwrite-and-wrap ring buffer WorkerJobMsWindow itself
// uses inline in DrainResults, pulled into one place because S0-3's writes
// happen at three different call sites (OnGpuMeshJobComplete, the CPU worker
// task body, DrainResults) rather than DrainResults alone.
void PushLatencyMsSample(TArray<float>& Window, int32& Next, int32& Count, float ValueMs)
{
	Window[Next] = ValueMs;
	Next = (Next + 1) % Window.Num();
	Count = FMath::Min(Count + 1, Window.Num());
}
} // namespace

void FVoxelWorldImpl::MaybeLogCounters(float DeltaTime)
{
	// Cadence is 5 s by default and overridable with -VoxelPerfLogInterval=<sec>.
	//
	// The override exists because this line is the ONLY instrument for cold-fill
	// time, and 5 s quantisation is the same order as the quantity being measured
	// (a 64 m cascade settles in ~10-30 s). Wave F compares fill time between two
	// cascades, so the quantisation error would land squarely on the difference.
	// Scalar parse, no comma -- see GetRingPresets for why that distinction is
	// load-bearing in this file.
	//
	// Clamped below at 0.25 s: this does an O(tracked) scan over ChunkRecords,
	// which is ~16,600 entries at R0 = 128 m, so an unclamped small value would
	// make the instrument a significant share of the thing it measures.
	static const float LogIntervalSeconds = []
	{
		float Value = 5.0f;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfLogInterval="), Value);
		return FMath::Max(0.25f, Value);
	}();

	LogTimerAccumSeconds += DeltaTime;
	if (LogTimerAccumSeconds < LogIntervalSeconds)
	{
		return;
	}
	// The actual elapsed window (>= LogIntervalSeconds by at most one frame,
	// per the "Voxel tick budget" comment below) -- captured before the reset
	// so the S0-2 chunks/s figure divides by what really elapsed, not the
	// nominal interval.
	const float ThisLogWindowSeconds = LogTimerAccumSeconds;
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
	       JobsInFlightCounter.GetValue(), PendingJobNum(), PendingGameThreadKeys.Num(), PendingUnloadKeys.Num(),
	       bAnchorUnderground ? 1 : 0, DeepTracked, DeepWithGeometry, (long long)ResidentQuads);

	// The anchor drives every ring footprint, so when one ring's population
	// differs between two runs this is the first thing to compare.
	UE_LOG(LogVoxelPerf, Log, TEXT("Voxel anchor: (%.0f, %.0f, %.0f)"),
	       LastAnchorLocation.X, LastAnchorLocation.Y, LastAnchorLocation.Z);

	// IS THE PLAYER STANDING ON THE TERRAIN THAT WAS ACTUALLY GENERATED?
	//
	// This has to be checked, out loud, because the failure is INVISIBLE. The
	// default spawn is (0,0). The tiles are wherever the terrain service put
	// them -- and if those disagree, TileGridSampler answers every elevation
	// query with the missing-tile default. The result is not an error or a hole:
	// it is a smooth, plausible, entirely fake sea-level world, with a far vista
	// that reads as stock UE terrain. No crash, no warning, nothing in the log
	// that a person would connect to "the shape is wrong".
	//
	// It presented exactly that way on the first hands-on test of this project,
	// after two days of headless work that had all been measuring the same fake
	// terrain. The tile-coordinate bounding box was already being logged -- in
	// TILE coordinates, which nobody can compare to a spawn position by eye.
	//
	// Logged ONCE, as an Error, naming the coordinate that fixes it.
	if (!bTileCoverageChecked && TileCoverageUU.bIsValid)
	{
		bTileCoverageChecked = true;
		const FVector2D AnchorXY(LastAnchorLocation.X, LastAnchorLocation.Y);
		if (!TileCoverageUU.IsInside(AnchorXY))
		{
			const FVector2D CentreUU = TileCoverageUU.GetCenter();
			UE_LOG(LogVoxelEarth, Error,
			       TEXT("SPAWN IS OUTSIDE THE GENERATED TERRAIN. Anchor (%.0f, %.0f) m is not inside "
			            "the loaded tiles, which cover X [%.0f, %.0f] m, Y [%.0f, %.0f] m. Every "
			            "elevation query here returns the missing-tile SEA-LEVEL DEFAULT, so the "
			            "world is flat and generic and the far vista looks like stock UE terrain -- "
			            "it is NOT the terrain the tiles describe. Relaunch with "
			            "-VoxelSpawnAt=%.0f,%.0f (metres) to stand on real data."),
			       AnchorXY.X / 100.0, AnchorXY.Y / 100.0,
			       TileCoverageUU.Min.X / 100.0, TileCoverageUU.Max.X / 100.0,
			       TileCoverageUU.Min.Y / 100.0, TileCoverageUU.Max.Y / 100.0,
			       CentreUU.X / 100.0, CentreUU.Y / 100.0);
		}
		else
		{
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("Voxel tile grid: anchor (%.0f, %.0f) m is INSIDE the loaded tile coverage -- "
			            "this is real generated terrain."),
			       AnchorXY.X / 100.0, AnchorXY.Y / 100.0);
		}
	}

	// ADR-0006 G3. Only logged when the pool is actually in use, so the
	// existing log shape is untouched on the shipping path. Free vs largest
	// run is the fragmentation signal: they diverge long before allocations
	// start failing, which is the whole point of watching them.
	if (const UVoxelGpuPoolComponent* Pool = GpuPool.Get())
	{
		// allocFail is on this line rather than only in a warning because it is
		// the one number that invalidates every other number on it: a non-zero
		// count means liveChunks/highWater describe a WORLD WITH HOLES IN IT, and
		// a smaller, cheaper, faster-settling run is exactly what that looks like
		// from the outside. capacityPct is beside it so the approach to the cliff
		// is visible before the cliff.
		const uint32 CapacityQuads = Pool->GetHighWaterMarkQuads() + Pool->GetFreeQuads();
		// S0-2: allocsEver tests §2.2 -- Allocations is append-only by default
		// (see GetNumAllocationsEver), so this is chunks-ever-added, not resident
		// (liveChunks is resident). Watch it against liveChunks over a leg: if
		// it grows while liveChunks plateaus, BuildChunkRuns's per-publication
		// walk is paying an ever-larger tax for no more live geometry.
		//
		// S1-2: freeHandles is GetFreeHandleCount() -- with
		// voxel.Stream.PoolRecycleHandles on, this is the fix in flight: allocsEver
		// should stop climbing (steady state reuses freeHandles instead of
		// appending) once churn saturates the free list.
		{
			// GATHER TIMING. The render thread is the frame (13.49 ms of a 13.41 ms
			// frame, game thread waiting 10 ms of it), so this is the breakdown
			// that decides what to optimise for frame rate.
			const UVoxelGpuPoolComponent::FCullTiming CT =
				UVoxelGpuPoolComponent::GetAndResetCullTiming();
			if (CT.Gathers > 0)
			{
				UE_LOG(LogVoxelPerf, Log,
				       TEXT("Voxel cull timing (5s window): gathers=%d | walk=%.1f ms (%.3f ms/gather, "
				            "%lld runs seen) | emit=%.1f ms (%.3f ms/gather, %d ranges) | walk+emit=%.1f ms"),
				       CT.Gathers, CT.WalkUs / 1000.0, CT.WalkUs / 1000.0 / double(CT.Gathers),
				       (long long)CT.RunsSeen,
				       CT.EmitUs / 1000.0, CT.EmitUs / 1000.0 / double(CT.Gathers), CT.Ranges,
				       (CT.WalkUs + CT.EmitUs) / 1000.0);
			}
		}
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel GPU pool: liveChunks=%d highWater=%u free=%u largestRun=%u freeRuns=%d ")
		       TEXT("capacityPct=%.1f allocFail=%lld allocFailQuads=%lld allocsEver=%d freeHandles=%d -- ")
		       TEXT("ONE primitive, ONE draw"),
		       Pool->GetNumChunks(), Pool->GetHighWaterMarkQuads(), Pool->GetFreeQuads(),
		       Pool->GetLargestFreeRun(), Pool->GetFreeRunCount(),
		       CapacityQuads > 0 ? 100.0 * double(Pool->GetHighWaterMarkQuads()) / double(CapacityQuads) : 0.0,
		       (long long)Pool->GetAllocFailureCount(), (long long)Pool->GetAllocFailureQuads(),
		       Pool->GetNumAllocationsEver(), Pool->GetFreeHandleCount());
	}

	// M2 item 1: "Per-level loaded/pending counters into the perf snapshot/
	// HUD" -- also into this periodic log line, so a headless run's log file
	// alone (no HUD to screenshot) is enough to verify every ring level is
	// actually loading chunks.
	int32 LevelLoaded[VoxelCoords::kNumLevels] = {};
	int32 LevelPending[VoxelCoords::kNumLevels] = {};
	for (const auto& Pair : ChunkRecords)
	{
		if (Pair.Value.HoldsGeometry())
		{
			++LevelLoaded[FMath::Clamp(Pair.Key.Level, 0, VoxelCoords::kNumLevels - 1)];
		}
	}
	for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
	{
		LevelPending[Level] += PendingJobKeysByLevel[Level].Num();
	}
	for (const VoxelCoords::FVoxelLevelChunkKey& K : PendingGameThreadKeys)
	{
		++LevelPending[FMath::Clamp(K.Level, 0, VoxelCoords::kNumLevels - 1)];
	}
	UE_LOG(LogVoxelPerf, Log, TEXT("Voxel rings: %s"), *JoinPerLevel([&](int32 L)
	       { return FString::Printf(TEXT("R%d loaded=%d pending=%d"), L, LevelLoaded[L], LevelPending[L]); }));

	// R2-R4 ring starvation wave: the dispatch side of the same picture. Read
	// with the line above, "loaded=0 pending=4000 dispatched=0" (starved: no
	// worker ever ran) is now distinguishable from "loaded=0 dispatched=4000
	// zeroQuad=4000" (served, but every chunk was buried rock). disp= is this
	// 5s window, total= and load= are cumulative over the whole run.
	UE_LOG(LogVoxelPerf, Log, TEXT("Voxel ring dispatch: %s"), *JoinPerLevel([&](int32 L)
	       {
		       return FString::Printf(TEXT("R%d disp=%lld total=%lld load=%lld zq=%lld"), L,
		                              (long long)LevelJobsDispatchedSinceLog[L], (long long)LevelJobsDispatchedTotal[L],
		                              (long long)LevelChunksLoadedTotal[L], (long long)LevelZeroQuadTotal[L]);
	       }));

	// Sky-band skip: chunks proven all-air and never dispatched, per ring. Read
	// against the zq= counts on the line above -- skip= is work that no longer
	// happens at all, zq= is work that still happened and produced nothing.
	UE_LOG(LogVoxelPerf, Log, TEXT("Voxel sky skip: %s%s"),
	       *JoinPerLevel([&](int32 L) { return FString::Printf(TEXT("R%d skip=%lld"), L, (long long)LevelSkySkippedTotal[L]); }),
	       VoxelSkyBand::GetVerifyEnabled() ? TEXT(" [VERIFY MODE: verdicts computed, jobs dispatched anyway]") : TEXT(""));
	if (VoxelSkyBand::GetVerifyEnabled())
	{
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("VoxelVerifySkyBand: results checked=%lld predicted-all-air=%lld VIOLATIONS=%lld | %s (predicted/checked)"),
		       (long long)VerifyChunksChecked, (long long)VerifyAirPredictions, (long long)VerifyAirViolations,
		       *JoinPerLevel([&](int32 L)
		       {
			       return FString::Printf(TEXT("R%d %lld/%lld"), L, (long long)VerifyAirPredictionsByLevel[L],
			                              (long long)VerifyResultsByLevel[L]);
		       }));
	}
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
	UE_LOG(LogVoxelPerf, Log, TEXT("Voxel worker ms/level: %s | mipCache bricks=%lld bytes=%lld evictions=%lld"),
	       *JoinPerLevel([&](int32 L)
	       { return FString::Printf(TEXT("R%d p50=%.2f p95=%.2f"), L, Snap.LevelWorkerMsP50[L], Snap.LevelWorkerMsP95[L]); }),
	       (long long)Snap.MipCacheBrickCount, (long long)Snap.MipCacheBytes, (long long)Snap.MipCacheEvictions);

	// -VoxelL0GridVerify running total: level-0 chunks meshed BOTH ways and
	// compared quad-for-quad. Any level-0 storage change is only legitimate if
	// MISMATCHES stays 0 -- see VoxelStreamAdmission::L0GridVerifyEnabled.
	if (VoxelStreamAdmission::L0GridVerifyEnabled())
	{
		UE_LOG(LogVoxelPerf, Log, TEXT("VoxelL0GridVerify: level-0 chunks checked=%lld MISMATCHES=%lld"),
		       (long long)VoxelStreamAdmission::GL0GridVerifyChecked.load(std::memory_order_relaxed),
		       (long long)VoxelStreamAdmission::GL0GridVerifyMismatches.load(std::memory_order_relaxed));
	}

	// -VoxelL0GridCacheProbe: what a cross-job level-0 column-grid cache WOULD
	// have hit, per 5s window. `distinct` is the ceiling (an infinite cache can
	// never do better than one build per distinct footprint); `hit%` is what the
	// probed capacity actually reaches. Multiply hit% by the grid's share of
	// level-0 job time -- the `grid N% of job` figure on the R0 census line
	// below -- to get the honest upper bound on what the cache is worth.
	if (const int32 ProbeCap = VoxelStreamAdmission::L0GridCacheProbeEntries(); ProbeCap > 0)
	{
		const int64 Probes = L0ProbeHitsSinceLog + L0ProbeMissesSinceLog;
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel L0 grid-cache probe (5s window): cap=%d dispatches=%lld distinct=%d hits=%lld (%.1f%%) ")
		       TEXT("misses=%lld coldMisses=%lld | ceiling=%.1f%%"),
		       ProbeCap, (long long)Probes, L0ProbeDistinct.Num(), (long long)L0ProbeHitsSinceLog,
		       Probes > 0 ? 100.0 * double(L0ProbeHitsSinceLog) / double(Probes) : 0.0,
		       (long long)L0ProbeMissesSinceLog, (long long)L0ProbeColdMissesSinceLog,
		       Probes > 0 ? 100.0 * double(Probes - L0ProbeDistinct.Num()) / double(Probes) : 0.0);
		L0ProbeHitsSinceLog = L0ProbeMissesSinceLog = L0ProbeColdMissesSinceLog = 0;
		L0ProbeDistinct.Reset();
	}

	// -VoxelCoarseGridVerify running total: coarse chunks meshed BOTH ways and
	// compared quad-for-quad. The flat coarse grid is only a legitimate change
	// if MISMATCHES stays 0 -- see VoxelStreamAdmission::CoarseGridVerifyEnabled.
	if (VoxelStreamAdmission::CoarseGridVerifyEnabled())
	{
		UE_LOG(LogVoxelPerf, Log, TEXT("VoxelCoarseGridVerify: coarse chunks checked=%lld MISMATCHES=%lld"),
		       (long long)VoxelStreamAdmission::GCoarseGridVerifyChecked.load(std::memory_order_relaxed),
		       (long long)VoxelStreamAdmission::GCoarseGridVerifyMismatches.load(std::memory_order_relaxed));
	}

	// M1 steady-state-hitch wave: worst RecomputeDesiredSet cost seen since the
	// last periodic log, per level. Independent of the hitch log (a burst that
	// lands on a frame under the 33.3ms threshold is invisible there), so this
	// is what quantifies "what does R3/R4 recompute cost when it fires".
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel recompute (max since last log): totalMs=%.2f exitScanMs=%.2f sortMs=%.2f calls=%d | ")
	       TEXT("entryMs %s | scans %s | ")
	       TEXT("tracked=%d pendingJob=%d pendingGT=%d pendingUnload=%d | framesOver16.6ms=%d (total %lld)"),
	       MaxRecomputeMs, MaxExitScanMs, MaxSortMs, RecomputeCalls,
	       *JoinPerLevel([&](int32 L) { return FString::Printf(TEXT("R%d=%.2f"), L, MaxLevelEntryMs[L]); }),
	       *JoinPerLevel([&](int32 L) { return FString::Printf(TEXT("R%d=%d"), L, LevelEntryScans[L]); }),
	       ChunkRecords.Num(), PendingJobNum(), PendingGameThreadKeys.Num(), PendingUnloadKeys.Num(),
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
	       TEXT("apply=%.1f remesh=%.1f unload=%.1f | specEnum=%.1f specDispatch=%.1f specPark=%.1f (of dispatch) ")
	       TEXT("| perTick tick=%.3f recompute=%.3f"),
	       AccumTicks, AccumTickMs, WindowMs > 0.0 ? 100.0 * AccumTickMs / WindowMs : 0.0, AccumRecomputeMs, AccumDispatchMs,
	       AccumApplyMs, AccumRemeshMs, AccumUnloadMs, AccumSpecEnumerateMs, AccumSpecDispatchMs, AccumSpecParkMs,
	       AccumTicks > 0 ? AccumTickMs / AccumTicks : 0.0,
	       AccumTicks > 0 ? AccumRecomputeMs / AccumTicks : 0.0);
	// S0-2: apply throughput for THIS window, alongside the leg-long mean
	// TotalChunksLoaded already gives on the "Voxel streaming" line above.
	// §2.2's testable prediction is that this decays monotonically across a
	// leg as Allocations.Num() (see GetNumAllocationsEver) grows -- this is
	// the number that decay would show up in. Divides by the actual elapsed
	// window (ThisLogWindowSeconds), not the nominal LogIntervalSeconds, so a
	// run with -VoxelPerfLogInterval= set to something other than 5s, or a
	// window that overran by a frame, still reports the true rate.
	const int64 ChunksLoadedThisWindow = TotalChunksLoaded - ChunksLoadedAtLastLog;
	const double ChunksPerSecThisWindow =
		ThisLogWindowSeconds > 0.f ? double(ChunksLoadedThisWindow) / double(ThisLogWindowSeconds) : 0.0;
	ChunksLoadedAtLastLog = TotalChunksLoaded;
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel job flow (5s window): dispatched=%lld drained=%lld stale=%lld (%.1f%%) zeroQuad=%lld ")
	       TEXT("recordsAdded=%lld recordsEvicted=%lld candidatesRejected=%lld chunksPerSec=%.1f"),
	       (long long)JobsDispatchedSinceLog, (long long)ResultsDrainedSinceLog, (long long)StaleDiscardsSinceLog,
	       ResultsDrainedSinceLog > 0 ? 100.0 * double(StaleDiscardsSinceLog) / double(ResultsDrainedSinceLog) : 0.0,
	       (long long)ZeroQuadAppliesSinceLog, (long long)RecordsAddedSinceLog, (long long)RecordsEvictedSinceLog,
	       (long long)CandidatesRejectedSinceLog, ChunksPerSecThisWindow);
	// T4-1 speculation census. specHitRate is reported but is NOT the deciding
	// number -- parking scored 89% and still looked like a loss until the
	// throughput metric was fixed to count adopted chunks. The decision is
	// placed-chunks/s and holes; this says whether the CONE was aimed correctly,
	// which is a different question.
	if (VoxelDebug::GetStreamVelocityLeadSec() > 0.f || SpecDispatchedTotal > 0)
	{
		// NOT SpeculativeInFlight.Num(). That set is "enumerated and not yet
		// resolved" -- it includes everything still sitting in SpeculativeKeys
		// waiting to be dispatched, so it read 153 on the first T4-1 leg while
		// the true outstanding count was capped at 32 and the GPU was running 63.
		// It was briefly reported as "the GPU is running 155 concurrent jobs",
		// which it was not. Count the jobs that are actually outstanding.
		int32 SpecOutstandingNow = 0;
		for (const auto& Pair : GpuJobsPending)
		{
			if (Pair.Value.bSpeculative)
			{
				++SpecOutstandingNow;
			}
		}
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel speculation (5s window): dispatched=%lld parked=%lld adopted=%lld evictedUnused=%lld ")
		       TEXT("| bandSkipped=%lld dropOvertaken=%lld dropEmpty=%lld (top=%lld mid=%lld bot=%lld) dropPoolFull=%lld ")
		       TEXT("| queued=%d tracked=%d gpuInFlight=%d parkedNow=%d leadSec=%.2f speed=%.1f m/s ")
		       TEXT("| cumulative dispatched=%lld adopted=%lld (hit %.0f%%)"),
		       (long long)SpecDispatchedSinceLog, (long long)SpecParkedSinceLog,
		       (long long)SpecAdoptedSinceLog, (long long)SpecEvictedUnusedSinceLog,
		       (long long)SpecBandSkippedSinceLog,
		       (long long)SpecDroppedOvertakenSinceLog, (long long)SpecDroppedEmptySinceLog,
		       (long long)SpecEmptyTopSinceLog, (long long)SpecEmptyMidSinceLog,
		       (long long)SpecEmptyBotSinceLog, (long long)SpecDroppedPoolFullSinceLog,
		       SpeculativeKeys.Num(), SpeculativeInFlight.Num(), SpecOutstandingNow, SpecParkedNow,
		       VoxelDebug::GetStreamVelocityLeadSec(), SmoothedAnchorSpeedUUPerSec / 100.0,
		       (long long)SpecDispatchedTotal, (long long)SpecAdoptedTotal,
		       SpecParkedTotal > 0 ? 100.0 * double(SpecAdoptedTotal) / double(SpecParkedTotal) : 0.0);
	}

	// S2-3 park census. adopted/parked is the hit rate: a park that is never
	// adopted was work kept warm for nothing, and it held a pool range and a
	// chunk-table entry the whole time. This is the same number T4-1's
	// specHitRate will be judged on, measured here first on demand traffic.
	//
	// THE ADOPTED FIGURE HERE IS DEMAND-ONLY, and it has to be. ChunksAdoptedTotal
	// counts EVERY adoption -- a speculatively parked chunk being picked up runs
	// the same AddCandidate branch -- while ChunksParkedTotal counts only DEMAND
	// parks (speculation's go to SpecParkedTotal). Dividing the first by the
	// second mixes denominators and printed "hit 102%" on the 2026-07-28 capacity
	// legs, which is the tell: a hit rate above 100% is not a close call, it is a
	// ratio between two different populations. Subtracting SpecAdoptedTotal makes
	// both sides demand.
	//
	// Fifth instance of this failure in this programme (see
	// docs/lessons-2026-07-27-s0-s1.md, appendix). It stayed invisible until
	// speculation raised the numerator enough to break 100%.
	if (VoxelDebug::GetStreamPoolParkMax() > 0 || ChunksParkedTotal > 0)
	{
		const UVoxelGpuPoolComponent* Pool = GpuPool.Get();
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel park (5s window): parked=%lld adopted=%lld evictedStale=%lld evictedCap=%lld | ")
		       TEXT("refused: noGeom=%lld unsettled=%lld notFiner=%lld edited=%lld | ")
		       TEXT("held=%d poolParked=%d | cumulative parked=%lld adopted=%lld (hit %.0f%%)"),
		       (long long)ChunksParkedSinceLog, (long long)ChunksAdoptedSinceLog,
		       (long long)ParkEvictedStaleSinceLog, (long long)ParkEvictedCapSinceLog,
		       (long long)ParkRefusedNoPoolGeomSinceLog, (long long)ParkRefusedUnsettledSinceLog,
		       (long long)ParkRefusedNotFinerSinceLog, (long long)ParkRefusedEditedSinceLog,
		       ParkedGeometry.Num(), Pool ? Pool->GetNumParkedChunks() : -1,
		       (long long)ChunksParkedTotal, (long long)(ChunksAdoptedTotal - SpecAdoptedTotal),
		       ChunksParkedTotal > 0
		           ? 100.0 * double(ChunksAdoptedTotal - SpecAdoptedTotal) / double(ChunksParkedTotal)
		           : 0.0);
	}

	// --- FRAME ATTRIBUTION REPORT ------------------------------------------
	//
	// Answers two questions the percentile summary cannot:
	//   1. WHAT IS IN A TYPICAL FRAME -- the ~8.7 ms that a frame costs before
	//      any terrain geometry is drawn. Fitting p50 against drawn quads across
	//      the draw-path sweep gives T ~= 8.7 ms + 0.66 ms per million quads, and
	//      nobody has ever broken that constant down.
	//   2. WHAT MAKES A SLOW FRAME SLOW. Comparing the mean of each component
	//      over the FASTEST half against the SLOWEST 5% names the difference
	//      instead of inferring it. The component with the largest delta is the
	//      tail.
	//
	// Sorted copies, not in-place: FrameSamples must stay in capture order in
	// case a future reader wants the time series rather than the distribution.
	if (VoxelDebug::GetStreamFrameAttribution() != 0 && FrameSamples.Num() >= 200)
	{
		TArray<float> Frames;
		Frames.Reserve(FrameSamples.Num());
		for (const FFrameSample& F : FrameSamples) { Frames.Add(F.FrameMs); }
		Frames.Sort();
		const float P50 = Frames[Frames.Num() / 2];
		const float P95 = Frames[int32(Frames.Num() * 0.95f)];

		// Buckets by FRAME time, then average every component within each --
		// that is what makes the deltas attributable.
		struct FAccum
		{
			double Tick = 0, Render = 0, RenderWait = 0, RHI = 0, GameWait = 0, Frame = 0;
			int32 N = 0;
			void Add(const FFrameSample& F)
			{
				Tick += F.VoxelTickMs; Render += F.RenderMs; RenderWait += F.RenderWaitMs;
				RHI += F.RHIMs; GameWait += F.GameWaitMs; Frame += F.FrameMs; ++N;
			}
			double M(double V) const { return N > 0 ? V / double(N) : 0.0; }
		};
		FAccum Fast, Slow;
		for (const FFrameSample& F : FrameSamples)
		{
			if (F.FrameMs <= P50) { Fast.Add(F); }
			else if (F.FrameMs >= P95) { Slow.Add(F); }
		}

		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel frame attribution (%d frames): p50=%.2f p95=%.2f | "
		            "FAST(n=%d) frame=%.2f tick=%.2f render=%.2f renderWait=%.2f rhi=%.2f gameWait=%.2f"),
		       FrameSamples.Num(), P50, P95, Fast.N, Fast.M(Fast.Frame), Fast.M(Fast.Tick),
		       Fast.M(Fast.Render), Fast.M(Fast.RenderWait), Fast.M(Fast.RHI), Fast.M(Fast.GameWait));
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel frame attribution SLOW(n=%d) frame=%.2f tick=%.2f render=%.2f renderWait=%.2f "
		            "rhi=%.2f gameWait=%.2f"),
		       Slow.N, Slow.M(Slow.Frame), Slow.M(Slow.Tick), Slow.M(Slow.Render),
		       Slow.M(Slow.RenderWait), Slow.M(Slow.RHI), Slow.M(Slow.GameWait));
		// The deciding line: which component actually differs. A component whose
		// delta is a small fraction of the frame delta is NOT the tail, however
		// large it is in absolute terms -- that mistake has already been made
		// once here with the streaming tick.
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel frame attribution DELTA (slow-fast): frame=%+.2f | tick=%+.2f render=%+.2f "
		            "renderWait=%+.2f rhi=%+.2f gameWait=%+.2f"),
		       Slow.M(Slow.Frame) - Fast.M(Fast.Frame), Slow.M(Slow.Tick) - Fast.M(Fast.Tick),
		       Slow.M(Slow.Render) - Fast.M(Fast.Render),
		       Slow.M(Slow.RenderWait) - Fast.M(Fast.RenderWait),
		       Slow.M(Slow.RHI) - Fast.M(Fast.RHI), Slow.M(Slow.GameWait) - Fast.M(Fast.GameWait));
	}

	UE_LOG(LogVoxelPerf, Log, TEXT("Voxel admission (5s window): cap=%d cutoffM=%.0f rejected=%lld dropped=%lld"),
	       VoxelStreamAdmission::GetPendingJobCap(),
	       WidestAdmissionCutoffM(),
	       (long long)CandidatesRejectedSinceLog, (long long)RecordsDroppedSinceLog);

	// Wave S0 (docs/speculative-generation-plan.md §4, executing T0-1). Two
	// questions in one line, both of which the open P0 currently answers by
	// assumption:
	//
	// EXIT: which of the four DrainResults exits fired. "Apply budget only 8.5%
	// saturated -- results are not ARRIVING" is derived from LastAppliedFrac,
	// which divides by the COUNT ceiling while the loop breaks on a 6 ms WALL
	// CLOCK. wallClock dominating means the loop is exiting on time, not on an
	// empty queue, and the consumer is the wall. Unconditional counters, so this
	// half is readable on any leg. The four should sum to the frame count; short
	// means an exit path nobody has accounted for.
	//
	// STAGES: where per-apply time goes. Gated -- zeroes when
	// voxel.Stream.ApplyStageStats is off, which is the default.
	{
		const double TimedApplies = double(FMath::Max<int64>(1, AppliesTimedSinceLog));
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel apply stages (5s window): exit queueEmpty=%lld wallClock=%lld countCap=%lld drainCap=%lld ")
		       TEXT("| timedApplies=%lld pack=%.2fms params=%.2fms poolAdd=%.2fms ")
		       TEXT("| per-apply pack=%.3f params=%.3f poolAdd=%.3f"),
		       (long long)DrainExitQueueEmptySinceLog, (long long)DrainExitWallClockSinceLog,
		       (long long)DrainExitCountCapSinceLog, (long long)DrainExitDrainCapSinceLog,
		       (long long)AppliesTimedSinceLog,
		       ApplyStagePackMs, ApplyStageParamsMs, ApplyStagePoolAddMs,
		       ApplyStagePackMs / TimedApplies, ApplyStageParamsMs / TimedApplies,
		       ApplyStagePoolAddMs / TimedApplies);
	}

	// The pool's own half of the same question, drained from the component so the
	// render-thread accumulator is reset in step with the game-thread one.
	//
	// pushes vs applies is the ratio the batching wave attacks directly: today
	// this should read roughly applies + unloads, and after T1-1 it should read
	// roughly ticks. allocsWalked/runsEmitted is the OTHER finding -- BuildChunkRuns
	// walks an append-only array, so if allocsWalked climbs across a leg while
	// runsEmitted stays flat, per-apply cost is growing with chunks ever added
	// rather than with residency, and that is a second mechanism behind the
	// plateau nobody has been looking for.
	if (UVoxelGpuPoolComponent* Pool = GpuPool.Get())
	{
		const UVoxelGpuPoolComponent::FPoolPushStats Push = Pool->GetAndResetPushStats();
		const double PerBuild = double(FMath::Max<int64>(1, Push.RunsBuilt));
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel pool publish (5s window): pushes=%lld runsBuilt=%lld allocsWalked=%lld runsEmitted=%lld ")
		       TEXT("(mean walk=%.0f emit=%.0f) | game tableCopy=%.2fms buildRuns=%.2fms ")
		       TEXT("| render runBounds=%.2fms calls=%lld runsWalked=%lld"),
		       (long long)Push.Pushes, (long long)Push.RunsBuilt,
		       (long long)Push.AllocationsWalked, (long long)Push.RunsEmitted,
		       double(Push.AllocationsWalked) / PerBuild, double(Push.RunsEmitted) / PerBuild,
		       Push.TableCopyMs, Push.BuildRunsMs,
		       Push.RunBoundsMs, (long long)Push.RunBoundsCalls, (long long)Push.RunBoundsRunsWalked);

		// S1-1's hazard counter. Cumulative, not per-window, because what matters
		// is "did this EVER fire on this run" -- see UnmarkQuadsDirty. Non-zero
		// proves the same-frame free-then-GPU-write race is real AND handled;
		// zero across a full flight means either it cannot occur or the subtract
		// is not wired, and those two must not be read as the same thing.
		// The gate lives in VoxelEarthShaders (no VoxelDebug dependency), so it is
		// read the same way ApplyMeshResult reads voxel.Stream.GPUMaxChunks.
		// Echoed on the line because "dirtyOverlapsResolved=0" means two entirely
		// different things depending on whether batching was even on.
		static const auto* CVarBatchPublish =
			IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Stream.PoolBatchPublish"));
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel pool batch: batchPublish=%d dirtyOverlapsResolved=%lld (%lld quads, cumulative) ")
		       TEXT("gpuDirectWritesDropped=%d allocFail=%lld"),
		       CVarBatchPublish ? CVarBatchPublish->GetInt() : -1,
		       (long long)Pool->GetDirtyOverlapsResolved(),
		       (long long)Pool->GetDirtyOverlapQuadsResolved(),
		       Pool->GetGpuDirectWritesDropped(),
		       (long long)Pool->GetAllocFailureCount());
	}

	{
		// added / dropped / evicted per ring. A ring whose adds far exceed its
		// residency, with drops and evictions to match, is churning rather than
		// filling -- and which of the two columns is non-zero says WHICH removal
		// path is doing it.
		FString Line;
		for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
		{
			Line += FString::Printf(TEXT("R%d[+%lld -%lld ev%lld] "), Level,
			                        (long long)LevelRecordsAdded[Level],
			                        (long long)LevelRecordsDropped[Level],
			                        (long long)LevelRecordsEvicted[Level]);
		}
		UE_LOG(LogVoxelPerf, Log, TEXT("Voxel records/level (5s window): %s"), *Line);
	}
	{
		// WHICH exit test did the evicting, per ring (see LevelEvictInner). The
		// `ev` column on the line above is the total and cannot separate "the
		// world moved away from this chunk" (out) from "a finer ring claimed this
		// chunk's footprint" (in) -- and only the second one hands the footprint
		// to another ring, i.e. only the second one can leave a gap if that ring
		// is not ready. `vert` is the underground deep box's vertical keep-test,
		// which has no replacement at all and is deliberately never retained.
		//
		// A SEPARATE LINE rather than extra columns on the records/level one:
		// that format is parsed outside this file and must not move.
		//
		// Only levels with something to say are printed, so a settled run's line
		// is short and a churning ring is conspicuous.
		FString Line;
		for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
		{
			if (LevelEvictInner[Level] == 0 && LevelEvictOuter[Level] == 0 && LevelEvictVertical[Level] == 0)
			{
				continue;
			}
			Line += FString::Printf(TEXT("R%d[in=%lld out=%lld vert=%lld] "), Level, (long long)LevelEvictInner[Level],
			                        (long long)LevelEvictOuter[Level], (long long)LevelEvictVertical[Level]);
		}
		UE_LOG(LogVoxelPerf, Log, TEXT("Voxel evictions/level (5s window): %s"),
		       Line.IsEmpty() ? TEXT("(none)") : *Line);
	}
	// Buried-chunk pre-dispatch skip, step 1 census. One line per ring level:
	// results/zeroQuad is the fraction of that ring's worker output that
	// produced no geometry at all, and air/solid/mixed says what those chunks
	// were (only non-zero under -VoxelMeasureEmpty). gridMs/jobMs on R0 is the
	// share of a level-0 job spent building the (32+2)^2 column grid, i.e. the
	// ceiling on what any pre-dispatch skip can reclaim there.
	for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
	{
		if (LevelResultsSinceLog[Level] == 0)
		{
			continue;
		}
		FString GridSuffix;
		if (Level == 0 && AccumLevel0JobMs > 0.0)
		{
			GridSuffix = FString::Printf(TEXT(" | gridMs=%.1f jobMs=%.1f (grid %.1f%% of job)"), AccumLevel0GridMs,
			                             AccumLevel0JobMs, 100.0 * AccumLevel0GridMs / AccumLevel0JobMs);
			// Per-JOB normalisation plus the cycle probe. Read gridGHz first: the
			// grid build is exactly 1156 Amplifier::column calls in every level-0
			// job ever dispatched, so gridKcyc (thousands of retired cycles per
			// job) is a fixed-work number, and gridGHz = gridKcyc / gridUs is the
			// rate the thread actually ran at. If ms/job rises while gridKcyc
			// holds and gridGHz falls, the job was descheduled or downclocked --
			// contention. If gridKcyc rises with it, the work itself got more
			// expensive -- cache/memory pressure. See FJobResult::JobCycles.
			if (AccumLevel0Jobs > 0)
			{
				const double N = double(AccumLevel0Jobs);
				const double GridUsPerJob = 1000.0 * AccumLevel0GridMs / N;
				const double GridKcycPerJob = double(AccumLevel0GridCycles) / N / 1000.0;
				const double JobUs = 1000.0 * AccumLevel0JobMs / N;
				const double JobKcyc = double(AccumLevel0JobCycles) / N / 1000.0;
				GridSuffix += FString::Printf(
					TEXT(" | jobs=%lld perJob jobUs=%.0f jobKcyc=%.0f jobGHz=%.2f | gridUs=%.0f gridKcyc=%.0f gridGHz=%.2f")
					TEXT(" cycPerColumn=%.0f"),
					(long long)AccumLevel0Jobs, JobUs, JobKcyc, JobUs > 0.0 ? JobKcyc / JobUs : 0.0, GridUsPerJob,
					GridKcycPerJob, GridUsPerJob > 0.0 ? GridKcycPerJob / GridUsPerJob : 0.0,
					double(AccumLevel0GridCycles) / N / 1156.0);
				const double TotalBricks = N * double(VoxelCoords::ChunkEdgeBricks * VoxelCoords::ChunkEdgeBricks *
				                                      VoxelCoords::ChunkEdgeBricks);
				GridSuffix += FString::Printf(
					TEXT(" | brickSkip air=%lld solid=%lld of %.0f (%.1f%%)"), (long long)AccumLevel0BricksSkippedAir,
					(long long)AccumLevel0BricksSkippedSolid, TotalBricks,
					TotalBricks > 0.0
						? 100.0 * double(AccumLevel0BricksSkippedAir + AccumLevel0BricksSkippedSolid) / TotalBricks
						: 0.0);
			}
		}
		const double TotalMs = LevelZeroQuadMsSinceLog[Level] + LevelQuadMsSinceLog[Level];
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel empty census R%d (5s window): results=%lld zeroQuad=%lld (%.1f%%) air=%lld solid=%lld mixed=%lld | ")
		       TEXT("workerMs zeroQuad=%.1f quad=%.1f (zeroQuad %.1f%% of worker time)%s"),
		       Level, (long long)LevelResultsSinceLog[Level], (long long)LevelZeroQuadSinceLog[Level],
		       100.0 * double(LevelZeroQuadSinceLog[Level]) / double(LevelResultsSinceLog[Level]),
		       (long long)LevelAllAirSinceLog[Level], (long long)LevelAllSolidSinceLog[Level],
		       (long long)LevelMixedEmptySinceLog[Level], LevelZeroQuadMsSinceLog[Level], LevelQuadMsSinceLog[Level],
		       TotalMs > 0.0 ? 100.0 * LevelZeroQuadMsSinceLog[Level] / TotalMs : 0.0, *GridSuffix);
	}
	BuriedVerifyCheckedTotal += BuriedVerifyCheckedSinceLog;
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel buried skip (5s window): enabled=%d verify=%d skipped=%lld (air=%lld solid=%lld) R0=%lld | ")
	       TEXT("bandCache=%d | verifyChecked=%lld (total %lld) violations=%lld"),
	       VoxelStreamAdmission::BuriedSkipEnabled() ? 1 : 0, VoxelStreamAdmission::VerifyBuriedSkipEnabled() ? 1 : 0,
	       (long long)BuriedSkipsSinceLog, (long long)BuriedSkipAirSinceLog, (long long)BuriedSkipSolidSinceLog,
	       (long long)BuriedSkipsByLevelSinceLog[0], FootprintBandCache.Num(), (long long)BuriedVerifyCheckedSinceLog,
	       (long long)BuriedVerifyCheckedTotal, (long long)BuriedVerifyViolations);
	// markTimeouts keeps its exact former meaning -- CPU-seeded marks only -- so
	// "should be ZERO on a healthy run" still reads true, and the GPU fork cannot
	// quietly turn the project's only signal for a stranded column into noise.
	// gpuLatencyTimeouts is the fork's own bucket: non-zero there means GPU jobs
	// are exceeding kBlindJobMarkTimeoutSeconds, which costs throughput (an extra
	// blind job) and is NOT the bug the backstop exists for. blindGpu is how many
	// of the currently-outstanding marks are GPU-seeded, so a rising
	// gpuLatencyTimeouts can be read against how much of the population is even
	// eligible to produce one.
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel cold-band throttle (5s window): defers=%lld heldLastPass=%d bandCache=%d blindInFlight=%d "
	            "(gpu=%d) markTimeouts=%lld gpuLatencyTimeouts=%lld"),
	       (long long)ColdBandDefersSinceLog, ColdBandHeldThisFrame, FootprintBandCache.Num(),
	       FootprintBlindJobInFlight.Num(), FootprintBlindJobIsGpu.Num(),
	       (long long)ColdBandMarkTimeoutsSinceLog, (long long)ColdBandGpuLatencyTimeoutsSinceLog);
	ColdBandDefersSinceLog = 0;
	ColdBandMarkTimeoutsSinceLog = 0;
	ColdBandGpuLatencyTimeoutsSinceLog = 0;

	// The GPU mesh fork. Printed only when it is on, so an unforked run's log is
	// byte-comparable with every log taken before this wave.
	//
	// failed>0 is the line to watch and the reason this is not just a rate:
	// every non-Success outcome delivers an EMPTY chunk, which on screen is
	// indistinguishable from terrain that is genuinely empty. The fork can
	// therefore be quietly deleting geometry while every aggregate here looks
	// healthy -- fewer quads, faster frames, no errors. Same shape as the pool
	// allocation failures Wave F had to instrument for the same reason.
	if (VoxelStreamAdmission::GpuMeshEnabled())
	{
		const double MeanDeliverMs = GpuMeshJobsDeliveredSinceLog > 0
			? GpuMeshSubmitToDeliverMsSinceLog / double(GpuMeshJobsDeliveredSinceLog)
			: 0.0;
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel GPU mesh fork (5s window): dispatched=%lld delivered=%lld failed=%lld "
		            "pending=%d queued=%d inFlight=%d | submitToDeliver mean=%.1f ms max=%.1f ms "
		            "slow(>=%.0fs)=%lld (total %lld)"),
		       (long long)GpuMeshJobsDispatchedSinceLog, (long long)GpuMeshJobsDeliveredSinceLog,
		       (long long)GpuMeshJobsFailedSinceLog, GpuJobsPending.Num(),
		       GpuMeshJobs.IsValid() ? GpuMeshJobs->NumQueued() : 0,
		       GpuMeshJobs.IsValid() ? GpuMeshJobs->NumInFlight() : 0,
		       MeanDeliverMs, GpuMeshSubmitToDeliverMaxMs,
		       kBlindJobMarkTimeoutSeconds,
		       (long long)GpuMeshSlowDeliveriesSinceLog, (long long)GpuMeshSlowDeliveriesTotal);
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel GPU mesh fork by level (5s window): L0=%lld L1=%lld L2=%lld L3=%lld "
		            "L4=%lld L5=%lld (cap L%d)"),
		       (long long)GpuMeshDispatchedByLevel[0], (long long)GpuMeshDispatchedByLevel[1],
		       (long long)GpuMeshDispatchedByLevel[2], (long long)GpuMeshDispatchedByLevel[3],
		       (long long)GpuMeshDispatchedByLevel[4], (long long)GpuMeshDispatchedByLevel[5],
		       VoxelStreamAdmission::GpuMeshMaxLevel());

		// Stage-0 tile-batching census (see TileCensusSinceLog doc comment):
		// occupancy each proposed 4x4-lattice tile would have had this window,
		// bucketed so the "how many chunks would a real tile dispatch have
		// combined" question can be read off directly, without waiting on the
		// batching implementation to answer it.
		{
			int32 TileDispatches = 0;
			int32 Occ1 = 0, Occ2to3 = 0, Occ4to7 = 0, Occ8to15 = 0, Occ16Plus = 0;
			for (const auto& TilePair : TileCensusSinceLog)
			{
				const int32 Count = FMath::Clamp(TilePair.Value, 1, 16);
				TileDispatches += TilePair.Value;
				if (Count == 1)        { ++Occ1; }
				else if (Count <= 3)   { ++Occ2to3; }
				else if (Count <= 7)   { ++Occ4to7; }
				else if (Count <= 15)  { ++Occ8to15; }
				else                   { ++Occ16Plus; }
			}
			const int32 TileCount = TileCensusSinceLog.Num();
			const double MeanOcc = TileCount > 0 ? double(TileDispatches) / double(TileCount) : 0.0;
			UE_LOG(LogVoxelPerf, Log,
			       TEXT("Voxel tile census (5s window): dispatches=%d tiles=%d meanOcc=%.1f | "
			            "occ 1:%d 2-3:%d 4-7:%d 8-15:%d 16+:%d"),
			       TileDispatches, TileCount, MeanOcc, Occ1, Occ2to3, Occ4to7, Occ8to15, Occ16Plus);
			TileCensusSinceLog.Reset();
		}

		for (int64& N : GpuMeshDispatchedByLevel) { N = 0; }
		GpuMeshJobsDispatchedSinceLog = 0;
		GpuMeshJobsDeliveredSinceLog = 0;
		GpuMeshJobsFailedSinceLog = 0;
		GpuMeshSubmitToDeliverMsSinceLog = 0.0;
		GpuMeshSlowDeliveriesTotal += GpuMeshSlowDeliveriesSinceLog;
		GpuMeshSlowDeliveriesSinceLog = 0;
		// Max is NOT reset: it is a run-high-water mark, and it is the number
		// that says whether kBlindJobMarkTimeoutSeconds (5 s) is anywhere near
		// being crossed. Resetting it every window would hide the one spike
		// that matters.
	}

	// S0-3 (docs/speculative-generation-plan.md Wave S0 / T0-2). Independent of
	// VoxelStreamAdmission::GpuMeshEnabled() above -- the CPU worker arm's
	// windows are worth reading on a CPU-only run too, and a run with the fork
	// on wants both printed together. Gated entirely on voxel.Stream.LatencyStats
	// (VoxelDebug::GetStreamLatencyStats), which is also what gates the window
	// writes themselves (OnGpuMeshJobComplete, the CPU worker task body,
	// DrainResults) -- see that cvar's source comment.
	//
	// CAVEAT on the GPU line below: queued/dispatchToReady/readyToDeliver read
	// as a real 0.0, not "unmeasured", whenever voxel.GPU.MeshLatencyStats (the
	// VoxelEarthShaders-module gate FJob::PromotedSeconds needs -- that module
	// has no VoxelDebug dependency, hence the separate cvar) is off even though
	// this cvar is on. Both must be set for those three columns to mean
	// anything; submitToDeliver and deliverToApply do not depend on the
	// shaders-side gate.
	if (VoxelDebug::GetStreamLatencyStats())
	{
		// SAY SO IN THE LINE, not only in the comment above. Three of the five
		// stage columns are zero rather than absent when the shaders-side gate is
		// off, and "queued/dispatchToReady/readyToDeliver all read 0.0" is a
		// publishable-looking result that would be an artifact of a cvar. This
		// programme has already retracted one set of numbers and corrected two
		// root-cause diagnoses; a reader of the log should not have to know which
		// module owns which gate to avoid being the third.
		//
		// FindConsoleVariable rather than a VoxelDebug accessor because the cvar
		// lives in VoxelEarthShaders, which has no VoxelDebug dependency -- same
		// idiom ApplyMeshResult uses for voxel.Stream.GPUMaxChunks.
		static const auto* CVarMeshLatencyStats =
			IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.GPU.MeshLatencyStats"));
		const bool bGpuStagesMeasured = CVarMeshLatencyStats && CVarMeshLatencyStats->GetInt() != 0;

		const FMsPercentiles GpuQueued = ComputeMsPercentiles(GpuQueuedMsWindow, GpuQueuedMsWindowCount);
		const FMsPercentiles GpuDispatchToReady = ComputeMsPercentiles(GpuDispatchToReadyMsWindow, GpuDispatchToReadyMsWindowCount);
		const FMsPercentiles GpuReadyToDeliver = ComputeMsPercentiles(GpuReadyToDeliverMsWindow, GpuReadyToDeliverMsWindowCount);
		const FMsPercentiles GpuSubmitToDeliver = ComputeMsPercentiles(GpuSubmitToDeliverMsWindow, GpuSubmitToDeliverMsWindowCount);
		const FMsPercentiles GpuDeliverToApply = ComputeMsPercentiles(GpuDeliverToApplyMsWindow, GpuDeliverToApplyMsWindowCount);
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel GPU latency stages (5s window, n=%d)%s: queued p50=%.1f p95=%.1f max=%.1f | ")
		       TEXT("dispatchToReady p50=%.1f p95=%.1f max=%.1f | readyToDeliver p50=%.1f p95=%.1f max=%.1f | ")
		       TEXT("submitToDeliver p50=%.1f p95=%.1f max=%.1f | deliverToApply p50=%.1f p95=%.1f max=%.1f (n=%d)"),
		       GpuSubmitToDeliverMsWindowCount,
		       bGpuStagesMeasured
		           ? TEXT("")
		           : TEXT(" [queued/dispatchToReady/readyToDeliver NOT MEASURED -- set voxel.GPU.MeshLatencyStats 1; "
		                  "the zeros below are the gate, not the pipeline]"),
		       GpuQueued.P50, GpuQueued.P95, GpuQueued.Max,
		       GpuDispatchToReady.P50, GpuDispatchToReady.P95, GpuDispatchToReady.Max,
		       GpuReadyToDeliver.P50, GpuReadyToDeliver.P95, GpuReadyToDeliver.Max,
		       GpuSubmitToDeliver.P50, GpuSubmitToDeliver.P95, GpuSubmitToDeliver.Max,
		       GpuDeliverToApply.P50, GpuDeliverToApply.P95, GpuDeliverToApply.Max,
		       GpuDeliverToApplyMsWindowCount);

		const FMsPercentiles CpuEndToEnd = ComputeMsPercentiles(CpuWorkerEndToEndMsWindow, CpuWorkerEndToEndMsWindowCount);
		const FMsPercentiles CpuDeliverToApply = ComputeMsPercentiles(CpuDeliverToApplyMsWindow, CpuDeliverToApplyMsWindowCount);
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel CPU worker latency (5s window): endToEnd p50=%.1f p95=%.1f max=%.1f (n=%d) | ")
		       TEXT("deliverToApply p50=%.1f p95=%.1f max=%.1f (n=%d)"),
		       CpuEndToEnd.P50, CpuEndToEnd.P95, CpuEndToEnd.Max, CpuWorkerEndToEndMsWindowCount,
		       CpuDeliverToApply.P50, CpuDeliverToApply.P95, CpuDeliverToApply.Max, CpuDeliverToApplyMsWindowCount);

		// Per-level (0..5) quad-count-per-delivered-chunk distribution -- the
		// input a speculative pool reserve is sized from (T4-1 needs this
		// before it can be costed at all). Buckets chosen against a measured
		// mean of ~902 quads/chunk (35,205,733 quads over 39,020 chunks) and
		// the hard per-chunk bound of 98,304 quads: 0 isolates the buried/
		// all-air/all-solid population the buried-skip census elsewhere in
		// this function already shows is a large fraction of deliveries, and
		// 1-255/256-1023/1024-4095/4096-16383 each roughly quadruple the last
		// so the mean lands inside the distribution's body rather than in the
		// first or last bucket, leaving the tail bucket (16384+) genuine
		// headroom below the hard bound instead of being where every dense
		// surface chunk piles up.
		for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
		{
			const int64 LevelChunks = LevelQuadCountSinceLog[Level];
			if (LevelChunks == 0)
			{
				continue;
			}
			const double MeanQuads = double(LevelQuadSumSinceLog[Level]) / double(LevelChunks);
			const int64* Hist = LevelQuadHistSinceLog[Level];
			UE_LOG(LogVoxelPerf, Log,
			       TEXT("Voxel quad census L%d (5s window): chunks=%lld mean=%.1f | ")
			       TEXT("0:%lld 1-255:%lld 256-1023:%lld 1024-4095:%lld 4096-16383:%lld 16384+:%lld"),
			       Level, (long long)LevelChunks, MeanQuads,
			       (long long)Hist[0], (long long)Hist[1], (long long)Hist[2],
			       (long long)Hist[3], (long long)Hist[4], (long long)Hist[5]);
		}

		for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
		{
			LevelQuadCountSinceLog[Level] = 0;
			LevelQuadSumSinceLog[Level] = 0;
			for (int32 Bucket = 0; Bucket < kNumQuadHistBuckets; ++Bucket)
			{
				LevelQuadHistSinceLog[Level][Bucket] = 0;
			}
		}
	}

	BuriedSkipsSinceLog = BuriedSkipAirSinceLog = BuriedSkipSolidSinceLog = BuriedVerifyCheckedSinceLog = 0;

	// Load-before-unload retention census. ONE headline number now:
	//
	//   capRel       -- stand-ins parked because voxel.Stream.LodRetentionMs ran
	//                   out before the replacement arrived. Every one of those is
	//                   a hole the player could have seen, and the cause is
	//                   THROUGHPUT. The SHIPPED default is 10000 (retuned
	//                   5000 -> 20000 -> 10000 on 2026-07-27, see
	//                   CVarVoxelStreamLodRetentionMs). The ZERO flight-phase
	//                   capRel measured at 20 m/s (2026-07-27) was taken at
	//                   LodRetentionMs=20000, NOT at the shipped 10000 default --
	//                   it is not a prediction for the shipped setting, and a
	//                   leg run at the default has not yet re-measured this.
	//
	// covRelAbsent WAS the second headline, back when ReplacementCovered treated
	// every absent replacement record as coverage and the counter could not tell
	// a sound absence from a ring-gap hole. Since the 2026-07-27 fix an
	// absent-but-DESIRED replacement blocks the release instead, so that
	// population shows up in `held` and covRelAbsent counts only absences proven
	// legitimate. It is now a sanity number, not a suspicion: it should be
	// non-trivial (footprint Z trims and ring annuli genuinely decline chunks)
	// and it no longer implies anything about holes.
	//
	// covRelSettled is the sound release and needs no interpretation. See the
	// counter block in DrainUnloads and ReplacementCovered's doc comment.
	{
		const int64 TotalReleases =
			RetainCoveredSettledReleasesSinceLog + RetainCoveredAbsentReleasesSinceLog + RetainCapReleasesSinceLog;
		// Per-level breakouts appended only when non-zero, so a healthy run's
		// line stays one line and a suspicious one names the ring immediately.
		FString AbsentPerLevel;
		FString CapPerLevel;
		for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
		{
			if (LevelRetainCoveredAbsent[Level] > 0)
			{
				AbsentPerLevel += FString::Printf(TEXT("R%d=%lld "), Level, (long long)LevelRetainCoveredAbsent[Level]);
			}
			if (LevelRetainCapReleases[Level] > 0)
			{
				CapPerLevel += FString::Printf(TEXT("R%d=%lld "), Level, (long long)LevelRetainCapReleases[Level]);
			}
		}
		FString Suffix;
		if (!AbsentPerLevel.IsEmpty())
		{
			Suffix += FString::Printf(TEXT(" | absent/level: %s"), *AbsentPerLevel);
		}
		if (!CapPerLevel.IsEmpty())
		{
			Suffix += FString::Printf(TEXT(" | cap/level: %s"), *CapPerLevel);
		}
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Voxel LOD retention (5s window): held=%d covRelSettled=%lld covRelAbsent=%lld capRel=%lld ")
		       TEXT("resurrected=%lld (capRel %.1f%% of releases, covRelAbsent %.1f%%) | retentionMs=%.0f%s"),
		       RetainHeldThisFrame, (long long)RetainCoveredSettledReleasesSinceLog,
		       (long long)RetainCoveredAbsentReleasesSinceLog, (long long)RetainCapReleasesSinceLog,
		       (long long)ResurrectionsSinceLog,
		       TotalReleases > 0 ? 100.0 * double(RetainCapReleasesSinceLog) / double(TotalReleases) : 0.0,
		       TotalReleases > 0 ? 100.0 * double(RetainCoveredAbsentReleasesSinceLog) / double(TotalReleases) : 0.0,
		       VoxelDebug::GetStreamLodRetentionMs(), *Suffix);
	}
	RetainCoveredSettledReleasesSinceLog = RetainCoveredAbsentReleasesSinceLog = RetainCapReleasesSinceLog = 0;
	ResurrectionsSinceLog = 0;
	FMemory::Memzero(LevelRetainCoveredAbsent);
	FMemory::Memzero(LevelRetainCapReleases);

	// The counters above say how retention BEHAVED; this says whether the world
	// currently has holes in it. Deliberately adjacent: covRelAbsent is a
	// suspicion and holes>0 is the confirmation, and reading them from the same
	// log window is the whole point. Default-off, one cvar read when off.
	if (VoxelDebug::GetStreamCoverageVerify())
	{
		LogCoverageVerify();
	}

	// All-solid ADMISSION skip census. Deliberately its own line rather than
	// folded into the buried-skip one above: they measure different things and
	// confusing them would hide the point. That line counts chunks that were
	// TRACKED and then not dispatched; this one counts chunks that never became
	// records -- the difference the follow-up was about.
	SolidSkippedAtAdmissionTotal += SolidSkippedAtAdmissionSinceLog;
	SolidVerifyCheckedTotal += SolidVerifyCheckedSinceLog;
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel solid skip at admission (5s window): enabled=%d verify=%d skipped=%lld (total %lld) | ")
	       TEXT("R0=%lld R1=%lld | floorCache=%d | verifyChecked=%lld (total %lld) VIOLATIONS=%lld"),
	       VoxelStreamAdmission::SolidSkipEnabled() ? 1 : 0, VoxelStreamAdmission::VerifySolidSkipEnabled() ? 1 : 0,
	       (long long)SolidSkippedAtAdmissionSinceLog, (long long)SolidSkippedAtAdmissionTotal,
	       (long long)LevelSolidSkippedAtAdmission[0], (long long)LevelSolidSkippedAtAdmission[1],
	       FootprintSolidFloorCache.Num(), (long long)SolidVerifyCheckedSinceLog,
	       (long long)SolidVerifyCheckedTotal, (long long)SolidVerifyViolations);
	SolidSkippedAtAdmissionSinceLog = 0;
	SolidVerifyCheckedSinceLog = 0;
	for (int64& N : LevelSolidSkippedAtAdmission)
	{
		N = 0;
	}

	// Buried-BAND admission skip census. `cold` is the number to read first: it
	// is level-0 surface-band footprint scans that had no cached band to
	// consult, i.e. the part of the opportunity an admission-time band skip can
	// never reach however well it is implemented. `editFloor` is the downward
	// escape hatch firing (footprint scans whose ChunkZMin was widened by a dig
	// below the worldgen floor) and is expected to be 0 on any run with no edits.
	BandSkippedAtAdmissionTotal += BandSkippedAtAdmissionSinceLog;
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel band skip at admission (5s window): mode=%d warm=%lld cold=%lld skipped=%lld (total %lld) ")
	       TEXT("editVeto=%lld | editFloor widened=%lld deepest=%d chunks forcedRescans=%lld"),
	       VoxelStreamAdmission::AdmissionBandSkipMode(), (long long)BandAdmitWarmSinceLog,
	       (long long)BandAdmitColdSinceLog, (long long)BandSkippedAtAdmissionSinceLog,
	       (long long)BandSkippedAtAdmissionTotal, (long long)BandAdmitEditVetoSinceLog,
	       (long long)EditFloorWidenedSinceLog, EditFloorWidestChunks, (long long)EditForcedRescansSinceLog);
	BandAdmitWarmSinceLog = BandAdmitColdSinceLog = BandSkippedAtAdmissionSinceLog = 0;
	BandAdmitEditVetoSinceLog = EditFloorWidenedSinceLog = EditForcedRescansSinceLog = 0;
	for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
	{
		BuriedSkipsByLevelSinceLog[Level] = 0;
	}

	for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
	{
		LevelResultsSinceLog[Level] = LevelZeroQuadSinceLog[Level] = LevelAllAirSinceLog[Level] = 0;
		LevelAllSolidSinceLog[Level] = LevelMixedEmptySinceLog[Level] = 0;
		LevelZeroQuadMsSinceLog[Level] = LevelQuadMsSinceLog[Level] = 0.0;
	}
	AccumLevel0GridMs = AccumLevel0JobMs = 0.0;
	AccumLevel0GridCycles = AccumLevel0JobCycles = 0;
	AccumLevel0Jobs = 0;
	AccumLevel0BricksSkippedAir = AccumLevel0BricksSkippedSolid = 0;

	AccumDispatchMs = AccumApplyMs = AccumRemeshMs = AccumUnloadMs = AccumRecomputeMs = AccumTickMs = 0.0;
	AccumSpecDispatchMs = AccumSpecEnumerateMs = AccumSpecParkMs = 0.0;
	AccumTicks = 0;
	JobsDispatchedSinceLog = ResultsDrainedSinceLog = StaleDiscardsSinceLog = ZeroQuadAppliesSinceLog = 0;
	RecordsAddedSinceLog = RecordsEvictedSinceLog = CandidatesRejectedSinceLog = RecordsDroppedSinceLog = 0;
	// Wave S0. The pool's own push stats are drained and reset by
	// GetAndResetPushStats where the line is emitted, not here -- they live on the
	// component, and resetting them from two places is how the render-thread half
	// would start double-counting.
	SpecDispatchedSinceLog = SpecParkedSinceLog = SpecAdoptedSinceLog = SpecEvictedUnusedSinceLog = 0;
	SpecDroppedEmptySinceLog = SpecDroppedOvertakenSinceLog = SpecDroppedPoolFullSinceLog = 0;
	SpecEmptyTopSinceLog = SpecEmptyMidSinceLog = SpecEmptyBotSinceLog = SpecBandSkippedSinceLog = 0;
	ParkRefusedNoPoolGeomSinceLog = ParkRefusedUnsettledSinceLog = 0;
	ParkRefusedNotFinerSinceLog = ParkRefusedEditedSinceLog = 0;
	ChunksParkedSinceLog = ChunksAdoptedSinceLog = 0;
	ParkEvictedStaleSinceLog = ParkEvictedCapSinceLog = 0;
	DrainExitQueueEmptySinceLog = DrainExitWallClockSinceLog = 0;
	DrainExitCountCapSinceLog = DrainExitDrainCapSinceLog = 0;
	ApplyStagePackMs = ApplyStageParamsMs = ApplyStagePoolAddMs = 0.0;
	AppliesTimedSinceLog = 0;
	FMemory::Memzero(LevelRecordsAdded);
	FMemory::Memzero(LevelRecordsDropped);
	FMemory::Memzero(LevelRecordsEvicted);
	FMemory::Memzero(LevelEvictInner);
	FMemory::Memzero(LevelEvictOuter);
	FMemory::Memzero(LevelEvictVertical);

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

// (2) Underground deep residency: vertical RADIUS in level-L chunks around
// the anchor's chunk Z. Radius 0 still means one chunk layer (the anchor's
// own).
//
// SHAPE: A SIGHT SPHERE, NOT A BANDED BOX.
//
// The shipped shape was a three-band step function of horizontal distance,
// {8, 4, 0} level-0 chunks over the <0.25 / <0.5 / >=0.5 fractions of ring
// 0's 64 m outer radius -- i.e. +-25.6 m of rock within 16 m of you, +-12.8 m
// out to 32 m, and a single 3.2 m layer beyond that. Two problems, both
// visible the moment you stand in a natural cavern:
//
//   - A cavern is BIGGER THAN THE WHOLE RESIDENT VOLUME. voxelcore/caverns.h
//     gives rooms a horizontal semi-axis of kCavernRxyMinMm..MaxMm =
//     12..28 m (24-56 m across) and deep children a vertical semi-axis up to
//     kCavernRzDeepMaxMm = 40 m, chained downward. Standing in one, the far
//     wall is 24-56 m away -- squarely in the old FAR band, where the deep
//     residency was one 3.2 m layer. So the generator built the cavern and
//     the streamer refused to show it: PR #74's veil correctly rendered the
//     missing rock as darkness rather than sky, which is honest but is not a
//     cavern vista.
//   - The step function spends its depth exactly where it is least useful.
//     Underground, "how far can I see" is a RADIUS, not a column height:
//     +-25.6 m of rock directly overhead is 25 m of solid ceiling nobody can
//     see through, while the 40 m sightline to the far wall gets nothing.
//
// So the deep set is now the set of chunks whose centre lies within
// SightRadius of the anchor -- a sphere. The vertical radius at horizontal
// distance d is sqrt(R^2 - d^2), which is naturally ANISOTROPIC in the
// direction that matters: it trades the useless deep column overhead for
// lateral reach, and it tapers to zero exactly at the horizon rather than
// stepping there. It is also a strict SUPERSET of the old three-band box at
// the default radius (see the table below), so nothing that used to be
// resident stops being resident.
//
//   horizontal d | old box radius | sphere radius (R = 40 m)
//   ------------ | -------------- | ------------------------
//   0 m          | 8  (+-25.6 m)  | 12 (+-38.4 m)
//   16 m         | 8  (+-25.6 m)  | 11 (+-35.2 m)
//   32 m         | 4  (+-12.8 m)  | 7  (+-22.4 m)
//   40 m         | 0  (one layer) | 0  (one layer)
//   40-64 m      | 0  (one layer) | 0  (one layer)
//
// COST, and why this is affordable. The naive count says the sphere is ~1.9x
// the old box (~8.2k vs ~4.4k level-0 chunks). The reason that is not ~1.9x
// the WORK is the buried-chunk pre-dispatch skip already in this file: a
// level-0 chunk whose footprint band says the chunk and its apron sit below
// SolidBelowVoxel is all-solid, meshes to zero quads by construction, and is
// never dispatched at all. Underground, that is the overwhelming majority of
// the sphere -- solid rock -- and it is precisely the cave and cavern voids,
// the thing we are trying to render, that the band declines to skip. The
// steady-state census bears this out: ~5,000 of the ~6,600 skips per 5 s
// window are the all-SOLID half. So the sphere buys sightline where there is
// something to see and costs a band lookup where there is not.
//
// This whole path is gated on bAnchorUnderground, so the M1 surface flight --
// which never goes below ground (underground=0, deepTracked=0 in every perf
// run) -- is byte-for-byte unaffected. The cost is paid only while the player
// is actually underground, which is exactly when the pixels are wanted.
//
// R is a cvar so the residency/cost trade can be A/B'd without a rebuild. It
// is safe to drive from -ExecCmds, unlike the master switch above: the deep
// set does not exist until the anchor first goes underground, which on any
// scripted flight is long after cvars have been applied.
// DEFAULT 64 m, which is exactly ring 0's own outer radius -- underground, the
// level-0 ring is fully resident in three dimensions rather than as a slab.
// Chosen by measurement, not taste. In the documented test cavern (a room
// 46.6 m wide and 49.4 m tall, 60.7 m down, camera 2 m off one wall), the
// capture probe reports where the sightline terminates and whether that rock
// is meshed:
//
//   R = 40 m: +0deg wall at 44.9 m MESHED; +10/+20/+30deg walls at 46-51 m
//             tracked but NOT MESHED -- i.e. everything the camera is actually
//             pointed at is a hole, and the screenshot is open sky above a
//             thin band of floor. At 44.6 m lateral an R = 40 sphere has zero
//             vertical radius, so the resident set out there is the single
//             3.2 m layer at the anchor's own Z. A horizontal ray runs down
//             the middle of that band and sees rock the whole way, which is
//             why a less careful probe called this a pass.
//   R = 64 m: all four elevations MESHED. No sky in frame.
//
// Cost, same scene, stationary: deep records 9,350 -> 31,648 and tracked
// 21,112 -> 44,331, for 548 -> 1,238 deep chunks that actually carry geometry
// (3.9% -- the rest is solid rock the buried-chunk skip never dispatches).
// Steady-state cost of that is close to nothing: recompute 0.0 ms (the refill
// trigger stops firing once the ring converges) and subsystem tick 0.27-0.37%
// of wall, against 0.29-0.33% at R = 40.
//
// WHAT IS NOT MEASURED, and it is the reason this is a cvar: MOVING
// underground. The exit scan is O(ChunkRecords) and runs on every recompute,
// so 44 k records is ~2.7x the surface flight's 16 k, where that scan cost
// 2.4-2.7 ms. Nothing here exercises a moving underground anchor. See the
// follow-up on skipping all-solid chunks at ADMISSION rather than at dispatch,
// which would cut these records by ~25x and is the real fix if this bites.
static float GSightRadiusM = 64.0f;
static FAutoConsoleVariableRef CVarUndergroundSightRadius(
	TEXT("voxel.Stream.UndergroundSightM"),
	GSightRadiusM,
	TEXT("Radius in metres of the underground sight sphere: while the anchor is underground, level-0 chunks whose centre is within this distance are resident. 40 (default) covers the far wall of a mid-size cavern from its centre."),
	ECVF_Default);

static double SightRadiusUU()
{
	// Clamped rather than trusted: this multiplies a level-0 chunk count, and
	// a fat-fingered 400 would admit ~8 million chunks. 64 m is ring 0's own
	// outer radius -- beyond it the sphere would want chunks this level does
	// not stream at all.
	return double(FMath::Clamp(GSightRadiusM, 0.0f, 64.0f)) * 100.0;
}

static constexpr int32 BoxRadiusChunksL1 = 0; // whole ring (fraction is always >= 0.5 there)

// Vertical keep-distance for the deep box, in level-L chunks, used by the
// exit scan. Mirrors the XY hysteresis: keep out to KeepChunks * chunkEdge *
// UnloadRingMultiplier. Generous relative to the near-band radius that created
// them (8 -> 9) so that a player moving HORIZONTALLY between bands does not
// thrash chunks in and out; surfacing still evicts the whole box, because the
// anchor's Z leaves the keep window entirely.
// Levels 2+ get no deep box at all (BoxRadiusChunks returns false for them), so
// their entry here is unused and 0 is the correct value for R5 as well.
static constexpr int32 KeepChunks[VoxelCoords::kNumLevels] = {9, 2, 0, 0, 0, 0};
static_assert(UE_ARRAY_COUNT(KeepChunks) == VoxelCoords::kNumLevels, "KeepChunks must have one entry per level");

// Vertical keep-distance in UU for a deep chunk of this level, used by the
// exit scan. Level 0 derives it from the sight sphere rather than from the
// constant above, and that is load-bearing rather than tidy: the keep window
// MUST exceed the largest vertical offset admission can create, or the exit
// scan queues a chunk for unload in the same recompute that admitted it and
// the pair thrash forever. The shipped constant (9 -> 40.0 m of keep) was
// generous against the old radius-8 box (25.6 m) but lands exactly ON the
// default sphere's 12-chunk pole (40.0 m), and would be far under it at a
// raised voxel.Stream.UndergroundSightM. Deriving it from the same radius the
// admission side uses makes the invariant structural: keep = 1.25 * R + one
// chunk of slack is always strictly greater than the R the sphere admits.
static double VerticalKeepUU(int32 Level, double ChunkEdgeUU)
{
	if (Level == 0)
	{
		return SightRadiusUU() * UVoxelWorldSubsystem::UnloadRingMultiplier + ChunkEdgeUU;
	}
	return double(KeepChunks[Level] + 1) * ChunkEdgeUU * UVoxelWorldSubsystem::UnloadRingMultiplier;
}

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
	const double OuterUU = UVoxelWorldSubsystem::GetRingPresets()[Level].OuterMeters * 100.0;
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
		// Sight sphere (see GSightRadiusM): vertical half-extent in level-0
		// chunks at horizontal distance sqrt(DistSq). Outside the sphere the
		// radius is 0, which still means the anchor's own chunk layer -- the
		// same single layer the old far band granted, so the far field is
		// unchanged rather than trimmed.
		const double RadiusUU = SightRadiusUU();
		const double RemainingSq = RadiusUU * RadiusUU - DistSq;
		OutRadius = RemainingSq <= 0.0
		                ? 0
		                : FMath::FloorToInt32(FMath::Sqrt(RemainingSq) / VoxelCoords::ChunkEdgeUUForLevel(0));
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

int64 FVoxelWorldImpl::FootprintSurfaceUpperBoundMm(int32 Level, int32 ChunkX, int32 ChunkY) const
{
	using namespace VoxelCoords;

	// DELEGATES to vxc::Amplifier::surfaceUpperBoundMm rather than reimplementing
	// the bound. This function used to carry its own copy, and that copy went
	// wrong twice, both times silently:
	//
	//   * at worldgen v6 the detail allowance stopped matching. It was derived
	//     from kDetailAmplitudeSumMm = 1800+700+260+100, the v1 octave table,
	//     while the real table became 2600+1100+500+190+60. The bound was
	//     therefore allowing LESS detail than the amplifier can produce.
	//   * at worldgen v9 the base term stopped matching. It computed a BILINEAR
	//     maximum, while the carrier is now a cubic B-spline -- which is not
	//     bounded by the bilinear value, since near a local minimum it sits
	//     above it.
	//
	// Either way the failure mode is the same and it is not a lost optimisation:
	// IsChunkProvablyAllAir feeds the streaming skip, so an under-stated upper
	// bound means a chunk containing terrain is never generated. A hole in the
	// world.
	//
	// The comment on the old constants block predicted this exactly -- "this
	// belongs in voxel-core ... so a change to kDetailOctaves cannot silently
	// invalidate it". Amplifier::surfaceUpperBoundMm is that follow-up, and
	// solidBelowBoundMm below already delegated to its sibling; this copy was
	// simply left behind. Deleting it, rather than re-mirroring it, is the point.
	//
	// Both sides use INT64_MAX to decline (vxc::kSurfaceBoundDeclined), so every
	// caller is unchanged.
	const int64 SpanVox = int64(ChunkEdgeVoxels) * (int64(1) << Level);
	const int64 Vx0 = int64(ChunkX) * SpanVox;
	const int64 Vy0 = int64(ChunkY) * SpanVox;
	return Voxels.amplifier().surfaceUpperBoundMm(Vx0, Vy0, Vx0 + SpanVox - 1, Vy0 + SpanVox - 1);
}

bool FVoxelWorldImpl::IsChunkProvablyAllAir(const VoxelCoords::FVoxelLevelChunkKey& LevelKey) const
{
	using namespace VoxelCoords;

	const int64 BoundMm = FootprintSurfaceUpperBoundMm(LevelKey.Level, LevelKey.Key.X, LevelKey.Key.Y);
	if (BoundMm == INT64_MAX)
	{
		return false; // declined to bound: never claim air on no information
	}

	// Lowest level-0 voxel INDEX this chunk covers. A level-L cell is solid
	// only if some level-0 voxel under it is solid (downsampleBricks takes a
	// threshold over its children and returns "no brick" for an all-air group),
	// so proving every level-0 voxel at or above this index is air proves the
	// whole chunk is air at any level.
	const int64 LevelScale = int64(1) << LevelKey.Level;
	const int64 BottomVoxel = (int64(LevelKey.Key.Z) * ChunkEdgeVoxels) * LevelScale;

	// Topmost level-0 voxel that could be solid anywhere in the footprint:
	// a voxel is solid iff its centre (vz*100 + 50 mm) is at or below the
	// surface, so vz <= (surfaceMm - 50)/100. floorDiv rounds toward -inf, i.e.
	// outward on the safe side for an upper bound that may be negative.
	const int64 TopSolidUpperBound =
		vxc::floorDiv(BoundMm - int64(vxc::kVoxelSizeMm) / 2, int64(vxc::kVoxelSizeMm));

	return BottomVoxel > TopSolidUpperBound;
}

int64 FVoxelWorldImpl::FootprintSolidFloorMmCached(int32 Level, int32 ChunkX, int32 ChunkY) const
{
	using namespace VoxelCoords;

	// Z forced to 0: this is a per-FOOTPRINT value, exactly like the z-range
	// memo it sits beside, so every chunk in the column shares one entry.
	const FVoxelLevelChunkKey CacheKey{Level, FVoxelChunkKey{ChunkX, ChunkY, 0}};
	if (const int64* Cached = FootprintSolidFloorCache.Find(CacheKey))
	{
		return *Cached;
	}

	// Footprint in level-0 voxel indices, apron included. The apron matters:
	// the mesher reads one voxel past the interior on every side, and a face is
	// emitted where a solid voxel has an air neighbour -- so a chunk is only
	// quad-free if its APRON is solid too, not just its interior.
	const int64 LevelScale = int64(1) << Level;
	const int64 Span = int64(ChunkEdgeVoxels) * LevelScale;
	const int64 VX0 = int64(ChunkX) * Span - LevelScale;
	const int64 VY0 = int64(ChunkY) * Span - LevelScale;
	const int64 VX1 = VX0 + Span + 2 * LevelScale - 1;
	const int64 VY1 = VY0 + Span + 2 * LevelScale - 1;

	const int64 FloorMm = Voxels.amplifier().solidBelowBoundMm(VX0, VY0, VX1, VY1);
	FootprintSolidFloorCache.Add(CacheKey, FloorMm);
	return FloorMm;
}

bool FVoxelWorldImpl::IsChunkProvablyAllSolid(const VoxelCoords::FVoxelLevelChunkKey& LevelKey) const
{
	using namespace VoxelCoords;

	const int64 FloorMm = FootprintSolidFloorMmCached(LevelKey.Level, LevelKey.Key.X, LevelKey.Key.Y);
	if (FloorMm == INT64_MIN)
	{
		return false; // declined to bound: never claim solid on no information
	}

	// Topmost level-0 voxel INDEX this chunk covers, apron included. A level-L
	// chunk spans ChunkEdgeVoxels cells of (1 << Level) level-0 voxels each,
	// and the mesher reads one level-L cell past the interior top.
	const int64 LevelScale = int64(1) << LevelKey.Level;
	const int64 TopVoxel = (int64(LevelKey.Key.Z) + 1) * int64(ChunkEdgeVoxels) * LevelScale + LevelScale - 1;

	// That voxel's CENTRE, which is what materialAt tests against. Strict `<`:
	// the bound's contract is that everything strictly below it is solid.
	const int64 TopCentreMm = TopVoxel * int64(vxc::kVoxelSizeMm) + int64(vxc::kVoxelSizeMm) / 2;

	return TopCentreMm < FloorMm;
}

void FVoxelWorldImpl::ComputeFootprintChunkZRange(int32 ChunkX, int32 ChunkY, int32 Level, int32& OutChunkZMin, int32& OutChunkZMax,
                                                   int32& OutChunkZMaxUntrimmed) const
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
	OutChunkZMin = (int32)VoxelCoords::FloorDiv(TopVoxelMinAtLevel, (int64)ChunkEdgeVoxels) - 1;

	// The +2 headroom, expressed in LEVEL-0 voxels so it can be compared against
	// an absolute surface bound. Identical arithmetic to the old
	// `FloorDiv(FloorDiv(TopVoxelMax, LevelScale), ChunkEdgeVoxels) + 2`:
	// 2 chunks == 2 * ChunkEdgeVoxels level-L cells == that times LevelScale
	// level-0 voxels, and adding an exact multiple of ChunkEdgeVoxels*LevelScale
	// commutes with both floor divisions.
	const int64 HeadroomVoxels = int64(2) * ChunkEdgeVoxels * LevelScale;
	int64 TopVoxelForMax = TopVoxelMax + HeadroomVoxels;

	// SKY-BAND TRIM. What the +2 was protecting is stated right above: corner-
	// only sampling misses interior extremes, so the range is padded ABOVE. But
	// it was padded in LEVEL-L CHUNKS, and a level-L chunk is (1<<L) times
	// taller than a level-0 one -- so a headroom sized for level 0's 3.2 m
	// chunks became 102.4 m of guaranteed-empty sky at level 4, where each of
	// those chunks costs ~6,000 ms of worker time to mesh into nothing. The
	// quantity being protected against is a property of the TERRAIN (how much
	// higher the interior of a footprint can be than its corners), not of the
	// LOD level, so scaling it by the chunk height was never right.
	//
	// FootprintSurfaceUpperBoundMm bounds that quantity directly and exactly on
	// the tile-scale term (see namespace VoxelSkyBand). Take the tighter of the
	// two: where the analytic bound binds -- overwhelmingly at levels 2-4, whose
	// footprints are wide enough that the tile raster, not the detail noise,
	// dominates -- the range stops just above the highest ground the footprint
	// can contain. Where it does not bind (levels 0-1, whose 3.2-6.4 m
	// footprints sit well inside a single 30 m tile pixel, so the pixel-corner
	// maximum is a much weaker statement than the sampled columns), the old
	// +2-chunk rule survives untouched. A min() with the shipped rule is what
	// makes this strictly-never-wider-than-before AND never a regression on the
	// near rings that the M1 hitch budget actually cares about.
	if (VoxelSkyBand::GetTrimEnabled())
	{
		const int64 BoundMm = FootprintSurfaceUpperBoundMm(Level, ChunkX, ChunkY);
		if (BoundMm != INT64_MAX)
		{
			// Same convention as TopVoxelMax above (the voxel CONTAINING the
			// surface, not the topmost solid one) -- one voxel more generous
			// than IsChunkProvablyAllAir's bound, deliberately, since this one
			// decides what is never requested at all.
			const int64 AnalyticTopVoxel = vxc::floorDiv(BoundMm, int64(vxc::kVoxelSizeMm));
			TopVoxelForMax = FMath::Min(TopVoxelForMax, AnalyticTopVoxel);
		}
	}

	OutChunkZMax = (int32)VoxelCoords::FloorDiv(VoxelCoords::FloorDiv(TopVoxelForMax, LevelScale), (int64)ChunkEdgeVoxels);
	OutChunkZMaxUntrimmed =
		(int32)VoxelCoords::FloorDiv(VoxelCoords::FloorDiv(TopVoxelMax + HeadroomVoxels, LevelScale), (int64)ChunkEdgeVoxels);
	// The trim may never invert the range: ZMin is derived from a corner the
	// bound is guaranteed to dominate, but assert the invariant rather than
	// reason about it at every call site.
	OutChunkZMax = FMath::Max(OutChunkZMax, OutChunkZMin);
}

void FVoxelWorldImpl::FootprintChunkZRangeCached(int32 ChunkX, int32 ChunkY, int32 Level, int32& OutChunkZMin,
                                                 int32& OutChunkZMax, int32& OutChunkZMaxUntrimmed) const
{
	const VoxelCoords::FVoxelLevelChunkKey CacheKey{Level, VoxelCoords::FVoxelChunkKey{ChunkX, ChunkY, 0}};
	if (const FFootprintZRange* Hit = FootprintZRangeCache.Find(CacheKey))
	{
		OutChunkZMin = Hit->ChunkZMin;
		OutChunkZMax = Hit->ChunkZMax;
		OutChunkZMaxUntrimmed = Hit->ChunkZMaxUntrimmed;
		return;
	}
	ComputeFootprintChunkZRange(ChunkX, ChunkY, Level, OutChunkZMin, OutChunkZMax, OutChunkZMaxUntrimmed);
	FootprintZRangeCache.Add(CacheKey, FFootprintZRange{OutChunkZMin, OutChunkZMax, OutChunkZMaxUntrimmed});
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
	// Buried-chunk skip: the band cache is keyed on the level-0 footprint and
	// grows with the same travel that grows FootprintZRangeCache, so it is
	// pruned on the same trigger and by the same rule (twice level 0's unload
	// ring). An entry is ~12 bytes and re-deriving one costs nothing extra --
	// it comes back with the next level-0 job in that footprint.
	{
		const UVoxelWorldSubsystem::FRingPreset& L0Preset = UVoxelWorldSubsystem::GetRingPresets()[0];
		const double L0ChunkEdge = VoxelCoords::ChunkEdgeUUForLevel(0);
		const double L0KeepRadiusUU = L0Preset.OuterMeters * 100.0 * UVoxelWorldSubsystem::UnloadRingMultiplier * 2.0;
		const double L0KeepRadiusSq = FMath::Square(L0KeepRadiusUU);
		for (auto It = FootprintBandCache.CreateIterator(); It; ++It)
		{
			const double CenterX = (double(It.Key().X) + 0.5) * L0ChunkEdge;
			const double CenterY = (double(It.Key().Y) + 0.5) * L0ChunkEdge;
			if (FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y) > L0KeepRadiusSq)
			{
				It.RemoveCurrent();
			}
		}
	}

	// All-solid admission skip: same key and same growth as the z-range memo,
	// so it prunes on the same trigger and by the same rule.
	for (auto It = FootprintSolidFloorCache.CreateIterator(); It; ++It)
	{
		const int32 Level = It.Key().Level;
		const double ChunkEdge = VoxelCoords::ChunkEdgeUUForLevel(Level);
		const double KeepRadiusUU =
			UVoxelWorldSubsystem::GetRingPresets()[Level].OuterMeters * 100.0 * UVoxelWorldSubsystem::UnloadRingMultiplier * 2.0;
		const double CenterX = (double(It.Key().Key.X) + 0.5) * ChunkEdge;
		const double CenterY = (double(It.Key().Key.Y) + 0.5) * ChunkEdge;
		if (FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y) > FMath::Square(KeepRadiusUU))
		{
			It.RemoveCurrent();
		}
	}

	for (auto It = FootprintZRangeCache.CreateIterator(); It; ++It)
	{
		const VoxelCoords::FVoxelLevelChunkKey& Key = It.Key();
		const UVoxelWorldSubsystem::FRingPreset& Preset = UVoxelWorldSubsystem::GetRingPresets()[Key.Level];
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
	//
	// R2-R4 starvation fix: the ORDER above is unchanged -- what changed is its
	// SCOPE. Each ring level now has its own queue and is sorted independently,
	// so this is still nearest-first, but nearest-first WITHIN A RING, which is
	// the only scope on which a distance ranking is well defined: a ring is an
	// XY annulus, so comparing an R0 chunk 38 m straight down against an R3
	// chunk 300 m out ranks two things that were never competing for the same
	// screen space. The cross-level decision moved out of the comparator and
	// into DispatchJobs, where it is an explicit slot floor -- see the note in
	// the ORIGINAL comment above about level-as-primary-key being measured
	// wrong: a floor is not level-major ordering, it reserves a MINIMUM for the
	// coarse rings and leaves everything above the floor nearest-first, so it
	// does not reintroduce the failure that ordering had.
	const auto SortEntries = [](TArray<FSortEntry>& Entries)
	{
		Entries.Sort(
		    [](const FSortEntry& A, const FSortEntry& B)
		    {
			    if (A.DistSq != B.DistSq)
			    {
				    return A.DistSq > B.DistSq; // farther = lower priority = sorts toward the front
			    }
			    return A.Key.Level > B.Key.Level; // exact-distance tie: higher level number = lower priority
		    });
	};
	// The worker queues already STORE their distance (FSortEntry), so a re-sort
	// only has to refresh the cached keys against the new anchor -- no separate
	// decorate pass, no scratch buffer, no undecorate write-back.
	for (TArray<FSortEntry>& Queue : PendingJobKeysByLevel)
	{
		for (FSortEntry& Entry : Queue)
		{
			Entry.DistSq = DistSq(Entry.Key);
		}
		SortEntries(Queue);
	}
	// The game-thread queue is still a bare-key array (it is edit-driven, always
	// near the player, and never deep enough for the storage to matter), so it
	// keeps the decorate-sort-undecorate form over a member scratch buffer.
	SortScratchGameThread.Reset(PendingGameThreadKeys.Num());
	for (const VoxelCoords::FVoxelLevelChunkKey& Key : PendingGameThreadKeys)
	{
		SortScratchGameThread.Add(FSortEntry{DistSq(Key), Key});
	}
	SortEntries(SortScratchGameThread);
	for (int32 Index = 0; Index < SortScratchGameThread.Num(); ++Index)
	{
		PendingGameThreadKeys[Index] = SortScratchGameThread[Index].Key;
	}
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

// Load-before-unload replacement direction (FChunkRecord::RetainReplaceDir).
// File scope so RecomputeDesiredSet (stamps it), ReplacementCovered (reads it),
// and DrainUnloads (gates on it) all see it unqualified.
enum : uint8 { RetainDir_None = 0, RetainDir_Finer = 1, RetainDir_Coarser = 2 };

uint8 FVoxelWorldImpl::ComputeRetainReplacementZMask(const VoxelCoords::FVoxelLevelChunkKey& Key, uint8 Dir,
                                                     const FVector& Anchor) const
{
	using namespace VoxelCoords;

	// The desired chunk-Z SPAN the entry pass would grant one footprint. Mirrors
	// RecomputeDesiredSet's Z loop step for step -- memoized base range, sky-band
	// edit hatch, depth skirt, edit-floor hatch -- because a mask that is
	// narrower than what the entry pass admits would mark a genuinely-desired
	// chunk "not wanted in Z" and let the stand-in release over it, which is the
	// exact bug this whole mask exists to close.
	//
	// TWO DELIBERATE OVER-APPROXIMATIONS, both in the safe direction (a WIDER
	// span means more bits set means more blocking):
	//  - The anchor-relative deep box is a separate set of chunks, not a
	//    contiguous extension of the surface band; folding it in with min/max
	//    yields a superset of (band UNION box). Underground only.
	//  - The level-0 admission band skip (BandProvesChunkEmpty) can drop chunks
	//    from INSIDE this span. Not mirrored: it would need the band cache per
	//    child chunk, and omitting it only ever holds a stand-in longer.
	const auto DesiredZSpan = [this, &Anchor](int32 Level, int32 Cx, int32 Cy, int32& OutZMin, int32& OutZMax)
	{
		int32 ZMaxUntrimmed = 0;
		FootprintChunkZRangeCached(Cx, Cy, Level, OutZMin, OutZMax, ZMaxUntrimmed);
		if (ZMaxUntrimmed > OutZMax)
		{
			if (const int32* EditedMaxZ = EditedFootprintMaxZ[Level].Find(FIntPoint(Cx, Cy)))
			{
				OutZMax = FMath::Max(OutZMax, FMath::Min(ZMaxUntrimmed, *EditedMaxZ));
			}
		}

		const double ChunkEdge = ChunkEdgeUUForLevel(Level);
		const double CenterX = (double(Cx) + 0.5) * ChunkEdge;
		const double CenterY = (double(Cy) + 0.5) * ChunkEdge;
		const double DistSq = FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y);
		OutZMin -= VoxelUnderground::SkirtDepthChunks(Level, DistSq);

		if (EditedFootprintMinZ[Level].Num() > 0 && VoxelStreamAdmission::EditFloorHatchEnabled())
		{
			if (const int32* EditedMinZ = EditedFootprintMinZ[Level].Find(FIntPoint(Cx, Cy)))
			{
				OutZMin = FMath::Min(OutZMin, *EditedMinZ);
			}
		}

		int32 BoxRadius = 0;
		if (bAnchorUnderground && VoxelUnderground::BoxRadiusChunks(Level, DistSq, BoxRadius))
		{
			const FVoxelChunkKey AnchorChunk = ChunkKeyForVoxel(WorldToVoxelForLevel(Anchor, Level));
			OutZMin = FMath::Min(OutZMin, AnchorChunk.Z - BoxRadius);
			OutZMax = FMath::Max(OutZMax, AnchorChunk.Z + BoxRadius);
		}
	};

	const int32 L = Key.Level, X = Key.Key.X, Y = Key.Key.Y, Z = Key.Key.Z;
	if (Dir == RetainDir_Finer)
	{
		if (L == 0)
		{
			return 0xFF; // no finer level exists; ReplacementCovered returns before it reads this
		}
		uint8 Mask = 0;
		// Four ComputeFootprintChunkZRange-backed lookups per eviction, and the
		// memo absorbs most of them (the entry pass has almost always just
		// visited these same child footprints). This runs per LOD-transition
		// eviction -- hundreds per 5 s window under 20 m/s motion -- NOT per
		// frame, which is precisely why the Z half lives here and not in
		// ReplacementCovered.
		for (int32 dy = 0; dy < 2; ++dy)
		{
			for (int32 dx = 0; dx < 2; ++dx)
			{
				int32 ZMin = 0, ZMax = 0;
				DesiredZSpan(L - 1, X * 2 + dx, Y * 2 + dy, ZMin, ZMax);
				for (int32 dz = 0; dz < 2; ++dz)
				{
					const int32 ChildZ = Z * 2 + dz;
					if (ChildZ >= ZMin && ChildZ <= ZMax)
					{
						Mask |= uint8(1u << (dx + dy * 2 + dz * 4));
					}
				}
			}
		}
		return Mask;
	}
	if (Dir == RetainDir_Coarser)
	{
		if (L + 1 >= kNumLevels)
		{
			return 0xFF; // outermost: nothing coarser, and ReplacementCovered returns before it reads this
		}
		int32 ZMin = 0, ZMax = 0;
		DesiredZSpan(L + 1, X >> 1, Y >> 1, ZMin, ZMax); // >> floors for negatives too
		const int32 ParentZ = Z >> 1;
		return (ParentZ >= ZMin && ParentZ <= ZMax) ? uint8(1) : uint8(0);
	}
	return 0;
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
	for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
	{
		bLevelScannedThisCall[Level] = false;
		LevelCandidatesRejectedThisCall[Level] = 0;
	}

	// Phase 2 fine-tier streaming (VoxelFineTileStreamer.h): pin the prefetch
	// ring around the anchor, best-effort load whatever in it is missing, and
	// evict whatever the LRU budget disallows outside that ring. A no-op
	// (FineStreamer null) unless -VoxelFineTileDir was passed. Placed once per
	// call, not per candidate: this is a budget/ring decision over the WHOLE
	// desired set, the same granularity RecomputeDesiredSet itself runs at
	// (anchor movement), not a per-chunk one -- AddCandidate's own gate below
	// is the per-footprint query against whatever residency this call leaves
	// in place.
	if (FineStreamer)
	{
		const int64 AnchorMmX = WorldToMm(Anchor.X);
		const int64 AnchorMmY = WorldToMm(Anchor.Y);
		FineStreamer->TickResidencyAndEviction(FVoxelFineTileStreamer::CoarseTileForWorldMm(AnchorMmX, AnchorMmY));
	}

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
	//
	// S2-0 (2026-07-27): AND THE QUEUE-DEPTH TEST ABOVE STOPPED MEANING WHAT IT
	// WAS WRITTEN TO MEAN, so it now carries a second condition.
	//
	// "The queue has drained below 3/4 cap" was a proxy for "there is spare
	// capacity downstream, so admitting more is safe". That proxy was only ever
	// correct because the queue was short for ONE reason: the consumer could not
	// keep up, so a short queue genuinely meant starvation. Wave S1 removed that
	// -- batched publication took applies from ~260 to ~800 chunks/s and the drain
	// loop now routinely empties the queue -- so the queue is short ALL THE TIME,
	// the cutoff relaxes on essentially every call, and bounded admission is
	// effectively off.
	//
	// Measured at the S1 config: 22,300 records admitted per second against ~800
	// chunks/s actually loading, ChunkRecords at 86,077 against a 39,020 settle,
	// and RecomputeDesiredSet at 66% of the streaming tick -- the O(tracked) exit
	// scan being paid for records that will never be reached.
	// docs/measurements/s1-close-2026-07-27.txt.
	//
	// So bound the thing that is actually growing. The pending JOB queue is
	// bounded and healthy; ChunkRecords is not, and it is what the exit scan
	// walks. A record cap is a direct statement of the invariant the queue depth
	// was standing in for.
	//
	// Default 0 = off = exactly the old behaviour, because this changes which
	// chunks get admitted and that has to be measured, not assumed. Sized from
	// measurement when it is: settle is 39,020 chunks and peak residency under
	// flight is ~50,900, so a useful cap sits above the latter and far below
	// 86,077.
	{
		const int32 Cap = VoxelStreamAdmission::GetPendingJobCap();
		const int32 RecordCap = VoxelDebug::GetStreamAdmissionRecordCap();
		const bool bQueueHasRoom = (Cap <= 0 || PendingJobNum() * 4 < Cap * 3);
		const bool bRecordsHaveRoom = (RecordCap <= 0 || ChunkRecords.Num() < RecordCap);
		if (bRecordsHaveRoom && bQueueHasRoom)
		{
			for (double& Cutoff : LevelAdmissionCutoffDistSq)
			{
				Cutoff = DBL_MAX;
			}
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
	for (auto& Pair : ChunkRecords)
	{
		const FVoxelLevelChunkKey& LevelKey = Pair.Key;
		if (PendingUnloadSet.Contains(LevelKey))
		{
			continue;
		}
		const UVoxelWorldSubsystem::FRingPreset& Preset = UVoxelWorldSubsystem::GetRingPresets()[LevelKey.Level];
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
			const double KeepUU = VoxelUnderground::VerticalKeepUU(LevelKey.Level, ChunkEdge);
			const double CenterZ = (double(LevelKey.Key.Z) + 0.5) * ChunkEdge;
			bBeyondVertical = FMath::Abs(CenterZ - Anchor.Z) > KeepUU;
		}
		if (bBeyondOuter || bInsideInner || bBeyondVertical)
		{
			// Load-before-unload (voxel.Stream.LodRetentionMs): if this eviction
			// is an LOD-ring TRANSITION (a finer ring took over from the inside,
			// or a coarser ring from the outside) and the chunk is still visible
			// (live component with geometry), keep it drawn as a stand-in until
			// its replacement can stream in -- DrainUnloads reads RetainUntilSeconds
			// and defers the actual park. A purely-vertical underground exit has no
			// coincident surface replacement, so it is NOT retained (nothing would
			// cover it; retaining would only delay cleanup). A zero-quad chunk
			// shows nothing, so there is no hole to bridge -- also not retained.
			const bool bLodTransition = bBeyondOuter || bInsideInner;
			auto& Rec = Pair.Value;
			if (bLodTransition && Rec.HoldsGeometry() && Rec.LastQuadCount > 0)
			{
				const float RetentionSeconds = VoxelDebug::GetStreamLodRetentionMs() / 1000.f;
				if (RetentionSeconds > 0.f)
				{
					Rec.RetainUntilSeconds = ElapsedSeconds + RetentionSeconds;
					// bInsideInner -> a FINER ring took over (check L-1 children);
					// otherwise bBeyondOuter -> a COARSER ring took over (check the
					// L+1 parent). ReplacementCovered uses this to release the
					// stand-in the instant its replacement is on screen.
					Rec.RetainReplaceDir = bInsideInner ? RetainDir_Finer : RetainDir_Coarser;
					// ...and WHICH of those replacement keys are wanted in Z, which
					// is a pure function of the terrain and therefore knowable once,
					// here, rather than per frame in DrainUnloads. Stamped
					// unconditionally on both directions so a record that is evicted,
					// re-admitted and evicted again can never consult a stale mask.
					// See FChunkRecord::RetainChildZMask.
					Rec.RetainChildZMask = ComputeRetainReplacementZMask(LevelKey, Rec.RetainReplaceDir, Anchor);
				}
			}
			// Eviction-cause split (see LevelEvictInner's doc comment). This is
			// the only point in the program where the three tests are all in
			// hand, and `in` -- a finer ring taking the footprint -- is the one
			// the retention stand-in exists to bridge. Priority order matches the
			// condition above, so the buckets partition the evictions.
			{
				const int32 EvictLevelIdx = FMath::Clamp(LevelKey.Level, 0, VoxelCoords::kNumLevels - 1);
				if (bInsideInner)
				{
					++LevelEvictInner[EvictLevelIdx];
				}
				else if (bBeyondOuter)
				{
					++LevelEvictOuter[EvictLevelIdx];
				}
				else
				{
					++LevelEvictVertical[EvictLevelIdx];
				}
			}
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
		for (TArray<FSortEntry>& Queue : PendingJobKeysByLevel)
		{
			Queue.RemoveAllSwap([this](const FSortEntry& E) { return EvictedThisCall.Contains(E.Key); },
			                    EAllowShrinking::No);
		}
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

	const int32 MaxRingLevel = UVoxelWorldSubsystem::GetMaxRingLevel();
	for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
	{
		if (Level > MaxRingLevel)
		{
			// Cascade ends below this level this run (-VoxelMaxRingLevel). Entry
			// only -- see GetMaxRingLevel: the exit scan above still runs for
			// every level so nothing already resident can be stranded.
			continue;
		}
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
		// Stale-scan refill (see LastEntryScanAnchorXY): the UNQUANTIZED anchor
		// this scan is about to make its distance decisions against. Written here,
		// past the gate, so it records only scans that actually ran -- which is
		// what makes TickStreaming's "anchor moved since this level was scanned"
		// trigger self-quiescing: the rescan it provokes sets this to the current
		// anchor, so a pinned anchor fires the trigger at most once per level.
		LastEntryScanAnchorXY[Level] = FVector2D(Anchor.X, Anchor.Y);
		bHasRecomputedLevel[Level] = true;
		++LevelEntryScans[Level];
		++LevelsScannedThisCall;
		bLevelScannedThisCall[Level] = true;
		AdmissionsThisLevel = 0; // per-level admission budget (see its doc comment)

		const UVoxelWorldSubsystem::FRingPreset& Preset = UVoxelWorldSubsystem::GetRingPresets()[Level];
		const double InnerUU = Preset.InnerMeters * 100.0;
		const double OuterUU = Preset.OuterMeters * 100.0;
		const double ChunkEdge = ChunkEdgeUUForLevel(Level);

		// RING SEAM COVERAGE (2026-07-24 fix). Admission is by chunk CENTRE
		// distance, but chunks have EXTENT and adjacent levels have DIFFERENT
		// chunk sizes -- and RingPresets annuli abut exactly (Outer[L] ==
		// Inner[L+1], zero overlap). That combination leaves ground covered by
		// NO level at every seam:
		//
		//   a level-L chunk's four level-(L-1) children sit at centre offsets
		//   (+-e/2, +-e/2), i.e. up to e/sqrt(2) FURTHER OUT radially. So a child
		//   can be rejected at L-1 (its centre >= Outer[L-1]) while its parent is
		//   rejected at L (the parent's centre < Inner[L] == Outer[L-1]).
		//
		// The result is 20-40 entirely missing chunk COLUMNS per boundary -- full
		// height, see-through to the sky -- 3.2 m wide at the 64 m seam up to
		// 51.2 m wide at the 1024 m seam. It never heals while stationary,
		// because the desired set is a pure function of anchor position and the
		// inner eviction test uses the same hard threshold with no hysteresis.
		// This is what Matt reported as "concentric rings of holes"; it is NOT
		// the load-before-unload path, which was a separate (also real) bug.
		//
		// FIX: pad this level's OUTER admit radius by the chunk half-diagonal, so
		// a chunk is admitted whenever any part of its footprint could fall
		// inside the annulus. Padding by exactly e/sqrt(2) is provably sufficient
		// and minimal: a gap child's centre is at most its parent's centre plus
		// e/sqrt(2), and the parent's centre is < Outer[L-1] by construction.
		//
		// Only the OUTER side needs padding -- padding there admits the finer
		// level slightly past the seam, so the finer (more accurate) mesh wins
		// in the overlap band, and the coarser ring's inner hole stays hard. The
		// exit pass does not fight this: bBeyondOuter uses Outer*1.25, which is
		// wider than Outer + half-diagonal at every level.
		//
		// Cost: ~7-10% more resident chunks per level. That is the price of the
		// annuli genuinely overlapping, which VoxelChunkComponent.cpp:828-844
		// already identified as the prerequisite for re-enabling the ring
		// cross-fade (disabled because the annuli did NOT overlap).
		const double ChunkHalfDiagUU = ChunkEdge * 0.70710678118654752440; // (edge/2)*sqrt(2)
		const double AdmitOuterUU = OuterUU + ChunkHalfDiagUU;

		const int32 ChunkSpan = FMath::CeilToInt32(AdmitOuterUU / ChunkEdge) + 1;

		for (int32 Cy = AnchorChunk.Y - ChunkSpan; Cy <= AnchorChunk.Y + ChunkSpan; ++Cy)
		{
			for (int32 Cx = AnchorChunk.X - ChunkSpan; Cx <= AnchorChunk.X + ChunkSpan; ++Cx)
			{
				const double CenterX = (double(Cx) + 0.5) * ChunkEdge;
				const double CenterY = (double(Cy) + 0.5) * ChunkEdge;
				const double DistSq = FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y);
				if (Level > 0 && DistSq < FMath::Square(InnerUU))
				{
					continue; // a finer ring owns this footprint
				}
				if (DistSq >= FMath::Square(OuterUU))
				{
					// Past this level's outer edge. Normally the NEXT level owns
					// this ground -- but only if the parent chunk containing it is
					// itself admitted. Admit this chunk ONLY when the parent is
					// not, which is exactly the seam gap and nothing else.
					//
					// (First cut padded the outer radius by the chunk half-diagonal
					// for every chunk. Correct, but blanket: +9.2% resident chunks
					// everywhere, measured at p50 14.9 -> 17.3 ms, chunks/s
					// 968 -> 672 and post-warmup hitches 1 -> 47. This admits only
					// the chunks that would otherwise be holes.)
					if (DistSq >= FMath::Square(AdmitOuterUU) || Level + 1 >= VoxelCoords::kNumLevels)
					{
						continue; // too far to be a seam case, or no coarser ring exists (clipmap takes over)
					}
					// Parent at L+1 covering this same ground. Its lattice is 2x
					// this one and origin-aligned, so the parent index is Cx>>1
					// (>> floors for negatives, which is what we want).
					const double ParentEdge = ChunkEdge * 2.0;
					const double ParentCX = (double(Cx >> 1) + 0.5) * ParentEdge;
					const double ParentCY = (double(Cy >> 1) + 0.5) * ParentEdge;
					const double ParentDistSq =
						FMath::Square(ParentCX - Anchor.X) + FMath::Square(ParentCY - Anchor.Y);
					// The parent's ring starts exactly where this one ends
					// (Inner[L+1] == Outer[L]), so the parent is admitted iff its
					// centre is at or beyond OuterUU. If it is, the ground is
					// covered and this chunk is redundant.
					if (ParentDistSq >= FMath::Square(OuterUU))
					{
						continue;
					}
				}

				++ThisFrameLevelFootprints[Level];
				int32 ChunkZMin, ChunkZMax, ChunkZMaxUntrimmed;
				FootprintChunkZRangeCached(Cx, Cy, Level, ChunkZMin, ChunkZMax, ChunkZMaxUntrimmed);

				// Sky-band trim escape hatch, applied OUTSIDE the memo for the
				// same reason the depth skirt is: it depends on the edit log,
				// which the memo may not key on. Worldgen puts nothing above the
				// terrain, but a PLAYER can -- TryPlace writes an arbitrary
				// solid material into the air above a hilltop, and the fixture
				// stampers place whole structures there. Those chunks must not
				// be trimmed away. Clamped to the pre-trim top so this only ever
				// restores chunks the shipped rule already granted; edits above
				// even that envelope are outside the desired set exactly as they
				// were before this change.
				if (ChunkZMaxUntrimmed > ChunkZMax)
				{
					if (const int32* EditedMaxZ = EditedFootprintMaxZ[Level].Find(FIntPoint(Cx, Cy)))
					{
						ChunkZMax = FMath::Max(ChunkZMax, FMath::Min(ChunkZMaxUntrimmed, *EditedMaxZ));
					}
				}

				// Underground streaming (see namespace VoxelUnderground), step
				// 1: widen the memoized SURFACE band downward by this band's
				// depth skirt. Applied here rather than inside
				// ComputeFootprintChunkZRange precisely so the memo's inputs
				// stay (Level, X, Y) only -- the skirt depends on the anchor's
				// horizontal distance, which the memo does not key on.
				ChunkZMin -= VoxelUnderground::SkirtDepthChunks(Level, DistSq);

				// DOWNWARD ESCAPE HATCH -- the exact mirror of the sky-band
				// trim's hatch above, and the prerequisite for skipping any
				// candidate inside this band on a worldgen-only proof.
				//
				// ChunkZMin is worldgen's statement about where the visible
				// world stops going down: the footprint's lowest corner surface,
				// minus one chunk, minus the depth skirt (12 level-0 chunks =
				// 38.4 m in the near band). Nothing WORLDGEN puts below that can
				// ever be seen. A PLAYER digging can: a shaft driven past the
				// skirt floor leaves its lowest chunks outside the desired set,
				// so they are never admitted, never meshed and never drawn --
				// a see-through hole at the bottom of the shaft that does not
				// heal while the anchor stays above ground (the anchor-relative
				// deep box, which would otherwise cover it, only exists once
				// the anchor is itself underground).
				//
				// EditedFootprintMinZ is the lowest level-L chunk any edit in
				// this footprint has touched, maintained by PropagateEditToMips
				// in the same one place and by the same walk as
				// EditedFootprintMaxZ, and already apron-extended across chunk
				// borders by CollectDirtyChunks -- so an edit that breaks
				// through a chunk floor records the chunk BELOW it too.
				//
				// Deliberately NOT clamped the way the sky hatch is. That one
				// clamps to ChunkZMaxUntrimmed because it is only undoing its
				// own trim; there is no untrimmed floor to clamp to here, and
				// clamping to the skirt would defeat the entire purpose. The
				// cost is bounded by how far somebody has actually dug.
				if (EditedFootprintMinZ[Level].Num() > 0 && VoxelStreamAdmission::EditFloorHatchEnabled())
				{
					if (const int32* EditedMinZ = EditedFootprintMinZ[Level].Find(FIntPoint(Cx, Cy)))
					{
						if (*EditedMinZ < ChunkZMin)
						{
							EditFloorWidestChunks = FMath::Max(EditFloorWidestChunks, ChunkZMin - *EditedMinZ);
							++EditFloorWidenedSinceLog;
							ChunkZMin = *EditedMinZ;
						}
					}
				}

				const auto AddCandidate = [this, ChunkEdge, CenterX, CenterY, &Anchor](const FVoxelLevelChunkKey& LevelKey,
				                                                                     bool bDeepAnchorRelative)
				{
					// RESURRECTION (2026-07-27, the last of the ring-gap residue). A
					// record can exist and yet be PENDING UNLOAD: the footprint
					// dipped inside this level's inner edge as the anchor passed
					// (bInsideInner eviction, stand-in retained), then re-entered
					// the annulus as the anchor receded. Skipping it as "already
					// tracked" while the unload was still queued stranded the
					// column permanently: the stand-in eventually parked (cap or
					// coverage), the record vanished, and no scan trigger remained
					// to re-admit it -- measured as a deterministic patch of R3
					// no-record holes at r=322-492 m surviving a full 60 s linger,
					// GPU legs only (slower R2 settling leaves more R3 stand-ins
					// alive at cap when the anchor pins). The chunk is desired
					// again and still holds its geometry, so CANCEL the unload
					// instead of letting it park and re-meshing later: pull it
					// from PendingUnloadSet (the pop side treats a set-absent key
					// as resurrected and skips it -- the stale PendingUnloadKeys
					// entry is inert) and clear the retention stamp so a future
					// eviction re-stamps fresh.
					if (VoxelStreaming::FChunkRecord* Existing = ChunkRecords.Find(LevelKey))
					{
						if (PendingUnloadSet.Contains(LevelKey))
						{
							PendingUnloadSet.Remove(LevelKey);
							Existing->RetainReplaceDir = RetainDir_None;
							Existing->RetainUntilSeconds = 0.0;
							++ResurrectionsSinceLog;
						}
						return;
					}

					// S2-3 ADOPTION. No record, but the geometry may still be in
					// the pool, hidden, from a recent eviction. Re-admitting it is
					// a table write; re-meshing it is a full round trip through
					// the GPU and the apply path.
					//
					// This is also the seam T4-1 lands on (Wave S4): speculatively
					// generated terrain arrives already parked, and admission
					// FINDS it here instead of commissioning it.
					if (FParkedGeometry* Parked = ParkedGeometry.Find(LevelKey))
					{
						// Staleness first. A parked chunk has no record, so this
						// is the only place its generation can be checked -- and
						// MarkChunkDirtyForRemesh already drops parked entries on
						// edit, so this catches the world-version case rather than
						// the edit case.
						const uint64 NowEpoch = EditEpoch.load(std::memory_order_relaxed);
						if (Parked->EditEpoch != NowEpoch)
						{
							EvictParkedKey(LevelKey);
							++ParkEvictedStaleSinceLog;
							// Fall through and admit it normally.
						}
						else
						{
							VoxelStreaming::FChunkRecord& Adopted = ChunkRecords.Add(LevelKey);
							Adopted.PoolSlot = Parked->PoolHandle;
							Adopted.GenerationId = Parked->GenerationId;
							Adopted.LastQuadCount = Parked->QuadCount;
							Adopted.bDeepAnchorRelative = bDeepAnchorRelative;
							// SETTLED, and that is the point: the geometry is
							// final and on screen the moment the table entry is
							// restored, so this record can immediately release a
							// retained stand-in above or below it.
							Adopted.bMeshSettled = true;
							Adopted.LoadedAtSeconds = ElapsedSeconds;

							if (UVoxelGpuPoolComponent* Pool = GpuPool.Get())
							{
								Pool->UnparkChunk(Parked->PoolHandle, Parked->OriginInPool, Parked->Level);
							}
							ResidentQuads += Parked->QuadCount;
							// COUNTS AS A LOAD, because it is one: the chunk is
							// resident and drawing. Without this every adopted
							// chunk is invisible to TotalChunksLoaded and so to
							// chunksPerSec -- which made parking read as a 21%
							// throughput regression when it is throughput-NEUTRAL.
							// Measured: 708.3 meshed + 186 adopted/s = 894.3
							// against 893.7 with parking off, while doing 6% less
							// tick work. A metric that cannot see half a feature's
							// output will reject the feature.
							++TotalChunksLoaded;
							++LevelChunksLoadedTotal[FMath::Clamp(LevelKey.Level, 0, VoxelCoords::kNumLevels - 1)];
							ParkedGeometry.Remove(LevelKey);
							++ChunksAdoptedSinceLog;
							++ChunksAdoptedTotal;
							// T4-1's deciding number. Counted apart from demand
							// parking because the two answer different questions:
							// demand parking asks "did evicted ground come back",
							// speculation asks "was the cone aimed right".
							if (Parked->bSpeculative)
							{
								SpecParkedNow = FMath::Max(0, SpecParkedNow - 1);
								++SpecAdoptedSinceLog;
								++SpecAdoptedTotal;
							}
							++RecordsAddedSinceLog;
							++LevelRecordsAdded[FMath::Clamp(LevelKey.Level, 0, VoxelCoords::kNumLevels - 1)];
							// NO JOB IS QUEUED. That is the whole win.
							return;
						}
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
					const int32 QueueLevel = FMath::Clamp(LevelKey.Level, 0, VoxelCoords::kNumLevels - 1);
					if (!bOverlayAware && Cap > 0 && AdmissionsThisLevel >= Cap / 4)
					{
						// Per-level admission budget (see AdmissionsThisLevel).
						++CandidatesRejectedSinceLog;
						++CandidatesRejectedThisCall;
						++LevelCandidatesRejectedThisCall[QueueLevel];
						bAdmissionDeferredWork[QueueLevel] = true;
						return;
					}
					// The cutoff is now this RING's own (see
					// LevelAdmissionCutoffDistSq): a single global radius always
					// excluded the outer annuli entirely.
					const double CenterZ = (double(LevelKey.Key.Z) + 0.5) * ChunkEdge;
					const double DistSq3D = FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y) +
					                        FMath::Square(CenterZ - Anchor.Z);
					if (!bOverlayAware && DistSq3D >= LevelAdmissionCutoffDistSq[QueueLevel])
					{
						++CandidatesRejectedSinceLog;
						++CandidatesRejectedThisCall;
						++LevelCandidatesRejectedThisCall[QueueLevel];
						bAdmissionDeferredWork[QueueLevel] = true;
						return;
					}

					// Phase 2 fine-tier residency gate ("block-until-ready" --
					// docs/terrain-amplification-plan.md: a missing fine tile is
					// a DESYNC, never a procedural fallback, because collision
					// and edits must agree on every client). No-op when
					// FineStreamer is null (-VoxelFineTileDir not passed).
					// Applies to EVERY candidate reaching this point, including
					// the overlay-aware (edit-driven) path above: an edit is
					// always near the player, so it is virtually always inside
					// the already-resident prefetch ring, but there is no
					// edit-path exception in the product rule and none is added
					// here. IsFootprintResident is a pure, no-I/O check (cheap,
					// the common case); RequestFootprint is the one call that
					// can synchronously touch disk, and only runs when the
					// footprint is not already resident.
					if (FineStreamer)
					{
						const double HalfEdge = ChunkEdge * 0.5;
						const int64 FineX0Mm = WorldToMm(CenterX - HalfEdge);
						const int64 FineX1Mm = WorldToMm(CenterX + HalfEdge);
						const int64 FineY0Mm = WorldToMm(CenterY - HalfEdge);
						const int64 FineY1Mm = WorldToMm(CenterY + HalfEdge);
						if (!FineStreamer->IsFootprintResident(FineX0Mm, FineY0Mm, FineX1Mm, FineY1Mm) &&
						    !FineStreamer->RequestFootprint(FineX0Mm, FineY0Mm, FineX1Mm, FineY1Mm))
						{
							// Still not resident (missing on disk, or failed
							// validation -- see VoxelFineTileStreamer.cpp's
							// EnsureTileResident logging). Do NOT admit; this
							// candidate is re-scanned on every future call
							// (RecomputeDesiredSet re-evaluates every
							// un-admitted candidate on anchor movement), so it
							// is retried, not dropped, once the fine tier
							// catches up.
							++CandidatesRejectedSinceLog;
							++CandidatesRejectedThisCall;
							++LevelCandidatesRejectedThisCall[QueueLevel];
							bAdmissionDeferredWork[QueueLevel] = true;
							return;
						}
					}

					VoxelStreaming::FChunkRecord& NewRecord = ChunkRecords.Add(LevelKey);
					++RecordsAddedSinceLog;
					++LevelRecordsAdded[QueueLevel];
					AdmissionsThisLevel += bOverlayAware ? 0 : 1;
					NewRecord.bDeepAnchorRelative = bDeepAnchorRelative;
					if (bOverlayAware)
					{
						PendingGameThreadKeys.Add(LevelKey);
					}
					else
					{
						// Distance cached at admission; SortPendingQueues refreshes
						// it against the anchor at the bottom of this same call.
						PendingJobKeysByLevel[QueueLevel].Add(FSortEntry{DistSq3D, LevelKey});
					}
				};

				// Buried-chunk band skip, ADMISSION side (see
				// VoxelStreamAdmission::AdmissionBandSkipMode). Level 0 only,
				// for the same reason the dispatch-side skip is: the band is
				// derived from a level-0 job's own 34x34 column grid and a
				// level-L (L>=1) chunk spans 2^L x 2^L level-0 footprints.
				//
				// One lookup per FOOTPRINT, not per chunk: the band is a
				// property of (X,Y) alone, which is the whole reason it is
				// cheap enough to consult here.
				//
				// WHAT THIS CANNOT DO. The band does not exist until some
				// level-0 job in this footprint has completed AND drained, so a
				// footprint entering the desired set for the first time has
				// nothing to consult and every one of its chunks is admitted
				// blind. That is counted (BandAdmitCold) rather than assumed
				// away.
				const int32 BandSkipMode = (Level == 0) ? VoxelStreamAdmission::AdmissionBandSkipMode() : 0;
				const VoxelStreaming::FFootprintBand* AdmitBand = nullptr;
				// Edit veto, identical in shape and reasoning to the all-solid
				// admission skip's below: everything at or above the lowest
				// edited chunk in this footprint is admitted normally. Chunks
				// strictly BELOW it are untouched rock whose apron is untouched
				// too -- CollectDirtyChunks extends an edit across every chunk
				// border it touches, so an edit that reaches a chunk floor has
				// already lowered this value to the chunk beneath.
				int32 AdmitEditFloorZ = MAX_int32;
				if (BandSkipMode != 0)
				{
					AdmitBand = FootprintBandCache.Find(FIntPoint(Cx, Cy));
					(AdmitBand ? BandAdmitWarmSinceLog : BandAdmitColdSinceLog) += 1;
					if (AdmitBand && EditedFootprintMinZ[0].Num() > 0)
					{
						if (const int32* E = EditedFootprintMinZ[0].Find(FIntPoint(Cx, Cy)))
						{
							AdmitEditFloorZ = *E;
						}
					}
				}

				for (int32 Cz = ChunkZMin; Cz <= ChunkZMax; ++Cz)
				{
					const FVoxelLevelChunkKey CandidateKey{Level, FVoxelChunkKey{Cx, Cy, Cz}};
					bool bBandEmpty = false;
					if (AdmitBand)
					{
						bool bAllAir = false;
						// Ordered so the arithmetic (free) runs before either
						// map lookup, and the ChunkRecords lookup replaces the
						// one AddCandidate would have done rather than adding
						// to it -- an already-tracked chunk is not a candidate
						// and must not be counted as one.
						bBandEmpty = VoxelStreaming::BandProvesChunkEmpty(*AdmitBand, Cz, bAllAir) &&
						             !ChunkRecords.Contains(CandidateKey);
					}
					if (bBandEmpty)
					{
						if (Cz >= AdmitEditFloorZ)
						{
							++BandAdmitEditVetoSinceLog;
						}
						else
						{
							++BandSkippedAtAdmissionSinceLog;
							if (BandSkipMode == 1)
							{
								continue; // never a record, never queued, never dispatched
							}
						}
					}
					AddCandidate(CandidateKey, /*bDeepAnchorRelative*/ false);
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
					// All-solid admission skip (see VoxelStreamAdmission::
					// SolidSkipEnabled). The EDIT VETO is resolved here, once
					// per footprint, and deliberately OUTSIDE the memo: the
					// analytic floor is a statement about worldgen, and a
					// player digging is not worldgen. If this footprint has any
					// edit at or below a candidate's Z, that candidate is
					// admitted normally.
					//
					// MIN_int32 (not MAX_int32) when there is no edit: it makes
					// the "is the candidate at or above the lowest edit"
					// comparison below false for every Cz, i.e. no veto, which
					// is the common case and the one that must be branch-cheap.
					const bool bSolidSkip = VoxelStreamAdmission::SolidSkipEnabled();
					const int32* EditedMinZ =
						bSolidSkip ? EditedFootprintMinZ[Level].Find(FIntPoint(Cx, Cy)) : nullptr;

					for (int32 Cz = AnchorChunk.Z - BoxRadius; Cz <= AnchorChunk.Z + BoxRadius; ++Cz)
					{
						if (Cz >= ChunkZMin && Cz <= ChunkZMax)
						{
							continue;
						}
						const FVoxelLevelChunkKey DeepKey{Level, FVoxelChunkKey{Cx, Cy, Cz}};

						// Veto if this footprint holds an edit at or below this
						// chunk. Conservative on purpose: it vetoes the whole
						// column below the lowest edit rather than tracking
						// which chunks were edited, because the cost of a false
						// veto is one tracked record and the cost of a missed
						// one is a chunk the world does not know exists sitting
						// where somebody is digging.
						const bool bEditVeto = EditedMinZ != nullptr && Cz >= *EditedMinZ;
						if (bSolidSkip && !bEditVeto && IsChunkProvablyAllSolid(DeepKey))
						{
							++SolidSkippedAtAdmissionSinceLog;
							++LevelSolidSkippedAtAdmission[FMath::Clamp(Level, 0, VoxelCoords::kNumLevels - 1)];
							if (!VoxelStreamAdmission::VerifySolidSkipEnabled())
							{
								continue;
							}
							// Verify mode: admit and dispatch it anyway, and
							// remember that we claimed it solid so DrainResults
							// can hold the claim against the real mesh.
							SolidSkipVerifyKeys.Add(DeepKey);
						}
						AddCandidate(DeepKey, /*bDeepAnchorRelative*/ true);
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
	// leaves each level queue's cached distances aligned with the anchor. Timed inside
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

bool FVoxelWorldImpl::DropFarthestOverCap(TArray<FSortEntry>& Entries, int32 EntryCap, double& OutCutoffDistSq)
{
	// Bounded admission gate (b) -- see LevelAdmissionCutoffDistSq's doc
	// comment. `Entries` must be sorted lowest-priority-first (farthest at
	// index 0), which is what SortPendingQueues leaves behind.
	const int32 OldNum = Entries.Num();
	if (EntryCap <= 0 || OldNum <= EntryCap)
	{
		OutCutoffDistSq = DBL_MAX; // not full: admit everything in range
		return false;
	}

	int32 ToDrop = OldNum - EntryCap;
	int32 Write = 0;
	for (int32 Read = 0; Read < OldNum; ++Read)
	{
		const FSortEntry Entry = Entries[Read];
		if (ToDrop > 0)
		{
			VoxelStreaming::FChunkRecord* Rec = ChunkRecords.Find(Entry.Key);
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
			if (!Rec->HoldsGeometry() && !Rec->bJobInFlight && !PendingUnloadSet.Contains(Entry.Key))
			{
				ChunkRecords.Remove(Entry.Key);
				++RecordsDroppedSinceLog;
				++LevelRecordsDropped[FMath::Clamp(Entry.Key.Level, 0, VoxelCoords::kNumLevels - 1)];
				--ToDrop;
				continue;
			}
		}
		Entries[Write++] = Entry;
	}
	Entries.SetNum(Write, EAllowShrinking::No);

	// The cutoff is the distance of a surviving entry a headroom band IN FROM
	// the farthest survivor. Drops only ever happen while ToDrop > 0, i.e. from
	// the front (farthest) of a sorted array, so the survivors are exactly the
	// original tail in the original order -- original index OldNum - Write + H
	// is survivor index H, and the array is still sorted.
	//
	// Why not simply "distance of the farthest survivor": candidates cluster
	// just inside it, so every call would admit a few thousand chunks barely
	// nearer than the cutoff and then drop most of them again on this same pass
	// -- a thrash costing one TMap insert + one erase per chunk per call
	// (measured at ~46k/second before this band existed). The band admits only
	// chunks that are clearly nearer, sized so a call's admissions are on the
	// order of what a call's dispatches drain, which is the whole point of the
	// exercise: make production track drain.
	const int32 Headroom = FMath::Max(1, EntryCap / 8);
	const int32 CutoffIndex = FMath::Min(Headroom, Write - 1);
	OutCutoffDistSq = (Write > 0) ? Entries[CutoffIndex].DistSq : DBL_MAX;
	return true;
}

void FVoxelWorldImpl::TruncatePendingJobQueue()
{
	// Called only from RecomputeDesiredSet, immediately after SortPendingQueues,
	// so every level's queue is sorted farthest-first with its distances current.
	const int32 Cap = VoxelStreamAdmission::GetPendingJobCap();
	bool bHeldBack = false;
	bool bLevelHeldBackThisCall[VoxelCoords::kNumLevels] = {}; // per-ring attribution for the clear below

	if (Cap <= 0)
	{
		for (double& Cutoff : LevelAdmissionCutoffDistSq)
		{
			Cutoff = DBL_MAX;
		}
	}
	else if (VoxelStreamAdmission::GetRingQuotaEnabled())
	{
		// Per-ring share of the cap. This is the half of the fix that matters
		// with the cap ON: a single shared cap is spent by whichever ring
		// enumerates the most candidates nearest the anchor, which is always R0
		// and R1, and the resulting global cutoff radius (~246 m measured) then
		// sits INSIDE R3's inner radius (256 m), so R3 and R4 never admitted a
		// single chunk. A share each cannot be crowded out.
		for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
		{
			const int32 LevelCap =
				FMath::Max(1, FMath::RoundToInt(double(Cap) * VoxelStreamAdmission::kRingCapShare[Level]));
			if (DropFarthestOverCap(PendingJobKeysByLevel[Level], LevelCap, LevelAdmissionCutoffDistSq[Level]))
			{
				// This ring, and only this ring, still has candidates it could
				// not take -- so only this ring's refill trigger stays armed.
				bHeldBack = true;
				bLevelHeldBackThisCall[Level] = true;
				bAdmissionDeferredWork[Level] = true;
			}
		}
	}
	else
	{
		// -VoxelRingQuota=0: the pre-fix behaviour, reproduced exactly -- one
		// shared cap over the union of every level's queue, dropped in global
		// distance order, one cutoff radius for all rings. Kept so the A/B runs
		// on ONE binary (see GetRingQuotaEnabled's comment).
		TruncateMergeScratch.Reset();
		for (const TArray<FSortEntry>& Queue : PendingJobKeysByLevel)
		{
			TruncateMergeScratch.Append(Queue);
		}
		if (TruncateMergeScratch.Num() > Cap)
		{
			TruncateMergeScratch.Sort(
			    [](const FSortEntry& A, const FSortEntry& B)
			    {
				    if (A.DistSq != B.DistSq)
				    {
					    return A.DistSq > B.DistSq;
				    }
				    return A.Key.Level > B.Key.Level;
			    });
			double GlobalCutoff = DBL_MAX;
			bHeldBack = DropFarthestOverCap(TruncateMergeScratch, Cap, GlobalCutoff);
			if (bHeldBack)
			{
				// -VoxelRingQuota=0 reproduces the pre-quota behaviour exactly,
				// including its single global cap, so there is no per-ring
				// attribution to make here: arm every ring's trigger.
				for (bool& bDeferred : bAdmissionDeferredWork)
				{
					bDeferred = true;
				}
			}
			for (TArray<FSortEntry>& Queue : PendingJobKeysByLevel)
			{
				Queue.Reset();
			}
			// The merged array is still globally sorted, so appending in order
			// leaves each per-level queue sorted too.
			for (const FSortEntry& Entry : TruncateMergeScratch)
			{
				PendingJobKeysByLevel[FMath::Clamp(Entry.Key.Level, 0, VoxelCoords::kNumLevels - 1)].Add(Entry);
			}
			for (double& Cutoff : LevelAdmissionCutoffDistSq)
			{
				Cutoff = GlobalCutoff;
			}
		}
		else
		{
			for (double& Cutoff : LevelAdmissionCutoffDistSq)
			{
				Cutoff = DBL_MAX;
			}
		}
	}

	// THE ONE-WAY-LATCH BUG (2026-07-27, the R4/R5 refill oscillation). This
	// used to be `if (bHeldBack) return;` -- a GLOBAL early-return gating a
	// PER-LEVEL clear. bHeldBack is an OR across all six rings, and R0
	// overflows its share on essentially every call of a flight, so while R0
	// was held back NO ring's deferral flag could ever clear -- the invariant
	// stated at the DropFarthestOverCap call ("this ring, and only this ring,
	// stays armed") held for the SET and not for the CLEAR. Result, measured
	// on the B1 leg: R4/R5's flags latched once at t~101s and their refill
	// triggers then free-ran at frame rate whenever their queues emptied --
	// 428/421 full-annulus rescans in a 14 s burst for ~1.7 admitted chunks
	// per scan. With ring quota ON the attribution is genuinely per ring, so
	// the clear must be too: skip only the levels THIS call held back.
	//
	// (-VoxelRingQuota=0 keeps the global return below: one shared cap has no
	// per-ring attribution to make, exactly as its arming comment says.)
	if (bHeldBack && !VoxelStreamAdmission::GetRingQuotaEnabled())
	{
		return; // global cap, global attribution: every flag was armed above
	}
	// Clear the deferral flag only for levels that actually re-enumerated on
	// this call with zero rejections AND were not held back by the truncation
	// above (a movement-triggered call scans only the levels whose own chunk
	// the anchor crossed, so a level gated out of it may still have candidates
	// waiting -- clearing then would lose the refill trigger for them). A
	// refill call always satisfies the scanned condition, since it clears the
	// per-level gate first.
	for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
	{
		if (bLevelScannedThisCall[Level] && LevelCandidatesRejectedThisCall[Level] == 0 &&
		    !bLevelHeldBackThisCall[Level])
		{
			bAdmissionDeferredWork[Level] = false;
		}
	}

	// voxel.Stream.LogAdmission: the admission loop's state, per level, per
	// call. Whether a ring keeps filling is a function of four things
	// interacting -- did it re-enumerate, how many did it take, how many did it
	// turn away, and is its refill trigger still armed -- and no combination of
	// the existing counters shows them together at the moment they decide.
	static const auto* CVarLogAdmission =
		IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Stream.LogAdmission"));
	if (CVarLogAdmission && CVarLogAdmission->GetInt() != 0)
	{
		FString Line;
		for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
		{
			Line += FString::Printf(TEXT("R%d[scan=%d rej=%d q=%d def=%d] "),
			                        Level, bLevelScannedThisCall[Level] ? 1 : 0,
			                        LevelCandidatesRejectedThisCall[Level],
			                        PendingJobKeysByLevel[Level].Num(),
			                        bAdmissionDeferredWork[Level] ? 1 : 0);
		}
		UE_LOG(LogVoxelPerf, Log, TEXT("Voxel admission pass: %s"), *Line);

		// A ring stuck with a handful of entries it never dispatches is the
		// pathology this whole investigation is about, and the entries are
		// invisible in aggregate counters. Dump them, with the record state that
		// decides whether DispatchJobs will act on them.
		for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
		{
			const TArray<FSortEntry>& Queue = PendingJobKeysByLevel[Level];
			if (Queue.Num() == 0 || Queue.Num() > 8)
			{
				continue;
			}
			for (const FSortEntry& Entry : Queue)
			{
				const VoxelStreaming::FChunkRecord* R = ChunkRecords.Find(Entry.Key);
				UE_LOG(LogVoxelPerf, Log,
				       TEXT("  stuck R%d (%d,%d,%d) rec=%d inFlight=%d settled=%d quads=%d overlay=%d dist=%.0fm"),
				       Level, Entry.Key.Key.X, Entry.Key.Key.Y, Entry.Key.Key.Z,
				       R != nullptr, R && R->bJobInFlight, R && R->bMeshSettled,
				       R ? R->LastQuadCount : -1,
				       NeedsOverlayAwarePath(Entry.Key) ? 1 : 0,
				       FMath::Sqrt(Entry.DistSq) / 100.0);
			}
		}
	}
}

// Which of a render chunk's four lateral faces abut a DIFFERENT cascade ring
// (or the clipmap/void past the outermost ring), i.e. where the neighbour chunk
// at THIS level does not belong to this level's annulus. Those faces get the
// ring-boundary skirt (see MeshChunkBricks' ERingSkirtFace comment). Uses the
// exact annulus-membership test RecomputeDesiredSet admits candidates with, so
// a set bit means precisely "RecomputeDesiredSet would not have placed a
// same-level chunk across this face" -- the definition of a ring boundary.
// Computed on the game thread at dispatch (anchor is fixed for the job) and
// baked into the mesh; a chunk that later changes ring membership is unloaded
// and re-meshed via the desired-set delta, so the baked mask never goes stale
// while the chunk is resident.
static uint8 ComputeRingSkirtMask(const VoxelCoords::FVoxelLevelChunkKey& LevelKey, const FVector& Anchor)
{
	if (!VoxelStreamAdmission::RingSkirtEnabled())
	{
		return 0;
	}
	const int32 Level = LevelKey.Level;
	if (Level == 0)
	{
		return 0; // finest ring: nothing finer sits inside it, so no seam to retain
	}
	const UVoxelWorldSubsystem::FRingPreset& Preset = UVoxelWorldSubsystem::GetRingPresets()[Level];
	const double InnerSq = FMath::Square(Preset.InnerMeters * 100.0);
	const double ChunkEdge = VoxelCoords::ChunkEdgeUUForLevel(Level);
	// A face is retained only when the neighbour across it belongs to a FINER
	// ring (its chunk centre falls inside this ring's inner hole). That is the
	// side whose covering wall faces the camera (inward, -radius): the coarser
	// ring drops an inward retaining wall down to meet the finer surface, which
	// is what actually closes the see-through. The finer ring's OUTER edge needs
	// no wall -- its outward-facing wall would be back-face culled, and the
	// coarser ring physically overlaps just beyond it (both admitted by centre
	// distance, and the coarser chunk is 2x wider), so the finer lip is backed
	// by coarse terrain rather than ocean.
	const auto NeighbourIsFiner = [&](int32 Cx, int32 Cy) -> bool
	{
		const double CenterX = (double(Cx) + 0.5) * ChunkEdge;
		const double CenterY = (double(Cy) + 0.5) * ChunkEdge;
		const double DistSq = FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y);
		return DistSq < InnerSq; // inside this ring's hole -> covered by a finer ring
	};
	const int32 Cx = LevelKey.Key.X;
	const int32 Cy = LevelKey.Key.Y;
	uint8 Mask = 0;
	if (NeighbourIsFiner(Cx - 1, Cy)) Mask |= RingSkirt_NegX;
	if (NeighbourIsFiner(Cx + 1, Cy)) Mask |= RingSkirt_PosX;
	if (NeighbourIsFiner(Cx, Cy - 1)) Mask |= RingSkirt_NegY;
	if (NeighbourIsFiner(Cx, Cy + 1)) Mask |= RingSkirt_PosY;
	return Mask;
}

// --- load-before-unload coverage index ----------------------------------
//
// A retained stand-in chunk (RecomputeDesiredSet kept it drawn when an LOD ring
// took over its footprint) must be parked the instant its REPLACEMENT LOD is
// actually on screen -- not after a fixed timer, which under fast movement
// expires mid-transition and leaves a rolling ring of holes.
//
// Coverage is answered straight off ChunkRecords: the replacement chunks are
// looked up by key and must all have SETTLED (FChunkRecord::bMeshSettled). The
// 2026-07-24 first cut instead maintained a side index, ColumnGeomCount, keyed
// (level, chunkX, chunkY) and counting records with LastQuadCount>0. That was
// wrong twice over, and Matt still saw rolling rings of holes with it in:
//
//  1. It DROPPED Z. "This column has geometry" was true if ANY chunk anywhere
//     in that vertical stack had geometry -- so a deep underground chunk
//     (bDeepAnchorRelative, ~38 m down, invisible) vouched for a SURFACE chunk
//     that had not arrived, and the stand-in was parked early. That is exactly
//     the hole retention exists to prevent.
//  2. It keyed on GEOMETRY, so a replacement that legitimately meshes to zero
//     quads (all air, all solid, a coastal all-ocean quarter) could never
//     report covered and always fell through to the safety cap.
//
// It also needed hand-maintained reconcile calls at every geometry gain/loss
// site, and two of those sites (the buried and sky-band pre-dispatch skips)
// were missing -- a leak that made columns read covered when they were not.
// Asking ChunkRecords directly removes the index, the reconcile calls, and that
// whole class of bug: there is no second copy of the truth to drift.
namespace
{
// Is the retained chunk's footprint now covered by the LOD that replaced it?
// Finer took over -> the eight child chunks at L-1 (2x2 in XY, 2 in Z -- Z
// matters, see above). Coarser -> the single parent chunk at L+1. Edge levels
// have no finer or coarser neighbour and report covered immediately, as before.
//
// --- THE ABSENCE BUG, AND THE FIX (ring-gap wave, 2026-07-27) --------------
//
// Until this change the rule was "a key with NO record is not DESIRED (the
// replacing ring's own footprint Z-range trim decided that) and therefore cannot
// block". That argument only holds when the replacing ring actually LOOKED at
// the footprint and declined it. A record also reads absent when the ring WANTED
// the chunk and could not have it: the per-level admission budget turned it away
// (AdmissionsThisLevel), the per-level distance cutoff did
// (LevelAdmissionCutoffDistSq), TruncatePendingJobQueue dropped it before it
// meshed, or the ENTRY-SCAN GATE skipped that level entirely because the anchor
// has not left its level-L chunk yet -- at level 4 a 51.2 m crossing, so a
// coarse ring can be seconds behind the eviction consulting it. Under any of
// those the old verdict was "covered" over ground nothing was drawing.
//
// The instrumented flights settled it. During 20 m/s motion a coarse chunk
// evicted at its INNER edge (bInsideInner, RecomputeDesiredSet's exit pass) had
// its stand-in released while the finer children that were supposed to replace
// it had been budget-rejected or queue-truncated and had NO records at all --
// median ~1700 uncovered columns in flight, dominantly R0 no-record at
// r=58-64 m, with covRelAbsent running 6-19k per 120 s leg. Standing holes at
// the ring boundary, exactly where retention exists to prevent them.
//
// SO: AN ABSENT REPLACEMENT NOW BLOCKS, unless the absent chunk is PROVABLY not
// desired. Desiredness has two independent halves and they are answered in
// different places, because they have opposite costs:
//
//   Z   -- anchor-INDEPENDENT (a pure function of terrain, via
//          ComputeFootprintChunkZRange) and expensive. Stamped ONCE per eviction
//          into FChunkRecord::RetainChildZMask; see
//          ComputeRetainReplacementZMask. A masked-OUT key is legitimately not
//          desired in Z and never blocks.
//   XY  -- anchor-DEPENDENT and therefore necessarily live, but pure arithmetic:
//          the replacement's own annulus test, INCLUDING the seam padding, run
//          against the current anchor right here. No sampling, no map lookups.
//
// WHY THE SEAM PADDING APPLIES TO THE XY HALF. The entry pass admits a chunk out
// to Outer + chunk-half-diagonal, but only where the PARENT covering the same
// ground is not itself admitted. For a child of an evicted stand-in that
// condition is satisfied by construction: the parent IS the stand-in, and once
// released it is not admitted. So the padded radius is the right radius here.
//
// The four verdicts per consulted key:
//   present && settled                    -> covered, does not block
//   present && !settled                   -> BLOCKS (unchanged)
//   absent  && (Z-masked-out || XY-undesired) -> legitimately absent, does not block
//   absent  && Z-desired && XY-desired    -> BLOCKS  <- this is the fix
//
// bOutAnyAbsent / OutAbsentCount therefore now mean "released while at least one
// consulted record was absent-but-UNDESIRED", i.e. the legitimate case.
// covRelAbsent stops being the ring-gap suspicion number and becomes a count of
// sound absences; the unsound case now shows up as `held` instead. See the
// counter block in DrainUnloads.
//
// The counts are meaningful ONLY on a `true` return: the loop short-circuits on
// the first blocking record, so a `false` verdict leaves them partial.
bool ReplacementCovered(const TMap<VoxelCoords::FVoxelLevelChunkKey, VoxelStreaming::FChunkRecord>& Records,
                        const VoxelCoords::FVoxelLevelChunkKey& Key, uint8 Dir, uint8 RetainChildZMask,
                        const FVector& Anchor, bool* bOutAnyAbsent = nullptr, int32* OutAbsentCount = nullptr,
                        int32* OutSettledCount = nullptr)
{
	if (bOutAnyAbsent) *bOutAnyAbsent = false;
	if (OutAbsentCount) *OutAbsentCount = 0;
	if (OutSettledCount) *OutSettledCount = 0;

	// bDesiredIfAbsent is the caller's answer to "would the replacing ring have
	// admitted this key, had nothing got in the way": Z from the stamped mask, XY
	// from the live test below. It decides ONLY what an absent record means; a
	// record that exists is judged on bMeshSettled exactly as before, since a
	// chunk that is admitted and mid-pipeline is worth waiting for whatever the
	// predicates now say about it.
	const auto Consult = [&Records, bOutAnyAbsent, OutAbsentCount, OutSettledCount](int32 L, int32 X, int32 Y, int32 Z,
	                                                                               bool bDesiredIfAbsent)
	{
		VoxelCoords::FVoxelLevelChunkKey K;
		K.Level = L;
		K.Key.X = X;
		K.Key.Y = Y;
		K.Key.Z = Z;
		const VoxelStreaming::FChunkRecord* R = Records.Find(K);
		if (R == nullptr)
		{
			if (bDesiredIfAbsent)
			{
				return false; // wanted, missing, nothing drawing it -- hold the stand-in
			}
			if (bOutAnyAbsent) *bOutAnyAbsent = true;
			if (OutAbsentCount) ++*OutAbsentCount;
			return true;
		}
		if (R->bMeshSettled && OutSettledCount)
		{
			++*OutSettledCount;
		}
		return R->bMeshSettled;
	};

	// The XY half of desiredness, at level `L` for the chunk footprint (Cx, Cy):
	// this level's own annulus, outer edge padded by the chunk half-diagonal --
	// the same constant and the same reasoning as the entry pass's AdmitOuterUU
	// (see RecomputeDesiredSet's RING SEAM COVERAGE comment). The `L > 0` inner
	// skip mirrors the entry pass too: a footprint inside level L's inner edge
	// belongs to an even finer ring, so level L declining it is correct and its
	// absence there is sound.
	const auto XYDesired = [&Anchor](int32 L, int32 Cx, int32 Cy)
	{
		const UVoxelWorldSubsystem::FRingPreset& Preset = UVoxelWorldSubsystem::GetRingPresets()[L];
		const double ChunkEdge = VoxelCoords::ChunkEdgeUUForLevel(L);
		const double CenterX = (double(Cx) + 0.5) * ChunkEdge;
		const double CenterY = (double(Cy) + 0.5) * ChunkEdge;
		const double DistSq = FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y);
		if (L > 0 && DistSq < FMath::Square(Preset.InnerMeters * 100.0))
		{
			return false;
		}
		const double AdmitOuterUU = Preset.OuterMeters * 100.0 + ChunkEdge * 0.70710678118654752440;
		return DistSq < FMath::Square(AdmitOuterUU);
	};

	const int32 L = Key.Level, X = Key.Key.X, Y = Key.Key.Y, Z = Key.Key.Z;
	if (Dir == RetainDir_Finer)
	{
		if (L == 0) return true; // nothing finer exists to wait for
		for (int32 dx = 0; dx < 2; ++dx)
		{
			for (int32 dy = 0; dy < 2; ++dy)
			{
				// Hoisted out of the dz loop: both children of a column share it.
				const bool bChildXYDesired = XYDesired(L - 1, X * 2 + dx, Y * 2 + dy);
				for (int32 dz = 0; dz < 2; ++dz)
				{
					const bool bChildZDesired = (RetainChildZMask & uint8(1u << (dx + dy * 2 + dz * 4))) != 0;
					if (!Consult(L - 1, X * 2 + dx, Y * 2 + dy, Z * 2 + dz, bChildZDesired && bChildXYDesired))
					{
						return false;
					}
				}
			}
		}
		return true;
	}
	if (Dir == RetainDir_Coarser)
	{
		if (L >= VoxelCoords::kNumLevels - 1) return true; // outermost: nothing coarser
		// The parent of an outward-evicted chunk sits well inside its own annulus
		// in practice, so the plain padded-annulus test is what applies; a parent
		// that falls outside it is genuinely somebody else's ground.
		const bool bParentDesired =
			(RetainChildZMask & 1u) != 0 && XYDesired(L + 1, X >> 1, Y >> 1);
		return Consult(L + 1, X >> 1, Y >> 1, Z >> 1, bParentDesired); // >> floors for negatives too
	}
	return true;
}
} // namespace

// --- coverage verifier (voxel.Stream.CoverageVerify) --------------------
//
// THE PROBLEM THIS SOLVES. Every counter in this file measures the streaming
// PIPELINE -- what was admitted, dispatched, drained, retained, released. None
// of them measures the OUTPUT, which is the only thing the player sees: is there
// ground under every footprint the cascade claims to own? "Concentric rings of
// holes" has now been diagnosed twice from screenshots and source reading (the
// ring-seam gap, and the load-before-unload first cut), because no number
// disagreed with a healthy-looking log. This is that number.
//
// WHAT IT DOES. One O(tracked) pass over ChunkRecords collapses every record
// into its COLUMN (Level, X, Y) with four flags, then each ring's core annulus
// is re-enumerated with the entry pass's own centre-distance test and every
// footprint is asked whether anything is visibly drawing it.
//
// THE TWO PRIOR FAILURE MODES THIS COVERAGE TEST IS BUILT TO DODGE -- both are
// documented in full in the ColumnGeomCount post-mortem above ReplacementCovered,
// and both were bugs in a coverage index that looked exactly like this one:
//
//  1. DEEP CHUNKS VOUCHING FOR SURFACE. Collapsing Z means an underground chunk
//     (bDeepAnchorRelative, ~38 m down, invisible from above) would otherwise
//     answer "this column has geometry" for a surface chunk that never arrived.
//     Excluded: bAnySettledNonDeep / bAnyGeomNonDeep both skip
//     bDeepAnchorRelative records, so only the surface band + skirt can vouch.
//     (bAnyRecord deliberately does NOT skip them -- it answers "does this
//     column exist in the desired set at all", which is the cause split.)
//  2. ZERO-QUAD LEGITIMATE EMPTINESS. A footprint that correctly meshes to no
//     quads -- all air above a valley floor, an all-ocean quarter -- has no
//     geometry and is not a hole. Keying coverage on geometry would report the
//     entire sky as missing. Primary test is therefore bMeshSettled ("this
//     chunk's mesh is FINAL"), with geometry only as an additional way to be
//     covered, never the requirement.
//
// WHAT "VISIBLY COVERED" MEANS. Any of:
//   (a) this column itself has a settled or drawing non-deep record;
//   (b) the PARENT column (L+1) is drawing -- a retained coarse stand-in still
//       on screen covers this ground even though this level owns it on paper.
//       Geometry, not settled: a parent that has left the desired set but is
//       still drawn is exactly the stand-in case, and a settled-but-parked
//       parent covers nothing;
//   (c) all four CHILD columns (L-1) have settled -- the finer ring has taken
//       over ahead of this level's own admission, which is the normal shape of
//       an inward LOD transition and is not a hole.
//
// ON THE Z QUESTION (asked explicitly by this wave's spec, answered from the
// code): CAN AN IN-ANNULUS FOOTPRINT BE LEGITIMATELY DESIRED WITH ZERO CHUNKS?
// From the Z math, NO. ComputeFootprintChunkZRange ends with
// `OutChunkZMax = FMath::Max(OutChunkZMax, OutChunkZMin)` -- the sky-band trim
// may never invert the range -- and the entry pass's `for (Cz = ChunkZMin; Cz <=
// ChunkZMax; ++Cz)` therefore always runs at least once. The sky-band and
// edit-floor hatches only ever WIDEN it. So no footprint is ever "trimmed to
// nothing", and a column with zero records is never explained by the Z rule.
// There is exactly ONE legitimate zero-record path: the level-0 ADMISSION BAND
// SKIP (-VoxelAdmissionBandSkip=1 / voxel.Stream.AdmissionBandSkip, DEFAULT OFF),
// which drops chunks a cached band proves empty and can in principle take a
// whole short column. That case is mirrored below and counted as covered.
void FVoxelWorldImpl::LogCoverageVerify()
{
	using namespace VoxelCoords;

	// --- pass 1: collapse ChunkRecords into columns ------------------------
	struct FColumnFlags
	{
		bool bAnyRecord = false;         // the column is in the desired set at all (deep or not)
		bool bAnySettledNonDeep = false; // a surface-band/skirt record whose mesh is FINAL
		bool bAnyGeomNonDeep = false;    // ...and is actually drawing geometry right now
		bool bAnyUnsettled = false;      // a record exists but is still waiting on a mesh
	};
	TMap<FIntVector, FColumnFlags> Columns;
	Columns.Reserve(ChunkRecords.Num());
	for (const auto& Pair : ChunkRecords)
	{
		FColumnFlags& Flags = Columns.FindOrAdd(FIntVector(Pair.Key.Level, Pair.Key.Key.X, Pair.Key.Key.Y));
		Flags.bAnyRecord = true;
		if (!Pair.Value.bMeshSettled)
		{
			Flags.bAnyUnsettled = true;
		}
		if (!Pair.Value.bDeepAnchorRelative) // failure mode 1: only surface records may vouch
		{
			Flags.bAnySettledNonDeep |= Pair.Value.bMeshSettled;
			Flags.bAnyGeomNonDeep |= Pair.Value.HoldsGeometry();
		}
	}

	const auto FindColumn = [&Columns](int32 L, int32 X, int32 Y) -> const FColumnFlags*
	{ return Columns.Find(FIntVector(L, X, Y)); };

	// --- pass 2: enumerate each ring's CORE annulus -------------------------
	const FVector& Anchor = LastAnchorLocation;
	const int32 MaxRingLevel = UVoxelWorldSubsystem::GetMaxRingLevel();
	// Mirrored from the entry pass's AddCandidate side (see the Z question
	// above). Hoisted: it is a command-line/cvar read, not a per-footprint one.
	const bool bBandSkipArmed = VoxelStreamAdmission::AdmissionBandSkipMode() == 1;

	int32 Holes = 0;
	int32 Scanned = 0;
	TArray<FString> Examples;
	static constexpr int32 kMaxExamples = 8;

	for (int32 Level = 0; Level <= MaxRingLevel; ++Level)
	{
		const UVoxelWorldSubsystem::FRingPreset& Preset = UVoxelWorldSubsystem::GetRingPresets()[Level];
		const double InnerUU = Preset.InnerMeters * 100.0;
		const double OuterUU = Preset.OuterMeters * 100.0;
		const double ChunkEdge = ChunkEdgeUUForLevel(Level);
		const FVoxelChunkKey AnchorChunk = ChunkKeyForVoxel(WorldToVoxelForLevel(Anchor, Level));
		// Span from the CORE outer radius, not the seam-padded AdmitOuterUU: the
		// padding band is admitted only where the parent is absent, so a footprint
		// out there having no records is the padding rule working, not a hole.
		// Verifying the core annulus alone keeps every reported hole unambiguous.
		const int32 ChunkSpan = FMath::CeilToInt32(OuterUU / ChunkEdge) + 1;

		for (int32 Cy = AnchorChunk.Y - ChunkSpan; Cy <= AnchorChunk.Y + ChunkSpan; ++Cy)
		{
			for (int32 Cx = AnchorChunk.X - ChunkSpan; Cx <= AnchorChunk.X + ChunkSpan; ++Cx)
			{
				// Identical centre math to the entry pass (RecomputeDesiredSet),
				// including the `Level > 0` inner skip -- level 0's annulus starts
				// at 0 and has no hole.
				const double CenterX = (double(Cx) + 0.5) * ChunkEdge;
				const double CenterY = (double(Cy) + 0.5) * ChunkEdge;
				const double DistSq = FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y);
				if (Level > 0 && DistSq < FMath::Square(InnerUU))
				{
					continue; // a finer ring owns this footprint
				}
				if (DistSq >= FMath::Square(OuterUU))
				{
					continue; // outside the core annulus (seam padding band -- see above)
				}
				++Scanned;

				const FColumnFlags* Self = FindColumn(Level, Cx, Cy);
				bool bCovered = Self != nullptr && (Self->bAnySettledNonDeep || Self->bAnyGeomNonDeep);
				if (!bCovered)
				{
					// (b) a coarser stand-in still on screen. Geometry, not settled.
					// Walk EVERY coarser level, not just the parent: under motion
					// the leading-edge cascade legitimately runs two levels behind
					// (an R2 stand-in held while R1 is still meshing and R0 has
					// not started), and footprints nest exactly, so a drawn
					// ancestor at any remove genuinely covers this column. The
					// first cut checked only L+1 and misread every such bridged
					// column as a hole (~1600 per window during the 2026-07-27
					// verification flights), burying the real signal.
					for (int32 Up = Level + 1; Up < VoxelCoords::kNumLevels && !bCovered; ++Up)
					{
						const FColumnFlags* Ancestor = FindColumn(Up, Cx >> (Up - Level), Cy >> (Up - Level));
						bCovered = Ancestor != nullptr && Ancestor->bAnyGeomNonDeep;
					}
				}
				if (!bCovered && Level > 0)
				{
					// (c) the finer ring has already taken the whole footprint.
					bool bAllChildren = true;
					for (int32 dx = 0; dx < 2 && bAllChildren; ++dx)
					{
						for (int32 dy = 0; dy < 2 && bAllChildren; ++dy)
						{
							const FColumnFlags* Child = FindColumn(Level - 1, Cx * 2 + dx, Cy * 2 + dy);
							bAllChildren = Child != nullptr && Child->bAnySettledNonDeep;
						}
					}
					bCovered = bAllChildren;
				}
				if (bCovered)
				{
					continue;
				}

				// The one legitimate zero-record path (see the Z question above):
				// every chunk this footprint would have contained was dropped at
				// admission because a cached band proved it empty. Deliberately
				// re-derives the range through ComputeFootprintChunkZRange rather
				// than FootprintChunkZRangeCached: the cached form populates the
				// memo, and a verifier must not write to a structure the streaming
				// path reads. Skirt included -- the entry pass widens ChunkZMin by
				// it before the admission loop, so the mirror must too.
				if (bBandSkipArmed && Level == 0)
				{
					if (const VoxelStreaming::FFootprintBand* Band = FootprintBandCache.Find(FIntPoint(Cx, Cy)))
					{
						int32 ZMin = 0, ZMax = 0, ZMaxUntrimmed = 0;
						ComputeFootprintChunkZRange(Cx, Cy, 0, ZMin, ZMax, ZMaxUntrimmed);
						ZMin -= VoxelUnderground::SkirtDepthChunks(0, DistSq);
						bool bWholeColumnProvenEmpty = true;
						for (int32 Cz = ZMin; Cz <= ZMax && bWholeColumnProvenEmpty; ++Cz)
						{
							bool bAllAir = false;
							bWholeColumnProvenEmpty = VoxelStreaming::BandProvesChunkEmpty(*Band, Cz, bAllAir);
						}
						if (bWholeColumnProvenEmpty)
						{
							continue; // nothing was ever supposed to be drawn here
						}
					}
				}

				++Holes;
				if (Examples.Num() < kMaxExamples)
				{
					// no-record        -- the column was never admitted (admission
					//                     budget, distance cutoff, queue drop, or the
					//                     entry-scan gate has not reached this level
					//                     yet). A DESIRED-SET hole.
					// unsettled        -- it WAS admitted and is still waiting on a
					//                     mesh. A THROUGHPUT/latency hole.
					// settled-deep-only-- records exist and have all settled, but every
					//                     one of them is bDeepAnchorRelative, so
					//                     nothing above ground is vouching. Rare, and
					//                     it is neither of the other two, so it gets
					//                     its own name rather than being filed under a
					//                     label that would be false.
					const TCHAR* Cause = (Self == nullptr || !Self->bAnyRecord)
						? TEXT("no-record")
						: (Self->bAnyUnsettled ? TEXT("unsettled") : TEXT("settled-deep-only"));
					Examples.Add(FString::Printf(TEXT("hole R%d (%d,%d) r=%.0fm cause=%s"), Level, Cx, Cy,
					                             FMath::Sqrt(DistSq) / 100.0, Cause));
				}
			}
		}
	}

	UE_LOG(LogVoxelPerf, Log, TEXT("Voxel coverage (window): holes=%d scanned=%d"), Holes, Scanned);
	for (const FString& Example : Examples)
	{
		UE_LOG(LogVoxelPerf, Log, TEXT("  %s"), *Example);
	}
}

// --- budgeted drains ----------------------------------------------------

FVoxelGpuMeshJobManager* FVoxelWorldImpl::EnsureGpuMeshJobs()
{
	if (GpuMeshJobs.IsValid())
	{
		return GpuMeshJobs.Get();
	}
	if (!bAcceptingGpuResults)
	{
		// Teardown has already disarmed the fork. Do not resurrect it.
		return nullptr;
	}

	// MaxInFlight is set deliberately high, and the reason is a measurement
	// hazard rather than a performance preference. The manager has its own
	// in-flight cap and QUEUES beyond it rather than rejecting, so with both it
	// and MaxJobsInFlight binding, a throughput number cannot be attributed to
	// either -- the same shape as the ring floor sweep that collapsed
	// throughput from 49,179 to 558 chunks and took a while to pin on the
	// floors. The subsystem's own MaxJobsInFlight is the one knob that should
	// bind; this one must not.
	// Pass the SAME resolved value the fork decision uses (-VoxelGpuMeshInFlight,
	// default in GpuMeshInFlight()), not a second literal: the two caps used to
	// both be 256 so "neither binds before the other" held by coincidence, and
	// raising the switch past the literal silently did nothing -- the exact
	// unattributable-throughput trap the comment above warns about, measured on
	// 2026-07-27 when a caps sweep moved chunks/s by 0.0 because THIS constant
	// was the binding one. One knob, both consumers.
	GpuMeshJobs = MakeUnique<FVoxelGpuMeshJobManager>(
		FVoxelGpuMeshJobComplete::CreateRaw(this, &FVoxelWorldImpl::OnGpuMeshJobComplete),
		/*InMaxInFlight*/ VoxelStreamAdmission::GpuMeshInFlight());
	// The conditions, stated as they ACTUALLY are. This line used to say
	// "level 0, unedited, band-known chunks only", which was true when it was
	// written and stopped being true twice: D6 removed the band-known
	// restriction, D5 raised the level cap. A log line describing a filter that
	// no longer exists is worse than no line -- it is what someone reads instead
	// of the code.
	UE_LOG(LogVoxelStream, Log,
	       TEXT("VoxelGpuMesh: GPU mesh fork ENABLED. Takes levels 0..%d, unedited chunks only "
	            "(NeedsOverlayAwarePath routes edits to the game thread), ring-boundary chunks "
	            "INCLUDED (the skirt is mirrored in regionCellMat), and not already predicted "
	            "empty. maxInFlight=%d (GPU), separate from the CPU worker budget."),
	       VoxelStreamAdmission::GpuMeshMaxLevel(), GpuMeshJobs->GetMaxInFlight());
	return GpuMeshJobs.Get();
}

bool FVoxelWorldImpl::SubmitGpuMeshJob(const VoxelCoords::FVoxelLevelChunkKey& LevelKey, uint64 GenId,
                                       uint8 RingSkirtMask, bool bSpeculative)
{
	FVoxelGpuMeshJobManager* Manager = EnsureGpuMeshJobs();
	if (Manager == nullptr)
	{
		return false;
	}

	FVoxelGpuRegionRequest Req;
	// The chunk footprint is LEVEL-AGNOSTIC and that is not a coincidence: the
	// coarse grid is self-similar, so a level-L chunk is still 4x4x4 bricks of
	// 8 level-L cells with a one-brick halo. SetChunkFootprint's arithmetic is
	// identical; only the UNITS of the result change, from level-0 voxels to
	// level-L cells, which is exactly what CoarseLevel tells the kernel.
	VoxelGpuChunkRegion::SetChunkFootprint(Req, LevelKey.Key.X, LevelKey.Key.Y, LevelKey.Key.Z);
	Req.Seed = Voxels.amplifier().seed();

	// BEFORE FillRasterWindow, WHICH READS IT. At level L the dispatch samples
	// over a span 2^L wider than its column count, so a window sized with
	// CoarseLevel still 0 is silently too narrow and the kernel clamps its reads
	// to the edge -- different terrain, no error. Assigning this afterwards is a
	// no-op that looks like a fix; it cost the D5 gate a full run.
	Req.CoarseLevel = LevelKey.Level;
	// D5.3. Computed on the game thread where the anchor and RingPresets are
	// live, exactly as the worker job's is, and baked into this dispatch.
	Req.RingSkirtMask = RingSkirtMask;

	// Ask for the band (D6), which is what lets the fork take a chunk whose
	// footprint has not been seen before instead of leaving every cold column to
	// the CPU.
	//
	// THE WINDOW, DERIVED RATHER THAN GUESSED. The worker job reduces over a
	// 34x34 grid starting at (ChunkX*32 - 1, ChunkY*32 - 1) -- the chunk plus a
	// one-column apron. SetChunkFootprint puts the dispatch origin at
	// ChunkX*32 - 8, one brick of halo. So the band window begins 7 columns into
	// the dispatch on both axes, and 7 + 34 = 41 <= 48, which is why
	// ValidateRegionRequest accepts it.
	//
	// Getting this offset wrong would not fault: the kernel would reduce over a
	// DIFFERENT 34x34 patch of real terrain and return a band that is plausible,
	// self-consistent, and wrong for this footprint -- and wrong in whichever
	// direction the neighbouring terrain happens to lie. The gate's
	// "production-shaped window" probes exist for exactly this arithmetic.
	static constexpr uint32 kBandApronOffset =
		VoxelGpuChunkRegion::kBrickEdge - 1;                          // 8 - 1 = 7
	static constexpr uint32 kBandGridEdge = VoxelCoords::ChunkEdgeVoxels + 2;  // 34
	static_assert(kBandApronOffset + kBandGridEdge <= VoxelGpuChunkRegion::kColumns,
	              "band window must fit inside the chunk dispatch");
	// LEVEL 0 ONLY. FootprintBandCache is a level-0 structure -- the cold-band
	// throttle and both admission skips are keyed on a level-0 footprint -- and
	// DrainResults only consults a band for level-0 results. Asking a coarse
	// dispatch for one would reduce over level-L cells and produce a band in the
	// wrong units, which is worse than not having one.
	if (LevelKey.Level == 0)
	{
		Req.BandOriginI = kBandApronOffset;
		Req.BandOriginJ = kBandApronOffset;
		Req.BandEdge = kBandGridEdge;
	}
	// The one place the raster-window arithmetic may live -- see
	// VoxelGpuRegionBuild's header. Undersizing it does not fault; the kernel
	// clamps to the window edge and silently produces different terrain.
	VoxelGpuRegionBuild::FillRasterWindow(Req, *Tiles);

	// --- Wave D / D1: may this chunk's quads stay on the GPU? ----------------
	//
	// DECIDED HERE, AT DISPATCH, AND LATCHED. The alternative -- deciding at
	// delivery -- would let the answer change under a job in flight, and the two
	// forms are not interchangeable: a direct result carries no quads, so a path
	// that expected them would read the chunk as empty rather than as an error.
	// Same reasoning as the manager latching voxel.GPU.MeshChunkLocal.
	//
	// Three conditions, and each one is a place the quads would be needed on the
	// CPU:
	//
	//  1. THE POOL MUST BE THE DESTINATION. voxel.Stream.GPU picks the renderer
	//     and voxel.Stream.GPUMaxLevel picks which rings it takes; the component
	//     path calls SetChunkQuads, which needs the geometry itself. The pool
	//     must also already be GPU-writable -- its persistent buffers are created
	//     by the FIRST scene proxy, so the opening chunks of a session
	//     necessarily arrive by readback and everything after them goes direct.
	//     That bootstrap is self-healing and deliberately has no special case.
	//
	//  2. THE RECORD MUST NOT ALREADY HOLD A COMPONENT. A chunk always unloads
	//     the way it loaded, so a record on the component path stays there.
	//
	//  3. GI MUST NOT WANT IT. UVoxelGISubsystem::NotifyPooledChunkMeshUpdated is
	//     the last consumer anywhere that reads quad CONTENTS on the CPU. See
	//     WantsChunkQuads for why asking it is a correctness condition rather
	//     than an optimisation.
	//
	// Every one of them is conservative in the same direction: unsure means
	// readback, which costs bandwidth and latency, never geometry.
	bool bDirectToPool = false;
	{
		static const auto* CVarGpuMaxLevel =
			IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Stream.GPUMaxLevel"));
		const int32 GpuMaxLevel = CVarGpuMaxLevel ? CVarGpuMaxLevel->GetInt() : -1;
		const bool bLevelPoolable = (GpuMaxLevel < 0) || (LevelKey.Level <= GpuMaxLevel);

		const UVoxelGpuPoolComponent* Pool = GpuPool.Get();
		const VoxelStreaming::FChunkRecord* Rec = ChunkRecords.Find(LevelKey);

		bDirectToPool =
			VoxelDebug::GetStreamGpu()
			&& bLevelPoolable
			&& Pool != nullptr
			&& Pool->IsGpuWritable()
			&& (Rec == nullptr || !Rec->Component.IsValid());

		// The GI test is last because it is the only one that costs a distance
		// computation, and because it needs the chunk's world origin -- composed
		// exactly the way ApplyMeshResult composes it, from the same root. With
		// GI off this is one cvar read and nothing else.
		const USceneComponent* PoolRoot = GpuPoolRoot.Get();
		if (bDirectToPool && PoolRoot != nullptr && VoxelGI::IsEnabled())
		{
			if (const UWorld* World = PoolRoot->GetWorld())
			{
				if (const UVoxelGISubsystem* GI = World->GetSubsystem<UVoxelGISubsystem>())
				{
					const FVector OriginWorld = PoolRoot->GetComponentLocation()
						+ VoxelCoords::ChunkOriginWorldForLevel(LevelKey.Key, LevelKey.Level);
					bDirectToPool = !GI->WantsChunkQuads(OriginWorld, LevelKey.Level);
				}
			}
		}
	}

	const uint64 JobId = Manager->Submit(MoveTemp(Req), /*UserTag*/ 0, bDirectToPool,
	                                     /*bLowPriority*/ bSpeculative);
	GpuJobsPending.Add(JobId, FGpuPendingJob{ LevelKey, GenId, bSpeculative });
	return true;
}

void FVoxelWorldImpl::OnGpuMeshJobComplete(FVoxelGpuMeshJobResult&& GpuResult)
{
	// Teardown absorbs rather than acts. See WaitForInFlightTasks for why this
	// has to come before anything that touches Impl state.
	if (!bAcceptingGpuResults)
	{
		++GpuResultsAbsorbedAtTeardown;
		GpuJobsPending.Remove(GpuResult.JobId);
		return;
	}

	FGpuPendingJob Pending;
	if (!GpuJobsPending.RemoveAndCopyValue(GpuResult.JobId, Pending))
	{
		// Cannot happen: the runner delivers exactly once per Submit and every
		// Submit inserts here. Logged as an Error rather than checked, because
		// the consequence is a LEAKED SLOT -- the counters below never
		// decrement, MaxJobsInFlight fills with ghosts and the whole ring stops
		// dispatching. That is a hang, and a silent one, so it must be loud.
		UE_LOG(LogVoxelStream, Error,
		       TEXT("VoxelGpuMesh: delivery for unknown job id %llu (status=%s). A dispatch slot has leaked."),
		       (unsigned long long)GpuResult.JobId, LexToString(GpuResult.Status));
		return;
	}

	++GpuMeshJobsDeliveredSinceLog;
	GpuMeshSubmitToDeliverMsSinceLog += GpuResult.SubmitToDeliverMs;
	GpuMeshSubmitToDeliverMaxMs = FMath::Max(GpuMeshSubmitToDeliverMaxMs, GpuResult.SubmitToDeliverMs);

	// S0-3: the per-stage split. QueuedMs/ReadyToDeliverMs are 0.0 unless
	// voxel.GPU.MeshLatencyStats is also on (see FVoxelGpuMeshJobResult's doc
	// comment) -- pushing a real 0.0 into the window in that case would read as
	// "this stage is free" rather than "not measured", so this is gated on the
	// SAME cvar the streaming side uses and skips entirely when off, rather
	// than pushing zeroes.
	if (VoxelDebug::GetStreamLatencyStats())
	{
		PushLatencyMsSample(GpuQueuedMsWindow, GpuQueuedMsWindowNext, GpuQueuedMsWindowCount,
		                    float(GpuResult.QueuedMs));
		PushLatencyMsSample(GpuDispatchToReadyMsWindow, GpuDispatchToReadyMsWindowNext, GpuDispatchToReadyMsWindowCount,
		                    float(GpuResult.DispatchToReadyMs));
		PushLatencyMsSample(GpuReadyToDeliverMsWindow, GpuReadyToDeliverMsWindowNext, GpuReadyToDeliverMsWindowCount,
		                    float(GpuResult.ReadyToDeliverMs));
		PushLatencyMsSample(GpuSubmitToDeliverMsWindow, GpuSubmitToDeliverMsWindowNext, GpuSubmitToDeliverMsWindowCount,
		                    float(GpuResult.SubmitToDeliverMs));
	}

	// DELIVERIES SLOWER THAN THE COLD-BAND BACKSTOP, counted directly.
	//
	// gpuLatencyTimeouts cannot see these, and that is not a bug in it so much
	// as a limit nobody had stated. It increments when a cold-band MARK ages
	// out, and a mark only exists for a footprint whose band was not already
	// cached. Once FootprintBandCache is warm -- 5,088 entries in the 128 m runs
	// -- almost no footprint carries a mark, so an arbitrarily slow delivery
	// passes without touching that counter at all.
	//
	// Which is exactly what happened: a 7,395 ms submit->deliver was measured on
	// a run whose gpuLatencyTimeouts read 0 for its entire length. "The fork is
	// healthy" then rests on a number that was never in a position to disagree.
	// This one is.
	if (GpuResult.SubmitToDeliverMs >= kBlindJobMarkTimeoutSeconds * 1000.0)
	{
		++GpuMeshSlowDeliveriesSinceLog;
	}

	// T4-1: a SPECULATIVE result never enters the results queue. Nothing asked
	// for this chunk, so it must not create a record, must not become visible,
	// and must not consume the apply budget demand work is competing for. It
	// goes straight into the pool, hidden, and waits for admission to find it.
	if (Pending.bSpeculative)
	{
		SpeculativeInFlight.Remove(Pending.Key);
		// KEEP THE BAND EVEN THOUGH THE GEOMETRY MAY BE DISCARDED.
		//
		// The GPU computes a per-COLUMN band (D6) alongside the mesh, and
		// speculation was throwing it away -- it returned straight to
		// ParkSpeculativeResult, which never touches FootprintBandCache. That is
		// why applying admission's buried skip to the speculative enumerator
		// measured bandSkipped=0: the skip consults a cache that speculation
		// consumes GPU to produce and then drops on the floor.
		//
		// One band covers the whole Z stack of its column, so storing it here
		// lets the NEXT tick's enumeration skip every remaining buried or air
		// slot in that column instead of dispatching each one. This is also the
		// only way the skip can ever fire on virgin terrain, which is precisely
		// what speculation looks at.
		if (Pending.Key.Level == 0)
		{
			FootprintBandCache.Add(FIntPoint(Pending.Key.Key.X, Pending.Key.Key.Y),
			                       VoxelStreaming::MakeFootprintBand(GpuResult.BandMaxSurfaceTopVoxel,
			                                                         GpuResult.BandMinDeepestAirVoxel));
		}
		// TIMED. This runs inside Manager->Tick(), which the tick-budget line
		// attributes to dispatch= -- so unlike a demand result (which only gets
		// enqueued here and is applied later under apply=), speculation does its
		// POOL WORK synchronously in the completion callback. That asymmetry is
		// the leading explanation for T4-1's +260 ms/window: speculation's own
		// enumerate and submit measured 0.7 ms and 0.6 ms, so the cost is not
		// where it was first looked for.
		const double ParkT0 = FPlatformTime::Seconds();
		ParkSpeculativeResult(Pending.Key, GpuResult);
		AccumSpecParkMs += (FPlatformTime::Seconds() - ParkT0) * 1000.0;
		return;
	}

	VoxelStreaming::FJobResult Result;
	Result.Key = Pending.Key;
	Result.GenerationId = Pending.GenerationId;
	Result.JobMs = float(GpuResult.SubmitToDeliverMs);
	Result.bFromGpuMesh = true;
	// S0-3: DELIVER time, i.e. now -- the moment this result is about to go on
	// ResultsQueue. Gated with the rest of S0-3's bookkeeping; see
	// FJobResult::DeliverSeconds for why a stats-off run must leave this at
	// 0.0 rather than paying the call for a number nobody reads.
	if (VoxelDebug::GetStreamLatencyStats())
	{
		Result.DeliverSeconds = FPlatformTime::Seconds();
	}

	// The band, when the GPU produced one (D6). This is what lets the fork seed
	// a cold footprint rather than leaving every cold column to the CPU.
	//
	// MakeFootprintBand is the SAME function the worker job calls, deliberately:
	// the +1/-1 widening and the clamp stay on this side, so the GPU supplies
	// two raw extrema and nothing about how a band is formed is mirrored into
	// HLSL. Three producers now share it (worker job, this, and the gate), and
	// that is exactly the drift this arrangement prevents.
	//
	// bBandValid is false rather than {0,0} when absent, because a zero band
	// claims the footprint is EMPTY -- the unsafe direction.
	Result.bBandValid = GpuResult.bBandValid;
	if (GpuResult.bBandValid)
	{
		Result.Band = VoxelStreaming::MakeFootprintBand(GpuResult.BandMaxSurfaceTopVoxel,
		                                               GpuResult.BandMinDeepestAirVoxel);
	}

	if (GpuResult.IsOk())
	{
		if (GpuResult.GpuQuads.IsValid())
		{
			// Wave D / D1. The quads never left the GPU, so there is nothing to
			// unpack -- which is the point: this loop and ApplyMeshResult's
			// matching PackVoxelChunkQuad loop were a provably lossless round
			// trip (VoxelGpuVerify.cpp asserts Pack(Unpack(P)) == P) that existed
			// only because two APIs disagreed on a type. ~1,300 iterations and
			// two ~10 KB allocations per chunk, on the game thread, to arrive at
			// the bytes we started with.
			Result.GpuQuads = MoveTemp(GpuResult.GpuQuads);
			Result.GpuQuadCount = GpuResult.NumQuads;
		}
		else
		{
			Result.Quads.Reserve(GpuResult.Quads.Num());
			for (const uint64 Packed : GpuResult.Quads)
			{
				Result.Quads.Add(UnpackVoxelChunkQuad(Packed));
			}
		}
	}
	else
	{
		++GpuMeshJobsFailedSinceLog;
		// A failed job still owes exactly one result, and it delivers an EMPTY
		// one. That is the honest outcome -- but it is also indistinguishable
		// on screen from terrain that is genuinely empty, which is why it is
		// counted and logged rather than merely returned.
		UE_LOG(LogVoxelStream, Warning,
		       TEXT("VoxelGpuMesh: job %llu for chunk (%d, %d, %d) L%d ended %s -- delivering an EMPTY chunk. %s"),
		       (unsigned long long)GpuResult.JobId,
		       Pending.Key.Key.X, Pending.Key.Key.Y, Pending.Key.Key.Z, Pending.Key.Level,
		       LexToString(GpuResult.Status), *GpuResult.Error);
	}

	// The matching half of the fork's contract: exactly one FJobResult on the
	// queue and exactly one decrement, in the same order the worker thread does
	// them. LevelJobsInFlight[] is NOT touched here -- it is decremented in
	// DrainResults for both producers alike, which is what keeps the GPU path
	// an exact analogue rather than a second set of rules.
	ResultsQueue.Enqueue(MoveTemp(Result));
	JobsInFlightCounter.Decrement();
}

void FVoxelWorldImpl::DispatchJobs()
{
	// docs/m1-plan.md Stage 2 decisions table: "<=2xLogicalCores jobs in
	// flight." The multiplier is now voxel.Stream.JobsInFlightPerCore (default
	// 2, i.e. byte-identical to the old hardcoded form) -- see that cvar's
	// comment in VoxelDebug.cpp for the dispatch-starvation measurement.
	//
	// SIZING THIS AGAINST A GPU JOB IS A DIFFERENT PROBLEM (Wave D / D4), and
	// the per-core form is the wrong shape for it. JobsInFlightCounter means
	// "work outstanding" and is decremented when the job's RESULT is produced,
	// not when it is dispatched -- true on the worker path today (the decrement
	// sits one line after ResultsQueue.Enqueue) and deliberately kept true on
	// the GPU path, where the analogous site is the manager's OnJobComplete.
	//
	// The consequence is that this cap must absorb GPU LATENCY as well as work.
	// A worker job's lifetime is its own CPU time; a GPU job's is the manager's
	// queue wait plus the dispatch plus TWO readback phases plus one
	// game-thread tick of poll quantisation per phase. So the cap that
	// saturates the GPU is a function of LATENCY x RATE, not of core count, and
	// a value tuned for 24 workers will starve a pipeline whose jobs take
	// frames rather than milliseconds.
	//
	// Do NOT retune it and the ring slot floors in the same change. The
	// recorded catastrophe is exactly that: floors {0,2,3,4,4} reserved 13 of
	// 24 slots and collapsed throughput from 49,179 chunks to 558, and with two
	// knobs moving nobody could say which had done it.
	const int32 MaxJobsInFlight =
		VoxelDebug::GetStreamJobsInFlightPerCore() * FPlatformMisc::NumberOfCoresIncludingHyperthreads();
	const bool bRingQuota = VoxelStreamAdmission::GetRingQuotaEnabled();

	// Cold-band throttle (see the block after the record lookup below). Hoisted:
	// loop-invariant, and the check runs on every popped level-0 candidate.
	const bool bBandSkipActive =
		(VoxelStreamAdmission::BuriedSkipEnabled() || VoxelStreamAdmission::VerifyBuriedSkipEnabled()) &&
		VoxelStreamAdmission::ColdBandThrottleEnabled();
	TArray<FSortEntry> DeferredColdBand;

	// THE GPU FORK GETS ITS OWN IN-FLIGHT BUDGET, AND THIS IS THE FIX FOR THE
	// 3.4x COLD-FILL REGRESSION D5.3a MEASURED.
	//
	// MaxJobsInFlight is JobsInFlightPerCore * logical cores -- 24 on this box.
	// That number sizes a pool of WORKERS: 24 threads each finishing a chunk in
	// about a millisecond is ~24,000 chunks/s of capacity. It is the wrong unit
	// for the fork. A GPU job is not a thread, it is a round trip, and at the
	// measured ~28 ms mean a budget of 24 caps the fork near ~860 chunks/s no
	// matter how idle the GPU is. Wave D's design pass said this in advance --
	// "MaxJobsInFlight must now absorb latency as well as work, so it sizes
	// against latency x rate, not worker count" -- and D5.3a then measured the
	// fork sitting at 19-20 in flight against a manager cap of 256, waiting
	// rather than saturated, with cold fill going 58.4 s to 205.4 s.
	//
	// SO GPU JOBS DO NOT CONSUME THE CPU BUDGET AT ALL. The loop condition
	// counts only CPU-outstanding work, which leaves the CPU baseline EXACTLY
	// what it was -- one knob at a time -- and lets the loop keep feeding the
	// fork while worker slots are busy.
	//
	// The fork's own bound is applied at the fork decision instead of here, so
	// that a chunk arriving when the GPU is full falls back to the CPU rather
	// than stalling the loop. Both bounds are therefore explicit and neither
	// hides the other: two caps in series make a throughput number
	// unattributable to either, which is the trap the ring floor sweep fell
	// into.
	const int32 GpuMaxInFlight = VoxelStreamAdmission::GpuMeshInFlight();
	const auto CpuJobsOutstanding = [&]() -> int32
	{
		return FMath::Max(0, JobsInFlightCounter.GetValue() - GpuJobsPending.Num());
	};

	while (CpuJobsOutstanding() < MaxJobsInFlight)
	{
		// Which ring gets this worker slot.
		//
		// Pass 1 (quota only): any ring that has work pending AND is below its
		// guaranteed slot floor takes the slot ahead of nearer work from any
		// other ring, most-starved ring first. This is a FLOOR, not a
		// partition, and it is the half of the fix that matters with the cap
		// OFF: strict nearest-first meant R0 -- whose footprint is a 38.4 m deep
		// column since underground streaming landed, so its chunks are nearer in
		// 3D than any R1+ surface chunk -- monopolised every worker forever,
		// leaving R3/R4 at 0 dispatched with 10.7k queued across a 60 s run.
		//
		// Pass 2: every ring at or above its floor, so fill strictly
		// nearest-first across the queue heads. Because each queue is sorted and
		// stores its distance, comparing the five heads is exactly the ordering
		// the old single flat queue produced -- including the "lower level wins
		// at equal distance" tie-break, which falls out of scanning levels in
		// ascending order with a strict <.
		int32 PickLevel = INDEX_NONE;
		if (bRingQuota)
		{
			const int32* const Floors = VoxelStreamAdmission::GetRingSlotFloors();
			int32 BestDeficit = 0;
			for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
			{
				if (PendingJobKeysByLevel[Level].Num() == 0)
				{
					continue;
				}
				const int32 Deficit = Floors[Level] - LevelJobsInFlight[Level];
				if (Deficit > BestDeficit)
				{
					BestDeficit = Deficit;
					PickLevel = Level;
				}
			}
		}
		if (PickLevel == INDEX_NONE)
		{
			double BestDistSq = 0.0;
			for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
			{
				const TArray<FSortEntry>& Queue = PendingJobKeysByLevel[Level];
				if (Queue.Num() == 0)
				{
					continue;
				}
				const double HeadDistSq = Queue.Last().DistSq; // nearest in this ring
				if (PickLevel == INDEX_NONE || HeadDistSq < BestDistSq)
				{
					BestDistSq = HeadDistSq;
					PickLevel = Level;
				}
			}
		}
		if (PickLevel == INDEX_NONE)
		{
			break; // nothing pending in any ring
		}

		// Keep the whole entry, not just the key: the cold-band throttle below may
		// re-queue it, and re-queuing must preserve its DistSq priority.
		const FSortEntry PoppedEntry =
			PendingJobKeysByLevel[PickLevel].Pop(EAllowShrinking::No); // highest priority in that ring (see SortPendingQueues)
		const VoxelCoords::FVoxelLevelChunkKey LevelKey = PoppedEntry.Key;

		VoxelStreaming::FChunkRecord* Rec = ChunkRecords.Find(LevelKey);
		if (!Rec)
		{
			continue; // left the desired set between recompute and dispatch
		}

		// --- Cold-band throttle: ONE blind job per footprint at a time -------
		//
		// The buried-skip band below can only fire once SOME level-0 job in this
		// (X,Y) footprint has completed AND drained (FootprintBandCache is
		// populated in DrainResults). Until then the footprint is COLD, and every
		// chunk of its ~16-chunk column pops off the nearest-first queue and
		// dispatches BLIND. Measured on a moving flight: ~3.8 blind dispatches per
		// footprint, ~90% of which turn out to be solid rock emitting nothing.
		//
		// It is a pure tax on MOVEMENT -- standing still it is invisible because
		// every band is already warm. That is exactly the asymmetry reported:
		// correct and fast at rest, holes and a long catch-up as soon as you move.
		//
		// The fix costs nothing, because the column grid a job builds is a
		// function of XY ONLY (Key.Z never enters it), so ANY one chunk of the
		// column yields the band for ALL of them. Dispatch exactly one and hold
		// the column-mates back; they are then skipped or dispatched on knowledge
		// instead of on a guess.
		//
		// Placed BEFORE NeedsOverlayAwarePath deliberately: that call is a live
		// overlay scan of the chunk plus a brick of border, and a cold column can
		// defer hundreds of chunks per frame -- paying for that scan on every one
		// would cost more than the dispatches this saves.
		//
		// Held chunks are re-queued after the pop loop, so this REORDERS work and
		// never drops it. The in-flight mark is cleared in DrainResults for every
		// level-0 result, band or not, so a stale result cannot strand a column.
		// NOTE the mark is NOT set here -- only checked. Setting it at pop time
		// was a bug: a popped chunk can still leave via NeedsOverlayAwarePath or
		// either pre-dispatch skip WITHOUT launching a worker job, so no level-0
		// result would ever arrive to clear the mark and the whole column stayed
		// deferred forever (observed as blindInFlight pinned at 92 and useful
		// throughput DOWN 15%). The mark is set at the actual launch site.
		if (LevelKey.Level == 0 && bBandSkipActive &&
		    !FootprintBandCache.Contains(FIntPoint(LevelKey.Key.X, LevelKey.Key.Y)))
		{
			const FIntPoint Footprint(LevelKey.Key.X, LevelKey.Key.Y);
			if (const double* MarkedAt = FootprintBlindJobInFlight.Find(Footprint))
			{
				if (ElapsedSeconds - *MarkedAt >= kBlindJobMarkTimeoutSeconds)
				{
					// Stale: the seeding job never produced a result. Drop the
					// mark and let this chunk through as the new seed rather
					// than deferring its column forever.
					//
					// Split by which kind of job seeded it. A GPU job that is
					// simply slower than 5 s is not the bug this backstop
					// exists for, and counting it in the same bucket would
					// destroy the "should be ZERO on a healthy run" property
					// that is the only signal for the real one.
					FootprintBlindJobInFlight.Remove(Footprint);
					if (FootprintBlindJobIsGpu.Remove(Footprint) > 0)
					{
						++ColdBandGpuLatencyTimeoutsSinceLog;
					}
					else
					{
						++ColdBandMarkTimeoutsSinceLog;
					}
				}
				else
				{
					DeferredColdBand.Add(PoppedEntry);
					++ColdBandDefersSinceLog;
					continue;
				}
			}
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

		// --- Buried-chunk pre-dispatch skip ---------------------------------
		//
		// If this footprint's band is already known (some earlier level-0 job
		// in the same (X,Y) returned it) and it proves this chunk can hold no
		// visible face, do not dispatch a job at all. The record STAYS in
		// ChunkRecords with no component, which is byte-for-byte the state
		// ApplyMeshResult's Quads.Num()==0 branch would have left it in after
		// a full generate+mesh -- so nothing downstream can tell the
		// difference, and RecomputeDesiredSet will not re-queue it (candidates
		// are admitted once, guarded by ChunkRecords.Contains).
		//
		// EDITS. This point is already past the NeedsOverlayAwarePath gate
		// immediately above, which AT LEVEL 0 IS exactly
		// ChunkHasEditedBrick(Key) -- an overlay scan of the chunk plus one
		// brick of border on every axis. So any chunk that an edit could have
		// made non-uniform, including a pristine chunk merely NEXT TO an
		// edited brick, has already been routed to the game-thread path and
		// can never reach here. The band itself is pure worldgen and knows
		// nothing about edits; the veto is this gate, and it is the same gate
		// the pre-existing worker path already trusted for correctness.
		//
		// LEVEL 0 ONLY. A level-L (L>=1) chunk spans 2^L x 2^L level-0
		// footprints and the mip path samples columns at stride 2^L rather
		// than building a dense grid, so no exact band is available for it --
		// see the follow-up in docs/status.md.
		// The band is only useful if something will consult it.
		const bool bComputeBand =
			VoxelStreamAdmission::BuriedSkipEnabled() || VoxelStreamAdmission::VerifyBuriedSkipEnabled();
		bool bPredictedEmpty = false;
		if (bComputeBand && LevelKey.Level == 0)
		{
			if (const VoxelStreaming::FFootprintBand* Band = FootprintBandCache.Find(FIntPoint(LevelKey.Key.X, LevelKey.Key.Y)))
			{
				// Shared with the admission-time skip -- see BandProvesChunkEmpty.
				bool bAllAir = false;
				if (VoxelStreaming::BandProvesChunkEmpty(*Band, LevelKey.Key.Z, bAllAir))
				{
					bPredictedEmpty = true;
					++BuriedSkipsSinceLog;
					BuriedSkipsByLevelSinceLog[0] += 1;
					(bAllAir ? BuriedSkipAirSinceLog : BuriedSkipSolidSinceLog) += 1;
				}
			}
		}
		// Under -VoxelVerifyBuriedSkip the verdict is computed and counted
		// exactly as above but never acted on: the job runs, and DrainResults
		// checks the verdict against the mesh it produced.
		if (bPredictedEmpty && VoxelStreamAdmission::BuriedSkipEnabled() && !VoxelStreamAdmission::VerifyBuriedSkipEnabled())
		{
			// Defensive: a first dispatch always has a null component, but keep
			// ResidentQuads exact if that ever stops holding.
			ResidentQuads -= Rec->LastQuadCount;
			Rec->LastQuadCount = 0;
			ReleaseChunkGeometry(*Rec);
			Rec->bJobInFlight = false;
			// Proven empty and never dispatched, so no ApplyMeshResult will ever
			// run for it -- settle it HERE or it blocks a retained stand-in above
			// or below until the safety cap expires. (This site and the sky-band
			// one below are the two the previous column-index version forgot,
			// which docs/streaming-handoff.md flagged as the prime suspect.)
			Rec->bMeshSettled = true;
			continue;
		}

		// --- Sky-band pre-dispatch skip (outer rings) ------------------------
		//
		// The band skip above is exact but LEVEL 0 ONLY, for the reason its own
		// comment gives: a level-L chunk spans 2^L x 2^L level-0 footprints and
		// never has a level-0 band. That is where the remaining waste lives --
		// an R4 chunk costs ~6,000 ms p50 to mesh, and the outer rings are
		// overwhelmingly sky.
		//
		// The all-air half of the band test needs far less than the general
		// case, and that is what makes it expressible at every level: proving a
		// chunk holds no geometry requires only an UPPER BOUND on the terrain
		// surface over its footprint -- no cave data, no cavern data, no
		// bedrock, no level-0 bricks -- because Amplifier::materialAt is
		// unconditionally MAT_AIR above surfaceMm (caves and caverns only ever
		// CARVE; nothing fills above the surface). IsChunkProvablyAllAir derives
		// that bound analytically; see namespace VoxelSkyBand.
		//
		// This runs AFTER the band skip so that at level 0, where both apply,
		// the exact band keeps its verdict (and its all-solid half, which no
		// surface bound can reproduce). It errs the same way: it may decline to
		// skip an empty chunk, never the reverse.
		//
		// EDITS. Same veto, and deliberately the same one: NeedsOverlayAwarePath
		// above has already routed any chunk carrying an edit -- including a
		// block TryPlace put in what worldgen calls sky -- to the game-thread
		// overlay path, so it can never reach here.
		if (VoxelSkyBand::GetSkipEnabled() && IsChunkProvablyAllAir(LevelKey))
		{
			if (VoxelSkyBand::GetVerifyEnabled())
			{
				// Verification mode, same shape as -VoxelVerifyBuriedSkip above:
				// record the verdict and DISPATCH ANYWAY, so DrainResults can
				// check it against the mesh the worker really produced.
				VerifyPredictedAirKeys.Add(LevelKey);
			}
			else
			{
				++LevelSkySkippedTotal[PickLevel];
				// Byte-for-byte the state ApplyMeshResult's Quads.Num()==0 branch
				// leaves behind, exactly as the band skip above does.
				ResidentQuads -= Rec->LastQuadCount;
				Rec->LastQuadCount = 0;
				ReleaseChunkGeometry(*Rec);
				Rec->bJobInFlight = false;
				Rec->bMeshSettled = true; // see the band-skip site above
				continue;
			}
		}

		// -VoxelL0GridCacheProbe: this job is definitely going to build a 34x34
		// column grid for footprint (X,Y). Probe the simulated LRU here, at the
		// last point before launch, so the stream it sees is exactly the stream a
		// real cross-job cache would see -- every skip above has already fired.
		if (LevelKey.Level == 0)
		{
			if (const int32 ProbeCap = VoxelStreamAdmission::L0GridCacheProbeEntries(); ProbeCap > 0)
			{
				const uint64 Footprint = (uint64(uint32(LevelKey.Key.X)) << 32) | uint64(uint32(LevelKey.Key.Y));
				const bool bNewThisWindow = (L0ProbeDistinct.Find(Footprint) == nullptr);
				L0ProbeDistinct.Add(Footprint);
				if (L0ProbeResident.Contains(Footprint))
				{
					++L0ProbeHitsSinceLog;
					L0ProbeRecency.RemoveSingle(Footprint);
					L0ProbeRecency.Add(Footprint);
				}
				else
				{
					++L0ProbeMissesSinceLog;
					L0ProbeColdMissesSinceLog += bNewThisWindow ? 1 : 0;
					if (L0ProbeRecency.Num() >= ProbeCap)
					{
						L0ProbeResident.Remove(L0ProbeRecency[0]);
						L0ProbeRecency.RemoveAt(0);
					}
					L0ProbeResident.Add(Footprint);
					L0ProbeRecency.Add(Footprint);
				}
			}
		}

		Rec->bJobInFlight = true;
		JobsInFlightCounter.Increment();
		++LevelJobsInFlight[PickLevel];
		++JobsDispatchedSinceLog;
		++LevelJobsDispatchedSinceLog[PickLevel];
		++LevelJobsDispatchedTotal[PickLevel];

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
		// Ring-boundary skirt mask -- computed here on the game thread where the
		// anchor and RingPresets are live, then baked into this job's mesh.
		const uint8 RingSkirtMask = ComputeRingSkirtMask(LevelKey, LastAnchorLocation);

		// --- The GPU fork (Wave D / D4) -------------------------------------
		//
		// Everything above this point has already happened for this chunk: the
		// record is marked in flight, both counters are incremented, and the
		// dispatch tallies are bumped. So the fork chooses only WHO MESHES IT,
		// and both branches owe exactly one FJobResult on ResultsQueue plus one
		// JobsInFlightCounter decrement. That symmetry is the whole safety
		// argument -- see the invariant quoted in DrainResults.
		//
		// THE BAND SEED USED TO BE EXCLUDED HERE, AND IS NOT ANY MORE.
		//
		// A level-0 worker job also reduces the footprint's FFootprintBand out
		// of the column grid it already built, and that band feeds two admission
		// skips AND the cold-band throttle. When the fork could not produce one,
		// it had to leave every cold footprint to the CPU -- and Wave F measured
		// what that cost: at cold fill almost every level-0 chunk IS a seed, so
		// the fork excluded precisely the population that dominates the number
		// it was meant to improve, and 128 m cold fill was unchanged (60.4 s
		// against 58.4 s, one log window at a 2 s sampling interval).
		//
		// D6's reduction kernel has now EXECUTED and passes its sweep, so
		// SubmitGpuMeshJob asks for a band and OnGpuMeshJobComplete hands it to
		// DrainResults through the same MakeFootprintBand the worker job uses.
		// The fork can therefore seed a cold column, and this condition is gone.
		//
		// WHAT IS STILL TRUE AND STILL LIMITS IT: the gate's cave coverage. The
		// sweep now searches all 4,096 columns of each fixture rather than the
		// 64 on its diagonal, which is what makes the cave-segment loop, the
		// shaft term, the bedrock clamps and ceilSqrt reachable at all. If a
		// fixture reports no cave column anywhere, the sweep says so out loud
		// and the band's cave path is untested on that terrain -- read the PASS
		// lines accordingly before trusting this in production.
		const bool bUseGpuMesh =
			VoxelStreamAdmission::GpuMeshEnabled()
			&& LevelKey.Level <= VoxelStreamAdmission::GpuMeshMaxLevel()
			// THE SKIRT IS NOW ON THE GPU (D5.3), so boundary chunks are no
			// longer excluded. regionCellMat applies the same mask the CPU
			// sampler does, against the same chunk interior, and
			// ValidateRegionRequest refuses a mask on any region that is not
			// exactly one chunk -- which is what makes a scalar sound rather
			// than sound-until-batching.
			//
			&& !bPredictedEmpty           // the band already says this meshes to nothing; do not pay a dispatch
			// The fork's own bound. Applied HERE rather than in the loop
			// condition so a chunk arriving when the GPU is full falls back to
			// the CPU instead of stalling the whole dispatch loop.
			&& GpuJobsPending.Num() < GpuMaxInFlight;

		if (bUseGpuMesh && SubmitGpuMeshJob(LevelKey, GenId, RingSkirtMask))
		{
			++GpuMeshJobsDispatchedSinceLog;
			++GpuMeshDispatchedByLevel[FMath::Clamp(LevelKey.Level, 0, VoxelCoords::kNumLevels - 1)];
			// Stage-0 tile-batching census (see TileCensusSinceLog doc comment):
			// this dispatch just took the GPU fork branch, so tally it under the
			// 4x4-lattice tile it would join if DispatchJobs batched by tile.
			// Measurement only -- does not change which branch this chunk took.
			++TileCensusSinceLog.FindOrAdd(
				FIntVector(LevelKey.Level, LevelKey.Key.X >> 2, LevelKey.Key.Y >> 2));
			// Deliberately NOT added to InFlightTasks: a GPU job has no
			// UE::Tasks::TTask. WaitForInFlightTasks covers it separately, and
			// says why at length.
			//
			// The cold-band mark below still applies -- a GPU job is in flight
			// for this footprint exactly as a worker job would be -- so fall
			// through to it rather than continuing the loop.
			if (LevelKey.Level == 0 && bBandSkipActive)
			{
				const FIntPoint SeedFootprint(LevelKey.Key.X, LevelKey.Key.Y);
				if (!FootprintBandCache.Contains(SeedFootprint))
				{
					FootprintBlindJobInFlight.Add(SeedFootprint, ElapsedSeconds);
					FootprintBlindJobIsGpu.Add(SeedFootprint);
				}
			}
			continue;
		}

		// S0-3 (docs/speculative-generation-plan.md Wave S0 / T0-2): read on the
		// GAME THREAD, same as bComputeBand above, and captured BY VALUE into
		// the task -- the worker body below runs on a background task thread,
		// and VoxelDebug's cvar accessors are GetValueOnGameThread(), not safe
		// to call from there. This is the established pattern in this loop for
		// getting a gate's value into the task body (bPredictedEmpty,
		// bComputeBand are the same shape).
		const bool bLatencyStatsEnabled = VoxelDebug::GetStreamLatencyStats();

		UE::Tasks::TTask<void> Task = UE::Tasks::Launch(
			TEXT("VoxelChunkMeshJob"),
			[GenPtr, LevelKey, GenId, QueuePtr, CounterPtr, PerfCountersPtr, SharedMipCachePtr, EditEpochPtr, EditEpochSnapshot,
			 bPredictedEmpty, bComputeBand, bLatencyStatsEnabled, RingSkirtMask]()
			{
				SCOPE_CYCLE_COUNTER(STAT_VoxelWorkerJob);
				const double JobStartSeconds = FPlatformTime::Seconds();
				// Paired with JobStartSeconds: wall time and retired cycles for
				// the SAME interval, which is the whole point (FJobResult::JobCycles).
				const uint64 JobStartCycles = VoxelThreadCycles();

				VoxelStreaming::FJobResult Result;
				Result.Key = LevelKey;
				Result.GenerationId = GenId;
				Result.bPredictedEmpty = bPredictedEmpty;

				const VoxelCoords::FVoxelChunkKey& Key = LevelKey.Key;
				const bool bMeasureEmpty = VoxelStreamAdmission::MeasureEmptyEnabled();
				// Classify a zero-quad chunk over the SAME domain the mesher
				// reads: the chunk interior plus its 1-voxel apron. The mesher
				// emits a face only where a solid voxel has an AIR neighbour
				// (voxelcore/mesher.h -- material boundaries emit nothing), so
				// "no SOLID in the interior" and "no AIR anywhere in
				// chunk+apron" are each individually sufficient for zero quads.
				// Anything else that still meshed to zero is class 3 and is the
				// interesting residue.
				const auto ClassifyEmpty = [](const auto& Sampler, const VoxelCoords::FVoxelChunkKey& K) -> uint8
				{
					constexpr int32 ChunkVox = VoxelCoords::ChunkEdgeVoxels;
					const int64 BX = int64(K.X) * ChunkVox;
					const int64 BY = int64(K.Y) * ChunkVox;
					const int64 BZ = int64(K.Z) * ChunkVox;
					bool bAnyAir = false, bAnySolidInterior = false;
					for (int32 Z = -1; Z <= ChunkVox; ++Z)
					{
						for (int32 Y = -1; Y <= ChunkVox; ++Y)
						{
							for (int32 X = -1; X <= ChunkVox; ++X)
							{
								const bool bAir = Sampler(BX + X, BY + Y, BZ + Z) == vxc::MAT_AIR;
								bAnyAir |= bAir;
								const bool bInterior =
									X >= 0 && X < ChunkVox && Y >= 0 && Y < ChunkVox && Z >= 0 && Z < ChunkVox;
								bAnySolidInterior |= (bInterior && !bAir);
							}
						}
					}
					if (!bAnySolidInterior)
					{
						return 1; // all air in the interior (apron irrelevant -- mesher early-out 1)
					}
					if (!bAnyAir)
					{
						return 2; // all solid across chunk+apron
					}
					return 3; // zero quads but neither
				};
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
					// Where the 34x34 grid LIVES, which is not a detail at this
					// size. sizeof(vxc::ColumnSample) is 184 bytes (16 B of
					// stratigraphy + surfaceMat, a 108 B CaveColumn and a 56 B
					// CavernColumn), so the grid is 1156 * 184 = 208 KB -- far above
					// any binned small-block pool, i.e. one OS-backed allocation,
					// commit, first-touch page-fault storm over 52 pages, and free
					// PER JOB. At the measured level-0 dispatch rate that is
					// hundreds of MB/s of virtual-memory churn through a shared
					// allocator, and the number of jobs doing it CONCURRENTLY is
					// exactly what rises under motion.
					//
					// A per-worker-thread scratch buffer removes the allocation
					// entirely: a job's grid is dead the moment the job returns, no
					// job outlives its own thread, and level-0 grid work never
					// nests, so one buffer per thread is sufficient and is reused
					// for the life of the process (24 workers x 208 KB = 5 MB,
					// which is what was transiently live anyway).
					//
					// BYTE-IDENTICAL by construction: every one of the 1156 cells is
					// unconditionally assigned by the loop below before anything
					// reads it, exactly as SetNumUninitialized + assignment did, so
					// no value from a previous job can survive into this one. Only
					// the address of the storage changes.
					//
					// -VoxelL0GridScratch=0 restores the per-job TArray as the A/B
					// control (command line, not a cvar, for the -VoxelCoarseMinLevel
					// reason: -ExecCmds lands after streaming has begun).
					constexpr int32 GridCells = GridEdge * GridEdge;
					static thread_local vxc::ColumnSample ScratchColumns[GridCells];
					TArray<vxc::ColumnSample> HeapColumns; // only populated when the switch is off
					vxc::ColumnSample* Columns = ScratchColumns;
					if (!VoxelStreamAdmission::L0GridScratchEnabled())
					{
						HeapColumns.SetNumUninitialized(GridCells);
						Columns = HeapColumns.GetData();
					}
					const vxc::Amplifier& Amp = GenPtr->amplifier();
					// Buried-chunk skip step 1: time the column-grid build apart
					// from the mesh. These (32+2)^2 Amplifier::column calls are
					// the only part of a level-0 job that a pre-dispatch skip
					// could reclaim -- the mesher's own early-outs already make
					// the mesh of a uniform chunk cheap -- so this split is what
					// decides whether the skip is worth building at all.
					const double GridStartSeconds = FPlatformTime::Seconds();
					const uint64 GridStartCycles = VoxelThreadCycles();
					for (int32 LY = 0; LY < GridEdge; ++LY)
					{
						for (int32 LX = 0; LX < GridEdge; ++LX)
						{
							Columns[LX + GridEdge * LY] =
								Amp.column(BaseVX + LX - 1, BaseVY + LY - 1);
						}
					}
					Result.GridCycles = VoxelThreadCycles() - GridStartCycles;
					Result.GridMs = float((FPlatformTime::Seconds() - GridStartSeconds) * 1000.0);

					// Buried-chunk pre-dispatch skip: reduce the grid this job
					// just built into its footprint's band (see FFootprintBand).
					// A max/min over 1156 already-resident ColumnSamples -- no
					// amplifier work, no allocation -- whose value is that every
					// OTHER level-0 chunk in this (X,Y) can then be answered
					// without a job at all. The one-voxel widening on each side
					// is pure insurance: the per-column bounds are already outer
					// bounds, this makes an off-by-one in the derivation harmless
					// rather than a hole in the world.
					// Gated so that -VoxelBuriedSkip=0 is a TRUE pre-wave
					// baseline: without this the "off" side of an A/B still pays
					// the reduction (1156 columns x up to 18 segment tests), which
					// would quietly flatter the "on" side by handicapping its
					// control. Costs nothing in production, where it is always on.
					// --- Per-column emptiness bounds -----------------------------
					//
					// The two numbers the band is a reduction of, kept PER COLUMN
					// instead of collapsed immediately, because the same 1156
					// values answer a second and much larger question: which of
					// this chunk's 64 BRICKS can emit nothing.
					//
					// The band proves things about a whole 32-voxel-tall chunk and
					// so is only ever decisive for chunks entirely above the
					// terrain or entirely inside it -- and the pre-dispatch skip has
					// already removed those. What is left is measured at 40% of
					// level-0 worker time spent on chunks that mesh to ZERO quads,
					// plus every surface chunk, in which typically only one of the
					// four brick layers in z straddles the surface at all. Reduced
					// over a single brick's 10x10 column block the same two bounds
					// ARE decisive, brick by brick, and the reduction is ~164
					// integer compares against the 1,000 sampler calls it saves.
					//
					// Nothing new is generated: these are the values the band
					// reduction was already computing per column and discarding.
					// -VoxelL0BrickSkip=0 restores the unconditional mesh.
					static thread_local int64 ScratchTopSolid[GridCells];
					static thread_local int64 ScratchDeepestAir[GridCells];
					const bool bBrickSkip = VoxelStreamAdmission::L0BrickSkipEnabled();
					if (bComputeBand || bBrickSkip)
					{
						for (int32 I = 0; I < GridCells; ++I)
						{
							const vxc::ColumnSample& Col = Columns[I];
							ScratchTopSolid[I] = VoxelStreaming::ColumnSurfaceTopVoxel(Col);
							ScratchDeepestAir[I] = VoxelStreaming::ColumnDeepestAirVoxel(Col);
						}
					}
					if (bComputeBand)
					{
						int64 MaxTop = INT64_MIN;
						int64 MinAir = INT64_MAX;
						for (int32 I = 0; I < GridCells; ++I)
						{
							MaxTop = FMath::Max(MaxTop, ScratchTopSolid[I]);
							MinAir = FMath::Min(MinAir, ScratchDeepestAir[I]);
						}
						// Shared with voxel.GPU.VerifyRegion's band gate and with
						// BandReduceMain's readback path, so the widening cannot
						// drift between the three producers of a band.
						Result.Band = VoxelStreaming::MakeFootprintBand(MaxTop, MinAir);
						Result.bBandValid = true;
					}
					const auto GridSampler = [Columns, BaseVX, BaseVY](int64 X, int64 Y, int64 Z)
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

					// The brick-level counterpart of BandProvesChunkEmpty, over
					// this brick's own column block rather than the whole chunk's.
					// Both halves carry the same one-voxel widening the band uses
					// -- the per-column bounds are already outer bounds, so this is
					// insurance against an off-by-one in a derivation, not part of
					// it.
					constexpr int32 BrickVox = VoxelCoords::BrickEdgeVoxels;
					constexpr int32 BricksPerChunkEdge = VoxelCoords::ChunkEdgeBricks;
					const int64 ChunkBaseVZ = int64(Key.Z) * ChunkVox;
					int32 SkippedAir = 0;
					int32 SkippedSolid = 0;
					const auto SkipBrick = [&](int32 Dx, int32 Dy, int32 Dz) -> bool
					{
						if (!bBrickSkip)
						{
							return false;
						}
						const int64 InteriorZMin = ChunkBaseVZ + int64(Dz) * BrickVox;
						// (a) No SOLID in the interior -- meshBrick's own early-out
						// 1, which looks at the interior alone, so the ring skirt
						// (an APRON rewrite) cannot affect it.
						int64 MaxTopInterior = INT64_MIN;
						for (int32 J = 0; J < BrickVox; ++J)
						{
							const int32 RowBase = GridEdge * (Dy * BrickVox + 1 + J) + Dx * BrickVox + 1;
							for (int32 I = 0; I < BrickVox; ++I)
							{
								MaxTopInterior = FMath::Max(MaxTopInterior, ScratchTopSolid[RowBase + I]);
							}
						}
						if (InteriorZMin > MaxTopInterior + 1)
						{
							++SkippedAir;
							return true;
						}
						// (b) No AIR in brick + apron. This one DOES read the apron,
						// so it is unusable on a brick whose apron the ring skirt
						// rewrites to AIR -- that is a lateral chunk face, i.e. the
						// outermost brick on a flagged axis.
						if (((RingSkirtMask & RingSkirt_NegX) && Dx == 0) ||
						    ((RingSkirtMask & RingSkirt_PosX) && Dx == BricksPerChunkEdge - 1) ||
						    ((RingSkirtMask & RingSkirt_NegY) && Dy == 0) ||
						    ((RingSkirtMask & RingSkirt_PosY) && Dy == BricksPerChunkEdge - 1))
						{
							return false;
						}
						int64 MinAirApron = INT64_MAX;
						for (int32 J = 0; J < BrickVox + 2; ++J)
						{
							const int32 RowBase = GridEdge * (Dy * BrickVox + J) + Dx * BrickVox;
							for (int32 I = 0; I < BrickVox + 2; ++I)
							{
								MinAirApron = FMath::Min(MinAirApron, ScratchDeepestAir[RowBase + I]);
							}
						}
						// meshBrick samples z over [-1, B] relative to the brick
						// origin, so the topmost cell it reads is InteriorZMin + B.
						if (InteriorZMin + BrickVox < MinAirApron - 1)
						{
							++SkippedSolid;
							return true;
						}
						return false;
					};
					MeshChunkBricks(Key, GridSampler, Result.Quads, PerfCountersPtr, RingSkirtMask, SkipBrick);
					Result.BricksSkippedAir = uint16(SkippedAir);
					Result.BricksSkippedSolid = uint16(SkippedSolid);
					if (bMeasureEmpty && Result.Quads.Num() == 0)
					{
						Result.EmptyClass = ClassifyEmpty(GridSampler, Key);
					}
					// Equivalence harness (-VoxelL0GridVerify): mesh the SAME chunk
					// through the pre-change reference -- a fresh per-job TArray
					// filled by a plain Amp.column loop -- and compare quad for
					// quad. A mismatch here is terrain that changed shape, not a
					// tuning result, so the chunk key is logged.
					if (VoxelStreamAdmission::L0GridVerifyEnabled())
					{
						TArray<vxc::ColumnSample> RefColumns;
						RefColumns.SetNumUninitialized(GridCells);
						for (int32 LY = 0; LY < GridEdge; ++LY)
						{
							for (int32 LX = 0; LX < GridEdge; ++LX)
							{
								RefColumns[LX + GridEdge * LY] = Amp.column(BaseVX + LX - 1, BaseVY + LY - 1);
							}
						}
						const auto RefSampler = [&RefColumns, BaseVX, BaseVY](int64 X, int64 Y, int64 Z)
						{
							const int32 LX = int32(X - BaseVX) + 1;
							const int32 LY = int32(Y - BaseVY) + 1;
							return vxc::Amplifier::materialAt(RefColumns[LX + GridEdge * LY], Z);
						};
						TArray<FVoxelChunkQuad> RefQuads;
						MeshChunkBricks(Key, RefSampler, RefQuads, /*PerfCounters*/ nullptr, RingSkirtMask);
						int32 FirstDiff = -1;
						VoxelStreamAdmission::GL0GridVerifyChecked.fetch_add(1, std::memory_order_relaxed);
						if (!VoxelChunkQuadsIdentical(Result.Quads, RefQuads, FirstDiff))
						{
							VoxelStreamAdmission::GL0GridVerifyMismatches.fetch_add(1, std::memory_order_relaxed);
							UE_LOG(LogVoxelPerf, Error,
							       TEXT("VoxelL0GridVerify MISMATCH: L0 chunk (%d, %d, %d) fast=%d quads ref=%d quads firstDiff=%d"),
							       Key.X, Key.Y, Key.Z, Result.Quads.Num(), RefQuads.Num(), FirstDiff);
						}
					}
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
					//
					// ...unless this level is at or above the coarse threshold,
					// in which case the brick comes straight out of
					// GeneratedWorld::makeCoarseBrick at O(1) per level instead
					// of folding 8^L level-0 bricks. Both samplers are pure
					// GeneratedWorld and satisfy the same
					// [-1,B]^3-across-borders contract MeshChunkBricks wants, so
					// this is a straight substitution of the brick RULE, nothing
					// else in the job changes.
					//
					// A coarse chunk goes one step further and skips the brick
					// LAYER too: FCoarseChunkGridSampler builds the chunk's
					// (32+2)^2 coarse columns once and evaluates the coarse
					// rule directly, which is the same composition
					// makeCoarseBrick stores (see that struct's derivation) --
					// so this is a plumbing change, not a generation change.
					// -VoxelCoarseGrid=0 takes the brick-cache path instead,
					// as the A/B control.
					const bool bCoarseLevel = LevelKey.Level >= VoxelStreamAdmission::GetCoarseMinLevel();
					if (bCoarseLevel && VoxelStreamAdmission::CoarseGridEnabled())
					{
						// Timed like the level-0 grid build for the same
						// reason: it is the only amplifier work a coarse job
						// does, so it bounds what any pre-dispatch skip could
						// ever reclaim from one.
						const double GridStartSeconds = FPlatformTime::Seconds();
						const FCoarseChunkGridSampler CoarseSampler(*GenPtr, LevelKey.Level, Key, PerfCountersPtr);
						Result.GridMs = float((FPlatformTime::Seconds() - GridStartSeconds) * 1000.0);
						MeshChunkBricks(Key, CoarseSampler, Result.Quads, PerfCountersPtr, RingSkirtMask);
						if (bMeasureEmpty && Result.Quads.Num() == 0)
						{
							Result.EmptyClass = ClassifyEmpty(CoarseSampler, Key);
						}
						// Equivalence harness: mesh the SAME chunk through the
						// old brick-cache sampler and compare quad-for-quad.
						// Logs the chunk key on any mismatch -- a mismatch here
						// is terrain that changed shape, not a tuning result.
						if (VoxelStreamAdmission::CoarseGridVerifyEnabled())
						{
							TArray<FVoxelChunkQuad> RefQuads;
							const auto RefSampler = MakeCoarseLevelSampler(*GenPtr, LevelKey.Level, /*PerfCounters*/ nullptr);
							MeshChunkBricks(Key, RefSampler, RefQuads, /*PerfCounters*/ nullptr, RingSkirtMask);
							int32 FirstDiff = -1;
							VoxelStreamAdmission::GCoarseGridVerifyChecked.fetch_add(1, std::memory_order_relaxed);
							if (!VoxelChunkQuadsIdentical(Result.Quads, RefQuads, FirstDiff))
							{
								VoxelStreamAdmission::GCoarseGridVerifyMismatches.fetch_add(1, std::memory_order_relaxed);
								UE_LOG(LogVoxelPerf, Error,
								       TEXT("VoxelCoarseGridVerify MISMATCH: L%d chunk (%d, %d, %d) grid=%d quads ref=%d quads firstDiff=%d"),
								       LevelKey.Level, Key.X, Key.Y, Key.Z, Result.Quads.Num(), RefQuads.Num(), FirstDiff);
							}
						}
					}
					else
					{
						const auto LevelSampler =
							bCoarseLevel
								? MakeCoarseLevelSampler(*GenPtr, LevelKey.Level, PerfCountersPtr)
								: MakeLevelSampler(*GenPtr, LevelKey.Level, PerfCountersPtr, SharedMipCachePtr, EditEpochPtr, EditEpochSnapshot);
						MeshChunkBricks(Key, LevelSampler, Result.Quads, PerfCountersPtr, RingSkirtMask);
						if (bMeasureEmpty && Result.Quads.Num() == 0)
						{
							Result.EmptyClass = ClassifyEmpty(LevelSampler, Key);
						}
					}
				}

				Result.JobCycles = VoxelThreadCycles() - JobStartCycles;
				Result.JobMs = float((FPlatformTime::Seconds() - JobStartSeconds) * 1000.0);
				// S0-3. Result.bFromGpuMesh stays false (its default) -- this IS
				// the CPU worker arm. bLatencyStatsEnabled was read on the game
				// thread before this task launched (see the capture-list comment
				// above); gating this new FPlatformTime::Seconds() call on it
				// here, on the worker thread, is why it had to be captured by
				// value rather than read straight from the cvar.
				if (bLatencyStatsEnabled)
				{
					Result.DeliverSeconds = FPlatformTime::Seconds();
				}
				QueuePtr->Enqueue(MoveTemp(Result));
				CounterPtr->Decrement();
			},
			UE::Tasks::ETaskPriority::BackgroundNormal);
		InFlightTasks.Add(MoveTemp(Task));

		// Cold-band throttle: a job is now genuinely in flight for this footprint
		// and will produce its band, so hold the column-mates back until it
		// drains. Set HERE and nowhere earlier -- every path that leaves the loop
		// without launching must leave the mark untouched, or the column strands.
		if (LevelKey.Level == 0 && bBandSkipActive)
		{
			const FIntPoint SeedFootprint(LevelKey.Key.X, LevelKey.Key.Y);
			if (!FootprintBandCache.Contains(SeedFootprint))
			{
				FootprintBlindJobInFlight.Add(SeedFootprint, ElapsedSeconds);
			}
		}

	}

	// Re-queue the chunks held back by the cold-band throttle. Order is
	// irrelevant here -- SortPendingQueues re-sorts both queues on the next
	// recompute -- and they are retried next frame, by which time the seeding
	// job has usually drained and the band can answer for the whole column.
	if (DeferredColdBand.Num() > 0)
	{
		PendingJobKeysByLevel[0].Append(DeferredColdBand);
		ColdBandHeldThisFrame = DeferredColdBand.Num();
	}
	else
	{
		ColdBandHeldThisFrame = 0;
	}

	// T4-1: demand has now taken everything it wants from this tick's budget.
	// Speculation gets the remainder, and only the remainder -- submitting here
	// rather than anywhere earlier is what makes starvation structurally
	// impossible rather than a matter of tuning.
	//
	// UNCONDITIONAL, and it was briefly not: this call was first written inside
	// the cold-band re-queue branch above, so speculation only dispatched on
	// ticks that happened to have cold-band deferrals. The census read
	// "queued=128 inFlight=128 dispatched=0" for a whole leg -- enumeration
	// working, submission never running -- which looks exactly like a budget
	// refusing speculation rather than a call site in the wrong scope.
	// TIMED SEPARATELY (task #21). dispatch= in the tick-budget line covers
	// demand dispatch, this call, and the post-drain second pass in one bucket,
	// which is why T4-1's +264 ms/window could not be attributed: per job the
	// speculative path costs 16x the demand path through the SAME
	// SubmitGpuMeshJob, so the cost is per-tick overhead somewhere in here
	// rather than per-submit. Still summed INTO AccumDispatchMs by the caller,
	// so the existing bucket keeps its meaning; this is a sub-total, not a
	// fifth phase.
	const double SpecT0 = FPlatformTime::Seconds();
	DispatchSpeculativeJobs();
	AccumSpecDispatchMs += (FPlatformTime::Seconds() - SpecT0) * 1000.0;
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

// --- ADR-0006 G3: the GPU-pool geometry path ------------------------------

UVoxelGpuPoolComponent* FVoxelWorldImpl::GetOrCreateGpuPool(AActor& Owner, USceneComponent& Root,
                                                           UMaterialInterface* Material,
                                                           const FVector& FirstChunkOrigin)
{
	if (UVoxelGpuPoolComponent* Existing = GpuPool.Get())
	{
		return Existing;
	}

	// The pool gets its OWN actor with the pool as the root component, rather
	// than hanging off ChunkRoot alongside the per-chunk components. That is
	// the configuration voxel.GPU.SpawnPool has always used and the only one
	// observed to actually reach the renderer: attached as a child, the
	// primitive never entered the visible set at all -- GetDynamicMeshElements
	// was never called on it, despite valid bounds and a live proxy.
	FActorSpawnParameters PoolSpawnParams;
	PoolSpawnParams.ObjectFlags |= RF_Transient;
	AActor* PoolOwner = Owner.GetWorld()->SpawnActor<AActor>(
		AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, PoolSpawnParams);
	if (PoolOwner == nullptr)
	{
		return nullptr;
	}

	UVoxelGpuPoolComponent* Pool = NewObject<UVoxelGpuPoolComponent>(PoolOwner);
	// Same terrain material the per-chunk components use. Without it the
	// proxy falls back to the engine default and the biome LUT never runs.
	Pool->SetChunkMaterial(Material);
	// BEFORE RegisterComponent, which creates the proxy -- the proxy takes a
	// COPY of this and a later setter is a silent no-op. The terrain pool never
	// set it at all, so it relied on MaxChunks = Max(InOrigins.Num() * 4, 1024).
	// At R0 = 128 m the settled cascade holds 43,328 chunks (measured). Crossing
	// whatever the proxy was built with is not an error but it IS a full
	// render-state rebuild, which re-uploads the ENTIRE quad buffer; terrain
	// crosses it while the cascade fills and then stops, so a floor above the
	// measured steady state turns a handful of whole-pool re-uploads into none.
	//
	// RAISED 49,152 -> 81,920 for S1-1 (2026-07-27). The old floor was one step
	// above the 43,328-chunk settle, which was ample while the pipeline applied
	// ~260 chunks/s. Batching publication doubles that and transient residency
	// with it: the first batched legs peaked at 49,349 live chunks, i.e. THROUGH
	// this floor. Crossing it is the branch above -- MarkRenderStateDirty, whose
	// rebuild per D4-R1 invalidates every GPU-written range, so at 580 chunks/s
	// that is not "a handful of whole-pool re-uploads", it is a repeating one.
	// See docs/measurements/s1-1-batch-publish-2026-07-27.txt.
	// Sized with the pool (2026-07-28). Peak liveChunks on the 330 s traverse is
	// 61,238, which is 75% of 81,920 -- and crossing MaxChunks is worse than
	// running out of quads: it takes the MarkRenderStateDirty branch, which per
	// D4-R1 invalidates EVERY GPU-written range. 98,304 puts the measured peak at
	// 62%. Entries are tiny next to the quad buffers, so this headroom is nearly
	// free; it must still be set before the proxy exists, because MaxChunks is
	// frozen at proxy construction.
	Pool->SetChunkTableCapacity(98304);
	PoolOwner->SetRootComponent(Pool);
	Pool->RegisterComponent();

	// REBASE. The chunk table is float32 (FVector4f per chunk), and this world
	// runs at ~8.4 MILLION unreal units from the origin. float32's ULP up there
	// is 1.0 UU against a 10 UU voxel, so chunk origins expressed as absolute
	// positions lose a tenth of a voxel before the shader even starts -- every
	// chunk in the pool visibly wrong, with no per-chunk transform to hide
	// behind the way the component path has.
	//
	// So the component carries the big offset in its (double-precision)
	// transform and the table stores everything relative to it. Chunks stay
	// within the cascade's couple of kilometres of this point, where float32's
	// ULP is ~0.015 UU -- three orders of magnitude of headroom.
	//
	// Set once, from the first chunk to load, and never moved: re-basing a live
	// pool would mean rewriting every entry in the table. Even a player crossing
	// the whole 84 km world only reaches ~0.015 UU of error.
	// SetWorldLocation AFTER RegisterComponent, not SetRelativeLocation before:
	// SetRootComponent on a freshly NewObject'd component installs an identity
	// transform and redefines the actor's location as the world origin, which
	// in this world is 84 km from anything worth drawing.
	GpuPoolRebase = FirstChunkOrigin;
	Pool->SetWorldLocation(GpuPoolRebase);
	// See the member: SubmitGpuMeshJob composes a chunk's world origin from this
	// exactly as ApplyMeshResult does.
	GpuPoolRoot = &Root;

	// Sized once, up front, and never grown. The live 2 km cascade measured
	// 9,441,170 quads (docs/gpu-g0-sizing.md); the headroom above that pays for
	// two things the steady-state figure does not cover. Load-before-unload
	// retention transiently holds a chunk's allocation WHILE its replacement
	// takes another, so a footprint mid-LOD-transition is double-allocated. And
	// first-fit fragments: the allocator soak refuses ~10% of allocations at
	// small capacity while still reporting plenty free. If AddChunk starts
	// refusing, GetLargestFreeRun() against GetFreeQuads() is the number that
	// says whether this is genuinely full or merely fragmented -- they are very
	// different problems and only the second one wants a compaction pass.
	// WAVE F RESIZE. 14,000,000 was sized for the 64 m cascade and IS NOT ENOUGH
	// for R0 = 128 m. Measured, not estimated: the shifted cascade settled
	// (jobsInFlight=0, pendingJobs=0, every ring pending=0) at
	//
	//     loaded = 43,328 chunks     residentQuads = 21,240,815
	//     rings  = 7520 / 6515 / 7188 / 7597 / 7655 / 6853
	//
	// so the old capacity was short by 52% and every quad past 14 M would have
	// failed to allocate. That failure is the dangerous kind: a chunk that
	// cannot allocate is simply absent, so liveChunks, highWater and resident
	// quads all stay self-consistently smaller and the run reads as CHEAPER AND
	// FASTER -- i.e. as a success. Worse, UpdateChunk's realloc path removes the
	// chunk BEFORE reallocating, so on a full pool an ordinary re-mesh deletes
	// resident terrain. Wave F's allocation-failure counters exist to make that
	// visible; this constant exists so they do not have to fire.
	//
	// 26 M is the measured 21.24 M plus ~22%. The headroom is not padding -- it
	// pays for the same two things the old comment names, both of which scale
	// with the cascade rather than being fixed: load-before-unload retention
	// double-allocates a footprint mid-LOD-transition, and first-fit fragments
	// (the allocator soak refuses ~10% of allocations at small capacity while
	// still reporting plenty free).
	//
	// AND IT IS ONE ANCHOR. 21.24 M was measured at the default spawn; a denser
	// anchor will differ. If AddChunk starts refusing, GetLargestFreeRun()
	// against GetFreeQuads() says whether this is genuinely full or merely
	// fragmented -- very different problems, and only the second wants a
	// compaction pass.
	// 2026-07-27 addendum: 26 M is NOT ENOUGH for the 128 m cascade on REAL
	// terrain either -- the ring-gap night's cold-fill legs settled with
	// residentQuads pegged at exactly this capacity (docs/measurements/
	// ring-gap-2026-07-27.txt), i.e. silently saturated. -VoxelPoolCapacityQuads
	// overrides it for that work (latched, like the ring switches: the pool is
	// sized once before any cvar could land); the pool component now also logs
	// a latched Error the first time it saturates or refuses an allocation, so
	// the silent-success failure shape above cannot recur unnoticed.
	// RESIZED 26M -> 44M for the adopted 128 m / 4 km cascade (2026-07-27):
	// measured demand at settle is 35,205,733 quads (39,020 chunks, identical
	// across four legs and both meshers, real terrain, pool overridden to 60M so
	// nothing saturated the measurement). 44M = 35.2M + ~13% motion stand-in
	// double-allocation (the ring-gap night's measured retention overhead) +
	// ~10% first-fit fragmentation -- the same headroom methodology as the 26M
	// sizing below, re-derived from the new cascade's numbers. 352 MB at 8 B/quad.
	// RESIZED 44M -> 64M for S1-1 (2026-07-27, same day as the 26M -> 44M above).
	//
	// 44M was sized from a settle of 35,205,733 quads at 39,020 chunks -- measured
	// when the pipeline could only apply ~260 chunks/s. Batching publication
	// (voxel.Stream.PoolBatchPublish) takes that to ~580, and TRANSIENT residency
	// with it: the first batched legs peaked at 49,349 live chunks and
	// capacityPct=98.5, then refused 77,290 allocations (96.9M quads) while the
	// free list shattered into 16,903 runs with a largest run of 68,326. Full
	// data: docs/measurements/s1-1-batch-publish-2026-07-27.txt.
	//
	// So this is not the cascade getting bigger. It is the pipeline getting fast
	// enough to hold more chunks IN FLIGHT between admission and unload, and the
	// old number was derived from a pipeline that could not do that.
	//
	// 64M = 49,349 peak chunks x ~902 quads/chunk (44.5M) + ~44% headroom for the
	// fragmentation first-fit produces under this churn. 512 MB at 8 B/quad, plus
	// the same again in the chunk-id buffer and 12 B/quad of CPU shadow -- see
	// docs/speculative-generation-plan.md §2.6 for the full memory arithmetic and
	// why the shadow is what makes further raises expensive.
	//
	// SIZED 2026-07-28 FROM A MEASURED PLATEAU, replacing the plan's 104M guess.
	//
	// The plan's capacity step 2 was 104M, justified by an assumption that
	// speculative generation (T4-1) would hold a large parked population. It does
	// not: measured peak speculative parked is ~172 chunks against a 4,000 cap,
	// with zero evicted-unused. That justification is void, so the number is set
	// from what the pool actually reaches instead.
	//
	// WHAT IT ACTUALLY REACHES. On a 330 s traverse -- 2.75x the standard leg, and
	// long enough to matter, because the standard 120 s leg stops BEFORE the
	// asymptote and reads like unbounded growth -- residency climbs from the
	// 39,020-chunk settle to ~60,000 by mid-flight and then oscillates
	// 58,208-61,238 for the remaining 150 windows. highWater plateaus at
	// 59,242,301 quads and stops moving entirely. It BOUNDS. The bound is the
	// cascade's own settle plus PoolParkMax (12,000) plus retention, so it is a
	// property of the ring geometry and the park cap, not of session length.
	//
	// Against the old 64,000,000 that plateau is 92.6% of the pool's address
	// space, leaving 4.76M quads of never-touched tail against 26,515 fragmented
	// free runs whose largest is 6.8M of 10.0M free. allocFail stayed 0 on every
	// leg, but the margin is thin and the failure mode is not graceful:
	// UpdateChunk's realloc path DELETES RESIDENT TERRAIN on a full pool.
	//
	// 80M puts the measured plateau at 74% with ~35% headroom, which covers
	// terrain denser than this traverse (mean 967 quads/chunk here) and first-fit
	// fragmentation without paying for a parked population that does not exist.
	//
	// WHY NOT MORE: the CPU shadow below is still allocated at full capacity
	// (PooledQuads + QuadChunkIds, 12 B/quad), so every quad of capacity costs
	// 12 B of VRAM AND 12 B of system RAM, and CreateSceneProxy copies both
	// arrays whole. At 80M that is 960 MB VRAM + 960 MB RAM and a 960 MB
	// game-thread memcpy on any render-state rebuild. S2-5 -- dropping the shadow
	// on the GPU-only arm, where nothing reads it -- is what makes any further
	// raise cheap. Do that before going higher, not after.
	constexpr uint32 kPoolCapacityQuads = 80u * 1000u * 1000u;   // 640 MB at 8 B/quad
	uint32 PoolCapacityQuads = kPoolCapacityQuads;
	{
		uint32 Override = 0;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelPoolCapacityQuads="), Override) && Override > 0)
		{
			PoolCapacityQuads = FMath::Clamp<uint32>(Override, 1u * 1000u * 1000u, 200u * 1000u * 1000u);
		}
	}
	Pool->InitPool(PoolCapacityQuads);

	GpuPool = Pool;
	// T4-1 parks results outside ApplyMeshResult, which is where Root is normally
	// threaded from. SampleChunkParamsForPool needs it, so the pool's root is
	// captured once here rather than plumbed through the completion path.
	GpuPoolRootComponent = &Root;
	UE_LOG(LogVoxelStream, Log,
	       TEXT("voxel.Stream.GPU: geometry pool up, %u quad capacity (%.0f MB)%s, chunk table floor %d. "
	            "Chunks now stream as ranges in ONE primitive."),
	       PoolCapacityQuads, double(PoolCapacityQuads) * 8.0 / (1024.0 * 1024.0),
	       // Read back from the component rather than repeating the literal: this
	       // line said 49152 for one commit after the floor moved to 81920, which
	       // is the exact shape of "stale comments about defaults are landmines"
	       // (docs/lessons-2026-07-27-gpu-sessions.md, lesson 3).
	       PoolCapacityQuads != kPoolCapacityQuads ? TEXT(" [OVERRIDDEN]") : TEXT(""),
	       Pool->GetChunkTableCapacity());
	return Pool;
}

// Climate for one chunk, sampled at its world centre.
//
// The component path samples per QUAD and bakes the result into vertex colour;
// the pooled path has no per-quad storage, so it samples once per chunk and the
// shader looks it up by chunk id. A chunk is 3.2 m across a 30 m climate raster
// cell -- about 1/100th of a pixel's area -- and climate is a smooth bilinear
// ramp over that distance, so this is one step coarser on an already heavily
// oversampled signal. The error is a gentle chunk-to-chunk gradient, not
// banding. If boundary artifacts ever do show, docs/gpu-g3-integration-plan.md
// records the two escalations.
// It also carries the surface height the pooled shader needs for its
// biome-tint gate. The component path samples that per chunk too
// (BuildChunkVertexData's ChunkSurfaceZUU), at the same point -- the chunk
// centre -- so this is the same number, not an approximation of it.
//
// It is returned RELATIVE TO THE CHUNK ORIGIN. The subtraction has to happen
// here, in double precision, because the absolute value is ~8.4M UU where
// float32's ULP is 1.0 UU against a 10 UU voxel; the relative value is bounded
// by the world's height range and is exact in float. This is the same reasoning
// that puts chunk origins in a rebase frame (docs/gpu-pool-rendering-notes.md
// invariant 4), applied to the one other absolute coordinate in the table.
static FVector4f SampleChunkParamsForPool(const USceneComponent& Root,
                                          const FVector& ChunkOriginRelative,
                                          int32 Level)
{
	const double HalfEdgeUU = 0.5 * VoxelCoords::ChunkEdgeUU * double(int64(1) << Level);
	const FVector ChunkWorldOrigin = Root.GetComponentLocation() + ChunkOriginRelative;
	const FVector Centre = ChunkWorldOrigin + FVector(HalfEdgeUU, HalfEdgeUU, 0.0);

	VoxelClimate::EnsureInitialized();
	const FVoxelClimateBytes Bytes = VoxelClimate::SampleClimateAtWorldUU(Centre.X, Centre.Y);

	// No subsystem -> no gate, matching BuildChunkVertexData's fallback for
	// transient/loading worlds ("always surface", how it behaved before the gate
	// existed).
	float SurfaceZRelUU = UVoxelGpuPoolComponent::kNoSurfaceGate;
	if (const UWorld* World = Root.GetWorld())
	{
		if (const UVoxelWorldSubsystem* Sub = World->GetSubsystem<UVoxelWorldSubsystem>())
		{
			const double SurfaceZUU = Sub->GetSurfaceHeightUU(Centre.X, Centre.Y);
			SurfaceZRelUU = float(SurfaceZUU - ChunkWorldOrigin.Z);
		}
	}

	return FVector4f(float(Bytes.Temperature) / 255.0f,
	                 float(Bytes.Precipitation) / 255.0f,
	                 SurfaceZRelUU,
	                 0.0f);
}

void FVoxelWorldImpl::ReleaseChunkGeometry(VoxelStreaming::FChunkRecord& Rec)
{
	// Dispatch on what the record HOLDS, not on the cvar: a chunk that loaded
	// before a mid-session voxel.Stream.GPU flip must unload the way it loaded.
	if (UVoxelChunkComponent* Existing = Rec.Component.Get())
	{
		ReturnChunkComponentToPool(*Existing);
	}
	Rec.Component = nullptr;

	if (Rec.PoolSlot != INDEX_NONE)
	{
		if (UVoxelGpuPoolComponent* Pool = GpuPool.Get())
		{
			Pool->RemoveChunk(Rec.PoolSlot);
		}
		Rec.PoolSlot = INDEX_NONE;
	}
}

// S2-3. ReleaseChunkGeometry's alternative: keep the range, hide it, remember it.
//
// Returns true if the geometry was parked (and therefore NOT freed). Callers
// must treat a true return exactly as they treat ReleaseChunkGeometry -- the
// record no longer holds geometry either way.
bool FVoxelWorldImpl::ParkChunkGeometry(const VoxelCoords::FVoxelLevelChunkKey& Key,
                                        VoxelStreaming::FChunkRecord& Rec)
{
	const int32 Cap = VoxelDebug::GetStreamPoolParkMax();
	if (Cap <= 0)
	{
		return false; // parking off -- the default
	}
	// Pool geometry only. A component-path chunk has its own pooling
	// (ReturnChunkComponentToPool) and nothing here to keep.
	if (Rec.PoolSlot == INDEX_NONE || Rec.Component.IsValid())
	{
		++ParkRefusedNoPoolGeomSinceLog;
		return false;
	}
	UVoxelGpuPoolComponent* Pool = GpuPool.Get();
	if (!Pool)
	{
		return false;
	}
	// An unsettled chunk's geometry is not final -- parking it would cache a
	// shape that was still being replaced.
	if (!Rec.bMeshSettled || Rec.LastQuadCount <= 0)
	{
		++ParkRefusedUnsettledSinceLog;
		return false;
	}

	// PARK ONLY LOD TRANSITIONS TOWARD FINER, AND THAT RESTRICTION IS THE WHOLE
	// DIFFERENCE BETWEEN THIS PAYING AND COSTING.
	//
	// Measured parking every eviction on a straight-line flight: 135,260 parks
	// against 10,419 adopts -- an 8% hit rate -- while the 92% that were never
	// reused held pool ranges and chunk-table entries until the cap pushed them
	// out. That took allocFail 0 -> 45,633, holes 0 -> 1,804 and chunks/s
	// 1,040 -> 936. Parking geometry is only free if it is reused.
	//
	// The reason is geometric: on a traverse, everything evicted by bBeyondOuter
	// is BEHIND the camera and never comes back, so caching it is pure cost. What
	// does come back is the ring-boundary oscillation -- a footprint that dipped
	// inside this level's inner edge as the anchor passed and will re-enter the
	// annulus as it recedes. RetainDir_Finer is exactly that eviction cause
	// (bInsideInner), already stamped by RecomputeDesiredSet.
	//
	// Wave S4 note: speculative geometry is the mirror image of this -- generated
	// AHEAD of the camera, where the hit rate should be high by construction --
	// so it will not route through here and must not inherit this test.
	if (Rec.RetainReplaceDir != RetainDir_Finer)
	{
		// THIS IS THE COUNTER TO READ FOR TASK #17. Wrapping RecomputeDesiredSet
		// in an FScopedBatch took parking from 28,117 parks (84% hit) to exactly
		// ZERO, reproducibly, and was reverted undiagnosed. "Exactly zero" means
		// either this function stopped being called or one of its refusals became
		// universal -- and RetainReplaceDir is the only one stamped by
		// RecomputeDesiredSet itself, which makes it the prime suspect. If the
		// experiment is repeated, this number distinguishes the two cases in one
		// leg instead of a reasoning session.
		++ParkRefusedNotFinerSinceLog;
		return false;
	}
	// Edited chunks are never parked. NeedsOverlayAwarePath is the same test
	// admission uses to route them to the game-thread mesher, and their geometry
	// is a function of the edit log rather than of worldgen alone.
	if (NeedsOverlayAwarePath(Key))
	{
		++ParkRefusedEditedSinceLog;
		return false;
	}

	FParkedGeometry Parked;
	Parked.PoolHandle = Rec.PoolSlot;
	Parked.OriginInPool = FVector3f(VoxelCoords::ChunkOriginWorldForLevel(Key.Key, Key.Level) - GpuPoolRebase);
	Parked.Level = Key.Level;
	Parked.QuadCount = Rec.LastQuadCount;
	Parked.GenerationId = Rec.GenerationId;
	Parked.EditEpoch = EditEpoch.load(std::memory_order_relaxed);
	Parked.ParkedAtSeconds = ElapsedSeconds;

	Pool->ParkChunk(Rec.PoolSlot);
	ParkedGeometry.Add(Key, Parked);
	ParkedInsertionOrder.Add(Key);
	++ChunksParkedSinceLog;
	++ChunksParkedTotal;

	// The record is handing the geometry over, not keeping it.
	Rec.PoolSlot = INDEX_NONE;

	EvictParkedOverCap(Cap);
	return true;
}

// T4-1. Enumerate the leading edge ahead of the PREDICTED anchor and queue what
// demand has not asked for yet.
//
// The whole feature rests on one measured fact: the GPU finishes a chunk in
// ~12 ms and then waits ~128 ms to be picked up, sitting at ~11 jobs in flight
// against a cap of 256 (docs/measurements/s0-apply-census-2026-07-27.txt). That
// idle capacity is real. Spending it early turns streaming from something the
// player watches arrive into something already there.
//
// CHEAP BY CONSTRUCTION, because admission is the largest item in the streaming
// tick before this adds anything. Level 0 only, one ring of chunks at the
// leading edge, and it early-outs to nothing when the anchor is not moving.
void FVoxelWorldImpl::EnumerateSpeculativeCandidates()
{
	using namespace VoxelCoords;

	const float LeadSec = VoxelDebug::GetStreamVelocityLeadSec();
	if (LeadSec <= 0.f)
	{
		return; // feature off
	}
	const int32 MaxParked = VoxelDebug::GetStreamSpeculativeMaxParked();
	if (MaxParked <= 0 || SpecParkedNow >= MaxParked)
	{
		return;
	}
	// Stationary means nothing to lead. The velocity EMA collapses to zero on its
	// own, so this is the same test as "is the predicted anchor the true anchor".
	const FVector Lead = PredictedAnchorLocation - LastAnchorLocation;
	if (Lead.SizeSquared() < FMath::Square(ChunkEdgeUU))
	{
		return;
	}

	// Keep the queue shallow. A deep speculative queue is stale by the time it
	// drains -- the anchor has moved and the cone points somewhere else -- and it
	// competes for the fork budget it is supposed to be scavenging.
	const int32 QueueBudget = FMath::Max(0, VoxelDebug::GetStreamSpeculativeMaxInFlight() * 4 - SpeculativeKeys.Num());
	if (QueueBudget <= 0)
	{
		return;
	}

	// LEVEL 0 ONLY. It is where the leading-edge lag is visible, where the fork
	// already runs, and where the D6 band request is wired. Coarser rings cover
	// far more ground per chunk and are correspondingly less likely to be the
	// thing a player sees pop in.
	constexpr int32 Level = 0;
	// Loop-invariant; checked once rather than per candidate.
	const bool bSpecBandSkip = VoxelStreamAdmission::BuriedSkipEnabled()
	                        && !VoxelStreamAdmission::VerifyBuriedSkipEnabled();
	const double ChunkEdge = ChunkEdgeUUForLevel(Level);
	const double OuterUU = UVoxelWorldSubsystem::GetRingPresets()[Level].OuterMeters * 100.0;

	// The band between the current R0 edge and where it will be. Walking the
	// predicted disc and skipping what is already desired would enumerate the
	// whole ring every tick; this walks only the sliver that is new.
	const FVector Dir = Lead.GetSafeNormal();
	const double LeadLen = Lead.Size();
	const int32 Span = FMath::Clamp(FMath::CeilToInt32(LeadLen / ChunkEdge) + 1, 1, 64);
	const int32 HalfWidth = FMath::Clamp(FMath::CeilToInt32(OuterUU / ChunkEdge), 1, 96);

	int32 Queued = 0;
	// March out along the velocity vector, and across the ring's width at each
	// step. Perpendicular in XY only -- the cascade is a horizontal annulus.
	const FVector Perp(-Dir.Y, Dir.X, 0.0);
	for (int32 Step = 0; Step <= Span && Queued < QueueBudget; ++Step)
	{
		const FVector Along = LastAnchorLocation + Dir * (OuterUU + double(Step) * ChunkEdge);
		for (int32 W = -HalfWidth; W <= HalfWidth && Queued < QueueBudget; ++W)
		{
			const FVector Probe = Along + Perp * (double(W) * ChunkEdge);

			// Must be inside the ring AT THE PREDICTED anchor and outside it at
			// the TRUE one -- i.e. exactly the ground admission is about to want
			// and does not yet. Anything already desired is demand's job.
			const double PredDistSq = FVector2D::DistSquared(
				FVector2D(Probe.X, Probe.Y), FVector2D(PredictedAnchorLocation.X, PredictedAnchorLocation.Y));
			if (PredDistSq >= FMath::Square(OuterUU))
			{
				continue;
			}
			const double TrueDistSq = FVector2D::DistSquared(
				FVector2D(Probe.X, Probe.Y), FVector2D(LastAnchorLocation.X, LastAnchorLocation.Y));
			if (TrueDistSq < FMath::Square(OuterUU))
			{
				continue; // already in the desired set; not speculation
			}

			const FVoxelChunkKey ColumnKey = ChunkKeyForVoxel(WorldToVoxel(Probe));

			// Z comes from the same footprint memo the entry pass uses. Reusing it
			// rather than deriving a second surface estimate is deliberate: two
			// copies of one calibration drifting apart is this repo's documented
			// recurring failure mode.
			int32 ChunkZMin = 0, ChunkZMax = 0, ChunkZMaxUntrimmed = 0;
			FootprintChunkZRangeCached(ColumnKey.X, ColumnKey.Y, Level, ChunkZMin, ChunkZMax, ChunkZMaxUntrimmed);
			if (ChunkZMax < ChunkZMin)
			{
				continue; // no surface band here
			}

			// TRIM THE BAND ENDS (voxel.Stream.SpeculativeZTrim).
			//
			// 49% of speculative dispatches return zero quads, and the empties
			// sit at BOTH ends of the surface band and never in the middle
			// (measured top=62-73, mid=0, bot=111-129). "Zero quads" covers
			// all-solid as well as all-air -- a fully buried chunk has no visible
			// faces -- which is exactly the shape of a band whose ends overshoot
			// the geometry.
			//
			// SAFE TO BE AGGRESSIVE, and that is the whole reason this is a blunt
			// trim rather than an analytic test. Speculation is OPTIONAL by
			// construction: a candidate it skips is still loaded by demand through
			// the normal path, so an over-trim costs HIT RATE and cannot cost a
			// hole. Contrast the demand-side buried skip, where being wrong means
			// missing terrain.
			//
			// The GPU capture is why this is worth doing at all: meshing is
			// 3.6-5.1 ms, 21-28% of the GPU frame, so a dispatch that produces
			// nothing is real GPU on the critical path -- not the "spare capacity
			// that was idle anyway" it was written off as.
			const int32 ZTrim = VoxelDebug::GetStreamSpeculativeZTrim();
			const int32 SpecZMin = ChunkZMin + ZTrim;
			const int32 SpecZMax = ChunkZMax - ZTrim;
			if (SpecZMax < SpecZMin)
			{
				continue; // trimmed away entirely; demand will handle this column
			}

			for (int32 Cz = SpecZMin; Cz <= SpecZMax && Queued < QueueBudget; ++Cz)
			{
				const FVoxelLevelChunkKey Key{ Level, FVoxelChunkKey{ ColumnKey.X, ColumnKey.Y, Cz } };
				if (ChunkRecords.Contains(Key) || ParkedGeometry.Contains(Key) || SpeculativeInFlight.Contains(Key))
				{
					continue;
				}
				// Edited chunks are worldgen plus an overlay; speculation only
				// knows worldgen.
				if (NeedsOverlayAwarePath(Key))
				{
					continue;
				}
				// THE SAME BURIED SKIP ADMISSION USES, and speculation was the only
				// path not applying it. Measured: 49% of speculative dispatches
				// returned zero quads, and the Z distribution of those empties was
				// top=73 mid=0 bot=128 -- clustered at BOTH ends of the surface
				// band and absent from the middle. "Zero quads" covers all-solid as
				// well as all-air (a fully buried chunk has no visible faces), which
				// is exactly the pair BandProvesChunkEmpty distinguishes, and demand
				// sees almost none of them: its census reads skipped=4064 with
				// air=4 solid=4060.
				//
				// REUSED, NOT REIMPLEMENTED. Two copies of one calibration drifting
				// apart is this repo's documented recurring failure.
				//
				// The band only exists for columns something has already meshed, so
				// this cannot fire on genuinely virgin terrain -- and that is fine:
				// the speculative cone re-enumerates the same columns for several
				// ticks as the anchor approaches, so the band is usually present by
				// the second or third look.
				if (bSpecBandSkip)
				{
					if (const VoxelStreaming::FFootprintBand* Band =
					        FootprintBandCache.Find(FIntPoint(Key.Key.X, Key.Key.Y)))
					{
						bool bAllAir = false;
						if (VoxelStreaming::BandProvesChunkEmpty(*Band, Key.Key.Z, bAllAir))
						{
							++SpecBandSkippedSinceLog;
							continue;
						}
					}
				}
				SpeculativeKeys.Add(Key);
				SpeculativeInFlight.Add(Key);
				++Queued;
			}
		}
	}
}

// T4-1. Submit speculative work into whatever the demand path left behind.
//
// Called at the END of DispatchJobs, after demand has taken what it wants. That
// ordering plus a small in-flight cap is what makes starvation structurally
// impossible rather than merely unlikely: the job manager's queue is strict
// FIFO, so anything submitted here sits ahead of the NEXT tick's demand jobs.
void FVoxelWorldImpl::DispatchSpeculativeJobs()
{
	if (SpeculativeKeys.Num() == 0 || VoxelDebug::GetStreamVelocityLeadSec() <= 0.f)
	{
		return;
	}
	if (!VoxelStreamAdmission::GpuMeshEnabled() || !GpuMeshJobs.IsValid())
	{
		// Speculation is GPU-fork only. The CPU worker arm is the deprecated
		// control path and its budget is the one demand actually needs.
		SpeculativeKeys.Reset();
		SpeculativeInFlight.Reset();
		return;
	}

	FVoxelGpuMeshJobManager* Manager = EnsureGpuMeshJobs();
	if (Manager == nullptr)
	{
		return;
	}
	const int32 SpecCap = VoxelDebug::GetStreamSpeculativeMaxInFlight();
	int32 SpecOutstanding = 0;
	for (const auto& Pair : GpuJobsPending)
	{
		if (Pair.Value.bSpeculative)
		{
			++SpecOutstanding;
		}
	}

	while (SpeculativeKeys.Num() > 0 && SpecOutstanding < SpecCap)
	{
		// DO NOT GATE ON DEMAND-QUEUE DEPTH HERE. Two predicates have already been
		// wrong in this exact spot, both because they measured how BUSY the
		// pipeline is rather than whether it has ROOM:
		//
		//   GpuJobsPending.Num() >= GpuCap - SpecCap  -- pipeline occupancy,
		//     ~252 of a 256 cap in steady state, true every tick.
		//   Manager->NumQueuedDemand() > 0            -- demand queue depth, which
		//     carries ~236 jobs for the entire flight, so also true every tick.
		//
		// Both make speculation a dead path while looking like prudence. Demand is
		// protected in the MANAGER, structurally: speculative jobs sit in their own
		// queue and are promoted only after demand has taken its full per-tick
		// allowance. That is the guarantee, and it does not need a second, weaker
		// copy of itself here.
		//
		// What this loop must bound is the SPECULATIVE queue's own depth -- a
		// backlog that outlives the prediction it was built from is stale work, not
		// lead time. SpecCap does that, and it is the only bound needed.
		if (Manager->NumQueuedLowPriority() >= SpecCap)
		{
			break;
		}
		const VoxelCoords::FVoxelLevelChunkKey Key = SpeculativeKeys.Pop(EAllowShrinking::No);

		// Re-check: demand may have admitted this key since it was enumerated,
		// in which case speculating it is duplicate work.
		if (ChunkRecords.Contains(Key) || ParkedGeometry.Contains(Key))
		{
			SpeculativeInFlight.Remove(Key);
			continue;
		}
		if (!SubmitGpuMeshJob(Key, /*GenId*/ 0, /*RingSkirtMask*/ 0, /*bSpeculative*/ true))
		{
			SpeculativeInFlight.Remove(Key);
			break; // fork refused (budget) -- stop, do not spin
		}
		++SpecOutstanding;
		++SpecDispatchedSinceLog;
		++SpecDispatchedTotal;
	}
}

// T4-1. A speculative mesh has arrived. Put it in the pool hidden and register
// it, so admission ADOPTS it instead of commissioning the same work later.
//
// This is the whole payoff: the adopt path already exists and is measured
// (S2-3), so speculation only has to deliver geometry into it. No FChunkRecord
// is created -- the record is what admission owns, and nothing has admitted this.
void FVoxelWorldImpl::ParkSpeculativeResult(const VoxelCoords::FVoxelLevelChunkKey& Key,
                                            const FVoxelGpuMeshJobResult& GpuResult)
{
	const int32 MaxParked = VoxelDebug::GetStreamSpeculativeMaxParked();
	UVoxelGpuPoolComponent* Pool = GpuPool.Get();

	// Every early-out drops the geometry, which is correct: it was never wanted,
	// and dropping speculation is always safe. The payload releases its GPU
	// memory when GpuResult goes out of scope.
	if (!Pool || MaxParked <= 0 || SpecParkedNow >= MaxParked)
	{
		return;
	}
	if (!GpuResult.GpuQuads.IsValid() || GpuResult.NumQuads == 0)
	{
		++SpecDroppedEmptySinceLog;
		// WHERE IN THE Z STACK, because that decides whether a filter is even
		// possible. If the empties cluster at the TOP of the band, they are
		// surface-height padding and an analytic test can drop them before
		// dispatch. If they are scattered through it, they are caves and
		// overhangs, and nothing short of meshing knows they are empty -- in
		// which case the honest answer is to accept the cost, since an air column
		// parks nothing and evicts nothing.
		{
			int32 ZMin = 0, ZMax = 0, ZMaxUntrimmed = 0;
			FootprintChunkZRangeCached(Key.Key.X, Key.Key.Y, Key.Level, ZMin, ZMax, ZMaxUntrimmed);
			if (ZMax >= ZMin)
			{
				const int32 Span = ZMax - ZMin;
				// 0 = bottom of the band, 100 = top. Bucketed so one counter
				// answers "top-heavy or scattered" without a histogram.
				const int32 Pct = Span > 0 ? ((Key.Key.Z - ZMin) * 100) / Span : 100;
				if (Pct >= 67)      { ++SpecEmptyTopSinceLog; }
				else if (Pct >= 34) { ++SpecEmptyMidSinceLog; }
				else                { ++SpecEmptyBotSinceLog; }
			}
		}
		return; // all-air column; nothing to park
	}
	// Demand may have overtaken it while it was in flight.
	//
	// COUNTED, because dispatched-minus-parked was 52% of all speculative work on
	// the first T4-1 leg and four silent returns could each have been the cause.
	// This one is the expensive kind: if demand admitted the key while the
	// speculative job was in flight, demand ALSO queued its own job for it, so
	// the GPU meshed the same chunk twice. That is the number that says whether
	// the lead time is too long for the pipeline's own latency.
	if (ChunkRecords.Contains(Key) || ParkedGeometry.Contains(Key))
	{
		++SpecDroppedOvertakenSinceLog;
		return;
	}

	const FVector OriginRelative = VoxelCoords::ChunkOriginWorldForLevel(Key.Key, Key.Level);
	const FVector3f OriginInPool = FVector3f(OriginRelative - GpuPoolRebase);

	// Params must be sampled HERE, while the pool entry is being written --
	// ParkChunk only zeroes the scale, so ChunkParams is never cleared and
	// UnparkChunk has nothing to restore. Getting it right once, now, is what
	// lets adoption be a single float write later.
	const int32 Slot = Pool->AddChunkFromGpu(GpuResult.GpuQuads, GpuResult.NumQuads, OriginInPool, Key.Level,
	                                         SampleChunkParamsForPool(*GpuPoolRootComponent, OriginRelative, Key.Level));
	if (Slot == INDEX_NONE)
	{
		// Pool refused. Speculation NEVER retries and never logs loudly for this:
		// a refused speculative allocation is the system correctly declining to
		// spend capacity demand might need. T1-7's retry path is for demand.
		++SpecDroppedPoolFullSinceLog;
		return;
	}

	// Hidden immediately. Between AddChunkFromGpu and here the chunk is
	// technically drawable, but both run inside one game-thread call and the
	// publication is deferred, so no frame can observe it visible.
	Pool->ParkChunk(Slot);

	FParkedGeometry Parked;
	Parked.PoolHandle = Slot;
	Parked.OriginInPool = OriginInPool;
	Parked.Level = Key.Level;
	Parked.QuadCount = int32(GpuResult.NumQuads);
	// Speculation is worldgen-only, so its generation is the base one. The edit
	// epoch is what actually invalidates it, and MarkChunkDirtyForRemesh evicts
	// parked entries directly on edit.
	Parked.GenerationId = 1;
	Parked.EditEpoch = EditEpoch.load(std::memory_order_relaxed);
	Parked.ParkedAtSeconds = ElapsedSeconds;
	Parked.bSpeculative = true;

	ParkedGeometry.Add(Key, Parked);
	ParkedInsertionOrder.Add(Key);
	++SpecParkedNow;
	++SpecParkedSinceLog;
	++SpecParkedTotal;

	// Speculative entries are capped SEPARATELY from demand parking, so a
	// speculative flood cannot evict the demand cache it depends on.
	if (SpecParkedNow > MaxParked)
	{
		EvictOldestSpeculative();
	}
}

// Evict the oldest speculative entry. Separate from EvictParkedOverCap so the
// two caps cannot cannibalise each other.
void FVoxelWorldImpl::EvictOldestSpeculative()
{
	for (int32 I = 0; I < ParkedInsertionOrder.Num(); ++I)
	{
		const VoxelCoords::FVoxelLevelChunkKey Key = ParkedInsertionOrder[I];
		const FParkedGeometry* Entry = ParkedGeometry.Find(Key);
		if (Entry && Entry->bSpeculative)
		{
			EvictParkedKey(Key);
			++SpecEvictedUnusedSinceLog;
			++SpecEvictedUnusedTotal;
			return;
		}
	}
}

// Free one parked entry for real. Used by the cap, and by the edit paths.
void FVoxelWorldImpl::EvictParkedKey(const VoxelCoords::FVoxelLevelChunkKey& Key)
{
	FParkedGeometry Parked;
	if (!ParkedGeometry.RemoveAndCopyValue(Key, Parked))
	{
		return;
	}
	if (Parked.bSpeculative)
	{
		SpecParkedNow = FMath::Max(0, SpecParkedNow - 1);
	}
	if (UVoxelGpuPoolComponent* Pool = GpuPool.Get())
	{
		// RemoveChunk on a PARKED handle is correct and is why parking did not
		// need the GPU hide pass: the range is still allocated and the handle
		// still valid, so this is the ordinary free path. The table entry it
		// neutralises is already neutral.
		Pool->RemoveChunk(Parked.PoolHandle);
	}
}

void FVoxelWorldImpl::EvictParkedOverCap(int32 Cap)
{
	if (Cap <= 0 || ParkedGeometry.Num() <= Cap)
	{
		return;
	}
	// Oldest first, from the front of the insertion queue. See
	// ParkedInsertionOrder for why this is not a scan.
	int32 Front = 0;
	while (ParkedGeometry.Num() > Cap && Front < ParkedInsertionOrder.Num())
	{
		const VoxelCoords::FVoxelLevelChunkKey Key = ParkedInsertionOrder[Front++];
		// Adopted (or edit-evicted) out from under us -- its queue slot is stale.
		if (!ParkedGeometry.Contains(Key))
		{
			continue;
		}
		EvictParkedKey(Key);
		++ParkEvictedCapSinceLog;
	}
	// Drop the consumed prefix in one move rather than RemoveAt-ing per entry.
	if (Front > 0)
	{
		ParkedInsertionOrder.RemoveAt(0, Front, EAllowShrinking::No);
	}
	// The queue accumulates stale keys for entries adopted out of the map. Compact
	// when they dominate, so it cannot grow without bound on a long session.
	if (ParkedInsertionOrder.Num() > 4 * FMath::Max(1, ParkedGeometry.Num()))
	{
		ParkedInsertionOrder.RemoveAll(
			[this](const VoxelCoords::FVoxelLevelChunkKey& K) { return !ParkedGeometry.Contains(K); });
	}
}

bool FVoxelWorldImpl::ApplyMeshResult(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material,
                                       const VoxelCoords::FVoxelLevelChunkKey& Key, VoxelStreaming::FChunkRecord& Rec,
                                       TArray<FVoxelChunkQuad>&& Quads, bool bIsGameThreadMesh,
                                       const FVoxelGpuQuadPayloadRef& GpuQuads, int32 GpuQuadCount)
{
	// WAVE D / D1: TWO WAYS TO BE HANDED A CHUNK'S GEOMETRY.
	//
	// Quads is the CPU form and is what every caller but one passes. GpuQuads is
	// a handle to quads that are already in GPU memory, and when it is set Quads
	// is EMPTY while GpuQuadCount describes the chunk. NumQuads below is the one
	// count both forms answer to; nothing past this line may ask Quads.Num().
	//
	// A GPU payload is proof the chunk was admitted as poolable at dispatch
	// (SubmitGpuMeshJob checks the renderer, the level, the record and GI before
	// asking for one), so the pooled branch below is taken UNCONDITIONALLY for
	// it -- a mid-flight voxel.Stream.GPU flip must not strand a chunk whose
	// geometry only exists somewhere the component path cannot read.
	const bool bGpuResident = GpuQuads.IsValid();
	const int32 NumQuads = bGpuResident ? GpuQuadCount : Quads.Num();

	// docs/debug-tooling-plan.md P1 "Memory" row: ResidentQuads tracks
	// currently-loaded quads (not the cumulative TotalQuadsLoaded below), so
	// it must be decremented by whatever this record held before, regardless
	// of which branch below runs.
	ResidentQuads -= Rec.LastQuadCount;
	Rec.LastQuadCount = 0;

	if (NumQuads == 0)
	{
		// No visible geometry (fully buried chunk, or an edit carved away
		// the last exposed faces): park (not destroy) any stale component --
		// M1 hitch-gap wave, same pooling path DrainUnloads uses -- rather
		// than spawning/keeping an empty one. The record stays in
		// ChunkRecords (this chunk key is still in the desired set; it might
		// gain quads again on a future edit), it just has no live component
		// until then.
		ReleaseChunkGeometry(Rec);
		// SETTLED, not "lost geometry": this chunk is genuinely empty here, so it
		// covers its footprint and may release a retained stand-in above or below
		// it. See FChunkRecord::bMeshSettled / ReplacementCovered.
		Rec.bMeshSettled = true;
		return false;
	}

	// ADR-0006 G3. A record already holding a component keeps the component
	// path even if the cvar flipped underneath it -- mixing representations for
	// one chunk is the one thing that would genuinely break, and a chunk always
	// unloads the way it loaded. New chunks take whichever path is current.
	// voxel.Stream.GPUMaxLevel: pool only rings at or below this level, leaving
	// coarser ones on the component path. Both renderers coexist per chunk, so
	// this is a real A/B and arguably a real shipping mode -- the pooling win
	// scales with chunk COUNT, which is concentrated in the dense near rings.
	static const auto* CVarGpuMaxLevel = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Stream.GPUMaxLevel"));
	const int32 GpuMaxLevel = CVarGpuMaxLevel ? CVarGpuMaxLevel->GetInt() : -1;
	const bool bLevelPoolable = (GpuMaxLevel < 0) || (Key.Level <= GpuMaxLevel);

	// bGpuResident FORCES this branch, and that is not a shortcut. A payload
	// exists only because SubmitGpuMeshJob already checked the renderer, the
	// level, the record and GI at dispatch; the quads live nowhere the component
	// path can read, so falling through to it would silently lose a chunk that
	// the fork has already counted as delivered.
	if (bGpuResident || (VoxelDebug::GetStreamGpu() && bLevelPoolable && !Rec.Component.IsValid()))
	{
		const bool bWasFirstLoad = (Rec.PoolSlot == INDEX_NONE);

		if (bGpuResident && Rec.Component.IsValid())
		{
			// The one condition SubmitGpuMeshJob checked that can genuinely have
			// changed in flight. Mixing representations for one chunk is the
			// thing that would actually break, so this refuses rather than
			// improvises -- loudly, and leaving the record UNSETTLED so it cannot
			// release a retained stand-in on the strength of geometry that was
			// dropped.
			UE_LOG(LogVoxelStream, Error,
			       TEXT("voxel.GPU.MeshDirectToPool: chunk L%d (%d,%d,%d) acquired a component while its GPU "
			            "mesh was in flight — %d quads DROPPED rather than draw the same chunk twice."),
			       Key.Level, Key.Key.X, Key.Key.Y, Key.Key.Z, NumQuads);
			return false;
		}

		// Wave S0 stage timing (docs/speculative-generation-plan.md §4, executing
		// T0-1), gated on voxel.Stream.ApplyStageStats: these are
		// FPlatformTime::Seconds pairs on a path that runs up to 64 times a frame,
		// which is exactly the kind of instrument that becomes what it measures.
		// A leg with the gate OFF, matching a leg with it ON, is part of closing
		// this wave rather than optional.
		const bool bStageStats = VoxelDebug::GetStreamApplyStageStats() != 0;

		// The CPU mesher's quads, packed into the same 8 bytes the GPU mesher
		// emits (see PackVoxelChunkQuad -- the layout is a contract with
		// VoxelQuadDecode.ush). This is the whole bridge: the pooled renderer
		// does not care which mesher produced the geometry.
		//
		// SKIPPED ENTIRELY for a GPU-resident chunk: those bytes are already in
		// the exact layout this loop produces, in GPU memory, and packing them
		// here would mean having read them back first. That round trip -- unpack
		// in OnGpuMeshJobComplete, repack here -- is what D1 deletes.
		const double PackStart = bStageStats ? FPlatformTime::Seconds() : 0.0;
		TArray<uint64> Packed;
		if (!bGpuResident)
		{
			Packed.SetNumUninitialized(Quads.Num());
			for (int32 I = 0; I < Quads.Num(); ++I)
			{
				Packed[I] = PackVoxelChunkQuad(Quads[I]);
			}
		}
		if (bStageStats)
		{
			ApplyStagePackMs += (FPlatformTime::Seconds() - PackStart) * 1000.0;
		}

		const FVector OriginRelative = VoxelCoords::ChunkOriginWorldForLevel(Key.Key, Key.Level);
		UVoxelGpuPoolComponent* Pool = GetOrCreateGpuPool(Owner, Root, Material, OriginRelative);

		// Bisection aid: cap how many chunks the pool will accept, so the
		// streamed path can be run at the same scale voxel.GPU.SpawnPool is
		// known-good at. 0 = unlimited.
		static const auto* CVarPoolCap = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Stream.GPUMaxChunks"));
		const int32 PoolCap = CVarPoolCap ? CVarPoolCap->GetInt() : 0;
		if (PoolCap > 0 && Rec.PoolSlot == INDEX_NONE && Pool->GetNumChunks() >= PoolCap)
		{
			return false;
		}

		static bool bLoggedFirstPooledChunk = false;
		if (!bLoggedFirstPooledChunk && !Quads.IsEmpty())
		{
			// CPU-form chunks only -- it prints a decoded quad, and there is no
			// decoded quad on the direct path. The first DIRECT chunk gets its own
			// line below rather than a version of this one that says nothing.
			bLoggedFirstPooledChunk = true;
			const FVoxelChunkQuad& Q = Quads[0];
			UE_LOG(LogVoxelStream, Log,
			       TEXT("voxel.Stream.GPU first pooled chunk: material=%s level=%d quads=%d "
			            "q0(axis=%d pos=%d slice=%d u0=%d v0=%d w=%d h=%d ao=%d mat=%d) packed=0x%016llx"),
			       Material ? *Material->GetName() : TEXT("<null>"), Key.Level, Quads.Num(),
			       Q.Axis, Q.Positive, Q.Slice, Q.U0, Q.V0, Q.W, Q.H, Q.Ao, Q.Mat,
			       (unsigned long long)PackVoxelChunkQuad(Q));
		}

		static bool bLoggedFirstDirectChunk = false;
		if (bGpuResident && !bLoggedFirstDirectChunk)
		{
			// The line that says the no-readback path is actually running. Without
			// it, "D1 is on" and "D1 silently fell back to readback for every
			// chunk" look identical in a log -- and the second reads as a
			// successful run with disappointing numbers.
			bLoggedFirstDirectChunk = true;
			UE_LOG(LogVoxelStream, Log,
			       TEXT("voxel.GPU.MeshDirectToPool: first chunk written GPU-side — level=%d chunk=(%d,%d,%d) "
			            "quads=%d, no readback, no CPU staging, no re-upload."),
			       Key.Level, Key.Key.X, Key.Key.Y, Key.Key.Z, NumQuads);
		}
		// Relative to the pool's own rebase origin, not the world -- see
		// GetOrCreateGpuPool. Climate still samples at the ABSOLUTE position.
		const FVector OriginInPool = OriginRelative - GpuPoolRebase;

		// HOISTED OUT OF THE TWO CALLS BELOW so it can be timed on its own, and
		// computed on exactly the branches that used to compute it -- the
		// UpdateChunk path never sampled params and still must not, or this
		// instrument would add a full Amplifier::column to every re-mesh.
		//
		// §1c is the reason it gets its own bucket: SampleChunkParamsForPool runs
		// GetSurfaceHeightUU, which is a whole column with the cave lattice and
		// cavern passes, on the GAME THREAD, once per applied chunk -- for a value
		// the producing job already computed. If this bucket is large, T1-3's
		// column cache moves up the queue.
		const bool bNeedsParams = bGpuResident || bWasFirstLoad;
		const double ParamsStart = bStageStats ? FPlatformTime::Seconds() : 0.0;
		const FVector4f PoolParams = bNeedsParams
			? SampleChunkParamsForPool(Root, OriginRelative, Key.Level)
			: FVector4f(0.f, 0.f, 0.f, 0.f);
		if (bStageStats && bNeedsParams)
		{
			ApplyStageParamsMs += (FPlatformTime::Seconds() - ParamsStart) * 1000.0;
		}

		const double PoolAddStart = bStageStats ? FPlatformTime::Seconds() : 0.0;
		if (bGpuResident)
		{
			// ONLY THE FIRST-LOAD SHAPE EXISTS ON THIS PATH, and it is not an
			// omission. The fork takes unedited chunks only; a re-mesh is an edit
			// and goes through the overlay-aware game-thread path, which produces
			// CPU quads. So a payload always arrives at a record with no pool
			// slot, and UpdateChunkFromGpu would be code with no caller -- which
			// is how it would rot. The check below is what says so if that ever
			// stops being true.
			if (!bWasFirstLoad)
			{
				UE_LOG(LogVoxelStream, Error,
				       TEXT("voxel.GPU.MeshDirectToPool: chunk L%d (%d,%d,%d) already holds pool slot %d — the "
				            "direct path has no in-place update, so %d quads were DROPPED. A re-mesh reached "
				            "the fork, which is supposed to be impossible."),
				       Key.Level, Key.Key.X, Key.Key.Y, Key.Key.Z, Rec.PoolSlot, NumQuads);
				return false;
			}
			Rec.PoolSlot = Pool->AddChunkFromGpu(
				GpuQuads, uint32(NumQuads), FVector3f(OriginInPool), Key.Level, PoolParams);
		}
		else if (bWasFirstLoad)
		{
			Rec.PoolSlot = Pool->AddChunk(
				Packed, FVector3f(OriginInPool), Key.Level, PoolParams);
		}
		else
		{
			// Re-mesh in place. UpdateChunk reuses the existing range whenever
			// the new quad count fits it, which is the common case for a dig --
			// free+realloc would fragment the pool hardest on exactly the
			// chunks that re-mesh most often.
			Rec.PoolSlot = Pool->UpdateChunk(Rec.PoolSlot, Packed);
		}
		if (bStageStats)
		{
			// Includes the PushUpdatesToProxy each of those three ends in -- which
			// is the point. The pool's own GetAndResetPushStats splits that cost
			// into its table-copy and BuildChunkRuns halves; this is the total the
			// apply loop's 6ms wall-clock budget actually sees.
			ApplyStagePoolAddMs += (FPlatformTime::Seconds() - PoolAddStart) * 1000.0;
			++AppliesTimedSinceLog;
		}

		if (Rec.PoolSlot == INDEX_NONE)
		{
			// Out of contiguous room. Do NOT mark this settled: a settled
			// record is allowed to release a retained stand-in, and releasing
			// one in favour of a chunk that failed to allocate turns a
			// capacity problem into a visible hole. Left unsettled and
			// geometry-less, which is the honest state.
			//
			// Free vs largest-run is the diagnosis: close together means the
			// pool is genuinely full and wants more capacity; far apart means
			// it is fragmented and wants compaction. Very different fixes.
			//
			// Identical for a GPU-resident chunk, deliberately: a full pool drops
			// geometry the same way whichever mesher produced it, and the payload
			// simply goes out of scope, releasing its GPU memory. The one
			// difference is that AddChunkFromGpu also returns INDEX_NONE when the
			// pool is not yet GPU-writable, which is the session's first few
			// chunks and is self-correcting.
			UE_LOG(LogVoxelStream, Warning,
			       TEXT("voxel.Stream.GPU: no room for %d quads at level %d (%u free, largest run %u)%s. "
			            "Chunk left undrawn."),
			       NumQuads, Key.Level, Pool->GetFreeQuads(), Pool->GetLargestFreeRun(),
			       bGpuResident ? TEXT(" [GPU-resident]") : TEXT(""));
			return false;
		}

		if (bWasFirstLoad)
		{
			++TotalChunksLoaded;
			++LevelChunksLoadedTotal[FMath::Clamp(Key.Level, 0, VoxelCoords::kNumLevels - 1)];
			Rec.LoadedAtSeconds = ElapsedSeconds;
		}
		else if (bIsGameThreadMesh)
		{
			Rec.RemeshedAtSeconds = ElapsedSeconds;
		}

		TotalQuadsLoaded += NumQuads;
		Rec.LastQuadCount = NumQuads;
		ResidentQuads += Rec.LastQuadCount;
		Rec.bMeshSettled = true;
		Rec.bHasOverlayBricks =
			(Key.Level == 0) ? ChunkOwnsEditedBrick(Key.Key) : EditedAncestorChunks[Key.Level].Contains(Key.Key);

		// M4 voxel GI, pooled equivalent of SetChunkQuads' NotifyChunkMeshUpdated.
		// This branch returns without ever constructing a UVoxelChunkComponent,
		// so the component hook cannot fire here and, until this call existed,
		// voxel.Stream.GPU 1 left the light field completely EMPTY -- no chunk
		// voxelized, no brick solved, voxel.GI.Enabled 1 a silent no-op instead
		// of a visible difference.
		//
		// Placed after the pool accepted the geometry (an INDEX_NONE slot
		// returns above) and after the last read of Quads, since the quads are
		// moved out: the GI queue has to own them, there being no component to
		// read them back off. The origin is the chunk's WORLD origin, the same
		// quantity GetComponentLocation() yields on the component path and
		// composed the same way SampleChunkParamsForPool composes it.
		// With voxel.GI.Enabled 0 this is one cvar read and an immediate
		// return, before the move.
		//
		// A GPU-RESIDENT CHUNK NEVER REACHES HERE WITH GEOMETRY GI WANTED, and
		// that is arranged at dispatch rather than defended here: SubmitGpuMeshJob
		// asks UVoxelGISubsystem::WantsChunkQuads before requesting a payload, so
		// any chunk GI would ingest came back through the readback path with real
		// Quads. If it did somehow arrive direct, Quads is empty and
		// NotifyPooledChunkMeshUpdated's own `Quads.Num() == 0` test drops it --
		// the safe direction, and the reason that test is worth having twice.
		if (UWorld* World = VoxelGI::IsEnabled() ? Owner.GetWorld() : nullptr)
		{
			if (UVoxelGISubsystem* GI = World->GetSubsystem<UVoxelGISubsystem>())
			{
				GI->NotifyPooledChunkMeshUpdated(Root.GetComponentLocation() + OriginRelative,
				                                 Key.Level, MoveTemp(Quads));
			}
		}
		return bWasFirstLoad;
	}

	// NOT TIMED, deliberately. The component-renderer branch is the CONTROL arm,
	// and S0's question is entirely about the pooled path -- §1a's claim is that
	// the POOLED apply carries an O(resident) tax the component apply does not.
	// The comparison that answers it is already the head-to-head's avgChunks/s;
	// a stage split here would need its own timed-apply counter and a scope guard
	// to survive this branch's early returns, for a number nothing in the wave
	// reads. Add it if and only if a leg raises a question about this arm.
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
		// voxel.Render.CastShadow A/B knob. Set HERE, alongside the other
		// per-load properties, so a pooled component that previously loaded under
		// the opposite value is reset too (the "indistinguishable from a fresh
		// component" bar this block exists to hold).
		Comp->SetCastShadow(VoxelDebug::GetRenderCastShadow());
		Rec.Component = Comp;
		++TotalChunksLoaded;
		++LevelChunksLoadedTotal[FMath::Clamp(Key.Level, 0, VoxelCoords::kNumLevels - 1)];
	}
	// NumQuads rather than Quads.Num(): identical on this path (a GPU-resident
	// chunk can never reach it -- bGpuResident forces the pooled branch above)
	// and one fewer place that would need changing if that ever stopped holding.
	TotalQuadsLoaded += NumQuads;
	Rec.LastQuadCount = NumQuads;
	ResidentQuads += Rec.LastQuadCount;
	Rec.bMeshSettled = true; // geometry final: may release a retained finer/coarser stand-in
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
	// Streaming-speed pass (2026-07-24): applies are driven by a per-frame
	// WALL-CLOCK budget, with MaxApplies as a hard safety ceiling. The old fixed
	// count of 3 starved fill to ~180 chunks/s (minutes to fill the cascade,
	// 1-2 min bare-terrain lag on every LOD upgrade). Now: always apply a small
	// floor (forward progress even if the first chunk is slow), then keep
	// applying while under ApplyBudgetMs. During a load storm the queue is deep
	// so this drains hard (fast fill, at the cost of the odd render-thread hitch
	// -- the accepted trade); in steady state the queue is near-empty so neither
	// the budget nor the ceiling binds and frame pacing is untouched. Budget is
	// game-thread wall-clock; the true proxy/GPU-upload cost partly lands on the
	// render thread a frame or two later, so ApplyBudgetMs is deliberately below
	// a full frame to leave that backlog room.
	const double ApplyBudgetSeconds = double(VoxelDebug::GetStreamApplyBudgetMs()) / 1000.0;
	const double ApplyLoopStart = FPlatformTime::Seconds();
	constexpr int32 kMinAppliesPerFrame = 4; // forward-progress floor, budget-independent
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
	// Wave S0: which of the four exits this frame took (see the DrainExit*
	// counters). Set by the two `break`s; left as None if the loop fell out of
	// its own condition, which is then attributed after the loop. There is no
	// fifth exit -- the loop body's only other jump is a `continue`.
	enum class EDrainExit : uint8 { None, QueueEmpty, WallClock };
	EDrainExit ExitReason = EDrainExit::None;
	VoxelStreaming::FJobResult Result;
	while (Applied < MaxApplies && Drains < kMaxResultDrainsPerFrame)
	{
		// Wall-clock apply budget (see ApplyBudgetSeconds comment). Checked at
		// the top of each iteration AFTER the min-applies floor: stale discards
		// below are cheap and don't count against the render budget, but they DO
		// count toward the wall-clock guard, which is correct -- a frame that
		// spends its whole budget shovelling stale results should still yield.
		//
		// THE BUDGET TEST MUST COME BEFORE THE DEQUEUE, and this used to be the
		// other way round -- Dequeue() sat in the loop condition, so an
		// over-budget frame popped a result off the MPSC queue and then broke
		// out of the body before doing anything with it. TQueue has no push-back
		// and nothing re-enqueued it, so that result was destroyed on return,
		// silently: the drop happened before ++ResultsDrainedSinceLog, so no
		// counter moved. It took the whole tail of the body with it --
		// FootprintBandCache.Add, FootprintBlindJobInFlight.Remove,
		// --LevelJobsInFlight[Lvl], and Rec->bJobInFlight = false -- which is
		// exactly the state the R0 freeze was diagnosed as: a footprint absent
		// from the band cache AND present in the blind-job set is the precise
		// conjunction the cold-band throttle defers on forever, and one dropped
		// result creates both halves in a single step. That is why moving where
		// the mark is SET could never fix this instance: the leak is entirely on
		// the result side. The 5 s mark age-out added alongside it is a backstop
		// for the symptom; this is the cause, and it strands the chunk itself
		// (pinned bJobInFlight, never re-dispatched) whether or not the mark is
		// involved, on the component path as much as the pooled one.
		if (Applied >= kMinAppliesPerFrame &&
		    (FPlatformTime::Seconds() - ApplyLoopStart) >= ApplyBudgetSeconds)
		{
			ExitReason = EDrainExit::WallClock;
			break;
		}
		if (!ResultsQueue.Dequeue(Result))
		{
			ExitReason = EDrainExit::QueueEmpty;
			break;
		}
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

		// S0-3: WorkerJobMsWindow above mixes both producers (OnGpuMeshJobComplete
		// sets Result.JobMs = SubmitToDeliverMs too). This isolates the CPU-only
		// population, gated with the rest of S0-3's bookkeeping.
		if (VoxelDebug::GetStreamLatencyStats() && !Result.bFromGpuMesh)
		{
			PushLatencyMsSample(CpuWorkerEndToEndMsWindow, CpuWorkerEndToEndMsWindowNext,
			                    CpuWorkerEndToEndMsWindowCount, Result.JobMs);
		}

		// Buried-chunk pre-dispatch skip: record this footprint's band. Done
		// BEFORE the stale-result discard below, because the band is a pure
		// function of (X,Y) and the amplifier -- it is equally true whether or
		// not this particular chunk is still wanted, and a stale result is
		// exactly as good a source for it as a live one.
		if (Result.bBandValid)
		{
			FootprintBandCache.Add(FIntPoint(Result.Key.Key.X, Result.Key.Key.Y), Result.Band);
		}

		// Cold-band throttle: this footprint's seeding job has landed, so release
		// its column-mates. Cleared for EVERY level-0 result, band or not (and
		// before the stale-result discard, same reasoning as the band above) --
		// otherwise a result that yielded no band would strand the whole column
		// behind a mark that nothing ever removes.
		if (Result.Key.Level == 0)
		{
			const FIntPoint DoneFootprint(Result.Key.Key.X, Result.Key.Key.Y);
			FootprintBlindJobInFlight.Remove(DoneFootprint);
			// Same site, same condition, so the GPU tag can never outlive the
			// mark it annotates (Wave D / D4).
			FootprintBlindJobIsGpu.Remove(DoneFootprint);
		}

		// -VoxelVerifyBuriedSkip soundness check: this chunk's band verdict
		// claimed "provably no geometry" and we dispatched it anyway. If it
		// produced quads, the bound is not conservative and the skip would
		// have deleted visible geometry. Logged as an Error with the full key
		// so it is impossible to miss in a headless run's log, and counted so
		// the run can report a hard zero.
		//
		// QuadCount(), NOT Quads.Num(), HERE AND EVERYWHERE BELOW (Wave D / D1).
		// A GPU-meshed chunk's quads never come to the CPU, so Quads is empty for
		// a chunk that meshed to thousands. Reading it would make every skip
		// verifier below incapable of ever firing -- they only trip on a NON-ZERO
		// count -- so an unsound skip would stop being detected at exactly the
		// moment the fork became the producer. Silent, and it would read as the
		// verifiers passing.
		if (Result.bPredictedEmpty)
		{
			++BuriedVerifyCheckedSinceLog;
			if (Result.QuadCount() > 0)
			{
				++BuriedVerifyViolations;
				UE_LOG(LogVoxelPerf, Error,
				       TEXT("Voxel buried skip UNSOUND: chunk L%d (%d,%d,%d) was predicted empty but meshed %d quads"),
				       Result.Key.Level, Result.Key.Key.X, Result.Key.Key.Y, Result.Key.Key.Z, Result.QuadCount());
			}
		}

		// -VoxelVerifySolidSkip soundness check, the mirror of the one above.
		// This chunk was claimed provably ALL SOLID at admission -- i.e. the
		// shipped path would never have tracked it at all -- and admitted
		// anyway. If it produced quads, the all-solid bound or the chunk-Z
		// arithmetic over it is wrong, and the skip would have left a hole in
		// the world that no record even points at.
		if (SolidSkipVerifyKeys.Remove(Result.Key) > 0)
		{
			++SolidVerifyCheckedSinceLog;
			if (Result.QuadCount() > 0)
			{
				++SolidVerifyViolations;
				UE_LOG(LogVoxelPerf, Error,
				       TEXT("Voxel solid skip UNSOUND: chunk L%d (%d,%d,%d) was predicted all-solid at admission ")
				       TEXT("but meshed %d quads"),
				       Result.Key.Level, Result.Key.Key.X, Result.Key.Key.Y, Result.Key.Key.Z, Result.QuadCount());
			}
		}

		// Buried-chunk skip census. Counted here -- BEFORE the stale-result
		// discard below -- deliberately: the question is what fraction of
		// WORKER OUTPUT meshes to zero quads, and a job whose result is later
		// discarded as stale still did the full generate+mesh. Counting only
		// live results would understate exactly the waste this is measuring.
		{
			const int32 Lvl = FMath::Clamp(Result.Key.Level, 0, VoxelCoords::kNumLevels - 1);
			++LevelResultsSinceLog[Lvl];
			const int32 ResultQuads = Result.QuadCount();
			(ResultQuads == 0 ? LevelZeroQuadMsSinceLog[Lvl] : LevelQuadMsSinceLog[Lvl]) += Result.JobMs;

			// S0-3 (docs/speculative-generation-plan.md Wave S0 / T0-2): the
			// per-level quad-count distribution -- free here, ResultQuads and Lvl
			// are already computed for the census above. Sizes the speculative
			// pool reserve a later wave needs; see the bucket-boundary reasoning
			// at the log site in MaybeLogCounters. Every delivered result counts,
			// live or stale, for the same reason the census above does: a result
			// later discarded as stale still meshed to a real quad count.
			if (VoxelDebug::GetStreamLatencyStats())
			{
				++LevelQuadCountSinceLog[Lvl];
				LevelQuadSumSinceLog[Lvl] += ResultQuads;
				int32 Bucket;
				if (ResultQuads == 0)             { Bucket = 0; }
				else if (ResultQuads <= 255)      { Bucket = 1; }
				else if (ResultQuads <= 1023)     { Bucket = 2; }
				else if (ResultQuads <= 4095)     { Bucket = 3; }
				else if (ResultQuads <= 16383)    { Bucket = 4; }
				else                              { Bucket = 5; }
				++LevelQuadHistSinceLog[Lvl][Bucket];
			}
			if (ResultQuads == 0)
			{
				++LevelZeroQuadSinceLog[Lvl];
				switch (Result.EmptyClass)
				{
				case 1: ++LevelAllAirSinceLog[Lvl]; break;
				case 2: ++LevelAllSolidSinceLog[Lvl]; break;
				case 3: ++LevelMixedEmptySinceLog[Lvl]; break;
				default: break; // census switch off
				}
			}
			if (Lvl == 0)
			{
				AccumLevel0GridMs += Result.GridMs;
				AccumLevel0JobMs += Result.JobMs;
				AccumLevel0GridCycles += Result.GridCycles;
				AccumLevel0JobCycles += Result.JobCycles;
				AccumLevel0BricksSkippedAir += Result.BricksSkippedAir;
				AccumLevel0BricksSkippedSolid += Result.BricksSkippedSolid;
				++AccumLevel0Jobs;
			}
			// Per-ring in-flight bookkeeping (see LevelJobsInFlight): every job
			// DispatchJobs launches produces exactly one result, live or stale,
			// so this is the matching decrement and must happen BEFORE the
			// stale `continue` below or a ring would leak slots against its
			// floor.
			LevelJobsInFlight[Lvl] = FMath::Max(0, LevelJobsInFlight[Lvl] - 1);
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

		// -VoxelVerifySkyBand: the verdict was computed at dispatch but the job
		// ran anyway, so this is the real mesh to check it against. A violation
		// is a chunk the skip would have dropped that in fact had geometry --
		// exactly the "silently vanishing R4 chunk" failure this whole change
		// has to be proven not to cause. Logged as an Error, one line per
		// violation, and counted for the run summary.
		if (VoxelSkyBand::GetVerifyEnabled())
		{
			const int32 VLvl = FMath::Clamp(Result.Key.Level, 0, VoxelCoords::kNumLevels - 1);
			++VerifyChunksChecked;
			++VerifyResultsByLevel[VLvl];
			if (VerifyPredictedAirKeys.Remove(Result.Key) > 0)
			{
				++VerifyAirPredictions;
				++VerifyAirPredictionsByLevel[VLvl];
				if (Result.QuadCount() != 0)
				{
					++VerifyAirViolations;
					UE_LOG(LogVoxelPerf, Error,
					       TEXT("VoxelVerifySkyBand VIOLATION: level=%d chunk=(%d,%d,%d) was predicted all-air but meshed %d quads"),
					       Result.Key.Level, Result.Key.Key.X, Result.Key.Key.Y, Result.Key.Key.Z, Result.QuadCount());
				}
			}
		}

		Rec->bJobInFlight = false;
		const int32 AppliedQuads = Result.QuadCount();
		ZeroQuadAppliesSinceLog += (AppliedQuads == 0) ? 1 : 0;
		if (AppliedQuads == 0)
		{
			++LevelZeroQuadTotal[FMath::Clamp(Result.Key.Level, 0, VoxelCoords::kNumLevels - 1)];
		}
		++Applied; // a live result: this IS a render-thread-facing apply
		// Both forms are handed over: exactly one of them is populated, and
		// ApplyMeshResult picks its branch on which. The payload is passed by
		// const reference and its refcount released when Result goes out of
		// scope at the top of the next iteration.
		if (ApplyMeshResult(Owner, Root, Material, Result.Key, *Rec, MoveTemp(Result.Quads),
		                    /*bIsGameThreadMesh*/ false, Result.GpuQuads, int32(Result.GpuQuadCount)))
		{
			++ProxiesCreated;
		}

		// S0-3 (docs/speculative-generation-plan.md Wave S0 / T0-2):
		// DeliverToApplyMs, immediately AFTER the apply rather than inside
		// ApplyMeshResult itself -- that function is being edited concurrently
		// elsewhere in this wave. Result.DeliverSeconds is 0.0 unless
		// voxel.Stream.LatencyStats was ALSO on back when this result was made
		// (see FJobResult::DeliverSeconds -- both producers gate that stamp on
		// the same cvar this line checks), so this only measures a result that
		// was actually stamped, never a false "instant apply".
		if (VoxelDebug::GetStreamLatencyStats() && Result.DeliverSeconds > 0.0)
		{
			const float DeliverToApplyMs = float((FPlatformTime::Seconds() - Result.DeliverSeconds) * 1000.0);
			if (Result.bFromGpuMesh)
			{
				PushLatencyMsSample(GpuDeliverToApplyMsWindow, GpuDeliverToApplyMsWindowNext,
				                    GpuDeliverToApplyMsWindowCount, DeliverToApplyMs);
			}
			else
			{
				PushLatencyMsSample(CpuDeliverToApplyMsWindow, CpuDeliverToApplyMsWindowNext,
				                    CpuDeliverToApplyMsWindowCount, DeliverToApplyMs);
			}
		}
	}
	// Wave S0 exit attribution. The two breaks named themselves; falling out of
	// the loop condition means one of the two caps bound, and Applied is checked
	// first because it is the render-facing one -- if both are at their ceiling in
	// the same frame, the count cap is the one that matters.
	switch (ExitReason)
	{
	case EDrainExit::QueueEmpty: ++DrainExitQueueEmptySinceLog; break;
	case EDrainExit::WallClock:  ++DrainExitWallClockSinceLog;  break;
	default:
		// No trailing `else`, on purpose. Falling out of the loop with NEITHER cap
		// at its ceiling is impossible -- the body's only exits are the two breaks
		// above and a `continue` -- and charging an impossible case to one of the
		// four real buckets would make this census quietly wrong instead of
		// visibly short. The four counters summing to less than the frame count IS
		// the tell that an exit path exists that nobody has accounted for.
		if (Applied >= MaxApplies)                   { ++DrainExitCountCapSinceLog; }
		else if (Drains >= kMaxResultDrainsPerFrame) { ++DrainExitDrainCapSinceLog; }
		break;
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
	RetainHeldThisFrame = 0; // recounted from scratch every frame (it is a level, not a total)
	int32 ComponentUnloads = 0; // render-thread-facing pool-parks this frame -- gated by MaxUnloads
	int32 Pops = 0;             // total queue pops incl. free component-less evictions -- gated by kMaxUnloadPopsPerFrame
	TArray<VoxelCoords::FVoxelLevelChunkKey> Deferred; // component-bearing unloads beyond MaxUnloads this frame

	// voxel.Stream.LogRetention (ring-gap wave): name the releases the census
	// line can only count. Radius from the anchor is the point of it -- the
	// hypothesis is that covered-absent releases cluster at a ring's INNER edge,
	// and that is a claim about a distance, not a total. Absent/Settled are -1
	// for a cap release (no coverage query was made). The cvar read is hoisted
	// out of the loop: this runs per popped unload and the loop pops up to 1024.
	const bool bLogRetention = VoxelDebug::GetStreamLogRetention();
	const auto LogRetentionRelease = [this, bLogRetention](const VoxelCoords::FVoxelLevelChunkKey& K, const TCHAR* Why,
	                                                       uint8 Dir, int32 Absent, int32 SettledCount)
	{
		if (!bLogRetention)
		{
			return;
		}
		const double ChunkEdge = VoxelCoords::ChunkEdgeUUForLevel(K.Level);
		const double CenterX = (double(K.Key.X) + 0.5) * ChunkEdge;
		const double CenterY = (double(K.Key.Y) + 0.5) * ChunkEdge;
		const double RadiusM =
			FMath::Sqrt(FMath::Square(CenterX - LastAnchorLocation.X) + FMath::Square(CenterY - LastAnchorLocation.Y)) /
			100.0;
		// `dir` is which side the replacement is on, and it is what says whether
		// the absent keys below are 8 children or 1 parent.
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("  retention release %s R%d (%d,%d,%d) r=%.0fm dir=%s absent=%d settled=%d"),
		       Why, K.Level, K.Key.X, K.Key.Y, K.Key.Z, RadiusM,
		       Dir == RetainDir_Finer ? TEXT("finer") : (Dir == RetainDir_Coarser ? TEXT("coarser") : TEXT("none")),
		       Absent, SettledCount);
	};

	// Which retention verdict a popped chunk got, carried from where it is
	// DECIDED (before the unload budget) to where the chunk actually PARKS (after
	// it) so a budget-deferred chunk cannot re-count its release every frame. See
	// the counter block below.
	enum class ERetentionVerdict : uint8
	{
		NotRetained,    // no RetainReplaceDir: an ordinary eviction, nothing to report
		Cap,            // LodRetentionMs elapsed with the replacement still not covering
		CoveredSettled, // every consulted replacement record existed and had settled
		CoveredAbsent,  // some consulted record was absent AND provably not desired
	};

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
		if (!PendingUnloadSet.Contains(Key))
		{
			// Resurrected: the entry pass re-desired this footprint while the
			// unload was queued and cancelled it (see AddCandidate). The record
			// lives on as an ordinary tracked chunk; this queue entry is inert.
			continue;
		}

		// Held geometry here means this pop costs something render-facing --
		// a RemovePrimitive on the component path, a pool free plus an
		// incremental buffer write on the GPU path -- so gate it either way. Over budget this frame: keep the record
		// tracked and re-queue it for the next frame (do NOT drop the chunk).
		if (Rec->HoldsGeometry())
		{
			// Load-before-unload: keep this chunk drawn as a stand-in until its
			// replacement LOD actually covers its footprint (coverage-based
			// release -- no fixed-timer rolling ring of holes). The park is
			// deferred while: still inside the safety cap (voxel.Stream.
			// LodRetentionMs, a backstop against a never-covered footprint such as
			// a coastal all-ocean quarter) AND this was an LOD transition AND the
			// replacement is not yet on screen. Once covered -- or the safety cap
			// elapses -- it falls through to the normal park. Chunks that were
			// never retained have RetainUntilSeconds far in the past, so they park
			// immediately as before.
			//
			// The counters below are the whole diagnostic for this mechanism
			// (there was none before, which is why the first cut's two bugs could
			// only be found by reading source). Read them as:
			//   held         -- stand-ins currently drawn waiting on a replacement
			//   covRelSettled-- released because the replacement RECORDS EXISTED
			//                   and had all settled: the clean, sound release
			//   covRelAbsent -- released because the consulted replacement records
			//                   were absent AND provably not desired (out of the
			//                   replacement footprint's Z range, or outside its
			//                   ring's padded annulus at the current anchor)
			//   capRel       -- released because the SAFETY CAP expired first
			//
			// capRel large => the replacement is genuinely not streaming in inside
			// LodRetentionMs, which is a THROUGHPUT problem (the ADR-0006 funnel),
			// not a retention bug: retention can only delay a hole, never
			// manufacture a chunk.
			//
			// WHAT TO WATCH CHANGED WITH THE 2026-07-27 RING-GAP FIX. covRelAbsent
			// used to be this wave's headline: absence was treated as coverage
			// unconditionally, so the counter mixed sound absences in with the
			// ring-gap holes and a large value was a hole SUSPICION. It no longer
			// is. ReplacementCovered now BLOCKS on an absent-but-desired
			// replacement, so that population moved into `held`, and covRelAbsent
			// counts only absences it could prove legitimate. The two numbers to
			// watch are now:
			//   held   -- if this climbs and stays up, replacements are not
			//             arriving; the stand-ins are doing their job but the
			//             funnel behind them is the problem.
			//   capRel -- a stand-in that reached the safety cap still holding is
			//             the case where a hole CAN still appear. Zero flight-phase
			//             capRel was the acceptance bar for LodRetentionMs=20000.
			// Still split per level: "R4 releasing at its inner edge" and "R0
			// releasing on an absent parent" are different bugs a single total
			// cannot tell apart.
			//
			// COUNTED AT THE PARK, NOT AT THE VERDICT (2026-07-27). The verdict is
			// decided here, but the release counters fire below, past the
			// MaxUnloads budget check. They used to fire here -- so a chunk whose
			// park was deferred by the unload budget re-counted its release on
			// every frame until the budget let it through, which is how one 5 s
			// window reported capRel=65,478 against held~1,010. The verdict rides
			// down in a local instead. `held` is unaffected: it is recounted from
			// scratch each frame and is a level, not a rate.
			ERetentionVerdict Verdict = ERetentionVerdict::NotRetained;
			int32 VerdictAbsentCount = 0;
			int32 VerdictSettledCount = 0;
			if (Rec->RetainReplaceDir != RetainDir_None)
			{
				if (ElapsedSeconds >= Rec->RetainUntilSeconds)
				{
					Verdict = ERetentionVerdict::Cap;
				}
				else
				{
					bool bAnyAbsent = false;
					if (!ReplacementCovered(ChunkRecords, Key, Rec->RetainReplaceDir, Rec->RetainChildZMask,
					                        LastAnchorLocation, &bAnyAbsent, &VerdictAbsentCount, &VerdictSettledCount))
					{
						++RetainHeldThisFrame;
						Deferred.Add(Key); // stays tracked + visible, retried next frame
						continue;
					}
					Verdict = bAnyAbsent ? ERetentionVerdict::CoveredAbsent : ERetentionVerdict::CoveredSettled;
				}
			}
			// The retention gate stays AHEAD of the budget check: a HELD chunk must
			// not consume an unload slot it is not going to use, or the real
			// unloads starve behind stand-ins that are still waiting.
			if (ComponentUnloads >= MaxUnloads)
			{
				Deferred.Add(Key); // stays in PendingUnloadSet, re-added below
				continue;
			}
			// Past every deferral: this chunk parks THIS frame, exactly once.
			{
				const int32 RetLevel = FMath::Clamp(Key.Level, 0, VoxelCoords::kNumLevels - 1);
				switch (Verdict)
				{
				case ERetentionVerdict::Cap:
					++RetainCapReleasesSinceLog;
					++LevelRetainCapReleases[RetLevel];
					LogRetentionRelease(Key, TEXT("cap"), Rec->RetainReplaceDir, /*Absent*/ -1, /*Settled*/ -1);
					break;
				case ERetentionVerdict::CoveredAbsent:
					++RetainCoveredAbsentReleasesSinceLog;
					++LevelRetainCoveredAbsent[RetLevel];
					LogRetentionRelease(Key, TEXT("covered-absent"), Rec->RetainReplaceDir, VerdictAbsentCount,
					                    VerdictSettledCount);
					break;
				case ERetentionVerdict::CoveredSettled:
					++RetainCoveredSettledReleasesSinceLog;
					break;
				default:
					break; // never retained: nothing to report
				}
			}
			// Any worker job still in flight for this key keeps running to
			// completion (it can't be cancelled); when its result arrives,
			// DrainResults finds no record for the key and discards it.
			// M1 hitch-gap wave: park (not destroy) -- see
			// ReturnChunkComponentToPool / docs/status.md M1 gate row.
			//
			// S2-3: try to keep the POOL RANGE first. Under motion the ring
			// boundaries oscillate and this same footprint is very often
			// re-admitted seconds later; parking makes that a table write
			// instead of a full mesh round trip. ParkChunkGeometry returns
			// false (and geometry is freed normally) when parking is off, when
			// the chunk is unsettled or edited, or when it is a component-path
			// chunk. Either way the record stops holding geometry here.
			if (!ParkChunkGeometry(Key, *Rec))
			{
				ReleaseChunkGeometry(*Rec);
			}
			++ComponentUnloads;
		}

		PendingUnloadSet.Remove(Key);
		ResidentQuads -= Rec->LastQuadCount;
		ChunkRecords.Remove(Key);
		++TotalChunksUnloaded;
		++RecordsEvictedSinceLog;
		++LevelRecordsEvicted[FMath::Clamp(Key.Level, 0, VoxelCoords::kNumLevels - 1)];
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
	//
	// S2-3 FIRST, AND UNCONDITIONALLY: a parked chunk has no record, so every
	// staleness path below is blind to it. This function does not release
	// geometry -- it bumps GenerationId and re-queues -- so without dropping the
	// parked copy here, an edited chunk would later un-park with its PRE-EDIT
	// shape and nothing in the system would report it. Cheap when parking is off
	// (one empty-map lookup).
	EvictParkedKey(LevelKey);

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
	// A dirtied chunk is never worker-dispatched; it moves to the game-thread
	// (overlay-aware) queue below.
	PendingJobKeysByLevel[FMath::Clamp(LevelKey.Level, 0, VoxelCoords::kNumLevels - 1)].RemoveAll(
	    [&LevelKey](const FSortEntry& E) { return E.Key == LevelKey; });
	PendingGameThreadKeys.AddUnique(LevelKey);
}

void FVoxelWorldImpl::RebuildEditedFootprintsFromOverlay()
{
	using namespace VoxelCoords;
	constexpr int32 BricksPerChunk = ChunkEdgeBricks;

	// vxc::World::replay writes the overlay DIRECTLY -- it is the edit log
	// replaying itself, not ApplyGroupedEdits -- so nothing on the UE side that
	// PropagateEditToMips maintains exists after a load. EditedFootprintMaxZ,
	// EditedFootprintMinZ and EditedAncestorChunks all come back empty, and
	// every escape hatch keyed on them silently reverts to worldgen-only
	// behaviour for edits that were saved rather than made this session.
	//
	// Both hatches are holes when that happens: the sky hatch loses a saved
	// structure standing above the trimmed band, and the downward hatch loses
	// the bottom of a saved shaft. Neither is visible until somebody reloads,
	// which is exactly the kind of bug that survives a whole wave of testing.
	//
	// Brick granularity with ONE CHUNK of margin on every axis: the same
	// conservative reach ChunkHasEditedBrick already scans with, and coarser
	// than CollectDirtyChunks' per-voxel border test. Over-wide by design --
	// a false entry costs a handful of admitted chunks in one footprint, a
	// missing one costs terrain.
	const vxc::ChunkMap<BrickEdgeVoxels>& Overlay = Voxels.editedBricks();
	if (Overlay.size() == 0)
	{
		return;
	}

	TSet<FVoxelChunkKey> DirtyChunks;
	DirtyChunks.Reserve(int32(Overlay.size()));
	for (const auto& Entry : Overlay)
	{
		const vxc::BrickKey& BKey = Entry.first;
		const int32 Bx = int32(FloorDiv(int64(BKey.x), int64(BricksPerChunk)));
		const int32 By = int32(FloorDiv(int64(BKey.y), int64(BricksPerChunk)));
		const int32 Bz = int32(FloorDiv(int64(BKey.z), int64(BricksPerChunk)));
		for (int32 Dz = -1; Dz <= 1; ++Dz)
		{
			for (int32 Dy = -1; Dy <= 1; ++Dy)
			{
				for (int32 Dx = -1; Dx <= 1; ++Dx)
				{
					DirtyChunks.Add(FVoxelChunkKey{Bx + Dx, By + Dy, Bz + Dz});
				}
			}
		}
	}

	// Deliberately NOT PropagateEditToMips: that call bumps EditEpoch,
	// invalidates SharedMipCache and logs one line per ancestor per level.
	// At load time nothing is resident, nothing is cached and no job is in
	// flight, so all of that is either a no-op or pure log spam -- on a large
	// save it would be tens of thousands of lines. Only the maps that outlive
	// the load are rebuilt here, by the same rules.
	int32 Level0Footprints = 0;
	for (const FVoxelChunkKey& Chunk : DirtyChunks)
	{
		int32& MaxZ0 = EditedFootprintMaxZ[0].FindOrAdd(FIntPoint(Chunk.X, Chunk.Y), MIN_int32);
		MaxZ0 = FMath::Max(MaxZ0, Chunk.Z);
		int32& MinZ0 = EditedFootprintMinZ[0].FindOrAdd(FIntPoint(Chunk.X, Chunk.Y), MAX_int32);
		MinZ0 = FMath::Min(MinZ0, Chunk.Z);
		++Level0Footprints;

		for (int32 Level = 1; Level < kNumLevels; ++Level)
		{
			const FVoxelChunkKey Ancestor = AncestorChunkKey(Chunk, Level);
			EditedAncestorChunks[Level].Add(Ancestor);
			int32& MaxZ = EditedFootprintMaxZ[Level].FindOrAdd(FIntPoint(Ancestor.X, Ancestor.Y), MIN_int32);
			MaxZ = FMath::Max(MaxZ, Ancestor.Z);
			int32& MinZ = EditedFootprintMinZ[Level].FindOrAdd(FIntPoint(Ancestor.X, Ancestor.Y), MAX_int32);
			MinZ = FMath::Min(MinZ, Ancestor.Z);
		}
	}

	UE_LOG(LogVoxelEdit, Log,
	       TEXT("LoadWorld: rebuilt edit-derived admission state from %llu overlay bricks -- %d level-0 chunks, ")
	       TEXT("%d level-0 footprints, %d level-1 edited ancestors"),
	       (unsigned long long)Overlay.size(), Level0Footprints, EditedFootprintMinZ[0].Num(),
	       EditedAncestorChunks[1].Num());
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

	// Sky-band trim escape hatch (see EditedFootprintMaxZ): record level 0's own
	// edited footprints here, since the ancestor walk below starts at level 1.
	//
	// A CHANGED value must also force the level to re-enumerate. Both hatches
	// are read in RecomputeDesiredSet's ENTRY scan, and that scan is gated on
	// the anchor having crossed a level-L chunk (bHasRecomputedLevel /
	// LastAnchorChunkPerLevel) -- so a player standing still and digging
	// straight down widens EditedFootprintMinZ and nothing ever looks at it.
	// Measured, not assumed: with the hatch in and this flag out, a 60 m shaft
	// dug from a stationary spawn still ended at the 41.6 m skirt floor with
	// the bottom 18 m untracked and unmeshed, byte-identical to having no
	// hatch at all.
	//
	// Only on a CHANGE, which for a sustained dig is at most once per 3.2 m of
	// new depth per footprint -- not once per edit. The flag costs exactly one
	// extra entry scan of that level on the next tick.
	for (const FVoxelChunkKey& Level0Chunk : DirtyLevel0Chunks)
	{
		int32& MaxZ = EditedFootprintMaxZ[0].FindOrAdd(FIntPoint(Level0Chunk.X, Level0Chunk.Y), MIN_int32);
		// ...and the mirror, for the all-solid admission skip's edit veto and
		// the downward escape hatch.
		int32& MinZ = EditedFootprintMinZ[0].FindOrAdd(FIntPoint(Level0Chunk.X, Level0Chunk.Y), MAX_int32);
		if (Level0Chunk.Z > MaxZ || Level0Chunk.Z < MinZ)
		{
			MaxZ = FMath::Max(MaxZ, Level0Chunk.Z);
			MinZ = FMath::Min(MinZ, Level0Chunk.Z);
			bHasRecomputedLevel[0] = false;
			++EditForcedRescansSinceLog;
		}
	}

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
			{
				// Same change-triggered rescan as level 0 above.
				int32& MaxZ = EditedFootprintMaxZ[Level].FindOrAdd(FIntPoint(Ancestor.X, Ancestor.Y), MIN_int32);
				int32& MinZ = EditedFootprintMinZ[Level].FindOrAdd(FIntPoint(Ancestor.X, Ancestor.Y), MAX_int32);
				if (Ancestor.Z > MaxZ || Ancestor.Z < MinZ)
				{
					MaxZ = FMath::Max(MaxZ, Ancestor.Z);
					MinZ = FMath::Min(MinZ, Ancestor.Z);
					bHasRecomputedLevel[Level] = false;
					++EditForcedRescansSinceLog;
				}
			}

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

	// Anchor position and speed, also always fresh rather than 1Hz-gated: these
	// are two doubles and a float, and a position row that lags a second behind
	// the world is worse than useless when you are trying to see WHERE the leg
	// is. 1 voxel = 10 UU = 0.1 m, so UU -> m is /100 and UU/s -> m/s likewise.
	LastPerfSnapshot.AnchorXMeters = LastAnchorLocation.X / 100.0;
	LastPerfSnapshot.AnchorYMeters = LastAnchorLocation.Y / 100.0;
	LastPerfSnapshot.AnchorZMeters = LastAnchorLocation.Z / 100.0;
	LastPerfSnapshot.AnchorSpeedMetersPerSec = float(SmoothedAnchorSpeedUUPerSec / 100.0);

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
	LastPerfSnapshot.PendingJobQueueDepth = PendingJobNum();
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
		if (Pair.Value.HoldsGeometry())
		{
			++ResidentComponents;
			++LevelLoaded[FMath::Clamp(Pair.Key.Level, 0, VoxelCoords::kNumLevels - 1)];
		}
	}
	for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
	{
		LevelPending[Level] += PendingJobKeysByLevel[Level].Num();
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

	// replay() only touches the overlay. Everything the ADMISSION path derives
	// from edits has to be rebuilt explicitly, or a reloaded world streams as
	// if it had never been dug.
	Impl.RebuildEditedFootprintsFromOverlay();
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
	// the world's tile source instead of the synthetic sampler.
	//
	// PRECEDENCE (2026-07-24: ini fallback added -- this deliberately REVERSES
	// the original Track B2 "command-line only, no ini fallback" call). The
	// reason: with no standing default, every launch that forgot the switch
	// -- double-clicking the .uproject, plain PIE, an IDE debug session --
	// silently booted a plausible-looking SYNTHETIC world, and terrain work
	// was then verified against terrain that wasn't the real terrain. That
	// exact failure mode is why VoxelEarthHUD carries a tile-source row; a
	// project default removes the trap instead of just reporting it.
	//   1. -VoxelTileDir=<path> on the command line -- WINS. Still the
	//      one-shot override for A/B runs, headless captures, and for
	//      deliberately testing the synthetic sampler (pass an empty value).
	//   2. DefaultTileDir under [/Script/VoxelEarth.VoxelWorldSubsystem] in
	//      Config/DefaultGame.ini -- the standing project default.
	//   3. Neither set -> empty -> synthetic sampler, exactly as before.
	// Neither source is trusted blindly: MakeTileSampler still rejects a bad
	// path / zero-loaded directory with a UE_LOG Error and falls back to the
	// synthetic sampler, and the HUD still reports which one is live.
	// Resolved once here, before Impl is constructed, as -VoxelSeed above is.
	FString TileDir;
	if (!FParse::Value(FCommandLine::Get(), TEXT("VoxelTileDir="), TileDir) && GConfig)
	{
		GConfig->GetString(TEXT("/Script/VoxelEarth.VoxelWorldSubsystem"),
		                   TEXT("DefaultTileDir"), TileDir, GGameIni);
	}
	if (!TileDir.IsEmpty() && FPaths::IsRelative(TileDir))
	{
		// Relative paths resolve against Content/ (unchanged rule). The ini
		// default is written relative so the checked-in config stays free of
		// one machine's absolute drive layout.
		TileDir = FPaths::Combine(FPaths::ProjectContentDir(), TileDir);
		FPaths::CollapseRelativeDirectories(TileDir);
	}

	// -VoxelTileScale=<int>: which tile scale to load (1 => 30m/px, the
	// default; 8 => 11.25m/px). Only meaningful alongside -VoxelTileDir --
	// MakeTileSampler validates it (vxc::tilePixelSizeMm returns 0 for
	// anything else) and falls back to the synthetic sampler with an Error
	// log if it's invalid, rather than silently misinterpreting tile pixels.
	int32 TileScale = 1;
	FParse::Value(FCommandLine::Get(), TEXT("VoxelTileScale="), TileScale);

	// Phase 2 fine-tier streaming (docs/terrain-amplification-plan.md;
	// VoxelFineTileStreamer.h). Command-line only, no ini fallback, unlike
	// -VoxelTileDir above: this is new, NOT verified against a running
	// engine (see the task report that introduced it), so it deliberately
	// stays opt-in-per-launch rather than becoming a standing project
	// default the way -VoxelTileDir did once it was trusted.
	//
	//   -VoxelFineTileDir=<path>   local directory mirroring terrain-service's
	//                              cache.py layout (<root>/<provider_id>/
	//                              <seed:016x>/s16/<x>_<y>.vxtl). Empty (the
	//                              default) => FineStreamer stays null and
	//                              every fine-tier gate is a no-op.
	//   -VoxelFineTileProviderId=<id>  the content-addressed provider_id this
	//                              run's fine tiles are stamped with; MUST
	//                              match the encoder's provider_id or every
	//                              tile fails FVoxelFineTileStreamer's
	//                              identity validation and is refused as a
	//                              mismatch (by design -- "validate, never
	//                              trust"). Defaults to empty, which will
	//                              correctly refuse every real tile -- pass
	//                              this whenever -VoxelFineTileDir is used.
	//   -VoxelFineTileCacheBudgetGB=<N>  LRU cache budget in GiB (plan
	//                              default 8-16); 0 or unset falls back to
	//                              FVoxelFineTileStreamer::kDefaultBudgetBytes.
	FString FineTileDir;
	FParse::Value(FCommandLine::Get(), TEXT("VoxelFineTileDir="), FineTileDir);
	FString FineProviderId;
	FParse::Value(FCommandLine::Get(), TEXT("VoxelFineTileProviderId="), FineProviderId);
	double FineBudgetGB = 0.0;
	FParse::Value(FCommandLine::Get(), TEXT("VoxelFineTileCacheBudgetGB="), FineBudgetGB);
	const uint64 FineBudgetBytes = FineBudgetGB > 0.0 ? uint64(FineBudgetGB * 1024.0 * 1024.0 * 1024.0) : 0;

	Impl = MakeUnique<FVoxelWorldImpl>(Seed, TileDir, TileScale, FineTileDir, FineProviderId, FineBudgetBytes);
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

	// -VoxelCavernShot[=<settleSeconds>] (see "Cavern vista capture" below).
	// Read from the command line, not a cvar, for the same reason
	// -VoxelNoUnderground is: it has to be known before the first tick.
	{
		float Settle = 0.f;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelCavernShot="), Settle) && Settle > 0.f)
		{
			CavernShotSettleSeconds = double(Settle);
			CavernShotElapsed = 0.0;
		}
		else if (FParse::Param(FCommandLine::Get(), TEXT("VoxelCavernShot")))
		{
			CavernShotElapsed = 0.0;
		}
		if (CavernShotElapsed >= 0.0)
		{
			UE_LOG(LogVoxelEarth, Log, TEXT("VoxelCavernShot: enabled, settle %.1fs."), CavernShotSettleSeconds);
		}
	}

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
	       (unsigned long long)Seed, GetLoadRadiusMeters(), GetUnloadRadiusMeters(), DigPlaceRangeMeters);
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

	TickCavernShot(DeltaTime);
}

// --- Cavern vista capture (-VoxelCavernShot) ---------------------------------
//
// WHY THIS EXISTS AT ALL. The underground sight sphere above is a claim about
// pixels ("you can see the far wall of a cavern"), and the only way to check a
// claim about pixels is to look. The two existing unattended capture switches
// cannot: -VoxelGICaveTest finds a SINKHOLE and points the camera UP the shaft
// (it is a GI test -- it wants the daylight shaft), and -VoxelUndergroundTest
// CARVES its own 1.3 m tunnel with the edit log, which is a corridor shot and
// also writes to the edit overlay. Neither photographs natural cavern geometry.
//
// WHY THE FRAMING IS COMPUTED AND NOT HARDCODED. Framing, not streaming, is
// what defeated the two previous attempts at this image: one pose sat on a
// sunlit mountainside, the other 0.9-3.8 m from a wall (both were 120 UU up
// the axis of a 130 UU-radius tunnel, i.e. jammed into the roof). Fixed poses
// are fragile because the geometry they point at is generated. So this
// MEASURES the room -- floor, ceiling, and both walls along the view axis --
// picks the camera from those measurements, and logs every number it used. If
// the shot is bad, the log says so before anyone opens the PNG.
//
// EVERY PROBE USES IsSolidAtVoxel, NEVER RaycastVoxelWorld, for the search.
// RaycastVoxelWorld tests the RESIDENT chunk set, and at search time the
// cavern is hundreds of metres away with nothing streamed, so it reports
// "miss" straight through solid rock -- the documented way -VoxelGICaveTest
// once parked its camera inside a wall. IsSolidAtVoxel goes to the amplifier
// and is correct regardless of residency. Raycasts appear only in the
// capture-time enclosure probe, where they are wanted precisely BECAUSE they
// see the resident set: that is the measurement of whether the streamer
// actually delivered the rock this change is about.
//
// The pose is applied by teleporting the PAWN, not by moving a camera: the
// streaming anchor is the pawn's actor location (see Tick above), so a camera-
// only move would leave the desired set behind and stream nothing underground.
//
// Switches:
//   -VoxelCavernShot[=<settleSeconds>]  enable; default 45 s of settle
//   -VoxelCavernAt=<X>,<Y>              search origin in UU (default 0,0)
//   -VoxelCavernFlooded                 accept flooded caverns (default: dry
//                                       only, so the shot is not underwater)
bool UVoxelWorldSubsystem::FindCavernPose(FVector& OutCameraUU, FRotator& OutLookRot, FString& OutReport) const
{
	if (!Impl)
	{
		return false;
	}
	const vxc::Amplifier& Amp = Impl->Voxels.amplifier();

	double OriginX = 0.0, OriginY = 0.0;
	{
		FString AtValue;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelCavernAt="), AtValue))
		{
			FString Xs, Ys;
			if (AtValue.Split(TEXT(","), &Xs, &Ys))
			{
				OriginX = FCString::Atod(*Xs);
				OriginY = FCString::Atod(*Ys);
			}
		}
	}
	const bool bAllowFlooded = FParse::Param(FCommandLine::Get(), TEXT("VoxelCavernFlooded"));

	// Score a column by the vertical half-extent of the best cavern room
	// reaching it. caverns.h defines CavernSeg::marginSq as
	// rz^2 * (rxy^2 - d^2) / rxy^2, so sqrt(marginSq) IS that half-extent in
	// mm and is maximised exactly on the room's vertical axis -- which makes
	// "maximise marginSq" the same thing as "walk to the middle of the room",
	// with no probing at all. Returns 0 for a column no room reaches.
	const auto ScoreColumn = [&Amp, bAllowFlooded](int64 Vx, int64 Vy, int32& OutZCenterMm, int32& OutZFloorMm) -> double
	{
		// By VALUE: Amplifier::columnCached hands back a reference into a
		// per-thread memo that the next call overwrites (the same trap
		// VoxelWaterSubsystem::FindFloodedCavernNear documents). column() is
		// the by-value form, but the copy here is deliberate regardless.
		const vxc::ColumnSample Col = Amp.column(Vx, Vy);
		if (Col.cavern.count == 0)
		{
			return 0.0;
		}
		if (!bAllowFlooded && Col.cavern.floodZMm != INT32_MIN)
		{
			return 0.0; // a flooded room photographs as water, not as a cavern vista
		}
		double Best = 0.0;
		for (int32 S = 0; S < Col.cavern.count; ++S)
		{
			const double Half = FMath::Sqrt(double(FMath::Max(0, Col.cavern.segs[S].marginSq)));
			if (Half > Best)
			{
				Best = Half;
				OutZCenterMm = Col.cavern.segs[S].zCenterMm;
				OutZFloorMm = Col.cavern.segs[S].zFloorMm;
			}
		}
		return Best;
	};

	// 1. COARSE SEARCH, expanding square rings so the first acceptable hit is
	// also the nearest. The step is 30 m, deliberately under caverns.h's
	// kCavernMaxReachMm (~36.4 m): a coarser grid could step clean over a
	// whole site. Perimeter-only per ring, so this is O(radius) rings not
	// O(radius^2) rescans.
	constexpr double StepUU = 3000.0;    // 30 m
	constexpr double MaxRadiusUU = 60000.0; // 600 m
	const int32 MaxRing = int32(MaxRadiusUU / StepUU);
	double BestScore = 0.0;
	double BestXUU = 0.0, BestYUU = 0.0;
	int32 BestZCenterMm = 0, BestZFloorMm = 0;
	for (int32 Ring = 0; Ring <= MaxRing && BestScore <= 0.0; ++Ring)
	{
		for (int32 Dy = -Ring; Dy <= Ring; ++Dy)
		{
			for (int32 Dx = -Ring; Dx <= Ring; ++Dx)
			{
				if (Ring > 0 && FMath::Abs(Dx) != Ring && FMath::Abs(Dy) != Ring)
				{
					continue; // interior of this ring was covered by an earlier one
				}
				const double Wx = OriginX + double(Dx) * StepUU;
				const double Wy = OriginY + double(Dy) * StepUU;
				int32 ZCenterMm = 0, ZFloorMm = 0;
				const double Score = ScoreColumn(int64(FMath::FloorToDouble(Wx / VoxelCoords::VoxelSizeUU)),
				                                 int64(FMath::FloorToDouble(Wy / VoxelCoords::VoxelSizeUU)), ZCenterMm,
				                                 ZFloorMm);
				if (Score > BestScore)
				{
					BestScore = Score;
					BestXUU = Wx;
					BestYUU = Wy;
					BestZCenterMm = ZCenterMm;
					BestZFloorMm = ZFloorMm;
				}
			}
		}
	}
	if (BestScore <= 0.0)
	{
		OutReport = FString::Printf(TEXT("no dry cavern found within %.0f m of (%.0f,%.0f)"), MaxRadiusUU / 100.0,
		                            OriginX, OriginY);
		return false;
	}

	// 2. REFINE onto the room axis. The coarse grid lands anywhere inside the
	// room; a 2 m grid over +-40 m (a whole room diameter, kCavernRxyMaxMm =
	// 28 m) around it maximises the same score, i.e. walks to the axis. This
	// matters for the shot: framed from the tapering EDGE of the room the far
	// wall is close and the ceiling is low, which is the "photographed a
	// crack" failure mode.
	{
		constexpr double RefineHalfUU = 4000.0;
		constexpr double RefineStepUU = 200.0;
		const double CenterX = BestXUU, CenterY = BestYUU;
		for (double Wy = CenterY - RefineHalfUU; Wy <= CenterY + RefineHalfUU; Wy += RefineStepUU)
		{
			for (double Wx = CenterX - RefineHalfUU; Wx <= CenterX + RefineHalfUU; Wx += RefineStepUU)
			{
				int32 ZCenterMm = 0, ZFloorMm = 0;
				const double Score = ScoreColumn(int64(FMath::FloorToDouble(Wx / VoxelCoords::VoxelSizeUU)),
				                                 int64(FMath::FloorToDouble(Wy / VoxelCoords::VoxelSizeUU)), ZCenterMm,
				                                 ZFloorMm);
				if (Score > BestScore)
				{
					BestScore = Score;
					BestXUU = Wx;
					BestYUU = Wy;
					BestZCenterMm = ZCenterMm;
					BestZFloorMm = ZFloorMm;
				}
			}
		}
	}

	// 3. MEASURE the room with solidity probes. Everything from here is the
	// real carved world (caves, bedrock clamps and the roof guard all apply),
	// not the analytic ellipsoid, so the numbers below are what the camera
	// will actually see.
	const auto IsAirAtUU = [this](double Wx, double Wy, double Wz)
	{
		return !IsSolidAtVoxel(int64(FMath::FloorToDouble(Wx / VoxelCoords::VoxelSizeUU)),
		                       int64(FMath::FloorToDouble(Wy / VoxelCoords::VoxelSizeUU)),
		                       int64(FMath::FloorToDouble(Wz / VoxelCoords::VoxelSizeUU)));
	};
	const double AxisZUU = double(BestZCenterMm) / 10.0;
	if (!IsAirAtUU(BestXUU, BestYUU, AxisZUU))
	{
		OutReport = FString::Printf(TEXT("room axis at (%.0f,%.0f,%.0f) is solid -- carve guards rejected it"), BestXUU,
		                            BestYUU, AxisZUU);
		return false;
	}
	constexpr double ProbeStepUU = 20.0;   // 20 cm, two voxels
	constexpr double ProbeLimitUU = 12000.0; // 120 m, well past kCavernRzDeepMaxMm*2
	// Clamped for the same reason as the ceiling walk below: a chain descends
	// through its own floor, so an unbounded downward walk lands in the room
	// beneath this one.
	const double FloorLimitUU = AxisZUU - BestScore / 10.0;
	double FloorZUU = AxisZUU;
	while (FloorZUU - ProbeStepUU >= FloorLimitUU && IsAirAtUU(BestXUU, BestYUU, FloorZUU - ProbeStepUU))
	{
		FloorZUU -= ProbeStepUU;
	}
	// The ceiling probe is CLAMPED to the analytic half-extent, and that clamp
	// is not cosmetic. Rooms in a cavern chain overlap vertically, so a naive
	// upward walk from the axis escapes through the join into the room above
	// and keeps going: the first run of this harness measured a "130 m tall"
	// room, aimed two thirds of the way up that, and produced a 62-degree
	// pitch -- a photograph of the ceiling. BestScore is sqrt(marginSq), the
	// half-height of THIS room at THIS column, so it is exactly the bound the
	// walk should not cross.
	const double CeilLimitUU = AxisZUU + BestScore / 10.0;
	double CeilZUU = AxisZUU;
	while (CeilZUU + ProbeStepUU <= CeilLimitUU && IsAirAtUU(BestXUU, BestYUU, CeilZUU + ProbeStepUU))
	{
		CeilZUU += ProbeStepUU;
	}
	// Eye height: 1.6 m off the floor. The -VoxelUndergroundTest lesson is
	// that the camera's height offset has to be well INSIDE the local void,
	// and that framing should come from pitch rather than from height -- its
	// tunnel shot put the lens 10 UU under the roof of a 130 UU tunnel and
	// photographed rock. A cavern room is metres tall, so 1.6 m is safe, but
	// clamp anyway for the case where the axis probe found a low room.
	const double EyeZUU = FMath::Min(FloorZUU + 160.0, (FloorZUU + CeilZUU) * 0.5);

	// Walls along +X / -X at eye height. Stand near the -X wall and look
	// across, so the whole room width becomes the sightline.
	double WallNegXUU = BestXUU;
	while (BestXUU - WallNegXUU < ProbeLimitUU && IsAirAtUU(WallNegXUU - ProbeStepUU, BestYUU, EyeZUU))
	{
		WallNegXUU -= ProbeStepUU;
	}
	double WallPosXUU = BestXUU;
	while (WallPosXUU - BestXUU < ProbeLimitUU && IsAirAtUU(WallPosXUU + ProbeStepUU, BestYUU, EyeZUU))
	{
		WallPosXUU += ProbeStepUU;
	}
	// Stand 2 m off the near wall (not against it: the near-plane would clip
	// into rock and the first metres of the vista would be wall).
	const double StandXUU = FMath::Min(WallNegXUU + 200.0, BestXUU);
	const double SightUU = WallPosXUU - StandXUU;

	OutCameraUU = FVector(StandXUU, BestYUU, EyeZUU);
	const_cast<UVoxelWorldSubsystem*>(this)->CavernShotSightlineUU = SightUU;
	// Aim LOW on the far wall -- a third of its height -- and then clamp the
	// pitch. Both halves are corrections from real runs. Aiming two thirds up
	// gave 34.8 degrees in a room that is 49 m tall and 47 m wide, and the
	// resulting frame was almost entirely ceiling: in a room whose height is
	// comparable to its width, "look at the far wall" and "look up" are nearly
	// the same instruction. Aiming low keeps the floor sweeping away to the
	// far wall in frame, which is what makes the distance legible -- an
	// unbroken floor running to a wall 45 m away IS the picture of the sight
	// sphere working. The clamp is the backstop for a room shape the ratio
	// does not anticipate.
	const double TargetZUU = FloorZUU + (CeilZUU - FloorZUU) * 0.33;
	const double RawPitch = FMath::RadiansToDegrees(FMath::Atan2(TargetZUU - EyeZUU, FMath::Max(SightUU, 1.0)));
	OutLookRot = FRotator(FMath::Clamp(RawPitch, -15.0, 18.0), 0.0, 0.0);

	const double SurfaceUU = GetSurfaceHeightUU(BestXUU, BestYUU);
	OutReport = FString::Printf(
		TEXT("room axis=(%.0f,%.0f) analytic halfHeight=%.1fm | floor=%.0f ceil=%.0f (height %.1fm) | ")
		TEXT("walls x=[%.0f,%.0f] (width %.1fm) | camera=(%.0f,%.0f,%.0f) pitch=%.1f sightline=%.1fm | ")
		TEXT("surface=%.0f depth=%.1fm"),
		BestXUU, BestYUU, BestScore / 1000.0, FloorZUU, CeilZUU, (CeilZUU - FloorZUU) / 100.0, WallNegXUU, WallPosXUU,
		(WallPosXUU - WallNegXUU) / 100.0, OutCameraUU.X, OutCameraUU.Y, OutCameraUU.Z, OutLookRot.Pitch,
		SightUU / 100.0, SurfaceUU, (SurfaceUU - EyeZUU) / 100.0);
	return true;
}

void UVoxelWorldSubsystem::PoseCavernCamera() const
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return;
	}
	// TeleportPhysics, and re-applied on a schedule below: a fly pawn with any
	// residual velocity will drift out of a pose applied once, and the drift
	// is exactly what turns a measured 20 m sightline into a shot of a wall.
	Pawn->SetActorLocation(CavernShotCameraUU, false, nullptr, ETeleportType::TeleportPhysics);
	Pawn->SetActorRotation(CavernShotLookRot);
	PC->SetControlRotation(CavernShotLookRot);
	PC->SetViewTarget(Pawn);
}

void UVoxelWorldSubsystem::TickCavernShot(float DeltaSeconds)
{
	if (CavernShotElapsed < 0.0 || bCavernShotFailed)
	{
		return;
	}
	const double Prev = CavernShotElapsed;
	CavernShotElapsed += double(DeltaSeconds);

	// Pose as early as the pawn exists. The teleport is what starts the
	// underground desired set building, so every second before it is settle
	// time wasted.
	if (!bCavernShotPosed)
	{
		UWorld* World = GetWorld();
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		if (!PC || !PC->GetPawn())
		{
			return; // no pawn yet; do not consume settle time
		}
		FString Report;
		if (!FindCavernPose(CavernShotCameraUU, CavernShotLookRot, Report))
		{
			UE_LOG(LogVoxelEarth, Error, TEXT("VoxelCavernShot: %s"), *Report);
			bCavernShotFailed = true;
			return;
		}
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelCavernShot: %s"), *Report);
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelCavernShot: settling %.1fs (voxel.Stream.UndergroundSightM=%.0f)"),
		       CavernShotSettleSeconds, double(VoxelUnderground::GSightRadiusM));
		PoseCavernCamera();
		bCavernShotPosed = true;
		CavernShotElapsed = 0.0;
		return;
	}

	// Re-pose twice more before the shot, for the drift reason above.
	const double RepinAt[2] = {CavernShotSettleSeconds - 4.0, CavernShotSettleSeconds - 1.0};
	for (const double T : RepinAt)
	{
		if (Prev < T && CavernShotElapsed >= T)
		{
			PoseCavernCamera();
		}
	}

	if (!bCavernShotCaptured && CavernShotElapsed >= CavernShotSettleSeconds)
	{
		bCavernShotCaptured = true;
		PoseCavernCamera();

		// Where the camera ACTUALLY ended up. "The pose did not stick" is the
		// other half of how the previous two attempts failed, and it is
		// invisible in the PNG.
		UWorld* World = GetWorld();
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		if (PC && PC->PlayerCameraManager)
		{
			const FVector Actual = PC->PlayerCameraManager->GetCameraLocation();
			const FRotator ActualRot = PC->PlayerCameraManager->GetCameraRotation();
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("VoxelCavernShot: ACTUAL cam=(%.0f,%.0f,%.0f) rot=(pitch %.1f yaw %.1f) | intended=(%.0f,%.0f,%.0f) pitch %.1f"),
			       Actual.X, Actual.Y, Actual.Z, ActualRot.Pitch, ActualRot.Yaw, CavernShotCameraUU.X,
			       CavernShotCameraUU.Y, CavernShotCameraUU.Z, CavernShotLookRot.Pitch);
		}

		// GEOMETRY probe: where the rock IS, per the voxel world. Note this is
		// NOT a statement about what is on screen -- RaycastVoxelWorld runs a
		// DDA against the generated/overlaid voxel world, so it reports the
		// wall whether or not a single chunk of it has been meshed. The first
		// run of this harness reported "+X=44.9m" here and photographed open
		// sky, which is exactly that distinction.
		const FVector Dirs[6] = {FVector(1, 0, 0), FVector(-1, 0, 0), FVector(0, 1, 0),
		                         FVector(0, -1, 0), FVector(0, 0, 1), FVector(0, 0, -1)};
		const TCHAR* Names[6] = {TEXT("+X"), TEXT("-X"), TEXT("+Y"), TEXT("-Y"), TEXT("+Z"), TEXT("-Z")};
		FString Probe;
		for (int32 I = 0; I < 6; ++I)
		{
			FVector Hit, PrevVoxel;
			if (RaycastVoxelWorld(CavernShotCameraUU, Dirs[I], 6000.0, Hit, PrevVoxel))
			{
				Probe += FString::Printf(TEXT("%s=%.1fm "), Names[I], (Hit - CavernShotCameraUU).Size() / 100.0);
			}
			else
			{
				Probe += FString::Printf(TEXT("%s=MISS "), Names[I]);
			}
		}
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelCavernShot: voxel-world geometry probe (60m rays): %s"), *Probe);

		// RESIDENCY probe: whether that rock is actually MESHED, which is the
		// claim the sight sphere makes and the only one the screenshot can
		// confirm. Asks DebugChunkStatusAt whether the chunk containing each
		// sample is tracked and owns a component.
		//
		// A FAN, along the actual view direction and above it -- not a single
		// horizontal ray. The horizontal-only version of this probe reported
		// "23/23 meshed" for a frame whose entire middle was open sky, and the
		// reason is the exact shape of what the sphere does: at 44.6 m lateral
		// an R = 40 m sphere has ZERO vertical radius, so the resident set
		// there is one 3.2 m band at the anchor's own Z. A ray at eye height
		// runs straight down the middle of that band and sees rock the whole
		// way, while everything the camera is actually looking at -- pitched
		// 18 degrees up -- is above it and unmeshed. Sampling the view cone is
		// what makes the probe answer the question the screenshot asks.
		{
			// For each elevation: raycast the VOXEL world to find where the rock
			// the player is looking at actually is, then ask whether the chunk
			// holding that rock is tracked and meshed. Both halves are needed
			// and neither alone is honest.
			//
			// Walking a ray and counting "meshed" samples does NOT work, and
			// the failure is instructive: a chunk of open cavern air is all-air,
			// meshes to zero quads and correctly owns NO component, so a sample
			// inside the room reads as "not meshed" while being exactly right.
			// A first attempt at this probe scored +20deg as 0/23 for that
			// reason alone. What matters is only the chunk the sightline
			// TERMINATES in -- that is the surface that either draws or is a
			// hole showing sky.
			const double Elevations[4] = {0.0, 10.0, 20.0, 30.0}; // degrees above horizontal
			FString Res;
			for (const double ElevDeg : Elevations)
			{
				const double Rad = FMath::DegreesToRadians(ElevDeg);
				const FVector Dir(FMath::Cos(Rad), 0.0, FMath::Sin(Rad));
				FVector Hit, PrevVoxel;
				if (!RaycastVoxelWorld(CavernShotCameraUU, Dir, 8000.0, Hit, PrevVoxel))
				{
					Res += FString::Printf(TEXT("[+%.0fdeg NO ROCK within 80m] "), ElevDeg);
					continue;
				}
				const double DistM = (Hit - CavernShotCameraUU).Size() / 100.0;
				// One voxel PAST the surface, so the sample lands in the solid
				// chunk that owns the visible face rather than in the air chunk
				// in front of it.
				bool bTracked = false, bHasComponent = false, bSettledUnused = false;
				int32 Quads = 0;
				const bool bKnown =
					DebugChunkStatusAt(Hit + Dir * VoxelCoords::VoxelSizeUU, bTracked, bHasComponent, Quads, bSettledUnused);
				Res += FString::Printf(TEXT("[+%.0fdeg wall at %.1fm: %s] "), ElevDeg, DistM,
				                       !bKnown          ? TEXT("NOT TRACKED -> renders as a hole")
				                       : !bHasComponent ? TEXT("tracked but NOT MESHED -> renders as a hole")
				                                        : TEXT("MESHED -> draws"));
			}
			UE_LOG(LogVoxelEarth, Log, TEXT("VoxelCavernShot: RESIDENCY fan (sightline %.1fm, sight sphere R=%.0fm): %s"),
			       CavernShotSightlineUU / 100.0, double(VoxelUnderground::GSightRadiusM), *Res);
		}

		const FVoxelPerfSnapshot Snap = GetPerfSnapshot();
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelCavernShot: residentComponents=%d residentQuads=%lld chunksLoaded=%lld"),
		       Snap.ResidentComponents, (long long)Snap.ResidentQuads, (long long)Snap.TotalChunksLoaded);

		FScreenshotRequest::RequestScreenshot(TEXT("VoxelCavern"), /*bShowUI*/ false, /*bAddFilenameSuffix*/ true);
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelCavernShot: screenshot requested."));
	}

	if (bCavernShotCaptured && CavernShotElapsed >= CavernShotSettleSeconds + 4.0)
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelCavernShot: done, exiting."));
		FPlatformMisc::RequestExit(false);
		bCavernShotFailed = true; // stop ticking this
	}
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
                                              int32& OutQuads, bool& bOutSettled) const
{
	if (!Impl)
	{
		return false;
	}
	const VoxelCoords::FVoxelLevelChunkKey Key{0, VoxelCoords::ChunkKeyForVoxel(VoxelCoords::WorldToVoxel(WorldPos))};
	const VoxelStreaming::FChunkRecord* Rec = Impl->ChunkRecords.Find(Key);
	bOutTracked = (Rec != nullptr);
	bOutHasComponent = Rec && Rec->HoldsGeometry();
	OutQuads = Rec ? Rec->LastQuadCount : 0;
	bOutSettled = Rec && Rec->bMeshSettled;
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
