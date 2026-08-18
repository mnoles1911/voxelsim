#include "voxelcore/assetmanifest.h"

#include <cstring>

namespace vxc {

const char* assetManifestErrorText(AssetManifestError e) {
    switch (e) {
        case AssetManifestError::kOk: return "ok";
        case AssetManifestError::kTooSmall: return "blob smaller than a VXM1 header";
        case AssetManifestError::kBadMagic: return "not a VXM1 file (bad magic)";
        case AssetManifestError::kBadVersion: return "unsupported VXM version";
        case AssetManifestError::kBadBiomeCount:
            return "manifest biome axis does not match this build's kBiomeCount";
        case AssetManifestError::kBiomeOrderMismatch:
            return "manifest biome names do not match this build's BiomeId order by "
                   "spelling -- refusing, because weights indexed against the wrong "
                   "biome are all individually valid and all wrong";
        case AssetManifestError::kBadLayerCount:
            return "manifest layer table is not kAssetLayerCount rows";
        case AssetManifestError::kBadLayer: return "degenerate layer row (cell <= 0)";
        case AssetManifestError::kBadRecordSize:
            return "per-species record size differs from this build's -- a reader that "
                   "guessed at the tail would read the next species' name as this one's "
                   "group sizes";
        case AssetManifestError::kTruncated: return "species table truncated";
        case AssetManifestError::kBadKind: return "species kind byte outside the enum";
        case AssetManifestError::kBadName: return "species name empty or not ASCII";
        case AssetManifestError::kSpeciesMisfiled:
            return "a species fails assetSpeciesFits against its own manifest's layer "
                   "table; the exporter checks this at bake, so the file and the "
                   "exporter disagree and nothing in the file should be trusted";
    }
    return "unknown";
}

namespace {

// Little-endian readers, spelled out for the same reason assetgrid.cpp spells
// them out: a wire format read with host-endian punning is correct until the
// day it silently is not.
uint32_t readU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}
uint16_t readU16(const uint8_t* p) { return uint16_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8)); }
int32_t readI32(const uint8_t* p) { return static_cast<int32_t>(readU32(p)); }

// This build's BiomeId order, BY SPELLING. The manifest carries the same list
// and parse() refuses on the first difference. If BiomeId ever gains a value,
// this list, forge/manifest.py's BIOME_ORDER and kBiomeCount all move in one
// commit -- which is exactly the property the check exists to enforce.
constexpr const char* kBiomeNames[kBiomeCount] = {
    "ocean",     "beach",  "grassland", "temperate_forest", "rainforest",
    "desert",    "savanna", "taiga",    "tundra_alpine",    "bare_rock",
};

} // namespace

void AssetManifest::clear() {
    layers_.clear();
    species_.clear();
    misfiledName_.clear();
    misfiledWhy_ = AssetTableError::kOk;
}

