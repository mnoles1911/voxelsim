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

// `kNoWaterMm` moved to tilestore.h at bake_ver 9 -- the water PLANE is decoded
// there and must answer "dry" in the same currency this file's lake sampler
// does. Still one constant, one meaning; only its home changed.

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

    // ---- THE SHEET HALF (watershed plan work item 5, §5.2) ----------------
    //
    // `waterSurfaceMmAtVoxel` answers "is there water over THIS column", which
    // is the only question the near field's brick sweep asks. A sheet asks the
    // other one: "which lakes are near me, and where does each one END". That
    // cannot be reconstructed from a per-column query without walking every
    // column in a 10 km disc, so the registry the sampler already holds is
    // exposed rather than re-derived.
    //
    // DEFAULTED TO "NOTHING HERE", not pure virtual, for the same reason
    // NullWaterSampler exists at all: a client with no fine tier is a supported
    // configuration, and it should draw no sheets by construction rather than
    // by a null check at the call site.
    //
    // The pointers are borrowed and stay valid until the sampler is destroyed
    // or the underlying tile is evicted; a caller that holds one across a tile
    // load is holding a dangling pointer, which is why the UE actor copies what
    // it needs into world space in the same call.
    virtual const std::vector<BasinEntry>* basinsForTile(int32_t /*tx*/, int32_t /*ty*/) { return nullptr; }
    // The 1-byte-per-cell wet mask over `basinsForTile(tx,ty)[id]`'s bbox, in
    // the layout `lakeExtentFill` writes. nullptr when the tile or the basin is
    // not resolvable -- which must NOT be read as "this basin is dry".
    virtual const std::vector<uint8_t>* extentMaskFor(int32_t /*tx*/, int32_t /*ty*/, uint16_t /*id*/) {
        return nullptr;
    }
    // Tile edge in fine pixels, and mm per fine pixel: what turns a tile-local
    // bbox into world millimetres. 0 means "this sampler has no tiles".
    virtual uint32_t tilePixels() const { return 0; }
    virtual int32_t pixelSizeMm() const { return 0; }
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

    // ---- IWaterSampler's sheet half, over the index this class already builds.
    const std::vector<BasinEntry>* basinsForTile(int32_t tx, int32_t ty) override {
        TileIndex* idx = indexFor(tx, ty);
        return idx == nullptr ? nullptr : idx->basins;
    }
    const std::vector<uint8_t>* extentMaskFor(int32_t tx, int32_t ty, uint16_t id) override {
        TileIndex* idx = indexFor(tx, ty);
        if (idx == nullptr || idx->basins == nullptr || id >= idx->basins->size()) return nullptr;
        const std::vector<uint8_t>& m = maskFor(*idx, id, tx, ty);
        // An EMPTY mask is `maskFor`'s "could not resolve" (it already counted
        // an unresolved basin), not a dry one -- a dry basin has a mask full of
        // zeroes, not no mask. Collapsing the two here would let the sheet draw
        // nothing for a tile that failed to decode and call it a shoreline.
        return m.empty() ? nullptr : &m;
    }
    uint32_t tilePixels() const override { return tiles_.tileSize(); }
    int32_t pixelSizeMm() const override { return tiles_.pixelSizeMm(); }

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

// ---------------------------------------------------------------------------
// THE SHEET (watershed plan work item 5, §5.2)
// ---------------------------------------------------------------------------
//
// WHAT PROBLEM THIS SOLVES, precisely. `RefreshImplicitWater` meshes water only
// inside a 65-brick disc -- 52 m across. A lake is 2 km across. So beyond 26 m
// a baked lake is simply ABSENT: not dim, not low-detail, absent. Every vista,
// every screenshot from a ridge, every flight over the basin shows a dry hole
// where the water is. That is the whole of work item 5's first half.
//
// WHY RECTANGLES AND NOT A HEIGHTFIELD. The sheet is FLAT by construction --
// the datum is one number for the whole basin (§5.1) -- so the only thing its
// geometry has to express is its OUTLINE. A rectangle decomposition of the wet
// mask says exactly that and nothing else: no vertex carries a height, no
// vertex can disagree with its neighbour, and the whole basin is a few hundred
// triangles instead of the 1.6 million a per-cell grid over an 800k-cell extent
// would be.
//
// WHY THE MASK IS DECIMATED RATHER THAN MESHED AT 1.875 m. At the range this
// exists for, the shoreline's own pixel is far below the screen-space error of
// the terrain it meets: the clipmap draws that ground at 256 m per vertex. A
// decimation that keeps the outline within ~20 m is therefore invisible against
// its own backdrop while costing 1/100th of the triangles. `step` is the
// caller's, not a constant here, because the right value is a function of range
// and the caller is the only one who knows it.
//
// THE DECIMATED CELL IS WET IFF ITS CENTRE CELL IS WET, which is the choice
// that keeps the sheet INSIDE the lake. "Wet if any cell in the block is wet"
// grows the lake by up to a step in every direction and floats water over dry
// ground at the shore -- the exact artefact §5.1 spends its ground bound
// avoiding in the near field. Eroding by a centre sample can only ever lose a
// sliver of real water at the rim, which the near-field voxels draw anyway.

