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
// CALIBRATION MODES (see the CALIBRATION HARNESS block near the bottom). Both
// REPLACE the report above rather than adding to it; every pre-existing command
// line keeps producing byte-identical output because neither is ever implied.
//   --band-fit            measure S2(d) on the SOURCE RASTER across the
//                         960 m -> pixel band, fit the local Hurst exponent,
//                         and print the continuation target down to 0.2 m,
//                         labelled measured or EXTRAPOLATED per lag.
//   --calibrate           the above, then SOLVE the `{3200,1600,400,200}` mm
//                         ladder amplitudes against that target and report what
//                         the method does and does not constrain for the other
//                         three Phase 3 terms.
//   --fine-dir DIR        load .vxtl v2 fine tiles from DIR and calibrate
//                         against THEM. This is what turns the 7.5 m band edge
//                         from an extrapolation into a measurement. Refuses to
//                         fall back silently if DIR holds no v2 tiles.
//   --include-ocean       disable the land mask (default ON: a sea-level
//                         stencil contributes S2 = 0 and biases the fit).
//   --cal-n N             source-raster lattice, pixels per axis  (default 384)
//   --kern-n N            octave-kernel lattice points per axis   (default 192)
//   --kern-stride V       octave-kernel stride, in voxels         (default 7)
//   --band-lo M/--band-hi M  fit window in metres    (default one pixel .. 960)
//   --target-h H          force the Hurst exponent instead of using the fit
//   --target-s2 M         force S2 at the 7.5 m band edge, in metres
//   --curv-ratio R        curvature gate convex/concave target    (default 3.5)
//   --rill-aniso R        rill across/along target at 1.6 m       (default 1.30)
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
#include <array>
#include <atomic>
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
// The calibration harness reads the PRODUCTION definitions of everything it is
// calibrating — the carrier's analytic curvature and its gate, the rill term,
// the bedding term, and the noise primitive the octave ladder is built from.
// Re-deriving any of them here would calibrate a lookalike.
#include "voxelcore/carrier.h"
#include "voxelcore/detail_bedding.h"
#include "voxelcore/detail_rill.h"
#include "voxelcore/hash.h"
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
    // v13: PREFILTERED on a sample tier, exactly as amplifier.cpp's cachedStencil
    // is. This probe's whole job is to be the carrier without the octaves, and a
    // copy that skipped the prefilter would report the un-prefiltered carrier's
    // seam and roughness numbers against an amplified surface that no longer has
    // them -- the same class of mislabelled measurement this file's own header
    // warns about.
    int64_t cp[16];
    if (carrierPrefiltersSamples(pxMm)) {
        constexpr int64_t S = kCarrierPrefilterSpan;
        int64_t raw[S * S];
        for (int64_t b = 0; b < S; ++b)
            for (int64_t a = 0; a < S; ++a)
                raw[a + S * b] =
                    tiles.elevationMm(px + kCarrierPrefilterLo + a, py + kCarrierPrefilterLo + b);
        carrierPrefilterStencil(raw, cp);
    } else {
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
                cp[i + 4 * j] = tiles.elevationMm(px - 1 + i, py - 1 + j);
    }

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

// --- PERIODICITY: WAVELENGTH *AND* ORIENTATION ------------------------------
//
// WHY THIS EXISTS. The owner reported "a clear pattern of jump ridges in a
// straight line", spacing "every metre or two". Three terms were then ablated one
// at a time on the strength of hypotheses read off the picture -- the 1.6 m
// axis-aligned microrelief octave, the rill term, and the whole fine tier -- and
// all three came back negative, while a fourth explanation (contour terracing) was
// killed by the terrace-run statistic (0.24 m, an order of magnitude short). Three
// ablations cost more than one measurement would have.
//
// Guessing terms is the wrong move because the artifact's ORIENTATION already
// distinguishes the candidates, and nothing was measuring it:
//
//   * a lattice term (value noise on a world-axis lattice) peaks along world x/y,
//     at its lattice wavelength, REGARDLESS of the local slope direction;
//   * a fall-line-aligned term (the rill/flute frame) peaks perpendicular to the
//     local aspect, so its peak direction ROTATES as the aspect does;
//   * a raster-pitch artifact peaks at exactly the tile pitch -- 1.875 m on the
//     fine tier, 30 m on the coarse one -- and again ignores aspect;
//   * pure voxel quantisation of a smooth ramp has a spacing that scales as
//     1/local-grade, so its wavelength MOVES between windows of different slope
//     while the others stay put.
//
// So this reports, per window: the plane-detrended residual's strongest
// directional autocorrelation peak (wavelength, strength, direction), the two
// cardinal directions explicitly, and the window's own aspect and grade. One run
// on two windows of different slope separates all four.
//
// Floats are used freely: bench/ is outside the float ban, which covers src/ and
// include/ only.
struct PeriodPeak {
    double wavelengthM = 0; // first autocorrelation maximum at nonzero lag
    double strength = 0;    // normalised autocorrelation there, 0..1
    double dirDeg = 0;      // direction of the SCAN, degrees CCW from world +x
    bool found = false;
};

