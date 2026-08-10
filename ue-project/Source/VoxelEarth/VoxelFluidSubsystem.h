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
// volume's minimum corner, latched from the camera (centred: camera voxel
// minus 256 per axis) at the first spawn/emit/faucet arm and never moved --
// v0 does not recentre (FVoxelFluidOccupancyVolume::SetOriginVoxel's cost
// note); toggle voxel.Fluid.Enable to re-anchor. Particle positions live in
// [0, 5120] UU.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelFluidSubsystem.generated.h"

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
	// the camera, queues the initial fill, and arms the spill intercept.
	void LatchOrigin(const FVector& ViewOriginUU);
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
