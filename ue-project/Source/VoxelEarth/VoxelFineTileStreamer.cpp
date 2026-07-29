#include "VoxelFineTileStreamer.h"

#include "VoxelEarth.h"      // LogVoxelEarth
#include "VoxelTileCodec.h"  // VoxelEarth::GetFineTileDecompressor -- CODEC_ZSTD host boundary
#include "Misc/Paths.h"      // FPaths::Combine

#include "voxelcore/caverns.h" // kCavernMaxReachMm -- see kFineReadMarginMm below
#include "voxelcore/core.h"    // kVoxelSizeMm

#include <filesystem>
#include <optional>
#include <utility>

// See VoxelFineTileStreamer.h's class-level comment for the full contract.
// Short version: this file is a synchronous, local-disk-only implementation of
// the residency/prefetch/eviction POLICY (voxelcore/tilestreaming.h owns the
// actual maths -- ring geometry, dilation, read-margin, LRU ordering,
// cache-key formatting, validation) plus the RWLock that makes one
// vxc::FineTileSampler safe to hand to the meshing worker pool. Everything
// else is glue: turning UE world-mm coordinates into calls against that pure
// library, and turning its answers into vxc::FineTileSampler loads/unloads.
//
// A future async/network loader would replace EnsureTileResident_Locked's body
// (currently vxc::readFileBytes + vxc::validateAndParseFineTile + a full
// prewarm, all synchronous) with a background fetch+decode that completes on
// some later tick and is published under the exclusive lock; the public API
// (IsFootprintResident / RequestFootprint / TickResidencyAndEviction) would
// not need to change shape, because RequestFootprint already returns false
// rather than blocking forever when something isn't ready -- "not yet
// resident" and "will never be resident" already look identical to a caller,
// which is exactly the "block until ready, never fall back" contract the plan
// requires.

namespace
{
	// MIRROR of ue-project/Source/VoxelEarth/VoxelGpuRegionBuild.h's
	// kRasterCavernMarginMm, and it must stay one. That constant sizes the
	// raster window the GPU worldgen path uploads; this one sizes the residency
	// the CPU path demands before it will voxelize. They describe the SAME
	// fact -- how far outside its own footprint a chunk's generation reads the
	// tile raster -- and if this one is smaller, the gate admits chunks whose
	// cavern reads land in a non-resident tile and come back as sea level.
	// Derived from the voxel-core constants rather than copied as a literal so
	// a cavern-size change moves both.
	constexpr int64 kFineReadMarginMm = vxc::kCavernMaxReachMm + vxc::kVoxelSizeMm;

	// The fixed 15.36 km footprint every tier shares (vxtl-v2-format.md §1):
	// 8192 fine px * 1875 mm/px == 512 s1 px * 30000 mm/px. Used for tile
	// SELECTION, which must work before anything is resident, so it comes from
	// the compile-time constants and never from a loaded tile's `size` field.
	constexpr int64 kTileFootprintMm = int64(vxc::kFineTileSize) * vxc::tilePixelSizeMm(vxc::kFineTileScale);

	constexpr uint64 TileHash(vxc::TileCoord T)
	{
		return (uint64(uint32(T.x)) << 32) | uint64(uint32(T.y));
	}
}

// ---------------------------------------------------------------------------
// FVoxelFineTileSamplerProxy -- the worker-facing query path.
//
// SHARED lock, and it is only sound because EnsureTileResident_Locked decodes
// every block of a tile before publishing it: vxc::FineTileSampler::blockFor
// then takes its found-block path and mutates nothing, so N workers reading
// concurrently is N pure reads. If whole-tile decode were ever relaxed back to
// lazy, this lock would have to become exclusive (see the header).
// ---------------------------------------------------------------------------

int32_t FVoxelFineTileSamplerProxy::elevationMm(int64_t px, int64_t py)
{
	FRWScopeLock Lock(Owner.Lock_, SLT_ReadOnly);
	return Owner.Sampler_.elevationMm(px, py);
}