// Bilinear read of a residual field, in cell coordinates. Out-of-range reads are
// rejected by the caller rather than clamped: clamping would fabricate
// correlation at the domain edge, which is exactly the kind of self-inflicted
// periodicity this function exists to detect.
inline bool residualAt(const std::vector<double>& e, int64_t n, double x, double y, double& out) {
    if (x < 0 || y < 0 || x > double(n - 1) || y > double(n - 1)) return false;
    const int64_t i0 = int64_t(x), j0 = int64_t(y);
    const int64_t i1 = std::min<int64_t>(i0 + 1, n - 1), j1 = std::min<int64_t>(j0 + 1, n - 1);
    const double fx = x - double(i0), fy = y - double(j0);
    const double a = e[size_t(j0 * n + i0)], b = e[size_t(j0 * n + i1)];
    const double c = e[size_t(j1 * n + i0)], d = e[size_t(j1 * n + i1)];
    out = (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
    return true;
}

// Directional autocorrelation peak search. `dirDeg` is the scan direction; a
// ridge pattern shows its peak when scanning ACROSS the ridges, so the reported
// direction is perpendicular to the ridge crests.
PeriodPeak periodPeakAlong(const std::vector<double>& e, int64_t n, double cellM, double dirDeg,
                           int64_t maxLag) {
    const double th = dirDeg * 3.14159265358979323846 / 180.0;
    const double ux = std::cos(th), uy = std::sin(th);
    PeriodPeak best;
    // PEARSON CORRELATION OVER THE OVERLAP, and both halves of that matter.
    //
    // The first cut of this divided by sum(v1*v1) and did not re-centre, which is
    // neither of the two things a normalised autocorrelation needs: the
    // denominator must be sqrt(sum(v1^2)*sum(v2^2)) so the Cauchy-Schwarz bound
    // holds, and the means must be taken over the OVERLAPPING set rather than the
    // whole window, because the overlap shrinks with lag and its mean drifts. The
    // consequence was not subtle and is the only reason it was caught: every lag
    // reported r just above 1.0 (1.001, 1.005, 1.006), which is impossible, so the
    // "first local maximum" was picking numerical noise and the wavelengths it
    // printed were meaningless. An instrument that returns r > 1 is announcing its
    // own bug; one that returns a plausible 0.6 would not have.
    std::vector<double> rr(size_t(maxLag) + 1, 0.0);
    rr[0] = 1.0;
    int64_t usable = maxLag;
    for (int64_t L = 1; L <= maxLag; ++L) {
        double s1 = 0, s2 = 0, s11 = 0, s22 = 0, s12 = 0;
        int64_t cnt = 0;
        for (int64_t j = 0; j < n; ++j) {
            for (int64_t i = 0; i < n; ++i) {
                double v2;
                if (!residualAt(e, n, double(i) + ux * double(L), double(j) + uy * double(L), v2))
                    continue;
                const double v1 = e[size_t(j * n + i)];
                s1 += v1;
                s2 += v2;
                s11 += v1 * v1;
                s22 += v2 * v2;
                s12 += v1 * v2;
                ++cnt;
            }
        }
        // Too few overlapping samples to mean anything; stop rather than report a
        // peak computed from a sliver of the window.
        if (cnt < n * n / 8) {
            usable = L - 1;
            break;
        }
        const double inv = 1.0 / double(cnt);
        const double m1 = s1 * inv, m2 = s2 * inv;
        const double var1 = s11 * inv - m1 * m1;
        const double var2 = s22 * inv - m2 * m2;
        const double cov = s12 * inv - m1 * m2;
        if (var1 <= 0 || var2 <= 0) {
            usable = L - 1;
            break;
        }
        rr[size_t(L)] = cov / std::sqrt(var1 * var2);
    }
    for (int64_t L = 2; L + 1 <= usable; ++L) {
        const double a = rr[size_t(L - 1)], b = rr[size_t(L)], c = rr[size_t(L + 1)];
        if (b > a && b >= c && b > 0.15) { // 0.15: below this a "peak" is noise
            best.found = true;
            best.wavelengthM = double(L) * cellM;
            best.strength = b;
            best.dirDeg = dirDeg;
            break; // FIRST peak: the fundamental, not a harmonic
        }
    }
    return best;
}

// HIGH-PASS, NOT A PLANE FIT, and this is the third correction to this instrument.
//
// The first version's normalisation was wrong and returned r > 1. Fixed, it
// reported "no periodic component" -- and the reason was the DETRENDING, not the
// correlation: a least-squares plane over a 25.6 m window on 85%-grade ground
// leaves a detrended residual with an RMS of 1711 mm, i.e. 17 voxels. That
// residual IS the landform. Its autocorrelation decays smoothly from 1.0 and
// buries any centimetre-scale periodic component at r ~ 0.001, so the curve was
// measuring hillslope curvature and calling it "no pattern".
//
// A plane cannot remove curvature; a local mean can. Subtracting a box mean of
// radius R keeps everything shorter than roughly 2R and removes the landform, so
// the residual is the decimetre-to-metre roughness the artifact would live in. R
// is 4 m here: comfortably above the 1-2 m spacing under investigation, so the
// signal survives, and far below the landform scale that was swamping it.
//
// The lesson is the same one this file keeps recording: an instrument that has not
// been sanity-checked against the quantity it is supposed to isolate is not
// evidence. Two of the three failures here were caught only because the output was
// impossible (r > 1) or absurd (a 17-voxel "residual" on ground the eye reads as
// smooth).
std::vector<double> highPassResidual(const Raster& r, double radiusM, double* rmsOut) {
    const int64_t n = r.n;
    const int64_t R = std::max<int64_t>(1, int64_t(radiusM / r.cellM + 0.5));
    // Separable box mean via prefix sums along each axis, clamped at the edges.
    std::vector<double> tmp(size_t(n * n), 0.0), out(size_t(n * n), 0.0);
    for (int64_t j = 0; j < n; ++j) {
        for (int64_t i = 0; i < n; ++i) {
            double s = 0; int64_t c = 0;
            for (int64_t k = std::max<int64_t>(0, i - R); k <= std::min<int64_t>(n - 1, i + R); ++k) {
                s += r.zMm[size_t(j * n + k)]; ++c;
            }
            tmp[size_t(j * n + i)] = s / double(c);
        }
    }
    double rms = 0;
    for (int64_t j = 0; j < n; ++j) {
        for (int64_t i = 0; i < n; ++i) {
            double s = 0; int64_t c = 0;
            for (int64_t k = std::max<int64_t>(0, j - R); k <= std::min<int64_t>(n - 1, j + R); ++k) {
                s += tmp[size_t(k * n + i)]; ++c;
            }
            const double v = r.zMm[size_t(j * n + i)] - s / double(c);
            out[size_t(j * n + i)] = v;
            rms += v * v;
        }
    }
    if (rmsOut) *rmsOut = std::sqrt(rms / double(n * n));
    return out;
}

// Fit and remove the least-squares plane, then scan directions. Centring the
// coordinates makes the normal equations diagonal, so the fit is three divides
// rather than a 3x3 solve.
PeriodPeak periodicityScan(const Raster& r, double* aspectDegOut, double* gradePctOut,
                           PeriodPeak* axisX, PeriodPeak* axisY) {
    const int64_t n = r.n;
    const double mid = double(n - 1) / 2.0;
    double sz = 0, sxx = 0, syy = 0, sxz = 0, syz = 0;
    for (int64_t j = 0; j < n; ++j) {
        for (int64_t i = 0; i < n; ++i) {
            const double x = double(i) - mid, y = double(j) - mid;
            const double z = r.zMm[size_t(j * n + i)];
            sz += z;
            sxx += x * x;
            syy += y * y;
            sxz += x * z;
            syz += y * z;
        }
    }
    const double a = sxx > 0 ? sxz / sxx : 0.0; // mm per cell in +x
    const double b = syy > 0 ? syz / syy : 0.0; // mm per cell in +y
    const double c = sz / double(n * n);
    // The plane is used ONLY for grade and aspect; the field the correlation runs
    // on is high-passed instead. See highPassResidual for why.
    (void)c;
    const std::vector<double> e = highPassResidual(r, 4.0, nullptr);

    // Grade and aspect of the fitted plane. Aspect is the direction of steepest
    // DESCENT, which is the fall line the rill frame would align to.
    const double gxMmPerM = a / r.cellM, gyMmPerM = b / r.cellM;
    if (gradePctOut) *gradePctOut = std::hypot(gxMmPerM, gyMmPerM) / 1000.0 * 100.0;
    if (aspectDegOut) {
        double d = std::atan2(-gyMmPerM, -gxMmPerM) * 180.0 / 3.14159265358979323846;
        if (d < 0) d += 360.0;
        *aspectDegOut = d;
    }

    // SEARCH ONLY LAGS THE HIGH-PASS ACTUALLY PASSES. A box mean of radius R
    // removes everything longer than roughly 2R and, worse, its negative lobe puts
    // a correlation MINIMUM near R followed by a rise -- which a "first local
    // maximum" search happily reports as a peak. That is exactly what happened:
    // the first working version of this returned 4.00 m at 0.1 m cells (R itself),
    // 6.00 m at 0.5 m cells and 28.50 m on the carrier, i.e. numbers that scale
    // with the FILTER rather than sitting at a fixed physical wavelength. A peak
    // whose value tracks the instrument's own parameter is the instrument's, not
    // the terrain's. Capping the search at 3R/4 keeps the search inside the passed
    // band; anything the artifact does at 1.6 m or 1.875 m is comfortably inside it.
    const int64_t R = std::max<int64_t>(1, int64_t(4.0 / r.cellM + 0.5));
    const int64_t maxLag = std::max<int64_t>(4, std::min<int64_t>(n / 4, R * 3 / 4));
    PeriodPeak best;
    for (int k = 0; k < 18; ++k) { // 10-degree steps over 180; the field is symmetric
        const PeriodPeak p = periodPeakAlong(e, n, r.cellM, double(k) * 10.0, maxLag);
        if (p.found && p.strength > best.strength) best = p;
    }
    if (axisX) *axisX = periodPeakAlong(e, n, r.cellM, 0.0, maxLag);
    if (axisY) *axisY = periodPeakAlong(e, n, r.cellM, 90.0, maxLag);
    return best;
}

// The full correlation curve for one direction, so a WEAK peak is visible instead
// of being thresholded away. This exists because the first honest version of this
// instrument reported "no periodic component above r=0.15" on a frame where a human
// had just described a clear repeating ridge pattern. Both can be true: the
// detrended residual is dominated by metre-scale landform variance, so a periodic
// component only a few centimetres tall is a small fraction of it -- and yet at
// 10 cm voxels under a low sun it casts a hard shadow every ridge and the eye locks
// onto it. A pass/fail threshold is the wrong output for that; the curve is the
// right one, and the reader can see whether there is a bump at all.
std::vector<double> correlationCurve(const std::vector<double>& e, int64_t n, double dirDeg,
                                     int64_t maxLag) {
    const double th = dirDeg * 3.14159265358979323846 / 180.0;
    const double ux = std::cos(th), uy = std::sin(th);
    std::vector<double> rr(size_t(maxLag) + 1, std::nan(""));
    rr[0] = 1.0;
    for (int64_t L = 1; L <= maxLag; ++L) {
        double s1 = 0, s2 = 0, s11 = 0, s22 = 0, s12 = 0;
        int64_t cnt = 0;
        for (int64_t j = 0; j < n; ++j) {
            for (int64_t i = 0; i < n; ++i) {
                double v2;
                if (!residualAt(e, n, double(i) + ux * double(L), double(j) + uy * double(L), v2))
                    continue;
                const double v1 = e[size_t(j * n + i)];
                s1 += v1; s2 += v2; s11 += v1 * v1; s22 += v2 * v2; s12 += v1 * v2;
                ++cnt;
            }
        }
        if (cnt < n * n / 8) break;
        const double inv = 1.0 / double(cnt);
        const double m1 = s1 * inv, m2 = s2 * inv;
        const double var1 = s11 * inv - m1 * m1, var2 = s22 * inv - m2 * m2;
        const double cov = s12 * inv - m1 * m2;
        if (var1 <= 0 || var2 <= 0) break;
        rr[size_t(L)] = cov / std::sqrt(var1 * var2);
    }
    return rr;
}

void printCurve(const std::vector<double>& e, int64_t n, double cellM, double dirDeg,
                const char* what, int64_t maxLag) {
    const std::vector<double> rr = correlationCurve(e, n, dirDeg, maxLag);
    std::printf("    %-22s", what);
    // A handful of physically meaningful lags rather than every one: the two
    // finest octaves, the rill spacing, the fine raster pitch, and above.
    const double wantM[] = {0.2, 0.4, 0.8, 1.6, 1.875, 3.2, 6.4};
    for (double w : wantM) {
        const int64_t L = int64_t(w / cellM + 0.5);
        if (L < 1 || L > maxLag || std::isnan(rr[size_t(L)])) {
            std::printf("  %5s", "-");
            continue;
        }
        std::printf("  %+.2f", rr[size_t(L)]);
    }
    std::printf("\n");
}

void reportPeriodicity(const Raster& r, const char* label) {
    double aspectDeg = 0, gradePct = 0;
    PeriodPeak ax, ay;
    const PeriodPeak best = periodicityScan(r, &aspectDeg, &gradePct, &ax, &ay);
    std::printf("\n%s — PERIODICITY of the plane-detrended residual "
                "(%lldx%lld cells at %.2f m, %.1f m window)\n",
                label, (long long)r.n, (long long)r.n, r.cellM, r.domainM());
    std::printf("  window plane: grade %.1f%%  aspect %.0f deg (steepest descent, CCW from +x)\n",
                gradePct, aspectDeg);
    if (best.found) {
        // The scan direction is ACROSS the ridges, so crests run perpendicular.
        double crest = best.dirDeg + 90.0;
        if (crest >= 180.0) crest -= 180.0;
        const double dFromAspect = std::fabs(best.dirDeg - aspectDeg);
        std::printf("  STRONGEST peak: wavelength %.2f m  r=%.3f  scan dir %.0f deg "
                    "(crests run %.0f deg)\n",
                    best.wavelengthM, best.strength, best.dirDeg, crest);
        std::printf("    scan dir vs fall line: %.0f deg apart -- near 0/180 means the pattern is "
                    "ALIGNED WITH THE SLOPE (rill-frame family); near 90 means it runs along the "
                    "contour (quantisation/terracing family)\n",
                    dFromAspect > 180.0 ? 360.0 - dFromAspect : dFromAspect);
    } else {
        std::printf("  STRONGEST peak: none above r=0.15 -- no periodic component at this scale\n");
    }
    if (ax.found)
        std::printf("  world +x: wavelength %.2f m  r=%.3f\n", ax.wavelengthM, ax.strength);
    else
        std::printf("  world +x: no peak\n");
    if (ay.found)
        std::printf("  world +y: wavelength %.2f m  r=%.3f\n", ay.wavelengthM, ay.strength);
    else
        std::printf("  world +y: no peak\n");
    // THE CURVE, not just the verdict. Four directions: the two world axes, and
    // along/across the fall line, which is what separates a lattice term from a
    // rill-frame one.
    {
        const int64_t n = r.n;
        const double mid = double(n - 1) / 2.0;
        double sz = 0, sxx = 0, syy = 0, sxz = 0, syz = 0;
        for (int64_t j = 0; j < n; ++j)
            for (int64_t i = 0; i < n; ++i) {
                const double x = double(i) - mid, y = double(j) - mid;
                const double z = r.zMm[size_t(j * n + i)];
                sz += z; sxx += x * x; syy += y * y; sxz += x * z; syz += y * z;
            }
        (void)sxx; (void)syy; (void)sxz; (void)syz; (void)sz; (void)mid;
        double rms = 0;
        const std::vector<double> e = highPassResidual(r, 4.0, &rms);
        const int64_t maxLag = std::max<int64_t>(4, r.n / 4);
        std::printf("  HIGH-PASSED residual (4 m box mean removed) RMS %.1f mm (%.2f voxels) -- this "
                    "is the band the artifact would live in; a plane fit left 17 voxels of landform "
                    "here and buried it\n",
                    rms, rms / 100.0);
        std::printf("    %-22s  %5s  %5s  %5s  %5s  %5s  %5s  %5s\n", "correlation at lag:",
                    "0.2m", "0.4m", "0.8m", "1.6m", "1.88m", "3.2m", "6.4m");
        printCurve(e, n, r.cellM, 0.0, "world +x", maxLag);
        printCurve(e, n, r.cellM, 90.0, "world +y", maxLag);
        printCurve(e, n, r.cellM, aspectDeg, "along fall line", maxLag);
        printCurve(e, n, r.cellM, aspectDeg + 90.0, "across fall line", maxLag);
    }
    std::printf("  READ IT LIKE THIS: a strong peak on world +x/+y at a fixed wavelength that does "
                "NOT move between windows of different grade is a LATTICE term; a peak that rotates "
                "with aspect is FALL-LINE aligned; exactly 1.875 m or 30.00 m is the RASTER pitch; "
                "a wavelength that scales as 1/grade is voxel QUANTISATION.\n");
}

// --- TERRACE-BOUNDARY CROOKEDNESS ------------------------------------------
//
// WHY THIS EXISTS, and why the previous instrument could not see the artifact it
// is aimed at. The periodicity scan above analyses the CONTINUOUS height field h,
// and found no periodic component at the reported 1-2 m spacing -- correctly. The
// artifact does not live in h. It lives in floor(h / kVoxelSizeMm), the QUANTISED
// level field, because terracing is a THRESHOLD phenomenon: a perfectly smooth
// surface with no periodic content at all still produces hard, long, straight step
// edges once it is quantised. Measuring h's spectrum was measuring amplitude
// structure when the defect is edge structure.
//
// So this measures the terrace boundaries themselves -- the curves where the voxel
// level changes -- and asks how crooked they are.
//
// THE PRIMARY METRIC HAS AN ABSOLUTE BENCHMARK, which is the whole reason to prefer
// it. Box-counting dimension of real topographic contour lines is a published
// quantity: natural contours measure D ~ 1.15-1.25. A smooth analytic surface's
// contours are rectifiable curves with D = 1.0 exactly. So "our contours are
// machined" becomes a number against an external target rather than an opinion, and
// needs no reference DTM -- which matters here because no metre-scale real DTM is
// available on this machine.
//
// CONTROLS ARE COMPUTED EVERY RUN, NOT ASSUMED. This file has now had three
// instrument bugs in one session (a normalisation that returned r > 1, a plane fit
// that left 17 voxels of landform in the "residual", and a peak search that
// reported its own filter radius). So contourStraightness is run on two fields
// whose answers are known before it is run on terrain:
//   * a tilted PLANE, which must give D ~= 1.0 and very long straight runs;
//   * a WHITE-NOISE field, which must give D ~= 2.0 and runs of ~1.
// If those two do not come out, the terrain number is not reported as meaningful.
struct ContourStats {
    double boxDim = 0;          // mean box-counting D over sampled levels
    double meanStraightRun = 0; // mean maximal axis-aligned boundary run, cells
    double p99StraightRun = 0;
    int64_t levelsUsed = 0;
    int64_t boundaryCells = 0;
};

// Maximal axis-aligned runs of boundary cells. A straight machined step edge is a
// long run; a natural contour turns every few cells.
void accumulateRuns(const std::vector<uint8_t>& b, int64_t n, std::vector<int64_t>& runs) {
    for (int64_t j = 0; j < n; ++j) {
        int64_t run = 0;
        for (int64_t i = 0; i < n; ++i) {
            if (b[size_t(j * n + i)]) {
                ++run;
            } else {
                if (run >= 2) runs.push_back(run);
                run = 0;
            }
        }
        if (run >= 2) runs.push_back(run);
    }
    for (int64_t i = 0; i < n; ++i) {
        int64_t run = 0;
        for (int64_t j = 0; j < n; ++j) {
            if (b[size_t(j * n + i)]) {
                ++run;
            } else {
                if (run >= 2) runs.push_back(run);
                run = 0;
            }
        }
        if (run >= 2) runs.push_back(run);
    }
}

ContourStats contourStraightness(const std::vector<int64_t>& level, int64_t n) {
    ContourStats out;
    int64_t lo = level[0], hi = level[0];
    for (int64_t v : level) {
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    if (hi <= lo) return out; // dead flat: no terraces to measure

    // Sample up to 12 levels spread through the range. Each level's boundary is
    // box-counted SEPARATELY: the union of all boundaries fills the plane on steep
    // ground (every column changes level), which would drive D to 2 for a reason
    // that has nothing to do with crookedness.
    const int64_t want = 12;
    const int64_t span = hi - lo;
    std::vector<double> dims;
    std::vector<int64_t> runs;
    std::vector<uint8_t> b(size_t(n * n), 0);
    for (int64_t t = 1; t <= want; ++t) {
        const int64_t k = lo + span * t / (want + 1);
        int64_t cells = 0;
        for (int64_t j = 0; j < n; ++j) {
            for (int64_t i = 0; i < n; ++i) {
                const bool inside = level[size_t(j * n + i)] >= k;
                bool edge = false;
                if (i + 1 < n && (level[size_t(j * n + i + 1)] >= k) != inside) edge = true;
                if (j + 1 < n && (level[size_t((j + 1) * n + i)] >= k) != inside) edge = true;
                b[size_t(j * n + i)] = edge ? 1u : 0u;
                if (edge) ++cells;
            }
        }
        // Too sparse to fit a slope over; skip rather than fit noise.
        if (cells < 64) continue;
        out.boundaryCells += cells;
        accumulateRuns(b, n, runs);

        // Box counting over dyadic sizes. Fit ln N vs ln s by least squares; D is
        // the negative slope.
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int pts = 0;
        for (int64_t s = 1; s <= 32; s *= 2) {
            int64_t occupied = 0;
            for (int64_t by = 0; by < n; by += s) {
                for (int64_t bx = 0; bx < n; bx += s) {
                    bool any = false;
                    for (int64_t j = by; j < by + s && j < n && !any; ++j)
                        for (int64_t i = bx; i < bx + s && i < n; ++i)
                            if (b[size_t(j * n + i)]) {
                                any = true;
                                break;
                            }
                    if (any) ++occupied;
                }
            }
            if (occupied <= 0) continue;
            const double X = std::log(double(s)), Y = std::log(double(occupied));
            sx += X; sy += Y; sxx += X * X; sxy += X * Y;
            ++pts;
        }
        if (pts < 3) continue;
        const double den = double(pts) * sxx - sx * sx;
        if (den == 0) continue;
        const double slope = (double(pts) * sxy - sx * sy) / den;
        dims.push_back(-slope);
        ++out.levelsUsed;
    }
    if (!dims.empty()) {
        double s = 0;
        for (double d : dims) s += d;
        out.boxDim = s / double(dims.size());
    }
    if (!runs.empty()) {
        std::sort(runs.begin(), runs.end());
        double s = 0;
        for (int64_t r : runs) s += double(r);
        out.meanStraightRun = s / double(runs.size());
        out.p99StraightRun = double(runs[size_t(double(runs.size()) * 0.99)]);
    }
    return out;
}

// Quantise a height raster to voxel levels.
std::vector<int64_t> levelsFromHeights(const std::vector<double>& zMm, int64_t n) {
    std::vector<int64_t> L(size_t(n * n), 0);
    for (size_t i = 0; i < L.size(); ++i)
        L[i] = int64_t(std::floor(zMm[i] / double(kVoxelSizeMm)));
    return L;
}

// --- RIB CONTINUITY ---------------------------------------------------------
//
// The feature the owner actually sees, pinned by a top-down data map and an
// in-game raking capture of the same 51 m patch: on a steep smooth face the
// surface crosses a voxel level every ~25 cm, so it is tiled by ribs of constant
// level. That spacing is pure geometry and cannot be removed. What makes them read
// as MACHINED rather than as rock is that each rib runs unbroken for tens of
// metres, because there is no roughness at the 10-50 cm scale to interrupt it.
//
// So the target is not the ribs' existence, it is their LENGTH. This measures a
// connected component of constant voxel level and reports how far it extends along
// its longest axis. A rib crossing the whole 38 m window is 384 cells; one broken
// every couple of metres is ~20.
//
// Reported as p50/p90/p99/max of component extent, area-weighted for the p-values
// so the answer is "how long is the rib under a randomly chosen voxel", which is
// what the eye samples -- not "how long is the average component", which is
// dominated by the thousands of tiny ones.
struct RibStats {
    double p50 = 0, p90 = 0, p99 = 0;
    int64_t maxExtent = 0, components = 0;
};

RibStats ribContinuity(const std::vector<int64_t>& level, int64_t n) {
    RibStats out;
    std::vector<int32_t> comp(size_t(n * n), -1);
    std::vector<int64_t> stack;
    std::vector<std::pair<int64_t, int64_t>> extents; // (extent, area)
    int32_t next = 0;
    for (int64_t j0 = 0; j0 < n; ++j0) {
        for (int64_t i0 = 0; i0 < n; ++i0) {
            if (comp[size_t(j0 * n + i0)] >= 0) continue;
            const int64_t lv = level[size_t(j0 * n + i0)];
            const int32_t id = next++;
            int64_t xmin = i0, xmax = i0, ymin = j0, ymax = j0, area = 0;
            stack.clear();
            stack.push_back(j0 * n + i0);
            comp[size_t(j0 * n + i0)] = id;
            while (!stack.empty()) {
                const int64_t p = stack.back();
                stack.pop_back();
                const int64_t i = p % n, j = p / n;
                ++area;
                if (i < xmin) xmin = i;
                if (i > xmax) xmax = i;
                if (j < ymin) ymin = j;
                if (j > ymax) ymax = j;
                const int64_t di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) {
                    const int64_t ni = i + di[k], nj = j + dj[k];
                    if (ni < 0 || nj < 0 || ni >= n || nj >= n) continue;
                    const size_t q = size_t(nj * n + ni);
                    if (comp[q] >= 0 || level[q] != lv) continue;
                    comp[q] = id;
                    stack.push_back(int64_t(q));
                }
            }
            const int64_t ext = std::max(xmax - xmin, ymax - ymin) + 1;
            extents.push_back({ext, area});
        }
    }
    out.components = int64_t(extents.size());
    if (extents.empty()) return out;
    std::sort(extents.begin(), extents.end());
    int64_t total = 0;
    for (auto& e : extents) total += e.second;
    // Area-weighted quantiles: walk the sorted extents accumulating AREA.
    int64_t acc = 0;
    double q50 = -1, q90 = -1, q99 = -1;
    for (auto& e : extents) {
        acc += e.second;
        const double frac = double(acc) / double(total);
        if (q50 < 0 && frac >= 0.50) q50 = double(e.first);
        if (q90 < 0 && frac >= 0.90) q90 = double(e.first);
        if (q99 < 0 && frac >= 0.99) q99 = double(e.first);
    }
    out.p50 = q50; out.p90 = q90; out.p99 = q99;
    out.maxExtent = extents.back().first;
    return out;
}

void reportContourStraightness(const Raster& r, const char* label) {
    // --- CONTROLS FIRST. See the header: this instrument has earned distrust.
    const int64_t n = r.n;
    std::vector<double> planeZ(size_t(n * n)), noiseZ(size_t(n * n));
    // A tilted plane at ~4% grade, the grade band where the artifact reads worst.
    // Voxel steps every 2.5 cells, so terraces exist and are perfectly straight.
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    for (int64_t j = 0; j < n; ++j)
        for (int64_t i = 0; i < n; ++i) {
            planeZ[size_t(j * n + i)] = 0.04 * double(i) * double(kVoxelSizeMm);
            rng = rng * 6364136223846793005ull + 1442695040888963407ull;
            noiseZ[size_t(j * n + i)] =
                double((rng >> 33) % 1000u) * double(kVoxelSizeMm) / 100.0;
        }
    const ContourStats ctlPlane = contourStraightness(levelsFromHeights(planeZ, n), n);
    const ContourStats ctlNoise = contourStraightness(levelsFromHeights(noiseZ, n), n);
    const bool controlsOk = ctlPlane.boxDim > 0.90 && ctlPlane.boxDim < 1.15 &&
                            ctlNoise.boxDim > 1.70 && ctlNoise.boxDim < 2.10;

    const ContourStats st = contourStraightness(levelsFromHeights(r.zMm, n), n);
    std::printf("\n%s — TERRACE-BOUNDARY CROOKEDNESS (voxel level field, %lldx%lld at %.2f m)\n",
                label, (long long)n, (long long)n, r.cellM);
    std::printf("  CONTROLS   tilted plane D=%.3f (want ~1.0, straight runs %.1f cells)\n",
                ctlPlane.boxDim, ctlPlane.meanStraightRun);
    std::printf("             white noise  D=%.3f (want ~2.0, straight runs %.1f cells)\n",
                ctlNoise.boxDim, ctlNoise.meanStraightRun);
    if (!controlsOk) {
        std::printf("  *** CONTROLS FAILED -- the instrument is wrong, and the terrain number below "
                    "is NOT evidence. Fix the metric before reading it. ***\n");
    }
    std::printf("  TERRAIN    D=%.3f over %lld levels, %lld boundary cells\n", st.boxDim,
                (long long)st.levelsUsed, (long long)st.boundaryCells);
    std::printf("             straight boundary runs: mean %.2f cells (%.2f m), p99 %.0f cells "
                "(%.2f m)\n",
                st.meanStraightRun, st.meanStraightRun * r.cellM, st.p99StraightRun,
                st.p99StraightRun * r.cellM);
    {
        const RibStats rb = ribContinuity(levelsFromHeights(r.zMm, n), n);
        std::printf("  RIB CONTINUITY (constant-level runs, area-weighted): p50 %.0f cells "
                    "(%.1f m), p90 %.0f (%.1f m), p99 %.0f (%.1f m), max %lld (%.1f m), "
                    "%lld components\n",
                    rb.p50, rb.p50 * r.cellM, rb.p90, rb.p90 * r.cellM, rb.p99,
                    rb.p99 * r.cellM, (long long)rb.maxExtent, double(rb.maxExtent) * r.cellM,
                    (long long)rb.components);
        std::printf("    This is the length of the rib a randomly chosen voxel sits on. Rib "
                    "SPACING is geometry and cannot be removed; rib LENGTH is what reads as "
                    "machined. A rib spanning the whole window is %lld cells.\n",
                    (long long)n);
    }
    std::printf("  HOW TO READ IT: real topographic contours box-count at D ~ 1.15-1.25; a smooth "
                "analytic surface's contours are rectifiable, D = 1.0 exactly. D near 1.0 with long "
                "straight runs is the machined-terrace signature. This is an ABSOLUTE benchmark, "
                "not a calibration against a reference DTM -- none is available on this box.\n");
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
// ---------------------------------------------------------------------------
// THE CARRIER'S ANALYTIC CURVATURE — the quantity the GATE actually reads.
//
// Calls carrier.h's PRODUCTION evalCarrierCurvature rather than re-deriving it
// the way evalProbeCarrier re-derives the value path. That asymmetry is
// deliberate: the value path is duplicated so the carrier can be measured with
// every octave silenced, but a gate calibration is meaningless unless the
// quantity measured is bit-identical to the quantity the gate will read.
int64_t probeCurvatureQ10(ITileSampler& tiles, int64_t vx, int64_t vy) {
    const int64_t xMm = vx * kVoxelSizeMm, yMm = vy * kVoxelSizeMm;
    const int64_t pxMm = tiles.pixelSizeMm();
    const int64_t px = floorDiv(xMm, pxMm), py = floorDiv(yMm, pxMm);
    // v13: prefiltered, for the reason stated on this function -- a gate
    // calibration is meaningless unless the quantity measured is bit-identical
    // to the quantity the gate reads, and the gate reads the PREFILTERED
    // stencil's curvature.
    int64_t cp[16];
    if (carrierPrefiltersSamples(pxMm)) {
        constexpr int64_t S = kCarrierPrefilterSpan;
        int64_t raw[S * S];
        for (int64_t b = 0; b < S; ++b)
            for (int64_t a = 0; a < S; ++a)
                raw[a + S * b] =
                    tiles.elevationMm(px + kCarrierPrefilterLo + a, py + kCarrierPrefilterLo + b);
        carrierPrefilterStencil(raw, cp);
    } else {
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i) cp[i + 4 * j] = tiles.elevationMm(px - 1 + i, py - 1 + j);
    }
    const CarrierCurvature c =
        evalCarrierCurvature(cp, xMm - px * pxMm, yMm - py * pxMm, pxMm);
    return carrierCurvatureMmPerM2Q10(c, pxMm);
}

// Exactly the argument curvatureScaleQ10 receives in evalSurface: the carrier's
// analytic Laplacian, normalised to the 30 m reference tier. Both steps come
// from carrier.h, so there is no second implementation of either to drift.
int64_t probeGateInputQ10(ITileSampler& tiles, int64_t vx, int64_t vy) {
    const int64_t pxMm = tiles.pixelSizeMm();
    return carrierCurvatureTierNormQ10(probeCurvatureQ10(tiles, vx, vy), pxMm);
}

// The microrelief band's gain, mirroring evalSurface's `1024 + (cScale-1024)/2`:
// half the shaping band's excursion, on the argument that a hollow's METRE-scale
// shape smooths as colluvium fills it while its clods and stones do not vanish.
int64_t probeMicroGateQ10(int64_t cScaleQ10) { return 1024 + (cScaleQ10 - 1024) / 2; }

struct CurvBins {
    int64_t nPts = 0;
    int64_t curvBaseVox = 0;
    double edgeLoMmPerM2 = 0, edgeHiMmPerM2 = 0;
    int64_t nBin[3] = {0, 0, 0};
    std::vector<int64_t> lagsVox;
    // s2[bin][lag], metres
    std::vector<double> s2[3];
    // Which quantity the bins were cut on. See the block below.
    bool byCarrier = false;
};

// `carrierTiles` selects WHAT THE BINS ARE CUT ON, and the choice is the whole
// measurement:
//
//   nullptr  — the v9 behaviour, retained byte-for-byte: a finite difference of
//              the AMPLIFIED surface over `curvBaseVox`. This asks "is roughness
//              conditioned on the shape of the rendered ground".
//
//   non-null — the CARRIER's analytic Laplacian, which is literally the argument
//              curvatureScaleQ10 receives in evalSurface.
//
// THEY ARE NOT THE SAME QUESTION AND ON REAL DATA THEY DISAGREE BY TWO ORDERS OF
// MAGNITUDE. A 5 m finite difference of the amplified surface has terciles
// around +/-25 mm/m^2; the carrier's analytic Laplacian on a 30 m raster has a
// mean magnitude near 0.3 mm/m^2. So the amplified-surface bins are sorting
// almost entirely by DETAIL NOISE, with the carrier's shape a rounding error
// inside them — and worse, they are partly self-referential, because a point
// lands in the "convex" bin largely BECAUSE it is rough there, which is the
// thing being measured. A gate keyed to the carrier can be working perfectly and
// still score 1.00 under the amplified-surface binning.
//
// Both are reported. The disagreement between them is the finding, not a bug in
// either: the amplified-surface version answers "does the RENDERED ground read as
// conditioned", the carrier version answers "is the GATE doing anything".
CurvBins curvatureConditionedRoughness(Amplifier& amp, ITileSampler* carrierTiles, int64_t vx0,
                                       int64_t vy0, int64_t n, int64_t strideVox,
                                       int64_t curvBaseVox,
                                       const std::vector<int64_t>& lagsVox) {
    CurvBins cb;
    cb.curvBaseVox = curvBaseVox;
    cb.lagsVox = lagsVox;
    cb.byCarrier = carrierTiles != nullptr;
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
            if (carrierTiles) {
                // SIGN: carrier.h's Laplacian and the finite difference below
                // AGREE — both are negative on a crest, positive in a hollow.
                // (carrier.h's "opposite convention" note is about the
                // geomorphological PROFILE curvature, not about this estimator.)
                // So no flip: bin 0 is convex in both paths. An earlier cut here
                // negated, on a misreading of that note, and reported the gate
                // working exactly backwards — which looked like a real and
                // alarming result rather than a sign error, so it is called out
                // rather than quietly corrected.
                const double kq = static_cast<double>(probeGateInputQ10(*carrierTiles, x, y));
                pts.push_back(Pt{x, y, kq / static_cast<double>(kCurveQ10One)});
                continue;
            }
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
    if (cb.byCarrier)
        std::printf("\n%s — CURVATURE-CONDITIONED ROUGHNESS, BINNED ON THE CARRIER'S ANALYTIC\n"
                    "  LAPLACIAN (%lld points) — the quantity curvatureScaleQ10 actually reads\n",
                    label, (long long)cb.nPts);
    else
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
    if (cb.byCarrier)
        std::printf("  NB an S2 RATIO CANNOT SEE A GATE ON A COARSE BAND. See the gate census\n"
                    "  below for the direct question, and the band-share table for which lags\n"
                    "  the gated band even reaches.\n");
}

// ---------------------------------------------------------------------------
// GATE CENSUS — the direct question, which no S2 ratio answers cleanly.
//
// "Is the curvature gate doing anything, and how much?" is a question about the
// DISTRIBUTION OF curvatureScaleQ10 over real terrain, and it can be answered by
// evaluating that function at every sample point and reporting its quantiles. If
// the gate sits inside [0.95, 1.05] on real data it is inert regardless of what
// its clamps say; if it spans [0.5, 1.75] it is working and any metric reporting
// 1.00 is blind rather than right.
//
// This is a strictly better instrument than the conditioned-S2 ratio for the
// question "is the gate live", because it has no estimator, no binning, and no
// lag: it is the gain field itself.
struct GateCensus {
    int64_t nPts = 0;
    int64_t pxMm = 0;
    double kneeMmPerM2 = 0;
    // Carrier curvature, in mm/m^2 (the gate's own currency, sign as carrier.h
    // defines it: NEGATIVE on crests).
    double curveMeanAbs = 0, curveP05 = 0, curveP50 = 0, curveP95 = 0;
    double curveTierNormMeanAbs = 0; // normalised to the 30 m reference tier
    double satPct = 0;               // % of points at or past the knee (gate clamped)
    // The gain itself: the SHAPING band's, and the microrelief band's half of it.
    double gMin = 0, gP10 = 0, gP50 = 0, gP90 = 0, gMax = 0, gMean = 0, gRms = 0;
    double mMin = 0, mMax = 0, mRms = 0;
    double inertPct = 0; // % of points with the shaping gain inside [0.95, 1.05]
};

GateCensus gateCensus(ITileSampler& tiles, int64_t vx0, int64_t vy0, int64_t n,
                      int64_t strideVox) {
    GateCensus g;
    g.pxMm = tiles.pixelSizeMm();
    g.kneeMmPerM2 = static_cast<double>(kCurvatureKneeQ10) / static_cast<double>(kCurveQ10One);
    std::vector<double> curv, gain, mgain;
    curv.reserve(static_cast<size_t>(n * n));
    gain.reserve(static_cast<size_t>(n * n));
    mgain.reserve(static_cast<size_t>(n * n));
    long double absAcc = 0, normAbsAcc = 0, gAcc = 0, gSq = 0, mSq = 0;
    int64_t sat = 0, inert = 0;
    for (int64_t j = 0; j < n; ++j)
        for (int64_t i = 0; i < n; ++i) {
            const int64_t x = vx0 + i * strideVox, y = vy0 + j * strideVox;
            const int64_t raw = probeCurvatureQ10(tiles, x, y);
            // The gate's ACTUAL argument. Reporting the raw Laplacian alongside
            // matters because the two differ by 256x on the fine tier, and a
            // knee quoted against the wrong one is the exact bug that made this
            // gate inert in the first place.
            const int64_t q = carrierCurvatureTierNormQ10(raw, g.pxMm);
            const double gg = static_cast<double>(curvatureScaleQ10(q)) / 1024.0;
            const double mm = static_cast<double>(probeMicroGateQ10(curvatureScaleQ10(q))) / 1024.0;
            curv.push_back(static_cast<double>(q) / static_cast<double>(kCurveQ10One));
            gain.push_back(gg);
            mgain.push_back(mm);
            absAcc += std::abs(static_cast<double>(raw)) / static_cast<double>(kCurveQ10One);
            normAbsAcc += std::abs(static_cast<double>(q)) / static_cast<double>(kCurveQ10One);
            gAcc += gg;
            gSq += static_cast<long double>(gg) * gg;
            mSq += static_cast<long double>(mm) * mm;
            if (q <= -kCurvatureKneeQ10 || q >= kCurvatureKneeQ10) ++sat;
            if (gg >= 0.95 && gg <= 1.05) ++inert;
        }
    g.nPts = static_cast<int64_t>(curv.size());
    if (!g.nPts) return g;
    const long double N = static_cast<long double>(g.nPts);
    g.curveMeanAbs = static_cast<double>(absAcc / N);
    g.curveTierNormMeanAbs = static_cast<double>(normAbsAcc / N);
    g.gMean = static_cast<double>(gAcc / N);
    g.gRms = std::sqrt(static_cast<double>(gSq / N));
    g.satPct = static_cast<double>(sat) * 100.0 / static_cast<double>(g.nPts);
    g.inertPct = static_cast<double>(inert) * 100.0 / static_cast<double>(g.nPts);
    g.mRms = std::sqrt(static_cast<double>(mSq / N));
    std::sort(curv.begin(), curv.end());
    std::sort(gain.begin(), gain.end());
    std::sort(mgain.begin(), mgain.end());
    auto q = [](const std::vector<double>& v, double p) {
        return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))];
    };
    g.curveP05 = q(curv, 0.05);
    g.curveP50 = q(curv, 0.50);
    g.curveP95 = q(curv, 0.95);
    g.gMin = gain.front();
    g.gP10 = q(gain, 0.10);
    g.gP50 = q(gain, 0.50);
    g.gP90 = q(gain, 0.90);
    g.gMax = gain.back();
    g.mMin = mgain.front();
    g.mMax = mgain.back();
    return g;
}

