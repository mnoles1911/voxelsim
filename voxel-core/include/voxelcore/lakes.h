#pragma once
// Baked lakes on the client: the basin table turned into a water surface a
// column at a time.
//
// docs/watershed-system-plan.md work item 4 (§5.1, §11). This is the CLIENT
// half of the registry the bake writes (tilestore.h `BasinEntry`,
// SECTION_BASIN_TABLE, bake_ver 8).
//
// ZERO NEW MECHANISMS, which is the whole design. A baked lake datum has the
// SAME SHAPE as `CavernColumn::floodZMm` -- "water fills open air below this
// millimetre level in this column" -- which `WaterMobilizer` already consumes
// through the implicit field at VoxelWaterSubsystem.cpp's binding site. So
// nothing here places, meshes, replicates, persists or mobilises water. It
// answers ONE question, `waterSurfaceMmAtVoxel`, and the tested, shipping,
// already-budgeted path downstream does the rest: unmobilised lake water is a
// wall, digging the shore fires `NotifyTerrainVoxelsCleared` ->
// `mobilizeEditRegion`, fill replicates as diffs, the ledger audits to zero.
//
// THE EXTENT RULE IS SHARED WITH THE BAKE, not re-derived. `lakeExtentFill`
// below is `terrain_service/bake/basins.py:lake_extent_mask` in C++:
//
//     the 8-connected component of {elevation <= surfaceMm}, clipped to the
//     basin's bbox, that contains the basin's seed cell.
//
// A THRESHOLD ALONE WOULD BE WRONG and that is why this is a fill. Two basins
// can share a bbox, and any hillside below the water level also satisfies
// `elevation <= surfaceMm`; a client that drew that would flood dry ground on
// the far side of a ridge. The seed is the deepest cell of the component the
// registry recorded, so the fill can only ever return that component.
//
// EIGHT-CONNECTED, matching `depression_components`. Four is the better
// physics -- water does not squeeze through a diagonal pinch -- but the
// registry's area was measured on the 8-connected component, and an extent
// that disagrees with the row it ships beside is a shoreline that does not
// close. Physics loses to the single definition, on purpose, in both
// languages, and `tests/test_lakes.cpp` asserts the two agree on a fixture.
//
// WHAT BOUNDS THE WATER FROM BELOW IS NOT THIS FILE. The extent is computed on
// the BAKED CONTROL LATTICE (1.875 m/px, which is exactly what a fine tile
// carries -- B5 subtracts `basin_depth` before encoding, so a shipped tile's
// elevation plane IS the re-opened surface these basins were measured on).
// What the player sees is the AMPLIFIED surface: carrier spline plus detail
// bands, at 10 cm. Those disagree by the detail band's amplitude near the
// shore. The composed predicate in §5.1 resolves it the only way that cannot
// produce floating water -- `zMm >= amplifiedGroundMm` -- so a column inside
// the extent whose amplified ground rises above the datum simply yields no
// water voxels, and the shoreline follows the ground's own contour instead of
// the lattice's staircase. The DATUM is authoritative; the extent says where
// to look; the ground says where to stop.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "voxelcore/core.h"
#include "voxelcore/tilestore.h"

