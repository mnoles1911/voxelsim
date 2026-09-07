#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelFineTileStreamer.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Async/Async.h"
namespace FineTileFallbackTests {
class FCoarse final:public vxc::ITileSampler {
public:
    int64 X=0,Y=0;int32 Calls=0;
    int32_t pixelSizeMm()const override{return 30000;}
    int32_t elevationMm(int64_t Px,int64_t Py)override{X=Px;Y=Py;++Calls;return int32(100000+Px*7+Py*11);}
    vxc::ClimateSample climate(int64_t,int64_t)override{return {};}
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFineTileFallbackTest,"Voxel.Objects.FineTileColdAbsenceFallback",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelFineTileFallbackTest::RunTest(const FString&){
    using namespace FineTileFallbackTests;
    const FString Root=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("Tests/FineColdAbsence"),FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FCoarse Coarse;FVoxelFineTileStreamer Stream(Root,TEXT("cold-absence-test"),123,1024*1024,&Coarse);Stream.SetCoarseFallback(&Coarse);
    auto& Sampler=Stream.WorldSampler();
    TestEqual(TEXT("first absent pixel immediately uses coarse elevation"),Sampler.elevationMm(-16384,-31232),int32(100000-1024*7-1952*11));
    TestEqual(TEXT("coarse X conversion"),Coarse.X,int64(-1024));TestEqual(TEXT("coarse Y conversion"),Coarse.Y,int64(-1952));
    TestTrue(TEXT("disk-confirmed absence is recorded"),Stream.DebugKnownAbsentForTest({-2,-4}));
    const uint64 Loads=Stream.BlockingLoadsSinceStart();
    TestEqual(TEXT("repeat absent query matches first answer"),Sampler.elevationMm(-16384,-31232),int32(100000-1024*7-1952*11));
    TestEqual(TEXT("repeat missing tile does not retry load"),Stream.BlockingLoadsSinceStart(),Loads);
    TestEqual(TEXT("negative fine pixel uses floor division"),Sampler.elevationMm(-1,-17),int32(100000-7-22));
    auto Worker=Async(EAsyncExecution::ThreadPool,[&](){return Sampler.elevationMm(-16384,-31232);});
    TestEqual(TEXT("worker uses established absence without loading"),Worker.Get(),int32(100000-1024*7-1952*11));
    TestEqual(TEXT("cold and warm absence never report gate leak"),Stream.GateLeaksSinceStart(),uint64(0));
    TestFalse(TEXT("fallback does not pretend fine footprint resident"),Stream.IsFootprintResident(-1,-1,1,1));

    // A present corrupt file must remain refused, not enter the absence memo.
    const std::string Key=vxc::formatFineTileCacheKey("cold-absence-test",123,2,2);
    const FString File=FPaths::Combine(Root,FString(UTF8_TO_TCHAR(Key.c_str()))+TEXT(".vxtl"));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(File),true);
    TArray<uint8> Bad;Bad.SetNumZeroed(16);
    if(!TestTrue(TEXT("corrupt fixture written"),FFileHelper::SaveArrayToFile(Bad,*File)))return false;
    AddExpectedError(TEXT("REFUSED and GIVEN UP"),EAutomationExpectedErrorFlags::Contains,1);
    constexpr int64 Mm=(2ll*8192+4096)*1875;
    TestFalse(TEXT("corrupt present tile refuses residency"),Stream.RequestFootprint(Mm,Mm,Mm+1,Mm+1));
    TestFalse(TEXT("corrupt bytes never authorize coarse fallback"),Stream.DebugKnownAbsentForTest({2,2}));
    IFileManager::Get().Delete(*File);
    return true;
}
#endif
