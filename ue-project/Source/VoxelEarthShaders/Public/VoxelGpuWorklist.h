#pragma once
// FVoxelGpuWorklist -- the persistent chunk-record ring for P3 (persistent
// worklist + indirect dispatch; docs/gpu-worklist-plan-2026-08-23.md).
//
// *** NOT WIRED. NOTHING DISPATCHES FROM THIS YET. ***
// This is the CONTRACT half of P3, landed ahead of the kernel conversion so
// the record layout and ring discipline are fixed while the conversion is
// authored: the 64-byte record, the host ring, and the args-setup pass that
// turns "N records pending" into per-stage DispatchIndirect arguments. The
// generation kernels still read per-region $Globals and are dispatched one
// pass per stack; converting them to read Records[recordIndex] is the P3 work
// proper, sequenced AFTER the raster atlas (A) because the atlas is what
// empties the request of its variable-size half (the 46 KB window) -- without
// A there is no 64-byte record to put in a ring.
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
	Column = 0,     // 32x32 columns / 64 threads = 16 groups per record
	Voxelize,       // 32x32xBricksZ*8 cells -> 4*BricksZ groups (BricksZ=4 fixed)
	AssetStamp,     // per-column gather loop, 16 groups (order-preserving form)
	ClassifyTotals, // 64 bricks, single group: classify + in-group scan + totals
	PackClaim,      // pack + claim fused, 64 bricks / group pair
	Write,          // occ+mat word copies, groups from claim sizes (upper bound)
	Record,         // descriptor + chunk record + index cell, 1 group
	COUNT
};

class VOXELEARTHSHADERS_API FVoxelGpuWorklist
{
public:
	// Capacity is a latch, not a growth policy: a full ring REFUSES appends
	// and counts them (the caller keeps the chunk on its pending queue), so
	// back-pressure is visible in a counter instead of hidden in a realloc.
	void Init(uint32 RecordCapacity);
	bool IsInitialized() const { return Capacity != 0; }

	// Game thread: stage records for this tick. Returns how many were
	// accepted (the rest refused-full and counted).
	int32 Append(TArrayView<const FVoxelGpuChunkWorkRecord> Records);

	// Game thread: enqueue this tick's upload + the args-setup pass. One
	// render command regardless of record count.
	void Flush(uint32 SliceBudgetRecords);

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
	FWindow Window;
};
