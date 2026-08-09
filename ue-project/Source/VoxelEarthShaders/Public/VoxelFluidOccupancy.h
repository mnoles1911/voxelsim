// The GPU occupancy volume the PBF particles collide against.
//
// Phase 0 spike (c) of docs/water-rearchitecture-plan-2026-08-09.md: "a GPU
// occupancy volume over the active region (512^3 bits ~= 16 MB for the 51.2 m
// cube), built incrementally from dirty bricks; particles project out along
// voxel face normals" (plan §5). This class owns the bits and the fill pass.
// The projection lives in ue-project/Shaders/VoxelFluidCollision.ush and the
// layout both agree on is defined once, engine-free and unit-tested, in
// voxel-core/include/voxelcore/fluidoccupancy.h.
//
// ---------------------------------------------------------------------------
// WHAT IT IS
// ---------------------------------------------------------------------------
//
// One bit per 10 cm voxel over a 512-voxel cube, packed x-major into uint32
// words (VoxelFluidContract.ush:64-69). 16 MiB, GPU-resident, never read back
// on any hot path. Set == solid terrain.
//
// TWO RULES MAKE IT SAFE RATHER THAN APPROXIMATELY SAFE, and both are the same
// rule the fine terrain tier uses -- blocked, never guessed:
//
//   * Outside the volume is SOLID. A particle cannot escape through an edge
//     that has not been built.
//   * An UNBUILT volume is solid everywhere. The clear writes all ones, so a
//     freshly created or recentred volume FREEZES the water inside it until
//     the regions land. The opposite convention (clear to air) leaks water
//     through terrain that merely has not arrived yet, and the symptom is
//     indistinguishable from a solver bug.
//
// ---------------------------------------------------------------------------
// WHERE SOLIDITY COMES FROM
// ---------------------------------------------------------------------------
//
// From vxc::World -- the edit overlay included -- via a host-side pack to one
// bit per voxel (vxc::packBrickSolidBits). That is the SAME source the
// game-thread mesher reads, which is the property that matters: the water must
// collide with the terrain the player can see and dig, not with the pure
// generated function.
//
// The alternative, reducing the GPU worldgen chain's OutCells, was
// investigated and rejected. The full argument is in the header comment of
// ue-project/Shaders/VoxelFluidOccupancy.usf; the short version is that
// OutCells is an RDG transient with no residency, and a chunk containing or
// bordering an edited brick is never GPU-meshed at all
// (FVoxelWorldImpl::ChunkHasEditedBrick, VoxelWorldSubsystem.cpp:6783-6814),
// so worldgen output is blind to exactly the edits the plan's scenario tests
// are about ("dig a channel from a river").
//
// ---------------------------------------------------------------------------
// INTEGRATION -- WHO CALLS WHAT, AND FROM WHERE
// ---------------------------------------------------------------------------
//
// NOTHING IS WIRED UP BY THIS FILE. The subsystem that owns the fluid
// (UVoxelWaterSubsystem / a new UVoxelFluidSubsystem -- another agent's file)
// owns the volume and drives it. The intended call sites, with the line
// numbers they sit at today:
//
//   1. TERRAIN EDITS. Every edit path funnels through
//      FVoxelWorldImpl::MarkChunkDirtyForRemesh
//      (VoxelWorldSubsystem.cpp:11946), reached from
//      FVoxelWorldImpl::ApplyGroupedEdits (:12170, the per-brick loop marks at
//      :12190). That is the hook: for each dirtied level-0 chunk whose bounds
//      intersect the volume, snap its voxel box with SnapRegion, pack it with
//      PackRegionBricks over UVoxelWorldSubsystem's world, and call
//      UpdateRegion. Level >= 1 keys are mip chunks and must be IGNORED here:
//      the volume is a level-0 structure and a coarse key would fill it with
//      the wrong grain.
//
//   2. TERRAIN ARRIVING. FVoxelWorldImpl::ApplyMeshResult
//      (VoxelWorldSubsystem.cpp:10867) is where a chunk's geometry becomes
//      resident. A chunk that streams in inside the volume needs the same
//      UpdateRegion call, or its part of the volume stays at the clear value
//      and water piles up against terrain that is visibly not there.
//
//   3. RECENTRING. SetOriginVoxel when the camera leaves the cube. It marks
//      everything unbuilt; the host then re-fills. SEE THE COST NOTE ON
//      SetOriginVoxel BEFORE BUILDING A RECENTRE POLICY -- a full refill is
//      not free and v0 does not scroll.
//
//   4. THE SOLVER. VoxelFluidSim.cpp (another agent's file) pastes
//      VOXEL_FLUID_OCCUPANCY_PARAMETERS() into its pass parameter struct and
//      fills it with BindShaderParameters below. It should call
//      VoxelFluidResolveCollisionEx, not the bare contract entry point -- see
//      the note on killing normal velocity in VoxelFluidCollision.ush.
//
// ORDERING. AddPasses must run BEFORE the solver's passes in the same graph,
// or a substep collides against last frame's terrain. RDG will not catch this
// for you: reading a stale-but-valid buffer is not an error.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"
#include "Templates/RefCounting.h"

