#pragma once
// Voxelization on demand (plan §3.1 step 4): fills sparse bricks from the
// amplifier. Only bricks intersecting the surface shell (or touched by edits)
// should be materialized by callers; underground stays implicit-solid.

#include "voxelcore/amplifier.h"
#include "voxelcore/assetfield.h"
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

    // THE THIRD TERM IN THE WORLD FUNCTION, and it is optional.
    //
    // With no field installed this class is bit-for-bit what it was: every
    // existing golden, every test and the worldgen digest are untouched until
    // somebody deliberately switches assets on. That is not politeness, it is
    // the only way to tell a wiring bug from a placement bug -- when the field
    // IS installed the digest MUST change, and an unchanged digest means the
    // field is not wired rather than that placement found nothing.
    //
    // It is composed HERE rather than in the UE mesher's sampler because
    // makeBrick is what World::applyToOverlay rebuilds an edited brick from,
    // and materialAt below is what raycasts, digging and the region graph read.
    // See assetfield.h's header for the two silent defects that composing it
    // only in the mesher produces.
    void setAssetField(const AssetField* field) { assets_ = field; }
    const AssetField* assetField() const { return assets_; }

    // THE CHANNEL SOURCE RIDES BESIDE THE FIELD, and every composition path in
    // this class (and every host-side parallel sampler that mirrors them) must
    // resolve facts through assetChannelsAt or the field's gates run on
    // sentinels: riparian species refused everywhere, the standing-water veto
    // inert, treeline/TWI/talus multipliers neutral -- the engine/probe split
    // of 2026-08-17. Null (the default) IS the sentinel world, kept for hosts
    // with no baked channels (tests, synthetic ground); with a source
    // installed the answer must be a pure function of (seed, tile bytes), so
    // installing one is a worldgen change exactly like installing the field.
    // Same bring-up rule as setAssetField: install before any worker reads.
    void setAssetChannelSource(IAssetChannelSource* src) { channels_ = src; }
    IAssetChannelSource* assetChannelSource() const { return channels_; }
    AssetColumnChannels assetChannelsAt(int64_t vx, int64_t vy) const {
        return channels_ != nullptr ? channels_->channelsAt(vx, vy) : AssetColumnChannels{};
    }

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

    // Every asset instance that can put a voxel in this brick, with each
    // instance's anchor column evaluated at ITS OWN xy -- not at the brick's.
    // A 9 m crown over a 0.8 m brick is usually anchored in a different brick
    // entirely, and resolving it against the brick's columns would put the tree
    // at the height of the ground under its own canopy edge.
    std::vector<AssetInstance> assetInstancesForBrick(const BrickKey& key) const {
        if (assets_ == nullptr || assets_->empty()) return {};
        const int64_t vx0 = int64_t(key.x) * B, vy0 = int64_t(key.y) * B;
        const AssetVoxelRect rect{vx0, vy0, vx0 + B - 1, vy0 + B - 1};
        return assets_->instancesForRect(rect, [this](int64_t vx, int64_t vy) {
            return assetColumnFactsFromSample(amp_->columnCached(vx, vy),
                                              assetChannelsAt(vx, vy));
        });
    }

    Brick<B> makeBrick(const BrickKey& key, const ColumnGrid& grid) const {
        Brick<B> brick;
        // Resolved once per brick, not once per voxel. Empty whenever no field
        // is installed, and the inner loop then costs one branch on an empty
        // vector -- which is what keeps the no-asset world exactly as fast and
        // exactly as bit-identical as it was.
        const std::vector<AssetInstance> insts = assetInstancesForBrick(key);
        for (int y = 0; y < B; ++y)
            for (int x = 0; x < B; ++x) {
                const ColumnSample& col = grid.at(x, y);
                for (int z = 0; z < B; ++z) {
                    const int64_t vz = int64_t(key.z) * B + z;
                    MaterialId m = Amplifier::materialAt(col, vz);
                    // AIR ONLY. An asset never replaces terrain, so a
                    // part-buried rock stays buried, MAT_BEDROCK is safe with
                    // no special case, and the composition is MONOTONE -- it
                    // can only add solid. That last one is load-bearing:
                    // assetplacement.h's bound assumes assets are solid ABOVE
                    // the surface, and a composition that could also remove
                    // solid would break the all-solid floor bound while the sky
                    // bound went on looking correct.
                    if (m == MAT_AIR && !insts.empty()) {
                        m = assets_->materialAt(insts, int64_t(key.x) * B + x,
                                                int64_t(key.y) * B + y, vz);
                    }
                    brick.set(x, y, z, m);
                }
            }
        brick.tryCollapse();
        return brick;
    }

    Brick<B> makeBrick(const BrickKey& key) const {
        return makeBrick(key, columns(key.x, key.y));
    }

    // Brick-z range intersecting the surface shell for footprint (bx, by):
    // every brick containing some column's topmost solid voxel.
    //
    // WIDENED BY THE 3D DENSITY ENVELOPE FROM v12 TO v19, AND NO LONGER. The
    // term is gone (core.h's v20 entry) and so is the widening, which is why
    // this reduces to "the topmost solid voxel, per column" again.
    //
    // Kept as a note because the widening was subtler than the term's envelope
    // and anything reintroducing a displacement has to re-derive both halves,
    // not just the first:
    //
    //   * the topmost solid voxel of a column lay anywhere in [top - b, top + b]
    //     because `centre <= surfaceMm + D`; and
    //   * more importantly, a brick BELOW the topmost solid voxel was no longer
    //     necessarily full. Inside the band a column could read air with solid
    //     above it, so the bricks holding the band's lower half stopped being
    //     homogeneous. A range that only tracked the top voxel would have left
    //     an overhang's underside as implicit-solid rock, never meshed.
    //
    // It was also widened PER COLUMN rather than per footprint, which is worth
    // keeping: applying the band unconditionally was sound, was the first cut,
    // and measured +38% cells over the vxc_gpu region set at the 700 mm
    // envelope — paid by flat ground that could never use it.
    void surfaceBrickRange(const ColumnGrid& grid, int32_t& bzMin, int32_t& bzMax) const {
        int64_t vzMin = INT64_MAX, vzMax = INT64_MIN;
        for (int i = 0; i < B * B; ++i) {
            // Topmost solid voxel: centre <= surfaceMm. The formula moved to
            // core.h's topSolidVoxelZ so that asset placement anchors trees to
            // the SAME voxel this materialises bricks for, rather than to a
            // second derivation of the same arithmetic.
            const int64_t top = topSolidVoxelZ(grid.cols[i].surfaceMm);
            vzMin = top < vzMin ? top : vzMin;
            vzMax = top > vzMax ? top : vzMax;
        }
        bzMin = static_cast<int32_t>(floorDiv(vzMin, B));
        bzMax = static_cast<int32_t>(floorDiv(vzMax, B));
    }

    // THE PER-VOXEL QUERY, and it must see assets or a tree is not a thing you
    // can dig, raycast, stand on or collapse. World::materialAt is this
    // function; it feeds IsSolidAtVoxel, the region graph and collapse.h.
    //
    // It costs a site enumeration over a one-voxel rect when a field is
    // installed and the terrain answered air. That is genuinely more expensive
    // than the batch path, and it is the same trade this function already makes
    // -- it re-derives a whole column per voxel, which is why the header above
    // tells batch callers to use makeBrick instead. Correctness here is not
    // optional: the alternative is geometry that renders and is not there.
    MaterialId materialAt(int64_t vx, int64_t vy, int64_t vz) const {
        const MaterialId m = amp_->materialAt(vx, vy, vz);
        if (m != MAT_AIR || assets_ == nullptr || assets_->empty()) return m;
        const AssetVoxelRect rect{vx, vy, vx, vy};
        const std::vector<AssetInstance> insts =
            assets_->instancesForRect(rect, [this](int64_t ax, int64_t ay) {
                return assetColumnFactsFromSample(amp_->columnCached(ax, ay),
                                                  assetChannelsAt(ax, ay));
            });
        if (insts.empty()) return MAT_AIR;
        return assets_->materialAt(insts, vx, vy, vz);
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
    //
    // surfacePreserve (backlog 0.0b): route each cell through
    // Amplifier::coarseSurfaceMaterialAt so the topmost solid cell of every
    // column takes the true level-0 surface voxel's material instead of the
    // representative sample up to a whole coarse cell below it -- the fix for
    // coarse rings browning out as thin snow/grass caps fall between
    // representatives. false (the default) is byte-identical to the historical
    // rule, and the helper is the identity at level 0 either way, so the
    // pinned makeCoarseBrick(0, ...) == makeBrick(...) tests stand untouched.
    // Solidity never changes in either mode -- see the helper's contract.
    Brick<B> makeCoarseBrick(int32_t level, const BrickKey& key, const ColumnGrid& grid,
                             bool surfacePreserve = false) const {
        Brick<B> brick;
        const int64_t s = int64_t(1) << level;
        for (int y = 0; y < B; ++y)
            for (int x = 0; x < B; ++x) {
                const ColumnSample& col = grid.at(x, y);
                for (int z = 0; z < B; ++z)
                    brick.set(x, y, z,
                              Amplifier::coarseSurfaceMaterialAt(
                                  col, coarseRep(int64_t(key.z) * B + z, level), s,
                                  surfacePreserve));
            }
        brick.tryCollapse();
        return brick;
    }

    Brick<B> makeCoarseBrick(int32_t level, const BrickKey& key,
                             bool surfacePreserve = false) const {
        return makeCoarseBrick(level, key, coarseColumns(level, key.x, key.y), surfacePreserve);
    }

    // Level-L brick-z range intersecting the surface shell for a coarse
    // grid: every level-L brick containing some column's topmost SOLID
    // coarse cell under the representative-sample rule (cell vz solid iff
    // the centre of voxel coarseRep(vz) is at or below the surface).
    // Reduces to surfaceBrickRange at level 0.
    //
    // v12: the same LEVEL-0 voxel band as surfaceBrickRange, mapped through
    // the same coarse-cell formula rather than through a separate one. At level
    // 0 the two expressions are identical by arithmetic (s == 1), which is what
    // keeps makeCoarseBrick(0, ...) == makeBrick(...) and its pinned test true.
    // At higher levels the band collapses to a cell or two, which is correct:
    // the representative-sample rule asks which coarse cell the top LEVEL-0
    // voxel falls in, so the band has to be widened in level-0 units first and
    // reduced afterwards -- widening by whole COARSE cells instead would over-cover
    // by a factor of 2^L for nothing.
    void coarseSurfaceBrickRange(int32_t level, const ColumnGrid& grid, int32_t& bzMin,
                                 int32_t& bzMax) const {
        const int64_t s = int64_t(1) << level;
        int64_t vzMin = INT64_MAX, vzMax = INT64_MIN;
        for (int i = 0; i < B * B; ++i) {
            const ColumnSample& col = grid.cols[i];
            const int64_t top0 =
                floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            const int64_t lo = floorDiv(top0 - s / 2, s);
            vzMin = lo < vzMin ? lo : vzMin;
            vzMax = lo > vzMax ? lo : vzMax;

            // THE DEBUG WATER MARKER STANDS ABOVE THE SURFACE, so this band has
            // to reach it or the bricks holding it are never generated.
            //
            // This is the other half of the widening Amplifier::surfaceUpperBoundMm
            // already pays for, and it was missing. The symptom is specific and
            // was reported exactly: a lake's shoreline draws magenta while its
            // MIDDLE is see-through, and flying closer makes it worse.
            //
            // The asymmetry is the tell. A level-0 brick is 8 voxels = 0.8 m, so
            // only water within 0.8 m of the bed shares the surface brick and
            // gets meshed -- the shallow rim. At level 5 a brick spans 8 coarse
            // cells of 3.2 m = 25.6 m and happens to swallow the whole water
            // column, so distant lakes look solid. Approaching one swaps coarse
            // for fine and hollows it out, which reads as chunks failing to load
            // and is nothing of the kind.
            //
            // Costs nothing when the marker is off: waterSurfaceMm is
            // kNoWaterMarkerMm on every column unless a sampler is installed.
            if (col.waterSurfaceMm != kNoWaterMarkerMm &&
                int64_t(col.waterSurfaceMm) > int64_t(col.surfaceMm)) {
                // CAPPED, and the cap is what makes this affordable. Reaching
                // the true surface of a 40 m lake means fifty 8-voxel bricks
                // where dry ground needs one, and the deepest water is a
                // basin's INTERIOR -- so the middle of every lake became fifty
                // times the work of its shoreline and stopped coming back at
                // all. Amplifier::column caps the marker to a
                // kWaterMarkerHeightMm slab for the same reason; this must
                // agree with it or the band and the fill disagree about which
                // bricks can hold magenta.
                const int64_t markerTopMm =
                    std::min(int64_t(col.waterSurfaceMm),
                             int64_t(col.surfaceMm) + kWaterMarkerHeightMm);
                const int64_t wtop0 =
                    floorDiv(markerTopMm - kVoxelSizeMm / 2, kVoxelSizeMm);
                const int64_t whi = floorDiv(wtop0 - s / 2, s);
                if (whi > vzMax) vzMax = whi;
            }
        }
        bzMin = static_cast<int32_t>(floorDiv(vzMin, B));
        bzMax = static_cast<int32_t>(floorDiv(vzMax, B));
    }

private:
    const Amplifier* amp_;
    // Null means "no assets", which is the world exactly as it was before this
    // existed. See setAssetField.
    const AssetField* assets_ = nullptr;
    // Null means "sentinel channels" -- the fail-closed world. See
    // setAssetChannelSource.
    IAssetChannelSource* channels_ = nullptr;
};

} // namespace vxc
