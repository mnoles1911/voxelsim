// GPU execution is explicit automation only; no startup or world ownership hook.
#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelGpuWorldGen.h"
#include "VoxelGpuWorldGenGraph.h"
#include "VoxelGpuWorklist.h"
#include "Misc/AutomationTest.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
namespace {
uint32 OwnershipCell(int X,int Y,int Z){return uint32(((X/8)+(Y/8)*4)*4+Z/8)*512+uint32((X%8)+8*((Y%8)+8*(Z%8)));}
FVoxelGpuRegionRequest OwnershipRequest(int Level,int Yaw,int Mode){
    FVoxelGpuRegionRequest R;R.DispatchColumns=FUintVector2(32,32);R.BricksZ=4;R.BrickZMin=0;R.CoarseLevel=Level;R.OriginVx=-32;R.OriginVy=-64;
    const int S=1<<Level,SX=2*S,SY=3*S,SZ=5*S,OX=-3*S,OY=-7*S;
    const int RX=Yaw==0?OX:Yaw==1?-(OY+SY-1):Yaw==2?-(OX+SX-1):OY;
    const int RY=Yaw==0?OY:Yaw==1?OX:Yaw==2?-(OY+SY-1):-(OX+SX-1);
    for(int A=0;A<3;++A){auto& I=R.AssetInstances.AddDefaulted_GetRef();I.SizeX=SX;I.SizeY=SY;I.SizeZ=SZ;I.YawQuarter=Yaw;
        I.RotOriginX=RX;I.RotOriginY=RY;I.GridOriginZ=-2*S;I.AnchorRelVx=S/2-RX;I.AnchorRelVy=S/2-RY;I.AnchorVz=2*S+S/2;
        I.RenderOwned=Mode==3?1u:Mode==1&&A==0?1u:Mode==2&&A==1?1u:0u;I.ColStartsBase=R.AssetColStarts.Num();
        for(int C=0;C<SX*SY;++C){R.AssetColStarts.Add(R.AssetSpans.Num());
            if(A==0){R.AssetSpans.Add((uint32(2*S)<<8)|uint32(16+(C%2)));R.AssetSpans.Add((uint32(3*S)<<20)|(uint32(2*S)<<8)|uint32(16+(C%2)));}
            else R.AssetSpans.Add((uint32(SZ)<<8)|uint32(17+A));
        }R.AssetColStarts.Add(R.AssetSpans.Num());
    }return R;
}
uint32 OwnershipSample(const FVoxelGpuRegionRequest& R,const FVoxelGpuRegionRequest::FAssetInstance& I,int X,int Y,int Z){
    const int Scale=1<<R.CoarseLevel,Half=Scale/2;
    const int RX=X*Scale+Half-I.AnchorRelVx-I.RotOriginX,RY=Y*Scale+Half-I.AnchorRelVy-I.RotOriginY;
    const int SX=(I.YawQuarter&1)?I.SizeY:I.SizeX,SY=(I.YawQuarter&1)?I.SizeX:I.SizeY;
    if(RX<0||RY<0||RX>=SX||RY>=SY)return 0;
    int BX=RX,BY=RY;
    if(I.YawQuarter==1){BX=RY;BY=int(I.SizeY)-1-RX;}else if(I.YawQuarter==2){BX=int(I.SizeX)-1-RX;BY=int(I.SizeY)-1-RY;}else if(I.YawQuarter==3){BX=int(I.SizeX)-1-RY;BY=RX;}
    const int BZ=Z*Scale+Half-I.AnchorVz-I.GridOriginZ;if(BZ<0||BZ>=int(I.SizeZ))return 0;
    const uint32 C=I.ColStartsBase+BX*I.SizeY+BY;
    for(uint32 J=R.AssetColStarts[C];J<R.AssetColStarts[C+1];++J){const uint32 P=R.AssetSpans[J];if(BZ>=int(P>>20)&&BZ<int((P>>20)+((P>>8)&4095)))return P&255;}return 0;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGpuOwnedWinnerTest,"Voxel.GPU.AssetOwnedWinner",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelGpuOwnedWinnerTest::RunTest(const FString&){
    if(!VoxelGpuWorldGen::IsSupportedOnCurrentRHI()){AddError(TEXT("Requires SM6 GPU"));return false;}
    for(int Level:{0,1,3,7})for(int Yaw=0;Yaw<4;++Yaw)for(int Mode=0;Mode<4;++Mode)for(int Channel=0;Channel<3;++Channel){
        auto Request=OwnershipRequest(Level,Yaw,Mode);
        // Exercise each public API independently and both together against the
        // same CPU first-winner oracle, including the mirrored 52-byte GPU ABI.
        for(auto& I:Request.AssetInstances){
            if(Channel!=0)I.SuppressTerrainRender=I.RenderOwned;
            if(Channel==1)I.RenderOwned=0;
        }
        TArray<uint32> Initial,Expected,Classic,Worklist;Initial.Init(0x5a123400u,32768);
        if(Mode!=3){Initial[OwnershipCell(0,0,0)]|=4;Initial[OwnershipCell(1,0,1)]|=5;}
        Expected=Initial;int OwnedWinners=0,LaterWinners=0;
        for(int X=0;X<32;++X)for(int Y=0;Y<32;++Y)for(int Z=0;Z<32;++Z){auto& C=Expected[OwnershipCell(X,Y,Z)];if(C&255)continue;
            for(int A=0;A<Request.AssetInstances.Num();++A){const auto& I=Request.AssetInstances[A];const uint32 M=OwnershipSample(Request,I,X,Y,Z);if(M){const bool Suppressed=I.RenderOwned!=0 || I.SuppressTerrainRender!=0;C|=Suppressed?0:M;OwnedWinners+=Suppressed;LaterWinners+=A>0;break;}}
        }
        if(Mode)TestTrue(TEXT("fixture actually exercises owned winners"),OwnedWinners>0);TestTrue(TEXT("fixture includes earlier-air later winner"),LaterWinners>0);
        FString Failure;
        ENQUEUE_RENDER_COMMAND(VoxelOwnedWinnerParity)([&](FRHICommandListImmediate& Cmd){
            FRDGBuilder Graph(Cmd);const uint32 Bytes=32768*sizeof(uint32);
            auto C=CreateStructuredBuffer(Graph,TEXT("OwnedTest.Classic"),sizeof(uint32),32768,Initial.GetData(),Bytes);
            auto W=CreateStructuredBuffer(Graph,TEXT("OwnedTest.Worklist"),sizeof(uint32),32768,Initial.GetData(),Bytes);
            VoxelGpuWorldGen::AddClassicAssetStampPasses(Graph,Request,C);
            FVoxelGpuChunkWorkRecord Record;Record.LevelFlags=uint32(Level)|(1u<<8)|(1u<<9);Record.AssetCount=Request.AssetInstances.Num();
            TArray<FVoxelWorklistAssetInstance> Instances;
            for(const auto& I:Request.AssetInstances){auto& V=Instances.AddDefaulted_GetRef();V.AnchorRelVx=I.AnchorRelVx;V.AnchorRelVy=I.AnchorRelVy;V.AnchorVz=I.AnchorVz;V.GridOriginZ=I.GridOriginZ;V.RotOriginX=I.RotOriginX;V.RotOriginY=I.RotOriginY;V.YawQuarter=I.YawQuarter;V.SizeX=I.SizeX;V.SizeY=I.SizeY;V.SizeZ=I.SizeZ;V.ColStartsBase=I.ColStartsBase;V.RenderOwned=I.RenderOwned;V.SuppressTerrainRender=I.SuppressTerrainRender;}
            uint32 Control[]={0,1};uint32 Args[]={16,1,1};
            VoxelGpuWorldGen::FWorklistAssetStampDispatch D;
            D.Records=CreateStructuredBuffer(Graph,TEXT("OwnedTest.Records"),sizeof(Record),1,&Record,sizeof(Record));
            D.Control=CreateStructuredBuffer(Graph,TEXT("OwnedTest.Control"),sizeof(uint32),2,Control,sizeof(Control));
            D.IndirectArgs=Graph.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc(3),TEXT("OwnedTest.Args"));Graph.QueueBufferUpload(D.IndirectArgs,Args,sizeof(Args),ERDGInitialDataFlags::None);
            D.RingCapacity=1;D.CellArena=W;D.bHasOwnedWinners=Mode!=0;
            D.Instances=CreateStructuredBuffer(Graph,TEXT("OwnedTest.Instances"),sizeof(FVoxelWorklistAssetInstance),Instances.Num(),Instances.GetData(),Instances.Num()*sizeof(FVoxelWorklistAssetInstance));
            D.ColStarts=CreateStructuredBuffer(Graph,TEXT("OwnedTest.Starts"),sizeof(uint32),Request.AssetColStarts.Num(),Request.AssetColStarts.GetData(),Request.AssetColStarts.Num()*sizeof(uint32));
            D.Spans=CreateStructuredBuffer(Graph,TEXT("OwnedTest.Spans"),sizeof(uint32),Request.AssetSpans.Num(),Request.AssetSpans.GetData(),Request.AssetSpans.Num()*sizeof(uint32));
            VoxelGpuWorldGen::AddWorklistAssetStampPass(Graph,D);
            FRHIGPUBufferReadback CR(TEXT("OwnedTest.ClassicReadback")),WR(TEXT("OwnedTest.WorklistReadback"));AddEnqueueCopyPass(Graph,&CR,C,Bytes);AddEnqueueCopyPass(Graph,&WR,W,Bytes);
            Graph.Execute();Cmd.SubmitAndBlockUntilGPUIdle();
            if(!CR.IsReady()||!WR.IsReady()){Failure=TEXT("GPU parity readbacks not ready");return;}
            const void* CP=CR.Lock(Bytes);const void* WP=WR.Lock(Bytes);
            if(CP&&WP){Classic.SetNumUninitialized(32768);Worklist.SetNumUninitialized(32768);FMemory::Memcpy(Classic.GetData(),CP,Bytes);FMemory::Memcpy(Worklist.GetData(),WP,Bytes);}else Failure=TEXT("GPU readback lock failed");
            if(CP)CR.Unlock();if(WP)WR.Unlock();
        });FlushRenderingCommands();
        const FString Case=FString::Printf(TEXT("L%d yaw%d mode%d"),Level,Yaw,Mode);
        if(!Failure.IsEmpty()){AddError(Case+TEXT(" ")+Failure);return false;}
        TestTrue(Case+TEXT(" classic exact CPU winner parity and upper bits"),Classic==Expected);TestTrue(Case+TEXT(" worklist exact CPU winner parity and upper bits"),Worklist==Expected);
        if(Mode==3)TestTrue(Case+TEXT(" all-owned output is all air"),Expected==Initial);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGpuOwnedWrappedClearTest,"Voxel.GPU.AssetOwnedWrappedClear",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelGpuOwnedWrappedClearTest::RunTest(const FString&)
{
    if(!VoxelGpuWorldGen::IsSupportedOnCurrentRHI()){AddError(TEXT("Requires SM6 GPU"));return false;}
    // 73,728 groups exceed a one-dimensional D3D dispatch. The final voxel
    // must be stamped in the control and cleared only in the owned run.
    constexpr uint32 Count=256u*256u*72u, Upper=0x5a123400u;
    FVoxelGpuRegionRequest Request;Request.DispatchColumns=FUintVector2(256,256);Request.BricksZ=9;
    for(int A=0;A<3;++A){
        auto& I=Request.AssetInstances.AddDefaulted_GetRef();I.SizeX=1;I.SizeY=1;I.SizeZ=1;
        I.AnchorRelVx=A==2?0:255;I.AnchorRelVy=A==2?0:255;I.AnchorVz=A==2?0:71;
        I.ColStartsBase=Request.AssetColStarts.Num();Request.AssetColStarts.Add(Request.AssetSpans.Num());
        Request.AssetSpans.Add((1u<<8)|uint32(16+A));Request.AssetColStarts.Add(Request.AssetSpans.Num());
    }
    TArray<uint32> Initial;Initial.Init(Upper,Count);
    for(bool Owned:{false,true}){
        Request.AssetInstances[0].RenderOwned=Owned?1u:0u;
        TArray<uint32> Actual;FString Failure;
        ENQUEUE_RENDER_COMMAND(VoxelOwnedWrappedClear)([&](FRHICommandListImmediate& Cmd){
            FRDGBuilder Graph(Cmd);const uint32 Bytes=Count*sizeof(uint32);
            auto Cells=CreateStructuredBuffer(Graph,TEXT("WrappedClear.Cells"),sizeof(uint32),Count,Initial.GetData(),Bytes);
            VoxelGpuWorldGen::AddClassicAssetStampPasses(Graph,Request,Cells);
            FRHIGPUBufferReadback Readback(TEXT("WrappedClear.Readback"));AddEnqueueCopyPass(Graph,&Readback,Cells,Bytes);
            Graph.Execute();Cmd.SubmitAndBlockUntilGPUIdle();
            if(!Readback.IsReady()){Failure=TEXT("Readback not ready");return;}
            const void* Data=Readback.Lock(Bytes);
            if(Data){Actual.SetNumUninitialized(Count);FMemory::Memcpy(Actual.GetData(),Data,Bytes);Readback.Unlock();}
            else Failure=TEXT("Readback lock failed");
        });FlushRenderingCommands();
        if(!Failure.IsEmpty()){AddError(Failure);return false;}
        if(!TestEqual(TEXT("Complete wrapped region read back"),Actual.Num(),int32(Count)))return false;
        for(uint32 I=0;I<Count;++I){
            const uint32 Expected=Upper|(I==0?18u:I==Count-1&&!Owned?16u:0u);
            if(Actual[I]!=Expected){AddError(FString::Printf(TEXT("Wrapped clear owned=%d cell=%u expected=%u actual=%u"),Owned,I,Expected,Actual[I]));return false;}
        }
    }
    return true;
}
#endif
