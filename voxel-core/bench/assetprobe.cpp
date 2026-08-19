// vxc_assetprobe -- the WIDENING CENSUS and the placement counters for the
// asset layer table. Counts, not clocks.
//
// WHY THIS EXISTS. assetplacement.h's bound is policy-independent, which means
// VETOED REGIONS PAY IT TOO: assetTopAboveSurfaceMm widens wherever the
// LATTICE has a site, including biomes where the policy vetoes every tree. So
// the layer table is a price list -- a tall layer at density 1000 is a
// near-constant widening over every footprint on the planet -- and
// docs/asset-placement-architecture.md section 2.4 requires the price to be
// MEASURED before the table is tuned. Nobody had measured it. This tool is
// that measurement, and forge/manifest.py's LAYERS carries its verdict.
//
// TWO CENSUSES:
//
//   1. WIDENING (always): over every level-0 render-chunk footprint (32
//      voxels = 3.2 m square) in a region, the distribution of
//      assetTopAboveSurfaceMm and the number of EXTRA admitted chunk layers
//      versus the terrain-only bound -- extra admissions being the actual
//      currency the streamer pays (an admitted all-air chunk is generated,
//      found empty and meshed to nothing; its cost is real and its count is
//      deterministic). --l1cap reprices the canopy layer's height cap without
//      re-exporting, which is how the 26 m / 34 m / 45 m question was priced.
//
//   2. PLACEMENT (--banks): resolve real instances from the manifest table
//      over the real amplifier and report the section-10 counters -- sites
//      considered, instances resolved, per-layer and per-species counts, the
//      anchor contact audit (every anchor independently re-verified solid),
//      stamped-voxel counts over sample bricks, and the bank library's own
//      served/missed/refused census. Zero instances against thousands of
//      sites is a wiring fault BY DEFINITION, and this is the tool that makes
//      it a number instead of a quiet green run.
//
// Real terrain: --fine <dir> loads v2 fine tiles (the two wet-bake tiles in
// bake-out/ are the on-disk corpus today). Climate still comes from the
// synthetic coarse raster when no coarse tiles are given -- labelled in the
// output, because a biome census over synthetic climate is a different fact
// from one over a baked world.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/assetbank.h"
#include "voxelcore/assetchannels.h"
#include "voxelcore/assetfield.h"
#include "voxelcore/assetmanifest.h"
#include "voxelcore/lakes.h"
#include "voxelcore/tilestore.h"

using namespace vxc;

namespace {

struct Options {
    std::string manifest;
    std::string banks;
    std::string fineDir;
    std::string coarseDir;
    uint64_t seed = 20260719;
    int64_t regionM = 512;    // half-edge of the census square, metres
    int64_t originXM = 0, originYM = 0;
    int32_t l1capMm = 0;      // 0 = as exported
    int64_t placeRegionM = 64; // half-edge of the placement census square
    // Pricing overrides for census 1 (placement refuses to run under them):
    int32_t l0capMm = 0, l0radiusMm = 0, l1radiusMm = 0;
    int32_t l0density = -1; // per-mille; -1 = as exported
    // --- the comparable-census instruments (owner ask, 2026-08-17) ----------
    std::string jsonPath;    // --json: machine-readable counters, diffable
    std::string comparePath; // --compare: a previous --json; prints the delta
    std::string overlayBase; // --overlay: <base>.instances.csv + <base>.ground.bin
    // --no-channels: serve SENTINEL channels over real ground -- the exact
    // facts the engine composed with before the channel binding was wired
    // (VoxelWorldSubsystem's fix of this date), so a --compare of the two runs
    // IS the before/after of that defect, as numbers.
    bool noChannels = false;
};

constexpr int kChunkVox = 32;                       // level-0 render chunk edge
constexpr int64_t kChunkMm = int64_t(kChunkVox) * kVoxelSizeMm; // 3200

const char* kindName(AssetKind k) {
    switch (k) {
        case AssetKind::kTree: return "tree";
        case AssetKind::kBush: return "bush";
        case AssetKind::kRock: return "rock";
        case AssetKind::kGrass: return "grass";
        case AssetKind::kReed: return "reed";
        case AssetKind::kFlower: return "flower";
        case AssetKind::kFish: return "fish";
        case AssetKind::kBird: return "bird";
        case AssetKind::kQuadruped: return "quadruped";
        case AssetKind::kCetacean: return "cetacean";
        default: return "?";
    }
}

// The counters of one run, as (flat key -> value). This IS the JSON's
// "counters" object and the whole currency of --compare: two builds diff
// numerically or not at all.
using CounterMap = std::map<std::string, long long>;

// Reads the "counters" object back out of a --json file. Deliberately matched
// to writeCensusJson's own output (flat `"key": value` lines inside one
// object) rather than a general JSON parser -- the probe only ever compares
// against files the probe wrote.
bool readCountersJson(const std::string& path, CounterMap& out) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    bool inCounters = false;
    while (std::getline(in, line)) {
        if (!inCounters) {
            if (line.find("\"counters\"") != std::string::npos) inCounters = true;
            continue;
        }
        if (line.find('}') != std::string::npos) break;
        const size_t k0 = line.find('"');
        if (k0 == std::string::npos) continue;
        const size_t k1 = line.find('"', k0 + 1);
        if (k1 == std::string::npos) continue;
        const size_t c = line.find(':', k1);
        if (c == std::string::npos) continue;
        out[line.substr(k0 + 1, k1 - k0 - 1)] = std::strtoll(line.c_str() + c + 1, nullptr, 10);
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--manifest" && i + 1 < argc) opt.manifest = argv[++i];
        else if (a == "--banks" && i + 1 < argc) opt.banks = argv[++i];
        else if (a == "--fine" && i + 1 < argc) opt.fineDir = argv[++i];
        else if (a == "--coarse" && i + 1 < argc) opt.coarseDir = argv[++i];
        else if (a == "--seed" && i + 1 < argc) opt.seed = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--region" && i + 1 < argc) opt.regionM = std::atoll(argv[++i]);
        else if (a == "--origin-x" && i + 1 < argc) opt.originXM = std::atoll(argv[++i]);
        else if (a == "--origin-y" && i + 1 < argc) opt.originYM = std::atoll(argv[++i]);
        else if (a == "--l1cap" && i + 1 < argc) opt.l1capMm = std::atoi(argv[++i]);
        else if (a == "--l0cap" && i + 1 < argc) opt.l0capMm = std::atoi(argv[++i]);
        else if (a == "--l0radius" && i + 1 < argc) opt.l0radiusMm = std::atoi(argv[++i]);
        else if (a == "--l1radius" && i + 1 < argc) opt.l1radiusMm = std::atoi(argv[++i]);
        else if (a == "--l0density" && i + 1 < argc) opt.l0density = std::atoi(argv[++i]);
        else if (a == "--place-region" && i + 1 < argc) opt.placeRegionM = std::atoll(argv[++i]);
        else if (a == "--json" && i + 1 < argc) opt.jsonPath = argv[++i];
        else if (a == "--compare" && i + 1 < argc) opt.comparePath = argv[++i];
        else if (a == "--overlay" && i + 1 < argc) opt.overlayBase = argv[++i];
        else if (a == "--no-channels") opt.noChannels = true;
        else {
            std::fprintf(stderr,
                         "usage: vxc_assetprobe --manifest species.vxm [--banks dir] "
                         "[--fine dir] [--coarse dir] [--seed n] [--region m] [--origin-x m] "
                         "[--origin-y m] [--l1cap mm] [--place-region m]\n"
                         "       [--json out.json]      counters as machine-readable JSON\n"
                         "       [--compare before.json] print the numeric delta vs an earlier --json\n"
                         "       [--overlay base]       base.instances.csv + base.ground.bin for map overlays\n"
                         "       [--no-channels]        sentinel channels over real ground (the pre-fix "
                         "engine binding, for before/after)\n");
            return 2;
        }
    }
    if (opt.manifest.empty()) {
        std::fprintf(stderr, "--manifest is required: the layer table under census "
                             "IS the manifest's\n");
        return 2;
    }

