#pragma once
// ---------------------------------------------------------------------------
// THE CARRIER — a C2 uniform cubic B-spline over the tile raster, and its
// analytic first and second derivatives.
//
// Extracted verbatim from amplifier.cpp at kWorldGenVersion 9 (the value and
// slope halves are a PURE MOVE — bit-identical, no behaviour change); the
// curvature half below is new, for docs/terrain-amplification-plan.md §3c.
//
// WHY THIS REPLACED BILINEAR (kWorldGenVersion 9). Bilinear interpolation is
// C0 but not C1: the height is continuous across a tile-pixel boundary, the
// GRADIENT is not. A slope discontinuity is invisible in the height and glaring
// under directional light, which is exactly why the artifact survived every
// check that looked at h alone -- it is a lighting defect, not a height defect.
//
// Measured with vxc_terrainprobe's seam scan (S2 stencils split by whether they
// straddle a pixel boundary), on real tiles at kWorldGenVersion 8:
//
//   * carrier alone: straddle/interior ratio 8x at a 0.1 m lag rising to 950x
//     at 12.8 m, over an interior floor of 0.48 mm. Growth exactly linear in
//     lag over a zero floor is the signature of a slope step -- the interior of
//     a bilinear cell is EXACTLY planar, so S2 there is pure division rounding,
//     and every scrap of second difference lives on the cell boundary;
//   * amplified surface: 3.28 / 1.61 / 1.23 at 0.1 / 0.2 / 0.4 m lags, ~1.1
//     beyond. The detail octaves mask the crease at metre scale and DO NOT mask
//     it at voxel scale, which is precisely where a 10 cm voxel game is looked
//     at.
//
// Cubic B-spline rather than Catmull-Rom, for three reasons in order of weight:
//
//   1. THE BOUND SURVIVES STRUCTURALLY. B-spline weights are non-negative and
//      sum exactly to the denominator, so the carrier is a convex combination
//      of control points and min/max over the stencil bounds it -- the same
//      property surfaceBoundsMm already leaned on for the bilinear patch.
//      Catmull-Rom has negative weights; its overshoot is bounded but the bound
//      would need a slack term derived from adjacent control-point differences,
//      i.e. a whole new derivation in the one place an error is a hole in the
//      world.
//   2. C2, NOT MERELY C1. Catmull-Rom fixes the shading crease but its
//      CURVATURE still steps at every cell line. The Phase-3 detail work
//      conditions amplitude on curvature (curvatureScaleQ10 at the bottom of
//      this file), so a C1 carrier would reproduce this same artifact one
//      derivative down.
//   3. The approximation deficit is fixable where it is cheap. A B-spline does
//      not interpolate its control points, it approximates them.
//      *** THAT PARAGRAPH USED TO END "at 30 m posts that is far below the
//      detail band". IT WAS WRONG, AND MEASURED WRONG AT v13. *** See THE
//      PREFILTER below: on a 30 m world the deficit is 0.8 m of real diffusion
//      output at a node, which is larger than every microrelief octave put
//      together. The fine tier ships prefiltered control points
//      (docs/vxtl-v2-format.md 2) and never had the problem; the COARSE tier
//      had no such step until v13, and now does it on the client.
//
// FIXED POINT. The cell fraction is q10 (the fadeFractionMm convention from
// hash.h). Weights are exact integer numerators over 6*1024^3; the quadratic
// derivative weights over 2*1024^2; the linear second-derivative weights over
// 1024. Evaluation is TWO-STAGE SEPARABLE with an intermediate division, and
// that is forced rather than chosen: the exact tensor form (weights in fx, not
// in q10) needs a denominator of 36*pxMm^6, which at pxMm = 30000 overflows
// int64 by about ten orders of magnitude. The same is true of the curvature
// path -- see the OVERFLOW ANALYSIS block above evalCarrierCurvature, which
// works the numbers rather than assuming the value path's argument carries.
//
// The q10 quantization of the cell fraction is harmless: one tq step is
// pxMm/1024 = 29 mm of horizontal position at 30 m pixels, and adjacent voxel
// columns (100 mm apart) advance tq by ~3, so no two columns collapse onto the
// same weights.
//
// Every division below truncates toward zero -- plain C++ `/`, mirrored by
// truncDiv in worldgen.ush. NOT floorDiv: the bound lemma in amplifier.cpp is
// stated against truncation, and the two disagree on negative numerators.
// Divisions whose numerator can genuinely be negative are called out
// individually, because that is the exact operand pattern that diverged AMD vs
// NVIDIA (worldgen.ush:181-234) and the HLSL mirror must use truncDiv there
// rather than a bare `/`.
//
// MIRRORING. Everything here is a plain free function over int64_t: no
// templates, no std::, no recursion, no floats. It mirrors into
// voxel-core/shaders/worldgen.ush one-for-one.
// ---------------------------------------------------------------------------

#include "voxelcore/core.h"

