// Tests for the far-field river producer (voxelcore/riverribbon.h).
//
// THE LOAD-BEARING TESTS HERE are the two that guard the owner's already-stated
// rejection. He looked at the lake sheet and said: "sharp, rectangular, square
// edges where it meets land rather than a natural curving, arching shoreline."
// A river is a 1-3 pixel ribbon, so the same defect on a river would not be an
// edge artefact, it would be the entire object. The two properties that have to
// hold, and that a screenshot cannot check quickly, are:
//
//   * `ribbon_simplify_collapses_a_diagonal_staircase_to_one_chord` -- a
//     straight reach at an arbitrary angle must NOT be drawn as a stack of
//     axis-aligned steps, and
//   * `ribbon_simplify_keeps_a_meander` -- the same tolerance that deletes the
//     staircase must NOT delete real curvature.
//
// Both fall out of one fact (a raster staircase deviates at most half a pixel
// from its own chord, a meander deviates by tens of metres), and a regression
// in either direction is a regression in what the owner rejected. Beside them,
// `ribbon_even_width_centreline_does_not_wobble` guards the same artefact one
// stage earlier: a per-cross-section tie-break would manufacture a sawtooth
// before the simplifier ever saw it.
//
// Everything here is synthetic. The stages under test are pure functions over
// a mask, so no tile, no bake and no fixture is needed -- the tile-fed path is
// exercised by vxc_riverribbonprobe against the real corridor.

#include <cstdint>
#include <vector>

#include "voxelcore/riverribbon.h"
#include "vxctest.h"

using namespace vxc;

