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
	inline constexpr int32 kNumLevels = 5;

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

	// UE units (cm) -> millimetres, matching vxc::kVoxelSizeMm's unit (this is
	// the ONLY place stage 2 (dig/place raycasts) converts to mm; voxelcore/
	// raycast.h works in mm exclusively). 1 UU = 10 mm since VoxelSizeUU (10
	// UU/voxel) = kVoxelSizeMm (100 mm/voxel).
	FORCEINLINE int64 WorldToMm(double WorldUU)
	{
		return (int64)FMath::RoundToDouble(WorldUU * 10.0);
	}

	// Integer voxel lattice coordinate (unbounded range; matches vxc voxel
	// coordinates 1:1).
	struct FVoxelCoord
	{
		int64 X = 0, Y = 0, Z = 0;
	};

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
}
