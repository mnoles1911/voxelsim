// vxc_hydroprobe -- what kMaxHydrostaticComponentCells actually costs and
// actually bounds.
//
// WHY THIS EXISTS. The hydrostatic cap (waterca.cpp, kMaxHydrostaticComponentCells)
// is the binding constraint on CA water correctness: an over-cap component is
// left COMPLETELY unmodified, the CA then goes quiet with zero active bricks,
// and every caller reads that as "settled" at a level nowhere near the datum.
// Before this probe the ONLY instrument was WaterCAProfile::hydroOverflowed,
// which needs -DVXC_WATER_PROFILE=ON, so the failure was invisible in any
// shipping build. This probe measures three things that were previously
// asserted rather than measured:
//
//   1. WHAT THE CAP BOUNDS. The flood does NOT stop at the cap -- it stops
//      RECORDING at the cap (`if (!overflowed) cells.push_back(...)`) and keeps
//      walking the whole component. So per-tick traversal cost is already
//      unbounded; the cap buys only the `cells` vector, the sort and the level
//      pass. Reported as popsOverflowed/tick against the component's true size.
//   2. HOW FAR OVER THE CAP a real engine-shaped runaway goes.
//   3. WHETHER THE DEFERRAL IS OBSERVABLE without a profile build --
//      i.e. whether a caller can tell "settled" from "gave up".
//
// The harness is the one docs/water-handover-2026-08-04.md and
// waterca_no_ceiling_can_fill_a_breach_over_the_hydrostatic_cap already use:
// an unbounded implicit sea on a flat rock seabed with a sealed dry room under
// it and a plug opening a shaft between the two. The room's own cells are CA
// territory from the first tick (mobilizeBrick refuses a brick holding no
// implicit water, and the room is below the datum band), which is why no
// mobilization policy -- front gate, mobilized ceiling, cooldown -- has any say
// over them. See the test's harness note; it is not restated here.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "voxelcore/lakes.h"
#include "voxelcore/waterca.h"

using namespace vxc;

