#include "VoxelApprovedAppearanceProbe.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
namespace {
class FVoxelApprovedAppearanceProbeCS:public FGlobalShader {
public:
    DECLARE_GLOBAL_SHADER(FVoxelApprovedAppearanceProbeCS);
    SHADER_USE_PARAMETER_STRUCT(FVoxelApprovedAppearanceProbeCS,FGlobalShader);
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM6);}
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
        SHADER_PARAMETER(uint32,QueryCount)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>,Queries)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>,Results)
    END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FVoxelApprovedAppearanceProbeCS,"/VoxelEarth/VoxelApprovedAppearanceProbe.usf","ApprovedAppearanceProbeMain",SF_Compute);
}
bool VoxelRunApprovedAppearanceProbe(const TArray<FVoxelApprovedAppearanceProbeQuery>& Queries,
    TArray<FVoxelApprovedAppearanceProbeResult>& Output,FString& Error)
{
    check(IsInGameThread());Error.Reset();
    if(!GIsRHIInitialized||GMaxRHIFeatureLevel<ERHIFeatureLevel::SM6){Error=TEXT("SM6 RHI required");return false;}
    if(Queries.IsEmpty()||Queries.Num()>65536){Error=TEXT("invalid approved appearance probe count");return false;}
    TArray<FVoxelApprovedAppearanceProbeResult> Result;
    ENQUEUE_RENDER_COMMAND(VoxelApprovedAppearanceProbe)([&](FRHICommandListImmediate& Cmd){
        FRDGBuilder Graph(Cmd);auto* P=Graph.AllocParameters<FVoxelApprovedAppearanceProbeCS::FParameters>();
        P->Queries=Graph.CreateSRV(CreateStructuredBuffer(Graph,TEXT("ApprovedProbe.Queries"),16,Queries.Num()*4,Queries.GetData(),Queries.Num()*sizeof(FVoxelApprovedAppearanceProbeQuery)));
        auto Buffer=Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(16,uint32(Queries.Num())*2),TEXT("ApprovedProbe.Results"));P->Results=Graph.CreateUAV(Buffer);P->QueryCount=uint32(Queries.Num());
        TShaderMapRef<FVoxelApprovedAppearanceProbeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FComputeShaderUtils::AddPass(Graph,RDG_EVENT_NAME("Voxel.ApprovedAppearanceProbe"),Shader,P,FIntVector(FMath::DivideAndRoundUp(Queries.Num(),64),1,1));
        const uint32 Bytes=uint32(Queries.Num())*sizeof(FVoxelApprovedAppearanceProbeResult);FRHIGPUBufferReadback Readback(TEXT("ApprovedProbe.Readback"));AddEnqueueCopyPass(Graph,&Readback,Buffer,Bytes);
        Graph.Execute();Cmd.SubmitAndBlockUntilGPUIdle();if(!Readback.IsReady()){Error=TEXT("approved probe readback not ready");return;}
        const void* Data=Readback.Lock(Bytes);if(!Data){Error=TEXT("approved probe readback lock failed");return;}
        Result.SetNumUninitialized(Queries.Num());FMemory::Memcpy(Result.GetData(),Data,Bytes);Readback.Unlock();
    });FlushRenderingCommands();
    if(!Error.IsEmpty())return false;Output=MoveTemp(Result);return true;
}
