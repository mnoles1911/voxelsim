#pragma once
// A synthetic VXM2 encoder for the manifest and bank tests.
//
// AN INDEPENDENT SPELLING OF THE FORMAT, deliberately: it mirrors
// forge/manifest.py's struct layout by hand, so a disagreement between this
// file and assetmanifest.cpp is a real two-sided disagreement about the wire
// -- the same reason test_assetgrid.cpp carries its own VXA encoder. It is
// never used to certify the exporter (the fixture test does that against a
// file asset-forge actually wrote); it exists so the refusal paths can be
// driven by corrupting known offsets in a well-formed blob.
//
// Layout offsets (kept in comments where the corruption tests poke them):
//   header 32 B:  magic 0, version 4, biomeCount 8, layerCount 12,
//                 speciesCount 16, recordBytes 20, ruleCount 24,
//                 attachCount 28
//   biome names:  32 .. 32 + 10*16                    (= 192)
//   layer table:  192 .. 192 + 4*24                   (= 288)
//   density:      288 .. 288 + 10*10*2                (= 488) kind-major u16
//   rules:        488 + i*64
//   species:      488 + rules*64 + i*152
//   attachments:  ... + species*152 + i*8

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "voxelcore/assetmanifest.h"
#include "voxelcore/assetplacement.h"
#include "voxelcore/biome.h"

namespace vxmtest {

inline void putU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v & 0xffu));
    b.push_back(uint8_t((v >> 8) & 0xffu));
}
inline void putU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v & 0xffu));
    b.push_back(uint8_t((v >> 8) & 0xffu));
    b.push_back(uint8_t((v >> 16) & 0xffu));
    b.push_back(uint8_t((v >> 24) & 0xffu));
}
inline void putI32(std::vector<uint8_t>& b, int32_t v) { putU32(b, uint32_t(v)); }

struct VxmSpecies {
    std::string name = "test-oak";
    uint8_t kind = 0; // tree
    uint8_t layer = 1;
    uint8_t flags = 1; // terrain lattice
    uint8_t waterKind = 0;
    uint8_t waterMask = 0x0e;
    uint16_t seedsBaked = 4;
    uint32_t voxelSizeMm = 100;
    uint16_t weights[vxc::kBiomeCount] = {};
    uint16_t abundanceQ10 = 1024;
    uint16_t clusterQ10 = 0;
    int32_t spacingMm = 6000;
    int32_t elevMinMm = -10'000;
    int32_t elevMaxMm = 2'000'000;
    int32_t slopeMinMmPerM = 0;
    int32_t slopeMaxMmPerM = 700;
    int32_t waterMaxMm = 0;
    int32_t heightMm = 9'000;
    int32_t depthMm = 0;
    int32_t depthMinMm = 300;
    int32_t depthMaxMm = 6'000;
    int32_t minWaterDepthMm = 500;
    uint16_t groupMin = 1;
    uint16_t groupMax = 1;
    int32_t groupRadiusMm = 0;
};

// The production-shaped default table: values match forge/manifest.py's
// LAYERS so a test species that fits here fits there.
inline std::vector<vxc::AssetLayer> defaultLayers() {
    std::vector<vxc::AssetLayer> L(4);
    L[0] = {24'000, 60'000, 8'000, 15'000, 60, 4, true};
    L[1] = {5'000, 34'000, 4'000, 12'000, 1000, 4, true};
    L[2] = {2'200, 7'500, 2'000, 6'000, 1000, 4, true};
    L[3] = {800, 2'500, 500, 1'500, 1000, 4, false};
    return L;
}

inline const char* kBiomeNames[vxc::kBiomeCount] = {
    "ocean",  "beach",   "grassland", "temperate_forest", "rainforest",
    "desert", "savanna", "taiga",     "tundra_alpine",    "bare_rock",
};

// A named placement rule, wire-shaped. Unmasked fields must stay zero.
struct VxmRule {
    std::string name = "test-rule";
    uint16_t fieldMask = 0;
    uint16_t abundanceQ10 = 0;
    uint16_t clusterQ10 = 0;
    uint8_t waterKind = 0;
    uint8_t waterMask = 0;
    int32_t elevMinMm = 0;
    int32_t elevMaxMm = 0;
    int32_t slopeMinMmPerM = 0;
    int32_t slopeMaxMmPerM = 0;
    int32_t waterMaxMm = 0;
    int32_t spacingMm = 0;
};

