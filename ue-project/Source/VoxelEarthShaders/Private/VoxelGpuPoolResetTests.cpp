#include "VoxelBrickPool.h"
#include "VoxelPreparedIndexTestSupport.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "voxelcore/brickpack.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace GpuPoolResetTests {
class FCommand final:public IAutomationLatentCommand {
    struct FRead {
        TUniquePtr<FRHIGPUBufferReadback> Buffer;
        TArray<uint32> Data;
        std::atomic<bool> Issued{false},Done{false};
        uint32 StateWords=0,BitmapWords=0,SideWords=0,TableWords=0;
    };
    FAutomationTestBase* Test;TUniquePtr<FVoxelBrickPool> Pool;
    TSharedPtr<FRead,ESPMode::ThreadSafe> Read;
    FVoxelBrickPoolBuffersRef OldBuffers;
    TArray<FVoxelBrickIndexEntry> Visible;
    uint32 FirstOccBump=0,FirstMatBump=0;
    int32 Phase=0;double Started=FPlatformTime::Seconds();
    FVoxelBrickChunkKey Key{17,-9,3,0};
    FVoxelBrickCpuPackRef Pack(){
        auto Source=vxc::packChunkBricksCanonical([](int X,int,int)->vxc::MaterialId{return X%3?16:0;});
        auto P=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>();P->OriginVoxel=FIntVector(Key.X*32,Key.Y*32,Key.Z*32);
        for(auto D:Source.descs){P->Desc.Add(D.OccWord);P->Desc.Add(D.MatWord);}for(auto W:Source.occ)P->Occ.Add(W);for(auto W:Source.mat)P->Mat.Add(W);
        P->BrickSolid=Source.brickSolid;P->bAnySolid=Source.anySolid;P->bAllSolid=Source.allSolid;return P;
    }
    void Attach(){
        TArray<FVoxelBrickIndexEntry> Initial;
        VoxelPreparedIndexTestSupport::SetSink(*Pool,[this](const FVoxelBrickIndexDelta& D){
            for(const auto& E:D.Removed)Visible.RemoveAll([&](const auto& V){return V.Key==E.Key;});
            for(const auto& E:D.Added)Visible.Add(E);
        },Initial);
        Visible=Initial;
    }
    void Capture(){
        Read=MakeShared<FRead,ESPMode::ThreadSafe>();const auto S=Read;const auto B=Pool->DebugGetBuffers();
        S->StateWords=B->AllocStateDwords;S->BitmapWords=B->AllocBitmapDwords;S->SideWords=B->AllocSideDwords;S->TableWords=B->ChunkSlots*16;
        ENQUEUE_RENDER_COMMAND(CaptureResetPoolState)([S,B](FRHICommandListImmediate& R){
            FVoxelBrickPool::EnsureCreated_RenderThread(R,B);
            FRDGBuilder G(R);const uint32 Words=S->StateWords+S->BitmapWords+S->SideWords+S->TableWords;
            auto Out=G.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(4,Words),TEXT("Voxel.ResetProof"));uint32 Offset=0;
            const auto Copy=[&](const TRefCountPtr<FRDGPooledBuffer>& Source,uint32 N){AddCopyBufferPass(G,Out,Offset*4,G.RegisterExternalBuffer(Source),0,N*4);Offset+=N;};
            Copy(B->AllocStatePooled,S->StateWords);Copy(B->AllocBitmapPooled,S->BitmapWords);Copy(B->AllocSidePooled,S->SideWords);Copy(B->ChunkTablePooled,S->TableWords);
            S->Buffer=MakeUnique<FRHIGPUBufferReadback>(TEXT("Voxel.ResetProof"));AddEnqueueCopyPass(G,S->Buffer.Get(),Out,Words*4);G.Execute();S->Issued.store(true);
        });
    }
    bool Poll(){
        if(Read->Done.load())return true;
        if(Read->Issued.load()){
            const auto S=Read;
            ENQUEUE_RENDER_COMMAND(PollResetPoolState)([S](FRHICommandListImmediate&){
                if(S->Done.load()||!S->Buffer->IsReady())return;
                const uint32 N=S->StateWords+S->BitmapWords+S->SideWords+S->TableWords;
                if(const void* Data=S->Buffer->Lock(N*4)){S->Data.SetNumUninitialized(N);FMemory::Memcpy(S->Data.GetData(),Data,N*4);S->Buffer->Unlock();}
                S->Buffer.Reset();S->Done.store(true);
            });
        }
        return false;
    }
