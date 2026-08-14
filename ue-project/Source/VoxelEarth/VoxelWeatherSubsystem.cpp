#include "VoxelWeatherSubsystem.h"

#include "VoxelSkySubsystem.h"   // the clock. There is no other clock; see the header.
#include "VoxelWorldSubsystem.h" // the seed

#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/StrongObjectPtr.h" // FVoxelWeatherImpl is a plain struct; see Params

// voxel-core. Legal HERE and not in the header: this translation unit is not
// UHT-parsed. Same arrangement as VoxelBathyField.cpp:14.
#include "voxelcore/weather.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelWeather, Log, All);

// --- cvars -------------------------------------------------------------------
//
// voxel.<Area>.<Name>, units in the name, ECVF_Default, and an accessor free
// function per knob that owns the clamp -- this module's convention throughout
// (VoxelSkySubsystem.h:254-259 states the rule and why).
//
// THE NAMES ARE NOT INVENTED HERE WHERE A PLAN ALREADY NAMED THEM.
// docs/lighting-weather-plan.md:965-966 pre-registered
// `voxel.Weather.Enabled`, `voxel.Weather.WindScale`,
// `voxel.Weather.FieldScaleM` and `voxel.Weather.Override`. The first three are
// implemented below under those names. THE FOURTH IS DELIBERATELY LEFT
// UNIMPLEMENTED: `Override` was specified there as a PRESET selector
// (clear/overcast/rain/snow/storm), v0 has no presets, and quietly repurposing
// a reserved name to mean "pin the wind" is how a plan and a build stop
// describing the same thing. The pins below get their own names.
namespace
{
	TAutoConsoleVariable<int32> CVarWeatherEnabled(
		TEXT("voxel.Weather.Enabled"), 1,
		TEXT("The wind field drives materials. 1 = on (default). 0 = off, and genuinely zero ")
		TEXT("per-frame cost: UVoxelWeatherSubsystem::IsTickable goes false after one frame of ")
		TEXT("undo, and that undo drops MPC_VoxelSky.WindFieldValid to 0 so every consumer falls ")
		TEXT("back to the constant it used before wind existed. So this is a clean A/B on 'how ")
		TEXT("much of the water's look is the wind', not a switch that makes the water ")
		TEXT("disappear. CLIENT-SIDE RENDERING ONLY, outside the determinism boundary -- nothing ")
		TEXT("this subsystem does is replicated, digested or simulated."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWeatherBaseWindFromDeg(
		TEXT("voxel.Weather.BaseWindFromDeg"), 240.0f,
		TEXT("The PREVAILING quarter the wind comes FROM, degrees clockwise from north (so 90 is ")
		TEXT("an easterly, i.e. blowing towards the west). Default 240 = west-south-west, and it ")
		TEXT("is chosen against the same evidence voxel.Sky.OriginLatitudeDeg's 52.0 is: ")
		TEXT("VoxelClimateProbe measures this world as cool-temperate maritime, which is the ")
		TEXT("northern-European westerly belt, and a world whose biomes say maritime while its ")
		TEXT("wind comes off the east reads as a worldgen fault even though neither half is ")
		TEXT("wrong alone. THIS ALSO SETS WHICH WAY WEATHER TRAVELS -- the synoptic pattern is ")
		TEXT("advected along this bearing -- so changing it moves the systems as well as the ")
		TEXT("breeze. The instantaneous wind wanders a long way either side of this (the regime ")
		TEXT("band alone is +/-180 deg over about three game days), so on any given afternoon ")
		TEXT("the wind will NOT be from 240; this is the centre of the distribution, not the ")
		TEXT("wind."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWeatherBaseWindMps(
		TEXT("voxel.Weather.BaseWindMps"), 6.0f,
		TEXT("Mean sustained wind speed before the synoptic multiplier, metres per second. ")
		TEXT("Default 6.0 = Beaufort 4, a 'moderate breeze': enough to raise whitecaps on a lake ")
		TEXT("and not enough to look like a storm. The realised distribution at the defaults is ")
		TEXT("p05 3.3, median 5.9, p95 8.9 m/s (measured with vxc_windprobe, seed 20260719). ")
		TEXT("A STARTING GUESS, NOT A MEASUREMENT: nobody has yet looked at water driven by this ")
		TEXT("field and said whether it moves right, and when they do, this is the first knob to ")
		TEXT("turn. Prefer voxel.Weather.WindScale for a temporary 'windier please' -- this ")
		TEXT("number carries an argument and that one does not."),
		ECVF_Default);

	// HOW LONG THE SEA TAKES TO NOTICE THE WIND, in seconds. This is the knob for
	// the owner's 2026-08-13 report: "the wave effect is way way too fast, it
	// looks like someone is playing a video at multiple times speed, and the wind
	// effect on water is jittery and moving too fast."
	//
	// The cause was not the wave animation -- with voxel.Weather.Enabled 0 the
	// material uses a STATIC fallback wind and the same waves were judged "quite
	// good". It is that wind speed sets WAVELENGTH as well as height
	// (water_wave_graph.py, U^0.68), so a wind that moves every frame slides
	// every crest every frame, and the eye reads that as the scene running fast.
	//
	// 45 -> 8 s on 2026-08-14, and the direction of that change is the opposite
	// of what three rounds of guessing suggested. A LONGER constant is WORSE, not
	// better: it converts every wind change into a longer smooth drift, and a
	// coherent drift slides every crest (wind speed sets wavelength) far more
	// visibly than the gust twitch it was suppressing. The owner reported the
	// water as "way way too fast" at 0 AND at 45, and had to reach 5000 -- an
	// effectively frozen wind -- before it looked right, which in hindsight is
	// the measurement saying "any drift at all is the problem".
	//
	// 8 s still removes the gust band (now 90 s, so it was never the issue) and
	// settles fast enough that a change is over rather than ongoing. The wind
	// field itself was slowed 8-15x in the same session, so there is very little
	// left for this to smooth.
	//
	// 0 disables smoothing entirely. Pins bypass it regardless -- see Tick.
	TAutoConsoleVariable<float> CVarWeatherWaveResponseSeconds(
		TEXT("voxel.Weather.WaveResponseSeconds"), 8.0f,
		TEXT("Time constant, in seconds, of the low-pass between the wind field and the wind that ")
		TEXT("reaches water materials. Waves have inertia: a real sea takes minutes to build to a new ")
		TEXT("wind, and driving wave height AND wavelength from a 6-second gust band makes the whole ")
		TEXT("surface appear to run at several times speed. 0 = no smoothing (raw instantaneous wind). ")
		TEXT("Does not affect the logged or HUD wind, which stays the true instantaneous value."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWeatherWindScale(
		TEXT("voxel.Weather.WindScale"), 1.0f,
		TEXT("Multiplier on the whole wind speed. Default 1.0. The knob for 'make the world ")
		TEXT("windier' without moving voxel.Weather.BaseWindMps, which carries a justification. ")
		TEXT("Multiplies the sustained speed, so the gust -- which is a FRACTION of sustained -- ")
		TEXT("scales with it and the gust factor stays put. 0 gives a dead calm, which is a ")
		TEXT("legitimate and useful control arm: it is the 'no wind at all' end of every ")
		TEXT("wind-driven-waves comparison. Floored at 0; a negative wind speed would flip every ")
		TEXT("direction in the world."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWeatherGustScale(
		TEXT("voxel.Weather.GustScale"), 1.0f,
		TEXT("Multiplier on the GUST band alone -- the fast, small component (about 180 m and 6 s ")
		TEXT("across) that rides on top of the sustained wind. Default 1.0, giving a gust factor ")
		TEXT("of 1.25, the low end of what real anemometers see over open water. 0 gives a wind ")
		TEXT("that is perfectly steady moment to moment while still varying from place to place ")
		TEXT("and over the slower bands. THAT IS USUALLY WHAT A CAPTURE WANTS and it is a ")
		TEXT("different thing from voxel.Weather.PinMps: this removes the twitch, the pin removes ")
		TEXT("the weather. Floored at 0."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWeatherFieldScaleM(
		TEXT("voxel.Weather.FieldScaleM"), 1.0f,
		TEXT("Multiplier on the field's SPATIAL scales -- the 2048 m synoptic cell and the 180 m ")
		TEXT("gust cell together. Default 1.0. Bigger makes weather cells wider, so the wind ")
		TEXT("changes less as you travel and more as you wait; smaller does the reverse. It does ")
		TEXT("NOT change how fast the field evolves in time, which is deliberate: those are two ")
		TEXT("questions and conflating them is how a single 'scale' knob ends up meaning nothing. ")
		TEXT("The 2048 m default is the one number in this whole system that is not a guess -- ")
		TEXT("docs/lighting-weather-plan.md:400 fixed it so a front crosses the visible world in ")
		TEXT("minutes of play. CLAMPED to 0.05..64: below that the lattice rounds toward zero and ")
		TEXT("the field degenerates, above it the integer arithmetic in voxelcore/weather.h leaves ")
		TEXT("its proved-sound domain."),
		ECVF_Default);

	// --- the pins ------------------------------------------------------------
	//
	// THE SENTINEL FOR THE BEARING IS -1000, NOT -1, AND THAT IS NOT FUSSINESS.
	// -1 is a perfectly legal bearing (it wraps to 359 degrees), so a -1
	// sentinel would silently swallow the first person who types a negative
	// angle. The test is `< -360`, which no wrapped bearing can satisfy.
	//
	// Both are spelled with the comparison the right way round for NaN --
	// `!(x >= 0)` rather than `(x < 0)` -- so a NaN typed into the console
	// derives rather than poisoning the MPC with a NaN wind vector. Same care
	// as ResolveStarAmbientGain (VoxelSkySubsystem.cpp:781-800).
	TAutoConsoleVariable<float> CVarWeatherPinFromDeg(
		TEXT("voxel.Weather.PinFromDeg"), -1000.0f,
		TEXT("PIN the direction the wind comes FROM, degrees clockwise from north. Default -1000 ")
		TEXT("= derive from the field. Any value from -360 to 720 pins and is wrapped; the ")
		TEXT("sentinel is -1000 rather than -1 because -1 is a legal bearing. A pinned bearing ")
		TEXT("is EXACTLY the value asked for at every place and every moment, which is what lets ")
		TEXT("two screenshots be differenced -- and it pins the ADVECTION direction too, so the ")
		TEXT("weather cells stop wandering as well. Also settable before the first frame with ")
		TEXT("-VoxelWindFromDeg=<deg>, which is what a capture leg should use: -ExecCmds lands ")
		TEXT("AFTER subsystem initialization and would leave the first frames on the derived ")
		TEXT("wind (VoxelSkySubsystem.cpp:1805-1813 records that lesson being learned three ")
		TEXT("times)."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWeatherPinMps(
		TEXT("voxel.Weather.PinMps"), -1.0f,
		TEXT("PIN the wind speed, metres per second. Default -1 = derive from the field (a ")
		TEXT("negative speed is meaningless, so -1 is a safe sentinel here in a way it is not ")
		TEXT("for the bearing). 0 IS A LEGAL PIN and means dead calm. Pinning the speed removes ")
		TEXT("the gust entirely -- that is the point of it; use voxel.Weather.GustScale 0 if you ")
		TEXT("want a steady wind that still varies from place to place. Also settable before the ")
		TEXT("first frame with -VoxelWindMps=<m/s>. NOTE that pinning the wind is NOT how you ")
		TEXT("make a capture reproducible: pinning the CLOCK already does that, because the wind ")
		TEXT("is a pure function of it. This exists for the other job -- holding the wind still ")
		TEXT("while something else changes."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWeatherLogIntervalSeconds(
		TEXT("voxel.Weather.LogIntervalSeconds"), 0.0f,
		TEXT("Seconds of real time between 'wind is currently ...' log lines. Default 0 = never. ")
		TEXT("NOT A DEBUG-ONLY KNOB: it is how a headless leg records the conditions its frames ")
		TEXT("were taken in, which this project needs because a screenshot cannot be re-read for ")
		TEXT("its wind speed afterwards. The line reports what was USED (including whether a pin ")
		TEXT("held), never what was asked for -- VoxelGpuVerify.cpp:2118-2126's rule."),
		ECVF_Default);

	// --- MPC parameter names --------------------------------------------------
	//
	// NAMED ONCE, HERE, for the reason VoxelSkySubsystem.cpp:1220-1224 gives for
	// its own path constant: an MPC parameter name that does not resolve DOES
	// NOT FAIL. It is not a compile error, not a warning and not a log line --
	// UMaterialExpressionCollectionParameter::Compile falls through to emitting
	// a CONSTANT when the lookup misses (MaterialExpressions.cpp:17179-17193).
	// A typo here writes into nothing at all and leaves the asset default
	// standing forever, which looks exactly like a wind system that works and a
	// water material that ignores it.
	//
	// Both names must match create_sky_material.py's SCALAR_PARAMS and
	// VECTOR_PARAMS tables exactly. ResolveCollection below checks that they do,
	// at runtime, and logs an Error naming each missing one -- which is the one
	// thing that turns the silent failure above into a log line.
	const TCHAR* kSkyCollectionPath = TEXT("/Game/Voxel/MPC_VoxelSky.MPC_VoxelSky");
	// ONE VELOCITY VECTOR, NOT A DIRECTION PLUS A SPEED.
	//
	// This started as five parameters -- WindFlowDirection, WindSpeedMps,
	// WindSustainedMps, WindGustMps, WindFieldValid -- and was reconciled down
	// to two when the consumer landed. Three of the five were REDUNDANT:
	// WindSpeedMps and WindSustainedMps are both just the length of the
	// velocity, and WindFlowDirection is its normalisation. A representation
	// that stores the same quantity three ways is three things that can
	// disagree, and nothing in a frame would report it if they did.
	//
	// The consumer (Tools/water_wave_graph.py, WIND_VECTOR_PARAM) asked for
	// exactly this shape and for exactly that reason.
	//
	// LAYOUT, and the component order is the part to be careful about --
	// see PublishWind for the bug this cost:
	//     R = velocity along world +X (NORTH), m/s
	//     G = velocity along world +Y (EAST),  m/s
	//     B = gust magnitude, m/s -- kept because it is NOT derivable from the
	//         velocity: the gust rides on top of the sustained wind and a
	//         consumer may want it for spray or foam without moving the waves.
	//     A = unused, zero.
	const FName kParamWindVector(TEXT("WindVectorMS"));
	const FName kParamValid(TEXT("WindFieldValid"));

	double WrapBearingDeg(double Deg)
	{
		const double W = FMath::Fmod(Deg, 360.0);
		return W < 0.0 ? W + 360.0 : W;
	}
} // namespace

namespace VoxelWeather
{
	bool IsEnabled() { return CVarWeatherEnabled.GetValueOnAnyThread() != 0; }

	double GetBaseFromBearingDeg()
	{
		return WrapBearingDeg((double)CVarWeatherBaseWindFromDeg.GetValueOnAnyThread());
	}

	double GetBaseSpeedMps()
	{
		// Floored, not clamped to a default. A negative base speed would come
		// back out as a wind blowing the opposite way at the same strength,
		// because the speed multiplies a unit direction -- a sign error that
		// looks like a direction bug and would be searched for in the rose.
		return FMath::Max(0.0, (double)CVarWeatherBaseWindMps.GetValueOnAnyThread());
	}

	float GetWaveResponseSeconds()
	{
		return FMath::Max(0.0f, CVarWeatherWaveResponseSeconds.GetValueOnAnyThread());
	}

	double GetWindScale() { return FMath::Max(0.0, (double)CVarWeatherWindScale.GetValueOnAnyThread()); }
	double GetGustScale() { return FMath::Max(0.0, (double)CVarWeatherGustScale.GetValueOnAnyThread()); }

	double GetFieldScale()
	{
		// Clamped at both ends and both ends are failure modes, not taste.
		// Below ~0.05 the gust lattice (180 m nominal) rounds down toward a
		// handful of millimetres and the noise degenerates into hash; above 64
		// the synoptic lattice passes 131 km and leaves the domain
		// voxelcore/weather.h's overflow budget was proved over.
		return FMath::Clamp((double)CVarWeatherFieldScaleM.GetValueOnAnyThread(), 0.05, 64.0);
	}

	double GetPinFromBearingDeg() { return (double)CVarWeatherPinFromDeg.GetValueOnAnyThread(); }
	double GetPinSpeedMps() { return (double)CVarWeatherPinMps.GetValueOnAnyThread(); }

	// RANGE TEST, NOT A SENTINEL TEST, and that is what makes it NaN-safe:
	// both comparisons are false for a NaN, so a NaN typed into the console
	// derives instead of pinning the whole world to a NaN wind vector. Writing
	// it as `V != -1000.0` or `V > -1000.0` would both pin on NaN. Same care as
	// ResolveStarAmbientGain (VoxelSkySubsystem.cpp:781-800).
	//
	// Out-of-range values (below -360 or above 720) derive rather than clamp,
	// which is what the cvar's help says. A clamp would turn a fat-fingered
	// 2400 into a silent pin at some arbitrary bearing.
	bool IsBearingPinned()
	{
		const double V = GetPinFromBearingDeg();
		return V >= -360.0 && V <= 720.0;
	}
	bool IsSpeedPinned() { return GetPinSpeedMps() >= 0.0; }

	double GetLogIntervalSeconds()
	{
		return FMath::Max(0.0, (double)CVarWeatherLogIntervalSeconds.GetValueOnAnyThread());
	}
} // namespace VoxelWeather

// --- the impl ----------------------------------------------------------------

struct FVoxelWeatherImpl
{
	FVoxelWeatherState State;

	// The world seed, latched once in Initialize. LATCHED RATHER THAN READ PER
	// FRAME because UVoxelWorldSubsystem::GetSeed is itself resolved once and
	// never changes, and because a wind whose seed could change mid-run would
	// teleport the whole weather pattern.
	uint64 Seed = 0;

	// Resolved once. A TStrongObjectPtr rather than a raw pointer because this
	// struct is invisible to the GC (VoxelSkySubsystem.cpp:1697-1710 makes the
	// same call for the same reason), and released in Deinitialize.
	TStrongObjectPtr<UMaterialParameterCollection> Collection;
	bool bCollectionLoadAttempted = false;

	// The sea's response to the wind, in m/s along world +X (north) and +Y
	// (east). NOT the wind -- see the wave-inertia note in Tick for why the
	// instantaneous wind must not reach a material. Seeded from the first real
	// sample rather than from zero, which is what bSmoothedWindValid tracks.
	double SmoothedNorthMS = 0.0;
	double SmoothedEastMS = 0.0;
	bool bSmoothedWindValid = false;

	// Real seconds since the last periodic log line.
	double SecondsSinceLog = 0.0;
	// So the "the sky clock is off, the wind is frozen at t=0" explanation is
	// printed once rather than every frame.
	bool bLoggedClockOff = false;
	bool bLoggedNoSky = false;
};

UVoxelWeatherSubsystem::UVoxelWeatherSubsystem() = default;
UVoxelWeatherSubsystem::~UVoxelWeatherSubsystem() = default;
UVoxelWeatherSubsystem::UVoxelWeatherSubsystem(FVTableHelper& Helper) : Super(Helper) {}

bool UVoxelWeatherSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Game and PIE only, matching UVoxelBathyFieldSubsystem (VoxelBathyField.cpp:121-127)
	// rather than UVoxelSkySubsystem, and the split is the one that file names:
	// the stateless subsystems include GamePreview, the ones that PUBLISH INTO A
	// SHARED ASSET do not. This one writes MPC_VoxelSky, which is process-global,
	// so a preview world publishing into it would fight the game world that is
	// also publishing into it and the loser would be whichever ticked first.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UVoxelWeatherSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelWeatherSubsystem, STATGROUP_Tickables);
}

bool UVoxelWeatherSubsystem::IsTickable() const
{
	// Zero per-frame cost when off, with bHasState keeping us alive for exactly
	// the one frame it takes to drop WindFieldValid back to 0. The same shape as
	// UVoxelSkySubsystem::IsTickable and UVoxelGISubsystem::IsTickable
	// (VoxelGI.cpp:349-354).
	return VoxelWeather::IsEnabled() || bHasState;
}

void UVoxelWeatherSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// ORDERING, DECLARED RATHER THAN HOPED FOR. Both dependencies are real:
	// the seed comes from the terrain subsystem and the clock from the sky. The
	// sky's own Initialize makes exactly this argument for depending on the
	// terrain (VoxelSkySubsystem.cpp:1780-1792), and it names weather as the
	// reason to establish the ordering early -- "retrofitting an ordering
	// guarantee later is how races get introduced".
	UVoxelWorldSubsystem* Terrain = Collection.InitializeDependency<UVoxelWorldSubsystem>();
	if (!Terrain)
	{
		UE_LOG(LogVoxelWeather, Error,
		       TEXT("UVoxelWeatherSubsystem::Initialize: no UVoxelWorldSubsystem, so there is no ")
		       TEXT("world seed to key the wind field on. The wind is DISABLED for this run and ")
		       TEXT("MPC_VoxelSky.WindFieldValid will stay 0, so every consumer falls back to its ")
		       TEXT("own constant."));
		return; // leaves Impl null; every method below null-checks
	}
	Collection.InitializeDependency<UVoxelSkySubsystem>();

	Impl = MakeUnique<FVoxelWeatherImpl>();
	Impl->Seed = Terrain->GetSeed();

	// --- command-line pins ---------------------------------------------------
	//
	// PARSED HERE, IN Initialize, AND NEVER VIA -ExecCmds. -ExecCmds lands AFTER
	// subsystem initialization, so a leg that pinned the wind that way would
	// spend its first frames on the derived field and could capture whichever
	// one the shutter happened to land in. This module has learned that three
	// times and written it down twice (VoxelGI.cpp:243-260,
	// VoxelWorldSubsystem.cpp:1597-1601, VoxelSkySubsystem.cpp:1805-1813).
	//
	// Set through the cvar rather than into a member, so that the console and
	// the command line stay one value with one clamp, and so `voxel.Weather.
	// PinFromDeg` typed at runtime reports the truth.
	float RequestedFromDeg = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelWindFromDeg="), RequestedFromDeg))
	{
		if (IConsoleVariable* Var = CVarWeatherPinFromDeg.AsVariable())
		{
			Var->Set(*FString::Printf(TEXT("%f"), RequestedFromDeg), ECVF_SetByCode);
		}
	}
	float RequestedMps = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelWindMps="), RequestedMps))
	{
		if (RequestedMps < 0.f)
		{
			UE_LOG(LogVoxelWeather, Warning,
			       TEXT("-VoxelWindMps=%f is negative, which is the sentinel for 'derive'. The ")
			       TEXT("wind speed is NOT pinned for this run."),
			       RequestedMps);
		}
		if (IConsoleVariable* Var = CVarWeatherPinMps.AsVariable())
		{
			Var->Set(*FString::Printf(TEXT("%f"), RequestedMps), ECVF_SetByCode);
		}
	}

	// RESOLVED, NOT REQUESTED. Every number below is read back out of the
	// accessor that owns its clamp, so this line states what the run will
	// actually do even if a cvar refused the write (ECVF_SetByCode loses to
	// ECVF_SetByConsole and ECVF_SetByCommandLine, so a request CAN be denied).
	//
	// The two pin strings are hoisted into named locals rather than built
	// inline in the UE_LOG argument list: a temporary FString dereferenced into
	// a %s argument is a lifetime question nobody should have to think about
	// while reading a log line.
	const FString PinFromText =
		VoxelWeather::IsBearingPinned()
			? FString::Printf(TEXT("%.1f deg"), WrapBearingDeg(VoxelWeather::GetPinFromBearingDeg()))
			: FString(TEXT("derived"));
	const FString PinSpeedText = VoxelWeather::IsSpeedPinned()
	                                 ? FString::Printf(TEXT("%.2f m/s"), VoxelWeather::GetPinSpeedMps())
	                                 : FString(TEXT("derived"));
	UE_LOG(LogVoxelWeather, Log,
	       TEXT("VoxelWeather RESOLVED: seed=%llu enabled=%d base=%.1f deg FROM at %.2f m/s ")
	       TEXT("windScale=%.2f gustScale=%.2f fieldScale=%.2f pinFrom=%s pinSpeed=%s"),
	       (unsigned long long)Impl->Seed, VoxelWeather::IsEnabled() ? 1 : 0,
	       VoxelWeather::GetBaseFromBearingDeg(), VoxelWeather::GetBaseSpeedMps(),
	       VoxelWeather::GetWindScale(), VoxelWeather::GetGustScale(),
	       VoxelWeather::GetFieldScale(), *PinFromText, *PinSpeedText);
}

void UVoxelWeatherSubsystem::Deinitialize()
{
	// Put the collection back before we go. A world that tears down leaving
	// WindFieldValid at 1 hands the next world a wind that will never update.
	PublishInvalid();
	bHasState = false;
	if (Impl)
	{
		Impl->Collection.Reset();
	}
	Impl.Reset();
	Super::Deinitialize();
}

// --- the field ---------------------------------------------------------------

namespace
{
	// Turn the cvars into the field's parameter block. Called per sample rather
	// than cached, because every one of these is a LIVE knob -- the whole point
	// of them is that somebody can turn one and see the water change without a
	// rebuild -- and the cost is a handful of reads against six noise
	// evaluations.
	vxc::WindParams BuildParams()
	{
		vxc::WindParams P{};

		P.baseFromBearingMilliDeg =
			(int32)FMath::RoundToInt(VoxelWeather::GetBaseFromBearingDeg() * 1000.0);

		const double SpeedMps = VoxelWeather::GetBaseSpeedMps() * VoxelWeather::GetWindScale();
		// Clamped to the field's own rail so that a silly cvar cannot produce a
		// speed the int32 cannot hold. 0 is legal and means dead calm.
		P.baseSpeedMmPerS =
			(int32)FMath::Clamp(FMath::RoundToDouble(SpeedMps * 1000.0), 0.0, (double)P.maxSpeedMmPerS);

		// The gust is a FRACTION of the sustained speed in the field, so scaling
		// it here scales the gust factor rather than adding an absolute wobble.
		P.gustFractionQ = (int32)FMath::Clamp(
			FMath::RoundToDouble((double)P.gustFractionQ * VoxelWeather::GetGustScale()), 0.0,
			(double)vxc::kWindQ);

		const double FieldScale = VoxelWeather::GetFieldScale();
		// SPATIAL lattices only. The temporal ones are deliberately untouched --
		// see the cvar's help for why those are a different question.
		P.synopticLatticeMm =
			FMath::Max((int64)1000, (int64)FMath::RoundToDouble((double)P.synopticLatticeMm * FieldScale));
		P.gustLatticeMm =
			FMath::Max((int64)1000, (int64)FMath::RoundToDouble((double)P.gustLatticeMm * FieldScale));

		if (VoxelWeather::IsBearingPinned())
		{
			// WRAPPED BEFORE IT CROSSES THE BOUNDARY. voxelcore/weather.h's
			// contract is that a pinned bearing arrives already inside
			// [0, 360000) because its own sentinel is "negative", so an unwrapped
			// -30 degrees would silently mean "derive" instead of "from the
			// north-north-west".
			P.pinFromBearingMilliDeg = (int32)FMath::Clamp(
				FMath::RoundToDouble(WrapBearingDeg(VoxelWeather::GetPinFromBearingDeg()) * 1000.0),
				0.0, 359999.0);
		}
		if (VoxelWeather::IsSpeedPinned())
		{
			P.pinSpeedMmPerS = (int32)FMath::Clamp(
				FMath::RoundToDouble(VoxelWeather::GetPinSpeedMps() * 1000.0), 0.0,
				(double)P.maxSpeedMmPerS);
		}
		return P;
	}

	FVoxelWindSample ToUE(const vxc::WindSample& W)
	{
		FVoxelWindSample Out;
		Out.EastMps = W.eastMmPerS / 1000.0;
		Out.NorthMps = W.northMmPerS / 1000.0;
		Out.SpeedMps = W.speedMmPerS / 1000.0;
		Out.SustainedMps = W.sustainedMmPerS / 1000.0;
		Out.GustMps = W.gustMmPerS / 1000.0;
		Out.FromBearingDeg = W.fromBearingMilliDeg / 1000.0;
		Out.ToBearingDeg = W.toBearingMilliDeg / 1000.0;
		Out.DirEast = W.dirEastQ / (double)vxc::kWindQ;
		Out.DirNorth = W.dirNorthQ / (double)vxc::kWindQ;
		return Out;
	}

	// UU are centimetres; the field is keyed in millimetres. Rounded rather than
	// truncated so that the two sides of zero behave the same -- a truncation
	// here would put a one-millimetre-wide seam through the world origin, which
	// is exactly where every capture fixture likes to stand.
	int64 UUToMm(double UU) { return (int64)FMath::RoundToDouble(UU * 10.0); }

	// Game seconds to whole milliseconds. THE CLOCK'S RESOLUTION FOR THE WIND IS
	// THEREFORE ONE MILLISECOND, which is far finer than any band in the field
	// (the fastest is a 6 s gust lattice) and is exactly reproducible: two runs
	// pinned to the same -VoxelTimeOfDay resolve the same epoch, hence the same
	// integer, hence bit-identical wind.
	int64 EpochToMs(double EpochSeconds) { return (int64)FMath::RoundToDouble(EpochSeconds * 1000.0); }
} // namespace

FVoxelWindSample UVoxelWeatherSubsystem::SampleWindAtWorldUUAtEpoch(double XUU, double YUU,
                                                                   double EpochSeconds) const
{
	if (!Impl)
	{
		return FVoxelWindSample();
	}
	const vxc::WindParams P = BuildParams();
	const vxc::WindSample W =
		vxc::sampleWind(Impl->Seed, UUToMm(XUU), UUToMm(YUU), EpochToMs(EpochSeconds), P);
	return ToUE(W);
}

FVoxelWindSample UVoxelWeatherSubsystem::SampleWindAtWorldUU(double XUU, double YUU) const
{
	// The FRAME's clock, not a fresh read of the sky -- so that everything
	// sampled during one frame agrees, including a caller who asks after the
	// sky has already ticked for the next one.
	const double Epoch = Impl ? Impl->State.EpochSeconds : 0.0;
	return SampleWindAtWorldUUAtEpoch(XUU, YUU, Epoch);
}

const FVoxelWeatherState& UVoxelWeatherSubsystem::GetWeatherState() const
{
	static const FVoxelWeatherState kEmpty;
	return Impl ? Impl->State : kEmpty;
}

// --- the material channel -----------------------------------------------------

UMaterialParameterCollection* UVoxelWeatherSubsystem::ResolveCollection()
{
	if (!Impl)
	{
		return nullptr;
	}
	if (!Impl->bCollectionLoadAttempted)
	{
		Impl->bCollectionLoadAttempted = true;
		// LoadObject here rather than in Initialize, for the reason
		// VoxelSkySubsystem.cpp:1697-1705 gives: /Game/ may not resolve through
		// the asset registry that early, and a failed LoadObject is a synchronous
		// package open that must not happen every frame either.
		Impl->Collection.Reset(LoadObject<UMaterialParameterCollection>(nullptr, kSkyCollectionPath));

		UMaterialParameterCollection* C = Impl->Collection.Get();
		if (!C)
		{
			UE_LOG(LogVoxelWeather, Error,
			       TEXT("%s did not load. Nothing this subsystem computes can reach a material ")
			       TEXT("without it, so the water will keep using its authored constant wave ")
			       TEXT("direction and amplitude and will look like a working feature. Run ")
			       TEXT("Tools/create_sky_material.py."),
			       kSkyCollectionPath);
			return nullptr;
		}

		// ==================================================================
		// THE CHECK THIS WHOLE SUBSYSTEM'S DEBUGGABILITY RESTS ON.
		//
		// UKismetMaterialLibrary::SetScalarParameterValue with a name the
		// collection does not have writes into NOTHING. No return value, no
		// warning, no log line -- and on the material side the mirror failure
		// is worse: an unresolved CollectionParameter compiles to a CONSTANT
		// (MaterialExpressions.cpp:17179-17193). So the visible result of
		// having added these names to this .cpp but not yet to
		// create_sky_material.py is: a wind system whose logs are perfect, a
		// water surface that never responds, and no evidence anywhere.
		//
		// Checking costs one scan of two small arrays, once per world.
		// ==================================================================
		const FName Scalars[] = {kParamValid};
		TArray<FString> Missing;
		for (const FName& N : Scalars)
		{
			const bool bFound = C->ScalarParameters.ContainsByPredicate(
				[&N](const FCollectionScalarParameter& P) { return P.ParameterName == N; });
			if (!bFound)
			{
				Missing.Add(N.ToString());
			}
		}
		const bool bHasVector = C->VectorParameters.ContainsByPredicate(
			[](const FCollectionVectorParameter& P) { return P.ParameterName == kParamWindVector; });
		if (!bHasVector)
		{
			Missing.Add(kParamWindVector.ToString());
		}

		Impl->State.MissingParamCount = Missing.Num();
		Impl->State.bMaterialBound = Missing.Num() == 0;

		if (Missing.Num() > 0)
		{
			// Hoisted for the same lifetime reason as the RESOLVED line's pin
			// strings: a temporary FString dereferenced straight into a %s is a
			// question nobody should have to answer while reading a log call.
			const FString MissingText = FString::Join(Missing, TEXT(", "));
			UE_LOG(LogVoxelWeather, Error,
			       TEXT("%s is missing %d of the 2 wind parameters (%s). EVERY WIND WRITE THIS ")
			       TEXT("RUN GOES NOWHERE and no material can see the wind -- and nothing else ")
			       TEXT("would have told you, because an MPC write to a name that does not exist ")
			       TEXT("is silent. FIX: add them to Tools/create_sky_material.py's ")
			       TEXT("SCALAR_PARAMS/VECTOR_PARAMS tables and re-run the FULL chain in order ")
			       TEXT("via tools/voxel-water-star-regen.ps1 (sky, then the atmosphere dome, ")
			       TEXT("then the water -- running the water alone leaves it bound to a ")
			       TEXT("collection that no longer exists in the form it expected, which is the ")
			       TEXT("2026-08-10 all-water-drew-with-the-default-material failure). The ")
			       TEXT("required entries are written out in docs/weather-system-v0.md."),
			       kSkyCollectionPath, Missing.Num(), *MissingText);
		}
		else
		{
			UE_LOG(LogVoxelWeather, Log,
			       TEXT("VoxelWeather bound to %s -- both wind parameters present. Driven every ")
			       TEXT("frame."),
			       kSkyCollectionPath);
		}
	}
	// Published even when parameters are missing: the writes are harmless
	// no-ops and stopping would only hide the fact that the rest of the
	// subsystem is working. The Error above is the diagnosis.
	return Impl->Collection.Get();
}

void UVoxelWeatherSubsystem::PublishWind(const FVoxelWindSample& Wind)
{
	UWorld* World = GetWorld();
	UMaterialParameterCollection* C = ResolveCollection();
	if (!World || !C)
	{
		return;
	}

	// THE DIRECTION THE AIR TRAVELS, not the direction it comes from. Materials
	// want the flow: a wave crest advances downwind, foam streaks downwind,
	// spray blows downwind. The human-facing "from" bearing stays on the CPU
	// side, in FVoxelWindSample and in the log line, and never crosses into a
	// shader -- which is the whole reason the two are separate fields with
	// separate names rather than one number and a convention to remember.
	//
	// Z IS ZERO AND MUST STAY ZERO. The field is horizontal; a material that
	// normalises this vector in 3D would otherwise be normalising against
	// whatever junk arrived in the third component.
	// R IS NORTH AND G IS EAST, AND THE FIRST VERSION OF THIS LINE HAD THEM THE
	// OTHER WAY ROUND. It published FLinearColor(DirEast, DirNorth, ...), which
	// is the (x=east, y=north) order of a map or a plot, and the natural one to
	// reach for. It is not this engine's. VoxelEphemeris.h:43-45 fixes the world
	// axes as X = NORTH, Y = east, Z = up, and a material reading a vector
	// parameter takes R/G/B as x/y/z -- so R must carry the north component.
	//
	// The failure this would have produced is nasty precisely because it is not
	// a crash or an obvious mess: swapping the two reflects every bearing about
	// the 45-degree diagonal (theta -> 90 - theta), so the lake would still have
	// perfectly plausible waves, travelling in a consistent direction, that
	// simply were not the direction the wind was blowing. Nothing on screen says
	// "the axes are swapped"; you would have to compare a wave crest against a
	// logged bearing to see it, and the log prints the FROM bearing while the
	// vector carries the TO one, so even that comparison needs care.
	//
	// It is the same class of trap as the from/to flip handled below, and both
	// were flagged in advance by both sides of this interface. This one still
	// got through, in a function whose comment correctly explains the other one.
	//
	// A VELOCITY, so the length is the sustained speed and direction cannot go
	// stale independently of it. Gust rides in B; it is a magnitude in m/s on
	// top of the sustained wind, not a multiplier.
	// THE SMOOTHED VECTOR, not the raw sample -- see the wave-inertia note in
	// Tick. The raw sample is still in FVoxelWeatherState for the log line, the
	// HUD and any consumer that genuinely wants the instantaneous wind; what
	// crosses into a material is the sea's response to it.
	UKismetMaterialLibrary::SetVectorParameterValue(
		World, C, kParamWindVector,
		FLinearColor((float)Impl->SmoothedNorthMS,
		             (float)Impl->SmoothedEastMS,
		             (float)Wind.GustMps, 0.0f));

	// LAST, AND IT MATTERS THAT IT IS LAST. WindFieldValid is the flag every
	// consumer gates on; raising it before the values it vouches for are
	// written would publish one frame of stale wind as though it were fresh.
	// Same ordering discipline as VoxelBathyField's same-frame publication
	// (VoxelBathyField.h:74-81).
	UKismetMaterialLibrary::SetScalarParameterValue(World, C, kParamValid, 1.0f);
}

void UVoxelWeatherSubsystem::PublishInvalid()
{
	UWorld* World = GetWorld();
	if (!World || !Impl)
	{
		return;
	}
	if (UMaterialParameterCollection* C = Impl->Collection.Get())
	{
		UKismetMaterialLibrary::SetScalarParameterValue(World, C, kParamValid, 0.0f);
	}
}

bool UVoxelWeatherSubsystem::GetCameraXY(double& OutX, double& OutY) const
{
	// The same anchor UVoxelBathyFieldSubsystem::GetCameraXY uses
	// (VoxelBathyField.cpp:134-154) and the same one the streamer runs against
	// -- the first local player's possessed pawn. Using the same one keeps "the
	// wind here" meaning the same place as "the terrain here".
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return false;
	}
	const FVector Loc = Pawn->GetActorLocation();
	OutX = Loc.X;
	OutY = Loc.Y;
	return true;
}

void UVoxelWeatherSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!Impl)
	{
		return;
	}

	if (!VoxelWeather::IsEnabled())
	{
		// One frame of work to undo the feature, then IsTickable goes false.
		if (bHasState)
		{
			PublishInvalid();
			// THE BINDING VERDICT SURVIVES THE RESET, deliberately. The rest of
			// the state is a per-frame report and should go, but bMaterialBound
			// records a one-time fact about the ASSET that ResolveCollection
			// will never re-derive (it latches bCollectionLoadAttempted). Wiping
			// it would make a subsystem that had already diagnosed a missing
			// parameter start reporting "bound=0" for the opposite reason, and
			// the log line cannot distinguish those.
			const bool bWasBound = Impl->State.bMaterialBound;
			const int32 Missing = Impl->State.MissingParamCount;
			Impl->State = FVoxelWeatherState();
			Impl->State.bMaterialBound = bWasBound;
			Impl->State.MissingParamCount = Missing;
			bHasState = false;
			UE_LOG(LogVoxelWeather, Log,
			       TEXT("voxel.Weather.Enabled 0: WindFieldValid dropped to 0, every consumer is ")
			       TEXT("back on its own constant."));
		}
		return;
	}

	// --- the clock ------------------------------------------------------------
	//
	// FROM THE SKY, ALWAYS. See the header: a second accumulator here would
	// drift away from the clock the capture harness pins and would quietly
	// break every appearance A/B this project takes.
	UWorld* World = GetWorld();
	const UVoxelSkySubsystem* Sky = World ? World->GetSubsystem<UVoxelSkySubsystem>() : nullptr;
	double Epoch = 0.0;
	if (Sky)
	{
		const FVoxelSkyState& S = Sky->GetSkyState();
		Epoch = S.EpochSeconds;
		if (!S.bClockRunning && !Impl->bLoggedClockOff)
		{
			Impl->bLoggedClockOff = true;
			// WORTH A LINE, because it is genuinely surprising and costs an
			// afternoon otherwise: the sky zeroes its whole state struct when
			// switched off, so EpochSeconds reads 0 rather than holding where it
			// was, and the wind therefore freezes at the t=0 field rather than at
			// what was on screen. That is deterministic and fine; it is just not
			// what "freeze" looks like from outside.
			UE_LOG(LogVoxelWeather, Log,
			       TEXT("the sky clock is not running (voxel.Sky.Enabled 0 or TimeScale 0), so ")
			       TEXT("the wind is frozen at epoch %.3f s. With voxel.Sky.Enabled at 0 that ")
			       TEXT("epoch is 0, not wherever the clock had got to -- the sky clears its ")
			       TEXT("state when it is switched off. The wind is still spatially varying and ")
			       TEXT("still correct; it just will not change."),
			       Epoch);
		}
	}
	else if (!Impl->bLoggedNoSky)
	{
		Impl->bLoggedNoSky = true;
		UE_LOG(LogVoxelWeather, Warning,
		       TEXT("no UVoxelSkySubsystem in this world, so there is no clock. The wind is being ")
		       TEXT("evaluated at epoch 0 and will never advance. It is still a correct, ")
		       TEXT("spatially varying wind field -- and it is the SAME field every run, which ")
		       TEXT("makes this state easy to mistake for a working one."));
	}

	// --- where ----------------------------------------------------------------
	double CamX = 0.0;
	double CamY = 0.0;
	const bool bHasCamera = GetCameraXY(CamX, CamY);

	// --- sample and publish ---------------------------------------------------
	FVoxelWeatherState& S = Impl->State;
	S.EpochSeconds = Epoch;
	S.SampleXUU = CamX;
	S.SampleYUU = CamY;
	S.bHasCamera = bHasCamera;
	S.bBearingPinned = VoxelWeather::IsBearingPinned();
	S.bSpeedPinned = VoxelWeather::IsSpeedPinned();
	S.Wind = SampleWindAtWorldUUAtEpoch(CamX, CamY, Epoch);

	// --- WAVE INERTIA ---------------------------------------------------------
	//
	// THE WIND THAT DRIVES WAVES IS NOT THE WIND. It is a heavily smoothed
	// version of it, and shipping without this was the first thing the owner
	// noticed: "the wave effect is way way too fast, it looks like someone is
	// playing a video at multiple times speed, and the wind effect on water is
	// jittery and moving too fast."
	//
	// WHY IT LOOKS LIKE SPEED RATHER THAN LIKE FLICKER, which is the part worth
	// understanding before touching the number. Wind speed does not merely scale
	// wave HEIGHT -- water_wave_graph.py derives the wavelength from it too
	// (lenScale, U^0.68). Retune the wavelength and every crest in the field
	// moves to a new position in that same frame. So a wind that wanders a few
	// per cent per frame slides the entire pattern a few per cent per frame, on
	// top of the waves' own correct motion, and the eye reads the sum as the
	// whole scene running fast.
	//
	// The owner's own A/B is the proof: with voxel.Weather.Enabled 0 the material
	// falls back to a STATIC 5 m/s wind and he judged it "quite good ... looks
	// like there is still slight breeze blowing across surface". Same wave field,
	// same animation, no per-frame wind -- so the wave maths was never the
	// problem.
	//
	// AND IT IS ALSO THE PHYSICS. Wind seas are duration- and fetch-limited: a
	// sea takes minutes to build to a new wind and longer to lie down, which is
	// why the JONSWAP fetch relation the amplitude comes from is quoted against a
	// SUSTAINED wind and not an instantaneous one. Driving wave height and
	// wavelength from a 6-second gust band is asking the ocean to have no mass.
	//
	// One-pole low-pass, frame-rate independent (alpha from exp(-dt/tau), not a
	// fixed per-frame fraction -- the latter makes the response depend on how
	// fast the machine is, and a capture would then not match a play session).
	// The GUST channel is deliberately NOT smoothed: it is published for spray
	// and foam, which SHOULD twitch, and nothing reads it yet anyway.
	const float TauSeconds = VoxelWeather::GetWaveResponseSeconds();
	const double SmoothN = S.Wind.DirNorth * S.Wind.SustainedMps;
	const double SmoothE = S.Wind.DirEast * S.Wind.SustainedMps;
	// A PIN BYPASSES THE FILTER ENTIRELY, and leaving it out was the bug that
	// kept the water moving after three attempts to stop it.
	//
	// MEASURED, 2026-08-14. With voxel.Weather.PinMps 5 and PinFromDeg 238.7 the
	// SAMPLE was rock steady -- "5.00 m/s from 238.7 deg (sustained 5.00, gust
	// +0.00)" on every line -- while the PUBLISHED vector was still sweeping:
	// (N 2.360, E -1.281) -> (N 2.421, E +0.204) over eight seconds, crawling
	// toward its target of (2.60, 4.27) at exactly the rate a 45 s time constant
	// predicts. About 135 s to converge, and every frame of it a changing wind.
	//
	// Wind speed sets wave WAVELENGTH as well as height, so a wind that is
	// slowly converging retunes the spectrum continuously and slides every crest
	// with it. The filter added to stop the surface moving was what kept it
	// moving -- and a smooth coherent drift reads far worse than a slightly
	// noisy but stationary wind, which is why raising the constant made it worse
	// rather than better.
	//
	// A pin is a deliberate step, not weather. It has no inertia to model.
	const bool bPinned = VoxelWeather::IsBearingPinned() && VoxelWeather::IsSpeedPinned();
	if (bPinned)
	{
		Impl->SmoothedNorthMS = SmoothN;
		Impl->SmoothedEastMS = SmoothE;
		Impl->bSmoothedWindValid = true;
	}
	else if (!Impl->bSmoothedWindValid || TauSeconds <= 0.0f || DeltaTime <= 0.0f)
	{
		// Seeded to the first real sample rather than to zero, so there is no
		// warm-up ramp from a dead calm at world start -- an artefact that would
		// be invisible in play and would quietly corrupt the first seconds of
		// any capture.
		Impl->SmoothedNorthMS = SmoothN;
		Impl->SmoothedEastMS = SmoothE;
		Impl->bSmoothedWindValid = true;
	}
	else
	{
		const double Alpha = 1.0 - FMath::Exp(-(double)DeltaTime / (double)TauSeconds);
		Impl->SmoothedNorthMS += (SmoothN - Impl->SmoothedNorthMS) * Alpha;
		Impl->SmoothedEastMS += (SmoothE - Impl->SmoothedEastMS) * Alpha;
	}

	PublishWind(S.Wind);
	++S.Publishes;
	bHasState = true;

	// --- the periodic line ----------------------------------------------------
	const double LogInterval = VoxelWeather::GetLogIntervalSeconds();
	if (LogInterval > 0.0)
	{
		Impl->SecondsSinceLog += (double)DeltaTime;
		if (Impl->SecondsSinceLog >= LogInterval)
		{
			Impl->SecondsSinceLog = 0.0;
			// Everything here is READ BACK from the state that was actually
			// published, including the ran-flag count and whether the material
			// binding held. A leg that greps this line learns what its frames
			// were taken in; a leg that greps nothing learns that this subsystem
			// did not run, which is a different fact from a calm day.
			// THE PUBLISHED VECTOR IS ON THIS LINE, not just the sample it came
			// from, and the two are different things. The sample is the wind;
			// what a material sees is the SMOOTHED velocity (wave inertia, see
			// Tick), and on 2026-08-14 the owner reported that pinning the live
			// path to the fallback's exact numbers still did not reproduce the
			// fallback's look. That question cannot be answered from the sample
			// alone -- it needs the number that actually crossed into the
			// collection. Printing one and debugging the other is how an
			// afternoon disappears.
			const double PubLen = FMath::Sqrt(Impl->SmoothedNorthMS * Impl->SmoothedNorthMS +
			                                  Impl->SmoothedEastMS * Impl->SmoothedEastMS);
			UE_LOG(LogVoxelWeather, Log,
			       TEXT("wind: %.2f m/s from %.1f deg (sustained %.2f, gust %+.2f) at (%.0f, %.0f) UU, ")
			       TEXT("epoch %.1f s, publishes=%lld, bound=%d, pins[dir=%d spd=%d], camera=%d | ")
			       TEXT("PUBLISHED WindVectorMS=(N %.3f, E %.3f) |v|=%.3f m/s"),
			       S.Wind.SpeedMps, S.Wind.FromBearingDeg, S.Wind.SustainedMps, S.Wind.GustMps,
			       S.SampleXUU, S.SampleYUU, S.EpochSeconds, (long long)S.Publishes,
			       S.bMaterialBound ? 1 : 0, S.bBearingPinned ? 1 : 0, S.bSpeedPinned ? 1 : 0,
			       S.bHasCamera ? 1 : 0,
			       Impl->SmoothedNorthMS, Impl->SmoothedEastMS, PubLen);
		}
	}
}
