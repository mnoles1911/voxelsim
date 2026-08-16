#pragma once
// VXM1 -- the species placement manifest, read side.
//
// asset-forge (forge/manifest.py, tools/export_manifest.py) reduces the 828
// authored specs to one versioned binary table: kind, layer, per-biome weight,
// abundance, spacing, cluster, elevation band, slope band, depth band,
// herd/shoal parameters and a bank reference per species -- everything the
// placement policy needs to know WITHOUT decoding a voxel. This is the
// engine's reader for it, on assetgrid.h's parse-refusal model: a table that
// fails any check is refused whole, with a named reason, because half a
// species table is a world with half its species and no message saying so.
//
// TWO CHECKS HERE HAVE NO ANALOGUE IN THE VXA READER, and both guard failures
// that would be invisible rather than broken:
//
//   * THE BIOME NAMES ARE IN THE FILE AND ARE VERIFIED BY SPELLING against
//     this build's own BiomeId order. Weights are indexed by biome; if either
//     side ever reorders its list, every rainforest weight in the library
//     becomes a desert weight, and every one of those is a perfectly valid
//     weight. A silent reorder is not a parse error anywhere else, so it has
//     to be made one here.
//
//   * THE LAYER TABLE RIDES IN THE MANIFEST, not in a header on this side.
//     assetplacement.h's bound is made of the layer table, and a species
//     record is meaningless except against the exact table it was filed under
//     -- "filed on the 26 m layer" is a hole in the world if the reader's
//     26 m layer is actually 14 m. Shipping them in one file makes the
//     disagreement unrepresentable. It also makes the table DATA: tuned by
//     vxc_assetprobe's widening census and re-exported, not recompiled.
//
// DETERMINISM. Once a world has generated against a manifest, the manifest IS
// worldgen input: any byte change is a kWorldGenVersion bump with goldens
// re-blessed (docs/asset-placement-architecture.md section 9). That is why the
// worldgen digest is required to MOVE when a manifest is first installed --
// the digest doubles as the ran-flag, and an unchanged digest means the field
// was never wired.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "voxelcore/assetplacement.h"
#include "voxelcore/assetpolicy.h"
#include "voxelcore/biome.h"

namespace vxc {

inline constexpr uint32_t kVxmMagic = 0x314D5856u; // "VXM1", little-endian
inline constexpr uint32_t kVxmVersion = 1u;
inline constexpr size_t kVxmHeaderBytes = 32u;
inline constexpr size_t kVxmBiomeNameBytes = 16u;
inline constexpr size_t kVxmLayerBytes = 24u;
inline constexpr size_t kVxmSpeciesBytes = 152u;
inline constexpr size_t kVxmNameBytes = 64u;

// The ten kinds, in the manifest's own stable order (forge/manifest.py
// KIND_ORDER -- append-only, deliberately NOT forge/kinds.py's UI order).
enum class AssetKind : uint8_t {
    kTree = 0,
    kBush,
    kRock,
    kGrass,
    kReed,
    kFlower,
    kFish,
    kBird,
    kQuadruped,
    kCetacean,
};
inline constexpr uint32_t kAssetKindCount = 10;

// detail.water, as authored. Kept alongside the salinity mask because two of
// the choices carry more than salinity: `reef` also means "bed within the
// species' depth_max of the surface" and `shallow` means "the margin of
// anything" -- gates a later pass can serve from the same record.
enum class AssetWaterKind : uint8_t {
    kAny = 0,
    kOcean,
    kRiver,
    kLake,
    kShallow,
    kReef,
};
inline constexpr uint32_t kAssetWaterKindCount = 6;

// The manifest's `layer` value for a species that is not on the scatter
// lattice at all: the detail ENTITIES (fish, birds, quadrupeds, cetaceans),
// which spawn through assetdetail.h's group scatter instead.
inline constexpr uint8_t kAssetLayerNotScattered = 255;

// Why a manifest was rejected. Named, whole-file: the exporter is the tool
// that must never produce these, so any one of them means the FILE is not
// what this build understands, and reading the intelligible half of a
// misunderstood file is how weights end up on the wrong biome.
enum class AssetManifestError : uint8_t {
    kOk = 0,
    kTooSmall,           // fewer bytes than a header
    kBadMagic,           // not a VXM file at all
    kBadVersion,         // a VXM this build does not understand
    kBadBiomeCount,      // file's biome axis is not this build's kBiomeCount
    kBiomeOrderMismatch, // a biome name differs by spelling or position
    kBadLayerCount,      // not exactly kAssetLayerCount layers
    kBadLayer,           // a layer row is degenerate (cell <= 0 on a real layer)
    kBadRecordSize,      // per-species record size is not this build's
    kTruncated,          // header promises more records than the blob carries
    kBadKind,            // species kind byte outside the enum
    kBadName,            // empty name, or one that is not NUL-terminated ASCII
    kSpeciesMisfiled,    // a species fails assetSpeciesFits against its own
                         // layer -- the exporter's bake-time check exists to
                         // make this unreachable, so reaching it means the
                         // file and the exporter disagree and NOTHING about
                         // the file should be trusted
};

const char* assetManifestErrorText(AssetManifestError e);

// One species, as the wire carries it -- authored facts, unfolded. The folded,
// policy-ready form is AssetSpecies, built by assetSpeciesTableFromManifest.
struct AssetManifestSpecies {
    std::string name; // the bank reference: banks/<name>/<name>-NNNN.vxa
    AssetKind kind = AssetKind::kTree;
    uint8_t layer = kAssetLayerNotScattered;
    bool terrainLattice = false;
    AssetWaterKind waterKind = AssetWaterKind::kAny;
    uint8_t waterMask = 0;
    uint16_t seedsBaked = 0;
    uint32_t voxelSizeMm = 0;
    uint16_t biomeWeightPerMille[kBiomeCount] = {};
    uint16_t abundanceQ10 = 0;
    uint16_t clusterQ10 = 0;
    int32_t spacingMm = 0;
    int32_t elevMinMm = 0;
    int32_t elevMaxMm = 0;
    int32_t slopeMinMmPerM = 0;
    int32_t slopeMaxMmPerM = 0;
    int32_t waterMaxMm = 0;
    int32_t heightMm = 0;
    int32_t depthMm = 0;
    int32_t depthMinMm = 0;
    int32_t depthMaxMm = 0;
    int32_t minWaterDepthMm = 0;
    uint16_t groupMin = 1;
    uint16_t groupMax = 1;
    int32_t groupRadiusMm = 0;
};

class AssetManifest {
public:
    AssetManifest() = default;

