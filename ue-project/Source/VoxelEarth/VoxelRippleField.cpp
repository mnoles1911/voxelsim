#include "VoxelRippleField.h"

#include "VoxelSkySubsystem.h" // VoxelSky::kSkyCollectionPath

#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h" // TActorIterator, for the auto-watcher
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"

#include "VoxelDebris.h"          // AVoxelDebris  -- one of the two watched classes
#include "VoxelDebug.h"           // LogVoxelWater
#include "VoxelExplosive.h"       // AVoxelExplosive -- the thing the player throws
#include "VoxelWaterSubsystem.h"  // IsUnderwaterAtWorld, the only query the watcher needs

namespace
{
// The five assets this subsystem owns at runtime, all authored by
// ue-project/Tools/create_ripple_field_materials.py, which is also where the
// size/format contract the guards below enforce is written down.
const TCHAR* kStateAPath = TEXT("/Game/Voxel/RT_VoxelRippleStateA.RT_VoxelRippleStateA");
const TCHAR* kStateBPath = TEXT("/Game/Voxel/RT_VoxelRippleStateB.RT_VoxelRippleStateB");
const TCHAR* kFieldPath = TEXT("/Game/Voxel/RT_VoxelRippleField.RT_VoxelRippleField");
const TCHAR* kStepMaterialPath = TEXT("/Game/Voxel/M_VoxelRippleStep.M_VoxelRippleStep");
const TCHAR* kDeriveMaterialPath = TEXT("/Game/Voxel/M_VoxelRippleDerive.M_VoxelRippleDerive");

// The project's existing CPU->material channel. See VoxelBathyField.cpp:26-30 --
// this collection is DELETED AND RECREATED by create_sky_material.py on every
// run, so a name that script does not know about makes the Set* calls below log
// a warning and do nothing. That is the silent-no-op trap, and it is why
// Initialize checks membership once and then refuses to write at all.
// The collection's path is VoxelSky::kSkyCollectionPath (VoxelSkySubsystem.h)
// -- one definition for the whole module; per-file copies collided in a unity
// blob of the game target.

const TCHAR* kRippleParamOrigin = TEXT("RippleFieldOrigin");
const TCHAR* kRippleParamInvSize = TEXT("RippleFieldInvSize");
const TCHAR* kRippleParamGain = TEXT("RippleFieldGain");

// ---------------------------------------------------------------------------
// CONSOLE VARIABLES
// ---------------------------------------------------------------------------

TAutoConsoleVariable<bool> CVarVoxelWaterRippleEnable(
	TEXT("voxel.Water.Ripple.Enable"), true,
	TEXT("Local interactive water ripples: a 51.2 m camera-following height field simulated on ")
	TEXT("the GPU, added into the water surface's normal and displacement. 0 publishes a gain of ")
	TEXT("zero and clears the field, which restores EXACTLY the water this project rendered ")
	TEXT("before the feature existed -- it is the A arm of any A/B on it."),
	ECVF_Default);

// ---------------------------------------------------------------------------
// THE DEFAULTS BELOW WERE RAISED ON 2026-08-14 BECAUSE THE FEATURE WAS INVISIBLE
// IN PLAY WHILE BEING COMPLETELY CORRECT.
//
// The owner tested every ripple command, threw voxel cubes into a lake and flew
// into it himself, and saw nothing on any path. Six separate measurements said
// the system was healthy, and they were all right: a CPU readback of the render
// targets (voxel.Water.Ripple.Probe, written for this) showed the splat landing
// -- state 0.5 -> 1.165 -- the derive pass producing gradients up to 2.13, and
// the field texture holding a ring over 1% of its area. A debug build routing
// that field straight to emissive rendered it plainly: 34,854 red-dominant
// pixels against ZERO in an otherwise identical frame where the drop had been
// placed outside the view.
//
// So nothing was broken. Three things made it unnoticeable, and all three are
// numbers:
//   * A 1.8 s HALF-LIFE. Fly in, then look down, and it is already 70% gone.
//   * STRENGTHS SIZED AGAINST FLAT WATER. 9 cm of player entry competes with a
//     6 cm wind-wave field on the same normal -- and the wind waves are moving,
//     which wins the eye every time.
//   * A/B ON THE SHIPPING MATERIAL measured the ring at a mean 3.59/255 delta:
//     really there, and about as visible as a compression artefact.
//
// These are a starting point for judgement, not a measurement. The whole
// apparatus for re-judging them is in place now: voxel.Water.Ripple.Probe reads
// the data, VOXEL_WATER_RIPPLE_DEBUG=1 renders the field directly, and
// -VoxelExecAfter runs a drop once the world is actually up.
// ---------------------------------------------------------------------------
TAutoConsoleVariable<float> CVarVoxelWaterRippleGain(
	TEXT("voxel.Water.Ripple.Gain"), 2.5f,
	TEXT("Global multiplier on the ripple field as the water material reads it. 1 is shipped ")
	TEXT("strength. This scales what is DRAWN, not what is simulated, so turning it down and ")
	TEXT("back up does not disturb a settling ring -- which is what makes it the right knob for ")
	TEXT("an A/B at a pinned pose."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarVoxelWaterRippleSpeed(
	TEXT("voxel.Water.Ripple.SpeedMPS"), 1.6f,
	TEXT("Ripple propagation speed in metres per second. 1.6 is roughly right for the ")
	TEXT("30-60 cm gravity-capillary waves a splash makes; a ring crosses 5 m in about 3 s. ")
	TEXT("CLAMPED so the Courant number stays under 0.6 against a 2D stability limit of 0.7071 ")
	TEXT("-- at the fixed 1/60 s timestep and 10 cm cells that ceiling is 3.6 m/s, and the clamp ")
	TEXT("says so in the log the first time it engages."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarVoxelWaterRippleHalfLife(
	TEXT("voxel.Water.Ripple.HalfLifeSec"), 5.0f,
	TEXT("Seconds for a ripple's amplitude to halve. 1.8 puts a splash at ~10% after 6 s, and ")
	TEXT("since 2026-08-12 that is the REALISED figure rather than the intended one -- the step ")
	TEXT("material damped only one of the two time levels, which made every configured half-life ")
	TEXT("come out at twice its value (1.8 s ran at 3.6 s, ~35% left at 6 s). Expressed as a ")
	TEXT("half-life rather than as a per-step factor ON PURPOSE: the per-step factor is ")
	TEXT("2^(-dt/halflife), so a change to the substep rate cannot silently change how long ")
	TEXT("ripples last."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelWaterRippleMaxSubsteps(
	TEXT("voxel.Water.Ripple.MaxSubsteps"), 4,
	TEXT("Most simulation steps one frame may run. 4 at 1/60 s absorbs a 66 ms frame; beyond ")
	TEXT("that the backlog is DROPPED rather than caught up, because catching up after a hitch ")
	TEXT("is how a fixed-step loop turns one long frame into several. Ripples run briefly slow ")
	TEXT("after a stall, which is invisible."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelWaterRippleFreeze(
	TEXT("voxel.Water.Ripple.Freeze"), false,
	TEXT("Stop advancing the simulation while continuing to publish the field. For CAPTURES: ")
	TEXT("inject a drop, run a fixed number of steps with voxel.Water.Ripple.Drop's last ")
	TEXT("argument, freeze, and every screenshot from then on photographs the same water. ")
	TEXT("voxel.Water.Ripple.Drop still works while frozen."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelWaterRippleMaskEnable(
	TEXT("voxel.Water.Ripple.MaskEnable"), true,
	TEXT("Mask the simulation by the baked shoreline distance so ripples exist only in water. ")
	TEXT("Set 0 to let them propagate everywhere. THAT IS THE DIAGNOSTIC FOR 'my test drop did ")
	TEXT("nothing': a drop on a spot the bake calls dry is silently deleted, and this separates ")
	TEXT("that from the field not running at all."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelWaterRippleAutoWatch(
	TEXT("voxel.Water.Ripple.AutoWatch"), true,
	TEXT("Watch the player pawn, thrown explosives and voxel debris for water entry and inject a ")
	TEXT("ripple on the crossing, WITHOUT any edit to those classes. COSTS one pass over the ")
	TEXT("world's actor list per frame plus one water query per watched actor -- the pass is the ")
	TEXT("bigger half and this text used to mention only the query. Its strength estimate is NOT ")
	TEXT("frame-rate independent (it divides a position delta by the frame time), so it is ")
	TEXT("suppressed entirely while voxel.Water.Ripple.Freeze is on and must not be relied on for ")
	TEXT("a capture; use voxel.Water.Ripple.Drop for those. The exact hooks inside those classes ")
	TEXT("are better -- they know the impact speed on the frame it happens -- and are written up ")
	TEXT("as patch notes in docs/water-interactive-ripples.md."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarVoxelWaterRipplePlayerStrength(
	TEXT("voxel.Water.Ripple.PlayerStrengthM"), 0.22f,
	TEXT("Ripple height in metres for a player entering water at full speed, scaled down for a ")
	TEXT("slow entry. 9 cm is already a big splash: the ambient wind wave on this water is ")
	TEXT("8.6 cm crest-to-mean (create_water_voxel_material.py:2356-2382)."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarVoxelWaterRipplePlayerRadius(
	TEXT("voxel.Water.Ripple.PlayerRadiusM"), 0.9f,
	TEXT("Initial radius in metres of the ring a player makes entering water. Roughly a body's ")
	TEXT("width; the ring expands from there at voxel.Water.Ripple.SpeedMPS."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarVoxelWaterRippleObjectStrength(
	TEXT("voxel.Water.Ripple.ObjectStrengthM"), 0.18f,
	TEXT("Ripple height in metres for a thrown voxel volume or a lump of debris hitting water at ")
	TEXT("full speed. Its RADIUS comes from the object's own bounds, not from here."),
	ECVF_Default);

// The impact-strength curve's two constants live in VoxelRippleField.h as
// VoxelRipple::kFullImpactSpeedMPS and VoxelRipple::kMinImpactFraction, shared with
// VoxelThrownItem.cpp -- see the header for why they moved.

UVoxelRippleFieldSubsystem* FindRippleSubsystem(UWorld* World)
{
	return World ? World->GetSubsystem<UVoxelRippleFieldSubsystem>() : nullptr;
}

// ---------------------------------------------------------------------------
// CONSOLE COMMANDS -- and the reason the first one exists at all
// ---------------------------------------------------------------------------
//
// Verification on this project is by screenshot at pinned poses, and the owner
// judges the screenshot (docs/water-architecture.md §4). A ripple that only
// appears when a player jumps into a lake cannot be photographed by a headless
// capture at all: there is no player, and there is no frame at which the effect
// is reproducibly the same. `Drop` with its Steps argument is what makes it
// photographable -- inject at a FIXED world position, advance a FIXED number of
// steps, and every capture from that pose shows the same rings.

FAutoConsoleCommandWithWorldAndArgs GVoxelWaterRippleDropCmd(
	TEXT("voxel.Water.Ripple.Drop"),
	TEXT("voxel.Water.Ripple.Drop <XUU> <YUU> [RadiusM=0.5] [StrengthM=0.09] [Steps=0] -- inject ")
	TEXT("one deterministic ripple at a world position (UE UNITS, the same numbers ")
	TEXT("GetActorLocation prints). Steps>0 advances the simulation that many fixed 1/60 s steps ")
	TEXT("immediately, so the field is in an exactly reproducible state for the next frame's ")
	TEXT("screenshot; follow it with voxel.Water.Ripple.Freeze 1 to hold it there. The drop is ")
	TEXT("dropped if it lands outside the 51.2 m window or on ground the bake calls dry -- ")
	TEXT("voxel.Water.Ripple.Stat counts both, and voxel.Water.Ripple.MaskEnable 0 removes the ")
	TEXT("second cause."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			UVoxelRippleFieldSubsystem* Ripple = FindRippleSubsystem(World);
			if (!Ripple)
			{
				UE_LOG(LogVoxelWater, Warning,
				       TEXT("Ripple.Drop: no UVoxelRippleFieldSubsystem in this world."));
				return;
			}
			if (Args.Num() < 2)
			{
				UE_LOG(LogVoxelWater, Warning,
				       TEXT("Ripple.Drop: usage is voxel.Water.Ripple.Drop <XUU> <YUU> ")
				       TEXT("[RadiusM] [StrengthM] [Steps]"));
				return;
			}
			const double X = FCString::Atod(*Args[0]);
			const double Y = FCString::Atod(*Args[1]);
			const float RadiusM = (Args.Num() > 2) ? FCString::Atof(*Args[2]) : 0.5f;
			const float StrengthM = (Args.Num() > 3) ? FCString::Atof(*Args[3]) : 0.09f;
			const int32 Steps = (Args.Num() > 4) ? FMath::Clamp(FCString::Atoi(*Args[4]), 0, 4096) : 0;

			Ripple->AddDisturbance(FVector(X, Y, 0.0), RadiusM, StrengthM);
			if (Steps > 0)
			{
				Ripple->RunSteps(Steps);
			}
			UE_LOG(LogVoxelWater, Log,
			       TEXT("Ripple.Drop: (%.0f, %.0f) UU r=%.2f m s=%.3f m, then %d step(s) ")
			       TEXT("(%.2f s of simulated time). Window origin now (%.0f, %.0f) UU."),
			       X, Y, RadiusM, StrengthM, Steps,
			       Steps * UVoxelRippleFieldSubsystem::kFixedDt,
			       Ripple->WindowOriginUU().X, Ripple->WindowOriginUU().Y);
		}));

FAutoConsoleCommandWithWorldAndArgs GVoxelWaterRippleDropHereCmd(
	TEXT("voxel.Water.Ripple.DropHere"),
	TEXT("voxel.Water.Ripple.DropHere [RadiusM=0.5] [StrengthM=0.09] [Steps=0] -- the same thing ")
	TEXT("at the player pawn's own position. Convenient interactively; NOT reproducible, so use ")
	TEXT("voxel.Water.Ripple.Drop for anything that will be photographed."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			UVoxelRippleFieldSubsystem* Ripple = FindRippleSubsystem(World);
			APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
			APawn* Pawn = PC ? PC->GetPawn() : nullptr;
			if (!Ripple || !Pawn)
			{
				UE_LOG(LogVoxelWater, Warning,
				       TEXT("Ripple.DropHere: no subsystem or no pawn."));
				return;
			}
			const float RadiusM = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 0.5f;
			const float StrengthM = (Args.Num() > 1) ? FCString::Atof(*Args[1]) : 0.09f;
			const int32 Steps = (Args.Num() > 2) ? FMath::Clamp(FCString::Atoi(*Args[2]), 0, 4096) : 0;
			const FVector P = Pawn->GetActorLocation();
			Ripple->AddDisturbance(P, RadiusM, StrengthM);
			if (Steps > 0)
			{
				Ripple->RunSteps(Steps);
			}
			UE_LOG(LogVoxelWater, Log, TEXT("Ripple.DropHere: (%.0f, %.0f) UU, %d step(s)."),
			       P.X, P.Y, Steps);
		}));

// READ THE TEXTURES BACK AND SAY WHAT IS IN THEM.
//
// WHY THIS EXISTS. On 2026-08-13 the ripple system reported perfect health and
// rendered nothing, through every path -- the console drop, the player entering
// water, and a thrown cube. Everything checkable was checked and all of it
// passed: `armed=1 published=1 steps=26585 injected=4 dropped(outside=0 full=0
// unarmed=0 inert=0)`, the derive shader correctly removing the storage bias,
// M_WaterVoxel's package naming RT_VoxelRippleField and all three RippleField*
// collection parameters, the field render target being RGBA16F so signed
// gradients survive, the sheet's vertex colour B being 255 so the top-face mask
// cannot be zeroing it, and not one warning in the log.
//
// Every one of those is a check on the PROCESS. None of them looks at the DATA.
// `injected` in particular counts calls that passed validation and were written
// into a splat slot -- it is not evidence that the shader ever used them.
//
// So this reads the pixels. It is the same move as the pinned-pose screenshot
// that settles material regressions on this project, one layer down and without
// needing anybody's eyes.
//
// HOW TO READ THE RESULT, which is the point of printing all three:
//   state non-zero, field non-zero -> the data is there and the fault is in the
//       water material: the UV mapping, or a contribution too small to see.
//   state non-zero, field ZERO     -> the derive pass is not writing.
//   state ZERO                     -> the step pass is not applying the splats,
//       and `injected` was measuring the wrong thing.
void ProbeRenderTarget(UTextureRenderTarget2D* RT, const TCHAR* Name)
{
	if (RT == nullptr)
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("Ripple.Probe: %s is NULL."), Name);
		return;
	}
	FTextureRenderTargetResource* Res = RT->GameThread_GetRenderTargetResource();
	if (Res == nullptr)
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("Ripple.Probe: %s has no render resource."), Name);
		return;
	}

	TArray<FLinearColor> Pixels;
	if (!Res->ReadLinearColorPixels(Pixels) || Pixels.Num() == 0)
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("Ripple.Probe: %s read back %d pixel(s) -- the readback itself failed, so this "
		            "says nothing about the contents."),
		       Name, Pixels.Num());
		return;
	}

	double MinR = 1e30, MaxR = -1e30, MinG = 1e30, MaxG = -1e30, MinB = 1e30, MaxB = -1e30;
	double SumAbs = 0.0;
	int32 NonZero = 0;
	for (const FLinearColor& C : Pixels)
	{
		MinR = FMath::Min(MinR, (double)C.R); MaxR = FMath::Max(MaxR, (double)C.R);
		MinG = FMath::Min(MinG, (double)C.G); MaxG = FMath::Max(MaxG, (double)C.G);
		MinB = FMath::Min(MinB, (double)C.B); MaxB = FMath::Max(MaxB, (double)C.B);
		const double A = FMath::Abs((double)C.R) + FMath::Abs((double)C.G) + FMath::Abs((double)C.B);
		SumAbs += A;
		if (A > 1e-6) { ++NonZero; }
	}
	UE_LOG(LogVoxelWater, Log,
	       TEXT("Ripple.Probe: %s %dx%d  R[%.5f..%.5f] G[%.5f..%.5f] B[%.5f..%.5f]  "
	            "mean|rgb|=%.6f  nonzero=%d/%d (%.1f%%)"),
	       Name, RT->SizeX, RT->SizeY, MinR, MaxR, MinG, MaxG, MinB, MaxB,
	       SumAbs / (double)Pixels.Num(), NonZero, Pixels.Num(),
	       100.0 * (double)NonZero / (double)Pixels.Num());
}

FAutoConsoleCommandWithWorldAndArgs GVoxelWaterRippleProbeCmd(
	TEXT("voxel.Water.Ripple.Probe"),
	TEXT("Read the ripple render targets back to the CPU and print their min/max/mean per channel. "
	     "THE INSTRUMENT FOR 'everything reports healthy and nothing renders': the counters and the "
	     "asset checks all measure the process, this measures the DATA. state non-zero + field "
	     "non-zero means the fault is in the water material; state non-zero + field zero means the "
	     "derive pass is not writing; state zero means the step pass never applied the splats."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& /*Args*/, UWorld* World)
		{
			UVoxelRippleFieldSubsystem* Ripple = FindRippleSubsystem(World);
			if (Ripple == nullptr)
			{
				UE_LOG(LogVoxelWater, Warning, TEXT("Ripple.Probe: no ripple subsystem in this world."));
				return;
			}
			Ripple->ProbeTextures();
		}));

FAutoConsoleCommandWithWorldAndArgs GVoxelWaterRippleStatCmd(
	TEXT("voxel.Water.Ripple.Stat"),
	TEXT("Dump the ripple field's counters. TotalSteps at 0 with water on screen is the whole ")
	TEXT("diagnosis; the four dropped counters separate the four reasons a disturbance came to ")
	TEXT("nothing -- unarmed (feature off or assets missing), inert (the caller passed strength 0 ")
	TEXT("or a non-finite number), outside (not within 25.6 m of the camera) and full (the queue ")
	TEXT("overflowed, which is a bug). injected + those four is every call AddDisturbance has ")
	TEXT("ever had; if it is not, a path out of that function has stopped counting."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& /*Args*/, UWorld* World)
		{
			UVoxelRippleFieldSubsystem* Ripple = FindRippleSubsystem(World);
			if (!Ripple)
			{
				UE_LOG(LogVoxelWater, Warning, TEXT("Ripple.Stat: no subsystem in this world."));
				return;
			}
			UE_LOG(LogVoxelWater, Log,
			       TEXT("Ripple: armed=%d published=%d steps=%llu injected=%llu ")
			       TEXT("dropped(outside=%llu full=%llu unarmed=%llu inert=%llu) ")
			       TEXT("origin=(%.0f, %.0f) UU window=%.1f m"),
			       Ripple->IsArmed() ? 1 : 0, Ripple->HasPublished() ? 1 : 0,
			       static_cast<unsigned long long>(Ripple->TotalSteps()),
			       static_cast<unsigned long long>(Ripple->DisturbancesInjected()),
			       static_cast<unsigned long long>(Ripple->DisturbancesDroppedOutside()),
			       static_cast<unsigned long long>(Ripple->DisturbancesDroppedFull()),
			       static_cast<unsigned long long>(Ripple->DisturbancesDroppedUnarmed()),
			       static_cast<unsigned long long>(Ripple->DisturbancesDroppedInert()),
			       Ripple->WindowOriginUU().X, Ripple->WindowOriginUU().Y,
			       UVoxelRippleFieldSubsystem::kWindowUU / 100.0);
		}));
} // namespace

// ============================================================================

void UVoxelRippleFieldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// NO InitializeDependency, and that is deliberate rather than an omission.
	// The only other subsystem this one reads is UVoxelWaterSubsystem, and only
	// from Tick, by which time every subsystem exists. Declaring a dependency
	// would make a purely cosmetic effect part of water's initialisation order,
	// which is exactly the kind of coupling that turns "the ripples are broken"
	// into "the lakes are broken".

	UTextureRenderTarget2D* A = LoadObject<UTextureRenderTarget2D>(nullptr, kStateAPath);
	UTextureRenderTarget2D* B = LoadObject<UTextureRenderTarget2D>(nullptr, kStateBPath);
	UTextureRenderTarget2D* F = LoadObject<UTextureRenderTarget2D>(nullptr, kFieldPath);
	UMaterialInterface* StepMat = LoadObject<UMaterialInterface>(nullptr, kStepMaterialPath);
	UMaterialInterface* DeriveMat = LoadObject<UMaterialInterface>(nullptr, kDeriveMaterialPath);

	if (!A || !B || !F || !StepMat || !DeriveMat)
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("RippleField: assets missing (stateA=%d stateB=%d field=%d step=%d derive=%d), ")
		       TEXT("so there are no interactive ripples this run. Water renders exactly as it did ")
		       TEXT("before the feature existed. Run ")
		       TEXT("ue-project/Tools/create_ripple_field_materials.py."),
		       A ? 1 : 0, B ? 1 : 0, F ? 1 : 0, StepMat ? 1 : 0, DeriveMat ? 1 : 0);
		return;
	}

	// THE GUARD. Same shape and same reasoning as VoxelBathyField.cpp:66-88: the
	// shader's neighbour offsets, the scroll's texel snap and the world<->UV
	// mapping are all derived from kSize, so a render target of another size does
	// not degrade -- it simulates a different world than the one it publishes.
	// Refusing is always the right answer, because "no field" is a supported
	// state and a wrong field is not.
	auto CheckTarget = [](const UTextureRenderTarget2D* RT, const TCHAR* Path,
	                      ETextureRenderTargetFormat Want, const TCHAR* WantName) -> bool
	{
		if (RT->SizeX != kSize || RT->SizeY != kSize || RT->RenderTargetFormat != Want)
		{
			UE_LOG(LogVoxelWater, Error,
			       TEXT("RippleField: %s is %dx%d format %d; this subsystem requires %dx%d %s and ")
			       TEXT("will NOT run against anything else. Interactive ripples are disabled for ")
			       TEXT("this run. Re-run ue-project/Tools/create_ripple_field_materials.py."),
			       Path, RT->SizeX, RT->SizeY, static_cast<int32>(RT->RenderTargetFormat),
			       kSize, kSize, WantName);
			return false;
		}
		return true;
	};
	if (!CheckTarget(A, kStateAPath, RTF_RG32f, TEXT("RG32f"))
	    || !CheckTarget(B, kStateBPath, RTF_RG32f, TEXT("RG32f"))
	    || !CheckTarget(F, kFieldPath, RTF_RGBA16f, TEXT("RGBA16f")))
	{
		return;
	}

	StateA_ = A;
	StateB_ = B;
	Field_ = F;
	StepMid_ = UMaterialInstanceDynamic::Create(StepMat, this);
	DeriveMid_ = UMaterialInstanceDynamic::Create(DeriveMat, this);
	if (!StepMid_ || !DeriveMid_)
	{
		UE_LOG(LogVoxelWater, Error,
		       TEXT("RippleField: could not create dynamic material instances; ripples disabled."));
		StateA_ = nullptr;
		StateB_ = nullptr;
		Field_ = nullptr;
		return;
	}

	// Resolution to the shader, once. kSize is the authority; the material's
	// default is only what makes the editor preview behave.
	StepMid_->SetScalarParameterValue(TEXT("TexelUV"), 1.0f / static_cast<float>(kSize));
	StepMid_->SetScalarParameterValue(TEXT("WindowSizeUU"), static_cast<float>(kWindowUU));
	StepMid_->SetScalarParameterValue(TEXT("ShoreMaskM"), static_cast<float>(kShoreMaskM));
	DeriveMid_->SetScalarParameterValue(TEXT("TexelUV"), 1.0f / static_cast<float>(kSize));
	DeriveMid_->SetScalarParameterValue(TEXT("TexelM"), static_cast<float>(kTexelUU / 100.0));

	// --- CAN WE EVEN PUBLISH? ------------------------------------------------
	//
	// Checked ONCE, here. UKismetMaterialLibrary's setters log a warning and do
	// nothing for a parameter that is not on the collection; called three times a
	// frame that would be 180 lines a second of warning and would bury every
	// other diagnostic in the run. The honest failure is one line saying which
	// patch has not been applied.
	SkyCollection_ = LoadObject<UMaterialParameterCollection>(nullptr, VoxelSky::kSkyCollectionPath);
	if (const UMaterialParameterCollection* Sky = SkyCollection_)
	{
		bool bHasOrigin = false, bHasInvSize = false, bHasGain = false;
		for (const FCollectionVectorParameter& P : Sky->VectorParameters)
		{
			bHasOrigin |= (P.ParameterName == FName(kRippleParamOrigin));
		}
		for (const FCollectionScalarParameter& P : Sky->ScalarParameters)
		{
			bHasInvSize |= (P.ParameterName == FName(kRippleParamInvSize));
			bHasGain |= (P.ParameterName == FName(kRippleParamGain));
		}
		bMpcHasParams_ = bHasOrigin && bHasInvSize && bHasGain;
	}
	if (!bMpcHasParams_)
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("RippleField: MPC_VoxelSky has no %s/%s/%s, so nothing can read the field and ")
		       TEXT("the water will look exactly as it does today. The simulation still runs (the ")
		       TEXT("render targets are live and inspectable in the texture visualiser). Apply the ")
		       TEXT("two patches in docs/water-interactive-ripples.md -- create_sky_material.py's ")
		       TEXT("parameter table and M_WaterVoxel's wave block -- then regenerate sky, dome, ")
		       TEXT("water IN THAT ORDER."),
		       kRippleParamOrigin, kRippleParamInvSize, kRippleParamGain);
	}

	Pending_.Reserve(kMaxPending);
	bArmed_ = true;
	ClearState();

	UE_LOG(LogVoxelWater, Log,
	       TEXT("RippleField: armed. %dx%d texels at %.2f m -> a %.1f m window (+/-%.1f m). ")
	       TEXT("Fixed dt %.4f s, speed %.2f m/s, Courant %.4f (limit %.4f), half-life %.2f s."),
	       kSize, kSize, kTexelUU / 100.0, kWindowUU / 100.0, kWindowUU / 200.0,
	       kFixedDt, CVarVoxelWaterRippleSpeed.GetValueOnGameThread(),
	       CVarVoxelWaterRippleSpeed.GetValueOnGameThread() * kFixedDt / (kTexelUU / 100.0),
	       kCourantLimit, CVarVoxelWaterRippleHalfLife.GetValueOnGameThread());
}

void UVoxelRippleFieldSubsystem::Deinitialize()
{
	// A field published over a world that is going away is worse than none: the
	// render targets are /Game assets and outlive this subsystem, so without this
	// the next world would start with the previous world's ripples at the
	// previous world's origin. Same argument as VoxelBathyField.cpp:106-111.
	PublishWindow(0.0f);
	bArmed_ = false;
	bPublished_ = false;
	StateA_ = nullptr;
	StateB_ = nullptr;
	Field_ = nullptr;
	StepMid_ = nullptr;
	DeriveMid_ = nullptr;
	SkyCollection_ = nullptr;
	Pending_.Empty();
	Watched_.Empty();
	Super::Deinitialize();
}

bool UVoxelRippleFieldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Game and PIE only, the same scope VoxelBathyField.cpp:121-127 takes. An
	// editor-preview world has no camera to follow, and publishing into the
	// shared render targets from one would fight with the game world that is also
	// publishing into them. DoesSupportWorldType and NOT ShouldCreateSubsystem:
	// nothing in this module uses the latter, and two gates with different
	// semantics is how the two answers drift (VoxelSkySubsystem.cpp:2004-2010).
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

bool UVoxelRippleFieldSubsystem::IsTickable() const
{
	// Super:: first, so the base's not-a-template/initialised gate is preserved
	// rather than reimplemented. bPublished_ is what keeps us ticking for exactly
	// one more frame after a runtime disable -- long enough to publish the gain
	// back to zero and clear the field. Without it, switching the CVar off would
	// freeze the last frame's ripples into the water forever.
	return Super::IsTickable()
	       && bArmed_
	       && (CVarVoxelWaterRippleEnable.GetValueOnGameThread() || bPublished_);
}

TStatId UVoxelRippleFieldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelRippleFieldSubsystem, STATGROUP_Tickables);
}

UTextureRenderTarget2D* UVoxelRippleFieldSubsystem::Front() const
{
	return bFrontIsA_ ? StateA_ : StateB_;
}

UTextureRenderTarget2D* UVoxelRippleFieldSubsystem::Back() const
{
	return bFrontIsA_ ? StateB_ : StateA_;
}

FVector2D UVoxelRippleFieldSubsystem::WindowOriginUU() const
{
	return FVector2D(static_cast<double>(OriginPx_) * kTexelUU,
	                 static_cast<double>(OriginPy_) * kTexelUU);
}

bool UVoxelRippleFieldSubsystem::GetCameraXY(double& OutX, double& OutY) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return false;
	}
	const FVector Loc = Pawn->GetActorLocation();
	OutX = Loc.X;
	OutY = Loc.Y;
	return true;
}

