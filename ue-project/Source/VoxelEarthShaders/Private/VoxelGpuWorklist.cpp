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

#include "VoxelGpuWorldGen.h"        // FVoxelGpuColumnSample -- the arena element
#include "VoxelGpuWorldGenGraph.h"   // AddWorklistColumnPass (the converted Column dispatch)
#include "VoxelRasterAtlasGpu.h"

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"

#include <atomic>
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
DECLARE_GPU_STAT_NAMED(VoxelStreamWorklist, TEXT("VoxelStreamWorklist"));

DEFINE_LOG_CATEGORY_STATIC(LogVoxelGpuWorklist, Log, All);

// Layout lock against the .ush mirror: 16 dwords, offsets as documented.
static_assert(offsetof(FVoxelGpuChunkWorkRecord, OriginVx) == 0, "record layout");
static_assert(offsetof(FVoxelGpuChunkWorkRecord, LevelFlags) == 12, "record layout");
static_assert(offsetof(FVoxelGpuChunkWorkRecord, ShadingSurfaceZBits) == 40, "record layout");
static_assert(offsetof(FVoxelGpuChunkWorkRecord, BrickBase) == 44, "record layout");

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
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RecordInY)
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
	// --- P2 (voxel.GPU.AsyncGen) engagement counters, render thread ---------
	// File-scope atomics for the GProof reason verbatim (written inside
	// render commands / the scene delegate, one worklist exists in practice).
	// sceneWindows: generation windows the SCENE builder consumed (the
	// overlap actually engaged); serialFallbacks: windows the next flush had
	// to build on graphics because no scene rendered (correct, no overlap);
	// windowsDropped: claim-less windows overwritten unconsumed (safe --
	// nothing ever reads a claim-less window's arena slices).
	std::atomic<int64> GAsyncGenSceneWindows{ 0 };
	std::atomic<int64> GAsyncGenSerialFallbacks{ 0 };
	std::atomic<int64> GAsyncGenWindowsDropped{ 0 };
	std::atomic<uint32> GProofGpuConsumed{ 0 };
	std::atomic<uint32> GProofGpuFold{ 0 };
	std::atomic<uint32> GProofGpuBad{ 0 };
	std::atomic<uint32> GProofGpuTail{ 0 };
	// Column-stage verify counters (stats [4..5]); cumulative, like the rest.
	std::atomic<uint32> GProofColMismatch{ 0 };
	std::atomic<uint32> GProofColChecked{ 0 };
	std::atomic<uint32> GProofVoxMismatch{ 0 };
	std::atomic<uint32> GProofVoxChecked{ 0 };
	std::atomic<uint32> GProofCtMismatch{ 0 };
	std::atomic<uint32> GProofCtChecked{ 0 };
	std::atomic<uint32> GProofStampMismatch{ 0 };
	std::atomic<uint32> GProofStampChecked{ 0 };
	std::atomic<uint32> GProofPackMismatch{ 0 };
	std::atomic<uint32> GProofPackChecked{ 0 };
	std::atomic<uint32> GProofClaimMismatch{ 0 };
	std::atomic<uint32> GProofClaimChecked{ 0 };
	// The claim stage's own TRAFFIC counter (stats[16]): how many records
	// ClaimWorklistMain found eligible, cumulative, written on every armed
	// tick whether or not a verify is armed. It is compared against the
	// host's CumClaimStaged every proof, because those two being different
	// sets is what let 96% of chunks be claimed TWICE -- once here and once
	// classically in the batch graph -- with every existing indicator green.
	std::atomic<uint32> GProofClaimEligible{ 0 };
	// The claim verify's witness triple (stats [17..19]): which categories of
	// byte disagreed, and the first record that disagreed. A count told us
	// only that there was a race; these say whether it is the descriptors
	// (the duplicate-slot mechanism), the word copies, the record composer,
	// or a slot the free path failed to clear.
	std::atomic<uint32> GProofClaimWitnessCat{ 0 };
	std::atomic<uint32> GProofClaimWitnessSlot{ 0 };
	std::atomic<uint32> GProofClaimWitnessRec{ 0 };

	// Groups per record per stage -- the host copy of the stage shapes the
	// converted kernels are written against. The Column entry is LOCKED: it
	// comes from FVoxelGpuWorklist::kColumnGroupsPerRecord, the same constant
	// FVoxelWorklistColumnCS hands the kernel as a define, and
	// VoxelWorklistColumn.usf #errors on disagreement -- the torn-dispatch
	// lock the plan doc mandates per converted kernel. The unconverted
	// entries are still the DESIGN numbers from the plan doc; each locks the
	// day its kernel lands.
	// constexpr, not const: the static_asserts below INDEX these tables, and an
	// lvalue-to-rvalue conversion on a subobject of a merely-const array is not
	// a constant expression. Making them constexpr is what lets the lock exist.
	constexpr uint32 kGroupsPerRecord[uint8(EVoxelWorklistStage::COUNT)] =
	{
		FVoxelGpuWorklist::kColumnGroupsPerRecord,   // Column: 1024 columns / 64 = 16 (LOCKED)
		// Voxelize keeps the classic kernel's one-thread-per-COLUMN mapping
		// (cave/cavern reductions are per column), not the plan's provisional
		// per-cell 512: 1024 columns / 64 = 16.
		FVoxelGpuWorklist::kVoxelizeGroupsPerRecord, // Voxelize: (LOCKED)
		FVoxelGpuWorklist::kStampGroupsPerRecord,    // AssetStamp: per-column gather (LOCKED)
		// Classify keeps the classic one-group-per-BRICK shape
		// (brickBuildOccupancy is groupshared), not the plan's provisional
		// single group; the scan + totals live in the next stage's 1 group.
		FVoxelGpuWorklist::kClassifyGroupsPerRecord,       // Classify: (LOCKED)
		FVoxelGpuWorklist::kClassifyTotalsGroupsPerRecord, // ClassifyTotals: (LOCKED)
		// The PackClaim slot carries the PACK dispatch (one group per brick,
		// the classic pack shape); the claim stays per-chunk in the batch
		// graph, sourcing from the pack arenas, until the Write/Record
		// stages land.
		FVoxelGpuWorklist::kPackGroupsPerRecord,           // PackClaim: (LOCKED, pack half)
		// The Claim stage's word-copy dispatch: worst-case groups per record
		// ((1,024 occ + 8,448 mat) / 64 = 148 -- the classic copies' own
		// worst-case-dispatch decision), locked against the kernel's #error.
		FVoxelGpuWorklist::kWriteGroupsPerRecord,          // Write: (LOCKED)
		// Record serves the spine prover AND the Claim stage's claim,
		// desc+record and verify dispatches -- all 1 group per record.
		1,    // Record (LOCKED)
	};

	// Which dimension carries the RECORD, parallel to kGroupsPerRecord and
	// uploaded beside it. 0 = X ({Take x groups, 1, 1}); 1 = Y ({groups,
	// Take, 1}). Only the Write stage uses Y, and only because its 148 groups
	// per record capped a flush at 65,535/148 = 442 records = ~26,500
	// chunks/s -- the lowest ceiling of any stage and below the 50,000 goal.
	// See FVoxelGpuWorklist::kWriteRecordInY for the whole argument and for
	// the #error that locks the consuming kernel to this same constant.
	constexpr uint32 kRecordInY[uint8(EVoxelWorklistStage::COUNT)] =
	{
		0,  // Column
		0,  // Voxelize
		0,  // AssetStamp
		0,  // Classify
		0,  // ClassifyTotals
		0,  // PackClaim
		FVoxelGpuWorklist::kWriteRecordInY,   // Write (LOCKED, and the only Y)
		0,  // Record
	};

	// THE CAP AND THE TABLE CANNOT DRIFT APART. kMaxRecordsPerFlush is
	// 65,535/64, which is only the true per-flush ceiling while every stage
	// that still carries its record in X has at most 64 groups per record.
	// Add a stage with more, or move one off Y, and this fires at compile
	// time instead of clipping a dispatch on a leg.
	// AND THE POSITION ITSELF, because kRecordInY is a POSITIONAL table with
	// exactly one nonzero entry. Insert a stage into the enum above Write and
	// every entry shifts: Y lands on Pack (whose kernel reads its record from
	// Gid.x) and the Write triple goes back to 1D -- so Pack silently
	// processes only record 0 AND the word copies silently clip past 442
	// records. Two silent failures from one insertion. This pins it.
	// THE FAMILY LOCK (2026-08-23). kRecordInY was flagged as a positional
	// table pinned by an assert rather than by the type system -- and the
	// audit that followed found it is not alone. TWO tables here are indexed
	// by this enum's ORDER (kGroupsPerRecord and kRecordInY) and neither
	// entry names the stage it belongs to. Every OTHER consumer of the enum
	// is symbolic -- every args offset is uint32(EVoxelWorklistStage::X) * 3,
	// which moves with the enum because the args kernel writes at the same S
	// the C++ names -- so the tables are the whole exposure, and this is it
	// closed.
	//
	// WHY IT MATTERS MORE FOR kGroupsPerRecord: insert one stage and every
	// group count slides onto the wrong stage. Column/Voxelize/AssetStamp are
	// all 16, so a slide near the top is INVISIBLE; a slide across
	// Classify(64) / ClassifyTotals(1) / PackClaim(64) / Write(148) /
	// Record(1) dispatches wildly wrong group counts with no error anywhere.
	// So the lock is not "COUNT is 8" -- it is every index, by name, plus
	// every table entry cross-checked against the constant it is supposed to
	// be. A positional table that is checked entry-by-entry is no longer a
	// positional table.
	static_assert(uint8(EVoxelWorklistStage::Column) == 0
	           && uint8(EVoxelWorklistStage::Voxelize) == 1
	           && uint8(EVoxelWorklistStage::AssetStamp) == 2
	           && uint8(EVoxelWorklistStage::Classify) == 3
	           && uint8(EVoxelWorklistStage::ClassifyTotals) == 4
	           && uint8(EVoxelWorklistStage::PackClaim) == 5
	           && uint8(EVoxelWorklistStage::Write) == 6
	           && uint8(EVoxelWorklistStage::Record) == 7
	           && uint8(EVoxelWorklistStage::COUNT) == 8,
	              "kGroupsPerRecord and kRecordInY are POSITIONAL tables -- pin every index, "
	              "not just COUNT: a stage inserted anywhere slides every entry onto the "
	              "wrong stage, and the three 16s at the top would hide it");
	// Entry-by-entry, so the tables cannot drift from the constants they
	// mirror even without an enum change (a hand-edited entry is the other
	// half of the same hazard).
	static_assert(kGroupsPerRecord[uint8(EVoxelWorklistStage::Column)]
	                  == FVoxelGpuWorklist::kColumnGroupsPerRecord
	           && kGroupsPerRecord[uint8(EVoxelWorklistStage::Voxelize)]
	                  == FVoxelGpuWorklist::kVoxelizeGroupsPerRecord
	           && kGroupsPerRecord[uint8(EVoxelWorklistStage::AssetStamp)]
	                  == FVoxelGpuWorklist::kStampGroupsPerRecord
	           && kGroupsPerRecord[uint8(EVoxelWorklistStage::Classify)]
	                  == FVoxelGpuWorklist::kClassifyGroupsPerRecord
	           && kGroupsPerRecord[uint8(EVoxelWorklistStage::ClassifyTotals)]
	                  == FVoxelGpuWorklist::kClassifyTotalsGroupsPerRecord
	           && kGroupsPerRecord[uint8(EVoxelWorklistStage::PackClaim)]
	                  == FVoxelGpuWorklist::kPackGroupsPerRecord
	           && kGroupsPerRecord[uint8(EVoxelWorklistStage::Write)]
	                  == FVoxelGpuWorklist::kWriteGroupsPerRecord
	           && kGroupsPerRecord[uint8(EVoxelWorklistStage::Record)] == 1,
	              "a kGroupsPerRecord entry no longer matches the constant its stage's kernel "
	              "is compiled against -- that is a torn dispatch, silent");
	static_assert(kRecordInY[uint8(EVoxelWorklistStage::Write)]
	                  == FVoxelGpuWorklist::kWriteRecordInY
	           && kRecordInY[uint8(EVoxelWorklistStage::Column)] == 0
	           && kRecordInY[uint8(EVoxelWorklistStage::Voxelize)] == 0
	           && kRecordInY[uint8(EVoxelWorklistStage::AssetStamp)] == 0
	           && kRecordInY[uint8(EVoxelWorklistStage::Classify)] == 0
	           && kRecordInY[uint8(EVoxelWorklistStage::ClassifyTotals)] == 0
	           && kRecordInY[uint8(EVoxelWorklistStage::PackClaim)] == 0
	           && kRecordInY[uint8(EVoxelWorklistStage::Record)] == 0,
	              "exactly one stage carries its record in Y and it must be Write -- "
	              "any other kernel reading Gid.y would see 0 and process only record 0");
	static_assert(FVoxelGpuWorklist::kMaxRecordsPerFlush == 65535 / 64,
	              "kMaxRecordsPerFlush must stay the 1D stages' own ceiling");
	static_assert(FVoxelGpuWorklist::kWriteRecordInY == 1,
	              "the Write stage carries its record in Y; the kernel #errors on anything else");
	static_assert(FVoxelGpuWorklist::kColumnGroupsPerRecord <= 64
	           && FVoxelGpuWorklist::kVoxelizeGroupsPerRecord <= 64
	           && FVoxelGpuWorklist::kStampGroupsPerRecord <= 64
	           && FVoxelGpuWorklist::kClassifyGroupsPerRecord <= 64
	           && FVoxelGpuWorklist::kClassifyTotalsGroupsPerRecord <= 64
	           && FVoxelGpuWorklist::kPackGroupsPerRecord <= 64,
	              "a stage carrying its record in X now exceeds 64 groups/record -- "
	              "kMaxRecordsPerFlush is no longer 65535/64; move it to Y or lower the cap");
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
		// RECORDS IS ALLOCATED OUTSIDE THE GRAPH. It is written by CPU uploads,
		// not by any RDG pass, so creating it here and extracting it hits
		//   Assertion failed: Resource->bProduced || bExternal || bQueuedForUpload
		//   "Unable to queue the extraction of Voxel.WorklistRecords because it
		//    has not been produced by any pass."
		// The same trap the raster atlas hit at init an hour earlier. Clearing
		// it to satisfy RDG would zero 256 KiB every session for nothing --
		// records are only ever read below the tail cursor the args kernel
		// publishes, so untouched bytes are unreachable by construction.
		PooledRecords = AllocatePooledBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVoxelGpuChunkWorkRecord), Capacity),
			TEXT("Voxel.WorklistRecords"));

		// ON THE RHI COMMAND LIST, NOT THE GRAPH, AND THAT IS FORCED. An
		// RDG_EVENT_SCOPE_STAT here asserts at FRDGBuilder::Execute --
		// RenderGraphBuilder.cpp:1770 checks the graph's breadcrumb is back at
		// Sentinel -- because these builders Execute inside the scope rather than
		// after it. Measured: it crashed the first leg at VoxelRasterAtlasGpu.
		// RHI_BREADCRUMB_EVENT_STAT is the same stat on the RHI timeline, feeds
		// the same GPU/<name> CSV column, and outlives the graph by construction
		// (declared before it, destroyed after it).
		RHI_BREADCRUMB_EVENT_STAT(RHICmdList, VoxelStreamWorklist, "VoxelStreamWorklist");
		FRDGBuilder GraphBuilder(RHICmdList);
		FRDGBufferRef Args = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(
				uint32(EVoxelWorklistStage::COUNT)),
			TEXT("Voxel.WorklistArgs"));
		FRDGBufferRef Control = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 3),
			TEXT("Voxel.WorklistControl"));
		// The evidence buffer (prover dwords [0..3], column verify [4..5];
		// VoxelWorklistConsume.usf documents the layout). Cleared ONCE, here:
		// the counters are cumulative for the process, so the proof compares
		// totals, never windows -- a readback that lands late compares
		// against the flush that enqueued it, not against whatever window
		// happens to be open.
		FRDGBufferRef Stats = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), kStatsDwords),
			TEXT("Voxel.WorklistStats"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Control), 0u);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Args, PF_R32_UINT), 0u);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Stats), 0u);
		// Records is already pooled above and never entered the graph; only the
		// three CLEARED buffers are extracted.
		GraphBuilder.QueueBufferExtraction(Args, &PooledArgs);
		GraphBuilder.QueueBufferExtraction(Control, &PooledControl);
		GraphBuilder.QueueBufferExtraction(Stats, &PooledStats);
		GraphBuilder.Execute();
	});
}

