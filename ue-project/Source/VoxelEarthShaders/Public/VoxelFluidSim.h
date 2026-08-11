// VoxelFluidSim.h -- render-thread state and pass builder for the Phase 0
// PBF solver spike (docs/water-rearchitecture-plan-2026-08-09.md, spike a).
//
// THIS IS THE PROJECT'S FIRST PERSISTENT GPU SIM STATE. Every GPU buffer the
// project has kept alive so far is either a snapshot of CPU-owned data (the
// quad pool -- the CPU shadow can rebuild it) or per-job transient (the mesh
// job manager's quad buffers, freed at delivery). The particle buffer is
// neither: the GPU's contents ARE the simulation, nothing on the CPU can
// reconstruct them, and they must survive an arbitrary number of frames and
// FRDGBuilder lifetimes. The ownership rules that follow from that:
//
//   - Buffers are TRefCountPtr<FRDGPooledBuffer>, allocated ONCE on the render
//     thread (AllocatePooledBuffer) and re-registered into each frame's graph
//     with RegisterExternalBuffer -- the same wrap FVoxelGpuPoolBuffers uses
//     (VoxelGpuPoolComponent.h:73-90) so RDG tracks transitions and hands the
//     buffer back in a known state each frame.
//   - The state object is held by TSharedPtr<..., ESPMode::ThreadSafe>. The
//     game thread (subsystem) holds one reference; every render command
//     captures its own. Destruction therefore happens wherever the LAST
//     reference drops -- which the subsystem forces to be the render thread by
//     enqueueing a release command before dropping its own reference
//     (mirroring UVoxelGpuPoolComponent::BeginDestroy's reasoning: RHI
//     resources must die on the render thread).
//   - The game thread NEVER touches the buffer members. It reads only the
//     snapshot block at the bottom, which is lock-protected.
//
// Particles are presentation, never authority -- see VoxelFluidContract.ush
// (the layout contract this mirrors) and the plan's determinism section.

#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "RHIGPUReadback.h"
#include "RenderGraphDefinitions.h"
#include "Templates/RefCounting.h"

class FRDGPooledBuffer;
class FRHICommandListImmediate;
class FVoxelFluidOccupancyVolume;

// ---- contract mirror -------------------------------------------------------
// C++ cannot include VoxelFluidContract.ush, so the constants the CPU side
// needs are mirrored here, each citing its line in the contract. The particle
// layout is additionally pinned by a static_assert in VoxelFluidSim.cpp; the
// scalar mirrors are pinned by the automation tests at the bottom of that
// file. Change the contract or these together, or a test fails -- that is the
// point.
namespace VoxelFluidSim
{
	// VoxelFluidContract.ush:30 -- one particle per 10 cm voxel at rest.
	inline constexpr float kRestSpacingUU = 10.0f;
	// VoxelFluidContract.ush:33 -- kernel support radius h == hash cell size.
	inline constexpr float kKernelHUU = 25.0f;
	// VoxelFluidContract.ush:37 -- buffer sizing cap.
	inline constexpr uint32 kMaxParticles = 300u * 1024u;
	// VOXEL_FLUID_BASIN_MASK_N -- cells per axis of the basin sink's extent
	// grid (contract item 6, amended). 32 is also the bit width of a row, so
	// the mask is exactly N uint32s and N may not exceed 32.
	inline constexpr int32 kBasinExtentMaskN = 32;
	// VoxelFluidContract.ush:45-47 -- flag bits of asuint(P0.w).
	inline constexpr uint32 kFlagAlive = 1u << 0;
	inline constexpr uint32 kFlagDespawnBasin = 1u << 1;
	inline constexpr uint32 kFlagDespawnBoundary = 1u << 2;

	// CPU mirror of the 32-byte AoS particle (VoxelFluidContract.ush:48-52),
	// used only to decode the debug-draw readback.
	struct FParticleCPU
	{
		FVector3f Pos;      // P0.xyz, origin-relative UU
		uint32 Flags;       // asuint(P0.w)
		FVector3f Vel;      // P1.xyz, UU/s
		// P1.w -- age stamp, game seconds: the last time this particle was
		// moving or born (contract item 9, stagnant-only aging).
		float AgeStampSec;
	};

