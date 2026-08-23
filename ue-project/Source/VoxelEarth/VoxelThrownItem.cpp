#include "VoxelThrownItem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h" // TActorIterator, for the stat line's two gauges
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "VoxelEarth.h" // LogVoxelEarth
#include "VoxelRippleField.h"
#include "VoxelWaterSubsystem.h"
#include "VoxelWorldSubsystem.h"

// THE INVENTORY DEPENDENCY, AND WHY IT IS BEHIND __has_include ---------------
//
// UVoxelInventoryComponent belongs to the item-system work, which was being
// written in parallel with this file on 2026-08-13. It was not in the tree when
// this class was designed and it WAS by the time this class was finished, so
// both states have to work: naming the type unconditionally would have meant
// this file could not compile until that one landed, which is a bad trade for a
// class whose first job -- the splash -- has nothing to do with inventories.
//
// So: compile the direct call only when the header is actually there, and give
// the runtime a sink (SetInventorySink) that needs no compile dependency at
// all. Both paths meet in GiveBackToThrower, which is the ONE place in this
// file that talks to an inventory.
//
// SIGNATURE VERIFIED, NOT ASSUMED. VoxelInventoryComponent.h:89 is
// `int32 TryAddItem(FName ItemId, int32 Count)` returning how many were actually
// accepted, which is exactly what this file was written against; its own comment
// (:71-75) names AVoxelThrownItem as one of the callers the signature is fixed
// for. NOTE WHERE THE COMPONENT LIVES: on the local PLAYER CONTROLLER, attached
// by UVoxelInventoryBootstrapSubsystem (VoxelInventoryComponent.h:205-218), NOT
// on the pawn. That is why GiveBackToThrower hops pawn -> controller instead of
// looking in one place; a thrower that is the pawn -- which is what the proximity
// pickup needs it to be -- would otherwise find nothing.
#if defined(__has_include)
#	if __has_include("VoxelInventoryComponent.h")
#		define VOXELTHROWNITEM_HAS_INVENTORY 1
#		include "VoxelInventoryComponent.h"
#	endif
#endif
#ifndef VOXELTHROWNITEM_HAS_INVENTORY
#	define VOXELTHROWNITEM_HAS_INVENTORY 0
#endif

