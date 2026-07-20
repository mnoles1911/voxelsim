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

TAutoConsoleVariable<bool> CVarVoxelDebugRings(
	TEXT("voxel.Debug.Rings"),
	false,
	TEXT("Tint loaded chunks by mip ring level (R0 green .. R4 magenta) instead of chunk-state tints. Live under voxel.Debug 2."),
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
