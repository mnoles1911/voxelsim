// Structural collapse for large edits (plan §4 M5) — the differential coarse
// support model in voxelcore/collapse.h.
//
// The tests below are organised around the three claims the header makes, in
// order of how badly a regression in each would hurt:
//   1. NO FALSE POSITIVES: ground never collapses; already-unsupported natural
//      geology is grandfathered; shrinking/clamping the region can only remove
//      collapse decisions, never add one.
//   2. THE MONEY CASE: blowing out the supports under a span collapses it,
//      including when the span is still incidentally touching something.
//   3. DETERMINISM: support distance is a shortest-path function (compared
//      against an order-independent Bellman-Ford fixpoint), component
//      splitting is input-order-independent, and a golden digest is pinned.

#include "voxelcore/collapse.h"
#include "vxctest.h"

#include <random>
#include <set>
#include <tuple>

using namespace vxc;

namespace {

using CellSet = std::set<std::tuple<int64_t, int64_t, int64_t>>;

CoarseOccupiedFn setOccupancy(const CellSet& s) {
    return [&s](int64_t x, int64_t y, int64_t z) {
        return s.find(std::make_tuple(x, y, z)) != s.end();
    };
}

void addBox(CellSet& s, int64_t x0, int64_t x1, int64_t y0, int64_t y1, int64_t z0, int64_t z1) {
    for (int64_t z = z0; z <= z1; ++z)
        for (int64_t y = y0; y <= y1; ++y)
            for (int64_t x = x0; x <= x1; ++x) s.insert(std::make_tuple(x, y, z));
}

void removeBox(CellSet& s, int64_t x0, int64_t x1, int64_t y0, int64_t y1, int64_t z0, int64_t z1) {
    for (int64_t z = z0; z <= z1; ++z)
        for (int64_t y = y0; y <= y1; ++y)
            for (int64_t x = x0; x <= x1; ++x) s.erase(std::make_tuple(x, y, z));
}

bool hasCell(const CollapseAnalysis& a, int64_t x, int64_t y, int64_t z) {
    for (const VoxelCoord& c : a.collapsingCells)
        if (c.x == x && c.y == y && c.z == z) return true;
    return false;
}

// Order-independent reference for computeSupport: relax EVERY edge repeatedly
// until nothing changes (Bellman-Ford to fixpoint). A fixpoint of the
// relaxation is the shortest-path distance function by definition and cannot
// depend on the order edges are visited in, so agreeing with it is a real
// proof that the bucket queue is not smuggling traversal order into the
// result.
std::vector<int32_t> bruteForceDist(const SupportField& f, int32_t budget) {
    const size_t n = f.occupied.size();
    std::vector<int32_t> d(n, kUnsupportedDist);
    for (int64_t lz = 0; lz < f.dz; ++lz)
        for (int64_t ly = 0; ly < f.dy; ++ly)
            for (int64_t lx = 0; lx < f.dx; ++lx) {
                const bool onSideOrBottom =
                    (lz == 0) || (lx == 0) || (lx == f.dx - 1) || (ly == 0) || (ly == f.dy - 1);
                const size_t i = f.localCellIndex(lx, ly, lz);
                if (onSideOrBottom && f.occupied[i]) d[i] = 0;
            }
    const std::array<std::array<int64_t, 4>, 6> steps = {{
        {{-1, 0, 0, 1}}, {{1, 0, 0, 1}}, {{0, -1, 0, 1}},
        {{0, 1, 0, 1}}, {{0, 0, -1, 0}}, {{0, 0, 1, 0}},
    }};
    bool changed = true;
    while (changed) {
        changed = false;
        for (int64_t lz = 0; lz < f.dz; ++lz)
            for (int64_t ly = 0; ly < f.dy; ++ly)
                for (int64_t lx = 0; lx < f.dx; ++lx) {
                    const size_t i = f.localCellIndex(lx, ly, lz);
                    if (!f.occupied[i] || d[i] == kUnsupportedDist) continue;
                    for (const auto& s : steps) {
                        const int64_t nx = lx + s[0], ny = ly + s[1], nz = lz + s[2];
                        if (nx < 0 || nx >= f.dx || ny < 0 || ny >= f.dy || nz < 0 || nz >= f.dz)
                            continue;
                        const size_t j = f.localCellIndex(nx, ny, nz);
                        if (!f.occupied[j]) continue;
                        const int32_t nd = d[i] + static_cast<int32_t>(s[3]);
                        if (nd > budget) continue;
                        if (nd < d[j]) {
                            d[j] = nd;
                            changed = true;
                        }
                    }
                }
    }
    return d;
}

} // namespace

