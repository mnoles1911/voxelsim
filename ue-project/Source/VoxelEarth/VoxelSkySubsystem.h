#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelSkySubsystem.generated.h"

// World clock + day/night light rig + exposure policy (W3/W4/W5 of
// docs/lighting-weather-plan.md).
//
// WHAT THIS OWNS. One game clock (a single accumulating "world epoch" in
// seconds), the ephemeris evaluation that turns it into a sun and a moon
// (VoxelEphemeris.h), the three-actor light rig those drive (DirectionalLight
// sun, DirectionalLight moon, SkyLight, SkyAtmosphere), and the post-process
// that pins EXPOSURE so that a dark frame renders dark. That last one is not a
// separate feature bolted on: without it the rest is unverifiable, and the
// reason is written out at length beside CVarSkyExposureMode in the .cpp.
//
// CLIENT-SIDE RENDERING ONLY, outside the determinism boundary -- the same
// argument VoxelGI.h:26-30 makes for the light field and VoxelEphemeris.h:7-25
// makes for the ephemeris itself. This subsystem reads a clock and writes
// light component properties. It never calls worldgen, never touches the edit
// log, and nothing it produces is replicated or digested. Two clients whose
// sun altitudes differ in the twelfth decimal place still agree bit-for-bit
// about world state.
//
// ZERO PER-FRAME COST WHEN OFF. IsTickable() is false once voxel.Sky.Enabled
// is 0 and the rig has been returned to its static pose (bHasState), exactly
// the shape VoxelGI.cpp:349-354 uses. With the clock off the rig still EXISTS
// and is still lit -- see OnWorldBeginPlay -- it is simply frozen at the pose
// the pre-W4 static rig used, so turning the feature off cannot turn the
// lights off.
//
// SCOPE: STANDALONE AND LISTEN SERVER ONLY, TODAY.
//
// The clock lives in a UWorldSubsystem and UWorldSubsystems do not replicate.
// The rig used to be spawned from AVoxelEarthGameMode::BeginPlay, which is
// server-only, and it now spawns from this subsystem's OnWorldBeginPlay, which
// runs on every instance -- so a dedicated-server CLIENT does get a rig, but it
// gets one driven by its OWN locally-started epoch, which begins at whatever
// moment that client joined. Every client would therefore render a different
// (and, relative to the server's world, stale) sky. Nothing about the game is
// WRONG in that state -- the sky is not simulation -- but two players standing
// next to each other would disagree about whether it is night, which is worse
// than either being wrong alone.
//
// TODO (not built today, deliberately): replicate the clock as TWO scalars --
// the world epoch in seconds and the time scale -- as UPROPERTY(Replicated)
// fields on the EXISTING AVoxelEditRelay, following VoxelEditRelay.h:55-60's
// ServerSeed/ServerWorldGenVersion pattern exactly. Two floats on an actor
// that already exists, already replicates, and whose header comment already
// anticipates exactly this kind of reuse ("the relay generalizes to non-edit
// authoritative streams later"). NOT a second actor: a second replicated actor
// for two scalars is a channel, a relevancy question and a spawn-ordering race
// bought for nothing. The client-side half is then "adopt the replicated epoch
// instead of accumulating locally", i.e. one branch in Tick.
//
// NO PERSISTENCE TODAY, also deliberately. The command-line pins below
// (-VoxelTimeOfDay / -VoxelDate / -VoxelTimeScale) cover every current need,
// which is entirely harness capture legs; a sidecar epoch file written ahead
// of the save-format design would be a file format to migrate later in
// exchange for nothing anyone has asked for. The epoch is a single double and
// will drop into whatever the world-save header becomes.
struct FVoxelSkyImpl;

