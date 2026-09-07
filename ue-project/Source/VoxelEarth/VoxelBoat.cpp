#include "VoxelBoat.h"

#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h" // TActorIterator
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h" // the WaveMirror fingerprint check
#include "PhysicsEngine/BodyInstance.h"
#include "UObject/ConstructorHelpers.h"

#include "VoxelAssetBody.h"
#include "VoxelCoords.h"
#include "VoxelDebug.h" // LogVoxelWater -- the wake half of this actor
#include "VoxelEarth.h" // LogVoxelEarth -- the chassis half
#include "VoxelRippleField.h"
#include "VoxelWaterSubsystem.h"
#include "VoxelWaveMirror.generated.h" // the CPU mirror of the drawn wave field
#include "VoxelWeatherSubsystem.h"     // the published wind the mirror phase needs
#include "VoxelWorldSubsystem.h"

// ---------------------------------------------------------------------------
// CONSOLE VARIABLES AND COMMANDS
// ---------------------------------------------------------------------------
//
// EVERY ARM HAS AN OFF SWITCH AND EVERY OFF SWITCH IS A CONTROL, not a
// convenience. Buoyancy off is the arm that proves the buoyancy forces are what
// is holding the boat up; wake off is the byte-identical control for any perf
// leg on the ripple injection. That is the house rule (docs/water-architecture.md
// §4) and it is cheaper to write now than to add during a bisect.

