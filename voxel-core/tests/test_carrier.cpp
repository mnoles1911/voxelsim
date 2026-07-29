// The C2 cubic B-spline carrier (voxelcore/carrier.h): the extracted value and
// slope halves, the analytic curvature added for
// docs/terrain-amplification-plan.md §3c, and the curvature gate.
//
// WHAT THIS FILE IS DEFENDING. Symptom 2 of the whole terrain-amplification
// project was a GRADIENT STEP at tile-pixel lines: the height was continuous
// across a 30 m pixel boundary and the slope was not, which is invisible in h
// and glaring under directional light. v9 fixed that by going from bilinear
// (C0) to a uniform cubic B-spline (C2). §3c now conditions detail amplitude on
// CURVATURE, so a merely-C1 carrier would reproduce the exact same artifact one
// derivative down — which is why carrier_curvature_is_continuous_at_pixel_lines
// below is the test that justifies using B-splines at all rather than the
// cheaper Catmull-Rom.
//
// The other load-bearing test is carrier_curvature_is_zero_on_a_plane. A linear
// field must have identically zero second derivative, so ANY error in the
// weight derivation, the differencing order or the fixed-point scaling shows up
// there as a non-zero number. It is the sharpest single check in the file.

#include "voxelcore/carrier.h"
#include "voxelcore/hash.h"

#include <cstdio>

#include "vxctest.h"

using namespace vxc;

namespace {

// Tile pixel sizes worth covering: scale 1 (the shipped 30 m raster), the
// 1.875 m fine tier docs/terrain-amplification-plan.md Phase 2 will bake, and a
// clean 10x of scale 1 for the scale-invariance check.
constexpr int64_t kPx30m = 30000;
constexpr int64_t kPxFine = 1875;
constexpr int64_t kPx3m = 3000;

// Planar control field: h = a + b*i + c*j over the 4x4 stencil.
void fillPlane(int64_t cp[16], int64_t a, int64_t b, int64_t c) {
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) cp[i + 4 * j] = a + b * (i - 1) + c * (j - 1);
}

// Quadratic control field: h = A*(i-1)^2 + B*(j-1)^2. Its second differences are
// the constants 2A (in x) and 2B (in y), so the carrier's second derivative is
// EXACTLY 2A / 2B mm per pixel^2 everywhere in the cell, with no truncation
// slack: the linear basis partitions its denominator, so a constant vector of
// second differences comes back unchanged.
void fillQuadratic(int64_t cp[16], int64_t A, int64_t B) {
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            cp[i + 4 * j] = A * (i - 1) * (i - 1) + B * (j - 1) * (j - 1);
}

// A deterministic, strongly-curved control raster for the transect tests.
int64_t roughElev(int64_t px, int64_t py) {
    return hashToSigned16(hash2(20260719, px, py, 11)) * 300 / 32768;
}

void stencilAt(int64_t cp[16], int64_t px, int64_t py) {
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) cp[i + 4 * j] = roughElev(px - 1 + i, py - 1 + j);
}

int64_t absi(int64_t v) { return v < 0 ? -v : v; }

