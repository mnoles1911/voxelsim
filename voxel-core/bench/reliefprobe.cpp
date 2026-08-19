// vxc_reliefprobe — REGION-SCALE relief statistics for the DEM comparison
// (docs/dem-reference-library.md §5).
//
// vxc_terrainprobe answers "does the amplified surface continue the coarse
// spectrum?" — its structure function tops out at a 120 m lag by design, so it
// characterises ROUGHNESS, not kilometre-scale relief. This probe answers the
// other question in the DEM plan: what does our world's DRAMA look like at the
// scales a Milford Sound or a Hunza valley is judged at? It is a sibling
// rather than a terrainprobe mode because terrainprobe promises byte-identical
// output for every pre-existing command line, and because the sampling
// geometry is different in kind: a dense region raster, not transects or
// local lattices. Everything it measures it measures on the SAME surface the
// client draws — Amplifier::surfaceMm over the production samplers — never a
// reimplementation (see the never-rebuild-ground rule; three retractions
// bought that lesson).
//
// Usage:
//   vxc_reliefprobe <tiledir> <seed> <x0M> <y0M> <widthM> [heightM] [options]
//
// Options:
//   --fine-dir DIR   load .vxtl v2 fine tiles intersecting the region and
//                    measure the fine-tier world (refuses to silently fall
//                    back if none load, same contract as terrainprobe)
//   --zstd PATH      explicit libzstd for CODEC_ZSTD fine tiles; otherwise
//                    searched on the default names (riverribbonprobe pattern)
//   --stride-m N     sample stride in metres (default 30 — see below)
//   --threads N      sampling threads (default hardware_concurrency)
//   --json PATH      also write the full result as JSON (machine-diffable,
//                    and the schema the DEM side must reproduce)
//   --region NAME    free-text label echoed into the output and JSON
//   --baseline       print ONLY the fixed-field machine-greppable table
//
// WHY A 30 m DEFAULT STRIDE. 30 m is simultaneously (a) the native posting of
// the coarse tier (512 px / 15.36 km tile) and (b) the posting of Copernicus
// GLO-30 / SRTM 1-arc-second — the DEM reference backbone. Sampling both
// sides at the same posting makes every statistic below comparable
// lag-for-lag with no scale correction (dem-reference-library.md §1, "the
// 30 m coincidence"). The amplifier's sub-30 m octaves add decimetre-to-metre
// texture that cannot move kilometre-window max−min; sampling finer would
// cost 100–256x for noise on these statistics. Fine-tier runs are still
// sampled at 30 m for the same comparability reason — the fine tier changes
// WHAT the surface is at each sample, not the lattice we ask on.
//
// DATUM CONVENTION (must match the DEM side exactly). GLO-30 is a DSM that
// reads ~0 over sea water. Our amplified surface is the GROUND, which keeps
// going under the ocean. So every statistic here is computed on
//     hc(x) = max(h(x), 0)        ["clamped" — the GLO-30 view]
// except the two keys suffixed `_bathy`, which use raw h and answer the
// continuous-wall question the DEM doc raises for fjords (Milford is 1,683 m
// from the water surface but 1,973 m from the fjord floor). land_frac is
// h > 0 on the raw surface.
//
// THE STATISTICS, defined precisely so the DEM side can implement them
// identically (that comparability IS the deliverable):
//
//  1. WINDOWED RELIEF R(w), w in {1, 2.5, 5, 10} km.
//     Window = an n x n block of samples, n = round(w/stride) + 1, so the
//     window SPAN is n-1 strides ~= w (exact span printed as relief.*.span).
//     R = max(hc) - min(hc) inside the window. The population is every
//     fully-interior window position at sample stride (dense sliding).
//     Reported: p50/p90/p99/max (nearest-rank on a 0.1 m histogram; a value
//     is its bin's midpoint) and the world-metre CENTER of the max window.
//
//  2. WALL SCORE W(L), L in {2000, 500} m — "the fjord number" (Milford
//     W2k = 1,683 m). Population: |hc(x + Lu) - hc(x)| over every grid
//     position x and the four lattice directions u = E, N, NE, SE, each
//     unordered endpoint pair counted once. This is identical to "elevation
//     gain along any straight L-metre transect over 8 compass directions",
//     because every unordered pair is one transect walked uphill. On a
//     stride-s lattice L is realised as round(L/s) samples on the axes and
//     round(L/(s*sqrt(2))) diagonal steps; the exact realised lags are
//     printed (wall.*.lag_axis / lag_diag). Reported: p50/p99/max and both
//     endpoints of the max so a person can go stand there. w2000_bathy is
//     the same on raw h.
//
//  3. SLOPE at the sampling posting: central differences,
//     slope_deg = atan(sqrt(sx^2+sy^2)) * 180/pi,
//     sx = (hc[i+1,j]-hc[i-1,j])/(2s). Reported: mean/p50/p90/p99 and the
//     bimodality coefficient BC = (g1^2 + 1) / (g2 + 3) with population
//     skewness g1 and population EXCESS kurtosis g2 (no finite-n
//     correction). BC > 5/9 ~= 0.555 suggests bimodality (fjords/canyons:
//     flat floors + near-vertical walls; rolling hills are unimodal).
//
//  4. HYPSOMETRIC INTEGRAL, elevation-relief ratio form:
//     HI = (mean(hc) - min(hc)) / (max(hc) - min(hc)) over all samples.
//
//  5. RUGGEDNESS. TRI (Riley et al. 1999): per interior cell,
//     sqrt( sum over 8 neighbours (h_n - h_c)^2 ), at the sampling posting.
//     VRM (Sappington et al. 2007), 3x3 window: per-cell unit surface
//     normal n = (-gx, -gy, 1)/|.| from the same central differences
//     (equivalent to the slope/aspect decomposition), then
//     VRM = 1 - |sum of the 9 normals| / 9.
//
// All neighbourhood statistics (slope/TRI/VRM) skip a 1-sample border.
// Percentiles everywhere are nearest-rank on fixed-width histograms (bin
// widths: elevation/relief/wall 0.1 m, slope 0.02 deg, TRI 0.01 m, VRM 1e-5);
// the reported value is the bin midpoint, so the quantisation error is
// bounded by half a bin.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "voxelcore/amplifier.h"
#include "voxelcore/core.h"
#include "voxelcore/tilestore.h"

