#pragma once

#include "CoreMinimal.h"
#include "VoxelMeshTypes.h"

// Voxel light field (M4: "voxel light field + cone-traced GI") ---------------
//
// CLIENT-SIDE RENDERING ONLY. This structure is OUTSIDE the determinism
// boundary: it is built from render-chunk geometry, uses floats freely, and
// never feeds back into worldgen, the edit log, or anything replicated. Two
// clients may legitimately converge to slightly different irradiance; they must
// never disagree about world *state*, and nothing here can make them.
//
// Formulation: classic voxel cone tracing (Crassin et al. 2011), adapted to a
// world that is already a voxel grid.
//
//  * SURFACE voxelization, not solid. Standard VCT rasterizes scene *surfaces*
//    into a 3D grid; cones stop at the first surface they hit, so solid
//    interiors are irrelevant. That matters here because the only voxel data
//    this module can see without reaching into UVoxelWorldSubsystem's PImpl is
//    the greedy-mesher quad list that already flows through
//    UVoxelChunkComponent::SetChunkQuads -- which is exactly a surface
//    description, and is already delivered on stream-in AND on every
//    edit-driven remesh. Consuming that stream gives edit responsiveness for
//    free without touching a single deterministic path.
//
//  * The field is aligned 1:1 with level-0 render chunks: one FVoxelLFBrick
//    per chunk. Chunk edge = 320 UU = 8 cells of 40 UU (4 voxels). So brick
//    lifetime, dirtying and eviction all follow chunk streaming exactly, and
//    "re-voxelize a brick" is always a clean clear-then-fill of one chunk's
//    worth of geometry (never a partial overwrite).
//
//  * Mip pyramid for the cone march: 4 in-brick levels (40/80/160/320 UU)
//    plus 3 sparse global levels (640/1280/2560 UU). Aggregation is MAX, not
//    average -- deliberately over-occluding. VCT's classic failure mode is
//    light LEAKING through thin walls once they blur out at coarse mips; for
//    a game about digging tunnels, erring toward "too dark" is the correct
//    side to be wrong on.
//
//  * Per air-cell adjacent to a surface we store 6 directional visibility
//    bytes (+/-X, +/-Y, +/-Z) -- an ambient cube. Six 90-degree cones is the
//    well-trodden VCT diffuse budget. A shading point recombines them with
//    max(0, N.D) weights, which is why a floor and the ceiling directly above
//    it get correctly different answers out of the same field.
//
//  * One bounce is PROGRESSIVE: a cone that terminates on a surface picks up
//    that surface's previously-solved irradiance and multiplies by a constant
//    albedo. Nothing converges in a single solve; re-solves (from edits, from
//    streaming, and from a slow round-robin refresh) carry it forward. This is
//    iterative radiosity, and it is the property that makes bounded per-frame
//    cost possible -- there is no pass that must complete before the frame can
//    be shown, so a 200-brick explosion is a longer queue, not a stall.
//
// Thread model: structural mutation (brick insert/evict, coarse rebuild) is
// game-thread only. SolveBricks fans out read-mostly work over workers via a
// blocking ParallelFor -- each worker writes only its OWN brick's Vis/AvgIrr
// and reads neighbours', so the only races are byte reads of a neighbour's
// in-flight irradiance, which are benign for a progressive gather. FRWLock
// guards the map against a concurrent sampler.

namespace VoxelLF
{
	// 40 UU = 4 voxels. Chosen so BrickEdgeUU lands exactly on the level-0
	// render chunk edge (VoxelCoords::ChunkEdgeUU == 320) -- see the 1:1
	// alignment note above.
	constexpr int32 CellSizeUU = 40;
	constexpr int32 BrickEdgeCells = 8;
	constexpr int32 BrickCells = BrickEdgeCells * BrickEdgeCells * BrickEdgeCells; // 512
	constexpr int32 BrickEdgeUU = CellSizeUU * BrickEdgeCells;                     // 320

