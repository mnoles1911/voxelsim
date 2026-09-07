#include "VoxelSaveJobs.h"
#include "VoxelSaveLibrary.h"
#include "VoxelWorldSubsystem.h"
#include "VoxelEarth.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "TimerManager.h"

namespace VoxelSaveJobsProbe
{
struct FRun { bool BusyRejected=false; uint64 StartFrame=0; };
FAutoConsoleCommandWithWorldAndArgs Probe(TEXT("voxel.SaveAsync.Probe"),TEXT("Async snapshot test; optional 'exit' tests shutdown while compression is pending."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args,UWorld* W){
        if(!W||W->GetNetMode()!=NM_Standalone)return;
        const bool ExitDuringSave=Args.Contains(TEXT("exit"));
        FTimerHandle Handle;W->GetTimerManager().SetTimer(Handle,FTimerDelegate::CreateLambda([W,ExitDuringSave](){
            auto Sub=W->GetSubsystem<UVoxelWorldSubsystem>();if(!Sub)return;
            auto Run=MakeShared<FRun>();Run->StartFrame=GFrameCounter;
            const FString Name=ExitDuringSave?TEXT("Async exit verification"):TEXT("Async save verification");
            auto PC=W->GetFirstPlayerController();const FTransform Player=PC&&PC->GetPawn()?PC->GetPawn()->GetActorTransform():FTransform::Identity;
            const bool Started=VoxelSave::WriteAsync(*Sub,Name,false,Player,0,[Run,ExitDuringSave](bool Success){
                const uint64 Frames=GFrameCounter-Run->StartFrame;
                const bool Pass=Success&&Run->BusyRejected&&(ExitDuringSave||Frames>2);
                UE_LOG(LogVoxelEarth,Log,TEXT("SaveAsync PROBE %s exit=%d busyRejected=%d framesAdvanced=%llu"),Pass?TEXT("PASS"):TEXT("FAIL"),ExitDuringSave,Run->BusyRejected,Frames);
                if(ExitDuringSave)return;
                // A file used as a parent directory forces a real worker-side
                // write failure, confined to this unique test artifact.
                const FString Blocker=FPaths::ProjectSavedDir()/TEXT("Tests")/(TEXT("async-blocker-")+FGuid::NewGuid().ToString(EGuidFormats::Digits));
                if(!FFileHelper::SaveStringToFile(TEXT("test"),*Blocker)){UE_LOG(LogVoxelEarth,Error,TEXT("SaveAsync FAILURE_PROBE setup failed"));FPlatformMisc::RequestExit(false);return;}
                VoxelSaveJobs::FSnapshot Job;Job.TerrainPath=Blocker/TEXT("world.vxlog");Job.Terrain={1};
                Job.Simulation.Water={1};Job.Simulation.Hydrology={1};Job.Simulation.Clock={1};
                const bool Accepted=VoxelSaveJobs::Submit(MoveTemp(Job),[](bool Ok){
                    UE_LOG(LogVoxelEarth,Log,TEXT("SaveAsync FAILURE_PROBE %s"),!Ok?TEXT("PASS"):TEXT("FAIL"));FPlatformMisc::RequestExit(false);
                });
                if(!Accepted){UE_LOG(LogVoxelEarth,Error,TEXT("SaveAsync FAILURE_PROBE admission failed"));FPlatformMisc::RequestExit(false);}
            });
            if(!Started){UE_LOG(LogVoxelEarth,Error,TEXT("SaveAsync PROBE admission failed"));FPlatformMisc::RequestExit(false);return;}
            Run->BusyRejected=!VoxelSave::WriteAsync(*Sub,Name,false,Player,0);
            if(ExitDuringSave)FPlatformMisc::RequestExit(false);
        }),ExitDuringSave?15.f:60.f,false);
    }));
}
