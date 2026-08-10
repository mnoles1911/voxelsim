#pragma once
// ==========================================================================
// DEPRECATED 2026-08-09 -- the only host of this header builds nothing by
// default. Do not extend; do not delete.
// ==========================================================================
//
// The first PBF playtest found far-field river ribbons unreadable against lake
// sheets, and the owner asked for them off. `AVoxelRiverRibbonActor` now
// requires -VoxelRiverRibbons=1, so on a default run every function below is
// dead code that still compiles and is still pinned by test_riverribbon.cpp
// (17 tests) and vxc_riverribbonprobe.
//
// THE CODE STAYS, by the standing rule in
// docs/water-deprecation-audit-2026-08-09.md: anything with a live call site
// outside its own tests, or with tests that pin its behaviour, is deprecated
// now and deleted only after the replacement is proven. The replacement is
// docs/water-rearchitecture-plan-2026-08-09.md's PBF near field plus whatever
// far-field handoff it grows. Nothing about LAKE SHEETS (lakes.h
// `lakeSheetRects`) is deprecated -- those are the standing-water far field and
// they stay on.
//
// The design note below is left intact: a deprecated producer that can still be
// switched on still has to be understandable by whoever switches it on.
//
// voxelcore/riverribbon.h -- the FAR-FIELD producer for flowing water.
//
// WHY THIS EXISTS. Water reaches the screen by two paths and, before this
// header, rivers only had one of them:
//
//   NEAR FIELD -- the implicit disc in RefreshImplicitWater, real voxels,
//     65x65x33 BRICKS centred on the CAMERA brick, i.e. +/-25.6 m in xy and
//     +/-12.8 m in z. Rivers use this. Because the box follows the camera and
//     not the water, a camera 90 m above a river is offered NO wet column at
//     all: the measured capture logged 0 candidate bricks. The vertical radius
//     is the binding constraint at altitude, not the horizontal one.
//
//   FAR FIELD -- sheets. Structurally lake-only: the basin registry,
//     `holdsWater()` and `extentMaskFor` all assume a basin, and
//     `CompositeWaterSampler` deliberately forwards the sheet half to lakes
//     (lakes.h: "a river reach is not a flat disc and cannot be drawn as one").
//
// So there was no producer that could draw flowing water past ~26 m. This is
// the missing producer. It turns the baked water plane into ORDERED CENTRELINE
// PATHS -- a polyline per reach, carrying its own datum and its own measured
// width -- which a host expands into a ribbon of quads.
//
// ==========================================================================
// WHY NOT `lakeSheetRects`, WHICH IS RIGHT THERE AND ALREADY SHIPS
// ==========================================================================
//
// `lakeSheetRects` (lakes.h:413) decomposes a wet mask into axis-aligned
// rectangles by run-length, point-sampling the mask at each block CENTRE
// (`sx = cx + half`) with TargetCellsPerSide = 128 -- roughly a 15 m
// axis-aligned staircase on a big basin. The owner has already rejected that
// output in these words: "sharp, rectangular, square edges where it meets land
// rather than a natural curving, arching shoreline."
//
// On a lake that is a bad edge. On a river it would be the WHOLE OBJECT: a
// reach is 1-3 fine pixels wide (1.9-5.6 m measured on the baked corridor
// tiles), so a 15 m axis-aligned decomposition of it is not a river with a bad
// edge, it is a chain of disconnected 15 m squares. Even at step 1 the
// rectangles would be a 1.875 m staircase, and a staircase is exactly what a
// meander is not.
//
// So the primitive here is a POLYLINE, not a rectangle set, and every stage
// below exists to keep it on the channel's actual curve:
//
//   1. the centreline is found at FULL fine-pixel resolution (no decimation of
//      the mask -- a 1 px channel does not survive any),
//   2. the polyline is simplified by DOUGLAS-PEUCKER against a perpendicular
//      tolerance of ONE PIXEL, which collapses the 8-connected staircase of a
//      diagonal run into a straight chord (a staircase deviates <= 0.5 px from
//      its own chord) while PRESERVING a meander, whose curvature is tens of
//      metres and so always exceeds the tolerance.
//
// That is the whole trick: the same tolerance that removes the artefact keeps
// the shape, because the artefact is sub-pixel and the shape is not.
//
// ==========================================================================
// THE DATUM. WHICH GROUND.
// ==========================================================================
//
// There are THREE grounds in this codebase (tilestore.h:654-674) and they have
// been conflated three times. Every elevation this header produces is:
//
//     surfaceMm = reconstructedGroundMm(tiles, px, py) + depth * kWaterDepthLsbMm
//
// -- ground #2, THE RECONSTRUCTED SURFACE, the cubic B-spline `evalCarrier`
// reconstruction of the baked control lattice, plus the baked water depth.
// It is NOT ground #1 (the control lattice, which stands up to 5.6 m off the
// surface it interpolates) and it is NOT ground #3 (the AMPLIFIED surface,
// `GetSurfaceHeightUU` / `Amplifier::columnCached().surfaceMm`, which is
// explicitly forbidden as a water datum: adding a depth to it would put metres
// of rill ripple on a surface that is flat by definition).
//
// This header does not compute that sum itself. It reads it from
// `RiverSampler::surfaceAtPixel`, which is the SAME call the near-field
// ImplicitFn makes through `waterSurfaceMmAtVoxel`. That is deliberate and it
// is the one structural advantage rivers have over lakes here: the lake sheet
// takes its datum from the basin table's `surfaceMm` while the lake near field
// takes it from `waterSurfaceMmAtVoxel`, so the two CAN disagree. For rivers
// there is one function and both sides call it, so the near/far datum agrees
// by construction. Tone can still differ (two surfaces, two shading paths) --
// that has to be measured -- but HEIGHT cannot.
//
// INDEPENDENCE FROM THE AMPLIFIER. Nothing here reads the amplified surface,
// so nothing here changes when the channel-carve lands in amplifier.cpp /
// worldgen.ush. What that change does affect is whether the ribbon is
// OCCLUDED by drawn ground -- a rendering question about the host's depth
// test, not about the geometry this header emits.
//
// ==========================================================================
// INTEGER ONLY
// ==========================================================================
//
// voxel-core is integer-only in src/ and include/ and this header is no
// exception: no float, no double, no sqrt. Perpendicular distance is compared
// as a squared cross product against a squared tolerance, and the sqrt(2) of a
// diagonal run is 181/128 (1.41406 vs 1.41421, 1.1e-4 relative -- 0.2 mm on a
// 1.875 m pixel).

