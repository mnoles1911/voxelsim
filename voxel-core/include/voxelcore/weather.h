#pragma once
// ============================================================================
// THE WEATHER FIELD, v0 -- WIND ONLY (docs/weather-system-v0.md).
// ============================================================================
//
// WHAT THIS IS, IN ONE SENTENCE. Given the world seed, a place and a moment,
// this file says which way the air is moving there and how fast, and it gives
// the same answer every time on every machine.
//
// WHY THE FILE IS CALLED weather.h AND NOT wind.h. Wind is the only quantity
// in it today. It is named for what it becomes because
// docs/lighting-weather-plan.md 5.1 already specifies the rest -- cloud cover,
// precipitation intensity, humidity, fog density, a snow/rain decision from
// temperature -- as OUTPUTS OF THE SAME FIELD SAMPLE, sharing the same
// advected pattern and the same clock. Renaming a header that a UE module, a
// bench probe and a test all include is pure churn, and the same "do it from
// the start" argument VoxelSkySubsystem.h:150-155 makes for its PImpl.
//
// ----------------------------------------------------------------------------
// WHY THIS LIVES IN voxel-core AT ALL, GIVEN THAT WIND IS PRESENTATION
// ----------------------------------------------------------------------------
//
// The sky is client-side rendering and lives in ue-project on purpose
// (VoxelSkySubsystem.h:18-24, VoxelGI.h:26-30). Wind is the same category of
// thing, so the obvious home was beside it. Three arguments moved it here, and
// the third is the one that decided it:
//
//   1. IT CAN BE INSPECTED WITHOUT THE EDITOR. voxel-core has a bench harness
//      and a test harness that run in seconds from a command line;
//      ue-project has neither. A wind field nobody can print is a wind field
//      nobody can argue about, and this project's whole method is to settle
//      questions on measurements rather than on impressions.
//   2. THE BAKE MAY WANT IT LATER. terrain-service already carries an
//      orographic rain-shadow wind (diffusion.py:553, oro_wind_from_deg=270),
//      as a single fixed global bearing in an unverified coordinate frame
//      (docs/world-generation-architecture.md:379-400 refuses to say which way
//      its dry side faces). If the runtime wind and the bake's wind are ever
//      to be reconciled, they have to be able to reach the same function.
//   3. INTEGER MATH MAKES THE DETERMINISM GATE FREE. docs/lighting-weather-
//      plan.md's Part B gate is "two clients at the same (seed, time,
//      position) sample identical weather". In integers that is true by
//      construction, on every compiler, at every optimisation level, forever.
//      In floating point it is a promise that has to be re-tested whenever
//      anything changes, and this project has been bitten by exactly that
//      class of drift before.
//
// The cost of living here is the FLOAT BAN (.github/workflows/ci.yml:77-104:
// no `float`/`double` token in voxel-core/include or /src). Everything below
// is integer fixed point, and the two places that would normally reach for
// trigonometry -- turning a compass bearing into a vector, and interpolating
// smoothly -- are solved the way this codebase already solves them, with a
// small compile-time table (detail_rill.h:191-198) and the quintic fade
// (hash.h:158-212).
//
// ----------------------------------------------------------------------------
// THE MODEL: FOUR BANDS, EACH ONE A THING A PLAYER CAN NAME
// ----------------------------------------------------------------------------
//
// Wind is built as a DIRECTION plus a SPEED rather than as two velocity
// components, because every knob anyone will ever want to turn is a knob on
// one or the other ("make it windier", "the wind should come off the sea").
// Four bands contribute, at four timescales, and each is separately nameable:
//
//   REGIME   ~3 h of clock, +/-180 deg of bearing, no dependence on place.
//            "This week the wind is out of the north." The band that lets the
//            wind visit the whole compass rather than fidgeting about a fixed
//            quarter. Time only: a regime that differed between two ends of
//            the map would not be a regime.
//
//   PREVAIL  ~20 min of clock, +/-55 deg of bearing, no dependence on place.
//            "It has backed round to the west this afternoon." Time only, same
//            reason.
//
//   SYNOPTIC ~2 km of ground and ~10 min of clock. Carries BOTH a bearing
//            perturbation (+/-25 deg) and the speed's whole variation
//            (x0.35 .. x1.65). This is the band that answers the actual
//            requirement -- different parts of the map having different wind
//            at the same moment -- and 2 km is not a free choice: it is the
//            cell size docs/lighting-weather-plan.md:400 already fixed, so
//            that a front takes minutes of play to cross the visible world.
//            THIS BAND ADVECTS (see below), which is what makes weather arrive
//            from upwind instead of fading in on the spot.
//
//   GUST     ~180 m of ground and ~6 s of clock. +/-9 deg of bearing and a
//            speed swing of +/-25% OF THE SUSTAINED SPEED -- a fraction, not
//            an absolute, so a calm day has small gusts and a rough one has
//            large ones, which is both the real behaviour and the only way a
//            pinned dead-calm wind can be genuinely dead calm. Does NOT
//            advect: a gust is a local eddy, not a system passing through.
//
// FOUR NAMED BANDS RATHER THAN N ANONYMOUS OCTAVES. A sum of fBm octaves is
// the standard cheap answer and would produce a similar-looking field, but
// every octave in it is a number with no meaning, so "the wind changes too
// fast" has no single knob and no single owner. Here it does.
//
// ----------------------------------------------------------------------------
// ADVECTION, AND THE BUG THAT WAS FOUND WRITING IT
// ----------------------------------------------------------------------------
//
// The synoptic pattern is sampled at a point that slides upwind as the clock
// runs, so a weather cell approaches you from the direction the wind is coming
// from. docs/lighting-weather-plan.md:393 writes this as
// `pressureNoise(x - windU*t, y - windV*t, t*eps)` with windU/windV coming
// from a slow noise -- i.e. with a VARYING advection direction.
//
// THAT FORMULA HAS A LEVER-ARM AMPLIFIER IN IT AND MUST NOT BE IMPLEMENTED AS
// WRITTEN. `direction(t) * speed * t` is not the path the air took; it is the
// whole journey re-aimed along wherever the wind happens to point right now.
// After a day of play the journey is about 1000 km long, so an entirely
// ordinary wobble in the prevailing bearing -- 0.07 degrees between one frame
// and the next -- swings the far end of that lever by 1.2 km, which is over
// half a synoptic cell. Measured, not guessed: at seed 20260719 and
// t = 84,895,298 ms the wind's bearing jumped 20.4 degrees in one 16 ms frame,
// and the speed with it. It reads as the weather teleporting.
//
// So THE ADVECTION DIRECTION IS THE FIXED BASE BEARING (WindParams::
// baseFromBearingMilliDeg), which has no wobble to amplify. The honest
// description of what that costs: weather systems always travel the same way
// even when the surface wind has backed round, which is wrong in detail and
// invisible in practice, because you cannot see the direction a pattern
// arrives from when the pattern is 2 km across and you can see 4. The correct
// fix -- integrating the velocity along the path -- needs either accumulated
// state (which would stop this being a pure function, and the pure function is
// the whole determinism argument) or an analytic integral of the noise. Both
// are v1 problems. See docs/weather-system-v0.md.
//
// ----------------------------------------------------------------------------
// WHAT THIS DELIBERATELY DOES NOT DO IN v0
// ----------------------------------------------------------------------------
//
//   * NO TERRAIN. A ridge does not accelerate the wind and a valley does not
//     shelter it. This is the single most visible missing effect for the thing
//     wind was asked for -- a lake in a steep valley should be glassy while
//     one on a plateau is choppy -- and it is left out because it needs the
//     carrier's elevation and gradient at the sample point, which means an
//     Amplifier and a tile source, which turns a pure four-argument function
//     into something that needs the streaming layer. The hook is named and
//     argued in docs/weather-system-v0.md.
//   * NO CLIMATE. climate.h's wire format carries exactly four channels
//     (bio_1, bio_4, bio_12, bio_15) and none of them is wind. Adding one
//     rolls provider_id and re-bakes every tile in the world.
//   * NO VERTICAL COMPONENT and no variation with altitude. Wind is a
//     horizontal 2D vector. Water surfaces are horizontal.
//   * NOTHING SIMULATED IS PUSHED BY IT. docs/lighting-weather-plan.md:1074-
//     1078 already rules out wind affecting physics, debris or projectiles.
//     This field is read by materials and by nothing that can change the
//     world.
//
// ----------------------------------------------------------------------------
// UNITS, AND THE ONE SIGN CONVENTION THAT WILL BITE SOMEBODY
// ----------------------------------------------------------------------------
//
//   position   millimetres, world space, the same key everything else in
//              voxel-core uses.
//   time       milliseconds of the GAME CLOCK -- UVoxelSkySubsystem's
//              EpochSeconds x 1000. Not wall-clock, not the calendar. Freezing
//              that clock freezes this field exactly; see the subsystem.
//   speed      millimetres per second. 6000 == 6 m/s.
//   bearing    milli-degrees, [0, 360000), COMPASS convention: 0 is north,
//              90000 is east.
//
// THE WORLD'S AXES ARE X = NORTH, Y = EAST, Z = UP, and this comment said the
// opposite until 2026-08-13. It read "+Y is north, +X is east", citing
// VoxelEphemeris.cpp:289 -- and the advection in sampleWind faithfully
// implemented that, which put the synoptic pattern about 90 degrees off the
// bearing it is documented to travel along.
//
// :289 is the OUTLIER, not the authority. VoxelEphemeris.h:43-45 states the
// convention outright, and DirectionFromAltAz backs it in code: with azimuth
// measured clockwise from north, Direction.x = cos(azimuth), which is the north
// component. GeoFromWorldUU at :289 takes latitude from Y, which disagrees with
// both and is a pre-existing inconsistency this file should not have adopted.
//
// GETTING THIS WRONG HAS NO SYMPTOM, which is why it needs saying here rather
// than being left to the reader. The noise is statistically isotropic, so a
// field advected along the wrong axis still looks exactly like weather. Only a
// stated property is false, and no screenshot can show you that.
//
// THE SIGN CONVENTION. `east/north` in WindSample is the direction the air
// TRAVELS. `fromBearingMilliDeg` is the direction it COMES FROM, which is what
// a human means by "a westerly" and what every weather report on Earth quotes.
// They differ by 180 degrees and both are returned, deliberately, because the
// alternative is every consumer doing that flip itself and half of them
// getting it backwards. This is the same trap VoxelSkySubsystem.cpp:3108-3133
// documents at length for the sun vector, and its failure mode here is
// identical in shape: waves that march into the wind, on a scene that
// otherwise looks completely fine.