namespace
{
// ---------------------------------------------------------------------------
// CONSOLE VARIABLES
// ---------------------------------------------------------------------------

TAutoConsoleVariable<float> CVarThrowableReturnSeconds(
	TEXT("voxel.Throwable.ReturnSeconds"),
	static_cast<float>(AVoxelThrownItem::DefaultReturnSeconds),
	TEXT("Seconds a settled thrown item waits on the ground before it goes back into the ")
	TEXT("thrower's inventory (or despawns -- see voxel.Throwable.ReturnToInventory). 300 s is the ")
	TEXT("owner's five minutes. READ AT SETTLE TIME, not at expiry, so changing it affects items ")
	TEXT("that settle from now on and not the ones already lying about. 0 or less means never, ")
	TEXT("which is a debugging state and not a shipping one -- items then accumulate for the life ")
	TEXT("of the level."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarThrowableReturnToInventory(
	TEXT("voxel.Throwable.ReturnToInventory"), true,
	TEXT("What the five-minute timer DOES. 1 (default) puts the item back in the thrower's ")
	TEXT("inventory and destroys the world actor; 0 just destroys it. This is the one switch ")
	TEXT("between the two readings of 'should respawn after 5 minutes if not picked back up' -- ")
	TEXT("see docs/throwables-v0.md SS5. Either way the item is ACCOUNTED FOR in ")
	TEXT("voxel.Throwable.Stat: nothing is ever silently deleted."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarThrowablePickupRadiusM(
	TEXT("voxel.Throwable.PickupRadiusM"), 1.5f,
	TEXT("How close the thrower must get to a settled item to pick it up, in metres, centre to ")
	TEXT("centre. v0 is a plain distance check on a 4 Hz tick against the THROWER ONLY -- not an ")
	TEXT("overlap volume, not a look-at test, and not available to anyone else. 1.5 m is roughly ")
	TEXT("arm's reach plus the pawn's own half-width."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarThrowableSplashScale(
	TEXT("voxel.Throwable.SplashScale"), 1.0f,
	TEXT("Multiplier on the ripple HEIGHT a thrown item makes when it enters water. The SHAPE of ")
	TEXT("the mapping -- radius and strength from the cube's own size and its impact speed -- is ")
	TEXT("fixed in code and deliberately not a set of knobs; this is the single level control. ")
	TEXT("Two knobs for one effect is how a tuning session stops converging ")
	TEXT("(VoxelRippleField.cpp:138-141 makes the same argument about the impact ramp). 0 disables ")
	TEXT("the splash and shows up as 'inert' in voxel.Water.Ripple.Stat, not as silence."),
	ECVF_Default);

// --- the impact ramp, restated rather than shared ---------------------------
//
// The impact-strength curve is shared with the ripple field through
// VoxelRipple::kFullImpactSpeedMPS / VoxelRipple::kMinImpactFraction (VoxelRippleField.h).
// This file used to carry a BY-VALUE COPY of both, with a comment citing the
// drift hazard; the copy never drifted, but a unity blob of the game target
// merged it with the original and the module stopped compiling (2026-08-23).

// A crossing may not fire again for this long. bWasUnderwater is already
// edge-triggered so this is insurance, not the mechanism -- see the header's
// "ONE SPLASH PER ENTRY". 0.5 s is longer than any plausible bob and far shorter
// than any real throw-to-throw interval.
constexpr float kSplashCooldownSeconds = 0.5f;

// Cap on water queries per item per frame. See the header's COST note: 16
// samples at one voxel apiece covers 1.6 m of travel in one frame, which is 6x
// the 26.7 cm a maximum-speed throw covers at 60 fps.
constexpr int32 kMaxWaterSamples = 16;

// How much of its velocity a cube keeps on entering water. Water is ~800x the
// density of air and a 30 cm bluff body has no business keeping its speed
// through the surface; without this the cube is at the lake bed within two
// frames of the splash and the player never sees the thing that made the ring.
// A single multiply on entry, not a drag model -- there is no buoyancy anywhere
// in this project yet (docs/water-architecture.md), so a drag model would be the
// only physics in the water and would look stranger than none.
constexpr double kWaterEntrySpeedRetained = 0.35;

// --- process-wide counters for voxel.Throwable.Stat -------------------------
//
// Cumulative, and NOT reset between PIE sessions -- read them as "since this
// editor launched". The two live gauges (in flight, settled) are counted by
// walking the actor list instead, in GetStatLine, because a gauge maintained by
// increment/decrement drifts the first time an actor dies by a route this class
// does not own (level teardown, PIE stop, world travel) and a drifting gauge is
// worse than no gauge at all.
int64 GThrown = 0;
int64 GSplashed = 0;
int64 GPickedUp = 0;
int64 GReturned = 0;
int64 GLost = 0;

FVoxelThrownItemInventorySink& SinkRef()
{
	// Function-local static: this is set from another translation unit (the
	// player controller), and a file-scope static would put its construction in
	// the middle of static-init order.
	static FVoxelThrownItemInventorySink Sink;
	return Sink;
}

// The item id the console commands throw. This is the REAL registered id --
// VoxelItem.cpp:193, `throwable.voxelcube30`, CubeEdgeMetres 0.3, MaxStack 16,
// ThrowSpeedUUPerSec 1200 -- not a placeholder, so a console throw that times
// out five minutes later lands in the inventory as a genuine item and the whole
// chain is exercised rather than half of it. Its 0.3 m edge and this class's
// 3x3x3 voxels at 10 cm are the same 30 cm, arrived at independently; see
// docs/throwables-v0.md SS8.3 for the wiring that should eventually make one of
// them the authority over the other instead of two agreeing numbers.
const FName kThrowCubeItemId(TEXT("throwable.voxelcube30"));

// ---------------------------------------------------------------------------
// CONSOLE COMMANDS -- and why the second one is the one that matters
// ---------------------------------------------------------------------------
//
// Verification on this project is by screenshot at a pinned pose and the owner
// judges the screenshot (docs/water-architecture.md SS4, and the same argument
// is made at VoxelRippleField.cpp:151-161 about voxel.Water.Ripple.Drop). An
// effect that only happens when a player presses a key cannot be photographed by
// a headless capture at all: there is no player and there is no frame at which
// the state is reproducibly the same.
//
//   ThrowHere  convenient, aims at the crosshair, NOT reproducible -- the
//              result depends on where the camera is standing and pointing.
//   DropAt     deterministic: an exact world position and an exact downward
//              speed, so the crossing point, the impact fraction, the radius and
//              the strength are all decided by the arguments and nothing else.
//              This is what makes a splash photographable.
//
// NEITHER OF THEM FREEZES THE RIPPLE FIELD, and they cannot: the splash happens
// some frames after the spawn, and by then the capture has to be waiting for it.
// For a pixel-identical A/B of the FIELD, voxel.Water.Ripple.Drop + Steps +
// Freeze is still the right tool. These commands prove that THIS ACTOR fires the
// hook with the right numbers -- the log line and voxel.Water.Ripple.Stat are
// their instruments. docs/throwables-v0.md SS6 has the exact sequence.

FAutoConsoleCommandWithWorldAndArgs GThrowableThrowHereCmd(
	TEXT("voxel.Throwable.ThrowHere"),
	TEXT("voxel.Throwable.ThrowHere [SpeedUU=1200] [PitchDeg=0] [Count=1] -- throw one 30 cm voxel ")
	TEXT("cube from the local player's camera along the crosshair, with no keypress and no ")
	TEXT("inventory. PitchDeg is added to the camera pitch (the F-key throw uses +30, ")
	TEXT("VoxelEarthPlayerController.h:149); 0 means straight at what you are looking at. Aim, ")
	TEXT("throw, then read voxel.Water.Ripple.Stat. NOT reproducible -- use ")
	TEXT("voxel.Throwable.DropAt for anything that will be photographed."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
			if (!PC)
			{
				UE_LOG(LogVoxelEarth, Warning,
				       TEXT("Throwable.ThrowHere: no local player controller in this world."));
				return;
			}
			const float SpeedUU = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 1200.0f;
			const float PitchDeg = (Args.Num() > 1) ? FCString::Atof(*Args[1]) : 0.0f;
			const int32 ItemCount = (Args.Num() > 2) ? FMath::Max(1, FCString::Atoi(*Args[2])) : 1;

			FVector CameraLocation;
			FRotator CameraRotation;
			PC->GetPlayerViewPoint(CameraLocation, CameraRotation);
			FRotator ThrowRotation = CameraRotation;
			ThrowRotation.Pitch = FMath::Clamp(CameraRotation.Pitch + PitchDeg, -89.0f, 89.0f);
			const FVector Dir = ThrowRotation.Vector();

			// 80 UU in front of the camera, the same offset the F-key throw uses
			// (VoxelEarthPlayerController.cpp:681), so the cube does not spawn
			// inside the pawn's own head.
			AVoxelThrownItem* Item = AVoxelThrownItem::SpawnAndThrow(
				World, CameraLocation + Dir * 80.0, Dir * SpeedUU, PC->GetPawn(), kThrowCubeItemId,
				ItemCount);
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("Throwable.ThrowHere: %s at %.0f UU/s, pitch %+.1f deg, from (%.0f, %.0f, %.0f)."),
			       Item ? TEXT("thrown") : TEXT("FAILED to spawn"), SpeedUU, PitchDeg,
			       CameraLocation.X, CameraLocation.Y, CameraLocation.Z);
		}));

FAutoConsoleCommandWithWorldAndArgs GThrowableDropAtCmd(
	TEXT("voxel.Throwable.DropAt"),
	TEXT("voxel.Throwable.DropAt <XUU> <YUU> <ZUU> [DownSpeedUU=0] [Count=1] -- spawn one 30 cm ")
	TEXT("voxel cube at an exact world position (UE UNITS, the numbers GetActorLocation prints) ")
	TEXT("moving straight down at an exact speed, with no thrower. THE DETERMINISTIC ONE: the ")
	TEXT("crossing point and the impact speed are decided by these arguments alone, so the same ")
	TEXT("command makes the same ring every time. Drop it 2-3 m above a known lake surface and ")
	TEXT("within 25.6 m of the camera, or the ripple window will not contain it -- the splash log ")
	TEXT("line says so explicitly when that happens. With no thrower the five-minute timer has ")
	TEXT("nowhere to return the item to and logs it as lost, which is expected for a test drop."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (!World || Args.Num() < 3)
			{
				UE_LOG(LogVoxelEarth, Warning,
				       TEXT("Throwable.DropAt: usage is voxel.Throwable.DropAt <XUU> <YUU> <ZUU> ")
				       TEXT("[DownSpeedUU] [Count]"));
				return;
			}
			const FVector P(FCString::Atod(*Args[0]), FCString::Atod(*Args[1]),
			                FCString::Atod(*Args[2]));
			const double DownUU = (Args.Num() > 3) ? FCString::Atod(*Args[3]) : 0.0;
			const int32 ItemCount = (Args.Num() > 4) ? FMath::Max(1, FCString::Atoi(*Args[4])) : 1;

			AVoxelThrownItem* Item = AVoxelThrownItem::SpawnAndThrow(
				World, P, FVector(0.0, 0.0, -FMath::Abs(DownUU)), nullptr, kThrowCubeItemId, ItemCount);
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("Throwable.DropAt: %s at (%.0f, %.0f, %.0f) UU falling at %.0f UU/s ")
			       TEXT("(%.2f m/s; the impact ramp saturates at %.1f m/s)."),
			       Item ? TEXT("dropped") : TEXT("FAILED to spawn"), P.X, P.Y, P.Z,
			       FMath::Abs(DownUU), FMath::Abs(DownUU) / 100.0, VoxelRipple::kFullImpactSpeedMPS);
		}));

FAutoConsoleCommandWithWorldAndArgs GThrowableStatCmd(
	TEXT("voxel.Throwable.Stat"),
	TEXT("Thrown-item counters: how many are in the air, how many are lying settled, and the ")
	TEXT("cumulative thrown / splashed / picked up / returned / lost. LOST IS THE ONE TO READ: it ")
	TEXT("counts items the five-minute timer could not give back (no thrower, no inventory, or a ")
	TEXT("full one) and destroyed anyway. Cumulative counters are per editor process, not per PIE ")
	TEXT("session. If splashed is 0 after a throw into a lake, the crossing never fired and the ")
	TEXT("problem is here; if splashed is nonzero and you see nothing, the problem is downstream ")
	TEXT("and voxel.Water.Ripple.Stat separates the four reasons."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& /*Args*/, UWorld* World)
		{
			UE_LOG(LogVoxelEarth, Log, TEXT("%s"), *AVoxelThrownItem::GetStatLine(World));
		}));
} // namespace

// ============================================================================

AVoxelThrownItem::AVoxelThrownItem()
{
	// Ticks so it can resolve its own collision against the voxel world and its
	// own crossing of the water surface -- the two things UE cannot do for it.
	// See the class comment. Once settled the tick interval drops to 4 Hz.
	PrimaryActorTick.bCanEverTick = true;

	CubeMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CubeMesh"));
	SetRootComponent(CubeMesh);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		CubeMesh->SetStaticMesh(CubeFinder.Object);
	}
	// BasicShapes/Cube is 100 UU on a side, so EdgeUU/100 = 0.3 makes exactly a
	// 3x3x3-voxel cube. AVoxelDebris does the same arithmetic per instance
	// (VoxelDebris.cpp:103).
	CubeMesh->SetRelativeScale3D(FVector(EdgeUU / 100.0));
	CubeMesh->SetCastShadow(true);

