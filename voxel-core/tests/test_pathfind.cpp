// Dig-aware windowed voxel A* (plan §3.6 NPCs & AI): the M6-groundwork
// primitive proving "walking, mining, tunneling, bridging fall out of ONE
// cost function" — same findPath() code, only PathCostConfig's weights
// change, and the chosen action sequence flips between tunneling through an
// obstacle and detouring around it.

#include "voxelcore/pathfind.h"
#include "vxctest.h"

#include <cstdio>

using namespace vxc;

namespace {

int countAction(const PathResult& r, Action a) {
    int n = 0;
    for (const PathStep& s : r.steps)
        if (s.action == a) ++n;
    return n;
}

} // namespace

VXC_TEST(pathfind_open_air_straight_line_all_walk) {
    // Flat ground (MAT_BEDROCK at z=-1, unconditionally unmineable so there
    // is never a cheaper "dig a shortcut through the floor" alternative)
    // everywhere, nothing else -> a straight line is the only sensible
    // path, entirely Walk, cost = distance x walkCost.
    auto solid = [](int64_t, int64_t, int64_t z) -> MaterialId {
        return z == -1 ? MAT_BEDROCK : MAT_AIR;
    };
    const PathCostConfig config;
    const SearchWindow window{{-10, -10, -10}, {10, 10, 10}, 100000};

    PathResult r = findPath(solid, PathCoord{0, 0, 0}, PathCoord{5, 0, 0}, config, window);

    CHECK(r.complete);
    CHECK(!r.capped);
    CHECK_EQ(r.steps.size(), size_t(5));
    for (const PathStep& s : r.steps) CHECK(s.action == Action::Walk);
    CHECK_EQ(r.totalCost, int64_t(5 * config.walkCost));
    CHECK(r.reached == (PathCoord{5, 0, 0}));
}

VXC_TEST(pathfind_diggable_wall_tunnel_vs_around_same_code) {
    // A single-voxel-thick MAT_ROCK wall at x=5, spanning y in [-2,2] and
    // z in [0,3], with a continuous MAT_BEDROCK ground plane (z=-1)
    // everywhere (including under/around the wall) -- bedrock so the floor
    // itself is never a cheaper "dig through" shortcut, isolating the
    // mine-cost tradeoff to the wall alone. Straight-line distance
    // start(0,0,0) -> goal(10,0,0) is 10; the wall sits directly on that
    // line and can only be crossed by mining exactly one voxel (5,0,0) or
    // detouring in y past y=+-2. THE PROOF: identical solidFn/start/goal/
    // window, only PathCostConfig.mineCostByMaterial[MAT_ROCK] changes, and
    // the action sequence flips between "tunnel through" and "go around".
    auto solid = [](int64_t x, int64_t y, int64_t z) -> MaterialId {
        if (x == 5 && y >= -2 && y <= 2 && z >= 0 && z <= 3) return MAT_ROCK;
        if (z == -1) return MAT_BEDROCK;
        return MAT_AIR;
    };
    const PathCoord start{0, 0, 0};
    const PathCoord goal{10, 0, 0};
    const SearchWindow window{{-10, -10, -10}, {10, 10, 10}, 100000};

    PathCostConfig cheapMine;
    cheapMine.mineCostByMaterial[MAT_ROCK] = 1; // far cheaper than any detour
    PathResult tunnel = findPath(solid, start, goal, cheapMine, window);
    CHECK(tunnel.complete);
    CHECK_EQ(countAction(tunnel, Action::Mine), 1);
    CHECK_EQ(tunnel.steps.size(), size_t(10)); // straight line, one cell mined
    CHECK(tunnel.steps[4].action == Action::Mine); // the 5th move, x=4->5, breaches the wall
    CHECK(tunnel.steps[4].affectedCell == (PathCoord{5, 0, 0}));

    PathCostConfig expensiveMine;
    expensiveMine.mineCostByMaterial[MAT_ROCK] = 1000; // far pricier than detouring
    PathResult around = findPath(solid, start, goal, expensiveMine, window);
    CHECK(around.complete);
    CHECK_EQ(countAction(around, Action::Mine), 0);
    CHECK(around.steps.size() > size_t(10)); // detour is longer than the beeline

    // Same world, same start/goal, same window, same code path -- only the
    // cost function's weights differ.
    CHECK(tunnel.totalCost < around.totalCost * 100); // tunneling is the cheap option here
}

VXC_TEST(pathfind_bedrock_never_mined_forces_detour) {
    // Identical wall shape to the tunnel test, but MAT_BEDROCK -- and the
    // mine cost table is set CHEAP for bedrock too, to prove the impassable
    // rule is hard-coded, not config-driven. Ground is bedrock too (never
    // mineable, so it can't become an accidental shortcut).
    auto solid = [](int64_t x, int64_t y, int64_t z) -> MaterialId {
        if (x == 5 && y >= -2 && y <= 2 && z >= 0 && z <= 3) return MAT_BEDROCK;
        if (z == -1) return MAT_BEDROCK;
        return MAT_AIR;
    };
    PathCostConfig config;
    config.mineCostByMaterial[MAT_BEDROCK] = 0; // deliberately cheap -- must still be ignored
    const SearchWindow window{{-10, -10, -10}, {10, 10, 10}, 100000};

    PathResult r = findPath(solid, PathCoord{0, 0, 0}, PathCoord{10, 0, 0}, config, window);

    CHECK(r.complete);
    CHECK_EQ(countAction(r, Action::Mine), 0);
    CHECK(r.steps.size() > size_t(10));
}

