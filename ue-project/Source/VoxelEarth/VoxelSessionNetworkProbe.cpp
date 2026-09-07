#include "VoxelSessionNetworkProbe.h"
#include "VoxelPlayerRecords.h"
#include "VoxelEarthPlayerController.h"
#include "VoxelEarthFlyPawn.h"
#include "VoxelSessionCheckpoint.h"
#include "VoxelWorldSubsystem.h"
#include "VoxelItem.h"
#include "VoxelEarth.h"
#include "Engine/World.h"
#include "Engine/NetConnection.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

void UVoxelSessionNetworkProbe::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    bEnabled=FParse::Value(FCommandLine::Get(),TEXT("VoxelSessionVerify="),Role) &&
        FParse::Value(FCommandLine::Get(),TEXT("VoxelSessionVerifyDir="),Directory) &&
        (Role==TEXT("write") || Role==TEXT("read") || Role==TEXT("client"));
    FParse::Value(FCommandLine::Get(),TEXT("VoxelSessionVerifyPlayers="),ExpectedPlayers);
    bEnabled=bEnabled && ExpectedPlayers>=1 && ExpectedPlayers<=16 && !Directory.IsEmpty();
}
void UVoxelSessionNetworkProbe::Tick(float DeltaSeconds)
{
    Elapsed+=DeltaSeconds;
    if(Elapsed<NextPoll) return;
    NextPoll=Elapsed+0.25;
    if(FPaths::FileExists(Directory/TEXT("stop"))) { bEnabled=false; FPlatformMisc::RequestExit(false); return; }
    if(Role==TEXT("client") || bReported || !VoxelSessionCheckpoint::Ready(GetWorld())) return;
    TArray<AVoxelEarthPlayerController*> Players;
    for(auto It=GetWorld()->GetPlayerControllerIterator();It;++It)
        if(auto PC=Cast<AVoxelEarthPlayerController>(It->Get()))
            if(!PC->IsLocalController() && VoxelPlayerRecords::IsBound(PC)) Players.Add(PC);
    if(Players.Num()!=ExpectedPlayers) return;
    Players.Sort([](const AVoxelEarthPlayerController& A,const AVoxelEarthPlayerController& B){return VoxelPlayerRecords::PlayerId(&A).ToString()<VoxelPlayerRecords::PlayerId(&B).ToString();});
    for(auto PC:Players) if(!Cast<AVoxelEarthFlyPawn>(PC->GetPawn()) || !PC->GetInventory()->IsInitialized()) return;
    bool Passed=true; TArray<TSharedPtr<FJsonValue>> Rows;
    for(int32 I=0;I<Players.Num();++I)
    {
        auto PC=Players[I]; auto Inventory=PC->GetInventory(); auto Pawn=CastChecked<AVoxelEarthFlyPawn>(PC->GetPawn());
        const FVector Position(-10000.0-I*1000.0,20000.0+I*1000.0,500000.0);
        const int32 Selection=(I+1)%Inventory->NumSlots();
        if(Role==TEXT("write"))
        {
            TArray<FVoxelInventorySlot> Slots; Slots.SetNum(Inventory->NumSlots());
            Slots[0].ItemId=VoxelItemIds::ThrowCube30(); Slots[0].Count=I+1;
            Passed=Inventory->RestoreSlots(Slots,Selection) && Passed;
            FVoxelPlayerMotion Motion; Motion.Walk=false; Motion.FlySpeed=1;
            Passed=Pawn->RestoreMotion(Motion) && Passed; Pawn->SetActorLocation(Position);
        }
        FVoxelPlayerMotion Motion; Passed=Pawn->CaptureMotion(Motion) && Passed;
        const bool Encrypted=PC->GetNetConnection() && PC->GetNetConnection()->IsEncryptionEnabled();
        Passed=Encrypted && Inventory->GetSlot(0).Count==I+1 && Inventory->GetSelectedSlot()==Selection &&
            !Motion.Walk && Pawn->GetActorLocation().Equals(Position,0.1) && Passed;
        auto Row=MakeShared<FJsonObject>(); Row->SetStringField(TEXT("id"),VoxelPlayerRecords::PlayerId(PC).ToString(EGuidFormats::Digits));
        Row->SetNumberField(TEXT("count"),Inventory->GetSlot(0).Count); Row->SetNumberField(TEXT("selected"),Inventory->GetSelectedSlot());
        Row->SetBoolField(TEXT("encrypted"),Encrypted); Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    if(Role==TEXT("write")) Passed=GetWorld()->GetSubsystem<UVoxelWorldSubsystem>()->SaveWorld() && Passed;
    auto Root=MakeShared<FJsonObject>(); Root->SetBoolField(TEXT("passed"),Passed); Root->SetStringField(TEXT("phase"),Role);
    Root->SetStringField(TEXT("worldId"),VoxelSessionCheckpoint::WorldId(GetWorld()).ToString(EGuidFormats::Digits)); Root->SetArrayField(TEXT("players"),Rows);
    FString Text; FJsonSerializer::Serialize(Root,TJsonWriterFactory<>::Create(&Text));
    bReported=FFileHelper::SaveStringToFile(Text,*(Directory/TEXT("ready.tmp")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) &&
        IFileManager::Get().Move(*(Directory/TEXT("ready.json")),*(Directory/TEXT("ready.tmp")),true,true);
    UE_LOG(LogVoxelEarth,Log,TEXT("SessionNetVerify %s phase=%s players=%d report=%d"),Passed?TEXT("PASS"):TEXT("FAIL"),*Role,Players.Num(),bReported);
}