#include "voxelcore/hash.h"

namespace vxc {

// --- fixed-point scales -----------------------------------------------------

// Q15. Chosen to equal hashToSigned16's range so that a noise value and a
// unit-vector component are the same currency and no scale factor is needed
// between them.
inline constexpr int64_t kWindQ = 32768;

// |windNoiseT| and |windNoiseXYT| are both bounded by this, exactly, because
// both are convex combinations of hashToSigned16 outputs.
inline constexpr int64_t kWindNoiseAbs = 32768;

inline constexpr int64_t kWindTurnMilliDeg = 360'000;
inline constexpr int64_t kWindRoseSectors = 32;
inline constexpr int64_t kWindRoseStepMilliDeg = kWindTurnMilliDeg / kWindRoseSectors; // 11250
static_assert(kWindRoseStepMilliDeg * kWindRoseSectors == kWindTurnMilliDeg,
              "the rose must divide the circle exactly or bearings drift at the wrap");

// --- the direction rose -----------------------------------------------------
//
// sin(k * 11.25 degrees) in Q15, k = 0..31. The COSINE table is this same
// table read at (k + 8), because cos(a) == sin(a + 90 deg) and 8 sectors is
// exactly 90 degrees -- so there is one table, not two that can disagree.
// Same device, same reason, as detail_rill.h:191-198's pair of 16-entry q12
// tables; the difference is that a wind direction is a RAY (a northerly and a
// southerly are not the same wind) where a rill axis is a LINE, so this table
// covers the whole turn instead of half of it.
//
// WHY A TABLE AND NOT AN ANGLE. The float ban means there is no sin() to call,
// and the alternative trick -- normalising a pair of noise channels into a
// unit vector -- spins wildly whenever the pair passes near the origin, which
// it does about 0.8% of the time. A table cannot do that.
//
// THE ERRORS, MEASURED RATHER THAN ASSERTED (all three from the generator in
// the scratchpad mirror described in docs/weather-system-v0.md):
//   * table entries are unit to within 0.0012%;
//   * linearly interpolating BETWEEN two entries and NOT renormalising leaves
//     the vector between 0.99514 and 1.00001 of unit length -- a 0.49% ripple
//     in speed as the bearing sweeps, which is why no integer square root
//     appears anywhere in this file and why WindSample::speedMmPerS, not the
//     vector's length, is the authority on speed;
//   * the bearing that comes back out is within 0.0094 degrees of the bearing
//     that went in.
inline constexpr int32_t kWindRoseSinQ[32] = {
         0,   6393,  12540,  18205,  23170,  27246,  30274,  32138,
     32768,  32138,  30274,  27246,  23170,  18205,  12540,   6393,
         0,  -6393, -12540, -18205, -23170, -27246, -30274, -32138,
    -32768, -32138, -30274, -27246, -23170, -18205, -12540,  -6393,
};

constexpr int64_t windRoseSinQ(int64_t k) {
    return kWindRoseSinQ[static_cast<size_t>(floorMod(k, kWindRoseSectors))];
}
constexpr int64_t windRoseCosQ(int64_t k) {
    return kWindRoseSinQ[static_cast<size_t>(floorMod(k + 8, kWindRoseSectors))];
}

// A horizontal unit direction in Q15. eastQ is +X, northQ is +Y.
struct WindDir {
    int32_t eastQ = 0;
    int32_t northQ = 0;
};

// Compass bearing -> unit vector. Wraps: any integer is legal input.
//
// The bearing here is whichever bearing the caller means; this function has no
// opinion about from-versus-toward. sampleWind passes it a TOWARD bearing.
constexpr WindDir windDirFromBearing(int64_t bearingMilliDeg) {
    const int64_t b = floorMod(bearingMilliDeg, kWindTurnMilliDeg); // [0, 360000)
    const int64_t k = b / kWindRoseStepMilliDeg;                    // b >= 0, so no floorDiv
    const int64_t f = b - k * kWindRoseStepMilliDeg;                // [0, 11250)
    const int64_t g = kWindRoseStepMilliDeg - f;
    // Widest intermediate 32768 * 11250 = 3.7e8.
    WindDir d{};
    d.eastQ = static_cast<int32_t>(
        (windRoseSinQ(k) * g + windRoseSinQ(k + 1) * f) / kWindRoseStepMilliDeg);
    d.northQ = static_cast<int32_t>(
        (windRoseCosQ(k) * g + windRoseCosQ(k + 1) * f) / kWindRoseStepMilliDeg);
    return d;
}

// --- seeding ----------------------------------------------------------------
//
// WHY THE WIND GETS ITS OWN SEED INSTEAD OF ITS OWN REGISTERED HASH CHANNELS.
//
// hash.h:16-122 is emphatic that the channel id is hash2/hash3's only domain
// separator and that reusing one is a world-breaking bug -- CH_CAVE_NODE cost
// a worldgen version for exactly that. The obvious move was therefore to
// allocate CH_WIND_* ids at 62.. and register them in
// hash_channel_registry.h.
//
// It is the wrong move here, for a reason that is about scope rather than
// about hygiene. Those ids are the namespace of WORLD DERIVATION: everything
// in it feeds terrain, feeds the edit log's digest, and is pinned by
// kWorldGenVersion. Wind feeds a material. Putting it in that namespace would
// make a change to the gust timescale look, to every tool that reads the
// registry, exactly like a change to the cave lattice -- and would invite the
// next person to bump kWorldGenVersion for it, which would invalidate every
// saved edit log in the project to make the water choppier.
//
// So the wind draws from a SEPARATE SEED, derived once from the world seed by
// this fixed salt, and numbers its channels privately from 0 inside that
// space. Two different seeds make the fields independent whatever the channel
// ids are, so a collision with worldgen is not merely unlikely, it is not the
// mechanism that could produce one.
//
// THE CONDITION UNDER WHICH THIS DECISION REVERSES, stated so it is not
// re-litigated from scratch: if wind ever becomes an INPUT to world derivation
// -- aeolian deposition, wind-driven erosion, a windward/leeward term baked
// into tiles -- then it belongs in the registry, it needs a kWorldGenVersion
// bump, and this salt has to go. Nothing in v0 comes close.
//
// The salt is the ASCII bytes "WIND_V0\0". Legible in a hex dump, which is the
// only property a salt needs beyond being fixed.
inline constexpr uint64_t kWindSeedSalt = 0x57494E445F563000ull;

constexpr uint64_t windSeed(uint64_t worldSeed) {
    return splitmix64(worldSeed ^ kWindSeedSalt);
}

// The wind's PRIVATE channel numbering. Legal to start at 0 only because of
// the derived seed above; see that comment before adding one.
enum WindChannel : uint32_t {
    CH_WIND_REGIME_TURN = 0,
    CH_WIND_PREVAIL_TURN = 1,
    CH_WIND_SYNOPTIC_TURN = 2,
    CH_WIND_SYNOPTIC_SPEED = 3,
    CH_WIND_GUST_TURN = 4,
    CH_WIND_GUST_SPEED = 5,
};

// --- noise ------------------------------------------------------------------

// One independent 2D field per time slice. splitmix64 of the slice index,
// mixed into the wind seed, so consecutive slices are uncorrelated -- which is
// what makes the pair of them a legitimate third noise axis rather than the
// same field twice.
constexpr uint64_t windSliceSeed(uint64_t seed, int64_t k) {
    return splitmix64(seed ^ splitmix64(static_cast<uint64_t>(k)));
}

// Quintic-faded value noise in TIME alone. Output in [-32768, 32767].
//
// fadeFractionMm's name says Mm but the function is unit-agnostic: it takes a
// fraction and a period in the SAME unit and returns the faded fraction in
// that unit. Here both are milliseconds.
//
// Widest intermediate: 32768 * latticeMs.
constexpr int64_t windNoiseT(uint64_t seed, int64_t tMs, int64_t latticeMs, uint32_t channel) {
    const int64_t k0 = floorDiv(tMs, latticeMs);
    const int64_t f = fadeFractionMm(tMs - k0 * latticeMs, latticeMs);
    const int64_t g = latticeMs - f;
    const int64_t a0 = hashToSigned16(hash2(seed, k0, 0, channel));
    const int64_t a1 = hashToSigned16(hash2(seed, k0 + 1, 0, channel));
    return (a0 * g + a1 * f) / latticeMs;
}

// Quintic-faded value noise in (x, y, t). Output in [-32768, 32767].
//
// BUILT IN TWO STAGES -- valueNoise2Fade on each of the two bracketing time
// slices, then a faded blend between the two results -- rather than as one
// eight-corner trilinear form. Two reasons, and the first is the binding one:
//
//   1. OVERFLOW. A monolithic 3D form's numerator is bounded by
//      32768 * Lx * Ly * Lt, and at this file's lattices (2,048,000 mm and
//      600,000 ms) that is 2.5e22 against int64's 9.2e18. Dividing after the
//      xy stage drops the bound to 32768 * Lx * Ly = 1.4e17, which is the
//      widest intermediate anywhere in this file and clears int64 by 67x. The
//      alternative was to carry position in metres instead of millimetres,
//      which would have quantised the field to a 1 m grid and broken this
//      project's one-unit-for-position rule for no gain.
//   2. It reuses the audited primitive instead of copying it. valueNoise2Fade
//      is what the whole terrain detail ladder runs on.
//
// WHAT THE TWO-STAGE FORM COSTS: the intermediate divide truncates, so a value
// can differ by at most 1 part in 65536 from the monolithic form. It is still
// EXACT in the sense that matters -- the same inputs give the same answer on
// every machine -- and it is far below the resolution of anything downstream.
//
// Continuity across the slice boundary is the quintic's, not the truncation's:
// at t exactly on a lattice line the blend weight is 0 or 1 and the answer is
// one slice's value, with matching first and second derivatives on both sides.
// Verified at 1 ms resolution across three slice boundaries.
constexpr int64_t windNoiseXYT(uint64_t seed, int64_t xMm, int64_t yMm, int64_t tMs,
                               int64_t latticeMm, int64_t latticeMs, uint32_t channel) {
    const int64_t k0 = floorDiv(tMs, latticeMs);
    const int64_t f = fadeFractionMm(tMs - k0 * latticeMs, latticeMs);
    const int64_t g = latticeMs - f;
    const int64_t a0 = valueNoise2Fade(windSliceSeed(seed, k0), xMm, yMm, latticeMm, channel);
    const int64_t a1 = valueNoise2Fade(windSliceSeed(seed, k0 + 1), xMm, yMm, latticeMm, channel);
    return (a0 * g + a1 * f) / latticeMs;
}

// (v * qNum) / kWindQ, for a v that may be enormous.
//
// EXISTS BECAUSE OF THE ADVECTION DISTANCE, which grows without bound as the
// clock runs -- about 1000 km per day of play at the defaults, and there is no
// upper limit on how long a world stays loaded. The naive `v * qNum / kWindQ`
// overflows int64 once |v| passes 2.8e14 mm (about 270 days), which is the
// kind of bug that ships because nobody plays that long during development and
// then wraps a coordinate into the middle of a different weather cell.
//
// Splitting the multiply keeps both halves small: the high half is bounded by
// |v| itself (since |qNum| <= kWindQ) and the low half by kWindQ^2 = 1.07e9.
// Exact to within one unit of truncation.
constexpr int64_t windMulQ(int64_t v, int64_t qNum) {
    const int64_t q = floorDiv(v, kWindQ);
    const int64_t r = v - q * kWindQ; // [0, kWindQ)
    return q * qNum + (r * qNum) / kWindQ;
}

// --- parameters -------------------------------------------------------------
//
// EVERY NUMBER IN HERE IS A STARTING GUESS, NOT A MEASUREMENT, with the single
// exception of the synoptic cell size. Nobody has yet looked at water driven
// by this field and said whether it moves right. They are gathered in a struct
// rather than written as constants so that the console variables on the UE
// side can move them without a rebuild, which is what makes that judgement
// cheap to make.
struct WindParams {
    // The prevailing quarter the wind COMES FROM, in milli-degrees, compass
    // convention. 240 deg is west-south-west.
    //
    // NOT AN ARBITRARY PICK, and it is chosen against the same evidence
    // voxel.Sky.OriginLatitudeDeg's 52.0 was (VoxelSkySubsystem.cpp:252-262):
    // VoxelClimateProbe measured this world's climate window as cool-temperate
    // maritime, which is the northern-European westerly belt. A world whose
    // biomes say maritime-westerly and whose wind comes off the east reads as
    // a worldgen mistake even though neither half is wrong on its own.
    //
    // ALSO THE ADVECTION DIRECTION. Changing it changes which way weather
    // travels, not merely which way it blows on average.
    int32_t baseFromBearingMilliDeg = 240'000;