TAutoConsoleVariable<FString> CVarVoxelBoatAsset(
	TEXT("voxel.Boat.Asset"), TEXT("canoe"),
	TEXT("asset-forge library name for the boat's visual grid. Resolved by ")
	TEXT("UVoxelAssetBodyComponent::ResolveVxaPath against voxel.AssetBody.LibraryRoot; when no ")
	TEXT("file is found the boat renders a hull-shaped placeholder at the same pitch and SAYS SO ")
	TEXT("in the log, so a grey box is never mistaken for the asset."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelBoatBuoyancy(
	TEXT("voxel.Boat.Buoyancy"), true,
	TEXT("The four hull-corner spring-dampers. 0 leaves the boat a plain falling Chaos body, which ")
	TEXT("is the A arm for 'is it floating, or is it stuck on something': with this off it MUST ")
	TEXT("sink through the surface."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelBoatWake(
	TEXT("voxel.Boat.Wake"), true,
	TEXT("Inject the bow and transom wake into the ripple field. 0 is byte-identical to a run with ")
	TEXT("no boat as far as the ripple field is concerned -- the control arm for any timing on it. ")
	TEXT("voxel.Water.Ripple.Stat's injected/dropped counters are how you tell this engaged."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarVoxelBoatWaveBobGain(
	TEXT("voxel.Boat.WaveBobGain"), 6.0f,
	TEXT("Multiplier on the wave height the buoyancy probes ride (owner directive 2026-09-06: 'I ")
	TEXT("want to see exaggerated bobbing and buoyancy effects for all floating vessels'). 1.0 = ")
	TEXT("physically matched to the drawn surface (measured invisible: kWaveWpoFraction of +-2 cm ")
	TEXT("waves is millimetres of hull travel); the default exaggerates by design and the owner ")
	TEXT("dials it live. Applies to every vessel riding this buoyancy (the raft inherits it)."),
	ECVF_Default);
TAutoConsoleVariable<float> CVarVoxelBoatWakeGain(
	TEXT("voxel.Boat.WakeGain"), 3.0f,
	TEXT("Multiplier on the bow/transom/slam splat strengths (same 2026-09-06 owner directive: the ")
	TEXT("verified-LIVE wake measured 1.5 cm of state height -- honest but invisible). Dials the ")
	TEXT("wake's visual weight without touching the ripple sim's physics constants."),
	ECVF_Default);
TAutoConsoleVariable<bool> CVarVoxelBoatWaveBob(
	TEXT("voxel.Boat.WaveBob"), true,
	TEXT("Couple the buoyancy probes to the CPU wave mirror (VoxelWaveMirror.generated.h): each ")
	TEXT("probe's surface becomes the datum PLUS the drawn wind-wave height at that point, so the ")
	TEXT("hull bobs on the waves the pixels show (owner overrule of D2's flat-datum v1, ")
	TEXT("2026-09-05). 0 = the flat-datum arm, byte-identical to pre-wave behaviour -- the A/B for ")
	TEXT("'is the bobbing the mirror or a probe bug'. Also forced flat, WITH a log line, when the ")
	TEXT("water material's WaveMirrorFingerprint does not match this build's mirror header -- ")
	TEXT("never bob boats on stale math."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelBoatDrive(
	TEXT("voxel.Boat.Drive"), true,
	TEXT("Throttle and rudder. 0 leaves a boat that floats and drifts and nothing else -- the arm ")
	TEXT("that separates 'the hull is unstable' from 'the drive is fighting the hull'."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelBoatDebugDraw(
	TEXT("voxel.Boat.DebugDraw"), false,
	TEXT("Draw the four buoyancy probes, the water surface they found and the bow probe. Costs a ")
	TEXT("few debug lines per tick; off by default because a capture must never photograph an ")
	TEXT("instrument."),
	ECVF_Default);

namespace VoxelBoatLocal
{
// Prefixed rather than anonymous: this module is a unity blob and two
// internal-linkage helpers of one name in one blob is the collision that
// stopped it compiling on 2026-08-23 (VoxelRippleField.h records it).

// A pawn's world position, or the world origin. Used for the sleep test and for
// the spawn command's anchor -- the SAME anchor UVoxelRippleFieldSubsystem::
// GetCameraXY uses, deliberately, so a boat that thinks it is near the camera
// and a ripple window that thinks otherwise cannot both be right.
bool CameraLocation(const UWorld* World, FVector& Out)
{
	const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return false;
	}
	Out = Pawn->GetActorLocation();
	return true;
}

// Impact speed -> ripple strength fraction. The SAME curve the ripple field's
// own auto-watcher uses, from the same two constants, for the reason those
// constants were pulled into a shared namespace in the first place: a wake and a
// splash that disagreed about how hard the water had been hit would be two
// tunings of one effect.
double ImpactFraction(double SpeedUUPerSec)
{
	const double MPS = FMath::Max(0.0, SpeedUUPerSec) / 100.0;
	return FMath::Clamp(MPS / VoxelRipple::kFullImpactSpeedMPS, VoxelRipple::kMinImpactFraction, 1.0);
}

// --- the WaveMirror staleness guard ----------------------------------------
//
// The mirror header's contract (VoxelWaveMirror.generated.h "STALENESS"): the
// regenerated water materials carry the header's fingerprint as the scalar
// parameter default "WaveMirrorFingerprint", and a mismatch means header and
// asset were built from DIFFERENT field math -- boats must ride the flat
// datum, never bob on the disagreement. Checked ONCE per process at the first
// boat's BeginPlay (a startup-ish place this file owns), against
// /Game/Voxel/M_WaterVoxel -- both water materials are regenerated by the one
// chain, so one asset answers for both. A material WITHOUT the parameter is a
// MISMATCH with its own log wording: the scalar only exists after the pending
// full chain regen, so the feature arms itself the run after that lands.
enum class EWaveMirrorCheck : uint8
{
	Unchecked,
	OK,
	Mismatch
};
static EWaveMirrorCheck GWaveMirrorCheck = EWaveMirrorCheck::Unchecked;

bool WaveMirrorFingerprintOK()
{
	if (GWaveMirrorCheck == EWaveMirrorCheck::Unchecked)
	{
		GWaveMirrorCheck = EWaveMirrorCheck::Mismatch; // guilty until proven
		UMaterialInterface* Mat = Cast<UMaterialInterface>(
			StaticLoadObject(UMaterialInterface::StaticClass(), nullptr,
			                 TEXT("/Game/Voxel/M_WaterVoxel.M_WaterVoxel")));
		float FP = 0.f;
		const bool bHasParam =
			Mat
			&& Mat->GetScalarParameterDefaultValue(
				   FHashedMaterialParameterInfo(TEXT("WaveMirrorFingerprint")), FP);
		if (bHasParam && FP == float(VOXELWAVEMIRROR_FINGERPRINT))
		{
			GWaveMirrorCheck = EWaveMirrorCheck::OK;
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("WaveMirror: fingerprint OK (0x%X) -- wave-coupled buoyancy armed ")
			       TEXT("(voxel.Boat.WaveBob)."),
			       VOXELWAVEMIRROR_FINGERPRINT);
		}
		else if (!bHasParam)
		{
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("WaveMirror: fingerprint MISMATCH (boats ride the flat datum) -- ")
			       TEXT("WaveMirrorFingerprint parameter absent from %s. Expected on materials ")
			       TEXT("regenerated after the mirror-emitting chain; the wave term arms itself ")
			       TEXT("when that regen lands."),
			       Mat ? TEXT("/Game/Voxel/M_WaterVoxel") : TEXT("a material that FAILED TO LOAD"));
		}
		else
		{
			UE_LOG(LogVoxelEarth, Warning,
			       TEXT("WaveMirror: fingerprint MISMATCH (boats ride the flat datum) -- material ")
			       TEXT("has 0x%X, this build's header has 0x%X. Header and asset were built from ")
			       TEXT("different field math: regenerate one of them."),
			       uint32(FP), VOXELWAVEMIRROR_FINGERPRINT);
		}
	}
	return GWaveMirrorCheck == EWaveMirrorCheck::OK;
}
} // namespace VoxelBoatLocal

AVoxelBoat::AVoxelBoat()
{
	PrimaryActorTick.bCanEverTick = true;
	// AFTER physics. The buoyancy forces are read back as a transform by the
	// solver, and the wake is swept from where the hull ACTUALLY ended up; a
	// pre-physics tick would sweep from where it was asked to go.
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	// Never auto-possessed. A boat is entered, and the plan's D3 says by which
	// key; a boat that grabbed player 0 on spawn would take the world away from
	// the fly pawn the moment a spawn command ran.
	AutoPossessPlayer = EAutoReceiveInput::Disabled;
	// The hull's heading is a physics result, not a controller value. Letting
	// the controller drive yaw here would fight the rudder every frame.
	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	PhysicsBody = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PhysicsBody"));
	SetRootComponent(PhysicsBody);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		PhysicsBody->SetStaticMesh(CubeFinder.Object);
	}
	PhysicsBody->SetVisibility(false);
	PhysicsBody->SetMobility(EComponentMobility::Movable);

	Body = CreateDefaultSubobject<UVoxelAssetBodyComponent>(TEXT("AssetBody"));
	Body->SetupAttachment(PhysicsBody);
	Body->FallbackShape = EVoxelAssetBodyFallback::Hull;
	Body->FallbackSizeM = FVector(4.0, 0.9, 0.45);
	Body->PlaceholderTint = FLinearColor(0.42f, 0.28f, 0.16f, 1.f);

	// The water-exclusion volume (contract: Tools/water_hull_mask_graph.py; see
	// the member comment). CUSTOM DEPTH ONLY: main pass off AND scene depth
	// prepass off -- a volume that wrote scene depth would occlude the very
	// water pixels the mask is supposed to test, not just the ones inside the
	// hull. The engine cube is closed and outward-facing, which is all the
	// contract's near-shell test needs; sized in BeginPlay once the hull
	// geometry is adopted from the asset.
	ExclusionVolume = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WaterExclusionVolume"));
	ExclusionVolume->SetupAttachment(PhysicsBody);
	if (CubeFinder.Succeeded())
	{
		ExclusionVolume->SetStaticMesh(CubeFinder.Object);
	}
	ExclusionVolume->SetRenderInMainPass(false);
	ExclusionVolume->SetRenderInDepthPass(false);
	ExclusionVolume->SetRenderCustomDepth(true);
	// Bit 0 = water exclusion. The registry of stencil bits lives in
	// Tools/water_hull_mask_graph.py's docstring; claim new bits there.
	ExclusionVolume->SetCustomDepthStencilValue(1);
	ExclusionVolume->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ExclusionVolume->SetCastShadow(false);
	ExclusionVolume->SetMobility(EComponentMobility::Movable);

	// The ends box: identical contract, second footprint (see the header
	// comment). Two boxes because ONE box can only fit the midsection of a
	// tapering hull -- the 2026-09-06 "TIGHTENED" note below records the
	// rectangle artefact an oversized single box paints.
	ExclusionVolumeEnds = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WaterExclusionVolumeEnds"));
	ExclusionVolumeEnds->SetupAttachment(PhysicsBody);
	if (CubeFinder.Succeeded())
	{
		ExclusionVolumeEnds->SetStaticMesh(CubeFinder.Object);
	}
	ExclusionVolumeEnds->SetRenderInMainPass(false);
	ExclusionVolumeEnds->SetRenderInDepthPass(false);
	ExclusionVolumeEnds->SetRenderCustomDepth(true);
	ExclusionVolumeEnds->SetCustomDepthStencilValue(1);
	ExclusionVolumeEnds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ExclusionVolumeEnds->SetCastShadow(false);
	ExclusionVolumeEnds->SetMobility(EComponentMobility::Movable);

	CameraArm = CreateDefaultSubobject<USceneComponent>(TEXT("CameraArm"));
	CameraArm->SetupAttachment(PhysicsBody);

	ChaseCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("ChaseCamera"));
	ChaseCamera->SetupAttachment(CameraArm);
	ChaseCamera->SetRelativeLocation(
		FVector(-VoxelBoatTuning::CameraBackUU, 0.0, VoxelBoatTuning::CameraUpUU));
}

void AVoxelBoat::BeginPlay()
{
	Super::BeginPlay();

	// The visual grid first, because the HULL GEOMETRY IS TAKEN FROM IT. Doing
	// this the other way round would place the probes against a constant and then
	// draw a hull somewhere else, which is a mismatch nothing on screen reports.
	if (Body)
	{
		Body->AssetName = CVarVoxelBoatAsset.GetValueOnGameThread();
		Body->Build();
		AdoptHullFromBody();
	}

	// --- the water-exclusion volume, sized against the adopted hull -----------
	//
	// A v1 box (the contract's stated v1; the fitted cockpit bathtub is a later
	// refinement): slightly inboard of the hull bounds in plan, so the volume's
	// near shell stays behind the drawn planking, and vertically from the inner
	// hull bottom to ~0.5 m above the at-rest waterline (keel + rest draft) --
	// the contract's "above the highest wave crest that can cross the gunwale".
	// The material tests the near shell within a 3 m band; the whole term is
	// inert until r.CustomDepth=3 (config, owned by the coordinator) and the
	// regenerated materials land, and inert is the safe direction.
	if (ExclusionVolume)
	{
		// TIGHTENED 2026-09-06 after the first in-water frame: at 0.90/0.85 the
		// box was wider than the tapering hull at bow and stern, and the mask
		// carved a visible RECTANGLE of water beyond the gunwales. A box can
		// only ever fit the hull's midsection, so it stays strictly inside the
		// taper: the bow/stern thirds keep a sliver of water against the shell
		// (correct-looking -- a canoe's ends sit low), and the cockpit stays
		// dry, which is the owner-visible requirement. A hull-shaped exclusion
		// mesh is the v2 upgrade if the sliver ever bothers anyone.
		const double InnerHalfL = HalfLengthUU * 0.62;
		const double InnerHalfB = HalfBeamUU * 0.55;
		const double BottomZ = KeelOffsetUU + 4.0; // just inside the shell
		// THE LID STAYS AT WATERLINE+10, AND THE 2026-09-06 ATTEMPT TO RAISE IT
		// IS RECORDED HERE SO IT IS NOT REPEATED. Raising it to gunwale-6 (34 UU
		// above the waterline on the shipped canoe) brought the beside-hull
		// artefact straight back: the owner's grazing-angle screenshot shows
		// water carved away BEYOND the far gunwale. The geometric argument the
		// raise was made on was WRONG -- it assumed a sightline reaching water
		// outside the hull must clear BOTH gunwales, but a near-horizontal ray
		// enters above the NEAR gunwale, crosses the box's above-water slab, and
		// exits past the FAR one onto open water, which the mask then kills. The
		// taller the slab, the wider the band of angles that do this.
		//
		// The ends-clipping the raise was meant to fix was never a height
		// problem: it was COVERAGE. The midship box reaches 0.62 of the hull
		// length, so the bow and stern thirds had no exclusion at all, and 6x
		// bobbing simply made the gap visible. The ends box below supplies that
		// coverage at this same lid height, which is the fix that does not trade
		// one artefact for the other.
		//
		// The remaining honest limit: a crest taller than 10 UU above the rest
		// waterline can still wash the shell for a frame. A hull-shaped mesh --
		// or the camera-independent version of this test, an oriented-box
		// containment test done in the water material against the boat's
		// published transform instead of a screen-space stencil -- is the v2
		// that removes the trade entirely.
		const double TopZ = KeelOffsetUU + VoxelBoatTuning::RestDraftUU + 10.0;
		const double HalfH = FMath::Max(1.0, 0.5 * (TopZ - BottomZ));
		ExclusionVolume->SetRelativeLocation(FVector(0.0, 0.0, 0.5 * (TopZ + BottomZ)));
		// The engine cube is 100 UU on a side, so scale = half-extent / 50.
		ExclusionVolume->SetRelativeScale3D(
			FVector(InnerHalfL / 50.0, InnerHalfB / 50.0, HalfH / 50.0));
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("Boat: hull water-exclusion volume %.2f x %.2f x %.2f m, top %.0f UU above the ")
		       TEXT("at-rest waterline (custom stencil bit 0)."),
		       2.0 * InnerHalfL / 100.0, 2.0 * InnerHalfB / 100.0, 2.0 * HalfH / 100.0,
		       TopZ - (KeelOffsetUU + VoxelBoatTuning::RestDraftUU));
	}
	if (ExclusionVolumeEnds)
	{
		// The ends box, same lid and floor, covering the bow/stern the midship
		// box leaves bare. Narrow enough (0.26 beam) to sit inside the taper
		// out to 0.92 of the length: an elliptic canoe plan at 0.92L still
		// carries ~0.39 of max beam, so 0.26 keeps planking between the box
		// shell and the water everywhere it reaches. The corners past 0.92L
		// keep their sliver, now inches wide instead of a third of the hull.
		const double EndsHalfL = HalfLengthUU * 0.92;
		const double EndsHalfB = HalfBeamUU * 0.26;
		const double BottomZ = KeelOffsetUU + 4.0;
		const double TopZ = KeelOffsetUU + VoxelBoatTuning::RestDraftUU + 10.0;
		const double HalfH = FMath::Max(1.0, 0.5 * (TopZ - BottomZ));
		ExclusionVolumeEnds->SetRelativeLocation(FVector(0.0, 0.0, 0.5 * (TopZ + BottomZ)));
		ExclusionVolumeEnds->SetRelativeScale3D(
			FVector(EndsHalfL / 50.0, EndsHalfB / 50.0, HalfH / 50.0));
	}

	// The WaveMirror staleness guard, once per process (see the helper): logs
	// 'WaveMirror: fingerprint OK/MISMATCH' and latches which datum the wave
	// term below is allowed to ride.
	VoxelBoatLocal::WaveMirrorFingerprintOK();

	if (PhysicsBody)
	{
		PhysicsBody->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		// ALL RESPONSES IGNORE. Terrain is not a Chaos body, and a boat that
		// pushed the pawn around would be a second, conflicting collision source
		// (AVoxelDebris' rule, unchanged).
		PhysicsBody->SetCollisionResponseToAllChannels(ECR_Ignore);
		PhysicsBody->SetEnableGravity(true);
		PhysicsBody->SetMassOverrideInKg(NAME_None, float(VoxelBoatTuning::MassKg), true);

		// THE BODY IS A 1 m CUBE AND THE BOAT IS NOT. Left alone, the solver
		// would give a 4 m hull the rotational inertia of a metre box: it would
		// spin about its own centre like a compass needle, which reads as
		// weightlessness rather than as a boat. The tensor scale is the ratio of
		// a box of the hull's dimensions to the unit cube the body actually is,
		// per axis. Not a tuning number -- geometry.
		const double L = 2.0 * HalfLengthUU / 100.0; // metres
		const double W = 2.0 * HalfBeamUU / 100.0;
		const double H = FMath::Max(0.2, 2.0 * FMath::Abs(KeelOffsetUU) / 100.0);
		const double CubeI = 1.0 / 6.0; // a 1 m cube, per unit mass
		const FVector TensorScale(((W * W + H * H) / 12.0) / CubeI, ((L * L + H * H) / 12.0) / CubeI,
		                          ((L * L + W * W) / 12.0) / CubeI);
		// Set on the FBodyInstance rather than through a component setter --
		// UPrimitiveComponent has no SetInertiaTensorScale, and the value is only
		// consumed when mass properties are recomputed, so both halves are needed.
		PhysicsBody->BodyInstance.InertiaTensorScale = TensorScale;
		if (FBodyInstance* BI = PhysicsBody->GetBodyInstance())
		{
			BI->InertiaTensorScale = TensorScale;
			BI->UpdateMassProperties();
		}

		// Damping is applied by hand below (keel-asymmetric in the BODY frame),
		// so the engine's isotropic channels must be zero or the two compound.
		PhysicsBody->SetLinearDamping(0.f);
		PhysicsBody->SetAngularDamping(0.f);
		PhysicsBody->SetSimulatePhysics(true);
	}

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelBoat spawned at (%.0f,%.0f,%.0f): hull %.2f x %.2f m, keel %.1f UU below ")
	       TEXT("origin, mass %.0f kg, rest draft %.0f UU. Visual: %s (%d instances, %d batches, ")
	       TEXT("pitch %.1f mm)."),
	       GetActorLocation().X, GetActorLocation().Y, GetActorLocation().Z, 2.0 * HalfLengthUU / 100.0,
	       2.0 * HalfBeamUU / 100.0, -KeelOffsetUU, VoxelBoatTuning::MassKg, VoxelBoatTuning::RestDraftUU,
	       (Body && !Body->IsPlaceholder()) ? *Body->GetResolvedPath() : TEXT("PLACEHOLDER"),
	       Body ? Body->GetInstanceCount() : 0, Body ? Body->GetBatchCount() : 0,
	       Body ? Body->GetPitchUU() * 10.0 : 0.0);
}

void AVoxelBoat::EndPlay(const EEndPlayReason::Type Reason)
{
	// NEVER STRAND THE PLAYER. A boat destroyed while crewed -- by a console
	// command mid-game -- must hand the pawn back first, or the controller is
	// left possessing nothing and the session is over. ONLY on Destroyed:
	// during level transition / quit / PIE end the stored pawn is tearing down
	// too, and possessing or teleporting it there is a use-after-teardown
	// (review finding #9).
	if (Reason == EEndPlayReason::Destroyed && StoredPawn.IsValid())
	{
		ExitToStoredPawn();
	}
	Super::EndPlay(Reason);
}

void AVoxelBoat::AdoptHullFromBody()
{
	if (!Body)
	{
		return;
	}
	const FBox B = Body->GetLocalBoundsUU();
	if (!B.IsValid || B.GetExtent().IsNearlyZero())
	{
		// Keep the tuning defaults and say so. A silently-defaulted hull that
		// happens to be about the right size is the reading nobody questions.
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelBoat: the asset body reported no bounds; keeping the VoxelBoatTuning hull ")
		       TEXT("(%.2f x %.2f m). The probes are now against a constant rather than against what ")
		       TEXT("is on screen."),
		       2.0 * HalfLengthUU / 100.0, 2.0 * HalfBeamUU / 100.0);
		return;
	}

	// Centre the drawn hull on the actor origin, which is where the solver puts
	// the centre of mass. Then the probes, the camera and the wake points are all
	// expressed against the same origin as the picture.
	Body->SetRelativeLocation(-B.GetCenter());

	const FVector Ext = B.GetExtent();
	HalfLengthUU = Ext.X;
	HalfBeamUU = Ext.Y;
	KeelOffsetUU = -Ext.Z;
}

void AVoxelBoat::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (DeltaSeconds <= 0.f || !PhysicsBody)
	{
		return;
	}

	// --- the unstreamed-tile rule, half two -----------------------------------
	//
	// Datum-only queries make a distant boat CORRECT; sleeping makes it free.
	// The possessed boat is always at distance zero from the anchor (the anchor
	// IS the possessed pawn), so this can never put the player's own boat to
	// sleep -- which is the property that makes a hard cutoff safe here where it
	// would not be elsewhere.
	FVector CamLoc;
	const bool bHaveCam = VoxelBoatLocal::CameraLocation(GetWorld(), CamLoc);
	const bool bFar = bHaveCam
	                  && FVector::DistSquared(CamLoc, GetActorLocation())
	                         > FMath::Square(VoxelBoatTuning::SleepRadiusUU);
	if (bFar != bAsleep)
	{
		bAsleep = bFar;
		PhysicsBody->SetSimulatePhysics(!bAsleep);
		UE_LOG(LogVoxelEarth, Verbose, TEXT("VoxelBoat: physics %s at %.0f m from the anchor."),
		       bAsleep ? TEXT("ASLEEP") : TEXT("awake"),
		       bHaveCam ? FVector::Dist(CamLoc, GetActorLocation()) / 100.0 : 0.0);
	}
	if (bAsleep)
	{
		++SleepTicks;
		// The wake history goes with it: waking up and sweeping from where the
		// boat was a minute ago would draw a wake across the whole lake.
		bHaveLastWake = false;
		return;
	}

	TickBuoyancy(DeltaSeconds);
	TickDrive(DeltaSeconds);
	TickGround(DeltaSeconds);
	TickWake(DeltaSeconds);
	TickCamera(DeltaSeconds);
}

void AVoxelBoat::TickBuoyancy(float DeltaSeconds)
{
	using namespace VoxelBoatTuning;

	UWorld* World = GetWorld();
	UVoxelWaterSubsystem* Water = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water || !CVarVoxelBoatBuoyancy.GetValueOnGameThread())
	{
		bProbeSeen = false; // a re-enable must not fire four slam splashes
		// And the wet flags must clear WITH it: drive and wake read
		// bProbeWasSubmerged as "am I in the water this tick", so leaving the
		// last wet values in place would give a buoyancy-off boat full throttle
		// and a wake all the way to the lake bed -- the A arm holding up the
		// thing it exists to isolate (review finding #4).
		for (bool& bWet : bProbeWasSubmerged) { bWet = false; }
		return;
	}

	// K is DERIVED, not tuned: the spring that holds exactly this boat's weight
	// at exactly RestDraftUU of submersion. If it floats too low, RestDraftUU is
	// the wrong number -- and that is a measurable fact about a hull rather than
	// a spring constant nobody can check.
	const double K = MassKg * 980.0 / (double(NumProbes) * RestDraftUU);
	const double MassPerProbe = MassKg / double(NumProbes);
	const double D = 2.0 * BuoyancyDampingRatio * FMath::Sqrt(K * MassPerProbe);
	// Numerical ceiling only. Three times the boat's whole weight from ONE probe
	// is not a hull on water, it is a solver spike, and letting one through
	// launches the boat.
	const double MaxProbeForce = 3.0 * MassKg * 980.0;

	// --- the wave term (owner overrule of D2's flat datum, 2026-09-05) -------
	//
	// Per probe, the surface is the DATUM plus the CPU mirror of the drawn
	// wind-wave field (VoxelWaveMirror.generated.h) -- the same eight-octave
	// sum the pixels evaluate, times kWaveWpoFraction because that is how much
	// of the field the drawn surface actually displaces. PHASE COHERENCE is
	// the whole game and the mirror header dictates both inputs:
	//   * TIME: World->GetTimeSeconds() * voxel.Water.WaveTimeScale, read
	//     straight off the cvar (the subsystem pushes the same value to the
	//     MPC each tick; reading the cvar keeps this file out of the water
	//     subsystem, which is another lane's tonight).
	//   * WIND: the PUBLISHED frame's camera wind (GetWeatherState().Wind) --
	//     what the materials see -- NOT SampleWindAtWorldUU at the hull, which
	//     is the field somewhere the pixels are not using. Residual, honestly:
	//     the MPC carries the SMOOTHED vector, which has no public accessor;
	//     the raw published sample diverges from it only while the wind is
	//     actively changing, adding centimetres of transient phase error --
	//     inside the mirror's own stated few-centimetre acceptance band.
	// Gated by voxel.Boat.WaveBob (the flat-datum A/B arm) AND the fingerprint
	// guard: stale mirror math rides the flat datum, never the waves.
	const bool bWaveBob = CVarVoxelBoatWaveBob.GetValueOnGameThread()
	                      && VoxelBoatLocal::WaveMirrorFingerprintOK();
	float ScaledTimeS = 0.f;
	float WindNorthMS = 0.f, WindEastMS = 0.f;
	if (bWaveBob)
	{
		static IConsoleVariable* WaveTimeScaleCVar =
			IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Water.WaveTimeScale"));
		const float TimeScale = WaveTimeScaleCVar ? WaveTimeScaleCVar->GetFloat() : 1.0f;
		ScaledTimeS = float(World->GetTimeSeconds()) * TimeScale;
		if (const UVoxelWeatherSubsystem* Weather = World->GetSubsystem<UVoxelWeatherSubsystem>())
		{
			const FVoxelWindSample& W = Weather->GetWeatherState().Wind;
			WindNorthMS = float(W.NorthMps);
			WindEastMS = float(W.EastMps);
		}
	}

	const FTransform Xf = GetActorTransform();
	// Slightly inboard of the extreme corners: a canoe's stem and stern are above
	// the keel, so a probe exactly at the tip would measure a submersion the hull
	// does not have there.
	const double PX = HalfLengthUU * 0.85;
	const double PY = HalfBeamUU * 0.85;
	const FVector LocalProbes[NumProbes] = {
		FVector(PX, -PY, KeelOffsetUU),
		FVector(PX, PY, KeelOffsetUU),
		FVector(-PX, -PY, KeelOffsetUU),
		FVector(-PX, PY, KeelOffsetUU),
	};

	int32 WetThisTick = 0;
	for (int32 I = 0; I < NumProbes; ++I)
	{
		const FVector P = Xf.TransformPosition(LocalProbes[I]);
		++ProbeQueries;

		FWaterSurfaceSample S;
		const bool bWater = Water->WaterSurfaceZAtWorld(P.X, P.Y, S);
		// CA-only columns are the contract's documented blind spot -- a
		// player-poured pool or a PBF body has no datum anywhere, so there is no
		// surface to float on and the honest answer is no force, not a guess.
		// A boat in a poured pool sinks, and that is a stated limit rather than a
		// bug to rediscover.
		const bool bUsable = bWater && S.Kind != EWaterSurfaceKind::CAOnly;

		double Submersion = 0.0;
		double SurfaceZ = S.SurfaceZUU;
		if (bUsable)
		{
			if (bWaveBob)
			{
				// Mirror axes match the material's: its position/wind "x" is
				// world X = NORTH (PublishWind puts NorthMps in R), so world
				// X/Y in metres pairs with (NorthMps, EastMps). Field height
				// is METRES of open-water field; the drawn surface moves by
				// kWaveWpoFraction of it, converted to UU exactly here.
				SurfaceZ += double(VoxelWaveMirror::FieldHeightM(
					            float(P.X / 100.0), float(P.Y / 100.0), ScaledTimeS,
					            WindNorthMS, WindEastMS))
				            * VoxelWaveMirror::kWaveWpoFraction * 100.0
				            * FMath::Max(0.0, double(CVarVoxelBoatWaveBobGain.GetValueOnGameThread()));
			}
			Submersion = SurfaceZ - P.Z;
			// SHALLOWER THAN THE DRAFT MEANS THE BOTTOM IS IN THE WAY. Capping the
			// submersion by the water's own depth is what stops a puddle floating a
			// canoe, and it uses the SAME sample's GroundZUU rather than a second
			// raycast -- so the depth the buoyancy sees and the surface it settles
			// to can never come from two different columns.
			const double WaterDepth = FMath::Max(0.0, SurfaceZ - S.GroundZUU);
			Submersion = FMath::Min(Submersion, WaterDepth);
		}

		const bool bSubmerged = Submersion > 0.0;
		const FVector ProbeVel = PhysicsBody->GetPhysicsLinearVelocityAtPoint(P);

		if (bSubmerged)
		{
			++WetThisTick;
			++ProbesWet;
			const double Depth = FMath::Clamp(Submersion, 0.0, MaxDraftUU);
			// The floor at zero is PHYSICAL: water pushes and does not pull, so a
			// damper is never allowed to become a downward force. The residual
			// upward bias that leaves is absorbed by DragVertPerSec below.
			const double F = FMath::Clamp(Depth * K - D * ProbeVel.Z, 0.0, MaxProbeForce);
			PhysicsBody->AddForceAtLocation(FVector(0.0, 0.0, F), P);

			if (CVarVoxelBoatDebugDraw.GetValueOnGameThread())
			{
				// The WAVED surface the probe actually pressed against, so the
				// debug line and the force can never disagree about where the
				// water was.
				DrawDebugLine(World, P, FVector(P.X, P.Y, SurfaceZ), FColor::Cyan, false, 0.f, 0,
				              1.5f);
			}
		}

		// --- hull slam -----------------------------------------------------------
		//
		// On the CROSSING, not on the state. bProbeSeen is the same guard
		// AutoWatch uses for an actor spawned already underwater: a boat that
		// spawns floating must not splash four times on its first tick.
		if (bProbeSeen && bSubmerged && !bProbeWasSubmerged[I])
		{
			const double DownUU = -ProbeVel.Z;
			if (DownUU > SlamProbeSpeedUU)
			{
				UVoxelRippleFieldSubsystem::AddDisturbanceAt(
					World, P, float(SlamRadiusM),
					float(SlamStrengthM * VoxelBoatLocal::ImpactFraction(DownUU))
						* FMath::Max(0.f, CVarVoxelBoatWakeGain.GetValueOnGameThread()));
				++SlamSplashes;
			}
		}
		bProbeWasSubmerged[I] = bSubmerged;
	}
	bProbeSeen = true;

	// --- drag, in the BODY frame ---------------------------------------------
	//
	// THE KEEL IS THE WHOLE POINT. A hull is not isotropic: sideways drag being
	// an order of magnitude above forward drag is what makes a boat track instead
	// of skating, and it is the cheapest stand-in for a keel there is. Scaled by
	// how much of the hull is actually in the water, so a boat in the air
	// coasts and a boat pressed under does not.
	const double WetFraction = double(WetThisTick) / double(NumProbes);
	if (WetFraction > 0.0)
	{
		const FVector Fwd = Xf.GetUnitAxis(EAxis::X);
		const FVector Rt = Xf.GetUnitAxis(EAxis::Y);
		const FVector Up = Xf.GetUnitAxis(EAxis::Z);
		const FVector V = PhysicsBody->GetPhysicsLinearVelocity();
		const FVector DragAccel = -WetFraction
		                          * (DragLongPerSec * FVector::DotProduct(V, Fwd) * Fwd
		                             + DragLatPerSec * FVector::DotProduct(V, Rt) * Rt
		                             + DragVertPerSec * FVector::DotProduct(V, Up) * Up);
		// bAccelChange: these rates are per-second on VELOCITY, so they are
		// accelerations, and expressing them as such means the mass can change
		// without every drag number needing to.
		PhysicsBody->AddForce(DragAccel, NAME_None, true);

		const FVector W = PhysicsBody->GetPhysicsAngularVelocityInRadians();
		const FVector WYaw = FVector::DotProduct(W, Up) * Up;
		const FVector WRollPitch = W - WYaw;
		PhysicsBody->AddTorqueInRadians(
			-WetFraction * (AngularDampYawPerSec * WYaw + AngularDampRollPitchPerSec * WRollPitch),
			NAME_None, true);
	}
}

