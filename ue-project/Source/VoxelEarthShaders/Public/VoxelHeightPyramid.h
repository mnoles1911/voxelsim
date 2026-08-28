#pragma once
// VoxelHeightPyramid.h -- a max-reduced 2D upper bound on terrain height, and
// the ONE rule that decides whether it is safe: ABSENCE IS +INFINITY.
//
// ===========================================================================
// WHAT THIS IS
// ===========================================================================
//
// A square, world-axis-aligned field of floats. Cell (cx, cy) at mip 0 holds a
// PROVABLE UPPER BOUND, in UE world Z units (UU, i.e. cm), on the top of the
// terrain anywhere inside the footprint of the level-`LeafChunkLevel` chunk
// column (cx, cy) -- crowns, detail octaves and all. Mip m holds the MAX over
// the 2x2 block of mip m-1, so a mip cell is a sound bound over its whole
// footprint too.
//
// The marcher uses it to prove a stretch of ray is in air: if the ray's Z stays
// strictly above the cell's bound while it crosses that cell, there is nothing
// in the cell to hit and the walk can jump the crossing outright. One DDA over
// the pyramid answers all three directions -- a rising ray terminates once it
// is above everything left, a level ray steps over a valley in coarse jumps,
// and a descending ray jumps straight to first contact.
//
// ===========================================================================
// WHY THE LEAF IS BUILT AND THE COARSE LEVELS ARE REDUCED, NOT QUERIED
// ===========================================================================
//
// This is the load-bearing implementation detail and it is worth 49x. The
// underlying bound (vxc::Amplifier::surfaceUpperBoundMm, via
// FVoxelWorldImpl::FootprintSurfaceUpperBoundMm) is a Lipschitz envelope around
// ONE centre evaluation, so its slack grows superlinearly with the footprint it
// is asked about. Measured on the real fine tier at the real leg spawn
// (-61440,-61440), docs/marcher-height-pyramid-design.md section 3.2:
//
//     footprint     direct query slack
//     3.2 m         3.84 m
//     12.8 m        9.68 m
//     102.4 m       188.83 m        <-- asking directly for a coarse footprint
//
// A max of sound upper bounds over a partition is a sound upper bound over the
// union, and it is tight to the tightest leaf. So a pyramid built at 12.8 m and
// max-reduced upward carries 9.68 m of slack all the way to its coarsest level,
// where a direct query would have answered 188.83 m.
//
// NEVER call the amplifier for a coarse footprint to fill a coarse mip. Fill
// mip 0 and reduce.
//
// ===========================================================================
// ABSENCE IS +INFINITY. THIS PROJECT HAS SHIPPED TERRAIN DELETION TWICE FROM
// EXACTLY THE OPPOSITE CHOICE.
// ===========================================================================
//
// tilestore.cpp:1759 returns SEA LEVEL for a missing block, and a missing fine
// tile therefore "reads as AIR". Both incidents had the same shape: a height
// that was absent got a plausible low number instead of a refusal, the code
// downstream proved air where there was mountain, and the mountain stopped
// being drawn with every counter healthy.
//
// So, without exception:
//
//   1. The value type is R32_FLOAT and an unbuilt, declined, non-resident or
//      out-of-field cell holds +INF -- literally asfloat(0x7F800000). Not a
//      large finite number. Not zero. Not sea level.
//   2. The array is CLEARED TO +INF, never to 0. Clear() below is the only
//      place that clears and it is the reason this class owns the storage.
//   3. Max-reduction propagates +INF for free (max(x, INF) == INF), so one
//      absent leaf poisons its ancestors automatically -- which is the correct
//      direction. No sentinel handling anywhere in the reduce.
//   4. The shader's air test is `RayZ > H`, which is FALSE for H = +INF for
//      free: no sentinel comparison, no branch, and no way to invert the test
//      by accident. That is why +INF is right and a sentinel like -1 is not.
//   5. The bound is ANALYTIC and must never be derived from residency. A cell
//      whose data has not streamed yet holds +INF and nothing else.
//
// The one thing the analytic bound cannot see is a PLAYER EDIT: a block set on
// a hilltop is above the amplifier's surface and the amplifier does not know.
// MaxLeafEdited() is how the edit path folds EditedFootprintMaxZ back in, and
// the builder must call it or gate the feature off. See the note there.
//
// ===========================================================================
// INDEXING: LINEAR, NOT TOROIDAL, AND THAT IS DELIBERATE
// ===========================================================================
//
// The raster atlas re-anchors toroidally to avoid refilling. This does not,
// because a toroidal read that escapes its valid rectangle aliases onto a cell
// 13 km away -- and a WRONG height here is not a blurred texel, it is proven
// air over real ground. A linear index plus an explicit rectangle test makes
// out-of-field structurally unrepresentable: the test that decides the index is
// valid is the same test that decides the read is in range, and the answer
// outside is +INF.
//
// The cost of that choice is that a re-anchor refills. It is paid down by
// re-anchoring on hysteresis (the field is far wider than the march reach) and
// by filling on a per-tick millisecond budget, with the partially-filled field
// SAFE to consult throughout because everything unfilled is still +INF.

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "RenderGraphFwd.h"

