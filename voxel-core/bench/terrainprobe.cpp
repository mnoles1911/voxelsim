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
//   3. STRUCTURE METRICS (drainage connectivity, curvature-conditioned
//      roughness, grade-binned rill anisotropy). See the long block above
//      `struct Raster` for why these exist: the plan's "H in [0.6, 1.0]"
//      acceptance criterion was DEMONSTRATED to be satisfiable by the exact
//      failure this project exists to fix, so Phase 3 needs gates that an
//      isotropic noise field cannot pass however its amplitudes are tuned.
//
// Usage: vxc_terrainprobe <tiledir> <seed> <xMetres> <yMetres> [lenMetres] [options]
//        vxc_terrainprobe --synthetic <seed> <xMetres> <yMetres> [lenMetres] [pixelMm] [options]
//
// Options (all may appear anywhere after the program name):
//   --baseline            print ONLY the compact machine-greppable metric
//                         table — fixed field names, no timings, no
//                         timestamps — so the same command run before and
//                         after a worldgen change diffs cleanly.
//   --drain-n N           drainage lattice cells per axis      (default 384)
//   --drain-stride V      drainage lattice stride, in voxels   (default 10 = 1.0 m)
//   --field-n N           point-metric lattice cells per axis  (default 96)
//   --field-stride V      point-metric stride, in voxels       (default 25 = 2.5 m)
//   --no-structure        skip the structure metrics entirely (legacy report only)
//
// Sampling the drainage raster is the expensive part of a run: cost is
// O(drain-n^2) amplifier evaluations, twice (amplified and carrier). The
// defaults are a 384 m domain at 1 m cells, which is a couple of seconds.
//
// pixelMm is SYNTHETIC-ONLY and defaults to 30000. Real tiles carry their own
// pixel size in the file, so overriding it there would be a lie about the data;
// the tool rejects it rather than silently ignoring it. What it buys is the
// bound sweep below: the corner-grid shape a footprint produces depends
// entirely on the pixel size, so `--synthetic ... 1875` is the cheapest way to
// exercise the .vxtl v2 fine tier (docs/vxtl-v2-format.md) without baking one.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <queue>
#include <string>
#include <utility>
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

// CARRIER-ONLY surface: the tile base with every detail octave off.
//
// This is a copy of the carrier from amplifier.cpp rather than a call into it,
// because the point is to measure the carrier IN ISOLATION and the amplifier
// has no knob to silence the octaves. Duplicating it here is much cheaper than
// adding a test-only branch to worldgen and its HLSL mirror. If the carrier
// changes, this must change with it — that is the intended coupling: the probe
// is how we prove the change worked.
//
// v9: uniform cubic B-spline over a 4x4 control stencil, replacing bilinear.
// See amplifier.cpp's carrier block for why, and for the fixed-point rules.
constexpr int64_t kCarrierT = 1024;
constexpr int64_t kCarrierValueDen = 6 * kCarrierT * kCarrierT * kCarrierT;
constexpr int64_t kCarrierSlopeDen = 2 * kCarrierT * kCarrierT;

struct ProbeW4 {
    int64_t w[4];
};
ProbeW4 probeValueW(int64_t tq) {
    const int64_t u = kCarrierT - tq, T = kCarrierT;
    return ProbeW4{{u * u * u, 3 * tq * tq * tq - 6 * tq * tq * T + 4 * T * T * T,
                    -3 * tq * tq * tq + 3 * tq * tq * T + 3 * tq * T * T + T * T * T,
                    tq * tq * tq}};
}
struct ProbeW3 {
    int64_t w[3];
};
ProbeW3 probeSlopeW(int64_t tq) {
    const int64_t u = kCarrierT - tq, T = kCarrierT;
    return ProbeW3{{u * u, -2 * tq * tq + 2 * T * tq + T * T, tq * tq}};
}

struct ProbeCarrier {
    int64_t heightMm = 0;
    int64_t sxMmPerPx = 0;
    int64_t syMmPerPx = 0;
};

ProbeCarrier evalProbeCarrier(ITileSampler& tiles, int64_t vx, int64_t vy) {
    const int64_t xMm = vx * kVoxelSizeMm, yMm = vy * kVoxelSizeMm;
    const int64_t pxMm = tiles.pixelSizeMm();
    const int64_t px = floorDiv(xMm, pxMm), py = floorDiv(yMm, pxMm);
    const int64_t fx = xMm - px * pxMm, fy = yMm - py * pxMm;
    int64_t cp[16];
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            cp[i + 4 * j] = tiles.elevationMm(px - 1 + i, py - 1 + j);

    const int64_t tx = fx * kCarrierT / pxMm, ty = fy * kCarrierT / pxMm;
    const ProbeW4 wx = probeValueW(tx), wy = probeValueW(ty);
    const ProbeW3 dx = probeSlopeW(tx), dy = probeSlopeW(ty);

    int64_t rowVal[4], rowDx[4];
    for (int j = 0; j < 4; ++j) {
        const int64_t* r = cp + 4 * j;
        int64_t v = 0, d = 0;
        for (int i = 0; i < 4; ++i) v += r[i] * wx.w[i];
        for (int i = 0; i < 3; ++i) d += (r[i + 1] - r[i]) * dx.w[i];
        rowVal[j] = v / kCarrierValueDen;
        rowDx[j] = d / kCarrierSlopeDen;
    }
    int64_t h = 0, sx = 0, sy = 0;
    for (int j = 0; j < 4; ++j) {
        h += rowVal[j] * wy.w[j];
        sx += rowDx[j] * wy.w[j];
    }
    for (int j = 0; j < 3; ++j) sy += (rowVal[j + 1] - rowVal[j]) * dy.w[j];

    ProbeCarrier c;
    c.heightMm = h / kCarrierValueDen;
    c.sxMmPerPx = sx / kCarrierValueDen;
    c.syMmPerPx = sy / kCarrierSlopeDen;
    return c;
}

int64_t carrierMm(ITileSampler& tiles, int64_t vx, int64_t vy) {
    return evalProbeCarrier(tiles, vx, vy).heightMm;
}

int64_t probeSlopeMmPerM(const ProbeCarrier& c, int64_t pxMm) {
    const int64_t ax = c.sxMmPerPx < 0 ? -c.sxMmPerPx : c.sxMmPerPx;
    const int64_t ay = c.syMmPerPx < 0 ? -c.syMmPerPx : c.syMmPerPx;
    return (ax + ay) * 1000 / pxMm;
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
// The gain curves, copied from amplifier.cpp; same coupling note as carrierMm.
int64_t probeClamp(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
int64_t probeSlopeScaleQ10(int64_t s) { return probeClamp(512 + s * 5 / 4, 256, 4096); }
int64_t probeMicroScaleQ10(int64_t s) { return probeClamp(768 + s * 15 / 128, 768, 1280); }

// The detail envelope (mm of amplitude) this column's gains admit.
double probeEnvelopeMm(ITileSampler& tiles, int64_t vx, int64_t vy) {
    // Nominal pre-gain amplitude of each band, from the kDetailOctaves table.
    const double landformNomMm = 2600.0 + 1100.0;
    const double microNomMm = 500.0 + 190.0 + 60.0;
    const int64_t s = probeSlopeMmPerM(evalProbeCarrier(tiles, vx, vy), tiles.pixelSizeMm());
    return landformNomMm * probeSlopeScaleQ10(s) / 1024.0 +
           microNomMm * probeMicroScaleQ10(s) / 1024.0;
}

void gainStepDirect(ITileSampler& tiles, int64_t vx0, int64_t vy0, int64_t nCells,
                    const char* label) {
    const int64_t pxMm = tiles.pixelSizeMm();
    const int64_t cellVox = pxMm / kVoxelSizeMm;
    // First cell boundary at or after vx0, in absolute voxel indices.
    const int64_t b0 = (floorDiv(vx0, cellVox) + 1) * cellVox;

    // Sample the envelope two voxels apart, once STRADDLING a cell boundary and
    // once at the middle of the same cell as a control. Under v8 the gain was a
    // per-cell constant, so the straddling pair jumped and the control pair was
    // identically zero; under v9 the gain is continuous and the two should be
    // the same small number. Reporting both is what makes the result readable
    // without knowing which version produced it.
    std::vector<double> across, within;
    for (int64_t k = 0; k + 1 < nCells; ++k) {
        const int64_t b = b0 + k * cellVox;
        across.push_back(std::abs(probeEnvelopeMm(tiles, b + 1, vy0) -
                                  probeEnvelopeMm(tiles, b - 1, vy0)));
        const int64_t m = b + cellVox / 2;
        within.push_back(std::abs(probeEnvelopeMm(tiles, m + 1, vy0) -
                                  probeEnvelopeMm(tiles, m - 1, vy0)));
    }
    if (across.empty()) {
        std::printf("\n%s — GAIN STEP AT CELL BOUNDARIES: no cells\n", label);
        return;
    }
    auto pct = [](std::vector<double> v, double p) {
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))];
    };
    std::printf("\n%s — GAIN STEP AT CELL BOUNDARIES (detail envelope change over 0.2 m)\n",
                label);
    std::printf("  straddling a %lld m boundary: median=%.0f mm  p90=%.0f mm  max=%.0f mm\n",
                (long long)(pxMm / 1000), pct(across, 0.5), pct(across, 0.90), pct(across, 1.0));
    std::printf("  mid-cell control           : median=%.0f mm  p90=%.0f mm  max=%.0f mm\n",
                pct(within, 0.5), pct(within, 0.90), pct(within, 1.0));
    std::printf("  (a step of ~100 mm is one voxel of texture amplitude appearing across a "
                "dead-straight %lld m line)\n",
                (long long)(pxMm / 1000));
}

