#pragma once
// MeshChunkBricks: the CPU render-chunk mesher.
//
// Lifted verbatim out of VoxelWorldSubsystem.cpp's anonymous namespace so that
// the GPU-vs-CPU parity harnesses can call the SAME function the streaming path
// calls, instead of a transcription of it. A transcribed reference proves that
// two copies of a spec agree; it does not prove the shipping mesher agrees with
// anything, which is the only question worth asking here.
//
// Not UHT-safe: it includes voxel-core. Keep it out of any header that UHT
// parses (the doctrine in VoxelMeshTypes.h explains why).

#include "CoreMinimal.h"
#include "VoxelCoords.h"
#include "VoxelMeshTypes.h"

#include "voxelcore/core.h"
#include "voxelcore/counters.h"
#include "voxelcore/mesher.h"

#include <vector>

// Ring-boundary skirt mask bits (VoxelWorldSubsystem.cpp, "voxel cascade seam"
// fix). A level-L render chunk is meshed with a 1-voxel apron sampled by its
// OWN level sampler, so at a ring boundary its outward apron is a PHANTOM
// same-level neighbour -- but the chunk physically abutting it across the seam
// is a different-resolution ring, sampling the surface at 2x spacing. The two
// surfaces sit at different heights, and because each side culls its boundary
// wall against the phantom neighbour, neither emits a wall: the seam is an open
// edge that reads see-through (ocean shows past the lip) with dangling boundary
// voxels. Forcing the apron to AIR on exactly the boundary faces makes the
// mesher emit a watertight vertical wall there (a "skirt"), on BOTH sides of
// the seam (the finer ring's outer face and the coarser ring's inner face), so
// whichever ring's surface is higher, a wall closes the gap. Bits are the four
// lateral chunk faces; Z faces are never ring boundaries (rings are XY annuli).
enum ERingSkirtFace : uint8
{
	RingSkirt_NegX = 1 << 0,
	RingSkirt_PosX = 1 << 1,
	RingSkirt_NegY = 1 << 2,
	RingSkirt_PosY = 1 << 3,
};

// Meshes one render chunk's 4x4x4 bricks via MaterialAt and bakes each
// brick's quad coordinates into chunk-local ones. Shared, unchanged, by both
// halves of the lock-free split (docs/m1-plan.md Stage 2 decisions table,
// "Worker threading" row): the worker-job path calls this with a sampler
// closed over GeneratedWorld only (pure function of seed); the game-thread
// edited-chunk path calls it with a sampler closed over World::materialAt
// (overlay-aware). Only the sampler differs -- the mesh-and-bake logic never
// drifts between the two.
// WHAT A CALLER MUST PROVE to supply a BrickSkipFn. meshBrick's mask loop sets
// a non-zero face key only where an INTERIOR voxel is non-air AND its neighbour
// is air, so either of these is sufficient on its own, and each is one of the
// mesher's own guarantees restated:
//
//   * no SOLID in the brick interior  -- meshBrick's early-out 1, verbatim;
//   * no AIR in the brick + its apron -- no face can have an air neighbour.
//
// Skipping such a brick is byte-identical to meshing it, because meshing it
// appends nothing to OutQuads. The value is that meshBrick's 1,000 sampler
// calls happen BEFORE either early-out can fire -- filling `mat[10^3]` IS the
// cost, and a predicate answered from data the caller already holds skips it.
struct FNeverSkipBrick
{
	bool operator()(int32 /*Dx*/, int32 /*Dy*/, int32 /*Dz*/) const { return false; }
};

