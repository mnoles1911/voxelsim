#include "VoxelGameplayActors.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGameplayActorCodecTest,"Voxel.Persistence.GameplayActors",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelGameplayActorCodecTest::RunTest(const FString&)
{
    using namespace VoxelGameplayActors;
    FRecord Charge; Charge.Kind=EKind::Explosive; Charge.Id=FGuid::NewGuid();
    Charge.Transform=FTransform(FRotator(12,35,-6),FVector(-1000000,2000000,400));
    Charge.Velocity=FVector(200,-80,40); Charge.Radius=213.5; Charge.Remaining=0.75;
    TArray<uint8> Bytes; TArray<FRecord> Restored;
    TestTrue(TEXT("Encode already-rolled charge"),Encode({Charge},Bytes));
    TestTrue(TEXT("Decode charge"),Decode(Bytes,Restored));
    if(Restored.Num()!=1) return false;
    TestEqual(TEXT("Persistent charge identity"),Restored[0].Id,Charge.Id);
    TestEqual(TEXT("Remaining fuse, not elapsed wall time"),Restored[0].Remaining,0.75);
    TestEqual(TEXT("Radius is not rerolled"),Restored[0].Radius,213.5);
    TestTrue(TEXT("Negative/large transform preserved"),Restored[0].Transform.Equals(Charge.Transform));
    TestEqual(TEXT("Velocity preserved"),Restored[0].Velocity,Charge.Velocity);
    TestFalse(TEXT("Duplicate identity refused"),Encode({Charge,Charge},Bytes));
    Charge.Remaining=4; TestFalse(TEXT("Impossible fuse refused"),Encode({Charge},Bytes));
    Charge.Remaining=0; TestTrue(TEXT("Due fuse is representable"),Encode({Charge},Bytes));
    Bytes.Add('x'); TestFalse(TEXT("Trailing malformed JSON refused"),Decode(Bytes,Restored));
    TestEqual(TEXT("No partially decoded actors"),Restored.Num(),0);
    FRecord Boat; Boat.Id=FGuid::NewGuid();
    TestFalse(TEXT("Missing hull identity cannot silently become a placeholder"),Encode({Boat},Bytes));
    return true;
}
#endif
