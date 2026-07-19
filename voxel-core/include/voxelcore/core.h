#pragma once
// voxel-core: engine-agnostic voxel world core. UE-header-free by doctrine.
// Determinism conventions: docs/determinism.md. No floats in world derivation.

#include <cstdint>

namespace vxc {

// Bumped on any deliberate change to worldgen math (hash, octave tables,
// stratigraphy constants). Invalidates edit logs and golden digests.
inline constexpr uint32_t kWorldGenVersion = 1;

inline constexpr int32_t kVoxelSizeMm = 100; // 10 cm voxels; z=0 is sea level

using MaterialId = uint8_t;

// Material set v0 (amplifier stratigraphy). Water is implicit (z<0 above
// terrain) and never stored in terrain bricks.
enum Material : MaterialId {
    MAT_AIR = 0,
    MAT_BEDROCK = 1,
    MAT_ROCK = 2, // sedimentary layers
    MAT_GRAVEL = 3,
    MAT_SAND = 4,
    MAT_SUBSOIL = 5,
    MAT_TOPSOIL = 6,
    MAT_SNOW = 7,
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
