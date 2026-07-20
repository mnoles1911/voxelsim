// vxc::Counters increment/read semantics (docs/debug-tooling-plan.md P1).

#include "voxelcore/counters.h"
#include "vxctest.h"

#include <thread>
#include <vector>

using namespace vxc;

VXC_TEST(counters_start_at_zero) {
    Counters c;
    CHECK_EQ(c.getBricksGenerated(), uint64_t(0));
    CHECK_EQ(c.getCellsWritten(), uint64_t(0));
    CHECK_EQ(c.getQuadsEmitted(), uint64_t(0));
    CHECK_EQ(c.getEditsApplied(), uint64_t(0));
    CHECK_EQ(c.getColumnEvals(), uint64_t(0));
}

VXC_TEST(counters_increment_default_and_explicit_amount) {
    Counters c;
    c.incBricksGenerated();      // default n=1
    c.incBricksGenerated(4);
    CHECK_EQ(c.getBricksGenerated(), uint64_t(5));

    c.incQuadsEmitted(10);
    CHECK_EQ(c.getQuadsEmitted(), uint64_t(10));

    c.incCellsWritten(512);
    CHECK_EQ(c.getCellsWritten(), uint64_t(512));

    c.incEditsApplied();
    CHECK_EQ(c.getEditsApplied(), uint64_t(1));

    c.incColumnEvals(3);
    CHECK_EQ(c.getColumnEvals(), uint64_t(3));
}

VXC_TEST(counters_are_independent) {
    Counters c;
    c.incBricksGenerated(2);
    CHECK_EQ(c.getCellsWritten(), uint64_t(0));
    CHECK_EQ(c.getQuadsEmitted(), uint64_t(0));
    CHECK_EQ(c.getEditsApplied(), uint64_t(0));
    CHECK_EQ(c.getColumnEvals(), uint64_t(0));
}

VXC_TEST(counters_reset_zeroes_all) {
    Counters c;
    c.incBricksGenerated(3);
    c.incCellsWritten(2);
    c.incQuadsEmitted(1);
    c.incEditsApplied(7);
    c.incColumnEvals(9);
    c.reset();
    CHECK_EQ(c.getBricksGenerated(), uint64_t(0));
    CHECK_EQ(c.getCellsWritten(), uint64_t(0));
    CHECK_EQ(c.getQuadsEmitted(), uint64_t(0));
    CHECK_EQ(c.getEditsApplied(), uint64_t(0));
    CHECK_EQ(c.getColumnEvals(), uint64_t(0));
}

VXC_TEST(counters_concurrent_increments_are_race_free) {
    Counters c;
    constexpr int kThreads = 8;
    constexpr uint64_t kPerThread = 20000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&c]() {
            for (uint64_t i = 0; i < kPerThread; ++i) {
                c.incCellsWritten();
                c.incQuadsEmitted(2);
            }
        });
    }
    for (auto& t : threads) t.join();
    CHECK_EQ(c.getCellsWritten(), uint64_t(kThreads) * kPerThread);
    CHECK_EQ(c.getQuadsEmitted(), uint64_t(kThreads) * kPerThread * 2);
}