namespace {

constexpr int64_t kSeaBedTopVz = 63;  // rock for vz <= 63, except where carved
constexpr int64_t kSeaSurfaceVz = 79; // sea occupies vz 64..79
constexpr int64_t kRoomLoVz = 32;     // the sealed dry room
constexpr int64_t kRoomHiVz = 47;

struct Breach {
    int64_t roomHi;
    int64_t shaftLo;
    int64_t shaftHi;
    // Cells in the room, air included -- the flood counts air, so this is the
    // number the cap is actually compared against, not the water volume.
    uint64_t roomCells() const {
        return uint64_t(roomHi + 1) * uint64_t(roomHi + 1) *
               uint64_t(kRoomHiVz - kRoomLoVz + 1);
    }
    uint64_t roomFull() const { return roomCells() * 255ull; }
};

WaterMobilizer::ImplicitFn seaWater() {
    return [](int64_t, int64_t, int64_t vz) -> uint8_t {
        return (vz > kSeaBedTopVz && vz <= kSeaSurfaceVz) ? 255 : 0;
    };
}

WaterCA::SolidFn seaTerrain(std::shared_ptr<bool> plugOpen, Breach b) {
    return [plugOpen, b](int64_t vx, int64_t vy, int64_t vz) -> MaterialId {
        if (vz > kSeaBedTopVz) return MAT_AIR;
        if (vz >= kRoomLoVz && vz <= kRoomHiVz && vx >= 0 && vx <= b.roomHi && vy >= 0 &&
            vy <= b.roomHi)
            return MAT_AIR;
        if (*plugOpen && vz > kRoomHiVz && vz <= kSeaBedTopVz && vx >= b.shaftLo &&
            vx <= b.shaftHi && vy >= b.shaftLo && vy <= b.shaftHi)
            return MAT_AIR;
        return MAT_ROCK;
    };
}

void openTheBreach(WaterMobilizer& mob, WaterCA& ca, std::shared_ptr<bool> plugOpen, Breach b) {
    *plugOpen = true;
    ca.invalidateSolidRegion(b.shaftLo, b.shaftLo, kRoomHiVz + 1, b.shaftHi, b.shaftHi,
                             kSeaBedTopVz);
    mob.mobilizeEditRegion(ca, b.shaftLo, b.shaftLo, kRoomHiVz + 1, b.shaftHi, b.shaftHi,
                           kSeaBedTopVz);
    ca.wakeRegion(b.shaftLo, b.shaftLo, kRoomHiVz + 1, b.shaftHi, b.shaftHi, kSeaBedTopVz);
}

uint64_t roomVolume(const WaterCA& ca, Breach b) {
    uint64_t v = 0;
    for (int64_t z = kRoomLoVz; z <= kRoomHiVz; ++z)
        for (int64_t y = 0; y <= b.roomHi; ++y)
            for (int64_t x = 0; x <= b.roomHi; ++x) v += ca.fillAt(x, y, z);
    return v;
}

void runArm(const char* label, Breach b, size_t ceiling, int ticks) {
    auto plugOpen = std::make_shared<bool>(false);
    WaterMobilizer mob(seaWater(), seaTerrain(plugOpen, b));
    WaterCA ca(mob.makeSolidFn());
    if (ceiling) {
        mob.setMobilizedCeiling(ceiling);
        mob.setCeilingRelief([&] { mob.demoteBudgeted(ca, 256); });
    }
    openTheBreach(mob, ca, plugOpen, b);

#ifdef VXC_WATER_PROFILE
    resetWaterCAProfile();
#endif
    int quietAt = -1;
    for (int i = 1; i <= ticks; ++i) {
        mob.advanceFront(ca);
        ca.step();
        mob.demoteBudgeted(ca, 32);
        if (quietAt < 0 && ca.activeBrickCount() == 0) quietAt = i;
    }

    const uint64_t filled = roomVolume(ca, b);
    const uint64_t full = b.roomFull();
    std::printf("%-22s roomCells=%-8llu cap=%-6llu (%.0fx over)  ceiling=%-5llu\n", label,
                (unsigned long long)b.roomCells(), 65536ull,
                double(b.roomCells()) / 65536.0, (unsigned long long)ceiling);
    std::printf("    filled %llu / %llu  = %llu%% of datum   activeBricks=%llu  quietAt=%d\n",
                (unsigned long long)filled, (unsigned long long)full,
                full ? (unsigned long long)(filled * 100 / full) : 0ull,
                (unsigned long long)ca.activeBrickCount(), quietAt);
    // The whole point: what can a SHIPPING build see here? Only these two
    // numbers, and both of them say "settled".
    std::printf("    observable without a profile build: activeBrickCount=%llu "
                "steppedBrickCount=%llu  ->  indistinguishable from settled\n",
                (unsigned long long)ca.activeBrickCount(),
                (unsigned long long)ca.steppedBrickCount());
#ifdef VXC_WATER_PROFILE
    const WaterCAProfile& p = waterCAProfile();
    std::printf("    [profile] components=%llu overflowed=%llu (%llu%%)  pops=%llu "
                "popsOverflowed=%llu (%llu%% of all pops)\n",
                (unsigned long long)p.hydroComponents, (unsigned long long)p.hydroOverflowed,
                p.hydroComponents ? (unsigned long long)(p.hydroOverflowed * 100 / p.hydroComponents) : 0ull,
                (unsigned long long)p.hydroPops, (unsigned long long)p.hydroPopsOverflowed,
                p.hydroPops ? (unsigned long long)(p.hydroPopsOverflowed * 100 / p.hydroPops) : 0ull);
    if (p.hydroOverflowed) {
        // THE HEADLINE. Mean true size of a deferred component, in cells,
        // against the 65,536 cap it was refused for. The flood walked every one
        // of these cells and then threw the answer away.
        std::printf("    [profile] mean deferred component walked %llu cells = %.1fx the cap "
                    "-- the traversal was PAID, only the level was skipped\n",
                    (unsigned long long)(p.hydroPopsOverflowed / p.hydroOverflowed),
                    double(p.hydroPopsOverflowed / p.hydroOverflowed) / 65536.0);
        std::printf("    [profile] hydro ns: flood=%llu level=%llu apply=%llu "
                    "-> level+apply is %llu%% of flood\n",
                    (unsigned long long)p.hydroFloodNs, (unsigned long long)p.hydroLevelNs,
                    (unsigned long long)p.hydroApplyNs,
                    p.hydroFloodNs ? (unsigned long long)((p.hydroLevelNs + p.hydroApplyNs) * 100 /
                                                          p.hydroFloodNs)
                                   : 0ull);
    }
#endif
    std::printf("\n");
}

} // namespace

