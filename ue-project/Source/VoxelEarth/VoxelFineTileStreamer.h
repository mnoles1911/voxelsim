#pragma once
// FVoxelFineTileStreamer -- residency/prefetch/eviction manager for the baked
// .vxtl v2 fine tier (docs/terrain-amplification-plan.md Phase 2, "Production:
// unbounded world, on-demand serving"; docs/vxtl-v2-format.md), AND the owner
// of the vxc::ITileSampler the world generates through when the fine tier is
// live.
//
// Plain C++, NOT UHT-parsed (no UCLASS/USTRUCT) -- like FVoxelWorldImpl in
// VoxelWorldSubsystem.cpp, this header is free to include voxelcore headers
// directly; voxel-core stays out of UHT-parsed headers by doctrine, and this
// is not one. Owned by FVoxelWorldImpl, constructed only when fine-tier
// streaming is explicitly enabled (-VoxelFineTileDir=<path>) so the feature is
// zero-cost and behavior-identical to today when off -- the same opt-in shape
// MakeTileSampler already uses for -VoxelTileDir.
//
// THE PRODUCT RULE THIS CLASS EXISTS TO ENFORCE ("block-until-ready", plan
// "Production" section): a footprint whose fine blocks -- PLUS the carrier's
// dilated control stencil, see vxc::kCarrierStencilLo/Hi, PLUS the cavern
// reach every chunk's generation samples out to, see kFineReadMarginMm below
// -- are not resident must not be voxelized. This is multiplayer: collision
// and edits must agree on every client, so a missing fine tile is a DESYNC,
// never a visual glitch to paper over with a procedural guess.
// IsFootprintResident() is that gate. Callers MUST check it before admitting a
// chunk whose footprint the fine tier covers, and MUST NOT admit the chunk if
// it returns false -- defer and let the caller's own per-tick rescan
// (RecomputeDesiredSet already re-evaluates every un-admitted candidate on
// anchor movement) retry later, once TickResidencyAndEviction has had a chance
// to load what was missing.
//
// The gate is not decoration: vxc::FineTileSampler::elevationMm answers a
// query into a non-resident tile with ELEVATION 0 (sea level) and a
// missingTileQueries bump, exactly as TileGridSampler does. Sea level IS the
// procedural fallback the product rule forbids, so the only thing standing
// between this feature and a silent desync is that no chunk is ever generated
// over a non-resident footprint. MissingTileQueriesSinceStart() is the
// after-the-fact check that the gate held; FVoxelWorldImpl::MaybeLogCounters
// logs it as an Error if it ever moves.
//
// --- WHY THE CHUNK-ADMISSION GATE WAS NOT ENOUGH -------------------------
//
// It was believed for some time that gating chunk ADMISSION
// (FVoxelWorldImpl::RecomputeDesiredSet) covered the rule, because that is the
// only path that VOXELIZES. It is not, and a run with a fine-tier root and no
// resolvable tile proved it with 18.7 MILLION sea-level answers while not a
// single chunk was admitted.
//
// The reason is structural, not a missing call site: FVoxelWorldImpl::Voxels is
// CONSTRUCTED OVER WorldSampler() (see FVoxelWorldImpl's ctor). Every consumer
// of the world -- not just meshing -- therefore reads the fine raster:
// character-movement collision, the agent Tier-1 region graph (a 1,875-region
// box rebuilt every 9.6 m of travel, ~4k solidity probes per region), the water
// CA's solidity callback at 10 Hz, GetSurfaceHeightUU for pawn spawn and agent
// placement, the SWE fixtures' 128x128 column surveys, the HUD's per-frame
// readout. None of those admit a chunk, so none of them ever consulted the
// gate, and there is no realistic prospect of finding and gating all of them:
// the audit found ~100 sites, and site 101 is one commit away.
//
// SO THE RULE IS ENFORCED AT THE FUNNEL INSTEAD, in
// FVoxelFineTileSamplerProxy::elevationMm -- the single choke point every one
// of those consumers passes through. A query whose tile is not resident does
// not get sea level:
//
//   * on the GAME THREAD it BLOCKS -- literally: it loads the tile
//     synchronously (EnsureTileResident_Locked) and then answers correctly.
//     This is legal precisely where RequestFootprint is legal, and it is where
//     essentially all of the leaking consumers above run. "Block until ready"
//     is implemented as blocking.
//   * when the tile CANNOT be made resident (absent on disk, corrupt, or the
//     caller is a worker that must not do I/O), there is no correct answer to
//     give, so the query is reported as a GATE LEAK and, under
//     SetLeakIsFatal(true), STOPS THE RUN. See the .cpp for the argument.
//
// The chunk-admission gate stays exactly where it is. It is still the right
// thing: it defers whole chunks cheaply and in bulk, before any work, rather
// than discovering misses one pixel at a time. The funnel check is the
// backstop that makes the RULE true rather than the meshing path's local
// convention.
//
// --- WIRED, AS OF THIS CHANGE -------------------------------------------
//
// WorldSampler() is the vxc::ITileSampler FVoxelWorldImpl::Voxels is
// constructed over when -VoxelFineTileDir is passed, so the amplifier's
// carrier evaluates docs/vxtl-v2-format.md §8's spline directly on the baked
// control lattice and amplifier.cpp's isFineTier(pixelSizeMm()) branch selects
// kFineDetailOctaves. That branch's safety argument (read it -- it is written
// next to the branch) requires pitch to be a property of the WORLD rather than
// of what one client has downloaded, which is why pixelSizeMm() here is the
// compile-time 1875 from the first frame, resident tiles or not, and why the
// residency gate rather than a coarse fallback is what covers the gap.
//
// --- THREADING: THE ONE THING THAT MAKES THIS SAFE ----------------------
//
// vxc::FineTileSampler is explicitly NOT safe for concurrent queries: it
// decodes blocks lazily, so an ordinary elevationMm() can insert into the
// block cache. Meshing worker jobs query the world's sampler from many
// threads at once (FVoxelWorldImpl::DispatchJobs), and the game thread loads
// and evicts tiles underneath them. Two rules make that sound, and BOTH are
// required:
//
//   1. A TILE IS FULLY DECODED AT LOAD, never lazily. EnsureTileResident
//      prewarms every block of a tile before publishing it, so after load a
//      resident tile's block map is complete and a query is a pure read
//      (vxc::FineTileSampler::blockFor takes the found-block path and mutates
//      nothing). This trades memory -- see kDecodedBytesPerTileNote in the
//      .cpp -- for the sampler's own documented "prewarm from one thread, then
//      it is TileGridSampler again" contract, applied at tile granularity
//      where it is checkable instead of per footprint where it is not.
//   2. THE TILE MAP ITSELF IS RWLOCKED. Loading and evicting mutate
//      FineTileSampler::tiles_, which rehashes; a concurrent find() during a
//      rehash is UB regardless of rule 1. Reads through WorldSampler() take
//      the lock shared, every mutation takes it exclusively.
//
// Rule 1 is what keeps rule 2's shared side genuinely shared: if a read could
// still decode, the read lock would have to become exclusive and the worker
// pool would serialise on terrain sampling.
//
// SYNCHRONOUS LOADING, NOT ASYNC -- BY SCOPE, NOT OVERSIGHT. Today's only
// transport is a local directory mirroring terrain-service's cache layout
// (cache.py), read with vxc::readFileBytes on the calling thread, exactly
// like MakeTileSampler's existing -VoxelTileDir path. A single fine tile is
// ~22-28 MB compressed and decodes to ~128 MB, so a cold ring shift that has
// to load several at once WILL block whatever thread calls
// TickResidencyAndEviction for the duration of those reads and decodes.
// Moving this to a background loader is future work -- see the .cpp's top
// comment for what that would need.

