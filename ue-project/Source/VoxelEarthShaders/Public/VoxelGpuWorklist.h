#pragma once
// FVoxelGpuWorklist -- the persistent chunk-record ring for P3 (persistent
// worklist + indirect dispatch; docs/gpu-worklist-plan-2026-08-23.md).
//
// *** SPINE WIRED; KERNELS CONVERTED: Column, Voxelize, fused ClassifyTotals
// (2026-08-23). ***
// Behind -VoxelGpuWorklistClassify=1 (on top of the Voxelize switch) the
// flush graph ALSO dispatches the fused ClassifyTotals pair -- one group per
// BRICK classify off the cell arena, then a one-group-per-record in-group
// scan + totals -- and every cell-fed chunk skips its classic
// BrickClassifyMain, BOTH 3-pass scans and BrickTotalMain: EIGHT passes, the
// conversion's largest single cut (15 -> 7 on the lean-alloc shape).
// BrickPackMain reads offsets through brickpack.ush's BrickScanReadBase; the
// claim pass reads totals through VoxelBrickPoolAlloc.usf's TotalsReadBase.
// Byte gate: -VoxelGpuWorklistVerifyCT=1 into stats [8..9]. Four stages
// remain: AssetStamp, PackClaim, Write, Record.
//
// Behind -VoxelGpuWorklistVoxelize=1 (on top of the two switches below) the
// flush graph ALSO dispatches VoxelizeWorklistMain once per tick: every
// consumed asset-free record's 32,768 cells land in a flush-level CELL ARENA
// (128 KiB/record; consume clamped to -VoxelGpuWorklistCellBudget records per
// flush, default 256), the chunk's BrickClassify/BrickPack read the slice
// through brickpack.ush's CellReadBase, and the chunk's own VoxelizeMain is
// SKIPPED (16 -> 15 passes on the lean-alloc shape). Asset chunks fall back
// by design until the flush-level asset buffer lands. Byte gate:
// -VoxelGpuWorklistVerifyVox=1 into stats [6..7].
//
// The stage-1 banner, still accurate for the Column stage:
// Behind -VoxelGpuWorklistColumns=1 (on top of -VoxelGpuWorklist=1) the flush
// graph now also dispatches ColumnWorklistMain (VoxelWorklistColumn.usf) once
// per tick through the Column-stage indirect triple: every consumed record's
// 1,024 columns land in a flush-level COLUMN ARENA, and the same chunk's
// classic VoxelizeMain reads its slice through worldgen.ush's ColumnReadBase
// instead of running its own ColumnMain pass. One pass per TICK replaces one
// pass per CHUNK for this stage; the six remaining stages (Voxelize,
// AssetStamp, ClassifyTotals, PackClaim, Write, Record) still read $Globals
// per chunk, so passes/tick does not go FLAT yet -- it drops by ~1 x
// chunks/tick. Gate: the pinned digest (classic path byte-identical) plus
// ColumnWorklistVerifyMain (-VoxelGpuWorklistVerifyCols=1, converted bytes vs
// a classic dispatch of the same chunks, riding the proof readback).
//
// The banner below is the pre-conversion state of the OTHER stages and stays
// accurate for them.
// FVoxelGpuMeshJobManager now drives this ring behind -VoxelGpuWorklist=1:
// every lean-eligible brick chunk appends a real 64-byte record at dispatch,
// Flush runs once per tick (upload + args pass + the indirect spine prover,
// VoxelWorklistConsume.usf, whose group count comes off the GPU cursor), and
// a 16-byte proof readback every ~5 s compares GPU consumption against the
// host's no-readback mirror -- consumed count, record fold, tail cursor, all
// three exact or the leg is invalid. What is NOT yet true: the generation
// kernels still read per-region $Globals and are dispatched one pass set per
// chunk/stack, so PASS COUNT PER TICK DOES NOT FLATTEN YET. Converting them
// to read Records[recordIndex] through these args is the P3 work proper,
// kernel by kernel against this now-verified spine, sequenced AFTER the
// raster atlas (A) because the atlas is what empties the request of its
// variable-size half (the 46 KB window) -- without A there is no 64-byte
// record to put in a ring.
//
// WHY P3 EXISTS, BY ARITHMETIC RATHER THAN HOPE (the four-arm 2026-08-23
// sweep showed pass-count work moves nothing at ~2,100 chunks/s -- this is
// NOT tonight's bottleneck): a fused stack costs ~14 passes and carries ~8
// chunks (the [gpu-batch] line's own ~1078 passes / 77 stacks / 311 chunks).
// At 50,000 chunks/s and 60 ticks/s that is 104 stacks = ~1,460 passes per
// tick -- three times the ~500 passes/tick where this project has measured
// the hitch cliff. So P3 becomes necessary somewhere above ~17,000 chunks/s
// (500 passes / 14 per stack * 8 chunks * 60 ticks), and is pure overhead
// below it. Build the conversion when the atlas legs approach that number,
// and re-measure before believing it helps (the architecture doc's own rule).

