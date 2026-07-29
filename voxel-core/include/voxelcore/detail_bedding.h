#pragma once
// Bedding term (Phase 3c/3d, docs/terrain-amplification-plan.md) -- regional
// strike/dip and the quasi-periodic banding it produces where the ground
// surface (2D) or a rock volume (3D) cuts through tilted bed planes.
//
// Header-only, integer-only (CI float ban). Every expression here is a plain
// free function of int64_t/uint64_t -- no templates, no std::, no sqrt, no
// trig -- so it mirrors into HLSL exactly like hash.h's valueNoise2Fade did.
// NOT wired into evalSurface/stratigraphyAt: functions only, the integrator
// composes them (task instruction).
//
// ---------------------------------------------------------------------------
// CHANNEL NOTE (read before adding more channels anywhere in this codebase)
// ---------------------------------------------------------------------------
// hash.h's HashChannel enum allocates CH_ECOTONE_TEMP=18 and CH_ECOTONE_PRECIP
// =19. caves.h separately defines CH_CAVE_NODE=18 and CH_CAVE_EDGE=19 in its
// own numbering comment ("Extends hash.h's HashChannel registry (18..20)").
// Those are the SAME two integers claimed by two different features already
// landed. This file does not touch either: it takes 27 and 28, unused by both
// registries as of this writing. Whoever owns hash.h/caves.h should reconcile
// 18/19 before it causes an actual noise collision (it hasn't yet only because
// the colliding features query different (x, y) domains in practice).
// ---------------------------------------------------------------------------

#include <cstdint>

#include "voxelcore/core.h"
#include "voxelcore/hash.h"