// =====================================================================
// 1. No false positives
// =====================================================================

VXC_TEST(collapse_flat_ground_never_collapses) {
    // A solid heightfield-style ground mass filling the bottom of the region.
    // Every column stands on the bottom face, vertical steps are free, so
    // every ground cell has lateral distance 0 — the whole mass is supported
    // before AND after. Nothing collapses even though the edit removed cells.
    CellSet before;
    addBox(before, 0, 19, 0, 19, 0, 7);
    CellSet after = before;
    removeBox(after, 8, 11, 8, 11, 4, 7); // a crater in the middle

    const CollapseAnalysis a = findCollapsingCells(setOccupancy(before), setOccupancy(after),
                                                   VoxelCoord{0, 0, 0}, VoxelCoord{19, 19, 19});
    CHECK_EQ(a.collapsingCells.size(), size_t(0));
    CHECK(a.supportedCellsAfter > 0);
}

VXC_TEST(collapse_grandfathered_overhang_survives) {
    // A natural arch/undercut: a long horizontal ledge cantilevered 12 cells
    // out from a cliff — far past the 6-cell budget, so most of it is NOT
    // supported under the model even BEFORE anything is edited. An unrelated
    // edit elsewhere must not disintegrate it: the differential rule requires
    // supported-BEFORE, and these cells never were.
    CellSet before;
    addBox(before, 0, 3, 0, 9, 0, 9);   // cliff body against the -x boundary
    addBox(before, 4, 16, 4, 5, 9, 9);  // ledge sticking out 13 cells at the top
    CellSet after = before;
    removeBox(after, 2, 3, 0, 1, 0, 3); // unrelated dig into the cliff foot

    const SupportField sf = computeSupport(setOccupancy(before), VoxelCoord{0, 0, 0},
                                           VoxelCoord{19, 19, 19}, 6);
    CHECK(!sf.isSupported(16, 4, 9));   // the ledge tip really is unsupported pre-edit
    CHECK(sf.isSupported(4, 4, 9));     // and its root really is supported

    const CollapseAnalysis a = findCollapsingCells(setOccupancy(before), setOccupancy(after),
                                                   VoxelCoord{0, 0, 0}, VoxelCoord{19, 19, 19});
    // Nothing at all: the ledge tip was already unsupported (grandfathered),
    // and the cliff itself is anchored on the -x side boundary.
    CHECK_EQ(a.collapsingCells.size(), size_t(0));
}

VXC_TEST(collapse_shrinking_region_only_removes_decisions) {
    // THE SOUNDNESS PROPERTY that the old bounded-box design lacked. Clamping
    // the analysis region turns interior cells into BOUNDARY cells, which are
    // anchored in both the before and the after pass and therefore can never
    // satisfy the collapse predicate. So the collapse set of a smaller region
    // must be a SUBSET of the collapse set of a larger one — under-sizing the
    // region costs recall, never precision. (This is what lets the UE caller
    // cap the region as hard as the frame budget needs.)
    CellSet before;
    addBox(before, 0, 29, 0, 29, 0, 2);   // ground
    addBox(before, 12, 13, 12, 13, 3, 9); // pillar
    addBox(before, 8, 17, 8, 17, 10, 10); // slab on top of the pillar
    CellSet after = before;
    removeBox(after, 12, 13, 12, 13, 3, 9); // blow the pillar out

    const CollapseAnalysis big = findCollapsingCells(setOccupancy(before), setOccupancy(after),
                                                     VoxelCoord{0, 0, 0}, VoxelCoord{29, 29, 19});
    CHECK(big.collapsingCells.size() > 0);

    for (int64_t shrink = 1; shrink <= 8; ++shrink) {
        const CollapseAnalysis small =
            findCollapsingCells(setOccupancy(before), setOccupancy(after),
                                VoxelCoord{shrink, shrink, 0},
                                VoxelCoord{29 - shrink, 29 - shrink, 19 - shrink});
        for (const VoxelCoord& c : small.collapsingCells) {
            CHECK(hasCell(big, c.x, c.y, c.z));
        }
    }
}

