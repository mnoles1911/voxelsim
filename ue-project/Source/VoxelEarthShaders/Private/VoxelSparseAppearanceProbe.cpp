#include "VoxelSparseAppearanceProbe.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
namespace {
class FVoxelSparseAppearanceProbeCS:public FGlobalShader {
public:
    DECLARE_GLOBAL_SHADER(FVoxelSparseAppearanceProbeCS);
    SHADER_USE_PARAMETER_STRUCT(FVoxelSparseAppearanceProbeCS,FGlobalShader);
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM6);}
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
        SHADER_PARAMETER(uint32,QueryCount)
        SHADER_PARAMETER(uint32,PageLength)
        SHADER_PARAMETER(uint32,ResourceCount)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>,PageWords)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>,SourceWords)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>,SourceRanges)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>,Queries)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>,Results)
    END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FVoxelSparseAppearanceProbeCS,"/VoxelEarth/VoxelSparseAppearanceProbe.usf","SparseAppearanceProbeMain",SF_Compute);
}
bool VoxelRunSparseAppearanceProbe(const TArray<uint32>& Page,const TArray<uint32>& Sources,
    const TArray<FUintVector2>& Ranges,const TArray<FVoxelSparseAppearanceProbeQuery>& Queries,
    TArray<FVoxelSparseAppearanceProbeResult>& Output,FString& Error)
{
    check(IsInGameThread());Error.Reset();
    if(!GIsRHIInitialized||GMaxRHIFeatureLevel<ERHIFeatureLevel::SM6){Error=TEXT("SM6 RHI required");return false;}
    if(Page.IsEmpty()||Sources.IsEmpty()||Ranges.IsEmpty()||Queries.IsEmpty()||Page.Num()>16777216||Sources.Num()>16777216||Ranges.Num()>65536||Queries.Num()>65536){Error=TEXT("invalid or oversized sparse probe input");return false;}
    TArray<FVoxelSparseAppearanceProbeResult> Result;
    ENQUEUE_RENDER_COMMAND(VoxelSparseAppearanceProbe)([&](FRHICommandListImmediate& Cmd){
        FRDGBuilder Graph(Cmd);auto* P=Graph.AllocParameters<FVoxelSparseAppearanceProbeCS::FParameters>();
        auto Words=[&](const TCHAR* Name,const TArray<uint32>& V){return CreateStructuredBuffer(Graph,Name,sizeof(uint32),V.Num(),V.GetData(),V.Num()*sizeof(uint32));};
        P->PageWords=Graph.CreateSRV(Words(TEXT("SparseProbe.Page"),Page));P->SourceWords=Graph.CreateSRV(Words(TEXT("SparseProbe.Source"),Sources));
        P->SourceRanges=Graph.CreateSRV(CreateStructuredBuffer(Graph,TEXT("SparseProbe.Ranges"),sizeof(FUintVector2),Ranges.Num(),Ranges.GetData(),Ranges.Num()*sizeof(FUintVector2)));
        P->Queries=Graph.CreateSRV(CreateStructuredBuffer(Graph,TEXT("SparseProbe.Queries"),sizeof(FVoxelSparseAppearanceProbeQuery),Queries.Num(),Queries.GetData(),Queries.Num()*sizeof(FVoxelSparseAppearanceProbeQuery)));
        auto Buffer=Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(16,uint32(Queries.Num())*3),TEXT("SparseProbe.Results"));P->Results=Graph.CreateUAV(Buffer);
        P->PageLength=uint32(Page.Num());P->QueryCount=uint32(Queries.Num());P->ResourceCount=uint32(Ranges.Num());
        TShaderMapRef<FVoxelSparseAppearanceProbeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FComputeShaderUtils::AddPass(Graph,RDG_EVENT_NAME("Voxel.SparseAppearanceProbe"),Shader,P,FIntVector(FMath::DivideAndRoundUp(Queries.Num(),64),1,1));
        const uint32 Bytes=uint32(Queries.Num())*sizeof(FVoxelSparseAppearanceProbeResult);FRHIGPUBufferReadback Readback(TEXT("SparseProbe.Readback"));AddEnqueueCopyPass(Graph,&Readback,Buffer,Bytes);
        Graph.Execute();Cmd.SubmitAndBlockUntilGPUIdle();if(!Readback.IsReady()){Error=TEXT("sparse probe readback not ready");return;}
        const void* Data=Readback.Lock(Bytes);if(!Data){Error=TEXT("sparse probe readback lock failed");return;}
        Result.SetNumUninitialized(Queries.Num());FMemory::Memcpy(Result.GetData(),Data,Bytes);Readback.Unlock();
    });FlushRenderingCommands();
    if(!Error.IsEmpty())return false;Output=MoveTemp(Result);return true;
}
