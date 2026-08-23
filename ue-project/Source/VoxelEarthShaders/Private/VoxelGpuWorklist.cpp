// FVoxelGpuWorklist -- the persistent chunk-record ring (P3 foundation).
// SPINE WIRED 2026-08-23 (see the header's banner): FVoxelGpuMeshJobManager
// appends records and calls Flush once per tick behind -VoxelGpuWorklist=1,
// and Flush now also dispatches the indirect spine prover
// (VoxelWorklistConsume.usf) plus a ~5 s proof readback that makes GPU
// consumption a VERIFIED fact instead of a mirrored assumption. The
// generation kernels are NOT converted yet -- pass count per tick does not
// flatten until they are; see docs/gpu-worklist-plan-2026-08-23.md for the
// sequencing arithmetic.

#include "VoxelGpuWorklist.h"

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"

#include <atomic>

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

	// The spine prover: one group per consumed record, dispatched INDIRECT
	// through the Record-stage triple the args pass just wrote. See
	// VoxelWorklistConsume.usf for the whole argument.
	class FVoxelWorklistConsumeCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelWorklistConsumeCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelWorklistConsumeCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<GpuChunkWorkRecord>, WorklistRecords)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, WorklistControl)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, WorklistStats)
			SHADER_PARAMETER(uint32, RingCapacity)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()
	};

	// The proof's render->game handoff. File-scope atomics rather than members
	// for VoxelGpuBatchDetail's reason verbatim: the values are written inside
	// render commands, and one worklist exists in practice. Values are stored
	// FIRST, then the sequence (release); the game thread reads the sequence
	// (acquire) before the values.
	std::atomic<uint32> GProofLandedSeq{ 0 };
	std::atomic<uint32> GProofGpuConsumed{ 0 };
	std::atomic<uint32> GProofGpuFold{ 0 };
	std::atomic<uint32> GProofGpuBad{ 0 };
	std::atomic<uint32> GProofGpuTail{ 0 };

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
#define VOXEL_WORKLIST_CONSUME_USF "/VoxelEarth/VoxelWorklistConsume.usf"
IMPLEMENT_GLOBAL_SHADER(FVoxelWorklistConsumeCS, VOXEL_WORKLIST_CONSUME_USF, "WorklistConsumeMain", SF_Compute);

FVoxelGpuWorklist::~FVoxelGpuWorklist()
{
	// The proof readback wraps RHI staging memory; free it in render-thread
	// order behind any Flush command still touching it -- FVoxelGpuBrickStack's
	// readback-release pattern, verbatim. (The pointer is normally written on
	// the render thread; by the time a destructor can run, the game thread has
	// stopped enqueuing flushes, so this read is not racing a creation.)
	if (FRHIGPUBufferReadback* Readback = ProofReadback)
	{
		ProofReadback = nullptr;
		ENQUEUE_RENDER_COMMAND(VoxelWorklistProofRelease)(
			[Readback](FRHICommandListImmediate&)
		{
			delete Readback;
		});
	}
}

uint32 FVoxelGpuWorklist::FoldRecord(const FVoxelGpuChunkWorkRecord& Record)
{
	// MIRROR of worklistRecordFold (VoxelWorklist.ush): same 16 dwords in
	// field order (the offsets are static_asserted above), same derived
	// multipliers (0x9E3779B1 * odd index), same uint32 wraparound. The .ush
	// spells the terms out because HLSL cannot walk struct fields; this side
	// loops, because 16 mirrored literals would be 16 chances to drift.
	uint32 Dwords[16];
	static_assert(sizeof(Dwords) == sizeof(FVoxelGpuChunkWorkRecord), "the fold walks the whole record");
	FMemory::Memcpy(Dwords, &Record, sizeof(Dwords));
	uint32 F = 0;
	for (uint32 I = 0; I < 16; ++I)
	{
		F += Dwords[I] * (0x9E3779B1u * (2u * I + 1u));
	}
	return F;
}

