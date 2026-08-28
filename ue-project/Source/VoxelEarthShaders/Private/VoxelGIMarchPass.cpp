#include "VoxelGIMarchPass.h"

#include "VoxelBrickPool.h"       // the traversal source (public seam)
#include "VoxelGIVolume.h"        // RegisterVolumesForCompute -- the P7 prerequisite
#include "VoxelMarchChunkIndex.h" // and the GPU lookup that makes the pool walkable

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderCompilerCore.h"
#include "ShaderParameterStruct.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h" // DECLARE_GPU_STAT_NAMED
#include "RHIBreadcrumbs.h"                         // RHI_BREADCRUMB_EVENT_STAT (5.8 spelling)

// ---------------------------------------------------------------------------
// STREAMING-SIDE GPU STATS -- the split for the unattributed +5.47 ms.
//
// THE SPELLING MATTERS AND THREE OF THEM ARE DEAD. In 5.8 SCOPED_GPU_STAT,
// RDG_GPU_STAT_SCOPE and RDG_RHI_GPU_STAT_SCOPE are UE_DEPRECATED_MACRO and
// expand to NOTHING -- they compile, they look armed, and they measure zero.
// RHI_BREADCRUMB_EVENT_STAT (RHIBreadcrumbs.h:1302) and RDG_EVENT_SCOPE_STAT
// (RenderGraphEvent.h:480) are the live spellings. These sites use the first;
// see the note at each one for why the RDG form cannot be used here.
//
// WHAT THE COLUMN IS. Every DECLARE_GPU_STAT_NAMED stat has its per-frame
// EXCLUSIVE (Busy + Wait) milliseconds written to the CSV profiler once per
// frame, end-of-pipe, as GPU/<StatName> (GPUProfiler.cpp:1065-1067). The
// engine also emits GPU/Unaccounted -- queue time inside NO stat scope
// (GPUProfiler.cpp:800, accumulated at :1556 only when the stat stack is
// empty). So the GPU/ columns of one row SUM to the frame's queue busy time
// and the decomposition checks itself.
//
// KEEP THESE SIBLINGS, NEVER NESTED. Exclusive time is charged to the
// innermost stat only; nesting would silently move a term and make "which
// number am I reading" a live question. Each scope below wraps one standalone
// FRDGBuilder in one ENQUEUE_RENDER_COMMAND, so they are siblings by
// construction.
//
// ARMING: -csvGpuStats on the command line (r.GPUCsvStatsEnabled defaults 0 --
// with it off the GPU/ columns are simply ABSENT, no error), plus
// `CsvProfile FRAMES=N`. A CSV with no GPU/ column measured nothing.
// ---------------------------------------------------------------------------
DECLARE_GPU_STAT_NAMED(VoxelStreamGIMarch, TEXT("VoxelStreamGIMarch"));

DEFINE_LOG_CATEGORY_STATIC(LogVoxelGIMarch, Log, All);

// Macro, not a const TCHAR*: IMPLEMENT_GLOBAL_SHADER stringizes its path
// argument, exactly as VoxelShadowMarch.cpp does with VOXEL_SHADOW_USF.
#define VOXEL_GIMARCH_USF "/VoxelEarth/VoxelGIMarch.usf"

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

