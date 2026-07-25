#pragma once
// The tile CLIMATE wire encoding, in physical units.
//
// WHY THIS FILE EXISTS. tiles.h documents ClimateSample's fields as a
// "service-defined encoding, see terrain-service" -- i.e. the consumer did not
// know the encoding at all. So every threshold that reads those bytes was
// necessarily written against whatever distribution the author happened to be
// looking at, and the author was looking at SyntheticTileSampler (noise centred
// on 128, spanning the full byte) while the shipping tiles carry physical
// WorldClim values quantized over Earth-extreme ranges. Two encodings, one
// wire format, no tag, and thresholds silently calibrated for the wrong one:
// kBiomePrecipAridU8 = 60 decodes to 2824 mm/yr, above every value any real
// tile has ever contained, so the whole world classified into one Whittaker
// branch.
//
// The fix is not better numbers, it is naming the units. A threshold written
// as climatePrecipU8FromMmPerYr(400) says what it means, survives a re-roll of
// the world, and follows automatically if the quantization range ever moves.
//
// MIRROR CONTRACT -- the second copy of these four ranges is
// terrain-service/terrain_service/providers/diffusion.py's EXPECTED_CHANNELS.
// There they serve double duty as BOTH the model-output validation range AND
// the quantization range adapt_raster_to_tile maps into uint8, which is what
// makes them the wire format: changing either copy changes tile BYTES and must
// roll the provider_id. terrain-service/tests/test_climate_contract.py fails if
// the two disagree, and GOLDEN(climate_encoding) in test_climate.cpp pins the
// rounding that the range comparison alone would miss.
//
// UNITS are chosen so every range endpoint is an exact integer -- that is what
// lets this header satisfy the float ban (no float/double token anywhere in
// voxel-core/include or /src; CI greps for it) without carrying a scale factor
// that would need a decimal point.

#include "voxelcore/core.h"