int32 FVoxelGpuWorklist::Append(TArrayView<const FVoxelGpuChunkWorkRecord> Records,
                                TArray<uint32>* OutMonotonicIndices,
                                TArray<FVoxelWorklistAssetPayload>* AssetPayloads)
{
	check(IsInGameThread());
	check(IsInitialized());
	check(AssetPayloads == nullptr || AssetPayloads->Num() == Records.Num());
	if (OutMonotonicIndices != nullptr)
	{
		OutMonotonicIndices->Reset();
		OutMonotonicIndices->Reserve(Records.Num());
	}
	// Head/Tail are MONOTONIC record counts (the ring index is count %
	// Capacity), so pending is a plain subtraction and wrap costs nothing.
	int32 Accepted = 0;
	for (int32 RIdx = 0; RIdx < Records.Num(); ++RIdx)
	{
		const FVoxelGpuChunkWorkRecord& R = Records[RIdx];
		if (Head - Tail + uint32(Staged.Num()) >= Capacity)
		{
			++Window.RefusedFull;
			if (OutMonotonicIndices != nullptr)
			{
				OutMonotonicIndices->Add(MAX_uint32);
			}
			continue;
		}
		// The monotonic index this record will occupy: Head advances only at
		// Flush, so a staged record's index is Head + its staging position.
		if (OutMonotonicIndices != nullptr)
		{
			OutMonotonicIndices->Add(Head + uint32(Staged.Num()));
		}
		Staged.Add(R);
		// StagedAssets stays PARALLEL to Staged even when the caller passed
		// no payloads -- Flush indexes the two together.
		StagedAssets.AddDefaulted();
		if (AssetPayloads != nullptr && !(*AssetPayloads)[RIdx].IsEmpty())
		{
			StagedAssets.Last() = MoveTemp((*AssetPayloads)[RIdx]);
		}
		++Accepted;
	}
	Window.Appended += uint64(Accepted);
	return Accepted;
}

void FVoxelGpuWorklist::SetColumnStageInputs(FVoxelRasterAtlasGpu* Atlas, uint64 Seed,
                                             int32 PixelSizeMm)
{
	check(IsInGameThread());
	if (Atlas == nullptr || PixelSizeMm == 0)
	{
		// A null atlas or zero pitch cannot dispatch columns; stay (or go)
		// unarmed rather than dispatch a kernel whose guard would silently
		// early-out every thread -- the exact quiet-dead shape this project
		// keeps paying for. The caller counts the fallback per chunk.
		bColumnStageArmed = false;
		return;
	}
	ColumnAtlas = Atlas;
	ColumnSeedLo = uint32(Seed & 0xffffffffull);
	ColumnSeedHi = uint32(Seed >> 32);
	ColumnPixelSizeMm = PixelSizeMm;
	bColumnStageArmed = true;
}

void FVoxelGpuWorklist::SetVoxelizeStageArmed(bool bArmed, uint32 CellBudgetRecords)
{
	check(IsInGameThread());
	if (bArmed && CellBudgetRecords == 0)
	{
		// A zero cell budget would clamp every flush's Take to zero -- the
		// worklist would consume NOTHING while every counter upstream looked
		// armed. Refuse the arm loudly instead of running dead.
		UE_LOG(LogVoxelGpuWorklist, Error,
		       TEXT("[gpu-worklist] Voxelize stage arm REFUSED: cell budget 0 would clamp ")
		       TEXT("consumption to zero. Set -VoxelGpuWorklistCellBudget."));
		bVoxelizeStageArmed = false;
		return;
	}
	bVoxelizeStageArmed = bArmed;
	VoxelizeCellBudget = CellBudgetRecords;
}

void FVoxelGpuWorklist::SetClaimStageArmed(bool bArmed, FPoolBinder Binder, bool bVerify)
{
	check(IsInGameThread());
	if (bArmed && !Binder)
	{
		// A claim stage with no way to reach the pool would set claimStaged
		// bits whose records nothing ever lands -- ghost chunks with an armed
		// switch. Refuse loudly instead of running dead.
		UE_LOG(LogVoxelGpuWorklist, Error,
		       TEXT("[gpu-worklist] Claim stage arm REFUSED: no pool binder was supplied."));
		bClaimStageArmed = false;
		ClaimPoolBinder = nullptr;
		return;
	}
	bClaimStageArmed = bArmed;
	bClaimVerifyArmed = bVerify;
	ClaimPoolBinder = MoveTemp(Binder);
}