#include "CoreMinimal.h" // FString/uint64/int64 -- see VoxelCoords.h for the same self-containment reasoning
#include "Misc/ScopeRWLock.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "voxelcore/tilestore.h"
#include "voxelcore/tilestreaming.h"

class FVoxelFineTileStreamer;

// The vxc::ITileSampler the world reads terrain through when the fine tier is
// live. Every query is a shared-locked pure read of an already fully decoded
// tile (see the threading note above); the streamer takes the same lock
// exclusively around load/evict.
//
// A separate object rather than making FVoxelFineTileStreamer itself an
// ITileSampler: the streamer's public surface is a residency POLICY that the
// game thread drives, and the sampler's is a hot, many-threaded query path.
// Keeping them distinct makes "which of these may a worker touch" a type
// question instead of a comment.
class FVoxelFineTileSamplerProxy final : public vxc::ITileSampler
{
public:
	explicit FVoxelFineTileSamplerProxy(FVoxelFineTileStreamer& InOwner) : Owner(InOwner) {}

	// 1875, unconditionally and from the first frame -- see the class comment
	// above on amplifier.cpp's tier branch. NOT derived from what is resident.
	int32_t pixelSizeMm() const override { return vxc::tilePixelSizeMm(vxc::kFineTileScale); }
	int32_t elevationMm(int64_t px, int64_t py) override;
	vxc::ClimateSample climate(int64_t px, int64_t py) override;

private:
	FVoxelFineTileStreamer& Owner;
};

