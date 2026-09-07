#include "VoxelDetachedPersistence.h"
#include "VoxelEnvironmentLODPrototype.h"
#include "VoxelTreeFellingPrototype.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"

namespace VoxelDetachedPersistence {
namespace {
struct FPending {
    uint64 Handle=0;
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<AActor> Actor;
    VoxelObjects::FEntry Entry;
    TFunction<bool(AActor*)> Completion;
    TFunction<bool()> StillWanted;
    bool Started=false,Ready=false,Failed=false;
};
TArray<TSharedPtr<FPending>> Pending;
uint64 NextHandle=0;
void CancelActor(AActor* A) {
    if(!IsValid(A))return;
    if(auto P=Cast<AVoxelEnvironmentLODPrototype>(A))P->CancelStagedObjectRestore();
    if(auto T=Cast<AVoxelFallingTimber>(A))T->CancelStagedObjectRestore();
    A->Destroy();
}
void Finish(const TSharedPtr<FPending>& P,bool Success) {
    Pending.Remove(P);
    AActor* A=P->Actor.Get();
    if(!P->Completion(Success?A:nullptr))CancelActor(A);
}
void TickRestores() {
    // A global budget, across worlds and actors. A single component upload is
    // indivisible, so this is a soft time limit rather than a hard frame cap.
    const double Start=FPlatformTime::Seconds();int32 Steps=0;
    const auto Work=Pending;
    for(const auto& P:Work) {
        if(!Pending.Contains(P))continue;
        auto W=P->World.Get();
        if(!W||W->bIsTearingDown||(P->StillWanted&&!P->StillWanted())){Finish(P,false);continue;}
        if(W->IsPaused())continue;
        if(Steps>=2||FPlatformTime::Seconds()-Start>=.002)break;
        if(!P->Started){
            P->Started=true;++Steps;
            if(P->Entry.GeometryFormat!=1||P->Entry.Kind==1){
                auto A=RestoreObject(W,P->Entry);P->Actor=A;
                if(A)A->Tags.AddUnique(TEXT("ObjectRestorePending"));
                P->Ready=A!=nullptr;P->Failed=!P->Ready;
            }else{
                AActor* A=P->Entry.Kind==2?static_cast<AActor*>(W->SpawnActor<AVoxelFallingTimber>()):static_cast<AActor*>(W->SpawnActor<AVoxelEnvironmentLODPrototype>());
                P->Actor=A;
                if(!A){P->Failed=true;}
                else{
                    A->Tags.AddUnique(TEXT("ObjectRestorePending"));
                    TWeakPtr<FPending> Weak=P;
                    auto Done=[Weak](bool Ok){if(auto Job=Weak.Pin()){Job->Ready=Ok;Job->Failed=!Ok;}};
                    if(auto T=Cast<AVoxelFallingTimber>(A))T->BeginStagedObjectRestore(P->Entry.Geometry,P->Entry.Dynamic,Done);
                    if(auto E=Cast<AVoxelEnvironmentLODPrototype>(A))E->BeginStagedObjectRestore(P->Entry.Geometry,P->Entry.Dynamic,Done);
                }
            }
        }else if(auto A=P->Actor.Get()){
            bool Advanced=false;
            if(auto T=Cast<AVoxelFallingTimber>(A))Advanced=T->AdvanceStagedObjectRestore();
            if(auto E=Cast<AVoxelEnvironmentLODPrototype>(A))Advanced=E->AdvanceStagedObjectRestore();
            if(Advanced)++Steps;
        }else P->Failed=true;
        if(P->Ready||P->Failed)Finish(P,P->Ready&&!P->Failed);
    }
}
struct FDriver {
    FTSTicker::FDelegateHandle Tick;
    FDelegateHandle Teardown;
    FDriver(){
        Tick=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float){TickRestores();return true;}));
        Teardown=FWorldDelegates::OnWorldBeginTearDown.AddLambda([](UWorld* W){
            const auto Jobs=Pending;for(const auto& P:Jobs)if(P->World.Get()==W)Finish(P,false);
        });
    }
    ~FDriver(){FTSTicker::GetCoreTicker().RemoveTicker(Tick);FWorldDelegates::OnWorldBeginTearDown.Remove(Teardown);}
} Driver;
}
uint64 BeginRestoreObject(UWorld* W,const VoxelObjects::FEntry& E,TFunction<bool(AActor*)> Completion,TFunction<bool()> StillWanted) {
    check(IsInGameThread());if(!W||W->bIsTearingDown||!E.Geometry||E.Kind<1||E.Kind>3||Pending.Num()>=4)return 0;
    auto P=MakeShared<FPending>();P->Handle=++NextHandle;P->World=W;P->Entry=E;P->Completion=MoveTemp(Completion);P->StillWanted=MoveTemp(StillWanted);Pending.Add(P);return P->Handle;
}
void CancelRestoreObject(uint64 Handle) {
    check(IsInGameThread());for(const auto& P:Pending)if(P->Handle==Handle){auto Copy=P;Finish(Copy,false);return;}
}
bool PublishRestoredObject(AActor* A,const VoxelObjects::FEntry& E) {
    if(!IsValid(A))return false;
    if(E.GeometryFormat==1){
        if(auto T=Cast<AVoxelFallingTimber>(A))if(!T->PublishStagedObjectRestore())return false;
        if(auto P=Cast<AVoxelEnvironmentLODPrototype>(A))if(!P->PublishStagedObjectRestore())return false;
    }
    A->SetActorTransform(E.Transform,false,nullptr,ETeleportType::TeleportPhysics);
    if(auto Life=A->FindComponentByClass<UVoxelDebrisLifecycle>())Life->RestoreState(E.Lifetime);
    if(auto Body=Cast<UPrimitiveComponent>(A->GetRootComponent())){
        if(A->GetWorld()->GetNetMode()==NM_Client)Body->SetSimulatePhysics(false);
        else if(Body->IsSimulatingPhysics()){Body->SetPhysicsLinearVelocity(E.Velocity);Body->SetPhysicsAngularVelocityInRadians(E.AngularVelocity);}
    }
    A->Tags.Remove(TEXT("ObjectRestorePending"));return true;
}
}
