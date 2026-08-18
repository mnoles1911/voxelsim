// Biome classification v1 (M4 round-1, docs/m4-plan.md): morphology gates
// before the Whittaker climate table. Direct classifyBiome()/
// biomeSurfaceMaterial() calls with hand-picked climate/morphology inputs
// (not routed through Amplifier/tiles) so every case is exactly controlled.

#include "voxelcore/biome.h"
#include "vxctest.h"

using namespace vxc;

namespace {
// Generic "inland, non-coastal, non-alpine" elevation used by every test
// that isn't specifically exercising the beach/ocean/treeline gates.
constexpr int32_t kInlandMm = 200'000; // 200m

// Climate inputs are written in PHYSICAL units and converted, exactly as
// biome.h's thresholds are. Before v8 these tests used bare u8 literals, which
// is how they kept passing while the thresholds meant something completely
// different from what their names said: `classifyBiome(200, 20, ...)` reads as
// "hot and dry" but 20 decoded to 941 mm/yr, which is not dry at all -- it only
// landed in the arid branch because that branch had swallowed the entire range.
constexpr int32_t T(int64_t degC) { return climateTempU8FromDegC(degC); }
constexpr int32_t P(int64_t mmPerYr) { return climatePrecipU8FromMmPerYr(mmPerYr); }
// v22: the savanna gate's channel is bio_15 (precipitation seasonality, a
// coefficient of variation in TENTHS OF A PERCENT), not bio_4. The old helper
// was S(bio4) = climateSeasonalityU8From; it is deliberately not kept, because
// the argument type is the whole point -- a test that still passed a bio_4
// number here would compile and quietly mean CV 400% clamped to the top of the
// range. V(1000) is CV 100%.
constexpr int32_t V(int64_t cvDeciPct) { return climatePrecipVarU8FromDeciPct(cvDeciPct); }
} // namespace

VXC_TEST(biome_hot_dry_is_desert) {
    // 30 C and 150 mm/yr: Sahara.
    CHECK_EQ(classifyBiome(T(30), P(150), V(400), kInlandMm, 0), DESERT);
}