vxc::ClimateSample FVoxelFineTileSamplerProxy::climate(int64_t px, int64_t py)
{
	// The delegate (the coarse vxc::TileGridSampler) is itself read-only after
	// init, but it is reached THROUGH Sampler_, so it takes the same lock.
	FRWScopeLock Lock(Owner.Lock_, SLT_ReadOnly);
	return Owner.Sampler_.climate(px, py);
}

// ---------------------------------------------------------------------------

FVoxelFineTileStreamer::FVoxelFineTileStreamer(FString RootDir, FString ProviderId, uint64 Seed,
                                               uint64 BudgetBytes, vxc::ITileSampler* ClimateSource)
	: RootDir_(MoveTemp(RootDir))
	, ProviderId_(TCHAR_TO_UTF8(*ProviderId))
	, Seed_(Seed)
	, Sampler_(Seed, ClimateSource)
	, Budget_(BudgetBytes)
	, Proxy_(*this)
{
	// CODEC_ZSTD tiles need the host's decompressor; CODEC_RAW ones (what
	// bake_real_tile.py writes today) never touch it. Registering it here, at
	// construction, rather than per load, because FineTileSampler documents the
	// registration as NOT retroactive -- a tile keeps whatever it was parsed
	// with, for its whole lazily-decoded life. With no zstd in this build this
	// is an empty decompressor and a CODEC_ZSTD tile is refused whole at load
	// (FineError::kNoDecompressor), which is the intended loud failure.
	Sampler_.setDecompressor(VoxelEarth::GetFineTileDecompressor());
}

FString FVoxelFineTileStreamer::LocalPathFor(vxc::TileCoord Tile) const
{
	// Mirrors terrain-service/terrain_service/cache.py's TileCache fine path
	// layout exactly (RootDir_ stands in for cache.py's `root`):
	// <root>/<provider_id>/<seed:016x>/s16/<x>_<y>.vxtl. Built from the SAME
	// vxc::formatFineTileCacheKey the LRU bookkeeping uses, so the on-disk
	// path and the cache key can never drift apart.
	const std::string Key = vxc::formatFineTileCacheKey(ProviderId_, Seed_, Tile.x, Tile.y);
	return FPaths::Combine(RootDir_, FString(Key.c_str()) + TEXT(".vxtl"));
}

