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
	SetActorLocation(FVector(SnappedX, SnappedY, 0.0));
}

void AVoxelOceanActor::UpdateUnderwaterState()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}

	double CameraZ = 0.0;
	bool bHaveCamera = false;
	if (PC->PlayerCameraManager)
	{
		CameraZ = PC->PlayerCameraManager->GetCameraLocation().Z;
		bHaveCamera = true;
	}
	else if (APawn* P = PC->GetPawn())
	{
		CameraZ = P->GetActorLocation().Z;
		bHaveCamera = true;
	}
	if (!bHaveCamera)
	{
		return;
	}

	const bool bNowUnderwater = CameraZ < 0.0;
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
	UE_LOG(LogVoxelEarth, Log, TEXT("Ocean: camera %s water (camera z=%.1f UU)"),
	       bUnderwater ? TEXT("entered") : TEXT("exited"), CameraZ);
}
