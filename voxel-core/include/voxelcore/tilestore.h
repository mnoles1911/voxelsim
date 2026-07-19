#pragma once
// Decoded tile storage + sampling (plan §3.1 step 2, §3.4 ITerrainSource).
// TileData parses the exact wire format produced by
// terrain-service/terrain_service/tile_codec.py (encode/decode); TileGridSampler
// serves a grid of decoded tiles for one (seed, scale) through ITileSampler,
// the same interface amplifier.h consumes for both local and remote
// ITerrainSource implementations.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

#include "voxelcore/tiles.h"

namespace vxc {

// Decoded tile. Wire format (all little-endian, matches tile_codec.py
// exactly):
//   magic    4B  "VXTL"
//   version  u16 (1)
//   seed     u64
//   x, y     i32  tile coords (tile (0,0) covers pixels [0,512) each axis)
//   scale    u8   (1 => 30m/px, 8 => 11.25m/px)
//   size     u16  (512)
//   elevation int16[size*size], row-major, y outer, metres
//   climate   uint8[4][size*size]  (temperature, seasonality, precipitation,
//                                   precipVariability planes)
struct TileData {
    static constexpr uint32_t kTileSize = 512;
    static constexpr uint32_t kPixelCount = kTileSize * kTileSize;
    static constexpr uint32_t kClimateChannels = 4;
    static constexpr uint16_t kFormatVersion = 1;

    uint64_t seed = 0;
    int32_t x = 0, y = 0;
    uint8_t scale = 1;
    std::vector<int16_t> elevation;                             // kPixelCount, metres
    std::array<std::vector<uint8_t>, kClimateChannels> climate;  // each kPixelCount

    int16_t elevationAt(uint32_t px, uint32_t py) const {
        return elevation[py * kTileSize + px];
    }
    uint8_t climateAt(uint32_t channel, uint32_t px, uint32_t py) const {
        return climate[channel][py * kTileSize + px];
    }

    // Exact parse: validates magic, version, declared size, AND total byte
    // length (rejects both truncated input and trailing bytes) — the same
    // rejects tile_codec.decode() enforces. Returns nullopt on any mismatch.
    static std::optional<TileData> parse(const uint8_t* data, size_t size);
};

// mm-per-pixel by scale; must match terrain-service's
// tile_codec.PIXEL_SIZE_MM and ITileSampler::pixelSizeMm's contract. Returns
// 0 for unsupported scales.
constexpr int32_t tilePixelSizeMm(uint8_t scale) {
    return scale == 1 ? 30000 : (scale == 8 ? 11250 : 0);
}

// Reads a whole file into memory. Returns nullopt on any I/O failure.
std::optional<std::vector<uint8_t>> readFileBytes(const std::filesystem::path& path);

// Grid of decoded tiles for a single (seed, scale), addressed in tile-pixel
// coordinates: pixel (px, py) belongs to tile (floorDiv(px,512),
// floorDiv(py,512)) at local offset (floorMod(px,512), floorMod(py,512)).
//
// Missing-tile policy (deterministic, doctrine-safe — plan §2.3 tiles are
// canonical data, never silently fabricated): a query into a tile that
// hasn't been loaded returns elevation 0 and default climate rather than
// throwing (ITileSampler's query methods are hot-path and must stay
// exception-free), but increments the public missingTileQueries counter so
// callers — tests, streaming code — can detect and fail loudly.
class TileGridSampler final : public ITileSampler {
public:
    TileGridSampler(uint64_t seed, uint8_t scale)
        : seed_(seed), scale_(scale), pixelSizeMm_(tilePixelSizeMm(scale)) {}

    uint64_t seed() const { return seed_; }
    uint8_t scale() const { return scale_; }
    int32_t pixelSizeMm() const override { return pixelSizeMm_; }

    // Stores a decoded tile, keyed by its (x, y). Rejects (returns false, no
    // state change) if the tile's seed or scale doesn't match this sampler.
    bool loadTile(TileData tile);
    // Parses then stores. Rejects on parse failure or seed/scale mismatch.
    bool loadTile(const std::vector<uint8_t>& bytes);
    // Reads a .vxtl file from disk, then parses and stores it.
    bool loadTileFile(const std::filesystem::path& path);

    size_t tileCount() const { return tiles_.size(); }

    int32_t elevationMm(int64_t px, int64_t py) override;
    ClimateSample climate(int64_t px, int64_t py) override;

    // Count of queries answered with the missing-tile default. Public by
    // design so tests/streaming code can assert directly on it.
    uint64_t missingTileQueries = 0;

private:
    static uint64_t tileKey(int32_t tx, int32_t ty) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(tx)) << 32) |
               static_cast<uint64_t>(static_cast<uint32_t>(ty));
    }
    const TileData* findTile(int64_t px, int64_t py, uint32_t& localX, uint32_t& localY) const;

    uint64_t seed_;
    uint8_t scale_;
    int32_t pixelSizeMm_;
    std::unordered_map<uint64_t, TileData> tiles_;
};

} // namespace vxc
