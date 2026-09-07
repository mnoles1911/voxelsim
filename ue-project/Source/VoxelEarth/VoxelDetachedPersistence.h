#pragma once
#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "VoxelObjectRegistry.h"
class AActor;
class UWorld;
namespace VoxelDetachedPersistence
{
using FSnapshot = TArray<VoxelObjects::FEntry>;
bool CaptureObject(VoxelObjects::FEntry& Entry);
AActor* RestoreObject(UWorld* World,const VoxelObjects::FEntry& Entry);
// Completion sees a hidden ready actor (or nullptr on failure). Return true only
// after publication/binding; rejected actors are destroyed. All callbacks are GT.
uint64 BeginRestoreObject(UWorld* World,const VoxelObjects::FEntry& Entry,TFunction<bool(AActor*)> Completion,TFunction<bool()> StillWanted = {});
void CancelRestoreObject(uint64 Handle);
bool PublishRestoredObject(AActor* Actor,const VoxelObjects::FEntry& Current);
bool CaptureSnapshot(UWorld* World,FSnapshot& Snapshot);
bool EncodeSnapshot(const FSnapshot& Snapshot,TArray<uint8>& Payload);
bool DecodeSnapshot(const TArray<uint8>& Payload,FSnapshot& Snapshot);
bool InstallSnapshot(UWorld* World,FSnapshot&& Snapshot);
void RefreshObjects(UWorld* World);
// Sidecar is addressed by the exact serialized terrain bytes. Publish it before
// atomically replacing the terrain log, preserving the previous matching pair.
bool Save(UWorld* World, const FString& TerrainPath, const TArray<uint8>& TerrainBytes);
// Capture touches actors and must run on the game thread. WritePayload consumes
// only immutable bytes and is safe on the save worker. Empty means no sidecar.
bool CapturePayload(UWorld* World,TArray<uint8>& Payload);
bool WritePayload(const FString& TerrainPath,const TArray<uint8>& TerrainBytes,const TArray<uint8>& Payload);
bool Load(UWorld* World, const FString& TerrainPath, const TArray<uint8>& TerrainBytes);
bool LoadAsync(UWorld* World,const FString& TerrainPath,const TArray<uint8>& TerrainBytes);
bool IsLoading(UWorld* World);
bool HasLoadFailure(UWorld* World);
void DrainLoads(UWorld* World);
void OnEndPlay(AActor* Actor, EEndPlayReason::Type Reason);
bool HasRestoredPrototypes(UWorld* World);
bool Bytes(FArchive& Ar, TArray<uint8>& Data, int32 Max=64*1024*1024);
template<class T, class F> bool Array(FArchive& Ar,TArray<T>& Values,int32 Max,F Serialize)
{
    int32 Count=Values.Num(); Ar<<Count;
    if(Ar.IsError()||Count<0||Count>Max){Ar.SetError();return false;}
    if(Ar.IsLoading())Values.SetNum(Count);
    for(auto& Value:Values){Serialize(Ar,Value);if(Ar.IsError())return false;}
    return true;
}
}