namespace vxc {

// --- the wire ranges -------------------------------------------------------
//
// Each pair is [min, max] of the PHYSICAL quantity that u8 0 and u8 255 stand
// for. WorldClim variable in brackets.

//: bio_1, annual mean temperature, in MILLI-degrees Celsius.
inline constexpr int64_t kClimateTempMinMilliC = -40'000; // -40 C
inline constexpr int64_t kClimateTempMaxMilliC = 40'000;  // +40 C

//: bio_4, temperature seasonality, in its own native unit -- sd(monthly) x 100,
//: i.e. hundredths of a degree Celsius.
inline constexpr int64_t kClimateSeasonalityMin = 0;
inline constexpr int64_t kClimateSeasonalityMax = 3'000;

//: bio_12, annual precipitation, in mm/year.
inline constexpr int64_t kClimatePrecipMinMmPerYr = 0;
inline constexpr int64_t kClimatePrecipMaxMmPerYr = 12'000;

//: bio_15, precipitation seasonality, a coefficient of variation, in TENTHS OF
//: A PERCENT. The physical span is 0..200 % -- same as diffusion.py's -- but
//: expressed in whole percent it would have only 201 distinct values against
//: 256 codes, so decode-then-encode would not be the identity and a threshold
//: stated physically would not compare reliably against a stored byte. Tenths
//: give 2001 values over 256 codes, which restores the invariant asserted
//: below. Same trick as milli-degrees for temperature.
inline constexpr int64_t kClimatePrecipVarMinDeciPct = 0;
inline constexpr int64_t kClimatePrecipVarMaxDeciPct = 2'000; // 200.0 %

// --- encode: physical -> u8 ------------------------------------------------
//
// Round-half-up, then clamp. Intended for COMPILE-TIME threshold derivation
// (see biome.h), where the input is a constant we chose, so the clamp is a
// backstop against a typo rather than an expected path.
//
// NOTE ON ROUNDING vs terrain-service: adapt_raster_to_tile uses numpy's rint,
// which is round-half-to-EVEN on float64; this is round-half-UP on integers.
// They differ only at exact half-steps. That is deliberate and harmless,
// because these functions NEVER re-derive tile bytes -- Python already produced
// those. They convert our own thresholds and define our own synthetic
// emission. test_climate_contract.py asserts the RANGES match exactly and
// allows the two encoders to differ by at most one u8 step.

constexpr int32_t climateU8FromPhysical(int64_t v, int64_t lo, int64_t hi) {
    // Span is a positive compile-time constant for all four channels, so the
    // division is never mixed-sign; a negative numerator (v below lo) still
    // truncates toward zero and is then clamped to 0, which is the right
    // answer either way.
    const int64_t span = hi - lo;
    return clampi32(((v - lo) * 255 + span / 2) / span, 0, 255);
}

constexpr int32_t climateTempU8FromMilliC(int64_t milliC) {
    return climateU8FromPhysical(milliC, kClimateTempMinMilliC, kClimateTempMaxMilliC);
}
constexpr int32_t climateTempU8FromDegC(int64_t degC) {
    return climateTempU8FromMilliC(degC * 1000);
}
constexpr int32_t climateSeasonalityU8From(int64_t bio4) {
    return climateU8FromPhysical(bio4, kClimateSeasonalityMin, kClimateSeasonalityMax);
}
constexpr int32_t climatePrecipU8FromMmPerYr(int64_t mm) {
    return climateU8FromPhysical(mm, kClimatePrecipMinMmPerYr, kClimatePrecipMaxMmPerYr);
}
constexpr int32_t climatePrecipVarU8FromDeciPct(int64_t deciPct) {
    return climateU8FromPhysical(deciPct, kClimatePrecipVarMinDeciPct,
                                 kClimatePrecipVarMaxDeciPct);
}

// --- decode: u8 -> physical ------------------------------------------------
//
// Truncating, not rounding: these run per column (the topsoil formula,
// rivernet's flow weight), and a half-step of precipitation is far below the
// resolution the quantization already imposed -- one u8 step of bio_12 is
// 47 mm/yr. Truncation keeps the arithmetic to one integer divide.
//
// MIRRORED IN HLSL: worldgen.ush's ColumnMain uses the precipitation decoder in
// the topsoil formula. Use truncDiv there -- the shader-UB lint rejects a bare
// signed `/` on principle even where, as here, both operands are provably
// non-negative.

constexpr int64_t climatePhysicalFromU8(int64_t u8, int64_t lo, int64_t hi) {
    return lo + (u8 * (hi - lo)) / 255;
}

constexpr int64_t climateTempMilliCFromU8(int64_t u8) {
    return climatePhysicalFromU8(u8, kClimateTempMinMilliC, kClimateTempMaxMilliC);
}
constexpr int64_t climateSeasonalityFromU8(int64_t u8) {
    return climatePhysicalFromU8(u8, kClimateSeasonalityMin, kClimateSeasonalityMax);
}
constexpr int64_t climatePrecipMmPerYrFromU8(int64_t u8) {
    return climatePhysicalFromU8(u8, kClimatePrecipMinMmPerYr, kClimatePrecipMaxMmPerYr);
}
constexpr int64_t climatePrecipVarDeciPctFromU8(int64_t u8) {
    return climatePhysicalFromU8(u8, kClimatePrecipVarMinDeciPct, kClimatePrecipVarMaxDeciPct);
}

// --- the properties the rest of the codebase relies on ---------------------

// EVERY channel's physical span must have at least as many representable
// values as there are codes. Otherwise several codes decode to one physical
// value, decode-then-encode stops being the identity, and a physically-stated
// threshold no longer compares reliably against a stored byte -- the exact
// failure this header exists to prevent, reintroduced one layer down.
//
// This is not hypothetical: precipVariability was first written in whole
// percent (0..200, i.e. 201 values against 256 codes) and test_climate's
// round-trip caught it. Hence tenths of a percent. A future channel added in
// too coarse a unit breaks the build here instead of shipping.
static_assert(kClimateTempMaxMilliC - kClimateTempMinMilliC >= 255);
static_assert(kClimateSeasonalityMax - kClimateSeasonalityMin >= 255);
static_assert(kClimatePrecipMaxMmPerYr - kClimatePrecipMinMmPerYr >= 255);
static_assert(kClimatePrecipVarMaxDeciPct - kClimatePrecipVarMinDeciPct >= 255);

static_assert(climateTempU8FromMilliC(kClimateTempMinMilliC) == 0);
static_assert(climateTempU8FromMilliC(kClimateTempMaxMilliC) == 255);
static_assert(climatePrecipU8FromMmPerYr(kClimatePrecipMinMmPerYr) == 0);
static_assert(climatePrecipU8FromMmPerYr(kClimatePrecipMaxMmPerYr) == 255);
static_assert(climateSeasonalityU8From(kClimateSeasonalityMax) == 255);
static_assert(climatePrecipVarU8FromDeciPct(kClimatePrecipVarMaxDeciPct) == 255);

// Out-of-range inputs clamp rather than wrap. The temperature case is the one
// that matters: its physical minimum is negative, so the numerator goes
// negative and a missing clamp would produce a large positive u8.
static_assert(climateTempU8FromMilliC(-100'000) == 0);
static_assert(climateTempU8FromMilliC(100'000) == 255);

// The two directions agree to within one u8 step everywhere -- this is what
// lets a threshold be stated physically and compared against a stored byte.
static_assert(climateTempU8FromMilliC(climateTempMilliCFromU8(128)) == 128);
static_assert(climatePrecipU8FromMmPerYr(climatePrecipMmPerYrFromU8(27)) == 27);
static_assert(climateSeasonalityU8From(climateSeasonalityFromU8(91)) == 91);

// The anchor biome.h's treeline is referenced to. u8 128 is +0.16 C, near
// enough to freezing to be a physically meaningful zero point -- which it has
// always been, though biome.h described it as "the synthetic-tile average".
static_assert(climateTempMilliCFromU8(128) == 156);

} // namespace vxc