bool FVoxelFineTileStreamer::EnsureTileResident_Locked(vxc::TileCoord Tile)
{
	if (Sampler_.findTile(Tile.x, Tile.y) != nullptr)
	{
		return true; // already resident
	}

	const FString Path = LocalPathFor(Tile);
	const std::filesystem::path StdPath(*Path);
	std::optional<std::vector<uint8_t>> Bytes = vxc::readFileBytes(StdPath);
	if (!Bytes)
	{
		// Not on disk (yet, or ever) -- caller keeps deferring, never a
		// fallback. Remembered so the per-recompute prefetch loop does not
		// re-stat the same absent frontier tiles on every anchor crossing;
		// forgotten whenever the ring centre moves (see TickResidencyAndEviction),
		// so a tile the bake produces mid-session is still picked up.
		++MissingFileLoads_;
		KnownMissing_.insert(TileHash(Tile));
		return false;
	}
	const uint64 ByteCount = Bytes->size();

	// "Validate, never trust": FineTile::parse's own all-or-nothing structural
	// check, PLUS the identity check that follows EditLog::checkProvider()'s
	// precedent (compare a stamped identity, refuse on mismatch) rather than
	// inventing a second mechanism. The decompressor is the sampler's, so a
	// CODEC_ZSTD tile is decodable here exactly when this build has zstd.
	vxc::FineTileValidationResult Validated = vxc::validateAndParseFineTile(
		std::move(*Bytes), Seed_, Tile.x, Tile.y, Sampler_.decompressor());
	if (Validated.verdict == vxc::FineTileVerdict::kCorrupt)
	{
		++CorruptLoads_;
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("Fine tile (%d,%d) at %s failed structural validation (%hs) -- discarding, NOT using it. ")
		       TEXT("Re-fetch required."),
		       Tile.x, Tile.y, *Path, vxc::fineErrorName(Validated.error));
		return false;
	}
	if (Validated.verdict == vxc::FineTileVerdict::kIdentityMismatch)
	{
		++IdentityMismatches_;
		UE_LOG(LogVoxelEarth, Warning, TEXT("Fine tile at %s does not match the requested identity (seed=%llu, ")
		       TEXT("x=%d, y=%d) -- discarding, NOT using it. This is a cache-key/provider mismatch, the same class ")
		       TEXT("of refusal EditLog::checkProvider() already models for edit-log replay."),
		       *Path, (unsigned long long)Seed_, Tile.x, Tile.y);
		return false;
	}

	// Capture the geometry before the move below hands ownership to the sampler.
	const int64 TileSizePx = int64(Validated.tile->size());
	const uint64 TileDecodedBytes =
		uint64(Validated.tile->blockCount()) * uint64(Validated.tile->blockPixelCount()) * sizeof(int16_t);

	if (!Sampler_.loadTile(std::move(*Validated.tile)))
	{
		// Should be unreachable: the identity check above already confirmed
		// seed matches, and loadTile only additionally rejects on a `size`
		// stride mismatch against tiles already loaded. Never trust a double
		// negative silently -- if this ever fires it means two tiles in this
		// run disagree on `size`, which the format allows per-tile but this
		// sampler (like the wire spec's own single-grid-stride assumption)
		// does not.
		++IdentityMismatches_;
		UE_LOG(LogVoxelEarth, Error, TEXT("Fine tile (%d,%d) at %s passed validation but FineTileSampler::loadTile ")
		       TEXT("rejected it (likely a `size` stride mismatch against an already-loaded tile) -- discarding."),
		       Tile.x, Tile.y, *Path);
		return false;
	}

	// RULE 1 (see the header's threading note): decode the WHOLE tile now, on
	// this thread, under the exclusive lock. After this the tile's block map is
	// complete and every later query -- from any number of worker threads -- is
	// a pure read. A tile that cannot be fully decoded is not a tile we can
	// serve safely, so it is unloaded again rather than left half-warm: a
	// partially decoded tile would let a worker query trigger a decode, which
	// is the one thing the shared read lock cannot survive.
	const double DecodeT0 = FPlatformTime::Seconds();
	const bool bDecoded = Sampler_.prewarm(int64(Tile.x) * TileSizePx, int64(Tile.y) * TileSizePx,
	                                       (int64(Tile.x) + 1) * TileSizePx - 1,
	                                       (int64(Tile.y) + 1) * TileSizePx - 1);
	if (!bDecoded)
	{
		++CorruptLoads_;
		Sampler_.unloadTile(Tile.x, Tile.y);
		UE_LOG(LogVoxelEarth, Error, TEXT("Fine tile (%d,%d) at %s parsed but at least one block failed to decode ")
		       TEXT("(blockDecodeFailures=%llu) -- unloaded. A half-decoded tile is not servable: a worker query ")
		       TEXT("into a missing block would decode on the read path, which the shared read lock cannot survive."),
		       Tile.x, Tile.y, *Path,
		       (unsigned long long)Sampler_.blockDecodeFailures.load(std::memory_order_relaxed));
		return false;
	}
	const double DecodeMs = (FPlatformTime::Seconds() - DecodeT0) * 1000.0;

	DecodedBytes_ += TileDecodedBytes;
	++TilesLoaded_;
	KnownMissing_.erase(TileHash(Tile));

	// Charge the LRU BOTH the file bytes and the decoded lattice. The decoded
	// half is ~2/3 of the total for a production tile (134 MB of int16 lattice
	// against a 201 MB CODEC_RAW file) and is the part that actually has to fit
	// in RAM, so a budget that counted only the file would be off by a factor
	// nobody could reason about.
	const std::string Key = vxc::formatFineTileCacheKey(ProviderId_, Seed_, Tile.x, Tile.y);
	Budget_.touch(Key, ByteCount + TileDecodedBytes);
	KeyToTile_.emplace(Key, Tile);

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("Fine tile (%d,%d) resident: %llu px edge, %.1f MB file + %.1f MB decoded lattice, ")
	       TEXT("full decode %.0f ms. Resident tiles now %llu, %.2f GiB of %.2f GiB budget."),
	       Tile.x, Tile.y, (unsigned long long)TileSizePx, double(ByteCount) / 1e6,
	       double(TileDecodedBytes) / 1e6, DecodeMs, (unsigned long long)Sampler_.tileCount(),
	       double(Budget_.residentBytes()) / double(1ull << 30), double(Budget_.budgetBytes()) / double(1ull << 30));
	return true;
}

