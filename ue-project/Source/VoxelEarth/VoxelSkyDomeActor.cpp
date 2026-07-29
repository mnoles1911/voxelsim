#include "VoxelSkyDomeActor.h"

#include "VoxelClipmapActor.h" // AVoxelClipmapActor::OuterHalfExtentUU -- the radius check
#include "VoxelEarth.h"
#include "VoxelSkySubsystem.h" // VoxelSky::IsDomeEnabled / GetDomeRadiusUU

#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// Asset paths, named once. Both are authored by
	// Tools/create_sky_material.py + Tools/import_sky_textures.py; neither is
	// checked in as a source file, so a missing one is a "the commandlet did not
	// run" failure and the log lines below say exactly that.
	const TCHAR* kNightSkyMaterialPath = TEXT("/Game/Voxel/M_NightSky.M_NightSky");
	const TCHAR* kDomeMeshPath = TEXT("/Engine/BasicShapes/Sphere.Sphere");

	// Fallback for the stock sphere's own radius, used only if
	// UStaticMesh::GetBounds() comes back degenerate. /Engine/BasicShapes/Sphere
	// is a 100 UU diameter ball; this is the documented value and the bounds read
	// is what actually decides.
	constexpr double kAssumedSourceRadiusUU = 50.0;
} // namespace

AVoxelSkyDomeActor::AVoxelSkyDomeActor()
{
	PrimaryActorTick.bCanEverTick = true;

	DomeMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DomeMesh"));
	SetRootComponent(DomeMesh);
	DomeMesh->SetMobility(EComponentMobility::Movable); // recentred every tick, see UpdateFollowCamera
	DomeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision); // a 200 km collision sphere around the player would be a very bad day
	DomeMesh->SetCastShadow(false); // additive unlit sky; a shadow-casting one would shadow the entire world
	DomeMesh->SetReceivesDecals(false);
	DomeMesh->SetCanEverAffectNavigation(false);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMeshFinder(kDomeMeshPath);
	if (SphereMeshFinder.Succeeded())
	{
		DomeMesh->SetStaticMesh(SphereMeshFinder.Object);
		// Read the source radius here, while the finder guarantees the asset is
		// loaded, rather than assuming 50 UU. See the member's comment.
		const double BoundsRadius = SphereMeshFinder.Object->GetBounds().SphereRadius;
		SourceRadiusUU = BoundsRadius > UE_KINDA_SMALL_NUMBER ? BoundsRadius : kAssumedSourceRadiusUU;
	}

	// LOADED IN THE CONSTRUCTOR, DELIBERATELY, and this is the pattern
	// AVoxelOceanActor (VoxelOceanActor.cpp:33-47) and AVoxelClipmapActor
	// (VoxelClipmapActor.cpp:132-136) both use and both state the reason for:
	// StaticLoadObject works fine at CDO construction time, so no frame ever
	// renders this component with the engine default material.
	//
	// The FALLBACK here is worse than theirs, which is why the diagnostic below
	// is louder than theirs. The ocean's fallback is a visible grey plane and the
	// clipmap's is visible grey terrain -- wrong, but unmistakably present. The
	// engine default material is OPAQUE and SINGLE-SIDED, and this dome is viewed
	// from the inside, so its backfaces are culled and the failure renders as
	// NOTHING AT ALL: a black night sky, indistinguishable from an unimported
	// texture, a mis-authored graph, or the commandlet never having been run. That
	// is the exact failure mode this feature was told not to have, so BeginPlay
	// logs an Error naming the path rather than a Warning.
	UMaterialInterface* NightSkyMaterial = Cast<UMaterialInterface>(
		StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, kNightSkyMaterialPath));
	if (NightSkyMaterial)
	{
		DomeMesh->SetMaterial(0, NightSkyMaterial);
	}
}