void FVoxelGpuWorklist::Flush(uint32 SliceBudgetRecords)
{
	check(IsInGameThread());
	check(IsInitialized());
	// Voxelize stage armed: the CELL arena caps how many records one flush
	// may consume -- 128 KiB per slice means the ring budget's default 1,024
	// would be a 128 MiB arena. Clamped HERE, before the host consume mirror
	// and the captured args budget, so the GPU's min(pending, budget), the
	// host's, and the arena size are the same number by construction. The
	// unconsumed remainder stays pending in the ring for the next tick;
	// sustained clamping shows up as pending>0 across window lines, and the
	// relief valve is -VoxelGpuWorklistCellBudget.
	if (IsVoxelizeStageArmed())
	{
		SliceBudgetRecords = FMath::Min(SliceBudgetRecords, VoxelizeCellBudget);
	}
	const uint32 NewHead = Head + uint32(Staged.Num());
	const uint32 FirstSlot = Head % Capacity;

	// Host mirror of consumption: the args kernel takes min(pending, budget),
	// and the host reproduces that arithmetic exactly rather than reading it
	// back -- the no-readback rule. Computed BEFORE the enqueue (it depends
	// only on NewHead/Tail) because the proof stash below must capture the
	// post-consume values of the SAME flush whose render command carries the
	// proof request -- and now also BEFORE the folds, because the AssetStamp
	// staging below MUTATES staged records (AssetBase + the stampsStaged
	// bit) for exactly the records this flush will consume, and both fold
	// mirrors must see the post-mutation bytes.
	const uint32 Pending = NewHead - Tail;
	const uint32 Take = FMath::Min(Pending, SliceBudgetRecords);

	// --- AssetStamp staging (-VoxelGpuWorklistAssetStamp) -------------------
	//
	// For every STAGED record that (a) carries an asset payload and (b) will
	// be consumed BY THIS FLUSH (its monotonic index falls inside the take
	// window -- the same arithmetic the GPU runs), concatenate its instances
	// into this flush's blob, rebase ColStartsBase into the blob's ColStarts
	// and the ColStarts VALUES into the blob's Spans, write the record's
	// AssetBase and set stampsStaged (bit 9). A record deferred past this
	// flush keeps bit 9 CLEAR forever -- the GPU-side chain skips it and the
	// host already counted its job as a fallback -- so no record can ever
	// read another flush's blob. Payloads of deferred/asset-free records are
	// dropped with the staging arrays either way.
	TArray<FVoxelWorklistAssetInstance> FlushInstances;
	TArray<uint32> FlushColStarts;
	TArray<uint32> FlushSpans;
	if (IsAssetStampStageArmed())
	{
		for (int32 I = 0; I < Staged.Num(); ++I)
		{
			FVoxelWorklistAssetPayload& P = StagedAssets[I];
			if (P.IsEmpty())
			{
				continue;
			}
			const uint32 Mono = Head + uint32(I);
			if (Mono - Tail >= Take)
			{
				continue;   // deferred: bit 9 stays clear, host fell back
			}
			const uint32 InstanceBase = uint32(FlushInstances.Num());
			const uint32 ColStartsOffset = uint32(FlushColStarts.Num());
			const uint32 SpansOffset = uint32(FlushSpans.Num());
			for (FVoxelWorklistAssetInstance Inst : P.Instances)
			{
				Inst.ColStartsBase += ColStartsOffset;
				FlushInstances.Add(Inst);
			}
			for (uint32 CS : P.ColStarts)
			{
				FlushColStarts.Add(CS + SpansOffset);   // values index Spans
			}
			FlushSpans.Append(P.Spans);
			Staged[I].AssetBase = InstanceBase;
			Staged[I].LevelFlags |= (1u << 9);
		}
	}
	StagedAssets.Reset();

	// --- Claim staging (-VoxelGpuWorklistClaim; P3 stage 6) -----------------
	//
	// claimStaged (bit 10) is set for exactly the STAGED records this flush
	// will consume -- the asset-staging window arithmetic verbatim, and for
	// the same reason: a record deferred past its staging flush keeps the bit
	// clear FOREVER (it is uploaded with it clear and never re-uploaded), so
	// a later flush consuming it claims nothing -- its host job already fell
	// back to the classic per-chunk claim, and a second claim for the same
	// slot would leak the first ranges and overwrite the side table. BEFORE
	// the folds, so both proof mirrors cover the post-mutation bytes.
	//
	// AND bit 11 (hostClaimCandidate), which the manager stamps at record
	// BUILD time for exactly the jobs whose batch graph will run NO classic
	// claim. Without that conjunct this loop stamped bit 10 on every consumed
	// record, while the host vetoed the majority of them AFTER the flush
	// (a stack-fused member claims classically through AddBrickStackPasses)
	// -- so those slots were claimed TWICE, once here and once in the batch
	// graph, the first grant leaked, and [brick-gpualloc] `unclaimed` ran to
	// -643,164 (claims + claimFails - shells: claims with no shell behind
	// them). THE GPU'S ELIGIBLE SET AND THE HOST'S CONVERTED SET MUST BE THE
	// SAME SET; ClaimStagedThisFlush below is the host's own count of it and
	// the manager cross-checks it against the GPU's every proof.
	ClaimStagedThisFlush = 0;
	if (IsClaimStageArmed())
	{
		// THE TRIPWIRE THAT MUST STAY AT ZERO. Two records for one ChunkSlot
		// in one flush both run poolClaimChunkAt, and the second overwrites
		// the first's side-table entry -- the first grant leaks and the
		// loser's landed descriptors disagree with its own sources. That is
		// the diagnosed cause of claimverify mism=138/174, and the manager
		// now refuses the second record at build time (wlcols skips
		// dupSlot=). This checks the property the manager is supposed to
		// guarantee, HERE, where the bit is actually stamped -- because the
		// manager's guard covers one batch and this is the set that really
		// claims. If the manager ever grows a second path into the ring, or
		// batch scope stops equalling flush scope, this fires and the fix has
		// a gap. Checked, not believed.
		//
		// The set is bounded by the flush budget (384 default, 1,023 ceiling),
		// so this is a few hundred hash inserts per tick against a per-tick
		// cost that is already flat in N.
		ClaimSlotsThisFlush.Reset();
		for (int32 I = 0; I < Staged.Num(); ++I)
		{
			const uint32 Mono = Head + uint32(I);
			if (Mono - Tail < Take && (Staged[I].LevelFlags & (1u << 11)) != 0u)
			{
				bool bAlreadySeen = false;
				ClaimSlotsThisFlush.Add(Staged[I].ChunkSlot, &bAlreadySeen);
				if (bAlreadySeen)
				{
					// Refuse the bit rather than trust the caller. The record
					// still rides the arena chain (its columns/cells/pack are
					// harmless per-slice work); it simply does not claim, and
					// the manager already fell its job back.
					++CumClaimDupRefused;
					if (!bClaimDupLogged)
					{
						bClaimDupLogged = true;
						UE_LOG(LogVoxelGpuWorklist, Error,
						       TEXT("[gpu-worklist] DUPLICATE CLAIM SLOT %u staged twice in one ")
						       TEXT("flush. Both records would claim, the second would overwrite ")
						       TEXT("the first's side-table entry, the first grant would LEAK and ")
						       TEXT("the loser's descriptors would fail claimverify. Refused here, ")
						       TEXT("but the manager's own dupSlot guard should have caught it ")
						       TEXT("first -- it has a gap. Logged once; the count is on the ")
						       TEXT("wlclaim line."),
						       Staged[I].ChunkSlot);
					}
					continue;
				}
				Staged[I].LevelFlags |= (1u << 10);
				++ClaimStagedThisFlush;
			}
		}
	}
	CumClaimStaged += ClaimStagedThisFlush;

	// --- P2 (voxel.GPU.AsyncGen): the one-slot deferred-claim pipeline ------
	//
	// bAsyncGenArmed is the manager's per-tick latch (it computes the
	// effective arm ONCE per tick, so a cvar flip can never split a flush
	// into half-deferred state -- house rule: the pass flags and the deferred
	// publication move TOGETHER or not at all). Three moves here:
	//
	//   * HeadClaim: the claim work flush N-1 stashed. Captured into THIS
	//     flush's render command unconditionally -- even if the arm dropped
	//     this tick -- because window N-1's records were uploaded with bit 10
	//     set and consumed; if their claim never ran they would be chunks the
	//     host believes published that nothing ever landed.
	//   * DeferredClaim: THIS window's claim work, stashed for flush N+1.
	//     Only when the arm is up, the claim stage is armed and something was
	//     actually staged -- an empty window has nothing to publish and must
	//     not inflate the engagement counters.
	//   * The counters: counted at PUBLISH-enqueue time (HeadClaim consumed),
	//     which is the "batches published deferred" number the window line
	//     prints. It can FAIL two ways: staying 0 while wlclaim hostStaged
	//     grows (deferral armed, head never runs), or growing while
	//     gpuClaimed (stats[16]) stays flat (head enqueued, kernel dark).
	const bool bAsyncGen = bAsyncGenArmed;
	const FDeferredClaimStash HeadClaim = DeferredClaim;
	DeferredClaim = FDeferredClaimStash{};
	if (bAsyncGen && IsClaimStageArmed() && ClaimStagedThisFlush > 0)
	{
		DeferredClaim.bValid = true;
		// The CLAMPED budget (the cell-budget clamp above already ran), so
		// the deferred claim buffer is sized by the same number the args
		// kernel used for this window -- an identity, not a hope.
		DeferredClaim.BudgetRecords = FMath::Max(SliceBudgetRecords, 1u);
		DeferredClaim.bVerify = bClaimVerifyArmed;
		DeferredClaim.StagedRecords = ClaimStagedThisFlush;
	}
	// The manager's publish trigger -- set from the STASH being consumed, not
	// from the arm, so a mid-leg toggle can never strand a published-in-
	// flight batch (see the accessor's comment).
	bLastFlushCarriedDeferredClaim = HeadClaim.bValid;
	if (HeadClaim.bValid)
	{
		++CumDeferredClaimBatches;
		CumDeferredClaimRecords += int64(HeadClaim.StagedRecords);
	}

	// Slot folds for the records being uploaded this flush -- BEFORE the
	// consume-fold mirror below, because the args pass sees HeadCursor ==
	// NewHead and can consume a record the same tick it arrives. AFTER the
	// asset staging above, so the fold covers the post-mutation bytes the
	// GPU will actually read.
	for (int32 I = 0; I < Staged.Num(); ++I)
	{
		FoldRing[int32((Head + uint32(I)) % Capacity)] = FoldRecord(Staged[I]);
	}
	for (uint32 I = 0; I < Take; ++I)
	{
		CumConsumedFold ^= FoldRing[int32((Tail + I) % Capacity)];
	}
	CumConsumedRecords += Take;
	// The consume window this flush mirrors, published for the column-stage
	// caller: a record at monotonic index m gets arena slice m - ConsumeFirst
	// iff that difference is < Take -- the same arithmetic ColumnWorklistMain
	// runs off WorklistControl on the GPU.
	LastFlush.ConsumeFirst = Tail;
	LastFlush.Take = Take;

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
		const uint32 ColMismatch = GProofColMismatch.load(std::memory_order_relaxed);
		const uint32 ColChecked = GProofColChecked.load(std::memory_order_relaxed);
		const uint32 VoxMismatch = GProofVoxMismatch.load(std::memory_order_relaxed);
		const uint32 VoxChecked = GProofVoxChecked.load(std::memory_order_relaxed);
		const uint32 CtMismatch = GProofCtMismatch.load(std::memory_order_relaxed);
		const uint32 CtChecked = GProofCtChecked.load(std::memory_order_relaxed);
		const uint32 StampMismatch = GProofStampMismatch.load(std::memory_order_relaxed);
		const uint32 StampChecked = GProofStampChecked.load(std::memory_order_relaxed);
		const uint32 PackMismatch = GProofPackMismatch.load(std::memory_order_relaxed);
		const uint32 PackChecked = GProofPackChecked.load(std::memory_order_relaxed);
		const uint32 ClaimMismatch = GProofClaimMismatch.load(std::memory_order_relaxed);
		const uint32 ClaimChecked = GProofClaimChecked.load(std::memory_order_relaxed);
		const uint32 ClaimEligible = GProofClaimEligible.load(std::memory_order_relaxed);
		Proof.ClaimWitnessCategories = GProofClaimWitnessCat.load(std::memory_order_relaxed);
		Proof.ClaimWitnessSlot = GProofClaimWitnessSlot.load(std::memory_order_relaxed);
		Proof.ClaimWitnessRecord = GProofClaimWitnessRec.load(std::memory_order_relaxed);
		GpuClaimEligible = int64(ClaimEligible);
		Proof.ClaimEligibleOnGpu = ClaimEligible;
		Proof.ClaimStagedOnHost = CumClaimStaged;
		Proof.MalformedOnGpu = GpuBad;
		Proof.ColumnDwordMismatches = ColMismatch;
		Proof.ColumnsChecked = ColChecked;
		Proof.VoxCellMismatches = VoxMismatch;
		Proof.VoxCellsChecked = VoxChecked;
		Proof.CtDwordMismatches = CtMismatch;
		Proof.CtDwordsChecked = CtChecked;
		Proof.StampCellMismatches = StampMismatch;
		Proof.StampCellsChecked = StampChecked;
		Proof.PackDwordMismatches = PackMismatch;
		Proof.PackDwordsChecked = PackChecked;
		Proof.ClaimDwordMismatches = ClaimMismatch;
		Proof.ClaimDwordsChecked = ClaimChecked;
		++Proof.Landed;
		// --- THE SET-IDENTITY GATE (the one that was missing) ---------------
		//
		// The records the GPU claims and the records the host staged for a GPU
		// claim must be THE SAME SET. Both counters are cumulative and both
		// are captured at the same flush, so the compare is exact -- with one
		// allowance: the GPU number is as of the flush whose readback landed,
		// which is a few flushes behind the host's, so the GPU may be BEHIND.
		// It may never be AHEAD.
		//
		// GPU AHEAD OF HOST is the double claim: the flush graph claimed a
		// slot the batch graph also claims classically, the first grant is
		// unreachable forever, and the pool arena fills at exactly that rate.
		// Its other face is [brick-gpualloc] `unclaimed` going NEGATIVE --
		// claims + claimFails exceeding shells, claims with no shell behind
		// them. The leg that found this read unclaimed = -643,164.
		if (int64(ClaimEligible) > CumClaimStaged)
		{
			UE_LOG(LogVoxelGpuWorklist, Error,
			       TEXT("[gpu-worklist] CLAIM SET MISMATCH: the GPU claimed %u records but ")
			       TEXT("the host staged only %lld for a GPU claim (excess %lld). Every ")
			       TEXT("excess record is a slot claimed TWICE -- once in the flush graph, ")
			       TEXT("once classically in the batch graph -- and the first grant of each ")
			       TEXT("pair LEAKS. Cross-check [brick-gpualloc] `unclaimed`: it will be ")
			       TEXT("negative by about this much. The leg is invalid."),
			       ClaimEligible, CumClaimStaged, int64(ClaimEligible) - CumClaimStaged);
		}
		if (ClaimEligible == 0u && CumClaimStaged > 0)
		{
			// The other direction, and it is NOT harmless: the host skipped
			// the batch graph's brick chain for chunks it believed the flush
			// graph would land. Nothing claimed them; they arrive unwritten.
			UE_LOG(LogVoxelGpuWorklist, Error,
			       TEXT("[gpu-worklist] CLAIM STAGE DARK: host staged %lld records for a GPU ")
			       TEXT("claim and the GPU claimed 0. Those chunks' batch brick chains were ")
			       TEXT("skipped and nothing landed them -- expect holes, not corruption."),
			       CumClaimStaged);
		}
		if (ClaimMismatch > 0)
		{
			// THE CLAIM FAILING READING: the pool holds different bytes than
			// the Claim stage's own sources demand -- a torn dispatch, a
			// wrong base, a racing tenant. POOL CORRUPTION at the landing
			// site; the leg is invalid outright.
			const uint32 Cat = Proof.ClaimWitnessCategories;
			UE_LOG(LogVoxelGpuWorklist, Error,
			       TEXT("[gpu-worklist] CLAIM VERIFY FAIL: %u mismatching dwords over %u ")
			       TEXT("compared (cumulative). The landed pool state and the Claim stage's ")
			       TEXT("sources disagree; the leg is invalid. WITNESS: categories=%s%s%s%s%s ")
			       TEXT("first failing ChunkSlot=%d record=%u. DESC ALONE is the duplicate-slot ")
			       TEXT("signature: two records for one shell describe the same terrain, so ")
			       TEXT("only the rebased offsets of NON-EMPTY bricks differ -- 2 dwords each, ")
			       TEXT("which is why every increment observed was even. WORDS point at the ")
			       TEXT("copies instead, and FAILZERO at a slot the free path did not clear."),
			       ClaimMismatch, ClaimChecked,
			       (Cat & (1u << 0)) ? TEXT("desc ") : TEXT(""),
			       (Cat & (1u << 1)) ? TEXT("occWords ") : TEXT(""),
			       (Cat & (1u << 2)) ? TEXT("matWords ") : TEXT(""),
			       (Cat & (1u << 3)) ? TEXT("recordDwords ") : TEXT(""),
			       (Cat & (1u << 4)) ? TEXT("failZero ") : TEXT(""),
			       int32(Proof.ClaimWitnessSlot) - 1,
			       Proof.ClaimWitnessRecord >> 8);
		}
		if (PackMismatch > 0)
		{
			// THE PACK FAILING READING: the converted pack emitted different
			// bytes than the classic one, and those bytes are the POOL
			// PAYLOAD ITSELF. Leg invalid outright.
			UE_LOG(LogVoxelGpuWorklist, Error,
			       TEXT("[gpu-worklist] PACK VERIFY FAIL: %u mismatching dwords over %u ")
			       TEXT("compared (cumulative). The converted Pack stage and the classic ")
			       TEXT("BrickPackMain disagree; the leg is invalid."),
			       PackMismatch, PackChecked);
		}
		if (StampMismatch > 0)
		{
			// THE ASSETSTAMP FAILING READING: the gather stamped different
			// cells than the classic per-instance passes -- a wrong asset
			// voxel is in the pool. Leg invalid.
			UE_LOG(LogVoxelGpuWorklist, Error,
			       TEXT("[gpu-worklist] ASSETSTAMP VERIFY FAIL: %u mismatching cells over ")
			       TEXT("%u compared (cumulative). The order-preserving gather and the ")
			       TEXT("classic per-instance stamp chain disagree; the leg is invalid."),
			       StampMismatch, StampChecked);
		}
		if (CtMismatch > 0)
		{
			// THE CLASSIFYTOTALS FAILING READING: the fused stage's offsets
			// or totals disagree with the classic chain's. Those numbers are
			// what BrickPack writes AT and what the claim SIZES -- this is
			// pool corruption, not cosmetics. Leg invalid.
			UE_LOG(LogVoxelGpuWorklist, Error,
			       TEXT("[gpu-worklist] CLASSIFYTOTALS VERIFY FAIL: %u mismatching dwords ")
			       TEXT("over %u compared (cumulative). The fused ClassifyTotals stage and ")
			       TEXT("the classic classify+scan chain disagree; the leg is invalid."),
			       CtMismatch, CtChecked);
		}
		if (VoxMismatch > 0)
		{
			// THE VOXELIZE-STAGE FAILING READING, the column one's twin: the
			// converted kernel computed different CELLS than the classic
			// dispatch of the same chunk. Those cells are what BrickPack put
			// in the pool -- the leg is invalid and the terrain is wrong.
			UE_LOG(LogVoxelGpuWorklist, Error,
			       TEXT("[gpu-worklist] VOXELIZE VERIFY FAIL: %u mismatching cells over %u ")
			       TEXT("compared (cumulative). The converted Voxelize kernel and the classic ")
			       TEXT("VoxelizeMain disagree; the leg is invalid."),
			       VoxMismatch, VoxChecked);
		}
		if (ColMismatch > 0)
		{
			// THE COLUMN-STAGE FAILING READING: the converted kernel computed
			// different bytes than the classic dispatch of the same chunk.
			// Terrain built from those columns is WRONG terrain -- the leg is
			// invalid, and the pinned digest will move the moment the verify
			// path samples it. Logged on every proof while nonzero (the
			// counter is cumulative): this must not be missable.
			UE_LOG(LogVoxelGpuWorklist, Error,
			       TEXT("[gpu-worklist] COLUMN VERIFY FAIL: %u mismatching dwords over %u ")
			       TEXT("columns compared (cumulative). The converted Column kernel and the ")
			       TEXT("classic ColumnMain disagree; the leg is invalid."),
			       ColMismatch, ColChecked);
		}
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
			       TEXT("== host (malformed-on-gpu=%u; colverify checked=%u mism=%u; ")
			       TEXT("voxverify checked=%u mism=%u; ctverify checked=%u mism=%u; ")
			       TEXT("stampverify checked=%u mism=%u; packverify checked=%u mism=%u; ")
			       TEXT("claimverify checked=%u mism=%u; claimSet gpu=%u host=%lld)"),
			       ProofSeq, GpuConsumed, GpuFold, GpuTail, GpuBad, ColChecked, ColMismatch,
			       VoxChecked, VoxMismatch, CtChecked, CtMismatch, StampChecked, StampMismatch,
			       PackChecked, PackMismatch, ClaimChecked, ClaimMismatch,
			       ClaimEligible, CumClaimStaged);
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
		 SliceBudgetRecords, bRequestProof, RequestSeq = ProofSeq,
		 // Column stage: plain values latched on the game thread. The atlas
		 // pointer is process-lifetime (FVoxelWorldImpl owns it), the same
		 // lifetime argument every region graph already leans on.
		 bColumns = bColumnStageArmed, bVoxelize = IsVoxelizeStageArmed(),
		 bClassify = IsClassifyStageArmed(), bStamp = IsAssetStampStageArmed(),
		 bPack = IsPackStageArmed(),
		 bClaim = IsClaimStageArmed(),
		 bClaimVerify = IsClaimStageArmed() && bClaimVerifyArmed,
		 ClaimBinder = ClaimPoolBinder,
		 // P2: the per-tick arm latch and the PREVIOUS window's claim stash,
		 // both plain values -- see their staging above for the contract.
		 bAsyncGen, HeadClaim,
		 StampInstances = MoveTemp(FlushInstances),
		 StampColStarts = MoveTemp(FlushColStarts),
		 StampSpans = MoveTemp(FlushSpans),
		 Atlas = ColumnAtlas,
		 ColSeedLo = ColumnSeedLo, ColSeedHi = ColumnSeedHi,
		 // mutable (P2): the stamp blob arrays are MOVED into the pending-gen
		 // stash on the async path instead of being uploaded here.
		 ColPixelSizeMm = ColumnPixelSizeMm](FRHICommandListImmediate& RHICmdList) mutable
	{
		// Land any outstanding proof copy BEFORE building this tick's graph:
		// Lock/Unlock want a quiescent readback, and the values must be
		// published before the game thread can see the sequence move.
		if (bProofCopyInFlight && ProofReadback != nullptr && ProofReadback->IsReady())
		{
			const uint32* Data = static_cast<const uint32*>(ProofReadback->Lock(kStatsDwords * sizeof(uint32)));
			if (Data != nullptr)
			{
				GProofGpuConsumed.store(Data[0], std::memory_order_relaxed);
				GProofGpuFold.store(Data[1], std::memory_order_relaxed);
				GProofGpuBad.store(Data[2], std::memory_order_relaxed);
				GProofGpuTail.store(Data[3], std::memory_order_relaxed);
				GProofColMismatch.store(Data[4], std::memory_order_relaxed);
				GProofColChecked.store(Data[5], std::memory_order_relaxed);
				GProofVoxMismatch.store(Data[6], std::memory_order_relaxed);
				GProofVoxChecked.store(Data[7], std::memory_order_relaxed);
				GProofCtMismatch.store(Data[8], std::memory_order_relaxed);
				GProofCtChecked.store(Data[9], std::memory_order_relaxed);
				GProofStampMismatch.store(Data[10], std::memory_order_relaxed);
				GProofStampChecked.store(Data[11], std::memory_order_relaxed);
				GProofPackMismatch.store(Data[12], std::memory_order_relaxed);
				GProofPackChecked.store(Data[13], std::memory_order_relaxed);
				GProofClaimMismatch.store(Data[14], std::memory_order_relaxed);
				GProofClaimChecked.store(Data[15], std::memory_order_relaxed);
				// [16..19] WERE ALLOCATED, WRITTEN AND COPIED, AND THEN NEVER
				// READ. The stats buffer is kStatsDwords = 20 dwords, the copy
				// pass copies all 20, and Lock() maps all 20 -- but this unpack
				// stopped at Data[15]. GProofClaimEligible therefore never left
				// its {0} initialiser, and `wlclaim gpuClaimed` printed a hard
				// 0 on every leg the claim stage has ever run.
				//
				// That zero was read as "CLAIM STAGE DARK: hostStaged>0,
				// gpuClaimed=0 -- those chunks' slots land nothing", which is
				// the manager's own documented failing reading, and it was
				// believed twice: it struck two legs as invalid and sent a
				// session's worth of localisation after a kernel that may have
				// been healthy the whole time. The kernel writes
				// WorklistStats[16] unconditionally and faithfully; nothing on
				// this side ever looked at it.
				//
				// THE SHAPE, because it is the one this file already warns
				// about in three other places: a counter whose producer is
				// correct, whose transport is correct, and whose CONSUMER is
				// missing, reads as the most alarming value in its range. It
				// cannot be caught by checking the producer -- which is what
				// every elimination pass here did.
				GProofClaimEligible.store(Data[16], std::memory_order_relaxed);
				// [17..19]: the claim verify's WITNESS -- categories seen, the
				// first failing record's ChunkSlot (+1), and its packed
				// record-index/category. Read here and nowhere else, and read
				// UNCONDITIONALLY: the last three dwords of this buffer were
				// allocated, written and copied and then not read for a whole
				// session, which is the defect this very line replaces.
				GProofClaimWitnessCat.store(Data[17], std::memory_order_relaxed);
				GProofClaimWitnessSlot.store(Data[18], std::memory_order_relaxed);
				GProofClaimWitnessRec.store(Data[19], std::memory_order_relaxed);
				GProofLandedSeq.store(ProofCopySeq, std::memory_order_release);
			}
			ProofReadback->Unlock();
			bProofCopyInFlight = false;
		}
		// ON THE RHI COMMAND LIST, NOT THE GRAPH, AND THAT IS FORCED. An
		// RDG_EVENT_SCOPE_STAT here asserts at FRDGBuilder::Execute --
		// RenderGraphBuilder.cpp:1770 checks the graph's breadcrumb is back at
		// Sentinel -- because these builders Execute inside the scope rather than
		// after it. Measured: it crashed the first leg at VoxelRasterAtlasGpu.
		// RHI_BREADCRUMB_EVENT_STAT is the same stat on the RHI timeline, feeds
		// the same GPU/<name> CSV column, and outlives the graph by construction
		// (declared before it, destroyed after it).
		RHI_BREADCRUMB_EVENT_STAT(RHICmdList, VoxelStreamWorklist, "VoxelStreamWorklist");
		FRDGBuilder GraphBuilder(RHICmdList);
		FRDGBufferRef Records = GraphBuilder.RegisterExternalBuffer(PooledRecords, TEXT("Voxel.WorklistRecords"));
		FRDGBufferRef Args = GraphBuilder.RegisterExternalBuffer(PooledArgs, TEXT("Voxel.WorklistArgs"));
		FRDGBufferRef Control = GraphBuilder.RegisterExternalBuffer(PooledControl, TEXT("Voxel.WorklistControl"));

		// --- P2: PUBLISH WINDOW N-1 AT THE HEAD, before anything below ------
		//
		// PASS-ADD ORDER IS THE WHOLE CORRECTNESS ARGUMENT, stated once here:
		// same-pipe RDG passes execute in add order, so adding the deferred
		// claim BEFORE this flush's ring-upload copies and args pass means it
		// reads window N-1 through the ring slots, Control cursor and args
		// triples EXACTLY as flush N-1 left them -- the copies/args that
		// overwrite them for window N are later passes. The same ordering
		// closes the ring-wrap hazard (an upload landing in a just-consumed
		// slot cannot outrun a claim added before it) and the arena-reuse
		// hazard (window N's generation, wherever it runs, is ordered after
		// these reads by RDG's WAR tracking -- and it runs on the async pipe,
		// so it is the ASYNC side that briefly waits, never graphics).
		//
		// The serial fallback: if NO scene rendered between the two flushes,
		// the stashed generation window was never dispatched -- its arenas
		// hold stale bytes and the claim below would publish garbage. Build
		// it HERE instead, graphics pipe, before the claim: one window of the
		// shipped serialization, counted, correctness independent of a frame
		// ever being rendered. A still-valid stash WITHOUT a head claim is
		// left alone -- it is a claim-less window (nothing will ever read its
		// slices) and the storage site below drops and counts it.
		if (HeadClaim.bValid)
		{
			if (PendingGen.bValid)
			{
				GAsyncGenSerialFallbacks.fetch_add(1, std::memory_order_relaxed);
				static bool bSerialFallbackLogged = false;
				if (!bSerialFallbackLogged)
				{
					bSerialFallbackLogged = true;
					UE_LOG(LogVoxelGpuWorklist, Warning,
					       TEXT("[gpu-worklist] asyncGen SERIAL FALLBACK: no scene render consumed ")
					       TEXT("the deferred generation window, so it is being built on the ")
					       TEXT("graphics pipe ahead of its claim. Correct, but it buys no ")
					       TEXT("overlap; frequent fallbacks mean frames are not rendering. ")
					       TEXT("Logged once; the count is on the wlclaim line."));
				}
				FPendingGenWindow FallbackWindow = MoveTemp(PendingGen);
				PendingGen = FPendingGenWindow{};
				RenderThread_AddGenStages(GraphBuilder, MoveTemp(FallbackWindow),
				                          /*bAsyncCompute*/ false);
			}
			RenderThread_AddClaimPasses(GraphBuilder, RHICmdList, ClaimBinder,
			                            HeadClaim.BudgetRecords, HeadClaim.bVerify);
		}

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
		// Parallel to the groups table: which dimension carries the record.
		const TArray<uint32> RecordInYTable(kRecordInY, int32(uint8(EVoxelWorklistStage::COUNT)));
		FRDGBufferRef RecordInYBuf = CreateStructuredBuffer(
			GraphBuilder, TEXT("Voxel.WorklistRecordInY"), sizeof(uint32),
			RecordInYTable.Num(), RecordInYTable.GetData(),
			RecordInYTable.Num() * sizeof(uint32));

		FVoxelWorklistArgsCS::FParameters* Params =
			GraphBuilder.AllocParameters<FVoxelWorklistArgsCS::FParameters>();
		Params->WorklistControl = GraphBuilder.CreateUAV(Control);
		Params->WorklistArgs = GraphBuilder.CreateUAV(Args, PF_R32_UINT);
		Params->GroupsPerRecord = GraphBuilder.CreateSRV(GroupsBuf);
		Params->RecordInY = GraphBuilder.CreateSRV(RecordInYBuf);
		Params->HeadCursor = NewHead;
		Params->SliceBudget = SliceBudgetRecords;
		Params->StageCount = uint32(EVoxelWorklistStage::COUNT);
		TShaderMapRef<FVoxelWorklistArgsCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.WorklistArgs"), Shader, Params,
			FIntVector(1, 1, 1));

		// --- the CONVERTED generation stages (P3), or their P2 stash --------
		//
		// Sync (voxel.GPU.AsyncGen 0 -- the shipped shape): the stage
		// dispatches are added HERE via RenderThread_AddGenStages, graphics
		// pipe, claim included -- pass for pass the pre-P2 graph (the
		// factoring moved the text, not the passes). Async: the window is
		// STASHED for the scene builder (the manager's post-opaque delegate
		// -> RenderThread_AddPendingGenToSceneGraph) and the claim was
		// stashed game-side for the NEXT flush's head; this graph then
		// carries only the head publication, the uploads, the args pass and
		// the prover -- all graphics, none of it consuming an async output,
		// which is what keeps this builder's epilogue join vacuous.
		if (bColumns && Atlas != nullptr)
		{
			FPendingGenWindow Window;
			Window.bValid = true;
			Window.SliceBudgetRecords = SliceBudgetRecords;
			Window.bVoxelize = bVoxelize;
			Window.bClassify = bClassify;
			Window.bStamp = bStamp;
			Window.bPack = bPack;
			Window.Atlas = Atlas;
			Window.SeedLo = ColSeedLo;
			Window.SeedHi = ColSeedHi;
			Window.PixelSizeMm = ColPixelSizeMm;
			Window.StampInstances = MoveTemp(StampInstances);
			Window.StampColStarts = MoveTemp(StampColStarts);
			Window.StampSpans = MoveTemp(StampSpans);
			if (bAsyncGen)
			{
				// A still-valid stash here can only be a CLAIM-LESS window:
				// a claim-carrying one was serial-built at the head of this
				// very graph (the head runs first), and under AsyncGen the
				// manager converts ONLY claim-fed jobs, so nothing -- not
				// the batch graph, not a claim -- ever reads a claim-less
				// window's arena slices. Overwriting it abandons bytes with
				// no readers; counted, never silent.
				if (PendingGen.bValid)
				{
					GAsyncGenWindowsDropped.fetch_add(1, std::memory_order_relaxed);
				}
				PendingGen = MoveTemp(Window);
			}
			else
			{
				RenderThread_AddGenStages(GraphBuilder, MoveTemp(Window),
				                          /*bAsyncCompute*/ false);
				// The same-tick claim, exactly the pre-P2 nesting: the claim
				// sources from the pack arenas and sizes from totals, so the
				// full chain must be armed -- restated by condition what the
				// old nesting stated by structure.
				if (bVoxelize && bClassify && bPack && bClaim)
				{
					RenderThread_AddClaimPasses(GraphBuilder, RHICmdList, ClaimBinder,
					                            FMath::Max(SliceBudgetRecords, 1u),
					                            bClaimVerify);
				}
			}
		}

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
				AddEnqueueCopyPass(GraphBuilder, ProofReadback, Stats, kStatsDwords * uint32(sizeof(uint32)));
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