void UVoxelRippleFieldSubsystem::ClearState()
{
	UWorld* World = GetWorld();
	if (!World || !StateA_ || !StateB_ || !Field_)
	{
		return;
	}
	// (bias, bias) AND NOT BLACK. Clearing to zero would set h = -kStateBias
	// everywhere -- the entire lake half a metre below itself, released at rest,
	// which is the largest disturbance this simulation can express and would
	// detonate on the first step. The Python side clears to the same colour for
	// the same reason (create_ripple_field_materials.py, main()).
	const FLinearColor Flat(kStateBias, kStateBias, 0.0f, 0.0f);
	UKismetRenderingLibrary::ClearRenderTarget2D(World, StateA_, Flat);
	UKismetRenderingLibrary::ClearRenderTarget2D(World, StateB_, Flat);
	UKismetRenderingLibrary::ClearRenderTarget2D(World, Field_, FLinearColor::Transparent);
	bFrontIsA_ = true;
}

void UVoxelRippleFieldSubsystem::AddDisturbance(const FVector& WorldPos, float RadiusM,
                                                float StrengthM)
{
	// EVERY EXIT FROM THIS FUNCTION COUNTS. The header's block above
	// AddDisturbance is the contract and it says why; the short version is that
	// these two early-outs used to return in silence, so "the hooks fire and the
	// field is off" and "nothing ever calls AddDisturbance" produced the same
	// output from voxel.Water.Ripple.Stat -- an absent-stat zero of exactly the
	// kind docs/water-architecture.md §4 records three false conclusions from.
	if (!bArmed_)
	{
		++DroppedUnarmed_;
		return;
	}
	// Zero strength, and non-finite anything, are the same diagnosis (the caller
	// asked for nothing usable) and share a counter. The finiteness half is not
	// theoretical: an infinity here reaches the splat slot, the shader's raised
	// cosine multiplies it, and the poison guard in STEP_CODE then blanks every
	// texel it touched to flat water -- a NaN splash deletes a disc of ripples
	// instead of making one. Cheaper and clearer to refuse it at the door, where
	// the counter can say which caller's arithmetic went wrong.
	if (!FMath::IsFinite(StrengthM) || StrengthM == 0.0f || !FMath::IsFinite(RadiusM)
	    || WorldPos.ContainsNaN())
	{
		++DroppedInert_;
		return;
	}
	if (Pending_.Num() >= kMaxPending)
	{
		++DroppedFull_;
		return;
	}

	double R = static_cast<double>(RadiusM);
	if (R < kMinDisturbanceRadiusM)
	{
		// Widened rather than refused. A caller asking for a 5 cm ring wants a
		// small splash, not nothing, and half a texel of ring is not a smaller
		// splash -- it is grid noise radiating from a point. Said once so a
		// caller that is systematically wrong is visible without being noisy.
		if (!bLoggedRadiusClamp_)
		{
			bLoggedRadiusClamp_ = true;
			UE_LOG(LogVoxelWater, Log,
			       TEXT("RippleField: a disturbance radius of %.3f m was widened to %.3f m. Below ")
			       TEXT("about three texels (%.2f m) a raised-cosine bump is mostly the 10 cm grid, ")
			       TEXT("and what radiates from it is checkerboard noise rather than a ring. Said ")
			       TEXT("once."),
			       R, kMinDisturbanceRadiusM, 3.0 * kTexelUU / 100.0);
		}
		R = kMinDisturbanceRadiusM;
	}

	FPendingDisturbance& D = Pending_.AddDefaulted_GetRef();
	D.WorldX = WorldPos.X;
	D.WorldY = WorldPos.Y;
	D.RadiusM = R;
	D.StrengthM = static_cast<double>(StrengthM);
}

