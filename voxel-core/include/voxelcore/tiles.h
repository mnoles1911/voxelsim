#pragma once
// Access to 30m-class terrain tiles (elevation + climate) from the terrain
// service. Tiles are canonical DATA (doctrine §2.3): generated once
// server-side by the diffusion model, cached and distributed. Everything the
// amplifier derives from them must be bit-deterministic.

#include "voxelcore/climate.h"
#include "voxelcore/core.h"
#include "voxelcore/hash.h"

namespace vxc {

// One climate reading, in the wire encoding defined by voxelcore/climate.h --
// NOT the "service-defined encoding, see terrain-service" this comment used to
// claim. That vagueness was the whole bug: the encoding is physical WorldClim
// values quantized over Earth-extreme ranges, nobody on this side knew it, and
// every threshold over these bytes was written against SyntheticTileSampler's
// unrelated one instead. Decode with climate.h's climate*FromU8.
struct ClimateSample {
    // Defaults are the MISSING-TILE answer (TileGridSampler returns a
    // default-constructed sample outside the loaded set), so they must be a
    // bland, plausible climate rather than zeros. The old 128/0/128/0 decoded
    // to +0.2 C, bio_4 0, and 6024 mm/yr -- a freezing rainforest with no
    // seasonality, which is not bland, it is one of the wettest places that has
    // ever existed. These are ~10 C, bio_4 800, 800 mm/yr, CV 40%: temperate,
    // unremarkable, and in the middle of every Whittaker band rather than at an
    // edge, so a missing tile cannot masquerade as an interesting biome.
    uint8_t temperature = static_cast<uint8_t>(climateTempU8FromDegC(10));
    uint8_t seasonality = static_cast<uint8_t>(climateSeasonalityU8From(800));
    uint8_t precipitation = static_cast<uint8_t>(climatePrecipU8FromMmPerYr(800));
    uint8_t precipVariability = static_cast<uint8_t>(climatePrecipVarU8FromDeciPct(400));
};

// Raster access in tile-pixel coordinates (pixel (0,0) covers world mm range
// [0,pixelSizeMm) on each axis; negative pixels extend the raster infinitely).
class ITileSampler {
public:
    virtual ~ITileSampler() = default;
    // 30000 at scale 1, 3750 at scale 8 (scale is a SUPERSAMPLE factor on the
    // pinned 30 m checkpoint: 30 m / 8 = 3.75 m/px). MIRROR: keep identical
    // with terrain-service tile_codec.PIXEL_SIZE_MM and tilestore.h's
    // tilePixelSizeMm(). (The old 11250 was 90 m / 8, from the superseded 90 m
    // model — wrong by 3x here.)
    virtual int32_t pixelSizeMm() const = 0;
    virtual int32_t elevationMm(int64_t px, int64_t py) = 0;
    virtual ClimateSample climate(int64_t px, int64_t py) = 0;
};

// --- SyntheticTileSampler's climate span -----------------------------------
//
// The physical range each synthetic climate channel sweeps. These are worldgen
// contract constants (they decide tile-derived bytes) -- tune only on a
// deliberate kWorldGenVersion bump.
//
// Chosen to bracket every threshold in biome.h with room on both sides, so the
// synthetic world contains all nine biomes and the GPU parity harness exercises
// every branch. They are Earth-plausible but intentionally not Earth-typical:
// this sampler's job is COVERAGE, not realism -- real terrain comes from the
// diffusion tiles.
inline constexpr int64_t kSynthTempMinMilliC = -25'000; // -25 C, well below the cold band
inline constexpr int64_t kSynthTempMaxMilliC = 35'000;  // +35 C, well above the hot band
inline constexpr int64_t kSynthPrecipMinMmPerYr = 50;   // true desert
inline constexpr int64_t kSynthPrecipMaxMmPerYr = 3'500; // true rainforest
inline constexpr int64_t kSynthSeasonalityMin = 200;    // bio_4, equable maritime
inline constexpr int64_t kSynthSeasonalityMax = 2'600;  // bio_4, strongly continental
inline constexpr int64_t kSynthPrecipVarMinDeciPct = 100;  // CV 10 %
inline constexpr int64_t kSynthPrecipVarMaxDeciPct = 1'500; // CV 150 %

// Map one valueNoise2 draw onto [lo, hi]. The draw's range is pinned to
// [-32768, 32767] at compile time by hash.h's hashToSigned16, which is what
// makes the output range exact rather than statistical.
constexpr int64_t synthRangeFromNoise(int64_t n, int64_t lo, int64_t hi) {
    return lo + ((n + 32768) * (hi - lo)) / 65536;
}

// Deterministic synthetic tiles for dev/bench/tests — a stand-in for cached
// diffusion tiles, NOT canonical world data. Continental-scale value noise;
// negative elevations produce oceans.
class SyntheticTileSampler final : public ITileSampler {
public:
    explicit SyntheticTileSampler(uint64_t seed, int32_t pixelSizeMm = 30000)
        : seed_(seed), pixelSizeMm_(pixelSizeMm) {}

    int32_t pixelSizeMm() const override { return pixelSizeMm_; }

