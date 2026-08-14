#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelWeatherSubsystem.generated.h"

// ============================================================================
// THE WEATHER SUBSYSTEM, v0 -- WIND ONLY (docs/weather-system-v0.md).
// ============================================================================
//
// WHAT IT DOES, PLAINLY. Every frame it asks one question -- "which way is the
// air moving where the camera is standing, and how fast" -- and writes the
// answer where materials can read it. That is the whole of v0. Waves driven by
// wind are the thing that was actually asked for; this is the half of it that
// has to exist first, because until the world has wind there is nothing for a
// wave to be driven by.
//
// WHERE THE ANSWER COMES FROM. Not from here. The field itself is
// voxelcore/weather.h -- a pure integer function of (world seed, x, y, game
// clock) with no state of any kind. This class is the adapter: it finds the
// four arguments, converts the integer answer to the units a material wants,
// and pushes it into MPC_VoxelSky. Everything interesting about the field --
// what the four bands are, why the synoptic cell is 2 km, why the advection
// uses a fixed bearing -- is documented at length in that header, and this one
// deliberately does not repeat it.
//
// THAT SPLIT IS NOT TIDINESS. It is what makes the field inspectable without
// an editor (vxc_windprobe), testable in CI on three compilers
// (voxel-core/tests/test_weather.cpp), and bit-identical between two clients
// without anyone having to promise that two floating-point pipelines agree.
// docs/lighting-weather-plan.md's Part B gate -- "two clients at the same
// (seed, time, position) sample identical weather" -- is satisfied by
// construction rather than by testing.
//
// ----------------------------------------------------------------------------
// IT USES THE SKY'S CLOCK. THIS IS THE MOST IMPORTANT LINE IN THE FILE.
// ----------------------------------------------------------------------------
//
// The time argument is UVoxelSkySubsystem::GetSkyState().EpochSeconds, the
// same accumulator that drives the sun, converted to whole milliseconds. There
// is deliberately NO second clock here, and adding one later would be a
// serious mistake: the entire capture harness works by pinning the sky clock
// (-VoxelTimeOfDay, voxel.Sky.TimeScale=0) so that two screenshots can be
// differenced, and a wind that ran on its own accumulator would drift between
// the settle wait and the shutter and quietly invalidate every appearance A/B
// this project takes.
//
// The consequence, stated because it will surprise somebody: WITH
// voxel.Sky.Enabled AT 0 THE WIND FREEZES AT THE t=0 FIELD, not at whatever it
// was showing. That is not a bug and it is not arbitrary -- the sky subsystem
// zeroes its whole state struct when it is switched off (VoxelSkySubsystem.cpp
// Tick, the `!VoxelSky::IsEnabled()` branch), so EpochSeconds genuinely
// becomes 0, and 0 is a perfectly good moment to freeze at. It is logged once
// when it happens so that nobody spends an afternoon on it.
//
// ----------------------------------------------------------------------------
// HOW IT REACHES A MATERIAL, AND WHY NOT A SECOND COLLECTION
// ----------------------------------------------------------------------------
//
// Two parameters, written into /Game/Voxel/MPC_VoxelSky every frame:
//
//   WindVectorMS   (vector) R = velocity along world +X (NORTH), m/s
//                           G = velocity along world +Y (EAST),  m/s
//                           B = gust magnitude, m/s
//                           A = unused, zero
//   WindFieldValid (scalar) 1 while this subsystem is publishing, else 0
//
// IT WAS FIVE, AND THREE OF THEM WERE THE SAME NUMBER. The first draft published
// WindFlowDirection (a unit vector) alongside WindSpeedMps, WindSustainedMps and
// WindGustMps. Two of those scalars are the length of the velocity and the
// vector is its normalisation, so the same quantity was stored three ways --
// three things that can disagree, with nothing in a frame able to report it.
// One velocity vector cannot have a direction that is stale with respect to its
// own speed. The consumer (Tools/water_wave_graph.py) asked for this shape
// independently, for the same reason.
//
// GUST STAYS SEPARATE because it is NOT derivable from the velocity: it rides on
// top of the sustained wind, and a consumer may want it for spray or foam
// without moving the waves. It is a magnitude in m/s, not a multiplier.
//
// THE COMPONENT ORDER IS R=NORTH, G=EAST, AND THE FIRST VERSION HAD IT SWAPPED.
// (x=east, y=north) is the order of a map or a plot and the natural one to
// reach for; this engine's world axes are X = north, Y = east, Z = up
// (VoxelEphemeris.h:43-45). Swapping them reflects every bearing about the
// 45-degree diagonal, which produces a lake with entirely plausible waves
// travelling in a direction unrelated to the wind -- nothing on screen says the
// axes are wrong. See PublishWind in the .cpp.
//
// A SECOND COLLECTION WAS THE OBVIOUS ANSWER AND IS THE WRONG ONE.
// docs/water-architecture.md:207-215 already decided it: create_sky_material.py
// DELETES and recreates MPC_VoxelSky on every run, and any material still
// holding a binding to the old object compiles to UE's DEFAULT MATERIAL while
// the log reports success -- the 2026-08-10 failure, where all water in the
// world drew with the default material and nothing said so. A second
// collection is a second instance of that hazard, and the standing decision is
// not to create one until something genuinely needs a runtime CPU-to-material
// channel that the sky collection cannot carry. Wind does not; it is two
// numbers a frame.
//
// WHAT THAT COSTS, AND IT IS NOT NOTHING: adding these names requires editing
// create_sky_material.py's SCALAR_PARAMS/VECTOR_PARAMS tables and then re-running
// the FULL mandated chain in order (sky, atmosphere dome, water) via
// tools/voxel-water-star-regen.ps1. Until that has been done, the writes below
// land on names the collection does not have -- which does NOT fail, does not
// warn, and does not log (MaterialExpressions.cpp:17179-17193). So this
// subsystem checks, once, that both names exist and says so loudly if they do
// not. See ResolveCollection in the .cpp; that check is the only thing standing
// between "the wind is not reaching the water" and a week of looking at the
// wave code.
//
// WHAT CONSUMES THIS: Tools/water_wave_graph.py, the wind-driven wave field. It
// reads WindVectorMS to steer and size the waves and gates on WindFieldValid, so
// a run with this subsystem disabled -- or a build where the sky chain has not
// been re-run -- falls back to its own constants and reproduces the pre-wind
// lake rather than going glassy.
//
// An earlier plan wired WaveDirBaseDeg and WaveAmplitudeM directly to the MPC
// instead. That is SUPERSEDED: water_wave_graph.py subsumes both, deriving
// amplitude and wavelength from wind speed through fetch-limited JONSWAP rather
// than lerping a single authored amplitude. Do not apply both.
//
// ----------------------------------------------------------------------------
// SCOPE, AND WHAT IT IS NOT
// ----------------------------------------------------------------------------
//
// CLIENT-SIDE RENDERING ONLY, OUTSIDE THE DETERMINISM BOUNDARY -- the same
// declaration VoxelSkySubsystem.h:18-24 and VoxelGI.h:26-30 make, and for the
// same reason. This subsystem reads a clock and writes uniforms. It never
// calls worldgen, never touches the edit log, and nothing it produces is
// replicated or digested. It also, by the standing decision at
// docs/lighting-weather-plan.md:1074-1078, must never push anything simulated:
// no physics, no debris, no projectiles.
//
// NOT REPLICATED, and it does not need to be. Two clients that agree on the
// seed and the clock compute identical wind independently -- which is strictly
// better than replicating it, since it costs no bandwidth and cannot desync.
// The clock is the only thing that has to travel, and VoxelSkySubsystem.h:46-55
// already owns that TODO.
//
// PIMPL FROM THE START, for the reason VoxelSkySubsystem.h:150-155 gives:
// today's impl is a WindParams and a couple of scalars, but the impl grows the
// moment precipitation or cloud cover lands, and retrofitting a PImpl onto a
// shipped UHT header is churn that buries whatever real change it rides with.
// The immediate reason is harder than that, though: WindParams is a
// voxelcore/weather.h type and voxel-core must not be visible to UHT.
// ============================================================================