	// NO COLLISION AT ALL, unlike AVoxelExplosive's "Projectile" profile
	// (VoxelExplosive.cpp:60). Three reasons, and none of them is tidiness:
	//   * Terrain has no Chaos body, so the only collision that would ever fire
	//     is against the pawn and other actors -- and a thrown pickup that shoves
	//     the player around is a bug, not a feature.
	//   * The ground contact that MATTERS is the voxel DDA in Tick, which does
	//     not consult Chaos.
	//   * v0 pickup is a distance check, not an overlap, so there is nothing an
	//     overlap volume would be used for yet. When pickup becomes an overlap
	//     (docs/throwables-v0.md SS9) this is the line to change.
	CubeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CubeMesh->SetCanEverAffectNavigation(false);

	Projectile = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Projectile"));
	Projectile->UpdatedComponent = CubeMesh;
	Projectile->ProjectileGravityScale = 1.0f;
	// No bounce: with no terrain collision there is nothing to bounce off, and
	// AVoxelExplosive's bBounce is honest only about other physics actors
	// (VoxelExplosive.cpp:66-71). A cube that bounces off the voxel surface is a
	// real want and it is a real feature -- reflect the velocity about the hit
	// voxel's face normal in the DDA branch below -- but it is v1, listed in
	// docs/throwables-v0.md SS9.
	Projectile->bShouldBounce = false;
	Projectile->bRotationFollowsVelocity = false; // a cube tumbling is v1 too
	Projectile->bAutoActivate = false;            // activated explicitly by Launch()
}

void AVoxelThrownItem::BeginPlay()
{
	Super::BeginPlay();

	// A pale tint so a grey cube on grey rock is findable. Best-effort and
	// deliberately silent on failure, exactly as AVoxelExplosive::BeginPlay
	// (VoxelExplosive.cpp:80-97): the parameter names on the engine's default
	// BasicShapeMaterial are not a documented contract, so this either tints or
	// does nothing, and never fails.
	if (CubeMesh)
	{
		if (UMaterialInterface* BaseMaterial = CubeMesh->GetMaterial(0))
		{
			if (UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMaterial, this))
			{
				const FLinearColor Tint(0.62f, 0.48f, 0.28f); // crate-ish tan
				MID->SetVectorParameterValue(TEXT("Color"), Tint);
				MID->SetVectorParameterValue(TEXT("BaseColor"), Tint);
				CubeMesh->SetMaterial(0, MID);
			}
		}
	}
}

