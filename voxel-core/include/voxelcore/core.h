#pragma once
// voxel-core: engine-agnostic voxel world core. UE-header-free by doctrine.
// Determinism conventions: docs/determinism.md. No floats in world derivation.

#include <cstdint>

namespace vxc {

// Bumped on any deliberate change to worldgen math (hash, octave tables,
// stratigraphy constants). Invalidates edit logs and golden digests.
// v3: SyntheticTileSampler spectral-gap fill — 4 new elevation octaves at
// 480/240/120/60 m wavelength (70/38/20/11 m amplitude), terrain-realism audit.
// v4: M4 cave pass (voxelcore/caves.h) — jittered-lattice tunnel network
// carved into the voxelize path. Underground is no longer uniformly solid.
// v5: M4 cave pass v2 — (a) the cavern system (voxelcore/caverns.h) folded
// into ColumnSample/materialAt alongside caves, and (b) the bedrock top moved
// from a 40-60 m band to a 180-220 m one (200 m mean, Matt's decision), which
// is what gives the multi-storey cavern chains their vertical room. One bump
// covers both; see docs/status.md "C4".
// v6: coarse-to-fine detail rework, the first time the amplifier was measured
// against REAL 30 m terrain-diffusion tiles rather than SyntheticTileSampler.
// Three changes, all in the surface term: (a) detail octave table v2 — five
// octaves down to a 200 mm lattice, split into a slope-scaled LANDFORM band
// and a microrelief band whose scale has a floor so it does not vanish on flat
// ground; (b) the detail octaves now use the quintic-faded value noise
// (hash.h valueNoise2Fade) instead of the raw bilinear one, which removes the
// dead-straight lattice creases; (c) the surface bounds widened to match.
// See amplifier.cpp kDetailOctaves for the measurements that forced this.
// v8: the CLIMATE half of what v6 did for the surface half -- the first time
// the biome and stratigraphy consumers were measured against real tiles rather
// than SyntheticTileSampler. voxelcore/climate.h now defines the wire encoding
// (physical WorldClim units, mirroring terrain-service's EXPECTED_CHANNELS),
// every biome threshold is derived from it at compile time instead of being a
// bare u8 literal, SyntheticTileSampler emits that same encoding (and finally
// fills seasonality/precipVariability), classifyBiome tests sea level before
// slope, BARE_ROCK is appended, and the topsoil formula erodes by a FRACTION
// rather than subtracting an absolute depth. Measured with vxc_climateprobe;
// see docs/status.md for the before/after census.
//
// NOTE ON THE SKIPPED v7: the unmerged branch claude/erosion-v7 already claims
// 7 (drainage carving + a region-fitted precip retune). This work branched from
// main at v6 and takes 8 so the two can land in either order without colliding.
// The check at editlog.h is exact equality, not a range, so a gap is harmless.
// v9 (docs/terrain-amplification-plan.md Phase 1): the C2 carrier. The tile
// base is a uniform cubic B-spline instead of bilinear, killing the gradient
// step at every tile-pixel boundary that made the 30 m grid visible under
// directional light; the detail/soil/biome slope currency moves from mm per
// tile PIXEL to mm per METRE, taken from the carrier's analytic gradient, which
// both removes the per-cell gain step and fixes the scale-dependence biome.h
// had recorded as a latent bug; climate is sampled with a faded bilinear plus
// an ecotone dither instead of nearest-pixel, so material boundaries stop
// snapping to the pixel grid. surfaceBoundsMm is re-derived as a Lipschitz
// bound around one carrier evaluation.
inline constexpr uint32_t kWorldGenVersion = 9;

inline constexpr int32_t kVoxelSizeMm = 100; // 10 cm voxels; z=0 is sea level

using MaterialId = uint8_t;

// Material set v1 (amplifier stratigraphy + M4 per-biome surface materials,
// voxelcore/biome.h). Water is implicit (z<0 above terrain) and never stored
// in terrain bricks. IDs are append-only — never renumber an existing entry,
// it would invalidate every saved edit log.
enum Material : MaterialId {
    MAT_AIR = 0,
    MAT_BEDROCK = 1,       // deep unweathered rock, unbounded depth floor
    MAT_ROCK = 2,          // sedimentary layers between subsoil and bedrock
    MAT_GRAVEL = 3,        // coarse subsoil under sandy surfaces (beach/desert)
    MAT_SAND = 4,          // beach + desert surface
    MAT_SUBSOIL = 5,       // generic layer between topsoil and rock
    MAT_TOPSOIL = 6,       // generic fertile soil; temperate-forest surface
    MAT_SNOW = 7,          // legacy v0 high-altitude cap (superseded by
                            // MAT_PERMAFROST/MAT_ROCK for biome surfaces, kept
                            // stable for any existing saved edit logs)
    MAT_GRASS = 8,         // grassland surface
    MAT_JUNGLE_SOIL = 9,   // rainforest surface: dark, wet, organic-rich soil
    MAT_SAVANNA_GRASS = 10,// savanna surface: dry, seasonal grass
    MAT_PODZOL = 11,       // taiga surface: acidic boreal-forest soil
    MAT_PERMAFROST = 12,   // tundra/alpine surface: frozen ground
    MAT_MUD = 13,          // ocean floor / future wetland surface
    MAT_CLAY = 14,         // fine sediment; headroom for future
                            // floodplain/riverbank biomes (M4 rounds 2-3)
    kMaterialCount
};

// Floored division/modulo (int division in C++ truncates toward zero; world
// coordinate -> lattice/pixel/brick index must floor instead).
constexpr int64_t floorDiv(int64_t a, int64_t b) {
    const int64_t q = a / b, r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}
constexpr int64_t floorMod(int64_t a, int64_t b) { return a - floorDiv(a, b) * b; }

constexpr int64_t clampi64(int64_t v, int64_t lo, int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
constexpr int32_t clampi32(int64_t v, int32_t lo, int32_t hi) {
    return static_cast<int32_t>(v < lo ? lo : (v > hi ? hi : v));
}

// FNV-1a 64 — determinism digests only (not worldgen randomness).
struct Digest {
    uint64_t h = 0xcbf29ce484222325ull;
    constexpr void u8(uint8_t v) { h = (h ^ v) * 0x100000001b3ull; }
    constexpr void u16(uint16_t v) { u8(static_cast<uint8_t>(v)); u8(static_cast<uint8_t>(v >> 8)); }
    constexpr void u32(uint32_t v) { u16(static_cast<uint16_t>(v)); u16(static_cast<uint16_t>(v >> 16)); }
    constexpr void u64(uint64_t v) { u32(static_cast<uint32_t>(v)); u32(static_cast<uint32_t>(v >> 32)); }
    constexpr void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
};

} // namespace vxc
