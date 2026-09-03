#pragma once
// VoxelCoords.h -- the ONE place for world<->voxel coordinate transforms
// (docs/m1-plan.md decisions table; docs/voxel-earth-implementation-plan.md
// SS3.3 LWC + origin rebasing). Deliberately UE-only (FVector/int64), no
// voxel-core include: this header is safe to pull into UHT-parsed headers
// (UVoxelWorldSubsystem.h, UVoxelChunkComponent.h). voxel-core owns the
// *voxel data* (vxc::kVoxelSizeMm, vxc::BrickKey, vxc::World<8>, ...); this
// header owns the *placement* of that data in the UE world.
//
// Convention: 1 voxel = 10 UU (UE units = cm) = vxc::kVoxelSizeMm (10cm).
// Voxel (0,0,0)'s negative corner sits at the UE world origin. +Z is up in
// both spaces. FVector in/out here is LWC-safe (double precision); only
// truncation to int64 voxel indices loses range, never precision, within a
// planet-scale world (int64 voxels covers +-~921 million km at 10cm/voxel).

#include "CoreMinimal.h"

namespace VoxelCoords
{
	// Kept in lockstep with vxc::kVoxelSizeMm (voxel-core/include/voxelcore/core.h)
	// by a static_assert in VoxelWorldSubsystem.cpp (the one .cpp allowed to
	// include both this header and voxelcore/core.h).
	inline constexpr double VoxelSizeUU = 10.0;

	inline constexpr int32 BrickEdgeVoxels = 8;  // vxc::World<8> (m1-plan.md decision)
	inline constexpr int32 ChunkEdgeBricks = 4;  // render chunk = 4x4x4 bricks
	inline constexpr int32 ChunkEdgeVoxels = BrickEdgeVoxels * ChunkEdgeBricks; // 32
	inline constexpr double ChunkEdgeUU = ChunkEdgeVoxels * VoxelSizeUU;        // 320 UU = 3.2m

	// M2 mip rings (docs/m2-plan.md decisions table): R0 = true voxels
	// (level 0, unchanged), R1-R4 = mip levels 1-4. A level-L voxel is
	// (1<<L) times the edge length of a level-0 voxel; a level-L render
	// chunk still spans ChunkEdgeVoxels (32) level-L cells, so its world
	// footprint doubles per level ("8 cubes become 1 in place").
	// Raised 5 -> 6 to push the voxel cascade from 1024 m out to 2048 m (R5 =
	// mip level 5, 102.4 m chunks over the 1024-2048 m annulus). This became
	// affordable only once the worker stopped building level-L chunks by folding
	// 8^L level-0 bricks and started generating them directly
	// (MakeCoarseLevelSampler in VoxelWorldSubsystem.cpp): per-chunk worker cost
	// is now flat in level (~4 ms at every level) instead of 8x per level, so an
	// extra ring costs chunks, not exponentially more time per chunk.
	//
	// If you change this, note that the hand-written per-level tables must grow
	// with it. C++ silently zero-fills a too-short initializer list, and the
	// resulting failures are all silent runtime ones (an all-zero ring annulus
	// admits nothing; an all-zero admission cutoff REJECTS everything; a zero
	// RingPresets outer radius collapses the clipmap to zero extent). A
	// static_assert on the DECLARED length does not catch a short initializer
	// list -- kRingSlotFloorDefault and kTints both passed theirs with 7 of 11
	// entries filled during the 11-level period. Grep kNumLevels and check the
	// INITIALIZER COUNT of every table it sizes.
	//
	// 8 SINCE 2026-09-02, and the story of 11 is the reason it is 8. The
	// cascade was pushed to 11 levels (65 km) on 2026-08-30 and the renderer
	// never coherently caught up: the marcher's ring clamp stayed at 7 (4 km --
	// far terrain streamed, was resident, and was never walked), the GPU
	// residency scan was silently capped at 8 levels, admission's Z-range came
	// from four corner samples that miss all interior relief at a 409 m
	// footprint, and walking all 11 rings measured ~219 ms/frame with holes.
	// The owner cut it to 8 rings (R7 outer = 8,192 m, double the long-standing
	// 4 km cascade) with the heightfield clipmap restored beyond -- see
	// docs/cascade-cut-to-8-2026-09-02.md.
	//
	// Level count and reach: R0 outer 64 m, doubling per ring, R7 = 8,192 m.
	// Ground cover owns level kNumLevels (== FVoxelBrickPool::kCoverLevel,
	// asserted equal in VoxelMarchChunkIndex.cpp); the VisBuffer's 4-bit split
	// level field carries values up to 15, so cover at 8 fits. The cascade's
	// construction ratio Outer/ChunkEdge == 40 holds at every level, so the
	// march index aliasing proof (span 80 < kDimXY 128) is level-independent.
	inline constexpr int32 kNumLevels = 8;  // R0..R7, 8,192 m cascade edge

	// Floored division matching vxc::floorDiv (C++ integer division truncates
	// toward zero; voxel/brick/chunk lattice indexing needs floor instead).
	constexpr int64 FloorDiv(int64 A, int64 B)
	{
		const int64 Q = A / B, R = A % B;
		return (R != 0 && ((R < 0) != (B < 0))) ? Q - 1 : Q;
	}

