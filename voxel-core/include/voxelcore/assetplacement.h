#pragma once
// Where environmental assets stand, and -- the part that matters -- a PROVABLE
// UPPER BOUND on how high they reach, cheap enough to run on the streaming
// admission path.
//
// ---------------------------------------------------------------------------
// WHY A BOUND IS THE CENTRE OF THIS FILE AND THE SCATTER IS NOT
// ---------------------------------------------------------------------------
//
// The streaming layer does not generate chunks it can prove are empty. That
// proof is Amplifier::surfaceUpperBoundMm, and its soundness argument is
// quoted in amplifier.h: "materialAt is unconditionally MAT_AIR above the
// surface ... no pass in the amplifier can turn air into solid. So a chunk
// whose lowest voxel centre sits above this bound is provably all air, and
// skipping it can never hide geometry."
//
// An asset is solid ABOVE the surface. It breaks that argument outright. The
// consequence is not a lost optimisation -- bench/terrainprobe.cpp:666-669
// says it plainly: "a bound that is too tight is not a lost optimisation, it
// is terrain that never generates. A hole in the world." A 28 m emergent whose
// crown lands in chunks the bound proved empty is not a tree that renders
// badly; it is a tree that does not exist, and nothing anywhere logs that it
// did not.
//
// There is exactly one precedent in this codebase for something solid above
// the surface, and it is instructive: MAT_WATERMARK (core.h:322-333, "the one
// thing in this enum that is SOLID ABOVE THE SURFACE"). It pays for itself at
// amplifier.cpp:2087-2113 by widening the bound by a CONSTANT
// (kWaterMarkerHeightMm), with the reasoning spelled out: "The widening is a
// CONSTANT rather than a max over the rect, because a per-rect water query
// would cost a block decode on the streaming admission path this function
// exists to keep cheap."
//
// This file deliberately does NOT copy that. A constant widening sized for the
// tallest asset in the library is 28 m of extra sky over EVERY footprint on
// the planet -- roughly nine additional level-0 chunk layers admitted, meshed
// and evicted across the entire world, to carry trees that stand on a small
// fraction of it. The water marker could afford a blunt constant because it is
// a debug instrument that is off in every shipping configuration; assets are
// always on.
//
// What makes a per-rect answer affordable here, where it was not for water, is
// that the query is PURE HASH: no tile decode, no block read, no I/O, no
// column evaluation. Deciding whether a cell carries a site is one splitmix64.
// That is the whole trick, and everything below is arranged to keep it true.
//
// ---------------------------------------------------------------------------
// THE VETO-ONLY RULE, which is what keeps the bound sound without a biome read
// ---------------------------------------------------------------------------
//
// Placement obviously has to depend on biome: emergents belong in rainforest,
// not tundra. But evaluating biome means evaluating a column, and a column is
// exactly the cost this bound may not pay (amplifier.h:428-431: "no column() --
// that is the entire point, since this runs per candidate chunk on the
// streaming admission path where a single column() would already be too
// expensive").
//
// The resolution is a rule on what the caller's policy is ALLOWED to do:
//
//     A POLICY MAY ONLY VETO A SITE. It may not create one, move one,
//     or substitute a taller asset than its layer's declared maximum.
//
// Under that rule the bound never needs to know what the policy decided,
// because vetoing can only ever make the true maximum LOWER. The bound
// computed here is therefore an upper bound over every policy, including the
// ones not written yet. That is what lets biome rules change -- a thing that
// will happen many times -- without any risk of invalidating a bound whose
// failure mode is silent and shaped like a rendering bug.
//
// The rule is not merely documented; assetTopAboveSurfaceMm is a function of
// the layer table alone, so there is no argument through which a policy COULD
// raise it. tests/test_assetplacement.cpp pins that direction explicitly.
//
// ---------------------------------------------------------------------------
// LAYERS, and why not one lattice
// ---------------------------------------------------------------------------
//
// A single "one candidate per cell" lattice cannot serve both a 28 m emergent
// (which wants ~30 m spacing) and ground cover (which wants ~40 cm). Sized for
// the trees it cannot place grass; sized for the grass it evaluates ~5,000
// cells per chunk footprint to find one tree.
//
// So scatter runs on several independent lattices -- one per SIZE CLASS -- and
// the size class is a property of the LATTICE, not of the species drawn on it.
// That is the second half of what makes the bound cheap: a layer's maximum
// height is a compile-time property of the layer, so the bound never has to
// know which species a site resolved to, only that the layer has a site at
// all. Which in turn means the bound can stop scanning a layer the instant it
// finds its first site (see assetTopAboveSurfaceMm) -- the dense layers, which
// have the most cells, are precisely the ones that answer on the first cell.

