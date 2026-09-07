#include "VoxelEnvironmentLODPrototype.h"
#include "Async/Async.h"
#include "VoxelPlantMeshComponent.h"
#include "VoxelVegetationRender.h"
#include "VoxelTreeFellingPrototype.h"
#include "VoxelDetachedPersistence.h"
#include "Components/BoxComponent.h"
#include "VoxelWorldSubsystem.h"
#include "VoxelEarthFlyPawn.h"
#include "VoxelMovementTuning.h"
#include "VoxelEarth.h"
#include "Components/StaticMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/IConsoleManager.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "voxelcore/assetgrid.h"
#include "VoxelEnvironmentSparseGrid.h"
#include "voxelcore/hash.h"
#include "voxelcore/assetslopefit.h"
#include "voxelcore/materialpalette.h"
#include "voxelcore/raycast.h"

namespace {
using FPrototypeGrid=FVoxelEnvironmentSparseGrid;
FPrototypeGrid Reduce(const FPrototypeGrid& In) {
    FPrototypeGrid Out;Out.Mm=In.Mm*2;
    for(int A=0;A<3;++A){Out.Origin[A]=int32(vxc::floorDiv(In.Origin[A],2));Out.Size[A]=int32(vxc::floorDiv(int64(In.Origin[A])+In.Size[A]+1,2))-Out.Origin[A];}
    if(!Out.Init())return Out;
    // Only parent chunks touched by occupied source chunks can contain material.
    TSet<FIntVector> Candidates;
    In.Data.visitChunks([&](int32 X,int32 Y,int32 Z,auto){
        if(Z*8>=In.MaxDataZ)return;
        FIntVector Lo,Hi;
        const FIntVector Base(X*8,Y*8,Z*8);
        for(int A=0;A<3;++A){Lo[A]=(int32(vxc::floorDiv(Base[A]+In.Origin[A],2))-Out.Origin[A])/8;Hi[A]=(int32(vxc::floorDiv(FMath::Min(Base[A]+7,In.Size[A]-1)+In.Origin[A],2))-Out.Origin[A])/8;}
        for(int CX=Lo.X;CX<=Hi.X;++CX)for(int CY=Lo.Y;CY<=Hi.Y;++CY)for(int CZ=Lo.Z;CZ<=Hi.Z;++CZ)Candidates.Add(FIntVector(CX,CY,CZ));
    });
    auto Keys=Candidates.Array();Keys.Sort([](const auto& A,const auto& B){return A.X!=B.X?A.X<B.X:A.Y!=B.Y?A.Y<B.Y:A.Z<B.Z;});
    for(const auto& Key:Keys){
        vxc::SparseAssetGrid::Edit Edits[512];size_t Count=0;
        for(int Z=Key.Z*8;Z<FMath::Min(Key.Z*8+8,Out.Size.Z);++Z)
        for(int Y=Key.Y*8;Y<FMath::Min(Key.Y*8+8,Out.Size.Y);++Y)
        for(int X=Key.X*8;X<FMath::Min(Key.X*8+8,Out.Size.X);++X){
            // Upper detached layers have already been reclaimed from storage.
            const uint8 M=In.Data.environmentReducedAtAnchor2(X+Out.Origin.X,Y+Out.Origin.Y,Z+Out.Origin.Z);
            if(M)Edits[Count++]={X,Y,Z,M};
        }
        if(Out.Data.apply({Edits,Count})!=vxc::SparseAssetGrid::Result::Ok){Out.Size=FIntVector::ZeroValue;return Out;}
    }
    return Out;
}
// Admission applies to the complete hierarchy, not independently to each LOD.
// Geometry arrays use about 296 bytes/face before allocator/component copies.
constexpr int64 MaxHierarchyChunks=262144; // 128 MiB material payload
constexpr int64 MaxHierarchyFaces=1000000;
constexpr int32 MaxHierarchySections=8192;
bool PrepareHierarchy(FPrototypeGrid&& Source,TArray<FPrototypeGrid>& Out) {
    Out.Reset();Out.Add(MoveTemp(Source));
    while(Out.Last().Mm<100){
        auto Coarse=Reduce(Out.Last());if(Coarse.Size.GetMin()<=0)return false;Out.Add(MoveTemp(Coarse));
    }
    int64 Chunks=0,Faces=0;int32 Sections=0;
    static const FIntVector Neighbors[]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for(const auto& G:Out){
        Chunks+=int64(G.Data.chunkCount());Sections+=G.SectionKeys(G.Mm==25?16:32).Num();
        if(Chunks>MaxHierarchyChunks||Sections>MaxHierarchySections)return false;
        G.Visit([&](int X,int Y,int Z,uint8){if(Faces>MaxHierarchyFaces)return;for(const auto& N:Neighbors)Faces+=!G.At(X+N.X,Y+N.Y,Z+N.Z);});
        if(Faces>MaxHierarchyFaces)return false;
    }
    return true;
}
TArray<TWeakObjectPtr<AVoxelEnvironmentLODPrototype>> Actors;
FString RootDir(){return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()/TEXT("../asset-forge/out/environment-lod-prototype"));}

// One placement-time footprint fit, before grids become render/collision data.
// Keep the actor upright on the shared cubic lattice. Quarter-turn yaw is
// applied to both support probes and authoritative query/edit coordinates.
bool SeatPrototypeAsset(AVoxelEnvironmentLODPrototype* Actor,const FPrototypeGrid& Grid,const FString& Name) {
    auto Terrain=Actor->GetWorld()->GetSubsystem<UVoxelWorldSubsystem>();
    if(!Terrain)return false;
    const double Start=FPlatformTime::Seconds();
    const bool Tree=Actor->GetAssetDescriptor().Kind==TEXT("tree"),Rock=Actor->GetAssetDescriptor().Kind==TEXT("rock");
    const bool Bush=Actor->GetAssetDescriptor().Kind==TEXT("bush");
    const double HeightMm=Grid.Size.Z*Grid.Mm;
    // Soft plants anchor by the true lowest layer; elevated foliage is not a
    // ground contact. Woody root spread and rock seating use broader bands.
    const double BandMm=Tree?500.:Rock?HeightMm*.3:0.;
    const double SinkMm=Tree?800.:Rock?HeightMm*.45:Bush?150.:50.;
    const double BurialMm=Tree?1000.:Rock?HeightMm*.65:Bush?300.:100.;
    const double LatticeMm=(Tree||Rock)?100.:Grid.Mm;
    auto IsSupport=[Tree](uint8 M){return Tree?(M==16||M==17||M==18||M==23):M!=0;};
    TArray<FVector> Feet;
    double LowestMm=DBL_MAX;
    TMap<FIntPoint,int32> Lowest;
    Grid.Visit([&](int X,int Y,int Z,uint8 M){if(IsSupport(M)){auto& Floor=Lowest.FindOrAdd(FIntPoint(X,Y),MAX_int32);Floor=FMath::Min(Floor,Z);}});
    for(const auto& Pair:Lowest){const auto XY=Pair.Key;const int Z=Pair.Value;
        FVector Foot((XY.X+Grid.Origin.X)*Grid.Mm,(XY.Y+Grid.Origin.Y)*Grid.Mm,(Z+Grid.Origin.Z)*Grid.Mm);
        Feet.Add(Foot);LowestMm=FMath::Min(LowestMm,Foot.Z);
    }
    if(Feet.IsEmpty())return false;
    double MinX=DBL_MAX,MinY=DBL_MAX,MaxX=-DBL_MAX,MaxY=-DBL_MAX;
    for(const auto& P:Feet)if(P.Z<=LowestMm+BandMm){
        MinX=FMath::Min(MinX,P.X);MinY=FMath::Min(MinY,P.Y);
        MaxX=FMath::Max(MaxX,P.X+Grid.Mm);MaxY=FMath::Max(MaxY,P.Y+Grid.Mm);
    }
    double Bottom[16];for(double& B:Bottom)B=DBL_MAX;
    // Include every bin touched by a support cell (not just its centre).
    for(const auto& P:Feet)if(P.Z<=LowestMm+BandMm){
        const int X0=FMath::Clamp(FMath::FloorToInt((P.X-MinX)*4/(MaxX-MinX)),0,3);
        const int Y0=FMath::Clamp(FMath::FloorToInt((P.Y-MinY)*4/(MaxY-MinY)),0,3);
        const int X1=FMath::Clamp(FMath::FloorToInt((P.X+Grid.Mm-MinX)*4/(MaxX-MinX)),0,3);
        const int Y1=FMath::Clamp(FMath::FloorToInt((P.Y+Grid.Mm-MinY)*4/(MaxY-MinY)),0,3);
        for(int X=X0;X<=X1;++X)for(int Y=Y0;Y<=Y1;++Y)Bottom[X*4+Y]=FMath::Min(Bottom[X*4+Y],P.Z);
    }
    const FVector Initial=Actor->GetActorLocation();
    // Fixed candidate order gives reproducible relocation, at most 2.83 m.
    const FIntPoint Offsets[]={{0,0},{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,1},{1,-1},{-1,-1}};
    int Samples=0;
    for(int Candidate=0;Candidate<UE_ARRAY_COUNT(Offsets);++Candidate){
        FVector Site=Initial+FVector(Offsets[Candidate].X*200.,Offsets[Candidate].Y*200.,0);
        double Ground[25];FTransform ProbeTransform=Actor->GetActorTransform();ProbeTransform.SetLocation(Site);
        for(int X=0;X<5;++X)for(int Y=0;Y<5;++Y){
            const FVector Probe=VoxelEnvironmentAsset::WorldPosition(ProbeTransform,FVector((MinX+(MaxX-MinX)*X/4)*.1,(MinY+(MaxY-MinY)*Y/4)*.1,0));
            Ground[X*5+Y]=Terrain->GetSurfaceHeightUU(Probe.X,Probe.Y)*10.;++Samples;
        }
        vxc::AssetSupportBin Bins[16];int Count=0;
        for(int X=0;X<4;++X)for(int Y=0;Y<4;++Y)if(Bottom[X*4+Y]!=DBL_MAX){
            auto& B=Bins[Count++];B.bottomMm=Bottom[X*4+Y];B.groundLowMm=DBL_MAX;B.groundHighMm=-DBL_MAX;
            for(int DX=0;DX<2;++DX)for(int DY=0;DY<2;++DY){
                const double H=Ground[(X+DX)*5+Y+DY];B.groundLowMm=FMath::Min(B.groundLowMm,H);B.groundHighMm=FMath::Max(B.groundHighMm,H);
            }
        }
        // Footprint centre is used as the fresh baseline; no accumulated sink
        // on Reset. A 25 mm root embed leaves room for terrain quantization.
        const auto Fit=vxc::fitAssetSupports(Bins,Count,Ground[12]-LowestMm,LatticeMm,SinkMm,BurialMm,Tree?25.:0.);
        if(!Fit.accepted)continue;
        Site.Z=Fit.originZMm*.1;Actor->SetActorLocation(Site);
        UE_LOG(LogVoxelEarth,Log,TEXT("EnvironmentSlopeFit ACCEPT %s candidate=%d samples=%d sinkMm=%.0f burialMm=%.0f position=(%.0f,%.0f,%.0f) ms=%.3f"),
            *Name,Candidate,Samples,Fit.sinkMm,Fit.maximumBurialMm,Site.X,Site.Y,Site.Z,(FPlatformTime::Seconds()-Start)*1000.);
        return true;
    }
    UE_LOG(LogVoxelEarth,Warning,TEXT("EnvironmentSlopeFit REFUSE %s samples=%d reason=unsupported-or-excessive-burial ms=%.3f"),*Name,Samples,(FPlatformTime::Seconds()-Start)*1000.);
    return false;
}
}
struct FEnvironmentLODState {
    TArray<FPrototypeGrid> Grids;
    TArray<TMap<FIntVector,UProceduralMeshComponent*>> SectionMaps;
    bool Collision=false; int32 Revision=0;
    int32 LastSections=0, LastFaces=0;
    TArray<int32> WoodPerLayer;
    bool Severed=false, AxeCut=false;
    FVector LastChopDirection=FVector::ForwardVector;
};
namespace {
struct FEnvironmentLodMeshGeometry
{
	uint32 MeshKey = 0;
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector3f> TangentsX;
	TArray<FVector4f> Colors; // LINEAR floats -- see the colour note up top
	TArray<FVector2f> UVs;
	TArray<FVector2f> WindUVs;
	TArray<uint32> Indices;
	uint64 SolidVoxels = 0;
};

struct FEnvironmentLodPaletteLinear
{
	FLinearColor Face[vxc::kMaterialCount][vxc::kFaceClassCount];
	uint8 Jitter[vxc::kMaterialCount];
	FEnvironmentLodPaletteLinear()
	{
		for (uint32 M = 0; M < uint32(vxc::kMaterialCount); ++M)
		{
			const vxc::MaterialAppearance& A = vxc::kMaterialPalette[M];
			for (uint32 F = 0; F < uint32(vxc::kFaceClassCount); ++F)
			{
				// FLinearColor(FColor) is the exact sRGB decode.
				Face[M][F] = FLinearColor(FColor(A.face[F].r, A.face[F].g, A.face[F].b));
			}
			Jitter[M] = A.voxelJitter;
		}
	}
};

const FEnvironmentLodPaletteLinear& EnvironmentLodPaletteLinear()
{
	static const FEnvironmentLodPaletteLinear P; // thread-safe magic-static init
	return P;
}

void BuildNaiveFaceGeometry(const FPrototypeGrid& Grid, uint32 MeshKey, FEnvironmentLodMeshGeometry& Out, const FIntVector& Min, const FIntVector& Max)
{
	const int32 SX = Grid.sizeX(), SY = Grid.sizeY(), SZ = Grid.sizeZ();
	if (SX <= 0 || SY <= 0 || SZ <= 0)
	{
		return;
	}

    // Read the authoritative grid directly, including neighbors outside the
    // section. Internal section boundaries must never emit hidden faces.
    uint64 Solid=0;
    Grid.VisitBox(Min,Max,[&](int,int,int,uint8){++Solid;});
    Out.SolidVoxels=Solid;
    if(!Solid)return;
    auto MatAt=[&](int32 X,int32 Y,int32 Z){return Grid.At(X,Y,Z);};

	// THE DETAIL GRID'S OWN PITCH, not the world's. assetgrid.h: "Read it
	// before placing anything ... at() deliberately does NOT scale by it."
	// This is the scale read; a 5 cm tuft renders at 5 cm.
	const float PitchUU = float(double(Grid.voxelSizeMm()) * 0.1);
	const int32 Origin[3] = {Grid.originX(), Grid.originY(), Grid.originZ()};

	const FEnvironmentLodPaletteLinear& Pal = EnvironmentLodPaletteLinear();
	const uint64 JitterSeed = (uint64(MeshKey) << 1) | 1u;

	static const FVector3f AxisDir[3] = {FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, 0, 1)};