	// Age-sink scale floor, mirror of VOXEL_FLUID_AGE_SCALE_FLOOR
	// (VoxelFluidSim.usf): the population-scaled effective max age never drops
	// below this fraction of MaxAgeSec.
	inline constexpr float kAgeScaleFloor = 0.1f;

	// CPU mirror of the finalize kernel's effective-max-age math (contract
	// item 9): full MaxAgeSec up to PopStart, linear down to the floor at
	// PopEnd, floored beyond. One definition here so the automation test pins
	// the semantics the shader implements.
	inline float EffectiveMaxAgeSec(uint32 Pop, uint32 PopStart, uint32 PopEnd, float MaxAgeSec)
	{
		float Scale = 1.0f;
		if (Pop > PopStart)
		{
			const float T = float(Pop - PopStart) / float(FMath::Max(PopEnd - PopStart, 1u));
			Scale = FMath::Max(1.0f - T, kAgeScaleFloor);
		}
		return MaxAgeSec * Scale;
	}

	// CPU mirror of the finalize kernel's stagnant-refresh rule (contract
	// item 9, stagnant-only aging): the age stamp P1.w is rewritten to Now on
	// every frame the derived speed EXCEEDS the stagnant threshold (strict >,
	// matching the shader), so only resting water keeps an old stamp and can
	// age out. One definition here so the automation test pins the semantics
	// the shader implements.
	inline float RefreshAgeStamp(float StampSec, float NowSec, float SpeedUU, float StagnantSpeedUU)
	{
		return SpeedUU > StagnantSpeedUU ? NowSec : StampSec;
	}

	// The shader's kernel-coefficient literals (VoxelFluidSim.usf), re-derived
	// here in double precision. An automation test asserts each literal is
	// within 1e-5 relative of its derivation, so the mirror cannot silently
	// drift -- the exact failure mode the contract header warns about.
	VOXELEARTHSHADERS_API double Poly6CoeffUU();      // 315 / (64 pi h^9)
	VOXELEARTHSHADERS_API double SpikyGradCoeffUU();  // 45 / (pi h^6)

	// W_poly6(r^2) with unit particle mass, double precision -- the reference
	// implementation the tests pin the shader's constants against.
	VOXELEARTHSHADERS_API double Poly6UU(double R2);

	// Rest density: sum of W_poly6 over the rest lattice (all integer offsets
	// of kRestSpacingUU within radius kKernelHUU, self included). Computed
	// once on the CPU and passed to the kernels as the RestDensity parameter
	// so the shader never re-derives it. For h = 2.5x spacing the lattice has
	// exactly 81 sites and the result lands within ~0.5% of the ideal number
	// density 1/spacing^3 -- both asserted by tests.
	VOXELEARTHSHADERS_API float ComputeRestDensity();
	VOXELEARTHSHADERS_API int32 RestLatticeNeighbourCount();

	// Smallest lattice edge whose cube holds Count particles (spawn block).
	VOXELEARTHSHADERS_API uint32 CubeEdgeForCount(uint32 Count);

	// The conservation predicate, verbatim from VoxelFluidContract.ush:60-62:
	// spawnedTotal - despawnedBasin - despawnedBoundary == alive. One
	// definition, used by the subsystem's per-frame assert AND the tests, so
	// the check in the shipping path is the tested one.
	inline bool CheckConservation(uint32 Alive, uint32 DespawnedBasin,
	                              uint32 DespawnedBoundary, uint32 SpawnedTotal)
	{
		return SpawnedTotal - DespawnedBasin - DespawnedBoundary == Alive;
	}
}

// What one frame's readback said. Generation is a monotone frame stamp so
// consumers can tell "new data" from "same data again"; bValid false means NO
// readback has ever landed -- deliberately distinct from a snapshot full of
// zeros, per the plan's ran-flag rule ("Every stage writes a ran-flag
// distinguishable from 'found nothing'").
struct FVoxelFluidCountsSnapshot
{
	uint32 Alive = 0;
	uint32 DespawnedBasin = 0;
	uint32 DespawnedBoundary = 0;
	uint32 SpawnedTotal = 0;
	// Age recycles (contract item 9): a SUBSET of DespawnedBoundary, telemetry
	// only -- never a term of the conservation predicate.
	uint32 RecycledAge = 0;
	uint64 Generation = 0;
	// Last completed GPU timing of the sim pass span, milliseconds. Negative
	// means "no timing available" (queries unsupported or none resolved yet)
	// -- again distinct from a real 0.00.
	float SimGpuMs = -1.0f;
	bool bValid = false;
};

