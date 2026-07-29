#include "VoxelEphemeris.h"

namespace VoxelSky
{
namespace
{
	// Conversion factors as explicit double constants.
	//
	// FMath::DegreesToRadians does have a correct double overload (it uses
	// UE_DOUBLE_PI), so this is not working around a defect. It is here because
	// that overload sits alongside a TEMPLATE fallback which multiplies by the
	// float-typed UE_PI / 180.f, and which is what gets selected for anything
	// that is not exactly float or double -- a long double, an integer literal,
	// or whatever a future refactor passes in. This file's entire job is not
	// losing the low bits of a Julian century, so its conversions are spelled
	// out and cannot silently degrade to seven significant digits.
	constexpr double kPi       = 3.14159265358979323846;
	constexpr double kDegToRad = kPi / 180.0;
	constexpr double kRadToDeg = 180.0 / kPi;

	// JD of 2000-01-01 00:00 UT. Note this is NOT the J2000.0 epoch itself:
	// J2000.0 is JD 2451545.0 = 2000-01-01 12:00 TT, half a day later. The
	// series coefficients below count centuries from 2451545.0; the calendar
	// mapping counts days from 2451544.5. Conflating the two is a 12-hour error
	// that shows up as the sun being on the wrong side of the sky, so both
	// constants exist by name and neither is ever spelled inline.
	constexpr double kJD2000Jan1_0hUT = 2451544.5;
	constexpr double kJ2000Epoch      = 2451545.0;

	// Mean tropical year. This is the divisor that turns a compressed game year
	// into a full real declination cycle; see JulianDayFromGameClock.
	constexpr double kTropicalYearDays = 365.2425;

	// Fraction of X above its floor, always in [0, 1).
	//
	// NOT FMath::Frac. FMath::Frac truncates toward zero, so Frac(-0.25) is
	// -0.25 and a negative WorldEpochSeconds -- which is a perfectly ordinary
	// thing for a test or a rewound clock to produce -- would put the sun on the
	// wrong side of midnight. FloorToDouble gives the periodic answer.
	FORCEINLINE double Wrap01(double X)
	{
		return X - FMath::FloorToDouble(X);
	}

	// Degrees into [0, 360). Half-open, and the second branch is what keeps it
	// half-open: for a tiny negative input Fmod returns that same tiny negative,
	// and adding 360 rounds to exactly 360.0. An azimuth of 360 is not wrong,
	// but it is out of the stated range and it is the sort of thing a consumer
	// bucketing azimuths into 8 compass sectors indexes an array with.
	FORCEINLINE double WrapDeg360(double Deg)
	{
		double R = FMath::Fmod(Deg, 360.0);
		if (R < 0.0) { R += 360.0; }
		return R >= 360.0 ? 0.0 : R;
	}

	// Degrees into [-180, 180]. Hour angles must live here before their sign is
	// used to pick an azimuth branch, or "just past midnight" reads as "just
	// before noon".
	FORCEINLINE double WrapDeg180(double Deg)
	{
		const double R = WrapDeg360(Deg);
		return R > 180.0 ? R - 360.0 : R;
	}

	FORCEINLINE double SinD(double Deg) { return FMath::Sin(Deg * kDegToRad); }
	FORCEINLINE double CosD(double Deg) { return FMath::Cos(Deg * kDegToRad); }
	FORCEINLINE double TanD(double Deg) { return FMath::Tan(Deg * kDegToRad); }
	FORCEINLINE double AsinD(double X)  { return FMath::Asin(FMath::Clamp(X, -1.0, 1.0)) * kRadToDeg; }
	FORCEINLINE double AcosD(double X)  { return FMath::Acos(FMath::Clamp(X, -1.0, 1.0)) * kRadToDeg; }

	// Everything the solar series produces that anything downstream wants. The
	// moon needs the sun's apparent longitude and the obliquity, so this is
	// factored out rather than being buried inside ComputeSun -- recomputing it
	// there with a second copy of the coefficients is exactly how the two halves
	// of a sky drift apart.
	struct FSolarElements
	{
		double JulianCentury    = 0.0; // T, centuries from J2000.0
		double MeanLongitudeDeg = 0.0; // L0
		double MeanAnomalyDeg   = 0.0; // M
		double ApparentLongDeg  = 0.0; // lambda, aberration + nutation applied
		double ObliquityDeg     = 0.0; // epsilon, nutation term applied
		double DeclinationDeg   = 0.0;
		double EqTimeMinutes    = 0.0; // apparent solar time minus mean solar time
	};

