#include "VoxelCheckpointStore.h"
#include "VoxelDurableFile.h"
#include "VoxelDetachedPersistence.h"
#include "VoxelSaveGuard.h"
#include "VoxelEarth.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace VoxelCheckpointStore
{
namespace Detail
{
constexpr int64 MaxPayload = 512ll * 1024 * 1024;
FString Root(const FString& Path) { return Path + TEXT(".checkpoints"); }
bool Read(const FString& Path, TArray<uint8>& Bytes, int64 Limit = MaxPayload)
{
    Bytes.Reset();
    TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
    if (!File) return false;
    const int64 Size=File->Size();
    if (Size<0 || Size>Limit || Size>MAX_int32) return false;
    Bytes.SetNumUninitialized(int32(Size));
    if (!File->Read(Bytes.GetData(),Size) || File->Size()!=Size) { Bytes.Reset(); return false; }
    return true;
}
FString Hash(const TArray<uint8>& Bytes) { return FMD5::HashBytes(Bytes.GetData(), Bytes.Num()); }
bool Write(const FString& Path, const TArray<uint8>& Bytes)
{
    TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Path));
    return File && File->Write(Bytes.GetData(), Bytes.Num()) && File->Flush(true);
}
TArray<uint8> Utf8(const FString& String)
{
    FTCHARToUTF8 Text(*String); TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Text.Get()), Text.Length()); return Bytes;
}
bool Publish(const FString& From, const FString& To)
{
    // A unique commit record is the publication point. No replacement/delete window.
    return VoxelDurableFile::Publish(From,To,false);
}
void Commits(const FString& Path, TArray<FString>& Names)
{
    IFileManager::Get().FindFiles(Names, *(Root(Path) / TEXT("*.commit")), true, false);
    Names.Sort([](const FString& A, const FString& B) { return A > B; });
}
// Only generated names may be used as relative paths from an untrusted manifest.
bool GenerationName(const FString& Name)
{
    if (Name.Len() != 53 || Name[20] != TCHAR('-')) return false;
    for (int32 I=0; I<20; ++I) if (!FChar::IsDigit(Name[I])) return false;
    FGuid Id; return FGuid::ParseExact(Name.Mid(21), EGuidFormats::Digits, Id);
}
enum class EValidation { Valid, Damaged, Unsupported };
EValidation Validate(const FString& Logical, const FString& CommitName, FResolved& Out)
{
    const FString Generation = FPaths::GetBaseFilename(CommitName);
    if (!GenerationName(Generation)) return EValidation::Damaged;
    const FString Directory = Root(Logical) / Generation;
    TArray<uint8> ManifestBytes;
    FString Expected;
    if (!Read(Directory / TEXT("manifest.json"), ManifestBytes, 64*1024) ||
        IFileManager::Get().FileSize(*(Root(Logical)/CommitName)) != 32 ||
        !FFileHelper::LoadFileToString(Expected, *(Root(Logical)/CommitName)) || Expected != Hash(ManifestBytes))
        return EValidation::Damaged;
    FString Json; FFileHelper::BufferToString(Json, ManifestBytes.GetData(), ManifestBytes.Num());
    TSharedPtr<FJsonObject> Manifest;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Manifest) || !Manifest) return EValidation::Damaged;
    double WireVersion=0;
    if (!Manifest->TryGetNumberField(TEXT("version"), WireVersion) || !FMath::IsFinite(WireVersion)) return EValidation::Damaged;
    if (WireVersion != 1 && WireVersion != 2 && WireVersion != 3) return EValidation::Unsupported;
    const int32 Version=static_cast<int32>(WireVersion);
    FString RecordedGeneration;
    if (!Manifest->TryGetStringField(TEXT("generation"), RecordedGeneration) || RecordedGeneration != Generation) return EValidation::Damaged;
    const TArray<TSharedPtr<FJsonValue>>* Files=nullptr;
    if (!Manifest->TryGetArrayField(TEXT("files"), Files) || Files->Num()<1 || Files->Num()>(Version>=2?(Version==3?7:6):3)) return EValidation::Damaged;
    TSet<FString> Seen;
    FString Metadata;
    for (const auto& Value : *Files)
    {
        const TSharedPtr<FJsonObject>* Entry=nullptr;
        FString Name, Digest; double Size=0;
        if (!Value->TryGetObject(Entry) || !Entry || !Entry->IsValid() ||
            !(*Entry)->TryGetStringField(TEXT("name"),Name) || !(*Entry)->TryGetStringField(TEXT("hash"),Digest) ||
            !(*Entry)->TryGetNumberField(TEXT("bytes"),Size)) return EValidation::Damaged;
        FGuid SidecarHash;
        const bool Sidecar = Name.StartsWith(TEXT("world.vxlog.detached-")) && Name.EndsWith(TEXT(".bin")) &&
            Name.Len()==57 && FGuid::ParseExact(Name.Mid(21,32),EGuidFormats::Digits,SidecarHash);
        const bool SimulationFile=Version>=2 && (Name==TEXT("water.vxwater") || Name==TEXT("hydro.vxhydro") || Name==TEXT("simulation.bin") || (Version==3 && Name==TEXT("gameplay.json")));
        if ((Name!=TEXT("world.vxlog") && Name!=TEXT("meta.json") && !Sidecar && !SimulationFile) || Seen.Contains(Name)) return EValidation::Damaged;
        Seen.Add(Name);
        TArray<uint8> Bytes;
        const int64 Limit=Name==TEXT("meta.json")?1024*1024:(Name==TEXT("simulation.bin")?64*1024:(Name==TEXT("gameplay.json")?64ll*1024*1024:MaxPayload));
        if (!Read(Directory/Name,Bytes,Limit) ||
            double(Bytes.Num())!=Size || Hash(Bytes)!=Digest || (SimulationFile && Bytes.IsEmpty())) return EValidation::Damaged;
        if (Name==TEXT("meta.json")) Metadata=Directory/Name;
    }
    if (!Seen.Contains(TEXT("world.vxlog"))) return EValidation::Damaged;
    // The required sidecar name is derived from the exact terrain payload.
    TArray<uint8> Terrain;
    if (!Read(Directory/TEXT("world.vxlog"),Terrain)) return EValidation::Damaged;
    FString DetachedName;
    if (!Manifest->TryGetStringField(TEXT("detached"),DetachedName)) return EValidation::Damaged;
    if (!DetachedName.IsEmpty() && (DetachedName!=TEXT("world.vxlog.detached-")+Hash(Terrain)+TEXT(".bin") || !Seen.Contains(DetachedName))) return EValidation::Damaged;
    if (Version>=2 && (!Seen.Contains(TEXT("water.vxwater")) || !Seen.Contains(TEXT("hydro.vxhydro")) || !Seen.Contains(TEXT("simulation.bin")))) return EValidation::Damaged;
    if (Version==3 && !Seen.Contains(TEXT("gameplay.json"))) return EValidation::Damaged;
    if (Seen.Num()!=1+int32(!Metadata.IsEmpty())+int32(!DetachedName.IsEmpty())+(Version>=2?3:0)+(Version==3?1:0)) return EValidation::Damaged;
    Out.TerrainPath=Directory/TEXT("world.vxlog"); Out.MetadataPath=Metadata; Out.bCheckpoint=true;
    Out.bSimulation=Version>=2;
    if (Version==3) Out.GameplayPath=Directory/TEXT("gameplay.json");
    if (Out.bSimulation)
    {
        Out.WaterPath=Directory/TEXT("water.vxwater");
        Out.HydrologyPath=Directory/TEXT("hydro.vxhydro");
        Out.ClockPath=Directory/TEXT("simulation.bin");
    }
    return EValidation::Valid;
}
}