#include <cstdint>
#include <vector>

#include "voxelcore/core.h"
#include "voxelcore/hash.h"

namespace vxc {

// Four size classes. Fixed rather than open-ended because each one reserves a
// hash channel per draw (hash.h's CH_ASSET_* blocks reserve exactly this many)
// and because the number is a world-shape decision, not a tuning knob.
inline constexpr int kAssetLayerCount = 4;

// One scatter lattice.
//
// Every distance is in millimetres and every draw is integer: voxel-core's CI
// bans `float` and `double` as types under include/ and src/ outright, and
// placement is worldgen -- it must give the same answer on every machine and,
// eventually, in HLSL.
struct AssetLayer {
    // Lattice pitch. One candidate site per cell, so this is the minimum
    // possible spacing between two assets of this class and roughly their
    // mean spacing at full density.
    int32_t cellMm = 32000;

    // The tallest any asset on this layer may reach ABOVE the ground voxel its
    // instance is anchored to. This is the number the bound is made of, so it
    // is a contract with the bake step, not a hint: baking an asset taller
    // than its layer's maxHeightMm puts a hole in the world at the top of that
    // asset, silently. `assetLayerAdmitsHeight` exists to check it at bake and
    // load time rather than discovering it in a screenshot.
    int32_t maxHeightMm = 30000;

    // The deepest it may reach BELOW that anchor (roots, buried rock, rubble
    // skirts). Mirrors maxHeightMm for the all-solid bound, which has the
    // opposite polarity and the same failure mode.
    int32_t maxDepthMm = 2000;

    // Horizontal reach from the anchor, on either axis. This is what a query
    // rect must be dilated by before enumerating cells: an instance anchored
    // OUTSIDE the rect can still put voxels inside it, and forgetting that
    // dilation is the same bug the cavern layer already paid for once
    // (amplifier.h:412-420, kCavernMaxReachMm).
    int32_t maxRadiusMm = 15000;

    // Chance in 1000 that a given cell carries a site at all, BEFORE the
    // caller's biome veto. Density beyond this is the policy's business.
    uint16_t densityPerMille = 1000;

    // How many baked seeds this layer's species banks carry. The pick draw is
    // reduced modulo this, so 0 is treated as 1.
    uint16_t seedCount = 64;

