#include "VoxelDebris.h"
#include "VoxelDebrisLifecycle.h"
#include "VoxelDetachedPersistence.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "UObject/ConstructorHelpers.h"
#include "VoxelEarth.h" // LogVoxelEarth
#include "VoxelEofDirtyLedger.h" // EndOfFrameUpdates attribution
#include "VoxelWorldSubsystem.h"
#include "VoxelFineTileStreamer.h"
#include "VoxelAssetAppearance.h"
#include "VoxelVegetationRender.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "voxelcore/materialpalette.h"

AVoxelDebris::AVoxelDebris()
{
	PrimaryActorTick.bCanEverTick = true;
	Cleanup = CreateDefaultSubobject<UVoxelDebrisLifecycle>(TEXT("Cleanup"));

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

namespace {
constexpr int32 DebrisAppearanceMarker=-1447313969;
constexpr int32 MaxAppearanceCells=512*1024; // coordinate+appearance payload below 64MiB
bool ValidDebrisAppearance(const TArray<VoxelCoords::FVoxelCoord>& Voxels,const TArray<FVoxelDebrisCellAppearance>& Cells){
    if(Voxels.IsEmpty()||Voxels.Num()>MaxAppearanceCells||Cells.Num()!=Voxels.Num())return false;
    TSet<VoxelCoords::FVoxelCoord> Seen;Seen.Reserve(Voxels.Num());
    for(int I=0;I<Cells.Num();++I){const auto& C=Cells[I];
        if(!(C.Coord==Voxels[I])||Seen.Contains(C.Coord)||C.Material==0||C.Material>=vxc::kMaterialCount||C.SourceYawQuarter>3)return false;
        for(int64 V:{C.Coord.X,C.Coord.Y,C.Coord.Z})if(V==MIN_int64||V==MAX_int64)return false;
        if(C.Approved&&(C.SourceCell.GetMin()<0||C.SourceCell.GetMax()>65535))return false;
        if(!C.Approved&&(C.Needle||C.FoliageMask))return false;
        Seen.Add(C.Coord);
    }
    return true;
}
}
int32 AVoxelDebris::InitFromIsland(const TArray<VoxelCoords::FVoxelCoord>& Voxels,int32 MaxInstances){
    PersistentAppearance.Reset();return InitIsland(Voxels,MaxInstances);
}
int32 AVoxelDebris::InitFromIslandWithAppearance(const TArray<VoxelCoords::FVoxelCoord>& Voxels,const TArray<FVoxelDebrisCellAppearance>& Cells,int32 MaxInstances){
    if(!ValidDebrisAppearance(Voxels,Cells))return 0;
    if(!LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Voxel/M_VoxelDetailAsset.M_VoxelDetailAsset"))){UE_LOG(LogVoxelEarth,Error,TEXT("Debris appearance admission refused: detail material missing"));return 0;}
    PersistentAppearance=Cells;return InitIsland(Voxels,MaxInstances);
}
int32 AVoxelDebris::InitIsland(const TArray<VoxelCoords::FVoxelCoord>& IslandVoxels, int32 MaxInstances)
{
	for(auto C:AppearanceMeshes)if(C)C->DestroyComponent();AppearanceMeshes.Reset();
    VoxelISM->ClearInstances();
    PersistentInstanceBudget=FMath::Clamp(MaxInstances,1,MaxInstancesPerBody);
	VoxelCount = IslandVoxels.Num();
	PersistentVoxels = IslandVoxels;
	CachedObjectGeometry.Reset();
	if (VoxelCount == 0)
	{
		Destroy();
		return 0;
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
	// Only genuinely small islands are cosmetic chips. Sparse long branches
	// must not disappear just because they contain few voxels.
	const double LongestSide = (MaxC - MinC).GetMax() + 2. * HalfVoxel;
	Cleanup->Configure(VoxelCount <= 8 && LongestSide <= 30.
		? EVoxelDebrisLifetime::Cosmetic : EVoxelDebrisLifetime::Substantial);

	SetActorLocation(CentreWorld);

	// --- Surface shell only (see the header comment) ------------------------
	// A voxel whose six face-neighbours are ALL in the island cannot be seen
	// from any angle, so it is dropped. Membership is tested against a set
	// built once from the island's own coords; the island list is already
	// deterministic (sorted by the detector), so the shell is too.
	TSet<VoxelCoords::FVoxelCoord> Occupied;
	Occupied.Reserve(VoxelCount);
	for (const VoxelCoords::FVoxelCoord& V : IslandVoxels)
	{
		Occupied.Add(V);
	}
	TArray<VoxelCoords::FVoxelCoord> Shell;
	Shell.Reserve(FMath::Min(VoxelCount, MaxInstancesPerBody * 4));
	for (const VoxelCoords::FVoxelCoord& V : IslandVoxels)
	{
		const bool bInterior =
			Occupied.Contains(VoxelCoords::FVoxelCoord{V.X - 1, V.Y, V.Z}) &&
			Occupied.Contains(VoxelCoords::FVoxelCoord{V.X + 1, V.Y, V.Z}) &&
			Occupied.Contains(VoxelCoords::FVoxelCoord{V.X, V.Y - 1, V.Z}) &&
			Occupied.Contains(VoxelCoords::FVoxelCoord{V.X, V.Y + 1, V.Z}) &&
			Occupied.Contains(VoxelCoords::FVoxelCoord{V.X, V.Y, V.Z - 1}) &&
			Occupied.Contains(VoxelCoords::FVoxelCoord{V.X, V.Y, V.Z + 1});
		if (!bInterior)
		{
			Shell.Add(V);
		}
	}

	// Budget: the smaller of this body's own ceiling and whatever the caller
	// has left for this edit. If the shell still does not fit, take every Nth
	// voxel (uniform stride, not a prefix -- a prefix would render only the
	// bottom slice of a big slab and look like half the piece vanished).
	const int32 Budget = FMath::Max(1, FMath::Min(MaxInstances, MaxInstancesPerBody));
	const int32 Stride = (Shell.Num() > Budget) ? FMath::DivideAndRoundUp(Shell.Num(), Budget) : 1;

	// One engine-cube instance per rendered voxel, positioned relative to the
	// body centre. Cube base mesh is 100 UU, so scale by VoxelSizeUU/100 to
	// make a single voxel.
	const double InstanceScale = VoxelCoords::VoxelSizeUU / 100.0;
	const int32 Estimate = FMath::DivideAndRoundUp(Shell.Num(), Stride);
	if(PersistentAppearance.IsEmpty())VoxelISM->PreAllocateInstancesMemory(Estimate);
	int32 Instances = 0;
	for (int32 I = 0; I < Shell.Num(); I += Stride)
	{
		const FVector Rel = VoxelCoords::VoxelToWorldCenter(Shell[I]) - CentreWorld;
		const FTransform Xf(FRotator::ZeroRotator, Rel, FVector(InstanceScale));
		if(PersistentAppearance.IsEmpty())VoxelISM->AddInstance(Xf); // relative to the ISM (= actor origin)
		++Instances;
	}
	if(!PersistentAppearance.IsEmpty())BuildAppearanceShell(Shell,Stride,CentreWorld);
	// ONE count for the whole fill, not one per instance: this is one debris
	// body's ISM, dirtied once as far as EndOfFrameUpdates is concerned.
	VoxelEofLedger::Count(VoxelEofLedger::ESource::Debris);

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
	       TEXT("VoxelDebris spawned: voxels=%d shell=%d instances=%d (stride %d) centre=(%.0f,%.0f,%.0f) halfH=%.0fUU ")
	       TEXT("-- falling (cosmetic Chaos body)"),
	       VoxelCount, Shell.Num(), Instances, Stride, CentreWorld.X, CentreWorld.Y, CentreWorld.Z, AabbHalfHeightUU);
	return Instances;
}

bool AVoxelDebris::GeometryState(FArchive& Ar)
{
    using namespace VoxelDetachedPersistence;
    bool Extended=!PersistentAppearance.IsEmpty();
    if(Ar.IsLoading()){
        const int64 Start=Ar.Tell();int32 Marker=0;Ar<<Marker;
        if(Ar.IsError())return false;
        Extended=Marker==DebrisAppearanceMarker;
        if(!Extended){Ar.Seek(Start);PersistentAppearance.Reset();PersistentInstanceBudget=MaxInstancesPerBody;}
    }else if(Extended){int32 Marker=DebrisAppearanceMarker;Ar<<Marker;}
    if(Extended){uint32 Version=1;Ar<<Version<<PersistentInstanceBudget;
        if(Ar.IsError()||Version!=1||PersistentInstanceBudget<1||PersistentInstanceBudget>MaxInstancesPerBody)return false;}
    if(!Array(Ar,PersistentVoxels,1024*1024,[](FArchive& A,VoxelCoords::FVoxelCoord& V){A<<V.X<<V.Y<<V.Z;}))return false;
    if(Extended){
        if(!Array(Ar,PersistentAppearance,MaxAppearanceCells,[](FArchive& A,FVoxelDebrisCellAppearance& C){
            A<<C.Coord.X<<C.Coord.Y<<C.Coord.Z<<C.Material<<C.BaseRGB<<C.SourceCell<<C.SourceYawQuarter;
            uint8 Flags=(C.Approved?1:0)|(C.Needle?2:0)|(C.FoliageMask?4:0);A<<Flags;
            if(Flags&~7){A.SetError();return;}C.Approved=Flags&1;C.Needle=Flags&2;C.FoliageMask=Flags&4;
        })||!ValidDebrisAppearance(PersistentVoxels,PersistentAppearance))return false;
    }
    return !Ar.IsError();
}

void AVoxelDebris::BuildAppearanceShell(const TArray<VoxelCoords::FVoxelCoord>& Shell,int32 Stride,const FVector& Centre)
{
    struct FSection {TArray<FVector> P,N;TArray<int32> I;TArray<FVector2D> UV,Wind;TArray<FLinearColor> C;TArray<FProcMeshTangent> T;};
    FSection Sections[5];
    TMap<VoxelCoords::FVoxelCoord,int32> Lookup;Lookup.Reserve(PersistentAppearance.Num());
    for(int I=0;I<PersistentAppearance.Num();++I)Lookup.Add(PersistentAppearance[I].Coord,I);
    TSet<VoxelCoords::FVoxelCoord> Rendered;
    for(int I=0;I<Shell.Num();I+=Stride)Rendered.Add(Shell[I]);
    for(int I=0;I<Shell.Num();I+=Stride){
        const auto& Cell=PersistentAppearance[Lookup.FindChecked(Shell[I])];
        auto& S=Sections[Cell.Approved?((Cell.Needle?1:0)|(Cell.FoliageMask?2:0)):4];
        const FVector Base=VoxelCoords::VoxelToWorldCenter(Cell.Coord)-Centre-FVector(5.);
        for(int Axis=0;Axis<3;++Axis)for(int Sign=0;Sign<2;++Sign){
            auto Neighbor=Cell.Coord;const int Delta=Sign?1:-1;
            if(Axis==0)Neighbor.X+=Delta;else if(Axis==1)Neighbor.Y+=Delta;else Neighbor.Z+=Delta;
            if(Rendered.Contains(Neighbor)){
                const auto& Other=PersistentAppearance[Lookup.FindChecked(Neighbor)];
                if(!(VoxelVegetationRender::IsWood(Cell.Material)&&VoxelVegetationRender::IsLeaf(Other.Material)))continue;
            }
            int SourceAxis=Axis;bool Positive=Sign!=0;
            if(SourceAxis<2){if(Cell.SourceYawQuarter==2||(Cell.SourceYawQuarter==1&&Axis==0)||(Cell.SourceYawQuarter==3&&Axis==1))Positive=!Positive;if(Cell.SourceYawQuarter&1)SourceAxis=1-SourceAxis;}
            FLinearColor Color;
            if(Cell.Approved)Color=FVoxelAssetAppearance::FaceColor(Cell.BaseRGB,Cell.SourceCell,SourceAxis,Positive);
            else{const auto R=vxc::kMaterialPalette[Cell.Material].face[Axis==2?(Sign?vxc::kFaceTop:vxc::kFaceBottom):vxc::kFaceSide];Color=FLinearColor::FromSRGBColor(FColor(R.r,R.g,R.b));}
            Color.A=VoxelVegetationRender::MaterialClass(Cell.Material);
            FVector Normal=FVector::ZeroVector;Normal[Axis]=Sign?1.:-1.;const int U=(Axis+1)%3,V=(Axis+2)%3;
            FVector Tangent=FVector::ZeroVector;Tangent[U]=1.;const int First=S.P.Num();
            constexpr int CU[4]={0,0,1,1},CV[4]={0,1,1,0};
            for(int Corner=0;Corner<4;++Corner){
                FVector Fraction=FVector::ZeroVector;Fraction[Axis]=Sign;Fraction[U]=CU[Corner];Fraction[V]=CV[Corner];
                S.P.Add(Base+Fraction*10.);S.N.Add(Normal);S.C.Add(Color);S.T.Add(FProcMeshTangent(Tangent,false));S.Wind.Add(FVector2D::ZeroVector);
                FVector SourceFraction=Fraction;
                if(Cell.SourceYawQuarter==1)SourceFraction=FVector(Fraction.Y,1.-Fraction.X,Fraction.Z);
                else if(Cell.SourceYawQuarter==2)SourceFraction=FVector(1.-Fraction.X,1.-Fraction.Y,Fraction.Z);
                else if(Cell.SourceYawQuarter==3)SourceFraction=FVector(1.-Fraction.Y,Fraction.X,Fraction.Z);
                const FVector Source=(FVector(Cell.SourceCell)+SourceFraction)*.1;
                S.UV.Add(Cell.Approved?(SourceAxis==2?FVector2D(Source.X,Source.Y):SourceAxis==0?FVector2D(Source.Y,Source.Z):FVector2D(Source.X,Source.Z)):FVector2D(CU[Corner]*.1,CV[Corner]*.1));
            }
            if(Sign)S.I.Append({First,First+1,First+2,First,First+2,First+3});else S.I.Append({First,First+2,First+1,First,First+3,First+2});
        }
    }
    auto Material=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Voxel/M_VoxelDetailAsset.M_VoxelDetailAsset"));
    for(int Flag=0;Flag<5;++Flag){auto& S=Sections[Flag];if(S.P.IsEmpty())continue;
        auto Mesh=NewObject<UProceduralMeshComponent>(this);AddInstanceComponent(Mesh);Mesh->SetupAttachment(PhysicsBody);
        Mesh->SetMobility(EComponentMobility::Movable);Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);Mesh->SetCanEverAffectNavigation(false);Mesh->RegisterComponent();
        Mesh->CreateMeshSection_LinearColor(0,S.P,S.I,S.N,S.UV,S.Wind,TArray<FVector2D>(),TArray<FVector2D>(),S.C,S.T,false,true);
        if(Material){auto MID=UMaterialInstanceDynamic::Create(Material,Mesh);MID->SetScalarParameterValue(TEXT("TreeAppearance"),Flag<4?1.f:0.f);MID->SetScalarParameterValue(TEXT("TreeNeedle"),Flag<4&&(Flag&1)?1.f:0.f);MID->SetScalarParameterValue(TEXT("FoliageCutout"),Flag<4&&(Flag&2)?1.f:0.f);Mesh->SetMaterial(0,MID);}
        else UE_LOG(LogVoxelEarth,Error,TEXT("Debris approved appearance material missing"));
        AppearanceMeshes.Add(Mesh);
    }
}

void AVoxelDebris::EndPlay(const EEndPlayReason::Type Reason)
{
    VoxelDetachedPersistence::OnEndPlay(this,Reason);
    Super::EndPlay(Reason);
}
bool AVoxelDebris::CaptureObjectState(TSharedPtr<const TArray<uint8>,ESPMode::ThreadSafe>& Geometry,TArray<uint8>& Dynamic)
{
    check(IsInGameThread());
    if(!CachedObjectGeometry){
        auto Data=MakeShared<TArray<uint8>,ESPMode::ThreadSafe>();FMemoryWriter Ar(*Data);
        if(!GeometryState(Ar))return false;
        CachedObjectGeometry=Data;
    }
    Geometry=CachedObjectGeometry;Dynamic.Reset();FMemoryWriter Ar(Dynamic);
    FTransform Transform=GetActorTransform();auto Linear=PhysicsBody->GetPhysicsLinearVelocity();auto Angular=PhysicsBody->GetPhysicsAngularVelocityInRadians();
    auto Lifetime=Cleanup->CaptureState();uint8 Kind=uint8(Lifetime.Kind);
    bool Simulating=PhysicsBody->IsSimulatingPhysics(),TickEnabled=IsActorTickEnabled();
    Ar<<Transform<<Linear<<Angular<<bSettled<<AgeSeconds<<Kind<<Lifetime.RemainingSeconds<<Simulating<<TickEnabled;
    return !Ar.IsError();
}
bool AVoxelDebris::RestoreObjectState(const TArray<uint8>& Geometry,const TArray<uint8>& Dynamic)
{
    TArray<uint8> Legacy;Legacy.Reserve(Geometry.Num()+Dynamic.Num());Legacy.Append(Geometry);Legacy.Append(Dynamic);
    FMemoryReader Ar(Legacy);if(!PersistentState(Ar)||Ar.IsError())return false;
    if(Ar.Tell()<Ar.TotalSize()){
        bool Simulating=false,TickEnabled=false;Ar<<Simulating<<TickEnabled;
        if(Ar.IsError())return false;
        PhysicsBody->SetSimulatePhysics(Simulating);SetActorTickEnabled(TickEnabled);
    }
    return Ar.Tell()==Ar.TotalSize();
}
bool AVoxelDebris::PersistentState(FArchive& Ar)
{
    using namespace VoxelDetachedPersistence;
    if(!GeometryState(Ar))return false;
    FTransform Transform=GetActorTransform();
    FVector Linear=PhysicsBody->GetPhysicsLinearVelocity(),Angular=PhysicsBody->GetPhysicsAngularVelocityInRadians();
    bool Settled=bSettled;float Elapsed=AgeSeconds;
    auto Lifetime=Cleanup->CaptureState();uint8 Kind=uint8(Lifetime.Kind);
    Ar<<Transform<<Linear<<Angular<<Settled<<Elapsed<<Kind<<Lifetime.RemainingSeconds;
    if(Ar.IsError()||Transform.ContainsNaN()||Linear.ContainsNaN()||Angular.ContainsNaN()||!FMath::IsFinite(Elapsed)||Kind>3)return false;
    if(Ar.IsLoading())
    {
        if(PersistentVoxels.IsEmpty())return false;
        auto Voxels=PersistentVoxels;auto Cells=PersistentAppearance;const int32 Budget=PersistentInstanceBudget;
        if(Cells.IsEmpty())InitFromIsland(Voxels,Budget);else if(!InitFromIslandWithAppearance(Voxels,Cells,Budget))return false;
        SetActorTransform(Transform);AgeSeconds=Elapsed;bSettled=Settled;
        if(Settled){PhysicsBody->SetSimulatePhysics(false);SetActorTickEnabled(false);}
        else{PhysicsBody->SetPhysicsLinearVelocity(Linear);PhysicsBody->SetPhysicsAngularVelocityInRadians(Angular);}
        Lifetime.Kind=EVoxelDebrisLifetime(Kind);Cleanup->RestoreState(Lifetime);
    }
    return true;
}

void AVoxelDebris::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bSettled)
	{
		return;
	}

	UWorld* W = GetWorld();
	UVoxelWorldSubsystem* Sub = W ? W->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Sub)
	{
		return;
	}

	const FVector Loc = GetActorLocation();
	// Detached bodies can outlive terrain residency. Never turn an unloaded
	// tile into a ground hit at the fallback elevation. Pause the cheap island
	// proxy until the same terrain gate used by terrain generation is ready.
	if(auto Fine=Sub->GetFineTileStreamer()){
		const int64 X=FMath::FloorToInt64(Loc.X*10.),Y=FMath::FloorToInt64(Loc.Y*10.);
		if(!Fine->IsFootprintResident(X,Y,X+1,Y+1)){PhysicsBody->SetSimulatePhysics(false);return;}
	}
	if(!PhysicsBody->IsSimulatingPhysics())PhysicsBody->SetSimulatePhysics(true);
	AgeSeconds += DeltaSeconds;

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
	SetActorTickEnabled(false); // cleanup has its own timer; no idle terrain rays

	PhysicsBody->SetSimulatePhysics(false);
	FVector L = GetActorLocation();
	L.Z = SurfaceTopZUU + AabbHalfHeightUU; // rest the island's lowest face on the surface
	SetActorLocation(L);
	// Level the body so the voxel cluster sits flat on rest (cosmetic).
	SetActorRotation(FRotator::ZeroRotator);

	UE_LOG(LogVoxelEarth, Log, TEXT("VoxelDebris settled: voxels=%d rest=(%.0f,%.0f,%.0f) after %.2fs"), VoxelCount, L.X,
	       L.Y, L.Z, AgeSeconds);
}
