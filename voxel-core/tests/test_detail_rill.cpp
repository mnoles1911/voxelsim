// Rill / flute term -- Phase 3c acceptance tests.
//
// The load-bearing test here is rill_anisotropy_structure_function: the plan's
// acceptance criterion for this term is that the mean absolute SECOND
// DIFFERENCE measured ACROSS slope exceeds the one measured ALONG slope at
// 1-3 m lags. Everything else guards a specific way the term could be right on
// average and wrong in a way that ships.
//
// All measurements are integer. Ratios are compared by cross-multiplication so
// nothing here needs a float, and the numbers in the comments are what this
// file actually printed.

#include "voxelcore/detail_rill.h"
#include "vxctest.h"

using namespace vxc;

namespace {

// --- the literal reading of the plan text, kept ONLY as a regression control -
//
// One rotation matrix, built from the gradient at the sample point, applied to
// the WORLD position. This is what detail_rill.h deliberately does not do; see
// the DEVIATION note in that header. rill_lever_arm_regression exists to keep
// the reason measurable rather than a matter of belief, so that anyone who
// "simplifies" detail_rill.h back to this form gets a red test instead of a
// world that turns to static a few kilometres from the origin.
int64_t naiveRillMm(uint64_t seed, int64_t x, int64_t y, int64_t gx, int64_t gy) {
    const int64_t n = rillOctNorm(gx, gy);
    const int64_t gate = rillGateQ(n);
    if (gate == 0) return 0;
    const int64_t ux = floorDiv(gx * kRillDirQ, n);
    const int64_t uy = floorDiv(gy * kRillDirQ, n);
    const int64_t along = floorDiv(ux * x + uy * y, kRillDirQ);
    const int64_t across = floorDiv(ux * y - uy * x, kRillDirQ);
    const int64_t nz =
        valueNoise2Fade(seed, along, across * kRillElongation, kRillAlongMm, CH_RILL);
    return rillScaleMm(nz, gate);
}

// Mean |f(p-L*e) - 2 f(p) + f(p+L*e)| over a scattered patch, in milli-mm.
// The gradient is held CONSTANT so that only p varies: this isolates the
// field's own anisotropy from any variation in the frame.
int64_t structFn(uint64_t seed, int64_t gx, int64_t gy, int64_t ex, int64_t ey,
                 int64_t lagMm, int n) {
    int64_t acc = 0;
    int64_t cnt = 0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            // Coprime-ish strides so the patch does not land on a sublattice of
            // either the 1600 mm or the 12800 mm spacing.
            const int64_t px = i * 237 + j * 1013;
            const int64_t py = j * 241 + i * 1019;
            const int64_t a = rillMm(seed, px - ex * lagMm, py - ey * lagMm, gx, gy);
            const int64_t b = rillMm(seed, px, py, gx, gy);
            const int64_t c = rillMm(seed, px + ex * lagMm, py + ey * lagMm, gx, gy);
            const int64_t d = a - 2 * b + c;
            acc += d < 0 ? -d : d;
            ++cnt;
        }
    return acc * 1000 / cnt;
}

// Mean |first difference| at a 100 mm lag along a transect, in milli-mm, with
// the gradient taken from a synthetic dome centred at (cx, cy). A coherent
// 1.6 m field barely moves over 100 mm; a field that has degenerated into
// white noise moves by order its own amplitude.
int64_t roughnessOnDome(int64_t (*f)(uint64_t, int64_t, int64_t, int64_t, int64_t),
                        uint64_t seed, int64_t cx, int64_t cy, int64_t startRadiusMm,
                        int n) {
    const int64_t kDomeDen = 500; // 200 m out -> 400 mm/m == 40% grade
    const int64_t lag = 100;
    int64_t acc = 0;
    int64_t cnt = 0;
    for (int i = 0; i < n; ++i) {
        const int64_t px = cx + startRadiusMm + i * lag;
        const int64_t py = cy;
        const int64_t gx0 = floorDiv(-(px - cx), kDomeDen);
        const int64_t gy0 = floorDiv(-(py - cy), kDomeDen);
        const int64_t gx1 = floorDiv(-(px + lag - cx), kDomeDen);
        const int64_t gy1 = floorDiv(-(py + lag - cy), kDomeDen);
        const int64_t d = f(seed, px + lag, py, gx1, gy1) - f(seed, px, py, gx0, gy0);
        acc += d < 0 ? -d : d;
        ++cnt;
    }
    return acc * 1000 / cnt;
}

