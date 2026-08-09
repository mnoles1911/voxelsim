// VoxelFluidSubsystem.h -- minimal game-thread host for the Phase 0 PBF
// solver spike (docs/water-rearchitecture-plan-2026-08-09.md, spike a).
//
// This subsystem exists to DRIVE AND MEASURE the GPU solver, nothing more: it
// owns the render-thread sim state, feeds it one tick per frame (view origin,
// dt, spawn/emit requests), asserts the conservation invariant on every
// readback that lands, and prints the 1 Hz perf line the integrator reads.
// No terrain coupling, no basin ledger, no rendering beyond debug points --
// those are later phases (plan Phase 2-4) and other agents' spikes.
//
// Everything is behind voxel.Fluid.Enable (default 0), so the subsystem is
// inert in every session that does not ask for it.
//
// Console surface (all defined in the .cpp):
//   voxel.Fluid.Enable 1        -- create the sim; 0 tears it down
//   voxel.Fluid.Spawn [count]   -- dam-break block above the camera (default 5000)
//   voxel.Fluid.Emit <perSec>   -- faucet stream; 0 stops it
//   voxel.Fluid.Iterations N    -- PBF constraint iterations (default 3, clamp 1..8)
//   voxel.Fluid.DebugDraw 1     -- DrawDebugPoint for <= 5k alive particles

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelFluidSubsystem.generated.h"

// Render-thread state, defined in VoxelEarthShaders (VoxelFluidSim.h). Held
// through a forward declaration so this UHT-parsed header stays light; the
// .cpp includes the real thing. Thread-safe shared pointer because render
// commands capture their own references -- see the lifetime doctrine in
// VoxelFluidSim.h.
class FVoxelFluidSimState;

UCLASS()
class VOXELEARTH_API UVoxelFluidSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
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

private:
	// Tears down the render-thread state such that the last reference drops
	// on the render thread, and resets every CPU-side accumulator so a
	// re-enable starts a fresh, conservation-clean session.
	void ReleaseSimState();

	TSharedPtr<FVoxelFluidSimState, ESPMode::ThreadSafe> SimState;

	// The fluid origin (VoxelFluidContract.ush's FluidOriginUU): latched from
	// the camera at the FIRST spawn/emit and never moved -- rebasing a live
	// sim would require rewriting every particle position, which the spike
	// does not need (its active box is 51.2 m around where the test starts).
	FVector FluidOriginWorld = FVector::ZeroVector;
	bool bOriginLatched = false;

	// Faucet emit point, latched at the first emission after each
	// voxel.Fluid.Emit command (so re-issuing the command moves the faucet
	// to the current camera).
	FVector FaucetCenterWorld = FVector::ZeroVector;
	bool bFaucetLatched = false;
	float EmitPerSecond = 0.0f;
	float EmitCarry = 0.0f;

	// Queued dam-break spawn (consumed by the next tick; one spawn dispatch
	// per frame, so a block spawn defers any faucet emission by one frame).
	int32 PendingSpawnCount = 0;

	// Upper bound on particles ever requested -- the dispatch-width bound
	// passed to the solver (see FVoxelFluidSimTickArgs::SimSlotBound for why
	// over-estimating is the safe direction). Never decremented.
	uint64 CumulativeSpawnRequested = 0;
	// Per-dispatch spawn batch id, stamped into P1.w (contract line 43).
	float SpawnBatchCounter = 0.0f;

	// 1 Hz perf line + conservation bookkeeping.
	double NextPerfLogTime = 0.0;
	uint64 LastConservationGeneration = 0;
	uint64 ConservationViolations = 0;
	uint64 LastDebugDrawGeneration = 0;
};
