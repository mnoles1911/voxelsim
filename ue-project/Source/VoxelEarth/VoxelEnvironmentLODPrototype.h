#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelObjectGeometrySnapshot.h"
#include "VoxelEnvironmentAsset.h"
#include "VoxelEnvironmentLODPrototype.generated.h"
class UStaticMeshComponent;
class UProceduralMeshComponent;
class UMaterialInstanceDynamic;
struct FEnvironmentLODState;
struct FEnvironmentStagedRestore;
struct FVoxelEnvironmentProductionCommit;
using FVoxelEnvironmentProductionCommitRef=TSharedPtr<const FVoxelEnvironmentProductionCommit,ESPMode::ThreadSafe>;

// Shared editable environment actor. The legacy class name and four-fixture
// launcher remain for compatibility; source identity and grids are generic.
UCLASS()
class VOXELEARTH_API AVoxelEnvironmentLODPrototype : public AActor
{
    GENERATED_BODY()
public:
    AVoxelEnvironmentLODPrototype();
    virtual ~AVoxelEnvironmentLODPrototype() override;
    AVoxelEnvironmentLODPrototype(FVTableHelper& Helper);
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    bool PersistentState(FArchive& Ar);
    bool CaptureObjectState(FVoxelImmutableGeometry& Geometry,TArray<uint8>& Dynamic);
    bool RestoreObjectState(const TArray<uint8>& Geometry,const TArray<uint8>& Dynamic);
    void BeginStagedObjectRestore(FVoxelImmutableGeometry Geometry,TArray<uint8> Dynamic,TFunction<void(bool)> Completion);
    // Success callback waits asynchronously for queued render-resource commands.
    // Advance never waits/flushes the rendering thread; actor stays hidden.
    bool AdvanceStagedObjectRestore();
    bool IsStagedRenderResourcesPending() const;
    bool PublishStagedObjectRestore();
    // Prepare/validate while hidden. Commit is a non-yielding GT preconditioned
    // operation: it only makes state authoritative, and never reveals the actor.
    // A future renderer transaction must latch visibility at its own boundary.
    FVoxelEnvironmentProductionCommitRef PrepareProductionCommit(FVoxelImmutableGeometry Geometry,const TArray<uint8>& Dynamic,const FTransform& Transform);
    bool ValidateProductionCommit(const FVoxelEnvironmentProductionCommitRef& Token);
    void CommitPreparedProduction(const FVoxelEnvironmentProductionCommitRef& Token);

    // Explicit production preparation status, independent of rendering visibility.
    // Retained on cancellation; only publication clears it. Persistence discovery
    // must not bind actors that are not yet authoritative world objects.
    void MarkUnpublishedPreparation();
    bool IsUnpublishedPreparation() const { return bUnpublishedPreparation; }
    void CancelStagedObjectRestore();
    bool RestoreObjectState(const TSharedPtr<const TArray<uint8>,ESPMode::ThreadSafe>& Geometry,const TArray<uint8>& Dynamic) {
        if(!Geometry||!RestoreObjectState(*Geometry,Dynamic))return false;
        GeometrySnapshot=Geometry;return true;
    }
    bool InitializeAsset(const FString& Name, int32 FinestMm, bool Collision);
    bool InitializeAssetFromVxa(FVoxelEnvironmentAssetDescriptor Descriptor,const TArray<uint8>& Vxa,bool Collision,bool FitTerrain=false);
    const FVoxelEnvironmentAssetDescriptor& GetAssetDescriptor() const {return SourceDescriptor;}
    bool IsFellable() const {return SourceDescriptor.Fellable;}
    bool SolidAt(const FVector& WorldUU) const;
    bool Trace(const FVector& Start, const FVector& Direction, double Range, FVector& Hit) const;
    bool GetDigBounds(const FVector& WorldHit,int32 SizeVoxels,FBox& Bounds) const;
    bool Carve(const FVector& Hit, int32 SizeVoxels);
    bool Chop(const FVector& Hit,const FVector& Direction,int32 SizeVoxels);
    bool CanChop(const FVector& Hit) const;
    void Rebuild();
    void SetTestLOD(int32 Level) { ForcedLOD = Level; }
    int32 CurrentLOD() const { return ActiveLOD; }
    int32 LevelCount() const { return Levels.Num(); }
    FString AssetName;
    UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> Levels;
    UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;
private:
    bool bUnpublishedPreparation=false;
    FGuid ProductionCommitSerial;
    FVoxelEnvironmentAssetDescriptor SourceDescriptor;
    bool RefreshGeometrySnapshot();
    bool CaptureStateForCommit(FVoxelImmutableGeometry& Geometry,TArray<uint8>& Dynamic,bool AllowPrepared);
    FVoxelImmutableGeometry GeometrySnapshot;
    TSharedPtr<FEnvironmentStagedRestore,ESPMode::ThreadSafe> StagedRestore;
    TSharedPtr<FEnvironmentStagedRestore,ESPMode::ThreadSafe> PreparedRestore;
    void RebuildSections(int32 Level, const FIntVector& Min, const FIntVector& Max);
    void SetLevelVisible(int32 Level, bool Visible);
    void DetachAbove(int32 CutLayer,const FVector& Direction);
    UPROPERTY(Transient) TArray<TObjectPtr<UProceduralMeshComponent>> Sections;
    TUniquePtr<FEnvironmentLODState> State;
    int32 ActiveLOD=0, PreviousLOD=-1, ForcedLOD=-1;
    double FadeTime=0;
};
namespace VoxelEnvironmentLODPrototype
{
    void StartFromCommandLine(UWorld* World);
    bool IsSolid(UWorld* World, const FVector& Position);
    bool GetDigPreview(UWorld* World, const FVector& Start, const FVector& Direction, int32 SizeVoxels, FBox& Bounds);
    bool TryDig(UWorld* World, const FVector& Start, const FVector& Direction, int32 SizeVoxels);
}
