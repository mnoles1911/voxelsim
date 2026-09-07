#include "VoxelGlider.h"

#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h" // TActorIterator
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "HAL/IConsoleManager.h"

#include "VoxelAssetBody.h"
#include "VoxelBoat.h" // the other vehicle, for the "already in one" refusal
#include "VoxelCoords.h"
#include "VoxelDebug.h" // LogVoxelWater -- the ditch splash
#include "VoxelEarth.h" // LogVoxelEarth
#include "VoxelRippleField.h"
#include "VoxelWaterSubsystem.h"
#include "VoxelWeatherSubsystem.h"
#include "VoxelWorldSubsystem.h"

// ---------------------------------------------------------------------------
// CONSOLE VARIABLES AND COMMANDS
// ---------------------------------------------------------------------------

TAutoConsoleVariable<FString> CVarVoxelGliderAsset(
	TEXT("voxel.Glider.Asset"), TEXT("glider"),
	TEXT("asset-forge library name for the glider's visual grid. Same resolver, same loud fallback ")
	TEXT("as voxel.Boat.Asset -- a wing-shaped placeholder at the same pitch, and a log line saying ")
	TEXT("it is one."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelGliderAero(
	TEXT("voxel.Glider.Aero"), true,
	TEXT("Lift and drag. 0 leaves gravity and the rate controls only, which is the A arm for 'is it ")
	TEXT("actually flying': with this off it MUST fall at g and cover no ground. Anyone quoting a ")
	TEXT("glide ratio should have run this arm first."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelGliderWind(
	TEXT("voxel.Glider.Wind"), true,
	TEXT("Subtract the weather subsystem's wind field from the glider's velocity to get airspeed. ")
	TEXT("0 flies in still air, which is the only condition in which a glide-ratio measurement means ")
	TEXT("anything -- a tailwind flatters the ratio and a headwind ruins it, and neither is the ")
	TEXT("wing's fault."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelGliderDeployAnywhere(
	TEXT("voxel.Glider.DeployAnywhere"), false,
	TEXT("Skip the airborne + minimum-clearance test on deploy. FOR TESTING ONLY: it exists so a ")
	TEXT("leg can open a wing standing on the ground, and it is off by default because a glider ")
	TEXT("deployed at head height is a landing that happens before anyone sees a glider."),
	ECVF_Default);

namespace VoxelGliderLocal
{
// Prefixed, not anonymous -- the unity-blob collision on record
// (VoxelRippleField.h, 2026-08-23).

// Ground clearance under a world point, or false if the column has not
// streamed. A MISS IS NOT CLEAR AIR: "absence reads as air" is a documented way
// this project has deleted terrain twice, and a glider that treated a miss as
// "nothing below" would fly through the far wall of a valley.
bool ClearanceAboveGround(const UWorld* World, const FVector& P, double& OutClearanceUU,
                          double& OutSurfaceTopZ)
{
	UVoxelWorldSubsystem* Voxels =
		World ? const_cast<UWorld*>(World)->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Voxels)
	{
		return false;
	}
	FVector HitCentre, PrevCentre;
	const FVector Start = P + FVector(0.0, 0.0, VoxelCoords::VoxelSizeUU);
	if (!Voxels->RaycastVoxelWorld(Start, FVector(0.0, 0.0, -1.0),
	                               VoxelGliderTuning::GroundProbeUU, HitCentre, PrevCentre))
	{
		return false;
	}
	OutSurfaceTopZ = HitCentre.Z + VoxelCoords::VoxelSizeUU * 0.5;
	OutClearanceUU = P.Z - OutSurfaceTopZ;
	return true;
}
} // namespace VoxelGliderLocal

AVoxelGlider::AVoxelGlider()
{
	PrimaryActorTick.bCanEverTick = true;

	AutoPossessPlayer = EAutoReceiveInput::Disabled;
	// The attitude below IS the actor rotation. Letting the controller write it
	// would mean two authorities for one transform.
	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	Body = CreateDefaultSubobject<UVoxelAssetBodyComponent>(TEXT("AssetBody"));
	Body->SetupAttachment(SceneRoot);
	Body->FallbackShape = EVoxelAssetBodyFallback::Wing;
	// Span along +Y is what a wing wants, but FallbackSizeM is (X length, Y beam,
	// Z height) and the placeholder's plate spans Y -- so a 3 m chord by 9 m span
	// glider is (3, 9, 0.6). Stated because the axis order reads backwards for a
	// wing and would otherwise be "fixed" into a 9 m chord.
	Body->FallbackSizeM = FVector(3.0, 9.0, 0.6);
	// 50 mm, not the component's 25 mm default: a two-voxel plate this size at
	// 25 mm is ~89k cells (nothing has six solid neighbours, so the shell
	// filter keeps ALL of it), which blows the 40k instance cap and renders as
	// an every-third-voxel sieve (review finding #6). At 50 mm the plate is
	// ~22k and draws solid. The real glider.json arrives at its own pitch and
	// ignores this.
	Body->FallbackPitchMm = 50;
	Body->PlaceholderTint = FLinearColor(0.80f, 0.80f, 0.84f, 1.f);

	CameraArm = CreateDefaultSubobject<USceneComponent>(TEXT("CameraArm"));
	CameraArm->SetupAttachment(SceneRoot);

	ChaseCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("ChaseCamera"));
	ChaseCamera->SetupAttachment(CameraArm);
	ChaseCamera->SetRelativeLocation(
		FVector(-VoxelGliderTuning::CameraBackUU, 0.0, VoxelGliderTuning::CameraUpUU));
}

void AVoxelGlider::BeginPlay()
{
	Super::BeginPlay();

	if (Body)
	{
		Body->AssetName = CVarVoxelGliderAsset.GetValueOnGameThread();
		Body->Build();
		const FBox B = Body->GetLocalBoundsUU();
		if (B.IsValid)
		{
			Body->SetRelativeLocation(-B.GetCenter());
			NoseOffsetUU = B.GetExtent().X;
			// Belly and span follow the thing on screen, the boat's
			// AdoptHullFromBody discipline: a parked glider whose belly constant
			// disagreed with its drawn underside would hover or sink visibly.
			BellyOffsetUU = -B.GetExtent().Z;
			HalfSpanUU = B.GetExtent().Y;
		}
	}

	const FRotator R = GetActorRotation();
	YawDeg = R.Yaw;
	PitchDeg = R.Pitch;
	RollDeg = R.Roll;

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelGlider spawned at (%.0f,%.0f,%.0f) heading %.0f deg. Wing %.1f m^2, %.0f kg, ")
	       TEXT("max L/D %.2f (CD0 %.3f, k %.4f). Visual: %s (%d instances, pitch %.1f mm)."),
	       GetActorLocation().X, GetActorLocation().Y, GetActorLocation().Z, YawDeg,
	       VoxelGliderTuning::WingAreaM2, VoxelGliderTuning::MassKg,
	       1.0 / (2.0 * FMath::Sqrt(VoxelGliderTuning::CD0 * VoxelGliderTuning::InducedK)),
	       VoxelGliderTuning::CD0, VoxelGliderTuning::InducedK,
	       (Body && !Body->IsPlaceholder()) ? *Body->GetResolvedPath() : TEXT("PLACEHOLDER"),
	       Body ? Body->GetInstanceCount() : 0, Body ? Body->GetPitchUU() * 10.0 : 0.0);
}

void AVoxelGlider::EndPlay(const EEndPlayReason::Type Reason)
{
	// NEVER STRAND THE PLAYER -- the same rule AVoxelBoat::EndPlay states. A
	// glider destroyed by anything other than its own landing path must still
	// hand the pawn back before it goes. ONLY on Destroyed: on level
	// transition / quit / PIE end the stored pawn is tearing down too
	// (review finding #9).
	if (Reason == EEndPlayReason::Destroyed && StoredPawn.IsValid())
	{
		ReturnPilot(GetActorLocation());
	}
	Super::EndPlay(Reason);
}

void AVoxelGlider::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bFinished || DeltaSeconds <= 0.f)
	{
		return;
	}

	// --- parked: inert on purpose --------------------------------------------
	//
	// No aero, no integration, no terrain probes -- a parked glider is scenery
	// that can be boarded, and Ticks stays a FLIGHT counter (ParkTicks counts
	// this state, so voxel.Glider.Stat can tell "parked all along" from "never
	// ticked"). The only live inputs are the launch keys and the look camera.
	if (bParked)
	{
		++ParkTicks;
		if (StoredPawn.IsValid() && (bLaunchPressed || PitchInput < -0.5f))
		{
			// W is the pitch axis at -1 (nose down, the "push forward" key) and
			// Space is the bound launch action; either reads as "go".
			Launch(bLaunchPressed ? TEXT("Space") : TEXT("W"));
		}
		bLaunchPressed = false;
		if (CameraArm)
		{
			CameraArm->SetRelativeRotation(
				FRotator(float(CameraPitchDeg), float(CameraYawDeg), 0.f));
		}
		return;
	}

	++Ticks;

	using namespace VoxelGliderTuning;
	UWorld* World = GetWorld();

	// --- attitude, from rate controls ----------------------------------------
	//
	// The stick commands a RATE. Arcade on purpose (see the stall note in the
	// tuning block): the alternative is a moment model, which needs an inertia
	// tensor, a damping model and a trim state to be flyable, and none of those
	// are things the plan asked for or that an owner screenshot can judge.
	PitchDeg = FMath::Clamp(PitchDeg + double(PitchInput) * PitchRateDegPerSec * DeltaSeconds,
	                        -MaxPitchDeg, MaxPitchDeg);
	if (FMath::IsNearlyZero(RollInput))
	{
		// Wings level themselves slowly with the stick centred. Small enough that
		// the glider does not fly itself.
		RollDeg = FMath::Lerp(RollDeg, 0.0, FMath::Min(1.0, RollLevelPerSec * DeltaSeconds));
	}
	else
	{
		RollDeg = FMath::Clamp(RollDeg + double(RollInput) * RollRateDegPerSec * DeltaSeconds,
		                       -MaxRollDeg, MaxRollDeg);
	}

	// --- wind, and the axis order the header warns about ---------------------
	FVector WindUU = FVector::ZeroVector;
	if (CVarVoxelGliderWind.GetValueOnGameThread())
	{
		if (UVoxelWeatherSubsystem* Weather = World ? World->GetSubsystem<UVoxelWeatherSubsystem>()
		                                            : nullptr)
		{
			const FVector Loc = GetActorLocation();
			const FVoxelWindSample W = Weather->SampleWindAtWorldUU(Loc.X, Loc.Y);
			// X = NORTH, Y = EAST (VoxelEphemeris.h:43-45; PublishWind's own
			// thirty-line note on having had this backwards once). NOT the
			// (x=east, y=north) order FVoxelWindSample's field comment suggests.
			WindUU = FVector(W.NorthMps, W.EastMps, 0.0) * 100.0;
			// SpeedMps is the AUTHORITY on speed and is deliberately not
			// recomputed from the vector -- the field's unit direction is up to
			// half a percent off unit length.
			LastWindMS = W.SpeedMps;
		}
	}

	// --- aero ------------------------------------------------------------------
	//
	// SI INSIDE THIS BLOCK AND NOWHERE ELSE. Every coefficient is tabulated in SI
	// in every source anyone would check them against, so the aero is computed in
	// metres and newtons and converted to UU exactly once, at the integration
	// below. The two are never mixed inside one expression.
	const FVector AirVelUU = VelocityUU - WindUU;
	const FVector VaMS = AirVelUU / 100.0;
	const double SpeedMS = VaMS.Size();
	LastAirspeedMS = SpeedMS;

	FVector AccelMS2(0.0, 0.0, -GravityMS2);

	if (CVarVoxelGliderAero.GetValueOnGameThread() && SpeedMS > KINDA_SMALL_NUMBER)
	{
		const FRotator Attitude{float(PitchDeg), float(YawDeg), float(RollDeg)};
		const FVector Fwd = Attitude.RotateVector(FVector::ForwardVector);
		const FVector Up = Attitude.RotateVector(FVector::UpVector);
		const FVector VaHat = VaMS / SpeedMS;

		// Angle of attack: the angle between the airflow and the wing's own
		// forward, measured in the plane containing that forward and the wing's
		// up. Positive when the air arrives from BELOW the wing, which is the
		// sign convention CL0 + CLAlpha*alpha assumes.
		const double U = FVector::DotProduct(VaMS, Fwd);
		const double Wv = -FVector::DotProduct(VaMS, Up);
		const double Alpha = FMath::Atan2(Wv, U);
		LastAlphaDeg = FMath::RadiansToDegrees(Alpha);

		// A CLAMP, not a break. A real wing loses lift past the stall; this one
		// merely stops gaining any. Forgiving on purpose -- a v1 glider that
		// departs controlled flight is a glider nobody photographs.
		const double CL = FMath::Clamp(CL0 + CLAlphaPerRad * Alpha, CLMin, CLMax);
		const double CD = CD0 + InducedK * CL * CL;
		LastGlideRatio = (CD > KINDA_SMALL_NUMBER) ? (CL / CD) : 0.0;

		// Below MinAirspeedMS the aero fades out entirely: q -> 0 makes every
		// coefficient meaningless and the flow direction numerically garbage, and
		// a NaN attitude is unrecoverable. A fade rather than a cliff so the
		// transition is not a visible kick.
		const double Fade =
			FMath::Clamp((SpeedMS - MinAirspeedMS) / FMath::Max(0.001, AeroFadeBandMS), 0.0, 1.0);
		if (Fade > 0.0)
		{
			++AeroTicks;
			const double Q = 0.5 * AirDensityKgM3 * SpeedMS * SpeedMS;
			const double LiftN = Q * WingAreaM2 * CL * Fade;
			const double DragN = Q * WingAreaM2 * CD * Fade;

			// Lift acts perpendicular to the AIRFLOW, in the plane of the wing's
			// up vector -- which is what makes a banked wing turn instead of
			// merely leaning. Drag acts straight back along the flow.
			FVector LiftDir = Up - FVector::DotProduct(Up, VaHat) * VaHat;
			if (LiftDir.Normalize())
			{
				AccelMS2 += LiftDir * (LiftN / MassKg);
			}
			AccelMS2 -= VaHat * (DragN / MassKg);

			// --- the coordinated turn -----------------------------------------
			//
			// yawRate = g * tan(roll) / V. This is the real relation, not an
			// approximation of one, which is why there is no gain to tune: a
			// banked wing turns at that rate or it is slipping. Capped so a steep
			// bank at low speed cannot ask for an infinite rate.
			const double YawRateDeg = FMath::Clamp(
				FMath::RadiansToDegrees(GravityMS2 * FMath::Tan(FMath::DegreesToRadians(RollDeg))
				                        / FMath::Max(MinAirspeedMS, SpeedMS)),
				-MaxYawRateDegPerSec, MaxYawRateDegPerSec);
			YawDeg = FMath::UnwindDegrees(YawDeg + YawRateDeg * DeltaSeconds);
		}
	}

	SetActorRotation(FRotator(float(PitchDeg), float(YawDeg), float(RollDeg)));

	// --- integrate --------------------------------------------------------------
	// The ONE conversion from SI to UU, and it happens here so no expression above
	// ever mixed the two.
	VelocityUU += AccelMS2 * 100.0 * double(DeltaSeconds);
	SetActorLocation(GetActorLocation() + VelocityUU * double(DeltaSeconds), false, nullptr,
	                 ETeleportType::TeleportPhysics);

	// Water first: a glider coming down over a lake shore should ditch rather
	// than land on the lake bed it is passing over.
	if (CheckWater())
	{
		return;
	}
	if (CheckTerrain(DeltaSeconds))
	{
		return;
	}

	// --- camera ---------------------------------------------------------------
	if (CameraArm)
	{
		CameraArm->SetRelativeRotation(FRotator(float(CameraPitchDeg), float(CameraYawDeg), 0.f));
	}
}

bool AVoxelGlider::CheckTerrain(float DeltaSeconds)
{
	using namespace VoxelGliderTuning;
	UWorld* World = GetWorld();
	const FVector Loc = GetActorLocation();

	double Clearance = 0.0;
	double SurfaceTopZ = 0.0;
	if (!VoxelGliderLocal::ClearanceAboveGround(World, Loc, Clearance, SurfaceTopZ))
	{
		// The column has not streamed. NOT "there is no ground" -- see the helper.
		// The glider keeps flying and will find the ground when the tile arrives.
		return false;
	}
	++GroundHits;

	if (Clearance <= TouchdownClearanceUU)
	{
		// PARK, do not destroy (owner respec 2026-09-05). v1 funnelled this
		// into ReturnPilot, whose unconditional Destroy() is exactly the
		// "despawned or fell through the surface" the owner reported on the
		// spawned glider: with no StoredPawn there was nobody to repossess and
		// the actor simply vanished at ground contact. Now the glider settles
		// onto the raycast surface and STAYS; a pilot aboard stays aboard,
		// with the launch keys live and E to dismount.
		SettleParked(SurfaceTopZ);
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelGlider: LANDED and parked at (%.0f,%.0f,%.0f) after %llu ticks (%llu with ")
		       TEXT("aero). Airspeed was %.1f m/s, alpha %.1f deg, L/D %.2f, wind %.1f m/s. %s"),
		       Loc.X, Loc.Y, SurfaceTopZ, (unsigned long long)Ticks, (unsigned long long)AeroTicks,
		       LastAirspeedMS, LastAlphaDeg, LastGlideRatio, LastWindMS,
		       IsCrewed() ? TEXT("Pilot aboard: W or Space relaunches, E dismounts.")
		                  : TEXT("Unpossessed: walk up and press E to board."));
		return true;
	}

	if (Clearance < FlareAltitudeUU)
	{
		// The flare: pitch up as the ground arrives, which trades speed for a
		// softer arrival. Applied as a bias toward a target pitch rather than as a
		// rate, so it cannot fight the stick into an oscillation.
		++FlareTicks;
		const double Blend = FMath::Clamp(1.0 - Clearance / FlareAltitudeUU, 0.0, 1.0);
		PitchDeg = FMath::Lerp(PitchDeg, FlarePitchDeg, Blend * FMath::Min(1.0, 3.0 * DeltaSeconds));
	}
	return false;
}

bool AVoxelGlider::CheckWater()
{
	// NO HULL WATER-EXCLUSION VOLUME HERE, deliberately (2026-09-05, same
	// session that gave AVoxelBoat one): a glider never floats -- ditching
	// destroys it in this function, and it parks on LAND -- so there is no
	// sustained hull-below-waterline frame for water to clip through. The
	// stencil-bit-0 contract (Tools/water_hull_mask_graph.py) is ready if a
	// floating glider ever exists.
	using namespace VoxelGliderTuning;
	UWorld* World = GetWorld();
	UVoxelWaterSubsystem* Water = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		return false;
	}

	// AT THE NOSE, not at the centre: a glider arrives nose first and the point
	// that touches the water is the point that decides it has.
	const FVector Nose = GetActorTransform().TransformPosition(FVector(NoseOffsetUU, 0.0, 0.0));
	if (!Water->IsUnderwaterAtWorld(Nose))
	{
		return false;
	}

	// ONE splash. AddDisturbanceAt, not the swept form: a ditching is an impact
	// at a point, not a wake, and the field's four drop counters will say if it
	// landed outside the window or on ground the bake calls dry.
	UVoxelRippleFieldSubsystem::AddDisturbanceAt(World, Nose, float(DitchSplashRadiusM),
	                                             float(DitchSplashStrengthM));

	// Put the pilot at the surface rather than at the nose, which is by now under
	// it. Datum query, so this is correct over unstreamed ground.
	FVector Out = Nose;
	FWaterSurfaceSample S;
	if (Water->WaterSurfaceZAtWorld(Nose.X, Nose.Y, S) && S.Kind != EWaterSurfaceKind::CAOnly)
	{
		Out.Z = S.SurfaceZUU;
	}

	UE_LOG(LogVoxelWater, Log,
	       TEXT("VoxelGlider: DITCHED at (%.0f,%.0f,%.0f) after %llu ticks (%llu with aero). One ")
	       TEXT("splash injected -- voxel.Water.Ripple.Stat says whether the field took it."),
	       Out.X, Out.Y, Out.Z, (unsigned long long)Ticks, (unsigned long long)AeroTicks);
	ReturnPilot(Out);
	return true;
}

