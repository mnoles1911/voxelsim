#include "VoxelFineTileStreamer.h"

#include "VoxelEarth.h"      // LogVoxelEarth
#include "VoxelTileCodec.h"  // VoxelEarth::GetFineTileDecompressor -- CODEC_ZSTD host boundary
#include "Misc/Paths.h"      // FPaths::Combine

#include "voxelcore/caverns.h"  // kCavernMaxReachMm -- see kFineReadMarginMm below
#include "voxelcore/core.h"     // kVoxelSizeMm
#include "voxelcore/tilerange.h" // FileRangeSource/readFineTilePreamble -- the ranged load

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
// (currently a vxc::FileRangeSource + vxc::readFineTilePreamble +
// vxc::fetchFineTileBlocks + vxc::validateAndParseFineTilePartial + a full
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
// FVoxelFineTileSamplerProxy -- the worker-facing query path, AND the place the
// block-until-ready rule is actually enforced.
//
// SHARED lock, and it is only sound because EnsureTileResident_Locked decodes
// every block of a tile before publishing it: vxc::FineTileSampler::blockFor
// then takes its found-block path and mutates nothing, so N workers reading
// concurrently is N pure reads. If whole-tile decode were ever relaxed back to
// lazy, this lock would have to become exclusive (see the header).
//
// THE RESIDENCY TEST HERE IS NOT REDUNDANT WITH THE CHUNK-ADMISSION GATE, and
// the header's "WHY THE CHUNK-ADMISSION GATE WAS NOT ENOUGH" section is the
// argument: FVoxelWorldImpl::Voxels is built over this sampler, so collision,
// agents, water and the pawn-spawn height all read through here without ever
// admitting a chunk. This is the one line every one of them crosses.
//
// COST: one extra findTile hash lookup per query on the hot path (the sampler
// then does its own). That is the price of the rule being true rather than
// conventional, it is paid only when the fine tier is switched on at all, and
// it is nothing beside the block lookup and spline evaluation that follow.
// ---------------------------------------------------------------------------