void AVoxelSkyDomeActor::BeginPlay()
{
	Super::BeginPlay();

	if (!DomeMesh)
	{
		return;
	}

	if (DomeMesh->GetStaticMesh() == nullptr)
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("VoxelSky dome: %s did not load -- there is no dome mesh, so THERE ARE NO STARS AND NO MOON ")
		       TEXT("this run. Everything else about the day/night cycle is unaffected."),
		       kDomeMeshPath);
	}
	if (DomeMesh->GetMaterial(0) == nullptr)
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("VoxelSky dome: %s did not load. The dome falls back to the engine default material, which is ")
		       TEXT("opaque and single-sided and therefore renders as NOTHING from inside the dome -- a blank ")
		       TEXT("night sky this run, and this line is the only thing that distinguishes that from a broken ")
		       TEXT("material graph or a missing texture. Run Tools/create_sky_material.py (which needs ")
		       TEXT("Tools/import_sky_textures.py first)."),
		       kNightSkyMaterialPath);
	}

	// --- the radius check ---------------------------------------------------
	//
	// M_NightSky depth-tests (Tools/create_sky_material.py, "DEPTH TEST STAYS
	// ON") so that terrain occludes the stars behind it. The cost of that choice
	// is a hard requirement on this actor: the dome must be FARTHER than the
	// farthest drawn geometry, or distant terrain is drawn in front of the sky
	// and the stars vanish behind the horizon ring.
	//
	// MEASURED AGAINST THE CLIPMAP RATHER THAN TAKEN ON TRUST. The material's
	// header recommends 2e7 UU on the strength of "the 50 km clipmap"; the
	// clipmap is not 50 km. Its outermost level's half-extent scales with the
	// ring cascade's runtime outer radius, and at the shipped 4096 m cascade it
	// is 65.5 km, whose CORNER -- the farthest point, and the one that matters --
	// is 92.7 km. 2e7 UU still clears that, by 2.16x, so the recommendation
	// survives; the number it was justified with does not, and the check is here
	// rather than in a comment so that -VoxelRingOuterMeters cannot quietly
	// invalidate it later.
	const double ClipmapCornerUU = AVoxelClipmapActor::OuterHalfExtentUU() * UE_DOUBLE_SQRT_2;
	const double RadiusUU = VoxelSky::GetDomeRadiusUU();
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSky dome RESOLVED: Enabled=%d RadiusUU=%.0f (%.1f km), source sphere radius %.1f UU -> ")
	       TEXT("uniform scale %.1f. Clipmap outer half-extent %.1f km, corner %.1f km; margin %.2fx."),
	       VoxelSky::IsDomeEnabled() ? 1 : 0, RadiusUU, RadiusUU / 100000.0, SourceRadiusUU,
	       SourceRadiusUU > 0.0 ? RadiusUU / SourceRadiusUU : 0.0,
	       AVoxelClipmapActor::OuterHalfExtentUU() / 100000.0, ClipmapCornerUU / 100000.0,
	       ClipmapCornerUU > 0.0 ? RadiusUU / ClipmapCornerUU : 0.0);

	if (RadiusUU <= ClipmapCornerUU)
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("VoxelSky dome: radius %.0f UU (%.1f km) does NOT exceed the clipmap's corner distance ")
		       TEXT("%.0f UU (%.1f km). M_NightSky depth-tests, so distant terrain will be drawn IN FRONT of the ")
		       TEXT("stars and the sky will read as empty over most of the horizon. Raise ")
		       TEXT("voxel.Sky.DomeRadiusUU above %.0f."),
		       RadiusUU, RadiusUU / 100000.0, ClipmapCornerUU, ClipmapCornerUU / 100000.0, ClipmapCornerUU);
	}

	ApplyDomeCvars();
	UpdateFollowCamera();
}

void AVoxelSkyDomeActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	ApplyDomeCvars();

	// Skipped while hidden: a dome nobody can see does not need to be anywhere.
	// It is recentred again the moment it is shown (ApplyDomeCvars logs that
	// transition), so a toggle cannot leave it stranded where the camera was.
	if (AppliedEnabled == 1)
	{
		UpdateFollowCamera();
	}
}

