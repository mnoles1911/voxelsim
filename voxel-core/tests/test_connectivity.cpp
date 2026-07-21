// Connectivity flood-fill (plan §3.5 Destruction & voxel bodies): the M5
// groundwork primitive that decides which voxels of a structural edit's
// affected region stay standing vs. become floating debris islands.

#include "voxelcore/connectivity.h"
// ODR / unity-build regression guard (see connectivity.h's note on
// `connectivity_detail`): pathfind.h also opens a nested detail namespace with
// an identically-signed localIndex. UE's adaptive unity build routinely lands
// both headers in one translation unit (VoxelWorldSubsystem.cpp includes
// connectivity.h, VoxelAgentSubsystem.cpp includes pathfind.h), which used to
// fail the VoxelEarth module build with "redefinition of 'localIndex'". This
// include makes that a compile-time test: if the two headers ever collide
// again, THIS FILE stops compiling.
#include "voxelcore/pathfind.h"
#include "vxctest.h"

#include <chrono>
#include <cstdio>

using namespace vxc;

namespace {

bool inBox(int64_t x, int64_t y, int64_t z, int64_t x0, int64_t x1, int64_t y0, int64_t y1,
           int64_t z0, int64_t z1) {
    return x >= x0 && x <= x1 && y >= y0 && y <= y1 && z >= z0 && z <= z1;
}

} // namespace

VXC_TEST(connectivity_single_blob_one_component) {
    // 3x3x3 solid cube inside a 5x5x5 analysis box -> 1 component, exact
    // voxel count, minCoord == the cube's own min corner.
    auto solid = [](int64_t x, int64_t y, int64_t z) { return inBox(x, y, z, 1, 3, 1, 3, 1, 3); };
    ConnectivityResult r = findComponents(solid, VoxelCoord{0, 0, 0}, VoxelCoord{4, 4, 4});

    CHECK_EQ(r.componentCount, 1);
    CHECK_EQ(r.components.size(), size_t(1));
    CHECK_EQ(r.components[0].size(), size_t(27));
    CHECK(r.components[0].minCoord() == (VoxelCoord{1, 1, 1}));
}

VXC_TEST(connectivity_two_separated_blobs_deterministic_order) {
    // Two 2x2x2 cubes separated by a >=1-voxel air gap in every axis -> 2
    // components, ordered min-coord-first (the low cube before the high one).
    auto solid = [](int64_t x, int64_t y, int64_t z) {
        return inBox(x, y, z, 0, 1, 0, 1, 0, 1) || inBox(x, y, z, 5, 6, 5, 6, 5, 6);
    };
    ConnectivityResult r = findComponents(solid, VoxelCoord{0, 0, 0}, VoxelCoord{7, 7, 7});

    CHECK_EQ(r.componentCount, 2);
    CHECK_EQ(r.components.size(), size_t(2));
    CHECK_EQ(r.components[0].size(), size_t(8));
    CHECK_EQ(r.components[1].size(), size_t(8));
    CHECK(r.components[0].minCoord() == (VoxelCoord{0, 0, 0}));
    CHECK(r.components[1].minCoord() == (VoxelCoord{5, 5, 5}));

    // Determinism: identical rerun.
    ConnectivityResult r2 = findComponents(solid, VoxelCoord{0, 0, 0}, VoxelCoord{7, 7, 7});
    Digest d1, d2;
    r.digest(d1);
    r2.digest(d2);
    CHECK_EQ(d1.h, d2.h);
}

VXC_TEST(connectivity_tree_cut_trunk_creates_floating_canopy_island) {
    // Vertical trunk (x=5,y=5, z 0..9) + canopy block (x 3..7, y 3..7, z
    // 8..12). Cutting trunk voxel z=4 severs the stump (z 0..3, grounded)
    // from the canopy + upper trunk (z 5..12, floating) -- the concrete
    // "chopped tree" scenario the plan's destruction section names.
    auto baseSolid = [](int64_t x, int64_t y, int64_t z) {
        const bool trunk = (x == 5 && y == 5 && z >= 0 && z <= 9);
        const bool canopy = inBox(x, y, z, 3, 7, 3, 7, 8, 12);
        return trunk || canopy;
    };
    auto cutSolid = [&](int64_t x, int64_t y, int64_t z) {
        if (x == 5 && y == 5 && z == 4) return false; // the cut
        return baseSolid(x, y, z);
    };

    const VoxelCoord minCorner{0, 0, 0};
    const VoxelCoord maxCorner{10, 10, 14};
    ConnectivityResult r = findComponents(cutSolid, minCorner, maxCorner);

    CHECK_EQ(r.componentCount, 2);
    // Stump: trunk z=0..3 at (5,5,*) -> 4 voxels, lowest z so first in order.
    CHECK_EQ(r.components[0].size(), size_t(4));
    CHECK(r.components[0].minCoord() == (VoxelCoord{5, 5, 0}));
    // Canopy + upper trunk: canopy (5x5x5=125) plus trunk z=5..7 (3 voxels
    // not already inside the canopy box; z=8,9 trunk voxels overlap canopy).
    CHECK_EQ(r.components[1].size(), size_t(128));
    CHECK(r.components[1].minCoord() == (VoxelCoord{5, 5, 5}));

    IslandAnalysis islands =
        findDisconnectedIslands(cutSolid, minCorner, maxCorner, bottomFaceAnchor(minCorner.z));
    CHECK_EQ(islands.anchoredComponentIndices.size(), size_t(1));
    CHECK_EQ(islands.islandComponentIndices.size(), size_t(1));
    // The anchored one is the stump (component 0); the island is the canopy
    // (component 1).
    CHECK_EQ(islands.anchoredComponentIndices[0], int32_t(0));
    CHECK_EQ(islands.islandComponentIndices[0], int32_t(1));
    CHECK_EQ(islands.connectivity.components[size_t(islands.islandComponentIndices[0])].size(),
             size_t(128));
}