namespace {

constexpr int32_t kPx = 1875; // the fine tier's pixel, 1.875 m

RiverWetWindow makeWindow(int32_t w, int32_t h) {
    RiverWetWindow win;
    win.resize(0, 0, w, h);
    return win;
}

void setWet(RiverWetWindow& win, int32_t x, int32_t y, int32_t surfaceMm = 100000) {
    if (!win.inBounds(x, y)) return;
    // The window does not allocate its datum plane until something writes one;
    // see RiverWetWindow::ensureDatum. Skipping this is an out-of-bounds write.
    win.ensureDatum();
    win.wet[win.at(x, y)] = 1;
    win.surfaceMm[win.at(x, y)] = surfaceMm;
}

// A 1-px Bresenham line, which is exactly the 8-connected staircase a traced
// centreline produces for a reach running at an arbitrary angle.
void drawLine(RiverWetWindow& win, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t surfaceMm = 100000) {
    int32_t dx = x1 - x0, dy = y1 - y0;
    const int32_t sx = dx >= 0 ? 1 : -1, sy = dy >= 0 ? 1 : -1;
    dx = dx >= 0 ? dx : -dx;
    dy = dy >= 0 ? dy : -dy;
    int32_t x = x0, y = y0, err = dx - dy;
    for (;;) {
        setWet(win, x, y, surfaceMm);
        if (x == x1 && y == y1) break;
        const int32_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }
}

size_t countCentres(const RiverThinField& t) {
    size_t n = 0;
    for (uint8_t c : t.centre) n += (c != 0);
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// STAGE 1 -- centreline and width
// ---------------------------------------------------------------------------

// A straight reach three pixels wide must thin to ONE pixel per cross-section,
// on the middle row, and must report the width the bake actually drew. Three
// pixels at 1.875 m is 5.625 m, the p90 of the measured 1.9-5.6 m band, so
// half-width is 2812 mm.
VXC_TEST(ribbon_three_wide_reach_thins_to_the_middle_row) {
    RiverWetWindow win = makeWindow(40, 11);
    for (int32_t x = 2; x < 38; ++x)
        for (int32_t y = 4; y <= 6; ++y) setWet(win, x, y);

    RiverThinField thin;
    riverRibbonThin(win, kPx, thin);

    CHECK_EQ(thin.wetPixels, uint64_t(36 * 3));

    // THE CONTRACT IS ONE CENTRE PER CROSS-SECTION, ON THE MIDDLE ROW, and it
    // is asserted as a bound rather than as an exact set because the last cell
    // of a reach is genuinely ambiguous -- see "the residual" in riverribbon.h.
    // What must hold, and what the artefacts this file guards against would all
    // break, is: no column ever carries TWO centres (that is a fork, or the
    // wobble), and no centre is more than one pixel off the true middle row
    // (that is a spur).
    for (int32_t x = 0; x < win.w; ++x) {
        int32_t found = 0;
        for (int32_t y = 0; y < win.h; ++y) {
            if (!thin.centre[win.at(x, y)]) continue;
            ++found;
            CHECK(y >= 4 && y <= 6);
        }
        CHECK(found <= 1);
    }
    // The interior is exact: middle row, and the width the bake drew.
    for (int32_t x = 3; x < 35; ++x) {
        CHECK(thin.centre[win.at(x, 5)] != 0);
        CHECK(thin.centre[win.at(x, 4)] == 0);
        CHECK(thin.centre[win.at(x, 6)] == 0);
        CHECK_EQ(thin.halfWidthMm[win.at(x, 5)], (3 * kPx) / 2);
    }
    // And the centreline is CONNECTED end to end -- the prune must not have
    // opened a gap in the middle of a reach.
    std::vector<RiverRibbonPath> paths;
    RiverTraceParams tp;
    tp.minPathLengthMm = 30000; // this synthetic reach is only 36 px = 67.5 m
    riverRibbonTrace(win, thin, kPx, tp, paths);
    CHECK_EQ(paths.size(), size_t(1));
    if (paths.size() == 1) CHECK(paths[0].pts.size() >= 34);
}

// The measured band on the corridor tiles is 1-3 fine pixels. All three widths
// must produce a centreline and a half-width that round-trips to the run.
VXC_TEST(ribbon_width_covers_the_measured_one_to_three_pixel_band) {
    for (int32_t width = 1; width <= 3; ++width) {
        RiverWetWindow win = makeWindow(30, 11);
        const int32_t y0 = 5 - (width - 1) / 2;
        for (int32_t x = 2; x < 28; ++x)
            for (int32_t y = y0; y < y0 + width; ++y) setWet(win, x, y);

        RiverThinField thin;
        riverRibbonThin(win, kPx, thin);
        // One centre per column at most, for every width in the measured band.
        for (int32_t x = 0; x < win.w; ++x) {
            int32_t found = 0;
            for (int32_t y = 0; y < win.h; ++y) found += (thin.centre[win.at(x, y)] != 0);
            CHECK(found <= 1);
        }
        // The centre sits `(width-1)/2` up from the run's low end, by the
        // documented tie-break, and reports the width the bake drew: 1.875 m,
        // 3.75 m, 5.625 m for 1, 2 and 3 pixels -- the measured 1.9-5.6 m band.
        const int32_t cy = y0 + (width - 1) / 2;
        CHECK(thin.centre[win.at(10, cy)] != 0);
        CHECK_EQ(thin.halfWidthMm[win.at(10, cy)], (width * kPx) / 2);
    }
}

// THE TIE-BREAK. An even-width reach has two candidate middle cells per
// cross-section. If the choice were made per-section -- by parity, by which
// neighbour was seen first, by anything local -- the centreline would alternate
// between two rows and put a one-pixel sawtooth on a dead straight river.
// Picking `(len-1)/2` from the low end always lands on the same side, so the
// error is a constant half-pixel offset of the whole line, which is invisible,
// instead of a wobble, which is not.
VXC_TEST(ribbon_even_width_centreline_does_not_wobble) {
    RiverWetWindow win = makeWindow(40, 11);
    for (int32_t x = 2; x < 38; ++x)
        for (int32_t y = 4; y <= 5; ++y) setWet(win, x, y); // width 2, even

    RiverThinField thin;
    riverRibbonThin(win, kPx, thin);

    int32_t seenRow = -1;
    for (int32_t x = 3; x < 37; ++x) { // interior; the two caps have no cross-section
        int32_t row = -1;
        for (int32_t y = 0; y < win.h; ++y)
            if (thin.centre[win.at(x, y)]) {
                CHECK_EQ(row, -1); // exactly one centre per cross-section
                row = y;
            }
        CHECK(row >= 0);
        if (seenRow < 0) seenRow = row;
        CHECK_EQ(row, seenRow); // and always the SAME row -- no sawtooth
    }
}

// A reach running at 45 degrees is the case where a naive "shortest run" would
// be ambiguous, because the two diagonal axes and the two axis-aligned ones can
// all be short. The fixed axis order has to resolve it to a single centreline.
VXC_TEST(ribbon_diagonal_reach_thins_to_a_single_centreline) {
    RiverWetWindow win = makeWindow(48, 48);
    drawLine(win, 4, 4, 43, 43);

    RiverThinField thin;
    riverRibbonThin(win, kPx, thin);
    // A 1-px line is its own medial axis: every pixel survives except the two
    // end caps, which have no cross-section.
    CHECK_EQ(countCentres(thin), size_t(38));
    CHECK_EQ(thin.wideRuns, uint64_t(0));
}

// ---------------------------------------------------------------------------
// STAGE 3 -- the anti-staircase property, and its opposite
// ---------------------------------------------------------------------------

// THE ARTEFACT THE OWNER REJECTED, on a river. A straight reach at ~22 degrees
// is stored as an alternating run of E and NE steps. Drawn step by step that is
// an axis-aligned sawtooth. Simplification at one pixel must reduce it to the
// two endpoints and nothing between them.
VXC_TEST(ribbon_simplify_collapses_a_diagonal_staircase_to_one_chord) {
    RiverWetWindow win = makeWindow(140, 80);
    drawLine(win, 5, 10, 125, 59); // slope 49/120, a hard staircase

    RiverThinField thin;
    riverRibbonThin(win, kPx, thin);
    std::vector<RiverRibbonPath> paths;
    riverRibbonTrace(win, thin, kPx, RiverTraceParams{}, paths);
    CHECK_EQ(paths.size(), size_t(1));
    const size_t rawPts = paths[0].pts.size();
    CHECK(rawPts > 100); // the staircase really is there before simplification

    riverRibbonSimplify(paths[0], RiverSimplifyParams{});
    // A Bresenham line never leaves its own chord by more than half a pixel,
    // so at a one-pixel tolerance NOTHING between the endpoints survives.
    CHECK_EQ(paths[0].pts.size(), size_t(2));
}

// THE OPPOSITE, at the same tolerance. A meander must not be flattened. This
// arc has a sagitta of ~14 px (26 m), which is what real curvature looks like
// against a 1.875 m pixel -- an order of magnitude above the tolerance, which
// is exactly why one number can serve both tests.
VXC_TEST(ribbon_simplify_keeps_a_meander) {
    RiverWetWindow win = makeWindow(160, 120);
    // A discrete arc: y = 60 - (x-80)^2/100, sampled per column.
    int32_t prevX = -1, prevY = -1;
    for (int32_t x = 20; x <= 140; ++x) {
        const int32_t d = x - 80;
        const int32_t y = 90 - (d * d) / 100;
        if (prevX >= 0)
            drawLine(win, prevX, prevY, x, y);
        else
            setWet(win, x, y);
        prevX = x;
        prevY = y;
    }

    RiverThinField thin;
    riverRibbonThin(win, kPx, thin);
    std::vector<RiverRibbonPath> paths;
    riverRibbonTrace(win, thin, kPx, RiverTraceParams{}, paths);
    CHECK(paths.size() >= 1);
    size_t longest = 0;
    for (size_t i = 1; i < paths.size(); ++i)
        if (paths[i].pts.size() > paths[longest].pts.size()) longest = i;

    riverRibbonSimplify(paths[longest], RiverSimplifyParams{});
    // Curvature survives: many more than the two endpoints of a chord.
    CHECK(paths[longest].pts.size() >= 8);
    // And it survives IN THE RIGHT PLACE. The chord between the two ends runs
    // at y ~= 54; the arc bows away to y = 90 at x = 80, a 36 px sagitta. A
    // kept point has to sit near that apex, or the simplifier cut the meander
    // across even though it kept a point count.
    int64_t maxY = paths[longest].pts[0].py;
    for (const RiverRibbonPoint& p : paths[longest].pts) maxY = p.py > maxY ? p.py : maxY;
    CHECK(maxY >= 85);
}

// A chord that is straight in XY can still be wrong in Z. A step-pool reach
// drops in bursts, and simplifying on geometry alone would hang the ribbon in
// the air above the plunge and bury it below. The elevation criterion has to
// keep the knickpoint even though every point is collinear.
VXC_TEST(ribbon_simplify_keeps_a_knickpoint_a_straight_chord_would_span) {
    RiverWetWindow win = makeWindow(80, 11);
    for (int32_t x = 2; x < 78; ++x) {
        // Flat for 30 px, then a 4 m plunge over 4 px, then flat again.
        int32_t z = 100000;
        if (x >= 36) z = 96000;
        if (x >= 32 && x < 36) z = 100000 - (x - 32) * 1000;
        setWet(win, x, 5, z);
    }
    RiverThinField thin;
    riverRibbonThin(win, kPx, thin);
    std::vector<RiverRibbonPath> paths;
    riverRibbonTrace(win, thin, kPx, RiverTraceParams{}, paths);
    CHECK_EQ(paths.size(), size_t(1));

    riverRibbonSimplify(paths[0], RiverSimplifyParams{});
    // Dead straight in XY, so the XY criterion alone would give 2 points.
    CHECK(paths[0].pts.size() > 2);
    // Every kept point pair must bracket the drop closely enough that the
    // interpolated surface never departs from the true one by more than the
    // tolerance. Spot-check that a point near the lip survived.
    bool nearLip = false;
    for (const RiverRibbonPoint& p : paths[0].pts)
        if (p.px >= 30 && p.px <= 38) nearLip = true;
    CHECK(nearLip);
}

// ---------------------------------------------------------------------------
// STAGE 2 -- tracing
// ---------------------------------------------------------------------------

// A tributary must reach the stem it joins. The junction pixel is claimed by
// whichever path gets there first, so without the stitch the branch would stop
// one pixel short and leave a 1.875 m hole at the confluence -- a gap in the
// water exactly where a player's eye goes.
VXC_TEST(ribbon_trace_stitches_a_confluence) {
    // Each limb has to clear the 120 m default minimum on its own, so the
    // window is sized in metres rather than in convenient round pixels: 95 px
    // at 1.875 m is 178 m per limb.
    RiverWetWindow win = makeWindow(200, 200);
    drawLine(win, 5, 100, 194, 100); // the stem
    drawLine(win, 100, 5, 100, 100); // a tributary meeting it at (100,100)

    RiverThinField thin;
    riverRibbonThin(win, kPx, thin);
    std::vector<RiverRibbonPath> paths;
    const size_t n = riverRibbonTrace(win, thin, kPx, RiverTraceParams{}, paths);
    CHECK(n >= 2);

    // EVERY path must reach the confluence: either it passes through the 3x3
    // neighbourhood of the junction, or it ends inside it. A path that stops
    // two or more pixels short has left a hole in the water at the one place
    // the eye is drawn to.
    for (const RiverRibbonPath& p : paths) {
        bool reaches = false;
        for (const RiverRibbonPoint& q : p.pts) {
            if (q.px >= 99 && q.px <= 101 && q.py >= 99 && q.py <= 101) reaches = true;
        }
        CHECK(reaches);
    }

    // And the three limbs must all be represented -- a walk that turned up the
    // tributary and abandoned half the stem would still pass the test above.
    bool west = false, east = false, north = false;
    for (const RiverRibbonPath& p : paths) {
        for (const RiverRibbonPoint& q : p.pts) {
            if (q.py == 100 && q.px < 20) west = true;
            if (q.py == 100 && q.px > 180) east = true;
            if (q.px == 100 && q.py < 20) north = true;
        }
    }
    CHECK(west);
    CHECK(east);
    CHECK(north);
}

// An isolated speck is not a river. Left in, it becomes a free-floating scrap
// of water surface hanging on a hillside, which reads as a rendering bug.
VXC_TEST(ribbon_trace_drops_a_speck_below_the_minimum_length) {
    RiverWetWindow win = makeWindow(60, 60);
    drawLine(win, 2, 30, 57, 30); // 56 px = 105 m -- a real reach at 1.875 m/px
    setWet(win, 10, 10);          // one lonely pixel
    setWet(win, 11, 10);
    setWet(win, 12, 10);

    RiverThinField thin;
    riverRibbonThin(win, kPx, thin);
    std::vector<RiverRibbonPath> paths;
    RiverTraceParams tp;
    tp.minPathLengthMm = 60000; // 60 m: keeps the reach, drops the speck
    riverRibbonTrace(win, thin, kPx, tp, paths);
    CHECK_EQ(paths.size(), size_t(1));
    CHECK(paths[0].pts.size() > 40);
}

// Determinism: the same mask must produce byte-identical paths. The trace has
// three places where an arbitrary choice could leak in (which endpoint starts,
// which neighbour continues a tie, which leftover pixel seeds a loop) and all
// three are pinned to a fixed order. A ribbon that re-orders between frames
// would re-tessellate every rebuild.
VXC_TEST(ribbon_trace_is_deterministic) {
    RiverWetWindow win = makeWindow(120, 120);
    drawLine(win, 5, 10, 110, 70);
    drawLine(win, 60, 5, 60, 115);
    drawLine(win, 10, 100, 100, 20);

    RiverThinField thin;
    riverRibbonThin(win, kPx, thin);
    std::vector<RiverRibbonPath> a, b;
    riverRibbonTrace(win, thin, kPx, RiverTraceParams{}, a);
    riverRibbonTrace(win, thin, kPx, RiverTraceParams{}, b);
    CHECK_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        CHECK_EQ(a[i].pts.size(), b[i].pts.size());
        for (size_t j = 0; j < a[i].pts.size() && j < b[i].pts.size(); ++j) {
            CHECK_EQ(a[i].pts[j].px, b[i].pts[j].px);
            CHECK_EQ(a[i].pts[j].py, b[i].pts[j].py);
        }
    }
}

// An empty window must produce nothing rather than one degenerate path, and a
// mask whose vectors do not match its declared shape must be refused the same
// way lakeSheetRects refuses a wrong-sized mask.
VXC_TEST(ribbon_empty_and_malformed_windows_produce_nothing) {
    RiverWetWindow empty = makeWindow(32, 32);
    RiverThinField thin;
    riverRibbonThin(empty, kPx, thin);
    CHECK_EQ(thin.wetPixels, uint64_t(0));
    CHECK_EQ(thin.centrePixels, uint64_t(0));
    std::vector<RiverRibbonPath> paths;
    CHECK_EQ(riverRibbonTrace(empty, thin, kPx, RiverTraceParams{}, paths), size_t(0));

    RiverWetWindow bad = makeWindow(32, 32);
    bad.wet.resize(7); // shape lie
    RiverThinField badThin;
    riverRibbonThin(bad, kPx, badThin);
    CHECK_EQ(badThin.wetPixels, uint64_t(0));

    // A zero or negative pixel pitch is a caller bug, not a crash.
    RiverWetWindow win = makeWindow(32, 32);
    drawLine(win, 2, 16, 29, 16);
    RiverThinField zero;
    riverRibbonThin(win, 0, zero);
    CHECK_EQ(zero.centrePixels, uint64_t(0));
}

// The datum carried on every point must be the one the window was filled with
// -- `reconstructedGroundMm + depth`, ground #2 -- and must survive both the
// trace and the simplification unchanged. It is never recomputed, never
// re-derived from a neighbour, and never touched by the amplifier.
VXC_TEST(ribbon_carries_the_reconstructed_datum_unchanged) {
    RiverWetWindow win = makeWindow(80, 11);
    for (int32_t x = 2; x < 78; ++x) setWet(win, x, 5, 250000 - x * 37);

    RiverThinField thin;
    riverRibbonThin(win, kPx, thin);
    std::vector<RiverRibbonPath> paths;
    riverRibbonTrace(win, thin, kPx, RiverTraceParams{}, paths);
    CHECK_EQ(paths.size(), size_t(1));
    for (const RiverRibbonPoint& p : paths[0].pts) {
        CHECK(p.surfaceMm != kNoWaterMm);
        CHECK_EQ(p.surfaceMm, 250000 - int32_t(p.px) * 37);
    }
    riverRibbonSimplify(paths[0], RiverSimplifyParams{});
    for (const RiverRibbonPoint& p : paths[0].pts) {
        CHECK_EQ(p.surfaceMm, 250000 - int32_t(p.px) * 37);
    }
}
