// Water pressure CA v0 (plan §3.7 Layer B, W2 groundwork): gravity + lateral
// equalization, volume conservation, activity/settling, determinism.

#include "voxelcore/waterca.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

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

// A classic U-bend / drain trap (Phase C target scenario, waterca.h "Phase
// C -- HYDROSTATIC"): two single-cell-wide vertical arms at (armX0,0) and
// (armX1,0), joined by a single-cell-wide horizontal connector along y==0,
// z==1, for x in [armX0, armX1] -- both arm bases are themselves part of
// that same connector row, so the whole z==1 layer (both bases + the
// channel between) is one contiguous open strip. Everything else is solid.
// Floor at z<=0. Gravity/lateral alone can fill the connector and stack
// water up EITHER arm independently, but can never push water up the FAR
// arm just because the NEAR arm has more in it -- that's exactly the gap
// Phase C closes.
WaterCA::SolidFn uBend(int64_t armX0, int64_t armX1) {
    return [armX0, armX1](int64_t vx, int64_t vy, int64_t vz) -> MaterialId {
        if (vz <= 0) return MAT_ROCK;
        if (vy != 0) return MAT_ROCK;
        if (vz == 1) return (vx >= armX0 && vx <= armX1) ? MAT_AIR : MAT_ROCK; // connector + both bases
        if (vx == armX0 || vx == armX1) return MAT_AIR;                       // the two vertical arms
        return MAT_ROCK;
    };
}

// Two communicating vessels: wider (2x2 footprint) tanks at
// x in [tankAx0,tankAx0+1] and x in [tankBx0,tankBx0+1] (both y in [0,1]),
// joined by a lower single-layer (z==1) channel spanning connX0..connX1 at
// the same y range. Wider than uBend's 1-cell arms specifically to exercise
// the level computation's per-layer CROSS-SECTION handling (a real
// container, not just a single column per side) -- see waterca.h "Phase C"
// step 2.
WaterCA::SolidFn communicatingVessels(int64_t tankAx0, int64_t connX0, int64_t connX1, int64_t tankBx0) {
    return [tankAx0, connX0, connX1, tankBx0](int64_t vx, int64_t vy, int64_t vz) -> MaterialId {
        if (vz <= 0) return MAT_ROCK;
        if (vy != 0 && vy != 1) return MAT_ROCK;
        const bool inTankA = vx >= tankAx0 && vx <= tankAx0 + 1;
        const bool inTankB = vx >= tankBx0 && vx <= tankBx0 + 1;
        if (vz == 1 && vx >= connX0 && vx <= connX1) return MAT_AIR; // connector layer only
        if (inTankA || inTankB) return MAT_AIR;                     // both tanks, full height
        return MAT_ROCK;
    };
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
    // GOLDEN(waterca_container_scenario), v3 hydrostatic contract
    // (kWaterCAVersion==3). Re-pinned from the v2 two-phase-only value
    // (0x5C8D36C83246CAFC) -- deliberate per Phase C landing (waterca.h
    // "Phase C -- HYDROSTATIC" comment, kWaterCAVersion doc comment): same
    // scenario, Phase READ/APPLY itself is byte-for-byte unchanged, but
    // Phase C now runs and equalizes the basin's own level every tick
    // instead of being a no-op, so a different digest is expected, not a
    // regression. (This scenario is a single walled basin, no U-bend/
    // multi-arm shape, so Phase C's only effect here is to make the
    // already-close-to-level basin surface exactly level a little faster
    // than Phase READ/APPLY's own lateral diffusion alone would.)
    CHECK_EQ(da.h, 0x3D2224BE4A253404ull);
}

