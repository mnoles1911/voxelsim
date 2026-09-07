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

// Explicit opt-in multiplayer smoke fixture. No geometry is spawned during
// ordinary play. Server waits for a player, then moves/retains/deletes a tiny
// resource so a late joining client can be checked without a full tree bake.
namespace
{
struct FRun
{
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<AVoxelDebris> Actor;
    FGuid Id;
    FTimerHandle Timer;
    double Age=0.;
    int32 Stage=0;
};
void StartFixture(UWorld* W)
{
    if(!W||W->GetNetMode()==NM_Client)return;
    auto Run=MakeShared<FRun>();Run->World=W;
    W->GetTimerManager().SetTimer(Run->Timer,FTimerDelegate::CreateLambda([Run]{
        auto World=Run->World.Get();if(!World)return;Run->Age+=.25;
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
        }else if(Run->Stage==1&&Run->Age>=2.){
            if(auto A=Run->Actor.Get())A->AddActorWorldOffset(FVector(100,0,0),false,nullptr,ETeleportType::TeleportPhysics);
            Run->Stage=2;UE_LOG(LogVoxelEarth,Log,TEXT("DetachedNetFixture move id=%s deltaX=100"),*Run->Id.ToString());
        }else if(Run->Stage==2&&Run->Age>=4.){
            if(auto A=Run->Actor.Get())if(auto Life=A->FindComponentByClass<UVoxelDebrisLifecycle>())Life->Retain();
            Run->Stage=3;UE_LOG(LogVoxelEarth,Log,TEXT("DetachedNetFixture retain id=%s; late join before 60 seconds"),*Run->Id.ToString());
        }else if(Run->Stage==3&&Run->Age>=60.){
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
    struct FObservation{double Age=0.;int32 Checkpoint=0;bool Started=false;};
    static TMap<TWeakObjectPtr<UWorld>,FObservation> Runs;
    for(auto It=Runs.CreateIterator();It;++It)if(!It.Key().IsValid())It.RemoveCurrent();
    auto& Run=Runs.FindOrAdd(W);
    if(!Run.Started){auto PC=W->GetFirstPlayerController();if(!PC||!PC->GetPawn())return;Run.Started=true;if(Server)StartFixture(W);UE_LOG(LogVoxelEarth,Log,TEXT("DetachedNetVerify observer started role=%s"),*Mode);}
    Run.Age+=Delta;const double At[]={8.,20.,70.};
    if(Run.Checkpoint<3&&Run.Age>=At[Run.Checkpoint]){
        UE_LOG(LogVoxelEarth,Log,TEXT("DetachedNetVerify checkpoint=%d role=%s"),int32(At[Run.Checkpoint]),*Mode);
        if(auto R=VoxelObjects::Find(W))for(const auto& E:R->Snapshot())UE_LOG(LogVoxelEarth,Log,
            TEXT("DetachedNetVerify id=%s rev=%llu geom=%llu state=%u actor=%d retained=%d pos=%s"),
            *E.Id.ToString(),E.Revision,E.GeometryRevision,uint32(E.Residency),E.Actor.IsValid(),E.bRetained,*E.Transform.GetLocation().ToString());
        ++Run.Checkpoint;
    }
    // Give a cold late-client startup enough time to verify its tombstone too.
    if(Run.Age>=(Server?130.:85.)){UE_LOG(LogVoxelEarth,Log,TEXT("DetachedNetVerify completed role=%s"),*Mode);FPlatformMisc::RequestExit(false);}
}
}