// MATERIAL BOUNDARY ALIGNMENT — does the surface material change ON the grid?
//
// The third mechanism behind the visible 30 m squares is not a height artifact
// at all. Climate was read NEAREST-PIXEL, so classifyBiome's inputs were
// piecewise constant per tile pixel, and surfaceMat could only change where a
// column crossed a pixel edge (or where its own elevation crossed the treeline
// or beach band). The result is material patches with straight edges on a 30 m
// lattice — visible as colour, not as shading, which is why the seam scan
// cannot see it.
//
// Measure it directly: walk a transect, find every column where surfaceMat
// differs from its neighbour, and ask what fraction of those changes land
// within one voxel of a pixel boundary. With cellVox = 300 and a +/-1 voxel
// window, chance alone puts 3/300 = 1.0% there. A large excess over chance
// means the material field is keyed to the grid; ~1x means it is not.
void materialBoundaryAlignment(Amplifier& amp, int64_t vx0, int64_t vy0, int64_t n,
                               int64_t pxMm, const char* label) {
    const int64_t cellVox = pxMm / kVoxelSizeMm;
    int64_t changes = 0, onBoundary = 0;
    MaterialId prev = amp.column(vx0, vy0).surfaceMat;
    for (int64_t i = 1; i < n; ++i) {
        const MaterialId m = amp.column(vx0 + i, vy0).surfaceMat;
        if (m != prev) {
            ++changes;
            // Distance from this column to the nearest cell boundary.
            const int64_t r = floorMod(vx0 + i, cellVox);
            const int64_t dist = r < cellVox - r ? r : cellVox - r;
            if (dist <= 1) ++onBoundary;
        }
        prev = m;
    }
    const double chancePct = 3.0 * 100.0 / static_cast<double>(cellVox);
    std::printf("\n%s — MATERIAL BOUNDARY ALIGNMENT over %lld columns\n", label, (long long)n);
    if (changes == 0) {
        std::printf("  no material changes on this transect (uniform biome); "
                    "inconclusive — try a longer or different transect\n");
        return;
    }
    const double obsPct = static_cast<double>(onBoundary) * 100.0 / static_cast<double>(changes);
    std::printf("  material changes: %lld;  on a %lld m boundary (+/-1 voxel): %lld (%.1f%%)\n",
                (long long)changes, (long long)(pxMm / 1000), (long long)onBoundary, obsPct);
    std::printf("  by chance: %.1f%%   excess: %.1fx", chancePct, obsPct / chancePct);
    if (obsPct / chancePct >= 2.0) std::printf("   <-- MATERIAL KEYED TO THE PIXEL GRID");
    std::printf("\n");
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

// ADVERSARIAL BOUND SWEEP OVER REAL TILES.
//
// surfaceUpperBoundMm / surfaceLowerBoundMm / solidBelowBoundMm are SAFETY
// primitives: the streaming layer skips generating any chunk they prove empty
// or fully solid, so a bound that is too tight is not a lost optimisation, it
// is terrain that never generates. A hole in the world.
//
// test_amplifier.cpp already sweeps them adversarially over
// SyntheticTileSampler at four pixel sizes. Real diffusion tiles are a
// different shape of input: kilometres of near-flat ocean, and cliffs where the
// 30 m raster steps hard. v9's bound is a Lipschitz envelope around ONE centre
// evaluation, and its tightness depends on the footprint-to-relief ratio, so
// synthetic coverage does not transfer for free.
//
// It runs on whatever sampler main() built, which is the point of the
// --synthetic pixelMm argument: the DECLINE column below is a pure function of
// the pixel size and the footprint, so a synthetic run at 1875 answers "does
// the cascade still get a computed bound on the fine tier?" without a baked
// fine tile existing. The slack columns from a synthetic run are not
// comparable with a real-tile run -- different terrain -- but the decline and
// violation columns are exactly as meaningful.
//
// Sampling can only ever MISS a violation, never invent one: every reported
// failure is a real counterexample.
void boundSweep(Amplifier& amp, int64_t vx0, int64_t vy0, int64_t spanVox, int32_t levels,
                int64_t pxMm, const char* source) {
    std::printf("\nADVERSARIAL BOUND SWEEP over %s (%lld mm/px)\n", source, (long long)pxMm);
    std::printf("  %5s %9s %9s %9s %10s %12s %12s %12s\n", "level", "edge (m)", "points/ax",
                "chunks", "declined", "upper slack", "lower slack", "violations");
    int64_t totalViol = 0;
    for (int32_t level = 0; level < levels; ++level) {
        const int64_t chunk = int64_t(32) << level;   // level-L chunk edge in level-0 voxels
        int64_t n = 0, declined = 0, viol = 0;
        long double slackHi = 0, slackLo = 0;
        int64_t worstHi = 0;
        for (int64_t cy = 0; cy + chunk <= spanVox; cy += chunk * 3) {
            for (int64_t cx = 0; cx + chunk <= spanVox; cx += chunk * 3) {
                const int64_t x0 = vx0 + cx, y0 = vy0 + cy;
                const int64_t x1 = x0 + chunk - 1, y1 = y0 + chunk - 1;
                const int64_t hi = amp.surfaceUpperBoundMm(x0, y0, x1, y1);
                const int64_t lo = amp.surfaceLowerBoundMm(x0, y0, x1, y1);
                const int64_t floorMm = amp.solidBelowBoundMm(x0, y0, x1, y1);
                ++n;
                if (hi == kSurfaceBoundDeclined || lo == kSurfaceLowerBoundDeclined) {
                    ++declined;
                    continue;
                }
                // Sample the footprint densely but boundedly; corners included.
                const int64_t step = chunk > 64 ? chunk / 64 : 1;
                int64_t maxS = INT64_MIN, minS = INT64_MAX;
                for (int64_t y = y0; y <= y1; y += step)
                    for (int64_t x = x0; x <= x1; x += step) {
                        const int64_t s = amp.surfaceMm(x, y);
                        if (s > maxS) maxS = s;
                        if (s < minS) minS = s;
                    }
                for (int64_t y : {y0, y1})
                    for (int64_t x : {x0, x1}) {
                        const int64_t s = amp.surfaceMm(x, y);
                        if (s > maxS) maxS = s;
                        if (s < minS) minS = s;
                    }
                if (maxS > hi) {
                    ++viol;
                    if (viol <= 3)
                        std::printf("    !! UPPER VIOLATED at (%lld,%lld) L%d: bound %lld < "
                                    "surface %lld\n",
                                    (long long)x0, (long long)y0, level, (long long)hi,
                                    (long long)maxS);
                }
                if (minS < lo) {
                    ++viol;
                    if (viol <= 3)
                        std::printf("    !! LOWER VIOLATED at (%lld,%lld) L%d: bound %lld > "
                                    "surface %lld\n",
                                    (long long)x0, (long long)y0, level, (long long)lo,
                                    (long long)minS);
                }
                // solidBelowBoundMm: every voxel strictly below the floor must
                // be non-air. Probe a few columns down a short way.
                if (floorMm != kSurfaceLowerBoundDeclined) {
                    const int64_t vz = floorDiv(floorMm, kVoxelSizeMm) - 1;
                    for (int64_t y : {y0, (y0 + y1) / 2, y1})
                        for (int64_t x : {x0, (x0 + x1) / 2, x1})
                            if (amp.materialAt(x, y, vz) == MAT_AIR) {
                                ++viol;
                                if (viol <= 3)
                                    std::printf("    !! SOLID-BELOW VIOLATED at (%lld,%lld,%lld) "
                                                "L%d: air beneath floor %lld\n",
                                                (long long)x, (long long)y, (long long)vz, level,
                                                (long long)floorMm);
                            }
                }
                slackHi += static_cast<long double>(hi - maxS);
                slackLo += static_cast<long double>(minS - lo);
                if (hi - maxS > worstHi) worstHi = hi - maxS;
            }
        }
        const int64_t used = n - declined;
        // Control points the widest footprint at this level needs per axis:
        // cells touched (phase-dependent, hence the +2) plus the cubic
        // carrier's dilation of 3. This is the number
        // kSurfaceBoundMaxCornersPerAxis has to cover, printed next to the
        // decline count so the two can be read together.
        const int64_t pointsPerAxis = (chunk - 1) * kVoxelSizeMm / pxMm + 2 + 3;
        std::printf("  %5d %9.1f %9lld %9lld %10lld %10.2f m %10.2f m %12lld%s\n", level,
                    static_cast<double>(chunk) * kVoxelSizeMm / 1000.0, (long long)pointsPerAxis,
                    (long long)n, (long long)declined,
                    used ? static_cast<double>(slackHi / used) / 1000.0 : 0.0,
                    used ? static_cast<double>(slackLo / used) / 1000.0 : 0.0,
                    (long long)viol, viol ? "   <-- HOLE IN THE WORLD" : "");
        totalViol += viol;
    }
    std::printf("  worst case is a bound that is TOO TIGHT; %lld violation(s) total\n",
                (long long)totalViol);
}

// ===========================================================================
//  STRUCTURE METRICS — the things a spectrum cannot see
// ===========================================================================
//
// WHY THIS BLOCK EXISTS. docs/terrain-amplification-plan.md gates Phase 3 on
// "H in [0.6, 1.0] at every lag decade". Phase 2 prototyping DEMONSTRATED that
// this gate is satisfiable by the exact failure the project exists to fix:
// `bake_prototype.py --rough -1` tuned B1 to the fitted target spectrum, H at
// the fine end went 1.65 -> 0.91 (squarely physical), and in the same step the
// dendritic flow field collapsed into a confetti of disconnected
// micro-catchments. A correct spectrum was necessary and nowhere near
// sufficient.
//
// The reason is structural, not a tuning accident. H is derived from a TWO-
// POINT statistic: it says how much energy sits at each scale and nothing
// whatever about whether the surface is CONNECTED, because connectivity is not
// a two-point property. Anything you can compute from S(d) alone, an
// isotropic noise field can be tuned to match.
//
// So every metric below is chosen for one property: an isotropic value-noise
// field must FAIL it no matter how its amplitudes are tuned.
//
//   1. DRAINAGE CONNECTIVITY. Flow routing is a NON-LOCAL operator — where a
//      cell's water goes depends on the whole upstream basin. No point
//      function of world coordinates can produce a connected network, which is
//      exactly why the plan moves erosion into the server bake instead of
//      trying harder at noise. Falsifies what H cannot: a field can have any
//      spectrum you like and still strand every drop of water in a private
//      pit two metres from where it fell.
//
//   2. CURVATURE-CONDITIONED ROUGHNESS. Asks whether roughness is
//      CONDITIONED on the shape of the surface it sits on. Stationary fBm is
//      conditioned on nothing by construction, so its structure function is
//      identical in every curvature bin and the convex/concave ratio is
//      exactly 1.0. H is an average over all bins and cannot distinguish
//      "crisp crests, soft hollows" from "uniformly crumpled" at all.
//
//   3. GRADE-BINNED ANISOTROPY. Asks whether roughness has a preferred
//      direction AND whether that direction is correctly ABSENT where the
//      terrain gives no direction. H is a scalar per lag; it is blind to
//      direction by definition.
//
// UNITS. Heights are millimetres throughout. Areas are square metres, lengths
// metres, drainage density inverse metres (metres of channel per square
// metre). Curvature is millimetres per square metre. Every printed table
// states its lattice size and sample count so a reader can tell what was
// actually measured.
//
// DETERMINISM. Every lattice is anchored at the transect origin with a fixed
// integer stride; every sort and priority queue breaks ties on cell index;
// there is no rand(), no clock, and no thread-order dependence. Two runs of
// the same command produce byte-identical output.

// A sampled height raster. Floating point is legal here: bench/ is outside the
// float ban, which CI applies to voxel-core/include and voxel-core/src only
// (.github/workflows/ci.yml, job `float-ban`). Nothing in this block feeds
// worldgen; it only reads it.
struct Raster {
    int64_t n = 0;           // cells per axis
    int64_t strideVox = 0;   // lattice stride, level-0 voxels
    double cellM = 0;        // cell size, metres
    std::vector<double> zMm; // heights in mm, row-major, index = y*n + x
    double domainM() const { return static_cast<double>(n) * cellM; }
    double cellAreaM2() const { return cellM * cellM; }
    double domainAreaM2() const { return domainM() * domainM(); }
};

Raster sampleAmplifiedRaster(Amplifier& amp, int64_t vx0, int64_t vy0, int64_t n,
                             int64_t strideVox) {
    Raster r;
    r.n = n;
    r.strideVox = strideVox;
    r.cellM = static_cast<double>(strideVox) * kVoxelSizeMm / 1000.0;
    r.zMm.resize(static_cast<size_t>(n * n));
    for (int64_t j = 0; j < n; ++j)
        for (int64_t i = 0; i < n; ++i)
            r.zMm[static_cast<size_t>(j * n + i)] =
                static_cast<double>(amp.surfaceMm(vx0 + i * strideVox, vy0 + j * strideVox));
    return r;
}

Raster sampleCarrierRaster(ITileSampler& tiles, int64_t vx0, int64_t vy0, int64_t n,
                           int64_t strideVox) {
    Raster r;
    r.n = n;
    r.strideVox = strideVox;
    r.cellM = static_cast<double>(strideVox) * kVoxelSizeMm / 1000.0;
    r.zMm.resize(static_cast<size_t>(n * n));
    for (int64_t j = 0; j < n; ++j)
        for (int64_t i = 0; i < n; ++i)
            r.zMm[static_cast<size_t>(j * n + i)] =
                static_cast<double>(carrierMm(tiles, vx0 + i * strideVox, vy0 + j * strideVox));
    return r;
}

// D8 neighbour offsets, in a FIXED scan order. Ties in steepest-descent are
// broken by taking the first in this order, so the routing is reproducible.
const int kD8dx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
const int kD8dy[8] = {0, 1, 1, 1, 0, -1, -1, -1};

// PRIORITY-FLOOD + EPSILON (Barnes, Lehman & Mulla 2014, "Priority-Flood").
//
// READ THIS BEFORE COMPARING ANY NUMBER BELOW WITH ANY OTHER NUMBER.
//
// A *plain* depression fill raises every pit to its spill elevation, which
// makes each filled pit a perfectly LEVEL lake — and on a level lake no cell
// has a lower neighbour, so flow routing terminates there. That is not a
// rounding detail; it invalidated a whole set of this project's Phase 2
// drainage figures (plan doc: 341,368 inland dead-ends and 69.2% of land area
// stranded, against 0 and 0 once corrected). The epsilon variant raises each
// newly discovered cell to at least `spill + eps`, so every filled region
// retains a monotone descending path to its outlet and routing continues
// through it.
//
// eps is 1e-4 mm: about seven orders of magnitude above the double ULP at the
// ~1e5 mm elevations involved (so it always survives the addition and the
// comparison), and eleven orders below the 100 mm voxel, so it can never move
// a rendered surface or bias a structure function.
//
// The fill is used ONLY for the network statistics, which are undefined on a
// pitted field. The pit statistics themselves (`raw.*` below) are computed on
// the RAW field, because "how many pits are there" is precisely the question a
// fill destroys the evidence for. Both are reported, always, side by side.
//
// AND HERE IS THE REASON THEY MUST BE READ TOGETHER, measured rather than
// assumed. Priority-flood + epsilon does not merely repair a field, it
// *manufactures* a drainage network on any input at all: it raises every pit
// until the whole domain has a monotone path to an edge, and D8 then traces
// perfectly respectable-looking channels down the ramps the fill just built.
// On the v9 baseline the DETAIL-ONLY field — five octaves of isotropic value
// noise, no landform in it whatsoever — scores an exceedance slope of 0.64
// with R^2 = 0.995, 37 channel components and 1031 junctions/km^2, all of
// which read as "a network" if quoted alone. The same field strands 97.9% of
// its area in 51,704 interior pits per km^2 before the fill touches it.
//
// So: the `net.*` block is descriptive, not diagnostic. The DIAGNOSTIC numbers
// are `raw.interior_sinks_per_km2`, `raw.stranded_area`, `raw.mean_path_len`
// and `fill.mean_depth` — the ones taken before the fill invented anything.
// Anything quoted out of `net.*` without its `raw.*` companion is the same
// class of error as quoting H alone.
constexpr double kFillEpsMm = 1e-4;

struct FillResult {
    std::vector<double> z; // filled heights, mm
    double meanFillMm = 0; // mean rise over the whole domain
    double raisedPct = 0;  // percentage of cells the fill moved
    double maxFillMm = 0;
};

FillResult priorityFloodEps(const Raster& r) {
    const int64_t n = r.n;
    FillResult out;
    out.z = r.zMm;
    std::vector<char> closed(static_cast<size_t>(n * n), 0);
    // (elevation, index): the index makes pop order total, hence deterministic.
    using Node = std::pair<double, int64_t>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> pq;
    for (int64_t j = 0; j < n; ++j)
        for (int64_t i = 0; i < n; ++i) {
            if (i != 0 && j != 0 && i != n - 1 && j != n - 1) continue;
            const int64_t k = j * n + i;
            closed[static_cast<size_t>(k)] = 1;
            pq.push({out.z[static_cast<size_t>(k)], k});
        }
    while (!pq.empty()) {
        const Node c = pq.top();
        pq.pop();
        const int64_t cx = c.second % n, cy = c.second / n;
        for (int d = 0; d < 8; ++d) {
            const int64_t nx = cx + kD8dx[d], ny = cy + kD8dy[d];
            if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
            const int64_t k = ny * n + nx;
            if (closed[static_cast<size_t>(k)]) continue;
            closed[static_cast<size_t>(k)] = 1;
            if (out.z[static_cast<size_t>(k)] <= c.first)
                out.z[static_cast<size_t>(k)] = c.first + kFillEpsMm;
            pq.push({out.z[static_cast<size_t>(k)], k});
        }
    }
    long double acc = 0;
    int64_t raised = 0;
    for (size_t k = 0; k < out.z.size(); ++k) {
        const double d = out.z[k] - r.zMm[k];
        // The epsilon itself is not a fill; only count rises the terrain can see.
        if (d > 1e-3) ++raised;
        if (d > out.maxFillMm) out.maxFillMm = d;
        acc += d;
    }
    out.meanFillMm = static_cast<double>(acc / static_cast<long double>(out.z.size()));
    out.raisedPct = static_cast<double>(raised) * 100.0 / static_cast<double>(out.z.size());
    return out;
}

// D8 routing over an arbitrary height field. Domain-edge cells discharge out
// of the domain (recv = -1, not a sink). An interior cell with no strictly
// lower neighbour is an INTERIOR SINK — the dead-end that noise produces in
// enormous numbers and that a connected network produces none of.
struct FlowField {
    std::vector<int32_t> recv;    // D8 receiver index, -1 = leaves the domain / sink
    std::vector<char> isSink;     // interior dead-end
    std::vector<int64_t> asc;     // cell indices, ascending by (z, index)
    std::vector<double> areaM2;   // upslope contributing area, m^2
    std::vector<double> pathM;    // flow distance to termination, m
    int64_t interiorSinks = 0;
    double strandedPct = 0;       // % of domain area terminating at an interior sink
};

FlowField routeD8(const std::vector<double>& z, int64_t n, double cellM) {
    FlowField f;
    const size_t nn = static_cast<size_t>(n * n);
    f.recv.assign(nn, -1);
    f.isSink.assign(nn, 0);
    f.areaM2.assign(nn, cellM * cellM);
    f.pathM.assign(nn, 0.0);
    const double diag = std::sqrt(2.0);
    for (int64_t j = 0; j < n; ++j)
        for (int64_t i = 0; i < n; ++i) {
            const int64_t k = j * n + i;
            if (i == 0 || j == 0 || i == n - 1 || j == n - 1) continue; // outlet
            double best = 0;
            int32_t bestK = -1;
            for (int d = 0; d < 8; ++d) {
                const int64_t nx = i + kD8dx[d], ny = j + kD8dy[d];
                const int64_t q = ny * n + nx;
                const double dist = (kD8dx[d] != 0 && kD8dy[d] != 0) ? diag : 1.0;
                const double drop = (z[static_cast<size_t>(k)] - z[static_cast<size_t>(q)]) / dist;
                if (drop > best) {
                    best = drop;
                    bestK = static_cast<int32_t>(q);
                }
            }
            if (bestK < 0) {
                f.isSink[static_cast<size_t>(k)] = 1;
                ++f.interiorSinks;
            } else {
                f.recv[static_cast<size_t>(k)] = bestK;
            }
        }

    f.asc.resize(nn);
    for (size_t k = 0; k < nn; ++k) f.asc[k] = static_cast<int64_t>(k);
    std::sort(f.asc.begin(), f.asc.end(), [&](int64_t a, int64_t b) {
        if (z[static_cast<size_t>(a)] != z[static_cast<size_t>(b)])
            return z[static_cast<size_t>(a)] < z[static_cast<size_t>(b)];
        return a < b; // total order -> deterministic
    });

    // Accumulate downstream: descending elevation, so a cell is finished
    // before its receiver is read. Path length runs the other way (ascending),
    // because a receiver is strictly lower and must be resolved first.
    for (size_t idx = nn; idx-- > 0;) {
        const int64_t k = f.asc[idx];
        const int32_t rk = f.recv[static_cast<size_t>(k)];
        if (rk >= 0) f.areaM2[static_cast<size_t>(rk)] += f.areaM2[static_cast<size_t>(k)];
    }
    for (size_t idx = 0; idx < nn; ++idx) {
        const int64_t k = f.asc[idx];
        const int32_t rk = f.recv[static_cast<size_t>(k)];
        if (rk < 0) {
            f.pathM[static_cast<size_t>(k)] = 0;
            continue;
        }
        const int64_t dx = (k % n) - (rk % n), dy = (k / n) - (rk / n);
        const double step = (dx != 0 && dy != 0) ? diag * cellM : cellM;
        f.pathM[static_cast<size_t>(k)] = step + f.pathM[static_cast<size_t>(rk)];
    }

    long double stranded = 0;
    for (size_t k = 0; k < nn; ++k)
        if (f.isSink[k]) stranded += f.areaM2[k];
    const double total = static_cast<double>(nn) * cellM * cellM;
    f.strandedPct = static_cast<double>(stranded) * 100.0 / total;
    return f;
}

// Least-squares fit of y = a + b*x, returning the slope and R^2.
struct LineFit {
    double slope = 0, r2 = 0;
    int64_t nPts = 0;
};
LineFit fitLine(const std::vector<double>& x, const std::vector<double>& y) {
    LineFit f;
    f.nPts = static_cast<int64_t>(x.size());
    if (x.size() < 3) return f;
    double sx = 0, sy = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        sx += x[i];
        sy += y[i];
    }
    const double mx = sx / static_cast<double>(x.size()), my = sy / static_cast<double>(y.size());
    double sxy = 0, sxx = 0, syy = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        sxy += (x[i] - mx) * (y[i] - my);
        sxx += (x[i] - mx) * (x[i] - mx);
        syy += (y[i] - my) * (y[i] - my);
    }
    if (sxx <= 0 || syy <= 0) return f;
    f.slope = sxy / sxx;
    f.r2 = (sxy * sxy) / (sxx * syy);
    return f;
}

