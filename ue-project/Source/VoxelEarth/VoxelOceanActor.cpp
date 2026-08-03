#include "VoxelOceanActor.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "UObject/ConstructorHelpers.h"
#include "VoxelEarth.h"
#include "VoxelWaterSubsystem.h"
#include "VoxelWorldSubsystem.h"

AVoxelOceanActor::AVoxelOceanActor()
{
	PrimaryActorTick.bCanEverTick = true;

	OceanPlane = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("OceanPlane"));
	SetRootComponent(OceanPlane);
	OceanPlane->SetMobility(EComponentMobility::Movable); // recentred every tick, see UpdateFollowPlane
	OceanPlane->SetCollisionEnabled(ECollisionEnabled::NoCollision); // cosmetic only -- no gameplay forces yet (W1)
	OceanPlane->SetCastShadow(false);
	OceanPlane->SetReceivesDecals(false);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneMeshFinder(TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (PlaneMeshFinder.Succeeded())
	{
		OceanPlane->SetStaticMesh(PlaneMeshFinder.Object);
	}
	const double Scale = PlaneSizeUU / SourcePlaneSizeUU;
	OceanPlane->SetRelativeScale3D(FVector(Scale, Scale, 1.0));

	// Loaded by path in BeginPlay-adjacent code below is the terrain
	// material's pattern; the ocean material is instead resolved here in the
	// constructor (StaticLoadObject works fine at CDO construction time too)
	// so OceanPlane never renders with the engine default material even for
	// one frame.
	UMaterialInterface* OceanMaterial = Cast<UMaterialInterface>(
		StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/Game/Voxel/M_Ocean.M_Ocean")));
	if (OceanMaterial)
	{
		OceanPlane->SetMaterial(0, OceanMaterial);
	}
	// else: engine default material (visible, if untinted) -- matches the
	// terrain material's "never crash, just warn" fallback; warning logged in
	// BeginPlay once GetWorld()/logging is safe to call from an easily
	// greppable place.

	UnderwaterPostProcess = CreateDefaultSubobject<UPostProcessComponent>(TEXT("UnderwaterPostProcess"));
	UnderwaterPostProcess->SetupAttachment(RootComponent);
	UnderwaterPostProcess->bUnbound = true; // applies globally -- there's no volume to swim into, water is everywhere below z=0
	UnderwaterPostProcess->BlendWeight = 0.f; // starts above water (see AVoxelEarthGameMode spawn height)
	UnderwaterPostProcess->Settings.bOverride_SceneColorTint = true;
	UnderwaterPostProcess->Settings.SceneColorTint = FLinearColor(0.05f, 0.30f, 0.35f);
	UnderwaterPostProcess->Settings.bOverride_VignetteIntensity = true;
	UnderwaterPostProcess->Settings.VignetteIntensity = 0.6f;
}

void AVoxelOceanActor::BeginPlay()
{
	Super::BeginPlay();

	if (OceanPlane && OceanPlane->GetMaterial(0) == nullptr)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("M_Ocean not found at /Game/Voxel/M_Ocean -- ocean plane using the engine default material."));
	}

	// Underwater fog: spawned once, hidden until the camera first submerges
	// (UpdateUnderwaterState toggles it thereafter). A dedicated actor rather
	// than reusing any level fog -- none exists yet (no authored map, see
	// AVoxelEarthGameMode::BeginPlay).
	if (UWorld* World = GetWorld())
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transient;
		UnderwaterFog = World->SpawnActor<AExponentialHeightFog>(SpawnParams);
		if (UnderwaterFog)
		{
			UnderwaterFog->SetActorHiddenInGame(true);
			if (UExponentialHeightFogComponent* FogComp = UnderwaterFog->GetComponent())
			{
				FogComp->SetVisibility(false);
				// Thick, close, blue-green -- reads as "underwater murk", not
				// realistic scattering (that's W5 polish).
				FogComp->FogDensity = 0.08f;
				FogComp->SetFogInscatteringColor(FLinearColor(0.02f, 0.14f, 0.18f)); // FogInscatteringColor member is UE 5.8-deprecated; setter still applies it
				FogComp->FogHeightFalloff = 0.0f; // uniform: there's no meaningful "height" underwater yet
				FogComp->StartDistance = 0.f;
			}
		}
	}

	UpdateFollowPlane();
}

void AVoxelOceanActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	UpdateFollowPlane();
	UpdateUnderwaterState();
}

