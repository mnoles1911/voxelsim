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
    virtual int32_t pixelSizeMm() const = 0; // 30000 at scale 1, 11250 at scale 8
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
        // Octaves in pixel units (lattice sizes 512/128/32 px), amplitudes in
        // metres; slight negative bias creates oceans/coastlines.
        const int64_t n0 = valueNoise2(seed_, px, py, 512, CH_SYNTH_TILE_BASE + 0);
        const int64_t n1 = valueNoise2(seed_, px, py, 128, CH_SYNTH_TILE_BASE + 1);
        const int64_t n2 = valueNoise2(seed_, px, py, 32, CH_SYNTH_TILE_BASE + 2);
        const int64_t m = (n0 * 1400 + n1 * 450 + n2 * 130) / 32768 - 120;
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
