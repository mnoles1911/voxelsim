// Tests for the solar/lunar ephemeris (VoxelEphemeris.h).
//
// The sun is the one part of the sky that can be checked against something
// outside this repository, so it is checked hard: altitude and azimuth within
// 0.5 degrees of the published solar-noon relation at four checkpoints, one of
// which is the midnight sun above the Arctic circle. That last one is not
// decoration -- it is the only checkpoint where a latitude sign error, an hour
// angle wrapped into the wrong half turn, or an azimuth branch taken on the
// wrong side of the meridian all produce a visibly wrong answer instead of a
// plausible one. Three noon checks at one latitude would pass with any of them.
//
// Every checkpoint is constructed by building a WorldEpochSeconds and pushing it
// through JulianDayFromGameClock, never by handing ComputeSun a Julian Day
// directly. The calendar mapping is the part most likely to be quietly wrong,
// and a test that skipped it would gate the astronomy while leaving the thing
// the astronomy is actually driven by untested.
//
// Run headlessly:
//   UnrealEditor-Cmd.exe VoxelEarth.uproject -unattended -nullrhi -nop4 \
//     -ExecCmds="Automation RunTests VoxelEarth.Sky; Quit"

#include "VoxelEphemeris.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// EAutomationTestFlags is a strongly-typed enum class in 5.8, not an int
	// bitmask, so this must keep the enum type all the way through.
	constexpr EAutomationTestFlags kTestFlags = EAutomationTestFlags::EditorContext
	                                          | EAutomationTestFlags::ClientContext
	                                          | EAutomationTestFlags::EngineFilter;

	// --- the game calendar these tests are written against -------------------
	//
	// A 20-minute day, which is the compression the sky is actually being built
	// for. DaysPerYear is 365.2425 here so that one game day is exactly one real
	// day of CALENDAR and the checkpoint dates below are literally 20 March,
	// 21 June and 21 December of the year 2000 -- the dates the published
	// declination values belong to. Compressing the year as well would leave the
	// checkpoints landing on dates nobody has tabulated, and the gate would be
	// measuring the ephemeris against itself.
	//
	// The compressed-year case is not skipped, it is tested separately at the
	// end of the solar test with kCompressedDaysPerYear, where the assertion is
	// the one that actually matters there: that a short year still SWEEPS the
	// full declination cycle rather than sampling a slice of it.
	constexpr double kDayLengthSeconds = 1200.0;
	constexpr double kDaysPerYear      = 365.2425;
	constexpr double kCompressedDaysPerYear = 60.0;

	// JD of 2000-01-01 00:00 UT, i.e. day-of-year index 0. Duplicated from the
	// implementation on purpose: a test that imported the constant could not
	// catch the implementation moving it by half a day, which is the classic
	// Julian Day mistake (JD ticks over at NOON).
	constexpr double kJD2000Jan1_0hUT = 2451544.5;

	// --- checkpoint dates ----------------------------------------------------
	//
	// Day-of-year INDEX, zero-based: index 0 is 1 January. 2000 is a leap year,
	// which is what makes these one lower than the non-leap arithmetic gives.
	//   1 Mar = 31 + 29           = 60,  so 20 Mar = 60 + 19  =  79
	//   1 Jun = 31+29+31+30+31    = 152, so 21 Jun = 152 + 20 = 172
	//   1 Dec = 152+30+31+31+30+31+30 = 335, so 21 Dec = 335 + 20 = 355
	constexpr int32 kDayIndexMarch20 = 79;
	constexpr int32 kDayIndexJune21  = 172;
	constexpr int32 kDayIndexDec21   = 355;

	// --- expected values, with their derivations -----------------------------
	//
	// All four come from the standard solar-noon relation
	//     altitude = 90 - |latitude - declination|
	// and its midnight counterpart
	//     altitude = declination + latitude - 90
	// so a future reader can check the TEST rather than having to trust it.
	//
	// Mean obliquity of the ecliptic at J2000.0 is 23 deg 26' 21.448"
	// = 23.43929 deg, and the solstice declination equals the obliquity.
	constexpr double kObliquityDeg = 23.4393;

	constexpr double kSiteLatitudeDeg   = 52.0;  // the three noon checkpoints
	constexpr double kArcticLatitudeDeg = 70.0;  // the midnight-sun checkpoint

	// 1. EQUINOX, 20 March 2000, 52.0N, local solar noon.
	//    The March equinox instant that year was 20 Mar 07:35 UT, and declination
	//    climbs about 0.40 deg/day around it, so by local noon it has reached
	//    +0.076 deg rather than exactly zero. Using 0.0 here would still pass the
	//    0.5 deg bar; the real number is used because a checkpoint whose expected
	//    value is "zero, near enough" teaches the next reader nothing.
	constexpr double kEquinoxDeclinationDeg  = 0.076;
	constexpr double kEquinoxNoonAltitudeDeg = 90.0 - (kSiteLatitudeDeg - kEquinoxDeclinationDeg); // 38.076

	// 2. JUNE SOLSTICE, 21 June 2000, 52.0N, local solar noon.
	//    Declination = +obliquity.
	constexpr double kSummerNoonAltitudeDeg = 90.0 - (kSiteLatitudeDeg - kObliquityDeg);           // 61.4393

	// 3. DECEMBER SOLSTICE, 21 December 2000, 52.0N, local solar noon.
	//    Declination = -obliquity, so the latitude difference ADDS.
	constexpr double kWinterNoonAltitudeDeg = 90.0 - (kSiteLatitudeDeg + kObliquityDeg);           // 14.5607

	// 4. MIDNIGHT SUN, 21 June 2000, 70.0N, local solar MIDNIGHT.
	//    At lower culmination the sun sits (90 - latitude) below the pole, so its
	//    altitude is declination - (90 - latitude). Positive by 3.44 deg because
	//    70N is 3.44 deg inside the Arctic circle (66.56N = 90 - obliquity).
	constexpr double kMidnightSunAltitudeDeg = kObliquityDeg + kArcticLatitudeDeg - 90.0;          // 3.4393

	// Longitude for checkpoint 4. At 180E, UTC noon IS local solar midnight, so
	// the lower culmination lands in the MIDDLE of the game day rather than at
	// its wrap point -- which keeps the search window below entirely inside one
	// game day and one calendar date. Solving it by searching across midnight
	// instead would have the date term step underneath the search.
	constexpr double kAntimeridianLongitudeDeg = 180.0;

	// The hard gate.
	constexpr double kToleranceDeg = 0.5;

	// Degrees->radians, spelled out to match VoxelEphemeris.cpp's own constant
	// exactly. The tolerances on the two exact-agreement checks below are 1e-9,
	// which is tight enough that "the test converted its angle slightly
	// differently from the implementation" would be a real failure mode -- and
	// a maddening one, since nothing would be wrong with the ephemeris.
	constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

	// The expected altitudes above are GEOMETRIC; ComputeSun returns the
	// apparent (refracted) altitude, which is larger. The gap is 0.007 deg at
	// checkpoint 2, 0.026 at 1, 0.062 at 3 and 0.208 at 4 -- refraction grows
	// as the sun approaches the horizon, which is exactly why checkpoint 4 is
	// the one that would fail first if the refraction term were dropped or
	// double-applied. All four sit comfortably inside 0.5 deg either way.

	double EpochSecondsFor(int32 DayIndex, double DayFraction)
	{
		// With DaysPerYear = 365.2425 the mapping inverts exactly:
		// epoch/(DayLength) = DayIndex + DayFraction gives
		// DayFraction  = frac(DayIndex + DayFraction) = DayFraction, and
		// RealDayOfYear = DayIndex + DayFraction, whose floor is DayIndex.
		return (static_cast<double>(DayIndex) + DayFraction) * kDayLengthSeconds;
	}

	// Shortest angular separation, in degrees, ignoring direction. Written in
	// double rather than using FMath::FindDeltaAngleDegrees, which is float.
	// Needed because "azimuth 0" and "azimuth 359.999" are the same direction
	// and the midnight-sun checkpoint lands on exactly that seam.
	double AngleErrorDeg(double A, double B)
	{
		double D = FMath::Fmod(A - B, 360.0);
		if (D < -180.0) { D += 360.0; }
		if (D >  180.0) { D -= 360.0; }
		return FMath::Abs(D);
	}

	VoxelSky::FSunState SunAt(int32 DayIndex, double DayFraction, const VoxelSky::FGeoCoord& Geo)
	{
		const double JulianDay = VoxelSky::JulianDayFromGameClock(
			EpochSecondsFor(DayIndex, DayFraction), kDayLengthSeconds, kDaysPerYear);
		return VoxelSky::ComputeSun(JulianDay, Geo);
	}

	// Find the day fraction at which the sun culminates (upper culmination for
	// bMaximum, lower for its opposite).
	//
	// SEARCHED, not asserted at a fixed clock time, because "local solar noon"
	// is not 12:00 UTC: the equation of time moves it by up to 16 minutes across
	// the year, and at 52N in June the azimuth swings 1.9 degrees per minute of
	// hour angle -- so a checkpoint pinned to 12:00 would miss the 0.5 deg
	// azimuth bar for reasons that have nothing to do with the ephemeris being
	// wrong. Altitude at an extremum is second-order flat and would not have
	// noticed; azimuth is first-order and does.
	//
	// Ternary search over [0.4, 0.6] of the day. That window is 9.6 hours wide,
	// contains every possible position of solar noon, is unimodal in altitude
	// (the sun rises to one maximum and falls), and stays inside a single game
	// day so floor(RealDayOfYear) -- and therefore the declination -- is fixed
	// throughout. 60 iterations shrink it by (2/3)^60, to about 3e-7 seconds.
	double FindCulminationFraction(int32 DayIndex, const VoxelSky::FGeoCoord& Geo, bool bMaximum)
	{
		double Lo = 0.4;
		double Hi = 0.6;
		for (int32 Iter = 0; Iter < 60; ++Iter)
		{
			const double M1 = Lo + (Hi - Lo) / 3.0;
			const double M2 = Hi - (Hi - Lo) / 3.0;
			const double A1 = SunAt(DayIndex, M1, Geo).AltitudeDeg;
			const double A2 = SunAt(DayIndex, M2, Geo).AltitudeDeg;
			const bool bKeepLeft = bMaximum ? (A1 > A2) : (A1 < A2);
			if (bKeepLeft) { Hi = M2; } else { Lo = M1; }
		}
		return 0.5 * (Lo + Hi);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSkySolarTest,
	"VoxelEarth.Sky.SolarPosition", kTestFlags)

