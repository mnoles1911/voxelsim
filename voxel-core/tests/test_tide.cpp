// THE TIDE'S ARITHMETIC, PINNED (docs/water-ocean-tides-plan-2026-09-04.md
// Phase A gates: "voxel-core tide tests green", each with a must-fail arm).
//
// EVERY ASSERTION HERE IS AN EXACT INTEGER EQUALITY OR A SWEPT INEQUALITY
// THAT A ONE-LINE REGRESSION WOULD BREAK. No tolerances: the whole point of
// tide.h's integer phase + compile-time table is that the answer is a
// specific number on every machine, so a test that accepted "close" would be
// testing away the property the header exists for. The pinned literals below
// (250, 550, -1050, 46341, 6981...) are the cross-machine contract -- CI runs
// this file under three compilers, and a compiler whose constexpr integer
// arithmetic produced different literals would be the bug being hunted.
//
// THE MUST-FAIL DISCIPLINE (test-must-be-able-to-fail): each section says
// what edit would trip it. A suite of identities that hold for any table is
// not a suite.

#include <cstdint>

#include "voxelcore/tide.h"
#include "vxctest.h"

using namespace vxc;

namespace {

// The shipping default spec (voxel.Water.Tide.Spec "720:800:0,360:250:250"),
// built here from literals so a change to the ENGINE default cannot silently
// retune what this file pins -- the two are supposed to be the same numbers,
// and this copy is the one under version control of the tests.
TideSpec defaultSpec() {
    TideSpec s;
    s.comp[0] = {720, 800, 0};
    s.comp[1] = {360, 250, 250};
    s.count = 2;
    return s;
}

TideSpec singleSpec() {
    TideSpec s;
    s.comp[0] = {720, 800, 0};
    s.count = 1;
    return s;
}

} // namespace

// --- the table itself -------------------------------------------------------
//
// Fails if: the quadrant reflection is rewritten with arithmetic (symmetry
// breaks by a bit), the Q30->Q16 rounding changes, the Taylor series loses a
// term (46341 moves), or the pi/2 endpoint pin is dropped.
VXC_TEST(tide_sine_table_cardinals_and_pinned_interior_points) {
    CHECK_EQ(kTideSin.v[0], 0);
    CHECK_EQ(kTideSin.v[1024], kTideSinOne);
    CHECK_EQ(kTideSin.v[2048], 0);
    CHECK_EQ(kTideSin.v[3072], -kTideSinOne);
    // Interior regression pins: round(sin(pi/4) * 65536) and
    // round(sin(pi/8) * 65536), computed offline. A one-term Taylor change or
    // a rounding change moves these.
    CHECK_EQ(kTideSin.v[512], 46341);
    CHECK_EQ(kTideSin.v[256], 25080);
}

VXC_TEST(tide_sine_table_symmetries_are_exact) {
    for (int32_t k = 0; k < kTideLutSize / 2; ++k) {
        // Half-turn antisymmetry, to the bit (construction copies, never
        // re-evaluates).
        CHECK_EQ(kTideSin.v[k + 2048], -kTideSin.v[k]);
    }
    for (int32_t k = 0; k <= 1024; ++k) {
        // Quarter-turn reflection.
        CHECK_EQ(kTideSin.v[2048 - k], kTideSin.v[k]);
    }
    // Non-decreasing over the rising quarter (ties near the crest are
    // legitimate at Q16; a DECREASE anywhere is a series or rounding bug).
    for (int32_t k = 1; k <= 1024; ++k) {
        CHECK(kTideSin.v[k] >= kTideSin.v[k - 1]);
    }
    // And bounded -- no entry may escape Q16 one.
    for (int32_t k = 0; k < kTideLutSize; ++k) {
        CHECK(kTideSin.v[k] >= -kTideSinOne && kTideSin.v[k] <= kTideSinOne);
    }
}