bool Resolve(const FString& Logical, FResolved& Out)
{
    if (Logical.IsEmpty()) { Out={}; return false; }
    Out={}; TArray<FString> Names; Detail::Commits(Logical,Names);
    for (int32 I=0; I<Names.Num(); ++I)
    {
        const auto Status=Detail::Validate(Logical,Names[I],Out);
        if (Status==Detail::EValidation::Unsupported) return false;
        if (Status==Detail::EValidation::Valid)
        {
            Out.bRecovered=I>0;
            if (Out.bRecovered) UE_LOG(LogVoxelEarth,Warning,TEXT("Checkpoint recovered previous generation: %s"),*Out.TerrainPath);
            return true;
        }
    }
    if (!Names.IsEmpty()) return false;
    Out.TerrainPath=Logical; Out.MetadataPath=FPaths::GetPath(Logical)/TEXT("meta.json"); return true;
}

bool Commit(const FString& Logical,const TArray<uint8>& Terrain,const TArray<uint8>& Detached,const FString& MetadataJson,int32 FailAfterStage,const FSimulationPayload* Simulation)
{
    if (Logical.IsEmpty()) return false;
    if (Terrain.IsEmpty() || Terrain.Num()>Detail::MaxPayload || Detached.Num()>Detail::MaxPayload || VoxelSaveGuard::RefuseWrite(Logical,TEXT("Checkpoint"))) return false;
    if (Simulation && (Simulation->Water.IsEmpty() || Simulation->Hydrology.IsEmpty() || Simulation->Clock.IsEmpty() ||
        Simulation->Water.Num()>Detail::MaxPayload || Simulation->Hydrology.Num()>Detail::MaxPayload || Simulation->Clock.Num()>64*1024 || Simulation->Gameplay.Num()>64*1024*1024)) return false;
    FString Canonical=FPaths::ConvertRelativePathToFull(Logical); FPaths::NormalizeFilename(Canonical); FPaths::CollapseRelativeDirectories(Canonical);
#if PLATFORM_WINDOWS
    Canonical.ToLowerInline();
#endif
    FSystemWideCriticalSection Lock(TEXT("VoxelCheckpoint-")+FMD5::HashAnsiString(*Canonical));
    if (!Lock.IsValid()) return false;
    FResolved Previous;
    if (!Resolve(Logical,Previous) || VoxelSaveGuard::RefuseWrite(Previous.TerrainPath,TEXT("Checkpoint"))) return false;
    // An older caller must not replace a full simulation checkpoint with terrain only.
    if (Previous.bSimulation && !Simulation) return false;
    if (!Previous.GameplayPath.IsEmpty() && (!Simulation || Simulation->Gameplay.IsEmpty())) return false;
    FString Metadata=MetadataJson;
    if (Metadata.IsEmpty() && FPaths::FileExists(Previous.MetadataPath) && !FFileHelper::LoadFileToString(Metadata,*Previous.MetadataPath)) return false;
    const auto MetadataBytes=Detail::Utf8(Metadata);
    if (MetadataBytes.Num()>1024*1024) return false;
    if (!Metadata.IsEmpty())
    {
        TSharedPtr<FJsonObject> ParsedMetadata;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Metadata),ParsedMetadata) || !ParsedMetadata) return false;
    }
    TArray<FString> Names; Detail::Commits(Logical,Names);
    uint64 Sequence=1;
    if (!Names.IsEmpty())
    {
        if (!Detail::GenerationName(FPaths::GetBaseFilename(Names[0]))) return false;
        Sequence=FCString::Strtoui64(*Names[0].Left(20),nullptr,10)+1;
        if (!Sequence) return false;
    }
    const FString Generation=FString::Printf(TEXT("%020llu-%s"),(unsigned long long)Sequence,*FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Directory=Detail::Root(Logical)/Generation;
    if (!IFileManager::Get().MakeDirectory(*Directory,true)) return false;
    int32 Stage=0;
    auto Stop=[&](){return Stage++==FailAfterStage;};
    if (Stop()) return false;
    if (!Detail::Write(Directory/TEXT("world.vxlog"),Terrain) || Stop()) return false;
    if (!VoxelDetachedPersistence::WritePayload(Directory/TEXT("world.vxlog"),Terrain,Detached) || Stop()) return false;
    if (!Metadata.IsEmpty() && !Detail::Write(Directory/TEXT("meta.json"),MetadataBytes)) return false;
    if (Stop()) return false;
    TArray<FString> Files{TEXT("world.vxlog")};
    FString DetachedName;
    if (!Detached.IsEmpty()) { DetachedName=TEXT("world.vxlog.detached-")+Detail::Hash(Terrain)+TEXT(".bin"); Files.Add(DetachedName); }
    if (!Metadata.IsEmpty()) Files.Add(TEXT("meta.json"));
    if (Simulation)
    {
        const TCHAR* DomainNames[]={TEXT("water.vxwater"),TEXT("hydro.vxhydro"),TEXT("simulation.bin")};
        const TArray<uint8>* DomainBytes[]={&Simulation->Water,&Simulation->Hydrology,&Simulation->Clock};
        for (int32 I=0; I<3; ++I)
        {
            if (!Detail::Write(Directory/DomainNames[I],*DomainBytes[I]) || Stop()) return false;
            Files.Add(DomainNames[I]);
        }
    }
    if (Simulation && !Simulation->Gameplay.IsEmpty())
    {
        if (!Detail::Write(Directory/TEXT("gameplay.json"),Simulation->Gameplay) || Stop()) return false;
        Files.Add(TEXT("gameplay.json"));
    }
    auto Manifest=MakeShared<FJsonObject>(); Manifest->SetNumberField(TEXT("version"),Simulation?(Simulation->Gameplay.IsEmpty()?2:3):1);
    Manifest->SetStringField(TEXT("generation"),Generation); Manifest->SetStringField(TEXT("detached"),DetachedName);
    TArray<TSharedPtr<FJsonValue>> Entries;
    for (const FString& Name : Files)
    {
        TArray<uint8> Bytes; if (!Detail::Read(Directory/Name,Bytes)) return false;
        // Also fully flush the legacy sidecar writer's output before publication.
        if (!Detail::Write(Directory/Name,Bytes)) return false;
        auto Entry=MakeShared<FJsonObject>(); Entry->SetStringField(TEXT("name"),Name);
        Entry->SetNumberField(TEXT("bytes"),Bytes.Num()); Entry->SetStringField(TEXT("hash"),Detail::Hash(Bytes));
        Entries.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Manifest->SetArrayField(TEXT("files"),Entries); FString Json;
    if (!FJsonSerializer::Serialize(Manifest,TJsonWriterFactory<>::Create(&Json))) return false;
    const auto ManifestBytes=Detail::Utf8(Json);
    if (!Detail::Write(Directory/TEXT("manifest.json"),ManifestBytes) || Stop()) return false;
    const FString Marker=Detail::Root(Logical)/(Generation+TEXT(".commit"));
    if (!Detail::Write(Marker+TEXT(".tmp"),Detail::Utf8(Detail::Hash(ManifestBytes))) || Stop()) return false;
    return Detail::Publish(Marker+TEXT(".tmp"),Marker);
}
}
