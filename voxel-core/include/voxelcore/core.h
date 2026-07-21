#pragma once
// voxel-core: engine-agnostic voxel world core. UE-header-free by doctrine.
// Determinism conventions: docs/determinism.md. No floats in world derivation.

#include <cstdint>

namespace vxc {

// Bumped on any deliberate change to worldgen math (hash, octave tables,
// stratigraphy constants). Invalidates edit logs and golden digests.
inline constexpr uint32_t kWorldGenVersion = 2;

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
