#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelCoords.h" // VoxelCoords::VoxelSizeUU -- UE-only, voxel-core-free, UHT-safe
#include "VoxelThrownItem.generated.h"

class UProjectileMovementComponent;
class UStaticMeshComponent;

// How a thrown item gets back into an inventory without this file knowing what
// an inventory is. See AVoxelThrownItem::SetInventorySink for the full argument.
// Returns the number of items ACTUALLY accepted, which may be fewer than Count
// and may be zero.
//
// Declared outside the UCLASS on purpose: UHT parses the class body, and a type
// alias in there is one more construct it has to be right about for no benefit.
using FVoxelThrownItemInventorySink = TFunction<int32(AActor* Thrower, FName ItemId, int32 Count)>;

// ============================================================================
// A THROWN VOXEL CUBE -- the first item that leaves the player's hand
// ============================================================================
//
// WHAT IT IS, in one sentence: a 30 cm cube (3x3x3 voxels at 10 cm,
// VoxelCoords.h:23) that the player throws, which SPLASHES if it lands in
// water, RESTS on the ground if it does not, and after five minutes takes
// itself back into the thrower's inventory rather than lying there forever.
//
// It is a sibling of AVoxelExplosive (VoxelExplosive.h:34), not a rewrite of
// it: same UStaticMeshComponent + UProjectileMovementComponent body, same
// Launch() entry point, and -- the important part -- the same answer to the
// problem that dominates every moving object in this project:
//
//   UE COLLISION DOES NOT KNOW ABOUT VOXEL TERRAIN. There is no Chaos body for
//   the ground anywhere (docs/voxel-earth-implementation-plan.md SS3.3, "Chaos
//   only for dynamic debris bodies, not per-chunk terrain"), so a projectile
//   left to UProjectileMovementComponent falls through the planet. The fix,
//   already proven twice (VoxelExplosive.cpp:139-171 and VoxelDebris.cpp:152-
//   170), is to step the voxel DDA along the SEGMENT ACTUALLY TRAVELLED this
//   frame -- UVoxelWorldSubsystem::RaycastVoxelWorld (VoxelWorldSubsystem.h:366)
//   -- and rest in the last empty voxel before the first solid one. Testing the
//   endpoint instead lets a fast throw tunnel through a thin roof between two
//   frames, which is how the "charges do nothing" bug presented.
//
// THE WATER HALF IS THE SAME IDEA APPLIED TO A SURFACE INSTEAD OF A SOLID, and
// it is the reason this class exists today: see the long block above
// UpdateWaterCrossing() below.
//
// --- WHAT THIS ACTOR IS NOT -------------------------------------------------
//
// NOT REPLICATED, and not part of world state. Like AVoxelDebris
// (VoxelDebris.h:18-23) it is presentation plus a local gameplay affordance;
// nothing it does goes through the edit log. In multiplayer the pickup and the
// inventory credit must become server-authoritative before this is anything
// but single-player scaffolding -- see docs/throwables-v0.md SS9.
//
// NOT AN ITEM DEFINITION. It carries an FName item id and a count and hands
// them back to a UVoxelInventoryComponent; what that id MEANS belongs to the
// item system, which is somebody else's file.
//
// NOT WATCHED BY THE RIPPLE FIELD'S AUTO-WATCHER, and that is deliberate.
// UVoxelRippleFieldSubsystem::AutoWatch polls exactly two classes by IsA --
// AVoxelExplosive and AVoxelDebris (VoxelRippleField.cpp:1083) -- so this class
// is invisible to it and there is no risk of two splashes for one entry. If
// anybody ever adds AVoxelThrownItem to that list, DELETE THE HOOK IN THIS FILE
// in the same commit; the poller's estimate is strictly worse (it is a frame
// late and divides a position delta by the frame time, VoxelRippleField.cpp:1062
// and docs/water-interactive-ripples.md SS12.4) and having both would double the
// splash.
// ============================================================================

UCLASS()
class VOXELEARTH_API AVoxelThrownItem : public AActor
{
	GENERATED_BODY()

public:
	AVoxelThrownItem();

