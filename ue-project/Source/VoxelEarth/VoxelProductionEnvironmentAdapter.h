#pragma once
#include "CoreMinimal.h"
#include "voxelcore/assetownership.h"
#include "VoxelGpuMeshJobManager.h"
#include "VoxelBrickPool.h"

// Presentation bridge for ordinary terrain assets. This class does not spawn
// actors, edit the terrain or suppress materials. The production renderer must
// supply an atomic page/object publication callback before claims are allowed.
namespace VoxelProductionEnvironment
{
using FSnapshot=TSharedPtr<const vxc::AssetOwnershipSnapshot,ESPMode::ThreadSafe>;
struct FAdmission
{
    // These are evidence from the world/request builder, not authoring flags.
    bool Canonical100MmSource=false;
    bool VisibleCellsMatchCanonicalComposition=false;
    bool CompleteTouchedPageSet=false;
    bool HasEditedTerrainCells=true;
};
struct FSource
{
    vxc::AssetProvenance Provenance;
    uint64 ObjectRevision=1,ProjectionRevision=1;
    FAdmission Admission;
};
struct FPreparedPage
{
    vxc::AssetRenderPage Page;
    uint64 Generation=0;
    FVoxelBrickCpuPackRef CpuBricks;
    TArray<uint64> CpuQuads;
    FVoxelGpuBrickPayloadRef GpuBricks;
    FVoxelGpuQuadPayloadRef GpuQuads;
    bool ValidatedAbsent=false;
};
using FAtomicPublish=TFunction<bool(const vxc::AssetOwnershipSnapshot& Before,
                                  const vxc::AssetOwnershipSnapshot& After,
                                  const std::vector<vxc::AssetRenderPage>& Pages)>;
FGuid StableId(const vxc::AssetProvenance& Source);
class FAdapter
{
public:
    explicit FAdapter(FAtomicPublish Publish={},uint8 RequiredBackends=vxc::AssetCpu|vxc::AssetGpu);
    FSnapshot Visible() const {return Published;}
    FSnapshot Prepared(vxc::AssetOwnershipTicket Ticket) const;
    vxc::AssetOwnershipTicket Prepare(const FSource& Source,vxc::AssetRenderOwner Owner,
                                     std::vector<vxc::AssetRenderPage> Pages);
    bool MarkObjectReady(vxc::AssetOwnershipTicket Ticket,uint64 Revision);
    bool MarkPageReady(vxc::AssetOwnershipTicket Ticket,const vxc::AssetRenderPage& Page,uint8 Backend,uint64 Generation);
    bool StageGpuPage(vxc::AssetOwnershipTicket Ticket,const vxc::AssetRenderPage& Page,FVoxelGpuMeshJobResult&& Result);
    bool StageCpuPage(vxc::AssetOwnershipTicket Ticket,const vxc::AssetRenderPage& Page,uint64 Generation,FVoxelBrickCpuPackRef Bricks,TArray<uint64>&& Quads);
    // Absence is explicit evidence, never an invented empty payload. The
    // publication callback must revalidate the same complete pool token.
    bool StageAbsentPage(vxc::AssetOwnershipTicket Ticket,const vxc::AssetRenderPage& Page,uint64 Generation,
                         const FVoxelBrickPool& Pool,const FVoxelBrickPreparedBatchRef& Batch);
    bool StageAbsentPage(vxc::AssetOwnershipTicket Ticket,const vxc::AssetRenderPage& Page,uint64 Generation,
                         const FVoxelBrickPool& Pool,const FVoxelPrivateGpuReservationRef& Batch);
    const TArray<FPreparedPage>* PreparedPages(vxc::AssetOwnershipTicket Ticket) const;
    bool Commit(vxc::AssetOwnershipTicket Ticket);
    bool Cancel(vxc::AssetOwnershipTicket Ticket);
    bool AcceptsVisibleJob(uint64 Generation) const {return Ownership.acceptsVisibleJob(Generation);}
private:
    vxc::AssetRenderOwnership Ownership;
    FSnapshot Published;
    // Allocated before the renderer callback; successful publication only
    // transfers this pointer and performs no snapshot allocation afterward.
    FSnapshot PreparedSnapshot;
    FAtomicPublish Publish;
    uint8 RequiredBackends;
    vxc::AssetOwnershipTicket StagingTicket;
    TArray<FPreparedPage> StagedPages;
};
}