void FVoxelGpuWorklist::Init(uint32 RecordCapacity)
{
	check(IsInGameThread());
	check(Capacity == 0 && RecordCapacity > 0);
	Capacity = RecordCapacity;
	// The proof's per-slot fold shadow: 4 bytes per slot, host-only. Zeroed,
	// matching the fold the GPU computes over a never-written all-zero slot --
	// not that either side should ever consume one.
	FoldRing.SetNumZeroed(int32(Capacity));
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
		// The prover's evidence (VoxelWorklistConsume.usf's 4-dword layout).
		// Cleared ONCE, here: the counters are cumulative for the process, so
		// the proof compares totals, never windows -- a readback that lands
		// late compares against the flush that enqueued it, not against
		// whatever window happens to be open.
		FRDGBufferRef Stats = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 4),
			TEXT("Voxel.WorklistStats"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Control), 0u);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Args, PF_R32_UINT), 0u);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Stats), 0u);
		GraphBuilder.QueueBufferExtraction(Records, &PooledRecords);
		GraphBuilder.QueueBufferExtraction(Args, &PooledArgs);
		GraphBuilder.QueueBufferExtraction(Control, &PooledControl);
		GraphBuilder.QueueBufferExtraction(Stats, &PooledStats);
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

	// Slot folds for the records being uploaded this flush -- BEFORE the
	// consume mirror below, because the args pass sees HeadCursor == NewHead
	// and can consume a record the same tick it arrives.
	for (int32 I = 0; I < Staged.Num(); ++I)
	{
		FoldRing[int32((Head + uint32(I)) % Capacity)] = FoldRecord(Staged[I]);
	}

	// Host mirror of consumption: the args kernel takes min(pending, budget),
	// and the host reproduces that arithmetic exactly rather than reading it
	// back -- the no-readback rule. Computed BEFORE the enqueue (it depends
	// only on NewHead/Tail) because the proof stash below must capture the
	// post-consume values of the SAME flush whose render command carries the
	// proof request.
	const uint32 Pending = NewHead - Tail;
	const uint32 Take = FMath::Min(Pending, SliceBudgetRecords);
	for (uint32 I = 0; I < Take; ++I)
	{
		CumConsumedFold ^= FoldRing[int32((Tail + I) % Capacity)];
	}
	CumConsumedRecords += Take;

	// --- proof landing ------------------------------------------------------
	// The render side stored the GPU's four dwords and then the sequence
	// (release); seeing our sequence here means the values are the answer to
	// the stash we captured when we requested it.
	if (bProofPending && GProofLandedSeq.load(std::memory_order_acquire) == ProofSeq)
	{
		const uint32 GpuConsumed = GProofGpuConsumed.load(std::memory_order_relaxed);
		const uint32 GpuFold = GProofGpuFold.load(std::memory_order_relaxed);
		const uint32 GpuBad = GProofGpuBad.load(std::memory_order_relaxed);
		const uint32 GpuTail = GProofGpuTail.load(std::memory_order_relaxed);
		Proof.MalformedOnGpu = GpuBad;
		++Proof.Landed;
		const bool bOk = GpuConsumed == ProofStashConsumed
		              && GpuFold == ProofStashFold
		              && GpuTail == ProofStashTail;
		if (!bOk)
		{
			++Proof.Failed;
			// Which of the three failed names the defect: consumed off means
			// the indirect dispatch ran the wrong group count (or not at
			// all -- GPU 0 with host N is the dead-spine reading); tail off
			// means the args kernel and the host disagree about the cursor;
			// fold off with the other two right means the RECORD BYTES
			// differ -- a torn ring-wrap copy or a stale slot.
			UE_LOG(LogVoxelGpuWorklist, Error,
			       TEXT("[gpu-worklist] proof #%u FAIL -- gpu consumed=%u fold=0x%08x tail=%u ")
			       TEXT("vs host consumed=%u fold=0x%08x tail=%u (malformed-on-gpu=%u). ")
			       TEXT("The GPU did not consume what the host mirrored; every number ")
			       TEXT("downstream of this worklist is suspect and the leg is invalid."),
			       ProofSeq, GpuConsumed, GpuFold, GpuTail,
			       ProofStashConsumed, ProofStashFold, ProofStashTail, GpuBad);
		}
		else
		{
			UE_LOG(LogVoxelGpuWorklist, Log,
			       TEXT("[gpu-worklist] proof #%u ok: gpu consumed=%u fold=0x%08x tail=%u ")
			       TEXT("== host (malformed-on-gpu=%u)"),
			       ProofSeq, GpuConsumed, GpuFold, GpuTail, GpuBad);
			if (GpuBad > 0)
			{
				UE_LOG(LogVoxelGpuWorklist, Error,
				       TEXT("[gpu-worklist] %u malformed records reached the GPU with the fold ")
				       TEXT("PASSING: the host is staging garbage and transporting it faithfully. ")
				       TEXT("Check the record build in FVoxelGpuMeshJobManager::DispatchBatch."),
				       GpuBad);
			}
		}
		bProofPending = false;
	}

	// --- proof request ------------------------------------------------------
	// Only ever taken with nonzero cumulative consumption, so a landed "ok"
	// can never be the vacuous 0 == 0 -- the shape of pass this project has
	// been burned by seven times in one night.
	bool bRequestProof = false;
	{
		const double Now = FPlatformTime::Seconds();
		if (!bProofPending && CumConsumedRecords > 0 && Now - LastProofSeconds >= 5.0)
		{
			bRequestProof = true;
			bProofPending = true;
			LastProofSeconds = Now;
			++ProofSeq;
			ProofStashTail = Tail + Take;
			ProofStashConsumed = uint32(CumConsumedRecords);
			ProofStashFold = CumConsumedFold;
		}
	}

	ENQUEUE_RENDER_COMMAND(VoxelWorklistFlush)(
		[this, StagedNow = MoveTemp(Staged), FirstSlot, NewHead,
		 SliceBudgetRecords, bRequestProof, RequestSeq = ProofSeq](FRHICommandListImmediate& RHICmdList)
	{
		// Land any outstanding proof copy BEFORE building this tick's graph:
		// Lock/Unlock want a quiescent readback, and the values must be
		// published before the game thread can see the sequence move.
		if (bProofCopyInFlight && ProofReadback != nullptr && ProofReadback->IsReady())
		{
			const uint32* Data = static_cast<const uint32*>(ProofReadback->Lock(4 * sizeof(uint32)));
			if (Data != nullptr)
			{
				GProofGpuConsumed.store(Data[0], std::memory_order_relaxed);
				GProofGpuFold.store(Data[1], std::memory_order_relaxed);
				GProofGpuBad.store(Data[2], std::memory_order_relaxed);
				GProofGpuTail.store(Data[3], std::memory_order_relaxed);
				GProofLandedSeq.store(ProofCopySeq, std::memory_order_release);
			}
			ProofReadback->Unlock();
			bProofCopyInFlight = false;
		}
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

		// --- the spine prover: the first indirect consumer ------------------
		// Group count comes from the Record-stage triple the args pass wrote
		// (1 group per consumed record) -- the CPU never sees it. RDG orders
		// these passes by their dependencies: the prover reads Control and
		// IndirectArgs the args pass wrote, and Records the copies above
		// filled.
		{
			FRDGBufferRef Stats = GraphBuilder.RegisterExternalBuffer(PooledStats, TEXT("Voxel.WorklistStats"));
			FVoxelWorklistConsumeCS::FParameters* CParams =
				GraphBuilder.AllocParameters<FVoxelWorklistConsumeCS::FParameters>();
			CParams->WorklistRecords = GraphBuilder.CreateSRV(Records);
			CParams->WorklistControl = GraphBuilder.CreateSRV(Control);
			CParams->WorklistStats = GraphBuilder.CreateUAV(Stats);
			CParams->RingCapacity = Capacity;
			CParams->IndirectArgs = Args;
			TShaderMapRef<FVoxelWorklistConsumeCS> ConsumeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(
				GraphBuilder, RDG_EVENT_NAME("Voxel.WorklistConsume"), ConsumeShader, CParams,
				Args, uint32(EVoxelWorklistStage::Record) * 3u * uint32(sizeof(uint32)));

			if (bRequestProof)
			{
				if (ProofReadback == nullptr)
				{
					ProofReadback = new FRHIGPUBufferReadback(TEXT("Voxel.WorklistProof"));
				}
				AddEnqueueCopyPass(GraphBuilder, ProofReadback, Stats, 4 * uint32(sizeof(uint32)));
				bProofCopyInFlight = true;
				ProofCopySeq = RequestSeq;
			}
		}
		GraphBuilder.Execute();
	});

	// Cursor advance for the Take the args kernel will take this tick -- the
	// arithmetic was mirrored above, before the enqueue; see the comment
	// there. If host and GPU ever disagree the PROOF fails loudly (that is
	// what it is for), and the window identity (appended == consumed +
	// pending) drifts as the second witness.
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
