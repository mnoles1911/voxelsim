#pragma once
#include "CoreMinimal.h"
#include "VoxelDebrisLifecycle.h"

class AActor;
class UWorld;

// Game-thread registry. Snapshot byte handles alone may cross to workers.
namespace VoxelObjects
{
using FGeometry = TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>;
enum class EResidency : uint8 { Live, Dormant, Restoring, Evicting, Tombstone };
struct FGeometryPage
{
    FString Path, Hash;
    int32 Bytes = 0;
    uint32 Crc = 0;
    bool IsValid() const { return !Path.IsEmpty() && Hash.Len()==32 && Bytes>0 && Bytes<=512*1024*1024; }
};
struct FEntry
{
    FGuid Id;
    uint64 Revision = 1, GeometryRevision = 0;
    uint8 Kind = 0;
    TWeakObjectPtr<AActor> Actor;
    FTransform Transform;
    FVector Velocity = FVector::ZeroVector, AngularVelocity = FVector::ZeroVector;
    FVector BoundsExtent = FVector::ZeroVector;
    FVoxelDebrisLifetimeState Lifetime;
    FGuid OwnerId;
    bool bRetained = false, bActivePhysics = false, bPinned = false;
    EResidency Residency = EResidency::Dormant;
    FGeometry Geometry;
    FGeometryPage Page;
    TArray<uint8> Dynamic;
    uint32 GeometryFormat = 1;
};
struct FView { FVector Position = FVector::ZeroVector, Forward = FVector::ForwardVector; };
struct FSettings
{
    double LoadDistanceCm = 16000., UnloadDistanceCm = 20000., RegionSizeCm = 25600.;
    int32 MaxTransitions = 2;
    // Bound candidate work as well as costly actor transitions.
    int32 MaxInspections = 128;
};
struct FCallbacks
{
    TFunction<bool(FEntry&)> Capture;
    TFunction<AActor*(const FEntry&)> Restore;
    TFunction<bool(const FEntry&)> BeginRestore;
    TFunction<void(AActor*)> Evict;
};
struct FTickResult { int32 Inspected = 0, Restored = 0, Evicted = 0, Expired = 0; };
class FRegistry
{
public:
    FGuid Bind(AActor* Actor, uint8 Kind, FGuid Id = FGuid());
    FEntry* Find(const FGuid& Id);
    const FEntry* Find(const FGuid& Id) const;
    FEntry* Find(const AActor* Actor);
    // Incoming entries need strictly newer revisions. Tombstones are permanent
    // within this world session; even a larger stale restore cannot resurrect.
    bool Import(FEntry Entry);
    bool Update(const FGuid& Id, TFunctionRef<void(FEntry&)> Mutate);
    bool PublishGeometry(const FGuid& Id, TArray<uint8>&& Bytes);
    bool Remove(const FGuid& Id);
    TArray<FEntry> Snapshot(bool IncludeTombstones = true) const;
    TArray<FEntry> SnapshotSlice(int32& ScanCursor, int32 Maximum = 128) const;
    bool CompleteRestore(const FGuid& Id, uint64 GeometryRevision, AActor* Actor);
    void CancelRestore(const FGuid& Id);
    TMap<FIntVector,TArray<FEntry>> SnapshotRegions(double RegionSizeCm = 25600., bool IncludeTombstones = true) const;
    FTickResult Tick(const TArray<FView>& Views, double DeltaSeconds, const FCallbacks& Callbacks, const FSettings& Settings = FSettings());
    static FIntVector RegionFor(const FVector& Position, double RegionSizeCm = 25600.);
    static bool ShouldLoad(const FEntry& Entry, const TArray<FView>& Views, double DistanceCm);
    static bool ShouldEvict(const FEntry& Entry, const TArray<FView>& Views, double DistanceCm);
    int32 Num() const { return Entries.Num(); }
private:
    TMap<FGuid,FEntry> Entries;
    TArray<FGuid> Order;
    TMap<FGuid,double> LastDormantSample;
    double Clock = 0.;
    int32 Cursor = 0;
};
FRegistry& Get(UWorld* World);
FRegistry* Find(UWorld* World);
void Forget(UWorld* World);
}