// --- offset: exact values at known phases -----------------------------------
//
// Fails if: tidePhaseIndex's floorMod/scale order changes, the per-component
// floorDiv changes, phaseMilliTurns stops mapping 250 -> a quarter turn, or
// the table moves. These are the numbers the engine's datum will stand at.
VXC_TEST(tide_offset_exact_mm_at_cardinal_epochs) {
    const TideSpec s = defaultSpec();
    // epoch 0: comp0 at sin(0)=0; comp1 a quarter turn ahead by phase -> +250.
    CHECK_EQ(tideOffsetMm(s, 0), 250);
    // 180 s = comp0's quarter period -> +800; comp1 at half its own period
    // plus the quarter-turn phase -> -250.
    CHECK_EQ(tideOffsetMm(s, 180000), 550);
    // 540 s: comp0 at three quarters -> -800; comp1 again at -250. This is
    // the spec's astronomical LOW -- exactly -tideMaxMm, which pins that the
    // bound is attained, not merely respected.
    CHECK_EQ(tideOffsetMm(s, 540000), -1050);
    // A lone component sweeps to exactly +-amplitude at its cardinals.
    const TideSpec one = singleSpec();
    CHECK_EQ(tideOffsetMm(one, 180000), 800);
    CHECK_EQ(tideOffsetMm(one, 540000), -800);
    CHECK_EQ(tideOffsetMm(one, 0), 0);
    CHECK_EQ(tideOffsetMm(one, 360000), 0);
}

// --- periodicity ------------------------------------------------------------
//
// The default spec's periods are 720 s and 360 s, so 720,000 ms is a common
// period: the offset must repeat EXACTLY, including at negative epochs (a
// clock that starts before zero is a floorMod correctness test, and truncating
// division here once cost this codebase a seam). Fails if floorMod becomes %
// anywhere on the phase path.
VXC_TEST(tide_offset_is_exactly_periodic_including_negative_epochs) {
    const TideSpec s = defaultSpec();
    for (int64_t e = -1440000; e <= 1440000; e += 7321) { // 7321: coprime-ish, hits odd phases
        CHECK_EQ(tideOffsetMm(s, e), tideOffsetMm(s, e + 720000));
        CHECK_EQ(tideOffsetMm(s, e), tideOffsetMm(s, e - 720000));
    }
}

// --- determinism across evaluation orders ------------------------------------
//
// Two properties, both of which a "sum in Q16, divide once" rewrite would
// break: (1) component order cannot matter; (2) a spec's offset equals the
// sum of its components evaluated as lone specs -- i.e. no evaluation
// STRATEGY (incremental, split across systems, re-grouped) can disagree with
// another. This is the property that lets the engine reason about one
// component (NormPhase reads comp[0]) while the datum reads the sum.
VXC_TEST(tide_offset_is_order_free_and_decomposes) {
    const TideSpec s = defaultSpec();
    TideSpec swapped;
    swapped.comp[0] = s.comp[1];
    swapped.comp[1] = s.comp[0];
    swapped.count = 2;
    TideSpec lone0 = s, lone1 = swapped;
    lone0.count = 1;
    lone1.count = 1;
    for (int64_t e = 0; e <= 720000; e += 3137) {
        const int32_t whole = tideOffsetMm(s, e);
        CHECK_EQ(whole, tideOffsetMm(swapped, e));
        CHECK_EQ(whole, tideOffsetMm(lone0, e) + tideOffsetMm(lone1, e));
    }
}

// --- the bound --------------------------------------------------------------
//
// tideMaxMm is 1050 for the default spec (fails if the amplitude sum starts
// skipping live components or counting inert ones), and no swept offset may
// exceed it. The sweep also re-attains the bound (the -1050 pin above), so
// this cannot be satisfied by an offset function that returns 0 everywhere.
VXC_TEST(tide_max_bounds_every_offset_and_is_1050_for_default_spec) {
    const TideSpec s = defaultSpec();
    CHECK_EQ(tideMaxMm(s), 1050);
    CHECK_EQ(tideMaxMm(singleSpec()), 800);
    for (int64_t e = 0; e <= 720000; e += 997) {
        const int32_t off = tideOffsetMm(s, e);
        CHECK(off >= -1050 && off <= 1050);
    }
}

