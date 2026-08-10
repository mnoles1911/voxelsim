// VoxelFluidSubsystem.h -- game-thread host for the PBF solver, now carrying
// the Phase 3 INTEGRATION duties (docs/water-rearchitecture-plan-2026-08-09.md
// Phase 3, wired by the integration pass):
//
//   * owns the GPU collision volume (FVoxelFluidOccupancyVolume), feeds it an
//     initial fill around the fluid origin and keeps it in step with terrain
//     edits/arrivals via UVoxelWorldSubsystem::SetFluidTerrainDirtyListener;
//   * runs the faucet/sink lifecycle v1: headwater faucets (baked heads, or
//     the bv23 fallback graph), sill faucets (basin-ledger spill events held
//     by UVoxelWaterSubsystem's intercept), the basin despawn sink (credits
//     the ledger at exactly 255 units per particle -- the constant and its
//     factor-of-255 test live in voxelcore/fluidlifecycle.h) and the boundary
//     despawn sink (injects into the routing graph);
//   * extends the conservation discipline across the particle/scalar seam:
//     every readback reconciles despawn counters against ledger credits and
//     graph injections, with pending (refused-but-retried) units carried, not
//     dropped;
//   * the GPU/CPU occupancy verify gate (voxel.Fluid.Occupancy.Verify).
//
// Everything is behind voxel.Fluid.Enable (default 0), so the subsystem is
// inert in every session that does not ask for it.
//
// Console surface (all defined in the .cpp):
//   voxel.Fluid.Enable 1            -- create the sim; 0 tears it down
//   voxel.Fluid.Spawn [count]       -- dam-break block above the camera (default 5000)
//   voxel.Fluid.Emit <perSec>       -- camera faucet stream; 0 stops it
//   voxel.Fluid.Iterations N        -- PBF constraint iterations (default 3, clamp 1..8)
//   voxel.Fluid.DebugDraw 1         -- DrawDebugPoint for <= 5k alive particles
//   voxel.Fluid.DebugFaucets        -- beacon at every active faucet (default 1;
//                                      magenta = springs/sills/camera, orange = edge inflows)
//   voxel.Fluid.Faucets 1           -- headwater + sill faucets and the sinks (default 0)
//   voxel.Fluid.Faucets.DefaultQ    -- m^3/yr for heads with no baked Q (default 8e6 ~= 253/s)
//   voxel.Fluid.MaxSpawnPerTick     -- shared faucet emit budget, particles (default 4096)
//   voxel.Fluid.MaxAgeSec           -- seconds a particle lives before it is recycled
//                                      downstream as scalars (default 75; 0 = off).
//                                      The round-6 fix: pocketed water that never
//                                      reaches a spatial sink turns over by age, so
//                                      emission == recycling at steady state and
//                                      faucets never throttle to zero.
//   voxel.Fluid.Occupancy.RegionsPerTick / .PackMsPerTick -- fill budget
//   voxel.Fluid.Occupancy.Verify 1  -- continuous GPU-vs-CPU region compare
//
// ORIGIN DOCTRINE (contract items 1/5): the fluid origin IS the occupancy
// volume's minimum corner, latched from the camera (centred in XY: camera voxel
// minus 256; floored in Z under the camera's ground column, see
// DesiredOriginVoxel). Particle positions live in [0, 5120] UU relative to it.
//
// AND IT NOW FOLLOWS THE CAMERA (contract item 4, wired 2026-08-10). v0 latched
// the origin once and never moved it, so flying past the 51.2 m window left the
// water frozen behind the player and the only cure was toggling
// voxel.Fluid.Enable. The volume is a toroidal rolling window
// (FVoxelFluidOccupancyVolume::RecentreTo), so a move now preserves every bit
// still in view and invalidates only the entering slab; MaybeRecentre drives it
// with the hysteresis policy in VoxelFluidRecentre below. The two halves that
// must happen together, in the same tick:
//
//   * EVERY ORIGIN-RELATIVE QUANTITY is re-derived from the new origin --
//     FluidOriginWorld, the spill intercept box, the basin sink box, the faucet
//     gather (both forced to refresh), and the spawn positions assembled later
//     in the same Tick. The tick-args boxes (boundary centre/half extent) are
//     origin-relative CONSTANTS and therefore need no update, which is a
//     property worth keeping: see the audit table at MaybeRecentre.
//   * THE PARTICLES ARE REBASED. Positions are origin-relative, so the delta
//     handed back by TakePendingRebaseDeltaVoxels rides the render mailbox to
//     FVoxelFluidOccupancyVolume::AddRebaseParticlesPass, which runs in the
//     renderer's graph BEFORE that frame's solver passes. Skipping it slides
//     the water sideways relative to the terrain, once per recentre,
//     cumulatively.
//
// SetOriginVoxel (the hard reset) remains for the two cases where preserving
// nothing is correct: the first latch, and a teleport past the window.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelFluidSubsystem.generated.h"