// Channel-area thresholds, in SQUARE METRES rather than cells, so the numbers
// stay comparable when the lattice stride changes.
constexpr double kChannelAreaM2 = 250.0;   // a rill head
constexpr double kTrunkAreaM2 = 2500.0;    // a trunk channel

struct DrainStats {
    int64_t latticeN = 0;
    double cellM = 0, domainM = 0;
    // Raw field — the pit census a fill would erase.
    int64_t rawSinks = 0;
    double rawSinksPerKm2 = 0, rawStrandedPct = 0;
    double rawMeanPathM = 0, rawMeanPathNorm = 0;
    double fillMeanMm = 0, fillMaxMm = 0, fillRaisedPct = 0;
    // Epsilon-filled field — the network statistics, undefined on a pitted field.
    int64_t fillSinks = 0; // self-check: must be 0
    double betaSlope = 0, betaR2 = 0;
    int64_t betaPts = 0;
    double maxCatchM2 = 0, maxCatchPct = 0;
    double channelPct = 0, trunkPct = 0;
    double drainDensity = 0, trunkDensity = 0;
    int64_t channelComps = 0;
    double largestCompPct = 0;
    double junctionsPerKm2 = 0;
    double meanPathToChannelM = 0, meanPathToChannelNorm = 0;
    double unreachedChannelPct = 0;
    double meanPathToEdgeNorm = 0;
};

