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
	// at the pawn's walk-mode collision-box center -- see
	// AVoxelEarthFlyPawn::WalkBoxHalfExtentZ = 90): legs span Z -90..-10,
	// torso -10..60, head 60..90 -- feet land on the box's bottom face.
	const FLinearColor TorsoColor(0.20f, 0.35f, 0.55f, 1.f);
	const FLinearColor LimbColor(0.55f, 0.30f, 0.15f, 1.f);
	const FLinearColor HeadColor(0.85f, 0.70f, 0.55f, 1.f);

	TorsoMesh = MakeBoxPart(TEXT("TorsoMesh"), this, CubeMesh, FVector(24.0, 40.0, 70.0), FVector(0.0, 0.0, 25.0), TorsoColor);
	HeadMesh = MakeBoxPart(TEXT("HeadMesh"), this, CubeMesh, FVector(26.0, 26.0, 26.0), FVector(0.0, 0.0, 75.0), HeadColor);

	LeftArmPivot = CreateDefaultSubobject<USceneComponent>(TEXT("LeftArmPivot"));
	LeftArmPivot->SetupAttachment(this);
	LeftArmPivot->SetRelativeLocation(FVector(0.0, -28.0, 55.0));

	RightArmPivot = CreateDefaultSubobject<USceneComponent>(TEXT("RightArmPivot"));
	RightArmPivot->SetupAttachment(this);
	RightArmPivot->SetRelativeLocation(FVector(0.0, 28.0, 55.0));

	LeftLegPivot = CreateDefaultSubobject<USceneComponent>(TEXT("LeftLegPivot"));
	LeftLegPivot->SetupAttachment(this);
	LeftLegPivot->SetRelativeLocation(FVector(0.0, -10.0, -10.0));

	RightLegPivot = CreateDefaultSubobject<USceneComponent>(TEXT("RightLegPivot"));
	RightLegPivot->SetupAttachment(this);
	RightLegPivot->SetRelativeLocation(FVector(0.0, 10.0, -10.0));

	// Each limb mesh hangs half its own length below its pivot so the pivot
	// sits at the hinge (shoulder/hip), not the limb's center.
	LeftArmMesh = MakeBoxPart(TEXT("LeftArmMesh"), LeftArmPivot, CubeMesh, FVector(16.0, 16.0, 70.0), FVector(0.0, 0.0, -35.0), LimbColor);
	RightArmMesh = MakeBoxPart(TEXT("RightArmMesh"), RightArmPivot, CubeMesh, FVector(16.0, 16.0, 70.0), FVector(0.0, 0.0, -35.0), LimbColor);
	LeftLegMesh = MakeBoxPart(TEXT("LeftLegMesh"), LeftLegPivot, CubeMesh, FVector(18.0, 18.0, 80.0), FVector(0.0, 0.0, -40.0), LimbColor);
	RightLegMesh = MakeBoxPart(TEXT("RightLegMesh"), RightLegPivot, CubeMesh, FVector(18.0, 18.0, 80.0), FVector(0.0, 0.0, -40.0), LimbColor);
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

void UVoxelProxyBodyComponent::UpdateAnimation(float DeltaTime, double HorizontalSpeedUU)
{
	const double Speed = FMath::Abs(HorizontalSpeedUU);

	// Walk cycle: phase advances with distance traveled (not just time), so
	// faster movement swings the limbs faster -- "speed-proportional rate"
	// per the Player experience decisions table.
	GaitPhase += (float)(Speed * DeltaTime) * (2.f * PI / StrideLengthUU);
	GaitPhase = FMath::Fmod(GaitPhase, 2.f * PI);

	const float SwingScale = (float)FMath::Clamp(Speed / RefWalkSpeedUU, 0.0, 1.0);
	const float SwingAngle = FMath::Sin(GaitPhase) * MaxSwingDegrees * SwingScale;

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
	BobPhase += DeltaTime * BobFrequencyHz * 2.f * PI;
	BobPhase = FMath::Fmod(BobPhase, 2.f * PI);
	const float BobOffset = FMath::Sin(BobPhase) * (float)BobAmplitudeUU * (1.f - SwingScale);
	SetRelativeLocation(FVector(0.0, 0.0, BobOffset));
}
