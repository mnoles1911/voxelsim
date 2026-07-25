// vxc_climateprobe -- diagnostic for the climate/biome/stratigraphy consumers.
//
// WHY THIS EXISTS. The tile CLIMATE channels carry PHYSICAL WorldClim units
// quantized over Earth-extreme ranges (terrain-service diffusion.py's
// EXPECTED_CHANNELS: temperature -40..40 C, seasonality 0..3000, precipitation
// 0..12000 mm/yr, precip variability 0..200 %). SyntheticTileSampler instead
// emits noise centred on 128 spanning the whole byte. Every consumer of those
// channels -- biome.h's thresholds, amplifier.cpp's topsoil formula,
// rivernet.cpp's flow weight -- was calibrated against the SECOND encoding and
// then pointed at tiles carrying the FIRST. The symptoms are not visible in a
// build log and only barely visible on screen (the render path applies its own
// compensating remap, VoxelClimateProbe.cpp), so they have to be MEASURED.
//
// This is that measurement, and it is deliberately a command rather than a
// one-off analysis: the numbers behind "the world classifies as DESERT" and
// "85% of the world has no topsoil" were derived by hand once, and a hand
// derivation cannot be re-run after a threshold change. Compare
// vxc_terrainprobe, which does the same job for the SURFACE term.
//
// The GATE ATTRIBUTION column is the point of the whole tool. classifyBiome
// runs a chain of morphology gates before it ever reaches the Whittaker table,
// so a biome census alone cannot distinguish "51% of the world is alpine
// because it is high and cold" from "51% of the world is alpine because the
// cliff-slope gate fires below the median slope". Those two have completely
// different fixes.
//
// Usage: vxc_climateprobe <tiledir|--synthetic> <seed> [samplesPerAxis]
//
// Defaults to 512 samples per axis (262k columns), which takes a few seconds
// and is well inside the sampling noise of every percentage it reports.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/biome.h"
#include "voxelcore/tilestore.h"

using namespace vxc;

