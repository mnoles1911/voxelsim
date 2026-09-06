#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
// Included rather than forward-declared: GetBody() below dereferences the
// TObjectPtr in an inline body, which needs the complete type.
#include "VoxelAssetBody.h"
#include "VoxelMovementTuning.h"
#include "VoxelBoat.generated.h"

class APlayerController;
class UCameraComponent;
class UStaticMeshComponent;

// ============================================================================
// A BOAT
// ============================================================================
//
// Phase D of docs/water-ocean-tides-plan-2026-09-04.md: a possessable hull that
// floats on the water datum, is driven with a throttle and a rudder, beaches on
// the voxel ground, and leaves a wake in the ripple field.
//
// --- THE CHASSIS IS AVoxelDebris', AND THAT IS A REAL DECISION --------------
//
// VoxelDebris.cpp:100-175 is the only shipped Chaos body in this project, and
// its shape is forced by a fact about the world rather than by taste: TERRAIN
// HAS NO CHAOS COLLISION AT ALL (doctrine SS3.3, "Chaos only for dynamic debris
// bodies, not per-chunk terrain"). So a rigid body here can carry gravity and
// integrate a transform, and it can do NOTHING with contacts -- every collision
// response is ECR_Ignore, and the ground is found by
// UVoxelWorldSubsystem::RaycastVoxelWorld, per tick, by hand.
//
// A reader coming to this file expecting the boat to bump into things should
// read that paragraph twice: it does not, it cannot, and the bow probe below is
// the entire substitute.
//
// --- WHY CHAOS AT ALL, WHEN EVERY OTHER MOVER HERE IS KINEMATIC -------------
//
// The fly pawn and the character mover are kinematic and the glider (Phase E)
// is too. A boat is the one case where the integrator earns its place: FOUR
// SEPARATE VERTICAL FORCES AT FOUR CORNERS produce pitch and roll for free, and
// pitch and roll ARE what a boat on water looks like. Writing that kinematically
// means writing a rigid-body integrator, and there is one in the box.
//
// --- BUOYANCY READS THE DATUM, NOT THE WAVES --------------------------------
//
// Every probe asks UVoxelWaterSubsystem::WaterSurfaceZAtWorld, which is the
// plan's A5 contract and is DATUM ONLY: the tide moves it, the drawn wind waves
// do not. That is the v1 decision recorded in plan D2 and it is quantified
// there -- drawn WPO is about +-2 cm at reference wind, below the threshold at
// which anyone in a boat could see the hull failing to follow it, while a CPU
// mirror of the wave field would phase-drift against MaterialExpressionTime and
// have the boat riding a crest the water is not on. The same call also backs
// SubmergedDepthUUAtWorld, so a boat and a swimmer cannot disagree about where
// the water is.
//
// THE RIPPLE FIELD IS NOT AN INPUT EITHER, in either direction. VoxelRippleField.h
// :16-20 makes it a material layer and nothing else; this actor WRITES a wake
// into it and never reads one back.
//
// --- THE UNSTREAMED-TILE RULE -----------------------------------------------
//
// A boat can be a hundred metres from the camera over ground that has not
// streamed. Two things keep that safe, and they are different things:
//   * Every water query is a DATUM query -- one number per column out of the
//     baked lake/ocean composition -- so it answers correctly over unstreamed
//     ground. (The terrain raycast does NOT; a miss reads as "no ground found",
//     which is why the beaching test requires a HIT rather than treating a miss
//     as clear water.)
//   * Physics SLEEPS past VoxelBoatTuning::SleepRadiusUU. A boat simulating for
//     nobody is cost with no picture.
//
// --- WAKE, AND ITS STATED BOUND ---------------------------------------------
//
// Per tick, three swept splats: two at the bow shoulders (+-half beam) and one
// at the transom, each swept from last tick's position to this one through
// UVoxelRippleFieldSubsystem::AddSweptDisturbance. The field's constant wave
// speed turns a moving source into a wedge on its own -- there is no wake model
// here, just rings laid along a track.
//
// The bound, stated rather than discovered later: THE RIPPLE FIELD IS 51.2 m
// WIDE AND FOLLOWS THE CAMERA. A possessed boat is always inside it (the field
// centres on the player's pawn, which IS this actor while driving). A boat
// somebody else is watching from 40 m away leaves a wake; a boat 200 m away
// floats wake-less and its splats are counted DroppedOutside. That is correct
// for what the field is for and it is not a bug to be found again in six weeks.
// ============================================================================

