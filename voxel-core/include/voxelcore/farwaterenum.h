#pragma once
// THE FAR-FIELD CANDIDATE SOURCE -- where the cascade's wet columns come from,
// and why they do not come from a sweep.
//
// ---------------------------------------------------------------------------
// WHAT THIS IS FOR
//
// `farwater.h` carries the cascade's RULES: which ring a column is drawn at,
// how a coarse column aggregates its children, what fill a cell takes, which
// bricks a column spans. It deliberately contains no sampler and no streaming,
// and it says so: "Enumeration stays at the binding site."
//
// This header is that enumeration, hoisted out of the binding site for the same
// reason the rules were: so `vxc_farwaterenum`, `vxc_farwaterprobe`,
// `test_farwaterenum.cpp` and `UVoxelWaterSubsystem` cannot express it
// differently. It answers exactly one question --
//
//     which brick columns in this window are wet, and what are their ground
//     and datum
//
// -- and nothing above it has to know that the answer came from two disjoint
// sources with completely different shapes.
//
// ---------------------------------------------------------------------------
// WHY IT IS NOT A SWEEP, IN THE ARITHMETIC THAT NEARLY WENT THE OTHER WAY
//
// The obvious binding is to walk the ring's coarse columns and ask each child
// "are you wet". That reads as reasonable and it is unaffordable: a coarse
// column at level L has 4^L children, so ring 5 alone is 4,225 x 1,024 = 4.3 M
// column queries, each an amplifier evaluation plus a datum resolve. Priced at
// the near field's own measured 2.8 us/column that is seconds of CPU for a
// single cold fill, and it is worse than the full-resolution scheme the cascade
// exists to replace.
//
// The order of the two stages is the whole fix, and it is not an optimisation:
//
//   1. Find the wet SET first, from the baked water plane, block-major, with
//      CONSTANT-dry blocks rejected out of the block INDEX -- no data-section
//      entry exists for them, so the reject costs zero bytes fetched and zero
//      decodes. Measured on the six wet-country tiles this is the majority of
//      the plane.
//   2. Resolve the amplified ground and the datum for the wet columns ONLY.
//
// Stage 1 costs blocks; stage 2 costs water. Neither costs swept area, which is
// the term that made the sweep quadratic in the radius.
//
// ---------------------------------------------------------------------------
// TWO SOURCES, AND THE UNION IS COUNTED RATHER THAN ASSERTED
//
// Rivers live in the water PLANE (an int16 depth per fine pixel). Lakes do not:
// the bake writes the plane DRY inside a registered basin and ships the basin
// registry plus a per-basin extent mask instead, which is why
// `CompositeWaterSampler` exists at all. So the candidate set is the union of a
// raster scan and a registry walk.
//
// THE BAKE CONTRACT SAYS THOSE TWO ARE DISJOINT. This header does not take its
// word for it. `src` carries one bit per source and `FarWaterEnumStats::
// bothWetCols` counts the columns that got both -- because if the two overlap,
// every brick, quad and byte the cascade is about to be sized on is inflated by
// the double count, and the failure is silent: a `wet |= 1` union absorbs an
// overlap perfectly and reports a smaller "new from lakes" number instead of a
// larger wrong one. A number that should be zero and is checked is worth more
// than a contract that is quoted.

#include <cstdint>
#include <vector>

#include "voxelcore/farwater.h"
#include "voxelcore/lakes.h"
#include "voxelcore/tilestore.h"

namespace vxc {

// Which source claimed a column. Bit flags, so an overlap is representable --
// which is the entire point (see the header note above).
inline constexpr uint8_t kFarWaterSrcPlane = 1u;
inline constexpr uint8_t kFarWaterSrcBasin = 2u;

// Everything the enumeration is judged on. Every field is a COUNT of something
// that actually happened, never a rate computed from an assumption: the
// rejection rate this header claims is the one its own walk realised over the
// window it was given, which is not the same number as the plane-wide census
// (a camera stands on water, so its neighbourhood is wetter than the average
// tile and the realised rate is the pessimistic one).
struct FarWaterEnumStats {
    // -- stage 1, the plane --------------------------------------------------
    uint64_t blocksVisited = 0;   // blocks the window's footprint touched
    uint64_t blocksNoTile = 0;    // not resident, or baked before the water plane
    uint64_t blocksConstDry = 0;  // INDEX-ONLY reject: zero bytes, zero decodes
    uint64_t blocksConstWet = 0;  // INDEX-ONLY accept: zero bytes, zero decodes
    uint64_t blocksDecoded = 0;   // the only blocks that cost a decode
    uint64_t blocksDecodeFailed = 0;

