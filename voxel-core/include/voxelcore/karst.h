#pragma once
// KARST CONDUITS -- the carve that replaces caves.h and caverns.h.
//
// WHAT THIS FILE IS AND IS NOT. It is the CLIENT half: given a table of conduit
// nodes and edges (produced by the bake, `docs/karst-phase1-carve.md`), it
// reduces them to a per-column segment list and answers "is this voxel air".
// It contains no generator. Where the conduits COME from -- Dijkstra from sinks
// to springs over an anisotropic geological cost -- is a global computation over
// tens of kilometres that cannot live in a per-column shader, which is precisely
// why the network is baked and this file only consumes it.
//
// THE SPLIT, and it is the same one caves.h used because it is the one that
// works: everything expensive happens ONCE PER COLUMN (which segments are near,
// how near), and the per-voxel test is then two integer compares. A 400 m column
// is 4,000 voxels; anything done per voxel is done 4,000 times.
//
// -- DECISIONS SETTLED BY THE PHASE 0 PROTOTYPE, NOT RE-OPENED HERE ----------
//
// HARD UNION, NO FILLETS. The reference method blends its conduits with a
// smooth-min, which has no early-out and no bounded primitive count. Measured on
// sinuous conduits at 5 m subdivision, a hard union matches smooth-min to
// IoU 0.971 -- adjacent capsules overlap so heavily that their union is smooth
// by construction. The crease that motivated a fillet mitigation was an artefact
// of 217 m segments meeting at sharp angles. So: `min` over segments, with an
// early-out, and no extra primitive class.
//
// ABSOLUTE Z, NOT DEPTH BELOW SURFACE. Conduits are anchored to the water table
// and to inception horizons, and both are absolute surfaces. A depth-draped
// conduit climbs over every ridge it passes under. caverns.h already made this
// choice for the same reason (amplifier.h:117-119); caves.h did not, and its
// tunnels drape.
//
// THE SEGMENT CAP IS THE ENCODER'S OBLIGATION, NOT A HOPE. caves.h's
// kMaxCaveSegs truncates silently (caves.h:450) and rests on a test measuring
// that it never binds -- which holds only because a regular lattice has bounded
// degree. An irregular routed network has no such bound. The bake must merge
// near-parallel overlaps to a fixed point and REFUSE a tile it cannot fit;
// `karstColumnFits` below is what it checks against, and `KarstColumn::overflow`
// is what a client sees if that contract is ever broken.

#include <cstdint>

#include "voxelcore/core.h"

namespace vxc {

// ---------------------------------------------------------------------------
// Wire types. These mirror the baked table; the decoder hands over views of
// them and this file never allocates.
// ---------------------------------------------------------------------------

//: Cross-section families. The kind decides how a section is SHAPED, not how
//: big it is -- size is the radii. Each is a different answer to "where was the
//: water table when this formed", which is why the set is closed and small.
enum KarstKind : uint8_t {
    KARST_PHREATIC = 0,   // formed full of water: circular
    KARST_VADOSE = 1,     // free-surface stream cutting down: tall and narrow
    KARST_KEYHOLE = 2,    // a drained tube with a vadose notch in its floor
    KARST_EPIPHREATIC = 3,// at the table: wide and low
    KARST_CHAMBER = 4,    // breakdown-widened
    KARST_SHAFT = 5,      // vertical, joint-guided
    KARST_ENTRANCE = 6,   // reaches daylight: exempt from the roof guard
    KARST_KIND_COUNT = 7,
};

struct KarstNode {
    int32_t xMm = 0, yMm = 0, zMm = 0;  // ABSOLUTE world position
    uint16_t rHorizCm = 0;              // horizontal semi-axis, to 655 m
    uint16_t rVertCm = 0;               // vertical semi-axis
    uint8_t kind = KARST_PHREATIC;
    uint8_t tier = 0;                   // base-level epoch, 0 = lowest/youngest
    uint16_t reserved = 0;
};

struct KarstEdge {
    uint32_t n0 = 0, n1 = 0;
};

//: A borrowed view of one tile's decoded conduit table. No ownership, no
//: allocation: the decoder owns the arrays and this is what the carve reads.
struct KarstTable {
    const KarstNode* nodes = nullptr;
    const KarstEdge* edges = nullptr;
    int32_t nodeCount = 0;
    int32_t edgeCount = 0;

