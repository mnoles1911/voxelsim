#include "VoxelAppearanceForest.h"
#include "VoxelEnvironmentLODPrototype.h"
#include "VoxelWorldSubsystem.h"
#include "VoxelDebug.h"
#include "VoxelEarth.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "VoxelDetailAssetSubsystem.h"
#include "VoxelFineTileStreamer.h"
#include "voxelcore/assetfield.h"
#include "voxelcore/amplifier.h"

namespace {
void LaunchEcologicalWorld(UWorld* World){
    struct FRun {TWeakObjectPtr<UWorld> World;TWeakObjectPtr<ACameraActor> Camera;FTimerHandle Timer;FString Output;int Stage=0;double Quiet=-1,Started=0,LastLog=-1;};
    auto Run=MakeShared<FRun>();Run->World=World;
    if(!FParse::Value(FCommandLine::Get(),TEXT("VoxelEcologyWorldOutput="),Run->Output))return;
    IFileManager::Get().MakeDirectory(*Run->Output,true);
    World->GetTimerManager().SetTimer(Run->Timer,[Run](){
        auto W=Run->World.Get();if(!W)return;
        auto Ground=W->GetSubsystem<UVoxelWorldSubsystem>();auto PC=W->GetFirstPlayerController();
        auto Details=W->GetSubsystem<UVoxelDetailAssetSubsystem>();
        if(!Ground||!PC||!PC->GetPawn()||!Details)return;
        const auto Field=Ground->GetAssetField();const auto Amp=Ground->GetWorldgenAmplifier();
        if(!Field||!Field->ecologyEnabled()||!Amp){UE_LOG(LogVoxelEarth,Error,TEXT("EcologyWorld missing active ecology"));W->GetTimerManager().ClearTimer(Run->Timer);FPlatformMisc::RequestExit(false);return;}
        const auto Progress=Ground->GetStreamingProgress();int Pending=0;
        for(int L=0;L<VoxelCoords::kNumLevels;++L)Pending+=Progress.LevelPendingCount[L];
        int FineReady=0,FineTotal=0;uint64 Live=0;
        const bool Settled=Ground->IsFineRingSettled(FineReady,FineTotal)&&Pending==0&&Progress.TotalJobsInFlight==0&&Details->IsPlacementSettled(Live);
        const double Now=FPlatformTime::Seconds();
        if(Run->Stage==0){
            if(!Settled)Run->Quiet=-1;else if(Run->Quiet<0)Run->Quiet=Now;
            if(Run->Quiet<0||Now-Run->Quiet<5){
                if(Now-Run->LastLog>10){Run->LastLog=Now;UE_LOG(LogVoxelEarth,Log,TEXT("EcologyWorld WAIT pending=%d jobs=%d fine=%d/%d detailInstances=%llu"),Pending,Progress.TotalJobsInFlight,FineReady,FineTotal,Live);}return;
            }
            const FVector P=PC->GetPawn()->GetActorLocation();
            const int64 X=FMath::FloorToInt64(P.X/10.),Y=FMath::FloorToInt64(P.Y/10.);
            const auto R=Field->columnSamplingReachMm();const auto Streamer=Ground->GetFineTileStreamer();
            if(Streamer&&!Streamer->IsFootprintResident((X-1280)*100-R,(Y-1280)*100-R,(X+1280)*100+R,(Y+1280)*100+R)){Run->Quiet=-1;return;}
            auto Channels=Ground->GetAssetChannelSource();
            const double Begin=FPlatformTime::Seconds();
            const auto Instances=Field->instancesForRect({X-160,Y-160,X+159,Y+159},[&](int64_t VX,int64_t VY){
                return vxc::assetColumnFactsFromSample(Amp->column(VX,VY),Channels?Channels->channelsAt(VX,VY):vxc::AssetColumnChannels{});
            });
            const double QueryMs=(FPlatformTime::Seconds()-Begin)*1000;
            int Trees=0,Cover=0;for(const auto& I:Instances){if(Field->layers()[I.layer].terrainLattice)++Trees;else ++Cover;}
            const auto Centre=Amp->column(X,Y);
            UE_LOG(LogVoxelEarth,Log,TEXT("EcologyWorld SAMPLE biome=%d trees=%d detail=%d liveHism=%llu queryMs=%.3f"),int(Centre.biome),Trees,Cover,Live,QueryMs);
            // Outside frame measurement: export real terrain and the same
            // terrain-conditioned context consumed by the runtime resolver.
            FString TerrainCsv=TEXT("x_mm,y_mm,surface_mm,slope_mm_per_m,curvature,heat,talus,water_mm,biome,active,stand,height_target,tree_keep,feature_strength,distance_water_mm,twi_milli,plot_lower_mm,plot_upper_mm,plot_tested_cells,plot_water_max_mm,plot_water_channels_verified\n");
            for(int64 VY=Y-1240;VY<Y+1280;VY+=80)for(int64 VX=X-1240;VX<X+1280;VX+=80){
                const auto Facts=vxc::assetColumnFactsFromSample(Amp->column(VX,VY),Channels?Channels->channelsAt(VX,VY):vxc::AssetColumnChannels{});
                vxc::EcoContext Context;const bool Active=Field->ecologicalContextAt(VX*100,VY*100,Facts,Context);
                // Enumerate the complete 100mm placement footprint. The
                // amplifier's streaming envelope includes unsampled detail
                // extrema and was too loose to certify even gentle ground.
                int64 PlotLow=INT64_MIN,PlotHigh=INT64_MAX;
                int PlotTested=0,PlotWaterMax=-1,PlotWaterChannelsVerified=0;
                // No synthetic/default channels may certify a dry foundation.
                // The streamer gate covers terrain; channel sentinels below
                // also reject missing/refused placement data in its own binding.
                const bool PlotResident=Streamer&&Streamer->IsFootprintResident(
                    (VX-25)*100,(VY-25)*100,(VX+25)*100,(VY+25)*100);
                if(Active&&Channels&&PlotResident){
                    int64 SampleLow=INT64_MAX,SampleHigh=INT64_MIN;
                    int SampleWaterMax=0;
                    for(int64 PY=VY-25;PY<=VY+24;++PY)for(int64 PX=VX-25;PX<=VX+24;++PX){
                        const auto CellChannels=Channels->channelsAt(PX,PY);
                        // A dry water datum is legitimately a sentinel, so use
                        // placement-channel validity rather than requiring water.
                        if(CellChannels.distanceToWaterMm==vxc::kAssetNoWaterDistanceMm||
                           CellChannels.twiMilli==vxc::kAssetNoTwiMilli)continue;
                        const auto Cell=vxc::assetColumnFactsFromSample(Amp->column(PX,PY),CellChannels);
                        if(Cell.known){
                            ++PlotTested;SampleWaterMax=FMath::Max(SampleWaterMax,int(Cell.standingWaterMm));
                            SampleLow=FMath::Min(SampleLow,int64(Cell.surfaceMm));
                            SampleHigh=FMath::Max(SampleHigh,int64(Cell.surfaceMm));
                        }
                    }
                    // An incomplete survey must not report partial extrema
                    // as bounds over the full foundation.
                    if(PlotTested==2500){PlotLow=SampleLow;PlotHigh=SampleHigh;PlotWaterMax=SampleWaterMax;PlotWaterChannelsVerified=1;}
                }
                TerrainCsv+=FString::Printf(TEXT("%lld,%lld,%d,%lld,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%d,%d,%lld,%lld,%d,%d,%d\n"),
                    VX*100,VY*100,Facts.surfaceMm,Facts.slopeMmPerM,int(Facts.curv),int(Facts.heat),int(Facts.talus),Facts.standingWaterMm,int(Facts.biome),int(Active),
                    Active?int(Context.stand):-1,Context.targetHeightPerMille,Context.treeKeepPerMille,Context.featureStrengthPerMille,Facts.distanceToWaterMm,Facts.twiMilli,PlotLow,PlotHigh,PlotTested,PlotWaterMax,PlotWaterChannelsVerified);
            }
            const auto MapInstances=Field->instancesForRect({X-1280,Y-1280,X+1279,Y+1279},[&](int64_t VX,int64_t VY){
                return vxc::assetColumnFactsFromSample(Amp->column(VX,VY),Channels?Channels->channelsAt(VX,VY):vxc::AssetColumnChannels{});
            });
            FString PlacementCsv=TEXT("bank_id,seed_slot,x_mm,y_mm,z_mm,layer,yaw,terrain_lattice,facts_known,biome,active,water_mm,distance_water_mm,slope_mm_per_m\n");
            for(const auto& I:MapInstances){
                // Sample the same floored voxel column used by selection.
                // An 8m display cell is not evidence of an individual reed's
                // water depth or shoreline distance. Outside timed capture.
                const int64 VX=vxc::floorDiv(I.anchorXMm,int64_t(100));
                const int64 VY=vxc::floorDiv(I.anchorYMm,int64_t(100));
                const auto Facts=vxc::assetColumnFactsFromSample(Amp->column(VX,VY),Channels?Channels->channelsAt(VX,VY):vxc::AssetColumnChannels{});
                vxc::EcoContext Context;
                const bool Active=Field->ecologicalContextAt(I.anchorXMm,I.anchorYMm,Facts,Context);
                PlacementCsv+=FString::Printf(TEXT("%u,%u,%lld,%lld,%d,%u,%u,%d,%d,%d,%d,%d,%d,%lld\n"),
                    I.bankId,I.seedIndex,I.anchorXMm,I.anchorYMm,I.anchorZMm,I.layer,I.yawQuarter,int(Field->layers()[I.layer].terrainLattice),
                    int(Facts.known),int(Facts.biome),int(Active),Facts.standingWaterMm,Facts.distanceToWaterMm,Facts.slopeMmPerM);
            }
            if(!FFileHelper::SaveStringToFile(TerrainCsv,*(Run->Output/TEXT("terrain-samples.csv")))||
               !FFileHelper::SaveStringToFile(PlacementCsv,*(Run->Output/TEXT("terrain-placement.csv")))){
                UE_LOG(LogVoxelEarth,Error,TEXT("EcologyWorld terrain map export failed"));W->GetTimerManager().ClearTimer(Run->Timer);FPlatformMisc::RequestExit(false);return;
            }
            auto Camera=W->SpawnActor<ACameraActor>();Run->Camera=Camera;Camera->GetCameraComponent()->SetFieldOfView(75);
            Camera->SetActorLocation(FVector(P.X,P.Y,Ground->GetSurfaceHeightUU(P.X,P.Y)+170));Camera->SetActorRotation(FRotator::ZeroRotator);PC->SetViewTarget(Camera);
            // Start on a later timer tick. CSV's first FrameTime covers the
            // whole frame, including work preceding the start command. Starting
            // here admitted seconds of survey work outside our event interval.
            Run->Stage=6;return;
        }
        if(Run->Stage==6){
            if(!Settled)return;
            Run->Started=FPlatformTime::Seconds();Run->Stage=1;
            PC->ConsoleCommand(TEXT("csvprofile start"));
            UE_LOG(LogVoxelEarth,Log,TEXT("EcologyWorld MEASURE_BEGIN mono=%.6f"),Run->Started);return;
        }
        if(!Settled)UE_LOG(LogVoxelEarth,Warning,TEXT("EcologyWorld STREAMING_BUSY"));
        const double Elapsed=Now-Run->Started;
        if(Run->Stage==1&&Elapsed>=10){FScreenshotRequest::RequestScreenshot(Run->Output/TEXT("world-player.png"),false,false);Run->Stage=2;}
        if(Run->Stage==2&&Elapsed>=20){
            const auto P=PC->GetPawn()->GetActorLocation();const FVector Target=P+FVector(0,0,500);
            Run->Camera->SetActorLocation(Target+FVector(-3000,-2000,2000));Run->Camera->SetActorRotation((Target-Run->Camera->GetActorLocation()).Rotation());
            Run->Stage=3;UE_LOG(LogVoxelEarth,Log,TEXT("EcologyWorld OVERVIEW mono=%.6f"),FPlatformTime::Seconds());
        }
        if(Run->Stage==3&&Elapsed>=30){FScreenshotRequest::RequestScreenshot(Run->Output/TEXT("world-overview.png"),false,false);Run->Stage=4;}
        if(Run->Stage==4&&Elapsed>=40){PC->ConsoleCommand(TEXT("csvprofile stop"));Run->Stage=5;UE_LOG(LogVoxelEarth,Log,TEXT("EcologyWorld MEASURE_END mono=%.6f"),FPlatformTime::Seconds());}
        if(Run->Stage==5&&Elapsed>=45){UE_LOG(LogVoxelEarth,Log,TEXT("EcologyWorld COMPLETE"));W->GetTimerManager().ClearTimer(Run->Timer);FPlatformMisc::RequestExit(false);}
    },1.f,true);
}
struct FForestRun {
    TWeakObjectPtr<UWorld> World;
    TArray<TWeakObjectPtr<AVoxelEnvironmentLODPrototype>> Trees;
    TArray<TPair<FString,int32>> Sources;
    TWeakObjectPtr<ACameraActor> Camera;
    FVector Anchor;
    FString Output,RootOutput;
    FTimerHandle Timer;
    int32 Count=16,Stage=0,PlacementAttempt=0,Pass=0;
    bool Sequence=false;
    bool EcologicalLayout=false;
    double LayoutWidthUU=6400;
    TArray<FVector> LayoutPositions;
    TArray<int32> LayoutYaw;
    double Ready=0,QuietSince=-1,LastWaitingLog=-1;
};
const TCHAR* PassName(int32 Pass){static const TCHAR* Names[]={TEXT("visible1"),TEXT("hidden1"),TEXT("hidden2"),TEXT("visible2")};return Names[Pass];}
void BeginPass(const TSharedPtr<FForestRun>& Run){
    const bool Hidden=Run->Sequence?(Run->Pass==1||Run->Pass==2):FParse::Param(FCommandLine::Get(),TEXT("VoxelAppearanceForestHidden"));
    Run->Output=Run->Sequence?Run->RootOutput/PassName(Run->Pass):Run->RootOutput;
    IFileManager::Get().MakeDirectory(*Run->Output,true);
    for(auto Tree:Run->Trees)if(Tree.IsValid())Tree->SetActorHiddenInGame(Hidden);
    Run->Ready=FPlatformTime::Seconds();Run->QuietSince=-1;Run->LastWaitingLog=-1;Run->Stage=0;
    if(Run->Sequence)UE_LOG(LogVoxelEarth,Log,TEXT("AppearanceForest PASS_BEGIN name=%s hidden=%d"),PassName(Run->Pass),int32(Hidden));
}
void Launch(UWorld* W){
    auto PC=W?W->GetFirstPlayerController():nullptr;
    auto Terrain=W?W->GetSubsystem<UVoxelWorldSubsystem>():nullptr;
    if(!PC||!PC->GetPawn()||!Terrain){UE_LOG(LogVoxelEarth,Error,TEXT("AppearanceForest missing game session"));return;}
    FString Dir;FParse::Value(FCommandLine::Get(),TEXT("VoxelAssetDir="),Dir);
    FString Text;TSharedPtr<FJsonObject> Manifest;
    if(!FFileHelper::LoadFileToString(Text,*(Dir/TEXT("appearance/published.json")))||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Manifest)||!Manifest){UE_LOG(LogVoxelEarth,Error,TEXT("AppearanceForest requires published inventory"));return;}
    const TArray<TSharedPtr<FJsonValue>>* Rows=nullptr;if(!Manifest->TryGetArrayField(TEXT("models"),Rows))return;
    auto Run=MakeShared<FForestRun>();Run->World=W;
    for(const auto& Row:*Rows){const auto O=Row->AsObject();FString Species;double Seed;
        if(O&&O->TryGetStringField(TEXT("species"),Species)&&O->TryGetNumberField(TEXT("seed"),Seed))Run->Sources.Emplace(Species,int32(Seed));
    }
    if(Run->Sources.IsEmpty())return;
    FParse::Value(FCommandLine::Get(),TEXT("VoxelAppearanceForestCount="),Run->Count);Run->Count=FMath::Clamp(Run->Count,1,36);
    Run->Output=FPaths::ProjectSavedDir()/TEXT("Screenshots/AppearanceForest");FParse::Value(FCommandLine::Get(),TEXT("VoxelAppearanceForestOutput="),Run->Output);
    Run->RootOutput=Run->Output;Run->Sequence=FParse::Param(FCommandLine::Get(),TEXT("VoxelAppearanceForestSequence"));
    FString LayoutPath;
    if(FParse::Value(FCommandLine::Get(),TEXT("VoxelEcologyForestLayout="),LayoutPath)){
        FString LayoutText;TSharedPtr<FJsonObject> Layout;const TArray<TSharedPtr<FJsonValue>>* Models=nullptr;
        bool PreviewOnly=false;double Width=0;
        if(Run->Sequence||!FFileHelper::LoadFileToString(LayoutText,*LayoutPath)||
           !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(LayoutText),Layout)||!Layout||
           !Layout->TryGetBoolField(TEXT("preview_only"),PreviewOnly)||!PreviewOnly||
           !Layout->TryGetNumberField(TEXT("width_mm"),Width)||!FMath::IsFinite(Width)||Width<1000||Width>128000||
           !Layout->TryGetArrayField(TEXT("models"),Models)||Models->Num()<1||Models->Num()>128){
            UE_LOG(LogVoxelEarth,Error,TEXT("AppearanceForest invalid ecological layout"));return;
        }
        TArray<TPair<FString,int32>> Selected;
        for(const auto& Value:*Models){const auto O=Value->AsObject();FString Species;double Seed=0,X=0,Y=0,Yaw=0;
            if(!O||!O->TryGetStringField(TEXT("species"),Species)||!O->TryGetNumberField(TEXT("seed"),Seed)||
               !O->TryGetNumberField(TEXT("x_mm"),X)||!O->TryGetNumberField(TEXT("y_mm"),Y)||!O->TryGetNumberField(TEXT("yaw_quarter"),Yaw)||
               !FMath::IsFinite(Seed)||Seed<1||Seed>INT32_MAX||Seed!=FMath::FloorToDouble(Seed)||
               !FMath::IsFinite(X)||!FMath::IsFinite(Y)||FMath::Abs(X)>Width/2||FMath::Abs(Y)>Width/2||
               !FMath::IsFinite(Yaw)||Yaw<0||Yaw>3||Yaw!=FMath::FloorToDouble(Yaw)||
               !Run->Sources.Contains(TPair<FString,int32>(Species,int32(Seed)))){
                UE_LOG(LogVoxelEarth,Error,TEXT("AppearanceForest invalid ecological source/position"));return;
            }
            Selected.Emplace(Species,int32(Seed));Run->LayoutPositions.Add(FVector(X/10.,Y/10.,0));Run->LayoutYaw.Add(int32(Yaw));
        }
        Run->Sources=MoveTemp(Selected);Run->Count=Run->Sources.Num();Run->EcologicalLayout=true;Run->LayoutWidthUU=Width/10.;
        UE_LOG(LogVoxelEarth,Log,TEXT("AppearanceForest ECOLOGY_LAYOUT count=%d path=%s terrain=projected treeOnly=1"),Run->Count,*LayoutPath);
    }
    Run->Anchor=PC->GetPawn()->GetActorLocation();Run->Anchor.X=FMath::GridSnap(Run->Anchor.X+2500,10.);Run->Anchor.Y=FMath::GridSnap(Run->Anchor.Y,10.);
    PC->ConsoleCommand(TEXT("t.MaxFPS 60"));PC->ConsoleCommand(TEXT("r.VSync 0"));
    W->GetTimerManager().SetTimer(Run->Timer,[Run](){
        auto World=Run->World.Get();if(!World)return;auto Controller=World->GetFirstPlayerController();auto Ground=World->GetSubsystem<UVoxelWorldSubsystem>();
        if(Run->Trees.Num()<Run->Count){
            const int I=Run->Trees.Num(),Width=FMath::CeilToInt(FMath::Sqrt(float(Run->Count)));
            FVector Pos=Run->Anchor+FVector((I/Width)*1500,(I%Width)*1500,0);
            if(Run->EcologicalLayout)Pos=Run->Anchor+Run->LayoutPositions[I];
            static const FIntPoint Offsets[]={{0,0},{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,1},{1,-1},{-1,-1},{2,0},{-2,0},{0,2},{0,-2},{2,2},{-2,2},{2,-2},{-2,-2}};
            const auto Offset=Offsets[Run->PlacementAttempt];Pos+=FVector(Offset.X*300,Offset.Y*300,0);
            Pos.Z=FMath::CeilToDouble(Ground->GetSurfaceHeightUU(Pos.X,Pos.Y)/10.)*10.+10.;
            auto Tree=World->SpawnActor<AVoxelEnvironmentLODPrototype>(Pos,FRotator(0,(Run->EcologicalLayout?Run->LayoutYaw[I]:I%4)*90,0));
            const auto& Source=Run->Sources[I%Run->Sources.Num()];const double Begin=FPlatformTime::Seconds();
            if(!Tree||!Tree->InitializePublishedTree(Source.Key,Source.Value,true)){
                if(Tree)Tree->Destroy();
                if(!Run->EcologicalLayout&&++Run->PlacementAttempt<UE_ARRAY_COUNT(Offsets))return;
                UE_LOG(LogVoxelEarth,Error,TEXT("AppearanceForest refused %s seed %d after bounded placement search"),*Source.Key,Source.Value);
                if(FParse::Param(FCommandLine::Get(),TEXT("VoxelAppearanceForestExit")))FPlatformMisc::RequestExit(false);
                World->GetTimerManager().ClearTimer(Run->Timer);return;
            }
            UE_LOG(LogVoxelEarth,Log,TEXT("AppearanceForest PLACEMENT i=%d attempt=%d position=%s"),I,Run->PlacementAttempt,*Tree->GetActorLocation().ToString());Run->PlacementAttempt=0;
            Run->Trees.Add(Tree);UE_LOG(LogVoxelEarth,Log,TEXT("AppearanceForest SPAWN i=%d species=%s seed=%d ms=%.3f"),I,*Source.Key,Source.Value,(FPlatformTime::Seconds()-Begin)*1000.);
            if(FParse::Param(FCommandLine::Get(),TEXT("VoxelAppearanceForestHidden")))Tree->SetActorHiddenInGame(true);
            return;
        }
        if(!Run->Camera.IsValid()){
            auto Camera=World->SpawnActor<ACameraActor>();Run->Camera=Camera;Camera->GetCameraComponent()->SetFieldOfView(60);
            const int Width=FMath::CeilToInt(FMath::Sqrt(float(Run->Count)));
            FVector Target=Run->Anchor+FVector((Width-1)*750,(Width-1)*750,800);Target.Z=Run->Trees[0]->GetActorLocation().Z+800;
            if(Run->EcologicalLayout)Target=Run->Anchor+FVector(0,0,800);
            const double Distance=Run->EcologicalLayout?Run->LayoutWidthUU*.8:2500.+(Width-1)*1300.;
            Camera->SetActorLocation(Target+FVector(-Distance,-Distance*.8,Distance*.45));Camera->SetActorRotation((Target-Camera->GetActorLocation()).Rotation());Controller->SetViewTarget(Camera);
            BeginPass(Run);UE_LOG(LogVoxelEarth,Log,TEXT("AppearanceForest READY count=%d"),Run->Count);return;
        }
        const double Now=FPlatformTime::Seconds();
        double Elapsed=Now-Run->Ready;
        // A fixed delay alone can measure the cold terrain fill. Poll the
        // existing live counters at 2 Hz and require five continuous quiet
        // seconds, including the fine-tile ring, before opening the window.
        const auto Progress=Ground->GetStreamingProgress();
        int32 Pending=0;
        for(int32 L=0;L<VoxelCoords::kNumLevels;++L)Pending+=Progress.LevelPendingCount[L];
        int32 FineReady=0,FineTotal=0;
        const bool FineSettled=Ground->IsFineRingSettled(FineReady,FineTotal);
        const bool Quiet=Progress.bSessionStarted&&Progress.TrackedChunks>0&&Pending==0&&Progress.TotalJobsInFlight==0&&FineSettled;
        if(Run->Stage==0){
            if(!Quiet)Run->QuietSince=-1;
            else if(Run->QuietSince<0)Run->QuietSince=Now;
            if(Elapsed<15||Run->QuietSince<0||Now-Run->QuietSince<5){
                if(Now-Run->LastWaitingLog>=10){
                    Run->LastWaitingLog=Now;
                    UE_LOG(LogVoxelEarth,Log,TEXT("AppearanceForest WAIT_STREAM pending=%d inFlight=%d fine=%d/%d tracked=%d"),Pending,Progress.TotalJobsInFlight,FineReady,FineTotal,Progress.TrackedChunks);
                }
                return;
            }
            UE_LOG(LogVoxelEarth,Log,TEXT("AppearanceForest STREAM_SETTLED pending=0 inFlight=0 fine=%d/%d quietSeconds=%.1f"),FineReady,FineTotal,Now-Run->QuietSince);
            Run->Ready=Now-15;Elapsed=15;
            if(FParse::Param(FCommandLine::Get(),TEXT("VoxelAppearanceForestCpuTrace"))){
                Controller->ConsoleCommand(FString::Printf(TEXT("Trace.File \"%s\" cpu,frame,bookmark"),*(Run->Output/TEXT("forest.utrace"))));
            }
            Controller->ConsoleCommand(TEXT("csvprofile start"));Run->Stage=1;
            UE_LOG(LogVoxelEarth,Log,TEXT("AppearanceForest MEASURE_BEGIN"));
        }else if(Run->Stage<5&&!Quiet){
            UE_LOG(LogVoxelEarth,Warning,TEXT("AppearanceForest STREAMING_BUSY pending=%d inFlight=%d fine=%d/%d"),Pending,Progress.TotalJobsInFlight,FineReady,FineTotal);
        }
        if(Run->Stage==1&&Elapsed>=25){FScreenshotRequest::RequestScreenshot(Run->Output/TEXT("forest-before.png"),false,false);Run->Stage=2;}
        if(Run->Stage==2&&Elapsed>=35){
            if(Run->EcologicalLayout){
                const FVector Eye=Run->Anchor+FVector(-Run->LayoutWidthUU*.45,0,0);
                const FVector Position(Eye.X,Eye.Y,Ground->GetSurfaceHeightUU(Eye.X,Eye.Y)+170);
                Run->Camera->SetActorLocation(Position);Run->Camera->SetActorRotation(FRotator(0,0,0));
                UE_LOG(LogVoxelEarth,Log,TEXT("AppearanceForest ECOLOGY_PLAYER_VIEW eyeHeightCm=170"));
                Run->Stage=3;return;
            }
            auto Tree=Run->Trees[0].Get();FVector Hit;const FVector Start=Tree->GetActorLocation()+FVector(-1800,0,150);
            const double Begin=FPlatformTime::Seconds();const bool Edited=Tree->Trace(Start,FVector::ForwardVector,3600,Hit)&&Tree->Carve(Hit,4);
            UE_LOG(LogVoxelEarth,Log,TEXT("AppearanceForest EDIT success=%d ms=%.3f"),int(Edited),(FPlatformTime::Seconds()-Begin)*1000.);
            Run->Stage=3;
        }
        if(Run->Stage==3&&Elapsed>=45){FScreenshotRequest::RequestScreenshot(Run->Output/TEXT("forest-after.png"),false,false);Run->Stage=4;}
        if(Run->Stage==4&&Elapsed>=55){
            Controller->ConsoleCommand(TEXT("csvprofile stop"));
            if(FParse::Param(FCommandLine::Get(),TEXT("VoxelAppearanceForestCpuTrace")))Controller->ConsoleCommand(TEXT("Trace.Stop"));
            Run->Stage=5;UE_LOG(LogVoxelEarth,Log,TEXT("AppearanceForest MEASURE_END"));
        }
        if(Run->Stage==5&&Elapsed>=60){UE_LOG(LogVoxelEarth,Log,TEXT("AppearanceForest COMPLETE count=%d"),Run->Count);
            if(Run->Sequence){
                UE_LOG(LogVoxelEarth,Log,TEXT("AppearanceForest PASS_COMPLETE name=%s"),PassName(Run->Pass));
                if(++Run->Pass<4){
                    // Restore the exact uncarved source for the next pass;
                    // terrain fit and actor transform were already verified.
                    auto Tree=Run->Trees[0].Get();const auto& Source=Run->Sources[0];
                    const auto OriginalTransform=Tree->GetActorTransform();
                    if(!Tree->InitializePublishedTree(Source.Key,Source.Value,false)||!Tree->GetActorTransform().Equals(OriginalTransform)){
                        UE_LOG(LogVoxelEarth,Error,TEXT("AppearanceForest sequence source reset failed"));
                        World->GetTimerManager().ClearTimer(Run->Timer);FPlatformMisc::RequestExit(false);return;
                    }
                    BeginPass(Run);return;
                }
                UE_LOG(LogVoxelEarth,Log,TEXT("AppearanceForest SEQUENCE_COMPLETE passes=4"));
            }
            if(FParse::Param(FCommandLine::Get(),TEXT("VoxelAppearanceForestExit")))FPlatformMisc::RequestExit(false);
            World->GetTimerManager().ClearTimer(Run->Timer);return;
        }
    },.5f,true);
}
}
void VoxelAppearanceForest::Start(UWorld* World){
    if(World&&FParse::Param(FCommandLine::Get(),TEXT("VoxelEcologyWorldCapture"))){LaunchEcologicalWorld(World);return;}
    if(!World||!FParse::Param(FCommandLine::Get(),TEXT("VoxelAppearanceForest")))return;
    FTimerHandle Timer;TWeakObjectPtr<UWorld> Weak=World;
    World->GetTimerManager().SetTimer(Timer,[Weak](){if(auto W=Weak.Get())Launch(W);},15.f,false);
}