	FSolarElements ComputeSolarElements(double JulianDay)
	{
		FSolarElements S;

		const double T = (JulianDay - kJ2000Epoch) / 36525.0;
		S.JulianCentury = T;

		// Meeus 25.2 / 25.3: geometric mean longitude and mean anomaly.
		S.MeanLongitudeDeg = WrapDeg360(280.46646 + T * (36000.76983 + T * 0.0003032));
		S.MeanAnomalyDeg   = 357.52911 + T * (35999.05029 - T * 0.0001537);

		// Eccentricity of Earth's orbit (Meeus 25.4). Feeds both the equation of
		// centre and the equation of time.
		const double Ecc = 0.016708634 - T * (0.000042037 + T * 0.0000001267);

		const double M = S.MeanAnomalyDeg;
		const double EqCentre = SinD(M)        * (1.914602 - T * (0.004817 + T * 0.000014))
		                      + SinD(2.0 * M)  * (0.019993 - T * 0.000101)
		                      + SinD(3.0 * M)  *  0.000289;

		const double TrueLongDeg = S.MeanLongitudeDeg + EqCentre;

		// Apparent longitude: -0.00569 is aberration (light takes 8 minutes to
		// arrive and the Earth has moved), -0.00478*sin(Omega) is nutation in
		// longitude, where Omega is the longitude of the ascending node of the
		// Moon's orbit and 1934.136 deg/century is its 18.6-year regression.
		const double OmegaDeg = 125.04 - 1934.136 * T;
		// Wrapped, because the moon subtracts this to get an elongation and a
		// field that is sometimes 361 degrees is a trap waiting for a reader who
		// assumes the range the name implies.
		S.ApparentLongDeg = WrapDeg360(TrueLongDeg - 0.00569 - 0.00478 * SinD(OmegaDeg));

		// Mean obliquity (Meeus 22.2) written out in degrees/arcmin/arcsec, plus
		// the nutation-in-obliquity term. The nutation term is only 0.00256
		// degrees and it is tempting to drop it -- keep it: it is free, it is
		// what makes this agree with published tables rather than merely being
		// close to them, and its absence is invisible until someone tries to
		// tighten the tolerance below 0.5 degrees and cannot work out why.
		const double MeanOblDeg = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
		S.ObliquityDeg = MeanOblDeg + 0.00256 * CosD(OmegaDeg);

		S.DeclinationDeg = AsinD(SinD(S.ObliquityDeg) * SinD(S.ApparentLongDeg));

		// Equation of time (Meeus 28.3, small-angle form). y = tan^2(eps/2).
		// Result is in MINUTES of time: the bracket is radians, radians->degrees,
		// and 4 minutes of time per degree of Earth rotation.
		const double Y = TanD(0.5 * S.ObliquityDeg) * TanD(0.5 * S.ObliquityDeg);
		const double L0 = S.MeanLongitudeDeg;
		const double EqTimeRad =
			  Y * SinD(2.0 * L0)
			- 2.0 * Ecc * SinD(M)
			+ 4.0 * Ecc * Y * SinD(M) * CosD(2.0 * L0)
			- 0.5 * Y * Y * SinD(4.0 * L0)
			- 1.25 * Ecc * Ecc * SinD(2.0 * M);
		S.EqTimeMinutes = 4.0 * EqTimeRad * kRadToDeg;

		return S;
	}

