#include "VoxelEnvironmentLODPrototype.h"
#include "VoxelAssetAppearance.h"
#include "Engine/World.h"
#include "ProceduralMeshComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "HAL/FileManager.h"
#include "Serialization/MemoryWriter.h"
#include <openssl/sha.h>
#if WITH_DEV_AUTOMATION_TESTS
namespace {
void CWord(TArray<uint8>& B,uint32 V){for(int I=0;I<4;++I)B.Add(uint8(V>>(8*I)));}
void CWrite(TArray<uint8>& B,int O,uint32 V){for(int I=0;I<4;++I)B[O+I]=uint8(V>>(8*I));}
FIntVector Rot(FIntVector P,int Q){for(int I=0;I<Q;++I){int X=P.X;P.X=-P.Y;P.Y=X;}return P;}
FColor ComposedCellRGB(FIntVector C){return FColor(80+C.X*20,90+C.Y*15,70+C.Z*25);}
TArray<uint8> CVxa(int Yaw,bool Clip){
    const int SX=Yaw%2?3:2,SY=Yaw%2?2:3;
    const FIntVector Origins[]={{-4,-7,-2},{5,-4,-2},{3,5,-2},{-7,3,-2}};
    const auto O=Origins[Yaw];TArray<uint8> Cells;Cells.Init(0,12);
    for(int X=0;X<2;++X)for(int Y=0;Y<3;++Y)for(int Z=0;Z<2;++Z){
        if(Clip&&X==0&&Y==0&&Z==0)continue;
        auto P=Rot(FIntVector(X-4,Y-7,Z-2),Yaw)-O;Cells[(P.X*SY+P.Y)*2+P.Z]=16;
    }
    TArray<uint8> B;CWord(B,0x31415856);CWord(B,3);CWord(B,O.X);CWord(B,O.Y);CWord(B,O.Z);
    CWord(B,SX);CWord(B,SY);CWord(B,2);CWord(B,100);CWord(B,12);CWord(B,0);CWord(B,0);
    for(uint8 M:Cells){B.Add(M);CWord(B,1);}return B;
}
TArray<uint8> CMesh(AActor* A){
    TArray<uint8> B;FMemoryWriter W(B);TArray<UProceduralMeshComponent*> Cs;A->GetComponents(Cs);
    for(auto C:Cs)if(auto S=C->GetProcMeshSection(0)){for(auto V:S->ProcVertexBuffer)W<<V.Position<<V.Normal<<V.UV0<<V.Color;auto I=S->ProcIndexBuffer;W<<I;}return B;
}
struct CStatus{bool Done=false,Ok=false;};
struct CRestore{AVoxelEnvironmentLODPrototype* Actor;TSharedPtr<CStatus> Status;TArray<uint8> Mesh;FVoxelEnvironmentAssetDescriptor D;FVector Hole;};
class CRestoreProbe final:public IAutomationLatentCommand{
    FAutomationTestBase* Test;UWorld* World;TArray<CRestore> Jobs;FString Path;double Start;
public:
    CRestoreProbe(FAutomationTestBase* T,UWorld* W,TArray<CRestore> J,FString P):Test(T),World(W),Jobs(MoveTemp(J)),Path(MoveTemp(P)),Start(FPlatformTime::Seconds()){}
    bool Update() override{
        bool Done=true;for(auto& J:Jobs){J.Actor->AdvanceStagedObjectRestore();Done&=J.Status->Done;}
        if(!Done&&FPlatformTime::Seconds()-Start<60)return false;
        for(auto& J:Jobs)if(Test->TestTrue(TEXT("composed staged restore completed"),J.Status->Done&&J.Status->Ok)){
            Test->TestTrue(TEXT("staged composition hidden"),J.Actor->IsHidden());Test->TestTrue(TEXT("publish staged composition"),J.Actor->PublishStagedObjectRestore());
            Test->TestTrue(TEXT("staged RGB UV geometry identical"),CMesh(J.Actor)==J.Mesh);
            Test->TestFalse(TEXT("staged carve remains air"),J.Actor->SolidAt(J.Hole));
            const auto& D=J.Actor->GetAssetDescriptor();Test->TestEqual(TEXT("staged original identity"),D.SourceHash,J.D.SourceHash);
            Test->TestEqual(TEXT("staged clipped identity"),D.ClippedGeometryHash,J.D.ClippedGeometryHash);Test->TestEqual(TEXT("staged canonical yaw"),D.SourceYawQuarter,J.D.SourceYawQuarter);
        }
        World->DestroyWorld(false);IFileManager::Get().Delete(*Path);return true;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelComposedEnvironmentActorTest,"Voxel.Environment.ComposedActor",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelComposedEnvironmentActorTest::RunTest(const FString&){
    FString Directory;if(!FParse::Value(FCommandLine::Get(),TEXT("VoxelAssetAppearanceDir="),Directory)){AddError(TEXT("Supply a private VoxelAssetAppearanceDir"));return false;}
    auto Source=CVxa(0,false);const FString Hash=FMD5::HashBytes(Source.GetData(),Source.Num());
    TArray<uint8> Packet;Packet.SetNumZeroed(248);FMemory::Memcpy(Packet.GetData(),"VAC1",4);CWrite(Packet,4,1);
    CWrite(Packet,8,2);CWrite(Packet,12,3);CWrite(Packet,16,2);CWrite(Packet,20,uint32(-4));CWrite(Packet,24,uint32(-7));CWrite(Packet,28,uint32(-2));CWrite(Packet,32,100);CWrite(Packet,36,12);HexToBytes(Hash,Packet.GetData()+48);
    for(int X=0;X<2;++X)for(int Y=0;Y<3;++Y)for(int Z=0;Z<2;++Z){uint8* R=Packet.GetData()+128+((X*3+Y)*2+Z)*10;R[0]=X;R[2]=Y;R[4]=Z;R[6]=16;auto C=ComposedCellRGB({X,Y,Z});R[7]=C.R;R[8]=C.G;R[9]=C.B;}
    TArray<uint8> Check;Check.Append(Packet.GetData(),96);Check.Append(Packet.GetData()+128,120);SHA256(Check.GetData(),Check.Num(),Packet.GetData()+96);
    FString Error;auto OriginalAppearance=FVoxelAssetAppearance::Parse(Packet,Hash,Error);if(!TestTrue(TEXT("fixture appearance valid"),OriginalAppearance.IsValid()))return false;
    IFileManager::Get().MakeDirectory(*Directory,true);const FString Path=Directory/(Hash+TEXT(".vac"));
    if(IFileManager::Get().FileExists(*Path)){AddError(TEXT("Fixture already exists; use a private clean directory"));return false;}
    if(!FFileHelper::SaveArrayToFile(Packet,*Path)){AddError(TEXT("write fixture appearance"));return false;}
    auto World=UWorld::CreateWorld(EWorldType::Game,false,FName(*FGuid::NewGuid().ToString()));if(!World){IFileManager::Get().Delete(*Path);return false;}
    TArray<CRestore> Jobs;
    for(uint8 Q=0;Q<4;++Q){
        auto Clipped=CVxa(Q,true);FVoxelEnvironmentAssetDescriptor D;D.SpecId=TEXT("composed-negative-origin-test");D.Kind=TEXT("tree");D.Category=TEXT("environment");D.SourceHash=Hash;
        D.ClippedGeometryHash=FMD5::HashBytes(Clipped.GetData(),Clipped.Num());D.SourceYawQuarter=Q;
        auto A=World->SpawnActor<AVoxelEnvironmentLODPrototype>();A->SetTestLOD(0);A->SetActorLocation(FVector(-1000,-2000,500));
        TestFalse(TEXT("ordinary import rejects composition"),A->InitializeAssetFromVxa(D,Clipped,true,false));
        auto Bad=D;Bad.SourceHash=FString::ChrN(32,'0');TestFalse(TEXT("wrong original digest refused"),A->InitializeComposedAssetFromVxa(Bad,Source,Clipped,Q,true));
        Bad=D;Bad.ClippedGeometryHash=FString::ChrN(32,'0');TestFalse(TEXT("wrong clipped digest refused"),A->InitializeComposedAssetFromVxa(Bad,Source,Clipped,Q,true));
        Bad=D;Bad.Legacy=true;TestFalse(TEXT("legacy composition refused"),A->InitializeComposedAssetFromVxa(Bad,Source,Clipped,Q,true));
        Bad=D;Bad.ClippedGeometryHash.Reset();TestFalse(TEXT("missing composition digest refused"),A->InitializeComposedAssetFromVxa(Bad,Source,Clipped,Q,true));
        auto FineSource=Source;CWrite(FineSource,32,50);Bad=D;Bad.SourceHash=FMD5::HashBytes(FineSource.GetData(),FineSource.Num());
        TestFalse(TEXT("fine source pitch refused"),A->InitializeComposedAssetFromVxa(Bad,FineSource,Clipped,Q,true));
        // Same valid lattice/subset, distinct source bytes/material: no packet
        // exists for this identity in the required private fixture directory.
        auto UnreviewedSource=Source;auto UnreviewedClip=Clipped;
        for(int I=48;I<UnreviewedSource.Num();I+=5)if(UnreviewedSource[I])UnreviewedSource[I]=17;
        for(int I=48;I<UnreviewedClip.Num();I+=5)if(UnreviewedClip[I])UnreviewedClip[I]=17;
        Bad=D;Bad.SourceHash=FMD5::HashBytes(UnreviewedSource.GetData(),UnreviewedSource.Num());
        Bad.ClippedGeometryHash=FMD5::HashBytes(UnreviewedClip.GetData(),UnreviewedClip.Num());
        TestFalse(TEXT("unreviewed source fixture has no appearance packet"),IFileManager::Get().FileExists(*(Directory/(Bad.SourceHash+TEXT(".vac")))));
        TestFalse(TEXT("tree subset with missing original appearance refused"),A->InitializeComposedAssetFromVxa(Bad,UnreviewedSource,UnreviewedClip,Q,true));
        TestFalse(TEXT("invalid yaw refused"),A->InitializeComposedAssetFromVxa(D,Source,Clipped,4,true));
        TestFalse(TEXT("inconsistent metadata yaw refused"),A->InitializeComposedAssetFromVxa(D,Source,Clipped,(Q+1)%4,true));
        for(int Mode=0;Mode<5;++Mode){auto Broken=Clipped;Bad=D;
            if(Mode==0)Broken[53]=17; // valid RLE, wrong nonair material
            if(Mode==1)CWrite(Broken,12,99); // origin
            if(Mode==2){CWrite(Broken,20,1);CWrite(Broken,24,6);} // same cells, wrong dimensions
            if(Mode==3)CWrite(Broken,32,50);
            if(Mode==4)for(int I=48;I<Broken.Num();I+=5)Broken[I]=0;
            Bad.ClippedGeometryHash=FMD5::HashBytes(Broken.GetData(),Broken.Num());TestFalse(TEXT("noncanonical clipped payload refused"),A->InitializeComposedAssetFromVxa(Bad,Source,Broken,Q,true));
        }
        A->SetActorScale3D(FVector(2));TestFalse(TEXT("scale refused"),A->InitializeComposedAssetFromVxa(D,Source,Clipped,Q,true));A->SetActorScale3D(FVector::OneVector);
        A->SetActorRotation(FRotator(0,90,0));TestFalse(TEXT("double actor yaw refused"),A->InitializeComposedAssetFromVxa(D,Source,Clipped,Q,true));A->SetActorRotation(FRotator::ZeroRotator);
        if(!TestTrue(TEXT("verified clipped import"),A->InitializeComposedAssetFromVxa(D,Source,Clipped,Q,true)))continue;
        TestEqual(TEXT("original digest never replaced by clipping"),A->GetAssetDescriptor().SourceHash,Hash);
        const auto Removed=Rot(FIntVector(-4,-7,-2),Q);TestFalse(TEXT("clipped source cell absent"),A->SolidAt(A->GetActorLocation()+FVector(Removed)*10+FVector(5)));
        TArray<UProceduralMeshComponent*> Cs;A->GetComponents(Cs);int Faces=0;
        for(auto C:Cs)if(auto S=C->GetProcMeshSection(0))for(int I=0;I<S->ProcVertexBuffer.Num();I+=4){
            FVector Center=FVector::ZeroVector;for(int J=0;J<4;++J)Center+=S->ProcVertexBuffer[I+J].Position*.25;
            const auto N=S->ProcVertexBuffer[I].Normal;const auto Inside=Center-N*5;FIntVector WC(FMath::FloorToInt(Inside.X/10),FMath::FloorToInt(Inside.Y/10),FMath::FloorToInt(Inside.Z/10));
            const auto SC=Rot(WC,(4-Q)%4);const auto Local=SC-FIntVector(-4,-7,-2);FIntVector Normal(FMath::RoundToInt(N.X),FMath::RoundToInt(N.Y),FMath::RoundToInt(N.Z));Normal=Rot(Normal,(4-Q)%4);
            const int Axis=Normal.X?0:Normal.Y?1:2;auto Expected=FVoxelAssetAppearance::FaceColor(ComposedCellRGB(Local),Local,Axis,Normal[Axis]>0).ToFColor(true);
            // Vertex alpha encodes bark classification (0.75), not opacity.
            Expected.A=191;
            for(int J=0;J<4;++J){const auto& V=S->ProcVertexBuffer[I+J];TestEqual(TEXT("source face RGB and bark class"),V.Color,Expected);
                FIntVector Corner(FMath::RoundToInt(V.Position.X/10)-WC.X,FMath::RoundToInt(V.Position.Y/10)-WC.Y,FMath::RoundToInt(V.Position.Z/10)-WC.Z);
                auto Offset=Rot(Corner*2-FIntVector(1),(4-Q)%4);auto SourceVertex=SC+(Offset+FIntVector(1))/2;
                const auto UV=OriginalAppearance->FaceUV(FVector3f(SourceVertex)*10,Axis);TestTrue(TEXT("source vertex UV phase"),V.UV0.Equals(FVector2D(UV),.00001));
            }++Faces;
        }
        TestTrue(TEXT("composed faces inspected"),Faces>0);
        const auto Cell=Rot(FIntVector(-3,-6,-1),Q);const FVector Hole=A->GetActorLocation()+FVector(Cell)*10+FVector(5);
        TestTrue(TEXT("source survivor solid"),A->SolidAt(Hole));TestTrue(TEXT("composed carve"),A->Carve(Hole,1));TestFalse(TEXT("composed carve removed"),A->SolidAt(Hole));
        FVoxelImmutableGeometry Geometry;TArray<uint8> Dynamic;if(!TestTrue(TEXT("capture composed"),A->CaptureObjectState(Geometry,Dynamic)))continue;
        auto Restored=World->SpawnActor<AVoxelEnvironmentLODPrototype>();if(TestTrue(TEXT("sync composed restore"),Restored->RestoreObjectState(Geometry,Dynamic))){
            TestTrue(TEXT("sync RGB UV geometry identical"),CMesh(Restored)==CMesh(A));TestFalse(TEXT("sync carve retained"),Restored->SolidAt(Hole));
            TestEqual(TEXT("sync source digest"),Restored->GetAssetDescriptor().SourceHash,Hash);TestEqual(TEXT("sync clipped digest"),Restored->GetAssetDescriptor().ClippedGeometryHash,D.ClippedGeometryHash);
        }
        auto Staged=World->SpawnActor<AVoxelEnvironmentLODPrototype>();auto Status=MakeShared<CStatus>();Staged->BeginStagedObjectRestore(Geometry,Dynamic,[Status](bool Ok){Status->Ok=Ok;Status->Done=true;});Jobs.Add({Staged,Status,CMesh(A),D,Hole});
    }
    ADD_LATENT_AUTOMATION_COMMAND(CRestoreProbe(this,World,MoveTemp(Jobs),Path));return true;
}
#endif
