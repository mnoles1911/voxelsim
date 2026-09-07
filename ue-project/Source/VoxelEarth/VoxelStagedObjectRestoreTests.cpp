#include "VoxelEnvironmentLODPrototype.h"
#include "VoxelTreeFellingPrototype.h"
#include "VoxelDetachedPersistence.h"
#include "VoxelPackedTimberMesh.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

namespace {
struct FRestoreProbeFlags {int32 PlantCalls=0,TimberCalls=0,CancelCalls=0;bool Plant=false,Timber=false,CancelResult=true;};
class FStagedObjectRestoreProbe final : public IAutomationLatentCommand {
    FAutomationTestBase* Test;
    UWorld* World=nullptr;
    AVoxelEnvironmentLODPrototype* Plant=nullptr;
    AVoxelEnvironmentLODPrototype* Cancelled=nullptr;
    AVoxelFallingTimber* Timber=nullptr;
    FVoxelImmutableGeometry PlantGeometry,TimberGeometry;
    TSharedPtr<FRestoreProbeFlags> Flags=MakeShared<FRestoreProbeFlags>();
    double Start=0;bool CancelIssued=false;
    void Cleanup(){if(World){VoxelObjects::Forget(World);World->DestroyWorld(false);World=nullptr;}}
public:
    explicit FStagedObjectRestoreProbe(FAutomationTestBase* In):Test(In){}
    virtual bool Update() override {
        if(!World){
            Start=FPlatformTime::Seconds();World=UWorld::CreateWorld(EWorldType::Game,false,FName(*FString::Printf(TEXT("StagedProbe_%s"),*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
            if(!World){Test->AddError(TEXT("create isolated staged restore world"));return true;}
            Plant=World->SpawnActor<AVoxelEnvironmentLODPrototype>();Cancelled=World->SpawnActor<AVoxelEnvironmentLODPrototype>();Timber=World->SpawnActor<AVoxelFallingTimber>();
            if(!Plant||!Cancelled||!Timber){Test->AddError(TEXT("spawn staged actors"));Cleanup();return true;}
            TArray<uint8> PlantDynamic,TimberDynamic;
            auto PG=MakeShared<TArray<uint8>,ESPMode::ThreadSafe>();
            {FMemoryWriter W(*PG);VoxelObjectGeometrySnapshot::WriteVersion(W);TArray<uint8> Cells;Cells.Init(16,8);VoxelDetachedPersistence::Bytes(W,Cells);}
            {FMemoryWriter W(PlantDynamic);VoxelObjectGeometrySnapshot::WriteVersion(W);int32 Kind=0,MaxZ=MAX_int32;FTransform Transform=FTransform::Identity;FIntVector Size(2),Origin(0);double Mm=100;bool Collision=true,Severed=false;W<<Kind<<Transform<<Size<<Origin<<Mm<<MaxZ<<Collision<<Severed;}
            PlantGeometry=PG;
            auto TG=MakeShared<TArray<uint8>,ESPMode::ThreadSafe>();
            {FMemoryWriter W(*TG);VoxelObjectGeometrySnapshot::WriteVersion(W);int32 Count=1;FTransform Relative=FTransform::Identity;bool Visible=true;uint8 Cap=0;W<<Count<<Relative<<Visible<<Cap;
                FProcMeshSection S;S.ProcVertexBuffer.SetNum(3);S.ProcVertexBuffer[0].Position=FVector(0,0,0);S.ProcVertexBuffer[1].Position=FVector(10,0,0);S.ProcVertexBuffer[2].Position=FVector(0,10,0);
                for(auto& V:S.ProcVertexBuffer){V.Normal=FVector::UpVector;V.UV0=FVector2D::ZeroVector;V.UV1=FVector2D::ZeroVector;V.Tangent=FProcMeshTangent(FVector::ForwardVector,false);V.Color=FColor::White;}
                S.ProcIndexBuffer.Append({0,1,2});FProcMeshSection Unused;Test->TestTrue(TEXT("encode packed test mesh"),VoxelPackedTimberMesh::Serialize(W,Unused,&S));}
            {FMemoryWriter W(TimberDynamic);VoxelObjectGeometrySnapshot::WriteVersion(W);FBox Trunk(FVector(-5,-5,0),FVector(5,5,10));FTransform Transform(FVector(200,0,100)),Hinge=FTransform::Identity;FVector Linear=FVector::ZeroVector,Angular=FVector::ZeroVector,Heading=FVector::ForwardVector;bool Hinged=false,Awake=false,Breakable=false;double Age=1,Remaining=900;uint8 Kind=2;W<<Trunk<<Transform<<Linear<<Angular<<Hinged<<Hinge<<Awake<<Breakable<<Heading<<Age<<Kind<<Remaining;}
            TimberGeometry=TG;auto F=Flags;
            Plant->BeginStagedObjectRestore(PlantGeometry,PlantDynamic,[F](bool Ok){++F->PlantCalls;F->Plant=Ok;});
            Cancelled->BeginStagedObjectRestore(PlantGeometry,PlantDynamic,[F](bool Ok){++F->CancelCalls;F->CancelResult=Ok;});
            Timber->BeginStagedObjectRestore(TimberGeometry,TimberDynamic,[F](bool Ok){++F->TimberCalls;F->Timber=Ok;});
            Test->TestTrue(TEXT("begin hides plant"),Plant->IsHidden());Test->TestFalse(TEXT("begin freezes timber"),Timber->Body->IsSimulatingPhysics());
            return false;
        }
        if(FPlatformTime::Seconds()-Start>60){Test->AddError(TEXT("staged restore timed out"));Cleanup();return true;}
        Plant->AdvanceStagedObjectRestore();Timber->AdvanceStagedObjectRestore();
        if(!CancelIssued&&Cancelled->AdvanceStagedObjectRestore()){
            CancelIssued=true;Cancelled->CancelStagedObjectRestore();
            Test->TestEqual(TEXT("partial setup removed on cancellation"),Cancelled->LevelCount(),0);
        }
        if(!Flags->PlantCalls||!Flags->TimberCalls||!Flags->CancelCalls)return false;
        Test->TestTrue(TEXT("plant worker and staged upload succeeded"),Flags->Plant);Test->TestTrue(TEXT("timber worker and staged upload succeeded"),Flags->Timber);
        Test->TestEqual(TEXT("cancel completion once"),Flags->CancelCalls,1);Test->TestFalse(TEXT("cancel reports failure"),Flags->CancelResult);
        Test->TestTrue(TEXT("ready plant still hidden"),Plant->IsHidden());Test->TestTrue(TEXT("ready timber still hidden"),Timber->IsHidden());Test->TestFalse(TEXT("ready timber still frozen"),Timber->Body->IsSimulatingPhysics());
        FVoxelImmutableGeometry Captured;TArray<uint8> Dynamic;
        Test->TestFalse(TEXT("unpublished plant cannot be saved"),Plant->CaptureObjectState(Captured,Dynamic));
        Test->TestTrue(TEXT("publish plant"),Plant->PublishStagedObjectRestore());Test->TestTrue(TEXT("publish timber"),Timber->PublishStagedObjectRestore());
        Test->TestTrue(TEXT("restored source collision agrees"),Plant->SolidAt(FVector(5,5,5)));
        Test->TestTrue(TEXT("capture published plant"),Plant->CaptureObjectState(Captured,Dynamic));Test->TestTrue(TEXT("plant retains original immutable geometry"),Captured==PlantGeometry);
        Test->TestTrue(TEXT("capture published timber"),Timber->CaptureObjectState(Captured,Dynamic));Test->TestTrue(TEXT("timber retains original immutable geometry"),Captured==TimberGeometry);
        Cleanup();return true;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelStagedObjectRestoreTest,"Voxel.Objects.StagedRestore",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelStagedObjectRestoreTest::RunTest(const FString&){
    ADD_LATENT_AUTOMATION_COMMAND(FStagedObjectRestoreProbe(this));return true;
}
#endif