class FVoxelFineTileStreamer
{
public:
	// Plan: "fine ring radius of 1 tile (+-15.4 km)". The DEFAULT is 0 -- the
	// tile under the player and nothing else -- because rule 1 above decodes a
	// whole tile at load: radius 1 is nine tiles, ~1.2 GB of decoded lattice
	// and (synchronously, on the game thread) nine tile decodes. Radius is a
	// command-line knob (-VoxelFineTileRingRadius=), and RequestFootprint
	// still pulls in whatever a footprint actually needs regardless of the
	// ring, so a larger ring buys prefetch, not correctness.
	static constexpr int32_t kDefaultFineRingRadiusTiles = 0;
	// Plan Storage section: "LRU with a configurable budget, default 8-16 GB".
	// A literal here is fine (it is the DEFAULT, overridable via the
	// constructor's BudgetBytes -- see -VoxelFineTileCacheBudgetGB= in
	// VoxelWorldSubsystem.cpp's Initialize()); the requirement is that
	// nothing downstream hardcodes the EFFECTIVE budget, which nothing does.
	// The budget is charged the DECODED size as well as the file size (rule 1
	// makes the decoded lattice, ~5x the file, the dominant cost), so this
	// number means what it says.
	static constexpr uint64_t kDefaultBudgetBytes = 12ull * 1024 * 1024 * 1024; // 12 GiB

	// RootDir mirrors terrain-service/terrain_service/cache.py's on-disk
	// layout: it is the cache ROOT, not a leaf -- the provider/seed/s16
	// segments come from vxc::formatFineTileCacheKey, so <root> is the
	// directory that CONTAINS <provider_id>/. (This differs from -VoxelTileDir,
	// which names the flat s1 leaf directly; the difference is deliberate,
	// because the fine tier's key is also its LRU identity and must be
	// formatted from one place.) ProviderId + Seed select the content-
	// addressed namespace this run's fine tiles live under (must match the
	// coarse tier's provider_id/seed exactly, or EnsureTileResident will
	// validate-reject every tile as an identity mismatch). BudgetBytes is the
	// LRU cache's byte ceiling. ClimateSource is borrowed and must outlive
	// this object: the fine tier carries elevation only, so climate delegates
	// to the coarse sampler (vxc::FineTileSampler handles the pixel-pitch
	// conversion itself).
	FVoxelFineTileStreamer(FString RootDir, FString ProviderId, uint64 Seed, uint64 BudgetBytes,
	                       vxc::ITileSampler* ClimateSource);

	// The sampler the world/amplifier generates terrain through. Safe to call
	// from meshing worker threads; see the threading note above.
	vxc::ITileSampler& WorldSampler() { return Proxy_; }

	// The ONE query gate callers must honor. Footprint is a column's XY
	// bounds in WORLD MILLIMETRES (VoxelCoords::WorldToMm), half-open like
	// the rest of this codebase's world-space rects; internally dilated by the
	// carrier's control stencil AND the cavern read margin before checking, so
	// callers must NOT pre-dilate. Pure, const, no I/O and no decode: with
	// whole-tile decode at load (rule 1), "resident" is a per-TILE fact, so
	// this is a handful of hash lookups under a shared lock. False means "do
	// not voxelize this yet"; see RequestFootprint for the call that can load.
	bool IsFootprintResident(int64 WorldMmX0, int64 WorldMmY0, int64 WorldMmX1, int64 WorldMmY1) const;

