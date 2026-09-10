#include "VoxelJumpBuffer.h"
#include "VoxelMovementIntegration.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelJumpBufferHitch,
    "Voxel.Movement.JumpBufferHitch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVoxelJumpBufferHitch::RunTest(const FString&)
{
    bool Fresh = true;
    double Remaining = VoxelMovement::AdvanceJumpBuffer(.12, Fresh, .35);
    TestEqual(TEXT("fresh press survives a frame longer than the buffer"), Remaining, .12);
    TestFalse(TEXT("freshness is spent once"), Fresh);
    Remaining = VoxelMovement::AdvanceJumpBuffer(Remaining, Fresh, .04);
    TestTrue(TEXT("unconsumed input ages on following update"), Remaining > .079 && Remaining < .081);
    Remaining = VoxelMovement::AdvanceJumpBuffer(Remaining, Fresh, .35);
    TestEqual(TEXT("old buffered input expires during a hitch"), Remaining, 0.0);
    Fresh = true;
    Remaining = VoxelMovement::AdvanceJumpBuffer(0.0, Fresh, .35);
    TestEqual(TEXT("cleared input is not resurrected"), Remaining, 0.0);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMovementHitchIntegration,
    "Voxel.Movement.HitchIntegration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVoxelMovementHitchIntegration::RunTest(const FString&)
{
    constexpr double Speed=495.0*.45, Gravity=980.0, Hitch=.304;
    // The former update lost this hop: (v-g*dt)*dt is already negative.
    TestTrue(TEXT("regression interval cancels old tapped jump"), (Speed-Gravity*Hitch)*Hitch<0.0);
    const int32 Steps=VoxelMovement::IntegrationSteps(Hitch);
    double Position=0.0,Velocity=Speed,Elapsed=0.0;
    const double Dt=Hitch/Steps;
    for(int32 I=0;I<Steps;++I)
    {
        Position+=VoxelMovement::GravityDisplacement(Velocity,Gravity,Dt);
        Velocity-=Gravity*Dt;
        Elapsed+=Dt;
    }
    TestTrue(TEXT("tap still visibly above floor after observed hitch"), Position>10.0 && Position<70.0);
    TestTrue(TEXT("substeps preserve full elapsed time"), FMath::Abs(Elapsed-Hitch)<1e-10);
    TestTrue(TEXT("free flight independent of frame partition"),
        FMath::Abs(Position-VoxelMovement::GravityDisplacement(Speed,Gravity,Hitch))<1e-10);
    TestTrue(TEXT("ordinary hitch steps bounded to 120Hz"), Dt<=1.0/120.0);
    TestEqual(TEXT("extreme intervals bound work"), VoxelMovement::IntegrationSteps(10.0),256);
    return true;
}
#endif