namespace vxc {

// "No baked water over this column." INT32_MIN rather than a sentinel like
// kSeaLevelMm because a lake CAN sit below sea level datum-wise in principle
// and because `CavernColumn::floodZMm` already uses exactly this value for
// exactly this meaning -- one convention, two producers.
inline constexpr int32_t kNoWaterMm = INT32_MIN;

// Fill units (0..255, the CA's own currency) for the voxel whose BOTTOM is at
// `zBottomMm`, under a water surface at `surfaceMm`.
//
// PARTIAL TOP FILL (§5.1's "nicety", and it is not one): without it the
// surface snaps to the 10 cm lattice, so a lake whose datum sits at x.37 of a
// voxel renders 6.3 cm too high or too low and two adjacent lakes 9 cm apart
// render at the same height. The CA expresses fractional fill natively, so
// carrying it costs nothing and buys a surface that sits AT the datum.
//
// The bottom, not the centre (which is what `cavernFloodedAt` uses): a
// fraction needs the distance from the bottom face, and rounding rather than
// truncating keeps the mean error at zero instead of half a fill unit low.
//
// NO CLAMP TO 1 IS NEEDED and one was written and then deleted, because the
// arithmetic already forbids the case it guarded: 255 units span 100 mm, so
// one millimetre of water rounds to 3 units, and the only way to reach 0 is
// `rem <= 0`, which returns above. A defensive clamp here would have been
// unreachable code with a plausible-sounding comment attached, which is worse
// than none.
constexpr uint8_t waterFillUnits(int64_t zBottomMm, int32_t surfaceMm) {
    if (surfaceMm == kNoWaterMm) return 0;
    const int64_t rem = static_cast<int64_t>(surfaceMm) - zBottomMm;
    if (rem <= 0) return 0;
    if (rem >= kVoxelSizeMm) return 255;
    return static_cast<uint8_t>((rem * 255 + kVoxelSizeMm / 2) / kVoxelSizeMm);
}

// The extent of one basin, as a bitmask over its own bbox.
//
// `elev(lx, ly)` returns the tile-local control point elevation in absolute
// mm. Templated rather than taking an interface so the bake fixture, the unit
// tests and `FineTileSampler` all drive the SAME code -- a second copy for
// testing is how the two languages would drift.
//
// Returns the number of cells set. `out` is resized to bboxW * bboxH bytes,
// one per cell, 1 = wet.
//
// Explicit stack, no recursion: a basin can be 2.5 million cells (the survey's
// largest is 865 ha at 1.875 m/px) and a recursive fill on that is a stack
// overflow, not a slow function.
template <class ElevFn>
size_t lakeExtentFill(const BasinEntry& b, ElevFn&& elev, std::vector<uint8_t>& out) {
    const int32_t x0 = b.bboxX0, y0 = b.bboxY0, x1 = b.bboxX1, y1 = b.bboxY1;
    const int32_t w = x1 - x0 + 1, h = y1 - y0 + 1;
    out.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    if (w <= 0 || h <= 0) return 0;
    if (b.seedX < x0 || b.seedX > x1 || b.seedY < y0 || b.seedY > y1) return 0;
    if (b.surfaceMm == kNoWaterMm) return 0;

    const int32_t sx = int32_t(b.seedX) - x0, sy = int32_t(b.seedY) - y0;
    if (elev(b.seedX, b.seedY) > b.surfaceMm) return 0;

    std::vector<int32_t> stack;
    stack.reserve(256);
    out[size_t(sy) * w + sx] = 1;
    stack.push_back(sy * w + sx);
    size_t n = 1;
    static constexpr int32_t kDy[8] = {-1, 1, 0, 0, -1, -1, 1, 1};
    static constexpr int32_t kDx[8] = {0, 0, -1, 1, -1, 1, -1, 1};
    while (!stack.empty()) {
        const int32_t c = stack.back();
        stack.pop_back();
        const int32_t cy = c / w, cx = c - cy * w;
        for (int k = 0; k < 8; ++k) {
            const int32_t ny = cy + kDy[k], nx = cx + kDx[k];
            if (ny < 0 || ny >= h || nx < 0 || nx >= w) continue;
            const int32_t ni = ny * w + nx;
            if (out[size_t(ni)]) continue;
            if (elev(x0 + nx, y0 + ny) > b.surfaceMm) continue;
            out[size_t(ni)] = 1;
            ++n;
            stack.push_back(ni);
        }
    }
    return n;
}

// What the composed ImplicitFn asks, and the only thing this layer answers.
//
// An interface rather than a concrete class because two implementations
// already matter: `LakeSampler` over baked tiles, and "nothing is baked here"
// -- a client with no fine tier must still run, with the ocean and the caverns
// unaffected, rather than crash or invent a lake.
class IWaterSampler {
public:
    virtual ~IWaterSampler() = default;
    // Absolute mm of the baked water surface over this world VOXEL column, or
    // kNoWaterMm where there is none. NOT const: implementations decode and
    // cache lazily, and pretending otherwise would be a lie the caller might
    // thread on.
    virtual int32_t waterSurfaceMmAtVoxel(int64_t vx, int64_t vy) = 0;
};

// Answers kNoWaterMm everywhere. The client's default, so "no fine tier
// loaded" is a supported configuration and not a null check at every call.
class NullWaterSampler final : public IWaterSampler {
public:
    int32_t waterSurfaceMmAtVoxel(int64_t, int64_t) override { return kNoWaterMm; }
};

// Baked lakes over a `FineTileSampler`.
//
// THREADING mirrors `FineTileSampler`'s exactly, and for the same reason: the
// first query inside a basin decodes elevation blocks and builds that basin's
// extent mask, so queries MUTATE. Call `prewarmTile` from one thread over the
// region of interest; after that every touched basin is resident and queries
// are pure reads.
//
// COST. Building a basin's mask is O(bbox) once, and the bbox is on the wire
// precisely so it is not O(tile): the survey's median basin is 0.59 ha (about
// 1,700 cells) and its 90th percentile 2.27 ha. The bucket index below turns a
// column query into a handful of bbox tests rather than a scan of up to 266
// basins, and a one-entry column memo collapses the inner z loop of a brick
// sweep -- which is the access pattern the ImplicitFn actually has -- to one
// lookup per column instead of one per voxel.
class LakeSampler final : public IWaterSampler {
public:
    // Bucket edge in fine pixels for the per-tile basin index. 256 gives a
    // 32x32 index over an 8192 tile: small enough that a bucket holds a couple
    // of basins, big enough that the index itself is 1 KB of pointers.
    static constexpr int32_t kBucketPx = 256;

