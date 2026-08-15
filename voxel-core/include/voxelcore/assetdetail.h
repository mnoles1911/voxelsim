#pragma once
// DETAIL ENTITIES: herds, flocks and shoals -- and salt versus fresh.
//
// Classes 3 and 4 of the owner's four. A detail entity is NOT a function of
// position the way a tree is. asset-forge says what it is, in the file that
// states its shape (forge/spec.py:1310-1315): "'detail' means nothing about this
// individual is saved: it is spawned from (species, seed) when the player is
// near, and when it despawns it is gone."
//
// So the split this file makes is:
//
//   WHERE a species can occur, and how densely -- a pure function of
//   (seed, position). Two clients agree that this valley is deer country and
//   that this reach holds trout.
//
//   WHICH individuals exist right now -- runtime, never persisted, nobody's
//   business here.
//
// That is what keeps classes 3 and 4 entirely out of the worldgen version
// contract. Nothing in this file can change a voxel, there is no bound here and
// there is deliberately none to add: a detail entity carries its own grid and
// its own transform and never enters the world lattice, so it cannot break the
// all-air proof that assetplacement.h exists to protect.
//
// ---------------------------------------------------------------------------
// SALT VERSUS FRESH: FOUND, NOT INVENTED
// ---------------------------------------------------------------------------
//
// There is no salinity concept anywhere in voxel-core and there does not need to
// be. There are two independent water terms and they are distinguishable right
// up until the moment they are composed:
//
//   THE SEA is not baked, it IS the datum (lakes.h:1406-1410):
//       oceanSurfaceMmAt(groundMm) = groundMm < kSeaLevelMm ? kSeaLevelMm : kNoWaterMm
//   THE BAKED WATER is lakes and rivers, from IWaterSampler::waterSurfaceMmAtVoxel.
//
// and lakes.h:1411 composes them with max():
//       implicitWaterDatumMm(bakedSurfaceMm, groundMm)
//
// AFTER THAT LINE THE PROVENANCE IS GONE. The result is a plain int32_t of mm
// with nothing on it to say which term won. So the salinity question has to be
// asked of the two OPERANDS, and classifyWaterSalinity below is exactly that --
// the same two numbers implicitWaterDatumMm takes, read for which one won
// instead of for the maximum.
//
// TWO SHORTCUTS THIS FILE REFUSES, and both are tempting:
//
//   * NOT classifyBiome(...) == OCEAN. That gate fires at surfaceMm < -3000
//     (biome.h:216), not at < kSeaLevelMm, and the band [-3 m, 0) is salt water
//     that classifies BEACH. The two "ocean" definitions in this codebase have
//     never agreed. A fish spawner keyed to the biome would find no water in
//     the top three metres of every shore in the world -- which is the top three
//     metres of every shore the player ever walks along.
//
//   * NOT a salinity byte on ColumnSample. The information is not in the
//     amplifier: the baked water arrives through an IWaterSampler the amplifier
//     only holds in debug-marker mode (amplifier.h:67-73). A field that is
//     populated only sometimes is how waterSurfaceMm came to mean BOTH "marker
//     off" and "no water here", which its own comment calls deliberate and which
//     is not a thing to do twice.

#include <cstdint>
#include <vector>

#include "voxelcore/biome.h" // BiomeId / kBiomeCount, for the per-biome weights
#include "voxelcore/core.h"
#include "voxelcore/hash.h"
#include "voxelcore/lakes.h" // oceanSurfaceMmAt / implicitWaterDatumMm / kNoWaterMm