void AVoxelThrownItem::Launch(const FVector& InitialVelocityUUPerSec, AActor* InThrower,
                              FName InItemId, int32 InCount)
{
	Thrower = InThrower;
	ItemId = InItemId;
	Count = FMath::Max(1, InCount);

	if (Projectile)
	{
		Projectile->Activate();
		Projectile->Velocity = InitialVelocityUUPerSec;
	}

	LastTickLocationUU = GetActorLocation();

	// PRIME THE WATER EDGE STATE HERE, not on the first tick. An item thrown
	// while the thrower is standing waist-deep in a lake starts submerged, and
	// without this it would read as a fresh entry on its first tick and splash at
	// the thrower's feet for no reason. The ripple field's own watcher needs the
	// identical guard and calls it bSeen (VoxelRippleField.h:318-321) -- it was
	// written there because debris promoted from a lake bed did exactly this.
	// Non-const UWorld* deliberately: UWorld::GetSubsystem is not a const member
	// in this engine version, which is why UVoxelRippleFieldSubsystem::
	// AddDisturbanceAt has to const_cast its way to the same call
	// (VoxelRippleField.cpp:569-577). Holding the pointer non-const here avoids
	// repeating that.
	if (UWorld* World = GetWorld())
	{
		if (const UVoxelWaterSubsystem* Water = World->GetSubsystem<UVoxelWaterSubsystem>())
		{
			bWasUnderwater =
				Water->IsUnderwaterAtWorld(LastTickLocationUU - FVector(0.0, 0.0, HalfEdgeUU));
			bHaveWaterState = true;
		}
	}

	++GThrown;
	UE_LOG(LogVoxelEarth, Verbose,
	       TEXT("VoxelThrownItem launched: item='%s' x%d from (%.0f, %.0f, %.0f) at %.0f UU/s, ")
	       TEXT("thrower=%s, starts %s."),
	       *ItemId.ToString(), Count, LastTickLocationUU.X, LastTickLocationUU.Y,
	       LastTickLocationUU.Z, InitialVelocityUUPerSec.Size(),
	       InThrower ? *InThrower->GetName() : TEXT("<none>"),
	       bWasUnderwater ? TEXT("SUBMERGED (no entry splash will fire until it leaves)")
	                      : TEXT("in air"));
}

AVoxelThrownItem* AVoxelThrownItem::SpawnAndThrow(UWorld* World, const FVector& SpawnLocationUU,
                                                  const FVector& VelocityUUPerSec,
                                                  AActor* InThrower, FName InItemId, int32 InCount)
{
	if (!World)
	{
		return nullptr;
	}
	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = InThrower;
	SpawnParams.Instigator = Cast<APawn>(InThrower);
	// AlwaysSpawn for the same reason the explosive throw uses it
	// (VoxelEarthPlayerController.cpp:686): the spawn point is 80 UU in front of
	// a camera that may be up against a wall, and a throw that silently does not
	// happen is the worst possible outcome.
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AVoxelThrownItem* Item = World->SpawnActor<AVoxelThrownItem>(
		AVoxelThrownItem::StaticClass(), SpawnLocationUU, FRotator::ZeroRotator, SpawnParams);
	if (Item)
	{
		Item->Launch(VelocityUUPerSec, InThrower, InItemId, InCount);
	}
	return Item;
}

void AVoxelThrownItem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bSettled)
	{
		// The slow 4 Hz tick. DeltaSeconds here is the accumulated interval, not
		// a frame, which is why nothing below divides by it.
		SettledSeconds += DeltaSeconds;
		TryProximityPickup();
		return;
	}

	FlightSeconds += DeltaSeconds;

	const FVector Now = GetActorLocation();
	const FVector Segment = Now - LastTickLocationUU;
	const double Travelled = Segment.Size();

	if (Travelled > KINDA_SMALL_NUMBER)
	{
		// WATER BEFORE TERRAIN, and the order is load-bearing. A cube entering a
		// shallow lake can cross the surface AND reach the bed inside one frame;
		// the terrain branch below returns early when it settles, so testing the
		// surface second would lose the splash in exactly the case -- shallow
		// water -- where the splash is the whole visible event.
		UpdateWaterCrossing(LastTickLocationUU - FVector(0.0, 0.0, HalfEdgeUU),
		                    Now - FVector(0.0, 0.0, HalfEdgeUU));

		UWorld* World = GetWorld();
		UVoxelWorldSubsystem* Voxels = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
		if (Voxels)
		{
			// The swept voxel test, copied from AVoxelExplosive::Tick
			// (VoxelExplosive.cpp:139-171) because it is the single thing that
			// makes a moving object work in this project. Testing the segment
			// rather than the endpoint is what stops a fast cube tunnelling
			// through a thin roof between two frames.
			FVector HitVoxelCenter, PrevVoxelCenter;
			if (Voxels->RaycastVoxelWorld(LastTickLocationUU, Segment, Travelled, HitVoxelCenter,
			                              PrevVoxelCenter))
			{
				// FLOOR OR WALL? The DDA steps one axis at a time, so the hit
				// voxel and the last empty voxel differ on exactly one axis. If
				// the hit is the one BELOW, we landed on something; anything else
				// is a wall or a ceiling and the cube should keep falling rather
				// than stick to it in mid-air, which is what a naive "settle on
				// any hit" does to a cube thrown at a cliff face.
				const bool bFloorHit = HitVoxelCenter.Z < PrevVoxelCenter.Z - 1.0;
				if (bFloorHit)
				{
					// Rest the cube's UNDERSIDE on the top face of the voxel it
					// hit. AVoxelExplosive parks its centre at the last empty
					// voxel's centre, which is right for a 15 UU sphere and wrong
					// for a 30 UU cube: that would bury the bottom 10 UU of it in
					// the ground. AVoxelDebris does the same top-face arithmetic
					// for the same reason (VoxelDebris.cpp:163).
					const double SurfaceTopZ = HitVoxelCenter.Z + VoxelCoords::VoxelSizeUU * 0.5;
					FVector Rest = PrevVoxelCenter;
					Rest.Z = SurfaceTopZ + HalfEdgeUU;
					// XY snapped to the voxel lattice is not a rounding error
					// here, it is the right look: this is a voxel cube in a voxel
					// world and it should sit square on the grid.
					Settle(Rest, TEXT("landed"));
					return;
				}

				// Wall or ceiling: put the cube in the last empty voxel and remove
				// only the component of velocity heading INTO the surface, so it
				// slides down a cliff instead of stopping dead against it.
				const FVector IntoSolid = (HitVoxelCenter - PrevVoxelCenter).GetSafeNormal();
				if (Projectile && !IntoSolid.IsNearlyZero())
				{
					const double IntoSpeed = Projectile->Velocity | IntoSolid;
					if (IntoSpeed > 0.0)
					{
						Projectile->Velocity -= IntoSolid * IntoSpeed;
					}
				}
				SetActorLocation(PrevVoxelCenter);
				LastTickLocationUU = PrevVoxelCenter;
				return;
			}
		}
	}

	if (FlightSeconds >= MaxFlightSeconds)
	{
		// Never found ground. Over solid terrain this should not happen; over
		// unstreamed space it can, and a cube falling forever never starts its
		// return timer and is therefore lost silently -- which is the failure
		// mode this whole class is supposed to not have. Same safety net as
		// AVoxelDebris::MaxFallSeconds (VoxelDebris.cpp:172-177).
		Settle(Now, TEXT("flight timeout -- no ground found"));
		return;
	}

	LastTickLocationUU = Now;
}

