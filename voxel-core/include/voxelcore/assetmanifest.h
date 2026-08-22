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
// VERSION 2 (2026-08-18, the per-biome placement expansion). Same magic --
// the family tag -- same 152-byte species record, two new sections:
//
//   * a KIND x BIOME DENSITY TABLE between the layer table and the species
//     records: kAssetKindCount x kBiomeCount u16 per-mille, kind-major. It
//     scales the occupancy test per kind per biome (AssetSpecies::
//     occupancyPerMille carries the species' row after import), which is the
//     audit's "per-biome density scalar" widened to one row per asset class.
//     1000 everywhere is today's world exactly; values above 1000 are REFUSED
//     because the veto-only rule says a policy may only remove sites.
//
//   * a NAMED PLACEMENT-RULE TABLE and a RULE-ATTACHMENT TABLE around the
//     species records. A rule is authored ONCE, by name ("near-fresh-water-
//     60m": water_max 60 m, fresh), and carries a masked subset of the
//     placement gates -- elevation band, slope band, water distance and
//     kind, spacing, abundance, cluster. An attachment binds (species,
//     biome) -> rule, one or many per pair. This is the owner's contract of
//     2026-08-18: "temperate type tree is almost unrestricted in temperate
//     forest but faces strict placement rules in deserts such as must be
//     near fresh water body", with the rules "custom authored and defined
//     prior" and attached per species by reference. parse() COMPOSES each
//     (species, biome)'s attached rules into one effective gate set --
//     intersection, strictest wins, see composeOverrides in the .cpp -- and
//     the import turns each composed pair into a SPLIT ROW of the species
//     table (assetSpeciesTableFromManifest), so the resolver's gates stay
//     biome-blind scalar compares.
inline constexpr uint32_t kVxmVersion = 2u;
inline constexpr size_t kVxmHeaderBytes = 32u;
inline constexpr size_t kVxmBiomeNameBytes = 16u;
inline constexpr size_t kVxmLayerBytes = 24u;
inline constexpr size_t kVxmSpeciesBytes = 152u;
inline constexpr size_t kVxmNameBytes = 64u;
inline constexpr size_t kVxmRuleBytes = 64u;
inline constexpr size_t kVxmRuleNameBytes = 32u;
inline constexpr size_t kVxmAttachBytes = 8u;

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
    kBadDensity,         // a kind x biome density entry above 1000 per-mille:
                         // the table may only THIN (veto-only), so a boost is
                         // not a tuning value, it is a file this build must
                         // not honour
    kBadRule,            // a named placement rule is malformed: bad name, no
                         // mask bit or an unknown one, a field carried
                         // without its mask bit, a non-positive spacing, an
                         // inverted band inside the rule, or a water kind
                         // outside the enum
    kBadAttachment,      // a rule attachment is malformed (species, biome or
                         // rule index out of range; unsorted or duplicate) --
                         // or the attached rules COMPOSE to a contradiction
                         // for their (species, biome): an empty band, an
                         // empty salinity mask, or two rules stating
                         // different water kinds or cluster strengths. The
                         // exporter refuses these by name at export, so
                         // reaching this means the file and the exporter
                         // disagree
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

// Which gate fields a named rule carries. A rule's unmasked fields MUST be
// zero on the wire (parse refuses otherwise) so a manifest has exactly one
// byte encoding -- the exporter and enginecheck compare manifests by bytes.
enum AssetOverrideField : uint16_t {
    kOverrideElevMin = 1u << 0,
    kOverrideElevMax = 1u << 1,
    kOverrideSlopeMin = 1u << 2,
    kOverrideSlopeMax = 1u << 3,
    kOverrideWaterMax = 1u << 4,
    kOverrideWaterKind = 1u << 5, // waterKind AND waterMask together
    kOverrideSpacing = 1u << 6,
    kOverrideAbundance = 1u << 7,
    kOverrideCluster = 1u << 8,
};
inline constexpr uint16_t kOverrideFieldMaskAll = 0x01FFu;