// Conservation-under-contention proof (two-phase v1 contract, Phase
// READ/APPLY): a walled "cross" -- origin plus its 4 lateral neighbors open
// at z=1, everything else at z=1 solid (so each arm's ONLY non-solid
// neighbor is the origin; no unrelated spreading to confuse the arithmetic)
// -- with origin near-full (253/255, budget 2) and all 4 arms completely
// full (255). Every arm wants to send flow toward origin (diff=2 each, i.e.
// flow=1 each per the lateral rule): a naive unconditional apply would try
// to stuff 4 units into a 2-unit budget (257 > 255, an illegal uint8_t
// overflow). The two-phase design's fixed processing order (colorOf's
// 8-way round order -- see waterca.h "Tick rules v1") resolves this
// deterministically: origin and the 4 arms fall into 3 different colors
// (origin alone, east+west together, north+south together), so by the time
// origin's capacity is actually consumed (in the earlier-processed
// east/west round), the later-processed north/south round finds zero
// budget left and correctly contributes nothing to Phase READ/APPLY.
//
// BUT this scenario's cross is now, after that redistribution, a single
// fully water-connected flat (all z=1) body -- exactly the shape Phase C
// (hydrostatic, kWaterCAVersion==3) exists to perfect: it runs immediately
// after Phase READ/APPLY within the SAME step() call and re-equalizes the
// whole connected cross to its true integer level (total 1273 over 5
// cells: base 254 + 3 remainder units, see waterca.h "Phase C" step 2), so
// the FINAL observable state below reflects both phases, not Phase
// READ/APPLY alone -- there is no public hook to observe the intermediate
// post-READ/APPLY-pre-hydrostatic state, and this test doesn't need one:
// the contention-resolution mechanism above is still exactly what runs
// and still deterministic, it's just no longer the LAST thing to touch
// these 5 cells this tick. What still matters, and is still checked here:
// the cap never overflows mid-tick, and total volume is exactly conserved
// end to end despite two different redistribution mechanisms both touching
// the same 5 cells in the same tick.
VXC_TEST(waterca_lateral_contention_capped_conserved_fixed_order) {
    auto solid = [](int64_t vx, int64_t vy, int64_t vz) -> MaterialId {
        if (vz <= 0) return MAT_ROCK; // floor
        if (vz != 1) return MAT_AIR;  // nothing above the cross layer matters
        const bool isArm = (vx == 0 && vy == 0) || (vx == 1 && vy == 0) || (vx == -1 && vy == 0) ||
                           (vx == 0 && vy == 1) || (vx == 0 && vy == -1);
        return isArm ? MAT_AIR : MAT_ROCK; // everything off the cross is a wall
    };
    WaterCA ca(solid);
    ca.addWater(0, 0, 1, 253);  // origin: budget 2 once at 253/255
    ca.addWater(1, 0, 1, 255);  // east: full
    ca.addWater(-1, 0, 1, 255); // west: full
    ca.addWater(0, 1, 1, 255);  // north: full
    ca.addWater(0, -1, 1, 255); // south: full
    CHECK_EQ(ca.totalVolume(), uint64_t(253 + 255 * 4));

    ca.step();

    // Post-hydrostatic exact level: total 1273 over the 5 connected cells
    // = base 254 each + 3 remainder units, awarded by the fixed (x,y)
    // ascending tie-break (waterca.h "Phase C" step 2) to the first 3 cells
    // in that order: (-1,0), (0,-1), (0,0) -- west, south, origin get 255;
    // (0,1), (1,0) -- north, east get 254. Never 257 anywhere -- the
    // Phase READ/APPLY cap held before hydrostatic ever ran.
    CHECK_EQ(int(ca.fillAt(0, 0, 1)), 255);  // origin
    CHECK_EQ(int(ca.fillAt(-1, 0, 1)), 255); // west
    CHECK_EQ(int(ca.fillAt(0, -1, 1)), 255); // south
    CHECK_EQ(int(ca.fillAt(1, 0, 1)), 254);  // east
    CHECK_EQ(int(ca.fillAt(0, 1, 1)), 254);  // north

    // Exact conservation despite the contention AND the subsequent
    // hydrostatic re-equalization: nothing created, nothing silently
    // dropped on the floor when a target ran out of room.
    CHECK_EQ(ca.totalVolume(), uint64_t(253 + 255 * 4));
    CHECK_EQ(ca.recomputeVolume(), ca.totalVolume());
}