// ---------------------------------------------------------------------------
// WATER
// ---------------------------------------------------------------------------

void AVoxelThrownItem::UpdateWaterCrossing(const FVector& SegStartBottomUU,
                                           const FVector& SegEndBottomUU)
{
	// NOTE ON COST: this keeps sampling while the cube sinks, rather than
	// stopping at the first splash. That is 1-3 water queries per frame for the
	// fraction of a second between the surface and the lake bed, and it is what
	// makes a re-entry (out of the water and back in) fire honestly instead of
	// being swallowed by a latch. A SETTLED cube costs nothing at all, because
	// Tick returns before ever reaching this function.
	UWorld* World = GetWorld();
	const UVoxelWaterSubsystem* Water =
		World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		return; // no water in this run; nothing can be entered
	}

	const FVector Segment = SegEndBottomUU - SegStartBottomUU;
	const double Travelled = Segment.Size();

	// One sample per voxel (10 cm), which is also exactly one texel of the ripple
	// field (VoxelRippleField.h:177). Capped -- see the header's COST note.
	// Clamped in DOUBLE before the narrowing conversion. This is a
	// double-precision LWC world (VoxelCoords.h:11-14) where an actor can in
	// principle be moved a planetary distance in one frame, and FMath::CeilToInt
	// on 2.1e10 is not a large number, it is an undefined one.
	const double WantedD = FMath::CeilToDouble(Travelled / VoxelCoords::VoxelSizeUU);
	const int32 Wanted = static_cast<int32>(FMath::Clamp(WantedD, 1.0, 1.0e6));
	const int32 Samples = FMath::Clamp(Wanted, 1, kMaxWaterSamples);
	if (Wanted > kMaxWaterSamples)
	{
		// Said once per process. Above 1.6 m of travel in one frame the sample
		// step is coarser than a voxel and water thinner than that step can be
		// stepped over again -- the exact defect this function exists to remove,
		// reappearing at 6x the game's maximum throw speed. Worth knowing about;
		// not worth spending 40 water queries a frame to prevent.
		static bool bLoggedSampleCap = false;
		if (!bLoggedSampleCap)
		{
			bLoggedSampleCap = true;
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("VoxelThrownItem: a %.0f UU frame step wanted %d water samples; capped at ")
			       TEXT("%d, so the step is %.1f UU (>1 voxel) and water thinner than that could be ")
			       TEXT("missed. Said once."),
			       Travelled, Wanted, kMaxWaterSamples, Travelled / double(kMaxWaterSamples));
		}
	}

	for (int32 I = 1; I <= Samples; ++I)
	{
		const FVector P = SegStartBottomUU + Segment * (double(I) / double(Samples));
		const bool bWet = Water->IsUnderwaterAtWorld(P);

		if (bWet && !bWasUnderwater && bHaveWaterState)
		{
			const float NowSeconds = World->GetTimeSeconds();
			if (NowSeconds - LastSplashTimeSeconds >= kSplashCooldownSeconds)
			{
				LastSplashTimeSeconds = NowSeconds;
				Splash(P);
			}
		}
		bWasUnderwater = bWet;
		bHaveWaterState = true;

		if (bWet)
		{
			// Stop sampling the rest of the segment: the cube is in now, and the
			// only thing further samples could report is the same state again.
			break;
		}
	}
}