namespace vxc {

inline constexpr int64_t kCarrierT = 1024; // q10 one
inline constexpr int64_t kCarrierValueDen = 6 * kCarrierT * kCarrierT * kCarrierT;
inline constexpr int64_t kCarrierSlopeDen = 2 * kCarrierT * kCarrierT;
// The SECOND derivative of a uniform cubic B-spline is a LINEAR B-spline over
// the second differences of the control points, and the linear basis is just
// {1-t, t} -- so the denominator is one factor of the q10 one, not three (the
// cubic value basis) or two (the quadratic slope basis). Written out as its own
// named constant rather than reusing kCarrierT so that the fixed-point scaling
// of the curvature path is auditable in one place, and so that a change of knot
// convention that alters it fails loudly.
inline constexpr int64_t kCarrierCurveDen = kCarrierT;

// Cubic B-spline basis at t = tq/1024, as exact integer numerators over
// kCarrierValueDen. Ordered for control points i-1, i, i+1, i+2.
struct CarrierW4 {
    int64_t w[4];
};
constexpr CarrierW4 carrierValueW(int64_t tq) {
    const int64_t u = kCarrierT - tq;
    const int64_t T = kCarrierT;
    return CarrierW4{{u * u * u,
                      3 * tq * tq * tq - 6 * tq * tq * T + 4 * T * T * T,
                      -3 * tq * tq * tq + 3 * tq * tq * T + 3 * tq * T * T + T * T * T,
                      tq * tq * tq}};
}

// Quadratic B-spline basis — the derivative of the cubic, applied to the THREE
// first differences of the four control points. Result is mm per pixel.
struct CarrierW3 {
    int64_t w[3];
};
constexpr CarrierW3 carrierSlopeW(int64_t tq) {
    const int64_t u = kCarrierT - tq;
    const int64_t T = kCarrierT;
    return CarrierW3{{u * u, -2 * tq * tq + 2 * T * tq + T * T, tq * tq}};
}

// Linear B-spline basis — the SECOND derivative of the cubic, applied to the
// TWO second differences of the four control points. Result is mm per pixel^2.
//
// DERIVATION, in the same q10 style as the two above. The cubic basis at
// t = tq/T is  b = { u^3, 3t^3-6t^2+4, -3t^3+3t^2+3t+1, t^3 } / 6  (with
// u = 1-t), so
//
//     b0'' = (1-t)        b1'' = (3t-2)        b2'' = (1-3t)        b3'' = t
//
// and, writing D0 = P1 - 2*P0 + P(-1) and D1 = P2 - 2*P1 + P0 for the two
// second differences of the stencil,
//
//     (1-t)*D0 + t*D1
//       = (1-t)P(-1) + (-2+2t+t)P0 + (1-t-2t)P1 + t*P2
//       = (1-t)P(-1) + (3t-2)P0 + (1-3t)P1 + t*P2
//       = S''(t).                                                (identity)
//
// So the weights are exactly {T - tq, tq} over kCarrierCurveDen = T. They are
// non-negative and sum EXACTLY to the denominator (swept at compile time
// below), which is the same convex-combination property the value and slope
// bases have -- so |S''| <= max |D_k| over the stencil, with no slack term.
struct CarrierW2 {
    int64_t w[2];
};
constexpr CarrierW2 carrierCurveW(int64_t tq) { return CarrierW2{{kCarrierT - tq, tq}}; }

// The three bases must all be non-negative and partition their denominator at
// every q10 fraction the evaluator can produce. amplifier.cpp's coupling (4b)
// asserts this for the value and slope bases because surfaceBoundsMm depends on
// it; the curvature basis gets the same treatment here, next to its derivation,
// because the "curvature is exactly zero on a planar field" property that the
// whole gate rests on is exactly `weights sum to the denominator` applied to a
// vector of zero second differences.
constexpr bool carrierCurveWeightsPartition() {
    for (int64_t tq = 0; tq <= kCarrierT; ++tq) {
        const CarrierW2 w = carrierCurveW(tq);
        if (w.w[0] < 0 || w.w[1] < 0) return false;
        if (w.w[0] + w.w[1] != kCarrierCurveDen) return false;
    }
    return true;
}
static_assert(carrierCurveWeightsPartition(),
              "the linear second-derivative basis must be non-negative and sum exactly to "
              "kCarrierCurveDen; both the curvature bound and the exact-zero-on-a-plane "
              "property depend on it");

// ---------------------------------------------------------------------------
// THE PREFILTER (kWorldGenVersion 13) — the coarse tier's missing step.
//
// WHAT WAS BROKEN. A uniform cubic B-spline APPROXIMATES its control points; at
// a knot it evaluates (c[-1] + 4c[0] + c[1]) / 6, not c[0]. Feeding raster
// SAMPLES straight in as control points therefore produces a surface that is
// systematically LOW-PASSED relative to the samples: exactly
//
//     carrier(node) - sample(node) = (Laplacian of the samples) / 6
//
// which is not a rounding error, it is a whole band. MEASURED at v12 with
// vxc_stagedump on the pinned 060b0c927ccc807e world, S0 raw tile against S2
// carrier at the 30 m nodes: mean |S0 - S2| = 815 mm on plains (-55,20) and
// 3222 mm on alpine (-5,15). That is real diffusion-model output thrown away
// before any of our own amplification runs, and it is larger than the entire
// microrelief band the client then synthesises to replace it.
//
// The fine tier never had this defect: `.vxtl` v2 ships PREFILTERED control
// points (docs/vxtl-v2-format.md 2, and tiles.h's ITileSampler comment says so
// in as many words). Only the 30 m s1 raster is samples. So the prefilter here
// is gated on the tier, and `carrierPrefiltersSamples` is that gate.
//
// WHY A TRUNCATED EXPONENTIAL AND NOT A NEUMANN SERIES. The exact inverse of
// the sampling operator B = [1,4,1]/6 is the two-sided exponential
// h[m] = sqrt(3) * r^|m| with r = -2 + sqrt(3) = -0.2679..., which is the
// standard Unser prefilter written as an FIR instead of as an IIR pass (an IIR
// needs the whole row, which a per-column client evaluator does not have).
// Truncating it at radius R and RENORMALISING so the weights sum exactly to the
// denominator keeps DC exact — a constant elevation field must map to itself,
// and an un-renormalised truncation gets that wrong by 5% at R=2, which would
// move sea level.
//
// The obvious alternative, a Neumann series I + (I-B) + (I-B)^2 + ..., is far
// worse per tap: its residual is e^(K+1) with e = (1-cos w)/3 reaching 2/3, so
// a 7-tap Neumann still leaves 20% of the deficit at Nyquist where a 7-tap
// exponential leaves 2.8%. Swept over the whole band (see the table below),
// max |1 - B*H|:
//
//     R = 1   43.8%      R = 4    0.74%
//     R = 2    9.5%      R = 5    0.20%
//     R = 3    2.8%      R = 6    0.054%
//
// R = 4 is shipped: it recovers 99.3% of the deficit for 12x12 raster reads per
// cell against the un-prefiltered 4x4, and the reads are memoised per cell on
// the CPU and are 144 buffer loads per column on the GPU. R = 5 buys 0.5% for
// another 44 reads and is not worth it; R = 3 gives back 2.8% of 3.2 m on
// alpine (90 mm), which is a voxel, and is.
//
// FIXED POINT. Weights are round(2^20 * r^m), so they are exact integers and
// the denominator is their exact sum — DC is exact BY CONSTRUCTION rather than
// by an error bound. Every division truncates toward zero (plain C++ `/`), and
// the numerators are routinely NEGATIVE (the odd taps are negative), so the
// HLSL mirror MUST use truncDiv here and not a bare `/`.
// ---------------------------------------------------------------------------

inline constexpr int64_t kCarrierPrefilterRadius = 4;
// round(2^20 * (-2+sqrt(3))^m) for m = 0..4. Generated, not typed: the sweep in
// carrierPrefilterIsUnitDc below re-checks the only property the bound needs.
inline constexpr int64_t kCarrierPrefilterW[kCarrierPrefilterRadius + 1] = {
    1048576, -280965, 75284, -20172, 5405,
};
inline constexpr int64_t carrierPrefilterDen() {
    int64_t d = kCarrierPrefilterW[0];
    for (int64_t m = 1; m <= kCarrierPrefilterRadius; ++m) d += 2 * kCarrierPrefilterW[m];
    return d;
}
inline constexpr int64_t kCarrierPrefilterDen = carrierPrefilterDen();
static_assert(kCarrierPrefilterDen == 607680,
              "the prefilter denominator is the exact sum of the weights; if it moved, the "
              "weight table moved, and a worldgen change is a kWorldGenVersion bump");
static_assert(kCarrierPrefilterDen > 0, "the prefilter denominator divides and must be positive");

// The raster span, in pixels either side of the cell's own px, that the
// prefiltered 4x4 control stencil reads. The stencil is px-1..px+2 and each
// control point convolves +/- R raw samples around itself.
// --- v16: THE HORIZONTAL CARRIER WARP ---------------------------------------
//
// WHAT IT FIXES, measured rather than assumed. The carrier is C2 and therefore very
// smooth by design; voxelising a smooth surface at 10 cm produces contour steps
// spaced 100 mm / local grade, which on gentle ground is METRES, running dead
// straight along the contour. Measured terrace runs at 1.2-3.7% local grade on
// three real sites: mean 0.48-0.55 m but p90 1.0-1.2 m and p99 1.9-2.4 m, and the
// eye reads the long runs. That is the "jump ridges in a straight line every metre
// or two" reported against the v15 survey.
//
// It is not any detail term, established by ablation with the shader mirrored so the
// GPU actually saw each change: the 1.6 m octave, the rill term, the bedding term,
// and finally the WHOLE ladder at 1/16 all left it intact, and dropping the fine
// tier left it intact too. Removing detail makes it WORSE, because sub-voxel
// dithering is what used to ragged the step edges -- and v14/v15's gradient cap
// took most of that away as the price of fixing drainage.
//
// WHY HORIZONTAL RATHER THAN MORE VERTICAL NOISE. Vertical noise is what the
// drainage cap forbids: moving a step edge needs about half a voxel at a wavelength
// short enough to vary along the step, and 50 mm over 400 mm is 0.125 of gradient
// against an allowance of 0.05 on 4% ground. Warping the EVALUATION POSITION moves
// the contour lines instead of adding relief: h(p + w(p)) has gradient
// grad(h)*(I + grad(w)), so 500 mm over a 4096 mm lattice changes the gradient
// MAGNITUDE by order 12% and preserves its DIRECTION. Straight contours become
// wandering ones for almost nothing in the quantity the cap bounds.
//
// TWO CONTRACTS IT TOUCHES, both of which bite silently if missed:
//
//   * THE READ WINDOW. The warp displaces which raster cell a column samples, so
//     every host's control-stencil window must dilate by carrierWarpPx or it
//     clamps at the edge and generates different terrain there without faulting.
//     tiles.h's kCarrierStencilLo/Hi derive from this.
//   * THE SURFACE BOUND. A column inside a footprint now reads the carrier up to
//     kCarrierWarpMaxMm OUTSIDE it, so surfaceBoundsMm must dilate its footprint
//     by the same amount before bounding. A bound that does not is a hole in the
//     world, which is why it is dilated at the entry rather than adjusted late.
//
// The warp does NOT apply to climate. Climate is a field on the tile grid, not on
// the carrier; SurfaceEval::px/py stay unwarped for that reason and the comment
// there records the divergence that taught it.
inline constexpr int64_t kCarrierWarpMaxMm = 500;
inline constexpr int64_t kCarrierWarpLatticeMm = 4096;
// v19: a SECOND, larger warp component. The v16 warp bends contours at its own
// 4 m wavelength, which is why step edges stopped being dead straight -- but a
// band RHYTHM spanning tens of metres is untouched by a wander whose coherence
// length is 4 m: adjacent contours wander together only over distances shorter
// than the rhythm the eye traces. This component is the same mechanism at
// landform scale: a 24.6 m lattice with 3.25 m of throw bends the whole band
// stack and locally dilates/compresses its spacing (the gradient magnitude
// modulation is amp/lattice ~ 13%, deliberately the same order as v16's
// 500/4096 = 12%, so the drainage argument carries over unchanged: direction
// is preserved, magnitude wobbles by order 13% per component). The throw is
// sized so the TOTAL (3750 mm) is exactly two fine pixels: at three the
// carrier stencil (+/-19) would overtake the cavern reach (19.4 px), which is
// the residency gate's dominant term -- see test_tilestreaming's read-margin
// test for why that boundary is load-bearing.
//
// Both contracts below (read window, surface bound) dilate by the SUM of the
// two throws -- kCarrierWarpTotalMaxMm -- because the components add.
inline constexpr int64_t kCarrierWarp2MaxMm = 3250;
inline constexpr int64_t kCarrierWarp2LatticeMm = 24576;
inline constexpr int64_t kCarrierWarpTotalMaxMm = kCarrierWarpMaxMm + kCarrierWarp2MaxMm;
// Ceiling division, so a finer tier cannot quietly under-dilate: 2 px at
// 1875 mm, 1 px at 30000 mm since the v19 second component.
constexpr int64_t carrierWarpPx(int64_t pxMm) {
    return (kCarrierWarpTotalMaxMm + pxMm - 1) / pxMm;
}

inline constexpr int64_t kCarrierPrefilterLo = -1 - kCarrierPrefilterRadius;
inline constexpr int64_t kCarrierPrefilterHi = 2 + kCarrierPrefilterRadius;
inline constexpr int64_t kCarrierPrefilterSpan = kCarrierPrefilterHi - kCarrierPrefilterLo + 1;
static_assert(kCarrierPrefilterSpan == 12, "the raw block is (4 + 2R)^2 = 12x12 at R = 4");

// A tier at or below this pitch ships PREFILTERED CONTROL POINTS on the wire and
// must not be prefiltered again. Same threshold, and the same reasoning, as
// amplifier.cpp's isFineTier: it is written against the BAND the raster
// resolves rather than against one blessed scale. Kept here rather than
// imported from amplifier.cpp because the carrier is what consumes it and
// stagedump/terrainprobe need the identical answer.
inline constexpr int64_t kCarrierSampleTierMinPixelMm = 3751;
constexpr bool carrierPrefiltersSamples(int64_t pxMm) {
    return pxMm >= kCarrierSampleTierMinPixelMm;
}
static_assert(!carrierPrefiltersSamples(1875) && !carrierPrefiltersSamples(3750),
              "the baked fine tiers already carry control points");
static_assert(carrierPrefiltersSamples(30000), "the 30 m s1 raster carries SAMPLES");

// Build the 4x4 control stencil from the (4+2R)^2 raw sample block.
//
// `raw` is row-major with j (y) outer and i (x) inner, kCarrierPrefilterSpan on
// each axis; raw[a + span*b] is the sample at pixel
// (px + kCarrierPrefilterLo + a, py + kCarrierPrefilterLo + b). `cp` comes out
// in exactly the layout evalCarrier and evalCarrierCurvature already take.
//
// SEPARABLE, TWO STAGE, for the same reason the evaluator is: the exact 2-D
// tensor form would carry kCarrierPrefilterDen^2 = 3.7e11 on a 9e6 mm control
// point, and the intermediate divide keeps every product inside int64 with room
// to spare (worst stage-1 numerator 9e6 * sum|w| = 1.6e13, worst stage-2
// 2.8e7 * sum|w| = 5.1e13, against int64's 9.2e18).
//
// THE OUTPUT IS CLAMPED to the same elevation range coupling (4c) in
// amplifier.cpp already assumes of a control point. The prefilter is a
// SHARPENING filter with a gain of up to 2.98 per axis, so an adversarial
// Nyquist checkerboard at the full +/-9000 m elevation range could produce a
// control point ~8.9x out of range and tighten the carrier's own overflow
// margins from 20x to 2.2x. Clamping costs two compares per control point and
// keeps every static_assert in this file true VERBATIM instead of re-derived
// against a number nobody will re-check. It cannot fire on real terrain: it
// needs a 9 km second difference between adjacent 30 m posts.
// The magnitude every control point is clamped to. It is the SAME number the
// overflow analyses in this file are written against (coupling (4c) in
// amplifier.cpp, and the curvature block below), so clamping here is what keeps
// every one of those static_asserts literally true rather than re-derived.
// Symmetric on purpose: the curvature path takes second differences of four
// control points and only the magnitude matters there.
inline constexpr int64_t kCarrierControlClampMm = 9'000'000;

inline void carrierPrefilterStencil(const int64_t* raw, int64_t cp[16]) {
    constexpr int64_t S = kCarrierPrefilterSpan;
    constexpr int64_t R = kCarrierPrefilterRadius;
    // Stage 1: convolve along y, for the four control rows only.
    int64_t tmp[S * 4];
    for (int64_t a = 0; a < S; ++a) {
        for (int64_t j = 0; j < 4; ++j) {
            int64_t acc = kCarrierPrefilterW[0] * raw[a + S * (j + R)];
            for (int64_t n = 1; n <= R; ++n)
                acc += kCarrierPrefilterW[n] *
                       (raw[a + S * (j + R + n)] + raw[a + S * (j + R - n)]);
            // Numerator is routinely negative: truncDiv in the HLSL mirror.
            tmp[a + S * j] = acc / kCarrierPrefilterDen;
        }
    }
    // Stage 2: convolve along x.
    for (int64_t j = 0; j < 4; ++j) {
        for (int64_t i = 0; i < 4; ++i) {
            int64_t acc = kCarrierPrefilterW[0] * tmp[(i + R) + S * j];
            for (int64_t m = 1; m <= R; ++m)
                acc += kCarrierPrefilterW[m] *
                       (tmp[(i + R + m) + S * j] + tmp[(i + R - m) + S * j]);
            cp[i + 4 * j] = clampi64(acc / kCarrierPrefilterDen, -kCarrierControlClampMm,
                                     kCarrierControlClampMm);
        }
    }
}

// The one property the whole thing rests on: a CONSTANT elevation field must
// come out unchanged, or the prefilter moves sea level. True by construction
// (the denominator IS the weight sum) and swept anyway, because "by
// construction" is what the denominator being hand-typed would break.
constexpr bool carrierPrefilterIsUnitDc() {
    const int64_t probes[6] = {-9'000'000, -1234, 0, 1, 5678, 9'000'000};
    for (int k = 0; k < 6; ++k) {
        const int64_t v = probes[k];
        int64_t acc = kCarrierPrefilterW[0] * v;
        for (int64_t m = 1; m <= kCarrierPrefilterRadius; ++m)
            acc += kCarrierPrefilterW[m] * (v + v);
        if (acc / kCarrierPrefilterDen != v) return false;
    }
    return true;
}
static_assert(carrierPrefilterIsUnitDc(),
              "the prefilter must reproduce a constant field EXACTLY; if it does not, the "
              "denominator no longer equals the weight sum and every elevation in the world "
              "is scaled by the difference.");

// ---------------------------------------------------------------------------
// LANDFORM RELIEF (kWorldGenVersion 13) — the quantity detail amplitude is
// conditioned on, replacing the carrier GRADIENT.
//
// WHY THE GRADIENT WAS THE WRONG QUANTITY, MEASURED. v12 scaled the shaping
// band by slopeScaleQ10(gradient) and the microrelief band by
// microScaleQ10(gradient), both clamped to a narrow range, so the detail pass
// laid down 0.36-0.70 m of second-difference roughness at a 1.875 m lag
// REGARDLESS OF CLASS. Against a plain that is everything and against a
// mountain it is nothing: docs/terrain-validation-2026-07.md measured the same
// pass adding +207% to a plain's 1.875 m mean slope and +3.3% to alpine's, and
// the finished plain coming out SIX TIMES more ridged than the finished
// mountain (frac_ridge_peak 0.352 against 0.061, where real plains are
// 0.054-0.073). A gradient cannot tell those apart: a plain and a mountainside
// can have the same local grade and differ 25x in how much landform there is
// to decorate.
//
// WHAT IS MEASURED INSTEAD. The relief the raster itself carries across a fixed
// PHYSICAL baseline -- how much the land actually rises over 30 m, in mm:
//
//     relief30 = (|h(x+L) - h(x-L)| + |h(y+L) - h(y-L)| ) / 2,  L*pxMm = 30 m
//
// Three properties make this the right scalar, and the third is why it is a
// FIRST difference and not a second one:
//
//   * it is a MEASUREMENT OF THE TIER, not an assumption about it. This is the
//     mechanism the brief asked for under defect 2: the client stops adding a
//     fixed ladder blind and reads how much landform the raster actually
//     carries before deciding how much to decorate it with.
//   * the BASELINE IS 30 m ON EVERY TIER, deliberately, and this is the whole
//     reason it is not just carrierSlopeMmPerM under a new name. 30 m is the
//     scale at which the diffusion tile is measurably Earth-like (every realism
//     gate passes on S0) and the bake preserves it (S1 at 30 m reproduces S0).
//     The carrier's own analytic gradient is a 30 m quantity on a 30 m tier and
//     a 1.875 m quantity on the fine tier, so it silently means a different
//     thing per tier; this does not. Anchoring at the tier's own Nyquist would
//     be worse still: terrain-validation-2026-07 section 7.1 measures the baked
//     fine tier at Hurst ~1.5 below 30 m, far smoother than the 0.7-0.9 of real
//     ground, so a client that continued THAT trend would faithfully extend a
//     defect. Measure where the data is known to be good.
//   * IT HAS THE DYNAMIC RANGE THE CLASSES ACTUALLY DIFFER BY, which is the
//     property that decides whether any of this works, and it was measured
//     rather than assumed. On the two exemplars:
//
//         quantity                        plains    alpine    ratio
//         second difference over 30 m      706 mm   3407 mm    4.8x
//         FIRST difference over 30 m       665 mm  14565 mm   21.9x
//         detail amplitude actually needed                    23.8x
//
//     The second difference is a perfectly good curvature and it separates the
//     two classes by only 4.8x, because this plains tile is a DISSECTED plain:
//     locally wrinkly relative to its own amplitude. Real terrain's own answer
//     is the first difference -- the Illinois-to-Teton mean-slope ratio is 21x
//     at 1.875 m and 33x at 30 m, i.e. roughly scale-free -- so a client that
//     wants a plain to finish like a plain and a mountain like a mountain has
//     to condition on the quantity that carries that ratio. This was tried both
//     ways: at p = 1 on the second difference the plain still finished at 4.5
//     degrees against a 2.1 degree reference.
//
// The cost of losing "exactly zero on a plane" is a uniformly tilted plane
// getting detail in proportion to its tilt, which is what real hillsides do.
// The cost the old gate had -- FLAT ground getting a mountain's roughness -- is
// gone either way, because relief goes to zero there under both measures.
//
// COST: four extra raster reads per CELL (the centre is not even needed), on a
// lattice constant over the whole cell, so it memoises exactly as the stencil
// does. On the GPU it is four more buffer loads per column.
// ---------------------------------------------------------------------------

inline constexpr int64_t kReliefBaselineMm = 30000;

// The lag in PIXELS that puts the relief baseline at kReliefBaselineMm, rounded
// to nearest and never below one pixel.
constexpr int64_t carrierReliefLagPx(int64_t pxMm) {
    const int64_t l = (kReliefBaselineMm + pxMm / 2) / pxMm;
    return l < 1 ? 1 : l;
}
static_assert(carrierReliefLagPx(30000) == 1, "30 m tier: one pixel IS the baseline");
static_assert(carrierReliefLagPx(3750) == 8, "scale 8");
static_assert(carrierReliefLagPx(1875) == 16, "scale 16, the shipped fine tier");

// Mean of the two axis-aligned relief magnitudes across the baseline, in mm.
// Arguments are the four raster samples at (px-L,py) (px+L,py) (px,py-L)
// (px,py+L) -- note the baseline is 2L pixels wide, i.e. 60 m, and the number
// is therefore the rise across 60 m; kReliefRefMm is calibrated in the same
// currency so the factor of two never has to appear anywhere else.
constexpr int64_t carrierReliefMm(int64_t wx, int64_t ex, int64_t sy, int64_t ny) {
    const int64_t dx = ex - wx;
    const int64_t dy = ny - sy;
    return ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy)) / 2;
}
static_assert(carrierReliefMm(0, 0, 0, 0) == 0,
              "relief must be EXACTLY zero on dead-flat ground -- that is the case the old "
              "gate got wrong, and it is where the artifact was worst");