struct FVoxelWeatherImpl;

// One evaluation of the wind field, in the units a reader thinks in.
//
// PLAIN STRUCT, NOT USTRUCT, AND SCALAR MEMBERS ONLY -- the doctrine
// VoxelSkySubsystem.h:67-73 and VoxelWaterSubsystem.h:44-47 state for their
// report PODs. No voxel-core type crosses this header: vxc::WindSample is an
// integer struct in millimetres per second and milli-degrees, and putting it
// in a UHT-parsed translation unit would drag voxel-core in for no benefit to
// anyone, since every consumer here wants metres per second and degrees.
struct FVoxelWindSample
{
	// --- the vector ----------------------------------------------------------
	// The direction the air TRAVELS, +X east and +Y north (the world's own axes,
	// as VoxelEphemeris.cpp:289 fixes them), in metres per second.
	double EastMps = 0.0;
	double NorthMps = 0.0;

	// --- the scalars ---------------------------------------------------------
	// THE AUTHORITY ON SPEED. Do not recompute it from the vector: the field's
	// unit direction comes from an interpolated 32-point table and is between
	// 0.99514 and 1.00001 of unit length, so the vector's own magnitude is up
	// to half a percent low. Cheap to get wrong and impossible to see.
	double SpeedMps = 0.0;
	// SpeedMps == SustainedMps + GustMps, exactly. Gust is signed.
	double SustainedMps = 0.0;
	double GustMps = 0.0;

