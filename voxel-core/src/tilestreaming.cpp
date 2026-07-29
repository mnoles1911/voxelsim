#include "voxelcore/tilestreaming.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace vxc {

std::vector<BlockCoord> blocksCoveringRect(PixelRect rect, int64_t tileSizePx, uint32_t blockDimPx) {
    std::vector<BlockCoord> out;
    if (tileSizePx <= 0 || blockDimPx == 0) return out;
    if (rect.px1 < rect.px0 || rect.py1 < rect.py0) return out;

    // Walk tile-by-tile (a footprint spans at most a handful of tiles for any
    // realistic dilation), and within each tile walk block-by-block over the
    // LOCAL (tile-relative) intersection.
    const int64_t tx0 = floorDiv(rect.px0, tileSizePx);
    const int64_t tx1 = floorDiv(rect.px1, tileSizePx);
    const int64_t ty0 = floorDiv(rect.py0, tileSizePx);
    const int64_t ty1 = floorDiv(rect.py1, tileSizePx);
    const int64_t bDim = static_cast<int64_t>(blockDimPx);

    for (int64_t ty = ty0; ty <= ty1; ++ty) {
        const int64_t tileOriginY = ty * tileSizePx;
        const int64_t localY0 = std::max<int64_t>(0, rect.py0 - tileOriginY);
        const int64_t localY1 = std::min<int64_t>(tileSizePx - 1, rect.py1 - tileOriginY);
        if (localY1 < localY0) continue;
        const int64_t by0 = localY0 / bDim; // local coords are >=0: plain / is floor here
        const int64_t by1 = localY1 / bDim;

        for (int64_t tx = tx0; tx <= tx1; ++tx) {
            const int64_t tileOriginX = tx * tileSizePx;
            const int64_t localX0 = std::max<int64_t>(0, rect.px0 - tileOriginX);
            const int64_t localX1 = std::min<int64_t>(tileSizePx - 1, rect.px1 - tileOriginX);
            if (localX1 < localX0) continue;
            const int64_t bx0 = localX0 / bDim;
            const int64_t bx1 = localX1 / bDim;

            for (int64_t by = by0; by <= by1; ++by) {
                for (int64_t bx = bx0; bx <= bx1; ++bx) {
                    out.push_back(BlockCoord{TileCoord{static_cast<int32_t>(tx), static_cast<int32_t>(ty)},
                                             static_cast<uint32_t>(bx), static_cast<uint32_t>(by)});
                }
            }
        }
    }
    return out;
}

std::vector<BlockCoord> dilatedBlockCoverage(PixelRect footprint, int64_t tileSizePx, uint32_t blockDimPx) {
    return blocksCoveringRect(dilateForCarrierStencil(footprint), tileSizePx, blockDimPx);
}

PixelRect fineReadPixelRect(int64_t worldMmX0, int64_t worldMmY0, int64_t worldMmX1, int64_t worldMmY1,
                            int64_t readMarginMm, int32_t pixelSizeMm) {
    if (pixelSizeMm <= 0) return PixelRect{0, 0, -1, -1}; // inverted => "covers nothing"
    if (readMarginMm < 0) readMarginMm = 0;
    // The world rect is half-open, so the last mm actually touched is x1-1;
    // taking floorDiv of x1 itself would claim one extra pixel column on every
    // call, which is harmless for a gate but makes the rect wrong for anyone
    // who later uses it to size a buffer.
    const int64_t x0 = worldMmX0 - readMarginMm;
    const int64_t y0 = worldMmY0 - readMarginMm;
    const int64_t x1 = (worldMmX1 > worldMmX0 ? worldMmX1 - 1 : worldMmX0) + readMarginMm;
    const int64_t y1 = (worldMmY1 > worldMmY0 ? worldMmY1 - 1 : worldMmY0) + readMarginMm;
    const PixelRect cells{floorDiv(x0, pixelSizeMm), floorDiv(y0, pixelSizeMm),
                          floorDiv(x1, pixelSizeMm), floorDiv(y1, pixelSizeMm)};
    return dilateForCarrierStencil(cells);
}

std::vector<TileCoord> tilesCoveringFootprint(int64_t worldMmX0, int64_t worldMmY0, int64_t worldMmX1,
                                              int64_t worldMmY1, int64_t readMarginMm, int64_t tileSizePx,
                                              int32_t pixelSizeMm) {
    std::vector<TileCoord> out;
    if (tileSizePx <= 0) return out;

    // ONE conversion, the shared one: margin in world mm, then pixels, then the
    // carrier stencil. Everything this function knows about how far a generator
    // reads comes from fineReadPixelRect and nowhere else.
    const PixelRect rect = fineReadPixelRect(worldMmX0, worldMmY0, worldMmX1, worldMmY1, readMarginMm, pixelSizeMm);
    if (rect.px1 < rect.px0 || rect.py1 < rect.py0) return out; // degenerate pixelSizeMm

    const TileCoord lo = tileCoordForPixel(rect.px0, rect.py0, tileSizePx);
    const TileCoord hi = tileCoordForPixel(rect.px1, rect.py1, tileSizePx);
    out.reserve(size_t(hi.x - lo.x + 1) * size_t(hi.y - lo.y + 1));
    for (int32_t ty = lo.y; ty <= hi.y; ++ty) {
        for (int32_t tx = lo.x; tx <= hi.x; ++tx) {
            out.push_back(TileCoord{tx, ty});
        }
    }
    return out;
}