	constexpr int32 NumDirs = 6;
	constexpr int32 NumBrickMips = 4;    // levels 0..3 -> 40 / 80 / 160 / 320 UU
	constexpr int32 NumCoarseLevels = 3; // levels 4..6 -> 640 / 1280 / 2560 UU
	constexpr int32 MaxLevel = NumBrickMips + NumCoarseLevels - 1; // 6

	// Ambient-cube basis, in the same order the Vis[] bytes are stored.
	extern const FVector3f DirTable[NumDirs];

	inline int32 CellIndex(int32 X, int32 Y, int32 Z)
	{
		return X + BrickEdgeCells * (Y + BrickEdgeCells * Z);
	}
}

// One brick == one level-0 render chunk's worth of light field. ~3.6 KB.
struct FVoxelLFBrick
{
	// Surface opacity, 0 or 255 at level 0 (a cell either has a face in it or
	// does not -- see FVoxelLightField::VoxelizeChunk), MAX-aggregated upward.
	uint8 Opacity[VoxelLF::BrickCells] = {};
	uint8 Mip1[64] = {}; // 4x4x4, cell 80 UU
	uint8 Mip2[8] = {};  // 2x2x2, cell 160 UU
	uint8 Mip3 = 0;      // 1x1x1, cell 320 UU (also this brick's entry in Coarse[0]'s parent chain)

	// Solved directional irradiance: 6 bytes per cell, indexed
	// [CellIndex * NumDirs + Dir]. Only cells that are empty AND adjacent to a
	// surface are ever solved; everything else stays 0 and reads back invalid.
	uint8 Vis[VoxelLF::BrickCells * VoxelLF::NumDirs] = {};

	// Directional average of Vis, cached so the bounce gather is one byte read
	// instead of six. Doubles as the "this cell has been solved" marker via
	// bSolvedCell below.
	uint8 AvgIrr[VoxelLF::BrickCells] = {};

	// Per-cell validity for sampling: set iff this cell carries a solved
	// irradiance. Kept separate from AvgIrr because a legitimately solved cell
	// deep in a sealed tunnel has AvgIrr == 0, and that is a real answer, not
	// a missing one.
	TBitArray<> SolvedCells;

	bool bHasGeometry = false;
	bool bSolved = false;
	// Bumped every time VoxelizeChunk rewrites this brick; lets the subsystem
	// tell "the solve I am looking at is current" from "the geometry moved
	// under me".
	uint32 GeometrySerial = 0;

	FVoxelLFBrick()
	{
		SolvedCells.Init(false, VoxelLF::BrickCells);
	}
};

// Tunables handed down from the subsystem's cvars (see VoxelGI.cpp) so the
// field itself owns no policy.
struct FVoxelGISolveParams
{
	// Cone march terminates here regardless of occlusion. 30 m: past that,
	// diffuse contribution through a 90-degree cone is a rounding error, and
	// the R0 ring this field lives inside is only 64 m anyway.
	float MaxConeDistanceUU = 3000.f;
	// tan(half-aperture). 1.0 == 90-degree cones, the standard 6-cone diffuse
	// configuration.
	float ConeTanHalfAperture = 1.0f;
	// Diffuse albedo used for the progressive bounce. M_VoxelTerrain's flat
	// albedo tint is (0.5, 0.45, 0.4); this is its luminance, rounded down --
	// the bounce is monochrome (see VoxelGI.h's honesty note on that).
	float BounceAlbedo = 0.4f;
	// Scales the sky term. 1.0 means an unoccluded cell solves to exactly 1.0,
	// which is what keeps open terrain looking identical with GI on and off.
	float SkyIntensity = 1.0f;
	// Guard against runaway self-feeding in the progressive gather.
	float MaxBounceContribution = 0.6f;
};

class FVoxelLightField
{
public:
	// --- structural, game thread only --------------------------------------

	// Clear-then-fill one brick from a level-0 chunk's greedy quads.
	// ChunkOriginUU is the component's world location; quads are chunk-local
	// voxel units (0..31). Returns the brick's new geometry serial.
	uint32 VoxelizeChunk(const FIntVector& BrickCoord, const FVector& ChunkOriginUU,
	                     const TArray<FVoxelChunkQuad>& Quads);