VXC_TEST(biome_cold_is_taiga_or_tundra) {
    // Cold, but below the (temperature-lowered) treeline at this elevation:
    // Whittaker's cold band -> TAIGA.
    CHECK_EQ(classifyBiome(T(-5), P(600), V(400), 100'000, 0), TAIGA);
    // Same cold climate, higher elevation crosses that lowered treeline ->
    // gated to TUNDRA_ALPINE before Whittaker ever runs.
    CHECK_EQ(classifyBiome(T(-5), P(600), V(400), 500'000, 0), TUNDRA_ALPINE);
}

VXC_TEST(biome_steep_slope_no_longer_overrides_climate_on_land) {
    // v27 (owner decision): dry land NEVER classifies BARE_ROCK, whatever the
    // slope -- a cliff-steep warm wet hillside keeps its climate's biome and
    // can carry the slope-curve-gated vegetation. (v8..v26 the second call
    // returned BARE_ROCK; before v8, TUNDRA_ALPINE at an 11-degree
    // "cliff" threshold.)
    CHECK_EQ(classifyBiome(T(25), P(2500), V(400), kInlandMm, 0), RAINFOREST);
    CHECK_EQ(classifyBiome(T(25), P(2500), V(400), kInlandMm,
                           kBiomeCliffSlopeMmPerM + 1),
             RAINFOREST);
    // Where the slope gate still lives: BELOW the coastal band. A steep
    // submarine face is rock, not mud seafloor -- sediment does not rest
    // above the angle of repose -- and this is the ONLY route to BARE_ROCK.
    CHECK_EQ(classifyBiome(T(25), P(2500), V(400), kBiomeBeachLowerMm - 1,
                           kBiomeCliffSlopeMmPerM + 1),
             BARE_ROCK);
    CHECK_EQ(classifyBiome(T(25), P(2500), V(400), kBiomeBeachLowerMm - 1,
                           kBiomeCliffSlopeMmPerM),
             OCEAN);
}

VXC_TEST(biome_wet_warm_is_rainforest) {
    CHECK_EQ(classifyBiome(T(25), P(2500), V(400), kInlandMm, 0), RAINFOREST);
}

VXC_TEST(biome_precip_seasonality_splits_savanna_from_grassland) {
    // Same warm, semi-arid climate; only the DRY SEASON differs (bio_15).
    // v27 moved the grassland/forest boundary to 450 mm/yr, so the semi-arid
    // band this test needs is now 400-446 mm (one u8 step); 420 mm sits in it.
    CHECK_EQ(classifyBiome(T(20), P(420), V(1000), kInlandMm, 0), SAVANNA);
    CHECK_EQ(classifyBiome(T(20), P(420), V(200), kInlandMm, 0), GRASSLAND);
    // 600 mm -- semi-arid grassland through v26 -- is exactly the ground the
    // owner wanted wooded: it reads TEMPERATE_FOREST now (still SAVANNA when
    // strongly seasonal, unchanged).
    CHECK_EQ(classifyBiome(T(20), P(600), V(1000), kInlandMm, 0), SAVANNA);
    CHECK_EQ(classifyBiome(T(20), P(600), V(200), kInlandMm, 0), TEMPERATE_FOREST);
}

VXC_TEST(biome_precip_seasonality_splits_forest_types) {
    // Same warm, moderate-precip climate; only the DRY SEASON differs (bio_15).
    CHECK_EQ(classifyBiome(T(20), P(1200), V(1000), kInlandMm, 0), SAVANNA);
    CHECK_EQ(classifyBiome(T(20), P(1200), V(200), kInlandMm, 0), TEMPERATE_FOREST);
}

VXC_TEST(biome_coastal_band_beach_and_ocean) {
    CHECK_EQ(classifyBiome(128, 128, 0, 0, 0), BEACH);
    CHECK_EQ(classifyBiome(128, 128, 0, kBiomeBeachUpperMm, 0), BEACH);
    CHECK_EQ(classifyBiome(128, 128, 0, kBiomeBeachLowerMm, 0), BEACH);
    CHECK_EQ(classifyBiome(128, 128, 0, kBiomeBeachLowerMm - 1, 0), OCEAN);
    CHECK_EQ(classifyBiome(128, 128, 0, -1'000'000, 0), OCEAN);
}

VXC_TEST(biome_morphology_gate_varying_slope) {
    // Fixed climate that Whittaker-picks TEMPERATE_FOREST at slope 0
    // (10 C is not "warm", 1200 mm/yr is in the moderate band). v27: the
    // answer is now slope-INDEPENDENT on land -- the third case used to flip
    // to BARE_ROCK past the cliff threshold and no longer does. This is the
    // exact hillside the owner walked: steep temperate ground must read as
    // forest, with rock showing only through the stratigraphy on the faces.
    CHECK_EQ(classifyBiome(T(10), P(1200), V(200), kInlandMm, 0), TEMPERATE_FOREST);
    CHECK_EQ(classifyBiome(T(10), P(1200), V(200), kInlandMm, kBiomeCliffSlopeMmPerM),
             TEMPERATE_FOREST);
    CHECK_EQ(classifyBiome(T(10), P(1200), V(200), kInlandMm, kBiomeCliffSlopeMmPerM + 1),
             TEMPERATE_FOREST);
}

VXC_TEST(biome_treeline_rises_with_temperature) {
    CHECK(biomeTreelineMm(128) == kBiomeTreelineBaseMm);
    CHECK(biomeTreelineMm(255) > biomeTreelineMm(128));
    CHECK(biomeTreelineMm(0) < biomeTreelineMm(128));
    // Never dips below the coastal band regardless of how cold.
    CHECK(biomeTreelineMm(0) >= kBiomeBeachUpperMm);
}

VXC_TEST(biome_surface_material_mapping) {
    CHECK_EQ(biomeSurfaceMaterial(OCEAN, -10'000), MAT_MUD);
    CHECK_EQ(biomeSurfaceMaterial(BEACH, 0), MAT_SAND);
    CHECK_EQ(biomeSurfaceMaterial(GRASSLAND, kInlandMm), MAT_GRASS);
    CHECK_EQ(biomeSurfaceMaterial(TEMPERATE_FOREST, kInlandMm), MAT_TOPSOIL);
    CHECK_EQ(biomeSurfaceMaterial(RAINFOREST, kInlandMm), MAT_JUNGLE_SOIL);
    CHECK_EQ(biomeSurfaceMaterial(DESERT, kInlandMm), MAT_SAND);
    CHECK_EQ(biomeSurfaceMaterial(SAVANNA, kInlandMm), MAT_SAVANNA_GRASS);
    CHECK_EQ(biomeSurfaceMaterial(TAIGA, kInlandMm), MAT_PODZOL);
    CHECK_EQ(biomeSurfaceMaterial(TUNDRA_ALPINE, kInlandMm), MAT_PERMAFROST);
    CHECK_EQ(biomeSurfaceMaterial(TUNDRA_ALPINE, kBiomeAlpineRockLineMm + 1), MAT_ROCK);
    CHECK_EQ(biomeSurfaceMaterial(BARE_ROCK, kInlandMm), MAT_ROCK);
    // BARE_ROCK is rock at ANY elevation -- that is the whole point of adding
    // it. A sea cliff is not permafrost.
    CHECK_EQ(biomeSurfaceMaterial(BARE_ROCK, kBiomeBeachUpperMm + 1), MAT_ROCK);
}

VXC_TEST(biome_never_alpine_below_sea_level) {
    // Until v8 the slope gate ran ahead of the sea-level gates, so steep
    // seafloor classified as TUNDRA_ALPINE -- 17.8% of the real 25-tile world
    // was submarine permafrost. Nothing below the coastal band may be anything
    // but OCEAN or (v27, steep faces only) BARE_ROCK, whatever the climate.
    for (int32_t t = 0; t <= 255; t += 5)
        for (int32_t p = 0; p <= 255; p += 7)
            for (int64_t slope = 0; slope <= 60'000; slope += 3000)
                for (int32_t depthMm : {kBiomeBeachLowerMm - 1, -100'000, -1'000'000,
                                        -6'000'000})
                    CHECK_EQ(classifyBiome(t, p, (t * 3 + p * 5) & 0xff, depthMm, slope),
                             slope > kBiomeCliffSlopeMmPerM ? BARE_ROCK : OCEAN);
}

VXC_TEST(biome_every_id_reachable_from_physical_ranges) {
    // Sweeps the DOCUMENTED PHYSICAL DOMAIN, not a sampler, and asserts every
    // BiomeId is produced by some real-world climate. This is the test that
    // would have caught the original bug on day one: at v6, four of the nine
    // ids were unreachable from ANY input a real tile could carry, because
    // kBiomePrecipAridU8 sat above the whole precipitation range.
    //
    // IT DID NOT CATCH THE v8..v21 SAVANNA BUG, and the reason is worth writing
    // down: the sweep is a CARTESIAN PRODUCT, so it happily evaluated bio_4
    // 2200 together with 30 C -- a combination that does not exist anywhere on
    // Earth (the maximum bio_4 above 18 C is 1084). Reachability from the
    // product of the marginals is not reachability from the JOINT distribution,
    // and only the latter says whether a biome can occur. See
    // biome_savanna_gate_is_reachable_from_real_climates below, which uses
    // measured co-occurring values rather than a product.
    bool seen[kBiomeCount] = {false};
    for (int64_t degC = -35; degC <= 35; degC += 1)
        for (int64_t mm = 0; mm <= 4000; mm += 50)
            for (int64_t cv : {int64_t(200), int64_t(1200)})
                for (int32_t elevMm : {kBiomeBeachLowerMm - 1, kBiomeBeachUpperMm,
                                       kInlandMm, 2'000'000, 4'000'000})
                    for (int64_t slope : {int64_t(0), kBiomeCliffSlopeMmPerM + 1})
                        seen[classifyBiome(T(degC), P(mm), V(cv), elevMm, slope)] = true;
    for (int b = 0; b < kBiomeCount; ++b) CHECK(seen[b]);
}

VXC_TEST(biome_savanna_gate_is_reachable_from_real_climates) {
    // THE REGRESSION FOR THE v8..v21 GATE BUG. Every triple below is a real
    // (bio_1, bio_12, bio_15) read off the WorldClim 2.1 10' rasters at the
    // named place -- co-occurring values, not a product of marginals -- so a
    // gate that no climate on Earth satisfies fails here instead of shipping.
    // Measurement: docs/measurements/biome-gates-2026-08-01.txt.
    struct Site { const char* name; int64_t degC; int64_t mm; int64_t cvDeciPct; };
    constexpr Site kSavanna[] = {
        {"Niamey, Sahel",          29, 497, 1370},
        {"Cerrado, Brasilia",      21, 1460, 830},
        {"Katherine, N Australia", 27, 982, 1140},
        {"Zambia miombo",          20, 1034, 1130},
        {"Deccan, India",          27, 794, 1080},
        {"Tsavo, Kenya",           24, 1000, 720},
        {"Burkina Faso",           27, 807, 1170},
    };
    // Warm and in the same precipitation window, but with no dry season. These
    // are the false positives a bio_4 gate produced.
    constexpr Site kNotSavanna[] = {
        {"Houston, TX",   20, 1209, 220},
        {"Brisbane, AU",  20, 1218, 460},
        {"Miami, FL",     24, 1498, 540},
        {"Durban, ZA",    20, 1010, 410},
        {"Seville, ES",   18, 536, 670},
    };
    for (const Site& s : kSavanna)
        CHECK_EQ(classifyBiome(T(s.degC), P(s.mm), V(s.cvDeciPct), kInlandMm, 0), SAVANNA);
    for (const Site& s : kNotSavanna)
        CHECK(classifyBiome(T(s.degC), P(s.mm), V(s.cvDeciPct), kInlandMm, 0) != SAVANNA);
}

VXC_TEST(biome_classification_is_deterministic) {
    for (int32_t t = 0; t <= 255; t += 17)
        for (int32_t p = 0; p <= 255; p += 23)
            for (int64_t s = 0; s <= 12000; s += 1500) {
                const BiomeId a = classifyBiome(t, p, (t * 37 + p * 11) & 0xff, kInlandMm, s);
                const BiomeId b = classifyBiome(t, p, (t * 37 + p * 11) & 0xff, kInlandMm, s);
                CHECK_EQ(a, b);
                CHECK(a < kBiomeCount);
            }
}

VXC_TEST(biome_map_golden_digest) {
    Digest d;
    for (int32_t t = 0; t <= 255; t += 5)
        for (int32_t p = 0; p <= 255; p += 7) {
            const int32_t precipVar = (t * 3 + p * 5) & 0xff;
            for (int64_t elevM = -20; elevM <= 40; elevM += 4) {
                const int32_t surfaceMm = static_cast<int32_t>(elevM * 100'000);
                for (int64_t slope = 0; slope <= 9000; slope += 3000) {
                    const BiomeId biome = classifyBiome(t, p, precipVar, surfaceMm, slope);
                    d.u8(static_cast<uint8_t>(biome));
                    d.u8(biomeSurfaceMaterial(biome, surfaceMm));
                }
            }
        }
    // Re-pinned at worldgen v22 (was 0x7D36AFFD6B9DE5C5 at v8..v21; bio_4 ->
    // bio_15 savanna gate). Re-pinned again at v27: the dry-land cliff gate is
    // gone (slope cells above 700 mm/m flip from BARE_ROCK to their climate's
    // biome on land and to BARE_ROCK only below the coastal band) and the
    // grassland/forest boundary moved 800 -> 450 mm/yr, so both the steep
    // cells and the 450-800 mm precipitation rows of this map change.
    CHECK_EQ(d.h, 0x9D322E24B6F38C9Dull); // GOLDEN(biome_map)
}
