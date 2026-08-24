#include "VoxelFineTileStreamer.h"

#include "VoxelEarth.h"      // LogVoxelEarth
#include "VoxelFineLockMeter.h" // FLockScope -- the instrument on Lock_, and the two switches
#include "VoxelDebug.h"    // LogVoxelPerf -- the probe line rides the perf category
#include "VoxelTileCodec.h"  // VoxelEarth::GetFineTileDecompressor -- CODEC_ZSTD host boundary
#include "Misc/Paths.h"      // FPaths::Combine

#include "voxelcore/caverns.h"  // kCavernMaxReachMm -- see kFineReadMarginMm below
#include "voxelcore/core.h"     // kVoxelSizeMm
#include "voxelcore/tilerange.h" // FileRangeSource/readFineTilePreamble -- the ranged load

#include <chrono>       // std::filesystem::file_time_type's rep -- see the retry contract
#include <filesystem>
#include <optional>
#include <system_error> // std::error_code: last_write_time without exceptions
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

	// voxel-core's sentence, as an FString. The explanation lives there (and is
	// unit-tested there) rather than in this log call, because this file is the
	// half that cannot be tested without an editor, a real bake and a
	// deliberately stale binary -- which is precisely why its messages went
	// unnoticed pointing at the wrong thing.
	FString DescribeRejection(vxc::FineError Err, const vxc::FineHeaderFacts& Facts)
	{
		return FString(UTF8_TO_TCHAR(vxc::fineDescribeRejection(Err, Facts).c_str()));
	}

	// THE FILE'S VERSION, not its contents. Size plus last-write time is what
	// the retry memo is keyed on (see the contract in the header): it costs one
	// stat rather than the 60+ KB preamble read a real re-validation costs, and
	// it changes for every one of the things that can legitimately make a
	// refused tile loadable -- a re-bake, a download completing, a file
	// repaired by hand. It cannot detect a rewrite that preserved both, which
	// is why the memo is a RETRY suppressor and not a cache of verdicts:
	// nothing is served from it, the worst case is one more restart.
	int64 FileWriteTimeTicks(const std::filesystem::path& P)
	{
		std::error_code Ec;
		const std::filesystem::file_time_type T = std::filesystem::last_write_time(P, Ec);
		// 0 on a filesystem that will not answer: the memo then keys on size
		// alone, which is weaker but never wrong in the dangerous direction (a
		// stale memo only costs a retry that a session restart recovers).
		return Ec ? int64(0) : int64(T.time_since_epoch().count());
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
		VoxelFineLock::FLockScope Lock(Owner.Lock_, SLT_ReadOnly, VoxelFineLock::ESite::ElevShared);
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
	VoxelFineLock::FLockScope Lock(Owner.Lock_, SLT_ReadOnly, VoxelFineLock::ESite::ClimateShared);
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

	// LATCH BOTH SWITCHES HERE, ON THE GAME THREAD, AND BEFORE THE GATE
	// SELF-CHECK BELOW USES THEM. They are function-local statics inside
	// VoxelFineLockMeter.h, and a worker's first elevationMm would otherwise be
	// the thread that initialises them -- which puts FCommandLine::Get() and a
	// static guard variable on the hottest read in the process. Reading them
	// once here makes every later read a plain load.
	const int32 MeterModeNow = VoxelFineLock::MeterMode();
	const int32 FastModeNow = VoxelFineLock::FastMode();
	LastProbeLogSeconds_ = FPlatformTime::Seconds();

	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Fine lock: fast=%d (%s) meter=%d (%s). Fast modes: 0=exclusive always (today), ")
	       TEXT("1=shared when already resident, 2=+lock-free game-thread mirror, 3=+mirror audit. ")
	       TEXT("Meter modes: 0=off, 1=traffic, 2=traffic+timing except the per-pixel reads (the mode to ")
	       TEXT("measure with), 3=timing everywhere (depresses throughput; never quote chunks/s from it)."),
	       FastModeNow,
	       FastModeNow == 0 ? TEXT("RequestFootprint takes Lock_ EXCLUSIVELY on every call")
	                        : TEXT("RequestFootprint escalates to exclusive only when a tile must be loaded"),
	       MeterModeNow,
	       MeterModeNow == 0   ? TEXT("no counters, no timing -- lock paths byte-identical")
	       : MeterModeNow == 1 ? TEXT("acquisition counts only")
	       : MeterModeNow == 2 ? TEXT("counts everywhere + wait/hold except on the per-pixel reads")
	                           : TEXT("counts + wait/hold EVERYWHERE, per-pixel reads included"));

	// SELF-CHECK 1: THE ALLOCATION-FREE COVERAGE AGREES WITH voxel-core'S.
	// Runs BEFORE the gate self-check below, because with -VoxelFineLockFast>0
	// that check now goes through the fast path -- so the fast path's own
	// coverage has to be established first, or a wrong range would be validated
	// by a gate that a wrong range had already broken.
	SelfCheckTileRangeAgreesWithCoverage_();

	// SELF-CHECK 2: THE GATE REFUSES WHEN NOTHING IS RESIDENT.
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

