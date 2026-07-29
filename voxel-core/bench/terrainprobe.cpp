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

// CARRIER-ONLY surface: the bilinear tile base with every detail octave off.
//
// This is a verbatim copy of Amplifier::bilinearBaseMm (amplifier.cpp) rather
// than a call into it, because the point is to measure the carrier IN
// ISOLATION and the amplifier has no knob to silence the octaves. Duplicating
// nine lines here is much cheaper than adding a test-only branch to worldgen
// and its HLSL mirror. If bilinearBaseMm changes, this must change with it —
// that is the intended coupling: the probe is how we prove the change worked.
int64_t carrierMm(ITileSampler& tiles, int64_t vx, int64_t vy) {
    const int64_t xMm = vx * kVoxelSizeMm;
    const int64_t yMm = vy * kVoxelSizeMm;
    const int64_t pxMm = tiles.pixelSizeMm();
    const int64_t px = floorDiv(xMm, pxMm), py = floorDiv(yMm, pxMm);
    const int64_t fx = xMm - px * pxMm, fy = yMm - py * pxMm;
    const int64_t e00 = tiles.elevationMm(px, py);
    const int64_t e10 = tiles.elevationMm(px + 1, py);
    const int64_t e01 = tiles.elevationMm(px, py + 1);
    const int64_t e11 = tiles.elevationMm(px + 1, py + 1);
    const int64_t gx = pxMm - fx, gy = pxMm - fy;
    return ((e00 * gx + e10 * fx) * gy + (e01 * gx + e11 * fx) * fy) / (pxMm * pxMm);
}

// SEAM SCAN — the direct numeric test for the visible 30 m grid.
//
// Bilinear interpolation is C0 but not C1: the surface height is continuous
// across a tile-pixel boundary but its GRADIENT steps. A slope discontinuity
// is invisible in the height and glaring under directional light, which is why
// the artifact survived every check that looked at h alone.
//
// The second difference S2 = |h(x+d) - 2h(x) + h(x-d)| annihilates any linear
// trend, so on the smooth interior of a pixel cell it measures only the detail
// octaves. On a stencil that STRADDLES a pixel boundary it additionally picks
// up the whole slope step. So split the stencils by whether [x-d, x+d] crosses
// a boundary and compare:
//
//   ratio ~ 1.0  -> no crease; the carrier is effectively C1 at this lag
//   ratio >> 1.0 -> a crease grid, of exactly the size the ratio reports
//
// Run it on the CARRIER to see the mechanism unmasked, and on the AMPLIFIED
// surface to see how much of it the detail octaves hide from the eye.
void seamScan(const std::vector<int64_t>& h, int64_t vx0, int64_t pxMm,
              const std::vector<int64_t>& lags, const char* label) {
    const int64_t cellVox = pxMm / kVoxelSizeMm; // 300 voxels per 30 m pixel
    std::printf("\n%s — SEAM SCAN (S2 straddling a %lld m pixel boundary vs interior)\n", label,
                (long long)(pxMm / 1000));
    std::printf("  %10s %14s %14s %10s %10s\n", "lag (m)", "straddle (m)", "interior (m)", "ratio",
                "n_strad");
    for (int64_t d : lags) {
        if (2 * d >= static_cast<int64_t>(h.size())) break;
        long double accS = 0, accI = 0;
        int64_t nS = 0, nI = 0;
        for (size_t i = static_cast<size_t>(d); i + static_cast<size_t>(d) < h.size(); ++i) {
            const int64_t s2 = std::abs(h[i + static_cast<size_t>(d)] - 2 * h[i] +
                                        h[i - static_cast<size_t>(d)]);
            // Does the stencil [i-d, i+d] contain a pixel boundary? Boundaries
            // sit where the absolute voxel index is a multiple of cellVox.
            const int64_t lo = vx0 + static_cast<int64_t>(i) - d;
            const int64_t hi = vx0 + static_cast<int64_t>(i) + d;
            const bool straddles = floorDiv(lo, cellVox) != floorDiv(hi, cellVox);
            if (straddles) {
                accS += s2;
                ++nS;
            } else {
                accI += s2;
                ++nI;
            }
        }
        if (nS == 0 || nI == 0) continue;
        const double S = static_cast<double>(accS / nS) / 1000.0;
        const double I = static_cast<double>(accI / nI) / 1000.0;
        std::printf("  %10.2f %14.5f %14.5f %10.2f %10lld", static_cast<double>(d) * 0.1, S, I,
                    I > 0 ? S / I : 0.0, (long long)nS);
        if (I > 0 && S / I >= 1.2) std::printf("   <-- CREASE");
        std::printf("\n");
    }
}