// One wet rectangle, in tile-local FINE PIXELS, inclusive on all four sides.
struct LakeSheetRect {
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// Decomposes a basin's extent mask into axis-aligned rectangles at `step` fine
// pixels, merging along X. Appends to `out`; returns the number appended.
//
// ROW RUNS, NOT A FULL RECTANGLE COVER. Merging in one axis is O(cells) and
// gives a convex lake ~one rectangle per row; a full 2D cover would give fewer
// primitives for a cost (and a bug surface) this does not need, since the
// rectangles are coplanar and abut exactly -- there is no crack to open between
// two of them however they are cut.
template <class MaskT>
size_t lakeSheetRects(const BasinEntry& b, const MaskT& mask, int32_t step,
                      std::vector<LakeSheetRect>& out) {
    if (step < 1) step = 1;
    const int32_t w = int32_t(b.bboxX1) - int32_t(b.bboxX0) + 1;
    const int32_t h = int32_t(b.bboxY1) - int32_t(b.bboxY0) + 1;
    if (w <= 0 || h <= 0) return 0;
    if (mask.size() != size_t(w) * size_t(h)) return 0;
    const size_t before = out.size();
    // Half a step, floored: the centre of a step-sized block. For step 1 this
    // is the cell itself, so a step of 1 reproduces the mask exactly.
    const int32_t half = step / 2;
    for (int32_t cy = 0; cy < h; cy += step) {
        const int32_t sy = (cy + half < h) ? (cy + half) : (h - 1);
        int32_t runStart = -1;
        for (int32_t cx = 0; cx < w; cx += step) {
            const int32_t sx = (cx + half < w) ? (cx + half) : (w - 1);
            const bool wet = mask[size_t(sy) * size_t(w) + size_t(sx)] != 0;
            if (wet && runStart < 0) {
                runStart = cx;
            } else if (!wet && runStart >= 0) {
                out.push_back(LakeSheetRect{b.bboxX0 + runStart, b.bboxY0 + cy,
                                            b.bboxX0 + cx - 1,
                                            b.bboxY0 + std::min(cy + step, h) - 1});
                runStart = -1;
            }
        }
        if (runStart >= 0) {
            out.push_back(LakeSheetRect{b.bboxX0 + runStart, b.bboxY0 + cy, b.bboxX0 + w - 1,
                                        b.bboxY0 + std::min(cy + step, h) - 1});
        }
    }
    return out.size() - before;
}

// `r` minus `hole`, as up to four rectangles written to `out`; returns how many.
// 0 means `r` is entirely inside `hole` and vanishes.
//
// THIS IS THE NEAR/FAR HANDOVER, and it is a subtraction rather than a fade
// because the two water surfaces are COPLANAR: the sheet and the near field's
// voxel water both sit at the datum, so an overlap is a z-fight AND a doubled
// translucent blend, and a gap is a ring of missing water. Only an exact cut
// gives neither, and an exact cut is available precisely because the near
// field's disc is an axis-aligned box in brick space.
inline size_t subtractRect(const LakeSheetRect& r, const LakeSheetRect& hole, LakeSheetRect out[4]) {
    // Disjoint (inclusive bounds, so touching edges do NOT overlap).
    if (hole.x1 < r.x0 || hole.x0 > r.x1 || hole.y1 < r.y0 || hole.y0 > r.y1) {
        out[0] = r;
        return 1;
    }
    size_t n = 0;
    if (hole.y0 > r.y0) out[n++] = LakeSheetRect{r.x0, r.y0, r.x1, hole.y0 - 1};        // below
    if (hole.y1 < r.y1) out[n++] = LakeSheetRect{r.x0, hole.y1 + 1, r.x1, r.y1};        // above
    const int32_t my0 = std::max(r.y0, hole.y0), my1 = std::min(r.y1, hole.y1);
    if (hole.x0 > r.x0) out[n++] = LakeSheetRect{r.x0, my0, hole.x0 - 1, my1};          // left
    if (hole.x1 < r.x1) out[n++] = LakeSheetRect{hole.x1 + 1, my0, r.x1, my1};          // right
    return n;
}

// Baked RIVERS over a `FineTileSampler`, from the P2 water plane.
//
// The counterpart of LakeSampler and deliberately much simpler, because the
// plane is already the answer: the bake did the non-local work (a
// runoff-weighted accumulation sweep and a descent-enforcing pass down the D8
// forest) and wrote the water surface per pixel, so a query here is one block
// decode and one lookup. No flood fill, no extent mask, no registry -- the
// asymmetry is the point, and it is why rivers cost a plane while lakes cost a
// table.
//
// THREADING and COST mirror FineTileSampler exactly: the first query in a
// block decodes it, so queries MUTATE. Call `prewarmTile` over the region of
// interest from one thread, then read from many.
class RiverSampler final : public IWaterSampler {
public:
    explicit RiverSampler(FineTileSampler& tiles) : tiles_(tiles) {}