int32_t FVoxelFineTileSamplerProxy::elevationMm(int64_t px, int64_t py)
{
	// Routed by the FIXED production stride, not Sampler_.tileSize(): tileSize()
	// is 0 until something is loaded, and "nothing is resident" is exactly the
	// case that must be detected. Same constant, same reasoning, as
	// FVoxelFineTileStreamer::CoveredTiles.
	const vxc::TileCoord Tile = vxc::tileCoordForPixel(px, py, int64(vxc::kFineTileSize));
	{
		FRWScopeLock Lock(Owner.Lock_, SLT_ReadOnly);
		// Whole-tile decode at load makes residency a per-TILE fact, so this one
		// lookup is the complete check.
		if (Owner.Sampler_.findTile(Tile.x, Tile.y) != nullptr)
		{
			return Owner.Sampler_.elevationMm(px, py);
		}
	}
	// Lock released: the cold path may need it EXCLUSIVELY to load, and FRWLock
	// is not recursive.
	return Owner.ResolveNonResidentPixel(px, py);
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

	// SELF-CHECK: THE GATE REFUSES WHEN NOTHING IS RESIDENT.
	//
	// Nothing has been loaded yet, so IsFootprintResident MUST be false for any
	// footprint at all. That sounds too trivial to assert -- and it is precisely
	// the property that was never once checked, because every verification of
	// this gate was performed with the tile PRESENT.
	//
	// It costs two hash lookups on an empty map, at construction, and it is
	// worth them because of the shape of the regression it catches. The gate has
	// several ways to become VACUOUSLY TRUE without looking wrong: an empty
	// covering vector treated as "all resident" (see IsFootprintResident's early
	// return, which nothing else exercises), a residency loop that iterates zero
	// times, a short-circuit added for the "nothing loaded yet" case as a
	// perceived optimisation. Each of those admits every chunk in the world and
	// none of them faults. voxel-core's unit tests pin the pure coverage maths;
	// this pins the UE wiring around it, on every run, with nobody having to
	// remember.
	//
	// Deliberately checked at TWO very different footprints: the origin, and a
	// far-negative one, because a truncating tile route mishandles only the
	// latter and the leaking capture spawned at negative X.
	const bool bRefusesOrigin = !IsFootprintResident(0, 0, 1, 1);
	const bool bRefusesNegative = !IsFootprintResident(-1'000'000'000, -1'000'000'000, -999'999'999, -999'999'999);
	if (!bRefusesOrigin || !bRefusesNegative)
	{
		UE_LOG(LogVoxelEarth, Fatal,
		       TEXT("FINE TIER GATE IS BROKEN AT STARTUP: IsFootprintResident returned TRUE with zero tiles loaded ")
		       TEXT("(origin refused=%d, far-negative refused=%d). The gate would admit every chunk in the world over ")
		       TEXT("non-resident tiles and this run would generate terrain no other client reproduces. Refusing to ")
		       TEXT("start rather than produce it."),
		       bRefusesOrigin ? 1 : 0, bRefusesNegative ? 1 : 0);
	}
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

	// RANGED READ, NOT A WHOLE-FILE SLURP (task #52). A file supports ranges
	// natively -- seek and read -- so this needs no server, no protocol and no
	// change to how tiles are delivered; it is the same path LocalPathFor has
	// always built, opened once and seeked instead of slurped.
	//
	// WHAT IS SKIPPED, and it is the whole of the saving: the §6 FLOW plane.
	// Nothing in ue-project reads it (grep: the only decodeFlowBlock callers in
	// the repo are voxel-core/bench probes, which load tiles from disk
	// themselves), and on the shipped bv12 tiles FLOW_DATA is 12.6 MB of a
	// 51.6 MB file. Measured over the four corridor tiles, this takes the load
	// from 179.4 MB to 133.7 MB read and from 197.4 MB to 133.7 MB held as file
	// bytes -- a 25% cut in a read that happens synchronously on the game
	// thread and which the header warns can block for a second or more.
	//
	// WHAT IS *NOT* SKIPPED, and why this is not the big win: every ELEVATION
	// block is still fetched, because rule 1 (see the header's threading note)
	// decodes the whole tile at load and therefore needs every one of them.
	// Block-granular residency would take the same working set to ~400 KB read
	// and ~1.6 MB held -- 100x further -- but it requires retiring rule 1, which
	// is what makes the worker-facing query path a pure read under a shared
	// lock. That is a separate change and it is deliberately not made here; see
	// docs/tile-slicing-2026-08-04.md and the report for the measured prize.
	vxc::FileRangeSource Source(StdPath);
	if (!Source.ok())
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

	// The preamble: header, section table, elevation index, water index, basin
	// table. Four DISJOINT regions, not a prefix -- encode_v2 puts each plane's
	// multi-megabyte data section immediately after its own index -- which is
	// why this is a call into voxel-core rather than a "read the first 64 KB"
	// here. ~62-67 KB of content in 2-4 requests.
	vxc::FinePreambleRequest Want;
	Want.wantFlow = false;   // see above: nothing in this module reads flow
	Want.wantWater = true;   // lakes.h / riverribbon.h decode water lazily
	Want.wantBasins = true;  // the lake registry; absent != empty, so fetch it
	vxc::FineTileBytes Held;
	vxc::FineError PreambleErr = vxc::FineError::kNone;
	if (!vxc::readFineTilePreamble(Source, Source.fileSize(), Want, Held, &PreambleErr))
	{
		++CorruptLoads_;
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("Fine tile (%d,%d) at %s: could not read its preamble (%hs) -- discarding, NOT using it. ")
		       TEXT("A truncated file is caught here rather than after a block decodes to plausible terrain."),
		       Tile.x, Tile.y, *Path, vxc::fineErrorName(PreambleErr));
		return false;
	}

	// "Validate, never trust": FineTile::parsePartial's own all-or-nothing
	// structural check, PLUS the identity check that follows
	// EditLog::checkProvider()'s precedent (compare a stamped identity, refuse
	// on mismatch) rather than inventing a second mechanism. The decompressor is
	// the sampler's, so a CODEC_ZSTD tile is decodable here exactly when this
	// build has zstd.
	vxc::FineTileValidationResult Validated = vxc::validateAndParseFineTilePartial(
		std::move(Held), Seed_, Tile.x, Tile.y, Sampler_.decompressor());
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

	// Now the payload bytes, in coalesced ranges planned off the indices the
	// preamble just gave us. CONSTANT blocks are served from the index and cost
	// no request at all -- on these tiles that is 72-87% of the water plane.
	//
	// EVERY non-CONSTANT elevation block, because rule 1 decodes them all below
	// and a block whose bytes are absent would fail that decode with
	// kBlockNotResident rather than silently yielding sea level. The residency
	// invariant this class publishes ("resident tile => every block readable")
	// is therefore still exactly true after this call, or the load fails.
	if (!vxc::fetchFineTileBlocks(Source, *Validated.tile, vxc::FinePlane::kElevation,
	                              vxc::fineNonConstantBlocks(*Validated.tile, vxc::FinePlane::kElevation)))
	{
		++CorruptLoads_;
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("Fine tile (%d,%d) at %s: elevation block fetch failed -- discarding. The file shrank or ")
		       TEXT("changed under us between the preamble read and the payload read."),
		       Tile.x, Tile.y, *Path);
		return false;
	}
	// The water plane, which lakes.h / riverribbon.h decode lazily out of the
	// tile's own bytes long after this function returns. It must be fetched HERE
	// rather than on demand: those decoders run on worker threads, and an
	// unfetched water block would answer kBlockNotResident there with no way to
	// go and get it. 51-174 KB, and mostly one coalesced span.
	if (Validated.tile->hasWater() &&
	    !vxc::fetchFineTileBlocks(Source, *Validated.tile, vxc::FinePlane::kWater,
	                              vxc::fineNonConstantBlocks(*Validated.tile, vxc::FinePlane::kWater)))
	{
		++CorruptLoads_;
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("Fine tile (%d,%d) at %s: water block fetch failed -- discarding."), Tile.x, Tile.y, *Path);
		return false;
	}

	// Capture the geometry before the move below hands ownership to the sampler.
	const int64 TileSizePx = int64(Validated.tile->size());
	// What this tile HOLDS, which is now less than its size on disk: the flow
	// plane's 12.6 MB (on the shipped tiles) was never read. Charging the LRU
	// the fetched bytes rather than the file size is what makes the budget mean
	// what it says once the two stop being equal.
	const uint64 ByteCount = Validated.tile->residentFileBytes();
	const uint64 FileSizeOnDisk = Source.fileSize();
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
	       TEXT("Fine tile (%d,%d) resident: %llu px edge, %.1f MB read of a %.1f MB file in %llu range(s) ")
	       TEXT("+ %.1f MB decoded lattice, full decode %.0f ms. Resident tiles now %llu, %.2f GiB of ")
	       TEXT("%.2f GiB budget."),
	       Tile.x, Tile.y, (unsigned long long)TileSizePx, double(ByteCount) / 1e6,
	       double(FileSizeOnDisk) / 1e6, (unsigned long long)Source.requests,
	       double(TileDecodedBytes) / 1e6, DecodeMs, (unsigned long long)Sampler_.tileCount(),
	       double(Budget_.residentBytes()) / double(1ull << 30), double(Budget_.budgetBytes()) / double(1ull << 30));
	return true;
}

