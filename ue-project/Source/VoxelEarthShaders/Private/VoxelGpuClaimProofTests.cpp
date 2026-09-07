#include "VoxelGpuClaimProof.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGpuClaimProofTest,"Voxel.Objects.GpuClaimProofCohort",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelGpuClaimProofTest::RunTest(const FString&){
    using namespace VoxelGpuClaimProof;
    const int64 First=Expected(396,396);
    TestEqual(TEXT("first async flush has no executed claim cohort"),First,int64(0));
    TestFalse(TEXT("later host staging cannot make first zero proof dark"),Dark(First,0));
    const int64 Next=Expected(800,404);
    TestEqual(TEXT("next flush expects prior deferred claims"),Next,int64(396));
    TestFalse(TEXT("matching deferred claims are healthy"),Dark(Next,396));
    TestTrue(TEXT("real missing claim cohort remains dark"),Dark(Next,0));
    TestTrue(TEXT("extra claim remains detectable"),Ahead(Next,397));
    TestEqual(TEXT("serial flush includes all claims"),Expected(800,0),int64(800));
    return true;
}
#endif