// CELL-GAIN STEP — the test for the second seam mechanism, which is not a
// crease in the surface but a step in the TEXTURE.
//
// tileSlopeMmPerPx is a forward difference that is constant over a whole pixel
// cell, and it drives slopeScaleQ10, which multiplies detail amplitude over a
// 0.25x..4.0x range. So the same noise field is scaled by a different gain on
// each side of every pixel line: the ground abruptly changes character at the
// boundary even where the height and slope are perfectly continuous.
//
// Measure it as local roughness per cell, then compare ACROSS cell boundaries
// against a WITHIN-cell control. The control matters: terrain roughness varies
// spatially for legitimate reasons, so a bare across-cell ratio proves nothing.
// If the gain is piecewise-constant per cell, across-steps exceed within-steps;
// if the gain is continuous, the two are equal and the excess is 1.0.
void cellGainStep(const std::vector<int64_t>& h, int64_t vx0, int64_t pxMm, int64_t dVox,
                  const char* label) {
    const int64_t cellVox = pxMm / kVoxelSizeMm;
    // Mean local roughness over each half-cell along the transect.
    std::vector<double> halfAmp;
    std::vector<int64_t> halfCell;
    const int64_t halfVox = cellVox / 2;
    for (int64_t start = 0; start + halfVox < static_cast<int64_t>(h.size()); start += halfVox) {
        long double acc = 0;
        int64_t n = 0;
        for (int64_t i = start; i < start + halfVox; ++i) {
            if (i - dVox < 0 || i + dVox >= static_cast<int64_t>(h.size())) continue;
            acc += std::abs(h[static_cast<size_t>(i + dVox)] - 2 * h[static_cast<size_t>(i)] +
                            h[static_cast<size_t>(i - dVox)]);
            ++n;
        }
        if (n == 0) continue;
        halfAmp.push_back(static_cast<double>(acc / n));
        halfCell.push_back(floorDiv(vx0 + start + halfVox / 2, cellVox));
    }
    // |ln(a1/a0)| for adjacent half-cells, split by whether the pair crosses a
    // cell boundary.
    std::vector<double> across, within;
    for (size_t i = 1; i < halfAmp.size(); ++i) {
        if (halfAmp[i] <= 0 || halfAmp[i - 1] <= 0) continue;
        const double v = std::abs(std::log(halfAmp[i] / halfAmp[i - 1]));
        if (halfCell[i] != halfCell[i - 1])
            across.push_back(v);
        else
            within.push_back(v);
    }
    if (across.empty() || within.empty()) {
        std::printf("\n%s — CELL-GAIN STEP: not enough samples\n", label);
        return;
    }
    auto med = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    const double a = med(across), w = med(within);
    std::printf("\n%s — CELL-GAIN STEP (median |ln| roughness ratio between adjacent half-cells)\n",
                label);
    std::printf("  across a cell boundary: %.4f  (n=%zu)\n", a, across.size());
    std::printf("  within  a cell        : %.4f  (n=%zu)   <- control\n", w, within.size());
    std::printf("  excess (across/within): %.2f", w > 0 ? a / w : 0.0);
    if (w > 0 && a / w >= 1.2) std::printf("   <-- TEXTURE STEPS ON THE PIXEL GRID");
    std::printf("\n");
}

