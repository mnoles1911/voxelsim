// Asset scatter and, mostly, the BOUND.
//
// The scatter tests here are ordinary determinism checks. The bound tests are
// not ordinary, and they are the reason this file is long: the bound's failure
// mode is a hole in the world that nothing logs, and the specific way a gate
// like this fails in this codebase is already on record --
// voxelcore/tilestreaming.h:173-188, on the fine-tier residency gate:
//
//   "it was only ever exercised with the tile PRESENT. Every check passed,
//    missingTileQueries stayed 0, and the absent branch -- the entire reason
//    the gate exists -- was never run."
//
// So the tests below deliberately drive the hard direction: rects with no
// assets near them, rects whose only asset is anchored OUTSIDE them and reaches
// in, a surface bound that declines, and an adversarial sweep that places every
// instance for real and checks that not one voxel escapes the bound.

#include "voxelcore/assetplacement.h"

#include <vector>

#include "vxctest.h"

using namespace vxc;

namespace {

// A four-layer table shaped like the real library: a sparse emergent class
// reaching 28 m, a canopy class, a shrub class, and dense ground cover.
// Numbers are illustrative -- the tests assert relationships, not values.
const AssetLayer kLayers[kAssetLayerCount] = {
    // cellMm, maxHeightMm, maxDepthMm, maxRadiusMm, densityPerMille, seedCount
    {64000, 28000, 3000, 9000, 120, 64},  // emergent
    {24000, 12000, 2000, 5000, 400, 64},  // canopy
    {6000, 3000, 500, 1500, 500, 32},     // shrub
    {800, 500, 100, 300, 700, 16},        // ground cover
};

AssetVoxelRect chunkRect(int64_t chunkX, int64_t chunkY) {
    // One level-0 render chunk footprint: 32 voxels a side.
    return AssetVoxelRect{chunkX * 32, chunkY * 32, chunkX * 32 + 31, chunkY * 32 + 31};
}

constexpr uint64_t kSeed = 0x5eed1234abcdef01ull;

} // namespace

VXC_TEST(assetplacement_is_deterministic_and_position_dependent) {
    AssetSite a, b;
    const bool ha = assetSiteInCell(kSeed, kLayers[1], 1, 17, -42, a);
    const bool hb = assetSiteInCell(kSeed, kLayers[1], 1, 17, -42, b);
    CHECK_EQ(int(ha), int(hb));
    if (ha) CHECK(a == b);

    // A different seed must be able to disagree somewhere in a decent sample --
    // a scatter that ignored its seed would pass every other test in this file.
    int differing = 0;
    for (int64_t cx = 0; cx < 64; ++cx) {
        AssetSite s1, s2;
        const bool h1 = assetSiteInCell(kSeed, kLayers[1], 1, cx, 7, s1);
        const bool h2 = assetSiteInCell(kSeed ^ 0x9e3779b97f4a7c15ull, kLayers[1], 1, cx, 7, s2);
        if (h1 != h2 || (h1 && !(s1 == s2))) ++differing;
    }
    CHECK(differing > 16);
}

VXC_TEST(assetplacement_density_bounds_are_honoured_at_both_ends) {
    AssetLayer none = kLayers[1];
    none.densityPerMille = 0;
    AssetLayer all = kLayers[1];
    all.densityPerMille = 1000;

    int nNone = 0, nAll = 0;
    for (int64_t cy = 0; cy < 32; ++cy)
        for (int64_t cx = 0; cx < 32; ++cx) {
            AssetSite s;
            if (assetSiteInCell(kSeed, none, 1, cx, cy, s)) ++nNone;
            if (assetSiteInCell(kSeed, all, 1, cx, cy, s)) ++nAll;
        }
    CHECK_EQ(nNone, 0);
    CHECK_EQ(nAll, 32 * 32);

    // And the middle is roughly the requested rate. Loose bounds: this is a
    // check that the per-mille arithmetic is not off by an order of magnitude
    // or inverted, not a statistical test.
    AssetLayer half = kLayers[1];
    half.densityPerMille = 500;
    int nHalf = 0;
    for (int64_t cy = 0; cy < 64; ++cy)
        for (int64_t cx = 0; cx < 64; ++cx) {
            AssetSite s;
            if (assetSiteInCell(kSeed, half, 1, cx, cy, s)) ++nHalf;
        }
    CHECK(nHalf > 64 * 64 * 40 / 100);
    CHECK(nHalf < 64 * 64 * 60 / 100);
}

