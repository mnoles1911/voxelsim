#pragma once
// ---------------------------------------------------------------------------
// WHERE A 51 m WINDOW'S WATER COMES IN: springs, and the river at the boundary
// ---------------------------------------------------------------------------
//
// THE PROBLEM THIS FILE EXISTS FOR, stated as the owner stated it. The bv24
// SECTION_HEADWATERS table is the top of every drawn reach FRAGMENT, not the
// set of springs: tile (-4,-4) alone ships 57,157 of them, minimum Q 552,537
// m^3/yr and median 1.9 million. Hang a faucet on each and the world grows
// 229k emitters that draw as SOLID LINES of water down every valley. What the
// world wants is RARE faucets at specific points in the inner gullies, with
// the water running down from them the way water does.
//
// Two rules, and they answer two different questions:
//
//   1. selectSprings -- WHICH HEADS ARE SPRINGS. A spring is a FIRST-ORDER
//      origin: its discharge is within a small factor of the smallest
//      discharge any head in its tile carries. A head at the median 1.9e6 is
//      not a spring, it is a place the bake's raster mask happened to break a
//      reach into fragments; a head at 5.5e5 next to a tile whose minimum is
//      5.5e5 is the top of a gully. Then a minimum SPACING, keeping the
//      LOWEST-Q head of each neighbourhood, because a cluster of first-order
//      heads 4 m apart is one spring drawn several times.
//
//   2. selectRiverCrossings -- WHERE THE RIVER ENTERS. A window sitting in the
//      middle of a river has no spring in it AND MUST NOT INVENT ONE. Its
//      water arrives across the boundary, so the boundary is where it is
//      emitted: walk the four faces of the box in fine pixels, find the drawn
//      channel crossing them inbound, and emit there. This is the honest
//      answer to the case rivernet.h's `dropRimHeadwaters` could only refuse
//      (see its comment: it makes a box a river merely passes through report
//      NO heads, which is right and also leaves the box dry).
//
// BOTH ARE PURE FUNCTIONS OF DATA ALREADY ON THE WIRE and neither touches a
// tile decoder: the heads come in as a vector the caller read off the table,
// and the crossings come through rivernet.h's IBakedWaterSource -- the same
// interface buildFromBakedWater takes, for the same testability reason its
// comment gives. That is what lets the whole selection be unit-tested against
// a twenty-line synthetic fixture instead of a bake.
//
// DETERMINISM. Every tie in here is broken by a total order on integers
// ((q, py, px) for springs, (accum, surface, walk index) for crossings) and no
// step consults iteration order of a hash container. The same input vector
// gives the same output vector, on every platform, forever -- the property the
// faucet refresh depends on so a re-gather cannot make the water jump.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "voxelcore/core.h"
#include "voxelcore/rivernet.h"

namespace vxc {

// ---------------------------------------------------------------------------
// 1. SPRING SELECTION
// ---------------------------------------------------------------------------

// One candidate head, as the caller read it out of SECTION_HEADWATERS.
struct SpringHead {
    //: ABSOLUTE fine-tile pixel (tile origin already added). Absolute rather
    //: than tile-local because the spacing rule spans tiles and a tile-local
    //: coordinate would let two heads 2 m apart across a tile seam both win.
    int64_t px = 0, py = 0;
    //: Discharge at the head, m^3/yr (tilestore.h HeadEntry). 0 means UNRATED
    //: -- the pre-bv24 fallback carries no Q -- and is never read as "dry".
    uint32_t qM3PerYear = 0;
};

//: Default Q band: a head is a spring candidate iff its Q is at most TWICE the
//: smallest Q in the reference set. 2.0 is "first-order origins only": on tile
//: (-4,-4) the minimum is 552,537 and the median 1.9e6, so a factor of 2 keeps
//: the gully tops and rejects the fragment breaks that make up the bulk. Held
//: as a RATIONAL rather than a double so the comparison is exact integer
//: arithmetic and the selection cannot drift with the FPU.
inline constexpr int64_t kSpringQFactorNumDefault = 2;
inline constexpr int64_t kSpringQFactorDenDefault = 1;

//: Default minimum spacing between springs, in FINE PIXELS. 80 px at the
//: shipped 1875 mm pitch is 150 m -- comfortably wider than the 51.2 m fluid
//: window, so a window holds at most one spring, which is the visual the owner
//: asked for ("specific points", not a line of dots).
inline constexpr int64_t kSpringSpacingPxDefault = 80;

struct SpringParams {
    //: Q band, as a rational multiple of the reference minimum.
    int64_t qFactorNum = kSpringQFactorNumDefault;
    int64_t qFactorDen = kSpringQFactorDenDefault;

