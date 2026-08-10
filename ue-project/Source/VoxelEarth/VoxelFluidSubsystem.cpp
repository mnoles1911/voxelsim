// VoxelFluidSubsystem.cpp -- see the header for what this is. Phase 3
// integration pass: collision volume ownership + feed, the faucet/sink
// lifecycle v1, the particle<->scalar conservation seam, and the occupancy
// verify gate.

#include "VoxelFluidSubsystem.h"

#include "VoxelFluidSim.h"       // VoxelEarthShaders -- the render-thread solver
#include "VoxelFluidOccupancy.h" // VoxelEarthShaders -- the collision volume
#include "VoxelFluidRender.h"    // VoxelEarthShaders -- the screen-space fluid renderer (Phase 4)
#include "VoxelWorldSubsystem.h"
#include "VoxelWaterSubsystem.h"
#include "VoxelSkySubsystem.h"   // sun direction for the renderer's constant-sky Fresnel
#include "SceneViewExtension.h"  // FSceneViewExtensions::NewExtension
#include "VoxelCoords.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "RenderingThread.h"
#include "RHI.h" // GUsingNullRHI

#include "voxelcore/fluidlifecycle.h"
#include "voxelcore/fluidoccupancy.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelFluid, Log, All);

namespace
{
	// ---- named constants (every magic number lives here) -------------------

	// Largest sim step. A background hitch must not become a 300 UU free-fall
	// step; clamping dt trades sim-time slowdown for stability.
	constexpr float kMaxSimDtSeconds = 1.0f / 30.0f;

	// Dam-break block centre, above the CURRENT camera.
	constexpr float kSpawnHeightAboveViewUU = 500.0f;

	// Camera-faucet emit point height above the camera at latch time.
	constexpr float kFaucetHeightAboveViewUU = 800.0f;

	// Camera-faucet stream velocity: horizontal, so the stream arcs visibly.
	const FVector3f kFaucetVelocityUU(300.0f, 0.0f, 0.0f);

	// Fallback ground plane -- DEAD in shipping compiles (collision is live,
	// contract item 5); still passed so an offline no-collision shader compile
	// keeps its plane.
	constexpr float kGroundBelowOriginUU = 1000.0f;

	// The active region: the occupancy volume's own cube (contract items 1/5).
	// Origin = volume min corner, so the box is [0, kActiveEdgeUU]^3 and its
	// centre is at half the edge on each axis.
	constexpr float kActiveEdgeUU = float(vxc::kFluidVolumeDimVoxels) * vxc::kFluidVoxelUU; // 5120
	constexpr float kBoundaryHalfExtentUU = kActiveEdgeUU * 0.5f;                           // 2560
	// Spawn positions are clamped this far inside the box, so a request from a
	// camera that wandered out of the latched region does not spawn particles
	// that boundary-despawn on their first frame and silently become graph
	// inflow.
	constexpr float kSpawnClampMarginUU = 200.0f;

	// Headwater/sill faucets emit this far above the local ground/sill, inside
	// the faucet jitter disc's reach of the channel. An EDGE INFLOW uses the
	// same clearance but above the DRAWN WATER SURFACE, not the ground: the bed
	// under a big reach is up to 25 m down, and a river entering the box has to
	// enter at the height the water is already drawn at or it falls in.
	constexpr float kLifecycleFaucetHeightUU = 30.0f;

	// Edge-inflow stream speed, UU/s, horizontal along the channel. Small on
	// purpose: it exists so the water ENTERS moving in the direction the river
	// runs rather than dropping in as a column, and the slope takes over from
	// there. 150 UU/s = 1.5 m/s, a brisk river.
	constexpr float kEdgeInflowSpeedUU = 150.0f;

	// How far from the volume centre the boundary sink searches for a river
	// segment to inject into (world mm). The region's own half-diagonal is
	// ~36 m; 64 m adds margin for a channel just outside the cube. Beyond it,
	// units stay PENDING rather than teleporting into an unrelated valley.
	constexpr int64 kBoundaryInjectReachMm = 64'000;

	// Initial-fill region granularity: 64^3 voxels = 8x8x8 bricks = 32 KB of
	// packed bits per region; the volume is 8x8x8 of them.
	constexpr int32 kFillCellVoxels = 64;
	constexpr int32 kFillCellsPerAxis = vxc::kFluidVolumeDimVoxels / kFillCellVoxels; // 8
	constexpr int32 kFillCellCount = kFillCellsPerAxis * kFillCellsPerAxis * kFillCellsPerAxis;

	// Occupancy queue hard cap. Hitting it drops the OLDEST entries in favour
	// of a full-volume dirty (counted); in practice the queue peaks at the 512
	// initial cells plus a handful of edits.
	constexpr int32 kMaxPendingRegions = 4096;

	// Faucet list cap and refresh cadence. 32 faucets x ~253/s = ~8k/s, well
	// past the default emit budget -- the cap bounds the gather, the budget
	// bounds the emission.
	constexpr int32 kMaxHeadFaucets = 32;
	constexpr double kFaucetRefreshSeconds = 10.0;
	constexpr double kSinkRefreshSeconds = 1.0;

	// voxel.Fluid.Spawn with no argument.
	constexpr int32 kDefaultSpawnCount = 5000;

	// Debug-draw ceiling (see the spike brief).
	constexpr uint32 kDebugDrawMaxParticles = 5000;

	constexpr double kPerfLogPeriodSeconds = 1.0;

	// ---- console surface ---------------------------------------------------