// One spawn request, executed as a single FluidSpawnMain dispatch. Count == 0
// means no spawn pass this frame.
struct FVoxelFluidSpawnRequest
{
	uint32 Count = 0;
	uint32 Mode = 0;           // 0 = block lattice (dam break), 1 = faucet
	FVector3f CenterLocalUU = FVector3f::ZeroVector;
	FVector3f VelocityUU = FVector3f::ZeroVector;
	// Faucet mode only: unit direction of the emission line (host-computed
	// horizontal perpendicular of VelocityUU). Zero = legacy point/disc.
	FVector3f JitterDirUU = FVector3f::ZeroVector;
	uint32 Seed = 0;
	// Game-time seconds at the spawn dispatch, stamped into every spawned
	// particle's P1.w -- the age stamp's initial value; finalize refreshes it
	// while the particle keeps moving (contract item 9).
	float SpawnTimeSec = 0.0f;
};

// Everything one sim tick needs, marshalled game -> render thread by value.
struct FVoxelFluidSimTickArgs
{
	// Clamped by the caller; a tick with Dt <= 0 must not be issued (velocity
	// derivation divides by it).
	float Dt = 1.0f / 60.0f;
	// PBF constraint iterations (2-4 per the design; clamped 1..8 by caller).
	int32 Iterations = 3;
	// Upper bound on ever-allocated particle slots, i.e. the dispatch width of
	// every per-particle kernel. CPU-tracked (cumulative spawns, clamped to
	// kMaxParticles): it can only over-estimate the GPU's alloc high water,
	// never under-estimate it, which is the safe direction -- a dead slot
	// costs one flag test, a missed live slot would be a frozen particle.
	uint32 SimSlotBound = 0;
	// Fallback collision plane (origin-relative). DEAD in shipping compiles:
	// VOXEL_FLUID_HAS_COLLISION is 1 (contract item 5) and the shader strips
	// the parameter. Kept so an isolated no-collision compile of the .usf
	// still has its plane.
	float GroundZLocalUU = 0.0f;
	// Despawn box: half extent around BoundaryCenterLocalUU. With collision
	// live the fluid origin is the occupancy volume's MIN corner, so the box
	// centre is (2560, 2560, 2560) -- uploaded, not assumed (contract item 5).
	float BoundaryHalfExtentUU = 2560.0f;
	FVector3f BoundaryCenterLocalUU = FVector3f::ZeroVector;

	// THE COLLISION VOLUME. When set, TickRenderThread calls
	// Occupancy->AddPasses on ITS OWN FRDGBuilder before any solver pass and
	// binds the result into DeltaPos/Finalize -- the "same graph, fills
	// first" ordering rule, enforced by a checkf against
	// FStats::AddPassesCount. When null (or the RHI refuses the volume), the
	// sim SKIPS the tick and counts it: with the collision define baked on
	// there is no fallback path to silently run without terrain.
	// LIFETIME: raw pointer; the enqueuing game thread captures its owning
	// TSharedPtr in the same render-command lambda, which is what keeps this
	// alive for the duration of the render-thread call.
	FVoxelFluidOccupancyVolume* Occupancy = nullptr;

	// Basin sink (contract item 6): one basin's clipped box + live datum +
	// TRUE EXTENT, all origin-local UU. bBasinSinkEnabled false leaves the
	// kernel's test off, which is also what the host must do when the extent
	// will not decode -- the bbox is not a safe fallback, it is the round-17
	// defect (it deleted the river; see the contract's item 6 amendment).
	bool bBasinSinkEnabled = false;
	FVector3f BasinBoxMinLocalUU = FVector3f::ZeroVector;
	FVector3f BasinBoxMaxLocalUU = FVector3f::ZeroVector;
	float BasinDatumZLocalUU = 0.0f;
	// The extent grid: kBasinExtentMaskN^2 bits, row cy in BasinExtentRows[cy],
	// bit cx (LSB = smallest x). Cell (0,0)'s min corner is
	// BasinMaskOriginLocalUU; a cell is 1/BasinMaskInvCellUU UU across.
	FVector2f BasinMaskOriginLocalUU = FVector2f::ZeroVector;
	float BasinMaskInvCellUU = 0.0f;
	uint32 BasinExtentRows[VoxelFluidSim::kBasinExtentMaskN] = {};

