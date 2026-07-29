#pragma once

#include "CoreMinimal.h"

// Solar / lunar ephemeris ----------------------------------------------------
//
// WHERE THIS LIVES, AND WHY IT IS NOT IN voxel-core.
//
// Solar position is irreducibly double-precision arithmetic: it is a Julian
// century multiplied by ~36000 degrees per century, and the answer is wanted to
// half a degree. There is no fixed-point formulation of that which is worth
// writing. voxel-core cannot host it -- ci.yml's `float-ban` job greps
// voxel-core/include and voxel-core/src for the tokens `float` and `double` as
// TYPES and fails the build on a hit -- and that ban is not an inconvenience to
// route around, it is the determinism doctrine stated mechanically.
//
// The placement is therefore not a workaround, it is the correct side of the
// boundary. The sun is a RENDERING INPUT. It orients a DirectionalLight and
// tints a sky; it never feeds worldgen, never enters the edit log, and nothing
// it produces is replicated or digested. Two clients may compute sun altitudes
// that differ in the twelfth decimal place and must still agree, bit for bit,
// about world state -- and nothing here can make them disagree, because nothing
// here is ever read by anything that derives world state. This is the same
// argument VoxelGI.h:26-30 makes for the light field, applied to a much smaller
// object. See that comment; the reasoning is identical and is not repeated.
//
// THE SIGN TRAP, stated here because the consumer is where it bites.
//
// FSunState::Direction points FROM the surface TOWARD the sun. A
// DirectionalLight's forward vector is the direction the light TRAVELS, which
// is the opposite. So a consumer orienting a light must write
//
//     SunLight->SetWorldRotation((-SunState.Direction).Rotation());
//
// and NOT `SunState.Direction.Rotation()`. Getting this backwards produces a
// scene that is lit from underground at noon and blazing at midnight, and it
// looks enough like "the ephemeris has the wrong sign" that the search starts
// in the wrong file. The vector is defined toward the sun rather than along the
// light because every OTHER consumer -- sky colour, shadow-length heuristics,
// "is the sun up", the moon's own phase geometry -- wants the toward form, and
// exactly one consumer wants the negation.
//
// AXIS CONVENTION. UE world axes: X = north, Y = east, Z = up. Azimuth is
// degrees clockwise from north (so east = 90, south = 180, west = 270), which
// is the surveying/astronomy convention and matches the (X=north, Y=east)
// handedness directly:
//
//     Direction = (cos(Alt)*cos(Az), cos(Alt)*sin(Az), sin(Alt))
//
// ACCURACY BAR. The SUN is held to 0.5 degrees of published values in altitude
// and azimuth, verified at four checkpoints in VoxelSkyTests.cpp (equinox,
// both solstices, and midnight sun above the Arctic circle). The MOON is a
// deliberately coarse circular-orbit approximation and is NOT held to that bar;
// see ComputeMoon for what it does and does not promise.
namespace VoxelSky
{
	// A point on the notional globe. Latitude north-positive, longitude
	// east-positive, both in degrees -- the convention every solar reference
	// uses, so nothing here has to be re-signed against a source.
	struct FGeoCoord
	{
		double LatitudeDeg = 0.0;
		double LongitudeDeg = 0.0;
	};

	struct FSunState
	{
		// APPARENT altitude: geometric altitude plus the atmospheric refraction
		// correction, because "is the sun up" is a question about the apparent
		// disc. Refraction is worth ~0.57 degrees at the horizon and under
		// 0.02 degrees above 40 degrees elevation, so it matters at exactly the
		// times of day anyone will look at.
		double AltitudeDeg = 0.0;   // degrees above the horizon, negative below
		double AzimuthDeg  = 0.0;   // degrees clockwise from north
		// Unit vector FROM the surface TOWARD the sun. Negate it for a
		// DirectionalLight -- see the file header comment.
		FVector Direction  = FVector::ZeroVector;
	};

