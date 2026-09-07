#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelObjectGeometrySnapshot.h"
#include "VoxelTreeFellingPrototype.generated.h"
class UBoxComponent;
class UProceduralMeshComponent;
class UPhysicsConstraintComponent;
class UVoxelDebrisLifecycle;
struct FTimberStagedRestore;

// Experimental rigid timber bodies. Terrain contact uses a bounded proxy;
// neither this proxy nor the bodies replace production terrain collision.
UCLASS()
class VOXELEARTH_API AVoxelFallingTimber : public AActor
{
    GENERATED_BODY()
public:
    AVoxelFallingTimber();
    void Initialize(const TArray<UProceduralMeshComponent*>& Meshes, const FBox& LocalTrunk,
                    const FTransform& Source, const FVector& PushDirection, bool CanBreak);
    FBox GetGroundSupportBounds() const;
    void PauseForGroundSupport();
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    bool PersistentState(FArchive& Ar);
    bool CaptureObjectState(FVoxelImmutableGeometry& Geometry,TArray<uint8>& Dynamic);
    bool RestoreObjectState(const TArray<uint8>& Geometry,const TArray<uint8>& Dynamic);
    void BeginStagedObjectRestore(FVoxelImmutableGeometry Geometry,TArray<uint8> Dynamic,TFunction<void(bool)> Completion);
    bool AdvanceStagedObjectRestore();
    bool PublishStagedObjectRestore();
    void CancelStagedObjectRestore();
    bool RestoreObjectState(const TSharedPtr<const TArray<uint8>,ESPMode::ThreadSafe>& Geometry,const TArray<uint8>& Dynamic) {
        if(!Geometry||!RestoreObjectState(*Geometry,Dynamic))return false;
        GeometrySnapshot=Geometry;return true;
    }
    UPROPERTY(Transient) TObjectPtr<UBoxComponent> Body;
private:
    bool SerializePersistentState(FArchive& Ar,bool GeometryOnly);
    bool RefreshGeometrySnapshot();
    FVoxelImmutableGeometry GeometrySnapshot;
    TSharedPtr<FTimberStagedRestore,ESPMode::ThreadSafe> StagedRestore;
    TSharedPtr<FTimberStagedRestore,ESPMode::ThreadSafe> PreparedRestore;
    bool DeferringGeometrySnapshot=false;
    bool GroundPaused=false,GroundWasAwake=false;
    FVector GroundLinear=FVector::ZeroVector,GroundAngular=FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UVoxelDebrisLifecycle> Cleanup;
    UFUNCTION() void Contact(UPrimitiveComponent* HitComponent,AActor* OtherActor,UPrimitiveComponent* OtherComp,
                            FVector Impulse,const FHitResult& Hit);
    void BreakOnImpact();
    UPROPERTY(Transient) TArray<TObjectPtr<UProceduralMeshComponent>> Visuals;
    UPROPERTY(Transient) TObjectPtr<UPhysicsConstraintComponent> StumpHinge;
    FBox Trunk=FBox(ForceInit);
    FVector LastLinear=FVector::ZeroVector,LastAngular=FVector::ZeroVector,LastCenter=FVector::ZeroVector;
    FVector FallHeading=FVector::ForwardVector;
    bool ReleaseHinge=false;
    double PeakImpactJ=0;
    bool Breakable=false,PendingBreak=false,ReportedRest=false;
    double Age=0,PreviousSpeed=0;
};

UCLASS()
class VOXELEARTH_API AVoxelStoneAxePrototype : public AActor
{
    GENERATED_BODY()
public:
    AVoxelStoneAxePrototype();
    bool Initialize();
    bool Swing();
    virtual void Tick(float DeltaSeconds) override;
    UPROPERTY(Transient) TObjectPtr<UProceduralMeshComponent> Mesh;
    bool Equipped=true;
private:
    double SwingAge=10;
    bool Struck=false;
};

namespace VoxelTreeFelling {
    bool IsEquipped(UWorld* World);
    bool GetPreview(UWorld* World,const FVector& Start,const FVector& Direction,int32 Size,FBox& Bounds);
    bool TrySwing(UWorld* World);
    bool Chop(UWorld* World,const FVector& Start,const FVector& Direction,int32 Size);
    void Prepare(UWorld* World,const FVector& TreeLocation);
    void PrepareGround(UWorld* World,const FVector& TreeLocation);
    void EnsureAxe(UWorld* World);
    bool AdvanceRestoreGround(UWorld* World,const FVector& Location);
    bool AdvanceRestoreGround(UWorld* World,const FBox& Bounds);
    void NotifyTerrainEdited(UWorld* World,const FBox& Bounds);
    void Reset(UWorld* World);
}