void AVoxelSkyDomeActor::ApplyDomeCvars()
{
	if (!DomeMesh)
	{
		return;
	}

	// GATED ON voxel.Sky.Enabled AS WELL AS ON voxel.Sky.DomeEnabled, and the
	// first half of that is not redundant. voxel.Sky.Enabled 0 freezes the whole
	// rig at the pre-W4 static pose -- a permanently-up sun at -45 degrees, i.e.
	// daylight (VoxelSkySubsystem.cpp ApplyStaticRigPose) -- and it also stops
	// UVoxelSkySubsystem::Tick, which is what writes MPC_VoxelSky. So the dome
	// would be left holding whatever StarBrightness the last live frame wrote,
	// which at night is 1.0, and the result is a full star field over a lit
	// afternoon that no cvar readout explains. The cvar's contract is that "off"
	// means the frame from before this feature existed; that frame had no dome.
	const int32 bEnabled = (VoxelSky::IsEnabled() && VoxelSky::IsDomeEnabled()) ? 1 : 0;
	if (bEnabled != AppliedEnabled)
	{
		AppliedEnabled = bEnabled;
		SetActorHiddenInGame(bEnabled == 0);
		DomeMesh->SetVisibility(bEnabled != 0);
		if (bEnabled != 0)
		{
			// Recentre BEFORE the first visible frame after a re-enable, so the
			// dome is never briefly drawn around wherever the camera used to be.
			UpdateFollowCamera();
		}
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelSky dome %s (voxel.Sky.DomeEnabled=%d, voxel.Sky.Enabled=%d). This is the mesh only ")
		       TEXT("-- the light rig, the atmosphere and the exposure curve are unaffected either way, which ")
		       TEXT("is what makes DomeEnabled a clean A/B for 'how much of the night frame is the star dome'."),
		       bEnabled != 0 ? TEXT("SHOWN") : TEXT("HIDDEN"),
		       VoxelSky::IsDomeEnabled() ? 1 : 0, VoxelSky::IsEnabled() ? 1 : 0);
	}

	const double RadiusUU = VoxelSky::GetDomeRadiusUU();
	if (SourceRadiusUU > 0.0 && !FMath::IsNearlyEqual(RadiusUU, AppliedRadiusUU, 1.0))
	{
		AppliedRadiusUU = RadiusUU;
		const double Scale = RadiusUU / SourceRadiusUU;
		DomeMesh->SetRelativeScale3D(FVector(Scale, Scale, Scale));
	}
}

void AVoxelSkyDomeActor::UpdateFollowCamera()
{
	// Resolution chain copied verbatim from AVoxelOceanActor::UpdateFollowPlane
	// (VoxelOceanActor.cpp:105-131): camera manager first, pawn as the fallback
	// for the frames before it exists, and NOTHING if neither does -- moving the
	// dome to the world origin on a controller-less frame would put a far-LWC
	// spawned player outside it, and outside an additive two-sided sphere the
	// stars render as a small ball in the middle of the screen rather than as a
	// sky. Leaving it where it was is always the better wrong answer.
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

	// All three axes, unlike the ocean's Z-pinned plane: the ocean is a surface
	// AT sea level and the dome is a shell AROUND the observer, and a player
	// flying to 20 km would leave a Z-pinned dome's centre 20 km below them --
	// harmless at this radius, but it buys nothing to be approximately right when
	// being exactly right is the same call.
	//
	// This runs in the default TG_PrePhysics and the camera updates in
	// TG_PostUpdateWork, so the dome follows the PREVIOUS frame's camera. That
	// lag is not worth a tick-group dependency here: the shading depends only on
	// the view direction (see the class comment), and a few metres of camera
	// motion against a 200 km radius is an angular error around 1e-8 radians.
	SetActorLocation(CameraLoc);
}