    // Parses `blob` in place. kOk and a filled manifest, or a named reason and
    // an empty one. Never partially fills.
    AssetManifestError parse(const uint8_t* blob, size_t bytes);
    AssetManifestError parse(const std::vector<uint8_t>& blob) {
        return parse(blob.data(), blob.size());
    }

    bool valid() const { return !species_.empty(); }

    // The scatter lattice table the species were filed under. Exactly
    // kAssetLayerCount entries after a successful parse.
    const std::vector<AssetLayer>& layers() const { return layers_; }
    const std::vector<AssetManifestSpecies>& species() const { return species_; }

    // The species a kSpeciesMisfiled refusal was about, for the error path
    // that has a human on it. Empty unless the last parse returned that.
    const std::string& misfiledName() const { return misfiledName_; }
    AssetTableError misfiledWhy() const { return misfiledWhy_; }

private:
    void clear();
    std::vector<AssetLayer> layers_;
    std::vector<AssetManifestSpecies> species_;
    std::string misfiledName_;
    AssetTableError misfiledWhy_ = AssetTableError::kOk;
};

// What the fold kept, dropped and bent, AS COUNTS. This project's signature
// failure is the silent no-op, and an importer that folds 828 species into a
// table of 500 without a number anywhere is one. Zero `kept` against a
// non-empty manifest is a wiring fault by definition.
struct AssetTableBuildStats {
    int kept = 0;           // rows in the output table
    int detailEntities = 0; // layer 255: not this table's business (classes 3-4)
    int tooRare = 0;        // every folded weight rounded to zero per-mille --
                            // the species is ABSENT from the world (the
                            // exporter predicted and named each one)
    int noBiome = 0;        // authored zero everywhere: never appears anywhere
    int withoutBanks = 0;   // kept, but seedsBaked == 0 -- placement is still
                            // deterministic (placement reads the MANIFEST, and
                            // must not depend on which banks happen to exist),
                            // but every instance composes as air until banks
                            // land, so this count is the first thing to check
                            // when "the table loaded and nothing renders"
};

// The policy-ready table: classes 1 and 2, folded.
//
// THE FOLD is weightPerMille x abundance x min(1, (cell/spacing)^2):
//   * abundance, because a rare species must be rarely PICKED so the site
//     goes to a common one (assetpolicy.h's weightPerMille comment);
//   * the spacing residual, because the lattice is the spacing MECHANISM and
//     a species authored sparser than its layer's pitch owes the difference
//     as pick probability -- without this factor a landmark rock filed on a
//     24 m lattice at 3000 m authored spacing would stand every 24 m.
//
// `speciesOut[i].bankId` is the species' ROW INDEX IN THE MANIFEST, which is
// what AssetBankLibrary resolves back to a name and a directory.
AssetTableBuildStats assetSpeciesTableFromManifest(const AssetManifest& m,
                                                   std::vector<AssetSpecies>& speciesOut);

} // namespace vxc
