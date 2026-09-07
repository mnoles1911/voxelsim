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
// THE CLOCK REPLICATES (F7, docs/water-ocean-tides-plan-2026-09-04.md Phase F).
//
// The clock lives in a UWorldSubsystem and UWorldSubsystems do not replicate,
// so it rides the EXISTING AVoxelEditRelay as two scalars -- the epoch and the
// time scale -- exactly the design this header's old TODO named and exactly
// VoxelEditRelay.h's ServerSeed pattern (NOT a second actor: a second
// replicated actor for two scalars is a channel, a relevancy question and a
// spawn-ordering race bought for nothing). This matters because the clock is
// not only the sky's: the TIDE is pure f(epoch) (VoxelWaterSubsystem's tide
// block reads GetSkyState().EpochSeconds, plan A2), so two clients with skewed
// clocks get skewed SEAS, not just skewed sunsets.
//
// Division of labour: the authority's Tick pushes the pair into the relay at a
// low fixed cadence (TickReplicatedClock); the relay's OnRep_SkyClock forwards
// them here (AdoptReplicatedEpoch); the client blends its local epoch toward
// the dead-reckoned server value under a bounded correction rate -- the whole
// policy, with its constants argued, sits above AdoptReplicatedEpoch's
// declaration below. Standalone touches none of this beyond one NetMode enum
// test per tick: no relay exists there (VoxelEarthGameMode.cpp only spawns one
// when networked), nothing is pushed, nothing is adopted, and the epoch
// accumulates exactly as it always has. A listen server with no client
// connected additionally writes two fields on its own relay per push, which
// replicates to nobody and changes no frame.
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

	// --- measurement arms ----------------------------------------------------
	//
	// PROOF OF TRAFFIC. These are here in the state struct, and not only in a log
	// line, because this project has shipped arms that were accepted on the
	// command line, changed nothing, and read as armed for weeks -- five of them
	// in one night. A cvar being SET proves a string parsed; these three fields
	// are what prove a branch ran.
	//
	// Re-orientation steps SUPPRESSED by voxel.Sky.PinLightOrientation. Exactly 0
	// in every shipped build and in every leg that failed to set the cvar; climbs
	// at the voxel.Sky.ShadowUpdateHz rate in a leg that really engaged the arm.
	int64 LightOrientationsPinned = 0;
	// How far the pinned sun has drifted from where the ephemeris says it should
	// be, in degrees, READ BACK off the actor rather than integrated from the
	// request. It exists to catch the arm's one silent failure: a leg that also
	// pinned voxel.Sky.TimeScale 0 will show LightOrientationsPinned climbing
	// while this stays at 0, because a sun that was not going to move anyway
	// costs nothing to stop -- that leg measured nothing and this is the only
	// reading that says so.
	float PinnedSunErrorDeg = 0.f;
	// What USkyLightComponent::IsRealTimeCaptureEnabled() ACTUALLY RETURNS after
	// voxel.Sky.RealTimeCapture was pushed -- not what was pushed. -1 = never
	// pushed (which is the default build: SpawnRig's call is the only one). The
	// distinction is the point: the engine ANDs our flag with the component's
	// mobility and with r.SkyLight.RealTimeReflectionCapture
	// (SkyLightComponent.cpp:1159-1162), so a leg that echoed its own request
	// could report an armed capture the renderer had already ignored.
	int32 RealTimeCaptureActive = -1;

	// --- sky-epoch replication (F7) ------------------------------------------
	// Proof of traffic, same doctrine as the measurement arms above: a log line
	// proves a receipt printed once; these prove the path kept running. Exactly
	// 0 / 0.0 forever in standalone and on every server -- both are CLIENT-side
	// counters, and that zero is itself the off-arm evidence.
	//
	// Receipts: AdoptReplicatedEpoch invocations landed on this client (one per
	// relay push received, ~1/5s while connected; a stall here with a live
	// connection means the server stopped pushing or the relay is gone).
	int64 EpochReplicationReceipts = 0;
	// The signed epoch error (dead-reckoned server clock minus local clock, in
	// epoch seconds) still being blended away by the bounded correction. Decays
	// toward 0 between receipts; pinned at 0 when no target has ever arrived.
	double EpochCorrectionRemainingS = 0.0;

	bool bSunUp = false;   // apparent altitude > 0 (refraction already folded in)
	bool bMoonUp = false;
	bool bClockRunning = false; // voxel.Sky.Enabled && TimeScale != 0
};

