#include "VoxelDetachedPersistence.h"
#include "VoxelDebris.h"
#include "VoxelDebrisLifecycle.h"
#include "VoxelTreeFellingPrototype.h"
#include "VoxelEnvironmentLODPrototype.h"
#include "VoxelSaveGuard.h"
#include "VoxelWorldSubsystem.h"
#include "VoxelSaveJobs.h"
#include "VoxelObjectPages.h"
#include "VoxelPackedTimberMesh.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "VoxelEarth.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/Crc.h"
#include "Misc/Compression.h"
#include "HAL/FileManager.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "TimerManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace VoxelDetachedPersistence
{
struct FRecord { uint8 Kind=0; TArray<uint8> Data; };
struct FSession { TMap<TWeakObjectPtr<AActor>,FRecord> Ending; bool Restored=false,FailedCapture=false; };
TMap<TWeakObjectPtr<UWorld>,FSession> Sessions;
constexpr int32 MaxSnapshot=512*1024*1024;
bool Bytes(FArchive& Ar,TArray<uint8>& Data,int32 Max)
{
    int32 N=Data.Num();Ar<<N;
    if(Ar.IsError()||N<0||N>Max||(Ar.IsLoading()&&int64(N)>Ar.TotalSize()-Ar.Tell())){Ar.SetError();return false;}
    if(Ar.IsLoading())Data.SetNumUninitialized(N);
    if(N)Ar.Serialize(Data.GetData(),N);return !Ar.IsError();
}
bool Eligible(AActor* A)
{
    if(!A||A->ActorHasTag(TEXT("ObjectRestorePending")))return false;
    auto Life=A->FindComponentByClass<UVoxelDebrisLifecycle>();
    if(Life&&Life->CaptureState().Kind==EVoxelDebrisLifetime::Cosmetic)return false;
    return A->IsA<AVoxelDebris>()||A->IsA<AVoxelFallingTimber>()||A->IsA<AVoxelEnvironmentLODPrototype>();
}
bool Capture(AActor* A,FRecord& R)
{
    if(!Eligible(A))return false;
    FMemoryWriter Ar(R.Data);Ar.SetCustomVersion(VoxelPackedTimberMesh::VersionKey,1,NAME_None);
    if(auto Debris=Cast<AVoxelDebris>(A)){R.Kind=1;return Debris->PersistentState(Ar);}
    if(auto Timber=Cast<AVoxelFallingTimber>(A)){R.Kind=2;return Timber->PersistentState(Ar);}
    if(auto Plant=Cast<AVoxelEnvironmentLODPrototype>(A)){R.Kind=3;return Plant->PersistentState(Ar);}
    return false;
}
void SnapshotBeforeTearDown(UWorld* W)
{
    if(!W||W->GetNetMode()==NM_Client)return;
    DrainLoads(W);VoxelObjectPages::Drain(W);VoxelSaveJobs::Drain();
    RefreshObjects(W);
    UE_LOG(LogVoxelEarth,Log,TEXT("ObjectSave teardown snapshot records=%d"),VoxelObjects::Get(W).Num());
}
struct FTeardownHook
{
    FDelegateHandle Handle;
    FTeardownHook(){Handle=FWorldDelegates::OnWorldBeginTearDown.AddStatic(&SnapshotBeforeTearDown);}
    ~FTeardownHook(){FWorldDelegates::OnWorldBeginTearDown.Remove(Handle);}
};
FTeardownHook TeardownHook;
void OnEndPlay(AActor* A,EEndPlayReason::Type Reason)
{
    if(!A||!A->GetWorld()||A->ActorHasTag(TEXT("ObjectRestorePending")))return;
    auto& S=Sessions.FindOrAdd(A->GetWorld());
    if(Reason==EEndPlayReason::Destroyed){
        if(auto R=VoxelObjects::Find(A->GetWorld()))if(auto E=R->Find(A))
            if(E->Residency!=VoxelObjects::EResidency::Evicting)R->Remove(E->Id);
        S.Ending.Remove(A);return;
    }
    if(auto Registry=VoxelObjects::Find(A->GetWorld()))if(Registry->Find(A))return;
    // A promoted mesh can still be owned by its source prototype actor. The
    // world hook snapshots all owners before either actor destroys components.
    if(S.Ending.Contains(A))return;
    FRecord R;if(Capture(A,R))S.Ending.Add(A,MoveTemp(R));else if(Eligible(A)){
        S.FailedCapture=true;UE_LOG(LogVoxelEarth,Error,TEXT("DetachedSave EndPlay capture failed actor=%s reason=%d"),*A->GetName(),int32(Reason));
    }
}
bool HasRestoredPrototypes(UWorld* W){auto S=Sessions.Find(W);return S&&S->Restored;}
FString Sidecar(const FString& Path,const TArray<uint8>& Terrain)
{return Path+TEXT(".detached-")+FMD5::HashBytes(Terrain.GetData(),Terrain.Num())+TEXT(".bin");}
bool CapturePayload(UWorld* W,TArray<uint8>& Payload)
{
    check(IsInGameThread());Payload.Reset();FSnapshot Snapshot;
    if(!CaptureSnapshot(W,Snapshot))return false;
    // Preserve the completed-save contract of this synchronous API, while all
    // page hydration and large encoding copies occur on a worker. Immutable
    // handles/path references outlive the worker even during world teardown.
    struct FEncodedSnapshot { bool Ok=false;TArray<uint8> Bytes; };
    auto Job=Async(EAsyncExecution::ThreadPool,[Snapshot=MoveTemp(Snapshot)](){
        FEncodedSnapshot Result;Result.Ok=EncodeSnapshot(Snapshot,Result.Bytes);return Result;
    });
    auto Result=Job.Get();if(!Result.Ok)return false;Payload=MoveTemp(Result.Bytes);return true;
}

bool WritePayload(const FString& Path,const TArray<uint8>& Terrain,const TArray<uint8>& Payload)
{
    if(Payload.IsEmpty())return true;
    if(Payload.Num()<4||Payload.Num()>MaxSnapshot||VoxelSaveGuard::RefuseWrite(Path,TEXT("DetachedSave")))return false;
    int32 N=0;FMemory::Memcpy(&N,Payload.GetData(),sizeof(N));
    int32 RawSize=Payload.Num(),CompressedSize=FCompression::CompressMemoryBound(NAME_Zlib,RawSize);
    TArray<uint8> Compressed;Compressed.SetNumUninitialized(CompressedSize);
    if(!FCompression::CompressMemory(NAME_Zlib,Compressed.GetData(),CompressedSize,Payload.GetData(),RawSize,COMPRESS_BiasSpeed))return false;
    Compressed.SetNum(CompressedSize);
    TArray<uint8> Out;FMemoryWriter File(Out);uint32 Magic=0x56444252,Version=N==-4?4:3,Crc=FCrc::MemCrc32(Payload.GetData(),Payload.Num());
    File<<Magic<<Version<<Crc<<RawSize;if(!Bytes(File,Compressed,MaxSnapshot))return false;
    if(Version==4&&Payload.Num()>=8)FMemory::Memcpy(&N,Payload.GetData()+4,sizeof(N));
    const FString Dest=Sidecar(Path,Terrain),Temp=Dest+TEXT(".tmp");
    if(VoxelSaveGuard::RefuseWrite(Dest,TEXT("DetachedSave")))return false;
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Dest),true);
    if(!FFileHelper::SaveArrayToFile(Out,*Temp)||!IFileManager::Get().Move(*Dest,*Temp,true,true))return false;
    UE_LOG(LogVoxelEarth,Log,TEXT("DetachedSave wrote actors=%d bytes=%d rawBytes=%d path=%s"),N,Out.Num(),RawSize,*Dest);return true;
}
bool Save(UWorld* W,const FString& Path,const TArray<uint8>& Terrain)
{
    VoxelSaveJobs::Drain();
    if(VoxelSaveGuard::RefuseWrite(Path,TEXT("DetachedSave")))return false;
    TArray<uint8> Payload;return CapturePayload(W,Payload)&&WritePayload(Path,Terrain,Payload);
}
bool ReadPayload(const FString& Path,const TArray<uint8>& Terrain,TArray<uint8>& Payload,uint32& Version)
{
    const FString Src=Sidecar(Path,Terrain);
    if(!FPaths::FileExists(Src))return true; // legacy terrain-only save
    auto Refuse=[&](){VoxelSaveGuard::Quarantine(Src,TEXT("Detached snapshot failed validation; original bytes preserved."));VoxelSaveGuard::Quarantine(Path,TEXT("Matching detached snapshot failed validation."));return false;};
    if(IFileManager::Get().FileSize(*Src)>MaxSnapshot+20)return Refuse();
    TArray<uint8> Raw;if(!FFileHelper::LoadFileToArray(Raw,*Src))return Refuse();
    FMemoryReader File(Raw);uint32 Magic=0,Crc=0;File<<Magic<<Version<<Crc;
    if(Magic!=0x56444252||(Version<1||Version>4))return Refuse();
    if(Version==1){if(!Bytes(File,Payload,MaxSnapshot))return Refuse();}
    else{
        int32 RawSize=0;File<<RawSize;if(File.IsError()||RawSize<4||RawSize>MaxSnapshot)return Refuse();
        TArray<uint8> Compressed;if(!Bytes(File,Compressed,MaxSnapshot))return Refuse();Payload.SetNumUninitialized(RawSize);
        if(!FCompression::UncompressMemory(NAME_Zlib,Payload.GetData(),int64(RawSize),Compressed.GetData(),int64(Compressed.Num())))return Refuse();
    }
    if(File.Tell()!=File.TotalSize()||FCrc::MemCrc32(Payload.GetData(),Payload.Num())!=Crc)return Refuse();
    return true;
}
bool ApplyPayload(UWorld* W,const FString& Path,const TArray<uint8>& Terrain,const TArray<uint8>& Payload,uint32 Version)
{
    if(Payload.IsEmpty())return true;
    const FString Src=Sidecar(Path,Terrain);
    auto Refuse=[&](){VoxelSaveGuard::Quarantine(Src,TEXT("Detached actor snapshot failed validation."));VoxelSaveGuard::Quarantine(Path,TEXT("Matching detached snapshot failed validation."));return false;};
    if(Version==4){FSnapshot Snapshot;if(!DecodeSnapshot(Payload,Snapshot)||!InstallSnapshot(W,MoveTemp(Snapshot)))return Refuse();Sessions.FindOrAdd(W).Restored=true;return true;}
    FMemoryReader Ar(Payload);int32 N=0;Ar<<N;if(N<0||N>4096||Ar.IsError())return Refuse();
    TArray<FRecord> Records;Records.SetNum(N);
    for(auto& R:Records){Ar<<R.Kind;if(R.Kind<1||R.Kind>3||!Bytes(Ar,R.Data,MaxSnapshot))return Refuse();}
    if(Ar.Tell()!=Ar.TotalSize())return Refuse();
    TArray<AActor*> Spawned;
    for(auto& R:Records)
    {
        AActor* A=R.Kind==1?static_cast<AActor*>(W->SpawnActor<AVoxelDebris>()):R.Kind==2?static_cast<AActor*>(W->SpawnActor<AVoxelFallingTimber>()):static_cast<AActor*>(W->SpawnActor<AVoxelEnvironmentLODPrototype>());
        if(!A){for(auto P:Spawned)P->Destroy();return Refuse();}Spawned.Add(A);
        FMemoryReader Reader(R.Data);if(Version>=3)Reader.SetCustomVersion(VoxelPackedTimberMesh::VersionKey,1,NAME_None);bool Ok=false;
        if(auto D=Cast<AVoxelDebris>(A))Ok=D->PersistentState(Reader);
        if(auto T=Cast<AVoxelFallingTimber>(A))Ok=T->PersistentState(Reader);
        if(auto P=Cast<AVoxelEnvironmentLODPrototype>(A))Ok=P->PersistentState(Reader);
        if(!Ok||Reader.IsError()||Reader.Tell()!=Reader.TotalSize()){for(auto P:Spawned)P->Destroy();return Refuse();}
    }
    Sessions.FindOrAdd(W).Restored=true;
    for(auto A:Spawned)if(auto P=Cast<AVoxelEnvironmentLODPrototype>(A))if(P->AssetName==TEXT("temperate-oak")){
        const FVector Location=P->GetActorLocation();TWeakObjectPtr<UWorld> Weak=W;
        W->GetTimerManager().SetTimerForNextTick([Weak,Location](){if(auto World=Weak.Get())VoxelTreeFelling::Prepare(World,Location);});break;
    }
    UE_LOG(LogVoxelEarth,Log,TEXT("DetachedSave restored actors=%d format=%u path=%s"),N,Version,*Src);return true;
}
bool Load(UWorld* W,const FString& Path,const TArray<uint8>& Terrain)
{
    if(!W||W->GetNetMode()==NM_Client)return true;
    TArray<uint8> Payload;uint32 Version=0;
    return ReadPayload(Path,Terrain,Payload,Version)&&ApplyPayload(W,Path,Terrain,Payload,Version);
}
struct FLoadResult { bool Ok=false;uint32 Version=0;TArray<uint8> Payload;FSnapshot Objects; };
struct FLoadJob { FString Path;TArray<uint8> Terrain;TFuture<FLoadResult> Future; };
TMap<TWeakObjectPtr<UWorld>,TSharedPtr<FLoadJob>> Loads;
bool IsLoading(UWorld* W){return Loads.Contains(W);}
bool HasLoadFailure(UWorld* W){auto S=Sessions.Find(W);return S&&S->FailedCapture;}
void DrainLoads(UWorld* W)
{
    auto P=Loads.Find(W);if(!P)return;auto Job=*P;auto Result=Job->Future.Get();Loads.Remove(W);
    bool Ok=Result.Ok;
    if(Ok){if(Result.Version==4)Ok=InstallSnapshot(W,MoveTemp(Result.Objects));else Ok=ApplyPayload(W,Job->Path,Job->Terrain,Result.Payload,Result.Version);}
    if(!Ok){VoxelSaveGuard::Quarantine(Job->Path,TEXT("Asynchronous object load failed; saving refused."));Sessions.FindOrAdd(W).FailedCapture=true;}
    UE_LOG(LogVoxelEarth,Log,TEXT("ObjectLoad async complete success=%d format=%u"),Ok,Result.Version);
}
bool LoadAsync(UWorld* W,const FString& Path,const TArray<uint8>& Terrain)
{
    if(!W||W->GetNetMode()==NM_Client)return true;if(IsLoading(W))return false;
    if(!FPaths::FileExists(Sidecar(Path,Terrain)))return true;
    auto Job=MakeShared<FLoadJob>();Job->Path=Path;Job->Terrain=Terrain;
    Job->Future=Async(EAsyncExecution::ThreadPool,[Path,Terrain](){FLoadResult R;R.Ok=ReadPayload(Path,Terrain,R.Payload,R.Version);if(R.Ok&&R.Version==4){R.Ok=DecodeSnapshot(R.Payload,R.Objects);R.Payload.Reset();}return R;});
    Loads.Add(W,Job);Sessions.FindOrAdd(W).Restored=true;TWeakObjectPtr<UWorld> Weak=W;
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float){
        auto W=Weak.Get();if(!W){Loads.Remove(Weak);return false;}auto J=Loads.Find(W);if(!J)return false;
        if((*J)->Future.IsReady()){DrainLoads(W);return false;}return true;
    }));return true;
}

