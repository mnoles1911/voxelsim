#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelProductionEnvironmentAdapter.h"
#include "Misc/AutomationTest.h"
#include "VoxelPreparedIndexTestSupport.h"

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
    TestTrue(TEXT("Publication transfers the preallocated target snapshot"),Adapter.Visible().Get()==Target.Get());
    TestFalse(TEXT("Captured old job snapshot remains unchanged"),Before->objectOwns(Source.Provenance));
    TestFalse(TEXT("Old-generation jobs rejected"),Adapter.AcceptsVisibleJob(0));
    TestTrue(TEXT("Stable GUID repeatable"),StableId(Source.Provenance)==StableId(Source.Provenance));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelProductionAbsentPageTest,"Voxel.Objects.ProductionAbsentPage",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelProductionAbsentPageTest::RunTest(const FString&)
{
    using namespace VoxelProductionEnvironment;
    FVoxelBrickPool Pool;FVoxelBrickPoolConfig Config;Config.ChunkCapacity=4;Config.OccWordCapacity=1024;Config.MatWordCapacity=1024;Pool.Init(Config);
    TArray<FVoxelBrickIndexEntry> Initial;VoxelPreparedIndexTestSupport::SetSink(Pool,[](const auto&){},Initial);
    auto Air=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>();Air->Desc.SetNumZeroed(128);
    const FVoxelBrickChunkKey Present{0,0,0,0},Absent{1,0,0,0};
    Pool.AddChunkFromCpu(Air,Present,FVoxelBrickChunkShading{});Pool.Flush();
    const auto Allocation=Pool.SnapshotAllocation(Present);
    TArray<FVoxelBrickPreparedReplacement> Replacements;
    auto& Replacement=Replacements.AddDefaulted_GetRef();Replacement.Key=Present;Replacement.ExpectedSlot=Allocation.Slot;Replacement.ExpectedSequence=Allocation.AddSequence;Replacement.CpuPack=Air;
    const TArray<FVoxelBrickChunkKey> Keys{Present,Absent},AbsentKeys{Absent};
    const auto Pins=Pool.AcquireEvictionPins(Keys);
    auto Batch=Pool.PreparePreparedBatch(Replacements,Pins,AbsentKeys);
    if(!TestTrue(TEXT("Complete footprint reservation admitted"),Batch.IsValid()))return false;
    FSource Source;Source.Provenance.worldSeed=9;Source.Provenance.providerFingerprint=17;Source.Provenance.catalogFingerprint=29;Source.Admission={true,true,true,false};
    const std::vector<vxc::AssetRenderPage> Pages{{0,0,0,0,vxc::AssetCpu},{1,0,0,0,vxc::AssetCpu}};
    FAdapter Adapter([&](const auto&,const auto&,const auto&){return Pool.ValidatePreparedBatch(Batch);},vxc::AssetCpu);
    const auto Ticket=Adapter.Prepare(Source,vxc::AssetRenderOwner::Object,Pages);
    TestFalse(TEXT("Present page cannot claim absence"),Adapter.StageAbsentPage(Ticket,Pages[0],1,Pool,Batch));
    TestFalse(TEXT("Absent page requires target generation"),Adapter.StageAbsentPage(Ticket,Pages[1],0,Pool,Batch));
    TestTrue(TEXT("Pinned validated absence stages without a payload"),Adapter.StageAbsentPage(Ticket,Pages[1],1,Pool,Batch));
    TestFalse(TEXT("Absence cannot be overwritten with a payload"),Adapter.StageCpuPage(Ticket,Pages[1],1,Air,{}));
    const auto Staged=Adapter.PreparedPages(Ticket);
    TestTrue(TEXT("Absence is explicit and retains no fake brick data"),Staged&&Staged->Num()==1&&(*Staged)[0].ValidatedAbsent&&!(*Staged)[0].CpuBricks&&!(*Staged)[0].GpuBricks);
    TestTrue(TEXT("Allocated page still needs real data"),Adapter.StageCpuPage(Ticket,Pages[0],1,Air,{}));
    Adapter.MarkObjectReady(Ticket,1);
    Pool.AddChunkFromCpu(Air,Absent,FVoxelBrickChunkShading{});
    TestFalse(TEXT("New allocation invalidates absence evidence"),Adapter.StageAbsentPage(Ticket,Pages[1],1,Pool,Batch));
    TestFalse(TEXT("Final callback revalidates the whole token"),Adapter.Commit(Ticket));
    TestFalse(TEXT("Invalidated absence leaves terrain ownership"),Adapter.Visible()->objectOwns(Source.Provenance));
    Adapter.Cancel(Ticket);Batch.Reset();Pool.ReleaseEvictionPins(Pins);
    TArray<FVoxelBrickIndexEntry> Ignored;Pool.SetIndexSink(nullptr,Ignored);
    return true;
}
#endif