#include "voxelcore/fluidoccupancy.h"

class FRDGBuilder;
class FRDGPooledBuffer;

// ---------------------------------------------------------------------------
// The shader-parameter block every consumer of the volume needs
// ---------------------------------------------------------------------------
//
// Paste into a BEGIN_SHADER_PARAMETER_STRUCT and fill with
// FVoxelFluidOccupancyVolume::BindShaderParameters. The names are the
// contract's (VoxelFluidContract.ush:64-69) and VoxelFluidCollision.ush binds
// them by exactly these names, so a consumer that spells one differently gets
// a silently unbound resource rather than a compile error -- which is why this
// is a macro and not three lines of documentation.
#define VOXEL_FLUID_OCCUPANCY_PARAMETERS() \
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, FluidOccupancyBits) \
	SHADER_PARAMETER(FIntVector, FluidVolumeOriginVoxel) \
	SHADER_PARAMETER(uint32,     FluidVolumeDimVoxels)

// One dirty box, packed and ready to dispatch.
//
// MinVoxel/SizeVoxels are VOLUME-LOCAL and must already be snapped to the
// update grid (32 voxels in x, 8 in y and z). Use
// FVoxelFluidOccupancyVolume::SnapRegion to produce them; UpdateRegion refuses
// anything else rather than clamping, because a clamped region silently leaves
// part of the dirty box holding pre-edit bits.
struct FVoxelFluidOccupancyRegion
{
	FIntVector MinVoxel = FIntVector::ZeroValue;
	FIntVector SizeVoxels = FIntVector::ZeroValue;

	// 16 words per brick, bricks ordered x fastest then y then z over the
	// region. Sized by BrickWordCount(SizeVoxels).
	TArray<uint32> BrickBits;
};

class VOXELEARTHSHADERS_API FVoxelFluidOccupancyVolume
{
public:
	FVoxelFluidOccupancyVolume();
	~FVoxelFluidOccupancyVolume();

	FVoxelFluidOccupancyVolume(const FVoxelFluidOccupancyVolume&) = delete;
	FVoxelFluidOccupancyVolume& operator=(const FVoxelFluidOccupancyVolume&) = delete;

	// 512 voxels, 51.2 m, 16 MiB. From voxel-core so the CPU reference and the
	// runtime cannot disagree about the size of the thing they describe.
	static constexpr int32 DimVoxels = vxc::kFluidVolumeDimVoxels;
	static constexpr int64 WordCount = vxc::kFluidVolumeWords;

	// Nothing here needs SM6 -- the fill is shifts and the collision read is a
	// typed buffer load, unlike worldgen.ush's 64-bit integer maths. It does
	// need compute, so this is an honest SM5 gate rather than an inherited one
	// (the same distinction FVoxelQuadTotalCS draws, VoxelGpuWorldGen.cpp:239).
	static bool IsSupportedOnCurrentRHI();

	// --- geometry -------------------------------------------------------
	//
	// THREADING. The origin and the two build flags are written by the game
	// thread (SetOriginVoxel) and read by the render thread (AddPasses,
	// BindShaderParameters), so all of them go through the same lock the
	// pending queue uses -- hence out of line rather than inline accessors.
	// It is uncontended in practice: one write per recentre against one read
	// per frame. Getting this wrong would be a torn FIntVector, i.e. water
	// colliding against terrain offset by whatever the other axes were.

	FIntVector GetOriginVoxel() const;

	// The UU origin particle positions MUST be relative to. See the top of
	// VoxelFluidCollision.ush: FluidOriginUU == FluidVolumeOriginVoxel *
	// 10 UU is a hard requirement, and this is the one place it is computed.
	FVector GetOriginUU() const;

