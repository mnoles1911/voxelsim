#include "VoxelSessionCheckpoint.h"
#include "VoxelWaterSubsystem.h"
#include "VoxelSkySubsystem.h"
#include "VoxelWorldSubsystem.h"
#include "VoxelSaveLibrary.h"
#include "VoxelDetachedPersistence.h"
#include "VoxelEarth.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace VoxelSessionCheckpoint
{
namespace Detail
{
struct FSession { bool Ready=false, Failed=false, Final=false; };
TMap<TWeakObjectPtr<UWorld>, FSession> Sessions;
struct FCleanup
{
    FDelegateHandle Handle;
    FCleanup() { Handle=FWorldDelegates::OnPreWorldFinishDestroy.AddLambda([](UWorld* W){Sessions.Remove(W);}); }
    ~FCleanup() { FWorldDelegates::OnPreWorldFinishDestroy.Remove(Handle); }
} Cleanup;
}
bool Ready(UWorld* W) { const auto S=Detail::Sessions.Find(W); return S && S->Ready && !S->Failed && !VoxelDetachedPersistence::IsLoading(W) && !VoxelDetachedPersistence::HasLoadFailure(W); }
bool Failed(UWorld* W) { const auto S=Detail::Sessions.Find(W); return (S && S->Failed) || (W && VoxelDetachedPersistence::HasLoadFailure(W)); }
void Fail(UWorld* W) { auto& S=Detail::Sessions.FindOrAdd(W); S.Failed=true; S.Ready=false; }
bool FinalSaveAttempted(UWorld* W) { const auto S=Detail::Sessions.Find(W); return S && S->Final; }

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
    return true;
}

bool Restore(UWorld* W,const FString& Logical,const VoxelCheckpointStore::FResolved* Checkpoint)
{
    check(IsInGameThread());
    if (!W || W->GetNetMode()==NM_Client) return false;
    auto Water=W->GetSubsystem<UVoxelWaterSubsystem>();
    auto Sky=W->GetSubsystem<UVoxelSkySubsystem>();
    if (!Water || !Sky) return false;
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
    auto& S=Detail::Sessions.FindOrAdd(W); S.Ready=true; S.Failed=false;
    return true;
}

void SaveBeforeTearDown(UWorld* W)
{
    if (!W || !W->HasBegunPlay() || W->GetNetMode()==NM_Client || FinalSaveAttempted(W)) return;
    Detail::Sessions.FindOrAdd(W).Final=true;
    if (!Ready(W)) return;
    auto Terrain=W->GetSubsystem<UVoxelWorldSubsystem>();
    const FString Slug=VoxelSave::GetActiveSlug();
    const bool Success=Terrain && (Slug.IsEmpty()?Terrain->SaveWorld():Terrain->SaveWorldToPath(VoxelSave::WorldLogPath(Slug)));
    UE_LOG(LogVoxelEarth,Log,TEXT("Session checkpoint before teardown success=%d"),Success);
}
}