	// --- geometry -----------------------------------------------------------
	//
	// 3x3x3 voxels at 10 cm = 30 cm on a side. Drawn as ONE scaled engine cube
	// rather than 27 instances (which is what AVoxelDebris does,
	// VoxelDebris.cpp:107-113): the outer surface of a solid 3x3x3 block IS a
	// 30 cm cube, so 27 instances would draw the same silhouette at 27x the
	// cost. The voxel subdivision would only start to matter if the cube could
	// break apart, which it cannot.
	static constexpr int32 EdgeVoxels = 3;
	static constexpr double EdgeUU = double(EdgeVoxels) * VoxelCoords::VoxelSizeUU; // 30 UU = 0.3 m
	static constexpr double HalfEdgeUU = EdgeUU * 0.5;                              // 15 UU = 0.15 m

	// --- lifetime defaults ---------------------------------------------------

	// The owner's words were "should respawn after 5 minutes if not picked back
	// up by player and put in inventory". Read as RETURN TO INVENTORY; the plain
	// -despawn reading is one console variable away (voxel.Throwable.ReturnToInventory
	// 0) and is written up in docs/throwables-v0.md SS5.
	static constexpr double DefaultReturnSeconds = 300.0;

	// Safety net copied in spirit from AVoxelDebris::MaxFallSeconds
	// (VoxelDebris.h:95): if the DDA never finds ground -- thrown off a cliff
	// into unstreamed space, thrown straight up in a world with no terrain --
	// settle in place rather than fall forever and never start the return timer.
	static constexpr float MaxFlightSeconds = 30.0f;

	// A settled cube cannot be picked up for this long. Without it, a cube
	// dropped at your own feet is back in your inventory on the next tick and
	// the throw looks like it did nothing.
	static constexpr float PickupArmSeconds = 1.0f;

	// Tick rate once settled. A settled item only has to notice the player
	// walking up to it, and 4 Hz is four times faster than a walking pawn can
	// cross the 1.5 m pickup radius. Costs 1/15th of a per-frame check at 60 fps.
	static constexpr float SettledTickIntervalSeconds = 0.25f;

	// --- the throw ------------------------------------------------------------
	//
	// ONE call, not three, so a caller cannot half-initialise a live actor. The
	// verb matches AVoxelExplosive::Launch (VoxelExplosive.h:49) and the velocity
	// argument means the same thing: the FULL initial velocity in UU/s, arc
	// already folded in by the thrower (AVoxelEarthPlayerController::
	// OnChargeRelease builds exactly this, VoxelEarthPlayerController.cpp:664-690).
	//
	// InThrower is whose inventory the item goes back to; normally the pawn. It
	// is held weakly, so a thrower who dies or disconnects turns the timeout into
	// a logged loss rather than a crash.
	void Launch(const FVector& InitialVelocityUUPerSec, AActor* InThrower, FName InItemId,
	            int32 InCount = 1);

	// Spawn + Launch in one call, at a chosen spawn point. Exists so that the
	// player controller's throw and the two console commands in the .cpp are the
	// SAME code path -- if they were two paths, the console command would be a
	// test of the console command.
	static AVoxelThrownItem* SpawnAndThrow(UWorld* World, const FVector& SpawnLocationUU,
	                                       const FVector& VelocityUUPerSec, AActor* InThrower,
	                                       FName InItemId, int32 InCount = 1);

	// One line for a HUD to draw, exactly as voxel.Throwable.Stat prints it:
	//   "Throwables: 2 in flight, 1 settled | thrown 7, splashed 3, picked up 1,
	//    returned 0, LOST 0"
	// The two gauges are counted by walking the actor list rather than kept as
	// running totals, because a running gauge drifts the first time an actor is
	// destroyed by something other than this class (level teardown, PIE stop) and
	// a drifting gauge is worse than no gauge. The three cumulative counters are
	// process-wide and are NOT reset by starting a new PIE session -- read them
	// as "since this editor launched".
	static FString GetStatLine(const UWorld* World);

