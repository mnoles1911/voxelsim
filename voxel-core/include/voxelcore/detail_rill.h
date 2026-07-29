#pragma once
// Rill / flute term -- terrain-amplification plan Phase 3c, "Structured,
// anisotropic detail".
//
// WHAT THIS IS FOR. Sub-30 m detail today is isotropic fBm: spectrally
// plausible, structurally meaningless. Water does not cut isotropic hollows,
// it cuts DOWNSLOPE-ELONGATED ones. This term supplies that anisotropy: a
// value-noise field whose features are ~1.6 m across-slope and ~12.8 m
// along-slope (8:1), oriented by the carrier's local gradient, gated off on
// ground too flat for a rill to exist.
//
// This header is a pure point function of world millimetres. It is NOT wired
// into evalSurface here -- integration is a separate change.
//
// MIRRORABILITY. Plain free functions over int64_t, no templates, no std::,
// no sqrt, no trig, every signed division routed through floorDiv. Intended to
// be transliterated into worldgen.ush line for line.
//
// ---------------------------------------------------------------------------
// DEVIATION FROM THE PLAN TEXT, AND WHY (read this before "simplifying" it).
//
// The plan says: rotate the sample point into the local gradient frame, scale
// the two axes 8:1, sample noise on the warped domain. Taken literally -- one
// rotation matrix built from the gradient at the sample point, applied to the
// WORLD position -- that construction is broken in an unbounded world, and
// not subtly.
//
// The noise phase is then R(u(p)) S p, whose spatial derivative is
//
//     d/dp [ R(u(p)) S p ] = R S  +  (dR/du)(du/dp) S p
//                                    ^^^^^^^^^^^^^^^^^^^
//                                    grows with |p|
//
// The second term scales with the DISTANCE FROM THE WORLD ORIGIN. On a real
// hillside the aspect swings ~1 rad over a few hundred metres, so
// |du/dp| ~ 5e-3 /m. At 10 km from the origin that term is ~50: stepping one
// metre across the ground steps ~50 m through the noise. The 1.6 m "rills"
// degenerate into per-voxel static, and they do so as a function of how far
// the player is from x=0, which is exactly the sort of position-dependent
// character change this project exists to remove. The function stays
// mathematically continuous the whole time -- it is a CONDITIONING failure,
// not a discontinuity, which is why it would have survived a continuity test
// and shipped. test_detail_rill.cpp measures it directly
// (rill_lever_arm_regression) against a literal implementation of the plan
// text; at 54 km out the literal form's 100 mm-lag roughness is ~5x ours,
// i.e. it has become white noise.
//
// The fix has to be local, and there is no way around that: any construction
// whose noise phase is a p-dependent transform of the GLOBAL coordinate has
// this lever arm, and a bounded correction cannot remove it (a continuous g
// with dg/dp ~= I everywhere is necessarily unbounded).
//
// So the rotation is quantised to a compile-time table of 16 directions
// (11.25 degrees apart, a line-direction covering 180 degrees) and the two
// fields either side of the measured aspect are blended. Each of the 16
// fields is a FIXED linear map of world position -- a compile-time constant
// matrix -- so each has zero lever arm by construction. Only the blend
// WEIGHTS depend on the local gradient, and they enter multiplicatively and
// boundedly. The elongation is still "rotate into the gradient frame, scale
// the axes 8:1, sample value noise on the warped domain"; it is done twice,
// on the two nearest representable frames, and mixed.
//
// Costs of that choice, both measured and bounded:
//   * two valueNoise2Fade calls (8 hash2) instead of one. For scale, the
//     existing detail cascade is five valueNoise2Fade calls.
//   * the elongation axis is misoriented by at most ~4.1 degrees (see the
//     diamond-angle note below). At 8:1 that costs ~30% of the along-slope
//     correlation length; the term still measures >4:1 anisotropic.
//   * CONTRAST MODULATION WITH ASPECT -- the one real cost, stated plainly.
//     The two blended fields are effectively independent, so the mixed
//     field's RMS is sqrt(w0^2 + w1^2) of a single field's: 1.0 when the
//     aspect lands on a table direction, 0.707 halfway between. Measured over
//     an aspect sweep, mean |rill| runs 76..114 (ratio 0.67, against the 0.707
//     predicted), peaking exactly on entries 0, 4 and 8. So rill contrast
//     varies by up to ~30% with aspect, with a period of 11.25 degrees of
//     aspect.
//     It is smooth, and it follows terrain aspect rather than a lattice, so it
//     does not read as a grid -- but it is a real artifact and it is
//     deliberately NOT papered over here. The obvious fix, constant-power
//     weights (w0^2 + w0^2 = 1), needs either a sqrt or a gain > 1 followed by
//     a clamp, and the clamp would cost the exact convex-combination bound
//     argument below, which is worth more. Sharpening the crossfade (quintic
//     weights) narrows the dim band instead of removing it, which may read
//     WORSE -- as a band with edges. Both are taste calls that the plan says
//     must be settled by probe measurement, not by eye, so neither is taken
//     here. If the calibration probe sees banding along aspect contours,
//     raise kRillSectors first: it is the one lever that costs nothing.
// ---------------------------------------------------------------------------