VXC_TEST(collapse_no_zigzag_budget_loophole) {
    // A 2-cell-thick slab cantilevered off a wall. An earlier formulation
    // "reset the budget on every vertical step", which let a thick slab
    // zig-zag up/lateral/down/lateral and span unbounded distance. With
    // "vertical free, lateral costs 1, never reset", a 2-thick slab spans
    // exactly the budget and no further.
    CellSet s;
    addBox(s, 0, 1, 0, 9, 0, 19);   // wall on the -x boundary (x==0 is anchored)
    addBox(s, 2, 16, 4, 5, 10, 11); // 2-thick slab off it, stopping short of +x
    const SupportField f = computeSupport(setOccupancy(s), VoxelCoord{0, 0, 0},
                                          VoxelCoord{19, 9, 19}, 6);
    // The wall sits on the region's bottom face and vertical steps are free,
    // so the whole wall (x==0..1) is at distance 0 — the slab's budget is
    // spent purely on its own horizontal run, x=2 (d=1) .. x=7 (d=6).
    CHECK(f.isSupported(7, 4, 10));   // 6 lateral steps out from the wall
    CHECK(!f.isSupported(8, 4, 10));  // 7 — past the budget
    CHECK(!f.isSupported(8, 4, 11));  // the second layer buys nothing (no reset)
    CHECK(!f.isSupported(16, 4, 10)); // and it does not creep further out
}

VXC_TEST(collapse_disabled_when_nothing_changed) {
    // Same occupancy before and after: the predicate needs a support CHANGE,
    // so an edit that removes nothing structural collapses nothing.
    CellSet s;
    addBox(s, 0, 19, 0, 19, 0, 5);
    addBox(s, 9, 10, 9, 10, 6, 12);
    const CollapseAnalysis a = findCollapsingCells(setOccupancy(s), setOccupancy(s),
                                                   VoxelCoord{0, 0, 0}, VoxelCoord{19, 19, 19});
    CHECK_EQ(a.collapsingCells.size(), size_t(0));
}

// =====================================================================
// 2. The money case
// =====================================================================

VXC_TEST(collapse_pillar_blown_out_drops_the_roof) {
    // Free-standing pillar + roof, well inside the region (so nothing is
    // boundary-anchored). Blow the pillar out; the whole roof must collapse.
    CellSet before;
    addBox(before, 0, 29, 0, 29, 0, 1);   // ground
    addBox(before, 14, 15, 14, 15, 2, 9); // pillar
    addBox(before, 10, 19, 10, 19, 10, 10); // roof
    CellSet after = before;
    removeBox(after, 14, 15, 14, 15, 2, 9);

    const CollapseAnalysis a = findCollapsingCells(setOccupancy(before), setOccupancy(after),
                                                   VoxelCoord{0, 0, 0}, VoxelCoord{29, 29, 19});
    // Every one of the 10x10 roof cells, and nothing else (the ground is
    // boundary+bottom anchored, the pillar is gone). Note the 12 outer corner
    // cells are further than the 6-cell budget from the pillar and so were
    // already unsupported BEFORE the edit — they are not seeds, and come down
    // only because of the closure pass (header §3b). Without closure this
    // would be 88 and a fringe of corners would be left floating.
    CHECK_EQ(a.collapsingCells.size(), size_t(100));
    CHECK(hasCell(a, 10, 10, 10));
    CHECK(hasCell(a, 19, 19, 10));
    CHECK(!hasCell(a, 5, 5, 0)); // ground untouched
}

