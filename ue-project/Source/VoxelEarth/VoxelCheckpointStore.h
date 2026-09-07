#pragma once
#include "CoreMinimal.h"

// Storage transaction only. Callers must supply a consistent authoritative capture.
namespace VoxelCheckpointStore
{
// Immutable bytes captured together on the authority thread. These three domains
// are all-or-nothing; the store never inherits simulation data from an older save.
struct FSimulationPayload
{
    TArray<uint8> Water, Hydrology, Clock, Gameplay;
};
struct FResolved
{
    FString TerrainPath;
    FString MetadataPath;
    FString WaterPath, HydrologyPath, ClockPath, GameplayPath;
    bool bSimulation = false;
    bool bCheckpoint = false;
    bool bRecovered = false;
};
// Legacy fallback is allowed only when no committed generation exists.
VOXELEARTH_API bool Resolve(const FString& LogicalTerrainPath, FResolved& Out);
// Empty metadata preserves the previous metadata (e.g. a shutdown world save).
// Each successful call publishes a new immutable generation; never modifies legacy files.
VOXELEARTH_API bool Commit(const FString& LogicalTerrainPath, const TArray<uint8>& Terrain,
    const TArray<uint8>& Detached, const FString& MetadataJson = FString(), int32 FailAfterStage = -1,
    const FSimulationPayload* Simulation = nullptr);
}