	// Rough reserve: ~4.5 visible faces per solid voxel is a generous upper
	// bound for sparse foliage; TArray growth handles the rest.
	const int32 ReserveFaces = int32(FMath::Min<uint64>(Solid * 5u, 200000u));
	Out.Positions.Reserve(ReserveFaces * 4);
	Out.Normals.Reserve(ReserveFaces * 4);
	Out.TangentsX.Reserve(ReserveFaces * 4);
	Out.Colors.Reserve(ReserveFaces * 4);
	Out.UVs.Reserve(ReserveFaces * 4);
	Out.WindUVs.Reserve(ReserveFaces * 4);
	Out.Indices.Reserve(ReserveFaces * 6);

    Grid.VisitBox(Min,Max,[&](int32 X,int32 Y,int32 Z,uint8 M){
                int32 Cell[3]={X,Y,Z};

				// Per-voxel lightness jitter, hashed from LOCAL coordinates +
				// the (species, seed) identity -- deterministic, and keyed to
				// the VOXEL so all six faces of one cube agree
				// (materialpalette.h's "keyed to the voxel and not the face").
				const double J =
					double(vxc::hashToSigned16(vxc::hash3(JitterSeed, Cell[0], Cell[1], Cell[2], 0))) /
					32768.0;
				const float Gain =
					1.0f + float(J) * 0.35f * (float(Pal.Jitter[M]) / 255.0f);

				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					for (int32 Positive = 0; Positive < 2; ++Positive)
					{
						int32 N[3] = {Cell[0], Cell[1], Cell[2]};
						N[Axis] += Positive ? 1 : -1;
						if (MatAt(N[0], N[1], N[2]) != 0 &&
                            !(VoxelVegetationRender::IsWood(M) && VoxelVegetationRender::IsLeaf(MatAt(N[0], N[1], N[2]))))
						{
							continue; // face culled against a solid neighbour
						}

						const int32 U = (Axis + 1) % 3;
						const int32 V = (Axis + 2) % 3;
						const vxc::FaceClass FC =
							(Axis == 2) ? (Positive ? vxc::kFaceTop : vxc::kFaceBottom)
							            : vxc::kFaceSide;
						FLinearColor C = Pal.Face[M][FC] * Gain;
						C.R = FMath::Clamp(C.R, 0.0f, 1.0f);
						C.G = FMath::Clamp(C.G, 0.0f, 1.0f);
						C.B = FMath::Clamp(C.B, 0.0f, 1.0f);
						C.A = 1.0f;

						const float FaceCoord = float(Cell[Axis] + Positive);
						const float U0 = float(Cell[U]), U1 = float(Cell[U] + 1);
						const float V0 = float(Cell[V]), V1 = float(Cell[V] + 1);
						const float CornerU[4] = {U0, U0, U1, U1};
						const float CornerV[4] = {V0, V1, V1, V0};

						const FVector3f Normal = AxisDir[Axis] * (Positive ? 1.0f : -1.0f);
						const FVector3f TangentX = AxisDir[U];
						const uint32 Base = uint32(Out.Positions.Num());

						for (int32 Corner = 0; Corner < 4; ++Corner)
						{
							FVector3f P;
							P[Axis] = (FaceCoord + float(Origin[Axis])) * PitchUU;
							P[U] = (CornerU[Corner] + float(Origin[U])) * PitchUU;
							P[V] = (CornerV[Corner] + float(Origin[V])) * PitchUU;
							Out.Positions.Add(P);
							Out.Normals.Add(Normal);
							Out.TangentsX.Add(TangentX);
							Out.Colors.Add(FVector4f(C.R, C.G, C.B, VoxelVegetationRender::MaterialClass(M)));
                            Out.WindUVs.Add(VoxelVegetationRender::WindData(P.Z, float(Origin[2] + SZ) * PitchUU));
							// The material samples no texture; a stable planar
							// UV keeps every downstream assumption (non-zero
							// UV channel, finite derivatives) honest.
							Out.UVs.Add(FVector2f(CornerU[Corner], CornerV[Corner]) *
							            (PitchUU / 100.0f));
						}

						if (Positive)
						{
							Out.Indices.Append({Base + 0, Base + 1, Base + 2, Base + 0, Base + 2, Base + 3});
						}
						else
						{
							Out.Indices.Append({Base + 0, Base + 2, Base + 1, Base + 0, Base + 3, Base + 2});
						}
					}
				}
    });
}