VXC_TEST(assetplacement_anchors_stay_inside_their_own_cell) {
    // The jitter must not carry an anchor out of its cell. If it could, two
    // cells could produce the same anchor, and the "one site per cell" property
    // that the bound's early-out relies on would stop being true.
    for (int64_t cy = -8; cy < 8; ++cy)
        for (int64_t cx = -8; cx < 8; ++cx) {
            AssetSite s;
            if (!assetSiteInCell(kSeed, kLayers[2], 2, cx, cy, s)) continue;
            const int64_t cell = int64_t(kLayers[2].cellMm);
            CHECK(s.anchorXMm >= cx * cell);
            CHECK(s.anchorXMm < (cx + 1) * cell);
            CHECK(s.anchorYMm >= cy * cell);
            CHECK(s.anchorYMm < (cy + 1) * cell);
            CHECK(s.yawQuarter < 4);
            CHECK(s.seedIndex < kLayers[2].seedCount);
        }
}

// ---------------------------------------------------------------------------
// THE BOUND
// ---------------------------------------------------------------------------

VXC_TEST(assetbound_finds_an_asset_anchored_outside_the_rect_that_reaches_in) {
    // THE SHAPE THAT LEAKS. A chunk footprint is 3.2 m; an emergent's canopy
    // reaches 9 m. So the overwhelmingly common case is that the tree standing
    // over a chunk is anchored in a DIFFERENT chunk -- and a bound that
    // enumerated only the cells overlapping the rect itself would report "no
    // assets here" for almost every chunk a canopy actually covers. That is a
    // hole in the world in the shape of every tree crown.
    //
    // Constructed rather than sampled: one layer, one guaranteed site, and a
    // rect placed deliberately outside that site's cell but inside its reach.
    // Written as a CONTRAST between two layers identical but for their reach,
    // rather than as "this rect finds a site". The first cut asserted the
    // latter and failed for a reason worth recording: with a 9 m reach on 32 m
    // cells, whether any neighbouring anchor lands within reach of a given
    // one-voxel rect is a coin toss on the jitter draw, ~28% here. The test was
    // wrong, not the code -- but a test whose pass depends on a hash draw is
    // worse than no test, because it fails once in five runs and gets rerun
    // until it passes.
    AssetLayer reaching{};
    reaching.cellMm = 32000;
    reaching.maxHeightMm = 20000;
    reaching.maxDepthMm = 0;
    reaching.maxRadiusMm = 64000; // 2 cells: reach across a boundary is certain
    reaching.densityPerMille = 1000;
    reaching.seedCount = 1;

    AssetLayer pointlike = reaching;
    pointlike.maxRadiusMm = 0; // reaches only its own voxel

    // A single-voxel rect, deliberately not at a cell origin.
    const AssetVoxelRect tiny{347, 211, 347, 211};
    const int64_t rectCellX = floorDiv(tiny.vx0 * int64_t(kVoxelSizeMm), int64_t(reaching.cellMm));
    const int64_t rectCellY = floorDiv(tiny.vy0 * int64_t(kVoxelSizeMm), int64_t(reaching.cellMm));

    // With reach, the rect must be covered, and -- the actual property -- by at
    // least one site anchored in a DIFFERENT cell than the rect sits in. That
    // is the case a bound enumerating only the rect's own cells would miss, and
    // it is the common case in the world: a chunk is 3.2 m and a canopy is 9 m
    // wide, so the tree standing over a chunk is usually anchored elsewhere.
    const std::vector<AssetSite> withReach = assetSitesForRect(kSeed, &reaching, 1, tiny);
    CHECK(!withReach.empty());
    int fromAnotherCell = 0;
    for (const AssetSite& site : withReach) {
        if (site.cellX != rectCellX || site.cellY != rectCellY) ++fromAnotherCell;
    }
    CHECK(fromAnotherCell > 0);
    CHECK_EQ(assetTopAboveSurfaceMm(kSeed, &reaching, 1, tiny), reaching.maxHeightMm);

    // Without reach, a site anchored in a neighbouring cell must NOT be
    // reported. The anchor would have to land in this exact voxel, which at
    // 32 m cells and 10 cm voxels it does not.
    CHECK(assetSitesForRect(kSeed, &pointlike, 1, tiny).empty());
    CHECK_EQ(assetTopAboveSurfaceMm(kSeed, &pointlike, 1, tiny), 0);

    // And with no sites at all the bound must be exactly 0, or every footprint
    // on the planet pays for vegetation that is not there.
    AssetLayer empty = reaching;
    empty.densityPerMille = 0;
    CHECK_EQ(assetTopAboveSurfaceMm(kSeed, &empty, 1, tiny), 0);
}