vxc::TileCoord FVoxelFineTileStreamer::CoarseTileForWorldMm(int64 WorldMmX, int64 WorldMmY)
{
	return vxc::tileCoordForWorldMm(WorldMmX, WorldMmY, kTileFootprintMm);
}

void FVoxelFineTileStreamer::CoveredTileRange(int64 WorldMmX0, int64 WorldMmY0, int64 WorldMmX1, int64 WorldMmY1,
                                              int32_t& OutTx0, int32_t& OutTy0, int32_t& OutTx1, int32_t& OutTy1)
{
	// One conversion, in voxel-core, applying BOTH the cavern read margin and
	// the carrier stencil -- see vxc::fineReadPixelRect's comment for why
	// leaving either out is a silent desync rather than a fault. The pixel rect
	// is then divided by the FIXED production stride: tile selection has to
	// work before anything is resident, and every production tile is
	// kFineTileSize (the per-tile `size` field is a test-fixture convenience,
	// see tilestore.h). A loaded tile whose size disagreed would have been
	// refused by FineTileSampler::loadTile anyway.
	const vxc::PixelRect Rect =
		vxc::fineReadPixelRect(WorldMmX0, WorldMmY0, WorldMmX1, WorldMmY1, kFineReadMarginMm);
	constexpr int64 TileSizePx = int64(vxc::kFineTileSize);
	OutTx0 = int32_t(vxc::floorDiv(Rect.px0, TileSizePx));
	OutTy0 = int32_t(vxc::floorDiv(Rect.py0, TileSizePx));
	OutTx1 = int32_t(vxc::floorDiv(Rect.px1, TileSizePx));
	OutTy1 = int32_t(vxc::floorDiv(Rect.py1, TileSizePx));
}

bool FVoxelFineTileStreamer::IsFootprintResident(int64 WorldMmX0, int64 WorldMmY0, int64 WorldMmX1,
                                                 int64 WorldMmY1) const
{
	int32_t Tx0, Ty0, Tx1, Ty1;
	CoveredTileRange(WorldMmX0, WorldMmY0, WorldMmX1, WorldMmY1, Tx0, Ty0, Tx1, Ty1);

	FRWScopeLock Lock(Lock_, SLT_ReadOnly);
	if (Sampler_.tileCount() == 0)
	{
		return false; // nothing loaded at all yet
	}
	// Whole-tile decode at load makes residency a per-TILE fact, so this is the
	// complete check -- no block walk, no decode, no I/O.
	for (int32_t Ty = Ty0; Ty <= Ty1; ++Ty)
	{
		for (int32_t Tx = Tx0; Tx <= Tx1; ++Tx)
		{
			if (Sampler_.findTile(Tx, Ty) == nullptr)
			{
				return false;
			}
		}
	}
	return true;
}