	TAutoConsoleVariable<int32> CVarVoxelFluidEnable(
		TEXT("voxel.Fluid.Enable"), 0,
		TEXT("Enable the GPU PBF fluid solver (water re-architecture Phase 3). ")
		TEXT("0 (default) = fully inert; 1 = sim + collision volume created and ticked. ")
		TEXT("Switching back to 0 destroys the sim, refunds owed sill units, and re-anchors on re-enable."),
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
		TEXT("the perf line)."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarVoxelFluidFaucets(
		TEXT("voxel.Fluid.Faucets"), 0,
		TEXT("Arm the faucet/sink lifecycle (plan Phase 3): headwater faucets from baked ")
		TEXT("heads (bv24) or the bv23 fallback graph, sill faucets from basin spill ")
		TEXT("events, the basin despawn sink (credits the ledger) and the boundary ")
		TEXT("despawn sink (injects the routing graph). Requires voxel.Fluid.Enable 1."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarVoxelFluidFaucetDefaultQ(
		TEXT("voxel.Fluid.Faucets.DefaultQ"), 8.0e6f,
		TEXT("Discharge, m^3/yr, assumed for a headwater the source could not rate ")
		TEXT("(the bv23 fallback carries no Q). Default 8e6 ~= 253 particles/s."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarVoxelFluidGpuTiming(
		TEXT("voxel.Fluid.GpuTiming"), 0,
		TEXT("EXPERIMENTAL: raw render-query GPU timing bracket around the fluid "
		     "graph. Crashed RDG's breadcrumb assert on first Execute in UE 5.8; "
		     "off until rebuilt on a sanctioned path. The gate metric is the A/B "
		     "frame-time delta while this is off."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarVoxelFluidMaxSpawnPerTick(
		TEXT("voxel.Fluid.MaxSpawnPerTick"), 4096,
		TEXT("Shared per-tick particle budget for ALL lifecycle emission (camera ")
		TEXT("faucet, headwater faucets, sill faucets). Owed particles above the ")
		TEXT("budget stay owed (carried), never dropped. Dam-break blocks are an ")
		TEXT("explicit user request and bypass it."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarVoxelFluidOccRegionsPerTick(
		TEXT("voxel.Fluid.Occupancy.RegionsPerTick"), 8,
		TEXT("Occupancy fill budget: max regions packed+queued to the GPU per game ")
		TEXT("tick. The deferred remainder is visible in the perf line as ")
		TEXT("occupancy=<built>/<deferred>."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarVoxelFluidOccPackMsPerTick(
		TEXT("voxel.Fluid.Occupancy.PackMsPerTick"), 4.0f,
		TEXT("Occupancy fill budget: max milliseconds per game tick spent packing ")
		TEXT("brick solidity (the game-thread half of a region fill). The initial ")
		TEXT("512-region fill spreads across ticks under this; raise it to fill ")
		TEXT("faster at the cost of frame time."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarVoxelFluidOccVerify(
		TEXT("voxel.Fluid.Occupancy.Verify"), 0,
		TEXT("THE GPU/CPU OCCUPANCY GATE: while 1, reads the 16 MiB volume back and ")
		TEXT("byte-compares one 64^3 region per landed snapshot (rotating cursor) ")
		TEXT("against vxc::fluidFillRegion, the unit-tested CPU reference. Result in ")
		TEXT("the perf line as verify=pass|FAIL|stale; every mismatch logs its first ")
		TEXT("differing word. Debug only -- it costs a 16 MiB readback per cycle."),
		ECVF_Default);

	// ---- the screen-space fluid renderer (Phase 4 / spike b) ---------------
	// OFF by default -- the perf plan's rule: the pass must be measurable and
	// budgetable before it is on anywhere, and DrawDebugPoint remains the
	// fallback view. Enabling costs 4 GPU passes at half resolution
	// (splat MRT, smooth x2, shade) whose cost lands in the perf line as
	// renderMs and in ProfileGPU under "VoxelFluidRender".
	TAutoConsoleVariable<int32> CVarVoxelFluidRender(
		TEXT("voxel.Fluid.Render"), 0,
		TEXT("Screen-space fluid rendering for the PBF particles (splat depth -> ")
		TEXT("bilateral smooth -> normals -> Beer-Lambert shade -> composite vs opaque ")
		TEXT("depth). 0 (default) = off; DrawDebugPoint stays the debug view. ")
		TEXT("Requires voxel.Fluid.Enable 1. GPU cost reported as renderMs in the ")
		TEXT("1 Hz Fluid perf line and attributed per-pass in ProfileGPU."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarVoxelFluidRenderRadius(
		TEXT("voxel.Fluid.Render.RadiusUU"), 15.0f,
		TEXT("Sprite radius of a splatted particle, UU. Default 1.5x the 10 UU rest ")
		TEXT("spacing so neighbours' impostors overlap into a closed surface. ")
		TEXT("Thickness stays volume-correct at any radius (normalised in-shader)."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarVoxelFluidRenderSmoothRadius(
		TEXT("voxel.Fluid.Render.SmoothRadiusPx"), 6,
		TEXT("Bilateral depth-smooth radius in half-res pixels (two separable ")
		TEXT("passes). Bigger = sheetier fluid, more GPU. Clamped 1..32."),
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

// ---------------------------------------------------------------------------
// The lifecycle state (voxel-core types live here, never in the header)
// ---------------------------------------------------------------------------

struct FVoxelFluidLifecycle
{
	// --- occupancy region queue (volume-local, snapped) ---------------------
	struct FRegion
	{
		FIntVector Min = FIntVector::ZeroValue;
		FIntVector Size = FIntVector::ZeroValue;
		bool bInitialFill = false;
	};
	TArray<FRegion> PendingRegions;
	uint64 EditEpoch = 0; // bumped by every NotifyTerrainDirty that queued
	int32 InitialCellsRemaining = 0;
	uint64 RegionsBuiltTotal = 0;
	uint64 RegionsQueuedTotal = 0;
	uint64 RegionsDropped = 0;   // queue overflow / refused by the volume
	double PackMsTotal = 0.0;

	// --- headwater faucets --------------------------------------------------
	struct FHeadFaucet
	{
		FVector WorldPos = FVector::ZeroVector; // z resolved at refresh (surface + offset)
		double QM3PerYear = 0.0;                // 0 == source had no rate; DefaultQ applies
		FVector3f Velocity = FVector3f::ZeroVector; // edge inflow only; a spring seeps
		bool bEdgeInflow = false;
		vxc::FluidFaucetAccumulator Acc;
	};
	TArray<FHeadFaucet> HeadFaucets;
	bool bHeadsFromBakedTable = false;
	bool bFaucetGatherEverRan = false;
	// The last gather's selection, for the perf line: how many of the box's
	// baked heads survived the spring rule, and how many faucets came from the
	// river crossing the boundary instead. (The full breakdown -- heads in box,
	// Q band, the fitted runoff -- goes to the Verbose log at gather time; only
	// the two numbers that answer "is the selection working" are kept here.)
	int32 FaucetSprings = 0;
	int32 FaucetEdgeInflows = 0;
	bool bFaucetCapTruncated = false;
	double NextFaucetRefreshSeconds = 0.0;
	uint64 HeadFaucetParticlesEmitted = 0;
	// Faucet-ticks skipped because the emit point sat in occupancy the initial
	// fill has not reached yet (or outside the volume entirely). Not a drop and
	// not a debt: the accumulator is left untouched, so the faucet resumes at
	// its exact carry the moment its cell lands. Printed as deferredNoOcc.
	uint64 FaucetTicksDeferredNoOccupancy = 0;
	int32 FaucetsDeferredNoOccupancyNow = 0; // faucets deferred on the last tick
	// ...and how many of those were deferred because the emit point is not in
	// the box AT ALL, which is a different bug report: an unbuilt cell clears
	// in seconds, an emit point outside the window never does (the window is
	// centred on the CAMERA, so flying puts the terrain below its floor).
	// Split out because the merged number cost a playtest to interpret.
	uint64 FaucetTicksDeferredOutsideVolume = 0;
	int32 FaucetsDeferredOutsideVolumeNow = 0;
	// Discharge the window could not afford as particles, routed through the
	// scalar graph instead (backpressure). Units are ledger units (1/255 vox).
	int64 FaucetVirtualUnitsRouted = 0;
	int64 FaucetVirtualUnitsRefused = 0;

	// --- sill faucets (spill events owed to the fluid) ----------------------
	struct FSillFaucet
	{
		uint64 BasinKey = 0;
		int64 UnitsRemaining = 0;
		FVector WorldPos = FVector::ZeroVector;
	};
	TArray<FSillFaucet> SillFaucets;
	int64 SpillUnitsClaimed = 0;
	uint64 SpillParticlesEmitted = 0;
	int64 SpillUnitsRefunded = 0;

	// --- basin sink v1 (one basin; the documented limit) --------------------
	bool bSinkValid = false;
	int32 SinkTileX = 0, SinkTileY = 0, SinkLocalBasinId = 0;
	// Clipped to the active region, world UU. Z: the datum is the live
	// (ledger-adjusted) lake surface; the box's own z-span is the whole cube.
	double SinkMinXUU = 0.0, SinkMinYUU = 0.0, SinkMaxXUU = 0.0, SinkMaxYUU = 0.0;
	double SinkDatumZUU = 0.0;
	double NextSinkRefreshSeconds = 0.0;

	// --- particle<->scalar reconciliation (the extended conservation line) --
	uint64 BasinDespawnsSeen = 0;   // cumulative counter value already converted to units
	int64 CreditedBasinUnits = 0;
	int64 PendingBasinCreditUnits = 0;
	int64 LostBasinUnits = 0;       // teardown-only: pending units nobody could take
	uint64 BoundaryDespawnsSeen = 0;
	int64 InjectedBoundaryUnits = 0;
	int64 PendingBoundaryUnits = 0;
	int64 LostBoundaryUnits = 0;
	uint64 ScalarLedgerViolations = 0;

	// --- per-perf-line rates ------------------------------------------------
	uint64 PerfFaucetEmitted = 0;
	uint64 PerfSpillEmitted = 0;
	uint64 LastPerfBasinDespawns = 0;
	uint64 LastPerfBoundaryDespawns = 0;

	// --- occupancy verify ---------------------------------------------------
	int32 VerifyCursor = 0;
	uint64 VerifyArmedEpoch = 0;
	uint64 LastVerifyTakenGeneration = 0;
	// -1 never ran, 1 pass, 0 FAIL, 2 stale-skip -- distinct states, because
	// "verify said nothing" and "verify said pass" must never be confusable.
	int32 LastVerifyResult = -1;
	uint64 VerifyPasses = 0, VerifyFails = 0, VerifySkips = 0;
	TArray<uint32> VerifyScratch;   // full-volume CPU shadow, allocated while verifying
	TArray<uint32> VerifyBrickBits; // pack scratch

	// Cells of the initial fill, centre-out, precomputed at latch.
	TArray<FIntVector> InitialCellOrder;

	// --- the screen-space renderer (voxel.Fluid.Render) ---------------------
	// Both live in the lifecycle (not the header) so the file-ownership of
	// this feature stays in the .cpp, and so ReleaseSimState's fresh-lifecycle
	// reset tears the renderer down with everything else. The extension is
	// created lazily on the first tick that sees the cvar at 1 and stays
	// registered (inert -- IsActiveThisFrame declines) when it drops back to
	// 0; the engine registry holds only a weak reference, so dropping these
	// shared pointers at teardown destroys it, and any in-flight frame's
	// gathered snapshot keeps it alive exactly long enough.
	TSharedPtr<FVoxelFluidRenderState, ESPMode::ThreadSafe> RenderState;
	TSharedPtr<FVoxelFluidRenderExtension, ESPMode::ThreadSafe> RenderExtension;
};

// ---------------------------------------------------------------------------

UVoxelFluidSubsystem::UVoxelFluidSubsystem()
	: Lifecycle(MakeUnique<FVoxelFluidLifecycle>())
{
}
UVoxelFluidSubsystem::UVoxelFluidSubsystem(FVTableHelper& Helper)
	: Super(Helper)
{
}
UVoxelFluidSubsystem::~UVoxelFluidSubsystem() = default;

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
	bFaucetLatched = false;
	UE_LOG(LogVoxelFluid, Display, TEXT("Fluid emit rate set: %.1f particles/s"), EmitPerSecond);
}

void UVoxelFluidSubsystem::NotifyTerrainDirty(int64 MinVx, int64 MinVy, int64 MinVz, int64 MaxVx,
                                              int64 MaxVy, int64 MaxVz)
{
	// Before the origin is latched there is no volume to keep in step; the
	// initial fill after latching covers everything that happened before it.
	if (!bOriginLatched || !Occupancy.IsValid() || !Lifecycle.IsValid())
	{
		return;
	}
	// int64 pre-reject BEFORE any int32 narrowing: a box that does not touch
	// the volume must be discarded on full-width arithmetic, or a (purely
	// theoretical) coordinate past 2^31 could alias INTO the volume through
	// the cast -- the failure that looks like terrain.
	{
		const int64 Dim = vxc::kFluidVolumeDimVoxels;
		if (MaxVx < int64(OriginVoxel.X) || MinVx >= int64(OriginVoxel.X) + Dim ||
		    MaxVy < int64(OriginVoxel.Y) || MinVy >= int64(OriginVoxel.Y) + Dim ||
		    MaxVz < int64(OriginVoxel.Z) || MinVz >= int64(OriginVoxel.Z) + Dim)
		{
			return; // outside the volume -- the overwhelmingly common case
		}
	}
	FIntVector MinLocal, Size;
	if (!FVoxelFluidOccupancyVolume::SnapRegion(OriginVoxel,
	                                            FIntVector(int32(MinVx), int32(MinVy), int32(MinVz)),
	                                            FIntVector(int32(MaxVx), int32(MaxVy), int32(MaxVz)),
	                                            MinLocal, Size))
	{
		return;
	}

	FVoxelFluidLifecycle& L = *Lifecycle;
	L.EditEpoch++;
	// Exact-duplicate suppression only (an edited chunk re-dirtied within one
	// budget window). Overlapping-but-different regions both run; the last
	// pack wins on the GPU, which is correct.
	for (const FVoxelFluidLifecycle::FRegion& R : L.PendingRegions)
	{
		if (R.Min == MinLocal && R.Size == Size)
		{
			return;
		}
	}
	if (L.PendingRegions.Num() >= kMaxPendingRegions)
	{
		L.RegionsDropped++;
		return;
	}
	L.PendingRegions.Add({MinLocal, Size, /*bInitialFill*/ false});
	L.RegionsQueuedTotal++;
}

int32 UVoxelFluidSubsystem::GroundVoxelZAtCamera() const
{
	// The analytic surface is fine for a WINDOW ANCHOR (metres of slack both
	// ways); it is NOT fine for faucet Z, which is documented separately.
	UWorld* W = GetWorld();
	UVoxelWorldSubsystem* WS = W ? W->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	const FVector Cam = LastViewOriginUU;
	const double GroundUU = WS ? WS->GetSurfaceHeightUU(Cam.X, Cam.Y) : Cam.Z;
	return int32(FMath::FloorToDouble(GroundUU / 10.0));
}

void UVoxelFluidSubsystem::LatchOrigin(const FVector& ViewOriginUU)
{
	LastViewOriginUU = ViewOriginUU;
	const VoxelCoords::FVoxelCoord CamVoxel = VoxelCoords::WorldToVoxel(ViewOriginUU);
	const int32 Half = vxc::kFluidVolumeDimVoxels / 2;
	OriginVoxel = FIntVector(int32(CamVoxel.X) - Half, int32(CamVoxel.Y) - Half,
	                         // Z ANCHORS TO THE GROUND, NOT THE CAMERA. The
	                         // round-3 playtest flew at 80 m and every faucet
	                         // landed 55 m BELOW the box floor -- water lives
	                         // on terrain, so the window does too: floor sits
	                         // ~13 m under the surface at the camera's column
	                         // (caves/pools), leaving ~38 m of air above it.
	                         GroundVoxelZAtCamera() - 128);
	// THE one-origin rule (contract item 1): FluidOriginUU == volume min
	// corner * 10, computed from the same voxel triple SetOriginVoxel gets.
	FluidOriginWorld = FVector(double(OriginVoxel.X), double(OriginVoxel.Y), double(OriginVoxel.Z)) *
	                   double(vxc::kFluidVoxelUU);
	Occupancy->SetOriginVoxel(OriginVoxel);
	bOriginLatched = true;

	// Initial fill: every 64^3 cell, centre-out, so the terrain around the
	// particles lands first and the corners last. The whole volume is packed
	// from vxc::World (via IsSolidAtVoxel) regardless of mesh residency --
	// materialAt generates on demand -- which is why this is budgeted across
	// ticks rather than done here.
	FVoxelFluidLifecycle& L = *Lifecycle;
	L.InitialCellOrder.Reset(kFillCellCount);
	for (int32 Z = 0; Z < kFillCellsPerAxis; ++Z)
		for (int32 Y = 0; Y < kFillCellsPerAxis; ++Y)
			for (int32 X = 0; X < kFillCellsPerAxis; ++X)
			{
				L.InitialCellOrder.Add(FIntVector(X, Y, Z));
			}
	const double Mid = (kFillCellsPerAxis - 1) * 0.5;
	L.InitialCellOrder.Sort([Mid](const FIntVector& A, const FIntVector& B)
	{
		const double Da = FMath::Square(A.X - Mid) + FMath::Square(A.Y - Mid) + FMath::Square(A.Z - Mid);
		const double Db = FMath::Square(B.X - Mid) + FMath::Square(B.Y - Mid) + FMath::Square(B.Z - Mid);
		return Da < Db;
	});
	for (const FIntVector& Cell : L.InitialCellOrder)
	{
		L.PendingRegions.Add({Cell * kFillCellVoxels,
		                      FIntVector(kFillCellVoxels, kFillCellVoxels, kFillCellVoxels),
		                      /*bInitialFill*/ true});
		L.RegionsQueuedTotal++;
	}
	L.InitialCellsRemaining = kFillCellCount;

	// Arm the sill-faucet intercept over the active region's XY footprint.
	if (UVoxelWaterSubsystem* Water = GetWorld() ? GetWorld()->GetSubsystem<UVoxelWaterSubsystem>() : nullptr)
	{
		Water->SetFluidSpillIntercept(true, FluidOriginWorld.X, FluidOriginWorld.Y,
		                              FluidOriginWorld.X + kActiveEdgeUU,
		                              FluidOriginWorld.Y + kActiveEdgeUU);
	}

	UE_LOG(LogVoxelFluid, Display,
	       TEXT("Fluid origin latched: volume min corner voxel (%d, %d, %d), world UU (%.0f, %.0f, %.0f), ")
	       TEXT("active box %.0fx%.0fx%.0f UU. Initial occupancy fill queued: %d regions (budgeted; ")
	       TEXT("unfilled space is SOLID until built -- water freezes rather than leaks)."),
	       OriginVoxel.X, OriginVoxel.Y, OriginVoxel.Z, FluidOriginWorld.X, FluidOriginWorld.Y,
	       FluidOriginWorld.Z, kActiveEdgeUU, kActiveEdgeUU, kActiveEdgeUU, kFillCellCount);
}

void UVoxelFluidSubsystem::ProcessOccupancyQueue()
{
	FVoxelFluidLifecycle& L = *Lifecycle;
	if (L.PendingRegions.Num() == 0 || !Occupancy.IsValid())
	{
		return;
	}
	UVoxelWorldSubsystem* WorldSub = GetWorld() ? GetWorld()->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (WorldSub == nullptr)
	{
		return;
	}

	// SOLIDITY SOURCE: the overlay-aware IsSolidAtVoxel -- the same
	// World::materialAt the mesher reads, which is the property that matters
	// (VoxelFluidOccupancy.h "WHERE SOLIDITY COMES FROM"). Mapped to a
	// MaterialId because packBrickSolidBits routes through isSolidForFluid.
	// KNOWN LIMIT, stated: under -VoxelWaterMarker=1 the debug marker's
	// MAT_WATERMARK voxels read solid through this boolean path and would
	// wall off marked rivers; the marker is a bring-up diagnostic that is
	// refused alongside the GPU mesh fork anyway, and the fluid is not
	// expected to run in that mode.
	const auto MaterialAt = [WorldSub](int64 X, int64 Y, int64 Z) -> vxc::MaterialId
	{
		return WorldSub->IsSolidAtVoxel(X, Y, Z) ? vxc::MAT_ROCK : vxc::MAT_AIR;
	};

	const int32 MaxRegions = FMath::Max(1, CVarVoxelFluidOccRegionsPerTick.GetValueOnGameThread());
	const double BudgetSeconds =
		FMath::Max(0.5, double(CVarVoxelFluidOccPackMsPerTick.GetValueOnGameThread())) / 1000.0;
	const double Start = FPlatformTime::Seconds();

	int32 Built = 0;
	while (Built < MaxRegions && L.PendingRegions.Num() > 0 &&
	       (Built == 0 || FPlatformTime::Seconds() - Start < BudgetSeconds))
	{
		// Front of the queue: initial cells were queued centre-out, edits
		// append behind whatever is left -- strict FIFO keeps both fair.
		FVoxelFluidLifecycle::FRegion Region = L.PendingRegions[0];
		L.PendingRegions.RemoveAt(0, 1, EAllowShrinking::No);

		FVoxelFluidOccupancyRegion Packed;
		Packed.MinVoxel = Region.Min;
		Packed.SizeVoxels = Region.Size;
		FVoxelFluidOccupancyVolume::PackRegionBricks(OriginVoxel, Region.Min, Region.Size,
		                                             MaterialAt, Packed.BrickBits);
		FString Error;
		if (!Occupancy->UpdateRegion(MoveTemp(Packed), Error))
		{
			// Refused means we computed the box wrong -- loud, because the
			// symptom otherwise is water walking through a wall somebody dug.
			L.RegionsDropped++;
			UE_LOG(LogVoxelFluid, Error, TEXT("occupancy region REFUSED: %s"), *Error);
		}
		else
		{
			L.RegionsBuiltTotal++;
			if (Region.bInitialFill && L.InitialCellsRemaining > 0)
			{
				L.InitialCellsRemaining--;
				if (L.InitialCellsRemaining == 0)
				{
					UE_LOG(LogVoxelFluid, Display,
					       TEXT("Fluid occupancy initial fill COMPLETE (%d regions; %.1f ms total pack time)"),
					       kFillCellCount, L.PackMsTotal);
				}
			}
		}
		Built++;
	}
	L.PackMsTotal += (FPlatformTime::Seconds() - Start) * 1000.0;
}

void UVoxelFluidSubsystem::RefreshHeadwaterFaucets(double NowSeconds)
{
	FVoxelFluidLifecycle& L = *Lifecycle;
	if (NowSeconds < L.NextFaucetRefreshSeconds)
	{
		return;
	}
	L.NextFaucetRefreshSeconds = NowSeconds + kFaucetRefreshSeconds;

	UWorld* World = GetWorld();
	UVoxelWaterSubsystem* Water = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	UVoxelWorldSubsystem* WorldSub = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (Water == nullptr || WorldSub == nullptr)
	{
		return;
	}

	const double CenterX = FluidOriginWorld.X + kBoundaryHalfExtentUU;
	const double CenterY = FluidOriginWorld.Y + kBoundaryHalfExtentUU;

	// TWO SOURCES OF WATER FOR THE WINDOW, and every window needs exactly the
	// one it has. A window over a hillside gets its SPRINGS (rare, first-order
	// heads -- see the water subsystem's header); a window sitting mid-river has
	// no spring in it and gets the river where it CROSSES THE BOUNDARY. Before
	// this, the second case either produced nothing (the bv23 arm culls its rim
	// heads) or produced every fragment head in the box at once, which is what
	// drew solid lines of water down the valleys.
	TArray<UVoxelWaterSubsystem::FVoxelHeadwaterFaucet> Heads;
	UVoxelWaterSubsystem::FVoxelHeadwaterGatherStats Stats;
	Water->GatherHeadwaterFaucets(CenterX, CenterY, kBoundaryHalfExtentUU, Heads, Stats);
	Water->GatherRiverCrossings(CenterX, CenterY, kBoundaryHalfExtentUU, Heads, Stats);
	L.bFaucetGatherEverRan = true;
	L.bHeadsFromBakedTable = Stats.bFromBakedHeads;
	L.FaucetSprings = Stats.Springs;
	L.FaucetEdgeInflows = Stats.EdgeInflows;
	UE_LOG(LogVoxelFluid, Verbose,
	       TEXT("Faucet gather: %d heads in box -> %d in the Q band (tile min Q %lld) -> %d ")
	       TEXT("springs; %d edge inflows at %lld mm/yr (%s); source=%s"),
	       Stats.HeadsInBox, Stats.Candidates, Stats.TileMinQ, Stats.Springs, Stats.EdgeInflows,
	       Stats.RunoffMmPerYr, Stats.bRunoffCalibrated ? TEXT("fitted") : TEXT("FALLBACK"),
	       Stats.bFromBakedHeads ? TEXT("baked heads") : TEXT("bv23 graph"));

	// THE CAP IS NOW A GUARD, NOT THE SELECTION. Springs are spaced 150 m apart
	// and a box has four faces, so springs+edges is single digits by
	// construction; if it is not, the selection has failed somewhere upstream
	// and that is worth saying out loud rather than silently keeping the first
	// 32 of something.
	L.bFaucetCapTruncated = Heads.Num() > kMaxHeadFaucets;
	if (L.bFaucetCapTruncated)
	{
		UE_LOG(LogVoxelFluid, Warning,
		       TEXT("Faucet cap TRUNCATED the gather: %d emitters selected (%d springs from %d ")
		       TEXT("candidates of %d heads in box, %d edge inflows) against a cap of %d. The ")
		       TEXT("spring rule is supposed to make this impossible -- check ")
		       TEXT("voxel.Water.Springs.QFactorPct / SpacingPx."),
		       Heads.Num(), Stats.Springs, Stats.Candidates, Stats.HeadsInBox, Stats.EdgeInflows,
		       kMaxHeadFaucets);
	}

	// Rebuild the faucet list, PRESERVING accumulators for faucets that are
	// still present (matched by XY) so a refresh cannot double-emit or drop a
	// carried fraction.
	TArray<FVoxelFluidLifecycle::FHeadFaucet> Old = MoveTemp(L.HeadFaucets);
	L.HeadFaucets.Reset();
	for (const UVoxelWaterSubsystem::FVoxelHeadwaterFaucet& H : Heads)
	{
		if (L.HeadFaucets.Num() >= kMaxHeadFaucets)
		{
			break; // bounded gather; the cap is a stated limit, not a silent one
		}
		FVoxelFluidLifecycle::FHeadFaucet F;
		if (H.bEdgeInflow)
		{
			// AT the drawn water surface, moving the way the channel runs. A
			// river entering the box at ground level would enter under its own
			// water; entering with no velocity would enter as a dropped column.
			F.WorldPos = FVector(H.XUU, H.YUU, H.SurfaceZUU + kLifecycleFaucetHeightUU);
			F.Velocity = FVector3f(float(H.DirX) * kEdgeInflowSpeedUU,
			                       float(H.DirY) * kEdgeInflowSpeedUU, 0.0f);
			F.bEdgeInflow = true;
		}
		else
		{
			// A spring seeps out of the ground and runs downhill from there, so
			// it emits just above the drawn ground with no velocity of its own.
			const double GroundZ = WorldSub->GetSurfaceHeightUU(H.XUU, H.YUU);
			F.WorldPos = FVector(H.XUU, H.YUU, GroundZ + kLifecycleFaucetHeightUU);
		}
		F.QM3PerYear = H.QM3PerYear;
		for (const FVoxelFluidLifecycle::FHeadFaucet& O : Old)
		{
			if (FMath::IsNearlyEqual(O.WorldPos.X, F.WorldPos.X, 1.0) &&
			    FMath::IsNearlyEqual(O.WorldPos.Y, F.WorldPos.Y, 1.0))
			{
				F.Acc = O.Acc;
				break;
			}
		}
		L.HeadFaucets.Add(F);
	}
}

void UVoxelFluidSubsystem::RefreshBasinSink(double NowSeconds)
{
	FVoxelFluidLifecycle& L = *Lifecycle;
	if (NowSeconds < L.NextSinkRefreshSeconds)
	{
		return;
	}
	L.NextSinkRefreshSeconds = NowSeconds + kSinkRefreshSeconds;

	UVoxelWaterSubsystem* Water = GetWorld() ? GetWorld()->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (Water == nullptr)
	{
		L.bSinkValid = false;
		return;
	}

	const double MinX = FluidOriginWorld.X, MinY = FluidOriginWorld.Y;
	const double MaxX = MinX + kActiveEdgeUU, MaxY = MinY + kActiveEdgeUU;
	const double CenterX = MinX + kBoundaryHalfExtentUU, CenterY = MinY + kBoundaryHalfExtentUU;

	// Gather water-holding basins from every fine tile the region overlaps
	// (usually one -- a tile is 15.36 km).
	int32 Tx0 = 0, Ty0 = 0, Tx1 = 0, Ty1 = 0;
	UVoxelWaterSubsystem::FineTileForWorldUU(MinX, MinY, Tx0, Ty0);
	UVoxelWaterSubsystem::FineTileForWorldUU(MaxX, MaxY, Tx1, Ty1);
	TArray<UVoxelWaterSubsystem::FLakeSheetBasin> Basins;
	for (int32 Ty = Ty0; Ty <= Ty1; ++Ty)
	{
		for (int32 Tx = Tx0; Tx <= Tx1; ++Tx)
		{
			Water->GatherLakeSheetBasinsInTile(Tx, Ty, CenterX, CenterY, kBoundaryHalfExtentUU, Basins);
		}
	}

	// The pick is pure logic, unit-tested in voxel-core (fluidPickBasinSink):
	// nearest water-holding bbox intersecting the region. ONE basin -- the
	// stated v1 limit; a region straddling two lakes despawns into the picked
	// one only.
	TArray<vxc::FluidBasinCandidate> Candidates;
	Candidates.Reserve(Basins.Num());
	for (const UVoxelWaterSubsystem::FLakeSheetBasin& B : Basins)
	{
		vxc::FluidBasinCandidate C;
		C.minXMm = VoxelCoords::WorldToMm(B.MinXUU);
		C.minYMm = VoxelCoords::WorldToMm(B.MinYUU);
		C.maxXMm = VoxelCoords::WorldToMm(B.MaxXUU);
		C.maxYMm = VoxelCoords::WorldToMm(B.MaxYUU);
		C.holdsWater = true; // GatherLakeSheetBasinsInTile only returns holders
		Candidates.Add(C);
	}
	const int32 Pick = vxc::fluidPickBasinSink(Candidates.GetData(), Candidates.Num(),
	                                           VoxelCoords::WorldToMm(MinX), VoxelCoords::WorldToMm(MinY),
	                                           VoxelCoords::WorldToMm(MaxX), VoxelCoords::WorldToMm(MaxY));
	if (Pick < 0)
	{
		L.bSinkValid = false;
		return;
	}
	const UVoxelWaterSubsystem::FLakeSheetBasin& B = Basins[Pick];
	L.bSinkValid = true;
	L.SinkTileX = B.TileX;
	L.SinkTileY = B.TileY;
	L.SinkLocalBasinId = B.BasinId;
	L.SinkMinXUU = FMath::Max(B.MinXUU, MinX);
	L.SinkMinYUU = FMath::Max(B.MinYUU, MinY);
	L.SinkMaxXUU = FMath::Min(B.MaxXUU, MaxX);
	L.SinkMaxYUU = FMath::Min(B.MaxYUU, MaxY);
	// SurfaceZUU is the LEDGER-ADJUSTED datum (basinDatumMm), refreshed every
	// second here -- so as credits raise the lake, the sink's mouth rises with
	// it and the loop is self-consistent.
	L.SinkDatumZUU = B.SurfaceZUU;
}

void UVoxelFluidSubsystem::DrainSillSpills()
{
	FVoxelFluidLifecycle& L = *Lifecycle;
	UVoxelWaterSubsystem* Water = GetWorld() ? GetWorld()->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (Water == nullptr)
	{
		return;
	}
	TArray<UVoxelWaterSubsystem::FVoxelFluidSpillFaucet> Events;
	Water->DrainFluidSpillFaucets(Events);
	for (const UVoxelWaterSubsystem::FVoxelFluidSpillFaucet& E : Events)
	{
		if (E.Units <= 0)
		{
			continue;
		}
		L.SpillUnitsClaimed += E.Units;
		FVoxelFluidLifecycle::FSillFaucet S;
		S.BasinKey = E.BasinKey;
		S.UnitsRemaining = E.Units;
		S.WorldPos = FVector(E.OutletXUU, E.OutletYUU, E.SpillZUU + kLifecycleFaucetHeightUU);
		L.SillFaucets.Add(S);
	}
}

void UVoxelFluidSubsystem::ReconcileScalars()
{
	// The extended conservation line: what left the particle domain must be
	// exactly accounted for in ledger units -- credited, injected, pending
	// (retrying), or explicitly lost (teardown only). Violations are counted
	// AND logged, like the particle-side conservation check.
	FVoxelFluidLifecycle& L = *Lifecycle;
	const int64 BasinUnits = vxc::fluidParticleUnits(int64(L.BasinDespawnsSeen));
	const int64 BoundaryUnits = vxc::fluidParticleUnits(int64(L.BoundaryDespawnsSeen));
	bool bOk = BasinUnits == L.CreditedBasinUnits + L.PendingBasinCreditUnits + L.LostBasinUnits;
	bOk = bOk && BoundaryUnits ==
	                 L.InjectedBoundaryUnits + L.PendingBoundaryUnits + L.LostBoundaryUnits;
	int64 SillOutstanding = 0;
	for (const FVoxelFluidLifecycle::FSillFaucet& S : L.SillFaucets)
	{
		SillOutstanding += S.UnitsRemaining;
	}
	bOk = bOk && L.SpillUnitsClaimed ==
	                 vxc::fluidParticleUnits(int64(L.SpillParticlesEmitted)) + L.SpillUnitsRefunded +
	                     SillOutstanding;
	if (!bOk)
	{
		L.ScalarLedgerViolations++;
		UE_LOG(LogVoxelFluid, Error,
		       TEXT("Fluid SCALAR LEDGER VIOLATION: basin %lld != %lld+%lld+%lld | boundary %lld != ")
		       TEXT("%lld+%lld+%lld | spill %lld != %lld+%lld+%lld"),
		       BasinUnits, L.CreditedBasinUnits, L.PendingBasinCreditUnits, L.LostBasinUnits,
		       BoundaryUnits, L.InjectedBoundaryUnits, L.PendingBoundaryUnits, L.LostBoundaryUnits,
		       L.SpillUnitsClaimed, vxc::fluidParticleUnits(int64(L.SpillParticlesEmitted)),
		       L.SpillUnitsRefunded, SillOutstanding);
	}
}

void UVoxelFluidSubsystem::RunOccupancyVerify()
{
	FVoxelFluidLifecycle& L = *Lifecycle;
	if (!SimState.IsValid() || !Occupancy.IsValid())
	{
		return;
	}
	TArray<uint32> Words;
	uint64 Generation = 0;
	if (!SimState->TakeOccupancyVerifyWords(Words, Generation, L.LastVerifyTakenGeneration))
	{
		return;
	}
	L.LastVerifyTakenGeneration = Generation;

	// A snapshot armed before an edit landed compares CPU-now against GPU-then
	// and would report a mismatch that is only a race. Skipped, counted, and
	// visibly distinct from pass/fail.
	if (L.EditEpoch != L.VerifyArmedEpoch || L.PendingRegions.Num() > 0 ||
	    L.InitialCellsRemaining > 0)
	{
		L.VerifySkips++;
		L.LastVerifyResult = 2;
		return;
	}
	UVoxelWorldSubsystem* WorldSub = GetWorld() ? GetWorld()->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (WorldSub == nullptr)
	{
		return;
	}

	// The region under the cursor, rotating through the volume one 64^3 cell
	// per landed snapshot.
	const int32 Cursor = L.VerifyCursor;
	L.VerifyCursor = (L.VerifyCursor + 1) % kFillCellCount;
	const FIntVector Cell(Cursor % kFillCellsPerAxis, (Cursor / kFillCellsPerAxis) % kFillCellsPerAxis,
	                      Cursor / (kFillCellsPerAxis * kFillCellsPerAxis));
	const FIntVector MinLocal = Cell * kFillCellVoxels;
	const FIntVector Size(kFillCellVoxels, kFillCellVoxels, kFillCellVoxels);

	// CPU expectation: pack the same bricks and run vxc::fluidFillRegion, THE
	// CPU REFERENCE, into a full-volume scratch -- literally the function the
	// GPU kernel mirrors and vxc_tests pins.
	const auto MaterialAt = [WorldSub](int64 X, int64 Y, int64 Z) -> vxc::MaterialId
	{
		return WorldSub->IsSolidAtVoxel(X, Y, Z) ? vxc::MAT_ROCK : vxc::MAT_AIR;
	};
	FVoxelFluidOccupancyVolume::PackRegionBricks(OriginVoxel, MinLocal, Size, MaterialAt,
	                                             L.VerifyBrickBits);
	if (L.VerifyScratch.Num() != int32(vxc::kFluidVolumeWords))
	{
		L.VerifyScratch.SetNumZeroed(int32(vxc::kFluidVolumeWords));
	}
	vxc::FluidRegion Region;
	Region.minVoxel[0] = MinLocal.X;
	Region.minVoxel[1] = MinLocal.Y;
	Region.minVoxel[2] = MinLocal.Z;
	Region.sizeVoxels[0] = Size.X;
	Region.sizeVoxels[1] = Size.Y;
	Region.sizeVoxels[2] = Size.Z;
	// THE VERIFY MUST APPLY THE SAME WRAP THE GPU APPLIED (contract item 4). The
	// volume is toroidal: the cursor names a cell in the WINDOW, and where those
	// words live depends on how far the window has slid. Compare at a flat index
	// and, the moment the wrap offset is non-zero, this gate compares two
	// different voxels and reports every one of them as a FAIL.
	const FIntVector WrapV = Occupancy->GetWrapOffsetVoxel();
	const int32 Wrap[3] = {WrapV.X, WrapV.Y, WrapV.Z};
	vxc::fluidFillRegion(L.VerifyScratch.GetData(), Wrap, Region, L.VerifyBrickBits.GetData());

	// Byte-compare the region's words.
	bool bPass = true;
	const int32 WordsX = Size.X / vxc::kFluidBitsPerWord;
	for (int32 Rz = 0; Rz < Size.Z && bPass; ++Rz)
	{
		for (int32 Ry = 0; Ry < Size.Y && bPass; ++Ry)
		{
			for (int32 Wx = 0; Wx < WordsX; ++Wx)
			{
				const int32 Lx = MinLocal.X + Wx * vxc::kFluidBitsPerWord;
				const int32 Ly = MinLocal.Y + Ry;
				const int32 Lz = MinLocal.Z + Rz;
				const int64 Idx = vxc::fluidVolumeWordIndexLocal(Wrap, Lx, Ly, Lz);
				const uint32 Gpu = Words[int32(Idx)];
				const uint32 Cpu = L.VerifyScratch[int32(Idx)];
				if (Gpu != Cpu)
				{
					bPass = false;
					UE_LOG(LogVoxelFluid, Error,
					       TEXT("Fluid occupancy VERIFY FAIL: region min local (%d,%d,%d), first mismatch ")
					       TEXT("word %lld at local voxel (%d..%d,%d,%d): gpu=0x%08x cpu=0x%08x ")
					       TEXT("(world voxel x %d..%d, y %d, z %d)"),
					       MinLocal.X, MinLocal.Y, MinLocal.Z, (long long)Idx, Lx,
					       Lx + vxc::kFluidBitsPerWord - 1, Ly, Lz, Gpu, Cpu, OriginVoxel.X + Lx,
					       OriginVoxel.X + Lx + vxc::kFluidBitsPerWord - 1, OriginVoxel.Y + Ly,
					       OriginVoxel.Z + Lz);
					break;
				}
			}
		}
	}
	L.LastVerifyResult = bPass ? 1 : 0;
	if (bPass)
	{
		L.VerifyPasses++;
	}
	else
	{
		L.VerifyFails++;
	}
}

void UVoxelFluidSubsystem::ReleaseSimState()
{
	if (!Lifecycle.IsValid())
	{
		// The FVTableHelper (hot-reload) constructor path never allocated one.
		Lifecycle = MakeUnique<FVoxelFluidLifecycle>();
	}
	FVoxelFluidLifecycle& L = *Lifecycle;
	UWorld* World = GetWorld();

	// Unhook the terrain listener and the spill intercept FIRST, so nothing
	// queues into state we are about to reset.
	if (bTerrainListenerRegistered)
	{
		if (UVoxelWorldSubsystem* WorldSub = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr)
		{
			WorldSub->SetFluidTerrainDirtyListener({});
		}
		bTerrainListenerRegistered = false;
	}
	if (UVoxelWaterSubsystem* Water = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr)
	{
		Water->SetFluidSpillIntercept(false);

		// NEVER LOSE UNITS: refund every sill-faucet unit still owed, and make
		// one last attempt at the pending basin credit / boundary injection.
		for (const FVoxelFluidLifecycle::FSillFaucet& S : L.SillFaucets)
		{
			if (S.UnitsRemaining > 0)
			{
				L.SpillUnitsRefunded += Water->RefundSpillUnits(S.BasinKey, S.UnitsRemaining);
			}
		}
		L.SillFaucets.Reset();
		if (L.PendingBasinCreditUnits > 0 && L.bSinkValid)
		{
			int32 LevelMm = 0, BakedMm = 0;
			const int64 Accepted = Water->CreditBasinVolume(L.SinkTileX, L.SinkTileY, L.SinkLocalBasinId,
			                                                L.PendingBasinCreditUnits, LevelMm, BakedMm);
			L.CreditedBasinUnits += Accepted;
			L.PendingBasinCreditUnits -= Accepted;
		}
		if (L.PendingBoundaryUnits > 0)
		{
			const int32 Half = vxc::kFluidVolumeDimVoxels / 2;
			const int64 Accepted = Water->InjectRiverInflowNearVoxel(
				int64(OriginVoxel.X) + Half, int64(OriginVoxel.Y) + Half, L.PendingBoundaryUnits,
				kBoundaryInjectReachMm);
			L.InjectedBoundaryUnits += Accepted;
			L.PendingBoundaryUnits -= Accepted;
		}
	}
	// Renderer teardown FIRST: flip the shared settings off so a frame already
	// in flight declines cleanly, and unlink the sim state so the extension
	// can never splat buffers that ReleaseRenderThread is about to null. The
	// shared pointers themselves die with the Lifecycle reset below.
	if (L.RenderState.IsValid())
	{
		FScopeLock Guard(&L.RenderState->Lock);
		L.RenderState->Settings.bEnabled = false;
		L.RenderState->SimState.Reset();
	}

	if (L.PendingBasinCreditUnits > 0 || L.PendingBoundaryUnits > 0)
	{
		// Stated loss, not silent: these units left the particle domain and
		// nothing scalar would take them (no resolvable basin / no graph).
		L.LostBasinUnits += L.PendingBasinCreditUnits;
		L.LostBoundaryUnits += L.PendingBoundaryUnits;
		UE_LOG(LogVoxelFluid, Warning,
		       TEXT("Fluid teardown: %lld basin-credit units and %lld boundary units had no scalar ")
		       TEXT("consumer and are recorded as LOST (lifetime lost: basin %lld, boundary %lld)."),
		       L.PendingBasinCreditUnits, L.PendingBoundaryUnits, L.LostBasinUnits, L.LostBoundaryUnits);
		L.PendingBasinCreditUnits = 0;
		L.PendingBoundaryUnits = 0;
	}

	if (SimState.IsValid())
	{
		TSharedPtr<FVoxelFluidSimState, ESPMode::ThreadSafe> StatePtr = SimState;
		SimState.Reset();
		ENQUEUE_RENDER_COMMAND(VoxelFluidRelease)(
			[StatePtr](FRHICommandListImmediate&)
			{
				VoxelFluidSim::ReleaseRenderThread(*StatePtr);
			});
		UE_LOG(LogVoxelFluid, Display,
		       TEXT("Fluid sim released (conservation violations %llu, scalar-ledger violations %llu, ")
		       TEXT("faucet emitted %llu, spill emitted %llu, credited %lld units, injected %lld units)"),
		       ConservationViolations, L.ScalarLedgerViolations, L.HeadFaucetParticlesEmitted,
		       L.SpillParticlesEmitted, L.CreditedBasinUnits, L.InjectedBoundaryUnits);
	}
	// The volume's destructor enqueues its own RHI release; dropping the last
	// game-thread reference here is safe whether or not render commands still
	// hold theirs.
	Occupancy.Reset();

	// Fresh-session accounting: a re-enable starts conservation-clean.
	Lifecycle = MakeUnique<FVoxelFluidLifecycle>();
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
	// CREATE, do not gate. Lifecycle was only ever constructed inside
	// ReleaseSimState() -- the DISABLE path -- so on the first tick it was null
	// and this function returned silently forever: voxel.Fluid.Enable 1 did
	// nothing, no perf line, no error. The measurement harness's NOT-RUN rule
	// is what caught it. A missing member on the happy path is constructed,
	// not treated as "not applicable".
	if (!Lifecycle.IsValid())
	{
		Lifecycle = MakeUnique<FVoxelFluidLifecycle>();
	}

	if (CVarVoxelFluidEnable.GetValueOnGameThread() == 0)
	{
		if (SimState.IsValid() || Occupancy.IsValid())
		{
			ReleaseSimState();
		}
		return;
	}

	if (GUsingNullRHI)
	{
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
	if (!Occupancy.IsValid())
	{
		Occupancy = MakeShared<FVoxelFluidOccupancyVolume, ESPMode::ThreadSafe>();
	}
	if (!bTerrainListenerRegistered)
	{
		if (UVoxelWorldSubsystem* WorldSub = World->GetSubsystem<UVoxelWorldSubsystem>())
		{
			TWeakObjectPtr<UVoxelFluidSubsystem> WeakThis(this);
			WorldSub->SetFluidTerrainDirtyListener(
				[WeakThis](int64 MinVx, int64 MinVy, int64 MinVz, int64 MaxVx, int64 MaxVy, int64 MaxVz)
				{
					if (UVoxelFluidSubsystem* Self = WeakThis.Get())
					{
						Self->NotifyTerrainDirty(MinVx, MinVy, MinVz, MaxVx, MaxVy, MaxVz);
					}
				});
			bTerrainListenerRegistered = true;
		}
	}

	FVoxelFluidLifecycle& L = *Lifecycle;
	const double Now = FPlatformTime::Seconds();
	const bool bFaucets = CVarVoxelFluidFaucets.GetValueOnGameThread() != 0;

	// ---- view origin -------------------------------------------------------
	FVector ViewOrigin = FVector::ZeroVector;
	if (const APlayerController* PC = World->GetFirstPlayerController())
	{
		if (PC->PlayerCameraManager != nullptr)
		{
			ViewOrigin = PC->PlayerCameraManager->GetCameraLocation();
		}
	}

	// ---- origin latch (first spawn / emit / faucet arm) --------------------
	if (!bOriginLatched && (PendingSpawnCount > 0 || EmitPerSecond > 0.0f || bFaucets))
	{
		LatchOrigin(ViewOrigin);
	}

	// ---- occupancy feed ----------------------------------------------------
	if (bOriginLatched)
	{
		ProcessOccupancyQueue();
	}

	// ---- lifecycle refreshes ----------------------------------------------
	if (bOriginLatched && bFaucets)
	{
		RefreshHeadwaterFaucets(Now);
		RefreshBasinSink(Now);
		DrainSillSpills();
	}

	// ---- assemble this frame's spawn dispatches -----------------------------
	// A local clamp keeps every emission inside the active box: a spawn that
	// starts outside would boundary-despawn on its first finalize and silently
	// become graph inflow.
	const auto ClampLocal = [](FVector3f P) -> FVector3f
	{
		const float Lo = kSpawnClampMarginUU;
		const float Hi = kActiveEdgeUU - kSpawnClampMarginUU;
		return FVector3f(FMath::Clamp(P.X, Lo, Hi), FMath::Clamp(P.Y, Lo, Hi),
		                 FMath::Clamp(P.Z, Lo, Hi));
	};

	TArray<FVoxelFluidSpawnRequest, TInlineAllocator<8>> Spawns;
	uint32 FaucetEmittedThisTick = 0;
	uint32 SpillEmittedThisTick = 0;
	if (bOriginLatched)
	{
		const auto PushSpawn = [&](uint32 Count, uint32 Mode, const FVector& CenterWorld,
		                           const FVector3f& Vel)
		{
			FVoxelFluidSpawnRequest S;
			S.Count = Count;
			S.Mode = Mode;
			S.CenterLocalUU = ClampLocal(FVector3f(CenterWorld - FluidOriginWorld));
			S.VelocityUU = Vel;
			S.Seed = uint32(GFrameCounter) * 97u + uint32(Spawns.Num());
			S.BatchId = SpawnBatchCounter;
			SpawnBatchCounter += 1.0f;
			CumulativeSpawnRequested += Count;
			Spawns.Add(S);
		};

		// 1. Dam-break block: an explicit user request; not budget-capped.
		if (PendingSpawnCount > 0)
		{
			PushSpawn(uint32(PendingSpawnCount), 0,
			          ViewOrigin + FVector(0, 0, kSpawnHeightAboveViewUU), FVector3f::ZeroVector);
			PendingSpawnCount = 0;
		}

		int32 Budget = FMath::Max(0, CVarVoxelFluidMaxSpawnPerTick.GetValueOnGameThread());

		// 2. Camera faucet (voxel.Fluid.Emit).
		if (EmitPerSecond > 0.0f)
		{
			EmitCarry += EmitPerSecond * DeltaTime;
			const int32 EmitNow = FMath::Min(FMath::FloorToInt(EmitCarry), Budget);
			if (EmitNow > 0)
			{
				EmitCarry -= float(EmitNow);
				if (!bFaucetLatched)
				{
					FaucetCenterWorld = ViewOrigin + FVector(0, 0, kFaucetHeightAboveViewUU);
					bFaucetLatched = true;
				}
				PushSpawn(uint32(EmitNow), 1, FaucetCenterWorld, kFaucetVelocityUU);
				Budget -= EmitNow;
			}
		}

		// 3. Headwater faucets: rate = baked Q (or the default), scheduled by
		// the exact integer accumulator (voxelcore/fluidlifecycle.h). Budget
		// overflow is RE-CARRIED, never dropped.
		if (bFaucets)
		{
			// POPULATION BACKPRESSURE, learned from the first faucet demo. The
			// 21 headwaters near the wet trunk legitimately carry ~8 m^3/s --
			// the emission maths was RIGHT -- but a 51 m window cannot hold a
			// real river's throughput: the buffer pinned at its 307k cap, the
			// sim hit 857 ms/frame, and the choked sim never advanced the flow
			// far enough to drain out the boundary. So faucets throttle as the
			// alive count approaches a soft ceiling, and the discharge that is
			// NOT emitted as particles is routed AROUND the window as scalar
			// graph inflow at the faucet's own cell -- the water still flows,
			// the ledgers still close, and the particles only ever model the
			// share of the river the window can afford to carry. Virtualised
			// litres are counted (FaucetVirtualLitres) so the perf line can
			// say how much of the river is scalar right now.
			// (Local fetch: the function-scope snapshot is taken further down,
			// after spawn assembly. One extra lock per tick; the ramp only
			// needs a value at most a readback-latency stale, which any landed
			// snapshot is anyway.)
			const FVoxelFluidCountsSnapshot Snapshot = SimState->GetLatestCounts();
			const uint32 AliveNow = Snapshot.bValid ? Snapshot.Alive : 0u;
			// kMaxParticles/3 = 102k: the MEASURED knee. 100k settled costs
			// simGpuMs 2.4 (sorted gathers); the first backpressure run let the
			// pool reach 156k and paid 21 ms -- deep piles compress and the
			// constraint cost is superlinear in local density, so the cap sits
			// at the scale the measurements actually cleared.
			const uint32 SoftCap = uint32(VoxelFluidSim::kMaxParticles / 3);
			const uint32 RampWidth = SoftCap / 2;                                  // full->zero over 76k
			float EmitScale = 1.0f;
			if (AliveNow >= SoftCap)
			{
				EmitScale = 0.0f;
			}
			else if (AliveNow + RampWidth > SoftCap)
			{
				EmitScale = float(SoftCap - AliveNow) / float(RampWidth);
			}

			const int64 DtMicros = int64(double(DeltaTime) * 1.0e6);
			const int64 DefaultQ = int64(FMath::Max(0.0f, CVarVoxelFluidFaucetDefaultQ.GetValueOnGameThread()));
			L.FaucetsDeferredNoOccupancyNow = 0;
			L.FaucetsDeferredOutsideVolumeNow = 0;
			// One helper for both faucet loops: defer, and record WHICH KIND of
			// deferral it was (unbuilt yet vs never in the box). The volume is
			// asked the containment question only on the deferral path.
			const auto CountDeferral = [&L, this](const FIntVector& WorldVoxel)
			{
				L.FaucetTicksDeferredNoOccupancy++;
				L.FaucetsDeferredNoOccupancyNow++;
				if (!Occupancy.IsValid() || !Occupancy->ContainsWorldVoxel(WorldVoxel))
				{
					L.FaucetTicksDeferredOutsideVolume++;
					L.FaucetsDeferredOutsideVolumeNow++;
				}
			};
			for (FVoxelFluidLifecycle::FHeadFaucet& F : L.HeadFaucets)
			{
				// DEFER INTO UNBUILT OCCUPANCY (playtest fix 2026-08-09). The
				// initial fill is centre-out and multi-second, and unfilled
				// space is SOLID by design -- so a faucet at the far corner of
				// the box emitted into notional rock and its particles froze in
				// mid-air at the edge of the built region. That frozen shell was
				// the other half of the "square plane of hovering water".
				//
				// Skipped BEFORE addMicros, deliberately: the accumulator keeps
				// its carry, so nothing is dropped and no debt accrues while the
				// fill catches up. (Contrast the backpressure path below, which
				// ROUTES its surplus because the alive cap is a steady state a
				// faucet would otherwise owe against forever. This one clears in
				// seconds.) Emitting nowhere is the right answer -- a headwater
				// with no terrain under it has nothing to run down.
				const VoxelCoords::FVoxelCoord EmitVoxel = VoxelCoords::WorldToVoxel(F.WorldPos);
				const FIntVector EmitWorldVoxel(int32(EmitVoxel.X), int32(EmitVoxel.Y),
				                                int32(EmitVoxel.Z));
				if (!Occupancy.IsValid() || !Occupancy->IsRegionBuilt(EmitWorldVoxel))
				{
					CountDeferral(EmitWorldVoxel);
					continue;
				}
				const int64 Q = F.QM3PerYear > 0.0 ? int64(F.QM3PerYear) : DefaultQ;
				int64 Owed = F.Acc.addMicros(Q, DtMicros);
				if (Owed <= 0)
				{
					continue;
				}
				const int64 Wanted = int64(double(Owed) * EmitScale);
				const int64 Emit = FMath::Min(Wanted, int64(Budget));
				const int64 Virtual = Owed - Emit;
				if (Virtual > 0)
				{
					// Not carried, ROUTED: carrying builds unbounded debt the
					// window can never repay (the river never stops). The
					// surplus flows through the scalar graph instead, injected
					// at the faucet's own cell; refusal (no graph armed) is
					// counted, not lost silently.
					UVoxelWaterSubsystem* WaterNow =
						GetWorld() ? GetWorld()->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
					const int64 VirtualUnits = vxc::fluidParticleUnits(Virtual);
					int64 Accepted = 0;
					if (WaterNow != nullptr)
					{
						Accepted = WaterNow->InjectRiverInflowNearVoxel(
							int64(F.WorldPos.X / 10.0), int64(F.WorldPos.Y / 10.0),
							VirtualUnits, kBoundaryInjectReachMm);
					}
					L.FaucetVirtualUnitsRouted += Accepted;
					L.FaucetVirtualUnitsRefused += (VirtualUnits - Accepted);
				}
				if (Emit <= 0)
				{
					continue;
				}
				PushSpawn(uint32(Emit), 1, F.WorldPos, F.Velocity);
				Budget -= int32(Emit);
				L.HeadFaucetParticlesEmitted += uint64(Emit);
				FaucetEmittedThisTick += uint32(Emit);
			}

			// 4. Sill faucets: emit whole particles from the owed units; the
			// sub-particle remainder is refunded when the entry drains.
			for (int32 I = L.SillFaucets.Num() - 1; I >= 0; --I)
			{
				FVoxelFluidLifecycle::FSillFaucet& S = L.SillFaucets[I];
				// Same unbuilt-occupancy deferral as the headwaters above. The
				// owed units simply stay owed (and are refunded at teardown by
				// the existing path), so a sill over an unfilled corner of the
				// volume waits instead of spraying into rock.
				const VoxelCoords::FVoxelCoord SillVoxel = VoxelCoords::WorldToVoxel(S.WorldPos);
				const FIntVector SillWorldVoxel(int32(SillVoxel.X), int32(SillVoxel.Y),
				                                int32(SillVoxel.Z));
				if (!Occupancy.IsValid() || !Occupancy->IsRegionBuilt(SillWorldVoxel))
				{
					CountDeferral(SillWorldVoxel);
					continue;
				}
				const int64 MaxParticles = vxc::fluidWholeParticlesFromUnits(S.UnitsRemaining);
				const int64 Emit = FMath::Min(MaxParticles, int64(Budget));
				if (Emit > 0)
				{
					PushSpawn(uint32(Emit), 1, S.WorldPos, FVector3f::ZeroVector);
					Budget -= int32(Emit);
					S.UnitsRemaining -= vxc::fluidParticleUnits(Emit);
					L.SpillParticlesEmitted += uint64(Emit);
					SpillEmittedThisTick += uint32(Emit);
				}
				if (S.UnitsRemaining < vxc::kFluidLedgerUnitsPerParticle)
				{
					// Below one particle: refund the tail so the ledger closes.
					if (S.UnitsRemaining > 0)
					{
						if (UVoxelWaterSubsystem* Water = World->GetSubsystem<UVoxelWaterSubsystem>())
						{
							L.SpillUnitsRefunded += Water->RefundSpillUnits(S.BasinKey, S.UnitsRemaining);
						}
						else
						{
							L.SpillUnitsRefunded += S.UnitsRemaining; // no water subsystem: accounted, not paid
						}
						S.UnitsRemaining = 0;
					}
					L.SillFaucets.RemoveAt(I);
				}
				if (Budget <= 0)
				{
					break;
				}
			}
		}
	}
	L.PerfFaucetEmitted += FaucetEmittedThisTick;
	L.PerfSpillEmitted += SpillEmittedThisTick;

	// ---- screen-space renderer hookup (voxel.Fluid.Render, Phase 4) --------
	// Created lazily; refreshed every tick with by-value settings + the live
	// sim-state pointer, so an Enable-cycle (new sim state object) re-links
	// automatically and a cvar flip to 0 turns the passes off next frame.
	const bool bRenderCVarOn = CVarVoxelFluidRender.GetValueOnGameThread() != 0;
	// The extension is created whenever the FLUID runs, not only when the
	// renderer cvar is on: it now carries the solver's passes too (the sim
	// rides the renderer's graph via PreRenderViewFamily -- see AddSimPasses'
	// comment for the standalone-builder crash that forced this). Rendering
	// stays gated inside the extension by Settings.bEnabled.
	if (!L.RenderExtension.IsValid())
	{
		L.RenderState = MakeShared<FVoxelFluidRenderState, ESPMode::ThreadSafe>();
		L.RenderExtension = FSceneViewExtensions::NewExtension<FVoxelFluidRenderExtension>(
			World, L.RenderState);
		UE_LOG(LogVoxelFluid, Display,
		       TEXT("Fluid view extension registered (sim passes at PreRenderViewFamily, ")
		       TEXT("render passes at PrePostProcessPass; ProfileGPU: VoxelFluidSim/-Render.*)"));
	}
	if (L.RenderState.IsValid())
	{
		// Sun for the constant-sky Fresnel + the diffuse approximation --
		// same ephemeris the water material's SunDirection MPC entry is fed
		// from (VoxelSkySubsystem), same toward-the-sun convention
		// (VoxelEphemeris.h:48). A sky subsystem that never ticked returns a
		// zeroed state (JulianDay 0); the renderer then keeps its overhead-sun
		// default rather than computing a sun at the eastern horizon out of
		// zeros -- mirrors the material reading an un-driven MPC default.
		FVector3f SunDirWorld(0.0f, 0.0f, 1.0f);
		float SunDayGate = 1.0f;
		if (UVoxelSkySubsystem* Sky = World->GetSubsystem<UVoxelSkySubsystem>())
		{
			const FVoxelSkyState& SkyState = Sky->GetSkyState();
			if (SkyState.JulianDay != 0.0)
			{
				const double AltRad = FMath::DegreesToRadians(SkyState.SunAltitudeDeg);
				const double AzRad = FMath::DegreesToRadians(SkyState.SunAzimuthDeg);
				SunDirWorld = FVector3f(float(FMath::Cos(AltRad) * FMath::Cos(AzRad)),
				                        float(FMath::Cos(AltRad) * FMath::Sin(AzRad)),
				                        float(FMath::Sin(AltRad)));
				SunDayGate = FMath::Clamp(float(FMath::Sin(AltRad)), 0.0f, 1.0f);
			}
		}

		FScopeLock Guard(&L.RenderState->Lock);
		L.RenderState->Settings.bEnabled =
			bRenderCVarOn && bOriginLatched && CumulativeSpawnRequested > 0;
		L.RenderState->Settings.FluidOriginWorld = FluidOriginWorld;
		L.RenderState->Settings.ParticleRadiusUU =
			CVarVoxelFluidRenderRadius.GetValueOnGameThread();
		L.RenderState->Settings.SmoothRadiusPx =
			CVarVoxelFluidRenderSmoothRadius.GetValueOnGameThread();
		L.RenderState->Settings.SunDirWorld = SunDirWorld;
		L.RenderState->Settings.SunDayGate = SunDayGate;
		L.RenderState->SimState = SimState;
	}

	const FVoxelFluidCountsSnapshot Snapshot = SimState->GetLatestCounts();

	// ---- drive the GPU (every frame the origin is latched) -----------------
	//
	// NOT "once something has ever spawned", which is what stood here and which
	// deadlocked the whole feature: the occupancy volume is only ever built
	// inside the sim tick, and faucets refuse to emit into occupancy that is
	// not built. See VoxelFluidSim::ShouldTickSim for the measured account. A
	// tick with SimSlotBound == 0 runs the volume's clear/fills and returns
	// before any solver pass.
	const float SimDt = FMath::Min(DeltaTime, kMaxSimDtSeconds);
	const bool bVerify = CVarVoxelFluidOccVerify.GetValueOnGameThread() != 0;
	if (VoxelFluidSim::ShouldTickSim(bOriginLatched, SimDt))
	{
		const bool bDebugDraw = CVarVoxelFluidDebugDraw.GetValueOnGameThread() != 0;

		FVoxelFluidSimTickArgs Args;
		Args.Dt = SimDt;
		Args.Iterations = FMath::Clamp(CVarVoxelFluidIterations.GetValueOnGameThread(), 1, 8);
		Args.SimSlotBound =
			uint32(FMath::Min<uint64>(CumulativeSpawnRequested, VoxelFluidSim::kMaxParticles));
		Args.GroundZLocalUU = -kGroundBelowOriginUU; // dead under collision; see the header
		Args.BoundaryHalfExtentUU = kBoundaryHalfExtentUU;
		Args.BoundaryCenterLocalUU =
			FVector3f(kBoundaryHalfExtentUU, kBoundaryHalfExtentUU, kBoundaryHalfExtentUU);
		Args.Occupancy = Occupancy.Get();
		Args.Spawns = Spawns;
		Args.bVerifyOccupancy = bVerify;
		if (bVerify)
		{
			L.VerifyArmedEpoch = L.EditEpoch;
		}
		if (bFaucets && L.bSinkValid)
		{
			Args.bBasinSinkEnabled = true;
			Args.BasinBoxMinLocalUU = FVector3f(float(L.SinkMinXUU - FluidOriginWorld.X),
			                                    float(L.SinkMinYUU - FluidOriginWorld.Y), 0.0f);
			Args.BasinBoxMaxLocalUU = FVector3f(float(L.SinkMaxXUU - FluidOriginWorld.X),
			                                    float(L.SinkMaxYUU - FluidOriginWorld.Y), kActiveEdgeUU);
			Args.BasinDatumZLocalUU = float(L.SinkDatumZUU - FluidOriginWorld.Z);
		}
		Args.bReadbackDebugSlots =
			bDebugDraw && (!Snapshot.bValid || Snapshot.Alive <= kDebugDrawMaxParticles);
		Args.DebugSlotCount = Args.SimSlotBound;

		// Post to the mailbox; the view extension consumes it exactly once at
		// PreRenderViewFamily and adds the passes to the renderer's graph.
		// (The ENQUEUE_RENDER_COMMAND + own-FRDGBuilder shape that stood here
		// crashed UE 5.8's breadcrumb assert on first Execute -- AddSimPasses'
		// comment carries the account.)
		{
			FScopeLock Guard(&L.RenderState->Lock);
			L.RenderState->PendingSimArgs = Args;
			L.RenderState->OccupancyKeepAlive = Occupancy;
			L.RenderState->SimState = SimState;
		}
	}

	// ---- conservation: asserted on every readback generation ---------------
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

	// ---- particle -> scalar hand-offs (basin credits, boundary injection) --
	if (Snapshot.bValid)
	{
		UVoxelWaterSubsystem* Water = World->GetSubsystem<UVoxelWaterSubsystem>();

		// Basin despawns since last accounted, in ledger units, plus any
		// pending (previously refused) units -- credited as one call. The
		// conversion is THE constant (255/particle, fluidlifecycle.h).
		if (uint64(Snapshot.DespawnedBasin) > L.BasinDespawnsSeen)
		{
			const int64 NewParticles = int64(uint64(Snapshot.DespawnedBasin) - L.BasinDespawnsSeen);
			L.BasinDespawnsSeen = uint64(Snapshot.DespawnedBasin);
			L.PendingBasinCreditUnits += vxc::fluidParticleUnits(NewParticles);
		}
		if (L.PendingBasinCreditUnits > 0 && L.bSinkValid && Water != nullptr)
		{
			int32 LevelMm = 0, BakedMm = 0;
			const int64 Accepted = Water->CreditBasinVolume(L.SinkTileX, L.SinkTileY, L.SinkLocalBasinId,
			                                                L.PendingBasinCreditUnits, LevelMm, BakedMm);
			if (Accepted > 0)
			{
				L.CreditedBasinUnits += Accepted;
				L.PendingBasinCreditUnits -= Accepted;
			}
			// Accepted == 0 (tile streamed out mid-session): units stay
			// pending and retry next tick -- refused, never guessed, never
			// dropped (the ledger's own doctrine).
		}

		// Boundary despawns -> the routing graph, attributed v1 to the segment
		// nearest the volume centre (positions are not read back; stated
		// limit). Refused units (no graph in reach) stay pending and retry.
		if (uint64(Snapshot.DespawnedBoundary) > L.BoundaryDespawnsSeen)
		{
			const int64 NewParticles = int64(uint64(Snapshot.DespawnedBoundary) - L.BoundaryDespawnsSeen);
			L.BoundaryDespawnsSeen = uint64(Snapshot.DespawnedBoundary);
			L.PendingBoundaryUnits += vxc::fluidParticleUnits(NewParticles);
		}
		if (L.PendingBoundaryUnits > 0 && Water != nullptr)
		{
			const int32 Half = vxc::kFluidVolumeDimVoxels / 2;
			const int64 Accepted = Water->InjectRiverInflowNearVoxel(
				int64(OriginVoxel.X) + Half, int64(OriginVoxel.Y) + Half, L.PendingBoundaryUnits,
				kBoundaryInjectReachMm);
			if (Accepted > 0)
			{
				L.InjectedBoundaryUnits += Accepted;
				L.PendingBoundaryUnits -= Accepted;
			}
		}

		ReconcileScalars();
	}

	// ---- occupancy verify compare ------------------------------------------
	if (bVerify)
	{
		RunOccupancyVerify();
	}
	else if (L.LastVerifyResult != -1)
	{
		L.LastVerifyResult = -1; // cvar off: back to "off", not a stale "pass"
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
			bDebugDrawSkippedTooMany = true;
		}
	}

	// ---- the 1 Hz perf line ------------------------------------------------
	// Extended (integration pass): faucet / sink / occupancy / verify, every
	// field distinguishable from did-not-run ("off" vs a real 0).
	if (Now >= NextPerfLogTime)
	{
		NextPerfLogTime = Now + kPerfLogPeriodSeconds;
		const TCHAR* StateMarker =
			!Snapshot.bValid ? TEXT("[idle: no readback yet]")
			: (Snapshot.SpawnedTotal == 0 ? TEXT("[idle]") : TEXT("[run]"));

		FString FaucetField;
		if (!bFaucets)
		{
			FaucetField = TEXT("off");
		}
		else if (!L.bFaucetGatherEverRan)
		{
			FaucetField = TEXT("gathering");
		}
		else
		{
			// springs=<selected> and edges=<crossings> are the two things the
			// selection is FOR, so they are the two numbers on the line: a box
			// with springs=0,edges=1 is a window mid-river being fed from its
			// boundary (correct), and springs=12 would mean the band has come
			// loose. "heads" vs "fallback" still says which source answered.
			FaucetField = FString::Printf(TEXT("%llu/s(springs=%d,edges=%d,%s%s)"),
			                              L.PerfFaucetEmitted, L.FaucetSprings,
			                              L.FaucetEdgeInflows,
			                              L.bHeadsFromBakedTable ? TEXT("heads") : TEXT("fallback"),
			                              L.bFaucetCapTruncated ? TEXT(",CAPPED") : TEXT(""));
		}

		// deferredNoOccupancy: emitters that skipped this tick because their
		// point sits in occupancy the initial fill has not reached, or outside
		// the volume entirely. "off" when the lifecycle is not armed,
		// "<now>(outside=<now>)/<total>(outside=<total>)" otherwise.
		//
		// THE (outside=) SPLIT IS THE WHOLE VALUE OF THIS FIELD, and it is here
		// because the merged form could not tell the two apart in
		// Saved/owner-playtest-round3.log: unbuilt clears in seconds, outside
		// never clears. outside == 0 with a steady non-zero left number means
		// the fill is not landing; outside == the left number means the
		// emitters are not in the box (the window follows the CAMERA, so flying
		// well above the ground leaves the terrain below its floor).
		const FString DeferredField =
			!bFaucets ? FString(TEXT("off"))
			          : FString::Printf(TEXT("%d(outside=%d)/%llu(outside=%llu)"),
			                            L.FaucetsDeferredNoOccupancyNow,
			                            L.FaucetsDeferredOutsideVolumeNow,
			                            L.FaucetTicksDeferredNoOccupancy,
			                            L.FaucetTicksDeferredOutsideVolume);
		const uint64 BasinRate = uint64(Snapshot.DespawnedBasin) - L.LastPerfBasinDespawns;
		const uint64 BoundaryRate = uint64(Snapshot.DespawnedBoundary) - L.LastPerfBoundaryDespawns;
		L.LastPerfBasinDespawns = uint64(Snapshot.DespawnedBasin);
		L.LastPerfBoundaryDespawns = uint64(Snapshot.DespawnedBoundary);
		const FString SinkBasinField =
			!bFaucets ? TEXT("off")
			          : (L.bSinkValid ? FString::Printf(TEXT("%llu/s"), BasinRate) : TEXT("none"));
		const TCHAR* VerifyField = !bVerify            ? TEXT("off")
		                           : L.LastVerifyResult == 1 ? TEXT("pass")
		                           : L.LastVerifyResult == 0 ? TEXT("FAIL")
		                           : L.LastVerifyResult == 2 ? TEXT("stale")
		                                                     : TEXT("pending");

		// renderMs: off (cvar 0), pending (armed but no pass has completed a
		// GPU timing yet -- includes "no particles spawned"), or the newest
		// completed GPU time of the VoxelFluidRender pass span. The ran-flag
		// rule again: a renderer that silently never ran must not print 0.00.
		FString RenderField;
		if (!bRenderCVarOn)
		{
			RenderField = TEXT("off");
		}
		else
		{
			const FVoxelFluidRenderStats RenderStats =
				L.RenderState.IsValid() ? L.RenderState->GetStats() : FVoxelFluidRenderStats();
			RenderField = (RenderStats.FramesRendered == 0 || RenderStats.RenderGpuMs < 0.0f)
			                  ? TEXT("pending")
			                  : FString::Printf(TEXT("%.2f"), RenderStats.RenderGpuMs);
		}

		UE_LOG(LogVoxelFluid, Display,
		       TEXT("Fluid perf %s alive=%u spawned=%u requested=%llu despawnBasin=%u ")
		       TEXT("despawnBoundary=%u simGpuMs=%.2f renderMs=%s iters=%d slots=%llu violations=%llu ")
		       TEXT("faucet=%s spill=%llu/s sink(basin)=%s sink(boundary)=%llu/s ")
		       TEXT("occupancy=%llu/%d verify=%s skippedNoOcc=%llu deferredNoOccupancy=%s%s"),
		       StateMarker, Snapshot.Alive, Snapshot.SpawnedTotal, CumulativeSpawnRequested,
		       Snapshot.DespawnedBasin, Snapshot.DespawnedBoundary, Snapshot.SimGpuMs, *RenderField,
		       FMath::Clamp(CVarVoxelFluidIterations.GetValueOnGameThread(), 1, 8),
		       FMath::Min<uint64>(CumulativeSpawnRequested, VoxelFluidSim::kMaxParticles),
		       ConservationViolations, *FaucetField, L.PerfSpillEmitted, *SinkBasinField, BoundaryRate,
		       L.RegionsBuiltTotal, L.PendingRegions.Num(), VerifyField,
		       SimState->GetTicksSkippedNoOccupancy(), *DeferredField,
		       bDebugDrawSkippedTooMany ? TEXT(" debugDraw=skipped(alive>5000)") : TEXT(""));

		// The scalar side of the extended conservation line, only while the
		// lifecycle is doing anything -- an all-zero line every second would
		// bury the signal it exists to carry.
		if (bFaucets && (L.CreditedBasinUnits != 0 || L.PendingBasinCreditUnits != 0 ||
		                 L.InjectedBoundaryUnits != 0 || L.PendingBoundaryUnits != 0 ||
		                 L.SpillUnitsClaimed != 0))
		{
			UE_LOG(LogVoxelFluid, Display,
			       TEXT("Fluid ledger: emittedFaucet=%llu emittedSpill=%llu creditedToBasin=%lld(+%lld pending) ")
			       TEXT("injectedToGraph=%lld(+%lld pending) spillClaimed=%lld spillRefunded=%lld ")
			       TEXT("scalarViolations=%llu"),
			       L.HeadFaucetParticlesEmitted, L.SpillParticlesEmitted, L.CreditedBasinUnits,
			       L.PendingBasinCreditUnits, L.InjectedBoundaryUnits, L.PendingBoundaryUnits,
			       L.SpillUnitsClaimed, L.SpillUnitsRefunded, L.ScalarLedgerViolations);
		}
		L.PerfFaucetEmitted = 0;
		L.PerfSpillEmitted = 0;
	}
}