// One attachment: (species row, biome) obeys rule row.
struct VxmAttach {
    uint16_t speciesIndex = 0;
    uint8_t biome = 0;
    uint16_t ruleIndex = 0;
};

// The neutral kind x biome density table: 1000 everywhere = today's world.
inline std::vector<uint16_t> neutralDensity() {
    return std::vector<uint16_t>(vxc::kAssetKindCount * vxc::kBiomeCount, 1000u);
}

inline std::vector<uint8_t> buildVxm(const std::vector<VxmSpecies>& species,
                                     const std::vector<vxc::AssetLayer>& layers =
                                         defaultLayers(),
                                     const std::vector<uint16_t>& density = neutralDensity(),
                                     const std::vector<VxmRule>& rules = {},
                                     const std::vector<VxmAttach>& attachments = {}) {
    std::vector<uint8_t> b;
    putU32(b, 0x314D5856u); // "VXM1"
    putU32(b, 2u);
    putU32(b, vxc::kBiomeCount);
    putU32(b, uint32_t(layers.size()));
    putU32(b, uint32_t(species.size()));
    putU32(b, 152u);
    putU32(b, uint32_t(rules.size()));
    putU32(b, uint32_t(attachments.size()));
    for (uint32_t i = 0; i < vxc::kBiomeCount; ++i) {
        const char* n = kBiomeNames[i];
        const size_t len = std::strlen(n);
        for (size_t c = 0; c < 16; ++c) b.push_back(c < len ? uint8_t(n[c]) : 0u);
    }
    for (const vxc::AssetLayer& L : layers) {
        putI32(b, L.cellMm);
        putI32(b, L.maxHeightMm);
        putI32(b, L.maxDepthMm);
        putI32(b, L.maxRadiusMm);
        putU16(b, L.densityPerMille);
        putU16(b, L.seedCount);
        b.push_back(L.terrainLattice ? 1u : 0u);
        b.push_back(0u);
        b.push_back(0u);
        b.push_back(0u);
    }
    for (uint16_t d : density) putU16(b, d);
    for (const VxmRule& r : rules) {
        for (size_t c = 0; c < 32; ++c)
            b.push_back(c < r.name.size() ? uint8_t(r.name[c]) : 0u);
        putU16(b, r.fieldMask);
        putU16(b, r.abundanceQ10);
        putU16(b, r.clusterQ10);
        b.push_back(r.waterKind);
        b.push_back(r.waterMask);
        putI32(b, r.elevMinMm);
        putI32(b, r.elevMaxMm);
        putI32(b, r.slopeMinMmPerM);
        putI32(b, r.slopeMaxMmPerM);
        putI32(b, r.waterMaxMm);
        putI32(b, r.spacingMm);
    }
    for (const VxmSpecies& s : species) {
        for (size_t c = 0; c < 64; ++c)
            b.push_back(c < s.name.size() ? uint8_t(s.name[c]) : 0u);
        b.push_back(s.kind);
        b.push_back(s.layer);
        b.push_back(s.flags);
        b.push_back(s.waterKind);
        b.push_back(s.waterMask);
        b.push_back(0u);
        putU16(b, s.seedsBaked);
        putU32(b, s.voxelSizeMm);
        for (uint32_t w = 0; w < vxc::kBiomeCount; ++w) putU16(b, s.weights[w]);
        putU16(b, s.abundanceQ10);
        putU16(b, s.clusterQ10);
        putI32(b, s.spacingMm);
        putI32(b, s.elevMinMm);
        putI32(b, s.elevMaxMm);
        putI32(b, s.slopeMinMmPerM);
        putI32(b, s.slopeMaxMmPerM);
        putI32(b, s.waterMaxMm);
        putI32(b, s.heightMm);
        putI32(b, s.depthMm);
        putI32(b, s.depthMinMm);
        putI32(b, s.depthMaxMm);
        putI32(b, s.minWaterDepthMm);
        putU16(b, s.groupMin);
        putU16(b, s.groupMax);
        putI32(b, s.groupRadiusMm);
    }
    for (const VxmAttach& a : attachments) {
        putU16(b, a.speciesIndex);
        b.push_back(a.biome);
        b.push_back(0u);
        putU16(b, a.ruleIndex);
        putU16(b, 0u);
    }
    return b;
}

} // namespace vxmtest