static_assert(carrierReliefMm(-3000, 3000, -3000, 3000) == 6000, "a uniform 10% grade");

// The carrier's value and analytic gradient at a point inside one cell, from
// the 4x4 control stencil (row-major, j = y outer, i = x inner; index 0
// corresponds to control point px-1 / py-1).
struct CarrierEval {
    int64_t heightMm = 0;
    int64_t sxMmPerPx = 0; // signed
    int64_t syMmPerPx = 0; // signed
};

inline CarrierEval evalCarrier(const int64_t cp[16], int64_t fx, int64_t fy, int64_t pxMm) {
    // fx, fy are in [0, pxMm) by floorDiv's contract, so both numerators are
    // non-negative and tq lands in [0, 1023].
    const int64_t tx = fx * kCarrierT / pxMm;
    const int64_t ty = fy * kCarrierT / pxMm;
    const CarrierW4 wx = carrierValueW(tx), wy = carrierValueW(ty);
    const CarrierW3 dx = carrierSlopeW(tx), dy = carrierSlopeW(ty);

    // Stage 1: collapse x for each of the four control rows. Both the value and
    // the x-derivative come off the same row read.
    int64_t rowVal[4], rowDx[4];
    for (int j = 0; j < 4; ++j) {
        const int64_t* r = cp + 4 * j;
        int64_t v = 0, d = 0;
        for (int i = 0; i < 4; ++i) v += r[i] * wx.w[i];
        for (int i = 0; i < 3; ++i) d += (r[i + 1] - r[i]) * dx.w[i];
        rowVal[j] = v / kCarrierValueDen;
        rowDx[j] = d / kCarrierSlopeDen;
    }

    // Stage 2: collapse y. The y-derivative is the quadratic basis over the
    // first differences of the already-collapsed rows — the tensor product
    // commutes, so this is the same value as collapsing y first.
    int64_t h = 0, sx = 0, sy = 0;
    for (int j = 0; j < 4; ++j) {
        h += rowVal[j] * wy.w[j];
        sx += rowDx[j] * wy.w[j];
    }
    for (int j = 0; j < 3; ++j) sy += (rowVal[j + 1] - rowVal[j]) * dy.w[j];

    CarrierEval e;
    e.heightMm = h / kCarrierValueDen;
    e.sxMmPerPx = sx / kCarrierValueDen;
    e.syMmPerPx = sy / kCarrierSlopeDen;
    return e;
}