// ---------------------------------------------------------------------------
// THE DAY-NIGHT LIGHT COLOUR RAMP (Phase L2,
// docs/vs-lighting-implementation-plan-2026-09-06.md)
// ---------------------------------------------------------------------------
//
// WHY AN AUTHORED TABLE AND NOT A MEASUREMENT OF THE SKY. The rig paints
// day-night with the SkyAtmosphere's GPU transmittance and holds the sun at a
// constant 5778 K (FVoxelSkyState::SunTemperatureK's own comment: "the sunset
// red comes from the atmosphere"), so there is NO CPU-readable colour anywhere
// in this subsystem to hand the marcher -- which is exactly what the research
// doc recorded as the reason recommendation 2 was skipped. Vintage Story does
// not measure its sky either: SunLightLevels[] and SunColor are hand-shaped
// tables the engine never argues with (research doc section 2, mechanism 7).
// So this is a table, it is small, every row says what it is for, and the owner
// tunes it by eye like every other appearance knob in this project.
//
// PURE FUNCTION OF ELEVATION, deliberately: it takes no world, no clock and no
// subsystem state, which is what lets VoxelSkyTests.cpp pin it as arithmetic
// rather than as a rendering outcome.
struct FVoxelSkyLightColours
{
	// Tints, NOT brightnesses -- multiplied into terms that already carry their
	// own intensity knob (voxel.March.SunWrapGain and voxel.March.AmbientIntensity
	// respectively). White is the neutral, shipped answer for both.
	FLinearColor Sun = FLinearColor::White;
	FLinearColor Ambient = FLinearColor::White;
	// The moon's illuminance as a fraction of the sun's -- passed straight
	// through from the caller so the marcher and MPC_VoxelSky's MoonLightFraction
	// scalar provably carry the same number.
	float MoonFraction = 0.0f;
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

	// How submerged the local camera is, 0..1 -- the SAME weight VoxelOceanActor
	// uses to fade M_Underwater in. Fog density is scaled by (1 - this), so the
	// two extinction models cross-fade and never sum at full strength. The ocean
	// PUSHES this; the sky never asks -- same direction-of-dependency rule as
	// the exposure ownership comment in the .cpp. Defaults 0, so a world with no
	// ocean actor gets full fog and no coupling.
	void SetUnderwaterFogSuppression(float Weight01);

	// Jump the clock to a given local hour (0..24) on the CURRENT game day,
	// preserving the seasonal position as closely as the calendar allows. This
	// is the in-engine half of -VoxelTimeOfDay and the primitive the W6 capture
	// ladder steps with; see the .cpp for why "as closely as the calendar
	// allows" is the honest phrasing and not a weasel.
	void SetTimeOfDay(double LocalHours);

	// Absolute clock set, in game seconds since world start. The one entry
	// point everything else funnels through.
	void SetEpochSeconds(double NewEpochSeconds);
	bool CaptureEpochSeconds(double& Out) const;
    bool CaptureClock(double& Epoch,double& Rate,double& Day,double& Year) const;
    bool RestoreClock(double Epoch,double Rate,double Day,double Year);

