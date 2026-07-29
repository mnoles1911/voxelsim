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
// `z <= surface + D`, with a compile-time-provable envelope |D| <= 350 mm.
//
// WIRED IN AT kWorldGenVersion 12 (Wave C). `Amplifier::stratigraphyAt` now
// tests `centre <= surface + D`, with the column-invariant half of D hoisted
// into `ColumnSample::d3` (see Density3Column at the bottom of this file) the
// same way the cave and cavern passes hoist theirs. That bump carried the
// bound widening this header's section 0 asks for -- surfaceUpperBoundMm,
// surfaceLowerBoundMm and solidBelowBoundMm all moved by kDensity3MaxAbsMm,
// GeneratedWorld's surface brick range widened PER COLUMN by the band (zero
// where the gates are shut, which is most ground), and amplifier.h's air-reason
// enumeration gained its fourth reason.
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
// This file now allocates NO channel of its own. It claimed CH_POCKET = 29 for
// the pocket term; that term was removed at kWorldGenVersion 12 (see section 2)
// and 29 went back to the free list. Survey of the whole tree at time of
// writing: hash.h 0..15 (detail octaves), 16, 17, 18, 19, 32..47 (synthetic
// tiles); caves.h 18, 19, 20, 21, 24; caverns.h 22, 23, 25; detail_rill.h 26;
// detail_bedding.h 27, 28. So 29, 30 and 31 are free.
//
// Everything this file hashes, it hashes through detail_bedding.h's channels,
// which is not an accident of implementation -- it is the design. See section 3.
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

