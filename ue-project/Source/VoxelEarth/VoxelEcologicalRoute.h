#pragma once
#include "CoreMinimal.h"
#include "Async/Future.h"
class UWorld;
// Opt-in real-terrain traversal. It drives the actual pawn, never relocates it.
class FVoxelEcologicalRoute {
public:
    static TSharedPtr<FVoxelEcologicalRoute> FromCommandLine();
    static TSharedPtr<FVoxelEcologicalRoute> ParseArguments(const TCHAR* Arguments);
    void Tick(UWorld* World,float DeltaTime);
private:
    friend class FVoxelRouteParserTest;
    struct FPoint { double X=0,Y=0,ArrivalRadius=1.5; FString Label; };
    enum class EStage { Waiting,Walking,Checkpoint,Flushing,Done };
    EStage Stage=EStage::Waiting;
    TArray<FPoint> Points;
    FString RoutePath,RouteHash,ConfigPath,ConfigHash,SpeciesPath,SpeciesHash,Output,Error;
    FString Csv=TEXT("seconds,stage,waypoint,x_m,y_m,z_m,controller_speed_m_s,actual_speed_m_s,grounded,waiting,water_mm,slope_mm_per_m,biome,distance_m,travel_m,frame_ms\n");
    double Started=0,StageStart=0,Quiet=-1,LastProgress=0,LastSample=-1,BestDistance=DBL_MAX,Travel=0;
    double SpawnX=0,SpawnY=0;
    FVector LastPosition=FVector::ZeroVector,LastSamplePosition=FVector::ZeroVector;
    int32 Point=0;
    bool bMovementStarted=false,bOutputStarted=false,bFoundationRequested=false;
    // THE PACE THE ROUTE WALKS AT, and why it is a parameter now.
    //
    // Every slice number this project has is from tier 2 (Stride, 2.2 m/s),
    // because this driver hard-coded that tier. The standing target is written
    // for 20 m/s, so the slice has never once been measured under the condition
    // it must pass, and streaming cost scales with how fast new ground arrives.
    //
    // ON FOOT 20 m/s DOES NOT EXIST: the dial's top tier is 9.5 m/s
    // (VoxelMovementTuning::kSpeedTiersUU, "Mad dash"), which is the fastest a
    // player can cross this ground. A 20 m/s leg is therefore a FLY-pawn
    // measurement and a different pawn; this knob is what makes the on-foot
    // half of that comparison possible at all.
    //
    // Default 2 keeps every existing capture byte-comparable.
    int32 SpeedTier=2;
    int64 FoundationMinX=0,FoundationMinY=0,FoundationPlaneZ=0;
    bool bProfileFrames=false,bProfileRequested=false,bProfileActive=false,bFinishSuccess=false;
    double ProfileRequestedAt=0,ProfileOrigin=0;
    FString FinishReason;
    TSharedFuture<FString> ProfileWrite;
    bool StartProfile(double Now);
    void RecordProfileFrame(double Now,bool Walking);
    void CompleteFinish(UWorld* World,bool Success,const FString& Reason);
    bool bDiagnoseStalls=false,bStallSnapshot=false;
    FVector StallPosition=FVector::ZeroVector,StallHalfExtent=FVector::ZeroVector;
    double StallMono=0,StallTargetX=0,StallTargetY=0,StallStepUU=0;
    uint64 StallFrame=0;
    bool bStallGrounded=false,bStallWaiting=false;
    bool ExportStallDiagnostic(UWorld* World);
    bool ExportFoundation(UWorld* World);
    bool StartOutput();
    void Finish(UWorld* World,bool Success,const FString& Reason);
    bool PinsMatch() const;
};
