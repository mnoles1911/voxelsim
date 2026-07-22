// vxc_terrainprobe — diagnostic for the coarse-to-fine detail problem.
//
// Measures the amplified surface the way the eye actually judges it:
//
//   1. STRUCTURE FUNCTION  S(d) = mean |h(x+d) - h(x)| over a transect, for
//      horizontal lags d from one voxel (0.1 m) to one tile pixel (30 m).
//      Natural terrain is approximately self-affine: S(d) ~ d^H with a
//      Hurst exponent H in roughly [0.6, 0.9]. If our amplified surface has
//      S(d) collapsing far below the coarse raster's own trend as d shrinks,
//      the detail octaves are failing to continue the spectrum — which is
//      exactly the "smooth vista / corduroy ground" signature.
//
//   2. TERRACE RUN LENGTHS. Voxelizing a surface takes floor(h / 100mm). A
//      near-planar surface produces long runs of identical voxel height —
//      contour terraces. The run-length distribution is the direct numeric
//      readout of the artifact in the ground-level screenshot: the mean and
//      p95 run length say how many voxels of dead-straight edge the eye gets
//      to lock onto.
//
// Usage: vxc_terrainprobe <tiledir> <seed> <xMetres> <yMetres> [lenMetres]
//        vxc_terrainprobe --synthetic <seed> <xMetres> <yMetres> [lenMetres]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/tilestore.h"

using namespace vxc;

namespace {

// Structure function of a 1-D height profile, in mm, at the given lags.
void structureFunction(const std::vector<int64_t>& h, const std::vector<int64_t>& lags,
                       const char* label, double unitMetres) {
    std::printf("\n%s — structure function  S(d) = mean |h(x+d)-h(x)|\n", label);
    std::printf("  %10s %12s %12s\n", "lag (m)", "S(d) (m)", "S/d");
    double prevLog = 0, prevLogS = 0;
    bool havePrev = false;
    for (int64_t d : lags) {
        if (d >= static_cast<int64_t>(h.size())) break;
        long double acc = 0;
        int64_t n = 0;
        for (size_t i = 0; i + static_cast<size_t>(d) < h.size(); ++i) {
            acc += std::abs(h[i + static_cast<size_t>(d)] - h[i]);
            ++n;
        }
        const double S = static_cast<double>(acc / n) / 1000.0; // metres
        const double dm = static_cast<double>(d) * unitMetres;
        std::printf("  %10.2f %12.4f %12.5f", dm, S, S / dm);
        if (havePrev && S > 0) {
            const double H = (std::log(S) - prevLogS) / (std::log(dm) - prevLog);
            std::printf("   local H = %.3f", H);
        }
        if (S > 0) {
            prevLog = std::log(dm);
            prevLogS = std::log(S);
            havePrev = true;
        }
        std::printf("\n");
    }
}

// SECOND-difference structure function:
//     S2(d) = mean |h(x+d) - 2h(x) + h(x-d)|
// The plain first difference on sloped ground is swamped by the mean slope —
// a perfectly smooth ramp scores S(d) = slope*d and a spurious H = 1.0, which
// tells you nothing about roughness. The second difference annihilates any
// linear trend exactly, so what is left is purely the deviation from planar:
// the quantity the eye reads as "is this ground or is this a ramp".
void curvatureFunction(const std::vector<int64_t>& h, const std::vector<int64_t>& lags,
                       const char* label, double unitMetres) {
    std::printf("\n%s — DETRENDED roughness  S2(d) = mean |h(x+d) - 2h(x) + h(x-d)|\n", label);
    std::printf("  %10s %14s %10s\n", "lag (m)", "S2(d) (m)", "in voxels");
    double prevLog = 0, prevLogS = 0;
    bool havePrev = false;
    for (int64_t d : lags) {
        if (2 * d >= static_cast<int64_t>(h.size())) break;
        long double acc = 0;
        int64_t n = 0;
        for (size_t i = static_cast<size_t>(d); i + static_cast<size_t>(d) < h.size(); ++i) {
            acc += std::abs(h[i + static_cast<size_t>(d)] - 2 * h[i] +
                            h[i - static_cast<size_t>(d)]);
            ++n;
        }
        const double S = static_cast<double>(acc / n) / 1000.0;
        const double dm = static_cast<double>(d) * unitMetres;
        std::printf("  %10.2f %14.5f %10.2f", dm, S, S * 1000.0 / kVoxelSizeMm);
        if (havePrev && S > 0)
            std::printf("   local H = %.3f", (std::log(S) - prevLogS) / (std::log(dm) - prevLog));
        if (S > 0) {
            prevLog = std::log(dm);
            prevLogS = std::log(S);
            havePrev = true;
        }
        std::printf("\n");
    }
}

void terraceStats(const std::vector<int64_t>& hMm, const char* label) {
    // Voxel height index the mesher will produce for this column.
    std::vector<int64_t> runs;
    int64_t cur = 1;
    for (size_t i = 1; i < hMm.size(); ++i) {
        const int64_t a = (hMm[i - 1] >= 0 ? hMm[i - 1] / 100 : (hMm[i - 1] - 99) / 100);
        const int64_t b = (hMm[i] >= 0 ? hMm[i] / 100 : (hMm[i] - 99) / 100);
        if (a == b) {
            ++cur;
        } else {
            runs.push_back(cur);
            cur = 1;
        }
    }
    runs.push_back(cur);
    std::sort(runs.begin(), runs.end());
    long double sum = 0;
    for (int64_t r : runs) sum += r;
    const double mean = static_cast<double>(sum / static_cast<long double>(runs.size()));
    auto pct = [&](double p) {
        return runs[std::min(runs.size() - 1,
                             static_cast<size_t>(p * static_cast<double>(runs.size())))];
    };
    std::printf("\n%s — voxel terrace runs (consecutive columns at the SAME voxel height)\n",
                label);
    std::printf("  count=%zu  mean=%.2f voxels (%.2f m)  median=%lld  p90=%lld  p99=%lld  max=%lld\n",
                runs.size(), mean, mean * 0.1, (long long)pct(0.5), (long long)pct(0.90),
                (long long)pct(0.99), (long long)runs.back());
    // Fraction of the transect sitting inside a run of >= 20 voxels (2 m of
    // perfectly straight, perfectly flat edge — the corduroy threshold).
    long double inLong = 0;
    for (int64_t r : runs)
        if (r >= 20) inLong += r;
    std::printf("  fraction of transect inside a run >= 20 voxels (2 m flat): %.1f%%\n",
                static_cast<double>(inLong * 100 / static_cast<long double>(hMm.size())));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr,
                     "usage: vxc_terrainprobe <tiledir|--synthetic> <seed> <xM> <yM> [lenM]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const uint64_t seed = std::strtoull(argv[2], nullptr, 10);
    const int64_t x0M = std::strtoll(argv[3], nullptr, 10);
    const int64_t y0M = std::strtoll(argv[4], nullptr, 10);
    const int64_t lenM = argc > 5 ? std::strtoll(argv[5], nullptr, 10) : 200;

    SyntheticTileSampler synth(seed);
    TileGridSampler grid(seed, 1);
    ITileSampler* tiles = &synth;

    if (dir != "--synthetic") {
        int loaded = 0, rejected = 0;
        for (auto& e : std::filesystem::directory_iterator(dir)) {
            if (e.path().extension() != ".vxtl") continue;
            if (grid.loadTileFile(e.path()))
                ++loaded;
            else
                ++rejected;
        }
        std::printf("tiles loaded=%d rejected=%d pixelSizeMm=%d\n", loaded, rejected,
                    grid.pixelSizeMm());
        if (loaded == 0) {
            std::fprintf(stderr, "no tiles loaded from %s\n", dir.c_str());
            return 1;
        }
        tiles = &grid;
    } else {
        std::printf("using SyntheticTileSampler pixelSizeMm=%d\n", synth.pixelSizeMm());
    }

    Amplifier amp(seed, *tiles);

    const int64_t vx0 = x0M * 1000 / kVoxelSizeMm;
    const int64_t vy0 = y0M * 1000 / kVoxelSizeMm;
    const int64_t n = lenM * 1000 / kVoxelSizeMm;

    // Amplified surface along +x.
    std::vector<int64_t> hAmp;
    hAmp.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) hAmp.push_back(amp.surfaceMm(vx0 + i, vy0));

