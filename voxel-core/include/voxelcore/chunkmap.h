#pragma once
// Hashed map of sparse bricks with unbounded 3D keys (plan §3.2). Only bricks
// intersecting the surface shell or touched by edits exist; everything else is
// implicit (air above terrain, generator-defined solid below).

#include <unordered_map>

#include "voxelcore/brick.h"

namespace vxc {

template <int B>
class ChunkMap {
public:
    using BrickT = Brick<B>;

    static BrickKey keyForVoxel(int64_t vx, int64_t vy, int64_t vz) {
        return BrickKey{static_cast<int32_t>(floorDiv(vx, B)),
                        static_cast<int32_t>(floorDiv(vy, B)),
                        static_cast<int32_t>(floorDiv(vz, B))};
    }

    BrickT* find(const BrickKey& k) {
        auto it = bricks_.find(k);
        return it == bricks_.end() ? nullptr : &it->second;
    }
    const BrickT* find(const BrickKey& k) const {
        auto it = bricks_.find(k);
        return it == bricks_.end() ? nullptr : &it->second;
    }

    BrickT& getOrCreate(const BrickKey& k) { return bricks_[k]; }
    BrickT& insert(const BrickKey& k, BrickT brick) {
        return bricks_.insert_or_assign(k, std::move(brick)).first->second;
    }
    bool erase(const BrickKey& k) { return bricks_.erase(k) != 0; }

    size_t size() const { return bricks_.size(); }
    auto begin() { return bricks_.begin(); }
    auto end() { return bricks_.end(); }
    auto begin() const { return bricks_.begin(); }
    auto end() const { return bricks_.end(); }

private:
    std::unordered_map<BrickKey, BrickT, BrickKeyHash> bricks_;
};

} // namespace vxc