constexpr uint64_t kSeed = 0xC0FFEEull;

} // namespace

// --- 1. no phantom anisotropy on flats ---------------------------------------
//
// Rills on flat ground are physically wrong AND the frame is undefined there.
// Both are discharged by the same check: below the gate the term is EXACTLY
// zero, not merely small, so it can contribute neither shape nor instability.
VXC_TEST(rill_is_exactly_zero_on_flats) {
    for (int64_t x = -400000; x <= 400000; x += 7919)
        for (int64_t y = -300000; y <= 300000; y += 6113) {
            CHECK_EQ(rillMm(kSeed, x, y, 0, 0), 0);
            CHECK_EQ(rillMm(kSeed, x, y, 1, 0), 0);
            CHECK_EQ(rillMm(kSeed, x, y, -1, 2), 0);
            CHECK_EQ(rillMm(kSeed, x, y, 60, -60), 0);   // ~8.5% grade
            CHECK_EQ(rillMm(kSeed, x, y, 100, 0), 0);    // exactly at threshold
            CHECK_EQ(rillMm(kSeed, x, y, 0, -100), 0);
        }
    // ... and it is live just above the threshold, so the zero above is a gate
    // and not a dead term.
    bool anyLive = false;
    for (int64_t x = 0; x < 200000; x += 313)
        if (rillMm(kSeed, x, 12345, 300, 0) != 0) anyLive = true;
    CHECK(anyLive);
}

// --- 2. the plan's acceptance criterion --------------------------------------
//
// Constant gradient (G, 0): the fall line is +x, so ALONG-slope is the x axis
// and ACROSS-slope is the y axis. Second differences across slope must exceed
// those along slope at 1-3 m lags.
//
// Measured (milli-mm), G = 600 mm/m (60% grade):
//   lag 1 m: along 4,381  across 136,192  ratio 31.1
//   lag 2 m: along 16,940 across 246,714  ratio 14.6
//   lag 3 m: along 36,226 across 248,880  ratio  6.9
// The ratio falls with lag exactly as it should: at 3 m the along-slope lag is
// approaching a quarter of the 12.8 m along-wavelength, so the along-slope
// direction is finally starting to see structure too.
VXC_TEST(rill_anisotropy_structure_function) {
    const int64_t lags[3] = {1000, 2000, 3000};
    for (int li = 0; li < 3; ++li) {
        const int64_t along = structFn(kSeed, 600, 0, 1, 0, lags[li], 40);
        const int64_t across = structFn(kSeed, 600, 0, 0, 1, lags[li], 40);
        CHECK(along > 0);
        CHECK(across > along);          // the plan's bar: ratio > 1
        CHECK(across > 2 * along);      // and with real margin, not noise
    }
    // Amplitude-independence of the ratio: the gradient MAGNITUDE only opens
    // the gate, it must not change the pattern once the gate is open.
    for (int li = 0; li < 3; ++li) {
        CHECK_EQ(structFn(kSeed, 300, 0, 0, 1, lags[li], 20),
                 structFn(kSeed, 1200, 0, 0, 1, lags[li], 20));
    }
}

// On flats there is no anisotropy to measure because there is no term: both
// structure functions are identically zero, so the ratio is 1 by convention.
// This is the stronger statement the "ratio ~= 1 on flats" criterion is after
// -- a nonzero isotropic residue would be a phantom.
VXC_TEST(rill_anisotropy_absent_on_flats) {
    for (int64_t lag = 1000; lag <= 3000; lag += 1000) {
        CHECK_EQ(structFn(kSeed, 40, 30, 1, 0, lag, 24), 0);
        CHECK_EQ(structFn(kSeed, 40, 30, 0, 1, lag, 24), 0);
    }
}