FAutoConsoleCommandWithWorld Probe(TEXT("voxel.DetachedPersistence.Probe"),TEXT("After 80 game seconds, roundtrip detached actors and prototype edits in an isolated test sidecar."),
    FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* W){
        if(!W||W->GetNetMode()!=NM_Standalone)return;
        FTimerHandle Handle;
        W->GetTimerManager().SetTimer(Handle,FTimerDelegate::CreateLambda([W](){
            const FString Path=FPaths::ProjectSavedDir()/TEXT("Tests/detached-roundtrip.vxlog");
            auto Resource=W->SpawnActor<AVoxelDebris>();TArray<VoxelCoords::FVoxelCoord> Cells;
            for(int32 I=0;I<9;++I)Cells.Add(VoxelCoords::FVoxelCoord{I,0,100000});Resource->InitFromIsland(Cells);
            auto Life=Resource->FindComponentByClass<UVoxelDebrisLifecycle>();FVoxelDebrisLifetimeState TestState;
            TestState.Kind=EVoxelDebrisLifetime::Harvestable;TestState.RemainingSeconds=123.;Life->RestoreState(TestState);
            TArray<uint8> Terrain;
            TArray<AActor*> Before;TArray<FVector> Locations;TArray<FRecord> Originals;
            for(TActorIterator<AActor> It(W);It;++It){FRecord R;if(Capture(*It,R)){Before.Add(*It);Locations.Add(It->GetActorLocation());Originals.Add(MoveTemp(R));}}
            auto Sub=W->GetSubsystem<UVoxelWorldSubsystem>();
            if(Before.IsEmpty()||!Sub||!Sub->SaveWorldToPath(Path)||!FFileHelper::LoadFileToArray(Terrain,*Path)){UE_LOG(LogVoxelEarth,Error,TEXT("DetachedSave PROBE FAIL save"));return;}
            for(auto A:Before)A->Destroy();
            if(!Load(W,Path,Terrain)){UE_LOG(LogVoxelEarth,Error,TEXT("DetachedSave PROBE FAIL load"));return;}
            int32 Count=0,Timber=0,Plants=0;bool Pass=true,TimerMatched=false;
            for(TActorIterator<AActor> It(W);It;++It){FRecord R;if(!Capture(*It,R))continue;++Count;Timber+=R.Kind==2;Plants+=R.Kind==3;
                bool Found=false;for(int32 I=0;I<Locations.Num();++I)if(Originals[I].Kind==R.Kind&&It->GetActorLocation().Equals(Locations[I],.01)){
                    Found=R.Kind==3?R.Data==Originals[I].Data:R.Data.Num()==Originals[I].Data.Num();break;}Pass&=Found;
                if(R.Kind==1){auto L=It->FindComponentByClass<UVoxelDebrisLifecycle>();TimerMatched=L&&FMath::IsNearlyEqual(L->CaptureState().RemainingSeconds,123.,.01);}
            }
            Pass&=Count==Before.Num()&&Timber==2&&Plants==4&&TimerMatched;
            UE_LOG(LogVoxelEarth,Log,TEXT("DetachedSave PROBE %s before=%d after=%d timber=%d plants=%d timerMatched=%d"),Pass?TEXT("PASS"):TEXT("FAIL"),Before.Num(),Count,Timber,Plants,TimerMatched);
            const FString BadPath=Path+TEXT(".corrupt-")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
            TArray<uint8> Corrupt;
            if(FFileHelper::LoadFileToArray(Corrupt,*Sidecar(Path,Terrain))&&Corrupt.Num()>12){
                Corrupt[8]^=1;const bool Written=FFileHelper::SaveArrayToFile(Corrupt,*Sidecar(BadPath,Terrain));
                const bool Rejected=Written&&!Load(W,BadPath,Terrain)&&VoxelSaveGuard::IsQuarantined(BadPath);
                int32 StillPresent=0;for(TActorIterator<AActor> It(W);It;++It)if(Eligible(*It))++StillPresent;
                UE_LOG(LogVoxelEarth,Log,TEXT("DetachedSave CORRUPTION %s actorsUnchanged=%d"),Rejected&&StillPresent==Count?TEXT("PASS"):TEXT("FAIL"),StillPresent==Count);
            }
        }),80.f,false);
    }));