FString FVoxelFineTileStreamer::DiagnoseNamespace(vxc::TileCoord Tile) const
{
	// Everything here is best-effort and failure-path-only: if the root cannot
	// be listed we say nothing extra rather than turning a diagnosis into a
	// second fault. std::error_code overloads throughout, for that reason.
	std::error_code Ec;
	const std::filesystem::path Root(*RootDir_);
	if (!std::filesystem::is_directory(Root, Ec))
	{
		return FString::Printf(
			TEXT(" THE FINE CACHE ROOT ITSELF IS NOT A DIRECTORY: %s does not exist or is not readable, so no "
			     "namespace under it could match. Check -VoxelFineTileDir (or DefaultFineTileDir in "
			     "Config/DefaultGame.ini)."),
			*RootDir_);
	}

	// Does the CONFIGURED namespace even exist here? This is the single most
	// informative bit and it is one stat. "The directory is not there" and "the
	// directory is there but this tile has not been baked" call for opposite
	// actions -- re-pin versus wait for the bake -- and the old message could
	// not tell them apart because it printed one path for both.
	const FString ConfiguredId(ProviderId_.c_str());
	const bool bConfiguredDirExists =
		std::filesystem::is_directory(Root / ProviderId_, Ec);

	// Which namespaces under this root actually hold the wanted tile, for THIS
	// seed? Formatted through vxc::formatFineTileCacheKey with each candidate
	// id rather than by pasting the path grammar together here, so this cannot
	// drift from the grammar LocalPathFor and the LRU key already share.
	TArray<FString> Holders;
	int32 NamespacesSeen = 0;
	for (std::filesystem::directory_iterator It(Root, std::filesystem::directory_options::skip_permission_denied, Ec),
	     End;
	     It != End && !Ec; It.increment(Ec))
	{
		if (!It->is_directory(Ec))
		{
			continue;
		}
		const std::string Candidate = It->path().filename().string();
		++NamespacesSeen;
		const std::string Key = vxc::formatFineTileCacheKey(Candidate, Seed_, Tile.x, Tile.y);
		if (std::filesystem::exists(Root / (Key + ".vxtl"), Ec))
		{
			Holders.Add(FString(Candidate.c_str()));
		}
	}
	Holders.Sort();

	if (Holders.Num() > 0)
	{
		// THE ANSWER, AND THE EXACT FLAG. An operator reading a fatal log at
		// 02:00 should not have to know that the id is a bake fingerprint, that
		// it lives in an ini, or that a command-line -VoxelFineTileDir does NOT
		// carry the id with it. They should be able to copy one line.
		FString Copyable;
		for (const FString& Holder : Holders)
		{
			Copyable += FString::Printf(TEXT("\n    -VoxelFineTileProviderId=%s"), *Holder);
		}
		return FString::Printf(
			TEXT(" NAMESPACE MISMATCH -- THIS TILE IS BAKED, UNDER A DIFFERENT PROVIDER ID. This run is configured "
			     "for fine provider id '%s'%s, but %s holds tile (%d,%d) for seed %016llx under %d OTHER namespace(s). "
			     "The fine provider id is a fingerprint of the terrain-service BAKE's constants, so it MOVES every "
			     "time the bake changes -- and -VoxelFineTileDir on the command line does NOT bring the matching id "
			     "with it (the id resolves independently, from -VoxelFineTileProviderId or DefaultFineTileProviderId "
			     "in Config/DefaultGame.ini). Re-run with exactly one of:%s\nSee "
			     "docs/fine-tile-provider-identity.md, and read the namespace the bake wrote off %s/<id>/%016llx/"
			     "world-identity.json rather than deriving it."),
			*ConfiguredId,
			bConfiguredDirExists ? TEXT("") : TEXT(" (which has NO directory under this cache root at all)"),
			*RootDir_, Tile.x, Tile.y, (unsigned long long)Seed_, Holders.Num(), *Copyable, *RootDir_,
			(unsigned long long)Seed_);
	}

	if (!bConfiguredDirExists)
	{
		return FString::Printf(
			TEXT(" THE CONFIGURED NAMESPACE DOES NOT EXIST UNDER THIS ROOT: no directory '%s' in %s (%d namespace "
			     "directory/ies are present, none holding tile (%d,%d) for seed %016llx). Either the cache root or "
			     "the provider id is wrong for this run -- they are resolved INDEPENDENTLY, so overriding "
			     "-VoxelFineTileDir alone leaves the id pointing at whatever Config/DefaultGame.ini pins. See "
			     "docs/fine-tile-provider-identity.md."),
			*ConfiguredId, *RootDir_, NamespacesSeen, Tile.x, Tile.y, (unsigned long long)Seed_);
	}

	return FString::Printf(
		TEXT(" The namespace is right and the tile is simply NOT BAKED: '%s' exists under %s, and no namespace there "
		     "holds tile (%d,%d) for seed %016llx. This is a coverage gap, not an identity mismatch -- bake the tile "
		     "(terrain-service: pregen --mode bake, or tools/bake_tiles_from_cache.py --tiles=\"%d,%d\") or move the "
		     "camera to a baked tile."),
		*ConfiguredId, *RootDir_, Tile.x, Tile.y, (unsigned long long)Seed_, Tile.x, Tile.y);
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
		//
		// ABSENT AND REFUSED ARE NOT THE SAME STATE and must not be merged:
		// this one resolves itself when the bake frontier arrives, the other
		// needs a human. A file that has GONE (deleted, or moved aside for a
		// re-bake) has no version left to remember, so its refusal memo is
		// dropped here -- whatever appears next earns a clean set of attempts.
		++MissingFileLoads_;
		KnownMissing_.insert(TileHash(Tile));
		LoadFailures_.erase(TileHash(Tile));
		return false;
	}

	// --- HAVE WE ALREADY REFUSED THESE EXACT BYTES? -------------------------
	//
	// This is the whole of the infinite-retry fix, and it sits here -- after
	// the open, before the 60+ KB preamble read -- because the file's identity
	// is what the answer depends on. Everything below this point is work that
	// a previously-refused file would spend again to reach the same verdict:
	// the read, the parse, the log line. At 30 ms a frame, times four tiles,
	// for the length of a session.
	const uint64 Hash = TileHash(Tile);
	const uint64 FileSizeNow = Source.fileSize();
	const int64 WriteTimeNow = FileWriteTimeTicks(StdPath);
	if (auto FailIt = LoadFailures_.find(Hash); FailIt != LoadFailures_.end())
	{
		if (FailIt->second.FileSize == FileSizeNow && FailIt->second.WriteTime == WriteTimeNow)
		{
			// Same bytes as last time. A permanent refusal is settled; a
			// transient one is settled once its attempts are spent.
			if (FailIt->second.bPermanent || FailIt->second.Attempts >= kMaxTransientLoadAttempts)
			{
				++SuppressedRetries_;
				return false; // silently -- the reason was logged when it was decided
			}
		}
		else
		{
			// The file CHANGED. That is a new fact, not a retry: a re-bake, a
			// download that finished, or a file someone fixed. Full budget.
			LoadFailures_.erase(FailIt);
		}
	}

	// The preamble: header, section table, elevation index, water index, basin
	// table, headwater table. DISJOINT regions, not a prefix -- encode_v2 puts
	// each plane's multi-megabyte data section immediately after its own index
	// -- which is why this is a call into voxel-core rather than a "read the
	// first 64 KB" here. ~62-67 KB of content in 2-5 requests.
	vxc::FinePreambleRequest Want;
	Want.wantFlow = false;   // see above: nothing in this module reads flow
	Want.wantWater = true;   // lakes.h / riverribbon.h decode water lazily
	Want.wantBasins = true;  // the lake registry; absent != empty, so fetch it
	// HEADS LOAD WHEREVER BASINS DO. Same argument, same cost class (a table of
	// a few dozen 8-byte rows, not a plane), and the same absent-vs-empty trap:
	// a tile whose heads were never fetched reports headsResident()==false, the
	// fluid's faucet gather reads that as "no baked heads in this box" and falls
	// to the rivernet graph fallback, whose rim false-heads were the "square of
	// hovering water" the first playtest saw. It is the default in
	// FinePreambleRequest; stated here so a future edit has to mean it.
	Want.wantHeads = true;
	vxc::FineTileBytes Held;
	vxc::FineError PreambleErr = vxc::FineError::kNone;
	vxc::FineHeaderFacts PreambleFacts;
	if (!vxc::readFineTilePreamble(Source, Source.fileSize(), Want, Held, &PreambleErr, &PreambleFacts))
	{
		++CorruptLoads_;
		// A preamble read that failed on the SOURCE (kFileUnreadable: locked,
		// being written, shrank) is the one failure here that another attempt
		// can fix. A malformed section table is not, and asking again for it is
		// the spin this whole path now refuses to perform.
		return RecordLoadFailure_Locked(Tile, Path, FileSizeNow, WriteTimeNow, TEXT("preamble"),
		                                DescribeRejection(PreambleErr, PreambleFacts),
		                                vxc::fineErrorIsTransient(PreambleErr));
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
		// "failed structural validation (bad-header)" used to be the whole of
		// this message, and it was the same words whether the file was
		// truncated or this binary was three weeks behind the bake. It is now
		// vxc::fineDescribeRejection's sentence, which names the file's
		// bake_ver AND this build's ceiling; and retryWorthwhile() decides
		// whether asking again could ever help, which for a structural refusal
		// it cannot.
		return RecordLoadFailure_Locked(Tile, Path, FileSizeNow, WriteTimeNow, TEXT("validation"),
		                                DescribeRejection(Validated.error, Validated.facts),
		                                Validated.retryWorthwhile());
	}
	if (Validated.verdict == vxc::FineTileVerdict::kIdentityMismatch)
	{
		++IdentityMismatches_;
		return RecordLoadFailure_Locked(
			Tile, Path, FileSizeNow, WriteTimeNow, TEXT("identity"),
			FString::Printf(
				TEXT("the file parsed cleanly but carries a DIFFERENT tile's identity than the one requested ")
				TEXT("(wanted seed=%llu x=%d y=%d). This is a cache-key/provider mismatch, the same class of ")
				TEXT("refusal EditLog::checkProvider() already models for edit-log replay -- check ")
				TEXT("-VoxelFineTileProviderId and the seed before suspecting the tile."),
				(unsigned long long)Seed_, Tile.x, Tile.y),
			/*bTransient=*/false);
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
		// GENUINELY TRANSIENT, and one of the two cases that justify keeping a
		// retry at all: the preamble read succeeded, so the file was whole a
		// moment ago and something rewrote it underneath us. The next attempt
		// sees the new file, whose size or timestamp differs, and starts over.
		return RecordLoadFailure_Locked(
			Tile, Path, FileSizeNow, WriteTimeNow, TEXT("elev-fetch"),
			TEXT("the elevation payload could not be read: the file shrank or changed between the preamble ")
			TEXT("read and the payload read. Transient -- a re-bake in progress looks exactly like this."),
			/*bTransient=*/true);
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
		return RecordLoadFailure_Locked(
			Tile, Path, FileSizeNow, WriteTimeNow, TEXT("water-fetch"),
			TEXT("the water payload could not be read: the file shrank or changed between the preamble read ")
			TEXT("and the payload read. Transient -- see elev-fetch."),
			/*bTransient=*/true);
	}

	// THE BATHYMETRY PAIR (bake_ver 27), fetched here for exactly the reason the
	// water plane above is: the consumer decodes it long after this function
	// returns, and an unfetched block answers kBlockNotResident with no way to go
	// and get the bytes. The consumer is the camera-centred bathymetry field
	// (VoxelBathyField.cpp), which reads a rect of it every time the window
	// recentres, off the game thread.
	//
	// The preamble already brought both INDICES down -- FinePreambleRequest's
	// wantBathy defaults to true and readFineTilePreamble honours it -- so
	// without these two calls a tile would look like it had bathymetry
	// (bathyDepthIndexResident() true) and answer every actual read
	// kBlockNotResident. That is the failure mode the flag/section agreement test
	// exists to make loud at the format level, arriving instead one layer up.
	//
	// UNCONDITIONAL, matching the water plane. The two planes are the same cost
	// class -- int16, block_log2 8, and mostly MODE_CONSTANT away from lakes, so
	// a dry tile pays nothing at all -- and gating them would mean a second
	// residency state for something the material reads on every water pixel.
	if (Validated.tile->hasBathy())
	{
		const vxc::FinePlane BathyPlanes[2] = { vxc::FinePlane::kBathyDepth,
		                                        vxc::FinePlane::kBathyShore };
		for (vxc::FinePlane Plane : BathyPlanes)
		{
			if (!vxc::fetchFineTileBlocks(Source, *Validated.tile, Plane,
			                              vxc::fineNonConstantBlocks(*Validated.tile, Plane)))
			{
				++CorruptLoads_;
				return RecordLoadFailure_Locked(
					Tile, Path, FileSizeNow, WriteTimeNow, TEXT("bathy-fetch"),
					TEXT("a bathymetry payload could not be read: the file shrank or changed between the ")
					TEXT("preamble read and the payload read. Transient -- see elev-fetch."),
					/*bTransient=*/true);
			}
		}
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
		return RecordLoadFailure_Locked(
			Tile, Path, FileSizeNow, WriteTimeNow, TEXT("stride"),
			TEXT("the file passed validation but FineTileSampler::loadTile rejected it -- almost certainly a ")
			TEXT("`size` stride mismatch against a tile already loaded in this run. The format allows `size` ")
			TEXT("to vary per tile; this sampler (like the wire spec's own single-grid-stride assumption) ")
			TEXT("does not."),
			/*bTransient=*/false);
	}
	// THE MIRROR, WRITE 1 OF 3. Kept literally beside the loadTile it mirrors
	// so the two cannot be edited apart -- the whole safety argument for the
	// lock-free read (see ResidentTiles_ in the header) is that this set says
	// exactly what FineTileSampler::findTile says. Placed here rather than
	// after the prewarm below so that the mirror and findTile agree at EVERY
	// point, including the few milliseconds the decode takes.
	NoteMirrorWriteThread_();
	ResidentTiles_.insert(TileHash(Tile));

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
		NoteMirrorWriteThread_();
		ResidentTiles_.erase(TileHash(Tile)); // THE MIRROR, WRITE 2 OF 3
		return RecordLoadFailure_Locked(
			Tile, Path, FileSizeNow, WriteTimeNow, TEXT("decode"),
			FString::Printf(
				TEXT("the file parsed but at least one block failed to DECODE (blockDecodeFailures=%llu) -- ")
				TEXT("unloaded again. A half-decoded tile is not servable: a worker query into a missing block ")
				TEXT("would decode on the read path, which the shared read lock cannot survive. The bytes are ")
				TEXT("bad; re-reading them decodes them the same way."),
				(unsigned long long)Sampler_.blockDecodeFailures.load(std::memory_order_relaxed)),
			/*bTransient=*/false);
	}
	const double DecodeMs = (FPlatformTime::Seconds() - DecodeT0) * 1000.0;

	DecodedBytes_ += TileDecodedBytes;
	++TilesLoaded_;
	KnownMissing_.erase(TileHash(Tile));
	// It loaded, so whatever we remembered about failing to load it is wrong
	// (the transient-with-attempts-left path is the only way to get here with
	// an entry still present). Dropping it also frees the stored explanation.
	LoadFailures_.erase(TileHash(Tile));

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