void AVoxelBoat::TickDrive(float DeltaSeconds)
{
	using namespace VoxelBoatTuning;

	if (!CVarVoxelBoatDrive.GetValueOnGameThread())
	{
		return;
	}
	// A propeller in the air does nothing. Requiring a wet probe THIS tick
	// rather than ever is what stops a beached boat driving itself up a hill.
	bool bAnyWet = false;
	for (int32 I = 0; I < NumProbes; ++I)
	{
		bAnyWet |= bProbeWasSubmerged[I];
	}
	if (!bAnyWet)
	{
		return;
	}

	const FTransform Xf = GetActorTransform();
	// PROJECTED ONTO THE WATER PLANE. A hull pitched bow-up on a wave must not be
	// able to thrust itself into the sky, and a hull pitched bow-down must not
	// drive itself under -- both of which a raw forward vector would do.
	FVector Fwd = Xf.GetUnitAxis(EAxis::X);
	Fwd.Z = 0.0;
	if (!Fwd.Normalize())
	{
		return; // the hull is vertical; there is no forward to speak of
	}

	// The scripted override (voxel.Boat.Throttle, see the header member) wins
	// while its clock runs; the axis value resumes the moment it expires.
	const bool bScriptDriving =
		ScriptThrottleUntilS > 0.0 && GetWorld()
		&& GetWorld()->GetTimeSeconds() < ScriptThrottleUntilS;
	const float EffectiveThrottle = bScriptDriving ? ScriptThrottle : ThrottleInput;
	if (!FMath::IsNearlyZero(EffectiveThrottle))
	{
		const double Accel = double(EffectiveThrottle)
		                     * ThrustAccelUUPerSec2
		                     * (EffectiveThrottle > 0.f ? 1.0 : ReverseThrustScale);
		PhysicsBody->AddForce(Fwd * Accel, NAME_None, true);
	}

	if (!FMath::IsNearlyZero(SteerInput))
	{
		// A RUDDER DOES NOTHING AT REST, which is correct and is also the first
		// thing that reads as broken if it is not said out loud: it is a control
		// surface and it needs flow over it. Hold throttle to turn. The sign
		// follows the direction of travel, so backing up steers in reverse the way
		// a boat actually does.
		const FVector V = PhysicsBody->GetPhysicsLinearVelocity();
		const double Along = FVector::DotProduct(V, Fwd);
		const double Authority =
			FMath::Min(FMath::Abs(Along), RudderSaturationSpeedUU) * FMath::Sign(Along);
		PhysicsBody->AddTorqueInRadians(
			FVector(0.0, 0.0, double(SteerInput) * RudderYawAccelPerSpeed * Authority), NAME_None,
			true);
	}
}