void UVoxelRippleFieldSubsystem::AddDisturbanceAt(const UWorld* World, const FVector& WorldPos,
                                                  float RadiusM, float StrengthM)
{
	if (!World)
	{
		return;
	}
	// const_cast rather than taking a non-const UWorld*, so that a caller holding
	// a const world -- which several of the hook sites in
	// docs/water-interactive-ripples.md do -- needs no ceremony at the call site.
	// Fetching a subsystem does not modify the world.
	if (UVoxelRippleFieldSubsystem* Ripple =
	        const_cast<UWorld*>(World)->GetSubsystem<UVoxelRippleFieldSubsystem>())
	{
		Ripple->AddDisturbance(WorldPos, RadiusM, StrengthM);
	}
}

void UVoxelRippleFieldSubsystem::FillSplatSlots()
{
	if (!StepMid_)
	{
		return;
	}
	// FNames built once. Splat0..Splat7 must match kSplatSlots and the material's
	// parameter names, which ripple_field_graph.STEP_SPLAT_SLOTS generates.
	static const FName SlotNames[kSplatSlots] = {
		FName(TEXT("Splat0")), FName(TEXT("Splat1")), FName(TEXT("Splat2")), FName(TEXT("Splat3")),
		FName(TEXT("Splat4")), FName(TEXT("Splat5")), FName(TEXT("Splat6")), FName(TEXT("Splat7")),
	};

	const double OriginXUU = static_cast<double>(OriginPx_) * kTexelUU;
	const double OriginYUU = static_cast<double>(OriginPy_) * kTexelUU;

	int32 Filled = 0;
	while (Filled < kSplatSlots && Pending_.Num() > 0)
	{
		const FPendingDisturbance D = Pending_[0];
		Pending_.RemoveAt(0, 1, EAllowShrinking::No);

		// UV in the window we are about to write, which is the window AFTER this
		// frame's scroll -- the step's output texels are in the new window, so
		// this is the frame in which the disturbance's world position and the
		// field's placement agree.
		const double U = (D.WorldX - OriginXUU) / kWindowUU;
		const double V = (D.WorldY - OriginYUU) / kWindowUU;
		const double RadiusUV = (D.RadiusM * 100.0) / kWindowUU;

		// DROPPED ONLY IF NO PART OF THE RING FALLS IN THE WINDOW. A ring whose
		// centre is outside but whose skirt reaches in IS injected, clipped by the
		// window edge. That is deliberate, and it is the opposite of what the
		// comment that used to sit here claimed (it described rejecting any ring
		// that would be cut, `U < RadiusUV || U > 1.0 - RadiusUV`, while the code
		// below did and still does the other thing). The code was kept and the
		// comment rewritten, for three reasons.
		//
		//   1. THE CUT CANNOT BE SEEN. Clipping only happens within RadiusUV of the
		//      edge -- 1.0 m for the 0.5 m player ring, 10 texels -- and that band
		//      sits inside two mechanisms that already erase it. The sponge starts
		//      6.1 m before the edge and takes anything crossing it down by 3.3e-11
		//      (create_ripple_field_materials.py, "THE SPONGE"), and the water
		//      material's own edge fade is already at zero 0.8 m before the edge
		//      (ripple_field_graph.FADE_END = 0.485, i.e. 24.8 m of the 25.6 m
		//      half-window). The straight edge a clip makes is therefore born
		//      invisible, inside an absorber, travelling inward through 6 m of it.
		//   2. REJECTING PARTIALS HAS A SILENT PATHOLOGY AND CLIPPING HAS NONE.
		//      RadiusUV comes from the caller and AutoWatch takes it from the
		//      actor's own bounds, so it is not bounded above by anything here. A
		//      request with a radius over half the window could never be entirely
		//      inside it, so it would be dropped from every position INCLUDING
		//      directly under the camera, and counted as "outside" when it was
		//      centred. That is a silent no-op with a misleading counter, which is
		//      strictly worse than an artefact in a band nothing can see.
		//   3. THE PLAYER IS ALWAYS AT THE CENTRE. This window follows the camera,
		//      so the only disturbances that can be near its edge are 25 m away --
		//      the ones (1) is already fading to nothing. There is no near-field
		//      splash in this band to trade away.
		//
		// So DroppedOutside_ counts exactly one thing: not one texel of that ring
		// lands in the 51.2 m window.
		if (U < -RadiusUV || U > 1.0 + RadiusUV || V < -RadiusUV || V > 1.0 + RadiusUV)
		{
			++DroppedOutside_;
			continue;
		}

		StepMid_->SetVectorParameterValue(
			SlotNames[Filled],
			FLinearColor(static_cast<float>(U), static_cast<float>(V),
			             static_cast<float>(RadiusUV), static_cast<float>(D.StrengthM)));
		++Filled;
		++Injected_;
	}

	// EMPTY THE REST, EVERY STEP. A slot left holding last step's disturbance
	// re-fires it on this step and every step after, which turns one splash into
	// a fountain that never stops. Strength 0 is the empty encoding.
	for (int32 i = Filled; i < kSplatSlots; ++i)
	{
		StepMid_->SetVectorParameterValue(SlotNames[i], FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
	}
}

void UVoxelRippleFieldSubsystem::StepOnce(double ShiftUvX, double ShiftUvY)
{
	UWorld* World = GetWorld();
	if (!World || !StepMid_ || !StateA_ || !StateB_)
	{
		return;
	}

	// --- the physics, recomputed every step from the console variables --------
	//
	// Recomputed rather than cached because these are tuning knobs and a knob
	// that needs a restart is a knob nobody turns. Six divisions a frame.
	const double TexelM = kTexelUU / 100.0;
	double SpeedMPS = FMath::Max(0.0, static_cast<double>(
	                                 CVarVoxelWaterRippleSpeed.GetValueOnGameThread()));
	const double MaxSpeed = kCourantCeiling * TexelM / kFixedDt;
	if (SpeedMPS > MaxSpeed)
	{
		if (!bLoggedCourantClamp_)
		{
			bLoggedCourantClamp_ = true;
			UE_LOG(LogVoxelWater, Warning,
			       TEXT("RippleField: voxel.Water.Ripple.SpeedMPS %.2f exceeds the %.2f m/s this ")
			       TEXT("grid can carry (Courant %.3f at dt=%.4f s and dx=%.2f m, against a 2D ")
			       TEXT("stability limit of %.4f) and is clamped. Above the limit the field ")
			       TEXT("diverges within a few frames and stays diverged. Said once."),
			       SpeedMPS, MaxSpeed, kCourantCeiling, kFixedDt, TexelM, kCourantLimit);
		}
		SpeedMPS = MaxSpeed;
	}
	const double Courant = SpeedMPS * kFixedDt / TexelM;

	// Half-life to a per-step multiplier: 2^(-dt/halflife). Expressed this way so
	// the substep rate and the perceived duration are independent -- see the
	// CVar's help text.
	//
	// THIS NUMBER IS ONLY THE REALISED PER-STEP FACTOR BECAUSE THE SHADER APPLIES
	// IT TO BOTH TIME LEVELS, and that is a real bug this project shipped for a
	// while rather than a hypothetical. Until 2026-08-12 the step material
	// multiplied only h(t+dt) by Damp and handed h(t) on undamped; the resulting
	// recurrence has characteristic polynomial z^2 - D*(2 - C^2*lambda)*z + D,
	// whose root PRODUCT is D, so every oscillatory mode decayed as sqrt(D) and
	// the realised half-life was 2x the configured one -- 1.8 s measured as
	// 3.626 s, with a splash still at 0.35 of its amplitude at 6 s where the CVar
	// below promises about a tenth. Nothing on the C++ side was wrong then and
	// nothing changed here in the fix; the algebra, the measurement and the two
	// candidate fixes are written up at "THE PER-STEP ATTENUATION" in
	// Tools/create_ripple_field_materials.py's STEP_CODE. If anyone ever makes
	// that shader damp one level again, this line silently becomes a half-rate
	// knob -- which is exactly how it went unnoticed, because the water still
	// looks like water.
	const double HalfLife = FMath::Max(0.05, static_cast<double>(
	                                       CVarVoxelWaterRippleHalfLife.GetValueOnGameThread()));
	const double Damp = FMath::Pow(2.0, -kFixedDt / HalfLife);

	StepMid_->SetScalarParameterValue(TEXT("Courant2"), static_cast<float>(Courant * Courant));
	StepMid_->SetScalarParameterValue(TEXT("Damp"), static_cast<float>(Damp));
	StepMid_->SetScalarParameterValue(
		TEXT("MaskEnable"),
		CVarVoxelWaterRippleMaskEnable.GetValueOnGameThread() ? 1.0f : 0.0f);
	StepMid_->SetVectorParameterValue(
		TEXT("ShiftUv"),
		FLinearColor(static_cast<float>(ShiftUvX), static_cast<float>(ShiftUvY), 0.0f, 0.0f));
	StepMid_->SetVectorParameterValue(
		TEXT("WindowOriginUU"),
		FLinearColor(static_cast<float>(static_cast<double>(OriginPx_) * kTexelUU),
		             static_cast<float>(static_cast<double>(OriginPy_) * kTexelUU), 0.0f, 0.0f));
	StepMid_->SetTextureParameterValue(TEXT("PrevState"), Front());

	FillSplatSlots();

	UKismetRenderingLibrary::DrawMaterialToRenderTarget(World, Back(), StepMid_);
	bFrontIsA_ = !bFrontIsA_;
	++TotalSteps_;

	// THE RAN-FLAG, and it is a standing rule on this project rather than a
	// nicety: "every stage must write a ran-flag distinguishable from 'found
	// nothing'" (docs/water-architecture.md §4, where three absent-stat zeros
	// produced false conclusions in one session). Without this line, "the
	// simulation never started" and "the simulation runs and produces nothing
	// visible" are the same empty log.
	if (!bLoggedFirstStep_)
	{
		bLoggedFirstStep_ = true;
		UE_LOG(LogVoxelWater, Log,
		       TEXT("RippleField: first simulation step ran. Origin (%.0f, %.0f) UU, Courant %.4f, ")
		       TEXT("damping %.5f/step, mask %s, publishing to MPC %s."),
		       static_cast<double>(OriginPx_) * kTexelUU, static_cast<double>(OriginPy_) * kTexelUU,
		       Courant, Damp,
		       CVarVoxelWaterRippleMaskEnable.GetValueOnGameThread() ? TEXT("on") : TEXT("OFF"),
		       bMpcHasParams_ ? TEXT("yes") : TEXT("NO -- see the warning at startup"));
	}
}

void UVoxelRippleFieldSubsystem::RunDerive()
{
	UWorld* World = GetWorld();
	if (!World || !DeriveMid_ || !Field_)
	{
		return;
	}
	DeriveMid_->SetTextureParameterValue(TEXT("State"), Front());
	UKismetRenderingLibrary::DrawMaterialToRenderTarget(World, Field_, DeriveMid_);
}

void UVoxelRippleFieldSubsystem::PublishWindow(float Gain)
{
	UWorld* World = GetWorld();
	UMaterialParameterCollection* Sky = SkyCollection_;
	if (!World || !bMpcHasParams_ || !Sky)
	{
		return;
	}

	// SAME TICK AS THE DRAWS, always. The pixels and the origin they are relative
	// to must never be a frame apart -- VoxelBathyField.cpp:331-334 makes the
	// argument, and it is sharper here because this window moves EVERY frame
	// rather than every 120 m: a frame that samples this frame's field through
	// last frame's origin shifts every ripple by the camera's travel, which at a
	// walking pace is a visible slosh locked to the player's motion, i.e. exactly
	// the artefact somebody would spend a session attributing to the physics.
	const double OriginXUU = static_cast<double>(OriginPx_) * kTexelUU;
	const double OriginYUU = static_cast<double>(OriginPy_) * kTexelUU;
	UKismetMaterialLibrary::SetVectorParameterValue(
		World, Sky, kRippleParamOrigin,
		FLinearColor(static_cast<float>(OriginXUU), static_cast<float>(OriginYUU), 0.0f, 0.0f));
	UKismetMaterialLibrary::SetScalarParameterValue(
		World, Sky, kRippleParamInvSize, static_cast<float>(1.0 / kWindowUU));
	UKismetMaterialLibrary::SetScalarParameterValue(World, Sky, kRippleParamGain, Gain);
	bPublished_ = (Gain > 0.0f);
}

void UVoxelRippleFieldSubsystem::RunSteps(int32 NumSteps)
{
	if (!bArmed_ || NumSteps <= 0)
	{
		return;
	}
	// The window does NOT move during a synchronous burst: no game time passes,
	// so the camera has not moved, so the shift is zero. That is also what makes
	// the burst reproducible -- the only thing that varies between two identical
	// Drop commands is the window origin, and that comes from a pinned pose.
	if (!bHaveOrigin_)
	{
		double CamX = 0.0, CamY = 0.0;
		if (GetCameraXY(CamX, CamY))
		{
			OriginPx_ = static_cast<int64>(FMath::FloorToDouble(CamX / kTexelUU)) - kSize / 2;
			OriginPy_ = static_cast<int64>(FMath::FloorToDouble(CamY / kTexelUU)) - kSize / 2;
			bHaveOrigin_ = true;
		}
	}
	for (int32 i = 0; i < NumSteps; ++i)
	{
		StepOnce(0.0, 0.0);
	}
	RunDerive();
	PublishWindow(CVarVoxelWaterRippleEnable.GetValueOnGameThread()
	                  ? FMath::Max(0.0f, CVarVoxelWaterRippleGain.GetValueOnGameThread())
	                  : 0.0f);
}

void UVoxelRippleFieldSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bArmed_)
	{
		return;
	}

	if (!CVarVoxelWaterRippleEnable.GetValueOnGameThread())
	{
		// One tick of tidying, then IsTickable stops calling us. Clearing as well
		// as publishing zero matters: a re-enable must start from flat water, not
		// from whatever was frozen into the targets when the feature was switched
		// off half a session ago.
		if (bPublished_)
		{
			PublishWindow(0.0f);
			ClearState();
			Pending_.Reset();
			Accumulator_ = 0.0;
			bHaveOrigin_ = false;
		}
		return;
	}

	AutoWatch(DeltaTime);

	double CamX = 0.0, CamY = 0.0;
	if (!GetCameraXY(CamX, CamY))
	{
		return; // no pawn yet; nothing to centre on
	}

	// --- the accumulator ------------------------------------------------------
	const int32 MaxSubsteps = FMath::Clamp(
		CVarVoxelWaterRippleMaxSubsteps.GetValueOnGameThread(), 1, 16);
	int32 Steps = 0;
	if (!CVarVoxelWaterRippleFreeze.GetValueOnGameThread())
	{
		Accumulator_ += FMath::Max(0.0, static_cast<double>(DeltaTime));
		Steps = static_cast<int32>(Accumulator_ / kFixedDt);
		if (Steps > MaxSubsteps)
		{
			// DROP the backlog rather than carry it. See the header's "STABILITY"
			// section: carrying it turns one 200 ms hitch into a second of
			// double-rate stepping, which is both a GPU spike on the frame after
			// a GPU spike and a visible speed-up of every ripple on screen.
			Steps = MaxSubsteps;
			Accumulator_ = 0.0;
		}
		else
		{
			Accumulator_ -= Steps * kFixedDt;
		}
	}

	if (Steps <= 0)
	{
		// Frozen, or less than a step of time has passed. The field and the origin
		// stay exactly where they are, which keeps them consistent with each
		// other -- moving the window without stepping would publish an origin the
		// pixels are not relative to.
		if (bPublished_)
		{
			PublishWindow(FMath::Max(0.0f, CVarVoxelWaterRippleGain.GetValueOnGameThread()));
		}
		return;
	}

	// --- where the window wants to be ----------------------------------------
	//
	// floor, not truncate: texel indices run negative and a truncating divide
	// mirrors them across the origin, which is the aliasing vxc's floorDiv
	// routing exists to avoid (VoxelBathyField.cpp:170-175).
	const int64 WantPx = static_cast<int64>(FMath::FloorToDouble(CamX / kTexelUU)) - kSize / 2;
	const int64 WantPy = static_cast<int64>(FMath::FloorToDouble(CamY / kTexelUU)) - kSize / 2;

	double ShiftUvX = 0.0;
	double ShiftUvY = 0.0;
	if (bHaveOrigin_)
	{
		// (OriginNew - OriginPrev) / windowWidth, in texels over texels, so an
		// exact multiple of 1/kSize and therefore a lossless integer scroll under
		// the state targets' nearest filtering.
		ShiftUvX = static_cast<double>(WantPx - OriginPx_) / static_cast<double>(kSize);
		ShiftUvY = static_cast<double>(WantPy - OriginPy_) / static_cast<double>(kSize);
	}
	OriginPx_ = WantPx;
	OriginPy_ = WantPy;
	bHaveOrigin_ = true;

	for (int32 i = 0; i < Steps; ++i)
	{
		// The window moves ONCE PER FRAME, not once per step: the camera has one
		// position this frame, and applying the same shift on each substep would
		// scroll the field N times as far as the camera actually moved.
		StepOnce(i == 0 ? ShiftUvX : 0.0, i == 0 ? ShiftUvY : 0.0);
	}
	RunDerive();
	PublishWindow(FMath::Max(0.0f, CVarVoxelWaterRippleGain.GetValueOnGameThread()));
}

