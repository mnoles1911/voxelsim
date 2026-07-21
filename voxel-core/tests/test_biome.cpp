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
} // namespace

VXC_TEST(biome_hot_dry_is_desert) {
    CHECK_EQ(classifyBiome(200, 20, 0, kInlandMm, 0), DESERT);
}

VXC_TEST(biome_cold_is_taiga_or_tundra) {
    // Cold, but below the (temperature-lowered) treeline at this elevation:
    // Whittaker's cold band -> TAIGA.
    CHECK_EQ(classifyBiome(10, 100, 0, 100'000, 0), TAIGA);
    // Same cold climate, higher elevation crosses that lowered treeline ->
    // gated to TUNDRA_ALPINE before Whittaker ever runs.
    CHECK_EQ(classifyBiome(10, 100, 0, 500'000, 0), TUNDRA_ALPINE);
}

VXC_TEST(biome_steep_slope_overrides_climate) {
    // Warm + wet would Whittaker-pick RAINFOREST at slope 0 ...
    CHECK_EQ(classifyBiome(200, 220, 0, kInlandMm, 0), RAINFOREST);
    // ... but a cliff-steep slope gates to TUNDRA_ALPINE regardless.
    CHECK_EQ(classifyBiome(200, 220, 0, kInlandMm, 10'000), TUNDRA_ALPINE);
}

VXC_TEST(biome_wet_warm_is_rainforest) {
    CHECK_EQ(classifyBiome(200, 220, 0, kInlandMm, 0), RAINFOREST);
}

VXC_TEST(biome_seasonality_splits_savanna_from_grassland) {
    // Same warm, semi-arid climate; only seasonality differs.
    CHECK_EQ(classifyBiome(200, 80, 200, kInlandMm, 0), SAVANNA);
    CHECK_EQ(classifyBiome(200, 80, 0, kInlandMm, 0), GRASSLAND);
}

VXC_TEST(biome_seasonality_splits_forest_types) {
    // Same warm, moderate-precip climate; only seasonality differs.
    CHECK_EQ(classifyBiome(200, 150, 200, kInlandMm, 0), SAVANNA);
    CHECK_EQ(classifyBiome(200, 150, 0, kInlandMm, 0), TEMPERATE_FOREST);
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
    // (temp==128 is not "warm", precip==128 is in the moderate band).
    CHECK_EQ(classifyBiome(128, 128, 0, kInlandMm, 0), TEMPERATE_FOREST);
    CHECK_EQ(classifyBiome(128, 128, 0, kInlandMm, kBiomeCliffSlopeMmPerPx), TEMPERATE_FOREST);
    CHECK_EQ(classifyBiome(128, 128, 0, kInlandMm, kBiomeCliffSlopeMmPerPx + 1), TUNDRA_ALPINE);
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
            const int32_t seasonality = (t * 3 + p * 5) & 0xff;
            for (int64_t elevM = -20; elevM <= 40; elevM += 4) {
                const int32_t surfaceMm = static_cast<int32_t>(elevM * 100'000);
                for (int64_t slope = 0; slope <= 9000; slope += 3000) {
                    const BiomeId biome = classifyBiome(t, p, seasonality, surfaceMm, slope);
                    d.u8(static_cast<uint8_t>(biome));
                    d.u8(biomeSurfaceMaterial(biome, surfaceMm));
                }
            }
        }
    CHECK_EQ(d.h, 0xEDBF3C9217ECBBF6ull); // GOLDEN(biome_map)
}
