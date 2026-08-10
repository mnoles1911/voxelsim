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
// AND IT IS A ROLLING WINDOW (contract item 4, taken 2026-08-09)
// ---------------------------------------------------------------------------
//
// The buffer is addressed TOROIDALLY: a bit's storage slot is
// (volume-local + wrap offset) mod 512, where the wrap offset is how far the
// window has slid since it was created. So MOVING THE WINDOW DOES NOT MOVE THE
// BITS. RecentreTo() slides the origin, leaves everything still in view exactly
// where it sits in memory, and hands back the list of 64^3 cells that have just
// come into view for the caller to refill; those cells are marked unbuilt
// (solid) on the GPU in the meantime, so the newly exposed space freezes water
// rather than colliding it against the terrain that used to be stored there.
// The cells that LEFT are neither cleared nor copied -- the entering cells land
// on precisely their slots, which is the whole trick.
//
// SetOriginVoxel() still exists and still throws all 16 MiB away. That is the
// right call for the first latch (nothing is built yet, so there is nothing to
// preserve) and for a teleport; RecentreTo() is the right call for a camera
// that walked. See both.
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
//   3. RECENTRING. RecentreTo() when the camera crosses the inner hysteresis
//      boundary. THE POLICY THE SUBSYSTEM SHOULD IMPLEMENT, spelled out because
//      the volume cannot implement it (it does not know where the camera is)
//      and because every number in it is a trade the owner may want to move:
//
//        Once per game tick, after the view origin is known and BEFORE
//        ProcessOccupancyQueue:
//
//          CamVoxel   = WorldToVoxel(ViewOriginUU)
//          WantOrigin = CamVoxel - DimVoxels/2          // re-centre exactly
//          Drift      = WantOrigin - CurrentOriginVoxel  // per axis
//          if (max|Drift| < RecentreTriggerVoxels) return;   // 96, see below
//          Step       = round(Drift / RecentreStepVoxels) * RecentreStepVoxels
//          NewOrigin  = CurrentOriginVoxel + Step
//          RecentreTo(NewOrigin, OutRefill, OutDelta, Error)
//
//        then, for each cell in OutRefill, queue the SAME work the initial fill
//        queues (pack with PackRegionBricks against the NEW origin, submit with
//        UpdateRegion) -- ordered centre-out like the initial fill, so the
//        terrain nearest the camera lands first -- and update the subsystem's
//        own OriginVoxel/FluidOriginWorld and the sill-faucet intercept box
//        from the new origin, in the same tick.
//
//        THE TWO NUMBERS. The step is vxc::kFluidRecentreStepVoxels (64 voxels,
//        6.4 m) because that is the addressing quantum and the fill-cell size;
//        the trigger is 1.5 steps (96 voxels, 9.6 m), and the 1.5 is the
//        hysteresis. With round-to-nearest, a move leaves the drift inside
//        +/-32 voxels, so the camera must travel another 64 voxels to trigger
//        again -- no chatter at a boundary, which matters because each recentre
//        costs 64 cells of refill (1/8 of the volume) and a camera oscillating
//        across a hard threshold would pay it every few frames.
//
//        THE COST, HONESTLY. 64 cells at the default budget
//        (voxel.Fluid.Occupancy.RegionsPerTick 8) is 8 ticks, ~0.13 s at 60 Hz,
//        during which the entering slab is solid and water at the leading edge
//        is frozen. A camera moving faster than one step per 8 ticks (6.4 m in
//        0.13 s = 48 m/s) outruns the refill and the queue grows: still safe
//        (unbuilt is solid, water freezes, nothing leaks) and still visible in
//        the perf line's occupancy=<built>/<deferred>, but the water at the
//        leading edge will lag. Flying is the case to measure.
//
//        THE PARTICLE HALF IS NOT OPTIONAL. Positions are origin-relative, so
//        after a recentre the solver MUST dispatch the rebase pass before its
//        next tick -- see TakePendingRebaseDeltaVoxels / AddRebaseParticlesPass
//        and contract item 8. Skipping it teleports the water 6.4 m sideways
//        relative to the terrain, once per recentre, cumulatively.
//
//      SetOriginVoxel remains for the two cases where preserving nothing is
//      correct: the FIRST latch, and a teleport past the window. It invalidates
//      all 16 MiB -- see its own cost note.
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
	SHADER_PARAMETER(FIntVector,  FluidVolumeOriginVoxel) \
	SHADER_PARAMETER(uint32,      FluidVolumeDimVoxels) \
	SHADER_PARAMETER(FUintVector, FluidVolumeWrapOffsetVoxel)

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