// ============================================================================
// THE AUTO-WATCHER
// ============================================================================
//
// WHAT IT IS FOR. The owner named two cases: the player jumping in, and a thrown
// voxel volume landing in water. Both are one line inside somebody else's Tick,
// and both of those files belong to other work right now -- so this is the
// version that needs no edit to anything: poll the pawn and the two actor
// classes that fall into water, and inject on the frame their submersion flips.
//
// WHAT IT CANNOT DO, AND WHY THE PATCH NOTES STILL EXIST. It is a frame late and
// it does not know the impact speed -- it infers one from the position delta,
// which for the player is wrong in the one case that matters most: the movement
// component zeroes VerticalVelocity the instant it decides you are swimming
// (VoxelCharacterMovement.cpp:662), so the frame the flag flips is the frame the
// speed is already gone from the mover's own state. Estimating from Δposition
// recovers most of it, but a hook at VoxelCharacterMovement.cpp:630 has the real
// number for free. The patches are in docs/water-interactive-ripples.md.
//
// ITS STRENGTH IS NOT FRAME-RATE INDEPENDENT, and that is stated here rather than
// worked around because the honest fix is those patches. Δz / DeltaTime is the
// AVERAGE vertical speed over the frame that contains the crossing, so a longer
// frame averages in more of the post-entry deceleration and reports a slower
// impact: the same jump makes a different ring at 30 fps and at 120. TWO THINGS
// BOUND HOW MUCH THAT MATTERS. ImpactFraction saturates at VoxelRipple::kFullImpactSpeedMPS
// (6 m/s, a 1.8 m fall), so any real jump into water is already pinned at full
// strength on every machine, and it floors at VoxelRipple::kMinImpactFraction (0.25), so the
// gentlest wade is pinned too. Only the middle band -- entries between 1.5 and
// 6 m/s, i.e. falls of 0.11 to 1.8 m -- varies with frame rate, and there by at
// most the ratio of the two frame times.
//
// WHICH IS WHY THIS WHOLE FUNCTION IS SUPPRESSED WHILE Freeze IS ON. Captures on
// this project must be reproducible from a pinned pose (see the note above
// voxel.Water.Ripple.Drop). A frame-rate-dependent strength queued into a field
// that a capture has deliberately frozen is the one combination that cannot be
// reproduced, and it would also disturb a settled field between two frames of an
// A/B. Freeze now means "nothing enters this field", not just "do not step it".
// The watcher keeps TRACKING while frozen -- see the guard below -- because a
// watcher that stopped looking would see a stale crossing the moment it thawed
// and fire a splash for an entry that happened minutes earlier.
//
// IT ALSO CANNOT SEE ANYTHING THAT IS NOT ONE OF THESE THREE THINGS. Agents
// (VoxelAgentSubsystem) are not actors and are not iterated here; the owner's
// "other objects/entities/assets" will each need their own line. AddDisturbance
// is public and safe to call from anywhere for exactly that reason.

