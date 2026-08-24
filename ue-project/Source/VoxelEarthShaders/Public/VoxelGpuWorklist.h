#pragma once
// FVoxelGpuWorklist -- the persistent chunk-record ring for P3 (persistent
// worklist + indirect dispatch; docs/gpu-worklist-plan-2026-08-23.md).
//
// *** SPINE WIRED; KERNELS CONVERTED: Column, Voxelize, fused ClassifyTotals,
// AssetStamp (2026-08-23). ***
// Behind -VoxelGpuWorklistAssetStamp=1 (on top of the Voxelize switch) Flush
// stages every consumed asset record's resolved instances into a per-flush
// blob (stampsStaged, LevelFlags bit 9 -- set ONLY for records consumed the
// same flush, so a deferred record can never read another flush's blob, and
// set BEFORE the folds so both proof mirrors see the same bytes) and
// dispatches the ORDER-PRESERVING gather (one thread per column, instances
// walked in slice order -- the classic per-instance pass order, in-thread)
// between the voxelize and classify dispatches. ASSET CHUNKS thereby enter
// the whole cell-arena chain. Byte gate: -VoxelGpuWorklistVerifyStamp=1 into
// stats [10..11] (classic voxelize + per-instance stamps as the reference).
//
// Behind -VoxelGpuWorklistClaim=1 (on top of the Pack switch) the flush graph
// ALSO dispatches the Claim stage -- the claim + the four pool writes as
// THREE indirect dispatches per tick (VoxelWorklistClaim.usf has the whole
// argument) -- and a claim-fed chunk has NO brick work in the batch graph at
// all: production is fully inside the flush graph, per-chunk batch passes
// 5 -> 0, and the pass term goes FLAT in N (the spine constant plus nothing
// per chunk). Byte gate: -VoxelGpuWorklistVerifyClaim=1 into stats [14..15]
// (the LANDED pool state vs the stage's own sources through the shared
// factored text). ALL SEVEN GENERATION STAGES ARE CONVERTED.
//
// Behind -VoxelGpuWorklistClassify=1 (on top of the Voxelize switch) the
// flush graph ALSO dispatches the fused ClassifyTotals pair -- one group per
// BRICK classify off the cell arena, then a one-group-per-record in-group
// scan + totals -- and every cell-fed chunk skips its classic
// BrickClassifyMain, BOTH 3-pass scans and BrickTotalMain: EIGHT passes, the
// conversion's largest single cut (15 -> 7 on the lean-alloc shape).
// BrickPackMain reads offsets through brickpack.ush's BrickScanReadBase; the
// claim pass reads totals through VoxelBrickPoolAlloc.usf's TotalsReadBase.
// Byte gate: -VoxelGpuWorklistVerifyCT=1 into stats [8..9].
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
#include "VoxelBrickPool.h"   // FVoxelBrickPoolAllocLayout -- the Claim stage binds the pool

class FRDGBuilder;
class FRHICommandListImmediate;
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
	// The CPU-dispensed descriptor-block base (the shell's BrickBase) -- the
	// one claim/write input the Claim stage needs that is neither derivable
	// from the origin fields nor a process-wide constant. Truthful on every
	// record (the AssetCount precedent).
	uint32 BrickBase = 0;
	uint32 Reserved[4] = {0, 0, 0, 0};
};
static_assert(sizeof(FVoxelGpuChunkWorkRecord) == 64, "the 64-byte record IS the contract");

// One resolved asset instance as the worklist AssetStamp gather reads it --
// MIRROR of GpuAssetStampInstance (VoxelWorklist.ush), 12 dwords, 48 B.
// AnchorRelVx/Vy are relative to the OWNING record's OriginVx/Vy;
// ColStartsBase indexes the flush blob's ColStarts, whose VALUES index the
// flush blob's Spans (both rebased when Flush stages the payload).
struct FVoxelWorklistAssetInstance
{
	int32  AnchorRelVx = 0;
	int32  AnchorRelVy = 0;
	int32  AnchorVz = 0;
	int32  GridOriginZ = 0;
	int32  RotOriginX = 0;
	int32  RotOriginY = 0;
	uint32 YawQuarter = 0;
	uint32 SizeX = 0;
	uint32 SizeY = 0;
	uint32 SizeZ = 0;
	uint32 ColStartsBase = 0;
	uint32 Pad0 = 0;
};
static_assert(sizeof(FVoxelWorklistAssetInstance) == 48, "the 48-byte instance IS the contract");