	// F7 sky-epoch replication, client-side receive half. Called by
	// AVoxelEditRelay::OnRep_SkyClock (and by nothing else) with the server's
	// epoch and the rate it is advancing at, once per relay push received.
	//
	// POLICY, in full, because this is where a mid-frame sun teleport would come
	// from if it were wrong:
	//   * FIRST receipt SNAPS (SetEpochSeconds). It is the join handshake: the
	//     client's locally-started clock is arbitrarily far from the server's
	//     (hours, for a late joiner) and "blend" across that gap at any honest
	//     rate is a sun that races across the sky for minutes. One discontinuity
	//     at join, before the player has any invested sense of the time of day,
	//     is the cheapest moment this correction will ever be.
	//   * Later receipts only update the TARGET; Tick blends the local epoch
	//     toward it (dead-reckoned forward at the server's time scale between
	//     pushes) at a rate bounded so the CORRECTION adds at most
	//     kSkyEpochMaxCorrectionSunDegPerSec of sun motion on top of the sun's
	//     own -- degrees per second, not epoch seconds, so the visual bound
	//     survives any voxel.Sky.DayLengthSeconds. Small errors decay
	//     exponentially (kSkyEpochCorrectionGainPerSec) so the sun eases out of
	//     a correction rather than hitting a rate cliff at zero.
	//   * A late delta too large for the bounded rate to close within
	//     kSkyEpochSnapRealSeconds (server clock jumped: SetTimeOfDay, a pin, a
	//     long client hitch) SNAPS with a Warning -- crawling the sun across the
	//     sky for minutes to avoid one visible cut is the worse artifact, and
	//     the tide (25 mm datum quanta, rate-limited steps) tracks a snap
	//     exactly as it tracks any other epoch jump.
	//
	// GATE LOG CONTRACT ("SkyEpoch REPLICATED: ..."): printed on the first
	// receipt and on any receipt whose |clientDelta| >= kSkyEpochLogDeltaS;
	// Verbose otherwise. Carries serverEpoch, clientDelta, the action taken
	// (SNAP-JOIN / SNAP-LARGE / BLEND) and the resolved correctionRate bound. A
	// connected client whose log lacks the first-receipt line did not engage
	// this path -- that absence is the gate's failure signal. A FULL MP gate
	// needs a two-process harness this project does not have (stated in the
	// plan's F7, not hidden); until one exists the evidence is this contract
	// plus FVoxelSkyState's receipt counters above. The correction arithmetic
	// itself deliberately stays inline in TickReplicatedClock -- it is four
	// lines against UE math; if it ever grows shape (drift filters, RTT
	// compensation) it should move to voxel-core as a pure function beside the
	// tide LUT, where a golden test can pin it.
	void AdoptReplicatedEpoch(double ServerEpochSeconds, float ServerTimeScale);

private:
	TUniquePtr<FVoxelSkyImpl> Impl;

	// Mirrors UVoxelGISubsystem::bHasState (VoxelGI.cpp:349-354): true from the
	// moment the clock has driven the rig at all, cleared once the rig has been
	// put back to its static pose after a runtime toggle-off. It is what keeps
	// IsTickable() true for exactly the one frame needed to undo the feature.
	bool bHasState = false;

	// True when OnWorldBeginPlay found the world HELD BY THE MENU and deferred
	// the rig spawn to the first unheld Tick.
	//
	// SpawnRig resolves the spawn column's ground height through
	// GetSurfaceHeightUU, which is a worldgen query -- and during the menu no
	// tile has been prefetched for anywhere, so on a world whose spawn tile is
	// not baked that query is fatal in an unattended run. It was the third and
	// last offender found in backlog 0.0k, and the only one that is not a tick:
	// OnWorldBeginPlay fires exactly once, so it could not simply be skipped
	// the way the water and ocean ticks are -- skipping it means no sky at all.
	//
	// Deferring rather than degrading to Z=0 is deliberate. The Z=0 path a few
	// lines above in SpawnRig is documented as "the least-wrong constant" for a
	// world with no terrain subsystem at all, which is a permanent condition.
	// The menu is a temporary one, and taking the permanent fallback for it
	// would leave the rig referenced to sea level for the whole session that
	// follows -- exactly the two-different-altitudes defect that block of
	// comments exists to prevent.
	bool bRigSpawnDeferredForMenu = false;

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

	// The atmospheric height fog (fog plan, 2026-08-20). Lives on the sky rig
	// beside the atmosphere and the exposure PP for the same one-actor-to-find
	// reason. Its colour is BLACK by design -- the SkyAtmosphere supplies all
	// fog colour via r.SupportSkyAtmosphereAffectsHeightFog -- and its density
	// is scaled by (1 - UnderwaterFogSuppression) every frame so it cross-fades
	// against M_Underwater's Beer-Lambert rather than double-counting extinction
	// (the failure that got the OLD height fog deleted, VoxelOceanActor.cpp:133).
	UPROPERTY(Transient)
	TObjectPtr<class UExponentialHeightFogComponent> SkyHeightFog;

