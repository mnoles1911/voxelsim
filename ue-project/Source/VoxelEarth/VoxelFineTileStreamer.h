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
// (cache.py), read on the calling thread, exactly like MakeTileSampler's
// existing -VoxelTileDir path. A single fine tile is ~22-28 MB compressed and
// decodes to ~128 MB, so a cold ring shift that has to load several at once
// WILL block whatever thread calls TickResidencyAndEviction for the duration
// of those reads and decodes. Moving this to a background loader is future
// work -- see the .cpp's top comment for what that would need.
//
// --- THE READ IS RANGED, THE DECODE IS NOT (task #52) -------------------
//
// EnsureTileResident_Locked no longer slurps the whole file. It seeks: the
// preamble (header, section table, elevation index, water index, basin table --
// four DISJOINT regions, not a prefix), then the elevation and water payload
// blocks in coalesced ranges, all through vxc::FileRangeSource. The §6 FLOW
// plane is never read at all, because nothing in this module decodes it.
// Measured over the four shipped bv12 corridor tiles that is 179.4 MB -> 133.7
// MB read and held, a 25% cut in a read that happens on the game thread.
//
// WHAT IS DELIBERATELY *NOT* DONE, because it is where the rest of the money is
// and it is not a small change: residency is still per-TILE, and rule 1 above
// still decodes every block of a tile at load. So a footprint that needs nine
// 480 m blocks still costs the whole tile -- ~34 MB read and ~168 MB held --
// where block-granular residency would cost ~400 KB read and ~1.6 MB held, a
// further 100x on both. Retiring rule 1 means the funnel's residency test
// (FVoxelFineTileSamplerProxy::elevationMm) has to become per-BLOCK
// (vxc::FineTileSampler::blockDecoded exists for exactly that), the LRU has to
// evict blocks rather than tiles, and the ring prefetch has to mean something
// other than "the whole tile". The voxel-core half of that is built and tested
// (voxelcore/tilerange.h, vxc_sliceprobe); this file is what has not been
// converted, and doing it blind -- no CI job builds ue-project -- on the one
// path standing between this feature and a silent desync is not a trade worth
// making in the same change as the read.

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

	// THE COARSE FALLBACK -- what an absent fine tile resolves to. Without it,
	// a pixel whose fine tile is not on disk resolves to SEA LEVEL and the
	// world generates flat there; with it, unbaked ground renders as real
	// (coarse, 30 m/px) terrain. Owner decision, kept at the 8-ring cascade
	// because only part of the world is fine-baked. Rationale and the
	// known-absent-only gating live at ResolveNonResidentPixel in the .cpp.
	// Null by default, so behaviour is unchanged until something wires it.
	void SetCoarseFallback(vxc::ITileSampler* Sampler) { CoarseFallback_ = Sampler; }
	vxc::ITileSampler* CoarseFallback() const { return CoarseFallback_; }

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

	// --- BATHYMETRY, IN BULK --------------------------------------------------
	//
	// Fills an inclusive rect of fine tile-pixel coordinates with the baked lake
	// depth and signed distance-to-shore planes, in RAW WIRE UNITS (see
	// tilestore.h's bathyDepthMm / bathyShoreMm). Cells that could not be read
	// come back as vxc::kBathyMissing with a reason in the returned stats -- a
	// hole is a fact the caller must be able to see, because "no data" and "dry
	// land" drive opposite decisions in the material downstream.
	//
	// SAFE FROM A WORKER THREAD, and it is the only query on this class that is
	// safe from one WITHOUT the tile being prewarmed first. vxc::sampleBathyRect
	// decodes into the caller's own buffers and caches nothing, so unlike
	// WorldSampler()'s elevation path it never mutates the sampler -- a SHARED
	// lock is genuinely enough. (The elevation path can only claim that because
	// EnsureTileResident_Locked prewarms every block at load.)
	//
	// Does NOT load anything: a tile outside the resident set is reported as a
	// hole rather than blocked on. That is deliberate -- this feeds a cosmetic
	// field with a screen-space fallback, and it must never be able to stall the
	// game thread or trip the gate-leak detector.
	vxc::BathyRectStats ReadBathyRect(int64 Px0, int64 Py0, int64 Px1, int64 Py1,
	                                  int16_t* DepthOut, int16_t* ShoreOut,
	                                  int64 RowStrideElems) const;

	void SetRingRadiusTiles(int32_t Radius) { RingRadiusTiles_ = Radius; }
	int32_t RingRadiusTiles() const { return RingRadiusTiles_; }

	// --- WHERE THE RING IS, AND WHETHER IT HAS EVER MOVED -------------------
	//
	// ADDED 2026-08-23 BECAUSE THE ABSENCE OF THESE TWO NUMBERS COST A DAY.
	// The owner's PIE session printed a BYTE-IDENTICAL fine tier line in all 40
	// of its perf windows -- resident=4, loaded=4, gateLeaks=10814, frozen --
	// and that was read as "the streamer loaded four tiles at spawn and then
	// stopped following the player". It had not stopped. A coarse tile is
	// 15.36 km on a side (kTileFootprintMm) and the anchor moved 1.2 km in the
	// whole session, from (-61440,-61440) m to (-60510,-60684) m: it never left
	// tile (-4,-4), so a frozen resident set was the CORRECT answer and there
	// was nothing to prefetch. Nothing in the log said that, because the log
	// printed the ring RADIUS and never the ring CENTRE.
	//
	// These distinguish the two failures that produce the identical frozen
	// line, which is the whole point:
	//
	//   ringCentre = (INT32_MIN, INT32_MIN)  => TickResidencyAndEviction has
	//       NEVER RUN. The ring is not stale, it does not exist. That is a
	//       missing call site, and it is a real bug this could not previously
	//       be told apart from the healthy case.
	//   ringCentre set, ringMoves == 0       => the anchor has not left the tile
	//       it started in. Residency is correct and frozen is expected.
	//   ringMoves > 0 with a frozen resident set => the ring IS tracking and the
	//       tiles it wants are absent or refused; read absentOnDisk/refusedTiles.
	//
	// ringMoves counts CENTRE CHANGES, not ticks, and deliberately does not
	// count the first centre being established -- "0" then means exactly "has
	// not crossed a tile boundary yet" rather than "has run once". It can come
	// out either way on any leg with real travel, which is the requirement:
	// a flight that visibly crosses a 15.36 km boundary and still reports
	// ringMoves=0 is the frozen-ring bug, out loud.
	vxc::TileCoord RingCentreTile() const;
	uint64 RingCentreMovesSinceStart() const;

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
	// Tiles PRESENT on disk that this build has stopped trying to load (see
	// the retry contract in the .cpp). Distinct from absentOnDisk in the way
	// that matters: an absent tile is a frontier the bake has not reached and
	// it will load itself when it arrives; a refused one will not, ever, until
	// the file or this binary changes. A nonzero value here with a black
	// screen IS the diagnosis.
	// Counts give-UPS, so a tile that is refused, re-baked, and refused again
	// counts twice. That is the intent: each one is a separate event a person
	// has to act on.
	uint64 RefusedTileCount() const { return GivenUpTiles_; }
	// Load attempts skipped because the same bytes had already been refused --
	// the size of the spin that is no longer happening.
	uint64 SuppressedRetriesSinceStart() const { return SuppressedRetries_; }
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

	// --- THE LOCK FAST PATH'S TRAFFIC (-VoxelFineLockFast) ------------------
	//
	// TRAFFIC BEFORE TIMING. These four are ALWAYS counted, at every mode
	// including 0, because the first question about a fast path is not how fast
	// it is but whether it ran, and the second is what share of calls it caught.
	// They are one increment per RequestFootprint call -- not per pixel -- on
	// the game thread, so they cost nothing worth gating.
	//
	// HOW EACH ONE FAILS, so a zero can be read:
	//   fastCalls == 0                  the switch is off (mode 0), or nothing
	//                                   called RequestFootprint at all. The
	//                                   report line prints `fast=0` for the
	//                                   first, so the two are distinguishable.
	//   fastFree + fastShared == 0 with fastCalls > 0
	//                                   the fast path is IN the path and never
	//                                   answers -- every call still escalates.
	//                                   That is the fix doing nothing, and it
	//                                   is the reading that says revert.
	//   escalated == 0 during cold fill THE COLD PATH IS NOT BEING EXERCISED.
	//                                   Suspect a leg with everything already
	//                                   resident; this fix is untested there.
	uint64 FootprintRequestsSinceStart() const { return ReqCalls_.load(std::memory_order_relaxed); }
	// Answered with NO lock acquisition at all (mode 2+, game thread, mirror hit).
	uint64 FootprintRequestsLockFreeSinceStart() const { return ReqFastFree_.load(std::memory_order_relaxed); }
	// Answered under a SHARED acquire only (mode 1+, all covered tiles resident).
	uint64 FootprintRequestsSharedSinceStart() const { return ReqFastShared_.load(std::memory_order_relaxed); }
	// Fell through to the EXCLUSIVE load path -- the acquisitions this change
	// exists to remove. In steady state this should be ~0.
	uint64 FootprintRequestsExclusiveSinceStart() const { return ReqEscalated_.load(std::memory_order_relaxed); }
	// THE MODE-2 SAFETY ARGUMENT, AS A NUMBER RATHER THAN A CLAIM. The
	// lock-free mirror read is sound because its only writer is the game
	// thread. That is an argument about call sites, and call sites move. This
	// counts every mirror mutation that happened on some OTHER thread, so the
	// day a background tile loader lands, or someone calls RequestFootprint
	// from a worker, the assumption fails LOUDLY in the probe line instead of
	// silently producing a stale residency answer.
	//
	// Its failing readings: 0 is the required value and is what the argument
	// predicts. ANY nonzero value means -VoxelFineLockFast=2 is unsound on this
	// build and must be dropped to 1 (which is safe under any threading,
	// because it holds the shared lock). It is reported next to the audit
	// counters so the two cannot be read apart.
	uint64 MirrorOffThreadWritesSinceStart() const
	{
		return MirrorOffThreadWrites_.load(std::memory_order_relaxed);
	}
	// -VoxelFineLockFast=3 only: mirror answers cross-checked against
	// FineTileSampler::findTile under the shared lock, and how many disagreed.
	// A single mismatch is a correctness fault, not a tuning observation.
	uint64 MirrorAuditsSinceStart() const { return MirrorAudits_.load(std::memory_order_relaxed); }
	uint64 MirrorMismatchesSinceStart() const { return MirrorMismatches_.load(std::memory_order_relaxed); }