    const auto blob = readFileBytes(opt.manifest);
    if (!blob) {
        std::fprintf(stderr, "cannot read %s\n", opt.manifest.c_str());
        return 1;
    }
    AssetManifest manifest;
    const AssetManifestError me = manifest.parse(*blob);
    if (me != AssetManifestError::kOk) {
        std::fprintf(stderr, "manifest refused: %s\n", assetManifestErrorText(me));
        return 1;
    }
    std::vector<AssetLayer> layers = manifest.layers();
    // Before any override or census: the engine tightens caps to their tallest
    // baked occupant at install, so a probe reading the authored caps would be
    // pricing a world nobody runs. Printed rather than applied silently -- the
    // gap between the two IS the finding.
    {
        std::vector<AssetLayer> authored = layers;
        assetTightenLayerCaps(manifest, layers);
        std::printf("layer caps (authored -> tightened to tallest baked occupant):");
        for (size_t li = 0; li < layers.size(); ++li) {
            if (!layers[li].terrainLattice) continue;
            std::printf(" L%zu %d->%d mm", li, authored[li].maxHeightMm, layers[li].maxHeightMm);
        }
        std::printf("\n");
    }
    bool overridden = false;
    if (opt.l1capMm > 0) { layers[1].maxHeightMm = opt.l1capMm; overridden = true; }
    if (opt.l0capMm > 0) { layers[0].maxHeightMm = opt.l0capMm; overridden = true; }
    if (opt.l0radiusMm > 0) { layers[0].maxRadiusMm = opt.l0radiusMm; overridden = true; }
    if (opt.l1radiusMm > 0) { layers[1].maxRadiusMm = opt.l1radiusMm; overridden = true; }
    if (opt.l0density >= 0) {
        layers[0].densityPerMille = uint16_t(opt.l0density);
        overridden = true;
    }

    std::vector<AssetSpecies> table;
    const AssetTableBuildStats st = assetSpeciesTableFromManifest(manifest, table);
    std::printf("manifest: %zu species -> table %d kept, %d detail entities, %d too rare, "
                "%d no-biome, %d without banks\n",
                manifest.species().size(), st.kept, st.detailEntities, st.tooRare,
                st.noBiome, st.withoutBanks);
    std::printf("layers:");
    for (size_t li = 0; li < layers.size(); ++li)
        std::printf(" L%zu[cell %d mm, maxH %d, r %d, density %u, %s]", li,
                    layers[li].cellMm, layers[li].maxHeightMm, layers[li].maxRadiusMm,
                    unsigned(layers[li].densityPerMille),
                    layers[li].terrainLattice ? "terrain" : "detail");
    std::printf("%s\n", overridden ? "  (OVERRIDDEN for pricing)" : "");

    // --- the world under census ---------------------------------------------
    //
    // WHY --coarse EXISTS, 2026-08-17: this probe censused 3,741 instances at
    // the alpine lake while the ENGINE composed exactly zero there -- because
    // the climate channel (and with it every biome weight, the treeline gate
    // and the temperature dither) fell back to synthetic. Same rule as
    // assetTightenLayerCaps: a probe pricing a world the engine does not run
    // is not evidence. FineTileSampler delegates whatever it does not carry to
    // its inner sampler, so wiring the real coarse tiles as that inner sampler
    // is the whole change.
    SyntheticTileSampler synth(opt.seed);
    TileGridSampler coarse(opt.seed, /*scale*/ 1);
    ITileSampler* tiles = &synth;
    bool realClimate = false;
    if (!opt.coarseDir.empty()) {
        int loaded = 0, rejected = 0;
        for (auto& e : std::filesystem::directory_iterator(opt.coarseDir)) {
            if (e.path().extension() != ".vxtl") continue;
            if (coarse.loadTileFile(e.path())) ++loaded;
            else ++rejected;
        }
        std::printf("coarse tiles: loaded %d, rejected %d from %s\n", loaded, rejected,
                    opt.coarseDir.c_str());
        if (loaded == 0) {
            std::fprintf(stderr, "no coarse tiles loaded; refusing to silently census "
                                 "synthetic climate under a --coarse flag\n");
            return 1;
        }
        tiles = &coarse;
        realClimate = true;
    }
    FineTileSampler fine(opt.seed, tiles);
    bool real = false;
    if (!opt.fineDir.empty()) {
        int loaded = 0, rejected = 0;
        for (auto& e : std::filesystem::directory_iterator(opt.fineDir)) {
            if (e.path().extension() != ".vxtl") continue;
            if (fine.loadTileFile(e.path())) ++loaded;
            else ++rejected;
        }
        std::printf("fine tiles: loaded %d, rejected %d from %s\n", loaded, rejected,
                    opt.fineDir.c_str());
        if (loaded == 0) {
            std::fprintf(stderr, "no fine tiles loaded; refusing to silently census "
                                 "synthetic ground under a --fine flag\n");
            return 1;
        }
        real = true;
    }
    Amplifier amp(opt.seed, real ? static_cast<ITileSampler&>(fine)
                                 : (realClimate ? static_cast<ITileSampler&>(coarse)
                                                : static_cast<ITileSampler&>(synth)));

