// Detail entities: herds, flocks, shoals -- and the salt/fresh split.
//
// Two things here are worth more than the rest of the file:
//
//   1. THE SALINITY TABLE, driven at every boundary. The salt/fresh answer is
//      read off which of implicitWaterDatumMm's two operands won its max(), so
//      the interesting cases are the ones where they TIE (a river mouth) and
//      where the tie is broken by a millimetre. Those are the cases a
//      hand-written spawner gets wrong and nobody notices, because a fish in
//      the wrong hundred metres of a river is not a bug report.
//
//   2. THE IN-COLUMN SWEEP. Every placed fish must be strictly between the bed
//      and the surface, over thousands of placements across water of every
//      depth including water shallower than the species asked for. A fish
//      placed at its authored depth into mud is invisible -- water hides it --
//      which makes it exactly the derived-not-verified failure this design is
//      built around.

#include "voxelcore/assetdetail.h"

#include <cstdio>
#include <vector>

#include "vxctest.h"

using namespace vxc;

namespace {

constexpr uint64_t kSeed = 0xf155c40015eed01ull;

AssetAquaticSpecies trout() {
    AssetAquaticSpecies s;
    for (int b = 0; b < kBiomeCount; ++b) s.weightPerMille[b] = 500;
    s.waterMask = kWaterMaskFresh;
    s.depthMinMm = 500;
    s.depthMaxMm = 4000;
    s.minWaterDepthMm = 800;
    s.groupMin = 3;
    s.groupMax = 12;
    s.groupRadiusMm = 6000;
    return s;
}

AssetLandSpecies deer() {
    AssetLandSpecies s;
    for (int b = 0; b < kBiomeCount; ++b) s.weightPerMille[b] = 500;
    s.groupMin = 2;
    s.groupMax = 9;
    s.groupRadiusMm = 30'000;
    return s;
}

WaterColumnFacts water(int32_t groundMm, int32_t bakedMm) {
    WaterColumnFacts w;
    w.known = true;
    w.groundMm = groundMm;
    w.bakedSurfaceMm = bakedMm;
    return w;
}

} // namespace

// ---------------------------------------------------------------------------
// SALT VERSUS FRESH
// ---------------------------------------------------------------------------