// A record's asset payload, handed to Append alongside the record and staged
// into the FLUSH that consumes it (never uploaded for deferred records --
// stampsStaged, LevelFlags bit 9, is set only when instances actually landed
// in the flush blob). Instance ColStartsBase indexes THIS payload's
// ColStarts; ColStarts values index THIS payload's Spans; Flush rebases both
// as it concatenates.
struct FVoxelWorklistAssetPayload
{
	TArray<FVoxelWorklistAssetInstance> Instances;
	TArray<uint32> ColStarts;
	TArray<uint32> Spans;
	bool IsEmpty() const { return Instances.Num() == 0; }
};

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
	// The AssetStamp gather: one thread per column, the Column stage's shape.
	static constexpr uint32 kStampGroupsPerRecord = kColumnsPerRecord / 64;
	// The Pack stage: one group per brick, the classic pack shape (the
	// payload assembly is groupshared). Arena strides are the WORST CASE per
	// record; actual counts live compactly at the front of each slice, which
	// is what lets the claim's spare pair hand the copy kernels a flat base.
	// kMatWordsPerRecord mirrors brickpack.ush's kMaxBrickMatWords (132) --
	// the kernel #errors if the product drifts.
	static constexpr uint32 kPackGroupsPerRecord = kBricksPerRecord;
	static constexpr uint32 kOccWordsPerRecord = kBricksPerRecord * 16;
	static constexpr uint32 kMatWordsPerRecord = kBricksPerRecord * 132;
	static constexpr uint32 kSkipWordsPerRecord = kOccWordsPerRecord / 16 * 2;
	static constexpr uint32 kMaskDwordsPerRecord = 2;
	// The Claim stage (stage 6): the word-copy dispatch rides the Write
	// triple at worst-case groups -- (1,024 occ + 8,448 mat) / 64 threads =
	// 148 per record, the classic copies' own worst-case-dispatch decision.
	// The claim, desc+record and verify dispatches ride the Record triple
	// (1 group per record, the prover's shape). 8 dwords of claim slice per
	// record, the classic claim's own contract.
	static constexpr uint32 kWriteGroupsPerRecord =
		(kOccWordsPerRecord + kMatWordsPerRecord) / 64;
	static constexpr uint32 kClaimDwordsPerRecord = 8;

	// --- THE RECORD DIMENSION, and why the Write triple alone uses Y --------
	//
	// Every stage's indirect triple was {Take x groupsPerRecord, 1, 1}. A
	// dispatch dimension is capped at 65,535 groups (D3D12_CS_DISPATCH_MAX_
	// THREAD_GROUPS_PER_DIMENSION), so the Write stage's 148 groups/record
	// capped a flush at 442 records = ~26,500 chunks/s at 60 ticks -- BELOW
	// the 50,000 goal, and the lowest ceiling of any stage by a factor of
	// 2.3. Carrying the record in Y instead, {148, Take, 1}, removes it: X is
	// a constant 148 and Y is the record count.
	//
	// ONE CONSTANT, TWO CONSUMERS, WHICH IS THE WHOLE POINT. This value fills
	// the args kernel's RecordInY table AND is handed to the claim kernels as
	// VXC_WORKLIST_WRITE_RECORD_IN_Y, and VoxelWorklistClaim.usf #errors if it
	// is not 1. A torn dispatch here is not loud on its own -- if the args
	// went back to 1D, Gid.y would be 0 for every group and only record 0's
	// pool words would be written, leaving every other chunk's volume
	// unwritten: HOLES, silently. The #error is what makes that impossible
	// rather than unlikely.
	static constexpr uint32 kWriteRecordInY = 1;

	// The per-flush record cap the 1D stages still impose. With the Write
	// stage on Y, the binding stage is whichever REMAINING stage has the most
	// groups per record -- Classify and Pack, at 64 (one group per brick) --
	// so 65,535 / 64 = 1,023 records per flush = ~61,000 chunks/s at 60
	// ticks, above the 50,000 goal. The .cpp static_asserts that no 1D stage
	// exceeds 64, so this number cannot drift away from the table it
	// describes; the arming path REFUSES a cell budget above it, loudly,
	// rather than clipping a dispatch silently.
	static constexpr uint32 kMaxRecordsPerFlush = 65535 / 64;
	// Stats buffer: [0..3] the prover's evidence (VoxelWorklistConsume.usf),
	// [4..5] the column verify's mismatch/checked counters
	// (VoxelWorklistColumn.usf), [6..7] the voxelize verify's
	// (VoxelWorklistVoxelize.usf), [8..9] the classify-totals verify's
	// (VoxelWorklistClassify.usf), [10..11] the asset-stamp verify's (the
	// voxelize verify kernel with VerifyStatsBase 10 -- it compares the
	// stamped cells), [12..13] the pack verify's (VoxelWorklistPack.usf),
	// [14..15] the claim verify's (VoxelWorklistClaim.usf -- [14]
	// mismatches, [15] dwords checked), [16] THE CLAIM STAGE'S OWN TRAFFIC
	// COUNTER: how many records ClaimWorklistMain actually found eligible,
	// cumulative, written on every armed tick whether or not any verify is
	// armed. It exists because the two things that must be the same set --
	// the records the GPU claims and the chunks the host counts as converted
	// -- were NOT the same set, and no instrument in the system could say so:
	// `checked` was read as evidence over the host's conv count when it was
	// really running over ~31x that many records. [17..19] reserved (the
	// readback copies whole dwords; kept a multiple of 4).
	static constexpr uint32 kStatsDwords = 20;
	static constexpr uint32 kStatsClaimEligible = 16;

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
	// AssetPayloads (AssetStamp stage): parallel to Records when given --
	// element i is record i's instances, empty for an asset-free record. A
	// REFUSED record's payload is dropped with it. Payloads are MOVED from.
	int32 Append(TArrayView<const FVoxelGpuChunkWorkRecord> Records,
	             TArray<uint32>* OutMonotonicIndices = nullptr,
	             TArray<FVoxelWorklistAssetPayload>* AssetPayloads = nullptr);

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

	// --- the AssetStamp gather stage (-VoxelGpuWorklistAssetStamp) ----------
	//
	// Gated behind the Voxelize stage (it stamps the cell arena that stage
	// fills). Arming it is what lets asset-bearing records into the whole
	// cell-arena chain: Flush stages their instance payloads into a per-flush
	// blob (for records consumed that same flush -- stampsStaged bit 9) and
	// dispatches the order-preserving gather between the voxelize and
	// classify dispatches.
	void SetAssetStampStageArmed(bool bArmed) { bStampStageArmed = bArmed; }
	bool IsAssetStampStageArmed() const { return bStampStageArmed && IsVoxelizeStageArmed(); }

	// --- the Pack stage (-VoxelGpuWorklistPack) -----------------------------
	//
	// Gated behind the fused ClassifyTotals stage (its scanned-offset arenas
	// are this stage's input). With it armed, a totals-fed chunk's classic
	// BrickChunkMask clear and BrickPackMain disappear; the claim/write
	// passes source descriptors and words from the pack arenas through the
	// bases FRegionGraphResources carries.
	void SetPackStageArmed(bool bArmed) { bPackStageArmed = bArmed; }
	bool IsPackStageArmed() const { return bPackStageArmed && IsClassifyStageArmed(); }

	// --- the Claim stage (-VoxelGpuWorklistClaim; P3 stage 6) ---------------
	//
	// The claim + the four pool writes, moved into the flush graph as three
	// indirect dispatches per tick (see VoxelWorklistClaim.usf's banner). A
	// claim-fed job adds ZERO brick passes to the batch graph; delivery keeps
	// the lean-alloc shape.
	//
	// The pool's alloc buffers are registered into the flush graph through a
	// BINDER the manager supplies (the plan doc's decision: the worklist does
	// not include the pool's lifecycle). It runs on the RENDER thread inside
	// the flush command, must EnsureCreated the buffers and register them
	// into the given graph, and returns false when the allocator is
	// unavailable -- the flush then skips the claim dispatches for that tick
	// (logged once, loud). The affected claim-staged records land NOTHING:
	// their slots stay unwritten (missing chunks, the P1 xcheck's
	// `unwritten` reading), never corrupted -- the batch side must still not
	// claim classically for them, because it cannot know which flushes went
	// dark.
	struct FPoolBindings
	{
		FRDGBufferRef PoolDesc = nullptr;
		FRDGBufferRef PoolOcc = nullptr;
		FRDGBufferRef PoolMat = nullptr;
		FRDGBufferRef PoolTable = nullptr;
		FRDGBufferRef AllocState = nullptr;
		FRDGBufferRef AllocBitmap = nullptr;
		FRDGBufferRef AllocSide = nullptr;
		FVoxelBrickPoolAllocLayout Layout;
		uint32 ChunkRecordDwords = 0;
		bool IsValid() const
		{
			return PoolDesc && PoolOcc && PoolMat && PoolTable
			    && AllocState && AllocBitmap && AllocSide && ChunkRecordDwords > 0;
		}
	};
	using FPoolBinder = TFunction<bool(FRDGBuilder&, FRHICommandListImmediate&, FPoolBindings&)>;

	// Game thread, before Flush, idempotent. Gated BEHIND the Pack stage (the
	// claim sources from the pack arenas and sizes from the totals arena).
	// bVerify arms the per-flush byte gate (ClaimWorklistVerifyMain into
	// stats [14..15]) -- one extra indirect dispatch per tick, verify arm
	// only. Arming without a binder is REFUSED loudly: a claim stage that
	// cannot reach the pool would set claimStaged bits whose records nothing
	// ever lands -- the exact quiet-dead shape this project keeps paying for.
	void SetClaimStageArmed(bool bArmed, FPoolBinder Binder, bool bVerify);
	bool IsClaimStageArmed() const { return bClaimStageArmed && IsPackStageArmed(); }

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

	// Game thread: how many records the LAST flush stamped claimStaged (bit
	// 10) on, and the running total. This is the HOST's count of the set the
	// GPU's ClaimWorklistMain will find eligible; the manager compares it to
	// its own bWorklistClaimFed count and to the GPU's [16] every proof.
	// THE FAILING READINGS, both directions:
	//   * CumClaimStaged > the manager's WorklistClaimConverted -- records
	//     staged for a GPU claim whose host job did NOT convert, so its batch
	//     graph claims the same slot too: A DOUBLE CLAIM, pool ranges leaking
	//     at exactly that rate ([brick-gpualloc] `unclaimed` goes negative).
	//   * CumClaimStaged < WorklistClaimConverted -- the host skipped a batch
	//     graph's brick chain for a chunk nothing ever claimed: the chunk
	//     lands unwritten (holes), never corruption.
	//   * CumClaimStaged > 0 with the GPU's [16] flat at 0 -- the stage is
	//     staging records and the kernel is not running: DEAD STAGE.
	uint32 GetClaimStagedThisFlush() const { return ClaimStagedThisFlush; }
	int64 GetCumClaimStaged() const { return CumClaimStaged; }
	// The GPU's own count of records ClaimWorklistMain found eligible, as of
	// the last landed proof; -1 until a proof has landed.
	int64 GetGpuClaimEligible() const { return GpuClaimEligible; }

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
		// The Pack stage's arenas (null until its first armed flush, same
		// contract). Desc: budget x 64 uint2; occ/mat words: budget x the
		// worst-case strides; mask: budget x 2 (the flush cleared it before
		// packing). The skip arena stays flush-side -- nothing consumes it.
		FRDGBufferRef PackDescArena = nullptr;
		FRDGBufferRef PackOccArena = nullptr;
		FRDGBufferRef PackMatArena = nullptr;
		FRDGBufferRef PackMaskArena = nullptr;
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
		// The AssetStamp gather's byte gate (stats [10..11]): the stamped
		// cell arena vs a classic voxelize + per-instance stamp re-run. A
		// mismatch is a wrong asset voxel in the pool -- leg invalid.
		uint64 StampCellMismatches = 0;
		uint64 StampCellsChecked = 0;
		// The Pack stage's byte gate (stats [12..13]): desc + bounded word
		// ranges + chunk mask vs the classic pack. A mismatch is the POOL
		// PAYLOAD ITSELF being wrong -- leg invalid outright.
		uint64 PackDwordMismatches = 0;
		uint64 PackDwordsChecked = 0;
		// The Claim stage's byte gate (stats [14..15]): the LANDED pool
		// state -- descriptors, bounded word ranges, the chunk record (or
		// its fail-zero) -- vs the stage's own sources through the shared
		// factored text. A mismatch is pool corruption at the landing site;
		// leg invalid outright. Checked == 0 with claim-fed chunks flowing
		// under the verify switch is a DEAD GATE, never a pass.
		uint64 ClaimDwordMismatches = 0;
		uint64 ClaimDwordsChecked = 0;
		// The set-identity pair. ClaimEligibleOnGpu is stats[16] -- records
		// ClaimWorklistMain actually claimed; ClaimStagedOnHost is the host's
		// own count of records it stamped claimStaged on. THEY MUST BE THE
		// SAME NUMBER (the GPU lagging by the readback latency is the only
		// permitted gap, and only in that direction). GPU ahead = a double
		// claim, one leaked grant per excess record.
		uint64 ClaimEligibleOnGpu = 0;
		int64 ClaimStagedOnHost = 0;
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
	// --- AssetStamp gather stage --------------------------------------------
	bool bStampStageArmed = false;
	// Parallel to Staged: record i's asset payload (empty for asset-free).
	// Consumed by Flush -- staged into the flush blob for records the flush
	// consumes, dropped (with a counter's worth of nothing: the record falls
	// back on the host) for deferred ones.
	TArray<FVoxelWorklistAssetPayload> StagedAssets;
	// --- fused ClassifyTotals stage (same lifetime rules) -------------------
	bool bClassifyStageArmed = false;
	uint32 ClassifyArenaRecords = 0; // slices the five arenas hold; latched at creation
	TRefCountPtr<FRDGPooledBuffer> PooledOccCountsArena;
	TRefCountPtr<FRDGPooledBuffer> PooledMatCountsArena;
	TRefCountPtr<FRDGPooledBuffer> PooledOccOffsetsArena;
	TRefCountPtr<FRDGPooledBuffer> PooledMatOffsetsArena;
	TRefCountPtr<FRDGPooledBuffer> PooledTotalsArena;
	// --- Pack stage (same lifetime rules) -----------------------------------
	bool bPackStageArmed = false;
	uint32 PackArenaRecords = 0;     // slices the pack arenas hold; latched at creation
	TRefCountPtr<FRDGPooledBuffer> PooledPackDescArena;
	TRefCountPtr<FRDGPooledBuffer> PooledPackOccArena;
	TRefCountPtr<FRDGPooledBuffer> PooledPackMatArena;
	TRefCountPtr<FRDGPooledBuffer> PooledPackSkipArena;
	TRefCountPtr<FRDGPooledBuffer> PooledPackMaskArena;
	// --- Claim stage (game-thread flags; the claim buffer is a per-flush
	// RDG transient -- nothing outside the flush graph reads it) ------------
	bool bClaimStageArmed = false;
	bool bClaimVerifyArmed = false;
	FPoolBinder ClaimPoolBinder;
	// The host's own count of the set the GPU will claim: records this flush
	// stamped claimStaged on (bit 10, which now requires the manager's bit 11
	// hostClaimCandidate). Compared against the manager's converted count and
	// the GPU's stats[16] -- see GetClaimStagedThisFlush's failing readings.
	uint32 ClaimStagedThisFlush = 0;
	int64 CumClaimStaged = 0;
	int64 GpuClaimEligible = -1;   // GPU stats[16] as of the last landed proof
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