bool FVoxelSkySolarTest::RunTest(const FString& Parameters)
{
	auto ExpectNear = [this](const TCHAR* Label, double Actual, double Expected, double Tolerance)
	{
		if (!(FMath::Abs(Actual - Expected) <= Tolerance))
		{
			AddError(FString::Printf(TEXT("%s: got %.4f, expected %.4f +/- %.4f (error %.4f)"),
				Label, Actual, Expected, Tolerance, Actual - Expected));
		}
	};
	auto ExpectAngleNear = [this](const TCHAR* Label, double Actual, double Expected, double Tolerance)
	{
		const double Err = AngleErrorDeg(Actual, Expected);
		if (!(Err <= Tolerance))
		{
			AddError(FString::Printf(TEXT("%s: got %.4f deg, expected %.4f deg +/- %.4f (error %.4f)"),
				Label, Actual, Expected, Tolerance, Err));
		}
	};

	// --- the calendar mapping itself, before anything depends on it ----------
	//
	// If this is wrong every checkpoint below is measuring the wrong day, and
	// the failures would all look like astronomy bugs.
	{
		const double JD = VoxelSky::JulianDayFromGameClock(
			EpochSecondsFor(kDayIndexMarch20, 0.5), kDayLengthSeconds, kDaysPerYear);
		ExpectNear(TEXT("calendar: day 79 at half past the game day -> JD 2000-03-20 12:00 UT"),
			JD, kJD2000Jan1_0hUT + kDayIndexMarch20 + 0.5, 1.0e-9);

		// The day fraction, not the date, is what drives the diurnal cycle, so a
		// whole number of game days must not move the time of day at all.
		const double JDStart = VoxelSky::JulianDayFromGameClock(
			EpochSecondsFor(0, 0.0), kDayLengthSeconds, kDaysPerYear);
		ExpectNear(TEXT("calendar: game epoch 0 -> JD 2000-01-01 00:00 UT"),
			JDStart, kJD2000Jan1_0hUT, 1.0e-9);

		// Negative epoch seconds are ordinary (a rewound clock, a pre-rolled
		// start, a test) and must stay PERIODIC rather than reflecting about
		// zero. This is the FMath::Frac trap the implementation avoids with
		// FloorToDouble: Frac truncates toward zero, so -0.25 of a day would
		// come back as -0.25 instead of +0.75 and the sun would be on the wrong
		// side of midnight. Compare only the time-of-day part; the two clocks
		// are on different dates and only the fraction is under test here.
		const double JDNeg = VoxelSky::JulianDayFromGameClock(
			-0.25 * kDayLengthSeconds, kDayLengthSeconds, kDaysPerYear);
		const double JDPos = VoxelSky::JulianDayFromGameClock(
			0.75 * kDayLengthSeconds, kDayLengthSeconds, kDaysPerYear);
		ExpectNear(TEXT("calendar: -0.25 day and +0.75 day are the same time of day"),
			JDNeg - FMath::FloorToDouble(JDNeg), JDPos - FMath::FloorToDouble(JDPos), 1.0e-9);
	}

	// --- checkpoint 1: equinox, 52N, local solar noon ------------------------
	{
		const VoxelSky::FGeoCoord Geo{ kSiteLatitudeDeg, 0.0 };
		const double Fraction = FindCulminationFraction(kDayIndexMarch20, Geo, /*bMaximum=*/true);
		const VoxelSky::FSunState Sun = SunAt(kDayIndexMarch20, Fraction, Geo);

		ExpectNear(TEXT("equinox 52N noon altitude"),
			Sun.AltitudeDeg, kEquinoxNoonAltitudeDeg, kToleranceDeg);
		ExpectAngleNear(TEXT("equinox 52N noon azimuth (due south)"),
			Sun.AzimuthDeg, 180.0, kToleranceDeg);

		// The direction vector must agree with the angles it was built from, or
		// a consumer reading Direction and a consumer reading AltitudeDeg would
		// disagree about where the sun is.
		TestTrue(TEXT("equinox: sun direction is unit length"), Sun.Direction.IsNormalized());
		ExpectNear(TEXT("equinox: direction Z matches sin(altitude)"),
			Sun.Direction.Z, FMath::Sin(Sun.AltitudeDeg * kDegToRad), 1.0e-9);
		// Due south in UE axes (X north, Y east) is -X, and the sun is above the
		// horizon, so Z is positive. This is the check that catches an X/Y swap,
		// which every other assertion here is blind to.
		TestTrue(TEXT("equinox: sun is to the south (-X)"), Sun.Direction.X < 0.0);
		TestTrue(TEXT("equinox: sun is above the horizon (+Z)"), Sun.Direction.Z > 0.0);
	}

	// --- checkpoint 2: June solstice, 52N, local solar noon ------------------
	{
		const VoxelSky::FGeoCoord Geo{ kSiteLatitudeDeg, 0.0 };
		const double Fraction = FindCulminationFraction(kDayIndexJune21, Geo, /*bMaximum=*/true);
		const VoxelSky::FSunState Sun = SunAt(kDayIndexJune21, Fraction, Geo);

		ExpectNear(TEXT("June solstice 52N noon altitude"),
			Sun.AltitudeDeg, kSummerNoonAltitudeDeg, kToleranceDeg);
		ExpectAngleNear(TEXT("June solstice 52N noon azimuth (due south)"),
			Sun.AzimuthDeg, 180.0, kToleranceDeg);
	}

	// --- checkpoint 3: December solstice, 52N, local solar noon --------------
	{
		const VoxelSky::FGeoCoord Geo{ kSiteLatitudeDeg, 0.0 };
		const double Fraction = FindCulminationFraction(kDayIndexDec21, Geo, /*bMaximum=*/true);
		const VoxelSky::FSunState Sun = SunAt(kDayIndexDec21, Fraction, Geo);

		ExpectNear(TEXT("December solstice 52N noon altitude"),
			Sun.AltitudeDeg, kWinterNoonAltitudeDeg, kToleranceDeg);
		ExpectAngleNear(TEXT("December solstice 52N noon azimuth (due south)"),
			Sun.AzimuthDeg, 180.0, kToleranceDeg);

		// The whole point of the winter checkpoint: the sun is low but up.
		TestTrue(TEXT("December solstice 52N: sun is still above the horizon at noon"),
			Sun.AltitudeDeg > 0.0);
	}

	// --- checkpoint 4: midnight sun, 70N, local solar midnight ---------------
	{
		const VoxelSky::FGeoCoord Geo{ kArcticLatitudeDeg, kAntimeridianLongitudeDeg };
		const double Fraction = FindCulminationFraction(kDayIndexJune21, Geo, /*bMaximum=*/false);
		const VoxelSky::FSunState Sun = SunAt(kDayIndexJune21, Fraction, Geo);

		ExpectNear(TEXT("June solstice 70N midnight altitude"),
			Sun.AltitudeDeg, kMidnightSunAltitudeDeg, kToleranceDeg);
		// Due NORTH at lower culmination -- the seam at 0/360, which is why this
		// comparison has to be wrap-aware.
		ExpectAngleNear(TEXT("June solstice 70N midnight azimuth (due north)"),
			Sun.AzimuthDeg, 0.0, kToleranceDeg);

		// THE ASSERTION THE CHECKPOINT EXISTS FOR. The sun's LOWEST point of the
		// day is still above the horizon: it does not set at all. Any sign error
		// in latitude, declination or the hour angle turns this negative.
		TestTrue(TEXT("midnight sun: lowest altitude of the day is still above the horizon"),
			Sun.AltitudeDeg > 0.0);
		TestTrue(TEXT("midnight sun: direction points north (+X)"), Sun.Direction.X > 0.0);
		TestTrue(TEXT("midnight sun: direction points up (+Z)"), Sun.Direction.Z > 0.0);
	}

	// --- the compressed year still sweeps the whole declination cycle --------
	//
	// This is the claim JulianDayFromGameClock's header makes and the reason the
	// year mapping exists at all. With DaysPerYear = 60 an entire year passes in
	// 60 game days (20 hours of wall clock at a 20-minute day), and the sun at
	// 52N must still reach both annual extremes: +61.44 at midsummer noon and
	// -61.44 at midwinter midnight. A mapping that merely scaled the date would
	// sample a 60-day slice and top out near whatever declination it started in.
	//
	// The sample step is one 200th of a game day. Altitude near an extremum is
	// second-order flat -- 0.5 deg of altitude corresponds to about 7 deg of
	// hour angle at this latitude -- so a step of 1.8 deg of hour angle resolves
	// the peak far more finely than the tolerance needs.
	{
		const VoxelSky::FGeoCoord Geo{ kSiteLatitudeDeg, 0.0 };
		constexpr int32 kSamplesPerDay = 200;
		const int32 NumSamples = static_cast<int32>(kCompressedDaysPerYear) * kSamplesPerDay;

		double MaxAltitude = -1000.0;
		double MinAltitude =  1000.0;
		for (int32 i = 0; i < NumSamples; ++i)
		{
			const double EpochSeconds = (static_cast<double>(i) / kSamplesPerDay) * kDayLengthSeconds;
			const double JulianDay = VoxelSky::JulianDayFromGameClock(
				EpochSeconds, kDayLengthSeconds, kCompressedDaysPerYear);
			const double Altitude = VoxelSky::ComputeSun(JulianDay, Geo).AltitudeDeg;
			MaxAltitude = FMath::Max(MaxAltitude, Altitude);
			MinAltitude = FMath::Min(MinAltitude, Altitude);
		}

		ExpectNear(TEXT("compressed year: annual maximum altitude at 52N"),
			MaxAltitude, kSummerNoonAltitudeDeg, kToleranceDeg);
		ExpectNear(TEXT("compressed year: annual minimum altitude at 52N"),
			MinAltitude, -kSummerNoonAltitudeDeg, kToleranceDeg);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSkyGeoMappingTest,
	"VoxelEarth.Sky.GeoMapping", kTestFlags)

bool FVoxelSkyGeoMappingTest::RunTest(const FString& Parameters)
{
	auto ExpectNear = [this](const TCHAR* Label, double Actual, double Expected, double Tolerance)
	{
		if (!(FMath::Abs(Actual - Expected) <= Tolerance))
		{
			AddError(FString::Printf(TEXT("%s: got %.9f, expected %.9f +/- %g"),
				Label, Actual, Expected, Tolerance));
		}
	};

	constexpr double kOriginLatDeg = 52.0;
	constexpr double kOriginLonDeg = -1.5;
	// One degree of latitude in Unreal units: 111320 m * 100 cm/m.
	constexpr double kUUPerLatDegree = 111320.0 * 100.0;

	// The origin is the origin. Trivial, and the first thing to break if anyone
	// ever "helpfully" recentres the mapping.
	{
		const VoxelSky::FGeoCoord Geo = VoxelSky::GeoFromWorldUU(0.0, 0.0, kOriginLatDeg, kOriginLonDeg);
		ExpectNear(TEXT("origin latitude"),  Geo.LatitudeDeg,  kOriginLatDeg, 1.0e-9);
		ExpectNear(TEXT("origin longitude"), Geo.LongitudeDeg, kOriginLonDeg, 1.0e-9);
	}

	// +Y is NORTH. One degree of latitude per 111320 m, and longitude untouched.
	{
		const VoxelSky::FGeoCoord Geo = VoxelSky::GeoFromWorldUU(0.0, kUUPerLatDegree, kOriginLatDeg, kOriginLonDeg);
		ExpectNear(TEXT("+Y moves one degree north"), Geo.LatitudeDeg, kOriginLatDeg + 1.0, 1.0e-9);
		ExpectNear(TEXT("+Y does not move longitude"), Geo.LongitudeDeg, kOriginLonDeg, 1.0e-9);

		const VoxelSky::FGeoCoord South = VoxelSky::GeoFromWorldUU(0.0, -kUUPerLatDegree, kOriginLatDeg, kOriginLonDeg);
		ExpectNear(TEXT("-Y moves one degree south"), South.LatitudeDeg, kOriginLatDeg - 1.0, 1.0e-9);
	}

	// +X is EAST, and a degree of longitude is shorter than a degree of latitude
	// by cos(latitude). At 52N that is 1/cos(52) = 1.6243 degrees for the same
	// distance. This is the assertion that catches an X/Y swap in the mapping,
	// which nothing else here would notice.
	{
		const VoxelSky::FGeoCoord Geo = VoxelSky::GeoFromWorldUU(kUUPerLatDegree, 0.0, kOriginLatDeg, kOriginLonDeg);
		const double ExpectedDeltaLon = 1.0 / FMath::Cos(kOriginLatDeg * kDegToRad);
		ExpectNear(TEXT("+X moves east by 1/cos(lat) degrees"),
			Geo.LongitudeDeg, kOriginLonDeg + ExpectedDeltaLon, 1.0e-9);
		ExpectNear(TEXT("+X does not move latitude"), Geo.LatitudeDeg, kOriginLatDeg, 1.0e-9);
		TestTrue(TEXT("a degree of longitude is shorter than a degree of latitude at 52N"),
			ExpectedDeltaLon > 1.0);
	}

	// Latitude CLAMPS at the poles rather than wrapping over them. Walking north
	// forever on a flat world must keep meaning north; wrapping would flip the
	// longitude by 180 degrees and reverse the direction of travel.
	{
		const VoxelSky::FGeoCoord North = VoxelSky::GeoFromWorldUU(0.0, 1.0e15, 0.0, 0.0);
		const VoxelSky::FGeoCoord South = VoxelSky::GeoFromWorldUU(0.0, -1.0e15, 0.0, 0.0);
		ExpectNear(TEXT("latitude clamps at +90"), North.LatitudeDeg,  90.0, 1.0e-12);
		ExpectNear(TEXT("latitude clamps at -90"), South.LatitudeDeg, -90.0, 1.0e-12);

		// And it clamps from a starting latitude that is already near the pole.
		const VoxelSky::FGeoCoord FromNearPole = VoxelSky::GeoFromWorldUU(0.0, 10.0 * kUUPerLatDegree, 89.0, 0.0);
		ExpectNear(TEXT("latitude clamps when the origin is already near the pole"),
			FromNearPole.LatitudeDeg, 90.0, 1.0e-12);
	}

	// Longitude must not blow up near the poles. cos(90) is 6e-17, so an
	// unguarded division produces ~1e16 degrees and then a NaN sun. The guard
	// floors the cosine, so the answer is large-but-finite and wrapped.
	{
		const VoxelSky::FGeoCoord AtPole = VoxelSky::GeoFromWorldUU(1.0e9, 1.0e15, 0.0, 0.0);
		TestTrue(TEXT("longitude at the pole is finite"), FMath::IsFinite(AtPole.LongitudeDeg));
		TestTrue(TEXT("longitude at the pole is wrapped into [-180, 180]"),
			AtPole.LongitudeDeg >= -180.0 && AtPole.LongitudeDeg <= 180.0);

		const VoxelSky::FGeoCoord NearPole = VoxelSky::GeoFromWorldUU(1.0e9, 0.0, 89.999, 0.0);
		TestTrue(TEXT("longitude just short of the pole is finite"), FMath::IsFinite(NearPole.LongitudeDeg));
		TestTrue(TEXT("longitude just short of the pole is wrapped into [-180, 180]"),
			NearPole.LongitudeDeg >= -180.0 && NearPole.LongitudeDeg <= 180.0);

		// The reason the guard exists: whatever comes out of the pole has to be
		// usable as a sun input without producing a NaN light direction.
		const double JulianDay = VoxelSky::JulianDayFromGameClock(
			EpochSecondsFor(kDayIndexJune21, 0.5), kDayLengthSeconds, kDaysPerYear);
		const VoxelSky::FSunState Sun = VoxelSky::ComputeSun(JulianDay, AtPole);
		TestTrue(TEXT("polar sun altitude is finite"), FMath::IsFinite(Sun.AltitudeDeg));
		TestTrue(TEXT("polar sun azimuth is finite"), FMath::IsFinite(Sun.AzimuthDeg));
		TestTrue(TEXT("polar sun direction contains no NaN"), !Sun.Direction.ContainsNaN());
		TestTrue(TEXT("polar sun direction is unit length"), Sun.Direction.IsNormalized());
		// 21 June at the north pole: the sun is up, at an altitude equal to the
		// declination. A degenerate-azimuth guard that returned garbage instead
		// of a finite value would still pass the NaN checks above but not this.
		ExpectNear(TEXT("north pole on the June solstice sees the sun at the declination"),
			Sun.AltitudeDeg, kObliquityDeg, kToleranceDeg);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSkyMoonPhaseTest,
	"VoxelEarth.Sky.MoonPhase", kTestFlags)

bool FVoxelSkyMoonPhaseTest::RunTest(const FString& Parameters)
{
	// SCOPE NOTE, deliberately narrow. ComputeMoon is a circular-orbit
	// approximation whose ecliptic longitude can be off by more than a degree,
	// so this test asserts NOTHING about where the moon is against a published
	// position -- doing so would either fail or force a tolerance so wide it
	// proved nothing. What it asserts is what the approximation genuinely
	// guarantees and what consumers genuinely rely on: the outputs stay in
	// range, the phase and the geometry agree with each other, and the synodic
	// period is right. The 0.5-degree bar belongs to the sun.
	auto ExpectNear = [this](const TCHAR* Label, double Actual, double Expected, double Tolerance)
	{
		if (!(FMath::Abs(Actual - Expected) <= Tolerance))
		{
			AddError(FString::Printf(TEXT("%s: got %.6f, expected %.6f +/- %g"),
				Label, Actual, Expected, Tolerance));
		}
	};

	const VoxelSky::FGeoCoord Geo{ 52.0, 0.0 };

	// A little over one synodic month (29.53 days), sampled every 15 simulated
	// minutes, so both a full and a new moon are certain to fall inside the
	// sweep and to be resolved finely enough to assert on.
	constexpr int32 kDaysSwept      = 40;
	constexpr int32 kSamplesPerDay  = 96;

	int32 BestFullIndex = INDEX_NONE;
	int32 BestNewIndex  = INDEX_NONE;
	double BestFullError = 1000.0;
	double BestNewError  = 1000.0;
	double MinIlluminated = 2.0;
	double MaxIlluminated = -1.0;
	bool bAllRangesOk = true;
	bool bAllUnitLength = true;

	auto SampleAt = [&](int32 SampleIndex, VoxelSky::FSunState& OutSun) -> VoxelSky::FMoonState
	{
		const double EpochSeconds = (static_cast<double>(SampleIndex) / kSamplesPerDay) * kDayLengthSeconds;
		const double JulianDay = VoxelSky::JulianDayFromGameClock(
			EpochSeconds, kDayLengthSeconds, kDaysPerYear);
		OutSun = VoxelSky::ComputeSun(JulianDay, Geo);
		return VoxelSky::ComputeMoon(JulianDay, Geo, OutSun);
	};

	const int32 NumSamples = kDaysSwept * kSamplesPerDay;
	for (int32 i = 0; i < NumSamples; ++i)
	{
		VoxelSky::FSunState Sun;
		const VoxelSky::FMoonState Moon = SampleAt(i, Sun);

		if (!(Moon.PhaseFraction >= 0.0 && Moon.PhaseFraction <= 1.0)) { bAllRangesOk = false; }
		if (!(Moon.IlluminatedFraction >= 0.0 && Moon.IlluminatedFraction <= 1.0)) { bAllRangesOk = false; }
		if (!(Moon.AltitudeDeg >= -91.0 && Moon.AltitudeDeg <= 91.0)) { bAllRangesOk = false; }
		if (!(Moon.AzimuthDeg >= 0.0 && Moon.AzimuthDeg < 360.0)) { bAllRangesOk = false; }
		if (!Moon.Direction.IsNormalized()) { bAllUnitLength = false; }

		MinIlluminated = FMath::Min(MinIlluminated, Moon.IlluminatedFraction);
		MaxIlluminated = FMath::Max(MaxIlluminated, Moon.IlluminatedFraction);

		// Distance to full (phase 0.5) and to new (phase 0 == phase 1), the
		// latter measured around the wrap.
		const double FullError = FMath::Abs(Moon.PhaseFraction - 0.5);
		const double NewError  = FMath::Min(Moon.PhaseFraction, 1.0 - Moon.PhaseFraction);
		if (FullError < BestFullError) { BestFullError = FullError; BestFullIndex = i; }
		if (NewError  < BestNewError)  { BestNewError  = NewError;  BestNewIndex  = i; }
	}

	TestTrue(TEXT("phase, illumination, altitude and azimuth stay in range all month"), bAllRangesOk);
	TestTrue(TEXT("moon direction is unit length at every sample"), bAllUnitLength);
	TestTrue(TEXT("a full moon occurs within the swept month"), BestFullIndex != INDEX_NONE && BestFullError < 0.01);
	TestTrue(TEXT("a new moon occurs within the swept month"),  BestNewIndex  != INDEX_NONE && BestNewError  < 0.01);

	// The cycle really is a cycle: illumination reaches both ends of its range.
	TestTrue(TEXT("illumination reaches near zero at new moon"), MinIlluminated < 0.02);
	TestTrue(TEXT("illumination reaches near one at full moon"), MaxIlluminated > 0.98);

	// AT FULL: the moon is roughly OPPOSITE the sun. Roughly, not exactly --
	// the lunar orbit is inclined 5.145 degrees, so a full moon can sit up to
	// that far off the anti-solar point (which is precisely why there is not an
	// eclipse every month). -0.95 is about 162 degrees of separation, which
	// admits the inclination and the model's own error without admitting a
	// wrong quadrant.
	if (BestFullIndex != INDEX_NONE)
	{
		VoxelSky::FSunState Sun;
		const VoxelSky::FMoonState Moon = SampleAt(BestFullIndex, Sun);
		const double Dot = Sun.Direction | Moon.Direction;
		TestTrue(TEXT("full moon sits opposite the sun"), Dot < -0.95);
		ExpectNear(TEXT("full moon is fully illuminated"), Moon.IlluminatedFraction, 1.0, 0.05);
	}

	// AT NEW: the moon is in the same direction as the sun, and dark.
	if (BestNewIndex != INDEX_NONE)
	{
		VoxelSky::FSunState Sun;
		const VoxelSky::FMoonState Moon = SampleAt(BestNewIndex, Sun);
		const double Dot = Sun.Direction | Moon.Direction;
		TestTrue(TEXT("new moon sits alongside the sun"), Dot > 0.95);
		ExpectNear(TEXT("new moon is unilluminated"), Moon.IlluminatedFraction, 0.0, 0.05);
	}

	// THE SYNODIC PERIOD. Phase must repeat after 29.530589 days -- the mean
	// synodic month -- and that is a genuinely independent check on the two
	// mean motions: it comes out right only if the lunar rate (13.176396 deg/day)
	// and the solar rate (0.985647 deg/day) differ by exactly 360/29.530589.
	// Getting either rate wrong, or reusing one for the other, breaks it.
	//
	// The residual is not zero because the SUN's longitude does not advance
	// uniformly (the equation of centre, up to 1.9 degrees), so the true
	// interval between identical phases breathes by a few tenths of a degree.
	// 0.01 cycles is 3.6 degrees of elongation, comfortably above the ~0.003
	// this actually produces and far below anything a wrong rate would give.
	{
		constexpr double kSynodicMonthDays = 29.530589;
		double WorstDrift = 0.0;
		for (int32 Day = 0; Day < 120; ++Day)
		{
			const double EpochA = (static_cast<double>(Day) + 0.5) * kDayLengthSeconds;
			const double EpochB = EpochA + kSynodicMonthDays * kDayLengthSeconds;

			VoxelSky::FSunState SunA = VoxelSky::ComputeSun(
				VoxelSky::JulianDayFromGameClock(EpochA, kDayLengthSeconds, kDaysPerYear), Geo);
			VoxelSky::FSunState SunB = VoxelSky::ComputeSun(
				VoxelSky::JulianDayFromGameClock(EpochB, kDayLengthSeconds, kDaysPerYear), Geo);

			const double PhaseA = VoxelSky::ComputeMoon(
				VoxelSky::JulianDayFromGameClock(EpochA, kDayLengthSeconds, kDaysPerYear), Geo, SunA).PhaseFraction;
			const double PhaseB = VoxelSky::ComputeMoon(
				VoxelSky::JulianDayFromGameClock(EpochB, kDayLengthSeconds, kDaysPerYear), Geo, SunB).PhaseFraction;

			// Wrap-aware: phases 0.999 and 0.001 are two thousandths apart.
			WorstDrift = FMath::Max(WorstDrift, AngleErrorDeg(PhaseA * 360.0, PhaseB * 360.0) / 360.0);
		}
		ExpectNear(TEXT("phase repeats after one synodic month"), WorstDrift, 0.0, 0.01);
	}

	// A default-constructed FSunState must not silently produce a permanent
	// first-quarter moon: the zero vector dotted with anything is zero, which
	// (1 - cos E)/2 reads as an elongation of exactly 90 degrees. The
	// implementation falls back to the geocentric elongation instead, so the
	// full moon found above must still read as full when the sun is omitted.
	if (BestFullIndex != INDEX_NONE)
	{
		const double EpochSeconds = (static_cast<double>(BestFullIndex) / kSamplesPerDay) * kDayLengthSeconds;
		const double JulianDay = VoxelSky::JulianDayFromGameClock(
			EpochSeconds, kDayLengthSeconds, kDaysPerYear);
		const VoxelSky::FMoonState Moon = VoxelSky::ComputeMoon(JulianDay, Geo, VoxelSky::FSunState());
		TestTrue(TEXT("full moon is still full when no sun state is supplied"),
			Moon.IlluminatedFraction > 0.98);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
