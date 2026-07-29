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
//      not interpolate its control points, it approximates them. That is
//      corrected by prefiltering the control lattice, which is a float IIR pass
//      -- illegal here, trivial in the tile bake. Until the baked fine tier
//      ships (docs/terrain-amplification-plan.md Phase 2) the raw samples are
//      used as control points directly and the carrier is very slightly
//      smoother than the raster; at 30 m posts that is far below the detail
//      band and vxc_terrainprobe's carrier-only mode is how it stays honest.
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
// 2.0 mm per metre per metre, in the q10 currency of
// carrierCurvatureMmPerM2Q10.                                    [PROVISIONAL]
inline constexpr int64_t kCurvatureKneeQ10 = 2 * kCurveQ10One;

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

} // namespace vxc