// =============================================================================
// 0. THE ENVELOPE, AND WHY THE BAND HALF-WIDTH EQUALS IT
// =============================================================================
//
// The plan fixed |D| <= 700 mm; this ships 350. That single number does three
// jobs:
//
//   1. It is the amount every surface-derived bound must widen by (brick
//      ranges, the air-reason enumeration at amplifier.h:167-196).
//   2. It is the half-width of the band in which D may be non-zero.
//   3. It is therefore what makes the skip EXACT rather than approximate.
//
// AND IT IS THE COST. Job (2) is why: the per-voxel work is proportional to how
// many voxels fall inside the band, so the envelope is not a quality knob with a
// cost side effect, it is the cost, linearly. 700 mm was measured end to end and
// rejected -- 2.6x world-average voxelisation on real diffusion tiles and +13%
// bricks, for an overhang on 0.22% of all columns. 350 halves the band and, with
// the two changes below (the pocket term deleted, the contrast curve at three
// passes), keeps two thirds of the overhang rate. The full before/after is in
// the Wave C retry notes; what belongs HERE is that this constant is the dial
// and that moving it moves cost linearly and overhang rate FASTER than
// linearly -- see kDensity3SharpenPasses.
//
// (2) and (3) are the same fact, and it is worth writing the argument out
// because "we skip far-from-surface voxels" usually means an approximation and
// here it does not. Let dz = z - surface.
//
//   * If dz > 350: since D <= 350 <= dz, `dz <= D` is false. The voxel is air
//     whether or not D was computed. Skipping gives the identical answer.
//   * If dz < -350: since D >= -350 >= dz, `dz <= D` is true. The voxel is
//     solid whether or not D was computed. Skipping gives the identical answer.
//
// There is no epsilon and no "visually indistinguishable" in that argument.
// Halving the envelope would halve the band and halve the cost, and widening
// it widens both -- band half-width is not a free tuning knob, it is forced
// equal to the envelope.
inline constexpr int64_t kDensity3MaxAbsMm = 350;
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
// WHY THIS TERM IS GATED AT ALL. An ungated volumetric displacement roughly
// DOUBLES VoxelizeMain everywhere -- it turns a stratigraphy lookup that is a
// handful of integer ops into one that hashes per voxel. Gated, the expensive
// part runs only where it can change an answer.
//
// There are TWO gates and they are combined into ONE per column:
//
//     per column, once:   density3ColumnFor(...)
//                           slope gate  -- 2 compares, 1 divide
//                           x lithology gate -- 2 compares, 1 divide
//                           if the product is 0, the whole column is skipped
//                           and not one hash is computed
//     per voxel:          density3BandGateOpen(z, s)   -- 1 subtract, 1 compare
//                         if false, D is exactly 0 -- skip
//     only then:          the hashes
//
// Hoisting the column gate out of the z loop is the entire cost argument.
// density3DisplacementMm, which computes the gates itself, exists for tests and
// for the reference/HLSL mirror, not for the hot loop.
//
// ---------------------------------------------------------------------------
// MEASURED COST -- AND WHAT THE FIRST ESTIMATE HERE GOT WRONG
// ---------------------------------------------------------------------------
// This block used to carry an estimate. It has been replaced by an end-to-end
// measurement of the term actually wired in (vxc_bench, three builds differing
// only in this header, same brick set), because the estimate was wrong in a way
// worth keeping on the record.
//
// WHAT IT GOT RIGHT: the per-voxel cost. It predicted ~100 ns per gated voxel
// unhoisted and ~46 ns hoisted; measured 83 ns and 55 ns at the 700 mm
// envelope. The cost model was sound.
//
// WHAT IT GOT WRONG, BY 3-5x: the GATE RATE, which is the multiplier on all of
// it. It estimated the slope gate open on 6.9% of columns from a small sample
// of SyntheticTileSampler -- and flagged that number as the one to distrust,
// correctly. Measured since, with a census over 25 REAL diffusion tiles
// (76.8 x 76.8 km, vxc_climateprobe): 11.95% of columns, and 30.6% on the
// synthetic sampler the estimate came from. At the 700 mm envelope that made
// voxelisation 2.6x world-average on real tiles and 4-6x on cliff ground,
// against the plan's "+50-80% on a cliff chunk". The plan's cost figure was not
// off by a tuning margin; it was off by an order of magnitude.
//
// THE LESSON, which is why this is stated rather than just corrected: for a
// per-voxel gated term the gate rate is not a detail of the cost model, it IS
// the cost model, and it cannot be estimated off the synthetic sampler because
// that sampler's slope distribution is nothing like a diffusion tile's. Measure
// it on real tiles before costing anything else in this band.
//
// AT THE SHIPPED 350 mm ENVELOPE, per gated voxel (hoisted), the composition is
// one bedding evaluation plus three quintic passes -- 4 hash2 after the domain
// hoist, no hash3 at all -- and the band covers half as many voxels.
//
// TWO HOISTS, BOTH DONE, both VALUE-IDENTICAL rearrangements (hash2 is pure, so
// a hoisted evaluation and a fresh one return the same integer -- which is what
// lets worldgen.ush mirror the UNHOISTED form and still be bit-exact):
//
//   1. beddingRawAt called beddingDomainHash three times for one hash --
//      beddingStrikeIndexAt, beddingDipQ10At and beddingThicknessMmAt each
//      recomputed it -- and all three are constant over an 819.2 m structural
//      domain. detail_bedding.h grew beddingRawFromDomain for it.
//   2. The pocket term's eight hash3 corners were constant across the band on
//      78% of columns and were cached per column. That hoist is GONE because
//      the term it optimised is gone (section 2) -- which is the better version
//      of the same saving.

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
// largest dz satisfying dz <= taper(dz)/Q * envelope, which for the shipped
// 200/350 split is 255 mm (pinned by test_density3.cpp). Widening the core
// raises that toward the envelope at the price of a steeper taper; narrowing it
// makes the term gentler and the overhangs smaller.
//
// HELD AT THE SAME FRACTION OF THE ENVELOPE (4/7) WHEN THE ENVELOPE HALVED, and
// that was checked rather than assumed: swept over 150/200/250/300 at a 350 mm
// envelope, the overhang rate moves 0.96% / 1.15% / 1.33% / 1.35% of columns --
// monotone, shallow, and saturating past 250. So the core is not where the
// overhang rate is won or lost (the contrast curve is), and holding the ratio
// keeps the taper's shape identical to the one every continuity test in
// test_density3.cpp was written against.
//
// The reachable displacement is now ~+/-0.25 m -- about 0.5 m of peak-to-trough
// relief across a face, and about 2.5 voxels. That is still a ledge the mesher
// can express; it is not the 5 voxels the 700 mm envelope bought.
inline constexpr int64_t kDensity3BandCoreMm = 200;
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