	// Atmospheric refraction, NOAA's piecewise fit, taking the GEOMETRIC
	// altitude and returning the amount to ADD to it in degrees.
	//
	// This is what puts the sun visibly above the horizon when it is
	// geometrically below it: 0.57 degrees of lift at the horizon, which is
	// slightly more than the solar disc's own diameter, so sunrise happens while
	// the sun is entirely below the geometric horizon. Omitting it costs about
	// two minutes at each end of the day and, more to the point here, half a
	// degree of altitude error exactly at the low-sun checkpoints the test bar
	// is written against.
	double RefractionDeg(double GeometricAltDeg)
	{
		if (GeometricAltDeg > 85.0)
		{
			// Under a tenth of an arcsecond up here, and the tan-based branch
			// below is numerically noisy near the zenith. Return zero rather
			// than a rounding artefact.
			return 0.0;
		}

		const double Te = TanD(GeometricAltDeg);
		double ArcSeconds;
		if (GeometricAltDeg > 5.0)
		{
			ArcSeconds = 58.1 / Te - 0.07 / (Te * Te * Te) + 0.000086 / (Te * Te * Te * Te * Te);
		}
		else if (GeometricAltDeg > -0.575)
		{
			// The tan form diverges through the horizon, so this window uses a
			// quartic in the altitude itself. The two branches agree to within
			// an arcsecond at 5 degrees and match exactly at -0.575.
			const double E = GeometricAltDeg;
			ArcSeconds = 1735.0 + E * (-518.2 + E * (103.4 + E * (-12.79 + E * 0.711)));
		}
		else
		{
			// Well below the horizon: refraction is decaying back toward zero
			// and nothing is rendered here anyway, but it must stay finite and
			// continuous so a light rig that interpolates through night does not
			// see a step.
			ArcSeconds = -20.772 / Te;
		}

		return ArcSeconds / 3600.0;
	}

	struct FHorizontal
	{
		double AltitudeDeg = 0.0;
		double AzimuthDeg  = 0.0;
	};

	// Equatorial (hour angle, declination) -> horizon (altitude, azimuth
	// clockwise from north), for an observer at LatitudeDeg. HourAngleDeg must
	// already be wrapped into [-180, 180]: negative is before transit (east of
	// the meridian), positive is after (west).
	//
	// Shared by the sun and the moon deliberately. The two get their hour angles
	// by different routes -- the sun through the equation of time, the moon
	// through Greenwich mean sidereal time and a right ascension -- and if they
	// also had separate horizon transforms, a sign slip in one would show up as
	// a moon that is somehow not opposite the sun at full, which is a miserable
	// thing to localise.
	FHorizontal HorizontalFromHourAngle(double HourAngleDeg, double DeclinationDeg, double LatitudeDeg)
	{
		const double CosZenith = FMath::Clamp(
			SinD(LatitudeDeg) * SinD(DeclinationDeg)
			+ CosD(LatitudeDeg) * CosD(DeclinationDeg) * CosD(HourAngleDeg), -1.0, 1.0);
		const double ZenithDeg = AcosD(CosZenith);

		FHorizontal Out;
		Out.AltitudeDeg = 90.0 - ZenithDeg;

		const double AzDenom = CosD(LatitudeDeg) * SinD(ZenithDeg);
		if (FMath::Abs(AzDenom) < 1.0e-12)
		{
			// Degenerate: the observer is at a pole, or the body is exactly at
			// the zenith. Azimuth is genuinely undefined in both cases (at the
			// pole every direction is south; at the zenith there is no
			// direction at all). Return the hour-angle-based value so the
			// result stays finite and continuous rather than NaN -- a NaN here
			// propagates into the light rotation and blanks the frame.
			Out.AzimuthDeg = WrapDeg360(180.0 - HourAngleDeg);
			return Out;
		}

		// The clamp is load-bearing, not defensive dressing: at transit the
		// numerator and denominator are equal to within rounding and the ratio
		// lands at 1.0000000002, which Acos answers with a NaN. Solar noon is
		// the single most likely moment for anyone to be looking at this.
		const double AzRatio = FMath::Clamp(
			(SinD(LatitudeDeg) * CosZenith - SinD(DeclinationDeg)) / AzDenom, -1.0, 1.0);
		const double AzAcosDeg = AcosD(AzRatio);

		// Acos loses the east/west sign; the hour angle carries it back.
		Out.AzimuthDeg = (HourAngleDeg > 0.0)
			? WrapDeg360(AzAcosDeg + 180.0)
			: WrapDeg360(540.0 - AzAcosDeg);
		return Out;
	}

