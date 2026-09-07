#include "VoxelSaveJobs.h"
#include "VoxelDetachedPersistence.h"
#include "VoxelSaveGuard.h"
#include "VoxelEarth.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace VoxelSaveJobs
{
struct FResult { bool Success=false; double WorkerMs=0; };
struct FActive
{
    TFuture<FResult> Future;
    TFunction<void(bool)> Completion;
    double CaptureMs=0;
    uint64 StartFrame=0;
};
TUniquePtr<FActive> Active;
FTSTicker::FDelegateHandle PollHandle;

bool WriteAtomic(const FString& Path,const TArray<uint8>& Bytes)
{
    if(VoxelSaveGuard::RefuseWrite(Path,TEXT("SaveAsync")))return false;
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
    const FString Temp=Path+TEXT(".tmp");
    if(!FFileHelper::SaveArrayToFile(Bytes,*Temp))return false;
    return IFileManager::Get().Move(*Path,*Temp,true);
}
FResult Write(FSnapshot&& Snapshot)
{
    check(!IsInGameThread());const double Start=FPlatformTime::Seconds();
    FResult Result;
    if(VoxelSaveGuard::RefuseWrite(Snapshot.TerrainPath,TEXT("SaveAsync")))return Result;
    if(Snapshot.bObjectSnapshot&&!VoxelDetachedPersistence::EncodeSnapshot(Snapshot.Objects,Snapshot.Detached))return Result;
    Result.Success=VoxelDetachedPersistence::WritePayload(Snapshot.TerrainPath,Snapshot.Terrain,Snapshot.Detached)
        &&WriteAtomic(Snapshot.TerrainPath,Snapshot.Terrain);
    if(Result.Success&&!Snapshot.MetadataJson.IsEmpty())
    {
        FTCHARToUTF8 Utf8(*Snapshot.MetadataJson);TArray<uint8> Data;Data.Append(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length());
        Result.Success=WriteAtomic(FPaths::GetPath(Snapshot.TerrainPath)/TEXT("meta.json"),Data);
    }
    Result.WorkerMs=(FPlatformTime::Seconds()-Start)*1000.;return Result;
}
void Complete()
{
    check(IsInGameThread());auto Finished=MoveTemp(Active);
    const FResult Result=Finished->Future.Get();
    UE_LOG(LogVoxelEarth,Log,TEXT("SaveAsync COMPLETE success=%d captureMs=%.3f workerMs=%.3f framesAdvanced=%llu"),
        Result.Success,Finished->CaptureMs,Result.WorkerMs,GFrameCounter-Finished->StartFrame);
    if(Finished->Completion)Finished->Completion(Result.Success);
}
bool IsBusy(){check(IsInGameThread());return Active.IsValid();}
bool Submit(FSnapshot&& Snapshot,TFunction<void(bool)> Completion)
{
    check(IsInGameThread());if(Active)return false;
    if(VoxelSaveGuard::RefuseWrite(Snapshot.TerrainPath,TEXT("SaveAsync")))return false;
    Active=MakeUnique<FActive>();Active->CaptureMs=Snapshot.CaptureMs;Active->StartFrame=GFrameCounter;Active->Completion=MoveTemp(Completion);
    Active->Future=Async(EAsyncExecution::ThreadPool,[Snapshot=MoveTemp(Snapshot)]() mutable{return Write(MoveTemp(Snapshot));});
    if(!PollHandle.IsValid())PollHandle=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float){
        if(Active&&Active->Future.IsReady())Complete();
        if(!Active){PollHandle.Reset();return false;}return true;
    }));
    return true;
}
void Drain()
{
    check(IsInGameThread());
    if(Active)UE_LOG(LogVoxelEarth,Log,TEXT("SaveAsync DRAIN waiting for pending save"));
    while(Active)Complete();
    if(PollHandle.IsValid()){FTSTicker::GetCoreTicker().RemoveTicker(PollHandle);PollHandle.Reset();}
}
}