// Order-independence proof: this is the property that makes a future
// GPU/parallel port valid (waterca.h "Tick rules v1" -- stepWithOrder is a
// pure function of the ACTIVE SET's CONTENTS, never the order it's listed
// in). Two identically-built WaterCA instances run the SAME scenario for
// many ticks; one always feeds step() (which passes activeSetSnapshot(), a
// BrickKeyLess-sorted vector) while the other explicitly REVERSES its own
// active-set snapshot every single tick before calling stepWithOrder with
// it -- a deliberately different (and itself varying, since a reversed
// active set is never the same permutation as the previous tick's) order
// every time. Byte-identical results throughout (not just at the end)
// proves both determinism (repeatable) AND order-independence in one test.
VXC_TEST(waterca_twophase_order_independent_and_deterministic) {
    auto scenario_sorted = [](WaterCA& ca) {
        ca.addWater(1, 1, 30, 2795);
        ca.addWater(0, 0, 25, 150);
    };

    WaterCA sorted(basin(0, 2, 0, 2));
    WaterCA shuffled(basin(0, 2, 0, 2));
    scenario_sorted(sorted);
    scenario_sorted(shuffled);

    for (int i = 0; i < 60; ++i) {
        sorted.step(); // sorted-order snapshot internally

        std::vector<BrickKey> order = shuffled.activeSetSnapshot();
        std::reverse(order.begin(), order.end()); // a different permutation than step() would use
        shuffled.stepWithOrder(order);

        CHECK_EQ(sorted.totalVolume(), shuffled.totalVolume());
        CHECK_EQ(sorted.activeBrickCount(), shuffled.activeBrickCount());
        Digest ds, dh;
        sorted.digest(ds);
        shuffled.digest(dh);
        CHECK_EQ(ds.h, dh.h); // byte-identical state every single tick, not just at the end
    }
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

// Phase C (hydrostatic, kWaterCAVersion==3) headline scenario: pour water
// into ONLY the left arm of a solid-walled U-bend (uBend fixture) and check
// it rises in the RIGHT arm too, settling with both arms at the same
// height -- the thing gravity+lateral alone structurally cannot do (see
// waterca.h "Phase C -- HYDROSTATIC" intro: gravity never flows up, lateral
// only trades flow between same-z neighbors). Volume is chosen to divide
// EXACTLY across the connector-layer's 5 cells (both arm bases + the 3
// cells between, x in [0,4] at z==1: 5*255=1275) plus 3 more full layers
// split evenly across the 2 arms (2*255*3=1530), total 2805 -- an exact
// integer equilibrium with no remainder, so both arms must land at
// PRECISELY the same fill, not just within the usual +/-1 tolerance.
VXC_TEST(waterca_hydrostatic_u_bend_equalizes_and_settles) {
    WaterCA ca(uBend(0, 4));
    const uint32_t placed = ca.addWater(0, 0, 40, 2805); // poured only into the LEFT arm (x=0), high up
    CHECK_EQ(placed, uint32_t(2805));

    const bool settled = runToSettleCheckingConservation(ca, 2000);
    CHECK(settled);
    CHECK_EQ(ca.totalVolume(), uint64_t(2805));

    // Both arms rose to EXACTLY the same height: z=1..4 full (255) in each,
    // nothing above z=4, nothing was poured into the right arm directly --
    // it only got there via Phase C equalizing through the connector.
    for (int64_t z = 1; z <= 4; ++z) {
        CHECK_EQ(int(ca.fillAt(0, 0, z)), 255); // left arm (pour side)
        CHECK_EQ(int(ca.fillAt(4, 0, z)), 255); // right arm (equalized side)
    }
    CHECK_EQ(int(ca.fillAt(0, 0, 5)), 0);
    CHECK_EQ(int(ca.fillAt(4, 0, 5)), 0);
    // Connector itself (the 3 cells strictly between the two arm bases)
    // full too -- it's part of the same z==1 layer.
    CHECK_EQ(int(ca.fillAt(2, 0, 1)), 255);

    // Exact conservation through both the two-phase rounds and Phase C.
    CHECK_EQ(ca.recomputeVolume(), ca.totalVolume());
}

// Communicating vessels: two wider (2x2 footprint) tanks joined by a lower
// channel (communicatingVessels fixture) -- exercises Phase C's per-layer
// CROSS-SECTION accounting (waterca.h "Phase C" step 2) rather than uBend's
// single-column arms. Connector spans x in [2,9] -- contiguous from tank
// A's right edge (x=1) through to tank B's left edge (x=10), no gap. Volume
// chosen to land exactly: connector layer (tankA 4 cells + connector 16
// cells + tankB 4 cells = 24*255=6120) plus 2 more full layers split across
// just the two tanks (8*255*2=4080), total 10200 -- both tanks must settle
// at EXACTLY the same per-cell fill.
VXC_TEST(waterca_hydrostatic_communicating_vessels_equalize) {
    WaterCA ca(communicatingVessels(0, 2, 9, 10));
    const uint32_t placed = ca.addWater(0, 0, 40, 10200); // poured only into tank A
    CHECK_EQ(placed, uint32_t(10200));

    const bool settled = runToSettleCheckingConservation(ca, 2000);
    CHECK(settled);
    CHECK_EQ(ca.totalVolume(), uint64_t(10200));

    for (int64_t z = 1; z <= 3; ++z) {
        for (int64_t y = 0; y <= 1; ++y) {
            CHECK_EQ(int(ca.fillAt(0, y, z)), 255);  // tank A
            CHECK_EQ(int(ca.fillAt(1, y, z)), 255);  // tank A
            CHECK_EQ(int(ca.fillAt(10, y, z)), 255); // tank B (equalized, nothing poured here)
            CHECK_EQ(int(ca.fillAt(11, y, z)), 255); // tank B
        }
    }
    CHECK_EQ(int(ca.fillAt(0, 0, 4)), 0);
    CHECK_EQ(int(ca.fillAt(10, 0, 4)), 0);
    CHECK_EQ(ca.recomputeVolume(), ca.totalVolume());
}

// Order-independence proof extended to Phase C (waterca.h "Phase C --
// HYDROSTATIC", "ORDER INDEPENDENCE" paragraph): the SAME reversed-order
// technique as waterca_twophase_order_independent_and_deterministic, but
// over the U-bend scenario specifically so a run of this test actually
// exercises Phase C doing real cross-arm redistribution (not just a
// single-layer basin settling toward flat) every tick along the way, not
// only Phase READ/APPLY.
VXC_TEST(waterca_hydrostatic_order_independent_and_deterministic) {
    auto pour = [](WaterCA& ca) { ca.addWater(0, 0, 40, 2805); };

    WaterCA sorted(uBend(0, 4));
    WaterCA shuffled(uBend(0, 4));
    pour(sorted);
    pour(shuffled);

    for (int i = 0; i < 200; ++i) {
        sorted.step(); // sorted-order snapshot internally

        std::vector<BrickKey> order = shuffled.activeSetSnapshot();
        std::reverse(order.begin(), order.end()); // a different permutation than step() would use
        shuffled.stepWithOrder(order);

        CHECK_EQ(sorted.totalVolume(), shuffled.totalVolume());
        CHECK_EQ(sorted.activeBrickCount(), shuffled.activeBrickCount());
        Digest ds, dh;
        sorted.digest(ds);
        shuffled.digest(dh);
        CHECK_EQ(ds.h, dh.h); // byte-identical state every single tick, not just at the end
    }
}