void printGateCensus(const GateCensus& g) {
    std::printf("\nCURVATURE GATE CENSUS — curvatureScaleQ10 evaluated at %lld points\n",
                (long long)g.nPts);
    std::printf("  carrier Laplacian (mm/m^2, NEGATIVE on crests, carrier.h's own sign):\n");
    std::printf("    RAW at this tier's %lld mm pixel: mean|kappa| = %.4f\n", (long long)g.pxMm,
                g.curveMeanAbs);
    std::printf("    TIER-NORMALISED (the gate's actual argument): mean|kappa| = %.4f\n",
                g.curveTierNormMeanAbs);
    std::printf("    quantiles of the gate's argument: p05 = %.4f  median = %.4f  p95 = %.4f\n",
                g.curveP05, g.curveP50, g.curveP95);
    std::printf("    gate knee = %.4f mm/m^2  ->  knee / mean|kappa| = %.2f, and %.1f%% of\n"
                "    points are AT the clamp\n",
                g.kneeMmPerM2,
                g.curveTierNormMeanAbs > 0 ? g.kneeMmPerM2 / g.curveTierNormMeanAbs : 0.0,
                g.satPct);
    std::printf("  the SHAPING band's gain (1.000 = no effect):\n");
    std::printf("    min = %.3f   p10 = %.3f   median = %.3f   p90 = %.3f   max = %.3f\n", g.gMin,
                g.gP10, g.gP50, g.gP90, g.gMax);
    std::printf("    mean = %.4f   rms = %.4f   inside [0.95, 1.05]: %.1f%% of points\n", g.gMean,
                g.gRms, g.inertPct);
    std::printf("  the MICRORELIEF band's gain (half the excursion): min = %.3f  max = %.3f  "
                "rms = %.4f\n",
                g.mMin, g.mMax, g.mRms);
    if (g.inertPct >= 80.0)
        std::printf("    <-- GATE IS EFFECTIVELY INERT: %.0f%% of the world sees a gain within\n"
                    "        5%% of 1.0. Whatever the clamps say, this is not conditioning\n"
                    "        anything. The knee is %.1fx the mean curvature it is asked to\n"
                    "        respond to.\n",
                    g.inertPct,
                    g.curveTierNormMeanAbs > 0 ? g.kneeMmPerM2 / g.curveTierNormMeanAbs : 0.0);
    else if (g.gP90 / (g.gP10 > 0 ? g.gP10 : 1.0) >= 1.5)
        std::printf("    <-- GATE IS LIVE: the p10..p90 span is %.2fx, so the shaping band's\n"
                    "        amplitude genuinely varies with landform shape.\n",
                    g.gP90 / (g.gP10 > 0 ? g.gP10 : 1.0));
    else
        std::printf("    <-- GATE IS WEAK BUT NOT DEAD: p10..p90 spans only %.2fx.\n",
                    g.gP90 / (g.gP10 > 0 ? g.gP10 : 1.0));
}

// ---------------------------------------------------------------------------
// BAND SHARE — WHICH LAGS CAN CARRY THE GATE'S SIGNAL AT ALL.
//
// The curvature gate multiplies the SHAPING band only. A value-noise octave of
// lattice L has a second difference that falls off as (d/L)^2 for d << L, so a
// 25.6 m octave contributes about (0.1/25.6)^2 ~ 1.5e-5 of its amplitude to
// S2(0.1 m). Gating that band by 1.75x changes S2 at a 0.1 m lag by a part in
// ten thousand — invisible to any estimator, however strong the gate.
//
// So "the conditioned-S2 ratio is 1.00 at the fine lags" is not evidence that
// the gate is off; at those lags it is arithmetically guaranteed whether the
// gate is off or not. This table says, per lag, what fraction of the total
// detail ENERGY the gated band supplies. Read it before reading any conditioned
// ratio: a ratio at a lag whose gated share is 1% can move by at most 1%.
//
// MIRROR WARNING. The two tables below are copies of amplifier.cpp's
// kDetailOctaves / kFineDetailOctaves and their band splits. They are copied for
// the same reason evalProbeCarrier copies the carrier: the probe must be able to
// weigh the bands SEPARATELY and the amplifier offers no such handle. If the
// amplifier's tables change, these must change with them — that is the intended
// coupling, and this comment is the tripwire.
struct ProbeOctave {
    int64_t latticeMm;
    int64_t amplitudeMm;
};
constexpr ProbeOctave kProbeCoarseOctaves[] = {
    {25600, 2600}, {6400, 1100}, {1600, 500}, {400, 190}, {200, 60},
};
constexpr int kProbeCoarseGated = 2; // kLandformOctaveCount
constexpr ProbeOctave kProbeFineOctaves[] = {
    {3200, 900}, {1600, 500}, {400, 190}, {200, 60},
};
constexpr int kProbeFineGated = 1; // kFineLandformOctaveCount

struct BandShare {
    bool fine = false;
    int nGated = 0;
    std::vector<double> lagM, gatedRmsM, flooredRmsM, share, maxChangePct;
};

BandShare bandShare(uint64_t seed, int64_t pxMm, int64_t vx0, int64_t vy0, int64_t n,
                    int64_t strideVox, const std::vector<int64_t>& lagsVox) {
    BandShare out;
    const bool fine = pxMm <= 3750;
    out.fine = fine;
    const ProbeOctave* tab = fine ? kProbeFineOctaves : kProbeCoarseOctaves;
    const int nOct = fine ? 4 : 5;
    const int nGated = fine ? kProbeFineGated : kProbeCoarseGated;
    out.nGated = nGated;
    for (int64_t d : lagsVox) {
        double e2g = 0, e2f = 0;
        for (int i = 0; i < nOct; ++i) {
            // One octave's unit-amplitude S2 rms, measured on the same lattice
            // the rest of this block uses.
            const int64_t L = tab[i].latticeMm;
            const uint32_t ch = CH_DETAIL_OCTAVE_BASE + static_cast<uint32_t>(i);
            long double sq = 0;
            int64_t cnt = 0;
            for (int64_t j = 0; j < n; ++j)
                for (int64_t k = 0; k < n; ++k) {
                    const int64_t x = (vx0 + k * strideVox) * kVoxelSizeMm;
                    const int64_t y = (vy0 + j * strideVox) * kVoxelSizeMm;
                    const int64_t dm = d * kVoxelSizeMm;
                    auto f = [&](int64_t ax, int64_t ay) {
                        return static_cast<double>(valueNoise2Fade(seed, ax, ay, L, ch)) *
                               static_cast<double>(tab[i].amplitudeMm) / 32768.0;
                    };
                    const double c = f(x, y);
                    const double sx = f(x + dm, y) - 2 * c + f(x - dm, y);
                    const double sy = f(x, y + dm) - 2 * c + f(x, y - dm);
                    sq += sx * sx + sy * sy;
                    cnt += 2;
                }
            const double e = cnt ? static_cast<double>(sq / static_cast<long double>(cnt)) : 0.0;
            if (i < nGated)
                e2g += e;
            else
                e2f += e;
        }
        const double tot = e2g + e2f;
        const double s = tot > 0 ? e2g / tot : 0.0;
        out.lagM.push_back(static_cast<double>(d) * kVoxelSizeMm / 1000.0);
        out.gatedRmsM.push_back(std::sqrt(e2g) / 1000.0);
        out.flooredRmsM.push_back(std::sqrt(e2f) / 1000.0);
        out.share.push_back(s);
        // v10 gates BOTH bands: the shaping band at the full excursion and the
        // microrelief band at half of it. So the largest possible move in total
        // rms at this lag is sqrt(gL^2*s + gM^2*(1-s)) - 1 with both gains at
        // their ceilings — which is what an S2 estimator could see at best.
        const double gmax = static_cast<double>(kCurvatureScaleMaxQ10) / 1024.0;
        const double mmax = static_cast<double>(probeMicroGateQ10(kCurvatureScaleMaxQ10)) / 1024.0;
        out.maxChangePct.push_back(
            (std::sqrt(gmax * gmax * s + mmax * mmax * (1 - s)) - 1.0) * 100.0);
    }
    return out;
}

void printBandShare(const BandShare& b) {
    std::printf("\nGATED-BAND SHARE OF DETAIL ENERGY, PER LAG (%s tier table)\n",
                b.fine ? "FINE" : "coarse");
    std::printf("  the first %d octave(s) take the FULL curvature gate; the rest take HALF its\n"
                "  excursion. The last column is what that can move total S2 by, at best.\n",
                b.nGated);
    std::printf("  %10s %14s %14s %12s %14s\n", "lag (m)", "gated rms(m)", "floored rms(m)",
                "gated share", "max S2 change");
    for (size_t i = 0; i < b.lagM.size(); ++i)
        std::printf("  %10.2f %14.6f %14.6f %11.2f%% %13.2f%%\n", b.lagM[i], b.gatedRmsM[i],
                    b.flooredRmsM[i], b.share[i] * 100.0, b.maxChangePct[i]);
    std::printf("  (last column is the LARGEST possible change in total S2 at that lag from both\n"
                "   gains reaching their ceilings, %.2fx and %.3fx. Where it is under a few per\n"
                "   cent, a conditioned-S2 ratio at that lag cannot see the gate no matter what.)\n",
                static_cast<double>(kCurvatureScaleMaxQ10) / 1024.0,
                static_cast<double>(probeMicroGateQ10(kCurvatureScaleMaxQ10)) / 1024.0);
}

// ---------------------------------------------------------------------------
// RILL ISOLATION — the term alone, the rest alone, and the noise floor.
//
// WHY THE COMPOSITE MEASUREMENT WAS NOT ENOUGH. rillAnisotropyByGrade above
// measures across/along on the RENDERED surface. On v10 that reads 0.99-1.05 at
// 1-3 m on steep ground, with the 45-degree control at 1.12 — i.e. the control
// is LARGER than the signal, so the composite measurement is not a measurement
// at all, it is noise. Two entirely different situations produce that reading:
//
//   (a) the rill term is doing nothing, or
//   (b) the rill term is doing exactly what it was built to do and is BURIED
//       under an isotropic band an order of magnitude larger.
//
// A composite ratio cannot separate them. This does, by measuring three fields
// on the same points, in the same frames, at the same lags:
//
//   RILL ALONE          — rillMm evaluated directly. If this is anisotropic, the
//                         term works and the question is amplitude.
//   DETAIL MINUS RILL   — (amplified - carrier) - rill. The isotropic bed the
//                         rill has to be seen against.
//   COMPOSITE           — amplified - carrier. What the eye gets.
//
// TWO FRAMES, because a frame mismatch and a missing signal look identical:
//
//   AMPLIFIED frame — the +/-25 m gradient of the RENDERED surface, which is
//                     what rillAnisotropyByGrade uses.
//   CARRIER frame   — the carrier's ANALYTIC gradient at the point, which is
//                     literally the vector rillMm receives. If the term is
//                     strong in the carrier frame and weak in the amplified
//                     one, the metric was pointing the wrong way and no
//                     amplitude change would have fixed it.
//
// AND A NOISE FLOOR, because the 45-degree control is not decoration. The frame
// is rounded onto the voxel lattice, so a perfectly isotropic field still scores
// a ratio slightly off 1.0; the control measures exactly that bias on the same
// points. NOTHING within the control's own excursion of 1.0 is a measurement.
// The detectability column below states what the ratio would have to reach
// before it means anything, and the amplitude solve targets THAT, not 1.0.
struct RillIso {
    int64_t nPts = 0;
    double minGradePct = 0;
    std::vector<double> lagM;
    // [frame][lag]; frame 0 = amplified +/-25 m gradient, 1 = carrier analytic.
    std::vector<double> comp[2], rill[2], rest[2], ctrl[2];
    // rms amplitude of each field, for the dilution arithmetic.
    std::vector<double> rillRmsM, restRmsM;
    // THE PAIRED STATISTIC (carrier frame): composite ratio minus rest-alone
    // ratio, on the SAME points with the SAME frame. Every bias the estimator
    // has — the voxel-lattice rounding of the frame, the grade selection, the
    // finite sample — is common to both terms and CANCELS in the difference, so
    // this is sensitive to the rill at amplitudes where the absolute ratio is
    // hopelessly buried. `pairedSpread` is a deterministic split-half estimate
    // of its own sampling error (even-indexed points against odd-indexed), which
    // is what turns it from a number into a measurement.
    std::vector<double> paired, pairedSpread;
};