// --- P2 (voxel.GPU.AsyncGen): the shared generation-window builder ----------
//
// LIFTED VERBATIM from Flush's render lambda (2026-09-03) so the sync path,
// the scene-builder async path and the no-scene serial fallback build ONE set
// of passes -- the factoring rule the region graph already lives by ("two
// graphs that agree today and drift the first time a pass is added"). The
// only deltas from the lifted text: the captured locals became Window fields,
// the ring/args/control are (idempotently) registered here, and every
// dispatch carries the caller's pipe choice.
//
// PIPE RULES, stated once: the five stages chain among themselves and read
// only ring/args/control (graphics-written, earlier), the atlas (graphics-
// written, earlier command lists) and each other's arenas. On the async pipe
// they therefore fork at the graph's prologue -- no in-graph graphics
// producer -- and nothing on the graphics pipe of the SAME graph consumes
// their outputs (the claim was deferred; the batch graph feeds only
// claim-fed jobs, which read nothing). The one graphics consumer is next
// tick's deferred claim, by which time the scene epilogue has already joined
// the async work and re-fenced the arenas to graphics: the claim pays no
// fence. That pair of facts -- fork at prologue, first consumer a tick later
// -- is the entire overlap argument.
void FVoxelGpuWorklist::RenderThread_AddGenStages(FRDGBuilder& GraphBuilder,
                                                  FPendingGenWindow&& InWindow,
                                                  bool bAsyncCompute)
{
	check(IsInRenderingThread());
	check(InWindow.bValid && InWindow.Atlas != nullptr);
	check(PooledRecords.IsValid() && PooledArgs.IsValid() && PooledControl.IsValid());

	// Idempotent per graph: the flush lambda registered these already on the
	// sync/fallback paths; in the scene builder this is the first time.
	FRDGBufferRef Records = GraphBuilder.RegisterExternalBuffer(PooledRecords, TEXT("Voxel.WorklistRecords"));
	FRDGBufferRef Args = GraphBuilder.RegisterExternalBuffer(PooledArgs, TEXT("Voxel.WorklistArgs"));
	FRDGBufferRef Control = GraphBuilder.RegisterExternalBuffer(PooledControl, TEXT("Voxel.WorklistControl"));

	// --- the CONVERTED Column stage: ONE indirect dispatch per tick --------
	//
	// Group count = Take * 16 off the triple the args pass wrote; the CPU
	// never sees it. Every consumed record's 1,024 columns land in the
	// persistent arena at slice (mono - consumeFirst) * 1024. The dispatch
	// is recorded even at Take == 0 (zero groups): constant pass count per
	// tick is the property being bought.
	if (!PooledColumnArena.IsValid())
	{
		// Lazy, in the first armed dispatch's own render-thread code path:
		// AllocatePooledBuffer, NOT an RDG transient, because later graphs
		// read it -- and NOT QueueBufferExtraction from a graph, because RDG
		// refuses to extract what no pass wrote. Sized to the latched slice
		// budget: Take <= SliceBudget by construction.
		ColumnArenaRecords = FMath::Max(InWindow.SliceBudgetRecords, 1u);
		PooledColumnArena = AllocatePooledBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVoxelGpuColumnSample),
			                                     ColumnArenaRecords * kColumnsPerRecord),
			TEXT("Voxel.WorklistColumnArena"));
		UE_LOG(LogVoxelGpuWorklist, Log,
		       TEXT("[gpu-worklist] column arena created: %u slices x %u columns ")
		       TEXT("(%.1f MiB); Column stage dispatching indirect from this flush on."),
		       ColumnArenaRecords, kColumnsPerRecord,
		       double(ColumnArenaRecords * kColumnsPerRecord *
		              sizeof(FVoxelGpuColumnSample)) / (1024.0 * 1024.0));
	}
	FRDGBufferRef Arena = GraphBuilder.RegisterExternalBuffer(
		PooledColumnArena, TEXT("Voxel.WorklistColumnArena"));

	VoxelGpuWorldGen::FWorklistColumnDispatch Dispatch;
	Dispatch.Records = Records;
	Dispatch.Control = Control;
	Dispatch.IndirectArgs = Args;
	Dispatch.IndirectArgsOffset =
		uint32(EVoxelWorklistStage::Column) * 3u * uint32(sizeof(uint32));
	Dispatch.RingCapacity = Capacity;
	Dispatch.ColumnArena = Arena;
	Dispatch.Atlas = InWindow.Atlas;
	Dispatch.SeedLo = InWindow.SeedLo;
	Dispatch.SeedHi = InWindow.SeedHi;
	Dispatch.PixelSizeMm = InWindow.PixelSizeMm;
	Dispatch.bAsyncCompute = bAsyncCompute;
	VoxelGpuWorldGen::AddWorklistColumnPass(GraphBuilder, Dispatch);

	// --- the CONVERTED Voxelize stage: SECOND indirect dispatch ------------
	//
	// Reads the column arena the pass above just wrote (RDG orders the two by
	// that dependency), writes the cell arena. Asset records early-out inside
	// the kernel (group-uniform) and keep their classic Voxelize + AssetStamp.
	if (InWindow.bVoxelize)
	{
		if (!PooledCellArena.IsValid())
		{
			// Lazy, AllocatePooledBuffer, batch-graph-readable: the column
			// arena's reasons verbatim. Sized to the CLAMPED budget -- the
			// clamp at the top of Flush is what makes "Take fits the arena"
			// an identity, not a hope.
			CellArenaRecords = FMath::Max(InWindow.SliceBudgetRecords, 1u);
			PooledCellArena = AllocatePooledBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32),
				                                     CellArenaRecords * kCellsPerRecord),
				TEXT("Voxel.WorklistCellArena"));
			UE_LOG(LogVoxelGpuWorklist, Log,
			       TEXT("[gpu-worklist] cell arena created: %u slices x %u cells ")
			       TEXT("(%.1f MiB); Voxelize stage dispatching indirect from this ")
			       TEXT("flush on (per-flush consume clamped to %u records)."),
			       CellArenaRecords, kCellsPerRecord,
			       double(CellArenaRecords) * double(kCellsPerRecord) *
			       double(sizeof(uint32)) / (1024.0 * 1024.0),
			       CellArenaRecords);
		}
		FRDGBufferRef CellArena = GraphBuilder.RegisterExternalBuffer(
			PooledCellArena, TEXT("Voxel.WorklistCellArena"));

		VoxelGpuWorldGen::FWorklistVoxelizeDispatch VDispatch;
		VDispatch.Records = Records;
		VDispatch.Control = Control;
		VDispatch.IndirectArgs = Args;
		VDispatch.IndirectArgsOffset =
			uint32(EVoxelWorklistStage::Voxelize) * 3u * uint32(sizeof(uint32));
		VDispatch.RingCapacity = Capacity;
		VDispatch.ColumnArena = Arena;
		VDispatch.CellArena = CellArena;
		VDispatch.Atlas = InWindow.Atlas;
		VDispatch.SeedLo = InWindow.SeedLo;
		VDispatch.SeedHi = InWindow.SeedHi;
		VDispatch.PixelSizeMm = InWindow.PixelSizeMm;
		VDispatch.bAsyncCompute = bAsyncCompute;
		VoxelGpuWorldGen::AddWorklistVoxelizePass(GraphBuilder, VDispatch);

		// --- AssetStamp gather: between voxelize and classify ---------------
		//
		// Stamps every stamps-staged record's instances into its cell arena
		// slice, in slice order inside one thread -- the order-preserving
		// form. BETWEEN the two dispatches because classify must see the
		// stamped cells. The blob buffers are transient to whichever graph
		// finally dispatches this window -- which is exactly why the ARRAYS
		// ride FPendingGenWindow instead of being uploaded at flush time.
		// With an empty blob every record early-outs on bit 9 and the dummy
		// one-element buffers below are never read.
		if (InWindow.bStamp)
		{
			const auto MakeBlobBuffer = [&](const TCHAR* Name, const void* Data,
			                                uint32 Elements, uint32 Stride)
			{
				// A zero-element structured buffer is not creatable; a
				// one-element dummy is never read (bit 9 gates).
				static const uint32 Zero[12] = { 0 };
				const bool bEmpty = Elements == 0;
				return CreateStructuredBuffer(
					GraphBuilder, Name, Stride,
					bEmpty ? 1 : int32(Elements),
					bEmpty ? Zero : Data,
					(bEmpty ? 1 : Elements) * Stride);
			};
			VoxelGpuWorldGen::FWorklistAssetStampDispatch SDispatch;
			SDispatch.Records = Records;
			SDispatch.Control = Control;
			SDispatch.IndirectArgs = Args;
			SDispatch.IndirectArgsOffset =
				uint32(EVoxelWorklistStage::AssetStamp) * 3u * uint32(sizeof(uint32));
			SDispatch.RingCapacity = Capacity;
			SDispatch.CellArena = CellArena;
			SDispatch.Instances = MakeBlobBuffer(
				TEXT("Voxel.WorklistAssetInstances"), InWindow.StampInstances.GetData(),
				uint32(InWindow.StampInstances.Num()), sizeof(FVoxelWorklistAssetInstance));
			SDispatch.ColStarts = MakeBlobBuffer(
				TEXT("Voxel.WorklistAssetColStarts"), InWindow.StampColStarts.GetData(),
				uint32(InWindow.StampColStarts.Num()), sizeof(uint32));
			SDispatch.Spans = MakeBlobBuffer(
				TEXT("Voxel.WorklistAssetSpans"), InWindow.StampSpans.GetData(),
				uint32(InWindow.StampSpans.Num()), sizeof(uint32));
			SDispatch.bAsyncCompute = bAsyncCompute;
			VoxelGpuWorldGen::AddWorklistAssetStampPass(GraphBuilder, SDispatch);
		}

		// --- fused ClassifyTotals: THIRD and FOURTH indirect ----------------
		//
		// Reads the cell arena the voxelize pass just filled; writes the five
		// small arenas the claim (and, sync only, the batch graph) read
		// through their read bases. Nested under bVoxelize because the cell
		// arena is this stage's input -- IsClassifyStageArmed() already
		// implies it, restated by structure.
		if (InWindow.bClassify)
		{
			if (!PooledOccCountsArena.IsValid())
			{
				ClassifyArenaRecords = FMath::Max(InWindow.SliceBudgetRecords, 1u);
				const auto MakeArena = [&](uint32 DwordsPerRecord, const TCHAR* Name)
				{
					return AllocatePooledBuffer(
						FRDGBufferDesc::CreateStructuredDesc(
							sizeof(uint32), ClassifyArenaRecords * DwordsPerRecord),
						Name);
				};
				PooledOccCountsArena = MakeArena(kBricksPerRecord, TEXT("Voxel.WorklistOccCounts"));
				PooledMatCountsArena = MakeArena(kBricksPerRecord, TEXT("Voxel.WorklistMatCounts"));
				PooledOccOffsetsArena = MakeArena(kBricksPerRecord, TEXT("Voxel.WorklistOccOffsets"));
				PooledMatOffsetsArena = MakeArena(kBricksPerRecord, TEXT("Voxel.WorklistMatOffsets"));
				PooledTotalsArena = MakeArena(2, TEXT("Voxel.WorklistTotals"));
				UE_LOG(LogVoxelGpuWorklist, Log,
				       TEXT("[gpu-worklist] classify arenas created: %u slices x ")
				       TEXT("(4 x %u + 2) dwords (%.1f KiB); fused ClassifyTotals ")
				       TEXT("dispatching indirect from this flush on."),
				       ClassifyArenaRecords, kBricksPerRecord,
				       double(ClassifyArenaRecords) * double(4 * kBricksPerRecord + 2)
				       * double(sizeof(uint32)) / 1024.0);
			}
			VoxelGpuWorldGen::FWorklistClassifyDispatch CDispatch;
			CDispatch.Records = Records;
			CDispatch.Control = Control;
			CDispatch.IndirectArgs = Args;
			CDispatch.ClassifyArgsOffset =
				uint32(EVoxelWorklistStage::Classify) * 3u * uint32(sizeof(uint32));
			CDispatch.TotalsArgsOffset =
				uint32(EVoxelWorklistStage::ClassifyTotals) * 3u * uint32(sizeof(uint32));
			CDispatch.RingCapacity = Capacity;
			CDispatch.CellArena = CellArena;
			CDispatch.OccCounts = GraphBuilder.RegisterExternalBuffer(
				PooledOccCountsArena, TEXT("Voxel.WorklistOccCounts"));
			CDispatch.MatCounts = GraphBuilder.RegisterExternalBuffer(
				PooledMatCountsArena, TEXT("Voxel.WorklistMatCounts"));
			CDispatch.OccOffsets = GraphBuilder.RegisterExternalBuffer(
				PooledOccOffsetsArena, TEXT("Voxel.WorklistOccOffsets"));
			CDispatch.MatOffsets = GraphBuilder.RegisterExternalBuffer(
				PooledMatOffsetsArena, TEXT("Voxel.WorklistMatOffsets"));
			CDispatch.Totals = GraphBuilder.RegisterExternalBuffer(
				PooledTotalsArena, TEXT("Voxel.WorklistTotals"));
			CDispatch.bAsyncCompute = bAsyncCompute;
			VoxelGpuWorldGen::AddWorklistClassifyPasses(GraphBuilder, CDispatch);

			// --- Pack: FIFTH indirect (+ the mask-arena clear) --------------
			//
			// Reads the cell arena and the classify stage's offset arenas;
			// writes the pack arenas the claim consumes through its read
			// bases. The chunk-mask arena is InterlockedOr-accumulated, so it
			// is CLEARED here first, every window -- the host precondition
			// brickpack.ush states. The clear rides the SAME pipe as the pack
			// (async when async): a graphics clear inside the scene builder
			// would become an in-graph producer and drag the async fork from
			// the prologue to the clear's position.
			if (InWindow.bPack)
			{
				if (!PooledPackDescArena.IsValid())
				{
					PackArenaRecords = FMath::Max(InWindow.SliceBudgetRecords, 1u);
					const auto MakePackArena =
						[&](uint32 BytesPerElement, uint32 ElementsPerRecord, const TCHAR* Name)
					{
						return AllocatePooledBuffer(
							FRDGBufferDesc::CreateStructuredDesc(
								BytesPerElement, PackArenaRecords * ElementsPerRecord),
							Name);
					};
					PooledPackDescArena = MakePackArena(sizeof(uint32) * 2, kBricksPerRecord,
					                                    TEXT("Voxel.WorklistPackDesc"));
					PooledPackOccArena = MakePackArena(sizeof(uint32), kOccWordsPerRecord,
					                                   TEXT("Voxel.WorklistPackOcc"));
					PooledPackMatArena = MakePackArena(sizeof(uint32), kMatWordsPerRecord,
					                                   TEXT("Voxel.WorklistPackMat"));
					PooledPackSkipArena = MakePackArena(sizeof(uint32), kSkipWordsPerRecord,
					                                    TEXT("Voxel.WorklistPackSkip"));
					PooledPackMaskArena = MakePackArena(sizeof(uint32), kMaskDwordsPerRecord,
					                                    TEXT("Voxel.WorklistPackMask"));
					UE_LOG(LogVoxelGpuWorklist, Log,
					       TEXT("[gpu-worklist] pack arenas created: %u slices ")
					       TEXT("(desc 64x8B, occ %u, mat %u, skip %u, mask %u dwords ")
					       TEXT("per record; %.1f MiB total); Pack stage dispatching ")
					       TEXT("indirect from this flush on."),
					       PackArenaRecords, kOccWordsPerRecord, kMatWordsPerRecord,
					       kSkipWordsPerRecord, kMaskDwordsPerRecord,
					       double(PackArenaRecords) *
					       double(kBricksPerRecord * 2 + kOccWordsPerRecord +
					              kMatWordsPerRecord + kSkipWordsPerRecord +
					              kMaskDwordsPerRecord) * 4.0 / (1024.0 * 1024.0));
				}
				FRDGBufferRef PackDesc = GraphBuilder.RegisterExternalBuffer(
					PooledPackDescArena, TEXT("Voxel.WorklistPackDesc"));
				FRDGBufferRef PackOcc = GraphBuilder.RegisterExternalBuffer(
					PooledPackOccArena, TEXT("Voxel.WorklistPackOcc"));
				FRDGBufferRef PackMat = GraphBuilder.RegisterExternalBuffer(
					PooledPackMatArena, TEXT("Voxel.WorklistPackMat"));
				FRDGBufferRef PackSkip = GraphBuilder.RegisterExternalBuffer(
					PooledPackSkipArena, TEXT("Voxel.WorklistPackSkip"));
				FRDGBufferRef PackMask = GraphBuilder.RegisterExternalBuffer(
					PooledPackMaskArena, TEXT("Voxel.WorklistPackMask"));
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(PackMask), 0u,
				                bAsyncCompute ? ERDGPassFlags::AsyncCompute
				                              : ERDGPassFlags::Compute);

				VoxelGpuWorldGen::FWorklistPackDispatch PDispatch;
				PDispatch.Records = Records;
				PDispatch.Control = Control;
				PDispatch.IndirectArgs = Args;
				PDispatch.IndirectArgsOffset =
					uint32(EVoxelWorklistStage::PackClaim) * 3u * uint32(sizeof(uint32));
				PDispatch.RingCapacity = Capacity;
				PDispatch.CellArena = CellArena;
				PDispatch.OccOffsets = CDispatch.OccOffsets;
				PDispatch.MatOffsets = CDispatch.MatOffsets;
				PDispatch.Desc = PackDesc;
				PDispatch.Occ = PackOcc;
				PDispatch.Mat = PackMat;
				PDispatch.Skip = PackSkip;
				PDispatch.Mask = PackMask;
				PDispatch.bAsyncCompute = bAsyncCompute;
				VoxelGpuWorldGen::AddWorklistPackPass(GraphBuilder, PDispatch);

				// The Claim stage does NOT live here: publication is graphics
				// work, and under voxel.GPU.AsyncGen it runs one flush later.
				// Both callers add it through RenderThread_AddClaimPasses.
			}
		}
	}
}

