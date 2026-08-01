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
// PRECIPITATION seasonality -- bio_15, not bio_4; see kBiomePrecipSeasonalHighU8
// -- splitting savanna off grassland and the forest types) pick among the
// remaining biomes. No floats; every threshold below is an integer worldgen
// contract constant — tune only on a deliberate kWorldGenVersion bump.

#include "voxelcore/climate.h"
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
    // Appended at worldgen v8. Before it, the cliff gate returned
    // TUNDRA_ALPINE and biomeSurfaceMaterial split rock from permafrost on
    // elevation alone -- so a steep LOW cliff read as permafrost. That was an
    // acceptable simplification while the gate fired at ~11 degrees and so
    // mostly caught high ground; with the gate corrected to ~35 degrees it
    // would have put permafrost on every temperate sea cliff. Append-only, so
    // no existing id is renumbered.
    BARE_ROCK,
    kBiomeCount
};

// --- morphology gate thresholds ---------------------------------------------

// Sum-of-abs-elevation-deltas-per-tile-pixel — the same "slopeMmPerPx" value
// Amplifier::column already derives for detail-amplitude scaling — beyond
// which terrain reads as a cliff face: bare rock regardless of climate.
//
// Stated as a GRADE so it means something. 70% over a tile pixel is about 35
// degrees, the angle of repose for soil and scree (33-37): above it loose
// material does not stay put, which is precisely the physical reason a cliff
// is bare. The old flat 6000 was ~20% grade, about 11 degrees -- a hillside,
// not a cliff -- and it sat BELOW the median slope of real 30 m tiles (7000),
// so it claimed 51% of the world and painted half the map alpine.
//
// FIXED IN v9. This constant used to be stated in mm per tile PIXEL, which is
// proportional to pixelSizeMm, so it meant a different grade at scale 8
// (3.75 m/px) than at scale 1. The note here used to say the honest fix was to
// do this together with slopeScaleQ10/microScaleQ10 in amplifier.cpp, which
// carried the identical latent bug, and to do all of them before generating
// scale-8 tiles. That is exactly what v9 did: the currency everywhere is now
// MM PER METRE, taken from the carrier's analytic gradient
// (Amplifier::evalSurface), so every threshold means a grade at every scale.
//
// The value is unchanged in meaning: 70% grade is ~35 degrees, the angle of
// repose for soil and scree (33-37), above which loose material does not stay
// put -- which is the physical reason a cliff is bare.
inline constexpr int64_t kBiomeCliffGradePercent = 70; // ~35 degrees
// mm of rise per metre of run: 70% grade == 700 mm/m.
inline constexpr int64_t kBiomeCliffSlopeMmPerM = kBiomeCliffGradePercent * 1000 / 100;

// Coastal band around sea level (z=0, mm): below is open ocean floor, within
// (inclusive) is beach.
inline constexpr int32_t kBiomeBeachLowerMm = -3000;
inline constexpr int32_t kBiomeBeachUpperMm = 4000;

// Treeline elevation at 0 degrees C mean annual temperature, and how far it
// moves per degree.
//
// The reference point is climateTempU8FromDegC(0) == 128. That is the same
// literal as before, but for a real reason now: u8 128 decodes to +0.16 C.
// The old comment called it "the synthetic-tile average", which was true of
// the sampler and irrelevant to the physics.
//
// kBiomeTreelineBaseMm is TUNED, NOT DERIVED -- the only such constant in this
// file, called out so nobody mistakes it for physics. 2.6 km at 0 C (the v6
// value) does not exist anywhere on Earth; the real figure is at or near the
// ground in northern Fennoscandia. But sweeping it against the real 25-tile
// set showed the alpine share barely responds (48.6% of land at 300 m, 39.9%
// at 900 m, 35.0% at 1200 m) because that region is genuinely cold and
// mountainous, so it is settled by eye, not by derivation. 900 m is the
// starting value; it is the constant to move if the world reads too bare.
//
// The per-degree rate IS derived: the environmental lapse rate is ~6.5 C/km,
// so ~154 m of elevation per degree of mean annual temperature. Do not tune
// this one to compensate for the base -- keep the derivable thing derivable.
inline constexpr int32_t kBiomeTreelineBaseMm = 900'000;    // 900 m at 0 C -- TUNED
inline constexpr int64_t kBiomeTreelineMmPerDegC = 150'000; // ~6.5 C/km lapse rate
//: The same rate expressed per u8 temperature step, which is what the
//: classifier actually indexes by. Derived, so a change to the temperature
//: quantization range carries into the treeline automatically.
inline constexpr int32_t kBiomeTreelineMmPerTempUnit = static_cast<int32_t>(
    kBiomeTreelineMmPerDegC * (kClimateTempMaxMilliC - kClimateTempMinMilliC) / (255 * 1000));