namespace vxc {

// What kind of water is over a column.
//
// FOUR VALUES, NOT TWO, and the fourth is not fussiness. A river mouth is the
// case implicitWaterDatumMm's own comment is written about (lakes.h:1417-1424):
// "every river mouth, where the reach's own datum descends to meet
// kSeaLevelMm ... at that point they are the same number". Both terms are
// present and equal there. Calling it salt keeps trout out of the last hundred
// metres of every river; calling it fresh puts a reef fish in the shallows.
// Naming it is the only answer that lets a species say which it tolerates.
enum class WaterSalinity : uint8_t {
    kNone = 0,  // dry
    kFresh,     // a lake or a river won the max()
    kBrackish,  // both terms present and equal: a river mouth, a tidal reach
    kSalt,      // the sea won, or is the only term
};

// A species' admission mask over the four. Bit per value so `detail.water`'s
// "any" is a mask of all of them and "ocean" is one bit -- rather than an enum
// the caller has to write a switch over, which is where a new value silently
// falls through to "allowed".
inline constexpr uint8_t kWaterMaskFresh = 1u << 1;
inline constexpr uint8_t kWaterMaskBrackish = 1u << 2;
inline constexpr uint8_t kWaterMaskSalt = 1u << 3;
inline constexpr uint8_t kWaterMaskAny = kWaterMaskFresh | kWaterMaskBrackish | kWaterMaskSalt;

constexpr uint8_t waterSalinityBit(WaterSalinity s) {
    return static_cast<uint8_t>(1u << static_cast<uint8_t>(s));
}

// WHICH TERM WON THE max() IN implicitWaterDatumMm.
//
// `groundMm` MUST be the worldgen amplified surface, never the edited overlay.
// That is not a preference, it is what makes a pit a player digs below sea
// level stay dry, and lakes.h:1379-1385 records it as the structural fix for a
// defect rather than a patch over one.
// WHAT THIS DELIBERATELY DOES NOT YET READ, and it is a real refinement rather
// than a gap in the reasoning. tilestore.h:241-248's BasinKind carries
// kBasinLakeTerminal -- an ENDORHEIC basin, one that holds water and does not
// overflow. On Earth that is the saline kind: the Caspian, the Great Salt Lake,
// Lake Eyre. So the wire already carries the bit that would keep trout out of a
// salt lake, and this function cannot see it because it takes a composed
// surface rather than the BasinEntry it came from.
//
// v1 treats every water-holding basin as fresh, which is right for the
// overwhelming majority of them. Carrying the terminal bit through to here is a
// manifest change plus one more operand, and it is written down rather than done
// so that whoever adds it knows the bit already exists and does not go looking
// for a salinity model.
constexpr WaterSalinity classifyWaterSalinity(int32_t groundMm, int32_t bakedSurfaceMm) {
    const int32_t sea = oceanSurfaceMmAt(groundMm);
    const bool hasSea = sea != kNoWaterMm;
    const bool hasBaked = bakedSurfaceMm != kNoWaterMm;
    if (!hasSea && !hasBaked) return WaterSalinity::kNone;
    if (!hasBaked) return WaterSalinity::kSalt;
    if (!hasSea) return WaterSalinity::kFresh;
    if (bakedSurfaceMm > sea) return WaterSalinity::kFresh;   // a coastal lake perched above
    if (bakedSurfaceMm < sea) return WaterSalinity::kSalt;    // the sea drowns the baked surface
    return WaterSalinity::kBrackish;                          // a river mouth
}

// What a water column says about itself. Filled from the same two numbers, so
// the datum here and the datum the water renderer uses are one composition and
// not two.
struct WaterColumnFacts {
    bool known = false;         // see AssetColumnFacts::known; same discipline
    int32_t groundMm = 0;       // worldgen amplified surface -- the BED
    int32_t bakedSurfaceMm = kNoWaterMm;

    WaterSalinity salinity() const { return classifyWaterSalinity(groundMm, bakedSurfaceMm); }
    int32_t surfaceMm() const { return implicitWaterDatumMm(bakedSurfaceMm, groundMm); }
    // Depth of the water column. Zero when dry, and never negative: a bed above
    // its own datum is not water an inch deep, it is dry ground.
    int32_t depthMm() const {
        const int32_t s = surfaceMm();
        if (s == kNoWaterMm || s <= groundMm) return 0;
        return s - groundMm;
    }
};

// The aquatic half of a species. Every field here already exists in asset-forge
// and every one of them is authored on all 106 fish and 18 cetaceans.
//
// THE DEPTH FIELDS ARE THE RIGHT SHAPE FOR AN AQUATIC PLANT TOO, and that is a
// decision rather than an accident. asset-forge/docs/aquatic-species.md section
// 3.3 establishes that detail.depth_min_m / depth_max_m / min_water_depth_m
// "already exist and already mean exactly the right thing" for a kelp, and that
// they are scoped kinds=('fish','cetacean') in a way that gates the APP'S
// SLIDERS and not `validate`. Depth below the water surface is one physical
// quantity; a kelp and a trout do not need two spellings of it, and two
// spellings is how the two get different answers. So this one record serves a
// fish, a whale, a kelp and a reef boulder, and the difference between them is
// only that a plant's band is its rooting depth and a fish's is a band it holds
// within. The forge-side scope widening is requested, not done, in
// docs/asset-placement-design.md section 8.3.
struct AssetAquaticSpecies {
    uint16_t bankId = 0;
    uint16_t weightPerMille[kBiomeCount] = {}; // biomes.* x placement.abundance