    //: The Q the band is measured against. 0 means DERIVE IT from the heads
    //: passed in.
    //:
    //: THE CALLER SHOULD PASS THE TILE MINIMUM, not leave this at 0, and the
    //: difference is the whole stability of the feature: a 51 m window holds
    //: ~0.6 heads, so a window-derived minimum is "the smallest of the one head
    //: I can see", which selects that head every time and makes every fragment
    //: break a spring. The tile minimum is a fixed datum the window slides over.
    int64_t referenceMinQ = 0;

    //: Minimum spacing in fine pixels. <= 1 disables the spacing pass.
    int64_t spacingPx = kSpringSpacingPxDefault;
};

struct SpringSelection {
    //: Indices into the input vector, in SELECTION ORDER -- ascending Q, then
    //: (py, px). Not input order, deliberately: the consumer caps its faucet
    //: list, and a cap that truncates this order keeps the smallest, most
    //: spring-like sources rather than the ones that happened to be baked
    //: first.
    std::vector<uint32_t> springs;

    //: Heads that passed the Q band (before spacing), and the ones the spacing
    //: pass then absorbed. candidates == springs.size() + suppressed.
    uint32_t candidates = 0;
    uint32_t suppressed = 0;

    //: The reference minimum actually used and the threshold derived from it,
    //: so a caller can print WHY a head was or was not a spring.
    int64_t minQ = 0;
    int64_t thresholdQ = 0;

