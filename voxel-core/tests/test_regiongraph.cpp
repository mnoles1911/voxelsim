// Hierarchical region-graph pathfinding (plan §3.6 NPCs & AI, M6): the
// single-level portal-graph acceleration layer over pathfind.h's fine
// windowed voxel A*, proving portal detection on a hand-built doorway,
// hierarchical-vs-direct reachability agreement + cheapness, the "no
// unbounded fallback" doctrine when a region is sealed, incremental
// recompute matching a from-scratch rebuild, and determinism.

#include "voxelcore/regiongraph.h"
#include "vxctest.h"

#include <cstdio>

using namespace vxc;

VXC_TEST(regiongraph_single_doorway_produces_exactly_one_portal_each_side) {
    // Two regions along x (region (0,0,0) and (1,0,0)); a full-height,
    // full-width MAT_BEDROCK wall separates them at the boundary (x=16)
    // except a single-cell doorway at (y=8,z=1) -- the "wall with a single
    // doorway" scenario the plan names for portal detection. A MAT_ROCK
    // floor at z=0 spans the whole world so the doorway cell is supported.
    auto solid = [](int64_t x, int64_t y, int64_t z) -> MaterialId {
        if (x == 16 && y >= 0 && y <= 15 && z >= 1 && z <= 15) {
            if (y == 8 && z == 1) return MAT_AIR; // the doorway
            return MAT_BEDROCK;
        }
        return z == 0 ? MAT_ROCK : MAT_AIR;
    };
    const PathCostConfig config;
    RegionGraph g = buildRegionGraph(solid, RegionCoord{0, 0, 0}, RegionCoord{1, 0, 0}, config);

    // Only the shared x=16 boundary is in-bounds (the other 4 faces of each
    // region have no in-bounds neighbor) -- exactly 2 portals total, one
    // per side of the single doorway.
    CHECK_EQ(g.portals.size(), size_t(2));

    int foundA = 0, foundB = 0;
    for (const Portal& p : g.portals) {
        if (p.region == (RegionCoord{0, 0, 0}) && p.face == Face::PlusX) {
            ++foundA;
            CHECK(p.cell == (PathCoord{15, 8, 1}));
            CHECK_EQ(p.clusterSize, uint32_t(1));
        }
        if (p.region == (RegionCoord{1, 0, 0}) && p.face == Face::MinusX) {
            ++foundB;
            CHECK(p.cell == (PathCoord{16, 8, 1}));
            CHECK_EQ(p.clusterSize, uint32_t(1));
        }
    }
    CHECK_EQ(foundA, 1);
    CHECK_EQ(foundB, 1);

    // Exactly one inter-region edge each direction (Walk, symmetric cost).
    int interCount = 0;
    for (const PortalEdge& e : g.edges)
        if (!e.intraRegion) ++interCount;
    CHECK_EQ(interCount, 2);
}

VXC_TEST(regiongraph_hierarchical_matches_direct_reachability_and_is_cheaper) {
    // Five regions in a line (x region-index 0..4, i.e. world x 0..79),
    // fully open flat floor, no walls at all. A hierarchical corridor
    // across all 5 regions must agree with a direct world-spanning fine
    // findPath's reachability verdict, while spending far fewer fine-A*
    // expansions AT QUERY TIME (the number that matters for "hundreds of
    // NPCs asking for a path every tick" -- the one-time graph build cost
    // is amortized across every future query, so it is reported separately,
    // not folded into this comparison).
    auto solid = [](int64_t, int64_t, int64_t z) -> MaterialId { return z == 0 ? MAT_ROCK : MAT_AIR; };
    const PathCostConfig config;
    const RegionCoord minR{0, 0, 0}, maxR{4, 0, 0};
    RegionGraph g = buildRegionGraph(solid, minR, maxR, config);

    const PathCoord start{1, 8, 1};
    const PathCoord goal{78, 8, 1};

    const SearchWindow bigWindow{{0, 0, 0}, {79, 15, 15}, 2000000};
    PathResult direct = findPath(solid, start, goal, config, bigWindow);
    CHECK(direct.complete);

    HierarchicalPathResult hp = findHierarchicalPath(g, start, goal, solid, config, /*refine=*/true);
    CHECK(hp.corridor.found);
    CHECK(hp.refined);
    CHECK(hp.concretePath.complete);
    CHECK(hp.concretePath.reached == goal);

    std::printf(
        "  regiongraph_hierarchical...: direct=%d, corridor-only(entry+exit)=%lld, "
        "fully-refined=%d, one-time graph build=%lld\n",
        direct.expansionsUsed, static_cast<long long>(hp.entryExitExpansionsUsed),
        hp.concretePath.expansionsUsed, static_cast<long long>(g.totalFinePathExpansions));

    // The cheapness claim, two forms: (1) the corridor-only query (what
    // hundreds of NPCs actually pay per path request against an
    // already-built graph -- entry/exit region searches only, the portal
    // Dijkstra itself touches zero fine-A* cells) is dramatically cheaper
    // and does NOT grow with the number of regions crossed, unlike a direct
    // search; (2) even fully refined to a concrete step-by-step path, the
    // hierarchical route still costs fewer expansions than one
    // world-spanning search over the same start/goal.
    CHECK(hp.entryExitExpansionsUsed < direct.expansionsUsed / 2);
    CHECK(hp.concretePath.expansionsUsed < direct.expansionsUsed);
}