// The slope currency, in MM PER METRE.
//
// This unit change is not cosmetic and it is not optional. The old currency was
// mm per tile PIXEL, which is proportional to pixelSizeMm -- so every threshold
// stated in it (slopeScaleQ10, microScaleQ10, classifyBiome's cliff gate, the
// topsoil retention term) silently means a different GRADE at scale 8 than at
// scale 1. biome.h recorded that latent bug and asked for all four to be fixed
// together before scale-8 tiles are generated. This is that fix; the baked fine
// tier in docs/terrain-amplification-plan.md Phase 2 is what made it urgent.
//
// L1 (|dx| + |dy|) rather than the Euclidean norm, keeping the SHAPE of the old
// tileSlopeMmPerPx so the recalibrated thresholds in amplifier.cpp are a pure
// unit conversion of the tuned v8 values rather than a fresh tuning problem.
inline int64_t carrierSlopeMmPerM(const CarrierEval& c, int64_t pxMm) {
    const int64_t ax = c.sxMmPerPx < 0 ? -c.sxMmPerPx : c.sxMmPerPx;
    const int64_t ay = c.syMmPerPx < 0 ? -c.syMmPerPx : c.syMmPerPx;
    return (ax + ay) * 1000 / pxMm;
}

// ---------------------------------------------------------------------------
// ANALYTIC CURVATURE (docs/terrain-amplification-plan.md §3b/§3c).
//
// Kept as a SEPARATE evaluation rather than folded into CarrierEval on purpose:
// evalCarrier is on the hot path of every column and the value/slope halves
// above must stay bit-identical to v9. A caller that does not gate on curvature
// pays nothing.
//
// SIGN CONVENTION, stated once because everything downstream depends on it.
// These are the plain second derivatives of HEIGHT, d2h/dx2 and d2h/dy2. So:
//
//     crest / ridge / convex-up ground  ->  NEGATIVE
//     hollow / valley / concave-up      ->  POSITIVE
//
// This is the opposite of the geomorphological "profile curvature" convention,
// where convex is positive. It is chosen because it is the derivative, full
// stop -- there is no place to hide a sign flip, and the tests assert the
// direction against an explicit quadratic control field rather than against
// prose.
//
// UNIT: MM PER METRE PER METRE, in q10 fixed point (kCurveQ10One == 1.0).
//
// That is a per-metre change in the mm-per-metre grade, i.e. exactly the
// derivative of carrierSlopeMmPerM's currency, and like it, it is SCALE
// INVARIANT: the same physical grade-change reads the same number at pixel
// scale 1, 8 and 16. This is deliberate and it is the direct lesson of the v9
// slope-currency bug, where slopeScaleQ10 took mm-per-PIXEL and would have
// silently meant a different grade on 1.875 m tiles than on 30 m ones.
//
// WHY q10 AND NOT WHOLE MM PER M^2. Whole mm/m^2 is far too coarse to carry
// real landform curvature at 30 m posts. A smooth hilltop 100 m high with a
// 500 m radius is a curvature of 2*100/500^2 = 0.8 mm/m^2 -- so an integer
// mm/m^2 quantum would round essentially all real landform curvature to 0 or
// +-1 and the gate below would be a three-level step function. q10 puts the
// quantum at ~0.001 mm/m^2, which at 30 m posts is a 0.9 mm second difference
// -- finer than the millimetre the control points themselves are quantized to,
// so nothing real is lost. The PHYSICAL unit is still mm per metre per metre;
// only the fixed-point scale is explicit.
//
// WHAT IS *NOT* SCALE INVARIANT: the MAGNITUDE terrain actually exhibits.
// Curvature is a spectral quantity -- terrain measured over a 1.875 m baseline
// is far more curved than the same terrain measured over a 30 m baseline (that
// is what a fractal surface means). So the UNIT transfers across tiers and the
// GATE'S KNEE DOES NOT. See kCurvatureKneeQ10.
// ---------------------------------------------------------------------------