    // -- the union -----------------------------------------------------------
    uint64_t planeWetCols = 0;  // columns the plane marked
    uint64_t lakeWetCols = 0;   // columns the basin registry marked
    uint64_t bothWetCols = 0;   // MUST BE 0 -- the disjointness proof
    uint64_t basinsWalked = 0;
    uint64_t basinsNoMask = 0;

    // -- stage 2, resolve ----------------------------------------------------
    uint64_t colsResolved = 0;       // expensive calls actually made
    uint64_t colsWetAfterResolve = 0;// of those, datum genuinely above ground

    uint64_t unionWetCols() const { return planeWetCols + lakeWetCols - bothWetCols; }
    // Blocks that cost nothing but an index read, over blocks that had an index
    // to read. Excludes not-resident blocks, which are not a rejection -- they
    // are an absence, and counting them as a win would flatter a window that
    // simply had no tiles under it.
    double indexOnlyRejectRate() const {
        const uint64_t withIndex = blocksConstDry + blocksConstWet + blocksDecoded;
        return withIndex ? double(blocksConstDry) / double(withIndex) : 0.0;
    }
};

// A rectangular window of LOD-0 BRICK columns, with provenance and, once
// resolved, the ground and datum the cascade rules consume.
//
// LOD-0 BRICK COLUMNS RATHER THAN FINE PIXELS, and it is worth being explicit
// because the two resolutions differ by more than 2x in each axis (0.8 m brick
// against a 1.875 m fine pixel). `FarWaterAccumulator::resolve` divides by
// `step * step` children where a child is a LOD-0 brick column, so the grid has
// to be in that unit or the majority rule is being applied to a different
// denominator than the one it was measured with.
struct FarWaterColumnGrid {
    int64_t bx0 = 0, by0 = 0; // origin, absolute LOD-0 brick columns
    int32_t w = 0, h = 0;
    std::vector<uint8_t> src;       // kFarWaterSrc* bits, 0 = dry
    std::vector<FarWaterColumn> cols;

