#include "VoxelEnvironmentAsset.h"
#include "VoxelEnvironmentLODPrototype.h"
#include "VoxelDetachedPersistence.h"
#include "Engine/World.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

namespace {
// Independent integer oracle: never use actor transform/query helpers to
// compute expected coordinates or the cells inside an edit.
FVector QuarterPoint(const FVector& P,int32 Q){
    if(Q==1)return FVector(-P.Y,P.X,P.Z);
    if(Q==2)return FVector(-P.X,-P.Y,P.Z);
    if(Q==3)return FVector(P.Y,-P.X,P.Z);
    return P;
}
bool CoarseWood(int X,int Y,int Z){
    return (Z>=0&&Z<2&&((Y==-1&&X>=-2&&X<=0)||(Y==0&&X>=0&&X<=1)))||(X==1&&Y==0&&Z==2);
}
TArray<uint8> YawFixture(int32 Mm){
    const int R=100/Mm;TArray<uint8> Dense;
    for(int X=0;X<5*R;++X)for(int Y=0;Y<3*R;++Y)for(int Z=0;Z<4*R;++Z)
        Dense.Add(CoarseWood(X/R-2,Y/R-1,Z/R-1)?16:0);
    TArray<uint8> Materials;TArray<uint32> Lengths;
    for(uint8 M:Dense){if(!Materials.IsEmpty()&&Materials.Last()==M)++Lengths.Last();else{Materials.Add(M);Lengths.Add(1);}}
    TArray<uint8> Out;Out.Append({'V','X','A','1'});auto U32=[&](uint32 V){for(int I=0;I<4;++I)Out.Add(uint8(V>>(I*8)));};
    U32(3);U32(uint32(-2*R));U32(uint32(-R));U32(uint32(-R));U32(5*R);U32(3*R);U32(4*R);U32(Mm);U32(Materials.Num());U32(0);U32(0);
    for(int I=0;I<Materials.Num();++I){Out.Add(Materials[I]);U32(Lengths[I]);}return Out;
}
FBox LocalEdit(int32 Size){const FVector Min((-1-Size/2)*10.,(-1-Size/2)*10.,(-Size/2)*10.);return FBox(Min,Min+FVector(Size*10.));}
bool InsideEdit(const FVector& P,const FBox& B){return P.X>=B.Min.X&&P.X<B.Max.X&&P.Y>=B.Min.Y&&P.Y<B.Max.Y&&P.Z>=B.Min.Z&&P.Z<B.Max.Z;}
FBox WorldEdit(const FBox& B,int32 Q,const FVector& Translation){
    FBox Out(ForceInit);for(int X=0;X<2;++X)for(int Y=0;Y<2;++Y)for(int Z=0;Z<2;++Z)
        Out+=Translation+QuarterPoint(FVector(X?B.Max.X:B.Min.X,Y?B.Max.Y:B.Min.Y,Z?B.Max.Z:B.Min.Z),Q);
    return Out;
}
struct FYawRestoreStatus {bool Called=false,Ok=false;};
class FEnvironmentYawProbe final : public IAutomationLatentCommand {
    FAutomationTestBase* Test;UWorld* World=nullptr;
    AVoxelEnvironmentLODPrototype* Source=nullptr;AVoxelEnvironmentLODPrototype* Restored=nullptr;AVoxelEnvironmentLODPrototype* BadRestore=nullptr;
    TSharedPtr<FYawRestoreStatus> Status,BadStatus;
    FVoxelImmutableGeometry Geometry;
    int32 Case=0,Mm=25,Q=0;FVector Translation;FTransform Pose;
    double Started=0;
    FVector ToWorld(const FVector& P)const{return Translation+QuarterPoint(P,Q);}
    void Cleanup(){if(World){VoxelObjects::Forget(World);World->DestroyWorld(false);World=nullptr;}}
    void VerifyCells(AVoxelEnvironmentLODPrototype* A,int32 Size){
        const int R=100/Mm;const double Pitch=Mm*.1;const FBox Edit=LocalEdit(Size);int FineErrors=0,CoarseErrors=0;
        for(int X=0;X<5*R;++X)for(int Y=0;Y<3*R;++Y)for(int Z=0;Z<4*R;++Z){
            const FVector Local((X-2*R+.5)*Pitch,(Y-R+.5)*Pitch,(Z-R+.5)*Pitch);
            const bool Expected=CoarseWood(X/R-2,Y/R-1,Z/R-1)&&(Size==0||!InsideEdit(Local,Edit));
            FineErrors+=A->CanChop(ToWorld(Local))!=Expected;
        }
        for(int X=-3;X<4;++X)for(int Y=-2;Y<3;++Y)for(int Z=-2;Z<4;++Z){
            const FVector Local((X+.5)*10.,(Y+.5)*10.,(Z+.5)*10.);
            const bool Expected=CoarseWood(X,Y,Z)&&(Size==0||!InsideEdit(Local,Edit));CoarseErrors+=A->SolidAt(ToWorld(Local))!=Expected;
        }
        Test->TestEqual(FString::Printf(TEXT("fine cells match oracle yaw%d pitch%d edit%d"),Q,Mm,Size),FineErrors,0);
        Test->TestEqual(FString::Printf(TEXT("collision cells match oracle yaw%d pitch%d edit%d"),Q,Mm,Size),CoarseErrors,0);
    }
    TArray<uint8> DynamicWithTransform(const FTransform& Transform){
        TArray<uint8> Bytes;FMemoryWriter W(Bytes);VoxelObjectGeometrySnapshot::WriteVersion(W);
        auto Descriptor=Source->GetAssetDescriptor();VoxelEnvironmentAsset::SerializeIdentity(W,Descriptor);
        FTransform T=Transform;const int R=100/Mm;FIntVector Size(5*R,3*R,4*R),Origin(-2*R,-R,-R);double Pitch=Mm;int32 MaxZ=MAX_int32;bool Collision=true,Severed=false;
        W<<T<<Size<<Origin<<Pitch<<MaxZ<<Collision<<Severed;return Bytes;
    }
public:
    explicit FEnvironmentYawProbe(FAutomationTestBase* In):Test(In){}
    virtual bool Update() override {
        if(!World){Started=FPlatformTime::Seconds();World=UWorld::CreateWorld(EWorldType::Game,false,FName(*FString::Printf(TEXT("EnvironmentYaw_%s"),*FGuid::NewGuid().ToString(EGuidFormats::Digits))));if(!World){Test->AddError(TEXT("create yaw test world"));return true;}}
        if(FPlatformTime::Seconds()-Started>90){Test->AddError(TEXT("yaw restore timeout"));Cleanup();return true;}
        if(!Restored){
            const int Pitches[]={25,50,100};Mm=Pitches[Case/4];Q=Case%4;Translation=FVector(-1370.+Case*100.,2630.-Case*100.,450.);Pose=FTransform(FRotator(0,Q*90.,0),Translation);
            FVoxelEnvironmentAssetDescriptor D;D.SpecId=TEXT("asymmetric-negative-origin-lattice-tree");D.Kind=TEXT("tree");D.Category=TEXT("environment");D.Fellable=true;D.SeedIndex=71;
            const auto Vxa=YawFixture(Mm);Source=World->SpawnActor<AVoxelEnvironmentLODPrototype>();if(!Source){Test->AddError(TEXT("spawn yaw source"));Cleanup();return true;}Source->SetActorTransform(Pose);
            if(!Source->InitializeAssetFromVxa(D,Vxa,true,false)){Test->AddError(TEXT("initialize valid quarter yaw"));Cleanup();return true;}
            VerifyCells(Source,0);
            const double Half=Mm*.05;FVector Hit=FVector::ZeroVector;
            Test->TestTrue(TEXT("rotated trace finds asymmetric arm"),Source->Trace(ToWorld(FVector(-40.,-10.+Half,Half)),QuarterPoint(FVector::ForwardVector,Q),100,Hit));
            Test->TestTrue(TEXT("trace returns exact transformed fine-cell centre"),Hit.Equals(ToWorld(FVector(-20.+Half,-10.+Half,Half)),.0001));
            if(Case==0){
                FVoxelImmutableGeometry Original;TArray<uint8> OriginalDynamic;if(!Source->CaptureObjectState(Original,OriginalDynamic)){Test->AddError(TEXT("capture transform validation fixture"));Cleanup();return true;}
                const FTransform Unsupported[]={FTransform(FRotator::ZeroRotator,Translation,FVector(2)),FTransform(FRotator(15,0,0),Translation),FTransform(FRotator(0,45,0),Translation)};
                for(const auto& Bad:Unsupported){
                    auto Invalid=World->SpawnActor<AVoxelEnvironmentLODPrototype>();if(!Invalid){Test->AddError(TEXT("spawn unsupported-transform actor"));Cleanup();return true;}Invalid->SetActorTransform(Bad);
                    Test->TestFalse(TEXT("unsupported placement transform rejected"),Invalid->InitializeAssetFromVxa(D,Vxa,true,false));Invalid->SetActorTransform(FTransform::Identity);
                    Test->TestFalse(TEXT("unsupported saved transform rejected"),Invalid->RestoreObjectState(*Original,DynamicWithTransform(Bad)));Invalid->Destroy();
                    Source->SetActorTransform(Bad);FVoxelImmutableGeometry Refused;TArray<uint8> Bytes;FBox B;FVector BadHit;
                    Test->TestFalse(TEXT("unsupported capture rejected"),Source->CaptureObjectState(Refused,Bytes));
                    Test->TestFalse(TEXT("unsupported collision rejected"),Source->SolidAt(ToWorld(FVector(-5,-5,5))));
                    Test->TestFalse(TEXT("unsupported chopping query rejected"),Source->CanChop(ToWorld(FVector(-5,-5,5))));
                    Test->TestFalse(TEXT("unsupported trace rejected"),Source->Trace(ToWorld(FVector(-40,-5,5)),FVector::ForwardVector,100,BadHit));
                    Test->TestFalse(TEXT("unsupported preview rejected"),Source->GetDigBounds(ToWorld(FVector(-5,-5,5)),1,B));
                    Test->TestFalse(TEXT("unsupported edit rejected"),Source->Carve(ToWorld(FVector(-5,-5,5)),1));Source->SetActorTransform(Pose);
                }
                BadRestore=World->SpawnActor<AVoxelEnvironmentLODPrototype>();if(!BadRestore){Test->AddError(TEXT("spawn invalid staged target"));Cleanup();return true;}BadStatus=MakeShared<FYawRestoreStatus>();auto F=BadStatus;
                BadRestore->BeginStagedObjectRestore(Original,DynamicWithTransform(Unsupported[2]),[F](bool Ok){F->Called=true;F->Ok=Ok;});
            }
            for(int Size=1;Size<=3;++Size){
                if(Size>1&&!Source->InitializeAssetFromVxa(D,Vxa,true,false)){Test->AddError(TEXT("reset edit fixture"));Cleanup();return true;}
                const FVector LocalHit(-10.+Half,-10.+Half,Half);FBox Preview;
                Test->TestTrue(TEXT("quarter-yaw preview available"),Source->GetDigBounds(ToWorld(LocalHit),Size,Preview));
                const FBox Expected=WorldEdit(LocalEdit(Size),Q,Translation);
                Test->TestTrue(TEXT("preview minimum matches eight-corner oracle"),Preview.Min.Equals(Expected.Min,.0001));Test->TestTrue(TEXT("preview maximum matches eight-corner oracle"),Preview.Max.Equals(Expected.Max,.0001));
                FVoxelImmutableGeometry Before,After;TArray<uint8> Dynamic;Source->CaptureObjectState(Before,Dynamic);
                Test->TestTrue(TEXT("quarter-yaw carve clears selected volume"),Source->Carve(ToWorld(LocalHit),Size));VerifyCells(Source,Size);
                Source->CaptureObjectState(After,Dynamic);Test->TestTrue(TEXT("edit refreshes immutable geometry"),Before!=After);
            }
            TArray<uint8> Dynamic;if(!Source->CaptureObjectState(Geometry,Dynamic)){Test->AddError(TEXT("capture yaw snapshot"));Cleanup();return true;}
            Restored=World->SpawnActor<AVoxelEnvironmentLODPrototype>();if(!Restored){Test->AddError(TEXT("spawn staged yaw target"));Cleanup();return true;}Status=MakeShared<FYawRestoreStatus>();auto F=Status;
            Restored->BeginStagedObjectRestore(Geometry,Dynamic,[F](bool Ok){F->Called=true;F->Ok=Ok;});return false;
        }
        Restored->AdvanceStagedObjectRestore();if(BadRestore&&!BadStatus->Called)BadRestore->AdvanceStagedObjectRestore();
        if(!Status->Called||(BadStatus&&!BadStatus->Called))return false;
        if(BadStatus){Test->TestFalse(TEXT("staged decoder refuses non-quarter yaw"),BadStatus->Ok);BadRestore->Destroy();BadRestore=nullptr;BadStatus.Reset();}
        if(!Status->Ok||!Restored->PublishStagedObjectRestore()){Test->AddError(TEXT("staged quarter-yaw publication failed"));Cleanup();return true;}
        Test->TestTrue(TEXT("staged restore preserves translation and rotation"),Restored->GetActorTransform().Equals(Pose,.0001));VerifyCells(Restored,3);
        FVoxelImmutableGeometry Again;TArray<uint8> Dynamic;Test->TestTrue(TEXT("capture staged yaw result"),Restored->CaptureObjectState(Again,Dynamic));Test->TestTrue(TEXT("staged yaw retains original geometry allocation"),Again==Geometry);
        Source->Destroy();Restored->Destroy();Source=nullptr;Restored=nullptr;Geometry.Reset();Status.Reset();
        ++Case;if(Case==12){Cleanup();return true;}return false;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEnvironmentYawTest,"Voxel.Objects.EnvironmentQuarterYaw",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelEnvironmentYawTest::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FEnvironmentYawProbe(this));return true;}
#endif
