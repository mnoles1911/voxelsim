#include "VoxelCpuQuiescence.h"
#include "Async/Async.h"
#include "HAL/ThreadingBase.h"
#include "Misc/AutomationTest.h"
#include "voxelcore/lakes.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCpuQuiescenceTest,"Voxel.Objects.CpuQuiescence",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelCpuQuiescenceTest::RunTest(const FString&) {
    struct FMarker final:vxc::IWaterSampler {
        std::atomic<int>& Reads;std::atomic<bool>& Destroyed;
        FMarker(std::atomic<int>& R,std::atomic<bool>& D):Reads(R),Destroyed(D){}
        ~FMarker(){Destroyed.store(true);}
        int32_t waterSurfaceMmAtVoxel(int64_t,int64_t) override {Reads.fetch_add(1);return 123;}
    };
    vxc::CpuAdmissionGate Gate;std::atomic<int> Reads{0};std::atomic<bool> Destroyed{false},TimedOut{false};
    auto Marker=MakeUnique<FMarker>(Reads,Destroyed);vxc::IWaterSampler* Installed=Marker.Get();
    TArray<UE::Tasks::TTask<void>> Tasks;TArray<TFuture<void>> Futures;
    FQueuedThreadPool* Pool=FQueuedThreadPool::Allocate();
    if(!Pool->Create(1,128*1024,TPri_Normal,TEXT("VoxelQuiescenceTest"))){delete Pool;AddError(TEXT("pool creation failed"));return false;}
    auto Work=[&Gate,&TimedOut,Borrowed=Installed] {
        const double Deadline=FPlatformTime::Seconds()+2.;
        while(Gate.isOpen()&&FPlatformTime::Seconds()<Deadline)FPlatformProcess::Sleep(.001f);
        if(Gate.isOpen()){TimedOut.store(true);return;}
        FPlatformProcess::Sleep(.02f); // Delayed CPU completion, no GT callback/ticker.
        Borrowed->waterSurfaceMmAtVoxel(0,0);
    };
    auto TaskWork=Work;
    Tasks.Add(UE::Tasks::Launch(TEXT("VoxelQuiescenceTestTask"),MoveTemp(TaskWork)));
    Futures.Add(AsyncPool(*Pool,TUniqueFunction<void()>(Work)));
    // Same production seam invoked by InstallWaterMarker before replacing its
    // external pointer. Neither borrower owns the marker or its unique owner.
    int Reports=0;
    const auto Receipt=DrainVoxelCpuBorrowers(Gate,Tasks,Futures,[&](int32,int32){++Reports;},.001);
    TestFalse(TEXT("external marker retained through drain"),Destroyed.load());
    TestEqual(TEXT("both scheduling paths sampled before uninstall"),Reads.load(),2);
    Installed=nullptr;Marker.Reset();
    TestTrue(TEXT("external marker can now be destroyed"),Destroyed.load());
    TestFalse(TEXT("worker never needed GT progress"),TimedOut.load());
    TestEqual(TEXT("task receipt"),Receipt.Tasks,1);TestEqual(TEXT("pool receipt"),Receipt.Pool,1);
    // Scheduling may finish both workers before the first readiness check.
    // Periodic reporting is covered by the core test's deterministic clock.
    TestTrue(TEXT("handles released after completion"),Tasks.IsEmpty()&&Futures.IsEmpty());
    TestFalse(TEXT("shutdown gate stays closed"),Gate.isOpen());
    Gate.reopen();TestTrue(TEXT("mutation may explicitly resume"),Gate.isOpen());
    Pool->Destroy();delete Pool;return true;
}
#endif