void AVoxelGlider::ReturnPilot(const FVector& PilotWorldPos)
{
	if (bFinished)
	{
		return;
	}
	bFinished = true;

	APawn* Pawn = StoredPawn.Get();
	APlayerController* PC = Driver.Get();
	StoredPawn = nullptr;
	Driver = nullptr;

	if (Pawn && PC)
	{
		Pawn->SetActorHiddenInGame(false);
		Pawn->SetActorEnableCollision(true);
		Pawn->SetActorTickEnabled(true);
		Pawn->SetActorLocation(PilotWorldPos, false, nullptr, ETeleportType::TeleportPhysics);
		PC->Possess(Pawn);
	}

	// This path DESTROYS: it is the stow, the mid-air bail and the water ditch.
	// A terrain landing does not come here any more -- it parks (SettleParked),
	// per the owner's 2026-09-05 respec, and a parked glider is inert rather
	// than "integrating forever", which was v1's reason for destroying on
	// every ending.
	Destroy();
}

// ---------------------------------------------------------------------------
// Parking, boarding, launching (owner respec 2026-09-05)
// ---------------------------------------------------------------------------

void AVoxelGlider::SettleParked(double SurfaceTopZ)
{
	// Level pose at the current heading -- a wing at rest sits flat -- with the
	// BELLY on the voxel top. Setting Z absolutely (rather than nudging by the
	// clearance) also repairs any penetration a fast final tick stepped into.
	VelocityUU = FVector::ZeroVector;
	PitchDeg = 0.0;
	RollDeg = 0.0;
	PitchInput = 0.f;
	RollInput = 0.f;
	bLaunchPressed = false;
	SetActorRotation(FRotator(0.f, float(YawDeg), 0.f));
	FVector Loc = GetActorLocation();
	Loc.Z = SurfaceTopZ - BellyOffsetUU; // BellyOffsetUU is negative: belly below origin
	SetActorLocation(Loc, false, nullptr, ETeleportType::TeleportPhysics);
	bParked = true;
}