// The clock's answer for one frame, in scalars.
//
// PLAIN STRUCT, NOT USTRUCT, AND SCALAR MEMBERS ONLY -- the same doctrine
// VoxelWaterSubsystem.h:44-47 states for its probe PODs. No voxel-core type
// and no ephemeris type crosses this header: VoxelSky::FSunState is a
// VoxelEphemeris.h type and including that header here would put a
// double-precision astronomy header inside a UHT-parsed translation unit for
// no reason at all, since every consumer of this struct wants degrees and
// intensities rather than vectors.
//
// Everything here is a READ-ONLY REPORT. Nothing consults it to decide
// anything; it exists so the HUD, the W6 capture ladder and any later
// verification leg can state what the frame actually used rather than what it
// was asked for (VoxelGpuVerify.cpp:2118-2126's rule, applied to state rather
// than to a cvar).
struct FVoxelSkyState
{
	// --- clock ---------------------------------------------------------------
	double EpochSeconds = 0.0;   // the accumulator itself; game seconds since world start
	double JulianDay = 0.0;      // what the ephemeris was actually evaluated at
	double DayFraction = 0.0;    // 0..1 through the current game day
	double LocalHours = 0.0;     // DayFraction * 24, i.e. the UTC hour the sun is at
	int32 DayOfYear = 0;         // 0..365, the SEASONAL clock (see JulianDayFromGameClock)

	// --- where the observer is standing --------------------------------------
	double LatitudeDeg = 0.0;
	double LongitudeDeg = 0.0;

	// --- bodies --------------------------------------------------------------
	double SunAltitudeDeg = 0.0;
	double SunAzimuthDeg = 0.0;
	double MoonAltitudeDeg = 0.0;
	double MoonAzimuthDeg = 0.0;
	double MoonPhaseFraction = 0.0;       // 0 new, 0.5 full -- waxing/waning distinguishable
	double MoonIlluminatedFraction = 0.0; // 0..1, THE ONE to scale moonlight by

	// --- what was pushed into the rig ----------------------------------------
	// Sun: UDirectionalLightComponent::Intensity, which is the OUTER-SPACE
	// illuminance and therefore does NOT vary with altitude and is NEVER zero.
	// It is not a measure of how bright the frame is -- the SkyAtmosphere applies
	// the air mass and UE applies N.L on top, so a reader wanting "how much sun
	// is landing" wants SunAltitudeDeg alongside it. It reads constant across a
	// whole ladder ON PURPOSE; that is the fix for twilight, not a stuck value.
	float SunIntensity = 0.f;
	float SunTemperatureK = 0.f;   // constant 5778 K; the sunset red comes from the atmosphere
	float MoonIntensity = 0.f;     // peak * illuminated fraction * gates; 0 when MoonEnabled is 0
	// The RESOLVED moon temperature, after the mired lerp against
	// voxel.Sky.MoonTintStrength and after UE's own 1000..15000 clamp -- not what
	// voxel.Sky.MoonTemperatureK was asked for. Deliberately HIGHER than
	// SunTemperatureK above: a cooler-looking moon is a HIGHER Kelvin, and that
	// inversion is what made this file ship a warm moon once already. Perceptual
	// convention, not spectroscopy; kMoonTemperatureK in the .cpp records the
	// physical fact (albedo ~0.12, spectrum near-identical to sunlight) so that
	// nobody undoes it on physical grounds.
	float MoonTemperatureK = 0.f;
	float ExposureBiasEV = 0.f;    // the stop the sky post-process is holding
	int32 ExposureMode = 0;        // the mode it is holding it in (0/1/2)

	// --- cadence bookkeeping (voxel.Sky.ShadowUpdateHz) ----------------------
	// Reported so a perf leg can say how many times the sun actually MOVED over
	// a capture rather than assuming the cap fired at the rate it was asked for.
	int64 LightUpdates = 0;
	double SecondsSinceLightUpdate = 0.0;

	bool bSunUp = false;   // apparent altitude > 0 (refraction already folded in)
	bool bMoonUp = false;
	bool bClockRunning = false; // voxel.Sky.Enabled && TimeScale != 0
};

