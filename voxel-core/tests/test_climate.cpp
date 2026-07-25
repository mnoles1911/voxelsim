// The tile climate wire encoding (voxelcore/climate.h): physical <-> u8.
//
// climate.h carries most of its own proof as static_asserts, so these tests
// cover what a static_assert cannot: the full-domain sweep behind
// GOLDEN(climate_encoding), and the properties that must hold for EVERY value
// rather than the handful worth asserting at compile time.

#include "voxelcore/climate.h"
#include "vxctest.h"

using namespace vxc;

VXC_TEST(climate_encode_is_monotone_and_onto) {
    // Monotone: a warmer temperature never encodes to a colder byte. This is
    // the property that makes a physically-stated threshold comparable against
    // a stored byte at all -- without it, `precipU8 < aridThreshold` would not
    // mean "drier than 400 mm/yr".
    int32_t prev = -1;
    for (int64_t mC = kClimateTempMinMilliC; mC <= kClimateTempMaxMilliC; mC += 100) {
        const int32_t u = climateTempU8FromMilliC(mC);
        CHECK(u >= prev);
        prev = u;
    }
    CHECK_EQ(prev, 255);

    prev = -1;
    for (int64_t mm = kClimatePrecipMinMmPerYr; mm <= kClimatePrecipMaxMmPerYr; mm += 10) {
        const int32_t u = climatePrecipU8FromMmPerYr(mm);
        CHECK(u >= prev);
        prev = u;
    }
    CHECK_EQ(prev, 255);

    // Onto: every one of the 256 codes is reachable from some physical value,
    // i.e. the encoding wastes no code points.
    bool seen[256] = {false};
    for (int64_t mm = kClimatePrecipMinMmPerYr; mm <= kClimatePrecipMaxMmPerYr; ++mm)
        seen[climatePrecipU8FromMmPerYr(mm)] = true;
    for (int i = 0; i < 256; ++i) CHECK(seen[i]);
}

VXC_TEST(climate_roundtrip_is_stable_for_every_code) {
    // decode-then-encode is the identity on all 256 codes, for all four
    // channels. This is the direction that matters: a threshold derived from a
    // physical constant must compare correctly against a byte that came off a
    // tile, and that only holds if no code decodes into another code's bucket.
    for (int64_t u = 0; u <= 255; ++u) {
        CHECK_EQ(climateTempU8FromMilliC(climateTempMilliCFromU8(u)), u);
        CHECK_EQ(climateSeasonalityU8From(climateSeasonalityFromU8(u)), u);
        CHECK_EQ(climatePrecipU8FromMmPerYr(climatePrecipMmPerYrFromU8(u)), u);
        CHECK_EQ(climatePrecipVarU8FromDeciPct(climatePrecipVarDeciPctFromU8(u)), u);
    }
}

VXC_TEST(climate_decode_stays_in_physical_range) {
    for (int64_t u = 0; u <= 255; ++u) {
        CHECK(climateTempMilliCFromU8(u) >= kClimateTempMinMilliC);
        CHECK(climateTempMilliCFromU8(u) <= kClimateTempMaxMilliC);
        CHECK(climatePrecipMmPerYrFromU8(u) >= kClimatePrecipMinMmPerYr);
        CHECK(climatePrecipMmPerYrFromU8(u) <= kClimatePrecipMaxMmPerYr);
        CHECK(climateSeasonalityFromU8(u) >= kClimateSeasonalityMin);
        CHECK(climateSeasonalityFromU8(u) <= kClimateSeasonalityMax);
        CHECK(climatePrecipVarDeciPctFromU8(u) >= kClimatePrecipVarMinDeciPct);
        CHECK(climatePrecipVarDeciPctFromU8(u) <= kClimatePrecipVarMaxDeciPct);
    }
}

VXC_TEST(climate_known_physical_anchors) {
    // Spot values a human can check against a thermometer and a rain gauge,
    // so a range edit that compiles is still caught by a reader.
    CHECK_EQ(climateTempU8FromDegC(0), 128);   // freezing sits mid-byte
    CHECK_EQ(climateTempU8FromDegC(-40), 0);
    CHECK_EQ(climateTempU8FromDegC(40), 255);
    CHECK_EQ(climatePrecipU8FromMmPerYr(0), 0);
    CHECK_EQ(climatePrecipU8FromMmPerYr(6000), 128);

    // The measured real-tile window (25 tiles, seed 20260719): precipitation
    // spans u8 13..35, i.e. 659..1647 mm/yr. Pinned here because it is the
    // evidence behind biome.h's thresholds being stated physically -- a
    // quantization range that made this window move would invalidate that
    // reasoning silently.
    CHECK_EQ(climatePrecipU8FromMmPerYr(659), 14);
    CHECK_EQ(climatePrecipU8FromMmPerYr(1647), 35);
    CHECK_EQ(climatePrecipMmPerYrFromU8(13), 611);
    CHECK_EQ(climatePrecipMmPerYrFromU8(35), 1647);
}

VXC_TEST(climate_encoding_golden_digest) {
    // Full-domain sweep of all four encoders and decoders. Version-FREE: this
    // pins the wire contract, not worldgen output, so it must NOT move on a
    // kWorldGenVersion bump. It moves only if a quantization range or the
    // rounding changes -- which is exactly the event that changes what every
    // physically-stated threshold in biome.h compiles to.
    Digest d;
    for (int64_t u = 0; u <= 255; ++u) {
        d.i64(climateTempMilliCFromU8(u));
        d.i64(climateSeasonalityFromU8(u));
        d.i64(climatePrecipMmPerYrFromU8(u));
        d.i64(climatePrecipVarDeciPctFromU8(u));
    }
    for (int64_t mC = kClimateTempMinMilliC; mC <= kClimateTempMaxMilliC; mC += 250)
        d.u8(static_cast<uint8_t>(climateTempU8FromMilliC(mC)));
    for (int64_t b4 = kClimateSeasonalityMin; b4 <= kClimateSeasonalityMax; b4 += 10)
        d.u8(static_cast<uint8_t>(climateSeasonalityU8From(b4)));
    for (int64_t mm = kClimatePrecipMinMmPerYr; mm <= kClimatePrecipMaxMmPerYr; mm += 25)
        d.u8(static_cast<uint8_t>(climatePrecipU8FromMmPerYr(mm)));
    for (int64_t p = kClimatePrecipVarMinDeciPct; p <= kClimatePrecipVarMaxDeciPct; p += 5)
        d.u8(static_cast<uint8_t>(climatePrecipVarU8FromDeciPct(p)));
    CHECK_EQ(d.h, 0x54CD35E91A7C3AFFull); // GOLDEN(climate_encoding)
}