DrainStats drainageStats(const Raster& r) {
    DrainStats s;
    s.latticeN = r.n;
    s.cellM = r.cellM;
    s.domainM = r.domainM();
    const int64_t n = r.n;
    const size_t nn = static_cast<size_t>(n * n);
    const double km2 = r.domainAreaM2() / 1e6;

    // --- RAW ---------------------------------------------------------------
    const FlowField raw = routeD8(r.zMm, n, r.cellM);
    s.rawSinks = raw.interiorSinks;
    s.rawSinksPerKm2 = km2 > 0 ? static_cast<double>(raw.interiorSinks) / km2 : 0;
    s.rawStrandedPct = raw.strandedPct;
    long double acc = 0;
    for (size_t k = 0; k < nn; ++k) acc += raw.pathM[k];
    s.rawMeanPathM = static_cast<double>(acc / static_cast<long double>(nn));
    s.rawMeanPathNorm = s.rawMeanPathM / r.domainM();

    // --- EPSILON-FILLED ----------------------------------------------------
    const FillResult fill = priorityFloodEps(r);
    s.fillMeanMm = fill.meanFillMm;
    s.fillMaxMm = fill.maxFillMm;
    s.fillRaisedPct = fill.raisedPct;
    const FlowField f = routeD8(fill.z, n, r.cellM);
    s.fillSinks = f.interiorSinks;

    // Exceedance probability P(A >= a). A real drainage network has a
    // power-law tail with an exponent near 0.43-0.45 (Rodriguez-Iturbe &
    // Rinaldo); an unorganised field has no straight segment at all, which is
    // why the fit's R^2 is reported next to the slope and neither number means
    // anything alone.
    std::vector<double> areas(f.areaM2.begin(), f.areaM2.end());
    std::sort(areas.begin(), areas.end());
    s.maxCatchM2 = areas.back();
    s.maxCatchPct = s.maxCatchM2 * 100.0 / r.domainAreaM2();
    {
        const double aLo = std::max(4.0 * r.cellAreaM2(), 100.0);
        const double aHi = 0.02 * r.domainAreaM2();
        std::vector<double> lx, ly;
        if (aHi > aLo * 3.0) {
            const int kBins = 24;
            for (int b = 0; b < kBins; ++b) {
                const double a = aLo * std::pow(aHi / aLo,
                                                static_cast<double>(b) / (kBins - 1));
                const size_t ge = areas.size() -
                    static_cast<size_t>(std::lower_bound(areas.begin(), areas.end(), a) -
                                        areas.begin());
                if (ge < 8) break; // tail too thin to fit; stop rather than fit noise
                lx.push_back(std::log10(a));
                ly.push_back(std::log10(static_cast<double>(ge) /
                                        static_cast<double>(areas.size())));
            }
        }
        const LineFit lf = fitLine(lx, ly);
        s.betaSlope = -lf.slope;
        s.betaR2 = lf.r2;
        s.betaPts = lf.nPts;
    }

    // Channel mask, drainage density, network components, junctions.
    std::vector<char> chan(nn, 0);
    int64_t nChan = 0, nTrunk = 0;
    for (size_t k = 0; k < nn; ++k) {
        if (f.areaM2[k] >= kChannelAreaM2) {
            chan[k] = 1;
            ++nChan;
        }
        if (f.areaM2[k] >= kTrunkAreaM2) ++nTrunk;
    }
    s.channelPct = static_cast<double>(nChan) * 100.0 / static_cast<double>(nn);
    s.trunkPct = static_cast<double>(nTrunk) * 100.0 / static_cast<double>(nn);
    // Drainage density: metres of channel per square metre. One channel cell
    // contributes one cell-length of channel.
    s.drainDensity = static_cast<double>(nChan) * r.cellM / r.domainAreaM2();
    s.trunkDensity = static_cast<double>(nTrunk) * r.cellM / r.domainAreaM2();

    // 8-connected components of the channel mask. A dendritic network is a few
    // big trees; a noise field is a scatter of unconnected fragments, and the
    // largest-component share is the cleanest single readout of the difference.
    {
        std::vector<char> seen(nn, 0);
        std::vector<int64_t> stack;
        int64_t largest = 0;
        for (int64_t start = 0; start < static_cast<int64_t>(nn); ++start) {
            if (!chan[static_cast<size_t>(start)] || seen[static_cast<size_t>(start)]) continue;
            ++s.channelComps;
            int64_t size = 0;
            stack.clear();
            stack.push_back(start);
            seen[static_cast<size_t>(start)] = 1;
            while (!stack.empty()) {
                const int64_t c = stack.back();
                stack.pop_back();
                ++size;
                const int64_t cx = c % n, cy = c / n;
                for (int d = 0; d < 8; ++d) {
                    const int64_t nx = cx + kD8dx[d], ny = cy + kD8dy[d];
                    if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
                    const int64_t q = ny * n + nx;
                    if (seen[static_cast<size_t>(q)] || !chan[static_cast<size_t>(q)]) continue;
                    seen[static_cast<size_t>(q)] = 1;
                    stack.push_back(q);
                }
            }
            if (size > largest) largest = size;
        }
        s.largestCompPct = nChan ? static_cast<double>(largest) * 100.0 /
                                       static_cast<double>(nChan)
                                 : 0.0;
    }

    // Junctions: a channel cell fed by two or more channel cells. Branching is
    // what makes a network dendritic rather than a bundle of parallel streaks.
    {
        std::vector<int32_t> donors(nn, 0);
        for (size_t k = 0; k < nn; ++k) {
            const int32_t rk = f.recv[k];
            if (rk >= 0 && chan[k] && chan[static_cast<size_t>(rk)])
                ++donors[static_cast<size_t>(rk)];
        }
        int64_t j = 0;
        for (size_t k = 0; k < nn; ++k)
            if (chan[k] && donors[k] >= 2) ++j;
        s.junctionsPerKm2 = km2 > 0 ? static_cast<double>(j) / km2 : 0;
    }

    // Hillslope length: flow distance from each cell to the first channel cell
    // downstream. This is the geometric dual of drainage density and it is the
    // number that says whether the network actually REACHES the ground: a
    // dense dendritic net leaves every point a short walk from a channel.
    {
        std::vector<double> toChan(nn, -1.0);
        const double diag = std::sqrt(2.0);
        for (size_t idx = 0; idx < nn; ++idx) {
            const int64_t k = f.asc[idx];
            const size_t sk = static_cast<size_t>(k);
            if (chan[sk]) {
                toChan[sk] = 0.0;
                continue;
            }
            const int32_t rk = f.recv[sk];
            if (rk < 0) continue; // leaves the domain without meeting a channel
            const double d = toChan[static_cast<size_t>(rk)];
            if (d < 0) continue;
            const int64_t dx = (k % n) - (rk % n), dy = (k / n) - (rk / n);
            toChan[sk] = ((dx != 0 && dy != 0) ? diag * r.cellM : r.cellM) + d;
        }
        long double sum = 0;
        int64_t have = 0, missing = 0;
        for (size_t k = 0; k < nn; ++k) {
            if (toChan[k] >= 0) {
                sum += toChan[k];
                ++have;
            } else {
                ++missing;
            }
        }
        s.meanPathToChannelM = have ? static_cast<double>(sum / have) : 0.0;
        s.meanPathToChannelNorm = s.meanPathToChannelM / r.domainM();
        s.unreachedChannelPct = static_cast<double>(missing) * 100.0 / static_cast<double>(nn);
    }
    {
        long double sum = 0;
        for (size_t k = 0; k < nn; ++k) sum += f.pathM[k];
        s.meanPathToEdgeNorm =
            static_cast<double>(sum / static_cast<long double>(nn)) / r.domainM();
    }
    return s;
}