// One 64^3 cell that a recentre exposed and the caller must refill. Volume-
// local under the NEW origin, already snapped (64 divides both alignment
// requirements), so the caller can hand it straight to PackRegionBricks and
// UpdateRegion with no further arithmetic.
//
// A separate type from FVoxelFluidOccupancyRegion on purpose: this one carries
// no bits and is an INSTRUCTION rather than a payload. Reusing the region
// struct would produce an object that looks submittable and would be silently
// rejected for a brick-word count of zero.
struct FVoxelFluidOccupancyRefillCell
{
	FIntVector MinVoxel = FIntVector::ZeroValue;
	FIntVector SizeVoxels = FIntVector::ZeroValue;
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

	// The STORAGE coordinate of volume-local (0,0,0): how far the rolling window
	// has slid, modulo the volume. Zero until the first RecentreTo, always a
	// multiple of vxc::kFluidRecentreStepVoxels, always in [0, DimVoxels).
	// Needed by anything that maps a volume-local coordinate to a word index by
	// hand -- notably the verify gate, which must apply the same wrap the GPU
	// applied or it compares two different voxels and calls it a mismatch.
	FIntVector GetWrapOffsetVoxel() const;

	// HARD RESET. Moves the volume and throws EVERY BIT AWAY: all 16 MiB become
	// unbuilt (solid) and the host must refill the whole thing.
	//
	// THE COST, WHICH IS WHY RecentreTo EXISTS. Refilling means packing
	// 64^3 = 262,144 bricks, and any brick not already resident has to be
	// GENERATED to pack it -- against a mesher measured with ~893 bricks/s of
	// spare capacity. So this is a multi-second refill, during which the
	// unfilled part is solid and the water inside it is frozen.
	//
	// USE IT FOR EXACTLY TWO THINGS: the first latch (nothing is built, so
	// nothing is being thrown away) and a teleport (the new window shares no
	// terrain with the old, so a slide would invalidate all of it anyway and
	// cost an extra particle rebase on the way). Everything else -- a camera
	// that WALKED -- is RecentreTo.
	//
	// No-op when the origin is unchanged.
	void SetOriginVoxel(const FIntVector& NewOriginVoxel);

	// SLIDES the window: toroidal recentre (contract item 4). Bits still in view
	// keep their storage slots and stay valid; only the newly exposed cells are
	// invalidated. See the RECENTRING section at the top of this file for the
	// policy the caller should drive this with, including the hysteresis.
	//
	// The delta must be a whole multiple of vxc::kFluidRecentreStepVoxels on
	// EVERY axis -- REFUSED, NOT ROUNDED, with OutError filled. A rounded delta
	// would put the window somewhere other than where the caller believes it is,
	// and every world coordinate the caller then packs would be off by the
	// remainder, which is the failure that reads as terrain.
	//
	// On success:
	//   * the origin is the new one and the wrap offset has advanced;
	//   * OutRefillCells lists every 64^3 cell of the NEW window that must be
	//     packed and submitted, in new-origin volume-local coordinates. It is
	//     EMPTY only when the delta was zero; a caller that ignores it leaves
	//     the entering slab permanently unbuilt (solid), which freezes water at
	//     the leading edge rather than leaking it, but freezes it forever;
	//   * those same cells are queued to be marked unbuilt on the GPU at the
	//     next AddPasses, so the window never collides against the terrain that
	//     used to live in those slots;
	//   * OutDeltaVoxels is the origin's motion, and the particle rebase owes
	//     exactly it (TakePendingRebaseDeltaVoxels, below);
	//   * any queued-but-not-yet-dispatched region is translated to the new
	//     local frame; one that no longer fits is dropped AND its cells are
	//     added to the refill list, so a dropped edit cannot leave pre-edit bits
	//     behind (counted as Stats::RegionsRetranslatedOut).
	//
	// A delta at or past the window on any axis is not a special case: every
	// cell fails the containment test, so every cell enters and the result is a
	// full rebuild reached by the general path rather than branched to. A delta
	// past vxc::kFluidRebaseExactMaxVoxels (167 km) IS refused -- that is a
	// teleport, the particle rebase could not represent it exactly, and
	// SetOriginVoxel is the honest call for it.
	//
	// Game thread (same lock as everything else here).
	bool RecentreTo(const FIntVector& NewOriginVoxel,
	                TArray<FVoxelFluidOccupancyRefillCell>& OutRefillCells,
	                FIntVector& OutDeltaVoxels, FString& OutError);