// --- gate (c): lithology ----------------------------------------------------
//
// Undercutting is a bare-rock phenomenon. On a soil-mantled slope the rock is
// buried and the surface is regolith, so the displacement must fade out as the
// column's soil thickens.
//
// THIS GATE USED TO APPLY ONLY TO THE POCKET TERM. When that term was deleted
// (section 2) the gate could have gone with it; instead it was promoted to the
// WHOLE displacement, and that is a deliberate widening, not salvage:
//
//   * it is at least as physical. The pocket's justification was "joints are
//     buried under regolith". A differentially weathered bedding CONTACT is
//     buried by exactly the same regolith, and a weak bed cannot weather back
//     out of a face that has no exposed face. If anything the argument is
//     stronger for bedding, because bedding relief is produced BY exposure.
//   * it is free and it pays. It is already computed per column, and closing it
//     skips the column entirely. On the 25 real diffusion tiles it takes the
//     gated share from 11.95% of columns to 6.52% -- a 45% cut in the term's
//     entire cost, on ground where an overhang would have been wrong anyway.
//   * it restores a guarantee. With the band live on soil columns, the top
//     solid voxel of an undercut nose could be a cut ACROSS the soil profile
//     and read MAT_SUBSOIL instead of the biome material -- measured at 0.48%
//     of columns, and a real weakening of the invariant
//     test_amplifier.cpp's amplifier_top_voxel_is_surface_material defends.
//     Restricted to rock columns the exception cannot arise at all, because
//     stratigraphyAt reads MAT_ROCK straight down under a BARE_ROCK surface.
//
// WHAT IT ACTUALLY SELECTS, and this is worth knowing because it is sharper
// than the ramp suggests: amplifier.cpp's soilAboveRockMm passes topsoilMm on a
// MAT_ROCK column and topsoilMm + subsoilMm otherwise, and subsoil is
// 2*topsoil + 500 with topsoil floored at kTopsoilMinMm = 100. So a non-rock
// column carries AT LEAST 100 + 700 = 800 mm, which is exactly
// kDensity3RockSoilZeroMm. The gate is therefore closed on every non-rock
// column and open on rock cliffs (where slope retention pins topsoil at its
// 100 mm floor). It is a continuous ramp that in practice resolves to "bare
// rock only" -- and the static_assert in amplifier.cpp pins that coupling so it
// cannot drift into a knife-edge silently.
//
// This is a COLUMN property, not a per-voxel depth ramp, and getting that
// wrong is a trap worth recording. The obvious implementation -- "zero the
// displacement until you are below the soil, then ramp it in" -- makes the term
// completely inert. D only changes an answer where it can flip `dz <= D`, and
// that happens near dz = 0; a term forced to zero in a neighbourhood of dz = 0
// carves nothing at all (at dz = -400 with D = -200, `-400 <= -200` is still
// true, so the voxel is still solid, and nothing was removed). So the gate is
// on the column's soil THICKNESS, evaluated once, and D keeps its full profile
// through dz = 0 where it can actually cut.
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
// (kTopsoilMinMm = 100 mm), so on real rock cliffs this gate is open.
inline constexpr int64_t kDensity3RockSoilFullMm = 200; // <= this: bare rock, full displacement
inline constexpr int64_t kDensity3RockSoilZeroMm = 800; // >= this: mantled, none at all
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
// 2. THE POCKET TERM -- DELETED AT kWorldGenVersion 12, AND WHY
// =============================================================================
//
// The plan specifies two contributors to D: this file's 3D bedding term and "a
// rock-gated valueNoise3 pocket term" for joint-controlled chimneys and flutes.
// The pocket term was implemented at 200 mm of the 700 mm envelope, wired in,
// measured, and removed. Its removal is recorded here rather than in a commit
// message because the measurement generalises to anything else anyone is
// tempted to add to this band.
//
// (1) IT PRODUCED NO OVERHANGS. NOT FEW -- NONE. An overhang exists exactly
//     where dD/dz > 1. Over 400 columns x the whole band sampled at 1 mm, the
//     pocket term alone reached dD/dz > 1 on 0.000% of samples, with a maximum
//     gradient of 1 mm per mm. Its overhang rate on 20,000 columns was 0.000%.
//     That is not a tuning failure, it is arithmetic: the z lattice is 6400 mm
//     -- deliberately 4x the horizontal one, because joints are near-vertical
//     and the term wants vertical flutes -- so a 200 mm amplitude gives a
//     vertical gradient of about 0.06. It was never going to double back. The
//     very property that made it the right SHAPE for joints made it incapable
//     of the geometry this band exists for.
//
// (2) IT WAS THE DOMINANT COST. 43 ns of the 100 ns per gated voxel, and eight
//     of the fifteen hashes -- the only hash3 in the composition. Removing it
//     removes more per-voxel work than every other saving in this file
//     combined, and it deletes the corner cache that existed only to make it
//     affordable.
//
// (3) INSIDE A 350 mm ENVELOPE IT IS BELOW THE RESOLUTION FLOOR. A proportional
//     share is 50-100 mm, i.e. HALF TO ONE VOXEL. Measured on what it actually
//     changes -- voxel solidity decisions with the term on versus off:
//
//       amplitude   cells flipped   of which ISOLATED single voxels   longest run
//         200 mm       3.70%                  70%                     8 voxels
//         100 mm       2.99%                  95%                     4 voxels
//          70 mm       1.75%                  97%                     2 voxels
//          50 mm       1.16%                  97%                     2 voxels
//
//     At any allocation a 350 mm envelope could give it, 97% of what it does is
//     flip a single isolated voxel. That is not a flute; it is per-voxel
//     speckle, and amplifier.cpp's octave table records the same artifact
//     twice, in-engine, as the thing that makes ground read as static rather
//     than as terrain ("the eye takes dense uncorrelated jitter as noise").
//     Even at the shipped 200 mm it was 70% isolated flips.
//
// SO THE WHOLE ENVELOPE GOES TO THE BEDDING TERM. That is not "the leftover
// budget": handing the pocket's 200 mm to the bedding term at the 700 mm
// envelope took the overhang rate from 2.9% to 5.7% of columns -- the same cost
// minus eight hashes, for double the geometry.
//
// WHAT IS LOST is the joint-controlled surface texture on rock faces. That is a
// real loss, and it is a TEXTURE loss, not a structure loss -- the band still
// has the rill term and the microrelief octaves working on the same faces at
// the same scales. If the envelope ever grows back past ~600 mm, so that a
// pocket allocation is two or more voxels again, this is the section to reverse
// and git history has the code.
//
// density3ValueNoise3Fade below is KEPT. It is a general primitive hash.h does
// not have, it is independently tested, and it is the restoration path.
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

