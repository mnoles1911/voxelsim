#include "VoxelPlayerRecords.h"
#include "VoxelDurableFile.h"
#include "VoxelEarthPlayerController.h"
#include "VoxelWorldSubsystem.h"
#include "VoxelEarthFlyPawn.h"
#include "VoxelGameplayActors.h"
#include "VoxelItem.h"
#include "VoxelSaveLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "VoxelCharacterMovement.h"
#include "VoxelSessionCheckpoint.h"
#include "Engine/World.h"
#include "Engine/NetConnection.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Hash/Blake3.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace VoxelPlayerRecords
{
namespace Detail
{
struct FState
{
    TArray<FRecord> Records;
    TMap<TWeakObjectPtr<const AVoxelEarthPlayerController>,FGuid> Bindings;
    TMap<TWeakObjectPtr<const AVoxelEarthPlayerController>,FGuid> Pending;
    TSet<FGuid> AppliedPawns;
};
TMap<TWeakObjectPtr<UWorld>,FState> States;
bool Hex64(const FString& Text)
{
    if(Text.Len()!=64) return false;
    for(TCHAR C:Text) if(!FChar::IsHexDigit(C)) return false;
    return true;
}
FString HashToken(const FString& Token)
{
    FTCHARToUTF8 Utf8(*Token);
    return LexToString(FBlake3::HashBuffer(Utf8.Get(),Utf8.Length()));
}
bool Identity(UWorld* W,const FString& Token,FGuid& Id,FString& Issued)
{
    const FGuid WorldId=VoxelSessionCheckpoint::WorldId(W);
    if(!WorldId.IsValid() || (!Token.IsEmpty() && !Hex64(Token))) return false;
    const FString Realm=WorldId.ToString(EGuidFormats::Digits);
    FSystemWideCriticalSection Lock(TEXT("VoxelPlayerIdentity-")+Realm);
    if(!Lock.IsValid()) return false;
    const FString Path=FPaths::ProjectSavedDir()/TEXT("PlayerIdentity")/(Realm+TEXT(".json"));
    auto Root=MakeShared<FJsonObject>(); auto Identities=MakeShared<FJsonObject>();
    bool Dirty=false;
    if(IFileManager::Get().FileExists(*Path))
    {
        FString Text; TSharedPtr<FJsonObject> Parsed; double Version=0;
        const TSharedPtr<FJsonObject>* Entries=nullptr;
        if(IFileManager::Get().FileSize(*Path)>2*1024*1024 || !FFileHelper::LoadFileToString(Text,*Path) ||
            !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Parsed) || !Parsed ||
            !Parsed->TryGetNumberField(TEXT("version"),Version) || Version!=1 || !Parsed->TryGetObjectField(TEXT("identities"),Entries) || !Entries || !Entries->IsValid()) return false;
        Identities=Entries->ToSharedRef();
    }
    if(Identities->Values.Num()>10000) return false;
    TSet<FGuid> UniqueIds;
    for(const auto& Pair:Identities->Values)
    {
        FString Stored; FGuid Parsed;
        if(!Hex64(FString(*Pair.Key)) || !Pair.Value->TryGetString(Stored) || !FGuid::ParseExact(Stored,EGuidFormats::Digits,Parsed) ||
            !Parsed.IsValid() || UniqueIds.Contains(Parsed)) return false;
        UniqueIds.Add(Parsed);
    }
    // Checkpoint rollback must not revoke credentials issued after that generation.
    // Conversely, a restored checkpoint can reconstruct a missing identity index.
    for(const auto& Record:States.FindOrAdd(W).Records)
    {
        if(Record.Host) continue;
        if(!Hex64(Record.CredentialHash)) return false;
        FString Existing;
        if(Identities->TryGetStringField(Record.CredentialHash,Existing))
        {
            if(Existing!=Record.Id.ToString(EGuidFormats::Digits)) return false;
        }
        else
        {
            if(UniqueIds.Contains(Record.Id)) return false;
            Identities->SetStringField(Record.CredentialHash,Record.Id.ToString(EGuidFormats::Digits));
            UniqueIds.Add(Record.Id); Dirty=true;
        }
    }
    FString Key;
    if(Token.IsEmpty())
    {
        if(Identities->Values.Num()>=10000) return false;
        Issued=FGuid::NewGuid().ToString(EGuidFormats::Digits)+FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Key=HashToken(Issued); Id=FGuid::NewGuid();
        Identities->SetStringField(Key,Id.ToString(EGuidFormats::Digits)); Dirty=true;
    }
    else
    {
        Key=HashToken(Token); FString Stored;
        if(!Identities->TryGetStringField(Key,Stored) || !FGuid::ParseExact(Stored,EGuidFormats::Digits,Id)) return false;
    }
    if(!Dirty) return true;
    Root->SetNumberField(TEXT("version"),1); Root->SetObjectField(TEXT("identities"),Identities);
    FString Text; if(!FJsonSerializer::Serialize(Root,TJsonWriterFactory<>::Create(&Text))) return false;
    if(!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true)) return false;
    const FString Temp=Path+TEXT(".")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".tmp");
    FTCHARToUTF8 Utf8(*Text);
    {
        TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Temp));
        if(!File || !File->Write(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length()) || !File->Flush(true)) return false;
    }
    return VoxelDurableFile::Publish(Temp,Path,true);
}
struct FCleanup
{
    FDelegateHandle Handle=FWorldDelegates::OnPreWorldFinishDestroy.AddLambda([](UWorld* W){States.Remove(W);});
    ~FCleanup(){FWorldDelegates::OnPreWorldFinishDestroy.Remove(Handle);}
} Cleanup;
FRecord* Find(AVoxelEarthPlayerController* PC)
{
    auto State=PC?States.Find(PC->GetWorld()):nullptr;
    const auto Id=State?State->Bindings.Find(PC):nullptr;
    return Id?State->Records.FindByPredicate([&](const FRecord& R){return R.Id==*Id;}):nullptr;
}
bool Update(AVoxelEarthPlayerController* PC)
{
    auto Record=Find(PC); auto Inventory=PC?PC->GetInventory():nullptr;
    if (!Record || !Inventory || !Inventory->IsInitialized()) return false;
    Record->Slots=Inventory->CaptureSlots(); Record->Selected=Inventory->GetSelectedSlot();
    Record->Vehicle=VoxelGameplayActors::VehicleId(PC);
    if (auto Pawn=VoxelGameplayActors::PlayerPawn(PC))
    {
        auto PlayerPawn=Cast<AVoxelEarthFlyPawn>(Pawn);
        if(!PlayerPawn || !PlayerPawn->CaptureMotion(Record->Motion)) return false;
        Record->HasPawn=true; Record->Position=Record->Vehicle.IsValid()?PC->GetPawn()->GetActorLocation():Pawn->GetActorLocation(); Record->Velocity=Record->Motion.Velocity;
        Record->Rotation=PC->GetControlRotation().GetNormalized();
    }
    return true;
}
TArray<TSharedPtr<FJsonValue>> Vector(const FVector& V)
{
    return {MakeShared<FJsonValueNumber>(V.X),MakeShared<FJsonValueNumber>(V.Y),MakeShared<FJsonValueNumber>(V.Z)};
}
bool Vector(const FJsonObject& Json,const TCHAR* Name,FVector& V,double Limit)
{
    const TArray<TSharedPtr<FJsonValue>>* A=nullptr;
    if (!Json.TryGetArrayField(Name,A) || A->Num()!=3) return false;
    for(int32 I=0;I<3;++I) if (!(*A)[I]->TryGetNumber(V[I]) || !FMath::IsFinite(V[I]) || FMath::Abs(V[I])>Limit) return false;
    return true;
}
}
bool IsBound(const AVoxelEarthPlayerController* PC)
{
    const auto State=PC?Detail::States.Find(PC->GetWorld()):nullptr;
    return State && State->Bindings.Contains(PC);
}
FGuid PlayerId(const AVoxelEarthPlayerController* PC)
{
    auto State=PC?Detail::States.Find(PC->GetWorld()):nullptr;
    const auto Id=State?State->Bindings.Find(PC):nullptr;
    return Id?*Id:FGuid();
}
int32 Credit(UWorld* W,const FGuid& Id,FName Item,int32 Count)
{
    if(!W || W->GetNetMode()==NM_Client || Count<=0) return 0;
    auto State=Detail::States.Find(W); const auto Def=FVoxelItemRegistry::Find(Item);
    if(!State || !Def) return 0;
    auto Record=State->Records.FindByPredicate([&](const FRecord& R){return R.Id==Id;});
    if(!Record) return 0;
    for(const auto& Binding:State->Bindings)
        if(Binding.Value==Id && Binding.Key.IsValid())
        {
            auto PC=const_cast<AVoxelEarthPlayerController*>(Binding.Key.Get());
            return PC->GetInventory()?PC->GetInventory()->TryAddItem(Item,Count):0;
        }
    int32 Remaining=Count;
    for(auto& Slot:Record->Slots)
        if(Slot.ItemId==Item && Slot.Count<Def->MaxStack)
        { const int32 N=FMath::Min(Remaining,Def->MaxStack-Slot.Count); Slot.Count+=N; Remaining-=N; }
    for(auto& Slot:Record->Slots)
        if(Remaining>0 && Slot.Count==0)
        { const int32 N=FMath::Min(Remaining,Def->MaxStack); Slot.ItemId=Item; Slot.Count=N; Remaining-=N; }
    return Count-Remaining;
}
APawn* ActivePawn(UWorld* W,const FGuid& Id)
{
    const auto State=Detail::States.Find(W);
    if(State) for(const auto& Binding:State->Bindings)
        if(Binding.Value==Id && Binding.Key.IsValid()) return Binding.Key->GetPawn();
    return nullptr;
}
bool BindHost(AVoxelEarthPlayerController* PC)
{
    if (!PC || !PC->HasAuthority() || !PC->IsLocalController() || !PC->GetInventory()) return false;
    if (IsBound(PC)) return true;
    auto& State=Detail::States.FindOrAdd(PC->GetWorld());
    auto Record=State.Records.FindByPredicate([](const FRecord& R){return R.Host;});
    if (!Record)
    {
        FRecord New; New.Id=FGuid::NewGuid(); New.Host=true;
        PC->GetInventory()->InitializeNewPlayer();
        New.Slots=PC->GetInventory()->CaptureSlots(); New.Selected=PC->GetInventory()->GetSelectedSlot();
        State.Records.Add(MoveTemp(New)); Record=&State.Records.Last();
    }
    for(const auto& Binding:State.Bindings) if(Binding.Key.IsValid() && Binding.Value==Record->Id) return false;
    if (!PC->GetInventory()->RestoreSlots(Record->Slots,Record->Selected)) return false;
    State.Bindings.Add(PC,Record->Id); return true;
}
bool ApplyPawn(AVoxelEarthPlayerController* PC)
{
    auto Record=Detail::Find(PC); auto Pawn=PC?PC->GetPawn():nullptr;
    if (!Record || !Pawn) return false;
    auto& State=Detail::States.FindChecked(PC->GetWorld());
    if(State.AppliedPawns.Contains(Record->Id)) return true;
    if(Record->HasPawn)
    {
        FVector Position=Record->Position;
        if (auto Terrain=Record->Motion.Walk?PC->GetWorld()->GetSubsystem<UVoxelWorldSubsystem>():nullptr)
        {
            const double Surface=Terrain->GetSurfaceHeightUU(Position.X,Position.Y);
            if(FMath::IsFinite(Surface) && Position.Z<Surface+100) Position.Z=Surface+200;
        }
        Pawn->SetActorLocation(Position,false,nullptr,ETeleportType::TeleportPhysics);
        PC->SetControlRotation(Record->Rotation);
        auto PlayerPawn=Cast<AVoxelEarthFlyPawn>(Pawn);
        if(!PlayerPawn || !PlayerPawn->RestoreMotion(Record->Motion)) return false;
    }
    if(Record->Vehicle.IsValid() && !VoxelGameplayActors::BindVehicle(PC,Record->Vehicle)) return false;
    State.AppliedPawns.Add(Record->Id); return true;
}
void VehicleDestroyed(UWorld* W,const FGuid& Vehicle,const FVector& Position)
{
    if(!W || W->GetNetMode()==NM_Client || !Vehicle.IsValid()) return;
    if(auto State=Detail::States.Find(W)) for(auto& Record:State->Records)
        if(Record.Vehicle==Vehicle)
        {
            Record.Vehicle.Invalidate();
            Record.Position=Position+FVector(0,0,200);
            Record.Velocity=FVector::ZeroVector;
            Record.Motion.Velocity=FVector::ZeroVector;
            Record.Motion.Walk=false;
            Record.HasPawn=true;
        }
}
void Disconnect(AVoxelEarthPlayerController* PC)
{
    Detail::Update(PC);
    VoxelGameplayActors::ReleaseVehicle(PC);
    if (PC) if(auto State=Detail::States.Find(PC->GetWorld()))
    {
        if(auto Id=State->Bindings.Find(PC)) State->AppliedPawns.Remove(*Id);
        State->Bindings.Remove(PC);
        State->Pending.Remove(PC);
    }
}
bool Capture(UWorld* W,TArray<uint8>& Bytes)
{
    Bytes.Reset(); if(!W || W->GetNetMode()==NM_Client) return false;
    for(auto It=W->GetPlayerControllerIterator();It;++It)
    {
        auto PC=Cast<AVoxelEarthPlayerController>(It->Get());
        if(!PC) continue;
        if(!IsBound(PC))
        {
            if(!PC->IsLocalController()) continue; // Unadmitted connections own no live gameplay state.
            if(!BindHost(PC)) return false;
        }
    }
    auto& State=Detail::States.FindOrAdd(W);
    // Admission owns this set. A bound controller can precede registration in
    // the world's iterator or outlive removal during disconnect/travel.
    for(const auto& Binding:State.Bindings)
        if(auto PC=const_cast<AVoxelEarthPlayerController*>(Binding.Key.Get()))
            if(PC->GetWorld()!=W || !Detail::Update(PC)) return false;
    auto Root=MakeShared<FJsonObject>(); Root->SetNumberField(TEXT("version"),3);
    TArray<TSharedPtr<FJsonValue>> Players;
    for(const auto& R:State.Records)
    {
        auto J=MakeShared<FJsonObject>(); J->SetStringField(TEXT("id"),R.Id.ToString(EGuidFormats::Digits));
        J->SetStringField(TEXT("vehicle"),R.Vehicle.ToString(EGuidFormats::Digits));
        J->SetBoolField(TEXT("host"),R.Host); J->SetBoolField(TEXT("hasPawn"),R.HasPawn); J->SetStringField(TEXT("credentialHash"),R.CredentialHash);
        J->SetNumberField(TEXT("selected"),R.Selected); J->SetArrayField(TEXT("position"),Detail::Vector(R.Position));
        J->SetArrayField(TEXT("velocity"),Detail::Vector(R.Velocity)); J->SetArrayField(TEXT("rotation"),Detail::Vector(FVector(R.Rotation.Pitch,R.Rotation.Yaw,R.Rotation.Roll)));
        auto Motion=MakeShared<FJsonObject>();
        Motion->SetBoolField(TEXT("walk"),R.Motion.Walk); Motion->SetBoolField(TEXT("crouched"),R.Motion.Crouched); Motion->SetBoolField(TEXT("jumpHeld"),R.Motion.JumpHeld);
        Motion->SetNumberField(TEXT("walkSpeed"),R.Motion.WalkSpeed); Motion->SetNumberField(TEXT("flySpeed"),R.Motion.FlySpeed);
        Motion->SetNumberField(TEXT("groundAge"),R.Motion.GroundAge); Motion->SetNumberField(TEXT("jumpRemaining"),R.Motion.JumpRemaining);
        J->SetObjectField(TEXT("motion"),Motion);
        TArray<TSharedPtr<FJsonValue>> Slots;
        for(const auto& S:R.Slots)
        {
            auto Slot=MakeShared<FJsonObject>(); Slot->SetStringField(TEXT("item"),S.ItemId.IsNone()?FString():S.ItemId.ToString());
            Slot->SetNumberField(TEXT("count"),S.Count); Slots.Add(MakeShared<FJsonValueObject>(Slot));
        }
        J->SetArrayField(TEXT("slots"),Slots); Players.Add(MakeShared<FJsonValueObject>(J));
    }
    Root->SetArrayField(TEXT("players"),Players); FString Text;
    if(!FJsonSerializer::Serialize(Root,TJsonWriterFactory<>::Create(&Text))) return false;
    FTCHARToUTF8 Utf8(*Text); Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length());
    TArray<FRecord> Checked; return Decode(Bytes,Checked);
}
bool Decode(const TArray<uint8>& Bytes,TArray<FRecord>& Records)
{
    Records.Reset(); if(Bytes.IsEmpty() || Bytes.Num()>16*1024*1024) return false;
    FString Text; FFileHelper::BufferToString(Text,Bytes.GetData(),Bytes.Num());
    TSharedPtr<FJsonObject> Root; double Version=0; const TArray<TSharedPtr<FJsonValue>>* Players=nullptr;
    if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Root) || !Root || Root->Values.Num()!=2 ||
        !Root->TryGetNumberField(TEXT("version"),Version) || (Version!=1 && Version!=2 && Version!=3) || !Root->TryGetArrayField(TEXT("players"),Players) || Players->Num()>10000) return false;
    TArray<FRecord> Parsed; TSet<FGuid> Ids; TSet<FString> Credentials; bool Host=false;
    for(const auto& Value:*Players)
    {
        const TSharedPtr<FJsonObject>* J=nullptr; FString Id; FRecord R; double Selected=0; FVector Rotation;
        const TArray<TSharedPtr<FJsonValue>>* Slots=nullptr;
        if(!Value->TryGetObject(J) || !J || !J->IsValid() || (*J)->Values.Num()!=(Version==1?9:Version==2?10:11) ||
            !(*J)->TryGetStringField(TEXT("id"),Id) || !FGuid::ParseExact(Id,EGuidFormats::Digits,R.Id) || !R.Id.IsValid() || Ids.Contains(R.Id) ||
            !(*J)->TryGetBoolField(TEXT("host"),R.Host) || (R.Host && Host) || !(*J)->TryGetBoolField(TEXT("hasPawn"),R.HasPawn) ||
            !(*J)->TryGetStringField(TEXT("credentialHash"),R.CredentialHash) || (!R.CredentialHash.IsEmpty() && R.CredentialHash.Len()!=64) ||
            !(*J)->TryGetNumberField(TEXT("selected"),Selected) || !FMath::IsFinite(Selected) || Selected<0 || Selected>63 || Selected!=FMath::FloorToDouble(Selected) ||
            !Detail::Vector(**J,TEXT("position"),R.Position,1e12) || !Detail::Vector(**J,TEXT("velocity"),R.Velocity,1e7) ||
            !Detail::Vector(**J,TEXT("rotation"),Rotation,360) || !(*J)->TryGetArrayField(TEXT("slots"),Slots) || Slots->Num()<1 || Slots->Num()>64) return false;
        R.Selected=int32(Selected); R.Rotation=FRotator(Rotation.X,Rotation.Y,Rotation.Z);
        R.Motion.Velocity=R.Velocity;
        if(Version==3)
        {
            FString Vehicle;
            if(!(*J)->TryGetStringField(TEXT("vehicle"),Vehicle) || !FGuid::ParseExact(Vehicle,EGuidFormats::Digits,R.Vehicle) || (R.Vehicle.IsValid() && !R.HasPawn)) return false;
        }
        if(Version>=2)
        {
            const TSharedPtr<FJsonObject>* M=nullptr; double WalkSpeed=0,FlySpeed=0;
            if(!(*J)->TryGetObjectField(TEXT("motion"),M) || !M || !M->IsValid() || (*M)->Values.Num()!=7 ||
                !(*M)->TryGetBoolField(TEXT("walk"),R.Motion.Walk) || !(*M)->TryGetBoolField(TEXT("crouched"),R.Motion.Crouched) || !(*M)->TryGetBoolField(TEXT("jumpHeld"),R.Motion.JumpHeld) ||
                !(*M)->TryGetNumberField(TEXT("walkSpeed"),WalkSpeed) || !FMath::IsFinite(WalkSpeed) || WalkSpeed<0 || WalkSpeed>=UVoxelCharacterMovementComponent::GetSpeedTierCount() || WalkSpeed!=FMath::FloorToDouble(WalkSpeed) ||
                !(*M)->TryGetNumberField(TEXT("flySpeed"),FlySpeed) || !FMath::IsFinite(FlySpeed) || FlySpeed<0 || FlySpeed>=AVoxelEarthFlyPawn::GetFlySpeedStepCount() || FlySpeed!=FMath::FloorToDouble(FlySpeed) ||
                !(*M)->TryGetNumberField(TEXT("groundAge"),R.Motion.GroundAge) || !FMath::IsFinite(R.Motion.GroundAge) || R.Motion.GroundAge<0 || R.Motion.GroundAge>1e9 ||
                !(*M)->TryGetNumberField(TEXT("jumpRemaining"),R.Motion.JumpRemaining) || !FMath::IsFinite(R.Motion.JumpRemaining) || R.Motion.JumpRemaining<0 || R.Motion.JumpRemaining>1) return false;
            R.Motion.WalkSpeed=int32(WalkSpeed); R.Motion.FlySpeed=int32(FlySpeed);
        }
        if((R.Host && !R.CredentialHash.IsEmpty()) || (!R.Host && (!Detail::Hex64(R.CredentialHash) || Credentials.Contains(R.CredentialHash)))) return false;
        if(!R.Host) Credentials.Add(R.CredentialHash);
        for(const auto& SlotValue:*Slots)
        {
            const TSharedPtr<FJsonObject>* Slot=nullptr; FString Item; double Count=0;
            if(!SlotValue->TryGetObject(Slot) || !Slot || !Slot->IsValid() || (*Slot)->Values.Num()!=2 ||
                !(*Slot)->TryGetStringField(TEXT("item"),Item) || Item.Len()>128 || !(*Slot)->TryGetNumberField(TEXT("count"),Count) ||
                !FMath::IsFinite(Count) || Count<0 || Count>MAX_int32 || Count!=FMath::FloorToDouble(Count)) return false;
            FVoxelInventorySlot S; S.ItemId=Item.IsEmpty()?NAME_None:FName(*Item); S.Count=int32(Count); R.Slots.Add(S);
        }
        if(!UVoxelInventoryComponent::ValidateSnapshot(R.Slots,R.Selected)) return false;
        Ids.Add(R.Id); Host|=R.Host; Parsed.Add(MoveTemp(R));
    }
    Records=MoveTemp(Parsed); return true;
}
bool Restore(UWorld* W,const TArray<uint8>& Bytes)
{
    if(!W || W->GetNetMode()==NM_Client) return false;
    auto& State=Detail::States.FindOrAdd(W);
    if(!State.Bindings.IsEmpty() || !State.Records.IsEmpty()) return false;
    TArray<FRecord> Staged; if(!Decode(Bytes,Staged)) return false;
    State.Records=MoveTemp(Staged); return true;
}
bool Authenticate(AVoxelEarthPlayerController* PC,const FString& Token,FGuid& Id,FString& IssuedToken)
{
    Id.Invalidate(); IssuedToken.Reset();
    if(!PC || !PC->HasAuthority() || PC->IsLocalController() || !VoxelSessionCheckpoint::Ready(PC->GetWorld()) ||
        !PC->GetNetConnection() || !PC->GetNetConnection()->IsEncryptionEnabled() || !PC->GetInventory()) return false;
    auto& State=Detail::States.FindOrAdd(PC->GetWorld());
    if(State.Bindings.Contains(PC) || State.Pending.Contains(PC)) return false;
    if(!Detail::Identity(PC->GetWorld(),Token,Id,IssuedToken)) return false;
    for(const auto& Binding:State.Bindings) if(Binding.Key.IsValid() && Binding.Value==Id) return false;
    for(const auto& Pending:State.Pending) if(Pending.Key.IsValid() && Pending.Value==Id) return false;
    auto Record=State.Records.FindByPredicate([&](const FRecord& R){return R.Id==Id;});
    if(!Record)
    {
        FRecord New; New.Id=Id; New.CredentialHash=Detail::HashToken(Token.IsEmpty()?IssuedToken:Token);
        PC->GetInventory()->InitializeNewPlayer(); New.Slots=PC->GetInventory()->CaptureSlots(); New.Selected=PC->GetInventory()->GetSelectedSlot();
        State.Records.Add(MoveTemp(New));
    }
    State.Pending.Add(PC,Id); return true;
}
bool Confirm(AVoxelEarthPlayerController* PC,const FGuid& Id)
{
    if(!PC || !PC->HasAuthority() || !PC->GetNetConnection() || !PC->GetNetConnection()->IsEncryptionEnabled()) return false;
    auto State=Detail::States.Find(PC->GetWorld()); const auto Pending=State?State->Pending.Find(PC):nullptr;
    if(!Pending || *Pending!=Id) return false;
    auto Record=State->Records.FindByPredicate([&](const FRecord& R){return R.Id==Id;});
    if(!Record || !PC->GetInventory()->RestoreSlots(Record->Slots,Record->Selected)) return false;
    State->Bindings.Add(PC,Id); State->Pending.Remove(PC); return true;
}
namespace Detail
{
FString CredentialPath(AVoxelEarthPlayerController* PC,const FGuid& Id)
{
    if(!PC || !PC->IsLocalController() || PC->HasAuthority() || !Id.IsValid() || !PC->GetNetConnection() ||
        !PC->GetNetConnection()->IsEncryptionEnabled()) return FString();
    const FString Endpoint=PC->GetWorld()->URL.Host.ToLower()+TEXT(":")+FString::FromInt(PC->GetWorld()->URL.Port);
    FString Root=FPaths::ProjectSavedDir()/TEXT("PlayerProfiles"),Profile;
    if(FParse::Value(FCommandLine::Get(),TEXT("VoxelPlayerProfile="),Profile))
    {
        if(!VoxelSave::IsValidSlug(Profile)) return FString();
        Root/=Profile;
    }
    return Root/(HashToken(Endpoint+TEXT("/")+Id.ToString(EGuidFormats::Digits))+TEXT(".credential"));
}
bool ReadProfile(const FString& Path,FString& Token)
{
    TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
    ANSICHAR Bytes[65]={};
    if(!File || File->Size()!=64 || !File->Read(reinterpret_cast<uint8*>(Bytes),64)) return false;
    Token=UTF8_TO_TCHAR(Bytes); return Hex64(Token);
}
}
bool ReadCredential(AVoxelEarthPlayerController* PC,const FGuid& Id,FString& Token)
{
    Token.Reset(); const FString Path=Detail::CredentialPath(PC,Id); if(Path.IsEmpty()) return false;
    if(!IFileManager::Get().FileExists(*Path)) return true;
    return Detail::ReadProfile(Path,Token);
}
bool WriteCredential(AVoxelEarthPlayerController* PC,const FGuid& Id,const FString& Token)
{
    const FString Path=Detail::CredentialPath(PC,Id); if(Path.IsEmpty() || !Detail::Hex64(Token)) return false;
    FSystemWideCriticalSection Lock(TEXT("VoxelPlayerProfile-")+Detail::HashToken(FPaths::ConvertRelativePathToFull(Path).ToLower()));
    if(!Lock.IsValid()) return false;
    if(IFileManager::Get().FileExists(*Path))
    {
        FString Existing;
        return Detail::ReadProfile(Path,Existing) && Existing==Token;
    }
    if(!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true)) return false;
    const FString Temp=Path+TEXT(".")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".tmp");
    FTCHARToUTF8 Utf8(*Token);
    {
        TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Temp));
        if(!File || !File->Write(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length()) || !File->Flush(true)) return false;
    }
    return VoxelDurableFile::Publish(Temp,Path,true);
}
}
