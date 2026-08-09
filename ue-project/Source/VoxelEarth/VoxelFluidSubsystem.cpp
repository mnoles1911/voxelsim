// VoxelFluidSubsystem.cpp -- see the header for what this is and is not.

#include "VoxelFluidSubsystem.h"

#include "VoxelFluidSim.h" // VoxelEarthShaders -- the render-thread solver
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "RenderingThread.h"
#include "RHI.h" // GUsingNullRHI

DEFINE_LOG_CATEGORY_STATIC(LogVoxelFluid, Log, All);

namespace
{
	// ---- named constants (every magic number in the spike lives here) ------

	// Largest sim step. A background hitch must not become a 300 UU free-fall
	// step that the fallback plane then has to absorb in one projection;
	// clamping dt trades sim-time slowdown for stability, which is the right
	// trade for a measurement spike.
	constexpr float kMaxSimDtSeconds = 1.0f / 30.0f;

	// Dam-break block centre, above the CURRENT camera ("seeds a block of
	// particles above the camera" -- the spike brief). 5 m up puts the block
	// clearly in view as it falls past.
	constexpr float kSpawnHeightAboveViewUU = 500.0f;

	// Faucet emit point height above the camera at latch time.
	constexpr float kFaucetHeightAboveViewUU = 800.0f;

	// Faucet stream velocity: horizontal, so the stream arcs visibly under
	// gravity instead of just raining straight down. 3 m/s.
	const FVector3f kFaucetVelocityUU(300.0f, 0.0f, 0.0f);

	// Fallback ground plane, below the latched origin (the camera at first
	// spawn). Far enough down that the dam break falls past the camera before
	// pooling; well inside the boundary box so pooled particles do not
	// boundary-despawn off the plane.
	constexpr float kGroundBelowOriginUU = 1000.0f;

	// Boundary despawn half-extent: half the contract's 512-voxel active
	// region edge (512 * 10 cm = 51.2 m cube -> 2560 UU half extent,
	// VoxelFluidContract.ush:67-69).
	constexpr float kBoundaryHalfExtentUU = 2560.0f;

	// voxel.Fluid.Spawn with no argument.
	constexpr int32 kDefaultSpawnCount = 5000;

	// Debug-draw ceiling, from the spike brief: DrawDebugPoint is a
	// game-thread line-batcher path and 5k points is where "enough to eyeball
	// a dam break" ends and "the instrument is the bottleneck" begins.
	constexpr uint32 kDebugDrawMaxParticles = 5000;

	constexpr double kPerfLogPeriodSeconds = 1.0;

	// ---- console surface ---------------------------------------------------

	TAutoConsoleVariable<int32> CVarVoxelFluidEnable(
		TEXT("voxel.Fluid.Enable"), 0,
		TEXT("Enable the GPU PBF fluid solver spike (water re-architecture Phase 0a). ")
		TEXT("0 (default) = fully inert; 1 = sim state created and ticked. ")
		TEXT("Switching back to 0 destroys the sim and its particles."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarVoxelFluidIterations(
		TEXT("voxel.Fluid.Iterations"), 3,
		TEXT("PBF constraint iterations per frame (Macklin-Mueller solver loop). ")
		TEXT("Design range 2-4; clamped to 1..8. More = stiffer, pricier."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarVoxelFluidDebugDraw(
		TEXT("voxel.Fluid.DebugDraw"), 0,
		TEXT("Draw alive fluid particles as debug points (game-thread readback; ")
		TEXT("capped at 5000 alive -- above that it draws nothing and says so in ")
		TEXT("the perf line). The real renderer is spike (b)."),
		ECVF_Default);

	UVoxelFluidSubsystem* FindFluidSubsystem(UWorld* World)
	{
		return World ? World->GetSubsystem<UVoxelFluidSubsystem>() : nullptr;
	}

	FAutoConsoleCommandWithWorldAndArgs GVoxelFluidSpawnCmd(
		TEXT("voxel.Fluid.Spawn"),
		TEXT("voxel.Fluid.Spawn [count] -- seed a dam-break block of fluid particles ")
		TEXT("above the camera (default 5000). Requires voxel.Fluid.Enable 1."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World)
			{
				if (UVoxelFluidSubsystem* Fluid = FindFluidSubsystem(World))
				{
					const int32 Count = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : kDefaultSpawnCount;
					Fluid->RequestSpawnBlock(Count);
				}
			}));

	FAutoConsoleCommandWithWorldAndArgs GVoxelFluidEmitCmd(
		TEXT("voxel.Fluid.Emit"),
		TEXT("voxel.Fluid.Emit <perSecond> -- faucet stream at a point above the ")
		TEXT("camera (latched when emission starts; re-issue to move it). 0 stops. ")
		TEXT("Requires voxel.Fluid.Enable 1."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World)
			{
				if (UVoxelFluidSubsystem* Fluid = FindFluidSubsystem(World))
				{
					const float PerSecond = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 0.0f;
					Fluid->SetEmitPerSecond(PerSecond);
				}
			}));
}

bool UVoxelFluidSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UVoxelFluidSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelFluidSubsystem, STATGROUP_Tickables);
}