VXC_TEST(connectivity_diagonal_adjacency_not_connected) {
    // 6-connectivity only: two voxels touching solely along an edge
    // (differ in x AND y, same z) must NOT connect.
    auto solid = [](int64_t x, int64_t y, int64_t z) {
        return (x == 0 && y == 0 && z == 0) || (x == 1 && y == 1 && z == 0);
    };
    ConnectivityResult r = findComponents(solid, VoxelCoord{0, 0, 0}, VoxelCoord{1, 1, 0});
    CHECK_EQ(r.componentCount, 2);
    CHECK_EQ(r.components[0].size(), size_t(1));
    CHECK_EQ(r.components[1].size(), size_t(1));

    // Corner-diagonal (differ in all 3 axes) must also NOT connect.
    auto solidCorner = [](int64_t x, int64_t y, int64_t z) {
        return (x == 0 && y == 0 && z == 0) || (x == 1 && y == 1 && z == 1);
    };
    ConnectivityResult rc = findComponents(solidCorner, VoxelCoord{0, 0, 0}, VoxelCoord{1, 1, 1});
    CHECK_EQ(rc.componentCount, 2);
}

VXC_TEST(connectivity_determinism_golden_digest) {
    // Three disjoint blobs of distinct sizes -> pinned golden digest
    // (printed once, then hardcoded per the "print-once" convention).
    auto solid = [](int64_t x, int64_t y, int64_t z) {
        return inBox(x, y, z, 0, 1, 0, 1, 0, 1) ||       // 2x2x2 = 8
               inBox(x, y, z, 3, 3, 3, 5, 3, 3) ||        // 1x3x1 = 3
               inBox(x, y, z, 7, 9, 7, 9, 7, 9);          // 3x3x3 = 27
    };
    const VoxelCoord minCorner{0, 0, 0};
    const VoxelCoord maxCorner{9, 9, 9};

    ConnectivityResult r1 = findComponents(solid, minCorner, maxCorner);
    ConnectivityResult r2 = findComponents(solid, minCorner, maxCorner);
    CHECK_EQ(r1.componentCount, 3);
    CHECK_EQ(r1.components[0].size(), size_t(8));
    CHECK_EQ(r1.components[1].size(), size_t(3));
    CHECK_EQ(r1.components[2].size(), size_t(27));

    Digest d1, d2;
    r1.digest(d1);
    r2.digest(d2);
    CHECK_EQ(d1.h, d2.h); // same input -> identical digest, run over run

    constexpr uint64_t kGoldenDigest = 0xeccfa24f24b88702ull; // pinned 2026-07-20
    std::printf("  connectivity_determinism_golden_digest: digest=0x%016llx\n",
                static_cast<unsigned long long>(d1.h));
    CHECK_EQ(d1.h, kGoldenDigest);
}

VXC_TEST(connectivity_fully_spanning_box_one_component_no_islands) {
    // Every voxel in the box is solid -> 1 component, and under a
    // bottom-face anchor it's grounded (no islands at all).
    auto solid = [](int64_t, int64_t, int64_t) { return true; };
    const VoxelCoord minCorner{0, 0, 0};
    const VoxelCoord maxCorner{3, 3, 3};
    ConnectivityResult r = findComponents(solid, minCorner, maxCorner);
    CHECK_EQ(r.componentCount, 1);
    CHECK_EQ(r.components[0].size(), size_t(4 * 4 * 4));

    IslandAnalysis islands =
        findDisconnectedIslands(solid, minCorner, maxCorner, bottomFaceAnchor(minCorner.z));
    CHECK_EQ(islands.anchoredComponentIndices.size(), size_t(1));
    CHECK_EQ(islands.islandComponentIndices.size(), size_t(0));
}

VXC_TEST(connectivity_perf_sanity_64_cubed) {
    // Reference-impl perf sanity, not a hard gate: a 64^3 region, mostly
    // solid with scattered single-voxel holes (so the flood fill actually
    // has to branch around obstacles rather than walk one giant contiguous
    // fill), should flood in well under a second -- proof this isn't
    // accidentally O(n^2).
    auto solid = [](int64_t x, int64_t y, int64_t z) { return (x + y * 3 + z * 7) % 97 != 0; };
    const VoxelCoord minCorner{0, 0, 0};
    const VoxelCoord maxCorner{63, 63, 63};

    const auto t0 = std::chrono::steady_clock::now();
    ConnectivityResult r = findComponents(solid, minCorner, maxCorner);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::printf("  connectivity_perf_sanity_64_cubed: %d component(s), %.2f ms\n",
                r.componentCount, ms);
    CHECK(r.componentCount >= 1);
    CHECK(ms < 3000.0);
}