    // Which water this species may spawn in -- detail.water, as a mask.
    uint8_t waterMask = kWaterMaskAny;

    // Depth BELOW THE WATER SURFACE this species holds within. detail.depth_min_m
    // / depth_max_m.
    int32_t depthMinMm = 300;
    int32_t depthMaxMm = 6000;

    // Water shallower than this holds none of this species -- the gate that
    // keeps a pike out of a puddle. detail.min_water_depth_m.
    int32_t minWaterDepthMm = 500;

    // Shoal size and spread. detail.school_min / school_max / school_radius_m.
    uint16_t groupMin = 1;
    uint16_t groupMax = 8;
    int32_t groupRadiusMm = 2500;
};

// The land half. Same shape, minus the water and plus the ground.
struct AssetLandSpecies {
    uint16_t bankId = 0;
    uint16_t weightPerMille[kBiomeCount] = {};
    int32_t elevMinMm = -10'000;
    int32_t elevMaxMm = 2'000'000;
    int32_t slopeMinMmPerM = 0;
    int32_t slopeMaxMmPerM = 700; // a quadruped on bare rock: see biomes.py's 2026-08-15 note
    uint16_t groupMin = 1;
    uint16_t groupMax = 8;
    int32_t groupRadiusMm = 20'000;
};

// A candidate GROUP -- a herd, a flock, a shoal -- before anything has looked at
// the ground or the water. The same shape as an AssetSite and for the same
// reason: the half that needs no world query is computed first and separately.
struct DetailGroupSite {
    int64_t cellX = 0, cellY = 0;
    int64_t centreXMm = 0, centreYMm = 0;
    uint16_t count = 1;      // drawn between groupMin and groupMax
    uint16_t seedIndex = 0;  // the first of `count` CONSECUTIVE bank seeds

    friend bool operator==(const DetailGroupSite&, const DetailGroupSite&) = default;
};

// One placed individual. z is in the struct because there is no way to get one
// without it -- see the placement functions.
struct DetailMember {
    int64_t xMm = 0, yMm = 0;
    int32_t zMm = 0;
    uint16_t seedIndex = 0;
    uint8_t yawQuarter = 0;

