// W2 perf-gap benchmark (docs/status.md): measures WaterCA::step() wall-clock
// cost at the ~1900-2000 active-brick scale the old sequential (v0,
// kWaterCAVersion==1) engine was documented at 500-650ms/tick. Deliberately
// separate from the correctness suite (tests/test_waterca.cpp) -- this is a
// timing report, not a pass/fail check. Timing uses std::chrono; floats
// appear ONLY in reporting (ms figures), never in world/CA math, matching
// the rest of the bench harness (bench_main.cpp).
//
//   vxc_waterca_bench [--seed N] [--ticks N] [--solid-cache] [--count-queries]
//                     [--lake [--wake-edit]]
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

// --lake scenario: a large, FULLY SETTLED walled pool that is then disturbed
// by one trivial local edit per tick (a single unit of water dropped in one
// corner). This is the "persistent per-water-body structure" motivating case
// (docs/adr/0003-hydrostatic-persistent-body.md), which the 441-column pour
// above does NOT represent: here almost nothing changes tick over tick, yet
// Phase C's flood re-derives the ENTIRE connected body every tick, because
// water-side reach is deliberately unbounded (waterca.h "Phase C" step 1) so a
// single touched brick anywhere on the lake drags the whole lake back in. The
// reported ms/tick is therefore the standing per-tick cost of a settled lake --
// i.e. the size of the prize for making an unchanged body cost O(1).
// `spin` emulates an EXPENSIVE terrain query. The basin predicate itself costs
// a few nanoseconds, which is unlike the real engine: the live SolidFn is
// UVoxelWorldSubsystem::IsSolidAtVoxel -> World::materialAt -> Amplifier, which
// docs/status.md measures at ~1us/call. Without this knob the --lake numbers
// only expose the flood MACHINERY cost and say nothing about the terrain-query
// cost that actually dominates in the engine. The spin is deliberately trivial
// integer work (float-ban clean) whose only purpose is to burn a comparable
// amount of time per call; it is NOT a model of the amplifier.
WaterCA::SolidFn flatWalledBasin(int64_t halfSpan, uint32_t spin) {
    return [halfSpan, spin](int64_t vx, int64_t vy, int64_t vz) -> MaterialId {
        if (spin) {
            uint64_t h = static_cast<uint64_t>(vx * 73856093) ^ static_cast<uint64_t>(vy * 19349663) ^
                         static_cast<uint64_t>(vz * 83492791);
            for (uint32_t i = 0; i < spin; ++i) h = h * 6364136223846793005ull + 1442695040888963407ull;
            if (h == 1) return MAT_ROCK; // never taken; keeps the loop live
        }
        if (vz <= 0) return MAT_ROCK;
        if (vx < -halfSpan || vx > halfSpan || vy < -halfSpan || vy > halfSpan) return MAT_ROCK;
        return MAT_AIR;
    };
}

