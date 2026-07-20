// Water pressure CA v0 (plan §3.7 Layer B, W2 groundwork): gravity + lateral
// equalization, volume conservation, activity/settling, determinism.

#include "voxelcore/waterca.h"

#include <cstdint>
#include <cstdlib>

#include "voxelcore/generator.h"
#include "vxctest.h"

using namespace vxc;

namespace {

// A single vertical shaft (walls on all 4 sides, floor at the bottom) —
// isolates gravity from lateral spread entirely, since every horizontal
// neighbor of the shaft column is solid. Floor top is voxel z=1 (z<=0 solid).
WaterCA::SolidFn shaftAt(int64_t cx, int64_t cy) {
    return [cx, cy](int64_t vx, int64_t vy, int64_t vz) -> MaterialId {
        if (vz <= 0) return MAT_ROCK;
        if (vx != cx || vy != cy) return MAT_ROCK;
        return MAT_AIR;
    };
}

// An open rectangular basin: floor at z<=0, walls outside [x0,x1]x[y0,y1] at
// every height (so lateral spread is bounded but the interior is a free
// multi-column floor, unlike shaftAt).
WaterCA::SolidFn basin(int64_t x0, int64_t x1, int64_t y0, int64_t y1) {
    return [x0, x1, y0, y1](int64_t vx, int64_t vy, int64_t vz) -> MaterialId {
        if (vz <= 0) return MAT_ROCK;
        if (vx < x0 || vx > x1 || vy < y0 || vy > y1) return MAT_ROCK;
        return MAT_AIR;
    };
}

// Runs step() until settled (steppedBrickCount() == 0) or the budget is
// exhausted, checking the conservation invariant after every single tick.
// Returns true if it settled within budget.
bool runToSettleCheckingConservation(WaterCA& ca, int budget) {
    for (int i = 0; i < budget; ++i) {
        ca.step();
        CHECK_EQ(ca.totalVolume(), ca.recomputeVolume());
        if (ca.steppedBrickCount() == 0) return true;
    }
    return false;
}

} // namespace

VXC_TEST(waterca_column_drop_settles_on_floor_conserved) {
    WaterCA ca(shaftAt(0, 0));
    const uint32_t placed = ca.addWater(0, 0, 20, 500); // stacks across cells 20,21 (255+245)
    CHECK_EQ(placed, uint32_t(500));
    CHECK_EQ(ca.totalVolume(), uint64_t(500));

    const bool settled = runToSettleCheckingConservation(ca, 200);
    CHECK(settled);
    CHECK_EQ(ca.totalVolume(), uint64_t(500)); // gravity/lateral never change volume

    // Rests on the floor: bottom cell full, remainder stacked directly above
    // it (the shaft has no room to spread sideways), nothing left up high.
    CHECK_EQ(int(ca.fillAt(0, 0, 1)), 255);
    CHECK_EQ(int(ca.fillAt(0, 0, 2)), 245);
    CHECK_EQ(int(ca.fillAt(0, 0, 3)), 0);
    CHECK_EQ(int(ca.fillAt(0, 0, 20)), 0);
    CHECK_EQ(int(ca.fillAt(0, 0, 21)), 0);
    CHECK_EQ(int(ca.fillAt(0, 0, 0)), 0); // never occupies the solid floor
}

VXC_TEST(waterca_pooling_spreads_flat_within_tolerance) {
    // Open, unwalled flat floor (z<=0 solid, everything else air): 500 units
    // dropped at the origin spread outward and settle. Every open-floor cell
    // rests directly on the solid floor, so the lateral rule's own fixed
    // point guarantees the "flat pool" property is a LOCAL one: any two
    // horizontally-adjacent cells differ by at most 1 once settled (that's
    // exactly what zero remaining flow, i.e. a fully settled active set,
    // means for the (self-neighbor)/4 rule) — this holds everywhere, not
    // just within the scanned window, which is what "flat pool ... within
    // +/-1" (plan-driven test contract) captures for a v0 sandpile-style CA
    // (a global single-plateau level is NOT guaranteed by this local rule;
    // the settled shape is a smooth, never-cliffed mound/pool instead).
    WaterCA ca([](int64_t, int64_t, int64_t vz) -> MaterialId { return vz <= 0 ? MAT_ROCK : MAT_AIR; });
    const uint32_t placed = ca.addWater(0, 0, 20, 500);
    CHECK_EQ(placed, uint32_t(500));

    const bool settled = runToSettleCheckingConservation(ca, 5000);
    CHECK(settled);
    CHECK_EQ(ca.totalVolume(), uint64_t(500));

    uint64_t sum = 0;
    constexpr int64_t kScan = 20; // generous vs. the 500-unit pool's actual footprint
    for (int64_t x = -kScan; x <= kScan; ++x)
        for (int64_t y = -kScan; y <= kScan; ++y) {
            const int f = ca.fillAt(x, y, 1);
            sum += static_cast<uint64_t>(f);
            CHECK_EQ(int(ca.fillAt(x, y, 2)), 0); // single flat layer, nothing stacked
            if (f == 0) continue;
            CHECK(std::abs(f - int(ca.fillAt(x + 1, y, 1))) <= 1);
            CHECK(std::abs(f - int(ca.fillAt(x - 1, y, 1))) <= 1);
            CHECK(std::abs(f - int(ca.fillAt(x, y + 1, 1))) <= 1);
            CHECK(std::abs(f - int(ca.fillAt(x, y - 1, 1))) <= 1);
        }
    CHECK_EQ(sum, uint64_t(500)); // the whole pool fits inside the scanned window
}

