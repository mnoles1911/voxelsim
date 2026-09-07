#include "VoxelDebrisLifecycle.h"
#include "Misc/AutomationTest.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelDebrisCleanupPolicyTest, "Voxel.DebrisCleanup.Policy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVoxelDebrisCleanupPolicyTest::RunTest(const FString&)
{
    FVoxelDebrisLifetimeState S;
    S.Kind=EVoxelDebrisLifetime::Cosmetic; S.RemainingSeconds=10.;
    TestFalse(TEXT("chip survives before 10 seconds"), UVoxelDebrisLifecycle::Advance(S,9.99,true));
    TestTrue(TEXT("chip expires even when observed"), UVoxelDebrisLifecycle::Advance(S,.02,true));
    S.Kind=EVoxelDebrisLifetime::Harvestable; S.RemainingSeconds=900.;
    TestFalse(TEXT("resource survives before 15 minutes"),UVoxelDebrisLifecycle::Advance(S,899.,false));
    TestFalse(TEXT("proximity pauses deadline"),UVoxelDebrisLifecycle::Advance(S,1000.,true));
    TestEqual(TEXT("protection preserves remaining time"),S.RemainingSeconds,1.);
    TestTrue(TEXT("unprotected resource expires"),UVoxelDebrisLifecycle::Advance(S,1.,false));
    for(auto Kind:{EVoxelDebrisLifetime::Substantial,EVoxelDebrisLifetime::Retained}) {
        S.Kind=Kind; S.RemainingSeconds=0.;
        TestFalse(TEXT("valuable/retained objects never expire"),UVoxelDebrisLifecycle::Advance(S,1.e9,false));
    }
    const FBox B(FVector(-10),FVector(10));
    TestTrue(TEXT("nearby behind camera protected"),UVoxelDebrisLifecycle::ProtectsBounds(B,FVector(1000,0,0),FVector(1,0,0)));
    TestTrue(TEXT("distant viewed object protected"),UVoxelDebrisLifecycle::ProtectsBounds(B,FVector(-8000,0,0),FVector(1,0,0)));
    TestFalse(TEXT("distant object behind camera unprotected"),UVoxelDebrisLifecycle::ProtectsBounds(B,FVector(8000,0,0),FVector(1,0,0)));
    TestFalse(TEXT("beyond viewing radius unprotected"),UVoxelDebrisLifecycle::ProtectsBounds(B,FVector(-20000,0,0),FVector(1,0,0)));
    auto Source=NewObject<UVoxelDebrisLifecycle>();
    S.Kind=EVoxelDebrisLifetime::Harvestable; S.RemainingSeconds=123.; Source->RestoreState(S);
    TArray<uint8> Bytes;
    { FMemoryWriter Writer(Bytes); FObjectAndNameAsStringProxyArchive Ar(Writer,false); Ar.ArIsSaveGame=true; Source->Serialize(Ar); }
    auto Restored=NewObject<UVoxelDebrisLifecycle>();
    { FMemoryReader Reader(Bytes); FObjectAndNameAsStringProxyArchive Ar(Reader,true); Ar.ArIsSaveGame=true; Restored->Serialize(Ar); }
    TestEqual(TEXT("save archive retains remaining timer"),Restored->CaptureState().RemainingSeconds,123.);
    TestTrue(TEXT("save archive retains category"),Restored->CaptureState().Kind==EVoxelDebrisLifetime::Harvestable);
    Restored->NotifyInteraction();
    TestEqual(TEXT("interaction resets inactivity"),Restored->CaptureState().RemainingSeconds,900.);
    Restored->Retain();
    TestTrue(TEXT("retention disables expiry"),Restored->CaptureState().Kind==EVoxelDebrisLifetime::Retained);
    return true;
}
#endif