using namespace vxc;

namespace {

// --- runtime zstd, bound the way the game binds it (riverribbonprobe) -------
using ZstdDecompressFn = size_t (*)(void*, size_t, const void*, size_t);
using ZstdIsErrorFn = unsigned (*)(size_t);
ZstdDecompressFn gZstdDecompress = nullptr;
ZstdIsErrorFn gZstdIsError = nullptr;
std::string gZstdPath;

bool zstdInflate(void*, const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    if (!src || !dst || srcLen == 0 || dstLen == 0) return false;
    if (!gZstdDecompress || !gZstdIsError) return false;
    const size_t produced = gZstdDecompress(dst, dstLen, src, srcLen);
    if (gZstdIsError(produced)) return false;
    return produced == dstLen;
}

void* openLib(const char* path) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryA(path));
#else
    return dlopen(path, RTLD_NOW);
#endif
}
void* symbol(void* h, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), name));
#else
    return dlsym(h, name);
#endif
}
bool bindZstd(const std::vector<std::string>& candidates) {
    for (const std::string& c : candidates) {
        if (c.empty()) continue;
        void* h = openLib(c.c_str());
        if (!h) continue;
        auto* d = reinterpret_cast<ZstdDecompressFn>(symbol(h, "ZSTD_decompress"));
        auto* e = reinterpret_cast<ZstdIsErrorFn>(symbol(h, "ZSTD_isError"));
        if (!d || !e) continue;
        gZstdDecompress = d;
        gZstdIsError = e;
        gZstdPath = c;
        return true;
    }
    return false;
}

// --- fixed-width histogram with nearest-rank percentiles ---------------------
// The reported percentile value is the MIDPOINT of the bin holding the
// nearest-rank sample. Values outside [lo, hi) clamp to the end bins (and a
// clamp is counted, so a saturated histogram is visible rather than silent).
struct Hist {
    double lo = 0, binW = 1;
    std::vector<int64_t> bins;
    int64_t n = 0;
    int64_t clamped = 0;

    Hist(double lo_, double hi_, double binW_) : lo(lo_), binW(binW_) {
        bins.assign(static_cast<size_t>(std::ceil((hi_ - lo_) / binW_)) + 1, 0);
    }
    void add(double v) {
        double b = (v - lo) / binW;
        int64_t i = static_cast<int64_t>(b);
        if (b < 0) {
            i = 0;
            ++clamped;
        } else if (i >= static_cast<int64_t>(bins.size())) {
            i = static_cast<int64_t>(bins.size()) - 1;
            ++clamped;
        }
        ++bins[static_cast<size_t>(i)];
        ++n;
    }
    void merge(const Hist& o) {
        for (size_t i = 0; i < bins.size(); ++i) bins[i] += o.bins[i];
        n += o.n;
        clamped += o.clamped;
    }
    // Nearest-rank: smallest value v such that at least ceil(p/100 * n)
    // samples are <= v; reported as that sample's bin midpoint.
    double pct(double p) const {
        if (n == 0) return 0;
        const int64_t rank = static_cast<int64_t>(std::ceil(p / 100.0 * static_cast<double>(n)));
        int64_t cum = 0;
        for (size_t i = 0; i < bins.size(); ++i) {
            cum += bins[i];
            if (cum >= rank) return lo + (static_cast<double>(i) + 0.5) * binW;
        }
        return lo + (static_cast<double>(bins.size()) - 0.5) * binW;
    }
};

// --- van Herk / Gil-Werman sliding max/min, O(1) per element ----------------
template <typename Cmp>
void slide1D(const float* src, int64_t nSrc, int64_t k, float* dst, Cmp better) {
    // dst[i] = extremum of src[i .. i+k-1], for i in [0, nSrc-k].
    const int64_t nOut = nSrc - k + 1;
    if (nOut <= 0) return;
    std::vector<float> pre(static_cast<size_t>(nSrc)), suf(static_cast<size_t>(nSrc));
    for (int64_t b = 0; b < nSrc; b += k) {
        const int64_t e = std::min(b + k, nSrc);
        pre[static_cast<size_t>(b)] = src[b];
        for (int64_t i = b + 1; i < e; ++i)
            pre[static_cast<size_t>(i)] =
                better(src[i], pre[static_cast<size_t>(i - 1)]) ? src[i]
                                                                : pre[static_cast<size_t>(i - 1)];
        suf[static_cast<size_t>(e - 1)] = src[e - 1];
        for (int64_t i = e - 2; i >= b; --i)
            suf[static_cast<size_t>(i)] =
                better(src[i], suf[static_cast<size_t>(i + 1)]) ? src[i]
                                                                : suf[static_cast<size_t>(i + 1)];
    }
    for (int64_t i = 0; i < nOut; ++i) {
        const float a = suf[static_cast<size_t>(i)];
        const float b = pre[static_cast<size_t>(i + k - 1)];
        dst[i] = better(a, b) ? a : b;
    }
}

void row(const char* key, double v, const char* unit) {
    std::printf("%-46s %16.6g  %s\n", key, v, unit);
}
void rowI(const char* key, long long v, const char* unit) {
    std::printf("%-46s %16lld  %s\n", key, v, unit);
}
void rowS(const char* key, const char* v, const char* unit) {
    std::printf("%-46s %16s  %s\n", key, v, unit);
}

struct WallDir {
    int64_t di, dj;   // sample offsets
    const char* name; // compass-ish label on the +x=east, +y=north convention
};

struct WallResult {
    double lagAxisM = 0, lagDiagM = 0;
    Hist hist{0, 12000, 0.1};
    double maxV = -1;
    int64_t maxI0 = 0, maxJ0 = 0, maxI1 = 0, maxJ1 = 0;
    const char* maxDir = "";
};

struct ReliefResult {
    double spanM = 0;
    Hist hist{0, 12000, 0.1};
    int64_t count = 0;
    double maxV = -1;
    double maxCxM = 0, maxCyM = 0;
};

std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if (static_cast<unsigned char>(c) >= 0x20) {
            o += c;
        }
    }
    return o;
}

} // namespace

