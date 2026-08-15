#pragma once
// ASSETS AS A THIRD TERM IN THE WORLD FUNCTION, and the one place it is
// composed.
//
// ---------------------------------------------------------------------------
// WHY THIS IS IN voxel-core AND NOT IN THE UE MESHER'S SAMPLER
// ---------------------------------------------------------------------------
//
// docs/asset-streaming-design.md section 6.3 says to wire the asset sample into
// the UE level-0 worker's GridSampler, after Amplifier::materialAt. That is
// NECESSARY AND NOT SUFFICIENT, and doing only it produces two defects that are
// both silent:
//
//   1. DIGGING ONE VOXEL OF A TREE DELETES THE REST OF THAT BRICK'S TREE.
//      World::applyToOverlay (world.h:125-127) materialises the overlay brick
//      with `gen_.makeBrick(e.key)` and then applies the edit cells. If
//      makeBrick does not contain the asset term, the FIRST edit anywhere in an
//      asset-bearing brick regenerates that 8^3 region as bare terrain and the
//      tree's voxels inside it vanish -- silently, persisted to the edit log,
//      and replicated to every client.
//
//   2. A TREE THAT RENDERS BUT IS NOT THERE. World::materialAt (world.h:46-52)
//      is what feeds IsSolidAtVoxel, the raycasts, the region graph and
//      collapse. An asset present only in the mesher's sampler is a tree you
//      can see, walk through, shoot through and cannot dig.
//
// So the composition point is here, inside GeneratedWorld, and the UE worker
// consults the same object rather than a parallel one. That also puts assets
// inside vxc_bench --digest and the golden tests, which is where a change whose
// failure mode is silent belongs.
//
// ---------------------------------------------------------------------------
// PRECEDENCE: overlay > asset > terrain, AND AN ASSET ONLY EVER FILLS AIR
// ---------------------------------------------------------------------------
//
// An asset voxel is written only where the terrain answered MAT_AIR. Three
// things follow, and the third is the one that matters:
//
//   * A part-buried rock is buried. Its below-ground voxels lose to the ground
//     they are inside, which is what "part-buried" means.
//   * MAT_BEDROCK is never replaced, without a special case for it: bedrock is
//     not air.
//   * THE COMPOSITION IS MONOTONE -- assets only ever ADD solid. That is
//     exactly the premise assetplacement.h's bound rests on ("an asset is solid
//     ABOVE the surface"), so a composition that could also REMOVE solid would
//     invalidate the ALL-SOLID floor bound (solidBelowBoundMm) while leaving
//     the sky bound looking fine. Nothing here can.
//
// ---------------------------------------------------------------------------
// WHY THE BANK IS AN INTERFACE
// ---------------------------------------------------------------------------
//
// Decoding a (species, seed) bank means file I/O, a search path and a cache
// policy, none of which voxel-core has or wants. IAssetBankSource is the seam:
// the host owns the library, this file owns the composition, and the tests own
// a synthetic source that returns hand-built grids with known voxels. Without
// that seam this whole file could only be exercised through a baked library
// that does not exist yet, which is how a gate ends up tested in one direction
// (tilestreaming.h:173-188).

#include <cstdint>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/assetgrid.h"
#include "voxelcore/assetplacement.h"
#include "voxelcore/assetpolicy.h"
#include "voxelcore/core.h"

