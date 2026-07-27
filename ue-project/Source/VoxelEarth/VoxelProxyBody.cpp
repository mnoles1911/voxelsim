#include "VoxelProxyBody.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

UVoxelProxyBodyComponent::UVoxelProxyBodyComponent()
{
	// Driven externally via UpdateAnimation, called from
	// AVoxelEarthFlyPawn::Tick -- see header comment.
	PrimaryComponentTick.bCanEverTick = false;
	SetMobility(EComponentMobility::Movable);

	// ConstructorHelpers::FObjectFinder is the supported way to load a
	// default asset from within a UObject constructor (works for both
	// editor and cooked builds); /Engine/BasicShapes/Cube ships with the
	// engine, so this needs zero project content.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* CubeMesh = CubeFinder.Succeeded() ? CubeFinder.Object : nullptr;

	// 2-3 tint colors (Player experience decisions table: "MID-tinted 2-3
	// colors"). Layout (UU, relative to this component's origin, which sits
	// at the pawn's walk-mode collision-box center --
	// VoxelMovementTuning::StandHalfExtentZ = 90): legs span Z -90..-10,
	// torso -10..60, head 62..90 -- feet land on the box's bottom face.
	//
	// PROPORTIONS ARE TIED TO THE COLLISION BOX (60 x 60 x 180 UU, i.e. +-30 XY,
	// +-90 Z). They did not used to be, and the mismatch was not cosmetic: the
	// body was 24 UU deep against a 60 UU box (43% of the real footprint) while
	// the ARMS reached +-36 and hung 6 UU OUTSIDE the volume that actually
	// collides. In third person that reported the footprint wrongly in two
	// directions at once, which is the opposite of what a scale placeholder is
	// for. Now the arms sit flush with the box wall at exactly +-30, and the
	// torso fills 40 of the 60 UU depth.
	//
	// The body still does not reach the box's CORNERS, and that is correct
	// rather than unfinished: a 0.6 x 0.6 m footprint is squarer than a human
	// (chest depth ~25 cm against ~45 cm shoulders), so a body that filled it
	// would stop reading as a character. The exact bounds are carried by the
	// debug wireframe (UVoxelCharacterMovementComponent::DebugDrawVolume); the
	// body's job is simply to stop contradicting them.
	//
	// Swinging limbs DO leave the box along X mid-stride (a 70 UU arm at the 35
	// degree swing limit reaches ~40 UU forward). That is not the mismatch fixed
	// here and needs no fixing: no character's animation is contained by its
	// collision volume. What mattered was the STATIC lateral overhang, which was
	// there whether you moved or not.
	const FLinearColor TorsoColor(0.20f, 0.35f, 0.55f, 1.f);
	const FLinearColor LimbColor(0.55f, 0.30f, 0.15f, 1.f);
	const FLinearColor HeadColor(0.85f, 0.70f, 0.55f, 1.f);

	// Torso X 40 (+-20, was +-12), Y 40 (+-20).
	TorsoMesh = MakeBoxPart(TEXT("TorsoMesh"), this, CubeMesh, FVector(40.0, 40.0, 70.0), FVector(0.0, 0.0, 25.0), TorsoColor);
	// Head X 32 (+-16), Y 30 (+-15), Z 28 -> spans 62..90, flush with the box top.
	HeadMesh = MakeBoxPart(TEXT("HeadMesh"), this, CubeMesh, FVector(32.0, 30.0, 28.0), FVector(0.0, 0.0, 76.0), HeadColor);

	// Shoulder pivots at +-23 with 14-wide arms put the outer arm face at
	// exactly +-30 -- the collision box wall, not past it.
	LeftArmPivot = CreateDefaultSubobject<USceneComponent>(TEXT("LeftArmPivot"));
	LeftArmPivot->SetupAttachment(this);
	LeftArmPivot->SetRelativeLocation(FVector(0.0, -23.0, 55.0));

	RightArmPivot = CreateDefaultSubobject<USceneComponent>(TEXT("RightArmPivot"));
	RightArmPivot->SetupAttachment(this);
	RightArmPivot->SetRelativeLocation(FVector(0.0, 23.0, 55.0));

	LeftLegPivot = CreateDefaultSubobject<USceneComponent>(TEXT("LeftLegPivot"));
	LeftLegPivot->SetupAttachment(this);
	LeftLegPivot->SetRelativeLocation(FVector(0.0, -11.0, -10.0));

	RightLegPivot = CreateDefaultSubobject<USceneComponent>(TEXT("RightLegPivot"));
	RightLegPivot->SetupAttachment(this);
	RightLegPivot->SetRelativeLocation(FVector(0.0, 11.0, -10.0));

	// Each limb mesh hangs half its own length below its pivot so the pivot
	// sits at the hinge (shoulder/hip), not the limb's center. Z spans are
	// UNCHANGED by the widening, so the feet stay on the box's bottom face and
	// the crouch squash (ProxyCrouchScaleZ) keeps mapping -90..+90 onto
	// -60..+60 exactly.
	LeftArmMesh = MakeBoxPart(TEXT("LeftArmMesh"), LeftArmPivot, CubeMesh, FVector(28.0, 14.0, 70.0), FVector(0.0, 0.0, -35.0), LimbColor);
	RightArmMesh = MakeBoxPart(TEXT("RightArmMesh"), RightArmPivot, CubeMesh, FVector(28.0, 14.0, 70.0), FVector(0.0, 0.0, -35.0), LimbColor);
	LeftLegMesh = MakeBoxPart(TEXT("LeftLegMesh"), LeftLegPivot, CubeMesh, FVector(28.0, 20.0, 80.0), FVector(0.0, 0.0, -40.0), LimbColor);
	RightLegMesh = MakeBoxPart(TEXT("RightLegMesh"), RightLegPivot, CubeMesh, FVector(28.0, 20.0, 80.0), FVector(0.0, 0.0, -40.0), LimbColor);
}