bool FVoxelFineTileStreamer::IsSettledFailure_Locked(vxc::TileCoord Tile) const
{
	const auto It = LoadFailures_.find(TileHash(Tile));
	return It != LoadFailures_.end() &&
	       (It->second.bPermanent || It->second.Attempts >= kMaxTransientLoadAttempts);
}

bool FVoxelFineTileStreamer::RecordLoadFailure_Locked(vxc::TileCoord Tile, const FString& Path,
                                                      uint64 FileSize, int64 WriteTime,
                                                      const TCHAR* ReasonTag, const FString& Why,
                                                      bool bTransient)
{
	FTileLoadFailure& Entry = LoadFailures_[TileHash(Tile)];
	if (Entry.FileSize != FileSize || Entry.WriteTime != WriteTime)
	{
		Entry.Attempts = 0; // a different file version: its own budget
	}
	Entry.FileSize = FileSize;
	Entry.WriteTime = WriteTime;
	Entry.bPermanent = !bTransient;
	Entry.Why = Why;
	++Entry.Attempts;

	const bool bGivingUp = Entry.bPermanent || Entry.Attempts >= kMaxTransientLoadAttempts;
	if (bGivingUp)
	{
		++GivenUpTiles_;
		// ERROR, not Warning, and ONCE. This is the end of the road for this
		// tile: its area stays non-resident, the gate keeps refusing to
		// voxelize there, and nothing further will happen without a human. The
		// old code said this at Warning and then said it again every frame,
		// which is the same information rate as saying nothing while being
		// considerably harder to read past.
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("Fine tile (%d,%d) REFUSED and GIVEN UP after %u attempt(s) [%s]: %s")
		       TEXT(" File: %s (%llu bytes). NOT RETRIED until that file's size or timestamp changes -- ")
		       TEXT("re-reading identical bytes produces an identical verdict, so a retry here can only spin. ")
		       TEXT("This tile stays NON-RESIDENT: the residency gate will keep refusing to voxelize its area, ")
		       TEXT("which is block-until-ready working, not a fallback. Nothing here is answered with sea level ")
		       TEXT("unless a gate leak is also reported."),
		       Tile.x, Tile.y, Entry.Attempts, ReasonTag, *Why, *Path, (unsigned long long)FileSize);
	}
	else
	{
		// Still transient and still in budget. Warning, because it may well fix
		// itself on the next pass -- and bounded, because if it does not, the
		// branch above ends it.
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("Fine tile (%d,%d) load failed, attempt %u of %u [%s]: %s Will retry."),
		       Tile.x, Tile.y, Entry.Attempts, uint32(kMaxTransientLoadAttempts), ReasonTag, *Why);
	}
	return false;
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

vxc::BathyRectStats FVoxelFineTileStreamer::ReadBathyRect(int64 Px0, int64 Py0, int64 Px1, int64 Py1,
                                                          int16_t* DepthOut, int16_t* ShoreOut,
                                                          int64 RowStrideElems) const
{
	// SHARED, not exclusive -- see the header. sampleBathyRect decodes into the
	// caller's buffers and touches no state inside Sampler_, so this is a read in
	// the same sense IsFootprintResident is, just a much longer one.
	VoxelFineLock::FLockScope Lock(Lock_, SLT_ReadOnly, VoxelFineLock::ESite::BathyShared);
	return vxc::sampleBathyRect(Sampler_, Px0, Py0, Px1, Py1, DepthOut, ShoreOut, RowStrideElems);
}