namespace vxc {

// Where a decoded (species, seed) comes from. Returns nullptr for anything it
// does not have, which composes as "no asset voxels here" rather than as an
// error: a bank that failed to load must leave the world it was going to stand
// in intact.
class IAssetBankSource {
public:
    virtual ~IAssetBankSource() = default;
    virtual const AssetGrid* bankGrid(uint16_t bankId, uint16_t seedIndex) const = 0;
};

// THE BINDING THAT MAKES THE ANCHOR CHECK IMPOSSIBLE TO FORGET.
//
// AssetColumnFacts::anchorSolid defaults to false and assetResolveSite refuses
// without it, so the only question is whether anything ever sets it correctly.
// This is that thing, and it is one function so there is exactly one answer:
// topSolidVoxelZ gives the voxel surfaceBrickRange would materialise, and
// Amplifier::materialAt says whether the carve passes left anything in it.
//
// A cave mouth is a column with a perfectly good surfaceMm whose top voxel is
// the open shaft. Nothing about surfaceMm says so, which is why this is a
// second call and not an inference.
inline AssetColumnFacts assetColumnFactsFromSample(const ColumnSample& col) {
    AssetColumnFacts f;
    f.known = true;
    f.biome = col.biome;
    f.surfaceMm = col.surfaceMm;
    f.slopeMmPerM = col.slopeMmPerM;
    f.anchorSolid = Amplifier::materialAt(col, topSolidVoxelZ(col.surfaceMm)) != MAT_AIR;
    // Distance to water is not servable from a column -- nothing in voxel-core
    // answers "how far to the nearest watercourse" (design doc section 9). It
    // is left at the sentinel so a species that needs it is refused rather than
    // placed on an assumption.
    f.distanceToWaterMm = kAssetNoWaterDistanceMm;
    return f;
}

// The layer table, the species table, the banks and the policy, as one object.
//
// Held by value in GeneratedWorld as a POINTER, and optional: a world with no
// AssetField is exactly the world that exists today, bit for bit, which is what
// keeps every existing golden and the worldgen digest valid until the field is
// deliberately switched on.
class AssetField {
public:
    AssetField() = default;

    void setLayers(const AssetLayer* layers, int count) {
        layers_.assign(layers, layers + (count < kAssetLayerCount ? count : kAssetLayerCount));
    }
    void setSpecies(const AssetSpecies* species, int count) {
        species_.assign(species, species + count);
    }
    void setBankSource(const IAssetBankSource* banks) { banks_ = banks; }
    void setSeed(uint64_t seed) { seed_ = seed; }

    const std::vector<AssetLayer>& layers() const { return layers_; }
    const std::vector<AssetSpecies>& species() const { return species_; }
    uint64_t seed() const { return seed_; }
    bool empty() const { return layers_.empty() || species_.empty() || banks_ == nullptr; }

    // Every instance that can put a voxel inside `rect`.
    //
    // `columnFacts` is any callable (int64_t vx, int64_t vy) -> AssetColumnFacts.
    // Templated rather than taking an Amplifier for the reason
    // assetAwareSurfaceUpperBoundMm is templated: the failure being guarded is a
    // tree standing on nothing, and a check that can only be run against real
    // terrain through a full amplifier is a check that will only ever be run
    // where the terrain is easy.
    //
    // ONE COLUMN PER SITE, NOT PER VOXEL. The column is evaluated at the site's
    // OWN anchor -- not at the query voxel -- because that is where the tree
    // stands. The rect's own columns are irrelevant to it.
    template <typename ColumnFactsFn>
    std::vector<AssetInstance> instancesForRect(const AssetVoxelRect& rect,
                                                const ColumnFactsFn& columnFacts) const {
        std::vector<AssetInstance> out;
        if (empty()) return out;
        const std::vector<AssetSite> sites =
            assetSitesForRect(seed_, layers_.data(), int(layers_.size()), rect);
        out.reserve(sites.size());
        for (const AssetSite& s : sites) {
            const int64_t avx = floorDiv(s.anchorXMm, int64_t(kVoxelSizeMm));
            const int64_t avy = floorDiv(s.anchorYMm, int64_t(kVoxelSizeMm));
            AssetInstance inst;
            if (!assetResolveSite(seed_, layers_.data(), int(layers_.size()), species_.data(),
                                  int(species_.size()), s, columnFacts(avx, avy), inst))
                continue;
            out.push_back(inst);
        }
        return out;
    }