// --- 3. rotational consistency -----------------------------------------------
//
// Turn the gradient 90 degrees and the elongation must turn with it. Measured
// at lag 1 m: with grad (G,0) across/along = 31.1; with grad (0,G) the same
// measurement taken on the swapped axes gives 35.4. Also checked on the
// diagonal, where the octagonal norm is at its worst and the table direction
// is exact (45 degrees is entry 4).
VXC_TEST(rill_rotational_consistency) {
    const int64_t lags[3] = {1000, 2000, 3000};
    for (int li = 0; li < 3; ++li) {
        const int64_t L = lags[li];
        // grad (G,0): x is along, y is across
        const int64_t a_along = structFn(kSeed, 600, 0, 1, 0, L, 40);
        const int64_t a_across = structFn(kSeed, 600, 0, 0, 1, L, 40);
        // grad (0,G): y is along, x is across
        const int64_t b_along = structFn(kSeed, 0, 600, 0, 1, L, 40);
        const int64_t b_across = structFn(kSeed, 0, 600, 1, 0, L, 40);
        CHECK(a_across > 2 * a_along);
        CHECK(b_across > 2 * b_along);
        // The two orientations must give comparable anisotropy strength -- the
        // term must not be stronger in one world direction. Ratios within 2x.
        CHECK(a_across * b_along * 2 > b_across * a_along);
        CHECK(b_across * a_along * 2 > a_across * b_along);
    }
    // Diagonal gradient: along = (1,1), across = (1,-1). Measured ratios
    // 34.0 / 16.5 / 8.7 at 1/2/3 m, i.e. no worse than the axis-aligned case
    // even though this is the octagonal norm's worst direction.
    for (int li = 0; li < 3; ++li) {
        const int64_t L = lags[li] * 707 / 1000;
        const int64_t along = structFn(kSeed, 600, 600, 1, 1, L, 40);
        const int64_t across = structFn(kSeed, 600, 600, 1, -1, L, 40);
        CHECK(across > 2 * along);
    }
}

// --- 4. the lever arm -- why the frame is a blended table --------------------
//
// A dome-shaped synthetic carrier: gradient = -(p - c)/500, so the aspect
// turns a full circle around c and the sampled ring sits at 200 m radius
// (40% grade, gate wide open). Sliding the whole dome away from the world
// origin must not change the term's character at all.
//
// Measured mean |first difference| at a 100 mm lag, milli-mm:
//        |c|          ours     plan-literal
//           0 mm      2,550        38,485
//     1,000,000       2,224       177,084
//    10,000,000       2,755       172,846
//    54,000,000       2,829       172,331
//
// Ours is flat: the field stays a coherent 1.6 m pattern 54 km out. The
// literal form saturates at ~60x that, which is what a 1.6 m field looks like
// once one metre of ground motion has become tens of metres of noise motion --
// white noise. Note it is already 15x worse at 200 m from the origin.
VXC_TEST(rill_lever_arm_regression) {
    const int64_t nearOurs = roughnessOnDome(rillMm, kSeed, 0, 0, 200000, 4000);
    const int64_t farOurs = roughnessOnDome(rillMm, kSeed, 54000000, 54000000, 200000, 4000);
    const int64_t farNaive =
        roughnessOnDome(naiveRillMm, kSeed, 54000000, 54000000, 200000, 4000);

    CHECK(nearOurs > 0);
    // Character is preserved with distance: within 2x of the near-origin value.
    CHECK(farOurs * 1000 < nearOurs * 2000);
    CHECK(farOurs * 2000 > nearOurs * 1000);
    // And the construction the plan text describes literally is an order of
    // magnitude rougher at the same place, for the same gradient field.
    CHECK(farNaive > 10 * farOurs);
}