    // Mean sustained speed before the synoptic multiplier, mm/s. 6000 = 6 m/s,
    // a Beaufort 4 "moderate breeze": enough to raise whitecaps on a lake, not
    // enough to look like a storm. The realised distribution at the defaults is
    // p05 3.3, median 5.9, p95 8.9 m/s.
    int32_t baseSpeedMmPerS = 6'000;

    // REGIME: the slow bearing wander that lets the wind reach the whole
    // compass. 3 h of clock; at the default voxel.Sky.DayLengthSeconds that is
    // about three game days per lattice cell.
    int64_t regimeLatticeMs = 10'800'000;
    int32_t regimeTurnMilliDeg = 180'000;

    // PREVAIL: the within-session bearing wander. 20 min of clock, a third of
    // a game day at the default day length -- long enough that a capture leg
    // does not see it move, short enough that a session does.
    int64_t prevailLatticeMs = 1'200'000;
    int32_t prevailTurnMilliDeg = 55'000;

    // SYNOPTIC. 2048 m is the one number here with a source:
    // docs/lighting-weather-plan.md:400 fixes the weather cell at ~2 km so a
    // front crosses the visible world in minutes of play. Kept exactly at a
    // power of two so the lattice lines land on round world coordinates, which
    // makes a probe's census readable.
    int64_t synopticLatticeMm = 2'048'000;
    int64_t synopticLatticeMs = 600'000;
    int32_t synopticTurnMilliDeg = 25'000;
    // Q15. 21299/32768 = 0.65, so the speed multiplier spans 0.35 .. 1.65.
    int32_t synopticSpeedSpanQ = 21'299;
    // Q15 gain on baseSpeedMmPerS to get the pattern's travel speed. 65536 =
    // 2.0, i.e. weather systems move at twice the surface wind. That is the
    // right sign and roughly the right size: real systems steer with the
    // mid-level flow, which is faster than the surface, and 2x puts a 2 km
    // cell across the visible world in a couple of minutes as the plan asks.
    int32_t advectionGainQ = 65'536;