VXC_TEST(pathfind_gap_bridge_vs_around_same_code) {
    // A 3-voxel-wide missing-floor gap (x in [5,7], y in [-2,2]) too wide
    // for a single 2-voxel Jump to land inside directly -- crossing it
    // needs at least one Bridge (placing a scaffold on unsupported air).
    // Same proof shape as the wall test: only PathCostConfig.bridgeCost
    // changes. (A cheap-enough config can legitimately mix in a trailing
    // Jump off an already-bridged cell -- classifyMove doesn't require the
    // JUMPING cell itself to be on original ground, only that its own
    // landing was reached by some valid prior action, which a Bridge
    // provides; the assertions below only pin down the header comment's
    // actual claim -- bridging gets used when cheap, never when
    // expensive -- not one specific optimal action sequence.)
    auto solid = [](int64_t x, int64_t y, int64_t z) -> MaterialId {
        const bool inGap = x >= 5 && x <= 7 && y >= -2 && y <= 2;
        if (z == -1 && !inGap) return MAT_BEDROCK;
        return MAT_AIR;
    };
    const PathCoord start{0, 0, 0};
    const PathCoord goal{12, 0, 0};
    const SearchWindow window{{-10, -10, -10}, {20, 10, 10}, 200000};

    PathCostConfig cheapBridge;
    // Just above walkCost (so bridging real ground instead of walking it
    // is never attractive) but far below the y-detour's cost -- isolates
    // the "cross the gap directly" tradeoff instead of making Bridge
    // globally cheaper than Walk everywhere.
    cheapBridge.bridgeCost = 12;
    PathResult bridged = findPath(solid, start, goal, cheapBridge, window);
    CHECK(bridged.complete);
    CHECK(countAction(bridged, Action::Bridge) >= 1);
    CHECK(bridged.steps.size() <= size_t(12));

    PathCostConfig expensiveBridge;
    expensiveBridge.bridgeCost = 1000;
    PathResult around = findPath(solid, start, goal, expensiveBridge, window);
    CHECK(around.complete);
    CHECK_EQ(countAction(around, Action::Bridge), 0);
    CHECK(around.steps.size() > size_t(12));
}

VXC_TEST(pathfind_ledge_step_up_and_step_down) {
    // Ground is one voxel higher for x>=3 than for x<3 -- crossing that
    // seam is exactly the StepUp/StepDown neighbor case (horizontal +1
    // combined with one voxel of rise/drop), not Walk, Mine, or Bridge.
    auto solid = [](int64_t x, int64_t, int64_t z) -> MaterialId {
        if (x < 3 && z == -1) return MAT_BEDROCK;
        if (x >= 3 && z == 0) return MAT_BEDROCK;
        return MAT_AIR;
    };
    const SearchWindow window{{-10, -10, -10}, {10, 10, 10}, 100000};
    PathCostConfig config;

    PathResult up = findPath(solid, PathCoord{0, 0, 0}, PathCoord{6, 0, 1}, config, window);
    CHECK(up.complete);
    CHECK_EQ(countAction(up, Action::StepUp), 1);
    CHECK_EQ(countAction(up, Action::StepDown), 0);
    CHECK_EQ(up.steps.size(), size_t(6));

    PathResult down = findPath(solid, PathCoord{6, 0, 1}, PathCoord{0, 0, 0}, config, window);
    CHECK(down.complete);
    CHECK_EQ(countAction(down, Action::StepDown), 1);
    CHECK_EQ(countAction(down, Action::StepUp), 0);
}

VXC_TEST(pathfind_jump_over_narrow_gap_when_cheap) {
    // A single-voxel-wide (in x) missing-floor slice spanning every y --
    // no detour is possible within the window, so the only ways across are
    // Jump (2-voxel leap, no block placed) or Bridge (placed scaffolds).
    // Cheap jumpGapCost + expensive bridgeCost forces Jump.
    auto solid = [](int64_t x, int64_t, int64_t z) -> MaterialId {
        if (x == 5) return MAT_AIR; // the gap: no floor at any y here
        return z == -1 ? MAT_BEDROCK : MAT_AIR;
    };
    PathCostConfig config;
    config.jumpGapCost = 1;
    config.bridgeCost = 1000;
    const SearchWindow window{{-5, -5, -5}, {15, 5, 5}, 100000};

    PathResult r = findPath(solid, PathCoord{0, 0, 0}, PathCoord{10, 0, 0}, config, window);

    CHECK(r.complete);
    CHECK_EQ(countAction(r, Action::Jump), 1);
    CHECK_EQ(countAction(r, Action::Bridge), 0);
    CHECK_EQ(r.steps.size(), size_t(9)); // 10 voxels of distance, one step covers 2
}