// ---------------------------------------------------------------------------
// THE LOCK FAST PATH (-VoxelFineLockFast) AND ITS SAFETY ARGUMENT
//
// THE MEASUREMENT THIS EXISTS FOR. GetSurfaceHeightUU is the most expensive
// per-chunk call in the pipeline -- four per chunk at two hot sites -- and each
// one calls RequestFootprint, which took Lock_ EXCLUSIVELY on every call
// whether or not there was anything to load. Meanwhile the meshing pool reads
// terrain through FVoxelFineTileSamplerProxy::elevationMm, which takes the SAME
// lock SHARED once per sampled pixel from up to 36 threads. So each of those
// prefetches was a full barrier across the worker pool: it had to wait for
// every in-flight reader to drain, and every reader arriving behind it queued.
// At the 14,099 chunks/s peak that is 56,000-112,000 barriers a second.
//
// That is the mechanism the 12-20x degradation under load points at -- 9-15 us
// per call uncontended, up to 183 us contended, the degradation arriving
// exactly when throughput rises, which is what a serialization point does and
// is not what work does.
//
// THE DISPROOF CONDITION, REGISTERED BEFORE THE MEASUREMENT:
//   If -VoxelFineLockMeter=2 shows the EXCLUSIVE sites' WAIT time is a small
//   share of their total (under ~1 us per TIMED acquire at peak throughput),
//   then the lock
//   was never the term, the 183 us is vxc::Amplifier::column doing work, and
//   this change must be reverted rather than tuned. `excl wait us/acq` in the
//   probe line is that number, and it can come out either way -- which is the
//   only reason it is worth printing.
//
// WHAT THE FIX DOES. Nothing about the exclusive path changes. What changes is
// that the exclusive path is only ENTERED when a tile actually has to be
// loaded. In the steady state every covered tile is already resident, so
// today's exclusive section was a pure read that happened to be holding a
// writer lock.
//
//   mode 0  today: SLT_Write, unconditionally. Byte-identical.
//   mode 1  check residency under SLT_ReadOnly first; escalate only on a miss.
//   mode 2  on the GAME THREAD, check the ResidentTiles_ mirror with NO lock at
//           all; escalate only on a miss. (See ResidentTiles_ in the header for
//           why a lock-free read is sound: the single writer and the reader are
//           the same thread.)
//   mode 3  mode 2 plus an audit -- every mirror answer is cross-checked
//           against FineTileSampler::findTile under the shared lock, and the
//           SAMPLER'S answer is the one returned. Disagreement is an Error and
//           a counter, never a silent correction.
//
// WHY EACH READ IS SAFE, stated per mode because a torn or stale read here does
// not crash, it silently produces terrain no other client reproduces:
//
//   mode 1's read is the SAME read IsFootprintResident already performs, under
//   the same shared lock, which is sound by the header's threading rule 2: the
//   tile map is only ever mutated under the lock held exclusively, so no rehash
//   can be in flight while a shared holder is inside findTile. Nothing here can
//   observe a half-inserted tile.
//
//   Returning true without loading is exactly what mode 0 does in the same
//   case: EnsureTileResident_Locked's first statement is "if (findTile != null)
//   return true", and it mutates nothing on that path. So mode 1 changes the
//   lock mode of a section that was already a pure read, and nothing else.
//
//   Eviction cannot race the answer. Evictions take the lock exclusively, so
//   nothing can be unloaded while the shared check holds it. The window between
//   returning true and the caller voxelizing is not new -- mode 0 released the
//   lock before returning too. This changes the lock mode, not the lifetime of
//   the answer.
//
//   mode 2's read is lock-free and it is safe by thread identity rather than by
//   ordering: the mirror's only writers sit beside the three
//   loadTile/unloadTile calls, all under the exclusive lock AND all on the game
//   thread, and the lock-free read only happens inside IsInGameThread(). A
//   stale TRUE -- the dangerous direction -- is therefore impossible; a stale
//   FALSE falls through to mode 1's shared check and then to the exclusive
//   load, i.e. to mode 0's behaviour.
// ---------------------------------------------------------------------------

FVoxelFineTileStreamer::FTileRange FVoxelFineTileStreamer::CoveredTileRange(int64 WorldMmX0, int64 WorldMmY0,
                                                                           int64 WorldMmX1, int64 WorldMmY1)
{
	// THE SAME TWO voxel-core CALLS, IN THE SAME ORDER, THAT
	// vxc::tilesCoveringFootprint MAKES INTERNALLY -- fineReadPixelRect, which
	// is the single authority on the cavern read margin and the carrier
	// stencil, then tileCoordForPixel on the two corners. No dilation is
	// computed here and none may ever be: a second arithmetic for the read
	// reach is exactly how the gate and the GPU raster window came to disagree
	// once already. The constructor's SelfCheckTileRangeAgreesWithCoverage_
	// asserts the agreement rather than trusting this comment.
	const vxc::PixelRect Rect = vxc::fineReadPixelRect(WorldMmX0, WorldMmY0, WorldMmX1, WorldMmY1, kFineReadMarginMm);
	if (Rect.px1 < Rect.px0 || Rect.py1 < Rect.py0)
	{
		return FTileRange{}; // degenerate: bValid stays false, and false means REFUSE
	}
	const vxc::TileCoord Lo = vxc::tileCoordForPixel(Rect.px0, Rect.py0, int64(vxc::kFineTileSize));
	const vxc::TileCoord Hi = vxc::tileCoordForPixel(Rect.px1, Rect.py1, int64(vxc::kFineTileSize));
	FTileRange Out;
	Out.X0 = Lo.x;
	Out.Y0 = Lo.y;
	Out.X1 = Hi.x;
	Out.Y1 = Hi.y;
	Out.bValid = true;
	return Out;
}

void FVoxelFineTileStreamer::SelfCheckTileRangeAgreesWithCoverage_()
{
	// A GATE THAT CAN BE SEEN FAILING. To watch this go red, delete
	// kFineReadMarginMm from CoveredTileRange's fineReadPixelRect call (pass 0):
	// the third case below sits one millimetre further inside the tile than the
	// cavern read margin reaches, so WITH the margin it covers two tiles in X
	// and WITHOUT it one, and this refuses to start. That is the exact defect
	// class this check exists for -- an under-dilated gate does not fault, it
	// admits chunks whose cavern reads land in a non-resident tile and come
	// back as sea level.
	struct FCase
	{
		int64 X0, Y0, X1, Y1;
		const TCHAR* Why;
	};
	const int64 T = kTileFootprintMm;
	const FCase Cases[] = {
		{0, 0, 1, 1, TEXT("the origin, and the 1 mm point footprint GetSurfaceHeightUU passes")},
		{-1000000000, -1000000000, -999999999, -999999999,
		 TEXT("far negative -- floorDiv and truncation differ ONLY on this side, and the leaking capture ")
		 TEXT("spawned at negative X")},
		{T - kFineReadMarginMm - 1000, 0, T - kFineReadMarginMm - 999, 1,
		 TEXT("one millimetre further inside the +X tile boundary than the cavern read margin reaches, so ")
		 TEXT("this case covers two tiles in X WITH the margin and one WITHOUT it -- it is the case that ")
		 TEXT("goes red if the margin is ever dropped from CoveredTileRange")},
		{-1, -1, 1, 1, TEXT("straddling the tile corner at the world origin")},
		{0, 0, T * 3, T * 2, TEXT("a multi-tile rect: the COUNT, not just the corners")},
		{-T * 2, T, -T, T * 3, TEXT("a multi-tile rect in the -X/+Y quadrant")},
	};

	for (const FCase& Case : Cases)
	{
		const std::vector<vxc::TileCoord> Truth = CoveredTiles(Case.X0, Case.Y0, Case.X1, Case.Y1);
		const FTileRange Range = CoveredTileRange(Case.X0, Case.Y0, Case.X1, Case.Y1);

		bool bAgree = (Range.bValid == !Truth.empty()) && (Range.Count() == int64(Truth.size()));
		if (bAgree)
		{
			for (const vxc::TileCoord& Tile : Truth)
			{
				if (Tile.x < Range.X0 || Tile.x > Range.X1 || Tile.y < Range.Y0 || Tile.y > Range.Y1)
				{
					bAgree = false;
					break;
				}
			}
		}
		if (!bAgree)
		{
			UE_LOG(LogVoxelEarth, Fatal,
			       TEXT("FINE LOCK FAST PATH IS UNSOUND: the allocation-free tile range disagrees with ")
			       TEXT("vxc::tilesCoveringFootprint on the case '%s'. Rect mm [%lld,%lld)-[%lld,%lld): voxel-core ")
			       TEXT("covers %d tile(s), the range covers %lld (x %d..%d, y %d..%d, valid=%d). The fast path ")
			       TEXT("would gate on a DIFFERENT footprint than the gate does, which admits chunks over ")
			       TEXT("non-resident tiles and produces terrain no other client reproduces. Refusing to start."),
			       Case.Why, (long long)Case.X0, (long long)Case.Y0, (long long)Case.X1, (long long)Case.Y1,
			       int32(Truth.size()), (long long)Range.Count(), Range.X0, Range.X1, Range.Y0, Range.Y1,
			       Range.bValid ? 1 : 0);
		}
	}
}