    // THE PLACEMENT CHANNELS (worldgen v26). The water datum is the SAME
    // composed lake/river surface the renderer draws -- LakeSampler over the
    // basin rows, RiverSampler over the water plane -- never the debug water
    // marker; the distance/TWI/talus/curvature/heat planes come off the fine
    // tiles' SECTION_PLACE_* sections; the treeline comes from the same
    // climate the biome gate reads. All three fall away cleanly when their
    // tiles are absent: the channels then stay at fail-closed sentinels and
    // this probe censuses the pre-channel world, labelled below.
    LakeSampler lakeWater(fine);
    RiverSampler riverWater(fine);
    CompositeWaterSampler bakedWater(lakeWater, riverWater);
    IWaterSampler* water = real ? static_cast<IWaterSampler*>(&bakedWater) : nullptr;
    FineTileSampler* fineChannels = real ? &fine : nullptr;
    const auto channelsAt = [&](int64_t vx, int64_t vy) {
        // --no-channels: real ground, sentinel channels -- byte-for-byte the
        // facts the engine composed with while only the probe was wired to the
        // binding. That world, measured, is the BEFORE of the wiring fix.
        if (opt.noChannels) return AssetColumnChannels{};
        return assetColumnChannelsAt(fineChannels, water, tiles, vx, vy);
    };
    std::printf("terrain: %s elevation, %s climate, water datum %s, placement planes %s%s\n",
                real ? "REAL fine-tile" : "synthetic",
                realClimate ? "REAL coarse-tile" : "synthetic",
                water != nullptr ? "BAKED lake+river" : "NONE (dry world)",
                real ? "from fine tiles (sentinels where absent)" : "NONE (sentinels)",
                opt.noChannels ? "  [CHANNELS DISABLED -- pre-fix engine emulation]" : "");

    // =========================================================================
    // CENSUS 1: WIDENING
    // =========================================================================
    const int64_t r = opt.regionM;
    const int64_t cx0 = floorDiv((opt.originXM - r) * 1000, kChunkMm);
    const int64_t cx1 = floorDiv((opt.originXM + r) * 1000 - 1, kChunkMm);
    const int64_t cy0 = floorDiv((opt.originYM - r) * 1000, kChunkMm);
    const int64_t cy1 = floorDiv((opt.originYM + r) * 1000 - 1, kChunkMm);

    std::map<int32_t, int64_t> widenHist; // widening mm -> footprints
    std::map<int64_t, int64_t> extraHist; // extra admitted chunk layers -> footprints
    int64_t footprints = 0, declined = 0, extraSum = 0;
    int64_t dilationSlackMmSum = 0, wideningMmSum = 0, componentCounted = 0;
    // Baseline shell span, SAMPLED (32x32 columns per footprint is the whole
    // cost of this tool; one footprint in 64 keeps the census honest and fast).
    int64_t shellSampled = 0, shellLayersSum = 0;

    const auto boundFn = [&](int64_t vx0, int64_t vy0, int64_t vx1, int64_t vy1) {
        return amp.surfaceUpperBoundMm(vx0, vy0, vx1, vy1);
    };

    for (int64_t cy = cy0; cy <= cy1; ++cy) {
        for (int64_t cx = cx0; cx <= cx1; ++cx) {
            ++footprints;
            const AssetVoxelRect rect{cx * kChunkVox, cy * kChunkVox,
                                      cx * kChunkVox + kChunkVox - 1,
                                      cy * kChunkVox + kChunkVox - 1};
            const int32_t w =
                assetTopAboveSurfaceMm(opt.seed, layers.data(), int(layers.size()), rect);
            ++widenHist[w];

            const int64_t base = amp.surfaceUpperBoundMm(rect.vx0, rect.vy0, rect.vx1, rect.vy1);
            const int64_t aware = assetAwareSurfaceUpperBoundMm(
                opt.seed, layers.data(), int(layers.size()), rect, boundFn);
            if (base == kSurfaceBoundDeclined || aware == kSurfaceBoundDeclined) {
                ++declined;
                continue;
            }
            // Extra admitted chunk layers on the SKY side: the top chunk the
            // asset-aware bound admits minus the top chunk terrain alone
            // admits. This includes BOTH halves of the price -- the widening
            // and the reach dilation of the terrain bound query -- and the
            // two are also accumulated separately, because they have
            // different knobs: the widening is a layer's height cap, the
            // dilation slack is its RADIUS times the local terrain slope.
            const int64_t extra = floorDiv(aware, kChunkMm) - floorDiv(base, kChunkMm);
            ++extraHist[extra];
            extraSum += extra;
            const int64_t reachVox =
                int64_t(assetMaxReachMm(layers.data(), int(layers.size()))) /
                    int64_t(kVoxelSizeMm) + 1;
            const int64_t baseDil =
                amp.surfaceUpperBoundMm(rect.vx0 - reachVox, rect.vy0 - reachVox,
                                        rect.vx1 + reachVox, rect.vy1 + reachVox);
            if (baseDil != kSurfaceBoundDeclined) {
                dilationSlackMmSum += baseDil - base;
                wideningMmSum += w;
                ++componentCounted;
            }

            if (((cx - cx0) & 7) == 0 && ((cy - cy0) & 7) == 0) {
                // Baseline surface-shell span for scale: chunk layers between
                // the lowest and highest topmost-solid voxel in the footprint.
                int64_t vzMin = INT64_MAX, vzMax = INT64_MIN;
                for (int y = 0; y < kChunkVox; ++y)
                    for (int x = 0; x < kChunkVox; ++x) {
                        const int64_t top =
                            topSolidVoxelZ(amp.surfaceMm(rect.vx0 + x, rect.vy0 + y));
                        vzMin = top < vzMin ? top : vzMin;
                        vzMax = top > vzMax ? top : vzMax;
                    }
                shellLayersSum += floorDiv(vzMax, kChunkVox) - floorDiv(vzMin, kChunkVox) + 1;
                ++shellSampled;
            }
        }
    }