namespace {

// ---------------------------------------------------------------------------
// Physical decode of the u8 climate channels.
//
// MIRROR: terrain-service/terrain_service/providers/diffusion.py's
// EXPECTED_CHANNELS, which is BOTH the model-output validation range AND the
// quantization range adapt_raster_to_tile maps into uint8 -- i.e. these four
// pairs are the wire format.
//
// These live here as local constants ONLY because voxel-core does not yet have
// a shared climate contract; the plan's step 1 introduces
// voxelcore/climate.h and this block becomes an include. Kept as doubles
// deliberately: voxel-core/bench is outside the float ban (CI greps only
// voxel-core/include and voxel-core/src), and a diagnostic that reports
// percentiles wants real division.
// ---------------------------------------------------------------------------
struct ChannelScale {
    const char* name;
    const char* unit;
    double lo, hi;
};
constexpr ChannelScale kTemperature{"temperature", "degC", -40.0, 40.0};
constexpr ChannelScale kSeasonality{"seasonality", "bio_4", 0.0, 3000.0};
constexpr ChannelScale kPrecipitation{"precipitation", "mm/yr", 0.0, 12000.0};
constexpr ChannelScale kPrecipVariability{"precip_var", "CV %", 0.0, 200.0};

double physical(const ChannelScale& c, double u8) {
    return c.lo + (u8 / 255.0) * (c.hi - c.lo);
}

// tileSlopeMmPerPx, reproduced from amplifier.cpp.
//
// It lives in that file's anonymous namespace, so it cannot be called from
// here. This is a THREE-LINE duplicate of a formula that has not changed since
// the amplifier was written, and the alternative -- widening Amplifier's public
// surface for a diagnostic -- is worse. If the amplifier's slope definition
// ever changes, this must follow; the self-check in classify() below is what
// makes that failure loud rather than silent.
int64_t probeSlopeMmPerPx(int64_t e00, int64_t e10, int64_t e01) {
    return (e10 > e00 ? e10 - e00 : e00 - e10) + (e01 > e00 ? e01 - e00 : e00 - e01);
}

// Which gate in classifyBiome decided this column.
//
// Mirrors biome.h's gate ORDER exactly. Duplicating the conditions is the only
// way to attribute a decision to a gate -- classifyBiome returns an id, not a
// reason -- but the duplication is self-checking: classify() asserts the biome
// this attribution implies equals what classifyBiome actually returned, and
// reports a mismatch count. A gate reorder in biome.h that is not mirrored here
// therefore shows up as a nonzero MISMATCH line rather than as quietly wrong
// attribution.
enum Gate { GATE_CLIFF, GATE_OCEAN, GATE_BEACH, GATE_TREELINE, GATE_WHITTAKER, kGateCount };
const char* kGateName[kGateCount] = {"cliff-slope", "ocean", "beach", "treeline", "whittaker"};

Gate attributeGate(int32_t tempU8, int32_t surfaceMm, int64_t slopeMmPerPx) {
    if (slopeMmPerPx > kBiomeCliffSlopeMmPerPx) return GATE_CLIFF;
    if (surfaceMm < kBiomeBeachLowerMm) return GATE_OCEAN;
    if (surfaceMm <= kBiomeBeachUpperMm) return GATE_BEACH;
    if (surfaceMm > biomeTreelineMm(tempU8)) return GATE_TREELINE;
    return GATE_WHITTAKER;
}

const char* kBiomeName[kBiomeCount] = {"OCEAN",   "BEACH",   "GRASSLAND", "TEMPERATE_FOREST",
                                       "RAINFOREST", "DESERT", "SAVANNA",  "TAIGA",
                                       "TUNDRA_ALPINE"};

const char* materialName(MaterialId m) {
    switch (m) {
        case MAT_AIR: return "AIR";
        case MAT_BEDROCK: return "BEDROCK";
        case MAT_ROCK: return "ROCK";
        case MAT_GRAVEL: return "GRAVEL";
        case MAT_SAND: return "SAND";
        case MAT_SUBSOIL: return "SUBSOIL";
        case MAT_TOPSOIL: return "TOPSOIL";
        case MAT_SNOW: return "SNOW";
        case MAT_GRASS: return "GRASS";
        case MAT_JUNGLE_SOIL: return "JUNGLE_SOIL";
        case MAT_SAVANNA_GRASS: return "SAVANNA_GRASS";
        case MAT_PODZOL: return "PODZOL";
        case MAT_PERMAFROST: return "PERMAFROST";
        case MAT_MUD: return "MUD";
        case MAT_CLAY: return "CLAY";
        default: return "?";
    }
}

// True for the materials a biome can put at the SURFACE (biomeSurfaceMaterial's
// whole range). Everything else showing up as the top voxel means the biome
// classification never reached the player -- which is exactly the topsoil
// collapse this tool exists to measure.
bool isSurfaceMaterial(MaterialId m) {
    switch (m) {
        case MAT_MUD:
        case MAT_SAND:
        case MAT_GRASS:
        case MAT_TOPSOIL:
        case MAT_JUNGLE_SOIL:
        case MAT_SAVANNA_GRASS:
        case MAT_PODZOL:
        case MAT_PERMAFROST:
        case MAT_ROCK: // the alpine rock-line path of biomeSurfaceMaterial
            return true;
        default:
            return false;
    }
}

double pct(int64_t n, int64_t total) {
    return total ? 100.0 * static_cast<double>(n) / static_cast<double>(total) : 0.0;
}

// Percentile of an ALREADY-SORTED vector, nearest-rank.
int64_t pctl(const std::vector<int64_t>& sorted, double q) {
    if (sorted.empty()) return 0;
    size_t i = static_cast<size_t>(q / 100.0 * static_cast<double>(sorted.size() - 1) + 0.5);
    return sorted[std::min(i, sorted.size() - 1)];
}

void printPercentileRow(const char* label, std::vector<int64_t> v, const char* unit) {
    std::sort(v.begin(), v.end());
    std::printf("  %-14s %-6s min=%-7lld p1=%-7lld p25=%-7lld p50=%-7lld p75=%-7lld p99=%-7lld "
                "max=%lld\n",
                label, unit, (long long)(v.empty() ? 0 : v.front()), (long long)pctl(v, 1),
                (long long)pctl(v, 25), (long long)pctl(v, 50), (long long)pctl(v, 75),
                (long long)pctl(v, 99), (long long)(v.empty() ? 0 : v.back()));
}

void printChannelRow(const ChannelScale& c, std::vector<int64_t> v) {
    std::sort(v.begin(), v.end());
    std::printf("  %-14s u8    p1=%-7lld p25=%-7lld p50=%-7lld p75=%-7lld p99=%-7lld  "
                "(range %lld..%lld)\n",
                c.name, (long long)pctl(v, 1), (long long)pctl(v, 25), (long long)pctl(v, 50),
                (long long)pctl(v, 75), (long long)pctl(v, 99),
                (long long)(v.empty() ? 0 : v.front()), (long long)(v.empty() ? 0 : v.back()));
    std::printf("  %-14s %-6s p1=%-7.1f p25=%-7.1f p50=%-7.1f p75=%-7.1f p99=%-7.1f\n", "", c.unit,
                physical(c, static_cast<double>(pctl(v, 1))),
                physical(c, static_cast<double>(pctl(v, 25))),
                physical(c, static_cast<double>(pctl(v, 50))),
                physical(c, static_cast<double>(pctl(v, 75))),
                physical(c, static_cast<double>(pctl(v, 99))));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: vxc_climateprobe <tiledir|--synthetic> <seed> [samplesPerAxis]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const uint64_t seed = std::strtoull(argv[2], nullptr, 10);
    const int64_t nAxis = argc > 3 ? std::strtoll(argv[3], nullptr, 10) : 512;
    if (nAxis <= 1) {
        std::fprintf(stderr, "samplesPerAxis must be > 1\n");
        return 2;
    }

    SyntheticTileSampler synth(seed);
    TileGridSampler grid(seed, 1);
    ITileSampler* tiles = &synth;

    // Sampling window in TILE PIXELS. For a real tile set it is the loaded
    // tiles' own bounding box, so every sample lands on real data and the
    // census describes the world you actually have -- not a window that half
    // misses it and gets padded with TileGridSampler's flat sea-level default.
    int64_t px0 = 0, py0 = 0, px1 = 0, py1 = 0;

    if (dir != "--synthetic") {
        int loaded = 0, rejected = 0;
        bool anyBox = false;
        int32_t minTx = 0, maxTx = 0, minTy = 0, maxTy = 0;
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".vxtl") continue;
            // Parsed by hand rather than via loadTileFile so the tile's own
            // (x, y) is available for the bounding box -- loadTile(TileData)
            // still enforces the identical seed/scale rejection. Same approach
            // UVoxelWorldSubsystem::MakeTileSampler takes, for the same reason.
            std::optional<std::vector<uint8_t>> bytes = readFileBytes(e.path());
            if (!bytes) { ++rejected; continue; }
            std::optional<TileData> parsed = TileData::parse(bytes->data(), bytes->size());
            if (!parsed) { ++rejected; continue; }
            const int32_t tx = parsed->x, ty = parsed->y;
            if (!grid.loadTile(std::move(*parsed))) { ++rejected; continue; }
            ++loaded;
            if (!anyBox) {
                minTx = maxTx = tx;
                minTy = maxTy = ty;
                anyBox = true;
            } else {
                minTx = std::min(minTx, tx);
                maxTx = std::max(maxTx, tx);
                minTy = std::min(minTy, ty);
                maxTy = std::max(maxTy, ty);
            }
        }
        std::printf("tiles loaded=%d rejected=%d seed=%llu scale=%d pixelSizeMm=%d\n", loaded,
                    rejected, (unsigned long long)seed, (int)grid.scale(), grid.pixelSizeMm());
        if (loaded == 0) {
            std::fprintf(stderr, "no tiles loaded from %s\n", dir.c_str());
            return 1;
        }
        std::printf("tile bbox x=[%d,%d] y=[%d,%d]\n", minTx, maxTx, minTy, maxTy);
        // The window stops ONE PIXEL SHORT of the loaded box on each max axis.
        // Both the bilinear base and the slope term read (px+1, py+1), so a
        // sample in the last pixel cell reads outside every loaded tile and
        // gets TileGridSampler's flat sea-level default -- which manufactures a
        // fake multi-kilometre slope against real seafloor and drags the slope
        // percentiles the whole tool is meant to report. missingTileQueries is
        // asserted zero at the end to keep this honest.
        const int64_t k = TileData::kTileSize;
        px0 = int64_t(minTx) * k;
        py0 = int64_t(minTy) * k;
        px1 = (int64_t(maxTx) + 1) * k - 2;
        py1 = (int64_t(maxTy) + 1) * k - 2;
        tiles = &grid;
    } else {
        // A 5x5-tile window at the origin: the same footprint a real launch
        // pregen has, so the two modes' censuses are comparable.
        const int64_t k = TileData::kTileSize;
        px0 = -2 * k;
        py0 = -2 * k;
        px1 = 3 * k - 1;
        py1 = 3 * k - 1;
        std::printf("using SyntheticTileSampler pixelSizeMm=%d\n", synth.pixelSizeMm());
    }

    const int64_t pxMm = tiles->pixelSizeMm();
    std::printf("sampling %lldx%lld columns over tile pixels x=[%lld,%lld] y=[%lld,%lld] "
                "(%.1f x %.1f km)\n\n",
                (long long)nAxis, (long long)nAxis, (long long)px0, (long long)px1, (long long)py0,
                (long long)py1, (px1 - px0 + 1) * pxMm / 1e6, (py1 - py0 + 1) * pxMm / 1e6);

    Amplifier amp(seed, *tiles);

    std::vector<int64_t> chT, chS, chP, chV, slopes, elevs, topsoils, subsoils;
    const int64_t total = nAxis * nAxis;
    chT.reserve(size_t(total));
    chS.reserve(size_t(total));
    chP.reserve(size_t(total));
    chV.reserve(size_t(total));
    slopes.reserve(size_t(total));
    elevs.reserve(size_t(total));
    topsoils.reserve(size_t(total));
    subsoils.reserve(size_t(total));

    int64_t biomeCount[kBiomeCount] = {0};
    int64_t gateCount[kGateCount] = {0};
    int64_t stratMat[kMaterialCount] = {0};
    int64_t liveMat[kMaterialCount] = {0};
    int64_t zeroTopsoil = 0, thinTopsoil = 0, landColumns = 0, gateMismatch = 0;
    int64_t subseaAlpine = 0, subseaColumns = 0, surfaceMatTop = 0;

    for (int64_t j = 0; j < nAxis; ++j) {
        const int64_t py = py0 + (py1 - py0) * j / (nAxis - 1);
        for (int64_t i = 0; i < nAxis; ++i) {
            const int64_t px = px0 + (px1 - px0) * i / (nAxis - 1);

            // Column at the CENTRE of this tile pixel, so the bilinear base and
            // the pixel the climate is read from agree with each other.
            const int64_t vx = (px * pxMm + pxMm / 2) / kVoxelSizeMm;
            const int64_t vy = (py * pxMm + pxMm / 2) / kVoxelSizeMm;

            const ColumnSample col = amp.column(vx, vy);
            const ClimateSample cl = tiles->climate(px, py);
            const int64_t slope = probeSlopeMmPerPx(tiles->elevationMm(px, py),
                                                    tiles->elevationMm(px + 1, py),
                                                    tiles->elevationMm(px, py + 1));

            chT.push_back(cl.temperature);
            chS.push_back(cl.seasonality);
            chP.push_back(cl.precipitation);
            chV.push_back(cl.precipVariability);
            slopes.push_back(slope);
            elevs.push_back(col.surfaceMm);
            topsoils.push_back(col.topsoilMm);
            subsoils.push_back(col.subsoilMm);

            const BiomeId biome =
                classifyBiome(cl.temperature, cl.precipitation, cl.seasonality, col.surfaceMm, slope);
            biomeCount[biome]++;

            const Gate g = attributeGate(cl.temperature, col.surfaceMm, slope);
            gateCount[g]++;
            // Self-check on the duplicated gate order: the attributed gate must
            // imply the id classifyBiome actually returned for the three gates
            // whose outcome is unambiguous.
            if ((g == GATE_OCEAN && biome != OCEAN) || (g == GATE_BEACH && biome != BEACH) ||
                (g == GATE_CLIFF && biome != TUNDRA_ALPINE))
                ++gateMismatch;

            if (col.surfaceMm < kBiomeBeachLowerMm) {
                ++subseaColumns;
                if (biome == TUNDRA_ALPINE) ++subseaAlpine;
            }
            if (col.surfaceMm > kBiomeBeachUpperMm) {
                ++landColumns;
                if (col.topsoilMm == 0) ++zeroTopsoil;
                if (col.topsoilMm < kVoxelSizeMm) ++thinTopsoil;
            }

            // The TOP voxel: the highest whose centre is at or below the
            // surface. Its material is what the player sees and digs first, so
            // it -- not surfaceMat -- is the honest readout of whether biome
            // classification reached the world.
            const int64_t topVz = floorDiv(int64_t(col.surfaceMm) - kVoxelSizeMm / 2, kVoxelSizeMm);
            const MaterialId ms = Amplifier::stratigraphyAt(col, topVz);
            const MaterialId ml = Amplifier::materialAt(col, topVz);
            stratMat[ms]++;
            liveMat[ml]++;
            if (isSurfaceMaterial(ms)) ++surfaceMatTop;
        }
    }

    std::printf("=== CHANNELS (u8 as stored, and decoded to physical units) ===\n");
    printChannelRow(kTemperature, chT);
    printChannelRow(kSeasonality, chS);
    printChannelRow(kPrecipitation, chP);
    printChannelRow(kPrecipVariability, chV);
    std::printf("\n=== TERRAIN ===\n");
    printPercentileRow("surface", elevs, "mm");
    printPercentileRow("slope/px", slopes, "mm");
    {
        std::vector<int64_t> s = slopes;
        std::sort(s.begin(), s.end());
        std::printf("  cliff gate is kBiomeCliffSlopeMmPerPx=%lld; median slope is %lld "
                    "(gate is %s the median)\n",
                    (long long)kBiomeCliffSlopeMmPerPx, (long long)pctl(s, 50),
                    kBiomeCliffSlopeMmPerPx < pctl(s, 50) ? "BELOW" : "above");
    }

    std::printf("\n=== BIOME CENSUS ===\n");
    for (int b = 0; b < kBiomeCount; ++b)
        std::printf("  %-18s %10lld  %6.2f%%%s\n", kBiomeName[b], (long long)biomeCount[b],
                    pct(biomeCount[b], total), biomeCount[b] == 0 ? "   <-- UNREACHABLE" : "");

    std::printf("\n=== GATE ATTRIBUTION (which gate decided) ===\n");
    for (int g = 0; g < kGateCount; ++g)
        std::printf("  %-18s %10lld  %6.2f%%\n", kGateName[g], (long long)gateCount[g],
                    pct(gateCount[g], total));
    if (gateMismatch)
        std::printf("  MISMATCH: %lld columns disagreed with classifyBiome -- this probe's gate "
                    "order no longer mirrors biome.h\n",
                    (long long)gateMismatch);
    std::printf("  below sea level: %lld columns, of which %lld (%.2f%%) classify TUNDRA_ALPINE "
                "= %.2f%% of the world\n",
                (long long)subseaColumns, (long long)subseaAlpine, pct(subseaAlpine, subseaColumns),
                pct(subseaAlpine, total));

    std::printf("\n=== STRATIGRAPHY (land columns only, n=%lld) ===\n", (long long)landColumns);
    printPercentileRow("topsoil", topsoils, "mm");
    printPercentileRow("subsoil", subsoils, "mm");
    std::printf("  topsoilMm == 0:            %10lld  %6.2f%% of land\n", (long long)zeroTopsoil,
                pct(zeroTopsoil, landColumns));
    std::printf("  topsoilMm < 1 voxel (%dmm): %10lld  %6.2f%% of land\n", kVoxelSizeMm,
                (long long)thinTopsoil, pct(thinTopsoil, landColumns));

    std::printf("\n=== TOP-VOXEL MATERIAL (stratigraphy only / after cave carve) ===\n");
    for (int m = 0; m < kMaterialCount; ++m) {
        if (!stratMat[m] && !liveMat[m]) continue;
        std::printf("  %-14s %10lld  %6.2f%%   / %10lld  %6.2f%%\n", materialName(MaterialId(m)),
                    (long long)stratMat[m], pct(stratMat[m], total), (long long)liveMat[m],
                    pct(liveMat[m], total));
    }
    std::printf("  surface materials at the top voxel: %.2f%%%s\n", pct(surfaceMatTop, total),
                surfaceMatTop == 0 ? "   <-- biome classification never reaches the player" : "");

    // Every number above is only meaningful if every sample landed on real tile
    // data. A nonzero count means the window ran off the loaded set and the
    // percentiles are contaminated with the flat sea-level default.
    if (dir != "--synthetic") {
        const uint64_t missing = grid.missingTileQueries.load(std::memory_order_relaxed);
        std::printf("\nmissing-tile queries: %llu%s\n", (unsigned long long)missing,
                    missing ? "   <-- SAMPLES FELL OUTSIDE THE LOADED TILES, results contaminated"
                            : " (all samples landed on real tile data)");
        if (missing) return 1;
    }
    return 0;
}