	// True until the first fill after a clear. Purely so a caller can tell
	// "the volume is solid because nothing has been built" from "the volume is
	// solid because the terrain is solid" -- two states that look identical
	// from inside the shader and produce very different bug reports.
	bool IsFullyUnbuilt() const;

	// The per-cell form of the question above, and the one an EMITTER needs.
	//
	// IsFullyUnbuilt() goes false after the FIRST region lands, but the initial
	// fill is centre-out over 512 cells and takes seconds, so for most of that
	// window most of the volume is still all-ones (solid). A faucet that emitted
	// into one of those cells put particles inside notional rock: they froze in
	// mid-air at the edge of the built box, which is half of what the first
	// playtest saw as a "square plane of hovering water".
	//
	// Granularity is the 64^3 initial-fill cell (8x8x8 = 512 of them), and a
	// cell counts as built only when an applied region COVERED IT WHOLE -- a
	// small edit region inside an unbuilt cell does not promote it. Takes a
	// WORLD voxel; false when it lies outside the volume at all, which for an
	// emitter is the same answer for the same reason (outside the box there is
	// no collision data, so anything spawned there is spawned blind).
	//
	// Any thread. Written under the same lock as the queue, once per flush.
	bool IsRegionBuilt(const FIntVector& WorldVoxel) const;

	// Edge of the built-tracking cell, in voxels. Matches the host's
	// initial-fill granularity by construction (static_assert in the .cpp).
	static constexpr int32 BuiltCellVoxels = 64;
	static constexpr int32 BuiltCellsPerAxis = DimVoxels / BuiltCellVoxels;

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
		// --- toroidal recentring (contract item 4) ---
		uint64 Recentres = 0;             // successful RecentreTo calls that moved
		uint64 RecentresRefused = 0;      // misaligned delta
		uint64 CellsEntered = 0;          // 64^3 cells invalidated + handed back
		uint64 CellsMarkedUnbuilt = 0;    // mark dispatches actually issued
		// Queued regions the recentre could not translate into the new frame.
		// NOT a silent loss: their cells go into the refill list. A number here
		// with no matching CellsEntered growth would be the bug.
		uint64 RegionsRetranslatedOut = 0;
		// Particle rebase deltas handed to a caller. A recentre with no matching
		// rebase is water teleported sideways relative to the terrain, so the
		// two counters are meant to be read together: Recentres should never run
		// away from this.
		uint64 RebaseDeltasTaken = 0;
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

	// --- the particle half of a recentre (contract item 8) ----------------

	// The origin motion the particles still owe, in voxels, accumulated over
	// every RecentreTo since the last call, and ZEROED by this call. Zero means
	// nothing to do.
	//
	// TAKE-AND-CLEAR rather than a read plus a separate acknowledge, because the
	// failure mode of the two-call form is skipping the acknowledge and rebasing
	// the same delta every frame -- water sliding away from the terrain at
	// 6.4 m per frame, which looks like the solver exploding. One call, one
	// obligation, and the obligation is in the caller's hands the moment it
	// returns non-zero.
	//
	// ACCUMULATED, so two recentres between solver ticks cost one pass and one
	// rounding rather than two.
	FIntVector TakePendingRebaseDeltaVoxels();

	// Adds FluidRebaseParticlesMain: subtracts DeltaVoxels * 10 UU from every
	// stored particle position, over SlotCount slots. Returns false and adds
	// nothing when the delta is zero, the slot count is zero, or the RHI cannot
	// run it.
	//
	// STATIC, and takes the particle UAV rather than reaching for it: the volume
	// does not own the particle buffer and must not start. The solver passes its
	// own ParticlesRW UAV and its own slot bound.
	//
	// WHERE IT GOES IN THE FRAME. Between solver ticks -- after the previous
	// tick's finalize, before the next tick's integrate. The solver's other
	// position buffers are per-tick transients rebuilt from ParticlesRW, so they
	// need no rebase and MUST NOT be rebased; a pass landing mid-tick would
	// shift the stored positions out from under a sorted domain built from the
	// old ones. In practice: first thing in the solver's own graph build, right
	// after the occupancy AddPasses that the ordering guard already pins.
	static bool AddRebaseParticlesPass(FRDGBuilder& GraphBuilder, FRDGBufferUAVRef ParticlesRW,
	                                   uint32 SlotCount, const FIntVector& DeltaVoxels);

