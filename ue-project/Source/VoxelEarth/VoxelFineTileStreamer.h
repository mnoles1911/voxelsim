#pragma once
// FVoxelFineTileStreamer -- residency/prefetch/eviction manager for the baked
// .vxtl v2 fine tier (docs/terrain-amplification-plan.md Phase 2, "Production:
// unbounded world, on-demand serving"; docs/vxtl-v2-format.md).
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
// dilated control stencil, see vxc::kCarrierStencilLo/Hi and the dilation
// comment on vxc::dilateForCarrierStencil -- are not resident must not be
// voxelized. This is multiplayer: collision and edits must agree on every
// client, so a missing fine tile is a DESYNC, never a visual glitch to paper
// over with a procedural guess. IsFootprintResident() is that gate. Callers
// MUST check it before admitting a chunk whose footprint the fine tier
// covers, and MUST NOT admit the chunk if it returns false -- defer and let
// the caller's own per-tick rescan (RecomputeDesiredSet already re-evaluates
// every un-admitted candidate on anchor movement) retry later, once
// TickResidencyAndEviction has had a chance to load what was missing.
//
// SCOPE NOTE (read before wiring this any further): this class does NOT
// replace FVoxelWorldImpl::Tiles as the Amplifier's live ITileSampler. It is
// an independent residency/prefetch/eviction gate that owns its own
// vxc::FineTileSampler and is consulted as a PRECONDITION before generation
// is allowed to proceed. Actually rewiring the amplifier/World to read
// elevation FROM the fine tier once it is resident is a separate integration
// step this task deliberately did not take -- amplifier.cpp/World are off
// limits here (owned by another workstream) and the plan's own phasing
// (Phase 2 vs Phase 3) treats "fine tier resident" and "amplifier consumes
// it" as separate concerns.
//
// SYNCHRONOUS LOADING, NOT ASYNC -- BY SCOPE, NOT OVERSIGHT. Today's only
// transport is a local directory mirroring terrain-service's cache layout
// (cache.py), read with vxc::readFileBytes on the calling thread, exactly
// like MakeTileSampler's existing -VoxelTileDir path. A single fine tile is
// ~21-25 MB compressed (docs/terrain-amplification-plan.md "Size" section),
// so a cold ring shift that has to load several at once WILL block whatever
// thread calls TickResidencyAndEviction for the duration of those reads.
// Moving this to a background loader is future work -- see the .cpp's top
// comment for what that would need. This is the single largest unverified
// cost in this feature (no editor was available to measure it); see the
// task's final report for the full list.

#include "CoreMinimal.h" // FString/uint64/int64 -- see VoxelCoords.h for the same self-containment reasoning

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "voxelcore/tilestore.h"
#include "voxelcore/tilestreaming.h"

class FVoxelFineTileStreamer
{
public:
	// Plan: "fine ring radius of 1 tile (+-15.4 km)".
	static constexpr int32_t kDefaultFineRingRadiusTiles = 1;
	// Plan Storage section: "LRU with a configurable budget, default 8-16 GB".
	// A literal here is fine (it is the DEFAULT, overridable via the
	// constructor's BudgetBytes -- see -VoxelFineTileCacheBudgetGB= in
	// VoxelWorldSubsystem.cpp's Initialize()); the requirement is that
	// nothing downstream hardcodes the EFFECTIVE budget, which nothing does.
	static constexpr uint64_t kDefaultBudgetBytes = 12ull * 1024 * 1024 * 1024; // 12 GiB

	// RootDir mirrors terrain-service/terrain_service/cache.py's on-disk
	// layout (<root>/<provider_id>/<seed:016x>/s16/<x>_<y>.vxtl). ProviderId
	// + Seed select the content-addressed namespace this run's fine tiles
	// live under (must match the coarse tier's provider_id/seed exactly, or
	// EnsureTileResident will validate-reject every tile as an identity
	// mismatch -- see the .cpp). BudgetBytes is the LRU cache's byte ceiling.
	FVoxelFineTileStreamer(FString RootDir, FString ProviderId, uint64 Seed, uint64 BudgetBytes);