    constexpr bool empty() const { return edgeCount <= 0 || nodes == nullptr; }
};

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

//: How many conduits may overlap one column. EIGHT, and the number is a
//: contract with the bake rather than a guess: the encoder rasterises segment
//: footprints and merges until no column exceeds it. Raising this widens
//: ColumnSample and the GPU mirror's struct, so it is not a free knob.
inline constexpr int32_t kMaxKarstSegs = 8;

//: Never carve within this of the surface. An ENTRANCE segment is the single
//: deliberate exception -- that is what an entrance IS -- and it is exempted by
//: kind rather than by a separate code path, so there is exactly one place where
//: daylight can happen.
inline constexpr int32_t kKarstRoofMinMm = 4000;

//: Stop this far above the bedrock floor. The third of three independent
//: bedrock guards, the others being materialAt's MAT_BEDROCK early-out and the
//: bake's own depth band. Any one of them alone is sufficient; all three exist
//: so a mistuned constant cannot punch a hole in the world's floor.
inline constexpr int32_t kKarstBedrockMarginMm = 2000;

//: Never carve at or below sea level: the implicit ocean owns everything down
//: there, so a void would be water, not a cave. Same rule and the same reason as
//: caves.h's kCaveMinVoxelZ, and it makes "caves cannot flood from the ocean" a
//: property of the definition rather than something to test for.
inline constexpr int64_t kKarstMinVoxelZ = kSeaLevelVoxelZ;

//: The largest horizontal reach any conduit can have, in mm. Used to size the
//: query window and to bound the streaming footprint. 9 m radius is the owner's
//: 5x hall; the margin covers wall roughness applied at read time.
inline constexpr int32_t kKarstMaxReachMm = 12000;

// ---------------------------------------------------------------------------
// Per-column reduction
// ---------------------------------------------------------------------------

//: One conduit passing near this column, reduced to what the per-voxel test
//: needs. `marginSq` is the SQUARED vertical half-extent of the ellipse at this
//: column's horizontal offset, so the test is `dz*dz < marginSq` and nothing
//: else. Deriving it once per column is the whole point of the split.
struct KarstSeg {
    int32_t marginSq = 0;      // > 0 for a recorded segment
    int32_t axisZMm = 0;       // ABSOLUTE z of the axis at this column
    int32_t floorDropMm = 0;   // keyhole: how far the notch cuts below the axis
    //: The segment's vertical semi-axis. Carried so the column band can be
    //: bounded WITHOUT a square root: marginSq <= rVert^2 always, so rVert is a
    //: conservative half-extent. voxel-core has no integer sqrt and the existing
    //: carve passes deliberately never need one -- they stay in squared space
    //: for the test and use a linear radius for bounds, which is what this does.
    int32_t rVertMm = 0;
    uint8_t kind = KARST_PHREATIC;
};

struct KarstColumn {
    int32_t count = 0;
    //: The column's carve band, absolute mm. Two compares kill most of a 400 m
    //: column before any segment is examined -- which depth-space caves cannot
    //: do, because their band moves with the surface.
    int32_t minZMm = 0;
    int32_t maxZMm = 0;
    //: True if the bake handed us more overlapping conduits than the cap. The
    //: carve stays CORRECT (it simply carves fewer), but the tile violated its
    //: contract and something should say so loudly rather than silently drop
    //: geometry the way caves.h does.
    bool overflow = false;
    KarstSeg segs[kMaxKarstSegs] = {};
};

//: Squared distance from (px,py) to the segment (ax,ay)-(bx,by), and the
//: interpolation parameter, both in fixed point. Integer-only: the parameter is
//: returned as a numerator/denominator pair so the caller can interpolate the
//: axis height and the radii without a division that loses the low bits.
struct KarstFoot {
    int64_t distSq = 0;
    int64_t tNum = 0;   // 0 <= tNum <= tDen
    int64_t tDen = 1;
};

constexpr KarstFoot karstFoot(int64_t px, int64_t py, int64_t ax, int64_t ay,
                              int64_t bx, int64_t by) {
    const int64_t abx = bx - ax, aby = by - ay;
    const int64_t apx = px - ax, apy = py - ay;
    const int64_t den = abx * abx + aby * aby;
    if (den <= 0) {
        return KarstFoot{apx * apx + apy * apy, 0, 1};
    }
    int64_t num = apx * abx + apy * aby;
    if (num < 0) num = 0;
    if (num > den) num = den;
    // Foot of the perpendicular, kept in the den-scaled space so the subtraction
    // is exact: (ap - ab * num/den) * den == ap*den - ab*num.
    const int64_t fx = apx * den - abx * num;
    const int64_t fy = apy * den - aby * num;
    // distSq = (fx^2 + fy^2) / den^2, computed as a rounded division so the
    // result stays in mm^2 rather than mm^2 * den^2.
    const int64_t sq = (fx / den) * (fx / den) + (fy / den) * (fy / den);
    return KarstFoot{sq, num, den};
}

//: Linear interpolation between two int32 endpoints at t = num/den, in integers.
constexpr int64_t karstLerp(int64_t a, int64_t b, int64_t num, int64_t den) {
    return a + ((b - a) * num) / den;
}

//: Reduce one edge against one column. Returns false when the conduit does not
//: reach this column at all, which is the overwhelmingly common case.
constexpr bool karstSegForColumn(const KarstTable& t, int32_t edgeIndex,
                                 int64_t vx, int64_t vy, KarstSeg& out) {
    const KarstEdge& e = t.edges[edgeIndex];
    const KarstNode& a = t.nodes[e.n0];
    const KarstNode& b = t.nodes[e.n1];

    const int64_t px = vx * int64_t(kVoxelSizeMm) + int64_t(kVoxelSizeMm) / 2;
    const int64_t py = vy * int64_t(kVoxelSizeMm) + int64_t(kVoxelSizeMm) / 2;
    const KarstFoot f = karstFoot(px, py, a.xMm, a.yMm, b.xMm, b.yMm);

    const int64_t rH = karstLerp(int64_t(a.rHorizCm) * 10, int64_t(b.rHorizCm) * 10,
                                 f.tNum, f.tDen);
    if (rH <= 0 || f.distSq >= rH * rH) return false;

    const int64_t rV = karstLerp(int64_t(a.rVertCm) * 10, int64_t(b.rVertCm) * 10,
                                 f.tNum, f.tDen);
    if (rV <= 0) return false;

    // Ellipse: at horizontal offset d the vertical half-extent h satisfies
    // (d/rH)^2 + (h/rV)^2 = 1, so h^2 = rV^2 * (rH^2 - d^2) / rH^2. Ordered to
    // keep the numerator inside int64: rV and rH are at most 12,000 mm, so
    // rV^2 * (rH^2 - d^2) peaks near 1.4e8 * 1.4e8 = 2.1e16, which fits.
    const int64_t marginSq = (rV * rV) * (rH * rH - f.distSq) / (rH * rH);
    if (marginSq <= 0) return false;

    out.marginSq = clampi32(marginSq, 0, INT32_MAX);
    out.rVertMm = clampi32(rV, 0, INT32_MAX);
    out.axisZMm = clampi32(karstLerp(a.zMm, b.zMm, f.tNum, f.tDen), INT32_MIN, INT32_MAX);
    out.kind = a.kind;
    out.floorDropMm = (a.kind == KARST_KEYHOLE)
                          ? clampi32(rV * 3 / 2, 0, INT32_MAX)
                          : 0;
    return true;
}

//: Reduce every conduit near this column. `karstColumnFor` is the contract form
//: the HLSL mirror is written against: a pure function of the table and the
//: column, with a fixed iteration order.
//
//: THE ITERATION ORDER IS PART OF THE WORLDGEN CONTRACT. Segments are visited
//: in edge-index order and appended in that order, so two implementations that
//: disagree about the order disagree about which segments survive an overflow.
constexpr KarstColumn karstColumnFor(const KarstTable& t, int64_t vx, int64_t vy) {
    KarstColumn col;
    if (t.empty()) return col;

    int32_t lo = INT32_MAX, hi = INT32_MIN;
    for (int32_t i = 0; i < t.edgeCount; ++i) {
        KarstSeg s;
        if (!karstSegForColumn(t, i, vx, vy, s)) continue;
        if (col.count >= kMaxKarstSegs) {
            col.overflow = true;
            continue;
        }
        col.segs[col.count++] = s;
        // Conservative band: rVert >= the true half-extent at this column, so
        // widening by it can only ADMIT voxels the per-voxel test then rejects.
        // Over-admitting costs a compare; under-admitting is a hole in a cave.
        const int32_t segLo = s.axisZMm - s.rVertMm - s.floorDropMm;
        const int32_t segHi = s.axisZMm + s.rVertMm;
        if (segLo < lo) lo = segLo;
        if (segHi > hi) hi = segHi;
    }
    if (col.count > 0) {
        col.minZMm = lo;
        col.maxZMm = hi;
    }
    return col;
}

//: Does this column fit the cap? The bake calls this while merging; a tile that
//: cannot be made to satisfy it is REFUSED rather than shipped with silently
//: dropped conduits.
constexpr bool karstColumnFits(const KarstColumn& col) { return !col.overflow; }

// ---------------------------------------------------------------------------
// Per-voxel carve
// ---------------------------------------------------------------------------

//: True if voxel (.., vz) of this column should become MAT_AIR.
//:
//: Guard order is cheapest and most restrictive first, and each guard is an
//: INDEPENDENT reason to refuse -- none is load-bearing for the others, so a
//: retuned constant degrades the shape rather than breaching the world.
constexpr bool karstCarveAt(const KarstColumn& col, int32_t surfaceMm,
                            int32_t bedrockDepthMm, int64_t vz) {
    if (col.count == 0) return false;

    const int64_t centreMm = vz * int64_t(kVoxelSizeMm) + int64_t(kVoxelSizeMm) / 2;

    // The column band: two compares, and they reject most of a deep column
    // before any segment is touched.
    if (centreMm < col.minZMm || centreMm > col.maxZMm) return false;

    // Sea level. The implicit ocean owns everything below; a void there is
    // water, not a cave.
    if (vz < kKarstMinVoxelZ) return false;

    // Bedrock floor.
    const int64_t bedrockTopMm = int64_t(surfaceMm) - bedrockDepthMm;
    if (centreMm <= bedrockTopMm + kKarstBedrockMarginMm) return false;

    const int64_t depthMm = int64_t(surfaceMm) - centreMm;

    for (int32_t i = 0; i < col.count; ++i) {
        const KarstSeg& s = col.segs[i];
        // Roof. An ENTRANCE is the one exemption, which is what an entrance is.
        if (depthMm < kKarstRoofMinMm && s.kind != KARST_ENTRANCE) continue;

        const int64_t dz = centreMm - s.axisZMm;
        if (dz * dz < s.marginSq) return true;

        // Keyhole: a vadose notch incised into the tube's floor. One extra band
        // test, and it is the most recognisable cave cross-section there is.
        if (s.floorDropMm > 0 && dz < 0 && -dz <= s.floorDropMm) {
            // The notch is narrow: a quarter of the tube's horizontal extent.
            if (s.marginSq > 0 && (dz * dz) / 16 < s.marginSq) return true;
        }
    }
    return false;
}

//: The deepest a conduit can reach below a column's surface, for the streaming
//: bound. Conservative by construction: it is the column's own band, which the
//: reduction already computed.
constexpr int64_t karstDeepestAirMm(const KarstColumn& col, int32_t surfaceMm) {
    if (col.count == 0) return 0;
    const int64_t d = int64_t(surfaceMm) - col.minZMm;
    return d > 0 ? d : 0;
}

}  // namespace vxc