// DIRECT GAIN STEP — measure the designed discontinuity, not its echo.
//
// A windowed roughness estimate is the wrong instrument for this mechanism:
// local roughness at a small lag is dominated by the MICRORELIEF band, whose
// gate (microScaleQ10) is deliberately almost slope-flat, so the window mostly
// measures the one band that does not step. The band that steps is the
// LANDFORM band under slopeScaleQ10, which spans 0.25x..4.0x.
//
// So read the mechanism directly off the tile raster: recompute the per-cell
// gains exactly as evalSurface does and report how much they jump between
// adjacent cells. No windowing, no estimator noise — this is the actual size
// of the discontinuity the shader applies.
//
// These four helpers are verbatim copies of amplifier.cpp; same coupling note
// as carrierMm above.
int64_t probeTileSlopeMmPerPx(int64_t e00, int64_t e10, int64_t e01) {
    return (e10 > e00 ? e10 - e00 : e00 - e10) + (e01 > e00 ? e01 - e00 : e00 - e01);
}
int64_t probeClamp(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
int64_t probeSlopeScaleQ10(int64_t s) { return probeClamp(512 + s / 24, 256, 4096); }
int64_t probeMicroScaleQ10(int64_t s) { return probeClamp(768 + s / 256, 768, 1280); }

void gainStepDirect(ITileSampler& tiles, int64_t vx0, int64_t vy0, int64_t nCells,
                    const char* label) {
    const int64_t pxMm = tiles.pixelSizeMm();
    const int64_t px0 = floorDiv(vx0 * kVoxelSizeMm, pxMm);
    const int64_t py = floorDiv(vy0 * kVoxelSizeMm, pxMm);

    // Nominal pre-gain amplitude of each band, from the kDetailOctaves table.
    const double landformNomMm = 2600.0 + 1100.0;
    const double microNomMm = 500.0 + 190.0 + 60.0;

    std::vector<double> sRatio, envStepMm;
    int64_t prevS = -1, prevM = -1;
    for (int64_t k = 0; k < nCells; ++k) {
        const int64_t px = px0 + k;
        const int64_t e00 = tiles.elevationMm(px, py);
        const int64_t e10 = tiles.elevationMm(px + 1, py);
        const int64_t e01 = tiles.elevationMm(px, py + 1);
        const int64_t s = probeTileSlopeMmPerPx(e00, e10, e01);
        const int64_t sQ = probeSlopeScaleQ10(s), mQ = probeMicroScaleQ10(s);
        if (prevS >= 0) {
            sRatio.push_back(sQ >= prevS ? static_cast<double>(sQ) / prevS
                                         : static_cast<double>(prevS) / sQ);
            // Millimetres of detail envelope that change discontinuously
            // across this one boundary line.
            envStepMm.push_back(std::abs(landformNomMm * (sQ - prevS) / 1024.0) +
                                std::abs(microNomMm * (mQ - prevM) / 1024.0));
        }
        prevS = sQ;
        prevM = mQ;
    }
    if (sRatio.empty()) {
        std::printf("\n%s — DIRECT GAIN STEP: no cells\n", label);
        return;
    }
    auto pct = [](std::vector<double> v, double p) {
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))];
    };
    std::printf("\n%s — DIRECT GAIN STEP across %lld adjacent %lld m cells\n", label,
                (long long)sRatio.size(), (long long)(pxMm / 1000));
    std::printf("  slopeScale ratio  median=%.2fx  p90=%.2fx  max=%.2fx\n", pct(sRatio, 0.5),
                pct(sRatio, 0.90), pct(sRatio, 1.0));
    std::printf("  detail envelope step  median=%.0f mm  p90=%.0f mm  max=%.0f mm\n",
                pct(envStepMm, 0.5), pct(envStepMm, 0.90), pct(envStepMm, 1.0));
    std::printf("  (a step of ~100 mm is one voxel of texture amplitude appearing across a "
                "dead-straight %lld m line)\n",
                (long long)(pxMm / 1000));
}

