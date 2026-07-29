// Tests for voxelcore/density3.h (Phase 4 bounded 3D density band). See that
// header for the design rationale; this file exists to catch a mistake in it,
// not to re-explain it.
//
// The tests that matter most here are the two that a plausible-looking but
// useless implementation would fail:
//
//   * `overhangs_actually_occur` -- a displacement that is bounded, continuous
//     and correctly gated but never produces solid-above-air would pass every
//     other test in this file and buy nothing.
//   * `bedding_component_is_in_phase_with_2d_banding` -- a 3D term hashed
//     independently of the 2D banding would produce undercuts at heights
//     unrelated to the bands visible on the face above them, which is worse
//     than having no undercuts.

#include "voxelcore/density3.h"
#include "vxctest.h"

#include <cstdio>

using namespace vxc;

namespace {

// Same tiny deterministic PRNG test_detail_bedding.cpp uses, for the same
// reason: "well distributed and reproducible" without pulling in <random>.
struct SplitMixRng {
    uint64_t state;
    explicit SplitMixRng(uint64_t seed) : state(seed) {}
    uint64_t next() {
        state += 0x9E3779B97F4A7C15ull;
        return splitmix64(state);
    }
    int64_t nextSigned(int64_t magnitude) {
        const uint64_t r = next();
        const int64_t bits = static_cast<int64_t>(r % static_cast<uint64_t>(2 * magnitude + 1));
        return bits - magnitude;
    }
};

int64_t iabs(int64_t v) { return v < 0 ? -v : v; }

constexpr uint64_t kSeed = 20260719;
constexpr int64_t kQ = kDensity3GateQ;

// A gradient that opens the slope gate fully, expressed on one axis so the
// octagonal norm is exact and the test's slope is unambiguous.
constexpr int64_t kSteepMmPerM = 1200;

// Both gates fully open, for the tests that are about the displacement itself
// rather than about the gates.
int64_t openD(int64_t xMm, int64_t yMm, int64_t zMm, int64_t surfaceMm) {
    return density3DisplacementGatedMm(kSeed, xMm, yMm, zMm, surfaceMm, kQ, kQ);
}

// The bedding half of the composed displacement, ungated, as the header
// composes it. Used by the alignment tests.
int64_t beddingComponent(int64_t xMm, int64_t yMm, int64_t zMm) {
    return density3BeddingMm(kSeed, xMm, yMm, zMm);
}

// An anchor well inside one 819.2 m structural domain, so that sweeps of a few
// metres never cross a domain boundary. detail_bedding.h's strike/dip field is
// deliberately a STEP function across those boundaries; every continuity claim
// in this file is a within-domain claim, exactly as it is for the 2D term.
constexpr int64_t kAnchorX = 400'000;
constexpr int64_t kAnchorY = 400'000;

} // namespace

// ---------------------------------------------------------------------------
// 1. THE GATES MAKE D EXACTLY ZERO -- not small, exactly zero.
// ---------------------------------------------------------------------------