	// --- the bearings --------------------------------------------------------
	// THE TWO ARE 180 DEGREES APART AND BOTH ARE HERE ON PURPOSE. Compass
	// convention: 0 is north, 90 is east.
	//
	// FromBearingDeg is where the air COMES FROM. It is what a human means by
	// "a westerly", what every weather report on Earth quotes, and the one to
	// print in a log or a HUD.
	//
	// ToBearingDeg is where it GOES. It is the one the vector points along and
	// the one a wave direction wants.
	//
	// Getting these the wrong way round produces waves marching into the wind
	// on a scene that otherwise looks entirely correct -- the same class of
	// silent sign error VoxelSkySubsystem.cpp:3108-3133 documents at length for
	// the sun vector.
	double FromBearingDeg = 0.0;
	double ToBearingDeg = 0.0;

	// The unit direction the vector was built from, so a consumer that wants to
	// scale it by something of its own does not divide the velocity by the speed
	// and rediscover the half-percent ripple.
	double DirEast = 0.0;
	double DirNorth = 0.0;
};

// What the frame actually used. A READ-ONLY REPORT: nothing consults it to
// decide anything. It exists so a HUD, a log line or a capture leg can state
// what happened rather than what was asked for -- VoxelGpuVerify.cpp:2118-2126's
// rule, applied to weather.
struct FVoxelWeatherState
{
	// The clock this sample was taken at. Copied from
	// UVoxelSkySubsystem::GetSkyState().EpochSeconds, never accumulated here.
	double EpochSeconds = 0.0;
	// Where the published sample was taken, in Unreal units. The camera, or the
	// world origin when there is no pawn yet.
	double SampleXUU = 0.0;
	double SampleYUU = 0.0;
	// True when SampleXUU/Y came from a real camera rather than the origin
	// fallback. A run whose whole log says false is a run whose wind is the
	// wind at (0,0), which is a fine wind and not the player's.
	bool bHasCamera = false;

	FVoxelWindSample Wind;

	// --- what is being held ---------------------------------------------------
	bool bBearingPinned = false;
	bool bSpeedPinned = false;

	// --- ran-flags -------------------------------------------------------------
	//
	// THE PROJECT RULE (docs/water-architecture.md:143): every stage must write
	// something that distinguishes "ran and found nothing" from "did not run".
	// A wind of 0 m/s from due north is a completely plausible reading and is
	// also exactly what a subsystem that never ticked reports, so the counter
	// below is what tells them apart. A HUD or a leg summary quoting the wind
	// without quoting this is quoting an unverified number.
	int64 Publishes = 0;
	// True once all five MPC parameters were found in the collection. FALSE IS
	// THE INTERESTING CASE: it means create_sky_material.py has not been re-run
	// since the wind parameters were added, every write below is going nowhere,
	// and no material can see the wind. Logged as an Error, once.
	bool bMaterialBound = false;
	// How many of the five names were missing, so the log can say "three of
	// five" rather than just "some".
	int32 MissingParamCount = 0;
};

UCLASS()
class VOXELEARTH_API UVoxelWeatherSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UVoxelWeatherSubsystem();
	// Declared (not defaulted) here and defined in the .cpp:
	// TUniquePtr<FVoxelWeatherImpl>'s destructor needs the full definition of a
	// struct that holds a vxc::WindParams, which this UHT-parsed header must not
	// see. Identical reasoning to VoxelSkySubsystem.h:141-145 and
	// VoxelWorldSubsystem.h:64-73.
	virtual ~UVoxelWeatherSubsystem() override;
	// UHT auto-generates this hot-reload constructor unless one is declared, and
	// the generated version lives in VoxelWeatherSubsystem.gen.cpp, which cannot
	// see FVoxelWeatherImpl either.
	UVoxelWeatherSubsystem(FVTableHelper& Helper);

	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

	//~ Begin FTickableGameObject / UTickableWorldSubsystem Interface
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject / UTickableWorldSubsystem Interface

	// The last published frame. Safe at any time -- returns a zeroed state
	// before the first tick and when Impl is null -- and allocates nothing.
	const FVoxelWeatherState& GetWeatherState() const;

	// --- THE QUERY API -------------------------------------------------------
	//
	// The one thing every future consumer needs: the wind somewhere else.
	// Precipitation drift wants it at the particle, a sail wants it at the boat,
	// a wind-shaped tree wants it at the trunk, and a wind info texture (v1)
	// wants it at 65,536 points a refresh.
	//
	// CHEAP AND STATELESS. Six noise evaluations, no allocation, no lock, no
	// cache to invalidate, and safe to call at any point in the frame including
	// before the first Tick. Two callers asking for the same (x, y, t) get the
	// same answer to the last bit.
	//
	// IT DOES NOT SAMPLE WHAT WAS PUBLISHED. GetWeatherState().Wind is the
	// camera's wind, which is what materials see; this is the field. At the
	// camera's own position and the frame's own clock the two agree exactly.

	// At the CURRENT frame's clock.
	FVoxelWindSample SampleWindAtWorldUU(double XUU, double YUU) const;

	// At an arbitrary moment of the sky clock, in game seconds since world
	// start. Exists for the harness (what will the wind be at the shutter?) and
	// for anything that needs to look ahead or behind. Passing the current
	// epoch reproduces SampleWindAtWorldUU exactly.
	FVoxelWindSample SampleWindAtWorldUUAtEpoch(double XUU, double YUU, double EpochSeconds) const;

