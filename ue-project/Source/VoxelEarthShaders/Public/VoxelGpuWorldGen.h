// GPU worldgen + greedy mesher, run through Unreal's RDG (ADR-0006, G2a).
//
// This is the in-engine counterpart of voxel-core/bench/gpu_harness.cpp. Both
// dispatch the SAME kernels from the SAME source file (voxel-core/shaders/
// worldgen.ush); the bench compiles them with standalone DXC and runs them on
// raw Vulkan, this compiles them through Unreal and runs them on whatever RHI
// the editor is using.
//
// The point of having both is that the bench owns a proven bit-exactness gate
// against the CPU mesher. Running the identical kernels here lets us ask a much
// narrower question — "does Unreal's compiler and binding produce the same
// bytes?" — instead of re-litigating whether the kernels are correct.
//
// NOTHING HERE RENDERS. The output is read straight back to the CPU so it can
// be compared. Drawing GPU-resident geometry is G2, and it depends on this
// being green first.

#pragma once

#include "CoreMinimal.h"

// Mirror of GpuColumnSample in worldgen.ush — the per-column stratigraphy the
// ColumnMain kernel writes. Field order and types must match the HLSL struct
// exactly; the .cpp static_asserts the size.
struct FVoxelGpuColumnSample
{
	int32 SurfaceMm = 0;
	int32 TopsoilMm = 0;
	int32 SubsoilMm = 0;
	int32 BedrockDepthMm = 0;
	uint32 SurfaceMat = 0;
};

// Everything the kernels need for one region dispatch. The caller is
// responsible for sizing the raster window correctly — see the comment on
// ElevationMm.
struct FVoxelGpuRegionRequest
{
	// Columns dispatched along x and y. Must both be multiples of 8 (the
	// kernels work in 8x8x8 bricks) and give at least 3 bricks per axis, or
	// the mesh chain has no interior brick to mesh.
	FUintVector2 DispatchColumns = FUintVector2(64, 64);

	// World voxel coordinate of dispatch column (0,0).
	int32 OriginVx = 0;
	int32 OriginVy = 0;

	// The elevation/climate raster window the kernels bilinearly sample.
	//
	// SIZING IS THE CALLER'S JOB AND IT IS NOT OPTIONAL. The window must cover
	// every pixel any thread in this dispatch can tap, including the cavern
	// pass's extra reach (it evaluates terrain height at a cave site's own xy,
	// which can lie well outside the dispatch footprint). The kernels clamp
	// out-of-window reads deterministically as a backstop, but a clamped read
	// means the window was sized wrong and the output will differ from the CPU
	// reference — which is exactly what the verification is looking for.
	FIntPoint RasterOriginPx = FIntPoint::ZeroValue;
	FUintVector2 RasterSize = FUintVector2(0, 0);
	int32 PixelSizeMm = 30000;
	TArray<int32> ElevationMm;      // RasterSize.x * RasterSize.y, x fastest
	TArray<uint32> ClimatePacked;   // same layout; t | s<<8 | p<<16 | v<<24

	// Vertical extent of the voxelized stack, in bricks.
	int32 BrickZMin = 0;
	uint32 BricksZ = 0;

	uint64 Seed = 0;

	// Skip the mesh chain and stop after voxelization. Useful to isolate a
	// failure to the generation half.
	bool bMeshChain = true;

	// --- Wave D / D2: the chunk-local emit permutation ----------------------
	//
	// Selects FVoxelMeshEmitCS's VXC_MESH_CHUNK_LOCAL permutation, which bakes
	// each interior brick's chunk-local origin into the emitted quad positions.
	// The quad stream then needs no rebase: it is already in the 0..31
	// chunk-local coordinates MeshChunkBricks produces and the geometry pool
	// consumes.
	//
	// MUST STAY FALSE ON THE DETERMINISM GATE. The digest folds in the packed
	// quad FIELDS, and slice/u0/v0 are quad fields, so flipping this changes
	// the digest for geometry that is identical. voxel.GPU.VerifyRegion is
	// pinned against the false permutation; voxel.GPU.VerifyAsyncMesh (a byte
	// compare against the shipping CPU mesher) is what gates the true one.
	bool bChunkLocalQuads = false;

	// First slot in the quad buffer this dispatch may write, so several chunks
	// can emit into disjoint slices of one buffer. Only honoured under
	// bChunkLocalQuads — the kernel reads it only in that permutation — so
	// ValidateRegionRequest REJECTS a non-zero base without it rather than let
	// it be silently ignored.
	uint32 QuadWriteBase = 0;