#include <algorithm>
#include <cstdint>
#include <vector>

#include "voxelcore/core.h"
#include "voxelcore/lakes.h"
#include "voxelcore/tilestore.h"

namespace vxc {

// One point on a river centreline.
//
// COORDINATES ARE ABSOLUTE FINE PIXELS, not tile-local -- a reach crosses tile
// boundaries constantly and the corridor this was built for spans four tiles,
// so a tile-local point would need its tile carried beside it forever.
//
// `halfWidthMm` is HALF the measured cross-channel wet run at this point, in
// millimetres. It is the width the bake actually drew, not `channelWidthMm`'s
// discharge law from channel.h: the law says what width the hydrology wants,
// the plane says what the raster got, and the ribbon must cover the raster or
// it will not line up with the near-field voxels that were meshed from it.
struct RiverRibbonPoint {
    int64_t px = 0, py = 0;
    int32_t surfaceMm = kNoWaterMm;
    int32_t halfWidthMm = 0;
};

// One reach, ordered along the channel.
//
// IT USED TO BE UNORIENTED, and the comment here said so: "head to foot along
// the channel (or foot to head -- the direction is not meaningful here and
// nothing downstream depends on it; a ribbon is symmetric)." That was true
// while a ribbon was only a shape. It stopped being true the moment anything
// wanted to show the water MOVING, because motion has a sign.
//
// `riverRibbonOrient` now guarantees pts[0] is UPSTREAM of pts.back(), so the
// polyline runs downstream and its tangent is the flow direction.
struct RiverRibbonPath {
    std::vector<RiverRibbonPoint> pts;
};

// Order a reach downstream: pts[0] upstream, pts.back() at the mouth.
//
// WHY THIS IS EXACT AND NOT A HEURISTIC. `graded_water_surface` enforces that
// the water surface NEVER RISES GOING DOWNSTREAM (water-system-architecture
// §5) -- the whole extent stage rests on it, because without it water flows
// uphill somewhere and the fill rules chase it. So comparing the surface
// height at the two ends of a reach reads that invariant directly. There is no
// gradient estimate, no threshold, and nothing to tune.
//
// AND IT IS THE DIRECTION SOURCE THAT SURVIVED MEASUREMENT. The obvious
// alternative was to derive flow from the gradient of the water surface, the
// way Minecraft's FlowingFluid::getFlow derives one from neighbouring fluid
// levels -- free, since the near-field mesher already computes that gradient
// for its normal. Measured on the wet alpine block with vxc_riverribbonprobe:
// across a +/-1-pixel (3.75 m) stencil the direction resolves above the depth
// plane's 10 mm LSB on only **37.9% of centreline cells** (154,419 of 407,042).
// The pre-registered bar was 90%. The centreline is exactly where flow matters,
// so gradient-as-direction is dead there; its MAGNITUDE is still a usable free
// speed proxy (p50 9.4 m/km where it resolves).
//
// A reach whose ends are equal is left as it is and reported: that is standing
// water -- a lake sheet caught by the tracer, or a pool -- and it has no flow
// direction because it has no flow. Suppressing motion there is correct.
//
// Returns the number of reaches that were REVERSED, which is only interesting
// as a sanity figure: over many reaches it should sit near half, because the
// tracer's walk direction has nothing to do with which way the water goes.
inline size_t riverRibbonOrient(std::vector<RiverRibbonPath>& paths,
                                size_t* flatOut = nullptr) {
    size_t reversed = 0, flat = 0;
    for (RiverRibbonPath& p : paths) {
        if (p.pts.size() < 2) continue;
        const int32_t a = p.pts.front().surfaceMm;
        const int32_t b = p.pts.back().surfaceMm;
        if (a == kNoWaterMm || b == kNoWaterMm) continue; // datum not resolved; leave alone
        if (a == b) {
            ++flat;
            continue;
        }
        if (a < b) { // front is LOWER, so the reach currently runs upstream
            std::reverse(p.pts.begin(), p.pts.end());
            ++reversed;
        }
    }
    if (flatOut) *flatOut = flat;
    return reversed;
}

// The wet mask over a rectangular window of fine pixels, with the datum
// already resolved for every wet cell.
//
// DENSE, deliberately. The window is ~1% wet so a sparse set would be smaller,
// but every stage below asks "is my neighbour wet" thousands of times per
// pixel and a dense byte answers that in one indexed load. At the shipped
// 4 km scan radius the window is 4267^2 cells: 18 MB of mask plus 73 MB of
// datum, which is why the host fills it in BLOCK-SIZED BITES over several
// ticks rather than in one call (see the budget in the ribbon actor).
struct RiverWetWindow {
    int64_t x0 = 0, y0 = 0; // window origin, absolute fine pixels
    int32_t w = 0, h = 0;
    std::vector<uint8_t> wet;       // w*h, 1 = wet
    std::vector<int32_t> surfaceMm; // w*h, kNoWaterMm where dry