	// Moves the volume. EVERY BIT BECOMES UNBUILT (solid), and the host must
	// re-fill.
	//
	// COST NOTE, AND IT IS THE OPEN QUESTION OF THIS SPIKE. v0 does not scroll
	// the volume: a recentre discards all 16 MiB. Refilling means packing
	// 64^3 = 262,144 bricks, and any brick not already resident has to be
	// GENERATED to pack it -- against a mesher measured with ~893 bricks/s of
	// spare capacity. So a recentre is a multi-second refill, during which the
	// unfilled part is solid and water freezes there.
	//
	// The fix is toroidal (wrap-around) addressing, where a recentre only
	// invalidates the newly exposed slab: ~1/50 of the volume for a one-chunk
	// step. It costs one extra int3 uniform and a per-axis modulo in
	// VoxelFluidSolidAtVoxel, and it is a CONTRACT CHANGE
	// (VoxelFluidContract.ush:64-69 describes a flat volume), so it is the
	// integrator's call and not a spike's. Until then: recentre rarely, on a
	// coarse grid, and prefer to size the active region so the camera stays
	// inside it.
	//
	// No-op when the origin is unchanged.
	void SetOriginVoxel(const FIntVector& NewOriginVoxel);

	// True until the first fill after a clear. Purely so a caller can tell
	// "the volume is solid because nothing has been built" from "the volume is
	// solid because the terrain is solid" -- two states that look identical
	// from inside the shader and produce very different bug reports.
	bool IsFullyUnbuilt() const;

	// --- region preparation, any thread ---------------------------------

	// Snaps an inclusive WORLD voxel box onto the update grid, converts it to
	// volume-local, and clips it to the volume. False when the box does not
	// intersect the volume at all -- a caller must not treat that as an empty
	// update it should still dispatch.
	static bool SnapRegion(const FIntVector& OriginVoxel, const FIntVector& MinVoxelWorld,
	                       const FIntVector& MaxVoxelWorld, FIntVector& OutMinVoxelLocal,
	                       FIntVector& OutSizeVoxels);

	// How many uint32s BrickBits must hold for a region of this size.
	static int64 BrickWordCount(const FIntVector& SizeVoxels);

	// Packs a snapped region's bricks into Out, in the order the kernel reads
	// them. MaterialFn: vxc::MaterialId(int64 vx, int64 vy, int64 vz) over
	// WORLD voxels -- pass UVoxelWorldSubsystem's overlay-aware materialAt, so
	// the volume sees the same terrain the mesher does.
	//
	// GAME THREAD, because that is where the overlay is safe to read
	// (VoxelWorldSubsystem.cpp:818-823). Templated so this module does not
	// have to know about vxc::World and does not gain a link dependency on
	// voxel-core -- see VoxelEarthShaders.Build.cs:33-41.
	template <typename MaterialFn>
	static void PackRegionBricks(const FIntVector& OriginVoxel, const FIntVector& MinVoxelLocal,
	                             const FIntVector& SizeVoxels, const MaterialFn& MaterialAt,
	                             TArray<uint32>& Out)
	{
		const int32 BricksX = SizeVoxels.X / vxc::kFluidBrickEdge;
		const int32 BricksY = SizeVoxels.Y / vxc::kFluidBrickEdge;
		const int32 BricksZ = SizeVoxels.Z / vxc::kFluidBrickEdge;

		Out.SetNumZeroed(int32(BrickWordCount(SizeVoxels)));
		for (int32 Ibz = 0; Ibz < BricksZ; ++Ibz)
		{
			for (int32 Iby = 0; Iby < BricksY; ++Iby)
			{
				for (int32 Ibx = 0; Ibx < BricksX; ++Ibx)
				{
					const int64 Base =
						vxc::fluidRegionBrickWordBase(Ibx, Iby, Ibz, BricksX, BricksY);
					// World voxel of this brick's minimum corner.
					const int64 Wx = int64(OriginVoxel.X) + MinVoxelLocal.X + int64(Ibx) * vxc::kFluidBrickEdge;
					const int64 Wy = int64(OriginVoxel.Y) + MinVoxelLocal.Y + int64(Iby) * vxc::kFluidBrickEdge;
					const int64 Wz = int64(OriginVoxel.Z) + MinVoxelLocal.Z + int64(Ibz) * vxc::kFluidBrickEdge;
					const auto BrickMaterial = [&](int32 Bx, int32 By, int32 Bz)
					{
						return MaterialAt(Wx + Bx, Wy + By, Wz + Bz);
					};
					vxc::packBrickSolidBits(BrickMaterial, &Out[int32(Base)]);
				}
			}
		}
	}