	// --- Wave D / D6: the footprint band ------------------------------------
	//
	// BandEdge 0 (the default) skips the band pass entirely. Set it to the
	// column-grid edge the CPU uses (ChunkEdgeVoxels + 2 = 34) and BandOriginI
	// to the dispatch-column index that grid starts at, and the graph reduces
	// the band on the GPU instead of the streaming job doing a 34x34 column
	// pass on a worker thread -- ~45% of level-0 job time.
	//
	// Only one chunk per (X,Y) footprint needs to ask for it, which is why it
	// is opt-in per request rather than implied by bMeshChain.
	uint32 BandOriginI = 0;
	uint32 BandEdge = 0;
};

struct FVoxelGpuRegionResult
{
	bool bOk = false;
	FString Error;

	TArray<FVoxelGpuColumnSample> Columns;  // DispatchColumns.x * .y
	TArray<uint32> Cells;                   // one per voxel, material in low byte

	// Packed quads, word0 | word1 << 32.
	//
	// Everything below describes the DEFAULT (bChunkLocalQuads == false)
	// permutation. Under bChunkLocalQuads the coordinates are already
	// chunk-local and none of the rebase discussion applies — the shader has
	// done it, and the first NumQuads entries starting at QuadWriteBase are
	// pool-ready as they stand.
	//
	// IMPORTANT: these coordinates are BRICK-LOCAL. greedyMask packs slice/u0/v0
	// as positions inside a single 8x8x8 brick; which brick is implied by the
	// mask index and is deliberately NOT stored in the quad. So a quad here
	// cannot be positioned in the world on its own -- every brick's geometry
	// would pile up inside the same 8-voxel cube.
	//
	// The CPU mesher adds the brick offset when it converts vxc::Quad to
	// FVoxelChunkQuad. Anything that draws these must do the equivalent; see
	// QuadCounts/QuadOffsets below, which is what makes that possible.
	//
	// The digest gate is unaffected either way, because it compares brick by
	// brick in the same order -- which is exactly why this can be got wrong
	// without the gate noticing.
	TArray<uint64> Quads;

	// Per-mask quad counts and their exclusive-scanned start offsets, one entry
	// per face-mask. maskIndex = meshBrickIndex * 48 + axis * 16 + dir * 8 +
	// slice, so these are what let a caller map a quad back to the brick it
	// came from and re-base its coordinates.
	TArray<uint32> QuadCounts;
	TArray<uint32> QuadOffsets;

	// Quads is sized to the upper bound (32 per mask); this is how many the
	// scan says are actually live. Only the first NumQuads entries are output.
	uint32 NumQuads = 0;
};

namespace VoxelGpuWorldGen
{
	// Runs ColumnMain -> VoxelizeMain -> MeshCount -> Scan -> MeshEmit and reads
	// the results back.
	//
	// SYNCHRONOUS AND SLOW ON PURPOSE. It blocks the calling thread until the
	// GPU is idle and the readback has landed. This is a verification and
	// sizing tool, not a streaming path — do not call it from a hot loop. The
	// streaming path (G3) keeps everything on the GPU and never reads back.
	//
	// Safe to call from the game thread.
	VOXELEARTHSHADERS_API FVoxelGpuRegionResult RunRegionBlocking(const FVoxelGpuRegionRequest& Request);

	// True when the current RHI can actually run these kernels. The worldgen
	// math is 64-bit integer throughout (hashing and world coordinates), which
	// needs SM6 with 64-bit integer shader ops.
	VOXELEARTHSHADERS_API bool IsSupportedOnCurrentRHI();

	// One decoded corner, as DecodeVoxelQuadVertex produced it.
	struct FDecodedVertex
	{
		float PositionUU[3];
		uint32 AmbientOcclusion;  // 0..3, as packed in the quad
		uint32 MaterialId;
	};

	// Runs the packed-quad decode over Quads and returns 6 vertices per quad.
	//
	// This is a TEST HOOK, not part of the draw path. The vertex factory calls
	// the same DecodeVoxelQuadVertex during rendering; this runs it in
	// isolation so its output can be compared against a CPU reference without
	// drawing anything. Blocking, same as RunRegionBlocking.
	VOXELEARTHSHADERS_API bool DecodeQuadsBlocking(const TArray<uint64>& Quads,
	                                               float LevelScale,
	                                               TArray<FDecodedVertex>& OutVertices,
	                                               FString& OutError);
}