bool FVoxelFineTileStreamer::RequestFootprint(int64 WorldMmX0, int64 WorldMmY0, int64 WorldMmX1, int64 WorldMmY1)
{
	int32_t Tx0, Ty0, Tx1, Ty1;
	CoveredTileRange(WorldMmX0, WorldMmY0, WorldMmX1, WorldMmY1, Tx0, Ty0, Tx1, Ty1);

	FRWScopeLock Lock(Lock_, SLT_Write);
	bool bAllResident = true;
	for (int32_t Ty = Ty0; Ty <= Ty1; ++Ty)
	{
		for (int32_t Tx = Tx0; Tx <= Tx1; ++Tx)
		{
			const vxc::TileCoord T{Tx, Ty};
			if (KnownMissing_.find(TileHash(T)) != KnownMissing_.end())
			{
				bAllResident = false; // absent as of this ring; do not re-stat per candidate
				continue;
			}
			if (!EnsureTileResident_Locked(T))
			{
				bAllResident = false; // keep going: load whatever IS available, still report incomplete
			}
		}
	}
	// At least one required tile missing/invalid => block. Never voxelize over
	// a partially covered footprint: the uncovered part reads back as sea level.
	return bAllResident;
}

void FVoxelFineTileStreamer::TickResidencyAndEviction(vxc::TileCoord PlayerCoarseTile)
{
	FRWScopeLock Lock(Lock_, SLT_Write);

	if (!(PlayerCoarseTile == LastRingCentre_))
	{
		// The ring moved: everything we believed absent is worth one more look
		// (the bake frontier advances, and this is the only thing that retries).
		KnownMissing_.clear();
		LastRingCentre_ = PlayerCoarseTile;
	}

	const std::vector<vxc::TileCoord> Ring = vxc::squareTileRing(PlayerCoarseTile, RingRadiusTiles_);

	std::unordered_set<std::string> Pinned;
	Pinned.reserve(Ring.size());
	for (const vxc::TileCoord& T : Ring)
	{
		Pinned.insert(vxc::formatFineTileCacheKey(ProviderId_, Seed_, T.x, T.y));
	}
	Budget_.setPinned(std::move(Pinned));

	// Prefetch: best-effort load of every ring tile not yet resident. A tile
	// the frontier has not generated yet simply stays non-resident --
	// IsFootprintResident keeps refusing that area, which is the intended
	// "block until ready" behavior, not a bug to work around here.
	for (const vxc::TileCoord& T : Ring)
	{
		if (KnownMissing_.find(TileHash(T)) == KnownMissing_.end())
		{
			EnsureTileResident_Locked(T);
		}
	}

	// Evict whatever the budget now disallows, outside the pinned ring.
	for (const std::string& Key : Budget_.selectEvictions())
	{
		auto It = KeyToTile_.find(Key);
		if (It == KeyToTile_.end())
		{
			continue; // defensive: should not happen, every touched key is recorded above
		}
		UE_LOG(LogVoxelEarth, Log, TEXT("Fine tile (%d,%d) evicted (LRU budget %.2f GiB exceeded)."),
		       It->second.x, It->second.y, double(Budget_.budgetBytes()) / double(1ull << 30));
		Sampler_.unloadTile(It->second.x, It->second.y);
		Budget_.remove(Key);
		KeyToTile_.erase(It);
	}
}

uint64 FVoxelFineTileStreamer::ResidentBytes() const
{
	FRWScopeLock Lock(Lock_, SLT_ReadOnly);
	return Budget_.residentBytes();
}

uint64 FVoxelFineTileStreamer::BudgetBytes() const
{
	FRWScopeLock Lock(Lock_, SLT_ReadOnly);
	return Budget_.budgetBytes();
}

uint64 FVoxelFineTileStreamer::ResidentTileCount() const
{
	FRWScopeLock Lock(Lock_, SLT_ReadOnly);
	return uint64(Sampler_.tileCount());
}