AssetManifestError AssetManifest::parse(const uint8_t* blob, size_t bytes) {
    clear();
    if (blob == nullptr || bytes < kVxmHeaderBytes) return AssetManifestError::kTooSmall;
    if (readU32(blob) != kVxmMagic) return AssetManifestError::kBadMagic;
    if (readU32(blob + 4) != kVxmVersion) return AssetManifestError::kBadVersion;
    const uint32_t biomes = readU32(blob + 8);
    const uint32_t layers = readU32(blob + 12);
    const uint32_t count = readU32(blob + 16);
    const uint32_t recBytes = readU32(blob + 20);
    if (biomes != kBiomeCount) return AssetManifestError::kBadBiomeCount;
    if (layers != uint32_t(kAssetLayerCount)) return AssetManifestError::kBadLayerCount;
    if (recBytes != kVxmSpeciesBytes) return AssetManifestError::kBadRecordSize;

    const size_t need = kVxmHeaderBytes + size_t(biomes) * kVxmBiomeNameBytes +
                        size_t(layers) * kVxmLayerBytes + size_t(count) * kVxmSpeciesBytes;
    if (bytes < need) return AssetManifestError::kTruncated;

    // Biome names, verified by spelling AND position.
    const uint8_t* p = blob + kVxmHeaderBytes;
    for (uint32_t b = 0; b < biomes; ++b, p += kVxmBiomeNameBytes) {
        const char* expect = kBiomeNames[b];
        const size_t n = std::strlen(expect);
        // A name may fill the field exactly ("temperate_forest" is 16 bytes),
        // in which case there is no padding to verify.
        if (n > kVxmBiomeNameBytes) return AssetManifestError::kBiomeOrderMismatch;
        if (std::memcmp(p, expect, n) != 0) return AssetManifestError::kBiomeOrderMismatch;
        for (size_t i = n; i < kVxmBiomeNameBytes; ++i)
            if (p[i] != 0) return AssetManifestError::kBiomeOrderMismatch;
    }

    std::vector<AssetLayer> layerTable(layers);
    for (uint32_t li = 0; li < layers; ++li, p += kVxmLayerBytes) {
        AssetLayer& L = layerTable[li];
        L.cellMm = readI32(p + 0);
        L.maxHeightMm = readI32(p + 4);
        L.maxDepthMm = readI32(p + 8);
        L.maxRadiusMm = readI32(p + 12);
        L.densityPerMille = readU16(p + 16);
        L.seedCount = readU16(p + 18);
        L.terrainLattice = p[20] != 0;
        if (L.cellMm <= 0 || L.maxHeightMm < 0 || L.maxDepthMm < 0 || L.maxRadiusMm < 0)
            return AssetManifestError::kBadLayer;
    }

    std::vector<AssetManifestSpecies> table;
    table.reserve(count);
    for (uint32_t i = 0; i < count; ++i, p += kVxmSpeciesBytes) {
        AssetManifestSpecies s;
        // Name: NUL-padded ASCII, must be non-empty and printable. A name is a
        // path component on the bank search path, so anything else is refused
        // rather than handed to the filesystem.
        size_t n = 0;
        while (n < kVxmNameBytes && p[n] != 0) ++n;
        if (n == 0 || n >= kVxmNameBytes) return AssetManifestError::kBadName;
        for (size_t c = 0; c < n; ++c) {
            const uint8_t ch = p[c];
            const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                            ch == '-' || ch == '_' || (ch >= 'A' && ch <= 'Z');
            if (!ok) return AssetManifestError::kBadName;
        }
        for (size_t c = n; c < kVxmNameBytes; ++c)
            if (p[c] != 0) return AssetManifestError::kBadName;
        s.name.assign(reinterpret_cast<const char*>(p), n);

        const uint8_t kind = p[64];
        if (kind >= kAssetKindCount) return AssetManifestError::kBadKind;
        s.kind = static_cast<AssetKind>(kind);
        s.layer = p[65];
        if (s.layer != kAssetLayerNotScattered && s.layer >= layers)
            return AssetManifestError::kBadLayer;
        s.terrainLattice = (p[66] & 1u) != 0;
        if (p[67] >= kAssetWaterKindCount) return AssetManifestError::kBadKind;
        s.waterKind = static_cast<AssetWaterKind>(p[67]);
        s.waterMask = p[68];
        // p[69] is pad.
        s.seedsBaked = readU16(p + 70);
        s.voxelSizeMm = readU32(p + 72);
        for (uint32_t b = 0; b < kBiomeCount; ++b)
            s.biomeWeightPerMille[b] = readU16(p + 76 + 2 * b);
        s.abundanceQ10 = readU16(p + 96);
        s.clusterQ10 = readU16(p + 98);
        s.spacingMm = readI32(p + 100);
        s.elevMinMm = readI32(p + 104);
        s.elevMaxMm = readI32(p + 108);
        s.slopeMinMmPerM = readI32(p + 112);
        s.slopeMaxMmPerM = readI32(p + 116);
        s.waterMaxMm = readI32(p + 120);
        s.heightMm = readI32(p + 124);
        s.depthMm = readI32(p + 128);
        s.depthMinMm = readI32(p + 132);
        s.depthMaxMm = readI32(p + 136);
        s.minWaterDepthMm = readI32(p + 140);
        s.groupMin = readU16(p + 144);
        s.groupMax = readU16(p + 146);
        s.groupRadiusMm = readI32(p + 148);
        table.push_back(std::move(s));
    }

    // Semantic check: every scattered species against its own file's layer
    // table, with the SERVED spacing (authored, floored at the cell pitch --
    // the exporter reports each floor by name; here it is simply the number
    // the world will actually use).
    for (const AssetManifestSpecies& s : table) {
        if (s.layer == kAssetLayerNotScattered) continue;
        AssetSpecies probe;
        probe.layer = s.layer;
        probe.heightMm = s.heightMm;
        probe.depthMm = s.depthMm;
        probe.voxelSizeMm = s.voxelSizeMm;
        for (uint32_t b = 0; b < kBiomeCount; ++b)
            probe.weightPerMille[b] = s.biomeWeightPerMille[b];
        const AssetLayer& L = layerTable[s.layer];
        const int32_t served = s.spacingMm < L.cellMm ? L.cellMm : s.spacingMm;
        // kNoBiome is legitimate here (a species authored dark everywhere is
        // an authoring statement, not a corrupt file); everything else means
        // the file disagrees with the exporter that claims to have checked it.
        const AssetTableError e =
            assetSpeciesFits(probe, layerTable.data(), int(layerTable.size()), served);
        if (e != AssetTableError::kOk && e != AssetTableError::kNoBiome) {
            misfiledName_ = s.name;
            misfiledWhy_ = e;
            return AssetManifestError::kSpeciesMisfiled;
        }
    }

    layers_ = std::move(layerTable);
    species_ = std::move(table);
    return AssetManifestError::kOk;
}

