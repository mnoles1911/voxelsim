#pragma once
// The footprint band: FFootprintBand and the two per-column reductions it is a
// max/min of.
//
// WHY THIS FILE EXISTS -- the same reason VoxelChunkMesher.h exists, verbatim.
// These lived inside VoxelWorldSubsystem.cpp's VoxelStreaming namespace, so
// nothing else could call them, so a GPU-vs-CPU parity harness could only have
// compared BandReduceMain against a TRANSCRIPTION of the reduction. A
// transcribed reference proves that two copies of a spec agree; it does not
// prove the shipping reduction agrees with anything, which is the only question
// worth asking. Lifted verbatim so voxel.GPU.VerifyRegion calls the SAME
// functions DispatchJobs' worker jobs call.
//
// AND THE ASYMMETRY THAT MAKES IT A PRECONDITION RATHER THAN A TIDY-UP. A band
// that is wrong in the conservative direction only wastes work -- a chunk that
// could have been skipped gets meshed and produces nothing. Wrong the other way
// it SKIPS CHUNKS THAT SHOULD HAVE BEEN MESHED, which reads as holes in the
// world and gets blamed on streaming. See "WHICH WAY IT ERRS" below.
//
// Not UHT-safe: it includes voxel-core. Keep it out of any header UHT parses
// (the doctrine in VoxelMeshTypes.h explains why).

#include "CoreMinimal.h"
#include "VoxelCoords.h"

#include "voxelcore/core.h"
#include "voxelcore/amplifier.h"
#include "voxelcore/caves.h"
#include "voxelcore/caverns.h"

