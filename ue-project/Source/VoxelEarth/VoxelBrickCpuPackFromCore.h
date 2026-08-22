#pragma once
// ONE COPY OF vxc::ChunkBrickPack -> FVoxelBrickCpuPack, FOR ALL PRODUCERS.
//
// This conversion used to live in a single anonymous namespace inside
// VoxelWorldSubsystem.cpp, which was correct while terrain was the only
// producer. Phase 6 adds a second one -- ground cover, packed by
// vxc::packCoverChunk in VoxelDetailAssetSubsystem.cpp -- and a second
// transcription of a byte format is the single defect shape this project has
// paid for most often ("derived, not verified, detaches"). So the function moved
// here rather than being copied, and VoxelWorldSubsystem.cpp's FinishPack now
// forwards to it.
//
// WHAT MAKES A SECOND COPY DANGEROUS RATHER THAN MERELY UNTIDY, restated from
// FVoxelBrickCpuPack's own header because it is the field a copy would get
// wrong: bAllSolid is NOT derivable from the descriptors and looks as though it
// is. A brick can be fully solid and still MIXED, so "every descriptor is
// uniform SOLID" is strictly stronger than allSolid and under-reports it. The
// packer walked the CELLS; this function copies what the packer decided and
// inspects nothing.
//
// OFFSETS STAY CHUNK-RELATIVE. FVoxelBrickPool::Flush folds in the arena bases,
// and it is the only place that does.

#include "CoreMinimal.h"

#include "VoxelBrickPool.h"

#include "voxelcore/brick.h"
#include "voxelcore/brickpack.h"

// OriginVoxel is in the LATTICE THE PACK WAS BUILT ON, not always level-0
// voxels: a terrain chunk passes level-L voxels, a cover chunk passes COVER
// CELLS. That is exactly what the chunk record stores and what the marcher
// validates the index against (WantOrigin = ChunkCoord * 32 in the chunk's own
// units), so the two agree by construction at every lattice.
inline FVoxelBrickCpuPackRef VoxelBrickCpuPackFromCore(const vxc::ChunkBrickPack& Packed,
                                                       int64 BaseX, int64 BaseY, int64 BaseZ)
{
	FVoxelBrickCpuPackRef Out = MakeShared<FVoxelBrickCpuPack, ESPMode::ThreadSafe>();

	// 64 descriptors, two dwords each, ALWAYS -- collapsed bricks included.
	// vxc::BrickDesc is {OccWord, MatWord} and is what the GPU kernel reads
	// as a uint2, so this is a copy of the same two dwords in the same order.
	Out->Desc.SetNumUninitialized(int32(vxc::kMarchChunkBricks) * 2);
	for (int32 I = 0; I < int32(vxc::kMarchChunkBricks); ++I)
	{
		Out->Desc[I * 2 + 0] = Packed.descs[size_t(I)].OccWord;
		Out->Desc[I * 2 + 1] = Packed.descs[size_t(I)].MatWord;
	}

	Out->Occ.SetNumUninitialized(int32(Packed.occ.size()));
	if (!Packed.occ.empty())
	{
		FMemory::Memcpy(Out->Occ.GetData(), Packed.occ.data(),
		                Packed.occ.size() * sizeof(uint32));
	}
	Out->Mat.SetNumUninitialized(int32(Packed.mat.size()));
	if (!Packed.mat.empty())
	{
		FMemory::Memcpy(Out->Mat.GetData(), Packed.mat.data(),
		                Packed.mat.size() * sizeof(uint32));
	}

	// COPIED, NOT RE-DERIVED -- see the header note on bAllSolid.
	Out->BrickSolid = Packed.brickSolid;
	Out->bAnySolid = Packed.anySolid;
	Out->bAllSolid = Packed.allSolid;
	Out->OriginVoxel = FIntVector(int32(BaseX), int32(BaseY), int32(BaseZ));
	return Out;
}
