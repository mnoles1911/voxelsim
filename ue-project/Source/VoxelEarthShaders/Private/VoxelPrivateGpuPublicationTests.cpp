#include "VoxelBrickPool.h"
#include "VoxelPreparedIndexTestSupport.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "voxelcore/brickpack.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace PrivateGpuPublicationDraftTests {
class FCommand final:public IAutomationLatentCommand {
    FAutomationTestBase* Test;TUniquePtr<FVoxelBrickPool> Pool;FVoxelPrivateGpuReservationRef Token,Committed;
    FVoxelBrickEvictionPinTicket Pins;TArray<FVoxelBrickPreparedReplacement> Pages;
    const FVoxelBrickChunkKey A{0,0,0,0},B{1,0,0,0},Absent{2,0,0,0};
    int32 Phase=0,Deliveries=0;double Started=FPlatformTime::Seconds();FString Error;
    TArray<FVoxelBrickIndexEntry> Added,Removed;int32 OldSlotA=-1;
    FVoxelBrickCpuPackRef Pack(const FVoxelBrickChunkKey& K){
        const auto Source=vxc::packChunkBricksCanonical([](int X,int Y,int)->vxc::MaterialId{return (X+Y)%3?16:0;});
        auto P=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>();P->OriginVoxel=FIntVector(K.X*32,K.Y*32,K.Z*32);
        for(const auto& D:Source.descs){P->Desc.Add(D.OccWord);P->Desc.Add(D.MatWord);}for(auto W:Source.occ)P->Occ.Add(W);for(auto W:Source.mat)P->Mat.Add(W);
        P->BrickSolid=Source.brickSolid;P->bAnySolid=Source.anySolid;P->bAllSolid=Source.allSolid;return P;
    }
public:
    explicit FCommand(FAutomationTestBase* T):Test(T){}
    ~FCommand(){Pool.Reset();FlushRenderingCommands();}
    bool Update()override{
        if(FPlatformTime::Seconds()-Started>45.){Test->AddError(TEXT("GPU publication draft test timeout"));return true;}
        if(Phase==0){
            FVoxelBrickPoolConfig C;C.ChunkCapacity=8;C.OccWordCapacity=8192;C.MatWordCapacity=32768;
            Pool=MakeUnique<FVoxelBrickPool>();Pool->InitPrivateGpuReservationTestPool(C);
            TArray<FVoxelBrickIndexEntry> Initial;
            VoxelPreparedIndexTestSupport::SetSink(*Pool,[this](const FVoxelBrickIndexDelta& D){++Deliveries;Added=D.Added;Removed=D.Removed;
                Test->TestFalse(TEXT("delivery callback cannot cancel transferred GPU ranges"),Pool->CancelPrivateGpuReservation(Token));
                Test->TestFalse(TEXT("delivery callback cannot validate consumed commit twice"),Pool->ValidatePrivateGpuCommit(Token));},Initial);
            Pins=Pool->AcquireEvictionPins(TArray<FVoxelBrickChunkKey>{A,B,Absent});
            for(const auto& K:{A,B}){auto& P=Pages.AddDefaulted_GetRef();P.Key=K;P.CpuPack=Pack(K);}
            Token=Pool->BeginPrivateGpuReservation(Pages,Pins,Error);
            if(!Test->TestTrue(TEXT("two private GPU pages admitted"),Token.IsValid()))return true;
            Phase=1;return false;
        }
        if(Phase==1||Phase==3){
            // No-op ordinary flushes change the old broad mutation counter but
            // cannot own private slots. Preparation must survive these ticks.
            Pool->Flush();
            const auto Status=Pool->PollPrivateGpuReservation(Token,Error);
            if(Status==EVoxelPrivateGpuReservationStatus::Pending)return false;
            if(!Test->TestEqual(TEXT("private proof survives ordinary empty flush"),Status,EVoxelPrivateGpuReservationStatus::ReadyPrivate))return true;
            Test->TestFalse(TEXT("zero host budget refuses without publication"),Pool->PreparePrivateGpuCommit(Token,TArray<FVoxelBrickChunkKey>{Absent},Error,0));
            Test->TestFalse(TEXT("overlapping absence refuses"),Pool->PreparePrivateGpuCommit(Token,TArray<FVoxelBrickChunkKey>{A},Error));
            const int Before=Deliveries;
            if(!Test->TestTrue(TEXT("GPU batch reserves index credits"),Pool->PreparePrivateGpuCommit(Token,TArray<FVoxelBrickChunkKey>{Absent},Error)))return true;
            Test->TestEqual(TEXT("prepare does not deliver index"),Deliveries,Before);
            Test->TestTrue(TEXT("exact absence evidence available"),Pool->PrivateGpuCommitCoversAbsent(Token,Absent));
            Test->TestFalse(TEXT("replacement not claimed absent"),Pool->PrivateGpuCommitCoversAbsent(Token,A));
            if(Phase==1){
                // Final delta has a separate strict version guard.
                Pool->Flush();
                Test->TestFalse(TEXT("post-reservation flush invalidates final packet"),Pool->ValidatePrivateGpuCommit(Token));
                Test->TestEqual(TEXT("failed final validation leaves residents absent"),Pool->GetNumResidentChunks(),0);
                Pool->CancelPrivateGpuReservation(Token);Phase=2;return false;
            }
            Pool->CommitPrivateGpuReservation(Token);Committed=Token;
            Test->TestEqual(TEXT("whole batch delivers once"),Deliveries,Before+1);
            Test->TestEqual(TEXT("two entries published"),Added.Num(),2);
            Test->TestEqual(TEXT("both residents visible to registry together"),Pool->GetNumResidentChunks(),2);
            Test->TestFalse(TEXT("explicit absent page stays absent"),Pool->SnapshotAllocation(Absent).bPresent);
            Test->TestFalse(TEXT("committed token cannot cancel residents"),Pool->CancelPrivateGpuReservation(Token));
            Test->TestEqual(TEXT("committed token records transfer"),Pool->PollPrivateGpuReservation(Token,Error),EVoxelPrivateGpuReservationStatus::Published);
            if(OldSlotA<0){
                OldSlotA=Pool->SnapshotAllocation(A).Slot;
                for(auto& P:Pages){const auto Old=Pool->SnapshotAllocation(P.Key);P.ExpectedSlot=Old.Slot;P.ExpectedSequence=Old.AddSequence;}
                Token=Pool->BeginPrivateGpuReservation(Pages,Pins,Error);if(!Test->TestTrue(TEXT("replacement privately admitted"),Token.IsValid()))return true;
                Phase=3;return false;
            }
            Test->TestEqual(TEXT("both old GPU entries retired in batch"),Removed.Num(),2);
            Test->TestTrue(TEXT("new private slot replaces old GPU allocation"),Pool->SnapshotAllocation(A).Slot!=OldSlotA);
            FlushRenderingCommands();
            Test->TestEqual(TEXT("renderer drain preserves both new residents"),Pool->GetNumResidentChunks(),2);
            return true;
        }
        if(Phase==2){
            Pool->PollPrivateGpuReservation(Token,Error);
            auto Retry=Pool->BeginPrivateGpuReservation(Pages,Pins,Error);if(!Retry)return false;
            Token=Retry;Phase=3;return false;
        }
        return true;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPrivateGpuPublicationDraftTest,"Voxel.Objects.PrivateGpuPublication.Batch",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelPrivateGpuPublicationDraftTest::RunTest(const FString&){
    if(GUsingNullRHI||!GIsRHIInitialized){AddWarning(TEXT("Real GPU required"));return true;}
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<PrivateGpuPublicationDraftTests::FCommand>(this));return true;
}
#endif
