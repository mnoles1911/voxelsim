#pragma once
// Voxel mip chain (plan §3.2): 2x downsample by aggregating 2x2x2 blocks of
// child cells into one parent cell. Mips serve BOTH render LOD (Band 2, plan
// §3.3) and cone-traced GI against the mip chain later — so this aggregation
// rule is worldgen-versioned exactly like the amplifier: it is a deterministic
// function of the level-0 (or level L-1) content, and any change to the rule
// (threshold, tie-break, majority policy) is world-breaking per
// docs/determinism.md — bump vxc::kWorldGenVersion and regenerate goldens
// when (and only when) the rule deliberately changes.
//
// Aggregation rule: parent cell is solid iff >= solidThreshold of its 8
// child cells are solid (default N=4; per-material tuning is a future
// parameter, not yet wired). Parent material = the material with the most
// solid votes among the 8 children; ties broken deterministically by LOWEST
// material id. Below-threshold cells are MAT_AIR.
//
// Brick layout: a level L+1 brick of edge B covers exactly the same world
// footprint as a 2x2x2 group of level L bricks of edge B (each level's voxel
// size is 2x the level below, doctrine plan §3.3 Band 2 "8 cubes become 1").
// Child bricks are ordered index = cx + 2*cy + 4*cz for child offset
// (cx,cy,cz) in {0,1}^3 within the group. A null child pointer means an
// all-air brick (nothing generated/edited there).

#include <functional>
#include <unordered_map>
#include <utility>

#include "voxelcore/brick.h"
#include "voxelcore/core.h"
#include "voxelcore/hash.h"

namespace vxc {

// Downsample one 2x2x2 group of level-L child bricks into a single level-L+1
// parent brick, both of edge B. Parent cell (x,y,z) draws its 8 contributing
// child cells from exactly one child brick (2x2x2 groups never straddle a
// child brick boundary since B is even): the child is selected per-axis by
// whether the parent coordinate is in the low or high half of the brick
// (x>=B/2 selects the +x child, etc.), and the local coordinate within that
// child is (x*2)%B (covering that cell and its +1 neighbor on each axis).
template <int B>
Brick<B> downsampleBricks(const Brick<B>* const children[8], int solidThreshold = 4) {
    static_assert(B % 2 == 0, "brick edge must be even for 2x downsample");
    constexpr int half = B / 2;

    Brick<B> parent;
    for (int z = 0; z < B; ++z) {
        const int cz = (z >= half) ? 1 : 0;
        const int lz = (z * 2) % B;
        for (int y = 0; y < B; ++y) {
            const int cy = (y >= half) ? 1 : 0;
            const int ly = (y * 2) % B;
            for (int x = 0; x < B; ++x) {
                const int cx = (x >= half) ? 1 : 0;
                const int lx = (x * 2) % B;

                const Brick<B>* child = children[cx + 2 * cy + 4 * cz];
                if (!child) continue; // all-air; parent cell stays MAT_AIR.

                int counts[kMaterialCount] = {};
                int solid = 0;
                for (int dz = 0; dz < 2; ++dz)
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const MaterialId m = child->get(lx + dx, ly + dy, lz + dz);
                            if (m != MAT_AIR) {
                                ++counts[m];
                                ++solid;
                            }
                        }
                if (solid < solidThreshold) continue; // stays MAT_AIR.

                // Majority vote; ascending scan + strict '>' ties toward the
                // lowest material id (doctrine: deterministic tie-break).
                MaterialId best = MAT_AIR;
                int bestCount = 0;
                for (int m = 1; m < kMaterialCount; ++m) {
                    if (counts[m] > bestCount) {
                        bestCount = counts[m];
                        best = static_cast<MaterialId>(m);
                    }
                }
                parent.set(x, y, z, best);
            }
        }
    }
    parent.tryCollapse(); // homogeneous fast path: uniform input -> uniform output.
    return parent;
}

// Key identifying a brick at a given mip level. Level 0 = source resolution.
struct MipKey {
    int32_t level = 0;
    BrickKey key;
    friend bool operator==(const MipKey&, const MipKey&) = default;
};

struct MipKeyHash {
    size_t operator()(const MipKey& k) const {
        uint64_t h = splitmix64(static_cast<uint64_t>(k.level));
        h = splitmix64(h ^ static_cast<uint64_t>(static_cast<uint32_t>(k.key.x)));
        h = splitmix64(h ^ static_cast<uint64_t>(static_cast<uint32_t>(k.key.y)));
        h = splitmix64(h ^ static_cast<uint64_t>(static_cast<uint32_t>(k.key.z)));
        return static_cast<size_t>(h);
    }
};

// Deterministic ordering for digest/serialization iteration, level-major.
struct MipKeyLess {
    bool operator()(const MipKey& a, const MipKey& b) const {
        if (a.level != b.level) return a.level < b.level;
        return BrickKeyLess{}(a.key, b.key);
    }
};

// Builds and caches mip levels on demand from a caller-provided level-0
// lookup. Level 0 bricks are owned by the caller (e.g. a ChunkMap/World
// overlay); levels >= 1 are computed by downsampleBricks and cached here,
// keyed by (level, key). No eviction yet — budgets land with the streaming
// system (plan §2.5); this container is intentionally simple and
// deterministic: same inputs always produce the same cached bricks.
template <int B>
class MipChain {
public:
    using BrickT = Brick<B>;
    // Returns the level-0 brick at `key`, or nullptr if nothing is
    // materialized there (treated as all-air by downsampleBricks).
    using SourceFn = std::function<const BrickT*(const BrickKey&)>;

    explicit MipChain(SourceFn level0Source, int solidThreshold = 4)
        : source_(std::move(level0Source)), threshold_(solidThreshold) {}

    int solidThreshold() const { return threshold_; }
    size_t cachedCount() const { return cache_.size(); }

    // Brick at (level, key). Level 0 is served directly by the source
    // callback (not cached here). Levels >=1 are built from the level below
    // and cached; nullptr propagates upward when an entire 2x2x2 child group
    // is all-air, so uniformly-empty regions never materialize bricks.
    const BrickT* brick(int32_t level, const BrickKey& key) {
        if (level <= 0) return source_(key);

        const MipKey mk{level, key};
        auto found = cache_.find(mk);
        if (found != cache_.end()) return &found->second;

        const BrickT* children[8] = {};
        bool anyChild = false;
        for (int cz = 0; cz < 2; ++cz)
            for (int cy = 0; cy < 2; ++cy)
                for (int cx = 0; cx < 2; ++cx) {
                    const BrickKey childKey{key.x * 2 + cx, key.y * 2 + cy, key.z * 2 + cz};
                    const BrickT* child = brick(level - 1, childKey);
                    if (child) anyChild = true;
                    children[cx + 2 * cy + 4 * cz] = child;
                }
        if (!anyChild) return nullptr; // whole group is air; don't materialize.

        auto [it, inserted] =
            cache_.emplace(mk, downsampleBricks<B>(children, threshold_));
        (void)inserted;
        return &it->second;
    }

private:
    SourceFn source_;
    int threshold_;
    std::unordered_map<MipKey, BrickT, MipKeyHash> cache_;
};

} // namespace vxc