    // GUST. 180 m of ground and 6 s of clock. Walking at 5 m/s through the
    // spatial part alone re-rolls the gust every 36 s, so the two axes
    // contribute at comparable rates and neither dominates.
    int64_t gustLatticeMm = 180'000;
    int64_t gustLatticeMs = 6'000;
    int32_t gustTurnMilliDeg = 9'000;
    // Q15, and a FRACTION OF THE SUSTAINED SPEED rather than an absolute.
    // 8192/32768 = 0.25, giving a gust factor (peak / sustained) of 1.25,
    // which is the low end of the 1.2-1.5 real anemometers see over open
    // water. The fraction form is what makes speed == sustained + gust
    // incapable of going negative, so there is no clamp to put a kink in.
    int32_t gustFractionQ = 8'192;

    // A RAIL, NOT A TUNING KNOB. Unreachable at the defaults -- the worst case
    // is 6 * 1.65 * 1.25 = 12.4 m/s against this 40 -- and it exists only so
    // that a console variable set to something silly produces a strong wind
    // rather than an arithmetic surprise downstream.
    int32_t maxSpeedMmPerS = 40'000;

    // --- pins, for reproducible captures -------------------------------------
    //
    // NEGATIVE MEANS DERIVE. A pinned bearing must be given already wrapped
    // into [0, 360000): -1 cannot mean "359.999 degrees" here, and the UE layer
    // wraps before it gets this far.
    //
    // Pinning the bearing also pins the ADVECTION direction, so a pinned run
    // has a completely stationary set of weather cells drifting one way.
    // Pinning the speed removes the gust entirely -- that is the point of it,
    // and voxel.Weather.GustScale exists for the case where you want a fixed
    // mean with the gusts left alive.
    int32_t pinFromBearingMilliDeg = -1;
    int32_t pinSpeedMmPerS = -1;
};

// --- the sample -------------------------------------------------------------

// Everything the field knows about one place at one moment.
//
// BOTH BEARINGS ARE RETURNED, and both directions of the vector question are
// answered, on purpose: see the sign-convention note in this file's header.
struct WindSample {
    // The velocity, mm/s, in the direction the air TRAVELS. +X east, +Y north.
    int32_t eastMmPerS = 0;
    int32_t northMmPerS = 0;

