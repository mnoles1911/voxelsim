// W2 perf-gap benchmark (docs/status.md): measures WaterCA::step() wall-clock
// cost at the ~1900-2000 active-brick scale the old sequential (v0,
// kWaterCAVersion==1) engine was documented at 500-650ms/tick. Deliberately
// separate from the correctness suite (tests/test_waterca.cpp) -- this is a
// timing report, not a pass/fail check. Timing uses std::chrono; floats
// appear ONLY in reporting (ms figures), never in world/CA math, matching
// the rest of the bench harness (bench_main.cpp).
//
//   vxc_waterca_bench [--seed N] [--ticks N]
//
// Scenario: a large pour (many drop columns, big total volume) over real
// bumpy terrain (SyntheticTileSampler + Amplifier, same solid-query source
// test_waterca.cpp's fuzz test uses) so the active-brick count climbs into
// the thousands during the early spreading phase before settling back down
// -- exactly the "large pour over bumpy terrain" scale docs/status.md's
// perf-gap note describes. Every step() call's wall-clock time and active
// brick count are recorded; the report highlights the average over ticks
// landing in the [1800, 2200] active-brick window (closest available
// window reported if that exact band is never hit) alongside the full-run
// average and max active count actually reached.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "voxelcore/generator.h"
#include "voxelcore/waterca.h"

using namespace vxc;

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct TickSample {
    size_t activeBricks;
    double ms;
};

} // namespace

int main(int argc, char** argv) {
    uint64_t seed = 20260719;
    int ticks = 600;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--ticks" && i + 1 < argc) ticks = std::atoi(argv[++i]);
        else {
            std::fprintf(stderr, "usage: vxc_waterca_bench [--seed n] [--ticks n]\n");
            return 2;
        }
    }

    SyntheticTileSampler tiles(seed);
    Amplifier amp(seed, tiles);
    WaterCA ca([&amp](int64_t vx, int64_t vy, int64_t vz) { return amp.materialAt(vx, vy, vz); });

    // Large pour: a grid of drop columns spread over a wide area, each well
    // clear of the local surface, so the flood front covers thousands of
    // bricks simultaneously during the spreading phase -- the scale
    // docs/status.md's perf-gap note measured the old engine at.
    uint64_t totalPlaced = 0;
    constexpr int64_t kSpacing = 6;
    constexpr int64_t kHalfSpan = 60; // (2*60/6+1)^2 = 441 drop columns
    for (int64_t x = -kHalfSpan; x <= kHalfSpan; x += kSpacing) {
        for (int64_t y = -kHalfSpan; y <= kHalfSpan; y += kSpacing) {
            const ColumnSample col = amp.column(x, y);
            const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            const int64_t dropZ = topVz + 40;
            totalPlaced += ca.addWater(x, y, dropZ, 4000);
        }
    }
    std::printf("vxc_waterca_bench: seed %llu, placed %llu units over 441 columns\n",
                (unsigned long long)seed, (unsigned long long)totalPlaced);

    std::vector<TickSample> samples;
    samples.reserve(ticks);
    size_t maxActive = 0;
    for (int t = 0; t < ticks; ++t) {
        const size_t activeBefore = ca.activeBrickCount();
        const auto t0 = Clock::now();
        ca.step();
        const double ms = msSince(t0);
        samples.push_back({activeBefore, ms});
        maxActive = std::max(maxActive, activeBefore);
        if (ca.steppedBrickCount() == 0) {
            std::printf("settled after %d ticks\n", t + 1);
            break;
        }
    }

    double sumAll = 0;
    for (const auto& s : samples) sumAll += s.ms;
    const double avgAll = samples.empty() ? 0.0 : sumAll / double(samples.size());

    // Closest-available window around [1800, 2200] active bricks (report
    // the literal target band if it was reached; otherwise the window
    // around the actual peak, so the report is still meaningful on a
    // scenario that doesn't hit exactly 1900-2000).
    size_t bestCenter = 0;
    size_t bestDist = SIZE_MAX;
    for (const auto& s : samples) {
        const size_t dist = s.activeBricks > 2000 ? s.activeBricks - 2000
                             : s.activeBricks < 1800 ? 1800 - s.activeBricks
                                                      : 0;
        if (dist < bestDist) { bestDist = dist; bestCenter = s.activeBricks; }
    }
    double sumWindow = 0;
    size_t countWindow = 0;
    const size_t lo = bestCenter > 200 ? bestCenter - 200 : 0;
    const size_t hi = bestCenter + 200;
    for (const auto& s : samples) {
        if (s.activeBricks >= lo && s.activeBricks <= hi) { sumWindow += s.ms; ++countWindow; }
    }
    const double avgWindow = countWindow ? sumWindow / double(countWindow) : 0.0;

    std::printf("ticks measured: %zu, max active bricks reached: %zu\n", samples.size(), maxActive);
    std::printf("avg step() ms over full run: %.3f ms\n", avgAll);
    std::printf("avg step() ms in [%zu, %zu] active-brick window (n=%zu): %.3f ms\n", lo, hi, countWindow,
                avgWindow);
    std::printf("reference (v0 sequential engine, docs/status.md): 500-650 ms/tick at ~1900 active bricks\n");
    if (avgWindow > 0.0)
        std::printf("speedup vs v0 midpoint (575ms): %.0fx\n", 575.0 / avgWindow);
    return 0;
}
