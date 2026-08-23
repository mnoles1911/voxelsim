// The seven-pass RDG graph, factored out of RunRegionBlocking so the blocking
// verification path and the async streaming runner build the SAME passes.
//
// WHY THIS HEADER EXISTS. RunRegionBlocking used to inline the whole graph
// inside its render command. FVoxelGpuMeshJobManager needs the identical chain
// -- ColumnMain, VoxelizeMain, MeshCountMain, the three scan passes, MeshEmitMain
// -- and copying it would have created two graphs that agree today and drift the
// first time a pass is added. So the graph construction lives in exactly one
// function and both callers call it. What each caller does differ on is what it
// reads BACK: the blocking path pulls columns and cells too (it is a bit-exactness
// harness), the async path pulls only the quad stream and its scan tables.
//
// PRIVATE ON PURPOSE. It exposes FRDGBufferRef, which is only meaningful on the
// render thread while a specific FRDGBuilder is alive, so it must not become
// part of the module's public surface.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"
#include "VoxelBrickPool.h"   // FVoxelBrickChunkShading, carried into the record kernel

struct FVoxelGpuRegionRequest;
class FRDGBuilder;
class FVoxelRasterAtlasGpu;

namespace VoxelGpuWorldGen
{
	// Every count the graph and its readbacks are sized from. Derived purely
	// from the request, so a caller can size staging buffers before touching the
	// render thread.
	struct FRegionGraphSizes
	{
		uint32 BricksX = 0;
		uint32 BricksY = 0;
		uint32 BricksZ = 0;

		uint32 NumColumns = 0;
		uint32 NumCells = 0;

		// Zero when bMeshChain is false.
		uint32 MaskCount = 0;
		uint32 NumBlocks = 0;
		uint32 MaxQuads = 0;

		// Wave D / D2. The quad buffer holds QuadWriteBase slots this dispatch
		// must not touch, followed by its own MaxQuads. Equal to MaxQuads
		// whenever the base is zero, which is every path except a batched
		// emit into a shared buffer.
		uint32 QuadWriteBase = 0;
		uint32 QuadBufferElements = 0;

		bool bMesh = false;

		// --- P1-C: the brick chain ------------------------------------------
		//
		// All zero unless the request set bBrickPack. Every count here is a
		// property of the region's CHUNK decomposition, not of its bricks in
		// general: brickpack.ush packs whole 4x4x4-brick chunks and nothing
		// else, so a region 6 bricks wide contributes ONE chunk and its two
		// leftover brick columns contribute nothing.
		uint32 NumBrickChunks = 0;
		uint32 NumBricks = 0;            // NumBrickChunks * 64
		uint32 BrickScanBlocks = 0;      // DivideAndRoundUp(NumBricks, 256)
		// Worst-case arena sizes, which is what the scratch buffers are sized
		// to. 16 dwords of occupancy per brick; 132 dwords of material per
		// brick (the 8 bpp case: 512 voxels x 8 bits and no local palette --
		// see kMaxBrickMatWords in brickpack.ush). ~38 KB for one chunk, which
		// is why the scratch side needs no readback to size itself and only the
		// POOL allocation does.
		uint32 BrickOccWordsMax = 0;
		uint32 BrickMatWordsMax = 0;
		bool bBrickPack = false;

		uint32 BrickTotalsBytes() const { return 2 * uint32(sizeof(uint32)); }

		// Byte sizes of the five readbackable buffers.
		uint32 ColumnsBytes() const;
		uint32 CellsBytes() const { return NumCells * uint32(sizeof(uint32)); }
		uint32 CountsBytes() const { return MaskCount * uint32(sizeof(uint32)); }
		// The WHOLE quad buffer, base included — that is what a readback of it
		// costs, and what AddEnqueueCopyPass has to be given.
		uint32 QuadsBytes() const { return QuadBufferElements * uint32(sizeof(uint64)); }
	};

	// The graph's output buffers. Counts/Offsets/Quads/Total are null when the
	// request asked for generation only.
	struct FRegionGraphResources
	{
		FRDGBufferRef Columns = nullptr;
		FRDGBufferRef Cells = nullptr;
		FRDGBufferRef Counts = nullptr;
		FRDGBufferRef Offsets = nullptr;
		FRDGBufferRef Quads = nullptr;