bool FVoxelGIMarchRequest::Validate(FString& OutWhy) const
{
	if (Bricks.Num() <= 0)
	{
		OutWhy = TEXT("no bricks");
		return false;
	}
	// D3D12 caps a dispatch dimension at 65535 groups. Z carries the brick
	// list, at kBrickEdgeCells/kTile groups per brick, so the cap is a real
	// bound rather than a theoretical one and it is checked HERE, where the
	// number is still nameable, instead of becoming a driver-level failure.
	const int32 GroupsPerBrick = VoxelGIMarch::kBrickEdgeCells / VoxelGIMarch::kTile;
	const int64 GroupsZ = int64(Bricks.Num()) * int64(GroupsPerBrick);
	if (GroupsZ > 65535)
	{
		OutWhy = FString::Printf(TEXT("%d bricks needs %lld dispatch groups in Z, over the 65535 cap"),
		                         Bricks.Num(), (long long)GroupsZ);
		return false;
	}
	if (VolumeDim <= 0)
	{
		OutWhy = TEXT("volume dim is 0 -- the GI volume has not been allocated");
		return false;
	}
	if (!(CellSizeUU > 0.0f) || !(ConeReachUU > 0.0f))
	{
		OutWhy = FString::Printf(TEXT("cellSizeUU=%.3f coneReachUU=%.3f -- both must be positive"),
		                         CellSizeUU, ConeReachUU);
		return false;
	}
	if (StepBudget <= 0)
	{
		OutWhy = FString::Printf(TEXT("stepBudget=%d"), StepBudget);
		return false;
	}
	if (Walk < 0 || Walk > 2)
	{
		OutWhy = FString::Printf(TEXT("walk=%d is not 0/1/2"), Walk);
		return false;
	}
	for (const FVoxelGIMarchBrick& B : Bricks)
	{
		if (B.CornerLocalUU.ContainsNaN())
		{
			OutWhy = TEXT("a brick corner is NaN");
			return false;
		}
	}
	// The row sums. See the header for why this one matters more than it looks:
	// a basis whose rows do not sum to 1 does not fail, it just makes the whole
	// world brighter or darker, which gets argued about as a tuning question.
	for (int32 S = 0; S < VoxelGIMarch::kNumSlots; ++S)
	{
		float Sum = 0.0f;
		for (int32 T = 0; T < VoxelGIMarch::kNumTraceDirs; ++T)
		{
			Sum += SlotWeight[S][T];
		}
		if (!FMath::IsNearlyEqual(Sum, 1.0f, 1e-3f))
		{
			OutWhy = FString::Printf(
				TEXT("slot %d weights sum to %.5f, not 1.0 -- an unoccluded cell would not solve to "
				     "exactly 1.0 and open terrain would differ between the GI arms"),
				S, Sum);
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Shader
// ---------------------------------------------------------------------------

BEGIN_SHADER_PARAMETER_STRUCT(FVoxelGIMarchParameters, )
	VOXEL_BRICK_POOL_PARAMETERS()
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchChunkIndex)
	SHADER_PARAMETER(FUintVector, MarchIndexDimChunks)
	SHADER_PARAMETER(FIntVector, MarchBrickOriginVoxel)
	SHADER_PARAMETER(int32, MarchStepBudget)

	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GIMarchBrickCornerLocalUU)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int4>, GIMarchBrickTexelMin)
	SHADER_PARAMETER(uint32, GIMarchNumBricks)
	SHADER_PARAMETER(uint32, GIMarchVolumeDim)

	SHADER_PARAMETER(float, GIMarchCellSizeUU)
	SHADER_PARAMETER(float, GIMarchConeReachUU)
	SHADER_PARAMETER(float, GIMarchStartOffsetUU)
	SHADER_PARAMETER(float, GIMarchConeSlopeUU)
	SHADER_PARAMETER(float, GIMarchSkyIntensity)
	SHADER_PARAMETER(uint32, GIMarchStatsEnabled)

	SHADER_PARAMETER_ARRAY(FVector4f, GIMarchTraceDir, [VoxelGIMarch::kNumTraceDirs])
	SHADER_PARAMETER_ARRAY(FVector4f, GIMarchSlotWeight,
	                       [VoxelGIMarch::kNumSlots * VoxelGIMarch::kNumTraceDirs])

	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, GIMarchOutPos)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, GIMarchOutNeg)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, GIMarchOutStats)
END_SHADER_PARAMETER_STRUCT()

// The walk is a PERMUTATION rather than a runtime branch, on the same rule the
// marcher states for its own source and ring arms: a runtime branch leaves both
// paths in the binary and silently re-bases whichever arm is being measured.
// tools\voxel-check-gimarch-shader.ps1 asserts the three permutations produce
// DISTINCT DXIL, because "it compiled" does not mean "the arm exists" -- a
// walk 2 that collapsed onto walk 0 would compare a walk against itself and
// report zero disagreement forever, which reads as "the hierarchy agrees".
class FVoxelGIMarchWalkDim : SHADER_PERMUTATION_INT("GIMARCH_WALK", 3);

class FVoxelGIMarchCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelGIMarchCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelGIMarchCS, FGlobalShader);
	using FParameters = FVoxelGIMarchParameters;
	using FPermutationDomain = TShaderPermutationDomain<FVoxelGIMarchWalkDim>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		// The brick pool is the ONLY source here -- see the #error in the .usf.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_SOURCE"), 1);
		OutEnvironment.SetDefine(TEXT("GIMARCH_TILE"), VoxelGIMarch::kTile);

		// VOXEL_MARCH_SKIP_LEVELS is what selects the two-level (brick mask +
		// chunk mask) hierarchy inside VoxelMarchTraverseBrickHier. It MUST
		// follow the walk: at 0 the "hier" arm would silently be a second flat
		// walk, and walk 2 would then compare a walk with itself. The checker
		// script compiles exactly these pairings for the same reason.
		const FPermutationDomain Domain(Parameters.PermutationId);
		const int32 Walk = Domain.Get<FVoxelGIMarchWalkDim>();
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_SKIP_LEVELS"), Walk == 0 ? 0 : 2);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVoxelGIMarchCS, VOXEL_GIMARCH_USF, "VoxelGIMarchMain", SF_Compute);

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