#include "voxelcore/hash.h"

namespace vxc {

// Hash channel. 26 is free: hash.h reserves 0..19 and 32..47, caves.h owns
// 18/20/21/24 and caverns.h owns 22/23/25.
//
// NOTE FOR INTEGRATION: 18 and 19 are currently DOUBLE-ALLOCATED --
// hash.h's CH_ECOTONE_TEMP/CH_ECOTONE_PRECIP collide with caves.h's
// CH_CAVE_NODE/CH_CAVE_EDGE. This id lives here rather than in hash.h per the
// task split, but the channel namespace needs centralising and that collision
// resolving.
inline constexpr uint32_t CH_RILL = 26;

// --- geometry ---------------------------------------------------------------

// Across-slope wavelength: the rill spacing. 1.6 m matches the existing
// isotropic 1600 mm detail octave (amplifier.cpp kDetailOctaves), which is the
// band this term is meant to ORGANISE rather than add to.
inline constexpr int64_t kRillAcrossMm = 1600;

// 8:1, giving ~12.8 m along-slope (the plan's "~13 m"). A power of two so the
// along-axis lattice is an exact multiple of the across-axis wavelength.
inline constexpr int64_t kRillElongation = 8;
inline constexpr int64_t kRillAlongMm = kRillAcrossMm * kRillElongation; // 12800

// PROVISIONAL -- UNCALIBRATED. AMPLITUDE MUST BE SET BY PROBE MEASUREMENT
// against the fine tier's measured S2 at the 1.6 m lag, per the plan
// ("amplitudes set by probe measurement, not taste"). The fine tier does not
// exist on the client yet, so this is a starting value, not a result.
//
// Where 300 comes from, so the next person can argue with the reasoning
// rather than the number: the existing isotropic 1600 mm octave carries
// 500 mm (amplifier.cpp kDetailOctaves, ~line 392) before its slope/micro
// scaling. A structured term that reorganises that band should not out-shout
// it, so 0.6x of it. DO NOT TUNE THIS BY EYE.
inline constexpr int64_t kRillAmplitudeMm = 300;

// The provable envelope. |rillMm(...)| <= kRillMaxAbsMm for every input in the
// documented domain; see the bound proof at rillScaleMm.
//
// WHY THIS IS EXACTLY ACHIEVABLE, AND WHY IT MATTERS. Domain warping cannot
// change value noise's output RANGE -- it only changes WHERE in the domain
// each output value lands. valueNoise2Fade is a convex combination of four
// corner hashes no matter what coordinates you hand it, so it is in
// [-32768, 32767] under any warp whatsoever, including this one. Blending two
// such samples with non-negative weights summing to one keeps that range, and
// the gate only ever scales it down. So this term costs the world's surface
// upper bound EXACTLY its gated amplitude and not a millimetre more. That
// bound is not bookkeeping: surfaceBoundsMm feeds the "this column is
// air/solid without evaluating it" tests, and a bound violation is a hole in
// the world.
inline constexpr int64_t kRillMaxAbsMm = kRillAmplitudeMm;

// --- gate -------------------------------------------------------------------
//
// Zero at or below 10% grade, full at 20%, quintic-smoothed between. TWO
// INDEPENDENT REASONS, both load-bearing:
//
//  (a) PHYSICS. Rills are cut by concentrated overland flow. On flat ground
//      there is no fall line for water to concentrate along, so downslope
//      grooves there are simply wrong -- they would read as combed carpet.
//
//  (b) NUMERICS. The gradient frame is undefined where the gradient vanishes:
//      the aspect of a flat is meaningless, and with integer components it is
//      also quantised to a handful of representable directions, so it snaps
//      between them. The gate makes that instability unobservable by
//      multiplying it by zero.
//
// THE GATE IS A RAMP, NOT A STEP. A step in amplitude across a contour is the
// same failure as a step across a tile boundary -- a seam, just along a
// different curve -- and it is exactly what this project is fixing. The ramp
// is quintic (C2), not linear, so even the amplitude's SLOPE is continuous;
// there is no kink to catch a hillshade.
//
// THE RAMP IS WIDE ENOUGH THAT (b) IS DISCHARGED BEFORE THE AMPLITUDE
// MATTERS. At the foot of the ramp the gradient components are already ~100
// mm/m, so the integer aspect is resolved to about 1/100 rad = 0.6 degrees,
// far finer than the 11.25 degree table spacing: the frame has already picked
// a stable sector before it contributes anything visible. By the half-
// amplitude point (15% grade) it is resolved to ~0.4 degrees.
inline constexpr int64_t kRillGateStartMmPerM = 100; // 10% grade, ~5.7 deg
inline constexpr int64_t kRillGateFullMmPerM = 200;  // 20% grade, ~11.3 deg
inline constexpr int64_t kRillGateWidthMmPerM = kRillGateFullMmPerM - kRillGateStartMmPerM;

// --- fixed-point scales -----------------------------------------------------

inline constexpr int64_t kRillDirQ = 4096;   // q12 direction cosines
inline constexpr int64_t kRillGateQ = 4096;  // q12 gate, also the quintic's domain
inline constexpr int64_t kRillSectors = 16;  // table directions over 180 degrees
inline constexpr int64_t kRillNoiseAbs = 32768; // |valueNoise2Fade| <= this

// Compile-time table of 16 integer direction pairs: cos/sin at 11.25 degree
// steps, q12, covering 180 degrees. A rill axis is a LINE, not a ray -- a
// groove running downhill is the same groove running uphill -- so the table
// only needs a half turn, and index 16 wraps to index 0.
//
// Rounded to q12 these pairs have |d| within 0.03% of 4096, so each field's
// wavelength is correct to 0.015%. That is 200x tighter than the octagonal
// norm used for the gate, which is the whole reason the frame comes from this
// table and not from a normalised gradient.
inline constexpr int32_t kRillDirCosQ[16] = {
    4096, 4017, 3783, 3406, 2896, 2276, 1567,  799,
       0, -799, -1567, -2276, -2896, -3406, -3783, -4017,
};
inline constexpr int32_t kRillDirSinQ[16] = {
       0,  799, 1567, 2276, 2896, 3406, 3783, 4017,
    4096, 4017, 3783, 3406, 2896, 2276, 1567,  799,
};

// --- domain -----------------------------------------------------------------
//
// Valid for |xMm|, |yMm| <= 2^45 (3.5e10 m, ~2000x the Earth's circumference)
// and |gradient| <= 2^40 mm/m. Widest intermediate is
// dirCos*xMm + dirSin*yMm <= 2^58, then *kRillElongation <= 2^49 after the
// q12 divide. Outside that domain the products overflow int64; nothing in the
// world can reach it.

// Octagonal norm: max(|x|,|y|) + min(|x|,|y|)/2. Sqrt-free magnitude estimate,
// used ONLY for the gate's grade test.
//
// TRADE-OFF. This overestimates the true length by 0 to +6.07% depending on
// direction (exact on the axes, worst at 45 degrees, where 1.5/sqrt(2) =
// 1.0607). It is a NORM, so it has no direction error of its own; the plan's
// "<=12% direction error" is the figure for the alpha=1, beta=1/4 variant and
// does not apply to this one -- the real cost is 6.07% and it is a magnitude
// error. What that buys us is a gate threshold that varies with aspect
// between 10% and 10.6% grade. That reads as natural variation in where rills
// start on a hillside; a sqrt would be exact, non-mirrorable and slower for
// no visible gain.
constexpr int64_t rillOctNorm(int64_t x, int64_t y) {
    const int64_t ax = x < 0 ? -x : x;
    const int64_t ay = y < 0 ? -y : y;
    const int64_t hi = ax > ay ? ax : ay;
    const int64_t lo = ax > ay ? ay : ax;
    return hi + floorDiv(lo, 2);
}

// Perlin's quintic 6t^5 - 15t^4 + 10t^3 on [0, kRillGateQ] -> [0, kRillGateQ].
//
// Factored as t^3 * (6t^2 - 15tT + 10T^2) / T^4 rather than expanded, for
// overflow: the expanded numerator would need 15*T^5 = 1.7e19 and blow int64,
// while this form's product is the quintic itself times T^5, hence <= T^5 =
// 1.15e18. The inner quadratic is strictly positive on [0, T] (discriminant
// 225T^2 - 240T^2 < 0), so every division here has a non-negative numerator
// and floorDiv agrees with HLSL's truncating divide.
constexpr int64_t rillQuinticQ(int64_t t) {
    const int64_t T = kRillGateQ;
    const int64_t inner = 6 * t * t - 15 * t * T + 10 * T * T;
    return floorDiv(t * t * t * inner, T * T * T * T);
}

// Grade gate, q12. 0 at or below kRillGateStartMmPerM, kRillGateQ at or above
// kRillGateFullMmPerM, C2 in between.
constexpr int64_t rillGateQ(int64_t gradMagMmPerM) {
    if (gradMagMmPerM <= kRillGateStartMmPerM) return 0;
    if (gradMagMmPerM >= kRillGateFullMmPerM) return kRillGateQ;
    const int64_t t =
        floorDiv((gradMagMmPerM - kRillGateStartMmPerM) * kRillGateQ, kRillGateWidthMmPerM);
    return rillQuinticQ(t);
}

// Aspect -> position in the direction table, in [0, kRillSectors*kRillDirQ].
// Integer part is the sector index, fractional part the blend weight.
//
// This is the "diamond angle" (L1 angle): a = (|gx| + gy - gx) / (2(|gx|+gy))
// for gy >= 0, which is continuous, strictly monotone in the true angle, and
// needs only adds and one divide -- no atan2, no trig table search. It warps
// the angle by at most ~4.07 degrees (worst near the octant midpoints: a true
// 22.5 degrees reports 26.4). That misorientation is the dominant term in the
// frame's total direction error and is why the table is 16 entries rather
// than more -- refining the table below ~8 degrees would be refining past the
// parametrisation's own error.
//
// The gradient is first folded into the upper half plane because the rill
// axis is a line: (gx,gy) and (-gx,-gy) must give the same field. The fold is
// seamless because a=0 and a=2 both land on table entry 0.
constexpr int64_t rillSectorQ(int64_t gradXMmPerM, int64_t gradYMmPerM) {
    int64_t gx = gradXMmPerM, gy = gradYMmPerM;
    if (gy < 0 || (gy == 0 && gx < 0)) { gx = -gx; gy = -gy; }
    const int64_t l1 = (gx < 0 ? -gx : gx) + gy; // > 0: caller gates |g| == 0 away
    return floorDiv((l1 - gx) * (kRillSectors * kRillDirQ), 2 * l1);
}

// One anisotropic field: rotate the world point into table direction k, scale
// the across-slope axis by kRillElongation, sample the quintic-faded value
// noise on that warped domain.
//
// valueNoise2Fade, NOT valueNoise2: the unfaded form is bilinear, so its
// gradient jumps across every lattice line. Under an 8:1 warp those lattice
// lines are 8:1 elongated too, which would put dead-straight creases along
// the fall line -- an artifact indistinguishable at a glance from the feature
// we are trying to create, and the exact artifact class this project exists
// to remove.
//
// The rotation matrix here is a COMPILE-TIME CONSTANT. That is the entire
// point: this field has no dependence on the local gradient, so it has no
// lever arm on |p| (see the header note).
constexpr int64_t rillFieldAtDir(uint64_t seed, int64_t xMm, int64_t yMm, int64_t k) {
    const int64_t dx = kRillDirCosQ[k];
    const int64_t dy = kRillDirSinQ[k];
    const int64_t along = floorDiv(dx * xMm + dy * yMm, kRillDirQ);
    const int64_t across = floorDiv(dx * yMm - dy * xMm, kRillDirQ);
    // One lattice cell = kRillAlongMm along the fall line, and
    // kRillAlongMm/kRillElongation = kRillAcrossMm across it.
    return valueNoise2Fade(seed, along, across * kRillElongation, kRillAlongMm, CH_RILL);
}

// Final scaling, split out so the bound is a compile-time statement.
//
// BOUND PROOF. noise is in [-kRillNoiseAbs, kRillNoiseAbs-1] and gateQ in
// [0, kRillGateQ], so the exact rational value is in
// [-kRillAmplitudeMm, kRillAmplitudeMm]. floorDiv is monotone
// non-decreasing in its numerator, so the extremes of the integer result are
// attained at the extremes of the input -- which the static_asserts below
// evaluate directly. No sampling argument, no slack.
constexpr int64_t rillScaleMm(int64_t noise, int64_t gateQ) {
    return floorDiv(noise * kRillAmplitudeMm * gateQ, kRillNoiseAbs * kRillGateQ);
}

static_assert(rillScaleMm(-kRillNoiseAbs, kRillGateQ) == -kRillMaxAbsMm,
              "most negative reachable output must equal -kRillMaxAbsMm exactly");
static_assert(rillScaleMm(kRillNoiseAbs - 1, kRillGateQ) <= kRillMaxAbsMm,
              "most positive reachable output must not exceed kRillMaxAbsMm");
static_assert(rillScaleMm(kRillNoiseAbs, kRillGateQ) == kRillMaxAbsMm,
              "the unreachable +kRillNoiseAbs corner still lands on the bound");
static_assert(rillScaleMm(-kRillNoiseAbs, 0) == 0 && rillScaleMm(kRillNoiseAbs, 0) == 0,
              "a closed gate must give exactly zero, not a rounding residue");
static_assert(kRillMaxAbsMm == kRillAmplitudeMm,
              "the term costs the surface bound exactly its gated amplitude");
static_assert(rillQuinticQ(0) == 0 && rillQuinticQ(kRillGateQ) == kRillGateQ,
              "quintic ramp must hit both endpoints exactly");
static_assert(rillGateQ(kRillGateStartMmPerM) == 0 &&
                  rillGateQ(kRillGateFullMmPerM) == kRillGateQ,
              "gate must close fully below the threshold and open fully above it");

// Signed downslope-elongated displacement in millimetres.
//
// gradXMmPerM/gradYMmPerM are the carrier's analytic gradient in the plan's
// mm-per-METRE currency (biome.h's kBiomeCliffSlopeMmPerM units), so every
// threshold in here means a grade at every tile scale.
//
// Pure function of world millimetres. No tile origin, no chunk, no level
// enters, so the field is seamless by construction rather than by matching
// boundary conditions.
constexpr int64_t rillMm(uint64_t seed, int64_t xMm, int64_t yMm,
                         int64_t gradXMmPerM, int64_t gradYMmPerM) {
    const int64_t gate = rillGateQ(rillOctNorm(gradXMmPerM, gradYMmPerM));
    // Early out on flats. Exact, not an approximation: the gate is identically
    // zero here, so nothing downstream can contribute. It also keeps the
    // undefined aspect of a flat -- and rillSectorQ's divide by |g| -- from
    // ever being evaluated.
    if (gate == 0) return 0;

    const int64_t sQ = rillSectorQ(gradXMmPerM, gradYMmPerM);
    const int64_t k0 = clampi64(floorDiv(sQ, kRillDirQ), 0, kRillSectors - 1);
    const int64_t k1 = (k0 + 1 == kRillSectors) ? 0 : k0 + 1;
    const int64_t f = sQ - k0 * kRillDirQ; // [0, kRillDirQ]

    const int64_t n0 = rillFieldAtDir(seed, xMm, yMm, k0);
    const int64_t n1 = rillFieldAtDir(seed, xMm, yMm, k1);
    // Convex combination: weights are non-negative and sum to kRillDirQ, so
    // the range is unchanged. This is what makes the bound above exact.
    const int64_t blend = floorDiv(n0 * (kRillDirQ - f) + n1 * f, kRillDirQ);

    return rillScaleMm(blend, gate);
}

} // namespace vxc
