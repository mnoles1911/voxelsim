#pragma once
#include "CoreMinimal.h"
#include "VoxelDetachedPersistence.h"
#include "VoxelCheckpointStore.h"
namespace VoxelSaveJobs
{
struct FSnapshot
{
    FString TerrainPath;
    TArray<uint8> Terrain,Detached;
    VoxelDetachedPersistence::FSnapshot Objects;
    bool bObjectSnapshot=false;
    FString MetadataJson;
    VoxelCheckpointStore::FSimulationPayload Simulation;
    double CaptureMs=0;
};
// Game-thread admission and completion. One active job bounds snapshot memory.
bool IsBusy();
// One deferred manual request; capture happens after the active write finishes.
bool Defer(TFunction<void()> Request);
bool Submit(FSnapshot&& Snapshot,TFunction<void(bool)> Completion);
// Synchronous writers and world teardown must drain before publishing new data.
void Drain();
}