// The pocket lattice sizes, retained next to the primitive they parameterised:
// 1.6 m across and 6.4 m vertically, the 4:1 vertical anisotropy that made the
// term the right shape for near-vertical joints and, as section 2 records, the
// exact reason it could never produce an overhang. Kept so the restoration path
// is a whole one and so the 4:1 fact stays attached to its consequence.
inline constexpr int64_t kDensity3PocketLatXYMm = 1600;
inline constexpr int64_t kDensity3PocketLatZMm = 6400; // 4:1 vertical -- joints are near-vertical

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
// contact slope by the quintic's own 1.875 and costs about 2.6 ns.
//
// THE ENVELOPE FORCED THIS FROM TWO TO THREE, and the reason is the whole
// argument for why an overhang term does not scale gently. dD/dz > 1 is a
// THRESHOLD, not a magnitude: halving the amplitude does not halve the overhang
// rate, it moves a whole population of contacts from just above the threshold to
// just below it. Measured on 20,000 columns with both gates open, the
// bedding-only composition at the amplitude each envelope allows:
//
//   passes   contrast retained   overhangs @700mm   overhangs @350mm
//     2          2.9 : 1              5.71%              0.54%
//     3          1.7 : 1                 --              1.97%   <-- chosen
//     4          1.1 : 1                 --              3.57%
//     5          1.0 : 1                 --              4.82%
//
// Halving the envelope at two passes cost 91% of the overhang rate. Three passes
// buys 3.6x of it back for 2.6 ns and takes the retained bed contrast from
// 2.9 : 1 to 1.7 : 1 -- i.e. a bed with a TENTH of another's hardness still
// stands 59% as proud, where at two passes it stood 34% as proud.
//
// WHY IT STOPS AT THREE AND NOT FOUR, which would have restored the rate
// outright. The curve saturates large values, and a bed's peak displacement is
// exactly what carries its hashed hardness, so sharpening the contact
// COMPRESSES the difference between a weak bed and a strong one. At four passes
// the contrast is 1.1 : 1: a tenth-hardness bed stands 88% as proud as a full
// one, and the face becomes a uniform square-wave corrugation in which every bed
// is equally resistant. That is not a cheaper version of this term, it is a
// different and worse one -- it throws away the differential weathering the term
// exists to express in exchange for the overhangs the term exists to produce.
// Three is where both survive.
//
// PROVISIONAL, like every other shape parameter here. This arguably belongs in
// detail_bedding.h next to the tent it corrects; it is here because that file
// was owned by another change in flight. Flagged for reconciliation.
inline constexpr int64_t kDensity3SharpenPasses = 3;