#include "CoreMinimal.h"
#include "RenderGraphResources.h"

class FRDGBuilder;
class FRHIGPUBufferReadback;
class FVoxelRasterAtlasGpu;

// One chunk of pure-worldgen production, 64 bytes, mirrored bit-for-bit by
// VoxelWorklist.ush's GpuChunkWorkRecord. 16 dwords; the .cpp static_asserts
// the size and every offset. Everything a lean brick job needs that is not a
// process-wide constant (Seed, PixelSizeMm, SurfaceMip live in per-dispatch
// uniforms) and not carried by the raster atlas.
struct FVoxelGpuChunkWorkRecord
{
	int32 OriginVx = 0;          // halo-free chunk origin, level-L cell units
	int32 OriginVy = 0;
	int32 BrickZMin = 0;         // level-L brick z of the chunk base
	// CoarseLevel in bits 0..3 (0..6 live), RingSkirtMask in 4..7 (0 on the
	// brick path -- the marcher owns ring seams), bit 8 = has assets.
	uint32 LevelFlags = 0;
	uint32 ChunkSlot = 0;        // CPU-dispensed descriptor/record slot (P1 keeps slots on the CPU)
	uint32 GenId = 0;            // stale-drop identity, as the job manager's
	uint32 AssetBase = 0;        // first instance in the flush's asset buffer
	uint32 AssetCount = 0;
	uint32 ShadingClimatePacked = 0;   // FVoxelBrickChunkShading::Pack's three dwords
	uint32 ShadingGradPacked = 0;
	uint32 ShadingSurfaceZBits = 0;
	uint32 Reserved[5] = {0, 0, 0, 0, 0};
};
static_assert(sizeof(FVoxelGpuChunkWorkRecord) == 64, "the 64-byte record IS the contract");

// The stages a converted chain dispatches, one indirect dispatch each per
// tick regardless of record count. Group counts per record differ per stage;
// the args-setup kernel multiplies them in on the GPU.
enum class EVoxelWorklistStage : uint8
{
	Column = 0,     // 32x32 columns / 64 threads = 16 groups per record (LOCKED)
	Voxelize,       // one thread per COLUMN (cave/cavern are per-column), 16 groups (LOCKED)
	AssetStamp,     // per-column gather loop, 16 groups (order-preserving form)
	Classify,       // ONE GROUP PER BRICK (the classic classify shape --
	                // brickBuildOccupancy is groupshared), 64 groups (LOCKED)
	ClassifyTotals, // in-group exclusive scan of the 64 counts + totals, 1 group (LOCKED)
	PackClaim,      // pack + claim fused, 64 bricks / group pair
	Write,          // occ+mat word copies, groups from claim sizes (upper bound)
	Record,         // descriptor + chunk record + index cell, 1 group
	COUNT
};

class VOXELEARTHSHADERS_API FVoxelGpuWorklist
{
public:
	// --- stage shapes, the HOST side of the torn-dispatch lock --------------
	// A lean brick chunk is 32x32 columns; ColumnWorklistMain runs 64 threads
	// per group, so 16 groups per record. kGroupsPerRecord[Column] in the .cpp
	// args table is built from THIS constant, and the kernel #errors if the
	// define built from it disagrees with its own derivation -- the mechanism
	// the plan doc mandates per converted kernel.
	static constexpr uint32 kColumnsPerRecord = 1024;
	static constexpr uint32 kColumnGroupsPerRecord = kColumnsPerRecord / 64;
	// The Voxelize stage keeps the classic kernel's own mapping -- one thread
	// per COLUMN looping the 32 z cells, NOT one per cell: the cave and cavern
	// reductions are per-column and per-cell threads would recompute each 32
	// times. So its group count per record is the Column stage's, and its
	// arena slice is the lean chunk's 32x32x32 cells.
	static constexpr uint32 kCellsPerRecord = 32768;
	static constexpr uint32 kVoxelizeGroupsPerRecord = kColumnsPerRecord / 64;
	// The fused ClassifyTotals stage: one group per BRICK for the classify
	// half (brickBuildOccupancy is groupshared -- the classic shape is the
	// only shape that calls the same text), one group per RECORD for the
	// scan + totals half.
	static constexpr uint32 kBricksPerRecord = 64;
	static constexpr uint32 kClassifyGroupsPerRecord = kBricksPerRecord;
	static constexpr uint32 kClassifyTotalsGroupsPerRecord = 1;
	// Stats buffer: [0..3] the prover's evidence (VoxelWorklistConsume.usf),
	// [4..5] the column verify's mismatch/checked counters
	// (VoxelWorklistColumn.usf), [6..7] the voxelize verify's
	// (VoxelWorklistVoxelize.usf), [8..9] the classify-totals verify's
	// (VoxelWorklistClassify.usf), [10..11] reserved for later stages.
	static constexpr uint32 kStatsDwords = 12;

