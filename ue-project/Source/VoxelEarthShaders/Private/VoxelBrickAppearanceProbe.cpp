#include "VoxelBrickAppearanceProbe.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
namespace {
class FVoxelBrickAppearanceProbeCS:public FGlobalShader {
public:
    DECLARE_GLOBAL_SHADER(FVoxelBrickAppearanceProbeCS);
    SHADER_USE_PARAMETER_STRUCT(FVoxelBrickAppearanceProbeCS,FGlobalShader);
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM6);}
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
        VOXEL_BRICK_POOL_PARAMETERS()
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>,MarchChunkIndex)
        SHADER_PARAMETER(FUintVector3,MarchIndexDimChunks)
        SHADER_PARAMETER(FIntVector,MarchBrickOriginVoxel)
        SHADER_PARAMETER(uint32,MarchIndexCellsPerLevel)
        SHADER_PARAMETER(int32,MarchStepBudget)
        SHADER_PARAMETER(uint32,QueryCount)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>,ProbeRays)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>,ProbeHits)
    END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FVoxelBrickAppearanceProbeCS,"/VoxelEarth/VoxelBrickAppearanceProbe.usf","BrickAppearanceProbeMain",SF_Compute);
}
bool VoxelRunBrickAppearanceProbe(FVoxelBrickPool& Pool,const TArray<FVoxelBrickChunkKey>& Keys,
    const TArray<FVoxelBrickAppearanceRay>& Rays,TArray<FVoxelBrickAppearanceHit>& Flat,TArray<FVoxelBrickAppearanceHit>& Hier,FString& Error){
    check(IsInGameThread());Error.Reset();Flat.Reset();Hier.Reset();
    if(!GIsRHIInitialized||GMaxRHIFeatureLevel<ERHIFeatureLevel::SM6||Keys.IsEmpty()||Keys.Num()>16||Rays.IsEmpty()||Rays.Num()>16384){Error=TEXT("invalid bounded brick probe input/RHI");return false;}
    TArray<uint32> Index;Index.SetNumZeroed(64*8);
    for(const auto& K:Keys){if(K.Level<0||K.Level>7){Error=TEXT("invalid probe level");return false;}const int32 Slot=Pool.FindChunkSlot(K);const uint32 At=uint32(K.Level)*64u+(uint32(K.X)&3u)+4u*((uint32(K.Y)&3u)+4u*(uint32(K.Z)&3u));if(Slot<0||Index[At]){Error=TEXT("missing resident or aliased probe key");return false;}Index[At]=0xc0000000u|uint32(Slot);}
    for(const auto& R:Rays)if(R.Level>7||R.Origin.ContainsNaN()||R.Origin.GetAbsMax()>4096.f||R.Direction.ContainsNaN()||!FMath::IsFinite(R.Reach)||R.Reach<=0||R.Reach>640||!FMath::IsNearlyEqual(R.Direction.SizeSquared(),1.f,.0001f)){Error=TEXT("invalid ray");return false;}
    Pool.Flush();FlushRenderingCommands();
    ENQUEUE_RENDER_COMMAND(VoxelBrickAppearanceProbe)([&](FRHICommandListImmediate& Cmd){
        FRDGBuilder Graph(Cmd);auto* P=Graph.AllocParameters<FVoxelBrickAppearanceProbeCS::FParameters>();
        if(!Pool.BindShaderParameters(Graph,*P)){Error=TEXT("actual pool binding failed");return;}
        auto IB=CreateVertexBuffer(Graph,TEXT("BrickAppearance.Index"),FRDGBufferDesc::CreateBufferDesc(4,Index.Num()),Index.GetData(),sizeof(uint32)*Index.Num());P->MarchChunkIndex=Graph.CreateSRV(IB,PF_R32_UINT);
        P->MarchIndexDimChunks=FUintVector3(4,4,4);P->MarchBrickOriginVoxel=FIntVector::ZeroValue;P->MarchIndexCellsPerLevel=64;P->MarchStepBudget=256;P->QueryCount=Rays.Num();
        P->ProbeRays=Graph.CreateSRV(CreateStructuredBuffer(Graph,TEXT("BrickAppearance.Rays"),16,Rays.Num()*2,Rays.GetData(),Rays.Num()*sizeof(FVoxelBrickAppearanceRay)));
        auto Output=Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(16,Rays.Num()*4),TEXT("BrickAppearance.Hits"));P->ProbeHits=Graph.CreateUAV(Output);
        TShaderMapRef<FVoxelBrickAppearanceProbeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));FComputeShaderUtils::AddPass(Graph,RDG_EVENT_NAME("Voxel.BrickAppearanceProbe"),Shader,P,FIntVector(FMath::DivideAndRoundUp(Rays.Num(),64),1,1));
        FRHIGPUBufferReadback Back(TEXT("BrickAppearance.Readback"));const uint32 Bytes=Rays.Num()*64;AddEnqueueCopyPass(Graph,&Back,Output,Bytes);Graph.Execute();Cmd.SubmitAndBlockUntilGPUIdle();
        if(!Back.IsReady()){Error=TEXT("brick probe readback not ready");return;}auto Data=static_cast<const FVoxelBrickAppearanceHit*>(Back.Lock(Bytes));if(!Data){Error=TEXT("brick probe readback lock failed");return;}
        Flat.SetNumUninitialized(Rays.Num());Hier.SetNumUninitialized(Rays.Num());for(int N=0;N<Rays.Num();++N){Flat[N]=Data[N*2];Hier[N]=Data[N*2+1];}Back.Unlock();
    });FlushRenderingCommands();return Error.IsEmpty();
}
