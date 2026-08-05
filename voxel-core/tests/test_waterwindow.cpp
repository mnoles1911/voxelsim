// Tests for voxelcore/waterwindow.h -- the incremental water window.
//
// Every one of these pins something a MEASUREMENT forced, not something a
// design preferred. The measurements are in
// docs/measurements/water-refresh-2026-08-05.txt.

#include "vxctest.h"

#include <set>
#include <tuple>
#include <vector>

#include "voxelcore/waterwindow.h"

using namespace vxc;

namespace {

using Cell = std::tuple<int64_t, int64_t, int64_t>;

std::set<Cell> cellsOf(const WaterWindow& w) {
    std::set<Cell> s;
    if (w.empty()) return s;
    for (int64_t z = w.z0; z <= w.z1; ++z)
        for (int64_t y = w.y0; y <= w.y1; ++y)
            for (int64_t x = w.x0; x <= w.x1; ++x) s.insert({x, y, z});
    return s;
}

std::set<Cell> columnsOf(const WaterWindow& w) {
    std::set<Cell> s;
    if (w.empty()) return s;
    for (int64_t y = w.y0; y <= w.y1; ++y)
        for (int64_t x = w.x0; x <= w.x1; ++x) s.insert({x, y, 0});
    return s;
}

} // namespace

// THE PER-RING INVALIDATION RULE, and it is the whole of it: ring L's centre
// changes 2^L times less often than ring 0's, because it is a floorDiv by a
// brick that is 2^L times wider. The far-water plan asks for "ring L refreshes
// roughly 2^L times less often"; this is that sentence as arithmetic.
VXC_TEST(ring_centre_changes_2_pow_L_times_less_often) {
    // Walk 10.24 km of voxels and count how often each level's centre brick
    // moves. The length is a whole multiple of the WIDEST brick (8 << 5 = 256
    // voxels) on purpose: with a length that is not, the last partial brick
    // gives each level a different remainder and the halving is off by one at
    // the coarse end -- which is a property of the ruler, not of the rule.
    constexpr int64_t kSteps = 102400; // a whole multiple of (8 << 5), the widest brick
    for (int lod = 0; lod <= 5; ++lod) {
        int64_t changes = 0;
        int64_t prev = waterWindowCentreBrick(0, lod);
        for (int64_t v = 1; v <= kSteps; ++v) {
            const int64_t now = waterWindowCentreBrick(v, lod);
            if (now != prev) ++changes;
            prev = now;
        }
        // A brick at level L is (8 << L) voxels wide, so the centre changes
        // once per that many steps.
        const int64_t expected = kSteps / (int64_t(WaterBrick8::kEdge) << lod);
        CHECK_EQ(changes, expected);
    }
}

// The doubling stated directly: each level refreshes half as often as the one
// below it. Written separately from the count above because THIS is the claim
// the plan makes, and a reader should not have to divide two numbers to see it.
VXC_TEST(each_ring_refreshes_half_as_often_as_the_one_below) {
    constexpr int64_t kSteps = 102400; // a whole multiple of (8 << 5), so the halving is exact
    std::vector<int64_t> changes(6, 0);
    for (int lod = 0; lod <= 5; ++lod) {
        int64_t prev = waterWindowCentreBrick(0, lod);
        for (int64_t v = 1; v <= kSteps; ++v) {
            const int64_t now = waterWindowCentreBrick(v, lod);
            if (now != prev) ++changes[size_t(lod)];
            prev = now;
        }
    }
    for (int lod = 1; lod <= 5; ++lod) {
        CHECK_EQ(changes[size_t(lod)] * 2, changes[size_t(lod) - 1]);
    }
}

// Negative coordinates are where a truncating divide silently puts the centre
// one brick off, and half this world has them (the bv14 sites are all at
// negative x and y). floorDiv is the reason this holds.
VXC_TEST(centre_brick_is_correct_across_zero) {
    const int64_t edge = int64_t(WaterBrick8::kEdge);
    CHECK_EQ(waterWindowCentreBrick(0, 0), 0);
    CHECK_EQ(waterWindowCentreBrick(edge - 1, 0), 0);
    CHECK_EQ(waterWindowCentreBrick(edge, 0), 1);
    CHECK_EQ(waterWindowCentreBrick(-1, 0), -1);
    CHECK_EQ(waterWindowCentreBrick(-edge, 0), -1);
    CHECK_EQ(waterWindowCentreBrick(-edge - 1, 0), -2);
}

