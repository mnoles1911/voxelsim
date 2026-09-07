#include "VoxelProductionEnvironmentAdapter.h"

namespace VoxelProductionEnvironment
{
FGuid StableId(const vxc::AssetProvenance& Source)
{
    const auto Id=vxc::assetObjectId(Source);
    return FGuid(uint32(Id.high>>32),uint32(Id.high),uint32(Id.low>>32),uint32(Id.low));
}
FAdapter::FAdapter(FAtomicPublish Callback,uint8 Backends)
    : Published(MakeShared<const vxc::AssetOwnershipSnapshot,ESPMode::ThreadSafe>()),
      Publish(MoveTemp(Callback)),RequiredBackends(Backends) {}
FSnapshot FAdapter::Prepared(vxc::AssetOwnershipTicket Ticket) const
{
    check(IsInGameThread());const auto Target=Ownership.target(Ticket);
    return Target?MakeShared<const vxc::AssetOwnershipSnapshot,ESPMode::ThreadSafe>(*Target):FSnapshot();
}
vxc::AssetOwnershipTicket FAdapter::Prepare(const FSource& Source,vxc::AssetRenderOwner Owner,
                                           std::vector<vxc::AssetRenderPage> Pages)
{
    check(IsInGameThread());
    const auto& Evidence=Source.Admission;
    // First production pilot is exact 100 mm canonical composition. Editing
    // and finer source grids need their own provenance-preserving projection
    // bridge; accepting them here would hide authored/player-owned cells.
    if(!Publish||!RequiredBackends||(RequiredBackends&~uint8(vxc::AssetCpu|vxc::AssetGpu))||
       !Source.Provenance.providerFingerprint||!Source.Provenance.catalogFingerprint||Source.Provenance.yawQuarter>3||
       !Evidence.Canonical100MmSource||!Evidence.VisibleCellsMatchCanonicalComposition||
       !Evidence.CompleteTouchedPageSet||Evidence.HasEditedTerrainCells)return {};
    for(const auto& Page:Pages)if((Page.backends&RequiredBackends)!=RequiredBackends)return {};
    const auto Ticket=Ownership.begin(Source.Provenance,Owner,Source.ObjectRevision,Source.ProjectionRevision,MoveTemp(Pages));
    if(Ticket.serial){StagingTicket=Ticket;StagedPages.Reset();}return Ticket;
}
bool FAdapter::MarkObjectReady(vxc::AssetOwnershipTicket Ticket,uint64 Revision)
{check(IsInGameThread());return Ownership.objectReady(Ticket,Revision);}
bool FAdapter::MarkPageReady(vxc::AssetOwnershipTicket Ticket,const vxc::AssetRenderPage& Page,uint8 Backend,uint64 Generation)
{check(IsInGameThread());return Ownership.pageReady(Ticket,Page,Backend,Generation);}
const TArray<FPreparedPage>* FAdapter::PreparedPages(vxc::AssetOwnershipTicket Ticket) const
{check(IsInGameThread());return Ticket.serial&&Ticket==StagingTicket&&Ownership.target(Ticket)?&StagedPages:nullptr;}
bool FAdapter::StageGpuPage(vxc::AssetOwnershipTicket Ticket,const vxc::AssetRenderPage& Page,FVoxelGpuMeshJobResult&& Result)
{
    check(IsInGameThread());
    if(!PreparedPages(Ticket)||!Result.bPublicationHeld||Result.Status!=EVoxelGpuMeshJobStatus::Success||!Result.BrickVolume.IsValid()||
       !MarkPageReady(Ticket,Page,vxc::AssetGpu,Result.OwnershipGeneration))return false;
    auto Item=StagedPages.FindByPredicate([&](const auto& P){return P.Page.x==Page.x&&P.Page.y==Page.y&&P.Page.z==Page.z&&P.Page.level==Page.level;});
    if(!Item){Item=&StagedPages.AddDefaulted_GetRef();Item->Page=Page;Item->Generation=Result.OwnershipGeneration;}
    Item->GpuBricks=MoveTemp(Result.BrickVolume);Item->GpuQuads=MoveTemp(Result.GpuQuads);return true;
}
bool FAdapter::StageCpuPage(vxc::AssetOwnershipTicket Ticket,const vxc::AssetRenderPage& Page,uint64 Generation,FVoxelBrickCpuPackRef Bricks,TArray<uint64>&& Quads)
{
    check(IsInGameThread());if(!PreparedPages(Ticket)||!Bricks||!MarkPageReady(Ticket,Page,vxc::AssetCpu,Generation))return false;
    auto Item=StagedPages.FindByPredicate([&](const auto& P){return P.Page.x==Page.x&&P.Page.y==Page.y&&P.Page.z==Page.z&&P.Page.level==Page.level;});
    if(!Item){Item=&StagedPages.AddDefaulted_GetRef();Item->Page=Page;Item->Generation=Generation;}
    Item->CpuBricks=MoveTemp(Bricks);Item->CpuQuads=MoveTemp(Quads);return true;
}
bool FAdapter::Commit(vxc::AssetOwnershipTicket Ticket)
{
    check(IsInGameThread());
    if(!Publish||!Ownership.publish(Ticket,[&](const auto& Before,const auto& After,const auto& Pages){return Publish(Before,After,Pages);} ))return false;
    Published=MakeShared<const vxc::AssetOwnershipSnapshot,ESPMode::ThreadSafe>(Ownership.visible());StagedPages.Reset();StagingTicket={};return true;
}
bool FAdapter::Cancel(vxc::AssetOwnershipTicket Ticket)
{check(IsInGameThread());if(!Ownership.cancel(Ticket))return false;StagedPages.Reset();StagingTicket={};return true;}
}