int main(int argc, char** argv) {
    std::string fineDir, zstdPath, jsonPath, regionName;
    int64_t strideM = 30;
    int threads = static_cast<int>(std::thread::hardware_concurrency());
    if (threads < 1) threads = 1;
    bool optBaseline = false;

    std::vector<char*> pos;
    pos.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        char* a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(a, "--fine-dir"))
            fineDir = need(a);
        else if (!std::strcmp(a, "--zstd"))
            zstdPath = need(a);
        else if (!std::strcmp(a, "--json"))
            jsonPath = need(a);
        else if (!std::strcmp(a, "--region"))
            regionName = need(a);
        else if (!std::strcmp(a, "--stride-m")) {
            strideM = std::strtoll(need(a), nullptr, 10);
            if (strideM < 1 || strideM > 10000) {
                std::fprintf(stderr, "--stride-m %lld out of range (1..10000)\n",
                             (long long)strideM);
                return 2;
            }
        } else if (!std::strcmp(a, "--threads")) {
            threads = static_cast<int>(std::strtol(need(a), nullptr, 10));
            if (threads < 1 || threads > 256) {
                std::fprintf(stderr, "--threads out of range (1..256)\n");
                return 2;
            }
        } else if (!std::strcmp(a, "--baseline")) {
            optBaseline = true;
        } else {
            pos.push_back(a);
        }
    }
    argc = static_cast<int>(pos.size());
    argv = pos.data();

    if (argc < 6) {
        std::fprintf(stderr,
                     "usage: vxc_reliefprobe <tiledir> <seed> <x0M> <y0M> <widthM> [heightM]\n"
                     "       [--fine-dir DIR] [--zstd PATH] [--stride-m N] [--threads N]\n"
                     "       [--json PATH] [--region NAME] [--baseline]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const uint64_t seed = std::strtoull(argv[2], nullptr, 10);
    const int64_t x0M = std::strtoll(argv[3], nullptr, 10);
    const int64_t y0M = std::strtoll(argv[4], nullptr, 10);
    const int64_t widthM = std::strtoll(argv[5], nullptr, 10);
    const int64_t heightM = argc > 6 ? std::strtoll(argv[6], nullptr, 10) : widthM;
    if (widthM < strideM * 4 || heightM < strideM * 4) {
        std::fprintf(stderr, "region smaller than 4 strides per axis is not a region\n");
        return 2;
    }

    // --- coarse tier -------------------------------------------------------
    TileGridSampler grid(seed, 1);
    {
        int loaded = 0, rejected = 0;
        if (!std::filesystem::exists(dir)) {
            std::fprintf(stderr, "no such directory: %s\n", dir.c_str());
            return 1;
        }
        for (auto& e : std::filesystem::directory_iterator(dir)) {
            if (e.path().extension() != ".vxtl") continue;
            if (grid.loadTileFile(e.path()))
                ++loaded;
            else
                ++rejected;
        }
        if (!optBaseline)
            std::printf("coarse tiles loaded=%d rejected=%d pixelSizeMm=%d\n", loaded, rejected,
                        grid.pixelSizeMm());
        if (loaded == 0) {
            std::fprintf(stderr, "no coarse tiles loaded from %s\n", dir.c_str());
            return 1;
        }
    }
    ITileSampler* tiles = &grid;

    // --- fine tier (optional; same refuse-to-fall-back contract as
    // terrainprobe). Only tiles whose 15.36 km footprint intersects the region
    // are loaded: fine tiles are hundreds of MB each and a region run only
    // ever reads the ones under it.
    FineTileSampler fine(seed, &grid);
    int fineLoaded = 0;
    if (!fineDir.empty()) {
        std::vector<std::string> cands;
        if (!zstdPath.empty()) cands.push_back(zstdPath);
#if defined(_WIN32)
        cands.push_back("libzstd.dll");
        cands.push_back("zstd.dll");
#else
        cands.push_back("libzstd.so.1");
        cands.push_back("libzstd.so");
#endif
        const bool bound = bindZstd(cands);
        if (!optBaseline)
            std::printf("zstd: %s%s\n", bound ? "bound from " : "NOT BOUND",
                        bound ? gZstdPath.c_str() : " -- every CODEC_ZSTD tile will be refused");
        FineDecompressor dec;
        dec.fn = &zstdInflate;
        dec.user = nullptr;
        fine.setDecompressor(dec);

        const int64_t tileM = static_cast<int64_t>(TileData::kTileSize) *
                              tilePixelSizeMm(1) / 1000; // 15360
        const int64_t tx0 = floorDiv(x0M, tileM), tx1 = floorDiv(x0M + widthM - 1, tileM);
        const int64_t ty0 = floorDiv(y0M, tileM), ty1 = floorDiv(y0M + heightM - 1, tileM);
        int rejected = 0, skipped = 0;
        if (!std::filesystem::exists(fineDir)) {
            std::fprintf(stderr, "no such directory: %s\n", fineDir.c_str());
            return 1;
        }
        for (auto& e : std::filesystem::directory_iterator(fineDir)) {
            if (e.path().extension() != ".vxtl") continue;
            // Cache naming is "<tx>_<ty>.vxtl"; a stem that does not parse is
            // loaded anyway (the sampler validates the header) rather than
            // silently skipped on a naming assumption.
            const std::string stem = e.path().stem().string();
            long long ftx = 0, fty = 0;
            const bool parsed = std::sscanf(stem.c_str(), "%lld_%lld", &ftx, &fty) == 2;
            if (parsed && (ftx < tx0 || ftx > tx1 || fty < ty0 || fty > ty1)) {
                ++skipped;
                continue;
            }
            if (fine.loadTileFile(e.path()))
                ++fineLoaded;
            else
                ++rejected;
        }
        if (!optBaseline)
            std::printf("fine tier: dir=%s loaded=%d rejected=%d skipped_outside_region=%d "
                        "pixelSizeMm=%d\n",
                        fineDir.c_str(), fineLoaded, rejected, skipped, fine.pixelSizeMm());
        if (fineLoaded == 0) {
            std::fprintf(stderr, "no v2 fine tiles under the region in %s; refusing to "
                                 "silently fall back to the coarse tier\n",
                         fineDir.c_str());
            return 1;
        }
        tiles = &fine;

        // FineTileSampler decodes blocks lazily and "a cold query is a write"
        // (tilestore.cpp) — the sampling loop below is multithreaded, so every
        // block the region can touch must be resident BEFORE the threads
        // start. prewarm() exists precisely for this. The margin covers the
        // carrier's 4x4 control stencil reach (2 fine px) with room to spare.
        const int64_t fineMm = tilePixelSizeMm(kFineTileScale);
        const int64_t margin = 8;
        const int64_t px0 = floorDiv(x0M * 1000, fineMm) - margin;
        const int64_t py0 = floorDiv(y0M * 1000, fineMm) - margin;
        const int64_t px1 = floorDiv((x0M + widthM) * 1000, fineMm) + margin;
        const int64_t py1 = floorDiv((y0M + heightM) * 1000, fineMm) + margin;
        const bool warm = fine.prewarm(px0, py0, px1, py1);
        if (!optBaseline)
            std::printf("fine prewarm: [%lld..%lld]x[%lld..%lld] px %s\n", (long long)px0,
                        (long long)px1, (long long)py0, (long long)py1,
                        warm ? "complete" : "INCOMPLETE (some pixels fall outside loaded "
                                            "tiles; those read as open ocean)");
    }

    Amplifier amp(seed, *tiles);

    // --- sample the amplified surface --------------------------------------
    const int64_t nx = widthM / strideM;
    const int64_t ny = heightM / strideM;
    const int64_t nCells = nx * ny;
    const int64_t voxPerStride = strideM * 1000 / kVoxelSizeMm;
    if (!optBaseline)
        std::printf("sampling %lld x %lld = %lld columns at %lld m stride, %d threads...\n",
                    (long long)nx, (long long)ny, (long long)nCells, (long long)strideM, threads);

    // Raw surface in metres (float: 0.1 mm precision at alpine elevations,
    // far below the 100 mm voxel). hc = clamped view is derived on the fly.
    std::vector<float> h(static_cast<size_t>(nCells));
    {
        std::atomic<int64_t> nextRow{0};
        auto worker = [&]() {
            for (;;) {
                const int64_t j = nextRow.fetch_add(1, std::memory_order_relaxed);
                if (j >= ny) return;
                const int64_t vy = (y0M + j * strideM) * 1000 / kVoxelSizeMm;
                float* out = h.data() + j * nx;
                int64_t vx = x0M * 1000 / kVoxelSizeMm;
                for (int64_t i = 0; i < nx; ++i, vx += voxPerStride)
                    out[i] = static_cast<float>(amp.surfaceMm(vx, vy)) * 0.001f;
            }
        };
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
    }

    auto hc = [&](int64_t idx) {
        const float v = h[static_cast<size_t>(idx)];
        return v > 0.0f ? v : 0.0f;
    };

    // --- elevation distribution, hypsometry, land fraction ------------------
    Hist elevHist(-12000, 12000, 0.1);
    double minRaw = 1e30, minC = 1e30, maxC = -1e30;
    long double sumC = 0;
    int64_t landCells = 0;
    for (int64_t idx = 0; idx < nCells; ++idx) {
        const double raw = h[static_cast<size_t>(idx)];
        const double c = raw > 0 ? raw : 0;
        elevHist.add(c);
        if (raw < minRaw) minRaw = raw;
        if (c < minC) minC = c;
        if (c > maxC) maxC = c;
        sumC += c;
        if (raw > 0) ++landCells;
    }
    const double meanC = static_cast<double>(sumC / nCells);
    const double hypso = (maxC - minC) > 0 ? (meanC - minC) / (maxC - minC) : 0;

    // --- windowed relief ----------------------------------------------------
    const double windowsKm[4] = {1000, 2500, 5000, 10000};
    ReliefResult relief[4];
    {
        std::vector<float> rowMax, rowMin, colBufMax, colBufMin, outMax, outMin;
        for (int wi = 0; wi < 4; ++wi) {
            const int64_t k = static_cast<int64_t>(std::llround(windowsKm[wi] / strideM)) + 1;
            ReliefResult& R = relief[wi];
            R.spanM = static_cast<double>((k - 1) * strideM);
            if (k > nx || k > ny) continue; // window does not fit; row stays count=0
            const int64_t ox = nx - k + 1, oy = ny - k + 1;
            rowMax.assign(static_cast<size_t>(ox * ny), 0);
            rowMin.assign(static_cast<size_t>(ox * ny), 0);
            std::vector<float> rowClamped(static_cast<size_t>(nx));
            for (int64_t j = 0; j < ny; ++j) {
                for (int64_t i = 0; i < nx; ++i) rowClamped[static_cast<size_t>(i)] = hc(j * nx + i);
                slide1D(rowClamped.data(), nx, k, rowMax.data() + j * ox,
                        [](float a, float b) { return a > b; });
                slide1D(rowClamped.data(), nx, k, rowMin.data() + j * ox,
                        [](float a, float b) { return a < b; });
            }
            colBufMax.resize(static_cast<size_t>(ny));
            colBufMin.resize(static_cast<size_t>(ny));
            outMax.resize(static_cast<size_t>(oy));
            outMin.resize(static_cast<size_t>(oy));
            for (int64_t i = 0; i < ox; ++i) {
                for (int64_t j = 0; j < ny; ++j) {
                    colBufMax[static_cast<size_t>(j)] = rowMax[static_cast<size_t>(j * ox + i)];
                    colBufMin[static_cast<size_t>(j)] = rowMin[static_cast<size_t>(j * ox + i)];
                }
                slide1D(colBufMax.data(), ny, k, outMax.data(),
                        [](float a, float b) { return a > b; });
                slide1D(colBufMin.data(), ny, k, outMin.data(),
                        [](float a, float b) { return a < b; });
                for (int64_t j = 0; j < oy; ++j) {
                    const double r = static_cast<double>(outMax[static_cast<size_t>(j)]) -
                                     static_cast<double>(outMin[static_cast<size_t>(j)]);
                    R.hist.add(r);
                    ++R.count;
                    if (r > R.maxV) {
                        R.maxV = r;
                        R.maxCxM = static_cast<double>(x0M) +
                                   (static_cast<double>(i) + static_cast<double>(k - 1) / 2.0) *
                                       static_cast<double>(strideM);
                        R.maxCyM = static_cast<double>(y0M) +
                                   (static_cast<double>(j) + static_cast<double>(k - 1) / 2.0) *
                                       static_cast<double>(strideM);
                    }
                }
            }
        }
    }

    // --- wall scores --------------------------------------------------------
    const double wallLagsM[2] = {2000, 500};
    WallResult wall[2];   // clamped
    WallResult wallBathy; // raw h, 2 km only
    for (int li = 0; li < 2; ++li) {
        const double L = wallLagsM[li];
        const int64_t a = std::llround(L / static_cast<double>(strideM));
        const int64_t d = std::llround(L / (static_cast<double>(strideM) * std::sqrt(2.0)));
        WallResult& W = wall[li];
        W.lagAxisM = static_cast<double>(a * strideM);
        W.lagDiagM = static_cast<double>(d * strideM) * std::sqrt(2.0);
        if (li == 0) {
            wallBathy.lagAxisM = W.lagAxisM;
            wallBathy.lagDiagM = W.lagDiagM;
        }
        const WallDir dirs[4] = {{a, 0, "E-W"}, {0, a, "N-S"}, {d, d, "NE-SW"}, {d, -d, "SE-NW"}};
        for (const WallDir& D : dirs) {
            if (std::abs(D.di) >= nx || std::abs(D.dj) >= ny) continue;
            const int64_t j0 = D.dj < 0 ? -D.dj : 0;
            const int64_t j1 = D.dj < 0 ? ny : ny - D.dj;
            for (int64_t j = j0; j < j1; ++j) {
                const int64_t base = j * nx, base2 = (j + D.dj) * nx + D.di;
                for (int64_t i = 0; i + D.di < nx; ++i) {
                    {
                        const double diff =
                            std::abs(static_cast<double>(hc(base2 + i)) - hc(base + i));
                        W.hist.add(diff);
                        if (diff > W.maxV) {
                            W.maxV = diff;
                            const bool up = hc(base2 + i) >= hc(base + i);
                            W.maxI0 = up ? i : i + D.di;
                            W.maxJ0 = up ? j : j + D.dj;
                            W.maxI1 = up ? i + D.di : i;
                            W.maxJ1 = up ? j + D.dj : j;
                            W.maxDir = D.name;
                        }
                    }
                    if (li == 0) {
                        const double diff = std::abs(
                            static_cast<double>(h[static_cast<size_t>(base2 + i)]) -
                            static_cast<double>(h[static_cast<size_t>(base + i)]));
                        wallBathy.hist.add(diff);
                        if (diff > wallBathy.maxV) {
                            wallBathy.maxV = diff;
                            const bool up = h[static_cast<size_t>(base2 + i)] >=
                                            h[static_cast<size_t>(base + i)];
                            wallBathy.maxI0 = up ? i : i + D.di;
                            wallBathy.maxJ0 = up ? j : j + D.dj;
                            wallBathy.maxI1 = up ? i + D.di : i;
                            wallBathy.maxJ1 = up ? j + D.dj : j;
                            wallBathy.maxDir = D.name;
                        }
                    }
                }
            }
        }
    }

    // --- slope, TRI, VRM ----------------------------------------------------
    Hist slopeHist(0, 90, 0.02);
    Hist triHist(0, 2000, 0.01);
    Hist vrmHist(0, 1, 1e-5);
    long double sN = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0; // slope raw moments
    long double triSum = 0, vrmSum = 0;
    {
        // Ring buffer of three rows of unit normals for the VRM 3x3 window.
        const double s2x = 2.0 * static_cast<double>(strideM);
        std::vector<float> nxr[3], nyr[3], nzr[3];
        for (int r = 0; r < 3; ++r) {
            nxr[r].assign(static_cast<size_t>(nx), 0);
            nyr[r].assign(static_cast<size_t>(nx), 0);
            nzr[r].assign(static_cast<size_t>(nx), 1);
        }
        auto fillNormals = [&](int64_t j, int r) {
            for (int64_t i = 1; i + 1 < nx; ++i) {
                const double gx = (hc(j * nx + i + 1) - hc(j * nx + i - 1)) / s2x;
                const double gy = (hc((j + 1) * nx + i) - hc((j - 1) * nx + i)) / s2x;
                const double inv = 1.0 / std::sqrt(gx * gx + gy * gy + 1.0);
                nxr[r][static_cast<size_t>(i)] = static_cast<float>(-gx * inv);
                nyr[r][static_cast<size_t>(i)] = static_cast<float>(-gy * inv);
                nzr[r][static_cast<size_t>(i)] = static_cast<float>(inv);
            }
        };
        if (ny >= 3) {
            fillNormals(1, 1);
            for (int64_t j = 1; j + 1 < ny; ++j) {
                if (j + 2 < ny) fillNormals(j + 1, static_cast<int>((j + 1) % 3));
                for (int64_t i = 1; i + 1 < nx; ++i) {
                    const double c = hc(j * nx + i);
                    const double gx = (hc(j * nx + i + 1) - hc(j * nx + i - 1)) / s2x;
                    const double gy = (hc((j + 1) * nx + i) - hc((j - 1) * nx + i)) / s2x;
                    const double slopeDeg =
                        std::atan(std::sqrt(gx * gx + gy * gy)) * 180.0 / 3.14159265358979323846;
                    slopeHist.add(slopeDeg);
                    sN += 1;
                    s1 += slopeDeg;
                    s2 += slopeDeg * slopeDeg;
                    s3 += slopeDeg * slopeDeg * slopeDeg;
                    s4 += slopeDeg * slopeDeg * slopeDeg * slopeDeg;
                    double sq = 0;
                    for (int64_t dj = -1; dj <= 1; ++dj)
                        for (int64_t di = -1; di <= 1; ++di) {
                            if (di == 0 && dj == 0) continue;
                            const double dh = hc((j + dj) * nx + i + di) - c;
                            sq += dh * dh;
                        }
                    const double tri = std::sqrt(sq);
                    triHist.add(tri);
                    triSum += tri;
                    // VRM needs normals of the full 3x3, which exist only for
                    // interior-of-interior cells.
                    if (i >= 2 && i + 2 < nx && j >= 2 && j + 2 < ny) {
                        double sx = 0, sy = 0, sz = 0;
                        for (int64_t dj = -1; dj <= 1; ++dj) {
                            const int r = static_cast<int>((j + dj) % 3);
                            for (int64_t di = -1; di <= 1; ++di) {
                                sx += nxr[r][static_cast<size_t>(i + di)];
                                sy += nyr[r][static_cast<size_t>(i + di)];
                                sz += nzr[r][static_cast<size_t>(i + di)];
                            }
                        }
                        const double vrm = 1.0 - std::sqrt(sx * sx + sy * sy + sz * sz) / 9.0;
                        vrmHist.add(vrm);
                        vrmSum += vrm;
                    }
                }
            }
        }
    }
    const double slopeMean = sN > 0 ? static_cast<double>(s1 / sN) : 0;
    double slopeBC = 0;
    if (sN > 3) {
        const long double m1 = s1 / sN;
        const long double m2 = s2 / sN - m1 * m1;
        const long double m3 = s3 / sN - 3 * m1 * (s2 / sN) + 2 * m1 * m1 * m1;
        const long double m4 = s4 / sN - 4 * m1 * (s3 / sN) + 6 * m1 * m1 * (s2 / sN) -
                               3 * m1 * m1 * m1 * m1;
        if (m2 > 0) {
            const long double g1 = m3 / std::pow(static_cast<double>(m2), 1.5);
            const long double g2 = m4 / (m2 * m2) - 3.0;
            slopeBC = static_cast<double>((g1 * g1 + 1.0) / (g2 + 3.0));
        }
    }

    const long long coarseMisses = grid.missingTileQueries.load(std::memory_order_relaxed);
    const long long fineMisses =
        fineLoaded > 0 ? fine.missingTileQueries.load(std::memory_order_relaxed) : 0;

    // --- human summary -------------------------------------------------------
    auto sampleM = [&](int64_t i, int64_t j, int64_t* xo, int64_t* yo) {
        *xo = x0M + i * strideM;
        *yo = y0M + j * strideM;
    };
    if (!optBaseline) {
        std::printf("\nregion%s%s: (%lld, %lld) m, %lld x %lld m, stride %lld m, tier %s\n",
                    regionName.empty() ? "" : " ", regionName.c_str(), (long long)x0M,
                    (long long)y0M, (long long)widthM, (long long)heightM, (long long)strideM,
                    fineLoaded > 0 ? "FINE (v2, 1.875 m/px)" : "coarse (30 m/px)");
        std::printf("elevation (clamped to sea level): min %.1f  mean %.1f  max %.1f m   "
                    "(raw seabed min %.1f m)   land %.1f%%   HI %.3f\n",
                    minC, meanC, maxC, minRaw, 100.0 * static_cast<double>(landCells) / nCells,
                    hypso);
        std::printf("\nWINDOWED RELIEF R(w) = max-min inside a sliding square window (clamped)\n");
        std::printf("  %8s %10s %10s %10s %10s %10s  %s\n", "w", "span (m)", "p50 (m)", "p90 (m)",
                    "p99 (m)", "max (m)", "max window centre");
        for (int wi = 0; wi < 4; ++wi) {
            const ReliefResult& R = relief[wi];
            if (R.count == 0) {
                std::printf("  %7.1fk %10.0f %43s\n", windowsKm[wi] / 1000.0, R.spanM,
                            "window does not fit in region");
                continue;
            }
            std::printf("  %7.1fk %10.0f %10.1f %10.1f %10.1f %10.1f  (%.0f, %.0f) m\n",
                        windowsKm[wi] / 1000.0, R.spanM, R.hist.pct(50), R.hist.pct(90),
                        R.hist.pct(99), R.maxV, R.maxCxM, R.maxCyM);
        }
        std::printf("\nWALL SCORE W(L) = |elevation gain| along straight L m transects, "
                    "4 lattice directions\n");
        const char* wallName[2] = {"W2k ", "W500"};
        for (int li = 0; li < 2; ++li) {
            const WallResult& W = wall[li];
            if (W.hist.n == 0) continue;
            int64_t xa, ya, xb, yb;
            sampleM(W.maxI0, W.maxJ0, &xa, &ya);
            sampleM(W.maxI1, W.maxJ1, &xb, &yb);
            std::printf("  %s lag axis %.0f m / diag %.1f m: p50 %.1f  p99 %.1f  MAX %.1f m\n",
                        wallName[li], W.lagAxisM, W.lagDiagM, W.hist.pct(50), W.hist.pct(99),
                        W.maxV);
            std::printf("        max climbs (%lld, %lld) -> (%lld, %lld) m  [%s]  "
                        "%.1f -> %.1f m\n",
                        (long long)xa, (long long)ya, (long long)xb, (long long)yb, W.maxDir,
                        hc(W.maxJ0 * nx + W.maxI0), hc(W.maxJ1 * nx + W.maxI1));
        }
        if (wallBathy.hist.n > 0) {
            int64_t xa, ya, xb, yb;
            sampleM(wallBathy.maxI0, wallBathy.maxJ0, &xa, &ya);
            sampleM(wallBathy.maxI1, wallBathy.maxJ1, &xb, &yb);
            std::printf("  W2k with bathymetry (raw ground, no sea-level clamp): p99 %.1f  "
                        "MAX %.1f m  (%lld, %lld) -> (%lld, %lld) m\n",
                        wallBathy.hist.pct(99), wallBathy.maxV, (long long)xa, (long long)ya,
                        (long long)xb, (long long)yb);
        }
        std::printf("\nSLOPE at %lld m posting: mean %.2f  p50 %.2f  p90 %.2f  p99 %.2f deg   "
                    "bimodality BC %.3f (%s; >0.555 suggests bimodal)\n",
                    (long long)strideM, slopeMean, slopeHist.pct(50), slopeHist.pct(90),
                    slopeHist.pct(99), slopeBC, slopeBC > 5.0 / 9.0 ? "BIMODAL" : "unimodal");
        std::printf("RUGGEDNESS: TRI mean %.2f p90 %.2f p99 %.2f m   VRM mean %.5f p90 %.5f "
                    "p99 %.5f (3x3)\n",
                    static_cast<double>(triSum / (sN > 0 ? sN : 1)), triHist.pct(90),
                    triHist.pct(99),
                    vrmHist.n ? static_cast<double>(vrmSum / vrmHist.n) : 0, vrmHist.pct(90),
                    vrmHist.pct(99));
        if (coarseMisses > 0 || fineMisses > 0)
            std::printf("\nWARNING: sampler fell back to open ocean %lld coarse / %lld fine "
                        "times — part of this region is OUTSIDE the loaded tiles.\n",
                        coarseMisses, fineMisses);
        std::printf("\n");
    }

    // --- baseline table ------------------------------------------------------
    std::printf("=== VXC_RELIEFPROBE BASELINE v1 ===\n");
    std::printf("%-46s %16s  %s\n", "# field", "value", "unit");
    rowI("config.worldgen_version", kWorldGenVersion, "int");
    rowI("config.seed", static_cast<long long>(seed), "int");
    rowS("config.region", regionName.empty() ? "-" : regionName.c_str(), "label");
    rowI("config.x0", x0M, "m");
    rowI("config.y0", y0M, "m");
    rowI("config.width", widthM, "m");
    rowI("config.height", heightM, "m");
    rowI("config.stride", strideM, "m");
    rowS("config.tier", fineLoaded > 0 ? "fine1875mm" : "coarse30m", "enum");
    rowI("config.samples_x", nx, "count");
    rowI("config.samples_y", ny, "count");
    rowI("config.fine_tiles", fineLoaded, "count");
    rowI("config.coarse_misses", coarseMisses, "count_must_be_0");
    rowI("config.fine_misses", fineMisses, "count");
    row("elev.min_bathy", minRaw, "m");
    row("elev.min", minC, "m");
    row("elev.p10", elevHist.pct(10), "m");
    row("elev.p50", elevHist.pct(50), "m");
    row("elev.p90", elevHist.pct(90), "m");
    row("elev.max", maxC, "m");
    row("elev.mean", meanC, "m");
    row("elev.hypsometric_integral", hypso, "ratio");
    row("elev.land_frac", static_cast<double>(landCells) / nCells, "ratio");
    for (int wi = 0; wi < 4; ++wi) {
        const ReliefResult& R = relief[wi];
        char k[96];
        auto K = [&](const char* suffix) {
            std::snprintf(k, sizeof(k), "relief.w%.0f.%s", windowsKm[wi], suffix);
            return k;
        };
        row(K("span"), R.spanM, "m");
        rowI(K("windows"), R.count, "count");
        if (R.count == 0) continue;
        row(K("p50"), R.hist.pct(50), "m");
        row(K("p90"), R.hist.pct(90), "m");
        row(K("p99"), R.hist.pct(99), "m");
        row(K("max"), R.maxV, "m");
        row(K("max_cx"), R.maxCxM, "m");
        row(K("max_cy"), R.maxCyM, "m");
    }
    const char* wallKey[2] = {"wall.w2000", "wall.w500"};
    for (int li = 0; li < 2; ++li) {
        const WallResult& W = wall[li];
        char k[96];
        auto K = [&](const char* suffix) {
            std::snprintf(k, sizeof(k), "%s.%s", wallKey[li], suffix);
            return k;
        };
        row(K("lag_axis"), W.lagAxisM, "m");
        row(K("lag_diag"), W.lagDiagM, "m");
        if (W.hist.n == 0) continue;
        row(K("p50"), W.hist.pct(50), "m");
        row(K("p99"), W.hist.pct(99), "m");
        row(K("max"), W.maxV, "m");
        int64_t xa, ya, xb, yb;
        sampleM(W.maxI0, W.maxJ0, &xa, &ya);
        sampleM(W.maxI1, W.maxJ1, &xb, &yb);
        rowI(K("max_x0"), xa, "m");
        rowI(K("max_y0"), ya, "m");
        rowI(K("max_x1"), xb, "m");
        rowI(K("max_y1"), yb, "m");
        row(K("max_lo"), hc(W.maxJ0 * nx + W.maxI0), "m");
        row(K("max_hi"), hc(W.maxJ1 * nx + W.maxI1), "m");
    }
    if (wallBathy.hist.n > 0) {
        row("wall.w2000_bathy.p99", wallBathy.hist.pct(99), "m");
        row("wall.w2000_bathy.max", wallBathy.maxV, "m");
        int64_t xa, ya, xb, yb;
        sampleM(wallBathy.maxI0, wallBathy.maxJ0, &xa, &ya);
        sampleM(wallBathy.maxI1, wallBathy.maxJ1, &xb, &yb);
        rowI("wall.w2000_bathy.max_x0", xa, "m");
        rowI("wall.w2000_bathy.max_y0", ya, "m");
        rowI("wall.w2000_bathy.max_x1", xb, "m");
        rowI("wall.w2000_bathy.max_y1", yb, "m");
    }
    row("slope.mean", slopeMean, "deg");
    row("slope.p50", slopeHist.pct(50), "deg");
    row("slope.p90", slopeHist.pct(90), "deg");
    row("slope.p99", slopeHist.pct(99), "deg");
    row("slope.bimodality_bc", slopeBC, "ratio_gt_0.555_bimodal");
    row("tri.mean", static_cast<double>(triSum / (sN > 0 ? sN : 1)), "m");
    row("tri.p50", triHist.pct(50), "m");
    row("tri.p90", triHist.pct(90), "m");
    row("tri.p99", triHist.pct(99), "m");
    row("vrm.mean", vrmHist.n ? static_cast<double>(vrmSum / vrmHist.n) : 0, "ratio");
    row("vrm.p50", vrmHist.pct(50), "ratio");
    row("vrm.p90", vrmHist.pct(90), "ratio");
    row("vrm.p99", vrmHist.pct(99), "ratio");

    // --- JSON ----------------------------------------------------------------
    if (!jsonPath.empty()) {
        FILE* f = std::fopen(jsonPath.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "cannot write %s\n", jsonPath.c_str());
            return 1;
        }
        std::fprintf(f, "{\n");
        std::fprintf(f,
                     "  \"config\": {\"worldgen_version\": %d, \"seed\": %llu, \"region\": "
                     "\"%s\", \"x0_m\": %lld, \"y0_m\": %lld, \"width_m\": %lld, \"height_m\": "
                     "%lld, \"stride_m\": %lld, \"tier\": \"%s\", \"samples_x\": %lld, "
                     "\"samples_y\": %lld, \"fine_tiles\": %d, \"coarse_misses\": %lld, "
                     "\"fine_misses\": %lld, \"datum\": \"max(h,0) except *_bathy\"},\n",
                     kWorldGenVersion, (unsigned long long)seed, jsonEsc(regionName).c_str(),
                     (long long)x0M, (long long)y0M, (long long)widthM, (long long)heightM,
                     (long long)strideM, fineLoaded > 0 ? "fine1875mm" : "coarse30m",
                     (long long)nx, (long long)ny, fineLoaded, coarseMisses, fineMisses);
        std::fprintf(f,
                     "  \"elev\": {\"min_bathy_m\": %.2f, \"min_m\": %.2f, \"p10_m\": %.2f, "
                     "\"p50_m\": %.2f, \"p90_m\": %.2f, \"max_m\": %.2f, \"mean_m\": %.2f, "
                     "\"hypsometric_integral\": %.4f, \"land_frac\": %.4f},\n",
                     minRaw, minC, elevHist.pct(10), elevHist.pct(50), elevHist.pct(90), maxC,
                     meanC, hypso, static_cast<double>(landCells) / nCells);
        std::fprintf(f, "  \"relief\": {\n");
        for (int wi = 0; wi < 4; ++wi) {
            const ReliefResult& R = relief[wi];
            std::fprintf(f, "    \"w%.0f\": ", windowsKm[wi]);
            if (R.count == 0)
                std::fprintf(f, "{\"span_m\": %.0f, \"windows\": 0}", R.spanM);
            else
                std::fprintf(f,
                             "{\"span_m\": %.0f, \"windows\": %lld, \"p50_m\": %.2f, \"p90_m\": "
                             "%.2f, \"p99_m\": %.2f, \"max_m\": %.2f, \"max_center_xy_m\": "
                             "[%.0f, %.0f]}",
                             R.spanM, (long long)R.count, R.hist.pct(50), R.hist.pct(90),
                             R.hist.pct(99), R.maxV, R.maxCxM, R.maxCyM);
            std::fprintf(f, "%s\n", wi < 3 ? "," : "");
        }
        std::fprintf(f, "  },\n  \"wall\": {\n");
        for (int li = 0; li < 2; ++li) {
            const WallResult& W = wall[li];
            int64_t xa, ya, xb, yb;
            sampleM(W.maxI0, W.maxJ0, &xa, &ya);
            sampleM(W.maxI1, W.maxJ1, &xb, &yb);
            std::fprintf(f,
                         "    \"w%.0f\": {\"lag_axis_m\": %.1f, \"lag_diag_m\": %.1f, \"p50_m\": "
                         "%.2f, \"p99_m\": %.2f, \"max_m\": %.2f, \"max_from_xy_m\": [%lld, "
                         "%lld], \"max_to_xy_m\": [%lld, %lld], \"max_lo_m\": %.2f, "
                         "\"max_hi_m\": %.2f, \"max_dir\": \"%s\"},\n",
                         wallLagsM[li], W.lagAxisM, W.lagDiagM, W.hist.pct(50), W.hist.pct(99),
                         W.maxV, (long long)xa, (long long)ya, (long long)xb, (long long)yb,
                         hc(W.maxJ0 * nx + W.maxI0), hc(W.maxJ1 * nx + W.maxI1), W.maxDir);
        }
        {
            int64_t xa, ya, xb, yb;
            sampleM(wallBathy.maxI0, wallBathy.maxJ0, &xa, &ya);
            sampleM(wallBathy.maxI1, wallBathy.maxJ1, &xb, &yb);
            std::fprintf(f,
                         "    \"w2000_bathy\": {\"p99_m\": %.2f, \"max_m\": %.2f, "
                         "\"max_from_xy_m\": [%lld, %lld], \"max_to_xy_m\": [%lld, %lld]}\n",
                         wallBathy.hist.n ? wallBathy.hist.pct(99) : 0, wallBathy.maxV,
                         (long long)xa, (long long)ya, (long long)xb, (long long)yb);
        }
        std::fprintf(f, "  },\n");
        std::fprintf(f,
                     "  \"slope\": {\"posting_m\": %lld, \"mean_deg\": %.3f, \"p50_deg\": %.3f, "
                     "\"p90_deg\": %.3f, \"p99_deg\": %.3f, \"bimodality_bc\": %.4f, "
                     "\"hist_deg_1bin\": [",
                     (long long)strideM, slopeMean, slopeHist.pct(50), slopeHist.pct(90),
                     slopeHist.pct(99), slopeBC);
        {
            // 1-degree rebin of the slope histogram, for shape comparison.
            const int binsPerDeg = static_cast<int>(std::llround(1.0 / slopeHist.binW));
            for (int d = 0; d < 90; ++d) {
                int64_t c = 0;
                for (int b = 0; b < binsPerDeg; ++b) {
                    const size_t idx = static_cast<size_t>(d * binsPerDeg + b);
                    if (idx < slopeHist.bins.size()) c += slopeHist.bins[idx];
                }
                std::fprintf(f, "%s%lld", d ? ", " : "", (long long)c);
            }
        }
        std::fprintf(f, "]},\n");
        std::fprintf(f,
                     "  \"tri\": {\"posting_m\": %lld, \"mean_m\": %.3f, \"p50_m\": %.3f, "
                     "\"p90_m\": %.3f, \"p99_m\": %.3f},\n",
                     (long long)strideM, static_cast<double>(triSum / (sN > 0 ? sN : 1)),
                     triHist.pct(50), triHist.pct(90), triHist.pct(99));
        std::fprintf(f,
                     "  \"vrm\": {\"window\": \"3x3\", \"mean\": %.6f, \"p50\": %.6f, \"p90\": "
                     "%.6f, \"p99\": %.6f}\n",
                     vrmHist.n ? static_cast<double>(vrmSum / vrmHist.n) : 0, vrmHist.pct(50),
                     vrmHist.pct(90), vrmHist.pct(99));
        std::fprintf(f, "}\n");
        std::fclose(f);
        if (!optBaseline) std::printf("json written to %s\n", jsonPath.c_str());
    }
    return 0;
}