    // THE AUTHORITY ON SPEED. The vector above is this scaled by a unit
    // direction that is within -0.49%/+0.001% of unit length, so reconstructing
    // the speed from the vector gives a slightly different and slightly wrong
    // answer. Report this one.
    int32_t speedMmPerS = 0;

    // speedMmPerS == sustainedMmPerS + gustMmPerS, exactly, always. gust is
    // signed and is bounded by +/-25% of sustained at the default parameters.
    // Split out because a material may reasonably want to drive foam or spray
    // off the gust alone while driving wave height off the total.
    int32_t sustainedMmPerS = 0;
    int32_t gustMmPerS = 0;

    // Where the air COMES FROM -- the human convention, the one to print.
    int32_t fromBearingMilliDeg = 0;
    // Where it GOES -- 180 degrees away, the one the vector points along.
    int32_t toBearingMilliDeg = 0;

    // The unit direction the vector was built from, Q15. Exposed so a consumer
    // that wants to scale the direction by something of its own does not have
    // to divide the velocity by the speed and rediscover the 0.49% ripple.
    int32_t dirEastQ = 0;
    int32_t dirNorthQ = 0;
};

// THE ONE ENTRY POINT.
//
// A pure function of exactly its arguments. No state, no cache, no first-call
// initialisation, nothing to warm up and nothing that can drift between two
// clients -- which is docs/lighting-weather-plan.md's Part B gate satisfied by
// construction rather than by test.
//
// DOMAIN. |xMm|, |yMm| <= 1e15 (a billion km) and 0 <= tMs <= 1e14 (three
// thousand years of clock). Verified by an exhaustive-intermediate sweep of
// the integer mirror: the widest value anywhere in the call is 1.4e17, which
// clears int64 by 67x. Negative tMs is legal and behaves; it is simply never
// produced, because the game clock starts at zero.
//
// COST. Six noise evaluations -- two 1D and four 2D-pairs -- plus two rose
// lookups. No division by a non-constant, no square root, no branch that
// depends on the noise. Call it per frame per camera without thinking about
// it; call it per chunk per frame only after reading the sampling-granularity
// section of docs/weather-system-v0.md, which argues you should not.
constexpr WindSample sampleWind(uint64_t worldSeed, int64_t xMm, int64_t yMm, int64_t tMs,
                                const WindParams& p) {
    const uint64_t s = windSeed(worldSeed);
    const bool bearingPinned = p.pinFromBearingMilliDeg >= 0;

    // --- bearing, the two place-independent bands ---------------------------
    int64_t toBearing = 0;
    if (bearingPinned) {
        toBearing = static_cast<int64_t>(p.pinFromBearingMilliDeg) + 180'000;
    } else {
        const int64_t regime = windNoiseT(s, tMs, p.regimeLatticeMs, CH_WIND_REGIME_TURN);
        const int64_t prevail = windNoiseT(s, tMs, p.prevailLatticeMs, CH_WIND_PREVAIL_TURN);
        toBearing = static_cast<int64_t>(p.baseFromBearingMilliDeg) + 180'000 +
                    regime * p.regimeTurnMilliDeg / kWindNoiseAbs +
                    prevail * p.prevailTurnMilliDeg / kWindNoiseAbs;
    }

    // --- advection, along the FIXED base bearing ----------------------------
    //
    // Read the lever-arm section in this file's header before changing this to
    // use the wandering bearing. It has been tried; it teleports the weather.
    const int64_t advectBearing =
        (bearingPinned ? static_cast<int64_t>(p.pinFromBearingMilliDeg)
                       : static_cast<int64_t>(p.baseFromBearingMilliDeg)) +
        180'000;
    const WindDir advectDir = windDirFromBearing(advectBearing);

    // Distance the pattern has travelled, mm. The Q divide happens BEFORE the
    // multiply by tMs so that the large factor is introduced last.
    const int64_t advectMm =
        (static_cast<int64_t>(p.baseSpeedMmPerS) * p.advectionGainQ / kWindQ) * tMs / 1000;
    // MINUS, not plus: sliding the sample point upwind is what makes the
    // pattern appear to arrive from upwind. Get this backwards and the weather
    // recedes from the direction the wind is blowing, which looks wrong in a
    // way that is very hard to name.
    //
    // x TAKES THE NORTH COMPONENT AND y TAKES THE EAST ONE, AND THESE WERE THE
    // OTHER WAY ROUND UNTIL 2026-08-13. Applying the east component to x and the
    // north component to y reflects the advection direction about the
    // 45-degree diagonal (bearing theta becomes 90 - theta), so the synoptic
    // pattern travelled roughly across the prevailing wind instead of along it.
    //
    // It produced a perfectly good-looking field -- the noise is statistically
    // isotropic, so a wrongly-rotated advection is still a valid random field,
    // just not the one every comment here describes. Nothing about the output
    // looks wrong; only the stated property "weather is advected along the base
    // bearing" was false. That is why it survived: there is no symptom.
    //
    // The root cause was a comment. The header said "+Y is north, +X is east",
    // citing VoxelEphemeris.cpp:289, and this code faithfully implemented it.
    // The engine is X = NORTH, Y = east (VoxelEphemeris.h:43-45, and
    // DirectionFromAltAz where X = cos(azimuth)); :289 is itself the outlier,
    // taking latitude from Y. See the axis note at the top of this file.
    const int64_t ax = xMm - windMulQ(advectMm, advectDir.northQ);
    const int64_t ay = yMm - windMulQ(advectMm, advectDir.eastQ);

    // --- the two place-dependent bands --------------------------------------
    const int64_t synTurn = windNoiseXYT(s, ax, ay, tMs, p.synopticLatticeMm, p.synopticLatticeMs,
                                         CH_WIND_SYNOPTIC_TURN);
    const int64_t synSpeed = windNoiseXYT(s, ax, ay, tMs, p.synopticLatticeMm, p.synopticLatticeMs,
                                          CH_WIND_SYNOPTIC_SPEED);
    // The gust samples the UNADVECTED point: an eddy belongs to the ground you
    // are standing on, not to the system passing overhead.
    const int64_t gustTurn =
        windNoiseXYT(s, xMm, yMm, tMs, p.gustLatticeMm, p.gustLatticeMs, CH_WIND_GUST_TURN);
    const int64_t gustSpeed =
        windNoiseXYT(s, xMm, yMm, tMs, p.gustLatticeMm, p.gustLatticeMs, CH_WIND_GUST_SPEED);

    if (!bearingPinned) {
        toBearing += synTurn * p.synopticTurnMilliDeg / kWindNoiseAbs;
        toBearing += gustTurn * p.gustTurnMilliDeg / kWindNoiseAbs;
    }
    const WindDir dir = windDirFromBearing(toBearing);

    // --- speed ---------------------------------------------------------------
    int64_t sustained = 0;
    int64_t gust = 0;
    if (p.pinSpeedMmPerS >= 0) {
        sustained = p.pinSpeedMmPerS;
    } else {
        sustained = static_cast<int64_t>(p.baseSpeedMmPerS) *
                    (kWindQ + synSpeed * p.synopticSpeedSpanQ / kWindNoiseAbs) / kWindQ;
        // Only reachable if a caller sets synopticSpeedSpanQ above kWindQ,
        // which would mean asking for a negative wind speed. Floored rather
        // than asserted because voxel-core does not assert.
        if (sustained < 0) {
            sustained = 0;
        }
        gust = sustained * p.gustFractionQ / kWindQ * gustSpeed / kWindNoiseAbs;
    }
    int64_t speed = sustained + gust;
    if (speed < 0) {
        speed = 0;
    }
    if (speed > p.maxSpeedMmPerS) {
        speed = p.maxSpeedMmPerS;
        // SUSTAINED IS RAILED TOO, AND IT WAS NOT UNTIL 2026-08-13. The old code
        // clamped `speed`, then back-solved `gust = speed - sustained` and left
        // `sustained` alone -- so a pinned 60 m/s reported speed 40, sustained
        // 60, gust -20 (reproduce with:
        // vxc_windprobe 20260719 --pin-speed 60). Two things were wrong with
        // that, and the second one is the expensive one:
        //
        //   * The documented invariant "gust is within +/-25% of sustained"
        //     (asserted in test_weather.cpp) is false whenever the rail fires.
        //
        //   * UVoxelWeatherSubsystem::PublishWind sends direction * SUSTAINED,
        //     not * speed. So the water would have been driven at 60 m/s while
        //     every log line, and the probe, said 40 -- a 1.5x wave amplitude
        //     with no way to see where it came from. Reachable from legal
        //     cvars: voxel.Weather.BaseWindMps 6 with WindScale 8.
        //
        // Railing sustained as well means every scalar this function returns is
        // bounded by maxSpeedMmPerS, so it does not matter which one a consumer
        // reaches for -- which is the property that makes the rail a rail
        // rather than a suggestion.
        if (sustained > speed) {
            sustained = speed;
        }
        gust = speed - sustained;
    }

    WindSample out{};
    out.dirEastQ = dir.eastQ;
    out.dirNorthQ = dir.northQ;
    out.eastMmPerS = static_cast<int32_t>(dir.eastQ * speed / kWindQ);
    out.northMmPerS = static_cast<int32_t>(dir.northQ * speed / kWindQ);
    out.speedMmPerS = static_cast<int32_t>(speed);
    out.sustainedMmPerS = static_cast<int32_t>(sustained);
    out.gustMmPerS = static_cast<int32_t>(gust);
    out.toBearingMilliDeg = static_cast<int32_t>(floorMod(toBearing, kWindTurnMilliDeg));
    out.fromBearingMilliDeg =
        static_cast<int32_t>(floorMod(toBearing + 180'000, kWindTurnMilliDeg));
    return out;
}

// --- naming -----------------------------------------------------------------

// "WSW" and friends, from a FROM-bearing. Sixteen points.
//
// Here rather than in each consumer for the reason
// VoxelSkySubsystem.h:280-287 gives about MonthDayFromDayOfYear: this is a
// derivation with exactly one right answer, and the moment there are two
// copies one of them is off by a sector. The probe prints it, the HUD will
// print it, and a log line quoting a bearing is much harder to read than one
// quoting a quarter.
//
// The half-sector offset is what makes the sector CENTRED on its name: north
// is 348.75..11.25 degrees, not 0..22.5.
inline constexpr const char* kWindCompass16[16] = {
    "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
    "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
};

constexpr const char* windCompass16(int64_t fromBearingMilliDeg) {
    const int64_t half = kWindTurnMilliDeg / 32; // 11250
    const int64_t b = floorMod(fromBearingMilliDeg + half, kWindTurnMilliDeg);
    return kWindCompass16[static_cast<size_t>(b / (kWindTurnMilliDeg / 16))];
}

// --- compile-time proofs ----------------------------------------------------
//
// These are not decoration. Each one pins a property something downstream
// relies on, and each fails the BUILD rather than a screenshot.

// The rose's cardinal points are exact, which is what lets a pinned bearing of
// 90 degrees mean exactly "due east" in a capture.
static_assert(windDirFromBearing(0).eastQ == 0 && windDirFromBearing(0).northQ == 32768);
static_assert(windDirFromBearing(90'000).eastQ == 32768 && windDirFromBearing(90'000).northQ == 0);
static_assert(windDirFromBearing(180'000).eastQ == 0 && windDirFromBearing(180'000).northQ == -32768);
static_assert(windDirFromBearing(270'000).eastQ == -32768 && windDirFromBearing(270'000).northQ == 0);

// Wrapping is real wrapping, not a clamp. A bearing accumulated over hours of
// wander goes well outside one turn before it gets here.
static_assert(windDirFromBearing(360'000).northQ == 32768);
static_assert(windDirFromBearing(-270'000).eastQ == 32768);
static_assert(windDirFromBearing(3'690'000).eastQ == 32768); // 10 turns + 90 deg

// The single-table trick: cos really is sin read 8 sectors along.
static_assert(windRoseCosQ(0) == windRoseSinQ(8));
static_assert(windRoseCosQ(29) == windRoseSinQ(37));

// The compass names line up with the bearings, including at the wrap where an
// off-by-a-half-sector would put north on the wrong side of the seam.
static_assert(windCompass16(0)[0] == 'N' && windCompass16(0)[1] == '\0');
static_assert(windCompass16(359'000)[0] == 'N' && windCompass16(359'000)[1] == '\0');
static_assert(windCompass16(90'000)[0] == 'E' && windCompass16(90'000)[1] == '\0');
static_assert(windCompass16(247'500)[0] == 'W' && windCompass16(247'500)[1] == 'S');

// windMulQ agrees with the naive form on values where the naive form is safe,
// and keeps working where it is not. The third case is the one this function
// exists for: 3e14 mm of advection is about 290 days of play at the defaults,
// and the naive product would have overflowed int64 well before it.
static_assert(windMulQ(32768, 16384) == 16384);
static_assert(windMulQ(-32768, 16384) == -16384);
static_assert(windMulQ(300'000'000'000'000, 32768) == 300'000'000'000'000);

// A pinned wind is EXACTLY what was pinned. This is the property every
// reproducible capture leans on, so it is proved at compile time rather than
// hoped for at runtime.
//
// NOTE THE SIGN, WHICH IS THE WHOLE POINT OF THE TEST. Pinning the wind to
// come FROM 270 (from the west) must produce a velocity pointing EAST, i.e.
// eastMmPerS POSITIVE. The first draft of this assertion had it negative --
// the flip is genuinely easy to get wrong even while writing the paragraph
// that warns about it.
static_assert([] {
    WindParams p{};
    p.pinFromBearingMilliDeg = 270'000;
    p.pinSpeedMmPerS = 9'000;
    const WindSample w = sampleWind(20260719ull, 123'456, -98'765, 55'555, p);
    return w.fromBearingMilliDeg == 270'000 && w.toBearingMilliDeg == 90'000 &&
           w.speedMmPerS == 9'000 && w.gustMmPerS == 0 && w.eastMmPerS == 9'000 &&
           w.northMmPerS == 0;
}());

} // namespace vxc