    // DOES THIS LAYER PUT VOXELS IN THE WORLD GRID?
    //
    // True for trees and rocks: forge/kinds.py:34-42 records that they "JOIN
    // THE WORLD'S OWN VOXEL GRID and are destructible exactly as terrain is",
    // which forces them to 10 cm = kVoxelSizeMm and nothing else. Those are
    // the assets that are solid above the surface, and they are the entire
    // reason the bounds in this file exist.
    //
    // False for everything else -- ground cover, bushes and every animal.
    // forge/kinds.py:44-51: "they never enter the terrain grid: they carry
    // their own voxel grid and their own transform". AssetGrid::onTerrainLattice
    // (assetgrid.h:162) is the per-asset twin of this flag.
    //
    // THE BOUNDS SKIP LAYERS WHERE THIS IS FALSE, AND THAT IS THE DANGEROUS
    // DIRECTION. Every other switch in this file makes the bound larger or
    // leaves it alone; this one makes it smaller, which is the polarity that
    // puts holes in worlds. Three things make it sound rather than merely
    // plausible:
    //
    //   1. THE DEFAULT IS TRUE. A layer that never mentions the field pays the
    //      full bound. Forgetting is the safe direction.
    //   2. assetLayerAdmitsVoxelSize refuses a mismatch at bake and load time,
    //      so a detail layer provably holds nothing on the world lattice. That
    //      is the premise the skip rests on, and it is checked rather than
    //      assumed.
    //   3. tests/test_assetplacement.cpp drives it in BOTH directions -- a
    //      detail layer contributing exactly zero, and the same layer flipped
    //      back to terrain contributing again. A gate exercised only in the
    //      easy direction is what tilestreaming.h:173-188 is a postmortem of.
    //
    // What it buys is not tidiness. The bound's early-out means the DENSEST
    // layer answers first, so with ground cover on a terrain layer every
    // footprint on the planet takes the grass layer's maxHeightMm -- and a few
    // hundred mm straddles a level-0 chunk boundary often enough to admit an
    // extra 32-voxel chunk layer over a large fraction of the world, to carry
    // geometry that is not in the voxel grid at all.
    bool terrainLattice = true;
};

// The lattice a layer expects an asset to be baked at, in mm. Terrain layers
// admit exactly kVoxelSizeMm because the world grid has exactly one cell size;
// detail layers admit anything BUT that, so that a 10 cm asset cannot be filed
// somewhere the bound will not account for it.
//
// `voxelSizeMm` is AssetGrid::voxelSizeMm() -- the size the asset was actually
// baked at, read out of the VXA header, not the size anybody believes it was.
inline bool assetLayerAdmitsVoxelSize(const AssetLayer& layer, uint32_t voxelSizeMm) {
    if (voxelSizeMm == 0) return false;
    return layer.terrainLattice ? (voxelSizeMm == uint32_t(kVoxelSizeMm))
                                : (voxelSizeMm != uint32_t(kVoxelSizeMm));
}

// A candidate placement, before any policy has looked at it.
//
// Deliberately carries no species: choosing a species needs biome, and the
// whole point of the split is that this half is computable without one. The
// policy turns a site into an instance by choosing a species (within the
// layer's declared size class) or by rejecting it.
struct AssetSite {
    int64_t cellX = 0, cellY = 0;
    int64_t anchorXMm = 0, anchorYMm = 0; // jittered position within the cell
    int32_t layer = 0;
    uint16_t seedIndex = 0; // which entry of the species bank
    uint8_t yawQuarter = 0; // 0..3 quarter turns about +z