template <typename MaterialFn, typename BrickSkipFn = FNeverSkipBrick>
void MeshChunkBricks(const VoxelCoords::FVoxelChunkKey& ChunkKey, const MaterialFn& MaterialAt, TArray<FVoxelChunkQuad>& OutQuads,
                      vxc::Counters* PerfCounters = nullptr, uint8 RingSkirtMask = 0,
                      const BrickSkipFn& SkipBrick = BrickSkipFn())
{
	using namespace vxc;
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;
	constexpr int32 BricksPerChunk = VoxelCoords::ChunkEdgeBricks;
	constexpr int32 ChunkVox = BricksPerChunk * B; // 32

	std::vector<Quad> BrickQuads;
	for (int32 Dz = 0; Dz < BricksPerChunk; ++Dz)
	{
		for (int32 Dy = 0; Dy < BricksPerChunk; ++Dy)
		{
			for (int32 Dx = 0; Dx < BricksPerChunk; ++Dx)
			{
				// Provably emits nothing (see FNeverSkipBrick): skip the 1,000
				// sampler calls meshBrick would make before its own early-out
				// could fire. Counted as generated so the census keeps meaning
				// what it meant -- the brick WAS accounted for, it just cost
				// nothing.
				if (SkipBrick(Dx, Dy, Dz))
				{
					if (PerfCounters)
					{
						PerfCounters->incBricksGenerated();
						PerfCounters->incCellsWritten(uint64_t(B) * B * B);
					}
					continue;
				}

				const int64 OriginVX = (int64(ChunkKey.X) * BricksPerChunk + Dx) * int64(B);
				const int64 OriginVY = (int64(ChunkKey.Y) * BricksPerChunk + Dy) * int64(B);
				const int64 OriginVZ = (int64(ChunkKey.Z) * BricksPerChunk + Dz) * int64(B);
				const int32 ChunkBaseX = Dx * B; // this brick's origin in chunk-relative voxels
				const int32 ChunkBaseY = Dy * B;

				// Sampler valid on [-1,B]^3 (mesher.h contract): MaterialAt
				// reads straight across brick AND render-chunk borders via
				// the same deterministic function, so no neighbor data needs
				// to be materialized just to mesh this one brick.
				//
				// Ring-boundary skirt: when a lateral face of THIS render chunk
				// (chunk-relative coord < 0 or >= ChunkVox on a flagged axis) is
				// a ring boundary, report the apron there as AIR so the mesher
				// emits the boundary wall. Only affects reads outside the chunk
				// (the apron the boundary decision is made from); interior cells
				// and internal brick borders are untouched, so within-ring chunks
				// stay byte-for-byte identical.
				const auto Sampler = [&](int X, int Y, int Z) -> MaterialId
				{
					if (RingSkirtMask)
					{
						const int32 Xc = ChunkBaseX + X;
						const int32 Yc = ChunkBaseY + Y;
						if (((RingSkirtMask & RingSkirt_NegX) && Xc < 0) ||
						    ((RingSkirtMask & RingSkirt_PosX) && Xc >= ChunkVox) ||
						    ((RingSkirtMask & RingSkirt_NegY) && Yc < 0) ||
						    ((RingSkirtMask & RingSkirt_PosY) && Yc >= ChunkVox))
						{
							return MAT_AIR;
						}
					}
					return MaterialAt(OriginVX + X, OriginVY + Y, OriginVZ + Z);
				};

				BrickQuads.clear();
				meshBrick<B>(Sampler, BrickQuads);

				// docs/debug-tooling-plan.md P1 "vxc::Counters": counted here
				// (around the UE layer's call into meshBrick), not inside
				// voxel-core itself -- bricksGenerated/cellsWritten count
				// every brick attempted (whether or not it emits quads: an
				// interior-solid brick still gets sampled/meshed), quads
				// counted separately below only when non-empty.
				if (PerfCounters)
				{
					PerfCounters->incBricksGenerated();
					PerfCounters->incCellsWritten(uint64_t(B) * B * B);
				}

				if (BrickQuads.empty())
				{
					continue;
				}

				if (PerfCounters)
				{
					PerfCounters->incQuadsEmitted(uint64_t(BrickQuads.size()));
				}

				// Bake this brick's position within the chunk into the
				// (already chunk-scale, uint8-safe: max 31) quad fields so
				// the scene proxy never needs to know about bricks.
				const int32 AxisOffset[3] = {Dx * B, Dy * B, Dz * B};
				OutQuads.Reserve(OutQuads.Num() + (int32)BrickQuads.size());
				for (const Quad& Q : BrickQuads)
				{
					const int32 U = (Q.axis + 1) % 3;
					const int32 V = (Q.axis + 2) % 3;
					FVoxelChunkQuad CQ;
					CQ.Axis = Q.axis;
					CQ.Positive = Q.positive;
					CQ.Slice = (uint8)(Q.slice + AxisOffset[Q.axis]);
					CQ.U0 = (uint8)(Q.u0 + AxisOffset[U]);
					CQ.V0 = (uint8)(Q.v0 + AxisOffset[V]);
					CQ.W = Q.w;
					CQ.H = Q.h;
					CQ.Ao = Q.ao;
					CQ.Mat = Q.mat;
					OutQuads.Add(CQ);
				}
			}
		}
	}
}