	// Floored modulo matching vxc::floorMod -- local-within-chunk-or-brick
	// coordinate of a voxel index (always in [0, B)).
	constexpr int64 FloorMod(int64 A, int64 B)
	{
		return A - FloorDiv(A, B) * B;
	}

	// UE units (cm) -> millimetres, matching vxc::kVoxelSizeMm's unit (stage 2
	// dig/place raycasts convert to mm here because voxelcore/raycast.h works
	// in mm exclusively; Phase 2 fine-tier streaming, VoxelFineTileStreamer.h,
	// is a second caller -- fine tile pixels and the wire format are also mm-
	// denominated). 1 UU = 10 mm since VoxelSizeUU (10 UU/voxel) = kVoxelSizeMm
	// (100 mm/voxel).
	FORCEINLINE int64 WorldToMm(double WorldUU)
	{
		return (int64)FMath::RoundToDouble(WorldUU * 10.0);
	}

	// The inverse, spelled once. Everything mm-denominated that has to be PLACED
	// in the world -- a baked lake datum, a basin bbox, a tile pixel edge -- has
	// been dividing by a literal 10.0 at the call site with its own comment
	// explaining the factor; this is the same conversion as WorldToMm's, read the
	// other way, and having both here is what keeps the pair from drifting.
	FORCEINLINE double MmToWorld(int64 Mm)
	{
		return double(Mm) * 0.1;
	}

	// Integer voxel lattice coordinate (unbounded range; matches vxc voxel
	// coordinates 1:1).
	struct FVoxelCoord
	{
		int64 X = 0, Y = 0, Z = 0;

		friend bool operator==(const FVoxelCoord&, const FVoxelCoord&) = default;
	};

	// W2 water groundwork: hashable so a TSet<FVoxelCoord>/TMap<FVoxelCoord, ...>
	// can key both actual voxel coordinates (dig-breach candidate cells) and,
	// where documented at the call site, water-brick-grid coordinates (the
	// same plain int64x3 shape, just a different lattice scale) -- reusing
	// this one type avoids a second near-identical struct for the water
	// subsystem, which stays voxel-core-free like everything else here.
	FORCEINLINE uint32 GetTypeHash(const FVoxelCoord& Coord)
	{
		return HashCombine(HashCombine(::GetTypeHash(Coord.X), ::GetTypeHash(Coord.Y)), ::GetTypeHash(Coord.Z));
	}

	// Render-chunk coordinates: one chunk = ChunkEdgeVoxels^3 voxels
	// (matches vxc::BrickKey scaled up by ChunkEdgeBricks). Level-relative:
	// (X,Y,Z) index chunks within whichever level's own independent lattice
	// they're paired with (see FVoxelLevelChunkKey below) -- a bare
	// FVoxelChunkKey on its own is only unambiguous at level 0.
	struct FVoxelChunkKey
	{
		int32 X = 0, Y = 0, Z = 0;

		friend bool operator==(const FVoxelChunkKey&, const FVoxelChunkKey&) = default;
	};

	FORCEINLINE uint32 GetTypeHash(const FVoxelChunkKey& Key)
	{
		return HashCombine(::GetTypeHash(Key.X), ::GetTypeHash(Key.Y), ::GetTypeHash(Key.Z));
	}

	// Level-aware render-chunk key (docs/m2-plan.md "Ring streaming" row):
	// pairs a mip level (0 = true voxels, 1-4 = mip rings) with a chunk key
	// in THAT level's own lattice. Level 0 keys are numerically identical to
	// the pre-M2 single-level scheme (back-compat: existing dig/edit code
	// keeps working against plain FVoxelChunkKey + implied level 0).
	struct FVoxelLevelChunkKey
	{
		int32 Level = 0;
		FVoxelChunkKey Key;

		friend bool operator==(const FVoxelLevelChunkKey&, const FVoxelLevelChunkKey&) = default;
	};

	FORCEINLINE uint32 GetTypeHash(const FVoxelLevelChunkKey& LevelKey)
	{
		return HashCombine(::GetTypeHash(LevelKey.Level), GetTypeHash(LevelKey.Key));
	}

	// Edge length (UU) of one voxel at the given mip level -- doubles per
	// level (docs/m2-plan.md: "8 cubes become 1 in place").
	FORCEINLINE double VoxelSizeUUForLevel(int32 Level)
	{
		return VoxelSizeUU * double(int64(1) << Level);
	}

	// Edge length (UU) of one render chunk at the given mip level.
	FORCEINLINE double ChunkEdgeUUForLevel(int32 Level)
	{
		return ChunkEdgeUU * double(int64(1) << Level);
	}

	// World position (UU, LWC) -> voxel lattice coordinate (floored).
	FORCEINLINE FVoxelCoord WorldToVoxel(const FVector& WorldPos)
	{
		return FVoxelCoord{
			(int64)FMath::FloorToDouble(WorldPos.X / VoxelSizeUU),
			(int64)FMath::FloorToDouble(WorldPos.Y / VoxelSizeUU),
			(int64)FMath::FloorToDouble(WorldPos.Z / VoxelSizeUU)};
	}