	// Synchronously loads+validates+decodes whatever `footprint` (same units
	// and dilation contract as IsFootprintResident) needs that is not
	// already resident. Returns false if any required tile is missing on
	// disk, or is present but fails validation (corrupt or a provider/seed/
	// coordinate mismatch -- see voxelcore/tilestreaming.h's
	// validateAndParseFineTile) -- the footprint stays non-resident and the
	// caller must keep deferring it. Never fabricates a fallback. GAME THREAD
	// ONLY, and it can block for a second or more per tile.
	bool RequestFootprint(int64 WorldMmX0, int64 WorldMmY0, int64 WorldMmX1, int64 WorldMmY1);

	// Per-streaming-tick housekeeping (call once per RecomputeDesiredSet,
	// not per chunk): pins every coarse tile within FineRingRadiusTiles of
	// PlayerCoarseTile (the prefetch ring), best-effort loads any of them
	// not yet resident (a tile the frontier has not generated yet simply
	// stays non-resident -- that is "block until ready" working as intended,
	// not a bug), then evicts whatever the LRU budget now disallows OUTSIDE
	// that pinned ring. GAME THREAD ONLY.
	void TickResidencyAndEviction(vxc::TileCoord PlayerCoarseTile);

	// Convenience: the coarse tile coordinate a world-mm point falls in,
	// using the fixed 15.36 km footprint every tier shares (vxtl-v2-format.md
	// §1). Callers use this to turn the anchor's world position into the
	// PlayerCoarseTile TickResidencyAndEviction wants.
	static vxc::TileCoord CoarseTileForWorldMm(int64 WorldMmX, int64 WorldMmY);

	void SetRingRadiusTiles(int32_t Radius) { RingRadiusTiles_ = Radius; }
	int32_t RingRadiusTiles() const { return RingRadiusTiles_; }

	// WHAT A GATE LEAK DOES. A leak means a query could not be answered from a
	// resident tile and could not be made resident either -- i.e. this run is
	// now generating terrain no other client will reproduce.
	//
	//   true  => UE_LOG(Fatal) on the FIRST leak. The run stops.
	//   false => Error once per distinct tile, then the sea-level answer, and
	//            the run continues.
	//
	// FVoxelWorldImpl::Initialize sets this from FApp::IsUnattended() (which
	// tools/voxel-capture.ps1 and the perf legs pass via -unattended), so an
	// automated run stops and an interactive editor session does not. See the
	// .cpp for why "stop" is the right default for an unattended run.
	void SetLeakIsFatal(bool bFatal) { bLeakIsFatal_ = bFatal; }
	bool LeakIsFatal() const { return bLeakIsFatal_; }

	// Diagnostics (HUD/logging), mirroring the missingTileQueries convention
	// already used elsewhere in this codebase for tile telemetry.
	uint64 ResidentBytes() const;
	uint64 BudgetBytes() const;
	uint64 ResidentTileCount() const;
	uint64 DecodedBytes() const { return DecodedBytes_; }
	uint64 CorruptTileLoadsSinceStart() const { return CorruptLoads_; }
	uint64 IdentityMismatchLoadsSinceStart() const { return IdentityMismatches_; }
	uint64 MissingFileLoadsSinceStart() const { return MissingFileLoads_; }
	uint64 TilesLoadedSinceStart() const { return TilesLoaded_; }
	// THE GATE-LEAK DETECTOR. Any nonzero value means some query reached the
	// sampler over a non-resident tile and was answered with sea level -- i.e.
	// the block-until-ready gate did not cover something it had to, and this
	// run's terrain is not reproducible on another client.
	uint64 MissingTileQueriesSinceStart() const
	{
		return Sampler_.missingTileQueries.load(std::memory_order_relaxed);
	}
	uint64 BlockDecodeFailuresSinceStart() const
	{
		return Sampler_.blockDecodeFailures.load(std::memory_order_relaxed);
	}
	// Queries that found their tile non-resident AND could not be satisfied by
	// a blocking load -- the leak count proper. Distinct from
	// MissingTileQueriesSinceStart(), which the sampler bumps for the same
	// queries but which no longer implies a leak on its own: a game-thread
	// query that misses, blocks, loads and answers correctly leaves this at 0.
	uint64 GateLeaksSinceStart() const { return GateLeaks_.load(std::memory_order_relaxed); }
	// Queries the funnel had to satisfy with a synchronous load because no
	// caller had gated them. Not an error -- this is block-until-ready doing
	// its job -- but a large number means some consumer should be calling
	// RequestFootprint in bulk instead of discovering misses one pixel at a
	// time, and it is the number that explains a stuttering game thread.
	uint64 BlockingLoadsSinceStart() const { return BlockingLoads_.load(std::memory_order_relaxed); }

private:
	friend class FVoxelFineTileSamplerProxy;