VXC_TEST(assetbound_is_zero_where_no_layer_can_reach) {
    // A layer whose sites exist but whose reach cannot cross to this rect must
    // contribute nothing. Built with a single sparse cell so the gap is real.
    AssetLayer sparse{};
    sparse.cellMm = 1000000;  // 1 km cells
    sparse.maxHeightMm = 28000;
    sparse.maxRadiusMm = 1000; // 1 m reach
    sparse.densityPerMille = 1000;
    sparse.seedCount = 1;

    AssetSite s;
    CHECK(assetSiteInCell(kSeed, sparse, 0, 0, 0, s));

    // A rect a long way from the anchor inside the same cell.
    const int64_t farVox = 500000 / int64_t(kVoxelSizeMm);
    AssetVoxelRect far{farVox, farVox, farVox + 31, farVox + 31};
    // Only true if the anchor's jitter did not land near this rect; the cell is
    // 1 km and the reach 1 m, so it cannot.
    const bool nearX = s.anchorXMm + 1000 >= far.vx0 * int64_t(kVoxelSizeMm) &&
                       s.anchorXMm - 1000 <= far.vx1 * int64_t(kVoxelSizeMm);
    const bool nearY = s.anchorYMm + 1000 >= far.vy0 * int64_t(kVoxelSizeMm) &&
                       s.anchorYMm - 1000 <= far.vy1 * int64_t(kVoxelSizeMm);
    if (!(nearX && nearY)) {
        CHECK_EQ(assetTopAboveSurfaceMm(kSeed, &sparse, 1, far), 0);
    }
}