// `actual` is the truncation-toward-zero of num/den, checked WITHOUT performing
// that division — so it does not restate the implementation's own arithmetic.
bool isTruncatedQuotient(int64_t actual, int64_t num, int64_t den) {
    if (den <= 0) return false;
    const int64_t prod = actual * den;
    if (absi(prod) > absi(num)) return false;        // did not overshoot
    if (absi(num) - absi(prod) >= den) return false; // and is the closest one
    if (num != 0 && actual != 0 && ((num < 0) != (actual < 0))) return false; // sign
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// (1) The extracted carrier still reproduces known values bit-identically.
//
// The extraction of the carrier from amplifier.cpp into voxelcore/carrier.h was
// a PURE MOVE, and the primary evidence for that is that
// `vxc_bench --radius 128 --digest` is unchanged across it. This test is the
// standing guard AFTER that move: goldens for the value, the slope and the
// curvature at fixed fractions over a fixed stencil, plus three analytic
// identities that are independent of how the evaluator is arranged.
// ---------------------------------------------------------------------------
VXC_TEST(carrier_value_and_slope_are_bit_identical) {
    int64_t cp[16];
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            cp[i + 4 * j] = hashToSigned16(hash2(20260719, i, j, 7)) * 7 / 3;
    // The stencil these goldens were taken over; pinned so that a change to the
    // hash cannot silently rebase the goldens onto different control points.
    const int64_t expectStencil[16] = {22101,  41160,  29925,  -45546, 18013, -48827,
                                       -55279, 40838,  -70980, -39286, -51186, -35634,
                                       12824,  51622,  -28907, 62405};
    for (int k = 0; k < 16; ++k) CHECK_EQ(cp[k], expectStencil[k]);

    struct Golden {
        int64_t fx, fy, h, sx, sy, cxx, cyy, lapQ10;
    };
    // Captured at kWorldGenVersion 9, pxMm = 30000. h/sx/sy are the values the
    // pre-move amplifier.cpp produced (the unchanged bench digest is the proof
    // of that); cxx/cyy/lapQ10 pin the new curvature path.
    const Golden g[] = {
        {0, 3000, -31362, -21426, -34554, 26352, 68975, 108460},
        {7000, 14000, -42180, -10759, -3899, 20880, 76055, 110290},
        {14000, 25000, -40495, -7095, 21451, 15755, 61258, 87623},
        {21000, 6000, -40308, 6885, -22946, 53912, 72152, 143432},
        {28000, 17000, -41846, 15974, -2723, 59091, 54353, 129074},
    };
    for (const Golden& e : g) {
        const CarrierEval v = evalCarrier(cp, e.fx, e.fy, kPx30m);
        CHECK_EQ(v.heightMm, e.h);
        CHECK_EQ(v.sxMmPerPx, e.sx);
        CHECK_EQ(v.syMmPerPx, e.sy);
        const CarrierCurvature c = evalCarrierCurvature(cp, e.fx, e.fy, kPx30m);
        CHECK_EQ(c.cxxMmPerPx2, e.cxx);
        CHECK_EQ(c.cyyMmPerPx2, e.cyy);
        CHECK_EQ(carrierCurvatureMmPerM2Q10(c, kPx30m), e.lapQ10);
    }

    // Analytic identity A: a CONSTANT control field must come back exactly,
    // with zero slope and zero curvature. This is the weights-partition
    // property of all three bases, end to end.
    for (int64_t h : {int64_t(0), int64_t(1234567), int64_t(-1234567)}) {
        fillPlane(cp, h, 0, 0);
        for (int64_t fx = 0; fx < kPx30m; fx += 1237)
            for (int64_t fy = 0; fy < kPx30m; fy += 2113) {
                const CarrierEval v = evalCarrier(cp, fx, fy, kPx30m);
                CHECK_EQ(v.heightMm, h);
                CHECK_EQ(v.sxMmPerPx, int64_t(0));
                CHECK_EQ(v.syMmPerPx, int64_t(0));
                const CarrierCurvature c = evalCarrierCurvature(cp, fx, fy, kPx30m);
                CHECK_EQ(c.cxxMmPerPx2, int64_t(0));
                CHECK_EQ(c.cyyMmPerPx2, int64_t(0));
            }
    }

    // Analytic identity B: on a LINEAR control field the x-slope is exactly the
    // per-pixel rise, everywhere in the cell, for either sign. (The x half is
    // exact because the quadratic basis partitions its denominator before any
    // division happens; the y half goes through the already-truncated row
    // values, so it is checked to within the truncation quantum instead.)
    for (int64_t b : {int64_t(0), int64_t(700), int64_t(-700), int64_t(45000)})
        for (int64_t c : {int64_t(0), int64_t(-1300), int64_t(1300)}) {
            fillPlane(cp, -20000, b, c);
            for (int64_t fx = 0; fx < kPx30m; fx += 971)
                for (int64_t fy = 0; fy < kPx30m; fy += 1543) {
                    const CarrierEval v = evalCarrier(cp, fx, fy, kPx30m);
                    CHECK_EQ(v.sxMmPerPx, b);
                    CHECK(absi(v.syMmPerPx - c) <= 2);
                }
        }

    // Analytic identity C: the slope currency is MM PER METRE, i.e. scale
    // invariant. The same physical grade must read the same number at 30 m,
    // 3 m and 1.875 m pixels — this is the v9 unit fix, and it is the property
    // the curvature currency below is modelled on.
    // (Within the truncation quantum, not exactly: the y half of the gradient
    // is read off already-truncated row values and the mm-per-metre conversion
    // truncates once more, so a 500 mm/m field reads 498-500. The property under
    // test is that the number does not SCALE with the pixel — a mm-per-pixel
    // currency would read 7500 / 750 / 468 here.)
    for (int64_t pxm : {kPx30m, kPx3m, kPxFine}) {
        // 250 mm/m of grade on each axis == 25% on each axis.
        fillPlane(cp, 0, pxm / 4, -pxm / 4);
        const CarrierEval v = evalCarrier(cp, pxm / 3, pxm / 7, pxm);
        CHECK(absi(carrierSlopeMmPerM(v, pxm) - 500) <= 3); // L1 of the two axes
    }
}

// ---------------------------------------------------------------------------
// (2) THE SHARPEST TEST. Curvature is exactly zero on a planar control field,
// at any tilt, at any pixel size, at any fraction within the cell.
//
// A linear field has identically zero second derivative, so this is not an
// approximation to check to within a tolerance — it must be the integer 0. Any
// error in the linear-basis derivation, in the denominator kCarrierCurveDen, or
// in the ORDER of the two separable stages breaks it. In particular, computing
// d2/dy2 by differencing the already-truncated stage-1 row values (the
// "obvious" reuse of evalCarrier's structure) fails here on any tilted plane
// whose row values straddle zero, because truncation toward zero is not
// translation invariant across a sign change.
// ---------------------------------------------------------------------------
VXC_TEST(carrier_curvature_is_zero_on_a_plane) {
    const int64_t tilts[] = {0, 1, -1, 5, -5, 1000, -1000, 12345, -12345, 999999};
    int64_t checked = 0;
    int64_t cp[16];
    for (int64_t a : tilts)
        for (int64_t b : tilts)
            for (int64_t c : tilts) {
                fillPlane(cp, a, b, c);
                for (int64_t pxm : {kPx30m, kPxFine, kPx3m})
                    for (int64_t fx = 0; fx < pxm; fx += pxm / 37 + 1)
                        for (int64_t fy = 0; fy < pxm; fy += pxm / 31 + 1) {
                            const CarrierCurvature cv = evalCarrierCurvature(cp, fx, fy, pxm);
                            CHECK_EQ(cv.cxxMmPerPx2, int64_t(0));
                            CHECK_EQ(cv.cyyMmPerPx2, int64_t(0));
                            CHECK_EQ(carrierCurvatureMmPerM2Q10(cv, pxm), int64_t(0));
                            // A planar field must also leave the gate at 1.0x,
                            // so a plain hillside is neither roughened nor
                            // smoothed.
                            CHECK_EQ(curvatureScaleQ10(carrierCurvatureMmPerM2Q10(cv, pxm)),
                                     kCurvatureScaleOneQ10);
                            ++checked;
                        }
            }
    std::printf("    [carrier] planar curvature: %lld (tilt, pixel size, fraction) samples, all "
                "exactly zero\n",
                (long long)checked);
}

// ---------------------------------------------------------------------------
// (3) Sign and magnitude against known convex and concave quadratic fields.
//
// SIGN CONVENTION (carrier.h): these are the plain second derivatives of
// height, so a CREST is negative and a HOLLOW is positive. The gate must
// roughen the crest and smooth the hollow.
// ---------------------------------------------------------------------------
VXC_TEST(carrier_curvature_sign_and_magnitude) {
    int64_t cp[16];

    // Axis separation: a quadratic in x alone must show up only in cxx.
    fillQuadratic(cp, 500, 0);
    for (int64_t fx = 0; fx < kPx30m; fx += 3001)
        for (int64_t fy = 0; fy < kPx30m; fy += 4001) {
            const CarrierCurvature c = evalCarrierCurvature(cp, fx, fy, kPx30m);
            CHECK_EQ(c.cxxMmPerPx2, int64_t(1000)); // 2A, exactly
            CHECK_EQ(c.cyyMmPerPx2, int64_t(0));
        }
    fillQuadratic(cp, 0, -500);
    for (int64_t fx = 0; fx < kPx30m; fx += 3001)
        for (int64_t fy = 0; fy < kPx30m; fy += 4001) {
            const CarrierCurvature c = evalCarrierCurvature(cp, fx, fy, kPx30m);
            CHECK_EQ(c.cxxMmPerPx2, int64_t(0));
            CHECK_EQ(c.cyyMmPerPx2, int64_t(-1000));
        }

    // Convex (crest, A < 0) and concave (hollow, A > 0), both axes, at both a
    // 30 m and a 1.875 m pixel. Magnitude is exact in mm per pixel^2 and is the
    // correct truncated quotient in the q10 mm/m^2 currency.
    for (int64_t A : {int64_t(900), int64_t(-900), int64_t(37), int64_t(-37)})
        for (int64_t pxm : {kPx30m, kPxFine}) {
            fillQuadratic(cp, A, A);
            const CarrierCurvature c = evalCarrierCurvature(cp, pxm / 3, pxm / 5, pxm);
            CHECK_EQ(c.cxxMmPerPx2, 2 * A);
            CHECK_EQ(c.cyyMmPerPx2, 2 * A);

            const int64_t lap = carrierCurvatureMmPerM2Q10(c, pxm);
            // Exact rational the currency conversion is defined to truncate:
            // (cxx + cyy) mm/px^2 * 1e6 / pxMm^2 mm/m^2, in q10.
            const int64_t num = 4 * A * kCurveQ10One * kMmPerMSquared;
            CHECK(isTruncatedQuotient(lap, num, pxm * pxm));
            CHECK((A < 0) == (lap < 0)); // sign follows the field

            const int64_t gate = curvatureScaleQ10(lap);
            if (A < 0)
                CHECK(gate > kCurvatureScaleOneQ10); // crest roughens
            else
                CHECK(gate < kCurvatureScaleOneQ10); // hollow smooths
            CHECK(gate <= kCurvatureScaleMaxQ10 && gate >= kCurvatureScaleMinQ10);
        }

    // THE UNIT IS SCALE INVARIANT. The same physical curvature — here
    // 4 mm per metre per metre — must read the same q10 number at a 30 m pixel
    // and at a 3 m pixel, even though the raw mm-per-pixel^2 second differences
    // differ by 100x. This is the property whose ABSENCE was the latent
    // mm-per-pixel bug v9 fixed on the slope side; do not let it regress here.
    fillQuadratic(cp, 900, 900); // 2A = 1800 mm/px^2 per axis at 30 m
    const int64_t lapCoarse =
        carrierCurvatureMmPerM2Q10(evalCarrierCurvature(cp, 7777, 13131, kPx30m), kPx30m);
    fillQuadratic(cp, 9, 9); // 2A = 18 mm/px^2 per axis at 3 m
    const int64_t lapFine =
        carrierCurvatureMmPerM2Q10(evalCarrierCurvature(cp, 777, 1313, kPx3m), kPx3m);
    CHECK_EQ(lapCoarse, int64_t(4 * kCurveQ10One)); // exactly 4 mm/m/m
    CHECK_EQ(lapFine, lapCoarse);

    // Truncation toward zero is symmetric, so negating the control field must
    // negate the curvature exactly. (floorDiv here would break this, which is
    // why the currency conversion deliberately truncates.)
    for (int seed = 0; seed < 8; ++seed) {
        int64_t neg[16];
        stencilAt(cp, seed * 13, seed * 7);
        for (int k = 0; k < 16; ++k) neg[k] = -cp[k];
        const CarrierCurvature a = evalCarrierCurvature(cp, 11111, 22222, kPx30m);
        const CarrierCurvature b = evalCarrierCurvature(neg, 11111, 22222, kPx30m);
        CHECK_EQ(b.cxxMmPerPx2, -a.cxxMmPerPx2);
        CHECK_EQ(b.cyyMmPerPx2, -a.cyyMmPerPx2);
        CHECK_EQ(carrierCurvatureMmPerM2Q10(b, kPx30m), -carrierCurvatureMmPerM2Q10(a, kPx30m));
    }
}

// ---------------------------------------------------------------------------
// (4) THE TEST THAT JUSTIFIES B-SPLINES. Curvature is continuous across a tile
// pixel boundary.
//
// Symptom 2 of this whole project was a gradient step at pixel lines. A
// Catmull-Rom carrier (C1) would fix that and still step in CURVATURE at every
// cell line — and §3c conditions detail amplitude on curvature, so the very
// same dead-straight 30 m seam would reappear in the texture amplitude. A
// uniform cubic B-spline is C2, so its second derivative is continuous (a
// linear B-spline: kinked at the knots, but never stepped).
//
// The assertion is comparative rather than absolute, which is what makes it
// sharp: a dense transect is walked across two pixel lines, and the step at a
// boundary must be no larger than the largest step anywhere in a cell interior.
// A C1 carrier fails this by orders of magnitude; a tolerance-based test would
// not.
// ---------------------------------------------------------------------------
namespace {
void transect(bool alongX, int64_t pxMm, int64_t& interiorMax, int64_t& straddleMax,
              int64_t& spread, int64_t& crossings) {
    interiorMax = 0;
    straddleMax = 0;
    crossings = 0;
    const int64_t step = pxMm / 512 + 1; // ~2 q10 fraction units per sample
    const int64_t begin = -pxMm - pxMm / 2, end = pxMm / 2;
    int64_t prev = 0, minV = 0, maxV = 0;
    bool first = true;
    for (int64_t u = begin; u <= end; u += step) {
        const int64_t p = floorDiv(u, pxMm);
        const int64_t f = u - p * pxMm;
        int64_t cp[16];
        int64_t v = 0;
        if (alongX) {
            stencilAt(cp, p, 0);
            v = carrierCurvatureMmPerM2Q10(evalCarrierCurvature(cp, f, pxMm / 4, pxMm), pxMm);
        } else {
            stencilAt(cp, 0, p);
            v = carrierCurvatureMmPerM2Q10(evalCarrierCurvature(cp, pxMm / 4, f, pxMm), pxMm);
        }
        if (first) {
            minV = maxV = v;
        } else {
            if (v < minV) minV = v;
            if (v > maxV) maxV = v;
            const int64_t d = absi(v - prev);
            if (floorDiv(u - step, pxMm) != p) {
                ++crossings;
                if (d > straddleMax) straddleMax = d;
            } else if (d > interiorMax) {
                interiorMax = d;
            }
        }
        prev = v;
        first = false;
    }
    spread = maxV - minV;
}
} // namespace

VXC_TEST(carrier_curvature_is_continuous_at_pixel_lines) {
    for (int64_t pxm : {kPx30m, kPxFine}) {
        for (int axis = 0; axis < 2; ++axis) {
            int64_t interiorMax = 0, straddleMax = 0, spread = 0, crossings = 0;
            transect(axis == 0, pxm, interiorMax, straddleMax, spread, crossings);
            std::printf("    [carrier] curvature transect px=%lld along %c: %lld pixel-line "
                        "crossings, spread %lld q10, max interior step %lld, max step AT a "
                        "pixel line %lld\n",
                        (long long)pxm, axis == 0 ? 'x' : 'y', (long long)crossings,
                        (long long)spread, (long long)interiorMax, (long long)straddleMax);
            // Non-vacuity: the transect must actually cross pixel lines, and the
            // control field must actually be curved, or "no step" is trivial.
            CHECK(crossings >= 2);
            CHECK(spread > 100);
            // The point of the test. +1 for the q10 quantum of the currency
            // conversion; nothing larger is allowed, so a C1 carrier (whose
            // curvature genuinely steps here) cannot pass.
            CHECK(straddleMax <= interiorMax + 1);
        }
    }
}

// ---------------------------------------------------------------------------
// (5) The gate: monotone, hits both clamps, and never exceeds its ceiling.
//
// The ceiling is already proved exhaustively at COMPILE time in carrier.h — the
// argument is clamped into [-knee, +knee] as the first operation, so the
// constexpr sweep over that interval is exhaustive over every int64 input, not
// a sample of it. This test is the runtime restatement plus the properties a
// consumer actually relies on, including that the two provisional amplitudes
// really are the plan's ~1.75x and ~0.5x.
//
// Amplifier::surfaceUpperBoundMm derives the world's surface bound from the
// detail envelope; if the gate is ever wired into evalSurface, that envelope
// gains a factor of kCurvatureScaleMaxQ10/1024 and a violation of the ceiling
// is a hole in the world. Hence a hard bound rather than a tolerance.
// ---------------------------------------------------------------------------
VXC_TEST(curvature_gate_is_monotone_and_bounded) {
    // The plan's numbers, pinned. These are PROVISIONAL and uncalibrated (see
    // carrier.h); the point of pinning them is that a recalibration is a
    // deliberate act with a test to update, not a quiet edit.
    CHECK_EQ(kCurvatureScaleMaxQ10, int64_t(1792)); // 1.75x
    CHECK_EQ(kCurvatureScaleMinQ10, int64_t(512));  // 0.5x
    CHECK_EQ(curvatureScaleQ10(0), kCurvatureScaleOneQ10);

    // Monotone non-increasing across, and well beyond, the whole clamped
    // domain: more concave must never mean rougher.
    int64_t prev = kCurvatureScaleMaxQ10;
    bool sawCeiling = false, sawFloor = false;
    const int64_t lo = -4 * kCurvatureKneeQ10, hi = 4 * kCurvatureKneeQ10;
    for (int64_t c = lo; c <= hi; ++c) {
        const int64_t v = curvatureScaleQ10(c);
        CHECK(v <= prev);
        CHECK(v <= kCurvatureScaleMaxQ10);
        CHECK(v >= kCurvatureScaleMinQ10);
        if (v == kCurvatureScaleMaxQ10) sawCeiling = true;
        if (v == kCurvatureScaleMinQ10) sawFloor = true;
        prev = v;
    }
    CHECK(sawCeiling);
    CHECK(sawFloor);

    // Saturation for anything, however extreme — including the values that
    // would overflow if the clamp were not the first operation.
    CHECK_EQ(curvatureScaleQ10(INT64_MIN), kCurvatureScaleMaxQ10);
    CHECK_EQ(curvatureScaleQ10(INT64_MAX), kCurvatureScaleMinQ10);
    CHECK_EQ(curvatureScaleQ10(-kCurvatureKneeQ10), kCurvatureScaleMaxQ10);
    CHECK_EQ(curvatureScaleQ10(kCurvatureKneeQ10), kCurvatureScaleMinQ10);

    // The gate is strictly responsive somewhere inside the knee — a gate that
    // is flat until it saturates would pass every check above.
    CHECK(curvatureScaleQ10(-kCurvatureKneeQ10 / 2) > kCurvatureScaleOneQ10);
    CHECK(curvatureScaleQ10(-kCurvatureKneeQ10 / 2) < kCurvatureScaleMaxQ10);
    CHECK(curvatureScaleQ10(kCurvatureKneeQ10 / 2) < kCurvatureScaleOneQ10);
    CHECK(curvatureScaleQ10(kCurvatureKneeQ10 / 2) > kCurvatureScaleMinQ10);
}

// ---------------------------------------------------------------------------
// (6) A measurement, not an assertion: what curvature the gate actually SEES on
// a realistic control raster, at a 30 m pixel and at the 1.875 m fine tier.
//
// Recorded because it is the single most important thing a calibration pass
// needs to know, and because it is the one property of the gate that does NOT
// transfer between tiers. The unit (mm per metre per metre) is scale invariant;
// the MAGNITUDE terrain exhibits is not, because curvature is a spectral
// quantity. A knee derived at 30 m posts saturates everywhere at 1.875 m posts.
// ---------------------------------------------------------------------------
VXC_TEST(curvature_magnitudes_on_a_realistic_raster) {
    for (int64_t pxm : {kPx30m, kPxFine}) {
        int64_t n = 0, sumAbs = 0, maxAbs = 0, saturated = 0;
        for (int64_t px = -20; px <= 20; ++px)
            for (int64_t py = -20; py <= 20; ++py) {
                int64_t cp[16];
                stencilAt(cp, px, py);
                const int64_t lap = carrierCurvatureMmPerM2Q10(
                    evalCarrierCurvature(cp, pxm / 3, pxm / 7, pxm), pxm);
                sumAbs += absi(lap);
                if (absi(lap) > maxAbs) maxAbs = absi(lap);
                const int64_t g = curvatureScaleQ10(lap);
                if (g == kCurvatureScaleMaxQ10 || g == kCurvatureScaleMinQ10) ++saturated;
                CHECK(g <= kCurvatureScaleMaxQ10 && g >= kCurvatureScaleMinQ10);
                ++n;
            }
        std::printf("    [carrier] curvature at px=%lld mm: mean |lap| %lld q10, max %lld q10, "
                    "gate saturated on %lld%% of %lld samples (knee %lld q10)\n",
                    (long long)pxm, (long long)(sumAbs / n), (long long)maxAbs,
                    (long long)(100 * saturated / n), (long long)n, (long long)kCurvatureKneeQ10);
    }
}