	// Fills a VOXEL_FLUID_OCCUPANCY_PARAMETERS() block. Templated on the
	// parameter struct so this header does not need to know any consumer's
	// type -- every consumer has the same three field names because they came
	// from the macro above.
	template <typename ParametersType>
	void BindShaderParameters(FRDGBuilder& GraphBuilder, ParametersType& Parameters)
	{
		const FIntVector Wrap = GetWrapOffsetVoxel();
		Parameters.FluidOccupancyBits = CreateBitsSRV(GraphBuilder);
		Parameters.FluidVolumeOriginVoxel = GetOriginVoxel();
		Parameters.FluidVolumeDimVoxels = uint32(DimVoxels);
		Parameters.FluidVolumeWrapOffsetVoxel = FUintVector(uint32(Wrap.X), uint32(Wrap.Y), uint32(Wrap.Z));
	}

private:
	// Out of line, and the reason this header only needs the forward
	// declaration of FRDGPooledBuffer -- same argument as
	// FVoxelGpuQuadPayload's destructor (VoxelGpuQuadPayload.h:60-72).
	FRDGBufferSRVRef CreateBitsSRV(FRDGBuilder& GraphBuilder);

	TRefCountPtr<FRDGPooledBuffer> PooledBits;

	// Marks a built cell of the coarse grid IsRegionBuilt() answers from.
	// Called with QueueLock held.
	void MarkCellsBuilt_Locked(const FIntVector& MinVoxel, const FIntVector& SizeVoxels);

	// Reads/clears one cell of that grid by cell coordinate. Called with
	// QueueLock held; used by the recentre to SHIFT the grid rather than clear
	// it (see BuiltCellBits).
	bool GetCellBuilt_Locked(const FIntVector& Cell) const;

	FIntVector OriginVoxel = FIntVector::ZeroValue;
	// The rolling window's storage offset. Only RecentreTo advances it;
	// SetOriginVoxel resets it to zero, because a full clear makes every slot
	// unbuilt and there is then nothing for an offset to preserve -- and zero is
	// the offset that makes the volume easiest to reason about in a dump.
	FIntVector WrapOffsetVoxel = FIntVector::ZeroValue;
	// Owed to the particles: the sum of every RecentreTo delta not yet taken.
	FIntVector PendingRebaseDeltaVoxels = FIntVector::ZeroValue;
	bool bFullyUnbuilt = true;
	bool bClearPending = true;

	// 8x8x8 = 512 bits, one per 64^3 cell. Cleared wholesale by SetOriginVoxel
	// and by the pending clear, for the same reason PendingRegions is: a hard
	// reset makes every bit in the volume unbuilt.
	//
	// A TOROIDAL RECENTRE DOES NOT CLEAR IT. The grid is in LOCAL space and a
	// slide only renames local coordinates, so RecentreTo SHIFTS these bits by
	// the window step and clears only the entering cells. Clearing wholesale
	// would be safe but over-conservative in a way that is visible in play:
	// every faucet defers on IsRegionBuilt through a full refill it no longer
	// needs, which is the "square plane of hovering water" symptom arriving by
	// a second route. (This is the merge the built-cell work asked for.)
	static constexpr int32 BuiltCellWords = (BuiltCellsPerAxis * BuiltCellsPerAxis *
	                                         BuiltCellsPerAxis + 63) / 64;
	uint64 BuiltCellBits[BuiltCellWords] = {};
	static_assert(BuiltCellVoxels == vxc::kFluidRecentreStepVoxels,
	              "the built-cell grid and the recentre quantum must be the same "
	              "grid, or a recentre shifts the built bits by a fractional cell");

	int32 MaxRegionsPerFlush = 32;

	mutable FCriticalSection QueueLock;
	TArray<FVoxelFluidOccupancyRegion> PendingRegions;
	// Cells to mark unbuilt at the next AddPasses, volume-local. Applied WHOLE
	// and before the fills, never budgeted: a fill for an entering cell that
	// landed in the same flush would otherwise be erased by the mark that was
	// meant to precede it, which is a stale-terrain bug that only appears when
	// the queue happens to be short.
	TArray<FVoxelFluidOccupancyRefillCell> PendingUnbuiltCells;
	FStats Stats;
};
