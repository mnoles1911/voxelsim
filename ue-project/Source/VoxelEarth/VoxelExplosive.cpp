#include "VoxelExplosive.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Shakes/PerlinNoiseCameraShakePattern.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "VoxelEarth.h"
#include "VoxelWorldSubsystem.h"

// UVoxelBlastCameraShake --------------------------------------------------

UVoxelBlastCameraShake::UVoxelBlastCameraShake(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UPerlinNoiseCameraShakePattern>(TEXT("RootShakePattern")))
{
	// Short, punchy rotation + FOV kick -- "brief camera shake" per
	// m1-plan.md, no particles yet.
	if (UPerlinNoiseCameraShakePattern* Pattern = Cast<UPerlinNoiseCameraShakePattern>(GetRootShakePattern()))
	{
		Pattern->Duration = 0.5f;
		Pattern->BlendInTime = 0.05f;
		Pattern->BlendOutTime = 0.3f;

		Pattern->RotationAmplitudeMultiplier = 1.f;
		Pattern->Pitch.Amplitude = 1.5f;
		Pattern->Pitch.Frequency = 20.f;
		Pattern->Yaw.Amplitude = 1.5f;
		Pattern->Yaw.Frequency = 18.f;
		Pattern->Roll.Amplitude = 1.0f;
		Pattern->Roll.Frequency = 15.f;

		Pattern->FOV.Amplitude = 2.0f;
		Pattern->FOV.Frequency = 10.f;
	}
}

// AVoxelExplosive -----------------------------------------------------------

AVoxelExplosive::AVoxelExplosive()
{
	// Ticks so it can resolve its own collision against the voxel world -- see
	// AVoxelExplosive::Tick. Nothing else here needs a tick.
	PrimaryActorTick.bCanEverTick = true;

	BlastMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BlastMesh"));
	SetRootComponent(BlastMesh);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMeshFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMeshFinder.Succeeded())
	{
		BlastMesh->SetStaticMesh(SphereMeshFinder.Object);
	}
	// Small hand-grenade-ish sphere (BasicShapes/Sphere is ~100 UU across).
	BlastMesh->SetRelativeScale3D(FVector(0.15));
	BlastMesh->SetCollisionProfileName(TEXT("Projectile"));
	BlastMesh->SetCastShadow(true);

	Projectile = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Projectile"));
	Projectile->UpdatedComponent = BlastMesh;
	Projectile->ProjectileGravityScale = 1.0f;
	// Cosmetic flight only (m1-plan.md "Explosives v1" row / class comment
	// above): there is no Chaos collision against voxel terrain, so bounces
	// mostly only happen against other physics-enabled actors, if any.
	Projectile->bShouldBounce = true;
	Projectile->Bounciness = 0.35f; // decisions table: "bounce 0.35 friction"
	Projectile->Friction = 0.35f;
	Projectile->bRotationFollowsVelocity = true;
	Projectile->bAutoActivate = false; // activated explicitly by Launch()
}

void AVoxelExplosive::BeginPlay()
{
	Super::BeginPlay();

	// Dark tint on the BasicShapes sphere (m1-plan.md "visible as a small
	// engine BasicShapes sphere (dark MID tint)"). Best-effort: the exact
	// parameter name on the engine's default BasicShapeMaterial isn't part of
	// any documented contract, so this silently no-ops (still visible, just
	// untinted) if the material has no such parameter -- never fatal.
	if (BlastMesh)
	{
		if (UMaterialInterface* BaseMaterial = BlastMesh->GetMaterial(0))
		{
			if (UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMaterial, this))
			{
				const FLinearColor DarkTint(0.05f, 0.05f, 0.06f);
				MID->SetVectorParameterValue(TEXT("Color"), DarkTint);
				MID->SetVectorParameterValue(TEXT("BaseColor"), DarkTint);
				BlastMesh->SetMaterial(0, MID);
			}
		}
	}
}

