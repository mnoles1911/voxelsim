#pragma once
// Fine-tier streaming POLICY (docs/terrain-amplification-plan.md Phase 2,
// "Production: unbounded world, on-demand serving"; docs/vxtl-v2-format.md).
//
// Pure, UE-header-free logic for the client-side residency/prefetch/eviction
// system that sits on top of FineTileSampler (tilestore.h). Nothing here
// touches file I/O, threads, or UE types -- that is the host's job (see
// ue-project/Source/VoxelEarth/VoxelFineTileStreamer.h for the UE glue that
// drives this). Kept separate from tilestore.h/.cpp because that file owns
// DECODING the wire format; this one owns the policy of WHICH bytes must be
// resident and WHEN. That is a different axis of complexity, and per this
// task's own brief, the piece cheapest and most valuable to test in
// isolation: an off-by-one in the dilation/coverage maths does not fault, it
// silently returns edge values -- i.e. different terrain on two clients,
// exactly the failure tiles.h's kCarrierStencilLo/Hi comment documents as
// having already happened once.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "voxelcore/tiles.h"     // kCarrierStencilLo/Hi, kFineTileScale
#include "voxelcore/tilestore.h" // FineTile::parse

namespace vxc {

// --- coordinates -------------------------------------------------------

// A closed pixel-INDEX rectangle in FINE tile-pixel coordinates (tiles.h:
// pixel (0,0) covers world mm [0, pixelSizeMm) on each axis). Inclusive both
// ends -- a single-column footprint is px0==px1, py0==py1.
struct PixelRect {
    int64_t px0 = 0, py0 = 0, px1 = 0, py1 = 0;

    friend bool operator==(const PixelRect&, const PixelRect&) = default;
};

// One coarse tile coordinate -- the addressing unit both TileGridSampler and
// FineTileSampler key on. One .vxtl v2 file covers exactly one coarse tile
// coordinate's 15.36 km footprint at the fine scale (vxtl-v2-format.md §1).
struct TileCoord {
    int32_t x = 0, y = 0;

    friend bool operator==(const TileCoord&, const TileCoord&) = default;
};

struct TileCoordHash {
    size_t operator()(const TileCoord& t) const noexcept {
        const uint64_t k = (static_cast<uint64_t>(static_cast<uint32_t>(t.x)) << 32) |
                           static_cast<uint64_t>(static_cast<uint32_t>(t.y));
        return std::hash<uint64_t>()(k);
    }
};

// One block (FineTile's §4 index granularity) within a coarse tile,
// addressed the same way FineTileSampler::blockFor resolves a pixel:
// (tile.x, tile.y) selects the .vxtl file, (blockX, blockY) selects the
// block within it.
struct BlockCoord {
    TileCoord tile;
    uint32_t blockX = 0, blockY = 0;