// The rolling window's trigger arithmetic, free functions so the automation
// test (VoxelEarth.Fluid.RecentreTrigger) pins the SHIPPING policy rather than
// a copy of it. Values mirror voxelcore/fluidoccupancy.h and are
// static_assert'ed against it in the .cpp -- this header is UHT-parsed and so
// must not include voxel-core (the doctrine every subsystem here follows).
namespace VoxelFluidRecentre
{
	// The addressing quantum AND the fill-cell edge: vxc::kFluidRecentreStepVoxels.
	// Every move is a whole multiple of it on every axis, which is what keeps the
	// toroidal wrap seam on an output-word boundary and the entering slab
	// brick-aligned. RecentreTo REFUSES anything else rather than rounding it.
	inline constexpr int32 StepVoxels = 64; // 6.4 m

	// THE HYSTERESIS: 1.5 steps. With round-to-nearest a move leaves the drift
	// inside +/-32 voxels, so the camera must travel another 64 to trigger again
	// -- no chatter at a boundary, which matters because each recentre costs 64
	// cells of refill (1/8 of the volume). Without the 1.5 a camera loitering on
	// a threshold would pay that every few frames.
	inline constexpr int32 TriggerVoxels = 96; // 9.6 m

	// How far under the camera's ground column the window floor sits. Water
	// lives on terrain, so the window does too: ~12.8 m of rock/caves below the
	// surface, ~38.4 m of air above it. (Round-3 playtest: anchoring Z to the
	// CAMERA put every faucet 55 m below the box floor while flying.)
	inline constexpr int32 FloorBelowGroundVoxels = 128;

	// A move this big shares no terrain with the old window, so sliding it buys
	// nothing and costs a particle rebase on the way: that is a teleport and the
	// honest call for it is SetOriginVoxel (a full re-latch).
	inline constexpr int32 TeleportVoxels = 512; // == the window edge

	// Round a per-axis drift to a whole number of steps, halves AWAY FROM ZERO
	// so the policy is symmetric under a sign flip (FMath::RoundToInt is
	// half-to-positive-infinity and would make a westward camera behave
	// differently from an eastward one).
	VOXELEARTH_API int32 SnapDriftToStep(int32 DriftVoxels);

	// The whole trigger: false (and OutNewOriginVoxel == CurrentOriginVoxel)
	// when the camera has not crossed the inner boundary on any axis. True with
	// a new origin that is Current plus a whole-step move on every axis.
	VOXELEARTH_API bool ComputeRecentreOrigin(const FIntVector& WantOriginVoxel,
	                                          const FIntVector& CurrentOriginVoxel,
	                                          FIntVector& OutNewOriginVoxel);
}

// Render-thread state, defined in VoxelEarthShaders (VoxelFluidSim.h /
// VoxelFluidOccupancy.h). Held through forward declarations so this
// UHT-parsed header stays light; the .cpp includes the real things.
class FVoxelFluidSimState;
class FVoxelFluidOccupancyVolume;

// The lifecycle's voxel-core-typed state (faucet accumulators, sill entries,
// occupancy region queue). PImpl for the same doctrine every other subsystem
// here follows: a UHT-parsed header never sees a voxel-core type.
struct FVoxelFluidLifecycle;

UCLASS()
class VOXELEARTH_API UVoxelFluidSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UVoxelFluidSubsystem();
	// TUniquePtr<FVoxelFluidLifecycle> needs the full type to destroy; this
	// header must not see it -- the same declared-destructor arrangement
	// UVoxelWaterSubsystem documents.
	virtual ~UVoxelFluidSubsystem() override;
	UVoxelFluidSubsystem(FVTableHelper& Helper);

	//~ Begin USubsystem
	virtual void Deinitialize() override;
	//~ End USubsystem

	//~ Begin UWorldSubsystem
	// Game and PIE only: an editor-preview world ticking a fluid sim would
	// contend for the GPU the playtest scene is being measured on.
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem

	//~ Begin FTickableGameObject / UTickableWorldSubsystem
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject / UTickableWorldSubsystem

	// Console command bodies (the FAutoConsoleCommand statics in the .cpp
	// route here). Both are game-thread only.
	void RequestSpawnBlock(int32 Count);
	void SetEmitPerSecond(float PerSecond);

	// The terrain-dirty notification (registered with
	// UVoxelWorldSubsystem::SetFluidTerrainDirtyListener; also the funnel the
	// initial fill uses). Inclusive world-voxel box; clipped and snapped here,
	// so callers may pass boxes far outside the volume. Game thread.
	void NotifyTerrainDirty(int64 MinVx, int64 MinVy, int64 MinVz, int64 MaxVx, int64 MaxVy,
	                        int64 MaxVz);