    explicit LakeSampler(FineTileSampler& tiles) : tiles_(tiles) {}

    // Decodes and indexes the tile's registry. Optional -- queries do it on
    // demand -- but doing it up front is what makes the query path a pure
    // read. False when the tile is not loaded.
    bool prewarmTile(int32_t tx, int32_t ty) { return indexFor(tx, ty) != nullptr; }

    int32_t waterSurfaceMmAtVoxel(int64_t vx, int64_t vy) override {
        const int32_t pxMm = tiles_.pixelSizeMm();
        if (pxMm <= 0) return kNoWaterMm;
        const int64_t px = floorDiv(vx * kVoxelSizeMm, pxMm);
        const int64_t py = floorDiv(vy * kVoxelSizeMm, pxMm);
        if (memoValid_ && px == memoPx_ && py == memoPy_) return memoMm_;
        const int32_t mm = surfaceAtPixel(px, py);
        memoPx_ = px;
        memoPy_ = py;
        memoMm_ = mm;
        memoValid_ = true;
        return mm;
    }

    // Same question in tile-pixel space, for tests and for the clipmap band
    // (work item 5), which walks pixels rather than voxels.
    int32_t surfaceAtPixel(int64_t px, int64_t py) {
        const uint32_t size = tiles_.tileSize();
        if (size == 0) return kNoWaterMm;
        const int32_t tx = int32_t(floorDiv(px, size)), ty = int32_t(floorDiv(py, size));
        TileIndex* idx = indexFor(tx, ty);
        if (idx == nullptr || idx->basins == nullptr) return kNoWaterMm;
        const int32_t lx = int32_t(px - int64_t(tx) * size);
        const int32_t ly = int32_t(py - int64_t(ty) * size);
        const int32_t bxi = lx / kBucketPx, byi = ly / kBucketPx;
        const std::vector<uint16_t>& cand =
            idx->buckets[size_t(byi) * idx->bucketsPerAxis + size_t(bxi)];
        // Highest surface wins where two basins somehow overlap. They should
        // not -- depression components are disjoint -- but "should not" is not
        // "cannot" across a bbox index, and picking the LOWER would drain a
        // lake into its neighbour's answer.
        int32_t best = kNoWaterMm;
        for (uint16_t id : cand) {
            const BasinEntry& b = (*idx->basins)[id];
            if (lx < b.bboxX0 || lx > b.bboxX1 || ly < b.bboxY0 || ly > b.bboxY1) continue;
            const std::vector<uint8_t>& mask = maskFor(*idx, id, tx, ty);
            if (mask.empty()) continue;
            const int32_t w = int32_t(b.bboxX1) - int32_t(b.bboxX0) + 1;
            if (!mask[size_t(ly - b.bboxY0) * size_t(w) + size_t(lx - b.bboxX0)]) continue;
            if (b.surfaceMm > best) best = b.surfaceMm;
        }
        return best;
    }

    // Diagnostics, so a bug is a number rather than an impression.
    size_t residentMaskCount() const { return maskCount_; }
    // Basins skipped because their tile is not loaded or a block failed to
    // decode. A non-zero value means water is MISSING, which looks exactly
    // like "there is no lake here" and must not.
    uint64_t unresolvedBasins() const { return unresolved_; }

private:
    struct TileIndex {
        const std::vector<BasinEntry>* basins = nullptr;
        size_t bucketsPerAxis = 0;
        std::vector<std::vector<uint16_t>> buckets;
        std::unordered_map<uint16_t, std::vector<uint8_t>> masks;
    };