// --- P2: the shared Claim-stage builder -------------------------------------
//
// Registers everything the claim dispatches need from the pooled members and
// adds them to the given graph. Two callers, one window apart: the sync path
// adds it right after the generation stages (the shipped, pre-P2 pass order),
// and the deferred head adds it at the TOP of the next flush's graph, where
// ring/control/args still hold the PREVIOUS window's state -- which is the
// entire deferral mechanism (see the head block in Flush).
void FVoxelGpuWorklist::RenderThread_AddClaimPasses(FRDGBuilder& GraphBuilder,
                                                    FRHICommandListImmediate& RHICmdList,
                                                    const FPoolBinder& Binder,
                                                    uint32 BudgetRecords, bool bVerify)
{
	check(IsInRenderingThread());
	check(BudgetRecords > 0);
	// The pack arenas + the totals arena are the claim's sources. They are
	// created by the first pack-armed generation dispatch, and a claim can
	// only be staged after such a window ran -- so absence here is a real
	// wiring defect, never a race. Loud, not fatal: the affected records'
	// slots stay UNWRITTEN (missing chunks, the P1 xcheck's `unwritten`
	// reading, and the marcher's record validation reads a zero record as
	// absent), never corrupted.
	if (!PooledPackDescArena.IsValid() || !PooledPackOccArena.IsValid() ||
	    !PooledPackMatArena.IsValid() || !PooledPackMaskArena.IsValid() ||
	    !PooledTotalsArena.IsValid() || !PooledStats.IsValid())
	{
		static bool bClaimSourcesMissingLogged = false;
		if (!bClaimSourcesMissingLogged)
		{
			bClaimSourcesMissingLogged = true;
			UE_LOG(LogVoxelGpuWorklist, Error,
			       TEXT("[gpu-worklist] CLAIM SOURCES MISSING: a claim dispatch was requested ")
			       TEXT("before any pack-armed generation window created the arenas. The ")
			       TEXT("staged records land nothing (unwritten slots, missing chunks). ")
			       TEXT("Logged once."));
		}
		return;
	}
	FPoolBindings PoolB;
	const bool bBound = Binder
		&& Binder(GraphBuilder, RHICmdList, PoolB)
		&& PoolB.IsValid();
	if (!bBound)
	{
		static bool bBinderFailLogged = false;
		if (!bBinderFailLogged)
		{
			bBinderFailLogged = true;
			UE_LOG(LogVoxelGpuWorklist, Error,
			       TEXT("[gpu-worklist] CLAIM STAGE DARK: the pool binder ")
			       TEXT("failed on an armed flush. Claim-staged records land ")
			       TEXT("nothing (unwritten slots, missing chunks); the ")
			       TEXT("classic paths are unaffected. Logged once."));
		}
		return;
	}

	FRDGBufferRef Records = GraphBuilder.RegisterExternalBuffer(PooledRecords, TEXT("Voxel.WorklistRecords"));
	FRDGBufferRef Args = GraphBuilder.RegisterExternalBuffer(PooledArgs, TEXT("Voxel.WorklistArgs"));
	FRDGBufferRef Control = GraphBuilder.RegisterExternalBuffer(PooledControl, TEXT("Voxel.WorklistControl"));

	VoxelGpuWorldGen::FWorklistClaimDispatch KDispatch;
	KDispatch.Records = Records;
	KDispatch.Control = Control;
	KDispatch.IndirectArgs = Args;
	KDispatch.RecordArgsOffset =
		uint32(EVoxelWorklistStage::Record) * 3u * uint32(sizeof(uint32));
	KDispatch.WriteArgsOffset =
		uint32(EVoxelWorklistStage::Write) * 3u * uint32(sizeof(uint32));
	KDispatch.RingCapacity = Capacity;
	KDispatch.ClaimBudgetRecords = BudgetRecords;
	KDispatch.Totals = GraphBuilder.RegisterExternalBuffer(
		PooledTotalsArena, TEXT("Voxel.WorklistTotals"));
	KDispatch.PackDesc = GraphBuilder.RegisterExternalBuffer(
		PooledPackDescArena, TEXT("Voxel.WorklistPackDesc"));
	KDispatch.PackOcc = GraphBuilder.RegisterExternalBuffer(
		PooledPackOccArena, TEXT("Voxel.WorklistPackOcc"));
	KDispatch.PackMat = GraphBuilder.RegisterExternalBuffer(
		PooledPackMatArena, TEXT("Voxel.WorklistPackMat"));
	KDispatch.PackMask = GraphBuilder.RegisterExternalBuffer(
		PooledPackMaskArena, TEXT("Voxel.WorklistPackMask"));
	KDispatch.Pool.PoolDesc = PoolB.PoolDesc;
	KDispatch.Pool.PoolOcc = PoolB.PoolOcc;
	KDispatch.Pool.PoolMat = PoolB.PoolMat;
	KDispatch.Pool.PoolTable = PoolB.PoolTable;
	KDispatch.Pool.AllocState = PoolB.AllocState;
	KDispatch.Pool.AllocBitmap = PoolB.AllocBitmap;
	KDispatch.Pool.AllocSide = PoolB.AllocSide;
	KDispatch.PoolLayout = PoolB.Layout;
	KDispatch.ChunkRecordDwords = PoolB.ChunkRecordDwords;
	KDispatch.bVerify = bVerify;
	// Unconditional: the claim kernel writes its eligible-record count to
	// stats[16] on every armed tick. That counter is the one that would have
	// caught the double claim on the first leg instead of the fourth.
	KDispatch.VerifyStats = GraphBuilder.RegisterExternalBuffer(
		PooledStats, TEXT("Voxel.WorklistStats"));
	VoxelGpuWorldGen::AddWorklistClaimPasses(GraphBuilder, KDispatch);
}

