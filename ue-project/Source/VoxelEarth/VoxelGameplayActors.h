#pragma once
#include "CoreMinimal.h"
class UWorld;
class APawn;
class AActor;
class AVoxelEarthPlayerController;

namespace VoxelGameplayActors
{
enum class EKind : uint8 { Boat=1, Glider=2, Item=3, Explosive=4 };
struct FRecord
{
    FGuid Id,Owner;
    EKind Kind=EKind::Boat;
    FTransform Transform=FTransform::Identity;
    FVector Velocity=FVector::ZeroVector,AngularVelocity=FVector::ZeroVector;
    FString AssetName,AssetPath,AssetHash;
    FName Item;
    int32 Count=0;
    bool Resting=false,Grounded=false,LaunchPending=false;
    double Remaining=0,FlightAge=0,SettledAge=0,Throttle=0,Radius=0;
};
VOXELEARTH_API bool Encode(const TArray<FRecord>& Records,TArray<uint8>& Bytes);
VOXELEARTH_API bool Decode(const TArray<uint8>& Bytes,TArray<FRecord>& Records);
VOXELEARTH_API bool ValidateContent(const TArray<FRecord>& Records);
VOXELEARTH_API bool Capture(UWorld* World,TArray<uint8>& Bytes);
VOXELEARTH_API bool Restore(UWorld* World,const TArray<uint8>& Bytes);
VOXELEARTH_API APawn* PlayerPawn(AVoxelEarthPlayerController* Controller);
VOXELEARTH_API FGuid VehicleId(AVoxelEarthPlayerController* Controller);
VOXELEARTH_API bool BindVehicle(AVoxelEarthPlayerController* Controller,const FGuid& Id);
VOXELEARTH_API bool ReleaseVehicle(AVoxelEarthPlayerController* Controller);
VOXELEARTH_API void FinishRestore(AActor* Actor);
}
