#pragma once
// Bounded 3D density displacement -- Phase 4, docs/terrain-amplification-plan.md
// section "3d. Bounded 3D density -- the overhang answer".
//
// WHAT THIS IS FOR. A heightmap has no overhang information. But real
// overhangs are not arbitrary: they are consequences of STRUCTURE -- bedding
// planes in layered rock, differential weathering of hard beds over soft ones,
// joint-controlled chimneys and flutes. All of those are functions of
// (surface, slope, aspect, lithology, drainage), every one of which the
// heightfield pipeline already computes. So this pass does not invent
// information; it expresses volumetrically the structure the heightfield
// already implies.
//
// Concretely: `stratigraphyAt` today tests `voxel centre <= surface`. This
// header supplies a displacement D(x, y, z) so the test becomes
// `z <= surface + D`, with a compile-time-provable envelope |D| <= 700 mm.
//
// WIRED IN AT kWorldGenVersion 12 (Wave C). `Amplifier::stratigraphyAt` now
// tests `centre <= surface + D`, with the column-invariant half of D hoisted
// into `ColumnSample::d3` (see Density3Column at the bottom of this file) the
// same way the cave and cavern passes hoist theirs. That bump carried the
// bound widening this header's section 0 asks for -- surfaceUpperBoundMm,
// surfaceLowerBoundMm and solidBelowBoundMm all moved by kDensity3MaxAbsMm,
// GeneratedWorld's surface brick range widened by 7 voxels either side, and
// amplifier.h's air-reason enumeration gained its fourth reason.
//
// Header-only, integer-only (CI float ban), C++20, UE-header-free. Every
// expression is a plain free function of int64_t/uint64_t -- no templates, no
// std::, no sqrt, no trig -- so it mirrors into HLSL exactly the way
// detail_rill.h and detail_bedding.h do. EVERY signed division goes through
// floorDiv; a bare `/` on a negative operand is what diverged AMD vs NVIDIA
// once already (docs/determinism.md).
//
// ---------------------------------------------------------------------------
// CHANNEL NOTE
// ---------------------------------------------------------------------------
// Allocated here: CH_POCKET = 29. Survey of the whole tree at time of writing:
// hash.h 0..15 (detail octaves), 16, 17, 18, 19, 32..47 (synthetic tiles);
// caves.h 18, 19, 20, 21, 24; caverns.h 22, 23, 25; detail_rill.h 26;
// detail_bedding.h 27, 28. So 29, 30 and 31 are the only free ids below the
// synthetic-tile reservation, and this file takes the first.
//
// detail_bedding.h's channel note records a REAL collision already in the
// tree: hash.h's CH_ECOTONE_TEMP/PRECIP (18/19) and caves.h's
// CH_CAVE_NODE/EDGE (18/19) are the same two integers claimed twice. This file
// does not touch either and does not fix it; it is flagged again here so the
// count above is not read as "the registry is clean".
// ---------------------------------------------------------------------------

#include <cstdint>

#include "voxelcore/biome.h" // kBiomeCliffSlopeMmPerM, tied to the slope gate below
#include "voxelcore/core.h"
#include "voxelcore/detail_bedding.h"
#include "voxelcore/detail_rill.h"
#include "voxelcore/hash.h"

