#pragma once
#include "CoreMinimal.h"
#include "VoxelInventoryComponent.h"
#include "VoxelPlayerMotion.h"
class UWorld;
class AVoxelEarthPlayerController;

namespace VoxelPlayerRecords
{
struct FRecord
{
    FGuid Id,Vehicle;
    bool Host=false,HasPawn=false;
    FString CredentialHash;
    TArray<FVoxelInventorySlot> Slots;
    int32 Selected=0;
    FVector Position=FVector::ZeroVector,Velocity=FVector::ZeroVector;
    FRotator Rotation=FRotator::ZeroRotator;
    FVoxelPlayerMotion Motion;
};
VOXELEARTH_API bool IsBound(const AVoxelEarthPlayerController* Player);
VOXELEARTH_API FGuid PlayerId(const AVoxelEarthPlayerController* Player);
VOXELEARTH_API int32 Credit(UWorld* World,const FGuid& Player,FName Item,int32 Count);
VOXELEARTH_API bool BindHost(AVoxelEarthPlayerController* Player);
VOXELEARTH_API bool ApplyPawn(AVoxelEarthPlayerController* Player);
VOXELEARTH_API void Disconnect(AVoxelEarthPlayerController* Player);
VOXELEARTH_API bool Capture(UWorld* World,TArray<uint8>& Bytes);
VOXELEARTH_API bool Decode(const TArray<uint8>& Bytes,TArray<FRecord>& Records);
VOXELEARTH_API bool Restore(UWorld* World,const TArray<uint8>& Bytes);
// Remote credentials are accepted only on an encrypted engine connection.
VOXELEARTH_API bool Authenticate(AVoxelEarthPlayerController* Player,const FString& Token,FGuid& Id,FString& IssuedToken);
VOXELEARTH_API bool Confirm(AVoxelEarthPlayerController* Player,const FGuid& Id);
VOXELEARTH_API bool ReadCredential(AVoxelEarthPlayerController* Player,const FGuid& WorldId,FString& Token);
VOXELEARTH_API bool WriteCredential(AVoxelEarthPlayerController* Player,const FGuid& WorldId,const FString& Token);
}