bool FVoxelFineTileStreamer::RangeResidentInMirror_(const FTileRange& Range) const
{
	// NO LOCK, GAME THREAD ONLY. See ResidentTiles_ in the header: the single
	// writer and this reader are the same thread, so there is nothing to order
	// and nothing to be stale about. Callers must have tested IsInGameThread().
	if (!Range.bValid)
	{
		return false;
	}
	for (int32 Ty = Range.Y0; Ty <= Range.Y1; ++Ty)
	{
		for (int32 Tx = Range.X0; Tx <= Range.X1; ++Tx)
		{
			if (ResidentTiles_.find(TileHash(vxc::TileCoord{Tx, Ty})) == ResidentTiles_.end())
			{
				return false;
			}
		}
	}
	return true;
}

bool FVoxelFineTileStreamer::RangeResidentInSampler_Locked(const FTileRange& Range) const
{
	if (!Range.bValid)
	{
		return false;
	}
	for (int32 Ty = Range.Y0; Ty <= Range.Y1; ++Ty)
	{
		for (int32 Tx = Range.X0; Tx <= Range.X1; ++Tx)
		{
			if (Sampler_.findTile(Tx, Ty) == nullptr)
			{
				return false;
			}
		}
	}
	return true;
}

bool FVoxelFineTileStreamer::AuditMirrorAgainstSampler_Locked(const FTileRange& Range,
                                                              bool bMirrorSaid) const
{
	// THE SAMPLER IS THE TRUTH AND IT IS WHAT IS RETURNED. The audit never lets
	// the mirror decide, so a mode-3 run is safe even if the mirror is wrong --
	// which is the only kind of audit worth running on a path whose failure mode
	// is silent wrong terrain rather than a fault.
	const bool bTruth = RangeResidentInSampler_Locked(Range);
	MirrorAudits_.fetch_add(1, std::memory_order_relaxed);
	if (bMirrorSaid != bTruth)
	{
		const uint64 Count = MirrorMismatches_.fetch_add(1, std::memory_order_relaxed) + 1;
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("FINE LOCK MIRROR DRIFT: the lock-free residency mirror said %s for tile range x %d..%d ")
		       TEXT("y %d..%d and FineTileSampler says %s. Mismatches=%llu of %llu audited. The mirror is ")
		       TEXT("maintained at the three loadTile/unloadTile sites; a drift means one of them was edited ")
		       TEXT("apart from its tile mutation. -VoxelFineLockFast=2 would have TRUSTED this answer, so ")
		       TEXT("treat any nonzero count as a correctness fault, not a tuning observation."),
		       bMirrorSaid ? TEXT("RESIDENT") : TEXT("NOT resident"), Range.X0, Range.X1, Range.Y0, Range.Y1,
		       bTruth ? TEXT("RESIDENT") : TEXT("NOT resident"), (unsigned long long)Count,
		       (unsigned long long)MirrorAudits_.load(std::memory_order_relaxed));
	}
	return bTruth;
}