void AVoxelThrownItem::Splash(const FVector& CrossPointUU)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// ------------------------------------------------------------------------
	// THE IMPACT SPEED, AND WHY THIS HOOK IS BETTER THAN THE POLLER
	// ------------------------------------------------------------------------
	// The ripple field's auto-watcher cannot see an object's velocity, so it
	// infers one from (LastPos.Z - P.Z) / DeltaTime (VoxelRippleField.cpp:1062),
	// which is the AVERAGE vertical speed over the frame containing the crossing
	// -- frame-rate dependent by construction, and written up as a known defect
	// in docs/water-interactive-ripples.md SS12.4. We are inside the object, so we
	// can ask the projectile what its velocity actually is. That number does not
	// change with frame rate.
	//
	// It is not perfect: UProjectileMovementComponent has already integrated
	// gravity for this frame, so this is the speed at the END of the tick rather
	// than at the instant of the crossing -- 16 cm/s high at 60 fps, 33 cm/s at
	// 30 fps. Against a ramp that saturates at 6 m/s that is a 3% error at worst
	// and zero in the common case, because a real throw is already saturated.
	//
	// VERTICAL, not total speed. The splash comes from the velocity component
	// normal to the surface, and lake water in this project is a flat sheet at a
	// per-basin datum (VoxelWaterSubsystem.h:477-487), so normal means vertical.
	// A cube skimmed flat across a lake genuinely should barely ring.
	const double DownUUPerSec = Projectile ? FMath::Max(0.0, -Projectile->Velocity.Z) : 0.0;
	const double DownMPS = DownUUPerSec / 100.0;
	const double ImpactFraction =
		FMath::Clamp(DownMPS / VoxelRipple::kFullImpactSpeedMPS, VoxelRipple::kMinImpactFraction, 1.0);

	// ------------------------------------------------------------------------
	// RADIUS AND STRENGTH, DERIVED FROM THE CUBE
	// ------------------------------------------------------------------------
	// RADIUS. The cube presents a 0.3 m x 0.3 m face, an area of 0.09 m^2, whose
	// equivalent circle has radius sqrt(0.09/pi) = 0.169 m. That is the hole it
	// makes in the surface at the instant of contact and it is the geometric
	// floor. A faster entry opens a wider crown than the body itself, so the
	// radius is scaled up with the impact fraction:
	//
	//     R = max(0.30, 0.169 * (1 + 2f))     f in [0.25, 1]
	//       = 0.30 m at a gentle drop   ...   0.51 m at 6 m/s and above
	//
	// THE 1 + 2f IS CHOSEN, NOT DERIVED, and the honest statement is that only
	// its endpoints were reasoned about: at the bottom the ring is the cube's own
	// footprint, and at the top it is 0.51 m, which lands next to the 0.5 m the
	// ripple field already uses for a whole PLAYER entering the water
	// (voxel.Water.Ripple.PlayerRadiusM, VoxelRippleField.cpp:126). A hard-thrown
	// 30 cm cube making about the ring a person does is the right relationship;
	// the straight line between the two endpoints is a convenience.
	//
	// THE FLOOR IS NOT MINE. UVoxelRippleFieldSubsystem::kMinDisturbanceRadiusM
	// is 0.30 m (VoxelRippleField.h:205) because below about three texels a
	// raised-cosine bump on a 10 cm grid is mostly grid, and what radiates from
	// it is checkerboard noise rather than a ring. Clamping here rather than
	// letting the subsystem widen it silently keeps its "radius was widened"
	// log line meaningful for callers that really did ask for something too
	// small. NOTE WHAT THIS MEANS: a 30 cm cube is AT the field's resolution
	// floor, so for a gentle drop the grid decides the radius and the geometry
	// never gets a vote.
	//
	// DERIVED FROM EdgeUU, not written as 0.169. The item registry already carries
	// a per-item CubeEdgeMetres (VoxelItem.h:136; `throwable.voxelcube30` sets it
	// to the same 0.3 m this class hard-codes as 3 voxels), so the day a thrown
	// item comes in a second size this arithmetic follows it instead of quietly
	// describing the wrong cube. docs/throwables-v0.md SS8.3 has that wiring.
	constexpr double kPi = 3.14159265358979323846;
	const double EdgeM = EdgeUU / 100.0;                        // 0.30 m at v0
	const double FootprintRadiusM = EdgeM / FMath::Sqrt(kPi);   // 0.169 m: equal-area circle
	const double MinRadiusM = UVoxelRippleFieldSubsystem::kMinDisturbanceRadiusM;
	const double RadiusM = FMath::Max(MinRadiusM, FootprintRadiusM * (1.0 + 2.0 * ImpactFraction));

	// STRENGTH. The cube displaces its own volume, 0.3^3 = 0.027 m^3. Spread over
	// the smallest ring the field can draw (pi * 0.30^2 = 0.283 m^2) that is
	//
	//     H0 = 0.027 / 0.283 = 0.0955 m
	//
	// of water pushed up -- and this is the reassuring part: that number arrived
	// from the cube's own geometry and landed within 6% of the 0.09 m the ripple
	// field independently chose for a player entering water at full speed
	// (voxel.Water.Ripple.PlayerStrengthM, VoxelRippleField.cpp:119). Two
	// different derivations agreeing to 6% is the best evidence available here
	// that the scale is right, since nothing about this can be measured without
	// a screenshot.
	//
	//     S = H0 * f * voxel.Throwable.SplashScale
	//       = 2.4 cm at the gentlest entry ... 9.6 cm at 6 m/s and above
	//
	// THIS IS NOT VOLUME-CONSERVING and does not pretend to be. Strength is an
	// AMPLITUDE handed to a linear wave equation, not a volume of water; H0 is
	// used as a scale that the cube's size sets, and f as the fraction of it the
	// impact actually delivers. For context on whether 9.6 cm is large: the
	// ambient wind wave on this water is 8.6 cm crest-to-mean
	// (VoxelRippleField.cpp:121-122), so a full-speed throw makes a ring slightly
	// bigger than the sea state it lands in, and a gentle drop makes one about a
	// quarter of it.
	// kPi is spelled out above rather than using the engine's PI macro, which is a
	// FLOAT literal; everything else in this arithmetic is double, and mixing the
	// two in a constant is the kind of thing that makes two builds print slightly
	// different numbers in the log line below.
	const double CubeVolumeM3 = EdgeM * EdgeM * EdgeM; // 0.027 m^3 at v0
	const double DisplacementHeightM = CubeVolumeM3 / (kPi * MinRadiusM * MinRadiusM);
	const double StrengthM = DisplacementHeightM * ImpactFraction
	                       * FMath::Max(0.0f, CVarThrowableSplashScale.GetValueOnGameThread());

	// ONE CALL, AND IT CANNOT FAIL. AddDisturbanceAt is a static designed for
	// exactly this -- "a hook into a system that need not be visible to the
	// caller" (VoxelRippleField.h:265-268). It is safe before the first frame,
	// with the feature off, with no assets and with no water anywhere near: the
	// disturbance is queued, or dropped AND COUNTED. This call site never has to
	// ask whether ripples are on, and deliberately does not.
	UVoxelRippleFieldSubsystem::AddDisturbanceAt(World, CrossPointUU,
	                                             static_cast<float>(RadiusM),
	                                             static_cast<float>(StrengthM));
	bSplashed = true;
	++GSplashed;

	// --- entry deceleration, one multiply ------------------------------------
	// See kWaterEntrySpeedRetained. Without it the cube is on the lake bed within
	// two frames and the player never connects the ring to the thing that made it.
	if (Projectile)
	{
		Projectile->Velocity *= kWaterEntrySpeedRetained;
	}

	// ------------------------------------------------------------------------
	// THE LOG LINE IS THE INSTRUMENT
	// ------------------------------------------------------------------------
	// Everything a "the splash did nothing" diagnosis needs, on the frame it
	// happened, because the alternative is guessing from a screenshot. Three of
	// the four things that make a correct call invisible are decided by numbers
	// only available here.
	//
	// THE SURFACE HEIGHT COMES FROM THE WATER SUBSYSTEM, NEVER FROM A TRACE. Lake
	// water is a flat sheet at a per-basin datum, so the water surface at a point
	// is emphatically not the terrain height there and a downward trace answers
	// the wrong question (VoxelWaterSubsystem.h:349-388 is the depth query that
	// answers the right one). A depth of exactly 0 at a point the predicate calls
	// submerged is itself informative: it means CA-only water with no datum
	// anywhere (VoxelWaterSubsystem.h:382-387) -- a player-poured pool, a flooded
	// dig -- which is also precisely the water the baked shore mask is most likely
	// to call dry and delete the ripple over.
	double SurfaceZUU = 0.0;
	bool bHaveSurface = false;
	if (const UVoxelWaterSubsystem* Water = World->GetSubsystem<UVoxelWaterSubsystem>())
	{
		// Sampled half a voxel below the crossing so the query is unambiguously
		// inside the water rather than exactly on its boundary.
		const FVector Probe = CrossPointUU - FVector(0.0, 0.0, VoxelCoords::VoxelSizeUU * 0.5);
		const double DepthUU = Water->SubmergedDepthUUAtWorld(Probe);
		if (DepthUU > 0.0)
		{
			SurfaceZUU = Probe.Z + DepthUU;
			bHaveSurface = true;
		}
	}

	// Is the splash even inside the ripple window? The field is 51.2 m across and
	// FOLLOWS THE CAMERA (VoxelRippleField.h:176-178), so a ring more than 25.6 m
	// away is discarded -- correctly, and it will show up as `outside` in
	// voxel.Water.Ripple.Stat. A cube thrown at 16 m/s on a 30-degree arc travels
	// well over 25 m, so this is not a corner case; it is the most likely reason a
	// perfectly correct splash is invisible, and it deserves to say so out loud
	// rather than be inferred from a counter afterwards.
	double CameraDistM = -1.0;
	if (const APlayerController* PC = World->GetFirstPlayerController())
	{
		if (const APlayerCameraManager* Cam = PC->PlayerCameraManager)
		{
			const FVector C = Cam->GetCameraLocation();
			CameraDistM = FVector2D(CrossPointUU.X - C.X, CrossPointUU.Y - C.Y).Size() / 100.0;
		}
	}
	const double HalfWindowM = UVoxelRippleFieldSubsystem::kWindowUU * 0.5 / 100.0;
	const bool bOutsideWindow = (CameraDistM >= 0.0) && (CameraDistM > HalfWindowM);

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelThrownItem SPLASH item='%s' at (%.0f, %.0f, %.0f) UU: down %.2f m/s -> f=%.2f, ")
	       TEXT("r=%.2f m, s=%.3f m. Water surface %s. Camera %.1f m away (window half-width %.1f m)%s"),
	       *ItemId.ToString(), CrossPointUU.X, CrossPointUU.Y, CrossPointUU.Z, DownMPS,
	       ImpactFraction, RadiusM, StrengthM,
	       bHaveSurface ? *FString::Printf(TEXT("Z=%.1f UU (basin datum)"), SurfaceZUU)
	                    : TEXT("has NO datum (CA-only water; the baked shore mask may delete this ")
	                      TEXT("ripple -- voxel.Water.Ripple.MaskEnable 0 to check)"),
	       CameraDistM, HalfWindowM,
	       bOutsideWindow ? TEXT(" -- OUTSIDE THE RIPPLE WINDOW, this one will be dropped as ")
	                        TEXT("'outside' and nothing will be drawn. Not a bug: get closer.")
	                      : TEXT("."));
}

