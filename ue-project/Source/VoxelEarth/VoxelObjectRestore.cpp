#include "VoxelDetachedPersistence.h"
#include "VoxelEarth.h"
#include "VoxelWorldSubsystem.h"
#include "VoxelFineTileStreamer.h"
#include "VoxelEnvironmentLODPrototype.h"
#include "VoxelTreeFellingPrototype.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"

namespace VoxelDetachedPersistence {
namespace {
struct FPending {
    uint64 Handle=0,StartFrame=0;
    double StartedAt=0,MaxStepMs=0;
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
    const bool Accepted=P->Completion(Success?A:nullptr);
    if(!Success||!Accepted)CancelActor(A);
    UE_LOG(LogVoxelEarth,Log,TEXT("ObjectRestore id=%s accepted=%d elapsedMs=%.3f frames=%llu maxStepMs=%.3f"),*P->Entry.Id.ToString(),int(Success&&Accepted),(FPlatformTime::Seconds()-P->StartedAt)*1000.,GFrameCounter-P->StartFrame,P->MaxStepMs);
}
void TickRestores() {
    // A global budget, across worlds and actors. A single component upload is
    // indivisible, so this is a soft time limit rather than a hard frame cap.
    const double Start=FPlatformTime::Seconds();int32 Steps=0;
    const auto Work=Pending;static uint32 Cursor=0;
    const uint32 Offset=Work.Num()?Cursor++%uint32(Work.Num()):0;
    for(int32 I=0;I<Work.Num();++I) {
        const auto& P=Work[(Offset+I)%Work.Num()];
        if(!Pending.Contains(P))continue;
        auto W=P->World.Get();
        if(!W||W->bIsTearingDown||(P->StillWanted&&!P->StillWanted())){Finish(P,false);continue;}
        if(W->IsPaused())continue;
        if(Steps>=2||FPlatformTime::Seconds()-Start>=.002)break;
        const double StepStart=FPlatformTime::Seconds();
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
                    A->SetActorTransform(P->Entry.Transform);
                    TWeakPtr<FPending> Weak=P;
                    auto Done=[Weak](bool Ok){if(auto Job=Weak.Pin()){Job->Ready=Ok;Job->Failed=!Ok;}};
                    if(auto T=Cast<AVoxelFallingTimber>(A))T->BeginStagedObjectRestore(P->Entry.Geometry,P->Entry.Dynamic,Done);
                    if(auto E=Cast<AVoxelEnvironmentLODPrototype>(A))E->BeginStagedObjectRestore(P->Entry.Geometry,P->Entry.Dynamic,Done);
                }
            }
        }else if(!P->Ready&&P->Actor.IsValid()){
            auto A=P->Actor.Get();
            bool Advanced=false;
            if(auto T=Cast<AVoxelFallingTimber>(A))Advanced=T->AdvanceStagedObjectRestore();
            if(auto E=Cast<AVoxelEnvironmentLODPrototype>(A))Advanced=E->AdvanceStagedObjectRestore();
            if(Advanced)++Steps;
        }else if(!P->Actor.IsValid())P->Failed=true;
        if(P->Ready&&!P->Failed){
            auto A=P->Actor.Get();auto Plant=Cast<AVoxelEnvironmentLODPrototype>(A);
            const bool Oak=Plant&&Plant->IsFellable();
            if(W->GetNetMode()!=NM_Client&&(P->Entry.Kind==2||Oak)){
                P->MaxStepMs=FMath::Max(P->MaxStepMs,(FPlatformTime::Seconds()-StepStart)*1000.);
                if(Steps>=2||FPlatformTime::Seconds()-Start>=.002)continue;
                ++Steps;
                const auto Timber=Cast<AVoxelFallingTimber>(A);
                const bool GroundReady=Timber?VoxelTreeFelling::AdvanceRestoreGround(W,Timber->GetGroundSupportBounds()):VoxelTreeFelling::AdvanceRestoreGround(W,P->Entry.Transform.GetLocation());
                P->MaxStepMs=FMath::Max(P->MaxStepMs,(FPlatformTime::Seconds()-StepStart)*1000.);
                if(!GroundReady)continue;
            }
            if(Oak)VoxelTreeFelling::EnsureAxe(W);
        }
        P->MaxStepMs=FMath::Max(P->MaxStepMs,(FPlatformTime::Seconds()-StepStart)*1000.);
        if(P->Ready||P->Failed)Finish(P,P->Ready&&!P->Failed);
    }
}
struct FRestoreDriver {
    FTSTicker::FDelegateHandle Tick;
    FDelegateHandle Teardown;
    FRestoreDriver(){
        Tick=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float){TickRestores();return true;}));
        Teardown=FWorldDelegates::OnWorldBeginTearDown.AddLambda([](UWorld* W){
            const auto Jobs=Pending;for(const auto& P:Jobs)if(P->World.Get()==W)Finish(P,false);
        });
    }
    ~FRestoreDriver(){FTSTicker::GetCoreTicker().RemoveTicker(Tick);FWorldDelegates::OnWorldBeginTearDown.Remove(Teardown);}
} RestoreDriver;
}
uint64 BeginRestoreObject(UWorld* W,const VoxelObjects::FEntry& E,TFunction<bool(AActor*)> Completion,TFunction<bool()> StillWanted) {
    check(IsInGameThread());if(!W||W->bIsTearingDown||!E.Geometry||E.Kind<1||E.Kind>3||Pending.Num()>=4)return 0;
    if(E.Kind==3&&!VoxelEnvironmentAsset::IsSupportedTransform(E.Transform))return 0;
    // Off-screen saved physics must not monopolize the bounded mesh queue
    // while its terrain support is unavailable. The fair registry retries it.
    if(E.Kind==2&&W->GetNetMode()!=NM_Client){
        if(auto Sub=W->GetSubsystem<UVoxelWorldSubsystem>())if(auto Fine=Sub->GetFineTileStreamer()){
            const auto P=E.Transform.GetLocation();
            if(!Fine->IsFootprintResident(FMath::FloorToInt64((P.X-1600)*10),FMath::FloorToInt64((P.Y-1600)*10),
                FMath::CeilToInt64((P.X+1600)*10)+1,FMath::CeilToInt64((P.Y+1600)*10)+1))return 0;
        }
    }
    auto P=MakeShared<FPending>();P->Handle=++NextHandle;P->StartFrame=GFrameCounter;P->StartedAt=FPlatformTime::Seconds();P->World=W;P->Entry=E;P->Completion=MoveTemp(Completion);P->StillWanted=MoveTemp(StillWanted);Pending.Add(P);return P->Handle;
}
void CancelRestoreObject(uint64 Handle) {
    check(IsInGameThread());for(const auto& P:Pending)if(P->Handle==Handle){auto Copy=P;Finish(Copy,false);return;}
}
bool PublishRestoredObject(AActor* A,const VoxelObjects::FEntry& E) {
    if(!IsValid(A)||(E.Kind==3&&!VoxelEnvironmentAsset::IsSupportedTransform(E.Transform)))return false;
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