    // The material one instance puts at a world voxel, or MAT_AIR.
    //
    // Only TERRAIN-LATTICE instances answer anything. A detail-lattice species
    // carries its own grid and its own transform and never enters the world
    // lattice (forge/kinds.py:44-51), so asking it for a world voxel is a
    // category error, and answering would put ground cover into the terrain
    // grid where the bound never accounted for it.
    MaterialId materialOfInstance(const AssetInstance& inst, int64_t vx, int64_t vy,
                                  int64_t vz) const {
        if (banks_ == nullptr) return MAT_AIR;
        if (size_t(inst.layer) >= layers_.size()) return MAT_AIR;
        if (!layers_[inst.layer].terrainLattice) return MAT_AIR;
        const AssetGrid* g = banks_->bankGrid(inst.bankId, inst.seedIndex);
        if (g == nullptr || !g->valid()) return MAT_AIR;
        // A grid baked at anything but the world lattice cannot be stamped, and
        // this is the last line of defence for that rather than the first --
        // assetSpeciesFits refuses it at load. Checked twice deliberately: the
        // load-time check is the one that reports a name, and this one is the
        // one that holds even if a table was assembled without it.
        if (!g->onTerrainLattice()) return MAT_AIR;

        const int64_t anchorVx = floorDiv(inst.anchorXMm, int64_t(kVoxelSizeMm));
        const int64_t anchorVy = floorDiv(inst.anchorYMm, int64_t(kVoxelSizeMm));
        // Local coordinates in the ROTATED box: subtract the anchor, then the
        // rotated origin. atYaw and rotatedOrigin* are one convention expressed
        // once, in assetgrid.h, so this cannot re-derive it differently.
        const int64_t rx = vx - anchorVx - int64_t(g->rotatedOriginX(inst.yawQuarter));
        const int64_t ry = vy - anchorVy - int64_t(g->rotatedOriginY(inst.yawQuarter));
        const int64_t rz = vz - inst.anchorVz - int64_t(g->originZ());
        // Out-of-range reads answer MAT_AIR inside AssetGrid by design (the
        // mesher's apron reads outside the box on every load), so the range
        // test here is only to keep the int32 narrowing honest.
        if (rx < INT32_MIN || rx > INT32_MAX || ry < INT32_MIN || ry > INT32_MAX ||
            rz < INT32_MIN || rz > INT32_MAX)
            return MAT_AIR;
        return g->atYaw(static_cast<int32_t>(rx), static_cast<int32_t>(ry),
                        static_cast<int32_t>(rz), inst.yawQuarter);
    }

    // The material the asset term puts at a voxel, over a resolved instance
    // list. First non-air wins; the list order is assetSitesForRect's, which is
    // deterministic (layer, then row-major cell), so two overlapping crowns
    // resolve the same way on every machine and in every session.
    MaterialId materialAt(const std::vector<AssetInstance>& instances, int64_t vx, int64_t vy,
                          int64_t vz) const {
        for (const AssetInstance& inst : instances) {
            const MaterialId m = materialOfInstance(inst, vx, vy, vz);
            if (m != MAT_AIR) return m;
        }
        return MAT_AIR;
    }

private:
    uint64_t seed_ = 0;
    std::vector<AssetLayer> layers_;
    std::vector<AssetSpecies> species_;
    const IAssetBankSource* banks_ = nullptr;
};

// NOTE ON DILATION, because the obvious helper here would be a bug.
//
// A caller asks instancesForRect about the brick's OWN rect and nothing wider.
// assetSitesForRect already dilates the cell enumeration by each layer's own
// maxRadiusMm and then tests each site's reach exactly (assetplacement.h:230-245),
// so an instance anchored outside the brick that reaches into it is already
// returned. Pre-dilating the rect here would enumerate a second time over a
// wider area and hand back instances that cannot touch the brick at all -- not
// wrong, but slower and, worse, it would make the reach look like the caller's
// responsibility when it is not.
//
// The dilation the caller DOES owe is a different one and lives elsewhere:
// assetAwareSurfaceUpperBoundMm dilates the TERRAIN bound query, because a tree
// anchored uphill stands on ground higher than anything in the rect.

} // namespace vxc
