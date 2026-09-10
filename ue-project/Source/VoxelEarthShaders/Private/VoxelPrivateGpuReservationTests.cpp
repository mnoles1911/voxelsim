#include "VoxelBrickPool.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "voxelcore/brickpack.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace PrivateGpuReservationTests {
static FVoxelBrickCpuPackRef Pack(const FVoxelBrickChunkKey& K){
    const auto Source=vxc::packChunkBricksCanonical([](int X,int,int)->vxc::MaterialId{return X%3?16:0;});
    auto P=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>();P->OriginVoxel=FIntVector(K.X*32,K.Y*32,K.Z*32);
    for(const auto& D:Source.descs){P->Desc.Add(D.OccWord);P->Desc.Add(D.MatWord);}for(auto W:Source.occ)P->Occ.Add(W);for(auto W:Source.mat)P->Mat.Add(W);
    P->BrickSolid=Source.brickSolid;P->bAnySolid=Source.anySolid;P->bAllSolid=Source.allSolid;return P;
}
class FCommand final:public IAutomationLatentCommand {
    FAutomationTestBase* Test;int Mode,Phase=0;double Start=FPlatformTime::Seconds();
    TUniquePtr<FVoxelBrickPool> Pool;FVoxelPrivateGpuReservationRef Token,Old;
    FVoxelBrickEvictionPinTicket Pins;TArray<FVoxelBrickPreparedReplacement> Pages;FString Error;
public:
    FCommand(FAutomationTestBase* T,int M):Test(T),Mode(M){}
    ~FCommand(){Pool.Reset();FlushRenderingCommands();}
    bool Update()override{
        if(FPlatformTime::Seconds()-Start>40.){Test->AddError(TEXT("private GPU reservation timed out"));return true;}
        if(Phase==0){
            FVoxelBrickPoolConfig C;C.ChunkCapacity=Mode==2?1:4;C.OccWordCapacity=Mode==1?16:4096;C.MatWordCapacity=16384;
            Pool=MakeUnique<FVoxelBrickPool>();Pool->InitPrivateGpuReservationTestPool(C);
            const FVoxelBrickChunkKey A{0,0,0,0},B{1,0,0,0};Pins=Pool->AcquireEvictionPins(TArray<FVoxelBrickChunkKey>{A,B});
            FVoxelBrickPreparedReplacement P;P.Key=A;P.CpuPack=Pack(A);Pages.Add(P);
            if(Mode==2){auto Q=P;Q.Key=B;Q.CpuPack=Pack(B);Pages.Add(Q);
                Test->TestFalse(TEXT("descriptor exhaustion refuses atomically without eviction"),Pool->BeginPrivateGpuReservation(Pages,Pins,Error).IsValid());Pages.Pop();}
            Test->TestFalse(TEXT("zero byte budget refuses without claiming"),Pool->BeginPrivateGpuReservation(Pages,Pins,Error,0).IsValid());
            auto Invalid=Pages;Invalid[0].CpuPack=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>(*Pages[0].CpuPack);
            Invalid[0].CpuPack->bAnySolid=!Invalid[0].CpuPack->bAnySolid;
            Test->TestFalse(TEXT("malformed any-solid flag refuses before allocation"),Pool->BeginPrivateGpuReservation(Invalid,Pins,Error).IsValid());
            Invalid[0].CpuPack->bAnySolid=Pages[0].CpuPack->bAnySolid;Invalid[0].CpuPack->bAllSolid=!Pages[0].CpuPack->bAllSolid;
            Test->TestFalse(TEXT("malformed all-solid flag refuses before allocation"),Pool->BeginPrivateGpuReservation(Invalid,Pins,Error).IsValid());
            Invalid=Pages;Invalid[0].Key.Level=-1;
            Test->TestFalse(TEXT("negative level refuses without clamping"),Pool->BeginPrivateGpuReservation(Invalid,Pins,Error).IsValid());
            Invalid[0].Key.Level=16;
            Test->TestFalse(TEXT("unsupported high level refuses without clamping"),Pool->BeginPrivateGpuReservation(Invalid,Pins,Error).IsValid());
            Token=Pool->BeginPrivateGpuReservation(Pages,Pins,Error);
            if(!Test->TestTrue(TEXT("private claim admitted"),Token.IsValid()))return true;
            Test->TestFalse(TEXT("second reservation is bounded"),Pool->BeginPrivateGpuReservation(Pages,Pins,Error).IsValid());
            Test->TestEqual(TEXT("admission exposes no resident"),Pool->GetNumResidentChunks(),0);
            if(Mode==3){Token.Reset();Phase=4;}else Phase=1;return false;
        }
        if(Phase==4){
            auto Retry=Pool->BeginPrivateGpuReservation(Pages,Pins,Error);
            if(!Retry)return false;
            Test->TestTrue(TEXT("abandoned reservation retains admission until bounded timeout"),FPlatformTime::Seconds()-Start>=30.);
            Test->TestEqual(TEXT("abandoned claims never expose a resident"),Pool->GetNumResidentChunks(),0);
            Token=Retry;Pool->CancelPrivateGpuReservation(Token);return true;
        }
        const auto Status=Pool->PollPrivateGpuReservation(Token,Error);
        if(Phase==1){
            if(Status==EVoxelPrivateGpuReservationStatus::Pending)return false;
            Test->TestEqual(TEXT("claim proof result"),Status,Mode==1?EVoxelPrivateGpuReservationStatus::Failed:EVoxelPrivateGpuReservationStatus::ReadyPrivate);
            Test->TestEqual(TEXT("private completed claims expose no resident"),Pool->GetNumResidentChunks(),0);
            if(Mode==1)return true;
            Test->TestTrue(TEXT("cancel private success"),Pool->CancelPrivateGpuReservation(Token));Phase=2;return false;
        }
        if(Phase==2){
            auto Retry=Pool->BeginPrivateGpuReservation(Pages,Pins,Error);if(!Retry)return false;
            Test->TestEqual(TEXT("old token remains cancelled"),Pool->PollPrivateGpuReservation(Token,Error),EVoxelPrivateGpuReservationStatus::Cancelled);
            Old=Token;Token=Retry;
            // Reset immediately, while claim/readback may still be queued. It
            // must enqueue old-buffer frees before any descriptor reuse.
            Pool->Reset();Phase=3;return false;
        }
        if(Phase==3){
            Test->TestEqual(TEXT("reset cancels pending reservation"),Status,EVoxelPrivateGpuReservationStatus::Cancelled);
            Test->TestFalse(TEXT("stale cancelled token cannot cancel replacement"),Pool->CancelPrivateGpuReservation(Old));
            Test->TestEqual(TEXT("reset and pending cleanup expose no resident"),Pool->GetNumResidentChunks(),0);
            return true;
        }
        return true;
    }
};
static bool Run(FAutomationTestBase* Test,int Mode){
    if(GUsingNullRHI||!GIsRHIInitialized){Test->AddWarning(TEXT("Requires real RHI; no GPU reservation proof under NullRHI"));return true;}
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FCommand>(Test,Mode));return true;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPrivateGpuReservationSuccess,"Voxel.Objects.PrivateGpuReservation.SuccessCancelReset",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelPrivateGpuReservationSuccess::RunTest(const FString&){return PrivateGpuReservationTests::Run(this,0);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPrivateGpuReservationExhaustion,"Voxel.Objects.PrivateGpuReservation.GpuExhaustion",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelPrivateGpuReservationExhaustion::RunTest(const FString&){return PrivateGpuReservationTests::Run(this,1);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPrivateGpuReservationDescriptors,"Voxel.Objects.PrivateGpuReservation.DescriptorExhaustion",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelPrivateGpuReservationDescriptors::RunTest(const FString&){return PrivateGpuReservationTests::Run(this,2);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPrivateGpuReservationAbandoned,"Voxel.Objects.PrivateGpuReservation.AbandonedTimeout",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelPrivateGpuReservationAbandoned::RunTest(const FString&){return PrivateGpuReservationTests::Run(this,3);}
#endif
