// One chunk's quads, left where the GPU wrote them (ADR-0006, Wave D / D1).
//
// WHAT THIS IS FOR. Until D1 the quad stream made a round trip through system
// memory that it did not need: the mesh chain wrote quads into a GPU buffer,
// phase 2 read them back (~10 KB per chunk over PCIe), the game thread unpacked
// them into FVoxelChunkQuad, ApplyMeshResult packed them straight back into the
// same 8 bytes, and PushUpdatesToProxy uploaded those bytes to the GPU again.
// Every quad crossed PCIe twice and was copied on both the game and the render
// thread, to arrive byte-identical to what the GPU already had.
//
// This handle is what replaces all of that. A completed job hands one of these
// to the streaming path instead of a TArray<uint64>; ApplyMeshResult passes it
// to UVoxelGpuPoolComponent::AddChunkFromGpu, which allocates a pool range and
// dispatches a compute pass copying the quads GPU-side into that range. The
// only thing that still crosses PCIe is the 4-byte quad total D3 already reads.
//
// THREADING, AND IT IS THE PART TO GET RIGHT. The two buffer fields are
// RENDER-THREAD-OWNED after construction. The game thread constructs the
// payload (it holds the scratch buffer reference the manager already has), then
// only ever MOVES THE HANDLE AROUND -- into FJobResult, through the results
// queue, into the pool component. It must never dereference Quads. Two render
// commands touch the fields, and render commands run in the order the game
// thread enqueued them:
//
//   1. the compaction pass (FVoxelGpuMeshJobManager phase 2) REPLACES Quads
//      with an exactly-NumQuads-sized buffer and sets SrcFirst to 0, dropping
//      the ~786 KB upper-bound scratch buffer;
//   2. the pool write pass (UVoxelGpuPoolComponent) READS them.
//
// CORRECTNESS DOES NOT DEPEND ON STEP 1. If the compaction never ran, the pool
// write still reads the right quads out of the scratch buffer at SrcFirst.
// Compaction buys MEMORY, not correctness: without it a payload pins 786 KB
// (MaskCount 3,072 x 32 quads x 8 B, the emit pass's static upper bound) from
// delivery until the streaming apply budget gets to it, and deliveries outrun
// applies under load -- the apply loop is bounded by a 6.0 ms wall-clock budget
// (voxel.Stream.ApplyBudgetMs) under a 64-per-frame ceiling, while the fork has
// been measured delivering ~850 chunks/s. That queue is not bounded by the
// fork's in-flight cap, because a delivered job has already left it. Compacted,
// a pending payload is the chunk's real size, ~10 KB.

#pragma once

#include "CoreMinimal.h"
#include "Templates/RefCounting.h"

class FRDGPooledBuffer;

// Deliberately NOT copied by value anywhere: the destructor releases an RHI
// resource reference and must do it on the render thread.
struct VOXELEARTHSHADERS_API FVoxelGpuQuadPayload
{
	// Out of line like the destructor, and for the same incomplete-type reason:
	// an in-class `= default` here makes MSVC instantiate ~TRefCountPtr for the
	// constructor's unwind path in every including TU, which fails wherever
	// FRDGPooledBuffer is only the forward declaration above (measured: the
	// module's .gen.cpp). Defaulted in the .cpp, where the type is complete.
	FVoxelGpuQuadPayload();

	// Drops the buffer reference ON THE RENDER THREAD, for the same reason
	// UVoxelGpuPoolComponent::BeginDestroy does: this is the last reference to an
	// RHI resource that render commands may still be a frame or two behind on,
	// and releasing it wherever the payload happens to die fails as a crash on
	// exit rather than as anything a compiler catches.
	//
	// Out of line so this header only needs a forward declaration of
	// FRDGPooledBuffer -- FRDGBufferRef and friends are render-thread-and-a-live-
	// FRDGBuilder concepts that have no business on a module's public surface
	// (see the header comment on VoxelGpuWorldGenGraph.h). A pooled buffer is a
	// PERSISTENT resource, so it is the one RDG type that is legitimately
	// shareable across graphs and threads.
	~FVoxelGpuQuadPayload();

	FVoxelGpuQuadPayload(const FVoxelGpuQuadPayload&) = delete;
	FVoxelGpuQuadPayload& operator=(const FVoxelGpuQuadPayload&) = delete;

	// RENDER THREAD ONLY after construction. See the threading note above.
	TRefCountPtr<FRDGPooledBuffer> Quads;
	// First element of Quads that belongs to this chunk. The emit pass writes at
	// QuadWriteBase, which is 0 on every path today; the compaction pass rebases
	// to 0. Kept rather than assumed because the alternative is an off-by-a-base
	// bug that produces plausible geometry from the wrong offset.
	uint32 SrcFirst = 0;

	// How many quads are live. Game-thread readable -- this is the count the pool
	// allocation, ResidentQuads and every census counter are sized from, and it
	// is the ONLY thing about the quads the CPU still knows.
	uint32 NumQuads = 0;
};

using FVoxelGpuQuadPayloadRef = TSharedPtr<FVoxelGpuQuadPayload, ESPMode::ThreadSafe>;
