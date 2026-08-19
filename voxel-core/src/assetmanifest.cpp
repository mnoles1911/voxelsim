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
        case AssetManifestError::kBadDensity:
            return "a kind x biome density entry exceeds 1000 per-mille -- the table may "
                   "only thin (a policy may only veto a site), so a boost is not a value "
                   "this build honours";
        case AssetManifestError::kBadRule:
            return "a named placement rule is malformed (bad name, unknown or violated "
                   "field mask, non-positive spacing, inverted band, or water kind outside "
                   "the enum)";
        case AssetManifestError::kBadAttachment:
            return "a rule attachment is malformed (index out of range, unsorted or "
                   "duplicate, or on a species the scatter never places) or its (species, "
                   "biome)'s attached rules compose to a contradiction (empty band, empty "
                   "salinity mask, or disagreeing water kinds / cluster strengths) -- the "
                   "exporter refuses these by name, so the file and the exporter disagree";
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
    for (size_t i = 0; i < kAssetKindCount * kBiomeCount; ++i) classDensity_[i] = 0;
    rules_.clear();
    attachments_.clear();
    overrides_.clear();
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
    const uint32_t ruleCount = readU32(blob + 24);
    const uint32_t attachCount = readU32(blob + 28);
    if (biomes != kBiomeCount) return AssetManifestError::kBadBiomeCount;
    if (layers != uint32_t(kAssetLayerCount)) return AssetManifestError::kBadLayerCount;
    if (recBytes != kVxmSpeciesBytes) return AssetManifestError::kBadRecordSize;

    const size_t need = kVxmHeaderBytes + size_t(biomes) * kVxmBiomeNameBytes +
                        size_t(layers) * kVxmLayerBytes +
                        kAssetKindCount * kBiomeCount * 2u + size_t(ruleCount) * kVxmRuleBytes +
                        size_t(count) * kVxmSpeciesBytes + size_t(attachCount) * kVxmAttachBytes;
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

    // The kind x biome density table. Capped at 1000 BY REFUSAL, not by
    // clamping: a value above 1000 would be a per-biome density BOOST, and
    // the veto-only contract the streaming bound rests on says a policy may
    // only remove sites. Clamping would honour the intelligible half of a
    // request this build must not honour at all.
    uint16_t density[kAssetKindCount * kBiomeCount];
    for (size_t di = 0; di < kAssetKindCount * kBiomeCount; ++di, p += 2) {
        density[di] = readU16(p);
        if (density[di] > 1000u) return AssetManifestError::kBadDensity;
    }

    // The named rule library. Names obey the species-name charset (they are
    // report and UI text, and asset-forge resolves spec references to these
    // rows by name at export). Every structural rule below keeps the file at
    // ONE byte encoding per meaning.
    std::vector<AssetPlacementRule> ruleTable;
    ruleTable.reserve(ruleCount);
    for (uint32_t i = 0; i < ruleCount; ++i, p += kVxmRuleBytes) {
        AssetPlacementRule r;
        size_t n = 0;
        while (n < kVxmRuleNameBytes && p[n] != 0) ++n;
        if (n == 0 || n >= kVxmRuleNameBytes) return AssetManifestError::kBadRule;
        for (size_t c = 0; c < n; ++c) {
            const uint8_t ch = p[c];
            const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                            ch == '-' || ch == '_' || (ch >= 'A' && ch <= 'Z');
            if (!ok) return AssetManifestError::kBadRule;
        }
        for (size_t c = n; c < kVxmRuleNameBytes; ++c)
            if (p[c] != 0) return AssetManifestError::kBadRule;
        r.name.assign(reinterpret_cast<const char*>(p), n);

        r.fieldMask = readU16(p + 32);
        r.abundanceQ10 = readU16(p + 34);
        r.clusterQ10 = readU16(p + 36);
        const uint8_t waterKind = p[38];
        r.waterMask = p[39];
        r.elevMinMm = readI32(p + 40);
        r.elevMaxMm = readI32(p + 44);
        r.slopeMinMmPerM = readI32(p + 48);
        r.slopeMaxMmPerM = readI32(p + 52);
        r.waterMaxMm = readI32(p + 56);
        r.spacingMm = readI32(p + 60);

        if (r.fieldMask == 0 || (r.fieldMask & ~kOverrideFieldMaskAll) != 0)
            return AssetManifestError::kBadRule;
        if (waterKind >= kAssetWaterKindCount) return AssetManifestError::kBadRule;
        r.waterKind = static_cast<AssetWaterKind>(waterKind);
        // Unmasked fields must be zero -- one encoding per meaning.
        if (!(r.fieldMask & kOverrideElevMin) && r.elevMinMm != 0)
            return AssetManifestError::kBadRule;
        if (!(r.fieldMask & kOverrideElevMax) && r.elevMaxMm != 0)
            return AssetManifestError::kBadRule;
        if (!(r.fieldMask & kOverrideSlopeMin) && r.slopeMinMmPerM != 0)
            return AssetManifestError::kBadRule;
        if (!(r.fieldMask & kOverrideSlopeMax) && r.slopeMaxMmPerM != 0)
            return AssetManifestError::kBadRule;
        if (!(r.fieldMask & kOverrideWaterMax) && r.waterMaxMm != 0)
            return AssetManifestError::kBadRule;
        if ((r.fieldMask & kOverrideWaterMax) && r.waterMaxMm <= 0)
            return AssetManifestError::kBadRule;
        if (!(r.fieldMask & kOverrideWaterKind) && (waterKind != 0 || r.waterMask != 0))
            return AssetManifestError::kBadRule;
        if ((r.fieldMask & kOverrideWaterKind) && r.waterMask == 0)
            return AssetManifestError::kBadRule;
        if (!(r.fieldMask & kOverrideSpacing) && r.spacingMm != 0)
            return AssetManifestError::kBadRule;
        if ((r.fieldMask & kOverrideSpacing) && r.spacingMm <= 0)
            return AssetManifestError::kBadRule;
        if (!(r.fieldMask & kOverrideAbundance) && r.abundanceQ10 != 0)
            return AssetManifestError::kBadRule;
        if (!(r.fieldMask & kOverrideCluster) && r.clusterQ10 != 0)
            return AssetManifestError::kBadRule;
        // A band inverted INSIDE one rule can never compose to anything.
        if ((r.fieldMask & kOverrideElevMin) && (r.fieldMask & kOverrideElevMax) &&
            r.elevMinMm > r.elevMaxMm)
            return AssetManifestError::kBadRule;
        if ((r.fieldMask & kOverrideSlopeMin) && (r.fieldMask & kOverrideSlopeMax) &&
            r.slopeMinMmPerM > r.slopeMaxMmPerM)
            return AssetManifestError::kBadRule;
        ruleTable.push_back(std::move(r));
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

    // The attachments, then their composition. Attachments are sorted
    // strictly ascending by (species, biome, rule) -- sorted AND unique, and
    // the composition below can walk each (species, biome) as one run.
    std::vector<AssetRuleAttachment> attachTable;
    attachTable.reserve(attachCount);
    for (uint32_t i = 0; i < attachCount; ++i, p += kVxmAttachBytes) {
        AssetRuleAttachment a;
        a.speciesIndex = readU16(p + 0);
        a.biome = p[2];
        if (p[3] != 0) return AssetManifestError::kBadAttachment; // pad
        a.ruleIndex = readU16(p + 4);
        if (readU16(p + 6) != 0) return AssetManifestError::kBadAttachment; // pad
        if (a.speciesIndex >= count || a.biome >= kBiomeCount || a.ruleIndex >= ruleCount)
            return AssetManifestError::kBadAttachment;
        if (!attachTable.empty()) {
            const AssetRuleAttachment& prev = attachTable.back();
            const bool ascending =
                a.speciesIndex > prev.speciesIndex ||
                (a.speciesIndex == prev.speciesIndex &&
                 (a.biome > prev.biome ||
                  (a.biome == prev.biome && a.ruleIndex > prev.ruleIndex)));
            if (!ascending) return AssetManifestError::kBadAttachment;
        }
        // A rule can only bind a species the scatter places: a detail entity
        // (layer 255) has no gate this table reaches.
        if (table[a.speciesIndex].layer == kAssetLayerNotScattered)
            return AssetManifestError::kBadAttachment;
        attachTable.push_back(a);
    }

    // COMPOSE each (species, biome)'s attached rules with the species' own
    // defaults. The law is INTERSECTION -- strictest wins -- because every
    // rule is a restriction and the veto-only contract composes restrictions
    // as "all must pass":
    //
    //   elev band   [max of mins, min of maxes]      (intersection of bands)
    //   slope band  [max of mins, min of maxes]      (ditto)
    //   water_max   min of setters (base counts only if it set one, i.e. > 0)
    //   spacing     max of setters and the base       (sparser is stricter)
    //   abundance   min of setters and the base       (rarer is stricter)
    //   water kind  masks AND together (base's admission included); the KIND
    //               byte has no order, so all setters must AGREE on it
    //   cluster     no order either: at most one distinct value among setters
    //
    // A composition that comes out empty -- an inverted band, a zero salinity
    // mask -- is refused: the exporter checks the same law by rule NAME at
    // export, so an empty intersection here is a file the exporter did not
    // write.
    std::vector<AssetManifestOverride> ovTable;
    for (size_t i = 0; i < attachTable.size();) {
        const uint16_t sp = attachTable[i].speciesIndex;
        const uint8_t bio = attachTable[i].biome;
        const AssetManifestSpecies& base = table[sp];
        AssetManifestOverride o;
        o.speciesIndex = sp;
        o.biome = bio;
        o.elevMinMm = base.elevMinMm;
        o.elevMaxMm = base.elevMaxMm;
        o.slopeMinMmPerM = base.slopeMinMmPerM;
        o.slopeMaxMmPerM = base.slopeMaxMmPerM;
        o.waterMaxMm = base.waterMaxMm;
        o.spacingMm = base.spacingMm;
        o.abundanceQ10 = base.abundanceQ10;
        o.clusterQ10 = base.clusterQ10;
        o.waterKind = base.waterKind;
        o.waterMask = base.waterMask;
        bool haveWaterKind = false;
        bool haveCluster = false;
        for (; i < attachTable.size() && attachTable[i].speciesIndex == sp &&
               attachTable[i].biome == bio;
             ++i) {
            const AssetPlacementRule& r = ruleTable[attachTable[i].ruleIndex];
            o.fieldMask |= r.fieldMask;
            if (r.fieldMask & kOverrideElevMin)
                o.elevMinMm = r.elevMinMm > o.elevMinMm ? r.elevMinMm : o.elevMinMm;
            if (r.fieldMask & kOverrideElevMax)
                o.elevMaxMm = r.elevMaxMm < o.elevMaxMm ? r.elevMaxMm : o.elevMaxMm;
            if (r.fieldMask & kOverrideSlopeMin)
                o.slopeMinMmPerM =
                    r.slopeMinMmPerM > o.slopeMinMmPerM ? r.slopeMinMmPerM : o.slopeMinMmPerM;
            if (r.fieldMask & kOverrideSlopeMax)
                o.slopeMaxMmPerM =
                    r.slopeMaxMmPerM < o.slopeMaxMmPerM ? r.slopeMaxMmPerM : o.slopeMaxMmPerM;
            if (r.fieldMask & kOverrideWaterMax)
                o.waterMaxMm = (o.waterMaxMm <= 0 || r.waterMaxMm < o.waterMaxMm)
                                   ? r.waterMaxMm
                                   : o.waterMaxMm;
            if (r.fieldMask & kOverrideSpacing)
                o.spacingMm = r.spacingMm > o.spacingMm ? r.spacingMm : o.spacingMm;
            if (r.fieldMask & kOverrideAbundance)
                o.abundanceQ10 =
                    r.abundanceQ10 < o.abundanceQ10 ? r.abundanceQ10 : o.abundanceQ10;
            if (r.fieldMask & kOverrideWaterKind) {
                if (haveWaterKind && o.waterKind != r.waterKind)
                    return AssetManifestError::kBadAttachment;
                o.waterKind = r.waterKind;
                o.waterMask = uint8_t(o.waterMask & r.waterMask);
                haveWaterKind = true;
            }
            if (r.fieldMask & kOverrideCluster) {
                if (haveCluster && o.clusterQ10 != r.clusterQ10)
                    return AssetManifestError::kBadAttachment;
                o.clusterQ10 = r.clusterQ10;
                haveCluster = true;
            }
        }
        if (o.elevMinMm > o.elevMaxMm) return AssetManifestError::kBadAttachment;
        if (o.slopeMinMmPerM > o.slopeMaxMmPerM) return AssetManifestError::kBadAttachment;
        if (haveWaterKind && o.waterMask == 0) return AssetManifestError::kBadAttachment;
        ovTable.push_back(o);
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
    // And the same check for every override's effective row: a per-biome
    // spacing is still floored at the cell pitch, and nothing an override
    // says may un-fit a species from the layer it is filed on (height, depth
    // and lattice are NOT overridable, so only spacing can move here).
    for (const AssetManifestOverride& o : ovTable) {
        if (!(o.fieldMask & kOverrideSpacing)) continue;
        const AssetManifestSpecies& s = table[o.speciesIndex];
        AssetSpecies probe;
        probe.layer = s.layer;
        probe.heightMm = s.heightMm;
        probe.depthMm = s.depthMm;
        probe.voxelSizeMm = s.voxelSizeMm;
        for (uint32_t b = 0; b < kBiomeCount; ++b)
            probe.weightPerMille[b] = s.biomeWeightPerMille[b];
        const AssetLayer& L = layerTable[s.layer];
        const int32_t served = o.spacingMm < L.cellMm ? L.cellMm : o.spacingMm;
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
    for (size_t di = 0; di < kAssetKindCount * kBiomeCount; ++di) classDensity_[di] = density[di];
    rules_ = std::move(ruleTable);
    attachments_ = std::move(attachTable);
    overrides_ = std::move(ovTable);
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

namespace {

// One output row's placement inputs: the base record's gates with one
// override's masked fields replaced. The variant machinery below is the whole
// consumption of AssetManifestOverride -- the RESOLVER never sees an override,
// only species rows, so every soundness argument about the gates is unchanged.
struct EffectiveGates {
    int32_t elevMinMm = 0;
    int32_t elevMaxMm = 0;
    int32_t slopeMinMmPerM = 0;
    int32_t slopeMaxMmPerM = 0;
    int32_t waterMaxMm = 0;
    int32_t spacingMm = 0;
    uint16_t abundanceQ10 = 0;
    uint16_t clusterQ10 = 0;
};

EffectiveGates effectiveGates(const AssetManifestSpecies& r, const AssetManifestOverride* o) {
    EffectiveGates g;
    g.elevMinMm = r.elevMinMm;
    g.elevMaxMm = r.elevMaxMm;
    g.slopeMinMmPerM = r.slopeMinMmPerM;
    g.slopeMaxMmPerM = r.slopeMaxMmPerM;
    g.waterMaxMm = r.waterMaxMm;
    g.spacingMm = r.spacingMm;
    g.abundanceQ10 = r.abundanceQ10;
    g.clusterQ10 = r.clusterQ10;
    if (o != nullptr) {
        if (o->fieldMask & kOverrideElevMin) g.elevMinMm = o->elevMinMm;
        if (o->fieldMask & kOverrideElevMax) g.elevMaxMm = o->elevMaxMm;
        if (o->fieldMask & kOverrideSlopeMin) g.slopeMinMmPerM = o->slopeMinMmPerM;
        if (o->fieldMask & kOverrideSlopeMax) g.slopeMaxMmPerM = o->slopeMaxMmPerM;
        if (o->fieldMask & kOverrideWaterMax) g.waterMaxMm = o->waterMaxMm;
        if (o->fieldMask & kOverrideSpacing) g.spacingMm = o->spacingMm;
        if (o->fieldMask & kOverrideAbundance) g.abundanceQ10 = o->abundanceQ10;
        if (o->fieldMask & kOverrideCluster) g.clusterQ10 = o->clusterQ10;
    }
    return g;
}

// Fold one output row for species `r` (manifest row `bankId`) carrying
// `authored[b]` weight per biome under gates `g`. Returns false when every
// folded weight rounds to zero -- the row expresses nothing and is not
// emitted. `parent` carries the species' FULL authored weights: the moisture
// heuristic reads what the author said about the whole species, not the
// single-biome slice a variant row carries (a temperate oak with a desert
// override is not a xerophile because its desert row mentions only desert).
bool foldRow(const AssetManifestSpecies& r, uint16_t bankId, const AssetLayer& L,
             const EffectiveGates& g, const uint16_t* authored,
             const AssetManifestSpecies& parent, const uint16_t* densityRow, AssetSpecies& s) {
    s = AssetSpecies{};
    // The bank reference is the manifest ROW INDEX; AssetBankLibrary maps it
    // back to the name. An index rather than a hash of the name so a bank
    // lookup cannot collide two species into one appearance. Variant rows
    // share their base row's bankId: one species, one bank, several gate sets.
    s.bankId = bankId;
    s.layer = r.layer;
    s.elevMinMm = g.elevMinMm;
    s.elevMaxMm = g.elevMaxMm;
    s.slopeMinMmPerM = g.slopeMinMmPerM;
    s.slopeMaxMmPerM = g.slopeMaxMmPerM;
    s.waterMaxMm = g.waterMaxMm;
    s.clusterQ10 = g.clusterQ10;
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
    // specs is the point: the curve moves with the species' own height.
    // A per-biome slope override feeds the SAME derivation through g, so a
    // biome that authors a strict ceiling gets a strict curve with it.
    {
        int32_t zero = g.slopeMaxMmPerM;
        const bool woody = r.kind == AssetKind::kTree || r.kind == AssetKind::kBush;
        if (woody && g.slopeMaxMmPerM > 300) {
            const int32_t derived = r.heightMm >= 15'000   ? 600
                                    : r.heightMm >= 8'000  ? 750
                                    : r.heightMm >= 3'000  ? 900
                                                           : 1000;
            if (derived > zero) zero = derived;
        }
        s.slopeMaxMmPerM = zero; // the curve's zero point -- see the field
        const int32_t full = zero * 2 / 3;
        s.slopeFullMmPerM = full < g.slopeMinMmPerM ? g.slopeMinMmPerM : full;
    }

    // MOISTURE AFFINITY, from what the author already said: a species
    // that binds itself to water (water_max_m > 0) is a hygrophile; one
    // whose weight lives mostly in the dry biomes is a xerophile; one
    // rooted in the rainforest leans wet. Reading the PARENT's authored
    // weights (pre-fold, pre-split) because abundance says how MANY, not
    // how thirsty -- and because a variant row's single-biome slice says
    // nothing about the species' temperament.
    {
        int64_t total = 0;
        for (uint32_t b = 0; b < kBiomeCount; ++b) total += parent.biomeWeightPerMille[b];
        const int64_t dry = int64_t(parent.biomeWeightPerMille[DESERT]) +
                            int64_t(parent.biomeWeightPerMille[SAVANNA]);
        const int64_t wet = int64_t(parent.biomeWeightPerMille[RAINFOREST]);
        if (g.waterMaxMm > 0) {
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

    // THE PER-BIOME OCCUPANCY ROW: the kind x biome density table's row for
    // this species' kind, verbatim. See AssetSpecies::occupancyPerMille.
    if (densityRow != nullptr)
        for (uint32_t b = 0; b < kBiomeCount; ++b) s.occupancyPerMille[b] = densityRow[b];

    // THE FOLD. weight x abundance x (cell/spacing)^2, exact integer, one
    // rounding at the end (round-to-nearest: floor would shave every
    // species, and the fold's job is to preserve the authored population).
    // The spacing residual uses the SERVED spacing -- authored, floored at
    // the cell pitch -- so a species tighter than its lattice folds at 1.
    const int64_t cell = int64_t(L.cellMm);
    const int64_t spacing = g.spacingMm < L.cellMm ? cell : int64_t(g.spacingMm);
    bool anyFolded = false;
    for (uint32_t b = 0; b < kBiomeCount; ++b) {
        const int64_t w = int64_t(authored[b]);
        const int64_t num = w * int64_t(g.abundanceQ10) * cell * cell;
        const int64_t den = 1024 * spacing * spacing;
        const int64_t folded = (num + den / 2) / den;
        s.weightPerMille[b] = static_cast<uint16_t>(folded > 1000 ? 1000 : folded);
        if (s.weightPerMille[b] > 0) anyFolded = true;
    }
    return anyFolded;
}

} // namespace

AssetTableBuildStats assetSpeciesTableFromManifest(const AssetManifest& m,
                                                   std::vector<AssetSpecies>& speciesOut) {
    AssetTableBuildStats st;
    speciesOut.clear();
    const std::vector<AssetLayer>& layers = m.layers();
    const std::vector<AssetManifestSpecies>& rows = m.species();
    const std::vector<AssetManifestOverride>& ovs = m.overrides();
    speciesOut.reserve(rows.size());
    size_t ovAt = 0; // overrides are sorted by species index: one forward walk
    for (size_t i = 0; i < rows.size(); ++i) {
        const AssetManifestSpecies& r = rows[i];
        // This species' override run (possibly empty).
        const size_t ovBegin = ovAt;
        while (ovAt < ovs.size() && ovs[ovAt].speciesIndex == i) ++ovAt;
        const size_t ovEnd = ovAt;

        if (r.layer == kAssetLayerNotScattered) {
            ++st.detailEntities;
            continue;
        }
        const AssetLayer& L = layers[r.layer];
        const uint16_t* densityRow = m.classDensity() + size_t(r.kind) * kBiomeCount;

        bool anyAuthored = false;
        for (uint32_t b = 0; b < kBiomeCount; ++b)
            if (r.biomeWeightPerMille[b] > 0) anyAuthored = true;
        if (!anyAuthored) {
            ++st.noBiome;
            continue;
        }

        // THE SPLIT. The base row keeps every biome no override touches; each
        // override moves its biome's authored weight onto a variant row that
        // carries the overridden gates. A column has exactly one biome, so
        // exactly one of the species' rows can be eligible at any site: the
        // pick arithmetic sees the same summed weight it would have seen from
        // one row, and the species cannot compete with itself.
        uint16_t baseW[kBiomeCount];
        for (uint32_t b = 0; b < kBiomeCount; ++b) baseW[b] = r.biomeWeightPerMille[b];
        for (size_t k = ovBegin; k < ovEnd; ++k) baseW[ovs[k].biome] = 0;

        AssetSpecies candidate;
        int emitted = 0;
        if (foldRow(r, static_cast<uint16_t>(i), L, effectiveGates(r, nullptr), baseW, r,
                    densityRow, candidate)) {
            speciesOut.push_back(candidate);
            ++emitted;
        }
        for (size_t k = ovBegin; k < ovEnd; ++k) {
            uint16_t variantW[kBiomeCount] = {};
            variantW[ovs[k].biome] = r.biomeWeightPerMille[ovs[k].biome];
            if (variantW[ovs[k].biome] == 0) continue; // override on a dark biome: nothing to carry
            if (foldRow(r, static_cast<uint16_t>(i), L, effectiveGates(r, &ovs[k]), variantW, r,
                        densityRow, candidate)) {
                speciesOut.push_back(candidate);
                ++emitted;
            }
        }

        if (emitted == 0) {
            // Authored present, folds to nothing everywhere: per-mille
            // resolution cannot express this species' rarity on its layer's
            // lattice. The exporter names each one; this side keeps the count
            // so a load can assert the two agree.
            ++st.tooRare;
            continue;
        }
        if (r.seedsBaked == 0) ++st.withoutBanks;
        ++st.kept;
        st.splitRows += emitted - 1;
    }
    return st;
}

} // namespace vxc