    friend bool operator==(const BlockCoord&, const BlockCoord&) = default;
};

struct BlockCoordHash {
    size_t operator()(const BlockCoord& b) const noexcept {
        size_t h = TileCoordHash{}(b.tile);
        const uint64_t bk = (static_cast<uint64_t>(b.blockX) << 32) | static_cast<uint64_t>(b.blockY);
        h ^= std::hash<uint64_t>()(bk) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return h;
    }
};

// --- dilation ------------------------------------------------------------

// Dilates a footprint of tile-pixel CELL indices (inclusive [px0,px1] x
// [py0,py1]) by the carrier's control-point stencil (tiles.h
// kCarrierStencilLo/Hi), returning the range of CONTROL-POINT pixel indices
// every column whose CELL lies in the footprint may read.
//
// THIS IS THE CONTRACT tiles.h's own comment on kCarrierStencilLo/Hi warns
// about: "a window that is not dilated to match does not fault ... it
// silently returns an edge value ... different terrain on the two paths."
// Every caller deciding which fine blocks must be resident before touching a
// footprint MUST run it through this function first -- see
// dilatedBlockCoverage below, which does exactly that so the dilation can
// never be forgotten at a call site.
constexpr PixelRect dilateForCarrierStencil(PixelRect footprint) {
    return PixelRect{footprint.px0 + kCarrierStencilLo, footprint.py0 + kCarrierStencilLo,
                     footprint.px1 + kCarrierStencilHi, footprint.py1 + kCarrierStencilHi};
}

// --- block coverage --------------------------------------------------------

// Every (tile, block) whose block a fine-pixel rect overlaps, given the fine
// grid's own per-tile edge (`tileSizePx`, e.g. FineTileSampler::tileSize()
// once a tile is loaded, or FineTile::size()) and block edge (`blockDimPx`,
// FineTile::blockDim()). Handles rects that span a tile boundary, including
// negative tile coordinates, and returns nothing for an inverted or
// degenerate rect.
//
// `tileSizePx` and `blockDimPx` are runtime values read from the loaded
// tile's own header -- NEVER hardcode kFineTileSize/8192 or a literal block
// edge here. docs/vxtl-v2-format.md's own "three consequences" section
// (item 2) leaves both free for a format change, and this matters concretely
// right now: the plan text this module was briefed from says 256 fine px /
// 960 m blocks, 16x16 = 256 per tile, while the FROZEN vxtl-v2-format.md
// (dated the same day, superseding that text) specifies 256 fine px / 480 m
// blocks at 1.875 m/px, 32x32 = 1024 blocks per tile -- see the header field
// table in vxtl-v2-format.md §3 and FineTile::blockDim()/blocksPerAxis().
// Reading the constants from the tile in hand is what makes this code
// correct under EITHER number, and immune to the next one.
std::vector<BlockCoord> blocksCoveringRect(PixelRect rect, int64_t tileSizePx, uint32_t blockDimPx);

// dilateForCarrierStencil(footprint) then blocksCoveringRect -- the one call
// residency checks and prefetch requests should use.
std::vector<BlockCoord> dilatedBlockCoverage(PixelRect footprint, int64_t tileSizePx, uint32_t blockDimPx);

// --- the FULL read reach of a chunk's generation ---------------------------
//
// dilateForCarrierStencil covers the CARRIER's stencil and nothing else, and
// that is not the whole of what generating one chunk reads. The cavern layer
// samples the surface out to vxc::kCavernMaxReachMm around the column it is
// carving, which is why the GPU raster window
// (ue-project/Source/VoxelEarth/VoxelGpuRegionBuild.h, kRasterCavernMarginMm)
// grows the dispatch footprint by that much in WORLD MILLIMETRES before it
// converts to pixels. A residency gate that dilated only by the carrier
// stencil would therefore let a chunk be admitted whose cavern reads land in
// the NEXT, non-resident tile -- and vxc::FineTileSampler answers those with
// elevation 0, i.e. exactly the sea-level fallback the fine tier's whole
// block-until-ready rule exists to forbid. Undersizing does not fault; it
// silently produces terrain no other client will reproduce.
//
// So this is the one conversion a residency gate should use: world-mm rect
// (half-open, as the rest of the codebase's world rects are) -> the CLOSED
// fine-pixel rect a generator over that rect may touch, margin and carrier
// stencil both applied. `readMarginMm` is the caller's own generation reach
// (the UE host passes kRasterCavernMarginMm); pass 0 for carrier-only.
PixelRect fineReadPixelRect(int64_t worldMmX0, int64_t worldMmY0, int64_t worldMmX1, int64_t worldMmY1,
                            int64_t readMarginMm, int32_t pixelSizeMm = tilePixelSizeMm(kFineTileScale));

// --- tile <-> world mapping -------------------------------------------------

// The coarse tile coordinate containing a world-mm point, given the tile's
// world footprint in mm (kFineTileSize * pixelSizeMm(kFineTileScale) ==
// TileData::kTileSize * pixelSizeMm(1) == 15,360,000 mm == 15.36 km -- the
// footprint is IDENTICAL for the coarse and fine tiers by construction,
// vxtl-v2-format.md §1, so one function serves both tiers). Returns {0,0}
// for a non-positive footprint (defensive; callers should never pass one).
TileCoord tileCoordForWorldMm(int64_t worldMmX, int64_t worldMmY, int64_t tileFootprintMm);

// --- ring geometry -----------------------------------------------------

// Every coarse tile coordinate within Chebyshev distance `radiusTiles` of
// `center`, inclusive -- a (2*radius+1)^2 square, matching the plan's own
// "+-15.4 km" / "+-30.7 km" framing (one tile's footprint per side), not a
// circular disk. Empty for radiusTiles < 0.
std::vector<TileCoord> squareTileRing(TileCoord center, int32_t radiusTiles);

// --- cache key formatting -------------------------------------------------

// "<provider_id>/<seed:016x>/s<scale>/<x>_<y>" -- mirrors
// terrain-service/terrain_service/cache.py's TileCache.path() exactly (minus
// the root and the ".vxtl" extension, which are host concerns: this is a
// CACHE KEY, used both for LRU bookkeeping and, by a host, to build a
// filesystem path or a future HTTP request). seed is formatted as lowercase
// 16-hex-digit to match cache.py's `f"{seed:016x}"` byte for byte.
std::string formatFineTileCacheKey(const std::string& providerId, uint64_t seed, int32_t x, int32_t y,
                                    uint8_t scale = kFineTileScale);

// --- validation: "validate, never trust" ------------------------------

enum class FineTileVerdict {
    kOk,               // parsed, and its stamped identity matches what was requested
    kCorrupt,          // FineTile::parse rejected the bytes (truncated/malformed)
    kIdentityMismatch, // parsed fine, but seed/x/y in the header != what was requested
};

struct FineTileValidationResult {
    FineTileVerdict verdict = FineTileVerdict::kCorrupt;
    std::optional<FineTile> tile; // present iff verdict == kOk
    // Why FineTile::parse refused, meaningful only for kCorrupt. Carried out
    // because the two reasons a host most needs to tell apart --
    // kNoDecompressor (this build has no zstd wired up, a DEPLOYMENT bug) and
    // everything else (bad bytes) -- are already distinguished by FineError
    // and would otherwise be flattened into one "corrupt" log line, which is
    // exactly the confusion tilestore.h's FineError comment warns about.
    FineError error = FineError::kNone;
};

// Parses `bytes` (moved in; FineTile owns its bytes -- see tilestore.h) and
// checks the header's (seed, x, y) against what the cache entry was fetched
// FOR. This is the one place validation happens for a freshly-loaded fine
// tile: FineTile::parse already gives the all-or-nothing structural check
// (magic/version/section table/block index, vxtl-v2-format.md §9 item 4);
// the identity check on top follows the SAME precedent EditLog::
// checkProvider() already established for edit-log replay (compare a
// stamped identity field, refuse on mismatch) rather than inventing a
// second validation mechanism such as a content digest. A caller that gets
// anything but kOk MUST discard the bytes and re-fetch -- never use a
// partially-validated tile, and never retry-loop on the SAME bytes (a
// truncated/mismatched file will not become correct by re-parsing it).
//
// `decompressor` is the injected CODEC_ZSTD reader (tilestore.h), forwarded
// straight to FineTile::parse and kept for the tile's lifetime. Defaulted to
// none, which means CODEC_RAW only -- a CODEC_ZSTD tile is then refused whole,
// with error == FineError::kNoDecompressor, rather than half-loaded. A host
// that HAS a decompressor (ue-project/.../VoxelTileCodec.h) must pass it here
// or its zstd tiles are all rejected as corrupt.
FineTileValidationResult validateAndParseFineTile(std::vector<uint8_t> bytes, uint64_t expectedSeed,
                                                   int32_t expectedX, int32_t expectedY,
                                                   const FineDecompressor& decompressor = {});

// --- LRU budget cache ----------------------------------------------------

// Generic byte-budgeted LRU over opaque string keys, with a pin set that can
// never be evicted (plan Storage section: "LRU with a configurable budget
// ... pinning the active ring"). Tracks bookkeeping ONLY -- it does not own
// or free any storage itself. touch() when a key becomes resident, remove()
// once the caller has actually freed the backing storage, selectEvictions()
// to ask what SHOULD be freed next. That split keeps this class host-free:
// the UE glue frees storage via FineTileSampler::unloadTile, a standalone
// harness could free something else, and neither has to teach this class
// about it.
class LruBudgetCache {
public:
    explicit LruBudgetCache(uint64_t budgetBytes) : budgetBytes_(budgetBytes) {}