VXC_TEST(assetbound_dominates_every_instance_it_covers) {
    // THE ADVERSARIAL SWEEP. For a spread of chunk footprints, enumerate every
    // instance the generation query returns, give each one the tallest asset
    // its layer permits standing on the highest ground within its reach, and
    // check that no voxel of any of them escapes the composed bound.
    //
    // The synthetic surface is deliberately steep and not axis-aligned: a flat
    // test surface would make the dilation step unnecessary and the test would
    // pass with the dilation deleted. Sloping it means an instance anchored
    // uphill of a rect genuinely stands above everything inside the rect, which
    // is exactly what the dilation is for.
    const auto surfaceMm = [](int64_t vx, int64_t vy) -> int64_t {
        return 100000 + vx * 37 + vy * 23; // mm; ~3.7 mm per 10 cm voxel of run
    };
    const auto surfaceUpperBound = [&](int64_t, int64_t, int64_t vx1, int64_t vy1) -> int64_t {
        // An exact maximum over the rect for this monotone surface -- a legal
        // (if unusually tight) upper bound, which makes the test strictly
        // harder than the real amplifier's conservative one.
        return surfaceMm(vx1, vy1);
    };

    int checked = 0;
    for (int64_t cy = -3; cy <= 3; ++cy) {
        for (int64_t cx = -3; cx <= 3; ++cx) {
            const AssetVoxelRect rect = chunkRect(cx, cy);
            const int64_t bound = assetAwareSurfaceUpperBoundMm(kSeed, kLayers, kAssetLayerCount,
                                                                rect, surfaceUpperBound);
            CHECK(bound != kSurfaceBoundDeclined);

            const std::vector<AssetSite> sites =
                assetSitesForRect(kSeed, kLayers, kAssetLayerCount, rect);
            for (const AssetSite& s : sites) {
                // Worst case for this instance: its base sits on the ground at
                // its own anchor, and it reaches its layer's full height.
                const int64_t anchorVx = floorDiv(s.anchorXMm, int64_t(kVoxelSizeMm));
                const int64_t anchorVy = floorDiv(s.anchorYMm, int64_t(kVoxelSizeMm));
                const int64_t topMm =
                    surfaceMm(anchorVx, anchorVy) + int64_t(kLayers[s.layer].maxHeightMm);
                CHECK(topMm <= bound);
                ++checked;
            }
        }
    }
    // The sweep must actually have found instances, or it proved nothing. This
    // is the vacuous-truth guard the fine-tier gate went without.
    CHECK(checked > 100);
}

VXC_TEST(assetbound_propagates_a_declined_terrain_bound) {
    // kSurfaceBoundDeclined is INT64_MAX. Adding an asset height to it wraps to
    // a large negative number, which every caller reads as "provably air" -- for
    // the entire sky, over a footprint the terrain bound just said it could not
    // reason about. Never claim air on no information.
    const auto declines = [](int64_t, int64_t, int64_t, int64_t) -> int64_t {
        return kSurfaceBoundDeclined;
    };
    const int64_t got = assetAwareSurfaceUpperBoundMm(kSeed, kLayers, kAssetLayerCount,
                                                      chunkRect(0, 0), declines);
    CHECK_EQ(got, kSurfaceBoundDeclined);
    CHECK(got > 0); // i.e. it did not wrap
}

VXC_TEST(assetbound_dilates_the_terrain_query_by_the_widest_reach) {
    // The dilation is invisible in the result, so assert it at the call: record
    // the rect the terrain bound is actually asked about.
    int64_t sawX0 = 0, sawY0 = 0, sawX1 = 0, sawY1 = 0;
    const auto record = [&](int64_t x0, int64_t y0, int64_t x1, int64_t y1) -> int64_t {
        sawX0 = x0; sawY0 = y0; sawX1 = x1; sawY1 = y1;
        return 0;
    };
    const AssetVoxelRect rect = chunkRect(0, 0);
    assetAwareSurfaceUpperBoundMm(kSeed, kLayers, kAssetLayerCount, rect, record);

    const int32_t reach = assetMaxReachMm(kLayers, kAssetLayerCount);
    CHECK_EQ(reach, 9000); // the emergent layer's radius, the widest present
    const int64_t reachVox = int64_t(reach) / int64_t(kVoxelSizeMm) + 1;
    CHECK_EQ(sawX0, rect.vx0 - reachVox);
    CHECK_EQ(sawY0, rect.vy0 - reachVox);
    CHECK_EQ(sawX1, rect.vx1 + reachVox);
    CHECK_EQ(sawY1, rect.vy1 + reachVox);
    // Rounded OUTWARD: the dilation in voxels must cover the reach in mm.
    CHECK(reachVox * int64_t(kVoxelSizeMm) >= int64_t(reach));
}