	// Age sink (contract item 9): recycle particles STAGNANT for longer than
	// the population-scaled max age as boundary despawns. MaxAgeSec <= 0
	// disables (cvar voxel.Fluid.MaxAgeSec 0). NowSeconds is the same
	// game-time clock the spawn requests stamp, uploaded per tick.
	// AgePopStart/End are the population band over which the effective age
	// shrinks to the floor; the subsystem anchors AgePopEnd at its emission
	// backpressure ramp start. StagnantSpeedUU is the moving/resting divide:
	// finalize refreshes a particle's age stamp while its speed exceeds this
	// (cvar voxel.Fluid.StagnantSpeedUU; threshold rationale at the uniform's
	// declaration in VoxelFluidSim.usf).
	float NowSeconds = 0.0f;
	float MaxAgeSec = 0.0f;
	float StagnantSpeedUU = 15.0f;
	uint32 AgePopStart = 0;
	uint32 AgePopEnd = 0;

	// Spawn dispatches this tick, in order: at most one dam-break block plus
	// every faucet (camera faucet, headwater faucets, sill faucets) that owes
	// particles this frame. Each entry is one FluidSpawnMain dispatch; the
	// subsystem enforces the per-tick particle budget before queueing.
	TArray<FVoxelFluidSpawnRequest, TInlineAllocator<8>> Spawns;

	// voxel.Fluid.Occupancy.Verify: also copy the whole 16 MiB occupancy
	// buffer back this tick (single-buffered; skipped while one is in flight)
	// so the game thread can byte-compare a region against the CPU reference.
	bool bVerifyOccupancy = false;

	// Debug-draw support: also read back the first DebugSlotCount particle
	// structs this frame (bounded, see FVoxelFluidSimState::kDebugMaxSlots).
	bool bReadbackDebugSlots = false;
	uint32 DebugSlotCount = 0;
};

// The persistent render-thread state. Constructed empty on the game thread;
// every other member touch happens on the render thread inside
// VoxelFluidSim::TickRenderThread / ReleaseRenderThread.
class VOXELEARTHSHADERS_API FVoxelFluidSimState
{
public:
	// Declared, not defaulted inline: the FRDGPooledBuffer references below
	// are held through a forward declaration, so the destructor has to be
	// instantiated in the one translation unit that has the complete type --
	// the exact arrangement FVoxelGpuPoolBuffers documents
	// (VoxelGpuPoolComponent.h:56-60).
	FVoxelFluidSimState();
	~FVoxelFluidSimState();

	// Counts readbacks in flight at once. 4 uints each; at 3-deep the poll can
	// lag the enqueue by two full frames before a tick has to skip its enqueue,
	// which does not happen in practice -- a skip is counted, not silent.
	static constexpr int32 kNumCountsReadbacks = 3;
	// Debug readback slot cap: 8192 slots x 32 B = 256 KB per frame, debug
	// only. Covers the <=5k-particle DrawDebugPoint path with headroom for
	// freelist holes; a sim whose high water exceeds this simply stops
	// offering debug positions for the slots above it (reported, not silent).
	static constexpr uint32 kDebugMaxSlots = 8192;
	// GPU timestamp query pairs in flight (RQT_AbsoluteTime begin/end).
	static constexpr int32 kNumTimingPairs = 4;

	// ---- render-thread-only members ----------------------------------------

	// Persistent buffers. Particles/counts are the contract state; FreeList/
	// SlotCounters are the solver's slot allocator (VoxelFluidSim.usf,
	// FluidSpawnMain). Allocated lazily on first tick; bBuffersInitialized
	// gates the one-time clear passes.
	TRefCountPtr<FRDGPooledBuffer> Particles;
	TRefCountPtr<FRDGPooledBuffer> Counts;
	TRefCountPtr<FRDGPooledBuffer> FreeList;
	TRefCountPtr<FRDGPooledBuffer> SlotCounters;
	bool bBuffersInitialized = false;