int runLake(int ticks, bool solidCache, int64_t kHalfSpan, uint32_t spin, bool wakeEdit) {
    WaterCA ca(flatWalledBasin(kHalfSpan, spin));
    ca.setSolidCacheEnabled(solidCache);

    // ~5 full layers, seeded COLUMN BY COLUMN straight onto the floor rather
    // than as one giant drop: addWater stacks vertically from its start voxel,
    // so a single 5M-unit call would build a ~19,600-voxel-tall water column
    // and spend thousands of ticks collapsing it. Seeding the floor directly
    // reaches the settled state we want to measure in a handful of ticks.
    uint64_t placed = 0;
    for (int64_t x = -kHalfSpan; x <= kHalfSpan; ++x)
        for (int64_t y = -kHalfSpan; y <= kHalfSpan; ++y) placed += ca.addWater(x, y, 1, 5 * 255);
    int settleTicks = 0;
    for (int i = 0; i < 20000; ++i) {
        ca.step();
        ++settleTicks;
        if (ca.steppedBrickCount() == 0) break;
    }
    std::printf("vxc_waterca_bench --lake: %llu units in a %lldx%lld basin, settled after %d ticks\n",
                (unsigned long long)placed, (long long)(2 * kHalfSpan + 1), (long long)(2 * kHalfSpan + 1),
                settleTicks);
    std::printf("  stored water bricks: %zu, solid cache: %s\n", ca.storedBrickCount(),
                solidCache ? "ON" : "off");

    // Now the steady state we actually care about: one trivial disturbance per
    // tick against an otherwise unchanging body.
    // --wake-edit: the WORST REALISTIC CASE for wakeRegion (waterca.h
    // "Terrain-edit reactivation") -- a player-sized terrain edit landing on
    // the rim of a large, fully settled lake EVERY SINGLE TICK, forever. This
    // is the "does waking cost back the solidity memo's win?" measurement.
    // Unlike the addWater disturbance it injects no volume: it only schedules,
    // so any cost it shows is purely the reactivation the fix introduces.
    double sum = 0;
    double wakeSum = 0;
    size_t wokenTotal = 0;
    int n = 0;
    for (int t = 0; t < ticks; ++t) {
        if (wakeEdit) {
            // A 3x3x3-voxel dig on the lake floor's corner, moved along the rim
            // so it is never the same brick two ticks running.
            const int64_t x = -kHalfSpan + (t % (2 * kHalfSpan));
            const auto w0 = Clock::now();
            wokenTotal += ca.wakeRegion(x, -kHalfSpan, 0, x + 2, -kHalfSpan + 2, 2);
            wakeSum += msSince(w0);
        } else {
            ca.addWater(-kHalfSpan, -kHalfSpan, 30, 1); // one unit, one corner
        }
        const auto t0 = Clock::now();
        ca.step();
        sum += msSince(t0);
        ++n;
    }
    std::printf("  %s avg step(): %.3f ms/tick over %d ticks\n",
                wakeEdit ? "wake-edit-per-tick settled lake" : "disturbed-settled-lake",
                n ? sum / double(n) : 0.0, n);
    if (wakeEdit)
        std::printf("  wakeRegion() itself: %.6f ms/call avg, %zu bricks woken over %d calls\n",
                    n ? wakeSum / double(n) : 0.0, wokenTotal, n);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    uint64_t seed = 20260719;
    int ticks = 600;
    // Cross-tick terrain-solidity memo (waterca.h setSolidCacheEnabled).
    // Defaults OFF, matching WaterCA's own default and therefore the live
    // engine path, so the unflagged bench number stays comparable with every
    // earlier docs/status.md figure. `--solid-cache` measures what the memo
    // buys; this bench's terrain (Amplifier worldgen, never edited) satisfies
    // the memo's purity contract vacuously.
    bool solidCache = false;
    bool countQueries = false;
    bool lake = false;
    int64_t lakeSpan = 31; // --lake footprint is (2*span+1)^2
    uint32_t lakeSpin = 0;  // --lake-solid-spin: emulated per-query terrain cost
    bool wakeEdit = false;  // --wake-edit: disturb the settled lake with terrain-edit wakes, not water
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--ticks" && i + 1 < argc) ticks = std::atoi(argv[++i]);
        else if (a == "--solid-cache") solidCache = true;
        else if (a == "--count-queries") countQueries = true;
        else if (a == "--lake") lake = true;
        else if (a == "--wake-edit") wakeEdit = true;
        else if (a == "--lake-span" && i + 1 < argc) lakeSpan = std::atoi(argv[++i]);
        else if (a == "--lake-solid-spin" && i + 1 < argc)
            lakeSpin = static_cast<uint32_t>(std::atoi(argv[++i]));
        else {
            std::fprintf(stderr,
                         "usage: vxc_waterca_bench [--seed n] [--ticks n] [--solid-cache] "
                         "[--count-queries]\n");
            return 2;
        }
    }

    if (lake) return runLake(ticks, solidCache, lakeSpan, lakeSpin, wakeEdit);

    SyntheticTileSampler tiles(seed);
    Amplifier amp(seed, tiles);
    // The query counter is OPT-IN because it is not free: the extra capture in
    // the SolidFn measurably slows the (type-erased, called tens of millions of
    // times per run) terrain callback, which would make any timing taken with
    // it on incomparable to every earlier docs/status.md figure. Time with it
    // OFF; count with it ON.
    uint64_t solidQueries = 0;
    WaterCA::SolidFn solidFn;
    if (countQueries) {
        solidFn = [&amp, &solidQueries](int64_t vx, int64_t vy, int64_t vz) {
            ++solidQueries;
            return amp.materialAt(vx, vy, vz);
        };
    } else {
        solidFn = [&amp](int64_t vx, int64_t vy, int64_t vz) { return amp.materialAt(vx, vy, vz); };
    }
    WaterCA ca(solidFn);
    ca.setSolidCacheEnabled(solidCache);

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
    samples.reserve(static_cast<size_t>(ticks < 0 ? 0 : ticks));
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

    if (countQueries)
        std::printf("solid cache: %s (terrain solid_ calls: %llu, memo bricks: %zu)\n",
                    solidCache ? "ON" : "off", (unsigned long long)solidQueries,
                    ca.solidCacheBrickCount());
    else
        std::printf("solid cache: %s (memo bricks: %zu; --count-queries reports solid_ call counts, "
                    "but slows the callback -- do not time with it on)\n",
                    solidCache ? "ON" : "off", ca.solidCacheBrickCount());
    std::printf("ticks measured: %zu, max active bricks reached: %zu\n", samples.size(), maxActive);
    std::printf("avg step() ms over full run: %.3f ms\n", avgAll);
    std::printf("avg step() ms in [%zu, %zu] active-brick window (n=%zu): %.3f ms\n", lo, hi, countWindow,
                avgWindow);
    std::printf("reference (v0 sequential engine, docs/status.md): 500-650 ms/tick at ~1900 active bricks\n");
    if (avgWindow > 0.0)
        std::printf("speedup vs v0 midpoint (575ms): %.0fx\n", 575.0 / avgWindow);
    return 0;
}