	struct FMoonState
	{
		double AltitudeDeg = 0.0;
		double AzimuthDeg  = 0.0;
		FVector Direction  = FVector::ZeroVector;
		// 0 = new, 0.25 = first quarter, 0.5 = full, 0.75 = last quarter,
		// 1 = new again. Monotonically increasing through a synodic month, so a
		// consumer can distinguish waxing from waning -- which the illuminated
		// fraction alone cannot do, since it is symmetric about full.
		double PhaseFraction = 0.0;
		// Fraction of the visible lunar disc that is lit, 0..1. This is the one
		// to multiply a moonlight intensity by; PhaseFraction is not that
		// number (a "half moon" at PhaseFraction 0.25 is 50% lit, not 25%).
		double IlluminatedFraction = 0.0;
	};

	// World position -> geographic coordinate.
	//
	// The world is an unlabelled infinite plane. This mapping is therefore a
	// DEFINITION, not a measurement: we declare that the world origin sits at
	// (OriginLatitudeDeg, OriginLongitudeDeg) and that the plane is locally
	// tangent there, with +Y north and +X east at 111320 m per degree of
	// latitude (the WGS84 mean). Nothing verifies it because there is nothing
	// to verify it against; what matters is that it is stable, continuous, and
	// documented, so two subsystems asking "where am I" get the same answer.
	//
	// Longitude divides by cos(latitude), so it is guarded near the poles: the
	// cosine is floored, latitude is clamped to [-90, 90], and the result is
	// wrapped into [-180, 180]. Walking to the pole gives you a large but
	// finite longitude, never an infinity that propagates into a NaN sun.
	VOXELEARTH_API FGeoCoord GeoFromWorldUU(double WorldXUU, double WorldYUU,
	                                        double OriginLatitudeDeg, double OriginLongitudeDeg);

	// Game clock -> Julian Day.
	//
	// THE CALENDAR MAPPING, which is load-bearing and easy to misread:
	//
	//     DayFraction   = frac(WorldEpochSeconds / DayLengthSeconds)
	//     YearFraction  = frac(WorldEpochSeconds / (DayLengthSeconds * DaysPerYear))
	//     RealDayOfYear = YearFraction * 365.2425
	//     JulianDay     = JD(2000-01-01 00:00 UT) + floor(RealDayOfYear) + DayFraction
	//
	// Two clocks run at once and they are deliberately decoupled. DayFraction
	// drives the DIURNAL cycle: exactly one sunrise per DayLengthSeconds, with
	// UTC hours = DayFraction * 24. RealDayOfYear drives the SEASONAL cycle: a
	// game year of DaysPerYear game-days is stretched across a full 365.2425-day
	// real year, so a compressed calendar still sweeps the entire declination
	// cycle -- solstice to solstice, midnight sun to polar night -- rather than
	// sampling a 60-day slice of it.
	//
	// THE CONSEQUENCE A READER WILL MISTAKE FOR A BUG. Because those two rates
	// differ, the DATE advances by 365.2425/DaysPerYear real days over a single
	// game day. At DaysPerYear = 60 that is ~6 real days per game day, so
	// floor(RealDayOfYear) steps six times BETWEEN one sunrise and the next and
	// the solar declination visibly moves WITHIN a single game day: the sun
	// rises at a noticeably different point on the horizon than it set at. That
	// is intended and is the whole point of compressing the year. It is not a
	// wrapping bug, and "fixing" it by locking the date to the game day would
	// reduce the seasonal cycle to a slideshow of DaysPerYear distinct dates.
	//
	// The floor() is what keeps the diurnal cycle honest and is not optional:
	// the date term must be a whole number of days so that frac(JD + 0.5) --
	// which is where ComputeSun reads UTC time-of-day from -- stays exactly
	// equal to DayFraction. Letting the date advance continuously instead would
	// add its own rotation to the hour angle and the sun would complete
	// 1 + 365.2425/DaysPerYear diurnal circuits per game day.
	//
	// REFERENCE YEAR 2000, chosen because it IS the J2000.0 epoch: every
	// coefficient in the Meeus/NOAA series below is expressed in Julian
	// centuries from JD 2451545.0, so a year-2000 date keeps the Julian century
	// T within +/-0.005 and the truncated series at their most accurate. It also
	// makes the checkpoint dates in VoxelSkyTests.cpp look up directly in
	// published almanac tables for 2000. Note 2000 is a leap year, which is why
	// day-of-year 79 is 20 March and not 21 March.
	VOXELEARTH_API double JulianDayFromGameClock(double WorldEpochSeconds,
	                                             double DayLengthSeconds,
	                                             double DaysPerYear);