// Perlin's quintic 6t^5 - 15t^4 + 10t^3 on [0, T] -> [0, T], for an arbitrary
// domain T. This is detail_rill.h's rillQuinticQ generalised off its hardwired
// q12 domain; the static_assert below proves the two agree there, so this is a
// generalisation and not a second, subtly different curve.
//
// Same factoring as rillQuinticQ, for the same overflow reason: t^3 * inner is
// the quintic itself times T^5 and so never exceeds T^5. That caps T at about
// 6100 for int64; the callers use T = 2 * kBedding3MaxAbsMm (700) and
// T = kDensity3GateQ (4096), both comfortably inside it.
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
    t = density3QuinticOn(t, T);
    return t - ampMm;
}
static_assert(kDensity3SharpenPasses == 3,
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
// HOW 350 SPLITS. It does not: there is one term.
//
//     bedding   |.| <= kBedding3MaxAbsMm      = 350   (detail_bedding.h's own)
//     x column gate (<= 1) x band taper (<= 1)
//     D         |.| <= kDensity3MaxAbsMm      = 350
//
// Written as `==` rather than `<=` deliberately: an inequality would let a
// future edit quietly leave budget on the floor, and the point of a fixed
// envelope is that it is fully allocated. If a second contributor is ever added
// back, this assert is what forces its allocation to come OUT of the bedding
// term rather than on top of it -- which is the conversation section 2 says to
// have before adding anything to this band.
static_assert(kBedding3MaxAbsMm == kDensity3MaxAbsMm,
              "the 3D displacement budget must be exactly allocated. The pocket term was "
              "removed at kWorldGenVersion 12 (see section 2) and the bedding term inherited "
              "its share; adding any second contributor means taking amplitude off the "
              "bedding term or renegotiating kDensity3MaxAbsMm with every consumer of the "
              "widened bounds.");

// Applies the two multiplicative gates to the displacement.
//
// BOUND. sum is in [-350, 350], each gate in [0, kDensity3GateQ]. The exact
// rational is in [-350, 350] and floorDiv lands in [-350, 350] -- attained
// exactly at both extremes when both gates are fully open, which is what the
// static_asserts below pin. Widest intermediate is 350 * 4096 * 4096 = 5.9e9,
// nine orders inside int64.
//
// `columnGateQ` is the slope and lithology gates already multiplied together --
// see density3ColumnGateQ. They were two arguments when the lithology gate
// applied only to the pocket term; now that both gate the whole displacement,
// combining them per column is one multiply saved per voxel and one fewer q12
// truncation to keep in step across the HLSL mirror.
constexpr int64_t density3ScaleMm(int64_t sumMm, int64_t columnGateQ, int64_t bandTaperQ) {
    return floorDiv(sumMm * columnGateQ * bandTaperQ, kDensity3GateQ * kDensity3GateQ);
}

// The two column gates, combined once per column. Exactly zero iff either is
// zero, and exactly kDensity3GateQ iff both are fully open -- both pinned
// below, because the exact-skip argument rests on the first and the "a fully
// open gate passes the maximum through exactly" bound rests on the second.
constexpr int64_t density3ColumnGateQ(int64_t slopeGateQ, int64_t rockGateQ) {
    return floorDiv(slopeGateQ * rockGateQ, kDensity3GateQ);
}
static_assert(density3ColumnGateQ(kDensity3GateQ, kDensity3GateQ) == kDensity3GateQ,
              "two fully open gates must combine to a fully open gate");
static_assert(density3ColumnGateQ(0, kDensity3GateQ) == 0 &&
                  density3ColumnGateQ(kDensity3GateQ, 0) == 0,
              "either gate closed must close the combined gate EXACTLY -- the exact-skip "
              "argument and the whole cost model rest on this being 0 and not 1");

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
                                              int64_t surfaceMm, int64_t columnGateQ) {
    if (columnGateQ == 0) return 0;
    const int64_t taperQ = density3BandTaperQ(zMm, surfaceMm);
    if (taperQ == 0) return 0; // outside the band: exactly zero, see section 0
    return density3ScaleMm(density3BeddingMm(seed, xMm, yMm, zMm), columnGateQ, taperQ);
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
    return density3DisplacementGatedMm(
        seed, xMm, yMm, zMm, surfaceMm,
        density3ColumnGateQ(density3SlopeGateQ(gradXMmPerM, gradYMmPerM),
                            density3RockGateQ(soilDepthMm)));
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
// EVERY FIELD IS BEHIND THE COLUMN GATE. On a column that fails it, this struct
// is left zeroed and NOT ONE hash is computed. That is the whole cost argument:
// the columns that fail pay two compares and two divides
// (density3SlopeGateQFromMag, density3RockGateQ) and nothing else, and the skip
// is exact because gateQ == 0 forces D == 0 identically. On the 25 real
// diffusion tiles that is 93.5% of columns.
struct Density3Column {
    uint64_t seed = 0;       // needed by the warp/hardness hashes, which are per-BED
    uint64_t domainHash = 0; // beddingDomainHash(seed, xMm, yMm)   -- the one hoist left
    int64_t xMm = 0, yMm = 0;
    int32_t gateQ = 0; // slope x lithology, q12. 0 == this column can never displace
};

// Reduce a column. `slopeMmPerM` is the carrier's analytic gradient MAGNITUDE
// (Amplifier::SurfaceEval::slopeMmPerM); `soilDepthMm` is the depth of soil
// above rock -- see density3RockGateQ's comment for which of topsoil /
// topsoil+subsoil that is.
//
// The gates are evaluated CHEAPEST FIRST and short-circuit: the slope gate is
// two compares on a value the caller already has, and it rejects most ground
// before the lithology gate is looked at, which rejects most of the rest before
// the single hash is computed.
constexpr Density3Column density3ColumnFor(uint64_t seed, int64_t xMm, int64_t yMm,
                                           int64_t slopeMmPerM, int64_t soilDepthMm) {
    Density3Column c;
    c.seed = seed;
    c.xMm = xMm;
    c.yMm = yMm;
    const int64_t slopeQ = density3SlopeGateQFromMag(slopeMmPerM);
    if (slopeQ == 0) return c; // exact skip -- no hash is computed on this column
    const int64_t gateQ = density3ColumnGateQ(slopeQ, density3RockGateQ(soilDepthMm));
    if (gateQ == 0) return c; // soil-mantled: same exact skip
    c.gateQ = static_cast<int32_t>(gateQ);
    c.domainHash = beddingDomainHash(seed, xMm, yMm);
    return c;
}

// The per-voxel query. `zMm` is the VOXEL CENTRE in world millimetres and
// `surfaceMm` the column's undisplaced surface. Equal, for every input, to
// density3DisplacementGatedMm(c.seed, c.xMm, c.yMm, zMm, surfaceMm, c.gateQ) --
// pinned by test_density3.cpp.
constexpr int64_t density3ColumnDisplacementMm(const Density3Column& c, int64_t zMm,
                                               int64_t surfaceMm) {
#ifdef VXC_DENSITY3_NO_HOIST
    // MEASUREMENT BUILD ONLY, and safe to have: this is the same answer with the
    // domain hoist switched off, so what the hoist is worth can be measured on
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
    return density3DisplacementGatedMm(c.seed, c.xMm, c.yMm, zMm, surfaceMm, c.gateQ);
#else
    if (c.gateQ == 0) return 0;
    const int64_t taperQ = density3BandTaperQ(zMm, surfaceMm);
    if (taperQ == 0) return 0; // outside the band: exactly zero, see section 0
    return density3ScaleMm(density3BeddingFromDomainMm(c.seed, c.domainHash, c.xMm, c.yMm, zMm),
                           c.gateQ, taperQ);
#endif
}

// How far, in VOXELS, this column's top solid voxel can move, and equivalently
// how far below its nominal top the bricks can stop being homogeneous. Zero on a
// column whose column gate is shut, which is the point: a brick-range widening
// applied unconditionally is sound but is paid by flat ground that can never use
// it. Callers: GeneratedWorld::surfaceBrickRange and its coarse sibling, and the
// gpu_harness mirror of the same rule.
//
// ROUNDED UP, AND THE ENVELOPE IS DELIBERATELY NOT A WHOLE VOXEL. At 700 mm this
// was an exact 7 and the division was written as one; at 350 mm it is 3.5, and
// the honest answer is 4 rather than a rounder envelope chosen to make the
// arithmetic tidy. The reason is phase: a voxel centre sits at vz*100 + 50, so
// whether a 350 mm displacement moves the top voxel by three indices or by four
// depends on where the surface falls between two centres. Three would be right
// most of the time and wrong some of the time, and "wrong some of the time"
// here means a brick that is not homogeneous being treated as if it were --
// i.e. an overhang's underside never meshed, or worse, a chunk skipped at
// admission. Rounding up costs one brick's worth of z range on gated columns
// only.
constexpr int64_t density3BandVoxels(const Density3Column& c) {
    // ceil, on operands that are provably positive; written out rather than via
    // a helper because core.h's ceilDivPos is private to amplifier.cpp.
    constexpr int64_t kVox = (kDensity3MaxAbsMm + kVoxelSizeMm - 1) / kVoxelSizeMm;
    static_assert(kVox * kVoxelSizeMm >= kDensity3MaxAbsMm,
                  "the band must round UP in voxels, or a displaced top voxel can fall outside "
                  "the brick range that was supposed to contain it");
    return c.gateQ == 0 ? 0 : kVox;
}

// The cheap per-voxel predicate a caller should apply BEFORE
// density3ColumnDisplacementMm: true iff this voxel could possibly be
// displaced. Two compares on a column that failed the slope gate, which is
// most of them.
constexpr bool density3ColumnCanDisplace(const Density3Column& c, int64_t zMm,
                                         int64_t surfaceMm) {
    return c.gateQ != 0 && density3BandGateOpen(zMm, surfaceMm);
}

} // namespace vxc