void AVoxelBoat::TickGround(float DeltaSeconds)
{
	using namespace VoxelBoatTuning;

	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* Voxels = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Voxels)
	{
		return;
	}

	const FVector Loc = GetActorLocation();
	const double KeelZ = Loc.Z + KeelOffsetUU;

	// --- beaching --------------------------------------------------------------
	//
	// A HIT IS REQUIRED. A raycast that finds nothing means the ground under this
	// column has not streamed, NOT that there is clear water below -- treating a
	// miss as clear is how "absence reads as air" deletes a hull through the
	// terrain. So a miss leaves the boat exactly as buoyancy left it.
	FVector HitCentre, PrevCentre;
	const FVector Start = Loc + FVector(0.0, 0.0, VoxelCoords::VoxelSizeUU);
	const double MaxDist = FMath::Abs(KeelOffsetUU) + 100000.0;
	bGrounded = false;
	if (Voxels->RaycastVoxelWorld(Start, FVector(0.0, 0.0, -1.0), MaxDist, HitCentre, PrevCentre))
	{
		const double SurfaceTopZ = HitCentre.Z + VoxelCoords::VoxelSizeUU * 0.5;
		if (KeelZ <= SurfaceTopZ + GroundClearanceUU)
		{
			bGrounded = true;
			++GroundedTicks;

			// Rest the keel on the surface and stop the descent. Not a physical
			// contact -- there is nothing here for Chaos to contact with -- so the
			// clamp is done by hand exactly as AVoxelDebris::SettleOnSurface does
			// it, and for the same reason.
			FVector V = PhysicsBody->GetPhysicsLinearVelocity();
			if (V.Z < 0.0)
			{
				V.Z = 0.0;
			}
			// Ground friction: a hull on gravel stops. Applied to the horizontal
			// velocity only, so a boat still refloats when the tide comes back over
			// it rather than being pinned by its own friction.
			const double Keep = FMath::Exp(-GroundFrictionPerSec * double(DeltaSeconds));
			V.X *= Keep;
			V.Y *= Keep;
			PhysicsBody->SetPhysicsLinearVelocity(V);

			FVector Fixed = Loc;
			Fixed.Z = FMath::Max(Loc.Z, SurfaceTopZ + GroundClearanceUU - KeelOffsetUU);
			if (!FMath::IsNearlyEqual(Fixed.Z, Loc.Z))
			{
				SetActorLocation(Fixed, false, nullptr, ETeleportType::TeleportPhysics);
			}
		}
	}

	// --- the bow probe: the entire substitute for collision --------------------
	//
	// There is no Chaos terrain, so nothing stops a hull entering a cliff. This
	// kills the velocity component heading INTO whatever is directly ahead. It
	// does not push back and it does not slide -- the boat simply stops going
	// that way, which is enough to keep it in the water and is honest about being
	// a stand-in.
	FVector V = PhysicsBody->GetPhysicsLinearVelocity();
	FVector Dir(V.X, V.Y, 0.0);
	const double SpeedXY = Dir.Size();
	if (SpeedXY > BowProbeMinSpeedUU)
	{
		Dir /= SpeedXY;
		const FVector Bow = Loc + Dir * HalfLengthUU + FVector(0.0, 0.0, VoxelCoords::VoxelSizeUU);
		if (Voxels->RaycastVoxelWorld(Bow, Dir, BowProbeUU, HitCentre, PrevCentre))
		{
			++BowBlockTicks;
			const double Into = FVector::DotProduct(V, Dir);
			if (Into > 0.0)
			{
				V -= Dir * Into;
				PhysicsBody->SetPhysicsLinearVelocity(V);
			}
			if (CVarVoxelBoatDebugDraw.GetValueOnGameThread())
			{
				DrawDebugLine(World, Bow, HitCentre, FColor::Red, false, 0.f, 0, 2.f);
			}
		}
	}
}