namespace vxc {

// This file's private channel allocation (hash.h's registry is append-only;
// these two are not claimed there or in caves.h/caverns.h as of this file's
// authoring -- see the channel note above).
inline constexpr uint32_t CH_BEDDING_STRIKE = 27; // regional strike/dip/thickness domain hash
inline constexpr uint32_t CH_BEDDING = 28;        // warp field + per-bed hardness/asymmetry

// =============================================================================
// 1. THE REGIONAL STRIKE/DIP FIELD
// =============================================================================
//
// Real bedding orientation is a property of the rock body, not of the point
// you happen to be standing on: it is constant over a whole hillside and
// steps only where you cross into a different structural domain (a fold limb,
// a fault block). So strike/dip is hashed from a coarse lattice -- 819.2 m,
// per the task spec -- and is a step function of (x, y), not a smoothly
// varying field. Two points 100 m apart read the identical strike; the field
// is only WITHIN-domain constant, and the domain-boundary step is measured,
// not hidden (see the "DOMAIN BOUNDARIES" section below and
// test_detail_bedding.cpp's transect test).
inline constexpr int64_t kBeddingDomainMm = 819'200; // 819.2 m, per task spec

// --- strike: a compile-time table of 16 integer direction pairs ------------
//
// Strike is a LINE orientation (mod 180 degrees), not a ray, so 16 evenly
// spaced strikes cover 180/16 = 11.25 degrees each, not 22.5. Trig is banned
// in this file at RUNTIME, but the table entries themselves are ordinary
// literal data -- exactly like a hand-written sine table -- computed once,
// offline, by rounding cos/sin(11.25*k) to the nearest integer at a common
// radius and writing down the result; nothing here calls a trig function.
//
// Construction: for k = 0..7 (0, 11.25, ..., 78.75 degrees), take
// round(64*cos), round(64*sin). For k = 8..15 (90, 101.25, ..., 168.75), the
// vector is the exact 90-degree rotation of entry (k-8): (-dy, dx). That
// rotation is an EXACT integer identity (no rounding), so the second half of
// the table inherits the first half's approximation quality for free, and
// strike and its perpendicular (the dip direction, used below) are always
// exactly orthogonal by construction: dot((dx,dy), (-dy,dx)) = -dx*dy+dy*dx
// = 0 identically, for every entry, with no trig and no rounding error. That
// exact orthogonality is load-bearing: it is what makes the along-strike
// invariance test below hold EXACTLY rather than approximately.
//
// Magnitude-squared of the 16 entries ranges 4050..4113 (63.64..64.13 units),
// i.e. within 0.8% of a common radius of 64 -- "similar magnitudes" per the
// task, chosen so a single compile-time divisor (kBeddingDirScale, below)
// approximates every entry's true length well enough that the banding
// wavelength does not visibly vary with strike direction (the residual
// +/-0.8% stretch is far below anything the eye or the probe would catch).
//
// ACTUAL ANGULAR SPACING ACHIEVED (target was a uniform 11.25 degrees):
//   idx : angle (deg) : gap to next
//    0  :   0.00       :  10.80
//    1  :  10.80       :  11.35
//    2  :  22.15       :  12.05
//    3  :  34.20       :  10.80
//    4  :  45.00       :  10.80
//    5  :  55.80       :  12.05
//    6  :  67.85       :  11.35
//    7  :  79.20       :  10.80
//    8  :  90.00       :  10.80
//    9  : 100.80        :  11.35
//   10  : 112.15        :  12.05
//   11  : 124.20        :  10.80
//   12  : 135.00        :  10.80
//   13  : 145.80        :  12.05
//   14  : 157.85        :  11.35
//   15  : 169.20        :  10.80  (wraps to 180.00 == 0.00)
// Mean spacing is exactly 11.25 degrees (16 gaps sum to 180 by construction);
// achieved spacing ranges 10.80..12.05 degrees, i.e. within +/-0.8 degrees of
// uniform. That is the honest number to quote, not "11.25 exactly" -- integer
// pairs cannot hit irrational-angle targets exactly.
struct BeddingDirection {
    int64_t dx;
    int64_t dy;
};

inline constexpr BeddingDirection kBeddingStrikeTable[16] = {
    {64, 0},     {63, 12},   {59, 24},   {53, 36},
    {45, 45},    {36, 53},   {24, 59},   {12, 63},
    {0, 64},     {-12, 63},  {-24, 59},  {-36, 53},
    {-45, 45},   {-53, 36},  {-59, 24},  {-63, 12},
};

// Common divisor approximating every table entry's true length (64.0 exactly
// for entries 0/8; within 0.8% for the rest). Deferring normalisation to a
// single constant -- rather than computing each direction's own isqrt, which
// this file cannot do at all (no sqrt) -- is precisely what keeps the whole
// pipeline sqrt-free.
inline constexpr int64_t kBeddingDirScale = 64;

// --- dip: a bounded integer ratio, not an angle -----------------------------
//
// Dip is expressed as rise-per-run in Q10 fixed point (1024 == a 45-degree
// dip), signed (which side of strike the beds dip down), clamped well short
// of vertical so the "unrolled" structural coordinate below never needs an
// unbounded slope. 3072/1024 = 3.0 corresponds to ~71.6 degrees -- steep, but
// finite, which a literal angle representation would not force without an
// explicit clamp of its own.
inline constexpr int64_t kBeddingDipMaxQ10 = 3072;

// --- bed thickness: hashed per structural domain, then perturbed per bed ---
inline constexpr int64_t kBeddingThicknessMinMm = 800;
inline constexpr int64_t kBeddingThicknessMaxMm = 3200;

// Extracts a uniform-ish integer in [lo, hi] from 16 bits of a hash, starting
// at bit `shift`. Not bit-exact-uniform (65535 does not divide most ranges),
// but the residual bias is far below anything a per-domain/per-bed hash needs
// to be free of. floorDiv is used even though both operands are provably
// non-negative here, per this codebase's discipline of never hand-verifying
// sign on a division (docs/determinism.md; a bare `/` on a negative operand
// is what diverged AMD vs NVIDIA once already).
constexpr int64_t beddingHashField16(uint64_t h, uint32_t shift, int64_t lo, int64_t hi) {
    const int64_t bits = static_cast<int64_t>((h >> shift) & 0xFFFFull); // [0, 65535]
    return lo + floorDiv(bits * (hi - lo), 65535);
}

// The one hash call that defines a whole structural domain's strike, dip and
// thickness together -- a single hash2, sliced into disjoint bit ranges,
// rather than three separate hash calls, since all three properties are
// legitimately the same regional fact (a fold limb's own geometry).
constexpr uint64_t beddingDomainHash(uint64_t seed, int64_t xMm, int64_t yMm) {
    const int64_t ci = floorDiv(xMm, kBeddingDomainMm);
    const int64_t cj = floorDiv(yMm, kBeddingDomainMm);
    return hash2(seed, ci, cj, CH_BEDDING_STRIKE);
}

// Diagnostic/composable accessors -- exposed publicly (not just used inside
// beddingRawAt below) because the task calls the strike/dip field out as its
// own deliverable (item 1), separate from the banding term that consumes it
// (item 2), and because the test suite needs to verify "regionally constant"
// directly rather than inferring it indirectly through banding output.
constexpr uint32_t beddingStrikeIndexAt(uint64_t seed, int64_t xMm, int64_t yMm) {
    return static_cast<uint32_t>(beddingDomainHash(seed, xMm, yMm) >> 60) & 0xFu; // top 4 bits
}
constexpr int64_t beddingDipQ10At(uint64_t seed, int64_t xMm, int64_t yMm) {
    return beddingHashField16(beddingDomainHash(seed, xMm, yMm), 32, -kBeddingDipMaxQ10,
                               kBeddingDipMaxQ10); // bits [32,48)
}
constexpr int64_t beddingThicknessMmAt(uint64_t seed, int64_t xMm, int64_t yMm) {
    return beddingHashField16(beddingDomainHash(seed, xMm, yMm), 16, kBeddingThicknessMinMm,
                               kBeddingThicknessMaxMm); // bits [16,32)
}

// =============================================================================
// 2/3. THE BANDING TERM, SHARED BETWEEN THE 2D AND 3D FORMS
// =============================================================================
//
// The bed planes are planes in 3D: elevation-of-bed-plane(x, y) = z0 +
// dipSlope * perp(x, y), where perp is the horizontal distance along the dip
// direction (strike rotated 90 degrees) and dipSlope is the Q10 ratio above.
// "Structural depth" D = z - dipSlope*perp UNROLLS the tilted stack into a
// flat one: at fixed D, you are always on the same bed regardless of where
// you are along strike, and stepping in z or in the dip direction moves you
// across beds exactly the way stepping down a real, tilted road-cut does.
//
// WHY THE RAW (UNDIVIDED) PROJECTION IS CARRIED AS FAR AS POSSIBLE, AND ONLY
// DIVIDED DOWN ONCE, LATE.
//
// The naive version of this computes perpMm = perpRaw / kBeddingDirScale
// immediately, then builds D from perpMm. That is exactly the mistake this
// codebase's own value-noise history warns about (hash.h's fadeFractionMm
// comment on the "classic value-noise blocking artifact"): perpRaw changes by
// only about kBeddingDirScale (64) per millimetre of x, and floorDiv-ing that
// by 64 produces a STAIRCASE -- perpMm sits still for ~64 consecutive 1 mm
// steps and then jumps by a whole unit. Multiplied by the dip slope and fed
// into the (steep) per-bed profile below, that single-unit jump becomes a
// visible multi-millimetre step recurring every ~64 mm, i.e. a new seam
// artifact of exactly the kind this project exists to remove -- except this
// one would appear WITHIN a structural domain, where the task requires no
// discontinuity at all.
//
// The fix is the one hash.h itself uses elsewhere: keep everything in "raw"
// (undivided) units through every stage whose own step-to-divisor ratio would
// otherwise be tiny, and only divide by something whose magnitude is large
// relative to the per-step change in its numerator (so the truncated result
// moves by a fraction of a unit most steps, not by a whole unit occasionally).
// Concretely: D itself is carried scaled by kBeddingDirScale (so z is scaled
// up to match perpRaw's units, and the only division applied to perpRaw is
// multiplication by dipQ10 THEN division by 1024 -- a divisor that is small
// relative to the pre-division delta, so the result moves smoothly). The
// warp and bed-index stages divide by quantities scaled by the bed thickness
// (hundreds of thousands of raw units), several orders of magnitude above the
// per-millimetre delta flowing into them. See
// test_detail_bedding.cpp's `no_large_step_within_domain` for the measured
// consequence.
inline constexpr int64_t kBeddingRawScale = 1024LL * 32768LL; // shapeQ10 * hardness denominator

// Bed thickness is not modelled as an explicit hashed scalar per bed index --
// that would need a cumulative sum of all thinner beds from some origin to
// find "which bed contains D", which is not an O(1) point function. Instead
// the structural coordinate is DOMAIN-WARPED by a smooth 1D hashed field
// before the uniform-period per-bed profile is applied. This is mathematically
// equivalent to a locally-varying instantaneous thickness (d(warped D)/d(D) =
// 1 + d(warp)/d(D), which is not constant), so real beds DO come out thicker
// and thinner in different places, without ever summing more than the two
// warp-lattice corners nearest D. Combined with a per-bed hashed asymmetry
// (the profile's peak is not fixed at the bed's midpoint -- see kBedding
// PeakFrac{Min,Max}Q10) this is the "beds vary in thickness" texture the task
// asks for, built as an O(1) point function rather than a search.
inline constexpr int64_t kBeddingWarpLatticeMul = 3;   // warp cycles over ~3 beds
inline constexpr int64_t kBeddingWarpAmpQ10 = 384;     // ~0.375 of thickness, Q10

// Per-bed profile asymmetry: the profile peaks somewhere in [30%, 70%] of the
// bed's own thickness rather than always at 50%, which is what gives some
// beds a steep resistant face on one side and a gentle weathered-out face on
// the other (real differential weathering), rather than a symmetric sine.
inline constexpr int64_t kBeddingPeakFracMinQ10 = 307; // ~0.30
inline constexpr int64_t kBeddingPeakFracMaxQ10 = 717; // ~0.70

constexpr int64_t beddingWarpCornerAt(uint64_t seed, int64_t i0) {
    return static_cast<int64_t>(hashToSigned16(hash2(seed, i0, 0, CH_BEDDING))); // [-32768,32767]
}
constexpr int64_t beddingHardnessAt(uint64_t seed, int64_t bedIdx) {
    return static_cast<int64_t>(hashToSigned16(hash2(seed, bedIdx, 1, CH_BEDDING))); // [-32768,32767]
}
constexpr int64_t beddingPeakFracQ10At(uint64_t seed, int64_t bedIdx) {
    return beddingHashField16(hash2(seed, bedIdx, 2, CH_BEDDING), 0, kBeddingPeakFracMinQ10,
                               kBeddingPeakFracMaxQ10);
}

// The shared core: same strike/dip/thickness field, same warp, same bed
// index and profile shape, for BOTH the 2D and 3D forms -- only the final
// amplitude differs between them (see beddingMm / beddingDisplacement3Mm
// below). Returns shapeQ10 * hardness, in [-kBeddingRawScale, kBeddingRawScale]
// exactly (shapeQ10 in [0,1024], hardness in [-32768,32767]).
//
// Precondition (not enforced, documented): |xMm|, |yMm| well under ~5e9 mm
// (5,000,000 km) so that perpRaw and dipQ10*perpRaw stay inside int64 with
// wide margin -- see the overflow arithmetic in the comment block above
// kBeddingMaxAbsMm below. test_detail_bedding.cpp fuzzes up to +/-2e9 mm.
// beddingRawAt's body, with the structural-domain hash SUPPLIED rather than
// recomputed. The three accessors above each call beddingDomainHash, so
// beddingRawAt paid for one hash three times -- and that hash is a function of
// the 819.2 m domain cell alone, i.e. constant over a whole column and over
// 819.2 m of ground either side of it. A caller evaluating a column's worth of
// z samples (Phase 4's density3.h band) hoists it out and calls this.
//
// This is a HOIST, not a variant: beddingRawAt below is literally this function
// applied to beddingDomainHash(seed, xMm, yMm), so the two cannot produce
// different numbers. hash2 is pure, so the hoist is value-identical and moves no
// worldgen output. worldgen.ush's mirror has always been written this way (one
// `dh`), so this also removes a shape difference between the two.
constexpr int64_t beddingRawFromDomain(uint64_t seed, uint64_t domainHash, int64_t xMm,
                                       int64_t yMm, int64_t zMm) {
    const uint32_t strikeIdx = static_cast<uint32_t>(domainHash >> 60) & 0xFu;
    const int64_t dipQ10 =
        beddingHashField16(domainHash, 32, -kBeddingDipMaxQ10, kBeddingDipMaxQ10);
    const int64_t thicknessMm =
        beddingHashField16(domainHash, 16, kBeddingThicknessMinMm, kBeddingThicknessMaxMm);

    const BeddingDirection strike = kBeddingStrikeTable[strikeIdx];
    const BeddingDirection dipDir{-strike.dy, strike.dx}; // exact 90-degree rotation

    // Raw (undivided) projection onto the dip direction -- see the big
    // comment above on why this is not reduced to "true mm" until forced to.
    const int64_t perpRaw = xMm * dipDir.dx + yMm * dipDir.dy;

    // Structural depth in "raw" units (mm * kBeddingDirScale).
    const int64_t drawZ = zMm * kBeddingDirScale;
    const int64_t drawDip = floorDiv(dipQ10 * perpRaw, 1024);
    const int64_t dRaw = drawZ - drawDip;

    // Domain warp: a smooth 1D hashed field over dRaw, giving locally
    // variable bed thickness (see kBeddingWarpLatticeMul's comment).
    const int64_t thicknessRaw = thicknessMm * kBeddingDirScale;
    const int64_t warpLatticeRaw = thicknessRaw * kBeddingWarpLatticeMul;
    const int64_t wi0 = floorDiv(dRaw, warpLatticeRaw);
    const int64_t wf = dRaw - wi0 * warpLatticeRaw; // [0, warpLatticeRaw)
    const int64_t wv0 = beddingWarpCornerAt(seed, wi0);
    const int64_t wv1 = beddingWarpCornerAt(seed, wi0 + 1);
    const int64_t warpAmpRaw = floorDiv(thicknessRaw * kBeddingWarpAmpQ10, 1024);
    const int64_t warpRaw =
        floorDiv((wv0 * (warpLatticeRaw - wf) + wv1 * wf) * warpAmpRaw, 32768LL * warpLatticeRaw);
    const int64_t dwRaw = dRaw + warpRaw;

    const int64_t bedIdx = floorDiv(dwRaw, thicknessRaw);
    const int64_t fracRaw = dwRaw - bedIdx * thicknessRaw; // [0, thicknessRaw)

    const int64_t pfQ10 = beddingPeakFracQ10At(seed, bedIdx);
    const int64_t pfRaw = floorDiv(pfQ10 * thicknessRaw, 1024); // in (0, thicknessRaw), see below

    // pfQ10 in [307,717] and thicknessRaw > 0 together guarantee 0 < pfRaw <
    // thicknessRaw, so both spans below are strictly positive; the `> 0`
    // guards are defensive only (kept because the whole point of this file is
    // not to trust an unchecked division).
    int64_t shapeQ10; // asymmetric tent, 0 at bed boundaries, 1024 at the hashed peak
    if (fracRaw <= pfRaw) {
        shapeQ10 = pfRaw > 0 ? floorDiv(fracRaw * 1024, pfRaw) : 1024;
    } else {
        const int64_t span = thicknessRaw - pfRaw;
        shapeQ10 = span > 0 ? floorDiv((thicknessRaw - fracRaw) * 1024, span) : 0;
    }

    const int64_t hardness = beddingHardnessAt(seed, bedIdx);
    return shapeQ10 * hardness;
}

constexpr int64_t beddingRawAt(uint64_t seed, int64_t xMm, int64_t yMm, int64_t zMm) {
    return beddingRawFromDomain(seed, beddingDomainHash(seed, xMm, yMm), xMm, yMm, zMm);
}

// =============================================================================
// 4. BOUNDS -- PROVABLE
// =============================================================================
//
// beddingRawAt returns shapeQ10*hardness with |shapeQ10| <= 1024 (both tent
// branches are a truncating division of a numerator no larger than the
// denominator, by construction of pfRaw above) and |hardness| <= 32768
// (hashToSigned16's documented range), so |beddingRawAt(...)| <=
// 1024*32768 = kBeddingRawScale EXACTLY, independent of x, y, z, seed, warp,
// bed index, or anything else -- the bound does not depend on the warp
// field's own amplitude at all, which is what makes it provable rather than
// merely plausible.
//
// beddingMm/beddingDisplacement3Mm then multiply by a fixed amplitude and
// floorDiv by kBeddingRawScale. For any raw in [-S, S] (S = kBeddingRawScale)
// and amplitude A > 0: A*raw is in [-A*S, A*S], and floorDiv(A*raw, S) is
// therefore in [-A, A] (floorDiv of X by positive S never produces a result
// whose magnitude exceeds |X|/S rounded away from zero by less than one full
// unit, and here X's magnitude is already an exact multiple of S at the
// extremes). So |beddingMm| <= kBeddingAmpMm and |beddingDisplacement3Mm| <=
// kBedding3AmpMm follow algebraically from the amplitude constants alone.
// test_detail_bedding.cpp's fuzz test exists to catch a mistake in this
// argument or its implementation, not to establish the bound in the first
// place.
//
// PROVISIONAL / UNCALIBRATED. The task (and the plan's Phase 3 text) is
// explicit that amplitudes here must eventually be set against the fine
// tier's measured S2(7.5m), which does not exist on the client yet -- there
// is nothing to calibrate against. These starting values are chosen by
// analogy with amplifier.cpp's kDetailOctaves table (the scale reference the
// task points at): bedding wavelength (800-3200mm, hashed thickness) sits
// between that table's 1600mm-lattice/500mm-amplitude microrelief octave and
// its 400mm-lattice/190mm-amplitude one, so 320mm is picked as a same-order-
// of-magnitude starting amplitude for the 2D term -- NOT tuned by eye, and
// NOT to be treated as final. Do not adjust without a real S2 measurement.
// 320 -> 120, MEASURED. At 320 mm this term alone contributes **4.66x the whole
// detail S2 budget** at the 0.4 m lag -- it was not one voice among several, it
// was drowning the ladder it sits on. The S2 method gives a CEILING here rather
// than a value (bedding is quasi-periodic and keyed on absolute elevation, so
// its horizontal wavelength varies with local slope and it has no fixed lag,
// which means S2 cannot distinguish it from an isotropic octave of the same
// energy); that ceiling is 69-132 mm across the sites measured. 120 sits at the
// top of the range, because the ceiling is the constraint and the term should be
// as legible as the budget allows.                                [PROVISIONAL]
inline constexpr int64_t kBeddingAmpMm = 120;

// The 3D envelope: plan Phase 3d sets an overall |D(x,y,z)| <= 700mm budget
// for stratigraphyAt's whole displacement, of which this bedding term is only
// ONE contributor (the plan's other contributor is a rock-gated valueNoise3
// pocket term, owned by whichever agent lands Phase 4). 500mm is chosen so
// the bedding contribution alone leaves ~200mm of the 700mm budget for that
// sibling term when the two are eventually summed -- the integrator must
// still check the SUM of all Phase-4 displacement terms against 700mm; this
// file only guarantees ITS OWN term stays within kBedding3MaxAbsMm.
inline constexpr int64_t kBedding3AmpMm = 500; // PROVISIONAL, see comment above

inline constexpr int64_t kBeddingMaxAbsMm = kBeddingAmpMm;   // 2D bound, proved above
inline constexpr int64_t kBedding3MaxAbsMm = kBedding3AmpMm; // 3D bound, proved above

static_assert(kBeddingAmpMm > 0 && kBedding3AmpMm > 0, "amplitudes must be positive");
static_assert(kBedding3MaxAbsMm <= 700,
              "plan Phase 3d's total 3D displacement envelope is 700mm; this term must leave "
              "room for the sibling pocket term, not consume the whole budget alone");
// Overflow guard for beddingMm/beddingDisplacement3Mm's `amp * beddingRawAt(...)`
// multiply: with |beddingRawAt| <= kBeddingRawScale (33,554,432), this holds
// with more than 12 orders of magnitude of margin for any sane amplitude, but
// it is asserted rather than assumed per this file's own "don't trust an
// unchecked multiply" rule.
static_assert(kBedding3AmpMm < floorDiv(int64_t{1} << 40, kBeddingRawScale),
              "amplitude too large: amp * beddingRawAt(...) risks int64 overflow");

// =============================================================================
// PUBLIC API
// =============================================================================

// 2D bedding displacement, in mm, for the surface amplifier: where the ground
// surface (at height surfaceMm) cuts the regional bed planes. |result| <=
// kBeddingMaxAbsMm, proved above.
constexpr int64_t beddingMm(uint64_t seed, int64_t xMm, int64_t yMm, int64_t surfaceMm) {
    return floorDiv(kBeddingAmpMm * beddingRawAt(seed, xMm, yMm, surfaceMm), kBeddingRawScale);
}

// 3D bedding displacement, in mm, for Phase 4's stratigraphyAt: the SAME
// strike/dip/warp/bed-index machinery as beddingMm, evaluated at an arbitrary
// z rather than only at the surface, so a recessed weak bed under a
// protruding resistant one is literally the same structure seen volumetrically
// (beddingMm(seed,x,y,s) and beddingDisplacement3Mm(seed,x,y,s) are built from
// the identical beddingRawAt(seed,x,y,s) call -- see the "2D and 3D agree"
// test). |result| <= kBedding3MaxAbsMm, proved above.
constexpr int64_t beddingDisplacement3Mm(uint64_t seed, int64_t xMm, int64_t yMm, int64_t zMm) {
    return floorDiv(kBedding3AmpMm * beddingRawAt(seed, xMm, yMm, zMm), kBeddingRawScale);
}

// The same displacement with the structural-domain hash hoisted out -- see
// beddingRawFromDomain. Identical value to beddingDisplacement3Mm when
// `domainHash == beddingDomainHash(seed, xMm, yMm)`, which is the only way any
// caller is allowed to produce it.
constexpr int64_t beddingDisplacement3FromDomain(uint64_t seed, uint64_t domainHash, int64_t xMm,
                                                 int64_t yMm, int64_t zMm) {
    return floorDiv(kBedding3AmpMm * beddingRawFromDomain(seed, domainHash, xMm, yMm, zMm),
                    kBeddingRawScale);
}

} // namespace vxc
