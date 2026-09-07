#include "VoxelSessionGameInstance.h"
#include "Engine/NetworkDelegates.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

namespace VoxelTransport
{
bool Read(const TCHAR* Option,const TCHAR* DefaultFile,TSharedPtr<FJsonObject>& Root)
{
    FString Path;
    if(!FParse::Value(FCommandLine::Get(),Option,Path)) Path=FPaths::ProjectSavedDir()/TEXT("Transport")/DefaultFile;
    TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
    if(!File || File->Size()<=0 || File->Size()>1024*1024) return false;
    TArray<uint8> Bytes; Bytes.SetNumUninitialized(int32(File->Size()));
    if(!File->Read(Bytes.GetData(),Bytes.Num())) return false;
    FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),Bytes.Num());
    FString Text(Utf8.Length(),Utf8.Get()); double Version=0;
    return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Root) && Root &&
        Root->TryGetNumberField(TEXT("version"),Version) && Version==1;
}
bool Key(const FString& Text,TArray<uint8>& Bytes)
{
    Bytes.Reset(); if(Text.Len()!=64) return false;
    for(TCHAR C:Text) if(!FChar::IsHexDigit(C)) return false;
    Bytes.SetNumUninitialized(32);
    return HexToBytes(Text,Bytes.GetData())==32;
}
bool Id(const FString& Text)
{
    FGuid Id; return FGuid::ParseExact(Text,EGuidFormats::Digits,Id) && Id.IsValid();
}
}
void UVoxelSessionGameInstance::ReceivedNetworkEncryptionToken(const FString& Token,const FOnEncryptionKeyResponse& Delegate)
{
    FEncryptionKeyResponse Response(EEncryptionResponse::Failure,TEXT("Transport invite refused"));
    TSharedPtr<FJsonObject> Root; const TSharedPtr<FJsonObject>* Keys=nullptr; FString Secret;
    if(VoxelTransport::Id(Token) && VoxelTransport::Read(TEXT("VoxelTransportKeys="),TEXT("server.json"),Root) &&
        Root->Values.Num()==2 && Root->TryGetObjectField(TEXT("keys"),Keys) && Keys && Keys->IsValid() &&
        (*Keys)->Values.Num()<=10000 && (*Keys)->TryGetStringField(Token,Secret) && VoxelTransport::Key(Secret,Response.EncryptionData.Key))
    {
        Response.Response=EEncryptionResponse::Success; Response.ErrorMsg.Empty(); Response.EncryptionData.Identifier=Token;
    }
    Delegate.ExecuteIfBound(Response);
}
void UVoxelSessionGameInstance::ReceivedNetworkEncryptionAck(const FOnEncryptionKeyResponse& Delegate)
{
    FEncryptionKeyResponse Response(EEncryptionResponse::Failure,TEXT("Local transport invite unavailable"));
    TSharedPtr<FJsonObject> Root; FString Id,Secret;
    if(VoxelTransport::Read(TEXT("VoxelTransportInvite="),TEXT("invite.json"),Root) && Root->Values.Num()==3 &&
        Root->TryGetStringField(TEXT("id"),Id) && VoxelTransport::Id(Id) && Root->TryGetStringField(TEXT("key"),Secret) &&
        VoxelTransport::Key(Secret,Response.EncryptionData.Key))
    {
        Response.Response=EEncryptionResponse::Success; Response.ErrorMsg.Empty(); Response.EncryptionData.Identifier=Id;
    }
    Delegate.ExecuteIfBound(Response);
}
EEncryptionFailureAction UVoxelSessionGameInstance::ReceivedNetworkEncryptionFailure(UNetConnection*)
{
    return EEncryptionFailureAction::RejectConnection;
}