	// World position (UU, LWC) -> voxel lattice coordinate at the given mip
	// level (floored; level-L voxels are (1<<L) times level-0's edge).
	FORCEINLINE FVoxelCoord WorldToVoxelForLevel(const FVector& WorldPos, int32 Level)
	{
		const double SizeUU = VoxelSizeUUForLevel(Level);
		return FVoxelCoord{
			(int64)FMath::FloorToDouble(WorldPos.X / SizeUU),
			(int64)FMath::FloorToDouble(WorldPos.Y / SizeUU),
			(int64)FMath::FloorToDouble(WorldPos.Z / SizeUU)};
	}

	// Voxel lattice coordinate -> world position (UU) of its negative corner.
	FORCEINLINE FVector VoxelToWorldCorner(const FVoxelCoord& Voxel)
	{
		return FVector(Voxel.X * VoxelSizeUU, Voxel.Y * VoxelSizeUU, Voxel.Z * VoxelSizeUU);
	}

	FORCEINLINE FVector VoxelToWorldCenter(const FVoxelCoord& Voxel)
	{
		return VoxelToWorldCorner(Voxel) + FVector(VoxelSizeUU * 0.5);
	}

	// Render-chunk key containing a given voxel (level-agnostic: works for
	// any level's independent lattice, since chunk indexing is always
	// floorDiv(voxelIndex, ChunkEdgeVoxels) regardless of what a "voxel"
	// means at that level).
	FORCEINLINE FVoxelChunkKey ChunkKeyForVoxel(const FVoxelCoord& Voxel)
	{
		return FVoxelChunkKey{
			(int32)FloorDiv(Voxel.X, ChunkEdgeVoxels),
			(int32)FloorDiv(Voxel.Y, ChunkEdgeVoxels),
			(int32)FloorDiv(Voxel.Z, ChunkEdgeVoxels)};
	}

	// Render-chunk key -> voxel coordinate of its negative corner (in that
	// same level's own voxel units).
	FORCEINLINE FVoxelCoord ChunkOriginVoxel(const FVoxelChunkKey& Chunk)
	{
		return FVoxelCoord{
			int64(Chunk.X) * ChunkEdgeVoxels,
			int64(Chunk.Y) * ChunkEdgeVoxels,
			int64(Chunk.Z) * ChunkEdgeVoxels};
	}

	// Render-chunk key -> world position (UU) of its negative corner; this is
	// where a UVoxelChunkComponent for that chunk should sit (relative
	// location, actor root at the world/voxel origin). Level 0 only; see
	// ChunkOriginWorldForLevel for higher mip levels.
	FORCEINLINE FVector ChunkOriginWorld(const FVoxelChunkKey& Chunk)
	{
		return VoxelToWorldCorner(ChunkOriginVoxel(Chunk));
	}

	// Level-aware variant: scales the corner position by (1<<Level) since a
	// level-L "voxel" spans that many level-0 voxels' worth of world space.
	FORCEINLINE FVector ChunkOriginWorldForLevel(const FVoxelChunkKey& Chunk, int32 Level)
	{
		const double Scale = double(int64(1) << Level);
		const FVoxelCoord Origin = ChunkOriginVoxel(Chunk);
		return FVector(double(Origin.X) * VoxelSizeUU * Scale, double(Origin.Y) * VoxelSizeUU * Scale,
		               double(Origin.Z) * VoxelSizeUU * Scale);
	}

	// M2 wave 2 ("Distant-edit mip propagation" row, docs/m2-plan.md): the
	// level-L ancestor chunk key whose footprint CONTAINS a given level-0
	// chunk key -- the "cheap key math" the plan's decisions table calls for.
	// A level-L chunk spans ChunkEdgeVoxels (32) level-L cells, and a
	// level-L cell is (1<<L) level-0 voxels wide ("8 cubes become 1 in
	// place"), so a level-L chunk's world footprint is exactly (1<<L)
	// level-0 chunks wide per axis -- floorDiv by that factor is the whole
	// rule. Verified against ChunkOriginWorldForLevel: a level-0 chunk at
	// Chunk0 has world origin Chunk0*ChunkEdgeUU; a level-L chunk at ChunkL
	// has world origin ChunkL*ChunkEdgeUU*(1<<L); Chunk0's origin falls
	// inside ChunkL's span iff ChunkL == floorDiv(Chunk0, 1<<L) on every
	// axis (X, Y, and Z alike -- the scaling is uniform across all three).
	FORCEINLINE FVoxelChunkKey AncestorChunkKey(const FVoxelChunkKey& Level0Chunk, int32 Level)
	{
		const int64 Scale = int64(1) << Level;
		return FVoxelChunkKey{
			(int32)FloorDiv(Level0Chunk.X, Scale),
			(int32)FloorDiv(Level0Chunk.Y, Scale),
			(int32)FloorDiv(Level0Chunk.Z, Scale)};
	}
}
