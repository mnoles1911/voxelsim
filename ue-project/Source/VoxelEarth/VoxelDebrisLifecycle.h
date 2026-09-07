#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "VoxelDebrisLifecycle.generated.h"

UENUM(BlueprintType)
enum class EVoxelDebrisLifetime : uint8 { Cosmetic, Harvestable, Substantial, Retained };

// Remaining gameplay time, never an absolute world timestamp. Owners must save
// this alongside their material/geometry/transform when actor persistence exists.
USTRUCT(BlueprintType)
struct FVoxelDebrisLifetimeState
{
    GENERATED_BODY()
    UPROPERTY(SaveGame) EVoxelDebrisLifetime Kind = EVoxelDebrisLifetime::Substantial;
    UPROPERTY(SaveGame) double RemainingSeconds = 900.;
};

UCLASS(ClassGroup=(Voxel), meta=(BlueprintSpawnableComponent))
class VOXELEARTH_API UVoxelDebrisLifecycle : public UActorComponent
{
    GENERATED_BODY()
public:
    UVoxelDebrisLifecycle();
    void Configure(EVoxelDebrisLifetime Kind);
    UFUNCTION(BlueprintCallable) void NotifyInteraction();
    UFUNCTION(BlueprintCallable) void Retain();
    FVoxelDebrisLifetimeState CaptureState() const;
    void RestoreState(const FVoxelDebrisLifetimeState& Saved);
    // Pure policy seam used by automation tests as well as the runtime timer.
    static bool Advance(FVoxelDebrisLifetimeState& Value, double Seconds, bool Protected);
    static bool ProtectsBounds(const FBox& Bounds, const FVector& Eye, const FVector& Forward);
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    void Arm();
    void Poll();
    void Accrue();
    bool IsProtected() const;
    UPROPERTY(SaveGame) FVoxelDebrisLifetimeState State;
    FTimerHandle CleanupTimer;
    double LastSample = 0.;
};
