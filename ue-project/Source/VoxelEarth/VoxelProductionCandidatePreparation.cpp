#include "VoxelProductionCandidatePreparation.h"
#include "VoxelEnvironmentLODPrototype.h"
#include "VoxelEnvironmentSparseGrid.h"
#include "HAL/IConsoleManager.h"
#include "Misc/SecureHash.h"
#include "Serialization/MemoryWriter.h"
#include "Engine/World.h"
namespace
{
TMap<TWeakObjectPtr<UWorld>,int32> CandidateRequests;
FAutoConsoleCommandWithWorld PrepareProductionCandidateCommand(
    TEXT("voxel.Environment.PrepareCandidate"),
    TEXT("Prepare one nearby canonical 100 mm environment asset, hidden and non-interactive. Does not publish ownership."),
    FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World){if(World)CandidateRequests.Add(World,1);}));
FAutoConsoleCommandWithWorld CancelProductionCandidateCommand(
    TEXT("voxel.Environment.CancelCandidate"),TEXT("Cancel and destroy the hidden production candidate preparation."),
    FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World){if(World)CandidateRequests.Add(World,2);}));
FAutoConsoleCommandWithWorld RehearseProductionHandoffCommand(
    TEXT("voxel.Environment.RehearseHandoff"),
    TEXT("Prepare a hidden canonical candidate, then rehearse bounded page quiescence and allocation-token validation. No ownership publication or allocator pin guarantee."),
    FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World){if(World)CandidateRequests.Add(World,3);}));
FAutoConsoleCommandWithWorld PrepareHeldCpuPagesCommand(
    TEXT("voxel.Environment.PrepareHeldCpuPages"),
    TEXT("Prepare private CPU replacement packs for all allocated candidate pages under a bounded freeze and eviction-pressure pins. No GPU readiness or ownership publication."),
    FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World){if(World)CandidateRequests.Add(World,4);}));
