#pragma once
// Sparse voxel brick storage (plan §3.2): occupancy bitmask + per-brick
// material palette; homogeneous bricks collapse to a single value. Edge size
// is a template parameter so the M0 bench can compare 8^3 vs 16^3.

#include <bitset>
#include <cstddef>
#include <vector>

#include "voxelcore/core.h"
#include "voxelcore/hash.h"

namespace vxc {

struct BrickKey {
    int32_t x = 0, y = 0, z = 0; // brick coords = floorDiv(voxel coord, B)
    friend bool operator==(const BrickKey&, const BrickKey&) = default;
};

struct BrickKeyHash {
    size_t operator()(const BrickKey& k) const {
        uint64_t h = splitmix64(static_cast<uint32_t>(k.x));
        h = splitmix64(h ^ static_cast<uint32_t>(k.y));
        h = splitmix64(h ^ static_cast<uint32_t>(k.z));
        return static_cast<size_t>(h);
    }
};

// Deterministic ordering for digest/serialization iteration.
struct BrickKeyLess {
    bool operator()(const BrickKey& a, const BrickKey& b) const {
        if (a.z != b.z) return a.z < b.z;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    }
};

template <int B>
class Brick {
    static_assert(B == 8 || B == 16, "benchmarked brick sizes");

public:
    static constexpr int kEdge = B;
    static constexpr int kCells = B * B * B;

    static constexpr int cellIndex(int x, int y, int z) { return x + B * (y + B * z); }

    Brick() = default;
    explicit Brick(MaterialId fill) : homogeneousMat_(fill) {
        if (fill != MAT_AIR) occupancy_.set();
    }

    bool isHomogeneous() const { return cells_.empty(); }
    MaterialId homogeneousMaterial() const { return homogeneousMat_; }

    MaterialId get(int x, int y, int z) const {
        if (cells_.empty()) return homogeneousMat_;
        return palette_[cells_[cellIndex(x, y, z)]];
    }

    bool occupied(int x, int y, int z) const { return occupancy_[cellIndex(x, y, z)]; }
    const std::bitset<kCells>& occupancy() const { return occupancy_; }
    size_t solidCount() const { return occupancy_.count(); }
    bool empty() const { return occupancy_.none(); }

    size_t paletteSize() const { return cells_.empty() ? 1 : palette_.size(); }

    void set(int x, int y, int z, MaterialId mat) {
        if (cells_.empty()) {
            if (mat == homogeneousMat_) return;
            expand();
        }
        const int ci = cellIndex(x, y, z);
        cells_[ci] = paletteIndex(mat);
        occupancy_[ci] = (mat != MAT_AIR);
    }

    // Collapse back to homogeneous storage if all cells hold one material.
    // Returns true if the brick is homogeneous afterwards.
    bool tryCollapse() {
        if (cells_.empty()) return true;
        const uint8_t first = cells_[0];
        for (int i = 1; i < kCells; ++i)
            if (cells_[i] != first) return false;
        homogeneousMat_ = palette_[first];
        cells_.clear();
        cells_.shrink_to_fit();
        palette_.clear();
        palette_.shrink_to_fit();
        return true;
    }

    // Logical content equality (representation-independent).
    friend bool operator==(const Brick& a, const Brick& b) {
        for (int i = 0; i < kCells; ++i) {
            const int x = i % B, y = (i / B) % B, z = i / (B * B);
            if (a.get(x, y, z) != b.get(x, y, z)) return false;
        }
        return true;
    }

    void digest(Digest& d) const {
        for (int z = 0; z < B; ++z)
            for (int y = 0; y < B; ++y)
                for (int x = 0; x < B; ++x) d.u8(get(x, y, z));
    }

private:
    void expand() {
        cells_.assign(kCells, 0);
        palette_.assign(1, homogeneousMat_);
        // occupancy_ already reflects the homogeneous fill.
    }

    uint8_t paletteIndex(MaterialId mat) {
        for (size_t i = 0; i < palette_.size(); ++i)
            if (palette_[i] == mat) return static_cast<uint8_t>(i);
        palette_.push_back(mat);
        return static_cast<uint8_t>(palette_.size() - 1);
    }

    // Homogeneous when cells_ is empty; then homogeneousMat_ fills the brick.
    MaterialId homogeneousMat_ = MAT_AIR;
    std::vector<MaterialId> palette_; // palette index -> material
    std::vector<uint8_t> cells_;      // kCells palette indices (dense)
    std::bitset<kCells> occupancy_;   // non-air mask, valid in both modes
};

} // namespace vxc