bool FVoxelFineTileStreamer::IsFootprintResident(int64 WorldMmX0, int64 WorldMmY0, int64 WorldMmX1,
                                                 int64 WorldMmY1) const
{
	// THE ALLOCATION IS THE POINT OF THE FAST PATH HERE, not the lock: this is
	// already a shared read. It is called once per admission CANDIDATE by the
	// recompute sweep, which is the largest single game-thread item in the perf
	// log (300-555 ms/window), and every call heap-allocates a std::vector to
	// enumerate what is almost always ONE tile. The range covers the same tiles
	// -- the constructor checks that against voxel-core -- with no allocation.
	const int32 FastHere = VoxelFineLock::FastMode();
	if (FastHere > 0)
	{
		const FTileRange Range = CoveredTileRange(WorldMmX0, WorldMmY0, WorldMmX1, WorldMmY1);
		if (!Range.bValid)
		{
			return false; // "covers nothing" must never read as "all resident"
		}
		// Mode 2+: the game thread does not need the lock at all. This is the
		// one place a FALSE answer from the mirror is final rather than a
		// fall-through -- and a false FALSE is impossible for the same
		// thread-identity reason a false TRUE is (see ResidentTiles_ in the
		// header), so the admission gate becomes neither more permissive nor
		// more conservative.
		if (FastHere >= 2 && IsInGameThread())
		{
			const bool bMirror = RangeResidentInMirror_(Range);
			if (FastHere < 3)
			{
				return bMirror;
			}
			VoxelFineLock::FLockScope AuditLock(Lock_, SLT_ReadOnly, VoxelFineLock::ESite::FootprintShared);
			return AuditMirrorAgainstSampler_Locked(Range, bMirror);
		}
		VoxelFineLock::FLockScope FastLock(Lock_, SLT_ReadOnly, VoxelFineLock::ESite::FootprintShared);
		return RangeResidentInSampler_Locked(Range);
	}

	const std::vector<vxc::TileCoord> Tiles = CoveredTiles(WorldMmX0, WorldMmY0, WorldMmX1, WorldMmY1);
	if (Tiles.empty())
	{
		// vxc::tilesCoveringFootprint declines only on degenerate geometry, which
		// this call site cannot produce (compile-time stride and pitch). Refuse
		// rather than admit: "covers nothing" must never read as "all resident".
		return false;
	}

	VoxelFineLock::FLockScope Lock(Lock_, SLT_ReadOnly, VoxelFineLock::ESite::FootprintShared);
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
	// TRAFFIC BEFORE TIMING, and unconditionally: one increment per CALL (not
	// per pixel) on the game thread. Without it the fast path's share is
	// unknowable, and "the fast path never ran" and "the fast path never caught
	// anything" would read identically as an absent line.
	ReqCalls_.fetch_add(1, std::memory_order_relaxed);

	const int32 Fast = VoxelFineLock::FastMode();
	if (Fast > 0)
	{
		const FTileRange Range = CoveredTileRange(WorldMmX0, WorldMmY0, WorldMmX1, WorldMmY1);
		if (!Range.bValid)
		{
			return false; // see IsFootprintResident: decline, never admit
		}
		const bool bUseMirror = (Fast >= 2) && IsInGameThread();
		if (bUseMirror && Fast < 3)
		{
			// NO LOCK ACQUIRED AT ALL. This removes the term rather than
			// reducing the constant: in the steady state every tile is already
			// resident, so the exclusive barrier this call used to raise across
			// the worker pool 4-8 times per chunk simply does not happen.
			if (RangeResidentInMirror_(Range))
			{
				ReqFastFree_.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
		}
		else
		{
			const bool bMirrorSaid = bUseMirror ? RangeResidentInMirror_(Range) : false;
			VoxelFineLock::FLockScope FastLock(Lock_, SLT_ReadOnly, VoxelFineLock::ESite::ReqShared);
			const bool bResident = (bUseMirror && Fast >= 3)
			                           ? AuditMirrorAgainstSampler_Locked(Range, bMirrorSaid)
			                           : RangeResidentInSampler_Locked(Range);
			if (bResident)
			{
				ReqFastShared_.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
		}
		// Fall through: something really is missing, so the exclusive load path
		// below is the RIGHT place to be, and it is unchanged.
	}

	const std::vector<vxc::TileCoord> Tiles = CoveredTiles(WorldMmX0, WorldMmY0, WorldMmX1, WorldMmY1);
	if (Tiles.empty())
	{
		return false; // see IsFootprintResident: decline, never admit
	}

	ReqEscalated_.fetch_add(1, std::memory_order_relaxed);
	VoxelFineLock::FLockScope Lock(Lock_, SLT_Write, VoxelFineLock::ESite::ReqExcl);
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
		VoxelFineLock::FLockScope Lock(Lock_, SLT_Write, VoxelFineLock::ESite::ColdGameExcl);
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
		//
		// IsSettledFailure_Locked is the same courtesy for a tile that IS on
		// disk and was refused, and it is needed for the same reason: this
		// branch is on the funnel every one of the ~100 unguarded consumers
		// crosses, and the audit measured 18.7 MILLION queries into one
		// non-resident tile in a single run. EnsureTileResident_Locked would
		// now answer each of those cheaply -- an open and a stat rather than a
		// 60 KB read -- but 18.7 million opens is still not a thing to do to
		// find out something already known.
		//
		// The cost is that this path does not notice a file that changed under
		// us. That is deliberate and it is not a loss: TickResidencyAndEviction
		// re-stats every ring tile every tick and clears the memo the moment
		// the file's size or timestamp moves, so a re-bake is still picked up
		// within a tick -- by the loop whose job that is, not by a
		// character-movement collision probe.
		if (KnownMissing_.find(TileHash(Tile)) == KnownMissing_.end() && !IsSettledFailure_Locked(Tile))
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
	VoxelFineLock::FLockScope Lock(Lock_, SLT_Write, VoxelFineLock::ESite::ColdWorkerExcl);
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

	// IF WE ALREADY KNOW WHY, SAY SO HERE. A gate leak is what a person
	// actually reads -- it is the fatal line, and the one that explains the
	// black screen -- and until now it could only say "not resident and could
	// not be loaded", which is true of an absent tile and of a refused one
	// alike. Those need opposite responses: wait for the bake, versus rebuild
	// the binary. The refusal was diagnosed the first time it happened; this
	// carries that diagnosis to where it will be seen.
	// AND IF THE TILE IS SOMEWHERE ELSE, SAY WHERE. The refusal note below
	// explains a tile that was READ and rejected; this explains the far more
	// common case that made this gate fire three times on 2026-08-18 -- a tile
	// that exists, is fine, and is sitting under a different provider id because
	// the bake fingerprint moved and nothing told this process.
	//
	// COMPUTED ONLY WHEN IT WILL ACTUALLY BE PRINTED, which is the whole reason
	// this is a lambda and not a plain local. DiagnoseNamespace lists a
	// directory; the non-fatal path reaches here on EVERY leaking query, and the
	// audit that motivated this gate measured 18.7 MILLION leaks into one tile
	// in a single run. Computing it eagerly would put a directory scan on that
	// path -- turning a diagnostic into a hang, which is precisely the mistake
	// the surrounding code refuses to make about tile loads.
	const auto NamespaceNote = [this, Tile]() -> FString { return DiagnoseNamespace(Tile); };

	// HOW FAR IS THIS LEAK FROM THE PLAYER? The one question that separates the
	// two causes, and until now the log could not be asked it.
	//
	// MEASURED 2026-08-23, the owner's PIE session. 10,814 leaks, into exactly
	// four tiles -- (0,0), (0,-1), (-1,0), (-1,-1) -- which are the four that
	// meet at the WORLD ORIGIN. The streaming anchor was in tile (-4,-4) and
	// never left it (it moved 1.2 km all session; a tile is 15.36 km). So the
	// leaks were 4 tiles, ~61 km, away from any ground the player could see.
	// Residency was correct the whole time. The actual cause was
	// AVoxelWaterSheetActor's FIRST lake-sheet gather running at CamUU (0,0) --
	// before the pawn had a position -- and reading 10,814 elevations out of
	// ground nobody has ever baked (only 15 fine tiles exist, all in the -3..-15
	// band). The line this function printed said "move the camera to a baked
	// tile", which was the wrong advice about the wrong place, and the session
	// was filed as a streaming regression on the strength of it.
	//
	// HOW TO READ THE DISTANCE (Chebyshev, in coarse tiles):
	//   0 or 1  -- the anchor is standing on, or one dilated footprint away
	//              from, unbaked ground. A genuine coverage gap: bake it, or
	//              raise -VoxelFineTileRingRadius so the neighbour arrives
	//              before the player does.
	//   >1      -- SOME CALLER QUERIED TERRAIN THE PLAYER IS NOWHERE NEAR. The
	//              bake is not the problem, the ring is not the problem, and no
	//              radius would have prevented it. Find the caller.
	//   never   -- TickResidencyAndEviction has not run at all. That is its own
	//              bug and it is not a coverage question.
	//
	// A LAMBDA, for the same reason NamespaceNote above is one: the non-fatal
	// path reaches here on EVERY leaking query and the audit that motivated this
	// gate measured 18.7 million of them in one run. This note is only two
	// subtractions, but it is also an FString::Printf, and 18.7 million string
	// allocations on the funnel is the same mistake in a smaller size.
	const auto AnchorNote = [this, Tile]() -> FString
	{
		if (LastRingCentre_.x == INT32_MIN && LastRingCentre_.y == INT32_MIN)
		{
			return TEXT(" The residency tick has NEVER RUN (no ring centre), so nothing has been prefetched for any ")
			       TEXT("position -- this is a missing TickResidencyAndEviction call, not a coverage gap.");
		}
		const int64 DistX = FMath::Abs(int64(Tile.x) - int64(LastRingCentre_.x));
		const int64 DistY = FMath::Abs(int64(Tile.y) - int64(LastRingCentre_.y));
		const int64 Dist = FMath::Max(DistX, DistY);
		return FString::Printf(
			TEXT(" The streaming anchor is in tile (%d,%d), so this query is %lld tile(s) (~%lld km) away from the ")
			TEXT("player. %s"),
			LastRingCentre_.x, LastRingCentre_.y, (long long)Dist, (long long)(Dist * kTileFootprintMm / 1000000),
			Dist > 1
				? TEXT("THAT IS TOO FAR TO BE A COVERAGE GAP: some caller is sampling terrain the player is nowhere ")
				  TEXT("near, and no ring radius or bake would prevent it. Find the caller before baking anything.")
				: TEXT("That is adjacent to the player, so it IS a coverage question: bake the tile, or raise ")
				  TEXT("-VoxelFineTileRingRadius so the neighbour is resident before the player reaches it."));
	};

	FString RefusalNote;
	if (auto FailIt = LoadFailures_.find(TileHash(Tile)); FailIt != LoadFailures_.end())
	{
		RefusalNote = FString::Printf(
			TEXT(" THIS TILE IS ON DISK AND WAS REFUSED (%s after %u attempt(s)), which is why it is not ")
			TEXT("resident -- it is NOT an absent tile waiting on the bake: %s"),
			FailIt->second.bPermanent ? TEXT("permanently") : TEXT("transiently"), FailIt->second.Attempts,
			*FailIt->second.Why);
	}

	if (bLeakIsFatal_)
	{
		UE_LOG(LogVoxelEarth, Fatal,
		       TEXT("FINE TIER GATE LEAK (fatal, unattended run): elevation query at fine pixel (%lld,%lld) needs tile ")
		       TEXT("(%d,%d), which is not resident and could not be loaded from %s. Answering it would mean sea level, ")
		       TEXT("and on the fine tier sea level is not a fallback -- it is terrain no other client computes, so this ")
		       TEXT("run's output would not be reproducible. Stopping instead of producing an artifact that looks fine. ")
		       TEXT("Leaks so far=%llu, blocking loads=%llu, resident=%llu tile(s), absentOnDisk=%llu, corrupt=%llu, ")
		       TEXT("identityMismatch=%llu, refusedTiles=%llu.%s%s%s Pass -VoxelFineTileGateFatal=0 to downgrade ")
		       TEXT("this to an Error."),
		       (long long)px, (long long)py, Tile.x, Tile.y, *LocalPathFor(Tile), (unsigned long long)LeakCount,
		       (unsigned long long)BlockingLoads_.load(std::memory_order_relaxed), (unsigned long long)Sampler_.tileCount(),
		       (unsigned long long)MissingFileLoads_, (unsigned long long)CorruptLoads_,
		       (unsigned long long)IdentityMismatches_, (unsigned long long)GivenUpTiles_, *AnchorNote(),
		       *RefusalNote, *NamespaceNote());
		// UE_LOG(Fatal) does not return. The sea-level answer below is
		// unreachable here and exists only for the non-fatal path.
	}

	if (bFirstForThisTile)
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("FINE TIER GATE LEAK: elevation query at fine pixel (%lld,%lld) landed in NON-RESIDENT tile (%d,%d) ")
		       TEXT("(%s) and is being answered with SEA LEVEL. That is a desync, not a fallback: this client's terrain, ")
		       TEXT("collision and edits here will not match a client that has the tile. Logged once per tile; total ")
		       TEXT("leaks=%llu. Thread=%s. Unattended runs stop on this instead (SetLeakIsFatal).%s%s%s"),
		       (long long)px, (long long)py, Tile.x, Tile.y, *LocalPathFor(Tile), (unsigned long long)LeakCount,
		       IsInGameThread() ? TEXT("game") : TEXT("worker"), *AnchorNote(), *RefusalNote, *NamespaceNote());
	}

	// The sampler's own missingTileQueries bump happens inside this call, which
	// keeps MaybeLogCounters' after-the-fact check working unchanged.
	return Sampler_.elevationMm(px, py);
}

