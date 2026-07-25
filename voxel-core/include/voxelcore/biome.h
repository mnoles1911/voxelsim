#pragma once
// Biome classification v1 (M4 round-1 scope, docs/m4-plan.md "How
// biome<->terrain consistency works"). Header-only, integer-only — this
// logic is mirrored bit-for-bit in voxel-core/shaders/worldgen.ush's
// ColumnMain (docs/determinism.md's CPU/GPU mirror contract). ANY change
// here must be made identically in both places and re-verified by vxc_gpu.
//
// classifyBiome() runs MORPHOLOGY GATES before the Whittaker climate table
// (m4-plan): slope beyond a cliff threshold reads as rock/alpine regardless
// of climate; a coastal band around sea level reads as ocean/beach; terrain
// above a temperature-adjusted treeline reads as alpine/tundra. Only once
// every gate has passed does climate (temperature x precipitation, with
// seasonality splitting savanna/grassland and forest types) pick among the
// remaining biomes. No floats; every threshold below is an integer worldgen
// contract constant — tune only on a deliberate kWorldGenVersion bump.

#include "voxelcore/core.h"

namespace vxc {

enum BiomeId : uint8_t {
    OCEAN = 0,
    BEACH,
    GRASSLAND,
    TEMPERATE_FOREST,
    RAINFOREST,
    DESERT,
    SAVANNA,
    TAIGA,
    TUNDRA_ALPINE,
    kBiomeCount
};

// --- morphology gate thresholds ---------------------------------------------

// Sum-of-abs-elevation-deltas-per-tile-pixel — the same "slopeMmPerPx" value
// Amplifier::column already derives for detail-amplitude scaling — beyond
// which terrain reads as a cliff face: rock/alpine regardless of climate.
inline constexpr int64_t kBiomeCliffSlopeMmPerPx = 6000;

// Coastal band around sea level (z=0, mm): below is open ocean floor, within
// (inclusive) is beach.
inline constexpr int32_t kBiomeBeachLowerMm = -3000;
inline constexpr int32_t kBiomeBeachUpperMm = 4000;

// Treeline elevation at the reference temperature (tempU8==128 — the
// synthetic-tile "average", see SyntheticTileSampler), and its per-unit
// temperature adjustment: colder-than-reference climates lower the treeline
// (down toward the coastal band at the coldest extreme), warmer climates
// raise it.
inline constexpr int32_t kBiomeTreelineBaseMm = 2'600'000;
inline constexpr int32_t kBiomeTreelineMmPerTempUnit = 20'000;

// Above this elevation, TUNDRA_ALPINE reads as bare exposed rock rather than
// permafrost ground (see biomeSurfaceMaterial's doc comment for the
// known-simplification this approximates).
inline constexpr int32_t kBiomeAlpineRockLineMm = 3'200'000;

// --- Whittaker climate bands -------------------------------------------------

inline constexpr int32_t kBiomeTempColdU8 = 70;    // < : cold band (taiga)
inline constexpr int32_t kBiomeTempWarmU8 = 140;   // >=: warm band
inline constexpr int32_t kBiomeTempHotU8 = 170;    // >=: hot band (intensifies desert/rainforest)
inline constexpr int32_t kBiomePrecipAridU8 = 60;  // < : arid
inline constexpr int32_t kBiomePrecipSemiU8 = 100; // < : semi-arid
inline constexpr int32_t kBiomePrecipModU8 = 170;  // < : moderate; >=: wet
inline constexpr int32_t kBiomeSeasonalHighU8 = 128; // >=: strongly seasonal (wet/dry cycle)

// Temperature-adjusted treeline (clamped so it never dips below the coastal
// band — the beach/ocean gate already owns anything that close to sea
// level).
constexpr int32_t biomeTreelineMm(int32_t tempU8) {
    const int64_t adjusted = int64_t(kBiomeTreelineBaseMm) +
                              (int64_t(tempU8) - 128) * int64_t(kBiomeTreelineMmPerTempUnit);
    return clampi32(adjusted, kBiomeBeachUpperMm, 9'000'000);
}

// Per-column biome classification. Gate order (m4-plan): slope -> beach/
// ocean -> temperature-adjusted treeline -> Whittaker climate table.
constexpr BiomeId classifyBiome(int32_t tempU8, int32_t precipU8, int32_t seasonalityU8,
                                 int32_t surfaceMm, int64_t slopeMmPerPx) {
    if (slopeMmPerPx > kBiomeCliffSlopeMmPerPx) return TUNDRA_ALPINE;
    if (surfaceMm < kBiomeBeachLowerMm) return OCEAN;
    if (surfaceMm <= kBiomeBeachUpperMm) return BEACH;
    if (surfaceMm > biomeTreelineMm(tempU8)) return TUNDRA_ALPINE;

    // Whittaker temperature x precipitation table (gates above already
    // consumed anything coastal, steep, or above the treeline).
    if (tempU8 < kBiomeTempColdU8) return TAIGA;

    const bool seasonal = seasonalityU8 >= kBiomeSeasonalHighU8;
    const bool warm = tempU8 >= kBiomeTempWarmU8;
    const bool hot = tempU8 >= kBiomeTempHotU8;

    if (precipU8 < kBiomePrecipAridU8) return hot ? DESERT : GRASSLAND;
    if (precipU8 < kBiomePrecipSemiU8) return (warm && seasonal) ? SAVANNA : GRASSLAND;
    if (precipU8 < kBiomePrecipModU8) return (warm && seasonal) ? SAVANNA : TEMPERATE_FOREST;
    return warm ? RAINFOREST : TEMPERATE_FOREST; // wet band
}

// Biome -> topsoil-layer surface material (append-only Material ids in
// core.h, never renumber).
constexpr MaterialId biomeSurfaceMaterial(BiomeId biome, int32_t surfaceMm) {
    switch (biome) {
        case OCEAN: return MAT_MUD;
        case BEACH: return MAT_SAND;
        case GRASSLAND: return MAT_GRASS;
        case TEMPERATE_FOREST: return MAT_TOPSOIL;
        case RAINFOREST: return MAT_JUNGLE_SOIL;
        case DESERT: return MAT_SAND;
        case SAVANNA: return MAT_SAVANNA_GRASS;
        case TAIGA: return MAT_PODZOL;
        case TUNDRA_ALPINE:
        default:
            // TUNDRA_ALPINE covers both gate paths that reach it: the
            // temperature-adjusted-treeline case (typically flat, cold
            // ground -> permafrost) and the slope-cliff case (any climate,
            // any elevation -> exposed rock). surfaceMm is the only signal
            // this function has to tell them apart, so high elevations read
            // as bare rock and everything else as permafrost — a steep LOW-
            // elevation cliff therefore reads as permafrost rather than rock
            // in round 1. Documented known simplification (m4-plan round-1
            // scope); revisit with a dedicated rock-face biome or a slope-
            // aware material picker if this matters once real diffusion
            // tiles are in.
            return surfaceMm > kBiomeAlpineRockLineMm ? MAT_ROCK : MAT_PERMAFROST;
    }
}

} // namespace vxc