// ---------------------------------------------------------------------------
// The COVE arm: what levelling an over-cap component costs on the fixture
// tests/test_ocean.cpp uses. Reproduced here (not shared) because the fixture
// lives in the test binary and this needs to sweep policies without paying for
// 427 other tests.
//
// One straight shoreline at vx == 0, seabed 1 m below the datum, land 1 m
// above; a 4x4 notch cut clean through the cliff. The sea is UNBOUNDED in x
// and y, so the mobilization front can always find another brick to convert --
// which is the whole point of the arm: the mobilized body grows every tick, so
// the hydrostatic component grows with it, and this is where the deferral used
// to be doing the mobilizer's job for it by accident.
// ---------------------------------------------------------------------------
namespace {

constexpr int32_t kSeabedMm = kSeaLevelMm - 1000;
constexpr int32_t kLandMm = kSeaLevelMm + 1000;
constexpr int64_t kCoveFloorVz = kSeabedMm / kVoxelSizeMm;
constexpr int64_t kCoveTopVz = kSeaLevelMm / kVoxelSizeMm - 1;

struct CoastWorld {
    std::set<std::array<int64_t, 3>> dug;
    static int32_t groundMm(int64_t vx) { return vx < 0 ? kSeabedMm : kLandMm; }
    bool isDug(int64_t x, int64_t y, int64_t z) const {
        return dug.count(std::array<int64_t, 3>{x, y, z}) != 0;
    }
    MaterialId solidAt(int64_t vx, int64_t vy, int64_t vz) const {
        if (isDug(vx, vy, vz)) return MAT_AIR;
        return vz * kVoxelSizeMm < groundMm(vx) ? MAT_ROCK : MAT_AIR;
    }
};

void runCove(const char* label, WaterCA::HydroLargeComponentPolicy pol, size_t ceiling,
             int budget, bool relief = true) {
    CoastWorld w;
    for (int64_t x = 0; x <= 3; ++x)
        for (int64_t y = 0; y <= 3; ++y)
            for (int64_t z = kCoveFloorVz; z <= 12; ++z) w.dug.insert({x, y, z});

    auto implicitFn = [](int64_t vx, int64_t vy, int64_t vz) -> uint8_t {
        (void)vy;
        const int32_t ground = CoastWorld::groundMm(vx);
        return implicitWaterFill(vz, ground, implicitWaterDatumMm(kNoWaterMm, ground), false);
    };
    auto solidFn = [&w](int64_t vx, int64_t vy, int64_t vz) { return w.solidAt(vx, vy, vz); };

    WaterMobilizer mob(implicitFn, solidFn);
    WaterCA ca(mob.makeSolidFn());
    ca.setHydroLargeComponentPolicy(pol);
    if (ceiling) {
        mob.setMobilizedCeiling(ceiling);
        // WITH relief the ceiling is a revolving door: hitting it demotes
        // bricks the front immediately re-mobilizes, so the pair is a cycle and
        // nothing can ever go quiet. WITHOUT relief the ceiling is a hard stop:
        // the mobilized body is FINITE, which is the only condition under which
        // a breach into an unbounded sea can settle at all.
        if (relief) mob.setCeilingRelief([&] { mob.demoteBudgeted(ca, 256); });
    }
    ca.invalidateSolidCache();
    mob.mobilizeEditRegion(ca, 0, 0, kCoveFloorVz, 3, 3, 12);

    int settled = -1;
    int caQuietAt = -1;
    for (int t = 0; t < budget; ++t) {
        mob.advanceFront(ca);
        ca.step();
        if (t > 3 && ca.steppedBrickCount() == 0 && mob.pendingFrontBricks() == 0) {
            settled = t;
            break;
        }
        if (caQuietAt < 0 && t > 3 && ca.activeBrickCount() == 0) caQuietAt = t;
    }
    // The cove's own column: is it wet to the datum?
    int topWet = -999;
    for (int64_t z = 12; z >= kCoveFloorVz; --z)
        if (ca.fillAt(1, 1, z) > 0) { topWet = int(z); break; }
    std::printf("%-34s settledAt=%-6d caQuietAt=%-6d pendFront=%-6llu topWet=%-4d (datum %lld)  "
                "mobilized=%llu  vol=%llu\n",
                label, settled, caQuietAt, (unsigned long long)mob.pendingFrontBricks(), topWet,
                (long long)kCoveTopVz, (unsigned long long)mob.mobilizedBricks().size(),
                (unsigned long long)ca.totalVolume());
    std::printf("    streamedComponents=%llu largestStreamedCells=%llu  gaveUp=%d "
                "deferralEvents=%llu  ledger==recomputed: %s  shortfall=%llu\n\n",
                (unsigned long long)ca.hydroStreamedComponents(),
                (unsigned long long)ca.hydroLargestStreamedCells(), int(ca.hydroGaveUp()),
                (unsigned long long)ca.hydroDeferralEvents(),
                ca.totalVolume() == ca.recomputeVolume() ? "YES" : "*** NO ***",
                (unsigned long long)mob.shortfallVolume());
}


// A CLOSED over-cap body: no mobilizer, no implicit field, no front. Just a
// walled basin bigger than kMaxHydrostaticComponentCells with water poured into
// one corner. This is the arm that answers "does the streaming level path
// CONVERGE", separately from anything the mobilizer does -- if levelling a
// large component were unstable, this would never go quiet and its volume would
// wander. Both policies are run over the identical scenario.
void runClosedBasin(const char* label, WaterCA::HydroLargeComponentPolicy pol, int budget) {
    // constexpr, and NOT captured: an integral constant expression is usable
    // inside the lambda without capture, and clang's -Wunused-lambda-capture
    // makes capturing it an error under -Werror. MSVC and g++ let it pass, so
    // this only ever shows up in CI.
    constexpr int64_t hi = 63; // 64x64 footprint, walls to z=31 -> 131,072 cells
    auto solid = [](int64_t x, int64_t y, int64_t z) -> MaterialId {
        if (z < 0) return MAT_ROCK;
        if (x < 0 || x > hi || y < 0 || y > hi) return MAT_ROCK;
        return MAT_AIR;
    };
    WaterCA ca(solid);
    ca.setHydroLargeComponentPolicy(pol);
    ca.addWater(3, 3, 30, 4000000);
    const uint64_t total = ca.totalVolume();
    int settled = -1;
    bool ledgerHeld = true;
    for (int t = 0; t < budget; ++t) {
        ca.step();
        if (ca.totalVolume() != total || ca.recomputeVolume() != total) ledgerHeld = false;
        if (ca.steppedBrickCount() == 0) { settled = t; break; }
    }
    // A levelled basin is flat: sample the surface height at two far corners.
    auto topWet = [&ca](int64_t x, int64_t y) {
        for (int64_t z = 40; z >= 0; --z)
            if (ca.fillAt(x, y, z) > 0) return int(z);
        return -1;
    };
    std::printf("%-34s settledAt=%-6d vol=%llu ledgerExact=%s  surface: (1,1)=%d (62,62)=%d "
                "(62,1)=%d  streamed=%llu gaveUp=%d\n",
                label, settled, (unsigned long long)ca.totalVolume(), ledgerHeld ? "YES" : "*NO*",
                topWet(1, 1), topWet(62, 62), topWet(62, 1),
                (unsigned long long)ca.hydroStreamedComponents(), int(ca.hydroGaveUp()));
}

} // namespace

