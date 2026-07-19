#pragma once
// Worldgen hash v1 — THE procedural randomness primitive (docs/determinism.md).
// Any change here is world-breaking: bump kWorldGenVersion, regenerate goldens.

#include "voxelcore/core.h"

namespace vxc {

constexpr uint64_t splitmix64(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Channel ids domain-separate hash uses. Append only — never renumber.
enum HashChannel : uint32_t {
    CH_DETAIL_OCTAVE_BASE = 0, // +octave index, reserve 0..15
    CH_TOPSOIL_JITTER = 16,
    CH_BEDROCK_JITTER = 17,
    CH_SYNTH_TILE_BASE = 32, // synthetic dev tiles, reserve 32..47
};

constexpr uint64_t hash2(uint64_t seed, int64_t x, int64_t y, uint32_t channel) {
    return splitmix64(seed ^ splitmix64(static_cast<uint64_t>(x) ^
                              splitmix64(static_cast<uint64_t>(y) ^
                              splitmix64(channel))));
}

constexpr uint64_t hash3(uint64_t seed, int64_t x, int64_t y, int64_t z, uint32_t channel) {
    return splitmix64(seed ^ splitmix64(static_cast<uint64_t>(x) ^
                              splitmix64(static_cast<uint64_t>(y) ^
                              splitmix64(static_cast<uint64_t>(z) ^
                              splitmix64(channel)))));
}

// Top 16 bits -> [-32768, 32767].
constexpr int32_t hashToSigned16(uint64_t h) {
    return static_cast<int32_t>(static_cast<uint32_t>(h >> 48)) - 32768;
}

// Integer bilinear value noise on a latticeMm grid, output in [-32768, 32767]
// scaled units (caller applies amplitude). Exact 64-bit integer math.
constexpr int64_t valueNoise2(uint64_t seed, int64_t xMm, int64_t yMm,
                              int64_t latticeMm, uint32_t channel) {
    const int64_t x0 = floorDiv(xMm, latticeMm), y0 = floorDiv(yMm, latticeMm);
    const int64_t fx = xMm - x0 * latticeMm, fy = yMm - y0 * latticeMm;
    const int64_t v00 = hashToSigned16(hash2(seed, x0, y0, channel));
    const int64_t v10 = hashToSigned16(hash2(seed, x0 + 1, y0, channel));
    const int64_t v01 = hashToSigned16(hash2(seed, x0, y0 + 1, channel));
    const int64_t v11 = hashToSigned16(hash2(seed, x0 + 1, y0 + 1, channel));
    const int64_t gx = latticeMm - fx, gy = latticeMm - fy;
    // |v|<=32768, fx<lattice; lattice <= ~1<<20 mm keeps this within int64.
    return ((v00 * gx + v10 * fx) * gy + (v01 * gx + v11 * fx) * fy) /
           (latticeMm * latticeMm);
}

} // namespace vxc