inline constexpr int64_t kCurveQ10One = 1024; // q10 one for the curvature currency
// mm per metre per metre from mm per pixel^2: divide by (pxMm/1000)^2.
inline constexpr int64_t kMmPerMSquared = 1'000'000;

// The carrier's two axis-aligned second derivatives at a point inside one cell,
// in MM PER PIXEL^2 (the raw spline unit; carrierCurvatureMmPerM2Q10 converts).
struct CarrierCurvature {
    int64_t cxxMmPerPx2 = 0; // signed
    int64_t cyyMmPerPx2 = 0; // signed
};

// ---------------------------------------------------------------------------
// OVERFLOW ANALYSIS FOR THE CURVATURE PATH. Worked, not assumed: the value
// path's two-stage argument does not carry over unchanged, because the second
// differences carry a factor of 4 on the control-point magnitude that the
// convex value combination does not.
//
// Let C be the control-point magnitude bound. Following amplifier.cpp's
// coupling (4c) that is kSurfaceClampMaxMm = 9e6 mm (elevations are int32 mm
// and the world clamp is well inside int32). A second difference of four such
// points is |D| <= 4C = 3.6e7 mm. int64 tops out at 9.22e18.
//
//   naive exact tensor form (weights in fx, no q10): denominator 6*pxMm^4,
//     numerator <= 4C * 6 * pxMm^4 = 1.75e26 at pxMm = 30000.
//     OVERFLOWS by ~7 orders of magnitude. Rejected, exactly as the value
//     path's fx-weighted form was.
//
//   one-stage q10 form (skip the stage-1 divide, carry q10 into stage 2):
//     numerator <= 4C * kCarrierCurveDen * kCarrierValueDen
//                = 3.6e7 * 1024 * 6.442e9 = 2.4e20.
//     OVERFLOWS by 26x. Also rejected -- and this is the tempting one, because
//     kCarrierCurveDen happens to equal the q10 one so the divide *looks* like
//     it cancels for free. It does not; do not "simplify" it back.
//
//   the two-stage form used below:
//     stage 1  |sum| <= 4C * kCarrierCurveDen = 3.6e7 * 1024 = 3.69e10
//                                                        (2.5e8x margin)
//     stage 2  |sum| <= 4C * kCarrierValueDen = 3.6e7 * 6.442e9 = 2.32e17
//                                                        (40x margin)
//   and the unit conversion in carrierCurvatureMmPerM2Q10:
//     |c| * kCurveQ10One * kMmPerMSquared <= 8C * 1024 * 1e6 = 7.4e16
//                                                        (125x margin)
//   (8C rather than 4C there because the Laplacian sums the two axes.)
//
// The tightest margin in the chain is 40x, at stage 2. That is the number to
// re-derive if the elevation clamp, the q10 width or the basis denominators
// ever move; the static_assert below pins it.
//
// EXACTLY ZERO ON A PLANAR CONTROL FIELD — the property the whole gate rests
// on, and the reason both axes are evaluated in the SAME order (difference
// first, interpolate second) rather than one each way. For a planar control
// field every second difference is identically 0 BEFORE any division happens,
// so stage 1 is exactly 0 and so is everything after it. Truncation cannot
// leak: it has nothing to truncate.
//
// This is not a free property of the tensor product. Evaluating d2/dy2 the
// other way round -- collapse x with the cubic basis first, then difference the
// row values -- would take the second difference of numbers that have ALREADY
// been truncated, and truncation toward zero is not translation invariant
// across a sign change (trunc(-1.5) = -1 but trunc(1.5) = 1), so a tilted plane
// whose row values straddle zero would report a spurious curvature of up to
// ~4 mm/px^2. At a 1.875 m fine-tier pixel that is 1.1 mm/m^2, comparable to
// the whole gate knee. Differencing first makes it structurally impossible.
// ---------------------------------------------------------------------------
static_assert(int64_t(4) * 9'000'000 * kCarrierValueDen < (int64_t(1) << 62),
              "curvature stage-2 product can overflow int64; the second differences carry "
              "a factor of 4 on the control-point magnitude, so this is NOT implied by the "
              "value path's bound. Re-derive the OVERFLOW ANALYSIS block above before "
              "widening the q10 fraction, the elevation range or the basis denominators.");