// --- P2: the scene-builder entry point --------------------------------------
//
// Called by the manager's post-opaque render delegate with the SCENE
// renderer's own FRDGBuilder. THE BUILDER CHOICE IS THE WIN, not a
// convenience: a standalone builder's epilogue finalises every external
// buffer back to the graphics pipe (RenderGraphBuilder.cpp,
// AddLastBufferTransition), so async work dispatched from a flush command
// would be fence-sandwiched between scene N-1 and scene N -- serialized
// exactly as the shipped graphics-queue version, which is the null result
// this comment exists to prevent someone re-earning. Inside the scene
// builder the chain forks at the prologue (no in-graph graphics producer),
// overlaps the whole frame -- the marcher included -- and the scene
// epilogue's join waits only max(0, gen - scene).
//
// Render thread only, like every touch of PendingGen. Consumes the stash, so
// a second view family in the same frame (or a second scene of another
// world) finds it empty and adds nothing -- any scene builder is an
// acceptable host because the passes depend on no view state.
bool FVoxelGpuWorklist::RenderThread_AddPendingGenToSceneGraph(FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread());
	if (!PendingGen.bValid)
	{
		return false;
	}
	FPendingGenWindow TakenWindow = MoveTemp(PendingGen);
	PendingGen = FPendingGenWindow{};
	RenderThread_AddGenStages(GraphBuilder, MoveTemp(TakenWindow), /*bAsyncCompute*/ true);
	GAsyncGenSceneWindows.fetch_add(1, std::memory_order_relaxed);
	return true;
}