void ApplyGeometry(UProceduralMeshComponent* Component,const FEnvironmentLodMeshGeometry& Geometry){
        TArray<FVector> Positions,Normals;TArray<FVector2D> UVs,WindUVs;
        TArray<FLinearColor> Colors;TArray<FProcMeshTangent> Tangents;TArray<int32> Indices;
        const int32 Count=Geometry.Positions.Num();
        Positions.Reserve(Count);Normals.Reserve(Count);UVs.Reserve(Count);Colors.Reserve(Count);Tangents.Reserve(Count);
        for(int I=0;I<Count;++I){
            Positions.Add(FVector(Geometry.Positions[I]));Normals.Add(FVector(Geometry.Normals[I]));
            UVs.Add(FVector2D(Geometry.UVs[I]));WindUVs.Add(Geometry.WindUVs.IsValidIndex(I)?FVector2D(Geometry.WindUVs[I]):FVector2D::ZeroVector);const auto C=Geometry.Colors[I];Colors.Add(FLinearColor(C.X,C.Y,C.Z,C.W));
            Tangents.Add(FProcMeshTangent(FVector(Geometry.TangentsX[I]),false));
        }
        Indices.Reserve(Geometry.Indices.Num());for(auto I:Geometry.Indices)Indices.Add(int32(I));
        // Match StaticMesh's sRGB vertex-color packing: the existing material
        // decodes that packed color. Linear byte packing would darken the asset.
        Component->CreateMeshSection_LinearColor(0,Positions,Indices,Normals,UVs,WindUVs,TArray<FVector2D>(),TArray<FVector2D>(),Colors,Tangents,false,true);
}


} // namespace
struct FEnvironmentStagedRestore {
    struct FSection {int32 Level=0;FIntVector Key;FEnvironmentLodMeshGeometry Mesh;};
    TAtomic<bool> Cancelled{false};
    FVoxelImmutableGeometry Geometry;
    TFunction<void(bool)> Completion;
    TArray<FPrototypeGrid> Grids;
    TArray<int32> WoodPerLayer;
    TArray<FSection> Meshes;
    FTransform Transform;
    FVoxelEnvironmentAssetDescriptor Descriptor;
    int32 Next=0;
    bool Collision=false,Severed=false,Ready=false,Valid=false,Adopted=false;
};
AVoxelEnvironmentLODPrototype::AVoxelEnvironmentLODPrototype() {
    PrimaryActorTick.bCanEverTick=true;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
    State=MakeUnique<FEnvironmentLODState>();
}
AVoxelEnvironmentLODPrototype::~AVoxelEnvironmentLODPrototype()=default;
AVoxelEnvironmentLODPrototype::AVoxelEnvironmentLODPrototype(FVTableHelper& Helper):Super(Helper) {}
bool AVoxelEnvironmentLODPrototype::InitializeAsset(const FString& Name,int32 FinestMm,bool Collision) {
    TArray<uint8> Vxa;const FString Path=RootDir()/FString::Printf(TEXT("%s-%dmm.vxa"),*Name,FinestMm);
    vxc::AssetGrid Source;
    if(!FFileHelper::LoadFileToArray(Vxa,*Path)||Source.parse(Vxa.GetData(),Vxa.Num())!=vxc::AssetParseError::kOk||Source.voxelSizeMm()!=FinestMm)return false;
    return InitializeAssetFromVxa(FVoxelEnvironmentAssetDescriptor::Prototype(Name),Vxa,Collision,true);
}
bool AVoxelEnvironmentLODPrototype::InitializeAssetFromVxa(FVoxelEnvironmentAssetDescriptor Descriptor,const TArray<uint8>& Vxa,bool Collision,bool FitTerrain) {
    if(StagedRestore||PreparedRestore||Vxa.IsEmpty()||!VoxelEnvironmentAsset::IsSupportedTransform(GetActorTransform()))return false;
    vxc::AssetGrid Source;if(Source.parse(Vxa.GetData(),Vxa.Num())!=vxc::AssetParseError::kOk)return false;
    const double FinestMm=Source.voxelSizeMm();if(FinestMm!=25&&FinestMm!=50&&FinestMm!=100)return false;
    const FString Hash=FMD5::HashBytes(Vxa.GetData(),Vxa.Num());
    if(!Descriptor.Legacy){if(!Descriptor.SourceHash.IsEmpty()&&!Descriptor.SourceHash.Equals(Hash,ESearchCase::IgnoreCase))return false;Descriptor.SourceHash=Hash;}
    if(!Descriptor.IsValid())return false;
    FPrototypeGrid Grid;Grid.Size=FIntVector(Source.sizeX(),Source.sizeY(),Source.sizeZ());
    Grid.Origin=FIntVector(Source.originX(),Source.originY(),Source.originZ());Grid.Mm=FinestMm;
    const int64 Cells=int64(Grid.Size.X)*Grid.Size.Y*Grid.Size.Z;
    if(Grid.Size.GetMin()<=0||Grid.Size.GetMax()>FPrototypeGrid::MaxAxis||Cells<=0)return false;
    for(int Axis=0;Axis<3;++Axis)if(FMath::Abs(int64(Grid.Origin[Axis]))>1000000)return false;
    if(!Grid.Init())return false;
    bool Imported=true;
    for(int X=0;X<Grid.Size.X;++X)for(int Y=0;Y<Grid.Size.Y;++Y)
        Source.columnRuns(X,Y,[&](int Z,int Len,vxc::MaterialId M){if(M&&Imported)Imported=Grid.SetRun(X,Y,Z,Len,uint8(M));});
    if(!Imported)return false;
    TArray<FPrototypeGrid> Prepared;if(!PrepareHierarchy(MoveTemp(Grid),Prepared))return false;
    const auto& PreparedFine=Prepared[0];
    const auto PreviousDescriptor=SourceDescriptor;const FString PreviousName=AssetName;
    SourceDescriptor=MoveTemp(Descriptor);AssetName=SourceDescriptor.SpecId;
    if(FitTerrain&&!SeatPrototypeAsset(this,PreparedFine,AssetName)){SourceDescriptor=PreviousDescriptor;AssetName=PreviousName;return false;}
    GeometrySnapshot.Reset();State->Collision=Collision;State->Revision=0;State->Severed=false;
    State->WoodPerLayer.Init(0,PreparedFine.Size.Z);
    PreparedFine.Visit([&](int,int,int Z,uint8 M){if(M==16||M==17||M==18||M==23)++State->WoodPerLayer[Z];});
    State->Grids=MoveTemp(Prepared);
    Rebuild();Actors.AddUnique(this);
    return !Levels.IsEmpty();
}
void AVoxelEnvironmentLODPrototype::EndPlay(const EEndPlayReason::Type Reason) {
    CancelStagedObjectRestore();
    VoxelDetachedPersistence::OnEndPlay(this,Reason);
    Actors.Remove(this);Super::EndPlay(Reason);
}
bool AVoxelEnvironmentLODPrototype::PersistentState(FArchive& Ar) {
    if(StagedRestore||PreparedRestore){if(Ar.IsLoading())CancelStagedObjectRestore();return false;}
    auto Descriptor=SourceDescriptor;
    FPrototypeGrid Grid;if(Ar.IsSaving()){
        if(State->Grids.IsEmpty())return false;const auto& Source=State->Grids[0];
        Grid.Size=Source.Size;Grid.Origin=Source.Origin;Grid.Mm=Source.Mm;Grid.MaxDataZ=Source.MaxDataZ;
    }
    FTransform Transform=GetActorTransform();bool Collision=State->Collision,Severed=State->Severed;
    if(!VoxelEnvironmentAsset::SerializeIdentity(Ar,Descriptor))return false;
    Ar<<Transform<<Grid.Size<<Grid.Origin<<Grid.Mm<<Grid.MaxDataZ<<Collision<<Severed;
    if(Ar.IsError()||!VoxelEnvironmentAsset::IsSupportedTransform(Transform)||Grid.Size.X<=0||Grid.Size.Y<=0||Grid.Size.Z<=0||Grid.Size.GetMax()>FPrototypeGrid::MaxAxis)return false;
    if(Grid.MaxDataZ<0||(Grid.MaxDataZ>Grid.Size.Z&&Grid.MaxDataZ!=MAX_int32))return false;
    for(int32 Axis=0;Axis<3;++Axis)if(FMath::Abs(int64(Grid.Origin[Axis]))>1000000)return false;
    if((Grid.Mm!=25&&Grid.Mm!=50&&Grid.Mm!=100))return false;
    if(Grid.MaxDataZ<0||(Grid.MaxDataZ>Grid.Size.Z&&Grid.MaxDataZ!=MAX_int32))return false;
    for(int32 Axis=0;Axis<3;++Axis)if(FMath::Abs(int64(Grid.Origin[Axis]))>1000000)return false;
    if(!(Ar.IsSaving()?State->Grids[0].Serialize(Ar):Grid.Serialize(Ar)))return false;
    if(Ar.IsLoading()){
        TArray<FPrototypeGrid> Prepared;if(!PrepareHierarchy(MoveTemp(Grid),Prepared))return false;
        SourceDescriptor=MoveTemp(Descriptor);AssetName=SourceDescriptor.SpecId;SetActorTransform(Transform);State->Collision=Collision;State->Severed=Severed;
        State->Grids=MoveTemp(Prepared);auto& G=State->Grids[0];
        State->WoodPerLayer.Init(0,G.Size.Z);
        G.Visit([&](int,int,int Z,uint8 M){if(M==16||M==17||M==18||M==23)++State->WoodPerLayer[Z];});
        Rebuild();Actors.AddUnique(this);return !Levels.IsEmpty();
    }
    return true;
}
bool AVoxelEnvironmentLODPrototype::RefreshGeometrySnapshot(){
    check(IsInGameThread());GeometrySnapshot.Reset();
    if(State->Grids.IsEmpty())return false;
    const double Started=FPlatformTime::Seconds();
    auto Bytes=MakeShared<TArray<uint8>,ESPMode::ThreadSafe>();
    FMemoryWriter Writer(*Bytes);VoxelObjectGeometrySnapshot::WriteVersion(Writer);
    // Serialize occupied source chunks directly. No derived LODs enter the save.
    if(!State->Grids[0].Serialize(Writer)||Writer.IsError())return false;
    GeometrySnapshot=Bytes;
    UE_LOG(LogVoxelEarth,Verbose,TEXT("ObjectGeometry CACHE environment %s revision=%d bytes=%d ms=%.3f"),*AssetName,State->Revision,Bytes->Num(),(FPlatformTime::Seconds()-Started)*1000.);
    return true;
}
bool AVoxelEnvironmentLODPrototype::CaptureObjectState(FVoxelImmutableGeometry& Geometry,TArray<uint8>& Dynamic){
    check(IsInGameThread());Geometry.Reset();Dynamic.Reset();
    if(StagedRestore||PreparedRestore||!GeometrySnapshot||State->Grids.IsEmpty())return false;
    auto Descriptor=SourceDescriptor;if(!Descriptor.IsValid())return false;
    auto& Grid=State->Grids[0];FTransform Transform=GetActorTransform();
    bool Collision=State->Collision,Severed=State->Severed;
    if(!VoxelEnvironmentAsset::IsSupportedTransform(Transform))return false;
    FMemoryWriter Writer(Dynamic);VoxelObjectGeometrySnapshot::WriteVersion(Writer);
    // This is exactly the small header preceding Bytes(Grid.Data) in the
    // legacy reader. Geometry generation and capture run atomically on GT.
    if(!VoxelEnvironmentAsset::SerializeIdentity(Writer,Descriptor))return false;
    Writer<<Transform<<Grid.Size<<Grid.Origin<<Grid.Mm<<Grid.MaxDataZ<<Collision<<Severed;
    if(Writer.IsError())return false;Geometry=GeometrySnapshot;return true;
}
bool AVoxelEnvironmentLODPrototype::RestoreObjectState(const TArray<uint8>& Geometry,const TArray<uint8>& Dynamic){
    check(IsInGameThread());TArray<uint8> Legacy;
    if(!VoxelObjectGeometrySnapshot::Combine(Geometry,Dynamic,Legacy))return false;
    FMemoryReader Reader(Legacy);
    return PersistentState(Reader)&&!Reader.IsError()&&Reader.Tell()==Reader.TotalSize();
}
void AVoxelEnvironmentLODPrototype::CancelStagedObjectRestore(){
    check(IsInGameThread());auto Job=StagedRestore?MoveTemp(StagedRestore):MoveTemp(PreparedRestore);if(!Job)return;
    Job->Cancelled.Store(true);
    if(Job->Adopted){
        for(auto C:Sections)if(IsValid(C))C->DestroyComponent();Sections.Reset();
        for(auto C:Levels)if(IsValid(C))C->DestroyComponent();Levels.Reset();Materials.Reset();
        State->Grids.Reset();State->SectionMaps.Reset();GeometrySnapshot.Reset();
    }
    auto Done=MoveTemp(Job->Completion);if(Done)Done(false);
}
void AVoxelEnvironmentLODPrototype::BeginStagedObjectRestore(FVoxelImmutableGeometry Geometry,TArray<uint8> Dynamic,TFunction<void(bool)> Completion){
    check(IsInGameThread());CancelStagedObjectRestore();
    // The bridge uses a fresh unpublished actor. Keeping old active geometry
    // on a replacement actor is the bridge's revision/ownership decision.
    if(!Levels.IsEmpty()||!Geometry||Geometry->Num()<4||Dynamic.Num()<4||Dynamic.Num()>4096||int64(Geometry->Num())+Dynamic.Num()>VoxelObjectGeometrySnapshot::MaxBytes){if(Completion)Completion(false);return;}
    SetActorHiddenInGame(true);SetActorEnableCollision(false);SetActorTickEnabled(false);
    auto Job=MakeShared<FEnvironmentStagedRestore,ESPMode::ThreadSafe>();Job->Geometry=MoveTemp(Geometry);Job->Completion=MoveTemp(Completion);StagedRestore=Job;
    TWeakObjectPtr<AVoxelEnvironmentLODPrototype> Weak(this);
    Async(EAsyncExecution::ThreadPool,[Job,Weak,Dynamic=MoveTemp(Dynamic)](){
        auto Decode=[&](){
            FMemoryReader D(Dynamic),G(*Job->Geometry);uint32 DV=0,GV=0;D<<DV;G<<GV;
            if(DV!=1||GV!=1)return false;
            FPrototypeGrid Grid;
            if(!VoxelEnvironmentAsset::SerializeIdentity(D,Job->Descriptor))return false;
            D<<Job->Transform<<Grid.Size<<Grid.Origin<<Grid.Mm<<Grid.MaxDataZ<<Job->Collision<<Job->Severed;
            if(D.IsError()||D.Tell()!=D.TotalSize()||!VoxelEnvironmentAsset::IsSupportedTransform(Job->Transform)||Grid.Size.GetMin()<=0||Grid.Size.GetMax()>FPrototypeGrid::MaxAxis)return false;
            if((Grid.Mm!=25&&Grid.Mm!=50&&Grid.Mm!=100)||Grid.MaxDataZ<0||(Grid.MaxDataZ>Grid.Size.Z&&Grid.MaxDataZ!=MAX_int32))return false;
            for(int A=0;A<3;++A)if(FMath::Abs(int64(Grid.Origin[A]))>1000000)return false;
            if(!Grid.Serialize(G)||G.Tell()!=G.TotalSize())return false;
            Job->WoodPerLayer.Init(0,Grid.Size.Z);
            Grid.Visit([&](int,int,int Z,uint8 M){if(M==16||M==17||M==18||M==23)++Job->WoodPerLayer[Z];});
            if(Job->Cancelled.Load()||!PrepareHierarchy(MoveTemp(Grid),Job->Grids))return false;
            const uint32 MeshKey=GetTypeHash(Job->Descriptor.SpecId);
            for(int L=0;L<Job->Grids.Num();++L){const auto& Source=Job->Grids[L];const int Edge=Source.Mm==25?16:32;
                for(const auto& Key:Source.SectionKeys(Edge)){
                    const int X=Key.X*Edge,Y=Key.Y*Edge,Z=Key.Z*Edge;
                    if(Job->Cancelled.Load())return false;
                    FEnvironmentStagedRestore::FSection Section;Section.Level=L;Section.Key=FIntVector(X/Edge,Y/Edge,Z/Edge);Section.Mesh.MeshKey=MeshKey;
                    BuildNaiveFaceGeometry(Source,MeshKey,Section.Mesh,FIntVector(X,Y,Z),FIntVector(FMath::Min(X+Edge,Source.Size.X),FMath::Min(Y+Edge,Source.Size.Y),FMath::Min(Z+Edge,Source.Size.Z)));
                    if(!Section.Mesh.Indices.IsEmpty())Job->Meshes.Add(MoveTemp(Section));
                }
            }
            return !Job->Cancelled.Load();
        };
        const bool Valid=Decode();
        // No UObject access on the worker. Queue publication establishes the
        // handoff; payload arrays are never touched by the worker afterward.
        AsyncTask(ENamedThreads::GameThread,[Job,Weak,Valid](){if(auto A=Weak.Get())if(A->StagedRestore==Job&&!Job->Cancelled.Load()){Job->Valid=Valid;Job->Ready=true;}});
    });
}
bool AVoxelEnvironmentLODPrototype::AdvanceStagedObjectRestore(){
    check(IsInGameThread());auto Job=StagedRestore;if(!Job||!Job->Ready)return false;
    if(!Job->Valid){CancelStagedObjectRestore();return true;}
    if(!Job->Adopted){
        auto Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Voxel/M_VoxelEnvironmentLOD.M_VoxelEnvironmentLOD"));
        if(!Base){CancelStagedObjectRestore();return true;}
        SourceDescriptor=Job->Descriptor;AssetName=SourceDescriptor.SpecId;SetActorTransform(Job->Transform);State->Collision=Job->Collision;State->Severed=Job->Severed;
        State->Grids=MoveTemp(Job->Grids);State->WoodPerLayer=MoveTemp(Job->WoodPerLayer);State->SectionMaps.SetNum(State->Grids.Num());
        // At most three tiny LOD roots/materials; mesh buffers publish separately.
        for(int L=0;L<State->Grids.Num();++L){
            auto Root=NewObject<UStaticMeshComponent>(this);Root->SetupAttachment(GetRootComponent());Root->SetMobility(EComponentMobility::Movable);Root->SetCollisionEnabled(ECollisionEnabled::NoCollision);Root->SetVisibility(false);Root->RegisterComponent();Levels.Add(Root);
            auto Mat=UMaterialInstanceDynamic::Create(Base,this);Mat->SetScalarParameterValue(TEXT("Fade"),1);Mat->SetScalarParameterValue(TEXT("Reverse"),0);Materials.Add(Mat);
        }
        Job->Adopted=true;return true;
    }
    if(Job->Next<Job->Meshes.Num()){
        auto& S=Job->Meshes[Job->Next++];auto C=NewObject<UVoxelPlantMeshComponent>(this);C->SetupAttachment(Levels[S.Level]);C->SetMobility(EComponentMobility::Movable);C->SetCollisionEnabled(ECollisionEnabled::NoCollision);C->SetMaterial(0,Materials[S.Level]);C->SetVisibility(false);C->RegisterComponent();
        ApplyGeometry(C,S.Mesh);Sections.Add(C);State->SectionMaps[S.Level].Add(S.Key,C);S.Mesh=FEnvironmentLodMeshGeometry();return true;
    }
    GeometrySnapshot=Job->Geometry;ActiveLOD=0;PreviousLOD=-1;SetLevelVisible(0,true);
    PreparedRestore=Job;StagedRestore.Reset();
    // Visibility/collision publication belongs to the bridge after its final
    // revision check. An actor remains hidden until Completion accepts it.
    auto Done=MoveTemp(Job->Completion);if(Done)Done(true);return true;
}
bool AVoxelEnvironmentLODPrototype::PublishStagedObjectRestore(){
    check(IsInGameThread());if(!PreparedRestore)return false;
    PreparedRestore.Reset();Actors.AddUnique(this);SetActorHiddenInGame(false);SetActorEnableCollision(true);SetActorTickEnabled(true);return true;
}
void AVoxelEnvironmentLODPrototype::Rebuild() {
    GeometrySnapshot.Reset();
    if(State->Grids.IsEmpty())return;
    const double Start=FPlatformTime::Seconds();
    UMaterialInterface* Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Voxel/M_VoxelEnvironmentLOD.M_VoxelEnvironmentLOD"));
    if(!Base){UE_LOG(LogVoxelEarth,Error,TEXT("EnvironmentLOD missing dither material; prototype refused"));return;}
    for(auto C:Sections)C->DestroyComponent();Sections.Reset();
    for(auto C:Levels)C->DestroyComponent();Levels.Reset();Materials.Reset();
    State->SectionMaps.Empty();State->SectionMaps.SetNum(State->Grids.Num());
    State->LastSections=0;State->LastFaces=0;
    for(int32 L=0;L<State->Grids.Num();++L){
        auto Root=NewObject<UStaticMeshComponent>(this);
        Root->SetupAttachment(GetRootComponent());Root->SetMobility(EComponentMobility::Movable);
        Root->SetCollisionEnabled(ECollisionEnabled::NoCollision);Root->SetVisibility(false);Root->RegisterComponent();
        Levels.Add(Root);
        auto Material=UMaterialInstanceDynamic::Create(Base,this);
        Material->SetScalarParameterValue(TEXT("Fade"),1);Material->SetScalarParameterValue(TEXT("Reverse"),0);
        Materials.Add(Material);
        RebuildSections(L,FIntVector::ZeroValue,State->Grids[L].Size);
    }
    ActiveLOD=FMath::Clamp(ActiveLOD,0,Levels.Num()-1);PreviousLOD=-1;
    SetLevelVisible(ActiveLOD,true);
    RefreshGeometrySnapshot();
    UE_LOG(LogVoxelEarth,Log,TEXT("EnvironmentLOD rebuilt %s %.1f ms revision=%d sections=%d faces=%d"),*AssetName,(FPlatformTime::Seconds()-Start)*1000,State->Revision,State->LastSections,State->LastFaces);
}
void AVoxelEnvironmentLODPrototype::SetLevelVisible(int32 Level,bool Visible){
    Levels[Level]->SetVisibility(Visible,true);
}
void AVoxelEnvironmentLODPrototype::RebuildSections(int32 L,const FIntVector& Min,const FIntVector& Max){
    // Bound mesh updates independently of tree size; procedural buffers avoid
    // expensive StaticMesh/MeshDescription construction on every strike.
    const auto& Grid=State->Grids[L];
    const int32 Edge=Grid.Mm==25?16:32;
    if(Min.X>=Max.X||Min.Y>=Max.Y||Min.Z>=Max.Z)return;
    TSet<FIntVector> Candidates;
    for(const auto& Key:Grid.SectionKeys(Edge))Candidates.Add(Key);
    // Existing empty sections must still be removed after an edit.
    for(const auto& Pair:State->SectionMaps[L])Candidates.Add(Pair.Key);
    auto Keys=Candidates.Array();Keys.Sort([](const auto& A,const auto& B){return A.X!=B.X?A.X<B.X:A.Y!=B.Y?A.Y<B.Y:A.Z<B.Z;});
    for(const auto& Key:Keys){
        const FIntVector Lo=Key*Edge;
        if(Lo.X>=Max.X||Lo.Y>=Max.Y||Lo.Z>=Max.Z||Lo.X+Edge<=Min.X||Lo.Y+Edge<=Min.Y||Lo.Z+Edge<=Min.Z)continue;
        const FIntVector Hi(FMath::Min(Lo.X+Edge,Grid.Size.X),FMath::Min(Lo.Y+Edge,Grid.Size.Y),FMath::Min(Lo.Z+Edge,Grid.Size.Z));
        FEnvironmentLodMeshGeometry Geometry;Geometry.MeshKey=GetTypeHash(AssetName);
        BuildNaiveFaceGeometry(Grid,Geometry.MeshKey,Geometry,Lo,Hi);
        auto Existing=State->SectionMaps[L].Find(Key);
        if(Geometry.Indices.IsEmpty()){
            if(Existing){Sections.Remove(*Existing);(*Existing)->DestroyComponent();State->SectionMaps[L].Remove(Key);}
            continue;
        }
        UProceduralMeshComponent* Component=Existing?*Existing:nullptr;
        if(!Component){
            Component=NewObject<UVoxelPlantMeshComponent>(this);
            Component->SetupAttachment(Levels[L]);Component->SetMobility(EComponentMobility::Movable);
            Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Component->SetMaterial(0,Materials[L]);Component->SetVisibility(Levels[L]->IsVisible());
            Component->RegisterComponent();
            Sections.Add(Component);State->SectionMaps[L].Add(Key,Component);
        }
        ApplyGeometry(Component,Geometry);
        ++State->LastSections;State->LastFaces+=Geometry.Indices.Num()/6;
    }
}