// One NAMED placement rule, authored once in asset-forge's rule library
// (rules/placement-rules.json) and referenced by any number of species. The
// name is for humans and reports; the gates are the payload. Only masked
// fields are meaningful; the rest are zero on the wire.
struct AssetPlacementRule {
    std::string name;       // <= 31 ASCII chars, species-name charset
    uint16_t fieldMask = 0; // AssetOverrideField bits
    AssetWaterKind waterKind = AssetWaterKind::kAny;
    uint8_t waterMask = 0;
    uint16_t abundanceQ10 = 0;
    uint16_t clusterQ10 = 0;
    int32_t elevMinMm = 0;
    int32_t elevMaxMm = 0;
    int32_t slopeMinMmPerM = 0;
    int32_t slopeMaxMmPerM = 0;
    int32_t waterMaxMm = 0;
    int32_t spacingMm = 0;
};

// One attachment: "in THIS biome, this species obeys THIS rule." One or many
// per (species, biome); parse() composes each pair's set by intersection.
struct AssetRuleAttachment {
    uint16_t speciesIndex = 0; // row in the species table
    uint8_t biome = 0;         // BiomeId
    uint16_t ruleIndex = 0;    // row in the rule table
};

// The COMPOSED per-(species, biome) override -- not a wire record. parse()
// builds one of these per (species, biome) that has attachments, by
// intersecting the attached rules with the species' own defaults (strictest
// wins; see composeOverrides in assetmanifest.cpp for the per-field law).
// The import (assetSpeciesTableFromManifest) applies these by SPLITTING the
// species: the base row loses its weight in every overridden biome and one
// variant row per composed pair carries that biome's weight with the
// composed gates -- the resolver never sees a rule, only species rows, so
// the veto-only argument is unchanged.
//
// spacing NEVER MOVES THE LAYER: a species is filed on one lattice at export
// (assign_layer reads the DEFAULT spacing), and a composed spacing acts only
// through the fold's (cell/spacing)^2 residual in its own biome. A per-biome
// lattice would be a per-biome streaming bound, which the bound --
// deliberately biome-blind -- cannot pay.
struct AssetManifestOverride {
    uint16_t speciesIndex = 0; // row in the species table
    uint8_t biome = 0;         // BiomeId
    uint16_t fieldMask = 0;    // AssetOverrideField bits (union of the set)
    AssetWaterKind waterKind = AssetWaterKind::kAny;
    uint8_t waterMask = 0;
    uint16_t abundanceQ10 = 0;
    uint16_t clusterQ10 = 0;
    int32_t elevMinMm = 0;
    int32_t elevMaxMm = 0;
    int32_t slopeMinMmPerM = 0;
    int32_t slopeMaxMmPerM = 0;
    int32_t waterMaxMm = 0;
    int32_t spacingMm = 0;
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

    // The kind x biome occupancy density table, per-mille, kind-major:
    // classDensity()[kind * kBiomeCount + biome]. Every entry <= 1000 after a
    // successful parse (parse refuses more). All-1000 is the neutral table.
    const uint16_t* classDensity() const { return classDensity_; }

    // The named rule library and the raw attachments, as authored -- for
    // reports and for any host that wants to show "which rules bind here".
    const std::vector<AssetPlacementRule>& rules() const { return rules_; }
    const std::vector<AssetRuleAttachment>& attachments() const { return attachments_; }

    // The COMPOSED per-(species, biome) overrides -- what the import
    // consumes. Sorted by (speciesIndex, biome), unique.
    const std::vector<AssetManifestOverride>& overrides() const { return overrides_; }

    // The species a kSpeciesMisfiled refusal was about, for the error path
    // that has a human on it. Empty unless the last parse returned that.
    const std::string& misfiledName() const { return misfiledName_; }
    AssetTableError misfiledWhy() const { return misfiledWhy_; }

private:
    void clear();
    std::vector<AssetLayer> layers_;
    std::vector<AssetManifestSpecies> species_;
    uint16_t classDensity_[kAssetKindCount * kBiomeCount] = {};
    std::vector<AssetPlacementRule> rules_;
    std::vector<AssetRuleAttachment> attachments_;
    std::vector<AssetManifestOverride> overrides_;
    std::string misfiledName_;
    AssetTableError misfiledWhy_ = AssetTableError::kOk;
};