VXC_TEST(zero_outside_the_band_exactly) {
    SplitMixRng rng(1);
    int64_t checked = 0;
    for (int i = 0; i < 60000; ++i) {
        const int64_t x = rng.nextSigned(2'000'000'000);
        const int64_t y = rng.nextSigned(2'000'000'000);
        const int64_t s = rng.nextSigned(100'000);
        // dz strictly outside the band, on both sides, near and far.
        const int64_t mag = kDensity3BandHalfMm + (rng.next() % 5'000'000);
        const int64_t dz = (rng.next() & 1) ? mag : -mag;
        CHECK_EQ(openD(x, y, s + dz, s), 0);
        ++checked;
    }
    CHECK(checked == 60000);
}

VXC_TEST(zero_at_the_band_edge_exactly) {
    // The boundary itself, both signs, on many columns -- the off-by-one that
    // a `<=` / `<` slip would produce.
    SplitMixRng rng(2);
    for (int i = 0; i < 20000; ++i) {
        const int64_t x = rng.nextSigned(2'000'000'000);
        const int64_t y = rng.nextSigned(2'000'000'000);
        const int64_t s = rng.nextSigned(100'000);
        CHECK_EQ(openD(x, y, s + kDensity3BandHalfMm, s), 0);
        CHECK_EQ(openD(x, y, s - kDensity3BandHalfMm, s), 0);
        CHECK_EQ(density3BandTaperQ(s + kDensity3BandHalfMm, s), 0);
        CHECK_EQ(density3BandTaperQ(s - kDensity3BandHalfMm, s), 0);
    }
}

VXC_TEST(zero_below_the_slope_gate_exactly) {
    SplitMixRng rng(3);
    for (int i = 0; i < 60000; ++i) {
        const int64_t x = rng.nextSigned(2'000'000'000);
        const int64_t y = rng.nextSigned(2'000'000'000);
        const int64_t s = rng.nextSigned(100'000);
        const int64_t dz = rng.nextSigned(kDensity3BandHalfMm - 1); // inside the band
        // Any gradient at or below the gate's start, in any direction.
        const int64_t g = rng.next() % (kDensity3SlopeStartMmPerM + 1);
        const int64_t gx = (rng.next() & 1) ? g : -g;
        CHECK_EQ(density3DisplacementMm(kSeed, x, y, s + dz, s, gx, 0, 0), 0);
        CHECK_EQ(density3DisplacementMm(kSeed, x, y, s + dz, s, 0, gx, 0), 0);
    }
    // And the gate predicate agrees with the displacement, which is what makes
    // a caller's column-level skip exact rather than approximate.
    CHECK(!density3SlopeGateOpenFromMag(kDensity3SlopeStartMmPerM));
    CHECK(density3SlopeGateOpenFromMag(kDensity3SlopeStartMmPerM + 1));
    CHECK_EQ(density3SlopeGateQFromMag(kDensity3SlopeStartMmPerM), 0);
}

VXC_TEST(cheap_gate_predicates_agree_with_the_displacement) {
    // The whole cost argument is that a caller may skip on the cheap
    // predicates. That is only sound if "predicate closed" implies "D == 0"
    // for EVERY z / every column, which is what this checks directly.
    SplitMixRng rng(4);
    for (int i = 0; i < 40000; ++i) {
        const int64_t x = rng.nextSigned(2'000'000'000);
        const int64_t y = rng.nextSigned(2'000'000'000);
        const int64_t s = rng.nextSigned(100'000);
        const int64_t z = s + rng.nextSigned(3000);
        const int64_t gx = rng.nextSigned(2000), gy = rng.nextSigned(2000);
        const int64_t soil = rng.next() % 2000;

        const int64_t d = density3DisplacementMm(kSeed, x, y, z, s, gx, gy, soil);
        if (!density3SlopeGateOpen(gx, gy)) CHECK_EQ(d, 0);
        if (!density3BandGateOpen(z, s)) CHECK_EQ(d, 0);
        if (d != 0) {
            CHECK(density3SlopeGateOpen(gx, gy));
            CHECK(density3BandGateOpen(z, s));
        }
    }
}

VXC_TEST(hoisted_gate_path_matches_the_reference_path) {
    // density3DisplacementGatedMm is what the hot loop calls with the column
    // gates hoisted; it must be bit-identical to the reference overload.
    SplitMixRng rng(5);
    for (int i = 0; i < 40000; ++i) {
        const int64_t x = rng.nextSigned(2'000'000'000);
        const int64_t y = rng.nextSigned(2'000'000'000);
        const int64_t s = rng.nextSigned(100'000);
        const int64_t z = s + rng.nextSigned(1000);
        const int64_t gx = rng.nextSigned(2000), gy = rng.nextSigned(2000);
        const int64_t soil = rng.next() % 2000;
        CHECK_EQ(density3DisplacementMm(kSeed, x, y, z, s, gx, gy, soil),
                 density3DisplacementGatedMm(kSeed, x, y, z, s, density3SlopeGateQ(gx, gy),
                                             density3RockGateQ(soil)));
    }
}

// ---------------------------------------------------------------------------
// 2. THE COMBINED BOUND.
// ---------------------------------------------------------------------------

VXC_TEST(combined_envelope_holds_over_wide_coordinates) {
    // Several hundred thousand points over the full coordinate range this
    // world can reach, INCLUDING large negatives. Large negatives are called
    // out because they are where a bare `/` diverges from floorDiv, which is
    // the failure this codebase has actually shipped once (docs/determinism.md).
    SplitMixRng rng(6);
    int64_t worst = 0, worstBed = 0, worstPocket = 0;
    int64_t negCoords = 0, n = 0;
    for (int i = 0; i < 400000; ++i) {
        const int64_t x = rng.nextSigned(2'000'000'000);
        const int64_t y = rng.nextSigned(2'000'000'000);
        const int64_t s = rng.nextSigned(2'000'000);
        const int64_t z = s + rng.nextSigned(1200);
        const int64_t gx = rng.nextSigned(3000), gy = rng.nextSigned(3000);
        const int64_t soil = rng.next() % 3000;
        if (x < 0 && y < 0) ++negCoords;
        ++n;

        const int64_t d = density3DisplacementMm(kSeed, x, y, z, s, gx, gy, soil);
        CHECK(iabs(d) <= kDensity3MaxAbsMm);
        if (iabs(d) > worst) worst = iabs(d);

        // The two contributors, checked against their own allocations too --
        // so a failure says WHICH term overspent, not just that the sum did.
        const int64_t b = beddingComponent(x, y, z);
        CHECK(iabs(b) <= kBedding3MaxAbsMm);
        if (iabs(b) > worstBed) worstBed = iabs(b);
        const int64_t p = density3PocketMm(kSeed, x, y, z, kQ);
        CHECK(iabs(p) <= kDensity3PocketAmpMm);
        if (iabs(p) > worstPocket) worstPocket = iabs(p);
    }
    // A fuzz that never reached a large negative coordinate would be silently
    // testing nothing about the thing it was written for.
    CHECK(negCoords > n / 8);
    std::printf("    [density3] fuzz n=%lld  max|D|=%lld/%lld  max|bed|=%lld/%lld  "
                "max|pocket|=%lld/%lld  (x<0,y<0 on %lld)\n",
                (long long)n, (long long)worst, (long long)kDensity3MaxAbsMm, (long long)worstBed,
                (long long)kBedding3MaxAbsMm, (long long)worstPocket,
                (long long)kDensity3PocketAmpMm, (long long)negCoords);
    // Not vacuous: the fuzz must actually approach the envelope, or a term
    // that returned 0 everywhere would "pass".
    CHECK(worst > kDensity3MaxAbsMm / 2);
}

VXC_TEST(budget_is_exactly_allocated) {
    // The static_assert in the header proves this at compile time; restated
    // here so the arithmetic is visible in the test log too.
    CHECK_EQ(kBedding3MaxAbsMm + kDensity3PocketAmpMm, kDensity3MaxAbsMm);
    CHECK_EQ(kDensity3PocketAmpMm, 200);
    CHECK_EQ(kDensity3BandHalfMm, kDensity3MaxAbsMm);
}

// ---------------------------------------------------------------------------
// 3. BEDDING ALIGNMENT -- the 3D banding is the 2D banding.
// ---------------------------------------------------------------------------

VXC_TEST(bedding_component_is_in_phase_with_2d_banding) {
    // (a) SAME FIELD. beddingMm (2D, amplitude 320) and beddingDisplacement3Mm
    // (3D, amplitude 500) are floorDiv scalings of the identical
    // beddingRawAt(seed, x, y, z). If they are, then
    //     b3 * 320  ==  b2 * 500   up to the two truncations,
    // and the residual cannot exceed 320 + 500. An independently hashed 3D
    // field would blow this by orders of magnitude.
    SplitMixRng rng(7);
    int64_t worstResidual = 0;
    for (int i = 0; i < 50000; ++i) {
        const int64_t x = rng.nextSigned(2'000'000'000);
        const int64_t y = rng.nextSigned(2'000'000'000);
        const int64_t s = rng.nextSigned(1'000'000);
        const int64_t b2 = beddingMm(kSeed, x, y, s);
        const int64_t b3 = beddingDisplacement3Mm(kSeed, x, y, s);
        const int64_t residual = iabs(b3 * kBeddingAmpMm - b2 * kBedding3MaxAbsMm);
        if (residual > worstResidual) worstResidual = residual;
        CHECK(residual <= kBeddingAmpMm + kBedding3MaxAbsMm);
    }
    std::printf("    [density3] 2D/3D cross-identity worst residual %lld (bar %lld)\n",
                (long long)worstResidual, (long long)(kBeddingAmpMm + kBedding3MaxAbsMm));

    // (b) THE SHARPENING DOES NOT MOVE A BAND. It is a monotone odd pointwise
    // remap, so it must fix zero exactly and preserve sign everywhere. If it
    // did not, the sharpened 3D term would drift out of phase with the 2D one
    // even though it was built from it.
    for (int64_t r = -kBedding3MaxAbsMm; r <= kBedding3MaxAbsMm; ++r) {
        const int64_t sr = density3SharpenMm(r, kBedding3MaxAbsMm);
        CHECK(iabs(sr) <= kBedding3MaxAbsMm);
        if (r > 0) CHECK(sr > 0);
        if (r < 0) CHECK(sr < 0);
        if (r == 0) CHECK_EQ(sr, 0);
        // Monotone, and never shrinks magnitude (it is a contrast curve).
        if (r > -kBedding3MaxAbsMm)
            CHECK(sr >= density3SharpenMm(r - 1, kBedding3MaxAbsMm));
        CHECK(iabs(sr) >= iabs(r));
    }

    // (c) NEVER OPPOSITE. Walk the surface elevation up through the rock and
    // compare what the 2D term paints on the face with what the composed 3D
    // displacement does at that same height. They must never have opposite
    // sign: a protruding band on the face cannot be a recess in the volume.
    int64_t opposed = 0, samples = 0, sign2Changes = 0, sign3Changes = 0;
    int64_t prev2 = 0, prev3 = 0;
    for (int64_t s = -3000; s <= 3000; ++s) {
        const int64_t b2 = beddingMm(kSeed, kAnchorX, kAnchorY, s);
        const int64_t b3 = beddingComponent(kAnchorX, kAnchorY, s);
        ++samples;
        if ((b2 > 0 && b3 < 0) || (b2 < 0 && b3 > 0)) ++opposed;
        if (s > -3000) {
            if ((prev2 > 0) != (b2 > 0)) ++sign2Changes;
            if ((prev3 > 0) != (b3 > 0)) ++sign3Changes;
        }
        prev2 = b2;
        prev3 = b3;
    }
    CHECK_EQ(opposed, 0);
    // Not vacuous: the 6 m sweep must actually cross several bands, and the
    // two terms must cross the SAME number of them.
    CHECK(sign2Changes >= 3);
    CHECK_EQ(sign2Changes, sign3Changes);
    std::printf("    [density3] 6 m vertical sweep: %lld band sign changes (2D) vs %lld (3D), "
                "%lld/%lld samples opposed\n",
                (long long)sign2Changes, (long long)sign3Changes, (long long)opposed,
                (long long)samples);

    // (d) THE COMPOSED, GATED D AT THE SURFACE still follows the 2D banding
    // wherever bedding dominates the pocket term. |b2| >= 140 implies
    // |b3| >= 200 = the pocket's entire allocation, so the sum's sign is the
    // bedding's sign and this is an exact claim, not a statistical one.
    SplitMixRng rng2(8);
    int64_t dominant = 0, agree = 0;
    for (int i = 0; i < 200000; ++i) {
        const int64_t x = rng2.nextSigned(2'000'000'000);
        const int64_t y = rng2.nextSigned(2'000'000'000);
        const int64_t s = rng2.nextSigned(1'000'000);
        const int64_t b2 = beddingMm(kSeed, x, y, s);
        if (iabs(b2) < 140) continue;
        ++dominant;
        const int64_t d = openD(x, y, s, s); // at the surface, both gates open
        if ((b2 > 0 && d > 0) || (b2 < 0 && d < 0)) ++agree;
    }
    CHECK(dominant > 1000); // the subset must be populated or the test is empty
    CHECK_EQ(agree, dominant);
    std::printf("    [density3] surface sign agreement with 2D banding: %lld/%lld\n",
                (long long)agree, (long long)dominant);
}

// ---------------------------------------------------------------------------
// 4. CONTINUITY.
// ---------------------------------------------------------------------------
//
// A step in D is a step in the SURFACE, and because each gate's argument is a
// smooth field, a step in D at a gate threshold is a seam following a curve on
// the ground -- an iso-slope contour, an iso-soil-depth contour, or the band's
// own offset surface. That is the failure class this project exists to remove,
// so each is swept at 1-unit resolution across its threshold.
//
// These sweeps hold (x, y) fixed inside one structural domain and vary only
// the gate's own argument, so any step they see is the GATE's, not the bedding
// field's.

VXC_TEST(continuous_across_the_slope_gate) {
    int64_t worst = 0;
    for (int64_t dz = -600; dz <= 600; dz += 137) {
        int64_t prev = density3DisplacementMm(kSeed, kAnchorX, kAnchorY, dz, 0, 0, 0, 0);
        for (int64_t g = 1; g <= 1400; ++g) {
            const int64_t d = density3DisplacementMm(kSeed, kAnchorX, kAnchorY, dz, 0, g, 0, 0);
            const int64_t step = iabs(d - prev);
            if (step > worst) worst = step;
            prev = d;
        }
    }
    // The gate opens over 200 mm/m with a quintic whose steepest slope is
    // 1.875, so the largest possible step is 700 * 1.875 / 200 = 6.6 mm per
    // 1 mm/m of grade. A discontinuity would be hundreds of millimetres.
    std::printf("    [density3] max |dD| per 1 mm/m of grade: %lld mm (bar 16)\n",
                (long long)worst);
    CHECK(worst <= 16);
    CHECK(worst > 0); // the sweep must actually cross the ramp
}

VXC_TEST(continuous_across_the_band_edges) {
    int64_t worst = 0, worstNearEdge = 0;
    for (int64_t k = 0; k < 24; ++k) {
        const int64_t x = kAnchorX + k * 4001;
        int64_t prev = 0;
        for (int64_t dz = -900; dz <= 900; ++dz) {
            const int64_t d = openD(x, kAnchorY, dz, 0);
            if (dz > -900) {
                const int64_t step = iabs(d - prev);
                if (step > worst) worst = step;
            }
            // Approaching the edge, D must be small -- if it were still large
            // one millimetre inside the band, forcing it to zero at the edge
            // would draw the offset surface into the geometry as a shelf.
            if (iabs(dz) >= kDensity3BandHalfMm - 2 && iabs(d) > worstNearEdge)
                worstNearEdge = iabs(d);
            prev = d;
        }
        CHECK_EQ(openD(x, kAnchorY, -kDensity3BandHalfMm, 0), 0);
        CHECK_EQ(openD(x, kAnchorY, kDensity3BandHalfMm, 0), 0);
    }
    // Bedding contributes up to 1.80 mm/mm before sharpening and 1.875^2 after;
    // the taper contributes up to 700 * 1.875 / 300 = 4.4 mm/mm. A hard edge
    // would show up as a step of order 100 mm.
    std::printf("    [density3] max |dD| per 1 mm of z: %lld mm (bar 32); "
                "max |D| within 2 mm of the band edge: %lld mm (bar 8)\n",
                (long long)worst, (long long)worstNearEdge);
    CHECK(worst <= 32);
    CHECK(worstNearEdge <= 8);
}

VXC_TEST(continuous_across_the_lithology_gate) {
    int64_t worst = 0;
    for (int64_t dz = -600; dz <= 600; dz += 173) {
        int64_t prev = density3DisplacementMm(kSeed, kAnchorX, kAnchorY, dz, 0, kSteepMmPerM, 0, 0);
        for (int64_t soil = 1; soil <= 1200; ++soil) {
            const int64_t d =
                density3DisplacementMm(kSeed, kAnchorX, kAnchorY, dz, 0, kSteepMmPerM, 0, soil);
            const int64_t step = iabs(d - prev);
            if (step > worst) worst = step;
            prev = d;
        }
    }
    // Only the 200 mm pocket allocation moves with this gate, over a 600 mm
    // ramp: at most 200 * 1.875 / 600 = 0.6 mm per millimetre of soil.
    std::printf("    [density3] max |dD| per 1 mm of soil depth: %lld mm (bar 4)\n",
                (long long)worst);
    CHECK(worst <= 4);
}

VXC_TEST(continuous_laterally_within_a_structural_domain) {
    // A 1 mm step in x, at a range of heights in the band. The bedding field's
    // dip reaches 3.0, so lateral change runs up to three times the vertical
    // rate; that is detail_bedding.h's behaviour, inherited deliberately.
    int64_t worst = 0;
    for (int64_t dz = -650; dz <= 650; dz += 50) {
        int64_t prev = openD(kAnchorX, kAnchorY, dz, 0);
        for (int64_t dx = 1; dx <= 4000; ++dx) {
            const int64_t d = openD(kAnchorX + dx, kAnchorY, dz, 0);
            const int64_t step = iabs(d - prev);
            if (step > worst) worst = step;
            prev = d;
        }
    }
    std::printf("    [density3] max |dD| per 1 mm of x: %lld mm (bar 48)\n", (long long)worst);
    CHECK(worst <= 48);
}

// ---------------------------------------------------------------------------
// 5. DETERMINISM AND PURITY.
// ---------------------------------------------------------------------------

VXC_TEST(pure_function_of_world_mm_and_seed) {
    // Structurally there is no tile origin, chunk index or brick-local
    // coordinate anywhere in this file's signature; this checks the
    // consequences that a mistake would break.
    SplitMixRng rng(9);
    for (int i = 0; i < 20000; ++i) {
        const int64_t x = rng.nextSigned(2'000'000'000);
        const int64_t y = rng.nextSigned(2'000'000'000);
        const int64_t s = rng.nextSigned(1'000'000);
        const int64_t z = s + rng.nextSigned(600);
        const int64_t gx = rng.nextSigned(2000), gy = rng.nextSigned(2000);
        const int64_t soil = rng.next() % 1500;

        const int64_t a = density3DisplacementMm(kSeed, x, y, z, s, gx, gy, soil);
        // Repeated evaluation is identical (no hidden state, no memo).
        CHECK_EQ(density3DisplacementMm(kSeed, x, y, z, s, gx, gy, soil), a);
        // The seed is actually consumed: a different world is a different
        // answer. (Not every point must differ, so this is checked in
        // aggregate below rather than per point.)
    }

    int64_t differs = 0;
    for (int i = 0; i < 5000; ++i) {
        const int64_t x = rng.nextSigned(1'000'000);
        const int64_t y = rng.nextSigned(1'000'000);
        if (openD(x, y, 0, 0) !=
            density3DisplacementGatedMm(kSeed + 1, x, y, 0, 0, kQ, kQ))
            ++differs;
    }
    CHECK(differs > 4000);
}

VXC_TEST(constexpr_evaluation_matches_runtime) {
    // Compile-time and run-time evaluation of the same expression must agree.
    // They can only diverge through UB (signed overflow, a division the
    // compiler folds differently), which is exactly the class of defect the
    // floorDiv discipline exists to prevent -- and a constant-folded
    // disagreement would be invisible to every other test here.
    constexpr int64_t a = density3DisplacementGatedMm(kSeed, -1'234'567'890, 987'654'321, -455,
                                                      0, kQ, kQ);
    constexpr int64_t b = density3DisplacementGatedMm(kSeed, 555'444'333, -222'111'000, 301,
                                                      0, kQ, kQ);
    constexpr int64_t c = density3PocketMm(kSeed, -9'876'543, -8'765'432, -7'654, kQ);
    volatile int64_t vx1 = -1'234'567'890, vy1 = 987'654'321, vz1 = -455;
    volatile int64_t vx2 = 555'444'333, vy2 = -222'111'000, vz2 = 301;
    volatile int64_t vx3 = -9'876'543, vy3 = -8'765'432, vz3 = -7'654;
    CHECK_EQ(density3DisplacementGatedMm(kSeed, vx1, vy1, vz1, 0, kQ, kQ), a);
    CHECK_EQ(density3DisplacementGatedMm(kSeed, vx2, vy2, vz2, 0, kQ, kQ), b);
    CHECK_EQ(density3PocketMm(kSeed, vx3, vy3, vz3, kQ), c);
}

VXC_TEST(value_noise_3_stays_in_range_and_is_smooth) {
    SplitMixRng rng(10);
    int64_t lo = 0, hi = 0, worstStep = 0;
    for (int i = 0; i < 100000; ++i) {
        const int64_t x = rng.nextSigned(2'000'000'000);
        const int64_t y = rng.nextSigned(2'000'000'000);
        const int64_t z = rng.nextSigned(2'000'000'000);
        const int64_t n = density3ValueNoise3Fade(kSeed, x, y, z, kDensity3PocketLatXYMm,
                                                  kDensity3PocketLatZMm, CH_POCKET);
        CHECK(n >= -kDensity3NoiseAbs && n < kDensity3NoiseAbs);
        if (n < lo) lo = n;
        if (n > hi) hi = n;
    }
    // Smooth across a lattice plane in each axis -- the quintic fade's whole
    // job. A bilinear/trilinear form would show a gradient kink here, not a
    // value step, so this checks the value's own step stays small while
    // crossing x0, y0 and z0 lattice boundaries head-on.
    for (int64_t axis = 0; axis < 3; ++axis) {
        for (int64_t t = -3000; t <= 3000; ++t) {
            const int64_t x = axis == 0 ? kDensity3PocketLatXYMm * 7 + t : 12345;
            const int64_t y = axis == 1 ? kDensity3PocketLatXYMm * 7 + t : 54321;
            const int64_t z = axis == 2 ? kDensity3PocketLatZMm * 7 + t : 6789;
            const int64_t a = density3ValueNoise3Fade(kSeed, x, y, z, kDensity3PocketLatXYMm,
                                                      kDensity3PocketLatZMm, CH_POCKET);
            const int64_t b = density3ValueNoise3Fade(
                kSeed, x + (axis == 0), y + (axis == 1), z + (axis == 2),
                kDensity3PocketLatXYMm, kDensity3PocketLatZMm, CH_POCKET);
            const int64_t step = iabs(b - a);
            if (step > worstStep) worstStep = step;
        }
    }
    // CEILING, DERIVED PROPERLY -- the obvious derivation is wrong and the
    // difference is instructive.
    //
    // The naive figure is "corner gap 65535, fade slope at most 1.875, over a
    // 1600 mm lattice" = 77 units per mm. Measured is ~156, twice that, and
    // the excess is in the CELL INTERIOR, not at the lattice planes (checked
    // separately: 78 near a plane, 156 inside). The cause is hash.h's
    // fadeFractionMm carrying its argument as a q10 fraction: over a 1600 mm
    // lattice tq advances by 1024/1600 = 0.64 per millimetre, so it steps by 0
    // or 1, and when it steps the faded fraction jumps 1.875 * 1600/1024 =
    // 2.93 mm rather than moving 1.875. The real ceiling is therefore
    //     65535 * 1.875 / 1024            = 120   (the q10 staircase)
    //   + 65535 / 1600                    =  41   (the final floorDiv)
    //                                     = 161
    // and the bar is set just above it.
    //
    // This is inherited, not introduced: valueNoise2Fade has exactly the same
    // property at amplifier.cpp's 1600 mm microrelief octave. It is worth
    // 156/65536 of this term's 200 mm allocation -- under 1 mm, i.e. under one
    // hundredth of a 100 mm voxel -- so it is recorded rather than fixed here.
    std::printf("    [density3] noise3 range [%lld, %lld], max step per mm %lld (bar 176)\n",
                (long long)lo, (long long)hi, (long long)worstStep);
    CHECK(worstStep <= 176);
    CHECK(lo < -30000 && hi > 30000); // the field must actually use its range
}

// ---------------------------------------------------------------------------
// 6. OVERHANGS ACTUALLY OCCUR.
// ---------------------------------------------------------------------------
//
// This is the test the whole file is for. Bounded, continuous, gated and
// geometrically inert is a real and very reachable outcome -- the unsharpened
// composition produced an overhang on 0.098% of columns, which is
// indistinguishable from none -- and it would pass every other test above.

namespace {

// Walks a column through the band AT VOXEL RESOLUTION and reports whether the
// displaced solidity test yields solid ABOVE air, which is what an overhang
// is. Returns the vertical extent of the overhung span in millimetres, or 0.
//
// Voxel centres, not millimetres, and the distinction is the whole point. An
// earlier version of this test walked z in 1 mm steps and reported 7.6% of
// columns overhanging -- but half of those spans were one to three millimetres
// tall, which at 100 mm voxels is not an overhang, it is nothing. The world
// only ever evaluates stratigraphy at voxel centres (amplifier.cpp:1385,
// `vz * kVoxelSizeMm + kVoxelSizeMm / 2`), so that is what this samples, and
// the rate it reports is the rate that can actually reach the mesher.
int64_t overhangExtentMm(int64_t xMm, int64_t yMm, int64_t surfaceMm) {
    const int64_t vz0 = floorDiv(surfaceMm - kDensity3BandHalfMm, kVoxelSizeMm);
    const int64_t vz1 = floorDiv(surfaceMm + kDensity3BandHalfMm, kVoxelSizeMm);
    bool sawAir = false, found = false;
    int64_t firstAir = 0, lastSolid = 0;
    for (int64_t vz = vz0; vz <= vz1; ++vz) {
        const int64_t z = vz * kVoxelSizeMm + kVoxelSizeMm / 2;
        const int64_t d = openD(xMm, yMm, z, surfaceMm);
        const bool solid = density3IsSolid(z, surfaceMm, d);
        if (!solid && !sawAir) {
            sawAir = true;
            firstAir = z;
        }
        if (solid && sawAir) {
            found = true;
            lastSolid = z;
        }
    }
    return found ? lastSolid - firstAir : 0;
}

} // namespace

VXC_TEST(overhangs_actually_occur) {
    // (a) Survey. The surface elevation varies per column, because the phase
    // between the voxel grid and the surface is what decides whether a given
    // overhung span lands on a voxel centre at all; holding the surface at a
    // fixed height would sample one phase and report it as the rate.
    SplitMixRng rng(11);
    int64_t witnessX = 0, witnessY = 0, witnessS = 0, witnessExtent = 0;
    int64_t found = 0, columns = 0, totalExtent = 0;
    for (int i = 0; i < 20000; ++i) {
        const int64_t x = rng.nextSigned(4'000'000);
        const int64_t y = rng.nextSigned(4'000'000);
        const int64_t s = rng.nextSigned(200'000);
        ++columns;
        const int64_t e = overhangExtentMm(x, y, s);
        if (e > 0) {
            ++found;
            totalExtent += e;
            // Keep the LARGEST as the pinned witness, not the first -- a 100 mm
            // one-voxel lip is a true overhang but a poor regression anchor.
            if (e > witnessExtent) {
                witnessX = x;
                witnessY = y;
                witnessS = s;
                witnessExtent = e;
            }
        }
    }

    CHECK(found > 0);
    std::printf("    [density3] overhangs at voxel resolution: %lld / %lld columns (%.3f%%), "
                "mean overhung span %lld mm, max %lld mm\n",
                (long long)found, (long long)columns,
                100.0 * static_cast<double>(found) / static_cast<double>(columns),
                (long long)(found ? totalExtent / found : 0), (long long)witnessExtent);

    // (b) The witness column, spelled out: a voxel that is AIR, and a strictly
    // higher voxel that is SOLID. Asserted on the solidity predicate directly,
    // not inferred from the sign of D, and counted so the span is not a single
    // ambiguous voxel.
    CHECK(witnessExtent >= 2 * kVoxelSizeMm);
    {
        bool sawAir = false, solidAboveAir = false;
        int64_t airVox = 0, solidAboveVox = 0;
        const int64_t vz0 = floorDiv(witnessS - kDensity3BandHalfMm, kVoxelSizeMm);
        const int64_t vz1 = floorDiv(witnessS + kDensity3BandHalfMm, kVoxelSizeMm);
        for (int64_t vz = vz0; vz <= vz1; ++vz) {
            const int64_t z = vz * kVoxelSizeMm + kVoxelSizeMm / 2;
            const bool solid =
                density3IsSolid(z, witnessS, openD(witnessX, witnessY, z, witnessS));
            if (!solid) {
                sawAir = true;
                ++airVox;
            }
            if (solid && sawAir) {
                solidAboveAir = true;
                ++solidAboveVox;
            }
        }
        CHECK(solidAboveAir);
        CHECK(airVox > 0 && solidAboveVox > 0);
        // The undisplaced heightfield cannot do this: without D every voxel at
        // or below the surface is solid and every voxel above it is air, so
        // solid-above-air is unreachable. Stated as a test so the witness is
        // demonstrably a consequence of D and not of the sampling.
        bool baselineOverhang = false, baselineAir = false;
        for (int64_t vz = vz0; vz <= vz1; ++vz) {
            const int64_t z = vz * kVoxelSizeMm + kVoxelSizeMm / 2;
            const bool solid = density3IsSolid(z, witnessS, 0);
            if (!solid) baselineAir = true;
            if (solid && baselineAir) baselineOverhang = true;
        }
        CHECK(!baselineOverhang);
        std::printf("    [density3] witness column x=%lld y=%lld surface=%lld: overhung span "
                    "%lld mm, %lld air voxels with %lld solid voxels above them\n",
                    (long long)witnessX, (long long)witnessY, (long long)witnessS,
                    (long long)witnessExtent, (long long)airVox, (long long)solidAboveVox);
    }

    // (c) A RATE FLOOR, so a future change that quietly flattens the profile
    // fails here instead of silently shipping an inert term. The unsharpened
    // composition scores 0.098%; the bar is set well above that and below the
    // measured rate, so it distinguishes "works" from "does nothing" without
    // pinning a number that has no calibration behind it.
    CHECK(found * 1000 >= columns * 10);
}

VXC_TEST(reachable_displacement_matches_the_taper_arithmetic) {
    // The band taper caps how far the displaced surface can actually move: a
    // nose can protrude only as far as the largest dz with dz <= taper(dz)/Q *
    // 700. The header quotes 511 mm; if the core/ramp split changes, this
    // fails and the header's number has to be re-derived rather than left
    // stale.
    int64_t maxNose = -1, maxRecess = 1;
    for (int64_t dz = 0; dz <= kDensity3BandHalfMm; ++dz) {
        if (density3ScaleMm(kDensity3MaxAbsMm, kQ, density3BandTaperQ(dz, 0)) >= dz) maxNose = dz;
    }
    for (int64_t dz = 0; dz >= -kDensity3BandHalfMm; --dz) {
        if (density3ScaleMm(-kDensity3MaxAbsMm, kQ, density3BandTaperQ(dz, 0)) <= dz)
            maxRecess = dz;
    }
    std::printf("    [density3] reachable displacement: nose +%lld mm, recess %lld mm\n",
                (long long)maxNose, (long long)maxRecess);
    CHECK_EQ(maxNose, 511);
    CHECK_EQ(maxRecess, -511);
}

VXC_TEST(skipping_outside_the_band_is_exact_not_approximate) {
    // The cost argument rests on this: outside +/-700 mm the answer with D
    // computed and the answer with D assumed zero are the SAME answer, because
    // |D| <= 700 means D cannot flip `z <= surface + D` there. Checked as the
    // claim itself -- solidity with and without the term -- rather than as
    // "D happens to be zero".
    SplitMixRng rng(12);
    for (int i = 0; i < 50000; ++i) {
        const int64_t x = rng.nextSigned(2'000'000'000);
        const int64_t y = rng.nextSigned(2'000'000'000);
        const int64_t s = rng.nextSigned(1'000'000);
        const int64_t mag = kDensity3BandHalfMm + (rng.next() % 100000);
        const int64_t z = s + ((rng.next() & 1) ? mag : -mag);
        const int64_t d = openD(x, y, z, s);
        CHECK_EQ(density3IsSolid(z, s, d), density3IsSolid(z, s, 0));
    }
}
