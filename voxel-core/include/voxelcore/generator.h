#pragma once
// Voxelization on demand (plan §3.1 step 4): fills sparse bricks from the
// amplifier. Only bricks intersecting the surface shell (or touched by edits)
// should be materialized by callers; underground stays implicit-solid.

#include "voxelcore/amplifier.h"
#include "voxelcore/brick.h"
#include "voxelcore/chunkmap.h"

#include <vector>

namespace vxc {

template <int B>
class GeneratedWorld {
public:
    // Column samples for one brick footprint (B*B columns), reusable across a
    // vertical stack of bricks.
    struct ColumnGrid {
        int32_t bx = 0, by = 0;
        ColumnSample cols[B * B];
        const ColumnSample& at(int x, int y) const { return cols[x + B * y]; }
    };

    explicit GeneratedWorld(const Amplifier& amp) : amp_(&amp) {}

    const Amplifier& amplifier() const { return *amp_; }

    ColumnGrid columns(int32_t bx, int32_t by) const {
        ColumnGrid g;
        g.bx = bx;
        g.by = by;
        for (int y = 0; y < B; ++y)
            for (int x = 0; x < B; ++x)
                g.cols[x + B * y] =
                    amp_->column(int64_t(bx) * B + x, int64_t(by) * B + y);
        return g;
    }

    Brick<B> makeBrick(const BrickKey& key, const ColumnGrid& grid) const {
        Brick<B> brick;
        for (int y = 0; y < B; ++y)
            for (int x = 0; x < B; ++x) {
                const ColumnSample& col = grid.at(x, y);
                for (int z = 0; z < B; ++z)
                    brick.set(x, y, z,
                              Amplifier::materialAt(col, int64_t(key.z) * B + z));
            }
        brick.tryCollapse();
        return brick;
    }

    Brick<B> makeBrick(const BrickKey& key) const {
        return makeBrick(key, columns(key.x, key.y));
    }

    // Brick-z range intersecting the surface shell for footprint (bx, by):
    // every brick containing some column's topmost solid voxel.
    void surfaceBrickRange(const ColumnGrid& grid, int32_t& bzMin, int32_t& bzMax) const {
        int64_t vzMin = INT64_MAX, vzMax = INT64_MIN;
        for (int i = 0; i < B * B; ++i) {
            // Topmost solid voxel: centre (vz*100+50) <= surfaceMm.
            const int64_t top = floorDiv(grid.cols[i].surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            vzMin = top < vzMin ? top : vzMin;
            vzMax = top > vzMax ? top : vzMax;
        }
        bzMin = static_cast<int32_t>(floorDiv(vzMin, B));
        bzMax = static_cast<int32_t>(floorDiv(vzMax, B));
    }

    MaterialId materialAt(int64_t vx, int64_t vy, int64_t vz) const {
        return amp_->materialAt(vx, vy, vz);
    }

private:
    const Amplifier* amp_;
};

} // namespace vxc
