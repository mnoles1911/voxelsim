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
}
