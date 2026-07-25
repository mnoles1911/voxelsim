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
};

struct FVoxelGpuRegionResult
{
	bool bOk = false;
	FString Error;

	TArray<FVoxelGpuColumnSample> Columns;  // DispatchColumns.x * .y
	TArray<uint32> Cells;                   // one per voxel, material in low byte
	TArray<uint64> Quads;                   // packed word0 | word1 << 32

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
}
