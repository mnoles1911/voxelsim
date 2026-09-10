#include "VoxelEcologicalRoute.h"
#include "VoxelEarthFlyPawn.h"
#include "VoxelCharacterMovement.h"
#include "VoxelWorldSubsystem.h"
#include "VoxelDetailAssetSubsystem.h"
#include "VoxelWalkTestSubsystem.h"
#include "VoxelFineTileStreamer.h"
#include "voxelcore/amplifier.h"
#include "voxelcore/foundationquery.h"
#include "voxelcore/assetfield.h"
#include "Engine/World.h"
#include "EngineGlobals.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealClient.h"
#include "ProfilingDebugging/CsvProfiler.h"
CSV_DEFINE_CATEGORY(VoxelRoute, true);
#include <openssl/sha.h>
namespace {
FString RouteDigest(const FString& Path){TArray<uint8> Bytes;uint8 Hash[32];
    if(!FFileHelper::LoadFileToArray(Bytes,*Path)||!SHA256(Bytes.GetData(),Bytes.Num(),Hash))return {};
    return BytesToHex(Hash,32).ToLower();}
bool HashValid(const FString& S){if(S.Len()!=64)return false;for(TCHAR C:S)if(!FChar::IsHexDigit(C))return false;return true;}
}
TSharedPtr<FVoxelEcologicalRoute> FVoxelEcologicalRoute::FromCommandLine(){return ParseArguments(FCommandLine::Get());}
TSharedPtr<FVoxelEcologicalRoute> FVoxelEcologicalRoute::ParseArguments(const TCHAR* Arguments){
    FString Path;if(!FParse::Value(Arguments,TEXT("VoxelEcologyRoute="),Path))return {};
    auto R=MakeShared<FVoxelEcologicalRoute>();R->RoutePath=Path;
    R->bProfileFrames=FParse::Param(Arguments,TEXT("VoxelEcologyRouteProfile"));
    R->bDiagnoseStalls=FParse::Param(Arguments,TEXT("VoxelEcologyRouteDiagnoseStalls"));
    FParse::Value(Arguments,TEXT("VoxelEcologyRouteSha256="),R->RouteHash);
    FParse::Value(Arguments,TEXT("VoxelEcologyRouteOutput="),R->Output);
    FParse::Value(Arguments,TEXT("VoxelEcologyConfig="),R->ConfigPath);
    FString Assets;FParse::Value(Arguments,TEXT("VoxelAssetDir="),Assets);
    R->SpeciesPath=Assets/TEXT("species.vxm");
    FString Json;TSharedPtr<FJsonObject> O;
    if(R->Output.IsEmpty()||!HashValid(R->RouteHash)||RouteDigest(Path)!=R->RouteHash.ToLower()||
       !FFileHelper::LoadFileToString(Json,*Path)||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json),O)||!O){
        R->Error=TEXT("Missing output, invalid route hash or invalid route JSON");return R;}
    double Version=0;const TArray<TSharedPtr<FJsonValue>>* Waypoints=nullptr;
    if(!O->TryGetNumberField(TEXT("version"),Version)||Version!=1||
       !O->TryGetStringField(TEXT("configuration_sha256"),R->ConfigHash)||
       !O->TryGetStringField(TEXT("species_manifest_sha256"),R->SpeciesHash)||
       !O->TryGetNumberField(TEXT("spawn_x_m"),R->SpawnX)||!O->TryGetNumberField(TEXT("spawn_y_m"),R->SpawnY)||
       !O->TryGetArrayField(TEXT("waypoints"),Waypoints)||Waypoints->Num()<1||Waypoints->Num()>32){R->Error=TEXT("Invalid route schema");return R;}
    if(!FMath::IsFinite(R->SpawnX)||!FMath::IsFinite(R->SpawnY)||FMath::Abs(R->SpawnX)>1.e7||FMath::Abs(R->SpawnY)>1.e7){R->Error=TEXT("Invalid spawn");return R;}
    for(const auto& V:*Waypoints){const TSharedPtr<FJsonObject>* P=nullptr;FPoint Q;
        if(!V->TryGetObject(P)||!P||!(*P)->TryGetNumberField(TEXT("x_m"),Q.X)||!(*P)->TryGetNumberField(TEXT("y_m"),Q.Y)||
           !(*P)->TryGetStringField(TEXT("label"),Q.Label)||!FMath::IsFinite(Q.X)||!FMath::IsFinite(Q.Y)||
           FMath::Abs(Q.X-R->SpawnX)>1000||FMath::Abs(Q.Y-R->SpawnY)>1000){R->Error=TEXT("Invalid waypoint");return R;}
        if((*P)->HasField(TEXT("arrival_radius_m"))&&(!(*P)->TryGetNumberField(TEXT("arrival_radius_m"),Q.ArrivalRadius)||
           !FMath::IsFinite(Q.ArrivalRadius)||Q.ArrivalRadius<.1||Q.ArrivalRadius>5.)){
            R->Error=TEXT("Invalid waypoint arrival radius (0.1 through 5 metres)");return R;}
        R->Points.Add(Q);}
    if(O->HasField(TEXT("foundation"))){
        const TSharedPtr<FJsonObject>* F=nullptr;double X=0,Y=0,Z=0;
        if(!O->TryGetObjectField(TEXT("foundation"),F)||!F||
           !(*F)->TryGetNumberField(TEXT("min_x_voxel"),X)||!(*F)->TryGetNumberField(TEXT("min_y_voxel"),Y)||
           !(*F)->TryGetNumberField(TEXT("plane_z_voxel"),Z)||!FMath::IsFinite(X)||!FMath::IsFinite(Y)||!FMath::IsFinite(Z)||
           FMath::Abs(X)>1.e8||FMath::Abs(Y)>1.e8||FMath::Abs(Z)>1.e7||
           X!=FMath::FloorToDouble(X)||Y!=FMath::FloorToDouble(Y)||Z!=FMath::FloorToDouble(Z)){
            R->Error=TEXT("Invalid explicit foundation voxel coordinates");return R;}
        const auto& Last=R->Points.Last();
        // Entire arrival disk must enter the footprint; no distant plot scan
        // can be presented as physical access to that foundation.
        if(Last.ArrivalRadius>.35||Last.X-Last.ArrivalRadius<X*.1||Last.X+Last.ArrivalRadius>(X+50)*.1||
           Last.Y-Last.ArrivalRadius<Y*.1||Last.Y+Last.ArrivalRadius>(Y+50)*.1){
            R->Error=TEXT("Foundation requires endpoint arrival radius <=0.35m wholly inside its footprint");return R;}
        R->bFoundationRequested=true;R->FoundationMinX=int64(X);R->FoundationMinY=int64(Y);R->FoundationPlaneZ=int64(Z);
    }
    if(!R->PinsMatch())R->Error=TEXT("Changed configuration, species or route");
    return R;
}
bool FVoxelEcologicalRoute::StartOutput(){
    if(bOutputStarted)return true;
    if(!PinsMatch()){Error=TEXT("Inputs changed before route start");return false;}
    if(IFileManager::Get().DirectoryExists(*Output)||IFileManager::Get().FileExists(*Output)){Error=TEXT("Route output must be fresh");return false;}
    TArray<uint8> Bytes;
    if(!FFileHelper::LoadFileToArray(Bytes,*RoutePath)){Error=TEXT("Cannot read pinned route");return false;}
    uint8 Hash[32];if(!SHA256(Bytes.GetData(),Bytes.Num(),Hash)||BytesToHex(Hash,32).ToLower()!=RouteHash.ToLower()){Error=TEXT("Route changed before copying");return false;}
    if(!IFileManager::Get().MakeDirectory(*Output,true)){Error=TEXT("Cannot create route output");return false;}
    bOutputStarted=true;
    if(!FFileHelper::SaveArrayToFile(Bytes,*(Output/TEXT("route.json")))){Error=TEXT("Cannot record route");return false;}
    return true;
}
bool FVoxelEcologicalRoute::PinsMatch()const{return HashValid(ConfigHash)&&HashValid(SpeciesHash)&&
    RouteDigest(RoutePath)==RouteHash.ToLower()&&RouteDigest(ConfigPath)==ConfigHash.ToLower()&&RouteDigest(SpeciesPath)==SpeciesHash.ToLower();}
