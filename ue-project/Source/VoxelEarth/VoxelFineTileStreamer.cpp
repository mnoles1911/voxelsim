#include "VoxelFineTileStreamer.h"

#include "VoxelEarth.h" // LogVoxelEarth
#include "Misc/Paths.h" // FPaths::Combine

#include <filesystem>
#include <optional>
#include <utility>

// See VoxelFineTileStreamer.h's class-level comment for the full scope note.
// Short version: this file is a synchronous, local-disk-only implementation
// of the residency/prefetch/eviction POLICY (voxelcore/tilestreaming.h owns
// the actual maths -- ring geometry, dilation, block coverage, LRU ordering,
// cache-key formatting, validation). Everything here is glue: turning UE
// world-mm coordinates into calls against that pure library, and turning its
// answers into vxc::FineTileSampler loads/unloads.
//
// A future async/network loader would replace EnsureTileResident's body
// (currently vxc::readFileBytes + vxc::validateAndParseFineTile, both
// synchronous) with a background fetch that completes on some later tick;
// the public API (IsFootprintResident / RequestFootprint /
// TickResidencyAndEviction) would not need to change shape, because
// RequestFootprint already returns false rather than blocking forever when
// something isn't ready -- "not yet resident" and "will never be resident"
// already look identical to a caller, which is exactly the "block until
// ready, never fall back" contract the plan requires.

FVoxelFineTileStreamer::FVoxelFineTileStreamer(FString RootDir, FString ProviderId, uint64 Seed, uint64 BudgetBytes)
	: RootDir_(MoveTemp(RootDir))
	, ProviderId_(TCHAR_TO_UTF8(*ProviderId))
	, Seed_(Seed)
	, Sampler_(Seed)
	, Budget_(BudgetBytes)
{
}

FString FVoxelFineTileStreamer::LocalPathFor(vxc::TileCoord Tile) const
{
	// Mirrors terrain-service/terrain_service/cache.py's TileCache.fine_path
	// layout exactly (RootDir_ stands in for cache.py's `root`):
	// <root>/<provider_id>/<seed:016x>/s16/<x>_<y>.vxtl. Built from the SAME
	// vxc::formatFineTileCacheKey the LRU bookkeeping uses, so the on-disk
	// path and the cache key can never drift apart.
	const std::string Key = vxc::formatFineTileCacheKey(ProviderId_, Seed_, Tile.x, Tile.y);
	return FPaths::Combine(RootDir_, FString(Key.c_str()) + TEXT(".vxtl"));
}

bool FVoxelFineTileStreamer::EnsureTileResident(vxc::TileCoord Tile)
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
		return false; // not on disk (yet, or ever) -- caller keeps deferring, never a fallback
	}
	const uint64 ByteCount = Bytes->size();

	// "Validate, never trust": FineTile::parse's own all-or-nothing
	// structural check, PLUS the identity check that follows
	// EditLog::checkProvider()'s precedent (compare a stamped identity,
	// refuse on mismatch) rather than inventing a second mechanism.
	vxc::FineTileValidationResult Validated =
		vxc::validateAndParseFineTile(std::move(*Bytes), Seed_, Tile.x, Tile.y);
	if (Validated.verdict == vxc::FineTileVerdict::kCorrupt)
	{
		++CorruptLoads_;
		UE_LOG(LogVoxelEarth, Warning, TEXT("Fine tile (%d,%d) at %s failed structural validation (truncated or ")
		       TEXT("malformed) -- discarding, NOT using it. Re-fetch required."), Tile.x, Tile.y, *Path);
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

	// Capture blockDim before the move below hands the tile's ownership to
	// the sampler.
	const uint32_t BlockDim = Validated.tile->blockDim();

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

	BlockDimPx_ = BlockDim;
	const std::string Key = vxc::formatFineTileCacheKey(ProviderId_, Seed_, Tile.x, Tile.y);
	Budget_.touch(Key, ByteCount);
	KeyToTile_.emplace(Key, Tile);
	return true;
}

vxc::TileCoord FVoxelFineTileStreamer::CoarseTileForWorldMm(int64 WorldMmX, int64 WorldMmY)
{
	// The fine tier's footprint is IDENTICAL to the coarse (s1) tile's --
	// vxtl-v2-format.md §1 -- 8192 fine px * 1875 mm/px == 512 s1 px *
	// 30000 mm/px == 15,360,000 mm == 15.36 km. Use the fixed pixel-size
	// constant (always known) rather than a loaded tile's `size` field
	// (unknown before the first load), so tile SELECTION never has a
	// bootstrap dependency on anything being resident yet.
	constexpr int64 kTileFootprintMm = int64(vxc::kFineTileSize) * vxc::tilePixelSizeMm(vxc::kFineTileScale);
	return vxc::tileCoordForWorldMm(WorldMmX, WorldMmY, kTileFootprintMm);
}