    std::printf("\n=== widening census: %lld footprints (%lld declined) over "
                "[%lld,%lld]x[%lld,%lld] m, seed %llu ===\n",
                (long long)footprints, (long long)declined,
                (long long)(opt.originXM - r), (long long)(opt.originXM + r),
                (long long)(opt.originYM - r), (long long)(opt.originYM + r),
                (unsigned long long)opt.seed);
    std::printf("widening (assetTopAboveSurfaceMm) distribution:\n");
    for (const auto& [w, n] : widenHist)
        std::printf("  %6d mm : %8lld footprints (%lld.%01lld%%)\n", w, (long long)n,
                    (long long)(n * 1000 / footprints / 10),
                    (long long)(n * 1000 / footprints % 10));
    std::printf("extra admitted chunk layers (sky side, dilation included):\n");
    for (const auto& [e, n] : extraHist)
        std::printf("  +%3lld chunks : %8lld footprints\n", (long long)e, (long long)n);
    const int64_t counted = footprints - declined;
    if (counted > 0)
        std::printf("mean extra: %lld.%02lld chunk layers per footprint\n",
                    (long long)(extraSum / counted),
                    (long long)((extraSum * 100 / counted) % 100));
    if (componentCounted > 0)
        std::printf("components: mean widening %lld mm (%lld.%01lld chunks), mean dilation "
                    "slack %lld mm (%lld.%01lld chunks)\n",
                    (long long)(wideningMmSum / componentCounted),
                    (long long)(wideningMmSum / componentCounted / kChunkMm),
                    (long long)(wideningMmSum * 10 / componentCounted / kChunkMm % 10),
                    (long long)(dilationSlackMmSum / componentCounted),
                    (long long)(dilationSlackMmSum / componentCounted / kChunkMm),
                    (long long)(dilationSlackMmSum * 10 / componentCounted / kChunkMm % 10));
    if (shellSampled > 0)
        std::printf("baseline surface shell (sampled %lld footprints): mean %lld.%02lld "
                    "chunk layers\n",
                    (long long)shellSampled, (long long)(shellLayersSum / shellSampled),
                    (long long)((shellLayersSum * 100 / shellSampled) % 100));

    // =========================================================================
    // CENSUS 2: PLACEMENT, only with banks
    // =========================================================================
    if (opt.banks.empty()) return 0;
    if (overridden) {
        std::fprintf(stderr, "layer overrides are pricing knobs for census 1 only; the "
                             "table was filed against the exported caps, so placement "
                             "under an overridden table would be a lie\n");
        return 2;
    }

    AssetBankLibrary lib;
    lib.configure(&manifest, opt.banks);
    AssetField field;
    field.setLayers(layers.data(), int(layers.size()));
    field.setSpecies(table.data(), int(table.size()));
    field.setBankSource(&lib);
    field.setSeed(opt.seed);

    const int64_t pr = opt.placeRegionM;
    const AssetVoxelRect prect{floorDiv((opt.originXM - pr) * 1000, int64_t(kVoxelSizeMm)),
                               floorDiv((opt.originYM - pr) * 1000, int64_t(kVoxelSizeMm)),
                               floorDiv((opt.originXM + pr) * 1000, int64_t(kVoxelSizeMm)),
                               floorDiv((opt.originYM + pr) * 1000, int64_t(kVoxelSizeMm))};

    const std::vector<AssetSite> sites =
        assetSitesForRect(opt.seed, layers.data(), int(layers.size()), prect);
    const std::vector<AssetInstance> instances =
        field.instancesForRect(prect, [&](int64_t vx, int64_t vy) {
            return assetColumnFactsFromSample(amp.columnCached(vx, vy), channelsAt(vx, vy));
        });

