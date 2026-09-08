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
#include "Kismet/KismetMaterialLibrary.h"          // the hull ripple mask's MPC push
#include "Materials/MaterialInterface.h" // the WaveMirror fingerprint check
#include "Materials/MaterialParameterCollection.h" // ...and its one-time name check
#include "PhysicsEngine/BodyInstance.h"
#include "UObject/ConstructorHelpers.h"

#include "VoxelAssetBody.h"
#include "VoxelCoords.h"
#include "VoxelDebug.h" // LogVoxelWater -- the wake half of this actor
#include "VoxelEarth.h" // LogVoxelEarth -- the chassis half
#include "VoxelRippleField.h"
#include "VoxelSkySubsystem.h" // VoxelSky::kSkyCollectionPath
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
	TEXT("voxel.Boat.WakeGain"), 1.5f,
	TEXT("Multiplier on the bow/transom/slam splat strengths. DEFAULT 1.5 since 2026-09-08 (owner, ")
	TEXT("live: wake lines 'much too large'; halved from 3.0 together with WakeWidthScale 0.3). ")
	TEXT("3.0 came from the 2026-09-06 owner directive: the ")
	TEXT("verified-LIVE wake measured 1.5 cm of state height -- honest but invisible). Dials the ")
	TEXT("wake's visual weight without touching the ripple sim's physics constants."),
	ECVF_Default);