void AVoxelEnvironmentLODPrototype::Tick(float DeltaSeconds) {
    Super::Tick(DeltaSeconds);if(Levels.IsEmpty())return;
    APlayerController* PC=GetWorld()->GetFirstPlayerController();if(!PC)return;
    FVector Eye;FRotator Rotation;PC->GetPlayerViewPoint(Eye,Rotation);
    const double Distance=FVector::Dist(Eye,GetActorLocation())/100.;
    // Provisional species-sized thresholds; collision is independent of them.
    const auto& Source=State->Grids[0];
    const double DiameterM=Source.Size.GetMax()*Source.Mm*.001;
    const double Near=FMath::Clamp(DiameterM*2.5,3.0,80.0);
    int32 Wanted=ActiveLOD;
    if(ForcedLOD>=0)Wanted=FMath::Clamp(ForcedLOD,0,Levels.Num()-1);
    else if(PreviousLOD<0){
        if(ActiveLOD==0&&Distance>Near*1.1)Wanted=1;
        else if(ActiveLOD==1&&Distance<Near*.9)Wanted=0;
        else if(Levels.Num()>2&&ActiveLOD==1&&Distance>Near*2.2)Wanted=2;
        else if(ActiveLOD==2&&Distance<Near*1.8)Wanted=1;
    }
    if(Wanted!=ActiveLOD&&PreviousLOD<0){
        PreviousLOD=ActiveLOD;ActiveLOD=Wanted;FadeTime=0;
        SetLevelVisible(ActiveLOD,true);
        Materials[PreviousLOD]->SetScalarParameterValue(TEXT("Reverse"),1);
        Materials[ActiveLOD]->SetScalarParameterValue(TEXT("Reverse"),0);
        UE_LOG(LogVoxelEarth,Log,TEXT("EnvironmentLOD transition %s %d->%d distance=%.2fm"),*AssetName,PreviousLOD,ActiveLOD,Distance);
    }
    if(PreviousLOD>=0){
        const double PreviousFade=FadeTime;FadeTime+=DeltaSeconds;const float Alpha=float(FMath::Clamp(FadeTime/.45,0.,1.));
        Materials[PreviousLOD]->SetScalarParameterValue(TEXT("Fade"),Alpha);
        Materials[ActiveLOD]->SetScalarParameterValue(TEXT("Fade"),Alpha);
        if(PreviousFade<.22&&FadeTime>=.22&&FadeTime<.45&&FParse::Param(FCommandLine::Get(),TEXT("VoxelEnvironmentLODCapture"))){
            FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir()/TEXT("Screenshots/EnvironmentLOD")/FString::Printf(TEXT("%s-blend-%d-%d.png"),*AssetName,PreviousLOD,ActiveLOD),false,false);
            UE_LOG(LogVoxelEarth,Log,TEXT("EnvironmentLOD BLEND %s alpha=%.3f"),*AssetName,Alpha);
        }
        if(Alpha>=1){SetLevelVisible(PreviousLOD,false);PreviousLOD=-1;}
    }
}
bool AVoxelEnvironmentLODPrototype::SolidAt(const FVector& WorldUU) const {
    if(!State->Collision||State->Grids.IsEmpty()||!VoxelEnvironmentAsset::IsSupportedTransform(GetActorTransform())||WorldUU.ContainsNaN())return false;
    const auto& Grid=State->Grids.Last();const FVector Local=VoxelEnvironmentAsset::LocalPosition(GetActorTransform(),WorldUU);
    return Grid.LocalAt(FMath::FloorToInt64(Local.X/10),FMath::FloorToInt64(Local.Y/10),FMath::FloorToInt64(Local.Z/10))!=0;
}
bool AVoxelEnvironmentLODPrototype::Trace(const FVector& Start,const FVector& Direction,double Range,FVector& Hit) const {
    if(State->Grids.IsEmpty()||!VoxelEnvironmentAsset::IsSupportedTransform(GetActorTransform())||Start.ContainsNaN()||Direction.ContainsNaN()||!FMath::IsFinite(Range)||Range<=0)return false;
    const auto& Grid=State->Grids[0];const double Pitch=Grid.Mm*.1;
    const FVector O=VoxelEnvironmentAsset::LocalPosition(GetActorTransform(),Start)*(100./Pitch),D=VoxelEnvironmentAsset::LocalVector(GetActorTransform(),Direction.GetSafeNormal())*(Range*100./Pitch);
    const auto H=vxc::raycastVoxels([&](int64 X,int64 Y,int64 Z){return vxc::MaterialId(Grid.LocalAt(X,Y,Z));},
        FMath::RoundToInt64(O.X),FMath::RoundToInt64(O.Y),FMath::RoundToInt64(O.Z),FMath::RoundToInt64(D.X),FMath::RoundToInt64(D.Y),FMath::RoundToInt64(D.Z));
    if(!H.hit)return false;
    Hit=VoxelEnvironmentAsset::WorldPosition(GetActorTransform(),FVector((H.vx+.5)*Pitch,(H.vy+.5)*Pitch,(H.vz+.5)*Pitch));return true;
}
bool AVoxelEnvironmentLODPrototype::GetDigBounds(const FVector& Hit,int32 SizeVoxels,FBox& Bounds) const {
    Bounds=FBox(ForceInit);
    if(State->Grids.IsEmpty()||SizeVoxels<1||SizeVoxels>4||Hit.ContainsNaN()||!VoxelEnvironmentAsset::IsSupportedTransform(GetActorTransform()))return false;
    const FVector Local=VoxelEnvironmentAsset::LocalPosition(GetActorTransform(),Hit);FVector Min;
    for(int Axis=0;Axis<3;++Axis)Min[Axis]=(FMath::FloorToInt(Local[Axis]/10)-SizeVoxels/2)*10.;
    const FVector Max=Min+FVector(SizeVoxels*10.);
    for(int X=0;X<2;++X)for(int Y=0;Y<2;++Y)for(int Z=0;Z<2;++Z)
        Bounds+=VoxelEnvironmentAsset::WorldPosition(GetActorTransform(),FVector(X?Max.X:Min.X,Y?Max.Y:Min.Y,Z?Max.Z:Min.Z));
    return true;
}
bool AVoxelEnvironmentLODPrototype::Carve(const FVector& Hit,int32 SizeVoxels) {
    if(StagedRestore||PreparedRestore){CancelStagedObjectRestore();return false;}
    if(State->Grids.IsEmpty()||SizeVoxels<1||SizeVoxels>4||Hit.ContainsNaN()||!VoxelEnvironmentAsset::IsSupportedTransform(GetActorTransform()))return false;
    auto& Grid=State->Grids[0];const double Pitch=Grid.Mm*.1;
    const FVector Local=VoxelEnvironmentAsset::LocalPosition(GetActorTransform(),Hit);FIntVector Lo,Hi;
    for(int A=0;A<3;++A){const int32 Coarse=FMath::FloorToInt(Local[A]/10)-SizeVoxels/2;
        Lo[A]=FMath::Max(0,Coarse*int32(100/Grid.Mm)-Grid.Origin[A]);
        Hi[A]=FMath::Min(Grid.Size[A],(Coarse+SizeVoxels)*int32(100/Grid.Mm)-Grid.Origin[A]);}
    Hi.Z=FMath::Min(Hi.Z,Grid.MaxDataZ);
    int32 Removed=0;
    TArray<vxc::SparseAssetGrid::Edit> Edits;
    Grid.VisitBox(Lo,Hi,[&](int X,int Y,int Z,uint8){Edits.Add({X,Y,Z,0});});
    int64 ExistingFaces=0;for(auto C:Sections)if(const auto S=C->GetProcMeshSection(0))ExistingFaces+=S->ProcIndexBuffer.Num()/6;
    // Conservative worst-case newly exposed faces across all derived levels.
    if(ExistingFaces+int64(Edits.Num())*6*State->Grids.Num()>MaxHierarchyFaces)return false;
    if(Grid.Data.apply({Edits.GetData(),size_t(Edits.Num())})!=vxc::SparseAssetGrid::Result::Ok)return false;
    Removed=Edits.Num();
    // Only changed layers need their wood count refreshed.
    for(int Z=Lo.Z;Z<Hi.Z;++Z)State->WoodPerLayer[Z]=0;
    Grid.VisitBox(FIntVector(0,0,Lo.Z),FIntVector(Grid.Size.X,Grid.Size.Y,Hi.Z),[&](int,int,int Z,uint8 M){if(M==16||M==17||M==18||M==23)++State->WoodPerLayer[Z];});
    if(!Removed)return false;
    GeometrySnapshot.Reset();
    const double EditStart=FPlatformTime::Seconds();
    ++State->Revision;State->LastSections=0;State->LastFaces=0;
    FIntVector DirtyLo=Lo,DirtyHi=Hi;
    for(int L=0;L<State->Grids.Num();++L){
        auto& Current=State->Grids[L];
        if(L>0){
            const auto& Fine=State->Grids[L-1];
            for(int Axis=0;Axis<3;++Axis){
                DirtyLo[Axis]=int32(vxc::floorDiv(DirtyLo[Axis]+Fine.Origin[Axis],2))-Current.Origin[Axis];
                DirtyHi[Axis]=int32(vxc::floorDiv(DirtyHi[Axis]+Fine.Origin[Axis]+1,2))-Current.Origin[Axis];
                DirtyLo[Axis]=FMath::Clamp(DirtyLo[Axis],0,Current.Size[Axis]);
                DirtyHi[Axis]=FMath::Clamp(DirtyHi[Axis],0,Current.Size[Axis]);
            }
            for(int X=DirtyLo.X;X<DirtyHi.X;++X)for(int Y=DirtyLo.Y;Y<DirtyHi.Y;++Y)for(int Z=DirtyLo.Z;Z<DirtyHi.Z;++Z){
                int Counts[256]={};
                for(int DX=0;DX<2;++DX)for(int DY=0;DY<2;++DY)for(int DZ=0;DZ<2;++DZ)
                    ++Counts[Fine.LocalAt((X+Current.Origin.X)*2+DX,(Y+Current.Origin.Y)*2+DY,(Z+Current.Origin.Z)*2+DZ)];
                int Best=0;for(int M=1;M<256;++M)if(Counts[M]>(Best?Counts[Best]:0))Best=M;
                verify(Current.Set(X,Y,Z,uint8(Best)));
            }
        }
        // Include one cell of neighbors for newly exposed faces across sections.
        const FIntVector MeshLo(FMath::Max(0,DirtyLo.X-1),FMath::Max(0,DirtyLo.Y-1),FMath::Max(0,DirtyLo.Z-1));
        const FIntVector MeshHi(FMath::Min(Current.Size.X,DirtyHi.X+1),FMath::Min(Current.Size.Y,DirtyHi.Y+1),FMath::Min(Current.Size.Z,DirtyHi.Z+1));
        RebuildSections(L,MeshLo,MeshHi);
    }
    UE_LOG(LogVoxelEarth,Log,TEXT("EnvironmentLOD incremental %s editMs=%.3f sections=%d faces=%d revision=%d"),*AssetName,(FPlatformTime::Seconds()-EditStart)*1000,State->LastSections,State->LastFaces,State->Revision);
    if(FParse::Param(FCommandLine::Get(),TEXT("VoxelEnvironmentLODValidate"))){
        for(int L=1;L<State->Grids.Num();++L){
            const auto Reference=Reduce(State->Grids[L-1]);
            checkf(Reference.Equals(State->Grids[L]),TEXT("Incremental LOD differs from full reduction"));
        }
        // Slow oracle is opt-in and deliberately outside the edit timing.
        // It catches lost or duplicate faces at section seams after carving.
        for(int L=0;L<State->Grids.Num();++L){
            FEnvironmentLodMeshGeometry Reference;const auto& G=State->Grids[L];
            BuildNaiveFaceGeometry(G,GetTypeHash(AssetName),Reference,FIntVector::ZeroValue,G.Size);
            int32 Triangles=0;for(const auto& Pair:State->SectionMaps[L])Triangles+=Pair.Value->GetProcMeshSection(0)->ProcIndexBuffer.Num()/3;
            checkf(Triangles==Reference.Indices.Num()/3,TEXT("Section face count differs from full mesh"));
        }
        UE_LOG(LogVoxelEarth,Log,TEXT("EnvironmentLOD reductionReferenceMatch %s revision=%d"),*AssetName,State->Revision);
    }
    bool Empty=true;
    // A coarse-aligned deletion must stay empty through every derived LOD.
    for(const auto& L:State->Grids)Empty&=L.LocalAt(FMath::FloorToInt64(Local.X/(L.Mm*.1)),FMath::FloorToInt64(Local.Y/(L.Mm*.1)),FMath::FloorToInt64(Local.Z/(L.Mm*.1)))==0;
    UE_LOG(LogVoxelEarth,Log,TEXT("EnvironmentLOD carve %s removed=%d allLODsEmpty=%d collisionEmpty=%d revision=%d"),*AssetName,Removed,int(Empty),int(!SolidAt(Hit)),State->Revision);
    if(State->AxeCut&&!State->Severed&&IsFellable()){
        for(int Z=Hi.Z-1;Z>=Lo.Z;--Z)if(Z>1&&Z<Grid.Size.Z-2&&State->WoodPerLayer[Z]==0){
            int Above=0,Below=0;for(int I=0;I<Z;++I)Below+=State->WoodPerLayer[I];
            for(int I=Z+1;I<Grid.Size.Z;++I)Above+=State->WoodPerLayer[I];
            if(Below>10&&Above>100){DetachAbove(Z,State->LastChopDirection);break;}
        }
    }
    if(!GeometrySnapshot)RefreshGeometrySnapshot();
    return Empty;
}
bool AVoxelEnvironmentLODPrototype::CanChop(const FVector& Hit) const {
    if(!IsFellable()||State->Grids.IsEmpty()||Hit.ContainsNaN()||!VoxelEnvironmentAsset::IsSupportedTransform(GetActorTransform()))return false;
    const auto& G=State->Grids[0];const FVector P=VoxelEnvironmentAsset::LocalPosition(GetActorTransform(),Hit)/(G.Mm*.1);
    const auto M=G.LocalAt(FMath::FloorToInt64(P.X),FMath::FloorToInt64(P.Y),FMath::FloorToInt64(P.Z));
    return M==16||M==17||M==18||M==23;
}
bool AVoxelEnvironmentLODPrototype::Chop(const FVector& Hit,const FVector& Direction,int32 Size){
    if(GetWorld()->GetNetMode()==NM_Client)return false;
    if(!CanChop(Hit))return false;
    State->AxeCut=true;State->LastChopDirection=Direction;
    const bool Result=Carve(Hit,Size);State->AxeCut=false;return Result;
}
void AVoxelEnvironmentLODPrototype::DetachAbove(int32 CutLayer,const FVector& Direction){
    GeometrySnapshot.Reset();
    const double Start=FPlatformTime::Seconds();State->Severed=true;
    auto& Fine=State->Grids[0];const int32 First=CutLayer+1;
    const double Plane=(First+Fine.Origin.Z)*Fine.Mm*.1;
    const double Top=(Fine.Size.Z+Fine.Origin.Z)*Fine.Mm*.1;
    TArray<UProceduralMeshComponent*> Moving;
    auto Material=UMaterialInstanceDynamic::Create(Materials[0]->Parent,this);
    Material->SetScalarParameterValue(TEXT("WindEnabled"),0);
    Material->SetScalarParameterValue(TEXT("Fade"),1);Material->SetScalarParameterValue(TEXT("Reverse"),0);
    // Prebuild only the two exposed cross-sections. Their surfaces remain hidden
    // until impact, so splitting the visual sections needs no full-tree remesh.
    const double Split=FMath::RoundToDouble((Plane+Top)*.5/160.)*160.;
    const int SplitZ=FMath::RoundToInt(Split/(Fine.Mm*.1))-Fine.Origin.Z;
    for(int Side=0;Side<2;++Side){
        FEnvironmentLodMeshGeometry Cap;const float P=float(Fine.Mm*.1);
        Fine.VisitBox(FIntVector(0,0,SplitZ),FIntVector(Fine.Size.X,Fine.Size.Y,SplitZ+1),[&](int X,int Y,int,uint8){
            if(!Fine.At(X,Y,SplitZ-1))return;
            const auto M=Fine.At(X,Y,Side?SplitZ:SplitZ-1);
            const auto Color=EnvironmentLodPaletteLinear().Face[(M==16||M==18)?17:M][vxc::kFaceTop];
            const uint32 B=Cap.Positions.Num();const float FX=(X+Fine.Origin.X)*P,FY=(Y+Fine.Origin.Y)*P;
            Cap.Positions.Append({FVector3f(FX,FY,Split),FVector3f(FX,FY+P,Split),FVector3f(FX+P,FY+P,Split),FVector3f(FX+P,FY,Split)});
            for(int V=0;V<4;++V){Cap.Normals.Add(FVector3f(0,0,Side?-1:1));Cap.TangentsX.Add(FVector3f(1,0,0));Cap.Colors.Add(FVector4f(Color.R,Color.G,Color.B,1));Cap.UVs.Add(FVector2f::ZeroVector);}
            if(Side)Cap.Indices.Append({B,B+2,B+1,B,B+3,B+2});else Cap.Indices.Append({B,B+1,B+2,B,B+2,B+3});
        });
        if(!Cap.Indices.IsEmpty()){
            auto C=NewObject<UVoxelPlantMeshComponent>(this);C->SetupAttachment(GetRootComponent());C->SetCollisionEnabled(ECollisionEnabled::NoCollision);C->RegisterComponent();C->SetMaterial(0,Material);
            C->ComponentTags.Add(Side?TEXT("FractureUpperCap"):TEXT("FractureLowerCap"));ApplyGeometry(C,Cap);Moving.Add(C);
        }
    }
    for(int L=0;L<State->Grids.Num();++L){
        auto& G=State->Grids[L];const int Edge=G.Mm==25?16:32;
        const int Cut=FMath::Clamp(FMath::CeilToInt(Plane/(G.Mm*.1))-G.Origin.Z,0,G.Size.Z);
        TArray<FIntVector> Keys;State->SectionMaps[L].GetKeys(Keys);
        for(auto Key:Keys){
            auto C=State->SectionMaps[L][Key];const FIntVector Lo=Key*Edge;
            if(Lo.Z>=Cut){
                State->SectionMaps[L].Remove(Key);Sections.Remove(C);
                if(L==0){C->SetMaterial(0,Material);Moving.Add(C);}else C->DestroyComponent();
            }else if(L==0&&Lo.Z+Edge>Cut){
                FEnvironmentLodMeshGeometry Geometry;const FIntVector UpperLo(Lo.X,Lo.Y,Cut);
                const FIntVector Hi(FMath::Min(Lo.X+Edge,G.Size.X),FMath::Min(Lo.Y+Edge,G.Size.Y),FMath::Min(Lo.Z+Edge,G.Size.Z));
                BuildNaiveFaceGeometry(G,GetTypeHash(AssetName),Geometry,UpperLo,Hi);
                if(!Geometry.Indices.IsEmpty()){
                    auto Upper=NewObject<UVoxelPlantMeshComponent>(this);Upper->SetupAttachment(GetRootComponent());
                    Upper->SetCollisionEnabled(ECollisionEnabled::NoCollision);Upper->RegisterComponent();
                    Upper->SetMaterial(0,Material);ApplyGeometry(Upper,Geometry);Moving.Add(Upper);
                }
            }
        }
        G.MaxDataZ=Cut;
        G.Data.clearAbove(Cut);
        RebuildSections(L,FIntVector(0,0,FMath::Max(0,Cut-1)),FIntVector(G.Size.X,G.Size.Y,FMath::Min(G.Size.Z,Cut+1)));
    }
    for(int Z=First;Z<State->WoodPerLayer.Num();++Z)State->WoodPerLayer[Z]=0;
    auto Timber=GetWorld()->SpawnActor<AVoxelFallingTimber>();
    Timber->Initialize(Moving,FBox(FVector(-25,-25,Plane),FVector(25,25,Top)),GetActorTransform(),Direction,true);
    auto Stump=NewObject<UBoxComponent>(this);Stump->ComponentTags.Add(TEXT("FellingStump"));
    Stump->SetupAttachment(GetRootComponent());Stump->SetRelativeLocation(FVector(0,0,Plane*.5));Stump->SetBoxExtent(FVector(25,25,FMath::Max(5.,Plane*.5-5)));
    Stump->SetCollisionObjectType(ECC_WorldStatic);Stump->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);Stump->SetCollisionResponseToAllChannels(ECR_Block);
    // The explicit stump hinge supplies support. A second box contact at the
    // same pivot would overconstrain the long trunk and fight its chosen plane.
    Stump->SetCollisionResponseToChannel(ECC_PhysicsBody,ECR_Ignore);Stump->RegisterComponent();
    RefreshGeometrySnapshot();
    UE_LOG(LogVoxelEarth,Log,TEXT("TreeFelling DETACH cutHeightCm=%.1f sections=%d transferMs=%.2f"),Plane,Moving.Num(),(FPlatformTime::Seconds()-Start)*1000);
}
bool AVoxelStoneAxePrototype::Initialize(){
    TArray<uint8> Bytes;vxc::AssetGrid Source;
    const FString Path=FPaths::ProjectDir()/TEXT("../asset-forge/out/tree-felling-prototype/stone-axe-12_5mm.vxa");
    if(!FFileHelper::LoadFileToArray(Bytes,*Path)||Source.parse(Bytes.GetData(),Bytes.Num())!=vxc::AssetParseError::kOk)return false;
    FPrototypeGrid G;G.Size=FIntVector(Source.sizeX(),Source.sizeY(),Source.sizeZ());G.Origin=FIntVector(Source.originX(),Source.originY(),Source.originZ());G.Mm=Source.voxelSizeMm();
    if(G.Mm!=12.5||int64(G.Size.X)*G.Size.Y*G.Size.Z>1000000)return false;
    if(!G.Init())return false;bool Imported=true;
    for(int X=0;X<G.Size.X;++X)for(int Y=0;Y<G.Size.Y;++Y)Source.columnRuns(X,Y,[&](int Z,int Len,vxc::MaterialId M){if(M&&Imported)Imported=G.SetRun(X,Y,Z,Len,uint8(M));});
    if(!Imported)return false;
    FEnvironmentLodMeshGeometry Geometry;BuildNaiveFaceGeometry(G,1,Geometry,FIntVector::ZeroValue,G.Size);ApplyGeometry(Mesh,Geometry);
    auto Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Voxel/M_VoxelEnvironmentLOD.M_VoxelEnvironmentLOD"));
    auto Material=UMaterialInstanceDynamic::Create(Base,this);Material->SetScalarParameterValue(TEXT("Fade"),1);Material->SetScalarParameterValue(TEXT("Reverse"),0);Material->SetScalarParameterValue(TEXT("WindEnabled"),0);Mesh->SetMaterial(0,Material);
    UE_LOG(LogVoxelEarth,Log,TEXT("TreeFelling AXE loaded pitchMm=12.5 faces=%d"),Geometry.Indices.Num()/6);return true;
}
namespace VoxelEnvironmentLODPrototype {
bool IsSolid(UWorld* World,const FVector& Position){
    for(auto Weak:Actors)if(auto A=Weak.Get())if(A->GetWorld()==World&&A->SolidAt(Position))return true;return false;
}
static AVoxelEnvironmentLODPrototype* FindDigTarget(UWorld* World,const FVector& Start,const FVector& Direction,FVector& Best){
    if(!World)return nullptr;
    AVoxelEnvironmentLODPrototype* Nearest=nullptr;double Range=800.;
    if(auto Sub=World->GetSubsystem<UVoxelWorldSubsystem>()){
        FVector Terrain,Previous;if(Sub->RaycastVoxelWorld(Start,Direction,Range,Terrain,Previous))Range=FMath::Max(0.,FVector::Dist(Start,Terrain)-8.7);
    }
    for(auto Weak:Actors)if(auto A=Weak.Get())if(A->GetWorld()==World){FVector Hit;if(A->Trace(Start,Direction,Range,Hit)){const double D=FVector::Dist(Start,Hit);if(D<=Range+5){Range=D;Best=Hit;Nearest=A;}}}
    return Nearest;
}
bool GetDigPreview(UWorld* World,const FVector& Start,const FVector& Direction,int32 SizeVoxels,FBox& Bounds){
    FVector Hit;auto A=FindDigTarget(World,Start,Direction,Hit);if(!A)return false;
    return A->GetDigBounds(Hit,SizeVoxels,Bounds);
}
bool TryDig(UWorld* World,const FVector& Start,const FVector& Direction,int32 SizeVoxels){
    if(!World||World->GetNetMode()==NM_Client)return false;
    FVector Hit;auto A=FindDigTarget(World,Start,Direction,Hit);
    return A&&A->Carve(Hit,SizeVoxels);
}
} // namespace
namespace VoxelTreeFelling {
bool GetPreview(UWorld* W,const FVector& Start,const FVector& Direction,int32 Size,FBox& Bounds){
    FVector Hit;auto A=VoxelEnvironmentLODPrototype::FindDigTarget(W,Start,Direction,Hit);
    if(!A||!A->CanChop(Hit)||FVector::Dist(Start,Hit)>=300)return false;
    return A->GetDigBounds(Hit,Size,Bounds);
}
bool Chop(UWorld* W,const FVector& Start,const FVector& Direction,int32 Size){
    if(!W||W->GetNetMode()==NM_Client)return false;
    FVector Hit;auto A=VoxelEnvironmentLODPrototype::FindDigTarget(W,Start,Direction,Hit);
    return A&&FVector::Dist(Start,Hit)<300&&A->Chop(Hit,Direction,Size);
}
}
namespace {
struct FEnvironmentCapture {
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<ACameraActor> Camera;
    TArray<TWeakObjectPtr<AVoxelEnvironmentLODPrototype>> Assets;
    FTimerHandle Timer;
    int32 Stage=0;
    double Start=0;
};
void SpawnEnvironmentPrototype(UWorld* World,bool Capture){
    if(!World||World->GetNetMode()==NM_Client)return;
    for(auto Weak:Actors)if(auto A=Weak.Get())if(A->GetWorld()==World)return;
    auto Sub=World->GetSubsystem<UVoxelWorldSubsystem>();auto PC=World->GetFirstPlayerController();
    if(!Sub||!PC||!PC->GetPawn()){UE_LOG(LogVoxelEarth,Error,TEXT("EnvironmentLOD requires a player session"));return;}
    const FVector Anchor=PC->GetPawn()->GetActorLocation();
    const TCHAR* Names[]={TEXT("temperate-oak"),TEXT("granite-boulder"),TEXT("bramble-thicket"),TEXT("meadow-daisy")};
    const int32 Pitches[]={50,50,25,25};
    auto Run=MakeShared<FEnvironmentCapture>();Run->World=World;Run->Start=FPlatformTime::Seconds();
    for(int I=0;I<4;++I){
        FVector Pos=Anchor+FVector(1800,I*1500,0);
        Pos.X=FMath::GridSnap(Pos.X,10.);Pos.Y=FMath::GridSnap(Pos.Y,10.);
        Pos.Z=FMath::CeilToDouble(Sub->GetSurfaceHeightUU(Pos.X,Pos.Y)/10.)*10.+10.;
        auto A=World->SpawnActor<AVoxelEnvironmentLODPrototype>(Pos,FRotator::ZeroRotator);
        if(!A||!A->InitializeAsset(Names[I],Pitches[I],I<2)){if(A)A->Destroy();continue;}
        Run->Assets.Add(A);
        Pos=A->GetActorLocation();
        UE_LOG(LogVoxelEarth,Log,TEXT("EnvironmentLOD SPAWN %s position=(%.0f,%.0f,%.0f) collision=%d"),Names[I],Pos.X,Pos.Y,Pos.Z,int(I<2));
    }
    if(FParse::Param(FCommandLine::Get(),TEXT("VoxelTreeFelling"))&&!Run->Assets.IsEmpty()&&Run->Assets[0]->AssetName==TEXT("temperate-oak")){
        VoxelTreeFelling::Prepare(World,Run->Assets[0]->GetActorLocation());
        if(FParse::Param(FCommandLine::Get(),TEXT("VoxelTreeFellingCapture"))){
            auto Camera=World->SpawnActor<ACameraActor>();Camera->GetCameraComponent()->SetFieldOfView(65.f);Run->Camera=Camera;
            const FVector Target=Run->Assets[0]->GetActorLocation()+FVector(0,0,450);
            Camera->SetActorLocation(Target+FVector(-2200,-1700,700));Camera->SetActorRotation((Target-Camera->GetActorLocation()).Rotation());PC->SetViewTarget(Camera);
            World->GetTimerManager().SetTimer(Run->Timer,[Run](){
                auto W=Run->World.Get();auto A=Run->Assets[0].Get();if(!W||!A)return;
                if(Run->Stage>=3&&Run->Stage<27){
                    const int I=Run->Stage-3;const FVector Directions[]={FVector(1,0,0),FVector(-1,0,0),FVector(0,1,0),FVector(0,-1,0)};
                    const FVector D=Directions[I%4],Side=FVector::CrossProduct(D,FVector::UpVector);FVector Hit;
                    const FVector Start=A->GetActorLocation()-D*220+Side*((I/4%3-1)*25.)+FVector(0,0,150);
                    bool Falling=false;for(TActorIterator<AVoxelFallingTimber> It(W);It;++It)Falling=true;
                    if(!Falling&&A->Trace(Start,D,440,Hit)){
                        auto Camera=Run->Camera.Get();Camera->SetActorLocation(Start);Camera->SetActorRotation(D.Rotation());
                        for(TActorIterator<AVoxelStoneAxePrototype> It(W);It;++It)It->Swing();
                        UE_LOG(LogVoxelEarth,Log,TEXT("TreeFelling TEST swing=%d"),I);
                    }
                    if(Falling){
                        auto Camera=Run->Camera.Get();const FVector Target=A->GetActorLocation()+FVector(0,0,450);
                        Camera->SetActorLocation(Target+FVector(-2200,-1700,700));Camera->SetActorRotation((Target-Camera->GetActorLocation()).Rotation());
                    }
                }
                if(Run->Stage%2==0){
                    FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir()/TEXT("Screenshots/TreeFelling")/FString::Printf(TEXT("stage-%02d.png"),Run->Stage),false,false);
                }
                if(++Run->Stage>=48){
                    int Bodies=0;for(TActorIterator<AVoxelFallingTimber> It(W);It;++It)++Bodies;
                    UE_LOG(LogVoxelEarth,Log,TEXT("TreeFelling TEST COMPLETE bodies=%d"),Bodies);
                    auto PC=W->GetFirstPlayerController();auto Pawn=Cast<AVoxelEarthFlyPawn>(PC->GetPawn());
                    if(Pawn){Pawn->SetWalkMode(false);Pawn->SetActorLocation(A->GetActorLocation()+FVector(-220,0,170));PC->SetControlRotation(FRotator(0,0,0));PC->SetViewTarget(Pawn);}
                    W->GetTimerManager().ClearTimer(Run->Timer);return;
                }
            },1.f,true,8.f);
        }
        return;
    }
    if(!Capture)return;
    if(Run->Assets.Num()!=4){UE_LOG(LogVoxelEarth,Error,TEXT("EnvironmentLOD incomplete fixture"));return;}
    auto Camera=World->SpawnActor<ACameraActor>();Camera->GetCameraComponent()->SetFieldOfView(60.f);Run->Camera=Camera;
    const FVector InitialTarget=Run->Assets[0]->GetActorLocation()+FVector(0,0,650);
    Camera->SetActorLocation(InitialTarget+FVector(-2500,-1375,750));
    Camera->SetActorRotation((InitialTarget-Camera->GetActorLocation()).Rotation());
    PC->SetViewTarget(Camera);
    // First settle, then matched-camera fine/coarse comparisons, followed by
    // distance transitions and a real TryDig through the player edit seam.
    World->GetTimerManager().SetTimer(Run->Timer,[Run](){
        UWorld* W=Run->World.Get();auto Camera=Run->Camera.Get();if(!W||!Camera)return;
        const int32 AssetIndex=Run->Stage/10,Step=Run->Stage%10;
        if(AssetIndex>=Run->Assets.Num()){
            UE_LOG(LogVoxelEarth,Log,TEXT("EnvironmentLOD COMPLETE elapsed=%.2fs"),FPlatformTime::Seconds()-Run->Start);
            if(FParse::Param(FCommandLine::Get(),TEXT("VoxelEnvironmentLODKeepOpen"))){
                auto Controller=W->GetFirstPlayerController();
                if(auto Pawn=Controller?Cast<AVoxelEarthFlyPawn>(Controller->GetPawn()):nullptr){
                    Pawn->ClearScriptedInput();Pawn->SetWalkMode(false);
                    const auto First=Run->Assets[0].Get();
                    const FVector Target=(First?First->GetActorLocation():Pawn->GetActorLocation())+FVector(0,0,200);
                    Pawn->SetActorLocation(Target+FVector(-800,-500,100));
                    Controller->SetControlRotation((Target-Pawn->GetActorLocation()).Rotation());Controller->SetViewTarget(Pawn);
                }
                for(auto Weak:Run->Assets)if(auto A=Weak.Get())A->SetTestLOD(-1);
                UE_LOG(LogVoxelEarth,Log,TEXT("EnvironmentLOD FREE EXPLORATION: reset with voxel.EnvironmentLOD.Reset"));
            }else FGenericPlatformMisc::RequestExit(false);
            // Clearing this timer destroys its delegate and captured Run.
            // Do not read the capture again after removing the timer.
            W->GetTimerManager().ClearTimer(Run->Timer);
            return;
        }
        auto A=Run->Assets[AssetIndex].Get();if(!A)return;
        const double Heights[]={650,70,80,22};const double Distances[]={2500,260,220,85};
        FVector Target=A->GetActorLocation()+FVector(0,0,Heights[AssetIndex]);
        if(Step==0||Step==2||Step==4||Step==6){
            double D=Distances[AssetIndex];
            if(Step==4)D*=4;
            Camera->SetActorLocation(Target+FVector(-D,-D*.55,D*.30));
            Camera->SetActorRotation((Target-Camera->GetActorLocation()).Rotation());
            A->SetTestLOD(Step==0?0:Step==2?A->LevelCount()-1:-1);
            if((Step==0||Step==2)&&AssetIndex<2){
                if(auto Pawn=Cast<AVoxelEarthFlyPawn>(W->GetFirstPlayerController()->GetPawn())){
                    auto Sub=W->GetSubsystem<UVoxelWorldSubsystem>();
                    FVector Position=A->GetActorLocation()+FVector(-400,0,0);
                    Position.Z=Sub->GetSurfaceHeightUU(Position.X,Position.Y)+VoxelMovementTuning::StandHalfExtentZ+2.;
                    Pawn->SetWalkMode(false);Pawn->SetActorLocation(Position);
                    W->GetFirstPlayerController()->SetControlRotation(FRotator::ZeroRotator);
                    Pawn->SetWalkMode(true);Pawn->SetScriptedInput(1,0);
                }
            }
            if(Step==6){
                // Find a low hit at interaction range; carve the same source
                // regardless of whichever visual LOD is currently selected.
                FVector Hit=FVector::ZeroVector;bool HitFound=false;FVector Start=FVector::ZeroVector,Direction=FVector::ZeroVector;
                auto Terrain=W->GetSubsystem<UVoxelWorldSubsystem>();
                const FVector Directions[]={FVector(1,0,0),FVector(-1,0,0),FVector(0,1,0),FVector(0,-1,0)};
                for(int Z=40;Z<=400&&!HitFound;Z+=10)for(const FVector& RayDirection:Directions){
                    Start=A->GetActorLocation()-RayDirection*400+FVector(0,0,Z);Direction=RayDirection;
                    if(!A->Trace(Start,RayDirection,800,Hit))continue;
                    FVector Ground,Previous;
                    if(Terrain->RaycastVoxelWorld(Start,RayDirection,800,Ground,Previous)&&FVector::Dist(Start,Ground)-12<=FVector::Dist(Start,Hit))continue;
                    HitFound=true;break;
                }
                if(!HitFound){
                    // Small flowers may have no cells on either central axis.
                    // Search down from above through their actual footprint.
                    for(int X=-100;X<=100&&!HitFound;X+=5)for(int Y=-100;Y<=100;Y+=5){
                        Start=A->GetActorLocation()+FVector(X,Y,400);Direction=FVector(0,0,-1);
                        if(!A->Trace(Start,Direction,800,Hit))continue;
                        FVector Ground,Previous;
                        if(Terrain->RaycastVoxelWorld(Start,Direction,800,Ground,Previous)&&FVector::Dist(Start,Ground)-12<=FVector::Dist(Start,Hit))continue;
                        HitFound=true;break;
                    }
                }
                if(HitFound){
                    auto Sub=W->GetSubsystem<UVoxelWorldSubsystem>();
                    const int64 VX=FMath::FloorToInt64(Hit.X/10),VY=FMath::FloorToInt64(Hit.Y/10),VZ=FMath::FloorToInt64(Hit.Z/10);
                    const bool Before=Sub->IsSolidAtVoxel(VX,VY,VZ);
                    const bool Dug=Sub->TryDig(Start,Direction,3);
                    const bool After=Sub->IsSolidAtVoxel(VX,VY,VZ);
                    UE_LOG(LogVoxelEarth,Log,TEXT("EnvironmentLOD SYSTEM %s trace=1 dig=%d collisionBefore=%d collisionAfter=%d"),*A->AssetName,int(Dug),int(Before),int(After));
                    if(FParse::Param(FCommandLine::Get(),TEXT("VoxelEnvironmentLODValidate"))){
                        for(int Probe=0;Probe<8;++Probe){
                            FVector ProbeHit;
                            const FVector ProbeStart=Start+FVector(0,0,Probe*15.);
                            if(A->Trace(ProbeStart,Direction,800,ProbeHit))A->Carve(ProbeHit,1+Probe%4);
                        }
                    }
                }else UE_LOG(LogVoxelEarth,Error,TEXT("EnvironmentLOD SYSTEM %s no low hit"),*A->AssetName);
                A->SetTestLOD(0);
            }
        }else if(Step==8){A->SetTestLOD(A->LevelCount()-1);}
        else{
            if((Step==1||Step==3)&&AssetIndex<2){
                if(auto Pawn=Cast<AVoxelEarthFlyPawn>(W->GetFirstPlayerController()->GetPawn())){
                    Pawn->ClearScriptedInput();const FVector Position=Pawn->GetActorLocation();
                    bool AssetAhead=false;
                    for(int Z=-80;Z<=80;Z+=10)AssetAhead|=A->SolidAt(Position+FVector(35,0,Z));
                    UE_LOG(LogVoxelEarth,Log,TEXT("EnvironmentLOD WALK %s lod=%d xRelative=%.2f zRelative=%.2f assetAhead=%d"),*A->AssetName,A->CurrentLOD(),Position.X-A->GetActorLocation().X,Position.Z-A->GetActorLocation().Z,int(AssetAhead));
                    Pawn->SetWalkMode(false);
                }
            }
            const TCHAR* Labels[]={TEXT("fine"),TEXT("coarse"),TEXT("distance"),TEXT("carved-near"),TEXT("carved-coarse")};
            const FString File=FPaths::ProjectSavedDir()/TEXT("Screenshots/EnvironmentLOD")/FString::Printf(TEXT("%s-%s.png"),*A->AssetName,Labels[Step/2]);
            FScreenshotRequest::RequestScreenshot(File,false,false);
            UE_LOG(LogVoxelEarth,Log,TEXT("EnvironmentLOD SHOT %s lod=%d frameMs=%.2f"),*File,A->CurrentLOD(),W->GetDeltaSeconds()*1000.f);
        }
        ++Run->Stage;
    },2.f,true,12.f);
}
FAutoConsoleCommandWithWorld ResetCommand(TEXT("voxel.EnvironmentLOD.Reset"),TEXT("Restore prototype source grids and resume distance LOD."),FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* W){
    VoxelTreeFelling::Reset(W);
    for(auto Weak:Actors)if(auto A=Weak.Get())if(A->GetWorld()==W&&A->GetAssetDescriptor().Legacy){
        TArray<UBoxComponent*> Boxes;A->GetComponents(Boxes);for(auto C:Boxes)if(C->ComponentHasTag(TEXT("FellingStump")))C->DestroyComponent();
        const bool Solid=A->AssetName==TEXT("temperate-oak")||A->AssetName==TEXT("granite-boulder");
        A->InitializeAsset(A->AssetName,Solid?50:25,Solid);A->SetTestLOD(-1);
    }
}));
FAutoConsoleCommandWithWorld SpawnCommand(TEXT("voxel.EnvironmentLOD.Spawn"),TEXT("Spawn the isolated oak, granite, bramble and daisy LOD pilot."),FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* W){SpawnEnvironmentPrototype(W,false);}));
}
namespace VoxelEnvironmentLODPrototype {
void StartFromCommandLine(UWorld* World){
    if(!FParse::Param(FCommandLine::Get(),TEXT("VoxelEnvironmentLOD")))return;
    const bool Capture=FParse::Param(FCommandLine::Get(),TEXT("VoxelEnvironmentLODCapture"));
    FTimerHandle Timer;TWeakObjectPtr<UWorld> WeakWorld=World;
    World->GetTimerManager().SetTimer(Timer,[WeakWorld,Capture](){if(auto W=WeakWorld.Get())SpawnEnvironmentPrototype(W,Capture);},10.f,false);
}
}