void AVoxelBoat::TickWake(float DeltaSeconds)
{
	using namespace VoxelBoatTuning;

	UWorld* World = GetWorld();
	if (!World || !CVarVoxelBoatWake.GetValueOnGameThread())
	{
		bHaveLastWake = false; // a re-enable must not sweep from a stale position
		return;
	}

	const FTransform Xf = GetActorTransform();
	// Bow shoulders at +-half beam and the transom on the centreline. Z is
	// carried but not used: AddDisturbance discards it at the door, because the
	// field is a 2D sheet and the water knows its own height better than this
	// actor does.
	const FVector BowPort = Xf.TransformPosition(FVector(HalfLengthUU * 0.9, -HalfBeamUU, 0.0));
	const FVector BowStarboard = Xf.TransformPosition(FVector(HalfLengthUU * 0.9, HalfBeamUU, 0.0));
	const FVector Transom = Xf.TransformPosition(FVector(-HalfLengthUU * 0.95, 0.0, 0.0));

	bool bAnyWet = false;
	for (int32 I = 0; I < NumProbes; ++I)
	{
		bAnyWet |= bProbeWasSubmerged[I];
	}

	const FVector V = PhysicsBody->GetPhysicsLinearVelocity();
	const double SpeedXY = FVector(V.X, V.Y, 0.0).Size();

	// WAKE ENGAGEMENT WITNESS (2026-09-06). The owner drove this boat for
	// minutes and the ripple field recorded ONE injection in 14,162 steps --
	// so the wake was never reaching the field, and every material-side theory
	// about why the wake is invisible was chasing the wrong half. Three gates
	// can swallow it silently and they are indistinguishable from outside, so
	// this says which, once a second, only while the player is aboard a boat
	// that is declining. Silent when the wake is actually injecting.
	{
		static double LastWakeReportSeconds = 0.0;
		const double NowSeconds = FPlatformTime::Seconds();
		const bool bWouldInject = bHaveLastWake && bAnyWet && SpeedXY > WakeMinSpeedUU;
		if (!bWouldInject && NowSeconds - LastWakeReportSeconds >= 1.0)
		{
			LastWakeReportSeconds = NowSeconds;
			UE_LOG(LogVoxelEarth, Display,
			       TEXT("[boat-wake] DECLINED: haveLastSweep=%d anyProbeWet=%d speed=%.1f UU/s "
			            "(needs > %.1f). Lifetime probe-wet ticks=%llu, wake splats=%llu."),
			       bHaveLastWake ? 1 : 0, bAnyWet ? 1 : 0, SpeedXY, WakeMinSpeedUU,
			       ProbesWet, WakeSplats);
		}
	}

	if (bHaveLastWake && bAnyWet && SpeedXY > WakeMinSpeedUU)
	{
		// Strength rides the SAME saturating ramp the ripple field's own
		// auto-watcher uses (VoxelRippleField.h's VoxelRipple namespace), so a
		// wake and a splash cannot end up as two tunings of one effect. Three
		// calls per tick, each of which subdivides into at most a handful of
		// sub-splats at any plausible speed -- comfortably inside kSplatSlots.
		const float Frac =
			float(VoxelBoatLocal::ImpactFraction(SpeedXY))
			* FMath::Max(0.f, CVarVoxelBoatWakeGain.GetValueOnGameThread());
		UVoxelRippleFieldSubsystem::AddSweptDisturbanceAt(World, LastBowPort, BowPort,
		                                                  float(BowWakeWidthM),
		                                                  float(BowWakeStrengthM) * Frac);
		UVoxelRippleFieldSubsystem::AddSweptDisturbanceAt(World, LastBowStarboard, BowStarboard,
		                                                  float(BowWakeWidthM),
		                                                  float(BowWakeStrengthM) * Frac);
		// The transom trail is wider and weaker: it is the hollow a hull leaves
		// behind it, not the crest it pushes ahead.
		UVoxelRippleFieldSubsystem::AddSweptDisturbanceAt(World, LastTransom, Transom,
		                                                  float(TransomWakeWidthM),
		                                                  float(TransomWakeStrengthM) * Frac);
		WakeSplats += 3;
	}

	LastBowPort = BowPort;
	LastBowStarboard = BowStarboard;
	LastTransom = Transom;
	bHaveLastWake = true;
}