// What the fold kept, dropped and bent, AS COUNTS. This project's signature
// failure is the silent no-op, and an importer that folds 828 species into a
// table of 500 without a number anywhere is one. Zero `kept` against a
// non-empty manifest is a wiring fault by definition.
// --- WHICH CONTENT A COVER VOLUME CANNOT HOLD, BY NAME ----------------------
//
// A cover volume composes ONE pitch (AssetField::resolveForCoverCompose, and
// asset-forge/README.md:38-41 for why: nothing in voxel-core resamples, so a
// grid composed at the wrong pitch does not degrade, it changes SIZE). Every
// detail species baked at any other pitch is therefore dropped -- correctly,
// and silently unless something says so.
//
// THIS EXISTS SO THAT IS NOT SILENT. The rule this project keeps paying for is
// that content failing a gate gets debugged as code: the drop happens per
// instance, deep in a resolve loop, where the species has no name any more.
// Called once when a cover volume is configured, this names every species that
// volume will never show -- the same discipline as ValidateRegionRequest
// refusing SizeZ > 4095 with the species still nameable by the caller, and as
// AssetTableBuildStats reporting tooRare/noBiome rather than just keeping fewer
// rows.
//
// It reads the MANIFEST, not the banks, for the same reason placement does:
// the manifest is the authored truth and does not depend on which banks happen
// to have been exported today.
struct AssetCoverPitchRefusal {
    std::string name;            // banks/<name>/<name>-NNNN.vxa
    AssetKind kind = AssetKind::kTree;
    uint32_t voxelSizeMm = 0;    // what it was actually baked at
};

// Every DETAIL-lattice, scattered species whose bake pitch is not `pitchMm`.
//
// Terrain-lattice species are not refusals and are not listed: they compose
// into the WORLD volume through the asset stamp and are already there. Layer
// kAssetLayerNotScattered (the 382 animals) is likewise not a refusal -- those
// never enter any volume, by an owner decision that predates this one.
inline std::vector<AssetCoverPitchRefusal>
assetCoverPitchRefusals(const AssetManifest& manifest, uint32_t pitchMm) {
    std::vector<AssetCoverPitchRefusal> out;
    for (const AssetManifestSpecies& s : manifest.species()) {
        if (s.layer == kAssetLayerNotScattered) continue;  // animals: not ours
        if (s.terrainLattice) continue;                    // already in the world
        if (s.voxelSizeMm == pitchMm) continue;            // composes fine
        out.push_back(AssetCoverPitchRefusal{s.name, s.kind, s.voxelSizeMm});
    }
    return out;
}

struct AssetTableBuildStats {
    int kept = 0;           // manifest SPECIES kept (>= 1 output row each)
    int splitRows = 0;      // EXTRA output rows from per-biome overrides: a
                            // species with overrides becomes a base row plus
                            // one variant per overridden biome, all sharing
                            // one bankId. 0 whenever no overrides are
                            // authored, so the pre-override accounting
                            // (kept + detail + tooRare + noBiome == species)
                            // still holds and table.size() == kept + this
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

// LOWER EACH LAYER'S maxHeightMm TO THE TALLEST THING ACTUALLY FILED ON IT.
//
// The authored cap is a bake-time CONTRACT and is deliberately loose; it is not
// a description of the library. Worse, it cannot become one: assign_layer files
// a species by SPACING among the layers whose cap admits it, so a 5 m shrub
// authored at 30 m spacing lands on L0 and inherits its 60 m cap. Nothing in the
// authoring path ties a layer's cap to its occupants.
//
// Every streaming bound is made of that cap -- assetTopAboveSurfaceMm returns
// `maxHeightMm or nothing`, and the level-0 chunk z-range adds it to every
// footprint. Measured 2026-08-17 against the shipped table: mean 16.49 extra
// chunk layers admitted per footprint versus a 1.45-layer terrain shell, i.e.
// ~12x the chunks the terrain needs, which collapsed streaming badly enough that
// a 240 s settle reached 4,234 of 41,069 chunks. The early-out in
// assetTopAboveSurfaceMm cannot help: L1 is a 5 m lattice at full density, so a
// site is found in the first cell for 89.2% of footprints.
//
// SAFE BY CONSTRUCTION: this only ever lowers a cap to the maximum height of the
// species already filed on that layer, so no asset that fits today stops fitting
// and assetLayerAdmitsHeight still holds for everything in the manifest. A layer
// with no baked occupants keeps its authored cap rather than collapsing to zero
// -- too small is a hole in the world at a crown, which is the unsafe direction.
//
// Call this AFTER parsing and BEFORE handing layers to AssetField or to any
// bound. The engine and vxc_assetprobe both call it, deliberately: a probe that
// measured the authored caps would be pricing a world the engine does not run.
void assetTightenLayerCaps(const AssetManifest& m, std::vector<AssetLayer>& layers);

} // namespace vxc