static_assert(int64_t(8) * 9'000'000 * kCurveQ10One * kMmPerMSquared < (int64_t(1) << 62),
              "curvature unit conversion can overflow int64; see the OVERFLOW ANALYSIS "
              "block above carrierCurvatureMmPerM2Q10.");

inline CarrierCurvature evalCarrierCurvature(const int64_t cp[16], int64_t fx, int64_t fy,
                                             int64_t pxMm) {
    // Same q10 cell fraction as evalCarrier. fx, fy are in [0, pxMm) by
    // floorDiv's contract, so both numerators are non-negative and tq lands in
    // [0, 1023]; truncation here agrees with the HLSL mirror's truncDiv.
    const int64_t tx = fx * kCarrierT / pxMm;
    const int64_t ty = fy * kCarrierT / pxMm;
    const CarrierW4 wx = carrierValueW(tx), wy = carrierValueW(ty);
    const CarrierW2 gx = carrierCurveW(tx), gy = carrierCurveW(ty);

    // d2/dx2: stage 1 differences ALONG X within each control row (exact, no
    // division), stage 2 collapses Y with the cubic value basis.
    // d2/dy2: the mirror image — stage 1 differences ALONG Y within each
    // control column, stage 2 collapses X.
    //
    // Both stage-1 divisions have a numerator that is routinely NEGATIVE (a
    // crest gives negative second differences). Truncation toward zero, plain
    // `/` in C++; the HLSL mirror must use truncDiv here, not a bare `/`.
    int64_t rowD2[4], colD2[4];
    for (int k = 0; k < 4; ++k) {
        const int64_t* r = cp + 4 * k; // row k, varying x
        int64_t dxx = 0, dyy = 0;
        for (int m = 0; m < 2; ++m) {
            dxx += (r[m + 2] - 2 * r[m + 1] + r[m]) * gx.w[m];
            // column k, varying y: stride 4.
            dyy += (cp[k + 4 * (m + 2)] - 2 * cp[k + 4 * (m + 1)] + cp[k + 4 * m]) * gy.w[m];
        }
        rowD2[k] = dxx / kCarrierCurveDen;
        colD2[k] = dyy / kCarrierCurveDen;
    }

    int64_t cxx = 0, cyy = 0;
    for (int k = 0; k < 4; ++k) {
        cxx += rowD2[k] * wy.w[k];
        cyy += colD2[k] * wx.w[k];
    }

    CarrierCurvature c;
    // Also routinely negative; truncDiv in the mirror.
    c.cxxMmPerPx2 = cxx / kCarrierValueDen;
    c.cyyMmPerPx2 = cyy / kCarrierValueDen;
    return c;
}

// The curvature currency: the LAPLACIAN d2h/dx2 + d2h/dy2, in q10 mm per metre
// per metre. Signed; see the sign convention above (crest negative).
//
// The sum rather than either axis alone, and rather than an L1 norm of the two:
// the gate wants to know whether the ground is bulging out or dishing in, which
// is a signed, rotation-invariant question, and the Laplacian is the cheapest
// rotation-invariant answer. (carrierSlopeMmPerM uses L1 because slope
// MAGNITUDE is what modulates detail there; here the SIGN is the whole point,
// and an L1 norm would throw it away.)
//
// Truncating division on a numerator that is routinely negative — truncDiv in
// the HLSL mirror. Truncation toward zero is symmetric, so it preserves the
// antisymmetry curvature(-h) == -curvature(h) exactly, which the tests assert.
inline int64_t carrierCurvatureMmPerM2Q10(const CarrierCurvature& c, int64_t pxMm) {
    const int64_t perPx2 = c.cxxMmPerPx2 + c.cyyMmPerPx2; // mm per pixel^2
    return perPx2 * kCurveQ10One * kMmPerMSquared / (pxMm * pxMm);
}

// ---------------------------------------------------------------------------
// THE CURVATURE GATE (docs/terrain-amplification-plan.md §3c).
//
// Convex crests roughen, concave hollows smooth. The plan's rationale: this
// ridge-sharp / valley-smooth asymmetry is what makes ground read as SHAPED
// rather than TEXTURED — and it is not merely an aesthetic trick, it is what
// the ground actually does. Hollows are where colluvium collects; a debris
// mantle is genuinely smoother than the crest that shed it.
//
// ***** THE CONSTANTS BELOW ARE PROVISIONAL AND UNCALIBRATED. *****
//
// The plan requires detail amplitudes to be set by PROBE MEASUREMENT against
// the fine tier's measured S2, and that tier does not exist on the client yet
// (Phase 2 ships it). Nothing here has been tuned by eye and nothing here has
// been measured; the values are defensible STARTING POINTS with their
// derivations written down, so that the probe pass has something to move rather
// than something to invent. Specifically:
//
//   * the two amplitude limits are the plan's own numbers (~1.75x on crests,
//     ~0.5x in hollows) taken at face value;
//   * the knee is an order-of-magnitude anchor: a convex hilltop that turns a
//     +20% grade into a -20% grade (a 400 mm/m change) across a 200 m crest
//     zone is 400/200 = 2 mm/m^2. That is a recognisable landform crest at
//     30 m posts, so 2 mm/m^2 is where the gate saturates.
//
// THE KNEE IS PER-TIER AND WILL NOT TRANSFER. Curvature magnitude is spectral:
// the same hillside measured over a 1.875 m fine-tier baseline reads curvature
// two to three orders of magnitude larger than over a 30 m baseline, so a knee
// derived at 30 m saturates everywhere at 1.875 m. This is NOT the mm-per-pixel
// bug returning — the UNIT is scale invariant and means the same grade-change
// at every scale; it is that the QUANTITY genuinely differs by scale. Whoever
// calibrates against the fine tier must re-derive the knee there, and should
// expect a much larger number.
//
// SHAPE. Piecewise linear in the curvature, continuous, with a single knee
// magnitude and two different gains so that the characteristic crest curvature
// lands exactly on the ceiling and the characteristic hollow curvature exactly
// on the floor. The kink at zero is a kink in the GAIN's dependence on
// curvature, not a step in the gain itself; curvature is continuous, so the
// amplitude field stays continuous. (slopeScaleQ10 and microScaleQ10 already
// have kinks at their clamps for the same reason.)
//
// HARD, COMPILE-TIME-PROVABLE CEILING. The argument is clamped into
// [-kCurvatureKneeQ10, +kCurvatureKneeQ10] as the FIRST operation, so the
// sweep below over exactly that interval is exhaustive over every int64 input
// — not a sample of it. Amplifier::surfaceUpperBoundMm derives the world's
// surface bound from the detail envelope and a bound violation is a hole in the
// world, so "the gate cannot exceed kCurvatureScaleMaxQ10" has to be a proof,
// not a comment.
//
// NOT WIRED IN. evalSurface does not call this yet; wiring it in changes
// worldgen output and requires re-deriving the surface bound (the detail
// allowance would gain a factor of kCurvatureScaleMaxQ10/1024) and bumping
// kWorldGenVersion.
// ---------------------------------------------------------------------------

