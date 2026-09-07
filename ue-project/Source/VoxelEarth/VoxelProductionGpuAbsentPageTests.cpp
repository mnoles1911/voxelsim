#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelProductionEnvironmentAdapter.h"
#include "VoxelPreparedIndexTestSupport.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "voxelcore/brickpack.h"
namespace ProductionGpuAbsentPageTests {
class FCommand final : public IAutomationLatentCommand {
    FAutomationTestBase* Test;
    TUniquePtr<FVoxelBrickPool> Pool;
    FVoxelPrivateGpuReservationRef Batch;
    FVoxelBrickEvictionPinTicket Pins;
    double Started=FPlatformTime::Seconds();
    const FVoxelBrickChunkKey Present{0,0,0,0},Absent{1,0,0,0},Other{2,0,0,0};
public:
    explicit FCommand(FAutomationTestBase* InTest):Test(InTest){}
    ~FCommand(){Pool.Reset();FlushRenderingCommands();}
    bool Update() override {
        if(FPlatformTime::Seconds()-Started>40.){Test->AddError(TEXT("GPU adapter absence proof timed out"));return true;}
        FString Error;
        if(!Pool){
            Pool=MakeUnique<FVoxelBrickPool>();FVoxelBrickPoolConfig Config;
            Config.ChunkCapacity=4;Config.OccWordCapacity=8192;Config.MatWordCapacity=32768;
            Pool->InitPrivateGpuReservationTestPool(Config);
            TArray<FVoxelBrickIndexEntry> Initial;VoxelPreparedIndexTestSupport::SetSink(*Pool,[](const auto&){},Initial);
            Pins=Pool->AcquireEvictionPins(TArray<FVoxelBrickChunkKey>{Present,Absent,Other});
            const auto Source=vxc::packChunkBricksCanonical([](int X,int Y,int)->vxc::MaterialId{return (X+Y)%3?16:0;});
            auto Pack=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>();
            for(const auto& D:Source.descs){Pack->Desc.Add(D.OccWord);Pack->Desc.Add(D.MatWord);}
            for(auto W:Source.occ)Pack->Occ.Add(W);for(auto W:Source.mat)Pack->Mat.Add(W);
            Pack->BrickSolid=Source.brickSolid;Pack->bAnySolid=Source.anySolid;Pack->bAllSolid=Source.allSolid;
            TArray<FVoxelBrickPreparedReplacement> Pages;auto& P=Pages.AddDefaulted_GetRef();P.Key=Present;P.CpuPack=Pack;
            Batch=Pool->BeginPrivateGpuReservation(Pages,Pins,Error);
            return !Test->TestTrue(TEXT("actual GPU private reservation admitted"),Batch.IsValid());
        }
        Pool->Flush();
        const auto Status=Pool->PollPrivateGpuReservation(Batch,Error);
        if(Status==EVoxelPrivateGpuReservationStatus::Pending)return false;
        if(!Test->TestEqual(TEXT("actual GPU payload proof ready"),Status,EVoxelPrivateGpuReservationStatus::ReadyPrivate))return true;
        if(!Test->TestTrue(TEXT("exact absent membership reserved"),Pool->PreparePrivateGpuCommit(Batch,TArray<FVoxelBrickChunkKey>{Absent},Error)))return true;
        using namespace VoxelProductionEnvironment;
        FSource Source;Source.Provenance.worldSeed=9;Source.Provenance.providerFingerprint=17;Source.Provenance.catalogFingerprint=29;Source.Admission={true,true,true,false};
        const std::vector<vxc::AssetRenderPage> Pages{{0,0,0,0,vxc::AssetCpu|vxc::AssetGpu},{1,0,0,0,vxc::AssetCpu|vxc::AssetGpu},{2,0,0,0,vxc::AssetCpu|vxc::AssetGpu}};
        FAdapter Adapter([&](const auto&,const auto&,const auto&){return Pool->ValidatePrivateGpuCommit(Batch);});
        const auto Ticket=Adapter.Prepare(Source,vxc::AssetRenderOwner::Object,Pages);
        Test->TestTrue(TEXT("adapter prepares hidden target"),Ticket.serial!=0);
        Test->TestFalse(TEXT("replacement is not absent evidence"),Adapter.StageAbsentPage(Ticket,Pages[0],1,*Pool,Batch));
        Test->TestFalse(TEXT("pin alone is not reserved absence evidence"),Adapter.StageAbsentPage(Ticket,Pages[2],1,*Pool,Batch));
        Test->TestFalse(TEXT("wrong generation cannot stage absence"),Adapter.StageAbsentPage(Ticket,Pages[1],0,*Pool,Batch));
        Test->TestTrue(TEXT("real GPU token stages exact absence"),Adapter.StageAbsentPage(Ticket,Pages[1],1,*Pool,Batch));
        const auto* Staged=Adapter.PreparedPages(Ticket);
        Test->TestTrue(TEXT("absence carries no invented CPU or GPU payload"),Staged&&Staged->Num()==1&&(*Staged)[0].ValidatedAbsent&&!(*Staged)[0].CpuBricks&&!(*Staged)[0].GpuBricks);
        Pool->Flush();
        Test->TestFalse(TEXT("invalidated packet cannot restage absence"),Adapter.StageAbsentPage(Ticket,Pages[1],1,*Pool,Batch));
        Test->TestFalse(TEXT("private preparation leaves ownership unchanged"),Adapter.Visible()->objectOwns(Source.Provenance));
        Test->TestEqual(TEXT("private preparation exposes no residents"),Pool->GetNumResidentChunks(),0);
        Adapter.Cancel(Ticket);Pool->CancelPrivateGpuReservation(Batch);return true;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelProductionGpuAbsentPageTest,"Voxel.Objects.ProductionGpuAbsentPage",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelProductionGpuAbsentPageTest::RunTest(const FString&){
    if(GUsingNullRHI||!GIsRHIInitialized){AddWarning(TEXT("Real GPU required"));return true;}
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<ProductionGpuAbsentPageTests::FCommand>(this));return true;
}
#endif