RillIso rillIsolation(Amplifier& amp, ITileSampler& tiles, uint64_t seed, int64_t vx0, int64_t vy0,
                      int64_t n, int64_t strideVox, int64_t gradBaseVox,
                      const std::vector<int64_t>& lagsVox, double minGradePct) {
    RillIso r;
    r.minGradePct = minGradePct;
    const int64_t pxMm = tiles.pixelSizeMm();
    const size_t nl = lagsVox.size();
    for (int f = 0; f < 2; ++f) {
        r.comp[f].assign(nl, 0.0);
        r.rill[f].assign(nl, 0.0);
        r.rest[f].assign(nl, 0.0);
        r.ctrl[f].assign(nl, 0.0);
    }
    r.rillRmsM.assign(nl, 0.0);
    r.restRmsM.assign(nl, 0.0);
    std::vector<long double> aC[2], cC[2], aR[2], cR[2], aE[2], cE[2], p1[2], p2[2];
    for (int f = 0; f < 2; ++f) {
        aC[f].assign(nl, 0.0L);
        cC[f].assign(nl, 0.0L);
        aR[f].assign(nl, 0.0L);
        cR[f].assign(nl, 0.0L);
        aE[f].assign(nl, 0.0L);
        cE[f].assign(nl, 0.0L);
        p1[f].assign(nl, 0.0L);
        p2[f].assign(nl, 0.0L);
    }
    std::vector<long double> rAmp(nl, 0.0L), eAmp(nl, 0.0L);
    // Split-half accumulators for the carrier frame only: [half][lag].
    std::vector<long double> hCa[2], hCc[2], hEa[2], hEc[2];
    for (int h = 0; h < 2; ++h) {
        hCa[h].assign(nl, 0.0L);
        hCc[h].assign(nl, 0.0L);
        hEa[h].assign(nl, 0.0L);
        hEc[h].assign(nl, 0.0L);
    }
    const double invSqrt2 = 1.0 / std::sqrt(2.0);
    int64_t cnt = 0;

    // The three fields, as point functions. rillAt re-evaluates the CARRIER
    // GRADIENT at every stencil arm rather than freezing it at the centre: the
    // shipped term is a function of position and the local gradient, and
    // freezing it would measure a different field from the one that renders.
    auto rillAt = [&](int64_t x, int64_t y) {
        const ProbeCarrier c = evalProbeCarrier(tiles, x, y);
        return static_cast<double>(rillMm(seed, x * kVoxelSizeMm, y * kVoxelSizeMm,
                                          c.sxMmPerPx * 1000 / pxMm, c.syMmPerPx * 1000 / pxMm));
    };
    auto compAt = [&](int64_t x, int64_t y) {
        return static_cast<double>(amp.surfaceMm(x, y)) - static_cast<double>(carrierMm(tiles, x, y));
    };
    auto restAt = [&](int64_t x, int64_t y) { return compAt(x, y) - rillAt(x, y); };

    for (int64_t j = 0; j < n; ++j)
        for (int64_t i = 0; i < n; ++i) {
            const int64_t x = vx0 + i * strideVox, y = vy0 + j * strideVox;
            // Frame 0: the amplified surface's +/-25 m gradient (the existing
            // metric's frame). Frame 1: the carrier's analytic gradient, i.e.
            // exactly what rillMm is handed.
            const double agx = static_cast<double>(amp.surfaceMm(x + gradBaseVox, y) -
                                                   amp.surfaceMm(x - gradBaseVox, y));
            const double agy = static_cast<double>(amp.surfaceMm(x, y + gradBaseVox) -
                                                   amp.surfaceMm(x, y - gradBaseVox));
            const double agl = std::sqrt(agx * agx + agy * agy);
            const double gradePct =
                agl / (2.0 * static_cast<double>(gradBaseVox) * kVoxelSizeMm) * 100.0;
            if (gradePct < minGradePct) continue;
            const ProbeCarrier pc = evalProbeCarrier(tiles, x, y);
            const double cgx = static_cast<double>(pc.sxMmPerPx), cgy = static_cast<double>(pc.syMmPerPx);
            const double cgl = std::sqrt(cgx * cgx + cgy * cgy);
            if (agl < 1.0 || cgl < 1.0) continue;
            ++cnt;
            const double ux[2] = {agx / agl, cgx / cgl};
            const double uy[2] = {agy / agl, cgy / cgl};
            for (int f = 0; f < 2; ++f) {
                const double vx = -uy[f], vy = ux[f];
                const double d1x = (ux[f] + vx) * invSqrt2, d1y = (uy[f] + vy) * invSqrt2;
                const double d2x = (ux[f] - vx) * invSqrt2, d2y = (uy[f] - vy) * invSqrt2;
                for (size_t li = 0; li < nl; ++li) {
                    const int64_t d = lagsVox[li];
                    auto s2 = [&](auto&& fn, double dx, double dy) {
                        const int64_t ox =
                            static_cast<int64_t>(std::lround(dx * static_cast<double>(d)));
                        const int64_t oy =
                            static_cast<int64_t>(std::lround(dy * static_cast<double>(d)));
                        const double v = fn(x + ox, y + oy) + fn(x - ox, y - oy) - 2 * fn(x, y);
                        return v * v;
                    };
                    aC[f][li] += s2(compAt, ux[f], uy[f]);
                    cC[f][li] += s2(compAt, vx, vy);
                    aR[f][li] += s2(rillAt, ux[f], uy[f]);
                    cR[f][li] += s2(rillAt, vx, vy);
                    aE[f][li] += s2(restAt, ux[f], uy[f]);
                    cE[f][li] += s2(restAt, vx, vy);
                    p1[f][li] += s2(compAt, d1x, d1y);
                    p2[f][li] += s2(compAt, d2x, d2y);
                    if (f == 0) {
                        rAmp[li] += s2(rillAt, ux[f], uy[f]);
                        eAmp[li] += s2(restAt, ux[f], uy[f]);
                    } else {
                        // Split by the accepted-point index, so the two halves
                        // are interleaved over the lattice rather than being two
                        // different pieces of ground.
                        const int h = static_cast<int>(cnt & 1);
                        hCa[h][li] += s2(compAt, ux[f], uy[f]);
                        hCc[h][li] += s2(compAt, vx, vy);
                        hEa[h][li] += s2(restAt, ux[f], uy[f]);
                        hEc[h][li] += s2(restAt, vx, vy);
                    }
                }
            }
        }
    r.nPts = cnt;
    for (size_t li = 0; li < nl; ++li) {
        r.lagM.push_back(static_cast<double>(lagsVox[li]) * kVoxelSizeMm / 1000.0);
        if (!cnt) continue;
        const long double N = static_cast<long double>(cnt);
        auto rat = [&](long double a, long double c) {
            return a > 0 ? std::sqrt(static_cast<double>(c / a)) : 0.0;
        };
        for (int f = 0; f < 2; ++f) {
            r.comp[f][li] = rat(aC[f][li], cC[f][li]);
            r.rill[f][li] = rat(aR[f][li], cR[f][li]);
            r.rest[f][li] = rat(aE[f][li], cE[f][li]);
            r.ctrl[f][li] = rat(p2[f][li], p1[f][li]);
        }
        r.rillRmsM[li] = std::sqrt(static_cast<double>(rAmp[li] / N)) / 1000.0;
        r.restRmsM[li] = std::sqrt(static_cast<double>(eAmp[li] / N)) / 1000.0;
        r.paired.push_back(r.comp[1][li] - r.rest[1][li]);
        const double d0 = rat(hCa[0][li], hCc[0][li]) - rat(hEa[0][li], hEc[0][li]);
        const double d1 = rat(hCa[1][li], hCc[1][li]) - rat(hEa[1][li], hEc[1][li]);
        r.pairedSpread.push_back(std::abs(d0 - d1) / 2.0);
    }
    return r;
}

void printRillIso(const RillIso& r) {
    std::printf("\nRILL ISOLATION — the term alone, the bed it sits in, and the noise floor\n");
    std::printf("  %lld points at >= %.0f%% grade. Ratios are across/along; 1.00 = isotropic.\n",
                (long long)r.nPts, r.minGradePct);
    if (!r.nPts) {
        std::printf("  no points at this grade on this lattice; try a steeper site\n");
        return;
    }
    for (int f = 0; f < 2; ++f) {
        std::printf("\n  FRAME: %s\n", f == 0 ? "amplified surface, +/-25 m gradient (the frame "
                                                "the composite metric uses)"
                                              : "CARRIER analytic gradient (the vector rillMm is "
                                                "actually handed)");
        std::printf("  %8s %11s %11s %11s %11s %13s %13s\n", "lag (m)", "rill alone", "rest alone",
                    "composite", "45deg ctrl", "rill rms(m)", "rest rms(m)");
        for (size_t li = 0; li < r.lagM.size(); ++li)
            std::printf("  %8.2f %11.3f %11.3f %11.3f %11.3f %13.5f %13.5f\n", r.lagM[li],
                        r.rill[f][li], r.rest[f][li], r.comp[f][li], r.ctrl[f][li], r.rillRmsM[li],
                        r.restRmsM[li]);
    }
    // THE PAIRED TEST. Comparing the composite ratio against an absolute 1.00
    // throws away the fact that we can measure the SAME surface with the rill
    // removed. The difference between the two is bias-free by construction — the
    // frame rounding, the grade selection and the sample are identical — so it
    // sees the term at amplitudes where the absolute ratio cannot. This is the
    // statistic to quote for "is the rill doing anything".
    std::printf("\n  PAIRED TEST (carrier frame): composite ratio minus rest-alone ratio, same\n"
                "  points, same frame, so every estimator bias cancels. +/- is a split-half\n"
                "  estimate of this statistic's own sampling error.\n");
    std::printf("  %8s %14s %14s %12s %14s\n", "lag (m)", "rest alone", "composite", "difference",
                "significant?");
    for (size_t li = 0; li < r.lagM.size(); ++li) {
        char verdict[48];
        const double s = r.pairedSpread[li];
        if (s > 0 && std::abs(r.paired[li]) > 3.0 * s)
            std::snprintf(verdict, sizeof(verdict), "YES, %.1f sigma", std::abs(r.paired[li]) / s);
        else if (s > 0 && std::abs(r.paired[li]) > s)
            std::snprintf(verdict, sizeof(verdict), "marginal, %.1f", std::abs(r.paired[li]) / s);
        else
            std::snprintf(verdict, sizeof(verdict), "no");
        std::printf("  %8.2f %14.4f %14.4f %+8.4f+-%.4f %14s\n", r.lagM[li], r.rest[1][li],
                    r.comp[1][li], r.paired[li], s, verdict);
    }

    // DETECTABILITY AGAINST AN ABSOLUTE REFERENCE. The control's own departure
    // from 1.0 is the estimator's bias on these very points, so a composite
    // ratio inside that band carries no information WHEN QUOTED ALONE. Solving
    // the dilution for the amplitude that clears it, rather than for a ratio
    // someone picked, is the difference between a calibration and a preference —
    // but read it together with the paired test above, which needs no such
    // amplitude because it has no bias to clear.
    std::printf("\n  DETECTABILITY AND THE AMPLITUDE THAT WOULD REACH IT (carrier frame):\n");
    std::printf("  %8s %13s %13s %14s %16s\n", "lag (m)", "floor |ctrl-1|", "composite-1",
                "detectable?", "amp x N to clear");
    for (size_t li = 0; li < r.lagM.size(); ++li) {
        const double floorAbs = std::abs(r.ctrl[1][li] - 1.0);
        const double sig = r.comp[1][li] - 1.0;
        // Composite ratio for a rill scaled by k, treating the two fields as
        // independent and the rest as isotropic:
        //   R(k)^2 = (E_across + k^2 R_across) / (E_along + k^2 R_along)
        // Solve R(k) = 1 + 2*floor (a 2-sigma-ish bar), then report k.
        const double target = 1.0 + 2.0 * floorAbs;
        const double T2 = target * target;
        // Recover the four energies from the ratios and the rms amplitudes.
        const double Ra = r.rillRmsM[li] * r.rillRmsM[li];
        const double Rc = Ra * r.rill[1][li] * r.rill[1][li];
        const double Ea = r.restRmsM[li] * r.restRmsM[li];
        const double Ec = Ea * r.rest[1][li] * r.rest[1][li];
        const double num = T2 * Ea - Ec, den = Rc - T2 * Ra;
        const bool detect = sig > floorAbs;
        if (den <= 0 || num <= 0)
            std::printf("  %8.2f %13.4f %13.4f %14s %16s\n", r.lagM[li], floorAbs, sig,
                        detect ? "YES" : "no", den <= 0 ? "unreachable" : "already");
        else
            std::printf("  %8.2f %13.4f %13.4f %14s %15.1fx\n", r.lagM[li], floorAbs, sig,
                        detect ? "YES" : "no", std::sqrt(num / den));
    }
    std::printf("  (last column multiplies kRillAmplitudeMm = %lld mm. A term needing a large\n"
                "   multiplier to become MEASURABLE is not necessarily wrong — it may be\n"
                "   correctly subtle — but nothing can be verified about it until it clears\n"
                "   the floor, and shouting it to make a metric move is how a wrong term ships.)\n",
                (long long)kRillAmplitudeMm);
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

// ===========================================================================
// CALIBRATION HARNESS — docs/terrain-amplification-plan.md §3c
// ===========================================================================
//
// WHAT PROBLEM THIS SOLVES. Phase 3 adds four detail mechanisms to the client
// amplifier and EVERY amplitude in all four is currently marked PROVISIONAL and
// UNCALIBRATED (carrier.h's kCurvatureScale*/kCurvatureKnee*, detail_rill.h's
// kRillAmplitudeMm, detail_bedding.h's kBeddingAmpMm/kBedding3AmpMm, and the
// plan's `{3200, 1600, 400, 200}` mm continuation ladder). The plan says they
// must be "set by probe measurement, not taste", against the fine tier's
// measured S2. This block is that measurement.
//
// THE GOVERNING IDEA. The coarse raster is self-affine: S2(d) ~ C*d^H from
// 960 m down to 30 m. The client's detail ladder should CONTINUE that structure
// function through the 7.5 m -> 0.2 m band rather than invent an unrelated
// spectrum. So: measure (H, C) on real data over the band where data exists,
// evaluate the continuation C*d^H over the client band, and solve for the
// octave amplitudes that make the client's own S2 land on it.
//
// ---------------------------------------------------------------------------
// WHY THE SOLVE IS LINEAR, AND WHY IT IS DONE IN RMS RATHER THAN MEAN-ABS.
//
// The octaves are independent zero-mean fields (different lattices, different
// hash channels), so their VARIANCES add and their mean-absolute values do not.
// Writing k_i(d) for the second-difference RMS of octave i at unit amplitude,
//
//     S2_rms(d)^2  =  SUM_i  A_i^2 * k_i(d)^2
//
// which is LINEAR in u_i = A_i^2. Four unknowns, eleven lags, one non-negative
// least squares — no search, no eyeballing, and a unique global optimum found
// by enumerating all 16 active sets (with 4 variables that enumeration IS the
// exact NNLS solution, not an approximation to it).
//
// Everything downstream is reported in BOTH currencies, because the rest of
// this tool and the plan's own acceptance text are written in mean-abs S2 and
// quoting one as the other would be a ~25% silent error (a Gaussian's
// E|X| = sqrt(2/pi)*sigma = 0.798*sigma).
//
// ---------------------------------------------------------------------------
// THE VERIFICATION PASS IS NOT OPTIONAL. Everything above is a MODEL of the
// octave sum: it assumes independence, it assumes the kernels are stationary,
// and it ignores the integer truncation evalSurface actually performs. So after
// solving, the ladder is SYNTHESISED at the solved integer amplitudes using
// valueNoise2Fade and the same `noise * amp / 32768` integer arithmetic
// evalSurface uses, and its S2 is measured directly on the same lattice. The
// residual table prints the MEASURED result, not the modelled one. If the two
// disagree, the model is wrong and the measurement wins.
//
// ---------------------------------------------------------------------------
// WHAT THIS METHOD CANNOT DO, STATED UP FRONT SO NOBODY QUOTES IT WRONGLY.
//
// S2(d) is a scalar per lag. It has exactly as many degrees of freedom as it
// has lags, it is direction-blind, and it is blind to any conditioning. So:
//
//   * The CONTINUATION LADDER is genuinely constrained by it. That is what an
//     fBm octave ladder IS — a set of amplitudes chosen to place energy per
//     scale — and S2 measures energy per scale. Take those numbers.
//
//   * The RILL term is NOT separately constrained. Its 1.6 m across-slope
//     wavelength puts its energy in the same band as the 1600 mm octave, and
//     S2 cannot tell a 1.6 m anisotropic field from a 1.6 m isotropic one:
//     the two design columns are collinear (this block prints the actual
//     collinearity, so the claim is measured rather than asserted). What DOES
//     constrain it is the across/along anisotropy ratio, which is a different
//     statistic and is solved for separately below.
//
//   * The CURVATURE GATE is a RATIO, not an amplitude. It multiplies whatever
//     detail is there, so S2 constrains only its ROOT-MEAN-SQUARE gain over
//     the terrain (which is a real constraint: it rescales the whole ladder,
//     and this block reports the factor). Its SHAPE — the min, max and knee —
//     is constrained by the curvature-CONDITIONED ratio, which is what
//     curvatureConditionedRoughness above measures. This block solves the knee
//     against that target from the measured curvature distribution.
//
//   * The BEDDING term is quasi-periodic and keyed on ELEVATION, not on
//     horizontal position, so its horizontal wavelength is thickness/sin(dip
//     relative to the ground) and varies with the local slope. It has no fixed
//     lag. This block measures the S2 it actually produces per lag on real
//     terrain, so its energy is at least accounted for in the budget — but the
//     number that says whether it is right is its VISIBILITY as layering, and
//     S2 cannot see that at all.
//
// DETERMINISM. Fixed integer lattices anchored on the transect origin, fixed
// lag sets, fixed seed, no rand(), no clock, no wall time in any output.
// ---------------------------------------------------------------------------

// The plan's continuation ladder. Lattice sizes are FIXED by the plan; the
// amplitudes are what this harness solves for.
constexpr int kLadderN = 4;
constexpr int64_t kLadderLatticeMm[kLadderN] = {3200, 1600, 400, 200};

// The kernels are measured at a 1000 mm reference amplitude, so a kernel value
// expressed in metres is exactly "metres of S2 per metre of octave amplitude" —
// dimensionless. That is what makes the design matrix unit-free.
constexpr double kKernelRefAmpMm = 1000.0;

// The client band, in voxels: 0.2 m (two voxels, the finest lattice in the
// ladder and the finest scale at which value noise is a shape rather than
// per-voxel static) through 7.5 m (four fine-tier posts — the band edge the
// plan names).
const std::vector<int64_t> kCalLagsVox = {2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 75};

struct S2Curve {
    std::vector<double> lagM, absM, rmsM;
    std::vector<int64_t> n;
};

// Structure function of the SOURCE RASTER — the tile control lattice itself,
// not the amplified surface. This is the trend the client is meant to continue,
// so it must be read off the data and not off our own output.
//
// Both axes at every lag, so the estimator is direction-neutral.
//
// THE LAND MASK IS LOAD-BEARING. A tile that is 60% ocean has 60% of its
// stencils sitting on a dead-flat sea, and those contribute S2 = 0 to the mean,
// which drags C down and H up by an amount that depends entirely on how much
// water happened to be in frame. The mask requires the centre AND all four arms
// to be above sea level; `n` per lag is printed so the shrinkage with lag is
// visible rather than hidden.
S2Curve rasterS2(ITileSampler& tiles, int64_t px0, int64_t py0, int64_t nPx,
                 const std::vector<int64_t>& lagPx, bool landOnly) {
    const double pxM = static_cast<double>(tiles.pixelSizeMm()) / 1000.0;
    S2Curve c;
    for (int64_t d : lagPx) {
        long double sa = 0, sq = 0;
        int64_t cnt = 0;
        for (int64_t j = 0; j < nPx; ++j)
            for (int64_t i = 0; i < nPx; ++i) {
                const int64_t x = px0 + i, y = py0 + j;
                const double h = static_cast<double>(tiles.elevationMm(x, y));
                if (landOnly && h <= 0) continue;
                const double xm = static_cast<double>(tiles.elevationMm(x - d, y));
                const double xp = static_cast<double>(tiles.elevationMm(x + d, y));
                const double ym = static_cast<double>(tiles.elevationMm(x, y - d));
                const double yp = static_cast<double>(tiles.elevationMm(x, y + d));
                if (landOnly && (xm <= 0 || xp <= 0 || ym <= 0 || yp <= 0)) continue;
                const double sx = xp - 2 * h + xm, sy = yp - 2 * h + ym;
                sa += std::abs(sx) + std::abs(sy);
                sq += sx * sx + sy * sy;
                cnt += 2;
            }
        c.lagM.push_back(static_cast<double>(d) * pxM);
        c.absM.push_back(cnt ? static_cast<double>(sa / cnt) / 1000.0 : 0.0);
        c.rmsM.push_back(cnt ? std::sqrt(static_cast<double>(sq / cnt)) / 1000.0 : 0.0);
        c.n.push_back(cnt);
    }
    return c;
}

// A fixed integer sample lattice, anchored on the transect origin.
struct CalLattice {
    int64_t vx0 = 0, vy0 = 0, n = 0, strideVox = 0;
};

// S2 of an arbitrary point function of voxel coordinates, returning millimetres.
// The stencil arms reach OUTSIDE the lattice by design — every field here is
// defined everywhere, and letting the arms leave keeps the centre set identical
// across lags so the curve is a curve rather than eleven different samples.
template <typename F>
S2Curve fieldS2(const CalLattice& L, const std::vector<int64_t>& lagVox, F f) {
    S2Curve c;
    for (int64_t d : lagVox) {
        long double sa = 0, sq = 0;
        int64_t cnt = 0;
        for (int64_t j = 0; j < L.n; ++j)
            for (int64_t i = 0; i < L.n; ++i) {
                const int64_t x = L.vx0 + i * L.strideVox, y = L.vy0 + j * L.strideVox;
                const double h = f(x, y);
                const double sx = f(x + d, y) - 2 * h + f(x - d, y);
                const double sy = f(x, y + d) - 2 * h + f(x, y - d);
                sa += std::abs(sx) + std::abs(sy);
                sq += sx * sx + sy * sy;
                cnt += 2;
            }
        c.lagM.push_back(static_cast<double>(d) * kVoxelSizeMm / 1000.0);
        c.absM.push_back(cnt ? static_cast<double>(sa / cnt) / 1000.0 : 0.0);
        c.rmsM.push_back(cnt ? std::sqrt(static_cast<double>(sq / cnt)) / 1000.0 : 0.0);
        c.n.push_back(cnt);
    }
    return c;
}

// Power-law fit log S2 = logC + H log d over a stated lag window. fitLine above
// supplies the slope and R^2; the intercept comes from the means, because the
// extrapolation needs C and a slope alone cannot give it.
struct BandFit {
    double H = 0, logC = 0, r2 = 0;
    int64_t nLags = 0;
    double loM = 0, hiM = 0;
    bool valid = false;
    double at(double dM) const { return std::exp(logC + H * std::log(dM)); }
};

BandFit fitBand(const S2Curve& c, bool useRms, double loM, double hiM) {
    BandFit f;
    f.loM = loM;
    f.hiM = hiM;
    std::vector<double> x, y;
    for (size_t i = 0; i < c.lagM.size(); ++i) {
        const double s = useRms ? c.rmsM[i] : c.absM[i];
        // The 1e-9 slack absorbs the binary representation of exact decimal lags
        // (7.5, 960) so a window stated in metres includes its own endpoints.
        if (s <= 0 || c.lagM[i] < loM * (1 - 1e-9) || c.lagM[i] > hiM * (1 + 1e-9)) continue;
        x.push_back(std::log(c.lagM[i]));
        y.push_back(std::log(s));
    }
    f.nLags = static_cast<int64_t>(x.size());
    if (x.size() < 3) return f;
    const LineFit lf = fitLine(x, y);
    double mx = 0, my = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        mx += x[i];
        my += y[i];
    }
    mx /= static_cast<double>(x.size());
    my /= static_cast<double>(y.size());
    f.H = lf.slope;
    f.r2 = lf.r2;
    f.logC = my - lf.slope * mx;
    f.valid = true;
    return f;
}

