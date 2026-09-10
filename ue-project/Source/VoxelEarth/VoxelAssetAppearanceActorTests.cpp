#include "VoxelAssetAppearance.h"
#include "VoxelEnvironmentLODPrototype.h"
#include "VoxelTreeFellingPrototype.h"
#include "VoxelVegetationRender.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"
#include "ProceduralMeshComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "HAL/FileManager.h"
#include <openssl/sha.h>
#include "voxelcore/assetgrid.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace {
void Append32(TArray<uint8>& B,uint32 V){for(int I=0;I<4;++I)B.Add(uint8(V>>(I*8)));}
void Write32(TArray<uint8>& B,int O,uint32 V){for(int I=0;I<4;++I)B[O+I]=uint8(V>>(I*8));}
FColor CellRGB(FIntVector C){return FColor(80+C.X*8,70+C.Y*9,60+C.Z*10);}
TArray<uint8> MeshSnapshot(AActor* Actor){
    TArray<uint8> Bytes;FMemoryWriter Writer(Bytes);TArray<UProceduralMeshComponent*> Components;Actor->GetComponents(Components);
    for(auto Component:Components)if(auto S=Component->GetProcMeshSection(0)){
        for(auto V:S->ProcVertexBuffer){Writer<<V.Position<<V.Normal<<V.UV0<<V.Color;}
        auto Indices=S->ProcIndexBuffer;Writer<<Indices;
    }
    return Bytes;
}
struct FAppearanceRestoreStatus {bool Done=false,Success=false;};
bool TimberMaterialMode(AActor* Actor,bool Needle){
    TArray<UProceduralMeshComponent*> Components;Actor->GetComponents(Components);
    if(Components.IsEmpty())return false;
    for(auto C:Components){float A=0,N=0;auto M=C->GetMaterial(0);if(!M)return false;
        M->GetScalarParameterValue(FMaterialParameterInfo(TEXT("TreeAppearance")),A);
        M->GetScalarParameterValue(FMaterialParameterInfo(TEXT("TreeNeedle")),N);
        if(A!=1.f||N!=(Needle?1.f:0.f))return false;
    }return true;
}
class FAppearanceTimberProbe final:public IAutomationLatentCommand {
    FAutomationTestBase* Test;UWorld* World;AVoxelFallingTimber* Actor;
    TSharedPtr<FAppearanceRestoreStatus> Status;TArray<uint8> Expected;bool Needle;double Started;
public:
    FAppearanceTimberProbe(FAutomationTestBase* T,UWorld* W,AVoxelFallingTimber* A,TSharedPtr<FAppearanceRestoreStatus> S,TArray<uint8> E,bool N)
        :Test(T),World(W),Actor(A),Status(S),Expected(MoveTemp(E)),Needle(N),Started(FPlatformTime::Seconds()){}
    bool Update() override {
        Actor->AdvanceStagedObjectRestore();
        if(!Status->Done&&FPlatformTime::Seconds()-Started<60)return false;
        if(Test->TestTrue(TEXT("staged timber restore completes"),Status->Done&&Status->Success)){
            Test->TestTrue(TEXT("staged timber hidden before publication"),Actor->IsHidden());
            Test->TestTrue(TEXT("publish staged timber"),Actor->PublishStagedObjectRestore());
            Test->TestTrue(TEXT("staged timber preserves source appearance and leaf mode"),TimberMaterialMode(Actor,Needle));
            Test->TestTrue(TEXT("staged timber preserves all geometry colors and UVs"),MeshSnapshot(Actor)==Expected);
        }
        World->DestroyWorld(false);return true;
    }
};
class FAppearanceStagedProbe final:public IAutomationLatentCommand {
    FAutomationTestBase* Test;UWorld* World;AVoxelEnvironmentLODPrototype* Actor;
    TSharedPtr<FAppearanceRestoreStatus> Status;TArray<uint8> Expected;FString Path;FVector Hole;double Started;
public:
    FAppearanceStagedProbe(FAutomationTestBase* T,UWorld* W,AVoxelEnvironmentLODPrototype* A,TSharedPtr<FAppearanceRestoreStatus> S,TArray<uint8> E,FString P,FVector H)
        :Test(T),World(W),Actor(A),Status(S),Expected(MoveTemp(E)),Path(MoveTemp(P)),Hole(H),Started(FPlatformTime::Seconds()){}
    bool Update() override {
        Actor->AdvanceStagedObjectRestore();
        if(!Status->Done&&FPlatformTime::Seconds()-Started<60)return false;
        if(Test->TestTrue(TEXT("staged appearance restore completes"),Status->Done&&Status->Success)){
            Test->TestTrue(TEXT("staged actor remains hidden before publication"),Actor->IsHidden());
            Test->TestTrue(TEXT("publish staged appearance actor"),Actor->PublishStagedObjectRestore());
            Test->TestFalse(TEXT("staged restore preserves carved air"),Actor->SolidAt(Hole));
            Test->TestTrue(TEXT("staged restore preserves all positions normals colors UVs and indices"),MeshSnapshot(Actor)==Expected);
        }
        World->DestroyWorld(false);IFileManager::Get().Delete(*Path);return true;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelAppearanceActorTest,"Voxel.Appearance.EditableActor",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelAppearanceActorTest::RunTest(const FString&){
    FString Directory;
    if(!FParse::Value(FCommandLine::Get(),TEXT("VoxelAssetAppearanceDir="),Directory)){
        AddError(TEXT("Supply a private VoxelAssetAppearanceDir for this filesystem-backed actor test"));return false;
    }
    TArray<uint8> Vxa;Vxa.Append({'V','X','A','1'});
    for(uint32 V:{3u,0u,0u,0u,4u,4u,4u,100u,1u,0u,0u})Append32(Vxa,V);
    Vxa.Add(16);Append32(Vxa,64);
    const FString MD5=FMD5::HashBytes(Vxa.GetData(),Vxa.Num());
    TArray<uint8> Packet;Packet.SetNumZeroed(128+64*10);FMemory::Memcpy(Packet.GetData(),"VAC1",4);
    Write32(Packet,4,1);for(int O:{8,12,16})Write32(Packet,O,4);Write32(Packet,32,100);Write32(Packet,36,64);
    HexToBytes(MD5,Packet.GetData()+48);
    for(int X=0;X<4;++X)for(int Y=0;Y<4;++Y)for(int Z=0;Z<4;++Z){
        uint8* R=Packet.GetData()+128+((X*4+Y)*4+Z)*10;R[0]=X;R[2]=Y;R[4]=Z;R[6]=16;
        auto C=CellRGB(FIntVector(X,Y,Z));R[7]=C.R;R[8]=C.G;R[9]=C.B;
    }
    TArray<uint8> Checked;Checked.Append(Packet.GetData(),96);Checked.Append(Packet.GetData()+128,640);
    SHA256(Checked.GetData(),Checked.Num(),Packet.GetData()+96);
    IFileManager::Get().MakeDirectory(*Directory,true);const FString Path=Directory/(MD5+TEXT(".vac"));
    if(IFileManager::Get().FileExists(*Path)){AddError(TEXT("Private fixture path already exists; use a clean test directory"));return false;}
    if(!FFileHelper::SaveArrayToFile(Packet,*Path)){AddError(TEXT("write private appearance fixture"));return false;}
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,FName(*FGuid::NewGuid().ToString()));
    if(!World){IFileManager::Get().Delete(*Path);AddError(TEXT("create isolated actor world"));return false;}
    auto Source=World->SpawnActor<AVoxelEnvironmentLODPrototype>();
    FVoxelEnvironmentAssetDescriptor D;D.SpecId=TEXT("appearance-test-source");D.Kind=TEXT("tree");D.Category=TEXT("environment");D.Fellable=true;
    auto CheckFaces=[&](AVoxelEnvironmentLODPrototype* Actor,const TCHAR* Stage){
        int Faces=0;bool Exact=true;
        TArray<UProceduralMeshComponent*> Components;Actor->GetComponents(Components);
        for(auto Component:Components){const auto S=Component->GetProcMeshSection(0);if(!S)continue;
            for(int I=0;I<S->ProcVertexBuffer.Num();I+=4){
                FVector Center=FVector::ZeroVector;for(int J=0;J<4;++J)Center+=S->ProcVertexBuffer[I+J].Position*.25;
                const FVector N=S->ProcVertexBuffer[I].Normal;const FVector Inside=Center-N*5.;
                const FIntVector C(FMath::FloorToInt(Inside.X/10.),FMath::FloorToInt(Inside.Y/10.),FMath::FloorToInt(Inside.Z/10.));
                const int Axis=FMath::Abs(N.X)>.5?0:FMath::Abs(N.Y)>.5?1:2;
                auto Expected=FVoxelAssetAppearance::FaceColor(CellRGB(C),C,Axis,N[Axis]>0).ToFColor(true);
                for(int J=0;J<4;++J){const auto Actual=S->ProcVertexBuffer[I+J].Color;Exact&=Actual.R==Expected.R&&Actual.G==Expected.G&&Actual.B==Expected.B;}
                ++Faces;
            }
        }
        TestTrue(FString(Stage)+TEXT(" every emitted face retains source RGB and face variation"),Exact);
        return Faces;
    };
    if(TestTrue(TEXT("initialize editable source with appearance"),Source&&Source->InitializeAssetFromVxa(D,Vxa,true,false))){
        TestEqual(TEXT("initial exposed face count"),CheckFaces(Source,TEXT("initial")),96);
        Source->SetActorTransform(FTransform(FRotator(0,90,0),FVector(500,300,100)));
        TestEqual(TEXT("quarter turn leaves source face colors unchanged"),CheckFaces(Source,TEXT("rotated")),96);
        const FVector Hit=Source->GetActorTransform().TransformPosition(FVector(15,15,15));
        TestTrue(TEXT("interior occupied before carve"),Source->SolidAt(Hit));
        TestTrue(TEXT("carve an interior voxel"),Source->Carve(Hit,1));
        TestFalse(TEXT("removed interior voxel is air"),Source->SolidAt(Hit));
        TestEqual(TEXT("six new interior faces use approved colors"),CheckFaces(Source,TEXT("carved")),102);
        FVoxelImmutableGeometry Geometry;TArray<uint8> Dynamic;
        if(TestTrue(TEXT("capture edited source"),Source->CaptureObjectState(Geometry,Dynamic))){
            auto Restored=World->SpawnActor<AVoxelEnvironmentLODPrototype>();
            if(TestTrue(TEXT("restore edited source"),Restored&&Restored->RestoreObjectState(Geometry,Dynamic))){
                TestEqual(TEXT("source identity retained"),Restored->GetAssetDescriptor().SourceHash,MD5);
                TestFalse(TEXT("saved carve remains air"),Restored->SolidAt(Hit));
                TestEqual(TEXT("restored face count and colors"),CheckFaces(Restored,TEXT("restored")),102);
                auto Staged=World->SpawnActor<AVoxelEnvironmentLODPrototype>();
                if(Staged){
                    auto Status=MakeShared<FAppearanceRestoreStatus>();
                    Staged->BeginStagedObjectRestore(Geometry,Dynamic,[Status](bool Success){Status->Success=Success;Status->Done=true;});
                    ADD_LATENT_AUTOMATION_COMMAND(FAppearanceStagedProbe(this,World,Staged,Status,MeshSnapshot(Restored),Path,Hit));
                    return true; // Latent probe owns world and private fixture cleanup.
                }
            }
        }
    }
    World->DestroyWorld(false);IFileManager::Get().Delete(*Path);return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelAppearanceRealTreeTest,"Voxel.Appearance.RealTree",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelAppearanceRealTreeTest::RunTest(const FString&){
    FString Path;
    if(!FParse::Value(FCommandLine::Get(),TEXT("VoxelAppearanceTestVxa="),Path)){
        AddError(TEXT("Supply VoxelAppearanceTestVxa and VoxelAssetAppearanceDir for the real tree test"));return false;
    }
    TArray<uint8> Vxa;vxc::AssetGrid Grid;
    if(!TestTrue(TEXT("read real VXA"),FFileHelper::LoadFileToArray(Vxa,*Path))||
       !TestTrue(TEXT("parse real VXA"),Grid.parse(Vxa.GetData(),Vxa.Num())==vxc::AssetParseError::kOk))return false;
    if(!TestEqual(TEXT("real tree test pitch"),Grid.voxelSizeMm(),100.))return false;
    const FString MD5=FMD5::HashBytes(Vxa.GetData(),Vxa.Num());auto Appearance=FVoxelAssetAppearance::Load(MD5);
    if(!TestTrue(TEXT("real source has complete appearance"),Appearance.IsValid()))return false;
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,FName(*FGuid::NewGuid().ToString()));
    if(!World){AddError(TEXT("create isolated real-tree world"));return false;}
    auto Source=World->SpawnActor<AVoxelEnvironmentLODPrototype>();
    FVoxelEnvironmentAssetDescriptor D;D.SpecId=TEXT("real-tree-appearance-validation");D.Kind=TEXT("tree");D.Category=TEXT("environment");D.Fellable=true;
    const FIntVector Origin(Grid.originX(),Grid.originY(),Grid.originZ());
    auto Verify=[&](AActor* A,const TCHAR* Stage){
        int64 Faces=0,Bad=0;TArray<UProceduralMeshComponent*> Components;A->GetComponents(Components);
        for(auto C:Components){const auto S=C->GetProcMeshSection(0);if(!S)continue;
            if(C->ComponentHasTag(TEXT("FractureLowerCap"))||C->ComponentHasTag(TEXT("FractureUpperCap")))continue;
            for(int I=0;I<S->ProcVertexBuffer.Num();I+=4){
                FVector Center=FVector::ZeroVector;for(int J=0;J<4;++J)Center+=S->ProcVertexBuffer[I+J].Position*.25;
                const FVector N=S->ProcVertexBuffer[I].Normal,Inside=Center-N*5.;
                const FIntVector Cell(FMath::FloorToInt(Inside.X/10.),FMath::FloorToInt(Inside.Y/10.),FMath::FloorToInt(Inside.Z/10.));
                const auto Local=Cell-Origin;const uint8 M=Grid.at(Local.X,Local.Y,Local.Z);
                FColor RGB;FIntVector SourceCell;const int Axis=FMath::Abs(N.X)>.5?0:FMath::Abs(N.Y)>.5?1:2;
                if(!Appearance->Sample(Cell,100,M,RGB,SourceCell)){++Bad;continue;}
                const auto Expected=FVoxelAssetAppearance::FaceColor(RGB,SourceCell,Axis,N[Axis]>0).ToFColor(true);
                for(int J=0;J<4;++J){const auto& V=S->ProcVertexBuffer[I+J];const auto UV=Appearance->FaceUV(FVector3f(V.Position),Axis);
                    if(V.Color.R!=Expected.R||V.Color.G!=Expected.G||V.Color.B!=Expected.B||!V.UV0.Equals(FVector2D(UV),1.e-5))++Bad;
                }
                ++Faces;
            }
        }
        TestEqual(FString(Stage)+TEXT(" color/UV errors across every real face"),Bad,int64(0));
        AddInfo(FString::Printf(TEXT("%s checked %lld real tree faces"),Stage,Faces));return Faces;
    };
    FString PublishedSpecies;int32 PublishedSeed=0;
    const bool Published=FParse::Value(FCommandLine::Get(),TEXT("VoxelAppearancePublishedSpecies="),PublishedSpecies);
    FParse::Value(FCommandLine::Get(),TEXT("VoxelAppearancePublishedSeed="),PublishedSeed);
    if(Published&&Source){
        TestFalse(TEXT("published entry refuses path traversal"),Source->InitializePublishedTree(TEXT("../temperate-oak"),PublishedSeed,false));
        TestFalse(TEXT("published entry refuses missing seed"),Source->InitializePublishedTree(PublishedSpecies,2147483647,false));
    }
    if(TestTrue(TEXT("initialize real editable tree"),Source&&(Published?Source->InitializePublishedTree(PublishedSpecies,PublishedSeed,false):Source->InitializeAssetFromVxa(D,Vxa,true,false)))){
        const int64 Before=Verify(Source,TEXT("initial"));TestTrue(TEXT("substantial real tree geometry"),Before>1000);
        FIntVector Cut(-1,-1,-1);
        for(int Z=1;Z<Grid.sizeZ()-1&&Cut.X<0;++Z)for(int X=1;X<Grid.sizeX()-1&&Cut.X<0;++X)for(int Y=1;Y<Grid.sizeY()-1;++Y){
            if(Grid.at(X,Y,Z)==16&&Grid.at(X-1,Y,Z)&&Grid.at(X+1,Y,Z)&&Grid.at(X,Y-1,Z)&&Grid.at(X,Y+1,Z)&&Grid.at(X,Y,Z-1)&&Grid.at(X,Y,Z+1)){Cut=FIntVector(X,Y,Z);break;}
        }
        if(TestTrue(TEXT("real trunk contains buried voxel"),Cut.X>=0)){
            const FVector Hit=(FVector(Cut+Origin)+FVector(.5))*10.;
            TestTrue(TEXT("carve real buried wood"),Source->Carve(Hit,1));TestFalse(TEXT("real carve changes occupancy"),Source->SolidAt(Hit));
            const int64 After=Verify(Source,TEXT("carved"));TestEqual(TEXT("real carve exposes six colored faces"),After,Before+6);
            FVoxelImmutableGeometry G;TArray<uint8> Dynamic;
            if(TestTrue(TEXT("capture real edited tree"),Source->CaptureObjectState(G,Dynamic))){
                auto Restored=World->SpawnActor<AVoxelEnvironmentLODPrototype>();
                if(TestTrue(TEXT("restore real edited tree"),Restored&&Restored->RestoreObjectState(G,Dynamic))){
                    TestFalse(TEXT("restored real carve remains air"),Restored->SolidAt(Hit));
                    TestEqual(TEXT("all restored real faces preserved"),Verify(Restored,TEXT("restored")),After);
                }
            }
        }
    }
    if(FParse::Param(FCommandLine::Get(),TEXT("VoxelAppearanceFellingTest"))&&Source){
        // Chop every original wood cell in one trunk layer through the public
        // axe path. This must actually transfer geometry to a falling actor.
        const int Z=FMath::Clamp(15-Origin.Z,3,Grid.sizeZ()-3);
        for(int X=0;X<Grid.sizeX();++X)for(int Y=0;Y<Grid.sizeY();++Y){
            if(!VoxelVegetationRender::IsWood(Grid.at(X,Y,Z)))continue;
            const FVector Hit=(FVector(FIntVector(X,Y,Z)+Origin)+FVector(.5))*10.;
            if(Source->CanChop(Hit))Source->Chop(Hit,FVector::ForwardVector,1);
        }
        AVoxelFallingTimber* Timber=nullptr;for(TActorIterator<AVoxelFallingTimber> It(World);It;++It){Timber=*It;break;}
        if(TestTrue(TEXT("real axe cut detaches falling tree"),Timber!=nullptr)){
            TestTrue(TEXT("falling real tree retains substantial source geometry"),Verify(Timber,TEXT("detached"))>1000);
            TestTrue(TEXT("detached source appearance mode"),TimberMaterialMode(Timber,Appearance->IsNeedle()));
            int Caps=0,Bad=0;TArray<UProceduralMeshComponent*> Cs;Timber->GetComponents(Cs);
            for(auto C:Cs)if(C->ComponentHasTag(TEXT("FractureLowerCap"))||C->ComponentHasTag(TEXT("FractureUpperCap"))){
                ++Caps;if(auto S=C->GetProcMeshSection(0))for(const auto& V:S->ProcVertexBuffer){
                    // UV coordinates remain source anchored on both fracture sides.
                    if(!V.UV0.Equals(FVector2D(Appearance->FaceUV(FVector3f(V.Position),2)),1.e-5))++Bad;
                    if(V.Color.A!=128&&V.Color.A!=191)++Bad;
                }
            }
            TestEqual(TEXT("two fracture caps built"),Caps,2);TestEqual(TEXT("fracture caps retain material class and source UV"),Bad,0);
            FVoxelImmutableGeometry G;TArray<uint8> Dynamic;
            if(TestTrue(TEXT("capture source-colored falling timber"),Timber->CaptureObjectState(G,Dynamic))){
                auto Restored=World->SpawnActor<AVoxelFallingTimber>();
                if(TestTrue(TEXT("restore source-colored falling timber"),Restored&&Restored->RestoreObjectState(G,Dynamic))){
                    TestTrue(TEXT("restored timber shader modes"),TimberMaterialMode(Restored,Appearance->IsNeedle()));
                    Verify(Restored,TEXT("restored timber"));
                    auto Staged=World->SpawnActor<AVoxelFallingTimber>();auto Status=MakeShared<FAppearanceRestoreStatus>();
                    Staged->BeginStagedObjectRestore(G,Dynamic,[Status](bool Ok){Status->Success=Ok;Status->Done=true;});
                    ADD_LATENT_AUTOMATION_COMMAND(FAppearanceTimberProbe(this,World,Staged,Status,MeshSnapshot(Restored),Appearance->IsNeedle()));
                    return true;
                }
            }
        }
    }
    World->DestroyWorld(false);return true;
}
#endif
