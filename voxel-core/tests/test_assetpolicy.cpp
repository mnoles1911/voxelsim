// The policy: which species stands at a site, and whether anything does.
//
// Three groups of tests, and the second and third are the reason the file is
// long.
//
//   1. Ordinary gate tests. Biome, elevation, slope, the weighted pick.
//   2. THE VETO-ONLY RULE, restated as a property: whatever the policy does,
//      the realised set is a SUBSET of the sites assetplacement.h's bound
//      already accounted for. If that ever stops being true the bound stops
//      being an upper bound, and its failure mode is a hole in the world that
//      nothing logs.
//   3. THE CLUSTERING MEASUREMENT. Clustering is exactly the feature that can
//      run, report success and change nothing -- this project's signature
//      failure, three times in the last week. So it is pinned with a statistic
//      in BOTH directions: clustered must measure clumped, unclustered must
//      not, and the population must survive.

#include "voxelcore/assetpolicy.h"

#include <cstdio>
#include <vector>

#include "vxctest.h"

using namespace vxc;

namespace {

constexpr uint64_t kSeed = 0x51ee7ca7d0d0faceull;

const AssetLayer kLayers[kAssetLayerCount] = {
    // cellMm, maxHeightMm, maxDepthMm, maxRadiusMm, densityPerMille, seedCount
    {64000, 28000, 3000, 9000, 120, 64},  // 0 emergent
    {24000, 12000, 2000, 5000, 400, 64},  // 1 canopy
    {6000, 3000, 500, 1500, 500, 32},     // 2 shrub
    {800, 500, 100, 300, 700, 16},        // 3 ground cover
};

// A species that grows anywhere, on `layer`, with weight `w` per mille in every
// biome. Everything else is left at the permissive defaults so a test that
// wants to exercise one gate can move one field.
AssetSpecies anywhere(uint8_t layer, uint16_t w, uint16_t bankId = 0) {
    AssetSpecies s;
    s.bankId = bankId;
    s.layer = layer;
    for (int b = 0; b < kBiomeCount; ++b) s.weightPerMille[b] = w;
    s.elevMinMm = -1'000'000;
    s.elevMaxMm = 9'000'000;
    s.slopeMinMmPerM = 0;
    s.slopeMaxMmPerM = 100000;
    s.heightMm = kLayers[layer].maxHeightMm;
    s.depthMm = 0;
    return s;
}

AssetColumnFacts goodGround(BiomeId b = TEMPERATE_FOREST, int32_t surfaceMm = 120'000,
                            int64_t slope = 100) {
    AssetColumnFacts c;
    c.known = true;
    c.anchorSolid = true;
    c.biome = b;
    c.surfaceMm = surfaceMm;
    c.slopeMmPerM = slope;
    c.distanceToWaterMm = 0;
    return c;
}

AssetVoxelRect chunkRect(int64_t cx, int64_t cy) {
    return AssetVoxelRect{cx * 32, cy * 32, cx * 32 + 31, cy * 32 + 31};
}

} // namespace

// ---------------------------------------------------------------------------
// 1. THE GATES
// ---------------------------------------------------------------------------

VXC_TEST(assetpolicy_refuses_a_site_it_cannot_evaluate) {
    // THE ANTI-FLOAT GUARD, at its two entry points. `known` false is "I could
    // not evaluate this column"; `anchorSolid` false is "the voxel this would
    // stand on is air". Both must refuse, and both DEFAULT to refusing, so a
    // caller who fills in half the struct places nothing rather than placing
    // everything on faith.
    const AssetSpecies sp = anywhere(1, 1000);
    AssetSite site{};
    site.layer = 1;
    site.anchorXMm = 1234;
    site.anchorYMm = 5678;
    AssetInstance inst;

    AssetColumnFacts unknown;         // default-constructed: known == false
    CHECK(!unknown.known);
    CHECK(!unknown.anchorSolid);
    CHECK(!assetResolveSite(kSeed, kLayers, kAssetLayerCount, &sp, 1, site, unknown, inst));

    // Known, but the anchor voxel is air -- a cave mouth or a sinkhole shaft.
    // The surface is perfectly real; the voxel under the tree is not.
    AssetColumnFacts hollow = goodGround();
    hollow.anchorSolid = false;
    CHECK(!assetResolveSite(kSeed, kLayers, kAssetLayerCount, &sp, 1, site, hollow, inst));

    // And with both, it places -- otherwise the two refusals above prove
    // nothing, because a function that always refuses passes them too.
    CHECK(assetResolveSite(kSeed, kLayers, kAssetLayerCount, &sp, 1, site, goodGround(), inst));
}