void AVoxelGlider::Launch(const TCHAR* TriggerDesc)
{
	using namespace VoxelGliderTuning;
	if (!bParked || !StoredPawn.IsValid())
	{
		return;
	}
	bParked = false;
	bLaunchPressed = false;

	// The simplest honest v2 (logged as such in the plan): an impulse, not a
	// ground run. Nose up at LaunchPitchDeg, velocity along the wing's own
	// forward at LaunchSpeedMS -- above the aero fade band, so the wing flies
	// from its first airborne tick instead of pancaking back onto the ground.
	PitchDeg = LaunchPitchDeg;
	RollDeg = 0.0;
	const FRotator Attitude(float(PitchDeg), float(YawDeg), 0.f);
	SetActorRotation(Attitude);
	VelocityUU = Attitude.Vector() * (LaunchSpeedMS * 100.0);

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelGlider: LAUNCHED from park via %s at (%.0f,%.0f,%.0f): %.1f m/s along heading ")
	       TEXT("%.0f deg, pitch %.0f deg (impulse launch -- no ground run in v2)."),
	       TriggerDesc, GetActorLocation().X, GetActorLocation().Y, GetActorLocation().Z,
	       LaunchSpeedMS, YawDeg, LaunchPitchDeg);
}

AVoxelGlider* AVoxelGlider::FindNearestParked(const UWorld* World, const FVector& From,
                                              double MaxRangeUU)
{
	if (!World)
	{
		return nullptr;
	}
	AVoxelGlider* Best = nullptr;
	double BestSq = MaxRangeUU * MaxRangeUU;
	for (TActorIterator<AVoxelGlider> It(const_cast<UWorld*>(World)); It; ++It)
	{
		AVoxelGlider* G = *It;
		// Parked AND uncrewed: a glider in flight is not a thing to walk up to,
		// and one with a pilot already aboard is taken.
		if (!G || !G->IsParked() || G->IsCrewed())
		{
			continue;
		}
		const double DSq = FVector::DistSquared(From, G->GetActorLocation());
		if (DSq <= BestSq)
		{
			BestSq = DSq;
			Best = G;
		}
	}
	return Best;
}