    int64_t perLayerSites[kAssetLayerCount] = {};
    for (const AssetSite& s : sites) ++perLayerSites[s.layer & (kAssetLayerCount - 1)];
    int64_t perLayer[kAssetLayerCount] = {};
    std::map<uint16_t, int64_t> perSpecies;
    int64_t anchorsAudited = 0, anchorsBad = 0;
    // THE SUBMERGED AUDIT, independently of the resolver, exactly like the
    // contact audit: re-ask the SAME water samplers the renderer draws from
    // whether each anchor stands under more than shin-deep water. Nonzero
    // here is the owner's lake-tree defect, and it must read 0.
    // (Deliberately NOT disabled by --no-channels: the audit measures against
    // the rendered datum, so the pre-fix run shows its submerged anchors as
    // the nonzero count they were on screen.)
    int64_t anchorsSubmerged = 0;
    // Inverse-height slope grading (owner rule): mean placed-species height
    // per anchor-slope bucket. Steeper buckets must trend SHORTER.
    int64_t slopeBucketCount[8] = {}, slopeBucketHeightMm[8] = {};
    // --- comparable counters (owner ask): kind / height / water reach /
    // slope bands / treeline, all from the SAME binding the engine composes
    // with, all landing in `counters` for --json/--compare. ------------------
    CounterMap counters;
    int64_t perKind[kAssetKindCount] = {};
    std::vector<int32_t> heightsMm;
    heightsMm.reserve(instances.size());
    int64_t waterLe2 = 0, waterLe10 = 0, waterLe30 = 0, waterLe120 = 0, waterFar = 0,
            waterUnknown = 0, wetAnchors = 0;
    // The owner's slope bands: 0-15 / 15-30 / 30-45 / 45-60 / 60+ %.
    int64_t slopeBand5[5] = {};
    int64_t treelineBelow = 0, treelineAbove = 0, treelineUnknown = 0;
    // --overlay: one CSV row per instance, written as the census walks.
    std::FILE* overlayCsv = nullptr;
    if (!opt.overlayBase.empty()) {
        const std::string p = opt.overlayBase + ".instances.csv";
        overlayCsv = std::fopen(p.c_str(), "w");
        if (overlayCsv == nullptr) {
            std::fprintf(stderr, "cannot write %s\n", p.c_str());
            return 1;
        }
        std::fprintf(overlayCsv,
                     "x_mm,y_mm,z_mm,layer,kind,species,riparian,height_mm,slope_mm_per_m,"
                     "dist_water_mm,standing_water_mm,treeline_delta_mm\n");
    }
    const std::vector<AssetManifestSpecies>& mrows = manifest.species();
    for (const AssetInstance& inst : instances) {
        ++perLayer[inst.layer & (kAssetLayerCount - 1)];
        ++perSpecies[inst.bankId];
        // THE CONTACT AUDIT, independently of the resolver: re-read the anchor
        // column and ask whether the voxel this instance stands on is solid.
        // The resolver already refused air anchors; this re-derivation is the
        // check that the refusal is WIRED, not merely written.
        const int64_t avx = floorDiv(inst.anchorXMm, int64_t(kVoxelSizeMm));
        const int64_t avy = floorDiv(inst.anchorYMm, int64_t(kVoxelSizeMm));
        const ColumnSample col = amp.column(avx, avy);
        ++anchorsAudited;
        if (Amplifier::materialAt(col, inst.anchorVz) == MAT_AIR) ++anchorsBad;
        if (water != nullptr) {
            const int32_t ws = water->waterSurfaceMmAtVoxel(avx, avy);
            if (ws != kNoWaterMm && int64_t(ws) > int64_t(inst.anchorZMm) + 300) {
                ++anchorsSubmerged;
            }
        }
        if (inst.speciesIndex < table.size()) {
            int b = int(col.slopeMmPerM / 150); // 15% grade per bucket
            if (b > 7) b = 7;
            if (b >= 0) {
                ++slopeBucketCount[b];
                slopeBucketHeightMm[b] += table[inst.speciesIndex].heightMm;
            }
        }
        // The comparable counters, from the engine-equivalent facts binding.
        const AssetColumnFacts f = assetColumnFactsFromSample(col, channelsAt(avx, avy));
        const AssetManifestSpecies* row = inst.bankId < mrows.size() ? &mrows[inst.bankId] : nullptr;
        int32_t heightMm = 0;
        if (row != nullptr) {
            ++perKind[uint32_t(row->kind) < kAssetKindCount ? uint32_t(row->kind) : 0];
            heightMm = row->heightMm;
            heightsMm.push_back(heightMm);
        }
        if (f.standingWaterMm > 0) ++wetAnchors;
        if (f.distanceToWaterMm == kAssetNoWaterDistanceMm) ++waterUnknown;
        else if (f.distanceToWaterMm <= 2000) ++waterLe2;
        else if (f.distanceToWaterMm <= 10000) ++waterLe10;
        else if (f.distanceToWaterMm <= 30000) ++waterLe30;
        else if (f.distanceToWaterMm <= 120000) ++waterLe120;
        else ++waterFar;
        {
            int b5 = int(col.slopeMmPerM / 150);
            if (b5 > 4) b5 = 4;
            if (b5 >= 0) ++slopeBand5[b5];
        }
        if (f.treelineDeltaMm == kAssetNoTreelineMm) ++treelineUnknown;
        else if (f.treelineDeltaMm < 0) ++treelineAbove;
        else ++treelineBelow;
        if (overlayCsv != nullptr) {
            std::fprintf(overlayCsv,
                         "%lld,%lld,%lld,%d,%s,%s,%d,%d,%lld,%lld,%d,%lld\n",
                         (long long)inst.anchorXMm, (long long)inst.anchorYMm,
                         (long long)inst.anchorZMm, int(inst.layer),
                         row != nullptr ? kindName(row->kind) : "?",
                         row != nullptr ? row->name.c_str() : "?",
                         (row != nullptr && row->waterMaxMm > 0) ? 1 : 0, heightMm,
                         (long long)col.slopeMmPerM,
                         f.distanceToWaterMm == kAssetNoWaterDistanceMm
                             ? -1LL
                             : (long long)f.distanceToWaterMm,
                         f.standingWaterMm,
                         f.treelineDeltaMm == kAssetNoTreelineMm ? 0LL
                                                                 : (long long)f.treelineDeltaMm);
        }
    }
    if (overlayCsv != nullptr) std::fclose(overlayCsv);

    // GATE ATTRIBUTION over the placement columns: which gate refused each
    // (site, species) pair on the site's own layer, through the SAME
    // assetSpeciesFirstRefusal the resolver's tolerates() wraps -- one
    // spelling, so this census cannot drift from the gate it measures. This is
    // the instrument "measure which gate binds on the ridges" asks for.
    int64_t gateCounts[size_t(AssetGate::kGateCount)] = {};
    int64_t pairsScanned = 0;
    for (const AssetSite& s : sites) {
        const int64_t avx = floorDiv(s.anchorXMm, int64_t(kVoxelSizeMm));
        const int64_t avy = floorDiv(s.anchorYMm, int64_t(kVoxelSizeMm));
        const AssetColumnFacts facts =
            assetColumnFactsFromSample(amp.columnCached(avx, avy), channelsAt(avx, avy));
        for (const AssetSpecies& sp : table) {
            if (int(sp.layer) != s.layer) continue;
            ++pairsScanned;
            ++gateCounts[size_t(assetSpeciesFirstRefusal(sp, facts))];
        }
    }

    std::printf("\n=== placement census over [%lld,%lld]x[%lld,%lld] m ===\n",
                (long long)(opt.originXM - pr), (long long)(opt.originXM + pr),
                (long long)(opt.originYM - pr), (long long)(opt.originYM + pr));
    std::printf("sites considered: %zu (per layer:", sites.size());
    for (int li = 0; li < kAssetLayerCount; ++li)
        std::printf(" L%d=%lld", li, (long long)perLayerSites[li]);
    std::printf(")\ninstances resolved: %zu (per layer:", instances.size());
    for (int li = 0; li < kAssetLayerCount; ++li)
        std::printf(" L%d=%lld", li, (long long)perLayer[li]);
    std::printf(")\nspecies represented: %zu\n", perSpecies.size());
    std::printf("anchor contact audit: %lld audited, %lld NOT SOLID\n",
                (long long)anchorsAudited, (long long)anchorsBad);
    if (water != nullptr)
        std::printf("submerged-anchor audit (vs the RENDERED water datum): %lld of %lld "
                    "stand in >300 mm of water%s\n",
                    (long long)anchorsSubmerged, (long long)anchorsAudited,
                    anchorsSubmerged == 0 ? "" : "  <-- LAKE-TREE DEFECT");

    // Per-species histogram, riparian species called out by name: the audit's
    // headline number is "112 water-gated species place nowhere", and this is
    // the counter that says whether serving the distance turned them on.
    {
        const std::vector<AssetManifestSpecies>& rows = manifest.species();
        int64_t riparianInstances = 0;
        int riparianSpecies = 0;
        std::printf("species histogram (name, instances; * = water-gated riparian):\n");
        for (const auto& [bankId, n] : perSpecies) {
            const bool rip = bankId < rows.size() && rows[bankId].waterMaxMm > 0;
            if (rip) { riparianInstances += n; ++riparianSpecies; }
            std::printf("  %c %-24s %6lld\n", rip ? '*' : ' ',
                        bankId < rows.size() ? rows[bankId].name.c_str() : "?", (long long)n);
        }
        std::printf("riparian (water-gated) species placed: %d species, %lld instances\n",
                    riparianSpecies, (long long)riparianInstances);
    }