VXC_TEST(assetpolicy_anchor_uses_the_same_top_solid_rule_the_bricks_do) {
    // The anchor voxel must be the voxel GeneratedWorld::surfaceBrickRange
    // materialises for, not a second derivation of the same arithmetic. Two
    // derivations of one rule is how the cavern reach and the carrier stencil
    // came to disagree once already.
    const AssetSpecies sp = anywhere(1, 1000);
    AssetSite site{};
    site.layer = 1;
    AssetInstance inst;
    for (int32_t surfaceMm : {0, 49, 50, 51, 99, 100, 101, -1, -50, -51, 123'456}) {
        AssetColumnFacts c = goodGround(TEMPERATE_FOREST, surfaceMm);
        if (!assetResolveSite(kSeed, kLayers, kAssetLayerCount, &sp, 1, site, c, inst)) continue;
        CHECK_EQ(inst.anchorZMm, surfaceMm);
        CHECK_EQ(inst.anchorVz, topSolidVoxelZ(surfaceMm));
        // And the rule itself: the voxel's centre is at or below the surface,
        // and the next one up is not.
        CHECK(inst.anchorVz * int64_t(kVoxelSizeMm) + kVoxelSizeMm / 2 <= int64_t(surfaceMm));
        CHECK((inst.anchorVz + 1) * int64_t(kVoxelSizeMm) + kVoxelSizeMm / 2 > int64_t(surfaceMm));
    }
}

VXC_TEST(assetpolicy_biome_weight_zero_is_never_placed_anywhere) {
    // The most basic promise of the whole biome system: a brown trout at 0.0
    // desert does not appear in a desert. 828 specs carry these weights and
    // this is the first thing that reads them.
    AssetSpecies desertOnly = anywhere(1, 800);
    for (int b = 0; b < kBiomeCount; ++b) desertOnly.weightPerMille[b] = 0;
    desertOnly.weightPerMille[DESERT] = 1000;

    AssetInstance inst;
    int placedInDesert = 0, placedElsewhere = 0;
    for (int64_t cy = -12; cy <= 12; ++cy)
        for (int64_t cx = -12; cx <= 12; ++cx) {
            AssetSite s;
            if (!assetSiteInCell(kSeed, kLayers[1], 1, cx, cy, s)) continue;
            if (assetResolveSite(kSeed, kLayers, kAssetLayerCount, &desertOnly, 1, s,
                                 goodGround(DESERT), inst))
                ++placedInDesert;
            for (BiomeId b : {TEMPERATE_FOREST, TAIGA, OCEAN, TUNDRA_ALPINE, BARE_ROCK}) {
                if (assetResolveSite(kSeed, kLayers, kAssetLayerCount, &desertOnly, 1, s,
                                     goodGround(b), inst))
                    ++placedElsewhere;
            }
        }
    CHECK_EQ(placedElsewhere, 0);
    CHECK(placedInDesert > 20); // the vacuous-truth guard: it CAN place
}

VXC_TEST(assetpolicy_elevation_and_slope_are_bands_not_ceilings) {
    // A ceiling cannot express scree. Talus sits on ground steep enough to be
    // talus and BELOW the 70% grade at which classifyBiome returns BARE_ROCK
    // (biome.h:220), and a species with a slope FLOOR is the thing that says so.
    AssetSpecies scree = anywhere(2, 1000);
    scree.slopeMinMmPerM = 400; // 40% grade: steeper than a hillside
    scree.slopeMaxMmPerM = 680; // ...and just under the BARE_ROCK gate at 700
    scree.elevMinMm = 200'000;
    scree.elevMaxMm = 1'800'000;

    AssetSite site{};
    site.layer = 2;
    AssetInstance inst;

    // Too flat, too steep, too low, too high: all refused.
    CHECK(!assetResolveSite(kSeed, kLayers, kAssetLayerCount, &scree, 1, site,
                            goodGround(TUNDRA_ALPINE, 900'000, 100), inst));
    CHECK(!assetResolveSite(kSeed, kLayers, kAssetLayerCount, &scree, 1, site,
                            goodGround(TUNDRA_ALPINE, 900'000, kBiomeCliffSlopeMmPerM + 50), inst));
    CHECK(!assetResolveSite(kSeed, kLayers, kAssetLayerCount, &scree, 1, site,
                            goodGround(TUNDRA_ALPINE, 100'000, 500), inst));
    CHECK(!assetResolveSite(kSeed, kLayers, kAssetLayerCount, &scree, 1, site,
                            goodGround(TUNDRA_ALPINE, 2'400'000, 500), inst));
    // In the band: placed.
    CHECK(assetResolveSite(kSeed, kLayers, kAssetLayerCount, &scree, 1, site,
                           goodGround(TUNDRA_ALPINE, 900'000, 500), inst));

    // And the boundaries are inclusive at both ends, because a species authored
    // at exactly the cliff gate should reach it.
    CHECK(assetSpeciesTolerates(scree, goodGround(TUNDRA_ALPINE, 200'000, 400)));
    CHECK(assetSpeciesTolerates(scree, goodGround(TUNDRA_ALPINE, 1'800'000, 680)));
}

VXC_TEST(assetpolicy_a_species_that_needs_water_is_refused_when_nobody_knows_where_it_is) {
    // FAILS CLOSED. Nothing in voxel-core answers "how far to the nearest
    // watercourse" yet, so the fact arrives as kAssetNoWaterDistanceMm. A
    // riverbank willow placed on the strength of not knowing where the river is
    // is exactly the derived-not-verified failure this whole design is about.
    AssetSpecies willow = anywhere(1, 1000);
    willow.waterMaxMm = 60'000; // within 60 m of water

    AssetColumnFacts noIdea = goodGround();
    noIdea.distanceToWaterMm = kAssetNoWaterDistanceMm;
    CHECK(!assetSpeciesTolerates(willow, noIdea));

    AssetColumnFacts nearRiver = goodGround();
    nearRiver.distanceToWaterMm = 12'000;
    CHECK(assetSpeciesTolerates(willow, nearRiver));

    AssetColumnFacts farFromRiver = goodGround();
    farFromRiver.distanceToWaterMm = 400'000;
    CHECK(!assetSpeciesTolerates(willow, farFromRiver));

    // A species that does not care is unaffected by the unknown -- otherwise
    // failing closed would delete 800 of the 828 specs.
    AssetSpecies anywhereElse = anywhere(1, 1000);
    CHECK_EQ(anywhereElse.waterMaxMm, 0);
    CHECK(assetSpeciesTolerates(anywhereElse, noIdea));
}

VXC_TEST(assetpolicy_summed_weight_thins_a_poor_biome_without_a_density_table) {
    // The mechanism that makes a desert sparse. Where the only eligible species
    // has a low weight, most sites carry nothing; where several saturate, most
    // sites carry something. A fixed layer density with a pick on top would have
    // put a saguaro at EVERY site, which is a forest of cacti.
    AssetSpecies sparse = anywhere(1, 60);   // one species, 6%
    AssetSpecies rich[3] = {anywhere(1, 500, 1), anywhere(1, 500, 2), anywhere(1, 500, 3)};

    int nSparse = 0, nRich = 0, nSites = 0;
    AssetInstance inst;
    for (int64_t cy = -20; cy <= 20; ++cy)
        for (int64_t cx = -20; cx <= 20; ++cx) {
            AssetSite s;
            if (!assetSiteInCell(kSeed, kLayers[1], 1, cx, cy, s)) continue;
            ++nSites;
            if (assetResolveSite(kSeed, kLayers, kAssetLayerCount, &sparse, 1, s, goodGround(),
                                 inst))
                ++nSparse;
            if (assetResolveSite(kSeed, kLayers, kAssetLayerCount, rich, 3, s, goodGround(), inst))
                ++nRich;
        }
    CHECK(nSites > 200);
    std::printf("    sites %d -> sparse biome %d (~6%% expected), rich biome %d (saturated)\n",
                nSites, nSparse, nRich);
    // 6% of sites, loosely: this is an order-of-magnitude check that the
    // per-mille arithmetic is not inverted, not a statistical test.
    CHECK(nSparse * 100 < nSites * 15);
    CHECK(nRich * 100 > nSites * 90); // 3 x 500 per mille caps at certainty
}

VXC_TEST(assetpolicy_pick_is_weighted_and_every_species_can_win) {
    // A weighted walk that always returns the first eligible entry is a pick
    // that compiles, runs, reports success and puts one species in the world.
    AssetSpecies tab[3] = {anywhere(1, 100, 11), anywhere(1, 300, 22), anywhere(1, 600, 33)};
    int hits[3] = {0, 0, 0};
    AssetInstance inst;
    for (int64_t cy = -30; cy <= 30; ++cy)
        for (int64_t cx = -30; cx <= 30; ++cx) {
            AssetSite s;
            if (!assetSiteInCell(kSeed, kLayers[1], 1, cx, cy, s)) continue;
            if (!assetResolveSite(kSeed, kLayers, kAssetLayerCount, tab, 3, s, goodGround(), inst))
                continue;
            CHECK(inst.speciesIndex < 3);
            CHECK_EQ(int(inst.bankId), int(tab[inst.speciesIndex].bankId));
            ++hits[inst.speciesIndex];
        }
    const int total = hits[0] + hits[1] + hits[2];
    CHECK(total > 300);
    std::printf("    pick shares: %d / %d / %d of %d (authored 100/300/600 per mille)\n", hits[0],
                hits[1], hits[2], total);
    // Every species wins sometimes, and the order is right. Loose bounds: this
    // catches "always the first" and "ignores the weights", not a chi-square.
    CHECK(hits[0] > 0);
    CHECK(hits[1] > hits[0]);
    CHECK(hits[2] > hits[1]);
}

VXC_TEST(assetpolicy_is_deterministic) {
    // Placement is worldgen. Same seed, same answer, twice.
    AssetSpecies tab[2] = {anywhere(1, 400, 7), anywhere(1, 400, 8)};
    std::vector<AssetInstance> a, b;
    for (int64_t cx = -10; cx <= 10; ++cx) {
        AssetSite s;
        if (!assetSiteInCell(kSeed, kLayers[1], 1, cx, 3, s)) continue;
        AssetInstance i1, i2;
        const bool r1 =
            assetResolveSite(kSeed, kLayers, kAssetLayerCount, tab, 2, s, goodGround(), i1);
        const bool r2 =
            assetResolveSite(kSeed, kLayers, kAssetLayerCount, tab, 2, s, goodGround(), i2);
        CHECK_EQ(int(r1), int(r2));
        if (r1) {
            CHECK(i1 == i2);
            a.push_back(i1);
        }
    }
    CHECK(!a.empty());
}

// ---------------------------------------------------------------------------
// 2. THE VETO-ONLY RULE
// ---------------------------------------------------------------------------

VXC_TEST(assetpolicy_realises_a_subset_of_what_the_bound_accounted_for) {
    // THE CONTRACT, as a property rather than a comment. For every chunk in a
    // sweep, every instance the policy produces must correspond to a site the
    // bound already saw, on the site's own layer, and its height must be within
    // that layer's declared maximum. If this fails, assetTopAboveSurfaceMm has
    // stopped being an upper bound and the failure mode is a hole in the world.
    AssetSpecies tab[4];
    for (int i = 0; i < 4; ++i) tab[i] = anywhere(uint8_t(i), 900, uint16_t(i));

    int checked = 0;
    for (int64_t cy = -4; cy <= 4; ++cy)
        for (int64_t cx = -4; cx <= 4; ++cx) {
            const AssetVoxelRect rect = chunkRect(cx, cy);
            const int32_t bound = assetTopAboveSurfaceMm(kSeed, kLayers, kAssetLayerCount, rect);
            const std::vector<AssetSite> sites =
                assetSitesForRect(kSeed, kLayers, kAssetLayerCount, rect);
            for (const AssetSite& s : sites) {
                AssetInstance inst;
                if (!assetResolveSite(kSeed, kLayers, kAssetLayerCount, tab, 4, s, goodGround(),
                                      inst))
                    continue;
                // The instance stayed on its site's own layer...
                CHECK_EQ(int(inst.layer), s.layer);
                CHECK_EQ(inst.anchorXMm, s.anchorXMm);
                CHECK_EQ(inst.anchorYMm, s.anchorYMm);
                // ...and nothing it can be is taller than the bound allowed.
                CHECK(tab[inst.speciesIndex].heightMm <= kLayers[inst.layer].maxHeightMm);
                CHECK(kLayers[inst.layer].maxHeightMm <= bound);
                ++checked;
            }
        }
    // The vacuous-truth guard. A policy that placed nothing would pass every
    // line above.
    CHECK(checked > 100);
}

VXC_TEST(assetpolicy_species_filing_is_checked_at_load_not_discovered_in_a_screenshot) {
    AssetSpecies s = anywhere(1, 500);
    CHECK_EQ(int(assetSpeciesFits(s, kLayers, kAssetLayerCount, 24000)),
             int(AssetTableError::kOk));

    // Taller than its layer: a hole in the world at its own crown, silently.
    // The failure LOOKS like a generator bug -- tall trees with flat tops --
    // which is why it has to be refused by the tool that files it.
    AssetSpecies tall = s;
    tall.heightMm = kLayers[1].maxHeightMm + 1;
    CHECK_EQ(int(assetSpeciesFits(tall, kLayers, kAssetLayerCount, 24000)),
             int(AssetTableError::kTooTall));

    AssetSpecies deep = s;
    deep.depthMm = kLayers[1].maxDepthMm + 1;
    CHECK_EQ(int(assetSpeciesFits(deep, kLayers, kAssetLayerCount, 24000)),
             int(AssetTableError::kTooDeep));

    // A 5 cm asset filed on a terrain layer would be stamped into the world
    // grid at twice its intended size, which is not an error anywhere else.
    AssetSpecies wrongLattice = s;
    wrongLattice.voxelSizeMm = 50;
    CHECK_EQ(int(assetSpeciesFits(wrongLattice, kLayers, kAssetLayerCount, 24000)),
             int(AssetTableError::kWrongLattice));

    // Spacing finer than the lattice can express: the species would be placed
    // further apart than it asked for, quietly.
    CHECK_EQ(int(assetSpeciesFits(s, kLayers, kAssetLayerCount, 1000)),
             int(AssetTableError::kSpacingTooTight));

    AssetSpecies homeless = s;
    for (int b = 0; b < kBiomeCount; ++b) homeless.weightPerMille[b] = 0;
    CHECK_EQ(int(assetSpeciesFits(homeless, kLayers, kAssetLayerCount, 24000)),
             int(AssetTableError::kNoBiome));

    AssetSpecies badLayer = s;
    badLayer.layer = 9;
    CHECK_EQ(int(assetSpeciesFits(badLayer, kLayers, kAssetLayerCount, 24000)),
             int(AssetTableError::kBadLayer));

    // Every reason says something, because "the tree did not appear" is not a
    // diagnosis.
    for (int e = 0; e <= int(AssetTableError::kNoBiome); ++e)
        CHECK(assetTableErrorText(AssetTableError(e))[0] != '\0');
}

// ---------------------------------------------------------------------------
// 3. THE CLUSTERING MEASUREMENT
// ---------------------------------------------------------------------------

namespace {

// Index of dispersion over quadrats: variance / mean of the per-quadrat counts.
// 1.0 is Poisson, above is clumped, below is regular. Returned x1000 because
// voxel-core bans floating point under include/ and src/ and the tests keep the
// same discipline so a statistic cannot mean one thing here and another there.
//
// WHY THIS STATISTIC AND NOT MEAN NEAREST-NEIGHBOUR DISTANCE. A previous
// measurement on this project averaged a 2.6x difference away by choosing a
// statistic that did not capture the claim (see the terracing plateau-area
// lesson). Clumping IS variance in counts per area -- that is what "groves with
// open ground between" means -- so the statistic is the definition rather than
// a proxy for it.
int64_t dispersionX1000(const std::vector<int>& counts) {
    if (counts.size() < 2) return 0;
    int64_t sum = 0;
    for (int c : counts) sum += c;
    if (sum == 0) return 0;
    const int64_t n = int64_t(counts.size());
    // Variance x n^2 and mean x n, so the ratio is exact integer arithmetic
    // with no intermediate rounding: var/mean = (n*sumsq - sum^2) / (n * sum).
    int64_t sumsq = 0;
    for (int c : counts) sumsq += int64_t(c) * int64_t(c);
    return (n * sumsq - sum * sum) * 1000 / (n * sum);
}

// Count sites per quadrat over a square of layer cells, applying `keep`.
template <typename KeepFn>
void quadratCounts(const AssetLayer& layer, int layerIndex, int cellsPerSide, int quadratCells,
                   const KeepFn& keep, std::vector<int>& counts, int& total) {
    counts.clear();
    total = 0;
    const int q = cellsPerSide / quadratCells;
    counts.assign(size_t(q) * size_t(q), 0);
    for (int64_t cy = 0; cy < cellsPerSide; ++cy)
        for (int64_t cx = 0; cx < cellsPerSide; ++cx) {
            AssetSite s;
            if (!assetSiteInCell(kSeed, layer, layerIndex, cx, cy, s)) continue;
            if (!keep(s)) continue;
            const size_t qi = size_t(cy / quadratCells) * size_t(q) + size_t(cx / quadratCells);
            if (qi < counts.size()) ++counts[qi];
            ++total;
        }
}

} // namespace

VXC_TEST(assetcluster_measures_as_clumped_and_the_bare_scatter_measures_as_regular) {
    // THE MEASUREMENT THAT JUSTIFIES THE MECHANISM, and it corrects the usual
    // argument for it.
    //
    // Clustering is normally motivated as "a uniform scatter is Poisson and
    // real stands are clumped". That is wrong here, in an interesting
    // direction: assetSiteInCell is one site per lattice cell with a jitter
    // INSIDE the cell -- a jittered grid, which is MORE REGULAR than Poisson,
    // not less. A forest scattered that way does not read as trees placed at
    // random; it reads as an orchard, and more strongly than a Poisson scatter
    // would.
    const AssetLayer& L = kLayers[1];
    const int side = 160, quad = 8;

    std::vector<int> bare, clumped;
    int nBare = 0, nClumped = 0;
    quadratCounts(L, 1, side, quad, [](const AssetSite&) { return true; }, bare, nBare);

    // Driven through assetClusterKeeps -- the function assetResolveSite itself
    // calls -- rather than through a copy of its arithmetic. A statistic
    // measured on a reimplementation of a mechanism proves the
    // reimplementation works.
    //
    // 500/1000 is the shape with HEADROOM: the gain can raise this species'
    // chance as well as lower it. See assetClusterKeeps for what a ceiling of
    // certainty does instead, and what it measured.
    const int64_t keepNum = 500, keepDen = 1000;
    quadratCounts(
        L, 1, side, quad,
        [&](const AssetSite& s) {
            return assetClusterKeeps(kSeed, L, 1, 0, kAssetQ10One, s.anchorXMm, s.anchorYMm,
                                     s.cellX, s.cellY, keepNum, keepDen);
        },
        clumped, nClumped);
    // The unclustered control AT THE SAME KEEP PROBABILITY, so the dispersion
    // comparison is between two patterns of equal density rather than between a
    // dense pattern and a sparse one -- which would move the statistic on its
    // own and prove nothing about clustering.
    std::vector<int> thinned;
    int nThinned = 0;
    quadratCounts(
        L, 1, side, quad,
        [&](const AssetSite& s) {
            return assetClusterKeeps(kSeed, L, 1, 0, 0, s.anchorXMm, s.anchorYMm, s.cellX, s.cellY,
                                     keepNum, keepDen);
        },
        thinned, nThinned);

    const int64_t dBare = dispersionX1000(bare);
    const int64_t dThin = dispersionX1000(thinned);
    const int64_t dClumped = dispersionX1000(clumped);
    int maxThin = 0, maxClumped = 0;
    for (int c : thinned) if (c > maxThin) maxThin = c;
    for (int c : clumped) if (c > maxClumped) maxClumped = c;

    std::printf("    index of dispersion (1000 == Poisson): full lattice %lld, "
                "unclustered control %lld, clustered %lld\n",
                static_cast<long long>(dBare), static_cast<long long>(dThin),
                static_cast<long long>(dClumped));
    std::printf("    sites: full %d, control %d, clustered %d; busiest quadrat %d vs %d\n", nBare,
                nThinned, nClumped, maxThin, maxClumped);

    CHECK(nBare > 2000);   // the sweep found something to measure
    CHECK(nThinned > 800); // and so did the control

    // THE FINDING, AND IT IS WHY THIS WAS MEASURED BEFORE ANY CODE WAS WRITTEN.
    // The bare scatter is UNDER-dispersed -- below Poisson, i.e. MORE regular
    // than random. It is a jittered grid, not a Poisson process, so the usual
    // motivation for clustering ("random looks clumpy, a uniform scatter looks
    // too even") has the sign backwards: a forest scattered this way reads as
    // an ORCHARD, and more strongly than a random scatter would.
    CHECK(dBare < 1000);
    CHECK(dThin < 1200); // the density-matched control is still not clumped

    // And clustering moves it decisively the other way, against a control of
    // the SAME population. A margin, not a hairline: this is the assertion that
    // catches a cluster field that silently does nothing.
    CHECK(dClumped > 1500);
    CHECK(dClumped > dThin * 2);
    // Groves: the busiest patch of a clustered pattern holds more than the
    // busiest patch of an evenly-thinned one of equal population. That is the
    // "stands, with open ground between" half, and a mere reshuffle cannot move
    // it.
    CHECK(maxClumped > maxThin);
}

VXC_TEST(assetcluster_preserves_density_within_a_few_percent) {
    // THE SILENT-NO-OP GUARD ON THE OTHER SIDE. A cluster field that also
    // changed the population would ship a species at a fraction of its authored
    // abundance and nothing would report it.
    //
    // This is why the field modulates the THRESHOLD rather than being
    // thresholded itself: valueNoise2Fade's output is bell-shaped, so
    // thresholding it at "the 40% quantile of a uniform" delivers something
    // quite different from 40% of the sites. Modulating gives E[gain] == 1024
    // whatever the distribution is.
    const AssetLayer& L = kLayers[1];
    const int side = 640; // ~15 km: ~53 forest-octave cells, see the note above

    // The baseline is the UNCLUSTERED population at the same keep probability,
    // not the full lattice: the question this test asks is whether turning
    // clustering ON moves the population, not whether thinning does.
    //
    // TWO OCCUPANCIES, AND THE CONTRAST BETWEEN THEM IS THE POINT. A veto-based
    // cluster field can only preserve density where the gain has ROOM to go up
    // as well as down -- and how much room there is is exactly the species'
    // headroom below certainty. So the residual loss is not a mystery to be
    // tolerated, it is a characterised property:
    //
    //   * a sparse species (125 per mille, headroom to 8x) loses NOTHING, even
    //     at full strength -- the gain tops out at 4x and never clips;
    //   * a species on half the lattice (500 per mille, headroom to 2x) clips
    //     its upper tail against certainty and pays a few percent for the
    //     strongest clustering.
    //
    // Almost the whole library is the first case: the median authored
    // abundance is 0.5 and the median biome weight well below 1, so a species
    // occupying half of its lattice's sites is the exception. The bound is
    // asserted at both ends so that a change which made the SPARSE case lossy
    // would fail, rather than hiding inside a tolerance sized for the dense one.
    //
    // AND THE AREA MUST BE LARGE ENOUGH FOR THE COARSE OCTAVE TO AVERAGE OUT.
    // The forest octave is 288 m on this layer; at the 160-cell (3.8 km) window
    // this test first used, that is ~13 cells across and the measured
    // population swung 8% on sampling alone. 640 cells is ~15 km and ~53 forest
    // cells. See assetcluster_gain_has_mean_one, which measures the same thing
    // one level down.
    struct Case {
        int64_t keepNum;
        const char* label;
        int64_t worstLossPct;
    };
    //
    // MEASURED, and the tolerances are set just above what was measured rather
    // than at a comfortable round number -- a tolerance wide enough to admit a
    // regression is not a test. Sparse comes out at 0% at all three strengths;
    // the dense case is 0%, 0%, -3%, and the -3% is the upper-tail clipping
    // described above, appearing only at full strength where the gain's 4x
    // reach reaches twice this species' headroom.
    const Case cases[] = {
        {125, "sparse (125 per mille, 8x headroom)", 2},
        {500, "half the lattice (500 per mille, 2x headroom)", 5},
    };

    for (const Case& c : cases) {
        std::vector<int> counts;
        int base = 0;
        quadratCounts(
            L, 1, side, 8,
            [&](const AssetSite& s) {
                return assetClusterKeeps(kSeed, L, 1, 0, 0, s.anchorXMm, s.anchorYMm, s.cellX,
                                         s.cellY, c.keepNum, 1000);
            },
            counts, base);
        CHECK(base > 200);
        for (uint16_t strength : {uint16_t(256), uint16_t(512), uint16_t(kAssetQ10One)}) {
            int kept = 0;
            quadratCounts(
                L, 1, side, 8,
                [&](const AssetSite& s) {
                    return assetClusterKeeps(kSeed, L, 1, 0, strength, s.anchorXMm, s.anchorYMm,
                                             s.cellX, s.cellY, c.keepNum, 1000);
                },
                counts, kept);
            const int64_t deltaPct = (int64_t(kept) - int64_t(base)) * 100 / int64_t(base);
            std::printf("    %-46s strength %4d: %d -> %d (%+lld%%)\n", c.label, int(strength),
                        base, kept, static_cast<long long>(deltaPct));
            CHECK(deltaPct >= -c.worstLossPct);
            CHECK(deltaPct <= c.worstLossPct);
        }
    }
}

VXC_TEST(assetcluster_gain_has_mean_one_which_is_what_density_preservation_rests_on) {
    // THE PROPERTY THE WHOLE MECHANISM IS BUILT ON, measured directly rather
    // than inferred from the population it produces. E[gain] == kAssetQ10One is
    // why modulating a keep threshold by this field does not change how many
    // individuals there are; if it drifts, every clustered species in the
    // library ships at the wrong abundance and the only symptom is a world that
    // looks slightly wrong.
    //
    // Measured at the strength that stresses it most -- the full authored
    // range, where the two octaves span [0, 4x].
    // THE AREA HAS TO BE BIG ENOUGH FOR THE COARSE OCTAVE TO AVERAGE OUT, and
    // the first cut of this test was not. The forest octave's lattice is 12x
    // the layer's 24 m cell -- 288 m -- so a 400 m window contains barely one
    // and a half of its cells and measured a mean of 1150 against a true 1024:
    // not a bias in the field, a sample of two numbers. 12 km spans ~42 forest
    // cells on each axis, which brings the sampling error under a percent.
    //
    // That is also the honest statement of what "density is preserved" means
    // here: it is preserved GLOBALLY, and locally it varies by design. A field
    // that did not vary locally would not be clustering.
    const AssetLayer& L = kLayers[1];
    int64_t sum = 0, n = 0, lo = 1 << 30, hi = 0;
    for (int64_t y = 0; y < 12'000'000; y += 11'987)
        for (int64_t x = 0; x < 12'000'000; x += 11'971) {
            const int64_t g = assetClusterGainQ10(kSeed, L, 1, 0, kAssetQ10One, x, y);
            sum += g;
            if (g < lo) lo = g;
            if (g > hi) hi = g;
            ++n;
        }
    const int64_t mean = sum / n;
    std::printf("    mean gain over %lld samples: %lld (want %d), range %lld..%lld\n",
                static_cast<long long>(n), static_cast<long long>(mean), kAssetQ10One,
                static_cast<long long>(lo), static_cast<long long>(hi));
    // Within 2% of unity. Not tighter: this is a finite sample of a hash field,
    // and a threshold at the noise floor is a test that fails on a seed change.
    CHECK(mean > kAssetQ10One * 98 / 100);
    CHECK(mean < kAssetQ10One * 102 / 100);
}

VXC_TEST(assetcluster_gain_is_neutral_at_zero_strength_and_never_negative) {
    const AssetLayer& L = kLayers[1];
    for (int64_t y = -50'000; y <= 50'000; y += 3700)
        for (int64_t x = -50'000; x <= 50'000; x += 4100) {
            // Zero strength must be EXACTLY neutral, not approximately: a
            // species authored cluster 0 must scatter exactly as it did before
            // clustering existed, or every unclustered species in the library
            // silently moved.
            CHECK_EQ(assetClusterGainQ10(kSeed, L, 1, 0, 0, x, y), kAssetQ10One);
            // Two multiplied octaves, each at most 2x, so the product is at
            // most 4x. The bound is asserted so that a third octave -- or a
            // strength above the authored range -- cannot quietly widen the
            // gain past what the keep test's int64 arithmetic was sized for.
            const int32_t g = assetClusterGainQ10(kSeed, L, 1, 3, kAssetQ10One, x, y);
            CHECK(g >= 0);
            CHECK(g <= 4 * kAssetQ10One);
        }
}

VXC_TEST(assetcluster_gives_each_species_its_own_grove) {
    // Without a per-species field every species in a biome clumps in exactly
    // the same places, and the world has fertile patches and bare patches
    // rather than a beech grove beside an oak grove. Same position, different
    // species index, must disagree often.
    const AssetLayer& L = kLayers[1];
    int differing = 0, samples = 0;
    for (int64_t y = 0; y < 200'000; y += 5300)
        for (int64_t x = 0; x < 200'000; x += 4900) {
            const int32_t a = assetClusterGainQ10(kSeed, L, 1, 0, kAssetQ10One, x, y);
            const int32_t b = assetClusterGainQ10(kSeed, L, 1, 1, kAssetQ10One, x, y);
            ++samples;
            // "Differ meaningfully", not "differ at all": two independent
            // fields agree to within a few units often enough that a bare !=
            // would be a weaker test than it looks.
            if (a > b + 64 || b > a + 64) ++differing;
        }
    CHECK(samples > 1000);
    CHECK(differing * 10 > samples * 8); // 80%+ meaningfully different
}

// ---------------------------------------------------------------------------
// 4. THE CHANNEL RESPONSES (worldgen v26): the slope curve, the standing-water
// veto, the krummholz band, and the moisture/talus/curvature multipliers.
// Every one is sentinel-neutral by contract -- a column with no channels must
// weigh exactly as it did before the channels existed -- and every one is
// tested in BOTH directions, because a response that never fires and a
// response that always fires both read as "ran" to a census.
// ---------------------------------------------------------------------------

VXC_TEST(assetpolicy_slope_curve_tapers_weight_and_zeroes_at_the_ceiling) {
    AssetSpecies s = anywhere(1, 1000);
    s.slopeMaxMmPerM = 600;  // the curve's zero point
    s.slopeFullMmPerM = 400; // full weight to here

    const uint32_t flat = assetSpeciesSiteWeightQ10(s, goodGround(TEMPERATE_FOREST, 120'000, 100));
    const uint32_t atFull = assetSpeciesSiteWeightQ10(s, goodGround(TEMPERATE_FOREST, 120'000, 400));
    const uint32_t mid = assetSpeciesSiteWeightQ10(s, goodGround(TEMPERATE_FOREST, 120'000, 500));
    const uint32_t nearZero =
        assetSpeciesSiteWeightQ10(s, goodGround(TEMPERATE_FOREST, 120'000, 598));
    CHECK_EQ(int(flat), 1000);
    CHECK_EQ(int(atFull), 1000);
    CHECK_EQ(int(mid), 500);   // exactly halfway down the taper
    CHECK(nearZero < 50);
    // Past the zero point the GATE refuses -- the taper and the veto meet.
    CHECK(!assetSpeciesTolerates(s, goodGround(TEMPERATE_FOREST, 120'000, 601)));
    CHECK_EQ(int(assetSpeciesFirstRefusal(s, goodGround(TEMPERATE_FOREST, 120'000, 601))),
             int(AssetGate::kSlopeZero));

    // A hand-built species (default slopeFull = INT32_MAX) has NO taper: the
    // old hard cut, bit for bit. This is what keeps every pre-v26 table and
    // every direct-construction test meaning what it meant.
    AssetSpecies old = anywhere(1, 1000);
    old.slopeMaxMmPerM = 600;
    CHECK_EQ(int(assetSpeciesSiteWeightQ10(old, goodGround(TEMPERATE_FOREST, 120'000, 599))),
             1000);
}

VXC_TEST(assetpolicy_standing_water_vetoes_submerged_ground_only) {
    // THE LAKE-TREE VETO. anchorSolid is TRUE on a lake bed -- to every other
    // gate it is just ground -- so without this gate an oak stands in six
    // metres of water (the owner-reported defect of 2026-08-17's first
    // scatter vista).
    const AssetSpecies oak = anywhere(1, 1000);
    AssetColumnFacts dry = goodGround();
    CHECK(assetSpeciesTolerates(oak, dry));

    AssetColumnFacts shin = goodGround();
    shin.standingWaterMm = 300; // shin-deep: a wet strand still hosts
    CHECK(assetSpeciesTolerates(oak, shin));

    AssetColumnFacts lakeBed = goodGround();
    lakeBed.standingWaterMm = 6'000'000;
    CHECK(!assetSpeciesTolerates(oak, lakeBed));
    CHECK_EQ(int(assetSpeciesFirstRefusal(oak, lakeBed)), int(AssetGate::kStandingWater));
}

VXC_TEST(assetpolicy_moisture_affinity_moves_weight_with_twi_and_is_sentinel_neutral) {
    AssetSpecies willow = anywhere(1, 1000);
    willow.moistureAffinity = 2; // hygrophile
    AssetSpecies pine = anywhere(1, 1000);
    pine.moistureAffinity = -2; // xerophile

    AssetColumnFacts hollow = goodGround();
    hollow.twiMilli = 12'000; // a wet hollow
    AssetColumnFacts ridge = goodGround();
    ridge.twiMilli = 500; // a dry convex ridge
    AssetColumnFacts unknown = goodGround(); // twiMilli stays at the sentinel

    // The hygrophile gains where it is wet and starves where it is dry...
    CHECK(assetSpeciesSiteWeightQ10(willow, hollow) > 1500);
    CHECK(assetSpeciesSiteWeightQ10(willow, ridge) < 500);
    // ...the xerophile mirrors it...
    CHECK(assetSpeciesSiteWeightQ10(pine, hollow) < 500);
    CHECK(assetSpeciesSiteWeightQ10(pine, ridge) > 1500);
    // ...and NOBODY moves on the sentinel: a world with no moisture plane is
    // the world before the plane existed.
    CHECK_EQ(int(assetSpeciesSiteWeightQ10(willow, unknown)), 1000);
    CHECK_EQ(int(assetSpeciesSiteWeightQ10(pine, unknown)), 1000);
}

VXC_TEST(assetpolicy_talus_boosts_rocks_and_curvature_sorts_rocks_from_trees) {
    AssetSpecies rock = anywhere(2, 500);
    rock.talusAffinity = 1;
    rock.curvatureAffinity = -1; // convex-seeking
    AssetSpecies bigTree = anywhere(0, 500);
    bigTree.curvatureAffinity = 1; // concave-seeking
    bigTree.heightMm = 20'000;

    AssetColumnFacts debris = goodGround();
    debris.talus = 200;
    AssetColumnFacts clean = goodGround();
    clean.talus = 0;
    // Talus is BOOST-ONLY: flux multiplies the rock up, zero flux leaves the
    // authored weight alone (never a penalty -- rocks still place off-talus).
    CHECK(assetSpeciesSiteWeightQ10(rock, debris) > 1000);
    CHECK_EQ(int(assetSpeciesSiteWeightQ10(rock, clean)), 500);

    AssetColumnFacts hollowC = goodGround();
    hollowC.curv = 190; // strongly concave
    AssetColumnFacts ridgeC = goodGround();
    ridgeC.curv = 70; // strongly convex
    // The anti-correlation research section 4.2 asks for, from ONE channel:
    // rocks up on the ridge nose and down in the hollow, big trees inverted.
    CHECK(assetSpeciesSiteWeightQ10(rock, ridgeC) > assetSpeciesSiteWeightQ10(rock, hollowC));
    CHECK(assetSpeciesSiteWeightQ10(bigTree, hollowC) >
          assetSpeciesSiteWeightQ10(bigTree, ridgeC));
}

VXC_TEST(assetpolicy_krummholz_band_thins_tall_species_toward_the_treeline) {
    // THE BAND, NOT A LINE (research section 3.6): approaching the
    // temperature-adjusted treeline, the TALL species taper out while the
    // small hold -- so the last few hundred metres below the line read as
    // krummholz because of what is MISSING, with no krummholz special case.
    AssetSpecies tall = anywhere(0, 1000);
    tall.heightMm = 20'000;
    AssetSpecies mid = anywhere(1, 1000);
    mid.heightMm = 10'000;
    AssetSpecies krummholz = anywhere(2, 1000);
    krummholz.heightMm = 2'500;

    auto below = [&](int32_t deltaMm) {
        AssetColumnFacts c = goodGround(TAIGA, 800'000, 100);
        c.treelineDeltaMm = deltaMm;
        return c;
    };
    // Deep below the line nobody is touched.
    CHECK_EQ(int(assetSpeciesSiteWeightQ10(tall, below(500'000))), 1000);
    // 100 m below: the 300 m band has the tall crown at a third weight, the
    // 150 m band has the mid tree at two thirds, the krummholz untouched.
    CHECK_EQ(int(assetSpeciesSiteWeightQ10(tall, below(100'000))), 333);
    CHECK_EQ(int(assetSpeciesSiteWeightQ10(mid, below(100'000))), 666);
    CHECK_EQ(int(assetSpeciesSiteWeightQ10(krummholz, below(100'000))), 1000);
    // At and above the line the tall are gone; the small still hold (the
    // BIOME flip to alpine is what finally stops them, not this band).
    CHECK_EQ(int(assetSpeciesSiteWeightQ10(tall, below(0))), 0);
    CHECK_EQ(int(assetSpeciesSiteWeightQ10(krummholz, below(0))), 1000);
    // The sentinel is neutral: no temperature, no band.
    AssetColumnFacts noTemp = goodGround(TAIGA, 800'000, 100);
    CHECK_EQ(int(assetSpeciesSiteWeightQ10(tall, noTemp)), 1000);

    // Warm aspects carry trees higher: the same column, 100 m below the line,
    // reads better under a hot SW heat load and worse under a cold NE one.
    AssetColumnFacts warm = below(100'000);
    warm.heat = 254;
    AssetColumnFacts cold = below(100'000);
    cold.heat = 0;
    CHECK(assetSpeciesSiteWeightQ10(tall, warm) > assetSpeciesSiteWeightQ10(tall, below(100'000)));
    CHECK(assetSpeciesSiteWeightQ10(tall, cold) < assetSpeciesSiteWeightQ10(tall, below(100'000)));
}

VXC_TEST(assetpolicy_water_distance_gate_opens_when_the_channel_is_served) {
    // The audit's headline: 112 authored riparian species, refused EVERYWHERE
    // because the distance arrived as the sentinel. Serving a real distance is
    // the whole fix -- no species change, no new gate.
    AssetSpecies reed = anywhere(2, 800);
    reed.waterMaxMm = 3'000; // within 3 m of water

    AssetColumnFacts unknown = goodGround();
    unknown.distanceToWaterMm = kAssetNoWaterDistanceMm;
    CHECK(!assetSpeciesTolerates(reed, unknown));
    CHECK_EQ(int(assetSpeciesFirstRefusal(reed, unknown)), int(AssetGate::kWaterDistance));

    AssetColumnFacts shore = goodGround();
    shore.distanceToWaterMm = 2'000;
    CHECK(assetSpeciesTolerates(reed, shore));

    AssetColumnFacts inland = goodGround();
    inland.distanceToWaterMm = 40'000;
    CHECK(!assetSpeciesTolerates(reed, inland));
}

// ---------------------------------------------------------------------------
// 4. THE KIND x BIOME OCCUPANCY SCALAR (VXM2's density table, per species)
// ---------------------------------------------------------------------------

VXC_TEST(assetpolicy_occupancy_scalar_thins_a_saturated_biome_linearly_and_only_downward) {
    // The saturation defect this exists to fix: a species whose summed weight
    // saturates the occupancy cap stands on the layer density's share of
    // cells in EVERY biome, so savanna == forest. The scalar must (a) thin a
    // saturated biome by its stated fraction, (b) never add a site anywhere
    // (veto-only), (c) at 1000 reproduce the unscaled world exactly, and (d)
    // at 0 place nothing at all.
    AssetSpecies full = anywhere(1, 1000);   // saturates: occupancy cap == 1000
    AssetSpecies half = full;
    for (int b = 0; b < kBiomeCount; ++b) half.occupancyPerMille[b] = 500;
    AssetSpecies dark = full;
    for (int b = 0; b < kBiomeCount; ++b) dark.occupancyPerMille[b] = 0;

    int nFull = 0, nHalf = 0, nDark = 0, sites = 0;
    for (int64_t cy = 0; cy < 60; ++cy)
        for (int64_t cx = 0; cx < 60; ++cx) {
            AssetSite s;
            if (!assetSiteInCell(kSeed, kLayers[1], 1, cx, cy, s)) continue;
            ++sites;
            AssetInstance inst;
            const bool keptFull = assetResolveSite(kSeed, kLayers, kAssetLayerCount, &full, 1,
                                                   s, goodGround(SAVANNA), inst);
            const bool keptHalf = assetResolveSite(kSeed, kLayers, kAssetLayerCount, &half, 1,
                                                   s, goodGround(SAVANNA), inst);
            const bool keptDark = assetResolveSite(kSeed, kLayers, kAssetLayerCount, &dark, 1,
                                                   s, goodGround(SAVANNA), inst);
            nFull += keptFull;
            nHalf += keptHalf;
            nDark += keptDark;
            // (b) veto-only, site by site: the scalar may only remove.
            CHECK(!(keptHalf && !keptFull));
        }
    CHECK(sites > 1000);       // the census is real
    CHECK_EQ(nFull, sites);    // (c) saturated + neutral scalar: every site
    CHECK_EQ(nDark, 0);        // (d)
    // (a) 500 per-mille halves the population; a deterministic draw over
    // 1400+ sites lands within a few percent of half.
    CHECK(nHalf * 1000 > nFull * 450);
    CHECK(nHalf * 1000 < nFull * 550);
    std::printf("    occupancy scalar: %d sites, full %d, half %d, dark %d\n", sites, nFull,
                nHalf, nDark);
}

VXC_TEST(assetpolicy_occupancy_scalar_is_per_biome) {
    // One species, thinned in savanna and untouched in temperate forest: the
    // same site keeps its tree in the forest and usually loses it in the
    // savanna. This is the cross-biome contrast the census measured as absent.
    AssetSpecies sp = anywhere(1, 1000);
    sp.occupancyPerMille[SAVANNA] = 150;

    int nForest = 0, nSavanna = 0, sites = 0;
    for (int64_t cy = 0; cy < 40; ++cy)
        for (int64_t cx = 0; cx < 40; ++cx) {
            AssetSite s;
            if (!assetSiteInCell(kSeed, kLayers[1], 1, cx, cy, s)) continue;
            ++sites;
            AssetInstance inst;
            nForest += assetResolveSite(kSeed, kLayers, kAssetLayerCount, &sp, 1, s,
                                        goodGround(TEMPERATE_FOREST), inst);
            nSavanna += assetResolveSite(kSeed, kLayers, kAssetLayerCount, &sp, 1, s,
                                         goodGround(SAVANNA), inst);
        }
    CHECK(sites > 400);
    CHECK_EQ(nForest, sites);
    CHECK(nSavanna > 0);
    CHECK(nSavanna * 1000 < sites * 250); // ~150 per-mille, generous ceiling
    std::printf("    per-biome occupancy: %d sites, forest %d, savanna %d\n", sites, nForest,
                nSavanna);
}