inline constexpr int64_t kCurvatureScaleOneQ10 = 1024; // 1.0x, at zero curvature
inline constexpr int64_t kCurvatureScaleMinQ10 = 512;  // 0.5x  in hollows  [PROVISIONAL]
inline constexpr int64_t kCurvatureScaleMaxQ10 = 1792; // 1.75x on crests   [PROVISIONAL]
// The knee, in the q10 currency of carrierCurvatureMmPerM2Q10, AT THE 30 m
// REFERENCE TIER. Feed it through carrierCurvatureTierNormQ10 first.
//
// 0.5 mm/m^2. This is a MEASURED anchor, not a round number: the mean
// |Laplacian| on a realistic 30 m raster is 288 q10 (0.28 mm/m^2), so a knee at
// 512 puts typical ground a little over half way to saturation and leaves a
// genuinely convex crest at the clamp. The first cut of this was 2 * kCurveQ10One
// = 2048, i.e. seven times the mean, and the probe showed the consequence
// bluntly: convex/concave detail ratio 0.99 at every lag from 0.1 m to 1.6 m --
// the gate was wired in, bounded, proved, mirrored, and doing NOTHING. A gate
// whose knee sits far outside the data's range is indistinguishable from no gate,
// and nothing but an end-to-end measurement catches that.                [PROVISIONAL]
// MEASURED, twice, in opposite directions. 2.0 was seven times the mean and the
// gate was inert. 0.5 overcorrected: the gate census on real tiles showed
// **70% of samples saturated at a gentle site and 96% at a steep one**, gain
// median 0.5-0.675 with only 0.4-2.7% of points anywhere inside [0.95, 1.05].
// That is not a ramp, it is a two-valued step -- the same seam class this project
// removes, just keyed on curvature instead of on the tile grid. A knee sweep on
// measured data gives convex/concave 3.41 at 0.5, **3.11 at 1.0**, 2.18 at 2.0:
// 1.0 keeps nearly all the conditioning for far less clipping, and 3.5 is only
// reachable by a gate that has become a step. Note the tier-normalised mean |k|
// is 1.00 mm/m^2 at a gentle site and 7.32 at a steep one, so no single knee can
// be un-saturated everywhere; this one is chosen to sit AT the gentle site's mean
// rather than far below both.
inline constexpr int64_t kCurvatureKneeQ10 = kCurveQ10One;

// --- WHY THE KNEE NEEDS A TIER NORMALISATION -------------------------------
//
// The curvature UNIT (mm per metre per metre) is scale-invariant. The curvature
// a landscape EXHIBITS is not: curvature is a second derivative, and on a
// self-affine surface its magnitude grows as you resolve finer scales. Measured
// on the same realistic raster: mean |Laplacian| 288 q10 at a 30 m pixel and
// 74,182 q10 at 1.875 m. That ratio is 257.6 against a pixel ratio of 16, i.e.
// almost exactly 16^2 -- curvature scales as pxMm^-2, which is what the second
// derivative of a self-affine field must do.
//
// So a single knee cannot serve both tiers. At 2048 it saturated 0% of samples
// at 30 m and 98% at 1.875 m: no-op on one world, hard-clipped on the other,
// and in both cases carrying no information. Normalising by (pxMm/30000)^2
// before gating makes the gate ask "how curved is this RELATIVE TO THE SCALE WE
// RESOLVE", which is the question "crest or hollow" actually means, and makes
// one knee correct on every tier.
//
// The exponent is empirical, from the 257.6-vs-256 agreement above, not assumed
// from theory -- if a future tier disagrees with pxMm^-2, re-measure rather than
// trusting this line.
inline constexpr int64_t kCurvatureRefPixelMm = 30000;
constexpr int64_t carrierCurvatureTierNormQ10(int64_t curveMmPerM2Q10, int64_t pxMm) {
    // Overflow: |curve| is bounded well under 1e10 in practice and pxMm <= 30000,
    // so the product stays under 9e18 against int64's 9.22e18. The assert below
    // pins the pixel side; the curvature side is bounded by the carrier's own
    // control-point range.
    return floorDiv(curveMmPerM2Q10 * pxMm * pxMm,
                    kCurvatureRefPixelMm * kCurvatureRefPixelMm);
}
static_assert(carrierCurvatureTierNormQ10(1000, kCurvatureRefPixelMm) == 1000,
              "the reference tier must be the identity, or the knee means something "
              "different from the number it was measured against");
static_assert(carrierCurvatureTierNormQ10(2560, 1875) == 10,
              "a 16x finer pixel must divide curvature by 16^2 = 256");
static_assert(carrierCurvatureTierNormQ10(-2560, 1875) == -10,
              "and it must be symmetric -- floorDiv, not truncation, so a crest and "
              "a hollow of equal magnitude gate equally");

static_assert(kCurvatureScaleMinQ10 < kCurvatureScaleOneQ10 &&
                  kCurvatureScaleOneQ10 < kCurvatureScaleMaxQ10,
              "the gate must straddle 1.0x: roughen on crests, smooth in hollows");
static_assert(kCurvatureKneeQ10 > 0, "the knee must be positive; it divides");

// Detail-amplitude gain from carrier curvature, in q10 (1024 == 1.0x).
// Argument: the signed Laplacian from carrierCurvatureMmPerM2Q10 — q10 mm per
// metre per metre, NEGATIVE on crests.
//
// Both divisions have a non-negative numerator by construction (the magnitude
// is taken first), so truncation and flooring agree and the HLSL mirror may use
// either helper; truncDiv matches the C++ `/` written here.
constexpr int64_t curvatureScaleQ10(int64_t curveMmPerM2Q10) {
    const int64_t c = clampi64(curveMmPerM2Q10, -kCurvatureKneeQ10, kCurvatureKneeQ10);
    if (c < 0) { // crest: roughen
        return kCurvatureScaleOneQ10 +
               (-c) * (kCurvatureScaleMaxQ10 - kCurvatureScaleOneQ10) / kCurvatureKneeQ10;
    }
    // hollow: smooth
    return kCurvatureScaleOneQ10 -
           c * (kCurvatureScaleOneQ10 - kCurvatureScaleMinQ10) / kCurvatureKneeQ10;
}