void AVoxelBoat::TickCamera(float DeltaSeconds)
{
	using namespace VoxelBoatTuning;
	if (!CameraArm || !ChaseCamera)
	{
		return;
	}
	CameraArm->SetRelativeRotation(FRotator(float(CameraPitchDeg), float(CameraYawDeg), 0.f));

	// Collision-aware pull-in through the voxel DDA, exactly as
	// AVoxelEarthFlyPawn::UpdateThirdPersonCamera does it -- a USpringArmComponent
	// cannot help here because its sphere sweep has no Chaos terrain to sweep
	// against.
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* Voxels = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	const FVector Anchor = CameraArm->GetComponentLocation();
	const FVector Desired = CameraArm->GetComponentTransform().TransformPosition(
		FVector(-CameraBackUU, 0.0, CameraUpUU));
	FVector Dir = Desired - Anchor;
	const double Want = Dir.Size();
	if (Voxels && Want > KINDA_SMALL_NUMBER)
	{
		Dir /= Want;
		FVector HitCentre, PrevCentre;
		if (Voxels->RaycastVoxelWorld(Anchor, Dir, Want, HitCentre, PrevCentre))
		{
			const double Allowed =
				FMath::Max(0.0, FVector::Dist(Anchor, PrevCentre)
				                    - VoxelMovementTuning::ThirdPersonPullInEpsilonUU);
			ChaseCamera->SetWorldLocation(Anchor + Dir * Allowed);
			return;
		}
	}
	ChaseCamera->SetWorldLocation(Desired);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void AVoxelBoat::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	if (!PlayerInputComponent)
	{
		return;
	}

	// Legacy input, the fly pawn's idiom verbatim (docs/m1-plan.md Stage 2:
	// "legacy input bindings, Enhanced Input assets deferred"). Digital keys must
	// go through named engine-defined axis mappings or BindAxisKey trips the
	// IsAxis1D ensure; registration is process-global and duplicate identical
	// registrations are no-ops. OWN NAMES rather than reusing VoxelFly_*, because
	// those are only registered when a fly pawn has had its input set up, and a
	// leg that spawns straight into a boat may never have had one.
	UPlayerInput::AddEngineDefinedAxisMapping(
		FInputAxisKeyMapping(TEXT("VoxelBoat_Throttle"), EKeys::W, 1.f));
	UPlayerInput::AddEngineDefinedAxisMapping(
		FInputAxisKeyMapping(TEXT("VoxelBoat_Throttle"), EKeys::S, -1.f));
	UPlayerInput::AddEngineDefinedAxisMapping(
		FInputAxisKeyMapping(TEXT("VoxelBoat_Steer"), EKeys::D, 1.f));
	UPlayerInput::AddEngineDefinedAxisMapping(
		FInputAxisKeyMapping(TEXT("VoxelBoat_Steer"), EKeys::A, -1.f));

	PlayerInputComponent->BindAxis(TEXT("VoxelBoat_Throttle"), this, &AVoxelBoat::InputThrottle);
	PlayerInputComponent->BindAxis(TEXT("VoxelBoat_Steer"), this, &AVoxelBoat::InputSteer);
	PlayerInputComponent->BindAxisKey(EKeys::MouseX, this, &AVoxelBoat::InputLookYaw);
	PlayerInputComponent->BindAxisKey(EKeys::MouseY, this, &AVoxelBoat::InputLookPitch);
}