bool AVoxelGlider::TryBoardNearest(APlayerController* PC)
{
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return false;
	}
	AVoxelGlider* G = FindNearestParked(PC->GetWorld(), Pawn->GetActorLocation(),
	                                    VoxelGliderTuning::InteractRangeUU);
	if (!G)
	{
		// SAY WHY, the boat's rule: no glider at all, a glider too far, and a
		// glider still airborne want different answers.
		int32 Total = 0, Parked = 0;
		for (TActorIterator<AVoxelGlider> It(PC->GetWorld()); It; ++It)
		{
			++Total;
			Parked += (It->IsParked() && !It->IsCrewed()) ? 1 : 0;
		}
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelGlider: nothing to board within %.1f m. %d glider(s) exist, %d parked and ")
		       TEXT("free. voxel.Glider.Spawn parks one in front of you."),
		       VoxelGliderTuning::InteractRangeUU / 100.0, Total, Parked);
		return false;
	}
	return G->Board(PC);
}

bool AVoxelGlider::Board(APlayerController* PC)
{
	APawn* Previous = PC ? PC->GetPawn() : nullptr;
	if (!PC || !Previous || Previous == this || StoredPawn.IsValid() || !bParked)
	{
		return false;
	}

	StoredPawn = Previous;
	Driver = PC;

	// The outgoing pawn is PARKED, not destroyed -- AVoxelBoat::Enter's rule,
	// for AVoxelBoat::Enter's reasons (camera mode, speed dial, crouch state
	// all live on it).
	Previous->SetActorHiddenInGame(true);
	Previous->SetActorEnableCollision(false);
	Previous->SetActorTickEnabled(false);

	PC->Possess(this);

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("Glider: boarded via E at (%.0f,%.0f,%.0f). Stored pawn '%s' parked. W or Space ")
	       TEXT("launches; E dismounts and leaves the glider parked; X stows it."),
	       GetActorLocation().X, GetActorLocation().Y, GetActorLocation().Z,
	       *Previous->GetName());
	return true;
}

