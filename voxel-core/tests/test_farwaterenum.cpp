// The far-field CANDIDATE SOURCE (farwaterenum.h): the union's bookkeeping and
// the stage-2 gate.
//
// WHAT THESE PIN, AND WHY EACH ONE EXISTS.
//
// The enumeration's whole value is a cost claim -- "the reject is index-only",
// "resolves are bounded by water, not by area", "the two sources are disjoint".
// Cost claims fail SILENTLY. A union written the obvious way (`mask |= bit`)
// absorbs an overlap perfectly: the plane and the basin registry can both claim
// a column and the only visible effect is that a "new from lakes" counter reads
// smaller, which looks like good news. Every brick, quad and byte the cascade is
// sized on would then be inflated by that overlap and nothing would say so.
//
// So the counters are the contract, and these tests pin the counters. The real
// tiles are measured by `vxc_farwaterenum`; what cannot be left to a probe run
// is whether the accounting could even represent the failure it is watching for.
//
// The plane and basin walks themselves need a FineTileSampler and a real baked
// tile, so they are exercised by the probe rather than here. The two things
// that are pure -- the grid's dry-by-default apron and the resolve gate -- are
// pinned here because both have already been got wrong elsewhere in this
// subsystem: an apron that answers "wet" outside its bounds makes
// farWaterBrickIsInterior skip a shoreline brick and punch a hole in the water
// surface, and a resolve loop that runs over the window instead of over the wet
// set is the sweep this whole header exists to avoid.

#include "voxelcore/farwaterenum.h"
#include "vxctest.h"

using namespace vxc;

namespace {

FarWaterColumnGrid makeGrid(int64_t bx0, int64_t by0, int32_t w, int32_t h) {
    FarWaterColumnGrid g;
    g.resize(bx0, by0, w, h);
    return g;
}

} // namespace

// A column outside the grid must read DRY, not "unset but present". This is the
// apron farWaterBrickIsInterior reads, and its rule is that a dry neighbour
// makes a brick a candidate -- so failing this direction skips a shoreline brick
// and leaves a hole in the water surface exactly where the eye is drawn.
VXC_TEST(enum_grid_is_dry_outside_its_bounds) {
    FarWaterColumnGrid g = makeGrid(-4, -4, 8, 8);
    for (size_t i = 0; i < g.cols.size(); ++i) {
        g.cols[i].groundMm = 1000;
        g.cols[i].datumMm = 5000;
    }
    CHECK(g.colAt(0, 0).wet());
    CHECK(!g.colAt(-5, 0).wet());
    CHECK(!g.colAt(4, 0).wet());
    CHECK(!g.colAt(0, -5).wet());
    CHECK(!g.colAt(0, 4).wet());
    // And "dry" here must be the header's own sentinel, not a zero datum -- a
    // datum of 0 mm is a legitimate water surface at sea level.
    CHECK_EQ(int64_t(g.colAt(-5, 0).datumMm), int64_t(kNoWaterMm));
}

// STAGE 2 RUNS OVER THE WET SET, NOT OVER THE WINDOW. If this ever regresses to
// a full sweep the cascade's cost argument collapses, and it would still pass
// every correctness test in the suite -- it would just be slow. So the count of
// expensive calls is asserted directly.
VXC_TEST(enum_resolve_touches_only_columns_a_source_claimed) {
    FarWaterColumnGrid g = makeGrid(0, 0, 10, 10); // 100 columns
    g.src[g.at(2, 3)] = kFarWaterSrcPlane;
    g.src[g.at(7, 1)] = kFarWaterSrcBasin;
    g.src[g.at(9, 9)] = kFarWaterSrcPlane;

    int64_t calls = 0;
    FarWaterEnumStats st;
    farWaterResolveWet(
        g,
        [&](int64_t, int64_t) {
            ++calls;
            return int32_t(1000);
        },
        [](int64_t, int64_t) { return int32_t(4000); }, st);

    CHECK_EQ(calls, int64_t(3));
    CHECK_EQ(int64_t(st.colsResolved), int64_t(3));
    CHECK_EQ(int64_t(st.colsWetAfterResolve), int64_t(3));
    CHECK(g.colAt(2, 3).wet());
    CHECK(!g.colAt(4, 4).wet()); // never claimed, never resolved, still dry
}

