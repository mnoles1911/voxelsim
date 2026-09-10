#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelSessionNetworkProbe.generated.h"

UCLASS()
class VOXELEARTH_API UVoxelSessionNetworkProbe : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual bool IsTickable() const override { return bEnabled; }
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelSessionNetworkProbe,STATGROUP_Tickables); }
private:
    FString Role,Directory;
    int32 ExpectedPlayers=2;
    bool bEnabled=false,bReported=false;
    double Elapsed=0,NextPoll=0;
};