	FString LocalPathFor(vxc::TileCoord Tile) const;
	// Loads+validates+FULLY DECODES one coarse tile's fine data if not already
	// resident. Returns true if it is resident after the call (already was, or
	// the load succeeded this call). Caller must already hold Lock_ exclusively.
	bool EnsureTileResident_Locked(vxc::TileCoord Tile);
	// Which coarse tiles the dilated footprint of this world-mm rect touches.
	// Thin wrapper over vxc::tilesCoveringFootprint -- the arithmetic lives in
	// voxel-core so that tests/test_tilestreaming.cpp can exercise it with a
	// tile ABSENT, which is the direction this gate was never tested in.
	static std::vector<vxc::TileCoord> CoveredTiles(int64 WorldMmX0, int64 WorldMmY0, int64 WorldMmX1,
	                                                int64 WorldMmY1);
	// The cold path of FVoxelFineTileSamplerProxy::elevationMm: this pixel's
	// tile was NOT resident. Blocks and loads on the game thread; otherwise
	// (and if the load fails) reports a gate leak. Takes Lock_ itself, so the
	// caller must NOT hold it.
	int32_t ResolveNonResidentPixel(int64_t px, int64_t py);
	// Records + reports one leak, and terminates the run when bLeakIsFatal_.
	// Caller must already hold Lock_ exclusively. Returns the sea-level answer
	// for the non-fatal case, so the expression reads as the fallback it is.
	int32_t ReportGateLeak_Locked(vxc::TileCoord Tile, int64_t px, int64_t py);

	FString RootDir_;
	std::string ProviderId_;
	uint64 Seed_;
	// Guards Sampler_ in full: its tile map, every tile's decoded-block map,
	// and tileSize_. Shared for WorldSampler() queries, exclusive for load and
	// evict. mutable so IsFootprintResident can stay const.
	mutable FRWLock Lock_;
	vxc::FineTileSampler Sampler_;
	vxc::LruBudgetCache Budget_;
	FVoxelFineTileSamplerProxy Proxy_;
	int32_t RingRadiusTiles_ = kDefaultFineRingRadiusTiles;
	// Reverse map so eviction (which LruBudgetCache reports by cache-key
	// STRING) can call FineTileSampler::unloadTile(x, y) without re-parsing
	// the formatted key back into a coordinate.
	std::unordered_map<std::string, vxc::TileCoord> KeyToTile_;
	// Tiles whose file was absent on the last attempt. Purely to keep the
	// per-recompute prefetch loop from re-stat'ing the same nine missing
	// frontier tiles forever; cleared whenever the ring centre moves, so a
	// tile that appears on disk mid-session is still picked up.
	std::unordered_set<uint64> KnownMissing_;
	vxc::TileCoord LastRingCentre_{INT32_MIN, INT32_MIN};
	uint64 DecodedBytes_ = 0;
	uint64 CorruptLoads_ = 0;
	uint64 IdentityMismatches_ = 0;
	uint64 MissingFileLoads_ = 0;
	uint64 TilesLoaded_ = 0;
	// Gate-leak bookkeeping. Written only under Lock_ held exclusively (the
	// funnel's cold path is the only writer) but READ without it by
	// MaybeLogCounters on the game thread, so atomic: a torn or racing read of
	// a plain uint64 here is UB, and this is the counter the whole
	// after-the-fact correctness check rests on. Relaxed ordering is right --
	// nothing is published through these, they are only ever compared against
	// their own previous value.
	std::atomic<uint64> GateLeaks_{0};
	std::atomic<uint64> BlockingLoads_{0};
	// Written once by SetLeakIsFatal before the streamer is handed to the world
	// (MakeFineTileStreamer), read on every leak thereafter -- publish before
	// use, so a plain bool is sound.
	bool bLeakIsFatal_ = false;
	// Tiles already reported, so a non-fatal run logs once per tile instead of
	// 18.7 million times. Not cleared with KnownMissing_: the point is the
	// LOG's volume, and a tile that reappears has already had its say.
	std::unordered_set<uint64> LeakReportedTiles_;
};