private:
	friend class FVoxelFineTileSamplerProxy;

	FString LocalPathFor(vxc::TileCoord Tile) const;
	// WHICH NAMESPACE SHOULD THIS RUN HAVE BEEN POINTED AT? Answered by reading
	// the cache root, never by recomputing an id.
	//
	// The fine provider id is a sha256 over the terrain-service BAKE's Python
	// constants (providers/diffusion.py::_bake_fingerprint). Nothing in C++ can
	// derive it and nothing in C++ should try: a second implementation of that
	// hash is a second answer, and the whole point of content addressing is that
	// there is one. What this side CAN do is look, because the bake writes its
	// answer down as the directory name -- <root>/<fine_provider_id>/... -- so
	// the namespaces that exist under a cache root are a fact readable with a
	// directory listing.
	//
	// Used only on the failure path, where one listing of a directory with a
	// handful of entries is free and the alternative is a fatal message that
	// names a path and leaves the operator to guess why it is the wrong one.
	// That guessing has cost this project three capture runs, each worked around
	// by hand-pinning -VoxelFineTileProviderId without anyone establishing why
	// the default was wrong. See docs/fine-tile-provider-identity.md.
	FString DiagnoseNamespace(vxc::TileCoord Tile) const;
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

	// --- THE RETRY CONTRACT -------------------------------------------------
	//
	// A load that failed falls into exactly one of two classes, and before this
	// they were the same class:
	//
	//   TRANSIENT -- the bytes were not all there. The file was locked, was
	//     being written, or shrank under the read (vxc::fineErrorIsTransient).
	//     Another attempt is the ONLY way this resolves, so it gets
	//     kMaxTransientLoadAttempts of them.
	//   PERMANENT -- the bytes were read and structurally refused, or carry
	//     another tile's identity. Re-reading them produces the identical
	//     verdict by construction. One attempt, then stop.
	//
	// Both are then memoised against the FILE VERSION -- its size and
	// last-write time -- rather than forever. That is what keeps this from
	// breaking the caller that legitimately depends on re-reading: a re-bake,
	// a finished download or a repaired file changes one of those two numbers
	// and earns a completely fresh set of attempts, automatically, with no
	// session restart and no cache to clear. What it will NOT do is read the
	// same bytes a second time and log the same refusal again.
	//
	// Deliberately NOT cleared with KnownMissing_ when the ring centre moves.
	// Absence is a fact about the WORLD and the bake frontier advances;
	// a structural refusal is a fact about these bytes and this binary, and
	// walking the player around changes neither.
	struct FTileLoadFailure
	{
		uint64 FileSize = 0;    // as of the attempt that failed
		int64 WriteTime = 0;    // std::filesystem::last_write_time ticks, 0 if unavailable
		uint32 Attempts = 0;    // against THIS file version only
		bool bPermanent = false;
		// The refusal, already spelled out (vxc::fineDescribeRejection). Kept
		// so a gate leak over this tile can name the cause instead of leaving
		// whoever reads the leak to go and find the earlier line.
		FString Why;
	};
	// Give up after this many TRANSIENT failures against one file version. 3
	// rather than 1 because a tile being rewritten by the bake genuinely does
	// produce a short read or two; rather than unbounded because a file that
	// is still unreadable on the third pass is not going to become readable on
	// the ten-thousandth, and the caller has no way to tell the difference.
	static constexpr uint32 kMaxTransientLoadAttempts = 3;
	// Records one failed attempt, logs it (Warning while attempts remain,
	// Error on the attempt that gives up), and returns false so call sites can
	// `return RecordLoadFailure_Locked(...)`. Caller must hold Lock_
	// exclusively. `Why` must already read as a complete sentence.
	bool RecordLoadFailure_Locked(vxc::TileCoord Tile, const FString& Path, uint64 FileSize,
	                              int64 WriteTime, const TCHAR* ReasonTag, const FString& Why,
	                              bool bTransient);
	// True when this tile's last load failed and its attempts are spent, WITHOUT
	// touching the filesystem. For the query funnel, which crosses this branch
	// millions of times a run and must not open a file to learn something it
	// already knows; EnsureTileResident_Locked does the stat'ing version, so a
	// changed file is still noticed by the residency tick. Caller must hold
	// Lock_ (shared is enough, but every current caller holds it exclusively).
	bool IsSettledFailure_Locked(vxc::TileCoord Tile) const;

	// --- THE ALLOCATION-FREE COVERAGE, AND WHY IT IS NOT A SECOND ARITHMETIC --
	//
	// CoveredTiles() returns a std::vector, i.e. a heap allocation, and
	// RequestFootprint is called four times per chunk at two hot sites. At the
	// 14,099 chunks/s peak that is ~100,000 allocations a second on the game
	// thread for a rect that covers ONE tile in the overwhelming majority of
	// cases. This is the same coverage with no allocation: a closed tile RANGE
	// [X0..X1] x [Y0..Y1] instead of an enumerated vector.
	//
	// IT COMPOSES THE SAME TWO voxel-core CALLS, IN THE SAME ORDER, AS
	// vxc::tilesCoveringFootprint DOES INTERNALLY -- fineReadPixelRect (the one
	// authority on the cavern read margin and the carrier stencil) then
	// tileCoordForPixel on the two corners. It does NOT re-derive either. The
	// margin and the stencil are exactly where they were: a dilation this file
	// computed for itself is precisely how the gate came to disagree with the
	// GPU raster window once already.
	//
	// And composition is not proof, so the constructor CHECKS it: see
	// SelfCheckTileRangeAgreesWithCoverage_, which compares this against
	// vxc::tilesCoveringFootprint over origin, negative, boundary-straddling and
	// multi-tile rects and refuses to start on a disagreement.
	struct FTileRange
	{
		int32 X0 = 0, Y0 = 0, X1 = -1, Y1 = -1;
		bool bValid = false;
		int64 Count() const
		{
			return bValid ? (int64(X1) - int64(X0) + 1) * (int64(Y1) - int64(Y0) + 1) : 0;
		}
	};
	static FTileRange CoveredTileRange(int64 WorldMmX0, int64 WorldMmY0, int64 WorldMmX1, int64 WorldMmY1);
	// Runs at construction. Fatal on disagreement -- see the comment above.
	static void SelfCheckTileRangeAgreesWithCoverage_();
	// True iff every tile in Range is in ResidentTiles_. GAME THREAD ONLY and no
	// lock: see the mirror's comment on ResidentTiles_ for why that is sound.
	bool RangeResidentInMirror_(const FTileRange& Range) const;
	// True iff every tile in Range is resident per FineTileSampler. Caller must
	// hold Lock_ (shared is enough).
	bool RangeResidentInSampler_Locked(const FTileRange& Range) const;
	// -VoxelFineLockFast=3: compares the two above and reports any disagreement.
	// Caller must hold Lock_ shared. Returns the SAMPLER's answer, always --
	// the audit never lets the mirror decide.
	bool AuditMirrorAgainstSampler_Locked(const FTileRange& Range, bool bMirrorSaid) const;
	// Called at every one of the three mirror writes. See
	// MirrorOffThreadWritesSinceStart() for what a nonzero count means.
	void NoteMirrorWriteThread_()
	{
		if (!IsInGameThread())
		{
			MirrorOffThreadWrites_.fetch_add(1, std::memory_order_relaxed);
		}
	}
	// The probe's own report line. Game thread, called from
	// TickResidencyAndEviction with Lock_ RELEASED (it must not be timed as
	// part of the hold it is measuring), on the -VoxelPerfLogInterval cadence.
	void MaybeLogLockProbe_();

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

	// Set once at construction time by the world subsystem; read from worker
	// threads without a lock, which is safe because it never changes after.
	vxc::ITileSampler* CoarseFallback_ = nullptr;

	// Samples CoarseFallback_ at the world position of a FINE pixel. Caller
	// must have checked CoarseFallback_ is non-null.
	int32_t CoarseElevationMm(int64_t px, int64_t py) const;
	// Tiles that ARE on disk and whose load failed, keyed by tile hash. See the
	// retry contract above: this is the thing that turns "retry forever at 30
	// ms" into "attempt, refuse, say why, stop".
	std::unordered_map<uint64, FTileLoadFailure> LoadFailures_;
	// The prefetch ring's centre, and the sentinel that means "no tick yet".
	// The sentinel is load-bearing for the diagnostic above: it is the only
	// thing that can say TickResidencyAndEviction was never called at all, as
	// opposed to called with an anchor that never moved.
	vxc::TileCoord LastRingCentre_{INT32_MIN, INT32_MIN};
	// Times the centre CHANGED after being established. Written under Lock_
	// exclusively by TickResidencyAndEviction (game thread) and read under it
	// shared by the perf log (also game thread), so a plain uint64 is enough --
	// unlike GateLeaks_, which the meshing workers can write.
	uint64 RingCentreMoves_ = 0;
	uint64 DecodedBytes_ = 0;
	uint64 CorruptLoads_ = 0;
	uint64 IdentityMismatches_ = 0;
	uint64 MissingFileLoads_ = 0;
	uint64 TilesLoaded_ = 0;
	// Distinct tile/file-version pairs this streamer has stopped retrying, and
	// the attempts that stop saved. Both are diagnostics only.
	uint64 GivenUpTiles_ = 0;
	uint64 SuppressedRetries_ = 0;
	// Gate-leak bookkeeping. Written only under Lock_ held exclusively (the
	// funnel's cold path is the only writer) but READ without it by
	// MaybeLogCounters on the game thread, so atomic: a torn or racing read of
	// a plain uint64 here is UB, and this is the counter the whole
	// after-the-fact correctness check rests on. Relaxed ordering is right --
	// nothing is published through these, they are only ever compared against
	// their own previous value.
	std::atomic<uint64> GateLeaks_{0};
	std::atomic<uint64> BlockingLoads_{0};

	// --- THE GAME-THREAD RESIDENCY MIRROR (-VoxelFineLockFast=2 and 3) -------
	//
	// The set of tile hashes currently resident in Sampler_. It exists so the
	// hot fast path can answer "is this footprint already resident" WITHOUT
	// touching Lock_ at all -- not even shared, because a shared acquire is
	// still an atomic write to one cacheline that 36 worker threads are also
	// writing, four to eight times per chunk.
	//
	// WHY READING IT WITHOUT A LOCK IS SAFE, AND IT IS A THREAD-IDENTITY
	// ARGUMENT RATHER THAN A MEMORY-ORDERING ONE:
	//
	//   WRITERS. Exactly three statements mutate it, and each sits immediately
	//   beside the Sampler_.loadTile / Sampler_.unloadTile it mirrors:
	//     1. EnsureTileResident_Locked, after a successful loadTile      (insert)
	//     2. EnsureTileResident_Locked, on the prewarm failure unloadTile (erase)
	//     3. TickResidencyAndEviction's LRU eviction unloadTile           (erase)
	//   All three are inside Lock_ held EXCLUSIVELY, and all three are on the
	//   GAME THREAD: RequestFootprint and TickResidencyAndEviction are game
	//   thread by contract, and ResolveNonResidentPixel only loads on the
	//   IsInGameThread() branch (its worker branch takes the lock but never
	//   loads or unloads -- it reports a leak and returns).
	//
	//   READERS. The lock-free read happens ONLY inside `IsInGameThread()`.
	//
	//   So the single writer and the lock-free reader are the SAME THREAD.
	//   There is no race to order and no staleness to bound: the value the game
	//   thread reads is the value the game thread last wrote. Nothing needs to
	//   be atomic and nothing needs a fence.
	//
	// WHY A STALE READ CANNOT PRODUCE WRONG TERRAIN, which is the failure this
	// project fears most because it does not crash:
	//   * A stale TRUE ("resident" when it is not) is the dangerous direction,
	//     and it is impossible: the only thing that removes residency is an
	//     eviction, evictions run on the game thread under the exclusive lock,
	//     and they erase from here in the same statement group that calls
	//     unloadTile. A reader on any other thread never consults this at all.
	//   * A stale FALSE is harmless and self-correcting: the call falls through
	//     to the shared-lock check and then to today's exclusive load path, so
	//     the worst case is exactly the behaviour of -VoxelFineLockFast=0.
	//   * The window between this answering TRUE and the caller voxelizing is
	//     NOT new. Today's exclusive path releases the lock before returning
	//     too, so a later eviction could always invalidate an answer already
	//     given. This changes the lock mode, not the lifetime of the answer.
	//
	// -VoxelFineLockFast=3 cross-checks every mirror answer against
	// FineTileSampler::findTile under the shared lock and reports disagreement
	// as an Error with a count, so "the mirror drifted" is a measured number
	// rather than this comment's promise.
	std::unordered_set<uint64> ResidentTiles_;
	// RequestFootprint traffic. Relaxed atomics rather than plain uint64s
	// purely defensively: the contract is game-thread-only, and if some future
	// caller breaks it these must still not be UB while the assert-shaped
	// IsInGameThread() branches route it to the locked path.
	std::atomic<uint64> ReqCalls_{0};
	std::atomic<uint64> ReqFastFree_{0};
	std::atomic<uint64> ReqFastShared_{0};
	std::atomic<uint64> ReqEscalated_{0};
	std::atomic<uint64> MirrorOffThreadWrites_{0};
	// mutable: the audit runs from the const IsFootprintResident, and an audit
	// that could not count from a const path would simply not be run there --
	// which is the admission sweep, i.e. most of the calls.
	mutable std::atomic<uint64> MirrorAudits_{0};
	mutable std::atomic<uint64> MirrorMismatches_{0};
	// When the probe last printed. FPlatformTime::Seconds() at construction, so
	// the first window is a full interval rather than an instant one.
	double LastProbeLogSeconds_ = 0.0;
	// Written once by SetLeakIsFatal before the streamer is handed to the world
	// (MakeFineTileStreamer), read on every leak thereafter -- publish before
	// use, so a plain bool is sound.
	bool bLeakIsFatal_ = false;
	// Tiles already reported, so a non-fatal run logs once per tile instead of
	// 18.7 million times. Not cleared with KnownMissing_: the point is the
	// LOG's volume, and a tile that reappears has already had its say.
	std::unordered_set<uint64> LeakReportedTiles_;
};
