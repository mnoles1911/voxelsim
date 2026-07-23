#pragma once
// Access to 30m-class terrain tiles (elevation + climate) from the terrain
// service. Tiles are canonical DATA (doctrine §2.3): generated once
// server-side by the diffusion model, cached and distributed. Everything the
// amplifier derives from them must be bit-deterministic.

#include "voxelcore/core.h"
#include "voxelcore/hash.h"

namespace vxc {

struct ClimateSample {
    uint8_t temperature = 128;   // service-defined encoding, see terrain-service
    uint8_t seasonality = 0;
    uint8_t precipitation = 128;
    uint8_t precipVariability = 0;
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
        // 2 px = 60 m is Nyquist for the 30 m raster. Channels +5..+7 are
        // left free for future climate channels (+3/+4 in use below).
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

    ClimateSample climate(int64_t px, int64_t py) override {
        ClimateSample c;
        const int64_t t = valueNoise2(seed_, px, py, 1024, CH_SYNTH_TILE_BASE + 3);
        const int64_t p = valueNoise2(seed_, px, py, 768, CH_SYNTH_TILE_BASE + 4);
        c.temperature = static_cast<uint8_t>(clampi64(128 + t / 256, 0, 255));
        c.precipitation = static_cast<uint8_t>(clampi64(128 + p / 256, 0, 255));
        return c;
    }

private:
    uint64_t seed_;
    int32_t pixelSizeMm_;
};

} // namespace vxc