UCLASS()
class VOXELEARTH_API UVoxelSkySubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UVoxelSkySubsystem();
	// Declared (not defaulted) here and defined in the .cpp: TUniquePtr<FVoxelSkyImpl>'s
	// destructor needs FVoxelSkyImpl's full definition, which this UHT-parsed
	// header must not see. Identical reasoning to VoxelWorldSubsystem.h:32-39
	// and VoxelWaterSubsystem.h:146-150; see either for the long form.
	virtual ~UVoxelSkySubsystem() override;
	// UHT auto-generates this hot-reload constructor unless one is already
	// declared, and the auto-generated version lives in VoxelSkySubsystem.gen.cpp,
	// which cannot see FVoxelSkyImpl either.
	//
	// PIMPL FROM THE START, even though today's impl is a handful of scalars and
	// four actor pointers. Retrofitting a PImpl onto a shipped UHT header is pure
	// churn -- every member moves, the destructor changes shape, and the diff
	// buries whatever real change it is riding along with. The impl grows the
	// moment weather lands (docs/lighting-weather-plan.md W8+), which is the next
	// thing to touch this file.
	UVoxelSkySubsystem(FVTableHelper& Helper);

	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	//~ End UWorldSubsystem Interface

	//~ Begin FTickableGameObject / UTickableWorldSubsystem Interface
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject / UTickableWorldSubsystem Interface

	// The last evaluated frame, in scalars. Safe at any time (returns a zeroed
	// state before the first tick, and when Impl is null); allocates nothing.
	const FVoxelSkyState& GetSkyState() const;

	// Jump the clock to a given local hour (0..24) on the CURRENT game day,
	// preserving the seasonal position as closely as the calendar allows. This
	// is the in-engine half of -VoxelTimeOfDay and the primitive the W6 capture
	// ladder steps with; see the .cpp for why "as closely as the calendar
	// allows" is the honest phrasing and not a weasel.
	void SetTimeOfDay(double LocalHours);

	// Absolute clock set, in game seconds since world start. The one entry
	// point everything else funnels through.
	void SetEpochSeconds(double NewEpochSeconds);

private:
	TUniquePtr<FVoxelSkyImpl> Impl;

	// Mirrors UVoxelGISubsystem::bHasState (VoxelGI.cpp:349-354): true from the
	// moment the clock has driven the rig at all, cleared once the rig has been
	// put back to its static pose after a runtime toggle-off. It is what keeps
	// IsTickable() true for exactly the one frame needed to undo the feature.
	bool bHasState = false;

	// The rig. Spawned in OnWorldBeginPlay (MOVED here wholesale from
	// AVoxelEarthGameMode::BeginPlay, which no longer spawns any of it) so that
	// the thing that drives the lights is the same thing that created them --
	// the alternative, "adopt whatever ADirectionalLight the game mode happened
	// to spawn", makes the driver's correctness depend on an actor-iteration
	// order and on nobody ever placing a second directional light in a map.
	UPROPERTY(Transient)
	TObjectPtr<class ADirectionalLight> SunLight;

	// The MOON, as UE's SECOND atmosphere light (SetAtmosphereSunLightIndex(1)).
	// UE supports exactly two natively and this is what the second one is for:
	// it gets its own disc in the SkyAtmosphere and its own scattering, which a
	// point light or a tinted-down sun cannot produce.
	UPROPERTY(Transient)
	TObjectPtr<class ADirectionalLight> MoonLight;

	UPROPERTY(Transient)
	TObjectPtr<class ASkyLight> SkyLightActor;

	// Hosts the SkyAtmosphereComponent AND the exposure post-process, on one
	// actor placed at the spawn column (see OnWorldBeginPlay for why the
	// placement is not optional).
	UPROPERTY(Transient)
	TObjectPtr<AActor> SkyRigActor;

	UPROPERTY(Transient)
	TObjectPtr<class UPostProcessComponent> SkyExposurePP;

	// The NIGHT SKY's mesh: a camera-following sphere carrying M_NightSky, which
	// is where the stars and the phased moon disc are drawn. Spawned here rather
	// than by the game mode (which spawns the ocean and the clipmap) because it
	// is part of the sky rig and must go through the SAME ParseSpawnColumnUU as
	// the rest of it -- see SpawnRig. It CANNOT be a component on SkyRigActor;
	// AVoxelSkyDomeActor's header states why at length, in one line: that actor's
	// root is the SkyAtmosphere and its transform must not move.
	UPROPERTY(Transient)
	TObjectPtr<class AVoxelSkyDomeActor> SkyDome;

	void SpawnRig(UWorld& World);
	void ApplyStaticRigPose();
	void ApplyLightsFromState();
	void ApplyExposureFromState();

	// Pushes the frame's sun/moon/observer state into /Game/Voxel/MPC_VoxelSky,
	// which is what M_NightSky reads. Called from Tick EVERY FRAME, immediately
	// after ApplyExposureFromState and OUTSIDE the voxel.Sky.ShadowUpdateHz
	// cadence gate; the reasoning is identical to exposure's (these are uniform
	// writes that bust no shadow cache) plus one of its own: the moon disc this
	// drives is 0.52 degrees across, so stepping its position at 10 Hz would be a
	// visible stutter on the one object in the night sky small enough to see it
	// happen.
	void ApplySkyMaterialParams();

	bool ResolveObserverXYUU(double& OutXUU, double& OutYUU) const;
};