// Above this elevation, TUNDRA_ALPINE reads as bare exposed rock rather than
// permafrost ground (see biomeSurfaceMaterial's doc comment for the
// known-simplification this approximates).
inline constexpr int32_t kBiomeAlpineRockLineMm = 3'200'000;

// --- Whittaker climate bands -------------------------------------------------

// Every band is stated in PHYSICAL units and converted at compile time, so the
// constant says what it means and follows automatically if the wire
// quantization ever moves. The v6 values were bare u8 literals carried over
// from SyntheticTileSampler's encoding, and what they actually decoded to on a
// real tile is written beside each one -- kBiomePrecipAridU8 = 60 meant
// "drier than 2824 mm/yr", i.e. drier than the Amazon, so every column on
// Earth took the arid branch and the three below it were dead code.
inline constexpr int32_t kBiomeTempColdU8 = climateTempU8FromDegC(5);   // was 70 = -18 C
inline constexpr int32_t kBiomeTempWarmU8 = climateTempU8FromDegC(18);  // was 140 = +3.9 C
// 24 C WAS RE-EXAMINED AT v22 AND DELIBERATELY NOT MOVED. The v21 biome census
// off the coarse model showed DESERT at exactly 0.00% of land, and the cause is
// this gate: 16.7% of that world's land is arid (bio_12 < 400) but its land
// temperature p95 is 20.7 C, so `arid && hot` is empty and every arid column
// falls through to GRASSLAND. It is tempting to lower the threshold until
// deserts appear. Do not: measured against the WorldClim rasters, 24 C puts
// DESERT at 9.50% of Earth land, which is already the right answer (Earth is
// ~8.6%), and every lower value overshoots hard -- 22 C gives 13.0%, 20 C gives
// 16.1%, 18 C gives 18.2%, at which point half the world's dry TEMPERATE land
// is sand. The gate is correct and the WORLD is wrong; the blocker is the
// coarse model's compressed climate tails, not this constant. See
// docs/measurements/biome-gates-2026-08-01.txt.
inline constexpr int32_t kBiomeTempHotU8 = climateTempU8FromDegC(24);   // was 170 = +13.3 C
inline constexpr int32_t kBiomePrecipAridU8 = climatePrecipU8FromMmPerYr(400);  // was 60 = 2824 mm
inline constexpr int32_t kBiomePrecipSemiU8 = climatePrecipU8FromMmPerYr(800);  // was 100 = 4706 mm
inline constexpr int32_t kBiomePrecipModU8 = climatePrecipU8FromMmPerYr(1600);  // was 170 = 8000 mm

// THE SAVANNA GATE READS PRECIPITATION SEASONALITY (bio_15), NOT TEMPERATURE
// SEASONALITY (bio_4). Changed at worldgen v22; the whole reason is worth
// keeping, because the old gate was not merely mistuned, it was UNSATISFIABLE.
//
// v8..v21 asked for `tempU8 >= kBiomeTempWarmU8 && seasonalityU8 >=
// climateSeasonalityU8From(1500)` -- bio_1 >= 18 C together with bio_4 >= 1500
// (sd of monthly temperature >= 15 C). Those two conditions are mutually
// exclusive ON EARTH, and close to mutually exclusive by definition: the warm
// band IS the tropics, and what makes a place tropical is that its temperature
// barely varies through the year. Measured over the WorldClim 2.1 10' rasters
// inside the +/-60 deg crop the conditioning stats use (docs/measurements/
// biome-gates-2026-08-01.txt): the maximum bio_4 anywhere with bio_1 >= 18 C is
// 1084, against a threshold of 1500. ZERO pixels of Earth land satisfy the
// gate, so SAVANNA was dead code for fourteen worldgen versions and every arid
// warm column fell through to GRASSLAND.
//
// No value of a bio_4 threshold fixes it. Sweeping it down to where it admits a
// plausible share (bio_4 >= 200 gives 12.5% of land) puts the savanna in the
// WRONG PLACES: only 77.0% of what it selects lies inside |lat| < 25, against
// 87.2% for the eligible window itself, i.e. the rule is actively ANTI-
// selective for the tropics. On named sites it calls Houston, Brisbane, Miami,
// Durban and Seville savanna while rejecting the Serengeti, the Cerrado and
// Tsavo. That is the signature of the wrong variable, not a wrong number.
//
// What actually distinguishes savanna from both rainforest and temperate
// woodland is a DRY SEASON -- a precipitation phenomenon. bio_15, the
// coefficient of variation of monthly precipitation, is already in the wire
// format (climate.h, ClimateSample::precipVariability) and separates them
// cleanly: at CV >= 70% every in-window negative control above is rejected and
// 8 of 9 savanna sites accepted (the Serengeti, at 65, is the one miss), and
// 93.4% of the selected area lies inside |lat| < 25.
//
// THE VALUE IS DERIVED TWICE AND BOTH ROUTES AGREE.
//   * Physically: a year with d months of no rain and the rest uniform has
//     CV = sqrt(d / (12 - d)). Four dry months -- a third of the year, the
//     usual marked-dry-season line for tropical savanna -- gives
//     sqrt(4/8) = 70.7%.
//   * Empirically: it puts SAVANNA at 15.57% of Earth land, against the ~15.6%
//     that tropical/subtropical grassland-savanna-shrubland actually covers.
// The neighbouring dry-season lengths bracket it and are both worse: d=3
// (CV 57.7%) reaches 18.75% and starts admitting Mediterranean Seville, d=6
// (CV 100%) falls to 8.32% and drops the Cerrado.
//
// Stated in TENTHS OF A PERCENT because that is climate.h's bio_15 unit (see
// its note on why whole percent breaks the encode/decode round trip).
inline constexpr int32_t kBiomePrecipSeasonalHighU8 = climatePrecipVarU8FromDeciPct(700);