int64 FVoxelGpuWorklist::GetAsyncGenSceneWindows()
{
	return GAsyncGenSceneWindows.load(std::memory_order_relaxed);
}

int64 FVoxelGpuWorklist::GetAsyncGenSerialFallbacks()
{
	return GAsyncGenSerialFallbacks.load(std::memory_order_relaxed);
}

int64 FVoxelGpuWorklist::GetAsyncGenWindowsDropped()
{
	return GAsyncGenWindowsDropped.load(std::memory_order_relaxed);
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

FVoxelGpuWorklist::FColumnStageBindings FVoxelGpuWorklist::RegisterColumnStage(FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread());
	FColumnStageBindings Out;
	// Null until the first ARMED flush executed on this thread. Render
	// commands run in enqueue order and the flush that fills the arena is
	// enqueued before any batch that consumes it, so a null here means the
	// stage genuinely never dispatched -- the caller's fallback-and-count
	// path, not a race to paper over.
	if (PooledColumnArena.IsValid())
	{
		Out.Arena = GraphBuilder.RegisterExternalBuffer(PooledColumnArena,
		                                                TEXT("Voxel.WorklistColumnArena"));
	}
	if (PooledCellArena.IsValid())
	{
		Out.CellArena = GraphBuilder.RegisterExternalBuffer(PooledCellArena,
		                                                    TEXT("Voxel.WorklistCellArena"));
	}
	if (PooledOccOffsetsArena.IsValid())
	{
		Out.OccOffsetsArena = GraphBuilder.RegisterExternalBuffer(
			PooledOccOffsetsArena, TEXT("Voxel.WorklistOccOffsets"));
		Out.MatOffsetsArena = GraphBuilder.RegisterExternalBuffer(
			PooledMatOffsetsArena, TEXT("Voxel.WorklistMatOffsets"));
		Out.TotalsArena = GraphBuilder.RegisterExternalBuffer(
			PooledTotalsArena, TEXT("Voxel.WorklistTotals"));
	}
	if (PooledPackDescArena.IsValid())
	{
		Out.PackDescArena = GraphBuilder.RegisterExternalBuffer(
			PooledPackDescArena, TEXT("Voxel.WorklistPackDesc"));
		Out.PackOccArena = GraphBuilder.RegisterExternalBuffer(
			PooledPackOccArena, TEXT("Voxel.WorklistPackOcc"));
		Out.PackMatArena = GraphBuilder.RegisterExternalBuffer(
			PooledPackMatArena, TEXT("Voxel.WorklistPackMat"));
		Out.PackMaskArena = GraphBuilder.RegisterExternalBuffer(
			PooledPackMaskArena, TEXT("Voxel.WorklistPackMask"));
	}
	if (PooledStats.IsValid())
	{
		Out.Stats = GraphBuilder.RegisterExternalBuffer(PooledStats,
		                                                TEXT("Voxel.WorklistStats"));
	}
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