		// Wave D / D3: ONE uint — how many quads the emit pass actually wrote.
		//
		// This is the only thing a streaming job needs off the GPU before it can
		// allocate a pool range, and reading it instead of Counts+Offsets+Quads
		// is the difference between 4 bytes and ~810 KB per chunk. See
		// VoxelQuadScan.usf for the arithmetic that makes that the whole point
		// of the wave.
		FRDGBufferRef Total = nullptr;

		// Wave D / D6: two ints -- max ColumnSurfaceTopVoxel and min
		// ColumnDeepestAirVoxel over the band grid, as raw voxel z. Null unless
		// the request asked for a band. The +1/-1 widening and the clamp that
		// turn these into an FFootprintBand stay on the CPU.
		FRDGBufferRef Band = nullptr;

		// --- P1-C: the packed brick volume, CHUNK-RELATIVE ------------------
		//
		// Null unless the request set bBrickPack. THE OFFSETS INSIDE BrickDesc
		// ARE CHUNK-RELATIVE, because these passes dispatch with all three
		// write bases at ZERO (docs/brick-volume-format.md section 6b). They
		// become pool addresses only in AddBrickDescPoolWritePass, which is the
		// one place a base is added -- see VoxelBrickPoolWrite.usf.
		//
		// BrickSkip is written by BrickPackMain and DELIBERATELY DISCARDED: the
		// 4^3 intra-brick mask is derivable for free from the 16 occupancy
		// dwords a marcher already holds in registers, and the contract's own
		// recommendation is to spend the ~7 MiB on payload instead. The buffer
		// exists because the kernel writes it unconditionally, not because
		// anything reads it.
		FRDGBufferRef BrickDesc = nullptr;
		FRDGBufferRef BrickOcc = nullptr;
		FRDGBufferRef BrickMat = nullptr;
		FRDGBufferRef BrickSkip = nullptr;
		FRDGBufferRef BrickChunkMask = nullptr;
		// Two uints: the occupancy and material dword totals this chunk needs.
		// The ONLY thing the CPU reads on this path, and what the pool
		// allocation is made from.
		FRDGBufferRef BrickTotals = nullptr;

		// Tier B.1 (voxel.GPU.WorldGenBatch): (2 + 2*NumBrickChunks) uints --
		// the region totals pair, then each chunk's own occ/mat dword pair.
		// Null unless the request set bPerChunkBrickTotals. This is what the
		// batched stack path reads back INSTEAD of K per-chunk BrickTotals
		// copies: one copy pass, (2 + 2K) * 4 bytes, still nothing but sizes
		// crossing PCIe.
		FRDGBufferRef BrickStackTotals = nullptr;

		FRegionGraphSizes Sizes;
	};

	// Rejects anything the kernels' own guards would otherwise have to absorb.
	// Safe to call from any thread; it only reads the request.
	bool ValidateRegionRequest(const FVoxelGpuRegionRequest& Request, FString& OutError);

	// Pure arithmetic over the request. Assumes it already validated.
	FRegionGraphSizes ComputeRegionGraphSizes(const FVoxelGpuRegionRequest& Request);

	// --- P3 Column stage (the first converted worklist kernel) --------------
	//
	// FWorklistColumnFeed: a lean brick region whose COLUMNS were already
	// computed this tick by the worklist's indirect Column dispatch
	// (ColumnWorklistMain writing the flush-level arena). With a feed,
	// AddRegionPasses SKIPS its ColumnMain pass and binds VoxelizeMain's
	// InColumns to the arena at ColumnReadBase = SliceIndex * 1024. bVerify
	// additionally runs the classic ColumnMain into a transient AND a compare
	// pass (ColumnWorklistVerifyMain) accumulating into VerifyStats [4..5] --
	// the plan doc's stage-2 byte gate, verify arm only.
	//
	// PRECONDITIONS the caller owns (checkf'd): atlas request, BandEdge 0,
	// 32x32 columns -- i.e. exactly the worklist record eligibility.
	struct FWorklistColumnFeed
	{
		FRDGBufferRef Arena = nullptr;        // budget x 1,024 GpuColumnSample
		uint32 SliceIndex = 0;                // this chunk's slice in this tick's consume
		bool bVerify = false;
		FRDGBufferRef VerifyStats = nullptr;  // the worklist stats buffer ([4..7])