// Shared by IsFootprintResident/RequestFootprint: world-mm rect -> the
// carrier-dilated fine PixelRect it must cover. pixelSizeMm(16) is a fixed
// compile-time constant (1875), so this never depends on anything being
// resident yet either.
static vxc::PixelRect DilatedFinePixelFootprint(int64 WorldMmX0, int64 WorldMmY0, int64 WorldMmX1, int64 WorldMmY1)
{
	const int32_t PxSizeMm = vxc::tilePixelSizeMm(vxc::kFineTileScale);
	const vxc::PixelRect Footprint{vxc::floorDiv(WorldMmX0, PxSizeMm), vxc::floorDiv(WorldMmY0, PxSizeMm),
	                               vxc::floorDiv(WorldMmX1, PxSizeMm), vxc::floorDiv(WorldMmY1, PxSizeMm)};
	return vxc::dilateForCarrierStencil(Footprint);
}

bool FVoxelFineTileStreamer::IsFootprintResident(int64 WorldMmX0, int64 WorldMmY0, int64 WorldMmX1, int64 WorldMmY1)
{
	if (Sampler_.tileSize() == 0)
	{
		return false; // nothing loaded at all yet
	}
	const vxc::PixelRect Dilated = DilatedFinePixelFootprint(WorldMmX0, WorldMmY0, WorldMmX1, WorldMmY1);
	// prewarm() only ever touches tiles ALREADY in the sampler (no disk I/O):
	// it returns false immediately for any block belonging to a tile that
	// was never loadTile()'d, and decodes (pure compute over already-
	// resident bytes) whatever it needs from tiles that were. That makes it
	// safe to use here as a pure, no-I/O residency query.
	return Sampler_.prewarm(Dilated.px0, Dilated.py0, Dilated.px1, Dilated.py1);
}

bool FVoxelFineTileStreamer::RequestFootprint(int64 WorldMmX0, int64 WorldMmY0, int64 WorldMmX1, int64 WorldMmY1)
{
	const vxc::PixelRect Dilated = DilatedFinePixelFootprint(WorldMmX0, WorldMmY0, WorldMmX1, WorldMmY1);

	// Which coarse tiles does the dilated footprint touch? Uses whichever
	// stride is currently authoritative: kFineTileSize before the first load
	// (production tiles are always exactly this size; the format's per-tile
	// `size` field is a test-fixture convenience, see tilestore.h), or
	// Sampler_.tileSize() once at least one tile has actually loaded and
	// pinned the real stride.
	const int64_t TileSizePx = Sampler_.tileSize() != 0 ? int64_t(Sampler_.tileSize()) : int64_t(vxc::kFineTileSize);
	const int64_t Tx0 = vxc::floorDiv(Dilated.px0, TileSizePx);
	const int64_t Tx1 = vxc::floorDiv(Dilated.px1, TileSizePx);
	const int64_t Ty0 = vxc::floorDiv(Dilated.py0, TileSizePx);
	const int64_t Ty1 = vxc::floorDiv(Dilated.py1, TileSizePx);

	bool bAllResident = true;
	for (int64_t Ty = Ty0; Ty <= Ty1; ++Ty)
	{
		for (int64_t Tx = Tx0; Tx <= Tx1; ++Tx)
		{
			if (!EnsureTileResident(vxc::TileCoord{int32_t(Tx), int32_t(Ty)}))
			{
				bAllResident = false; // keep going: load whatever IS available, still report incomplete
			}
		}
	}
	if (!bAllResident)
	{
		return false; // at least one required tile is missing/invalid -- block, do not prewarm partial data
	}

	// Every needed tile is now resident; tileSize()/blockDim are
	// authoritative. Decode the exact dilated blocks.
	return Sampler_.prewarm(Dilated.px0, Dilated.py0, Dilated.px1, Dilated.py1);
}

void FVoxelFineTileStreamer::TickResidencyAndEviction(vxc::TileCoord PlayerCoarseTile, int32_t FineRingRadiusTiles)
{
	const std::vector<vxc::TileCoord> Ring = vxc::squareTileRing(PlayerCoarseTile, FineRingRadiusTiles);

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
		EnsureTileResident(T);
	}

	// Evict whatever the budget now disallows, outside the pinned ring.
	for (const std::string& Key : Budget_.selectEvictions())
	{
		auto It = KeyToTile_.find(Key);
		if (It == KeyToTile_.end())
		{
			continue; // defensive: should not happen, every touched key is recorded in EnsureTileResident
		}
		Sampler_.unloadTile(It->second.x, It->second.y);
		Budget_.remove(Key);
		KeyToTile_.erase(It);
	}
}
