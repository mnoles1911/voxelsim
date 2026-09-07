#include "VoxelGameplayActors.h"
#include "VoxelBoat.h"
#include "VoxelGlider.h"
#include "VoxelThrownItem.h"
#include "VoxelExplosive.h"
#include "VoxelPlayerRecords.h"
#include "VoxelEarthPlayerController.h"
#include "VoxelEarthFlyPawn.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "Hash/Blake3.h"
#include "HAL/PlatformFileManager.h"

namespace VoxelGameplayActors
{
namespace Detail
{
TMap<TWeakObjectPtr<AActor>,FRecord> Pending;
bool HashFile(const FString& Path,FString& Hash)
{
    TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
    if(!File || File->Size()<=0 || File->Size()>128*1024*1024) return false;
    FBlake3 Hasher; TArray<uint8> Block; Block.SetNumUninitialized(64*1024);
    int64 Left=File->Size();
    while(Left>0)
    {
        const int64 N=FMath::Min<int64>(Left,Block.Num());
        if(!File->Read(Block.GetData(),N)) return false;
        Hasher.Update(Block.GetData(),N); Left-=N;
    }
    Hash=LexToString(Hasher.Finalize()); return true;
}
bool Content(const UVoxelAssetBodyComponent* Body,FRecord& R,TMap<FString,FString>& Cache)
{
    if(!Body || Body->IsPlaceholder()) return false;
    R.AssetName=Body->AssetName; R.AssetPath=Body->GetResolvedPath();
    if(const auto Hash=Cache.Find(R.AssetPath)) R.AssetHash=*Hash;
    else { if(!HashFile(R.AssetPath,R.AssetHash)) return false; Cache.Add(R.AssetPath,R.AssetHash); }
    return true;
}
}
struct FAdapter
{
    static bool Capture(AActor* Actor,FRecord& R,TMap<FString,FString>& Cache)
    {
        if(const auto Pending=Detail::Pending.Find(Actor)) { R=*Pending; return true; }
        R.Transform=Actor->GetActorTransform();
        if(auto Boat=Cast<AVoxelBoat>(Actor))
        {
            R.Kind=EKind::Boat; R.Id=Boat->PersistentId; R.Owner=Boat->PersistentPilot;
            if(auto PC=Cast<AVoxelEarthPlayerController>(Boat->Driver.Get())) R.Owner=VoxelPlayerRecords::PlayerId(PC);
            if(!Detail::Content(Boat->Body,R,Cache) || !Boat->PhysicsBody) return false;
            R.Velocity=Boat->PhysicsBody->GetPhysicsLinearVelocity(); R.AngularVelocity=Boat->PhysicsBody->GetPhysicsAngularVelocityInRadians();
            R.Resting=Boat->bAsleep; R.Grounded=Boat->bGrounded; R.Throttle=Boat->ScriptThrottle;
            R.Remaining=FMath::Max(0.0,Boat->ScriptThrottleUntilS-Actor->GetWorld()->GetTimeSeconds());
        }
        else if(auto Glider=Cast<AVoxelGlider>(Actor))
        {
            R.Kind=EKind::Glider; R.Id=Glider->PersistentId; R.Owner=Glider->PersistentPilot;
            if(auto PC=Cast<AVoxelEarthPlayerController>(Glider->Driver.Get())) R.Owner=VoxelPlayerRecords::PlayerId(PC);
            if(!Detail::Content(Glider->Body,R,Cache)) return false;
            R.Velocity=Glider->VelocityUU; R.Resting=Glider->bParked; R.LaunchPending=Glider->bLaunchPressed;
            R.Transform.SetRotation(FRotator(Glider->PitchDeg,Glider->YawDeg,Glider->RollDeg).Quaternion());
        }
        else if(auto Item=Cast<AVoxelThrownItem>(Actor))
        {
            if(Item->bItemAccountedFor || !Item->Projectile) return false;
            R.Kind=EKind::Item; R.Id=Item->PersistentId; R.Owner=Item->PersistentOwner;
            R.Item=Item->ItemId; R.Count=Item->Count; R.Velocity=Item->Projectile->Velocity;
            R.Resting=Item->bSettled; R.FlightAge=Item->FlightSeconds; R.SettledAge=Item->SettledSeconds;
            R.Remaining=Actor->GetWorld()->GetTimerManager().GetTimerRemaining(Item->ReturnTimerHandle);
        }
        else if(auto Charge=Cast<AVoxelExplosive>(Actor))
        {
            if(!Charge->Projectile) return false;
            R.Kind=EKind::Explosive; R.Id=Charge->PersistentId; R.Velocity=Charge->Projectile->Velocity;
            R.Resting=Charge->bLanded; R.Radius=Charge->ThisChargeRadiusUU;
            R.Remaining=FMath::Max(0.0,double(Actor->GetWorld()->GetTimerManager().GetTimerRemaining(Charge->FuseTimerHandle)));
        }
        else return false;
        return true;
    }
    static void Prepare(AActor* Actor,const FRecord& R)
    {
        if(auto Boat=Cast<AVoxelBoat>(Actor))
        { Boat->PersistentId=R.Id; Boat->PersistentPilot=R.Owner; Boat->bCheckpointContent=true; Boat->Body->AssetName=R.AssetName; Boat->Body->AssetFilePath=R.AssetPath; }
        if(auto Glider=Cast<AVoxelGlider>(Actor))
        { Glider->PersistentId=R.Id; Glider->PersistentPilot=R.Owner; Glider->bCheckpointContent=true; Glider->Body->AssetName=R.AssetName; Glider->Body->AssetFilePath=R.AssetPath; }
        if(auto Item=Cast<AVoxelThrownItem>(Actor)) { Item->PersistentId=R.Id; Item->PersistentOwner=R.Owner; }
        if(auto Charge=Cast<AVoxelExplosive>(Actor)) Charge->PersistentId=R.Id;
        Detail::Pending.Add(Actor,R);
    }
    static void Apply(AActor* Actor,const FRecord& R)
    {
        Actor->SetActorTransform(R.Transform,false,nullptr,ETeleportType::TeleportPhysics);
        auto& Timers=Actor->GetWorld()->GetTimerManager();
        if(auto Boat=Cast<AVoxelBoat>(Actor))
        {
            Boat->bAsleep=R.Resting; Boat->bGrounded=R.Grounded;
            Boat->ScriptThrottle=R.Throttle; Boat->ScriptThrottleUntilS=Actor->GetWorld()->GetTimeSeconds()+R.Remaining;
            Boat->PhysicsBody->SetSimulatePhysics(!R.Resting);
            Boat->PhysicsBody->SetPhysicsLinearVelocity(R.Velocity); Boat->PhysicsBody->SetPhysicsAngularVelocityInRadians(R.AngularVelocity);
            Boat->bHaveLastWake=false; Boat->bProbeSeen=false;
        }
        if(auto Glider=Cast<AVoxelGlider>(Actor))
        {
            Glider->VelocityUU=R.Velocity; Glider->bParked=R.Resting; Glider->bLaunchPressed=R.LaunchPending;
            const auto Rotation=R.Transform.Rotator(); Glider->YawDeg=Rotation.Yaw; Glider->PitchDeg=Rotation.Pitch; Glider->RollDeg=Rotation.Roll;
        }
        if(auto Item=Cast<AVoxelThrownItem>(Actor))
        {
            Item->ItemId=R.Item; Item->Count=R.Count; Item->bSettled=R.Resting; Item->FlightSeconds=R.FlightAge; Item->SettledSeconds=R.SettledAge;
            Item->LastTickLocationUU=R.Transform.GetLocation(); Item->Projectile->Velocity=R.Velocity;
            if(R.Resting) Item->Projectile->Deactivate(); else Item->Projectile->Activate();
            Item->SetActorTickInterval(R.Resting?AVoxelThrownItem::SettledTickIntervalSeconds:0);
            if(R.Remaining>=0) Timers.SetTimer(Item->ReturnTimerHandle,Item,&AVoxelThrownItem::OnReturnTimerExpired,FMath::Max(0.0001,R.Remaining),false);
        }
        if(auto Charge=Cast<AVoxelExplosive>(Actor))
        {
            Charge->bLanded=R.Resting; Charge->ThisChargeRadiusUU=R.Radius; Charge->LastTickLocationUU=R.Transform.GetLocation();
            Charge->Projectile->Velocity=R.Velocity;
            if(R.Resting) Charge->Projectile->Deactivate(); else Charge->Projectile->Activate();
            Timers.SetTimer(Charge->FuseTimerHandle,Charge,&AVoxelExplosive::Detonate,FMath::Max(0.0001,R.Remaining),false);
        }
    }
    static APawn* Pawn(AVoxelEarthPlayerController* PC)
    {
        if(!PC) return nullptr;
        if(auto Boat=Cast<AVoxelBoat>(PC->GetPawn())) return Boat->StoredPawn.Get();
        if(auto Glider=Cast<AVoxelGlider>(PC->GetPawn())) return Glider->StoredPawn.Get();
        return PC->GetPawn();
    }
    static FGuid Vehicle(AVoxelEarthPlayerController* PC)
    {
        if(!PC) return FGuid();
        if(auto Boat=Cast<AVoxelBoat>(PC->GetPawn())) return Boat->PersistentId;
        if(auto Glider=Cast<AVoxelGlider>(PC->GetPawn())) return Glider->PersistentId;
        return FGuid();
    }
    template<class T> static bool Bind(T* Vehicle,AVoxelEarthPlayerController* PC,const FGuid& Id)
    {
        if(Vehicle->PersistentId!=Id || Vehicle->PersistentPilot!=VoxelPlayerRecords::PlayerId(PC) || Vehicle->StoredPawn.IsValid()) return false;
        auto Pawn=PC->GetPawn(); if(!Cast<AVoxelEarthFlyPawn>(Pawn)) return false;
        Vehicle->StoredPawn=Pawn; Vehicle->Driver=PC;
        Pawn->SetActorHiddenInGame(true); Pawn->SetActorEnableCollision(false); Pawn->SetActorTickEnabled(false);
        PC->Possess(Vehicle); return true;
    }
    template<class T> static bool Release(T* Vehicle,AVoxelEarthPlayerController* PC)
    {
        Vehicle->PersistentPilot=VoxelPlayerRecords::PlayerId(PC);
        PC->UnPossess();
        if(auto Pawn=Vehicle->StoredPawn.Get()) Pawn->Destroy();
        Vehicle->StoredPawn=nullptr; Vehicle->Driver=nullptr; return true;
    }
};
void FinishRestore(AActor* Actor)
{
    if(auto Pending=Detail::Pending.Find(Actor))
    { const FRecord R=*Pending; Detail::Pending.Remove(Actor); FAdapter::Apply(Actor,R); }
}
APawn* PlayerPawn(AVoxelEarthPlayerController* PC) { return FAdapter::Pawn(PC); }
FGuid VehicleId(AVoxelEarthPlayerController* PC) { return FAdapter::Vehicle(PC); }
bool BindVehicle(AVoxelEarthPlayerController* PC,const FGuid& Id)
{
    if(!PC || !PC->HasAuthority() || !Id.IsValid()) return false;
    if(VehicleId(PC)==Id) return true;
    for(TActorIterator<AVoxelBoat> It(PC->GetWorld());It;++It) if(FAdapter::Bind(*It,PC,Id)) return true;
    for(TActorIterator<AVoxelGlider> It(PC->GetWorld());It;++It) if(FAdapter::Bind(*It,PC,Id)) return true;
    return false;
}
bool ReleaseVehicle(AVoxelEarthPlayerController* PC)
{
    if(!PC || !PC->HasAuthority()) return false;
    if(auto Boat=Cast<AVoxelBoat>(PC->GetPawn())) return FAdapter::Release(Boat,PC);
    if(auto Glider=Cast<AVoxelGlider>(PC->GetPawn())) return FAdapter::Release(Glider,PC);
    return false;
}
bool Capture(UWorld* W,TArray<uint8>& Bytes)
{
    Bytes.Reset(); if(!W || W->GetNetMode()==NM_Client) return false;
    TArray<FRecord> Records; TMap<FString,FString> Cache;
    for(auto It=Detail::Pending.CreateIterator();It;++It) if(!It.Key().IsValid()) It.RemoveCurrent();
    for(TActorIterator<AActor> It(W);It;++It)
    {
        if(!It->IsA<AVoxelBoat>() && !It->IsA<AVoxelGlider>() && !It->IsA<AVoxelThrownItem>() && !It->IsA<AVoxelExplosive>()) continue;
        if(It->IsActorBeingDestroyed()) continue;
        if(Records.Num()>=8192) return false;
        FRecord R; if(!FAdapter::Capture(*It,R,Cache)) return false; Records.Add(MoveTemp(R));
    }
    Records.Sort([](const FRecord& A,const FRecord& B){return A.Id.ToString()<B.Id.ToString();});
    return Encode(Records,Bytes);
}
bool ValidateContent(const TArray<FRecord>& Records)
{
    TMap<FString,FString> Cache;
    for(const auto& R:Records)
        if(R.Kind==EKind::Boat || R.Kind==EKind::Glider)
        {
            FString Hash;
            if(const auto Existing=Cache.Find(R.AssetPath)) Hash=*Existing;
            else { if(!Detail::HashFile(R.AssetPath,Hash)) return false; Cache.Add(R.AssetPath,Hash); }
            if(Hash!=R.AssetHash) return false;
        }
    return true;
}
bool Restore(UWorld* W,const TArray<uint8>& Bytes)
{
    if(!W || W->GetNetMode()==NM_Client) return false;
    TArray<FRecord> Records; if(!Decode(Bytes,Records) || !ValidateContent(Records)) return false;
    for(TActorIterator<AActor> It(W);It;++It)
        if(It->IsA<AVoxelBoat>() || It->IsA<AVoxelGlider>() || It->IsA<AVoxelThrownItem>() || It->IsA<AVoxelExplosive>()) return false;
    TArray<AActor*> Spawned;
    for(const auto& R:Records)
    {
        UClass* Class=R.Kind==EKind::Boat?AVoxelBoat::StaticClass():R.Kind==EKind::Glider?AVoxelGlider::StaticClass():R.Kind==EKind::Item?AVoxelThrownItem::StaticClass():AVoxelExplosive::StaticClass();
        FActorSpawnParameters Params; Params.bDeferConstruction=true; Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto Actor=W->SpawnActor<AActor>(Class,R.Transform,Params);
        if(!Actor) { for(auto Created:Spawned) { Detail::Pending.Remove(Created); Created->Destroy(); } return false; }
        Spawned.Add(Actor); FAdapter::Prepare(Actor,R); Actor->FinishSpawning(R.Transform);
    }
    return true;
}
}
