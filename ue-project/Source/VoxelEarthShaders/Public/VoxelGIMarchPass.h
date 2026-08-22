// Phase 7: the GI cone march as a GPU pass over the resident brick pool.
//
// The shader is ue-project/Shaders/VoxelGIMarch.usf and its header carries the
// design -- why the walk defaults to FLAT, why this reads neither the ring
// cascade nor a new pyramid, and what v1 deliberately does not do (no bounce,
// binary per-cone visibility). This file owns only the dispatch.
//
// THE SEAM IS DELIBERATELY ONE FUNCTION AND A PLAIN STRUCT. UVoxelGISubsystem
// lives in VoxelEarth and this pass lives in VoxelEarthShaders; the dependency
// runs VoxelEarth -> VoxelEarthShaders and never the reverse (see
// VoxelEarth.Build.cs). So the subsystem hands over a fully-resolved request
// and learns nothing about RDG, and this module learns nothing about the light
// field.
//
// EVERYTHING IN THE REQUEST IS ALREADY RESOLVED, IN DOUBLE, ON THE GAME THREAD.
// Nothing here re-derives a world position: a GI cell coordinate at this
// world's scale is ~150,000 and float32 quantises it coarser than the 40 UU
// cell it addresses. That is the same precision seam MarchBrickOriginVoxel
// exists for, and the same one the shadow march differences in double before
// handing a float3 to its shader.

#pragma once

#include "CoreMinimal.h"

// The traced-cone basis, restated as sizes only. The VALUES come from
// VoxelLF::TraceDirTable / VoxelLF::SlotWeight in VoxelLightField.cpp, which is
// the single definition; these two constants exist so this header can size the
// arrays without including a VoxelEarth header (the dependency runs the other
// way). FVoxelGIMarchRequest::Validate() checks them against what the caller
// filled, and the shader's own GIMARCH_NUM_* defines must agree -- three
// places, one number, checked rather than assumed.
namespace VoxelGIMarch
{
	inline constexpr int32 kNumTraceDirs = 14;
	inline constexpr int32 kNumSlots = 6;
	inline constexpr int32 kBrickEdgeCells = 8;

	// Matches GIMARCH_TILE in the .usf. A brick is 8 cells per axis, so a tile
	// of 4 divides it exactly and the dispatch needs no bounds slack in X/Y.
	inline constexpr int32 kTile = 4;

	// Stats words the shader may write. Bound every dispatch because the
	// parameter map names the buffer unconditionally; written only when
	// bStatsEnabled, and NEVER read back -- there is no readback path in this
	// pass and no counter derived from it. The arm is reported from the game
	// thread's own brick count, through the GI arm line that already exists.
	inline constexpr int32 kStatsWords = 8 + kNumTraceDirs;
}

// One GI brick to solve this dispatch.
struct FVoxelGIMarchBrick
{
	// The brick's minimum corner, in the pool-local UU frame anchored at
	// FVoxelGIMarchRequest::FrameOriginVoxel. Resolved on the game thread.
	FVector3f CornerLocalUU = FVector3f::ZeroVector;
	// Where that corner lands in the GI volume, in texels. The shader DROPS a
	// brick whose texels fall outside the volume rather than clamping: clamping
	// folds one edge of the volume onto the other, which renders as
	// correct-looking lighting in the wrong place.
	FIntVector TexelMin = FIntVector::ZeroValue;
};

struct FVoxelGIMarchRequest
{
	TArray<FVoxelGIMarchBrick> Bricks;

	// The local frame, in LEVEL-0 voxel coordinates, snapped down to a 32-voxel
	// chunk boundary. Constructed by the CALLER exactly as VoxelShadowMarch.cpp
	// constructs its own: a pure function of the pose with no hysteresis and no
	// history, sized to hold every ray this dispatch can fire. The chunk index
	// is a wrapped grid, so any origin is legal as long as the marched span
	// stays inside it.
	FIntVector FrameOriginVoxel = FIntVector::ZeroValue;

	int32 VolumeDim = 0;

	float CellSizeUU = 40.0f;
	float ConeReachUU = 3000.0f;
	float StartOffsetUU = 10.0f;
	float ConeSlopeUU = 0.0f;
	float SkyIntensity = 1.0f;
	int32 StepBudget = 886;

	// 0 = flat, 1 = hierarchical, 2 = both-and-compare. See the .usf header.
	// THE DEFAULT IS FLAT AND THAT IS A DECISION, not an oversight: the
	// hierarchical walk has an open, unexplained, NON-DIRECTIONAL burst
	// phenomenon against a frozen world hash, and in a cone march a walk that
	// intermittently misses content reads as light leaking through a wall.
	int32 Walk = 0;

	bool bStatsEnabled = false;

	// Cone directions (unit) and the per-slot normalised cosine weights, copied
	// from the single CPU definition. Copied rather than referenced because the
	// request crosses to the render thread and must own everything it names.
	FVector3f TraceDir[VoxelGIMarch::kNumTraceDirs];
	float SlotWeight[VoxelGIMarch::kNumSlots][VoxelGIMarch::kNumTraceDirs];

	// Cheap structural self-check. Returns false, with a reason, for the
	// mistakes that would otherwise dispatch and render as lighting: an empty
	// or oversized brick list, a zero volume dim, a non-finite frame, a cone
	// reach of zero, or a slot-weight row that does not sum to ~1 (which is the
	// property that makes an unoccluded cell solve to exactly 1.0 and therefore
	// keeps open terrain identical between the two arms).
	//
	// THE WEIGHT-SUM CHECK IS THE LOAD-BEARING ONE. A basis whose rows do not
	// sum to 1 does not fail: it makes everything uniformly brighter or darker,
	// which is indistinguishable from a GI tuning change and would be argued
	// about rather than fixed.
	bool Validate(FString& OutWhy) const;
};

namespace VoxelGIMarch
{
	// Queues one dispatch. GAME THREAD.
	//
	// Returns the number of bricks accepted -- 0 means nothing was queued and
	// the caller must not report the arm as having run. It refuses, rather than
	// dispatching, when the request does not validate; the reason is logged
	// once per distinct cause.
	//
	// A RETURN OF >0 IS NOT A PROMISE THAT THE GPU RAN IT. The render-thread
	// half can still refuse -- the volumes may not be UAV-capable
	// (voxel.GI.VolumeUAV was 0 at startup) or the brick pool may have nothing
	// resident yet. Both of those log at Warning from where they happen and
	// neither is silent. This split is deliberate: the game thread reports what
	// it SUBMITTED and never guesses what the GPU did with it.
	VOXELEARTHSHADERS_API int32 Enqueue_GameThread(FVoxelGIMarchRequest&& Request);
}