	// --- how the item gets back into an inventory ----------------------------
	//
	// THE PROBLEM THIS SOLVES: UVoxelInventoryComponent is another agent's file
	// and does not exist in this tree yet, so this .cpp cannot name its type
	// unconditionally without breaking the build for everyone until it lands.
	// Two mechanisms, in priority order, both in one function (GiveBackToThrower):
	//
	//   1. THIS SINK, if something has installed one. A single line in the
	//      player controller or the game mode binds it to whatever the real
	//      inventory API turns out to be, and this file never has to be edited
	//      again if that API's shape differs from the guess below. Signature:
	//      (Thrower, ItemId, Count) -> the number ACTUALLY accepted.
	//   2. A DIRECT UVoxelInventoryComponent::TryAddItem CALL, compiled only
	//      when "VoxelInventoryComponent.h" exists next to this file
	//      (__has_include). See the .cpp; the assumed signature is
	//      int32 TryAddItem(FName ItemId, int32 Count).
	//
	// With neither, a timeout is a LOGGED LOSS rather than a silent delete --
	// which is the whole point of counting it.
	static void SetInventorySink(FVoxelThrownItemInventorySink InSink);
	static bool HasInventorySink();

	FName GetItemId() const { return ItemId; }
	int32 GetCount() const { return Count; }
	bool IsSettled() const { return bSettled; }

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// ------------------------------------------------------------------------
	// THE WATER CROSSING -- today's deliverable, and the part with a real trap
	// ------------------------------------------------------------------------
	//
	// WHAT IS WRONG WITH "AM I UNDERWATER NOW". The obvious hook is one
	// UVoxelWaterSubsystem::IsUnderwaterAtWorld (VoxelWaterSubsystem.h:347) per
	// frame on the actor's centre, firing when it flips false->true. It is what
	// the ripple field's own poller does (VoxelRippleField.cpp:1050) and what the
	// patch note in docs/water-interactive-ripples.md SS8.5 writes out, and for a
	// grenade arcing into a deep lake it is right. It is WRONG HERE in two ways,
	// and both of them fail SILENTLY -- no splash, no log, no counter:
	//
	//   1. IT IS A POINT TEST AT DISCRETE TIMES. A cube thrown at the top of this
	//      project's speed range (1600 UU/s = 16 m/s,
	//      VoxelEarthPlayerController.h:148) covers 26.7 cm in a 60 fps frame and
	//      53.3 cm in a 30 fps one -- 2.7 and 5.3 VOXELS. Water thinner than the
	//      step is stepped clean over: a 30 cm puddle, the shallow lip of a lake
	//      shelf, or a river the CA has filled two voxels deep. Both endpoints are
	//      honestly dry and the object went straight through the wet part.
	//   2. IT ASKS ABOUT THE WRONG POINT OF THE OBJECT. The cube is 30 cm; its
	//      centre is 15 cm above its underside. A cube skimming the surface wets
	//      its bottom face and never its centre. (VoxelCharacterMovement.cpp:630
	//      makes the opposite choice for the opposite reason -- it asks whether
	//      the character's TOP is under water, because it is deciding "am I
	//      swimming", i.e. FULLY in. We are deciding "did I touch", i.e. FIRST
	//      contact, so we ask about the bottom face.)
	//
	// WHAT THIS DOES INSTEAD. Walk the segment the cube's BOTTOM CENTRE travelled
	// this frame at one sample per 10 cm -- one voxel, and also exactly one texel
	// of the ripple field (VoxelRippleField.h:177, kTexelUU = 10.0) -- and fire on
	// the first dry->wet sample. That is the same "test the segment, not the
	// endpoint" rule the terrain sweep above already lives by, applied to a
	// surface instead of a solid.
	//
	// NO SUB-SAMPLE REFINEMENT, ON PURPOSE. Bisecting the 10 cm interval to find
	// the exact crossing costs four more water queries and buys accuracy finer
	// than one texel of a 10 cm field that then discards the caller's Z entirely
	// (VoxelRippleField.h:222-224). It would be precision the consumer cannot
	// represent.
	//
	// COST, BOUNDED. IsUnderwaterAtWorld is a CA cell read plus, on a miss, one
	// implicit worldgen column, and VoxelWaterSubsystem.cpp:6999 says in as many
	// words "do not call this one per cell". So the sample count is capped at
	// kMaxWaterSamples (16): typical flight is 3 samples per frame, the cap is
	// only reached above 1.6 m of travel in one frame (a 96 m/s object at 60 fps,
	// 6x anything this game can throw), and past that the step widens beyond a
	// voxel and thin water CAN be missed again -- logged once, rather than
	// pretended away. Sampling stops entirely after the splash fires and while
	// settled, so a lake full of resting cubes costs nothing.
	//
	// ONE SPLASH PER ENTRY. bWasUnderwater is edge-triggered, so being submerged
	// is not an event; only becoming submerged is. A cube that leaves the water
	// and re-enters splashes again, which is correct. A cooldown backs that up in
	// case a future buoyancy pass makes the bottom face oscillate across the
	// surface -- an object re-triggering every frame is the standard way this
	// effect turns into a mess, and it is cheaper to make it impossible now than
	// to diagnose it later.
	void UpdateWaterCrossing(const FVector& SegStartBottomUU, const FVector& SegEndBottomUU);