void printDrainage(const DrainStats& s, const char* label) {
    std::printf("\n%s — DRAINAGE CONNECTIVITY  (%lldx%lld lattice, %.2f m cells, %.0f m domain, "
                "%lld cells)\n",
                label, (long long)s.latticeN, (long long)s.latticeN, s.cellM, s.domainM,
                (long long)(s.latticeN * s.latticeN));
    std::printf("  RAW FIELD (no fill — the pit census a fill destroys)\n");
    std::printf("    interior sinks           : %lld  (%.0f per km^2)\n", (long long)s.rawSinks,
                s.rawSinksPerKm2);
    std::printf("    area stranded in pits    : %.1f%%   <- 100%% means nothing drains anywhere\n",
                s.rawStrandedPct);
    std::printf("    mean flow path before it terminates: %.2f m  (%.4f of domain edge)\n",
                s.rawMeanPathM, s.rawMeanPathNorm);
    std::printf("    fill needed to route it  : mean %.1f mm, max %.0f mm, %.1f%% of cells "
                "raised\n",
                s.fillMeanMm, s.fillMaxMm, s.fillRaisedPct);
    std::printf("  EPSILON-FILLED (Barnes; filled regions still drain — a PLAIN fill would make\n"
                "                  every pit a level lake and terminate routing there)\n");
    std::printf("    residual interior sinks  : %lld   <- self-check, must be 0\n",
                (long long)s.fillSinks);
    std::printf("    exceedance slope beta    : %.3f  (R^2 %.3f over %lld points; real networks "
                "~0.43-0.45 WITH a straight tail)\n",
                s.betaSlope, s.betaR2, (long long)s.betaPts);
    std::printf("    largest catchment        : %.0f m^2 (%.1f%% of domain)\n", s.maxCatchM2,
                s.maxCatchPct);
    std::printf("    channel cells (A>=%.0f m^2): %.2f%% of area;  drainage density %.5f 1/m\n",
                kChannelAreaM2, s.channelPct, s.drainDensity);
    std::printf("    trunk   cells (A>=%.0f m^2): %.2f%% of area;  trunk density    %.5f 1/m\n",
                kTrunkAreaM2, s.trunkPct, s.trunkDensity);
    std::printf("    channel components       : %lld  (largest holds %.1f%% of channel cells)\n",
                (long long)s.channelComps, s.largestCompPct);
    std::printf("    junctions                : %.0f per km^2   <- branching, i.e. dendritic\n",
                s.junctionsPerKm2);
    std::printf("    mean hillslope path to a channel: %.2f m (%.4f of domain edge); "
                "%.1f%% of cells never meet one\n",
                s.meanPathToChannelM, s.meanPathToChannelNorm, s.unreachedChannelPct);
    std::printf("    mean flow path to domain edge   : %.4f of domain edge\n",
                s.meanPathToEdgeNorm);
    std::printf("  NOTE: the epsilon fill MANUFACTURES a network on any input — it raises pits\n"
                "  until everything drains, and D8 then traces plausible channels down the ramps\n"
                "  the fill just built. Read every EPSILON-FILLED number against the RAW block\n"
                "  above it; quoting one alone is the same error as quoting H alone.\n");
}

// ---------------------------------------------------------------------------
// CURVATURE-CONDITIONED ROUGHNESS
//
// Bin sample points by local surface curvature and report the structure
// function separately per bin. Phase 3 adds a curvature gate meant to roughen
// convex crests ~1.75x and smooth concave hollows to ~0.5x; the ratio between
// the convex and concave bins is the number that says whether it did, and by
// how much. Target after Phase 3: convex/concave ~ 3.5.
//
// Stationary fBm — what v9 lays down today — is conditioned on nothing, so it
// scores exactly 1.0 in this metric no matter what its spectrum is. That is
// the whole point: H cannot see the difference between "crisp crests, soft
// hollows" and "uniformly crumpled", because H averages over both.
//
// THE TRAP, and why the baseline is 50 voxels. The curvature baseline must be
// MUCH larger than the roughness lag. If they overlap, a point is classed as
// curved *because* it is rough, and the metric measures its own binning — a
// guaranteed non-unity ratio out of an isotropic field. Curvature is taken at
// +/-5 m (landform scale, the scale the Phase 3 gate will actually read) and
// the reported lags stop at 1.6 m.
//
// SIGN CONVENTION: kappa = h(x+b) + h(x-b) + h(y+b) + h(y-b) - 4h(x,y).
// POSITIVE is concave-up (a hollow), NEGATIVE is convex (a crest).
//
// Bins are TERCILES of the measured curvature distribution rather than fixed
// thresholds, so each bin always holds a third of the samples and the
// comparison is statistically fair on flat sites and cliff sites alike. The
// bin edges are printed in physical units so a reader can see what "convex"
// meant on this run.
struct CurvBins {
    int64_t nPts = 0;
    int64_t curvBaseVox = 0;
    double edgeLoMmPerM2 = 0, edgeHiMmPerM2 = 0;
    int64_t nBin[3] = {0, 0, 0};
    std::vector<int64_t> lagsVox;
    // s2[bin][lag], metres
    std::vector<double> s2[3];
};