		// P3 Voxelize stage: non-null when this chunk's CELLS were also
		// computed this tick by the worklist's indirect Voxelize dispatch
		// (VoxelizeWorklistMain writing the flush-level cell arena at slice
		// SliceIndex * 32768). AddRegionPasses then SKIPS its VoxelizeMain
		// pass and binds BrickClassify/BrickPack's InCells to this arena at
		// CellReadBase = SliceIndex * 32768. Null -- the chunk voxelizes
		// classically (spine-only legs, asset chunks, deferred records).
		// EXTRA PRECONDITIONS the caller owns (checkf'd): no asset instances,
		// BricksZ == 4, brick-only (no mesh chain), bBrickPack.
		FRDGBufferRef CellArena = nullptr;    // cellBudget x 32,768 uint
		// Verify arm for the Voxelize stage: run the classic VoxelizeMain as
		// well (into the region's transient Cells, reading the SAME column
		// arena slice) plus a compare pass into VerifyStats [6..7].
		bool bVerifyVox = false;
	};

	// The once-per-tick indirect Column dispatch, added to the worklist FLUSH
	// graph (caller: FVoxelGpuWorklist::Flush). Declared here for the quad
	// passes' reason: the shader class lives in VoxelGpuWorldGen.cpp (it
	// compiles worldgen.ush and must carry the version-lock define), the
	// caller is another translation unit.
	struct FWorklistColumnDispatch
	{
		FRDGBufferRef Records = nullptr;       // the ring
		FRDGBufferRef Control = nullptr;       // [0]=consumeFirst [1]=consumeCount
		FRDGBufferRef IndirectArgs = nullptr;  // the args pass's triples
		uint32 IndirectArgsOffset = 0;         // byte offset of the Column triple
		uint32 RingCapacity = 0;
		FRDGBufferRef ColumnArena = nullptr;   // written: one slice per consumed record
		FVoxelRasterAtlasGpu* Atlas = nullptr; // registered into this graph here
		uint32 SeedLo = 0;
		uint32 SeedHi = 0;
		int32 PixelSizeMm = 0;
	};
	void AddWorklistColumnPass(FRDGBuilder& GraphBuilder, const FWorklistColumnDispatch& Dispatch);

	// The once-per-tick indirect Voxelize dispatch (P3 stage 2), added to the
	// worklist FLUSH graph right after the column pass -- it reads the column
	// arena that pass wrote and fills the cell arena the batch graph's brick
	// chain reads. Same declaration-placement reasons as the column pass.
	struct FWorklistVoxelizeDispatch
	{
		FRDGBufferRef Records = nullptr;       // the ring
		FRDGBufferRef Control = nullptr;       // [0]=consumeFirst [1]=consumeCount
		FRDGBufferRef IndirectArgs = nullptr;  // the args pass's triples
		uint32 IndirectArgsOffset = 0;         // byte offset of the Voxelize triple
		uint32 RingCapacity = 0;
		FRDGBufferRef ColumnArena = nullptr;   // read: the column stage's output
		FRDGBufferRef CellArena = nullptr;     // written: one 32,768-cell slice per record
		FVoxelRasterAtlasGpu* Atlas = nullptr; // registered into this graph here
		uint32 SeedLo = 0;
		uint32 SeedHi = 0;
		int32 PixelSizeMm = 0;
	};
	void AddWorklistVoxelizePass(FRDGBuilder& GraphBuilder, const FWorklistVoxelizeDispatch& Dispatch);

	// Adds the seven passes to GraphBuilder and returns the buffers they wrote.
	// RENDER THREAD ONLY. Does not execute the graph, does not enqueue any
	// readback, and does not block -- the caller owns all three decisions.
	//
	// Request must stay alive for the duration of this call. It does NOT need to
	// outlive it: the raster arrays are copied into RDG's own allocator here
	// (CreateStructuredBuffer with the default ERDGInitialDataFlags copies), and
	// the loose parameters are plain scalars written into the parameter struct.
	//
	// ColumnFeed (P3 Column stage): see FWorklistColumnFeed above. Null -- the
	// default, and every caller except the lean worklist path -- is the shipped
	// graph, byte for byte.
	FRegionGraphResources AddRegionPasses(FRDGBuilder& GraphBuilder, const FVoxelGpuRegionRequest& Request,
	                                      const FWorklistColumnFeed* ColumnFeed = nullptr);

