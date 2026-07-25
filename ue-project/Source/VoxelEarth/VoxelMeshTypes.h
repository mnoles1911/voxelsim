#pragma once
// Plain (UHT-free) mesh intermediate representation shared between
// UVoxelWorldSubsystem (drives vxc::meshBrick) and UVoxelChunkComponent /
// FVoxelChunkSceneProxy (turns it into GPU buffers). Mirrors vxc::Quad's
// field layout (voxel-core/include/voxelcore/mesher.h) but deliberately does
// NOT include any voxel-core header, so it stays safe to pull into
// UHT-parsed UE headers (doctrine: voxel-core is UE-header-free, and UE
// reflection headers stay voxel-core-free too -- see VoxelWorldSubsystem.cpp
// for the one place that bridges the two).
//
// Unlike vxc::Quad (brick-local, coordinates in [0,8)), Slice/U0/V0 here are
// CHUNK-local voxel coordinates (up to VoxelCoords::ChunkEdgeVoxels-1 = 31,
// still fits a uint8) -- the subsystem bakes each brick's offset within its
// render chunk into these fields at conversion time, so the scene proxy
// never needs to know about bricks at all.

#include "CoreMinimal.h"

struct FVoxelChunkQuad
{
	uint8 Axis = 0;       // normal axis: 0=x, 1=y, 2=z
	uint8 Positive = 0;   // 1 if the face normal points toward +axis
	uint8 Slice = 0;      // chunk-local voxel layer owning the face (0..31)
	uint8 U0 = 0, V0 = 0; // quad origin in chunk-local slice coords (0..31)
	uint8 W = 1, H = 1;   // quad extent along u, v (brick-local, 1..8)
	uint8 Ao = 0xFF;      // 2 bits per corner: (0,0),(1,0),(0,1),(1,1); 3=unoccluded
	uint8 Mat = 0;        // vxc::MaterialId
};

// Packs one quad into the 8 bytes the GPU pool stores and the vertex factory
// decodes.
//
// THIS LAYOUT IS A CONTRACT with DecodeVoxelQuadVertex in
// ue-project/Shaders/VoxelQuadDecode.ush, which reads the same 8 bytes as a
// uint2 and pulls the fields back out at these exact shifts. The GPU mesher
// emits this layout directly; this function is the CPU mesher's way into the
// same buffer, which is what lets the pooled path render CPU-meshed chunks
// without a second decode path. Change one side and you must change the other.
//
// Every field is <= 8 bits and Slice/U0/V0 are chunk-local (0..31), so nothing
// here can overflow for a 32-voxel chunk.
inline uint64 PackVoxelChunkQuad(const FVoxelChunkQuad& Q)
{
	const uint32 Lo = (uint32(Q.Axis)     & 0xfu)
	                | ((uint32(Q.Positive) & 0xfu) <<  4)
	                | (uint32(Q.Slice)             <<  8)
	                | (uint32(Q.U0)                << 16)
	                | (uint32(Q.V0)                << 24);
	const uint32 Hi = uint32(Q.W)
	                | (uint32(Q.H)   <<  8)
	                | (uint32(Q.Ao)  << 16)
	                | (uint32(Q.Mat) << 24);
	return uint64(Lo) | (uint64(Hi) << 32);
}