	// 0..1, pushed by AVoxelOceanActor's underwater blend; see the setter.
	float UnderwaterFogSuppression = 0.0f;

	// Last tier actually applied, so the tier sink logs once per change and the
	// grid cvars are not re-set every frame. -1 = never applied, forces the
	// first ApplyFogFromState to run the sink.
	int32 AppliedVolumetricTier = -1;

	// --- what the fog was last SEEN to be, for the read-back log --------------
	//
	// THE DEFECT THESE EXIST FOR. voxel.Sky.FogDensity, voxel.Sky.FogHeightFalloff
	// and voxel.Sky.Fog were reported (owner, 2026-08-23) as "set in a live PIE
	// session with no visible change", and the diagnosis written into the backlog
	// was that they are read once at spawn and never re-applied. They are NOT --
	// ApplyFogFromState has re-read all three every tick since the fog landed, and
	// the owner's own log proves that function runs (it prints the volumetric tier
	// line from inside it). But NOTHING IN THE LOG COULD DISTINGUISH "the cvar did
	// not apply" FROM "the cvar applied and the fog is not doing anything", and
	// those are opposite bugs in opposite files. That ambiguity is what cost the
	// session, so the fix is a line that answers it.
	//
	// Sentinels are negative/-1 rather than 0, because 0 is a legal value for
	// every one of these and a first-frame log line is exactly the one worth
	// having.
	float AppliedFogDensity = -1.f;
	float AppliedFogFalloff = -1.f;
	int32 AppliedFogVisible = -1;
	int32 AppliedFogInSkyCapture = -1;

	void ApplyFogFromState();

	// The NIGHT SKY's mesh: a camera-following sphere carrying M_NightSky, which
	// is where the stars and the phased moon disc are drawn. Spawned here rather
	// than by the game mode (which spawns the ocean and the clipmap) because it
	// is part of the sky rig and must go through the SAME ParseSpawnColumnUU as
	// the rest of it -- see SpawnRig. It CANNOT be a component on SkyRigActor;
	// AVoxelSkyDomeActor's header states why at length, in one line: that actor's
	// root is the SkyAtmosphere and its transform must not move.
	UPROPERTY(Transient)
	TObjectPtr<class AVoxelSkyDomeActor> SkyDome;

	// The spawn column's WORLDGEN ground height in UU, resolved once in SpawnRig
	// through GetSurfaceHeightUU. Two things need it and they are 2187.6 m apart
	// if either gets it wrong: the height fog's reference height, and the
	// SkyLight's capture position under voxel.Sky.SkyLightAtGroundZ. It is a
	// member rather than a local because the second of those is re-applied live.
	// 0 (sea level) is the honest degraded answer when there is no terrain
	// subsystem, and the fog's ran-flag line prints it either way.
	double SpawnGroundZUU = 0.0;

	// Last placement actually applied for the SkyLight, so the push below is a
	// compare rather than a per-frame SetActorLocation on a light that a
	// real-time capture is reading. -1 = never applied.
	int32 AppliedSkyLightAtGroundZ = -1;

	// Last value of voxel.Sky.RealTimeCapture actually pushed into the SkyLight,
	// so the per-frame call is an int compare rather than a
	// SetRealTimeCaptureEnabled (which calls MarkRenderStateDirty and
	// SetCaptureIsDirty on every invocation, i.e. it is NOT free to re-assert).
	// SEEDED TO 1 BY SpawnRig, not left at -1, because SpawnRig has just set the
	// component true: that is what makes a default build byte-for-byte the build
	// it was before this arm existed, rather than one redundant render-state
	// dirty per session. -1 would mean "never pushed" and is only reachable if
	// SpawnRig never ran.
	int32 AppliedRealTimeCapture = -1;

