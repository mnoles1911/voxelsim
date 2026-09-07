#include "VoxelSurfaceLighting.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSurfaceLightingDerivationTest,"Voxel.Objects.SurfaceLightingDerivation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FVoxelSurfaceLightingDerivationTest::RunTest(const FString&)
{
    using namespace VoxelSurfaceLighting;
    // Independent pre-refactor oracle across off/no-sun/dusk/day and clamped
    // controls, including authored warm and dark night colours.
    for(float Z:{-1.f,-.02f,0.f,.065f,.15f,1.f})
    for(float Floor:{-.2f,0.f,.34f,1.4f})
    for(float Intensity:{-1.f,0.f,1.5f,3.f})
    for(const FVector3f Tint:{FVector3f(1,1.04f,1.12f),FVector3f(1,.4f,.1f),FVector3f(.01f,.02f,.04f)})
    {
        const float T=FMath::Clamp((Z+.02f)/.17f,0.f,1.f);
        const float Engagement=T*T*(3.f-2.f*T);
        float I=FMath::Max(Intensity,0.f);I*=1.f-FMath::Clamp(Floor,0.f,.95f)*Engagement;
        const FVector4f Reference(Tint.X*I,Tint.Y*I,Tint.Z*I,.45f);
        const auto Actual=Ambient(Intensity,.45f,Floor,DuskFade(Z),Tint);
        TestTrue(TEXT("ambient preserves original float result"),FMemory::Memcmp(&Actual,&Reference,sizeof(Reference))==0);
        const auto U=Wrap(true,true,FVector3f(0,0,Z),Floor,.95f,1.5f,Tint);
        const float Gain=1.5f*Engagement;
        TestEqual(TEXT("wrap uses identical dusk colour gain"),U.WrapColorAndSkyBoost.X,Gain*Tint.X);
    }
    const auto Off=Wrap(false,true,FVector3f(0,0,1),.34f,.95f,1.5f,FVector3f(1));
    TestEqual(TEXT("master off uses sentinel"),Off.SunDirAndWrapFloor.W,-1.f);
    const auto Missing=Wrap(true,false,FVector3f(0,0,1),.34f,.95f,1.5f,FVector3f(1));
    TestEqual(TEXT("unpublished sun contributes no wrap"),Missing.WrapColorAndSkyBoost.X,0.f);
    const auto Noon=Wrap(true,true,FVector3f(0,0,1),.34f,.95f,1.5f,FVector3f(1));
    TestEqual(TEXT("sun-facing normal receives no duplicated direct sun"),SunDeficit(FVector3f(0,0,1),Noon.SunDirAndWrapFloor,.95f),0.f);
    TestEqual(TEXT("anti-sun face receives only floor deficit"),SunDeficit(FVector3f(0,0,-1),Noon.SunDirAndWrapFloor,.95f),.34f);
    TestEqual(TEXT("side receives half-Lambert deficit"),SunDeficit(FVector3f(1,0,0),Noon.SunDirAndWrapFloor,.95f),.5f);
    TestEqual(TEXT("off arm never adds support sun"),SunDeficit(FVector3f(0,0,-1),Off.SunDirAndWrapFloor,.95f),0.f);
    const auto Night=Wrap(true,true,FVector3f(0,0,-1),.34f,.95f,1.5f,FVector3f(1));
    TestEqual(TEXT("night has no persistent sun emission"),Night.WrapColorAndSkyBoost.X,0.f);
    return true;
}
#endif
