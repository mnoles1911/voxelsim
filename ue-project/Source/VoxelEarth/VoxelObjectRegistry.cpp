#include "VoxelObjectRegistry.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

namespace VoxelObjects
{
namespace {
TMap<TWeakObjectPtr<UWorld>,TUniquePtr<FRegistry>> Worlds;
struct FWorldRegistryCleanup
{
    FDelegateHandle Handle;
    FWorldRegistryCleanup(){Handle=FWorldDelegates::OnPreWorldFinishDestroy.AddStatic(&Forget);}
    ~FWorldRegistryCleanup(){FWorldDelegates::OnPreWorldFinishDestroy.Remove(Handle);}
};
// CleanupWorld delegates precede subsystem Deinitialize (shutdown saving).
// FinishDestroy is later, after that save no longer needs immutable records.
FWorldRegistryCleanup WorldRegistryCleanup;
}
FRegistry& Get(UWorld* W)
{
    check(IsInGameThread()); check(W);
    for(auto It=Worlds.CreateIterator();It;++It)if(!It.Key().HasSameIndexAndSerialNumber(TWeakObjectPtr<UWorld>(W))&&!It.Key().IsValid())It.RemoveCurrent();
    auto& Registry=Worlds.FindOrAdd(W); if(!Registry)Registry=MakeUnique<FRegistry>(); return *Registry;
}
FRegistry* Find(UWorld* W) { auto P=Worlds.Find(W);return P?P->Get():nullptr; }
void Forget(UWorld* W) { check(IsInGameThread());Worlds.Remove(W); }
FEntry* FRegistry::Find(const FGuid& Id) { return Entries.Find(Id); }
const FEntry* FRegistry::Find(const FGuid& Id) const { return Entries.Find(Id); }
FEntry* FRegistry::Find(const AActor* A)
{
    if(!A)return nullptr;
    // EndPlay can run after pending-kill makes Get() null. Ordinary weak ==
    // also considers two invalid pointers equal, which would pick another ID.
    const TWeakObjectPtr<const AActor> Identity(A);
    for(auto& Pair:Entries)if(Pair.Value.Actor.HasSameIndexAndSerialNumber(Identity))return &Pair.Value;
    return nullptr;
}
FGuid FRegistry::Bind(AActor* A,uint8 Kind,FGuid Id)
{
    check(IsInGameThread());
    if(!IsValid(A)||Kind<1||Kind>3)return FGuid();
    if(auto Existing=Find(A))return Existing->Kind==Kind&&(!Id.IsValid()||Id==Existing->Id)?Existing->Id:FGuid();
    if(!Id.IsValid())Id=FGuid::NewGuid();
    if(auto E=Find(Id)) {
        if(E->Kind!=Kind||E->Residency==EResidency::Tombstone||(E->Actor.IsValid()&&E->Actor.Get()!=A))return FGuid();
        E->Actor=A;E->Residency=EResidency::Live;E->Transform=A->GetActorTransform();++E->Revision;return Id;
    }
    FEntry E;E.Id=Id;E.Kind=Kind;E.Actor=A;E.Transform=A->GetActorTransform();E.Residency=EResidency::Live;
    Entries.Add(Id,MoveTemp(E));Order.Add(Id);return Id;
}
bool FRegistry::Import(FEntry E)
{
    check(IsInGameThread());
    if(!E.Id.IsValid()||!E.Revision||E.Kind<1||E.Kind>3||E.Transform.ContainsNaN()||!E.Transform.IsValid()||
       E.Velocity.ContainsNaN()||E.AngularVelocity.ContainsNaN()||E.BoundsExtent.ContainsNaN()||
       !FMath::IsFinite(E.Lifetime.RemainingSeconds)||E.Lifetime.RemainingSeconds<0.||
       uint8(E.Lifetime.Kind)>uint8(EVoxelDebrisLifetime::Retained))return false;
    auto Old=Find(E.Id);
    if(E.Actor.IsValid())if(auto Bound=Find(E.Actor.Get()))if(Bound->Id!=E.Id)return false;
    if(Old&&(Old->Residency==EResidency::Tombstone||Old->Revision>=E.Revision||
             Old->Kind!=E.Kind||Old->GeometryRevision>E.GeometryRevision))return false;
    if(!Old)Order.Add(E.Id);
    const FGuid Id=E.Id;
    if(E.Residency==EResidency::Tombstone){E.Actor.Reset();E.Geometry.Reset();E.Page={};E.Dynamic.Empty();}
    else E.Residency=E.Actor.IsValid()?EResidency::Live:EResidency::Dormant;
    Entries.Add(Id,MoveTemp(E));LastDormantSample.Add(Id,Clock);return true;
}
bool FRegistry::Update(const FGuid& Id,TFunctionRef<void(FEntry&)> Mutate)
{
    auto E=Find(Id);if(!E||E->Residency==EResidency::Tombstone)return false;
    const uint64 Revision=E->Revision;const auto Old=E->Geometry;Mutate(*E);if(Old!=E->Geometry)E->Page={};E->Id=Id;E->Revision=Revision+1;return true;
}
bool FRegistry::PublishGeometry(const FGuid& Id,TArray<uint8>&& Bytes)
{
    return Update(Id,[&](FEntry& E){E.Geometry=MakeShared<const TArray<uint8>,ESPMode::ThreadSafe>(MoveTemp(Bytes));++E.GeometryRevision;});
}
bool FRegistry::Remove(const FGuid& Id)
{
    auto E=Find(Id);if(!E||E->Residency==EResidency::Tombstone)return false;
    E->Actor.Reset();E->Geometry.Reset();E->Page={};E->Dynamic.Empty();E->Residency=EResidency::Tombstone;++E->Revision;LastDormantSample.Remove(Id);return true;
}
bool FRegistry::CompleteRestore(const FGuid& Id,uint64 GeometryRevision,AActor* A)
{
    auto E=Find(Id);if(!E||E->Residency!=EResidency::Restoring||E->GeometryRevision!=GeometryRevision||!IsValid(A))return false;
    if(auto Bound=Find(A))if(Bound->Id!=Id)return false;
    E->Actor=A;E->Residency=EResidency::Live;++E->Revision;LastDormantSample.Remove(Id);return true;
}
void FRegistry::CancelRestore(const FGuid& Id)
{
    if(auto E=Find(Id))if(E->Residency==EResidency::Restoring)E->Residency=EResidency::Dormant;
}
TArray<FEntry> FRegistry::SnapshotSlice(int32& ScanCursor,int32 Maximum) const
{
    TArray<FEntry> Out;const int32 Count=FMath::Min(Order.Num(),FMath::Max(0,Maximum));Out.Reserve(Count);
    for(int32 I=0;I<Count;++I){ScanCursor=FMath::Max(0,ScanCursor)%Order.Num();if(const auto E=Find(Order[ScanCursor++]))Out.Add(*E);}return Out;
}
TArray<FEntry> FRegistry::Snapshot(bool IncludeTombstones) const
{
    TArray<FEntry> Out;Out.Reserve(Entries.Num());
    for(const FGuid& Id:Order)if(const auto E=Find(Id))if(IncludeTombstones||E->Residency!=EResidency::Tombstone)Out.Add(*E);
    return Out;
}
TMap<FIntVector,TArray<FEntry>> FRegistry::SnapshotRegions(double Size,bool IncludeTombstones) const
{
    TMap<FIntVector,TArray<FEntry>> Out;
    for(const FGuid& Id:Order)if(const auto E=Find(Id))if(IncludeTombstones||E->Residency!=EResidency::Tombstone)
        Out.FindOrAdd(RegionFor(E->Transform.GetLocation(),Size)).Add(*E);
    return Out;
}
FIntVector FRegistry::RegionFor(const FVector& P,double Size)
{
    Size=FMath::IsFinite(Size)&&Size>0.?Size:25600.;
    return FIntVector(FMath::FloorToInt(P.X/Size),FMath::FloorToInt(P.Y/Size),FMath::FloorToInt(P.Z/Size));
}
bool FRegistry::ShouldLoad(const FEntry& E,const TArray<FView>& Views,double Distance)
{
    const FBox Bounds(E.Transform.GetLocation()-E.BoundsExtent,E.Transform.GetLocation()+E.BoundsExtent);
    for(const auto& V:Views)if(Bounds.ComputeSquaredDistanceToPoint(V.Position)<=FMath::Square(Distance))return true;
    return false;
}
bool FRegistry::ShouldEvict(const FEntry& E,const TArray<FView>& Views,double Distance)
{
    return !Views.IsEmpty()&&!E.bActivePhysics&&!E.bPinned&&!ShouldLoad(E,Views,Distance);
}
FTickResult FRegistry::Tick(const TArray<FView>& Views,double Delta,const FCallbacks& C,const FSettings& S)
{
    check(IsInGameThread());FTickResult Result;
    if(FMath::IsFinite(Delta)&&Delta>0.)Clock+=Delta;
    const int32 Inspect=FMath::Min(Order.Num(),FMath::Max(0,S.MaxInspections));
    const double Load=FMath::Max(0.,S.LoadDistanceCm),Unload=FMath::Max(Load,S.UnloadDistanceCm);
    int32 Transitions=0;
    for(int32 I=0;I<Inspect;++I){
        // Stop at the attempt budget rather than wrapping the scan to its old
        // start. Otherwise two failed restores starve every later entry when
        // the whole registry fits in one inspection pass. Dormant timers use
        // elapsed samples, so skipped entries accrue time on their next visit.
        if(S.MaxTransitions>0&&Transitions>=S.MaxTransitions)break;
        if(Order.IsEmpty())break;Cursor%=Order.Num();const FGuid Id=Order[Cursor++];++Result.Inspected;
        auto E=Find(Id);if(!E||E->Residency==EResidency::Tombstone)continue;
        if(E->Residency==EResidency::Dormant||E->Residency==EResidency::Restoring){
            double& Last=LastDormantSample.FindOrAdd(Id,Clock);const double Elapsed=Clock-Last;Last=Clock;
            bool Protected=E->bRetained;
            const FBox Bounds(E->Transform.GetLocation()-E->BoundsExtent,E->Transform.GetLocation()+E->BoundsExtent);
            for(const auto& V:Views)Protected|=UVoxelDebrisLifecycle::ProtectsBounds(Bounds,V.Position,V.Forward);
            const double Before=E->Lifetime.RemainingSeconds;
            if(!E->bRetained&&UVoxelDebrisLifecycle::Advance(E->Lifetime,Elapsed,Protected)){Remove(Id);++Result.Expired;continue;}
            if(Before!=E->Lifetime.RemainingSeconds)++E->Revision;
            if(E->Residency==EResidency::Restoring)continue;
            if(Transitions>=S.MaxTransitions||(!C.Restore&&!C.BeginRestore)||!E->Geometry||(!E->bActivePhysics&&!ShouldLoad(*E,Views,Load)))continue;
            FEntry Saved=*E;E->Residency=EResidency::Restoring;++Transitions;
            if(C.BeginRestore){if(!C.BeginRestore(Saved))CancelRestore(Id);continue;}
            AActor* A=C.Restore(Saved);E=Find(Id);
            // A callback may process removal/import; do not resurrect its stale actor.
            if(!E||E->Residency==EResidency::Tombstone||E->Revision!=Saved.Revision){if(IsValid(A)&&C.Evict)C.Evict(A);continue;}
            if(!IsValid(A)){E->Residency=EResidency::Dormant;continue;}
            E->Actor=A;E->Residency=EResidency::Live;++E->Revision;++Result.Restored;LastDormantSample.Remove(Id);
        } else if(E->Residency==EResidency::Live&&Transitions<S.MaxTransitions&&C.Capture&&C.Evict&&ShouldEvict(*E,Views,Unload)){
            FEntry Saved=*E;const uint64 Revision=E->Revision;++Transitions;
            if(!C.Capture(Saved)||!Saved.Geometry||Saved.bActivePhysics||Saved.bPinned)continue;
            E=Find(Id);if(!E||E->Revision!=Revision||E->Residency!=EResidency::Live)continue;
            AActor* A=E->Actor.Get();Saved.Residency=EResidency::Evicting;Saved.Revision=Revision+1;*E=MoveTemp(Saved);
            if(IsValid(A))C.Evict(A);E=Find(Id);
            if(E&&E->Residency==EResidency::Evicting){E->Actor.Reset();E->Residency=EResidency::Dormant;LastDormantSample.Add(Id,Clock);++Result.Evicted;}
        }
    }
    return Result;
}
}
