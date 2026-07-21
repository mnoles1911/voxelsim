#include "VoxelDebug.h"

#include "HAL/IConsoleManager.h"
#include "Math/UnrealMathUtility.h"

DEFINE_LOG_CATEGORY(LogVoxelStream);
DEFINE_LOG_CATEGORY(LogVoxelEdit);
DEFINE_LOG_CATEGORY(LogVoxelPerf);
DEFINE_LOG_CATEGORY(LogVoxelWater);

DEFINE_STAT(STAT_VoxelSubsystemTick);
DEFINE_STAT(STAT_VoxelWorkerJob);
DEFINE_STAT(STAT_VoxelGameThreadMesh);
DEFINE_STAT(STAT_VoxelEditApply);
DEFINE_STAT(STAT_VoxelChunksLoaded);
DEFINE_STAT(STAT_VoxelChunksInFlight);

namespace
{
TAutoConsoleVariable<int32> CVarVoxelDebug(
	TEXT("voxel.Debug"),
	0,
	TEXT("Voxel debug mode: 0=off, 1=perf HUD, 2=HUD+visualizations. F3 cycles in PIE/game."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelDebugChunkStates(
	TEXT("voxel.Debug.ChunkStates"),
	true,
	TEXT("Chunk-state debug tints (just-loaded blue flash / edited orange / re-meshed purple flash). Live under voxel.Debug 2."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelDebugBounds(
	TEXT("voxel.Debug.Bounds"),
	true,
	TEXT("Chunk AABB bounds wireframe for the nearest ~200 tracked chunks. Live under voxel.Debug 2."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelDebugRings(
	TEXT("voxel.Debug.Rings"),
	false,
	TEXT("Tint loaded chunks by mip ring level (R0 green .. R4 magenta) instead of chunk-state tints. Live under voxel.Debug 2."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelMipCacheBudgetMB(
	TEXT("voxel.MipCacheBudgetMB"),
	512,
	TEXT("Approximate byte budget (MB) for the shared cross-job mip cache (FSharedMipCache, VoxelWorldSubsystem.cpp). ")
	TEXT("Sharded approximate-LRU evicts on insert once over budget. <= 0 disables eviction (unbounded)."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelServerMaxIntentsPerSec(
	TEXT("voxel.Server.MaxIntentsPerSec"),
	10,
	TEXT("M3 wave 2 validation hardening: per-connection token-bucket cap on dig/place/carve intent RPCs accepted per ")
	TEXT("second. Excess intents are rejected (logged), not disconnected."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarVoxelServerMaxCarveRadiusUU(
	TEXT("voxel.Server.MaxCarveRadiusUU"),
	400.0f,
	TEXT("M3 wave 2 validation hardening: server-side cap (UU) on ServerSubmitCarveIntent's RadiusUU. Requests above ")
	TEXT("this are rejected (logged)."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelWaterMaxActiveBricks(
	TEXT("voxel.Water.MaxActiveBricks"),
	4096,
	TEXT("W2: advisory per-tick budget on vxc::WaterCA's active-set size. The CA tick is atomic (no mid-step cutoff ")
	TEXT("without breaking conservation/determinism), so exceeding this only logs a throttled warning -- see ")
	TEXT("UVoxelWaterSubsystem::TickWater."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelStreamMaxAppliesPerFrame(
	TEXT("voxel.Stream.MaxAppliesPerFrame"),
	3,
	TEXT("Hitch isolation (docs/status.md 'Perf-run hitches'): max worker-mesh-result chunk-component applies ")
	TEXT("(FVoxelWorldImpl::DrainResults) per frame. Lower to smooth proxy-creation/scene-mutation cost more evenly ")
	TEXT("across frames. Was a compile-time constant of 8 (docs/m1-plan.md Stage 2); measured 2026-07-20 ")
	TEXT("(-VoxelPerfRun hitch-attribution log) that EVERY hitch frame had this budget pinned at its cap, with the ")
	TEXT("actual cost surfacing later as game-thread time OUTSIDE the subsystem tick (render-thread scene-mutation ")
	TEXT("backlog) rather than in DrainResults itself -- tightened to 3 to spread that load thinner (steady-state ")
	TEXT("hitches ~27->0-1 per 60s run at this value; see the M1 gate row in docs/status.md for the full trade)."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelStreamMaxRemeshesPerFrame(
	TEXT("voxel.Stream.MaxRemeshesPerFrame"),
	2,
	TEXT("Hitch isolation: max game-thread overlay-aware edit re-meshes (FVoxelWorldImpl::DrainGameThreadMesh) per ")
	TEXT("frame. Was a compile-time constant of 4; tightened to 2 alongside MaxAppliesPerFrame (see its comment)."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelStreamMaxUnloadsPerFrame(
	TEXT("voxel.Stream.MaxUnloadsPerFrame"),
	2,
	TEXT("Hitch isolation: max chunk-component unload events (FVoxelWorldImpl::DrainUnloads -- pool-park, or ")
	TEXT("DestroyComponent once the pool is at voxel.Stream.ComponentPoolMax) per frame. Was a compile-time constant ")
	TEXT("of 4; tightened to 2 alongside MaxAppliesPerFrame (see its comment)."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelStreamComponentPoolMax(
	TEXT("voxel.Stream.ComponentPoolMax"),
	512,
	TEXT("M1 hitch-gap wave (docs/status.md M1 gate row): max UVoxelChunkComponent instances parked (hidden, quads ")
	TEXT("cleared, no scene proxy, still registered/attached) in the reuse pool after leaving the desired set, ")
	TEXT("instead of DestroyComponent()'d immediately. A pending first-load prefers a pooled component -- see ")
	TEXT("FVoxelWorldImpl::AcquireChunkComponent -- over NewObject+RegisterComponent, avoiding the render-thread ")
	TEXT("AddPrimitive/RemovePrimitive churn the hitch-attribution instrumentation pinned as the dominant cost at ")
	TEXT("ring-boundary crossings. Unloads past the cap fall back to DestroyComponent() (no unbounded growth)."),
	ECVF_Default);
} // namespace

int32 VoxelDebug::GetDebugMode()
{
	return CVarVoxelDebug.GetValueOnAnyThread();
}

void VoxelDebug::SetDebugMode(int32 NewMode)
{
	CVarVoxelDebug->Set(FMath::Clamp(NewMode, 0, 2), ECVF_SetByCode);
}

void VoxelDebug::CycleDebugMode()
{
	SetDebugMode((GetDebugMode() + 1) % 3);
}

bool VoxelDebug::IsChunkStatesEnabled()
{
	return GetDebugMode() >= 2 && CVarVoxelDebugChunkStates.GetValueOnAnyThread();
}

bool VoxelDebug::IsBoundsEnabled()
{
	return GetDebugMode() >= 2 && CVarVoxelDebugBounds.GetValueOnAnyThread();
}

bool VoxelDebug::IsRingsEnabled()
{
	return GetDebugMode() >= 2 && CVarVoxelDebugRings.GetValueOnAnyThread();
}

void VoxelDebug::SetRingsEnabled(bool bEnabled)
{
	CVarVoxelDebugRings->Set(bEnabled, ECVF_SetByCode);
}

FLinearColor VoxelDebug::RingLevelTint(int32 Level)
{
	// m2-plan.md first implementation wave item 4: "R0 green, R1 yellow, R2
	// orange, R3 red, R4 magenta."
	static const FLinearColor kTints[VoxelCoords::kNumLevels] = {
		FLinearColor(0.1f, 0.9f, 0.15f, 1.0f),  // R0 green
		FLinearColor(0.95f, 0.9f, 0.1f, 1.0f),  // R1 yellow
		FLinearColor(1.0f, 0.55f, 0.05f, 1.0f), // R2 orange
		FLinearColor(0.9f, 0.1f, 0.1f, 1.0f),   // R3 red
		FLinearColor(0.85f, 0.1f, 0.85f, 1.0f), // R4 magenta
	};
	return kTints[FMath::Clamp(Level, 0, VoxelCoords::kNumLevels - 1)];
}

FLinearColor VoxelDebug::HeightmapBandTint()
{
	// m2-plan.md "Debug" row / debug-tooling-plan.md palette: "heightmap
	// band cyan".
	return FLinearColor(0.1f, 0.85f, 0.95f, 1.0f);
}

int64 VoxelDebug::GetMipCacheBudgetBytes()
{
	return int64(CVarVoxelMipCacheBudgetMB.GetValueOnAnyThread()) * 1024 * 1024;
}

void VoxelDebug::SetMipCacheBudgetMB(int32 NewBudgetMB)
{
	CVarVoxelMipCacheBudgetMB->Set(NewBudgetMB, ECVF_SetByCode);
}

int32 VoxelDebug::GetServerMaxIntentsPerSec()
{
	return CVarVoxelServerMaxIntentsPerSec.GetValueOnGameThread();
}

float VoxelDebug::GetServerMaxCarveRadiusUU()
{
	return CVarVoxelServerMaxCarveRadiusUU.GetValueOnGameThread();
}

int32 VoxelDebug::GetWaterMaxActiveBricks()
{
	return CVarVoxelWaterMaxActiveBricks.GetValueOnAnyThread();
}

int32 VoxelDebug::GetStreamMaxAppliesPerFrame()
{
	return FMath::Max(1, CVarVoxelStreamMaxAppliesPerFrame.GetValueOnGameThread());
}

int32 VoxelDebug::GetStreamMaxRemeshesPerFrame()
{
	return FMath::Max(1, CVarVoxelStreamMaxRemeshesPerFrame.GetValueOnGameThread());
}

int32 VoxelDebug::GetStreamMaxUnloadsPerFrame()
{
	return FMath::Max(1, CVarVoxelStreamMaxUnloadsPerFrame.GetValueOnGameThread());
}

int32 VoxelDebug::GetComponentPoolMax()
{
	return FMath::Max(0, CVarVoxelStreamComponentPoolMax.GetValueOnGameThread());
}
