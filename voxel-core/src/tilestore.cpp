#include "voxelcore/tilestore.h"

#include <fstream>

#include "voxelcore/bytes.h"
#include "voxelcore/core.h"

namespace vxc {

std::optional<TileData> TileData::parse(const uint8_t* data, size_t size) {
    ByteReader r(data, size);

    uint8_t m0, m1, m2, m3;
    if (!r.u8(m0) || !r.u8(m1) || !r.u8(m2) || !r.u8(m3)) return std::nullopt;
    if (m0 != 'V' || m1 != 'X' || m2 != 'T' || m3 != 'L') return std::nullopt;

    uint16_t version;
    if (!r.u16(version) || version != kFormatVersion) return std::nullopt;

    TileData tile;
    if (!r.u64(tile.seed)) return std::nullopt;
    if (!r.i32(tile.x) || !r.i32(tile.y)) return std::nullopt;
    if (!r.u8(tile.scale)) return std::nullopt;

    uint16_t size16;
    if (!r.u16(size16) || size16 != kTileSize) return std::nullopt;

    tile.elevation.resize(kPixelCount);
    for (uint32_t i = 0; i < kPixelCount; ++i) {
        uint16_t raw;
        if (!r.u16(raw)) return std::nullopt;
        tile.elevation[i] = static_cast<int16_t>(raw);
    }

    for (uint32_t c = 0; c < kClimateChannels; ++c) {
        tile.climate[c].resize(kPixelCount);
        for (uint32_t i = 0; i < kPixelCount; ++i) {
            if (!r.u8(tile.climate[c][i])) return std::nullopt;
        }
    }

    if (!r.atEnd()) return std::nullopt; // trailing bytes
    return tile;
}

std::optional<std::vector<uint8_t>> readFileBytes(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return std::nullopt;
    const std::streamoff n = f.tellg();
    if (n < 0) return std::nullopt;
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    if (n > 0 && !f.read(reinterpret_cast<char*>(buf.data()), n)) return std::nullopt;
    return buf;
}

bool TileGridSampler::loadTile(TileData tile) {
    if (tile.seed != seed_ || tile.scale != scale_) return false;
    const uint64_t key = tileKey(tile.x, tile.y);
    tiles_.insert_or_assign(key, std::move(tile));
    return true;
}

bool TileGridSampler::loadTile(const std::vector<uint8_t>& bytes) {
    std::optional<TileData> parsed = TileData::parse(bytes.data(), bytes.size());
    if (!parsed) return false;
    return loadTile(std::move(*parsed));
}

bool TileGridSampler::loadTileFile(const std::filesystem::path& path) {
    std::optional<std::vector<uint8_t>> bytes = readFileBytes(path);
    if (!bytes) return false;
    return loadTile(*bytes);
}

const TileData* TileGridSampler::findTile(int64_t px, int64_t py, uint32_t& localX,
                                          uint32_t& localY) const {
    const int64_t tileSize = static_cast<int64_t>(TileData::kTileSize);
    const int32_t tx = static_cast<int32_t>(floorDiv(px, tileSize));
    const int32_t ty = static_cast<int32_t>(floorDiv(py, tileSize));
    localX = static_cast<uint32_t>(floorMod(px, tileSize));
    localY = static_cast<uint32_t>(floorMod(py, tileSize));
    auto it = tiles_.find(tileKey(tx, ty));
    return it == tiles_.end() ? nullptr : &it->second;
}

int32_t TileGridSampler::elevationMm(int64_t px, int64_t py) {
    uint32_t lx, ly;
    const TileData* t = findTile(px, py, lx, ly);
    if (!t) {
        missingTileQueries.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const int64_t metres = t->elevationAt(lx, ly);
    return static_cast<int32_t>(metres * 1000);
}

ClimateSample TileGridSampler::climate(int64_t px, int64_t py) {
    uint32_t lx, ly;
    const TileData* t = findTile(px, py, lx, ly);
    if (!t) {
        missingTileQueries.fetch_add(1, std::memory_order_relaxed);
        return ClimateSample{};
    }
    ClimateSample c;
    c.temperature = t->climateAt(0, lx, ly);
    c.seasonality = t->climateAt(1, lx, ly);
    c.precipitation = t->climateAt(2, lx, ly);
    c.precipVariability = t->climateAt(3, lx, ly);
    return c;
}

} // namespace vxc