    //: False when NO head in the input carried a Q. The Q band is then skipped
    //: entirely (an unrated set has no band to measure) and only the spacing
    //: pass runs, which is what keeps the pre-bv24 fallback usable through this
    //: same function instead of growing a second selection rule.
    bool rated = false;
};

// The spring rule. See the header comment.
//
// A head with qM3PerYear == 0 in an otherwise RATED set is skipped: it cannot
// be shown to be a first-order origin, and admitting an unrated head into a
// rule whose whole content is "smallest Q wins" would let it win by default.
inline SpringSelection selectSprings(const std::vector<SpringHead>& heads,
                                     const SpringParams& params) {
    SpringSelection out;
    if (heads.empty()) return out;

    // The reference minimum. Over the RATED heads only; see above.
    int64_t minQ = params.referenceMinQ;
    bool anyRated = false;
    for (const SpringHead& h : heads) {
        if (h.qM3PerYear != 0) anyRated = true;
    }
    if (minQ <= 0) {
        for (const SpringHead& h : heads) {
            if (h.qM3PerYear == 0) continue;
            const int64_t q = int64_t(h.qM3PerYear);
            if (minQ <= 0 || q < minQ) minQ = q;
        }
    }
    out.rated = anyRated && minQ > 0;
    out.minQ = out.rated ? minQ : 0;

    const int64_t den = params.qFactorDen > 0 ? params.qFactorDen : 1;
    const int64_t num = params.qFactorNum > 0 ? params.qFactorNum : 1;
    out.thresholdQ = out.rated ? (minQ * num) / den : 0;

    // Candidates, in the canonical selection order. Built as (q, py, px, idx)
    // keys and sorted by insertion into a vector + std::sort-free stable pass:
    // the set is small (a 51 m window holds single digits), so an O(n log n)
    // sort through a comparator on a plain struct is the clearest thing that
    // is also exact.
    struct Cand {
        int64_t q;
        int64_t py, px;
        uint32_t idx;
    };
    std::vector<Cand> cands;
    cands.reserve(heads.size());
    const uint32_t headCount = uint32_t(heads.size());
    for (uint32_t i = 0; i < headCount; ++i) {
        const SpringHead& h = heads[i];
        if (out.rated) {
            if (h.qM3PerYear == 0) continue;
            if (int64_t(h.qM3PerYear) > out.thresholdQ) continue;
        }
        cands.push_back(Cand{int64_t(h.qM3PerYear), h.py, h.px, i});
    }
    out.candidates = uint32_t(cands.size());
    if (cands.empty()) return out;

    // Insertion sort on the total order (q, py, px). The vector is tiny by
    // construction and this keeps the file free of <algorithm>'s introsort,
    // whose ordering of EQUAL elements is unspecified -- here there are no
    // equal elements, because (py, px) is unique per head.
    for (size_t i = 1; i < cands.size(); ++i) {
        Cand key = cands[i];
        size_t j = i;
        while (j > 0) {
            const Cand& p = cands[j - 1];
            const bool greater = (p.q != key.q)     ? (p.q > key.q)
                                 : (p.py != key.py) ? (p.py > key.py)
                                                    : (p.px > key.px);
            if (!greater) break;
            cands[j] = cands[j - 1];
            --j;
        }
        cands[j] = key;
    }

    const int64_t spacing = params.spacingPx;
    if (spacing <= 1) {
        out.springs.reserve(cands.size());
        for (const Cand& c : cands) out.springs.push_back(c.idx);
        return out;
    }

    // GREEDY, LOWEST Q FIRST, with a true distance test rather than a bare bin
    // membership test. Bins of exactly `spacing` make the search O(1) -- any
    // accepted spring within `spacing` of a candidate must lie in one of the 9
    // bins around it -- while the accept/reject decision itself stays the
    // honest "no two springs closer than spacingPx". A bin-only rule (one
    // spring per bin) is cheaper and was rejected: it puts two springs 2 px
    // apart across a bin boundary, which is exactly the cluster this pass
    // exists to collapse.
    //
    // THE SELECTION IS WINDOW-RELATIVE and that is a stated limit: the caller
    // passes the heads inside its box, so a spring suppressed by a neighbour
    // just outside the box will be selected once the box moves off that
    // neighbour. The Q band, which is anchored on the TILE minimum, is what
    // makes the result stable in the way that matters; the spacing pass is
    // there to break up clusters, and a cluster is inside one window by
    // definition.
    std::unordered_map<uint64_t, std::vector<uint32_t>> bins;
    const int64_t spacingSq = spacing * spacing;
    const auto binKey = [](int64_t bx, int64_t by) -> uint64_t {
        return (uint64_t(uint32_t(int32_t(bx))) << 32) | uint64_t(uint32_t(int32_t(by)));
    };
    for (const Cand& c : cands) {
        const int64_t bx = floorDiv(c.px, spacing);
        const int64_t by = floorDiv(c.py, spacing);
        bool blocked = false;
        for (int64_t dy = -1; dy <= 1 && !blocked; ++dy) {
            for (int64_t dx = -1; dx <= 1 && !blocked; ++dx) {
                auto it = bins.find(binKey(bx + dx, by + dy));
                if (it == bins.end()) continue;
                for (const uint32_t other : it->second) {
                    const int64_t ex = heads[other].px - c.px;
                    const int64_t ey = heads[other].py - c.py;
                    if (ex * ex + ey * ey < spacingSq) {
                        blocked = true;
                        break;
                    }
                }
            }
        }
        if (blocked) {
            ++out.suppressed;
            continue;
        }
        bins[binKey(bx, by)].push_back(c.idx);
        out.springs.push_back(c.idx);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 2. WINDOW-EDGE RIVER INFLOW
// ---------------------------------------------------------------------------
//
// THE DISCHARGE UNIT CHAIN, spelled out because it is the one thing in this
// file that is an ESTIMATE rather than a datum, and because a rate that is
// wrong by a decade is invisible on screen until the window either floods or
// trickles:
//
//   flow byte  --(rivernet.h flowAccumLog2)-->  L, the log2 bucket of upstream
//                                              catchment area in m^2
//   L          --(springAreaM2 below)-------->  A, m^2, at the bucket's
//                                              GEOMETRIC midpoint sqrt(2)*2^L
//   A          --(x runoff, mm/yr / 1000)---->  Q, m^3/yr
//   Q          --(fluidlifecycle.h)---------->  particles/s at the faucet
//
// STEP 2 REUSES buildFromBakedWater'S RULE, which is the instruction and also
// the right call: that builder's build-time `discharge` is this same log2
// bucket read as an area (rivernet.h item 4), so a crossing and a segment of
// the graph it feeds cannot disagree about how big the river is. The midpoint
// is GEOMETRIC, not arithmetic, because the pipeline measured the quantisation
// spread against that midpoint: Q ratio p5/p95 = 0.732/1.366 (pipeline.py, B6).
// So the bucket alone costs +/-40%.
//
// STEP 3 IS THE WEAK LINK AND IS FITTED, NOT ASSUMED. Turning an area into a
// discharge needs a coefficient that is not on the wire and, as the next block
// measures, is not physical either. `calibrateRunoff` below fits it from the
// heads nearest the box -- the only cells that carry both a Q and a flow byte
// -- and `kEdgeInflowRunoffMmPerYrDefault` is only the fallback for a box with
// too few heads near it to fit. Read that block before touching either.
//
// The consequence of getting it wrong is bounded, which is why an estimator
// this rough is acceptable at all: the emitted rate is throttled by the
// window's population backpressure long before the error matters, and the
// surplus is routed as scalar graph inflow rather than dropped. What must NOT
// happen is the estimate being mistaken for the bake's own Q. A head carries a
// MEASURED Q and a crossing carries this ESTIMATE, so every consumer that
// reports a rate has to keep the two apart -- which is what the edge-inflow
// flag on the emitter and the springs/edges split in the perf line are for.

//: Fallback runoff coefficient, mm/yr: the catchment mean pipeline.py's B6
//: comment measures at a river cell (577, against 247 of LOCAL runoff). It is
//: the honest physical number and it is 10.9x too LOW as an estimator here --
//: see calibrateRunoff for why, and prefer the fit.
inline constexpr int64_t kEdgeInflowRunoffMmPerYrDefault = 577;

//: The log2 bucket back to a catchment area in m^2 at its GEOMETRIC midpoint:
//: sqrt(2) * 2^L, with sqrt(2) as the integer ratio 181/128 (1.4141, the same
//: approximation channelStepLengthMm uses for a diagonal). SATURATES at the top
//: bucket rather than wrapping, for flowAccumM2's stated reason: a wrapped
//: negative area would make a trunk reach read as a headwater.
constexpr int64_t springAreaM2(uint8_t flowByte) {
    const int64_t log2 = int64_t(flowAccumLog2(flowByte));
    if (log2 >= 31) return int64_t(INT32_MAX) * 181 / 128;
    return (int64_t(1) << log2) * 181 / 128;
}

//: Area (m^2) x runoff (mm/yr) -> discharge (m^3/yr). One function so the
//: /1000 that turns millimetres into metres exists once.
constexpr int64_t springDischargeM3PerYear(int64_t areaM2, int64_t runoffMmPerYr) {
    if (areaM2 <= 0 || runoffMmPerYr <= 0) return 0;
    return areaM2 * runoffMmPerYr / 1000;
}

// ---------------------------------------------------------------------------
// CALIBRATING THE RUNOFF OFF THE TILE'S OWN HEADS
// ---------------------------------------------------------------------------
//
// MEASURED, not assumed, and the measurement is why this function exists.
// Scored against the 57,157 heads of tile (-4,-4) -- the only cells on the wire
// that carry BOTH a bake-computed Q and a flow byte, so the only place the
// estimator can be graded:
//
//   estimator                                    p50 ratio   within 2x
//   area x 577 mm/yr (the physical constant)     0.09        --
//   area x runoff fitted to that tile's heads    1.00        61%   (88% in 4x)
//   inverting the water plane's DEPTH law        0.03        2.7%
//
// The constant is 10.9x LOW and the depth inversion is worse still. Both have
// the same cause and it is worth writing down, because it will catch the next
// person too:
//
//   * THE FLOW PLANE'S ACCUMULATION IS MFD, THE DISCHARGE IS D8. pipeline.py's
//     B6 splits them deliberately -- `acc` stays multi-flow-direction because
//     stream-power incision reads it and pure D8 carves 45-degree channels,
//     while the water pass concentrates. So a channel cell's shipped
//     accumulation is a FRACTION of the catchment its Q was summed over, and
//     the fraction depends on how much the terrain above it fanned out. That is
//     a per-terrain factor, not a physical one, and no rainfall constant can
//     stand in for it.
//   * DEPTH IS FLOORED. The bake widens to WIDEN_MIN_DEPTH_M = 0.1 m, and the
//     median head sits exactly on that floor, so the depth of a small reach
//     carries no Q information at all -- and the depth law's exponent (1/0.3516
//     = 2.84) then amplifies whatever noise is left by nearly three orders.
//
// So the runoff coefficient here is a FITTED FUDGE FACTOR that absorbs the
// MFD/D8 ratio, and it must be fitted LOCALLY (the bv24 bake log's per-tile
// boundary inflow runs 8..918 mm/yr across six adjacent tiles). Fit it from the
// heads nearest the box, take the MEDIAN so one exotic head cannot move it, and
// say in the perf line when the fit did not happen.

struct RunoffCalibration {
    int64_t runoffMmPerYr = kEdgeInflowRunoffMmPerYrDefault;
    uint32_t samples = 0;   //: heads that resolved a flow byte and a positive Q
    bool calibrated = false; //: false == `fallbackMmPerYr` is in use
};

// Median of (q * 1000 / catchment area) over `heads`, in mm/yr.
//
// `heads` should be the heads NEAR the box, not the whole tile: the fit is
// local on purpose (see above) and the caller pays one flow-plane probe per
// head passed in.
inline RunoffCalibration calibrateRunoff(
    IBakedWaterSource& source, const std::vector<SpringHead>& heads,
    uint32_t minSamples = 8,
    int64_t fallbackMmPerYr = kEdgeInflowRunoffMmPerYrDefault) {
    RunoffCalibration out;
    out.runoffMmPerYr = fallbackMmPerYr > 0 ? fallbackMmPerYr : kEdgeInflowRunoffMmPerYrDefault;

    std::vector<int64_t> ratios;
    ratios.reserve(heads.size());
    for (const SpringHead& h : heads) {
        if (h.qM3PerYear == 0) continue;
        uint8_t flow = 0;
        if (!source.flowAt(h.px, h.py, flow)) continue;
        if (!flowIsChannel(flow)) continue;
        const int64_t areaM2 = springAreaM2(flow);
        if (areaM2 <= 0) continue;
        ratios.push_back(int64_t(h.qM3PerYear) * 1000 / areaM2);
    }
    out.samples = uint32_t(ratios.size());
    if (out.samples < minSamples) return out;

    // Insertion sort: the sample is a few dozen values, and this keeps the
    // median a pure function of the multiset (no introsort tie behaviour to
    // reason about). Lower median on an even count -- deterministic and, for a
    // rate, the conservative half.
    for (size_t i = 1; i < ratios.size(); ++i) {
        const int64_t key = ratios[i];
        size_t j = i;
        while (j > 0 && ratios[j - 1] > key) {
            ratios[j] = ratios[j - 1];
            --j;
        }
        ratios[j] = key;
    }
    const int64_t median = ratios[(ratios.size() - 1) / 2];
    if (median <= 0) return out;
    out.runoffMmPerYr = median;
    out.calibrated = true;
    return out;
}

// One place the baked channel crosses a face of the box, INBOUND.
struct RiverCrossing {
    //: The EDGE pixel itself (absolute fine pixel, on the boundary ring).
    int64_t px = 0, py = 0;
    //: The inward D8 step toward the downstream neighbour: each of dirX/dirY is
    //: -1, 0 or +1 and at least one is non-zero. This is the direction the
    //: water is going, so it is the direction an emitter's velocity points.
    int8_t dirX = 0, dirY = 0;
    //: The baked water surface at the crossing, absolute mm. The emitter's
    //: datum: water entering a river enters AT the river's surface, not at the
    //: ground under it (which is up to 25 m below on a big reach).
    int32_t surfaceMm = 0;
    //: The flow byte's bucket, and the two derived quantities. Kept alongside
    //: the estimate so a log line can show the chain rather than its answer.
    uint8_t accumLog2 = 0;
    int64_t areaM2 = 0;
    int64_t qM3PerYear = 0;
};

struct RiverCrossingParams {
    //: Inclusive FINE-pixel rectangle. Its boundary ring is what gets walked.
    RegionBounds bounds{};

    //: Same meaning as BakedWaterBuildParams' fields of these names, and the
    //: same defaults, so a crossing and the graph built over the same box admit
    //: the same cells.
    uint8_t minAccumLog2 = 0;
    bool requireWet = true;

    //: Catchment-mean runoff for the area->discharge step. See the unit chain.
    int64_t runoffMmPerYr = kEdgeInflowRunoffMmPerYrDefault;
};

struct RiverCrossingResult {
    std::vector<RiverCrossing> crossings;

    //: Ran-flags, the same pair rivernet.h insists on: a walk that resolved
    //: nothing and a walk over dry ground must not read alike.
    uint64_t edgePixelsScanned = 0;
    uint64_t channelEdgePixels = 0; //: had the channel bit (before the wet/inbound tests)
    uint64_t unresolved = 0;        //: source could not answer (tile not resident)
    //: Inbound edge pixels that did NOT become their own crossing: absorbed
    //: into the run they belong to, or (at a corner, which is on two faces)
    //: found a second time and dropped.
    uint64_t mergedRunPixels = 0;
};

// Find the inbound channel crossings on the four faces of `params.bounds`.
//
// THE INBOUND TEST IS AN APPROXIMATION and here is exactly what it assumes.
// The bake ships no per-pixel flow DIRECTION -- only the channel bit and the
// log2 accumulation -- so "which way is this river going" has to be inferred.
// An edge pixel counts as inbound when some in-box D8 neighbour is
//   (a) also a qualifying channel cell (channel bit, wet, accumulation band),
//   (b) at an accumulation at least as large as the edge pixel's, and
//   (c) not higher than the edge pixel on the WATER SURFACE.
// (b) is the load-bearing one: accumulation never decreases downstream, so a
// neighbour with less catchment behind it is upstream and the river is leaving,
// not entering. (c) is buildFromBakedWater's own ordering rule -- water runs
// down its own surface -- and it is a `<=` rather than a `<` because the bake's
// settled surface is exactly flat across long reaches.
//
// WHERE IT IS WRONG: a river that leaves the box across a face whose outside
// happens to be flat and equally accumulated reads as inbound, and a river
// entering across a perfectly flat pool where the accumulation bucket has not
// yet ticked over is admitted on (b) alone. Both put an emitter on a real
// channel at the real water surface with a plausible rate -- the failure mode
// is a faucet pointing the wrong way down a river, not water in a field.
//
// RUN MERGING: a 40 m wide river crosses a face as ~20 consecutive pixels and
// is ONE crossing, not twenty. Consecutive inbound pixels along a face collapse
// to their thalweg -- highest accumulation, then lowest water surface, then
// first along the walk.
inline RiverCrossingResult selectRiverCrossings(IBakedWaterSource& source,
                                                const RiverCrossingParams& params) {
    RiverCrossingResult out;
    const RegionBounds& b = params.bounds;
    if (b.px1 < b.px0 || b.py1 < b.py0) return out;

    // One qualifying-cell probe, shared by the edge walk and the inward test.
    struct Cell {
        bool ok = false;
        uint8_t accumLog2 = 0;
        int32_t surfaceMm = 0;
    };
    const auto probe = [&](int64_t px, int64_t py, bool isEdgePixel) -> Cell {
        Cell c;
        uint8_t flow = 0;
        if (!source.flowAt(px, py, flow)) {
            if (isEdgePixel) ++out.unresolved;
            return c;
        }
        if (!flowIsChannel(flow)) return c;
        const uint8_t accum = flowAccumLog2(flow);
        if (isEdgePixel) ++out.channelEdgePixels;
        if (accum < params.minAccumLog2) return c;
        int32_t surfaceMm = 0;
        bool wet = false;
        if (!source.waterAt(px, py, surfaceMm, wet)) {
            if (isEdgePixel) ++out.unresolved;
            return c;
        }
        if (params.requireWet && !wet) return c;
        c.ok = true;
        c.accumLog2 = accum;
        c.surfaceMm = surfaceMm;
        return c;
    };

    // The four faces, each walked in ascending coordinate with a fixed inward
    // normal. A 1-pixel-wide box degenerates to the same face twice; the
    // duplicate is removed by the (px, py) dedupe at the end.
    struct Face {
        int64_t stepX, stepY; // walk direction
        int64_t inX, inY;     // inward normal
        int64_t startX, startY;
        int64_t count;
    };
    const int64_t w = b.px1 - b.px0 + 1;
    const int64_t h = b.py1 - b.py0 + 1;
    const Face faces[4] = {
        {0, 1, 1, 0, b.px0, b.py0, h},  // west face, walk north, inward +x
        {0, 1, -1, 0, b.px1, b.py0, h}, // east face, walk north, inward -x
        {1, 0, 0, 1, b.px0, b.py0, w},  // south face, walk east, inward +y
        {1, 0, 0, -1, b.px0, b.py1, w}, // north face, walk east, inward -y
    };

    for (const Face& f : faces) {
        // The current run of consecutive inbound pixels, and its thalweg.
        bool inRun = false;
        RiverCrossing best;
        const auto flushRun = [&]() {
            if (inRun) out.crossings.push_back(best);
            inRun = false;
        };
        for (int64_t i = 0; i < f.count; ++i) {
            const int64_t px = f.startX + f.stepX * i;
            const int64_t py = f.startY + f.stepY * i;
            ++out.edgePixelsScanned;
            const Cell e = probe(px, py, true);
            if (!e.ok) {
                flushRun();
                continue;
            }
            // The three inward D8 neighbours: the normal and the two diagonals
            // that still step into the box. Best = the one furthest downstream
            // under buildFromBakedWater's order (lowest surface, then highest
            // accumulation), among those that satisfy (a)(b)(c).
            int bestDx = 0, bestDy = 0;
            bool haveDir = false;
            int32_t bestSurface = 0;
            uint8_t bestAccum = 0;
            for (int64_t t = -1; t <= 1; ++t) {
                // Along-face offset t, plus the inward normal.
                const int64_t dx = f.inX + f.stepX * t;
                const int64_t dy = f.inY + f.stepY * t;
                const int64_t nx = px + dx, ny = py + dy;
                if (nx < b.px0 || nx > b.px1 || ny < b.py0 || ny > b.py1) continue;
                const Cell n = probe(nx, ny, false);
                if (!n.ok) continue;
                if (n.accumLog2 < e.accumLog2) continue;   // (b) upstream: river leaving
                if (n.surfaceMm > e.surfaceMm) continue;   // (c) uphill on the water surface
                const bool better = !haveDir || n.surfaceMm < bestSurface ||
                                    (n.surfaceMm == bestSurface && n.accumLog2 > bestAccum);
                if (!better) continue;
                haveDir = true;
                bestSurface = n.surfaceMm;
                bestAccum = n.accumLog2;
                bestDx = int(dx);
                bestDy = int(dy);
            }
            if (!haveDir) {
                flushRun();
                continue;
            }

            RiverCrossing c;
            c.px = px;
            c.py = py;
            c.dirX = int8_t(bestDx);
            c.dirY = int8_t(bestDy);
            c.surfaceMm = e.surfaceMm;
            c.accumLog2 = e.accumLog2;
            c.areaM2 = springAreaM2(e.accumLog2);
            c.qM3PerYear = springDischargeM3PerYear(c.areaM2, params.runoffMmPerYr);
            if (!inRun) {
                inRun = true;
                best = c;
                continue;
            }
            ++out.mergedRunPixels;
            // The thalweg of the run: most catchment, then lowest surface, then
            // earliest along the walk (which `best` already holds).
            if (c.accumLog2 > best.accumLog2 ||
                (c.accumLog2 == best.accumLog2 && c.surfaceMm < best.surfaceMm)) {
                best = c;
            }
        }
        flushRun();
    }

    // Corner pixels belong to two faces and a degenerate box repeats a face
    // wholesale, so the same crossing can be found twice. Drop the later
    // duplicate; the walk order above is deterministic, so which one survives
    // is too.
    std::vector<RiverCrossing> unique;
    unique.reserve(out.crossings.size());
    for (const RiverCrossing& c : out.crossings) {
        bool dup = false;
        for (const RiverCrossing& kept : unique) {
            if (kept.px == c.px && kept.py == c.py) {
                dup = true;
                break;
            }
        }
        if (dup) {
            ++out.mergedRunPixels;
            continue;
        }
        unique.push_back(c);
    }
    out.crossings.swap(unique);
    return out;
}

} // namespace vxc
