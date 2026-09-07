#include "VoxelDetachedPersistence.h"
#include "VoxelDebris.h"
#include "VoxelDebrisLifecycle.h"
#include "VoxelEarth.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Components/PrimitiveComponent.h"
#include "HAL/IConsoleManager.h"
#include "TimerManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"

// Explicit opt-in multiplayer smoke fixture. No geometry is spawned during
// ordinary play. Server waits for a player, then moves/retains/deletes a tiny
// resource so a late joining client can be checked without a full tree bake.
namespace
{
const FString& NetworkGateFile()
{
    // A test-driver action barrier only. Normal console fixtures retain their
    // original schedule, and ordinary replication never reads this file.
    static const FString Path=[](){
        FString Verify,Value;
        if(FParse::Value(FCommandLine::Get(),TEXT("VoxelObjectsNetVerify="),Verify)&&!Verify.IsEmpty())
            FParse::Value(FCommandLine::Get(),TEXT("VoxelObjectsNetGateFile="),Value);
        return Value;
    }();
    return Path;
}
struct FRun
{
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<AVoxelDebris> Actor;
    FGuid Id;
    FTimerHandle Timer;
    double Age=0.;
    int32 Stage=0;
    double StartedWall=FPlatformTime::Seconds();
    FString GateFile;
};
void StartFixture(UWorld* W)
{
    if(!W||W->GetNetMode()==NM_Client)return;
    auto Run=MakeShared<FRun>();Run->World=W;Run->GateFile=NetworkGateFile();
    W->GetTimerManager().SetTimer(Run->Timer,FTimerDelegate::CreateLambda([Run]{
        auto World=Run->World.Get();if(!World)return;Run->Age+=.25;
        if(!Run->GateFile.IsEmpty()&&FPlatformTime::Seconds()-Run->StartedWall>300.){
            UE_LOG(LogVoxelEarth,Error,TEXT("DetachedNetVerify failed: server ready barrier timed out"));
            World->GetTimerManager().ClearTimer(Run->Timer);
            FPlatformMisc::RequestExitWithStatus(false,1);return;
        }
        if(Run->Stage==0){
            auto PC=World->GetFirstPlayerController();auto Player=PC?PC->GetPawn():nullptr;
            if(!Player){if(Run->Age>60.)World->GetTimerManager().ClearTimer(Run->Timer);return;}
            const FVector Center=Player->GetActorLocation()+FVector(200,0,100);
            TArray<VoxelCoords::FVoxelCoord> Cells;
            for(int32 X=0;X<3;++X)for(int32 Y=0;Y<3;++Y)
                Cells.Add({FMath::FloorToInt64(Center.X/10.)+X,FMath::FloorToInt64(Center.Y/10.)+Y,FMath::FloorToInt64(Center.Z/10.)});
            auto A=World->SpawnActor<AVoxelDebris>();if(!A||A->InitFromIsland(Cells)!=9){if(A)A->Destroy();World->GetTimerManager().ClearTimer(Run->Timer);return;}
            A->SetActorTickEnabled(false);if(auto Root=Cast<UPrimitiveComponent>(A->GetRootComponent()))Root->SetSimulatePhysics(false);
            if(auto Life=A->FindComponentByClass<UVoxelDebrisLifecycle>())Life->Configure(EVoxelDebrisLifetime::Harvestable);
            VoxelDetachedPersistence::RefreshObjects(World);auto E=VoxelObjects::Get(World).Find(A);
            if(!E){A->Destroy();World->GetTimerManager().ClearTimer(Run->Timer);return;}
            Run->Actor=A;Run->Id=E->Id;Run->Age=0.;Run->Stage=1;
            UE_LOG(LogVoxelEarth,Log,TEXT("DetachedNetFixture spawn id=%s cells=9 pos=%s"),*Run->Id.ToString(),*A->GetActorLocation().ToString());
        }else if(Run->Stage==1&&(Run->GateFile.IsEmpty()?Run->Age>=2.:IFileManager::Get().FileExists(*(Run->GateFile+TEXT(".early"))))){
            if(auto A=Run->Actor.Get())A->AddActorWorldOffset(FVector(100,0,0),false,nullptr,ETeleportType::TeleportPhysics);
            Run->Stage=2;UE_LOG(LogVoxelEarth,Log,TEXT("DetachedNetFixture move id=%s deltaX=100"),*Run->Id.ToString());
        }else if(Run->Stage==2&&Run->Age>=4.){
            if(auto A=Run->Actor.Get())if(auto Life=A->FindComponentByClass<UVoxelDebrisLifecycle>())Life->Retain();
            Run->Stage=3;UE_LOG(LogVoxelEarth,Log,TEXT("DetachedNetFixture retain id=%s; removal=%s"),*Run->Id.ToString(),Run->GateFile.IsEmpty()?TEXT("60 seconds"):TEXT("client-ready barrier"));
        }else if(Run->Stage==3&&(Run->GateFile.IsEmpty()?Run->Age>=60.:IFileManager::Get().FileExists(*Run->GateFile))){
            if(auto A=Run->Actor.Get())A->Destroy();
            UE_LOG(LogVoxelEarth,Log,TEXT("DetachedNetFixture remove id=%s"),*Run->Id.ToString());
            World->GetTimerManager().ClearTimer(Run->Timer);
        }
    }),.25f,true);
}
FAutoConsoleCommandWithWorld Fixture(TEXT("voxel.Objects.NetFixture"),TEXT("Server-only explicit nine-cell multiplayer smoke fixture; moves at2s, retains at4s, removes at60s."),FConsoleCommandWithWorldDelegate::CreateStatic(&StartFixture));
FAutoConsoleCommandWithWorld Dump(TEXT("voxel.Objects.NetDump"),TEXT("Log local detached IDs/revisions/transforms for cross-process smoke comparisons."),FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* W){
    if(!W)return;auto R=VoxelObjects::Find(W);if(!R){UE_LOG(LogVoxelEarth,Log,TEXT("DetachedNetDump empty"));return;}
    for(const auto& E:R->Snapshot())UE_LOG(LogVoxelEarth,Log,TEXT("DetachedNetDump id=%s rev=%llu geom=%llu state=%u actor=%d retained=%d pos=%s"),
        *E.Id.ToString(),E.Revision,E.GeometryRevision,uint32(E.Residency),E.Actor.IsValid(),E.bRetained,*E.Transform.GetLocation().ToString());
}));
}