    bool prewarmTile(int32_t tx, int32_t ty) {
        const FineTile* t = tiles_.findTile(tx, ty);
        return t != nullptr && t->hasWater();
    }

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

    // Same question in tile-pixel space, for tests and for the clipmap band.
    int32_t surfaceAtPixel(int64_t px, int64_t py) {
        const uint32_t size = tiles_.tileSize();
        if (size == 0) return kNoWaterMm;
        const int32_t tx = int32_t(floorDiv(px, size)), ty = int32_t(floorDiv(py, size));
        const FineTile* t = tiles_.findTile(tx, ty);
        // A tile that is not resident, or one baked before the water plane
        // existed, answers DRY -- and the residency gate
        // (FVoxelFineTileStreamer, block-until-ready) is what makes that safe:
        // no chunk generates over a non-resident footprint, so "dry because not
        // loaded" can never be voxelised into a desync.
        if (t == nullptr || !t->hasWater()) return kNoWaterMm;
        const uint32_t lx = uint32_t(px - int64_t(tx) * size);
        const uint32_t ly = uint32_t(py - int64_t(ty) * size);
        const uint32_t log2 = t->blockLog2();
        const uint32_t dim = t->blockDim();
        const uint64_t k = blockKey(tx, ty, lx >> log2, ly >> log2);
        auto it = blocks_.find(k);
        if (it == blocks_.end()) {
            std::vector<int16_t> depth;
            if (!t->decodeWaterBlock(lx >> log2, ly >> log2, depth)) {
                ++unresolvedBlocks_;
                return kNoWaterMm;
            }
            it = blocks_.emplace(k, std::move(depth)).first;
        }
        const size_t i = size_t(ly & (dim - 1)) * dim + size_t(lx & (dim - 1));
        const int16_t d = it->second[i];
        // DRY IS DECIDED BEFORE THE SPLINE, not after. The reconstruction below
        // costs a 4x4 stencil gather, and ~99% of a production tile is dry --
        // paying it to compute a ground we are about to throw away would put a
        // 16-probe carrier evaluation on every voxel column of the near-field
        // sweep, the hottest loop in the water path.
        if (d < 0) return kNoWaterMm;
        // THE RECONSTRUCTED SURFACE, `spline(cp)` -- not this pixel's control
        // point, which is what this function used to pass and what the whole
        // signature of `waterMmFromDepth` exists to prevent.
        //
        // The lattice was chosen here originally on the argument that a
        // per-pixel sampler has no spline and that a sub-voxel datum offset
        // cannot change a coarse "is there water near here" answer. The first
        // half is no longer true (`reconstructedGroundMm` is the spline, and it
        // is the shipped one), and the second half was never the whole story:
        // THIS SAMPLER IS THE NEAR-FIELD DATUM. `FVoxelWaterImpl`'s ImplicitFn
        // takes `waterSurfaceMmAtVoxel` and hands it straight to
        // `implicitWaterFill` as the water surface, so an offset here is an
        // offset in the rendered waterline -- and |cp - surface| reaches 5.6 m
        // on this world, which is 56 voxels.
        //
        // Having ONE datum, correct, is deliberate: the alternative on the
        // table was to keep the cheap lattice answer here and add a
        // ground-taking overload for the near field, which is exactly the shape
        // of the conflation that has now been made three times in this
        // codebase. A caller cannot pick the wrong one if there is only one.
        return FineTile::waterMmFromDepth(d, reconstructedGroundMm(tiles_, px, py));
    }