    std::printf("gate attribution over %lld (site, species) pairs:\n", (long long)pairsScanned);
    for (size_t g = 0; g < size_t(AssetGate::kGateCount); ++g)
        if (gateCounts[g] > 0)
            std::printf("  %-28s %10lld (%lld.%01lld%%)\n",
                        assetGateName(AssetGate(g)), (long long)gateCounts[g],
                        (long long)(gateCounts[g] * 1000 / pairsScanned / 10),
                        (long long)(gateCounts[g] * 1000 / pairsScanned % 10));

    std::printf("mean placed-species height by anchor slope (inverse-height check):\n");
    for (int b = 0; b < 8; ++b) {
        if (slopeBucketCount[b] == 0) continue;
        std::printf("  slope %3d-%3d%%: %6lld instances, mean height %5.1f m\n", b * 15,
                    b == 7 ? 999 : (b + 1) * 15, (long long)slopeBucketCount[b],
                    double(slopeBucketHeightMm[b]) / double(slopeBucketCount[b]) / 1000.0);
    }

    // --- the comparable census (owner ask): one block of numbers a second
    // build can be diffed against, printed AND filed into `counters`. --------
    int64_t heightMeanMm = 0, heightMedianMm = 0;
    if (!heightsMm.empty()) {
        int64_t sum = 0;
        for (int32_t h : heightsMm) sum += h;
        heightMeanMm = sum / int64_t(heightsMm.size());
        std::nth_element(heightsMm.begin(), heightsMm.begin() + heightsMm.size() / 2,
                         heightsMm.end());
        heightMedianMm = heightsMm[heightsMm.size() / 2];
    }
    std::printf("kinds:");
    for (uint32_t k = 0; k < kAssetKindCount; ++k)
        if (perKind[k] > 0)
            std::printf(" %s=%lld", kindName(AssetKind(k)), (long long)perKind[k]);
    std::printf("\nheight: mean %.1f m, median %.1f m over %zu instances\n",
                double(heightMeanMm) / 1000.0, double(heightMedianMm) / 1000.0,
                instances.size());
    std::printf("distance to water at anchors: <=2m %lld, <=10m %lld, <=30m %lld, "
                "<=120m %lld, >120m %lld, unknown %lld; wet anchors %lld\n",
                (long long)waterLe2, (long long)waterLe10, (long long)waterLe30,
                (long long)waterLe120, (long long)waterFar, (long long)waterUnknown,
                (long long)wetAnchors);
    std::printf("slope bands: 0-15%% %lld, 15-30%% %lld, 30-45%% %lld, 45-60%% %lld, "
                "60%%+ %lld\n",
                (long long)slopeBand5[0], (long long)slopeBand5[1], (long long)slopeBand5[2],
                (long long)slopeBand5[3], (long long)slopeBand5[4]);
    std::printf("treeline: below %lld, above %lld, unknown %lld\n",
                (long long)treelineBelow, (long long)treelineAbove,
                (long long)treelineUnknown);

    // Stamp census: real voxels through the same composition GeneratedWorld
    // uses, over every brick that intersects the placed instances' boxes --
    // approximated here by walking the placement rect's surface bricks.
    int64_t stamped = 0;
    for (const AssetInstance& inst : instances) {
        const AssetGrid* g = lib.bankGrid(inst.bankId, inst.seedIndex);
        if (g == nullptr) continue;
        // Count the instance's own solid voxels once; the composition into
        // bricks is pinned by test_assetfield, so the census counts what the
        // world will actually gain.
        if (layers[inst.layer].terrainLattice) stamped += int64_t(g->solidCount());
    }
    std::printf("stampable voxels over resolved terrain instances: %lld\n",
                (long long)stamped);

    const AssetBankLibrary::Stats bs = lib.stats();
    std::printf("banks: %llu requests, %llu served, %llu miss(no species), %llu miss(no "
                "seeds), %llu files loaded, %llu refused, %llu seed-count mismatches, "
                "%llu bytes resident\n",
                (unsigned long long)bs.requests, (unsigned long long)bs.servedGrids,
                (unsigned long long)bs.missNoSpecies, (unsigned long long)bs.missNoSeeds,
                (unsigned long long)bs.filesLoaded, (unsigned long long)bs.filesRefused,
                (unsigned long long)bs.seedCountMismatch,
                (unsigned long long)bs.bytesResident);
    for (int e = 0; e <= int(AssetBankError::kTooWide); ++e)
        if (bs.refusedBy[e] > 0)
            std::printf("  refused %llu: %s\n", (unsigned long long)bs.refusedBy[e],
                        assetBankErrorText(AssetBankError(e)));