VXC_TEST(collapse_unsupported_span_falls_while_still_touching) {
    // THE case pure connectivity cannot express, and the reason this is a
    // support model rather than a connectivity model. A long roof runs from a
    // ground-anchored wall out to a far pillar. Blow out ONLY the far pillar:
    // the roof is still 6-connected to the ground through the wall, so
    // findDisconnectedIslands would say "attached, nothing falls" — but the
    // outer end of the span is now 10+ cells from anything carrying load and
    // must come down, while the part within the cantilever budget of the wall
    // stays up.
    CellSet before;
    addBox(before, 0, 29, 0, 29, 0, 1);    // ground
    addBox(before, 2, 3, 10, 13, 2, 9);    // near wall
    addBox(before, 24, 25, 10, 13, 2, 9);  // far pillar
    addBox(before, 2, 25, 10, 13, 10, 10); // roof spanning the two
    CellSet after = before;
    removeBox(after, 24, 25, 10, 13, 2, 9); // far pillar destroyed

    const CollapseAnalysis a = findCollapsingCells(setOccupancy(before), setOccupancy(after),
                                                   VoxelCoord{0, 0, 0}, VoxelCoord{29, 29, 19});
    CHECK(a.collapsingCells.size() > 0);
    // Within 6 lateral cells of the wall top (x=3) the roof stays up ...
    CHECK(!hasCell(a, 4, 11, 10));
    CHECK(!hasCell(a, 9, 11, 10));
    // ... beyond it, the unsupported span falls, all the way to the far end.
    CHECK(hasCell(a, 10, 11, 10));
    CHECK(hasCell(a, 25, 11, 10));
    // The wall itself and the ground never move.
    CHECK(!hasCell(a, 2, 11, 5));
    CHECK(!hasCell(a, 15, 15, 0));

    // And to make the contrast explicit: plain 6-connectivity over the same
    // post-edit geometry finds the roof attached, i.e. ZERO islands. The
    // support model is doing work connectivity structurally cannot.
    const auto solidAfter = [&after](int64_t x, int64_t y, int64_t z) {
        return after.find(std::make_tuple(x, y, z)) != after.end();
    };
    const IslandAnalysis ia = findDisconnectedIslands(
        solidAfter, VoxelCoord{0, 0, 0}, VoxelCoord{29, 29, 19},
        [](int64_t, int64_t, int64_t z) { return z == 0; });
    CHECK_EQ(ia.islandComponentIndices.size(), size_t(0));
}

VXC_TEST(collapse_truncation_is_a_deterministic_prefix) {
    CellSet before;
    addBox(before, 0, 39, 0, 39, 0, 1);
    addBox(before, 19, 20, 19, 20, 2, 9);
    addBox(before, 5, 34, 5, 34, 10, 10); // a 30x30 = 900-cell roof
    CellSet after = before;
    removeBox(after, 19, 20, 19, 20, 2, 9);

    CollapseParams p;
    p.maxCollapsingCells = 100;
    const CollapseAnalysis a = findCollapsingCells(setOccupancy(before), setOccupancy(after),
                                                   VoxelCoord{0, 0, 0}, VoxelCoord{39, 39, 19}, p);
    CHECK(a.bTruncated);
    CHECK_EQ(a.collapsingCells.size(), size_t(100));
    // A prefix in VoxelCoordLess order, so it is stable across runs/machines.
    for (size_t i = 1; i < a.collapsingCells.size(); ++i) {
        CHECK(VoxelCoordLess{}(a.collapsingCells[i - 1], a.collapsingCells[i]));
    }
}

// =====================================================================
// 3. Determinism
// =====================================================================

VXC_TEST(collapse_support_matches_order_independent_fixpoint) {
    // computeSupport's bucket queue vs. a Bellman-Ford relax-everything-until-
    // stable reference. The fixpoint is order-independent by construction, so
    // matching it proves no traversal order leaks into dist.
    CellSet s;
    addBox(s, 0, 24, 0, 24, 0, 2);
    addBox(s, 5, 6, 5, 6, 3, 12);
    addBox(s, 18, 19, 18, 19, 3, 8);
    addBox(s, 3, 22, 3, 22, 13, 13);
    addBox(s, 10, 11, 10, 20, 14, 18);
    removeBox(s, 12, 14, 12, 14, 13, 13);

    for (int32_t budget : {0, 1, 3, 6, 12}) {
        const SupportField f =
            computeSupport(setOccupancy(s), VoxelCoord{0, 0, 0}, VoxelCoord{24, 24, 24}, budget);
        const std::vector<int32_t> ref = bruteForceDist(f, budget);
        CHECK_EQ(f.dist.size(), ref.size());
        for (size_t i = 0; i < ref.size(); ++i) {
            CHECK_EQ(f.dist[i], ref[i]);
        }
    }
}

