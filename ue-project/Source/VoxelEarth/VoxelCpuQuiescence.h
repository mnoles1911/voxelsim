#pragma once
#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Tasks/Task.h"
#include "voxelcore/cpuquiescence.h"
// The production CPU join seam. No provider lock or GT pumping is permitted.
struct FVoxelCpuDrainReceipt { int32 Tasks=0,Pool=0; double Seconds=0; };
template<class Report>
FVoxelCpuDrainReceipt DrainVoxelCpuBorrowers(vxc::CpuAdmissionGate& Gate,
    TArray<UE::Tasks::TTask<void>>& Tasks,TArray<TFuture<void>>& Futures,
    Report&& ReportPending,double ReportSeconds=60.) {
    check(IsInGameThread());Gate.close();
    FVoxelCpuDrainReceipt Receipt{Tasks.Num(),Futures.Num(),0.};
    const double Start=FPlatformTime::Seconds();
    vxc::drainCpuBorrowers([&]{
        for(const auto& T:Tasks)if(!T.IsCompleted())return false;
        for(const auto& F:Futures)if(!F.IsReady())return false;
        return true;
    },[&]{
        for(auto& T:Tasks)if(!T.IsCompleted()){T.Wait(FTimespan::FromSeconds(1));return;}
        for(auto& F:Futures)if(!F.IsReady()){F.WaitFor(FTimespan::FromSeconds(1));return;}
    },[]{return uint64(FPlatformTime::Seconds()*1000.);},[&]{
        int32 T=0,F=0;for(const auto& H:Tasks)T+=!H.IsCompleted();
        for(const auto& H:Futures)F+=!H.IsReady();ReportPending(T,F);
    },uint64(FMath::Max(1.,ReportSeconds*1000.)));
    Tasks.Empty();Futures.Empty();Receipt.Seconds=FPlatformTime::Seconds()-Start;
    return Receipt;
}
