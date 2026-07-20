#include "voxelcore/waterca.h"

#include <algorithm>

namespace vxc {

namespace {
// Fixed neighbor order for phase B (header comment "Tick rules v0"): +x, -x,
// +y, -y. Append-only if this ever grows (it won't for a 2D lateral pass);
// never reorder — it's part of the determinism contract.
constexpr int kDx[4] = {1, -1, 0, 0};
constexpr int kDy[4] = {0, 0, 1, -1};
} // namespace

uint8_t WaterCA::getFill(int64_t vx, int64_t vy, int64_t vz) const {
    const BrickKey key = waterKeyForVoxel(vx, vy, vz);
    const WaterBrick8* b = water_.find(key);
    if (!b) return 0;
    return b->get(static_cast<int>(floorMod(vx, WaterBrick8::kEdge)),
                  static_cast<int>(floorMod(vy, WaterBrick8::kEdge)),
                  static_cast<int>(floorMod(vz, WaterBrick8::kEdge)));
}

void WaterCA::setFillAccounted(int64_t vx, int64_t vy, int64_t vz, uint8_t newFill,
                               std::set<BrickKey, BrickKeyLess>* changed) {
    const BrickKey key = waterKeyForVoxel(vx, vy, vz);
    const int lx = static_cast<int>(floorMod(vx, WaterBrick8::kEdge));
    const int ly = static_cast<int>(floorMod(vy, WaterBrick8::kEdge));
    const int lz = static_cast<int>(floorMod(vz, WaterBrick8::kEdge));

    WaterBrick8* b = water_.find(key);
    const uint8_t oldFill = b ? b->get(lx, ly, lz) : 0;
    if (oldFill == newFill) return; // no-op write: not a change, no ledger/activity effect

    if (!b) {
        // Only ever create a brick to store a genuine non-zero write; a
        // newFill==0 write against an absent brick already matches oldFill
        // (both zero) and returned above.
        b = &water_.getOrCreate(key);
    }
    b->set(lx, ly, lz, newFill);

    // Ledger delta (signed to allow the decrement side of a move).
    const int64_t delta = static_cast<int64_t>(newFill) - static_cast<int64_t>(oldFill);
    if (delta >= 0) totalVolume_ += static_cast<uint64_t>(delta);
    else totalVolume_ -= static_cast<uint64_t>(-delta);

    if (b->empty()) water_.erase(key); // homogeneous-empty collapse: absence, not storage

    if (changed) changed->insert(key);
}

uint32_t WaterCA::addWater(int64_t vx, int64_t vy, int64_t vz, uint32_t amount) {
    uint32_t placed = 0;
    int64_t z = vz;
    while (placed < amount) {
        if (isSolid(vx, vy, z)) break; // nowhere left in this column
        const uint8_t cur = getFill(vx, vy, z);
        const uint32_t capacity = 255u - cur;
        const uint32_t remaining = amount - placed;
        const uint32_t add = remaining < capacity ? remaining : capacity;
        if (add > 0) {
            setFillAccounted(vx, vy, z, static_cast<uint8_t>(cur + add), nullptr);
            activate(waterKeyForVoxel(vx, vy, z));
            placed += add;
        }
        ++z;
    }
    return placed;
}

uint64_t WaterCA::recomputeVolume() const {
    uint64_t sum = 0;
    for (const auto& [key, brick] : water_) sum += brick.volume();
    return sum;
}

void WaterCA::gravityPass(const std::vector<BrickKey>& order, std::set<BrickKey, BrickKeyLess>& changed) {
    for (const BrickKey& key : order) {
        for (int ci = 0; ci < WaterBrick8::kCells; ++ci) {
            const int x = ci % WaterBrick8::kEdge;
            const int y = (ci / WaterBrick8::kEdge) % WaterBrick8::kEdge;
            const int z = ci / (WaterBrick8::kEdge * WaterBrick8::kEdge);
            const int64_t vx = int64_t(key.x) * WaterBrick8::kEdge + x;
            const int64_t vy = int64_t(key.y) * WaterBrick8::kEdge + y;
            const int64_t vz = int64_t(key.z) * WaterBrick8::kEdge + z;

            const uint8_t f = getFill(vx, vy, vz);
            if (f == 0) continue;
            const int64_t belowZ = vz - 1;
            if (isSolid(vx, vy, belowZ)) continue;
            const uint8_t below = getFill(vx, vy, belowZ);
            if (below >= 255) continue;
            const int moved = std::min<int>(f, 255 - below);
            if (moved == 0) continue;

            setFillAccounted(vx, vy, vz, static_cast<uint8_t>(f - moved), &changed);
            setFillAccounted(vx, vy, belowZ, static_cast<uint8_t>(below + moved), &changed);
        }
    }
}

void WaterCA::lateralPass(const std::vector<BrickKey>& order, std::set<BrickKey, BrickKeyLess>& changed) {
    for (const BrickKey& key : order) {
        for (int ci = 0; ci < WaterBrick8::kCells; ++ci) {
            const int x = ci % WaterBrick8::kEdge;
            const int y = (ci / WaterBrick8::kEdge) % WaterBrick8::kEdge;
            const int z = ci / (WaterBrick8::kEdge * WaterBrick8::kEdge);
            const int64_t vx = int64_t(key.x) * WaterBrick8::kEdge + x;
            const int64_t vy = int64_t(key.y) * WaterBrick8::kEdge + y;
            const int64_t vz = int64_t(key.z) * WaterBrick8::kEdge + z;

            if (getFill(vx, vy, vz) == 0) continue;
            const int64_t belowZ = vz - 1;
            const bool supported = isSolid(vx, vy, belowZ) || getFill(vx, vy, belowZ) >= 255;
            if (!supported) continue;

            for (int dir = 0; dir < 4; ++dir) {
                const uint8_t self = getFill(vx, vy, vz); // re-read: earlier dirs may have drained it
                if (self == 0) break;
                const int64_t nx = vx + kDx[dir];
                const int64_t ny = vy + kDy[dir];
                if (isSolid(nx, ny, vz)) continue;
                const uint8_t nf = getFill(nx, ny, vz);
                if (self <= nf) continue; // flow only downhill

                const int diff = self - nf;
                int flow = diff / 4;
                if (flow == 0 && diff > 1) flow = 1; // guarantee forward progress on diff in {2,3}
                if (flow <= 0) continue;
                flow = std::min(flow, int(self));       // self never goes negative
                flow = std::min(flow, 255 - int(nf));    // neighbor never exceeds 255
                if (flow <= 0) continue;

                setFillAccounted(vx, vy, vz, static_cast<uint8_t>(self - flow), &changed);
                setFillAccounted(nx, ny, vz, static_cast<uint8_t>(nf + flow), &changed);
            }
        }
    }
}

void WaterCA::hydrostaticPass(const std::vector<BrickKey>&, std::set<BrickKey, BrickKeyLess>&) {
    // v0 stub (header comment "Phase C"): full hydrostatic column pressure
    // (U-bends, breach inrush) is W2-proper, not v0. Intentionally a no-op —
    // exists so the pipeline shape (gravity -> lateral -> hydrostatic) is
    // fixed for the next version to fill in without reshuffling step().
}

void WaterCA::step() {
    std::vector<BrickKey> order(active_.begin(), active_.end()); // std::set is already sorted by BrickKeyLess
    lastSteppedBrickCount_ = order.size();

    std::set<BrickKey, BrickKeyLess> changed;
    gravityPass(order, changed);
    lateralPass(order, changed);
    hydrostaticPass(order, changed);

    active_ = std::move(changed);
}

void WaterCA::digest(Digest& d) const {
    std::vector<BrickKey> keys;
    keys.reserve(water_.size());
    for (const auto& [key, brick] : water_) keys.push_back(key);
    std::sort(keys.begin(), keys.end(), BrickKeyLess{});
    for (const BrickKey& key : keys) {
        d.i64(key.x);
        d.i64(key.y);
        d.i64(key.z);
        water_.find(key)->digest(d);
    }
}

} // namespace vxc