VXC_TEST(pathfind_determinism_golden_digest) {
    // Same tunnel scenario as the wall test, run twice -- identical steps
    // and an identical digest every time.
    auto solid = [](int64_t x, int64_t y, int64_t z) -> MaterialId {
        if (x == 5 && y >= -2 && y <= 2 && z >= 0 && z <= 3) return MAT_ROCK;
        if (z == -1) return MAT_BEDROCK;
        return MAT_AIR;
    };
    PathCostConfig config;
    config.mineCostByMaterial[MAT_ROCK] = 1;
    const SearchWindow window{{-10, -10, -10}, {10, 10, 10}, 100000};
    const PathCoord start{0, 0, 0};
    const PathCoord goal{10, 0, 0};

    PathResult r1 = findPath(solid, start, goal, config, window);
    PathResult r2 = findPath(solid, start, goal, config, window);

    CHECK_EQ(r1.steps.size(), r2.steps.size());
    for (size_t i = 0; i < r1.steps.size(); ++i) {
        CHECK(r1.steps[i].action == r2.steps[i].action);
        CHECK(r1.steps[i].cell == r2.steps[i].cell);
    }

    Digest d1, d2;
    r1.digest(d1);
    r2.digest(d2);
    CHECK_EQ(d1.h, d2.h);

    constexpr uint64_t kGoldenDigest = 0xa88bdd2f0eb8afd1ull; // pinned 2026-07-20
    std::printf("  pathfind_determinism_golden_digest: digest=0x%016llx\n",
                static_cast<unsigned long long>(d1.h));
    CHECK_EQ(d1.h, kGoldenDigest);
}

VXC_TEST(pathfind_windowing_unreachable_within_sealed_box_capped) {
    // A full-height, full-width MAT_BEDROCK wall at x=4 spans the window's
    // entire y and z extent -- start and goal end up in disconnected
    // halves of the window with no way around (the window boundary itself
    // blocks any detour), so the search must exhaust its whole reachable
    // region and report capped=true without hanging.
    auto solid = [](int64_t x, int64_t y, int64_t z) -> MaterialId {
        if (x == 4 && y >= -3 && y <= 3 && z >= 0 && z <= 4) return MAT_BEDROCK;
        if (z == 0) return MAT_ROCK;
        return MAT_AIR;
    };
    PathCostConfig config;
    const SearchWindow window{{0, -3, 0}, {8, 3, 4}, 100000};

    PathResult r = findPath(solid, PathCoord{1, 0, 1}, PathCoord{7, 0, 1}, config, window);

    CHECK(!r.complete);
    CHECK(r.capped);
    CHECK(r.expansionsUsed < window.maxExpansions); // stopped by exhaustion, not the cap
    CHECK(r.expansionsUsed > 0);
}

VXC_TEST(pathfind_windowing_maxExpansions_cap_bounds_search) {
    // Open flat ground, a genuinely reachable goal 50 voxels away, but a
    // maxExpansions cap of 3 -- the search must stop after exactly 3
    // expansions and report capped=true, proving the cap (not just window
    // exhaustion) bounds the work regardless of how easy the goal is.
    auto solid = [](int64_t, int64_t, int64_t z) -> MaterialId {
        return z == -1 ? MAT_ROCK : MAT_AIR;
    };
    PathCostConfig config;
    const SearchWindow window{{-5, -5, -5}, {60, 5, 5}, 3};

    PathResult r = findPath(solid, PathCoord{0, 0, 0}, PathCoord{50, 0, 0}, config, window);

    CHECK(!r.complete);
    CHECK(r.capped);
    CHECK_EQ(r.expansionsUsed, int32_t(3));
    CHECK(!(r.reached == (PathCoord{50, 0, 0})));
}

VXC_TEST(pathfind_still_valid_detects_dirty_cell) {
    // The straight-line flat-ground path from the first test: valid against
    // the world it was computed on; a single mutated cell along the route
    // (as if a dirty-brick edit had landed there) must flip it to invalid.
    auto solid = [](int64_t, int64_t, int64_t z) -> MaterialId {
        return z == -1 ? MAT_BEDROCK : MAT_AIR;
    };
    const PathCostConfig config;
    const SearchWindow window{{-10, -10, -10}, {10, 10, 10}, 100000};
    PathResult r = findPath(solid, PathCoord{0, 0, 0}, PathCoord{5, 0, 0}, config, window);
    CHECK(r.complete);

    CHECK(pathStillValid(r, solid, config));

    auto mutatedSolid = [](int64_t x, int64_t y, int64_t z) -> MaterialId {
        if (x == 3 && y == 0 && z == 0) return MAT_ROCK; // a block appeared on the route
        return z == -1 ? MAT_BEDROCK : MAT_AIR;
    };
    CHECK(!pathStillValid(r, mutatedSolid, config));
}
