#include "VoxelAssetAppearance.h"
#include "VoxelEnvironmentLODPrototype.h"
#include "VoxelTreeFellingPrototype.h"
#include "Engine/World.h"
#include "ProceduralMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/SecureHash.h"
#include "HAL/FileManager.h"
#include "Serialization/MemoryWriter.h"
#include <openssl/sha.h>
#if WITH_DEV_AUTOMATION_TESTS
namespace {
void GenericWrite(TArray<uint8>& B,int O,uint32 V){for(int I=0;I<4;++I)B[O+I]=uint8(V>>(8*I));}
TArray<uint8> GenericMesh(AActor* Actor){TArray<uint8> B;FMemoryWriter W(B);TArray<UProceduralMeshComponent*> Cs;Actor->GetComponents(Cs);for(auto C:Cs)if(auto S=C->GetProcMeshSection(0)){for(auto V:S->ProcVertexBuffer)W<<V.Position<<V.Normal<<V.UV0<<V.UV1<<V.Color;auto I=S->ProcIndexBuffer;W<<I;}return B;}
bool GenericMode(AActor* Actor,bool Mask){TArray<UProceduralMeshComponent*> Cs;Actor->GetComponents(Cs);int Count=0;for(auto C:Cs){if(!C->GetProcMeshSection(0))continue;++Count;auto M=C->GetMaterial(0);if(!M)return false;float A=-1,N=-1,F=-1;M->GetScalarParameterValue(FMaterialParameterInfo(TEXT("TreeAppearance")),A);M->GetScalarParameterValue(FMaterialParameterInfo(TEXT("TreeNeedle")),N);M->GetScalarParameterValue(FMaterialParameterInfo(TEXT("FoliageCutout")),F);if(A!=1||N!=0||F!=(Mask?1.f:0.f))return false;}return Count>0;}
struct FGenericDone {bool Done=false,OK=false;};
struct FGenericStage {AActor* Actor=nullptr;bool Timber=false,Mask=false;TSharedPtr<FGenericDone> Status;TArray<uint8> Expected;FString SourceHash;double Mm=0;};
class FGenericRestoreProbe final:public IAutomationLatentCommand {
    FAutomationTestBase* Test;UWorld* World;TArray<FGenericStage> Items;TArray<FString> Files;double Start=FPlatformTime::Seconds();
public:
    FGenericRestoreProbe(FAutomationTestBase* T,UWorld* W,TArray<FGenericStage> I,TArray<FString> P):Test(T),World(W),Items(MoveTemp(I)),Files(MoveTemp(P)){}
    bool Update() override {
        bool Done=true;
        for(auto& I:Items){if(I.Timber)CastChecked<AVoxelFallingTimber>(I.Actor)->AdvanceStagedObjectRestore();else CastChecked<AVoxelEnvironmentLODPrototype>(I.Actor)->AdvanceStagedObjectRestore();Done&=I.Status->Done;}
        if(!Done&&FPlatformTime::Seconds()-Start<90)return false;
        for(auto& I:Items){
            if(Test->TestTrue(*FString::Printf(TEXT("generic/legacy staged completion (%g mm %s)"),I.Mm,I.Timber?TEXT("timber"):TEXT("environment")),I.Status->Done&&I.Status->OK)){
                Test->TestTrue(TEXT("staged geometry hidden until publication"),I.Actor->IsHidden());
                const bool Published=I.Timber?CastChecked<AVoxelFallingTimber>(I.Actor)->PublishStagedObjectRestore():CastChecked<AVoxelEnvironmentLODPrototype>(I.Actor)->PublishStagedObjectRestore();
                Test->TestTrue(TEXT("staged appearance publication"),Published);
                Test->TestTrue(TEXT("staged explicit opaque/legacy mask flag retained"),GenericMode(I.Actor,I.Mask));
                Test->TestTrue(TEXT("staged exact color UV and geometry restoration"),GenericMesh(I.Actor)==I.Expected);
                if(!I.Timber)Test->TestEqual(TEXT("staged original source identity"),CastChecked<AVoxelEnvironmentLODPrototype>(I.Actor)->GetAssetDescriptor().SourceHash,I.SourceHash);
            }
        }
        for(auto& I:Items){if(I.Timber)CastChecked<AVoxelFallingTimber>(I.Actor)->CancelStagedObjectRestore();else CastChecked<AVoxelEnvironmentLODPrototype>(I.Actor)->CancelStagedObjectRestore();}
        World->DestroyWorld(false);for(const auto& P:Files)IFileManager::Get().Delete(*P);return true;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGenericRestoreTest,"Voxel.Appearance.GenericOpaqueRestore",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelGenericRestoreTest::RunTest(const FString&){
    FString Directory;if(!FParse::Value(FCommandLine::Get(),TEXT("VoxelAssetAppearanceDir="),Directory)){AddError(TEXT("Supply private VoxelAssetAppearanceDir"));return false;}
    IFileManager::Get().MakeDirectory(*Directory,true);
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,FName(*FGuid::NewGuid().ToString()));if(!World){AddError(TEXT("create isolated world"));return false;}
    TArray<FString> Files;TArray<FGenericStage> Staged;bool Good=true;
    // Environment sources sit on the world ladder (ADR-0010: 100/50/25 mm; 12.5 mm is
    // reserved for craftables and creatures), so the finest generic case is 25 mm.
    for(double Mm:{50.,25.,100.}){
        const bool Legacy=Mm==100.;const uint8 Material=Legacy?19:24;const uint32 Um=uint32(Mm*1000);
        constexpr int Size=8,Count=Size*Size*Size;
        TArray<uint8> Vxa;Vxa.SetNumZeroed(53);FMemory::Memcpy(Vxa.GetData(),"VXA1",4);GenericWrite(Vxa,4,Mm==12.5?4:3);
        for(int O:{8,12,16})GenericWrite(Vxa,O,uint32(-8));
        for(int O:{20,24,28})GenericWrite(Vxa,O,Size);GenericWrite(Vxa,32,Mm==12.5?Um:uint32(Mm));GenericWrite(Vxa,36,1);Vxa[48]=Material;GenericWrite(Vxa,49,Count);
        const FString Hash=FMD5::HashBytes(Vxa.GetData(),Vxa.Num());
        TArray<uint8> Packet;Packet.SetNumZeroed(128+Count*10);FMemory::Memcpy(Packet.GetData(),"VAC1",4);GenericWrite(Packet,4,Legacy?1:2);
        for(int O:{8,12,16})GenericWrite(Packet,O,Size);for(int O:{20,24,28})GenericWrite(Packet,O,uint32(-8));GenericWrite(Packet,32,Legacy?uint32(Mm):Um);GenericWrite(Packet,36,Count);HexToBytes(Hash,Packet.GetData()+48);
        for(int X=0;X<Size;++X)for(int Y=0;Y<Size;++Y)for(int Z=0;Z<Size;++Z){auto R=Packet.GetData()+128+((X*Size+Y)*Size+Z)*10;R[0]=uint8(X);R[2]=uint8(Y);R[4]=uint8(Z);R[6]=Material;R[7]=uint8(80+X*15);R[8]=uint8(60+Y*17);R[9]=uint8(90+Z*13);}
        TArray<uint8> Checked;Checked.Append(Packet.GetData(),96);Checked.Append(Packet.GetData()+128,Count*10);check(SHA256(Checked.GetData(),Checked.Num(),Packet.GetData()+96));
        const FString Path=Directory/(Hash+TEXT(".vac"));if(IFileManager::Get().FileExists(*Path)||!FFileHelper::SaveArrayToFile(Packet,*Path)){AddError(TEXT("use a clean private fixture directory"));Good=false;break;}Files.Add(Path);
        FString Error;auto Appearance=FVoxelAssetAppearance::Parse(Packet,Hash,Error);
        if(!TestTrue(TEXT("packet parsed"),Appearance.IsValid())){Good=false;break;}
        TestEqual(TEXT("legacy implicit mask and generic opaque flag"),Appearance->UsesFoliageMask(),Legacy);
        auto Source=World->SpawnActor<AVoxelEnvironmentLODPrototype>();FVoxelEnvironmentAssetDescriptor Descriptor;Descriptor.SpecId=Legacy?TEXT("legacy-mask-fixture"):TEXT("opaque-petal-fixture");Descriptor.Kind=Legacy?TEXT("tree"):TEXT("flower");Descriptor.Category=TEXT("environment");Descriptor.Fellable=false;
        if(!TestTrue(TEXT("initialize source actor"),Source&&Source->InitializeAssetFromVxa(Descriptor,Vxa,true,false))){Good=false;break;}
        TestTrue(TEXT("source material24 petals opaque, legacy foliage masked"),GenericMode(Source,Legacy));
        const int ExpectedLevels=Mm==25?3:Mm==50?2:1;
        TestEqual(TEXT("complete hierarchy through 100mm"),Source->LevelCount(),ExpectedLevels);
        TArray<int> FacesPerLevel;FacesPerLevel.Init(0,ExpectedLevels);
        TArray<UProceduralMeshComponent*> Meshes;Source->GetComponents(Meshes);int Faces=0;
        for(auto C:Meshes)if(auto Section=C->GetProcMeshSection(0)){
            int Level=INDEX_NONE;for(int L=0;L<Source->Levels.Num();++L)if(C->GetAttachParent()==Source->Levels[L].Get())Level=L;
            if(!TestTrue(TEXT("section has a known hierarchy level"),Level>=0&&Level<ExpectedLevels)){Good=false;break;}
            const double LevelMm=Mm*(1<<Level);
            for(int I=0;I<Section->ProcVertexBuffer.Num();I+=4){
            FVector Center=FVector::ZeroVector;for(int J=0;J<4;++J)Center+=Section->ProcVertexBuffer[I+J].Position*.25;
            const FVector N=Section->ProcVertexBuffer[I].Normal;const double Pitch=LevelMm*.1;const FVector Local=(Center-N*(Pitch*.5))/Pitch;
            const FIntVector Cell(FMath::FloorToInt(Local.X),FMath::FloorToInt(Local.Y),FMath::FloorToInt(Local.Z));FColor Base;FIntVector Zero;
            if(!Appearance->Sample(Cell,LevelMm,Material,Base,Zero)){AddError(TEXT("source face missing appearance cell"));Good=false;break;}
            const int Axis=FMath::Abs(N.X)>.5?0:FMath::Abs(N.Y)>.5?1:2;const FColor Expected=FVoxelAssetAppearance::FaceColor(Base,Zero,Axis,N[Axis]>0).ToFColor(true);
            for(int J=0;J<4;++J){const auto& V=Section->ProcVertexBuffer[I+J];TestTrue(TEXT("actual source face RGB"),V.Color.R==Expected.R&&V.Color.G==Expected.G&&V.Color.B==Expected.B);TestTrue(TEXT("source UV preserved"),V.UV0.Equals(FVector2D(Appearance->FaceUV(FVector3f(V.Position),Axis)),1.e-5));}++Faces;++FacesPerLevel[Level];
            }
        }
        for(int L=0;L<ExpectedLevels;++L)TestTrue(*FString::Printf(TEXT("LOD %d has appearance-checked faces"),L),FacesPerLevel[L]>0);
        TestTrue(TEXT("nonempty checked source faces"),Faces>0);
        FVoxelImmutableGeometry Geometry;TArray<uint8> Dynamic;
        if(!TestTrue(TEXT("capture generic/legacy actor"),Source->CaptureObjectState(Geometry,Dynamic))){Good=false;break;}
        auto Restored=World->SpawnActor<AVoxelEnvironmentLODPrototype>();
        if(!TestTrue(TEXT("synchronous actor restore"),Restored&&Restored->RestoreObjectState(Geometry,Dynamic))){Good=false;break;}
        TestEqual(TEXT("sync original source identity"),Restored->GetAssetDescriptor().SourceHash,Hash);
        TestTrue(TEXT("sync actor appearance flags"),GenericMode(Restored,Legacy));TestTrue(TEXT("sync actor RGB UV geometry exact"),GenericMesh(Restored)==GenericMesh(Source));
        auto ActorStage=World->SpawnActor<AVoxelEnvironmentLODPrototype>();auto ActorDone=MakeShared<FGenericDone>();
        if(!TestNotNull(TEXT("spawn staged actor"),ActorStage)){Good=false;break;}
        ActorStage->BeginStagedObjectRestore(Geometry,Dynamic,[ActorDone](bool OK){ActorDone->OK=OK;ActorDone->Done=true;});Staged.Add({ActorStage,false,Legacy,ActorDone,GenericMesh(Source),Hash,Mm});
        // Transfer actual rendered source sections through the same public timber
        // ownership path used by felling, then test its packed save serializer.
        Meshes.Reset();Restored->GetComponents(Meshes);auto Timber=World->SpawnActor<AVoxelFallingTimber>();
        if(!TestNotNull(TEXT("spawn detached actor"),Timber)){Good=false;break;}
        Timber->Initialize(Meshes,FBox(FVector(-5,-5,-10),FVector(5,5,10)),Restored->GetActorTransform(),FVector::ForwardVector,false);
        TestTrue(TEXT("detached mode preserved on transfer"),GenericMode(Timber,Legacy));
        FVoxelImmutableGeometry TG;TArray<uint8> TD;if(!TestTrue(TEXT("capture detached sections"),Timber->CaptureObjectState(TG,TD))){Good=false;break;}
        auto TimberSync=World->SpawnActor<AVoxelFallingTimber>();if(!TestTrue(TEXT("sync detached restore"),TimberSync&&TimberSync->RestoreObjectState(TG,TD))){Good=false;break;}
        TestTrue(TEXT("bit4 opaque disabled only for legacy cutouts"),GenericMode(TimberSync,Legacy));TestTrue(TEXT("sync detached RGB UV geometry exact"),GenericMesh(TimberSync)==GenericMesh(Timber));
        auto TimberStage=World->SpawnActor<AVoxelFallingTimber>();auto TimberDone=MakeShared<FGenericDone>();
        if(!TestNotNull(TEXT("spawn staged detached actor"),TimberStage)){Good=false;break;}
        TimberStage->BeginStagedObjectRestore(TG,TD,[TimberDone](bool OK){TimberDone->OK=OK;TimberDone->Done=true;});Staged.Add({TimberStage,true,Legacy,TimberDone,GenericMesh(TimberSync),{},Mm});
    }
    if(Good){ADD_LATENT_AUTOMATION_COMMAND(FGenericRestoreProbe(this,World,MoveTemp(Staged),MoveTemp(Files)));return true;}
    for(auto& I:Staged){if(I.Timber)CastChecked<AVoxelFallingTimber>(I.Actor)->CancelStagedObjectRestore();else CastChecked<AVoxelEnvironmentLODPrototype>(I.Actor)->CancelStagedObjectRestore();}
    World->DestroyWorld(false);for(const auto& P:Files)IFileManager::Get().Delete(*P);return false;
}
#endif
