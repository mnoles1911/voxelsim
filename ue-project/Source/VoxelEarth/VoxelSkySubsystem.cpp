#include "VoxelSkySubsystem.h"

#include "VoxelEarth.h"
#include "VoxelEarthGameMode.h" // VoxelEarthSpawn::ParseSpawnColumnUU -- see SpawnRig
#include "VoxelEphemeris.h"
#include "VoxelSkyDomeActor.h"
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
#include "Kismet/KismetMaterialLibrary.h" // the MPC writes -- see ApplySkyMaterialParams
#include "Materials/MaterialParameterCollection.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/StrongObjectPtr.h" // FVoxelSkyImpl is a plain struct; see SkyParams

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

	// THE MOON'S LIGHT ON THE GROUND. 0.04 -> 0.16 on 2026-08-11, +2.00 stops
	// exactly, because the owner asked for "the moon acting as a strong
	// directional light for the entire scene" and 0.04 was not one.
	//
	// WHAT 0.04 ACTUALLY PRODUCED, since a multiplier picked without stating that
	// is the thing this file's conventions forbid. Three numbers, all from the
	// round-21 settings line (Saved/owner-playtest-round21.log:1607):
	//   * against the sun:   15.0 / 0.04 = 375x = 8.55 stops below it.
	//   * against the frame: the exposure curve lifts deep night by
	//     DeepNightEV - DayEV = 13.60 - 8.83 = 4.77 stops (ExposureBiasForSunAltitude;
	//     higher EV is BRIGHTER here, it is a compensation and not a metering point).
	//     At round 21's 86%-illuminated moon the delivered light is 0.04 * 0.86, so
	//     the moonlit ground rendered log2(0.0344 / 15) + 4.77 = -4.00 stops below a
	//     noon ground. A sixteenth of daylight is a legible night, and it is NOT a
	//     "strong directional light" -- it is at the bottom of the tonemapper's toe
	//     where the shoulder has already crushed most of the contrast that would
	//     have made the direction readable.
	//   * against physics:   a real full moon is ~0.25 lux against the sun's
	//     ~120,000, i.e. 18.9 stops down, so this rig's physical moon would be
	//     15 / 480000 = 3.1e-5. 0.04 was 10.3 stops above that.
	//
	// 0.16 PUTS THE MOONLIT GROUND AT -2.00 STOPS BELOW A NOON GROUND
	// (log2(0.16 * 0.86 / 15) + 4.77 = -2.00), which is the classic day-for-night
	// key and is the point at which a directional light stops being "a tint on the
	// ambient" and starts modelling form. It is 6.55 stops below the sun and 12.3
	// stops above a physical moon -- so this constant's existing confession got two
	// stops worse, and that is the honest way to describe it.
	//
	// WHY +2 STOPS AND NOT +1 OR +3. +1 (0.08) is inside the range where "did that
	// change?" is a real question against a tonemapped frame, and the point of this
	// edit is to be unambiguous. +3 (0.32) puts night one stop below noon, at which
	// point the DeepNightDrop of 2.0 EV that keeps a full moon from outshining noon
	// is doing the only work holding it back -- i.e. two knobs fighting. +2 is also
	// exact and reversible: `voxel.Sky.MoonIntensity 0.04` restores every night
	// frame in the archive bit-for-bit, which is what makes this safe to ship
	// ahead of the owner's verdict.
	//
	// THIS IS AN AESTHETIC CALL AND IT IS THE OWNER'S, not a correctness claim.
	// The rungs are 0.04 (-4.0 stops, what shipped), 0.08 (-3.0), 0.16 (-2.0, this
	// default), 0.32 (-1.0). Live at voxel.Sky.MoonIntensity, unchanged.
	//
	// NOTE THE KNOCK-ON, because it is deliberate and not a side effect: the water
	// material's moon glint is scaled by MoonLightFraction, which is this value
	// divided by the sun's (ApplySkyMaterialParams). Dialling this knob moves the
	// moon path on the lake by the same number of stops as it moves the ground, so
	// the two can never be tuned against each other into disagreement.
	constexpr float kMoonPeakIntensity = 0.16f;
	constexpr float kMoonTemperatureK = 12000.f;
	// 0 = the moon is lit the colour the SPECTRUM says: the sun's own temperature,
	// warmed by lunar regolith (kMoonAlbedoTint). Was 1.0 -- the full artistic blue
	// shift -- until the owner asked for the moon's natural colour to be what the
	// directional light carries. The blue is still one cvar away; see
	// voxel.Sky.MoonTintStrength, which is the A/B and which nothing else depends on.
	constexpr float kMoonTintStrength = 0.0f;
	constexpr float kDeepNightDropEV = 2.0f;

	// THE COLOUR OF THE MOON ITSELF, measured off the pixels rather than asserted.
	//
	// Mean linear RGB of tools/sky-assets/lroc_color_poles_4k.tif -- the LROC
	// mosaic that IS T_MoonColor, the texture the disc samples -- sRGB-decoded and
	// averaged over all 4096x2048 texels: (0.33259, 0.32433, 0.29813), which
	// normalised to green is (1.0255, 1.0, 0.9192) at R:B = 1.116.
	//
	// So the moon is a very slightly WARM neutral grey, which is exactly what
	// kMoonTemperatureK's comment says the physics says and is the opposite of the
	// 12000 K blue that shipped. Multiplying this onto the sun's 5778 K blackbody
	// gives "sunlight reflected off lunar basalt" -- one colour, derived from the
	// same texture the disc is drawn with, so the light and the disc cannot drift
	// apart. That single-source property is the whole point; it is the same
	// argument sky_star_graph.py makes for one copy of the star derivation.
	//
	// NORMALISED TO MAX = 1, not to unit luminance, because ULightComponent stores
	// LightColor as an FColor -- eight bits per channel -- and anything above 1.0
	// clamps to 255 and silently loses the ratio. The luminance that costs is put
	// back in kMoonAlbedoTintLuminanceRecip below, so the tint changes the moon's
	// HUE and not one photon of its brightness.
	constexpr float kMoonAlbedoTintR = 1.0f;
	constexpr float kMoonAlbedoTintG = 0.9751f;
	constexpr float kMoonAlbedoTintB = 0.8963f;
	// Rec.709 luminance of the tint above (0.2126 R + 0.7152 G + 0.0722 B),
	// reciprocated. Dividing it back out is what makes voxel.Sky.MoonIntensity mean
	// the same number of lux it meant before the tint existed -- otherwise adding a
	// colour would have quietly darkened every night by 0.037 stops and the
	// exposure ladder would have been re-tuned against a bug.
	constexpr float kMoonAlbedoTintLuminanceRecip = 1.0f / 0.97484f;

	// THE MOON DISC'S GAIN, and the number that decides whether it reads as the
	// moon or as a light bulb.
	//
	// The emissive M_NightSky writes is MoonBrightness * T_MoonColor * shading
	// (create_sky_material.py, build_moon). MPC_VoxelSky shipped MoonBrightness =
	// 20.0 and NOTHING IN C++ EVER DROVE IT, so 20.0 is not a tuned value -- it is
	// an asset default nobody ever measured.
	//
	// MEASURED, and it is not close. T_MoonColor's mean linear luminance is 0.324
	// (the LROC mosaic is stretched for display; the real moon's albedo is ~0.12),
	// so at gain 20 the disc emits ~5.4 and clips to a featureless white blob at
	// every night exposure this project uses. That is what the owner reported as
	// "a partial textured phase and the rest is just a glowing blob": the ONLY
	// place the texture survived was the terminator, where the shading term pulled
	// it back under 1.0.
	//
	// WHAT PHYSICALLY CORRECT WOULD BE, so the size of this cheat is on record.
	// A Lambertian disc lit by voxel.Sky.SunIntensity has radiance
	// SunIntensity/pi * albedo = 15/pi * 0.12 = 0.573, and since T_MoonColor
	// already carries 0.324 rather than 0.12, the gain that reproduces it is
	// (15/pi) * (0.12/0.324) = 1.77. So the shipped 20.0 was ELEVEN TIMES brighter
	// than a physically correct moon, and physically correct would ITSELF still
	// clip -- as it does in every photograph ever taken of the moon against stars.
	// There is no exposure that shows a star field and an unclipped physical moon;
	// the eye only manages it by adapting locally, which a single global tonemapper
	// cannot do.
	//
	// 0.15 IS CALIBRATED, NOT PICKED. It is the gain at which the maria read and
	// the brightest highlands sit just under clipping at the deep-night exposure --
	// established by re-shooting the owner's own 23:00 pose against a measured
	// bracket, not by eye. Runtime knob: voxel.Sky.MoonDiscBrightness.
	constexpr float kMoonDiscBrightness = 0.15f;

	// The moon disc's DRAWN angular radius in degrees, and the number that makes
	// the moon a thing you can see rather than a thing the maths agrees is there.
	//
	// The real moon's angular radius is 0.26 deg (0.52 deg across). At this
	// project's 2200x1300 / 90 deg horizontal FOV that is TEN PIXELS wide, and ten
	// pixels of moon at 51 deg altitude -- above the top edge of a 61 deg vertical
	// FOV whenever the player is looking level -- is a moon nobody finds. It is
	// what the owner reported as "there is no moon" in round 15.
	//
	// HOW WE GOT HERE, because both halves of this were correct in isolation and
	// the pair was not. M_NightSky drew the moon at 1.63 deg radius for its whole
	// life, not by choice but because UMaterialExpressionSine defaults Period to
	// 1.0 and so computed sin(2pi*x) -- every angle in the sky graphs multiplied by
	// 6.283 (sky_star_graph.py, _PERIOD_RADIANS). Fixing that trap was right, and
	// it silently took the moon from 63 pixels to 10 in the same commit that fixed
	// the star field's hemisphere. NOBODY CHOSE 0.52 DEG. It arrived as the
	// side-effect of a bug fix, and this constant is the first time the drawn size
	// has been a decision.
	//
	// 2.34 IS NINE TIMES PHYSICAL, and it is the owner's call on a screenshot, not
	// a derivation. It went 0.26 (physical, ~10 px, "there is no moon") -> 0.78
	// (3x, ~30 px) -> 2.34 (9x, ~90 px at 2200x1300 / 90 deg FOV) after he reviewed
	// the first two and asked for three times more, explicitly waiving real
	// earth-to-moon sizing. Recorded as HIS decision so nobody "corrects" it back
	// toward physical and quietly reintroduces a moon nobody can find.
	//
	// Cheating it is also the same call this file already makes one screen up for
	// the moon's LIGHT: kMoonPeakIntensity is ten stops brighter than physics for
	// exactly the same reason, and says so at length. A moon that lights the ground
	// ten stops harder than the real one while drawing at strict physical size is
	// not "more accurate", it is inconsistent. Every shipped game enlarges the moon.
	//
	// AT THIS SIZE THE TEXTURE IS THE POINT. Ninety pixels is enough for the maria
	// to read, which is exactly why kMoonDiscBrightness had to stop clipping: a
	// 10-pixel white dot reads as a moon, a 90-pixel white dot reads as a bug.
	// The two constants were settled together and should move together.
	//
	// Runtime knob: voxel.Sky.MoonAngularRadiusDeg. 0.26 is physical truth, 1.63 is
	// what the owner saw before the Period fix, and both are one console command
	// away -- this is a judgement to settle on a screenshot, so it is a live knob
	// and not a rebuild.
	constexpr float kMoonDrawnAngularRadiusDeg = 2.34f;

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
		TEXT("voxel.Sky.DayLengthSeconds"), 3600.0f,
		TEXT("Wall-clock seconds per game day. Default 2400 = a 40-minute day. DOUBLED from 1200 ")
		TEXT("on 2026-08-09: at 1200 the sky visibly moved while you stood still. This is the one ")
		TEXT("knob that sets the whole sky's angular rate -- the sun, the moon and the star field ")
		TEXT("all derive from frac(epoch / DayLengthSeconds), so doubling it halves every one of ")
		TEXT("them together and nothing can drift out of step with anything else. Dawn and dusk ")
		TEXT("now run ~100 s each at 52 N. This is the DIURNAL clock only; the seasonal one is ")
		TEXT("DaysPerYear below and the two are deliberately decoupled -- see VoxelEphemeris.h's ")
		TEXT("JulianDayFromGameClock comment. NOTE the knock-on there: a game YEAR is ")
		TEXT("DayLength * DaysPerYear, so a doubled day also doubles the year to 32 h of play, ")
		TEXT("and the calendar's reachable dates stay 365.2425/DaysPerYear = 7.6 real days apart ")
		TEXT("-- the DATE spacing does not change, only how long you wait between dates."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSkyDaysPerYear(
		TEXT("voxel.Sky.DaysPerYear"), 48.0f,
		TEXT("Game days per game year. Default 48, i.e. a season per ~8 hours of play at the ")
		TEXT("default day length (48 * 2400 s = 32 h per year, /4 = 8 h per season). NOTE the ")
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
		TEXT("Default 0.16, RAISED FROM 0.04 on 2026-08-11 (+2.00 stops exactly) after the owner asked ")
		TEXT("for a moon that lights the scene. AN ARTISTIC NUMBER, NOT A PHYSICAL ONE, AND THE SIZE OF ")
		TEXT("THE LIE IS RECORDED SO NOBODY 'CORRECTS' IT: a real full moon delivers ~0.25 lux against ")
		TEXT("the sun's ~120000, a ratio of 1:480000 (18.9 stops). 0.16 against voxel.Sky.SunIntensity 15 ")
		TEXT("is 1:94 (6.6 stops), so this moon is cheated TWELVE stops brighter than the sky it claims ")
		TEXT("to model -- two worse than the ten 0.04 confessed to. That is intentional and universal -- ")
		TEXT("every shipped game does it -- because a physically scaled moon renders as literally ")
		TEXT("nothing, and because scene colour down at 1e-5 is where fp16 stops carrying signal and ")
		TEXT("starts carrying banding. THE RUNGS, in stops below a NOON ground once the exposure curve's ")
		TEXT("4.77-stop night lift is counted: 0.04 = -4.0 (what shipped through round 21), 0.08 = -3.0, ")
		TEXT("0.16 = -2.0 (this default, the day-for-night key), 0.32 = -1.0. Raise for a more navigable ")
		TEXT("night, lower for a harsher one; a NEW moon stays genuinely dark either way because ")
		TEXT("MoonIlluminatedFraction multiplies this (0 at new, 1 at full). ALSO MOVES THE WATER: the ")
		TEXT("moon glint in M_WaterVoxel is scaled by MoonLightFraction = this / SunIntensity, so the ")
		TEXT("moon path on the lake tracks this knob stop for stop and cannot drift from the ground. ")
		TEXT("This knob and the night end of ExposureBiasForSunAltitude move the same pixel, so change ")
		TEXT("one at a time."),
		ECVF_Default);

	// MOON SHADOWS, OFF BY DEFAULT, AND THE DEFAULT IS THE MEASURED CALL.
	//
	// This exists because raising kMoonPeakIntensity two stops changes the terms of
	// the argument recorded at SpawnRig's SetCastShadows(false) -- that argument was
	// "shadows from a light 8.6 stops below the sun, under an exposure curve that
	// lifts the frame nearly seven, are not worth a second whole-scene cascade set",
	// and the light is now 6.6 stops down rather than 8.6. It is a closer call than
	// it was. It is NOT a call this file should make silently in the same change
	// that made it close, so the mechanism ships reachable and off.
	//
	// WHAT IT COSTS, stated rather than guessed at: a second shadow-casting
	// directional light is a second FProjectedShadowInfo set -- DynamicShadowCascades
	// whole-scene cascades, each with its own depth pass over every shadow-casting
	// primitive in range. The 2026-07-27 draw-path diagnosis measured the sun's
	// gathers at ~4-5 per frame, each submitting against the whole pool, on a frame
	// that is RENDER-THREAD BOUND (voxelsim-draw-path-2k). Turning this on is
	// therefore expected to roughly DOUBLE the shadow half of a night frame, and
	// night is not a cheap frame here. NOT MEASURED at 0.16 -- no leg has been run
	// with this on, and this comment must be updated with a number by whoever does.
	//
	// WHY IT MIGHT STILL BE WORTH IT. Without shadows the moonlight is a flat wash:
	// every up-facing surface in the world gets the same 0.16 regardless of what is
	// above it, so a cliff overhang, a tree canopy and open ground are equally lit.
	// That is a large part of what "the moon does not read as a light source" means
	// perceptually -- a key light with no shadow does not model space. The two stops
	// added above make the wash BRIGHTER, which makes its flatness MORE visible, not
	// less. So if the owner turns the moon up and it still reads wrong, this is the
	// next thing to try, and it is one console command away.
	TAutoConsoleVariable<int32> CVarSkyMoonShadows(
		TEXT("voxel.Sky.MoonShadows"), 0,
		TEXT("Whether the MOON casts shadows. Default 0 = OFF, which is what has always shipped. 1 = ON, ")
		TEXT("and it is a real cost: a second shadow-casting directional light is a second whole-scene ")
		TEXT("cascade set and a second depth pass over every caster, on a frame this project has ")
		TEXT("measured as render-thread bound. NOT YET MEASURED at the current moon intensity -- if you ")
		TEXT("run it, put the number in the comment above this cvar. Turn it on if moonlight reads FLAT ")
		TEXT("rather than DIM: raising voxel.Sky.MoonIntensity fixes dim, and only shadows fix flat, ")
		TEXT("because without them every up-facing surface gets identical moonlight whatever is above it. ")
		TEXT("Gated by the same horizon logic as the moonlight itself, so a moon that is down or new ")
		TEXT("never pays for a cascade even with this at 1."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSkyMoonAngularRadiusDeg(
		TEXT("voxel.Sky.MoonAngularRadiusDeg"), kMoonDrawnAngularRadiusDeg,
		TEXT("How big the moon DISC is drawn, as an angular RADIUS in degrees (so the disc is twice ")
		TEXT("this across). Default 2.34 = NINE times the real moon's 0.26, deliberately -- see ")
		TEXT("kMoonDrawnAngularRadiusDeg for the argument, which is the same one kMoonPeakIntensity ")
		TEXT("makes for cheating the moon's LIGHT ten stops. Useful values: 0.26 physical truth (ten ")
		TEXT("pixels wide at 2200x1300/90deg, which is what 'there is no moon' looked like); 1.63 the ")
		TEXT("accidental pre-fix size the owner last saw a moon at; 2.34 default (~90 px). THIS ONLY MOVES THE ")
		TEXT("DISC. Moonlight is voxel.Sky.MoonIntensity and the two are independent, so a bigger moon ")
		TEXT("does not brighten the ground by one photon -- if the pair are meant to move together ")
		TEXT("that has to be done on purpose. Written into MPC_VoxelSky.MoonAngularRadius every frame, ")
		TEXT("so editing the asset default does nothing; this cvar is the knob."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSkyMoonDiscBrightness(
		TEXT("voxel.Sky.MoonDiscBrightness"), kMoonDiscBrightness,
		TEXT("Gain on the moon DISC's emissive, multiplying T_MoonColor. Default 0.15, CALIBRATED so ")
		TEXT("the maria read and the highlands sit just under clipping at the deep-night exposure. ")
		TEXT("This replaces an MPC asset default of 20.0 that nothing ever drove and nobody ever ")
		TEXT("measured -- eleven times brighter than a physically correct moon, which is why the disc ")
		TEXT("rendered as a white blob with texture visible only along the terminator. See ")
		TEXT("kMoonDiscBrightness for the full arithmetic, including why the PHYSICALLY correct 1.77 ")
		TEXT("would also clip. Raise it and you get the blob back; that is the honest trade and it is ")
		TEXT("the same one every photograph of the moon makes. THIS IS THE DISC ONLY -- the ground is ")
		TEXT("lit by voxel.Sky.MoonIntensity, and the two are deliberately separate because one is ")
		TEXT("bounded by the tonemapper and the other by the exposure ladder."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSkyMoonTemperatureK(
		TEXT("voxel.Sky.MoonTemperatureK"), kMoonTemperatureK,
		TEXT("Moonlight colour temperature, Kelvin. Default 12000, i.e. DELIBERATELY BLUER THAN THE SUN'S ")
		TEXT("5778. THIS IS PERCEPTUAL CONVENTION, NOT SPECTROSCOPY, and the physical fact is recorded ")
		TEXT("beside kMoonTemperatureK so nobody 'corrects' it back: the moon is grey basalt at albedo ")
		TEXT("~0.12 reflecting sunlight, so its raw spectrum is marginally WARMER than the sun's, not ")
		TEXT("cooler. Every photograph and every film renders night cool anyway, and human scotopic ")
		TEXT("vision genuinely shifts blue (the Purkinje effect), so a warm moon reads as a rendering ")
		TEXT("mistake to any viewer. NOTE THE AXIS RUNS BACKWARDS from the word 'cool': LOWER Kelvin is ")
		TEXT("WARMER light, which is exactly how this shipped at 4100 and rendered the terrain ")
		TEXT("yellow-brown. UE clamps this to 1000..15000 inside FColorSpace::MakeFromColorTemperature, ")
		TEXT("so 15000 is the hard ceiling; use voxel.Sky.MoonTintStrength for the rest. Changing this ")
		TEXT("does NOT change how bright the moon is -- the blackbody colour is normalised to unit ")
		TEXT("luminance (Y=1 in XYZ) at every temperature -- so this knob and voxel.Sky.MoonIntensity are ")
		TEXT("genuinely independent, unlike this knob and the exposure curve."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSkyMoonTintStrength(
		TEXT("voxel.Sky.MoonTintStrength"), kMoonTintStrength,
		TEXT("How far to travel from the physically honest moon colour toward voxel.Sky.MoonTemperatureK. ")
		TEXT("0 = the moon is given the SUN's temperature, which is what the spectrum actually says (it ")
		TEXT("is reflected sunlight); 1 = the full artistic blue shift, DEFAULT. This is the A/B control ")
		TEXT("for 'is the night blue because we chose it or because the sky is', and it is a separate ")
		TEXT("knob from the temperature itself so that the SIZE of the lie is adjustable without ")
		TEXT("relitigating the target. Interpolated in MIRED (1e6/K), not in Kelvin: equal steps in ")
		TEXT("Kelvin are wildly unequal steps in perceived colour up at the blue end, so a Kelvin lerp ")
		TEXT("would spend most of its travel in the first tenth of the slider."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSkyDeepNightDropEV(
		TEXT("voxel.Sky.DeepNightDropEV"), kDeepNightDropEV,
		TEXT("Stops the exposure curve gives back once the sun is below astronomical twilight, on top of ")
		TEXT("the +15.6 EV twilight cap. Default 2.0. 0 RESTORES THE PREVIOUS FLAT CAP EXACTLY and is the ")
		TEXT("control arm for this whole change. Why it exists: the cap alone cannot tell a -14 deg ")
		TEXT("midsummer midnight from a -61 deg midwinter one, and since USkyAtmosphere has no airglow or ")
		TEXT("starlight term its sky is equally dead at both, so BOTH used to render as whatever the moon ")
		TEXT("happened to be doing -- making a high winter full moon the BRIGHTEST night of the year. See ")
		TEXT("DeepNightDropForSunAltitude for the altitude band and why it is where it is. This only ever ")
		TEXT("DARKENS, so it cannot break the constraint the +15.6 cap was set to satisfy (a full moon ")
		TEXT("must not out-render noon)."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSkyShadowUpdateHz(
		TEXT("voxel.Sky.ShadowUpdateHz"), 10.0f,
		TEXT("Cap on how often the sun/moon are actually RE-ORIENTED, in Hz. Default 10. A ")
		TEXT("continuously rotating directional light invalidates UE's cached whole-scene shadow ")
		TEXT("setup every single frame; stepping it instead quantises shadow motion, which is ")
		TEXT("visible as a pop if the step is coarse. 10 Hz is a starting guess, NOT a measurement ")
		TEXT("-- at the default 2400 s day the sun moves 0.015 deg per step, which should be below ")
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

	TAutoConsoleVariable<int32> CVarSkyDomeEnabled(
		TEXT("voxel.Sky.DomeEnabled"), 1,
		TEXT("Draw the night-sky dome -- the stars and the textured, phased moon disc (AVoxelSkyDomeActor + ")
		TEXT("/Game/Voxel/M_NightSky). 1 = on (default). 0 HIDES THE MESH ONLY: the light rig, the ")
		TEXT("SkyAtmosphere, the exposure curve and the MPC writes are all untouched, so this is a clean A/B ")
		TEXT("for 'how much of the night frame is the star dome' rather than a night-off switch. Live -- ")
		TEXT("toggling it at runtime shows/hides within a frame and logs the transition. NOTE the moon's flat ")
		TEXT("atmosphere disc is suppressed unconditionally (SetAtmosphereSunDiskColorScale in SpawnRig), so ")
		TEXT("with this at 0 there is NO moon disc at all, not the old untextured one."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSkyDomeRadiusUU(
		TEXT("voxel.Sky.DomeRadiusUU"), 2.0e7f,
		TEXT("Radius of the night-sky dome, in Unreal units. Default 2e7 = 200 km. THIS IS A LOWER BOUND ")
		TEXT("PROBLEM, NOT A TUNING KNOB: M_NightSky keeps depth testing on so that a mountain occludes the ")
		TEXT("stars behind it, which means the dome must be FARTHER than the farthest drawn geometry or ")
		TEXT("distant terrain is drawn in front of the sky instead. The binding constraint is ")
		TEXT("AVoxelClipmapActor, whose outermost half-extent is 16x the ring cascade's outer radius -- 65.5 ")
		TEXT("km at the shipped 4096 m cascade, whose CORNER is 92.7 km -- so the default clears it by 2.16x. ")
		TEXT("Raise it if -VoxelRingOuterMeters grows the cascade; AVoxelSkyDomeActor::BeginPlay measures the ")
		TEXT("actual extent and logs an Error if this loses. There is no upper clip to worry about: UE's ")
		TEXT("default projection has an infinite far plane. Live."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarSkyAtmosphereDome(
		TEXT("voxel.Sky.AtmosphereDome"), 1,
		TEXT("Draw the canonical IsSky dome -- AVoxelSkyDomeActor's SECOND sphere, carrying ")
		TEXT("/Game/Voxel/M_SkyAtmosphereDome. 1 = on (default). THIS IS NOT A COSMETIC TOGGLE: while the dome ")
		TEXT("is in the scene View.bSceneHasSkyMaterial is true and the SkyAtmosphere's own full-screen pass ")
		TEXT("STOPS PAINTING SKY PIXELS (SkyAtmosphereRendering.cpp:2214 sets bRenderSkyPixel = ")
		TEXT("!bSceneHasSkyMaterial, and SkyAtmosphere.usf:982-992 then clips far-depth pixels out), so the dome ")
		TEXT("is the thing painting the sky. 0 hands the sky back to the atmosphere pass. Which means this cvar ")
		TEXT("is exactly an A/B on WHO PAINTS THE SKY, and it must agree to within 2/255 of mean luma at every ")
		TEXT("hour -- see docs/sky-and-local-light-plan.md S1 and run it with -VoxelSkyLadder=N ")
		TEXT("-VoxelSkyLadderAltCvar=voxel.Sky.AtmosphereDome, which pairs each hour on/off inside ONE process ")
		TEXT("(the cross-session screenshot floor is 1.81%%, the within-session floor 0.00%%). LIVE -- toggling ")
		TEXT("at runtime shows/hides within a frame and logs the transition; a spawn-time-only switch would make ")
		TEXT("that gate unreadable. REFUSED, with an Error, if M_SkyAtmosphereDome failed to load: an IsSky dome ")
		TEXT("wearing the engine default material would suppress the atmosphere pass AND paint the sky wrong."),
		ECVF_Default);

	// --- the star ambient calibration ----------------------------------------
	//
	// THE ONE CONSTANT THAT TIES "HOW BRIGHT STARS LOOK" TO "HOW MUCH THEY LIGHT
	// THE WORLD", and it is CALIBRATION, NOT PHYSICS. It converts between two
	// renderer conventions carrying the SAME star map:
	//
	//   VISIBLE   M_NightSky, BLEND_Additive, composited onto already-composited
	//             scene colour in the main view (Tools/create_sky_material.py:628).
	//   AMBIENT   M_SkyAtmosphereDome's star branch, emissive into the SkyLight's
	//             real-time cubemap behind a ReflectionCapturePassSwitch, then
	//             SH-projected into irradiance
	//             (Tools/create_sky_atmosphere_dome_material.py:286-289).
	//
	// Nothing makes those two paths owe each other the same number, so the number
	// is MEASURED. Starlit capture, 22h00 on 03-20 with the Milky Way core high
	// (Saved/Screenshots/WindowsEditor/VoxelSkyLadder_00_22h00_StarAmbientGain1,
	// +3 EV bias for visibility -- the bias CANCELS in a ratio, which is exactly
	// why the gate below is a ratio and not an absolute level). Proper sRGB EOTF,
	// clipped pixels excluded, Y = 0.2126R + 0.7152G + 0.0722B:
	//
	//   whole visible sky   0.14342 linear        Milky Way core   0.27833
	//   snowfield ground    0.11325               ground / sky     0.790
	//
	// For a Lambertian surface under a sky of luminance L_sky the illuminance is
	// E = pi * L_sky and the surface reads rho * E / pi, so L_ground / L_sky IS the
	// albedo -- every geometry term cancels, which is what makes this measurable off
	// one screenshot with no light meter. Snow is conventionally 0.75..0.85 and it
	// measured 0.790. So the shipped pair (StarGain 0.15, ambient gain 1.0) is
	// physically consistent, and the calibration constant IS that consistent ratio.
	//
	// RE-MEASURE IF EITHER DOME'S MATERIAL CHANGES. This is a fit to two specific
	// material graphs, not a derived quantity: change the star branch's gain chain,
	// the horizon fade, T_SkyStarmap's encoding, or M_NightSky's blend mode and this
	// constant is stale with no error anywhere. tools/sky-albedo-check.py IS the
	// re-measurement, and it exits non-zero, so it can gate.
	constexpr float kStarAmbientCalibration = 1.0f;

	// The visible star gain the calibration was measured against. NOT a factor in
	// the derivation -- see the next paragraph -- it is logged as the anchor so a
	// capture can state what its ambient gain was calibrated at, and it is what to
	// compare against if voxel.Sky.StarGain's default ever moves.
	constexpr float kStarAmbientCalibrationStarGain = 0.15f;

	// WHY THE DERIVED VALUE IS A CONSTANT AND NOT "StarGain / 0.15", WHICH IS THE
	// OBVIOUS-LOOKING SHAPE AND IS A DOUBLE COUNT.
	//
	// Both domes multiply the SAME MPC scalar, StarBrightness, and this subsystem
	// writes it as StarBrightnessForSunAltitude() = sunriseFade * voxel.Sky.StarGain
	// (see ApplySkyMaterialParams). So:
	//
	//   visible radiance  V = StarMap * fade * StarGain
	//   capture radiance  C = StarMap * fade * StarGain * StarAmbientGain
	//   ==> C / V = StarAmbientGain, exactly, with no StarGain left in it.
	//
	// StarGain therefore ALREADY propagates to the ambient term, linearly, through
	// the shared scalar -- the two gains are not independent today and never were.
	// Making the derived value proportional to StarGain would multiply it in a
	// SECOND time and give the ambient term a quadratic response: at StarGain 0.30
	// the stars would look twice as bright and light the world four times as hard,
	// which is precisely the appearance/illumination mismatch this constant exists
	// to prevent, introduced by the fix for it. The honest coupling is that
	// StarAmbientGain is a DIMENSIONLESS CONVERSION FACTOR and its value is 1.0.
	// (A second reason not to write it as a product: 0.15f * 6.667f is 1.0000501f,
	// so the "obvious" spelling is not even a bit-identical no-op at the defaults.)
	//
	// AND WHY THE AMBIENT BRANCH NEEDS NO SUNRISE FADE OF ITS OWN. It already HAS
	// one, the visible dome's, for free: StarBrightness carries it, so above 0 deg
	// solar altitude StarBrightnessForSunAltitude returns exactly 0 and the capture
	// branch is IDENTICALLY zero at noon whatever this gain is. That -- not merely
	// "the atmosphere swamps it" -- is why the S2 day rungs came back unchanged to
	// 0.02/255 (101.888 -> 101.866). Adding a second fade here would be a second
	// curve to keep in sync with the first, and the sky would then be capable of
	// fading its light and its look on different schedules.

	// The star branch of M_SkyAtmosphereDome's REFLECTION-pass output, i.e. how
	// much starlight the SkyLight's real-time capture integrates into the world's
	// ambient term.
	//
	// DEFAULT -1, WHICH MEANS "DERIVE FROM kStarAmbientCalibration". Any value
	// >= 0 is an explicit override; the whole negative half-line is the sentinel.
	// It used to default to a bare 1.0, and that was the defect: a second free gain
	// sitting beside voxel.Sky.StarGain, in a different rendering context, with
	// nothing tying them together. They agreed. Nothing enforced it, nothing would
	// have said so if they stopped, and the symptom -- a sky whose brightness does
	// not match its illumination -- is not a shape anyone recognises as a bug.
	//
	// THE WHOLE NEGATIVE HALF-LINE, not just exactly -1, because the alternative is
	// worse in both directions: a typo'd -0.5 must not reach the material (a
	// negative gain SUBTRACTS radiance from the capture, and a SkyLight irradiance
	// SH with negative lobes darkens the surfaces facing the Milky Way -- that reads
	// as "the band is upside down", not as bad input), and it must not silently
	// become 0 either, because "starlight quietly switched off" is the one outcome
	// no ladder rung would flag. Both collapse into "derive", which is loud in the
	// log and correct in the frame. NOTE the consequence: -1 does NOT mean "off".
	// 0 means off.
	//
	// WHY IT CANNOT LIVE ON THE MPC ASSET AS A TUNING KNOB: ApplySkyMaterialParams
	// writes StarAmbientGain every frame, so the asset default is overwritten
	// before anything renders. Same trap StarBrightness and StarRotation both hit
	// -- once C++ drives a collection scalar, that scalar's asset default is dead
	// as a tuning surface (see CVarSkyStarGain and CVarSkyStarRotationOffsetTurns).
	// S2 MEASURED AND ADOPTED 1.0. The gate ran this cvar as its A arm against 0,
	// paired at one frozen epoch and pose, winter 12-21 (docs/measurements has the
	// table). Ground-crop mean luma:
	//
	//   18h00 moonless   0.050 -> 5.756      21h00 moonless   0.050 -> 4.415
	//   00h00 moonlit    7.826 -> 12.717     12h00 daylight  101.888 -> 101.866
	//
	// All three conditions of the S2 gate hold at 1.0: moonless ground leaves black,
	// moonless stays strictly BELOW moonlit, and daylight is unchanged to 0.02/255
	// because the star branch only exists in the capture pass. That is the value the
	// calibration constant now carries, so the shipped behaviour is unchanged and
	// the S2 measurement still stands behind it. It stays an OVERRIDABLE cvar
	// because the S2 gate A/Bs it directly, arm A 1.0 against arm B 0, and because
	// it multiplies against the exposure curve's deep-night lift.
	TAutoConsoleVariable<float> CVarSkyStarAmbientGain(
		TEXT("voxel.Sky.StarAmbientGain"), -1.0f,
		TEXT("Gain on the star map inside the SkyLight's real-time capture, via ")
		TEXT("MPC_VoxelSky.StarAmbientGain and M_SkyAtmosphereDome's ReflectionCapturePassSwitch. DEFAULT -1, ")
		TEXT("WHICH MEANS DERIVE: the effective gain becomes the measured star-ambient calibration constant ")
		TEXT("(1.0), so appearance and illumination cannot be tuned apart by accident. Any value >= 0 is an ")
		TEXT("explicit override and is used verbatim; every negative value means derive, so -1 does NOT mean ")
		TEXT("'off' -- 0 means off. The run log states the RESOLVED gain and whether it was DERIVED or ")
		TEXT("OVERRIDDEN, so read that rather than this default. Measured at S2: 1.0 takes moonless ground ")
		TEXT("from 0.05 to ~5 mean luma while leaving daylight unchanged to 0.02/255. It multiplies ")
		TEXT("StarBrightness (the sunrise fade, ALREADY carrying voxel.Sky.StarGain) and the horizon fade, ")
		TEXT("which is why the derived value is a constant and not proportional to StarGain -- StarGain is ")
		TEXT("already in there once, and putting it in twice would make ambient starlight quadratic. Then it ")
		TEXT("rides the exposure curve's up-to-15.6-stop night lift, so the S2 gate is two-sided: winter ")
		TEXT("night ground luma must rise above 0.5 and stay STRICTLY BELOW the moonlit 17.4-25.5, or ")
		TEXT("'brighter' just means 'washed grey' and every one-sided gate passes anyway. Does NOT affect ")
		TEXT("the main view: the switch reads View.RenderingReflectionCaptureMask (Common.ush:2306-2309), ")
		TEXT("which is 0 on screen. It IS also read by ordinary reflection captures; this project ships ")
		TEXT("none. tools/sky-albedo-check.py re-measures the calibration off a starlit capture."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarSkyStarRotationOffsetTurns(
		TEXT("voxel.Sky.StarRotationOffsetTurns"), 0.0f,
		TEXT("Constant offset added to the star map's rotation, in TURNS (1.0 = a full revolution). Default ")
		TEXT("0.0. WHY THIS EXISTS AS A CVAR AT ALL: M_NightSky's StarRotation parameter is documented as ")
		TEXT("carrying local sidereal time PLUS whatever constant offset the star map's right-ascension ")
		TEXT("origin needs (Tools/create_sky_material.py, 'EQUIRECT UV'). The two are folded into one scalar, ")
		TEXT("so the moment C++ drives it every frame the MPC's own default is overwritten and there is no ")
		TEXT("asset-side place left to put the offset -- it has to live here or in a rebuild. Whether the ")
		TEXT("offset is needed cannot be known without an editor: if U=0 of T_SkyStarmap is not RA=0, the sky ")
		TEXT("comes out correctly oriented, correctly rotating, and turned by a constant angle about the ")
		TEXT("celestial pole, which no screenshot review would catch. This is the knob that fixes that in one ")
		TEXT("console line instead of a recompile, exactly as StarUDirection is the one that fixes handedness ")
		TEXT("in one asset edit. 0.25 = a quarter turn; the sign follows the resolved StarUDirection, so this ")
		TEXT("is applied in the same sense the sidereal drive is."),
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

	bool AreMoonShadowsEnabled() { return CVarSkyMoonShadows.GetValueOnAnyThread() != 0; }

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

	// Clamped to a band whose ENDS are both failure modes rather than tastes. Below
	// 0.05 deg the disc is under two pixels at 2200x1300 and the smoothstep limb
	// (MoonEdgeSoftness, 12% of the radius) has no pixels left to feather across,
	// so it aliases into a flickering dot; above 5 deg the flat orthographic
	// projection build_moon uses -- which treats the disc as a plane, exact only
	// for small angles -- starts visibly distorting the texture toward the limb.
	// Neither end is a hard error, which is why they are clamps and not raises.
	float GetMoonAngularRadiusDeg() { return FMath::Clamp(CVarSkyMoonAngularRadiusDeg.GetValueOnAnyThread(), 0.05f, 5.f); }

	// Floored at 0 and not clamped above: 0 is a legitimate A/B arm (the disc off
	// while the moonlight stays on, which is how you tell how much of a night frame
	// is the disc), and there is no upper value that is WRONG rather than merely
	// blown out -- the tonemapper is what bounds it, so a ceiling here would be
	// this file inventing a limit the renderer does not have.
	float GetMoonDiscBrightness() { return FMath::Max(0.f, CVarSkyMoonDiscBrightness.GetValueOnAnyThread()); }

	// Clamped to the SAME 1000..15000 window UE clamps to inside
	// FColorSpace::MakeFromColorTemperature (ColorSpace.cpp:271-273). Restated
	// here rather than left to the engine so that the read-back log can report the
	// ceiling being hit; a knob that silently stops moving at 15000 while the log
	// echoes 20000 is exactly the failure the read-back rule exists to prevent.
	float GetMoonTemperatureK() { return FMath::Clamp(CVarSkyMoonTemperatureK.GetValueOnAnyThread(), 1000.f, 15000.f); }

	float GetMoonTintStrength() { return FMath::Clamp(CVarSkyMoonTintStrength.GetValueOnAnyThread(), 0.f, 1.f); }

	// Floored at 0: this term may only ever DARKEN. A negative value would lift
	// deep night ABOVE the +15.6 twilight cap, which is the one thing the cap was
	// chosen to prevent (a full moon rendering brighter than noon).
	float GetDeepNightDropEV() { return FMath::Max(0.f, CVarSkyDeepNightDropEV.GetValueOnAnyThread()); }

	float GetShadowUpdateHz() { return FMath::Max(0.f, CVarSkyShadowUpdateHz.GetValueOnAnyThread()); }

	int32 GetExposureMode() { return FMath::Clamp(CVarSkyExposureMode.GetValueOnAnyThread(), 0, 2); }

	float GetExposureBias() { return CVarSkyExposureBias.GetValueOnAnyThread(); }

	bool IsDomeEnabled() { return CVarSkyDomeEnabled.GetValueOnAnyThread() != 0; }

	// Floored at 1e6 UU (10 km) purely so that a fat-fingered 0 or a negative
	// value cannot collapse the dome to a point at the camera, which renders as a
	// full-screen wash of whatever texel the star map has at the zenith and looks
	// nothing like "the dome is too small". The floor is NOT the correctness
	// bound -- 10 km is well inside the clipmap and would still hide the stars;
	// AVoxelSkyDomeActor::BeginPlay is what measures and reports that.
	double GetDomeRadiusUU()
	{
		return FMath::Max(1.0e6, (double)CVarSkyDomeRadiusUU.GetValueOnAnyThread());
	}

	bool IsAtmosphereDomeEnabled() { return CVarSkyAtmosphereDome.GetValueOnAnyThread() != 0; }

	// The ONE place voxel.Sky.StarAmbientGain turns into a number the material sees.
	// Both public accessors go through it so "what gain am I at" and "was that
	// derived" can never answer from two different reads of the cvar.
	//
	// There is deliberately no ceiling on an override: the honest upper bound is
	// whatever a capture measures, and a clamp invented here would silently cap the
	// very sweep the S2 gate exists to run -- the failure the read-back rule in this
	// file exists to prevent (a knob that stops moving while the log echoes what was
	// typed). No floor is needed either, because the sentinel test below already
	// makes every negative value unreachable by the material.
	float ResolveStarAmbientGain(bool& bOutDerived)
	{
		const float Requested = CVarSkyStarAmbientGain.GetValueOnAnyThread();

		// SPELT AS !(x >= 0) RATHER THAN (x < 0) SO NaN DERIVES. A NaN gain would
		// otherwise pass a `< 0` test as false, reach the MPC, and NaN-poison the
		// SkyLight's irradiance SH -- which does not look like a bad input, it looks
		// like the world's ambient term went black or white at random.
		bOutDerived = !(Requested >= 0.f);
		return bOutDerived ? kStarAmbientCalibration : Requested;
	}

	float GetStarAmbientGain()
	{
		bool bDerived = false;
		return ResolveStarAmbientGain(bDerived);
	}

	bool IsStarAmbientGainDerived()
	{
		bool bDerived = false;
		ResolveStarAmbientGain(bDerived);
		return bDerived;
	}

	// Unclamped and unwrapped on purpose: it is a rotation in turns, so every
	// real value is meaningful and 1.25 means the same thing as 0.25. Wrapping it
	// into [0,1) here would only make the read-back log disagree with what was
	// typed, which is the one thing the read-back rule forbids.
	double GetStarRotationOffsetTurns()
	{
		return (double)CVarSkyStarRotationOffsetTurns.GetValueOnAnyThread();
	}

	// --- the calendar ---------------------------------------------------------
	//
	// Days elapsed before the first of each month, and days in each month, in the
	// ephemeris's REFERENCE YEAR 2000. LEAP YEAR -- February has 29 days -- which
	// is the whole reason these are written out rather than computed from a
	// 365-day constant, and the reason day-of-year 79 is 20 March and not 21
	// (VoxelEphemeris.h:150-153).
	//
	// MOVED OUT OF THIS FILE'S ANONYMOUS NAMESPACE (where they used to live beside
	// the tuning constants) so that MonthDayFromDayOfYear below can be exported.
	// It had already been copied once -- kPerfDaysBeforeMonth/kPerfDaysInMonth in
	// VoxelPerfRunSubsystem.cpp, whose comment recorded the copy as debt and named
	// this exact accessor as the fix -- and the F1 overlay's calendar readout was
	// about to be the third copy. See VoxelSkySubsystem.h's declaration for why a
	// third copy is the specific failure VoxelClimateProbe.h documents.
	constexpr int32 kDaysBeforeMonth[12] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};
	constexpr int32 kDaysInMonth[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

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

	// THE MOON IS COLOURED BY CONVENTION, NOT BY SPECTROSCOPY, AND THE PHYSICS IS
	// WRITTEN DOWN HERE SO THAT NOBODY "CORRECTS" IT BACK ON PHYSICAL GROUNDS.
	// (kMoonTemperatureK, kMoonTintStrength: top of this file, beside the other
	// cvar defaults. Runtime knobs voxel.Sky.MoonTemperatureK and
	// voxel.Sky.MoonTintStrength.)
	//
	// WHAT THE PHYSICS ACTUALLY SAYS. Moonlight is sunlight reflected off lunar
	// regolith -- basalt and anorthosite, geometric albedo ~0.12. That surface is
	// spectrally near-neutral with a slight RED slope, so the moon's light is if
	// anything marginally WARMER than the 5778 K sun it is reflecting, around
	// 4100-4500 K by the usual correlated-colour-temperature fit. There is no
	// physical sense in which moonlight is blue.
	//
	// WHY WE RENDER IT BLUE ANYWAY. Two reasons, both about the viewer rather than
	// the light. (1) Human scotopic vision really does shift blue: at moonlight
	// levels the rods take over and their peak sensitivity moves from 555 nm to
	// 507 nm (the Purkinje effect), so a real moonlit scene IS perceived cooler
	// than its spectrum. (2) Every photograph and every film of night is graded
	// cool, and that convention is now what "night" looks like. We render the
	// perception, not the spectrum. A warm moon does not read as an unusual
	// artistic choice; it reads as a bug.
	//
	// THE TRAP THAT PUT THE 4100 HERE, NAMED SO IT CANNOT REPEAT. This constant
	// shipped at 4100 K with a comment correctly arguing for the Purkinje shift
	// and then writing down a number that produces its exact opposite. COLOUR
	// TEMPERATURE RUNS BACKWARDS FROM THE WORD "COOL": lower Kelvin is redder.
	// FColorSpace::MakeFromColorTemperature (ColorSpace.cpp:271) at 4100 K returns
	// linear RGB (1.389, 0.929, 0.559) -- an R:B ratio of 2.48, TWICE as red-biased
	// as the sun's own (1.113, 0.975, 0.915) at R:B 1.22. The moon was lighting the
	// terrain more orange than the sun does. That is the "night reads as warm dusk"
	// defect, whole. At 12000 K the same function returns (0.825, 0.995, 1.565),
	// R:B 0.53, and the frame reads as night.
	//
	// AND IT COSTS NOTHING IN BRIGHTNESS, which is why this is a colour fix and not
	// a lighting change. MakeFromColorTemperature normalises to Y = 1 in XYZ at
	// every temperature, so the luminance the moon delivers is identical at 4100 K
	// and at 12000 K to within the sRGB-encoding of the hue (<0.5% on a mean-luma
	// metric). The one place it is NOT neutral is the SKY: the moon is atmosphere
	// sun light 1, and UE's default Rayleigh coefficients scatter blue ~5.7x more
	// efficiently than red, so a blue moon produces a moon-scattered night sky
	// roughly 0.25 stops brighter as well as far more saturated. Expect the night
	// rungs of a ladder to move up by a luma point or two and NOT more; if they
	// move by ten, something other than this constant changed.
	//
	// Applied through ApplyLightsFromState, not through SpawnRig -- see there for
	// why a colour set once at spawn is a colour no cvar can reach.

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

	// ======================================================================
	// WHICH BODY OWNS THE FORWARD / TRANSLUCENT SINGLE-DIRECTIONAL-LIGHT SLOT
	//
	// THE WARNING THE OWNER SEES IN EDITOR -- "Multiple directional lights are
	// competing to be the single one used for forward shading, translucent, water
	// or volumetric fog. Please adjust their ForwardShadingPriority." -- is emitted
	// verbatim from LightGridInjection.cpp:1904, and it is not cosmetic. It is the
	// renderer reporting that it had to guess, and the guess it makes is wrong here
	// for the whole of every night.
	//
	// THE MECHANISM, from the 5.8 source rather than from inference:
	//
	//   1. FSceneRenderer::ComputeLightGrid walks the sorted lights and, for each
	//      DIRECTIONAL one, keeps a single winner in
	//      View.ForwardLightingResources.SelectedForwardDirectionalLightProxy
	//      (LightGridInjection.cpp:1121, 1513-1520). There is exactly one slot.
	//   2. The comparison is priority FIRST, brightness as the tiebreak
	//      (LightGridInjection.cpp:1500-1514):
	//          LightIntensitySq = |LightParameters.Color|^2
	//          win if  Priority > best  ||  (Priority == best && IntensitySq > best)
	//      UDirectionalLightComponent::ForwardShadingPriority defaults to 0
	//      (DirectionalLightComponent.cpp:1040) and NOTHING in this file has ever
	//      set it, so BOTH of our lights sit at 0 and the tiebreak decides.
	//   3. The tiebreak is on the light's OWN colour * brightness, taken BEFORE any
	//      atmosphere transmittance is applied -- transmittance is multiplied in at
	//      LightGridInjection.cpp:1524-1530, four lines AFTER the winner has been
	//      chosen, and only when the light opts into per-pixel transmittance, which
	//      defaults to 0 anyway (DirectionalLightComponent.cpp:1082). So the sun
	//      competes at its full 15.0 at MIDNIGHT. It outweighs the moon 94:1 and
	//      wins every frame of every night.
	//   4. The warning itself: ConflictingLightCountForForwardShading counts how
	//      many lights share the top priority (1502-1511) and >= 2 raises the flag
	//      (1662). Two lights, both at priority 0, both registered -> the count is
	//      permanently 2 and the message is permanently on screen.
	//
	// WHY THIS IS THE LAKE BUG AND NOT JUST A NAG. M_WaterVoxel is
	// BLEND_TRANSLUCENT with TLM_SURFACE_PER_PIXEL_LIGHTING
	// (create_water_voxel_material.py:114, 140), so its direct lighting comes from
	// the forward path -- from that one selected proxy and nothing else. At night
	// the selected proxy is a sun 37 degrees BELOW the horizon, so N.L is negative
	// for every up-facing water surface and clamps to zero: the lake receives no
	// direct light at all, from either body, all night. The moon is not dim on the
	// water; it is not addressed to the water. That is the owner's "the moon does
	// not light up or reflect against the lake".
	//
	// PRIORITY, NOT VISIBILITY, IS THE FIX. Hiding the below-horizon light would
	// also silence the warning -- and it is the trap this whole file is built
	// around. ApplyLightsFromState's opening comment documents at length that a
	// hidden or zero-intensity directional light is REMOVED FROM FScene, which
	// clears AtmosphereLights[] and makes USkyAtmosphere integrate zero radiance;
	// that is exactly how twilight rendered black once already. The SkyAtmosphere
	// needs BOTH lights present at ALL hours -- the sun for twilight scattering
	// from below the horizon, the moon for the night sky's own tint -- so the two
	// requirements are in direct conflict and only ForwardShadingPriority satisfies
	// both. It changes which light the forward path addresses and touches nothing
	// the atmosphere reads.
	//
	// WHERE THE HANDOVER SITS, and why it is not the horizon. The obvious rule is
	// "whichever body is above the horizon", i.e. hand over at sun altitude 0. That
	// is right for WATER (a sun at -0.5 deg already delivers nothing to an up-facing
	// surface) and wrong for the other consumer of this same slot: VOLUMETRIC FOG,
	// which is lit by the selected proxy too, and whose whole reason for existing at
	// dusk is the sun scattering through it from below the horizon. Handing the slot
	// to the moon the instant the sun clips the horizon would drain the colour out
	// of every twilight.
	//
	// So the handover is kMoonSunSuppressStartDeg -- REUSED, not a new number, and
	// that is the point. That constant already means "the sun is far enough down
	// that the moon is at full strength": above it the moon's own SunSuppress term
	// is scaling the moonlight toward zero, so a moon holding the forward slot
	// during civil twilight would be holding it while contributing almost nothing.
	// Deriving the handover from the same constant makes the two rules incapable of
	// disagreeing. Below -6 deg the sun's transmittance is long since ray-marching
	// through the planet body (see kSunShadowOffBelowDeg) and there is no twilight
	// left to drain.
	//
	// 1 degree of hysteresis, for the reason kSunShadowOffBelowDeg gives:
	// SetForwardShadingPriority MarkRenderStateDirty's
	// (DirectionalLightComponent.cpp:1448-1455), which rebuilds the light's scene
	// proxy. Twice a day is free; twice a second on a sun parked at -6.0 is not.
	constexpr double kForwardMoonPrimarySunBelowDeg = kMoonSunSuppressStartDeg;       // -6: moon takes the slot
	constexpr double kForwardSunPrimarySunAboveDeg = kMoonSunSuppressStartDeg + 1.0;  // -5: sun takes it back

	// The two priority values. Any pair of DIFFERENT non-negative ints silences the
	// warning (the conflict count only rises when the TOP priority is shared), and
	// SetForwardShadingPriority clamps negatives to 0 anyway
	// (DirectionalLightComponent.cpp:1453), so 1/0 is the smallest pair that cannot
	// be defeated by that clamp. They are named rather than literal because the
	// invariant that matters is "these are never equal", and a reader has to be able
	// to see that at the point of use.
	constexpr int32 kForwardPriorityPrimary = 1;
	constexpr int32 kForwardPrioritySecondary = 0;

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

	// (The month tables that used to live here, and MonthDayFromDayOfYear with
	// them, moved UP into namespace VoxelSky so they could be exported -- see the
	// comment above VoxelSky::kDaysBeforeMonth. DayOfYearFromMonthDay below is the
	// only consumer left in this file that is not exported, so it qualifies.)

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
		const int32 D = FMath::Clamp(Day, 1, VoxelSky::kDaysInMonth[M - 1]);
		return VoxelSky::kDaysBeforeMonth[M - 1] + (D - 1);
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

	// --- the night sky's material (M_NightSky + MPC_VoxelSky) ----------------

	// The collection M_NightSky binds every one of its parameters to. Named once,
	// here, because the string appears in a LoadObject and in three separate
	// diagnostics and a typo in any of them is a night sky that renders the
	// asset's DEFAULTS -- a plausible-looking star field frozen at the wrong
	// sidereal time with a moon due east at 45 degrees -- rather than an error.
	const TCHAR* kSkyCollectionPath = TEXT("/Game/Voxel/MPC_VoxelSky.MPC_VoxelSky");

	// THE STAR FIELD'S SUNRISE FADE, and it is NOT optional decoration.
	//
	// M_NightSky is BLEND_Additive and is composited AFTER the SkyAtmosphere
	// (Tools/create_sky_material.py, "WHY ADDITIVE TRANSLUCENT INSTEAD"), which
	// means nothing in the renderer extinguishes the stars as the sky brightens
	// -- physically the day sky drowns them, and here it simply does not. The
	// material's author states the consequence plainly: an always-on star field
	// at noon is the expected failure mode if this hook is never written. This is
	// that hook, and it is the only thing standing between the shipped default
	// (StarBrightness 1.0) and stars over a blue afternoon.
	//
	// THE BAND IS ASTRONOMICAL TWILIGHT TO THE HORIZON, which is the honest
	// answer rather than a chosen one: -18 deg is where sunlight stops reaching
	// any part of the sky above the observer, and 0 is where the sun's disc is on
	// the horizon. -12 rather than -18 for the full-brightness end because real
	// naked-eye stars are already out through most of nautical twilight, and
	// because the last six degrees would otherwise spend the fade in a band where
	// the atmosphere renders essentially nothing anyway. Smoothstepped for the
	// same reason every other gate in this file is: a linear ramp's two corners
	// are visible as a kink at exactly the moment anyone is watching the sky.
	//
	// NOTE WHAT THIS IS NOT COUPLED TO. It is a function of sun altitude ALONE,
	// like the exposure curve above and for the same reason -- the same sky must
	// not render differently in two places. In particular it does NOT know about
	// the exposure curve, which is lifting the frame by up to 15.6 stops at the
	// same time; the two multiply on screen, and if the stars come out too bright
	// or too dim at twilight the knob for that is voxel.Sky.StarGain below.
	//
	// CORRECTION to the line above, which said the knob was "the MPC's own
	// StarBrightness default (an asset edit, no regeneration)". It is not. This
	// subsystem WRITES StarBrightness every frame, so the asset default is
	// overwritten before anything renders and editing it does nothing. Same trap
	// the star-map handedness note hit for StarRotation: once C++ drives a
	// collection scalar, that scalar's asset default is dead as a tuning surface
	// and the knob has to live here.
	constexpr double kStarFadeFullBelowDeg = -12.0;
	constexpr double kStarFadeZeroAboveDeg = 0.0;

	// Master star gain. create_sky_material.py ships StarBrightness/MoonBrightness
	// defaults deliberately high (1.0 / 20.0) so a first capture proves the graph
	// compiled rather than being invisible -- an over-bright sky is diagnosable, an
	// empty one is indistinguishable from a dozen other failures. Measured on the
	// first live capture (winter 21h00, sun -44.7 deg, moon below the horizon):
	// gain 1.0 gave mean luma 30.77 against 0.02 with no dome at all, i.e. the
	// star field alone was outshining a moonlit night. This scales it back to
	// something a night sky reads as.
	//
	// Artistic, not photometric, and deliberately a cvar rather than a constant:
	// it multiplies against the exposure curve's lift of up to 15.6 stops, so the
	// two have to be tuned against each other on real captures.
	TAutoConsoleVariable<float> CVarSkyStarGain(
		TEXT("voxel.Sky.StarGain"), 0.15f,
		TEXT("Master multiplier on the star field's brightness (default 0.15). ")
		TEXT("The MPC's own StarBrightness default cannot serve this purpose: the ")
		TEXT("sky subsystem overwrites that scalar every frame. 1.0 is the raw ")
		TEXT("asset default, which measured mean luma 30.77 on a moonless winter ")
		TEXT("night against 0.02 with no star dome -- far too bright. Artistic, ")
		TEXT("not photometric; it multiplies against the exposure curve."),
		ECVF_Default);

	double StarBrightnessForSunAltitude(double AltitudeDeg)
	{
		const double T = FMath::Clamp(
			(kStarFadeZeroAboveDeg - AltitudeDeg) /
				(kStarFadeZeroAboveDeg - kStarFadeFullBelowDeg), 0.0, 1.0);
		const double Fade = T * T * (3.0 - 2.0 * T);
		return Fade * FMath::Max(0.f, CVarSkyStarGain.GetValueOnAnyThread());
	}

	// The moon's colour temperature the frame will ACTUALLY be given, after
	// voxel.Sky.MoonTintStrength has decided how far from the physically honest
	// answer to travel. Strength 0 hands the moon the sun's own temperature, which
	// is what the spectrum says (reflected sunlight, see kMoonTemperatureK's
	// comment); strength 1 hands it voxel.Sky.MoonTemperatureK in full.
	//
	// LERPED IN MIRED (1e6/K), NOT IN KELVIN. Reciprocal colour temperature is the
	// axis on which equal steps are roughly equal perceived colour steps; Kelvin is
	// not. 5778 K -> 12000 K is 173 -> 83 mired, and a Kelvin lerp at strength 0.5
	// lands on 8889 K (112 mired) where the perceptual midpoint is 128 mired
	// (7800 K). A Kelvin slider would therefore look like it does almost nothing
	// for its first half and everything in its second, which is how a knob gets a
	// reputation for being broken.
	float ResolveMoonTemperatureK()
	{
		const float TargetK = VoxelSky::GetMoonTemperatureK();
		const float Strength = VoxelSky::GetMoonTintStrength();
		const float SunMired = 1.0e6f / kSunTemperatureK;
		const float MoonMired = 1.0e6f / FMath::Max(1.f, TargetK);
		const float Mired = FMath::Lerp(SunMired, MoonMired, Strength);
		// Re-clamped to UE's own window rather than trusted to stay inside it: the
		// lerp is between two already-clamped endpoints so it cannot escape today,
		// but a future third term here could, and MakeFromColorTemperature would
		// swallow it silently.
		return FMath::Clamp(1.0e6f / FMath::Max(1.f, Mired), 1000.f, 15000.f);
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
	// nine stops from noon to a moonlit midnight and this curve lifts by 6.8
	// (+8.8 to +15.6). The stops that do not get lifted are what the player sees
	// as darkness. A curve that compensated fully would be auto-exposure with
	// extra steps -- and "fully" is exactly what the pre-retune table did above
	// the horizon, which is the defect fixed below.
	//
	// THE ANCHORS BELOW ARE MEASURED, FROM TWO FULL LADDERS. This paragraph and
	// the four that follow are the derivation; nothing in the table is a taste
	// call except the target luminances, which are named.
	//
	// The evidence is 16 (sun altitude -> EV bias -> mean luminance) triples: the
	// 8-rung 06-21 ladder and the 8-rung 12-21 ladder, same observer (52.48 N),
	// same camera, captured against the anchor set this one replaces. Between
	// them they span -60.8 deg to +60.9 deg, and 8 of the 16 are above the
	// horizon, which is enough to fit the transfer function properly.
	//
	//   FIRST TRAP, AND IT IS WORTH A STOP: "mean luma" is the mean of sRGB CODE
	//   VALUES out of 255, not of light. Every stop arithmetic below decodes it
	//   (v/255 -> linear) first. Skip that and the measured falloff comes out
	//   about 0.7x of the true one and the fitted curve is a stop wrong at the
	//   low end -- in the direction that makes low sun too bright, which is the
	//   defect this retune exists to remove.
	//
	// THE MODEL. log2(Llin) = c + gamma * ( p * log2 sin(alt) + bias ), i.e. the
	// scene's ground illuminance is a power of sin(alt), the bias adds stops on
	// top, and the tonemapper turns the sum into display-linear luminance with a
	// local slope gamma. Fitted by least squares on the DAY rungs of each ladder
	// independently (three free parameters, and bias is not quite a linear
	// function of log2 sin because the table it came from is piecewise linear in
	// DEGREES, which is the only reason the two regressors separate at all):
	//
	//   06-21 day rungs (5)   gamma 1.09   p 0.775
	//   12-21 day rungs (3)   gamma 1.01   p 0.816
	//
	// Adopted: gamma 1.05, p 0.80. Residuals at those values are 0.09 stops
	// across the five summer rungs and 0.03 across the three winter ones, so the
	// model is not being generous to itself. gamma 1.05 also agrees with the two
	// independent readings already on record -- the W6 two-point fit's 1.13, and
	// ~1.17 from comparing the moonlit midsummer midnight against the midwinter
	// deep-night rungs at their known moon intensities.
	//
	// AND THAT p IS THE WHOLE DEFECT. The scene falls as sin(alt)^0.80, i.e. 0.80
	// stops per octave of sin. The old table lifted 0.763 EV per octave of sin.
	// It was compensating 95% of the scene's own altitude falloff, so display
	// luminance came out FLAT in altitude -- 123 to 129 across +17.5 to +60.9 --
	// and any measurement noise on top of flat reads as an inversion. The old
	// anchors were built on the standard clear-sky illuminance table, which is
	// about sin^1.15; USkyAtmosphere plus this project's ambient floor is much
	// shallower than that, and one wrong exponent is all it took.
	//
	// THE TWO LADDERS ARE NOT LIT BY THE SAME SCENE, AND THE BUDGET HAS TO SAY SO.
	// At equal (altitude, bias) the 12-21 ladder is +0.52 DISPLAY STOPS brighter
	// than the 06-21 one. Its three day rungs agree on that offset to 0.03 stops,
	// so it is not noise -- it is scene content (seasonal ground albedo, and a
	// due-south winter noon against a fixed camera). This curve is a function of
	// sun altitude ALONE and must stay one, or the same sky renders differently
	// in two places; it therefore cannot cancel the offset. But it has to be
	// BUDGETED FOR, because "a low winter noon must read dimmer than a high
	// summer noon" is a CROSS-LADDER comparison, and half a stop of the answer is
	// already spent before the curve opens its mouth. That is why the day slope
	// below is as shallow as it is.
	//
	// THE NEW DAY LAW: bias = 8.79 - 0.19 * log2 sin(alt), for alt >= +2 deg.
	// 0.19 against the scene's 0.80 is 24% compensation instead of 95%, which
	// leaves 0.64 display stops per octave of sin visible on screen. The chosen
	// luminances that falls out to, on the 06-21 scene: 129 at +61, 122 at +45,
	// 98 at +20, 80 at +10, 64 at +5, 47 at +2. Full daylight sits 105-130 and a
	// low sun reads visibly low, which is the entire requirement.
	//
	//   NOTE WHAT DID NOT MOVE. The law returns 8.829 at +60.9 deg -- the same
	//   number the old table returned there, to three decimals. High sun was
	//   never the problem and the measurement says the old value was right; the
	//   whole retune is below +45 deg. Anyone bisecting a "the day got darker"
	//   report should start at +10 to +20, not at noon.
	//
	// WHAT THE FLAT CAP COULD NOT DO, AND WHY THAT IS NOT THE BUG IT LOOKS LIKE.
	//
	// The cap below was originally flat all the way down, so the curve returned
	// +15.6 for a -14 deg midsummer midnight and +15.6 for a -61 deg midwinter one.
	// That reads like a defect -- one number for two completely different nights --
	// but an exposure BIAS does not need to distinguish them. It is a lift, and a
	// constant lift passes whatever difference the scene has straight through to
	// the screen 1:1. A curve that kept RISING below -2 deg would be the actual
	// mistake: more lift where the scene is darker is compression, and it would
	// erase the seasonal difference rather than preserve it. Anyone arriving here
	// intending to "extend the curve downward" should be clear which direction they
	// mean, because up is wrong.
	//
	// THE REAL PROBLEM IS THAT THE SCENE HAS NO DIFFERENCE TO PASS THROUGH, and the
	// 06-21 ladder log says so. Take the three below-horizon rungs:
	//
	//   sun -3.8 deg, no moon                 luma 83.47
	//   sun -5.1 deg, moon 100% lit at +7.9   luma 55.17
	//   sun -14.1 deg, moon 99% lit at +18.1  luma 38.01
	//
	// The first two bracket the twilight falloff: 1.3 degrees of sun altitude costs
	// 0.6 stops of displayed luminance even WITH a moon added, i.e. the sky is
	// falling by roughly 2.5x per degree through civil twilight. Extrapolate that
	// nine degrees further to -14.1 and the sky's contribution is down by ~3800x --
	// three orders of magnitude below the moon term at that rung. So the 38.01 at
	// midsummer midnight is NOT twilight. It is a nearly full moon 18 degrees up,
	// and essentially nothing else.
	//
	// That is a statement about USkyAtmosphere, not about this curve: it models
	// scattered SUNLIGHT and has no airglow, no starlight and no zodiacal term, so
	// its sky is equally dead at -14 deg and at -61 deg. There is no midsummer
	// skyglow for a curve to preserve, because the renderer never produced any.
	//
	// AND THE CONSEQUENCE WAS BACKWARDS. With the sky dead at both, a flat cap made
	// every deep night render as whatever the moon happened to be doing -- and the
	// moon is HIGHER in winter, because a full moon rides opposite the sun. At 52 N
	// a full winter moon reaches ~60 deg where the midsummer one measured above sat
	// at 18 deg, which is 1.7 stops MORE ground illuminance. Under a flat cap the
	// darkest night of the year rendered as the brightest frame of any night. That
	// is the defect, and it is a policy defect: sun altitude below -18 deg carries
	// no information about how bright the sky is, so if deep night is to read as
	// deep night, this curve is the only place that can say so.
	//
	// HENCE DeepNightDropForSunAltitude, SUBTRACTED FROM THE TABLE BELOW. It only
	// ever darkens, so it cannot violate the constraint the +15.6 cap was set to
	// satisfy (below). voxel.Sky.DeepNightDropEV 0 restores the old flat cap
	// exactly, which is the control arm.
	//
	// WHY THE CAP IS REACHED AT -6 DEG AND NOT AT -12. Below the horizon the scene
	// falls off a cliff -- the two measured summer twilight rungs put it near a
	// stop PER DEGREE -- and a curve that kept tracking it would need +20 EV by
	// -6 deg. It cannot, because the MOON is also on down there, and a full moon
	// under a +20 EV exposure renders brighter than noon. So the cap is set where
	// a full moon reads as a full moon (voxel.Sky.MoonIntensity and this cap move
	// the same pixel; they were solved together, and 15.6 / 0.04 is that
	// solution). The price is paid by MOONLESS civil twilight, which lands dimmer
	// than the moonlit kind. That is the honest trade for a curve driven by sun
	// altitude alone, and it is the owner's stated preference: a new moon should
	// be genuinely dark.
	//
	// THE CAP'S ONSET MOVED FROM -2 DEG TO -6 DEG, AND THAT IS THIS RETUNE'S ONLY
	// BELOW-HORIZON CHANGE. The cap's VALUE is untouched, the deep-night ramp is
	// untouched, and everything at or below -6 keeps the exact bias it had.
	//
	// WHY IT HAD TO MOVE: THE SUNSET FLARE NOBODY HAD SAMPLED. Neither ladder has
	// a rung between +4.6 deg and -3.8 deg -- an 8.4-degree hole containing the
	// entire sunrise. Interpolate the scene across it from the two rungs that DO
	// bracket it and the old table's +14.1 at 0 deg and +15.6 at -2 deg put the
	// BRIGHTEST frame of the whole day at about -1 deg: ~140 luma against noon's
	// 129. The sun was below the horizon and the frame was brighter than noon.
	// Sixteen measured rungs and not one of them could see it, which is the thing
	// to remember about this hole -- it is still there, and it is still the least
	// certain part of this curve.
	//
	// WHY -6 IS THE PRINCIPLED PLACE AND -2 WAS NOT. The 15.6 number was solved
	// against the MOON, not against the sky. Above -6 deg there is still real
	// scattered sunlight for the curve to track, and handing that band a
	// moon-sized lift is what produced the flare. Below -6 there is not: civil
	// twilight ends there by definition, the frame is moon-lit from that point
	// down, and a moon-solved cap is exactly the right policy for a moon-lit
	// frame. So the table now descends 15.60 -> 9.71 across -6 .. +2 instead of
	// jumping 15.6 -> 12.5 across -2 .. +2.
	//
	// WHAT THAT COSTS, STATED SO THE RE-RUN CAN BE CHECKED AGAINST IT: exactly two
	// rungs in the whole corpus move below the horizon, both midsummer civil
	// twilight. -3.8 deg goes 15.60 -> 13.59 (luma 83.47 -> ~39) and -5.1 deg goes
	// 15.60 -> 14.76 (55.17 -> ~40). Midsummer midnight at -14.1 deg keeps 15.60
	// and therefore keeps its measured 38.01, and every midwinter night rung
	// (-17.9 through -60.8) keeps 13.60 and its measured 5.85 / 9.03 / 7.25 /
	// 0.02. The seasonal night relationship the deep-night drop was built to
	// produce -- midwinter 5.9-9.0 against midsummer 38.0 -- is preserved
	// numerically, not approximately.
	//
	// STILL EXTRAPOLATION, and this is the one to check first if the re-run
	// disagrees: the scene between +4.6 deg and -3.8 deg, per the hole above.
	// The anchors at 0 and -2 assume the scene's fall accelerates smoothly across
	// the horizon from the measured rung above to the measured rung below. If
	// that assumption is wrong the two anchors will be wrong together and in one
	// direction, which voxel.Sky.ExposureBias shifts in one move without touching
	// this table. A ladder with rungs at +2, 0 and -2 would close it for good.

	// THE DEEP-NIGHT DROP. Stops given back once the sun is below astronomical
	// twilight, subtracted from the table below. Zero everywhere above the ramp.
	//
	// THE BAND IS -15 TO -18 DEG AND BOTH ENDS ARE DERIVED, NOT CHOSEN.
	//
	// -15 deg is the floor midsummer never reaches. At solar midnight the sun's
	// altitude is exactly (latitude + declination - 90); at the shipped
	// voxel.Sky.OriginLatitudeDeg 52.0 and the solstice declination +23.44 that is
	// -14.56 deg, and the 06-21 ladder measured -14.135 at the observer's actual
	// 52.48 N. Starting the ramp at -15 therefore leaves EVERY midsummer frame at
	// this latitude on the flat cap, untouched, which is the owner's requirement
	// that a midsummer midnight stay legitimately bright-ish.
	//
	//   THE TRAP THAT COMES WITH THAT: it is a latitude threshold, not a universal
	//   one. Midsummer midnight clears -15 deg only for latitudes above
	//   90 - 23.44 - 15 = 51.56 N. Move voxel.Sky.OriginLatitudeDeg south of that
	//   and midsummer nights start dimming, gently at first. That is not a bug to
	//   fix here -- this curve is a function of sun altitude ALONE and must stay
	//   one, or the same sky renders differently in two places -- but it is the
	//   thing to check first if a southern world's summer nights look wrong.
	//
	// -18 deg is where astronomical twilight ends BY DEFINITION: no sunlight
	// reaches any part of the sky above the observer. There is nothing left for the
	// curve to track down there, so it is free to express policy instead, and the
	// policy is the owner's: night is genuinely dark, and the darkness comes from
	// dimming lights and an exposure stop, never from crushing albedo. Nothing in
	// this file touches voxel.GI.AmbientFloor or any other GI cvar and it must not.
	//
	// THE 3-DEGREE BAND IS NOT ABRUPT. At the default 2400 s day the sun sweeps
	// 3 degrees in ~20 seconds of wall clock, so this is a 2-stop fade over twenty
	// seconds -- 0.10 stops/s, still no faster than what ships at sunrise, where
	// the table below now moves 5.89 stops across the 8 degrees from -6 to +2
	// (~54 s, 0.11 stops/s). That comparison was rechecked when the cap's onset
	// moved from -2 to -6: the sunrise ramp got LONGER in degrees and gentler per
	// degree, so the claim survives with margin it did not have before.
	//
	// WHY 2.0 STOPS. It is the size that puts a deep-night frame back below the
	// midsummer-midnight frame it used to out-render. Worked from the one measured
	// moonlit rung (sun -14.1, moon 99% at +18.1, luma 38.01): the brightest night
	// this world can produce is a full moon at ~60 deg in midwinter, whose ground
	// illuminance is 1.7 stops above that rung's. Two stops of exposure lands it
	// just under, at ~34 luma predicted, which is still comfortably navigable and
	// still ~1.9 display stops below noon. The cap's original constraint -- a full
	// moon must not render brighter than noon -- was solved together with
	// voxel.Sky.MoonIntensity and is NOT reopened here: this term only subtracts,
	// so the margin it had can only grow.
	constexpr double kDeepNightRampStartDeg = -15.0; // flat cap at and above this
	constexpr double kDeepNightRampEndDeg = -18.0;   // full drop at and below this

	double DeepNightDropForSunAltitude(double AltitudeDeg)
	{
		const double T = FMath::Clamp(
			(kDeepNightRampStartDeg - AltitudeDeg) /
				(kDeepNightRampStartDeg - kDeepNightRampEndDeg), 0.0, 1.0);
		const double S = T * T * (3.0 - 2.0 * T); // same smoothstep the light gates use
		return S * (double)VoxelSky::GetDeepNightDropEV();
	}

	double ExposureBiasForSunAltitude(double AltitudeDeg)
	{
		struct FAnchor { double AltDeg; double BiasEV; };
		// Altitudes ascending. Below the first and above the last the TABLE is
		// flat; the deep-night drop subtracted at the end is what carries the
		// curve below -15. The three night anchors are all equal on purpose -- the
		// cap is reached at -6 deg and the table holds it from there down because
		// the moon, not the sun, is what the frame is lit by (above).
		//
		// The first anchor moved from -18 to -15 when the drop was added: -15 is
		// where the ramp starts, so a table entry below it would be describing an
		// altitude the drop already owns, and the two would have to be kept
		// consistent by hand forever.
		//
		// EVERY ENTRY FROM +2 UP IS THE FITTED LAW bias = 8.79 - 0.19*log2 sin(alt),
		// SAMPLED -- not hand-placed. The altitudes are chosen so that linear
		// interpolation IN DEGREES between them reproduces that law to within 0.03
		// EV everywhere (worst case is +3 deg, on the 2..5 span), which is under a
		// luma point and well under capture noise. Add altitudes if a span ever
		// needs to be finer; do NOT edit one entry by taste, because the entries
		// are not independent -- they are eight samples of one two-parameter line
		// and moving one alone puts a kink in the middle of the day.
		//
		// The four entries from 0 down to -6 are NOT that law: sin^0.80 diverges at
		// the horizon and the scene does not (the sky's diffuse term and the GI
		// ambient floor keep the ground lit through sunset). They interpolate the
		// scene between the lowest measured day rung (+4.6) and the lowest measured
		// moonless twilight rung (-3.8) and hand back a constant 90% of its fall,
		// which is what lands the cap exactly at -6. This is the extrapolated part
		// of the curve; see the hole named above.
		static const FAnchor Anchors[] = {
			{-15.0, 15.60}, // the deepest midsummer midnight at 52 N; below this the drop takes over
			{-12.0, 15.60}, // nautical twilight
			{ -6.0, 15.60}, // civil twilight ends; the cap, and the moon owns the frame below here
			{ -4.0, 13.74}, // interpolated scene; -3.8 was measured moonless at luma 83.47
			{ -2.0, 12.23},
			{  0.0, 10.89}, // sunrise/sunset
			{  2.0,  9.71}, // the fitted day law starts here and runs to the top of the table
			{  5.0,  9.46},
			{ 10.0,  9.27},
			{ 20.0,  9.08},
			{ 30.0,  8.98},
			{ 45.0,  8.89},
			{ 60.0,  8.83}, // the 12h00 rung of the 06-21 ladder; unchanged by this retune
			{ 90.0,  8.79}, // full day
		};
		constexpr int32 Count = UE_ARRAY_COUNT(Anchors);

		// The drop is applied to EVERY return path rather than only to the
		// below-table one, so that the two cannot disagree if the first anchor ever
		// moves. It is identically zero above kDeepNightRampStartDeg, so the cost
		// of that is one clamp on the day rungs.
		double Bias = Anchors[Count - 1].BiasEV;
		if (AltitudeDeg <= Anchors[0].AltDeg)
		{
			Bias = Anchors[0].BiasEV;
		}
		else
		{
			for (int32 I = 1; I < Count; ++I)
			{
				if (AltitudeDeg <= Anchors[I].AltDeg)
				{
					const double Span = Anchors[I].AltDeg - Anchors[I - 1].AltDeg;
					const double T = Span > 0.0 ? (AltitudeDeg - Anchors[I - 1].AltDeg) / Span : 0.0;
					Bias = FMath::Lerp(Anchors[I - 1].BiasEV, Anchors[I].BiasEV, T);
					break;
				}
			}
		}
		return Bias - DeepNightDropForSunAltitude(AltitudeDeg);
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

	// Latched state of the MOON's CastShadows flag, mirroring bSunShadowsOn.
	// Starts FALSE because SpawnRig explicitly calls SetCastShadows(false) on the
	// moon, so unlike the sun the latch and the component genuinely agree at spawn.
	bool bMoonShadowsOn = false;

	// Which body currently owns the forward/translucent single-directional-light
	// slot, latched so the hysteresis band has something to compare against and so
	// the handover is logged once per crossing rather than every update.
	//
	// -1 = nothing decided yet, which is DISTINCT from both states rather than
	// aliasing "sun". The first update therefore always applies a priority pair and
	// always logs it: a night capture has to be able to state from its own log which
	// light its water was addressed by, and "no line" must not be readable as "the
	// sun, probably".
	int32 AppliedForwardPrimaryIsMoon = -1;

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

	// --- the night sky's material parameters (ApplySkyMaterialParams) --------
	//
	// MPC_VoxelSky, resolved ONCE. LoadObject on the first frame that needs it
	// rather than in Initialize, because Initialize can run before the asset
	// registry is in a state where a /Game/ path resolves; and once rather than
	// per frame because a failed LoadObject is a synchronous package-open attempt
	// and retrying it sixty times a second would be a real cost for a case that is
	// never going to start working mid-run.
	//
	// TStrongObjectPtr, NOT a raw pointer. FVoxelSkyImpl is a plain struct (see
	// the header's PImpl note), so a UObject* here is invisible to the GC: the
	// world's own UMaterialParameterCollectionInstance does reference the
	// collection once a parameter has been written, but nothing guarantees that
	// for the window before the first write, and a dangling collection pointer
	// would be a crash rather than a blank sky.
	TStrongObjectPtr<UMaterialParameterCollection> SkyParams;
	bool bSkyParamsLoadAttempted = false;

	// The frame's body directions, TOWARD the body, exactly as the ephemeris
	// returned them and with no negation applied.
	//
	// CACHED HERE RATHER THAN IN FVoxelSkyState, which is where a reader would
	// look for them first. FVoxelSkyState is deliberately scalar-only and
	// deliberately free of every ephemeris type (see its header comment): it is a
	// read-only report for a HUD and a capture log, both of which want degrees.
	// ApplySkyMaterialParams wants the vectors, and this struct is in the .cpp
	// where an FVector costs nothing. Recomputing them from
	// SunAltitudeDeg/SunAzimuthDeg instead would mean a second copy of
	// DirectionFromAltAz living outside VoxelEphemeris.cpp, which is exactly the
	// duplication LocalSiderealTimeDeg was exported to avoid.
	FVector SunDirection = FVector::ZeroVector;
	FVector MoonDirection = FVector::ZeroVector;

	// StarUDirection is an ASSET-SIDE knob the C++ has to agree with -- see
	// ApplySkyMaterialParams for why StarRotation's sign depends on it. Latched so
	// the resolved pair is logged when it changes and not every frame.
	double AppliedStarUDirection = 0.0;

	// Last RESOLVED star ambient gain logged -- the number the material was handed,
	// never the number the cvar was asked for. Starts at a value the RESOLVED gain
	// cannot hold (resolution maps the whole negative half-line to the calibration
	// constant, so nothing negative ever comes out of it) so the FIRST frame always
	// logs the gain the run is actually at -- an S2 capture has to be able to state
	// its own gain from its own log, and "no line" would be indistinguishable from
	// "gain 0".
	float AppliedStarAmbientGain = -1.f;

	// Whether that gain was DERIVED or OVERRIDDEN, latched separately because the
	// two can change without the value changing: `voxel.Sky.StarAmbientGain 1.0`
	// resolves to the same 1.0 the default derives, and a run that had been pinned
	// to an explicit value would otherwise never say so. -1 = nothing logged yet,
	// which is distinct from both states rather than aliasing one of them.
	int32 AppliedStarAmbientGainDerived = -1;

	// Last drawn moon radius written into the MPC. Starts negative -- a value
	// GetMoonAngularRadiusDeg cannot return, since it clamps at 0.05 -- so the
	// FIRST frame always logs the size the run is actually rendering. A run that
	// reports "no moon" has to be able to state its own disc size from its own log.
	float AppliedMoonAngularRadiusDeg = -1.f;

	// Last disc gain written into the MPC. Same -1 sentinel and same reason: a run
	// that reports "the moon is a white blob" has to be able to state the gain it
	// was rendering at without anyone guessing at an asset default.
	float AppliedMoonDiscBrightness = -1.f;

	// Whether the moon was above the horizon on the previous update. -1 = nothing
	// logged yet, which is deliberately distinct from both 0 and 1 so that a run
	// whose moon is down for its whole length still emits one line saying so.
	int32 AppliedMoonUp = -1;

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
				if (Month < 1 || Month > 12 || Day < 1 || Day > VoxelSky::kDaysInMonth[Month - 1])
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
		VoxelSky::MonthDayFromDayOfYear(ResolvedDayOfYear, ResolvedMonth, ResolvedDay);
		int32 RequestedMonth = 1, RequestedDay = 1;
		VoxelSky::MonthDayFromDayOfYear(TargetDayOfYear, RequestedMonth, RequestedDay);

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
		       TEXT("MoonTempK=%.0f (requested %.0f, tint %.2f) ")
		       TEXT("ShadowUpdateHz=%.2f ExposureMode=%d ExposureBias=%.2f ")
		       TEXT("DayEV=%.2f SunsetEV=%.2f TwilightEV=%.2f DeepNightEV=%.2f DeepNightDrop=%.2f ")
		       TEXT("Dome=%d DomeRadiusUU=%.0f StarRotationOffsetTurns=%.4f ")
		       TEXT("StarsAt0Deg=%.2f StarsAt-6Deg=%.2f StarsAt-12Deg=%.2f ")
		       TEXT("StarAmbientGain=%.4f (%s, calibrated at StarGain %.2f)"),
		       VoxelSky::IsEnabled() ? 1 : 0, VoxelSky::GetTimeScale(), DayLength, DaysPerYear,
		       VoxelSky::GetOriginLatitudeDeg(), VoxelSky::GetOriginLongitudeDeg(),
		       VoxelSky::IsMoonEnabled() ? 1 : 0,
		       // Read back through the clamping accessors, never off the cvar: the
		       // sun's is FLOORED away from zero and a run that hit that floor has
		       // to be able to say so from its own log.
		       VoxelSky::GetSunIntensity(), VoxelSky::GetMoonIntensity(),
		       // Resolved AND requested, because the mired lerp against the tint
		       // strength and UE's own 1000..15000 clamp can each move the answer.
		       ResolveMoonTemperatureK(), VoxelSky::GetMoonTemperatureK(), VoxelSky::GetMoonTintStrength(),
		       VoxelSky::GetShadowUpdateHz(),
		       VoxelSky::GetExposureMode(), VoxelSky::GetExposureBias(),
		       // FOUR points on the curve, and each one exists because two
		       // policies that used to be indistinguishable in a log are not any
		       // more. TwilightEV and DeepNightEV used to be the same number,
		       // which is why a midwinter midnight rendered like a midsummer one.
		       // SunsetEV is the newer of the pair: the cap's onset moved from -2
		       // to -6, so the horizon is no longer at the cap and a capture that
		       // does not print 0 deg cannot say which side of that change it ran
		       // on. A capture states the exposure policy it ran under rather than
		       // the one in this revision of the source.
		       ExposureBiasForSunAltitude(60.0), ExposureBiasForSunAltitude(0.0),
		       ExposureBiasForSunAltitude(-6.0),
		       ExposureBiasForSunAltitude(-30.0), VoxelSky::GetDeepNightDropEV(),
		       // THREE points on the star fade rather than the two endpoints,
		       // because the endpoints are 0 and 1 by construction and would say
		       // nothing. The -6 sample is the one worth having in a capture log:
		       // it is where the exposure curve reaches its +15.6 cap, so it is
		       // where the star gain and the exposure lift are BOTH at their most
		       // aggressive, and it is the rung to compare against if a twilight
		       // frame comes back with a washed-out sky.
		       VoxelSky::IsDomeEnabled() ? 1 : 0, VoxelSky::GetDomeRadiusUU(),
		       VoxelSky::GetStarRotationOffsetTurns(),
		       StarBrightnessForSunAltitude(0.0), StarBrightnessForSunAltitude(-6.0),
		       StarBrightnessForSunAltitude(-12.0),
		       // RESOLVED and MODE, on the same line as StarsAt-12Deg (which IS
		       // voxel.Sky.StarGain, the fade being 1 there) so the two halves of the
		       // star calibration are readable side by side in one grep. Printing the
		       // cvar instead would print the -1 sentinel, i.e. would report every
		       // shipped run as having negative starlight.
		       VoxelSky::GetStarAmbientGain(),
		       VoxelSky::IsStarAmbientGainDerived() ? TEXT("DERIVED") : TEXT("OVERRIDDEN"),
		       kStarAmbientCalibrationStarGain);
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
	SkyDome = nullptr;
	bHasState = false;
	// Impl.Reset() releases the TStrongObjectPtr on MPC_VoxelSky with it, which is
	// the whole reason that cache is a strong pointer rather than a raw one: the
	// collection stops being rooted by this subsystem at exactly the moment the
	// subsystem stops existing, without a second teardown line to forget.
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

			// bUseTemperature only. THE TEMPERATURE ITSELF IS NOT SET HERE -- it
			// is pushed on every light update from ApplyLightsFromState, the same
			// way the sun's is. It used to be one SetTemperature call at spawn,
			// which is why voxel.Sky.MoonTemperatureK could not have existed: a
			// colour written once during OnWorldBeginPlay is a colour no cvar and
			// no console can reach afterwards. The moon's colour is the number in
			// this file most likely to be argued about after a play session and it
			// should not need a rebuild to settle -- the same argument
			// voxel.Sky.MoonIntensity is already exposed under.
			MoonComp->SetUseTemperature(true);

			// NO SHADOWS FROM THE MOON. A second shadow-casting directional
			// light is a second whole-scene shadow setup and a second set of
			// cascades every frame -- the single most expensive thing in this
			// file -- bought for shadows cast by a light running ~8.6 stops below
			// the sun (voxel.Sky.MoonIntensity) while the exposure curve lifts
			// the whole frame nearly seven. It is not a close call today. Revisit
			// only with a number from the W7 leg.
			MoonComp->SetCastShadows(false);

			// ===========================================================
			// KILL UE'S OWN MOON DISC. There is exactly one moon and
			// M_NightSky draws it.
			//
			// This light is atmosphere light index 1 (just above), and UE
			// draws a disc for every atmosphere light it has -- that is what
			// index 1 is FOR, and it is what the pre-dome sky showed as "the
			// moon". It is a flat, untextured, PHASELESS disc: a uniform
			// blob of the light's own colour, with no terminator, because
			// the SkyAtmosphere has no concept of the moon being lit by the
			// sun. Now that M_NightSky draws a textured moon with a
			// geometrically correct terminator AT THE SAME DIRECTION, both
			// would be drawn in the same place -- and the phaseless one is
			// on top of the crescent, filling in the dark limb, which reads
			// as "the phase code does not work" rather than as "there are
			// two discs".
			//
			// SetAtmosphereSunDiskColorScale (verified against
			// DirectionalLightComponent.h:317 in UE 5.8) scales ONLY the
			// disc's colour, so this removes the disc and changes nothing
			// else: the moon keeps its intensity, its temperature, its
			// direct lighting and its own scattering contribution to the
			// night sky. It is set unconditionally rather than under
			// voxel.Sky.DomeEnabled because the alternative -- switching UE's
			// disc back on whenever the dome is hidden -- would make the A/B
			// compare two different moons instead of measuring the dome, and
			// because a phaseless disc is not a thing this project wants
			// back on any code path. voxel.Sky.DomeEnabled 0 therefore means
			// NO moon disc at all; the cvar's help string says so.
			//
			// Once at spawn, not per update, and that is safe here for the
			// reason the temperature is NOT: this is a constant, so no cvar
			// or console command needs to reach it afterwards.
			// ===========================================================
			MoonComp->SetAtmosphereSunDiskColorScale(FLinearColor::Black);
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
		// WHAT THE EXPOSURE RETUNES DID TO THE STEP BETWEEN THE TWO VOLUMES.
		// Nothing about the ownership rule changed, but the SIZE of the jump at a
		// cave mouth did, three times now, and it is asymmetric enough to be worth
		// stating. This curve used to run +7.0 (day) to +12.0 (night); the W6
		// retune made it +8.7 to +15.6; the deep-night ramp
		// (DeepNightDropForSunAltitude) then gave 2.0 stops back below -18 deg; and
		// the measured retune (see ExposureBiasForSunAltitude) then moved the cap's
		// ONSET from -2 deg to -6 deg, which is what changed the sunset case below
		// out of all recognition. The cave's +10 did NOT move through any of that --
		// it is still the only exposure number in this project backed by an A/B, and
		// re-deriving it to tidy up a step would throw that away. The four cases
		// that now exist:
		//
		//   into a cave at NOON               +8.8  -> +10.0   1.2 stops BRIGHTER
		//   into a cave at SUNSET  (0 deg)    +10.9 -> +10.0   0.9 stops DARKER
		//   into a cave in TWILIGHT (<= -6)   +15.6 -> +10.0   5.6 stops DARKER
		//   into a cave in DEEP NIGHT (<=-18) +13.6 -> +10.0   3.6 stops DARKER
		//
		// Twilight is the worst case and it is unchanged. Deep night, the one a
		// player actually meets most often, improved by the full 2.0 stops. And
		// SUNSET -- which used to be a 4.1-stop DROP, because the old table had
		// already reached the +15.6 moon cap by 0 deg -- is now very nearly
		// seamless. That is a side effect of fixing the day curve, not something
		// anyone tuned for, and it is the one case worth re-checking by eye rather
		// than by arithmetic.
		// A cave being darker than the moonlit surface outside it is arguably
		// correct -- a cave at night has no light in it at all and the lamp is the
		// point -- and if it still reads badly the fix belongs in
		// AVoxelClipmapActor's rig (make CaveExposureEV100 track the sky's night
		// end, re-measured), NOT in flattening this curve's night end, which is
		// doing separate work.
		//
		// BOTH COPIES OF THIS COMMENT NOW AGREE, AND THE POINTER THAT SAID
		// OTHERWISE IS GONE. The previous revision of this block ended with a note
		// that the VoxelClipmapActor.cpp copy was deliberately stale at the
		// two-case W6 version, to be pasted over the next time that file was open.
		// It has been. Both blocks state the same four cases, and the rule stands:
		// if you change one, change both -- they exist in duplicate precisely
		// because whoever next touches either file will only read that one.
		// ===================================================================
		SkyExposurePP->Priority = 10.f;
		SkyExposurePP->bEnabled = false; // ApplyExposureFromState turns it on
	}

	// --- the night-sky dome (stars + the textured, phased moon disc) ---------
	//
	// SPAWNED AT THE SPAWN COLUMN like every other actor in this function, for
	// the reason stated at the top of it: an actor left at the world origin while
	// the player is 20,000 km out is not "slightly misplaced". For the dome
	// specifically the failure is total rather than cosmetic -- the camera would
	// be OUTSIDE a 200 km sphere, and an additive two-sided sphere seen from
	// outside is a small ball of stars floating in the middle of the screen.
	// AVoxelSkyDomeActor::UpdateFollowCamera takes over on the first tick that
	// has a camera; this placement is what covers the frames before that.
	//
	// SPAWNED EVEN WHEN voxel.Sky.DomeEnabled IS 0, and hidden by the actor
	// itself. A hidden actor with no collision and no shadow costs nothing per
	// frame, and it makes the cvar a LIVE A/B control -- the same shape
	// voxel.Sky.Enabled has, where "off" is a state the rig can be put into and
	// taken back out of rather than a decision frozen at OnWorldBeginPlay.
	SkyDome = World.SpawnActor<AVoxelSkyDomeActor>(
		FVector(SpawnColumnXUU, SpawnColumnYUU, 0.0), FRotator::ZeroRotator);
	if (!SkyDome)
	{
		UE_LOG(LogVoxelSky, Error,
		       TEXT("AVoxelSkyDomeActor failed to spawn -- there is no night-sky dome this run, so there are NO ")
		       TEXT("STARS and NO MOON DISC. The light rig, the atmosphere and the exposure curve are ")
		       TEXT("unaffected, so a night capture will still be correctly dark and correctly lit; it will ")
		       TEXT("simply be empty overhead, and this line is what distinguishes that from a material or ")
		       TEXT("texture failure."));
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

	// The vectors themselves, for M_NightSky. NOT NEGATED -- see
	// ApplySkyMaterialParams, and see the negation two blocks down that exists
	// only because a DirectionalLight's forward is where the light TRAVELS.
	Impl->SunDirection = Sun.Direction;
	Impl->MoonDirection = Moon.Direction;

	// --- exposure, EVERY frame ---------------------------------------------
	//
	// Deliberately NOT under the shadow cadence cap. Exposure is a scalar on a
	// post-process; writing it costs nothing in the renderer and busts no shadow
	// cache, and stepping it at 10 Hz through a sunset would produce a visible
	// brightness staircase for no saving at all. The at-most-100 ms disagreement
	// between the stepped light and the continuous exposure is not observable.
	ApplyExposureFromState();

	// --- the night sky's material parameters, ALSO EVERY frame ---------------
	//
	// Immediately after the exposure and, like it, OUTSIDE the
	// voxel.Sky.ShadowUpdateHz gate below. Same first reason: these are uniform
	// writes into one UMaterialParameterCollectionInstance, they touch no
	// primitive and bust no cached shadow setup, so the cap has nothing to save
	// here. And one reason of their own, which is stronger than exposure's: this
	// drives the position of a moon disc 0.52 degrees wide. A 10 Hz step through a
	// 2400 s day moves the sun and moon 0.015 degrees per step -- invisible on a
	// shadow, but 3% of the moon's own diameter, i.e. a disc that visibly jerks
	// ten times a second while everything around it moves smoothly.
	ApplySkyMaterialParams();

	// --- light orientation, CADENCE-CAPPED ----------------------------------
	//
	// WHY THE CAP EXISTS: a directional light whose rotation changes every frame
	// invalidates UE's cached whole-scene shadow setup every frame. Stepping the
	// rotation instead lets that cache survive between steps -- at the cost of
	// quantising shadow motion, which shows up as a pop if the step is coarse
	// enough. At the default 2400 s day and 10 Hz that step is 0.015 degrees of
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
	S.MoonTemperatureK = ResolveMoonTemperatureK();
	if (MoonLight)
	{
		if (UDirectionalLightComponent* MoonComp = Cast<UDirectionalLightComponent>(MoonLight->GetLightComponent()))
		{
			// Unlike the sun, the moon MAY reach zero and leave the scene. Losing
			// AtmosphereLights[1] costs nothing -- index 0 is what the sky is
			// scattering at every hour -- and a moon below the horizon should not
			// be contributing to it. SetIntensity early-outs on an unchanged
			// value, so this does not churn the render state while it sits at 0.
			// The luminance recip is folded in HERE rather than into the tint,
			// because the tint has to be normalised to max = 1 to survive
			// ULightComponent's 8-bit FColor storage (kMoonAlbedoTint). Together the
			// two are exactly luminance-neutral: adding the moon's colour must not
			// change how many lux it delivers, or every rung of the exposure ladder
			// would have been silently re-based by 0.037 stops.
			MoonComp->SetIntensity(S.MoonIntensity * kMoonAlbedoTintLuminanceRecip);
			MoonComp->SetVisibility(bMoonOn && S.MoonIntensity > 0.f);

			// --- THE MOON'S OWN COLOUR, TAKEN FROM THE MOON ---------------------
			//
			// LightColor carries the lunar regolith's measured chromaticity and
			// Temperature carries the illuminant, and UE multiplies them
			// (ULightComponent::GetColoredLightBrightness), so the light the ground
			// receives is literally "sunlight, reflected off the surface
			// T_MoonColor is a photograph of". The disc and the light are now the
			// same colour by construction rather than by two people agreeing.
			//
			// WHY THIS REPLACED A PURE COLOUR TEMPERATURE. A temperature alone can
			// only travel along the blackbody locus, and lunar basalt is not on it
			// -- it is a slightly warm grey (kMoonAlbedoTint, R:B 1.116). The old
			// rig expressed the moon's colour as 12000 K, which is not the moon's
			// colour by any measurement; it was a deliberate perceptual convention,
			// documented at length beside kMoonTemperatureK, and the owner has since
			// asked for the natural colour instead. kMoonTintStrength now defaults
			// to 0, which hands the illuminant the SUN's temperature -- the answer
			// the spectrum gives, since moonlight is reflected sunlight.
			//
			// THE BLUE IS NOT DELETED, and must not be: voxel.Sky.MoonTintStrength 1
			// restores it exactly, which is the A/B for "is the night blue because
			// we chose it or because the sky is". Every word of the argument for the
			// blue is still at kMoonTemperatureK; what changed is which side of that
			// argument ships as the default.
			//
			// SET UNCONDITIONALLY rather than latched: SetLightColor early-outs on
			// an unchanged FColor, so this is one comparison per update, and a
			// constant that is only written once is a constant a Blueprint can clear
			// without anything noticing -- the same reason SetUseTemperature is
			// re-asserted below.
			MoonComp->SetLightColor(
				FLinearColor(kMoonAlbedoTintR, kMoonAlbedoTintG, kMoonAlbedoTintB),
				/*bSRGB=*/false); // LINEAR. sRGB-encoding a linear tint would apply the transfer curve twice.

			// COLOUR, EVERY UPDATE, so voxel.Sky.MoonTemperatureK and
			// voxel.Sky.MoonTintStrength are live knobs rather than build-time
			// constants. SetUseTemperature/SetTemperature both early-out on an
			// unchanged value (LightComponent.cpp), so on the overwhelming
			// majority of updates this is two comparisons. Re-asserting
			// bUseTemperature alongside is the same belt-and-braces the sun gets:
			// a Blueprint or level tool that cleared it would otherwise leave the
			// moon pure white with no log line anywhere to say so.
			//
			// A resolved value is LOGGED ON CHANGE and never echoed from the cvar,
			// because two things can move it out from under a request: the mired
			// lerp against voxel.Sky.MoonTintStrength, and UE's own 1000..15000
			// clamp inside MakeFromColorTemperature.
			MoonComp->SetUseTemperature(true);
			if (!FMath::IsNearlyEqual(MoonComp->Temperature, S.MoonTemperatureK, 0.5f))
			{
				MoonComp->SetTemperature(S.MoonTemperatureK);
				UE_LOG(LogVoxelSky, Log,
				       TEXT("VoxelSky moon colour RESOLVED: %.0f K illuminant (requested %.0f K, tint ")
				       TEXT("strength %.2f against the sun's %.0f K), times the MEASURED lunar albedo ")
				       TEXT("(%.3f, %.3f, %.3f) from T_MoonColor -- so the ground is lit by sunlight ")
				       TEXT("reflected off the same surface the disc is drawn with. Read back from the ")
				       TEXT("component: %.0f K. At tint strength 0 this is the SPECTRUM's answer, not a ")
				       TEXT("convention; voxel.Sky.MoonTintStrength 1 restores the old 12000 K blue and ")
				       TEXT("is the A/B -- see kMoonTemperatureK for why that argument is a real one."),
				       S.MoonTemperatureK, VoxelSky::GetMoonTemperatureK(), VoxelSky::GetMoonTintStrength(),
				       kSunTemperatureK, kMoonAlbedoTintR, kMoonAlbedoTintG, kMoonAlbedoTintB,
				       MoonComp->Temperature);
			}

			// --- moon shadows, off unless asked for ---------------------------
			//
			// Same hysteresis-and-read-back shape as the sun's cadence above, and
			// gated on the SAME condition the moonlight itself is gated on
			// (S.MoonIntensity > 0, which already folds in the horizon gate, the
			// illuminated fraction and the daylight suppression). A moon that is
			// down, new, or drowned by the sun therefore never sets up a cascade
			// even with voxel.Sky.MoonShadows at 1 -- the cost is paid only on the
			// nights it could possibly buy anything.
			//
			// See CVarSkyMoonShadows for what it costs and why the default is 0.
			// The value is READ BACK from the component rather than echoed, for the
			// reason the sun's line gives: SetCastShadows early-outs when
			// AreDynamicDataChangesAllowed() is false, which is exactly what a
			// Stationary-mobility regression would produce, and an echoed log would
			// report a change that did not happen.
			const bool bWantMoonShadows =
				VoxelSky::AreMoonShadowsEnabled() && bMoonOn && S.MoonIntensity > 0.f;
			if (bWantMoonShadows != Impl->bMoonShadowsOn)
			{
				Impl->bMoonShadowsOn = bWantMoonShadows;
				MoonComp->SetCastShadows(bWantMoonShadows);
				UE_LOG(LogVoxelSky, Log,
				       TEXT("VoxelSky moon shadows %s at moon altitude %+.2f deg, moon intensity %.4f ")
				       TEXT("(CastShadows read back %d, voxel.Sky.MoonShadows=%d). ON means a SECOND ")
				       TEXT("whole-scene cascade set every frame -- see CVarSkyMoonShadows for the cost ")
				       TEXT("argument. The moon's LIGHT is voxel.Sky.MoonIntensity and is unaffected ")
				       TEXT("either way; this only decides whether that light is occluded."),
				       bWantMoonShadows ? TEXT("ON") : TEXT("OFF"), S.MoonAltitudeDeg, S.MoonIntensity,
				       MoonComp->CastShadows ? 1 : 0, VoxelSky::AreMoonShadowsEnabled() ? 1 : 0);
			}
		}
	}

	// --- who owns the forward / translucent single-directional-light slot -----
	//
	// THE WHOLE ARGUMENT IS AT kForwardMoonPrimarySunBelowDeg. Read it before
	// touching this: it has the engine file:line evidence for why two directional
	// lights at the default priority 0 make the renderer pick by raw brightness,
	// why that means a sun 37 degrees below the horizon owns the lake all night,
	// and why hiding a light instead would re-open the black-twilight defect this
	// function opens with.
	//
	// THIS IS ALSO WHAT SILENCES THE EDITOR WARNING. The conflict count at
	// LightGridInjection.cpp:1502-1511 only rises when two lights SHARE the top
	// priority, so the sole invariant that matters here is that these two values
	// are never equal. Both are written on every update, unconditionally, for the
	// same belt-and-braces reason the sun's SetAtmosphereSunLightIndex is: a
	// Blueprint or level tool that reset one of them would otherwise put both back
	// on 0 with nothing in the log to say so. SetForwardShadingPriority early-outs
	// on an unchanged value (DirectionalLightComponent.cpp:1448-1455), so the
	// steady-state cost is two integer compares.
	//
	// The moon only takes the slot if it is actually DELIVERING light --
	// S.MoonIntensity is already the product of the horizon gate, the illuminated
	// fraction and the daylight suppression, so this one test covers moonset, new
	// moon and voxel.Sky.MoonEnabled 0 together. On a moonless night the sun keeps
	// the slot, which changes nothing versus what shipped: a below-horizon sun and
	// an absent moon both deliver exactly zero to an up-facing surface.
	{
		const bool bMoonPrimary =
			S.MoonIntensity > 0.f &&
			(Impl->AppliedForwardPrimaryIsMoon == 1
				? S.SunAltitudeDeg < kForwardSunPrimarySunAboveDeg   // stay on the moon until -5
				: S.SunAltitudeDeg < kForwardMoonPrimarySunBelowDeg); // take the moon below -6

		UDirectionalLightComponent* SunComp = SunLight
			? Cast<UDirectionalLightComponent>(SunLight->GetLightComponent()) : nullptr;
		UDirectionalLightComponent* MoonComp = MoonLight
			? Cast<UDirectionalLightComponent>(MoonLight->GetLightComponent()) : nullptr;

		if (SunComp)
		{
			SunComp->SetForwardShadingPriority(
				bMoonPrimary ? kForwardPrioritySecondary : kForwardPriorityPrimary);
		}
		if (MoonComp)
		{
			MoonComp->SetForwardShadingPriority(
				bMoonPrimary ? kForwardPriorityPrimary : kForwardPrioritySecondary);
		}

		const int32 NowMoon = bMoonPrimary ? 1 : 0;
		if (NowMoon != Impl->AppliedForwardPrimaryIsMoon)
		{
			Impl->AppliedForwardPrimaryIsMoon = NowMoon;
			// READ BACK off the components, never echoed from the request:
			// SetForwardShadingPriority is guarded by AreDynamicDataChangesAllowed()
			// and does nothing at all when that is false, so echoing the intent
			// would report a handover that never occurred -- and the symptom of the
			// silent failure (water lit by the wrong body) is invisible in a log.
			UE_LOG(LogVoxelSky, Log,
			       TEXT("VoxelSky forward-shading PRIMARY is now the %s (sun altitude %+.2f deg, moon ")
			       TEXT("altitude %+.2f deg, moon intensity %.4f). Read back: sun priority %d, moon ")
			       TEXT("priority %d -- these MUST differ or the editor's 'multiple directional lights ")
			       TEXT("are competing' warning returns (LightGridInjection.cpp:1662). This is the ONE ")
			       TEXT("light that lights translucency, water and volumetric fog; handover at sun ")
			       TEXT("%+.1f deg with 1 deg of hysteresis, derived from kMoonSunSuppressStartDeg."),
			       bMoonPrimary ? TEXT("MOON") : TEXT("SUN"), S.SunAltitudeDeg, S.MoonAltitudeDeg,
			       S.MoonIntensity,
			       SunComp ? SunComp->ForwardShadingPriority : -1,
			       MoonComp ? MoonComp->ForwardShadingPriority : -1,
			       kForwardMoonPrimarySunBelowDeg);
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
		// VoxelClipmapActor.cpp:455-462 records what happens if you take the
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

void UVoxelSkySubsystem::ApplySkyMaterialParams()
{
	// ======================================================================
	// THE ONE THING TO KNOW BEFORE EDITING ANY NAME IN THIS FUNCTION.
	//
	// An MPC parameter name that does not resolve DOES NOT FAIL. It is not a
	// compile error, it is not a warning, and it is not a log line:
	// UMaterialExpressionCollectionParameter::Compile falls through to emitting a
	// CONSTANT when GetParameterIndex misses (MaterialExpressions.cpp:17179-17193,
	// cited by Tools/create_sky_material.py:10-15, which raises on the same
	// condition for the same reason). So a typo on the material side bakes a
	// constant into the shader, and a typo HERE -- SetScalarParameterValue with a
	// name the collection does not have -- writes into nothing at all and leaves
	// the asset's DEFAULT standing forever.
	//
	// What that looks like: a perfectly plausible night sky. Stars, sharp,
	// correctly oriented, frozen at sidereal time zero; a moon due east at 45
	// degrees, permanently full, that never moves. Nothing about it says
	// "unbound". EVERY name below was read out of Tools/create_sky_material.py's
	// SCALAR_PARAMS / VECTOR_PARAMS tables (create_sky_material.py:442-463) rather
	// than typed from memory, and the two log lines in this function print the
	// values back so a capture can prove the writes landed.
	// ======================================================================

	if (!Impl)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	FVoxelSkyState& S = Impl->State;

	// --- resolve the collection, ONCE ---------------------------------------
	if (!Impl->bSkyParamsLoadAttempted)
	{
		Impl->bSkyParamsLoadAttempted = true;
		Impl->SkyParams.Reset(LoadObject<UMaterialParameterCollection>(nullptr, kSkyCollectionPath));
		if (!Impl->SkyParams.IsValid())
		{
			// ERROR, NOT WARNING, AND THIS IS THE POINT OF THE WHOLE DIAGNOSTIC
			// BUDGET IN THIS FILE. Without the collection the material still
			// compiles, the dome still draws, and it draws MPC_VoxelSky's authored
			// defaults -- a static star field at the wrong sidereal time with a
			// permanently-full moon parked due east. That is a WORSE failure than a
			// black sky, because it looks like a working feature. Nothing else in
			// the frame can tell anyone it happened, so this line has to.
			UE_LOG(LogVoxelSky, Error,
			       TEXT("%s did not load. M_NightSky will render this collection's AUTHORED DEFAULTS -- a star ")
			       TEXT("field frozen at sidereal time 0 and a permanently-full moon due east at 45 deg -- ")
			       TEXT("which looks like a working night sky and is not one. Nothing in this subsystem can ")
			       TEXT("reach the material without it. Run Tools/create_sky_material.py."),
			       kSkyCollectionPath);
		}
		else
		{
			UE_LOG(LogVoxelSky, Log,
			       TEXT("VoxelSky material params bound to %s (%d scalars, %d vectors in the asset). Driven ")
			       TEXT("EVERY FRAME, outside the voxel.Sky.ShadowUpdateHz gate."),
			       kSkyCollectionPath,
			       Impl->SkyParams->ScalarParameters.Num(), Impl->SkyParams->VectorParameters.Num());
		}
	}
	UMaterialParameterCollection* Collection = Impl->SkyParams.Get();
	if (!Collection)
	{
		return; // already logged, once, above
	}

	// --- directions: TOWARD the body, NOT negated ---------------------------
	//
	// ===================================================================
	// THE SIGN, AND IT IS THE OPPOSITE OF THE ONE TWO FUNCTIONS UP.
	//
	// FSunState::Direction and FMoonState::Direction point FROM the observer
	// TOWARD the body (VoxelEphemeris.h:75-77, 84). Tick negates them -- see
	// `SunLight->SetActorRotation((-Sun.Direction).Rotation())` -- and that
	// negation exists for exactly one reason, which has nothing to do with the
	// ephemeris: a DirectionalLight's forward vector is the direction the light
	// TRAVELS, which is away from the body. It is a property of the light, not of
	// the vector.
	//
	// M_NightSky is not a light. It dots these against the view direction to
	// decide "am I looking at the moon" and "which way is the sun lighting it
	// from" (create_sky_material.py, "MOON DISC" and "TERMINATOR"), and both of
	// those want the TOWARD form -- which is why the ephemeris defines it that
	// way in the first place (VoxelEphemeris.h:38-41: every consumer but the light
	// wants toward). So they go through unchanged.
	//
	// GETTING THIS BACKWARDS IS NOT SUBTLE AND IS ALSO NOT OBVIOUS. The moon
	// would be drawn exactly 180 degrees from where the moonlight comes from --
	// below the horizon whenever the moon is up, hence invisible, hence a night
	// sky with stars and no moon at all. That reads as "the moon disc code does
	// not work", and the search would start in the material.
	// ===================================================================
	UKismetMaterialLibrary::SetVectorParameterValue(
		World, Collection, TEXT("SunDirection"), FLinearColor(Impl->SunDirection));
	UKismetMaterialLibrary::SetVectorParameterValue(
		World, Collection, TEXT("MoonDirection"), FLinearColor(Impl->MoonDirection));

	// --- the moon's phase ----------------------------------------------------
	//
	// PhaseFraction, NOT IlluminatedFraction, and this is the one place in this
	// file where that is the right way round. ApplyLightsFromState uses
	// IlluminatedFraction because it wants "how much light is coming from the
	// moon". The material uses PhaseFraction for EARTHSHINE ONLY -- earthshine
	// tracks the EARTH's illuminated fraction as seen from the moon, which is the
	// complement of the moon's own phase, (1 + cos(2*pi*phase))/2, and that
	// requires the SIGNED, monotonic quantity (create_sky_material.py's
	// MoonPhaseFraction note). IlluminatedFraction is symmetric about full
	// (VoxelEphemeris.h:85-93) and cannot express it. The TERMINATOR does not use
	// this at all; it is pure geometry from SunDirection.
	UKismetMaterialLibrary::SetScalarParameterValue(
		World, Collection, TEXT("MoonPhaseFraction"), (float)S.MoonPhaseFraction);

	// --- the moon's LIGHT, as a fraction of the sun's ------------------------
	//
	// WHAT THIS IS FOR: M_WaterVoxel's moon glint, the specular "moon path on the
	// water" (create_water_voxel_material.py, build_moon_glint). Nothing else reads
	// it yet.
	//
	// WHY A RATIO AND NOT THE INTENSITY ITSELF, which is the question a reader will
	// have. The water material already carries a glint tint (2.6, 2.45, 2.15) that
	// was calibrated against the SUN at voxel.Sky.SunIntensity 15. Handing the
	// material the moon's absolute intensity would force it to divide by a
	// reference sun intensity to use that calibration -- i.e. a copy of 15.0 living
	// in a Python asset generator, silently wrong the first time anyone moves
	// voxel.Sky.SunIntensity, with the failure being a moon glint at the wrong
	// brightness and no diagnostic anywhere. Dividing HERE, where both numbers are
	// already in scope, means the material multiplies the sun's own calibrated tint
	// by "how much dimmer the moon is than the sun right now" and there is exactly
	// one copy of every constant involved.
	//
	// WHAT IT CARRIES, all of it for free, because S.MoonIntensity is already the
	// product the light itself is set from (ApplyLightsFromState): the horizon gate,
	// so the glint fades out as the moon sets and is zero once it is down; the
	// illuminated fraction, so a new moon lays no path on the water; the daylight
	// suppression, so the glint is gone before sunrise; and voxel.Sky.MoonIntensity,
	// so the owner dialling the moonlight moves the moon path by the same number of
	// stops as it moves the ground. Those four behaviours are the ones asked for and
	// none of them needed a line of material logic.
	//
	// THE DENOMINATOR IS GetSunIntensity() AND NOT kSunOuterSpaceIntensity, so the
	// ratio stays true if the sun is dialled at runtime. It cannot divide by zero:
	// GetSunIntensity is floored at UE_KINDA_SMALL_NUMBER, and that floor is
	// load-bearing for a different reason entirely (see its comment).
	const float MoonLightFraction = S.MoonIntensity / VoxelSky::GetSunIntensity();
	UKismetMaterialLibrary::SetScalarParameterValue(
		World, Collection, TEXT("MoonLightFraction"), MoonLightFraction);

	// --- how big the moon is drawn -------------------------------------------
	//
	// DEGREES, and a RADIUS -- build_moon multiplies by DEG2RAD and takes the sine
	// of it (create_sky_material.py, "disc mask"), so the units are the material's
	// and no conversion belongs here. Same rule as ObserverLatitude below.
	//
	// THIS PARAMETER USED TO BE ASSET-ONLY, and that is precisely how the moon went
	// missing without a single log line saying so. MPC_VoxelSky shipped
	// MoonAngularRadius = 0.26; nothing in C++ ever wrote it; and the drawn size
	// was therefore whatever the material's own trigonometry happened to make of
	// that number -- which for the graph's whole life was 6.283x too big because of
	// the Sine Period trap, and which became exactly 0.52 deg across the moment
	// that trap was fixed. An asset default nobody drives is a decision nobody
	// makes. Driving it puts the size where every other sky judgement in this file
	// already lives: on a cvar, with the physical value written down beside the
	// chosen one (kMoonDrawnAngularRadiusDeg).
	const float MoonRadiusDeg = VoxelSky::GetMoonAngularRadiusDeg();
	UKismetMaterialLibrary::SetScalarParameterValue(
		World, Collection, TEXT("MoonAngularRadius"), MoonRadiusDeg);

	// --- how BRIGHT the moon is drawn ----------------------------------------
	//
	// Driven for the same reason the radius is, and it is the same failure: the
	// MPC's 20.0 was an asset default no code path ever touched, so it was never a
	// decision and never showed up in a log. At 20.0 the disc clips to white and
	// the LROC texture only survives along the terminator. kMoonDiscBrightness has
	// the measured arithmetic; the short version is that 20.0 is 11x brighter than
	// physically correct and physically correct would clip too.
	const float MoonDiscBrightness = VoxelSky::GetMoonDiscBrightness();
	UKismetMaterialLibrary::SetScalarParameterValue(
		World, Collection, TEXT("MoonBrightness"), MoonDiscBrightness);

	// Logged on CHANGE, and the log states the pixel width rather than only the
	// angle, because "0.26 degrees" reads as a perfectly reasonable moon and "10
	// pixels" reads as the bug it was. The width is quoted at this project's
	// 2200-wide / 90 deg pose so the number is comparable between runs; it is a
	// stated reference pose, not a read of the actual viewport.
	if (!FMath::IsNearlyEqual(MoonRadiusDeg, Impl->AppliedMoonAngularRadiusDeg, 1.e-4f) ||
		!FMath::IsNearlyEqual(MoonDiscBrightness, Impl->AppliedMoonDiscBrightness, 1.e-4f))
	{
		Impl->AppliedMoonAngularRadiusDeg = MoonRadiusDeg;
		Impl->AppliedMoonDiscBrightness = MoonDiscBrightness;
		UE_LOG(LogVoxelSky, Log,
		       TEXT("VoxelSky moon DISC RESOLVED: %.3f deg radius (%.2f deg across, %.1fx the real ")
		       TEXT("moon's 0.26), gain %.3f. At 2200x1300 / 90 deg FOV that is about %.0f pixels wide. ")
		       TEXT("Written into %s (MoonAngularRadius + MoonBrightness), so the asset defaults are ")
		       TEXT("dead -- voxel.Sky.MoonAngularRadiusDeg and voxel.Sky.MoonDiscBrightness are the ")
		       TEXT("knobs. These move the DISC ONLY; the ground is lit by voxel.Sky.MoonIntensity."),
		       MoonRadiusDeg, 2.f * MoonRadiusDeg, MoonRadiusDeg / 0.26f, MoonDiscBrightness,
		       // 1100 px per radian on the view axis: half of 2200 divided by
		       // tan(45 deg), which is 1. Spelled as the constant it evaluates to
		       // rather than as a tan() of a literal that is always the same angle.
		       2.f * FMath::DegreesToRadians(MoonRadiusDeg) * 1100.f,
		       kSkyCollectionPath);
	}

	// --- where the moon actually IS -------------------------------------------
	//
	// THE LINE THAT WAS MISSING WHEN IT MATTERED. Round 15 was reported as "stars
	// but no moon" and the log could not answer the first question anyone asks --
	// is the moon above the horizon at all, and how lit is it -- because nothing
	// printed the moon's own position. Settling it took a Python re-derivation of
	// ComputeMoon against the log's epoch. That is a diagnostic this file can
	// afford once per horizon crossing.
	//
	// TRANSITION-LATCHED on bMoonUp rather than printed per frame: a moon that
	// never rises then logs exactly one line saying so, which is the case that
	// needs the line most. A new moon (IlluminatedFraction ~ 0) is genuinely
	// invisible and is NOT a defect, so the illuminated fraction is on the same
	// line as the altitude -- the two together are the whole answer.
	const int32 MoonUpNow = S.bMoonUp ? 1 : 0;
	if (Impl->AppliedMoonUp != MoonUpNow)
	{
		Impl->AppliedMoonUp = MoonUpNow;
		UE_LOG(LogVoxelSky, Log,
		       TEXT("VoxelSky moon %s: altitude %+.2f deg, azimuth %.1f deg, %.0f%% illuminated (phase ")
		       TEXT("%.3f, 0=new 0.5=full). MoonEnabled=%d, disc radius %.3f deg. A moon BELOW the ")
		       TEXT("horizon or at ~0%% illuminated is not a bug -- both draw nothing on purpose."),
		       S.bMoonUp ? TEXT("IS UP") : TEXT("has SET"),
		       S.MoonAltitudeDeg, S.MoonAzimuthDeg, 100.0 * S.MoonIlluminatedFraction,
		       S.MoonPhaseFraction, VoxelSky::IsMoonEnabled() ? 1 : 0, MoonRadiusDeg);
	}

	// --- the observer ---------------------------------------------------------
	//
	// DEGREES, passed straight through with no conversion, because the material
	// takes degrees for precisely this reason (create_sky_material.py's
	// ObserverLatitude note: "one fewer place to get a factor wrong"). This is
	// what puts the celestial pole at the right altitude, so it is the parameter
	// that makes the star field a sky rather than a painting on the inside of a
	// dome -- and it moves as the player walks north (GeoFromWorldUU), which is
	// why it is written per frame rather than once.
	UKismetMaterialLibrary::SetScalarParameterValue(
		World, Collection, TEXT("ObserverLatitude"), (float)S.LatitudeDeg);

	// --- the star field's rotation ------------------------------------------
	//
	// TURNS, not degrees and not radians: StarRotation is local sidereal time as a
	// fraction of a rotation. The material folds LST wholesale into this one
	// scalar because RA appears in its UV only as (LST - H)/2pi and LST is
	// constant across the frame (create_sky_material.py, "EQUIRECT UV").
	//
	// The FGeoCoord is rebuilt from the state rather than threaded in from Tick:
	// S.LatitudeDeg and S.LongitudeDeg are assigned directly from the same
	// FGeoCoord the ephemeris was evaluated with, a few lines apart, so this is
	// the same value and not an approximation of it.
	//
	// -------------------------------------------------------------------------
	// AND IT IS MULTIPLIED BY StarUDirection, WHICH IS READ BACK OUT OF THE
	// ASSET. This is the one piece of coupling in this function and it is not
	// optional.
	//
	// StarUDirection (+1 / -1) exists because an equirectangular celestial map
	// can be authored for viewing from outside the sphere or from inside it, and
	// nobody has been able to check which this one is without an editor -- it is
	// the material author's single named unverifiable ("WHAT COULD NOT BE
	// VERIFIED", item 1), and the stated fix is to flip the MPC scalar to -1 and
	// re-shoot, with no regeneration.
	//
	// THE TRAP: that flip alone does not work, because the material's U is
	//     u = StarRotation + StarUDirection * (-H / 2pi)
	// and RA/2pi = LST/2pi - H/2pi. At StarUDirection = +1 that identifies
	// StarRotation with +LST/360 turns. At -1 the H term changes sign, so
	// correctly mirroring the map requires StarRotation to be -LST/360 -- and if
	// C++ keeps feeding +LST/360, the sky comes out mirrored AND rotating
	// BACKWARDS. Since "does it rotate the right way about the pole" is the exact
	// test the author documented for deciding the handedness, an uncompensated
	// flip would make that test unreadable: both settings would fail it, for two
	// different reasons, and the honest conclusion would be that neither is
	// right.
	//
	// So the sign is taken from the asset. One MPC read per frame (a lookup in the
	// world's collection instance), and the flip becomes a genuine one-value asset
	// edit exactly as documented. Defaults to +1 if the parameter is missing --
	// GetScalarParameterValue returns 0 for an unknown name, and a zero here would
	// freeze the star field solid, which is a much worse failure than assuming the
	// documented default.
	// -------------------------------------------------------------------------
	const float RawStarU = UKismetMaterialLibrary::GetScalarParameterValue(
		World, Collection, TEXT("StarUDirection"));
	const double StarU = RawStarU >= 0.f ? 1.0 : -1.0;

	const VoxelSky::FGeoCoord Geo{S.LatitudeDeg, S.LongitudeDeg};
	const double LstDeg = VoxelSky::LocalSiderealTimeDeg(S.JulianDay, Geo);
	// The offset rides INSIDE the StarU factor, not outside it. Both terms are
	// rotations about the same polar axis in the same UV, so mirroring the map has
	// to mirror them together or a calibrated offset would flip to the wrong side
	// the moment the handedness was corrected -- and the person doing that would
	// then be re-calibrating an offset they had already found.
	const double StarTurns = StarU * (LstDeg / 360.0 + VoxelSky::GetStarRotationOffsetTurns());
	UKismetMaterialLibrary::SetScalarParameterValue(
		World, Collection, TEXT("StarRotation"), (float)StarTurns);

	if (!FMath::IsNearlyEqual(StarU, Impl->AppliedStarUDirection, 0.01))
	{
		Impl->AppliedStarUDirection = StarU;
		UE_LOG(LogVoxelSky, Log,
		       TEXT("VoxelSky star map handedness RESOLVED: StarUDirection=%+.1f (read back from %s, raw ")
		       TEXT("%+.3f), so StarRotation is being driven as %+.1f * (LST/360 + %.4f turns). Flipping the ")
		       TEXT("MPC scalar mirrors the map AND re-signs this drive together; driving +LST against a -1 ")
		       TEXT("map would rotate the sky backwards."),
		       StarU, kSkyCollectionPath, RawStarU, StarU, VoxelSky::GetStarRotationOffsetTurns());
	}

	// --- the sunrise fade ----------------------------------------------------
	//
	// The ONLY thing that stops stars rendering over a blue afternoon: the dome is
	// additive and composited after the atmosphere, so nothing in the renderer
	// extinguishes them (see StarBrightnessForSunAltitude for the whole argument,
	// and create_sky_material.py's "THE COST OF THAT CHOICE" for the author's
	// statement of the same fact).
	//
	// IT DOES NOT TOUCH THE MOON, and this comment used to claim that it did.
	// Corrected against the graph rather than against intent: build_moon's gain is
	// MoonBrightness * horizonFade * disc (create_sky_material.py:730) and
	// StarBrightness appears nowhere in it -- only build_stars multiplies by it
	// (:628). So the moon disc renders at its full MPC gain whenever it is above
	// the horizon, including at midday, when the compressed calendar puts it above
	// the horizon for roughly half of every game day.
	//
	// LEFT AS IT IS, DELIBERATELY, because a daylight moon is REAL -- the actual
	// moon is plainly visible in a blue sky for much of every month -- and the
	// question of how bright it should read against one is a judgement to settle on
	// a screenshot, not to decide here by making the comment true. What is NOT
	// acceptable is the file asserting a fade that does not exist, because the next
	// person to debug a too-bright daytime moon would look for it in the C++ and it
	// is not there. If a fade is wanted, it belongs in build_moon's gain next to
	// horizon_fade, on its own MPC scalar, so that the moon and the stars can be
	// faded on different curves -- they are not the same problem.
	UKismetMaterialLibrary::SetScalarParameterValue(
		World, Collection, TEXT("StarBrightness"),
		(float)StarBrightnessForSunAltitude(S.SunAltitudeDeg));

	// --- the capture's star gain (the star ambient calibration) ---------------
	//
	// Read by M_SkyAtmosphereDome ONLY, and only on the Reflection branch of its
	// ReflectionCapturePassSwitch -- so at any gain it is invisible on screen and
	// only ever changes what the SkyLight's real-time capture integrates
	// (View.RenderingReflectionCaptureMask, Common.ush:2306-2309, is 0 in the main
	// view and 1 in the capture, ReflectionEnvironmentRealTimeCapture.cpp:570).
	//
	// RESOLVED ONCE, then written and logged from that one value. Calling the
	// accessor twice would be a second read of a live cvar between the write and
	// the log, so a console edit landing in the gap would make the log describe a
	// frame that was never rendered -- the exact class of lie the read-back rule in
	// this file exists to forbid.
	//
	// NOTE WHAT THE GAIN IS MULTIPLYING. The MPC's StarBrightness, written a few
	// lines above, is sunriseFade * voxel.Sky.StarGain, and BOTH domes multiply it
	// (create_sky_material.py:628, create_sky_atmosphere_dome_material.py:286-289).
	// So this scalar is exactly the ratio (capture star radiance)/(visible star
	// radiance) and nothing else -- it carries no StarGain of its own and needs no
	// fade of its own. See kStarAmbientCalibration for the measurement that fixes
	// its value and for why deriving it from StarGain would be a double count.
	//
	// WRITTEN EVERY FRAME EVEN THOUGH IT IS A CVAR THAT RARELY MOVES, for the same
	// reason every other name in this function is: the MPC has no per-material
	// fallback, so "sometimes written" and "written once" both mean the asset
	// default is standing on the frames in between. There is no cheap path here to
	// take -- these are uniform writes that bust no shadow cache, which is the
	// whole argument for this function being outside the ShadowUpdateHz gate.
	//
	// WHY THE ASSET DEFAULT CANNOT BE THE KNOB: this write kills it. Editing
	// MPC_VoxelSky.StarAmbientGain does nothing once this line exists -- exactly
	// the correction recorded above StarBrightnessForSunAltitude, and the reason
	// voxel.Sky.StarAmbientGain exists as a cvar at all.
	bool bStarAmbientDerived = false;
	const float StarAmbientGain = VoxelSky::ResolveStarAmbientGain(bStarAmbientDerived);
	UKismetMaterialLibrary::SetScalarParameterValue(
		World, Collection, TEXT("StarAmbientGain"), StarAmbientGain);

	// Logged on TRANSITION only, and logged at all because S2's whole gate is
	// "did the ambient term move, and by how much" -- a capture has to be able to
	// state the gain it was shot at from its own log rather than from the command
	// line that was supposed to have been used.
	//
	// THE LOG STATES THE RESOLVED GAIN AND THE MODE, NOT THE REQUESTED VALUE. With
	// a sentinel default the requested value is -1 on every shipped run, so a log
	// that echoed the cvar would say "-1" for the frames that rendered at 1.0 --
	// which is not merely unhelpful, it is the one reading that would make someone
	// conclude starlight was off. The mode is latched separately because an explicit
	// `voxel.Sky.StarAmbientGain 1.0` resolves to the same number the default
	// derives, and a pinned run has to be able to say it was pinned.
	if (!FMath::IsNearlyEqual(StarAmbientGain, Impl->AppliedStarAmbientGain, 1.e-6f) ||
		Impl->AppliedStarAmbientGainDerived != (bStarAmbientDerived ? 1 : 0))
	{
		Impl->AppliedStarAmbientGain = StarAmbientGain;
		Impl->AppliedStarAmbientGainDerived = bStarAmbientDerived ? 1 : 0;
		UE_LOG(LogVoxelSky, Log,
		       TEXT("VoxelSky StarAmbientGain RESOLVED to %.4f, %s (voxel.Sky.StarAmbientGain requested %.4f; ")
		       TEXT("written into %s). %s This is the SkyLight capture's star gain ONLY -- it cannot change a ")
		       TEXT("main-view pixel, because M_SkyAtmosphereDome routes it through a ")
		       TEXT("ReflectionCapturePassSwitch. It is the RATIO of capture star radiance to visible star ")
		       TEXT("radiance: voxel.Sky.StarGain (%.4f here) is already inside the shared StarBrightness ")
		       TEXT("scalar both domes multiply, so this number carries none of it. 0 removes starlight from ")
		       TEXT("the ambient term entirely. Re-measure with tools/sky-albedo-check.py on a starlit ")
		       TEXT("capture: ground/sky luminance must equal the ground's albedo."),
		       StarAmbientGain,
		       bStarAmbientDerived ? TEXT("DERIVED from kStarAmbientCalibration") : TEXT("OVERRIDDEN explicitly"),
		       CVarSkyStarAmbientGain.GetValueOnAnyThread(), kSkyCollectionPath,
		       bStarAmbientDerived
		           ? TEXT("Derived means appearance and illumination cannot drift apart unnoticed; the ")
		             TEXT("constant was measured at voxel.Sky.StarGain 0.15 against a snowfield whose ")
		             TEXT("ground/sky luminance ratio came out 0.790, i.e. snow's albedo.")
		           : TEXT("OVERRIDDEN: the calibration is NOT being enforced on this run, which is right ")
		             TEXT("for an A/B arm and wrong for a shipped capture. Set it to -1 to hand it back."),
		       FMath::Max(0.f, CVarSkyStarGain.GetValueOnAnyThread()));
	}
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