    int32_t elevationMm(int64_t px, int64_t py) override {
        // Octaves in pixel units, amplitudes in metres; slight negative bias
        // creates oceans/coastlines. Lattices 512/128/32 px are the original
        // continental octaves (wavelengths 15.36 km / 3.84 km / 0.96 km at
        // 30 m pixels). Lattices 16/8/4/2 px (480/240/120/60 m) fill the
        // former 960 m -> 25.6 m spectral gap with a ~lambda^0.9 amplitude
        // ramp so sub-km landforms exist (terrain-realism audit, worldgen v3);
        // 2 px = 60 m is Nyquist for the 30 m raster. Climate uses +3..+6
        // (temperature, precipitation, seasonality, precip variability; the
        // last two were added at v8 -- they had been left at their defaults,
        // which made SAVANNA unreachable on synthetic tiles). +7 is free.
        const int64_t n0 = valueNoise2(seed_, px, py, 512, CH_SYNTH_TILE_BASE + 0);
        const int64_t n1 = valueNoise2(seed_, px, py, 128, CH_SYNTH_TILE_BASE + 1);
        const int64_t n2 = valueNoise2(seed_, px, py, 32, CH_SYNTH_TILE_BASE + 2);
        const int64_t n3 = valueNoise2(seed_, px, py, 16, CH_SYNTH_TILE_BASE + 8);
        const int64_t n4 = valueNoise2(seed_, px, py, 8, CH_SYNTH_TILE_BASE + 9);
        const int64_t n5 = valueNoise2(seed_, px, py, 4, CH_SYNTH_TILE_BASE + 10);
        const int64_t n6 = valueNoise2(seed_, px, py, 2, CH_SYNTH_TILE_BASE + 11);
        const int64_t m = (n0 * 1400 + n1 * 450 + n2 * 130 + n3 * 70 + n4 * 38 +
                           n5 * 20 + n6 * 11) /
                              32768 -
                          120;
        return static_cast<int32_t>(m * 1000);
    }

    // Climate in the SAME wire encoding real tiles use (voxelcore/climate.h).
    //
    // Before worldgen v8 this emitted `128 + noise/256` -- a byte centred on
    // 128 spanning the full 0..255 -- which under the real encoding decodes to
    // a median of -19 C and 5200 mm/yr: a frozen rainforest. It also left
    // seasonality and precipVariability at their defaults, so on synthetic
    // tiles the SAVANNA branch was structurally unreachable and no test or GPU
    // parity run ever exercised it.
    //
    // The ranges below are deliberately WIDER than any one real region, for
    // two reasons. They have to span every Whittaker threshold, because the
    // GPU determinism harness can only feed this sampler (gpu_harness.cpp
    // takes a concrete SyntheticTileSampler&), so if a threshold is not
    // crossed here it is never checked for CPU/GPU agreement anywhere. And a
    // dev sampler that only ever produces temperate-maritime terrain would
    // reproduce exactly the blind spot that let the biome table ship
    // mis-calibrated in the first place.
    ClimateSample climate(int64_t px, int64_t py) override {
        ClimateSample c;
        // Lattices are much tighter than the old 1024/768 (30.7/23 km). Those
        // were chosen for Earth-like climate scale, but the consequence was
        // that only ~2.5 temperature wavelengths fit across a 76.8 km window --
        // so ANY bounded sample, including the GPU harness's region, saw a
        // narrow slice of the range and left most Whittaker branches untouched.
        // 256/192/160/128 px = 7.7/5.8/4.8/3.8 km puts 10-20 periods across
        // that same window. Climate that varies over kilometres is not Earth-
        // like, and that is the correct trade for a sampler whose job is
        // coverage: real climate comes from the diffusion tiles.
        const int64_t t = valueNoise2(seed_, px, py, 256, CH_SYNTH_TILE_BASE + 3);
        const int64_t p = valueNoise2(seed_, px, py, 192, CH_SYNTH_TILE_BASE + 4);
        const int64_t s = valueNoise2(seed_, px, py, 160, CH_SYNTH_TILE_BASE + 5);
        const int64_t v = valueNoise2(seed_, px, py, 128, CH_SYNTH_TILE_BASE + 6);
        c.temperature = static_cast<uint8_t>(
            climateTempU8FromMilliC(synthRangeFromNoise(t, kSynthTempMinMilliC, kSynthTempMaxMilliC)));
        c.precipitation = static_cast<uint8_t>(climatePrecipU8FromMmPerYr(
            synthRangeFromNoise(p, kSynthPrecipMinMmPerYr, kSynthPrecipMaxMmPerYr)));
        c.seasonality = static_cast<uint8_t>(climateSeasonalityU8From(
            synthRangeFromNoise(s, kSynthSeasonalityMin, kSynthSeasonalityMax)));
        c.precipVariability = static_cast<uint8_t>(climatePrecipVarU8FromDeciPct(
            synthRangeFromNoise(v, kSynthPrecipVarMinDeciPct, kSynthPrecipVarMaxDeciPct)));
        return c;
    }

private:
    uint64_t seed_;
    int32_t pixelSizeMm_;
};

} // namespace vxc