namespace
{
	// Render thread. Everything it needs is in the request; it derives nothing.
	void VoxelGIMarchDispatch_RenderThread(FRHICommandListImmediate& RHICmdList,
	                                       const FVoxelGIMarchRequest& Request)
	{
		// ON THE RHI COMMAND LIST, NOT THE GRAPH, AND THAT IS FORCED. An
		// RDG_EVENT_SCOPE_STAT here asserts at FRDGBuilder::Execute --
		// RenderGraphBuilder.cpp:1770 checks the graph's breadcrumb is back at
		// Sentinel -- because these builders Execute inside the scope rather than
		// after it. Measured: it crashed the first leg at VoxelRasterAtlasGpu.
		// RHI_BREADCRUMB_EVENT_STAT is the same stat on the RHI timeline, feeds
		// the same GPU/<name> CSV column, and outlives the graph by construction
		// (declared before it, destroyed after it).
		RHI_BREADCRUMB_EVENT_STAT(RHICmdList, VoxelStreamGIMarch, "VoxelStreamGIMarch");
		FRDGBuilder GraphBuilder(RHICmdList);

		// The volumes first, because this is the refusal most likely to fire and
		// it costs nothing to hit it before building any buffers.
		// RegisterVolumesForCompute logs its own reason, once, and returns false
		// rather than registering a sampled-only texture that would fail later
		// inside RDG at the CreateUAV below.
		FRDGTextureRef VolumePos = nullptr;
		FRDGTextureRef VolumeNeg = nullptr;
		if (!GVoxelGIVolume.RegisterVolumesForCompute(GraphBuilder, VolumePos, VolumeNeg))
		{
			return;
		}

		auto* Params = GraphBuilder.AllocParameters<FVoxelGIMarchParameters>();

		// FALSE MEANS THERE IS NOTHING TO MARCH, and it is ordinary on the first
		// frames of every run: the arenas are created lazily by the first flush.
		// Skipping is correct; dispatching against a half-filled struct would
		// march null SRVs, and a null SRV reads as zeros -- which is a legal
		// descriptor meaning uniform AIR. The whole world would be empty, every
		// cone would return sky, and GI would be uniformly bright with no error
		// anywhere.
		if (!GetGlobalVoxelBrickPool().BindShaderParameters(GraphBuilder, *Params))
		{
			return;
		}

		FRDGBufferRef IndexBuffer = GetGlobalVoxelMarchChunkIndex().Register(GraphBuilder);
		if (IndexBuffer == nullptr)
		{
			return;
		}
		Params->MarchChunkIndex = GraphBuilder.CreateSRV(IndexBuffer, PF_R32_UINT);
		Params->MarchIndexDimChunks = FUintVector(FVoxelMarchChunkIndex::kDimXY,
		                                          FVoxelMarchChunkIndex::kDimXY,
		                                          FVoxelMarchChunkIndex::kDimZ);
		Params->MarchBrickOriginVoxel = Request.FrameOriginVoxel;
		Params->MarchStepBudget = Request.StepBudget;

		// The work list. float4/int4 rather than float3/int3 because a
		// StructuredBuffer element must match the HLSL declaration exactly, and
		// HLSL pads a float3 in a structured buffer to 16 bytes anyway -- so the
		// padded form is the honest one and there is no stride to get wrong.
		const int32 NumBricks = Request.Bricks.Num();
		TArray<FVector4f> Corners;
		TArray<FIntVector4> Texels;
		Corners.Reserve(NumBricks);
		Texels.Reserve(NumBricks);
		for (const FVoxelGIMarchBrick& B : Request.Bricks)
		{
			Corners.Emplace(B.CornerLocalUU.X, B.CornerLocalUU.Y, B.CornerLocalUU.Z, 0.0f);
			Texels.Emplace(B.TexelMin.X, B.TexelMin.Y, B.TexelMin.Z, 0);
		}

		FRDGBufferRef CornerBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("VoxelGIMarch.BrickCorners"), sizeof(FVector4f),
			Corners.Num(), Corners.GetData(), Corners.Num() * sizeof(FVector4f));
		FRDGBufferRef TexelBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("VoxelGIMarch.BrickTexels"), sizeof(FIntVector4),
			Texels.Num(), Texels.GetData(), Texels.Num() * sizeof(FIntVector4));

		Params->GIMarchBrickCornerLocalUU = GraphBuilder.CreateSRV(CornerBuffer);
		Params->GIMarchBrickTexelMin = GraphBuilder.CreateSRV(TexelBuffer);
		Params->GIMarchNumBricks = uint32(NumBricks);
		Params->GIMarchVolumeDim = uint32(Request.VolumeDim);

		Params->GIMarchCellSizeUU = Request.CellSizeUU;
		Params->GIMarchConeReachUU = Request.ConeReachUU;
		Params->GIMarchStartOffsetUU = Request.StartOffsetUU;
		Params->GIMarchConeSlopeUU = Request.ConeSlopeUU;
		Params->GIMarchSkyIntensity = Request.SkyIntensity;
		Params->GIMarchStatsEnabled = Request.bStatsEnabled ? 1u : 0u;

		for (int32 T = 0; T < VoxelGIMarch::kNumTraceDirs; ++T)
		{
			Params->GIMarchTraceDir[T] = FVector4f(Request.TraceDir[T].X, Request.TraceDir[T].Y,
			                                       Request.TraceDir[T].Z, 0.0f);
		}
		// Flattened [slot][trace], one float per float4 slot. The .usf indexes
		// it as S * kNumTraceDirs + T and reads .x; that arithmetic is spelled
		// identically at both ends on purpose -- a packed layout would need two
		// index expressions that could drift.
		for (int32 S = 0; S < VoxelGIMarch::kNumSlots; ++S)
		{
			for (int32 T = 0; T < VoxelGIMarch::kNumTraceDirs; ++T)
			{
				Params->GIMarchSlotWeight[S * VoxelGIMarch::kNumTraceDirs + T] =
					FVector4f(Request.SlotWeight[S][T], 0.0f, 0.0f, 0.0f);
			}
		}

		Params->GIMarchOutPos = GraphBuilder.CreateUAV(VolumePos);
		Params->GIMarchOutNeg = GraphBuilder.CreateUAV(VolumeNeg);

		// Bound every dispatch because the parameter map names it
		// unconditionally. Cleared so a stats-enabled run starts from zero;
		// never read back -- there is no readback path here by design.
		FRDGBufferRef Stats = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), uint32(VoxelGIMarch::kStatsWords)),
			TEXT("VoxelGIMarch.Stats"));
		FRDGBufferUAVRef StatsUAV = GraphBuilder.CreateUAV(Stats, PF_R32_UINT);
		AddClearUAVPass(GraphBuilder, StatsUAV, 0u);
		Params->GIMarchOutStats = StatsUAV;

		FVoxelGIMarchCS::FPermutationDomain Domain;
		Domain.Set<FVoxelGIMarchWalkDim>(Request.Walk);
		TShaderMapRef<FVoxelGIMarchCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel), Domain);

		// X and Y cover one brick's 8 cells at kTile each; Z carries the brick
		// list, kBrickEdgeCells/kTile groups per brick. 8 divides by 4 exactly,
		// so there is no partial group and no bounds slack in X/Y -- the
		// shader's own X/Y guard is a belt-and-braces check, not load-bearing.
		const int32 GroupsPerAxis = VoxelGIMarch::kBrickEdgeCells / VoxelGIMarch::kTile;
		const FIntVector Groups(GroupsPerAxis, GroupsPerAxis, GroupsPerAxis * NumBricks);

		FComputeShaderUtils::AddPass(GraphBuilder,
		                             RDG_EVENT_NAME("VoxelGIMarch(%d bricks, walk=%d)",
		                                            NumBricks, Request.Walk),
		                             ERDGPassFlags::Compute, Shader, Params, Groups);

		GraphBuilder.Execute();
	}
}