// THE IDENTITY THE WHOLE SCHEME RESTS ON. `waterWindowDifference` must produce
// exactly the set difference -- no cell missed (that is a hole in the water)
// and no cell twice (that is the re-meshing this change exists to remove).
VXC_TEST(window_difference_is_exactly_the_set_difference) {
    // Deterministic sweep over every relative offset a camera step can make,
    // including steps larger than the window (a teleport).
    const int64_t r = 3, rz = 2;
    const WaterWindow a = waterWindowAt(0, 0, 0, r, rz);
    for (int64_t dz = -6; dz <= 6; ++dz) {
        for (int64_t dy = -9; dy <= 9; ++dy) {
            for (int64_t dx = -9; dx <= 9; ++dx) {
                const WaterWindow b = waterWindowAt(dx, dy, dz, r, rz);
                WaterWindow regions[kWaterWindowMaxRegions];
                const int n = waterWindowDifference(b, a, regions);
                CHECK(n <= kWaterWindowMaxRegions);

                const std::set<Cell> setA = cellsOf(a), setB = cellsOf(b);
                std::set<Cell> expected;
                for (const Cell& c : setB)
                    if (setA.find(c) == setA.end()) expected.insert(c);

                std::set<Cell> got;
                int64_t emitted = 0;
                for (int i = 0; i < n; ++i) {
                    const std::set<Cell> rc = cellsOf(regions[i]);
                    emitted += int64_t(rc.size());
                    got.insert(rc.begin(), rc.end());
                }
                CHECK(got == expected);
                // DISJOINT: the emitted count must equal the set size. A naive
                // three-slab decomposition passes the union check above and
                // fails this one, by re-sweeping every edge and corner.
                CHECK_EQ(emitted, int64_t(got.size()));
            }
        }
    }
}

// A camera that has not moved does no work at all. This is what makes the
// far-field rings free between their own boundary crossings.
VXC_TEST(an_unmoved_window_yields_no_regions) {
    const WaterWindow a = waterWindowAt(5, -7, 2, 32, 16);
    WaterWindow regions[kWaterWindowMaxRegions];
    CHECK_EQ(waterWindowDifference(a, a, regions), 0);
    WaterWindow cols[kWaterWindowMaxColumnRegions];
    CHECK_EQ(waterWindowColumnDifference(a, a, cols), 0);
}

// The first build has nothing to shift from and must sweep everything.
VXC_TEST(first_build_sweeps_the_whole_window) {
    const WaterWindow none;
    const WaterWindow a = waterWindowAt(0, 0, 0, 32, 16);
    WaterWindow regions[kWaterWindowMaxRegions];
    const int n = waterWindowDifference(a, none, regions);
    CHECK_EQ(n, 1);
    CHECK(regions[0] == a);
    CHECK_EQ(a.count(), int64_t(65) * 65 * 33);
}

// THE NUMBER THIS CHANGE EXISTS FOR. One 0.8 m step on the client's own
// geometry re-sweeps 65*33 = 2,145 brick slots instead of 65*65*33 = 139,425.
VXC_TEST(one_brick_step_touches_a_single_slab) {
    const WaterWindow a = waterWindowAt(0, 0, 0, 32, 16);
    const WaterWindow b = waterWindowAt(1, 0, 0, 32, 16);
    WaterWindow regions[kWaterWindowMaxRegions];
    const int n = waterWindowDifference(b, a, regions);
    int64_t total = 0;
    for (int i = 0; i < n; ++i) total += regions[i].count();
    CHECK_EQ(total, int64_t(65) * 33);
    CHECK_EQ(a.count(), int64_t(65) * 65 * 33);
    // 1.5% of the box, which is the whole claim.
    CHECK(total * 65 == a.count());
}

// A PURE-ALTITUDE STEP ENTERS NEW BRICK SLOTS BUT NO NEW COLUMNS, and the
// expensive half of the sweep is per-column. Conflating the two reports a free
// step as a full rebuild. This is why waterWindowColumnDifference exists.
VXC_TEST(a_pure_altitude_step_enters_no_new_columns) {
    const WaterWindow a = waterWindowAt(0, 0, 0, 32, 16);
    const WaterWindow b = waterWindowAt(0, 0, 1, 32, 16);
    WaterWindow cols[kWaterWindowMaxColumnRegions];
    CHECK_EQ(waterWindowColumnDifference(b, a, cols), 0);
    // ...while the brick difference is a whole z layer.
    WaterWindow regions[kWaterWindowMaxRegions];
    const int n = waterWindowDifference(b, a, regions);
    int64_t total = 0;
    for (int i = 0; i < n; ++i) total += regions[i].count();
    CHECK_EQ(total, int64_t(65) * 65);
}