void assetTightenLayerCaps(const AssetManifest& m, std::vector<AssetLayer>& layers) {
    // Tallest BAKED occupant per layer. seedsBaked > 0 because a species with no
    // grids on disk composes nothing and must not hold a layer's ceiling up;
    // terrainLattice because only those reach the world grid, and the detail
    // layer's cap is not a streaming bound (see assetTopAboveSurfaceMm's skip).
    int32_t tallest[kAssetLayerCount] = {};
    for (const AssetManifestSpecies& s : m.species()) {
        if (s.seedsBaked == 0 || !s.terrainLattice) continue;
        if (s.layer >= kAssetLayerCount) continue;
        if (s.heightMm > tallest[s.layer]) tallest[s.layer] = s.heightMm;
    }
    for (size_t li = 0; li < layers.size() && li < size_t(kAssetLayerCount); ++li) {
        AssetLayer& L = layers[li];
        if (!L.terrainLattice) continue;
        const int32_t t = tallest[li];
        // Never raise, and never collapse an unoccupied layer to zero: an empty
        // layer keeps its authored cap so a manifest that under-reports heights
        // degrades to today's behaviour instead of clipping crowns.
        if (t > 0 && t < L.maxHeightMm) L.maxHeightMm = t;
    }
}

AssetTableBuildStats assetSpeciesTableFromManifest(const AssetManifest& m,
                                                   std::vector<AssetSpecies>& speciesOut) {
    AssetTableBuildStats st;
    speciesOut.clear();
    const std::vector<AssetLayer>& layers = m.layers();
    const std::vector<AssetManifestSpecies>& rows = m.species();
    speciesOut.reserve(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        const AssetManifestSpecies& r = rows[i];
        if (r.layer == kAssetLayerNotScattered) {
            ++st.detailEntities;
            continue;
        }
        const AssetLayer& L = layers[r.layer];

        AssetSpecies s;
        // The bank reference is the manifest ROW INDEX; AssetBankLibrary maps
        // it back to the name. An index rather than a hash of the name so a
        // bank lookup cannot collide two species into one appearance.
        s.bankId = static_cast<uint16_t>(i);
        s.layer = r.layer;
        s.elevMinMm = r.elevMinMm;
        s.elevMaxMm = r.elevMaxMm;
        s.slopeMinMmPerM = r.slopeMinMmPerM;
        s.slopeMaxMmPerM = r.slopeMaxMmPerM;
        s.waterMaxMm = r.waterMaxMm;
        s.clusterQ10 = r.clusterQ10;
        s.heightMm = r.heightMm;
        s.depthMm = r.depthMm;
        s.voxelSizeMm = r.voxelSizeMm;

        // --- CHANNEL RESPONSES (worldgen v26): derived here, not authored ---
        //
        // THE SLOPE CURVE, sized inversely from height for trees and bushes
        // (owner rule 2026-08-17: "steeper slopes have small trees" -- soil
        // depth and windthrow bite the big crowns first): tall canopy tapers
        // out by 60% slope, mid by 75%, small by 90%, krummholz-scale holds
        // to 100%. A species that AUTHORED a deliberately low ceiling
        // (<= 30%) keeps it as its zero point -- a floodplain willow must not
        // climb a hillside because it is short. Everything else (rocks,
        // grass, reeds -- kinds whose authored band IS their habitat
        // statement) keeps the authored ceiling as the zero point and gains
        // only the taper below it. Deriving here rather than hand-editing 78
        // specs is the point: the curve moves with the species' own height,
        // and an authored override field can land in the spec later without
        // touching this default.
        {
            int32_t zero = r.slopeMaxMmPerM;
            const bool woody = r.kind == AssetKind::kTree || r.kind == AssetKind::kBush;
            if (woody && r.slopeMaxMmPerM > 300) {
                const int32_t derived = r.heightMm >= 15'000   ? 600
                                        : r.heightMm >= 8'000  ? 750
                                        : r.heightMm >= 3'000  ? 900
                                                               : 1000;
                if (derived > zero) zero = derived;
            }
            s.slopeMaxMmPerM = zero; // the curve's zero point -- see the field
            const int32_t full = zero * 2 / 3;
            s.slopeFullMmPerM = full < r.slopeMinMmPerM ? r.slopeMinMmPerM : full;
        }

        // MOISTURE AFFINITY, from what the author already said: a species
        // that binds itself to water (water_max_m > 0) is a hygrophile; one
        // whose weight lives mostly in the dry biomes is a xerophile; one
        // rooted in the rainforest leans wet. Reading the AUTHORED weights
        // (pre-fold) because abundance says how MANY, not how thirsty.
        {
            int64_t total = 0;
            for (uint32_t b = 0; b < kBiomeCount; ++b) total += r.biomeWeightPerMille[b];
            const int64_t dry = int64_t(r.biomeWeightPerMille[DESERT]) +
                                int64_t(r.biomeWeightPerMille[SAVANNA]);
            const int64_t wet = int64_t(r.biomeWeightPerMille[RAINFOREST]);
            if (r.waterMaxMm > 0) {
                s.moistureAffinity = 2;
            } else if (total > 0 && dry * 2 >= total) {
                s.moistureAffinity = -2;
            } else if (total > 0 && dry * 4 >= total) {
                s.moistureAffinity = -1;
            } else if (total > 0 && wet * 2 >= total) {
                s.moistureAffinity = 1;
            }
        }

        // TALUS: rocks concentrate below cliffs (boost-only; see the field's
        // comment). CURVATURE: rocks seek convex ridge noses and cliff bases,
        // the biggest trees seek concave deep-soil hollows -- the two ends of
        // the soil-depth gradient, one channel (research 3.3/4.2).
        s.talusAffinity = r.kind == AssetKind::kRock ? int8_t(1) : int8_t(0);
        s.curvatureAffinity = r.kind == AssetKind::kRock ? int8_t(-1)
                              : (r.kind == AssetKind::kTree && r.heightMm >= 15'000)
                                  ? int8_t(1)
                                  : int8_t(0);

        // THE FOLD. weight x abundance x (cell/spacing)^2, exact integer, one
        // rounding at the end (round-to-nearest: floor would shave every
        // species, and the fold's job is to preserve the authored population).
        // The spacing residual uses the SERVED spacing -- authored, floored at
        // the cell pitch -- so a species tighter than its lattice folds at 1.
        const int64_t cell = int64_t(L.cellMm);
        const int64_t spacing = r.spacingMm < L.cellMm ? cell : int64_t(r.spacingMm);
        bool anyAuthored = false;
        bool anyFolded = false;
        for (uint32_t b = 0; b < kBiomeCount; ++b) {
            const int64_t w = int64_t(r.biomeWeightPerMille[b]);
            if (w > 0) anyAuthored = true;
            const int64_t num = w * int64_t(r.abundanceQ10) * cell * cell;
            const int64_t den = 1024 * spacing * spacing;
            const int64_t folded = (num + den / 2) / den;
            s.weightPerMille[b] = static_cast<uint16_t>(folded > 1000 ? 1000 : folded);
            if (s.weightPerMille[b] > 0) anyFolded = true;
        }
        if (!anyAuthored) {
            ++st.noBiome;
            continue;
        }
        if (!anyFolded) {
            // Authored present, folds to nothing: per-mille resolution cannot
            // express this species' rarity on its layer's lattice. The
            // exporter names each one; this side keeps the count so a load
            // can assert the two agree.
            ++st.tooRare;
            continue;
        }
        if (r.seedsBaked == 0) ++st.withoutBanks;
        speciesOut.push_back(s);
        ++st.kept;
    }
    return st;
}

} // namespace vxc