    static uint64_t key(int32_t tx, int32_t ty) {
        return (uint64_t(uint32_t(tx)) << 32) | uint64_t(uint32_t(ty));
    }

    TileIndex* indexFor(int32_t tx, int32_t ty) {
        auto it = index_.find(key(tx, ty));
        if (it != index_.end()) return &it->second;
        const FineTile* t = tiles_.findTile(tx, ty);
        if (t == nullptr) return nullptr;
        TileIndex idx;
        // hasBasins() false means "baked before the registry existed", which
        // is NOT "no basins": leaving `basins` null keeps those two apart, and
        // a tile with an empty-but-present table indexes to zero candidates.
        if (t->hasBasins()) {
            idx.basins = &t->basins();
            const uint32_t size = t->size();
            idx.bucketsPerAxis = size_t((size + kBucketPx - 1) / kBucketPx);
            idx.buckets.resize(idx.bucketsPerAxis * idx.bucketsPerAxis);
            for (size_t i = 0; i < idx.basins->size(); ++i) {
                const BasinEntry& b = (*idx.basins)[i];
                if (!b.holdsWater()) continue;  // a dry playa has no surface
                for (int32_t byi = b.bboxY0 / kBucketPx; byi <= b.bboxY1 / kBucketPx; ++byi)
                    for (int32_t bxi = b.bboxX0 / kBucketPx; bxi <= b.bboxX1 / kBucketPx; ++bxi)
                        idx.buckets[size_t(byi) * idx.bucketsPerAxis + size_t(bxi)]
                            .push_back(uint16_t(i));
            }
        }
        auto ins = index_.emplace(key(tx, ty), std::move(idx));
        return &ins.first->second;
    }

    const std::vector<uint8_t>& maskFor(TileIndex& idx, uint16_t id, int32_t tx, int32_t ty) {
        auto it = idx.masks.find(id);
        if (it != idx.masks.end()) return it->second;
        const BasinEntry& b = (*idx.basins)[id];
        const uint32_t size = tiles_.tileSize();
        const int64_t ox = int64_t(tx) * size, oy = int64_t(ty) * size;
        // Every cell of the bbox is about to be read, so decode the blocks
        // once here instead of block-faulting inside the fill.
        if (!tiles_.prewarm(ox + b.bboxX0, oy + b.bboxY0, ox + b.bboxX1, oy + b.bboxY1)) {
            ++unresolved_;
            auto ins = idx.masks.emplace(id, std::vector<uint8_t>{});
            return ins.first->second;
        }
        std::vector<uint8_t> mask;
        lakeExtentFill(b,
                       [&](int32_t lx, int32_t ly) {
                           return tiles_.elevationMm(ox + lx, oy + ly);
                       },
                       mask);
        ++maskCount_;
        auto ins = idx.masks.emplace(id, std::move(mask));
        return ins.first->second;
    }

    FineTileSampler& tiles_;
    std::unordered_map<uint64_t, TileIndex> index_;
    size_t maskCount_ = 0;
    uint64_t unresolved_ = 0;
    int64_t memoPx_ = 0, memoPy_ = 0;
    int32_t memoMm_ = kNoWaterMm;
    bool memoValid_ = false;
};

// The composed predicate of §5.1, as one function so the client's binding site
// and the tests cannot express it differently.
//
//   cavern flood      -> as today, unchanged
//   baked lake        -> open air between the AMPLIFIED ground and the datum
//   otherwise         -> dry
//
// `groundMm` is the amplified surface for this column (Amplifier's
// `columnCached(vx, vy).surfaceMm`); it is what excludes a cave under the
// lakebed, and the mobilizer's own terrain half re-checks solidity per cell
// anyway. `waterSurfaceMm` is kNoWaterMm for a dry column.
constexpr uint8_t implicitWaterFill(int64_t vz, int32_t groundMm, int32_t waterSurfaceMm,
                                    bool cavernFlooded) {
    if (cavernFlooded) return 255;
    if (waterSurfaceMm == kNoWaterMm) return 0;
    const int64_t zMm = vz * kVoxelSizeMm;
    if (zMm < groundMm) return 0;  // below the ground surface: not open air
    return waterFillUnits(zMm, waterSurfaceMm);
}

} // namespace vxc