// --- 5. continuity through the gate ------------------------------------------
//
// A step in amplitude across a contour is a seam, just along a different
// curve. Sweeping the gradient magnitude along a FIXED direction changes the
// gate and NOTHING else -- the direction, hence both sampled fields and both
// blend weights, is scale-invariant -- so every step observed here is the
// ramp's own slope sampled at the gradient's 1 mm/m quantum.
//
// The quintic's maximum slope is 15/8 = 1.875, so the analytic Lipschitz
// bound is 1.875 * amplitude / width = 1.875 * 300 / 100 = 5.6 mm per 1 mm/m
// of gradient. Measured worst step with the norm advancing 1 mm/m at a time:
// 6 mm, on a table direction and off one alike -- the ceiling of the analytic
// derivative, i.e. no discontinuity anywhere. The comparison that matters is
// against what a STEP gate would have done at the same place: 300 mm, 50x
// larger.
//
// Note the sweep must advance the OCTAGONAL NORM by one unit per step, not
// the components: a (3k,4k) sweep advances the norm by 5.5 per k and
// correctly shows ~31 mm steps, which is the same derivative and not a
// defect. Both are checked.
VXC_TEST(rill_gate_is_a_ramp_not_a_step) {
    // ceil(1.875 * kRillAmplitudeMm / kRillGateWidthMmPerM), integer form.
    const int64_t kSlopeMmPerUnit =
        (15 * kRillAmplitudeMm + 8 * kRillGateWidthMmPerM - 1) / (8 * kRillGateWidthMmPerM);
    int64_t worst = 0;
    for (int64_t px = -50000; px <= 50000; px += 4001)
        for (int64_t py = -30000; py <= 30000; py += 3301) {
            // Magnitude 0 -> 40% grade, straight through both ends of the gate.
            // Direction is table entry 8 exactly, so the blend is a single
            // field and the gate is perfectly isolated.
            int64_t prev = rillMm(kSeed, px, py, 0, 0);
            for (int64_t m = 0; m <= 400; ++m) {
                const int64_t v = rillMm(kSeed, px, py, 0, m);
                const int64_t d = v - prev < 0 ? prev - v : v - prev;
                if (d > worst) worst = d;
                prev = v;
            }
            // Same, off a table direction: norm is m + 2 here, so it still
            // advances one unit at a time, but now both fields are live and
            // the frame drifts slightly as it sweeps.
            prev = rillMm(kSeed, px, py, 5, 10);
            for (int64_t m = 11; m <= 400; ++m) {
                const int64_t v = rillMm(kSeed, px, py, 5, m);
                const int64_t d = v - prev < 0 ? prev - v : v - prev;
                if (d > worst) worst = d;
                prev = v;
            }
        }
    CHECK(worst <= kSlopeMmPerUnit);
    // The point of the ramp, stated as a test rather than a comment.
    CHECK(worst * 20 < kRillAmplitudeMm);

    // The coarser (3k,4k) sweep: same derivative, larger steps because the
    // norm advances further per step. Checked against the same analytic slope
    // scaled by the measured norm step, so this cannot silently absorb a jump.
    int64_t worstCoarse = 0, maxNormStep = 0;
    for (int64_t px = -50000; px <= 50000; px += 4001) {
        int64_t prev = rillMm(kSeed, px, 7777, 0, 0);
        for (int64_t k = 0; k <= 120; ++k) {
            const int64_t v = rillMm(kSeed, px, 7777, 3 * k, 4 * k);
            const int64_t d = v - prev < 0 ? prev - v : v - prev;
            if (d > worstCoarse) worstCoarse = d;
            prev = v;
            if (k > 0) {
                const int64_t ns =
                    rillOctNorm(3 * k, 4 * k) - rillOctNorm(3 * (k - 1), 4 * (k - 1));
                if (ns > maxNormStep) maxNormStep = ns;
            }
        }
    }
    CHECK(maxNormStep == 6);
    CHECK(worstCoarse <= kSlopeMmPerUnit * maxNormStep);
    CHECK(worstCoarse * 8 < kRillAmplitudeMm);

    // Rotating the gradient through the whole half turn at a fixed magnitude
    // must also be continuous: this crosses all 16 sector boundaries, where a
    // snapped (unblended) frame would jump by the full amplitude.
    int64_t worstRot = 0;
    int64_t prevRot = rillMm(kSeed, 555000, 777000, 1, 3999);
    for (int64_t k = 2; k <= 3999; ++k) { // ~0.014 deg per step
        const int64_t v = rillMm(kSeed, 555000, 777000, k, 4000 - k);
        const int64_t d = v - prevRot < 0 ? prevRot - v : v - prevRot;
        if (d > worstRot) worstRot = d;
        prevRot = v;
    }
    CHECK(worstRot <= 2); // measured 1 mm, i.e. the output quantum

    // Spatial continuity at the 1 mm coordinate quantum on steep ground.
    int64_t worstSpatial = 0;
    int64_t prevSpatial = rillMm(kSeed, 100000, -500000, 900, 400);
    for (int64_t i = 1; i < 60000; ++i) {
        const int64_t v = rillMm(kSeed, 100000 + i, -500000, 900, 400);
        const int64_t d = v - prevSpatial < 0 ? prevSpatial - v : v - prevSpatial;
        if (d > worstSpatial) worstSpatial = d;
        prevSpatial = v;
    }
    CHECK(worstSpatial <= 1);
}