TAutoConsoleVariable<float> CVarVoxelBoatWakeWidthScale(
	TEXT("voxel.Boat.WakeWidthScale"), 0.3f,
	TEXT("Multiplier on the bow/transom splat WIDTHS. DEFAULT 0.3 since 2026-09-08 (owner, live, ")
	TEXT("second pass: still 'much too large, spread too far'; the footprint sets the ring ")
	TEXT("wavelength, so 0.3 is what makes them 'smaller, finer, closer to the canoe'). ")
	TEXT("First pass, same day: the wake waves ")
	TEXT("'should be much smaller and finer. fine ripples in high quantity'). 1.0 = the authored ")
	TEXT("BowWakeWidthM/TransomWakeWidthM; 0.5 halves the ring radius so the field carries ")
	TEXT("shorter, more numerous ripples. Dial with voxel.Boat.WakeGain (amplitude) and ")
	TEXT("voxel.Water.Ripple.HalfLifeSec (how long the field remembers)."),
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

TAutoConsoleVariable<bool> CVarVoxelBoatHullMask(
	TEXT("voxel.Boat.HullMask"), true,
	TEXT("The hull water-exclusion mask (AVoxelBoat::UpdateWaterExclusion). 0 hides every station, ")
	TEXT("which is the contract's own off arm (Tools/water_hull_mask_graph.py): with no stencil ")
	TEXT("writer in the world the water term multiplies by 1 and the frame is pixel-identical to a ")
	TEXT("world with no boat in it. That is the A arm for 'is the cockpit dry because of the mask' ")
	TEXT("and the only way to photograph the water this mask is removing."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarVoxelBoatHullMaskLidUU(
	TEXT("voxel.Boat.HullMaskLidUU"), 6.0f,
	TEXT("How far the exclusion mask stands above the DRAWN water surface, in UU. This is the ")
	TEXT("artefact knob and it trades in both directions: the band of water the mask kills OUTSIDE ")
	TEXT("the hull is lid/tan(view depression) wide, so a taller lid carves a wider ring of open ")
	TEXT("water beside the planking, while a shorter one lets ripples and the mirror-vs-pixel ")
	TEXT("mismatch (a few cm, VoxelWaveMirror.generated.h's stated band) wash back into the ")
	TEXT("cockpit. At the pond pose 2026-09-07 BOTH 10 UU (VoxelVerify00854) and 6 UU ")
	TEXT("(VoxelVerify00860) left the cockpit dry end to end and showed no ring at that camera ")
	TEXT("depression; 6 is the default because the ring is the failure that reaches the owner's ")
	TEXT("grazing shots and 6 UU still clears the few-cm mismatch and the 4 UU wake."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelBoatHullRippleMask(
	TEXT("voxel.Boat.HullRippleMask"), true,
	TEXT("The SECOND half of 'the cockpit is dry' (owner, live 2026-09-08: 'there is water and ")
	TEXT("wake, surface effects inside the canoe'). The lid mask above stands 6 UU over the ")
	TEXT("AMBIENT surface, and the wake's ripple height is ADDED to that surface as WPO, so a ")
	TEXT("crest inside the hull lifts the sheet over the lid and out of the stencil cull. This ")
	TEXT("pushes the hull's plan ellipse through MPC_VoxelSky (HullEllipseA/B) and both water ")
	TEXT("materials zero the ripple WPO and the disturbance foam inside it ")
	TEXT("(Tools/water_hull_mask_graph.py). 0 pushes the off encoding: pixel-identical to the ")
	TEXT("mask never existing, and the A arm for 'is the cockpit dry because of this'."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarVoxelBoatHullRippleMaskEdgeUU(
	TEXT("voxel.Boat.HullRippleMaskEdgeUU"), 10.0f,
	TEXT("Width of the soft edge on the hull ripple mask's ellipse, UU, measured along the ")
	TEXT("beam. A hard edge (1) shows as a seam in the wake where it meets the planking; too ")
	TEXT("wide and the cockpit's last few centimetres pick the wake back up."),
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

// --- the water-exclusion mask's geometry constants -------------------------
//
// Numbers, not tuning: every one of them is an argument in
// AVoxelBoat::UpdateWaterExclusion, which is where they are justified.
constexpr int32 NumExclusionStations = 7;
constexpr double kExclusionPlanFraction = 0.96;    // of the hull half-length, per end
constexpr double kExclusionInboardFraction = 0.85; // of the local elliptic half-beam
constexpr double kExclusionMinHalfBeamUU = 4.0;
constexpr double kExclusionSkirtUU = 60.0;    // how far it reaches below it
constexpr double kExclusionKeelSlackUU = 2.0; // "the water has reached the planking here"

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

	// The water-exclusion stations (contract: Tools/water_hull_mask_graph.py;
	// mechanism in UpdateWaterExclusion). CUSTOM DEPTH ONLY: main pass off AND
	// scene depth prepass off -- a volume that wrote scene depth would occlude
	// the very water pixels the mask is supposed to test, not just the ones
	// inside the hull. The engine cube is closed and outward-facing, which is
	// all the contract's near-shell test needs.
	//
	// ABSOLUTE LOCATION AND ROTATION, which is the one surprising flag here: a
	// station's lid has to stay HORIZONTAL and sit on the drawn water surface,
	// and a component that inherited the hull's pitch, roll and heave cannot do
	// either. The per-tick solve writes the whole world transform, and every
	// path that does not solve a station HIDES it (SetRenderCustomDepth(false))
	// -- an absolute-placed mask left behind by a moving boat would carve a
	// hole in open water.
	ExclusionStations.Reserve(VoxelBoatLocal::NumExclusionStations);
	for (int32 StationIdx = 0; StationIdx < VoxelBoatLocal::NumExclusionStations; ++StationIdx)
	{
		UStaticMeshComponent* Station = CreateDefaultSubobject<UStaticMeshComponent>(
			FName(*FString::Printf(TEXT("WaterExclusionStation%d"), StationIdx)));
		Station->SetupAttachment(PhysicsBody);
		if (CubeFinder.Succeeded())
		{
			Station->SetStaticMesh(CubeFinder.Object);
		}
		Station->SetRenderInMainPass(false);
		Station->SetRenderInDepthPass(false);
		// Bit 0 = water exclusion. The registry of stencil bits lives in
		// Tools/water_hull_mask_graph.py's docstring; claim new bits there.
		Station->SetCustomDepthStencilValue(1);
		Station->SetRenderCustomDepth(false); // armed only by a solved tick
		Station->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Station->SetCastShadow(false);
		Station->SetMobility(EComponentMobility::Movable);
		Station->SetUsingAbsoluteLocation(true);
		Station->SetUsingAbsoluteRotation(true);
		ExclusionStations.Add(Station);
	}

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

	// --- the hull ripple mask's MPC binding, checked ONCE (PushHullRippleMask)
	//
	// UKismetMaterialLibrary's setters log a warning and do nothing for a
	// parameter that is not on the collection; pushed twice a tick that would
	// bury every other diagnostic in the run. The honest failure is one line
	// naming the regen that has not been applied, and a boat that then behaves
	// exactly as it did before this mask existed.
	HullRippleMaskCollection =
		LoadObject<UMaterialParameterCollection>(nullptr, VoxelSky::kSkyCollectionPath);
	bHullRippleMaskMpcOk = false;
	if (const UMaterialParameterCollection* Sky = HullRippleMaskCollection)
	{
		bool bHasA = false, bHasB = false;
		for (const FCollectionVectorParameter& P : Sky->VectorParameters)
		{
			bHasA |= (P.ParameterName == FName(TEXT("HullEllipseA")));
			bHasB |= (P.ParameterName == FName(TEXT("HullEllipseB")));
		}
		bHullRippleMaskMpcOk = bHasA && bHasB;
	}
	if (!bHullRippleMaskMpcOk)
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("VoxelBoat: MPC_VoxelSky has no HullEllipseA/HullEllipseB, so the hull ripple mask ")
		       TEXT("cannot engage and wake ripples will still draw inside the cockpit. Regenerate the ")
		       TEXT("sky chain (create_sky_material.py, then M_WaterVoxel and M_Ocean)."));
	}

	// --- the water-exclusion mask's plan footprint, from the adopted hull -----
	//
	// PLAN ONLY: where each station sits along the keel line and how wide it is
	// allowed to be. The heights are re-solved every tick against the DRAWN water
	// surface, and UpdateWaterExclusion carries that argument.
	//
	// The hull is cut into NumExclusionStations equal slices out to
	// kExclusionPlanFraction of the half-length, and each slice takes the
	// half-beam an ELLIPTIC canoe plan carries at the slice's OUTER edge, times
	// kExclusionInboardFraction. Reading the beam at the outer edge and not at
	// the slice centre is what keeps planking between every station and the water
	// everywhere the station reaches: a slice can only be as wide as its narrow
	// end. That is the whole of the "hull-shaped mask", as a staircase.
	//
	// It replaces the 2026-09-06 pair of boxes (a 0.62L x 0.55B midship box and a
	// 0.92L x 0.26B ends box). Two rectangles could fit the midsection or reach
	// the ends, not both -- the ends box's 0.26 beam was the price of reaching
	// 0.92L with a rectangle, and the corners past it kept a sliver anyway. The
	// mask term itself is inert (the safe direction) until r.CustomDepth=3 is set
	// in config and the regenerated materials carry it.
	ExclusionPlan.Reset();
	{
		const double PlanHalfL = HalfLengthUU * VoxelBoatLocal::kExclusionPlanFraction;
		const double SliceLenUU = 2.0 * PlanHalfL / double(VoxelBoatLocal::NumExclusionStations);
		for (int32 I = 0; I < VoxelBoatLocal::NumExclusionStations; ++I)
		{
			FVoxelBoatExclusionStation Plan;
			Plan.LocalX = -PlanHalfL + (double(I) + 0.5) * SliceLenUU;
			Plan.HalfLenUU = 0.5 * SliceLenUU;
			const double EdgeT =
				FMath::Min(1.0, (FMath::Abs(Plan.LocalX) + Plan.HalfLenUU) / FMath::Max(1.0, HalfLengthUU));
			Plan.HalfBeamUU =
				FMath::Max(VoxelBoatLocal::kExclusionMinHalfBeamUU,
				           HalfBeamUU * FMath::Sqrt(FMath::Max(0.0, 1.0 - EdgeT * EdgeT))
				               * VoxelBoatLocal::kExclusionInboardFraction);
			ExclusionPlan.Add(Plan);
		}
	}
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("Boat: water-exclusion mask = %d stations over %.2f m of hull, half-beam %.0f UU ")
	       TEXT("amidships to %.0f UU at the ends, lid %.0f UU above the DRAWN surface (custom ")
	       TEXT("stencil bit 0)."),
	       ExclusionPlan.Num(), 2.0 * HalfLengthUU * VoxelBoatLocal::kExclusionPlanFraction / 100.0,
	       ExclusionPlan.Num() ? ExclusionPlan[ExclusionPlan.Num() / 2].HalfBeamUU : 0.0,
	       ExclusionPlan.Num() ? ExclusionPlan[0].HalfBeamUU : 0.0,
	       double(CVarVoxelBoatHullMaskLidUU.GetValueOnGameThread()));

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
	// A destroyed boat must not leave a dry ellipse of lake behind it. Only on
	// Destroyed, for the same teardown reason as above: during level transition
	// the collection instance is going with the world.
	if (Reason == EEndPlayReason::Destroyed)
	{
		PushHullRippleMask(false);
	}
	Super::EndPlay(Reason);
}

void AVoxelBoat::PushHullRippleMask(bool bEnabled)
{
	// THE HULL'S PLAN ELLIPSE, in world XY, for the water materials.
	//
	// Encoding (Tools/water_hull_mask_graph.py is the registry):
	//   HullEllipseA = (centreX, centreY, cos yaw, sin yaw)   world UU
	//   HullEllipseB = (half-length, half-beam, enabled, edge) UU, UU, 0/1, UU
	//
	// THE FOOTPRINT IS THE COCKPIT, NOT THE WAKE STATIONS. Half-length is
	// kExclusionPlanFraction of the hull's (the same reach the stencil lid
	// covers); half-beam is the hull's, since the ellipse is the canoe's own
	// plan and the inboard fraction the lid uses is about planking clearance
	// for a rectangle, which an ellipse does not need. The bow shoulders that
	// inject the wake sit at 0.9 L, +-B and the transom at -0.95 L, 0
	// (TickWake): the shoulders fall OUTSIDE this ellipse (r = 1.4 there), the
	// transom lands on its edge band. The splats' own radius (BowWakeWidthM x
	// WakeWidthScale) does overlap the mask's rim, which is accepted and
	// display-only: a splat is deposited in the SIMULATION untouched, and this
	// mask only decides where the drawn surface shows the result.
	//
	// ONE COLLECTION, ONE ELLIPSE. MPC_VoxelSky is world-global, so this is the
	// nearest-to-the-camera hull's by construction: every other hull within
	// SleepRadiusUU also pushes, last writer wins, and today there is one boat.
	// The day there are two awake hulls in one lake this becomes a per-hull
	// slot list like the ripple field's splats; noted, not built.
	UWorld* World = GetWorld();
	if (!World || !bHullRippleMaskMpcOk || !HullRippleMaskCollection)
	{
		return;
	}
	if (!bEnabled)
	{
		if (!bHullRippleMaskPushedOff)
		{
			UKismetMaterialLibrary::SetVectorParameterValue(
				World, HullRippleMaskCollection, TEXT("HullEllipseB"),
				FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
			bHullRippleMaskPushedOff = true;
		}
		return;
	}

	const FTransform Xf = GetActorTransform();
	const FVector FwdW = Xf.GetUnitAxis(EAxis::X);
	// Yaw from the hull's own forward axis in XY, exactly as UpdateWaterExclusion
	// takes it: no rotator decomposition, so a pitched or rolled hull keeps the
	// plan heading. A vertical hull (capsized end-up) degenerates to yaw 0.
	const double FwdLen = FVector2D(FwdW.X, FwdW.Y).Size();
	const double CosYaw = FwdLen > 1e-6 ? FwdW.X / FwdLen : 1.0;
	const double SinYaw = FwdLen > 1e-6 ? FwdW.Y / FwdLen : 0.0;
	const FVector Centre = Xf.GetLocation();
	const double HalfLen = HalfLengthUU * VoxelBoatLocal::kExclusionPlanFraction;
	const double HalfBeam = HalfBeamUU;
	const double EdgeUU =
		FMath::Max(1.0, double(CVarVoxelBoatHullRippleMaskEdgeUU.GetValueOnGameThread()));

	// LWC NOTE: the centre goes through a float4, so at |x| ~ 6.5e6 UU it carries
	// ~0.5 UU of quantisation -- the same budget the ripple window's origin
	// (RippleFieldOrigin) already spends, and a hundredth of the edge band.
	UKismetMaterialLibrary::SetVectorParameterValue(
		World, HullRippleMaskCollection, TEXT("HullEllipseA"),
		FLinearColor(float(Centre.X), float(Centre.Y), float(CosYaw), float(SinYaw)));
	UKismetMaterialLibrary::SetVectorParameterValue(
		World, HullRippleMaskCollection, TEXT("HullEllipseB"),
		FLinearColor(float(HalfLen), float(HalfBeam), 1.0f, float(EdgeUU)));
	bHullRippleMaskPushedOff = false;

	if (!bHullRippleMaskLogged)
	{
		bHullRippleMaskLogged = true;
		// The engagement line: a log without it is a run in which the cockpit
		// was never masked, whatever the picture looks like.
		UE_LOG(LogVoxelWater, Log,
		       TEXT("VoxelBoat: hull ripple mask ENGAGED halfLen=%.0f halfBeam=%.0f edge=%.0f UU ")
		       TEXT("(MPC_VoxelSky HullEllipseA/B; voxel.Boat.HullRippleMask 0 is the off arm)."),
		       HalfLen, HalfBeam, EdgeUU);
	}
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
	// The exclusion mask is solved on EVERY path, asleep included: it is placed
	// in absolute world space, so a sleeping boat has to HIDE its stations
	// rather than leave them behind in the lake (see the function).
	UpdateWaterExclusion();
	// The ripple mask rides the same world-space argument: an awake hull
	// publishes its ellipse every tick (the boat moves, the sheet does not), a
	// sleeping one publishes the off encoding once and leaves the water alone.
	PushHullRippleMask(!bAsleep && CVarVoxelBoatHullRippleMask.GetValueOnGameThread());

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

// ---------------------------------------------------------------------------
// THE WATER-EXCLUSION MASK, PLACED AGAINST THE WATER AND NOT AGAINST THE HULL
// ---------------------------------------------------------------------------
//
// WHY THIS IS A PER-TICK SOLVE AT ALL. voxel.Boat.WaveBobGain is 6.0 by owner
// directive, which means the buoyancy probes ride a surface six times as tall
// as the one the pixels draw (TickBuoyancy's wave term) and the hull therefore
// heaves and pitches several times further than the water it sits in. A mask
// bolted to the hull is then wrong by up to (gain-1) x the drawn amplitude --
// tens of UU at shipped defaults, against a canoe with 33 UU of freeboard. That
// is the 2026-09-06 owner report ("water is clipping through the front and back
// ends of the cockpit ... because of the more drastic bobbing"), and no lid
// height expressed in the hull's frame answers it: too low and every trough
// puts water over it, too high and the beside-hull artefact comes back.
//
// THE INVARIANT THAT REPLACES A FIXED LID HEIGHT: a station stands exactly
// voxel.Boat.HullMaskLidUU above the LOCAL DRAWN SURFACE, always, wherever the
// hull is.
//
// The lid is the ONLY thing the beside-hull artefact depends on, and the
// 2026-09-06 note that blamed the gunwale had it wrong: a ray that reaches
// water beyond the hull stays above the water surface for its whole crossing,
// so it can only pick this mask up where the mask rises into that band, and the
// ring of open water it then kills is lid/tan(view depression) wide. What the
// gunwale is doing at the time does not enter. That is why gunwale-6 (34 UU)
// carved water outside the hull and why the fix is to hold the lid a few
// centimetres over the water rather than anywhere over the boat.
//
// The floor under the lid is the mirror-vs-pixel mismatch (a few cm, the
// mirror header's own stated band) plus ripple height (the wake measured 4 UU),
// so the knob is a real trade in both directions and is a cvar for that reason.
//
// WHY THE STATIONS ARE WORLD-HORIZONTAL (absolute rotation, yaw only). A lid
// that inherited the hull's pitch is a ramp: to clear the water at its low end
// it has to stand tens of UU above it at the high end, which spends the
// invariant above to buy nothing. Level lids, with the hull's shape carried by
// SEVEN of them along the keel line instead, is the trade the other way.
// Foreshortening is not ignored -- a pitched hull covers less ground and a
// rolled one covers less width, so each station's footprint is scaled by the
// XY projection of the hull's own axes.
//
// WHY THE SURFACE IS SAMPLED AT GAIN 1, WITH THE WPO FADE. The mask has to sit
// on the surface the PIXELS draw, which is the mirror times kWaveWpoFraction
// faded out over kWpoFadeStartM..kWpoFadeEndM of camera distance -- NOT the
// exaggerated one the probes ride. Same mirror, same clock, same published wind
// as TickBuoyancy, and gated by the same fingerprint guard: on stale mirror
// math the boat and this mask fall back to the flat datum together, which is
// the one way they cannot disagree.
//
// WHEN A STATION IS HIDDEN, AND WHY HIDDEN IS THE SAFE DIRECTION. No writer
// means the water is pixel-identical to a world with no boat in it (the
// contract's own off arm), so every case this solve cannot answer hides:
//   * the drawn surface is below the hull bottom at that station -- a bow
//     thrown clear of the water by the exaggerated bob has no water inside it
//     to cull, and a mask left down at the surface would carve the water the
//     hull is flying over;
//   * the column has no datum, or is CA-only (a player-poured pool): the same
//     blind spot the buoyancy documents, and a boat does not float there either;
//   * the boat is asleep past SleepRadiusUU. The components are placed in
//     ABSOLUTE world space, so a stale station is not merely wrong, it is a
//     hole in the lake somewhere the boat used to be.
//
// COST: NumExclusionStations custom-depth boxes per boat, no main pass, no
// shadow, no collision.
void AVoxelBoat::UpdateWaterExclusion()
{
	auto HideAll = [this]()
	{
		for (UStaticMeshComponent* Station : ExclusionStations)
		{
			if (Station)
			{
				Station->SetRenderCustomDepth(false);
			}
		}
	};

	UWorld* World = GetWorld();
	UVoxelWaterSubsystem* Water = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water || bAsleep || !CVarVoxelBoatHullMask.GetValueOnGameThread()
	    || ExclusionPlan.Num() != ExclusionStations.Num())
	{
		HideAll();
		return;
	}

	// The drawn-surface wave term: TickBuoyancy's inputs exactly, minus the gain.
	const bool bWave = CVarVoxelBoatWaveBob.GetValueOnGameThread()
	                   && VoxelBoatLocal::WaveMirrorFingerprintOK();
	float ScaledTimeS = 0.f;
	float WindNorthMS = 0.f, WindEastMS = 0.f;
	if (bWave)
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
	FVector CamLoc = FVector::ZeroVector;
	const bool bHaveCam = VoxelBoatLocal::CameraLocation(World, CamLoc);

	const FTransform Xf = GetActorTransform();
	const FVector FwdW = Xf.GetUnitAxis(EAxis::X);
	const FVector RightW = Xf.GetUnitAxis(EAxis::Y);
	// cos(pitch) and cos(roll) as the LENGTH OF THE XY PROJECTION of the hull's
	// own axes -- no rotator decomposition, no gimbal cases. Floored so a
	// capsized hull produces a thin mask rather than a zero-scale component.
	const double CosPitch = FMath::Clamp(double(FVector2D(FwdW.X, FwdW.Y).Size()), 0.25, 1.0);
	const double CosRoll = FMath::Clamp(double(FVector2D(RightW.X, RightW.Y).Size()), 0.25, 1.0);
	const FRotator YawOnly(0.0, FMath::RadiansToDegrees(FMath::Atan2(FwdW.Y, FwdW.X)), 0.0);
	const bool bDebugDraw = CVarVoxelBoatDebugDraw.GetValueOnGameThread();
	const double LidUU =
		FMath::Max(0.0, double(CVarVoxelBoatHullMaskLidUU.GetValueOnGameThread()));

	for (int32 I = 0; I < ExclusionStations.Num(); ++I)
	{
		UStaticMeshComponent* Station = ExclusionStations[I];
		if (!Station)
		{
			continue;
		}
		const FVoxelBoatExclusionStation& Plan = ExclusionPlan[I];
		const FVector CentreW = Xf.TransformPosition(FVector(Plan.LocalX, 0.0, 0.0));
		// The hull bottom AT THIS STATION, through the hull's real orientation --
		// this is the number a pitched bow rises on.
		const double KeelZ = Xf.TransformPosition(FVector(Plan.LocalX, 0.0, KeelOffsetUU)).Z;

		FWaterSurfaceSample S;
		if (!Water->WaterSurfaceZAtWorld(CentreW.X, CentreW.Y, S)
		    || S.Kind == EWaterSurfaceKind::CAOnly)
		{
			Station->SetRenderCustomDepth(false);
			continue;
		}
		double SurfaceZ = S.SurfaceZUU;
		if (bWave)
		{
			double Fade = 1.0;
			if (bHaveCam)
			{
				const double DistM = FVector::Dist(CamLoc, CentreW) / 100.0;
				Fade = 1.0
				       - FMath::Clamp((DistM - double(VoxelWaveMirror::kWpoFadeStartM))
				                          / double(VoxelWaveMirror::kWpoFadeEndM
				                                   - VoxelWaveMirror::kWpoFadeStartM),
				                      0.0, 1.0);
			}
			SurfaceZ += double(VoxelWaveMirror::FieldHeightM(
				            float(CentreW.X / 100.0), float(CentreW.Y / 100.0), ScaledTimeS,
				            WindNorthMS, WindEastMS))
			            * VoxelWaveMirror::kWaveWpoFraction * 100.0 * Fade;
		}

		if (SurfaceZ < KeelZ - VoxelBoatLocal::kExclusionKeelSlackUU)
		{
			Station->SetRenderCustomDepth(false);
			continue;
		}

		const double TopZ = SurfaceZ + LidUU;
		const double BottomZ = FMath::Min(KeelZ, SurfaceZ) - VoxelBoatLocal::kExclusionSkirtUU;
		const double HalfH = FMath::Max(1.0, 0.5 * (TopZ - BottomZ));
		Station->SetWorldLocationAndRotation(
			FVector(CentreW.X, CentreW.Y, 0.5 * (TopZ + BottomZ)), YawOnly);
		// The engine cube is 100 UU on a side, so scale = half-extent / 50.
		Station->SetWorldScale3D(FVector(Plan.HalfLenUU * CosPitch / 50.0,
		                                 Plan.HalfBeamUU * CosRoll / 50.0, HalfH / 50.0));
		Station->SetRenderCustomDepth(true);

		if (bDebugDraw)
		{
			// The mask is invisible by construction, so the only way to see a
			// station in the wrong place is to draw it.
			DrawDebugBox(World, FVector(CentreW.X, CentreW.Y, 0.5 * (TopZ + BottomZ)),
			             FVector(Plan.HalfLenUU * CosPitch, Plan.HalfBeamUU * CosRoll, HalfH),
			             YawOnly.Quaternion(), FColor::Yellow, false, 0.f, 0, 1.0f);
		}
	}
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
		// Ring radius scale, live (owner 2026-09-08: finer, more numerous ripples).
		const float WakeWidthScale =
			FMath::Clamp(CVarVoxelBoatWakeWidthScale.GetValueOnGameThread(), 0.1f, 4.f);
		UVoxelRippleFieldSubsystem::AddSweptDisturbanceAt(World, LastBowPort, BowPort,
		                                                  float(BowWakeWidthM) * WakeWidthScale,
		                                                  float(BowWakeStrengthM) * Frac);
		UVoxelRippleFieldSubsystem::AddSweptDisturbanceAt(World, LastBowStarboard, BowStarboard,
		                                                  float(BowWakeWidthM) * WakeWidthScale,
		                                                  float(BowWakeStrengthM) * Frac);
		// The transom trail is wider and weaker: it is the hollow a hull leaves
		// behind it, not the crest it pushes ahead.
		UVoxelRippleFieldSubsystem::AddSweptDisturbanceAt(World, LastTransom, Transom,
		                                                  float(TransomWakeWidthM) * WakeWidthScale,
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