private:
	TUniquePtr<FVoxelWeatherImpl> Impl;

	// Mirrors UVoxelSkySubsystem::bHasState and UVoxelGISubsystem::bHasState
	// (VoxelGI.cpp:349-354): true from the moment we have published anything,
	// cleared once WindFieldValid has been put back to 0 after a runtime
	// toggle-off. It is what keeps IsTickable true for exactly the one frame
	// needed to undo the feature, and no longer.
	bool bHasState = false;

	// Resolves MPC_VoxelSky once and verifies that all five wind parameters
	// exist in it. Returns null (having logged, once) if either fails.
	class UMaterialParameterCollection* ResolveCollection();
	// Pushes the frame's sample into the collection.
	void PublishWind(const FVoxelWindSample& Wind);
	// Drops WindFieldValid to 0 so every consumer falls back to its own
	// constant. A stale wind left published over a world that has switched the
	// feature off is worse than no wind at all -- it is a wind that will never
	// change again, which reads as a frozen simulation.
	void PublishInvalid();

	// The camera's XY in Unreal units, or false when there is no view to follow.
	bool GetCameraXY(double& OutX, double& OutY) const;
};

// Cvar accessors.
//
// Free functions rather than exported cvar objects, following the rule
// VoxelSkySubsystem.h:254-259 states: a TAutoConsoleVariable other files can
// reach is a value four files clamp four different ways. Clamping happens once,
// here, and every caller gets the clamped answer. Placed after the UCLASS,
// matching VoxelGI.h:385 and VoxelSkySubsystem.h:266 -- UHT is happiest with
// everything non-reflected out of its way.
namespace VoxelWeather
{
	VOXELEARTH_API bool IsEnabled();

	// The prevailing quarter the wind comes FROM, degrees, wrapped to [0, 360).
	VOXELEARTH_API double GetBaseFromBearingDeg();
	// Mean sustained speed before the synoptic multiplier, m/s. Floored at 0.
	VOXELEARTH_API double GetBaseSpeedMps();
	// Multiplier on the base speed. The knob for "make the whole world windier"
	// without moving the number the base is justified against.
	// Seconds. The low-pass between the wind field and the wind a material sees
	// -- waves have inertia; see the cvar's own note in the .cpp.
	VOXELEARTH_API float GetWaveResponseSeconds();

	VOXELEARTH_API double GetWindScale();
	// Multiplier on the gust band only. 0 gives a perfectly steady wind that is
	// still varying in space and over the slower bands -- which is what a
	// capture usually wants, as against a full pin which also freezes those.
	VOXELEARTH_API double GetGustScale();
	// Multiplier on the SPATIAL lattices (synoptic and gust together), so
	// weather cells get bigger or smaller without changing how fast they evolve.
	// Clamped to a range in which the field's integer arithmetic is still known
	// to be sound; see the cvar.
	VOXELEARTH_API double GetFieldScale();

	// --- the pins -------------------------------------------------------------
	//
	// NEGATIVE MEANS DERIVE, in both cases, and the two are independent: a run
	// may pin the direction and let the speed live, or the reverse.
	//
	// The bearing's sentinel is BELOW -360 rather than the usual -1, because -1
	// is a legal bearing (359 degrees) and a sentinel that collides with a
	// legal value is a bug waiting for the first person who types a negative
	// angle. Anything from -360 to 720 is accepted and wrapped.
	VOXELEARTH_API double GetPinFromBearingDeg(); // < -360 == derive
	VOXELEARTH_API double GetPinSpeedMps();       // < 0 == derive
	VOXELEARTH_API bool IsBearingPinned();
	VOXELEARTH_API bool IsSpeedPinned();

	// Seconds between "the wind is currently ..." log lines. 0 = never, the
	// default. Not a debug-only knob: it is how a headless leg records what
	// conditions its frames were taken in.
	VOXELEARTH_API double GetLogIntervalSeconds();
}