    uint32_t tilePixels() const override { return tiles_.tileSize(); }
    int32_t pixelSizeMm() const override { return tiles_.pixelSizeMm(); }

    size_t residentBlockCount() const { return blocks_.size(); }
    // Blocks whose payload failed to decode. Non-zero means water is MISSING,
    // which looks exactly like "there is no river here" and must not.
    uint64_t unresolvedBlocks() const { return unresolvedBlocks_; }

private:
    static uint64_t blockKey(int32_t tx, int32_t ty, uint32_t bx, uint32_t by) {
        return (uint64_t(uint32_t(tx)) << 44) ^ (uint64_t(uint32_t(ty)) << 24) ^
               (uint64_t(bx) << 12) ^ uint64_t(by);
    }

    // THE DEPTH PLANE ONLY. This used to cache the elevation block beside it,
    // on the argument that a water query always needs both -- true, but the
    // ground it needs is `spline(cp)`, whose 4x4 stencil straddles block and
    // tile boundaries and so cannot come from one cached block anyway.
    // `FineTileSampler` already caches decoded elevation blocks, and
    // `reconstructedGroundMm` reads through it, so keeping a second copy here
    // bought nothing and cost a decode per water block.
    FineTileSampler& tiles_;
    std::unordered_map<uint64_t, std::vector<int16_t>> blocks_;
    uint64_t unresolvedBlocks_ = 0;
    int64_t memoPx_ = 0, memoPy_ = 0;
    int32_t memoMm_ = kNoWaterMm;
    bool memoValid_ = false;
};

// Lakes and rivers as ONE query, which is what §5.1 means by "river water goes
// through the same ImplicitFn as lakes".
//
// The plan asked for this by putting lake surfaces INTO the water plane so the
// client had one uniform query. It is done here instead, and the difference is
// deliberate: writing a basin's surface into the plane as well would put a
// second copy of an already-shipped fact (SECTION_BASIN_TABLE's `surfaceMm`)
// on the wire, free to disagree with the first. Composing two samplers gives
// the same single query with no duplicated bytes and nothing to keep in step
// -- and the bake writes the plane DRY inside registered basins precisely so
// the two never both answer.
//
// HIGHEST WINS where they overlap anyway. They should not overlap; "should
// not" is not "cannot", and taking the lower would drain a lake into the river
// that feeds it.
class CompositeWaterSampler final : public IWaterSampler {
public:
    CompositeWaterSampler(IWaterSampler& lakes, IWaterSampler& rivers)
        : lakes_(lakes), rivers_(rivers) {}

    int32_t waterSurfaceMmAtVoxel(int64_t vx, int64_t vy) override {
        const int32_t a = lakes_.waterSurfaceMmAtVoxel(vx, vy);
        const int32_t b = rivers_.waterSurfaceMmAtVoxel(vx, vy);
        if (a == kNoWaterMm) return b;
        if (b == kNoWaterMm) return a;
        return a > b ? a : b;
    }

    // The sheet half belongs to the lakes: a river reach is not a flat disc
    // and cannot be drawn as one. Forwarded rather than merged so the sheet
    // actor keeps seeing exactly the registry it already consumes.
    const std::vector<BasinEntry>* basinsForTile(int32_t tx, int32_t ty) override {
        return lakes_.basinsForTile(tx, ty);
    }
    const std::vector<uint8_t>* extentMaskFor(int32_t tx, int32_t ty, uint16_t id) override {
        return lakes_.extentMaskFor(tx, ty, id);
    }
    uint32_t tilePixels() const override { return lakes_.tilePixels(); }
    int32_t pixelSizeMm() const override { return lakes_.pixelSizeMm(); }

private:
    IWaterSampler& lakes_;
    IWaterSampler& rivers_;
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