// A column the PLANE called wet can resolve DRY, and that is a real answer
// rather than a miscount: the plane is the 1.875 m baked lattice and the ground
// is the 10 cm amplified surface, so at a shoreline they disagree. Both numbers
// are reported precisely so a run where the amplifier has risen above every
// datum reads as "resolved 40,000, wet 0" instead of as "found no water".
VXC_TEST(enum_reports_claimed_and_resolved_separately) {
    FarWaterColumnGrid g = makeGrid(0, 0, 4, 4);
    g.src[g.at(0, 0)] = kFarWaterSrcPlane;
    g.src[g.at(1, 1)] = kFarWaterSrcPlane;

    FarWaterEnumStats st;
    farWaterResolveWet(
        g, [](int64_t vx, int64_t) { return int32_t(vx == 0 ? 1000 : 9000); },
        [](int64_t, int64_t) { return int32_t(4000); }, st);

    CHECK_EQ(int64_t(st.colsResolved), int64_t(2));
    // (0,0) has ground 1000 under a 4000 datum: wet. (1,1) is voxel x 8, ground
    // 9000, above the datum: claimed by the plane but not water on screen.
    CHECK_EQ(int64_t(st.colsWetAfterResolve), int64_t(1));
}

// THE DISJOINTNESS COUNTER MUST BE ABLE TO SEE AN OVERLAP. Pinning that a
// counter can report the failure it watches for is not a tautology here: the
// natural way to write this union hides the overlap entirely, and the number
// the probe prints on real tiles is only evidence if a non-zero value was
// reachable in the first place.
VXC_TEST(enum_union_counts_a_column_claimed_by_both_sources) {
    FarWaterColumnGrid g = makeGrid(0, 0, 4, 4);
    FarWaterEnumStats st;

    // Hand-run the two marks the way the two walks do, so this pins the
    // accounting rule rather than a helper.
    const size_t i = g.at(1, 1);
    g.src[i] = uint8_t(g.src[i] | kFarWaterSrcPlane);
    ++st.planeWetCols;

    if ((g.src[i] & kFarWaterSrcBasin) == 0) {
        if ((g.src[i] & kFarWaterSrcPlane) != 0) ++st.bothWetCols;
        g.src[i] = uint8_t(g.src[i] | kFarWaterSrcBasin);
        ++st.lakeWetCols;
    }

    CHECK_EQ(int64_t(st.planeWetCols), int64_t(1));
    CHECK_EQ(int64_t(st.lakeWetCols), int64_t(1));
    CHECK_EQ(int64_t(st.bothWetCols), int64_t(1));
    // The union must not double count, which is the whole reason the overlap is
    // tracked rather than absorbed.
    CHECK_EQ(int64_t(st.unionWetCols()), int64_t(1));
}

// The rejection rate is over blocks that HAD an index to read. A block with no
// resident tile is an absence, not a win, and counting it as a rejection would
// let a window with no tiles under it report a perfect 100%.
VXC_TEST(enum_reject_rate_excludes_blocks_with_no_index) {
    FarWaterEnumStats st;
    st.blocksVisited = 100;
    st.blocksNoTile = 90;
    st.blocksConstDry = 6;
    st.blocksConstWet = 1;
    st.blocksDecoded = 3;
    // 6 of the 10 blocks that had an index, NOT 96 of 100.
    CHECK(st.indexOnlyRejectRate() > 0.59 && st.indexOnlyRejectRate() < 0.61);

    FarWaterEnumStats empty;
    empty.blocksVisited = 50;
    empty.blocksNoTile = 50;
    CHECK_EQ(empty.indexOnlyRejectRate(), 0.0);
}

// The brick-column -> fine-pixel map is what makes the plane half and the basin
// half land on the SAME columns. If the two used different roundings a river
// and the lake it flows into would be offset by half a brick at every join.
VXC_TEST(enum_brick_column_to_pixel_is_a_floor_at_the_origin) {
    // 1.875 m pixels, 0.8 m brick columns.
    const int64_t pxMm = 1875;
    CHECK_EQ(farWaterBrickColToPixel(0, pxMm), int64_t(0));
    // Brick column 1 starts at voxel 8 = 800 mm, which is still pixel 0.
    CHECK_EQ(farWaterBrickColToPixel(1, pxMm), int64_t(0));
    // Column 3 starts at 2400 mm -> pixel 1.
    CHECK_EQ(farWaterBrickColToPixel(3, pxMm), int64_t(1));
    // Negative coordinates floor toward minus infinity, not toward zero -- the
    // world is centred near the origin and half of it has negative brick
    // columns, so a truncating divide would mirror the map about x = 0.
    CHECK_EQ(farWaterBrickColToPixel(-1, pxMm), int64_t(-1));
    CHECK_EQ(farWaterBrickColToPixel(-3, pxMm), int64_t(-2));
}

