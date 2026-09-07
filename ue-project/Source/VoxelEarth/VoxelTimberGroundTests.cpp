#include "VoxelTreeFellingPrototype.h"
#include "VoxelDetachedPersistence.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelTimberGroundBounds,"Voxel.Objects.TimberGroundBounds",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelTimberGroundBounds::RunTest(const FString&){
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,FName(*FString::Printf(TEXT("TimberGround_%s"),*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
    if(!TestNotNull(TEXT("isolated world"),World))return false;
    auto Timber=World->SpawnActor<AVoxelFallingTimber>();
    if(TestNotNull(TEXT("timber"),Timber)){
        // No visuals are needed for this geometric/support admission test.
        // The 60 m specimen deliberately exceeds the previous 32 m proxy.
        Timber->Initialize({},FBox(FVector(-50,-40,0),FVector(50,40,6000)),FTransform(FVector(-1234,5678,200)),FVector::ForwardVector,false);
        const FBox B=Timber->GetGroundSupportBounds();
        const double Radius=FVector(50,40,3000).Size()+400;
        TestTrue(TEXT("free body rotation radius includes trunk diagonal and margin"),B.GetExtent().Equals(FVector(Radius),.01));
        TestTrue(TEXT("coverage follows translated mass center"),B.GetCenter().Equals(Timber->GetActorLocation(),.01));
        TestTrue(TEXT("large support is not clamped to 32 m"),B.GetSize().X>6000);
        TestFalse(TEXT("no resident terrain cannot admit physics"),VoxelTreeFelling::AdvanceRestoreGround(World,B));
        Timber->Tick(0);
        TestFalse(TEXT("body waits frozen for terrain support"),Timber->Body->IsSimulatingPhysics());
        VoxelTreeFelling::NotifyTerrainEdited(World,B);
        VoxelTreeFelling::NotifyTerrainEdited(World,B);
        TestFalse(TEXT("repeated terrain invalidation keeps unsupported body frozen"),Timber->Body->IsSimulatingPhysics());
        const FVector Before=Timber->GetActorLocation();Timber->Tick(1);
        TestTrue(TEXT("waiting does not move body"),Timber->GetActorLocation().Equals(Before));
        TestFalse(TEXT("invalid box rejected"),VoxelTreeFelling::AdvanceRestoreGround(Wo