void FVoxelFineTileStreamer::TickResidencyAndEviction(vxc::TileCoord PlayerCoarseTile)
{
	// SCOPED, so the probe's own report is NOT inside the exclusive section it
	// is measuring. An instrument that appears in its own hold time reads a
	// number nobody can act on, and this one prints an FString::Printf with a
	// dozen conversions.
	{
		VoxelFineLock::FLockScope Lock(Lock_, SLT_Write, VoxelFineLock::ESite::TickExcl);

		if (!(PlayerCoarseTile == LastRingCentre_))
		{
			// The ring moved: everything we believed absent is worth one more look
			// (the bake frontier advances, and this is the only thing that retries).
			//
			// LoadFailures_ is deliberately NOT cleared with it. Absence is a fact
			// about the world and the frontier moves; a tile that was read and
			// refused is a fact about those bytes and this binary, and walking the
			// player around changes neither. Clearing it here would restore the
			// per-frame re-read this change exists to stop -- the memo would be
			// wiped on the same tick it was written, every tick the anchor moved.
			// What DOES earn a fresh attempt is the file itself changing; see
			// EnsureTileResident_Locked.
			KnownMissing_.clear();
			// Counted BEFORE the assignment and only when a centre already existed,
			// so ringMoves==0 reads as "the anchor has not crossed a tile boundary"
			// and not as "the tick has never run" -- the sentinel centre says the
			// second thing, and the two needed separating. See the header.
			if (!(LastRingCentre_.x == INT32_MIN && LastRingCentre_.y == INT32_MIN))
			{
				++RingCentreMoves_;
			}
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
			NoteMirrorWriteThread_();
			ResidentTiles_.erase(TileHash(It->second)); // THE MIRROR, WRITE 3 OF 3
			Budget_.remove(Key);
			KeyToTile_.erase(It);
		}
	} // Lock_ released here -- see the scope comment at the top of the function.

	MaybeLogLockProbe_();
}