void AVoxelGlider::ExitToStoredPawn()
{
	using namespace VoxelGliderTuning;

	if (!bParked)
	{
		// Airborne E: bail out where it is, exactly the v1 behaviour. The wing
		// is stowed (destroyed), the pilot falls.
		ReturnPilot(GetActorLocation());
		return;
	}

	APawn* Pawn = StoredPawn.Get();
	APlayerController* PC = Driver.Get();
	StoredPawn = nullptr;
	Driver = nullptr;
	if (!Pawn || !PC)
	{
		return;
	}

	// Off the WINGTIP, clear of the span -- stepping out of a 9 m wing at the
	// beam would put the pilot inside it. Feet on the raycast ground when the
	// column has streamed; the lift above the glider's own origin when it has
	// not (never "absence reads as air").
	const FTransform Xf = GetActorTransform();
	FVector Out = Xf.TransformPosition(FVector(0.0, HalfSpanUU + ExitSideClearanceUU, 0.0));
	double Clearance = 0.0, SurfaceTopZ = 0.0;
	if (VoxelGliderLocal::ClearanceAboveGround(GetWorld(), Out, Clearance, SurfaceTopZ))
	{
		Out.Z = SurfaceTopZ + VoxelMovementTuning::StandHalfExtentZ;
	}
	else
	{
		Out.Z = GetActorLocation().Z + ExitLiftUU;
	}

	Pawn->SetActorHiddenInGame(false);
	Pawn->SetActorEnableCollision(true);
	Pawn->SetActorTickEnabled(true);
	Pawn->SetActorLocation(Out, false, nullptr, ETeleportType::TeleportPhysics);
	PC->Possess(Pawn);

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelGlider: DISMOUNTED to (%.0f,%.0f,%.0f). Pawn '%s' repossessed; the glider ")
	       TEXT("stays parked at (%.0f,%.0f,%.0f)."),
	       Out.X, Out.Y, Out.Z, *Pawn->GetName(), GetActorLocation().X, GetActorLocation().Y,
	       GetActorLocation().Z);
}