FAutoConsoleCommandWithWorld RestoreCheck(TEXT("voxel.DetachedPersistence.CheckRestored"),TEXT("Verify a fresh-process restore after 30 gameplay seconds."),
    FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* W){
        if(!W||W->GetNetMode()!=NM_Standalone)return;
        FTimerHandle Handle;W->GetTimerManager().SetTimer(Handle,FTimerDelegate::CreateLambda([W](){
            int32 Timber=0,Plants=0,Resource=0;bool CutPreserved=false,TimerPreserved=false;
            for(TActorIterator<AVoxelFallingTimber> It(W);It;++It)++Timber;
            for(TActorIterator<AVoxelEnvironmentLODPrototype> It(W);It;++It){++Plants;if(It->AssetName==TEXT("temperate-oak"))CutPreserved=!It->SolidAt(It->GetActorLocation()+FVector(0,0,600));}
            for(TActorIterator<AVoxelDebris> It(W);It;++It){++Resource;auto L=It->FindComponentByClass<UVoxelDebrisLifecycle>();if(L){auto S=L->CaptureState();TimerPreserved=S.Kind==EVoxelDebrisLifetime::Harvestable&&S.RemainingSeconds>60&&S.RemainingSeconds<123;}}
            const bool Pass=Timber==2&&Plants==4&&Resource==1&&CutPreserved&&TimerPreserved;
            UE_LOG(LogVoxelEarth,Log,TEXT("DetachedSave RESTART %s timber=%d plants=%d resources=%d cutPreserved=%d timerPreserved=%d"),Pass?TEXT("PASS"):TEXT("FAIL"),Timber,Plants,Resource,CutPreserved,TimerPreserved);
            if(FParse::Param(FCommandLine::Get(),TEXT("VoxelDetachedRestoreProbe")))FPlatformMisc::RequestExit(false);
        }),30.f,false);
    }));
}