// Gaussian elimination with partial pivoting on a k x k system, k <= 4.
bool solveDense(double A[kLadderN][kLadderN], double b[kLadderN], int k, double x[kLadderN]) {
    for (int col = 0; col < k; ++col) {
        int piv = col;
        for (int r = col + 1; r < k; ++r)
            if (std::abs(A[r][col]) > std::abs(A[piv][col])) piv = r;
        if (std::abs(A[piv][col]) < 1e-300) return false;
        if (piv != col) {
            for (int j = 0; j < k; ++j) std::swap(A[col][j], A[piv][j]);
            std::swap(b[col], b[piv]);
        }
        for (int r = col + 1; r < k; ++r) {
            const double fct = A[r][col] / A[col][col];
            for (int j = col; j < k; ++j) A[r][j] -= fct * A[col][j];
            b[r] -= fct * b[col];
        }
    }
    for (int r = k - 1; r >= 0; --r) {
        double s = b[r];
        for (int j = r + 1; j < k; ++j) s -= A[r][j] * x[j];
        x[r] = s / A[r][r];
    }
    return true;
}

// EXACT non-negative least squares for the four-octave design, by enumerating
// every active set. The constrained optimum has SOME set of octaves pinned at
// zero and is the unconstrained minimiser over the rest; there are only 16 such
// sets, so enumerating them and keeping the best feasible one is the global
// optimum rather than a heuristic. No iteration count, no tolerance, no
// dependence on a starting point — which also means it is bit-reproducible.
//
// Rows are pre-divided by the target, so the objective is RELATIVE error: a
// 10% miss at the 0.2 m lag counts the same as a 10% miss at 7.5 m, which is
// what "continue the spectrum" means. An unweighted fit would be dominated
// entirely by the largest lag and would leave the fine end unconstrained.
struct LadderSolve {
    double u[kLadderN] = {0, 0, 0, 0}; // A_i^2 in metres^2 of amplitude
    double ampMm[kLadderN] = {0, 0, 0, 0};
    int32_t ampMmInt[kLadderN] = {0, 0, 0, 0};
    double resid = 0;
    int64_t nRows = 0;
    bool ok = false;
};

// `maxLagM` exists for a reason that is easy to get wrong. A value-noise octave
// of lattice L is FLAT in d for d > L — its second difference has already
// decorrelated — so at 7.5 m all four columns of a `{3200,...,200}` ladder are
// within 2% of each other and the row degenerates to "the sum of the four
// amplitudes is X". It carries no information about how to SPLIT them, but under
// relative weighting it carries full weight, and because the ladder physically
// cannot reach the 7.5 m target it carries a CONTRADICTORY one. Left in, it
// drags the coarsest octave up and wrecks the fine end. Excluding lags past the
// coarsest lattice is not tuning the answer; it is declining to fit a parameter
// with data that cannot constrain it.
LadderSolve solveLadder(const std::vector<double>& lagM, const std::vector<double>& targetM,
                        const std::vector<double>& carrierM, const S2Curve kern[kLadderN],
                        double maxLagM) {
    // Rows: only lags where the target has headroom left over the carrier. A
    // lag where the carrier ALREADY exceeds the continuation target has nothing
    // to ask of the ladder and must not be turned into a negative demand.
    std::vector<std::array<double, kLadderN>> A;
    std::vector<double> rhs;
    for (size_t li = 0; li < lagM.size(); ++li) {
        if (lagM[li] > maxLagM * (1 + 1e-9)) continue;
        const double b = targetM[li] * targetM[li] - carrierM[li] * carrierM[li];
        if (b <= 0) continue;
        std::array<double, kLadderN> row{};
        for (int i = 0; i < kLadderN; ++i) {
            const double k = kern[i].rmsM[li];
            row[static_cast<size_t>(i)] = k * k / b; // pre-divided: relative residual
        }
        A.push_back(row);
        rhs.push_back(1.0);
    }
    LadderSolve out;
    out.nRows = static_cast<int64_t>(A.size());
    if (A.size() < 2) return out;

    double best = 0;
    bool have = false;
    for (int mask = 1; mask < (1 << kLadderN); ++mask) {
        int idx[kLadderN], k = 0;
        for (int i = 0; i < kLadderN; ++i)
            if (mask & (1 << i)) idx[k++] = i;
        double N[kLadderN][kLadderN] = {{0}}, g[kLadderN] = {0}, sol[kLadderN] = {0};
        for (size_t r = 0; r < A.size(); ++r)
            for (int a = 0; a < k; ++a) {
                g[a] += A[r][static_cast<size_t>(idx[a])] * rhs[r];
                for (int b2 = 0; b2 < k; ++b2)
                    N[a][b2] += A[r][static_cast<size_t>(idx[a])] * A[r][static_cast<size_t>(idx[b2])];
            }
        if (!solveDense(N, g, k, sol)) continue;
        bool feasible = true;
        for (int a = 0; a < k; ++a)
            if (sol[a] < 0) feasible = false;
        if (!feasible) continue;
        double u[kLadderN] = {0, 0, 0, 0};
        for (int a = 0; a < k; ++a) u[idx[a]] = sol[a];
        double res = 0;
        for (size_t r = 0; r < A.size(); ++r) {
            double v = 0;
            for (int i = 0; i < kLadderN; ++i) v += A[r][static_cast<size_t>(i)] * u[i];
            res += (v - rhs[r]) * (v - rhs[r]);
        }
        if (!have || res < best) {
            best = res;
            have = true;
            for (int i = 0; i < kLadderN; ++i) out.u[i] = u[i];
        }
    }
    if (!have) return out;
    out.resid = std::sqrt(best / static_cast<double>(A.size()));
    for (int i = 0; i < kLadderN; ++i) {
        out.ampMm[i] = std::sqrt(out.u[i]) * kKernelRefAmpMm;
        // Rounded to integer millimetres because that is what the Octave table
        // holds; the verification pass below then measures the ROUNDED ladder,
        // so the printed residual is the residual of what would actually ship.
        out.ampMmInt[i] = static_cast<int32_t>(std::llround(out.ampMm[i]));
    }
    out.ok = true;
    return out;
}

// The gate, re-parameterised on the knee so the knee can be SOLVED for rather
// than asserted. Identical in shape to carrier.h's curvatureScaleQ10; that
// function's own constants are passed in as the defaults at the call site, so
// this is a generalisation of it and not a second opinion about it.
double gateGain(int64_t curveQ10, double kneeQ10, double minQ10, double maxQ10) {
    double c = static_cast<double>(curveQ10);
    if (c < -kneeQ10) c = -kneeQ10;
    if (c > kneeQ10) c = kneeQ10;
    const double g = c < 0 ? 1024.0 + (-c) * (maxQ10 - 1024.0) / kneeQ10
                           : 1024.0 - c * (1024.0 - minQ10) / kneeQ10;
    return g / 1024.0;
}

struct CurvGateCal {
    int64_t nPts = 0;
    double baselineM = 0;
    double terLo = 0, terHi = 0; // tercile edges, mm/m^2
    double p05 = 0, p95 = 0;
    double gConvex = 0, gPlanar = 0, gConcave = 0; // mean gain per tercile, current constants
    double ratioNow = 0;
    double rmsGain = 0;      // sqrt(E[g^2]) — the factor the ladder must absorb
    double kneeForTarget = 0;
    double ratioAtSolvedKnee = 0;
    double ratioCeiling = 0; // the saturated limit, max/min
    bool kneeSolved = false;
    // A sweep, because a single solved knee hides the shape of the trade. The
    // knee is the one constant the plan does NOT pin (it pins 1.75x and 0.5x),
    // so what a reader needs is "what does each knee buy", not one number.
    static const int kSweep = 7;
    double sweepKnee[kSweep] = {0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0};
    double sweepRatio[kSweep] = {0, 0, 0, 0, 0, 0, 0};
    double sweepRmsGain[kSweep] = {0, 0, 0, 0, 0, 0, 0};
};

CurvGateCal calibrateCurvatureGate(ITileSampler& tiles, const CalLattice& L, double targetRatio) {
    CurvGateCal g;
    g.baselineM = static_cast<double>(tiles.pixelSizeMm()) / 1000.0;
    std::vector<int64_t> k;
    k.reserve(static_cast<size_t>(L.n * L.n));
    for (int64_t j = 0; j < L.n; ++j)
        for (int64_t i = 0; i < L.n; ++i)
            // The TIER-NORMALISED value, because that is what the gate is
            // handed. At 30 m the normalisation is the identity and this makes
            // no difference; on the fine tier it is a factor of 256, and a knee
            // solved against the raw quantity there would be wrong by that much.
            k.push_back(probeGateInputQ10(tiles, L.vx0 + i * L.strideVox, L.vy0 + j * L.strideVox));
    g.nPts = static_cast<int64_t>(k.size());
    if (k.empty()) return g;
    std::vector<int64_t> sorted = k;
    std::sort(sorted.begin(), sorted.end());
    const size_t nq = sorted.size();
    auto q = [&](double p) {
        return static_cast<double>(sorted[std::min(nq - 1, static_cast<size_t>(p * static_cast<double>(nq)))]);
    };
    const double loQ = q(1.0 / 3.0), hiQ = q(2.0 / 3.0);
    g.terLo = loQ / static_cast<double>(kCurveQ10One);
    g.terHi = hiQ / static_cast<double>(kCurveQ10One);
    g.p05 = q(0.05) / static_cast<double>(kCurveQ10One);
    g.p95 = q(0.95) / static_cast<double>(kCurveQ10One);

    // Mean gain per tercile under the constants that are in the tree today.
    // Terciles, not fixed thresholds, so the comparison matches exactly what
    // curvatureConditionedRoughness reports and the two can be read together.
    auto tercileGains = [&](double knee, double& gc, double& gp, double& gk) {
        double s[3] = {0, 0, 0};
        int64_t c[3] = {0, 0, 0};
        for (int64_t v : k) {
            const int b = static_cast<double>(v) < loQ ? 0 : (static_cast<double>(v) < hiQ ? 1 : 2);
            s[b] += gateGain(v, knee, static_cast<double>(kCurvatureScaleMinQ10),
                             static_cast<double>(kCurvatureScaleMaxQ10));
            ++c[b];
        }
        gc = c[0] ? s[0] / static_cast<double>(c[0]) : 0;
        gp = c[1] ? s[1] / static_cast<double>(c[1]) : 0;
        gk = c[2] ? s[2] / static_cast<double>(c[2]) : 0;
    };
    tercileGains(static_cast<double>(kCurvatureKneeQ10), g.gConvex, g.gPlanar, g.gConcave);
    g.ratioNow = g.gConcave > 0 ? g.gConvex / g.gConcave : 0;
    long double sq = 0;
    for (int64_t v : k) {
        const double gg = gateGain(v, static_cast<double>(kCurvatureKneeQ10),
                                   static_cast<double>(kCurvatureScaleMinQ10),
                                   static_cast<double>(kCurvatureScaleMaxQ10));
        sq += static_cast<long double>(gg) * gg;
    }
    g.rmsGain = std::sqrt(static_cast<double>(sq / static_cast<long double>(k.size())));
    g.ratioCeiling = static_cast<double>(kCurvatureScaleMaxQ10) /
                     static_cast<double>(kCurvatureScaleMinQ10);

    // The achieved ratio is monotone DECREASING in the knee (a wider knee means
    // less of the distribution saturates, so both tercile means move toward
    // 1.0x). Bisect. If even a knee of one q10 unit — a gate that is a step
    // function in practice — cannot reach the target, the target is above the
    // ceiling max/min and no knee exists; say so rather than return the endpoint.
    double lo = 1.0, hi = 1e9, gc, gp, gk;
    for (int s = 0; s < CurvGateCal::kSweep; ++s) {
        const double kn = g.sweepKnee[s] * static_cast<double>(kCurveQ10One);
        tercileGains(kn, gc, gp, gk);
        g.sweepRatio[s] = gk > 0 ? gc / gk : 0;
        long double q2 = 0;
        for (int64_t v : k) {
            const double gg = gateGain(v, kn, static_cast<double>(kCurvatureScaleMinQ10),
                                       static_cast<double>(kCurvatureScaleMaxQ10));
            q2 += static_cast<long double>(gg) * gg;
        }
        g.sweepRmsGain[s] = std::sqrt(static_cast<double>(q2 / static_cast<long double>(k.size())));
    }
    tercileGains(lo, gc, gp, gk);
    const double rAtLo = gk > 0 ? gc / gk : 0;
    if (rAtLo >= targetRatio) {
        for (int it = 0; it < 200; ++it) {
            const double mid = std::sqrt(lo * hi); // geometric bisection: the knee spans decades
            tercileGains(mid, gc, gp, gk);
            const double r = gk > 0 ? gc / gk : 0;
            if (r > targetRatio)
                lo = mid;
            else
                hi = mid;
        }
        g.kneeForTarget = std::sqrt(lo * hi) / static_cast<double>(kCurveQ10One);
        tercileGains(std::sqrt(lo * hi), gc, gp, gk);
        g.ratioAtSolvedKnee = gk > 0 ? gc / gk : 0;
        g.kneeSolved = true;
    }
    return g;
}

// Empirical anisotropy of the SOURCE RASTER on steep ground, in pixel lags.
//
// This exists so the rill amplitude has a measured anchor rather than a taste
// call. It is NOT a measurement of rill-scale anisotropy — a 30 m raster cannot
// resolve a 1.6 m groove — it is the anisotropy real fluvially-dissected
// terrain shows at the LANDFORM scale, which is the only empirical statement
// about downslope grain this data can make. Quoting it as a rill target is an
// assumption (that grain persists in character across two decades of scale) and
// is labelled as one wherever it is printed.
struct RasterAniso {
    std::vector<double> lagM, along, across, ratio;
    int64_t nPts = 0;
    double minGradePct = 0;
};

RasterAniso rasterAnisotropy(ITileSampler& tiles, int64_t px0, int64_t py0, int64_t nPx,
                             const std::vector<int64_t>& lagPx, double minGradePct) {
    RasterAniso a;
    a.minGradePct = minGradePct;
    const double pxM = static_cast<double>(tiles.pixelSizeMm()) / 1000.0;
    std::vector<long double> sAl(lagPx.size(), 0.0L), sAc(lagPx.size(), 0.0L);
    std::vector<int64_t> cnt(lagPx.size(), 0);
    for (int64_t j = 0; j < nPx; ++j)
        for (int64_t i = 0; i < nPx; ++i) {
            const int64_t x = px0 + i, y = py0 + j;
            const double h = static_cast<double>(tiles.elevationMm(x, y));
            if (h <= 0) continue;
            const double gx = static_cast<double>(tiles.elevationMm(x + 1, y) -
                                                  tiles.elevationMm(x - 1, y));
            const double gy = static_cast<double>(tiles.elevationMm(x, y + 1) -
                                                  tiles.elevationMm(x, y - 1));
            const double gl = std::sqrt(gx * gx + gy * gy);
            const double gradePct = gl / (2.0 * pxM * 1000.0) * 100.0;
            if (gradePct < minGradePct) continue;
            const double ux = gx / gl, uy = gy / gl;
            const double vx = -uy, vy = ux;
            ++a.nPts;
            for (size_t li = 0; li < lagPx.size(); ++li) {
                const int64_t d = lagPx[li];
                auto s2 = [&](double dx, double dy) {
                    const int64_t ox = static_cast<int64_t>(std::lround(dx * static_cast<double>(d)));
                    const int64_t oy = static_cast<int64_t>(std::lround(dy * static_cast<double>(d)));
                    const double v = static_cast<double>(tiles.elevationMm(x + ox, y + oy)) +
                                     static_cast<double>(tiles.elevationMm(x - ox, y - oy)) - 2 * h;
                    return v * v;
                };
                sAl[li] += s2(ux, uy);
                sAc[li] += s2(vx, vy);
                ++cnt[li];
            }
        }
    for (size_t li = 0; li < lagPx.size(); ++li) {
        const double al = cnt[li] ? std::sqrt(static_cast<double>(sAl[li] /
                                                                 static_cast<long double>(cnt[li]))) /
                                        1000.0
                                  : 0.0;
        const double ac = cnt[li] ? std::sqrt(static_cast<double>(sAc[li] /
                                                                 static_cast<long double>(cnt[li]))) /
                                        1000.0
                                  : 0.0;
        a.lagM.push_back(static_cast<double>(lagPx[li]) * pxM);
        a.along.push_back(al);
        a.across.push_back(ac);
        a.ratio.push_back(al > 0 ? ac / al : 0.0);
    }
    return a;
}