class FRDGBuilder;
class FRDGPooledBuffer;

// The bound's own refusal, mirrored as a float. Written as a bit pattern rather
// than as a division or a literal so it cannot be turned into a finite number
// by a fast-math flag.
VOXELEARTHSHADERS_API float VoxelHeightPyramidPositiveInfinity();

class VOXELEARTHSHADERS_API FVoxelHeightPyramid
{
public:
	// Eight mips over a 1024-cell leaf: 1024, 512, ..., 8. The coarsest cell is
	// 128 leaf cells on a side, which at a 12.8 m leaf is 1,638.4 m -- coarse
	// enough that a sky ray clears the whole field in a handful of steps.
	static constexpr int32 kMaxMips = 8;

	// THE FIELD'S WORLD SPAN, FIXED, AND WHY IT IS WIDER THAN THE MARCH REACH.
	// The reach is 8,396 m. A field exactly that wide would have to re-anchor on
	// almost every step the camera takes, and each re-anchor is a full refill.
	// 13,107.2 m leaves ~2.3 km of slop in every direction, which is what makes
	// the hysteresis in ShouldReanchor() cheap.
	static constexpr double kFieldSpanUU = 1310720.0;

	// Fill granularity. A tile is filled atomically by the builder so that
	// progress is always a whole number of tiles and a torn tile is not a state
	// the field can be in.
	static constexpr int32 kTileLeafCells = 64;

	// Everything the shader needs to turn a world position into a cell, and
	// everything the builder needs to turn a cell back into a chunk. One struct,
	// published as a unit, because a geometry read that mixes a new anchor with
	// an old Dim indexes another cell's height -- see the class note on why a
	// wrong height here is not a cosmetic error.
	struct FGeometry
	{
		// The chunk level whose footprint is exactly one mip-0 cell. A level-L
		// chunk spans 32 * 2^L level-0 voxels = 3.2 * 2^L metres.
		int32 LeafChunkLevel = 2;
		// log2 of the level-0 voxels per leaf cell edge = 5 + LeafChunkLevel.
		// Carried rather than recomputed because the shader indexes with a shift
		// and a shift derived twice is a shift that can disagree once.
		int32 LeafVoxelShift = 7;
		// Leaf cells per side. Power of two, and a multiple of 2^(NumMips-1) so
		// every mip divides it exactly.
		int32 Dim = 0;
		int32 NumMips = 0;
		// Leaf-cell coordinate of the field's minimum corner, in absolute
		// level-LeafChunkLevel chunk coordinates. ALIGNED to 2^(NumMips-1) so
		// that a mip-m cell's children are exactly its 2x2 block -- an unaligned
		// origin makes the parent of a cell depend on the anchor, which is how a
		// reduce silently stops covering one of its children.
		int32 MinCellX = 0;
		int32 MinCellY = 0;
		// Element offset of each mip inside the flat array.
		int32 MipOffset[kMaxMips] = {};
		int32 TotalFloats = 0;

		bool IsValid() const { return Dim > 0 && NumMips > 0 && TotalFloats > 0; }
		// Leaf cell edge, in UU.
		double LeafEdgeUU() const { return 320.0 * double(int64(1) << LeafChunkLevel); }
	};

	FVoxelHeightPyramid();
	// Declared, not defaulted inline: the pooled-buffer member is a
	// TRefCountPtr to a forward-declared type, so the destructor has to be
	// emitted where that type is complete.
	~FVoxelHeightPyramid();

	// ---- builder side (game thread) ---------------------------------------

	// Compute the geometry a camera at WorldXY would want, at the given leaf
	// chunk level. Pure; does not touch the field.
	static FGeometry MakeGeometryFor(double WorldXUU, double WorldYUU, int32 LeafChunkLevel);