VXC_TEST(regiongraph_sealed_region_no_abstract_path_no_fallback) {
    // Same 5-region line, but a full-height, full-width MAT_BEDROCK wall
    // with NO doorway at x=32 (region (2,0,0)'s own MinusX boundary column)
    // completely seals region (1,0,0) from region (2,0,0). Neither a direct
    // fine search NOR the hierarchical query should find a path -- and the
    // hierarchical query must say so directly, not fall back to an
    // unbounded direct search.
    auto solid = [](int64_t x, int64_t y, int64_t z) -> MaterialId {
        if (x == 32 && y >= 0 && y <= 15 && z >= 0 && z <= 15) return MAT_BEDROCK; // full seal, incl. floor
        return z == 0 ? MAT_ROCK : MAT_AIR;
    };
    const PathCostConfig config;
    const RegionCoord minR{0, 0, 0}, maxR{4, 0, 0};
    RegionGraph g = buildRegionGraph(solid, minR, maxR, config);

    for (const Portal& p : g.portals) {
        const bool onSeal = (p.region == (RegionCoord{1, 0, 0}) && p.face == Face::PlusX) ||
                             (p.region == (RegionCoord{2, 0, 0}) && p.face == Face::MinusX);
        CHECK(!onSeal);
    }

    const PathCoord start{1, 8, 1};
    const PathCoord goal{78, 8, 1};

    const SearchWindow bigWindow{{0, 0, 0}, {79, 15, 15}, 2000000};
    PathResult direct = findPath(solid, start, goal, config, bigWindow);
    CHECK(!direct.complete); // sealed -- the direct search can't reach it either (matching verdict)

    HierarchicalPathResult hp = findHierarchicalPath(g, start, goal, solid, config, /*refine=*/true);
    CHECK(!hp.corridor.found);
    CHECK(!hp.refined);
}

VXC_TEST(regiongraph_incremental_recompute_matches_from_scratch_rebuild) {
    // Build a graph over the open 5-region world, then edit region (2,0,0)
    // (x 32..47) by adding a walled doorway at its OWN PlusX boundary
    // (x=47, facing region (3,0,0)) -- a "dig/build" edit entirely inside
    // region (2,0,0)'s own bricks. markRegionDirty(graph, (2,0,0), ...)
    // should touch only that region, its neighbors' mirrored portals, and
    // incident edges -- and the result must be byte-identical (per
    // digest()) to a from-scratch buildRegionGraph() over the post-edit
    // world.
    auto solidBefore = [](int64_t, int64_t, int64_t z) -> MaterialId { return z == 0 ? MAT_ROCK : MAT_AIR; };
    auto solidAfter = [](int64_t x, int64_t y, int64_t z) -> MaterialId {
        if (x == 47 && y >= 0 && y <= 15 && z >= 1 && z <= 15) {
            if (y == 8 && z == 1) return MAT_AIR;
            return MAT_BEDROCK;
        }
        return z == 0 ? MAT_ROCK : MAT_AIR;
    };
    const PathCostConfig config;
    const RegionCoord minR{0, 0, 0}, maxR{4, 0, 0};

    RegionGraph incremental = buildRegionGraph(solidBefore, minR, maxR, config);
    markRegionDirty(incremental, RegionCoord{2, 0, 0}, solidAfter, config);

    RegionGraph fromScratch = buildRegionGraph(solidAfter, minR, maxR, config);

    // Sanity: the edit actually changed something observable (the boundary
    // between (2,0,0) and (3,0,0) went from one big open portal to one
    // single-cell doorway) -- otherwise this test would trivially pass by
    // comparing two identical no-op graphs.
    uint32_t doorwayClusterSize = 0;
    bool foundDoorway = false;
    for (const Portal& p : fromScratch.portals) {
        if (p.alive && p.region == (RegionCoord{2, 0, 0}) && p.face == Face::PlusX) {
            foundDoorway = true;
            doorwayClusterSize = p.clusterSize;
        }
    }
    CHECK(foundDoorway);
    CHECK_EQ(doorwayClusterSize, uint32_t(1));

    Digest dInc, dScratch;
    incremental.digest(dInc);
    fromScratch.digest(dScratch);
    CHECK_EQ(dInc.h, dScratch.h);
}