	// Re-applies voxel.Sky.SkyLightAtGroundZ. LIVE rather than spawn-time-only,
	// and that is not a nicety: tools/voxel-capture.ps1 passes -Cvars through
	// -ExecCmds, which lands AFTER BeginPlay (the script says so at :114-116 and
	// refuses some names for exactly this reason), so a spawn-time-only switch
	// could not be reached by the capture sweep that is supposed to judge it.
	// voxel.Sky.AtmosphereDome's help string records the same rule in one line:
	// a switch that cannot be flipped inside one session makes its own A/B
	// unreadable.
	void ApplySkyLightPlacement();

	// Pushes voxel.Sky.RealTimeCapture into the SkyLight and logs the value READ
	// BACK off the component. LIVE for the same reason ApplySkyLightPlacement is,
	// with one consequence worth knowing before you go looking for it: it is
	// driven from Tick, and Tick does not run at voxel.Sky.Enabled 0, so the arm
	// is unreachable while the clock is off. That is documented in the cvar's
	// help rather than worked around, because a second application path is a
	// second thing that can disagree about the state of one component.
	//
	// This is a COST arm, not an appearance one -- it is the only switch in this
	// file that exists purely so a perf leg can size something -- but it has an
	// appearance consequence at 0 (the ambient term freezes rather than following
	// the sky down), which is why its default is 1 and why nothing in this file
	// chooses 0 on its own.
	void ApplySkyLightRealTimeCapture();

	// F7 sky-epoch replication, the per-tick half. Called from Tick right after
	// the local epoch accumulates and BEFORE the ephemeris reads it, so a
	// correction is part of the frame's clock rather than a retroactive nudge.
	// Branches on NetMode once: standalone returns immediately (the off arm --
	// no relay exists, nothing else runs); a server pushes the epoch + time
	// scale into the relay at kSkyEpochPushPeriodSeconds; a client dead-reckons
	// the replicated target and applies the bounded correction documented at
	// AdoptReplicatedEpoch.
	void TickReplicatedClock(float DeltaTime, double TimeScale);

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
	// The material parameter collection every sky-coupled system binds to:
	// M_NightSky's parameters, the weather wind vector, the bathy and ripple
	// field windows. Named ONCE, here, because the string appears in LoadObject
	// calls and diagnostics across four .cpp files and a typo in any of them is
	// a system that renders the asset's DEFAULTS rather than an error. It lived
	// as four per-file internal-linkage copies until 2026-08-23, when two of
	// them landed in the same unity blob of the GAME target (the editor target
	// happened to group them apart) and the module stopped compiling with
	// C2374. An inline constexpr is one entity; it cannot collide with itself.
	inline constexpr const TCHAR* kSkyCollectionPath = TEXT("/Game/Voxel/MPC_VoxelSky.MPC_VoxelSky");

	VOXELEARTH_API bool IsEnabled();
	VOXELEARTH_API float GetTimeScale();
	VOXELEARTH_API void SetTimeScale(float NewScale); // ECVF_SetByCode; -VoxelTimeScale= uses it
	VOXELEARTH_API double GetDayLengthSeconds();
	VOXELEARTH_API double GetDaysPerYear();

	// FVoxelSkyState::DayOfYear (0..365) -> calendar month (1..12) and day (1..31)
	// in the ephemeris's REFERENCE YEAR 2000, which is a LEAP year -- February has
	// 29 days and day-of-year 79 is 20 March, not 21 (VoxelEphemeris.h:150-153).
	// The output pair is deliberately the same MM-DD form -VoxelDate= takes, so a
	// recorded leg can be replayed by pasting its own numbers back.
	//
	// EXPORTED RATHER THAN COPIED, and that is the point of it being here. This
	// derivation had two independent copies -- the anonymous-namespace original in
	// VoxelSkySubsystem.cpp and kPerfDaysBeforeMonth/kPerfDaysInMonth in
	// VoxelPerfRunSubsystem.cpp, whose own comment recorded the copy as debt and
	// named this accessor as the correct end state. The F1 overlay was the third
	// consumer, and a third copy is how VoxelClimateProbe.h's disaster happens:
	// four independent climate calibrations drifting apart is what made the whole
	// world classify as desert. One table, one definition, one answer.
	VOXELEARTH_API void MonthDayFromDayOfYear(int32 DayOfYear, int32& OutMonth, int32& OutDay);

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

