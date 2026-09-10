#include "VoxelSessionCheckpoint.h"
#include "VoxelWaterSubsystem.h"
#include "VoxelSkySubsystem.h"
#include "VoxelWorldSubsystem.h"
#include "VoxelSaveLibrary.h"
#include "VoxelDetachedPersistence.h"
#include "VoxelAgentSubsystem.h"
#include "VoxelSaveJobs.h"
#include "VoxelPlayerRecords.h"
#include "VoxelGameplayActors.h"
#include "VoxelEarthPlayerController.h"
#include "VoxelEarth.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Base64.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace VoxelSessionCheckpoint
{
namespace Detail
{
bool Relationships(const TArray<VoxelPlayerRecords::FRecord>& StagedPlayers,const TArray<VoxelGameplayActors::FRecord>& StagedActors)
{
    for(const auto& Actor:StagedActors)
    {
        if(!Actor.Owner.IsValid()) continue;
        const auto Player=StagedPlayers.FindByPredicate([&](const auto& P){return P.Id==Actor.Owner;});
        if(!Player) return false;
        if((Actor.Kind==VoxelGameplayActors::EKind::Boat || Actor.Kind==VoxelGameplayActors::EKind::Glider) && Player->Vehicle!=Actor.Id) return false;
    }
    for(const auto& Player:StagedPlayers)
        if(Player.Vehicle.IsValid() && !StagedActors.ContainsByPredicate([&](const auto& A){return A.Id==Player.Vehicle && A.Owner==Player.Id &&
            (A.Kind==VoxelGameplayActors::EKind::Boat || A.Kind==VoxelGameplayActors::EKind::Glider);})) return false;
    return true;
}
struct FSession { bool Ready=false, Failed=false, Final=false, SavePending=false; double SaveElapsed=0; FGuid WorldId=FGuid::NewGuid(); };
TMap<TWeakObjectPtr<UWorld>, FSession> Sessions;
struct FCleanup
{
    FDelegateHandle Handle;
    FDelegateHandle TickHandle;
    FCleanup()
    {
        Handle=FWorldDelegates::OnPreWorldFinishDestroy.AddLambda([](UWorld* W){Sessions.Remove(W);});
        TickHandle=FWorldDelegates::OnWorldTickEnd.AddLambda([](UWorld* W,ELevelTick,float Delta){TickAutosave(W,Delta);});
    }
    ~FCleanup() { FWorldDelegates::OnPreWorldFinishDestroy.Remove(Handle); FWorldDelegates::OnWorldTickEnd.Remove(TickHandle); }
} Cleanup;
}
bool Ready(UWorld* W) { const auto S=Detail::Sessions.Find(W); return S && S->Ready && !S->Failed && !VoxelDetachedPersistence::IsLoading(W) && !VoxelDetachedPersistence::HasLoadFailure(W); }
bool Failed(UWorld* W) { const auto S=Detail::Sessions.Find(W); return (S && S->Failed) || (W && VoxelDetachedPersistence::HasLoadFailure(W)); }
FGuid WorldId(UWorld* W) { const auto S=Detail::Sessions.Find(W); return S?S->WorldId:FGuid(); }
void Fail(UWorld* W) { auto& S=Detail::Sessions.FindOrAdd(W); S.Failed=true; S.Ready=false; }
bool FinalSaveAttempted(UWorld* W) { const auto S=Detail::Sessions.Find(W); return S && S->Final; }
void NotifySaved(const UWorld* W)
{
    if (auto S=Detail::Sessions.Find(const_cast<UWorld*>(W))) S->SaveElapsed=0;
}
void TickAutosave(UWorld* W,float GameplaySeconds)
{
    if (!W || !W->HasBegunPlay() || W->IsPaused() || W->GetNetMode()==NM_Client || !Ready(W) ||
        !FMath::IsFinite(GameplaySeconds) || GameplaySeconds<=0) return;
    auto& S=Detail::Sessions.FindChecked(W);
    for(auto It=W->GetPlayerControllerIterator();It;++It)
        if(auto PC=Cast<AVoxelEarthPlayerController>(It->Get()))
            if(PC->IsLocalController() && VoxelPlayerRecords::BindHost(PC)) VoxelPlayerRecords::ApplyPawn(PC);
    if (S.Final) return;
    S.SaveElapsed=FMath::Min(300.0,S.SaveElapsed+double(GameplaySeconds));
    if (S.SaveElapsed<300 || S.SavePending || VoxelSaveJobs::IsBusy() || VoxelSave::GetActiveSlug(W).IsEmpty()) return;
    auto Terrain=W->GetSubsystem<UVoxelWorldSubsystem>();
    if (!Terrain) return;
    S.SavePending=true;
    const FGuid Id=S.WorldId; TWeakObjectPtr<UWorld> Weak=W;
    const bool Admitted=VoxelSave::WriteActiveAsync(*Terrain,[Weak,Id](bool Success){
        auto State=Detail::Sessions.Find(Weak);
        if (!Weak.IsValid() || !State || State->WorldId!=Id) return;
        State->SavePending=false; State->SaveElapsed=Success?0:270;
    });
    if (!Admitted) { S.SavePending=false; S.SaveElapsed=270; }
}

bool Capture(UWorld* W, VoxelCheckpointStore::FSimulationPayload& Out)
{
    check(IsInGameThread()); Out={};
    if (!W || W->GetNetMode()==NM_Client || !Ready(W)) return false;
    auto Water=W->GetSubsystem<UVoxelWaterSubsystem>();
    auto Sky=W->GetSubsystem<UVoxelSkySubsystem>();
    double Epoch=0,Rate=0,Day=0,Year=0,Remainder=0; bool Implicit=false;
    if (!Water || !Sky || !Sky->CaptureClock(Epoch,Rate,Day,Year) ||
        !Water->CaptureCheckpoint(Out.Water,Out.Hydrology,Remainder,Implicit)) return false;
    auto Json=MakeShared<FJsonObject>();
    Json->SetNumberField(TEXT("version"),1);
    Json->SetNumberField(TEXT("epoch"),Epoch);
    Json->SetNumberField(TEXT("rate"),Rate);
    Json->SetNumberField(TEXT("daySeconds"),Day);
    Json->SetNumberField(TEXT("daysPerYear"),Year);
    Json->SetNumberField(TEXT("waterRemainder"),Remainder);
    Json->SetBoolField(TEXT("implicitOcean"),Implicit);
    FString Text;
    if (!FJsonSerializer::Serialize(Json,TJsonWriterFactory<>::Create(&Text))) return false;
    FTCHARToUTF8 Utf8(*Text); Out.Clock.Append(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length());
    auto Agents=W->GetSubsystem<UVoxelAgentSubsystem>(); TArray<uint8> AgentBytes;
    if (!Agents || !Agents->CaptureCheckpoint(AgentBytes)) return false;
    auto Gameplay=MakeShared<FJsonObject>();
    Gameplay->SetNumberField(TEXT("version"),3);
    Gameplay->SetStringField(TEXT("worldId"),Detail::Sessions.FindChecked(W).WorldId.ToString(EGuidFormats::Digits));
    Gameplay->SetNumberField(TEXT("agentSchema"),1);
    Gameplay->SetStringField(TEXT("agents"),FBase64::Encode(AgentBytes));
    TArray<uint8> PlayerBytes;
    if(!VoxelPlayerRecords::Capture(W,PlayerBytes)) return false;
    Gameplay->SetStringField(TEXT("players"),FBase64::Encode(PlayerBytes));
    TArray<uint8> ActorBytes;
    if(!VoxelGameplayActors::Capture(W,ActorBytes)) return false;
    TArray<VoxelPlayerRecords::FRecord> Players; TArray<VoxelGameplayActors::FRecord> Actors;
    if(!VoxelPlayerRecords::Decode(PlayerBytes,Players) || !VoxelGameplayActors::Decode(ActorBytes,Actors) ||
        !Detail::Relationships(Players,Actors)) return false;
    Gameplay->SetStringField(TEXT("actors"),FBase64::Encode(ActorBytes));
    FString GameplayText;
    if (!FJsonSerializer::Serialize(Gameplay,TJsonWriterFactory<>::Create(&GameplayText))) return false;
    FTCHARToUTF8 GameplayUtf8(*GameplayText);
    Out.Gameplay.Append(reinterpret_cast<const uint8*>(GameplayUtf8.Get()),GameplayUtf8.Length());
    return true;
}

bool Restore(UWorld* W,const FString& Logical,const VoxelCheckpointStore::FResolved* Checkpoint)
{
    check(IsInGameThread());
    if (!W || W->GetNetMode()==NM_Client) return false;
    auto Water=W->GetSubsystem<UVoxelWaterSubsystem>();
    auto Sky=W->GetSubsystem<UVoxelSkySubsystem>();
    if (!Water || !Sky) return false;
    auto Agents=W->GetSubsystem<UVoxelAgentSubsystem>();
    TArray<uint8> AgentBytes,PlayerBytes,ActorBytes; FGuid WorldId=FGuid::NewGuid();
    if (Checkpoint && !Checkpoint->GameplayPath.IsEmpty())
    {
        FString Text,Id,Encoded; TSharedPtr<FJsonObject> Json; double Version=0,AgentSchema=0;
        if (!Agents || Agents->GetAgentCount()!=0 || IFileManager::Get().FileSize(*Checkpoint->GameplayPath)>64*1024*1024 ||
            !FFileHelper::LoadFileToString(Text,*Checkpoint->GameplayPath) ||
            !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Json) || !Json ||
            !Json->TryGetNumberField(TEXT("version"),Version) || (Version!=1 && Version!=2 && Version!=3) || Json->Values.Num()!=(Version==1?4:Version==2?5:6) ||
            !Json->TryGetNumberField(TEXT("agentSchema"),AgentSchema) || AgentSchema!=1 ||
            !Json->TryGetStringField(TEXT("worldId"),Id) || !FGuid::ParseExact(Id,EGuidFormats::Digits,WorldId) || !WorldId.IsValid() ||
            !Json->TryGetStringField(TEXT("agents"),Encoded) || Encoded.Len()>1024*1024 || !FBase64::Decode(Encoded,AgentBytes)) return false;
        TArray<FVoxelAgent> Staged;
        if (!UVoxelAgentSubsystem::DecodeCheckpoint(AgentBytes,Staged)) return false;
        TArray<VoxelPlayerRecords::FRecord> StagedPlayers;
        if(Version>=2)
        {
            FString Players;
            if(!Json->TryGetStringField(TEXT("players"),Players) || Players.Len()>24*1024*1024 || !FBase64::Decode(Players,PlayerBytes)) return false;
            if(!VoxelPlayerRecords::Decode(PlayerBytes,StagedPlayers)) return false;
        }
        if(Version==3)
        {
            FString Actors; TArray<VoxelGameplayActors::FRecord> StagedActors;
            if(!Json->TryGetStringField(TEXT("actors"),Actors) || Actors.Len()>24*1024*1024 || !FBase64::Decode(Actors,ActorBytes) ||
                !VoxelGameplayActors::Decode(ActorBytes,StagedActors) || !VoxelGameplayActors::ValidateContent(StagedActors)) return false;
            if(!Detail::Relationships(StagedPlayers,StagedActors)) return false;
        }
    }
    if (Checkpoint && Checkpoint->bSimulation)
    {
        TArray<uint8> WaterBytes,HydroBytes; FString Clock;
        if (!FFileHelper::LoadFileToArray(WaterBytes,*Checkpoint->WaterPath) ||
            !FFileHelper::LoadFileToArray(HydroBytes,*Checkpoint->HydrologyPath) ||
            !FFileHelper::LoadFileToString(Clock,*Checkpoint->ClockPath)) return false;
        TSharedPtr<FJsonObject> Json;
        double Version=0,Epoch=0,Rate=0,Day=0,Year=0,Remainder=0; bool Implicit=false;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Clock),Json) || !Json ||
            !Json->TryGetNumberField(TEXT("version"),Version) || Version!=1 ||
            !Json->TryGetNumberField(TEXT("epoch"),Epoch) || (!FMath::IsFinite(Epoch) || FMath::Abs(Epoch)>1e15) ||
            !Json->TryGetNumberField(TEXT("rate"),Rate) || (!FMath::IsFinite(Rate) || FMath::Abs(Rate)>1000000) ||
            !Json->TryGetNumberField(TEXT("daySeconds"),Day) || (!FMath::IsFinite(Day) || Day<1 || Day>1000000000) ||
            !Json->TryGetNumberField(TEXT("daysPerYear"),Year) || (!FMath::IsFinite(Year) || Year<1 || Year>1000000) ||
            !Json->TryGetNumberField(TEXT("waterRemainder"),Remainder) ||
            !Json->TryGetBoolField(TEXT("implicitOcean"),Implicit)) return false;
        if (!Water->RestoreCheckpoint(WaterBytes,HydroBytes,Remainder,Implicit)) return false;
        if (!Sky->RestoreClock(Epoch,Rate,Day,Year)) return false; // No wall-clock catch-up.
    }
    else if (Checkpoint)
    {
        // Seed files can only be associated with the default legacy slot.
        auto Terrain=W->GetSubsystem<UVoxelWorldSubsystem>();
        const FString Legacy=FPaths::ProjectSavedDir()/TEXT("VoxelWorlds")/FString::Printf(TEXT("%llu.vxlog"),(unsigned long long)Terrain->GetSeed());
        if (FPaths::IsSamePath(Logical,Legacy))
        {
            if (!Water->RestoreLegacyCheckpoint()) return false;
        }
        else UE_LOG(LogVoxelEarth,Warning,TEXT("Legacy named save contains no bound water or clock state; initializing those domains without importing another slot's seed files."));
    }
    if (!AgentBytes.IsEmpty() && !Agents->RestoreCheckpoint(AgentBytes)) return false;
    if (!PlayerBytes.IsEmpty() && !VoxelPlayerRecords::Restore(W,PlayerBytes)) return false;
    if (!ActorBytes.IsEmpty() && !VoxelGameplayActors::Restore(W,ActorBytes)) return false;
    auto& S=Detail::Sessions.FindOrAdd(W); S.WorldId=WorldId; S.Ready=true; S.Failed=false;
    return true;
}

void SaveBeforeTearDown(UWorld* W)
{
    if (!W || !W->HasBegunPlay() || W->GetNetMode()==NM_Client || FinalSaveAttempted(W)) return;
    Detail::Sessions.FindOrAdd(W).Final=true;
    if (!Ready(W)) return;
    auto Terrain=W->GetSubsystem<UVoxelWorldSubsystem>();
    const FString Slug=VoxelSave::GetActiveSlug(W);
    const bool Success=Terrain && (Slug.IsEmpty()?Terrain->SaveWorld():VoxelSave::WriteActive(*Terrain));
    UE_LOG(LogVoxelEarth,Log,TEXT("Session checkpoint before teardown success=%d"),Success);
}
}