    // `surfaceMm` is NOT allocated here. It is four bytes per cell against the
    // mask's one, and it is only ever read at centreline pixels (~0.3% of the
    // window), so allocating it up front would quadruple the resident cost of a
    // window for data that is written after thinning. riverRibbonResolveDatum
    // allocates it.
    void resize(int64_t ox, int64_t oy, int32_t width, int32_t height) {
        x0 = ox;
        y0 = oy;
        w = width;
        h = height;
        wet.assign(size_t(w) * size_t(h), 0);
        surfaceMm.clear();
        surfaceMm.shrink_to_fit();
    }
    // Allocates the datum plane if it is not there yet. Anything that WRITES a
    // datum must call this first: `resize` deliberately leaves `surfaceMm`
    // empty, and indexing an empty vector is a silent out-of-bounds write that
    // corrupts the heap and crashes somewhere else entirely.
    void ensureDatum() {
        if (surfaceMm.size() != wet.size()) surfaceMm.assign(wet.size(), kNoWaterMm);
    }
    int32_t datumAt(size_t i) const { return i < surfaceMm.size() ? surfaceMm[i] : kNoWaterMm; }
    bool inBounds(int32_t x, int32_t y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    size_t at(int32_t x, int32_t y) const { return size_t(y) * size_t(w) + size_t(x); }
    bool isWet(int32_t x, int32_t y) const { return inBounds(x, y) && wet[at(x, y)] != 0; }
};

// How far a cross-channel run is allowed to be walked before we give up and
// call it "wide".
//
// 32 px is 60 m. A reach wider than that is not a ribbon and should not be
// drawn as one -- but note the bake writes the water plane DRY inside a
// registered basin precisely so lakes and rivers never both answer, so a run
// this long means either a genuinely huge trunk river or an unregistered
// waterbody. Either way the clamp keeps the cost bounded and the caller can
// see it in `riverRibbonWideRuns`.
inline constexpr int32_t kRiverRunScanCap = 32;

// sqrt(2) as a 128ths fraction: 181/128 = 1.414063 (true 1.414214).
inline constexpr int64_t kSqrt2Num = 181;
inline constexpr int64_t kSqrt2Den = 128;

// The four undirected axes a cross-channel run can lie along.
inline constexpr int32_t kRiverAxisDx[4] = {1, 0, 1, 1};
inline constexpr int32_t kRiverAxisDy[4] = {0, 1, 1, -1};
inline constexpr bool kRiverAxisDiagonal[4] = {false, false, true, true};
// The eight neighbour directions, in order, as both the trace and the end-cap
// gate below index them.
inline constexpr int32_t kRiverNeighDx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
inline constexpr int32_t kRiverNeighDy[8] = {0, 1, 1, 1, 0, -1, -1, -1};

// Directions that lie MORE THAN 90 DEGREES from direction k, as a bitmask over
// the eight. For k, those are k+3, k+4 and k+5 (mod 8) -- the three whose dot
// product with k is negative.
inline constexpr uint8_t riverOpposedMask(int32_t k) {
    return uint8_t((1u << ((k + 3) & 7)) | (1u << ((k + 4) & 7)) | (1u << ((k + 5) & 7)));
}

// Result of the thinning pass. Parallel to the window.
struct RiverThinField {
    std::vector<uint8_t> centre;     // 1 = this pixel is a centreline pixel
    std::vector<int32_t> halfWidthMm; // valid where centre != 0
    uint64_t wetPixels = 0;
    uint64_t centrePixels = 0;
    uint64_t wideRuns = 0; // runs that hit kRiverRunScanCap on every axis
};

// ---------------------------------------------------------------------------
// STAGE 1: CENTRELINE SELECTION BY SHORTEST RUN
// ---------------------------------------------------------------------------
//
// WHY NOT A GENERAL THINNING (Zhang-Suen and friends). A general skeletoniser
// is the right tool for an arbitrary blob and the wrong one here: it is
// iterative (unbounded passes over an 18 MB mask), it is defined by a table of
// neighbourhood cases that nobody on this project can check by eye, and it
// throws the width away -- which we then have to measure again anyway, because
// the ribbon needs it.
//
// A river reach is not an arbitrary blob. It is a RIBBON of measured width
// 1-3 px, and for a ribbon the medial axis has a closed form: at any wet
// pixel, the SHORTEST wet run through it is the cross-channel one, and its
// midpoint is on the medial axis. That is one bounded pass, it is exact for
// widths up to the scan cap at any of the four sampled orientations, and it
// yields the width as a by-product instead of as a second measurement that
// could disagree.
//
// THE TIE-BREAK IS LOAD-BEARING. For an EVEN run there are two middle cells
// and picking "whichever" per pixel would zig-zag the centreline by a pixel
// per cross-section, i.e. manufacture exactly the staircase this file exists
// to avoid. The rule is: the centre is the cell `(len-1)/2` steps along +axis
// from the run's LOW end, where "low" is the -axis end. `(len-1)/2` floors, so
// a run of 2 picks its low cell and a run of 4 picks its second, always on the
// same side, and the bias is a constant half-pixel offset of the whole
// centreline rather than a per-section wobble. A constant sub-pixel offset is
// invisible; a wobble is not.
//
// Ties BETWEEN AXES of equal length resolve to the lowest axis index, which
// orders them H, V, D+, D- -- again a fixed choice, so a straight reach cannot
// alternate between two equally short runs and wobble.
//
// THE END-CAP GATE, and the two versions of it that were wrong first.
//
// "Shortest run" is the cross-channel direction in the INTERIOR of a ribbon and
// nowhere else. At an END CAP it is not, and the first version of this function
// got that wrong in a way the tests caught: at the corner of a three-wide reach
// the two DIAGONAL runs are 1 and 3 cells long, both shorter than the true
// cross-section of 3, so the corner pixel was marked as its own centreline and
// the reach grew a diagonal spur at each end. On an even-width reach the same
// effect put centres on BOTH rows near the ends -- the exact wobble the
// tie-break above exists to prevent, arriving through a different door.
//
// THE SECOND ATTEMPT was per-axis: admit an axis only if both of the pixel's
// neighbours along the PERPENDICULAR axis are wet. That is correct for a wide
// reach and CATASTROPHIC for a narrow one, which is the case that matters here.
// A 1.9 m river is ONE fine pixel, drawn as an 8-connected staircase, and at
// every step transition of that staircase the reach continues along (-1,-1) and
// (+1,0) -- two different axes. No single axis has both perpendicular
// neighbours wet, so every transition pixel was rejected, every 1 px reach
// shattered into fragments below the minimum length, and the producer emitted
// NOTHING. The staircase test went from 1 path to 0.
//
// THE RULE THAT HOLDS is about the pixel, not about an axis: a pixel is an
// interior pixel of a reach if the channel leaves it in two directions more
// than 90 degrees apart. At a cap every wet neighbour is within a right angle
// of every other (the mask only continues one way); at a staircase transition
// (-1,-1) and (+1,0) are 135 degrees apart and it passes. A cap therefore
// contributes no centre, so a reach is up to one pixel (1.875 m) shorter at
// each end than the mask -- invisible, and much preferable to a spur.
inline void riverRibbonThin(const RiverWetWindow& win, int32_t pixelMm, RiverThinField& out) {
    const size_t n = size_t(win.w) * size_t(win.h);
    out.centre.assign(n, 0);
    out.halfWidthMm.assign(n, 0);
    out.wetPixels = 0;
    out.centrePixels = 0;
    out.wideRuns = 0;
    if (win.w <= 0 || win.h <= 0 || pixelMm <= 0 || win.wet.size() != n) return;

    for (int32_t y = 0; y < win.h; ++y) {
        for (int32_t x = 0; x < win.w; ++x) {
            const size_t i = win.at(x, y);
            if (win.wet[i] == 0) continue;
            ++out.wetPixels;

            // END-CAP GATE, before any run is walked: cheap, and it rejects the
            // pixels whose runs would lie anyway.
            uint8_t neigh = 0;
            for (int32_t k = 0; k < 8; ++k)
                if (win.isWet(x + kRiverNeighDx[k], y + kRiverNeighDy[k])) neigh = uint8_t(neigh | (1u << k));
            bool interior = false;
            for (int32_t k = 0; k < 8 && !interior; ++k)
                if ((neigh & (1u << k)) && (neigh & riverOpposedMask(k))) interior = true;
            if (!interior) continue;

            int32_t len[4] = {0, 0, 0, 0}, back[4] = {0, 0, 0, 0};
            bool allCapped = true;
            for (int32_t a = 0; a < 4; ++a) {
                const int32_t dx = kRiverAxisDx[a], dy = kRiverAxisDy[a];
                int32_t b = 0, f = 0;
                while (b < kRiverRunScanCap && win.isWet(x - dx * (b + 1), y - dy * (b + 1))) ++b;
                while (f < kRiverRunScanCap && win.isWet(x + dx * (f + 1), y + dy * (f + 1))) ++f;
                if (b < kRiverRunScanCap && f < kRiverRunScanCap) allCapped = false;
                len[a] = b + f + 1;
                back[a] = b;
            }
            if (allCapped) ++out.wideRuns;

            // Shortest run wins; strict < keeps the LOWEST axis index on a tie.
            int32_t bestAxis = 0;
            for (int32_t a = 1; a < 4; ++a)
                if (len[a] < len[bestAxis]) bestAxis = a;

            // Midpoint of that run, counted from its low (-axis) end.
            const int32_t bestLen = len[bestAxis], bestBack = back[bestAxis];
            if (bestBack != (bestLen - 1) / 2) continue;

            out.centre[i] = 1;
            ++out.centrePixels;
            // Half the run in mm; a diagonal step is sqrt(2) pixels long.
            int64_t halfMm = (int64_t(bestLen) * int64_t(pixelMm)) / 2;
            if (kRiverAxisDiagonal[bestAxis]) halfMm = (halfMm * kSqrt2Num) / kSqrt2Den;
            out.halfWidthMm[i] = int32_t(halfMm);
        }
    }

    // REDUNDANCY PRUNE. The end-cap gate removes the extreme corner of a cap
    // but not the cell one in from it: the last two cells of a wide reach form
    // a wedge whose own shortest run is a 2-cell DIAGONAL, so they are honest
    // cross-sections of the wedge and the rule above admits them. Left in, a
    // three-wide reach ends in a little three-pronged fork.
    //
    // They are redundant in the graph sense, which is what actually matters to
    // the trace: a centre pixel with EXACTLY TWO centre neighbours that are
    // themselves adjacent to each other carries no connectivity -- delete it
    // and its two neighbours are still joined, directly. Nothing else can be
    // removed by this rule, so a straight run (neighbours two apart), a
    // staircase transition ((-1,-1) and (+1,0), two apart) and a junction
    // (three neighbours, not two) are all untouched.
    //
    // Applied SEQUENTIALLY against the live set rather than to a snapshot. The
    // argument that removal is safe -- "its two neighbours remain joined" --
    // only holds if those two neighbours are still there, which a simultaneous
    // pass cannot promise.
    //
    // THE RESIDUAL, stated rather than papered over. Being sequential makes it
    // order-dependent at a wedge, and the very last cell of a reach can end up
    // one pixel off the centreline rather than on it: a three-wide reach thins
    // to a clean middle row for its whole length and then hooks by one cell at
    // the tip. That is 1.875 m at the end of a reach; it does not fork and it
    // does not disconnect. Closing it properly means a real medial-axis
    // transform rather than a run measurement, which is not worth it for one
    // cell. The tests therefore assert the BOUND -- at most one centre per
    // cross-section, never more than one pixel off the middle -- so this is a
    // measured limit rather than an untested one.
    for (int32_t y = 0; y < win.h; ++y) {
        for (int32_t x = 0; x < win.w; ++x) {
            const size_t i = win.at(x, y);
            if (out.centre[i] == 0) continue;
            int32_t nx[2] = {0, 0}, ny[2] = {0, 0}, count = 0;
            for (int32_t k = 0; k < 8 && count <= 2; ++k) {
                const int32_t ax = x + kRiverNeighDx[k], ay = y + kRiverNeighDy[k];
                if (!win.inBounds(ax, ay) || out.centre[win.at(ax, ay)] == 0) continue;
                if (count < 2) {
                    nx[count] = ax;
                    ny[count] = ay;
                }
                ++count;
            }
            if (count != 2) continue;
            const int32_t sx = nx[0] - nx[1], sy = ny[0] - ny[1];
            const bool adjacent = (sx >= -1 && sx <= 1 && sy >= -1 && sy <= 1);
            if (!adjacent) continue;
            out.centre[i] = 0;
            out.halfWidthMm[i] = 0;
            --out.centrePixels;
        }
    }
}

// ---------------------------------------------------------------------------
// STAGE 2: TRACE THE CENTRELINE PIXELS INTO ORDERED PATHS
// ---------------------------------------------------------------------------
//
// The centre set from stage 1 is thin and 8-connected along each reach, but it
// is a SET -- and a ribbon needs an ORDER, because the perpendicular at a
// point is defined by its neighbours along the path and by nothing else.
//
// The walk is greedy and prefers the STRAIGHTEST continuation, scored by the
// dot product of the step against the previous step. That is what keeps a
// trace going through a confluence instead of turning up a tributary: at a
// junction the main stem continues nearly straight (dot 2 for a repeat of the
// same direction) and the tributary arrives at an angle (dot <= 1).
//
// STARTS ARE ORDERED, and it matters for determinism: all degree-1 endpoints
// first in (y,x) order, then anything left over. Leftovers are real -- a loop
// (a braided reach) has no endpoint at all -- and without the second pass they
// would silently vanish rather than being drawn.
//
// CONFLUENCES ARE STITCHED. A junction pixel is consumed by whichever path
// reaches it first, so a later branch would start one pixel short and leave a
// 1.875 m hole exactly where two rivers meet. When a new path starts on a
// pixel that has an already-visited centre neighbour, that neighbour is
// PREPENDED to the path without being re-claimed, so the branch reaches the
// stem it joins.
struct RiverTraceParams {
    // Reaches shorter than this are dropped. An isolated wet speck -- a pond
    // the bake left in the plane, a one-pixel artefact at a tile seam -- would
    // otherwise become a free-floating scrap of water surface, which reads as
    // a rendering bug rather than as a river.
    int64_t minPathLengthMm = 120000; // 120 m
};

inline size_t riverRibbonTrace(const RiverWetWindow& win, const RiverThinField& thin, int32_t pixelMm,
                               const RiverTraceParams& params, std::vector<RiverRibbonPath>& out) {
    const size_t before = out.size();
    if (win.w <= 0 || win.h <= 0 || pixelMm <= 0) return 0;
    if (thin.centre.size() != size_t(win.w) * size_t(win.h)) return 0;

    // The same eight directions the end-cap gate indexes, so the two stages
    // cannot disagree about what "adjacent" means.
    const int32_t* kNx = kRiverNeighDx;
    const int32_t* kNy = kRiverNeighDy;

    auto isCentre = [&](int32_t x, int32_t y) {
        return win.inBounds(x, y) && thin.centre[win.at(x, y)] != 0;
    };
    std::vector<uint8_t> visited(thin.centre.size(), 0);

    auto degree = [&](int32_t x, int32_t y) {
        int32_t d = 0;
        for (int32_t k = 0; k < 8; ++k)
            if (isCentre(x + kNx[k], y + kNy[k])) ++d;
        return d;
    };
    auto makePoint = [&](int32_t x, int32_t y) {
        RiverRibbonPoint p;
        p.px = win.x0 + x;
        p.py = win.y0 + y;
        p.surfaceMm = win.datumAt(win.at(x, y));
        p.halfWidthMm = thin.halfWidthMm[win.at(x, y)];
        return p;
    };

    // Walk from (sx,sy), claiming pixels as it goes.
    auto walk = [&](int32_t sx, int32_t sy, RiverRibbonPath& path) {
        int32_t x = sx, y = sy, pdx = 0, pdy = 0;
        for (;;) {
            visited[win.at(x, y)] = 1;
            path.pts.push_back(makePoint(x, y));
            int32_t bestK = -1, bestScore = -100;
            for (int32_t k = 0; k < 8; ++k) {
                const int32_t nx = x + kNx[k], ny = y + kNy[k];
                if (!isCentre(nx, ny) || visited[win.at(nx, ny)]) continue;
                // Straightness: dot of this step with the previous one. The
                // first step has no previous, so every direction scores 0 and
                // the lowest k wins -- deterministic, and the direction of a
                // path is not meaningful anyway.
                const int32_t score = kNx[k] * pdx + kNy[k] * pdy;
                if (score > bestScore) {
                    bestScore = score;
                    bestK = k;
                }
            }
            if (bestK < 0) return;
            pdx = kNx[bestK];
            pdy = kNy[bestK];
            x += pdx;
            y += pdy;
        }
    };

    // Polyline length, for the minimum-length drop. Diagonal steps count
    // sqrt(2); ignoring that would under-measure a diagonal reach by 41% and
    // drop reaches that are long enough.
    auto pathLengthMm = [&](const RiverRibbonPath& p) {
        int64_t total = 0;
        for (size_t i = 1; i < p.pts.size(); ++i) {
            const int64_t dx = p.pts[i].px - p.pts[i - 1].px;
            const int64_t dy = p.pts[i].py - p.pts[i - 1].py;
            const int64_t steps = std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
            const bool diag = (dx != 0 && dy != 0);
            int64_t segMm = steps * int64_t(pixelMm);
            if (diag) segMm = (segMm * kSqrt2Num) / kSqrt2Den;
            total += segMm;
        }
        return total;
    };

    // BOTH ENDS ARE STITCHED, and the first version of this only did the start.
    // A path can meet already-claimed geometry at either end -- it starts on a
    // junction another path took, or it walks INTO one and halts because every
    // continuation is claimed. Stitching only the start left a reach that
    // arrives at a confluence stopping one pixel short of it, which is a 1.875 m
    // hole in the water exactly where two rivers meet.
    auto stitchPoint = [&](int32_t x, int32_t y, const RiverRibbonPoint* skip, RiverRibbonPoint& outPt) {
        for (int32_t k = 0; k < 8; ++k) {
            const int32_t nx = x + kNx[k], ny = y + kNy[k];
            if (!isCentre(nx, ny) || !visited[win.at(nx, ny)]) continue;
            if (skip && skip->px == win.x0 + nx && skip->py == win.y0 + ny) continue;
            outPt = makePoint(nx, ny);
            return true;
        }
        return false;
    };

    auto tryEmit = [&](int32_t sx, int32_t sy) {
        RiverRibbonPath path;
        RiverRibbonPoint head;
        const bool hasHead = stitchPoint(sx, sy, nullptr, head);
        if (hasHead) path.pts.push_back(head);
        walk(sx, sy, path);
        // The tail stitch must not simply re-append the point before it, which
        // is trivially both a centre and visited.
        const size_t n = path.pts.size();
        if (n >= 1) {
            const int32_t ex = int32_t(path.pts[n - 1].px - win.x0);
            const int32_t ey = int32_t(path.pts[n - 1].py - win.y0);
            const RiverRibbonPoint* prev = n >= 2 ? &path.pts[n - 2] : nullptr;
            RiverRibbonPoint tail;
            if (stitchPoint(ex, ey, prev, tail)) path.pts.push_back(tail);
        }
        if (path.pts.size() >= 2 && pathLengthMm(path) >= params.minPathLengthMm) {
            out.push_back(std::move(path));
        }
    };

    for (int32_t y = 0; y < win.h; ++y)
        for (int32_t x = 0; x < win.w; ++x)
            if (isCentre(x, y) && !visited[win.at(x, y)] && degree(x, y) == 1) tryEmit(x, y);
    for (int32_t y = 0; y < win.h; ++y)
        for (int32_t x = 0; x < win.w; ++x)
            if (isCentre(x, y) && !visited[win.at(x, y)]) tryEmit(x, y);

    return out.size() - before;
}

// ---------------------------------------------------------------------------
// STAGE 3: SIMPLIFY -- THE STEP THAT REMOVES THE STAIRCASE
// ---------------------------------------------------------------------------
//
// Douglas-Peucker with an integer perpendicular test, plus an ELEVATION test.
//
// THE XY TEST is the answer to the owner's rejected lake edge. A traced
// centreline on a raster is an 8-connected staircase: a reach running at 30
// degrees is stored as alternating E and NE steps, and drawing a quad per step
// would put a visible sawtooth on every diagonal reach. But a staircase
// deviates AT MOST half a pixel from the chord it approximates, so a tolerance
// of one pixel deletes every intermediate step of a straight run and leaves
// one long chord -- while a meander, whose sagitta over the same span is tens
// of metres, keeps all of its points. Same tolerance, opposite outcomes,
// because the artefact is sub-pixel and the shape is not.
//
// THE ELEVATION TEST exists because XY simplification alone would let a chord
// span a knickpoint. The water surface descends monotonically down a reach
// (the bake enforces it), but not at a constant rate: a step-pool sequence can
// drop a metre in ten and a chord across it would hang the ribbon in the air
// at the top and bury it at the bottom. A point is kept if EITHER its
// perpendicular XY deviation exceeds the XY tolerance OR its surface deviates
// from the straight-line interpolation by more than `elevTolMm`.
//
// INTEGER PERPENDICULAR DISTANCE. dist(C, AB) > tol is tested as
// cross(B-A, C-A)^2 > tol^2 * |B-A|^2, which has no division and no sqrt. All
// terms are int64 over WINDOW-LOCAL coordinates: absolute fine pixels reach
// ~1e5 on this world and squaring their differences directly would be tight,
// so the caller's window origin is subtracted first.
struct RiverSimplifyParams {
    // One fine pixel. Anything smaller preserves the staircase it is meant to
    // remove; anything much larger starts cutting corners off meanders.
    int32_t xyTolPx = 1;
    // A quarter of the measured p50 water headroom above drawn ground
    // (723 mm), so a chord can never lift the ribbon far enough to clear the
    // bed it is supposed to sit in.
    int32_t elevTolMm = 180;
};

namespace detail {

// ITERATIVE, not recursive. A single traced reach on this world can be
// thousands of points (a 10 km run at 1.875 m/px is ~5300), and textbook
// Douglas-Peucker recurses to depth O(n) in the worst case -- a monotonically
// curving reach, which is precisely what a meander is. An explicit stack costs
// one vector and removes the failure mode entirely.
//
// THE TWO CRITERIA ARE NOT COMMENSURABLE and are not mixed. `xyOver` is a
// squared cross product in pixel^4; `elevOver` is millimetres. Adding or
// max()-ing them would make the split point depend on units. The rule is
// lexicographic: if ANY point breaks the XY tolerance, split at the worst XY
// offender; otherwise, if any point breaks the elevation tolerance, split at
// the worst elevation offender; otherwise the chord stands.
inline void riverRibbonDp(const std::vector<RiverRibbonPoint>& pts, int64_t tolSq, int64_t elevTolMm,
                          std::vector<uint8_t>& keep) {
    if (pts.size() < 3) return;
    std::vector<std::pair<size_t, size_t>> stack;
    stack.emplace_back(size_t(0), pts.size() - 1);
    while (!stack.empty()) {
        const size_t a = stack.back().first, b = stack.back().second;
        stack.pop_back();
        if (b <= a + 1) continue;
        const int64_t ax = pts[a].px, ay = pts[a].py;
        const int64_t dx = pts[b].px - ax, dy = pts[b].py - ay;
        const int64_t lenSq = dx * dx + dy * dy;

        size_t worstXy = 0, worstElev = 0;
        int64_t bestXy = 0, bestElev = 0;
        for (size_t i = a + 1; i < b; ++i) {
            const int64_t cx = pts[i].px - ax, cy = pts[i].py - ay;
            const int64_t cross = dx * cy - dy * cx;
            // dist(C, AB) > tol  <=>  cross^2 > tol^2 * |AB|^2. No sqrt, no
            // division. A degenerate chord (lenSq == 0, a closed loop) makes
            // this cross^2 > 0, which keeps any point off the start -- right.
            const int64_t xyOver = cross * cross - tolSq * lenSq;
            if (xyOver > 0 && xyOver > bestXy) {
                bestXy = xyOver;
                worstXy = i;
            }
            if (bestXy > 0 || lenSq == 0) continue;
            // Elevation against the straight-line interpolation, at this
            // point's projection along the chord. Guarded on kNoWaterMm
            // (INT32_MIN) rather than trusted: a traced point is wet by
            // construction, but sign-extending INT32_MIN into this arithmetic
            // would be a silent catastrophe rather than a visible one.
            if (pts[a].surfaceMm == kNoWaterMm || pts[b].surfaceMm == kNoWaterMm ||
                pts[i].surfaceMm == kNoWaterMm)
                continue;
            const int64_t t = dx * cx + dy * cy; // projection numerator, over lenSq
            const int64_t chord = int64_t(pts[a].surfaceMm) +
                                  ((int64_t(pts[b].surfaceMm) - int64_t(pts[a].surfaceMm)) * t) / lenSq;
            int64_t d = int64_t(pts[i].surfaceMm) - chord;
            if (d < 0) d = -d;
            const int64_t elevOver = d - elevTolMm;
            if (elevOver > 0 && elevOver > bestElev) {
                bestElev = elevOver;
                worstElev = i;
            }
        }
        const size_t split = bestXy > 0 ? worstXy : (bestElev > 0 ? worstElev : 0);
        if (split == 0) continue;
        keep[split] = 1;
        stack.emplace_back(a, split);
        stack.emplace_back(split, b);
    }
}

} // namespace detail

inline void riverRibbonSimplify(RiverRibbonPath& path, const RiverSimplifyParams& params) {
    const size_t n = path.pts.size();
    if (n < 3) return;
    // PATH-LOCAL coordinates. Absolute fine pixels reach ~1e5 on this world,
    // and `cross` is a product of two coordinate differences: rebasing on the
    // path's own first point keeps both differences inside the reach's own
    // extent, so cross^2 stays many orders under int64 instead of merely
    // fitting.
    const int64_t ox = path.pts[0].px, oy = path.pts[0].py;
    std::vector<RiverRibbonPoint> local = path.pts;
    for (RiverRibbonPoint& p : local) {
        p.px -= ox;
        p.py -= oy;
    }
    std::vector<uint8_t> keep(n, 0);
    keep[0] = 1;
    keep[n - 1] = 1;
    const int64_t tolSq = int64_t(params.xyTolPx) * int64_t(params.xyTolPx);
    detail::riverRibbonDp(local, tolSq, params.elevTolMm, keep);

    std::vector<RiverRibbonPoint> kept;
    kept.reserve(n);
    for (size_t i = 0; i < n; ++i)
        if (keep[i]) kept.push_back(path.pts[i]);
    path.pts.swap(kept);
}

// ---------------------------------------------------------------------------
// FILLING THE WINDOW FROM THE BAKED PLANE
// ---------------------------------------------------------------------------
//
// THE MASK IS FILLED BLOCK-WISE FROM THE RAW DEPTH RASTER, AND THE DATUM IS
// RESOLVED AFTERWARDS AT THE CENTRELINE ONLY. That split is a ~300x saving and
// it is the reason this is affordable at kilometre scale at all.
//
// The obvious implementation was one sweep of `RiverSampler::surfaceAtPixel`
// over the window. It is correct, and at a 4 km radius it is 18 million calls,
// each a hash lookup into the block cache -- to answer a question ("is this
// pixel wet") that is one sign bit of an int16 that has already been decoded.
// Worse, it resolves the datum for every wet pixel when only the ~0.3% that
// survive thinning will ever carry one.
//
// So: `riverRibbonFillWet` decodes each 256x256 water block ONCE and scans its
// int16 array linearly, touching no spline at all; `riverRibbonResolveDatum`
// then calls `surfaceAtPixel` for the centreline pixels only, which is where
// the 16-probe carrier evaluation is actually wanted.
//
// The datum still comes from `surfaceAtPixel` -- the SAME call the near-field
// ImplicitFn reaches through `waterSurfaceMmAtVoxel` -- so the far field cannot
// land at a different height from the near field. Only the dry/wet test is
// short-circuited, and it is short-circuited to the identical condition
// (`depth < 0`) that `surfaceAtPixel` itself applies before the spline.
//
// Returns the number of wet pixels written into the sub-rectangle.
inline uint64_t riverRibbonFillWet(FineTileSampler& tiles, RiverWetWindow& win, int32_t rx0, int32_t ry0,
                                   int32_t rw, int32_t rh) {
    uint64_t wetCount = 0;
    const uint32_t size = tiles.tileSize();
    if (size == 0) return 0;
    const int32_t x1 = std::min(win.w, rx0 + rw), y1 = std::min(win.h, ry0 + rh);
    const int32_t xs = std::max(0, rx0), ys = std::max(0, ry0);

    // ITERATION IS BLOCK-MAJOR, NOT ROW-MAJOR, and the difference is a factor
    // of 256. A water block is 256x256 fine pixels; a row-major sweep across a
    // multi-kilometre window crosses every block in the row, so a one-block
    // cache decodes each block once PER ROW -- 256 decodes of the same 128 KB
    // for one pass. Walking blocks on the outside and pixels on the inside
    // decodes each exactly once.
    std::vector<int16_t> depth;
    // Block size is a property of the tile, so it is read from the first
    // resident tile rather than assumed; a window that touches no tile at all
    // just returns 0.
    const FineTile* probe = nullptr;
    for (int32_t y = ys; y < y1 && !probe; y += 1) {
        const int64_t py = win.y0 + y;
        for (int32_t x = xs; x < x1 && !probe; x += 1) {
            const int64_t px = win.x0 + x;
            probe = tiles.findTile(int32_t(floorDiv(px, size)), int32_t(floorDiv(py, size)));
            x += int32_t(size); // one probe per tile column is enough
        }
        y += int32_t(size);
    }
    if (probe == nullptr) return 0;
    const uint32_t log2 = probe->blockLog2();
    const uint32_t dim = probe->blockDim();

    // Absolute pixel bounds of the sub-rectangle, snapped out to block edges.
    const int64_t ax0 = win.x0 + xs, ay0 = win.y0 + ys;
    const int64_t ax1 = win.x0 + x1, ay1 = win.y0 + y1;
    const int64_t gb0x = floorDiv(ax0, int64_t(dim)), gb1x = floorDiv(ax1 - 1, int64_t(dim));
    const int64_t gb0y = floorDiv(ay0, int64_t(dim)), gb1y = floorDiv(ay1 - 1, int64_t(dim));

    for (int64_t gby = gb0y; gby <= gb1y; ++gby) {
        for (int64_t gbx = gb0x; gbx <= gb1x; ++gbx) {
            // The block's own absolute pixel origin, then its tile.
            const int64_t bpx = gbx * int64_t(dim), bpy = gby * int64_t(dim);
            const int32_t tx = int32_t(floorDiv(bpx, size)), ty = int32_t(floorDiv(bpy, size));
            const FineTile* tile = tiles.findTile(tx, ty);
            // A tile that is not resident, or one baked before the water plane
            // existed, answers DRY -- the same policy surfaceAtPixel applies,
            // and safe for the same reason: the residency gate means no chunk
            // generates over a non-resident footprint.
            if (tile == nullptr || !tile->hasWater()) continue;
            const uint32_t bx = uint32_t((bpx - int64_t(tx) * size) >> log2);
            const uint32_t by = uint32_t((bpy - int64_t(ty) * size) >> log2);
            if (!tile->decodeWaterBlock(bx, by, depth)) continue; // counted by the sampler
            if (depth.size() < size_t(dim) * size_t(dim)) continue;

            const int64_t px0 = std::max(ax0, bpx), px1 = std::min(ax1, bpx + int64_t(dim));
            const int64_t py0 = std::max(ay0, bpy), py1 = std::min(ay1, bpy + int64_t(dim));
            for (int64_t py = py0; py < py1; ++py) {
                const size_t drow = size_t(py - bpy) * size_t(dim);
                const size_t wrow = size_t(py - win.y0) * size_t(win.w);
                for (int64_t px = px0; px < px1; ++px) {
                    if (depth[drow + size_t(px - bpx)] < 0) continue;
                    win.wet[wrow + size_t(px - win.x0)] = 1;
                    ++wetCount;
                }
            }
        }
    }
    return wetCount;
}

// Resolves the datum for the centreline pixels only. Must run AFTER thinning
// and BEFORE tracing, because the trace copies the datum onto its points and
// the simplifier's elevation criterion reads it.
inline uint64_t riverRibbonResolveDatum(RiverSampler& rivers, RiverWetWindow& win,
                                        const RiverThinField& thin) {
    uint64_t resolved = 0;
    if (thin.centre.size() != win.wet.size()) return 0;
    win.ensureDatum();
    for (int32_t y = 0; y < win.h; ++y) {
        for (int32_t x = 0; x < win.w; ++x) {
            const size_t i = win.at(x, y);
            if (thin.centre[i] == 0) continue;
            const int32_t mm = rivers.surfaceAtPixel(win.x0 + x, win.y0 + y);
            win.surfaceMm[i] = mm;
            resolved += (mm != kNoWaterMm);
        }
    }
    return resolved;
}

// The whole producer, for tests and for the bench probe. The host does not call
// this -- it needs the stages separately so it can budget the fill across ticks.
inline size_t buildRiverRibbons(FineTileSampler& tiles, RiverSampler& rivers, RiverWetWindow& win,
                                int32_t pixelMm, const RiverTraceParams& trace,
                                const RiverSimplifyParams& simplify,
                                std::vector<RiverRibbonPath>& out, RiverThinField* thinOut = nullptr) {
    riverRibbonFillWet(tiles, win, 0, 0, win.w, win.h);
    RiverThinField local;
    RiverThinField& thin = thinOut ? *thinOut : local;
    riverRibbonThin(win, pixelMm, thin);
    riverRibbonResolveDatum(rivers, win, thin);
    const size_t n = riverRibbonTrace(win, thin, pixelMm, trace, out);
    for (size_t i = out.size() - n; i < out.size(); ++i) riverRibbonSimplify(out[i], simplify);
    // Orient AFTER simplification: Douglas-Peucker keeps the endpoints, so the
    // comparison reads the same two surface heights either way, and doing it
    // once at the end covers every path the host produced.
    riverRibbonOrient(out);
    return n;
}

// ---------------------------------------------------------------------------
// FLOW DIRECTION AT A POINT, from the oriented reaches
// ---------------------------------------------------------------------------
//
// WHAT THIS IS FOR. The near field draws water as REAL VOXELS, and that is the
// half of the world where "make the river look like it is flowing" has to be
// answered -- a scrolling flat quad is the thing the owner rejected outright
// ("I don't want to use lighting or shadow gimmicks. I want to physically
// change the geometry"). So the direction has to reach the voxel water, not
// just the ribbon.
//
// WHY NOT THE WATER-SURFACE GRADIENT, which would have been free: MEASURED AND
// FAILED. Across a +/-1-pixel stencil it resolves above the depth plane's 10 mm
// LSB on only 37.9% of centreline cells (docs/measurements/
// water-surface-gradient-2026-08-06.txt), against a pre-registered bar of 90%.
// It fails worst exactly on the centreline. A reach tangent has no such
// failure mode: the reach is an ordered polyline and riverRibbonOrient has
// already given it a sign from an invariant rather than from a derivative.
//
// THE COST MODEL, stated because it decides where this may be called. This is a
// linear scan over reaches with a cheap bounding-box reject, NOT a spatial
// index. That is correct for the near field -- the implicit box is +/-25.6 m
// (VoxelWaterSubsystem.cpp) and only a handful of reaches can intersect it --
// and it is WRONG for a whole-window sweep at the ribbon actor's 4 km scan
// radius, where 1,345 reaches were traced on one measured block. Call it per
// water BRICK (0.8 m), not per voxel, and not over the far window.
// INTEGER, because this header is integer-only and says so at the top ("no
// float, no double, no sqrt"). The tangent is returned as the RAW segment
// delta in fine pixels and the distance as its SQUARE, so nothing here needs a
// square root. The caller normalises -- the engine side is float-native and a
// direction is only ever consumed as one, so the division belongs there rather
// than as a rounding error baked into the library.
struct RiverFlowSample {
    bool valid = false;      // false = no reach within the search radius
    int64_t dx = 0, dy = 0;  // downstream tangent, UNNORMALISED, fine pixels
    int64_t dist2Px = 0;     // SQUARED distance to the nearest point on that reach
};

// Nearest oriented reach to (px, py), and its downstream tangent there.
//
// `paths` MUST have been through riverRibbonOrient, or the sign is whatever the
// tracer happened to walk. There is no way to check that here, which is why
// buildRiverRibbons orients unconditionally at the end.
//
// A reach left FLAT by the orienter (both ends at the same surface height) is
// standing water and is skipped: it has no flow direction because it has no
// flow, and inventing one for a lake is worse than showing none. Callers get
// `valid == false` and should suppress motion, exactly as the CA-activity gate
// does for player-disturbed water.
inline RiverFlowSample riverFlowDirAt(const std::vector<RiverRibbonPath>& paths, int64_t px,
                                      int64_t py, int64_t searchRadiusPx) {
    RiverFlowSample out;
    int64_t bestD2 = searchRadiusPx * searchRadiusPx;

    for (const RiverRibbonPath& p : paths) {
        if (p.pts.size() < 2) continue;
        // A flat reach carries no direction. Cheap to test and it is the whole
        // lake case.
        const int32_t a = p.pts.front().surfaceMm, b = p.pts.back().surfaceMm;
        if (a == kNoWaterMm || b == kNoWaterMm || a == b) continue;

        for (size_t i = 0; i + 1 < p.pts.size(); ++i) {
            const int64_t x0 = p.pts[i].px, y0 = p.pts[i].py;
            const int64_t ex = p.pts[i + 1].px - x0, ey = p.pts[i + 1].py - y0;
            const int64_t len2 = ex * ex + ey * ey;
            if (len2 <= 0) continue;

            // Closest point on the SEGMENT, clamped to its ends so the answer
            // is the distance to the polyline and not to its infinite line.
            //
            // Kept in integers by scaling: the closest point is
            // (x0,y0) + (num/len2)*(ex,ey), so the offset from the query point
            // is ((px-x0)*len2 - num*ex, ...) over len2, and comparing squared
            // distances means comparing those numerators against bestD2*len2^2.
            // Everything stays exact; nothing is rounded before the comparison.
            const int64_t wx = px - x0, wy = py - y0;
            int64_t num = wx * ex + wy * ey;
            if (num < 0) num = 0;
            if (num > len2) num = len2;
            const int64_t offx = wx * len2 - num * ex;
            const int64_t offy = wy * len2 - num * ey;

            // d2 = (offx^2 + offy^2) / len2^2, compared as
            // offx^2 + offy^2 < bestD2 * len2^2 to stay integral. len2 is
            // bounded by the simplifier's chord length, so this does not
            // overflow int64 at any world coordinate we address.
            const int64_t lhs = offx * offx + offy * offy;
            const int64_t rhs = bestD2 * len2 * len2;
            if (lhs >= rhs) continue;

            bestD2 = lhs / (len2 * len2);
            out.valid = true;
            out.dx = ex;
            out.dy = ey;
            out.dist2Px = bestD2;
        }
    }
    return out;
}

} // namespace vxc
