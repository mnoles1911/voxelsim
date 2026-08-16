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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/assetbank.h"
#include "voxelcore/assetfield.h"
#include "voxelcore/assetmanifest.h"
#include "voxelcore/tilestore.h"

using namespace vxc;

namespace {

struct Options {
    std::string manifest;
    std::string banks;
    std::string fineDir;
    uint64_t seed = 20260719;
    int64_t regionM = 512;    // half-edge of the census square, metres
    int64_t originXM = 0, originYM = 0;
    int32_t l1capMm = 0;      // 0 = as exported
    int64_t placeRegionM = 64; // half-edge of the placement census square
    // Pricing overrides for census 1 (placement refuses to run under them):
    int32_t l0capMm = 0, l0radiusMm = 0, l1radiusMm = 0;
    int32_t l0density = -1; // per-mille; -1 = as exported
};

constexpr int kChunkVox = 32;                       // level-0 render chunk edge
constexpr int64_t kChunkMm = int64_t(kChunkVox) * kVoxelSizeMm; // 3200

} // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--manifest" && i + 1 < argc) opt.manifest = argv[++i];
        else if (a == "--banks" && i + 1 < argc) opt.banks = argv[++i];
        else if (a == "--fine" && i + 1 < argc) opt.fineDir = argv[++i];
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
        else {
            std::fprintf(stderr,
                         "usage: vxc_assetprobe --manifest species.vxm [--banks dir] "
                         "[--fine dir] [--seed n] [--region m] [--origin-x m] "
                         "[--origin-y m] [--l1cap mm] [--place-region m]\n");
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
    SyntheticTileSampler synth(opt.seed);
    ITileSampler* tiles = &synth;
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
                                 : static_cast<ITileSampler&>(synth));
    std::printf("terrain: %s elevation, %s climate\n", real ? "REAL fine-tile" : "synthetic",
                "synthetic"); // no coarse tiles are wired here yet; say so

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
            return assetColumnFactsFromSample(amp.columnCached(vx, vy));
        });

    int64_t perLayerSites[kAssetLayerCount] = {};
    for (const AssetSite& s : sites) ++perLayerSites[s.layer & (kAssetLayerCount - 1)];
    int64_t perLayer[kAssetLayerCount] = {};
    std::map<uint16_t, int64_t> perSpecies;
    int64_t anchorsAudited = 0, anchorsBad = 0;
    for (const AssetInstance& inst : instances) {
        ++perLayer[inst.layer & (kAssetLayerCount - 1)];
        ++perSpecies[inst.bankId];
        // THE CONTACT AUDIT, independently of the resolver: re-read the anchor
        // column and ask whether the voxel this instance stands on is solid.
        // The resolver already refused air anchors; this re-derivation is the
        // check that the refusal is WIRED, not merely written.
        const ColumnSample col = amp.column(
            floorDiv(inst.anchorXMm, int64_t(kVoxelSizeMm)),
            floorDiv(inst.anchorYMm, int64_t(kVoxelSizeMm)));
        ++anchorsAudited;
        if (Amplifier::materialAt(col, inst.anchorVz) == MAT_AIR) ++anchorsBad;
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

    // The one-line verdicts the failure table asks for.
    if (!instances.empty() && anchorsBad == 0)
        std::printf("VERDICT: placement wired; %zu instances, all anchors solid\n",
                    instances.size());
    else if (instances.empty())
        std::printf("VERDICT: NOTHING PLACED against %zu sites -- wiring fault until "
                    "proven otherwise\n",
                    sites.size());
    else
        std::printf("VERDICT: %lld FLOATING ANCHORS -- the anti-float guard is not "
                    "wired\n",
                    (long long)anchorsBad);
    return anchorsBad == 0 ? 0 : 1;
}