CurvBins curvatureConditionedRoughness(Amplifier& amp, int64_t vx0, int64_t vy0, int64_t n,
                                       int64_t strideVox, int64_t curvBaseVox,
                                       const std::vector<int64_t>& lagsVox) {
    CurvBins cb;
    cb.curvBaseVox = curvBaseVox;
    cb.lagsVox = lagsVox;
    struct Pt {
        int64_t x, y;
        double kappa;
    };
    std::vector<Pt> pts;
    pts.reserve(static_cast<size_t>(n * n));
    const double bM = static_cast<double>(curvBaseVox) * kVoxelSizeMm / 1000.0;
    for (int64_t j = 0; j < n; ++j)
        for (int64_t i = 0; i < n; ++i) {
            const int64_t x = vx0 + i * strideVox, y = vy0 + j * strideVox;
            const double c = static_cast<double>(amp.surfaceMm(x, y));
            const double k = static_cast<double>(amp.surfaceMm(x + curvBaseVox, y)) +
                             static_cast<double>(amp.surfaceMm(x - curvBaseVox, y)) +
                             static_cast<double>(amp.surfaceMm(x, y + curvBaseVox)) +
                             static_cast<double>(amp.surfaceMm(x, y - curvBaseVox)) - 4.0 * c;
            pts.push_back(Pt{x, y, k / (bM * bM)}); // mm per m^2
        }
    cb.nPts = static_cast<int64_t>(pts.size());
    std::vector<double> ks;
    ks.reserve(pts.size());
    for (const Pt& p : pts) ks.push_back(p.kappa);
    std::sort(ks.begin(), ks.end());
    cb.edgeLoMmPerM2 = ks[ks.size() / 3];
    cb.edgeHiMmPerM2 = ks[2 * ks.size() / 3];

    std::vector<long double> acc[3];
    std::vector<int64_t> cnt[3];
    for (int b = 0; b < 3; ++b) {
        acc[b].assign(lagsVox.size(), 0.0L);
        cnt[b].assign(lagsVox.size(), 0);
        cb.s2[b].assign(lagsVox.size(), 0.0);
    }
    for (const Pt& p : pts) {
        // bin 0 = convex (most negative kappa), 1 = planar, 2 = concave.
        const int b = p.kappa < cb.edgeLoMmPerM2 ? 0 : (p.kappa < cb.edgeHiMmPerM2 ? 1 : 2);
        ++cb.nBin[b];
        const double c = static_cast<double>(amp.surfaceMm(p.x, p.y));
        for (size_t li = 0; li < lagsVox.size(); ++li) {
            const int64_t d = lagsVox[li];
            // Both axes, so the estimator itself is direction-neutral.
            const double sx = std::abs(static_cast<double>(amp.surfaceMm(p.x + d, p.y)) +
                                       static_cast<double>(amp.surfaceMm(p.x - d, p.y)) - 2 * c);
            const double sy = std::abs(static_cast<double>(amp.surfaceMm(p.x, p.y + d)) +
                                       static_cast<double>(amp.surfaceMm(p.x, p.y - d)) - 2 * c);
            acc[b][li] += (sx + sy) / 2.0;
            cnt[b][li] += 1;
        }
    }
    for (int b = 0; b < 3; ++b)
        for (size_t li = 0; li < lagsVox.size(); ++li)
            cb.s2[b][li] = cnt[b][li] ? static_cast<double>(acc[b][li] /
                                                            static_cast<long double>(cnt[b][li])) /
                                            1000.0
                                      : 0.0;
    return cb;
}

void printCurvBins(const CurvBins& cb, const char* label) {
    std::printf("\n%s — CURVATURE-CONDITIONED ROUGHNESS  (%lld points, curvature baseline "
                "%.1f m)\n",
                label, (long long)cb.nPts,
                static_cast<double>(cb.curvBaseVox) * kVoxelSizeMm / 1000.0);
    std::printf("  terciles of kappa (mm/m^2, +ve = concave hollow): convex < %.3f <= planar < "
                "%.3f <= concave\n",
                cb.edgeLoMmPerM2, cb.edgeHiMmPerM2);
    std::printf("  bin counts: convex=%lld planar=%lld concave=%lld\n", (long long)cb.nBin[0],
                (long long)cb.nBin[1], (long long)cb.nBin[2]);
    std::printf("  %10s %13s %13s %13s %14s\n", "lag (m)", "convex S2(m)", "planar S2(m)",
                "concave S2(m)", "convex/concave");
    for (size_t li = 0; li < cb.lagsVox.size(); ++li) {
        const double r = cb.s2[2][li] > 0 ? cb.s2[0][li] / cb.s2[2][li] : 0.0;
        std::printf("  %10.2f %13.5f %13.5f %13.5f %14.3f\n",
                    static_cast<double>(cb.lagsVox[li]) * kVoxelSizeMm / 1000.0, cb.s2[0][li],
                    cb.s2[1][li], cb.s2[2][li], r);
    }
    std::printf("  (1.00 = roughness is NOT conditioned on shape, i.e. stationary fBm. Phase 3\n"
                "   target is ~3.5: crests roughened ~1.75x, hollows smoothed to ~0.5x.)\n");
}

// ---------------------------------------------------------------------------
// GRADE-BINNED RILL ANISOTROPY
//
// directionalRoughness() above reports ONE across/along ratio at ONE site.
// The plan's criterion has two halves — "> 1 at 1-3 m lags on >=20% grades"
// AND "~1 on flats" — and a single global number cannot check the second. The
// second half is the one most likely to be violated silently, because a rill
// term whose gate leaks below its grade threshold puts phantom grain on flat
// ground where water never ran, and no aggregate number would show it.
//
// So: sample a lattice, take the LANDFORM gradient at each point (+/-25 m, so
// it is the hillslope and not whatever the microrelief is doing at this
// voxel), bin the point by grade, and accumulate along/across structure
// functions per (bin, lag).
//
// THE CONTROL COLUMN IS NOT DECORATION. The along/across frame is rounded onto
// the voxel lattice, and a biased estimator would manufacture anisotropy out
// of an isotropic field. The control repeats the identical measurement on the
// pair of directions at 45 degrees to the gradient frame, where a downslope
// rill grain cuts both members equally: it must read ~1.00. If the control
// departs from 1.00, the measurement machinery is broken and the signal column
// means nothing.
struct AnisoBins {
    static const int kBins = 6;
    int64_t nPts = 0;
    int64_t gradBaseVox = 0;
    double edges[kBins + 1] = {0, 2, 5, 10, 20, 50, 1e9}; // grade %
    int64_t nBin[kBins] = {0, 0, 0, 0, 0, 0};
    std::vector<int64_t> lagsVox;
    std::vector<double> along[kBins], across[kBins], ctrlA[kBins], ctrlB[kBins];
};

AnisoBins rillAnisotropyByGrade(Amplifier& amp, int64_t vx0, int64_t vy0, int64_t n,
                                int64_t strideVox, int64_t gradBaseVox,
                                const std::vector<int64_t>& lagsVox) {
    AnisoBins ab;
    ab.gradBaseVox = gradBaseVox;
    ab.lagsVox = lagsVox;
    const size_t nl = lagsVox.size();
    std::vector<long double> aAcc[AnisoBins::kBins], cAcc[AnisoBins::kBins],
        p1Acc[AnisoBins::kBins], p2Acc[AnisoBins::kBins];
    std::vector<int64_t> cnt[AnisoBins::kBins];
    for (int b = 0; b < AnisoBins::kBins; ++b) {
        aAcc[b].assign(nl, 0.0L);
        cAcc[b].assign(nl, 0.0L);
        p1Acc[b].assign(nl, 0.0L);
        p2Acc[b].assign(nl, 0.0L);
        cnt[b].assign(nl, 0);
        ab.along[b].assign(nl, 0.0);
        ab.across[b].assign(nl, 0.0);
        ab.ctrlA[b].assign(nl, 0.0);
        ab.ctrlB[b].assign(nl, 0.0);
    }
    const double invSqrt2 = 1.0 / std::sqrt(2.0);
    for (int64_t j = 0; j < n; ++j)
        for (int64_t i = 0; i < n; ++i) {
            const int64_t x = vx0 + i * strideVox, y = vy0 + j * strideVox;
            const double gx = static_cast<double>(amp.surfaceMm(x + gradBaseVox, y) -
                                                  amp.surfaceMm(x - gradBaseVox, y));
            const double gy = static_cast<double>(amp.surfaceMm(x, y + gradBaseVox) -
                                                  amp.surfaceMm(x, y - gradBaseVox));
            const double gl = std::sqrt(gx * gx + gy * gy);
            const double gradePct =
                gl / (2.0 * static_cast<double>(gradBaseVox) * kVoxelSizeMm) * 100.0;
            int b = 0;
            while (b < AnisoBins::kBins - 1 && gradePct >= ab.edges[b + 1]) ++b;
            ++ab.nPts;
            ++ab.nBin[b];
            double ux = 1.0, uy = 0.0;
            if (gl >= 1.0) {
                ux = gx / gl;
                uy = gy / gl;
            }
            const double vx = -uy, vy2 = ux;                              // across-slope
            const double d1x = (ux + vx) * invSqrt2, d1y = (uy + vy2) * invSqrt2; // +45 deg
            const double d2x = (ux - vx) * invSqrt2, d2y = (uy - vy2) * invSqrt2; // -45 deg
            const double c = static_cast<double>(amp.surfaceMm(x, y));
            auto s2dir = [&](double dx, double dy, int64_t d) {
                const int64_t px = static_cast<int64_t>(std::lround(dx * static_cast<double>(d)));
                const int64_t py = static_cast<int64_t>(std::lround(dy * static_cast<double>(d)));
                return std::abs(static_cast<double>(amp.surfaceMm(x + px, y + py)) +
                                static_cast<double>(amp.surfaceMm(x - px, y - py)) - 2 * c);
            };
            for (size_t li = 0; li < nl; ++li) {
                const int64_t d = lagsVox[li];
                aAcc[b][li] += s2dir(ux, uy, d);
                cAcc[b][li] += s2dir(vx, vy2, d);
                p1Acc[b][li] += s2dir(d1x, d1y, d);
                p2Acc[b][li] += s2dir(d2x, d2y, d);
                cnt[b][li] += 1;
            }
        }
    for (int b = 0; b < AnisoBins::kBins; ++b)
        for (size_t li = 0; li < nl; ++li) {
            if (!cnt[b][li]) continue;
            const long double q = static_cast<long double>(cnt[b][li]) * 1000.0L;
            ab.along[b][li] = static_cast<double>(aAcc[b][li] / q);
            ab.across[b][li] = static_cast<double>(cAcc[b][li] / q);
            ab.ctrlA[b][li] = static_cast<double>(p1Acc[b][li] / q);
            ab.ctrlB[b][li] = static_cast<double>(p2Acc[b][li] / q);
        }
    return ab;
}