UCLASS()
class VOXELEARTH_API AVoxelBoat : public APawn
{
	GENERATED_BODY()

public:
	AVoxelBoat();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	// --- possession (plan D3) -------------------------------------------------

	// The nearest boat to `From` within MaxRangeUU, or null. Static and public
	// so the interact key lives in the player controller (which owns input
	// across possession changes) without that file learning anything about how
	// a boat works.
	static AVoxelBoat* FindNearest(const UWorld* World, const FVector& From, double MaxRangeUU);

	// Scripted throttle (voxel.Boat.Throttle) -- see ScriptThrottle's member
	// comment for why this is not a write to ThrottleInput.
	void SetScriptThrottle(float Value, double Seconds)
	{
		ScriptThrottle = FMath::Clamp(Value, -1.f, 1.f);
		ScriptThrottleUntilS =
			GetWorld() ? GetWorld()->GetTimeSeconds() + FMath::Max(0.0, Seconds) : -1.0;
	}

	// Interact: possess the nearest boat to the controller's current pawn.
	// Returns false and SAYS WHY in the log when there is nothing in range --
	// "I pressed the key and nothing happened" is the one report that costs an
	// evening.
	static bool TryEnterNearest(APlayerController* PC);

	// Take the wheel. Stores the outgoing pawn (hidden, ticking off, collision
	// off) and possesses this boat. False if there is no controller or the boat
	// is already crewed.
	bool Enter(APlayerController* PC);

	// Step out: the stored pawn is put back beside the hull at the waterline and
	// re-possessed. Safe to call when nobody is aboard.
	void ExitToStoredPawn();

	bool IsCrewed() const { return StoredPawn.IsValid(); }

	// --- spawning -------------------------------------------------------------
	//
	// In front of the camera, dropped onto the water surface if the column holds
	// any and onto the ground otherwise. Exists so headless and interactive
	// testing needs no UI work at all (voxel.Boat.Spawn).
	static AVoxelBoat* SpawnAhead(UWorld* World, double AheadUU);

	// --- diagnostics ----------------------------------------------------------
	//
	// Read by voxel.Boat.Stat. Every one of these can be zero for a REASON, and
	// the point of having them separately is that the reasons are different:
	// zero wet probes with a non-zero probe count is a boat in the air; zero
	// probe queries at all is a Tick that is not running.
	uint64 GetProbeQueries() const { return ProbeQueries; }
	uint64 GetProbesWet() const { return ProbesWet; }
	uint64 GetWakeSplats() const { return WakeSplats; }
	uint64 GetSlamSplashes() const { return SlamSplashes; }
	uint64 GetSleepTicks() const { return SleepTicks; }
	uint64 GetGroundedTicks() const { return GroundedTicks; }
	uint64 GetBowBlockTicks() const { return BowBlockTicks; }
	const UVoxelAssetBodyComponent* GetBody() const { return Body; }

private:
	// Hull half-extents in UU, taken from VoxelBoatTuning and then OVERRIDDEN by
	// the loaded asset's own bounds. A boat whose probes sit outside its own hull
	// looks like a physics bug and is not one, so the geometry follows the thing
	// that is actually on screen rather than a constant that agreed with it once.
	void AdoptHullFromBody();

