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

		FRegionGraphSizes Sizes;
	};

	// Rejects anything the kernels' own guards would otherwise have to absorb.
	// Safe to call from any thread; it only reads the request.
	bool ValidateRegionRequest(const FVoxelGpuRegionRequest& Request, FString& OutError);

	// Pure arithmetic over the request. Assumes it already validated.
	FRegionGraphSizes ComputeRegionGraphSizes(const FVoxelGpuRegionRequest& Request);

	// Adds the seven passes to GraphBuilder and returns the buffers they wrote.
	// RENDER THREAD ONLY. Does not execute the graph, does not enqueue any
	// readback, and does not block -- the caller owns all three decisions.
	//
	// Request must stay alive for the duration of this call. It does NOT need to
	// outlive it: the raster arrays are copied into RDG's own allocator here
	// (CreateStructuredBuffer with the default ERDGInitialDataFlags copies), and
	// the loose parameters are plain scalars written into the parameter struct.
	FRegionGraphResources AddRegionPasses(FRDGBuilder& GraphBuilder, const FVoxelGpuRegionRequest& Request);

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
}