bool AVoxelGlider::TryDeploy(APlayerController* PC)
{
	using namespace VoxelGliderTuning;

	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	UWorld* World = PC ? PC->GetWorld() : nullptr;
	if (!Pawn || !World)
	{
		return false;
	}
	if (Pawn->IsA<AVoxelGlider>() || Pawn->IsA<AVoxelBoat>())
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelGlider: cannot deploy from inside a vehicle. Step out first."));
		return false;
	}

	const FVector Loc = Pawn->GetActorLocation();
	const bool bAnywhere = CVarVoxelGliderDeployAnywhere.GetValueOnGameThread();
	if (!bAnywhere)
	{
		// REFUSE, AND SAY WHICH TEST FAILED. "Deploy while falling" is the plan's
		// launch rule and the clearance is what makes it mean something -- a wing
		// opened at head height is a landing that happens before anyone sees a
		// glider.
		double Clearance = 0.0, SurfaceTopZ = 0.0;
		if (!VoxelGliderLocal::ClearanceAboveGround(World, Loc, Clearance, SurfaceTopZ))
		{
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("VoxelGlider: no ground found under the pawn -- the column has not streamed, ")
			       TEXT("so the deploy clearance cannot be checked. Refusing rather than guessing. ")
			       TEXT("voxel.Glider.DeployAnywhere 1 overrides."));
			return false;
		}
		if (Clearance < DeployMinClearanceUU)
		{
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("VoxelGlider: %.1f m above ground, needs %.1f. Jump or fly higher first, or ")
			       TEXT("set voxel.Glider.DeployAnywhere 1."),
			       Clearance / 100.0, DeployMinClearanceUU / 100.0);
			return false;
		}
	}

	FRotator Attitude = PC->GetControlRotation();
	Attitude.Pitch = FMath::Clamp(Attitude.Pitch, -20.f, 5.f);
	Attitude.Roll = 0.f;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AVoxelGlider* G =
		World->SpawnActor<AVoxelGlider>(AVoxelGlider::StaticClass(), Loc, Attitude, Params);
	if (!G)
	{
		UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelGlider: SpawnActor failed."));
		return false;
	}

	// Inherit the pawn's velocity so opening at speed keeps that speed, then floor
	// it along the wing's own forward: a glider deployed from a standstill needs
	// enough airspeed to be controllable in the first second, and the alternative
	// is a second of freefall that reads as the key not having worked.
	FVector V = Pawn->GetVelocity();
	const FVector Fwd = Attitude.Vector();
	if (FVector::DotProduct(V, Fwd) < DeployMinSpeedMS * 100.0)
	{
		V += Fwd * (DeployMinSpeedMS * 100.0 - FVector::DotProduct(V, Fwd));
	}
	G->VelocityUU = V;

	G->StoredPawn = Pawn;
	G->Driver = PC;
	Pawn->SetActorHiddenInGame(true);
	Pawn->SetActorEnableCollision(false);
	Pawn->SetActorTickEnabled(false);
	PC->Possess(G);

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelGlider: DEPLOYED at (%.0f,%.0f,%.0f) heading %.0f deg with %.1f m/s. Stored ")
	       TEXT("pawn '%s' parked. The ripple window now follows the glider."),
	       Loc.X, Loc.Y, Loc.Z, Attitude.Yaw, V.Size() / 100.0, *Pawn->GetName());
	return true;
}

AVoxelGlider* AVoxelGlider::SpawnAhead(UWorld* World, double AheadUU, double AltitudeUU)
{
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		UE_LOG(LogVoxelEarth, Warning, TEXT("voxel.Glider.Spawn: no player pawn to spawn near."));
		return nullptr;
	}
	FRotator ViewRot = PC->GetControlRotation();
	ViewRot.Pitch = 0.f;
	ViewRot.Roll = 0.f;
	const FVector Spawn =
		Pawn->GetActorLocation() + ViewRot.Vector() * AheadUU + FVector(0.0, 0.0, AltitudeUU);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AVoxelGlider* G =
		World->SpawnActor<AVoxelGlider>(AVoxelGlider::StaticClass(), Spawn, ViewRot, Params);
	if (G)
	{
		G->VelocityUU = ViewRot.Vector() * (VoxelGliderTuning::DeployMinSpeedMS * 100.0);
	}
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("voxel.Glider.Spawn: %s AIRBORNE at (%.0f,%.0f,%.0f). It will glide until it lands, ")
	       TEXT("then park on the surface."),
	       G ? TEXT("spawned") : TEXT("FAILED"), Spawn.X, Spawn.Y, Spawn.Z);
	return G;
}