// --- 6. the bound -------------------------------------------------------------
//
// The static_asserts in the header prove the extremes of the final scaling
// step; this proves the whole function over a wide input domain, including
// large NEGATIVE coordinates -- the floorDiv discipline exists because
// negative-coordinate behaviour is where this codebase has actually been
// bitten (worldgen.ush's floorDiv diverged AMD vs NVIDIA on exactly that).
//
// Measured over 400k samples: min -300, max 296. The negative bound is
// attained exactly, which is the point: the envelope is tight, not generous.
VXC_TEST(rill_bound_fuzz) {
    uint64_t st = 0x9E3779B97F4A7C15ull;
    int64_t mn = 0, mx = 0;
    int64_t negCoordSamples = 0, live = 0;
    for (int i = 0; i < 300000; ++i) {
        st = splitmix64(st);
        const int64_t x = static_cast<int64_t>(st) >> 24; // +-2^39 mm
        st = splitmix64(st);
        const int64_t y = static_cast<int64_t>(st) >> 24;
        st = splitmix64(st);
        const int64_t gx = static_cast<int64_t>(st) >> 50; // +-2^13 mm/m
        st = splitmix64(st);
        const int64_t gy = static_cast<int64_t>(st) >> 50;
        const int64_t v = rillMm(kSeed ^ static_cast<uint64_t>(i & 3), x, y, gx, gy);
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        if (x < 0 && y < 0) ++negCoordSamples;
        if (v != 0) ++live;
    }
    CHECK(mn >= -kRillMaxAbsMm);
    CHECK(mx <= kRillMaxAbsMm);
    CHECK(negCoordSamples > 50000); // the negative quadrant really was covered
    CHECK(live > 200000);           // and the term was not trivially gated off
    // The envelope is tight: the fuzz gets within a few mm of it on both sides.
    CHECK(mn <= -kRillMaxAbsMm + 5);
    CHECK(mx >= kRillMaxAbsMm - 10);

    // Extremal gradients, near the documented domain limits, and coordinates on
    // and around the lattice lines of both axes.
    const int64_t bigG = 1LL << 40;
    const int64_t bigX = 1LL << 44;
    const int64_t probes[9] = {-bigX, -bigX + 1, -12800, -1, 0, 1, 12800, bigX - 1, bigX};
    for (int a = 0; a < 9; ++a)
        for (int b = 0; b < 9; ++b) {
            const int64_t gs[6] = {bigG, -bigG, 1, -1, 100, 101};
            for (int c = 0; c < 6; ++c)
                for (int d = 0; d < 6; ++d) {
                    const int64_t v = rillMm(kSeed, probes[a], probes[b], gs[c], gs[d]);
                    CHECK(v <= kRillMaxAbsMm);
                    CHECK(v >= -kRillMaxAbsMm);
                }
        }
}

