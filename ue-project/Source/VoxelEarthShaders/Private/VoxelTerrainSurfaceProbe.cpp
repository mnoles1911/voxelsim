#include "VoxelTerrainSurfaceProbe.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
namespace {
class FVoxelTerrainSurfaceProbeCS:public FGlobalShader {
public:
    DECLARE_GLOBAL_SHADER(FVoxelTerrainSurfaceProbeCS);
    SHADER_USE_PARAMETER_STRUCT(FVoxelTerrainSurfaceProbeCS,FGlobalShader);
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM6);}
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
        SHADER_PARAMETER(uint32,QueryCount)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>,PageWords)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>,SourceWords)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>,SourceRanges)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>,Queries)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>,Results)
    END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FVoxelTerrainSurfaceProbeCS,"/VoxelEarth/VoxelTerrainSurfaceProbe.usf","TerrainSurfaceProbeMain",SF_Compute);
}
bool VoxelRunTerrainSurfaceProbe(const TArray<uint32>& Page,const TArray<uint32>& Sources,
    const TArray<FUintVector2>& Ranges,const TArray<FVoxelTerrainSurfaceProbeQuery>& Queries,
    TArray<FVoxelTerrainSurfaceProbeResult>& Output,FString& Error)
{
    check(IsInGameThread());Error.Reset();
    if(!GIsRHIInitialized||GMaxRHIFeatureLevel<ERHIFeatureLevel::SM6){Error=TEXT("SM6 RHI required");return false;}
    if(Page.IsEmpty()||Sources.IsEmpty()||Ranges.IsEmpty()||Queries.IsEmpty()||Page.Num()>16777216||Sources.Num()>16777216||Ranges.Num()>65536||Queries.Num()>65536){Error=TEXT("invalid or oversized terrain surface probe input");return false;}
    TArray<FVoxelTerrainSurfaceProbeResult> Result;
    ENQUEUE_RENDER_COMMAND(VoxelTerrainSurfaceProbe)([&](FRHICommandListImmediate& Cmd){
        FRDGBuilder Graph(Cmd);auto* P=Graph.AllocParameters<FVoxelTerrainSurfaceProbeCS::FParameters>();
        auto Words=[&](const TCHAR* Name,const TArray<uint32>& V){return CreateStructuredBuffer(Graph,Name,sizeof(uint32),V.Num(),V.GetData(),V.Num()*sizeof(uint32));};
        P->PageWords=Graph.CreateSRV(Words(TEXT("TerrainSurfaceProbe.Page"),Page));P->SourceWords=Graph.CreateSRV(Words(TEXT("TerrainSurfaceProbe.Source"),Sources));
        P->SourceRanges=Graph.CreateSRV(CreateStructuredBuffer(Graph,TEXT("TerrainSurfaceProbe.Ranges"),sizeof(FUintVector2),Ranges.Num(),Ranges.GetData(),Ranges.Num()*sizeof(FUintVector2)));
        P->Queries=Graph.CreateSRV(CreateStructuredBuffer(Graph,TEXT("TerrainSurfaceProbe.Queries"),16,Queries.Num()*5,Queries.GetData(),Queries.Num()*sizeof(FVoxelTerrainSurfaceProbeQuery)));
        auto Buffer=Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(16,uint32(Queries.Num())*4),TEXT("TerrainSurfaceProbe.Results"));P->Results=Graph.CreateUAV(Buffer);
        P->QueryCount=uint32(Queries.Num());
        TShaderMapRef<FVoxelTerrainSurfaceProbeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FComputeShaderUtils::AddPass(Graph,RDG_EVENT_NAME("Voxel.TerrainSurfaceProbe"),Shader,P,FIntVector(FMath::DivideAndRoundUp(Queries.Num(),64),1,1));
        const uint32 Bytes=uint32(Queries.Num())*sizeof(FVoxelTerrainSurfaceProbeResult);FRHIGPUBufferReadback Readback(TEXT("TerrainSurfaceProbe.Readback"));AddEnqueueCopyPass(Graph,&Readback,Buffer,Bytes);
        Graph.Execute();Cmd.SubmitAndBlockUntilGPUIdle();if(!Readback.IsReady()){Error=TEXT("terrain surface probe readback not ready");return;}
        const void* Data=Readback.Lock(Bytes);if(!Data){Error=TEXT("terrain surface probe readback lock failed");return;}
        Result.SetNumUninitialized(Queries.Num());FMemory::Memcpy(Result.GetData(),Data,Bytes);Readback.Unlock();
    });FlushRenderingCommands();
    if(!Error.IsEmpty())return false;Output=MoveTemp(Result);return true;
}