	// Counts readback ring. bInFlight distinguishes "holds a pending copy"
	// from "free to reuse"; Generation stamps which tick the copy belongs to
	// so late arrivals can never regress the snapshot.
	struct FCountsReadback
	{
		TUniquePtr<FRHIGPUBufferReadback> Readback;
		uint64 Generation = 0;
		bool bInFlight = false;
	};
	FCountsReadback CountsRing[kNumCountsReadbacks];
	// Ticks that found no free readback slot. Non-zero is a symptom worth a
	// log line (the GPU is more than kNumCountsReadbacks frames behind), and a
	// counter is how it stays visible.
	uint64 CountsReadbackSkips = 0;

	// Debug particle readback -- single-buffered on purpose: debug draw
	// tolerates a stale frame, and one 256 KB copy in flight is enough.
	TUniquePtr<FRHIGPUBufferReadback> DebugReadback;
	uint32 DebugReadbackSlots = 0;
	uint64 DebugReadbackGeneration = 0;
	bool bDebugReadbackInFlight = false;

	// Occupancy verify readback (voxel.Fluid.Occupancy.Verify): the whole
	// 16 MiB bit volume, single-buffered, debug only. Copied AFTER the frame's
	// fill passes so the snapshot is exactly what this tick's solver collided
	// against.
	TUniquePtr<FRHIGPUBufferReadback> OccupancyReadback;
	uint64 OccupancyReadbackGeneration = 0;
	bool bOccupancyReadbackInFlight = false;

	// Ticks refused because Args.Occupancy was null or the volume could not
	// register (unsupported RHI). Non-zero belongs in the perf line: with the
	// collision define baked on, "sim silently ran without terrain" is not a
	// state this system is allowed to have.
	uint64 TicksSkippedNoOccupancy = 0;

	// GPU timing: RQT_AbsoluteTime query pairs bracketing the sim pass span,
	// issued as untracked NeverCull RDG passes at the start/end of the graph.
	// Results are microseconds; polled non-blocking, so a value is always one
	// or more frames stale -- fine for a 1 Hz perf line.
	struct FTimingPair
	{
		FRenderQueryRHIRef Begin;
		FRenderQueryRHIRef End;
		bool bInFlight = false;
	};
	FTimingPair TimingRing[kNumTimingPairs];

	// Monotone tick counter, render thread. Doubles as readback generation.
	uint64 TickGeneration = 0;

	// THE RENDERER'S ACCESSOR (screen-space fluid pass, VoxelFluidRender.*):
	// the dispatch/instance width the last executed sim tick ran at, i.e. how
	// many slots of Particles are worth splatting. Written by TickRenderThread,
	// read by FVoxelFluidRenderExtension::PrePostProcessPass_RenderThread --
	// both render thread, no lock. Stays at its last value across skipped
	// ticks (the particles didn't move, but they still exist and still draw);
	// 0 until the first real tick, which is the renderer's "nothing yet" gate.
	// The renderer reads Particles + this and NOTHING else of the sim state.
	uint32 RenderSlotBound = 0;

	// ---- cross-thread snapshot (the ONLY game-thread-readable part) --------

	// Lock-protected because writer (render thread, on readback completion)
	// and reader (game thread, every tick) are different threads and the
	// payload is multi-field. Contention is one lock per frame per side.
	mutable FCriticalSection SnapshotLock;
	FVoxelFluidCountsSnapshot LatestCounts;
	// Alive particle positions (world-relative to the fluid origin) from the
	// most recent completed debug readback. Empty when debug draw is off.
	TArray<FVector3f> DebugAlivePositionsLocal;
	uint64 DebugSnapshotGeneration = 0;

	// The landed occupancy-verify snapshot (all kFluidVolumeWords words), and
	// which tick it was copied on. Consumed by MOVE (TakeOccupancyVerifyWords)
	// because it is 16 MiB and the game thread compares it once.
	TArray<uint32> OccupancyVerifyWords;
	uint64 OccupancyVerifySnapshotGeneration = 0;

