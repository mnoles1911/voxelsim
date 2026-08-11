#pragma once
// Worldgen hash v1 â€” THE procedural randomness primitive (docs/determinism.md).
// Any change here is world-breaking: bump kWorldGenVersion, regenerate goldens.

#include "voxelcore/core.h"

namespace vxc {

constexpr uint64_t splitmix64(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// ============================================================================
// CHANNEL ID REGISTRY â€” the single authoritative allocation table.
// ============================================================================
// The channel is hash2/hash3's ONLY domain separator. Two subsystems that
// hash the same (seed, x, y[, z]) under the same channel are not "similar" â€”
// they are literally the same noise function, differing only because they
// happen to be evaluated at different coordinate magnitudes. That is luck,
// not design, and it turns into a visible correlation the moment someone
// changes a lattice size. This is not hypothetical: CH_CAVE_NODE/CH_CAVE_EDGE
// (caves.h) shipped reusing 18/19, the same ids CH_ECOTONE_TEMP/CH_ECOTONE_
// PRECIP already had here, for an entire worldgen version before anyone
// noticed (see HISTORY below).
//
// hash.h cannot list every id as an enumerator: caves.h, caverns.h,
// detail_rill.h, detail_bedding.h and density3.h all `#include "hash.h"`, so
// the dependency only runs one way and their symbols cannot appear in this
// file's own enum. This table is therefore the human-readable ledger â€” the
// place to check before picking a new id â€” and voxelcore/hash_channel_
// registry.h is the machine-checked one: a constexpr uniqueness scan over the
// REAL symbols (downstream of all of them, where every one is visible at
// once) wired into a static_assert, so a future collision fails the build
// instead of waiting to be read about. Keep this table and that file in sync;
// if they disagree, the static_assert is the one that is right.
//
//   id(s)    owner              symbol
//   -------  -----------------  --------------------------------------------
//   0..15    hash.h             CH_DETAIL_OCTAVE_BASE + octave index
//   16       hash.h             CH_TOPSOIL_JITTER
//   17       hash.h             CH_BEDROCK_JITTER
//   18       hash.h             CH_ECOTONE_TEMP    (v9 biome-boundary dither)
//   19       hash.h             CH_ECOTONE_PRECIP  (v9 biome-boundary dither)
//   20       caves.h            CH_CAVE_RADIUS
//   21       caves.h            CH_CAVE_SHAFT
//   22       caverns.h          CH_CAVERN_SITE
//   23       caverns.h          CH_CAVERN_ROUGH
//   24       caves.h            CH_CREVICE
//   25       caverns.h          CH_CAVERN_FLOOD
//   26       detail_rill.h      CH_RILL
//   27       detail_bedding.h   CH_BEDDING_STRIKE
//   28       detail_bedding.h   CH_BEDDING
//   29       FREE               (was density3.h CH_POCKET â€” see HISTORY)
//   30       caves.h            CH_CAVE_NODE       (moved from 18 â€” see HISTORY)
//   31       caves.h            CH_CAVE_EDGE       (moved from 19 â€” see HISTORY)
//   32..47   hash.h             CH_SYNTH_TILE_BASE + synthetic dev-tile index
//   48       hash.h             CH_CARRIER_WARP_X  (v16 carrier warp)
//   49       hash.h             CH_CARRIER_WARP_Y  (v16 carrier warp)
//   50..     FREE
//
// HISTORY. CH_CAVE_NODE and CH_CAVE_EDGE originally reused ids 18 and 19 â€”
// the exact ids CH_ECOTONE_TEMP and CH_ECOTONE_PRECIP already occupied â€” a
// genuine double allocation rather than a naming clash. It was moved here,
// to 30 and 31 (the two ids density3.h's own channel-note survey had already
// flagged as the free ones below the synthetic-tile reservation), so the
// ecotone dither and the cave lattice draw from provably independent fields
// instead of the same one. This changes the output of every hash2/hash3 call
// with channel CH_CAVE_NODE or CH_CAVE_EDGE â€” see vxc_bench --radius 128
// --digest â€” but was done WITHOUT a kWorldGenVersion bump at the explicit
// request of the id's owner, who batches version bumps and goldens
// separately. CH_ECOTONE_TEMP/PRECIP were left at 18/19 rather than moved:
// they are the ids hash.h's own enum has always had, and caves.h's own
// comment already deferred to hash.h as the numbering authority.
//
// Id 29 was density3.h's CH_POCKET, for the 3D density band's joint-controlled
// pocket term. That term was deleted at kWorldGenVersion 12 -- it produced zero
// overhangs (its 6400 mm z lattice made dD/dz > 1 unreachable) and, inside the
// halved 350 mm envelope, any amplitude it could have had was under one voxel,
// so 97% of what it changed was isolated single-voxel speckle. See density3.h
// section 2. The id is FREE, and it is listed as free rather than quietly
// reused so that anyone restoring the term gets its own field back rather than
// colliding with whatever took the number in the meantime.
// ============================================================================

// Channel ids domain-separate hash uses. Append only â€” never renumber a
// shipped id without a kWorldGenVersion bump (see the registry above).
enum HashChannel : uint32_t {
    CH_DETAIL_OCTAVE_BASE = 0, // +octave index, reserve 0..15
    CH_TOPSOIL_JITTER = 16,
    CH_BEDROCK_JITTER = 17,
    CH_ECOTONE_TEMP = 18,   // v9 biome-boundary dither, temperature side
    CH_ECOTONE_PRECIP = 19, // v9 biome-boundary dither, precipitation side
    // v16 horizontal carrier warp. 48/49, the first ids past CH_SYNTH_TILE_BASE's
    // 32..47 reserve. NOT 29/30: 30 is CH_CAVE_NODE and density3.h still defines
    // CH_POCKET = 29, and the registry's collision scan -- the authority -- rejects
    // both. Registered in hash_channel_registry.h.
    CH_CARRIER_WARP_X = 48,
    CH_CARRIER_WARP_Y = 49,
    CH_SYNTH_TILE_BASE = 32, // synthetic dev tiles, reserve 32..47
    // Environmental-asset scatter (voxelcore/assetplacement.h). Three
    // channels, not one, because the three draws are asked at DIFFERENT
    // rates and must not correlate: occupancy is asked of every cell of
    // every layer, the anchor jitter only of cells that carry a site, and
    // the bank pick only of sites that survive the caller's biome veto. One
    // shared channel would make "which seed this tree is" a function of
    // "how far from its cell centre it stands", which reads as a stand of
    // identical trees along a diagonal -- the exact class of correlation the
    // registry below exists to prevent, and the reason CH_CAVE_NODE cost a
    // worldgen version.
    //
    // Each layer additionally offsets by its own index (see
    // AssetLayer::channelBase), so the four size-class layers are
    // domain-separated from each other too: without that, a groundcover cell
    // and a canopy cell at the same lattice coordinate would agree on
    // occupancy, and every big tree would stand in a patch of flowers.
    CH_ASSET_SITE = 50,   // reserves 50..53, one per layer
    CH_ASSET_JITTER = 54, // reserves 54..57
    CH_ASSET_PICK = 58,   // reserves 58..61
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
// and rectangular blocking on the finer ones. It reads as a "hard step" â€” it
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
// [-32768, 32767] â€” the property amplifier.cpp's coupling (1) pins and the
// whole detail allowance is derived from. No bound moves because of this.
//
// EXACT INTEGER FORM. t is carried as a 10-bit fraction (tq in [0, 1024]) so
// that the quintic's t^5 term stays inside int64: the numerator is bounded by
// 15 * 1024^5 ~ 1.7e16 against int64's 9.2e18. The result is returned in the
// SAME units as fx, so the bilinear form it feeds is unchanged. Truncating
// division preserves monotonicity, so the faded fraction is still
// nondecreasing in fx and still lands in [0, latticeMm].
//
// fx is always in [0, latticeMm) â€” floorDiv guarantees it â€” so every division
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
// hashes, same output range â€” only the interpolation weights differ.
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