// ---------------------------------------------------------------------------
// THE DRIVER. Prints the human report and the greppable rows for both modes;
// `solve` selects whether it stops after the band fit.
void runCalibration(uint64_t seed, ITileSampler& tiles, Amplifier& amp, int64_t vx0, int64_t vy0,
                    int64_t calNPx, const CalLattice& kernL, const CalLattice& fieldL,
                    bool landOnly, bool solve, double bandLoM, double bandHiM, double forcedH,
                    double forcedS2, double curvRatioTarget, double rillRatioTarget,
                    const char* sourceName, const std::atomic<uint64_t>* missingCounter) {
    // COVERAGE GUARD, and it is here because it cost a whole afternoon of wrong
    // conclusions. An ITileSampler answers a query for an unloaded tile with a
    // bland default rather than failing, so a lattice that runs off the edge of
    // the loaded set reads a 3000 m alpine face against a 0 m plain and reports
    // a carrier whose sub-metre roughness is a hundred times the truth. Every
    // number downstream is then garbage that looks perfectly plausible. Count
    // the misses and say so loudly.
    const uint64_t missBefore = missingCounter ? missingCounter->load() : 0;
    const int64_t pxMm = tiles.pixelSizeMm();
    const double pxM = static_cast<double>(pxMm) / 1000.0;
    const int64_t px0 = floorDiv(vx0 * kVoxelSizeMm, pxMm);
    const int64_t py0 = floorDiv(vy0 * kVoxelSizeMm, pxMm);

    // Lags: powers of two in pixels, which at 30 m/px is 30 m .. 960 m (the band
    // the plan names) and at 1875 mm/px is 1.875 m .. 960 m (the same top end,
    // reaching four decades further down).
    std::vector<int64_t> lagPx;
    for (int64_t d = 1; d * pxM <= bandHiM * 1.0000001; d *= 2) lagPx.push_back(d);

    std::printf("\n=== SOURCE RASTER STRUCTURE FUNCTION =====================================\n");
    std::printf("source        : %s\n", sourceName);
    std::printf("pixel size    : %lld mm (%.4f m)\n", (long long)pxMm, pxM);
    std::printf("lattice       : %lld x %lld pixels anchored at pixel (%lld, %lld) = %.1f km sq\n",
                (long long)calNPx, (long long)calNPx, (long long)px0, (long long)py0,
                static_cast<double>(calNPx) * pxM / 1000.0);
    std::printf("land mask     : %s\n",
                landOnly ? "ON (centre and all four arms must be above sea level)" : "OFF");

    const S2Curve src = rasterS2(tiles, px0, py0, calNPx, lagPx, landOnly);
    std::printf("\n  %10s %14s %14s %10s %12s %10s\n", "lag (m)", "S2 mean|.| (m)", "S2 rms (m)",
                "rms/abs", "stencils", "local H");
    for (size_t i = 0; i < src.lagM.size(); ++i) {
        std::printf("  %10.3f %14.5f %14.5f %10.3f %12lld", src.lagM[i], src.absM[i], src.rmsM[i],
                    src.absM[i] > 0 ? src.rmsM[i] / src.absM[i] : 0.0, (long long)src.n[i]);
        if (i > 0 && src.absM[i] > 0 && src.absM[i - 1] > 0)
            std::printf(" %10.3f", (std::log(src.absM[i]) - std::log(src.absM[i - 1])) /
                                       (std::log(src.lagM[i]) - std::log(src.lagM[i - 1])));
        std::printf("\n");
    }

    // --- THE SOURCE'S OWN QUANTISATION FLOOR ------------------------------
    //
    // This is not a detail. A .vxtl v1 elevation plane is int16 METRES, so the
    // raster the whole extrapolation is fitted to has a 1 m quantum. Rounding to
    // that quantum injects an independent uniform(-q/2, q/2) error at every post;
    // the second-difference stencil (1, -2, 1) has coefficient-square sum 6, so
    // it adds a LAG-INDEPENDENT variance of 6 * q^2/12 = q^2/2, i.e. an rms floor
    // of q/sqrt(2) at EVERY lag.
    //
    // Two consequences, both load-bearing for how the fit is read:
    //   * a flat floor added to a rising power law depresses the measured slope,
    //     so the true H is at least the fitted H, never less;
    //   * if the continuation target at the band edge is BELOW this floor, the
    //     source physically cannot measure the quantity being extrapolated, and
    //     no amount of lattice or land masking fixes that.
    //
    // The quantum is MEASURED, not assumed: the GCD of the raster's own first
    // differences. That works at either tier (v2 carries a 100 or 250 mm quant)
    // and cannot go stale if the format changes.
    int64_t quantMm = 0;
    for (int64_t j = 0; j < calNPx; ++j)
        for (int64_t i = 0; i < calNPx; ++i) {
            int64_t d = tiles.elevationMm(px0 + i + 1, py0 + j) - tiles.elevationMm(px0 + i, py0 + j);
            if (d < 0) d = -d;
            while (d) { // binary-free Euclid; the values are small and this runs once per cell
                const int64_t t = quantMm % d;
                quantMm = d;
                d = t;
            }
        }
    const double floorRmsM = static_cast<double>(quantMm) / std::sqrt(2.0) / 1000.0;
    S2Curve deq = src;
    for (size_t i = 0; i < deq.rmsM.size(); ++i) {
        const double v = deq.rmsM[i] * deq.rmsM[i] - floorRmsM * floorRmsM;
        deq.rmsM[i] = v > 0 ? std::sqrt(v) : 0.0;
    }
    const BandFit fitDeq = fitBand(deq, true, bandLoM, bandHiM);

    if (missingCounter) {
        const uint64_t missed = missingCounter->load() - missBefore;
        if (missed > 0)
            std::printf("\n  *** %llu RASTER QUERIES FELL OUTSIDE THE LOADED TILE SET and were\n"
                        "      answered with the missing-tile default. Every number in this run\n"
                        "      is contaminated by an artificial cliff at the edge of coverage.\n"
                        "      Move the site inland of the loaded region or shrink --cal-n. ***\n",
                        (unsigned long long)missed);
        else
            std::printf("\n  coverage: every raster query landed on a loaded tile.\n");
    }

    const BandFit fitAbs = fitBand(src, false, bandLoM, bandHiM);
    const BandFit fitRms = fitBand(src, true, bandLoM, bandHiM);
    // The same fit with the FINEST lag dropped. The finest raster lag is where a
    // band-limited source rolls off — the diffusion decoder's own smoothing sits
    // exactly there — so if H moves much between these two fits, the extrapolated
    // target is being set by the one lag least entitled to set it.
    const BandFit fitAbsNoFinest = fitBand(src, false, bandLoM * 2.0, bandHiM);
    const BandFit fitRmsNoFinest = fitBand(src, true, bandLoM * 2.0, bandHiM);

    // A pure-ocean site leaves the land mask with nothing to average, so the fit
    // has no lags at all. Stop rather than emit a zero-slope "power law" and a
    // ladder solved against it: an all-zero table would be indistinguishable
    // from a measured result in a log file.
    if (!fitRms.valid || !fitAbs.valid) {
        std::printf("\n  *** NO USABLE LAGS. Only %lld of %zu lags carried any land stencils —\n"
                    "      this site is (near enough) all ocean, and there is no structure\n"
                    "      function to fit. Pick a land site, or pass --include-ocean if you\n"
                    "      genuinely mean to fit a sea surface. ***\n",
                    (long long)fitRms.nLags, src.lagM.size());
        return;
    }

    std::printf("\n  power-law fit over [%.3f, %.1f] m:\n", bandLoM, bandHiM);
    std::printf("    mean|.|  H = %.4f   C = %.5f   R^2 = %.5f   (%lld lags)\n", fitAbs.H,
                std::exp(fitAbs.logC), fitAbs.r2, (long long)fitAbs.nLags);
    std::printf("    rms      H = %.4f   C = %.5f   R^2 = %.5f   (%lld lags)\n", fitRms.H,
                std::exp(fitRms.logC), fitRms.r2, (long long)fitRms.nLags);
    std::printf("  same fit with the finest lag dropped ([%.3f, %.1f] m):\n", bandLoM * 2.0,
                bandHiM);
    std::printf("    mean|.|  H = %.4f   rms H = %.4f   (delta %+.4f / %+.4f)\n",
                fitAbsNoFinest.H, fitRmsNoFinest.H, fitAbsNoFinest.H - fitAbs.H,
                fitRmsNoFinest.H - fitRms.H);
    std::printf("\n  source elevation quantum (measured, GCD of first differences): %lld mm\n",
                (long long)quantMm);
    std::printf("    -> lag-independent S2 rms floor of q/sqrt(2) = %.4f m at EVERY lag\n",
                floorRmsM);
    std::printf("    -> fit with that floor removed in quadrature: rms H = %.4f (was %.4f)\n",
                fitDeq.H, fitRms.H);

    // --- the continuation target ------------------------------------------
    const double useH = forcedH > 0 ? forcedH : fitRms.H;
    const bool measuredBandEdge = pxM * 2.0 <= 7.5 * (1 + 1e-9);
    double logCrms = fitRms.logC;
    double logCabs = fitAbs.logC;
    if (forcedH > 0) {
        // Re-anchor on the band edge so a forced H still passes through the
        // measured point rather than through the fit's own intercept.
        const double anchorM = 7.5;
        const double sAnchor = forcedS2 > 0 ? forcedS2 : fitRms.at(anchorM);
        logCrms = std::log(sAnchor) - forcedH * std::log(anchorM);
        logCabs = std::log(forcedS2 > 0 ? forcedS2 * (fitAbs.at(anchorM) / fitRms.at(anchorM))
                                        : fitAbs.at(anchorM)) -
                  forcedH * std::log(anchorM);
    } else if (forcedS2 > 0) {
        logCrms = std::log(forcedS2) - useH * std::log(7.5);
        logCabs = std::log(forcedS2 * (fitAbs.at(7.5) / fitRms.at(7.5))) - useH * std::log(7.5);
    }
    auto targetRms = [&](double dM) { return std::exp(logCrms + useH * std::log(dM)); };
    auto targetAbs = [&](double dM) { return std::exp(logCabs + useH * std::log(dM)); };

    std::printf("\n  CONTINUATION TARGET  S2(d) = C * d^H  with H = %.4f%s\n", useH,
                forcedH > 0 ? "  (FORCED by --target-h)" : "  (measured)");
    std::printf("  %10s %14s %14s %14s\n", "lag (m)", "target abs (m)", "target rms (m)",
                "provenance");
    const double reportLags[] = {30.0, 15.0, 7.5, 3.75, 1.875, 1.6, 0.8, 0.4, 0.2};
    for (double dM : reportLags)
        std::printf("  %10.3f %14.5f %14.5f   %s\n", dM, targetAbs(dM), targetRms(dM),
                    dM >= pxM * 2.0 ? "measured band" : "EXTRAPOLATED");
    if (targetRms(7.5) < floorRmsM)
        std::printf("\n  *** THE 7.5 m TARGET (%.4f m rms) IS BELOW THE SOURCE'S OWN\n"
                    "      QUANTISATION FLOOR (%.4f m rms, from a %lld mm quantum). The raster\n"
                    "      cannot represent, let alone measure, roughness of that size. The\n"
                    "      extrapolation is therefore an extrapolation of a fit whose fine end\n"
                    "      is partly quantisation noise. Treat the amplitudes below as an UPPER\n"
                    "      BOUND on what the coarse tier can justify, not as a measurement. ***\n",
                    targetRms(7.5), floorRmsM, (long long)quantMm);
    if (!measuredBandEdge)
        std::printf("\n  *** THE BAND EDGE AT 7.5 m IS AN EXTRAPOLATION. The source raster's\n"
                    "      finest resolved lag is %.3f m (2 x pixel). Everything below that is\n"
                    "      the fitted power law continued, NOT data. A baked fine tier at\n"
                    "      1875 mm/px would make 7.5 m a measured number; run with --fine-dir\n"
                    "      when one exists and re-read this table. ***\n",
                    pxM * 2.0);

    if (!solve) {
        std::printf("\n=== VXC_TERRAINPROBE BAND FIT v1 ===\n");
        rowI("bandfit.pixel_size", (long long)pxMm, "mm");
        rowI("bandfit.lattice_n", (long long)calNPx, "px_per_axis");
        rowI("bandfit.land_mask", landOnly ? 1 : 0, "bool");
        row("bandfit.band_lo", bandLoM, "m");
        row("bandfit.band_hi", bandHiM, "m");
        row("bandfit.H_abs", fitAbs.H, "dimensionless");
        row("bandfit.H_rms", fitRms.H, "dimensionless");
        row("bandfit.r2_abs", fitAbs.r2, "dimensionless");
        row("bandfit.r2_rms", fitRms.r2, "dimensionless");
        row("bandfit.H_abs_drop_finest", fitAbsNoFinest.H, "dimensionless");
        rowI("bandfit.source_quantum", (long long)quantMm, "mm");
        row("bandfit.source_s2_floor_rms", floorRmsM, "m");
        row("bandfit.H_rms_dequantised", fitDeq.H, "dimensionless");
        row("bandfit.C_abs", std::exp(fitAbs.logC), "m_at_1m");
        row("bandfit.C_rms", std::exp(fitRms.logC), "m_at_1m");
        for (size_t i = 0; i < src.lagM.size(); ++i) {
            char k[96];
            std::snprintf(k, sizeof(k), "bandfit.s2_abs.lag_%.4gm", src.lagM[i]);
            row(k, src.absM[i], "m");
            std::snprintf(k, sizeof(k), "bandfit.s2_rms.lag_%.4gm", src.lagM[i]);
            row(k, src.rmsM[i], "m");
        }
        row("bandfit.target_abs_7m5", targetAbs(7.5), "m");
        row("bandfit.target_rms_7m5", targetRms(7.5), "m");
        row("bandfit.target_abs_0m2", targetAbs(0.2), "m");
        rowI("bandfit.band_edge_measured", measuredBandEdge ? 1 : 0, "bool");
        std::printf("=== END BAND FIT ===\n");
        return;
    }

    // ======================================================================
    // OCTAVE KERNELS AND THE SOLVE
    // ======================================================================
    std::printf("\n=== OCTAVE KERNELS =======================================================\n");
    std::printf("kernel lattice: %lld x %lld points, stride %lld voxels (%.2f m), domain %.1f m\n",
                (long long)kernL.n, (long long)kernL.n, (long long)kernL.strideVox,
                static_cast<double>(kernL.strideVox) * kVoxelSizeMm / 1000.0,
                static_cast<double>(kernL.n * kernL.strideVox) * kVoxelSizeMm / 1000.0);
    std::printf("reference amplitude: %.0f mm, so a kernel in metres is metres of S2 per metre\n"
                "of octave amplitude (dimensionless).\n",
                kKernelRefAmpMm);

    S2Curve kern[kLadderN];
    for (int i = 0; i < kLadderN; ++i) {
        const int64_t L = kLadderLatticeMm[i];
        const uint32_t ch = CH_DETAIL_OCTAVE_BASE + static_cast<uint32_t>(i);
        kern[i] = fieldS2(kernL, kCalLagsVox, [&](int64_t vx, int64_t vy) {
            return static_cast<double>(
                       valueNoise2Fade(seed, vx * kVoxelSizeMm, vy * kVoxelSizeMm, L, ch)) *
                   kKernelRefAmpMm / 32768.0;
        });
    }
    // The carrier's own S2 in the client band. Whatever it already supplies is
    // NOT the ladder's job, so the ladder's demand is the target with this
    // removed in quadrature.
    const S2Curve carrier = fieldS2(kernL, kCalLagsVox, [&](int64_t vx, int64_t vy) {
        return static_cast<double>(carrierMm(tiles, vx, vy));
    });

    std::printf("\n  %8s %11s %11s %11s %11s %13s\n", "lag (m)", "k[3200]", "k[1600]", "k[400]",
                "k[200]", "carrier rms(m)");
    for (size_t li = 0; li < kCalLagsVox.size(); ++li) {
        std::printf("  %8.2f", kern[0].lagM[li]);
        for (int i = 0; i < kLadderN; ++i) std::printf(" %11.6f", kern[i].rmsM[li]);
        std::printf(" %13.6f\n", carrier.rmsM[li]);
    }

    // COLLINEARITY. The design columns are k_i(d)^2 as functions of d; two
    // columns that point the same way are indistinguishable to the fit, and the
    // solve will split energy between them arbitrarily. Printed because the
    // honest reading of a solved amplitude depends on it.
    std::printf("\n  design-column cosine similarity (1.000 = indistinguishable to S2):\n");
    std::printf("  %10s", "");
    for (int i = 0; i < kLadderN; ++i) std::printf(" %8lld", (long long)kLadderLatticeMm[i]);
    std::printf("\n");
    for (int i = 0; i < kLadderN; ++i) {
        std::printf("  %10lld", (long long)kLadderLatticeMm[i]);
        for (int j = 0; j < kLadderN; ++j) {
            double dot = 0, ni = 0, nj = 0;
            for (size_t li = 0; li < kCalLagsVox.size(); ++li) {
                const double a = kern[i].rmsM[li] * kern[i].rmsM[li];
                const double b = kern[j].rmsM[li] * kern[j].rmsM[li];
                dot += a * b;
                ni += a * a;
                nj += b * b;
            }
            std::printf(" %8.3f", (ni > 0 && nj > 0) ? dot / std::sqrt(ni * nj) : 0.0);
        }
        std::printf("\n");
    }

    std::vector<double> tgtRms, tgtAbs, carRms;
    for (size_t li = 0; li < kCalLagsVox.size(); ++li) {
        tgtRms.push_back(targetRms(kern[0].lagM[li]));
        tgtAbs.push_back(targetAbs(kern[0].lagM[li]));
        carRms.push_back(carrier.rmsM[li]);
    }
    const double reachM = static_cast<double>(kLadderLatticeMm[0]) / 1000.0;
    const LadderSolve sol = solveLadder(kern[0].lagM, tgtRms, carRms, kern, reachM);
    const LadderSolve solAll = solveLadder(kern[0].lagM, tgtRms, carRms, kern, 1e9);

    std::printf("\n=== SOLVED LADDER AMPLITUDES =============================================\n");
    if (!sol.ok) {
        std::printf("  SOLVE FAILED: only %lld usable lags.\n", (long long)sol.nRows);
        return;
    }
    std::printf("  non-negative least squares over the %lld lags at or below the coarsest\n"
                "  lattice (%.2f m), relative-error weighting; RMS relative residual of the\n"
                "  MODEL = %.4f\n",
                (long long)sol.nRows, reachM, sol.resid);
    std::printf("  %10s %14s %14s %18s\n", "lattice mm", "exact mm", "rounded mm",
                "all-lags variant");
    for (int i = 0; i < kLadderN; ++i)
        std::printf("  %10lld %14.2f %14d %18d\n", (long long)kLadderLatticeMm[i], sol.ampMm[i],
                    sol.ampMmInt[i], solAll.ok ? solAll.ampMmInt[i] : -1);
    std::printf("  (the all-lags variant includes lags past %.2f m, where every column is flat\n"
                "   and the row constrains only the total — reported so the difference is\n"
                "   visible, NOT recommended. See the note above solveLadder.)\n",
                reachM);

    // VERIFICATION: synthesise the rounded ladder with the SAME integer
    // arithmetic evalSurface uses and measure it. This is what the residual
    // table below reports — not the model's prediction of it.
    int32_t ampInt[kLadderN];
    for (int i = 0; i < kLadderN; ++i) ampInt[i] = sol.ampMmInt[i];
    const S2Curve got = fieldS2(kernL, kCalLagsVox, [&](int64_t vx, int64_t vy) {
        const int64_t xMm = vx * kVoxelSizeMm, yMm = vy * kVoxelSizeMm;
        int64_t s = 0;
        for (int i = 0; i < kLadderN; ++i)
            s += valueNoise2Fade(seed, xMm, yMm, kLadderLatticeMm[i],
                                 CH_DETAIL_OCTAVE_BASE + static_cast<uint32_t>(i)) *
                 ampInt[i] / 32768;
        return static_cast<double>(s);
    });
    // Ladder + carrier, which is what the player would stand on.
    const S2Curve tot = fieldS2(kernL, kCalLagsVox, [&](int64_t vx, int64_t vy) {
        const int64_t xMm = vx * kVoxelSizeMm, yMm = vy * kVoxelSizeMm;
        int64_t s = 0;
        for (int i = 0; i < kLadderN; ++i)
            s += valueNoise2Fade(seed, xMm, yMm, kLadderLatticeMm[i],
                                 CH_DETAIL_OCTAVE_BASE + static_cast<uint32_t>(i)) *
                 ampInt[i] / 32768;
        return static_cast<double>(s + carrierMm(tiles, vx, vy));
    });

    std::printf("\n  PER-LAG RESIDUAL — measured on the synthesised integer ladder, not modelled\n");
    std::printf("  %8s %13s %13s %9s %13s %13s %9s\n", "lag (m)", "target rms", "got rms(+car)",
                "err %", "target abs", "got abs(+car)", "err %");
    for (size_t li = 0; li < kCalLagsVox.size(); ++li) {
        const double tr = tgtRms[li], ta = tgtAbs[li];
        const double gr = tot.rmsM[li], ga = tot.absM[li];
        std::printf("  %8.2f %13.5f %13.5f %8.1f%% %13.5f %13.5f %8.1f%%\n", kern[0].lagM[li], tr,
                    gr, tr > 0 ? (gr / tr - 1.0) * 100.0 : 0.0, ta, ga,
                    ta > 0 ? (ga / ta - 1.0) * 100.0 : 0.0);
    }
    std::printf("  (ladder alone, without the carrier, at 0.2 / 1.6 / 7.5 m rms: "
                "%.5f / %.5f / %.5f m)\n",
                got.rmsM[0], got.rmsM[7], got.rmsM.back());

    // THE CURRENCY CHOICE IS WORTH ~30% AND THE PLAN DOES NOT MAKE IT.
    //
    // rms/mean|.| is a shape statistic: 1.253 for a Gaussian, higher for a
    // heavy-tailed distribution. Real terrain's second differences ARE heavy
    // tailed — mostly smooth ground with occasional cliffs — while a sum of
    // value-noise octaves is near-Gaussian by the central limit theorem. So the
    // two distributions cannot be matched in both currencies at once: fitting
    // rms overshoots mean-abs by exactly this ratio, and fitting mean-abs
    // undershoots rms by it. Printed rather than silently chosen, because
    // "match the measured S2" is ambiguous by ~30% until someone picks one.
    {
        double srcShape = 0, gotShape = 0;
        int64_t ns = 0, ng = 0;
        for (size_t i = 0; i < src.lagM.size(); ++i)
            if (src.absM[i] > 0) {
                srcShape += src.rmsM[i] / src.absM[i];
                ++ns;
            }
        for (size_t i = 0; i < got.lagM.size(); ++i)
            if (got.absM[i] > 0) {
                gotShape += got.rmsM[i] / got.absM[i];
                ++ng;
            }
        if (ns && ng) {
            srcShape /= static_cast<double>(ns);
            gotShape /= static_cast<double>(ng);
            std::printf("\n  DISTRIBUTION SHAPE (rms / mean|.|; 1.253 = Gaussian):\n");
            std::printf("    source raster : %.3f   (heavy-tailed: smooth ground plus cliffs)\n",
                        srcShape);
            std::printf("    solved ladder : %.3f   (near-Gaussian: a sum of noise octaves)\n",
                        gotShape);
            std::printf("    -> the two CANNOT match in both currencies. This fit matched RMS,\n"
                        "       so mean|.| overshoots by about %.0f%%. Fitting mean|.| instead\n"
                        "       would scale every amplitude above by %.3f.\n",
                        (srcShape / gotShape - 1.0) * 100.0, gotShape / srcShape);
        }
    }

    double envelope = 0;
    for (int i = 0; i < kLadderN; ++i)
        envelope += 32767.0 * static_cast<double>(sol.ampMmInt[i]) / 32768.0;
    std::printf("\n  // === PASTE INTO amplifier.cpp — fine-tier continuation ladder ===\n");
    std::printf("  // Solved by vxc_terrainprobe --calibrate against %s.\n", sourceName);
    std::printf("  // Target: S2(d) = %.5f * d^%.4f (rms, metres), %s.\n", std::exp(logCrms), useH,
                measuredBandEdge ? "band edge MEASURED" : "band edge EXTRAPOLATED from the "
                                                          "coarse tier");
    std::printf("  constexpr Octave kFineDetailOctaves[] = {\n");
    for (int i = 0; i < kLadderN; ++i)
        std::printf("      {%4lld, %4d},\n", (long long)kLadderLatticeMm[i], sol.ampMmInt[i]);
    std::printf("  };\n");
    std::printf("  // ungated detail envelope from this table: %.0f mm\n", envelope);

    // ======================================================================
    // CURVATURE GATE — a RATIO, not an amplitude
    // ======================================================================
    const CurvGateCal cg = calibrateCurvatureGate(tiles, fieldL, curvRatioTarget);
    std::printf("\n=== CURVATURE GATE =======================================================\n");
    std::printf("  measured carrier curvature over %lld points, %.4f m baseline (the source\n"
                "  raster's own pixel size — the gate reads the carrier, so this IS its input)\n",
                (long long)cg.nPts, cg.baselineM);
    std::printf("  distribution (mm/m^2, +ve = concave hollow):  p05=%.4f  tercile_lo=%.4f  "
                "tercile_hi=%.4f  p95=%.4f\n",
                cg.p05, cg.terLo, cg.terHi, cg.p95);
    std::printf("  current constants: min=%.2fx  max=%.2fx  knee=%.3f mm/m^2\n",
                static_cast<double>(kCurvatureScaleMinQ10) / 1024.0,
                static_cast<double>(kCurvatureScaleMaxQ10) / 1024.0,
                static_cast<double>(kCurvatureKneeQ10) / static_cast<double>(kCurveQ10One));
    std::printf("    mean gain: convex=%.4f  planar=%.4f  concave=%.4f  ->  convex/concave = "
                "%.3f\n",
                cg.gConvex, cg.gPlanar, cg.gConcave, cg.ratioNow);
    std::printf("    saturated ceiling (max/min) = %.3f; a smooth gate can only approach it\n",
                cg.ratioCeiling);
    std::printf("  KNEE SWEEP — what each knee buys on this measured distribution:\n");
    std::printf("    %14s %16s %14s\n", "knee (mm/m^2)", "convex/concave", "rms gain");
    for (int s = 0; s < CurvGateCal::kSweep; ++s)
        std::printf("    %14.3f %16.3f %14.4f\n", cg.sweepKnee[s], cg.sweepRatio[s],
                    cg.sweepRmsGain[s]);
    if (cg.kneeSolved) {
        std::printf("    knee that yields convex/concave = %.2f on THIS distribution: %.5f "
                    "mm/m^2  (achieved %.3f)\n",
                    curvRatioTarget, cg.kneeForTarget, cg.ratioAtSolvedKnee);
        // A knee far inside the measured spread is a step function wearing a
        // ramp's clothes: essentially every sample saturates, so the gain field
        // becomes two-valued and its edges are the same class of artifact this
        // project exists to remove. Say so rather than let the number stand.
        const double spread = cg.terHi - cg.terLo;
        if (spread > 0 && cg.kneeForTarget < spread * 0.1)
            std::printf("    *** DEGENERATE: that knee is %.1f%% of the tercile spread (%.4f\n"
                        "        mm/m^2), so the gate saturates almost everywhere and is a STEP\n"
                        "        in practice. The target %.2f is not reachable by a gate that is\n"
                        "        still a ramp. Pick a ratio from the sweep instead. ***\n",
                        100.0 * cg.kneeForTarget / spread, spread, curvRatioTarget);
    } else {
        std::printf("    NO KNEE reaches convex/concave = %.2f on this distribution: the target\n"
                    "    is at or above the saturated ceiling %.3f, which only a step gate hits.\n",
                    curvRatioTarget, cg.ratioCeiling);
    }
    std::printf("  rms gain over the whole distribution: %.4f\n", cg.rmsGain);
    std::printf("    -> gating the ladder multiplies its S2 by this, so the amplitudes above\n"
                "       must be DIVIDED by it to keep global S2 on target:\n");
    for (int i = 0; i < kLadderN; ++i)
        std::printf("       {%4lld, %4d}  (was %d)\n", (long long)kLadderLatticeMm[i],
                    static_cast<int>(std::llround(static_cast<double>(sol.ampMmInt[i]) /
                                                  (cg.rmsGain > 0 ? cg.rmsGain : 1.0))),
                    sol.ampMmInt[i]);
    // WILL THIS KNEE TRANSFER TO THE FINE TIER? Partly, and the two effects have
    // to be separated or the answer is wrong by an order of magnitude.
    //
    //   (a) The PURELY GEOMETRIC part -- curvature measured over a shorter
    //       baseline is larger by (b_fine/b_coarse)^-2 -- is already removed:
    //       carrierCurvatureTierNormQ10 divides it out, and this block feeds the
    //       gate the normalised value. So the knee IS tier-invariant against
    //       that, which is exactly what that function was added for.
    //
    //   (b) The SPECTRAL part is NOT removed and cannot be. A 1.875 m raster
    //       resolves real relief the 30 m raster never carried, so its
    //       normalised curvature distribution is genuinely WIDER, by roughly
    //       (1.875/pxM)^(H-2) / (1.875/pxM)^-2 = (1.875/pxM)^H if the measured
    //       power law continues. That is a statement about the DATA, not about
    //       units, and only a baked fine tier settles it.
    const double geomFactor = std::pow(1.875 / pxM, -2.0);
    const double spectralFactor = std::pow(1.875 / pxM, useH);
    std::printf("  transfer to the 1.875 m fine tier:\n");
    std::printf("    geometric factor %.1fx  -- ALREADY REMOVED by carrierCurvatureTierNormQ10\n",
                geomFactor);
    std::printf("    spectral factor  %.3fx  -- NOT removed: the fine raster carries relief the\n"
                "                              coarse one does not, so its normalised curvature\n"
                "                              distribution is genuinely different\n",
                spectralFactor);
    if (cg.kneeSolved)
        std::printf("    -> predicted fine-tier knee ~ %.4f mm/m^2 (tier-normalised).\n"
                    "       PREDICTED from the measured H, not measured; re-run with --fine-dir.\n",
                    cg.kneeForTarget * spectralFactor);

    // ======================================================================
    // RILL — constrained by anisotropy, NOT by S2
    // ======================================================================
    const std::vector<int64_t> anisoLagPx = {1, 2, 4};
    const RasterAniso ra = rasterAnisotropy(tiles, px0, py0, calNPx, anisoLagPx, 20.0);
    std::printf("\n=== RILL / FLUTE TERM ====================================================\n");
    std::printf("  source-raster anisotropy on >=%.0f%% grades (%lld points) — the only\n"
                "  empirical anchor this data offers, and it is at the LANDFORM scale:\n",
                ra.minGradePct, (long long)ra.nPts);
    std::printf("  %10s %13s %13s %10s\n", "lag (m)", "along rms(m)", "across rms(m)", "acr/alo");
    for (size_t li = 0; li < ra.lagM.size(); ++li)
        std::printf("  %10.2f %13.5f %13.5f %10.3f\n", ra.lagM[li], ra.along[li], ra.across[li],
                    ra.ratio[li]);

    // Along/across S2 of the unit rill field and of the solved ladder, on steep
    // ground only (the gate is shut elsewhere, so measuring elsewhere would
    // average the signal away with zeros).
    const std::vector<int64_t> rillLags = {16, 32}; // 1.6 m, 3.2 m
    double rAlong[2] = {0, 0}, rAcross[2] = {0, 0}, lAlong[2] = {0, 0}, lAcross[2] = {0, 0};
    int64_t rillPts = 0;
    {
        long double ra2[2] = {0, 0}, rc2[2] = {0, 0}, la2[2] = {0, 0}, lc2[2] = {0, 0};
        int64_t cnt = 0;
        for (int64_t j = 0; j < fieldL.n; ++j)
            for (int64_t i = 0; i < fieldL.n; ++i) {
                const int64_t x = fieldL.vx0 + i * fieldL.strideVox;
                const int64_t y = fieldL.vy0 + j * fieldL.strideVox;
                const ProbeCarrier c = evalProbeCarrier(tiles, x, y);
                const double gx = static_cast<double>(c.sxMmPerPx) * 1000.0 /
                                  static_cast<double>(pxMm);
                const double gy = static_cast<double>(c.syMmPerPx) * 1000.0 /
                                  static_cast<double>(pxMm);
                const double gl = std::sqrt(gx * gx + gy * gy);
                if (gl < static_cast<double>(kRillGateFullMmPerM)) continue; // gate not fully open
                ++cnt;
                const double ux = gx / gl, uy = gy / gl, vx = -uy, vy = ux;
                // The rill field must be evaluated with the gradient AT each
                // sample point, not at the stencil centre: it is a point
                // function of position and local gradient, and freezing the
                // gradient would measure a different field from the one that
                // ships.
                auto rillAt = [&](int64_t ax, int64_t ay) {
                    const ProbeCarrier cc = evalProbeCarrier(tiles, ax, ay);
                    return static_cast<double>(
                        rillMm(seed, ax * kVoxelSizeMm, ay * kVoxelSizeMm,
                               cc.sxMmPerPx * 1000 / pxMm, cc.syMmPerPx * 1000 / pxMm));
                };
                auto ladderAt = [&](int64_t ax, int64_t ay) {
                    int64_t s = 0;
                    for (int q = 0; q < kLadderN; ++q)
                        s += valueNoise2Fade(seed, ax * kVoxelSizeMm, ay * kVoxelSizeMm,
                                             kLadderLatticeMm[q],
                                             CH_DETAIL_OCTAVE_BASE + static_cast<uint32_t>(q)) *
                             ampInt[q] / 32768;
                    return static_cast<double>(s);
                };
                for (int li = 0; li < 2; ++li) {
                    const double d = static_cast<double>(rillLags[static_cast<size_t>(li)]);
                    auto s2 = [&](auto&& f, double dx, double dy) {
                        const int64_t ox = static_cast<int64_t>(std::lround(dx * d));
                        const int64_t oy = static_cast<int64_t>(std::lround(dy * d));
                        const double v = f(x + ox, y + oy) + f(x - ox, y - oy) - 2 * f(x, y);
                        return v * v;
                    };
                    ra2[li] += s2(rillAt, ux, uy);
                    rc2[li] += s2(rillAt, vx, vy);
                    la2[li] += s2(ladderAt, ux, uy);
                    lc2[li] += s2(ladderAt, vx, vy);
                }
            }
        rillPts = cnt;
        for (int li = 0; li < 2; ++li) {
            if (!cnt) continue;
            const long double q = static_cast<long double>(cnt);
            // Normalised to the reference amplitude so the rill numbers are a
            // kernel, comparable with the octave kernels above.
            const double sc = kKernelRefAmpMm / static_cast<double>(kRillAmplitudeMm) / 1000.0;
            rAlong[li] = std::sqrt(static_cast<double>(ra2[li] / q)) * sc;
            rAcross[li] = std::sqrt(static_cast<double>(rc2[li] / q)) * sc;
            lAlong[li] = std::sqrt(static_cast<double>(la2[li] / q)) / 1000.0;
            lAcross[li] = std::sqrt(static_cast<double>(lc2[li] / q)) / 1000.0;
        }
    }
    std::printf("\n  measured on %lld lattice points with the gate FULLY open (grade >= %lld%%):\n",
                (long long)rillPts, (long long)(kRillGateFullMmPerM / 10));
    std::printf("  %8s %13s %13s %9s %13s %13s\n", "lag (m)", "rill k along", "rill k across",
                "rill acr/alo", "ladder along", "ladder across");
    for (int li = 0; li < 2; ++li)
        std::printf("  %8.2f %13.6f %13.6f %9.3f %13.6f %13.6f\n",
                    static_cast<double>(rillLags[static_cast<size_t>(li)]) * kVoxelSizeMm / 1000.0,
                    rAlong[li], rAcross[li], rAlong[li] > 0 ? rAcross[li] / rAlong[li] : 0.0,
                    lAlong[li], lAcross[li]);
    if (rillPts > 0) {
        // Composite anisotropy of ladder + rill at amplitude A:
        //   R^2 = (Lacr^2 + w Racr^2) / (Lalo^2 + w Ralo^2),   w = (A/1000)^2
        // which inverts in closed form. This is the calibration S2 CANNOT do:
        // the rill and the 1600 mm octave are the same column to S2, and only
        // the direction-resolved statistic separates them.
        auto solveRill = [&](double R, const char* provenance) {
            const double R2 = R * R;
            const double num = R2 * lAlong[0] * lAlong[0] - lAcross[0] * lAcross[0];
            const double den = rAcross[0] * rAcross[0] - R2 * rAlong[0] * rAlong[0];
            if (den <= 0)
                std::printf("    target %.3f (%s): UNREACHABLE — the rill term's own anisotropy\n"
                            "      is %.3f, below the target. Raise kRillElongation or\n"
                            "      kRillSectors, or lower the target.\n",
                            R, provenance, rAlong[0] > 0 ? rAcross[0] / rAlong[0] : 0.0);
            else if (num <= 0)
                std::printf("    target %.3f (%s): already met at ZERO rill amplitude (the\n"
                            "      ladder alone reads %.3f).\n",
                            R, provenance, lAlong[0] > 0 ? lAcross[0] / lAlong[0] : 0.0);
            else
                std::printf("    target %.3f (%s): kRillAmplitudeMm = %lld mm\n", R, provenance,
                            (long long)std::llround(std::sqrt(num / den) * kKernelRefAmpMm));
        };
        std::printf("\n  amplitude solved from the ANISOTROPY target at the 1.6 m lag\n"
                    "  (S2 alone cannot do this — see the collinearity table). Currently "
                    "%lld mm, PROVISIONAL:\n",
                    (long long)kRillAmplitudeMm);
        solveRill(rillRatioTarget, "--rill-aniso, a CHOICE not a measurement");
        if (!ra.ratio.empty() && ra.ratio[0] > 1.0)
            solveRill(ra.ratio[0], "the source raster's OWN landform anisotropy, measured");
    }

    // ======================================================================
    // BEDDING — quasi-periodic, keyed on elevation, no fixed lag
    // ======================================================================
    const S2Curve bed = fieldS2(kernL, kCalLagsVox, [&](int64_t vx, int64_t vy) {
        return static_cast<double>(beddingMm(seed, vx * kVoxelSizeMm, vy * kVoxelSizeMm,
                                             amp.surfaceMm(vx, vy)));
    });
    std::printf("\n=== BEDDING TERM =========================================================\n");
    std::printf("  S2 the CURRENT %lld mm amplitude actually produces on this terrain, and what\n"
                "  fraction of the continuation budget it consumes at each lag:\n",
                (long long)kBeddingAmpMm);
    std::printf("  %8s %13s %13s %12s\n", "lag (m)", "bedding rms", "target rms", "share of "
                                                                                  "budget");
    for (size_t li = 0; li < kCalLagsVox.size(); ++li) {
        const double t = tgtRms[li];
        std::printf("  %8.2f %13.6f %13.6f %11.1f%%\n", bed.lagM[li], bed.rmsM[li], t,
                    t > 0 ? 100.0 * bed.rmsM[li] * bed.rmsM[li] / (t * t) : 0.0);
    }
    std::printf("  (share is in ENERGY, i.e. squared, because that is what adds. A term at 20%%\n"
                "   of the budget costs the ladder only ~10%% of its amplitude.)\n");
    // THE ONE THING S2 GENUINELY SAYS ABOUT THIS TERM: an UPPER BOUND. Whatever
    // else the bedding is for, at no lag may it alone exceed the continuation
    // budget, because the budget is the total and every other term is additive
    // on top. That bound is a real constraint and is not a value — a term at
    // half the bound and a term at a tenth of it are both admissible to S2 and
    // are told apart only by whether the layering reads.
    double worstShare = 0;
    double worstLagM = 0;
    for (size_t li = 0; li < kCalLagsVox.size(); ++li) {
        if (tgtRms[li] <= 0) continue;
        const double sh = bed.rmsM[li] / tgtRms[li];
        if (sh > worstShare) {
            worstShare = sh;
            worstLagM = bed.lagM[li];
        }
    }
    if (worstShare > 0)
        std::printf("\n  HARD UPPER BOUND from S2 alone: the term peaks at %.2fx the budget at\n"
                    "  the %.2f m lag, so kBeddingAmpMm <= %lld mm (currently %lld mm) merely to\n"
                    "  stay inside the total. That is a CEILING, not a calibration: S2 cannot\n"
                    "  distinguish this term from an isotropic octave of the same energy, and\n"
                    "  the number that decides whether bedding is right is whether the layering\n"
                    "  reads as layering.\n",
                    worstShare, worstLagM,
                    (long long)std::llround(static_cast<double>(kBeddingAmpMm) / worstShare),
                    (long long)kBeddingAmpMm);

    // --- greppable rows ---------------------------------------------------
    std::printf("\n=== VXC_TERRAINPROBE CALIBRATION v1 ===\n");
    rowI("cal.pixel_size", (long long)pxMm, "mm");
    rowI("cal.band_edge_measured", measuredBandEdge ? 1 : 0, "bool");
    row("cal.H_used", useH, "dimensionless");
    row("cal.H_fit_abs", fitAbs.H, "dimensionless");
    row("cal.H_fit_rms", fitRms.H, "dimensionless");
    row("cal.r2_fit_rms", fitRms.r2, "dimensionless");
    rowI("cal.source_quantum", (long long)quantMm, "mm");
    row("cal.source_s2_floor_rms", floorRmsM, "m");
    row("cal.H_fit_rms_dequantised", fitDeq.H, "dimensionless");
    row("cal.target_rms_7m5", targetRms(7.5), "m");
    row("cal.target_abs_7m5", targetAbs(7.5), "m");
    row("cal.solve_model_residual", sol.resid, "rel_rms");
    rowI("cal.solve_lags", (long long)sol.nRows, "count");
    for (int i = 0; i < kLadderN; ++i) {
        char k[96];
        std::snprintf(k, sizeof(k), "cal.ladder.amp_%lldmm", (long long)kLadderLatticeMm[i]);
        rowI(k, (long long)sol.ampMmInt[i], "mm");
        std::snprintf(k, sizeof(k), "cal.ladder.amp_exact_%lldmm", (long long)kLadderLatticeMm[i]);
        row(k, sol.ampMm[i], "mm");
    }
    row("cal.ladder.envelope", envelope, "mm");
    for (size_t li = 0; li < kCalLagsVox.size(); ++li) {
        char k[96];
        std::snprintf(k, sizeof(k), "cal.resid.rel_rms.lag_%.4gm", kern[0].lagM[li]);
        row(k, tgtRms[li] > 0 ? tot.rmsM[li] / tgtRms[li] - 1.0 : 0.0, "fraction");
    }
    rowI("cal.curv.points", (long long)cg.nPts, "count");
    row("cal.curv.baseline", cg.baselineM, "m");
    row("cal.curv.tercile_lo", cg.terLo, "mm/m2");
    row("cal.curv.tercile_hi", cg.terHi, "mm/m2");
    row("cal.curv.ratio_now", cg.ratioNow, "dimensionless");
    row("cal.curv.ratio_ceiling", cg.ratioCeiling, "dimensionless");
    row("cal.curv.knee_for_target", cg.kneeSolved ? cg.kneeForTarget : -1.0, "mm/m2_-1_if_none");
    row("cal.curv.target_ratio", curvRatioTarget, "dimensionless");
    row("cal.curv.rms_gain", cg.rmsGain, "dimensionless");
    row("cal.curv.fine_tier_spectral_factor", spectralFactor, "dimensionless_geom_part_removed");
    rowI("cal.rill.points", (long long)rillPts, "count");
    row("cal.rill.kernel_aniso_1m6", rAlong[0] > 0 ? rAcross[0] / rAlong[0] : 0.0,
        "dimensionless");
    row("cal.rill.raster_aniso_landform", ra.ratio.empty() ? 0.0 : ra.ratio[0], "dimensionless");
    row("cal.bedding.share_1m6", tgtRms[7] > 0 ? bed.rmsM[7] * bed.rmsM[7] /
                                                     (tgtRms[7] * tgtRms[7])
                                               : 0.0,
        "energy_fraction");
    row("cal.bedding.peak_share", worstShare, "amplitude_ratio");
    rowI("cal.bedding.upper_bound_amp",
         worstShare > 0
             ? (long long)std::llround(static_cast<double>(kBeddingAmpMm) / worstShare)
             : -1,
         "mm_ceiling_not_a_value");
    std::printf("=== END CALIBRATION ===\n");
}

} // namespace