vxc::TileCoord FVoxelFineTileStreamer::CoarseTileForWorldMm(int64 WorldMmX, int64 WorldMmY)
{
	return vxc::tileCoordForWorldMm(WorldMmX, WorldMmY, kTileFootprintMm);
}

std::vector<vxc::TileCoord> FVoxelFineTileStreamer::CoveredTiles(int64 WorldMmX0, int64 WorldMmY0, int64 WorldMmX1,
                                                                 int64 WorldMmY1)
{
	// One conversion, in voxel-core, applying BOTH the cavern read margin and
	// the carrier stencil -- see vxc::fineReadPixelRect's comment for why
	// leaving either out is a silent desync rather than a fault. The FIXED
	// production stride is passed because tile selection has to work before
	// anything is resident, and every production tile is kFineTileSize (the
	// per-tile `size` field is a test-fixture convenience, see tilestore.h). A
	// loaded tile whose size disagreed would have been refused by
	// FineTileSampler::loadTile anyway.
	//
	// This function is now four lines because the arithmetic moved to
	// vxc::tilesCoveringFootprint. That was not tidying: while it lived here it
	// could only be exercised by launching an editor over a real bake, which is
	// why it was only ever exercised with the tile PRESENT. In voxel-core the
	// absent direction is a unit test (test_tilestreaming.cpp,
	// "gate_blocks_when_nothing_is_resident" and the read-margin pair).
	return vxc::tilesCoveringFootprint(WorldMmX0, WorldMmY0, WorldMmX1, WorldMmY1, kFineReadMarginMm,
	                                   int64(vxc::kFineTileSize));
}