	~FVoxelGpuWorklist();

	// Capacity is a latch, not a growth policy: a full ring REFUSES appends
	// and counts them (the caller keeps the chunk on its pending queue), so
	// back-pressure is visible in a counter instead of hidden in a realloc.
	void Init(uint32 RecordCapacity);
	bool IsInitialized() const { return Capacity != 0; }

	// Game thread: stage records for this tick. Returns how many were
	// accepted (the rest refused-full and counted). OutMonotonicIndices, if
	// given, receives one entry PER INPUT record: its monotonic ring index
	// (the coordinate GetLastFlush()'s window is expressed in) or MAX_uint32
	// if it was refused -- this is how a caller learns which arena slice the
	// column stage will write for its chunk.
	int32 Append(TArrayView<const FVoxelGpuChunkWorkRecord> Records,
	             TArray<uint32>* OutMonotonicIndices = nullptr);

	// --- the Column stage (-VoxelGpuWorklistColumns) ------------------------
	//
	// Game thread, before Flush, idempotent: hands over the process-wide
	// generation inputs a column dispatch needs that no record carries (the
	// atlas the VXC_RASTER_ATLAS accessors sample, the world seed, the raster
	// pitch). First call arms the stage; Flush dispatches ColumnWorklistMain
	// only once armed, so a spine-only leg (-VoxelGpuWorklist=1 alone) never
	// reaches any of it and stays byte-identical to the measured spine.
	void SetColumnStageInputs(FVoxelRasterAtlasGpu* Atlas, uint64 Seed, int32 PixelSizeMm);
	bool IsColumnStageArmed() const { return bColumnStageArmed; }

	// --- the Voxelize stage (-VoxelGpuWorklistVoxelize) ---------------------
	//
	// Game thread, before Flush, idempotent. No inputs of its own: the
	// voxelize kernel reads the SAME process-wide inputs the column stage
	// carries (atlas, seed, pitch -- SetColumnStageInputs) plus the column
	// arena itself, which is why arming is a flag and a budget rather than a
	// second input set, and why IsVoxelizeStageArmed() is false until the
	// column stage is armed too. CellBudgetRecords caps how many records one
	// flush may consume while this stage is armed: the cell arena costs 128
	// KiB per slice (32,768 cells x 4 B), so the default budget of 1,024
	// records would be 128 MiB -- the plan doc says start smaller and latch
	// it up when refusedFull says so.
	void SetVoxelizeStageArmed(bool bArmed, uint32 CellBudgetRecords);
	bool IsVoxelizeStageArmed() const { return bVoxelizeStageArmed && bColumnStageArmed; }

	// --- the fused ClassifyTotals stage (-VoxelGpuWorklistClassify) ---------
	//
	// Same flag-not-inputs shape as the Voxelize arm, and gated BEHIND it:
	// the classify half reads the CELL arena, so without the Voxelize stage
	// there is nothing to classify and the stage stays dark (every chunk
	// falls back, counted). No budget of its own -- its five arenas total
	// ~1 KiB per record, three orders under the cell arena's clamp.
	void SetClassifyStageArmed(bool bArmed) { bClassifyStageArmed = bArmed; }
	bool IsClassifyStageArmed() const { return bClassifyStageArmed && IsVoxelizeStageArmed(); }

	// Game thread: the consume window the most recent Flush mirrored --
	// [ConsumeFirst, ConsumeFirst + Take) in monotonic record indices. A
	// record appended at monotonic index m was consumed by that flush iff
	// m - ConsumeFirst < Take, and its column arena slice is that difference.
	struct FLastFlush
	{
		uint32 ConsumeFirst = 0;
		uint32 Take = 0;
	};
	FLastFlush GetLastFlush() const { return LastFlush; }