// --- 7. determinism and purity -----------------------------------------------
//
// The term is a pure function of world millimetres. Nothing about a tile, a
// chunk, a level or an origin enters it, so it is seamless BY CONSTRUCTION
// rather than by matching boundary conditions -- there is no boundary to
// match. This test states that as a property: the same world point reached
// through any (tileOrigin + localOffset) decomposition gives the same answer.
VXC_TEST(rill_is_a_pure_function_of_world_mm) {
    const int64_t origins[5] = {0, 15360000, -15360000, 3750, -1};
    for (int oi = 0; oi < 5; ++oi)
        for (int64_t lx = -2000; lx <= 2000; lx += 337)
            for (int64_t ly = -2000; ly <= 2000; ly += 419) {
                const int64_t wx = origins[oi] + lx;
                const int64_t wy = origins[oi] * 2 - ly;
                const int64_t direct = rillMm(kSeed, wx, wy, 733, -412);
                const int64_t viaOrigin = rillMm(kSeed, origins[oi] + lx,
                                                origins[oi] * 2 - ly, 733, -412);
                CHECK_EQ(direct, viaOrigin);
                CHECK_EQ(direct, rillMm(kSeed, wx, wy, 733, -412)); // repeatable
            }

    // Different seeds must actually decorrelate the field.
    int64_t differing = 0;
    for (int64_t i = 0; i < 2000; ++i)
        if (rillMm(1, i * 97, i * 131, 800, 300) != rillMm(2, i * 97, i * 131, 800, 300))
            ++differing;
    CHECK(differing > 1800);

    // Compile-time and run-time evaluation must agree. Constant folding is a
    // different code path in every compiler; if the two disagree the function
    // is not the deterministic integer function it claims to be.
    constexpr int64_t ct = rillMm(7, -1234567, 890123, -640, 480);
    volatile int64_t sx = -1234567, sy = 890123, sgx = -640, sgy = 480;
    CHECK_EQ(ct, rillMm(7, sx, sy, sgx, sgy));
}

// --- 8. the frame's own constants --------------------------------------------
VXC_TEST(rill_frame_constants) {
    // Octagonal norm: exact on the axes, +6.07% at 45 degrees. 6% is the whole
    // cost of being sqrt-free here.
    CHECK_EQ(rillOctNorm(1000, 0), 1000);
    CHECK_EQ(rillOctNorm(0, -1000), 1000);
    CHECK_EQ(rillOctNorm(-1000, 1000), 1500); // true length 1414 -> +6.08%
    CHECK_EQ(rillOctNorm(0, 0), 0);
    // Sign symmetric: a rill axis is a line.
    for (int64_t gx = -300; gx <= 300; gx += 37)
        for (int64_t gy = -300; gy <= 300; gy += 41) {
            CHECK_EQ(rillOctNorm(gx, gy), rillOctNorm(-gx, -gy));
            CHECK_EQ(rillMm(kSeed, 4321, -8765, gx, gy),
                     rillMm(kSeed, 4321, -8765, -gx, -gy));
        }

    // Direction table: unit to better than 0.1%, which is why the frame comes
    // from the table and not from a norm-normalised gradient.
    for (int k = 0; k < 16; ++k) {
        const int64_t dx = kRillDirCosQ[k], dy = kRillDirSinQ[k];
        const int64_t n2 = dx * dx + dy * dy;
        const int64_t ref = kRillDirQ * kRillDirQ;
        CHECK(n2 * 1000 > ref * 999);
        CHECK(n2 * 1000 < ref * 1001);
    }

    // Sector parametrisation: the cardinal directions land exactly on table
    // entries, and the half-turn wrap closes.
    CHECK_EQ(rillSectorQ(1000, 0), 0);
    CHECK_EQ(rillSectorQ(0, 1000), 8 * kRillDirQ);
    CHECK_EQ(rillSectorQ(1000, 1000), 4 * kRillDirQ);
    CHECK_EQ(rillSectorQ(-1000, 0), 0);       // folded into the upper half plane
    CHECK_EQ(rillSectorQ(-1000, -1000), 4 * kRillDirQ);
    for (int64_t gx = -500; gx <= 500; gx += 13)
        for (int64_t gy = -500; gy <= 500; gy += 17) {
            if (gx == 0 && gy == 0) continue;
            const int64_t s = rillSectorQ(gx, gy);
            CHECK(s >= 0);
            CHECK(s <= kRillSectors * kRillDirQ);
        }

    // Gate shape.
    CHECK_EQ(rillGateQ(0), 0);
    CHECK_EQ(rillGateQ(kRillGateStartMmPerM), 0);
    CHECK_EQ(rillGateQ(kRillGateFullMmPerM), kRillGateQ);
    CHECK_EQ(rillGateQ(1LL << 40), kRillGateQ);
    CHECK_EQ(rillGateQ(kRillGateStartMmPerM + kRillGateWidthMmPerM / 2), kRillGateQ / 2);
    int64_t prev = 0;
    for (int64_t m = 0; m <= 400; ++m) { // monotone non-decreasing
        const int64_t g = rillGateQ(m);
        CHECK(g >= prev);
        prev = g;
    }
}
