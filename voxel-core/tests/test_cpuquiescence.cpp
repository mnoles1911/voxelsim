#include "voxelcore/cpuquiescence.h"
#include "vxctest.h"
#include <memory>
#include <vector>
using namespace vxc;
VXC_TEST(cpu_quiescence_retains_handles_and_external_marker_past_deadline) {
    CpuAdmissionGate gate; CHECK(gate.isOpen()); CHECK(gate.close()); CHECK(!gate.close());
    auto marker=std::make_shared<int>(37); std::weak_ptr<int> weak=marker;
    // External owner remains responsible for this borrowed marker until drain.
    const int* borrowed=marker.get();
    struct Handle { bool ready=false; }; std::vector<Handle> handles(2);
    int waits=0,reports=0; uint64_t now=0;
    drainCpuBorrowers([&]{for(const auto& h:handles)if(!h.ready)return false;return true;},
      [&]{++waits;now+=31000;CHECK(!weak.expired());CHECK_EQ(*borrowed,37);
          CHECK(!gate.isOpen());CHECK_EQ(handles.size(),size_t(2));
          if(waits==3)handles[0].ready=true;
          if(waits==5)handles[1].ready=true;},
      [&]{return now;},[&]{++reports;CHECK(!weak.expired());CHECK_EQ(handles.size(),size_t(2));});
    CHECK_EQ(waits,5);CHECK_EQ(reports,2);CHECK(!gate.isOpen());
    handles.clear();marker.reset();CHECK(weak.expired());
    gate.reopen();CHECK(gate.isOpen());
}
VXC_TEST(cpu_quiescence_empty_drain_does_not_wait_or_reopen_shutdown) {
    CpuAdmissionGate gate;gate.close();int waits=0,reports=0;
    const bool reopen=gate.close();
    drainCpuBorrowers([]{return true;},[&]{++waits;},[]{return uint64_t(0);},[&]{++reports;});
    if(reopen)gate.reopen();
    CHECK(!gate.isOpen());CHECK_EQ(waits,0);CHECK_EQ(reports,0);
}