	// Fire the splash at a crossing point. Everything about the disturbance's
	// SIZE is derived here from the cube and its speed; see the long comment on
	// the implementation for the arithmetic and for which endpoints were chosen
	// rather than derived.
	void Splash(const FVector& CrossPointUU);

	// Come to rest: stop the projectile, sit on the surface, slow the tick down
	// and start the return timer.
	void Settle(const FVector& RestLocationUU, const TCHAR* Reason);

	// The return timer fired. Hands the item back (or does not) and destroys the
	// actor either way -- see docs/throwables-v0.md SS5.
	void OnReturnTimerExpired();

	// Proximity pickup against the thrower, on the slow settled tick.
	void TryProximityPickup();

	// The one place that talks to an inventory. Returns how many of Count were
	// actually accepted (0 if there is no inventory at all), and never throws
	// away the caller's decision about what to do with the remainder.
	int32 GiveBackToThrower(const TCHAR* Reason);

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Throwable")
	TObjectPtr<UStaticMeshComponent> CubeMesh;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Throwable")
	TObjectPtr<UProjectileMovementComponent> Projectile;

	// What this cube is, in the item system's terms. FName rather than a struct
	// so this class has no compile dependency on the item system at all.
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Throwable")
	FName ItemId;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Throwable")
	int32 Count = 1;

	// Weak: a thrown item must never keep a pawn alive, and a thrower who is gone
	// by the time the timer fires is a logged loss, not a crash.
	TWeakObjectPtr<AActor> Thrower;

	// Where the cube's CENTRE was at the end of the previous tick -- the terrain
	// sweep's start point, exactly as AVoxelExplosive::LastTickLocationUU
	// (VoxelExplosive.h:86). The water sweep uses the same segment shifted down
	// by HalfEdgeUU; see UpdateWaterCrossing.
	FVector LastTickLocationUU = FVector::ZeroVector;

	// Water-crossing edge state. bHaveWaterState guards the first tick: an item
	// that is ALREADY in water when it is thrown (thrown while standing waist
	// deep) must not read as a fresh entry on the frame it appears. The ripple
	// field's own watcher needs the identical guard and calls it bSeen
	// (VoxelRippleField.h:320).
	bool bHaveWaterState = false;
	bool bWasUnderwater = false;
	bool bSplashed = false;
	float LastSplashTimeSeconds = -1000.0f;

	bool bSettled = false;
	bool bItemAccountedFor = false; // returned, lost, or picked up -- exactly once
	float FlightSeconds = 0.0f;
	float SettledSeconds = 0.0f;

	// "Said once" for the two inventory failures (nothing accepted it; nothing
	// was reachable at all). Per ITEM rather than per process: two different
	// items failing for two different reasons are two diagnoses, but one item
	// failing 240 times while the player stands next to it is one, and the
	// settled tick runs at 4 Hz.
	bool bLoggedInventoryFull = false;

	FTimerHandle ReturnTimerHandle;
};