int main(int argc, char** argv) {
    // Options are stripped out first so the positional grammar is untouched;
    // every pre-existing command line keeps working byte-for-byte.
    bool optBaseline = false, optStructure = true;
    int64_t drainN = 384, drainStride = 10, fieldN = 96, fieldStride = 25;
    // Calibration mode. Off unless asked for, and when asked for it REPLACES the
    // legacy report and the structure metrics rather than adding to them: it is
    // a different question about the same world, and every pre-existing command
    // line must keep producing byte-identical output.
    bool optBandFit = false, optCalibrate = false, optIncludeOcean = false;
    int64_t calNPx = 384, kernN = 192, kernStride = 7;
    double bandLoM = 0, bandHiM = 960.0;
    double targetH = 0, targetS2 = 0, curvRatio = 3.5, rillRatio = 1.30;
    std::string fineDir;
    std::vector<char*> pos;
    pos.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        char* a = argv[i];
        auto wantD = [&](const char* name, double& dst, double lo, double hi) {
            if (std::strcmp(a, name) != 0) return false;
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                std::exit(2);
            }
            dst = std::strtod(argv[++i], nullptr);
            if (!(dst >= lo && dst <= hi)) {
                std::fprintf(stderr, "%s %g out of range (%g..%g)\n", name, dst, lo, hi);
                std::exit(2);
            }
            return true;
        };
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
        } else if (std::strcmp(a, "--band-fit") == 0) {
            optBandFit = true;
        } else if (std::strcmp(a, "--calibrate") == 0) {
            optCalibrate = true;
        } else if (std::strcmp(a, "--include-ocean") == 0) {
            optIncludeOcean = true;
        } else if (std::strcmp(a, "--fine-dir") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--fine-dir needs a value\n");
                return 2;
            }
            fineDir = argv[++i];
        } else if (want("--drain-n", drainN, 8, 8192) ||
                   want("--drain-stride", drainStride, 1, 100000) ||
                   want("--field-n", fieldN, 2, 4096) ||
                   want("--field-stride", fieldStride, 1, 100000) ||
                   want("--cal-n", calNPx, 8, 8192) || want("--kern-n", kernN, 8, 4096) ||
                   want("--kern-stride", kernStride, 1, 10000)) {
            // consumed
        } else if (wantD("--target-h", targetH, 0.05, 2.0) ||
                   wantD("--target-s2", targetS2, 1e-6, 1e4) ||
                   wantD("--band-lo", bandLoM, 0.001, 1e6) ||
                   wantD("--band-hi", bandHiM, 0.001, 1e6) ||
                   wantD("--curv-ratio", curvRatio, 1.0001, 100.0) ||
                   wantD("--rill-aniso", rillRatio, 1.0001, 100.0)) {
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
    if (optCalibrate) optBandFit = true;
    if (optBandFit && optBaseline) {
        std::fprintf(stderr, "--baseline is the structure-metric table; it does not apply to "
                             "--band-fit/--calibrate\n");
        return 2;
    }

    if (argc < 5) {
        std::fprintf(
            stderr,
            "usage: vxc_terrainprobe <tiledir|--synthetic> <seed> <xM> <yM> [lenM] [pixelMm]\n"
            "       pixelMm is synthetic-only (default 30000; 3750 = scale 8, 1875 = scale 16)\n"
            "       [--baseline] [--drain-n N] [--drain-stride V] [--field-n N]\n"
            "       [--field-stride V] [--no-structure]\n"
            "       [--band-fit] [--calibrate] [--fine-dir DIR] [--include-ocean]\n"
            "       [--cal-n N] [--kern-n N] [--kern-stride V] [--band-lo M] [--band-hi M]\n"
            "       [--target-h H] [--target-s2 M] [--curv-ratio R] [--rill-aniso R]\n");
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

    // --- THE FINE TIER IS A PROPERTY OF THE WORLD, NOT OF ONE TABLE ----------
    //
    // This used to be loaded inside the --band-fit block, so `--fine-dir`
    // rebound only the sampler the CALIBRATION read and left the terrace,
    // drainage, structure and seam sections measuring the 30 m coarse tier. A
    // run could therefore print "fine tier" in one table and 30 m numbers in
    // every other one, which is precisely the mislabelled measurement this tool
    // exists to prevent -- and it hid the question that matters most, because on
    // a fine world the amplifier DELETES its two loudest octaves (25.6 m and
    // 6.4 m; see kFineDetailOctaves) and nothing downstream could see the
    // consequence.
    //
    // So the fine tier now rebinds `tiles` itself, before `amp` is constructed,
    // and every metric in the run measures the world the client would actually
    // evaluate. `fine` is declared in this scope so it outlives `amp`, which
    // holds a reference to it. The coarse sampler stays as the climate source:
    // a v2 fine tile carries elevation control points and a flow plane, not
    // climate, so biome and material still come from the 30 m raster.
    FineTileSampler fine(seed, tiles);
    bool fineLoaded = false;
    if (!fineDir.empty()) {
        int loaded = 0, rejected = 0;
        if (std::filesystem::exists(fineDir)) {
            for (auto& e : std::filesystem::directory_iterator(fineDir)) {
                if (e.path().extension() != ".vxtl") continue;
                if (fine.loadTileFile(e.path()))
                    ++loaded;
                else
                    ++rejected;
            }
        }
        if (!optBaseline)
            std::printf("fine tier: dir=%s loaded=%d rejected=%d pixelSizeMm=%d\n",
                        fineDir.c_str(), loaded, rejected, fine.pixelSizeMm());
        if (loaded == 0) {
            // Refuse rather than silently fall back: the whole point of naming a
            // fine tier is that the answer changes, and a run that quietly
            // extrapolated from 30 m while its header said "fine tier" is the
            // failure this tool exists to prevent.
            std::fprintf(stderr, "no v2 fine tiles loaded from %s; refusing to silently "
                                 "fall back to the coarse tier\n",
                         fineDir.c_str());
            return 1;
        }
        tiles = &fine;
        sourceLabel = "real .vxtl v2 FINE tier";
        fineLoaded = true;
    }

    Amplifier amp(seed, *tiles);

    const int64_t vx0 = x0M * 1000 / kVoxelSizeMm;
    const int64_t vy0 = y0M * 1000 / kVoxelSizeMm;
    const int64_t n = lenM * 1000 / kVoxelSizeMm;

    // --- CALIBRATION MODES ---------------------------------------------------
    //
    // A baked .vxtl v2 fine tier, when one exists, is the RIGHT source: it makes
    // the 7.5 m band edge a measured number instead of a two-decade
    // extrapolation from 30 m posts. It is loaded into its own sampler and its
    // own Amplifier, so the carrier measured under it is the fine-tier carrier
    // and not the coarse one. When it is absent the run continues on the coarse
    // tier and says so in every table that depends on it.
    if (optBandFit) {
        // The fine tier is loaded and bound above, for the whole run rather than
        // for this block, so there is nothing tier-specific left to do here:
        // `tiles` and `amp` ARE the fine ones when --fine-dir was given. Keeping
        // the calTiles/calAmp names avoids churning the body below, which reads
        // them in a dozen places.
        ITileSampler* calTiles = tiles;
        Amplifier* calAmp = &amp;
        std::string srcName =
            fineLoaded ? "real .vxtl v2 FINE tier (1875 mm/px)"
                       : (dir == "--synthetic" ? "synthetic tiles (30 m band)"
                                               : "real .vxtl v1 tiles (30 m/px coarse tier)");
        // Default the band's low end to the source's own Nyquist-ish floor: one
        // pixel. Stated in metres so it reads the same at either tier.
        const double loM = bandLoM > 0 ? bandLoM
                                       : static_cast<double>(calTiles->pixelSizeMm()) / 1000.0;
        const CalLattice kernL{vx0, vy0, kernN, kernStride};
        const CalLattice fieldL{vx0, vy0, fieldN, fieldStride};
        runCalibration(seed, *calTiles, *calAmp, vx0, vy0, calNPx, kernL, fieldL, !optIncludeOcean,
                       optCalibrate, loM, bandHiM, targetH, targetS2, curvRatio, rillRatio,
                       srcName.c_str(),
                       // Only TileGridSampler exposes a miss counter; the
                       // synthetic sampler cannot miss and the fine sampler
                       // does not carry one, so those runs go unguarded and the
                       // report says nothing rather than something false.
                       (calTiles == &grid) ? &grid.missingTileQueries : nullptr);
        return 0;
    }

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

    // PERIODICITY, at two cell sizes on purpose. A 0.1 m lattice resolves
    // everything down to the voxel but only spans 25.6 m; a 0.5 m lattice spans
    // 128 m and so can see a metre-to-decametre pattern that the tight window
    // clips. An artifact that appears at ONE cell size and not the other is a
    // sampling artifact of this instrument, not a property of the terrain -- which
    // is the failure mode a single window would hide.
    {
        const Raster pFine = sampleAmplifiedRaster(amp, vx0, vy0, 256, 1);
        reportPeriodicity(pFine, "AMPLIFIED SURFACE (0.1 m cells)");
        const Raster pWide = sampleAmplifiedRaster(amp, vx0, vy0, 256, 5);
        reportPeriodicity(pWide, "AMPLIFIED SURFACE (0.5 m cells)");
        const Raster cWide = sampleCarrierRaster(*tiles, vx0, vy0, 256, 5);
        reportPeriodicity(cWide, "CARRIER ONLY (0.5 m cells)");

        // TERRACE CROOKEDNESS at true voxel spacing, which is the only spacing at
        // which the quantisation artifact exists. Amplified and carrier-only, so the
        // client's contribution is separable from the bake's.
        const Raster tFine = sampleAmplifiedRaster(amp, vx0, vy0, 384, 1);
        reportContourStraightness(tFine, "AMPLIFIED SURFACE");
        const Raster tCar = sampleCarrierRaster(*tiles, vx0, vy0, 384, 1);
        reportContourStraightness(tCar, "CARRIER ONLY");
    }

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
        curvatureConditionedRoughness(amp, nullptr, vx0, vy0, fieldN, fieldStride, 50, curvLags);
    // The SAME metric binned on the carrier's analytic Laplacian — the quantity
    // the gate actually reads. Reported alongside rather than instead: the
    // disagreement between the two is the finding. See the block above
    // curvatureConditionedRoughness.
    const CurvBins cbc =
        curvatureConditionedRoughness(amp, tiles, vx0, vy0, fieldN, fieldStride, 50, curvLags);
    const GateCensus gc = gateCensus(*tiles, vx0, vy0, fieldN, fieldStride);
    // Capped at 64 per axis: this is a pure statistic of the noise fields, so a
    // 4096-point lattice already converges it, and the full field lattice would
    // pay five octaves x five lags x five evaluations for no extra precision.
    const BandShare bs = bandShare(seed, tiles->pixelSizeMm(), vx0, vy0,
                                   std::min<int64_t>(fieldN, 64), fieldStride, curvLags);
    // Rill isolation on steep ground only: below the term's own grade gate it is
    // identically zero by construction, so including flats would average the
    // signal away with exact zeros and report a smaller effect than exists.
    // Capped at 48 per axis because every point costs three field evaluations
    // per arm and each of those costs a carrier.
    const RillIso ri =
        rillIsolation(amp, *tiles, seed, vx0, vy0, std::min<int64_t>(fieldN, 48), fieldStride, 250,
                      {10, 20, 30}, 20.0);

    // 1, 2 and 3 m: the plan's stated rill band. Gradient baseline 250 voxels
    // (25 m), matching directionalRoughness above so the two are comparable.
    const std::vector<int64_t> rillLags = {10, 20, 30};
    const AnisoBins ab = rillAnisotropyByGrade(amp, vx0, vy0, fieldN, fieldStride, 250, rillLags);

    if (!optBaseline) {
        printDrainage(dCar, "CARRIER ONLY (30 m band; the control)");
        printDrainage(dAmp, "AMPLIFIED SURFACE (what is rendered)");
        printDrainage(dDet, "DETAIL ONLY (amplified - carrier; the sub-30 m band)");
        printCurvBins(cb, "AMPLIFIED SURFACE");
        printCurvBins(cbc, "AMPLIFIED SURFACE");
        printGateCensus(gc);
        printBandShare(bs);
        printAnisoBins(ab, "AMPLIFIED SURFACE");
        printRillIso(ri);
        std::printf("\n");
    }

    // --- the compact table -------------------------------------------------
    // v2 adds the carrier-binned curvature rows (curvc.*), the gate census
    // (gate.*) and the gated-band share (band.*). Every v1 field keeps its exact
    // name, units and value, so a v1 baseline still diffs against a v2 one for
    // the fields both carry; the version bump is what makes the ADDITIONS
    // visible rather than a silent widening of the contract.
    std::printf("=== VXC_TERRAINPROBE STRUCTURE BASELINE v2 ===\n");
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

    // v2: the same metric binned on the CARRIER's analytic Laplacian, which is
    // the quantity curvatureScaleQ10 receives. `curv.*` above bins on a 5 m
    // finite difference of the AMPLIFIED surface, whose terciles are two orders
    // of magnitude larger and are cut mostly by detail noise — so `curv.*` can
    // read 1.00 while the gate is working, and quoting it as evidence the gate
    // is dead is a mistake this pair exists to prevent.
    rowI("curvc.points", (long long)cbc.nPts, "count");
    row("curvc.tercile_lo", cbc.edgeLoMmPerM2, "mm/m2_tiernorm_neg_is_crest");
    row("curvc.tercile_hi", cbc.edgeHiMmPerM2, "mm/m2_tiernorm_neg_is_crest");
    for (size_t li = 0; li < cbc.lagsVox.size(); ++li) {
        char k[96];
        const double lm = static_cast<double>(cbc.lagsVox[li]) * kVoxelSizeMm / 1000.0;
        std::snprintf(k, sizeof(k), "curvc.ratio_convex_over_concave.lag_%.1fm", lm);
        row(k, cbc.s2[2][li] > 0 ? cbc.s2[0][li] / cbc.s2[2][li] : 0.0, "dimensionless");
    }

    // v2: the gate census. THIS is the direct answer to "is the gate live"; the
    // two ratio blocks above are indirect and both can be blind.
    rowI("gate.points", (long long)gc.nPts, "count");
    row("gate.knee", gc.kneeMmPerM2, "mm/m2");
    row("gate.carrier_mean_abs_curvature", gc.curveMeanAbs, "mm/m2");
    row("gate.carrier_mean_abs_curvature_tiernorm", gc.curveTierNormMeanAbs, "mm/m2_at_30m_ref");
    row("gate.knee_over_mean_curvature", gc.curveMeanAbs > 0 ? gc.kneeMmPerM2 / gc.curveMeanAbs
                                                             : -1.0,
        "ratio");
    row("gate.saturated", gc.satPct, "pct_of_points");
    row("gate.gain_min", gc.gMin, "x");
    row("gate.gain_p10", gc.gP10, "x");
    row("gate.gain_median", gc.gP50, "x");
    row("gate.gain_p90", gc.gP90, "x");
    row("gate.gain_max", gc.gMax, "x");
    row("gate.gain_mean", gc.gMean, "x");
    row("gate.gain_rms", gc.gRms, "x");
    row("gate.gain_within_5pct_of_unity", gc.inertPct, "pct_of_points_high_means_inert");

    // v2: which lags can carry the gate's signal at all. A conditioned ratio at
    // a lag whose gated share is 1% is arithmetically pinned near 1.00 whether
    // the gate works or not, so this table is a precondition for reading either
    // ratio block above rather than an extra.
    rowI("band.gated_octaves", (long long)bs.nGated, "count");
    rowI("band.fine_tier_table", bs.fine ? 1 : 0, "bool");
    for (size_t i = 0; i < bs.lagM.size(); ++i) {
        char k[96];
        std::snprintf(k, sizeof(k), "band.gated_energy_share.lag_%.1fm", bs.lagM[i]);
        row(k, bs.share[i], "fraction");
        std::snprintf(k, sizeof(k), "band.max_s2_change_from_gate.lag_%.1fm", bs.lagM[i]);
        row(k, bs.maxChangePct[i], "pct");
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

    // v2: rill isolation. `rill.*` above measures the COMPOSITE surface, where
    // the term is buried; these rows measure it alone, alongside the bed it is
    // buried in and the estimator's own noise floor, so "invisible" and "absent"
    // can be told apart.
    rowI("rilliso.points", (long long)ri.nPts, "count");
    row("rilliso.min_grade", ri.minGradePct, "pct");
    for (size_t li = 0; li < ri.lagM.size(); ++li) {
        char k[128];
        const double lm = ri.lagM[li];
        // Frame 1 is the carrier's analytic gradient — the vector the shipped
        // term is handed, hence the one a null result can be trusted from.
        std::snprintf(k, sizeof(k), "rilliso.carrierframe.rill_alone.lag_%.0fm", lm);
        row(k, ri.rill[1][li], "across_over_along");
        std::snprintf(k, sizeof(k), "rilliso.carrierframe.rest_alone.lag_%.0fm", lm);
        row(k, ri.rest[1][li], "across_over_along");
        std::snprintf(k, sizeof(k), "rilliso.carrierframe.composite.lag_%.0fm", lm);
        row(k, ri.comp[1][li], "across_over_along");
        std::snprintf(k, sizeof(k), "rilliso.carrierframe.control_45deg.lag_%.0fm", lm);
        row(k, ri.ctrl[1][li], "noise_floor");
        std::snprintf(k, sizeof(k), "rilliso.ampframe.composite.lag_%.0fm", lm);
        row(k, ri.comp[0][li], "across_over_along");
        std::snprintf(k, sizeof(k), "rilliso.rill_rms.lag_%.0fm", lm);
        row(k, ri.rillRmsM[li], "m");
        std::snprintf(k, sizeof(k), "rilliso.rest_rms.lag_%.0fm", lm);
        row(k, ri.restRmsM[li], "m");
        std::snprintf(k, sizeof(k), "rilliso.dilution.lag_%.0fm", lm);
        row(k, ri.rillRmsM[li] > 0 ? ri.restRmsM[li] / ri.rillRmsM[li] : -1.0,
            "rest_over_rill_amplitude");
        // The bias-cancelling statistic and its own error bar. THIS is the row
        // to quote for "is the rill term doing anything on real terrain".
        std::snprintf(k, sizeof(k), "rilliso.paired_delta.lag_%.0fm", lm);
        row(k, ri.paired[li], "ratio_points");
        std::snprintf(k, sizeof(k), "rilliso.paired_delta_err.lag_%.0fm", lm);
        row(k, ri.pairedSpread[li], "ratio_points_split_half");
        std::snprintf(k, sizeof(k), "rilliso.paired_sigma.lag_%.0fm", lm);
        row(k, ri.pairedSpread[li] > 0 ? std::abs(ri.paired[li]) / ri.pairedSpread[li] : -1.0,
            "sigma");
    }
    std::printf("=== END STRUCTURE BASELINE ===\n");
    return 0;
}