// The case that made the first version of waterWindowColumnDifference wrong: a
// camera that jumps FURTHER THAN THE WINDOW IS TALL (26.4 m at rZ = 16 -- a
// dive, or a teleport). Inferring the column set from the 3D peel then reports
// every surviving column as new, because the z peel spans the full new extent
// too. The 2D difference cannot make that mistake.
VXC_TEST(a_vertical_jump_past_the_window_still_reuses_its_columns) {
    const WaterWindow a = waterWindowAt(0, 0, 0, 32, 16);
    const WaterWindow b = waterWindowAt(0, 0, 100, 32, 16); // no z overlap at all
    WaterWindow cols[kWaterWindowMaxColumnRegions];
    CHECK_EQ(waterWindowColumnDifference(b, a, cols), 0);
}

// The column difference must itself be an exact, disjoint decomposition of the
// XY footprint difference -- the same discipline as the 3D one.
VXC_TEST(column_difference_is_exactly_the_footprint_difference) {
    const int64_t r = 3, rz = 2;
    const WaterWindow a = waterWindowAt(0, 0, 0, r, rz);
    for (int64_t dy = -9; dy <= 9; ++dy) {
        for (int64_t dx = -9; dx <= 9; ++dx) {
            const WaterWindow b = waterWindowAt(dx, dy, 0, r, rz);
            WaterWindow cols[kWaterWindowMaxColumnRegions];
            const int n = waterWindowColumnDifference(b, a, cols);
            CHECK(n <= kWaterWindowMaxColumnRegions);

            const std::set<Cell> ca = columnsOf(a), cb = columnsOf(b);
            std::set<Cell> expected;
            for (const Cell& c : cb)
                if (ca.find(c) == ca.end()) expected.insert(c);

            std::set<Cell> got;
            int64_t emitted = 0;
            for (int i = 0; i < n; ++i) {
                const std::set<Cell> rc = columnsOf(cols[i]);
                emitted += int64_t(rc.size());
                got.insert(rc.begin(), rc.end());
            }
            CHECK(got == expected);
            CHECK_EQ(emitted, int64_t(got.size()));
        }
    }
}

// Departure is the same operation with the arguments swapped, and the client
// needs it to destroy the components that left. Pinned so nobody adds a second
// function for it.
VXC_TEST(departed_bricks_are_the_difference_the_other_way) {
    const WaterWindow a = waterWindowAt(0, 0, 0, 4, 2);
    const WaterWindow b = waterWindowAt(2, 1, 0, 4, 2);
    WaterWindow gone[kWaterWindowMaxRegions];
    const int n = waterWindowDifference(a, b, gone);

    const std::set<Cell> sa = cellsOf(a), sb = cellsOf(b);
    std::set<Cell> expected;
    for (const Cell& c : sa)
        if (sb.find(c) == sb.end()) expected.insert(c);
    std::set<Cell> got;
    for (int i = 0; i < n; ++i) {
        const std::set<Cell> rc = cellsOf(gone[i]);
        got.insert(rc.begin(), rc.end());
    }
    CHECK(got == expected);
}

// Retained + entered = the new window, exactly. This is the statement that
// nothing is dropped on the floor during a step: every brick of the new window
// is either one the last window already held or one the difference reports.
VXC_TEST(retained_plus_entered_is_the_whole_new_window) {
    const WaterWindow a = waterWindowAt(0, 0, 0, 4, 3);
    for (int64_t dz = -8; dz <= 8; ++dz) {
        for (int64_t dy = -11; dy <= 11; ++dy) {
            for (int64_t dx = -11; dx <= 11; ++dx) {
                const WaterWindow b = waterWindowAt(dx, dy, dz, 4, 3);
                const WaterWindow keep = waterWindowIntersect(a, b);
                WaterWindow regions[kWaterWindowMaxRegions];
                const int n = waterWindowDifference(b, a, regions);
                int64_t total = keep.count();
                for (int i = 0; i < n; ++i) total += regions[i].count();
                CHECK_EQ(total, b.count());
            }
        }
    }
}

// The client's geometry, restated so a change to kImplicitRadiusBricks that
// forgets this file fails here rather than in a screenshot.
VXC_TEST(the_clients_own_window_is_65_by_65_by_33) {
    const WaterWindow w = waterWindowAt(0, 0, 0, 32, 16);
    CHECK_EQ(w.spanX(), 65);
    CHECK_EQ(w.spanY(), 65);
    CHECK_EQ(w.spanZ(), 33);
    CHECK_EQ(w.count(), int64_t(139425));
}

// An empty window is not a point at the origin. `contains` must refuse it, or
// the first frame reuses a brick it never built.
VXC_TEST(an_empty_window_contains_nothing) {
    const WaterWindow none;
    CHECK(none.empty());
    CHECK(!none.contains(0, 0, 0));
    CHECK_EQ(none.count(), 0);
    CHECK(waterWindowIntersect(none, waterWindowAt(0, 0, 0, 4, 4)).empty());
}