VXC_TEST(regiongraph_determinism_golden_digest) {
    // Same single-doorway world as the first test, built twice -- identical
    // digest every time, pinned golden constant (print-once convention,
    // matching test_pathfind.cpp / test_connectivity.cpp).
    auto solid = [](int64_t x, int64_t y, int64_t z) -> MaterialId {
        if (x == 16 && y >= 0 && y <= 15 && z >= 1 && z <= 15) {
            if (y == 8 && z == 1) return MAT_AIR;
            return MAT_BEDROCK;
        }
        return z == 0 ? MAT_ROCK : MAT_AIR;
    };
    const PathCostConfig config;
    RegionGraph g1 = buildRegionGraph(solid, RegionCoord{0, 0, 0}, RegionCoord{1, 0, 0}, config);
    RegionGraph g2 = buildRegionGraph(solid, RegionCoord{0, 0, 0}, RegionCoord{1, 0, 0}, config);

    Digest d1, d2;
    g1.digest(d1);
    g2.digest(d2);
    CHECK_EQ(d1.h, d2.h);

    constexpr uint64_t kGoldenDigest = 0xeb05deb529b8f143ull; // pinned 2026-07-21
    std::printf("  regiongraph_determinism_golden_digest: digest=0x%016llx\n",
                static_cast<unsigned long long>(d1.h));
    CHECK_EQ(d1.h, kGoldenDigest);
}

VXC_TEST(regiongraph_time_sliced_build_matches_one_shot_build) {
    // The M6 Tier-1 enablement change (regiongraph.h header comment "Build
    // cost") makes the build RESUMABLE so the UE side can spend a bounded
    // slice of it per tick instead of stalling the game thread for the whole
    // thing. The contract that makes that safe is: a graph assembled from N
    // budgeted slices is BYTE-IDENTICAL to the same graph built in one
    // unlimited slice. buildRegionGraph() is itself the unlimited-slice case,
    // so this pins the two against each other over a multi-region box with
    // the tightest possible budget (ONE region of work per advance() call --
    // which forces a resume boundary at every single phase/region step,
    // including mid-phase and exactly on phase transitions).
    auto solid = [](int64_t x, int64_t y, int64_t z) -> MaterialId {
        // Rolling floor with a couple of walls, so regions differ from each
        // other and portal counts vary (a flat world would make the test
        // pass trivially).
        if (z == 0) return MAT_ROCK;
        if (x == 16 && z <= 6 && !(y >= 4 && y <= 6)) return MAT_ROCK;
        if (y == 16 && z <= 4 && !(x >= 20 && x <= 22)) return MAT_BEDROCK;
        if (z == 1 && ((x + y) % 11) == 0) return MAT_ROCK;
        return MAT_AIR;
    };
    PathCostConfig config;
    const RegionCoord minR{0, 0, 0}, maxR{2, 2, 1}; // 18 regions

    const RegionGraph oneShot = buildRegionGraph(solid, minR, maxR, config);

    RegionGraphBuilder builder(minR, maxR);
    int64_t slices = 0;
    // NOTE the deliberately hostile calling convention: a FRESH, TEMPORARY
    // MaterialFn per advance() call, destroyed the instant the call returns.
    // That is exactly how the UE caller drives this (it builds its solidFn
    // lambda inside the per-tick function), and the builder keeps a
    // RegionMaterialCache alive ACROSS calls for the region it is part-way
    // through -- so a cache that captured the first call's MaterialFn by
    // pointer and never rebound would read freed memory on the next call.
    // It did, and this is the regression pin for it (see
    // RegionMaterialCache's LIFETIME CONTRACT).
    while (!builder.advance(MaterialFn(solid), config, /*regionBudget=*/1)) ++slices;
    ++slices;

    // Every advance() must have made progress and the walk must have taken
    // exactly one slice per (phase, region) step -- i.e. the budget is
    // actually honored, not silently ignored.
    CHECK_EQ(slices, builder.totalRegionSteps());
    CHECK_EQ(builder.completedRegionSteps(), builder.totalRegionSteps());

    Digest dOne, dSliced;
    oneShot.digest(dOne);
    builder.graph().digest(dSliced);
    CHECK_EQ(dOne.h, dSliced.h);

    // Sanity: the fixture is actually exercising something (a world with no
    // portals would make the digest equality vacuous).
    CHECK(oneShot.portals.size() > 0);
    CHECK(oneShot.edges.size() > 0);
}
