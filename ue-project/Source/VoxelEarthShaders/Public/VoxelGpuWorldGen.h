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

#include "VoxelBrickPool.h"   // FVoxelBrickChunkShading

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
	// WAVE 2. The per-chunk shading this region's chunk record will carry.
	// Travels on the REQUEST because the record is written on the GPU at job
	// completion, long after the game thread that sampled the climate is gone --
	// the same reason the region already carries its own origin and level.
	FVoxelBrickChunkShading BrickShading;

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

	// --- P1-C: the resident brick volume ------------------------------------
	//
	// Runs BrickClassifyMain -> Scan x2 -> BrickPackMain over the SAME Cells
	// buffer the mesh chain reads, immediately after the asset stamp, in the
	// same graph. Additive: with this false the graph is byte-for-byte the one
	// that shipped, and nothing about the mesh chain changes when it is true.
	//
	// THE REGION MUST DECOMPOSE INTO WHOLE RENDER CHUNKS FROM BRICK ZERO --
	// DispatchColumns multiples of 32 and BricksZ a multiple of 4 -- because
	// brickpack.ush's decodeBrick has no brick origin: chunk c of the dispatch
	// is bricks [4c, 4c+4) counted from the region's own corner. That is NOT
	// the mesher's 48x48x6 footprint, whose interior bricks start at brick 1,
	// so a job that wants both dispatches this on a SECOND, halo-free region
	// (VoxelGpuChunkRegion::MakeBrickRegion). ValidateRegionRequest refuses a
	// region of the wrong shape rather than silently packing the halo corner --
	// which would produce a complete, self-consistent, one-brick-displaced
	// world.
	//
	// docs/brick-volume-format.md is the byte contract, and
	// docs/ray-marching-plan-2026-08-19.md section 8 is why the halo goes away:
	// a marcher reads neighbours by index, so it needs no apron, and the
	// dispatch is 3.375x less voxelize work per chunk than the mesher's.
	bool bBrickPack = false;

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
	// D5: coarse level. 0 = level 0 and is the identity in the kernel's
	// coarseRep(), so an unset request is byte-for-byte the pre-D5 dispatch.
	// At level L > 0, OriginVx/OriginVy and BrickZMin are in LEVEL-L cell units
	// -- exactly as vxc::coarseColumns and makeCoarseBrick take them -- and the
	// kernel maps each to its representative level-0 coordinate.
	int32 CoarseLevel = 0;

	// D5.3: ring-boundary skirt, bit per lateral face (1=-X 2=+X 4=-Y 8=+Y),
	// mirroring ComputeRingSkirtMask. Non-zero is REJECTED on any region that is
	// not exactly one chunk -- see regionCellMat in worldgen.ush for why that
	// refusal is what makes a single mask sound rather than sound-for-now.
	uint32 RingSkirtMask = 0;

	uint32 BandOriginI = 0;
	// Y origin of the band window. Production sets it equal to BandOriginI (the
	// window is symmetric); it exists so a single-column PROBE can sit anywhere
	// in the dispatch instead of only on the diagonal. See VoxelBandReduce.usf.
	uint32 BandOriginJ = 0;
	uint32 BandEdge = 0;

	// --- Asset compose ------------------------------------------------------
	//
	// Terrain-lattice asset instances stamped into Cells between VoxelizeMain
	// and the mesh chain, one dispatch per instance IN ARRAY ORDER -- that
	// order is the byte-parity contract with the CPU's first-non-air-wins
	// compose (AssetField::materialAtResolved at level 0,
	// FCoarseChunkGridSampler's shortlist walk at CoarseLevel > 0), so the
	// caller must fill it from resolveForCompose order and nothing may sort
	// it. Empty arrays skip the pass entirely, which is what keeps the
	// verify/digest path (which never fills them) terrain-only by
	// construction.
	//
	// AT CoarseLevel > 0 the stamp runs the rep-coordinate GATHER kernel
	// (AssetStampCoarseMain): each level-L cell samples the instance at its
	// coarseRep level-0 coordinate, nearest-neighbour, exactly as the terrain
	// around it is sampled. The anchor fields keep their LEVEL-0 VOXEL units
	// at every level; only the base they are made relative to follows the
	// region: AnchorRelVx = anchorVx - OriginVx * 2^CoarseLevel (OriginVx is
	// in level-L cell units, so OriginVx * 2^L is the region's level-0 base
	// -- at level 0 this is the same subtraction it always was). AnchorVz
	// stays absolute. Everything else -- origins, yaw, sizes, span tables --
	// is filled identically at every level.
	//
	// AssetSpans packs one RLE run as z0:12 | len:12 | mat:8. AssetColStarts
	// holds, per instance, SizeX*SizeY+1 prefix offsets into AssetSpans
	// (instance base recorded in ColStartsBase). Grids repeated within a job
	// share one span block -- the caller dedups by (bankId, seedIndex).
	struct FAssetInstance
	{
		int32 AnchorRelVx = 0;   // anchor voxel minus OriginVx * 2^CoarseLevel
		int32 AnchorRelVy = 0;   // anchor voxel minus OriginVy * 2^CoarseLevel
		int32 AnchorVz = 0;      // absolute anchor voxel z
		int32 GridOriginZ = 0;   // AssetGrid::originZ()
		int32 RotOriginX = 0;    // AssetGrid::rotatedOriginX(YawQuarter)
		int32 RotOriginY = 0;    // AssetGrid::rotatedOriginY(YawQuarter)
		uint32 YawQuarter = 0;
		uint32 SizeX = 0;        // BAKED box extents (pre-rotation)
		uint32 SizeY = 0;
		uint32 SizeZ = 0;        // host-validated <= 4095 (span packing)
		uint32 ColStartsBase = 0;
	};
	TArray<FAssetInstance> AssetInstances;
	TArray<uint32> AssetColStarts;
	TArray<uint32> AssetSpans;
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

	// --- Wave D / D6: the footprint band ------------------------------------
	//
	// RAW voxel z, exactly as BandReduceMain wrote them: the max of
	// ColumnSurfaceTopVoxel and the min of ColumnDeepestAirVoxel over the band
	// window. NOT an FFootprintBand -- the +1/-1 widening and the int32 clamp
	// that turn these into one live in VoxelStreaming::MakeFootprintBand, on the
	// CPU, because every constant mirrored into HLSL is a determinism liability.
	//
	// bBandValid is false whenever the request did not ask for a band
	// (BandEdge 0), and the two values are then meaningless rather than zero.
	bool bBandValid = false;
	int32 BandMaxSurfaceTopVoxel = 0;
	int32 BandMinDeepestAirVoxel = 0;
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
