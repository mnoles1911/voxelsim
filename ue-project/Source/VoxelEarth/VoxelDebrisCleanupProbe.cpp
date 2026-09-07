#include "VoxelDebrisLifecycle.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "HAL/IConsoleManager.h"
#include "TimerManager.h"
#include "VoxelEarth.h"

namespace VoxelDebrisCleanupProbe
{
struct FRun
{
    TWeakObjectPtr<AActor> Chip, Resource, Large, Kept;
    FTimerHandle Timer;
};
FAutoConsoleCommandWithWorld Command(TEXT("voxel.DebrisCleanup.Probe"),
    TEXT("Spawn invisible lifecycle fixtures; report timer integration after 12 gameplay seconds."),
    FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* W)
    {
        if (!W || !W->IsGameWorld()) return;
        auto Run=MakeShared<FRun>();
        auto Spawn=[W](EVoxelDebrisLifetime Kind, double Remaining)
        {
            auto A=W->SpawnActor<AActor>();
            auto Root=NewObject<USceneComponent>(A); A->SetRootComponent(Root); Root->RegisterComponent();
            A->SetActorLocation(FVector(1.e8,1.e8,1.e8));
            auto Life=NewObject<UVoxelDebrisLifecycle>(A); A->AddInstanceComponent(Life); Life->RegisterComponent();
            FVoxelDebrisLifetimeState S; S.Kind=Kind; S.RemainingSeconds=Remaining; Life->RestoreState(S);
            return A;
        };
        Run->Chip=Spawn(EVoxelDebrisLifetime::Cosmetic,10.);
        Run->Resource=Spawn(EVoxelDebrisLifetime::Harvestable,1.);
        Run->Large=Spawn(EVoxelDebrisLifetime::Substantial,0.);
        Run->Kept=Spawn(EVoxelDebrisLifetime::Retained,0.);
        UE_LOG(LogVoxelEarth,Log,TEXT("DebrisCleanup PROBE started"));
        W->GetTimerManager().SetTimer(Run->Timer,FTimerDelegate::CreateLambda([Run]()
        {
            const bool Pass=!Run->Chip.IsValid()&&!Run->Resource.IsValid()&&Run->Large.IsValid()&&Run->Kept.IsValid();
            UE_LOG(LogVoxelEarth,Log,TEXT("DebrisCleanup PROBE %s chipGone=%d resourceGone=%d largeKept=%d retainedKept=%d"),
                Pass?TEXT("PASS"):TEXT("FAIL"),!Run->Chip.IsValid(),!Run->Resource.IsValid(),Run->Large.IsValid(),Run->Kept.IsValid());
            if(Run->Large.IsValid())Run->Large->Destroy(); if(Run->Kept.IsValid())Run->Kept->Destroy();
            if(Run->Chip.IsValid())Run->Chip->Destroy(); if(Run->Resource.IsValid())Run->Resource->Destroy();
        }),12.f,false);
    }));
}