	// --- Wave D / D1: the two GPU-side quad copies --------------------------
	//
	// Declared here rather than in each caller because the shader classes they
	// dispatch live in VoxelGpuWorldGen.cpp next to FVoxelQuadTotalCS (one
	// IMPLEMENT_GLOBAL_SHADER per class, in one translation unit) while the
	// callers are two OTHER translation units -- FVoxelGpuMeshJobManager for the
	// compaction and UVoxelGpuPoolComponent for the pool write. This header is
	// already the private seam between the graph and its callers, and exposing a
	// PASS rather than a shader class keeps the parameter struct where its
	// kernel is.
	//
	// Both are RENDER THREAD ONLY and take FRDGBufferRefs, which is exactly why
	// this header stays private. See ue-project/Shaders/VoxelQuadPoolWrite.usf
	// for what the kernels do and why they are compute passes rather than
	// AddCopyBufferPass.

	// Copies [SrcFirst, SrcFirst + NumQuads) of Src into Dst[0, NumQuads).
	// Dst must hold at least NumQuads elements of 8 bytes.
	//
	// This is phase 2 of a mesh job under voxel.GPU.MeshDirectToPool: it moves a
	// chunk's quads out of the emit pass's static upper-bound buffer (~786 KB)
	// into one sized to what actually exists (~10 KB), so a delivered chunk
	// waiting on the streaming apply budget does not pin the bound. Nothing
	// waits for it and correctness does not depend on it -- see
	// FVoxelGpuQuadPayload.
	void AddQuadCompactPass(FRDGBuilder& GraphBuilder, FRDGBufferRef Dst, FRDGBufferRef Src,
	                        uint32 SrcFirst, uint32 NumQuads);

	// Copies [SrcFirst, SrcFirst + NumQuads) of Src into DstQuads at DstFirst,
	// and writes ChunkId across the matching DstIds range in the SAME pass.
	//
	// This is the write that replaces UpdateQuadRange_RenderThread's two
	// Lock/Memcpy/Unlock pairs for a GPU-meshed chunk. Quads and ids move
	// together so the pool never holds geometry belonging to nobody.
	void AddQuadPoolWritePass(FRDGBuilder& GraphBuilder, FRDGBufferRef DstQuads, FRDGBufferRef DstIds,
	                          FRDGBufferRef Src, uint32 SrcFirst, uint32 DstFirst,
	                          uint32 NumQuads, uint32 ChunkId);

	// S2-1: writes HiddenChunkId across [DstFirst, DstFirst + NumQuads) of DstIds
	// and touches nothing else.
	//
	// Replaces the per-quad CPU loop in RemoveChunkInternal that stamped
	// QuadChunkIds and rode MarkQuadsDirty to the GPU. That loop was the LAST
	// writer of the CPU shadow on the GPU-only path, so removing it is what makes
	// dropping the shadow possible (S2-5) -- 12 B/quad of system RAM plus a
	// whole-array copy in CreateSceneProxy.
	//
	// ORDERING: recorded in the same graph and command as the write passes, and
	// BEFORE the chunk-table update, under the rule FPendingGpuWrite documents.
	// Where a hide and a same-frame GPU write cover the same range the hide is
	// subtracted first, so the two are disjoint by construction rather than by
	// pass order.
	void AddQuadPoolHidePass(FRDGBuilder& GraphBuilder, FRDGBufferRef DstIds,
	                         uint32 DstFirst, uint32 NumQuads, uint32 HiddenChunkId);

	// --- P1-C / P2: the four moves that make a packed chunk resident ---------
	//
	// Declared here for the same reason the quad passes are: the shader classes
	// live in VoxelGpuWorldGen.cpp (one IMPLEMENT_GLOBAL_SHADER per class, one
	// translation unit) and the caller is another one -- FVoxelBrickPool. See
	// ue-project/Shaders/VoxelBrickPoolWrite.usf for what each kernel does.
	//
	// ALL FOUR ARE RENDER THREAD ONLY, and the ORDER a caller records them in is
	// load-bearing exactly once: a chunk-record CLEAR for a slot being reused
	// must be recorded before the writes that repopulate it, so the retired
	// record can never be the last writer. FVoxelBrickPool does that in one
	// graph; see FVoxelBrickPool::FlushPendingWrites.