void AVoxelOceanActor::UpdateFollowPlane()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}

	FVector CameraLoc;
	if (PC->PlayerCameraManager)
	{
		CameraLoc = PC->PlayerCameraManager->GetCameraLocation();
	}
	else if (APawn* P = PC->GetPawn())
	{
		CameraLoc = P->GetActorLocation();
	}
	else
	{
		return; // nothing to follow yet
	}

	const double SnappedX = FMath::GridSnap(CameraLoc.X, FollowSnapUU);
	const double SnappedY = FMath::GridSnap(CameraLoc.Y, FollowSnapUU);
	// The visual plane sits ON the datum, by name (voxelcore/core.h
	// kSeaLevelMm via UVoxelWaterSubsystem::SeaLevelZUU) rather than on a
	// literal 0 that happened to agree with it.
	SetActorLocation(FVector(SnappedX, SnappedY, UVoxelWaterSubsystem::SeaLevelZUU()));
}

void AVoxelOceanActor::UpdateUnderwaterState()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}

	FVector CameraPos = FVector::ZeroVector;
	bool bHaveCamera = false;
	if (PC->PlayerCameraManager)
	{
		CameraPos = PC->PlayerCameraManager->GetCameraLocation();
		bHaveCamera = true;
	}
	else if (APawn* P = PC->GetPawn())
	{
		CameraPos = P->GetActorLocation();
		bHaveCamera = true;
	}
	if (!bHaveCamera)
	{
		return;
	}
	const double CameraZ = CameraPos.Z;

	// WAS `CameraZ < 0.0`, with nothing else consulted. A dry cavern below sea
	// level -- which this world has plenty of, caves.h only refuses to carve
	// AT or below the datum, not above it under a valley floor -- got full
	// underwater fog and the tinted post-process, and the post-process is
	// `bUnbound = true`, so the tint was global. See
	// UVoxelWaterSubsystem::IsUnderwaterAtWorld for what replaces it and what
	// it still cannot tell apart (docs/watershed-system-plan.md §5.3).
	bool bNowUnderwater;
	if (UVoxelWaterSubsystem* Water = World->GetSubsystem<UVoxelWaterSubsystem>())
	{
		bNowUnderwater = Water->IsUnderwaterAtWorld(CameraPos);
	}
	else if (UVoxelWorldSubsystem* Terrain = World->GetSubsystem<UVoxelWorldSubsystem>())
	{
		// No water simulation in this world (the transient loading world, or a
		// stripped configuration): the ocean datum alone, which is still the
		// terrain-aware test rather than the camera one.
		bNowUnderwater = UVoxelWaterSubsystem::IsOpenSeaAtWorld(
			CameraZ, Terrain->GetSurfaceHeightUU(CameraPos.X, CameraPos.Y));
	}
	else
	{
		bNowUnderwater = false;
	}
	if (bNowUnderwater == bUnderwater)
	{
		return; // no transition -- fog/post-process state is already correct
	}
	bUnderwater = bNowUnderwater;

	if (UnderwaterPostProcess)
	{
		UnderwaterPostProcess->BlendWeight = bUnderwater ? 1.f : 0.f;
	}
	if (UnderwaterFog)
	{
		UnderwaterFog->SetActorHiddenInGame(!bUnderwater);
		if (UExponentialHeightFogComponent* FogComp = UnderwaterFog->GetComponent())
		{
			FogComp->SetVisibility(bUnderwater);
		}
	}

	// The only observable signal for this branch in an unattended
	// screenshot run (task spec: verify via log lines instead of a second
	// screenshot) -- logged once per transition, not per tick.
	// The ground height goes in the line because it is now HALF THE ANSWER: a
	// transition at z=-500 over ground at +12000 would be the old camera-only
	// bug returning, and the log is the only signal an unattended screenshot
	// run has.
	const double GroundZ = World->GetSubsystem<UVoxelWorldSubsystem>()
	                           ? World->GetSubsystem<UVoxelWorldSubsystem>()->GetSurfaceHeightUU(CameraPos.X, CameraPos.Y)
	                           : 0.0;
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("Ocean: camera %s water (camera z=%.1f UU, worldgen ground z=%.1f UU, sea z=%.1f UU)"),
	       bUnderwater ? TEXT("entered") : TEXT("exited"), CameraZ, GroundZ,
	       UVoxelWaterSubsystem::SeaLevelZUU());
}