    friend bool operator==(const AssetSite&, const AssetSite&) = default;
};

// A rectangle of the level-0 voxel lattice, inclusive on both ends -- the same
// convention Amplifier::surfaceUpperBoundMm and solidBelowBoundMm take, so the
// two compose without a unit or half-openness conversion at the call site.
// (tilestreaming.h's rects are half-open world mm; these are not those, and
// mixing them is the sort of off-by-one that produces a hole one chunk wide.)
struct AssetVoxelRect {
    int64_t vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
    bool valid() const { return vx1 >= vx0 && vy1 >= vy0; }
};

// --- the draws --------------------------------------------------------------

// Whether cell (cx, cy) of `layer` carries a site, and where. Pure: one hash
// for occupancy, one for the jitter, one for the bank pick and yaw.
inline bool assetSiteInCell(uint64_t seed, const AssetLayer& layer, int32_t layerIndex, int64_t cx,
                            int64_t cy, AssetSite& out) {
    if (layer.cellMm <= 0 || layer.densityPerMille == 0) return false;
    const uint32_t li = static_cast<uint32_t>(layerIndex & (kAssetLayerCount - 1));

    const uint64_t occ = hash2(seed, cx, cy, CH_ASSET_SITE + li);
    // Top 16 bits scaled to per-mille. Using the TOP bits rather than a
    // modulo of the whole word keeps this identical in shape to
    // hashToSigned16, which every other draw in the amplifier uses -- and a
    // modulo of a splitmix64 output by 1000 is fine statistically but is one
    // more thing to have to mirror exactly in HLSL later.
    const uint64_t occDraw = (occ >> 48) * 1000u >> 16;
    if (occDraw >= uint64_t(layer.densityPerMille)) return false;

    const uint64_t jit = hash2(seed, cx, cy, CH_ASSET_JITTER + li);
    const int64_t cellMm = int64_t(layer.cellMm);
    // Two independent 16-bit fields of one hash, not two hashes: the jitter is
    // asked of every cell that carries a site, and halving that cost matters
    // more here than anywhere else in the file.
    const int64_t jx = int64_t((jit >> 48) & 0xffffu) * cellMm >> 16;
    const int64_t jy = int64_t((jit >> 32) & 0xffffu) * cellMm >> 16;

    const uint64_t pick = hash2(seed, cx, cy, CH_ASSET_PICK + li);
    const uint16_t seeds = layer.seedCount == 0 ? uint16_t(1) : layer.seedCount;

    out.cellX = cx;
    out.cellY = cy;
    out.anchorXMm = cx * cellMm + jx;
    out.anchorYMm = cy * cellMm + jy;
    out.layer = layerIndex;
    out.seedIndex = static_cast<uint16_t>((pick >> 16) % uint64_t(seeds));
    out.yawQuarter = static_cast<uint8_t>(pick & 3u);
    return true;
}

// Every site of every layer that can put a voxel inside `rect`. Row-major by
// layer then cell, no duplicates, deterministic order.
//
// This is the GENERATION query -- what a chunk asks to find out which assets
// intersect it. It is not on the admission path and it may cost more than the
// bound does. It still costs only hashes.
inline std::vector<AssetSite> assetSitesForRect(uint64_t seed, const AssetLayer* layers,
                                                int layerCount, const AssetVoxelRect& rect) {
    std::vector<AssetSite> out;
    if (!rect.valid() || layers == nullptr) return out;
    const int64_t vs = int64_t(kVoxelSizeMm);
    // The rect's own world-mm span, inclusive: voxel vx covers
    // [vx*vs, vx*vs + vs).
    const int64_t x0 = rect.vx0 * vs, x1 = rect.vx1 * vs + vs - 1;
    const int64_t y0 = rect.vy0 * vs, y1 = rect.vy1 * vs + vs - 1;

    for (int li = 0; li < layerCount && li < kAssetLayerCount; ++li) {
        const AssetLayer& L = layers[li];
        if (L.cellMm <= 0) continue;
        const int64_t r = int64_t(L.maxRadiusMm);
        const int64_t cx0 = floorDiv(x0 - r, int64_t(L.cellMm));
        const int64_t cx1 = floorDiv(x1 + r, int64_t(L.cellMm));
        const int64_t cy0 = floorDiv(y0 - r, int64_t(L.cellMm));
        const int64_t cy1 = floorDiv(y1 + r, int64_t(L.cellMm));
        for (int64_t cy = cy0; cy <= cy1; ++cy) {
            for (int64_t cx = cx0; cx <= cx1; ++cx) {
                AssetSite s;
                if (!assetSiteInCell(seed, L, li, cx, cy, s)) continue;
                // Cell coverage is conservative (a whole cell is visited if any
                // part of its dilated footprint touches the rect); the site's
                // OWN reach is then tested exactly, so a site whose jitter
                // carried it away from the rect is dropped here rather than
                // handed to the caller to re-test.
                if (s.anchorXMm + r < x0 || s.anchorXMm - r > x1) continue;
                if (s.anchorYMm + r < y0 || s.anchorYMm - r > y1) continue;
                out.push_back(s);
            }
        }
    }
    return out;
}

// --- the bounds -------------------------------------------------------------

// The largest height above its anchor's ground that any asset intersecting
// `rect` can reach, over every policy obeying the veto-only rule. 0 means no
// layer has a site anywhere that could reach this rect, i.e. the terrain
// bounds stand unmodified.
//
// EARLY-OUT, and it is what makes this affordable. A layer's contribution is
// its maxHeightMm or nothing -- it does not depend on WHICH cell carried the
// site -- so the scan stops at the first site found in each layer. The dense
// layers (ground cover, hundreds of cells over a chunk footprint) answer on
// the first or second cell; the sparse layers (emergents, a handful of cells)
// are cheap because there are a handful of cells. There is no configuration in
// which this walks many cells AND finds nothing, except genuinely empty
// terrain at low density, which is the case that also does the least work per
// cell (one hash, no jitter).
inline int32_t assetTopAboveSurfaceMm(uint64_t seed, const AssetLayer* layers, int layerCount,
                                      const AssetVoxelRect& rect) {
    if (!rect.valid() || layers == nullptr) return 0;
    const int64_t vs = int64_t(kVoxelSizeMm);
    const int64_t x0 = rect.vx0 * vs, x1 = rect.vx1 * vs + vs - 1;
    const int64_t y0 = rect.vy0 * vs, y1 = rect.vy1 * vs + vs - 1;

    int32_t top = 0;
    for (int li = 0; li < layerCount && li < kAssetLayerCount; ++li) {
        const AssetLayer& L = layers[li];
        // A detail layer is not in the world grid, so it cannot break the
        // all-air proof and must not widen it. See AssetLayer::terrainLattice
        // for why this skip is the dangerous direction and what guards it.
        if (!L.terrainLattice) continue;
        if (L.cellMm <= 0 || L.maxHeightMm <= top) continue; // cannot raise the answer
        const int64_t r = int64_t(L.maxRadiusMm);
        const int64_t cx0 = floorDiv(x0 - r, int64_t(L.cellMm));
        const int64_t cx1 = floorDiv(x1 + r, int64_t(L.cellMm));
        const int64_t cy0 = floorDiv(y0 - r, int64_t(L.cellMm));
        const int64_t cy1 = floorDiv(y1 + r, int64_t(L.cellMm));
        bool found = false;
        for (int64_t cy = cy0; cy <= cy1 && !found; ++cy) {
            for (int64_t cx = cx0; cx <= cx1; ++cx) {
                AssetSite s;
                if (!assetSiteInCell(seed, L, li, cx, cy, s)) continue;
                if (s.anchorXMm + r < x0 || s.anchorXMm - r > x1) continue;
                if (s.anchorYMm + r < y0 || s.anchorYMm - r > y1) continue;
                found = true;
                break;
            }
        }
        if (found) top = L.maxHeightMm;
    }
    return top;
}

// The mirror, for the all-solid floor bound. Same shape, opposite polarity.
inline int32_t assetBottomBelowSurfaceMm(uint64_t seed, const AssetLayer* layers, int layerCount,
                                         const AssetVoxelRect& rect) {
    if (!rect.valid() || layers == nullptr) return 0;
    const int64_t vs = int64_t(kVoxelSizeMm);
    const int64_t x0 = rect.vx0 * vs, x1 = rect.vx1 * vs + vs - 1;
    const int64_t y0 = rect.vy0 * vs, y1 = rect.vy1 * vs + vs - 1;

    int32_t depth = 0;
    for (int li = 0; li < layerCount && li < kAssetLayerCount; ++li) {
        const AssetLayer& L = layers[li];
        if (!L.terrainLattice) continue; // not in the world grid; see above
        if (L.cellMm <= 0 || L.maxDepthMm <= depth) continue;
        const int64_t r = int64_t(L.maxRadiusMm);
        const int64_t cx0 = floorDiv(x0 - r, int64_t(L.cellMm));
        const int64_t cx1 = floorDiv(x1 + r, int64_t(L.cellMm));
        const int64_t cy0 = floorDiv(y0 - r, int64_t(L.cellMm));
        const int64_t cy1 = floorDiv(y1 + r, int64_t(L.cellMm));
        bool found = false;
        for (int64_t cy = cy0; cy <= cy1 && !found; ++cy) {
            for (int64_t cx = cx0; cx <= cx1; ++cx) {
                AssetSite s;
                if (!assetSiteInCell(seed, L, li, cx, cy, s)) continue;
                if (s.anchorXMm + r < x0 || s.anchorXMm - r > x1) continue;
                if (s.anchorYMm + r < y0 || s.anchorYMm - r > y1) continue;
                found = true;
                break;
            }
        }
        if (found) depth = L.maxDepthMm;
    }
    return depth;
}

// The widest horizontal reach in a layer table -- the dilation a composed
// surface bound must apply before it may be added to. Exposed because the
// caller has to dilate the rect it hands to the TERRAIN bound too, and doing
// that with a second, differently-derived constant is exactly how the cavern
// reach and the carrier stencil came to disagree once already
// (tilestreaming.h:190-197).
inline int32_t assetMaxReachMm(const AssetLayer* layers, int layerCount) {
    int32_t r = 0;
    if (layers == nullptr) return 0;
    for (int li = 0; li < layerCount && li < kAssetLayerCount; ++li) {
        // TERRAIN LAYERS ONLY, and this one is load-bearing rather than an
        // optimisation. This reach is the dilation assetAwareSurfaceUpperBoundMm
        // applies to the TERRAIN bound query, and the reason for that dilation
        // (assetplacement.h's note 1 on that function) is that an instance
        // anchored outside the rect stands on ground at its own anchor and
        // reaches in. A detail-lattice instance puts no voxel in the rect at
        // all, so it has nothing to reach in WITH, and dilating for it would
        // widen every bound query on the planet for geometry that is not in
        // the grid.
        if (!layers[li].terrainLattice) continue;
        if (layers[li].cellMm <= 0) continue;
        if (layers[li].maxRadiusMm > r) r = layers[li].maxRadiusMm;
    }
    return r;
}

// THE ONE FUNCTION A STREAMING GATE SHOULD CALL.
//
// Composes the terrain surface bound with the asset reach correctly, and there
// is deliberately no way to compose them incorrectly through this API. The two
// halves that a hand-rolled composition gets wrong, both silently:
//
//   1. The DILATION. An instance anchored outside `rect` reaches into it, and
//      its base sits on the ground at ITS OWN anchor -- which can be far above
//      the surface anywhere inside the rect. So the terrain bound must be
//      taken over the DILATED rectangle, not the original. This is precisely
//      the argument solidBelowBoundMm already makes for caverns
//      (amplifier.h:412-420) and it has the same answer.
//
//   2. The DECLINE. surfaceUpperBoundMm returns kSurfaceBoundDeclined when it
//      will not bound a footprint, and that sentinel is INT64_MAX -- adding an
//      asset height to it wraps to a negative number, which reads as "this
//      chunk is provably air" for the entire sky. A decline must propagate as
//      a decline. Never claim air on no information.
//
// `surfaceUpperBoundMm` is any callable with the signature of
// Amplifier::surfaceUpperBoundMm (int64_t (int64_t, int64_t, int64_t,
// int64_t)). Templated rather than taking an Amplifier so this stays testable
// against a synthetic surface -- the failure being guarded here is a hole in
// the world, and a bound that can only be exercised through a full amplifier
// on real terrain is a bound that will only ever be tested in the easy
// direction (tilestreaming.h:173-188 records what that cost last time).
template <typename SurfaceUpperBoundFn>
int64_t assetAwareSurfaceUpperBoundMm(uint64_t seed, const AssetLayer* layers, int layerCount,
                                      const AssetVoxelRect& rect,
                                      const SurfaceUpperBoundFn& surfaceUpperBoundMm) {
    const int32_t reachMm = assetMaxReachMm(layers, layerCount);
    const int64_t reachVox = int64_t(reachMm) / int64_t(kVoxelSizeMm) + 1; // round outward
    const int64_t bound = surfaceUpperBoundMm(rect.vx0 - reachVox, rect.vy0 - reachVox,
                                              rect.vx1 + reachVox, rect.vy1 + reachVox);
    if (bound == kSurfaceBoundDeclined) return kSurfaceBoundDeclined;
    return bound + int64_t(assetTopAboveSurfaceMm(seed, layers, layerCount, rect));
}

// A bake/load-time check that an asset actually fits the layer it is filed
// under. `heightVox` is the asset's own extent above its anchor in level-0
// voxels (originZ + sizeZ for a grid whose base is at z = 0).
//
// Exists so that "this tree is taller than its layer says" is caught by the
// tool that files it, rather than by a player noticing that tall trees have
// flat tops -- which is what the failure looks like, and which reads as a
// generator bug rather than a bounds bug.
inline bool assetLayerAdmitsHeight(const AssetLayer& layer, int64_t heightVox) {
    return heightVox * int64_t(kVoxelSizeMm) <= int64_t(layer.maxHeightMm);
}

} // namespace vxc
