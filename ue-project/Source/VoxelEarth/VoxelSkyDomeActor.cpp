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

	// The IsSky dome's material, authored by
	// Tools/create_sky_atmosphere_dome_material.py -- which must run AFTER
	// Tools/create_sky_material.py, because that script is the sole author of
	// MPC_VoxelSky and deletes the asset on every run.
	const TCHAR* kAtmosphereDomeMaterialPath = TEXT("/Game/Voxel/M_SkyAtmosphereDome.M_SkyAtmosphereDome");

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

	// --- the second dome: the canonical IsSky sphere -------------------------
	//
	// ATTACHED TO DomeMesh RATHER THAN GIVEN ITS OWN TRANSFORM. The camera follow
	// and the uniform radius scale then apply to both by construction: there is one
	// place that decides where the domes are and how big they are, and it is
	// impossible for the two to end up at different radii -- which would matter,
	// because the radius is a lower bound against the clipmap corner for both of
	// them and for two different reasons (see the header on AtmosphereDomeMesh).
	//
	// KeepRelativeTransform with an identity relative transform, so the child is
	// the parent sphere exactly. Faceting is irrelevant for the same reason it is
	// on the star dome: every quantity either material evaluates is a function of
	// normalize(P - camera), which is the pixel's exact ray direction wherever
	// along that ray the faceted surface happens to sit.
	AtmosphereDomeMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("AtmosphereDomeMesh"));
	AtmosphereDomeMesh->SetupAttachment(DomeMesh);
	AtmosphereDomeMesh->SetMobility(EComponentMobility::Movable);
	AtmosphereDomeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// SetCastShadow(false) is belt-and-braces: IsSky materials are already excluded
	// from the shadow-depth pass (ShouldIncludeMaterialInDefaultOpaquePass,
	// MaterialShared.h:3717-3721, gating ShadowDepthRendering.cpp:2311) and
	// M_SkyAtmosphereDome sets cast_ray_traced_shadows false. A 200 km sphere that
	// ever did cast would shadow the entire world, so all three places say so.
	AtmosphereDomeMesh->SetCastShadow(false);
	AtmosphereDomeMesh->SetReceivesDecals(false);
	AtmosphereDomeMesh->SetCanEverAffectNavigation(false);
	// SET EXPLICITLY EVEN THOUGH true IS THE ENGINE DEFAULT
	// (PrimitiveComponent.cpp:364). This is the flag SceneVisibility.cpp:1976 copies
	// into FSkyMeshBatch::bVisibleInRealTimeSkyCapture, and it is therefore the one
	// bit that decides whether phase S2's starlight ever reaches the SkyLight at
	// all. A silent default on the load-bearing bit of an unbuilt phase is how S2
	// turns into an afternoon of wondering why the gain does nothing.
	AtmosphereDomeMesh->bVisibleInRealTimeSkyCaptures = true;
	// Starts HIDDEN and is shown by ApplyDomeCvars on the first tick, unlike the
	// star dome. That ordering is not cosmetic: showing it flips
	// View.bSceneHasSkyMaterial and takes the sky away from the atmosphere pass, so
	// the one code path that decides whether that is allowed to happen --
	// ApplyDomeCvars, which also holds the "material failed to load" refusal -- must
	// be the only thing that ever turns it on.
	AtmosphereDomeMesh->SetVisibility(false);

	if (SphereMeshFinder.Succeeded())
	{
		AtmosphereDomeMesh->SetStaticMesh(SphereMeshFinder.Object);
	}

	// SAME CONSTRUCTOR-TIME LOAD as the night sky material above, and the fallback
	// here is the worst of the three in this module. The ocean's fallback is a grey
	// plane and the star dome's is an empty sky; this one is an IsSky dome painting
	// the engine default material's checkerboard-grey OVER A SKY THE ATMOSPHERE
	// PASS HAS ALREADY BEEN TOLD NOT TO PAINT. bAtmosphereMaterialValid is what
	// stops that shipping: BeginPlay logs an Error and ApplyDomeCvars refuses to
	// show the component at all, so the frame degrades to exactly today's frame
	// rather than to a broken one.
	UMaterialInterface* AtmosphereDomeMaterial = Cast<UMaterialInterface>(
		StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, kAtmosphereDomeMaterialPath));
	if (AtmosphereDomeMaterial)
	{
		AtmosphereDomeMesh->SetMaterial(0, AtmosphereDomeMaterial);
		bAtmosphereMaterialValid = true;
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
	// --- the IsSky dome's material, which is the loud one --------------------
	//
	// ERROR, AND THE DOME IS THEN REFUSED. Everything else in this file degrades to
	// "a bit of the sky is missing"; this one degrades to "the sky is painted wrong
	// AND the atmosphere has been told not to paint it", because
	// View.bSceneHasSkyMaterial is set by the mere PRESENCE of a visible sky-material
	// primitive (SceneVisibility.cpp:2096) and does not care whether the material is
	// the intended one. The engine ships its own on-screen warning for the adjacent
	// misconfiguration (ReflectionEnvironmentRealTimeCapture.cpp:313-316), which is a
	// fair signal of how easy this is to get wrong.
	//
	// So the choice made here is: no dome rather than a wrong dome. With the
	// component refused the frame is bit-for-bit the pre-S1 frame -- the atmosphere
	// keeps painting the sky, the star dome keeps drawing stars, and the SkyLight
	// keeps capturing the atmosphere alone. That is a state this project already
	// understands and has numbers for.
	if (!bAtmosphereMaterialValid)
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("VoxelSky atmosphere dome: %s did not load. REFUSING TO ENABLE THE DOME (voxel.Sky.AtmosphereDome ")
		       TEXT("is being treated as 0 for this run, whatever it is set to). An IsSky dome wearing the engine ")
		       TEXT("default material would still set View.bSceneHasSkyMaterial, which stops the SkyAtmosphere pass ")
		       TEXT("painting sky pixels (SkyAtmosphereRendering.cpp:2214) -- so the sky would be BOTH suppressed and ")
		       TEXT("wrong, and would read as permanent night rather than as a missing asset. Run ")
		       TEXT("Tools/create_sky_material.py and THEN Tools/create_sky_atmosphere_dome_material.py; the order ")
		       TEXT("matters, the first one recreates MPC_VoxelSky from scratch. Until then this run behaves exactly ")
		       TEXT("as it did before phase S1: the atmosphere paints the sky and the SkyLight captures it alone."),
		       kAtmosphereDomeMaterialPath);
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
	       TEXT("VoxelSky dome RESOLVED: Enabled=%d AtmosphereDome=%d (material=%s) RadiusUU=%.0f (%.1f km), ")
	       TEXT("source sphere radius %.1f UU -> uniform scale %.1f. Clipmap outer half-extent %.1f km, corner ")
	       TEXT("%.1f km; margin %.2fx. ONE radius serves both domes, deliberately -- it is a lower bound against ")
	       TEXT("that corner for each of them."),
	       VoxelSky::IsDomeEnabled() ? 1 : 0, VoxelSky::IsAtmosphereDomeEnabled() ? 1 : 0,
	       bAtmosphereMaterialValid ? TEXT("OK") : TEXT("MISSING -- dome refused"),
	       RadiusUU, RadiusUU / 100000.0, SourceRadiusUU,
	       SourceRadiusUU > 0.0 ? RadiusUU / SourceRadiusUU : 0.0,
	       AVoxelClipmapActor::OuterHalfExtentUU() / 100000.0, ClipmapCornerUU / 100000.0,
	       ClipmapCornerUU > 0.0 ? RadiusUU / ClipmapCornerUU : 0.0);

	if (RadiusUU <= ClipmapCornerUU)
	{
		// TWO DISTINCT FAILURES, ONE CAUSE, AND THE SECOND ONE IS WORSE. For the
		// star dome the symptom is subtractive: M_NightSky depth-tests, so distant
		// terrain draws in front of it and the stars vanish behind a horizon ring.
		// For the IsSky dome it is ADDITIVE and destructive -- EMeshPass::SkyPass
		// depth-tests with CF_DepthNearOrEqual, so a dome NEARER than a mountain
		// passes the test and paints sky OVER the mountain in the base-pass colour
		// target, deleting the vista. Same number fixes both.
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("VoxelSky dome: radius %.0f UU (%.1f km) does NOT exceed the clipmap's corner distance ")
		       TEXT("%.0f UU (%.1f km). Both domes depth-test. M_NightSky loses -- distant terrain is drawn IN ")
		       TEXT("FRONT of the stars and the sky reads as empty over most of the horizon. ")
		       TEXT("M_SkyAtmosphereDome WINS, which is worse: it is nearer than the distant clipmap, so it ")
		       TEXT("passes CF_DepthNearOrEqual and PAINTS SKY OVER THE DISTANT TERRAIN. Raise ")
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

	// Skipped only while BOTH domes are hidden: a dome nobody can see does not need
	// to be anywhere. Either one visible is enough to require the follow, because
	// they share this actor's transform -- gating on AppliedEnabled alone would
	// strand the IsSky dome around wherever the camera was whenever
	// voxel.Sky.DomeEnabled happened to be 0, which is a legitimate configuration
	// (the star dome is decorative; the IsSky dome is what paints the sky).
	//
	// Either dome is recentred again the moment it is shown (ApplyDomeCvars logs
	// each transition), so a toggle cannot leave one stranded.
	if (AppliedEnabled == 1 || AppliedAtmosphereEnabled == 1)
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
		// COMPONENT VISIBILITY ONLY -- the SetActorHiddenInGame that used to sit
		// here has gone, and it had to. AActor::SetActorHiddenInGame hides every
		// component on the actor, so once the IsSky dome became a second component
		// here, voxel.Sky.DomeEnabled 0 would have hidden that one too. That is not
		// a tidiness point: it would have made voxel.Sky.DomeEnabled silently
		// decide who paints the sky, and it would have coupled the two arms of S1's
		// A/B to a cvar the gate does not touch. The star dome's own contract says
		// what it is allowed to do -- "0 HIDES THE MESH ONLY" -- and
		// SetVisibility(false) already delivers exactly that, so nothing about the
		// star dome's behaviour changes.
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

	// --- the IsSky dome, switched independently -------------------------------
	//
	// NOT GATED ON voxel.Sky.Enabled, unlike the star dome above, and the asymmetry
	// is deliberate. The star dome is gated because voxel.Sky.Enabled 0 stops the
	// MPC writes and would leave it holding a stale StarBrightness -- a star field
	// over a lit afternoon. This dome's MAIN-VIEW output reads no MPC parameter at
	// all: SkyAtmosphereViewLuminance and SkyAtmosphereLightDiskLuminance both come
	// from the atmosphere's own LUTs, which the frozen static rig still populates.
	// Its only MPC-driven term is the star branch, and that is behind a
	// ReflectionCapturePassSwitch and therefore unreachable on screen.
	//
	// So there is no stale-parameter hazard here, and hiding it on Enabled 0 would
	// instead ADD one: the sky would change hands (atmosphere pass on, dome off) at
	// the same moment the rig froze, which makes voxel.Sky.Enabled stop being the
	// clean "return to the pre-W4 pose" control it is documented as. The dome is a
	// faithful stand-in for the atmosphere pass at every hour and in every rig
	// state, or it is broken -- that is what S1 measures.
	//
	// REFUSED OUTRIGHT when the material did not load; see bAtmosphereMaterialValid
	// in the header, and the Error at BeginPlay.
	const int32 bAtmosphereEnabled = (bAtmosphereMaterialValid && VoxelSky::IsAtmosphereDomeEnabled()) ? 1 : 0;
	if (AtmosphereDomeMesh && bAtmosphereEnabled != AppliedAtmosphereEnabled)
	{
		AppliedAtmosphereEnabled = bAtmosphereEnabled;
		AtmosphereDomeMesh->SetVisibility(bAtmosphereEnabled != 0);
		if (bAtmosphereEnabled != 0)
		{
			// Recentre before the first visible frame, same as the star dome. It
			// matters less here (the shading is a function of view direction alone,
			// see the class comment) but the camera being INSIDE the dome is what
			// makes that true, and this is what keeps it an identity.
			UpdateFollowCamera();
		}
		// LOGGED AS AN ARM, NOT AS A SETTING, because that is what a ladder rung
		// needs to be able to read back. The resolved value is printed alongside the
		// raw cvar so a refusal (material missing) can never look like a 0 that
		// someone typed.
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelSky ATMOSPHERE DOME %s (voxel.Sky.AtmosphereDome=%d, materialValid=%d -> resolved %d). ")
		       TEXT("SHOWN means THIS MESH paints the sky and the SkyAtmosphere's full-screen pass does not ")
		       TEXT("(View.bSceneHasSkyMaterial goes true, SkyAtmosphereRendering.cpp:2214); HIDDEN hands the sky ")
		       TEXT("back to that pass. The two must agree to within 2/255 of mean luma at every hour -- that is ")
		       TEXT("the whole of phase S1, and this line is what tells a capture which arm it is."),
		       bAtmosphereEnabled != 0 ? TEXT("SHOWN") : TEXT("HIDDEN"),
		       VoxelSky::IsAtmosphereDomeEnabled() ? 1 : 0, bAtmosphereMaterialValid ? 1 : 0, bAtmosphereEnabled);
	}

	const double RadiusUU = VoxelSky::GetDomeRadiusUU();
	if (SourceRadiusUU > 0.0 && !FMath::IsNearlyEqual(RadiusUU, AppliedRadiusUU, 1.0))
	{
		AppliedRadiusUU = RadiusUU;
		const double Scale = RadiusUU / SourceRadiusUU;
		// ONE WRITE FOR BOTH DOMES. AtmosphereDomeMesh is attached to DomeMesh with
		// an identity relative transform, so this uniform scale reaches it through
		// the attachment rather than through a second SetRelativeScale3D that could
		// drift from this one. See the header on AtmosphereDomeMesh: the radius is a
		// lower bound against the clipmap corner for both domes, and two independent
		// writes would be two chances to lose that bound.
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
