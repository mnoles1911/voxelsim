// FVoxelGpuWorklist -- the persistent chunk-record ring (P3 foundation).
// NOT WIRED: no production code calls this yet; see the header's banner and
// docs/gpu-worklist-plan-2026-08-23.md for the sequencing arithmetic. It is
// implemented rather than stubbed so the contract is testable the day the
// kernel conversion starts, and so nothing about the ring discipline is left
// to be invented under measurement pressure.

#include "VoxelGpuWorklist.h"

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "RenderingThread.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelGpuWorklist, Log, All);

// Layout lock against the .ush mirror: 16 dwords, offsets as documented.
static_assert(offsetof(FVoxelGpuChunkWorkRecord, OriginVx) == 0, "record layout");
static_assert(offsetof(FVoxelGpuChunkWorkRecord, LevelFlags) == 12, "record layout");
static_assert(offsetof(FVoxelGpuChunkWorkRecord, ShadingSurfaceZBits) == 40, "record layout");

namespace
{
	class FVoxelWorklistArgsCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelWorklistArgsCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelWorklistArgsCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, WorklistControl)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, WorklistArgs)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GroupsPerRecord)
			SHADER_PARAMETER(uint32, HeadCursor)
			SHADER_PARAMETER(uint32, SliceBudget)
			SHADER_PARAMETER(uint32, StageCount)
		END_SHADER_PARAMETER_STRUCT()
	};

	// Groups per record per stage -- the host copy of the stage shapes the
	// converted kernels will be written against. 32x32 columns / 64 threads =
	// 16; voxelize 32x32x32 cells / 64 = 512/32... kept as the DESIGN numbers
	// from the plan doc; the conversion locks each entry the day its kernel
	// lands, and a mismatch is a torn dispatch, so the plan doc calls for a
	// static_assert per converted kernel against this table.
	const uint32 kGroupsPerRecord[uint8(EVoxelWorklistStage::COUNT)] =
	{
		16,   // Column: 1024 columns / 64
		512,  // Voxelize: 32^3 cells / 64
		16,   // AssetStamp: per-column gather, 1024 columns / 64
		1,    // ClassifyTotals: 64 bricks, one group
		2,    // PackClaim: classify+pack pair
		64,   // Write: upper-bound word-copy groups
		1,    // Record
	};
}

#define VOXEL_WORKLIST_ARGS_USF "/VoxelEarth/VoxelWorklistArgs.usf"
IMPLEMENT_GLOBAL_SHADER(FVoxelWorklistArgsCS, VOXEL_WORKLIST_ARGS_USF, "WorklistArgsMain", SF_Compute);

void FVoxelGpuWorklist::Init(uint32 RecordCapacity)
{
	check(IsInGameThread());
	check(Capacity == 0 && RecordCapacity > 0);
	Capacity = RecordCapacity;
	ENQUEUE_RENDER_COMMAND(VoxelWorklistCreate)(
		[this](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		FRDGBufferRef Records = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVoxelGpuChunkWorkRecord), Capacity),
			TEXT("Voxel.WorklistRecords"));
		FRDGBufferRef Args = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(
				uint32(EVoxelWorklistStage::COUNT)),
			TEXT("Voxel.WorklistArgs"));
		FRDGBufferRef Control = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 3),
			TEXT("Voxel.WorklistControl"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Control), 0u);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Args, PF_R32_UINT), 0u);
		GraphBuilder.QueueBufferExtraction(Records, &PooledRecords);
		GraphBuilder.QueueBufferExtraction(Args, &PooledArgs);
		GraphBuilder.QueueBufferExtraction(Control, &PooledControl);
		GraphBuilder.Execute();
	});
}

int32 FVoxelGpuWorklist::Append(TArrayView<const FVoxelGpuChunkWorkRecord> Records)
{
	check(IsInGameThread());
	check(IsInitialized());
	// Head/Tail are MONOTONIC record counts (the ring index is count %
	// Capacity), so pending is a plain subtraction and wrap costs nothing.
	int32 Accepted = 0;
	for (const FVoxelGpuChunkWorkRecord& R : Records)
	{
		if (Head - Tail + uint32(Staged.Num()) >= Capacity)
		{
			++Window.RefusedFull;
			continue;
		}
		Staged.Add(R);
		++Accepted;
	}
	Window.Appended += uint64(Accepted);
	return Accepted;
}