	// Copies [SrcFirst, SrcFirst + NumWords) of Src into Dst at DstFirst. Used
	// for both arenas -- occupancy and materials are the same dwords to a copy.
	void AddBrickWordCopyPass(FRDGBuilder& GraphBuilder, FRDGBufferRef Dst, FRDGBufferRef Src,
	                          uint32 SrcFirst, uint32 DstFirst, uint32 NumWords);

	// Copies BrickCount descriptors and ADDS THE POOL BASES TO THEIR OFFSET
	// FIELDS. The only place in the project where a chunk-relative offset
	// becomes a pool address -- see docs/brick-volume-format.md section 6b, and
	// the file header of VoxelBrickPoolWrite.usf.
	void AddBrickDescPoolWritePass(FRDGBuilder& GraphBuilder, FRDGBufferRef DstDesc, FRDGBufferRef SrcDesc,
	                               uint32 SrcFirst, uint32 DstFirst, uint32 BrickCount,
	                               uint32 OccBase, uint32 MatBase);

	// Writes the one 32 B FVoxelMarchChunk record. Reads the SCRATCH descriptors
	// and occupancy (chunk-relative, so it needs no base) plus the scratch L1
	// mask, and computes allSolid FROM THE CELL DATA -- it is not derivable from
	// the descriptors, and looks as though it is.
	void AddBrickChunkRecordPass(FRDGBuilder& GraphBuilder, FRDGBufferRef DstTable,
	                             FRDGBufferRef SrcDesc, FRDGBufferRef SrcOcc, FRDGBufferRef SrcChunkMask,
	                             uint32 SrcFirst, uint32 SrcChunkIndex, uint32 BrickCount,
	                             uint32 ChunkSlot, uint32 BrickBase, uint32 RingLevel,
	                             const FIntVector& OriginVoxel,
	                             const FVoxelBrickChunkShading& Shading);

	// Zeroes one record, which reads as "nothing here" (anySolid clear).
	void AddBrickChunkClearPass(FRDGBuilder& GraphBuilder, FRDGBufferRef DstTable, uint32 ChunkSlot);

	// --- the BATCHED pool flush (voxel.GPU.BrickFlushBatch) -----------------
	//
	// Table-driven twins of the four passes above: one dispatch per STAGE for a
	// whole fused group of chunks, instead of one per stage PER CHUNK. The
	// destinations are non-contiguous (the pool arenas are first-fit
	// suballocators), so each pass reads its addressing from a per-chunk table
	// the caller built -- see FVoxelBrickPool::AddFlushPasses_RenderThread for
	// the table's layout, construction and lifetime, and
	// VoxelBrickPoolWrite.usf for the kernel-side field map. All RENDER THREAD
	// ONLY, and the caller's clear-before-write recording rule is unchanged.

	// One arena's every fused run: thread per word over the group's summed
	// word count; TableFieldFirst selects the occupancy (2) or material (6)
	// field group so one kernel serves both arenas, as the classic copy does.
	void AddBrickFlushBatchWordCopyPass(FRDGBuilder& GraphBuilder, FRDGBufferRef Dst, FRDGBufferRef Src,
	                                    FRDGBufferRef Table, uint32 TableStride, uint32 TableFieldFirst,
	                                    uint32 NumEntries, uint32 TotalWords);

	// Every fused chunk's 64 descriptors, rebased MIXED-only, in one dispatch.
	void AddBrickFlushBatchDescWritePass(FRDGBuilder& GraphBuilder, FRDGBufferRef DstDesc,
	                                     FRDGBufferRef SrcDesc, FRDGBufferRef Table,
	                                     uint32 TableStride, uint32 NumEntries, uint32 BrickCount);

	// Every fused chunk's 64 B record: one workgroup per chunk, one dispatch.
	void AddBrickFlushBatchRecordPass(FRDGBuilder& GraphBuilder, FRDGBufferRef DstTable,
	                                  FRDGBufferRef SrcDesc, FRDGBufferRef SrcOcc,
	                                  FRDGBufferRef SrcChunkMask, FRDGBufferRef Table,
	                                  uint32 TableStride, uint32 NumEntries, uint32 BrickCount);