AVoxelGlider* AVoxelGlider::SpawnParkedAhead(UWorld* World, double AheadUU)
{
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		UE_LOG(LogVoxelEarth, Warning, TEXT("voxel.Glider.Spawn: no player pawn to spawn near."));
		return nullptr;
	}
	FRotator ViewRot = PC->GetControlRotation();
	ViewRot.Pitch = 0.f;
	ViewRot.Roll = 0.f;

	// Probe from a few metres above the player's own height, so ground ahead
	// that rises moderately is still found from above rather than from inside.
	FVector Probe = Pawn->GetActorLocation() + ViewRot.Vector() * AheadUU;
	Probe.Z += 400.0;

	// A HIT IS REQUIRED, the house rule: a raycast miss means the column has
	// not streamed, and a glider parked on guessed ground is the fell-through
	// bug wearing a different hat.
	double Clearance = 0.0, SurfaceTopZ = 0.0;
	if (!VoxelGliderLocal::ClearanceAboveGround(World, Probe, Clearance, SurfaceTopZ))
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("voxel.Glider.Spawn: no ground found ahead -- the column has not streamed, so ")
		       TEXT("there is no surface to park on. Refusing rather than guessing; move, or use ")
		       TEXT("the airborne form (add an AltitudeM argument)."));
		return nullptr;
	}
	// A glider does not park on a lake bed. Refuse over water and say which
	// form to use instead.
	if (UVoxelWaterSubsystem* Water = World->GetSubsystem<UVoxelWaterSubsystem>())
	{
		if (Water->IsUnderwaterAtWorld(FVector(Probe.X, Probe.Y, SurfaceTopZ + 1.0)))
		{
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("voxel.Glider.Spawn: the ground ahead is underwater; a glider does not park ")
			       TEXT("on a lake. Face dry land, or use the airborne form (add an AltitudeM ")
			       TEXT("argument)."));
			return nullptr;
		}
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AVoxelGlider* G = World->SpawnActor<AVoxelGlider>(
		AVoxelGlider::StaticClass(), FVector(Probe.X, Probe.Y, SurfaceTopZ + 200.0), ViewRot, Params);
	if (!G)
	{
		UE_LOG(LogVoxelEarth, Warning, TEXT("voxel.Glider.Spawn: SpawnActor failed."));
		return nullptr;
	}
	// Settle applied POST-spawn because BellyOffsetUU is measured from the
	// asset body's bounds in BeginPlay, which SpawnActor has run by the time it
	// returns (the boat's KeelOffsetUU discipline).
	G->SettleParked(SurfaceTopZ);
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("voxel.Glider.Spawn: PARKED glider at (%.0f,%.0f,%.0f), resting on the surface. ")
	       TEXT("Walk up and press E to board it."),
	       G->GetActorLocation().X, G->GetActorLocation().Y, G->GetActorLocation().Z);
	return G;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void AVoxelGlider::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	if (!PlayerInputComponent)
	{
		return;
	}

	// The fly pawn's legacy-input idiom, with this vehicle's own mapping names --
	// VoxelFly_* only exist once a fly pawn has had its input set up, and a leg
	// that spawns straight into a glider may never have had one.
	//
	// W IS NOSE DOWN. Aircraft convention, and it is the opposite of the fly
	// pawn's W-is-forward: pushing the stick forward lowers the nose. Said here
	// because it will read as inverted to anyone who has just been flying.
	UPlayerInput::AddEngineDefinedAxisMapping(
		FInputAxisKeyMapping(TEXT("VoxelGlider_Pitch"), EKeys::S, 1.f));
	UPlayerInput::AddEngineDefinedAxisMapping(
		FInputAxisKeyMapping(TEXT("VoxelGlider_Pitch"), EKeys::W, -1.f));
	UPlayerInput::AddEngineDefinedAxisMapping(
		FInputAxisKeyMapping(TEXT("VoxelGlider_Roll"), EKeys::D, 1.f));
	UPlayerInput::AddEngineDefinedAxisMapping(
		FInputAxisKeyMapping(TEXT("VoxelGlider_Roll"), EKeys::A, -1.f));

	PlayerInputComponent->BindAxis(TEXT("VoxelGlider_Pitch"), this, &AVoxelGlider::InputPitch);
	PlayerInputComponent->BindAxis(TEXT("VoxelGlider_Roll"), this, &AVoxelGlider::InputRoll);
	PlayerInputComponent->BindAxisKey(EKeys::MouseX, this, &AVoxelGlider::InputLookYaw);
	PlayerInputComponent->BindAxisKey(EKeys::MouseY, this, &AVoxelGlider::InputLookPitch);

	// Space = launch from park (with W as the equivalent, read straight off the
	// pitch axis in the parked tick). Bound on THIS pawn's input component, so
	// it only exists while a glider is possessed -- the fly pawn's own Space
	// binding is inactive then and there is no collision. A plain action-style
	// BindKey, not an axis: digital, edge-triggered, no IsAxis1D concern.
	PlayerInputComponent->BindKey(EKeys::SpaceBar, IE_Pressed, this,
	                              &AVoxelGlider::InputLaunchPressed);
}

void AVoxelGlider::InputPitch(float Value)
{
	PitchInput = FMath::Clamp(Value, -1.f, 1.f);
}

void AVoxelGlider::InputRoll(float Value)
{
	RollInput = FMath::Clamp(Value, -1.f, 1.f);
}

void AVoxelGlider::InputLookYaw(float Value)
{
	// The camera looks around; the stick flies. Same separation as the boat's,
	// and for the same reason.
	CameraYawDeg = FMath::UnwindDegrees(CameraYawDeg + double(Value) * 2.5);
}

void AVoxelGlider::InputLookPitch(float Value)
{
	CameraPitchDeg = FMath::Clamp(CameraPitchDeg - double(Value) * 2.5, -70.0, 30.0);
}