	// --- the IsSky dome (AVoxelSkyDomeActor + M_SkyAtmosphereDome) ------------
	//
	// A SEPARATE KNOB FROM IsDomeEnabled, and the two must not be folded together.
	// IsDomeEnabled hides the star dome, which is additive and purely decorative:
	// off means one less thing added to the sky. This one decides WHO PAINTS THE
	// SKY AT ALL -- while the IsSky dome is in the scene the SkyAtmosphere's
	// full-screen pass stops emitting sky pixels (SkyAtmosphereRendering.cpp:2214),
	// so hiding it hands that job back and showing it takes it away. Same actor,
	// same follow, same radius; completely different consequence, which is why
	// each has its own cvar and its own logged transition.
	//
	// Read every tick by AVoxelSkyDomeActor::ApplyDomeCvars, so it is a live knob
	// -- non-negotiable, because S1's gate is an on/off A/B inside ONE process
	// (the cross-session screenshot floor is 1.81%, the within-session floor
	// 0.00%) and a spawn-time-only switch cannot express that comparison.
	VOXELEARTH_API bool IsAtmosphereDomeEnabled();

	// The RESOLVED gain on the star map inside the SkyLight's real-time capture,
	// written into MPC_VoxelSky.StarAmbientGain every frame.
	//
	// DERIVED BY DEFAULT from kStarAmbientCalibration (VoxelSkySubsystem.cpp), which
	// is the measured ratio between the two rendering contexts that carry the same
	// star map: the visible additive dome and the capture's emissive branch. That is
	// what stops "how bright stars look" and "how much they light the world" from
	// being two free knobs that agree only by coincidence.
	//
	// voxel.Sky.StarAmbientGain defaults to the NEGATIVE SENTINEL -1 meaning derive;
	// any value >= 0 overrides verbatim. So -1 does NOT mean "off" -- 0 means off --
	// and this accessor never returns a negative number whatever the cvar holds.
	// Always report what this returns, never the cvar: the cvar reads -1 on every
	// shipped run.
	// --- the day-night light colour ramp (Phase L2) --------------------------
	//
	// (sun elevation in degrees, moon illuminance as a fraction of the sun's)
	// -> the tints the marcher's VS-lighting terms are multiplied by. PURE: the
	// same two inputs always give the same answer, on any thread, with no world
	// and no subsystem. That is what makes it testable (VoxelSkyTests.cpp,
	// VoxelEarth.Sky.LightColourRamp) and it is the reason the function is here
	// rather than a private method on the subsystem.
	//
	// SunAltitudeDeg is the APPARENT altitude the ephemeris reports (refraction
	// already folded in), clamped internally to [-90, +90]; anything outside is
	// a caller bug and is clamped rather than extrapolated, because the table's
	// end rows are the answers for "below the horizon" and "high", not the start
	// of a trend to continue.
	//
	// MoonFraction is FVoxelSkyState::MoonIntensity / VoxelSky::GetSunIntensity()
	// -- the SAME quantity ApplySkyMaterialParams writes into MPC_VoxelSky's
	// MoonLightFraction, carrying the horizon gate, the illuminated fraction, the
	// daylight suppression and voxel.Sky.MoonIntensity with it. It only ever
	// scales the NIGHT rows: a full moon lifts and neutralises the night tint, a
	// new moon leaves it at its dim cool floor.
	VOXELEARTH_API FVoxelSkyLightColours SampleLightColours(double SunAltitudeDeg,
	                                                        float MoonFraction);

	VOXELEARTH_API float GetStarAmbientGain();
	// Whether GetStarAmbientGain() came from the calibration constant or from an
	// explicit override. Exists so the log can say WHICH -- an override and the
	// derived value can be the same number (the S2 gate's A arm pins 1.0, which is
	// also what the calibration derives), and "the calibration is not being enforced
	// on this run" is not visible in the value alone.
	VOXELEARTH_API bool IsStarAmbientGainDerived();
}