VXC_TEST(salinity_is_read_off_which_term_won_the_datum_max) {
    // Dry land: neither term.
    CHECK_EQ(int(classifyWaterSalinity(120'000, kNoWaterMm)), int(WaterSalinity::kNone));

    // Open sea: ground below the datum, nothing baked.
    CHECK_EQ(int(classifyWaterSalinity(-40'000, kNoWaterMm)), int(WaterSalinity::kSalt));
    // ONE MILLIMETRE MATTERS AND IT IS DELIBERATE. oceanSurfaceMmAt is strictly
    // below, because a column whose ground sits exactly on the datum is a beach
    // with zero depth of water over it (lakes.h:1386-1390).
    CHECK_EQ(int(classifyWaterSalinity(kSeaLevelMm - 1, kNoWaterMm)), int(WaterSalinity::kSalt));
    CHECK_EQ(int(classifyWaterSalinity(kSeaLevelMm, kNoWaterMm)), int(WaterSalinity::kNone));

    // An inland lake: ground above the datum, a baked surface over it.
    CHECK_EQ(int(classifyWaterSalinity(300'000, 305'000)), int(WaterSalinity::kFresh));
    // A coastal lake perched above sea level on ground that is still below it.
    // Both terms present; the lake wins the max, so it is fresh.
    CHECK_EQ(int(classifyWaterSalinity(-2000, 5000)), int(WaterSalinity::kFresh));

    // THE RIVER MOUTH. The reach's own datum has descended to meet
    // kSeaLevelMm, so the two terms are the SAME NUMBER -- which
    // implicitWaterDatumMm's own comment says is on purpose, so that the join
    // is invisible. Naming it brackish is the only answer that lets a species
    // say which it tolerates; calling it salt keeps trout out of the last
    // hundred metres of every river in the world.
    CHECK_EQ(int(classifyWaterSalinity(-1000, kSeaLevelMm)), int(WaterSalinity::kBrackish));

    // The sea drowning a baked surface below it.
    CHECK_EQ(int(classifyWaterSalinity(-8000, -3000)), int(WaterSalinity::kSalt));
}

VXC_TEST(salinity_agrees_with_the_datum_composition_it_is_derived_from) {
    // The classification and implicitWaterDatumMm must never disagree about
    // whether there is water at all. They read the same two numbers; if they
    // could differ, a fish would spawn where the renderer draws no water.
    for (int32_t ground = -20'000; ground <= 20'000; ground += 137)
        for (int32_t baked : {kNoWaterMm, -15'000, -1, 0, 1, 9000, 30'000}) {
            const WaterSalinity s = classifyWaterSalinity(ground, baked);
            const int32_t datum = implicitWaterDatumMm(baked, ground);
            CHECK_EQ(int(s == WaterSalinity::kNone), int(datum == kNoWaterMm));
        }
}

VXC_TEST(salinity_mask_admits_exactly_what_it_names) {
    CHECK_EQ(int(kWaterMaskAny & waterSalinityBit(WaterSalinity::kFresh)), int(kWaterMaskFresh));
    CHECK_EQ(int(kWaterMaskAny & waterSalinityBit(WaterSalinity::kSalt)), int(kWaterMaskSalt));
    CHECK_EQ(int(kWaterMaskFresh & waterSalinityBit(WaterSalinity::kSalt)), 0);
    // kNone has a bit too, and no mask must ever contain it: "any water" is not
    // "anywhere".
    CHECK_EQ(int(kWaterMaskAny & waterSalinityBit(WaterSalinity::kNone)), 0);
}

VXC_TEST(waterfacts_depth_is_never_negative_and_is_zero_on_dry_ground) {
    CHECK_EQ(water(120'000, kNoWaterMm).depthMm(), 0);
    CHECK_EQ(water(-40'000, kNoWaterMm).depthMm(), 40'000);
    CHECK_EQ(water(300'000, 305'000).depthMm(), 5000);
    // A bed above its own baked datum is not water an inch deep, it is dry
    // ground -- and a negative depth would sail straight through every
    // `depth < minWaterDepth` gate as a very large unsigned number if anyone
    // ever widened these types.
    CHECK_EQ(water(310'000, 305'000).depthMm(), 0);
}

// ---------------------------------------------------------------------------
// GROUPS
// ---------------------------------------------------------------------------

VXC_TEST(detailgroups_are_deterministic_sized_and_seeded_consecutively) {
    DetailGroupLayer L;
    L.cellMm = 120'000;
    L.densityPerMille = 600;

    int found = 0;
    for (int64_t cy = -8; cy <= 8; ++cy)
        for (int64_t cx = -8; cx <= 8; ++cx) {
            DetailGroupSite a, b;
            const bool ha = detailGroupInCell(kSeed, L, 3, 12, cx, cy, a);
            const bool hb = detailGroupInCell(kSeed, L, 3, 12, cx, cy, b);
            CHECK_EQ(int(ha), int(hb));
            if (!ha) continue;
            CHECK(a == b);
            ++found;
            // Size within the authored bounds, at both ends inclusive.
            CHECK(a.count >= 3);
            CHECK(a.count <= 12);
            // The centre stays in its own cell, exactly as an asset anchor does.
            CHECK(a.centreXMm >= cx * L.cellMm);
            CHECK(a.centreXMm < (cx + 1) * L.cellMm);
            CHECK(a.centreYMm >= cy * L.cellMm);
            CHECK(a.centreYMm < (cy + 1) * L.cellMm);
        }
    CHECK(found > 100);
}

VXC_TEST(detailgroups_solitary_species_get_groups_of_one) {
    // groupMin == groupMax == 1 is how the library spells "solitary", and a
    // span computed as (max - min) without the +1 would divide by zero here.
    DetailGroupLayer L;
    L.cellMm = 90'000;
    int found = 0;
    for (int64_t cx = -20; cx <= 20; ++cx) {
        DetailGroupSite g;
        if (!detailGroupInCell(kSeed, L, 1, 1, cx, 0, g)) continue;
        CHECK_EQ(int(g.count), 1);
        ++found;
    }
    CHECK(found > 10);
    // And an inverted pair must not produce a nonsense count.
    DetailGroupSite g;
    if (detailGroupInCell(kSeed, L, 9, 2, 0, 0, g)) CHECK_EQ(int(g.count), 9);
}

VXC_TEST(detailgroups_in_a_disc_are_inside_the_disc) {
    DetailGroupLayer L;
    L.cellMm = 60'000;
    const int64_t r = 250'000;
    const std::vector<DetailGroupSite> gs = detailGroupsInDisc(kSeed, L, 2, 6, 1'000'000, -700'000,
                                                               r);
    CHECK(!gs.empty());
    for (const DetailGroupSite& g : gs) {
        const int64_t dx = g.centreXMm - 1'000'000, dy = g.centreYMm + 700'000;
        CHECK(dx * dx + dy * dy <= r * r);
    }
    // A zero radius must not return the world, and a negative one must not
    // loop forever.
    CHECK(detailGroupsInDisc(kSeed, L, 2, 6, 0, 0, -1).empty());
}

// ---------------------------------------------------------------------------
// PLACEMENT, VERIFIED
// ---------------------------------------------------------------------------

VXC_TEST(detail_members_are_refused_when_the_world_fact_is_missing) {
    // Same discipline as AssetColumnFacts::known. There is no overload that
    // omits the ground or the water, and the default is "unknown", so a caller
    // who forgets places nothing.
    DetailGroupLayer L;
    DetailGroupSite g;
    CHECK(detailGroupInCell(kSeed, L, 2, 6, 0, 0, g));
    DetailMember m;

    CHECK(!detailPlaceLandMember(kSeed, g, deer(), 0, false, 120'000, 100, TEMPERATE_FOREST, m));
    CHECK(detailPlaceLandMember(kSeed, g, deer(), 0, true, 120'000, 100, TEMPERATE_FOREST, m));

    WaterColumnFacts unknown; // default: known == false
    CHECK(!unknown.known);
    CHECK(!detailPlaceWaterMember(kSeed, g, trout(), 0, unknown, TEMPERATE_FOREST, m));
    CHECK(detailPlaceWaterMember(kSeed, g, trout(), 0, water(100'000, 106'000), TEMPERATE_FOREST,
                                 m));

    // And an index past the end of the group is refused rather than placed as
    // a phantom member -- a shoal of 4 must not be able to produce a fifth fish.
    CHECK(!detailPlaceWaterMember(kSeed, g, trout(), g.count, water(100'000, 106'000),
                                  TEMPERATE_FOREST, m));
}

VXC_TEST(detail_land_members_stand_on_the_ground_they_were_given) {
    // Not on the group centre's ground -- on their own. A herd on a hillside
    // stands on the hillside; using the centre's height would tilt every herd
    // into the slope, which is a floating animal at one end and a buried one at
    // the other.
    DetailGroupLayer L;
    L.cellMm = 150'000;
    const AssetLandSpecies sp = deer();
    // A synthetic sloping surface, deliberately not axis-aligned.
    const auto surfaceAt = [](int64_t x, int64_t y) -> int32_t {
        return static_cast<int32_t>(200'000 + x / 40 + y / 70);
    };
    int placed = 0;
    for (int64_t cy = -6; cy <= 6; ++cy)
        for (int64_t cx = -6; cx <= 6; ++cx) {
            DetailGroupSite g;
            if (!detailGroupInCell(kSeed, L, sp.groupMin, sp.groupMax, cx, cy, g)) continue;
            for (uint16_t i = 0; i < g.count; ++i) {
                // Two passes: the offset first so we know where the member is,
                // then the ground THERE, which is what the real spawner does.
                int64_t mx = 0, my = 0;
                detailMemberOffsetMm(kSeed, g, i, sp.groupRadiusMm, mx, my);
                DetailMember m;
                if (!detailPlaceLandMember(kSeed, g, sp, i, true, surfaceAt(mx, my), 100,
                                           TEMPERATE_FOREST, m))
                    continue;
                CHECK_EQ(m.xMm, mx);
                CHECK_EQ(m.yMm, my);
                CHECK_EQ(m.zMm, surfaceAt(mx, my)); // on ITS OWN ground, exactly
                CHECK_EQ(int(m.seedIndex), int(uint16_t(g.seedIndex + i)));
                CHECK(m.yawQuarter < 4);
                ++placed;
            }
        }
    // The vacuous-truth guard.
    CHECK(placed > 200);
    std::printf("    land members placed on their own ground: %d\n", placed);
}

VXC_TEST(detail_water_members_are_strictly_inside_the_water_column) {
    // THE SWEEP. Every fish, over water of every depth from a puddle to 600 m,
    // including water far shallower than the species asked for and water where
    // the authored band overhangs the bed entirely.
    DetailGroupLayer L;
    L.cellMm = 80'000;
    const AssetAquaticSpecies sp = trout();

    int placed = 0, refusedShallow = 0, refusedDry = 0, refusedSalt = 0;
    for (int64_t cx = -14; cx <= 14; ++cx)
        for (int32_t depth : {0, 100, 400, 799, 800, 1500, 5000, 60'000, 600'000}) {
            DetailGroupSite g;
            if (!detailGroupInCell(kSeed, L, sp.groupMin, sp.groupMax, cx, 5, g)) continue;
            // A freshwater column: ground above the datum with a baked surface
            // `depth` above it.
            const int32_t ground = 400'000;
            const WaterColumnFacts w =
                depth == 0 ? water(ground, kNoWaterMm) : water(ground, ground + depth);
            for (uint16_t i = 0; i < g.count; ++i) {
                DetailMember m;
                if (!detailPlaceWaterMember(kSeed, g, sp, i, w, TEMPERATE_FOREST, m)) {
                    if (depth == 0)
                        ++refusedDry;
                    else if (depth < sp.minWaterDepthMm)
                        ++refusedShallow;
                    continue;
                }
                // THE POST-CONDITION. Strictly between the bed and the surface,
                // with no tolerance: a fish exactly on the bed is inside the
                // mud and one exactly on the surface reads as a dead fish, and
                // both are one rounding away from being out of the water.
                CHECK(m.zMm > w.groundMm);
                CHECK(m.zMm < w.surfaceMm());
                ++placed;
            }
        }

    // A salt species must be refused in fresh water, and this is the direction
    // that would pass silently if the mask were ignored: the fish would spawn,
    // in water, at a plausible depth, in the wrong body.
    AssetAquaticSpecies reef = trout();
    reef.waterMask = kWaterMaskSalt;
    reef.minWaterDepthMm = 1000;
    for (int64_t cx = -14; cx <= 14; ++cx) {
        DetailGroupSite g;
        if (!detailGroupInCell(kSeed, L, reef.groupMin, reef.groupMax, cx, 5, g)) continue;
        DetailMember m;
        if (!detailPlaceWaterMember(kSeed, g, reef, 0, water(400'000, 410'000), TEMPERATE_FOREST,
                                    m))
            ++refusedSalt;
    }

    std::printf("    fish placed in-column %d; refused dry %d, too shallow %d, wrong water %d\n",
                placed, refusedDry, refusedShallow, refusedSalt);
    CHECK(placed > 300);      // the sweep proved something
    CHECK(refusedDry > 0);    // ... and each refusal branch was actually taken
    CHECK(refusedShallow > 0);
    CHECK(refusedSalt > 0);
}

VXC_TEST(detail_water_member_band_is_clamped_into_shallow_water_not_pushed_through_the_bed) {
    // A bottom fish in water only slightly deeper than its minimum: the
    // authored band (0.5 m to 4 m below the surface) reaches below the bed. The
    // member must land at the deepest legal point, not at its authored depth,
    // and certainly not below the mud.
    AssetAquaticSpecies bottom;
    for (int b = 0; b < kBiomeCount; ++b) bottom.weightPerMille[b] = 900;
    bottom.waterMask = kWaterMaskAny;
    bottom.depthMinMm = 20'000; // holds 20-40 m down...
    bottom.depthMaxMm = 40'000;
    bottom.minWaterDepthMm = 300; // ...but tolerates shallow water
    bottom.groupMin = 4;
    bottom.groupMax = 4;

    DetailGroupLayer L;
    DetailGroupSite g;
    CHECK(detailGroupInCell(kSeed, L, 4, 4, 0, 0, g));
    const WaterColumnFacts w = water(0, 1200); // 1.2 m of water
    int placed = 0;
    for (uint16_t i = 0; i < g.count; ++i) {
        DetailMember m;
        if (!detailPlaceWaterMember(kSeed, g, bottom, i, w, TEMPERATE_FOREST, m)) continue;
        CHECK(m.zMm > w.groundMm);
        CHECK(m.zMm < w.surfaceMm());
        ++placed;
    }
    CHECK_EQ(placed, 4);

    // And a column with under one voxel of water holds nothing at all, whatever
    // the species tolerates: there is nowhere strictly between bed and surface.
    AssetAquaticSpecies puddleFish = bottom;
    puddleFish.minWaterDepthMm = 10;
    DetailMember m;
    CHECK(!detailPlaceWaterMember(kSeed, g, puddleFish, 0, water(0, kVoxelSizeMm),
                                  TEMPERATE_FOREST, m));
}

VXC_TEST(detail_biome_weight_zero_keeps_a_species_out) {
    // The same promise the land policy makes, for the 106 fish and 18 cetaceans
    // that carry the weights. A reef fish's ocean weight is what puts it in the
    // sea, and a zero anywhere else is what keeps it out.
    AssetAquaticSpecies oceanOnly = trout();
    for (int b = 0; b < kBiomeCount; ++b) oceanOnly.weightPerMille[b] = 0;
    oceanOnly.weightPerMille[OCEAN] = 900;
    oceanOnly.waterMask = kWaterMaskAny;

    DetailGroupLayer L;
    DetailGroupSite g;
    CHECK(detailGroupInCell(kSeed, L, 3, 12, 0, 0, g));
    const WaterColumnFacts w = water(-30'000, kNoWaterMm); // open sea
    DetailMember m;
    CHECK(detailPlaceWaterMember(kSeed, g, oceanOnly, 0, w, OCEAN, m));
    CHECK(!detailPlaceWaterMember(kSeed, g, oceanOnly, 0, w, TEMPERATE_FOREST, m));
    // An out-of-range biome id must refuse rather than read past the array.
    CHECK(!detailPlaceWaterMember(kSeed, g, oceanOnly, 0, w, BiomeId(kBiomeCount), m));
}