UStaticMeshComponent* UVoxelProxyBodyComponent::MakeBoxPart(FName Name, USceneComponent* AttachTo, UStaticMesh* CubeMesh, const FVector& SizeUU,
                                                              const FVector& RelativeLocationUU, const FLinearColor& Tint)
{
	UStaticMeshComponent* Mesh = CreateDefaultSubobject<UStaticMeshComponent>(Name);
	Mesh->SetupAttachment(AttachTo);
	if (CubeMesh)
	{
		Mesh->SetStaticMesh(CubeMesh);
	}
	// The engine unit cube is 100x100x100 UU; scale per-axis to the desired
	// box size.
	Mesh->SetRelativeScale3D(SizeUU / 100.0);
	Mesh->SetRelativeLocation(RelativeLocationUU);
	Mesh->SetMobility(EComponentMobility::Movable);
	// Visual-only proxy: walk-mode collision is the DDA sweep against
	// UVoxelWorldSubsystem (docs/m1-plan.md SS3.3, "no Chaos for terrain");
	// this must never add a second, conflicting collision source.
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(true);

	if (UMaterialInterface* BaseMat = Mesh->GetMaterial(0))
	{
		if (UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMat, Mesh))
		{
			MID->SetVectorParameterValue(TEXT("Color"), Tint);
			Mesh->SetMaterial(0, MID);
		}
	}
	return Mesh;
}

void UVoxelProxyBodyComponent::UpdateAnimation(float DeltaTime, double HorizontalSpeedUU, float GaitPhase, float CrouchAlpha)
{
	using namespace VoxelMovementTuning;

	const double Speed = FMath::Abs(HorizontalSpeedUU);

	// The walk-cycle PHASE now arrives from the movement component, which
	// advances it with distance travelled and shares it with the first-person
	// camera bob. Only the AMPLITUDE is derived here, from current speed, so
	// the swing still grows with pace ("speed-proportional rate" per the Player
	// experience decisions table) across the full 8-tier speed dial.
	const float SwingScale = (float)FMath::Clamp(Speed / ProxyRefSpeedUU, 0.0, 1.0);
	const float SwingAngle = FMath::Sin(GaitPhase) * ProxyMaxSwingDegrees * SwingScale;

	// FRotator(Pitch, Yaw, Roll): Pitch swings a vertically-hanging limb
	// forward/back about its pivot -- exactly the walk-cycle motion wanted
	// here. Opposite arm/leg pairs swing in antiphase (left leg forward
	// pairs with right arm forward).
	if (LeftLegPivot) LeftLegPivot->SetRelativeRotation(FRotator(SwingAngle, 0.f, 0.f));
	if (RightLegPivot) RightLegPivot->SetRelativeRotation(FRotator(-SwingAngle, 0.f, 0.f));
	if (LeftArmPivot) LeftArmPivot->SetRelativeRotation(FRotator(-SwingAngle, 0.f, 0.f));
	if (RightArmPivot) RightArmPivot->SetRelativeRotation(FRotator(SwingAngle, 0.f, 0.f));

	// Idle breathing bob: a slow vertical sine offset on the whole body that
	// fades out as the walk cycle takes over (SwingScale -> 1).
	BobPhase += DeltaTime * ProxyBobFrequencyHz * 2.f * PI;
	BobPhase = FMath::Fmod(BobPhase, 2.f * PI);
	const float BobOffset = FMath::Sin(BobPhase) * (float)ProxyBobAmplitudeUU * (1.f - SwingScale);

	// Crouch squash. The collision box resizes instantly (there is no half-way
	// box to collide against), but the VISUAL blend is eased so the body sinks
	// rather than popping.
	const float BlendAlpha = 1.f - FMath::Exp(-CrouchBlendRatePerSec * DeltaTime);
	CrouchBlend = FMath::Lerp(CrouchBlend, FMath::Clamp(CrouchAlpha, 0.f, 1.f), BlendAlpha);

	// Scaling about this component's origin -- the box CENTRE -- maps the body's
	// -90..+90 UU span onto the crouched box's -60..+60 exactly, so the feet
	// stay on the box's bottom face without any compensating offset.
	const float ScaleZ = FMath::Lerp(1.f, (float)ProxyCrouchScaleZ, CrouchBlend);
	SetRelativeScale3D(FVector(1.0, 1.0, ScaleZ));

	// A component's own scale applies to its children, not to its own relative
	// location (which lives in PARENT space, and the parent is the unscaled
	// pawn root) -- so the bob stays the same physical size in both stances
	// with no compensation needed.
	SetRelativeLocation(FVector(0.0, 0.0, BobOffset));
}