	// Game-thread accessors (copy out under the lock).
	FVoxelFluidCountsSnapshot GetLatestCounts() const
	{
		FScopeLock Lock(&SnapshotLock);
		return LatestCounts;
	}
	void GetDebugPositions(TArray<FVector3f>& Out, uint64& OutGeneration) const
	{
		FScopeLock Lock(&SnapshotLock);
		Out = DebugAlivePositionsLocal;
		OutGeneration = DebugSnapshotGeneration;
	}
	// Moves the landed verify snapshot out (empties the stored one). Returns
	// false when no NEW snapshot has landed since the last take.
	bool TakeOccupancyVerifyWords(TArray<uint32>& Out, uint64& OutGeneration, uint64 LastTakenGeneration)
	{
		FScopeLock Lock(&SnapshotLock);
		if (OccupancyVerifySnapshotGeneration == 0 ||
		    OccupancyVerifySnapshotGeneration == LastTakenGeneration || OccupancyVerifyWords.Num() == 0)
		{
			return false;
		}
		Out = MoveTemp(OccupancyVerifyWords);
		OccupancyVerifyWords.Reset();
		OutGeneration = OccupancyVerifySnapshotGeneration;
		return true;
	}
	// Skipped-tick counter, game-thread readable (see TicksSkippedNoOccupancy).
	uint64 GetTicksSkippedNoOccupancy() const
	{
		FScopeLock Lock(&SnapshotLock);
		return TicksSkippedNoOccupancySnapshot;
	}
	uint64 TicksSkippedNoOccupancySnapshot = 0; // written under SnapshotLock by the render thread
};

namespace VoxelFluidSim
{
	// Adds one sim frame's passes to the RENDERER'S OWN graph: spawn,
	// integrate, hash-grid build (count + 3-kernel scan + scatter),
	// Iterations x (density/lambda + delta-p with collision resolve),
	// finalize (velocity, despawn, conservation counters), counts readback,
	// optional debug readback -- then polls outstanding readbacks into the
	// snapshot. Render thread only, called from the view extension's
	// PreRenderViewFamily_RenderThread.
	//
	// THIS USED TO BE TickRenderThread, WHICH BUILT AND EXECUTED ITS OWN
	// FRDGBuilder FROM AN ENQUEUED RENDER COMMAND -- and UE 5.8 killed the
	// editor on the very first Execute with the RDG breadcrumb sentinel
	// assert (RenderGraphBuilder.cpp:1772), reproducibly, with every custom
	// pass stripped back. The standalone-builder pattern is not sanctioned
	// where we ran it; riding the scene renderer's builder is, and it also
	// makes sim-before-render ordering structural instead of enqueued-and-
	// hoped. Measured, not theorised: the 100k gate run crashed 2 s in on
	// the old shape and survives on this one.
	VOXELEARTHSHADERS_API void AddSimPasses(FRDGBuilder& GraphBuilder,
	                                        FVoxelFluidSimState& State,
	                                        const FVoxelFluidSimTickArgs& Args);

	// Whether the caller must post a sim tick this frame. THE GATE IS THE
	// ORIGIN, NEVER THE PARTICLE COUNT, and it is a named function precisely so
	// that the second cannot creep back into it.
	//
	// THE DEADLOCK THIS EXISTS TO PREVENT, measured 2026-08-10
	// (Saved/owner-playtest-round3.log). The occupancy volume's clear and its
	// queued region fills ride the SOLVER's graph -- AddSimPasses is the only
	// caller of FVoxelFluidOccupancyVolume::AddPasses -- while every faucet
	// refuses to emit into occupancy that is not built yet
	// (FVoxelFluidOccupancyVolume::IsRegionBuilt). Gate the tick on "has
	// anything ever spawned" and the two rules close a loop with no way in:
	// nothing spawns because no cell is built, and no cell is built because
	// nothing spawned. The log ran 8.5 minutes with occupancy=512/0 (all 512
	// cells packed and handed to the volume), spawned=0, deferredNoOccupancy=6
	// for 174,840 faucet-ticks, and NOT ONE LogVoxelFluidOccupancy line --
	// AddPasses never ran at all, so the built-cell mask never had a bit set.
	//
	// An idle tick is cheap and it is not a no-op: it is the tick that builds
	// the volume the first faucet needs (see AddSimPasses, which runs the
	// occupancy passes BEFORE its own nothing-to-simulate early-out).
	inline bool ShouldTickSim(bool bOriginLatched, float Dt)
	{
		return bOriginLatched && Dt > 0.0f;
	}

	// Drops every RHI resource the state holds. Must be the last render-thread
	// touch; the subsystem enqueues this and then releases its reference.
	VOXELEARTHSHADERS_API void ReleaseRenderThread(FVoxelFluidSimState& State);
}