	// Every retired record of a flush in one dispatch. SlotList is a plain
	// stride-1 uint list of chunk slots.
	void AddBrickFlushBatchClearPass(FRDGBuilder& GraphBuilder, FRDGBufferRef DstTable,
	                                 FRDGBufferRef SlotList, uint32 NumSlots);

	// --- the batched flush's live cross-check -------------------------------
	//
	// One chunk, one workgroup, comparing what the batched kernels LANDED in
	// the pool against what the classic per-chunk kernels would have written.
	// The args are LOOSE per-chunk values filled from the same FPendingWrite
	// the classic passes read -- never from the flush table, because a verify
	// that reads the table it is checking follows any table bug to the wrong
	// address and passes. Mismatched dwords are accumulated into
	// OutVerify[0]; OutVerify[1] counts chunks checked.
	struct FBrickFlushVerifyArgs
	{
		uint32 SrcBrickFirst = 0;
		uint32 SrcChunkIndex = 0;
		uint32 BrickCount = 0;
		uint32 ChunkSlot = 0;
		uint32 BrickBase = 0;
		uint32 RingLevel = 0;
		uint32 OccBase = 0;
		uint32 MatBase = 0;
		uint32 OccSrcFirst = 0;
		uint32 MatSrcFirst = 0;
		uint32 OccWords = 0;
		uint32 MatWords = 0;
		FIntVector OriginVoxel = FIntVector::ZeroValue;
		FVoxelBrickChunkShading Shading;
	};
	struct FBrickFlushVerifyBuffers
	{
		// The pool's four arenas, read-only for the compare.
		FRDGBufferRef PoolDesc = nullptr;
		FRDGBufferRef PoolOcc = nullptr;
		FRDGBufferRef PoolMat = nullptr;
		FRDGBufferRef PoolTable = nullptr;
		// The producing dispatch's scratch: the reference bytes.
		FRDGBufferRef SrcDesc = nullptr;
		FRDGBufferRef SrcOcc = nullptr;
		FRDGBufferRef SrcMat = nullptr;
		FRDGBufferRef SrcChunkMask = nullptr;
		// Two dwords: [0] mismatches, [1] chunks checked. Caller clears it.
		FRDGBufferRef OutVerify = nullptr;
	};
	void AddBrickFlushVerifyPass(FRDGBuilder& GraphBuilder, const FBrickFlushVerifyBuffers& Buffers,
	                             const FBrickFlushVerifyArgs& Args);

	// --- P1 of the GPU streaming architecture: GPU-side pool allocation --------
	//
	// (voxel.GPU.PoolAlloc.) The generation graph claims its own occ/mat arena
	// ranges with atomics and writes words, descriptors and record IN THE SAME
	// GRAPH -- no totals readback, no CPU allocation, no later flush graph for
	// these chunks. See VoxelBrickPoolAlloc.usf's header for the whole allocator
	// design (size-class bump + free stacks, the page-bitmap double-grant gate,
	// and the one-allocator rule: both producers claim from THIS allocator).
	//
	// All RENDER THREAD ONLY, same standing as every other pass here.

	// The pool-side buffers every alloc-path pass binds. Filled by the caller
	// from FVoxelBrickPoolBuffers via RegisterExternalBuffer.
	struct FBrickPoolAllocBuffers
	{
		FRDGBufferRef PoolDesc = nullptr;    // written by the desc pass
		FRDGBufferRef PoolOcc = nullptr;     // written by the occ word copy
		FRDGBufferRef PoolMat = nullptr;     // written by the mat word copy
		FRDGBufferRef PoolTable = nullptr;   // written by the record / free passes
		FRDGBufferRef AllocState = nullptr;  // bumps + counters + free stacks
		FRDGBufferRef AllocBitmap = nullptr; // one bit per class page
		FRDGBufferRef AllocSide = nullptr;   // 4 dwords per chunk slot: the ranges

		bool IsValid() const
		{
			return PoolDesc && PoolOcc && PoolMat && PoolTable
			    && AllocState && AllocBitmap && AllocSide;
		}
	};

