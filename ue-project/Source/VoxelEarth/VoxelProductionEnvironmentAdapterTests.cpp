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

// Staging-only contract test: no RHI allocation, pool write or world publication.
// GPU descriptor contents are covered by the separate GPU stamp/readback test;
// this fixture models the successful producer's payload metadata explicitly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelProductionEmptyStageTest,"Voxel.Objects.ProductionEmptyStage",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelProductionEmptyStageTest::RunTest(const FString&)
{
    using namespace VoxelProductionEnvironment;
    FSource Source;Source.Provenance.worldSeed=9;Source.Provenance.providerFingerprint=17;
    Source.Provenance.catalogFingerprint=29;Source.Admission={true,true,true,false};
    const vxc::AssetRenderPage Page{-1,-2,-3,0,vxc::AssetCpu|vxc::AssetGpu};
    int32 Calls=0;
    FAdapter Adapter([&](const auto&,const auto&,const auto&){++Calls;return true;});
    const auto Ticket=Adapter.Prepare(Source,vxc::AssetRenderOwner::Object,{Page});
    if(!TestTrue(TEXT("prepared empty replacement"),Ticket.serial!=0))return false;
    const uint64 Generation=Adapter.Prepared(Ticket)->generation;
    Adapter.MarkObjectReady(Ticket,1);
    TestFalse(TEXT("missing CPU output is not air"),Adapter.StageCpuPage(Ticket,Page,Generation,{},{}));
    auto Air=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>();Air->Desc.SetNumZeroed(128);
    auto Short=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>();Short->Desc.SetNumZeroed(126);
    TestFalse(TEXT("partial descriptor page refused"),Adapter.StageCpuPage(Ticket,Page,Generation,Short,{}));
    TestFalse(TEXT("stale empty CPU result refused"),Adapter.StageCpuPage(Ticket,Page,Generation+1,Air,{}));
    TestTrue(TEXT("complete CPU air pack retained"),Adapter.StageCpuPage(Ticket,Page,Generation,Air,{}));
    TestFalse(TEXT("CPU-only staging cannot publish both backends"),Adapter.Commit(Ticket));
    auto GpuResult=[&](){FVoxelGpuMeshJobResult R;R.Status=EVoxelGpuMeshJobStatus::Success;
        R.bPublicationHeld=true;R.OwnershipGeneration=Generation;
        R.BrickVolume=MakeShared<FVoxelGpuBrickPayload,ESPMode::ThreadSafe>();R.BrickVolume->BrickCount=64;return R;};
    auto Missing=GpuResult();Missing.BrickVolume.Reset();
    TestFalse(TEXT("held success without descriptors is not air"),Adapter.StageGpuPage(Ticket,Page,MoveTemp(Missing)));
    auto Failed=GpuResult();Failed.Status=EVoxelGpuMeshJobStatus::Rejected;
    TestFalse(TEXT("failure cannot clear old page"),Adapter.StageGpuPage(Ticket,Page,MoveTemp(Failed)));
    auto Ordinary=GpuResult();Ordinary.bPublicationHeld=false;
    TestFalse(TEXT("ordinary result cannot enter held transaction"),Adapter.StageGpuPage(Ticket,Page,MoveTemp(Ordinary)));
    auto Stale=GpuResult();Stale.OwnershipGeneration++;
    TestFalse(TEXT("stale held air refused"),Adapter.StageGpuPage(Ticket,Page,MoveTemp(Stale)));
    auto WrongSize=GpuResult();WrongSize.BrickVolume->BrickCount=63;
    TestFalse(TEXT("incomplete GPU descriptor page refused"),Adapter.StageGpuPage(Ticket,Page,MoveTemp(WrongSize)));
    auto Overflow=GpuResult();Overflow.BrickVolume->MatWords=8449;
    TestFalse(TEXT("GPU invalid totals refused"),Adapter.StageGpuPage(Ticket,Page,MoveTemp(Overflow)));
    TestEqual(TEXT("staging never invokes publisher"),Calls,0);
    auto Complete=GpuResult();
    TestTrue(TEXT("held zero-arena page accepted"),Adapter.StageGpuPage(Ticket,Page,MoveTemp(Complete)));
    const auto Staged=Adapter.PreparedPages(Ticket);
    TestTrue(TEXT("both complete packs retained for replacement"),Staged&&Staged->Num()==1&&(*Staged)[0].CpuBricks==Air&&(*Staged)[0].GpuBricks.IsValid());
    TestFalse(TEXT("ownership stays unchanged during staging"),Adapter.Visible()->objectOwns(Source.Provenance));
    TestTrue(TEXT("complete staging permits atomic callback"),Adapter.Commit(Ticket));
    TestEqual(TEXT("exactly one explicit publication"),Calls,1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelProductionQuadStageTest,"Voxel.Objects.ProductionQuadStage",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelProductionQuadStageTest::RunTest(const FString&)
{
    using namespace VoxelProductionEnvironment;
    FSource Source;Source.Provenance.worldSeed=9;Source.Provenance.providerFingerprint=17;
    Source.Provenance.catalogFingerprint=29;Source.Admission={true,true,true,false};
    const vxc::AssetRenderPage Page{-1,-2,-3,0,vxc::AssetCpu|vxc::AssetGpu};
    FAdapter Adapter([](const auto&,const auto&,const auto&){return true;});
    const auto Ticket=Adapter.Prepare(Source,vxc::AssetRenderOwner::Object,{Page});
    if(!TestTrue(TEXT("prepared quad replacement"),Ticket.serial!=0))return false;
    const uint64 Generation=Adapter.Prepared(Ticket)->generation;
    auto Pack=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>();Pack->Desc.SetNumZeroed(128);
    // Distinct sentinels exercise transport only; this does not claim mesh parity.
    TArray<uint64> Cpu{11,12};
    TestTrue(TEXT("CPU output staged"),Adapter.StageCpuPage(Ticket,Page,Generation,Pack,MoveTemp(Cpu)));
    FVoxelGpuMeshJobResult R;R.Status=EVoxelGpuMeshJobStatus::Success;R.bPublicationHeld=true;
    R.OwnershipGeneration=Generation;R.BrickVolume=MakeShared<FVoxelGpuBrickPayload,ESPMode::ThreadSafe>();
    R.BrickVolume->BrickCount=64;R.Quads={21,22,23};R.NumQuads=3;
    TestTrue(TEXT("GPU readback output staged"),Adapter.StageGpuPage(Ticket,Page,MoveTemp(R)));
    const auto Staged=Adapter.PreparedPages(Ticket);
    if(!TestTrue(TEXT("one page retained"),Staged&&Staged->Num()==1))return false;
    TestTrue(TEXT("independent CPU output retained"),(*Staged)[0].CpuQuads==TArray<uint64>({11,12}));
    TestTrue(TEXT("GPU readback not discarded or confused with CPU output"),(*Staged)[0].GpuReadbackQuads==TArray<uint64>({21,22,23}));
    TestTrue(TEXT("cancel releases staged streams"),Adapter.Cancel(Ticket));
    TestTrue(TEXT("cancelled transaction has no staged pages"),Adapter.PreparedPages(Ticket)==nullptr);
    return true;
}
#endif
