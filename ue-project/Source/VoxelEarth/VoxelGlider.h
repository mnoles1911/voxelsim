#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
// Included rather than forward-declared: GetBody() below dereferences the
// TObjectPtr in an inline body, which needs the complete type.
#include "VoxelAssetBody.h"
#include "VoxelMovementTuning.h"
#include "VoxelGlider.generated.h"

class APlayerController;
class UCameraComponent;

// ============================================================================
// A GLIDER
// ============================================================================
//
// Phase E of docs/water-ocean-tides-plan-2026-09-04.md: deploy while falling,
// ride the same wind that drives the waves, glide at about 1:8, land on the
// terrain or ditch in the water, and give the player back their feet.
//
// --- KINEMATIC, AND THAT IS THE HOUSE PATTERN, NOT A SHORTCUT ---------------
//
// No Chaos body. AVoxelEarthFlyPawn and UVoxelCharacterMovementComponent are
// both kinematic against the voxel DDA for a structural reason -- terrain
// carries no Chaos collision anywhere in this project -- and a glider gains
// nothing from a solver it cannot collide with. AVoxelBoat is the deliberate
// exception and says why in its own header (four corner forces are how a hull
// gets pitch and roll for free); a wing is one force at one point.
//
// So: a point mass with a velocity, integrated here, with the ATTITUDE driven
// directly by rate controls rather than by torques. That is the arcade choice
// and it is deliberate -- see the stall note in VoxelGliderTuning.
//
// --- THE WIND IS THE SAME WIND THE WAVES USE --------------------------------
//
// UVoxelWeatherSubsystem::SampleWindAtWorldUU, the field query -- not
// GetWeatherState().Wind, which is the CAMERA's wind and is what materials see.
// At the camera's own position the two agree exactly; a glider three hundred
// metres downwind wants the wind where the glider is.
//
// AND THE AXIS ORDER IS A TRAP THIS FILE WALKS PAST DELIBERATELY. This engine's
// world axes are X = NORTH, Y = EAST, Z = up (VoxelEphemeris.h:43-45, and
// UVoxelWeatherSubsystem::PublishWind spends thirty lines on having got exactly
// this backwards once). FVoxelWindSample's own field comment currently says
// "+X east and +Y north", which is the (x=east, y=north) order of a map and is
// NOT this engine's -- the authority is PublishWind, which puts North in R (the
// x component) and East in G. So the world wind vector here is
// (NorthMps, EastMps, 0) and not the other way round. Getting it wrong reflects
// every bearing about the 45-degree diagonal and produces a glider drifting in a
// completely plausible direction that is not the one the waves are running.
//
// --- THE GLIDE RATIO IS THE GATE --------------------------------------------
//
// CD0 and InducedK in VoxelGliderTuning are not two independent knobs: together
// they ARE the maximum lift-to-drag ratio, 1/(2*sqrt(CD0*k)) = 8.16, which is
// the plan's "~1:8". Anyone retuning should pick the ratio first and solve back,
// and the falsifiable check is the cheapest one available -- fly in still air
// from a known height and measure the horizontal distance. Eight times the drop
// or the numbers are wrong.
//
// --- HOW IT ENDS (owner respec 2026-09-05) ----------------------------------
//
// v1 destroyed the glider on every ending, on the theory that an unpossessed
// kinematic actor integrating forever serves nobody. The owner's live session
// overruled it: a spawned glider that "despawned or fell through the surface"
// at touchdown is the recorded FAIL, and the spec is that landed gliders REST
// ON THE SURFACE persistently. So the endings are now:
//
//   * TERRAIN LANDING -> PARK. Flare, settle onto the raycast surface (belly
//     on the voxel top, level pose, velocity zeroed) and STAY, as a persistent
//     inert actor. Parked is not "integrating forever": the parked tick does
//     no aero, no integration and no terrain probes. A pilot aboard stays
//     aboard -- W or Space relaunches (an impulse; a ground run is out of
//     scope, stated in VoxelGliderTuning), E dismounts and leaves the wing
//     parked. A parked glider is boarded with E, the boat's interact model.
//   * WATER DITCH -> destroy, as before. One splash, the pilot surfaces on
//     their own feet. A glider does not float and does not park on a lake.
//   * STOW (X in flight, voxel.Glider.Stow) -> destroy, as before: the wing
//     goes back in the pack wherever it is.
// ============================================================================

UCLASS()
class VOXELEARTH_API AVoxelGlider : public APawn
{
	GENERATED_BODY()

public:
	AVoxelGlider();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	// --- launch (plan E, "Launch v1: deploy while falling") -------------------
	//
	// Spawns a glider under the controller's current pawn, hands it the pawn's
	// own velocity (so opening at speed keeps that speed) and possesses it.
	// REFUSES, with a log line saying which test failed, when the pawn is
	// already in a vehicle or is too close to the ground -- opening a wing 3 m up
	// is a crash, and refusing it is better than simulating one.
	static bool TryDeploy(APlayerController* PC);

	// Put the player back where the glider is, right now, and DESTROY it. Since
	// the 2026-09-05 respec this is the stow/bail/ditch path only -- a terrain
	// landing parks instead (see Park below).
	void ReturnPilot(const FVector& PilotWorldPos);

	// --- boarding (owner respec 2026-09-05; the boat's interact model) --------

	// The nearest PARKED, uncrewed glider to `From` within MaxRangeUU, or null.
	// Static and public for the same reason AVoxelBoat::FindNearest is: the
	// interact key lives in the player controller, which picks the nearest
	// boardable vehicle across both classes (tie rule documented there).
	static AVoxelGlider* FindNearestParked(const UWorld* World, const FVector& From,
	                                       double MaxRangeUU);