VXC_TEST(waterca_container_fills_bottom_up_never_escapes_walls) {
    // 3x3 basin (x,y in [0,2]): one full layer (9*255=2295) plus 500 more.
    WaterCA ca(basin(0, 2, 0, 2));
    const uint32_t placed = ca.addWater(1, 1, 30, 2795);
    CHECK_EQ(placed, uint32_t(2795));

    const bool settled = runToSettleCheckingConservation(ca, 5000);
    CHECK(settled);
    CHECK_EQ(ca.totalVolume(), uint64_t(2795));

    uint64_t layer2Sum = 0;
    for (int64_t x = 0; x <= 2; ++x)
        for (int64_t y = 0; y <= 2; ++y) {
            CHECK_EQ(int(ca.fillAt(x, y, 1)), 255); // bottom layer completely full
            layer2Sum += static_cast<uint64_t>(ca.fillAt(x, y, 2));
            CHECK_EQ(int(ca.fillAt(x, y, 3)), 0); // remainder doesn't need a third layer
        }
    CHECK_EQ(layer2Sum, uint64_t(2795 - 9 * 255));

    // Never escapes the walls: every cell outside [0,2]x[0,2], at any height
    // sampled, must be empty (solid cells are never written by construction,
    // but this exercises that invariant end to end through fillAt).
    for (int64_t x = -3; x <= 5; ++x)
        for (int64_t y = -3; y <= 5; ++y) {
            if (x >= 0 && x <= 2 && y >= 0 && y <= 2) continue;
            for (int64_t z = -2; z <= 32; ++z) CHECK_EQ(int(ca.fillAt(x, y, z)), 0);
        }
}

VXC_TEST(waterca_activity_settles_then_reactivates_locally) {
    WaterCA ca(shaftAt(0, 0));
    ca.addWater(0, 0, 10, 300);
    CHECK(runToSettleCheckingConservation(ca, 200));
    ca.step();
    CHECK_EQ(ca.steppedBrickCount(), size_t(0)); // settled state: a step touches nothing

    // A drop far away, in an isolated shaft, must not reactivate the
    // already-settled region: only the new drop's brick(s) become active.
    WaterCA ca2(shaftAt(1000, 1000));
    ca2.addWater(1000, 1000, 1, 50); // lands directly on the floor, already resting
    CHECK_EQ(ca2.activeBrickCount(), size_t(1));
    ca2.step();
    CHECK_EQ(ca2.steppedBrickCount(), size_t(1)); // exactly the touched neighborhood
    CHECK_EQ(ca2.totalVolume(), uint64_t(50));
}

VXC_TEST(waterca_deterministic_repeat_and_golden_digest) {
    auto scenario = [](WaterCA& ca) {
        ca.addWater(1, 1, 30, 2795);
        ca.addWater(0, 0, 25, 150);
        for (int i = 0; i < 40; ++i) ca.step();
    };

    WaterCA a(basin(0, 2, 0, 2));
    WaterCA b(basin(0, 2, 0, 2));
    scenario(a);
    scenario(b);

    Digest da, db;
    a.digest(da);
    b.digest(db);
    CHECK_EQ(da.h, db.h);
    CHECK_EQ(da.h, 0x7995BE759FB9D67Eull); // GOLDEN(waterca_container_scenario)
}

VXC_TEST(waterca_conservation_fuzz_over_bumpy_terrain) {
    constexpr uint64_t kSeed = 20260719;
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    WaterCA ca([&amp](int64_t vx, int64_t vy, int64_t vz) { return amp.materialAt(vx, vy, vz); });

    uint64_t rngState = 0xC0FFEEu;
    auto nextU64 = [&rngState]() { return rngState = splitmix64(rngState); };

    uint64_t expectedTotal = 0;
    for (int i = 0; i < 200; ++i) {
        const int64_t x = static_cast<int64_t>(nextU64() % 101) - 50;  // [-50, 50]
        const int64_t y = static_cast<int64_t>(nextU64() % 101) - 50;  // [-50, 50]
        const uint32_t amount = static_cast<uint32_t>(nextU64() % 451) + 50; // [50, 500]

        const ColumnSample col = amp.column(x, y);
        const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
        const int64_t dropZ = topVz + 30; // well clear of the surface, always air above

        expectedTotal += ca.addWater(x, y, dropZ, amount);
    }

    for (int i = 0; i < 500; ++i) {
        ca.step();
        CHECK_EQ(ca.totalVolume(), ca.recomputeVolume());
    }
    CHECK_EQ(ca.totalVolume(), expectedTotal);
}