	// (altitude, azimuth-clockwise-from-north) -> UE world direction, X north,
	// Y east, Z up. Unit length by construction.
	FORCEINLINE FVector DirectionFromAltAz(double AltitudeDeg, double AzimuthDeg)
	{
		const double CosAlt = CosD(AltitudeDeg);
		return FVector(CosAlt * CosD(AzimuthDeg), CosAlt * SinD(AzimuthDeg), SinD(AltitudeDeg));
	}

	// Greenwich mean sidereal time in degrees (Meeus 12.4). The moon's hour
	// angle comes from here rather than from the equation of time, because a
	// right ascension is what the ecliptic-to-equatorial transform naturally
	// produces and converting it into a "solar time" first would be a detour
	// through a quantity that only means something for the sun.
	double GreenwichMeanSiderealTimeDeg(double JulianDay)
	{
		const double D = JulianDay - kJ2000Epoch;
		const double T = D / 36525.0;
		return WrapDeg360(280.46061837 + 360.98564736629 * D
			+ 0.000387933 * T * T - (T * T * T) / 38710000.0);
	}
}

FGeoCoord GeoFromWorldUU(double WorldXUU, double WorldYUU,
                         double OriginLatitudeDeg, double OriginLongitudeDeg)
{
	// Unreal units are centimetres. 111320 m per degree is the WGS84 mean
	// meridional degree; using one constant for both axes (with the cosine
	// factor on longitude) is the standard equirectangular tangent-plane
	// approximation and is exact enough for a world that has no true geodesy
	// underneath it anyway.
	constexpr double kUUPerMetre        = 100.0;
	constexpr double kMetresPerLatDeg   = 111320.0;
	// cos(89.94 degrees). The floor bounds a degree of longitude at ~194 m, so
	// walking 10 km east at the pole moves you ~51 degrees of longitude instead
	// of infinitely far. Arbitrary, but it has to be SOMETHING: the alternative
	// is a division by zero whose NaN reaches the sun's hour angle and blanks
	// the sky. Chosen well outside any latitude a player will stand at.
	constexpr double kMinCosLatitude    = 1.0e-3;

	FGeoCoord Out;

	// +Y is north, so latitude is the Y offset. Clamped rather than wrapped:
	// crossing the pole would flip the longitude by 180 degrees and reverse the
	// direction of travel, which is correct geodesy and utterly wrong for a
	// flat world where walking north forever should keep meaning "north".
	Out.LatitudeDeg = FMath::Clamp(
		OriginLatitudeDeg + (WorldYUU / kUUPerMetre) / kMetresPerLatDeg, -90.0, 90.0);

	// The cosine uses the RESULTING latitude, not the origin latitude, so that
	// a long north-south traverse narrows the longitude scale as it should.
	const double CosLat = FMath::Max(FMath::Abs(CosD(Out.LatitudeDeg)), kMinCosLatitude);
	Out.LongitudeDeg = WrapDeg180(
		OriginLongitudeDeg + (WorldXUU / kUUPerMetre) / (kMetresPerLatDeg * CosLat));

	return Out;
}

double JulianDayFromGameClock(double WorldEpochSeconds, double DayLengthSeconds, double DaysPerYear)
{
	// A zero or negative day length is a misconfigured cvar, not a request for
	// a division by zero. Floor both inputs to something the maths survives; a
	// wrong-but-finite sky is recoverable, a NaN one is not.
	const double SafeDayLength  = FMath::Max(DayLengthSeconds, 1.0e-6);
	const double SafeDaysPerYear = FMath::Max(DaysPerYear, 1.0e-3);

	const double DayFraction  = Wrap01(WorldEpochSeconds / SafeDayLength);
	const double YearFraction = Wrap01(WorldEpochSeconds / (SafeDayLength * SafeDaysPerYear));
	const double RealDayOfYear = YearFraction * kTropicalYearDays;

	// floor() on the date and the raw fraction on the time of day. See the
	// header: this is what keeps exactly one diurnal cycle per game day while
	// the date runs ahead at its own rate.
	return kJD2000Jan1_0hUT + FMath::FloorToDouble(RealDayOfYear) + DayFraction;
}

FSunState ComputeSun(double JulianDay, const FGeoCoord& Geo)
{
	const FSolarElements S = ComputeSolarElements(JulianDay);

	// UTC time of day straight out of the Julian Day. The +0.5 is because a
	// Julian Day begins at NOON, not midnight -- JD 2451544.5 is 2000-01-01
	// 00:00 UT, and its fractional part is 0.5, not 0.0.
	const double UtcMinutes = Wrap01(JulianDay + 0.5) * 1440.0;

	// True solar time in minutes: mean time, corrected by the equation of time,
	// shifted by 4 minutes per degree of east longitude. 720 minutes of true
	// solar time is local apparent noon by definition.
	const double TrueSolarTimeMinutes = UtcMinutes + S.EqTimeMinutes + 4.0 * Geo.LongitudeDeg;
	const double HourAngleDeg = WrapDeg180(TrueSolarTimeMinutes / 4.0 - 180.0);

	const FHorizontal H = HorizontalFromHourAngle(HourAngleDeg, S.DeclinationDeg, Geo.LatitudeDeg);

	FSunState Out;
	Out.AltitudeDeg = H.AltitudeDeg + RefractionDeg(H.AltitudeDeg);
	Out.AzimuthDeg  = H.AzimuthDeg;
	// Direction uses the APPARENT altitude, so that "the sun is on the horizon"
	// and "Direction.Z is zero" happen at the same instant. A shadow-caster set
	// from the geometric altitude and a sky tint set from the apparent one
	// disagree for about two minutes at each end of the day, which reads as a
	// flicker at exactly the moment the lighting is most conspicuous.
	Out.Direction = DirectionFromAltAz(Out.AltitudeDeg, Out.AzimuthDeg);
	return Out;
}

FMoonState ComputeMoon(double JulianDay, const FGeoCoord& Geo, const FSunState& Sun)
{
	const double D = JulianDay - kJ2000Epoch;

	// Circular-orbit model -- see the header for why this is the right amount
	// of moon. Both rates are the J2000 mean motions expressed per day:
	// 481267.88123421 deg/century / 36525 = 13.176396 deg/day (sidereal month,
	// 27.32 days), and 483202.0175233 / 36525 = 13.229350 deg/day for the
	// argument of latitude (draconic month, 27.21 days). The two differ by the
	// 18.6-year nodal regression, which is why eclipses are not monthly and why
	// keeping F on its own rate rather than reusing the longitude matters.
	const double MoonLongDeg = WrapDeg360(218.3164477 + 13.176396 * D);
	const double ArgLatDeg   = WrapDeg360(93.2720950  + 13.229350 * D);

	// Single inclination term. The real series has a dozen latitude terms; this
	// one carries ~99% of the amplitude and is what decides whether the moon
	// rides high or low relative to the ecliptic.
	constexpr double kLunarInclinationDeg = 5.145;
	const double MoonLatDeg = kLunarInclinationDeg * SinD(ArgLatDeg);

	// Obliquity is shared with the sun rather than re-derived. Same epoch, same
	// number, one place to be wrong.
	const FSolarElements S = ComputeSolarElements(JulianDay);
	const double Obl = S.ObliquityDeg;

	// Ecliptic -> equatorial (Meeus 13.3, 13.4).
	const double DecDeg = AsinD(
		SinD(MoonLatDeg) * CosD(Obl) + CosD(MoonLatDeg) * SinD(Obl) * SinD(MoonLongDeg));
	const double RaDeg  = WrapDeg360(kRadToDeg * FMath::Atan2(
		SinD(MoonLongDeg) * CosD(Obl) - TanD(MoonLatDeg) * SinD(Obl),
		CosD(MoonLongDeg)));

	// Local hour angle = local sidereal time - right ascension.
	//
	// Routed through the EXPORTED LocalSiderealTimeDeg rather than calling
	// GreenwichMeanSiderealTimeDeg directly, so that the moon's hour angle and
	// M_NightSky's StarRotation are the same expression and not two copies of it
	// (see the header). The wrap this now applies is harmless here -- WrapDeg180
	// below wraps the difference anyway.
	const double LocalSiderealDeg = LocalSiderealTimeDeg(JulianDay, Geo);
	const double HourAngleDeg = WrapDeg180(LocalSiderealDeg - RaDeg);

	const FHorizontal H = HorizontalFromHourAngle(HourAngleDeg, DecDeg, Geo.LatitudeDeg);

	FMoonState Out;
	// Refracted for the same reason the sun is: moonrise should happen when the
	// moon looks like it has risen. The correction is far smaller than this
	// model's own longitude error, so it changes nothing that matters -- it is
	// here for consistency between the two bodies, not for accuracy.
	Out.AltitudeDeg = H.AltitudeDeg + RefractionDeg(H.AltitudeDeg);
	Out.AzimuthDeg  = H.AzimuthDeg;
	Out.Direction   = DirectionFromAltAz(Out.AltitudeDeg, Out.AzimuthDeg);

	// PHASE comes from the ecliptic longitude difference, because it must be
	// SIGNED: a waxing crescent and a waning crescent look identical to any
	// measure of illumination, and only the direction the moon is pulling away
	// from the sun tells them apart. Elongation 0 = new, 180 = full, so
	// dividing the wrapped difference by 360 gives 0 -> new, 0.5 -> full,
	// 1 -> new again exactly as the contract states.
	const double ElongationDeg = WrapDeg360(MoonLongDeg - S.ApparentLongDeg);
	Out.PhaseFraction = ElongationDeg / 360.0;

	// ILLUMINATION comes from the observed elongation -- the angle between the
	// caller's direction-to-sun and our direction-to-moon, both unit vectors in
	// the same local horizon frame. Geocentric elongation and topocentric
	// elongation differ by lunar parallax (up to ~1 degree), which is an order
	// of magnitude below this model's own error, and taking it from the actual
	// direction vectors guarantees the returned phase agrees with the returned
	// geometry: a caller that draws the terminator from Sun.Direction and
	// Moon.Direction cannot end up disagreeing with IlluminatedFraction.
	//
	// (1 - cos E) / 2 is the standard disc-illumination relation: E = 0 gives 0
	// (new), E = 180 gives 1 (full), and it is symmetric about full, which is
	// precisely why PhaseFraction above cannot be derived from it.
	double CosElongation;
	if (Sun.Direction.IsNormalized())
	{
		CosElongation = FMath::Clamp(Sun.Direction | Out.Direction, -1.0, 1.0);
	}
	else
	{
		// Caller passed a default-constructed FSunState. Fall back to the
		// geocentric elongation so the answer is still meaningful rather than
		// silently zero -- a default FVector dotted with anything is 0, which
		// would report a permanent first-quarter moon.
		CosElongation = FMath::Clamp(CosD(MoonLatDeg) * CosD(ElongationDeg), -1.0, 1.0);
	}
	Out.IlluminatedFraction = FMath::Clamp(0.5 * (1.0 - CosElongation), 0.0, 1.0);

	return Out;
}

double LocalSiderealTimeDeg(double JulianDay, const FGeoCoord& Geo)
{
	// Greenwich mean sidereal time plus EAST longitude. The sign is the one place
	// this can go wrong and it is fixed by FGeoCoord's own convention
	// (VoxelEphemeris.h:59-64): longitude is east-positive, and sidereal time runs
	// eastward with the observer, so a place further east reaches a given star's
	// meridian transit EARLIER in universal time -- hence plus, not minus. Getting
	// it backwards puts the star field at the antipodal longitude, which at this
	// project's default OriginLongitudeDeg of 0.0 is exactly zero degrees of
	// error and would ship unnoticed until someone moved the origin.
	//
	// Re-wrapped to [0, 360) because the sum can leave the range GMST was wrapped
	// into: the caller in ComputeMoon takes a difference and does not care, but
	// M_NightSky's StarRotation is this divided by 360 and a value outside [0,1)
	// only survives because the star map's U wraps. It is cheaper to be in range
	// than to depend on that.
	return WrapDeg360(GreenwichMeanSiderealTimeDeg(JulianDay) + Geo.LongitudeDeg);
}

} // namespace VoxelSky