FAutoConsoleCommandWithWorld PrepareHeldGpuPagesCommand(
    TEXT("voxel.Environment.PrepareHeldGpuPages"),
    TEXT("Prepare private CPU and brick-only GPU packs for candidate pages, with bounded admission and parity checks. Does not publish ownership or backend readiness."),
    FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World){if(World)CandidateRequests.Add(World,5);}));
}
namespace VoxelProductionCandidate
{
int32 ConsumeRequest(UWorld* World){int32 Request=0;if(World)CandidateRequests.RemoveAndCopyValue(World,Request);return Request;}
void ForgetWorld(UWorld* World){CandidateRequests.Remove(World);}
bool IsCurrent(const FWork& Work,uint64 Edit,uint64 Residency){return !Work.Cancelled.Load()&&Work.EditEpoch==Edit&&Work.ResidencyEpoch==Residency;}
uint64 Fingerprint(const FString& Text){FTCHARToUTF8 Bytes(*Text);uint64 Hash=14695981039346656037ull;for(int32 I=0;I<Bytes.Length();++I)Hash=(Hash^uint8(Bytes.Get()[I]))*1099511628211ull;return Hash;}
FString GridContentHash(const vxc::AssetGrid& Grid)
{
    // Canonical source content, independent of VXA compression/run boundaries.
    FMD5 Hash;
    const auto Word=[&Hash](uint32 Value){uint8 Bytes[4];for(int I=0;I<4;++I)Bytes[I]=uint8(Value>>(I*8));Hash.Update(Bytes,4);};
    Word(1);Word(uint32(Grid.originX()));Word(uint32(Grid.originY()));Word(uint32(Grid.originZ()));
    Word(uint32(Grid.sizeX()));Word(uint32(Grid.sizeY()));Word(uint32(Grid.sizeZ()));Word(Grid.voxelSizeUm());
    uint8 Buffer[4096];
    for(int32 X=0;X<Grid.sizeX();++X)for(int32 Y=0;Y<Grid.sizeY();++Y)
        for(int32 Z=0;Z<Grid.sizeZ();){const int32 Count=FMath::Min(4096,Grid.sizeZ()-Z);for(int32 I=0;I<Count;++I)Buffer[I]=uint8(Grid.at(X,Y,Z+I));Hash.Update(Buffer,Count);Z+=Count;}
    uint8 Digest[16];Hash.Final(Digest);return BytesToHex(Digest,16).ToLower();
}
bool EnumeratePages(const vxc::AssetCandidateBounds& Bounds,int32 Levels,std::vector<vxc::AssetRenderPage>& Out)
{
    Out.clear();std::vector<vxc::AssetRenderPage> Pages;if(Levels<1||Levels>16||Bounds.x0>Bounds.x1||Bounds.y0>Bounds.y1||Bounds.z0>Bounds.z1)return false;
    for(int32 Level=0;Level<Levels;++Level)
    {
        const int64 Edge=32ll<<Level;
        const int64 X0=vxc::floorDiv(Bounds.x0,Edge)-1,X1=vxc::floorDiv(Bounds.x1,Edge)+1;
        const int64 Y0=vxc::floorDiv(Bounds.y0,Edge)-1,Y1=vxc::floorDiv(Bounds.y1,Edge)+1;
        const int64 Z0=vxc::floorDiv(Bounds.z0,Edge)-1,Z1=vxc::floorDiv(Bounds.z1,Edge)+1;
        if(X0<MIN_int32||Y0<MIN_int32||Z0<MIN_int32||X1>MAX_int32||Y1>MAX_int32||Z1>MAX_int32)return false;
        if(X1-X0>MaxPages||Y1-Y0>MaxPages||Z1-Z0>MaxPages)return false;
        // Bound even rejected apron candidates, before entering the product loop.
        if(uint64(X1-X0+1)*uint64(Y1-Y0+1)*uint64(Z1-Z0+1)>uint64(MaxPages)*8)return false;
        for(int64 X=X0;X<=X1;++X)for(int64 Y=Y0;Y<=Y1;++Y)for(int64 Z=Z0;Z<=Z1;++Z)
        {
            vxc::AssetRenderPage Page{X,Y,Z,uint8(Level),vxc::AssetCpu|vxc::AssetGpu};
            if(vxc::assetPageTouches(Bounds,Page)){if(Pages.size()>=MaxPages)return false;Pages.push_back(Page);}
        }
    }
    Out=std::move(Pages);return !Out.empty();
}
bool BuildImmutableSnapshot(FWork& Work,const std::vector<uint8>& Vxa)
{
    if(Work.Cancelled.Load()||Vxa.empty()||Vxa.size()>32u*1024*1024)return false;
    vxc::AssetGrid Source;if(Source.parse(Vxa.data(),Vxa.size())!=vxc::AssetParseError::kOk||!Source.onTerrainLattice())return false;
    if(uint64(Source.sizeX())*Source.sizeY()*Source.sizeZ()>MaxCells)return false;
    Work.ClippedGeometryHash=FMD5::HashBytes(Vxa.data(),int32(Vxa.size()));
    Work.Descriptor.SourceHash=Work.ClippedGeometryHash;
    if(Work.Provenance.providerFingerprint||Work.Provenance.catalogFingerprint){
        FVoxelEnvironmentProductionProvenance Identity;
        Identity.Source=Work.Provenance;Identity.StableId=vxc::assetObjectId(Work.Provenance);
        Identity.CanonicalSourceHash=Work.CanonicalSourceHash;
        Work.Descriptor.ProductionProvenance=MoveTemp(Identity);
    }else if(Work.Descriptor.ProductionProvenance.IsSet())return false;

    if(!Work.Descriptor.IsValid())return false;
    FVoxelEnvironmentSparseGrid Grid;
    Grid.Size=FIntVector(Source.sizeX(),Source.sizeY(),Source.sizeZ());Grid.Origin=FIntVector(Source.originX(),Source.originY(),Source.originZ());Grid.Mm=100;
    if(!Grid.Init())return false;
    bool Valid=true;
    for(int32 X=0;X<Source.sizeX()&&Valid;++X)for(int32 Y=0;Y<Source.sizeY()&&Valid;++Y)
    {
        if(Work.Cancelled.Load())return false;
        Source.columnRuns(X,Y,[&](int32 Z,int32 Count,vxc::MaterialId M){if(M&&Valid)Valid=Grid.SetRun(X,Y,Z,Count,uint8(M));});
    }
    if(!Valid)return false;
    auto Geometry=MakeShared<TArray<uint8>,ESPMode::ThreadSafe>();FMemoryWriter G(*Geometry);VoxelObjectGeometrySnapshot::WriteVersion(G);
    if(!Grid.Serialize(G)||G.IsError())return false;
    Work.Dynamic.Reset();FMemoryWriter D(Work.Dynamic);VoxelObjectGeometrySnapshot::WriteVersion(D);
    if(!VoxelEnvironmentAsset::SerializeIdentity(D,Work.Descriptor))return false;
    // Yaw is baked into clipped indices. Preserve canonical yaw in provenance,
    // while the prepared actor is translated only, never rotated a second time.
    FTransform Transform(FQuat::Identity,FVector(double(Work.Provenance.anchorVx)*10.,double(Work.Provenance.anchorVy)*10.,double(Work.Provenance.anchorVz)*10.));
    bool Collision=false,Severed=false;
    D<<Transform<<Grid.Size<<Grid.Origin<<Grid.Mm<<Grid.MaxDataZ<<Collision<<Severed;
    if(D.IsError()||Work.Dynamic.Num()>4096||Geometry->Num()>32*1024*1024)return false;
    Work.Geometry=Geometry;return !Work.Cancelled.Load();
}
void Cancel(FWorkRef& Work)
{
    if(!Work)return;Work->Cancelled.Store(true);
    if(auto Actor=Work->Actor.Get()){Actor->CancelStagedObjectRestore();Actor->Destroy();}
    Work.Reset();
}
}
