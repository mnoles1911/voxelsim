// VoxelMarchBound.h -- THE PER-RAY RESIDENT-EXTENT BOUND'S PRODUCER
// (voxel.March.Bound, Stage 0b).
//
// One entry point. VoxelMarchRenderer.cpp calls it immediately before the
// march pass, only when the arm resolved on (rings on, sky ladder off, full
// res, cvar 1), inside the Bound timing bracket. It adds TWO passes:
//
//   VoxelMarch.BoundList   [cs]  one thread per pool chunk slot over
//                                VoxelBrickChunkTable -> per-level slot lists
//                                + per-level FRHIDrawIndirectParameters
//   VoxelMarch.BoundRaster [raster x SliceCount, indirect]  one cube per
//                                listed record into slice L of a
//                                Texture2DArray<float2> (RG32F), BO_Min both
//                                channels, storing (tNear, -tFar)
//
// and returns the texture, or NULL when it declined (pool never flushed, no
// capacity, degenerate target). THE NULL IS LOAD-BEARING: the caller keys the
// VOXEL_MARCH_BOUND permutation on it, because a bound kernel handed an
// unbound Texture2DArray reads zeros, zeros decode as EMPTY intervals, and an
// empty interval skips the walk -- i.e. the failure would delete the world.
// Null means the frame runs the byte-identical control instead.
//
// The soundness doctrine (why a superset of the chunk table bounds every
// possible hit, and why that is NOT residency-inferred emptiness) lives in
// VoxelMarchBound.ush; the consumer clamp and its debt fold live in
// VoxelBrickTraverse.ush's ZCut socket. This header is only the seam.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"

class FGlobalShaderMap;

// Everything the producer needs about the frame, COPIED FROM THE SAME
// AUTHORITIES THE MARCH DISPATCH BINDS (the caller fills the seven view
// fields from its FVoxelMarchViewParameters instance and the frame origin
// from the same local that fills MarchBrickOriginVoxel). A POD rather than a
// shared parameter-struct type because FVoxelMarchViewParameters is private
// to VoxelMarchRenderer.cpp; copying member-from-member at ONE call site,
// adjacent to the march's own fill, keeps a single authority without
// exporting shader-parameter machinery through a public header (the
// VoxelBrickPool.h self-containment rule).
struct FVoxelMarchBoundInputs
{
	// The ray model -- the uniforms VoxelMarchBuildRay reads, verbatim.
	FMatrix44f ViewToTranslatedWorld = FMatrix44f::Identity;
	FVector3f RayOriginLocalUU = FVector3f::ZeroVector;
	float VolumeExtentUU = 0.0f;
	FVector2f ViewRectMin = FVector2f::ZeroVector;
	FVector2f ViewRectSize = FVector2f(1.0f, 1.0f);
	FVector2f InvProjDiag = FVector2f(1.0f, 1.0f);
	FVector2f TemporalAAJitter = FVector2f::ZeroVector;
	// MarchPixelConeSlope: world-space pixel HALF-width per unit distance
	// along the ray, copied from the same MarchView block as the seven
	// above. The producer's half-res dilation is 4x this (the argument is
	// at the VS); a non-positive value makes half-res coverage unsound, so
	// the producer DECLINES on it rather than rendering an undilated bound.
	float PixelConeSlope = 0.0f;
	// The march frame origin in level-0 world voxels (MarchBrickOriginVoxel).
	FIntVector FrameOriginVoxel = FIntVector::ZeroValue;
	// How many ring levels get a slice -- the resolved ring count, clamped
	// inside to [1, 7] (kVoxelMarchMaxRings' value; the producer must never
	// out-slice VOXEL_MARCH_MAX_RINGS or the consumer's fixed-trip loop
	// cannot read what was written).
	int32 SliceCount = 0;
	// The march sample grid == the view rect size (half-res MARCHING is a
	// refused permutation). Since 2026-08-28 the producer's texture is HALF
	// of this per axis (rounded up) and the consumer loads texel
	// (pixel >> 1); this field stays full-res so there is exactly one
	// authority for the mapping, inside the producer.
	FIntPoint TargetSize = FIntPoint::ZeroValue;
};

// Returns the per-level bound texture, or null when the producer declined --
// see the header block for why null must switch the march to the control
// permutation rather than being bound anyway.
VOXELEARTHSHADERS_API FRDGTextureRef VoxelMarchBoundProduce(
	FRDGBuilder& GraphBuilder, const FGlobalShaderMap* ShaderMap,
	const FVoxelMarchBoundInputs& Inputs);

// ---------------------------------------------------------------------------
// The frustum cull's engagement window (2026-08-28, the boundMs mitigation)
// ---------------------------------------------------------------------------
//
// Summed from the producer's own readback ring as slots land (render
// thread), drained by the bound engagement line's printer (game thread) on
// the same 5 s cadence as every other window in this workstream. The
// identity considered == culled + sum(drawn) is checkable because all three
// come from the SAME dispatch's two buffers, read back side by side.
struct FVoxelMarchBoundCullStats
{
	uint64 Considered = 0;                 // slots at a sliced level, pre-cull
	uint64 Culled = 0;                     // slots the plane test rejected
	uint64 DrawnPerLevel[7] = {};          // the indirect args' InstanceCounts
	uint64 Frames = 0;                     // readbacks landed into the window
	// The shader's own mirror of MarchBoundCullEnable from the LAST landed
	// frame: 0 here with Culled == 0 is "the CPU disabled the cull
	// (degenerate frustum)" and must never print as "nothing was outside
	// the frustum" -- an arm that cannot show it fired is not an arm.
	uint32 LastEnable = 0;
	bool bEverLanded = false;
};

// Drain-and-reset, the GetAndReset pattern every 5 s window here uses. Game
// thread. Zeros with Frames == 0 are "no sample landed", never a reading.
VOXELEARTHSHADERS_API FVoxelMarchBoundCullStats VoxelMarchBoundGetAndResetCullStats();