namespace VoxelStreaming
{
// --- Buried-chunk pre-dispatch skip -----------------------------------------
//
// docs/status.md "Buried-chunk pre-dispatch skip". Measured on this build:
// ~76-79% of level-0 worker output meshes to ZERO quads, and those jobs are
// 71-75% of level-0 worker wall time (the mesher's uniform-brick early-outs
// save little, because the dominant cost is upstream of the mesher -- the
// (32+2)^2 Amplifier::column grid alone is ~42% of a level-0 job). Roughly
// 70% of the zero-quad level-0 chunks are fully-buried solid and ~30% are
// entirely above the terrain.
//
// A FOOTPRINT BAND is a pair of level-0 voxel z values summarising everything
// the whole (X,Y) footprint can contain, derived from the SAME 34x34 column
// grid a level-0 job already builds. Every level-0 chunk in a given (X,Y)
// shares that grid's XY extent exactly -- the mesher's 1-voxel apron makes a
// chunk read columns [32X-1, 32X+32], which is precisely the grid -- so one
// job's band answers the "can this chunk contain geometry" question for every
// chunk stacked above and below it, for free. That is the whole trick: the
// band costs one reduction over columns that were paid for anyway, and it is
// then reused by the ~10-20 other chunks in the same footprint.
//
// WHICH WAY IT ERRS. Every bound below is an OUTER bound of the corresponding
// voxel-core predicate, taken over a SUPERSET of the columns a chunk reads
// (the apron-inclusive grid), with an extra one-voxel margin on each side. So
// the band can only ever claim LESS emptiness than really exists: it may fail
// to skip a chunk that is in fact featureless (pure lost opportunity, the
// chunk meshes as before and still produces nothing), and it cannot claim
// "definitely empty" for a chunk that has geometry. False "might have
// geometry" is free; false "definitely empty" would be a rendering bug.
struct FFootprintBand
{
	// Highest level-0 voxel z that is solid in ANY column of the footprint
	// (apron included). Strictly above this every column is air, so a chunk
	// whose interior starts above it has an all-air interior and hits
	// meshBrick's early-out 1 in every one of its 64 bricks.
	int32 MaxSurfaceTopVoxel = 0;
	// Lowest level-0 voxel z at which air can exist in ANY column of the
	// footprint. Strictly below this every column is solid, so a chunk whose
	// apron-inclusive top is below it contains no AIR at all -- and
	// voxelcore/mesher.h emits a face only where a solid voxel has an AIR
	// neighbour, so it meshes to zero quads whatever materials it holds.
	int32 SolidBelowVoxel = 0;
};

// The band verdict for one level-0 chunk Z, in ONE place. Two call sites now
// ask it -- the pre-dispatch skip in DispatchJobs and the admission skip in
// RecomputeDesiredSet -- and they must not be able to drift apart: an
// admission site that skipped one chunk more than the dispatch site would
// delete geometry the verify harness has never checked.
//
// Pure in (Band, ChunkZ). The band itself is a pure function of the level-0
// footprint (X,Y) and the amplifier, so this verdict is too -- that is what
// makes it legal to evaluate at admission time at all.
inline bool BandProvesChunkEmpty(const FFootprintBand& Band, int32 ChunkZ, bool& bOutAllAir)
{
	constexpr int64 ChunkVox = VoxelCoords::ChunkEdgeVoxels;
	const int64 InteriorZMin = int64(ChunkZ) * ChunkVox;
	const int64 ApronZMax = InteriorZMin + ChunkVox; // interior top is +31; the mesher's apron reads +32
	// All air: the whole interior sits above every column's topmost solid
	// voxel, so every one of the 64 bricks hits meshBrick's early-out 1.
	const bool bAllAir = InteriorZMin > int64(Band.MaxSurfaceTopVoxel);
	// All solid: chunk AND apron sit below the lowest z at which any column
	// can hold air, so no face has an AIR neighbour.
	const bool bAllSolid = ApronZMax < int64(Band.SolidBelowVoxel);
	bOutAllAir = bAllAir;
	return bAllAir || bAllSolid;
}

// Smallest integer >= sqrt(v), for v >= 0. The carve predicates all test
// `dz*dz < marginSq`, i.e. |dz| < sqrt(marginSq); rounding the radius UP is
// what keeps every bound below an outer bound.
inline int64 CeilSqrtI64(int64 V)
{
	if (V <= 0)
	{
		return 0;
	}
	int64 R = int64(FMath::CeilToDouble(FMath::Sqrt(double(V))));
	while (R * R < V) { ++R; }   // never under-estimate, whatever the FP rounding did
	while (R > 0 && (R - 1) * (R - 1) >= V) { --R; }
	return R;
}

// Topmost level-0 voxel z whose material is not AIR by stratigraphy alone.
// Mirrors Amplifier::stratigraphyAt: a voxel's centre is vz*100+50 mm and it
// is air iff surfaceMm - centre < 0. Caves and caverns only ever REMOVE solid,
// never add it, so nothing above this can be solid.
inline int64 ColumnSurfaceTopVoxel(const vxc::ColumnSample& Col)
{
	return vxc::floorDiv(int64(Col.surfaceMm) - vxc::kVoxelSizeMm / 2, int64(vxc::kVoxelSizeMm));
}

// Lowest level-0 voxel z at which this column can hold AIR. A conservative
// OUTER bound (i.e. possibly lower than the truth, never higher) on the three
// independent sources of air, mirroring voxelcore/amplifier.cpp's materialAt:
//
//  1. Everything strictly above the surface (always present).
//  2. caveCarveAt (voxelcore/caves.h): air needs `depthMm < segs[s].depthMm +
//     sqrt(marginSq)` for some segment, or `depthMm <= shaftDepthMaxMm` for a
//     sinkhole shaft. The shaft term is taken OUTSIDE the bedrock clamp
//     because caveCarveAt tests the shaft BEFORE its roof and bedrock guards.
//  3. cavernCarveAt (voxelcore/caverns.h): air needs absolute
//     `zAbs >= zFloorMm` and `zAbs > zCenterMm - sqrt(marginSq)`.
//
// Both carve passes independently refuse `vz < kCaveMinVoxelZ` (sea level:
// the implicit ocean owns everything below z=0, so a void there is water, not
// a cave) and refuse a column whose surface is below kCaveMinSurfaceMm -- both
// are used to tighten the bound, but only against the CARVE terms, never
// against source 1: terrain whose surface is itself below sea level has air
// above it and must not be claimed solid.
inline int64 ColumnDeepestAirVoxel(const vxc::ColumnSample& Col)
{
	const int64 SurfaceTop = ColumnSurfaceTopVoxel(Col);
	int64 CarveMin = INT64_MAX;

	const bool bCarveEligible = Col.surfaceMm >= vxc::kCaveMinSurfaceMm;
	if (bCarveEligible && (Col.cave.count > 0 || Col.cave.shaftMarginSq > 0))
	{
		int64 MaxCaveDepthMm = INT64_MIN;
		if (Col.cave.shaftMarginSq > 0)
		{
			MaxCaveDepthMm = FMath::Max(MaxCaveDepthMm, int64(Col.cave.shaftDepthMaxMm));
		}
		int64 SegDepthMm = INT64_MIN;
		for (int32 S = 0; S < Col.cave.count; ++S)
		{
			SegDepthMm = FMath::Max(SegDepthMm, int64(Col.cave.segs[S].depthMm) + CeilSqrtI64(int64(Col.cave.segs[S].marginSq)));
		}
		if (SegDepthMm > INT64_MIN)
		{
			// caveCarveAt refuses once depthMm + kCaveBedrockMarginMm >= bedrockDepthMm.
			SegDepthMm = FMath::Min(SegDepthMm, int64(Col.bedrockDepthMm) - vxc::kCaveBedrockMarginMm);
			MaxCaveDepthMm = FMath::Max(MaxCaveDepthMm, SegDepthMm);
		}
		if (MaxCaveDepthMm > INT64_MIN)
		{
			// depthMm < MaxCaveDepthMm  <=>  vz*100+50 > surfaceMm - MaxCaveDepthMm.
			CarveMin = FMath::Min(CarveMin, vxc::floorDiv(int64(Col.surfaceMm) - MaxCaveDepthMm - vxc::kVoxelSizeMm / 2,
			                                             int64(vxc::kVoxelSizeMm)));
		}
	}

	if (bCarveEligible && Col.cavern.count > 0)
	{
		int64 MinZAbsMm = INT64_MAX;
		for (int32 S = 0; S < Col.cavern.count; ++S)
		{
			const vxc::CavernSeg& Sg = Col.cavern.segs[S];
			MinZAbsMm = FMath::Min(MinZAbsMm, FMath::Max(int64(Sg.zFloorMm),
			                                             int64(Sg.zCenterMm) - CeilSqrtI64(int64(Sg.marginSq))));
		}
		if (MinZAbsMm < INT64_MAX)
		{
			// cavernCarveAt refuses once depthMm + kCaveBedrockMarginMm >= bedrockDepthMm.
			MinZAbsMm = FMath::Max(MinZAbsMm, int64(Col.surfaceMm) - int64(Col.bedrockDepthMm) + vxc::kCaveBedrockMarginMm);
			CarveMin = FMath::Min(CarveMin, vxc::floorDiv(MinZAbsMm - vxc::kVoxelSizeMm / 2, int64(vxc::kVoxelSizeMm)));
		}
	}

	if (CarveMin != INT64_MAX)
	{
		CarveMin = FMath::Max(CarveMin, int64(vxc::kCaveMinVoxelZ)); // no carving at or below sea level
	}
	// Source 1 is unconditional and must NOT be clamped to sea level.
	return FMath::Min(CarveMin, SurfaceTop + 1);
}

// The last step of the reduction: the two raw extrema over the column grid,
// widened by one voxel each way and clamped into the band's int32 fields.
//
// A FUNCTION rather than four lines inlined at the job site, because there are
// now three producers of a band and they must not drift: the CPU worker job,
// voxel.GPU.VerifyRegion's gate, and (Wave D / D6) BandReduceMain's readback,
// which deliberately returns the RAW extrema and leaves this widening on the
// CPU -- every constant mirrored into HLSL is a determinism liability, and
// these two are additions against constants that only exist here.
//
// The one-voxel widening on each side is pure insurance: the per-column bounds
// are already outer bounds, so this makes an off-by-one in a derivation
// harmless rather than a hole in the world.
inline FFootprintBand MakeFootprintBand(int64 MaxSurfaceTopVoxel, int64 MinDeepestAirVoxel)
{
	FFootprintBand Band;
	Band.MaxSurfaceTopVoxel = int32(FMath::Clamp<int64>(MaxSurfaceTopVoxel + 1, INT32_MIN, INT32_MAX));
	Band.SolidBelowVoxel = int32(FMath::Clamp<int64>(MinDeepestAirVoxel - 1, INT32_MIN, INT32_MAX));
	return Band;
}
} // namespace VoxelStreaming