int main(int argc, char** argv) {
    int ticks = 300;
    int coveBudget = 300;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--ticks=", 8) == 0) ticks = std::atoi(argv[i] + 8);
        if (std::strncmp(argv[i], "--cove-budget=", 14) == 0) coveBudget = std::atoi(argv[i] + 14);
    }
#ifndef VXC_WATER_PROFILE
    std::printf("NOTE: built without VXC_WATER_PROFILE -- the [profile] lines are absent,\n"
                "      which is exactly the shipping-build blindness this probe measures.\n\n");
#endif
    // 16x16x16 room = 4,096 cells: comfortably UNDER the cap. The control.
    runArm("under-cap  16^3", Breach{15, 4, 7}, 48, ticks);
    // 64x64x16 room = 65,536 cells: the cap EXACTLY, before one drop of sea.
    runArm("at-cap     64x64x16", Breach{63, 24, 39}, 64, ticks);
    // 128x128x16 = 262,144 cells: 4x over.
    runArm("over-cap   128x128x16", Breach{127, 56, 71}, 64, ticks);

    // The cove, three arms. Same fixture, same dig, only the hydrostatic
    // large-component policy and the mobilized ceiling differ.
    std::printf("--- CLOSED over-cap basin (no mobilizer at all) ---\n");
    runClosedBasin("v4 defer", WaterCA::HydroLargeComponentPolicy::kDeferOverCap, 3000);
    runClosedBasin("v5 stream", WaterCA::HydroLargeComponentPolicy::kLevelStreaming, 3000);
    std::printf("--- cove into an UNBOUNDED sea (tests/test_ocean.cpp fixture) ---\n");
    runCove("v4 defer, no ceiling", WaterCA::HydroLargeComponentPolicy::kDeferOverCap, 0, 300);
    runCove("v5 stream, no ceiling", WaterCA::HydroLargeComponentPolicy::kLevelStreaming, 0, coveBudget);
    runCove("v4 defer, ceiling 256", WaterCA::HydroLargeComponentPolicy::kDeferOverCap, 256, 2000);
    runCove("v5 stream, ceiling 256, no relief", WaterCA::HydroLargeComponentPolicy::kLevelStreaming, 256, 2000, false);
    runCove("v4 defer,  ceiling 256, no relief", WaterCA::HydroLargeComponentPolicy::kDeferOverCap, 256, 2000, false);
    runCove("v5 stream, ceiling 512", WaterCA::HydroLargeComponentPolicy::kLevelStreaming, 512, 2000);
    runCove("v5 stream, ceiling 256", WaterCA::HydroLargeComponentPolicy::kLevelStreaming, 256, 2000);
    return 0;
}