	// Drops bricks beyond RadiusUU of CameraUU, at most MaxEvictions per call
	// (this is the only thing that reclaims memory -- eviction is by distance
	// rather than by a chunk-unload hook, because UVoxelChunkComponent has no
	// unload callback this module is allowed to add one to). Returns the count
	// evicted.
	int32 EvictFarBricks(const FVector& CameraUU, double RadiusUU, int32 MaxEvictions,
	                     TArray<FIntVector>& OutEvicted);

	// Rebuilds the 3 sparse global levels from scratch. O(NumBricks) map
	// inserts; called only when the resident set has actually changed.
	void RebuildCoarse();

	void GetResidentKeys(TArray<FIntVector>& Out) const;
	int32 NumBricks() const;
	bool HasBrick(const FIntVector& Key) const;
	SIZE_T EstimatedBytes() const;
	void Reset();

	// --- solve --------------------------------------------------------------

	// Cone-traces every solvable cell in each of Keys. Blocking ParallelFor.
	// Returns the number of cells actually solved (for budget reporting).
	int32 SolveBricks(const TArray<FIntVector>& Keys, const FVoxelGISolveParams& Params);

	// --- sampling, game thread (scene proxy construction) -------------------

	// Trilinear ambient-cube lookup. Returns false (and leaves OutIrradiance
	// untouched) when the field has nothing solved around WorldUU -- callers
	// MUST fall back to their non-GI path on false, which is what prevents a
	// black flash on a chunk whose brick has streamed in but not yet solved.
	bool SampleIrradiance(const FVector& WorldUU, const FVector3f& Normal, float& OutIrradiance) const;

	// Batched form of the above. A scene proxy shades thousands of vertices in
	// one construction; taking the read lock per vertex would cost more atomics
	// than the lookup itself, so it is hoisted into this RAII scope. Game
	// thread only (proxy construction), and must not outlive a tick.
	class FReadScope
	{
	public:
		explicit FReadScope(const FVoxelLightField& InField)
			: Field(InField)
		{
			Field.Lock.ReadLock();
		}
		~FReadScope() { Field.Lock.ReadUnlock(); }
		FReadScope(const FReadScope&) = delete;
		FReadScope& operator=(const FReadScope&) = delete;

		bool Sample(const FVector& WorldUU, const FVector3f& Normal, float& OutIrradiance) const
		{
			return Field.SampleIrradianceUnlocked(WorldUU, Normal, OutIrradiance);
		}

	private:
		const FVoxelLightField& Field;
	};

	// Level-0 chunk coord <-> brick coord are the same lattice; exposed so the
	// subsystem and the scene proxy agree on it.
	static FIntVector WorldToBrick(const FVector& WorldUU);

private:
	bool SampleIrradianceUnlocked(const FVector& WorldUU, const FVector3f& Normal, float& OutIrradiance) const;
	bool SampleIrradianceAtProbe(const FVector& P, const float (&DirWeight)[VoxelLF::NumDirs],
	                             float InvDirWeightSum, float& OutIrradiance) const;

	// Opacity in [0,1] at mip level L (0..MaxLevel) at a world position.
	float SampleOpacity(const FVector& WorldUU, int32 Level) const;
	// Previously-solved average irradiance at a world position, 0 if unsolved.
	float SampleAvgIrr(const FVector& WorldUU) const;

	const FVoxelLFBrick* FindBrick(const FIntVector& Key) const;

	int32 SolveBrickInternal(const FIntVector& Key, FVoxelLFBrick& Brick, const FVoxelGISolveParams& Params);
	void BuildBrickMips(FVoxelLFBrick& Brick) const;

	TMap<FIntVector, TUniquePtr<FVoxelLFBrick>> Bricks;
	// Levels 4..6 (640 / 1280 / 2560 UU cells). Sparse; MAX-aggregated.
	TMap<FIntVector, uint8> Coarse[VoxelLF::NumCoarseLevels];

	mutable FRWLock Lock;
	uint32 NextGeometrySerial = 1;
};
