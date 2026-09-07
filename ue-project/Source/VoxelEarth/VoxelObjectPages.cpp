#include "VoxelObjectPages.h"
#include "VoxelEarth.h"
#include "Async/Async.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "Misc/Crc.h"
#include "Misc/Compression.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace VoxelObjectPages
{
using namespace VoxelObjects;
using FGeometry = VoxelObjects::FGeometry;
namespace {
constexpr int32 MaxBytes=512*1024*1024;
struct FResult { bool Ok=false,Loading=false;FGuid Id;uint64 Revision=0;VoxelObjects::FGeometry Original,Loaded;FGeometryPage Page; };
struct FState { FString Directory;int32 Cursor=0;TArray<TFuture<FResult>> Jobs;TSet<FGuid> Pending;TMap<FGuid,double> RetryAfter; };
TMap<TWeakObjectPtr<UWorld>,TUniquePtr<FState>> States;
}
bool Read(const FGeometryPage& Page,VoxelObjects::FGeometry& Geometry)
{
    check(!IsInGameThread());if(!Page.IsValid())return false;
    const int64 FileSize=IFileManager::Get().FileSize(*Page.Path);if(FileSize<20||FileSize>MaxBytes+20)return false;
    TArray<uint8> File;if(!FFileHelper::LoadFileToArray(File,*Page.Path))return false;
    FMemoryReader R(File);uint32 Magic=0,Version=0,Crc=0;int32 Size=0,Packed=0;R<<Magic<<Version<<Size<<Crc<<Packed;
    if(R.IsError()||Magic!=0x56504745||Version!=1||Size!=Page.Bytes||Crc!=Page.Crc||Packed!=File.Num()-20||Packed<=0)return false;
    auto Data=MakeShared<TArray<uint8>,ESPMode::ThreadSafe>();Data->SetNumUninitialized(Size);
    if(!FCompression::UncompressMemory(NAME_Zlib,Data->GetData(),int64(Size),File.GetData()+20,int64(Packed)))return false;
    if(FCrc::MemCrc32(Data->GetData(),Size)!=Page.Crc||FMD5::HashBytes(Data->GetData(),Size)!=Page.Hash)return false;
    Geometry=Data;return true;
}
bool Write(const FString& Directory,const VoxelObjects::FGeometry& Geometry,FGeometryPage& Page)
{
    check(!IsInGameThread());if(!Geometry||Geometry->IsEmpty()||Geometry->Num()>MaxBytes)return false;
    FGeometryPage Result;Result.Bytes=Geometry->Num();Result.Crc=FCrc::MemCrc32(Geometry->GetData(),Result.Bytes);
    Result.Hash=FMD5::HashBytes(Geometry->GetData(),Result.Bytes);Result.Path=Directory/(Result.Hash+TEXT(".vpage"));
    VoxelObjects::FGeometry Existing;if(Read(Result,Existing)){Page=MoveTemp(Result);return true;}
    int32 PackedSize=FCompression::CompressMemoryBound(NAME_Zlib,Result.Bytes);TArray<uint8> Packed;Packed.SetNumUninitialized(PackedSize);
    if(!FCompression::CompressMemory(NAME_Zlib,Packed.GetData(),PackedSize,Geometry->GetData(),Result.Bytes,COMPRESS_BiasSpeed))return false;
    TArray<uint8> File;FMemoryWriter W(File);uint32 Magic=0x56504745,Version=1;W<<Magic<<Version<<Result.Bytes<<Result.Crc<<PackedSize;W.Serialize(Packed.GetData(),PackedSize);
    if(W.IsError()||!IFileManager::Get().MakeDirectory(*Directory,true))return false;
    const FString Temp=Result.Path+TEXT(".")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".tmp");
    TUniquePtr<FArchive> Out(IFileManager::Get().CreateFileWriter(*Temp));if(!Out)return false;
    Out->Serialize(File.GetData(),File.Num());Out->Flush();const bool Ok=!Out->IsError()&&Out->Close();Out.Reset();
    if(!Ok||!IFileManager::Get().Move(*Result.Path,*Temp,true,true))return false;
    // Verify the published bytes before the only resident copy can be released.
    VoxelObjects::FGeometry Verified;if(!Read(Result,Verified))return false;Page=MoveTemp(Result);return true;
}
bool Hydrate(FEntry& E)
{
    if(E.Geometry||E.Residency==EResidency::Tombstone)return true;
    // Reject missing backing data before entering the worker-only disk reader.
    if(!E.Page.IsValid())return false;
    VoxelObjects::FGeometry Loaded;if(!Read(E.Page,Loaded))return false;E.Geometry=MoveTemp(Loaded);return true;
}
bool PublishWrite(FRegistry& Registry,const FGuid& Id,uint64 Revision,const VoxelObjects::FGeometry& Original,const FGeometryPage& Page)
{
    check(IsInGameThread());auto E=Registry.Find(Id);
    if(!E||E->Residency!=EResidency::Dormant||E->GeometryRevision!=Revision||!Original||E->Geometry!=Original||!Page.IsValid())return false;
    E->Page=Page;E->Geometry.Reset();return true;
}
bool PublishRead(FRegistry& Registry,const FGuid& Id,uint64 Revision,const FGeometryPage& Page,const VoxelObjects::FGeometry& Loaded)
{
    check(IsInGameThread());auto E=Registry.Find(Id);
    if(!E||E->Residency!=EResidency::Dormant||E->GeometryRevision!=Revision||E->Geometry||E->Page.Path!=Page.Path||!Loaded)return false;
    E->Geometry=Loaded;return true;
}
namespace {
void Apply(UWorld* World,FState& S,FResult&& Result)
{
    S.Pending.Remove(Result.Id);auto Registry=VoxelObjects::Find(World);auto E=Registry?Registry->Find(Result.Id):nullptr;
    if(!Result.Ok){S.RetryAfter.Add(Result.Id,FPlatformTime::Seconds()+10.);UE_LOG(LogVoxelEarth,Warning,TEXT("ObjectPages IO failed id=%s; last usable state retained"),*Result.Id.ToString());return;}
    if(!E)return;
    if(Result.Loading)PublishRead(*Registry,Result.Id,Result.Revision,Result.Page,Result.Loaded);
    else PublishWrite(*Registry,Result.Id,Result.Revision,Result.Original,Result.Page);
}
void Finish(UWorld* W,FState& S,bool Wait)
{
    for(int32 I=S.Jobs.Num()-1;I>=0;--I)if(Wait||S.Jobs[I].IsReady()){auto Result=S.Jobs[I].Get();S.Jobs.RemoveAtSwap(I);Apply(W,S,MoveTemp(Result));}
}
}
void Tick(UWorld* W,const TArray<FView>& Views)
{
    check(IsInGameThread());if(!W||W->GetNetMode()==NM_Client||W->bIsTearingDown)return;
    auto Registry=VoxelObjects::Find(W);if(!Registry)return;
    auto& Slot=States.FindOrAdd(W);if(!Slot){Slot=MakeUnique<FState>();Slot->Directory=FPaths::ProjectSavedDir()/TEXT("ObjectPages")/FGuid::NewGuid().ToString(EGuidFormats::Digits);}
    auto& S=*Slot;Finish(W,S,false);if(S.Jobs.Num()>=2)return;
    for(const auto& E:Registry->SnapshotSlice(S.Cursor,128)){
        if(S.Jobs.Num()>=2)break;
        if(E.Residency!=EResidency::Dormant||S.Pending.Contains(E.Id))continue;
        if(const auto Retry=S.RetryAfter.Find(E.Id))if(*Retry>FPlatformTime::Seconds())continue;
        const bool Near=E.bActivePhysics||FRegistry::ShouldLoad(E,Views,16000.);
        const bool Loading=!E.Geometry&&E.Page.IsValid()&&Near;
        if(!Loading&&(!E.Geometry||Near||E.bActivePhysics||E.bPinned||Views.IsEmpty()||!FRegistry::ShouldEvict(E,Views,20000.)))continue;
        S.Pending.Add(E.Id);const auto Region=FRegistry::RegionFor(E.Transform.GetLocation());
        const FString Directory=S.Directory/FString::Printf(TEXT("%d_%d_%d"),Region.X,Region.Y,Region.Z);
        S.Jobs.Add(Async(EAsyncExecution::ThreadPool,[E,Directory,Loading](){FResult R;R.Id=E.Id;R.Revision=E.GeometryRevision;R.Loading=Loading;R.Page=E.Page;R.Original=E.Geometry;
            R.Ok=Loading?Read(E.Page,R.Loaded):Write(Directory,E.Geometry,R.Page);return R;}));
    }
}
void Drain(UWorld* W){check(IsInGameThread());if(auto S=States.Find(W))Finish(W,**S,true);}
void Forget(UWorld* W){check(IsInGameThread());Drain(W);States.Remove(W);}
namespace {
struct FCleanup { FDelegateHandle Handle;FCleanup(){Handle=FWorldDelegates::OnPreWorldFinishDestroy.AddStatic(&Forget);}~FCleanup(){FWorldDelegates::OnPreWorldFinishDestroy.Remove(Handle);} } Cleanup;
}
}