// THE PROOF. Exhaustive over the clamped domain, which -- because the clamp is
// the function's first operation -- is exhaustive over int64. Checks the
// ceiling, the floor, monotonicity (non-increasing in curvature: more concave
// must never mean rougher) and that 1.0x sits exactly at zero curvature.
constexpr bool curvatureScaleIsBounded() {
    int64_t prev = curvatureScaleQ10(-kCurvatureKneeQ10 - 1);
    if (prev != kCurvatureScaleMaxQ10) return false;
    for (int64_t c = -kCurvatureKneeQ10; c <= kCurvatureKneeQ10 + 1; ++c) {
        const int64_t v = curvatureScaleQ10(c);
        if (v > kCurvatureScaleMaxQ10 || v < kCurvatureScaleMinQ10) return false;
        if (v > prev) return false; // non-increasing
        if (c == 0 && v != kCurvatureScaleOneQ10) return false;
        prev = v;
    }
    return prev == kCurvatureScaleMinQ10;
}
static_assert(curvatureScaleIsBounded(),
              "curvatureScaleQ10 must be non-increasing, must reach both clamps, must be "
              "exactly 1.0x at zero curvature, and must NEVER exceed kCurvatureScaleMaxQ10. "
              "Amplifier::surfaceUpperBoundMm's detail allowance would be multiplied by "
              "this ceiling; a violation is a hole in the world.");
// The extremes, separately, so that a future edit which moves the clamp out of
// first position (and thus makes the sweep above non-exhaustive) still trips.
static_assert(curvatureScaleQ10(INT64_MIN) == kCurvatureScaleMaxQ10,
              "the gate must saturate at the ceiling for ANY input, however extreme");
static_assert(curvatureScaleQ10(INT64_MAX) == kCurvatureScaleMinQ10,
              "the gate must saturate at the floor for ANY input, however extreme");
static_assert(curvatureScaleQ10(0) == kCurvatureScaleOneQ10, "1.0x at zero curvature");

// ---------------------------------------------------------------------------
// THE RELIEF GATE (kWorldGenVersion 13) — one gate over the WHOLE detail
// ladder, replacing slopeScaleQ10 and microScaleQ10.
//
// LINEAR IN RELIEF, because that is what a self-affine continuation is. If the
// raster carries S2(30 m) = relief30 of detrended structure, then a surface
// that continues its own trend carries S2(lambda) = relief30 * (lambda/30 m)^H
// at every shorter wavelength: the SHAPE across octaves is the table's job and
// the LEVEL is proportional to the measured relief. Doubling the landform
// doubles the decoration; flattening it to a plane removes the decoration
// entirely.
//
// kReliefRefMm IS A CALIBRATION, and it was made against the slope-by-scale
// ladder in docs/terrain-validation-2026-07.md section 4 rather than by eye, at
// BOTH exemplar sites and on BOTH tiers -- the single discipline whose absence
// caused this defect. Measured relief30 (rise across the 60 m baseline) on the
// pinned world -- and note it is nearly the SAME NUMBER ON EITHER TIER, which
// is the property that lets one calibration serve both and is exactly what the
// baseline being physical buys:
//
//                       30 m raster      1.875 m baked lattice
//     plains (-55,20)       872 mm              881 mm
//     alpine  (-5,15)    15,692 mm           18,983 mm
//
// (the alpine pair differs by 20% only because the two were measured over
// different windows -- the whole 15.33 km tile against its central 2.04 km.)
//
// So the raw quantity spans 18x between the two classes where slopeScaleQ10
// spanned 1.9x and microScaleQ10 spanned 1.06x. THAT RATIO IS THE FIX.
// kReliefRefMm is set AT THE ALPINE MEASUREMENT, so alpine gates to ~1.0x: it
// is the site whose detail level measured correct (+3.3% on its own mean slope,
// and still under-textured against the Teton DTM), so it is the one that must
// not move, and the plain is the one that comes down -- all the way onto the
// floor, which is where a plain belongs.
//
// THE FLOOR. Decimetre roughness is a property of the material, not of the
// landform: a dead-flat playa still has clods and stones, and at 10 cm voxels a
// perfectly smooth surface is precisely where terrace runs are longest and the
// corduroy artifact is worst (see kDetailOctaves). v13 answers that in TWO
// places rather than one, because the old single answer -- microScaleQ10's 0.75
// floor on everything from 1.6 m down -- is a large part of why a plain got a
// mountain's roughness:
//
//   * the MATERIAL band (0.4 m and 0.2 m) is not relief-gated at all. It is
//     material, not landform, so it does not scale with the landform.
//   * this floor keeps a little of the METRE band alive on the flattest ground,
//     which is what actually breaks a terrace run. 0.1x of a 500 mm octave is
//     50 mm at 1.6 m -- half a voxel of relief across sixteen voxels, enough to
//     wander across a contour and not enough to read as texture.
// THE CEILING IS 2.0x, NOT slopeScaleQ10's 4.0x, and that is a deliberate
// tightening rather than an oversight. The surface bound takes this gate AT its
// clamp (it cannot bound a second difference over a footprint -- see coupling
// (5) in amplifier.cpp), so the ceiling is multiplied straight into the world's
// detail envelope. At 4.0 the envelope would GROW from 27.3 m to 30.7 m,
// because v13 also puts the microrelief band and the two additive terms under
// this gate where they used to be under a 1.25x one and a constant. At 2.0 it
// SHRINKS to 15.4 m, and the streaming trims get better rather than worse.
// 2.0x is not a compromise on the terrain either: relief30 has to reach 32 m of
// rise across the 60 m baseline -- a sustained 53% grade, twice the alpine
// exemplar's own mean -- before the clamp bites at all.
inline constexpr int64_t kReliefScaleMinQ10 = 102;  // 0.10x -- the anti-terrace floor
inline constexpr int64_t kReliefScaleMaxQ10 = 2048; // 2.0x
inline constexpr int64_t kReliefRefMm = 16000;
static_assert(kReliefRefMm > 0, "the relief reference divides");
static_assert(kReliefScaleMinQ10 > 0,
              "the relief gate must never reach zero: a flat raster still needs decimetre "
              "roughness or the voxel terrace artifact comes straight back");
static_assert(kReliefScaleMinQ10 < 1024 && 1024 < kReliefScaleMaxQ10,
              "the gate must straddle 1.0x");

constexpr int64_t reliefScaleQ10(int64_t relief30Mm) {
    // relief30Mm is a magnitude, so the numerator is non-negative and
    // truncation and flooring agree; the HLSL mirror may use either helper.
    return clampi64(relief30Mm * 1024 / kReliefRefMm, kReliefScaleMinQ10, kReliefScaleMaxQ10);
}

// THE PROOF the bound needs: nondecreasing, and clamped at both ends for ANY
// input. Amplifier::surfaceUpperBoundMm takes this gate at its ceiling, so
// "cannot exceed kReliefScaleMaxQ10" has to be a proof and not a comment -- a
// violation is a hole in the world. The sweep runs every value across the whole
// range where the result varies (it saturates at relief30 = 9600 mm) plus a
// coarse tail out to 300 m of second difference over a 30 m baseline, which no
// int32 elevation can produce.
constexpr bool reliefScaleIsNondecreasing() {
    int64_t prev = reliefScaleQ10(0);
    if (prev != kReliefScaleMinQ10) return false;
    for (int64_t r = 0; r <= 60000; ++r) {
        const int64_t v = reliefScaleQ10(r);
        if (v < prev || v > kReliefScaleMaxQ10 || v < kReliefScaleMinQ10) return false;
        prev = v;
    }
    for (int64_t r = 60000; r <= 3'000'000; r += 97) {
        const int64_t v = reliefScaleQ10(r);
        if (v < prev || v > kReliefScaleMaxQ10) return false;
        prev = v;
    }
    return prev == kReliefScaleMaxQ10;
}
static_assert(reliefScaleIsNondecreasing(),
              "reliefScaleQ10 must be nondecreasing, must reach both clamps, and must NEVER "
              "exceed kReliefScaleMaxQ10; the surface bound feeds it the footprint's maximum "
              "and takes the ceiling.");
static_assert(reliefScaleQ10(INT64_MAX / 2048) == kReliefScaleMaxQ10,
              "the gate must saturate at the ceiling for any input past the sweep");

} // namespace vxc