	// Low-precision solar position (NOAA solar calculator / Meeus ch. 25).
	//
	// Geometric mean longitude and anomaly -> equation of centre -> apparent
	// longitude (aberration + nutation in longitude) -> obliquity with its
	// nutation term -> declination and equation of time -> true solar time ->
	// hour angle -> altitude/azimuth. Accurate to well under a minute of arc
	// for any date within a couple of centuries of J2000, which is two orders
	// of magnitude better than the 0.5-degree bar this is held to.
	VOXELEARTH_API FSunState ComputeSun(double JulianDay, const FGeoCoord& Geo);

	// Approximate lunar position and phase.
	//
	// DELIBERATELY COARSE, AND NOT A PLACEHOLDER. This is a circular-orbit
	// model: mean lunar longitude at a constant 13.176396 deg/day (the 27.32-day
	// sidereal period), ecliptic latitude as a single 5.145-degree inclination
	// term, and phase from the sun-moon elongation. It omits evection, variation
	// and the annual equation, so the moon's ECLIPTIC LONGITUDE can be off by up
	// to ~1.3 degrees and its rise time by several minutes.
	//
	// That is the right trade and should not be "fixed" without a reason.
	// A truncated ELP2000 is hundreds of periodic terms for an error nobody can
	// observe here: the moon is a 0.5-degree disc rendered against a compressed
	// clock where a game day may be 20 minutes, so an error of one lunar
	// diameter is under a second of wall-clock time and there is no in-game
	// instrument that could resolve it. The 0.5-degree accuracy bar in
	// VoxelSkyTests.cpp applies to the SUN ONLY; the moon test asserts only what
	// this approximation actually guarantees (phase in range, illumination in
	// range, opposition at full, and a correct 29.53-day synodic period).
	//
	// Sun is passed in rather than recomputed because the observed elongation --
	// the angle between the direction to the sun and the direction to the moon,
	// both already in the same local horizon frame -- is exactly what sets the
	// illuminated fraction, and taking it from the caller's FSunState keeps the
	// returned phase self-consistent with the returned directions.
	VOXELEARTH_API FMoonState ComputeMoon(double JulianDay, const FGeoCoord& Geo, const FSunState& Sun);

	// Local mean sidereal time, in degrees, wrapped into [0, 360).
	//
	// WHAT IT IS FOR. This is the one number that turns a CELESTIAL coordinate
	// (right ascension, which is fixed to the stars) into a LOCAL one (hour
	// angle, which is where the thing actually is in your sky). Everything in
	// this file that needs it already had it -- ComputeMoon computes the moon's
	// hour angle from exactly this quantity -- but a star FIELD needs it too, and
	// needs it OUTSIDE this file: M_NightSky's StarRotation parameter is local
	// sidereal time expressed in TURNS, i.e. this divided by 360
	// (Tools/create_sky_material.py, "EQUIRECT UV").
	//
	// EXPORTED AS A WRAPPER OVER THE EXISTING FORMULA, NOT AS A SECOND COPY OF
	// IT. Meeus 12.4 is four coefficients and it would be trivial to paste into
	// VoxelSkySubsystem.cpp; that is the trap. The stars and the moon would then
	// be driven by two independently-editable expressions for the same angle, and
	// a divergence between them shows up as a moon that drifts through a star
	// field over hours of play -- slow, subtle, and impossible to attribute to
	// either file. There is one expression, in ComputeMoon's own path, and this
	// is it.
	//
	// MEAN, NOT APPARENT: no equation of the equinoxes (nutation in right
	// ascension), which is under 1.2 arcseconds. That is four orders of magnitude
	// below the moon model's own ~1.3-degree longitude error and about a
	// thousandth of a star map texel at 4k, so it would be a term nothing here
	// could resolve.
	VOXELEARTH_API double LocalSiderealTimeDeg(double JulianDay, const FGeoCoord& Geo);
}
