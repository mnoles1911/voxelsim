#include "VoxelDebris.h"
#include "VoxelAssetAppearance.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace {
FVector DebrisForward(FVector P,int Yaw){for(int I=0;I<Yaw;++I)P=FVector(-P.Y,P.X,P.Z);return P;}
TArray<uint8> DebrisMeshBytes(AVoxelDebris* Actor){TArray<uint8> B;FMemoryWriter W(B);TArray<UProceduralMeshComponent*> Cs;Actor->GetComponents(Cs);for(auto C:Cs)if(auto S=C->GetProcMeshSection(0)){for(auto V:S->ProcVertexBuffer)W<<V.Position<<V.Normal<<V.UV0<<V.UV1<<V.Color;auto Indices=S->ProcIndexBuffer;W<<Indices;float A=-1,N=-1,F=-1;auto M=C->GetMaterial(0);if(M){M->GetScalarParameterValue(FMaterialParameterInfo(TEXT("TreeAppearance")),A);M->GetScalarParameterValue(FMaterialParameterInfo(TEXT("TreeNeedle")),N);M->GetScalarParameterValue(FMaterialParameterInfo(TEXT("FoliageCutout")),F);}W<<A<<N<<F;}return B;}
void DebrisFreeze(AVoxelDebris* Actor){CastChecked<UStaticMeshComponent>(Actor->GetRootComponent())->SetSimulatePhysics(false);Actor->SetActorTickEnabled(false);}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelDebrisAppearanceTest,"Voxel.Appearance.DebrisRestore",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelDebrisAppearanceTest::RunTest(const FString&){
    auto World=UWorld::CreateWorld(EWorldType::Game,false,FName(*FGuid::NewGuid().ToString()));if(!TestNotNull(TEXT("isolated debris world"),World))return false;
    bool OK=true;
    for(int Yaw=0;Yaw<4&&OK;++Yaw)for(int Policy=0;Policy<3&&OK;++Policy){
        const TArray<VoxelCoords::FVoxelCoord> Coords={{-23,17,-5}};
        FVoxelDebrisCellAppearance C;C.Coord=Coords[0];C.Material=Policy==0?24:Policy==1?19:20;C.BaseRGB=FColor(123,178,67);C.SourceCell=FIntVector(3,5,7);C.SourceYawQuarter=uint8(Yaw);C.Approved=true;C.Needle=Policy==2;C.FoliageMask=Policy!=0;
        auto A=World->SpawnActor<AVoxelDebris>();if(!A||!TestEqual(TEXT("approved one-cell island admitted"),A->InitFromIslandWithAppearance(Coords,{C}),1)){OK=false;break;}DebrisFreeze(A);
        TArray<UProceduralMeshComponent*> Meshes;A->GetComponents(Meshes);TestEqual(TEXT("one explicit policy section"),Meshes.Num(),1);
        int Faces=0,Bad=0;
        for(auto M:Meshes)if(auto S=M->GetProcMeshSection(0)){
            float App=-1,Needle=-1,Mask=-1;auto Mat=M->GetMaterial(0);if(!TestNotNull(TEXT("debris appearance material"),Mat)){OK=false;break;}
            Mat->GetScalarParameterValue(FMaterialParameterInfo(TEXT("TreeAppearance")),App);Mat->GetScalarParameterValue(FMaterialParameterInfo(TEXT("TreeNeedle")),Needle);Mat->GetScalarParameterValue(FMaterialParameterInfo(TEXT("FoliageCutout")),Mask);
            TestEqual(TEXT("approved variation"),App,1.f);TestEqual(TEXT("explicit needle"),Needle,C.Needle?1.f:0.f);TestEqual(TEXT("petals opaque / legacy leaf cutout"),Mask,C.FoliageMask?1.f:0.f);
            for(int I=0;I<S->ProcVertexBuffer.Num();I+=4){
                const FVector WorldNormal=S->ProcVertexBuffer[I].Normal;int Axis=-1,Sign=0;
                // Independent forward normal rotation finds source face identity.
                for(int K=0;K<3;++K)for(int D=0;D<2;++D){FVector N=FVector::ZeroVector;N[K]=D?1.:-1.;if(DebrisForward(N,Yaw).Equals(WorldNormal,1.e-6)){Axis=K;Sign=D;}}
                if(Axis<0){++Bad;continue;}const auto Expected=FVoxelAssetAppearance::FaceColor(C.BaseRGB,C.SourceCell,Axis,Sign!=0).ToFColor(true);
                for(int J=0;J<4;++J){const auto& V=S->ProcVertexBuffer[I+J];if(V.Color.R!=Expected.R||V.Color.G!=Expected.G||V.Color.B!=Expected.B)++Bad;
                    bool Found=false;for(int X=0;X<2;++X)for(int Y=0;Y<2;++Y)for(int Z=0;Z<2;++Z){const FVector Corner(X,Y,Z);const auto P=DebrisForward(Corner-FVector(.5),Yaw)*10.;if(!P.Equals(V.Position,1.e-6))continue;
                        const auto Source=(FVector(C.SourceCell)+Corner)*.1;const FVector2D UV=Axis==2?FVector2D(Source.X,Source.Y):Axis==0?FVector2D(Source.Y,Source.Z):FVector2D(Source.X,Source.Z);Found=V.UV0.Equals(UV,1.e-5);}
                    if(!Found)++Bad;
                }++Faces;
            }
        }
        TestEqual(TEXT("six original-source faces"),Faces,6);TestEqual(TEXT("exact source face color / yaw-corner UV"),Bad,0);
        TSharedPtr<const TArray<uint8>,ESPMode::ThreadSafe> Geometry;TArray<uint8> Dynamic;
        if(!TestTrue(TEXT("capture appearance debris"),A->CaptureObjectState(Geometry,Dynamic))){OK=false;break;}
        auto B=World->SpawnActor<AVoxelDebris>();if(!B||!TestTrue(TEXT("sync appearance restore"),B->RestoreObjectState(Geometry,Dynamic))){OK=false;break;}
        TestTrue(TEXT("restored geometry RGB UV material flags exact"),DebrisMeshBytes(A)==DebrisMeshBytes(B));
        TSharedPtr<const TArray<uint8>,ESPMode::ThreadSafe> Again;TArray<uint8> AgainDynamic;TestTrue(TEXT("recapture restored debris"),B->CaptureObjectState(Again,AgainDynamic));
        TestTrue(TEXT("immutable appearance payload exact"),Again&&*Again==*Geometry);TestTrue(TEXT("dynamic physics/lifecycle state exact"),AgainDynamic==Dynamic);
        auto Invalid=C;Invalid.SourceYawQuarter=4;const auto Before=DebrisMeshBytes(A);TestEqual(TEXT("bad yaw refused"),A->InitFromIslandWithAppearance(Coords,{Invalid}),0);TestTrue(TEXT("refusal preserves existing actor"),Before==DebrisMeshBytes(A));
        Invalid=C;Invalid.Coord.X++;TestEqual(TEXT("coordinate mismatch refused"),A->InitFromIslandWithAppearance(Coords,{Invalid}),0);
        TArray<uint8> Corrupt=*Geometry;Corrupt.Last()=128;auto Refused=World->SpawnActor<AVoxelDebris>();TestFalse(TEXT("unknown serialized flags refused"),Refused->RestoreObjectState(Corrupt,Dynamic));
    }
    // Legacy coordinate-only saves keep the old positive-count byte layout and ISM.
    const TArray<VoxelCoords::FVoxelCoord> OldCoords={{1,2,3},{1,2,4}};auto Old=World->SpawnActor<AVoxelDebris>();
    if(Old){TestEqual(TEXT("old coordinate-only path"),Old->InitFromIsland(OldCoords),2);DebrisFreeze(Old);TSharedPtr<const TArray<uint8>,ESPMode::ThreadSafe> G;TArray<uint8> D;
        if(Old->CaptureObjectState(G,D)){TArray<uint8> Expected;FMemoryWriter W(Expected);int32 Count=2;W<<Count;for(auto C:OldCoords)W<<C.X<<C.Y<<C.Z;TestTrue(TEXT("legacy geometry remains byte-compatible"),*G==Expected);
            auto Restored=World->SpawnActor<AVoxelDebris>();TestTrue(TEXT("old save decodes"),Restored&&Restored->RestoreObjectState(G,D));if(Restored){auto ISM=Restored->FindComponentByClass<UInstancedStaticMeshComponent>();TestTrue(TEXT("legacy cubes retained"),ISM&&ISM->GetInstanceCount()==2);}}
        else{AddError(TEXT("legacy capture failed"));OK=false;}}
    else OK=false;
    // New snapshot preserves the original caller budget through restore.
    TArray<VoxelCoords::FVoxelCoord> Many;TArray<FVoxelDebrisCellAppearance> Cells;
    for(int X=0;X<3;++X)for(int Y=0;Y<3;++Y)for(int Z=0;Z<3;++Z){FVoxelDebrisCellAppearance C;C.Coord={X,Y,Z};C.Material=1;Many.Add(C.Coord);Cells.Add(C);}
    auto Budget=World->SpawnActor<AVoxelDebris>();if(Budget){TestEqual(TEXT("shell stride respects caller budget"),Budget->InitFromIslandWithAppearance(Many,Cells,4),4);DebrisFreeze(Budget);TSharedPtr<const TArray<uint8>,ESPMode::ThreadSafe> G;TArray<uint8> D;if(Budget->CaptureObjectState(G,D)){auto B=World->SpawnActor<AVoxelDebris>();TestTrue(TEXT("budget save restores"),B&&B->RestoreObjectState(G,D));if(B)TestTrue(TEXT("strided material fallback geometry exact"),DebrisMeshBytes(B)==DebrisMeshBytes(Budget));}}
    TArray<VoxelCoords::FVoxelCoord> MixedCoords;TArray<FVoxelDebrisCellAppearance> MixedCells;
    for(int I=0;I<4;++I){FVoxelDebrisCellAppearance C;C.Coord={I*3,0,0};C.Material=I==0?24:I==1?19:I==2?20:1;C.Approved=I<3;C.Needle=I==2;C.FoliageMask=I==1||I==2;C.BaseRGB=FColor(100,150,70);C.SourceCell=FIntVector(2,3,4);C.SourceYawQuarter=uint8(I);MixedCoords.Add(C.Coord);MixedCells.Add(C);}
    auto Mixed=World->SpawnActor<AVoxelDebris>();if(Mixed){TestEqual(TEXT("mixed source island admitted"),Mixed->InitFromIslandWithAppearance(MixedCoords,MixedCells),4);DebrisFreeze(Mixed);TArray<UProceduralMeshComponent*> Components;Mixed->GetComponents(Components);TestEqual(TEXT("distinct approved/fallback policy groups"),Components.Num(),4);
        TSharedPtr<const TArray<uint8>,ESPMode::ThreadSafe> G;TArray<uint8> D;if(Mixed->CaptureObjectState(G,D)){auto B=World->SpawnActor<AVoxelDebris>();TestTrue(TEXT("mixed appearance restore"),B&&B->RestoreObjectState(G,D));if(B)TestTrue(TEXT("mixed policy geometry preserved"),DebrisMeshBytes(B)==DebrisMeshBytes(Mixed));}else{AddError(TEXT("mixed capture failed"));OK=false;}}
    else OK=false;
    World->DestroyWorld(false);return OK;
}
#endif