    // --- the overlay's ground raster: elevation / water datum / shore
    // distance / treeline over the placement region, at the fine tiles' own
    // 1.875 m pitch. The PROBE samples the ground (fine tiles + the same
    // composed water datum placement reads); the overlay tool only draws --
    // never rebuild ground in Python. -----------------------------------------
    if (!opt.overlayBase.empty()) {
        if (!real) {
            // No fine tiles: write the raster from the SAME amplified-coarse
            // ground the placement census above resolved against (amp over the
            // coarse tiles). This is the engine's own surface function, not a
            // rebuild -- but it is NOT the fine bake: no baked lake/river
            // datum (sea only) and no shore-distance plane (-1 = unknown).
            // Labelled below so a map from this branch can never be mistaken
            // for one over baked fine tiles.
            const int32_t pxMm = 1875; // match the fine tiles' pitch for a comparable map
            const int64_t x0Mm = (opt.originXM - pr) * 1000, x1Mm = (opt.originXM + pr) * 1000;
            const int64_t y0Mm = (opt.originYM - pr) * 1000, y1Mm = (opt.originYM + pr) * 1000;
            const int64_t px0 = floorDiv(x0Mm, int64_t(pxMm)), px1 = floorDiv(x1Mm, int64_t(pxMm));
            const int64_t py0 = floorDiv(y0Mm, int64_t(pxMm)), py1 = floorDiv(y1Mm, int64_t(pxMm));
            const uint32_t W = uint32_t(px1 - px0 + 1), H = uint32_t(py1 - py0 + 1);
            const std::string p = opt.overlayBase + ".ground.bin";
            std::FILE* gf = std::fopen(p.c_str(), "wb");
            if (gf == nullptr) {
                std::fprintf(stderr, "cannot write %s\n", p.c_str());
                return 1;
            }
            const uint32_t ver = 1, planes = 4;
            const int64_t gx0Mm = px0 * pxMm, gy0Mm = py0 * pxMm;
            const int32_t stepMm = pxMm;
            std::fwrite("VXOV", 1, 4, gf);
            std::fwrite(&ver, 4, 1, gf);
            std::fwrite(&gx0Mm, 8, 1, gf);
            std::fwrite(&gy0Mm, 8, 1, gf);
            std::fwrite(&stepMm, 4, 1, gf);
            std::fwrite(&W, 4, 1, gf);
            std::fwrite(&H, 4, 1, gf);
            std::fwrite(&planes, 4, 1, gf);
            std::vector<int32_t> elevP(size_t(W) * H), waterP(size_t(W) * H),
                distP(size_t(W) * H), treeP(size_t(W) * H);
            for (int64_t py = py0; py <= py1; ++py) {
                for (int64_t px = px0; px <= px1; ++px) {
                    const size_t i = size_t(px - px0) + size_t(W) * size_t(py - py0);
                    const int64_t vx = floorDiv(px * pxMm + pxMm / 2, int64_t(kVoxelSizeMm));
                    const int64_t vy = floorDiv(py * pxMm + pxMm / 2, int64_t(kVoxelSizeMm));
                    const int32_t elev = amp.surfaceMm(vx, vy);
                    // Sea is the only datum this branch knows.
                    const int32_t w = elev < kSeaLevelMm ? kSeaLevelMm : kNoWaterMm;
                    int32_t tl = INT32_MIN;
                    {
                        const int64_t cMm = int64_t(tiles->pixelSizeMm());
                        if (cMm > 0) {
                            const ClimateSample c =
                                tiles->climate(floorDiv(vx * kVoxelSizeMm, cMm),
                                               floorDiv(vy * kVoxelSizeMm, cMm));
                            tl = biomeTreelineMm(c.temperature);
                        }
                    }
                    elevP[i] = elev;
                    waterP[i] = w;
                    distP[i] = -1; // shore distance is a fine-tile plane; unknown here
                    treeP[i] = tl;
                }
            }
            std::fwrite(elevP.data(), 4, elevP.size(), gf);
            std::fwrite(waterP.data(), 4, waterP.size(), gf);
            std::fwrite(distP.data(), 4, distP.size(), gf);
            std::fwrite(treeP.data(), 4, treeP.size(), gf);
            std::fclose(gf);
            std::printf("overlay: wrote %s.instances.csv and %s (%ux%u @ %d mm, "
                        "AMPLIFIED-COARSE ground: sea-only datum, no shore distance)\n",
                        opt.overlayBase.c_str(), p.c_str(), W, H, stepMm);
        } else {
            const int32_t pxMm = fine.pixelSizeMm();
            const int64_t x0Mm = (opt.originXM - pr) * 1000, x1Mm = (opt.originXM + pr) * 1000;
            const int64_t y0Mm = (opt.originYM - pr) * 1000, y1Mm = (opt.originYM + pr) * 1000;
            const int64_t px0 = floorDiv(x0Mm, int64_t(pxMm)), px1 = floorDiv(x1Mm, int64_t(pxMm));
            const int64_t py0 = floorDiv(y0Mm, int64_t(pxMm)), py1 = floorDiv(y1Mm, int64_t(pxMm));
            const uint32_t W = uint32_t(px1 - px0 + 1), H = uint32_t(py1 - py0 + 1);
            const std::string p = opt.overlayBase + ".ground.bin";
            std::FILE* gf = std::fopen(p.c_str(), "wb");
            if (gf == nullptr) {
                std::fprintf(stderr, "cannot write %s\n", p.c_str());
                return 1;
            }
            const uint32_t ver = 1, planes = 4;
            const int64_t gx0Mm = px0 * pxMm, gy0Mm = py0 * pxMm;
            const int32_t stepMm = pxMm;
            std::fwrite("VXOV", 1, 4, gf);
            std::fwrite(&ver, 4, 1, gf);
            std::fwrite(&gx0Mm, 8, 1, gf);
            std::fwrite(&gy0Mm, 8, 1, gf);
            std::fwrite(&stepMm, 4, 1, gf);
            std::fwrite(&W, 4, 1, gf);
            std::fwrite(&H, 4, 1, gf);
            std::fwrite(&planes, 4, 1, gf);
            std::vector<int32_t> elevP(size_t(W) * H), waterP(size_t(W) * H),
                distP(size_t(W) * H), treeP(size_t(W) * H);
            for (int64_t py = py0; py <= py1; ++py) {
                for (int64_t px = px0; px <= px1; ++px) {
                    const size_t i = size_t(px - px0) + size_t(W) * size_t(py - py0);
                    const int32_t elev = fine.elevationMm(px, py);
                    // Voxel column under the pixel centre -- the same
                    // addressing the channel binding uses.
                    const int64_t vx = floorDiv(px * pxMm + pxMm / 2, int64_t(kVoxelSizeMm));
                    const int64_t vy = floorDiv(py * pxMm + pxMm / 2, int64_t(kVoxelSizeMm));
                    int32_t w = water != nullptr ? water->waterSurfaceMmAtVoxel(vx, vy)
                                                 : kNoWaterMm;
                    // The sea is the datum, composed exactly as the facts
                    // binding composes it (assetfield.h).
                    if (elev < kSeaLevelMm && (w == kNoWaterMm || w < kSeaLevelMm)) {
                        w = kSeaLevelMm;
                    }
                    const FineTileSampler::FinePlacementSample ps = fine.placementAtVoxel(vx, vy);
                    const int32_t d = ps.valid ? placementDistanceMm(ps.distWater)
                                               : kAssetNoWaterDistanceMm;
                    int32_t tl = INT32_MIN;
                    {
                        const int64_t cMm = int64_t(tiles->pixelSizeMm());
                        if (cMm > 0) {
                            const ClimateSample c =
                                tiles->climate(floorDiv(vx * kVoxelSizeMm, cMm),
                                               floorDiv(vy * kVoxelSizeMm, cMm));
                            tl = biomeTreelineMm(c.temperature);
                        }
                    }
                    elevP[i] = elev;
                    waterP[i] = w;
                    distP[i] = d == kAssetNoWaterDistanceMm ? -1 : d;
                    treeP[i] = tl;
                }
            }
            std::fwrite(elevP.data(), 4, elevP.size(), gf);
            std::fwrite(waterP.data(), 4, waterP.size(), gf);
            std::fwrite(distP.data(), 4, distP.size(), gf);
            std::fwrite(treeP.data(), 4, treeP.size(), gf);
            std::fclose(gf);
            std::printf("overlay: wrote %s.instances.csv and %s (%ux%u @ %d mm)\n",
                        opt.overlayBase.c_str(), p.c_str(), W, H, stepMm);
        }
    }