// ---------------------------------------------------------------------------
// SETTLING, PICKUP AND THE FIVE-MINUTE TIMER
// ---------------------------------------------------------------------------

void AVoxelThrownItem::Settle(const FVector& RestLocationUU, const TCHAR* Reason)
{
	if (bSettled)
	{
		return;
	}
	bSettled = true;
	SettledSeconds = 0.0f;

	if (Projectile)
	{
		Projectile->StopMovementImmediately();
		Projectile->Deactivate();
	}
	SetActorLocation(RestLocationUU);
	SetActorRotation(FRotator::ZeroRotator); // square on the voxel grid
	LastTickLocationUU = RestLocationUU;

	// A settled item only has to notice the player approaching, so it drops from
	// per-frame to 4 Hz. With no items in flight this class then costs 4 ticks a
	// second per item and no water or voxel queries at all.
	SetActorTickInterval(SettledTickIntervalSeconds);

	// The timer is read from the CVar HERE rather than at expiry, so a settled
	// item's fate is decided when it lands. Non-positive means never, which is a
	// debugging state.
	const double ReturnSeconds =
		static_cast<double>(CVarThrowableReturnSeconds.GetValueOnGameThread());
	if (UWorld* World = GetWorld(); World && ReturnSeconds > 0.0)
	{
		World->GetTimerManager().SetTimer(ReturnTimerHandle, this,
		                                  &AVoxelThrownItem::OnReturnTimerExpired,
		                                  static_cast<float>(ReturnSeconds), false);
	}

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelThrownItem settled (%s): item='%s' x%d at (%.0f, %.0f, %.0f) UU after %.2f s ")
	       TEXT("of flight; %s"),
	       Reason, *ItemId.ToString(), Count, RestLocationUU.X, RestLocationUU.Y, RestLocationUU.Z,
	       FlightSeconds,
	       ReturnSeconds > 0.0
	           ? *FString::Printf(TEXT("returns to inventory in %.0f s."), ReturnSeconds)
	           : TEXT("voxel.Throwable.ReturnSeconds <= 0, so it will lie here forever."));
}

void AVoxelThrownItem::TryProximityPickup()
{
	if (bItemAccountedFor || SettledSeconds < PickupArmSeconds)
	{
		// The arming delay stops a cube dropped at your own feet from being back
		// in your inventory before you have seen it land.
		return;
	}
	AActor* T = Thrower.Get();
	if (!T)
	{
		return; // nobody to pick it up; the timer will deal with it
	}

	const double RadiusUU =
		FMath::Max(0.0f, CVarThrowablePickupRadiusM.GetValueOnGameThread()) * 100.0;
	if (FVector::DistSquared(T->GetActorLocation(), GetActorLocation()) > RadiusUU * RadiusUU)
	{
		return;
	}

	const int32 Added = GiveBackToThrower(TEXT("pickup"));
	if (Added <= 0)
	{
		// A FULL INVENTORY MUST NOT DESTROY THE ITEM. This is the opposite of
		// what the five-minute timeout does, and deliberately: the timeout has
		// been asked to remove the actor from the world, and a pickup has not.
		// Leaving it on the ground is the only behaviour that does not lose the
		// player's property because they walked past it.
		if (!bLoggedInventoryFull)
		{
			bLoggedInventoryFull = true;
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("VoxelThrownItem: '%s' x%d could not be picked up (inventory took 0). ")
			       TEXT("Leaving it on the ground. Said once per item."),
			       *ItemId.ToString(), Count);
		}
		return;
	}

	GPickedUp += Added;
	if (Added < Count)
	{
		// Partial: keep the remainder in the world rather than eating it.
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelThrownItem: picked up %d of %d '%s'; %d left on the ground."), Added,
		       Count, *ItemId.ToString(), Count - Added);
		Count -= Added;
		return;
	}

	bItemAccountedFor = true;
	UE_LOG(LogVoxelEarth, Log, TEXT("VoxelThrownItem: picked up '%s' x%d after %.0f s on the ground."),
	       *ItemId.ToString(), Added, SettledSeconds);
	Destroy();
}

