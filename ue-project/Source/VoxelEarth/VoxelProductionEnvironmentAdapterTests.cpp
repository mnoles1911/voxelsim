#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelProductionEnvironmentAdapter.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelProductionOwnershipTest,"Voxel.Objects.ProductionOwnership",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelProductionOwnershipTest::RunTest(const FString&)
{
    using namespace VoxelProductionEnvironment;
    FSource Source;Source.Provenance.worldSeed=9;Source.Provenance.anchorVx=-7;
    Source.Provenance.providerFingerprint=17;Source.Provenance.catalogFingerprint=29;
    std::vector<vxc::AssetRenderPage> Pages{{-1,0,0,0,vxc::AssetCpu|vxc::AssetGpu}};
    FAdapter Unwired;Source.Admission={true,true,true,false};
    TestEqual(TEXT("No renderer callback means no claim"),Unwired.Prepare(Source,vxc::AssetRenderOwner::Object,Pages).serial,uint64(0));
    bool Published=false;int32 Calls=0;
    FAdapter Adapter([&](const auto& Before,const auto& After,const auto& Touched){
        ++Calls;return Published&&!Before.objectOwns(Source.Provenance)&&After.objectOwns(Source.Provenance)&&Touched.size()==1;
    });
    Source.Admission.HasEditedTerrainCells=true;
    TestEqual(TEXT("Edited footprint cannot use unedited pilot"),Adapter.Prepare(Source,vxc::AssetRenderOwner::Object,Pages).serial,uint64(0));
    Source.Admission.HasEditedTerrainCells=false;
    auto Incomplete=Pages;Incomplete[0].backends=vxc::AssetCpu;
    TestEqual(TEXT("Missing active GPU backend refused"),Adapter.Prepare(Source,vxc::AssetRenderOwner::Object,Incomplete).serial,uint64(0));
    const auto Before=Adapter.Visible();const auto Ticket=Adapter.Prepare(Source,vxc::AssetRenderOwner::Object,Pages);
    TestTrue(TEXT("Evidence admits a prepared transaction"),Ticket.serial!=0);
    TestFalse(TEXT("Prepared ownership is not visible"),Before->objectOwns(Source.Provenance));
    const auto Target=Adapter.Prepared(Ticket);TestTrue(TEXT("Prepared ownership excludes target tree"),Target&&Target->objectOwns(Source.Provenance));
    Adapter.MarkObjectReady(Ticket,1);Adapter.MarkPageReady(Ticket,Pages[0],vxc::AssetCpu,1);
    TestFalse(TEXT("Cannot publish partial backend set"),Adapter.Commit(Ticket));TestEqual(TEXT("Callback never ran early"),Calls,0);
    Adapter.MarkPageReady(Ticket,Pages[0],vxc::AssetGpu,1);
    TestFalse(TEXT("Renderer refusal preserves old scene"),Adapter.Commit(Ticket));
    TestFalse(TEXT("Refused snapshot stays terrain-owned"),Adapter.Visible()->objectOwns(Source.Provenance));
    Published=true;TestTrue(TEXT("Atomic renderer callback allows publication"),Adapter.Commit(Ticket));
    TestTrue(TEXT("New immutable snapshot object-owned"),Adapter.Visible()->objectOwns(Source.Provenance));
    TestFalse(TEXT("Captured old job snapshot remains unchanged"),Before->objectOwns(Source.Provenance));
    TestFalse(TEXT("Old-generation jobs rejected"),Adapter.AcceptsVisibleJob(0));
    TestTrue(TEXT("Stable GUID repeatable"),StableId(Source.Provenance)==StableId(Source.Provenance));
    return true;
}
#endif
