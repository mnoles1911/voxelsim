#include "VoxelDetachedPersistence.h"
#include "VoxelDebris.h"
#include "VoxelTreeFellingPrototype.h"
#include "VoxelEnvironmentLODPrototype.h"
#include "VoxelPackedTimberMesh.h"
#include "VoxelObjectPages.h"
#include "VoxelEarth.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "Components/PrimitiveComponent.h"
#include "Containers/Ticker.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace VoxelDetachedPersistence {
namespace {
constexpr int32 Limit=512*1024*1024;
bool IsObject(AActor* A) {
    if(!IsValid(A)||A->ActorHasTag(TEXT("ObjectRestorePending")))return false;
    // Actor discovery is independent of the prototype interaction list. A
    // hidden preparation actor must not acquire an empty registry entry that
    // poisons the next whole-world snapshot. Ordinary hidden live actors and
    // existing restoring data records remain eligible for persistence.
    if(const auto Environment=Cast<AVoxelEnvironmentLODPrototype>(A))return !Environment->IsUnpublishedPreparation();
    return A->IsA<AVoxelDebris>()||A->IsA<AVoxelFallingTimber>();
}
}
bool CaptureObject(VoxelObjects::FEntry& E) {
    check(IsInGameThread());auto A=E.Actor.Get();if(!IsObject(A))return false;
    auto Old=E.Geometry;bool Ok=false;
    if(auto D=Cast<AVoxelDebris>(A))Ok=D->CaptureObjectState(E.Geometry,E.Dynamic);
    if(auto T=Cast<AVoxelFallingTimber>(A))Ok=T->CaptureObjectState(E.Geometry,E.Dynamic);
    if(auto P=Cast<AVoxelEnvironmentLODPrototype>(A))Ok=P->CaptureObjectState(E.Geometry,E.Dynamic);
    if(!Ok)return false;
    E.GeometryFormat=1;if(Old!=E.Geometry)++E.GeometryRevision;
    E.Transform=A->GetActorTransform();FVector Center;A->GetActorBounds(false,Center,E.BoundsExtent);
    E.BoundsExtent+= (Center-E.Transform.GetLocation()).GetAbs();
    if(auto Life=A->FindComponentByClass<UVoxelDebrisLifecycle>())E.Lifetime=Life->CaptureState();
    E.bRetained=E.Lifetime.Kind==EVoxelDebrisLifetime::Retained;
    E.bActivePhysics=false;
    if(auto Body=Cast<UPrimitiveComponent>(A->GetRootComponent())){
        E.Velocity=Body->GetPhysicsLinearVelocity();E.AngularVelocity=Body->GetPhysicsAngularVelocityInRadians();
        E.bActivePhysics=Body->IsSimulatingPhysics()&&Body->IsAnyRigidBodyAwake();
    }
    return true;
}
AActor* RestoreObject(UWorld* W,const VoxelObjects::FEntry& E) {
    if(!W||!E.Geometry||E.Kind<1||E.Kind>3||(E.Kind==3&&!VoxelEnvironmentAsset::IsSupportedTransform(E.Transform)))return nullptr;
    AActor* A=E.Kind==1?static_cast<AActor*>(W->SpawnActor<AVoxelDebris>()):E.Kind==2?static_cast<AActor*>(W->SpawnActor<AVoxelFallingTimber>()):static_cast<AActor*>(W->SpawnActor<AVoxelEnvironmentLODPrototype>());
    if(!A)return nullptr;bool Ok=false;
    if(E.GeometryFormat==1){
        if(auto D=Cast<AVoxelDebris>(A))Ok=D->RestoreObjectState(E.Geometry,E.Dynamic);
        if(auto T=Cast<AVoxelFallingTimber>(A))Ok=T->RestoreObjectState(E.Geometry,E.Dynamic);
        if(auto P=Cast<AVoxelEnvironmentLODPrototype>(A))Ok=P->RestoreObjectState(E.Geometry,E.Dynamic);
    }else if(E.GeometryFormat==0){
        FMemoryReader Ar(*E.Geometry);Ar.SetCustomVersion(VoxelPackedTimberMesh::VersionKey,1,NAME_None);
        if(auto D=Cast<AVoxelDebris>(A))Ok=D->PersistentState(Ar);
        if(auto T=Cast<AVoxelFallingTimber>(A))Ok=T->PersistentState(Ar);
        if(auto P=Cast<AVoxelEnvironmentLODPrototype>(A))Ok=P->PersistentState(Ar);
        Ok&=!Ar.IsError()&&Ar.Tell()==Ar.TotalSize();
    }
    if(!Ok){A->Destroy();return nullptr;}
    A->SetActorTransform(E.Transform,false,nullptr,ETeleportType::TeleportPhysics);
    if(auto Life=A->FindComponentByClass<UVoxelDebrisLifecycle>())Life->RestoreState(E.Lifetime);
    if(auto Body=Cast<UPrimitiveComponent>(A->GetRootComponent())){
        if(W->GetNetMode()==NM_Client)Body->SetSimulatePhysics(false);
        else if(Body->IsSimulatingPhysics()){Body->SetPhysicsLinearVelocity(E.Velocity);Body->SetPhysicsAngularVelocityInRadians(E.AngularVelocity);}
    }
    if(W->GetNetMode()==NM_Client&&!A->IsA<AVoxelEnvironmentLODPrototype>())A->SetActorTickEnabled(false);
    if(auto P=Cast<AVoxelEnvironmentLODPrototype>(A))if(P->IsFellable()&&!VoxelTreeFelling::IsEquipped(W))VoxelTreeFelling::Prepare(W,P->GetActorLocation());
    return A;
}
void RefreshObjects(UWorld* W) {
    if(!W||W->GetNetMode()==NM_Client||IsLoading(W))return;
    auto& Registry=VoxelObjects::Get(W);
    for(TActorIterator<AActor> It(W);It;++It){
        auto A=*It;if(!IsObject(A))continue;
        if(auto L=A->FindComponentByClass<UVoxelDebrisLifecycle>())if(L->CaptureState().Kind==EVoxelDebrisLifetime::Cosmetic)continue;
        const uint8 Kind=A->IsA<AVoxelDebris>()?1:A->IsA<AVoxelFallingTimber>()?2:3;
        const FGuid Id=Registry.Bind(A,Kind);auto Existing=Registry.Find(Id);if(!Existing)continue;
        auto E=*Existing;if(!CaptureObject(E))continue;
        if(E.Geometry!=Existing->Geometry||E.Dynamic!=Existing->Dynamic||!E.Transform.Equals(Existing->Transform)||E.Lifetime.RemainingSeconds!=Existing->Lifetime.RemainingSeconds||E.bActivePhysics!=Existing->bActivePhysics||E.bRetained!=Existing->bRetained||E.Velocity!=Existing->Velocity||E.AngularVelocity!=Existing->AngularVelocity)
            Registry.Update(Id,[&](VoxelObjects::FEntry& Out){Out=MoveTemp(E);});
    }
}
bool CaptureSnapshot(UWorld* W,FSnapshot& Out) {
    check(IsInGameThread());Out.Reset();if(!W||W->GetNetMode()==NM_Client||IsLoading(W)||HasLoadFailure(W))return false;
    RefreshObjects(W);
    if(!W->bIsTearingDown){
        for(TActorIterator<AActor> It(W);It;++It){
            if(!IsObject(*It))continue;
            if(auto Life=It->FindComponentByClass<UVoxelDebrisLifecycle>())if(Life->CaptureState().Kind==EVoxelDebrisLifetime::Cosmetic)continue;
            auto E=VoxelObjects::Get(W).Find(*It);if(!E||!E->Geometry)return false;
            auto Check=*E;if(!CaptureObject(Check))return false;
        }
    }
    Out=VoxelObjects::Get(W).Snapshot();
    for(auto& E:Out){E.Actor.Reset();if(E.Residency!=VoxelObjects::EResidency::Tombstone&&!E.Geometry&&!E.Page.IsValid())return false;}
    return Out.Num()<=4096;
}
bool EncodeSnapshot(const FSnapshot& Snapshot,TArray<uint8>& Payload) {
    Payload.Reset();FMemoryWriter Ar(Payload);int32 Marker=-4,N=Snapshot.Num();Ar<<Marker<<N;if(N<0||N>4096)return false;
    for(const auto& Original:Snapshot){auto E=Original;if(!VoxelObjectPages::Hydrate(E))return false;uint8 State=uint8(E.Residency),Life=uint8(E.Lifetime.Kind);
        Ar<<E.Id<<E.Revision<<E.GeometryRevision<<E.Kind<<State<<E.Transform<<E.Velocity<<E.AngularVelocity<<E.BoundsExtent
          <<Life<<E.Lifetime.RemainingSeconds<<E.OwnerId<<E.bRetained<<E.bActivePhysics<<E.GeometryFormat;
        if(!Bytes(Ar,E.Dynamic,1024*1024))return false;
        int32 Size=E.Geometry?E.Geometry->Num():0;Ar<<Size;if(Size)Ar.Serialize(const_cast<uint8*>(E.Geometry->GetData()),Size);
        if(Ar.IsError()||Payload.Num()>Limit)return false;
    }return true;
}
bool DecodeSnapshot(const TArray<uint8>& Payload,FSnapshot& Out) {
    FMemoryReader Ar(Payload);int32 Marker=0,N=0;Ar<<Marker<<N;if(Marker!=-4||N<0||N>4096||Ar.IsError())return false;
    Out.Reset();TSet<FGuid> Seen;
    for(int32 I=0;I<N;++I){VoxelObjects::FEntry E;uint8 State=0,Life=0;
        Ar<<E.Id<<E.Revision<<E.GeometryRevision<<E.Kind<<State<<E.Transform<<E.Velocity<<E.AngularVelocity<<E.BoundsExtent
          <<Life<<E.Lifetime.RemainingSeconds<<E.OwnerId<<E.bRetained<<E.bActivePhysics<<E.GeometryFormat;
        if(!E.Id.IsValid()||Seen.Contains(E.Id)||!E.Revision||E.Kind<1||E.Kind>3||State>uint8(VoxelObjects::EResidency::Tombstone)||Life>3||E.GeometryFormat>1||!E.Transform.IsValid()||E.Velocity.ContainsNaN()||E.AngularVelocity.ContainsNaN()||E.BoundsExtent.ContainsNaN()||!FMath::IsFinite(E.Lifetime.RemainingSeconds)||E.Lifetime.RemainingSeconds<0)return false;
        if(E.Kind==3&&!VoxelEnvironmentAsset::IsSupportedTransform(E.Transform))return false;
        Seen.Add(E.Id);E.Lifetime.Kind=EVoxelDebrisLifetime(Life);E.Residency=State==uint8(VoxelObjects::EResidency::Tombstone)?VoxelObjects::EResidency::Tombstone:VoxelObjects::EResidency::Dormant;
        TArray<uint8> Geometry;if(!Bytes(Ar,E.Dynamic,1024*1024)||!Bytes(Ar,Geometry,Limit))return false;
        if(E.Residency!=VoxelObjects::EResidency::Tombstone&&Geometry.IsEmpty())return false;
        if(!Geometry.IsEmpty())E.Geometry=MakeShared<const TArray<uint8>,ESPMode::ThreadSafe>(MoveTemp(Geometry));
        Out.Add(MoveTemp(E));
    }return !Ar.IsError()&&Ar.Tell()==Ar.TotalSize();
}
bool InstallSnapshot(UWorld* W,FSnapshot&& Snapshot) {
    if(!W||W->GetNetMode()==NM_Client)return false;
    // A loaded world begins with data records; nearby actors materialize under
    // the same bounded streaming policy used when returning to a region.
    auto& Registry=VoxelObjects::Get(W);
    if(Registry.Num())return false;
    for(auto& E:Snapshot)if(!Registry.Import(MoveTemp(E)))return false;
    UE_LOG(LogVoxelEarth,Log,TEXT("ObjectSave installed records=%d; nearby residency staged"),Registry.Num());return true;
}
namespace {
struct FDriver {
    FTSTicker::FDelegateHandle TickHandle;
    FDriver(){TickHandle=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float){
        if(!GEngine)return true;
        static TMap<TWeakObjectPtr<UWorld>,double> Samples;
        for(auto It=Samples.CreateIterator();It;++It)if(!It.Key().IsValid())It.RemoveCurrent();
        for(const auto& Context:GEngine->GetWorldContexts()){
            auto W=Context.World();if(!W||!W->IsGameWorld()||W->bIsTearingDown||W->GetNetMode()==NM_Client||W->IsPaused())continue;
            double& Last=Samples.FindOrAdd(W,W->GetTimeSeconds());const double Delta=W->GetTimeSeconds()-Last;if(Delta<.1)continue;Last=W->GetTimeSeconds();
            RefreshObjects(W);TArray<VoxelObjects::FView> Views;
            for(auto It=W->GetPlayerControllerIterator();It;++It)if(auto PC=It->Get()){FVector P;FRotator ViewRotation;PC->GetPlayerViewPoint(P,ViewRotation);Views.Add({P,ViewRotation.Vector()});}
            VoxelObjects::FCallbacks C;C.Capture=&CaptureObject;C.BeginRestore=[W](const auto& E){
                const FGuid Id=E.Id;const uint64 GeometryRevision=E.GeometryRevision;
                return BeginRestoreObject(W,E,[W,Id,GeometryRevision](AActor* A){
                    auto R=VoxelObjects::Find(W);auto Current=R?R->Find(Id):nullptr;
                    if(!A){if(R)R->CancelRestore(Id);return false;}
                    if(!Current||Current->Residency!=VoxelObjects::EResidency::Restoring||Current->GeometryRevision!=GeometryRevision)return false;
                    if(!PublishRestoredObject(A,*Current)){R->CancelRestore(Id);return false;}
                    return R->CompleteRestore(Id,GeometryRevision,A);
                },[W,Id,GeometryRevision](){
                    auto R=VoxelObjects::Find(W);auto E=R?R->Find(Id):nullptr;
                    if(!E||E->Residency!=VoxelObjects::EResidency::Restoring||E->GeometryRevision!=GeometryRevision)return false;
                    TArray<VoxelObjects::FView> Views;
                    for(auto It=W->GetPlayerControllerIterator();It;++It)if(auto PC=It->Get()){FVector P;FRotator ViewRotation;PC->GetPlayerViewPoint(P,ViewRotation);Views.Add({P,ViewRotation.Vector()});}
                    return !VoxelObjects::FRegistry::ShouldEvict(*E,Views,20000.);
                })!=0;
            };C.Evict=[](AActor* A){A->Destroy();};
            auto Result=VoxelObjects::Get(W).Tick(Views,Delta,C);
            VoxelObjectPages::Tick(W,Views);
            if(Result.Restored||Result.Evicted||Result.Expired)UE_LOG(LogVoxelEarth,Log,TEXT("ObjectStream restored=%d evicted=%d expired=%d"),Result.Restored,Result.Evicted,Result.Expired);
        }return true;
    }));}
    ~FDriver(){FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);}
};FDriver Driver;
}
}
