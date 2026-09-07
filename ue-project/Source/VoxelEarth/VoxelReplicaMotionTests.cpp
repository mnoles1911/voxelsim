#include "VoxelReplicaMotion.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelReplicaMotionTest,"Voxel.Objects.ReplicaMotion",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelReplicaMotionTest::RunTest(const FString&)
{
    const FTransform A(FQuat::Identity,FVector::ZeroVector);
    const FTransform B(FQuat(FVector::UpVector,UE_DOUBLE_PI*.5),FVector(100,0,0));
    VoxelReplicaMotion::FBlend Blend{A,B,10.,.1};
    TestTrue(TEXT("before first sample clamps"),Blend.Sample(9.).Equals(A));
    TestTrue(TEXT("midpoint translation"),Blend.Sample(10.05).GetLocation().Equals(FVector(50,0,0),1.e-6));
    TestTrue(TEXT("midpoint rotation"),Blend.Sample(10.05).GetRotation().Equals(FQuat(FVector::UpVector,UE_DOUBLE_PI*.25),1.e-6));
    TestTrue(TEXT("packet loss holds final pose without extrapolation"),Blend.Sample(100.).Equals(B));
    TestTrue(TEXT("sample completes"),Blend.Complete(10.11));
    TestFalse(TEXT("midpoint not complete"),Blend.Complete(10.05));
    TestTrue(TEXT("timber interpolates"),VoxelReplicaMotion::ShouldBlend(2,A,B));
    TestFalse(TEXT("standing lattice never rotates between quarter turns"),VoxelReplicaMotion::ShouldBlend(3,A,B));
    TestFalse(TEXT("teleport snaps"),VoxelReplicaMotion::ShouldBlend(2,A,FTransform(FVector(1001,0,0))));
    FTransform Scaled=B;Scaled.SetScale3D(FVector(2));
    TestFalse(TEXT("scale correction snaps"),VoxelReplicaMotion::ShouldBlend(2,A,Scaled));
    // A packet arriving during a blend restarts from the displayed pose.
    const auto Middle=Blend.Sample(10.05);
    VoxelReplicaMotion::FBlend Next{Middle,FTransform(FVector(200,0,0)),10.05,.1};
    TestTrue(TEXT("retarget has no position discontinuity"),Next.Sample(10.05).Equals(Middle));
    TestTrue(TEXT("retarget reaches newest server state"),Next.Sample(10.2).GetLocation().Equals(FVector(200,0,0)));
    return true;
}
#endif
