#pragma once
#include "CoreMinimal.h"
#include "VoxelCheckpointStore.h"
class UWorld;

namespace VoxelSessionCheckpoint
{
VOXELEARTH_API bool Ready(UWorld* World);
VOXELEARTH_API bool Failed(UWorld* World);
VOXELEARTH_API FGuid WorldId(UWorld* World);
VOXELEARTH_API void Fail(UWorld* World);
VOXELEARTH_API bool Capture(UWorld* World, VoxelCheckpointStore::FSimulationPayload& Out);
// Called after terrain replay and before simulation or pawn admission.
VOXELEARTH_API bool Restore(UWorld* World, const FString& LogicalPath,
    const VoxelCheckpointStore::FResolved* Checkpoint);
VOXELEARTH_API void SaveBeforeTearDown(UWorld* World);
VOXELEARTH_API bool FinalSaveAttempted(UWorld* World);
VOXELEARTH_API void NotifySaved(const UWorld* World);
// End-of-world-tick checkpoint boundary; exposed for focused scheduling tests.
VOXELEARTH_API void TickAutosave(UWorld* World,float GameplaySeconds);
}
