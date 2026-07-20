#include "VoxelDebug.h"

#include "HAL/IConsoleManager.h"
#include "Math/UnrealMathUtility.h"

DEFINE_LOG_CATEGORY(LogVoxelStream);
DEFINE_LOG_CATEGORY(LogVoxelEdit);
DEFINE_LOG_CATEGORY(LogVoxelPerf);

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