	// Board a parked glider (voxel.Glider.Board -- the same path as E). Same
	// contract as AVoxelBoat::TryEnterNearest, including the say-why log line
	// when nothing is in range.
	static bool TryBoardNearest(APlayerController* PC);

	// Take the seat of a PARKED glider: store the outgoing pawn (hidden,
	// ticking off, collision off) and possess this. False if airborne, already
	// crewed, or there is no controller.
	bool Board(APlayerController* PC);

	// The interact key while possessed. Parked: dismount off the wingtip and
	// LEAVE the glider parked. Airborne: bail out exactly as before (stow --
	// ReturnPilot at the current position).
	void ExitToStoredPawn();

	bool IsParked() const { return bParked; }
	bool IsCrewed() const { return StoredPawn.IsValid(); }

	// The AIRBORNE autonomous spawn (voxel.Glider.Spawn with an AltitudeM
	// argument): already moving, glides until it lands, then PARKS.
	static AVoxelGlider* SpawnAhead(UWorld* World, double AheadUU, double AltitudeUU);

	// The PARKED spawn (voxel.Glider.Spawn's default since the respec): resting
	// on the raycast ground ahead of the player, like voxel.Boat.Spawn on
	// water. Refuses -- with a log line -- over unstreamed columns and over
	// water, rather than guessing where the surface is.
	static AVoxelGlider* SpawnParkedAhead(UWorld* World, double AheadUU);

	// --- diagnostics ----------------------------------------------------------
	//
	// AeroTicks is the engagement proof and it is separate from Ticks on purpose:
	// a glider integrating gravity with the aero faded out (below MinAirspeedMS)
	// is a rock, and a rock and a wing produce the same log line if only one
	// counter exists.
	uint64 GetTicks() const { return Ticks; }
	uint64 GetAeroTicks() const { return AeroTicks; }
	uint64 GetGroundHits() const { return GroundHits; }
	uint64 GetFlareTicks() const { return FlareTicks; }
	uint64 GetParkTicks() const { return ParkTicks; }
	double GetLastAirspeedMS() const { return LastAirspeedMS; }
	double GetLastAlphaDeg() const { return LastAlphaDeg; }
	double GetLastGlideRatio() const { return LastGlideRatio; }
	double GetLastWindMS() const { return LastWindMS; }
	const UVoxelAssetBodyComponent* GetBody() const { return Body; }

private:
	void InputPitch(float Value);
	void InputRoll(float Value);
	void InputLookYaw(float Value);
	void InputLookPitch(float Value);
	void InputLaunchPressed();

	// Terrain ending parks the glider (and keeps any pilot aboard); the water
	// ending returns the pilot and destroys it, as v1 did.
	bool CheckTerrain(float DeltaSeconds);
	bool CheckWater();

	// Settle at rest on the raycast surface: belly on the voxel top, level
	// pose, velocity zeroed, bParked set. Pose and state only -- the callers
	// (touchdown, parked spawn) each log their own line, because "LANDED" and
	// "spawned parked" are different facts.
	void SettleParked(double SurfaceTopZ);

	// Leave the parked state under a crewed glider: an impulse along the
	// wing's forward at LaunchPitchDeg/LaunchSpeedMS (VoxelGliderTuning -- a
	// ground-run takeoff is out of scope). TriggerDesc names the key, for the
	// log.
	void Launch(const TCHAR* TriggerDesc);

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Glider")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Glider")
	TObjectPtr<UVoxelAssetBodyComponent> Body;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Glider")
	TObjectPtr<USceneComponent> CameraArm;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Glider")
	TObjectPtr<UCameraComponent> ChaseCamera;

	UPROPERTY(Transient)
	TWeakObjectPtr<APawn> StoredPawn;
	UPROPERTY(Transient)
	TWeakObjectPtr<APlayerController> Driver;

	// World velocity, UU/s. The one piece of integrated state -- attitude is a
	// control result, not an integration.
	FVector VelocityUU = FVector::ZeroVector;

	// Attitude, held as three doubles rather than read back from the actor so
	// the clamps are applied to the authority and not to a rounded copy.
	double YawDeg = 0.0;
	double PitchDeg = 0.0;
	double RollDeg = 0.0;

	float PitchInput = 0.f;
	float RollInput = 0.f;
	double CameraYawDeg = 0.0;
	double CameraPitchDeg = -8.0;

	// Half the CHORD along +X (B.GetExtent().X -- the craft flies along +X, so
	// this is nose-ward reach, not span), from the asset bounds; the nose point
	// the water check uses.
	double NoseOffsetUU = 200.0;

	// Belly below the origin (negative, the boat's KeelOffsetUU convention) and
	// half the SPAN along +Y, both from the asset bounds. The belly is where a
	// parked glider meets the ground; the span is what a dismounting pilot has
	// to step clear of.
	double BellyOffsetUU = -30.0;
	double HalfSpanUU = 450.0;

	// Parked: resting on the surface, inert -- no aero, no integration, no
	// terrain probes (see the header's HOW IT ENDS). Crewed + parked waits for
	// the launch key.
	bool bParked = false;
	// Space was pressed while parked; consumed by the parked tick. Only ever
	// SET while parked, so a mid-air Space cannot queue a phantom relaunch for
	// the moment the glider next touches down.
	bool bLaunchPressed = false;

	uint64 Ticks = 0;
	uint64 AeroTicks = 0;
	uint64 GroundHits = 0;
	uint64 FlareTicks = 0;
	uint64 ParkTicks = 0;
	double LastAirspeedMS = 0.0;
	double LastAlphaDeg = 0.0;
	double LastGlideRatio = 0.0;
	double LastWindMS = 0.0;
	bool bFinished = false;
};