    std::printf("\ntransect: (%lld, %lld) m, +x, %lld m at %d mm/voxel (%lld columns)\n",
                (long long)x0M, (long long)y0M, (long long)lenM, kVoxelSizeMm, (long long)n);
    std::printf("surface range: %.2f m .. %.2f m\n", hAmp.front() / 1000.0, hAmp.back() / 1000.0);

    std::vector<int64_t> lags = {1, 2, 4, 8, 16, 32, 64, 128, 300, 600, 1200};
    structureFunction(hAmp, lags, "AMPLIFIED SURFACE (10 cm columns)", 0.1);
    curvatureFunction(hAmp, lags, "AMPLIFIED SURFACE (10 cm columns)", 0.1);

    // The coarse raster's own structure function, in pixel lags, for the trend
    // the detail octaves are supposed to continue.
    std::vector<int64_t> hTile;
    const int64_t pxMm = tiles->pixelSizeMm();
    const int64_t px0 = (x0M * 1000) / pxMm, py0 = (y0M * 1000) / pxMm;
    const int64_t npx = std::max<int64_t>(64, lenM * 1000 / pxMm * 8);
    for (int64_t i = 0; i < npx; ++i) hTile.push_back(tiles->elevationMm(px0 + i, py0));
    std::vector<int64_t> plags = {1, 2, 4, 8, 16, 32, 64};
    structureFunction(hTile, plags, "COARSE RASTER (30 m pixels)",
                      static_cast<double>(pxMm) / 1000.0);
    curvatureFunction(hTile, plags, "COARSE RASTER (30 m pixels)",
                      static_cast<double>(pxMm) / 1000.0);

    terraceStats(hAmp, "AMPLIFIED SURFACE");
    return 0;
}