bool FVoxelFineTileStreamer::IsFootprintResident(int64 WorldMmX0, int64 WorldMmY0, int64 WorldMmX1,
                                                 int64 WorldMmY1) const
{
	const std::vector<vxc::TileCoord> Tiles = CoveredTiles(WorldMmX0, WorldMmY0, WorldMmX1, WorldMmY1);
	if (Tiles.empty())
	{
		// vxc::tilesCoveringFootprint declines only on degenerate geometry, which
		// this call site cannot produce (compile-time stride and pitch). Refuse
		// rather than admit: "covers nothing" must never read as "all resident".
		return false;
	}

	FRWScopeLock Lock(Lock_, SLT_ReadOnly);
	// Whole-tile decode at load makes residency a per-TILE fact, so this is the
	// complete check -- no block walk, no decode, no I/O.
	for (const vxc::TileCoord& T : Tiles)
	{
		if (Sampler_.findTile(T.x, T.y) == nullptr)
		{
			return false;
		}
	}
	return true;
}

bool FVoxelFineTileStreamer::RequestFootprint(int64 WorldMmX0, int64 WorldMmY0, int64 WorldMmX1, int64 WorldMmY1)
{
	const std::vector<vxc::TileCoord> Tiles = CoveredTiles(WorldMmX0, WorldMmY0, WorldMmX1, WorldMmY1);
	if (Tiles.empty())
	{
		return false; // see IsFootprintResident: decline, never admit
	}

	FRWScopeLock Lock(Lock_, SLT_Write);
	bool bAllResident = true;
	for (const vxc::TileCoord& T : Tiles)
	{
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
	// At least one required tile missing/invalid => block. Never voxelize over
	// a partially covered footprint: the uncovered part reads back as sea level.
	return bAllResident;
}

// ---------------------------------------------------------------------------
// The funnel's cold path: this pixel's tile is not resident.
// ---------------------------------------------------------------------------

int32_t FVoxelFineTileStreamer::ResolveNonResidentPixel(int64_t px, int64_t py)
{
	const vxc::TileCoord Tile = vxc::tileCoordForPixel(px, py, int64(vxc::kFineTileSize));

	// BLOCK UNTIL READY, in the literal sense, on the one thread where blocking
	// is permitted. This is the same synchronous load RequestFootprint performs
	// and is legal for exactly the same reason: the game thread may do disk I/O
	// and may hold this lock exclusively, whereas a meshing worker doing so
	// would stall the whole pool behind a 200 MB read.
	//
	// This single branch is what fixes the ~100 unguarded consumers found in the
	// audit -- character movement, the agent region graph, the water CA's
	// solidity callback, GetSurfaceHeightUU, the SWE fixtures, the HUD. Every
	// one of them runs on the game thread, so every one of them now gets a
	// correct answer instead of sea level, without a gate call of its own.
	if (IsInGameThread())
	{
		FRWScopeLock Lock(Lock_, SLT_Write);
		// Re-check under the exclusive lock: another game-thread call earlier in
		// this frame, or TickResidencyAndEviction, may have loaded it since the
		// shared-lock miss above.
		if (Sampler_.findTile(Tile.x, Tile.y) != nullptr)
		{
			return Sampler_.elevationMm(px, py);
		}
		// KnownMissing_ is the "we already stat'ed this and it is not on disk"
		// memo. Honouring it here keeps a query storm over a genuinely absent
		// tile from re-stat'ing the filesystem millions of times; the memo is
		// cleared whenever the ring centre moves, so a tile the bake produces
		// mid-session is still picked up.
		if (KnownMissing_.find(TileHash(Tile)) == KnownMissing_.end())
		{
			BlockingLoads_.fetch_add(1, std::memory_order_relaxed);
			if (EnsureTileResident_Locked(Tile))
			{
				return Sampler_.elevationMm(px, py);
			}
		}
		return ReportGateLeak_Locked(Tile, px, py);
	}

	// A WORKER got here. With the chunk-admission gate in front of the meshing
	// path this should be unreachable, and if it is reached the gate has a hole
	// -- so it is reported as a leak rather than papered over. It is emphatically
	// NOT loaded here: a worker taking this lock exclusively for a 200 MB read
	// serialises every other worker behind it, and the tile may never arrive
	// anyway, so "block" would mean "hang".
	FRWScopeLock Lock(Lock_, SLT_Write);
	if (Sampler_.findTile(Tile.x, Tile.y) != nullptr)
	{
		return Sampler_.elevationMm(px, py); // raced with a load; not a leak
	}
	return ReportGateLeak_Locked(Tile, px, py);
}

int32_t FVoxelFineTileStreamer::ReportGateLeak_Locked(vxc::TileCoord Tile, int64_t px, int64_t py)
{
	const uint64 LeakCount = GateLeaks_.fetch_add(1, std::memory_order_relaxed) + 1;

	// WHY FATAL, AND WHY ONLY WHEN UNATTENDED.
	//
	// A leak means this process is generating collision and edits from sea level
	// where a client holding the tile generates real terrain. There is no
	// partially-correct outcome to salvage: the run's terrain is not
	// reproducible, so every number, screenshot and digest it goes on to produce
	// describes a world that does not exist. An unattended run that continues
	// past this point does not degrade, it LIES -- and it lies while reporting
	// success, which is strictly worse than not finishing, because the next
	// person reads the artifact and believes it. That is not a hypothetical:
	// this whole class of bug survived because a run leaked 18.7 million times
	// and still exited 0.
	//
	// Interactive editor sessions get an Error instead. A developer poking at a
	// half-populated tile cache is not producing an artifact anyone will trust,
	// and killing their editor on the first frame the camera crosses the bake
	// frontier teaches them to switch the check off -- which costs the
	// unattended case its protection too.
	const bool bFirstForThisTile = LeakReportedTiles_.insert(TileHash(Tile)).second;

	if (bLeakIsFatal_)
	{
		UE_LOG(LogVoxelEarth, Fatal,
		       TEXT("FINE TIER GATE LEAK (fatal, unattended run): elevation query at fine pixel (%lld,%lld) needs tile ")
		       TEXT("(%d,%d), which is not resident and could not be loaded from %s. Answering it would mean sea level, ")
		       TEXT("and on the fine tier sea level is not a fallback -- it is terrain no other client computes, so this ")
		       TEXT("run's output would not be reproducible. Stopping instead of producing an artifact that looks fine. ")
		       TEXT("Leaks so far=%llu, blocking loads=%llu, resident=%llu tile(s), absentOnDisk=%llu, corrupt=%llu, ")
		       TEXT("identityMismatch=%llu. If the tile SHOULD exist, check -VoxelFineTileProviderId: it is part of the ")
		       TEXT("cache key and of the on-disk path, and omitting it makes every lookup miss. Pass ")
		       TEXT("-VoxelFineTileGateFatal=0 to downgrade this to an Error."),
		       (long long)px, (long long)py, Tile.x, Tile.y, *LocalPathFor(Tile), (unsigned long long)LeakCount,
		       (unsigned long long)BlockingLoads_.load(std::memory_order_relaxed), (unsigned long long)Sampler_.tileCount(),
		       (unsigned long long)MissingFileLoads_, (unsigned long long)CorruptLoads_,
		       (unsigned long long)IdentityMismatches_);
		// UE_LOG(Fatal) does not return. The sea-level answer below is
		// unreachable here and exists only for the non-fatal path.
	}

	if (bFirstForThisTile)
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("FINE TIER GATE LEAK: elevation query at fine pixel (%lld,%lld) landed in NON-RESIDENT tile (%d,%d) ")
		       TEXT("(%s) and is being answered with SEA LEVEL. That is a desync, not a fallback: this client's terrain, ")
		       TEXT("collision and edits here will not match a client that has the tile. Logged once per tile; total ")
		       TEXT("leaks=%llu. Thread=%s. Unattended runs stop on this instead (SetLeakIsFatal)."),
		       (long long)px, (long long)py, Tile.x, Tile.y, *LocalPathFor(Tile), (unsigned long long)LeakCount,
		       IsInGameThread() ? TEXT("game") : TEXT("worker"));
	}

	// The sampler's own missingTileQueries bump happens inside this call, which
	// keeps MaybeLogCounters' after-the-fact check working unchanged.
	return Sampler_.elevationMm(px, py);
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