void AVoxelBoat::InputThrottle(float Value)
{
	ThrottleInput = FMath::Clamp(Value, -1.f, 1.f);
}

void AVoxelBoat::InputSteer(float Value)
{
	SteerInput = FMath::Clamp(Value, -1.f, 1.f);
}

void AVoxelBoat::InputLookYaw(float Value)
{
	// LOOKING AROUND IS NOT STEERING. On a real boat they are the same stick and
	// separating them here is the difference between seeing where you are going
	// and motion sickness: the hull's heading is a physics result and the camera
	// is a free look on top of it.
	CameraYawDeg = FMath::UnwindDegrees(CameraYawDeg + double(Value) * 2.5);
}

void AVoxelBoat::InputLookPitch(float Value)
{
	CameraPitchDeg = FMath::Clamp(CameraPitchDeg - double(Value) * 2.5,
	                              VoxelBoatTuning::CameraPitchMinDeg,
	                              VoxelBoatTuning::CameraPitchMaxDeg);
}

// ---------------------------------------------------------------------------
// Possession (plan D3)
// ---------------------------------------------------------------------------

AVoxelBoat* AVoxelBoat::FindNearest(const UWorld* World, const FVector& From, double MaxRangeUU)
{
	if (!World)
	{
		return nullptr;
	}
	AVoxelBoat* Best = nullptr;
	double BestSq = MaxRangeUU * MaxRangeUU;
	for (TActorIterator<AVoxelBoat> It(const_cast<UWorld*>(World)); It; ++It)
	{
		AVoxelBoat* B = *It;
		if (!B || B->IsCrewed())
		{
			continue;
		}
		const double DSq = FVector::DistSquared(From, B->GetActorLocation());
		if (DSq <= BestSq)
		{
			BestSq = DSq;
			Best = B;
		}
	}
	return Best;
}

bool AVoxelBoat::TryEnterNearest(APlayerController* PC)
{
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return false;
	}
	AVoxelBoat* Boat =
		FindNearest(PC->GetWorld(), Pawn->GetActorLocation(), VoxelBoatTuning::InteractRangeUU);
	if (!Boat)
	{
		// SAY WHY. "I pressed the key and nothing happened" is the one report
		// that costs an evening, and the two causes -- no boat in the world at
		// all, and a boat that is simply too far -- want different answers.
		int32 Count = 0;
		for (TActorIterator<AVoxelBoat> It(PC->GetWorld()); It; ++It)
		{
			++Count;
		}
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelBoat: nothing to board within %.1f m. %d boat(s) exist in this world. ")
		       TEXT("voxel.Boat.Spawn puts one in front of you."),
		       VoxelBoatTuning::InteractRangeUU / 100.0, Count);
		return false;
	}
	return Boat->Enter(PC);
}

bool AVoxelBoat::Enter(APlayerController* PC)
{
	APawn* Previous = PC ? PC->GetPawn() : nullptr;
	if (!PC || !Previous || Previous == this || StoredPawn.IsValid())
	{
		return false;
	}

	StoredPawn = Previous;
	Driver = PC;

	// The outgoing pawn is PARKED, not destroyed: it carries the player's camera
	// mode, speed dial and crouch state, and rebuilding one on exit would quietly
	// reset all of that. Ticking off matters most -- a walk-mode pawn left
	// ticking under the lake would keep swimming while nobody is in it.
	Previous->SetActorHiddenInGame(true);
	Previous->SetActorEnableCollision(false);
	Previous->SetActorTickEnabled(false);

	PC->Possess(this);

	// AutoWatch AND THE RIPPLE WINDOW FOLLOW THIS FOR FREE, and that was checked
	// rather than assumed: both UVoxelRippleFieldSubsystem::GetCameraXY and its
	// AutoWatch read World->GetFirstPlayerController()->GetPawn(), not a fly-pawn
	// type. So possessing the boat re-centres the 51.2 m ripple window on the
	// boat and starts watching the boat for water entry, with no edit to that
	// file. (The one consequence worth knowing: the watcher will splash once when
	// the boat's ORIGIN first crosses the surface, on top of this actor's own
	// hull-slam splash. One extra ring on launch, which is not wrong.)
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelBoat: BOARDED at (%.0f,%.0f,%.0f). Stored pawn '%s' parked. The ripple window ")
	       TEXT("now follows the boat."),
	       GetActorLocation().X, GetActorLocation().Y, GetActorLocation().Z, *Previous->GetName());
	return true;
}

void AVoxelBoat::ExitToStoredPawn()
{
	APawn* Pawn = StoredPawn.Get();
	APlayerController* PC = Driver.Get();
	StoredPawn = nullptr;
	Driver = nullptr;
	if (!Pawn || !PC)
	{
		return;
	}

	using namespace VoxelBoatTuning;

	// BESIDE the hull, not astern: stepping out of a beached boat should not put
	// you inside the rock it is resting against. Starboard side, clear of the
	// beam, lifted to the waterline if there is one and to the gunwale if there
	// is not.
	const FTransform Xf = GetActorTransform();
	FVector Out = Xf.TransformPosition(FVector(0.0, HalfBeamUU + ExitSideClearanceUU, 0.0));

	UWorld* World = GetWorld();
	if (UVoxelWaterSubsystem* Water = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr)
	{
		FWaterSurfaceSample S;
		if (Water->WaterSurfaceZAtWorld(Out.X, Out.Y, S) && S.Kind != EWaterSurfaceKind::CAOnly)
		{
			// The datum alone is NOT enough: a beached hull sits metres above
			// the waterline, so "waterline + lift" can be inside the bank and
			// under the boat (review finding #3). The pawn steps out at
			// whichever is higher -- the water surface or the boat's own
			// resting height, which TickGround has already reconciled with the
			// real bed by raycast.
			Out.Z = FMath::Max(S.SurfaceZUU, GetActorLocation().Z) + ExitLiftUU;
		}
		else
		{
			Out.Z = GetActorLocation().Z + ExitLiftUU;
		}
	}

	Pawn->SetActorHiddenInGame(false);
	Pawn->SetActorEnableCollision(true);
	Pawn->SetActorTickEnabled(true);
	Pawn->SetActorLocation(Out, false, nullptr, ETeleportType::TeleportPhysics);
	PC->Possess(Pawn);

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelBoat: DISEMBARKED to (%.0f,%.0f,%.0f). Pawn '%s' repossessed."), Out.X, Out.Y,
	       Out.Z, *Pawn->GetName());
}