VXC_TEST(assetbound_cannot_be_raised_by_a_policy) {
    // THE VETO-ONLY RULE, stated as a test. The bound is a function of (seed,
    // layer table, rect) alone; a policy sees sites and may drop them. So for
    // ANY subset of the enumerated sites -- which is every policy that obeys
    // the rule -- the realised maximum is at most the bound.
    //
    // This is what lets biome placement rules change freely later without
    // anyone having to re-verify the bound, which matters because the bound's
    // failure is silent and the rules will change many times.
    const AssetVoxelRect rect = chunkRect(2, -5);
    const int32_t bound = assetTopAboveSurfaceMm(kSeed, kLayers, kAssetLayerCount, rect);
    const std::vector<AssetSite> all = assetSitesForRect(kSeed, kLayers, kAssetLayerCount, rect);
    CHECK(!all.empty());

    // Every one of the 2^n subsets is impractical; the property is monotone, so
    // checking that each individual site is within the bound is equivalent.
    for (const AssetSite& s : all) {
        CHECK(kLayers[s.layer].maxHeightMm <= bound);
    }
    // And the empty policy -- veto everything -- realises 0, which is also
    // within the bound. Conservative, which is the safe direction.
    CHECK(bound >= 0);
}

VXC_TEST(assetbound_degenerate_and_null_inputs_are_refused_quietly) {
    const AssetVoxelRect inverted{10, 10, 0, 0};
    CHECK_EQ(assetTopAboveSurfaceMm(kSeed, kLayers, kAssetLayerCount, inverted), 0);
    CHECK_EQ(assetTopAboveSurfaceMm(kSeed, nullptr, 4, chunkRect(0, 0)), 0);
    CHECK_EQ(assetTopAboveSurfaceMm(kSeed, kLayers, 0, chunkRect(0, 0)), 0);
    CHECK_EQ(assetMaxReachMm(nullptr, 4), 0);
    CHECK(assetSitesForRect(kSeed, kLayers, kAssetLayerCount, inverted).empty());

    AssetLayer bad = kLayers[0];
    bad.cellMm = 0; // would divide by zero
    AssetSite s;
    CHECK(!assetSiteInCell(kSeed, bad, 0, 0, 0, s));
    CHECK_EQ(assetTopAboveSurfaceMm(kSeed, &bad, 1, chunkRect(0, 0)), 0);
}

VXC_TEST(assetbound_layer_height_contract_is_checkable_at_bake_time) {
    // An asset baked taller than its layer's declared maximum puts a hole in
    // the world at its own crown, silently. This is the check that turns that
    // into a bake-time refusal.
    const AssetLayer& canopy = kLayers[1]; // 12 m
    CHECK(assetLayerAdmitsHeight(canopy, 120));  // 12.0 m at 10 cm
    CHECK(assetLayerAdmitsHeight(canopy, 60));
    CHECK(!assetLayerAdmitsHeight(canopy, 121)); // 12.1 m -- one voxel over
}

VXC_TEST(assetsites_are_ordered_and_free_of_duplicates) {
    // The generation query feeds a mesher; a duplicated site would draw the
    // same tree twice into the same voxels, which is invisible in the render
    // and doubles the cost.
    const std::vector<AssetSite> sites =
        assetSitesForRect(kSeed, kLayers, kAssetLayerCount, chunkRect(-1, 4));
    CHECK(!sites.empty());
    for (size_t i = 0; i < sites.size(); ++i)
        for (size_t j = i + 1; j < sites.size(); ++j)
            CHECK(!(sites[i].layer == sites[j].layer && sites[i].cellX == sites[j].cellX &&
                    sites[i].cellY == sites[j].cellY));

    // Deterministic order, so a chunk regenerated on a different thread or in a
    // different session produces the same voxels in the same sequence.
    const std::vector<AssetSite> again =
        assetSitesForRect(kSeed, kLayers, kAssetLayerCount, chunkRect(-1, 4));
    CHECK_EQ(int(sites.size()), int(again.size()));
    for (size_t i = 0; i < sites.size(); ++i) CHECK(sites[i] == again[i]);
}