static_assert(kBiomePrecipSeasonalHighU8 == 89,
              "CV 70% must quantize to u8 89; a change here changes every savanna boundary");
static_assert(kBiomeTempColdU8 < kBiomeTempWarmU8 && kBiomeTempWarmU8 < kBiomeTempHotU8);
static_assert(kBiomePrecipAridU8 < kBiomePrecipSemiU8 && kBiomePrecipSemiU8 < kBiomePrecipModU8);

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
//
// THE THIRD ARGUMENT CHANGED CHANNEL AT v22: it was seasonalityU8 (bio_4,
// temperature seasonality) and is now precipVarU8 (bio_15, precipitation
// seasonality). Same arity, so a caller that was not updated still COMPILES --
// there are only three outside the tests (Amplifier::column, worldgen.ush's
// ColumnMain, climateprobe.cpp) and all three moved in the same commit, but
// check the argument, not the call count, if this is ever revisited.
constexpr BiomeId classifyBiome(int32_t tempU8, int32_t precipU8, int32_t precipVarU8,
                                 int32_t surfaceMm, int64_t slopeMmPerM) {
    // SEA LEVEL FIRST. Until v8 the slope gate ran ahead of these, so steep
    // SEAFLOOR classified as TUNDRA_ALPINE: measured over the real 25-tile set,
    // 39.8% of everything below sea level -- 17.8% of the whole world -- was
    // submarine terrain wearing alpine permafrost. Nothing underwater can be
    // alpine, and no threshold value fixes an ordering bug.
    if (surfaceMm < kBiomeBeachLowerMm) return OCEAN;
    if (surfaceMm <= kBiomeBeachUpperMm) return BEACH;
    // Steep ground inside the beach band now reads BEACH rather than rock.
    // That is correct: the band is 7 m tall, so it is the foot of a sea cliff.
    if (slopeMmPerM > kBiomeCliffSlopeMmPerM) return BARE_ROCK;
    if (surfaceMm > biomeTreelineMm(tempU8)) return TUNDRA_ALPINE;

    // Whittaker temperature x precipitation table (gates above already
    // consumed anything coastal, steep, or above the treeline).
    if (tempU8 < kBiomeTempColdU8) return TAIGA;

    const bool seasonal = precipVarU8 >= kBiomePrecipSeasonalHighU8;
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
        case BARE_ROCK: return MAT_ROCK;
        case TUNDRA_ALPINE:
        default:
            // Since v8 only ONE gate reaches TUNDRA_ALPINE -- the
            // temperature-adjusted treeline -- so this is unambiguously cold
            // high ground, and the elevation split is just permafrost below
            // the rock line and exposed rock above it.
            //
            // The v6 note that a steep LOW cliff read as permafrost no longer
            // applies: the cliff gate returns BARE_ROCK now, which is exactly
            // the "dedicated rock-face biome" that note asked for.
            return surfaceMm > kBiomeAlpineRockLineMm ? MAT_ROCK : MAT_PERMAFROST;
    }
}

} // namespace vxc