AVoxelBoat* AVoxelBoat::SpawnAhead(UWorld* World, double AheadUU)
{
	if (!World)
	{
		return nullptr;
	}
	FVector Anchor;
	if (!VoxelBoatLocal::CameraLocation(World, Anchor))
	{
		UE_LOG(LogVoxelEarth, Warning, TEXT("voxel.Boat.Spawn: no player pawn to spawn in front of."));
		return nullptr;
	}
	const APlayerController* PC = World->GetFirstPlayerController();
	FRotator ViewRot = PC ? PC->GetControlRotation() : FRotator::ZeroRotator;
	ViewRot.Pitch = 0.f;
	ViewRot.Roll = 0.f;

	FVector Spawn = Anchor + ViewRot.Vector() * AheadUU;

	// Put it ON the water if there is any, and above the ground otherwise, so the
	// command lands a usable boat rather than one falling from the camera's
	// altitude. Datum query only -- correct over unstreamed ground.
	bool bOnWater = false;
	double SurfaceZUU = 0.0;
	if (UVoxelWaterSubsystem* Water = World->GetSubsystem<UVoxelWaterSubsystem>())
	{
		FWaterSurfaceSample S;
		if (Water->WaterSurfaceZAtWorld(Spawn.X, Spawn.Y, S) && S.Kind != EWaterSurfaceKind::CAOnly)
		{
			// Provisionally at the surface; the keel-depth correction below
			// needs the SPAWNED boat's own KeelOffsetUU (measured from the
			// asset body's bounds in BeginPlay), which no static context has.
			bOnWater = true;
			SurfaceZUU = S.SurfaceZUU;
			Spawn.Z = S.SurfaceZUU;
		}
		else
		{
			Spawn.Z = S.GroundZUU + 200.0;
		}
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AVoxelBoat* Boat = World->SpawnActor<AVoxelBoat>(AVoxelBoat::StaticClass(), Spawn, ViewRot, Params);
	if (Boat != nullptr && bOnWater)
	{
		// Put the KEEL at its rest depth: origin = surface - draft - keel
		// offset (KeelOffsetUU is NEGATIVE, keel below origin). The old
		// `surface + RestDraftUU` settled anyway but read as if it meant
		// the keel (review finding #8). Applied post-spawn because the
		// instance's KeelOffsetUU is only measured in BeginPlay, which
		// SpawnActor has run by the time it returns.
		FVector Loc = Boat->GetActorLocation();
		Loc.Z = SurfaceZUU - VoxelBoatTuning::RestDraftUU - Boat->KeelOffsetUU;
		Boat->SetActorLocation(Loc);
	}
	UE_LOG(LogVoxelEarth, Log, TEXT("voxel.Boat.Spawn: %s at (%.0f,%.0f,%.0f) facing %.0f deg."),
	       Boat ? TEXT("spawned") : TEXT("FAILED to spawn"), Spawn.X, Spawn.Y, Spawn.Z, ViewRot.Yaw);
	return Boat;
}

// ---------------------------------------------------------------------------
// Commands. Headless and interactive testing needs no UI work at all -- the
// plan's D-phase gates are owner screenshots at pinned poses, and a screenshot
// that requires someone to build a menu first does not get taken.
// ---------------------------------------------------------------------------

FAutoConsoleCommandWithWorldAndArgs GVoxelBoatSpawnCmd(
	TEXT("voxel.Boat.Spawn"),
	TEXT("voxel.Boat.Spawn [AheadM=8] -- put a boat in front of the camera, resting on the water ")
	TEXT("surface if the column holds any and above the ground if not. Then press E to board it. ")
	TEXT("The spawn line prints the position and whether the visual grid loaded or fell back to a ")
	TEXT("placeholder."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			const double AheadM = (Args.Num() > 0) ? FCString::Atod(*Args[0]) : 8.0;
			AVoxelBoat::SpawnAhead(World, AheadM * 100.0);
		}));

FAutoConsoleCommandWithWorld GVoxelBoatEnterCmd(
	TEXT("voxel.Boat.Enter"),
	TEXT("Board the nearest boat, exactly as the interact key does. Exists so an unattended leg can ")
	TEXT("drive a boat with no keyboard."),
	FConsoleCommandWithWorldDelegate::CreateStatic(
		[](UWorld* World)
		{
			AVoxelBoat::TryEnterNearest(World ? World->GetFirstPlayerController() : nullptr);
		}));

FAutoConsoleCommandWithWorld GVoxelBoatExitCmd(
	TEXT("voxel.Boat.Exit"), TEXT("Step out of the boat currently possessed, if any."),
	FConsoleCommandWithWorldDelegate::CreateStatic(
		[](UWorld* World)
		{
			APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
			if (AVoxelBoat* B = PC ? Cast<AVoxelBoat>(PC->GetPawn()) : nullptr)
			{
				B->ExitToStoredPawn();
			}
		}));

// The missing half of voxel.Boat.Enter's promise: Enter boards with no
// keyboard, this drives with no keyboard. Spawn -> Enter -> Throttle is the
// whole unattended underway-wake leg (D-gate: wedge + transom trail).
FAutoConsoleCommandWithWorldAndArgs GVoxelBoatThrottleCmd(
	TEXT("voxel.Boat.Throttle"),
	TEXT("voxel.Boat.Throttle [value=1] [seconds=10] -- scripted throttle on the possessed boat, ")
	TEXT("for unattended legs (the drive half of voxel.Boat.Enter). Clamped to [-1,1]."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
			AVoxelBoat* B = PC ? Cast<AVoxelBoat>(PC->GetPawn()) : nullptr;
			if (!B)
			{
				UE_LOG(LogVoxelEarth, Warning,
				       TEXT("voxel.Boat.Throttle: no boat possessed. voxel.Boat.Enter boards the ")
				       TEXT("nearest one first."));
				return;
			}
			const float Value =
				(Args.Num() > 0) ? FMath::Clamp(FCString::Atof(*Args[0]), -1.f, 1.f) : 1.f;
			const double Seconds = (Args.Num() > 1) ? FCString::Atod(*Args[1]) : 10.0;
			B->SetScriptThrottle(Value, Seconds);
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("voxel.Boat.Throttle: %.2f for %.1fs (scripted override; player input ")
			       TEXT("resumes when it expires)."),
			       Value, Seconds);
		}));

FAutoConsoleCommandWithWorld GVoxelBoatStatCmd(
	TEXT("voxel.Boat.Stat"),
	TEXT("One line per boat: probe queries, wet probes, wake splats, slams, grounded ticks, bow ")
	TEXT("blocks and sleep ticks. EVERY ONE OF THESE CAN BE ZERO FOR A DIFFERENT REASON, which is ")
	TEXT("why they are separate: probes=0 is a Tick that is not running; probes>0 with wet=0 is a ")
	TEXT("boat in the air (or over CA-only water, which has no datum to float on); wake=0 with ")
	TEXT("wet>0 is a boat that never got above the wake speed."),
	FConsoleCommandWithWorldDelegate::CreateStatic(
		[](UWorld* World)
		{
			if (!World)
			{
				return;
			}
			int32 N = 0;
			for (TActorIterator<AVoxelBoat> It(World); It; ++It)
			{
				const AVoxelBoat* B = *It;
				++N;
				const FVector L = B->GetActorLocation();
				UE_LOG(LogVoxelWater, Log,
				       TEXT("Boat[%d] at (%.0f,%.0f,%.0f) crewed=%d visual=%s(%d inst) probes=%llu ")
				       TEXT("wet=%llu wake=%llu slams=%llu grounded=%llu bowblock=%llu asleep=%llu"),
				       N - 1, L.X, L.Y, L.Z, B->IsCrewed() ? 1 : 0,
				       (B->GetBody() && !B->GetBody()->IsPlaceholder()) ? TEXT("asset")
				                                                        : TEXT("PLACEHOLDER"),
				       B->GetBody() ? B->GetBody()->GetInstanceCount() : 0,
				       (unsigned long long)B->GetProbeQueries(), (unsigned long long)B->GetProbesWet(),
				       (unsigned long long)B->GetWakeSplats(),
				       (unsigned long long)B->GetSlamSplashes(),
				       (unsigned long long)B->GetGroundedTicks(),
				       (unsigned long long)B->GetBowBlockTicks(),
				       (unsigned long long)B->GetSleepTicks());
			}
			if (N == 0)
			{
				UE_LOG(LogVoxelWater, Log,
				       TEXT("voxel.Boat.Stat: no boats in this world. voxel.Boat.Spawn makes one."));
			}
		}));