    uint64_t budgetBytes() const { return budgetBytes_; }
    void setBudgetBytes(uint64_t bytes) { budgetBytes_ = bytes; }
    uint64_t residentBytes() const { return residentBytes_; }
    size_t size() const { return entries_.size(); }
    bool contains(const std::string& key) const { return entries_.find(key) != entries_.end(); }

    // Records/refreshes `key` as resident, `bytes` large, and marks it
    // most-recently-used. Re-touching an already-tracked key with a
    // DIFFERENT byte count corrects residentBytes() by the delta rather than
    // double-counting (callers should not normally do this, but drift-free
    // accounting costs nothing to guarantee).
    void touch(const std::string& key, uint64_t bytes);

    // Untracks `key` (no-op if absent). The caller must already have freed
    // the backing storage -- this only updates bookkeeping.
    void remove(const std::string& key);

    // Replaces the pinned set outright with `keys` (the ACTIVE ring/block
    // set as of THIS call -- the caller recomputes it every tick from the
    // player's position, so there is no separate pin/unpin pair to keep in
    // sync). A pinned key need not already be tracked via touch().
    void setPinned(std::unordered_set<std::string> keys);

    // Every key the caller should evict to bring residentBytes() back under
    // budget, ordered LEAST-recently-used first, EXCLUDING every pinned key
    // even if that leaves the cache over budget -- pinning the active ring
    // is a harder constraint than the byte budget (plan: "pin the active
    // ring so the block under the player can never be evicted"). Pure
    // query: does not mutate any state. The caller is expected to free the
    // storage for each returned key and then call remove() on it.
    std::vector<std::string> selectEvictions() const;

private:
    struct Entry {
        uint64_t bytes = 0;
        uint64_t lastTouch = 0;
    };
    uint64_t budgetBytes_;
    uint64_t residentBytes_ = 0;
    uint64_t clock_ = 0;
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_set<std::string> pinned_;
};

} // namespace vxc