	// True if the live field's anchor is missing, is at a different leaf level,
	// or has drifted far enough from WorldXY that a refill is warranted. The
	// hysteresis is a quarter of the field half-span, which at the shipped
	// geometry is ~1.6 km of camera travel per refill.
	bool ShouldReanchor(double WorldXUU, double WorldYUU, int32 LeafChunkLevel) const;

	// Adopt a new geometry and CLEAR EVERY CELL TO +INF. Returns the number of
	// fill tiles the new anchor needs, in the order the builder should fill them
	// (nearest to the field centre first, so the arm engages on the ground the
	// camera is actually looking at before it engages on the far field).
	int32 Reanchor(const FGeometry& NewGeom, TArray<FIntPoint>& OutTileOrder);

	// Write one mip-0 cell. CellX/CellY are ABSOLUTE level-LeafChunkLevel chunk
	// coordinates; out-of-field writes are dropped rather than wrapped.
	void WriteLeaf(int32 CellX, int32 CellY, float ValueUU);

	// The same, for a whole rectangular block, UNDER ONE LOCK. The builder fills
	// a million cells over the life of an anchor and taking the lock per cell
	// would cost more than computing the bounds does. Values is row-major, W
	// wide and H tall, and the outcome tallies are folded into the census in the
	// same critical section so the two can never disagree about how many cells
	// were visited.
	void WriteLeafBlock(int32 CellX0, int32 CellY0, int32 W, int32 H, const float* Values,
	                    int32 DeclinedCount, int32 NotResidentCount);

	// Fold a player edit in. SEPARATE FROM WriteLeaf, and the separation is the
	// point: WriteLeaf publishes the analytic bound and is allowed to LOWER a
	// cell (a refill after a fine tile lands legitimately tightens it), while
	// this only ever RAISES one. An edit that adds a block above the analytic
	// surface is the single thing the amplifier cannot see, so it has to arrive
	// by a path that cannot be undone by the next refill of that cell -- which
	// is why the builder replays the edit map after every fill batch as well as
	// on the edit itself.
	void MaxLeafEdited(int32 CellX, int32 CellY, float ValueUU);

	// Recompute every mip from mip 0 by max-reduction. Cheap (a few hundred
	// microseconds at the shipped geometry) and idempotent, so the builder runs
	// it after each batch rather than trying to reduce incrementally: an
	// incremental reduce that misses a child publishes a bound BELOW its
	// subtree, which is the one error class this whole file exists to prevent.
	void RebuildMips();

	// Marks the field as changed so the render thread re-uploads. Call once per
	// tick after a batch, not per cell.
	void BumpVersion();

	FGeometry GetGeometry() const;
	uint32 GetVersion() const;

	// Fill accounting, so a leg can say what the field actually held rather than
	// having the reader infer it from a millisecond. A field that is silently
	// all-+INF is INERT and would otherwise read as a clean null result.
	struct FCensus
	{
		int32 LeafCells = 0;      // Dim * Dim
		int32 Filled = 0;         // leaves the builder has visited
		int32 Infinite = 0;       // leaves still +INF after being visited
		int32 Declined = 0;       // of those, the amplifier refused to bound
		int32 NotResident = 0;    // of those, a fine tile was not resident yet
		int32 Edited = 0;         // leaves raised by MaxLeafEdited
		float MinUU = 0.0f;       // over finite leaves
		float MaxUU = 0.0f;
	};
	FCensus GetCensus() const;
	void NoteLeafOutcome(bool bDeclined, bool bNotResident);
	void NoteEdited();

	// ---- render thread ----------------------------------------------------

	struct FBinding
	{
		FRDGBufferRef Buffer = nullptr;
		FGeometry Geom;
		bool bValid = false;
	};

	// Registers (and re-uploads if the version moved) the field for this graph.
	// bValid false means there is nothing safe to bind and the caller MUST leave
	// the arm off -- never bind a null SRV and let the shader read zeros, which
	// at this datum is "the terrain tops out at sea level".
	FBinding BindForRender(FRDGBuilder& GraphBuilder);

private:
	mutable FCriticalSection Lock;
	FGeometry Geom;
	TArray<float> Cells;
	uint32 Version = 0;
	FCensus Census;

	TRefCountPtr<FRDGPooledBuffer> Pooled;
	uint32 PooledVersion = 0;

	// Unlocked helpers; callers hold Lock.
	int32 LeafIndexUnlocked(int32 CellX, int32 CellY) const;
};

VOXELEARTHSHADERS_API FVoxelHeightPyramid& GetGlobalVoxelHeightPyramid();