// ---------------------------------------------------------------------------
// THE PROBE'S REPORT
//
// SELF-CONTAINED ON PURPOSE. It is emitted from here rather than from
// FVoxelWorldImpl::MaybeLogCounters so that every leg -- including one whose
// author never heard of this switch -- carries the traffic reading without a
// second file having to be edited. TickResidencyAndEviction is called once per
// RecomputeDesiredSet on the game thread, so the cadence is a tick and the
// interval below is what turns that into a window.
//
// THE THREE READINGS THIS LINE MUST KEEP APART, because this project has found
// twelve instruments that were green while sitting outside the path:
//
//   meter=off                     the switch is 0. Nothing was measured. The
//                                 line still prints, and still carries the
//                                 always-on RequestFootprint traffic, so an
//                                 unarmed leg is not a silent leg.
//   METER ARMED BUT NEVER ENTERED entered=0 with the probe on. No acquisition
//                                 of any kind reached the guard. That is an
//                                 instrument outside the path -- NOT an
//                                 uncontended lock -- and it is said in words.
//   contended=NO                  entered>0, acquisitions>0, and the wait
//                                 totals are ~0. The probe ran and the lock was
//                                 genuinely free. THIS IS THE DISPROOF of the
//                                 fast path: if it reads this way at peak
//                                 throughput, the 183 us/call is work and this
//                                 change should be reverted, not tuned.
//
// `win=` is appended at the END, per the old-leg-grep rule this project adopted
// after % of wall was found to have been 2.5x low on every leg ever run.
// ---------------------------------------------------------------------------
void FVoxelFineTileStreamer::MaybeLogLockProbe_()
{
	// Same interval the perf log uses, read from the same switch, so the probe
	// window and the `Voxel apply stages` window line up and can be divided by
	// one another. Legs pass -VoxelPerfLogInterval=2.
	static const double IntervalSec = []
	{
		float Value = 5.0f;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfLogInterval="), Value);
		return double(FMath::Clamp(Value, 0.25f, 600.0f));
	}();

	const double Now = FPlatformTime::Seconds();
	const double WindowSec = Now - LastProbeLogSeconds_;
	if (WindowSec < IntervalSec)
	{
		return;
	}
	LastProbeLogSeconds_ = Now;

	const int32 Meter = VoxelFineLock::MeterMode();
	const int32 Fast = VoxelFineLock::FastMode();

	// Deltas, not cumulative totals. A rate divided by the wrong denominator is
	// the single most common retracted finding in this project's history, so
	// the window's own numbers are what is printed and the window's own length
	// is printed beside them.
	static VoxelFineLock::FAggregate Prev;
	static uint64 PrevCalls = 0, PrevFree = 0, PrevShared = 0, PrevEscal = 0;
	const VoxelFineLock::FAggregate Now_ = VoxelFineLock::Collect();

	const uint64 Calls = ReqCalls_.load(std::memory_order_relaxed);
	const uint64 Free = ReqFastFree_.load(std::memory_order_relaxed);
	const uint64 Shared = ReqFastShared_.load(std::memory_order_relaxed);
	const uint64 Escal = ReqEscalated_.load(std::memory_order_relaxed);
	const uint64 DCalls = Calls - PrevCalls;
	const uint64 DFree = Free - PrevFree;
	const uint64 DShared = Shared - PrevShared;
	const uint64 DEscal = Escal - PrevEscal;
	PrevCalls = Calls;
	PrevFree = Free;
	PrevShared = Shared;
	PrevEscal = Escal;

	// The headline: what share of RequestFootprint calls no longer raise a
	// barrier across the worker pool. 0.0% with Calls>0 and Fast>0 is the fast
	// path running and catching nothing, which is the reading that says revert.
	const double AvoidedPct = DCalls > 0 ? 100.0 * double(DFree + DShared) / double(DCalls) : 0.0;

	if (Meter <= 0)
	{
		UE_LOG(LogVoxelPerf, Log,
		       TEXT("Fine lock: meter=off fast=%d | req calls=%llu lockFree=%llu shared=%llu excl=%llu ")
		       TEXT("(exclusive avoided %.1f%%) | cumulative calls=%llu excl=%llu | audit checked=%llu ")
		       TEXT("mismatch=%llu mirrorOffThread=%llu | -VoxelFineLockMeter=1 for acquisition counts, ")
		       TEXT("=2 for wait/hold win=%.2fs"),
		       Fast, (unsigned long long)DCalls, (unsigned long long)DFree, (unsigned long long)DShared,
		       (unsigned long long)DEscal, AvoidedPct, (unsigned long long)Calls, (unsigned long long)Escal,
		       (unsigned long long)MirrorAudits_.load(std::memory_order_relaxed),
		       (unsigned long long)MirrorMismatches_.load(std::memory_order_relaxed),
		       (unsigned long long)MirrorOffThreadWrites_.load(std::memory_order_relaxed), WindowSec);
		return;
	}

	VoxelFineLock::FAggregate D;
	for (int32 I = 0; I < VoxelFineLock::kSiteCount; ++I)
	{
		D.Acq[I] = Now_.Acq[I] - Prev.Acq[I];
		D.TimedAcq[I] = Now_.TimedAcq[I] - Prev.TimedAcq[I];
		D.WaitNs[I] = Now_.WaitNs[I] - Prev.WaitNs[I];
		D.HoldNs[I] = Now_.HoldNs[I] - Prev.HoldNs[I];
		// MAXIMA ARE CUMULATIVE AND ARE NEVER RESET, and the log says so with
		// worstSinceStart: a per-window max would have to be zeroed from the
		// game thread while workers are still writing it, which loses samples
		// exactly in the window that had the worst one.
		D.WaitMaxNs[I] = Now_.WaitMaxNs[I];
		D.HoldMaxNs[I] = Now_.HoldMaxNs[I];
	}
	D.Entered = Now_.Entered - Prev.Entered;
	Prev = Now_;

	if (Now_.Entered == 0)
	{
		UE_LOG(LogVoxelPerf, Warning,
		       TEXT("Fine lock: METER ARMED BUT NEVER ENTERED (mode=%d, entered=0 acquisitions of any kind, ")
		       TEXT("cumulative). This is an INSTRUMENT OUTSIDE THE PATH, not an uncontended lock: no ")
		       TEXT("FLockScope has been constructed at all, so either the fine tier is not being queried or ")
		       TEXT("the guard is no longer at the lock sites. Do not read this as zero contention. ")
		       TEXT("req calls=%llu win=%.2fs"),
		       Meter, (unsigned long long)DCalls, WindowSec);
		return;
	}

	const uint64 ExAcq = D.AcqIn(true);
	const uint64 ShAcq = D.AcqIn(false);
	// THE DENOMINATOR IS THE **TIMED** COUNT, NOT THE ACQUISITION COUNT. At
	// mode 2 the per-pixel reads are counted but not timed, so the two differ
	// by orders of magnitude on the shared side and dividing by the wrong one
	// would report a per-acquire wait that is nonsense in the reassuring
	// direction. Both are printed so the ratio is visible.
	const uint64 ExTimed = D.TimedAcqIn(true);
	const uint64 ShTimed = D.TimedAcqIn(false);
	const uint64 ExWait = D.WaitNsIn(true);
	const uint64 ExHold = D.HoldNsIn(true);
	const uint64 ShWait = D.WaitNsIn(false);
	const uint64 ShHold = D.HoldNsIn(false);
	const double ExWaitUsPer = ExTimed > 0 ? double(ExWait) / 1000.0 / double(ExTimed) : 0.0;
	const double ExHoldUsPer = ExTimed > 0 ? double(ExHold) / 1000.0 / double(ExTimed) : 0.0;
	const double ShWaitUsPer = ShTimed > 0 ? double(ShWait) / 1000.0 / double(ShTimed) : 0.0;

	// WAIT VERSUS WORK, which is the one question this whole instrument exists
	// to answer. Undefined rather than 0 when the timing half is not armed, so
	// a mode-1 leg cannot be quoted as "0% waiting".
	const uint64 TotalWait = ExWait + ShWait;
	const uint64 TotalHold = ExHold + ShHold;
	const double WaitShare = (TotalWait + TotalHold) > 0
	                             ? 100.0 * double(TotalWait) / double(TotalWait + TotalHold)
	                             : 0.0;

	const TCHAR* Verdict =
		(ExTimed + ShTimed) == 0
			? TEXT("timing NOT armed: pass -VoxelFineLockMeter=2 for wait/hold -- this line's ")
			  TEXT("wait/hold shares are UNMEASURED, not zero")
	    : (TotalWait * 100 < TotalHold)
	          ? TEXT("contended=NO: the probe ran and waiting is under 1% of time in the lock -- the cost ")
	            TEXT("here is WORK, and the fast path's premise is FALSIFIED")
	          : TEXT("contended=YES: waiting is a real share of time in the lock");

	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Fine lock: meter=%d fast=%d | req calls=%llu lockFree=%llu shared=%llu excl=%llu ")
	       TEXT("(exclusive avoided %.1f%%) | EXCL acq=%llu timed=%llu wait=%.1fms hold=%.1fms us/timed ")
	       TEXT("wait=%.2f hold=%.2f | SHARED acq=%llu timed=%llu wait=%.1fms hold=%.1fms us/timed wait=%.3f ")
	       TEXT("| waitShare=%.1f%% of %llu ns in-lock | worstSinceStart us: wait excl=%.1f shared=%.1f hold excl=%.1f ")
	       TEXT("| acq[reqExcl=%llu reqFast=%llu tick=%llu coldGame=%llu coldWorker=%llu elev=%llu climate=%llu ")
	       TEXT("isResident=%llu bathy=%llu diag=%llu] | entered=%llu | audit checked=%llu mismatch=%llu ")
	       TEXT("mirrorOffThread=%llu | %s ")
	       TEXT("win=%.2fs"),
	       Meter, Fast, (unsigned long long)DCalls, (unsigned long long)DFree, (unsigned long long)DShared,
	       (unsigned long long)DEscal, AvoidedPct,
	       (unsigned long long)ExAcq, (unsigned long long)ExTimed, double(ExWait) / 1.0e6,
	       double(ExHold) / 1.0e6, ExWaitUsPer, ExHoldUsPer,
	       (unsigned long long)ShAcq, (unsigned long long)ShTimed, double(ShWait) / 1.0e6,
	       double(ShHold) / 1.0e6, ShWaitUsPer,
	       WaitShare, (unsigned long long)(TotalWait + TotalHold),
	       double(D.MaxWaitNsIn(true)) / 1000.0, double(D.MaxWaitNsIn(false)) / 1000.0,
	       double(D.MaxHoldNsIn(true)) / 1000.0,
	       (unsigned long long)D.Acq[int32(VoxelFineLock::ESite::ReqExcl)],
	       (unsigned long long)D.Acq[int32(VoxelFineLock::ESite::ReqShared)],
	       (unsigned long long)D.Acq[int32(VoxelFineLock::ESite::TickExcl)],
	       (unsigned long long)D.Acq[int32(VoxelFineLock::ESite::ColdGameExcl)],
	       (unsigned long long)D.Acq[int32(VoxelFineLock::ESite::ColdWorkerExcl)],
	       (unsigned long long)D.Acq[int32(VoxelFineLock::ESite::ElevShared)],
	       (unsigned long long)D.Acq[int32(VoxelFineLock::ESite::ClimateShared)],
	       (unsigned long long)D.Acq[int32(VoxelFineLock::ESite::FootprintShared)],
	       (unsigned long long)D.Acq[int32(VoxelFineLock::ESite::BathyShared)],
	       (unsigned long long)D.Acq[int32(VoxelFineLock::ESite::DiagShared)],
	       (unsigned long long)D.Entered,
	       (unsigned long long)MirrorAudits_.load(std::memory_order_relaxed),
	       (unsigned long long)MirrorMismatches_.load(std::memory_order_relaxed),
	       (unsigned long long)MirrorOffThreadWrites_.load(std::memory_order_relaxed), Verdict, WindowSec);
}

uint64 FVoxelFineTileStreamer::ResidentBytes() const
{
	VoxelFineLock::FLockScope Lock(Lock_, SLT_ReadOnly, VoxelFineLock::ESite::DiagShared);
	return Budget_.residentBytes();
}

uint64 FVoxelFineTileStreamer::BudgetBytes() const
{
	VoxelFineLock::FLockScope Lock(Lock_, SLT_ReadOnly, VoxelFineLock::ESite::DiagShared);
	return Budget_.budgetBytes();
}

uint64 FVoxelFineTileStreamer::ResidentTileCount() const
{
	VoxelFineLock::FLockScope Lock(Lock_, SLT_ReadOnly, VoxelFineLock::ESite::DiagShared);
	return uint64(Sampler_.tileCount());
}

vxc::TileCoord FVoxelFineTileStreamer::RingCentreTile() const
{
	// Shared lock for consistency with the other diagnostics rather than out of
	// necessity: today the only writer is TickResidencyAndEviction on the game
	// thread and the only reader is the 5 s perf log, also on the game thread.
	// It costs one uncontended shared acquire every five seconds, and it means
	// this accessor does not become the exception the day something else reads
	// it. The sentinel (INT32_MIN, INT32_MIN) is returned as-is -- the caller
	// must be able to see "never ticked", see the header.
	VoxelFineLock::FLockScope Lock(Lock_, SLT_ReadOnly, VoxelFineLock::ESite::DiagShared);
	return LastRingCentre_;
}

uint64 FVoxelFineTileStreamer::RingCentreMovesSinceStart() const
{
	VoxelFineLock::FLockScope Lock(Lock_, SLT_ReadOnly, VoxelFineLock::ESite::DiagShared);
	return RingCentreMoves_;
}
