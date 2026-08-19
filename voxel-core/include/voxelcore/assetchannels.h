#pragma once
// THE CANONICAL BINDING from baked tiles to AssetColumnChannels -- the one
// spelling of "what the placement channels say at this world voxel", used by
// the census probe and the UE composition workers alike so the two cannot
// drift (placement is worldgen: same instances on every machine, or
// multiplayer diverges silently).
//
// Split out of assetfield.h so that file keeps its small include graph: this
// header pulls tilestore.h and lakes.h, which the field's own composition
// never needs -- a host that installs no samplers passes the default-
// constructed channels and gets the fail-closed world.
//
// THE THREE SOURCES, and why each is the one it is:
//
//   * FineTileSampler -- SECTION_PLACE_* (bake_ver 28): water distance, TWI,
//     talus, curvature, heat load. Absent planes (pre-28 tiles, missing
//     tiles) leave the sentinels, and the gate keeps failing closed.
//   * IWaterSampler -- the SAME composed lake/river datum the renderer draws
//     (LakeSampler's basin rows + RiverSampler's water plane through
//     CompositeWaterSampler). NOT ColumnSample::waterSurfaceMm: that is the
//     debug water marker, populated only when a marker sampler is installed,
//     which production never does -- reading it is how the first
//     standing-water veto shipped inert and the owner photographed trees in
//     the alpine lake (2026-08-17). The sea needs no sampler at all and is
//     composed inside assetColumnFactsFromSample from the column itself.
//   * climate ITileSampler -- the coarse climate tier, for the temperature
//     that positions the treeline (biomeTreelineMm). The fine tier carries no
//     climate; passing the FineTileSampler's own climate() delegate works too
//     when it was built over a coarse source.
//
// DETERMINISM: every answer below is a pure function of tile bytes at the
// anchor. The IWaterSampler must be the BAKED composition -- a ledger-adjusted
// datum (basinDatumMm with a live ledger) is runtime state and must not reach
// placement; pass the sampler in its baked configuration, which is its
// default.

#include <cstdint>

#include "voxelcore/assetfield.h"
#include "voxelcore/biome.h"
#include "voxelcore/core.h"
#include "voxelcore/lakes.h"
#include "voxelcore/tilestore.h"

namespace vxc {

// The distance plane's unknown code and the policy's fail-closed sentinel are
// the same number ON PURPOSE (a saturated distance and an unknown one are both
// "refuse the riparian gate"), and this is the pin that keeps them so.
static_assert(placementDistanceMm(kPlacementDistUnknown) == kAssetNoWaterDistanceMm,
              "the placement plane's unknown distance must BE the policy sentinel");
static_assert(placementTwiMilli(kPlacementTwiUnknown) == kAssetNoTwiMilli,
              "the placement plane's unknown TWI must BE the policy sentinel");

// All channels at one world VOXEL column. Any source may be null; each one's
// channels then stay at their fail-closed sentinels.
//
// NOT const-clean by design: the fine sampler decodes placement blocks into
// its cache on first touch and the water sampler decodes lazily too, so this
// carries their threading contract -- prewarm-then-share, or serialize.
inline AssetColumnChannels assetColumnChannelsAt(FineTileSampler* fine, IWaterSampler* water,
                                                 ITileSampler* climate, int64_t vx, int64_t vy) {
    AssetColumnChannels ch;
    if (fine != nullptr) {
        const FineTileSampler::FinePlacementSample p = fine->placementAtVoxel(vx, vy);
        if (p.valid) {
            ch.distanceToWaterMm = placementDistanceMm(p.distWater);
            ch.twiMilli = placementTwiMilli(p.twi);
            ch.talus = p.talus;
            ch.curv = p.curv;
            ch.heat = p.heat;
        }
    }
    if (water != nullptr) {
        // kNoWaterMm == kNoWaterMarkerMm (amplifier.cpp pins it), so the dry
        // answer flows through as the channels' own sentinel.
        ch.waterSurfaceMm = water->waterSurfaceMmAtVoxel(vx, vy);
    }
    if (climate != nullptr) {
        const int64_t pxMm = static_cast<int64_t>(climate->pixelSizeMm());
        if (pxMm > 0) {
            const ClimateSample c = climate->climate(floorDiv(vx * kVoxelSizeMm, pxMm),
                                                     floorDiv(vy * kVoxelSizeMm, pxMm));
            ch.treelineMm = biomeTreelineMm(c.temperature);
        }
    }
    return ch;
}

} // namespace vxc