	// The ONE query gate callers must honor. Footprint is a column's XY
	// bounds in WORLD MILLIMETRES (VoxelCoords::WorldToMm), half-open like
	// the rest of this codebase's world-space rects; internally dilated by
	// the carrier's control stencil before checking, so callers must NOT
	// pre-dilate. Pure query -- decodes already-resident bytes if needed
	// (cheap, no I/O) but never reads from disk; see RequestFootprint for
	// the call that can. False means "do not voxelize this yet".
	bool IsFootprintResident(int64 WorldMmX0, int64 WorldMmY0, int64 WorldMmX1, int64 WorldMmY1);

	// Synchronously loads+validates+decodes whatever `footprint` (same units
	// and dilation contract as IsFootprintResident) needs that is not
	// already resident. Returns false if any required tile is missing on
	// disk, or is present but fails validation (corrupt or a provider/seed/
	// coordinate mismatch -- see voxelcore/tilestreaming.h's
	// validateAndParseFineTile) -- the footprint stays non-resident and the
	// caller must keep deferring it. Never fabricates a fallback.
	bool RequestFootprint(int64 WorldMmX0, int64 WorldMmY0, int64 WorldMmX1, int64 WorldMmY1);

	// Per-streaming-tick housekeeping (call once per RecomputeDesiredSet,
	// not per chunk): pins every coarse tile within FineRingRadiusTiles of
	// PlayerCoarseTile (the prefetch ring), best-effort loads any of them
	// not yet resident (a tile the frontier has not generated yet simply
	// stays non-resident -- that is "block until ready" working as intended,
	// not a bug), then evicts whatever the LRU budget now disallows OUTSIDE
	// that pinned ring.
	void TickResidencyAndEviction(vxc::TileCoord PlayerCoarseTile,
	                              int32_t FineRingRadiusTiles = kDefaultFineRingRadiusTiles);

	// Convenience: the coarse tile coordinate a world-mm point falls in,
	// using the fixed 15.36 km footprint every tier shares (vxtl-v2-format.md
	// §1). Callers use this to turn the anchor's world position into the
	// PlayerCoarseTile TickResidencyAndEviction wants.
	static vxc::TileCoord CoarseTileForWorldMm(int64 WorldMmX, int64 WorldMmY);

	vxc::FineTileSampler& Sampler() { return Sampler_; }
	const vxc::FineTileSampler& Sampler() const { return Sampler_; }

	// Diagnostics (HUD/logging), mirroring the missingTileQueries convention
	// already used elsewhere in this codebase for tile telemetry.
	uint64 ResidentBytes() const { return Budget_.residentBytes(); }
	uint64 BudgetBytes() const { return Budget_.budgetBytes(); }
	uint64 ResidentTileCount() const { return uint64(Sampler_.tileCount()); }
	uint64 CorruptTileLoadsSinceStart() const { return CorruptLoads_; }
	uint64 IdentityMismatchLoadsSinceStart() const { return IdentityMismatches_; }

private:
	FString LocalPathFor(vxc::TileCoord Tile) const;
	// Loads+validates+stores ONE coarse tile's fine data if not already
	// resident. Returns true if it is resident after the call (already was,
	// or the load+validate succeeded this call).
	bool EnsureTileResident(vxc::TileCoord Tile);

	FString RootDir_;
	std::string ProviderId_;
	uint64 Seed_;
	vxc::FineTileSampler Sampler_;
	vxc::LruBudgetCache Budget_;
	// blockDim, in fine pixels, of whichever tile was most recently loaded --
	// cached because FineTileSampler exposes tileSize() (the grid stride,
	// pinned from the first loaded tile) but not blockDim() directly; every
	// production tile shares one blockLog2 (encoded by one tile_codec.py
	// version), same assumption FineTileSampler itself makes about size().
	uint32_t BlockDimPx_ = 0;
	// Reverse map so eviction (which LruBudgetCache reports by cache-key
	// STRING) can call FineTileSampler::unloadTile(x, y) without re-parsing
	// the formatted key back into a coordinate.
	std::unordered_map<std::string, vxc::TileCoord> KeyToTile_;
	uint64 CorruptLoads_ = 0;
	uint64 IdentityMismatches_ = 0;
};