// Cvar accessors.
//
// Free functions rather than exported cvar objects, following VoxelDebug.h's
// pattern for the same reason: a TAutoConsoleVariable that other files can reach
// is a value four files clamp four different ways. Clamping happens once, here,
// and every caller gets the clamped answer.
//
// These land in the SAME namespace VoxelEphemeris.h uses, deliberately -- a
// caller already writing VoxelSky::ComputeSun should not have to learn a second
// namespace to ask how long a day is. Placed AFTER the UCLASS, matching
// VoxelGI.h:385's placement of namespace VoxelGI; UHT is happiest with
// everything non-reflected out of its way.
namespace VoxelSky
{
	VOXELEARTH_API bool IsEnabled();
	VOXELEARTH_API float GetTimeScale();
	VOXELEARTH_API void SetTimeScale(float NewScale); // ECVF_SetByCode; -VoxelTimeScale= uses it
	VOXELEARTH_API double GetDayLengthSeconds();
	VOXELEARTH_API double GetDaysPerYear();
	VOXELEARTH_API double GetOriginLatitudeDeg();
	VOXELEARTH_API double GetOriginLongitudeDeg();
	VOXELEARTH_API bool IsMoonEnabled();
	// Sun: the OUTER-SPACE illuminance, altitude-independent, and floored strictly
	// above zero -- a directional light at exactly 0 is removed from FScene and
	// takes the SkyAtmosphere's twilight with it. Moon: the full-moon peak, an
	// artistic number ~10 stops above the real 1:400000 lux ratio. Both are
	// argued at length beside their cvars in the .cpp; do not move either without
	// reading that.
	VOXELEARTH_API float GetSunIntensity();
	VOXELEARTH_API float GetMoonIntensity();
	// The REQUESTED moon temperature, clamped to UE's own 1000..15000 window. NOT
	// what the light is given: voxel.Sky.MoonTintStrength lerps between the sun's
	// temperature and this one in mired space first. The resolved answer is
	// FVoxelSkyState::MoonTemperatureK, and that is the one a log line may quote.
	VOXELEARTH_API float GetMoonTemperatureK();
	VOXELEARTH_API float GetMoonTintStrength();
	// Stops subtracted from the +15.6 EV twilight cap once the sun is below
	// astronomical twilight, ramped in over -15..-18 deg. 0 restores the flat cap
	// the pre-fix curve had. Floored at 0 -- it may only ever darken, or it would
	// reopen the constraint the cap was chosen to satisfy.
	//
	// THE CAP IS NOW REACHED AT -6 DEG, NOT -2. Nothing about this knob changed
	// with that -- it still starts at -15 and still only subtracts -- but a
	// reader reasoning about "the twilight cap" should know the band it covers
	// begins at civil twilight's end, because above -6 the curve is tracking a
	// sky that is still lit. ExposureBiasForSunAltitude has the measurement.
	VOXELEARTH_API float GetDeepNightDropEV();
	VOXELEARTH_API float GetShadowUpdateHz();
	VOXELEARTH_API int32 GetExposureMode();
	VOXELEARTH_API float GetExposureBias();

	// --- the night-sky dome (AVoxelSkyDomeActor + M_NightSky) ----------------
	// Read every tick by the dome actor, so both are live knobs.
	VOXELEARTH_API bool IsDomeEnabled();
	// FLOORED, not clamped to a default: the dome must be farther than the
	// farthest drawn terrain or M_NightSky's depth test hides the stars behind
	// the horizon. The floor here is a sanity bound only -- the REAL check is
	// AVoxelSkyDomeActor::BeginPlay measuring this against
	// AVoxelClipmapActor::OuterHalfExtentUU() and logging an Error if it loses.
	VOXELEARTH_API double GetDomeRadiusUU();
	// Constant offset on the star map's rotation, in TURNS. Exists because
	// M_NightSky folds local sidereal time and the map's RA origin into one
	// scalar, so once C++ drives that scalar every frame there is nowhere on the
	// asset left to put the offset. See the cvar for the full argument.
	VOXELEARTH_API double GetStarRotationOffsetTurns();
}