// DIRECTIONAL ROUGHNESS — does the ground have downslope grain?
//
// Real hillslopes are grooved by water: rills run down the gradient. Walking
// DOWN a rill you stay in the groove and the surface is smooth; walking ACROSS
// the slope you cross groove after groove and it is rough. So natural slopes
// are anisotropic, with S2(across) > S2(along), while isotropic fBm scores
// exactly 1.0 by construction. This is the metric that says whether the detail
// has structure or is merely textured.
//
// The gradient is taken at a +/-25 m baseline so it is the LANDFORM slope, not
// whatever the microrelief octaves happen to be doing at this voxel.
void directionalRoughness(Amplifier& amp, int64_t vx0, int64_t vy0, int64_t n,
                          const std::vector<int64_t>& lags, const char* label) {
    const int64_t base = 250; // 25 m in voxels
    const double gx = static_cast<double>(amp.surfaceMm(vx0 + base, vy0) -
                                          amp.surfaceMm(vx0 - base, vy0));
    const double gy = static_cast<double>(amp.surfaceMm(vx0, vy0 + base) -
                                          amp.surfaceMm(vx0, vy0 - base));
    const double gl = std::sqrt(gx * gx + gy * gy);
    const double gradePct = gl / (2.0 * static_cast<double>(base) * kVoxelSizeMm) * 100.0;
    std::printf("\n%s — DIRECTIONAL ROUGHNESS (local grade %.1f%%)\n", label, gradePct);
    if (gl < 1.0) {
        std::printf("  gradient too small to define a frame; skipping\n");
        return;
    }
    const double ux = gx / gl, uy = gy / gl; // downslope unit
    // Sample along the gradient and along the contour (perpendicular).
    std::vector<int64_t> along, across;
    along.reserve(static_cast<size_t>(n));
    across.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i);
        along.push_back(amp.surfaceMm(vx0 + static_cast<int64_t>(std::lround(ux * t)),
                                      vy0 + static_cast<int64_t>(std::lround(uy * t))));
        across.push_back(amp.surfaceMm(vx0 + static_cast<int64_t>(std::lround(-uy * t)),
                                       vy0 + static_cast<int64_t>(std::lround(ux * t))));
    }
    std::printf("  %10s %14s %14s %10s\n", "lag (m)", "along (m)", "across (m)", "across/along");
    for (int64_t d : lags) {
        if (2 * d >= static_cast<int64_t>(along.size())) break;
        auto s2 = [&](const std::vector<int64_t>& v) {
            long double acc = 0;
            int64_t c = 0;
            for (size_t i = static_cast<size_t>(d); i + static_cast<size_t>(d) < v.size(); ++i) {
                acc += std::abs(v[i + static_cast<size_t>(d)] - 2 * v[i] +
                                v[i - static_cast<size_t>(d)]);
                ++c;
            }
            return static_cast<double>(acc / c) / 1000.0;
        };
        const double A = s2(along), C = s2(across);
        std::printf("  %10.2f %14.5f %14.5f %10.2f\n", static_cast<double>(d) * 0.1, A, C,
                    A > 0 ? C / A : 0.0);
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

// 2-D TERRACE PLATEAUS — the metric that actually matches the screenshot.
//
// A 1-D transect can tell you a step happens every N columns, but not whether
// the steps LINE UP across rows, and lining up is the whole artifact: a
// terrace edge that runs dead straight for tens of metres reads as machined,
// while the same number of steps scattered reads as ground. So take the 2-D
// field of voxel heights floor(h/100mm) and measure the connected components
// of CONSTANT height (4-connectivity). Long straight terraces are large
// components; natural ground is a confetti of tiny ones.
void terracePlateaus(Amplifier& amp, int64_t vx0, int64_t vy0, int64_t n, const char* label) {
    std::vector<int64_t> hv(static_cast<size_t>(n * n));
    for (int64_t j = 0; j < n; ++j)
        for (int64_t i = 0; i < n; ++i) {
            const int64_t mm = amp.surfaceMm(vx0 + i, vy0 + j);
            hv[static_cast<size_t>(j * n + i)] = (mm >= 0 ? mm / 100 : (mm - 99) / 100);
        }

    std::vector<char> seen(static_cast<size_t>(n * n), 0);
    std::vector<int64_t> areas;
    std::vector<int64_t> stack;
    for (int64_t s = 0; s < n * n; ++s) {
        if (seen[static_cast<size_t>(s)]) continue;
        const int64_t lvl = hv[static_cast<size_t>(s)];
        int64_t area = 0;
        stack.clear();
        stack.push_back(s);
        seen[static_cast<size_t>(s)] = 1;
        while (!stack.empty()) {
            const int64_t c = stack.back();
            stack.pop_back();
            ++area;
            const int64_t cx = c % n, cy = c / n;
            const int64_t nb[4][2] = {{cx - 1, cy}, {cx + 1, cy}, {cx, cy - 1}, {cx, cy + 1}};
            for (auto& q : nb) {
                if (q[0] < 0 || q[0] >= n || q[1] < 0 || q[1] >= n) continue;
                const int64_t k = q[1] * n + q[0];
                if (seen[static_cast<size_t>(k)] || hv[static_cast<size_t>(k)] != lvl) continue;
                seen[static_cast<size_t>(k)] = 1;
                stack.push_back(k);
            }
        }
        areas.push_back(area);
    }
    std::sort(areas.begin(), areas.end());
    long double sum = 0;
    for (int64_t a : areas) sum += a;
    // Area-weighted mean: the size of the plateau a RANDOMLY CHOSEN VOXEL sits
    // in, which is what the eye samples — not the mean over components, which
    // is dominated by the confetti and flatters the result.
    long double w = 0;
    for (int64_t a : areas) w += static_cast<long double>(a) * a;
    std::printf("\n%s — 2-D constant-height plateaus over a %lldx%lld voxel patch (%.1f m sq)\n",
                label, (long long)n, (long long)n, static_cast<double>(n) * 0.1);
    std::printf("  components=%zu  largest=%lld voxels  area-weighted mean=%.1f voxels\n",
                areas.size(), (long long)areas.back(),
                static_cast<double>(w / sum));
    long double big = 0;
    for (int64_t a : areas)
        if (a >= 400) big += a; // 400 voxels = 4 m^2 of dead-flat, dead-level ground
    std::printf("  fraction of area in plateaus >= 400 voxels (4 m sq flat): %.1f%%\n",
                static_cast<double>(big * 100 / sum));
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

    // --- Phase 0 seam instrumentation -------------------------------------
    //
    // The carrier transect is the same line with every detail octave off, so
    // the bilinear slope step is unmasked. Run the seam scan on both: the
    // carrier says how big the crease IS, the amplified surface says how much
    // of it the eye still gets after the noise is laid over it.
    std::vector<int64_t> hCar;
    hCar.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) hCar.push_back(carrierMm(*tiles, vx0 + i, vy0));

    curvatureFunction(hCar, lags, "CARRIER ONLY (bilinear base, no octaves)", 0.1);

    const std::vector<int64_t> seamLags = {1, 2, 4, 8, 16, 32, 64, 128};
    seamScan(hCar, vx0, pxMm, seamLags, "CARRIER ONLY");
    seamScan(hAmp, vx0, pxMm, seamLags, "AMPLIFIED SURFACE");

    // 2 voxels is the finest lag at which S2 is a shape rather than per-voxel
    // static, so it is the cleanest read on local roughness AMPLITUDE.
    cellGainStep(hAmp, vx0, pxMm, 2, "AMPLIFIED SURFACE");
    gainStepDirect(*tiles, vx0, vy0, std::max<int64_t>(8, n * kVoxelSizeMm / pxMm),
                   "TILE RASTER");

    directionalRoughness(amp, vx0, vy0, n, {8, 16, 32, 64}, "AMPLIFIED SURFACE");

    terraceStats(hAmp, "AMPLIFIED SURFACE");
    terracePlateaus(amp, vx0, vy0, 512, "AMPLIFIED SURFACE");

    // Optional raw dump of the voxel-quantised height field, for hillshading
    // offline. A hillshade of floor(h/100mm) shows the terrace artifact
    // exactly as the eye sees it in-engine, in about a second — which makes
    // parameter iteration a tight loop instead of a UE rebuild each time.
    if (const char* out = std::getenv("VXC_PROBE_DUMP")) {
        const int64_t nn = 512;
        std::vector<int32_t> f(static_cast<size_t>(nn * nn));
        for (int64_t j = 0; j < nn; ++j)
            for (int64_t i = 0; i < nn; ++i)
                f[static_cast<size_t>(j * nn + i)] = amp.surfaceMm(vx0 + i, vy0 + j);
        FILE* fp = std::fopen(out, "wb");
        if (fp) {
            std::fwrite(f.data(), sizeof(int32_t), f.size(), fp);
            std::fclose(fp);
            std::printf("\ndumped %lldx%lld int32 surfaceMm to %s\n", (long long)nn,
                        (long long)nn, out);
        }
    }
    return 0;
}