	void TickBuoyancy(float DeltaSeconds);
	void TickDrive(float DeltaSeconds);
	void TickGround(float DeltaSeconds);
	void TickWake(float DeltaSeconds);
	void TickCamera(float DeltaSeconds);

	// Input
	void InputThrottle(float Value);
	void InputSteer(float Value);
	void InputLookYaw(float Value);
	void InputLookPitch(float Value);

	// Root: the Chaos rigid body. An invisible engine cube at scale 1 -- see
	// AVoxelDebris, which keeps its body unscaled for the same reason: a scaled
	// parent would distort every attached instance. The HULL's inertia is
	// supplied through the body's inertia tensor scale instead (BeginPlay), so
	// the boat turns like a 4 m hull rather than like a 1 m box.
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Boat")
	TObjectPtr<UStaticMeshComponent> PhysicsBody;

	// The thing you can see: an entity-lattice .vxa grid as cubes. Never a
	// collision source (see UVoxelAssetBodyComponent).
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Boat")
	TObjectPtr<UVoxelAssetBodyComponent> Body;

	// Camera arm: yaw/pitch relative to the HULL, not to the world. Looking
	// around does not steer, and steering does not swing the camera -- the two
	// are the same stick on most boats and separating them is the difference
	// between "I can see where I am going" and motion sickness.
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Boat")
	TObjectPtr<USceneComponent> CameraArm;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Boat")
	TObjectPtr<UCameraComponent> ChaseCamera;

	UPROPERTY(Transient)
	TWeakObjectPtr<APawn> StoredPawn;
	UPROPERTY(Transient)
	TWeakObjectPtr<APlayerController> Driver;

	// Hull geometry, adopted from the asset (see AdoptHullFromBody).
	double HalfLengthUU = VoxelBoatTuning::HullHalfLengthUU;
	double HalfBeamUU = VoxelBoatTuning::HullHalfBeamUU;
	double KeelOffsetUU = 0.0; // hull bottom relative to the actor origin

	float ThrottleInput = 0.f;
	float SteerInput = 0.f;
	// Scripted throttle for unattended legs (voxel.Boat.Throttle) -- the other
	// half of voxel.Boat.Enter's "drive a boat with no keyboard" promise. A
	// SEPARATE member and not a write to ThrottleInput because the throttle
	// AXIS fires every frame and stomps ThrottleInput back to 0 whenever W is
	// not physically held; a console write into that member survives exactly
	// one tick. The override wins while its clock runs, then input resumes.
	float ScriptThrottle = 0.f;
	double ScriptThrottleUntilS = -1.0;
	double CameraYawDeg = 0.0;
	double CameraPitchDeg = VoxelBoatTuning::CameraDefaultPitchDeg;

	// Wake source points from the previous tick, in world space. Absent on the
	// first tick after a spawn or a wake -- a swept splat from a stale position
	// after a teleport would draw a wake across the whole lake.
	bool bHaveLastWake = false;
	FVector LastBowPort = FVector::ZeroVector;
	FVector LastBowStarboard = FVector::ZeroVector;
	FVector LastTransom = FVector::ZeroVector;

	// Per-probe submerged state, so a hull slam fires on the CROSSING rather
	// than every tick the probe is under (the same bSeen/bWasSubmerged discipline
	// UVoxelRippleFieldSubsystem::AutoWatch uses, and for the same reason: a
	// boat spawned already floating must not splash on its first tick).
	bool bProbeSeen = false;
	bool bProbeWasSubmerged[VoxelBoatTuning::NumProbes] = {};

	bool bAsleep = false;
	bool bGrounded = false;

	uint64 ProbeQueries = 0;
	uint64 ProbesWet = 0;
	uint64 WakeSplats = 0;
	uint64 SlamSplashes = 0;
	uint64 SleepTicks = 0;
	uint64 GroundedTicks = 0;
	uint64 BowBlockTicks = 0;
};
