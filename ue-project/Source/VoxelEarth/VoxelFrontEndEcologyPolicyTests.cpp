#include "VoxelFrontEndPolicy.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFrontEndEcologySwitchPolicyTest,
    "VoxelEarth.FrontEnd.EcologySwitchPolicy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVoxelFrontEndEcologySwitchPolicyTest::RunTest(const FString& Parameters)
{
    const TCHAR* Drivers[]={TEXT("VoxelAppearanceForest"),TEXT("VoxelEcologyWorldCapture"),TEXT("VoxelEcologyRoute")};
    for(const TCHAR* Name:Drivers)TestTrue(Name,VoxelFrontEnd::IsSelfDrivingSwitchName(Name));
    const TCHAR* Inputs[]={TEXT("VoxelAppearanceForestCount"),TEXT("VoxelAppearanceForestCpuTrace"),TEXT("VoxelAppearanceForestExit"),TEXT("VoxelAppearanceForestHidden"),TEXT("VoxelAppearanceForestOutput"),TEXT("VoxelAppearanceForestSequence"),TEXT("VoxelAppearancePublishedSeed"),TEXT("VoxelAppearancePublishedSpecies"),TEXT("VoxelAssetAppearanceDir"),TEXT("VoxelAssetScatterOff"),TEXT("VoxelDetailLodColorTolerance"),TEXT("VoxelDetailLodMinSaving"),TEXT("VoxelDetailMeshCache"),TEXT("VoxelDetailMeshCachePreview"),TEXT("VoxelDetailMeshLOD"),TEXT("VoxelDetailSizeCull"),TEXT("VoxelEcologyConfig"),TEXT("VoxelEcologyDecisionTrace"),TEXT("VoxelEcologyForestLayout"),TEXT("VoxelEcologyRouteDiagnoseStalls"),TEXT("VoxelEcologyRouteOutput"),TEXT("VoxelEcologyRouteProfile"),TEXT("VoxelEcologyRouteSha256"),TEXT("VoxelEcologyWorldOutput"),TEXT("VoxelMarchDispatchIdentity"),TEXT("VoxelNoEcologyResolveCache"),TEXT("VoxelPredictiveAssetResolve"),TEXT("VoxelRayQuery"),TEXT("VoxelWalkWaitForEcology"),TEXT("VoxelAppearanceFellingTest"),TEXT("VoxelAppearanceTestVxa"),TEXT("VoxelEcologyTestPreview")};
    for(const TCHAR* Name:Inputs)TestFalse(Name,VoxelFrontEnd::IsSelfDrivingSwitchName(Name));
    // Exempt inputs do not weaken conventional explicit automation entry names.
    TestTrue(TEXT("ordinary fixture remains self-driving"),VoxelFrontEnd::IsSelfDrivingSwitchName(TEXT("VoxelWalkTest")));
    return true;
}
#endif