// --- zero and malformed specs ------------------------------------------------
//
// The "ships dark" contract: an empty spec is EXACTLY zero everywhere -- the
// engine's Tide=0 arm composes with seaLevelNowMm == kSeaLevelMm and must be
// bit-identical to a build with no tide code at all. Inert (periodS <= 0)
// components contribute nothing rather than dividing by zero, and are
// excluded from the bound. Fails if a default-constructed TideSpec ever gains
// a live component, or the periodS guard is dropped.
VXC_TEST(tide_zero_and_inert_specs_answer_exactly_zero) {
    const TideSpec zero;
    for (int64_t e = -720000; e <= 720000; e += 60000) {
        CHECK_EQ(tideOffsetMm(zero, e), 0);
        CHECK_EQ(tideVelocityUmPerS(zero, e), int64_t(0));
    }
    CHECK_EQ(tideMaxMm(zero), 0);
    TideSpec inert;
    inert.comp[0] = {0, 5000, 0};  // periodS 0: inert, whatever the amplitude
    inert.comp[1] = {-7, 5000, 0}; // negative period: inert
    inert.count = 2;
    CHECK_EQ(tideOffsetMm(inert, 12345), 0);
    CHECK_EQ(tideMaxMm(inert), 0);
}

// --- velocity ---------------------------------------------------------------
//
// Analytic, via the table a quarter turn ahead. 6981 um/s is
// floor(800 * 6283 / 720): the lone component's peak rate, at its zero
// crossing (cos = 1). At the crest the velocity is exactly 0 -- a finite-
// difference implementation would not land 0 there, which is this test's
// must-fail arm against that rewrite. Sign convention: rising tide, positive
// velocity.
VXC_TEST(tide_velocity_exact_at_cardinals_and_antisymmetric) {
    const TideSpec one = singleSpec();
    CHECK_EQ(tideVelocityUmPerS(one, 0), int64_t(6981));
    CHECK_EQ(tideVelocityUmPerS(one, 180000), int64_t(0));   // crest
    CHECK_EQ(tideVelocityUmPerS(one, 540000), int64_t(0));   // trough
    CHECK_EQ(tideVelocityUmPerS(one, 360000), int64_t(-6982)); // falling zero crossing
    // (6982, not -6981: floorDiv's downward bias on the negative lobe --
    // documented in tide.h, and pinned here so a rounding-mode change shows.)
}

// --- quantisation ------------------------------------------------------------
//
// Floor-to-quantum, sign-stable: uniform bins with NO double-width dead band
// straddling zero. The pins below are the ebb-side behaviour a truncating
// divide gets wrong (-1 -> 0 instead of -25); the sweep asserts monotonicity
// and the bin identity q(x) <= x < q(x) + quantum, which together are the
// "crosses each threshold exactly once" property the datum stepper relies on.
VXC_TEST(tide_quantise_is_floor_sign_stable_and_monotone) {
    CHECK_EQ(quantiseTideMm(0, 25), 0);
    CHECK_EQ(quantiseTideMm(24, 25), 0);
    CHECK_EQ(quantiseTideMm(25, 25), 25);
    CHECK_EQ(quantiseTideMm(49, 25), 25);
    CHECK_EQ(quantiseTideMm(-1, 25), -25); // the truncation trap, pinned
    CHECK_EQ(quantiseTideMm(-25, 25), -25);
    CHECK_EQ(quantiseTideMm(-26, 25), -50);
    CHECK_EQ(quantiseTideMm(1050, 25), 1050);
    CHECK_EQ(quantiseTideMm(-1050, 25), -1050);
    // quantum <= 0 is the identity, not a crash.
    CHECK_EQ(quantiseTideMm(137, 0), 137);
    CHECK_EQ(quantiseTideMm(-137, -5), -137);
    int32_t prev = quantiseTideMm(-1100, 25);
    for (int32_t x = -1099; x <= 1100; ++x) {
        const int32_t q = quantiseTideMm(x, 25);
        CHECK(q >= prev);            // monotone
        CHECK(q <= x && x < q + 25); // bin identity
        prev = q;
    }
}