public:
    explicit FCommand(FAutomationTestBase* T):Test(T){}
    ~FCommand(){
        // Test-only shutdown drain: no outstanding readback may die on GT.
        if(Read){const auto S=Read;ENQUEUE_RENDER_COMMAND(DrainResetPoolProof)([S](FRHICommandListImmediate& R){R.SubmitAndBlockUntilGPUIdle();S->Buffer.Reset();});}
        Pool.Reset();OldBuffers.Reset();FlushRenderingCommands();
    }
    bool Update()override{
        if(FPlatformTime::Seconds()-Started>40.){Test->AddError(TEXT("GPU reset regression timed out"));return true;}
        if(Phase==0){
            Pool=MakeUnique<FVoxelBrickPool>();FVoxelBrickPoolConfig C;C.ChunkCapacity=4;C.OccWordCapacity=4096;C.MatWordCapacity=16384;
            Pool->InitPrivateGpuReservationTestPool(C);Attach();Pool->AddChunkFromCpu(Pack(),Key,{});Pool->Flush();OldBuffers=Pool->DebugGetBuffers();Capture();Phase=1;return false;
        }
        if(!Poll())return false;
        if(!Test->TestTrue(TEXT("GPU readback mapped"),!Read->Data.IsEmpty()))return true;
        const auto& D=Read->Data;
        if(Phase==1){
            Test->TestTrue(TEXT("ordinary CPU producer claimed real GPU ranges"),D[11]>0&&D[12]>0);
            FirstOccBump=D[0];FirstMatBump=D[1];
            Test->TestEqual(TEXT("first generation has one installed index entry"),Visible.Num(),1);
            TArray<FVoxelBrickIndexEntry> Ignored;Pool->SetIndexSink(nullptr,Ignored);Visible.Reset();Pool->Reset();
            Test->TestTrue(TEXT("reset creates a distinct buffer holder"),Pool->DebugGetBuffers()!=OldBuffers);
            Test->TestEqual(TEXT("reset clears host residents"),Pool->GetNumResidentChunks(),0);
            Attach();Test->TestTrue(TEXT("reattached index receives no stale snapshot"),Visible.IsEmpty());Capture();Phase=2;return false;
        }
        if(Phase==2){
            bool Zero=true;for(uint32 W:D)Zero&=W==0;
            Test->TestTrue(TEXT("fresh GPU state bitmap side-table and records are all zero"),Zero);
            Key={-23,5,-7,0};Pool->AddChunkFromCpu(Pack(),Key,{});Pool->Flush();Capture();Phase=3;return false;
        }
        if(Phase==3){
            Test->TestEqual(TEXT("new arena starts with exactly one claim"),D[2],uint32(1));
            Test->TestEqual(TEXT("occupancy bump has no prior-world range charge"),D[0],FirstOccBump);
            Test->TestEqual(TEXT("material bump has no prior-world range charge"),D[1],FirstMatBump);
            Test->TestTrue(TEXT("new claim owns ranges without prior-world leakage"),D[11]>0&&D[12]>0&&D[7]==0&&D[10]==0);
            const uint32 Base=Read->StateWords+Read->BitmapWords+Read->SideWords;
            // Record origin is the first three signed dwords; slot is reused0.
            Test->TestEqual(TEXT("reused slot receives new X"),int32(D[Base]),Key.X*32);
            Test->TestEqual(TEXT("reused slot receives new Y"),int32(D[Base+1]),Key.Y*32);
            Test->TestEqual(TEXT("reused slot receives new Z"),int32(D[Base+2]),Key.Z*32);
            Test->TestTrue(TEXT("index contains only new-world key"),Visible.Num()==1&&Visible[0].Key==Key);
            Pool->RemoveChunk(Key);Pool->Flush();
            Test->TestTrue(TEXT("removal-only GPU flush empties the index"),Visible.IsEmpty());
            Test->TestTrue(TEXT("removal-only flush leaves ordinary inputs drained"),Pool->PrivateGpuInputsDrained());
            Capture();Phase=4;return false;
        }
        Test->TestEqual(TEXT("free returns all occupancy ranges"),D[11],uint32(0));Test->TestEqual(TEXT("free returns all material ranges"),D[12],uint32(0));
        bool BitmapZero=true;for(uint32 I=0;I<Read->BitmapWords;++I)BitmapZero&=D[Read->StateWords+I]==0;
        Test->TestTrue(TEXT("new-world retirement leaves no bitmap ownership"),BitmapZero);
        Test->TestEqual(TEXT("no double-grant"),D[7],uint32(0));Test->TestEqual(TEXT("no bad-free"),D[10],uint32(0));
        return true;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGpuPoolSequentialResetTest,"Voxel.Objects.GpuPoolSequentialReset",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelGpuPoolSequentialResetTest::RunTest(const FString&){
    if(GUsingNullRHI||!GIsRHIInitialized){AddWarning(TEXT("Real GPU required"));return true;}
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<GpuPoolResetTests::FCommand>(this));return true;
}
#endif