void printAnisoBins(const AnisoBins& ab, const char* label) {
    std::printf("\n%s — RILL ANISOTROPY BY GRADE  (%lld points, landform gradient at +/-%.1f m)\n",
                label, (long long)ab.nPts,
                static_cast<double>(ab.gradBaseVox) * kVoxelSizeMm / 1000.0);
    std::printf("  %14s %8s %8s %12s %12s %10s %10s\n", "grade band", "n", "lag(m)", "along S2(m)",
                "across S2(m)", "acr/alo", "control");
    for (int b = 0; b < AnisoBins::kBins; ++b) {
        if (!ab.nBin[b]) continue;
        char band[24];
        if (b == AnisoBins::kBins - 1)
            std::snprintf(band, sizeof(band), ">=%.0f%%", ab.edges[b]);
        else
            std::snprintf(band, sizeof(band), "%.0f-%.0f%%", ab.edges[b], ab.edges[b + 1]);
        for (size_t li = 0; li < ab.lagsVox.size(); ++li) {
            const double A = ab.along[b][li], C = ab.across[b][li];
            const double ctrl = ab.ctrlB[b][li] > 0 ? ab.ctrlA[b][li] / ab.ctrlB[b][li] : 0.0;
            std::printf("  %14s %8lld %8.2f %12.5f %12.5f %10.3f %10.3f\n",
                        li == 0 ? band : "", li == 0 ? (long long)ab.nBin[b] : 0LL,
                        static_cast<double>(ab.lagsVox[li]) * kVoxelSizeMm / 1000.0, A, C,
                        A > 0 ? C / A : 0.0, ctrl);
        }
    }
    std::printf("  criterion: acr/alo > 1 at 1-3 m lags on >=20%% grades, ~1.00 on flats.\n");
    std::printf("  control is the same estimator on the 45-degree pair; it must read ~1.00 or\n"
                "  the frame rounding is manufacturing the signal.\n");
}

// ---------------------------------------------------------------------------
// BASELINE TABLE — one fixed-field, machine-greppable block, no timings and no
// timestamps, so `vxc_terrainprobe ... --baseline > before.txt` and the same
// command after a worldgen change diff cleanly.
void row(const char* key, double v, const char* unit) {
    std::printf("%-46s %16.6g  %s\n", key, v, unit);
}
void rowI(const char* key, long long v, const char* unit) {
    std::printf("%-46s %16lld  %s\n", key, v, unit);
}

void printDrainRows(const char* prefix, const DrainStats& s) {
    char k[96];
    auto K = [&](const char* suffix) {
        std::snprintf(k, sizeof(k), "%s.%s", prefix, suffix);
        return k;
    };
    rowI(K("raw.interior_sinks"), (long long)s.rawSinks, "count");
    row(K("raw.interior_sinks_per_km2"), s.rawSinksPerKm2, "1/km2");
    row(K("raw.stranded_area"), s.rawStrandedPct, "pct");
    row(K("raw.mean_path_len"), s.rawMeanPathM, "m");
    row(K("raw.mean_path_len_norm"), s.rawMeanPathNorm, "frac_of_domain_edge");
    row(K("fill.mean_depth"), s.fillMeanMm, "mm");
    row(K("fill.max_depth"), s.fillMaxMm, "mm");
    row(K("fill.cells_raised"), s.fillRaisedPct, "pct");
    rowI(K("fill.residual_interior_sinks"), (long long)s.fillSinks, "count_must_be_0");
    row(K("net.exceedance_slope_beta"), s.betaSlope, "dimensionless");
    row(K("net.exceedance_fit_r2"), s.betaR2, "dimensionless");
    rowI(K("net.exceedance_fit_points"), (long long)s.betaPts, "count");
    row(K("net.max_catchment"), s.maxCatchM2, "m2");
    row(K("net.max_catchment_of_domain"), s.maxCatchPct, "pct");
    row(K("net.channel_cells"), s.channelPct, "pct_of_area");
    row(K("net.trunk_cells"), s.trunkPct, "pct_of_area");
    row(K("net.drainage_density"), s.drainDensity, "1/m");
    row(K("net.trunk_density"), s.trunkDensity, "1/m");
    rowI(K("net.channel_components"), (long long)s.channelComps, "count");
    row(K("net.largest_component"), s.largestCompPct, "pct_of_channel_cells");
    row(K("net.junctions_per_km2"), s.junctionsPerKm2, "1/km2");
    row(K("net.mean_path_to_channel"), s.meanPathToChannelM, "m");
    row(K("net.mean_path_to_channel_norm"), s.meanPathToChannelNorm, "frac_of_domain_edge");
    row(K("net.never_meets_channel"), s.unreachedChannelPct, "pct_of_cells");
    row(K("net.mean_path_to_edge_norm"), s.meanPathToEdgeNorm, "frac_of_domain_edge");
}

} // namespace