void AVoxelThrownItem::OnReturnTimerExpired()
{
	if (bItemAccountedFor)
	{
		return;
	}
	bItemAccountedFor = true;

	const bool bReturn = CVarThrowableReturnToInventory.GetValueOnGameThread();
	const int32 Added = bReturn ? GiveBackToThrower(TEXT("5-minute timeout")) : 0;
	const int32 Missing = Count - Added;

	GReturned += Added;
	GLost += FMath::Max(0, Missing);

	if (Added > 0)
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelThrownItem: timeout returned '%s' x%d to %s after %.0f s on the ground."),
		       *ItemId.ToString(), Added,
		       Thrower.IsValid() ? *Thrower->GetName() : TEXT("<gone>"), SettledSeconds);
	}
	if (Missing > 0)
	{
		// NEVER A SILENT DELETE. The actor goes away either way -- that is what
		// the timeout was asked to do -- but an item that could not be given back
		// is a LOSS and is logged and counted as one. "It vanished" is not a
		// diagnosis; "3 of 3 'voxel.cube' lost: inventory accepted none" is.
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelThrownItem: LOST %d of %d '%s' on timeout -- %s. The world actor is being ")
		       TEXT("destroyed anyway (voxel.Throwable.Stat counts this)."),
		       Missing, Count, *ItemId.ToString(),
		       !bReturn ? TEXT("voxel.Throwable.ReturnToInventory is 0, so this is despawn mode ")
		                  TEXT("and the loss is intended")
		       : !Thrower.IsValid() ? TEXT("the thrower no longer exists")
		                            : TEXT("no inventory accepted it (full, or none was found)"));
	}

	Destroy();
}

int32 AVoxelThrownItem::GiveBackToThrower(const TCHAR* Reason)
{
	AActor* T = Thrower.Get();
	if (!T)
	{
		return 0;
	}

	// 1. The installed sink wins, always. It is the escape hatch for an inventory
	//    API whose shape differs from the guess below, and the only mechanism
	//    that needs no compile-time knowledge of the item system.
	if (SinkRef())
	{
		const int32 Added = SinkRef()(T, ItemId, Count);
		return FMath::Clamp(Added, 0, Count);
	}

#if VOXELTHROWNITEM_HAS_INVENTORY
	// 2. The direct call, compiled only when the header exists. The component
	//    could reasonably live on the pawn, on its controller, or on a controller
	//    passed as the thrower -- so look in all three rather than pick one and
	//    be wrong. First hit wins.
	UVoxelInventoryComponent* Inv = T->FindComponentByClass<UVoxelInventoryComponent>();
	if (!Inv)
	{
		if (const APawn* AsPawn = Cast<APawn>(T))
		{
			if (AController* C = AsPawn->GetController())
			{
				Inv = C->FindComponentByClass<UVoxelInventoryComponent>();
			}
		}
		else if (const AController* AsController = Cast<AController>(T))
		{
			if (APawn* P = AsController->GetPawn())
			{
				Inv = P->FindComponentByClass<UVoxelInventoryComponent>();
			}
		}
	}
	if (Inv)
	{
		return FMath::Clamp(Inv->TryAddItem(ItemId, Count), 0, Count);
	}
#endif

	if (!bLoggedInventoryFull)
	{
		bLoggedInventoryFull = true;
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelThrownItem (%s): no inventory reachable from '%s' for item '%s' x%d. ")
		       TEXT("Either UVoxelInventoryComponent does not exist in this build (compiled=%d) or ")
		       TEXT("nothing called AVoxelThrownItem::SetInventorySink. docs/throwables-v0.md SS5 has ")
		       TEXT("the one line that fixes this. Said once per item."),
		       Reason, *T->GetName(), *ItemId.ToString(), Count, VOXELTHROWNITEM_HAS_INVENTORY);
	}
	return 0;
}

void AVoxelThrownItem::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ReturnTimerHandle);
	}
	// Deliberately NOT counted as lost when the reason is anything but Destroyed:
	// a PIE stop or a level change is not the game losing the player's item, and
	// counting it would make the `lost` counter -- the one number in
	// voxel.Throwable.Stat that should mean something is wrong -- meaningless.
	Super::EndPlay(EndPlayReason);
}

// ---------------------------------------------------------------------------
// STATICS
// ---------------------------------------------------------------------------

void AVoxelThrownItem::SetInventorySink(FVoxelThrownItemInventorySink InSink)
{
	SinkRef() = MoveTemp(InSink);
}

bool AVoxelThrownItem::HasInventorySink()
{
	return static_cast<bool>(SinkRef());
}

FString AVoxelThrownItem::GetStatLine(const UWorld* World)
{
	int32 InFlight = 0;
	int32 Settled = 0;
	if (World)
	{
		// const_cast for the same reason AddDisturbanceAt does it
		// (VoxelRippleField.cpp:569-572): iterating a world's actors does not
		// modify it, and forcing every caller to hold a non-const UWorld* is
		// ceremony at the call site for nothing.
		for (TActorIterator<AVoxelThrownItem> It(const_cast<UWorld*>(World)); It; ++It)
		{
			if (It->IsSettled())
			{
				++Settled;
			}
			else
			{
				++InFlight;
			}
		}
	}
	return FString::Printf(
		TEXT("Throwables: %d in flight, %d settled | thrown %lld, splashed %lld, picked up %lld, ")
		TEXT("returned %lld, LOST %lld"),
		InFlight, Settled, static_cast<long long>(GThrown), static_cast<long long>(GSplashed),
		static_cast<long long>(GPickedUp), static_cast<long long>(GReturned),
		static_cast<long long>(GLost));
}
