#include "VoxelSkySubsystem.h"

#include "VoxelEarth.h"
#include "VoxelEarthGameMode.h" // VoxelEarthSpawn::ParseSpawnColumnUU -- see SpawnRig
#include "VoxelEphemeris.h"
#include "VoxelWorldSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Scene.h" // FPostProcessSettings, EAutoExposureMethod
#include "Engine/SkyLight.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelSky, Log, All);

// --- cvars -----------------------------------------------------------------
//
// Naming follows this module's existing convention (voxel.<Area>.<Name>, see
// VoxelDebug.cpp). Declared here rather than in VoxelDebug.cpp purely as file
// ownership hygiene while several agents are live in this module -- the same
// note VoxelGI.cpp:31-35 carries, for the same reason.
//
// Every help string states the DEFAULT and WHY it is that default. A knob whose
// default is undocumented is a knob nobody dares move.

namespace
{
	TAutoConsoleVariable<int32> CVarSkyEnabled(
		TEXT("voxel.Sky.Enabled"), 1,
		TEXT("Day/night world clock drives the light rig. 1 = on (default). 0 = off, and genuinely ")
		TEXT("zero per-frame cost: UVoxelSkySubsystem::IsTickable goes false and the rig is left at ")
		TEXT("the fixed pose the pre-W4 static rig used, so switching this off does NOT switch the ")
		TEXT("lights off -- it freezes them. CLIENT-SIDE RENDERING ONLY, outside the determinism ")
		TEXT("boundary."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSkyTimeScale(
		TEXT("voxel.Sky.TimeScale"), 1.0f,
		TEXT("Multiplier on the world clock's advance rate. Default 1.0 = one game day per ")
		TEXT("voxel.Sky.DayLengthSeconds of wall clock. 0 FREEZES the clock at wherever it is, which ")
		TEXT("is what a reproducible capture wants (the sun must not drift between the settle wait ")
		TEXT("and the shutter). Negative runs the day backwards; nothing forbids it and it is ")
		TEXT("occasionally the fastest way to walk back to a pose you just passed."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSkyDayLengthSeconds(
		TEXT("voxel.Sky.DayLengthSeconds"), 1200.0f,
		TEXT("Wall-clock seconds per game day. Default 1200 = a 20-minute day, which is the ")
		TEXT("shortest cycle that still leaves a recognisable dawn and dusk (~50 s each at 52 N) ")
		TEXT("rather than a strobe, and short enough that a play session sees several. This is the ")
		TEXT("DIURNAL clock only; the seasonal one is DaysPerYear below and the two are ")
		TEXT("deliberately decoupled -- see VoxelEphemeris.h's JulianDayFromGameClock comment."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSkyDaysPerYear(
		TEXT("voxel.Sky.DaysPerYear"), 48.0f,
		TEXT("Game days per game year. Default 48, i.e. a season per ~4 hours of play at the ")
		TEXT("default day length (48 * 1200 s = 16 h per year, /4 = 4 h per season). NOTE the ")
		TEXT("consequence VoxelEphemeris.h:130-138 spells out: the solar declination moves ")
		TEXT("VISIBLY WITHIN one game day at this compression (365.2425/48 = 7.6 real days per ")
		TEXT("game day), so the sun rises at a different point on the horizon than it set at. ")
		TEXT("That is the point of a compressed year, not a wrapping bug."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSkyOriginLatitudeDeg(
		TEXT("voxel.Sky.OriginLatitudeDeg"), 52.0f,
		TEXT("Latitude the world ORIGIN is declared to sit at, degrees north. Default 52.0, and ")
		TEXT("this is NOT an arbitrary pick: VoxelClimateProbe.h:52-60 measures this world's own ")
		TEXT("climate window at -8.6..+19.3 degC and 659..1506 mm/yr and names it COOL-TEMPERATE ")
		TEXT("MARITIME, which is 50-55 N (northern Europe / southern England). A latitude that ")
		TEXT("contradicts the terrain is not a cosmetic mismatch -- it puts the snow line, the ")
		TEXT("daylight hours and the biome the ground was generated for at odds with each other, ")
		TEXT("and every one of those disagreements reads as a worldgen bug rather than as a sky ")
		TEXT("setting. Move this only together with the climate window it is derived from."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSkyOriginLongitudeDeg(
		TEXT("voxel.Sky.OriginLongitudeDeg"), 0.0f,
		TEXT("Longitude the world ORIGIN is declared to sit at, degrees east. Default 0.0 (the ")
		TEXT("prime meridian) because with no real geography to anchor to, longitude's only job is ")
		TEXT("to offset solar noon from the game clock's 12:00, and 0 makes those coincide -- so ")
		TEXT("-VoxelTimeOfDay=12:00 means the sun is actually due south. Any other value silently ")
		TEXT("shifts every pinned capture hour by 4 minutes per degree."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarSkyMoonEnabled(
		TEXT("voxel.Sky.MoonEnabled"), 1,
		TEXT("Render and light from the moon, as UE's SECOND atmosphere light. 1 = on (default; ")
		TEXT("a moonless night at these intensities is unnavigable, and the moon is the only thing ")
		TEXT("giving night a direction to its shadows). 0 = the moon light is hidden entirely, ")
		TEXT("which is also the A/B control for 'how much of the night frame is moonlight'."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSkyShadowUpdateHz(
		TEXT("voxel.Sky.ShadowUpdateHz"), 10.0f,
		TEXT("Cap on how often the sun/moon are actually RE-ORIENTED, in Hz. Default 10. A ")
		TEXT("continuously rotating directional light invalidates UE's cached whole-scene shadow ")
		TEXT("setup every single frame; stepping it instead quantises shadow motion, which is ")
		TEXT("visible as a pop if the step is coarse. 10 Hz is a starting guess, NOT a measurement ")
		TEXT("-- at the default 1200 s day the sun moves 0.03 deg per step, which should be below ")
		TEXT("the visible-pop threshold, and whether the cap buys anything at all is exactly what ")
		TEXT("the W7 perf leg exists to settle. 0 = uncapped (re-orient every frame), which is the ")
		TEXT("control arm of that measurement."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarSkyExposureMode(
		TEXT("voxel.Sky.ExposureMode"), 2,
		TEXT("0 = engine default (the sky post-process is disabled entirely and eye adaptation is ")
		TEXT("left exactly as the project ships it). 1 = clamped auto (histogram adaptation, but ")
		TEXT("with its brightness window pinned to a few stops either side of the sun-altitude ")
		TEXT("curve so it can breathe without walking a night frame up to 18% grey). 2 = manual EV ")
		TEXT("curve against sun altitude, DEFAULT. 2 is the default because a harness capture has ")
		TEXT("to be REPRODUCIBLE: modes 0 and 1 both make the frame's brightness a function of what ")
		TEXT("was on screen a moment ago, so two runs of the same leg differ. It is also the only ")
		TEXT("mode in which 'night' is verifiable at all -- see the long comment above ")
		TEXT("SkyExposureBiasEV for why unclamped auto-exposure makes a dark ship render mid-grey ")
		TEXT("and every screenshot gate pass anyway."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSkyExposureBias(
		TEXT("voxel.Sky.ExposureBias"), 0.0f,
		TEXT("Extra stops added on top of whatever voxel.Sky.ExposureMode resolves to. Default 0.0 ")
		TEXT("so that the shipped look IS the curve and not the curve plus a fudge. Positive is ")
		TEXT("brighter (UE's AutoExposureBias convention: +1 = 2x brighter). This is the knob for ")
		TEXT("'the whole day is a stop too dark on my monitor', not for fixing one time of day."),
		ECVF_Default);
} // namespace

namespace VoxelSky
{
	bool IsEnabled() { return CVarSkyEnabled.GetValueOnAnyThread() != 0; }

	float GetTimeScale() { return CVarSkyTimeScale.GetValueOnAnyThread(); }

	void SetTimeScale(float NewScale)
	{
		// ECVF_SetByCode, per the module's convention for programmatic writes.
		// NOTE this is a LOWER priority than ECVF_SetByConsole and
		// ECVF_SetByCommandLine, so this call can legitimately be REFUSED if the
		// value was already set from one of those. That is why every caller logs
		// by reading the value back rather than by echoing what it asked for.
		if (IConsoleVariable* Var = CVarSkyTimeScale.AsVariable())
		{
			Var->Set(*FString::Printf(TEXT("%f"), NewScale), ECVF_SetByCode);
		}
	}

	double GetDayLengthSeconds()
	{
		// Floored rather than clamped-to-default: a zero or negative day length
		// divides by zero inside JulianDayFromGameClock. One second is absurd but
		// it is finite, and the absurdity is visible in the logged state instead
		// of arriving as a NaN sun three subsystems later.
		return FMath::Max(1.0, (double)CVarSkyDayLengthSeconds.GetValueOnAnyThread());
	}

	double GetDaysPerYear()
	{
		return FMath::Max(1.0, (double)CVarSkyDaysPerYear.GetValueOnAnyThread());
	}

	double GetOriginLatitudeDeg()
	{
		return FMath::Clamp((double)CVarSkyOriginLatitudeDeg.GetValueOnAnyThread(), -90.0, 90.0);
	}

	double GetOriginLongitudeDeg()
	{
		return FMath::Clamp((double)CVarSkyOriginLongitudeDeg.GetValueOnAnyThread(), -180.0, 180.0);
	}

	bool IsMoonEnabled() { return CVarSkyMoonEnabled.GetValueOnAnyThread() != 0; }

	float GetShadowUpdateHz() { return FMath::Max(0.f, CVarSkyShadowUpdateHz.GetValueOnAnyThread()); }

	int32 GetExposureMode() { return FMath::Clamp(CVarSkyExposureMode.GetValueOnAnyThread(), 0, 2); }

	float GetExposureBias() { return CVarSkyExposureBias.GetValueOnAnyThread(); }
} // namespace VoxelSky

// --- tuning constants ------------------------------------------------------

namespace
{
	// THE SUN'S PEAK INTENSITY, AND WHY IT IS NOT 100000 LUX.
	//
	// The pre-W4 static rig ran the sun at Intensity 8 (VoxelEarthGameMode.cpp's
	// old BeginPlay). That number is load-bearing well outside this file:
	// AVoxelClipmapActor's cave exposure lock was calibrated against it by A/B
	// (VoxelClipmapActor.h:233-240, "the cave is lit by an 8-lux sun"), and every
	// surface screenshot the harness has ever taken was framed at it. Replacing
	// it with a physically-real ~100000 lux would be more correct and would
	// invalidate all of that at once.
	//
	// So the peak is set so that the SHIPPED DEFAULT POSE reproduces the old
	// number instead. The default pose is equinox noon at 52 N (see
	// kDefaultTimeOfDayHours / kDefaultDateMonth below), where the sun sits at
	// ~38 deg altitude, and SunIntensityFraction(38) is ~0.53 -- so 15.0 * 0.53
	// is ~8.0, the value the static rig used. A no-switch capture therefore
	// looks like a pre-W4 capture, and the difference between them is the
	// TIME OF DAY rather than a global exposure shift.
	//
	// UNVERIFIED PENDING THE BUILD: the 0.53 is arithmetic, not a measurement.
	// The first capture leg is where this gets confirmed or corrected.
	constexpr float kSunPeakIntensity = 15.0f;

	// Colour temperature endpoints. 1900 K is a low sunrise/sunset sun (deep
	// orange, heavily reddened by the air mass it is shining through); 6500 K is
	// a high sun (the D65 white point, i.e. "no tint"). The interpolation runs
	// over the first 25 degrees of altitude because that is roughly where the
	// air-mass reddening stops being visible -- above it the sun is white and
	// stays white all the way to the zenith, so spending curve on 25..90 would
	// only make noon subtly wrong.
	constexpr float kSunHorizonTemperatureK = 1900.f;
	constexpr float kSunZenithTemperatureK = 6500.f;
	constexpr double kSunTemperatureRampEndDeg = 25.0;

	// THE MOON IS A DELIBERATE CHEAT AND THE SIZE OF THE CHEAT IS STATED HERE.
	//
	// A real full moon delivers ~0.25 lux against the sun's ~100000, a ratio of
	// about 1:400000, i.e. 18.6 stops. Honouring that ratio against the peak
	// above would give the moon an intensity of 0.00004 and a night frame lit by
	// literally nothing -- which is astronomically right and unplayable, and is
	// why every game that has ever shipped a moon has cheated here.
	//
	// 0.12 is ~1:125 against the peak (7 stops), so a full moon reads as a dim
	// silver-blue key light with real directional shadowing rather than as
	// ambient mush. The exposure curve (below) is doing the other half of the
	// work: it lifts a night frame by ~5 stops, so the two together put the moon
	// somewhere legible without ever approaching daylight.
	constexpr float kMoonPeakIntensity = 0.12f;

	// ~4100 K. Moonlight is reflected sunlight and is therefore spectrally
	// almost identical to it; it LOOKS blue because of the Purkinje shift in
	// human scotopic vision, not because the light is blue. We are rendering the
	// perception, not the spectrum, so the cooler number is the right one.
	constexpr float kMoonTemperatureK = 4100.f;

	// The moon's contribution is faded out as the sun comes up, over the civil
	// twilight band. Not for realism (the moon really is still up in daylight)
	// but because a second atmosphere light contributing during the day is
	// invisible in the frame and not free in the renderer.
	constexpr double kMoonSunSuppressStartDeg = -6.0; // sun at civil twilight: moon at full strength
	constexpr double kMoonSunSuppressEndDeg = 0.0;    // sun at the horizon: moon contributes nothing

	// DEFAULT CLOCK POSE, and this default is doing real work.
	//
	// The naive default is epoch 0, which is midnight on 1 January -- i.e. every
	// existing harness leg that does not pass -VoxelTimeOfDay would suddenly
	// capture a black frame, and "the terrain did not stream" and "it is night"
	// look identical in a screenshot. (That failure mode has already cost this
	// project time once: see the settle-before-capture rule.)
	//
	// 12:00 on 20 March instead: the equinox, so the sun is at ~38 deg altitude
	// at 52 N and due south. That is within a few degrees of the static rig's
	// -45 deg pitch, so a no-switch capture is directly comparable to every
	// pre-W4 capture in the archive. Day-of-year 79 is 20 March and not 21
	// March because the reference year 2000 is a leap year (VoxelEphemeris.h:150-153).
	constexpr double kDefaultTimeOfDayHours = 12.0;
	constexpr int32 kDefaultDateMonth = 3;
	constexpr int32 kDefaultDateDay = 20;

	// Days elapsed before the first of each month in the reference year 2000.
	// LEAP YEAR -- February has 29 days -- which is the whole reason this table
	// is written out rather than computed from a 365-day constant.
	constexpr int32 kDaysBeforeMonth[12] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};
	constexpr int32 kDaysInMonth[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

	// The tropical year the seasonal clock is stretched across; must match
	// VoxelEphemeris.cpp's own constant or the resolved date logged here and the
	// date the ephemeris actually uses drift apart.
	constexpr double kDaysPerTropicalYear = 365.2425;

	// AEM_Manual's default physical camera (f/4, 1/60 s, ISO 100) is EV100 ~9.9.
	// Every bias below is an offset from that stop, and it is written down here
	// because the number is what makes the biases interpretable at all -- the
	// same calibration AVoxelClipmapActor's cave rig is stated in
	// (VoxelClipmapActor.h:233-240).
	constexpr double kManualBaseEV100 = 9.9;

	// How far histogram adaptation is allowed to wander from the curve in mode 1.
	// +/- 1.5 stops is enough to soften a step into a cave mouth and nowhere near
	// enough to walk a night frame to grey.
	constexpr double kClampedAutoWindowStops = 1.5;

	int32 DayOfYearFromMonthDay(int32 Month, int32 Day)
	{
		const int32 M = FMath::Clamp(Month, 1, 12);
		const int32 D = FMath::Clamp(Day, 1, kDaysInMonth[M - 1]);
		return kDaysBeforeMonth[M - 1] + (D - 1);
	}

	void MonthDayFromDayOfYear(int32 DayOfYear, int32& OutMonth, int32& OutDay)
	{
		const int32 Doy = FMath::Clamp(DayOfYear, 0, 365);
		OutMonth = 12;
		for (int32 M = 0; M < 12; ++M)
		{
			if (Doy < kDaysBeforeMonth[M] + kDaysInMonth[M])
			{
				OutMonth = M + 1;
				OutDay = Doy - kDaysBeforeMonth[M] + 1;
				return;
			}
		}
		OutDay = Doy - kDaysBeforeMonth[11] + 1;
	}

	// Solve for the world epoch that puts the clock at a given local hour AND as
	// close as this calendar can get to a given day of the year.
	//
	// "AS CLOSE AS THIS CALENDAR CAN GET" IS NOT A WEASEL, IT IS THE CALENDAR.
	// There are exactly DaysPerYear distinct game days in a game year, and they
	// are spread across a full 365.2425-day real year, so the REACHABLE dates are
	// quantised to 365.2425/DaysPerYear real days apart -- 7.6 days at the
	// default 48. Asking for 21 June therefore lands on whichever reachable date
	// is nearest, which may be several days off. There is no arrangement of the
	// epoch that fixes this without either breaking the diurnal cycle (see the
	// floor() argument in VoxelEphemeris.h:140-145) or raising DaysPerYear.
	//
	// The search is over the whole first game year, which is at most
	// ceil(DaysPerYear) candidates -- 48 iterations at the default, run once at
	// Initialize. Nothing here is on any hot path.
	double SolveEpochFor(double LocalHours, int32 TargetDayOfYear,
	                     double DayLengthSeconds, double DaysPerYear,
	                     int32& OutResolvedDayOfYear)
	{
		const double Hours = FMath::Fmod(FMath::Fmod(LocalHours, 24.0) + 24.0, 24.0);
		const double DayFraction = Hours / 24.0;
		const double N = FMath::Max(1.0, DaysPerYear);
		const int32 KMax = FMath::Max(1, FMath::CeilToInt(N));

		int32 BestK = 0;
		int32 BestDoy = 0;
		double BestErr = TNumericLimits<double>::Max();
		for (int32 K = 0; K < KMax; ++K)
		{
			// Whole game days plus the requested fraction: keeping K an INTEGER
			// is what makes frac(Epoch / DayLengthSeconds) come out exactly equal
			// to DayFraction, which is where the ephemeris reads the hour from.
			const double YearFraction = ((double)K + DayFraction) / N;
			if (YearFraction >= 1.0)
			{
				break; // past one game year; frac() would wrap and re-cover the same dates
			}
			const int32 Doy = (int32)FMath::FloorToDouble(YearFraction * kDaysPerTropicalYear);
			const double Err = FMath::Abs((double)Doy - (double)TargetDayOfYear);
			if (Err < BestErr)
			{
				BestErr = Err;
				BestK = K;
				BestDoy = Doy;
			}
		}

		OutResolvedDayOfYear = BestDoy;
		return DayLengthSeconds * ((double)BestK + DayFraction);
	}

	int32 DayOfYearFromEpoch(double EpochSeconds, double DayLengthSeconds, double DaysPerYear)
	{
		const double YearSeconds = FMath::Max(1.0, DayLengthSeconds * FMath::Max(1.0, DaysPerYear));
		const double YearFraction = FMath::Frac(EpochSeconds / YearSeconds);
		const double Wrapped = YearFraction < 0.0 ? YearFraction + 1.0 : YearFraction;
		return FMath::Clamp((int32)FMath::FloorToDouble(Wrapped * kDaysPerTropicalYear), 0, 365);
	}

	// Direct-beam strength, 0..1, as a function of apparent solar altitude.
	//
	// sin(altitude) is the geometric term (a surface receives light in proportion
	// to the cosine of the incidence angle) and the 1.3 exponent folds in
	// atmospheric extinction, which is what actually makes a low sun DIM as well
	// as red: at 5 degrees the beam is crossing roughly ten atmospheres. A pure
	// sin() would leave sunset far too bright and the whole cycle would read as
	// "the sun changed colour" rather than "the sun went down".
	//
	// Zero at and below the horizon, and continuous there -- the apparent
	// altitude the ephemeris returns already has refraction folded in, so
	// altitude 0 IS the moment the disc appears to touch the horizon.
	double SunIntensityFraction(double AltitudeDeg)
	{
		if (AltitudeDeg <= 0.0)
		{
			return 0.0;
		}
		const double S = FMath::Sin(FMath::DegreesToRadians(FMath::Min(AltitudeDeg, 90.0)));
		return FMath::Clamp(FMath::Pow(FMath::Max(S, 0.0), 1.3), 0.0, 1.0);
	}

	double SunTemperatureK(double AltitudeDeg)
	{
		const double T = FMath::Clamp(AltitudeDeg / kSunTemperatureRampEndDeg, 0.0, 1.0);
		const double Smooth = T * T * (3.0 - 2.0 * T); // smoothstep: no visible kink at either end
		return FMath::Lerp((double)kSunHorizonTemperatureK, (double)kSunZenithTemperatureK, Smooth);
	}

	// THE EXPOSURE CURVE, in UE AutoExposureBias stops (positive = brighter).
	//
	// WHY A CURVE AT ALL, AND WHY IT MUST NOT TRACK THE SCENE. This is the whole
	// of W5 and it is the trap VoxelClipmapActor.cpp:311-348 already walked into
	// once underground: UE's histogram eye adaptation hunts until the frame
	// averages 18% grey, and a night scene has no 18% grey in it, so adaptation
	// walks the entire image up until something is. Ship "night" with unclamped
	// auto-exposure and it RENDERS as a slightly blue afternoon -- and every
	// screenshot gate passes, because the gates measure whether pixels are there,
	// not whether they are dark. Night is then unverifiable by construction.
	//
	// So the exposure is pinned to the CLOCK, not to the frame. And critically it
	// only PARTIALLY compensates: the scene's own luminance drops by roughly ten
	// stops from noon to a moonlit midnight, and this curve lifts by five. The
	// remaining five stops are what the player sees as darkness. A curve that
	// compensated fully would be auto-exposure with extra steps.
	//
	// THE ANCHORS BELOW ARE UNMEASURED FIRST GUESSES. The SHAPE is the part
	// backed by an argument (monotone in altitude, partial compensation, ~1 stop
	// per doubling); the numbers are seeded from the one measurement this project
	// does have -- the cave rig's +10 against an 8-intensity sun -- and are what
	// the W6 capture ladder exists to calibrate. Do not treat them as measured
	// until that leg has run.
	double ExposureBiasForSunAltitude(double AltitudeDeg)
	{
		struct FAnchor { double AltDeg; double BiasEV; };
		// Altitudes ascending. Below the first and above the last the curve is
		// flat, deliberately: there is nothing left to darken once the sun is 12
		// degrees down (astronomical-ish twilight is over) and nothing left to
		// brighten once it is well up.
		static const FAnchor Anchors[] = {
			{-18.0, 12.0},  // full night: five stops of lift against ~ten stops of scene
			{-12.0, 12.0},  // nautical twilight -- night has already bottomed out here
			{ -6.0, 10.5},  // civil twilight: the sky still carries the frame
			{  0.0,  9.0},  // sunrise/sunset
			{  2.0,  8.6},
			{ 10.0,  7.6},
			{ 20.0,  7.0},  // full day
			{ 90.0,  7.0},
		};
		constexpr int32 Count = UE_ARRAY_COUNT(Anchors);

		if (AltitudeDeg <= Anchors[0].AltDeg)
		{
			return Anchors[0].BiasEV;
		}
		for (int32 I = 1; I < Count; ++I)
		{
			if (AltitudeDeg <= Anchors[I].AltDeg)
			{
				const double Span = Anchors[I].AltDeg - Anchors[I - 1].AltDeg;
				const double T = Span > 0.0 ? (AltitudeDeg - Anchors[I - 1].AltDeg) / Span : 0.0;
				return FMath::Lerp(Anchors[I - 1].BiasEV, Anchors[I].BiasEV, T);
			}
		}
		return Anchors[Count - 1].BiasEV;
	}
} // namespace

// --- impl ------------------------------------------------------------------

struct FVoxelSkyImpl
{
	// THE CLOCK. One double, advanced by DeltaTime * TimeScale, and the single
	// source of truth for everything else in this file. Not a wall-clock read,
	// not a UWorld time: it has to be settable (the CLI pins, the W6 ladder) and
	// pausable (TimeScale 0) without either of those meaning anything to the
	// engine's own timekeeping.
	double EpochSeconds = 0.0;

	// Cadence accumulator for voxel.Sky.ShadowUpdateHz.
	double LightUpdateAccumulator = 0.0;
	bool bLightsEverApplied = false;

	// Last-known observer XY, in world UU. Held across frames so that a frame
	// with no player controller (loading, travel) reuses the previous position
	// instead of snapping the observer to the world origin and, with it, the
	// latitude and therefore the sun.
	double ObserverXUU = 0.0;
	double ObserverYUU = 0.0;

	// Last exposure actually pushed, so ApplyExposureFromState can skip the
	// post-process write on the overwhelming majority of frames (the bias moves
	// by ~0.0005 stops per frame at the default day length).
	int32 AppliedExposureMode = -1;
	double AppliedExposureBias = TNumericLimits<double>::Max();

	FVoxelSkyState State;
};

// --- lifetime --------------------------------------------------------------

UVoxelSkySubsystem::UVoxelSkySubsystem() = default;
UVoxelSkySubsystem::~UVoxelSkySubsystem() = default;
UVoxelSkySubsystem::UVoxelSkySubsystem(FVTableHelper& Helper)
	: Super(Helper)
{
}

void UVoxelSkySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Direct dependency on the terrain subsystem, exactly as
	// UVoxelWaterSubsystem::Initialize takes one: both are UWorldSubsystem-derived,
	// so InitializeDependency handles construction ORDER through the subsystem
	// collection rather than through a BeginPlay race.
	//
	// Being honest about what the dependency buys today: the sky makes no call
	// into terrain in this slice. What it buys is (a) an ordering guarantee for
	// the moment weather does -- precipitation has to ask the terrain what the
	// ground is, and retrofitting an ordering guarantee later is how races get
	// introduced -- and (b) a single well-defined failure shape. If the terrain
	// subsystem is absent this world is not a voxel world at all, and the right
	// answer is to leave Impl null and let every method null-check, which is the
	// same failure shape water uses.
	UVoxelWorldSubsystem* Terrain = Collection.InitializeDependency<UVoxelWorldSubsystem>();
	if (!Terrain)
	{
		UE_LOG(LogVoxelSky, Error,
		       TEXT("UVoxelSkySubsystem::Initialize: no UVoxelWorldSubsystem -- the world clock is disabled for ")
		       TEXT("this run and the light rig will not be spawned."));
		return;
	}

	Impl = MakeUnique<FVoxelSkyImpl>();

	// --- command line ------------------------------------------------------
	//
	// PARSED HERE, IN Initialize, AND NEVER VIA -ExecCmds. This module has
	// learned that lesson three separate times and written it down twice
	// (VoxelGI.cpp:243-260, VoxelWorldSubsystem.cpp:1597-1601): -ExecCmds lands
	// AFTER subsystem initialization, so a leg that set the hour that way would
	// spend its first frames at the default pose and its capture would be of
	// whatever the clock had drifted to -- silently, and differently on every
	// machine depending on load time. Half the harness legs would be capturing
	// the wrong hour and the screenshots would all look plausible.

	double TimeOfDayHours = kDefaultTimeOfDayHours;
	{
		// bShouldStopOnSeparator=false IS THE POINT OF THIS CALL. FParse::Value's
		// default terminator set includes ':' and ',', so the default parse of
		// "-VoxelTimeOfDay=06:30" silently returns "06" and the run captures
		// 06:00 while the log says 06:30. VoxelEarthGameMode.cpp:54-92
		// documents this exact trap for -VoxelSpawnAt=X,Y; it is the same trap
		// with a different separator.
		FString Arg;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelTimeOfDay="), Arg, /*bShouldStopOnSeparator=*/false))
		{
			FString HourStr, MinuteStr;
			if (!Arg.Split(TEXT(":"), &HourStr, &MinuteStr))
			{
				UE_LOG(LogVoxelSky, Warning,
				       TEXT("-VoxelTimeOfDay=%s malformed (expected HH:MM); keeping the default %02d:%02d."),
				       *Arg, (int32)kDefaultTimeOfDayHours, 0);
			}
			else
			{
				const int32 Hour = FCString::Atoi(*HourStr);
				const int32 Minute = FCString::Atoi(*MinuteStr);
				if (Hour < 0 || Hour > 23 || Minute < 0 || Minute > 59)
				{
					UE_LOG(LogVoxelSky, Warning,
					       TEXT("-VoxelTimeOfDay=%s out of range (expected 00:00..23:59); keeping the default ")
					       TEXT("%02d:00."), *Arg, (int32)kDefaultTimeOfDayHours);
				}
				else
				{
					TimeOfDayHours = (double)Hour + (double)Minute / 60.0;
				}
			}
		}
	}

	int32 TargetDayOfYear = DayOfYearFromMonthDay(kDefaultDateMonth, kDefaultDateDay);
	{
		// Same bShouldStopOnSeparator=false reasoning as above. '-' is not in the
		// default terminator set, but relying on that would make this call's
		// correctness depend on an engine detail that the sibling call two blocks
		// up already had to work around; parsing both the same way is one rule
		// instead of two.
		FString Arg;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelDate="), Arg, /*bShouldStopOnSeparator=*/false))
		{
			FString MonthStr, DayStr;
			if (!Arg.Split(TEXT("-"), &MonthStr, &DayStr))
			{
				UE_LOG(LogVoxelSky, Warning,
				       TEXT("-VoxelDate=%s malformed (expected MM-DD); keeping the default %02d-%02d."),
				       *Arg, kDefaultDateMonth, kDefaultDateDay);
			}
			else
			{
				const int32 Month = FCString::Atoi(*MonthStr);
				const int32 Day = FCString::Atoi(*DayStr);
				if (Month < 1 || Month > 12 || Day < 1 || Day > kDaysInMonth[Month - 1])
				{
					UE_LOG(LogVoxelSky, Warning,
					       TEXT("-VoxelDate=%s out of range (expected 01-01..12-31, reference year 2000 is a LEAP ")
					       TEXT("year so 02-29 is legal); keeping the default %02d-%02d."),
					       *Arg, kDefaultDateMonth, kDefaultDateDay);
				}
				else
				{
					TargetDayOfYear = DayOfYearFromMonthDay(Month, Day);
				}
			}
		}
	}

	{
		float RequestedScale = 0.f;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelTimeScale="), RequestedScale))
		{
			VoxelSky::SetTimeScale(RequestedScale);
		}
	}

	const double DayLength = VoxelSky::GetDayLengthSeconds();
	const double DaysPerYear = VoxelSky::GetDaysPerYear();
	int32 ResolvedDayOfYear = 0;
	Impl->EpochSeconds = SolveEpochFor(TimeOfDayHours, TargetDayOfYear, DayLength, DaysPerYear, ResolvedDayOfYear);

	// EVERY SWITCH LOGS WHAT IT RESOLVED TO, BY READING IT BACK.
	//
	// Not by echoing the request. The date in particular CANNOT generally be
	// honoured exactly (see SolveEpochFor: reachable dates are quantised to
	// 365.2425/DaysPerYear days apart, 7.6 at the default), the time scale can be
	// refused outright by cvar priority, and the hour is only exact because the
	// solver keeps the day count an integer. VoxelGpuVerify.cpp:2118-2126 states
	// the rule this follows: the log has to say what the run actually used or it
	// is not evidence.
	{
		const double ResolvedDayFraction = FMath::Frac(Impl->EpochSeconds / DayLength);
		const double ResolvedHours = ResolvedDayFraction * 24.0;
		int32 ResolvedMonth = 1, ResolvedDay = 1;
		MonthDayFromDayOfYear(ResolvedDayOfYear, ResolvedMonth, ResolvedDay);
		int32 RequestedMonth = 1, RequestedDay = 1;
		MonthDayFromDayOfYear(TargetDayOfYear, RequestedMonth, RequestedDay);

		UE_LOG(LogVoxelSky, Log,
		       TEXT("VoxelSky clock RESOLVED: %02d:%02d on %02d-%02d (day-of-year %d) at epoch %.3f s. ")
		       TEXT("Requested %02d-%02d (day-of-year %d); the calendar's reachable dates are %.2f real days ")
		       TEXT("apart at DaysPerYear=%.1f."),
		       // Minutes FLOORED, not rounded: rounding can produce "12:60".
		       (int32)ResolvedHours, (int32)FMath::FloorToDouble(FMath::Frac(ResolvedHours) * 60.0 + 0.5e-9),
		       ResolvedMonth, ResolvedDay, ResolvedDayOfYear, Impl->EpochSeconds,
		       RequestedMonth, RequestedDay, TargetDayOfYear,
		       kDaysPerTropicalYear / DaysPerYear, DaysPerYear);

		UE_LOG(LogVoxelSky, Log,
		       TEXT("VoxelSky settings RESOLVED: Enabled=%d TimeScale=%.3f DayLength=%.1f s DaysPerYear=%.1f ")
		       TEXT("Origin=(%.4f N, %.4f E) Moon=%d ShadowUpdateHz=%.2f ExposureMode=%d ExposureBias=%.2f"),
		       VoxelSky::IsEnabled() ? 1 : 0, VoxelSky::GetTimeScale(), DayLength, DaysPerYear,
		       VoxelSky::GetOriginLatitudeDeg(), VoxelSky::GetOriginLongitudeDeg(),
		       VoxelSky::IsMoonEnabled() ? 1 : 0, VoxelSky::GetShadowUpdateHz(),
		       VoxelSky::GetExposureMode(), VoxelSky::GetExposureBias());
	}
}

void UVoxelSkySubsystem::Deinitialize()
{
	// The actors are world-owned and go with the world; dropping the references
	// is all this owes. Same shape as UVoxelWaterSubsystem::Deinitialize's
	// ChunkOwner/ChunkRoot release.
	SunLight = nullptr;
	MoonLight = nullptr;
	SkyLightActor = nullptr;
	SkyRigActor = nullptr;
	SkyExposurePP = nullptr;
	bHasState = false;
	Impl.Reset();

	Super::Deinitialize();
}

bool UVoxelSkySubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Same scope as UVoxelWaterSubsystem (VoxelWaterSubsystem.cpp:527-531) and
	// UVoxelWorldSubsystem: game worlds only. Deliberately NOT ShouldCreateSubsystem
	// -- nothing in this module uses it, and introducing a second gate with
	// different semantics for one subsystem is how the two answers drift.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::GamePreview;
}

void UVoxelSkySubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (!InWorld.IsGameWorld() || !Impl)
	{
		return;
	}

	// Dedicated server: no viewport, so nothing to light. The clock still runs
	// (Tick is not gated on this) because it costs a handful of transcendentals
	// and because the replication TODO in the header will want it running on the
	// authority. Same reasoning as UVoxelWaterSubsystem's dedicated-server branch,
	// which skips render chunks and keeps simulating.
	if (InWorld.GetNetMode() == NM_DedicatedServer)
	{
		UE_LOG(LogVoxelSky, Log,
		       TEXT("Sky rig NOT spawned (NM_DedicatedServer has no viewport); the world clock still runs."));
		return;
	}

	SpawnRig(InWorld);

	// Put the rig somewhere sane before the first Tick. With the clock off this
	// is also the FINAL pose, and it is byte-identical to the pre-W4 static rig,
	// which is what makes voxel.Sky.Enabled 0 a real A/B control rather than a
	// blackout switch.
	ApplyStaticRigPose();
}

// --- rig -------------------------------------------------------------------

void UVoxelSkySubsystem::SpawnRig(UWorld& World)
{
	// THE RIG WAS MOVED HERE FROM AVoxelEarthGameMode::BeginPlay, WHOLESALE.
	//
	// Moved rather than adopted. "Adopt whatever ADirectionalLight the game mode
	// spawned" would make the driver's correctness depend on an actor-iteration
	// order, on the game mode having run first, and on nobody ever placing a
	// second directional light in a map -- three invisible preconditions bought
	// in exchange for leaving twenty lines where they were. The thing that drives
	// the lights now creates them, and AVoxelEarthGameMode::BeginPlay no longer
	// mentions lighting at all.

	// EVERY SKY ACTOR IS PLACED THROUGH ParseSpawnColumnUU. This is not a style
	// preference: at a far LWC spawn (-VoxelSpawnAt=2000000,1500000 is 20,000 km
	// out) an actor left at the world origin is 20,000 km from the player, and
	// for the SkyAtmosphere that renders as a horizon sphere slicing across the
	// sky. The helper is deliberately shared with the pawn spawn
	// (VoxelEarthGameMode.cpp:54-92) so the two cannot drift apart.
	double SpawnColumnXUU = 0.0;
	double SpawnColumnYUU = 0.0;
	VoxelEarthSpawn::ParseSpawnColumnUU(SpawnColumnXUU, SpawnColumnYUU); // stays (0,0) if the switch is absent
	Impl->ObserverXUU = SpawnColumnXUU;
	Impl->ObserverYUU = SpawnColumnYUU;

	// --- sun ---------------------------------------------------------------
	SunLight = World.SpawnActor<ADirectionalLight>();
	if (SunLight)
	{
		// MOVABLE, AND THIS IS LOAD-BEARING. ALight defaults to Stationary
		// mobility, and UE refuses to move a Stationary light at runtime (it
		// warns once and then ignores the transform). The pre-W4 rig got away
		// with it because it set the rotation exactly once, during BeginPlay,
		// before registration had settled; a rig that re-orients every tenth of
		// a second does not. Forgetting this produces a sun that is stuck at
		// whatever pose it was spawned with while every log line reports it
		// moving, which is a genuinely nasty afternoon.
		SunLight->SetMobility(EComponentMobility::Movable);

		if (UDirectionalLightComponent* SunComp = Cast<UDirectionalLightComponent>(SunLight->GetLightComponent()))
		{
			// Kept from the pre-W4 rig unchanged: the sun is the atmosphere's
			// primary light. Index 0 is already the default, but it is set
			// EXPLICITLY here so that the sun and the moon below read as a pair
			// -- an unstated 0 next to a stated 1 invites someone to "fix" the
			// asymmetry by removing the 1.
			SunComp->SetAtmosphereSunLight(true);
			SunComp->SetAtmosphereSunLightIndex(0);
			SunComp->SetUseTemperature(true);

			// -VoxelShadowCascades=<N>: PRESERVED VERBATIM from
			// the pre-W4 AVoxelEarthGameMode::BeginPlay, including the reason it exists.
			// The 2026-07-27 draw-path diagnosis measured the shadow gathers at
			// ~4 per camera gather, each submitting against the whole pool; the
			// confirmation leg that tried to vary this with
			// `r.Shadow.CSM.MaxCascades 1` via -ExecCmds moved NOTHING (the
			// gather census stayed at 5/frame). The PER-LIGHT property below is
			// what actually governs, so the sweep has to set it here, at the
			// light, before the first shadow setup. Do not "simplify" this back
			// to the cvar; that route has already been tried and recorded.
			// Latched; absent = engine default (unchanged behaviour).
			int32 Cascades = 0;
			if (FParse::Value(FCommandLine::Get(), TEXT("VoxelShadowCascades="), Cascades) && Cascades >= 0)
			{
				SunComp->DynamicShadowCascades = FMath::Clamp(Cascades, 0, 10);
				SunComp->MarkRenderStateDirty();
				UE_LOG(LogVoxelSky, Log,
				       TEXT("VoxelShadowCascades override: sun DynamicShadowCascades=%d"),
				       SunComp->DynamicShadowCascades);
			}
		}
	}

	// --- moon, as UE's SECOND atmosphere light ------------------------------
	MoonLight = World.SpawnActor<ADirectionalLight>();
	if (MoonLight)
	{
		MoonLight->SetMobility(EComponentMobility::Movable);
		if (UDirectionalLightComponent* MoonComp = Cast<UDirectionalLightComponent>(MoonLight->GetLightComponent()))
		{
			// SetAtmosphereSunLightIndex(1) is what makes this a MOON rather
			// than a second sun: UE's SkyAtmosphere supports exactly two lights
			// natively, and index 1 gets its own disc, its own scattering and
			// its own contribution to the sky colour. A point light or a
			// dimmed-down second sun at index 0 can produce neither the disc nor
			// the correct night-sky tint.
			MoonComp->SetAtmosphereSunLight(true);
			MoonComp->SetAtmosphereSunLightIndex(1);
			MoonComp->SetUseTemperature(true);
			MoonComp->SetTemperature(kMoonTemperatureK);

			// NO SHADOWS FROM THE MOON. A second shadow-casting directional
			// light is a second whole-scene shadow setup and a second set of
			// cascades every frame -- the single most expensive thing in this
			// file -- bought for shadows cast by a 0.12-intensity light that the
			// exposure curve is simultaneously lifting five stops. It is not a
			// close call today. Revisit only with a number from the W7 leg.
			MoonComp->SetCastShadows(false);
		}
	}

	// --- sky light ----------------------------------------------------------
	SkyLightActor = World.SpawnActor<ASkyLight>();
	if (SkyLightActor)
	{
		if (USkyLightComponent* SkyComp = SkyLightActor->GetLightComponent())
		{
			// MOVABLE, and note carefully what this is NOT for. Real-time
			// capture does NOT require it -- SkyLightComponent.cpp:1161 accepts
			// Movable OR Stationary, so the pre-W4 rig's Stationary sky light was
			// genuinely getting real-time capture. Movable is required here only
			// because the SetActorLocation BELOW moves the actor, and UE refuses
			// to move a Stationary root at runtime. It is safe because the only
			// other thing Stationary buys a sky light is participation in baked
			// static shadowing (SkyLightComponent.cpp:242), and this project has
			// no baked lighting at all.
			//
			// ASkyLight derives from AInfo, not ALight, so there is no
			// ASkyLight::SetMobility -- it goes on the component, which is the
			// actor's root (SkyLightComponent.cpp:1213).
			SkyComp->SetMobility(EComponentMobility::Movable);

			// Real-time capture is HOW NIGHT GETS DARK HERE: the ambient term
			// follows the sky down as the sun sets, without anything having to
			// dim it explicitly. Nothing in this file touches
			// voxel.GI.AmbientFloor or any other GI cvar, and it must not -- the
			// GI term multiplies ALBEDO, so crushing it to make night dark would
			// mean a torch on a black wall lights nothing.
			SkyComp->SetRealTimeCaptureEnabled(true);
		}

		// THE BUG THIS FIXES, and it is the one the brief called out. The old rig
		// (the pre-W4 AVoxelEarthGameMode::BeginPlay) spawned the SkyLight and NEVER PLACED
		// IT, so it sat at the world origin regardless of -VoxelSpawnAt while the
		// SkyAtmosphere right beside it was carefully placed at the spawn column.
		// At a 20,000 km LWC spawn the real-time capture was therefore capturing
		// the sky as seen from 20,000 km away from the player. It goes through the
		// SAME ParseSpawnColumnUU as everything else in this function now.
		//
		// Placed AFTER the mobility change above, not before: moving a Stationary
		// root at runtime is refused, so the order here is load-bearing.
		SkyLightActor->SetActorLocation(FVector(SpawnColumnXUU, SpawnColumnYUU, 0.0));
	}

	// --- atmosphere + the exposure post-process, on one actor ---------------
	SkyRigActor = World.SpawnActor<AActor>();
	if (SkyRigActor)
	{
		USkyAtmosphereComponent* AtmosphereComp = NewObject<USkyAtmosphereComponent>(SkyRigActor);

		// M2 task "SkyAtmosphere origin fix", preserved verbatim from
		// the pre-W4 AVoxelEarthGameMode::BeginPlay: the component's default TransformMode
		// (PlanetTopAtAbsoluteWorldOrigin) hardcodes the planet's ground level at
		// world (0,0,0). At a far LWC spawn the player is nowhere near that
		// assumed ground level, so the atmosphere's horizon sphere renders badly
		// misplaced -- visible as a horizon-sphere artifact cutting across the
		// sky. PlanetTopAtComponentTransform makes the planet's ground level
		// follow THIS component's own world transform, so placing the actor at
		// the spawn column keeps the horizon correct at any spawn offset.
		AtmosphereComp->TransformMode = ESkyAtmosphereTransformMode::PlanetTopAtComponentTransform;
		AtmosphereComp->RegisterComponent();
		SkyRigActor->SetRootComponent(AtmosphereComp);
		SkyRigActor->SetActorLocation(FVector(SpawnColumnXUU, SpawnColumnYUU, 0.0));

		// W5's post-process. Hosted here rather than on its own actor purely so
		// that the sky rig is one actor to find in the outliner; being unbound,
		// its transform is irrelevant.
		SkyExposurePP = NewObject<UPostProcessComponent>(SkyRigActor, TEXT("SkyExposurePP"));
		SkyExposurePP->SetupAttachment(AtmosphereComp);
		SkyExposurePP->RegisterComponent();

		// Unbound: the volume has no shape and applies everywhere. Correct here
		// for the same reason it is correct for the cave rig -- the gate is a
		// property of the CLOCK, not of a region a designer could author.
		SkyExposurePP->bUnbound = true;

		// ===================================================================
		// EXPOSURE OWNERSHIP RULE -- READ THIS BEFORE ADDING A THIRD VOLUME.
		// (The same rule is written into VoxelClipmapActor.cpp beside the other
		// volume. If you change one copy, change both; they exist in duplicate
		// precisely because whoever next touches either file will only read that
		// one.)
		//
		// Two UNBOUND post-process volumes that both override auto-exposure do
		// not blend. For any setting BOTH override, the higher Priority wins
		// outright and the loser's value vanishes -- no warning, no log line,
		// no visual clue except that one of them stopped working. There are
		// exactly two in this project:
		//
		//   AVoxelClipmapActor::CaveExposurePP   Priority 100   conditional
		//   UVoxelSkySubsystem::SkyExposurePP    Priority  10   always-on base
		//
		// THE RULE: the SKY volume is the BASE LAYER and the CAVE volume is a
		// strictly more specific override that wins where it applies.
		//
		// The sky volume covers the whole world and is enabled whenever
		// voxel.Sky.ExposureMode is non-zero. The cave volume is enabled only
		// while AVoxelClipmapActor::IsCameraUnderRock is true -- and under rock
		// the sun's altitude is not an input to anything, because no daylight
		// reaches the camera. The sky curve has nothing to say there, and the
		// cave's +10 EV100 is the only exposure number in this project backed by
		// an actual A/B against a reference frame (VoxelClipmapActor.h:233-240).
		// So the more specific volume should win, and giving it the higher
		// priority is how that is expressed.
		//
		// WHY PRIORITY ORDERING AND NOT "THE SKY OWNS BOTH". Making this
		// subsystem the single owner and demoting the cave case to a modifier
		// was the alternative and it was rejected for two concrete reasons.
		// (1) LIFETIME: AVoxelClipmapActor is suppressible (-VoxelNoClipmap) and
		// optional; the sky must not acquire a dependency on an actor that may
		// not exist, and the cave rig must not acquire one on a subsystem that
		// may be disabled. (2) PROVENANCE: folding +10 into this curve would
		// re-derive by taste a number that was arrived at by measurement, and
		// the measurement's paper trail would be lost in the merge.
		//
		// THE INVARIANT A THIRD VOLUME MUST HOLD: override the SAME exposure
		// fields these two do (method AND bias), or it will win the fields it
		// overrides and silently inherit the rest from a lower-priority volume,
		// producing a hybrid exposure matching neither. And declare its priority
		// band in both of these comments.
		//
		// (Mode 1 additionally overrides the min/max brightness clamps, which
		// the cave volume does not. That is safe and not an exception to the
		// invariant above: under rock the cave volume wins AutoExposureMethod
		// with AEM_Manual, and manual exposure ignores those clamps entirely.)
		// ===================================================================
		SkyExposurePP->Priority = 10.f;
		SkyExposurePP->bEnabled = false; // ApplyExposureFromState turns it on
	}
}

void UVoxelSkySubsystem::ApplyStaticRigPose()
{
	// The pre-W4 pose, reproduced exactly: FRotator(-45, 30, 0) and Intensity 8,
	// no colour temperature. This is what voxel.Sky.Enabled 0 leaves behind, and
	// keeping it byte-identical is what makes the cvar a usable A/B control --
	// "off" has to mean "the frame from before this feature existed", not "a
	// different frame that happens to also be lit".
	if (SunLight)
	{
		SunLight->SetActorRotation(FRotator(-45.f, 30.f, 0.f));
		if (UDirectionalLightComponent* SunComp = Cast<UDirectionalLightComponent>(SunLight->GetLightComponent()))
		{
			SunComp->SetUseTemperature(false);
			SunComp->SetIntensity(8.f);
			SunComp->SetVisibility(true);
		}
	}
	if (MoonLight)
	{
		if (UDirectionalLightComponent* MoonComp = Cast<UDirectionalLightComponent>(MoonLight->GetLightComponent()))
		{
			MoonComp->SetIntensity(0.f);
			MoonComp->SetVisibility(false);
		}
	}
	if (SkyExposurePP)
	{
		SkyExposurePP->bEnabled = false;
	}
	if (Impl)
	{
		Impl->AppliedExposureMode = -1;
		Impl->AppliedExposureBias = TNumericLimits<double>::Max();
		Impl->bLightsEverApplied = false;
	}
}

// --- tick ------------------------------------------------------------------

bool UVoxelSkySubsystem::IsTickable() const
{
	// Zero per-frame cost when the clock is off. bHasState keeps ticking for
	// exactly as long as it takes to put the rig back to its static pose after a
	// runtime toggle-off -- the same shape as UVoxelGISubsystem::IsTickable
	// (VoxelGI.cpp:349-354).
	return VoxelSky::IsEnabled() || bHasState;
}

TStatId UVoxelSkySubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelSkySubsystem, STATGROUP_Tickables);
}

bool UVoxelSkySubsystem::ResolveObserverXYUU(double& OutXUU, double& OutYUU) const
{
	// Same fallback chain as UVoxelGISubsystem::ResolveViewOriginUU and
	// AVoxelClipmapActor::GetCameraLocationUU. Kept identical on purpose: three
	// different answers to "where is the player" is three different worlds.
	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			if (const APlayerCameraManager* Cam = PC->PlayerCameraManager)
			{
				const FVector Loc = Cam->GetCameraLocation();
				OutXUU = Loc.X;
				OutYUU = Loc.Y;
				return true;
			}
			if (const APawn* Pawn = PC->GetPawn())
			{
				const FVector Loc = Pawn->GetActorLocation();
				OutXUU = Loc.X;
				OutYUU = Loc.Y;
				return true;
			}
		}
	}
	return false;
}

void UVoxelSkySubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!Impl)
	{
		return;
	}

	if (!VoxelSky::IsEnabled())
	{
		// One frame of work to undo the feature, then IsTickable goes false.
		if (bHasState)
		{
			ApplyStaticRigPose();
			Impl->State = FVoxelSkyState();
			bHasState = false;
			UE_LOG(LogVoxelSky, Log, TEXT("voxel.Sky.Enabled 0: rig returned to the static pre-W4 pose."));
		}
		return;
	}

	const double TimeScale = (double)VoxelSky::GetTimeScale();
	const double DayLength = VoxelSky::GetDayLengthSeconds();
	const double DaysPerYear = VoxelSky::GetDaysPerYear();

	Impl->EpochSeconds += (double)DeltaTime * TimeScale;

	// The observer's own position, because latitude is a function of it
	// (GeoFromWorldUU). Held over from the previous frame when there is no
	// controller: snapping to the world origin would move the observer by
	// however far out the spawn column is, and at a far LWC spawn that is a
	// visible jump in the sun.
	double ObsX = Impl->ObserverXUU;
	double ObsY = Impl->ObserverYUU;
	if (ResolveObserverXYUU(ObsX, ObsY))
	{
		Impl->ObserverXUU = ObsX;
		Impl->ObserverYUU = ObsY;
	}

	const VoxelSky::FGeoCoord Geo = VoxelSky::GeoFromWorldUU(
		Impl->ObserverXUU, Impl->ObserverYUU,
		VoxelSky::GetOriginLatitudeDeg(), VoxelSky::GetOriginLongitudeDeg());

	const double JulianDay = VoxelSky::JulianDayFromGameClock(Impl->EpochSeconds, DayLength, DaysPerYear);
	const VoxelSky::FSunState Sun = VoxelSky::ComputeSun(JulianDay, Geo);
	const VoxelSky::FMoonState Moon = VoxelSky::ComputeMoon(JulianDay, Geo, Sun);

	FVoxelSkyState& S = Impl->State;
	S.EpochSeconds = Impl->EpochSeconds;
	S.JulianDay = JulianDay;
	S.DayFraction = FMath::Frac(Impl->EpochSeconds / DayLength);
	if (S.DayFraction < 0.0)
	{
		S.DayFraction += 1.0; // negative TimeScale is legal; a negative day fraction is not
	}
	S.LocalHours = S.DayFraction * 24.0;
	S.DayOfYear = DayOfYearFromEpoch(Impl->EpochSeconds, DayLength, DaysPerYear);
	S.LatitudeDeg = Geo.LatitudeDeg;
	S.LongitudeDeg = Geo.LongitudeDeg;
	S.SunAltitudeDeg = Sun.AltitudeDeg;
	S.SunAzimuthDeg = Sun.AzimuthDeg;
	S.MoonAltitudeDeg = Moon.AltitudeDeg;
	S.MoonAzimuthDeg = Moon.AzimuthDeg;
	S.MoonPhaseFraction = Moon.PhaseFraction;
	S.MoonIlluminatedFraction = Moon.IlluminatedFraction;
	S.bSunUp = Sun.AltitudeDeg > 0.0;
	S.bMoonUp = Moon.AltitudeDeg > 0.0;
	S.bClockRunning = TimeScale != 0.0;

	// --- exposure, EVERY frame ---------------------------------------------
	//
	// Deliberately NOT under the shadow cadence cap. Exposure is a scalar on a
	// post-process; writing it costs nothing in the renderer and busts no shadow
	// cache, and stepping it at 10 Hz through a sunset would produce a visible
	// brightness staircase for no saving at all. The at-most-100 ms disagreement
	// between the stepped light and the continuous exposure is not observable.
	ApplyExposureFromState();

	// --- light orientation, CADENCE-CAPPED ----------------------------------
	//
	// WHY THE CAP EXISTS: a directional light whose rotation changes every frame
	// invalidates UE's cached whole-scene shadow setup every frame. Stepping the
	// rotation instead lets that cache survive between steps -- at the cost of
	// quantising shadow motion, which shows up as a pop if the step is coarse
	// enough. At the default 1200 s day and 10 Hz that step is 0.03 degrees of
	// solar motion, which should be far below the visible-pop threshold.
	//
	// THE TRADEOFF IS UNMEASURED. "Should be" is doing real work in that
	// sentence: nobody has run the frozen-sun vs moving-sun A/B on this project's
	// actual draw path, and the draw path here is unusual enough (render-thread
	// bound, submission-heavy) that borrowed intuitions about shadow caching are
	// not evidence. The W7 perf leg exists for exactly this question; until it
	// has run, treat both the existence of the cap and the choice of 10 Hz as
	// hypotheses. voxel.Sky.ShadowUpdateHz 0 is the uncapped control arm.
	//
	// The WHOLE light state (rotation, intensity, colour) is stepped together
	// rather than only the rotation. Splitting them would leave the sun's
	// brightness ahead of its position by up to a step, which is a subtler and
	// harder-to-attribute artifact than a slightly stale light.
	const float UpdateHz = VoxelSky::GetShadowUpdateHz();
	Impl->LightUpdateAccumulator += (double)DeltaTime;
	const double UpdatePeriod = UpdateHz > 0.f ? 1.0 / (double)UpdateHz : 0.0;
	const bool bDue = !Impl->bLightsEverApplied || UpdatePeriod <= 0.0 ||
	                  Impl->LightUpdateAccumulator >= UpdatePeriod;

	S.SecondsSinceLightUpdate = Impl->LightUpdateAccumulator;
	if (bDue)
	{
		Impl->LightUpdateAccumulator = 0.0;
		Impl->bLightsEverApplied = true;

		// THE SIGN. FSunState::Direction points FROM the surface TOWARD the sun;
		// a DirectionalLight's forward vector is the direction light TRAVELS,
		// which is the opposite. Hence the negation, which VoxelEphemeris.h:27-41
		// spells out at length because getting it backwards produces a scene lit
		// from underground at noon and blazing at midnight -- and that looks
		// enough like a broken ephemeris that the search starts in the wrong file.
		if (SunLight)
		{
			SunLight->SetActorRotation((-Sun.Direction).Rotation());
		}
		if (MoonLight)
		{
			MoonLight->SetActorRotation((-Moon.Direction).Rotation());
		}
		ApplyLightsFromState();
		S.LightUpdates++;
		S.SecondsSinceLightUpdate = 0.0;
	}

	bHasState = true;
}

void UVoxelSkySubsystem::ApplyLightsFromState()
{
	if (!Impl)
	{
		return;
	}
	FVoxelSkyState& S = Impl->State;

	// --- sun ----------------------------------------------------------------
	const double SunFrac = SunIntensityFraction(S.SunAltitudeDeg);
	S.SunIntensity = (float)(kSunPeakIntensity * SunFrac);
	S.SunTemperatureK = (float)SunTemperatureK(S.SunAltitudeDeg);
	if (SunLight)
	{
		if (UDirectionalLightComponent* SunComp = Cast<UDirectionalLightComponent>(SunLight->GetLightComponent()))
		{
			SunComp->SetUseTemperature(true);
			SunComp->SetTemperature(S.SunTemperatureK);
			SunComp->SetIntensity(S.SunIntensity);
			// Hidden outright below the horizon rather than merely set to zero
			// intensity. A zero-intensity directional light still participates in
			// shadow setup, and "the sun is down" is the half of the day where
			// that cost buys literally nothing. It also stops the SkyAtmosphere
			// drawing a black disc where the sun is.
			SunComp->SetVisibility(S.SunIntensity > 0.f);
		}
	}

	// --- moon ---------------------------------------------------------------
	//
	// Three multiplicands, and each is a different question:
	//   IlluminatedFraction -- how much of the disc is lit (NOT PhaseFraction;
	//                          VoxelEphemeris.h:85-93 is explicit that a "half
	//                          moon" at PhaseFraction 0.25 is 50% lit).
	//   the altitude falloff -- the moon low on the horizon is dimmed by the
	//                          same air mass the sun is, so the same curve.
	//   the daylight suppression -- a second atmosphere light contributing
	//                          during the day is invisible and not free.
	const bool bMoonOn = VoxelSky::IsMoonEnabled();
	double MoonFrac = 0.0;
	if (bMoonOn)
	{
		const double SunSuppress = 1.0 - FMath::Clamp(
			(S.SunAltitudeDeg - kMoonSunSuppressStartDeg) /
				(kMoonSunSuppressEndDeg - kMoonSunSuppressStartDeg), 0.0, 1.0);
		MoonFrac = SunIntensityFraction(S.MoonAltitudeDeg) * S.MoonIlluminatedFraction * SunSuppress;
	}
	S.MoonIntensity = (float)(kMoonPeakIntensity * MoonFrac);
	if (MoonLight)
	{
		if (UDirectionalLightComponent* MoonComp = Cast<UDirectionalLightComponent>(MoonLight->GetLightComponent()))
		{
			MoonComp->SetIntensity(S.MoonIntensity);
			MoonComp->SetVisibility(bMoonOn && S.MoonIntensity > 0.f);
		}
	}
}

void UVoxelSkySubsystem::ApplyExposureFromState()
{
	if (!Impl || !SkyExposurePP)
	{
		return;
	}
	FVoxelSkyState& S = Impl->State;

	const int32 Mode = VoxelSky::GetExposureMode();
	const double CurveBias = ExposureBiasForSunAltitude(S.SunAltitudeDeg);
	const double Bias = CurveBias + (double)VoxelSky::GetExposureBias();

	S.ExposureMode = Mode;
	S.ExposureBiasEV = (float)Bias;

	// Skip the write when nothing moved enough to matter. At the default day
	// length the bias moves by ~0.0005 stops per frame, so without this the
	// post-process settings would be rewritten sixty times a second to no effect.
	// 0.01 stops is a hundredth of the smallest step anyone can see.
	if (Mode == Impl->AppliedExposureMode && FMath::Abs(Bias - Impl->AppliedExposureBias) < 0.01)
	{
		return;
	}
	Impl->AppliedExposureMode = Mode;
	Impl->AppliedExposureBias = Bias;

	FPostProcessSettings& PP = SkyExposurePP->Settings;

	if (Mode == 0)
	{
		// ENGINE DEFAULT: the volume is switched off entirely, not neutralised.
		// A disabled volume overrides nothing, so mode 0 is genuinely "the frame
		// this project rendered before W5 existed" rather than "W5 configured to
		// look similar". That distinction is the difference between a control arm
		// and a second treatment.
		SkyExposurePP->bEnabled = false;
		return;
	}

	SkyExposurePP->bEnabled = true;

	if (Mode == 2)
	{
		// MANUAL, and AEM_Manual specifically -- NOT "histogram with min == max".
		// VoxelClipmapActor.cpp:417-427 records what happens if you take the
		// apparently-more-surgical route: the min/max clamp fields are
		// interpreted through r.EyeAdaptation.ExposureFormat, so "0" is not
		// unambiguously EV100 0, and the wrong reading of it is an exposure
		// divide that runs away -- it produced a frame that was 100.0% pure
		// white, every pixel clipped. AEM_Manual has no such ambiguity: the stop
		// comes from the physical camera fields and AutoExposureBias offsets it
		// in whole stops.
		PP.bOverride_AutoExposureMethod = true;
		PP.AutoExposureMethod = AEM_Manual;
		PP.bOverride_AutoExposureBias = true;
		PP.AutoExposureBias = (float)Bias;
		// The clamps are explicitly NOT overridden in this mode: manual exposure
		// ignores them, and leaving them un-overridden keeps the "override the
		// same fields as the other volume" invariant clean.
		PP.bOverride_AutoExposureMinBrightness = false;
		PP.bOverride_AutoExposureMaxBrightness = false;
		return;
	}

	// Mode 1: CLAMPED AUTO.
	//
	// Histogram adaptation, but with the window it may hunt inside pinned to a
	// few stops either side of where the curve says the scene should be. That
	// gives back the one thing manual exposure cannot do -- soften the step
	// through a cave mouth or a tree line -- without giving back the failure
	// this whole deliverable exists to prevent, which is adaptation walking a
	// night frame up to 18% grey.
	//
	// The window is centred on the EFFECTIVE stop the manual mode would land at:
	// AEM_Manual's default physical camera is EV100 ~9.9 and a positive bias
	// brightens, so an effective EV100 of (9.9 - Bias) is the same exposure
	// expressed in the units these clamp fields want.
	//
	// THIS MODE IS THE RISKY ONE AND THAT IS WHY IT IS NOT THE DEFAULT. It
	// depends on the min/max fields being read as EV100, which is exactly the
	// ambiguity that produced the all-white frame cited above. If mode 1 ever
	// renders white or black, check r.EyeAdaptation.ExposureFormat FIRST.
	const double SceneEV100 = kManualBaseEV100 - Bias;
	PP.bOverride_AutoExposureMethod = true;
	PP.AutoExposureMethod = AEM_Histogram;
	PP.bOverride_AutoExposureBias = true;
	PP.AutoExposureBias = 0.f; // the clamps carry the whole policy in this mode
	PP.bOverride_AutoExposureMinBrightness = true;
	PP.AutoExposureMinBrightness = (float)(SceneEV100 - kClampedAutoWindowStops);
	PP.bOverride_AutoExposureMaxBrightness = true;
	PP.AutoExposureMaxBrightness = (float)(SceneEV100 + kClampedAutoWindowStops);
}

// --- queries / clock control -----------------------------------------------

const FVoxelSkyState& UVoxelSkySubsystem::GetSkyState() const
{
	// A zeroed state rather than a null return when there is no Impl: every
	// caller of this is a HUD row or a log line, and none of them wants a
	// null-check more than it wants "midnight at the origin".
	static const FVoxelSkyState Empty;
	return Impl ? Impl->State : Empty;
}

void UVoxelSkySubsystem::SetEpochSeconds(double NewEpochSeconds)
{
	if (!Impl)
	{
		return;
	}
	Impl->EpochSeconds = NewEpochSeconds;
	// Force the next tick to re-orient regardless of where the cadence
	// accumulator happened to be. A deliberate clock jump that then waited up to
	// 100 ms for the lights to catch up would be a race with any capture taken
	// straight afterwards.
	Impl->bLightsEverApplied = false;
	Impl->LightUpdateAccumulator = 0.0;
}

void UVoxelSkySubsystem::SetTimeOfDay(double LocalHours)
{
	if (!Impl)
	{
		return;
	}
	const double DayLength = VoxelSky::GetDayLengthSeconds();
	const double DaysPerYear = VoxelSky::GetDaysPerYear();
	const int32 CurrentDayOfYear = DayOfYearFromEpoch(Impl->EpochSeconds, DayLength, DaysPerYear);

	int32 ResolvedDayOfYear = CurrentDayOfYear;
	const double NewEpoch = SolveEpochFor(LocalHours, CurrentDayOfYear, DayLength, DaysPerYear, ResolvedDayOfYear);
	SetEpochSeconds(NewEpoch);

	// Read-back log, same rule as Initialize's: the seasonal position is NOT
	// generally preserved exactly, because the reachable dates are quantised and
	// the new hour may not be reachable on the same one.
	UE_LOG(LogVoxelSky, Log,
	       TEXT("VoxelSky::SetTimeOfDay RESOLVED: %05.2f h, day-of-year %d (was %d) at epoch %.3f s."),
	       FMath::Frac(NewEpoch / DayLength) * 24.0, ResolvedDayOfYear, CurrentDayOfYear, NewEpoch);
}