private:
	// Tears down the render-thread state such that the last reference drops
	// on the render thread, resets every CPU-side accumulator, refunds every
	// sill-faucet unit still owed, and unregisters the terrain listener and
	// the spill intercept -- a re-enable starts a fresh, conservation-clean
	// session.
	void ReleaseSimState();

	// Latches the fluid origin (== occupancy volume min corner) centred on
	// the camera, queues the initial fill, and arms the spill intercept. HARD:
	// throws every built bit away (SetOriginVoxel). Correct for the first latch
	// and for a teleport; a camera that WALKED goes through MaybeRecentre.
	void LatchOrigin(const FVector& ViewOriginUU);

	// The origin the camera wants RIGHT NOW -- centred in XY, floored under the
	// ground column in Z. One definition, shared by the latch and the recentre
	// trigger, because a disagreement between them would make the window chase
	// an origin it never reaches. Updates LastViewOriginUU (which
	// GroundVoxelZAtCamera reads), hence non-const.
	FIntVector DesiredOriginVoxel(const FVector& ViewOriginUU);

	// The rolling-window policy (contract item 4): once per tick, after the view
	// origin is known and BEFORE ProcessOccupancyQueue. Slides the volume when
	// the camera has crossed the inner hysteresis boundary, re-derives every
	// origin-relative quantity, requeues the entering cells and leaves the
	// particle rebase owed on the volume for the tick's mailbox post.
	void MaybeRecentre(const FVector& ViewOriginUU);

	// Everything origin-relative that is NOT re-derived per tick: the spill
	// intercept box, sill faucets the window has left, and the two gathers
	// keyed to the window centre. Called by both movers (LatchOrigin and
	// MaybeRecentre) so a latch and a slide cannot disagree.
	void OnOriginMoved();

	int32 GroundVoxelZAtCamera() const;
	FVector LastViewOriginUU = FVector::ZeroVector;

	// Drains the occupancy region queue under the pack budget.
	void ProcessOccupancyQueue();

	// Lifecycle sub-steps, all game thread, all no-ops when not armed.
	void RefreshHeadwaterFaucets(double NowSeconds);
	void RefreshBasinSink(double NowSeconds);
	void DrainSillSpills();
	void ReconcileScalars();
	void RunOccupancyVerify();

	TSharedPtr<FVoxelFluidSimState, ESPMode::ThreadSafe> SimState;
	// ThreadSafe shared pointer for the same reason SimState is: render
	// commands capture their own reference, so destruction lands wherever the
	// last one drops (the volume's destructor enqueues its RHI release).
	TSharedPtr<FVoxelFluidOccupancyVolume, ESPMode::ThreadSafe> Occupancy;
	TUniquePtr<FVoxelFluidLifecycle> Lifecycle;

	// The fluid origin (contract FluidOriginUU == volume min corner * 10).
	FVector FluidOriginWorld = FVector::ZeroVector;
	FIntVector OriginVoxel = FIntVector::ZeroValue;
	bool bOriginLatched = false;
	bool bTerrainListenerRegistered = false;

	// Camera faucet (voxel.Fluid.Emit), unchanged from the spike.
	FVector FaucetCenterWorld = FVector::ZeroVector;
	bool bFaucetLatched = false;
	float EmitPerSecond = 0.0f;
	float EmitCarry = 0.0f;

	// Queued dam-break spawn (consumed by the next tick).
	int32 PendingSpawnCount = 0;

	// Upper bound on particles ever requested -- the dispatch-width bound
	// passed to the solver. Never decremented; saturates at the contract cap.
	uint64 CumulativeSpawnRequested = 0;

	// 1 Hz perf line + conservation bookkeeping.
	double NextPerfLogTime = 0.0;
	uint64 LastConservationGeneration = 0;
	uint64 ConservationViolations = 0;
	uint64 LastDebugDrawGeneration = 0;
};