void AVoxelGlider::InputLaunchPressed()
{
	// Only ever SET while parked: a mid-air Space press must not queue a
	// phantom relaunch for the first tick after touchdown.
	if (bParked)
	{
		bLaunchPressed = true;
	}
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

FAutoConsoleCommandWithWorld GVoxelGliderDeployCmd(
	TEXT("voxel.Glider.Deploy"),
	TEXT("Open a glider under the player, exactly as the deploy key does -- including the ")
	TEXT("airborne/clearance refusal, which prints which test failed. ")
	TEXT("voxel.Glider.DeployAnywhere 1 skips it for a leg with no altitude to spare."),
	FConsoleCommandWithWorldDelegate::CreateStatic(
		[](UWorld* World)
		{ AVoxelGlider::TryDeploy(World ? World->GetFirstPlayerController() : nullptr); }));

FAutoConsoleCommandWithWorldAndArgs GVoxelGliderSpawnCmd(
	TEXT("voxel.Glider.Spawn"),
	TEXT("voxel.Glider.Spawn [AheadM=8] [AltitudeM] -- with NO AltitudeM (the default since the ")
	TEXT("2026-09-05 respec): a PARKED glider resting on the ground ahead of the player, like ")
	TEXT("voxel.Boat.Spawn on water; walk up and press E to board it. Refuses (and says so) over ")
	TEXT("unstreamed columns and over water. WITH AltitudeM: the old AIRBORNE autonomous spawn, ")
	TEXT("ahead of and above the camera, already moving -- for photographing one in the air; it ")
	TEXT("glides until it lands, then parks on the surface."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			const double AheadM = (Args.Num() > 0) ? FCString::Atod(*Args[0]) : 8.0;
			if (Args.Num() > 1)
			{
				AVoxelGlider::SpawnAhead(World, AheadM * 100.0, FCString::Atod(*Args[1]) * 100.0);
			}
			else
			{
				AVoxelGlider::SpawnParkedAhead(World, AheadM * 100.0);
			}
		}));

FAutoConsoleCommandWithWorld GVoxelGliderBoardCmd(
	TEXT("voxel.Glider.Board"),
	TEXT("Board the nearest PARKED glider, exactly as the interact key does (E dispatches to the ")
	TEXT("nearest boardable vehicle -- see the controller for the boat-vs-glider tie rule). Exists ")
	TEXT("so an unattended leg can board with no keyboard; W/Space then launch, or ")
	TEXT("voxel.Glider.Stow packs it away."),
	FConsoleCommandWithWorldDelegate::CreateStatic(
		[](UWorld* World)
		{
			AVoxelGlider::TryBoardNearest(World ? World->GetFirstPlayerController() : nullptr);
		}));

FAutoConsoleCommandWithWorld GVoxelGliderStowCmd(
	TEXT("voxel.Glider.Stow"),
	TEXT("Stow the glider being possessed right now, wherever it is: the pilot is put back on their ")
	TEXT("feet and the glider is DESTROYED (this is the pack-it-away path -- a landed glider left ")
	TEXT("alone parks and persists instead; E dismounts without stowing)."),
	FConsoleCommandWithWorldDelegate::CreateStatic(
		[](UWorld* World)
		{
			APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
			if (AVoxelGlider* G = PC ? Cast<AVoxelGlider>(PC->GetPawn()) : nullptr)
			{
				G->ReturnPilot(G->GetActorLocation());
			}
		}));

FAutoConsoleCommandWithWorld GVoxelGliderStatCmd(
	TEXT("voxel.Glider.Stat"),
	TEXT("One line per glider. ticks vs aero is the engagement proof and the two are separate on ")
	TEXT("purpose: a glider integrating gravity with the aero faded out is a ROCK, and a rock and a ")
	TEXT("wing produce the same line if only one counter exists. ground=0 with ticks>0 means every ")
	TEXT("terrain probe missed -- unstreamed columns, not clear air. parked/crewed and park (the ")
	TEXT("parked-tick counter) are the persistence proof: a landed glider shows parked=1 with park ")
	TEXT("climbing, and a vanished one shows nothing at all."),
	FConsoleCommandWithWorldDelegate::CreateStatic(
		[](UWorld* World)
		{
			if (!World)
			{
				return;
			}
			int32 N = 0;
			for (TActorIterator<AVoxelGlider> It(World); It; ++It)
			{
				const AVoxelGlider* G = *It;
				const FVector L = G->GetActorLocation();
				UE_LOG(LogVoxelEarth, Log,
				       TEXT("Glider[%d] at (%.0f,%.0f,%.0f) parked=%d crewed=%d ticks=%llu aero=%llu ")
				       TEXT("ground=%llu flare=%llu park=%llu airspeed=%.1f m/s alpha=%.1f deg ")
				       TEXT("L/D=%.2f wind=%.1f m/s visual=%s(%d inst)"),
				       N++, L.X, L.Y, L.Z, G->IsParked() ? 1 : 0, G->IsCrewed() ? 1 : 0,
				       (unsigned long long)G->GetTicks(),
				       (unsigned long long)G->GetAeroTicks(), (unsigned long long)G->GetGroundHits(),
				       (unsigned long long)G->GetFlareTicks(), (unsigned long long)G->GetParkTicks(),
				       G->GetLastAirspeedMS(),
				       G->GetLastAlphaDeg(), G->GetLastGlideRatio(), G->GetLastWindMS(),
				       (G->GetBody() && !G->GetBody()->IsPlaceholder()) ? TEXT("asset")
				                                                        : TEXT("PLACEHOLDER"),
				       G->GetBody() ? G->GetBody()->GetInstanceCount() : 0);
			}
			if (N == 0)
			{
				UE_LOG(LogVoxelEarth, Log, TEXT("voxel.Glider.Stat: no gliders in this world."));
			}
		}));