	// The arena/state-buffer layout is computed ONCE by FVoxelBrickPool::Init and
	// handed to every kernel as parameters -- never restated as literals on either
	// side (the ChunkRecordDwords rule). It is DEFINED in VoxelBrickPool.h (the
	// pool owns it and this private header already includes that public one);
	// aliased here so the pass signatures read in this namespace's vocabulary.
	using FBrickPoolAllocLayout = ::FVoxelBrickPoolAllocLayout;

	// One thread: totals -> class -> pop-or-bump -> bitmap -> side table + claim.
	// Returns the 8-dword claim buffer the three write passes consume. BrickTotals
	// is the region graph's own totals buffer -- the two dwords that used to cross
	// PCIe, now consumed where they were produced.
	// TotalsChunkIndexPlusOne (stack-claim, 2026-08-23): 0 = classic totals at
	// [0..1] of BrickTotals; n = fused-stack member n-1 -- the kernel reads its
	// pair at [2+2c..] of the stack's per-chunk totals (bPerChunkBrickTotals)
	// and derives its shared-scratch prefix in-kernel, which is what deletes
	// the (2+2K)-dword stack totals READBACK and the CPU-side prefix harvest.
	FRDGBufferRef AddBrickPoolClaimPass(FRDGBuilder& GraphBuilder,
	                                    const FBrickPoolAllocBuffers& Buffers,
	                                    const FBrickPoolAllocLayout& Layout,
	                                    FRDGBufferRef BrickTotals, uint32 ChunkSlot,
	                                    uint32 OccWorstWords, uint32 MatWorstWords,
	                                    uint32 TotalsChunkIndexPlusOne = 0,
	                                    uint32 TotalsNumChunks = 0);

	// The three writes: occ words, mat words (worst-case dispatch, actual count
	// read from the claim), descriptors (claim-based rebase), and the record
	// (zeroed on a failed claim). One call because their ordering relative to the
	// claim is the only thing a caller could get wrong.
	// SrcDescBase / ChunkMaskBase (stack-claim): a fused member's slice of the
	// SHARED batch scratch -- chunkIndex * bricks-per-chunk descriptors,
	// chunkIndex * 2 mask dwords. 0/0 on the classic single-chunk path, where
	// the whole scratch is the chunk's; the word-copy source prefix travels in
	// the claim itself (spare pair), not here.
	void AddBrickPoolAllocWritePasses(FRDGBuilder& GraphBuilder,
	                                  const FBrickPoolAllocBuffers& Buffers,
	                                  FRDGBufferRef Claim,
	                                  FRDGBufferRef SrcOcc, FRDGBufferRef SrcMat,
	                                  FRDGBufferRef SrcDesc, FRDGBufferRef SrcChunkMask,
	                                  uint32 BrickCount, uint32 ChunkSlot, uint32 BrickBase,
	                                  uint32 RingLevel, const FIntVector& OriginVoxel,
	                                  const FVoxelBrickChunkShading& Shading,
	                                  uint32 OccWorstWords, uint32 MatWorstWords,
	                                  uint32 SrcDescBase = 0, uint32 ChunkMaskBase = 0);

	// Eviction without a round trip: one thread per slot reads the side table,
	// pushes the ranges onto their class stacks, clears the bitmap, zeroes the
	// record and side entry. The CALLER owns the enqueue-order rule recorded on
	// BrickPoolFreeMain: every pending free lands before any batch that could
	// re-claim a freed slot.
	void AddBrickPoolFreePass(FRDGBuilder& GraphBuilder,
	                          const FBrickPoolAllocBuffers& Buffers,
	                          const FBrickPoolAllocLayout& Layout,
	                          FRDGBufferRef SlotList, uint32 NumSlots);

	// The sampled window cross-check. Expect: 8 dwords per entry
	// {slot, ox, oy, oz, level, brickBase, 0, 0}, from the CPU's resident map.
	// OutVerify: [0] mismatches (FAIL), [1] checked, [2] unwritten. Caller clears.
	void AddBrickPoolAllocVerifyPass(FRDGBuilder& GraphBuilder,
	                                 const FBrickPoolAllocBuffers& Buffers,
	                                 const FBrickPoolAllocLayout& Layout,
	                                 FRDGBufferRef Expect, uint32 NumEntries,
	                                 FRDGBufferRef OutVerify);
}
