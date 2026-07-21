#include "VoxelDebris.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "UObject/ConstructorHelpers.h"
#include "VoxelEarth.h" // LogVoxelEarth
#include "VoxelWorldSubsystem.h"

AVoxelDebris::AVoxelDebris()
{
	PrimaryActorTick.bCanEverTick = true;

	// Root = the physics body (see class comment): an engine cube kept at
	// component scale 1 so the attached ISM's per-voxel instances are never
	// distorted by parent scale. It is invisible; its only job is to carry
	// gravity and drive the actor transform. Its 1m size is irrelevant -- it
	// never collides (responses all Ignore) and the settle clamp uses the
	// island's own AABB, not this body's extent.
	PhysicsBody = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PhysicsBody"));
	SetRootComponent(PhysicsBody);

	VoxelISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("VoxelISM"));
	VoxelISM->SetupAttachment(PhysicsBody);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		PhysicsBody->SetStaticMesh(CubeMesh.Object);
		VoxelISM->SetStaticMesh(CubeMesh.Object);
	}

	PhysicsBody->SetVisibility(false);
	PhysicsBody->SetMobility(EComponentMobility::Movable);
	VoxelISM->SetMobility(EComponentMobility::Movable);
	// The ISM is decorative only -- no collision, no shadows-cost surprises.
	VoxelISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	VoxelISM->SetCanEverAffectNavigation(false);
}

void AVoxelDebris::InitFromIsland(const TArray<VoxelCoords::FVoxelCoord>& IslandVoxels)
{
	VoxelCount = IslandVoxels.Num();
	if (VoxelCount == 0)
	{
		return;
	}

	// World-space AABB of the island's voxel CENTRES.
	FVector MinC(TNumericLimits<double>::Max());
	FVector MaxC(TNumericLimits<double>::Lowest());
	for (const VoxelCoords::FVoxelCoord& V : IslandVoxels)
	{
		const FVector C = VoxelCoords::VoxelToWorldCenter(V);
		MinC = MinC.ComponentMin(C);
		MaxC = MaxC.ComponentMax(C);
	}
	const FVector CentreWorld = (MinC + MaxC) * 0.5;
	const double HalfVoxel = VoxelCoords::VoxelSizeUU * 0.5;
	AabbHalfHeightUU = (MaxC.Z - MinC.Z) * 0.5 + HalfVoxel;

	SetActorLocation(CentreWorld);

	// One engine-cube instance per voxel, positioned relative to the body
	// centre. Cube base mesh is 100 UU, so scale by VoxelSizeUU/100 to make a
	// single voxel.
	const double InstanceScale = VoxelCoords::VoxelSizeUU / 100.0;
	VoxelISM->PreAllocateInstancesMemory(VoxelCount);
	for (const VoxelCoords::FVoxelCoord& V : IslandVoxels)
	{
		const FVector Rel = VoxelCoords::VoxelToWorldCenter(V) - CentreWorld;
		const FTransform Xf(FRotator::ZeroRotator, Rel, FVector(InstanceScale));
		VoxelISM->AddInstance(Xf); // relative to the ISM (= actor origin)
	}

	// Start the Chaos rigid body falling. Gravity only; ignore every collision
	// channel (terrain is not a Chaos body, and we do not want the debris
	// shoving the pawn around). A gentle spin makes the fall read as a real
	// chunk toppling rather than a rigid slab dropping.
	PhysicsBody->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	PhysicsBody->SetCollisionResponseToAllChannels(ECR_Ignore);
	PhysicsBody->SetEnableGravity(true);
	PhysicsBody->SetSimulatePhysics(true);
	// Deterministic-enough tumble seed from the island's min corner (purely
	// cosmetic, so exact reproducibility is not required).
	const VoxelCoords::FVoxelCoord& Seed0 = IslandVoxels[0];
	const FVector Spin(float((Seed0.X % 7) - 3) * 12.f, float((Seed0.Y % 7) - 3) * 12.f, float((Seed0.Z % 5) - 2) * 8.f);
	PhysicsBody->SetPhysicsAngularVelocityInDegrees(Spin);

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelDebris spawned: voxels=%d centre=(%.0f,%.0f,%.0f) halfH=%.0fUU -- falling (cosmetic Chaos body)"),
	       VoxelCount, CentreWorld.X, CentreWorld.Y, CentreWorld.Z, AabbHalfHeightUU);
}

void AVoxelDebris::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bSettled)
	{
		return;
	}
	AgeSeconds += DeltaSeconds;

	UWorld* W = GetWorld();
	UVoxelWorldSubsystem* Sub = W ? W->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Sub)
	{
		return;
	}

	const FVector Loc = GetActorLocation();

	// Raycast the voxel world straight down (terrain has no Chaos collision --
	// see class comment). Start a little above the body centre so a body that
	// has just tunnelled slightly below the surface between ticks still finds
	// the surface above its centre and clamps back up.
	FVector HitCentre, PrevCentre;
	const FVector Start = Loc + FVector(0.0, 0.0, VoxelCoords::VoxelSizeUU);
	const double MaxDist = AabbHalfHeightUU + 100000.0; // generous; terrain is always below
	if (Sub->RaycastVoxelWorld(Start, FVector(0.0, 0.0, -1.0), MaxDist, HitCentre, PrevCentre))
	{
		const double SurfaceTopZ = HitCentre.Z + VoxelCoords::VoxelSizeUU * 0.5; // top face of the hit voxel
		const double BottomZ = Loc.Z - AabbHalfHeightUU;
		if (BottomZ <= SurfaceTopZ)
		{
			SettleOnSurface(SurfaceTopZ);
			return;
		}
	}

	if (AgeSeconds >= MaxFallSeconds)
	{
		// Never found ground (shouldn't happen over solid terrain) -- freeze in
		// place rather than fall forever.
		SettleOnSurface(Loc.Z - AabbHalfHeightUU);
	}
}

void AVoxelDebris::SettleOnSurface(double SurfaceTopZUU)
{
	if (bSettled)
	{
		return;
	}
	bSettled = true;

	PhysicsBody->SetSimulatePhysics(false);
	FVector L = GetActorLocation();
	L.Z = SurfaceTopZUU + AabbHalfHeightUU; // rest the island's lowest face on the surface
	SetActorLocation(L);
	// Level the body so the voxel cluster sits flat on rest (cosmetic).
	SetActorRotation(FRotator::ZeroRotator);

	UE_LOG(LogVoxelEarth, Log, TEXT("VoxelDebris settled: voxels=%d rest=(%.0f,%.0f,%.0f) after %.2fs"), VoxelCount, L.X,
	       L.Y, L.Z, AgeSeconds);
}