void UVoxelRippleFieldSubsystem::AutoWatch(float DeltaTime)
{
	if (!CVarVoxelWaterRippleAutoWatch.GetValueOnGameThread() || DeltaTime <= 0.0f)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	UVoxelWaterSubsystem* Water = World->GetSubsystem<UVoxelWaterSubsystem>();
	if (!Water)
	{
		return; // no water in this run; nothing can enter it
	}

	// TRACK BUT DO NOT INJECT while the field is frozen. See the block above this
	// function: the crossing state and last positions must stay current or the
	// first unfrozen frame fires a splash for a crossing that happened during the
	// freeze, but nothing may enter a field a capture is holding still.
	const bool bFrozen = CVarVoxelWaterRippleFreeze.GetValueOnGameThread();

	// Impact speed -> strength. A single saturating ramp with a floor: gentle
	// entries make a small ring rather than none, and everything above ~6 m/s
	// (a 1.8 m fall) makes the same big one. Two knobs for one effect is how a
	// tuning session stops converging, so the SHAPE is fixed here and only the
	// LEVEL is a console variable.
	auto ImpactFraction = [](double DownSpeedUUPerSec) -> double
	{
		const double MPS = FMath::Max(0.0, DownSpeedUUPerSec) / 100.0;
		return FMath::Clamp(MPS / VoxelRipple::kFullImpactSpeedMPS, VoxelRipple::kMinImpactFraction, 1.0);
	};

	// --- the player -----------------------------------------------------------
	APlayerController* PC = World->GetFirstPlayerController();
	if (APawn* Pawn = PC ? PC->GetPawn() : nullptr)
	{
		const FVector P = Pawn->GetActorLocation();
		const bool bNow = Water->IsUnderwaterAtWorld(P);
		if (bNow && !bPawnWasSubmerged_ && bHaveLastPawnPos_ && !bFrozen)
		{
			const double DownUU = (LastPawnPos_.Z - P.Z) / static_cast<double>(DeltaTime);
			AddDisturbance(
				P, CVarVoxelWaterRipplePlayerRadius.GetValueOnGameThread(),
				static_cast<float>(CVarVoxelWaterRipplePlayerStrength.GetValueOnGameThread()
				                   * ImpactFraction(DownUU)));
		}
		bPawnWasSubmerged_ = bNow;
		LastPawnPos_ = P;
		bHaveLastPawnPos_ = true;
	}

	// --- thrown volumes and debris -------------------------------------------
	//
	// A weak key, so this map can never be the reason a debris actor outlives its
	// own cleanup. Dead entries are swept here rather than on destruction,
	// because there is no destruction callback to hook that would not itself be
	// an edit to those classes.
	for (auto It = Watched_.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}

	const float ObjectStrength = CVarVoxelWaterRippleObjectStrength.GetValueOnGameThread();
	auto Consider = [&](AActor* Actor)
	{
		if (!Actor)
		{
			return;
		}
		const FVector P = Actor->GetActorLocation();
		const bool bNow = Water->IsUnderwaterAtWorld(P);
		FWatchedActor& W = Watched_.FindOrAdd(Actor);
		if (W.bSeen && bNow && !W.bWasSubmerged && !bFrozen)
		{
			// Radius from the object's own bounds. A thrown boulder should make a
			// wider ring than a pebble, and the actor already knows how big it is
			// -- asking it is both more correct and one fewer number to tune.
			FVector BoundsOrigin = FVector::ZeroVector;
			FVector BoundsExtent = FVector::ZeroVector;
			Actor->GetActorBounds(true, BoundsOrigin, BoundsExtent);
			const double RadiusM =
				FMath::Max(BoundsExtent.X, BoundsExtent.Y) / 100.0;
			const double DownUU = (W.LastPos.Z - P.Z) / static_cast<double>(DeltaTime);
			AddDisturbance(P, static_cast<float>(RadiusM),
			               static_cast<float>(ObjectStrength * ImpactFraction(DownUU)));
		}
		W.bWasSubmerged = bNow;
		W.LastPos = P;
		W.bSeen = true;
	};

	// ONE PASS OVER THE ACTOR LIST, NOT ONE PER CLASS. TActorIterator<T> does not
	// index by class -- it walks every actor in every loaded level and filters
	// with IsA -- so the two iterators this used to run were 2 x O(all actors)
	// per frame to look at two classes with no common base but AActor
	// (VoxelExplosive.h:34, VoxelDebris.h:40, both `: public AActor`). One pass
	// with two IsA checks does the same number of class tests and half the
	// traversal, and it is what the AutoWatch CVar's cost note now describes; the
	// note used to mention only the per-actor water query, which was the smaller
	// half of what this function actually costs.
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && (Actor->IsA<AVoxelExplosive>() || Actor->IsA<AVoxelDebris>()))
		{
			Consider(Actor);
		}
	}
}

void UVoxelRippleFieldSubsystem::ProbeTextures()
{
	// All three, in the order the data flows, so the log reads as a pipeline and
	// the first zero is the culprit. Front() is the state the derive pass reads,
	// which is the one that matters -- probing the back buffer would report the
	// previous step and could look like a one-frame lag that is not there.
	UE_LOG(LogVoxelWater, Log,
	       TEXT("Ripple.Probe: armed=%d published=%d steps=%lld injected=%lld -- the counters say "
	            "healthy; below is what is actually in the textures."),
	       bArmed_ ? 1 : 0, bPublished_ ? 1 : 0, (long long)TotalSteps_, (long long)Injected_);
	ProbeRenderTarget(Front(), TEXT("state(front)"));
	ProbeRenderTarget(Front() == StateA_ ? StateB_ : StateA_, TEXT("state(back)"));
	ProbeRenderTarget(Field_, TEXT("field(derived)"));
}