    friend bool operator==(const DetailMember&, const DetailMember&) = default;
};

// The lattice groups sit on. Not one of assetplacement.h's four size classes: a
// herd's spacing has nothing to do with an asset's size, and reusing a size-class
// layer would tie "how far apart are the deer" to "how tall is a tree".
struct DetailGroupLayer {
    int32_t cellMm = 200'000;      // 200 m between candidate groups
    uint16_t densityPerMille = 1000;
};

// Whether cell (cx, cy) carries a group, and where its centre is. Pure: one hash
// for occupancy and the size draw, one for the jitter.
inline bool detailGroupInCell(uint64_t seed, const DetailGroupLayer& layer, uint16_t groupMin,
                              uint16_t groupMax, int64_t cx, int64_t cy, DetailGroupSite& out) {
    if (layer.cellMm <= 0 || layer.densityPerMille == 0) return false;
    const uint64_t h = hash2(seed, cx, cy, CH_DETAIL_GROUP);
    const uint64_t occ = (h >> 48) * 1000u >> 16; // [0, 999], same shape as assetSiteInCell
    if (occ >= uint64_t(layer.densityPerMille)) return false;

    const uint64_t j = hash2(seed, cx, cy, CH_DETAIL_MEMBER);
    const int64_t cellMm = int64_t(layer.cellMm);
    const int64_t jx = int64_t((j >> 48) & 0xffffu) * cellMm >> 16;
    const int64_t jy = int64_t((j >> 32) & 0xffffu) * cellMm >> 16;

    const uint16_t lo = groupMin == 0 ? uint16_t(1) : groupMin;
    const uint16_t hi = groupMax < lo ? lo : groupMax;
    const uint32_t span = uint32_t(hi) - uint32_t(lo) + 1u;

    out.cellX = cx;
    out.cellY = cy;
    out.centreXMm = cx * cellMm + jx;
    out.centreYMm = cy * cellMm + jy;
    // A SHOAL IS ONE DECISION (spec.py:1330-1332): N individuals from
    // consecutive seeds, so they vary the way the `variation` group says rather
    // than being N copies of one draw.
    out.count = static_cast<uint16_t>(lo + ((h >> 16) & 0xffffu) % span);
    out.seedIndex = static_cast<uint16_t>((h >> 32) & 0xffffu);
    return true;
}

// Every group whose CENTRE lies within `radiusMm` of (cxMm, cyMm) -- the despawn
// disc, typically.
//
// ENUMERATE WATER, DO NOT SEARCH FOR IT. The disc is walked cell by cell and
// every cell is asked; nothing here hunts outward for somewhere suitable. A
// spawner that searches finds a place eventually and so never reports finding
// nothing, which is how "the shoal spawned somewhere odd" becomes unobservable.
inline std::vector<DetailGroupSite> detailGroupsInDisc(uint64_t seed,
                                                       const DetailGroupLayer& layer,
                                                       uint16_t groupMin, uint16_t groupMax,
                                                       int64_t cxMm, int64_t cyMm,
                                                       int64_t radiusMm) {
    std::vector<DetailGroupSite> out;
    if (layer.cellMm <= 0 || radiusMm < 0) return out;
    const int64_t cell = int64_t(layer.cellMm);
    const int64_t c0x = floorDiv(cxMm - radiusMm, cell), c1x = floorDiv(cxMm + radiusMm, cell);
    const int64_t c0y = floorDiv(cyMm - radiusMm, cell), c1y = floorDiv(cyMm + radiusMm, cell);
    for (int64_t cy = c0y; cy <= c1y; ++cy)
        for (int64_t cx = c0x; cx <= c1x; ++cx) {
            DetailGroupSite g;
            if (!detailGroupInCell(seed, layer, groupMin, groupMax, cx, cy, g)) continue;
            const int64_t dx = g.centreXMm - cxMm, dy = g.centreYMm - cyMm;
            // Exact, in mm, and in int64 -- a 200 m radius squared is 4e10, far
            // inside int64, and doing it in int32 would overflow at 46 km.
            if (dx * dx + dy * dy > radiusMm * radiusMm) continue;
            out.push_back(g);
        }
    return out;
}

// Where member `i` of a group stands, horizontally. Split out from the two
// placement functions below because it is the half that does not need a world
// query, and because both of them need exactly this.
inline void detailMemberOffsetMm(uint64_t seed, const DetailGroupSite& g, uint16_t i,
                                 int32_t radiusMm, int64_t& outXMm, int64_t& outYMm) {
    const uint64_t h = hash2(seed ^ (uint64_t(i) + 1u) * 0x9E3779B97F4A7C15ull, g.cellX, g.cellY,
                             CH_DETAIL_MEMBER);
    // A square offset rather than a disc: rejection sampling is not available to
    // a pure hash, and a herd in a square of 20 m versus a circle of 20 m is not
    // a difference anybody can see. Said here so nobody "fixes" it later and
    // changes every herd's layout for nothing.
    const int64_t r = int64_t(radiusMm);
    outXMm = g.centreXMm + (int64_t((h >> 48) & 0xffffu) * 2 * r >> 16) - r;
    outYMm = g.centreYMm + (int64_t((h >> 32) & 0xffffu) * 2 * r >> 16) - r;
}

// --- placement, with the ground and the water verified ----------------------
//
// BOTH FUNCTIONS BELOW TAKE THE WORLD FACT AND RETURN false RATHER THAN A
// MEMBER. There is no overload that omits it. A land animal placed without a
// ground sample is an animal in the sky; a fish placed without a bed is a fish
// in the mud, and the second one is worse because water hides it.

// Place one member of a land group. `surfaceMm` is the ground at the member's
// OWN position, not the group centre's -- a herd on a hillside stands on the
// hillside, and using the centre's height would tilt every herd into the slope.
inline bool detailPlaceLandMember(uint64_t seed, const DetailGroupSite& g,
                                  const AssetLandSpecies& s, uint16_t i, bool groundKnown,
                                  int32_t surfaceMm, int64_t slopeMmPerM, BiomeId biome,
                                  DetailMember& out) {
    if (!groundKnown) return false;
    if (i >= g.count) return false;
    if (uint32_t(biome) >= uint32_t(kBiomeCount)) return false;
    if (s.weightPerMille[biome] == 0) return false;
    if (surfaceMm < s.elevMinMm || surfaceMm > s.elevMaxMm) return false;
    if (slopeMmPerM < int64_t(s.slopeMinMmPerM) || slopeMmPerM > int64_t(s.slopeMaxMmPerM))
        return false;
    detailMemberOffsetMm(seed, g, i, s.groupRadiusMm, out.xMm, out.yMm);
    out.zMm = surfaceMm; // READ, never derived
    out.seedIndex = static_cast<uint16_t>(g.seedIndex + i);
    const uint64_t h = hash2(seed ^ (uint64_t(i) + 1u), g.cellX, g.cellY, CH_DETAIL_MEMBER);
    out.yawQuarter = static_cast<uint8_t>(h & 3u);
    return true;
}

// Place one member of a shoal. Takes BOTH ends of the water column.
//
// THE DEPTH BAND IS CLAMPED INTO THE COLUMN, NOT TRUSTED. A species whose band
// is 20-80 m in 4 m of water is not an error to report, it is a species out of
// its habitat -- minWaterDepthMm is the gate that says so, and it runs first. If
// the band merely overhangs the bed (a bottom fish in slightly shallow water)
// the member is placed at the deepest legal point instead of below the mud. The
// post-condition is asserted rather than assumed: the returned z is strictly
// between bed and surface, and test_assetdetail.cpp sweeps for an escape.
inline bool detailPlaceWaterMember(uint64_t seed, const DetailGroupSite& g,
                                   const AssetAquaticSpecies& s, uint16_t i,
                                   const WaterColumnFacts& w, BiomeId biome, DetailMember& out) {
    if (!w.known) return false;
    if (i >= g.count) return false;
    if (uint32_t(biome) >= uint32_t(kBiomeCount)) return false;
    if (s.weightPerMille[biome] == 0) return false;

    const WaterSalinity sal = w.salinity();
    if (sal == WaterSalinity::kNone) return false;
    if ((s.waterMask & waterSalinityBit(sal)) == 0) return false;

    const int32_t depth = w.depthMm();
    if (depth < s.minWaterDepthMm) return false;
    // A column with less than one voxel of water holds nothing, whatever the
    // species asked for: there is nowhere strictly between the bed and the
    // surface to put anything.
    if (depth <= kVoxelSizeMm) return false;

    const int32_t surface = w.surfaceMm();
    // The band, clamped into the open water. Both ends are pulled inside by one
    // voxel so the result is STRICTLY between bed and surface -- a fish exactly
    // on the surface reads as a dead fish and one exactly on the bed is inside
    // the mud, and both are one rounding away from being outside the water
    // entirely.
    const int32_t hiZ = surface - kVoxelSizeMm;                       // just under the surface
    const int32_t loZ = w.groundMm + kVoxelSizeMm;                    // just off the bed
    //
    // BOTH ENDS ARE CLAMPED INTO [loZ, hiZ], AND THE FIRST CUT ONLY CLAMPED ONE
    // OF EACH. It pulled bandBot up to loZ and bandTop down to hiZ, then
    // restored the ordering with `if (bandBot > bandTop) bandBot = bandTop` --
    // which, for a deep-water species in shallow water, pushed bandBot straight
    // back DOWN to a bandTop that was still far below the bed. The fish came out
    // 18 m underground, in water, where nothing would ever have seen it.
    // detail_water_member_band_is_clamped_into_shallow_water caught it.
    int32_t bandTop = surface - s.depthMinMm;
    int32_t bandBot = surface - s.depthMaxMm;
    if (bandTop > hiZ) bandTop = hiZ;
    if (bandTop < loZ) bandTop = loZ;
    if (bandBot < loZ) bandBot = loZ;
    if (bandBot > hiZ) bandBot = hiZ;
    if (bandBot > bandTop) bandBot = bandTop;

    detailMemberOffsetMm(seed, g, i, s.groupRadiusMm, out.xMm, out.yMm);
    const uint64_t h = hash2(seed ^ ((uint64_t(i) + 1u) << 8), g.cellX, g.cellY, CH_DETAIL_MEMBER);
    const int64_t span = int64_t(bandTop) - int64_t(bandBot);
    out.zMm = static_cast<int32_t>(int64_t(bandBot) +
                                   (int64_t((h >> 48) & 0xffffu) * (span + 1) >> 16));
    if (out.zMm > bandTop) out.zMm = bandTop;
    out.seedIndex = static_cast<uint16_t>(g.seedIndex + i);
    out.yawQuarter = static_cast<uint8_t>(h & 3u);
    return true;
}

} // namespace vxc