	// Render thread, from inside a graph that consumes the column stage's
	// output (the batch graph): registers the arena and the stats buffer.
	// Arena is null until the first armed Flush has run on the render thread
	// -- the caller must fall back to a classic ColumnMain pass AND COUNT IT
	// when that happens, never silently read a buffer that does not exist.
	struct FColumnStageBindings
	{
		FRDGBufferRef Arena = nullptr;     // budget x 1,024 GpuColumnSample
		FRDGBufferRef Stats = nullptr;     // kStatsDwords dwords (verifies write [4..9])
		// The Voxelize stage's cell arena (cellBudget x 32,768 uint). Null
		// until the first voxelize-armed Flush has run on the render thread
		// -- same fallback-and-count contract as Arena.
		FRDGBufferRef CellArena = nullptr;
		// The fused ClassifyTotals stage's arenas (null until its first armed
		// flush, same contract). Offsets/counts: budget x 64 uints each;
		// totals: budget x 2. The batch graph binds the offsets to
		// BrickPackMain (BrickScanReadBase) and the totals to the claim pass
		// (TotalsReadBase); the counts exist for the verify compare only.
		FRDGBufferRef OccOffsetsArena = nullptr;
		FRDGBufferRef MatOffsetsArena = nullptr;
		FRDGBufferRef TotalsArena = nullptr;
	};
	FColumnStageBindings RegisterColumnStage(FRDGBuilder& GraphBuilder);

	// Game thread: enqueue this tick's upload + the args-setup pass + the
	// indirect spine-prover dispatch (VoxelWorklistConsume.usf). One render
	// command regardless of record count. Every ~5 s of nonzero consumption
	// it also enqueues the 16-byte PROOF readback and, when a prior proof has
	// landed, compares GPU consumption (count, record fold, tail cursor)
	// against the host mirror -- logging ok or a loud FAIL. All three values
	// are captured at the same flush on both sides (render commands and GPU
	// graphs execute in enqueue order), so the compare is exact, not
	// windowed-and-hopeful.
	void Flush(uint32 SliceBudgetRecords);

	// The host half of worklistRecordFold (VoxelWorklist.ush) -- same 16
	// terms, same derived multipliers, same uint32 wraparound. See the .ush
	// comment for why XOR-across-records is the combine.
	static uint32 FoldRecord(const FVoxelGpuChunkWorkRecord& Record);

	// Proof tallies for the caller's window line. THE FAILING READINGS:
	// Failed > 0 invalidates the leg outright (the GPU consumed different
	// records than the host mirrored -- every downstream number is suspect);
	// Landed == 0 while records flow means GPU consumption is UNVERIFIED and
	// the spine may be dispatching nothing (the exact silent state this
	// project keeps paying for). MalformedOnGpu > 0 with proofs passing means
	// the HOST staged garbage records -- the fold agrees on both sides
	// because the garbage was faithfully transported.
	// ColumnDwordMismatches > 0 means the CONVERTED column kernel and the
	// classic one disagree on bytes -- leg invalid, terrain wrong, digest at
	// risk. ColumnsChecked == 0 with the verify switch armed and converted
	// chunks flowing means the verify itself is dead and "no mismatches" is
	// vacuous. Both are cumulative GPU counters riding the proof readback.
	struct FProofStatus
	{
		uint64 Landed = 0;
		uint64 Failed = 0;
		uint64 MalformedOnGpu = 0;
		uint64 ColumnDwordMismatches = 0;
		uint64 ColumnsChecked = 0;
		// The Voxelize stage's byte gate, same contract as the column pair:
		// VoxCellMismatches > 0 invalidates the leg; VoxCellsChecked == 0
		// with the verify switch armed and converted chunks flowing means the
		// gate is dead and "no mismatches" is vacuous.
		uint64 VoxCellMismatches = 0;
		uint64 VoxCellsChecked = 0;
		// The fused ClassifyTotals stage's byte gate, same contract: offsets
		// + totals compared (which determine the counts), so a mismatch here
		// means the pack offsets or the claim size are WRONG -- pool
		// corruption, the leg invalid outright.
		uint64 CtDwordMismatches = 0;
		uint64 CtDwordsChecked = 0;
	};
	FProofStatus GetProofStatus() const { return Proof; }

	// Render thread: register the ring + args buffers into a graph that will
	// consume them (the converted chain's dispatches).
	struct FBindings
	{
		FRDGBufferSRVRef Records = nullptr;
		FRDGBufferRef IndirectArgs = nullptr;   // EVoxelWorklistStage::COUNT * 3 dwords
		FRDGBufferSRVRef Control = nullptr;     // [0]=consumeFirst [1]=consumeCount
	};
	FBindings Register(FRDGBuilder& GraphBuilder);