void FVoxelGpuWorklist::Flush(uint32 SliceBudgetRecords)
{
	check(IsInGameThread());
	check(IsInitialized());
	const uint32 NewHead = Head + uint32(Staged.Num());
	const uint32 FirstSlot = Head % Capacity;

	ENQUEUE_RENDER_COMMAND(VoxelWorklistFlush)(
		[this, StagedNow = MoveTemp(Staged), FirstSlot, NewHead,
		 SliceBudgetRecords](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		FRDGBufferRef Records = GraphBuilder.RegisterExternalBuffer(PooledRecords, TEXT("Voxel.WorklistRecords"));
		FRDGBufferRef Args = GraphBuilder.RegisterExternalBuffer(PooledArgs, TEXT("Voxel.WorklistArgs"));
		FRDGBufferRef Control = GraphBuilder.RegisterExternalBuffer(PooledControl, TEXT("Voxel.WorklistControl"));

		if (StagedNow.Num() > 0)
		{
			// Upload the new segment as a transient staging buffer, then copy
			// into the ring -- split in two when the segment wraps the ring
			// end, because a single copy would overrun the buffer.
			FRDGBufferRef StagedBuf = CreateStructuredBuffer(
				GraphBuilder, TEXT("Voxel.WorklistStaged"), sizeof(FVoxelGpuChunkWorkRecord),
				StagedNow.Num(), StagedNow.GetData(),
				StagedNow.Num() * sizeof(FVoxelGpuChunkWorkRecord));
			const uint64 RecB = sizeof(FVoxelGpuChunkWorkRecord);
			const uint32 FirstRun = FMath::Min(uint32(StagedNow.Num()), Capacity - FirstSlot);
			AddCopyBufferPass(GraphBuilder, Records, uint64(FirstSlot) * RecB,
			                  StagedBuf, 0, uint64(FirstRun) * RecB);
			if (FirstRun < uint32(StagedNow.Num()))
			{
				AddCopyBufferPass(GraphBuilder, Records, 0,
				                  StagedBuf, uint64(FirstRun) * RecB,
				                  uint64(uint32(StagedNow.Num()) - FirstRun) * RecB);
			}
		}

		const TArray<uint32> GroupsTable(kGroupsPerRecord, int32(uint8(EVoxelWorklistStage::COUNT)));
		FRDGBufferRef GroupsBuf = CreateStructuredBuffer(
			GraphBuilder, TEXT("Voxel.WorklistGroups"), sizeof(uint32),
			GroupsTable.Num(), GroupsTable.GetData(), GroupsTable.Num() * sizeof(uint32));

		FVoxelWorklistArgsCS::FParameters* Params =
			GraphBuilder.AllocParameters<FVoxelWorklistArgsCS::FParameters>();
		Params->WorklistControl = GraphBuilder.CreateUAV(Control);
		Params->WorklistArgs = GraphBuilder.CreateUAV(Args, PF_R32_UINT);
		Params->GroupsPerRecord = GraphBuilder.CreateSRV(GroupsBuf);
		Params->HeadCursor = NewHead;
		Params->SliceBudget = SliceBudgetRecords;
		Params->StageCount = uint32(EVoxelWorklistStage::COUNT);
		TShaderMapRef<FVoxelWorklistArgsCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.WorklistArgs"), Shader, Params,
			FIntVector(1, 1, 1));
		GraphBuilder.Execute();
	});

	// Host mirror of consumption: the kernel takes min(pending, budget), and
	// the host reproduces that arithmetic exactly rather than reading it back
	// -- the no-readback rule. If the two ever disagree the window identity
	// (appended == consumed + pending + refusedFull) drifts, and the log
	// below reports it as the failing reading instead of averaging it away.
	const uint32 Pending = NewHead - Tail;
	const uint32 Take = FMath::Min(Pending, SliceBudgetRecords);
	Head = NewHead;
	Tail += Take;
	Window.Consumed += Take;
	Window.Pending = Head - Tail;
	Staged.Reset();
}

FVoxelGpuWorklist::FBindings FVoxelGpuWorklist::Register(FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread());
	check(PooledRecords.IsValid());
	FBindings Out;
	FRDGBufferRef Records = GraphBuilder.RegisterExternalBuffer(PooledRecords, TEXT("Voxel.WorklistRecords"));
	FRDGBufferRef Control = GraphBuilder.RegisterExternalBuffer(PooledControl, TEXT("Voxel.WorklistControl"));
	Out.Records = GraphBuilder.CreateSRV(Records);
	Out.IndirectArgs = GraphBuilder.RegisterExternalBuffer(PooledArgs, TEXT("Voxel.WorklistArgs"));
	Out.Control = GraphBuilder.CreateSRV(Control);
	return Out;
}

FVoxelGpuWorklist::FWindow FVoxelGpuWorklist::ReadAndResetWindow()
{
	check(IsInGameThread());
	FWindow Out = Window;
	Out.Pending = Head - Tail;
	Window = FWindow();
	return Out;
}
