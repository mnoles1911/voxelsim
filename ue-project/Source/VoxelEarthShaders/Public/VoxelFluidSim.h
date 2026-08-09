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
	// VoxelFluidContract.ush:45-47 -- flag bits of asuint(P0.w).
	inline constexpr uint32 kFlagAlive = 1u << 0;
	inline constexpr uint32 kFlagDespawnBasin = 1u << 1;
	inline constexpr uint32 kFlagDespawnBoundary = 1u << 2;

	// CPU mirror of the 32-byte AoS particle (VoxelFluidContract.ush:48-52),
	// used only to decode the debug-draw readback.
	struct FParticleCPU
	{
		FVector3f Pos;   // P0.xyz, origin-relative UU
		uint32 Flags;    // asuint(P0.w)
		FVector3f Vel;   // P1.xyz, UU/s
		float BatchId;   // P1.w
	};

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
	uint32 Seed = 0;
	float BatchId = 0.0f;
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
	// Fallback collision plane (origin-relative), used only while
	// VOXEL_FLUID_HAS_COLLISION is 0 -- see VoxelFluidSim.usf.
	float GroundZLocalUU = 0.0f;
	// Despawn box half extent around the fluid origin. Default is half the
	// contract's 512-voxel active region edge (51.2 m cube -> 2560 UU).
	float BoundaryHalfExtentUU = 2560.0f;
	FVoxelFluidSpawnRequest Spawn;
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
};

namespace VoxelFluidSim
{
	// Builds and executes one sim frame's render graph: spawn, integrate,
	// hash-grid build (count + 3-kernel scan + scatter), Iterations x
	// (density/lambda + delta-p with collision resolve), finalize (velocity,
	// despawn, conservation counters), counts readback, optional debug
	// readback, GPU timing -- then polls all outstanding readbacks/queries
	// into the snapshot. Render thread only.
	VOXELEARTHSHADERS_API void TickRenderThread(FRHICommandListImmediate& RHICmdList,
	                                            FVoxelFluidSimState& State,
	                                            const FVoxelFluidSimTickArgs& Args);

	// Drops every RHI resource the state holds. Must be the last render-thread
	// touch; the subsystem enqueues this and then releases its reference.
	VOXELEARTHSHADERS_API void ReleaseRenderThread(FVoxelFluidSimState& State);
}