std::vector<TileCoord> missingTilesForFootprint(int64_t worldMmX0, int64_t worldMmY0, int64_t worldMmX1,
                                                int64_t worldMmY1, int64_t readMarginMm, int64_t tileSizePx,
                                                const TileCoordSet& resident, int32_t pixelSizeMm) {
    std::vector<TileCoord> out;
    for (const TileCoord& t :
         tilesCoveringFootprint(worldMmX0, worldMmY0, worldMmX1, worldMmY1, readMarginMm, tileSizePx, pixelSizeMm)) {
        if (resident.find(t) == resident.end()) out.push_back(t);
    }
    return out;
}

TileCoord tileCoordForWorldMm(int64_t worldMmX, int64_t worldMmY, int64_t tileFootprintMm) {
    if (tileFootprintMm <= 0) return TileCoord{0, 0};
    return TileCoord{static_cast<int32_t>(floorDiv(worldMmX, tileFootprintMm)),
                     static_cast<int32_t>(floorDiv(worldMmY, tileFootprintMm))};
}

std::vector<TileCoord> squareTileRing(TileCoord center, int32_t radiusTiles) {
    std::vector<TileCoord> out;
    if (radiusTiles < 0) return out;
    const size_t edge = static_cast<size_t>(radiusTiles) * 2u + 1u;
    out.reserve(edge * edge);
    for (int32_t dy = -radiusTiles; dy <= radiusTiles; ++dy) {
        for (int32_t dx = -radiusTiles; dx <= radiusTiles; ++dx) {
            out.push_back(TileCoord{center.x + dx, center.y + dy});
        }
    }
    return out;
}

std::string formatFineTileCacheKey(const std::string& providerId, uint64_t seed, int32_t x, int32_t y,
                                    uint8_t scale) {
    std::ostringstream os;
    os << providerId << '/' << std::hex << std::setw(16) << std::setfill('0') << seed << std::dec
       << "/s" << static_cast<unsigned>(scale) << '/' << x << '_' << y;
    return os.str();
}

FineTileValidationResult validateAndParseFineTile(std::vector<uint8_t> bytes, uint64_t expectedSeed,
                                                   int32_t expectedX, int32_t expectedY,
                                                   const FineDecompressor& decompressor) {
    FineTileValidationResult out;
    std::optional<FineTile> parsed = FineTile::parse(std::move(bytes), decompressor, &out.error);
    if (!parsed) {
        out.verdict = FineTileVerdict::kCorrupt;
        return out;
    }
    if (parsed->seed() != expectedSeed || parsed->tileX() != expectedX || parsed->tileY() != expectedY) {
        out.verdict = FineTileVerdict::kIdentityMismatch;
        return out;
    }
    out.verdict = FineTileVerdict::kOk;
    out.tile = std::move(*parsed);
    return out;
}

void LruBudgetCache::touch(const std::string& key, uint64_t bytes) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        entries_.emplace(key, Entry{bytes, ++clock_});
        residentBytes_ += bytes;
    } else {
        residentBytes_ = residentBytes_ - it->second.bytes + bytes;
        it->second.bytes = bytes;
        it->second.lastTouch = ++clock_;
    }
}

void LruBudgetCache::remove(const std::string& key) {
    auto it = entries_.find(key);
    if (it == entries_.end()) return;
    residentBytes_ -= it->second.bytes;
    entries_.erase(it);
}

void LruBudgetCache::setPinned(std::unordered_set<std::string> keys) { pinned_ = std::move(keys); }

std::vector<std::string> LruBudgetCache::selectEvictions() const {
    std::vector<std::string> out;
    if (residentBytes_ <= budgetBytes_) return out;

    std::vector<const std::pair<const std::string, Entry>*> candidates;
    candidates.reserve(entries_.size());
    for (const auto& kv : entries_) {
        if (pinned_.find(kv.first) == pinned_.end()) candidates.push_back(&kv);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto* a, const auto* b) { return a->second.lastTouch < b->second.lastTouch; });

    uint64_t projected = residentBytes_;
    for (const auto* kv : candidates) {
        if (projected <= budgetBytes_) break;
        out.push_back(kv->first);
        projected -= kv->second.bytes;
    }
    return out;
}

} // namespace vxc
