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

    // ------------------------------------------------------------------
    // Coarse generation path (M2 perf: outer-ring LOD chunks). A level-L
    // brick is generated DIRECTLY at its own resolution instead of
    // materializing and downsampling its 8^L level-0 descendants (measured
    // on the UE worker-job replica: a cold level-4 chunk costs ~3,273 ms
    // through the fine mip path, ~95% of it level-0 brick fill + the
    // downsample chain + map overhead — see docs/status.md).
    //
    // Rule (worldgen-versioned like everything else, but a NEW path with
    // its OWN goldens — it does not feed or alter the fine mip chain in
    // voxelcore/mips.h): a level-L cell takes the material of the
    // REPRESENTATIVE level-0 voxel at the centre of its S^3 footprint
    // (S = 2^L), i.e. cell index c on each axis samples level-0 index
    // c*S + S/2. This is a nearest-neighbour reduction, not the mip
    // majority vote: deterministic (a pure composition of the same integer
    // functions the fine path uses — Amplifier::column + materialAt, so
    // caves, caverns, bedrock and biome materials all participate), but
    // NOT in general equal to downsampling a full-resolution generation.
    // At level 0, S/2 == 0 makes it bit-identical to makeBrick/columns()
    // by construction (pinned by test_coarsegen.cpp), so one rule serves
    // every level. Centre (not corner) sampling is deliberate: it has no
    // systematic lateral shift at any level, so coarse levels agree with
    // the fine world and with EACH OTHER in expectation (corner sampling
    // would translate features by S/2, a different shift per level).
    //
    // Choosing the representative voxel's own centre (offset S/2 voxels
    // + the fine half-voxel) over the coarse cell's geometric centre costs
    // a constant +50 mm z bias at every level — half a level-0 voxel,
    // below the coarse quantization itself — and buys exact reuse of
    // materialAt (no new solidity rule to mirror in HLSL later).

    // Representative level-0 coordinate for coarse cell index `c` at
    // `level` (identity at level 0).
    static constexpr int64_t coarseRep(int64_t c, int32_t level) {
        const int64_t s = int64_t(1) << level;
        return c * s + s / 2;
    }

    // Column grid for a level-`level` brick footprint: B*B amplifier
    // columns at the representative xy of each coarse cell. Same cost as
    // a level-0 columns() call at ANY level. coarseColumns(0, ...) ==
    // columns(...) bit-identically.
    ColumnGrid coarseColumns(int32_t level, int32_t bx, int32_t by) const {
        ColumnGrid g;
        g.bx = bx;
        g.by = by;
        for (int y = 0; y < B; ++y)
            for (int x = 0; x < B; ++x)
                g.cols[x + B * y] = amp_->column(coarseRep(int64_t(bx) * B + x, level),
                                                 coarseRep(int64_t(by) * B + y, level));
        return g;
    }

    // Level-`level` brick at `key` (level-L brick coords), generated at
    // its own resolution from a coarseColumns(level, key.x, key.y) grid.
    // makeCoarseBrick(0, key, grid) == makeBrick(key, grid) bit-identically.
    Brick<B> makeCoarseBrick(int32_t level, const BrickKey& key, const ColumnGrid& grid) const {
        Brick<B> brick;
        for (int y = 0; y < B; ++y)
            for (int x = 0; x < B; ++x) {
                const ColumnSample& col = grid.at(x, y);
                for (int z = 0; z < B; ++z)
                    brick.set(x, y, z,
                              Amplifier::materialAt(col, coarseRep(int64_t(key.z) * B + z, level)));
            }
        brick.tryCollapse();
        return brick;
    }

    Brick<B> makeCoarseBrick(int32_t level, const BrickKey& key) const {
        return makeCoarseBrick(level, key, coarseColumns(level, key.x, key.y));
    }

    // Level-L brick-z range intersecting the surface shell for a coarse
    // grid: every level-L brick containing some column's topmost SOLID
    // coarse cell under the representative-sample rule (cell vz solid iff
    // the centre of voxel coarseRep(vz) is at or below the surface).
    // Reduces to surfaceBrickRange at level 0.
    void coarseSurfaceBrickRange(int32_t level, const ColumnGrid& grid, int32_t& bzMin,
                                 int32_t& bzMax) const {
        const int64_t s = int64_t(1) << level;
        int64_t vzMin = INT64_MAX, vzMax = INT64_MIN;
        for (int i = 0; i < B * B; ++i) {
            const int64_t top0 =
                floorDiv(grid.cols[i].surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            const int64_t top = floorDiv(top0 - s / 2, s);
            vzMin = top < vzMin ? top : vzMin;
            vzMax = top > vzMax ? top : vzMax;
        }
        bzMin = static_cast<int32_t>(floorDiv(vzMin, B));
        bzMax = static_cast<int32_t>(floorDiv(vzMax, B));
    }

private:
    const Amplifier* amp_;
};

} // namespace vxc
