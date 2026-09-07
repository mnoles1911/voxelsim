#pragma once
#include "CoreMinimal.h"
#include "voxelcore/assetownership.h"

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
    bool Commit(vxc::AssetOwnershipTicket Ticket);
    bool Cancel(vxc::AssetOwnershipTicket Ticket);
    bool AcceptsVisibleJob(uint64 Generation) const {return Ownership.acceptsVisibleJob(Generation);}
private:
    vxc::AssetRenderOwnership Ownership;
    FSnapshot Published;
    FAtomicPublish Publish;
    uint8 RequiredBackends;
};
}