namespace vxc {

inline constexpr uint32_t CH_POCKET = 29; // joint-controlled pocket/chimney noise

// =============================================================================
// 0. THE ENVELOPE, AND WHY THE BAND HALF-WIDTH EQUALS IT
// =============================================================================
//
// The plan fixes |D| <= 700 mm. That single number does three jobs:
//
//   1. It is the amount every surface-derived bound must widen by (brick
//      ranges, the air-reason enumeration at amplifier.h:167-196).
//   2. It is the half-width of the band in which D may be non-zero.
//   3. It is therefore what makes the skip EXACT rather than approximate.
//
// (2) and (3) are the same fact, and it is worth writing the argument out
// because "we skip far-from-surface voxels" usually means an approximation and
// here it does not. Let dz = z - surface.
//
//   * If dz > 700: since D <= 700 <= dz, `dz <= D` is false. The voxel is air
//     whether or not D was computed. Skipping gives the identical answer.
//   * If dz < -700: since D >= -700 >= dz, `dz <= D` is true. The voxel is
//     solid whether or not D was computed. Skipping gives the identical answer.
//
// There is no epsilon and no "visually indistinguishable" in that argument.
// Halving the envelope would halve the band and halve the cost, and widening
// it widens both -- band half-width is not a free tuning knob, it is forced
// equal to the envelope.
inline constexpr int64_t kDensity3MaxAbsMm = 700;
inline constexpr int64_t kDensity3BandHalfMm = kDensity3MaxAbsMm; // forced equal, see above

// Fixed-point quantum for all three gates. Aliased to detail_rill.h's rather
// than freshly invented, because rillQuinticQ -- reused below -- is written
// against it, and two quanta that must agree are one constant.
inline constexpr int64_t kDensity3GateQ = kRillGateQ;
static_assert(kDensity3GateQ == 4096,
              "the bound arithmetic below is written against a q12 gate; if detail_rill.h's "
              "quantum changes, re-derive the overflow margins here rather than assuming");

// =============================================================================
// 1. THE GATES -- SEPARATE, CHEAP, AND CALLABLE BEFORE ANYTHING EXPENSIVE
// =============================================================================
//
// WHY THIS TERM IS GATED AT ALL. The plan costs an ungated volumetric
// displacement at roughly DOUBLE VoxelizeMain everywhere -- it turns a
// stratigraphy lookup that is a handful of integer ops into one that hashes
// per voxel. That was rejected. Gated, the plan's figure is +50-80% on a
// cliff-dominated chunk and +5-10% world-average, and the whole difference is
// that the expensive part runs only where it can change an answer.
//
// So the gates are exposed as their own functions, in the order the caller
// should apply them, and NOT folded into the displacement call:
//
//     per column, once:   density3SlopeGateQ(gx, gy)   -- 4 compares, 1 divide
//                         density3RockGateQ(soilMm)    -- 1 compare, 1 divide
//                         if both are 0, the whole column is skipped
//     per voxel:          density3BandGateOpen(z, s)   -- 1 subtract, 1 compare
//                         if false, D is exactly 0 -- skip
//     only then:          density3DisplacementGatedMm(...)  -- ~70 hash rounds
//
// Hoisting the two column gates out of the z loop is the entire cost argument.
// A caller that calls density3DisplacementMm per voxel gets the right answer
// and pays the column gates 32x over; that overload exists for tests and for
// the reference/HLSL mirror, not for the hot loop.
//
// ---------------------------------------------------------------------------
// MEASURED COST, AND WHERE THE PLAN'S FIGURE LOOKS OPTIMISTIC
// ---------------------------------------------------------------------------
// Measured on this machine (CPU, single core, Release), against vxc_bench
// --radius 64 at 8^3 bricks, whose voxelize pass runs at 6.45 ns/voxel:
//
//     band predicate alone                     1.0 ns
//     full D, both gates open                100.5 ns   <- 7 hash2 + 8 hash3
//       of which  bedding component           48.4 ns
//                 pocket component            43.0 ns
//                 contrast curve               5.3 ns
//
// Gate pass rates on amplified SyntheticTileSampler terrain (90,000 columns;
// the shipped diffusion tile set is not in this checkout, so these are an
// ESTIMATE and the slope distribution is the synthetic sampler's, not a real
// region's):
//
//     slope gate open                      6.9% of columns
//     slope AND lithology gate open        5.1% of columns
//     voxels reaching the hash             ~6.9% of the voxels voxelize visits
//
// The slope-open rate is the number to distrust most: a second sample over a
// wider area of the same synthetic world read 12.9%, so it swings by 2x with
// the region and would swing further on real diffusion tiles, whose slope
// distribution reaches grades the synthetic sampler does not (the plan's five
// real sites run to 116% grade). Everything downstream of it scales linearly.
//
// That gives +6.9 ns on a 6.45 ns/voxel pass, i.e. voxelize roughly DOUBLES
// world-average, which is about +10% of total worldgen time (voxelize is 9.6%
// of amplify+voxelize+mesh). The plan quotes "+50-80% VoxelizeMain on a
// cliff-dominated chunk, +5-10% world-average". The world-average figure
// survives if it is read as a share of TOTAL worldgen; the cliff-chunk figure
// does not -- on a chunk where the slope gate is open everywhere the band
// covers essentially every visited voxel and voxelize goes up by an order of
// magnitude, not by 50-80%. The integrator should re-cost that claim rather
// than inherit it.
//
// TWO HOISTS RECOVER ABOUT HALF, AND NEITHER NEEDS THIS FILE TO CHANGE:
//
//   1. The pocket's z lattice (6400 mm) is more than four times the band
//      (1400 mm), so on 78% of columns all eight hash3 corners are the same
//      eight for every voxel in the column. Caching them per column removes
//      most of 43 ns.
//   2. beddingRawAt calls beddingDomainHash three times for one hash --
//      beddingStrikeIndexAt, beddingDipQ10At and beddingThicknessMmAt each
//      recompute it -- and all three are constant over an 819.2 m structural
//      domain, so they are column-invariant. That is ~3 of bedding's 7 hash2,
//      about 21 ns.
//
// Together those take ~100 ns to ~46 ns, i.e. voxelize +50% rather than +107%
// world-average.
//
// BOTH ARE DONE, at kWorldGenVersion 12. detail_bedding.h grew
// beddingRawFromDomain (hoist 2) and this file grew Density3PocketCorners
// (hoist 1) plus the Density3Column that carries both; the numbers each
// actually bought on the integrated path are in the Wave C notes rather than
// here, because they are properties of the caller's loop shape, not of this
// file. Both are VALUE-IDENTICAL rearrangements -- hash2/hash3 are pure, so a
// hoisted evaluation and a fresh one return the same integer, which is what
// lets worldgen.ush mirror the UNHOISTED form and still be bit-exact.

// --- gate (b): slope ---------------------------------------------------------
//
// Overhangs are a cliff phenomenon. Below roughly the angle of repose the
// ground is soil-mantled and creeping, and an undercut there would not stand
// up; the plan's threshold is "~60% grade".
//
// THE GATE IS CONTINUOUS, AND THAT IS NOT A NICETY. A step in overhang
// displacement across a grade contour is a step in the SURFACE along a curve
// -- a visible seam following an iso-slope line, which is the same failure
// class this whole project exists to remove (see the plan's v8 "gain step"
// row: median 150-310 mm of exactly this). So the gate ramps: zero at or below
// 60% grade, full at or above 90%, Perlin quintic in between, so the ramp is
// C2 at both ends and the displacement grows out of nothing.
//
// The band 600..800 is CENTRED on biome.h's kBiomeCliffSlopeMmPerM (700 mm/m,
// 70% grade, the angle of repose at which the biome table already paints
// BARE_ROCK): the gate is exactly half open at the angle of repose, closed 10
// points of grade below it and fully open 10 points above. One physical
// threshold, two consumers, rather than a second independent cliff constant
// that could drift away from the first.
//
// The WIDTH was set by measurement, and the first cut of it was wrong in a way
// worth recording. It was 600..900, which reads more conservative and is
// worse: the terrain's slope distribution falls off steeply, so a 600..900
// ramp left 95% of the qualifying columns sitting INSIDE the ramp with a
// partly-closed gate scaling the displacement down, and the overhang rate
// collapsed to 0.8% of cliff columns. Narrowing to 600..800 costs nothing in
// continuity -- the gate is C2 at both ends either way, and slope varies over
// 30 m tile pixels so even the narrower ramp changes D by a fraction of a
// millimetre per millimetre of ground -- and it is what makes the term
// actually do something on the ground that qualifies for it.
inline constexpr int64_t kDensity3SlopeStartMmPerM = 600; // 60% grade, ~31 degrees
inline constexpr int64_t kDensity3SlopeFullMmPerM = 800;  // 80% grade, ~39 degrees
inline constexpr int64_t kDensity3SlopeWidthMmPerM =
    kDensity3SlopeFullMmPerM - kDensity3SlopeStartMmPerM;
static_assert(kDensity3SlopeStartMmPerM < kBiomeCliffSlopeMmPerM &&
                  kBiomeCliffSlopeMmPerM < kDensity3SlopeFullMmPerM,
              "the slope ramp must straddle the biome cliff threshold: overhangs should be "
              "emerging as the ground becomes bare rock, not switching on somewhere else");

// Magnitude of the surface gradient, sqrt-free. Reuses detail_rill.h's
// octagonal norm rather than restating it -- it is a pure norm with no
// feature-specific constants, it is already fuzzed by test_detail_rill.cpp,
// and using a DIFFERENT magnitude estimate here would mean the rill gate and
// the overhang gate disagreed about what "60% grade" means by up to 6%.
// It overestimates true length by 0..+6.07% depending on aspect, so the
// effective threshold varies between 60% and 63.6% grade with aspect. That
// reads as natural variation in where overhangs start on a hillside.
constexpr int64_t density3SlopeMagMmPerM(int64_t gradXMmPerM, int64_t gradYMmPerM) {
    return rillOctNorm(gradXMmPerM, gradYMmPerM);
}

// The gate proper, taking a slope MAGNITUDE. This is the primary form because
// it is the one the existing pipeline can call today: Amplifier::SurfaceEval
// and the biome classifier already carry `slopeMmPerM` as a scalar (from the
// carrier's analytic gradient), and requiring the gradient VECTOR would force
// ColumnSample to widen for a term that only ever needed the magnitude. The
// vector overloads below exist for callers that happen to hold the components
// (detail_rill.h's consumers do).
constexpr int64_t density3SlopeGateQFromMag(int64_t slopeMmPerM) {
    if (slopeMmPerM <= kDensity3SlopeStartMmPerM) return 0;
    if (slopeMmPerM >= kDensity3SlopeFullMmPerM) return kDensity3GateQ;
    const int64_t t = floorDiv((slopeMmPerM - kDensity3SlopeStartMmPerM) * kDensity3GateQ,
                               kDensity3SlopeWidthMmPerM);
    return rillQuinticQ(t);
}

// Cheap boolean form: is this column capable of a non-zero D at any z?
// Exact -- the gate returns exactly 0 on the same condition -- so a caller
// that skips a whole column on this gets bit-identical results.
constexpr bool density3SlopeGateOpenFromMag(int64_t slopeMmPerM) {
    return slopeMmPerM > kDensity3SlopeStartMmPerM;
}

constexpr int64_t density3SlopeGateQ(int64_t gradXMmPerM, int64_t gradYMmPerM) {
    return density3SlopeGateQFromMag(density3SlopeMagMmPerM(gradXMmPerM, gradYMmPerM));
}
constexpr bool density3SlopeGateOpen(int64_t gradXMmPerM, int64_t gradYMmPerM) {
    return density3SlopeGateOpenFromMag(density3SlopeMagMmPerM(gradXMmPerM, gradYMmPerM));
}

// --- gate (a): the band ------------------------------------------------------
//
// Cheap per-voxel predicate: one subtract, one absolute value, one compare.
// This is the test that must be affordable, because it runs on every voxel of
// every column that passed the slope gate.
constexpr bool density3BandGateOpen(int64_t zMm, int64_t surfaceMm) {
    const int64_t dz = zMm - surfaceMm;
    return (dz < 0 ? -dz : dz) < kDensity3BandHalfMm;
}

// The band's own taper. D must reach zero AT the band edge, not jump to zero
// there: |dz| = 700 is a surface in the world (it is an offset surface of the
// terrain), and a discontinuity across it would draw that offset surface into
// the geometry as a shelf. So D is at full strength within +/-400 mm and
// quintic-ramps to exactly 0 over the outer 300 mm.
//
// The core half-width is a real trade and worth stating. The taper caps how
// far the displaced surface can actually move: a nose can protrude to the
// largest dz satisfying dz <= taper(dz)/Q * 700, which for a 400/700 split is
// 511 mm (pinned by test_density3.cpp). Widening the core raises that
// toward 700 mm at the price of a steeper taper; narrowing it makes the term
// gentler and the overhangs smaller. 400 is chosen so the reachable
// displacement is ~+/-0.5 m -- about 1 m of peak-to-trough relief across a
// face, which is a ledge, and about 5 voxels, which is enough for the mesher
// to express.
inline constexpr int64_t kDensity3BandCoreMm = 400;
inline constexpr int64_t kDensity3BandRampMm = kDensity3BandHalfMm - kDensity3BandCoreMm;
static_assert(kDensity3BandRampMm > 0, "the band needs a taper region");

constexpr int64_t density3BandTaperQ(int64_t zMm, int64_t surfaceMm) {
    const int64_t d0 = zMm - surfaceMm;
    const int64_t d = d0 < 0 ? -d0 : d0;
    if (d >= kDensity3BandHalfMm) return 0; // exactly zero outside the band
    if (d <= kDensity3BandCoreMm) return kDensity3GateQ;
    const int64_t t = floorDiv((d - kDensity3BandCoreMm) * kDensity3GateQ, kDensity3BandRampMm);
    return kDensity3GateQ - rillQuinticQ(t);
}

// --- the pocket term's own gate: lithology ----------------------------------
//
// Joint-controlled chimneys and flutes are a bare-rock phenomenon. On a
// soil-mantled slope the joints are buried and the surface is regolith, so the
// pocket term must fade out as the column's soil thickens.
//
// This is a COLUMN property, not a per-voxel depth ramp, and getting that
// wrong is a trap worth recording. The obvious implementation -- "zero the
// pocket until you are below the soil, then ramp it in" -- makes the term
// completely inert. D only changes an answer where it can flip `dz <= D`, and
// that happens near dz = 0; a term forced to zero in a neighbourhood of dz = 0
// carves nothing at all (at dz = -400 with D = -200, `-400 <= -200` is still
// true, so the voxel is still solid, and nothing was removed). So the gate is
// on the column's soil THICKNESS, evaluated once, and the pocket keeps its
// full profile through dz = 0 where it can actually cut.
//
// Continuous for the same reason the slope gate is: soil depth is a smooth
// field, and a step in it would be a seam along a soil-depth contour.
//
// The caller passes the depth of soil above rock: `col.topsoilMm` where the
// surface material is rock (amplifier.cpp's stratigraphyAt reads ROCK straight
// through the subsoil band under a BARE_ROCK surface), otherwise
// `col.topsoilMm + col.subsoilMm`. Passing 0 opens the gate fully. Note that
// the slope gate has already restricted us to >=60% grades, where
// amplifier.cpp's slope-retention factor has driven topsoil to its floor
// (kTopsoilMinMm = 100 mm), so on real cliffs this gate is open and it is
// doing its work at the margins -- soil-choked benches inside an otherwise
// steep face.
inline constexpr int64_t kDensity3RockSoilFullMm = 200; // <= this: bare rock, full pockets
inline constexpr int64_t kDensity3RockSoilZeroMm = 800; // >= this: mantled, no pockets
inline constexpr int64_t kDensity3RockSoilWidthMm =
    kDensity3RockSoilZeroMm - kDensity3RockSoilFullMm;
static_assert(kDensity3RockSoilWidthMm > 0, "the lithology gate needs a ramp region");

constexpr int64_t density3RockGateQ(int64_t soilDepthMm) {
    if (soilDepthMm <= kDensity3RockSoilFullMm) return kDensity3GateQ;
    if (soilDepthMm >= kDensity3RockSoilZeroMm) return 0;
    const int64_t t =
        floorDiv((soilDepthMm - kDensity3RockSoilFullMm) * kDensity3GateQ, kDensity3RockSoilWidthMm);
    return kDensity3GateQ - rillQuinticQ(t);
}

// =============================================================================
// 2. THE POCKET TERM -- 3D VALUE NOISE
// =============================================================================
//
// hash.h HAS NO valueNoise3. It ships valueNoise2 and valueNoise2Fade only,
// and hash.h is owned by another change in flight, so the 3D form is
// implemented here instead of added there. If a valueNoise3 lands in hash.h
// later, this is the function to delete -- it is a straight trilinear
// extension of valueNoise2Fade over hash.h's existing hash3, reusing hash.h's
// fadeFractionMm verbatim, so a hash.h version with the same lattice
// convention would be bit-identical.
//
// TWO LATTICES, NOT ONE. Joints are near-vertical fracture sets, so the
// features this wants are vertical flutes and chimneys, not isotropic blobs.
// The z lattice is therefore 4x the horizontal one. Doing it with two lattices
// rather than by pre-scaling z (`z * 1 / 4`) matters: pre-scaling divides a
// world coordinate by a small constant, which is precisely the staircase
// detail_bedding.h's long comment warns about -- z would sit still for four
// consecutive millimetres and then jump. Two lattices have no division on the
// coordinate at all.
//
// BILINEAR WOULD NOT DO. valueNoise2's plain bilinear form has a discontinuous
// gradient across every lattice plane; in 3D that is a visible rectangular
// blocking of the recesses on a 1.6 m grid. The quintic fade (hash.h's own
// cure, and the reason valueNoise2Fade exists) makes the lattice disappear,
// and it costs the bound nothing because a monotone remap of the fraction
// leaves the interpolation a CONVEX COMBINATION of the eight corner hashes.
//
// DOMAIN. Requires latXYMm, latZMm > 0 and latXYMm^2 * latZMm * 32768 inside
// int64: with the constants below that product is 5.4e14 against 9.2e18, i.e.
// four orders of margin. World coordinates are unrestricted apart from the
// x0/y0/z0 lattice indices staying in int64, which they do everywhere in this
// world.
//
// Returns [-32768, 32767], exactly valueNoise2Fade's range.
constexpr int64_t density3ValueNoise3Fade(uint64_t seed, int64_t xMm, int64_t yMm, int64_t zMm,
                                          int64_t latXYMm, int64_t latZMm, uint32_t channel) {
    const int64_t x0 = floorDiv(xMm, latXYMm);
    const int64_t y0 = floorDiv(yMm, latXYMm);
    const int64_t z0 = floorDiv(zMm, latZMm);
    // floorDiv guarantees each raw fraction is in [0, lattice), so
    // fadeFractionMm's internal truncating divides all have non-negative
    // numerators and agree with the HLSL mirror's truncDiv.
    const int64_t fx = fadeFractionMm(xMm - x0 * latXYMm, latXYMm);
    const int64_t fy = fadeFractionMm(yMm - y0 * latXYMm, latXYMm);
    const int64_t fz = fadeFractionMm(zMm - z0 * latZMm, latZMm);
    const int64_t gx = latXYMm - fx, gy = latXYMm - fy, gz = latZMm - fz;

    const int64_t v000 = hashToSigned16(hash3(seed, x0, y0, z0, channel));
    const int64_t v100 = hashToSigned16(hash3(seed, x0 + 1, y0, z0, channel));
    const int64_t v010 = hashToSigned16(hash3(seed, x0, y0 + 1, z0, channel));
    const int64_t v110 = hashToSigned16(hash3(seed, x0 + 1, y0 + 1, z0, channel));
    const int64_t v001 = hashToSigned16(hash3(seed, x0, y0, z0 + 1, channel));
    const int64_t v101 = hashToSigned16(hash3(seed, x0 + 1, y0, z0 + 1, channel));
    const int64_t v011 = hashToSigned16(hash3(seed, x0, y0 + 1, z0 + 1, channel));
    const int64_t v111 = hashToSigned16(hash3(seed, x0 + 1, y0 + 1, z0 + 1, channel));

    const int64_t lo = (v000 * gx + v100 * fx) * gy + (v010 * gx + v110 * fx) * fy;
    const int64_t hi = (v001 * gx + v101 * fx) * gy + (v011 * gx + v111 * fx) * fy;
    // (gx+fx)(gy+fy)(gz+fz) == latXY^2 * latZ exactly, so this is a convex
    // combination of eight values in [-32768, 32767] and the quotient is in
    // that same range before flooring; floorDiv of a value in [-32768, 32767]
    // is in [-32768, 32767]. Numerator is routinely negative -- floorDiv, not
    // a bare divide.
    return floorDiv(lo * gz + hi * fz, latXYMm * latXYMm * latZMm);
}

inline constexpr int64_t kDensity3NoiseAbs = 32768; // |density3ValueNoise3Fade| <= this

// PROVISIONAL / UNCALIBRATED -- do not treat as final, and do not tune by eye.
//
// detail_bedding.h's amplitudes carry the same warning for the same reason:
// the thing these should be set against is the fine tier's measured S2 at the
// relevant scale, and that measurement does not exist on the client yet. What
// IS fixed, and is not provisional, is the arithmetic: 500 mm of the 700 mm
// envelope is already claimed by detail_bedding.h's kBedding3MaxAbsMm, so this
// term gets what is left and not a millimetre more. If the pocket amplitude is
// ever raised, the bedding amplitude must come down by the same amount or the
// envelope must be renegotiated with every consumer of kDensity3MaxAbsMm --
// the static_assert below is what forces that conversation.
//
// The lattice sizes are likewise a starting point: 1.6 m across and 6.4 m
// vertically, chosen to sit at the same scale as amplifier.cpp's finest
// microrelief octave (1600 mm lattice) so the pockets read as the same
// material as the surrounding rock rather than as a separate effect.
inline constexpr int64_t kDensity3PocketAmpMm = kDensity3MaxAbsMm - kBedding3MaxAbsMm; // 200
inline constexpr int64_t kDensity3PocketLatXYMm = 1600;
inline constexpr int64_t kDensity3PocketLatZMm = 6400; // 4:1 vertical -- joints are near-vertical

// Amplitude + lithology gate applied to a raw noise sample. Split out from
// density3PocketMm so the bound can be static_asserted at the extremes.
//
// BOUND. noise is in [-kDensity3NoiseAbs, kDensity3NoiseAbs - 1] and gateQ in
// [0, kDensity3GateQ], so the exact rational is in [-amp, amp) and floorDiv
// (toward -inf) lands in [-amp, amp - 1] -- hence |result| <= amp, attained
// exactly at the negative extreme and never exceeded at the positive one.
constexpr int64_t density3PocketScaleMm(int64_t noise, int64_t rockGateQ) {
    return floorDiv(noise * kDensity3PocketAmpMm * rockGateQ,
                    kDensity3NoiseAbs * kDensity3GateQ);
}

static_assert(density3PocketScaleMm(-kDensity3NoiseAbs, kDensity3GateQ) ==
                  -kDensity3PocketAmpMm,
              "the negative extreme must reach exactly -amp");
static_assert(density3PocketScaleMm(kDensity3NoiseAbs - 1, kDensity3GateQ) <
                  kDensity3PocketAmpMm,
              "the positive extreme must stay strictly inside +amp");
static_assert(density3PocketScaleMm(-kDensity3NoiseAbs, 0) == 0 &&
                  density3PocketScaleMm(kDensity3NoiseAbs - 1, 0) == 0,
              "a closed lithology gate must give exactly zero, not a rounding residue");

// Signed pocket displacement, mm. |result| <= kDensity3PocketAmpMm.
constexpr int64_t density3PocketMm(uint64_t seed, int64_t xMm, int64_t yMm, int64_t zMm,
                                   int64_t rockGateQ) {
    if (rockGateQ == 0) return 0; // exact early out, not an approximation
    const int64_t n = density3ValueNoise3Fade(seed, xMm, yMm, zMm, kDensity3PocketLatXYMm,
                                              kDensity3PocketLatZMm, CH_POCKET);
    return density3PocketScaleMm(n, rockGateQ);
}

// --- HOIST 1: the eight corners, cached per column ---------------------------
//
// The pocket's z lattice is 6400 mm and the band is 1400 mm, so
// floorDiv(z, 6400) is the SAME index for every voxel of the band on
// 1 - 1400/6400 = 78.1% of columns. On those columns all eight hash3 corners
// are the same eight for the whole column, and eight hashes is nearly the whole
// cost of the pocket term.
//
// Cached as the eight hashToSigned16 VALUES, not as the hashes: they are in
// [-32768, 32767] so int32_t holds them exactly, and 8 x 4 bytes is what this
// costs a ColumnSample. The cache carries its own z0 so the 21.9% of columns
// whose band straddles a lattice plane are detected rather than mis-served --
// density3PocketCachedMm falls back to a fresh evaluation there, so the answer
// is the SAME integer either way and the cache is an optimisation with no
// correctness surface at all. That is the property worldgen.ush's mirror leans
// on: it does not cache, and it is still bit-exact.
struct Density3PocketCorners {
    int64_t z0 = 0;    // the z lattice index these eight corners belong to
    int32_t v[8] = {}; // (x0,y0,z0) (x0+1,..) (..y0+1..) ... in the order below
};

constexpr Density3PocketCorners density3PocketCornersAt(uint64_t seed, int64_t xMm, int64_t yMm,
                                                        int64_t zMm) {
    const int64_t x0 = floorDiv(xMm, kDensity3PocketLatXYMm);
    const int64_t y0 = floorDiv(yMm, kDensity3PocketLatXYMm);
    const int64_t z0 = floorDiv(zMm, kDensity3PocketLatZMm);
    Density3PocketCorners c;
    c.z0 = z0;
    c.v[0] = static_cast<int32_t>(hashToSigned16(hash3(seed, x0, y0, z0, CH_POCKET)));
    c.v[1] = static_cast<int32_t>(hashToSigned16(hash3(seed, x0 + 1, y0, z0, CH_POCKET)));
    c.v[2] = static_cast<int32_t>(hashToSigned16(hash3(seed, x0, y0 + 1, z0, CH_POCKET)));
    c.v[3] = static_cast<int32_t>(hashToSigned16(hash3(seed, x0 + 1, y0 + 1, z0, CH_POCKET)));
    c.v[4] = static_cast<int32_t>(hashToSigned16(hash3(seed, x0, y0, z0 + 1, CH_POCKET)));
    c.v[5] = static_cast<int32_t>(hashToSigned16(hash3(seed, x0 + 1, y0, z0 + 1, CH_POCKET)));
    c.v[6] = static_cast<int32_t>(hashToSigned16(hash3(seed, x0, y0 + 1, z0 + 1, CH_POCKET)));
    c.v[7] = static_cast<int32_t>(hashToSigned16(hash3(seed, x0 + 1, y0 + 1, z0 + 1, CH_POCKET)));
    return c;
}

// density3ValueNoise3Fade's interpolation with the eight corner hashes already
// in hand. Character-for-character the same arithmetic in the same order --
// only the eight `hashToSigned16(hash3(...))` calls are replaced by the cached
// values they would have produced.
constexpr int64_t density3PocketNoiseFrom(const Density3PocketCorners& c, int64_t xMm,
                                          int64_t yMm, int64_t zMm) {
    const int64_t latXYMm = kDensity3PocketLatXYMm;
    const int64_t latZMm = kDensity3PocketLatZMm;
    const int64_t x0 = floorDiv(xMm, latXYMm);
    const int64_t y0 = floorDiv(yMm, latXYMm);
    const int64_t z0 = floorDiv(zMm, latZMm);
    const int64_t fx = fadeFractionMm(xMm - x0 * latXYMm, latXYMm);
    const int64_t fy = fadeFractionMm(yMm - y0 * latXYMm, latXYMm);
    const int64_t fz = fadeFractionMm(zMm - z0 * latZMm, latZMm);
    const int64_t gx = latXYMm - fx, gy = latXYMm - fy, gz = latZMm - fz;

    const int64_t lo = (int64_t{c.v[0]} * gx + int64_t{c.v[1]} * fx) * gy +
                       (int64_t{c.v[2]} * gx + int64_t{c.v[3]} * fx) * fy;
    const int64_t hi = (int64_t{c.v[4]} * gx + int64_t{c.v[5]} * fx) * gy +
                       (int64_t{c.v[6]} * gx + int64_t{c.v[7]} * fx) * fy;
    return floorDiv(lo * gz + hi * fz, latXYMm * latXYMm * latZMm);
}

// density3PocketMm with the corner cache consulted. EXACTLY equal to
// density3PocketMm for every input, whether the cache hits or misses; pinned by
// test_density3.cpp's `pocket_cache_is_value_identical`.
constexpr int64_t density3PocketCachedMm(const Density3PocketCorners& c, uint64_t seed,
                                         int64_t xMm, int64_t yMm, int64_t zMm,
                                         int64_t rockGateQ) {
    if (rockGateQ == 0) return 0;
    const int64_t n = (floorDiv(zMm, kDensity3PocketLatZMm) == c.z0)
                          ? density3PocketNoiseFrom(c, xMm, yMm, zMm)
                          : density3ValueNoise3Fade(seed, xMm, yMm, zMm, kDensity3PocketLatXYMm,
                                                    kDensity3PocketLatZMm, CH_POCKET);
    return density3PocketScaleMm(n, rockGateQ);
}

// =============================================================================
// 3. THE BEDDING TERM -- THE SAME STRUCTURE, SEEN VOLUMETRICALLY
// =============================================================================
//
// This is the load-bearing design decision in the whole file, so it is stated
// rather than left to be inferred from a one-line call.
//
// The 3D bedding contribution is detail_bedding.h's beddingDisplacement3Mm and
// nothing else -- the SAME regional strike/dip field hashed off the same
// 819.2 m structural lattice, the same domain warp, the same bed index, the
// same per-bed hardness and asymmetry that produce the 2D banding on the face.
// Only the amplitude differs (500 mm here against kBeddingAmpMm for the 2D
// term -- 120 mm since that constant was re-measured against the detail S2
// budget, not the 320 mm this comment was first written against), and
// both are floorDiv scalings of the identical beddingRawAt(seed, x, y, z).
// NOTHING in this file is derived from kBeddingAmpMm: the 500 mm allocation
// comes out of kDensity3MaxAbsMm via kBedding3MaxAbsMm, so the 2D amplitude can
// move again without touching the envelope arithmetic below. The one place the
// two are coupled is a TEST -- test_density3.cpp's dominance threshold, which
// derives itself from both constants for exactly this reason.
//
// WHY NOT AN INDEPENDENT 3D FIELD. Because then the overhangs would not line
// up with the bands. A face already shows quasi-periodic banding from the 2D
// term; if the volumetric recesses were hashed independently, the undercuts
// would sit at unrelated heights and the face would read as two unrelated
// patterns superimposed -- structurally incoherent in a way a viewer notices
// immediately, and WORSE than a face with no overhangs at all, which at least
// reads as one consistent rock. Sharing the field is what makes "a recessed
// weak bed under a protruding resistant one" literally the same object seen
// two ways. test_density3.cpp's `bedding_component_is_in_phase_with_2d_banding`
// is the test that fails if anyone replaces this call.
//
// The one honest caveat, inherited not introduced: the strike/dip field is a
// STEP function of (x, y) across 819.2 m structural-domain boundaries, so the
// bedding orientation changes discontinuously there. That is detail_bedding.h's
// documented and intended behaviour -- a fold limb or fault block really does
// end -- and this file does not soften it. It means the continuity claims below
// are within-domain claims, exactly as they are for the 2D term.
// --- the bed CONTACT is sharp, and without that there are no overhangs ------
//
// THE MEASUREMENT THAT FORCED THIS. An overhang exists exactly where the
// displaced surface `z = surface + D(z)` doubles back, i.e. where dD/dz > 1.
// beddingDisplacement3Mm alone, sampled over 1.6M points, reaches |dD/dz| > 1
// on 0.29% of samples and never exceeds 1.80; feeding it straight through with
// BOTH gates forced fully open produced an overhang on 39 of 40,000 columns
// (0.098%), and on realistic terrain 2 of 20,283 cliff columns (0.01%). That
// is a term that is bounded, continuous, gated -- and geometrically inert. It
// would pass every other test in test_density3.cpp and buy nothing.
//
// The arithmetic behind that is not a bug, it is a scale mismatch, and it is
// worth stating because it constrains anything else that gets added to this
// band. A displacement of amplitude A produces dD/dz > 1 only if its vertical
// feature size is smaller than about A. detail_bedding.h's beds are 0.8-3.2 m
// thick and its tent profile ramps over a whole half-bed, so with A = 500 mm
// the profile is three to six times too gentle. No amplitude available inside
// a 700 mm envelope fixes that: doubling A to 1000 mm would still leave the
// median bed too thick, and 1000 mm is outside the envelope anyway.
//
// WHAT IS ACTUALLY WRONG IS THE PROFILE, NOT THE AMPLITUDE. A bedding plane is
// a CONTACT -- millimetres of shale between metres of sandstone. The rock
// either side of it is uniform; the hardness change across it is a step, not a
// ramp. detail_bedding.h's linear tent is the smooth approximation, and it is
// the smoothness, not the size, that removes the overhangs. So this file
// applies a contrast curve: a monotone odd remap of the bedding value that
// steepens it near zero (the bed contacts) and flattens it near its extremes
// (the bed faces). Physically that is "uniform bed, sharp contact"; visually it
// turns a sine-ish corrugation into a stack of ledges.
//
// WHY A MONOTONE POINTWISE REMAP AND NOT A DIFFERENT FIELD. This is the whole
// reason it is safe:
//
//   * It is a function of the bedding VALUE only, so it cannot move a band. The
//     zeros of the sharpened term are exactly the zeros of the unsharpened one
//     (the curve maps the midpoint to the midpoint EXACTLY in integers -- see
//     the static_assert), the extrema are exactly the extrema, and the sign is
//     preserved everywhere. Phase alignment with the 2D banding is therefore
//     preserved by construction, not by luck.
//   * It maps [-A, A] onto [-A, A] with both endpoints exact, so the 500 mm
//     bedding allocation is unchanged and the envelope arithmetic below is
//     untouched.
//   * It is C2 (Perlin's quintic, twice), so it introduces no new
//     discontinuity and no new gradient step.
//
// A flat region in D means dD/dz = 0, which is an exactly VERTICAL face -- on
// a cliff that is a feature, not a terrace. Terracing is flatness in the
// SURFACE HEIGHT field; this is flatness in a vertical displacement, and the
// two are perpendicular.
//
// HOW MANY PASSES -- MEASURED, AND THE TRADE IS REAL. Each pass multiplies the
// contact slope by the quintic's own 1.875. Overhang incidence on 40,000
// idealised columns with both gates fully open:
//
//   passes  contact gain  overhang columns  hardness range retained
//     0        1.00           0.098%              11 : 1
//     1        1.88           0.973%               6 : 1
//     2        3.52           3.737%             3.3 : 1     <-- chosen
//     3        6.59           8.420%             1.9 : 1
//     4       12.36          13.113%             ~1 : 1
//
// The right-hand column is the cost, and it is why this stops at two. The
// curve saturates large values toward the ends, and a bed's peak displacement
// is exactly what carries its hashed hardness -- so sharpening the contact
// also COMPRESSES the difference between a weak bed and a strong one. At three
// passes a bed with a tenth the hardness of another still protrudes more than
// half as far, and the face becomes a uniform square-wave corrugation in which
// every bed is equally resistant. That throws away the differential weathering
// this term exists to express, in exchange for overhangs. Two passes is the
// point where the overhang rate is up 38x and a 3.3:1 spread in bed relief
// survives.
//
// PROVISIONAL, like every other shape parameter here. If a later measurement
// wants more overhangs, three passes is the lever and the flattening above is
// the price to argue about. This arguably belongs in detail_bedding.h next to
// the tent it corrects; it is here because that file is owned by another
// change in flight. Flagged for reconciliation at integration.
inline constexpr int64_t kDensity3SharpenPasses = 2;

// Perlin's quintic 6t^5 - 15t^4 + 10t^3 on [0, T] -> [0, T], for an arbitrary
// domain T. This is detail_rill.h's rillQuinticQ generalised off its hardwired
// q12 domain; the static_assert below proves the two agree there, so this is a
// generalisation and not a second, subtly different curve.
//
// Same factoring as rillQuinticQ, for the same overflow reason: t^3 * inner is
// the quintic itself times T^5 and so never exceeds T^5. That caps T at about
// 6100 for int64; the only caller uses T = 1000.
constexpr int64_t density3QuinticOn(int64_t t, int64_t T) {
    const int64_t inner = 6 * t * t - 15 * t * T + 10 * T * T;
    return floorDiv(t * t * t * inner, T * T * T * T);
}
static_assert(density3QuinticOn(0, kDensity3GateQ) == rillQuinticQ(0) &&
                  density3QuinticOn(997, kDensity3GateQ) == rillQuinticQ(997) &&
                  density3QuinticOn(2048, kDensity3GateQ) == rillQuinticQ(2048) &&
                  density3QuinticOn(3001, kDensity3GateQ) == rillQuinticQ(3001) &&
                  density3QuinticOn(kDensity3GateQ, kDensity3GateQ) ==
                      rillQuinticQ(kDensity3GateQ),
              "density3QuinticOn must be rillQuinticQ generalised, not a different curve");

// Monotone odd contrast curve on [-ampMm, ampMm] -> [-ampMm, ampMm].
constexpr int64_t density3SharpenMm(int64_t rMm, int64_t ampMm) {
    const int64_t T = 2 * ampMm;
    int64_t t = rMm + ampMm; // [0, T]
    t = density3QuinticOn(t, T);
    t = density3QuinticOn(t, T);
    return t - ampMm;
}
static_assert(kDensity3SharpenPasses == 2,
              "density3SharpenMm unrolls its passes explicitly for the HLSL mirror; if the "
              "pass count changes, change the body too");
static_assert(density3SharpenMm(0, kBedding3MaxAbsMm) == 0,
              "the contrast curve must fix zero EXACTLY -- this is what preserves the phase of "
              "the banding, and the alignment with the 2D term rests on it");
static_assert(density3SharpenMm(kBedding3MaxAbsMm, kBedding3MaxAbsMm) == kBedding3MaxAbsMm &&
                  density3SharpenMm(-kBedding3MaxAbsMm, kBedding3MaxAbsMm) == -kBedding3MaxAbsMm,
              "the contrast curve must fix both endpoints exactly, or it changes the budget");
static_assert(density3SharpenMm(1, kBedding3MaxAbsMm) >= 0 &&
                  density3SharpenMm(-1, kBedding3MaxAbsMm) <= 0,
              "the contrast curve must preserve sign");

constexpr int64_t density3BeddingMm(uint64_t seed, int64_t xMm, int64_t yMm, int64_t zMm) {
    return density3SharpenMm(beddingDisplacement3Mm(seed, xMm, yMm, zMm), kBedding3MaxAbsMm);
}

// HOIST 2: the same term with detail_bedding.h's structural-domain hash already
// computed. Equal to density3BeddingMm whenever
// `domainHash == beddingDomainHash(seed, xMm, yMm)`.
constexpr int64_t density3BeddingFromDomainMm(uint64_t seed, uint64_t domainHash, int64_t xMm,
                                              int64_t yMm, int64_t zMm) {
    return density3SharpenMm(beddingDisplacement3FromDomain(seed, domainHash, xMm, yMm, zMm),
                             kBedding3MaxAbsMm);
}

// =============================================================================
// 4. COMPOSITION AND THE BOUND
// =============================================================================
//
// HOW 700 SPLITS.
//
//     bedding   |.| <= kBedding3MaxAbsMm      = 500   (detail_bedding.h's own)
//     pocket    |.| <= kDensity3PocketAmpMm   = 200   (this file, = 700 - 500)
//     sum       |.| <=                          700
//     x slope gate (<= 1) x band taper (<= 1)
//     D         |.| <= kDensity3MaxAbsMm      = 700
//
// The two terms are independently bounded and then SUMMED, so checking each
// against 700 separately would be wrong; the assert below checks the sum,
// which is the only check that means anything. It is written as `==` rather
// than `<=` deliberately: an inequality would let a future edit quietly leave
// budget on the floor, and the point of a fixed envelope is that it is fully
// allocated and every change is a redistribution.
static_assert(kBedding3MaxAbsMm + kDensity3PocketAmpMm == kDensity3MaxAbsMm,
              "the 3D displacement budget must be exactly allocated: bedding + pocket == 700 mm. "
              "Raising one term means lowering the other, or renegotiating "
              "kDensity3MaxAbsMm with every consumer of the widened bounds.");
static_assert(kDensity3PocketAmpMm > 0,
              "detail_bedding.h has consumed the entire envelope; there is no room for the "
              "pocket term and the 700 mm figure needs revisiting, not silently exceeding");

// Applies the two multiplicative gates to the summed displacement.
//
// BOUND. sum is in [-700, 700], each gate in [0, kDensity3GateQ]. The exact
// rational is in [-700, 700] and floorDiv lands in [-700, 700] -- attained
// exactly at both extremes when both gates are fully open, which is what the
// static_asserts below pin. Widest intermediate is 700 * 4096 * 4096 = 1.17e10,
// nine orders inside int64.
constexpr int64_t density3ScaleMm(int64_t sumMm, int64_t slopeGateQ, int64_t bandTaperQ) {
    return floorDiv(sumMm * slopeGateQ * bandTaperQ, kDensity3GateQ * kDensity3GateQ);
}

static_assert(density3ScaleMm(kDensity3MaxAbsMm, kDensity3GateQ, kDensity3GateQ) ==
                  kDensity3MaxAbsMm,
              "a fully open gate must pass the maximum through exactly");
static_assert(density3ScaleMm(-kDensity3MaxAbsMm, kDensity3GateQ, kDensity3GateQ) ==
                  -kDensity3MaxAbsMm,
              "a fully open gate must pass the minimum through exactly");
static_assert(density3ScaleMm(kDensity3MaxAbsMm, 0, kDensity3GateQ) == 0 &&
                  density3ScaleMm(-kDensity3MaxAbsMm, 0, kDensity3GateQ) == 0 &&
                  density3ScaleMm(kDensity3MaxAbsMm, kDensity3GateQ, 0) == 0 &&
                  density3ScaleMm(-kDensity3MaxAbsMm, kDensity3GateQ, 0) == 0,
              "either gate closed must give exactly zero, not a rounding residue -- the exact "
              "skip argument depends on it");
// The taper's own endpoints, checked at compile time so the band edge cannot
// silently stop being zero.
static_assert(density3BandTaperQ(0, 0) == kDensity3GateQ,
              "at the surface the band must be fully open");
static_assert(density3BandTaperQ(kDensity3BandCoreMm, 0) == kDensity3GateQ &&
                  density3BandTaperQ(-kDensity3BandCoreMm, 0) == kDensity3GateQ,
              "the taper must still be fully open at the core edge");
static_assert(density3BandTaperQ(kDensity3BandHalfMm, 0) == 0 &&
                  density3BandTaperQ(-kDensity3BandHalfMm, 0) == 0 &&
                  density3BandTaperQ(kDensity3BandHalfMm + 1, 0) == 0,
              "the taper must be exactly zero at and beyond the band edge");
static_assert(density3SlopeGateQ(kDensity3SlopeStartMmPerM, 0) == 0 &&
                  density3SlopeGateQ(kDensity3SlopeFullMmPerM, 0) == kDensity3GateQ,
              "the slope gate must close fully below its threshold and open fully above it");
static_assert(density3RockGateQ(kDensity3RockSoilFullMm) == kDensity3GateQ &&
                  density3RockGateQ(kDensity3RockSoilZeroMm) == 0,
              "the lithology gate must open fully on bare rock and close fully under soil");

// =============================================================================
// PUBLIC API
// =============================================================================

// The hot-loop entry point: both column gates already computed and hoisted.
//
// Returns the signed displacement D in millimetres for use as
// `solid <=> zMm <= surfaceMm + D`. |D| <= kDensity3MaxAbsMm, always,
// unconditionally, for every seed and every coordinate.
//
// Pure: a function of world millimetres and the seed only. No tile origin, no
// chunk index, no brick-local coordinate appears anywhere in this file's call
// graph, so the same world point gives the same answer regardless of which
// tile, chunk or dispatch computed it. That is the property the whole streamed
// world rests on and it is tested directly.
constexpr int64_t density3DisplacementGatedMm(uint64_t seed, int64_t xMm, int64_t yMm, int64_t zMm,
                                              int64_t surfaceMm, int64_t slopeGateQ,
                                              int64_t rockGateQ) {
    if (slopeGateQ == 0) return 0;
    const int64_t taperQ = density3BandTaperQ(zMm, surfaceMm);
    if (taperQ == 0) return 0; // outside the band: exactly zero, see section 0
    const int64_t sumMm = density3BeddingMm(seed, xMm, yMm, zMm) +
                          density3PocketMm(seed, xMm, yMm, zMm, rockGateQ);
    return density3ScaleMm(sumMm, slopeGateQ, taperQ);
}

// Reference form: computes the column gates itself. Correct but not for the
// hot loop -- see the cost note in section 1.
//
//   gradXMmPerM / gradYMmPerM : the carrier's analytic surface gradient, in
//       the v9 mm-per-metre currency (Amplifier::evalSurface,
//       biome.h's kBiomeCliffSlopeMmPerM units).
//   soilDepthMm : depth of soil above rock for this column; see
//       density3RockGateQ's comment. 0 opens the lithology gate.
constexpr int64_t density3DisplacementMm(uint64_t seed, int64_t xMm, int64_t yMm, int64_t zMm,
                                         int64_t surfaceMm, int64_t gradXMmPerM,
                                         int64_t gradYMmPerM, int64_t soilDepthMm) {
    return density3DisplacementGatedMm(seed, xMm, yMm, zMm, surfaceMm,
                                       density3SlopeGateQ(gradXMmPerM, gradYMmPerM),
                                       density3RockGateQ(soilDepthMm));
}

// The displaced solidity test itself, provided so callers cannot get the
// direction of the inequality wrong. Solid at or below the displaced surface,
// exactly as the undisplaced test reads today.
constexpr bool density3IsSolid(int64_t zMm, int64_t surfaceMm, int64_t displacementMm) {
    return zMm <= surfaceMm + displacementMm;
}

// =============================================================================
// THE COLUMN STATE -- the integration boundary, and both hoists in one object
// =============================================================================
//
// Everything in D that does not depend on z, reduced once per column and
// carried on the ColumnSample exactly the way caves.h's CaveColumn and
// caverns.h's CavernColumn are. That is not a convention borrowed for
// tidiness: Amplifier::stratigraphyAt is a STATIC function of (ColumnSample,
// vz) called by GeneratedWorld, by the UE subsystem and by collapse.h, so
// anything the per-voxel test needs and cannot derive from vz has to arrive on
// the sample. Widening the signature instead would be an API break across four
// consumers, one of them out of this repository.
//
// EVERY FIELD IS BEHIND THE SLOPE GATE. On a column that fails it, this struct
// is left zeroed and NOT ONE hash is computed -- not the domain hash, not the
// eight corners. That is the whole cost argument: ~93% of columns pay two
// compares and a divide (density3SlopeGateQFromMag) and nothing else, and the
// skip is exact because slopeGateQ == 0 forces D == 0 identically.
struct Density3Column {
    uint64_t seed = 0;       // needed by the warp/hardness hashes, which are per-BED
    uint64_t domainHash = 0; // beddingDomainHash(seed, xMm, yMm)   -- hoist 2
    Density3PocketCorners pocket{};                        //       -- hoist 1
    int64_t xMm = 0, yMm = 0;
    int32_t slopeGateQ = 0; // 0 == this column can never displace; see above
    int32_t rockGateQ = 0;
};

// Reduce a column. `slopeMmPerM` is the carrier's analytic gradient MAGNITUDE
// (Amplifier::SurfaceEval::slopeMmPerM); `soilDepthMm` is the depth of soil
// above rock -- see density3RockGateQ's comment for which of topsoil /
// topsoil+subsoil that is. `surfaceMm` picks the z lattice cell the corner
// cache is built for, and is the band's own centre.
constexpr Density3Column density3ColumnFor(uint64_t seed, int64_t xMm, int64_t yMm,
                                           int64_t surfaceMm, int64_t slopeMmPerM,
                                           int64_t soilDepthMm) {
    Density3Column c;
    c.seed = seed;
    c.xMm = xMm;
    c.yMm = yMm;
    const int64_t slopeQ = density3SlopeGateQFromMag(slopeMmPerM);
    if (slopeQ == 0) return c; // exact skip -- no hash is computed on this column
    c.slopeGateQ = static_cast<int32_t>(slopeQ);
    c.rockGateQ = static_cast<int32_t>(density3RockGateQ(soilDepthMm));
    c.domainHash = beddingDomainHash(seed, xMm, yMm);
    // The pocket's eight corners are only worth caching if the pocket can be
    // non-zero at all; a closed lithology gate zeroes it exactly.
    if (c.rockGateQ != 0) c.pocket = density3PocketCornersAt(seed, xMm, yMm, surfaceMm);
    return c;
}

// The per-voxel query. `zMm` is the VOXEL CENTRE in world millimetres and
// `surfaceMm` the column's undisplaced surface. Equal, for every input, to
// density3DisplacementGatedMm(c.seed, c.xMm, c.yMm, zMm, surfaceMm,
// c.slopeGateQ, c.rockGateQ) -- pinned by test_density3.cpp.
constexpr int64_t density3ColumnDisplacementMm(const Density3Column& c, int64_t zMm,
                                               int64_t surfaceMm) {
#ifdef VXC_DENSITY3_NO_HOIST
    // MEASUREMENT BUILD ONLY, and safe to have: this is the same answer with
    // both hoists switched off, so what the hoists are worth can be measured on
    // one machine and one compiler instead of argued from a cost model. It is
    // also EXACTLY what worldgen.ush computes, which makes it a second, cruder
    // check on the mirror -- vxc_bench --digest must be identical between a
    // hoisted and an unhoisted build, and if it is not, the CPU and the GPU
    // disagree too.
    //
    // Deliberately the only ablation switch in this file. A switch that turned
    // the TERM off would produce v11 geometry under a v12 version stamp, which
    // is a desync waiting for someone to leave a flag set; this one cannot
    // change an output at all.
    return density3DisplacementGatedMm(c.seed, c.xMm, c.yMm, zMm, surfaceMm, c.slopeGateQ,
                                       c.rockGateQ);
#else
    if (c.slopeGateQ == 0) return 0;
    const int64_t taperQ = density3BandTaperQ(zMm, surfaceMm);
    if (taperQ == 0) return 0; // outside the band: exactly zero, see section 0
    const int64_t sumMm =
        density3BeddingFromDomainMm(c.seed, c.domainHash, c.xMm, c.yMm, zMm) +
        density3PocketCachedMm(c.pocket, c.seed, c.xMm, c.yMm, zMm, c.rockGateQ);
    return density3ScaleMm(sumMm, c.slopeGateQ, taperQ);
#endif
}

// How far, in VOXELS, this column's top solid voxel can move, and equivalently
// how far below its nominal top the bricks can stop being homogeneous. Zero on a
// column whose slope gate is shut, which is the point: a brick-range widening
// applied unconditionally is sound but is paid by flat ground that can never use
// it. Callers: GeneratedWorld::surfaceBrickRange and its coarse sibling, and the
// gpu_harness mirror of the same rule.
constexpr int64_t density3BandVoxels(const Density3Column& c) {
    static_assert(kDensity3MaxAbsMm % kVoxelSizeMm == 0,
                  "the envelope must be a whole number of voxels, or every consumer of this "
                  "needs a rounding argument it does not currently have");
    return c.slopeGateQ == 0 ? 0 : kDensity3MaxAbsMm / kVoxelSizeMm;
}

// The cheap per-voxel predicate a caller should apply BEFORE
// density3ColumnDisplacementMm: true iff this voxel could possibly be
// displaced. Two compares on a column that failed the slope gate, which is
// most of them.
constexpr bool density3ColumnCanDisplace(const Density3Column& c, int64_t zMm,
                                         int64_t surfaceMm) {
    return c.slopeGateQ != 0 && density3BandGateOpen(zMm, surfaceMm);
}

} // namespace vxc