	// Window counters, and the identity that must hold: appended ==
	// consumed + pending + refusedFull. A drifting identity means records
	// are being lost or double-consumed -- the failing reading -- and the
	// .cpp logs it as such.
	struct FWindow
	{
		uint64 Appended = 0;
		uint64 Consumed = 0;
		uint64 RefusedFull = 0;
		uint32 Pending = 0;
	};
	FWindow ReadAndResetWindow();

private:
	uint32 Capacity = 0;
	uint32 Head = 0;   // next slot to write (host-owned)
	uint32 Tail = 0;   // first unconsumed (host mirror of the GPU cursor)
	TArray<FVoxelGpuChunkWorkRecord> Staged;
	TRefCountPtr<FRDGPooledBuffer> PooledRecords;
	TRefCountPtr<FRDGPooledBuffer> PooledArgs;
	TRefCountPtr<FRDGPooledBuffer> PooledControl;
	// The kStatsDwords-dword evidence buffer (prover [0..3], column verify
	// [4..5]; VoxelWorklistConsume.usf documents the layout).
	TRefCountPtr<FRDGPooledBuffer> PooledStats;
	FWindow Window;

	// --- Column stage (game-thread config, render-thread arena) -------------
	// The arena is created lazily inside the first armed Flush's render
	// command (AllocatePooledBuffer -- it must outlive the flush graph so the
	// batch graph can read it) and only ever touched on the render thread
	// after that; RegisterColumnStage is the render-thread accessor.
	bool bColumnStageArmed = false;
	FVoxelRasterAtlasGpu* ColumnAtlas = nullptr;
	uint32 ColumnSeedLo = 0;
	uint32 ColumnSeedHi = 0;
	int32 ColumnPixelSizeMm = 0;
	uint32 ColumnArenaRecords = 0;   // slices the arena holds; latched at creation
	TRefCountPtr<FRDGPooledBuffer> PooledColumnArena;
	// --- Voxelize stage (same lifetime rules as the column arena) -----------
	bool bVoxelizeStageArmed = false;
	uint32 VoxelizeCellBudget = 0;   // per-flush record cap while armed (see setter)
	uint32 CellArenaRecords = 0;     // slices the cell arena holds; latched at creation
	TRefCountPtr<FRDGPooledBuffer> PooledCellArena;
	// --- fused ClassifyTotals stage (same lifetime rules) -------------------
	bool bClassifyStageArmed = false;
	uint32 ClassifyArenaRecords = 0; // slices the five arenas hold; latched at creation
	TRefCountPtr<FRDGPooledBuffer> PooledOccCountsArena;
	TRefCountPtr<FRDGPooledBuffer> PooledMatCountsArena;
	TRefCountPtr<FRDGPooledBuffer> PooledOccOffsetsArena;
	TRefCountPtr<FRDGPooledBuffer> PooledMatOffsetsArena;
	TRefCountPtr<FRDGPooledBuffer> PooledTotalsArena;
	FLastFlush LastFlush;

	// --- the spine proof (game-thread state) --------------------------------
	// Per-slot record folds, written when a staged record is assigned its ring
	// slot, XORed into CumConsumedFold when the host mirror consumes the slot.
	// 4 bytes per slot; this is what lets the host know EXACTLY what the GPU
	// should have consumed without ever reading the ring back.
	TArray<uint32> FoldRing;
	uint64 CumConsumedRecords = 0;
	uint32 CumConsumedFold = 0;
	// One proof in flight at a time. The stash is the host's answer, captured
	// at the requesting flush; the render side lands the GPU's answer in
	// file-scope atomics (see .cpp) keyed by ProofSeq.
	bool bProofPending = false;
	uint32 ProofSeq = 0;
	uint32 ProofStashTail = 0;
	uint32 ProofStashConsumed = 0;
	uint32 ProofStashFold = 0;
	double LastProofSeconds = 0.0;
	FProofStatus Proof;

	// --- render-thread-only proof plumbing ----------------------------------
	// Created lazily inside a Flush render command, polled at the top of each
	// subsequent one, deleted by a render command from ~FVoxelGpuWorklist
	// (FVoxelGpuBrickStack's readback-release pattern, verbatim).
	FRHIGPUBufferReadback* ProofReadback = nullptr;
	bool bProofCopyInFlight = false;
	uint32 ProofCopySeq = 0;
};