VXC_TEST(collapse_split_components_is_input_order_independent) {
    // Two disjoint blobs plus a lone voxel, fed in 16 different shuffles.
    // Component order, per-component voxel order, and the digest must all be
    // identical every time — the ordering guarantee downstream debris
    // promotion relies on.
    std::vector<VoxelCoord> voxels;
    for (int64_t z = 0; z < 3; ++z)
        for (int64_t y = 0; y < 3; ++y)
            for (int64_t x = 0; x < 3; ++x) voxels.push_back(VoxelCoord{x, y, z});
    for (int64_t z = 0; z < 2; ++z)
        for (int64_t y = 0; y < 2; ++y)
            for (int64_t x = 10; x < 12; ++x) voxels.push_back(VoxelCoord{x, y, z});
    voxels.push_back(VoxelCoord{20, 20, 20});

    Digest d0;
    digestComponents(splitIntoComponents(voxels), d0);
    const uint64_t golden = d0.h;

    std::mt19937_64 rng(20260721);
    for (int trial = 0; trial < 16; ++trial) {
        std::vector<VoxelCoord> shuffled = voxels;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        const std::vector<Component> comps = splitIntoComponents(shuffled);
        CHECK_EQ(comps.size(), size_t(3));
        CHECK_EQ(comps[0].size(), size_t(27)); // ordered by min coord: the 3^3 blob at the origin
        CHECK_EQ(comps[1].size(), size_t(8));
        CHECK_EQ(comps[2].size(), size_t(1));
        // Within-component order is sorted, never BFS order.
        for (const Component& c : comps)
            for (size_t i = 1; i < c.voxels.size(); ++i)
                CHECK(VoxelCoordLess{}(c.voxels[i - 1], c.voxels[i]));
        Digest d;
        digestComponents(comps, d);
        CHECK_EQ(d.h, golden);
    }

    // Duplicates in the input are collapsed, not double-counted.
    std::vector<VoxelCoord> dup = voxels;
    dup.insert(dup.end(), voxels.begin(), voxels.end());
    Digest dd;
    digestComponents(splitIntoComponents(dup), dd);
    CHECK_EQ(dd.h, golden);
}

VXC_TEST(collapse_golden_digest_pinned) {
    // Golden pin for the whole decision (cells + counts + truncation flag) on
    // a fixed pillar-and-roof scenario. Any change to the support model, the
    // anchor set, the emit order, or the digest layout moves this value — it
    // must only ever be updated deliberately, with the reason recorded in
    // docs/status.md.
    CellSet before;
    addBox(before, 0, 29, 0, 29, 0, 1);
    addBox(before, 2, 3, 10, 13, 2, 9);
    addBox(before, 24, 25, 10, 13, 2, 9);
    addBox(before, 2, 25, 10, 13, 10, 10);
    CellSet after = before;
    removeBox(after, 24, 25, 10, 13, 2, 9);

    const CollapseAnalysis a = findCollapsingCells(setOccupancy(before), setOccupancy(after),
                                                   VoxelCoord{0, 0, 0}, VoxelCoord{29, 29, 19});
    Digest d;
    a.digest(d);
    CHECK_EQ(d.h, 0x64CE88EFEC89BF80ULL);

    // Re-running the identical scenario is byte-identical (no hidden state,
    // no address/iteration-order dependence in the unordered_set path).
    const CollapseAnalysis b = findCollapsingCells(setOccupancy(before), setOccupancy(after),
                                                   VoxelCoord{0, 0, 0}, VoxelCoord{29, 29, 19});
    Digest d2;
    b.digest(d2);
    CHECK_EQ(d2.h, d.h);
}

VXC_TEST(collapse_coarse_merge_is_conservative_not_permissive) {
    // Documents the ONE approximation coarsening makes and proves its
    // direction. Two cells that only touch diagonally are NOT coarse-adjacent
    // (6-connected cells only), and a cell counts as occupied on a single
    // solid voxel — so coarsening merges things that are really separate and
    // never separates things that are really joined. Concretely: a slab whose
    // only link to the ground is a diagonal cell touch gets NO support and
    // collapses, which is the safe direction (it really is not load-bearing).
    CellSet before;
    addBox(before, 0, 19, 0, 19, 0, 1);
    addBox(before, 9, 9, 9, 9, 2, 5);   // thin column
    addBox(before, 6, 13, 6, 13, 6, 6); // slab resting on it
    CellSet after = before;
    removeBox(after, 9, 9, 9, 9, 2, 5);

    const SupportField bf =
        computeSupport(setOccupancy(before), VoxelCoord{0, 0, 0}, VoxelCoord{19, 19, 19}, 6);
    CHECK(bf.isSupported(9, 9, 6));   // directly carried by the column before
    CHECK(bf.isSupported(12, 12, 6)); // 6 lateral cells out — the budget edge
    CHECK(!bf.isSupported(13, 13, 6)); // 8 out — already past it pre-edit

    const CollapseAnalysis a = findCollapsingCells(setOccupancy(before), setOccupancy(after),
                                                   VoxelCoord{0, 0, 0}, VoxelCoord{19, 19, 19});
    CHECK_EQ(a.collapsingCells.size(), size_t(64)); // the whole 8x8 slab
}
