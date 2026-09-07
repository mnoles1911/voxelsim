#pragma once
#include "CoreMinimal.h"
#include "VoxelDetachedPersistence.h"
namespace VoxelSaveJobs
{
struct FSnapshot
{
    FString TerrainPath;
    TArray<uint8> Terrain,Detached;
    VoxelDetachedPersistence::FSnapshot Objects;
    bool bObjectSnapshot=false;
    FString MetadataJson;
    double CaptureMs=0;
};
// Game-thread admission and completion. One active job bounds snapshot memory.
bool IsBusy();
bool Submit(FSnapshot&& Snapshot,TFunction<void(bool)> Completion);
// Synchronous writers and world teardown must drain before publishing new data.
void Drain();
}
