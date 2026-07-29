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
	// Declared up here ONLY so that the cvars below can use them as their
	// defaults -- one number, not a constant and a literal that drift apart. The
	// arguments for both values are long and live with the rest of the tuning
	// constants further down; search for kSunOuterSpaceIntensity before moving
	// either.
	constexpr float kSunOuterSpaceIntensity = 15.0f;
	constexpr float kMoonPeakIntensity = 0.04f;

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

	TAutoConsoleVariable<float> CVarSkySunIntensity(
		TEXT("voxel.Sky.SunIntensity"), kSunOuterSpaceIntensity,
		TEXT("The sun's OUTER-SPACE illuminance -- what the light carries ABOVE the atmosphere, not ")
		TEXT("what lands on the ground. Default 15.0. This is deliberately altitude-INDEPENDENT: ")
		TEXT("USkyAtmosphereComponent already applies air-mass extinction to the direct beam ")
		TEXT("(DirectionalLightComponent.cpp:592-595 GetSunIlluminanceOnGroundPostTransmittance = ")
		TEXT("OuterSpaceIlluminance * AtmosphereTransmittanceTowardSun, consumed by the deferred ")
		TEXT("directional pass at ShadowRendering.cpp:2676), and UE applies the N.L cosine on top of ")
		TEXT("that. Anything this file multiplies in as well is counted two or three times. See the ")
		TEXT("long comment above kSunOuterSpaceIntensity for why 15.0 reproduces the pre-W4 8-lux ")
		TEXT("ground pose to within 0.1 stop even though the curve it replaces is gone."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSkyMoonIntensity(
		TEXT("voxel.Sky.MoonIntensity"), kMoonPeakIntensity,
		TEXT("Peak moonlight, i.e. the intensity a FULL moon well clear of the horizon is given. ")
		TEXT("Default 0.04. AN ARTISTIC NUMBER, NOT A PHYSICAL ONE, AND THE SIZE OF THE LIE IS ")
		TEXT("RECORDED SO NOBODY 'CORRECTS' IT: a real full moon delivers ~0.25 lux against the sun's ")
		TEXT("~100000, a ratio of 1:400000 (18.6 stops). 0.04 against voxel.Sky.SunIntensity 15 is ")
		TEXT("1:375 (8.6 stops), so this moon is cheated ten stops brighter than the sky it claims to ")
		TEXT("model. That is intentional and universal -- every shipped game does it -- because a ")
		TEXT("physically scaled moon renders as literally nothing, and because scene colour down at ")
		TEXT("1e-5 is where fp16 stops carrying signal and starts carrying banding. Raise for a more ")
		TEXT("navigable night, lower for a harsher one; a NEW moon stays genuinely dark either way ")
		TEXT("because MoonIlluminatedFraction multiplies this (0 at new, 1 at full). This knob and the ")
		TEXT("night end of ExposureBiasForSunAltitude move the same pixel, so change one at a time."),
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
		TEXT("ExposureBiasForSunAltitude for why unclamped auto-exposure makes a dark ship render ")
		TEXT("mid-grey and every screenshot gate pass anyway."),
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

	// FLOORED AWAY FROM ZERO, and this floor is not defensive padding -- it is the
	// whole of defect 1. ULightComponent::UpdateColorAndBrightness
	// (LightComponent.cpp:1455-1477) computes
	//     bValidIntensity = Intensity > 0.f || GetLightUnits() == ELightUnits::EV
	// and UDirectionalLightComponent::GetLightUnits returns ELightUnits::Unitless
	// (DirectionalLightComponent.cpp:1509), so a directional light at EXACTLY zero
	// intensity is not a dim light -- it is MarkRenderStateDirty'd straight out of
	// FScene, and FScene::ProcessAtmosphereLightRemoval_RenderThread
	// (RendererScene.cpp:4721-4744) then clears AtmosphereLights[0]. See
	// ApplyLightsFromState for what that costs.
	float GetSunIntensity() { return FMath::Max(UE_KINDA_SMALL_NUMBER, CVarSkySunIntensity.GetValueOnAnyThread()); }

	// Clamped to >= 0 only: the moon IS allowed to reach exactly zero, because a
	// moon below the horizon dropping out of AtmosphereLights[1] costs nothing --
	// the SUN is what the sky is scattering, at every hour. It is only index 0
	// that must never leave the scene.
	float GetMoonIntensity() { return FMath::Max(0.f, CVarSkyMoonIntensity.GetValueOnAnyThread()); }

	float GetShadowUpdateHz() { return FMath::Max(0.f, CVarSkyShadowUpdateHz.GetValueOnAnyThread()); }

	int32 GetExposureMode() { return FMath::Clamp(CVarSkyExposureMode.GetValueOnAnyThread(), 0, 2); }

	float GetExposureBias() { return CVarSkyExposureBias.GetValueOnAnyThread(); }
} // namespace VoxelSky

// --- tuning constants ------------------------------------------------------

namespace
{
	// THE SUN'S INTENSITY IS AN OUTER-SPACE ILLUMINANCE AND IT DOES NOT VARY
	// WITH ALTITUDE. Read this before "restoring" a falloff curve here.
	//
	// What this number feeds. UDirectionalLightComponent's proxy returns
	// GetOuterSpaceIlluminance() == GetColor() (DirectionalLightComponent.cpp:
	// 582-585), and that value is used for TWO different things:
	//
	//   (1) THE SKY. SkyAtmosphereRendering.cpp:1586-1597 hands it to the sky
	//       LUT pass as AtmosphereLightIlluminanceOuterSpace0. This is the
	//       radiance the atmosphere SCATTERS, and it is the only thing that
	//       makes twilight exist at all.
	//   (2) THE GROUND. DirectionalLightComponent.cpp:592-595 defines
	//       GetSunIlluminanceOnGroundPostTransmittance() as OuterSpaceIlluminance
	//       * AtmosphereTransmittanceTowardSun, and the deferred directional pass
	//       reads it at ShadowRendering.cpp:2676. The transmittance factor is
	//       FAtmosphereSetup::GetTransmittanceAtGroundLevel
	//       (SkyAtmosphereCommonData.cpp:182-270), a real ray-march of the air
	//       mass along the sun direction.
	//
	// So the engine ALREADY dims and reddens the direct beam by air mass, and
	// ALREADY applies the N.L cosine on top of that. The curve this constant
	// replaced multiplied Intensity by sin(altitude)^1.3, which counted the
	// cosine twice and the extinction twice -- and, far worse, applied a
	// DIRECT-BEAM model to (1), the sky's source term, where it does not belong.
	// A sun a few degrees below the horizon has undiminished outer-space
	// illuminance; only its path to the ground has changed. Strangling (1) as the
	// sun set is why twilight rendered as 0.0% non-black pixels.
	//
	// WHY 15.0 SURVIVES THE REMOVAL OF THAT CURVE. The old calibration story was
	// "15.0 * SunIntensityFraction(38 deg) ~= 8.0, the pre-W4 static rig's
	// Intensity, which AVoxelClipmapActor's cave lock was A/B'd against
	// (VoxelClipmapActor.h:233-240)". Under the corrected model the equivalent
	// quantity is the illuminance that actually lands on flat ground at the same
	// default pose: 15.0 * sin(38 deg) * T(38 deg) ~= 15.0 * 0.616 * 0.80 ~= 7.4,
	// against the old 8.0. That is 0.11 stops, i.e. the shipped default pose is
	// preserved and the cave calibration does not move. Runtime knob:
	// voxel.Sky.SunIntensity.
	//
	// T(38) ~= 0.80 is arithmetic from UE's default atmosphere (zenith optical
	// depth ~0.14, air mass 1.62), NOT a measurement. It is the one number in
	// this paragraph the ladder re-run can falsify.
	//
	// (kSunOuterSpaceIntensity itself is declared at the top of this file, beside
	// kMoonPeakIntensity, only because voxel.Sky.SunIntensity needs it as its
	// default and a cvar's default has to be visible before the cvar.)

	// ONE colour temperature, held at every altitude, and the flat value is the
	// same correction as the flat intensity above.
	//
	// 5778 K is the sun's effective blackbody temperature -- what it is ABOVE the
	// atmosphere. The orange sunset is not a property of the sun, it is Rayleigh
	// scattering removing blue along a long air path, and
	// GetTransmittanceAtGroundLevel computes it PER CHANNEL and applies it to the
	// beam already. The ramp this replaced drove the light to 1900 K at the
	// horizon, which double-reddened the beam and -- because the same colour is
	// the sky's source term -- would have rendered the blue hour ORANGE. A blue
	// twilight requires scattering a WHITE source; that is where the blue comes
	// from. Do not reintroduce a horizon temperature ramp; tint the sky through
	// USkyAtmosphereComponent if the sunset wants art direction.
	constexpr float kSunTemperatureK = 5778.f;

	// The moon's peak (kMoonPeakIntensity, declared at the top of this file) is
	// reachable at runtime through voxel.Sky.MoonIntensity, because it is the
	// number in this file most likely to be argued about after a play session and
	// it should not need a rebuild to settle.
	//
	// NOTE FOR ANYONE COMPARING AGAINST THE PRE-FIX NUMBER: the old peak read
	// 0.12, but it was then multiplied by SunIntensityFraction(MoonAltitude),
	// which at a moon 18 deg up is 0.22 -- so the value that actually reached the
	// light was 0.026, and the 2.2 stops it lost were the same double-counted
	// extinction described above the sun constant. 0.04 applied straight through
	// is therefore ~0.6 stops BRIGHTER than the old 0.12 ever was in practice,
	// not three times dimmer. The ladder log's `moonIntensity=` field is the
	// number to compare against; the source constant is not.

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

	// The moon's own horizon fade. A GATE, NOT AN EXTINCTION MODEL -- the air
	// mass a low moon is shining through is the atmosphere's job for the same
	// reason it is the sun's (see kSunOuterSpaceIntensity), and applying a second
	// one here is exactly the bug being fixed. This exists only so the moonlight
	// does not switch on as a step the instant the disc clears the horizon, and
	// so that a moon below the horizon reaches a clean zero and leaves
	// AtmosphereLights[1] rather than lingering at a value nothing can see.
	constexpr double kMoonHorizonGateStartDeg = -1.0; // fully off
	constexpr double kMoonHorizonGateEndDeg = 3.0;    // fully on

	// Below this sun altitude the sun stops CASTING SHADOWS (it emphatically does
	// NOT stop existing -- see ApplyLightsFromState). By -2 deg the atmosphere's
	// transmittance toward the sun is ray-marching through the planet body, so
	// GetSunIlluminanceOnGroundPostTransmittance is already ~0 and there is
	// nothing left for a cascade to shadow; all that survives is the scattered
	// sky, which a whole-scene shadow setup contributes nothing to. This is what
	// recovers the "do not pay for shadows all night" saving that hiding the
	// light used to buy, without the side effect that made hiding fatal.
	//
	// 1 degree of hysteresis. ULightComponentBase::SetCastShadows
	// (LightComponent.cpp:79-87) MarkRenderStateDirty's, which destroys and
	// recreates the light's scene proxy -- harmless twice a day, silly twice a
	// second if the sun is hovering exactly on the threshold.
	constexpr double kSunShadowOffBelowDeg = -2.0;
	constexpr double kSunShadowOnAboveDeg = -1.0;

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

	// Smoothstepped 0..1 gate over the moon's own horizon crossing. See
	// kMoonHorizonGateStartDeg for why this is the ONLY altitude term the moon
	// gets and why it must not grow an extinction exponent.
	double MoonHorizonGate(double AltitudeDeg)
	{
		const double T = FMath::Clamp(
			(AltitudeDeg - kMoonHorizonGateStartDeg) /
				(kMoonHorizonGateEndDeg - kMoonHorizonGateStartDeg), 0.0, 1.0);
		return T * T * (3.0 - 2.0 * T); // no visible kink at either end
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
	// only PARTIALLY compensates: the scene's own ground illuminance drops about
	// nine stops from noon to a moonlit midnight (11.2 to 0.008 in this file's
	// units) and this curve lifts by 6.9 (+8.7 to +15.6). The two stops that do
	// not get lifted are what the player sees as darkness -- and because the
	// filmic toe steepens whatever it is handed down there, two scene stops read
	// as roughly five on screen. A curve that compensated fully would be
	// auto-exposure with extra steps.
	//
	// THE ANCHORS BELOW ARE DERIVED FROM THE W6 LADDER, NOT GUESSED -- but the
	// derivation still has one unmeasured leg and it is named at the end.
	//
	// The previous anchor set was seeded before any capture existed and held
	// +7.0 EV flat from +10 deg to the zenith. The first ladder run measured what
	// that produced (mean luminance out of 255, over one simulated day at 52.5 N):
	//
	//   sun +60.9 deg  intensity 12.59  bias +7.000  ->  luma 47.92   98.1% non-black
	//   sun +17.5 deg  intensity  3.15  bias +7.149  ->  luma  4.85   60.2% non-black
	//
	// Those two points calibrate the whole transfer function, because they share
	// a bias to within 0.15 stops. Ground illuminance across them differs by 3.98
	// stops (15 * sin(alt) * T(alt) at each) while the frame's DISPLAY-LINEAR
	// luminance differs by 4.33 stops, so display-linear ~ scene^1.13 through
	// this range -- a mild filmic toe, not the steep one a night frame suggests.
	// (The 00h00 rung looks far steeper, but at 1.5% non-black its mean is
	// measuring how many pixels exist, not how bright they are. It is not a
	// usable calibration point and was not used as one.)
	//
	// From that: each anchor below is the bias that lands a chosen mean luminance
	// at that altitude, given the scene's own ground illuminance there (the
	// standard clear-sky solar-altitude illuminance table, scaled so that
	// +60.9 deg == our 11.19). The chosen luminances run 106 at the zenith, 80 at
	// +20, 55 at +5, 38 at the horizon, and hold ~20 through twilight and night.
	// The gap between "what the scene did" and "what was chosen" IS the darkness:
	// the scene falls ~14 stops from noon to civil twilight and the curve lifts
	// ~7, so half the fall survives to the screen. That is the same partial-
	// compensation doctrine as before, now with the falls measured.
	//
	// WHY IT FLATTENS AT -2 DEG AND NOT AT -12. Below the horizon the scene falls
	// off a cliff -- roughly 4x PER DEGREE through civil twilight -- and a curve
	// that kept tracking it would need +20 EV by -6 deg. It cannot, because the
	// MOON is also on down there, and a full moon under a +20 EV exposure renders
	// brighter than noon. So the cap is set where a full moon reads as a full
	// moon (voxel.Sky.MoonIntensity and this cap move the same pixel; they were
	// solved together, and 15.6 / 0.04 is that solution). The price is paid by
	// MOONLESS civil twilight, which lands dimmer than the moonlit kind. That is
	// the honest trade for a curve driven by sun altitude alone, and it is the
	// owner's stated preference: a new moon should be genuinely dark.
	//
	// STILL UNMEASURED, and this is the one to check first if the re-run
	// disagrees: everything at or below 0 deg. Both twilight rungs read 0.0%
	// non-black before this change, so there is no observation of ANY below-
	// horizon frame to calibrate against -- those anchors come from the
	// illuminance table and the transfer function fitted above the horizon, and
	// they assume the SkyAtmosphere's twilight tracks the real one. If they are
	// wrong they will be wrong together and in one direction, which
	// voxel.Sky.ExposureBias shifts in one move without touching this table.
	double ExposureBiasForSunAltitude(double AltitudeDeg)
	{
		struct FAnchor { double AltDeg; double BiasEV; };
		// Altitudes ascending. Below the first and above the last the curve is
		// flat. The four night anchors are all equal on purpose -- the cap is
		// reached at -2 deg and everything below it is held there because the
		// moon, not the sun, is what the frame is lit by down there (above).
		static const FAnchor Anchors[] = {
			{-18.0, 15.6},  // astronomical night: the moon carries the frame here
			{-12.0, 15.6},  // nautical twilight
			{ -6.0, 15.6},  // civil twilight ends
			{ -2.0, 15.6},  // the cap; the scene keeps falling below this, the curve does not
			{  0.0, 14.1},  // sunrise/sunset
			{  2.0, 12.5},
			{  5.0, 11.3},
			{ 10.0, 10.5},
			{ 20.0,  9.8},
			{ 30.0,  9.4},
			{ 45.0,  8.9},
			{ 90.0,  8.7},  // full day
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

	// Latched state of the sun's CastShadows flag, so the hysteresis in
	// ApplyLightsFromState has something to compare against and so the flip is
	// logged once rather than every tick. Starts true because SpawnRig leaves the
	// light at ADirectionalLight's shadow-casting default; if the first tick
	// happens to be at night, the first flip is OFF and it is logged.
	bool bSunShadowsOn = true;

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
		       TEXT("Origin=(%.4f N, %.4f E) Moon=%d SunIntensity=%.4f MoonIntensity=%.4f ")
		       TEXT("ShadowUpdateHz=%.2f ExposureMode=%d ExposureBias=%.2f DayEV=%.2f NightEV=%.2f"),
		       VoxelSky::IsEnabled() ? 1 : 0, VoxelSky::GetTimeScale(), DayLength, DaysPerYear,
		       VoxelSky::GetOriginLatitudeDeg(), VoxelSky::GetOriginLongitudeDeg(),
		       VoxelSky::IsMoonEnabled() ? 1 : 0,
		       // Read back through the clamping accessors, never off the cvar: the
		       // sun's is FLOORED away from zero and a run that hit that floor has
		       // to be able to say so from its own log.
		       VoxelSky::GetSunIntensity(), VoxelSky::GetMoonIntensity(),
		       VoxelSky::GetShadowUpdateHz(),
		       VoxelSky::GetExposureMode(), VoxelSky::GetExposureBias(),
		       // The two ends of the retuned curve, logged so a capture states the
		       // exposure policy it ran under rather than the one in this revision
		       // of the source.
		       ExposureBiasForSunAltitude(60.0), ExposureBiasForSunAltitude(-18.0));
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
			// file -- bought for shadows cast by a light running ~8.6 stops below
			// the sun (voxel.Sky.MoonIntensity) while the exposure curve lifts
			// the whole frame nearly seven. It is not a close call today. Revisit
			// only with a number from the W7 leg.
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
		//
		// WHAT THE W6 EXPOSURE RETUNE DID TO THE STEP BETWEEN THE TWO VOLUMES.
		// Nothing about the ownership rule changed, but the SIZE of the jump at a
		// cave mouth did, and it is now asymmetric enough to be worth stating.
		// This curve used to run +7.0 (day) to +12.0 (night); it now runs +8.7 to
		// +15.6. The cave's +10 did NOT move -- it is still the only exposure
		// number in this project backed by an A/B, and re-deriving it to tidy up a
		// step would throw that away. The consequence:
		//
		//   walking into a cave at NOON   +8.7 -> +10.0   1.3 stops BRIGHTER (was 3.0)
		//   walking into a cave at NIGHT  +15.6 -> +10.0  5.6 stops DARKER   (was 2.0)
		//
		// The night step is the one to watch: a cave is now much darker than the
		// moonlit surface outside it, where it used to be slightly brighter. That
		// is arguably correct -- a cave at night has no light in it at all and the
		// lamp is the point -- but it is a behaviour change nobody asked for, and
		// if it reads badly the fix belongs in AVoxelClipmapActor's rig (make
		// CaveExposureEV100 track the sky's night end, re-measured), NOT in
		// flattening this curve's night end, which is doing separate work.
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
			// The static pose is a fixed -45 deg sun, i.e. permanently up, so it
			// casts shadows. Restored explicitly because the clock may have
			// switched them off before voxel.Sky.Enabled went to 0, and "off"
			// must mean the pre-W4 frame rather than the pre-W4 frame minus its
			// shadows.
			SunComp->SetCastShadows(true);
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
		Impl->bSunShadowsOn = true; // matches the SetCastShadows(true) just applied
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
	//
	// ===================================================================
	// THE SUN LIGHT IS NEVER HIDDEN, NEVER DEREGISTERED, AND NEVER SET TO
	// EXACTLY ZERO INTENSITY. All three are the same bug. Read before editing.
	//
	// This function used to end with SetVisibility(S.SunIntensity > 0.f), which
	// hid the sun below the horizon on the argument that a light nothing can see
	// should not be paying for shadow setup. The argument is fine. The mechanism
	// is fatal, and it is fatal three layers down:
	//
	//   1. ULightComponent::CreateRenderState_Concurrent (LightComponent.cpp:
	//      964-995) computes bHidden = !ShouldComponentAddToScene() ||
	//      !ShouldRender() || !bValidIntensity, and SKIPS World->Scene->AddLight
	//      entirely when it is true. A hidden light is not a dim light; it is not
	//      in FScene at all.
	//   2. FScene::ProcessAtmosphereLightRemoval_RenderThread (RendererScene.cpp:
	//      4721-4744) then clears AtmosphereLights[Index] and rescans the
	//      remaining lights for a replacement. There is no other index-0 light in
	//      this world, so AtmosphereLights[0] stays null.
	//   3. SkyAtmosphereRendering.cpp:1586-1597 branches on exactly that pointer:
	//      with it null the sky LUT pass is handed
	//      AtmosphereLightIlluminanceOuterSpace0 = FLinearColor::Black and a
	//      direction forced to straight up. The atmosphere integrates zero
	//      radiance, so the sky renders black -- and the SkyLight's real-time
	//      capture (SpawnRig) then captures that black sky as the ambient term,
	//      so the ground goes black too.
	//
	// That is the whole of "twilight renders 0.0% non-black". USkyAtmosphere
	// computes civil, nautical and astronomical twilight precisely FROM a sun a
	// few degrees below the horizon -- it is the one thing that needs the light
	// most, at exactly the altitude the old code removed it.
	//
	// AND THE OBVIOUS FIX IS ALSO WRONG. "Do not hide it, just let the intensity
	// fall to zero" reproduces the bug identically:
	// ULightComponent::UpdateColorAndBrightness (LightComponent.cpp:1455-1477)
	// treats Intensity == 0 on a unitless light as bNeedsToBeRemovedFromScene and
	// MarkRenderStateDirty's it straight back out through step 1 above.
	// UDirectionalLightComponent::GetLightUnits returns ELightUnits::Unitless
	// (DirectionalLightComponent.cpp:1509), so the ELightUnits::EV escape hatch in
	// that condition does not apply to us. Hence VoxelSky::GetSunIntensity's
	// floor.
	//
	// WHAT REPLACES THE SAVING. The direct beam still falls to zero as the sun
	// sets -- the atmosphere does it, per channel, by ray-marching the air mass
	// (see kSunOuterSpaceIntensity) -- and the shadow cost is dropped separately
	// by SetCastShadows below, which does NOT take the light out of FScene.
	// ===================================================================
	S.SunIntensity = VoxelSky::GetSunIntensity();
	S.SunTemperatureK = kSunTemperatureK;
	if (SunLight)
	{
		if (UDirectionalLightComponent* SunComp = Cast<UDirectionalLightComponent>(SunLight->GetLightComponent()))
		{
			SunComp->SetUseTemperature(true);
			SunComp->SetTemperature(S.SunTemperatureK);
			SunComp->SetIntensity(S.SunIntensity);

			// Belt and braces against a future edit, a Blueprint, or a level
			// tool: re-asserting the registration costs a branch (both setters
			// early-out when unchanged) and makes AtmosphereLights[0] an
			// invariant of this function rather than of SpawnRig running once.
			SunComp->SetVisibility(true);
			SunComp->SetAtmosphereSunLight(true);
			SunComp->SetAtmosphereSunLightIndex(0);

			// Shadow cadence, with hysteresis (see kSunShadowOffBelowDeg).
			const bool bWantShadows = Impl->bSunShadowsOn
				? S.SunAltitudeDeg > kSunShadowOffBelowDeg
				: S.SunAltitudeDeg > kSunShadowOnAboveDeg;
			if (bWantShadows != Impl->bSunShadowsOn)
			{
				Impl->bSunShadowsOn = bWantShadows;
				SunComp->SetCastShadows(bWantShadows);
				// Read back rather than echo: SetCastShadows early-outs when
				// AreDynamicDataChangesAllowed() is false, which is exactly the
				// case a Stationary-mobility regression would produce.
				UE_LOG(LogVoxelSky, Log,
				       TEXT("VoxelSky sun shadows %s at altitude %+.2f deg (CastShadows now %d). The light ")
				       TEXT("itself stays registered as atmosphere sun light 0 at every altitude."),
				       bWantShadows ? TEXT("ON") : TEXT("OFF"), S.SunAltitudeDeg,
				       SunComp->CastShadows ? 1 : 0);
			}
		}
	}

	// --- moon ---------------------------------------------------------------
	//
	// Three multiplicands, and each is a different question:
	//   IlluminatedFraction -- how much of the disc is lit (NOT PhaseFraction;
	//                          VoxelEphemeris.h:85-93 is explicit that a "half
	//                          moon" at PhaseFraction 0.25 is 50% lit). This is
	//                          the physical one, and it is what makes a new moon
	//                          genuinely dark without anything else changing.
	//   the horizon gate    -- a smoothstep across the moon's own rise, and
	//                          NOTHING MORE. The air mass a low moon shines
	//                          through is the atmosphere's job: the moon is
	//                          atmosphere sun light 1, so PrepareSunLightProxy
	//                          (SkyAtmosphereRendering.cpp:507-509, 584-595) gives
	//                          it its own transmittance exactly as it does the
	//                          sun's. The sin^1.3 curve that used to sit here was
	//                          a second copy of that extinction and cost a moon
	//                          18 deg up 2.2 stops for nothing.
	//   the daylight suppression -- a second atmosphere light contributing
	//                          during the day is invisible and not free.
	const bool bMoonOn = VoxelSky::IsMoonEnabled();
	double MoonFrac = 0.0;
	if (bMoonOn)
	{
		const double SunSuppress = 1.0 - FMath::Clamp(
			(S.SunAltitudeDeg - kMoonSunSuppressStartDeg) /
				(kMoonSunSuppressEndDeg - kMoonSunSuppressStartDeg), 0.0, 1.0);
		MoonFrac = MoonHorizonGate(S.MoonAltitudeDeg) * S.MoonIlluminatedFraction * SunSuppress;
	}
	S.MoonIntensity = (float)(VoxelSky::GetMoonIntensity() * MoonFrac);
	if (MoonLight)
	{
		if (UDirectionalLightComponent* MoonComp = Cast<UDirectionalLightComponent>(MoonLight->GetLightComponent()))
		{
			// Unlike the sun, the moon MAY reach zero and leave the scene. Losing
			// AtmosphereLights[1] costs nothing -- index 0 is what the sky is
			// scattering at every hour -- and a moon below the horizon should not
			// be contributing to it. SetIntensity early-outs on an unchanged
			// value, so this does not churn the render state while it sits at 0.
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