int main(int argc, char** argv) {
    // Options are stripped out first so the positional grammar is untouched;
    // every pre-existing command line keeps working byte-for-byte.
    bool optBaseline = false, optStructure = true;
    int64_t drainN = 384, drainStride = 10, fieldN = 96, fieldStride = 25;
    std::vector<char*> pos;
    pos.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        char* a = argv[i];
        // Lattice sizes need at least a few cells to mean anything; a stride of
        // 1 voxel (10 cm, the voxel itself) is a legitimate finest setting and
        // is how the aliasing objection to a 1 m default gets tested.
        auto want = [&](const char* name, int64_t& dst, int64_t lo, int64_t hi) {
            if (std::strcmp(a, name) != 0) return false;
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                std::exit(2);
            }
            dst = std::strtoll(argv[++i], nullptr, 10);
            if (dst < lo || dst > hi) {
                std::fprintf(stderr, "%s %lld out of range (%lld..%lld)\n", name, (long long)dst,
                             (long long)lo, (long long)hi);
                std::exit(2);
            }
            return true;
        };
        if (std::strcmp(a, "--baseline") == 0) {
            optBaseline = true;
        } else if (std::strcmp(a, "--no-structure") == 0) {
            optStructure = false;
        } else if (want("--drain-n", drainN, 8, 8192) ||
                   want("--drain-stride", drainStride, 1, 100000) ||
                   want("--field-n", fieldN, 2, 4096) ||
                   want("--field-stride", fieldStride, 1, 100000)) {
            // consumed
        } else {
            pos.push_back(a);
        }
    }
    argc = static_cast<int>(pos.size());
    argv = pos.data();
    if (optBaseline && !optStructure) {
        std::fprintf(stderr, "--baseline and --no-structure are contradictory\n");
        return 2;
    }

    if (argc < 5) {
        std::fprintf(
            stderr,
            "usage: vxc_terrainprobe <tiledir|--synthetic> <seed> <xM> <yM> [lenM] [pixelMm]\n"
            "       pixelMm is synthetic-only (default 30000; 3750 = scale 8, 1875 = scale 16)\n"
            "       [--baseline] [--drain-n N] [--drain-stride V] [--field-n N]\n"
            "       [--field-stride V] [--no-structure]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const uint64_t seed = std::strtoull(argv[2], nullptr, 10);
    const int64_t x0M = std::strtoll(argv[3], nullptr, 10);
    const int64_t y0M = std::strtoll(argv[4], nullptr, 10);
    const int64_t lenM = argc > 5 ? std::strtoll(argv[5], nullptr, 10) : 200;
    const int64_t pixelArgMm = argc > 6 ? std::strtoll(argv[6], nullptr, 10) : 0;

    if (pixelArgMm != 0 && dir == "--synthetic" && (pixelArgMm < 1 || pixelArgMm > 1000000)) {
        std::fprintf(stderr, "pixelMm %lld is out of range (1..1000000)\n",
                     (long long)pixelArgMm);
        return 2;
    }
    if (pixelArgMm != 0 && dir != "--synthetic") {
        // Refuse rather than ignore: a real tile's pixel size is a property of
        // the file, and a run that silently probed at a size the data does not
        // have would report a corner-grid shape nothing will ever see.
        std::fprintf(stderr,
                     "pixelMm is --synthetic-only; real tiles carry their own pixel size\n");
        return 2;
    }

    SyntheticTileSampler synth(seed, pixelArgMm ? static_cast<int32_t>(pixelArgMm) : 30000);
    TileGridSampler grid(seed, 1);
    ITileSampler* tiles = &synth;
    const char* sourceLabel = "synthetic tiles";

    if (dir != "--synthetic") {
        int loaded = 0, rejected = 0;
        for (auto& e : std::filesystem::directory_iterator(dir)) {
            if (e.path().extension() != ".vxtl") continue;
            if (grid.loadTileFile(e.path()))
                ++loaded;
            else
                ++rejected;
        }
        if (!optBaseline)
            std::printf("tiles loaded=%d rejected=%d pixelSizeMm=%d\n", loaded, rejected,
                        grid.pixelSizeMm());
        if (loaded == 0) {
            std::fprintf(stderr, "no tiles loaded from %s\n", dir.c_str());
            return 1;
        }
        tiles = &grid;
        sourceLabel = "real tiles";
    } else if (!optBaseline) {
        std::printf("using SyntheticTileSampler pixelSizeMm=%d\n", synth.pixelSizeMm());
    }

    Amplifier amp(seed, *tiles);

    const int64_t vx0 = x0M * 1000 / kVoxelSizeMm;
    const int64_t vy0 = y0M * 1000 / kVoxelSizeMm;
    const int64_t n = lenM * 1000 / kVoxelSizeMm;

    if (!optBaseline) {
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
    // Labelled from the sampler, not hardcoded: --synthetic can now run at any
    // pixel size, and a header claiming 30 m over a 1.875 m raster is exactly
    // the kind of mislabelled measurement this tool exists to avoid.
    char rasterLabel[64];
    std::snprintf(rasterLabel, sizeof(rasterLabel), "COARSE RASTER (%lld mm pixels)",
                  (long long)pxMm);
    structureFunction(hTile, plags, rasterLabel, static_cast<double>(pxMm) / 1000.0);
    curvatureFunction(hTile, plags, rasterLabel, static_cast<double>(pxMm) / 1000.0);

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

    materialBoundaryAlignment(amp, vx0, vy0, n, pxMm, "AMPLIFIED SURFACE");

    // 6 levels covers the whole ring cascade the streamer uses.
    boundSweep(amp, vx0, vy0, n, 6, pxMm, sourceLabel);

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
    } // end of the legacy report

    if (!optStructure) return 0;

    // === STRUCTURE METRICS ==================================================
    //
    // THREE FIELDS, and the comparison between them is the measurement.
    //
    //   AMPLIFIED — what the player walks on: carrier + every detail octave.
    //   CARRIER   — the C2 B-spline of the 30 m raster with every octave off.
    //               On REAL tiles this is genuine diffusion-model terrain and
    //               it is the WITHIN-RUN CONTROL that proves the metric can
    //               see a network at all. (On --synthetic it is itself value
    //               noise, so it is a control for the estimator, not for
    //               terrain — say so when quoting a synthetic run.)
    //   DETAIL    — amplified minus carrier: the sub-30 m band ALONE, which is
    //               the band Phase 3 owns and the band this whole plan exists
    //               to fix.
    //
    // The falsifying comparison is CARRIER vs AMPLIFIED. If the detail octaves
    // carried drainage organisation, adding them to a drained carrier would
    // refine its network. If they are isotropic noise, adding them shreds the
    // network into disconnected micro-catchments — which is exactly the Phase 2
    // `--rough -1` failure, and exactly what H scored 0.91 through.
    const Raster rAmp = sampleAmplifiedRaster(amp, vx0, vy0, drainN, drainStride);
    const Raster rCar = sampleCarrierRaster(*tiles, vx0, vy0, drainN, drainStride);
    Raster rDet = rAmp;
    for (size_t k = 0; k < rDet.zMm.size(); ++k) rDet.zMm[k] -= rCar.zMm[k];

    const DrainStats dAmp = drainageStats(rAmp);
    const DrainStats dCar = drainageStats(rCar);
    const DrainStats dDet = drainageStats(rDet);

    // Curvature baseline 50 voxels = 5 m: landform scale, and >3x the largest
    // reported lag, which is what keeps the binning from measuring itself.
    const std::vector<int64_t> curvLags = {1, 2, 4, 8, 16};
    const CurvBins cb =
        curvatureConditionedRoughness(amp, vx0, vy0, fieldN, fieldStride, 50, curvLags);

    // 1, 2 and 3 m: the plan's stated rill band. Gradient baseline 250 voxels
    // (25 m), matching directionalRoughness above so the two are comparable.
    const std::vector<int64_t> rillLags = {10, 20, 30};
    const AnisoBins ab = rillAnisotropyByGrade(amp, vx0, vy0, fieldN, fieldStride, 250, rillLags);

    if (!optBaseline) {
        printDrainage(dCar, "CARRIER ONLY (30 m band; the control)");
        printDrainage(dAmp, "AMPLIFIED SURFACE (what is rendered)");
        printDrainage(dDet, "DETAIL ONLY (amplified - carrier; the sub-30 m band)");
        printCurvBins(cb, "AMPLIFIED SURFACE");
        printAnisoBins(ab, "AMPLIFIED SURFACE");
        std::printf("\n");
    }

    // --- the compact table -------------------------------------------------
    std::printf("=== VXC_TERRAINPROBE STRUCTURE BASELINE v1 ===\n");
    std::printf("%-46s %16s  %s\n", "# field", "value", "unit");
    rowI("config.worldgen_version", (long long)kWorldGenVersion, "int");
    rowI("config.seed", (long long)seed, "int");
    rowI("config.site_x", (long long)x0M, "m");
    rowI("config.site_y", (long long)y0M, "m");
    std::printf("%-46s %16s  %s\n", "config.tile_source",
                dir == "--synthetic" ? "synthetic" : "real", "enum");
    rowI("config.pixel_size", (long long)tiles->pixelSizeMm(), "mm");
    rowI("config.drain_lattice_n", (long long)drainN, "cells_per_axis");
    row("config.drain_cell", static_cast<double>(drainStride) * kVoxelSizeMm / 1000.0, "m");
    row("config.drain_domain", rAmp.domainM(), "m");
    rowI("config.drain_cells", (long long)(drainN * drainN), "count");
    rowI("config.field_lattice_n", (long long)fieldN, "points_per_axis");
    row("config.field_spacing", static_cast<double>(fieldStride) * kVoxelSizeMm / 1000.0, "m");
    rowI("config.field_points", (long long)(fieldN * fieldN), "count");
    row("config.channel_threshold", kChannelAreaM2, "m2");
    row("config.trunk_threshold", kTrunkAreaM2, "m2");
    printDrainRows("drain.carrier", dCar);
    printDrainRows("drain.amplified", dAmp);
    printDrainRows("drain.detail", dDet);

    // THE HEADLINE PAIR. The carrier drains; the question Phase 3 is judged on
    // is what the detail octaves do to that. These two ratios are the single
    // greppable answer, and they are what an isotropic-noise detail band
    // cannot make good however its spectrum is tuned.
    //   sinks_ratio     >> 1  : the detail band is shredding the network
    //   path_len_ratio  << 1  : flow that used to cross the domain now dies
    //                           a few metres from where it started
    // A detail band that carried real drainage structure would leave both
    // near 1 (or improve the path length), because refining a drained surface
    // with more drainage adds channels, it does not add pits.
    row("drain.headline.sinks_amplified_over_carrier",
        dCar.rawSinksPerKm2 > 0 ? dAmp.rawSinksPerKm2 / dCar.rawSinksPerKm2 : -1.0,
        "ratio_-1_if_carrier_has_none");
    row("drain.headline.pathlen_amplified_over_carrier",
        dCar.rawMeanPathM > 0 ? dAmp.rawMeanPathM / dCar.rawMeanPathM : -1.0, "ratio");
    row("drain.headline.stranded_delta", dAmp.rawStrandedPct - dCar.rawStrandedPct,
        "pct_points");

    rowI("curv.points", (long long)cb.nPts, "count");
    row("curv.baseline", static_cast<double>(cb.curvBaseVox) * kVoxelSizeMm / 1000.0, "m");
    row("curv.tercile_lo", cb.edgeLoMmPerM2, "mm/m2");
    row("curv.tercile_hi", cb.edgeHiMmPerM2, "mm/m2");
    for (size_t li = 0; li < cb.lagsVox.size(); ++li) {
        char k[96];
        const double lm = static_cast<double>(cb.lagsVox[li]) * kVoxelSizeMm / 1000.0;
        std::snprintf(k, sizeof(k), "curv.s2_convex.lag_%.1fm", lm);
        row(k, cb.s2[0][li], "m");
        std::snprintf(k, sizeof(k), "curv.s2_planar.lag_%.1fm", lm);
        row(k, cb.s2[1][li], "m");
        std::snprintf(k, sizeof(k), "curv.s2_concave.lag_%.1fm", lm);
        row(k, cb.s2[2][li], "m");
        std::snprintf(k, sizeof(k), "curv.ratio_convex_over_concave.lag_%.1fm", lm);
        row(k, cb.s2[2][li] > 0 ? cb.s2[0][li] / cb.s2[2][li] : 0.0, "dimensionless");
    }

    rowI("rill.points", (long long)ab.nPts, "count");
    row("rill.gradient_baseline", static_cast<double>(ab.gradBaseVox) * kVoxelSizeMm / 1000.0,
        "m");
    for (int b = 0; b < AnisoBins::kBins; ++b) {
        char band[24], k[128];
        if (b == AnisoBins::kBins - 1)
            std::snprintf(band, sizeof(band), "grade_ge%.0f", ab.edges[b]);
        else
            std::snprintf(band, sizeof(band), "grade_%.0f_%.0f", ab.edges[b], ab.edges[b + 1]);
        std::snprintf(k, sizeof(k), "rill.%s.n", band);
        rowI(k, (long long)ab.nBin[b], "count");
        if (!ab.nBin[b]) continue;
        for (size_t li = 0; li < ab.lagsVox.size(); ++li) {
            const double lm = static_cast<double>(ab.lagsVox[li]) * kVoxelSizeMm / 1000.0;
            const double A = ab.along[b][li], C = ab.across[b][li];
            std::snprintf(k, sizeof(k), "rill.%s.across_over_along.lag_%.0fm", band, lm);
            row(k, A > 0 ? C / A : 0.0, "dimensionless");
            std::snprintf(k, sizeof(k), "rill.%s.control_45deg.lag_%.0fm", band, lm);
            row(k, ab.ctrlB[b][li] > 0 ? ab.ctrlA[b][li] / ab.ctrlB[b][li] : 0.0, "dimensionless");
        }
    }
    std::printf("=== END STRUCTURE BASELINE ===\n");
    return 0;
}