int32 VoxelGIMarch::Enqueue_GameThread(FVoxelGIMarchRequest&& Request)
{
	check(IsInGameThread());

	FString Why;
	if (!Request.Validate(Why))
	{
		// Once per distinct reason. A refusal that prints every frame is a
		// refusal nobody reads, and one that prints never is a refusal nobody
		// knows about.
		static TSet<FString> Reported;
		if (!Reported.Contains(Why))
		{
			Reported.Add(Why);
			UE_LOG(LogVoxelGIMarch, Warning,
			       TEXT("VoxelGIMarch: REFUSED a dispatch -- %s. Nothing was queued; the GI volume "
			            "keeps whatever it already held. This is the game-thread half of the arm, so "
			            "the arm must NOT be reported as having run."),
			       *Why);
		}
		return 0;
	}

	const int32 NumBricks = Request.Bricks.Num();
	// The request owns everything it names and crosses by value into the render
	// command -- nothing here points back at light-field memory the game thread
	// will mutate on the next tick.
	ENQUEUE_RENDER_COMMAND(VoxelGIMarchDispatch)(
		[Payload = MoveTemp(Request)](FRHICommandListImmediate& RHICmdList)
		{
			VoxelGIMarchDispatch_RenderThread(RHICmdList, Payload);
		});
	return NumBricks;
}