bool FVoxelEcologicalRoute::StartProfile(double Now){
    if(!bProfileFrames)return true;
#if CSV_PROFILER
    auto Profiler=FCsvProfiler::Get();
    if(!bProfileRequested){
        if(FCsvProfiler::IsCapturing()||Profiler->IsWritingFile()||Profiler->IsEndCapturePending()){
            Error=TEXT("Route profiling requires exclusive CSV profiler ownership");return false;}
        ProfileRequestedAt=Now;bProfileRequested=true;
        Profiler->BeginCapture(-1,Output,TEXT("route-frames.csv"));
        return false; // capture begins on a frame boundary, never assume immediate
    }
    if(!FCsvProfiler::IsCapturing()){
        if(Now-ProfileRequestedAt>10)Error=TEXT("Route CSV capture failed to become active");
        return false;
    }
    if(!bProfileActive){
        bProfileActive=true;ProfileOrigin=Now;
        UE_LOG(LogVoxelWalkTest,Display,TEXT("VoxelRoute PROFILE_BEGIN mono=%.6f captureFrame=%d path=%s"),
            Now,Profiler->GetCaptureFrameNumber(),*(Output/TEXT("route-frames.csv")));
        RecordProfileFrame(Now,false);
    }
    return true;
#else
    Error=TEXT("Route profiling requires CSV profiler support");return false;
#endif
}
void FVoxelEcologicalRoute::RecordProfileFrame(double Now,bool Walking){
#if CSV_PROFILER
    if(!bProfileActive||!FCsvProfiler::IsCapturing())return;
    // Labels describe this route tick's decision, not a retroactive GPU frame.
    // Both transition frames are zero; analysis excludes zero-labeled frames.
    CSV_CUSTOM_STAT(VoxelRoute,Walking,int32(Walking),ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(VoxelRoute,Point,Point,ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(VoxelRoute,ElapsedSeconds,Now-ProfileOrigin,ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(VoxelRoute,CaptureFrame,FCsvProfiler::Get()->GetCaptureFrameNumber(),ECsvCustomStatOp::Set);
#endif
}
void FVoxelEcologicalRoute::Finish(UWorld* W,bool Success,const FString& Reason){
    if(Stage==EStage::Flushing||Stage==EStage::Done)return;
    if(auto PC=W?W->GetFirstPlayerController():nullptr)if(auto Pawn=Cast<AVoxelEarthFlyPawn>(PC->GetPawn()))Pawn->SetScriptedInput(0,0);
#if CSV_PROFILER
    if(bProfileRequested){
        const double Now=FPlatformTime::Seconds();RecordProfileFrame(Now,false);
        UE_LOG(LogVoxelWalkTest,Display,TEXT("VoxelRoute PROFILE_END mono=%.6f elapsed=%.6f captureFrame=%d"),Now,Now-ProfileOrigin,FCsvProfiler::Get()->GetCaptureFrameNumber());
        bFinishSuccess=Success;FinishReason=Reason;Stage=EStage::Flushing;
        ProfileWrite=FCsvProfiler::Get()->EndCapture();
        return; // future completion, not a fixed delay, owns the exit below
    }
#endif
    CompleteFinish(W,Success,Reason);
}
void FVoxelEcologicalRoute::CompleteFinish(UWorld* W,bool Success,const FString& Reason){
    if(auto PC=W?W->GetFirstPlayerController():nullptr)if(auto Pawn=Cast<AVoxelEarthFlyPawn>(PC->GetPawn()))Pawn->SetScriptedInput(0,0);
    // A stalled route remains FAIL regardless of diagnostic success. This runs
    // only after PROFILE_END/file flush (when profiling), outside walk labels.
    if(bStallSnapshot){
        const bool Saved=ExportStallDiagnostic(W);
        UE_LOG(LogVoxelWalkTest,Display,TEXT("VoxelRoute STALL_DIAGNOSTIC saved=%d; route remains FAIL"),int(Saved));
        Success=false;
    }
    Success=Success&&PinsMatch();
    const FString Summary=FString::Printf(TEXT("%s: %s; arrived=%d/%d; travel_m=%.3f; no broad navigability or building-support acceptance\n"),Success?TEXT("PASS"):TEXT("FAIL"),*Reason,Point,Points.Num(),Travel);
    if(bOutputStarted){
        if(!FFileHelper::SaveStringToFile(Csv,*(Output/TEXT("route-samples.csv")))||!FFileHelper::SaveStringToFile(Summary,*(Output/TEXT("result.txt"))))Success=false;}
    UE_LOG(LogVoxelWalkTest,Display,TEXT("VoxelRoute COMPLETE %s %s"),Success?TEXT("PASS"):TEXT("FAIL"),*Summary);
    Stage=EStage::Done;FPlatformMisc::RequestExit(false);
}
bool FVoxelEcologicalRoute::ExportStallDiagnostic(UWorld* W){
    const double QueryMono=FPlatformTime::Seconds();
    UE_LOG(LogVoxelWalkTest,Display,TEXT("VoxelRoute STALL_DIAGNOSTIC_BEGIN stallMono=%.6f queryMono=%.6f; outside walking/profile"),StallMono,QueryMono);
    auto O=MakeShared<FJsonObject>();O->SetNumberField(TEXT("schema"),1);
    O->SetStringField(TEXT("scope"),TEXT("Stopped body-box material evidence; no swept movement/step feasibility, placement edits, or species inference"));
    O->SetStringField(TEXT("route_sha256"),RouteHash);O->SetStringField(TEXT("configuration_sha256"),ConfigHash);O->SetStringField(TEXT("species_manifest_sha256"),SpeciesHash);
    O->SetBoolField(TEXT("pins_match_at_query"),PinsMatch());O->SetNumberField(TEXT("stall_mono_seconds"),StallMono);
    O->SetNumberField(TEXT("query_mono_seconds"),QueryMono);O->SetStringField(TEXT("stall_frame"),LexToString(StallFrame));
    O->SetNumberField(TEXT("point"),Point);O->SetNumberField(TEXT("target_x_m"),StallTargetX);O->SetNumberField(TEXT("target_y_m"),StallTargetY);
    O->SetBoolField(TEXT("grounded_at_stall"),bStallGrounded);O->SetBoolField(TEXT("waiting_at_stall"),bStallWaiting);
    O->SetNumberField(TEXT("step_height_uu"),StallStepUU);O->SetNumberField(TEXT("probe_translation_uu"),2.0);
    for(int A=0;A<3;++A){O->SetNumberField(FString::Printf(TEXT("stall_center_%d_uu"),A),StallPosition[A]);O->SetNumberField(FString::Printf(TEXT("body_half_extent_%d_uu"),A),StallHalfExtent[A]);}
    const double DX=FMath::Sign(StallTargetX*100.-StallPosition.X)*2.,DY=FMath::Sign(StallTargetY*100.-StallPosition.Y)*2.;
    const FVector Offsets[]={FVector::ZeroVector,FVector(-2,0,0),FVector(DX,0,0),FVector(0,DY,0)};
    const TCHAR* Names[]={TEXT("current"),TEXT("west_2cm"),TEXT("target_x_2cm"),TEXT("target_y_2cm")};
    auto Ground=W?W->GetSubsystem<UVoxelWorldSubsystem>():nullptr;
    TArray<TSharedPtr<FJsonValue>> Boxes;
    for(int Lift=0;Lift<2;++Lift)for(int I=0;I<4;++I){
        const FVector Center=StallPosition+Offsets[I]+FVector(0,0,Lift?StallStepUU:0);
        FString Json;TSharedPtr<FJsonObject> Box;
        if(Ground&&PinsMatch())Ground->DiagnoseRouteBodyBox(Center,StallHalfExtent,Json);
        if(Json.IsEmpty()||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json),Box)||!Box){
            Box=MakeShared<FJsonObject>();Box->SetBoolField(TEXT("composed_known"),false);Box->SetBoolField(TEXT("amplifier_known"),false);Box->SetStringField(TEXT("reason"),TEXT("World unavailable, pins changed, or diagnostic serialization failed"));}
        Box->SetStringField(TEXT("name"),FString(Names[I])+(Lift?TEXT("_step_up"):TEXT("")));
        for(int A=0;A<3;++A)Box->SetNumberField(FString::Printf(TEXT("center_%d_uu"),A),Center[A]);
        Boxes.Add(MakeShared<FJsonValueObject>(Box));
    }
    O->SetArrayField(TEXT("boxes"),Boxes);O->SetNumberField(TEXT("query_end_mono_seconds"),FPlatformTime::Seconds());
    FString Json;return bOutputStarted&&FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&Json))&&
        FFileHelper::SaveStringToFile(Json,*(Output/TEXT("stall-material-diagnostic.json")));
}
bool FVoxelEcologicalRoute::ExportFoundation(UWorld* W){
    auto Ground=W->GetSubsystem<UVoxelWorldSubsystem>();auto PC=W->GetFirstPlayerController();
    auto Pawn=PC?Cast<AVoxelEarthFlyPawn>(PC->GetPawn()):nullptr;
    if(!Ground||!Pawn||!PinsMatch())return false;
    Pawn->SetScriptedInput(0,0);
    const auto Pos=Pawn->GetActorLocation();const auto& Last=Points.Last();
    if(!Pawn->IsGroundedNow()||Pawn->IsWaitingForTerrain()||
       FVector2D::Distance(FVector2D(Pos.X/100.,Pos.Y/100.),FVector2D(Last.X,Last.Y))>Last.ArrivalRadius)return false;
    UE_LOG(LogVoxelWalkTest,Display,TEXT("VoxelRoute FOUNDATION_BEGIN outside WALK interval"));
    vxc::FoundationSurvey Survey;FString Reason;
    const bool Known=Ground->SurveyFoundation(FoundationMinX,FoundationMinY,FoundationPlaneZ,Survey,Reason);
    FString Columns=TEXT("x_voxel,y_voxel,ground_found,first_ground_z_voxel,fill_gap_mm,bearing_thickness_mm,first_void_depth_mm,material_unknown,water_known,water_mm,overhead_occupied_voxels,non_ground_below_plane_voxels,provisional_criteria\n");
    for(const auto& C:Survey.columns)Columns+=FString::Printf(TEXT("%lld,%lld,%d,%lld,%d,%d,%d,%d,%d,%d,%d,%d,%d\n"),
        C.x,C.y,int(C.groundFound),C.firstGroundZ,C.fillGapMm,C.bearingThicknessMm,C.firstVoidDepthMm,
        int(C.materialUnknown),int(C.waterKnown),C.waterMm,C.overheadOccupiedVoxels,C.nonGroundBelowPlaneVoxels,int(C.meetsProvisionalReportingCriteria));
    auto O=MakeShared<FJsonObject>();O->SetNumberField(TEXT("schema"),1);
    O->SetStringField(TEXT("scope"),TEXT("One reached footprint; finite support and clearance survey, no structural or broad building approval"));
    O->SetStringField(TEXT("route_sha256"),RouteHash);O->SetStringField(TEXT("configuration_sha256"),ConfigHash);O->SetStringField(TEXT("species_manifest_sha256"),SpeciesHash);
    O->SetNumberField(TEXT("min_x_voxel"),double(FoundationMinX));O->SetNumberField(TEXT("min_y_voxel"),double(FoundationMinY));O->SetNumberField(TEXT("plane_z_voxel"),double(FoundationPlaneZ));
    O->SetNumberField(TEXT("voxel_mm"),100);O->SetNumberField(TEXT("width_mm"),5000);O->SetNumberField(TEXT("clearance_mm"),3000);
    O->SetNumberField(TEXT("max_fill_mm_provisional"),500);O->SetNumberField(TEXT("bearing_inspection_mm"),1000);
    O->SetStringField(TEXT("depth_scope"),TEXT("Includes top ground voxel; deeper voids unexamined; no fill or terrain edits applied"));
    O->SetNumberField(TEXT("column_count"),double(Survey.columns.size()));O->SetNumberField(TEXT("unknown_columns"),Survey.unknownColumns);
    O->SetNumberField(TEXT("wet_columns"),Survey.wetColumns);O->SetNumberField(TEXT("obstructed_columns"),Survey.obstructedColumns);
    O->SetNumberField(TEXT("provisional_criteria_columns"),Survey.provisionalCriteriaColumns);
    O->SetBoolField(TEXT("complete_known_survey"),Known);O->SetBoolField(TEXT("approved_build_site"),false);O->SetStringField(TEXT("reason"),Reason);
    O->SetNumberField(TEXT("pawn_x_m"),Pos.X/100.);O->SetNumberField(TEXT("pawn_y_m"),Pos.Y/100.);O->SetNumberField(TEXT("pawn_z_m"),Pos.Z/100.);
    FString Json;const bool Saved=FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&Json))&&
        FFileHelper::SaveStringToFile(Columns,*(Output/TEXT("foundation-columns.csv")))&&
        FFileHelper::SaveStringToFile(Json,*(Output/TEXT("foundation-summary.json")));
    UE_LOG(LogVoxelWalkTest,Display,TEXT("VoxelRoute FOUNDATION_END known=%d saved=%d criteria=%u/2500; no approval"),int(Known),int(Saved),Survey.provisionalCriteriaColumns);
    return Known&&Saved&&PinsMatch();
}
void FVoxelEcologicalRoute::Tick(UWorld* W,float Dt){
    if(Stage==EStage::Done)return;
    if(Stage==EStage::Flushing){
        // Continue ticking until asynchronous file writing completes. A hung
        // writer is an external wrapper timeout/failure, never a PASS/early quit.
        if(!ProfileWrite.IsValid()){
#if CSV_PROFILER
            // EndCapture can decline if capture never started or ended early.
            // Still do not quit while a profiler write/stop remains pending.
            if(FCsvProfiler::IsCapturing()||FCsvProfiler::Get()->IsWritingFile()||FCsvProfiler::Get()->IsEndCapturePending())return;
#endif
            CompleteFinish(W,false,TEXT("Route CSV stop returned no completion future"));return;
        }
        if(!ProfileWrite.IsReady())return;
        const FString Saved=ProfileWrite.Get();
        const bool Written=FPaths::IsSamePath(Saved,Output/TEXT("route-frames.csv"))&&IFileManager::Get().FileSize(*Saved)>0;
        UE_LOG(LogVoxelWalkTest,Display,TEXT("VoxelRoute PROFILE_SAVED ok=%d path=%s"),int(Written),*Saved);
        CompleteFinish(W,bFinishSuccess&&Written,Written?FinishReason:TEXT("Route CSV export failed"));return;
    }
    if(!Error.IsEmpty()){Finish(W,false,Error);return;}
    const double Now=FPlatformTime::Seconds();RecordProfileFrame(Now,Stage==EStage::Walking);if(Started==0){Started=Now;StageStart=Now;}
    if(Now-Started>3600){Finish(W,false,TEXT("Overall wall-clock timeout"));return;}
    auto PC=W?W->GetFirstPlayerController():nullptr;auto Pawn=PC?Cast<AVoxelEarthFlyPawn>(PC->GetPawn()):nullptr;
    auto Ground=W?W->GetSubsystem<UVoxelWorldSubsystem>():nullptr;auto Details=W?W->GetSubsystem<UVoxelDetailAssetSubsystem>():nullptr;
    if(!Pawn||!Ground||!Details){if(Now-Started>1800)Finish(W,false,TEXT("Startup services timeout"));return;}
    const auto Pos=Pawn->GetActorLocation();auto Mover=Pawn->GetWalkMovement();if(!Mover)return;
    // Subsystem initialization also runs for pre-play worlds. Only a live
    // game world with the actual player may claim the fresh output directory.
    if(!W->IsGameWorld()||!W->HasBegunPlay())return;
    if(!StartOutput()){Finish(W,false,Error);return;}
    if(Stage==EStage::Waiting){
        Pawn->SetScriptedInput(0,0);
        if(Now-Started>1800){Finish(W,false,TEXT("Initial streaming/grounding timeout"));return;}
        const auto Progress=Ground->GetStreamingProgress();int Pending=0;for(int L=0;L<8;++L)Pending+=Progress.LevelPendingCount[L];
        int Ready=0,Total=0;uint64 Instances=0;
        const auto Field=Ground->GetAssetField();
        const bool Settled=Field&&Field->ecologyEnabled()&&Ground->IsFineRingSettled(Ready,Total)&&Pending==0&&Progress.TotalJobsInFlight==0&&Details->IsPlacementSettled(Instances);
        if(!Settled){Quiet=-1;return;}if(Quiet<0)Quiet=Now;if(Now-Quiet<5)return;
        if(FVector2D::Distance(FVector2D(Pos.X/100.,Pos.Y/100.),FVector2D(SpawnX,SpawnY))>4){Finish(W,false,TEXT("Spawn differs from pinned route"));return;}
        if(!bMovementStarted){Pawn->SetWalkMode(true);Mover->AdjustSpeedTier(2-Mover->GetSpeedTierIndex());bMovementStarted=true;return;}
        if(!Pawn->IsGroundedNow())return;
        if(!StartProfile(Now))return;
        Stage=EStage::Walking;StageStart=LastProgress=Now;LastPosition=Pos;LastSample=Now;LastSamplePosition=Pos;
        UE_LOG(LogVoxelWalkTest,Display,TEXT("VoxelRoute WALK_BEGIN point=%d mono=%.6f"),Point,Now);
    }
    if(Stage==EStage::Checkpoint){
        Pawn->SetScriptedInput(0,0);if(Now-StageStart<2)return;
        if(!IFileManager::Get().FileExists(*(Output/FString::Printf(TEXT("checkpoint-%02d.png"),Point)))){Finish(W,false,TEXT("Checkpoint screenshot missing"));return;}
        ++Point;if(Point==Points.Num()){
            const bool SurveyOk=!bFoundationRequested||ExportFoundation(W);
            Finish(W,SurveyOk,SurveyOk?TEXT("All authored waypoints reached; optional foundation evidence recorded, not approved"):TEXT("Foundation evidence incomplete or export failed"));return;}
        Stage=EStage::Walking;StageStart=LastProgress=Now;BestDistance=DBL_MAX;LastPosition=Pos;LastSample=Now;LastSamplePosition=Pos;
        UE_LOG(LogVoxelWalkTest,Display,TEXT("VoxelRoute WALK_BEGIN point=%d mono=%.6f"),Point,Now);
    }
    const auto& Target=Points[Point];const FVector Delta(Target.X*100.-Pos.X,Target.Y*100.-Pos.Y,0);
    const double Distance=Delta.Size()/100.;Travel+=FVector::Dist2D(Pos,LastPosition)/100.;LastPosition=Pos;
    if(Distance<BestDistance-.25){BestDistance=Distance;LastProgress=Now;}
    const bool bArrived=Distance<=Target.ArrivalRadius&&Pawn->IsGroundedNow()&&!Pawn->IsWaitingForTerrain();
    // Preserve endpoint evidence even for a leg shorter than the sample period.
    if(Now>LastSample&&(Now-LastSample>=.5||bArrived)){
        // Displacement over this entire CSV interval, not the mover's desired
        // velocity or a noisy one-frame estimate. Each walk phase resets both
        // baselines, excluding startup falls and checkpoint pauses.
        const double ActualSpeed=FVector::Dist2D(Pos,LastSamplePosition)/(100.*(Now-LastSample));
        int Water=-1,Slope=-1,Biome=-1;const auto Amp=Ground->GetWorldgenAmplifier();const auto Streamer=Ground->GetFineTileStreamer();
        const int64 X=FMath::FloorToInt64(Pos.X/10.),Y=FMath::FloorToInt64(Pos.Y/10.);
        if(Amp&&(!Streamer||Streamer->IsFootprintResident(X*100,Y*100,X*100+100,Y*100+100))){const auto C=Amp->column(X,Y);auto Channels=Ground->GetAssetChannelSource();const auto F=vxc::assetColumnFactsFromSample(C,Channels?Channels->channelsAt(X,Y):vxc::AssetColumnChannels{});Water=F.standingWaterMm;Slope=int(F.slopeMmPerM);Biome=int(F.biome);}
        Csv+=FString::Printf(TEXT("%.3f,walk,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f\n"),Now-Started,Point,Pos.X/100.,Pos.Y/100.,Pos.Z/100.,Mover->GetHorizontalSpeedUU()/100.,ActualSpeed,Pawn->IsGroundedNow()?1:0,Pawn->IsWaitingForTerrain()?1:0,Water,Slope,Biome,Distance,Travel,Dt*1000.);LastSample=Now;LastSamplePosition=Pos;}
    if(bArrived){
        RecordProfileFrame(Now,false);
        Pawn->SetScriptedInput(0,0);Stage=EStage::Checkpoint;StageStart=Now;
        UE_LOG(LogVoxelWalkTest,Display,TEXT("VoxelRoute WALK_END arrived=%d mono=%.6f; screenshot pause excluded"),Point,Now);
        FScreenshotRequest::RequestScreenshot(Output/FString::Printf(TEXT("checkpoint-%02d.png"),Point),false,false);return;}
    if(Now-StageStart>180||Now-LastProgress>15){
        if(bDiagnoseStalls){bStallSnapshot=true;StallPosition=Pos;
            StallHalfExtent=FVector(VoxelMovementTuning::BoxHalfExtentXY,VoxelMovementTuning::BoxHalfExtentXY,Mover->GetHalfExtentZ());
            StallStepUU=VoxelMovementTuning::StepUpHeightUU;StallMono=Now;StallFrame=GFrameCounter;
            StallTargetX=Target.X;StallTargetY=Target.Y;bStallGrounded=Pawn->IsGroundedNow();bStallWaiting=Pawn->IsWaitingForTerrain();}
        UE_LOG(LogVoxelWalkTest,Display,TEXT("VoxelRoute WALK_ABORT point=%d mono=%.6f; stalled, following diagnostics excluded"),Point,Now);
        Finish(W,false,TEXT("Waypoint stalled or exceeded leg timeout; route is not demonstrated connected"));return;
    }
    PC->SetControlRotation(FRotator(0,Delta.Rotation().Yaw,0));Pawn->SetScriptedInput(1,0);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelRouteParserTest,"Voxel.Ecology.RouteParserSideEffects",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelRouteParserTest::RunTest(const FString&){
    const FString Root=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation")/FGuid::NewGuid().ToString());
    IFileManager::Get().MakeDirectory(*Root,true);
    const FString Config=Root/TEXT("config.json"),Species=Root/TEXT("species.vxm"),Route=Root/TEXT("route.json"),Output=Root/TEXT("output");
    FFileHelper::SaveStringToFile(TEXT("config"),*Config);FFileHelper::SaveStringToFile(TEXT("species"),*Species);
    const FString Json=FString::Printf(TEXT("{\"version\":1,\"configuration_sha256\":\"%s\",\"species_manifest_sha256\":\"%s\",\"spawn_x_m\":0,\"spawn_y_m\":0,\"waypoints\":[{\"x_m\":1,\"y_m\":2,\"label\":\"test\"}]}"),*RouteDigest(Config),*RouteDigest(Species));
    FFileHelper::SaveStringToFile(Json,*Route);
    const FString Args=FString::Printf(TEXT("-VoxelEcologyRoute=\"%s\" -VoxelEcologyRouteSha256=%s -VoxelEcologyRouteOutput=\"%s\" -VoxelEcologyConfig=\"%s\" -VoxelAssetDir=\"%s\""),*Route,*RouteDigest(Route),*Output,*Config,*Root);
    auto A=FVoxelEcologicalRoute::ParseArguments(*Args),B=FVoxelEcologicalRoute::ParseArguments(*Args);
    TestTrue(TEXT("repeated subsystem parse succeeds"),A&&B&&A->Error.IsEmpty()&&B->Error.IsEmpty());
    TestFalse(TEXT("parsing does not create output"),IFileManager::Get().DirectoryExists(*Output));
    if(A)TestFalse(TEXT("profiling default remains disabled"),A->bProfileFrames);
    const auto Profile=FVoxelEcologicalRoute::ParseArguments(*(Args+TEXT(" -VoxelEcologyRouteProfile")));
    TestTrue(TEXT("explicit profiling flag parsed without starting capture"),Profile&&Profile->bProfileFrames&&!Profile->bProfileRequested&&!Profile->bProfileActive);
    TestFalse(TEXT("profiling parse does not create output"),IFileManager::Get().DirectoryExists(*Output));
    if(A)TestFalse(TEXT("stall diagnostics default disabled"),A->bDiagnoseStalls);
    const auto Diagnose=FVoxelEcologicalRoute::ParseArguments(*(Args+TEXT(" -VoxelEcologyRouteDiagnoseStalls")));
    TestTrue(TEXT("stall diagnostic flag does not create a snapshot during parse"),Diagnose&&Diagnose->bDiagnoseStalls&&!Diagnose->bStallSnapshot);
    if(A&&B){TestTrue(TEXT("first live owner claims output"),A->StartOutput());TestFalse(TEXT("second owner refuses existing output"),B->StartOutput());TestEqual(TEXT("route copy preserves pinned bytes"),RouteDigest(Output/TEXT("route.json")),RouteDigest(Route));}
    // Optional local turn precision does not change the default route radius.
    if(A)TestEqual(TEXT("omitted arrival radius keeps original default"),A->Points[0].ArrivalRadius,1.5);
    const FString OriginalHash=RouteDigest(Route);
    for(double Radius:{0.,.05,.1,.35,5.,5.1}){
        const FString WithRadius=Json.Replace(TEXT("\"x_m\":1"),*FString::Printf(TEXT("\"arrival_radius_m\":%.3f,\"x_m\":1"),Radius));
        FFileHelper::SaveStringToFile(WithRadius,*Route);
        const FString RadiusArgs=Args.Replace(*OriginalHash,*RouteDigest(Route));
        const auto Parsed=FVoxelEcologicalRoute::ParseArguments(*RadiusArgs);
        const bool Valid=Radius>=.1&&Radius<=5.;
        TestEqual(TEXT("arrival radius range enforced"),Parsed&&Parsed->Error.IsEmpty(),Valid);
        if(Valid&&Parsed&&Parsed->Error.IsEmpty())TestEqual(TEXT("explicit radius retained"),Parsed->Points[0].ArrivalRadius,Radius);
    }
    for(const auto& Pair:TArray<TPair<FString,bool>>{
        {TEXT("{\"min_x_voxel\":-15,\"min_y_voxel\":-5,\"plane_z_voxel\":0}"),true},
        {TEXT("{\"min_x_voxel\":-15.5,\"min_y_voxel\":-5,\"plane_z_voxel\":0}"),false},
        {TEXT("{\"min_x_voxel\":100,\"min_y_voxel\":100,\"plane_z_voxel\":0}"),false}}){
        FString WithFoundation=Json.Replace(TEXT("\"x_m\":1"),TEXT("\"arrival_radius_m\":0.35,\"x_m\":1"));
        WithFoundation.RemoveAt(WithFoundation.Len()-1);WithFoundation+=TEXT(",\"foundation\":")+Pair.Key+TEXT("}");
        FFileHelper::SaveStringToFile(WithFoundation,*Route);
        const auto Parsed=FVoxelEcologicalRoute::ParseArguments(*Args.Replace(*OriginalHash,*RouteDigest(Route)));
        TestEqual(TEXT("foundation explicit bounded coordinates and reached footprint"),Parsed&&Parsed->Error.IsEmpty(),Pair.Value);
    }
    IFileManager::Get().Delete(*(Output/TEXT("route.json")));IFileManager::Get().DeleteDirectory(*Output);
    for(const auto& File:{Config,Species,Route})IFileManager::Get().Delete(*File);
    IFileManager::Get().DeleteDirectory(*Root);return true;
}
#endif