    // --- the counters object: every number above, flat-keyed, so two builds
    // diff numerically (--json to file it, --compare to read one back). ------
    {
        counters["sites"] = (long long)sites.size();
        counters["instances"] = (long long)instances.size();
        counters["species_represented"] = (long long)perSpecies.size();
        for (int li = 0; li < kAssetLayerCount; ++li) {
            counters["layer.L" + std::to_string(li)] = perLayer[li];
            counters["sites.L" + std::to_string(li)] = perLayerSites[li];
        }
        for (uint32_t k = 0; k < kAssetKindCount; ++k)
            counters[std::string("kind.") + kindName(AssetKind(k))] = perKind[k];
        counters["height.mean_mm"] = heightMeanMm;
        counters["height.median_mm"] = heightMedianMm;
        counters["water.d_le_2m"] = waterLe2;
        counters["water.d_le_10m"] = waterLe10;
        counters["water.d_le_30m"] = waterLe30;
        counters["water.d_le_120m"] = waterLe120;
        counters["water.d_gt_120m"] = waterFar;
        counters["water.d_unknown"] = waterUnknown;
        counters["water.wet_anchors"] = wetAnchors;
        counters["slope.0_15"] = slopeBand5[0];
        counters["slope.15_30"] = slopeBand5[1];
        counters["slope.30_45"] = slopeBand5[2];
        counters["slope.45_60"] = slopeBand5[3];
        counters["slope.60_plus"] = slopeBand5[4];
        counters["treeline.below"] = treelineBelow;
        counters["treeline.above"] = treelineAbove;
        counters["treeline.unknown"] = treelineUnknown;
        counters["audit.anchors"] = anchorsAudited;
        counters["audit.floating"] = anchorsBad;
        counters["audit.submerged"] = anchorsSubmerged;
        {
            int64_t ripInst = 0, ripSpecies = 0;
            for (const auto& [bankId, n] : perSpecies) {
                if (bankId < mrows.size() && mrows[bankId].waterMaxMm > 0) {
                    ripInst += n;
                    ++ripSpecies;
                }
            }
            counters["riparian.species"] = ripSpecies;
            counters["riparian.instances"] = ripInst;
        }
        for (size_t g = 0; g < size_t(AssetGate::kGateCount); ++g) {
            std::string k = std::string("gate.") + assetGateName(AssetGate(g));
            for (char& c : k)
                if (c == ' ' || c == '(' || c == ')' || c == '/') c = '_';
            counters[k] = gateCounts[g];
        }
        for (const auto& [bankId, n] : perSpecies)
            counters["species." + (bankId < mrows.size() ? mrows[bankId].name
                                                         : std::to_string(bankId))] = n;
    }
    if (!opt.jsonPath.empty()) {
        std::FILE* jf = std::fopen(opt.jsonPath.c_str(), "w");
        if (jf == nullptr) {
            std::fprintf(stderr, "cannot write %s\n", opt.jsonPath.c_str());
            return 1;
        }
        std::fprintf(jf, "{\n");
        std::fprintf(jf,
                     "  \"site\": {\"origin_x_m\": %lld, \"origin_y_m\": %lld, "
                     "\"place_region_m\": %lld, \"seed\": %llu},\n",
                     (long long)opt.originXM, (long long)opt.originYM, (long long)pr,
                     (unsigned long long)opt.seed);
        std::fprintf(jf,
                     "  \"conditions\": {\"elevation\": \"%s\", \"climate\": \"%s\", "
                     "\"water_datum\": \"%s\", \"channels\": %s},\n",
                     real ? "fine" : "synthetic", realClimate ? "coarse" : "synthetic",
                     water != nullptr ? "baked" : "none",
                     opt.noChannels ? "false" : "true");
        std::fprintf(jf, "  \"counters\": {\n");
        size_t left = counters.size();
        for (const auto& [k, v] : counters)
            std::fprintf(jf, "    \"%s\": %lld%s\n", k.c_str(), v, --left ? "," : "");
        std::fprintf(jf, "  }\n}\n");
        std::fclose(jf);
        std::printf("json: wrote %s (%zu counters)\n", opt.jsonPath.c_str(), counters.size());
    }
    if (!opt.comparePath.empty()) {
        CounterMap before;
        if (!readCountersJson(opt.comparePath, before)) {
            std::fprintf(stderr, "cannot read %s\n", opt.comparePath.c_str());
            return 1;
        }
        std::printf("\n=== delta vs %s (before -> after) ===\n", opt.comparePath.c_str());
        // Union of keys; species rows only when they moved, everything else
        // always -- the zero rows are the claim "nothing changed", stated.
        int64_t moved = 0;
        for (const auto& [k, v] : counters) {
            const auto it = before.find(k);
            const long long b = it != before.end() ? it->second : 0;
            if (k.rfind("species.", 0) == 0 && b == v) continue;
            if (b != v) ++moved;
            std::printf("  %-40s %10lld -> %10lld  (%+lld)\n", k.c_str(), b, v, v - b);
        }
        for (const auto& [k, b] : before) {
            if (counters.count(k)) continue;
            ++moved;
            std::printf("  %-40s %10lld -> %10d  (%+lld)\n", k.c_str(), b, 0, -b);
        }
        std::printf("  (%lld counters moved)\n", (long long)moved);
    }

    // The one-line verdicts the failure table asks for.
    if (!instances.empty() && anchorsBad == 0 && anchorsSubmerged == 0)
        std::printf("VERDICT: placement wired; %zu instances, all anchors solid, none "
                    "submerged\n",
                    instances.size());
    else if (instances.empty())
        std::printf("VERDICT: NOTHING PLACED against %zu sites -- wiring fault until "
                    "proven otherwise\n",
                    sites.size());
    else if (anchorsBad > 0)
        std::printf("VERDICT: %lld FLOATING ANCHORS -- the anti-float guard is not "
                    "wired\n",
                    (long long)anchorsBad);
    else
        std::printf("VERDICT: %lld SUBMERGED ANCHORS -- the standing-water veto is not "
                    "wired to the rendered water datum\n",
                    (long long)anchorsSubmerged);
    return (anchorsBad == 0 && anchorsSubmerged == 0) ? 0 : 1;
}