void UVoxelFluidSubsystem::Deinitialize()
{
	ReleaseSimState();
	Super::Deinitialize();
}

void UVoxelFluidSubsystem::RequestSpawnBlock(int32 Count)
{
	if (CVarVoxelFluidEnable.GetValueOnGameThread() == 0)
	{
		// Refuse loudly rather than queue silently: a spawn that sits in a
		// queue until someone flips the cvar an hour later is a mystery
		// dam break, not a test.
		UE_LOG(LogVoxelFluid, Warning,
		       TEXT("voxel.Fluid.Spawn ignored: voxel.Fluid.Enable is 0"));
		return;
	}
	const int32 Clamped = FMath::Clamp(Count, 1, int32(VoxelFluidSim::kMaxParticles));
	if (Clamped != Count)
	{
		UE_LOG(LogVoxelFluid, Warning, TEXT("voxel.Fluid.Spawn %d clamped to %d"), Count, Clamped);
	}
	PendingSpawnCount = Clamped;
	UE_LOG(LogVoxelFluid, Display, TEXT("Fluid spawn queued: %d particles (dam-break block)"), Clamped);
}

void UVoxelFluidSubsystem::SetEmitPerSecond(float PerSecond)
{
	if (PerSecond > 0.0f && CVarVoxelFluidEnable.GetValueOnGameThread() == 0)
	{
		UE_LOG(LogVoxelFluid, Warning,
		       TEXT("voxel.Fluid.Emit ignored: voxel.Fluid.Enable is 0"));
		return;
	}
	EmitPerSecond = FMath::Max(PerSecond, 0.0f);
	EmitCarry = 0.0f;
	// Re-latch so the stream appears where the camera is NOW, not where the
	// previous stream was.
	bFaucetLatched = false;
	UE_LOG(LogVoxelFluid, Display, TEXT("Fluid emit rate set: %.1f particles/s"), EmitPerSecond);
}

void UVoxelFluidSubsystem::ReleaseSimState()
{
	if (SimState.IsValid())
	{
		// The render command's captured copy becomes the last reference once
		// ours is reset below, so the RHI resources AND the state object die
		// on the render thread -- the same rule UVoxelGpuPoolComponent::
		// BeginDestroy follows for the pool buffers.
		TSharedPtr<FVoxelFluidSimState, ESPMode::ThreadSafe> StatePtr = SimState;
		SimState.Reset();
		ENQUEUE_RENDER_COMMAND(VoxelFluidRelease)(
			[StatePtr](FRHICommandListImmediate&)
			{
				VoxelFluidSim::ReleaseRenderThread(*StatePtr);
			});
		UE_LOG(LogVoxelFluid, Display,
		       TEXT("Fluid sim released (conservation violations this session: %llu)"),
		       ConservationViolations);
	}
	// Fresh-session accounting: a re-enable must start conservation-clean.
	bOriginLatched = false;
	bFaucetLatched = false;
	EmitPerSecond = 0.0f;
	EmitCarry = 0.0f;
	PendingSpawnCount = 0;
	CumulativeSpawnRequested = 0;
	SpawnBatchCounter = 0.0f;
	LastConservationGeneration = 0;
	ConservationViolations = 0;
	LastDebugDrawGeneration = 0;
}

void UVoxelFluidSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	if (CVarVoxelFluidEnable.GetValueOnGameThread() == 0)
	{
		if (SimState.IsValid())
		{
			ReleaseSimState();
		}
		return;
	}

	if (GUsingNullRHI)
	{
		// The headless automation run (-nullrhi) can reach here with the cvar
		// set; there is no GPU to sim on, and saying so once beats a silent
		// no-op (the "did not run must be distinguishable" rule).
		static bool bLoggedNullRhi = false;
		if (!bLoggedNullRhi)
		{
			bLoggedNullRhi = true;
			UE_LOG(LogVoxelFluid, Warning, TEXT("Fluid perf: [idle] null RHI, sim not running"));
		}
		return;
	}

	if (!SimState.IsValid())
	{
		SimState = MakeShared<FVoxelFluidSimState, ESPMode::ThreadSafe>();
		NextPerfLogTime = 0.0; // log immediately on enable
	}

	// ---- view origin -------------------------------------------------------
	FVector ViewOrigin = FVector::ZeroVector;
	if (const APlayerController* PC = World->GetFirstPlayerController())
	{
		if (PC->PlayerCameraManager != nullptr)
		{
			ViewOrigin = PC->PlayerCameraManager->GetCameraLocation();
		}
	}

	// ---- assemble this frame's spawn (one spawn dispatch per frame) --------
	FVoxelFluidSpawnRequest Spawn;
	if (PendingSpawnCount > 0)
	{
		if (!bOriginLatched)
		{
			FluidOriginWorld = ViewOrigin;
			bOriginLatched = true;
			UE_LOG(LogVoxelFluid, Display, TEXT("Fluid origin latched at (%.0f, %.0f, %.0f) UU"),
			       FluidOriginWorld.X, FluidOriginWorld.Y, FluidOriginWorld.Z);
		}
		Spawn.Count = uint32(PendingSpawnCount);
		Spawn.Mode = 0; // block lattice
		const FVector CenterWorld = ViewOrigin + FVector(0, 0, kSpawnHeightAboveViewUU);
		Spawn.CenterLocalUU = FVector3f(CenterWorld - FluidOriginWorld);
		Spawn.VelocityUU = FVector3f::ZeroVector;
		PendingSpawnCount = 0;
	}
	else if (EmitPerSecond > 0.0f)
	{
		// Real DeltaTime (not the sim-clamped dt): the faucet owes particles
		// by wall-clock rate.
		EmitCarry += EmitPerSecond * DeltaTime;
		const int32 EmitNow = FMath::FloorToInt(EmitCarry);
		if (EmitNow > 0)
		{
			EmitCarry -= float(EmitNow);
			if (!bOriginLatched)
			{
				FluidOriginWorld = ViewOrigin;
				bOriginLatched = true;
				UE_LOG(LogVoxelFluid, Display, TEXT("Fluid origin latched at (%.0f, %.0f, %.0f) UU"),
				       FluidOriginWorld.X, FluidOriginWorld.Y, FluidOriginWorld.Z);
			}
			if (!bFaucetLatched)
			{
				FaucetCenterWorld = ViewOrigin + FVector(0, 0, kFaucetHeightAboveViewUU);
				bFaucetLatched = true;
			}
			Spawn.Count = uint32(FMath::Min(EmitNow, int32(VoxelFluidSim::kMaxParticles)));
			Spawn.Mode = 1; // faucet
			Spawn.CenterLocalUU = FVector3f(FaucetCenterWorld - FluidOriginWorld);
			Spawn.VelocityUU = kFaucetVelocityUU;
		}
	}
	if (Spawn.Count > 0)
	{
		Spawn.Seed = uint32(GFrameCounter);
		Spawn.BatchId = SpawnBatchCounter;
		SpawnBatchCounter += 1.0f;
		CumulativeSpawnRequested += Spawn.Count;
	}

	const FVoxelFluidCountsSnapshot Snapshot = SimState->GetLatestCounts();

	// ---- drive the GPU (only once something has ever spawned) --------------
	const float SimDt = FMath::Min(DeltaTime, kMaxSimDtSeconds);
	if (CumulativeSpawnRequested > 0 && SimDt > 0.0f)
	{
		const bool bDebugDraw = CVarVoxelFluidDebugDraw.GetValueOnGameThread() != 0;

		FVoxelFluidSimTickArgs Args;
		Args.Dt = SimDt;
		Args.Iterations = FMath::Clamp(CVarVoxelFluidIterations.GetValueOnGameThread(), 1, 8);
		Args.SimSlotBound =
			uint32(FMath::Min<uint64>(CumulativeSpawnRequested, VoxelFluidSim::kMaxParticles));
		Args.GroundZLocalUU = -kGroundBelowOriginUU;
		Args.BoundaryHalfExtentUU = kBoundaryHalfExtentUU;
		Args.Spawn = Spawn;
		// Only pay the (256 KB, debug-only) particle readback while the draw
		// can actually happen -- unknown alive counts as drawable so the first
		// frames are not a chicken-and-egg stall.
		Args.bReadbackDebugSlots =
			bDebugDraw && (!Snapshot.bValid || Snapshot.Alive <= kDebugDrawMaxParticles);
		Args.DebugSlotCount = Args.SimSlotBound;

		TSharedPtr<FVoxelFluidSimState, ESPMode::ThreadSafe> StatePtr = SimState;
		ENQUEUE_RENDER_COMMAND(VoxelFluidSimTick)(
			[StatePtr, Args](FRHICommandListImmediate& RHICmdList)
			{
				VoxelFluidSim::TickRenderThread(RHICmdList, *StatePtr, Args);
			});
	}

	// ---- conservation: asserted on every readback generation, not sampled --
	if (Snapshot.bValid && Snapshot.Generation != LastConservationGeneration)
	{
		LastConservationGeneration = Snapshot.Generation;
		if (!VoxelFluidSim::CheckConservation(Snapshot.Alive, Snapshot.DespawnedBasin,
		                                      Snapshot.DespawnedBoundary, Snapshot.SpawnedTotal))
		{
			ConservationViolations++;
			UE_LOG(LogVoxelFluid, Error,
			       TEXT("Fluid CONSERVATION VIOLATION (gen %llu): spawned %u - basin %u - boundary %u ")
			       TEXT("= %d but alive = %u"),
			       Snapshot.Generation, Snapshot.SpawnedTotal, Snapshot.DespawnedBasin,
			       Snapshot.DespawnedBoundary,
			       int64(Snapshot.SpawnedTotal) - Snapshot.DespawnedBasin - Snapshot.DespawnedBoundary,
			       Snapshot.Alive);
		}
	}

	// ---- debug draw --------------------------------------------------------
	bool bDebugDrawSkippedTooMany = false;
	if (CVarVoxelFluidDebugDraw.GetValueOnGameThread() != 0 && Snapshot.bValid && bOriginLatched)
	{
		if (Snapshot.Alive <= kDebugDrawMaxParticles)
		{
			TArray<FVector3f> Positions;
			uint64 Generation = 0;
			SimState->GetDebugPositions(Positions, Generation);
			LastDebugDrawGeneration = Generation;
			for (const FVector3f& P : Positions)
			{
				DrawDebugPoint(World, FluidOriginWorld + FVector(P), 5.0f, FColor::Cyan,
				               /*bPersistent*/ false, /*LifeTime*/ -1.0f);
			}
		}
		else
		{
			bDebugDrawSkippedTooMany = true; // reported in the perf line below
		}
	}

	// ---- the 1 Hz perf line ------------------------------------------------
	// Printed whenever the system is enabled, INCLUDING at zero alive, with an
	// explicit state marker -- a missing line means "not running", a [idle]
	// line means "running and empty", and those two must never be confusable
	// (the plan's ran-flag rule).
	const double Now = FPlatformTime::Seconds();
	if (Now >= NextPerfLogTime)
	{
		NextPerfLogTime = Now + kPerfLogPeriodSeconds;
		const TCHAR* StateMarker =
			!Snapshot.bValid ? TEXT("[idle: no readback yet]")
			: (Snapshot.SpawnedTotal == 0 ? TEXT("[idle]") : TEXT("[run]"));
		UE_LOG(LogVoxelFluid, Display,
		       TEXT("Fluid perf %s alive=%u spawned=%u requested=%llu despawnBasin=%u ")
		       TEXT("despawnBoundary=%u simGpuMs=%.2f iters=%d slots=%llu violations=%llu%s"),
		       StateMarker,
		       Snapshot.Alive, Snapshot.SpawnedTotal, CumulativeSpawnRequested,
		       Snapshot.DespawnedBasin, Snapshot.DespawnedBoundary,
		       Snapshot.SimGpuMs,
		       FMath::Clamp(CVarVoxelFluidIterations.GetValueOnGameThread(), 1, 8),
		       FMath::Min<uint64>(CumulativeSpawnRequested, VoxelFluidSim::kMaxParticles),
		       ConservationViolations,
		       bDebugDrawSkippedTooMany ? TEXT(" debugDraw=skipped(alive>5000)") : TEXT(""));
	}
}
