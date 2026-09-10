#include "VoxelBrickPool.h"
#include "VoxelMarchChunkIndex.h"
#include "Misc/AutomationTest.h"
#include "HAL/PlatformProcess.h"
#include "RenderingThread.h"
#include "voxelcore/brickpack.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace PrivateGpuRealIndexDraftTests {
struct FView{uint64 Generation[3]{};uint32 Value[3]{},Occupied[3]{};};
class FCommand final:public IAutomationLatentCommand {
    FAutomationTestBase* Test;TUniquePtr<FVoxelBrickPool> Pool;TUniquePtr<FVoxelMarchChunkIndex> Index;
    FVoxelPrivateGpuReservationRef Token;FVoxelBrickEvictionPinTicket Pins;
    TArray<FVoxelBrickPreparedReplacement> Pages;FString Error;int32 Phase=0;
    double Started=FPlatformTime::Seconds();
    const FVoxelBrickChunkKey A{0,0,0,0},B{1,0,0,0},Absent{2,0,0,0};
    FView LastPublished;
    FVoxelBrickCpuPackRef Pack(const FVoxelBrickChunkKey& K){
        const auto Source=vxc::packChunkBricksCanonical([](int X,int Y,int)->vxc::MaterialId{return (X+Y)%3?16:0;});
        auto P=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>();P->OriginVoxel=FIntVector(K.X*32,K.Y*32,K.Z*32);
        for(const auto& D:Source.descs){P->Desc.Add(D.OccWord);P->Desc.Add(D.MatWord);}for(auto W:Source.occ)P->Occ.Add(W);for(auto W:Source.mat)P->Mat.Add(W);
        P->BrickSolid=Source.brickSolid;P->bAnySolid=Source.anySolid;P->bAllSolid=Source.allSolid;return P;
    }
    FView ReadView(){
        auto V=MakeShared<FView,ESPMode::ThreadSafe>();auto* LocalIndex=Index.Get();
        ENQUEUE_RENDER_COMMAND(ReadPrivateGpuRealIndex)([LocalIndex,V](FRHICommandListImmediate&){
            for(uint32 I=0;I<3;++I)LocalIndex->DebugReadOrderedForTest(I,V->Generation[I],V->Value[I],V->Occupied[I]);
        });
        FlushRenderingCommands();return *V;
    }
    bool Same(const FView& L,const FView& R){return FMemory::Memcmp(&L,&R,sizeof(FView))==0;}
    void Publish(){
        FEvent* Gate=FPlatformProcess::GetSynchEventFromPool(true);
        ENQUEUE_RENDER_COMMAND(HoldPrivateGpuRealIndexDelivery)([Gate](FRHICommandListImmediate&){Gate->Wait(10000);});
        Pool->CommitPrivateGpuReservation(Token);
        Test->TestTrue(TEXT("actual queued index packet retains credits before RT execution"),Index->GetReservedPilotIndexBytes()>0);
        Gate->Trigger();LastPublished=ReadView();FPlatformProcess::ReturnSynchEventToPool(Gate);
        Test->TestEqual(TEXT("executed real packet releases credits"),Index->GetReservedPilotIndexBytes(),uint64(0));
        for(int32 I=0;I<2;++I){
            Test->TestTrue(TEXT("complete batch resident bit installed"),(LastPublished.Value[I]&FVoxelMarchChunkIndex::kResidentBit)!=0);
            Test->TestEqual(TEXT("real index names exact published slot"),LastPublished.Value[I]&FVoxelMarchChunkIndex::kSlotMask,uint32(Pool->SnapshotAllocation(Pages[I].Key).Slot));
            Test->TestEqual(TEXT("both index cells installed at one generation"),LastPublished.Generation[I],LastPublished.Generation[0]);
        }
        Test->TestFalse(TEXT("reserved absent cell remains absent in real index"),(LastPublished.Value[2]&FVoxelMarchChunkIndex::kResidentBit)!=0);
    }
public:
    explicit FCommand(FAutomationTestBase* T):Test(T){}
    ~FCommand(){
        if(Pool){TArray<FVoxelBrickIndexEntry> Snapshot;Pool->SetIndexSink(nullptr,Snapshot);}
        Pool.Reset();FlushRenderingCommands();Index.Reset();FlushRenderingCommands();
    }
    bool Update()override{
        if(FPlatformTime::Seconds()-Started>45.){Test->AddError(TEXT("GPU real-index draft test timeout"));return true;}
        if(Phase==0){
            FVoxelBrickPoolConfig C;C.ChunkCapacity=8;C.OccWordCapacity=8192;C.MatWordCapacity=32768;
            Pool=MakeUnique<FVoxelBrickPool>();Pool->InitPrivateGpuReservationTestPool(C);
            Index=MakeUnique<FVoxelMarchChunkIndex>();TArray<FVoxelBrickIndexEntry> Empty,Snapshot;Index->DebugSeedOrderedForTest(Empty);
            // Real production reservation/delivery implementation, local index.
            Pool->SetIndexSink([this](const auto& D){Index->DebugApplyOrderedForTest(D);},Snapshot,
                [this](const auto& D){return Index->DebugPrepareIndexForTest(D);});
            Pins=Pool->AcquireEvictionPins(TArray<FVoxelBrickChunkKey>{A,B,Absent});
            for(const auto& K:{A,B}){auto& P=Pages.AddDefaulted_GetRef();P.Key=K;P.CpuPack=Pack(K);}
            LastPublished=ReadView();Token=Pool->BeginPrivateGpuReservation(Pages,Pins,Error);
            if(!Test->TestTrue(TEXT("real-index fixture private GPU claims admitted"),Token.IsValid()))return true;
            Phase=1;return false;
        }
        if(Phase==2){
            Pool->PollPrivateGpuReservation(Token,Error);
            if(Index->GetReservedPilotIndexBytes()!=0)return false;
            Test->TestTrue(TEXT("cancelled reservation leaves installed real index unchanged"),Same(LastPublished,ReadView()));
            auto Retry=Pool->BeginPrivateGpuReservation(Pages,Pins,Error);if(!Retry)return false;
            Token=Retry;Phase=3;return false;
        }
        Pool->Flush(); // unrelated/no-op index mutation cannot poison private claims
        const auto Status=Pool->PollPrivateGpuReservation(Token,Error);
        if(Status==EVoxelPrivateGpuReservationStatus::Pending)return false;
        if(!Test->TestEqual(TEXT("actual GPU proof completes"),Status,EVoxelPrivateGpuReservationStatus::ReadyPrivate))return true;
        if(Phase==1){
            Index->DebugSetPilotBudgetForTest(0);
            Test->TestFalse(TEXT("real zero-credit pressure refuses commit preparation"),Pool->PreparePrivateGpuCommit(Token,TArray<FVoxelBrickChunkKey>{Absent},Error));
            Test->TestTrue(TEXT("credit refusal preserves installed real index"),Same(LastPublished,ReadView()));
            Test->TestEqual(TEXT("credit refusal preserves absent host residents"),Pool->GetNumResidentChunks(),0);
            Index->DebugSetPilotBudgetForTest(128ull*1024*1024);
            FVoxelBrickIndexDelta None;auto Other=Index->DebugPrepareIndexForTest(None);
            const uint64 Held=Index->GetReservedPilotIndexBytes();
            if(!Test->TestTrue(TEXT("competing real packet reserves credits"),Other.IsValid()&&Held>0))return true;
            Index->DebugSetPilotBudgetForTest(Held);
            Test->TestFalse(TEXT("competing packet exhausts available index credits"),Pool->PreparePrivateGpuCommit(Token,TArray<FVoxelBrickChunkKey>{Absent},Error));
            Other.Reset();Test->TestEqual(TEXT("competing cancellation releases exact credits"),Index->GetReservedPilotIndexBytes(),uint64(0));
            Index->DebugSetPilotBudgetForTest(128ull*1024*1024);
            if(!Test->TestTrue(TEXT("real index reservation prepared"),Pool->PreparePrivateGpuCommit(Token,TArray<FVoxelBrickChunkKey>{Absent},Error)))return true;
            Test->TestTrue(TEXT("prepared GPU token owns real packet credits"),Index->GetReservedPilotIndexBytes()>0);
            Test->TestTrue(TEXT("preparation leaves rendered index generation unchanged"),Same(LastPublished,ReadView()));
            Pool->CancelPrivateGpuReservation(Token);Phase=2;return false;
        }
        if(!Test->TestTrue(TEXT("actual index packet preflight accepts complete batch"),Pool->PreparePrivateGpuCommit(Token,TArray<FVoxelBrickChunkKey>{Absent},Error)))return true;
        Test->TestTrue(TEXT("before commit retains complete prior index"),Same(LastPublished,ReadView()));
        const FView Before=LastPublished;Publish();
        Test->TestEqual(TEXT("one complete delta advances installed generation once"),LastPublished.Generation[0],Before.Generation[0]+1);
        if(Phase==3){
            for(auto& P:Pages){const auto Old=Pool->SnapshotAllocation(P.Key);P.ExpectedSlot=Old.Slot;P.ExpectedSequence=Old.AddSequence;}
            Token=Pool->BeginPrivateGpuReservation(Pages,Pins,Error);
            if(!Test->TestTrue(TEXT("private replacement of real-index residents admitted"),Token.IsValid()))return true;
            Phase=4;return false;
        }
        Test->TestTrue(TEXT("replacement changed both installed slots"),LastPublished.Value[0]!=Before.Value[0]&&LastPublished.Value[1]!=Before.Value[1]);
        Test->TestEqual(TEXT("old and new batch never coexist in host registry"),Pool->GetNumResidentChunks(),2);
        return true;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPrivateGpuRealIndexDraftTest,"Voxel.Objects.PrivateGpuPublication.RealIndexCredits",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelPrivateGpuRealIndexDraftTest::RunTest(const FString&){
    if(GUsingNullRHI||!GIsRHIInitialized){AddWarning(TEXT("Real GPU required"));return true;}
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<PrivateGpuRealIndexDraftTests::FCommand>(this));return true;
}
#endif