namespace VoxelDetachedNetworkFixture
{
void Tick(UWorld* W,float Delta)
{
    static FString Mode=[](){FString Value;FParse::Value(FCommandLine::Get(),TEXT("VoxelObjectsNetVerify="),Value);return Value;}();
    if(Mode.IsEmpty()||!W||!W->HasBegunPlay())return;
    const bool Server=Mode==TEXT("server");
    if(Server?(W->GetNetMode()!=NM_DedicatedServer&&W->GetNetMode()!=NM_ListenServer):W->GetNetMode()!=NM_Client)return;
    struct FObservation{
        double Age=0.,CreatedWall=FPlatformTime::Seconds(),StartedWall=0.,FinishedWall=0.,AcknowledgedWall=0.;
        int32 Checkpoint=0;
        bool Started=false,Finished=false;
        FGuid ObservedId;
    };
    static TMap<TWeakObjectPtr<UWorld>,FObservation> Runs;
    for(auto It=Runs.CreateIterator();It;++It)if(!It.Key().IsValid())It.RemoveCurrent();
    auto& Run=Runs.FindOrAdd(W);
    if(Run.Finished)return;
    const bool Gated=!NetworkGateFile().IsEmpty();
    const double Now=FPlatformTime::Seconds();
    if(Gated&&Now-Run.CreatedWall>300.){
        Run.Finished=true;
        UE_LOG(LogVoxelEarth,Error,TEXT("DetachedNetVerify failed: observer timed out role=%s checkpoint=%d"),*Mode,Run.Checkpoint);
        FPlatformMisc::RequestExitWithStatus(false,1);return;
    }
    if(!Run.Started){auto PC=W->GetFirstPlayerController();if(!PC||!PC->GetPawn())return;Run.Started=true;Run.StartedWall=Now;if(Server)StartFixture(W);UE_LOG(LogVoxelEarth,Log,TEXT("DetachedNetVerify observer started role=%s"),*Mode);}
    Run.Age=Gated?Now-Run.StartedWall:Run.Age+Delta;
    auto Emit=[&](int32 Label){
        UE_LOG(LogVoxelEarth,Log,TEXT("DetachedNetVerify checkpoint=%d role=%s"),Label,*Mode);
        if(auto R=VoxelObjects::Find(W))for(const auto& E:R->Snapshot()){
            UE_LOG(LogVoxelEarth,Log,
                TEXT("DetachedNetVerify id=%s rev=%llu geom=%llu state=%u actor=%d retained=%d pos=%s"),
                *E.Id.ToString(),E.Revision,E.GeometryRevision,uint32(E.Residency),E.Actor.IsValid(),E.bRetained,*E.Transform.GetLocation().ToString());
            if(auto Actor=E.Actor.Get())UE_LOG(LogVoxelEarth,Log,TEXT("DetachedNetVerify visual id=%s errorCm=%.6f"),
                *E.Id.ToString(),FVector::Distance(Actor->GetActorLocation(),E.Transform.GetLocation()));
        }
    };
    if(Gated){
        if(auto Registry=VoxelObjects::Find(W)){
            const auto Entries=Registry->Snapshot();
            if(Run.Checkpoint==0&&Run.Age>=8.){
                FGuid ReadyId;int32 ReadyCount=0;
                for(const auto& E:Entries)if(E.Kind==1&&E.bRetained&&E.Residency==VoxelObjects::EResidency::Live){
                    if(auto Actor=E.Actor.Get())if(FVector::Distance(Actor->GetActorLocation(),E.Transform.GetLocation())<=.1){ReadyId=E.Id;++ReadyCount;}
                }
                // The isolated fixture must have one unambiguous live source.
                // No empty checkpoint is emitted while a cold client restores.
                if(ReadyCount==1){Run.ObservedId=ReadyId;Emit(8);Run.Checkpoint=1;}
            }else if(Run.Checkpoint>0&&Run.Checkpoint<3){
                for(const auto& E:Entries)if(E.Id==Run.ObservedId){
                    if(E.Residency==VoxelObjects::EResidency::Tombstone&&!E.Actor.IsValid()){
                        Emit(70);Run.Checkpoint=3;Run.FinishedWall=Now;
                    }else if(Run.Checkpoint==1&&Run.Age>=20.&&E.bRetained&&E.Residency==VoxelObjects::EResidency::Live){
                        if(auto Actor=E.Actor.Get())if(FVector::Distance(Actor->GetActorLocation(),E.Transform.GetLocation())<=.1){Emit(20);Run.Checkpoint=2;}
                    }
                    break;
                }
            }
        }
        // Labels 8/70 identify observed states, not assumed delivery times.
        // The harness acknowledges BOTH clients' real tombstones before the
        // authority exits. A server-local delay cannot prove remote delivery.
        if(Server&&Run.Checkpoint==3&&Run.AcknowledgedWall==0.&&IFileManager::Get().FileExists(*(NetworkGateFile()+TEXT(".complete"))))Run.AcknowledgedWall=Now;
        const double ExitAfter=Server?Run.AcknowledgedWall:Run.FinishedWall;
        if(Run.Checkpoint==3&&ExitAfter>0.&&Now-ExitAfter>=3.){
            Run.Finished=true;UE_LOG(LogVoxelEarth,Log,TEXT("DetachedNetVerify completed role=%s"),*Mode);FPlatformMisc::RequestExit(false);
        }
        return;
    }
    const double At[]={8.,20.,70.};
    if(Run.Checkpoint<3&&Run.Age>=At[Run.Checkpoint]){Emit(int32(At[Run.Checkpoint]));++Run.Checkpoint;}
    // Give a cold late-client startup enough time to verify its tombstone too.
    if(Run.Age>=(Server?130.:85.)){UE_LOG(LogVoxelEarth,Log,TEXT("DetachedNetVerify completed role=%s"),*Mode);FPlatformMisc::RequestExit(false);}
}
}