	// --- submission, game thread ----------------------------------------

	// Queues one snapped, packed region. Returns false and fills OutError on a
	// misaligned, out-of-bounds or wrongly sized region: REFUSED, NOT CLAMPED,
	// because every one of those means the caller computed the box wrong and a
	// clamp would leave part of it stale and invisible.
	//
	// Takes the region by value and moves it; BrickBits is up to a few hundred
	// KB and copying it per dirty chunk is exactly the kind of quiet cost this
	// path cannot afford.
	bool UpdateRegion(FVoxelFluidOccupancyRegion&& Region, FString& OutError);

	int32 NumPendingRegions() const;

	// Regions applied per AddPasses call. A burst limiter on render-thread
	// pass setup, not a capacity limit -- the same distinction
	// voxel.GPU.MeshBatchCap draws (VoxelGpuMeshJobManager.h:37-45). Leftover
	// regions stay queued and go next frame.
	void SetMaxRegionsPerFlush(int32 InMax);

	// --- counters -------------------------------------------------------
	//
	// Every stage writes a ran-flag distinguishable from "found nothing" (the
	// plan's Verification section, and the standing rule after three absent
	// stats produced three false conclusions in one session). A zero here
	// means the pass did not run; DispatchCount is what separates that from
	// "it ran and the terrain was empty".
	struct FStats
	{
		uint64 ClearCount = 0;        // whole-volume clears to all-solid
		uint64 RegionsQueued = 0;
		uint64 RegionsRejected = 0;   // misaligned / out of bounds / bad size
		uint64 RegionsApplied = 0;    // dispatched
		uint64 WordsWritten = 0;      // output words the dispatches covered
		uint64 BrickWordsUploaded = 0;
		// Times AddPasses has run, ever. The ORDERING GUARD's counter: the
		// solver checkf's that this advanced by exactly one inside its own
		// graph build, before any solver pass was added -- RDG will not catch
		// a stale volume for you (reading last frame's terrain is a valid
		// buffer read), so the integrator's rule "AddPasses BEFORE solver
		// passes, same graph" is enforced with this rather than with hope.
		uint64 AddPassesCount = 0;
	};
	FStats GetStats() const;

	// --- render thread ---------------------------------------------------

	// Registers the volume, clears it if it is new or was recentred, and
	// dispatches the queued region fills. Call once per frame BEFORE any pass
	// that reads the volume.
	//
	// Returns the registered buffer so the caller can build its own SRVs; null
	// when the RHI cannot run the pass at all.
	FRDGBufferRef AddPasses(FRDGBuilder& GraphBuilder);

	// Registers the volume WITHOUT clearing or filling, for a reader in a
	// later graph in the same frame. Null if AddPasses has never run.
	FRDGBufferRef Register(FRDGBuilder& GraphBuilder);

	// Fills a VOXEL_FLUID_OCCUPANCY_PARAMETERS() block. Templated on the
	// parameter struct so this header does not need to know any consumer's
	// type -- every consumer has the same three field names because they came
	// from the macro above.
	template <typename ParametersType>
	void BindShaderParameters(FRDGBuilder& GraphBuilder, ParametersType& Parameters)
	{
		Parameters.FluidOccupancyBits = CreateBitsSRV(GraphBuilder);
		Parameters.FluidVolumeOriginVoxel = GetOriginVoxel();
		Parameters.FluidVolumeDimVoxels = uint32(DimVoxels);
	}

private:
	// Out of line, and the reason this header only needs the forward
	// declaration of FRDGPooledBuffer -- same argument as
	// FVoxelGpuQuadPayload's destructor (VoxelGpuQuadPayload.h:60-72).
	FRDGBufferSRVRef CreateBitsSRV(FRDGBuilder& GraphBuilder);

	TRefCountPtr<FRDGPooledBuffer> PooledBits;

	FIntVector OriginVoxel = FIntVector::ZeroValue;
	bool bFullyUnbuilt = true;
	bool bClearPending = true;

	int32 MaxRegionsPerFlush = 32;

	mutable FCriticalSection QueueLock;
	TArray<FVoxelFluidOccupancyRegion> PendingRegions;
	FStats Stats;
};
