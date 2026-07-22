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

// --- quintic fade, and why the detail octaves need one ----------------------
//
// valueNoise2 above interpolates its lattice BILINEARLY, so its gradient is
// discontinuous across every lattice line. That is the classic value-noise
// blocking artifact, and at 10 cm voxels it is plainly visible: a hillshade of
// the amplified surface shows dead-straight creases on the 25.6 m octave grid
// and rectangular blocking on the finer ones. It reads as a "hard step" — it
// was, in fact, the artifact that got this problem reported.
//
// The standard cure, and the one terrain-diffusion's own Minecraft integration
// gets for free by using Perlin gradient noise, is to fade the lattice
// fraction through Perlin's quintic  6t^5 - 15t^4 + 10t^3  before
// interpolating. Its first and second derivatives vanish at t = 0 and t = 1,
// so neighbouring cells meet with matching slope and curvature and the grid
// disappears.
//
// WHY THIS COSTS THE SURFACE BOUND NOTHING. fade maps [0, 1] onto [0, 1]
// monotonically, so the interpolation stays a CONVEX COMBINATION of the same
// four corner hashes. The output range is therefore still exactly
// [-32768, 32767] — the property amplifier.cpp's coupling (1) pins and the
// whole detail allowance is derived from. No bound moves because of this.
//
// EXACT INTEGER FORM. t is carried as a 10-bit fraction (tq in [0, 1024]) so
// that the quintic's t^5 term stays inside int64: the numerator is bounded by
// 15 * 1024^5 ~ 1.7e16 against int64's 9.2e18. The result is returned in the
// SAME units as fx, so the bilinear form it feeds is unchanged. Truncating
// division preserves monotonicity, so the faded fraction is still
// nondecreasing in fx and still lands in [0, latticeMm].
//
// fx is always in [0, latticeMm) — floorDiv guarantees it — so every division
// here has a non-negative numerator and truncation matches the HLSL mirror's
// truncDiv without needing the floorDiv correction.
constexpr int64_t fadeFractionMm(int64_t fx, int64_t latticeMm) {
    const int64_t tq = fx * 1024 / latticeMm; // [0, 1024)
    const int64_t t3 = tq * tq * tq;          // < 2^30
    const int64_t w = (6 * t3 * tq * tq - 15 * t3 * tq * 1024 + 10 * t3 * 1024 * 1024) /
                      (1024LL * 1024 * 1024 * 1024); // [0, 1024]
    return w * latticeMm / 1024;
}

// valueNoise2 with the quintic fade applied to both axes. Same lattice, same
// hashes, same output range — only the interpolation weights differ.
constexpr int64_t valueNoise2Fade(uint64_t seed, int64_t xMm, int64_t yMm,
                                  int64_t latticeMm, uint32_t channel) {
    const int64_t x0 = floorDiv(xMm, latticeMm), y0 = floorDiv(yMm, latticeMm);
    const int64_t fx = fadeFractionMm(xMm - x0 * latticeMm, latticeMm);
    const int64_t fy = fadeFractionMm(yMm - y0 * latticeMm, latticeMm);
    const int64_t v00 = hashToSigned16(hash2(seed, x0, y0, channel));
    const int64_t v10 = hashToSigned16(hash2(seed, x0 + 1, y0, channel));
    const int64_t v01 = hashToSigned16(hash2(seed, x0, y0 + 1, channel));
    const int64_t v11 = hashToSigned16(hash2(seed, x0 + 1, y0 + 1, channel));
    const int64_t gx = latticeMm - fx, gy = latticeMm - fy;
    return ((v00 * gx + v10 * fx) * gy + (v01 * gx + v11 * fx) * fy) /
           (latticeMm * latticeMm);
}

} // namespace vxc