    void resize(int64_t ox, int64_t oy, int32_t width, int32_t height) {
        bx0 = ox;
        by0 = oy;
        w = width;
        h = height;
        src.assign(size_t(w) * size_t(h), 0);
        cols.assign(size_t(w) * size_t(h), FarWaterColumn{});
    }
    bool inBounds(int64_t bx, int64_t by) const {
        return bx >= bx0 && by >= by0 && bx < bx0 + w && by < by0 + h;
    }
    size_t at(int64_t bx, int64_t by) const {
        return size_t(by - by0) * size_t(w) + size_t(bx - bx0);
    }
    // Dry-by-default outside the grid, which is what `farWaterBrickIsInterior`
    // needs from its apron: a missing neighbour must read as a shoreline (and so
    // keep the brick a candidate), never as more water.
    const FarWaterColumn& colAt(int64_t bx, int64_t by) const {
        static const FarWaterColumn dry;
        return inBounds(bx, by) ? cols[at(bx, by)] : dry;
    }
};

// The fine pixel a brick column's ORIGIN voxel falls on.
//
// The ORIGIN and not the centre, matching `vxc_farwaterprobe`'s reference walk.
// It matters only at the 2.3-fine-pixels-per-brick boundary and only for which
// of two adjacent pixels a marginal column takes, but the two probes have to
// agree or their brick counts differ for no reason a reader could find.
inline int64_t farWaterBrickColToPixel(int64_t brickCol, int64_t pixelSizeMm) {
    return floorDiv(brickCol * int64_t(WaterBrick8::kEdge) * int64_t(kVoxelSizeMm), pixelSizeMm);
}

// ---------------------------------------------------------------------------
// STAGE 1a: THE RIVER HALF -- the water plane, block-major.
//
// Structurally `riverRibbonFillWet` with two differences, both of which matter
// here and neither of which matters there:
//
//   * IT REJECTS ON THE INDEX. `riverRibbonFillWet` decodes every block that
//     has a water plane and then scans all 65,536 pixels, because it wants a
//     dense mask and a CONSTANT-dry block costs it only the scan. The cascade
//     wants the opposite: `mode == kBlockConstant && constCp < 0` is the whole
//     answer for a 480 m square, so the block is skipped without decoding and
//     without touching a pixel. That reject is the property the whole
//     architecture rests on and it is measured, not assumed
//     (`FarWaterEnumStats::indexOnlyRejectRate`).
//
//   * IT WRITES BRICK COLUMNS, not fine pixels. The cascade's unit is the
//     LOD-0 brick column; a dense fine-pixel mask would have to be re-sampled
//     into that unit by the caller, and the resampling is where an off-by-one
//     would put the river half a brick from the lake half.
//
// A CONSTANT-WET block is accepted the same way, also without a decode: every
// pixel in it is wet by definition, so the inner loop marks columns without
// reading depth at all.
inline void farWaterEnumPlane(FineTileSampler& tiles, FarWaterColumnGrid& g, FarWaterEnumStats& st) {
    const int64_t pxMm = tiles.pixelSizeMm();
    const int64_t tileSize = int64_t(tiles.tileSize());
    if (pxMm <= 0 || tileSize <= 0 || g.w <= 0 || g.h <= 0) return;

    // The fine-pixel span the grid covers, then the blocks over it. `+1` on the
    // far edge because a brick column maps to a pixel by flooring, so the last
    // column can land on the pixel after the naive end.
    const int64_t px0 = farWaterBrickColToPixel(g.bx0, pxMm);
    const int64_t py0 = farWaterBrickColToPixel(g.by0, pxMm);
    const int64_t px1 = farWaterBrickColToPixel(g.bx0 + g.w, pxMm) + 1;
    const int64_t py1 = farWaterBrickColToPixel(g.by0 + g.h, pxMm) + 1;

    // Block geometry is a property of the tile, so it is read off a resident
    // one rather than assumed. A window with no tiles under it does nothing.
    const FineTile* probe = nullptr;
    for (int64_t ty = floorDiv(py0, tileSize); ty <= floorDiv(py1 - 1, tileSize) && !probe; ++ty)
        for (int64_t tx = floorDiv(px0, tileSize); tx <= floorDiv(px1 - 1, tileSize) && !probe; ++tx)
            probe = tiles.findTile(int32_t(tx), int32_t(ty));
    if (probe == nullptr) return;
    const uint32_t log2 = probe->blockLog2();
    const int64_t dim = int64_t(probe->blockDim());
    if (dim <= 0) return;

    std::vector<int16_t> depth;
    const int64_t gb0x = floorDiv(px0, dim), gb1x = floorDiv(px1 - 1, dim);
    const int64_t gb0y = floorDiv(py0, dim), gb1y = floorDiv(py1 - 1, dim);

    for (int64_t gby = gb0y; gby <= gb1y; ++gby) {
        for (int64_t gbx = gb0x; gbx <= gb1x; ++gbx) {
            ++st.blocksVisited;
            const int64_t bpx = gbx * dim, bpy = gby * dim;
            const int32_t tx = int32_t(floorDiv(bpx, tileSize));
            const int32_t ty = int32_t(floorDiv(bpy, tileSize));
            const FineTile* tile = tiles.findTile(tx, ty);
            if (tile == nullptr || !tile->hasWater()) {
                ++st.blocksNoTile;
                continue;
            }
            const uint32_t lbx = uint32_t((bpx - int64_t(tx) * tileSize) >> log2);
            const uint32_t lby = uint32_t((bpy - int64_t(ty) * tileSize) >> log2);
            const std::vector<FineBlockEntry>& idx = tile->waterIndex();
            const size_t bi = size_t(lby) * size_t(tile->blocksPerAxis()) + size_t(lbx);
            if (bi >= idx.size()) {
                ++st.blocksNoTile; // no index means no way to know: not a reject
                continue;
            }

            bool constWet = false;
            if (idx[bi].mode == kBlockConstant) {
                // kWaterDryDepth is -1 and every negative cp is the dry
                // sentinel. THIS BRANCH IS THE ARCHITECTURE: no data-section
                // entry is read, no frame is decompressed, no pixel is touched,
                // and a 480 m square of the world is answered.
                if (idx[bi].constCp < 0) {
                    ++st.blocksConstDry;
                    continue;
                }
                ++st.blocksConstWet;
                constWet = true;
            } else {
                ++st.blocksDecoded;
                if (!tile->decodeWaterBlock(lbx, lby, depth) ||
                    depth.size() < size_t(dim) * size_t(dim)) {
                    ++st.blocksDecodeFailed;
                    continue;
                }
            }

            // The brick columns whose origin pixel lands inside this block.
            // Derived by inverting farWaterBrickColToPixel and padding by one
            // on each side rather than by dividing -- the map is a floor, so the
            // exact inverse is a range and the pad is what stops a column
            // falling between two blocks.
            int64_t cbx0 = floorDiv(bpx * pxMm, int64_t(WaterBrick8::kEdge) * kVoxelSizeMm) - 1;
            int64_t cby0 = floorDiv(bpy * pxMm, int64_t(WaterBrick8::kEdge) * kVoxelSizeMm) - 1;
            int64_t cbx1 = floorDiv((bpx + dim) * pxMm, int64_t(WaterBrick8::kEdge) * kVoxelSizeMm) + 1;
            int64_t cby1 = floorDiv((bpy + dim) * pxMm, int64_t(WaterBrick8::kEdge) * kVoxelSizeMm) + 1;
            if (cbx0 < g.bx0) cbx0 = g.bx0;
            if (cby0 < g.by0) cby0 = g.by0;
            if (cbx1 > g.bx0 + g.w - 1) cbx1 = g.bx0 + g.w - 1;
            if (cby1 > g.by0 + g.h - 1) cby1 = g.by0 + g.h - 1;

            for (int64_t by = cby0; by <= cby1; ++by) {
                for (int64_t bx = cbx0; bx <= cbx1; ++bx) {
                    const int64_t ppx = farWaterBrickColToPixel(bx, pxMm);
                    const int64_t ppy = farWaterBrickColToPixel(by, pxMm);
                    if (ppx < bpx || ppx >= bpx + dim) continue;
                    if (ppy < bpy || ppy >= bpy + dim) continue;
                    if (!constWet) {
                        const size_t di = size_t(ppy - bpy) * size_t(dim) + size_t(ppx - bpx);
                        if (depth[di] < 0) continue;
                    }
                    uint8_t& s = g.src[g.at(bx, by)];
                    if ((s & kFarWaterSrcPlane) == 0) {
                        s = uint8_t(s | kFarWaterSrcPlane);
                        ++st.planeWetCols;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// STAGE 1b: THE LAKE HALF -- the basin registry and its extent masks.
//
// A lake is NOT in the plane (the bake writes it dry there), so this is not a
// second opinion about the same data -- it is the other half of the water, and
// in wet country it is the deep half. `holdsWater()` is the gate: a dry playa
// is a registered basin with no standing water and drawing it would put a lake
// in a desert.
//
// The mask is the bake's own `lake_extent_mask` (`lakeExtentFill` in lakes.h is
// the same rule in C++), so this walks what the registry recorded rather than
// re-deriving an extent from a threshold -- which would flood every hillside
// below the datum on the far side of a ridge.
inline void farWaterEnumBasins(IWaterSampler& water, FarWaterColumnGrid& g, FarWaterEnumStats& st) {
    const int64_t pxMm = water.pixelSizeMm();
    const int64_t tileSize = int64_t(water.tilePixels());
    if (pxMm <= 0 || tileSize <= 0 || g.w <= 0 || g.h <= 0) return;

    const int64_t px0 = farWaterBrickColToPixel(g.bx0, pxMm);
    const int64_t py0 = farWaterBrickColToPixel(g.by0, pxMm);
    const int64_t px1 = farWaterBrickColToPixel(g.bx0 + g.w, pxMm) + 1;
    const int64_t py1 = farWaterBrickColToPixel(g.by0 + g.h, pxMm) + 1;

    const int64_t brickMm = int64_t(WaterBrick8::kEdge) * int64_t(kVoxelSizeMm);

    for (int64_t ty = floorDiv(py0, tileSize); ty <= floorDiv(py1 - 1, tileSize); ++ty) {
        for (int64_t tx = floorDiv(px0, tileSize); tx <= floorDiv(px1 - 1, tileSize); ++tx) {
            const std::vector<BasinEntry>* rows = water.basinsForTile(int32_t(tx), int32_t(ty));
            if (rows == nullptr) continue;
            for (const BasinEntry& b : *rows) {
                if (!b.holdsWater()) continue;
                ++st.basinsWalked;
                const std::vector<uint8_t>* mask = water.extentMaskFor(int32_t(tx), int32_t(ty), b.basinId);
                if (mask == nullptr) {
                    // NOT "this basin is dry" -- lakes.h is explicit that a null
                    // mask means unresolvable. Counted so a run that quietly
                    // lost every lake cannot read as a run with no lakes.
                    ++st.basinsNoMask;
                    continue;
                }
                const int64_t ox = tx * tileSize, oy = ty * tileSize;
                const int64_t bw = int64_t(b.bboxX1) - int64_t(b.bboxX0) + 1;
                const int64_t bh = int64_t(b.bboxY1) - int64_t(b.bboxY0) + 1;
                if (bw <= 0 || bh <= 0 || int64_t(mask->size()) < bw * bh) continue;
                for (int64_t ly = 0; ly < bh; ++ly) {
                    for (int64_t lx = 0; lx < bw; ++lx) {
                        if ((*mask)[size_t(ly * bw + lx)] == 0) continue;
                        const int64_t ppx = ox + int64_t(b.bboxX0) + lx;
                        const int64_t ppy = oy + int64_t(b.bboxY0) + ly;
                        // Every brick column whose origin pixel is this pixel.
                        const int64_t cbx0 = floorDiv(ppx * pxMm, brickMm);
                        const int64_t cbx1 = floorDiv((ppx + 1) * pxMm - 1, brickMm);
                        const int64_t cby0 = floorDiv(ppy * pxMm, brickMm);
                        const int64_t cby1 = floorDiv((ppy + 1) * pxMm - 1, brickMm);
                        for (int64_t by = cby0; by <= cby1; ++by) {
                            for (int64_t bx = cbx0; bx <= cbx1; ++bx) {
                                if (!g.inBounds(bx, by)) continue;
                                if (farWaterBrickColToPixel(bx, pxMm) != ppx) continue;
                                if (farWaterBrickColToPixel(by, pxMm) != ppy) continue;
                                uint8_t& s = g.src[g.at(bx, by)];
                                if ((s & kFarWaterSrcBasin) != 0) continue;
                                if ((s & kFarWaterSrcPlane) != 0) ++st.bothWetCols;
                                s = uint8_t(s | kFarWaterSrcBasin);
                                ++st.lakeWetCols;
                            }
                        }
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// STAGE 2: RESOLVE, AND ONLY WHERE THE FIRST STAGE SAID TO.
//
// This is where the expensive calls live -- one amplifier column and one datum
// resolve each -- and the ONLY reason the cascade is affordable is that this
// loop is bounded by wet columns rather than by the window. Every column it
// touches is counted, so "order 4e4 for a cold fill at 819 m" is a number the
// probe prints rather than one anybody has to believe.
//
// `ground` must be the AMPLIFIED surface (#3, what the renderer draws), not the
// bake's reconstructed lattice: the cascade's brick z range starts at the
// ground, and using the lattice would bury or float the bed by the detail
// band's amplitude. `datum` is the composed water surface -- the same
// `waterSurfaceMmAtVoxel` the near field reaches, which is what makes near and
// far agree on height by construction rather than by tuning.
template <class GroundFn, class DatumFn>
inline void farWaterResolveWet(FarWaterColumnGrid& g, GroundFn&& ground, DatumFn&& datum,
                               FarWaterEnumStats& st) {
    for (int64_t by = g.by0; by < g.by0 + g.h; ++by) {
        for (int64_t bx = g.bx0; bx < g.bx0 + g.w; ++bx) {
            const size_t i = g.at(bx, by);
            if (g.src[i] == 0) continue;
            const int64_t vx = bx * int64_t(WaterBrick8::kEdge);
            const int64_t vy = by * int64_t(WaterBrick8::kEdge);
            FarWaterColumn c;
            c.groundMm = ground(vx, vy);
            c.datumMm = datum(vx, vy);
            g.cols[i] = c;
            ++st.colsResolved;
            // `wet()` is farwater.h's own test (datum present AND above the
            // ground). A column the plane called wet can still resolve dry --
            // the plane is the 1.875 m lattice and the ground is the 10 cm
            // amplified surface, and at a shoreline they disagree. That is a
            // real answer, not a miscount, so both numbers are reported.
            if (c.wet()) ++st.colsWetAfterResolve;
        }
    }
}

} // namespace vxc
