#include "VoxelEnvironmentLODPrototype.h"
#include "VoxelEarth.h"
#include "VoxelEarthFlyPawn.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UnrealClient.h"

namespace {
struct FVegetationCapture {
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<ACameraActor> Camera;
    TArray<TWeakObjectPtr<AVoxelEnvironmentLODPrototype>> Assets;
    FTimerHandle Timer;
    int Stage=0,Waits=0;
    float OldSpeed=-1,OldBearing=-1000;
};
void SetWeather(const TCHAR* Name,float Value){if(auto C=IConsoleManager::Get().FindConsoleVariable(Name))C->Set(Value,ECVF_SetByConsole);}
void CaptureVegetation(UWorld* W){
    if(!W||W->GetNetMode()!=NM_Standalone)return;
    auto Run=MakeShared<FVegetationCapture>();Run->World=W;
    Run->OldSpeed=IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Weather.PinMps"))->GetFloat();
    Run->OldBearing=IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Weather.PinFromDeg"))->GetFloat();
    W->GetTimerManager().SetTimer(Run->Timer,[Run](){
        auto W=Run->World.Get();if(!W)return;
        auto PC=W->GetFirstPlayerController();if(!PC)return;
        if(Run->Assets.IsEmpty()){
            const TCHAR* Names[]={TEXT("temperate-oak"),TEXT("bramble-thicket"),TEXT("meadow-daisy"),TEXT("granite-boulder")};
            TArray<TWeakObjectPtr<AVoxelEnvironmentLODPrototype>> Found;
            for(auto Name:Names)for(TActorIterator<AVoxelEnvironmentLODPrototype> It(W);It;++It)if(It->AssetName==Name){Found.Add(*It);break;}
            if(Found.Num()!=4){if(++Run->Waits>45){UE_LOG(LogVoxelEarth,Error,TEXT("VegetationCapture missing pilot assets"));W->GetTimerManager().ClearTimer(Run->Timer);}return;}
            Run->Assets=MoveTemp(Found);Run->Camera=W->SpawnActor<ACameraActor>();Run->Camera->GetCameraComponent()->SetFieldOfView(60);
            PC->SetViewTarget(Run->Camera.Get());
        }
        const int AssetIndex=Run->Stage/9,Step=Run->Stage%9;
        if(AssetIndex>=Run->Assets.Num()){
            for(auto Weak:Run->Assets)if(auto A=Weak.Get()){
                for(auto M:A->Materials){M->SetScalarParameterValue(TEXT("WindEnabled"),1);M->SetScalarParameterValue(TEXT("FoliageCutout"),1);}A->SetTestLOD(-1);
            }
            SetWeather(TEXT("voxel.Weather.PinMps"),Run->OldSpeed);SetWeather(TEXT("voxel.Weather.PinFromDeg"),Run->OldBearing);
            if(auto Pawn=Cast<AVoxelEarthFlyPawn>(PC->GetPawn())){
                const FVector Target=Run->Assets[0]->GetActorLocation()+FVector(0,0,650);
                Pawn->SetWalkMode(false);Pawn->SetActorLocation(Target+FVector(-1700,-1000,400));
                PC->SetControlRotation((Target-Pawn->GetActorLocation()).Rotation());PC->SetViewTarget(Pawn);
            }
            if(auto Camera=Run->Camera.Get())Camera->Destroy();
            UE_LOG(LogVoxelEarth,Log,TEXT("VegetationCapture COMPLETE restoredWeather=1"));
            W->GetTimerManager().ClearTimer(Run->Timer);return;
        }
        auto A=Run->Assets[AssetIndex].Get();if(!A)return;
        if(Step==0){
            const double Heights[]={650,75,22,70},Distances[]={2100,220,85,260};
            const FVector Target=A->GetActorLocation()+FVector(0,0,Heights[AssetIndex]);const double D=Distances[AssetIndex];
            Run->Camera->SetActorLocation(Target+FVector(-D,-D*.55,D*.30));Run->Camera->SetActorRotation((Target-Run->Camera->GetActorLocation()).Rotation());
            A->SetTestLOD(0);SetWeather(TEXT("voxel.Weather.PinMps"),0);
            for(auto M:A->Materials){M->SetScalarParameterValue(TEXT("WindEnabled"),0);M->SetScalarParameterValue(TEXT("FoliageCutout"),0);}
        }
        if(Step==2)for(auto M:A->Materials)M->SetScalarParameterValue(TEXT("FoliageCutout"),1);
        if(Step==4){SetWeather(TEXT("voxel.Weather.PinMps"),6);SetWeather(TEXT("voxel.Weather.PinFromDeg"),270);for(auto M:A->Materials)M->SetScalarParameterValue(TEXT("WindEnabled"),1);}
        if(Step==7)SetWeather(TEXT("voxel.Weather.PinFromDeg"),90);
        if(Step==1||Step==3||Step==5||Step==6||Step==8){
            const TCHAR* Label=Step==1?TEXT("opaque-still"):Step==3?TEXT("cutout-still"):Step==5?TEXT("east-a"):Step==6?TEXT("east-b"):TEXT("west");
            auto Collection=LoadObject<UMaterialParameterCollection>(nullptr,TEXT("/Game/Voxel/MPC_VoxelSky.MPC_VoxelSky"));
            const auto Wind=UKismetMaterialLibrary::GetVectorParameterValue(W,Collection,TEXT("WindVectorMS"));
            const auto Valid=UKismetMaterialLibrary::GetScalarParameterValue(W,Collection,TEXT("WindFieldValid"));
            UE_LOG(LogVoxelEarth,Log,TEXT("VegetationCapture SHOT %s %s sharedWind=(%.3f,%.3f) valid=%.0f time=%.3f"),*A->AssetName,Label,Wind.R,Wind.G,Valid,W->GetTimeSeconds());
            FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir()/TEXT("Screenshots/Vegetation")/(A->AssetName+TEXT("-")+Label+TEXT(".png")),false,false);
        }
        ++Run->Stage;
    },2.f,true,12.f);
}
FAutoConsoleCommandWithWorld VegetationCaptureCommand(TEXT("voxel.Vegetation.Capture"),TEXT("Capture the four LOD pilot assets: opaque, cutout, shared east wind over time, reversed wind."),FConsoleCommandWithWorldDelegate::CreateStatic(&CaptureVegetation));
}
