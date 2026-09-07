#pragma once
#include <atomic>
#include <cstdint>
namespace vxc {
// Owner-thread state transitions. Worker bodies do not reopen admission.
class CpuAdmissionGate {
public:
    bool isOpen() const{return open_.load(std::memory_order_acquire);}
    bool close(){return open_.exchange(false,std::memory_order_acq_rel);}
    // Only after all tracked borrowers have completed or prior to first launch.
    void reopen(){open_.store(true,std::memory_order_release);}
private:
    std::atomic<bool> open_{true};
};
// Deliberately no timeout return. Handles/resources stay owned by the caller
// throughout every wait/report cycle. waitOne must not need owner-thread work.
template<class Ready,class WaitOne,class Now,class Report>
void drainCpuBorrowers(Ready&& ready,WaitOne&& waitOne,Now&& now,Report&& report,uint64_t reportMilliseconds=60000) {
    uint64_t lastReport=now();
    while(!ready()) {
        waitOne();const uint64_t current=now();
        if(current-lastReport>=reportMilliseconds){report();lastReport=current;}
    }
}
}