void AVoxelExplosive::Launch(const FVector& InitialVelocityUUPerSec)
{
	if (Projectile)
	{
		Projectile->Activate();
		Projectile->Velocity = InitialVelocityUUPerSec;
	}

	// Roll this charge's radius once, here, rather than at detonation. The carve
	// goes through the edit log, so the radius has to be a decided value that
	// travels with the edit -- re-rolling it at detonation on each machine would
	// make two clients carve different craters from the same entry.
	ThisChargeRadiusUU = BlastRadiusUU + FMath::FRandRange(-BlastRadiusVarianceUU, BlastRadiusVarianceUU);

	LastTickLocationUU = GetActorLocation();

	UWorld* World = GetWorld();
	if (World)
	{
		World->GetTimerManager().SetTimer(FuseTimerHandle, this, &AVoxelExplosive::Detonate, FuseSeconds, false);
	}
}

void AVoxelExplosive::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bLanded)
	{
		return; // already resting on terrain; the fuse does the rest
	}

	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Subsystem)
	{
		return;
	}

	const FVector Now = GetActorLocation();
	const FVector Segment = Now - LastTickLocationUU;
	const double Travelled = Segment.Size();
	if (Travelled <= KINDA_SMALL_NUMBER)
	{
		LastTickLocationUU = Now;
		return;
	}

	// Test the SEGMENT actually travelled, not the endpoint. A charge thrown at
	// the top of the speed range covers well over a voxel per frame, so an
	// endpoint test would let it tunnel through a thin roof and detonate in the
	// open air below -- which is indistinguishable, from the player's side, from
	// the falling-through bug this replaces.
	FVector HitVoxelCenter, PrevVoxelCenter;
	if (Subsystem->RaycastVoxelWorld(LastTickLocationUU, Segment, Travelled, HitVoxelCenter, PrevVoxelCenter))
	{
		// Rest in the last EMPTY voxel before the solid one, so the charge sits
		// on the surface rather than inside it. Detonating from inside would
		// bury the crater's centre and carve a hollow under the ground instead
		// of a visible bowl in it.
		SetActorLocation(PrevVoxelCenter);
		if (Projectile)
		{
			Projectile->StopMovementImmediately();
			Projectile->Deactivate();
		}
		bLanded = true;
		LastTickLocationUU = PrevVoxelCenter;
		return;
	}

	LastTickLocationUU = Now;
}

void AVoxelExplosive::Detonate()
{
	UWorld* World = GetWorld();
	if (World)
	{
		if (UVoxelWorldSubsystem* Subsystem = World->GetSubsystem<UVoxelWorldSubsystem>())
		{
			const int32 Removed = Subsystem->CarveSphere(GetActorLocation(), ThisChargeRadiusUU, BlastJitterUU);
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("VoxelExplosive detonated at (%.0f, %.0f, %.0f): r=%.0f UU (+/-%.0f jitter), landed=%s, ")
			       TEXT("removed %d voxels."),
			       GetActorLocation().X, GetActorLocation().Y, GetActorLocation().Z, ThisChargeRadiusUU,
			       BlastJitterUU, bLanded ? TEXT("y") : TEXT("n"), Removed);

			// removed==0 with landed=y means the charge came to rest against
			// terrain and then carved nothing, which should be impossible --
			// worth a warning rather than a silent no-op, because that is
			// exactly how the fall-through bug presented (a detonation log line
			// that looked fine next to a crater that did not exist).
			if (Removed == 0)
			{
				UE_LOG(LogVoxelEarth, Warning,
				       TEXT("VoxelExplosive removed 0 voxels -- detonated in open air or against unstreamed terrain."));
			}
		}

		// Brief camera shake on whichever local player controller exists (no
		// particles yet -- see class comment).
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (PC->PlayerCameraManager)
			{
				PC->PlayerCameraManager->StartCameraShake(UVoxelBlastCameraShake::StaticClass());
			}
		}
	}

	Destroy();
}
