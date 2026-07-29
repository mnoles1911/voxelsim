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
// test_amplifier.cpp already sweeps them adversarially, but ONLY over
// SyntheticTileSampler (four environments, pixel sizes 30000 and 11250 -- the
// file's own FOLLOW-UP note asks for more). Real diffusion tiles are a
// different shape of input: kilometres of near-flat ocean, and cliffs where the
// 30 m raster steps hard. v9's bound is a Lipschitz envelope around ONE centre
// evaluation, and its tightness depends on the footprint-to-relief ratio, so
// synthetic coverage does not transfer for free.
//
// Sampling can only ever MISS a violation, never invent one: every reported
// failure is a real counterexample.
void boundSweep(Amplifier& amp, int64_t vx0, int64_t vy0, int64_t spanVox, int32_t levels) {
    std::printf("\nADVERSARIAL BOUND SWEEP over real tiles\n");
    std::printf("  %5s %9s %10s %12s %12s %12s\n", "level", "chunks", "declined",
                "upper slack", "lower slack", "violations");
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
        std::printf("  %5d %9lld %10lld %10.2f m %10.2f m %12lld%s\n", level, (long long)n,
                    (long long)declined,
                    used ? static_cast<double>(slackHi / used) / 1000.0 : 0.0,
                    used ? static_cast<double>(slackLo / used) / 1000.0 : 0.0,
                    (long long)viol, viol ? "   <-- HOLE IN THE WORLD" : "");
        totalViol += viol;
    }
    std::printf("  worst case is a bound that is TOO TIGHT; %lld violation(s) total\n",
                (long long)totalViol);
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

    materialBoundaryAlignment(amp, vx0, vy0, n, pxMm, "AMPLIFIED SURFACE");

    // 6 levels covers the whole ring cascade the streamer uses.
    boundSweep(amp, vx0, vy0, n, 6);

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
