// Water pressure CA v0 (plan §3.7 Layer B, W2 groundwork): gravity + lateral
// equalization, volume conservation, activity/settling, determinism.

#include "voxelcore/waterca.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
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

// A 4x4 open basin (x,y in [0,3]) with its floor at z<=0 and walls outside,
// plus a 1x1 drain shaft at (0,0) punched straight through that floor down to
// bedrock at z<=-10 -- but ONLY once *holeOpen becomes true. That flip is
// exactly a player digging underneath a settled pond: the terrain query's
// answers change at runtime, with no water added or removed. Used by the
// wakeRegion tests below.
WaterCA::SolidFn basinWithDrain(std::shared_ptr<bool> holeOpen) {
    return [holeOpen](int64_t vx, int64_t vy, int64_t vz) -> MaterialId {
        if (vz <= -10) return MAT_ROCK; // bedrock under the shaft
        if (*holeOpen && vx == 0 && vy == 0 && vz <= 0) return MAT_AIR; // the dug shaft
        if (vz <= 0) return MAT_ROCK;                                   // basin floor
        if (vx < 0 || vx > 3 || vy < 0 || vy > 3) return MAT_ROCK;      // basin walls
        return MAT_AIR;
    };
}

// A 4x4 walled chamber (x,y in [0,3]) whose EAST wall is carved away once
// *breached becomes true, opening a channel + second chamber out to x=11. The
// carve/breach counterpart of basinWithDrain: it proves water reacts to a
// SIDEWAYS opening, several bricks wide, not just to a hole underneath.
WaterCA::SolidFn basinWithBreach(std::shared_ptr<bool> breached) {
    return [breached](int64_t vx, int64_t vy, int64_t vz) -> MaterialId {
        if (vz <= 0) return MAT_ROCK;                              // floor everywhere
        if (vy < 0 || vy > 3) return MAT_ROCK;                     // north/south walls
        if (vx >= 0 && vx <= 3) return MAT_AIR;                    // the original chamber
        if (*breached && vx >= 4 && vx <= 11) return MAT_AIR;      // carved channel + chamber
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

// Phase C flood over a LARGE, multi-brick connected pool -- the regression
// guard for the W2 hydrostatic perf rewrite (docs/status.md, 2026-07-20:
// per-brick flood cache + deferred writes + no-op-write skip + raw solid_ for
// the once-per-voxel air queries). That rewrite is a pure ACCESS-COST change
// -- component partition, totalVol, bottom-up leveling and the overflow cap
// are all supposed to be byte-for-byte identical to the old from-scratch
// flood -- but the other hydrostatic tests above only exercise tiny
// (single-column / 2x2) components that never cross a brick boundary in the
// flood or stress the air-shell exploration. This one pours into a wide
// 16x16 basin (footprint spans a 2x2 block of 8^3 bricks; the water column
// plus the air headroom above it span several bricks in z too), so the flood
// genuinely crosses brick faces in every direction, resolves the same brick
// many times (exercising the cache), and explores a real air shell above the
// surface -- then pins the settled digest so any future change that perturbs
// the flood's cell set, ordering-into-leveling, or write set is caught.
// (Verified byte-identical against the pre-rewrite implementation at this and
// larger scales, incl. the 441-column overflow bench, before pinning.)
VXC_TEST(waterca_hydrostatic_large_pool_multibrick_golden) {
    WaterCA ca(basin(0, 15, 0, 15)); // 16x16 footprint = 2x2 bricks in x,y
    // 4 full layers (4*256*255 = 261120) plus a partial 5th, chosen with a
    // remainder so the top layer's fixed (x,y) tie-break distribution is
    // exercised, not just clean full layers.
    const uint32_t placed = ca.addWater(7, 7, 30, 300000);
    CHECK_EQ(placed, uint32_t(300000));

    const bool settled = runToSettleCheckingConservation(ca, 5000);
    CHECK(settled);
    CHECK_EQ(ca.totalVolume(), uint64_t(300000));

    // Bottom-up fill: layers z=1..4 completely full (each 256*255=65280),
    // consuming 261120; the remaining 38880 sits in z=5, nothing at z>=6.
    for (int64_t x = 0; x <= 15; ++x)
        for (int64_t y = 0; y <= 15; ++y) {
            CHECK_EQ(int(ca.fillAt(x, y, 1)), 255);
            CHECK_EQ(int(ca.fillAt(x, y, 4)), 255);
            CHECK_EQ(int(ca.fillAt(x, y, 6)), 0);
        }
    CHECK_EQ(ca.recomputeVolume(), ca.totalVolume());

    // GOLDEN(waterca_large_pool_multibrick), v3 hydrostatic contract
    // (kWaterCAVersion==3). Locks the multi-brick flood + leveling output.
    Digest d;
    ca.digest(d);
    CHECK_EQ(d.h, 0x56BC18914355A205ull);
}

// ---------------------------------------------------------------------------
// Cross-tick terrain-solidity memo (waterca.h setSolidCacheEnabled)
// ---------------------------------------------------------------------------
// The memo is an OPT-IN cache of the caller's `solid_` query that survives
// across ticks. Memoizing a pure function cannot change its answers, so with
// the memo on the tick output must be byte-for-byte what the uncached path
// produces -- these tests are the in-suite proof of exactly that, at the two
// pinned goldens, tick-by-tick over a Phase-C-heavy scenario, under the
// order-independence property, and (the one case where the memo is NOT
// vacuously safe) across a live terrain edit with the invalidation hook.

// A basin split by a removable divider wall at x==divX. `wallUp` is read
// through a pointer so a test can DIG THE WALL AWAY between ticks -- the
// runtime-editable-terrain case that the memo's invalidation contract exists
// for (the live UE water subsystem's SolidFn is overlay-aware in exactly this
// way; see docs/adr/0003-hydrostatic-persistent-body.md).
namespace {
WaterCA::SolidFn dividedBasin(const bool* wallUp, int64_t divX) {
    return [wallUp, divX](int64_t vx, int64_t vy, int64_t vz) -> MaterialId {
        if (vz <= 0) return MAT_ROCK;
        if (vx < 0 || vx > 11 || vy < 0 || vy > 3) return MAT_ROCK;
        if (*wallUp && vx == divX) return MAT_ROCK;
        return MAT_AIR;
    };
}
} // namespace

// Both pinned goldens, re-derived with the memo ENABLED. If the memo ever
// perturbed the flood's cell set, leveling, or write set, these constants --
// the same ones waterca_deterministic_repeat_and_golden_digest and
// waterca_hydrostatic_large_pool_multibrick_golden pin on the uncached path --
// would move. They must not.
VXC_TEST(waterca_solid_cache_golden_digests_unchanged) {
    WaterCA a(basin(0, 2, 0, 2));
    a.setSolidCacheEnabled(true);
    CHECK(a.solidCacheEnabled());
    a.addWater(1, 1, 30, 2795);
    a.addWater(0, 0, 25, 150);
    for (int i = 0; i < 40; ++i) a.step();
    Digest da;
    a.digest(da);
    CHECK_EQ(da.h, 0x3D2224BE4A253404ull); // GOLDEN(waterca_container_scenario), memo on
    CHECK(a.solidCacheBrickCount() > 0);   // the memo really was populated

    WaterCA b(basin(0, 15, 0, 15));
    b.setSolidCacheEnabled(true);
    CHECK_EQ(b.addWater(7, 7, 30, 300000), uint32_t(300000));
    CHECK(runToSettleCheckingConservation(b, 5000));
    CHECK_EQ(b.totalVolume(), uint64_t(300000));
    Digest db;
    b.digest(db);
    CHECK_EQ(db.h, 0x56BC18914355A205ull); // GOLDEN(waterca_large_pool_multibrick), memo on
}

// Tick-by-tick equality (not just at settling) between a memoized and an
// unmemoized CA, over the U-bend -- so every single tick genuinely runs Phase
// C cross-arm redistribution while the memo is being built up and hit.
VXC_TEST(waterca_solid_cache_tick_by_tick_identical_to_uncached) {
    WaterCA plain(uBend(0, 4));
    WaterCA memo(uBend(0, 4));
    memo.setSolidCacheEnabled(true);
    plain.addWater(0, 0, 40, 2805);
    memo.addWater(0, 0, 40, 2805);

    for (int i = 0; i < 200; ++i) {
        plain.step();
        memo.step();
        CHECK_EQ(plain.totalVolume(), memo.totalVolume());
        CHECK_EQ(plain.activeBrickCount(), memo.activeBrickCount());
        Digest dp, dm;
        plain.digest(dp);
        memo.digest(dm);
        CHECK_EQ(dp.h, dm.h); // byte-identical EVERY tick, not just at the end
    }
}

// The memo must not disturb order-independence either: same reversed-active-set
// replay as waterca_hydrostatic_order_independent_and_deterministic, memo on.
VXC_TEST(waterca_solid_cache_order_independent_and_deterministic) {
    WaterCA sorted(uBend(0, 4));
    WaterCA shuffled(uBend(0, 4));
    sorted.setSolidCacheEnabled(true);
    shuffled.setSolidCacheEnabled(true);
    sorted.addWater(0, 0, 40, 2805);
    shuffled.addWater(0, 0, 40, 2805);

    for (int i = 0; i < 200; ++i) {
        sorted.step();
        std::vector<BrickKey> order = shuffled.activeSetSnapshot();
        std::reverse(order.begin(), order.end());
        shuffled.stepWithOrder(order);
        Digest ds, dh;
        sorted.digest(ds);
        shuffled.digest(dh);
        CHECK_EQ(ds.h, dh.h);
    }
}

// The case the memo is NOT vacuously safe for: terrain edited under settled
// water. A memoized CA that honours the invalidation contract must track an
// unmemoized CA byte-for-byte across the edit; this is the executable spec for
// what a caller owes WaterCA before it may enable the memo.
VXC_TEST(waterca_solid_cache_invalidation_tracks_terrain_edit) {
    bool wallUpPlain = true, wallUpMemo = true;
    WaterCA plain(dividedBasin(&wallUpPlain, 5));
    WaterCA memo(dividedBasin(&wallUpMemo, 5));
    memo.setSolidCacheEnabled(true);

    // Pour into the LEFT half only; let it settle against the divider.
    plain.addWater(2, 2, 30, 20000);
    memo.addWater(2, 2, 30, 20000);
    for (int i = 0; i < 60; ++i) {
        plain.step();
        memo.step();
    }
    Digest d0p, d0m;
    plain.digest(d0p);
    memo.digest(d0m);
    CHECK_EQ(d0p.h, d0m.h);
    CHECK_EQ(int(plain.fillAt(8, 2, 1)), 0); // right half still dry: wall held
    CHECK(memo.solidCacheBrickCount() > 0);  // and the divider IS memoized as solid

    // Dig the divider out. The memoized CA is told; the water must now spread
    // right in BOTH instances, identically, every tick.
    wallUpPlain = false;
    wallUpMemo = false;
    memo.invalidateSolidRegion(5, 0, -8, 5, 3, 64);
    // Terrain edits do not by themselves wake bricks (that is the caller's
    // existing activation duty, unchanged by this memo) -- nudge both the same
    // way so the comparison isolates the memo, not activation.
    plain.addWater(2, 2, 30, 255);
    memo.addWater(2, 2, 30, 255);

    for (int i = 0; i < 400; ++i) {
        plain.step();
        memo.step();
        CHECK_EQ(plain.totalVolume(), memo.totalVolume());
        Digest dp, dm;
        plain.digest(dp);
        memo.digest(dm);
        CHECK_EQ(dp.h, dm.h); // identical across and after the edit
    }
    CHECK(plain.fillAt(8, 2, 1) > 0); // water really did cross the removed wall
    CHECK_EQ(plain.recomputeVolume(), plain.totalVolume());
    CHECK_EQ(memo.recomputeVolume(), memo.totalVolume());
}

// Disabling the memo drops it entirely (never keep a cache we have stopped
// maintaining), and a full invalidateSolidCache() is always safe: both leave
// the CA on the plain path with identical results.
VXC_TEST(waterca_solid_cache_disable_and_full_invalidate_are_safe) {
    WaterCA plain(basin(0, 7, 0, 7));
    WaterCA memo(basin(0, 7, 0, 7));
    memo.setSolidCacheEnabled(true);
    plain.addWater(3, 3, 30, 60000);
    memo.addWater(3, 3, 30, 60000);

    for (int i = 0; i < 120; ++i) {
        plain.step();
        memo.step();
        if (i == 30) {
            CHECK(memo.solidCacheBrickCount() > 0);
            memo.invalidateSolidCache(); // full drop mid-run: only costs re-queries
            CHECK_EQ(memo.solidCacheBrickCount(), size_t(0));
        }
        if (i == 60) {
            memo.setSolidCacheEnabled(false); // turning it off clears it too
            CHECK(!memo.solidCacheEnabled());
            CHECK_EQ(memo.solidCacheBrickCount(), size_t(0));
        }
        if (i == 90) memo.setSolidCacheEnabled(true); // and back on, from cold
        Digest dp, dm;
        plain.digest(dp);
        memo.digest(dm);
        CHECK_EQ(dp.h, dm.h);
    }
    CHECK_EQ(plain.totalVolume(), memo.totalVolume());
}

// --- Terrain-edit reactivation (wakeRegion, waterca.h "Terrain-edit
// reactivation") -------------------------------------------------------------

// THE BUG, reproduced: a settled pond is frozen against a terrain edit until
// something wakes it. The first half of this test is the regression witness
// (the old, broken behavior); the second half is the fix.
VXC_TEST(waterca_wake_region_drains_settled_pond_into_new_hole) {
    auto holeOpen = std::make_shared<bool>(false);
    WaterCA ca(basinWithDrain(holeOpen));
    ca.addWater(1, 1, 6, 2000);
    CHECK(runToSettleCheckingConservation(ca, 300));
    CHECK_EQ(ca.totalVolume(), uint64_t(2000));
    CHECK_EQ(int(ca.fillAt(1, 1, 1)), 125); // 2000 spread flat over the 16-cell floor
    CHECK_EQ(ca.activeBrickCount(), size_t(0));

    Digest settled;
    ca.digest(settled);

    // Dig the shaft out from under the pond. WITHOUT a wake call this is a
    // complete no-op forever -- the exact gameplay bug (docs/status.md W2).
    *holeOpen = true;
    for (int i = 0; i < 20; ++i) {
        ca.step();
        CHECK_EQ(ca.steppedBrickCount(), size_t(0));
    }
    Digest frozen;
    ca.digest(frozen);
    CHECK_EQ(frozen.h, settled.h);
    CHECK_EQ(int(ca.fillAt(1, 1, 1)), 125); // still sitting there, untouched

    // Now wake the edited region. The dug voxels were ALL terrain and hold no
    // water at all -- the water that must react lives in a different brick,
    // which is precisely what the halo rule is for.
    const size_t woken = ca.wakeRegion(0, 0, -9, 0, 0, 0);
    CHECK(woken >= size_t(1));
    CHECK_EQ(ca.totalVolume(), uint64_t(2000)); // waking creates/destroys nothing
    Digest afterWake;
    ca.digest(afterWake);
    CHECK_EQ(afterWake.h, settled.h); // ...and writes no fill either

    CHECK(runToSettleCheckingConservation(ca, 500));
    CHECK_EQ(ca.totalVolume(), uint64_t(2000)); // exact conservation through the drain

    Digest drained;
    ca.digest(drained);
    CHECK(drained.h != settled.h); // the whole point: the state actually moved

    // 2000 units in the 9-cell shaft (z=-9..-1): fills bottom-up, 7 full cells
    // (1785) + 215 in the 8th, leaving the basin floor above it dry.
    CHECK_EQ(int(ca.fillAt(0, 0, -9)), 255);
    CHECK_EQ(int(ca.fillAt(0, 0, -3)), 255);
    CHECK_EQ(int(ca.fillAt(0, 0, -2)), 215);
    CHECK_EQ(int(ca.fillAt(0, 0, -1)), 0);
    CHECK_EQ(int(ca.fillAt(1, 1, 1)), 0); // pond fully drained
    CHECK_EQ(ca.recomputeVolume(), uint64_t(2000));
}

// Waking is scheduling only: no fill written, ledger untouched, and an edit
// nowhere near any water wakes nothing at all (so a dig in the desert can
// never resurrect a distant lake's per-tick cost).
VXC_TEST(waterca_wake_region_writes_nothing_and_ignores_dry_regions) {
    WaterCA ca(basin(0, 3, 0, 3));
    ca.addWater(1, 1, 6, 2000);
    CHECK(runToSettleCheckingConservation(ca, 300));
    CHECK_EQ(ca.activeBrickCount(), size_t(0));

    Digest before;
    ca.digest(before);
    const uint64_t vol = ca.totalVolume();

    // Far away (many bricks off in every direction): nothing to wake.
    CHECK_EQ(ca.wakeRegion(4000, 4000, 4000, 4001, 4001, 4001), size_t(0));
    CHECK_EQ(ca.activeBrickCount(), size_t(0));

    // Degenerate/inverted boxes are rejected rather than misinterpreted.
    CHECK_EQ(ca.wakeRegion(5, 5, 5, 4, 6, 6), size_t(0));

    // On the pond itself: wakes, but changes no stored state whatsoever.
    CHECK(ca.wakeRegion(1, 1, 1, 1, 1, 1) >= size_t(1));
    Digest after;
    ca.digest(after);
    CHECK_EQ(after.h, before.h);
    CHECK_EQ(ca.totalVolume(), vol);
    CHECK_EQ(ca.recomputeVolume(), vol);

    // Re-waking an already-active brick is idempotent (0 newly woken).
    const size_t n = ca.activeBrickCount();
    CHECK_EQ(ca.wakeRegion(1, 1, 1, 1, 1, 1), size_t(0));
    CHECK_EQ(ca.activeBrickCount(), n);
}

// Order/strategy independence (waterca.h "DETERMINISM"): the woken set is a
// pure function of (region, WaterMap contents) -- independent of the order the
// water was inserted in (i.e. of unordered_map bucket order) AND of which of
// wakeRegion's two enumeration strategies it picks for a given region.
VXC_TEST(waterca_wake_region_order_and_strategy_independent) {
    // Same body, built in opposite insertion orders -> identical contents,
    // different internal hash-table layout.
    auto build = [](WaterCA& ca, bool reversed) {
        for (int i = 0; i < 4; ++i) {
            const int64_t x = reversed ? (3 - i) : i;
            for (int64_t y = 0; y < 4; ++y) ca.addWater(x, y, 6, 400);
        }
    };
    WaterCA a(basin(0, 3, 0, 3)), b(basin(0, 3, 0, 3));
    build(a, false);
    build(b, true);
    CHECK(runToSettleCheckingConservation(a, 300));
    CHECK(runToSettleCheckingConservation(b, 300));
    Digest ds0, ds1;
    a.digest(ds0);
    b.digest(ds1);
    CHECK_EQ(ds0.h, ds1.h);

    // A region far larger than the stored body forces wakeRegion's
    // walk-the-stored-bricks branch (over an unordered_map) on both.
    const size_t wa = a.wakeRegion(-500, -500, -500, 500, 500, 500);
    const size_t wb = b.wakeRegion(-500, -500, -500, 500, 500, 500);
    CHECK_EQ(wa, wb);
    CHECK_EQ(a.activeBricks().size(), b.activeBricks().size());
    for (const BrickKey& k : a.activeBricks()) CHECK(b.activeBricks().count(k) == size_t(1));

    // ...and the resulting tick is itself order-independent, exactly the way
    // the existing two-phase property tests check it.
    std::vector<BrickKey> fwd = a.activeSetSnapshot();
    std::vector<BrickKey> rev = fwd;
    std::reverse(rev.begin(), rev.end());
    a.stepWithOrder(fwd);
    b.stepWithOrder(rev);
    Digest da, db;
    a.digest(da);
    b.digest(db);
    CHECK_EQ(da.h, db.h);
    CHECK_EQ(a.totalVolume(), b.totalVolume());

    // STRATEGY CROSS-CHECK: the SAME region against the SAME nearby body must
    // wake the same bricks whether wakeRegion walks the region (chosen when
    // the region spans fewer bricks than the map stores) or walks the map.
    // `large` carries extra, unrelated water far away purely to push the
    // stored brick count past the region's span and flip the branch.
    WaterCA small(basin(0, 3, 0, 3));
    small.addWater(1, 1, 3, 600);
    CHECK(runToSettleCheckingConservation(small, 200));
    WaterCA large(basin(0, 3, 0, 3));
    large.addWater(1, 1, 3, 600);
    CHECK(runToSettleCheckingConservation(large, 200));
    for (int64_t i = 0; i < 64; ++i) large.addWater(2, 2, 1 + 8 * (i + 20), 100); // far column, own bricks
    CHECK(large.storedBrickCount() > small.storedBrickCount());

    const auto beforeLarge = large.activeBricks();
    const size_t nSmall = small.wakeRegion(0, 0, 0, 3, 3, 3);
    large.wakeRegion(0, 0, 0, 3, 3, 3);
    size_t nLarge = 0;
    for (const BrickKey& k : large.activeBricks())
        if (beforeLarge.find(k) == beforeLarge.end()) ++nLarge;
    CHECK_EQ(nSmall, nLarge);
    for (const BrickKey& k : small.activeBricks()) CHECK(large.activeBricks().count(k) == size_t(1));
}

// The carve/breach counterpart of the drain test: a settled pool behind a wall
// that is carved away sideways must flow out and re-level across the newly
// opened volume -- and must sit there frozen if nothing wakes it.
VXC_TEST(waterca_wake_region_settled_pool_flows_through_a_carved_breach) {
    auto breached = std::make_shared<bool>(false);
    WaterCA ca(basinWithBreach(breached));
    ca.addWater(1, 1, 6, 2000);
    CHECK(runToSettleCheckingConservation(ca, 300));
    CHECK_EQ(int(ca.fillAt(1, 1, 1)), 125);
    CHECK_EQ(int(ca.fillAt(9, 1, 1)), 0);
    CHECK_EQ(ca.activeBrickCount(), size_t(0));
    Digest settled;
    ca.digest(settled);

    *breached = true; // carve the east wall + a channel out to x=11
    for (int i = 0; i < 10; ++i) {
        ca.step();
        CHECK_EQ(ca.steppedBrickCount(), size_t(0)); // frozen until woken
    }
    Digest frozen;
    ca.digest(frozen);
    CHECK_EQ(frozen.h, settled.h);

    // The carve spans several bricks; wake over its whole inclusive box.
    CHECK(ca.wakeRegion(4, 0, 1, 11, 3, 8) >= size_t(1));
    CHECK(runToSettleCheckingConservation(ca, 800));
    CHECK_EQ(ca.totalVolume(), uint64_t(2000)); // exact conservation through the breach
    CHECK_EQ(ca.recomputeVolume(), uint64_t(2000));

    // Re-levelled across all 48 newly connected floor cells (3 chambers' worth
    // of 4x4 footprint): 2000/48 = 41 remainder 32, so every floor cell holds
    // 41 or 42 and the original chamber's 125 is long gone.
    for (int64_t x = 0; x <= 11; ++x)
        for (int64_t y = 0; y <= 3; ++y) {
            const int f = int(ca.fillAt(x, y, 1));
            CHECK(f == 41 || f == 42);
        }
    CHECK_EQ(int(ca.fillAt(1, 1, 2)), 0); // nothing left stacked above the new level
}

// The halo is what makes this work at all: the edited voxels themselves are
// dry terrain, so a zero-halo rule (wake only bricks the edit overlaps) would
// wake nothing whenever the dig sits one brick below the water. Pins that the
// halo reaches exactly one brick and no further.
VXC_TEST(waterca_wake_region_halo_reaches_one_brick_not_two) {
    WaterCA ca(basin(0, 3, 0, 3));
    ca.addWater(1, 1, 6, 2000); // settles into brick (0,0,0) (voxels z=1..)
    CHECK(runToSettleCheckingConservation(ca, 300));
    CHECK_EQ(ca.activeBrickCount(), size_t(0));
    CHECK(ca.findBrick(BrickKey{0, 0, 0}) != nullptr);

    // An edit in brick (0,0,-2) is two bricks below the water: out of reach.
    CHECK_EQ(ca.wakeRegion(1, 1, -12, 1, 1, -12), size_t(0));
    // An edit in brick (0,0,-1) is directly below it: within the halo.
    CHECK_EQ(ca.wakeRegion(1, 1, -1, 1, 1, -1), size_t(1));
}

// ===========================================================================
// C8 — mobilize-on-approach (waterca.h WaterMobilizer, docs/cavern-design.md
// §5.2). A synthetic stand-in for a flooded cavern: voxel-core's water layer
// never includes caverns.h, so the implicit field arrives as a plain lambda
// and these tests exercise the handover, not worldgen.
// ===========================================================================

namespace {

// The synthetic cavern. Rock everywhere except:
//
//   ROOM  : 0<=x<8,  0<=y<8, 4<=z<12   the cavern, implicitly flooded to z<10
//   PLUG  : x==8,    2<=y<6, 4<=z<6    rock until the "dig" opens it
//   SUMP  : 9<=x<16, 2<=y<6, -4<=z<6   a dry lower chamber to drain into
//
// The room holds 8*8*6 = 384 implicit water cells at 255 = 97,920 fill units,
// and the sump has room for most but not all of it, so the settled result has
// water on both sides of the dig — a partial drain, like the real thing.
constexpr int64_t kRoomX = 8, kRoomY = 8, kRoomZ0 = 4, kRoomZ1 = 12;
constexpr int64_t kFloodZ = 10; // implicit water fills 4 <= z < 10
constexpr uint64_t kLakeVolume = 8 * 8 * 6 * 255;

bool inRoom(int64_t vx, int64_t vy, int64_t vz) {
    return vx >= 0 && vx < kRoomX && vy >= 0 && vy < kRoomY && vz >= kRoomZ0 && vz < kRoomZ1;
}
bool inPlug(int64_t vx, int64_t vy, int64_t vz) {
    return vx == 8 && vy >= 2 && vy < 6 && vz >= 4 && vz < 6;
}
bool inSump(int64_t vx, int64_t vy, int64_t vz) {
    return vx >= 9 && vx < 16 && vy >= 2 && vy < 6 && vz >= -4 && vz < 6;
}

// `plugOpen` is the dig: flipping it turns the PLUG cells to air at runtime,
// exactly the way basinWithDrain() models a dig above.
WaterCA::SolidFn cavernTerrain(std::shared_ptr<bool> plugOpen) {
    return [plugOpen](int64_t vx, int64_t vy, int64_t vz) -> MaterialId {
        if (inRoom(vx, vy, vz) || inSump(vx, vy, vz)) return MAT_AIR;
        if (inPlug(vx, vy, vz)) return *plugOpen ? MAT_AIR : MAT_ROCK;
        return MAT_ROCK;
    };
}

// The implicit static flood field: cave air below the flood level. Mirrors
// caverns.h's documented pairing `cavernFloodedAt(col.cavern, vz) &&
// materialAt(col, vz) == MAT_AIR`.
WaterMobilizer::ImplicitFn cavernFlood() {
    return [](int64_t vx, int64_t vy, int64_t vz) -> uint8_t {
        return (inRoom(vx, vy, vz) && vz < kFloodZ) ? uint8_t(255) : uint8_t(0);
    };
}

// Sums the implicit field's CURRENT contribution (i.e. respecting the
// mobilization handover) over a box comfortably containing the whole fixture.
uint64_t implicitVolume(const WaterMobilizer& mob) {
    uint64_t sum = 0;
    for (int64_t z = -8; z < 16; ++z)
        for (int64_t y = -2; y < 10; ++y)
            for (int64_t x = -2; x < 18; ++x) sum += mob.implicitFillAt(x, y, z);
    return sum;
}

} // namespace

// An untouched lake is FREE: no bricks, no ledger, no activity — the entire
// point of generating cavern water implicitly instead of filling the CA.
VXC_TEST(waterca_mobilize_untouched_lake_costs_nothing) {
    auto plugOpen = std::make_shared<bool>(false);
    WaterMobilizer mob(cavernFlood(), cavernTerrain(plugOpen));
    WaterCA ca(mob.makeSolidFn());

    CHECK_EQ(implicitVolume(mob), kLakeVolume);
    CHECK_EQ(ca.totalVolume(), uint64_t(0));
    CHECK_EQ(ca.storedBrickCount(), size_t(0));
    CHECK_EQ(mob.mobilizedBricks().size(), size_t(0));

    for (int i = 0; i < 10; ++i) {
        mob.advanceFront(ca);
        ca.step();
    }
    CHECK_EQ(ca.storedBrickCount(), size_t(0));  // nothing ever ticked
    CHECK_EQ(implicitVolume(mob), kLakeVolume);  // and the lake is untouched
    CHECK_EQ(mob.shortfallVolume(), uint64_t(0));
}

// The wall invariant (waterca.h "SO WE MAKE IT STRUCTURALLY IMPOSSIBLE"):
// still-implicit water reads as SOLID, so CA water physically cannot occupy a
// cell the implicit field still owns. This is what makes double occupancy —
// and therefore the create/destroy choice at mobilization time — impossible.
VXC_TEST(waterca_mobilize_implicit_water_is_a_wall_until_mobilized) {
    auto plugOpen = std::make_shared<bool>(false);
    WaterMobilizer mob(cavernFlood(), cavernTerrain(plugOpen));
    WaterCA::SolidFn terrain = mob.terrainSolidFn();
    WaterCA ca(mob.makeSolidFn());

    // Bare terrain says this cell is open cave air; the composed function says
    // solid, because the implicit lake still owns it.
    CHECK_EQ(int(terrain(4, 4, 6)), int(MAT_AIR));
    CHECK_EQ(int(mob.makeSolidFn()(4, 4, 6)), int(MAT_ROCK));
    // Above the flood level it is ordinary air again.
    CHECK_EQ(int(mob.makeSolidFn()(4, 4, 11)), int(MAT_AIR));

    // Pour CA water into the air gap above the lake: it lands ON the implicit
    // surface and never sinks into it.
    const uint32_t placed = ca.addWater(4, 4, 11, 255);
    CHECK_EQ(placed, uint32_t(255));
    CHECK(runToSettleCheckingConservation(ca, 400));
    CHECK_EQ(ca.totalVolume(), uint64_t(255));
    for (int64_t z = kRoomZ0; z < kFloodZ; ++z) CHECK_EQ(int(ca.fillAt(4, 4, z)), 0);
    CHECK_EQ(mob.shortfallVolume(), uint64_t(0));
}

// THE CONSERVATION PROOF. Dig into the lake and drain it, asserting after
// EVERY tick that implicit + CA is exactly the volume we started with — no
// unit created, none lost, across the whole mobilization sequence.
VXC_TEST(waterca_mobilize_dig_into_lake_conserves_exactly) {
    auto plugOpen = std::make_shared<bool>(false);
    WaterMobilizer mob(cavernFlood(), cavernTerrain(plugOpen));
    WaterCA ca(mob.makeSolidFn());

    CHECK_EQ(implicitVolume(mob) + ca.totalVolume(), kLakeVolume);

    // The dig: open the plug, then run the two hooks an engine edit runs —
    // invalidate the solidity memo, mobilize the edit region, wake it.
    *plugOpen = true;
    ca.invalidateSolidRegion(8, 2, 4, 8, 5, 5);
    const size_t seeded = mob.mobilizeEditRegion(ca, 8, 2, 4, 8, 5, 5);
    CHECK(seeded > 0); // the dig reached real lake water
    ca.wakeRegion(8, 2, 4, 8, 5, 5);

    // Drain, advancing the front before every tick, auditing every tick.
    bool settled = false;
    for (int i = 0; i < 3000; ++i) {
        mob.advanceFront(ca);
        ca.step();
        CHECK_EQ(ca.totalVolume(), ca.recomputeVolume());
        CHECK_EQ(implicitVolume(mob) + ca.totalVolume(), kLakeVolume);
        CHECK_EQ(mob.shortfallVolume(), uint64_t(0));
        if (ca.steppedBrickCount() == 0 && mob.pendingFrontBricks() == 0) {
            settled = true;
            break;
        }
    }
    CHECK(settled);

    // It really drained: the whole lake mobilized, and water reached the sump.
    CHECK_EQ(implicitVolume(mob), uint64_t(0));
    CHECK_EQ(ca.totalVolume(), kLakeVolume);
    CHECK_EQ(mob.debitedVolume(), kLakeVolume);
    CHECK_EQ(mob.creditedVolume(), kLakeVolume);
    CHECK(int(ca.fillAt(12, 3, -4)) > 0); // sump floor, on the far side of the dig
}

// The per-tick budget is a rate limit, never a correctness knob: a deferred
// brick is still a wall, so a one-brick-per-tick front and an unbounded one
// converge on the same conserved end state and the same mobilized set. (The
// intermediate drain path legitimately differs — that IS the budget working.)
VXC_TEST(waterca_mobilize_front_budget_does_not_change_the_outcome) {
    auto run = [](size_t budget, uint64_t& outVolume, std::vector<BrickKey>& outMobilized) {
        auto plugOpen = std::make_shared<bool>(false);
        WaterMobilizer mob(cavernFlood(), cavernTerrain(plugOpen));
        WaterCA ca(mob.makeSolidFn());
        *plugOpen = true;
        ca.invalidateSolidRegion(8, 2, 4, 8, 5, 5);
        mob.mobilizeEditRegion(ca, 8, 2, 4, 8, 5, 5);
        ca.wakeRegion(8, 2, 4, 8, 5, 5);
        for (int i = 0; i < 3000; ++i) {
            mob.advanceFront(ca, budget);
            ca.step();
            CHECK_EQ(implicitVolume(mob) + ca.totalVolume(), kLakeVolume);
            if (ca.steppedBrickCount() == 0 && mob.pendingFrontBricks() == 0) break;
        }
        CHECK_EQ(mob.shortfallVolume(), uint64_t(0));
        outVolume = ca.totalVolume();
        outMobilized.assign(mob.mobilizedBricks().begin(), mob.mobilizedBricks().end());
    };

    uint64_t volSlow = 0, volFast = 0;
    std::vector<BrickKey> mobSlow, mobFast;
    run(1, volSlow, mobSlow);
    run(4096, volFast, mobFast);

    CHECK_EQ(volSlow, kLakeVolume);
    CHECK_EQ(volFast, kLakeVolume);
    CHECK(mobSlow == mobFast);
}

// Mobilization is one-way and idempotent — the `mobilizedBricks` set is what
// stops a re-entered brick from being credited twice (the duplication this
// whole class exists to prevent).
VXC_TEST(waterca_mobilize_brick_is_idempotent) {
    auto plugOpen = std::make_shared<bool>(false);
    WaterMobilizer mob(cavernFlood(), cavernTerrain(plugOpen));
    WaterCA ca(mob.makeSolidFn());

    const BrickKey k = waterKeyForVoxel(4, 4, 6); // squarely inside the lake
    const uint32_t first = mob.mobilizeBrick(ca, k);
    CHECK(first > 0);
    CHECK(mob.isMobilized(k));
    const uint64_t after = ca.totalVolume();

    for (int i = 0; i < 5; ++i) CHECK_EQ(mob.mobilizeBrick(ca, k), uint32_t(0));
    CHECK_EQ(ca.totalVolume(), after);
    CHECK_EQ(mob.shortfallVolume(), uint64_t(0));
    CHECK_EQ(mob.debitedVolume(), mob.creditedVolume());

    // A brick with no implicit water is not recorded at all — ordinary surface
    // water must not grow the persisted set.
    const BrickKey dry = waterKeyForVoxel(12, 3, -4); // in the dry sump
    CHECK_EQ(mob.mobilizeBrick(ca, dry), uint32_t(0));
    CHECK(!mob.isMobilized(dry));
}

// The replication / savegame-load inbound path credits NOTHING: the units are
// already present in the replicated or loaded CA fill, so crediting them again
// would duplicate exactly the water this class protects. It only moves the
// ownership line so the client stops drawing the brick as implicit water.
VXC_TEST(waterca_mobilize_mark_mobilized_credits_nothing) {
    auto plugOpen = std::make_shared<bool>(false);
    WaterMobilizer mob(cavernFlood(), cavernTerrain(plugOpen));
    WaterCA ca(mob.makeSolidFn());

    const BrickKey k = waterKeyForVoxel(4, 4, 6);
    mob.markMobilized(k);
    CHECK(mob.isMobilized(k));
    CHECK_EQ(ca.totalVolume(), uint64_t(0));      // nothing credited
    CHECK_EQ(mob.debitedVolume(), uint64_t(0));   // and nothing debited
    CHECK_EQ(mob.shortfallVolume(), uint64_t(0)); // so the ledger still balances

    // The brick's cells now read as CA-owned (implicit contributes 0) and are
    // no longer a wall, which is what lets the replicated fill live there.
    CHECK_EQ(int(mob.implicitFillAt(4, 4, 6)), 0);
    CHECK(implicitVolume(mob) < kLakeVolume);

    // A subsequent mobilizeBrick is a no-op, so a load followed by normal play
    // cannot re-credit the loaded lake.
    CHECK_EQ(mob.mobilizeBrick(ca, k), uint32_t(0));
    CHECK_EQ(ca.totalVolume(), uint64_t(0));
}

// Filling a hole destroys water, and it must do so QUIETLY — no shortfall
// alarm. A block placed into a still-implicit lake leaves that cell holding
// nothing (implicitFillAt gates on current terrain), so the later mobilization
// credits exactly what it debits. This is the same discontinuity a placement
// into CA water already causes in totalVolume(); the point is that the two
// kinds of water now behave identically. See the WaterMobilizer constructor's
// "WHY TERRAIN IS PART OF THE IMPLICIT FIELD" comment.
VXC_TEST(waterca_mobilize_placing_into_an_implicit_lake_raises_no_shortfall) {
    auto plugOpen = std::make_shared<bool>(false);
    // A placement overlay on top of the cavern terrain, the way an engine edit
    // sits on top of the worldgen raster.
    auto placed = std::make_shared<bool>(false);
    WaterCA::SolidFn base = cavernTerrain(plugOpen);
    WaterCA::SolidFn withPlacement = [base, placed](int64_t vx, int64_t vy,
                                                    int64_t vz) -> MaterialId {
        if (*placed && vx == 4 && vy == 4 && vz == 5) return MAT_ROCK;
        return base(vx, vy, vz);
    };
    WaterMobilizer mob(cavernFlood(), withPlacement);
    WaterCA ca(mob.makeSolidFn());

    CHECK_EQ(implicitVolume(mob), kLakeVolume);
    CHECK_EQ(int(mob.implicitFillAt(4, 4, 5)), 255);

    // Place a block into the unmobilized lake. That cell's implicit water is
    // destroyed by the placement — exactly one cell's worth, no more.
    *placed = true;
    ca.invalidateSolidRegion(4, 4, 5, 4, 4, 5);
    CHECK_EQ(int(mob.implicitFillAt(4, 4, 5)), 0);
    CHECK_EQ(implicitVolume(mob), kLakeVolume - 255);

    // Now mobilize the affected brick the way the edit hook would. The ledger
    // balances: nothing was debited for the filled cell, so nothing is missing.
    mob.mobilizeEditRegion(ca, 4, 4, 5, 4, 4, 5);
    CHECK_EQ(mob.shortfallVolume(), uint64_t(0));
    CHECK_EQ(mob.debitedVolume(), mob.creditedVolume());
    CHECK_EQ(int(ca.fillAt(4, 4, 5)), 0); // the CA never put water in the rock
    CHECK(int(ca.fillAt(4, 4, 6)) > 0);   // but the rest of the brick converted
}

// ===========================================================================
// WaterState — savegame persistence (waterca.h "WaterState",
// docs/adr/0005-water-persistence.md)
// ===========================================================================

namespace {

// Drives the synthetic cavern through a full dig-and-drain, leaving a world
// with a genuinely interesting water state to persist: a partially drained
// lake, mobilized bricks on both sides of the dig, and CA fill in bricks the
// implicit field used to own. `ticks` < 0 means "run to settled".
//
// The front budget is deliberately unbounded here. pending_ is NOT persisted
// (it is re-derived from the active set — see waterca.h's "OUT, and
// deliberately" list), so draining the queue every call is what lets the
// mid-flow resume test below compare against an uninterrupted run without the
// comparison turning into a test of queue timing rather than of persistence.
constexpr size_t kUnboundedFront = size_t(-1);

void drainCavern(WaterCA& ca, WaterMobilizer& mob, std::shared_ptr<bool> plugOpen, int ticks) {
    *plugOpen = true;
    ca.invalidateSolidRegion(8, 2, 4, 8, 5, 5);
    mob.mobilizeEditRegion(ca, 8, 2, 4, 8, 5, 5);
    ca.wakeRegion(8, 2, 4, 8, 5, 5);

    for (int i = 0; ticks < 0 ? i < 3000 : i < ticks; ++i) {
        mob.advanceFront(ca, kUnboundedFront);
        ca.step();
        if (ticks < 0 && ca.steppedBrickCount() == 0 && mob.pendingFrontBricks() == 0) return;
    }
}

uint64_t caDigest(const WaterCA& ca) {
    Digest d;
    ca.digest(d);
    return d.h;
}

uint64_t mobDigest(const WaterMobilizer& mob) {
    Digest d;
    mob.digest(d);
    return d.h;
}

} // namespace

// THE TEST ADR-0005 WAS WRITTEN TO DEMAND. Save -> load -> digest must be
// byte-identical on BOTH digests, and the total volume must be conserved
// across the cycle. A cycle that silently loses water is precisely the failure
// the ADR exists to catch, so it fails loudly here rather than being asserted
// in a comment.
VXC_TEST(waterca_state_round_trip_digest_identical_and_volume_conserved) {
    auto plugOpen = std::make_shared<bool>(false);
    WaterMobilizer mob(cavernFlood(), cavernTerrain(plugOpen));
    WaterCA ca(mob.makeSolidFn());
    drainCavern(ca, mob, plugOpen, -1);

    // Precondition for the assertions below: this really is a drained lake
    // with state worth persisting, not an empty world trivially round-tripping.
    CHECK(ca.storedBrickCount() > 0);
    CHECK(mob.mobilizedBricks().size() > 0);
    CHECK_EQ(ca.totalVolume(), kLakeVolume);

    std::vector<uint8_t> blob;
    WaterState::serialize(ca, mob, blob);

    // Load into a FRESH pair, exactly as a savegame load would.
    auto plugOpen2 = std::make_shared<bool>(true); // the dig is terrain, from the edit log
    WaterMobilizer mob2(cavernFlood(), cavernTerrain(plugOpen2));
    WaterCA ca2(mob2.makeSolidFn());
    CHECK(WaterState::load(blob.data(), blob.size(), ca2, mob2));

    // Byte-identical digests: the CA fill AND the mobilized set.
    CHECK_EQ(caDigest(ca2), caDigest(ca));
    CHECK_EQ(mobDigest(mob2), mobDigest(mob));

    // Volume conserved, on both the O(1) ledger and the independent re-sum.
    CHECK_EQ(ca2.totalVolume(), ca.totalVolume());
    CHECK_EQ(ca2.recomputeVolume(), ca.recomputeVolume());
    CHECK_EQ(ca2.totalVolume(), ca2.recomputeVolume());

    // And the WHOLE world's water — both accountants — is conserved, which is
    // the assertion that actually notices a destroyed lake: a mobilized-keys-
    // only loader passes every CA-side check above by making both terms zero.
    CHECK_EQ(implicitVolume(mob2) + ca2.totalVolume(), kLakeVolume);

    // Structural equality too, not just digest equality.
    CHECK_EQ(ca2.storedBrickCount(), ca.storedBrickCount());
    CHECK_EQ(mob2.mobilizedBricks().size(), mob.mobilizedBricks().size());
    CHECK(mob2.mobilizedBricks() == mob.mobilizedBricks());
    CHECK(ca2.activeBricks() == ca.activeBricks());

    // The blob itself is deterministic: re-serializing the loaded world
    // reproduces it byte for byte (all three key lists are sorted, and the
    // per-brick mode choice is a pure function of the brick's contents).
    std::vector<uint8_t> blob2;
    WaterState::serialize(ca2, mob2, blob2);
    CHECK(blob2 == blob);
}

// THE TRAP, MADE EXECUTABLE. This is the serializer the backlog asked for —
// mobilized keys only, no CA fill — and it is here to demonstrate, not to be
// used: it destroys the lake. Mobilization is a one-way surrender of the only
// record of where the water was, so a brick restored WITHOUT its fill reports
// zero units from the implicit field AND zero from the CA. If someone ever
// "simplifies" WaterState back to a key list, this test is what fails.
VXC_TEST(waterca_state_mobilized_keys_alone_would_destroy_the_lake) {
    auto plugOpen = std::make_shared<bool>(false);
    WaterMobilizer mob(cavernFlood(), cavernTerrain(plugOpen));
    WaterCA ca(mob.makeSolidFn());
    drainCavern(ca, mob, plugOpen, -1);
    CHECK_EQ(implicitVolume(mob) + ca.totalVolume(), kLakeVolume);

    // The naive load: replay the mobilized keys into a fresh world, as
    // markMobilized's doc comment invites — WITHOUT satisfying its stated
    // precondition that the units are already present in the loaded CA fill.
    auto plugOpen2 = std::make_shared<bool>(true);
    WaterMobilizer naive(cavernFlood(), cavernTerrain(plugOpen2));
    WaterCA naiveCa(naive.makeSolidFn());
    for (const BrickKey& k : mob.mobilizedBricks()) naive.markMobilized(k);

    // Every unit of the lake is gone. Not drained — GONE: the implicit field
    // has handed ownership over and the CA holds nothing.
    CHECK_EQ(naiveCa.totalVolume(), uint64_t(0));
    CHECK_EQ(implicitVolume(naive), uint64_t(0));
    // The mobilized-set digest ROUND TRIPS PERFECTLY while this happens, which
    // is exactly why the naive version would have shipped behind a green test.
    CHECK_EQ(mobDigest(naive), mobDigest(mob));
    // And the flooded cave now reads as open air to the CA.
    CHECK_EQ(int(naive.makeSolidFn()(4, 4, 6)), int(MAT_AIR));

    // The real loader, on the same world, keeps the water.
    std::vector<uint8_t> blob;
    WaterState::serialize(ca, mob, blob);
    auto plugOpen3 = std::make_shared<bool>(true);
    WaterMobilizer good(cavernFlood(), cavernTerrain(plugOpen3));
    WaterCA goodCa(good.makeSolidFn());
    CHECK(WaterState::load(blob.data(), blob.size(), goodCa, good));
    CHECK_EQ(implicitVolume(good) + goodCa.totalVolume(), kLakeVolume);
}

// Persistence must survive a save taken MID-FLOW, not just a settled one —
// and the resumed world must reach the same end state as one that was never
// interrupted. This is what the persisted ACTIVE SET buys: without it the
// reloaded water is a frozen snapshot waiting for an unrelated disturbance.
VXC_TEST(waterca_state_mid_flow_save_resumes_identically) {
    auto plugOpenA = std::make_shared<bool>(false);
    WaterMobilizer mobA(cavernFlood(), cavernTerrain(plugOpenA));
    WaterCA caA(mobA.makeSolidFn());
    drainCavern(caA, mobA, plugOpenA, 3); // stop mid-drain: water still moving
    CHECK(caA.activeBrickCount() > 0);
    CHECK_EQ(mobA.pendingFrontBricks(), size_t(0)); // unbounded front: nothing queued
    CHECK_EQ(implicitVolume(mobA) + caA.totalVolume(), kLakeVolume);

    std::vector<uint8_t> blob;
    WaterState::serialize(caA, mobA, blob);

    auto plugOpenB = std::make_shared<bool>(true);
    WaterMobilizer mobB(cavernFlood(), cavernTerrain(plugOpenB));
    WaterCA caB(mobB.makeSolidFn());
    CHECK(WaterState::load(blob.data(), blob.size(), caB, mobB));
    CHECK_EQ(caDigest(caB), caDigest(caA));
    CHECK_EQ(implicitVolume(mobB) + caB.totalVolume(), kLakeVolume);

    // Run BOTH to settled and compare. The reloaded world must not merely be
    // conserved, it must evolve the same way.
    for (int i = 0; i < 3000; ++i) {
        mobA.advanceFront(caA, kUnboundedFront);
        caA.step();
        if (caA.steppedBrickCount() == 0 && mobA.pendingFrontBricks() == 0) break;
    }
    for (int i = 0; i < 3000; ++i) {
        mobB.advanceFront(caB, kUnboundedFront);
        caB.step();
        if (caB.steppedBrickCount() == 0 && mobB.pendingFrontBricks() == 0) break;
    }
    CHECK_EQ(caDigest(caB), caDigest(caA));
    CHECK_EQ(mobDigest(mobB), mobDigest(mobA));
    CHECK_EQ(caB.totalVolume(), caA.totalVolume());
    CHECK_EQ(implicitVolume(mobB) + caB.totalVolume(), kLakeVolume);
    CHECK_EQ(mobB.shortfallVolume(), uint64_t(0));
}

// A truncated or tampered blob must fail CLEANLY and never half-load: the
// caller is left with an untouched (empty) world it can fall back to implicit
// water with, not a world holding a prefix of someone else's lake.
VXC_TEST(waterca_state_rejects_truncated_and_tampered_blobs) {
    auto plugOpen = std::make_shared<bool>(false);
    WaterMobilizer mob(cavernFlood(), cavernTerrain(plugOpen));
    WaterCA ca(mob.makeSolidFn());
    drainCavern(ca, mob, plugOpen, -1);

    std::vector<uint8_t> blob;
    WaterState::serialize(ca, mob, blob);
    CHECK(WaterState::parse(blob.data(), blob.size()).has_value());

    // EVERY proper prefix is rejected, and rejecting leaves the target world
    // completely untouched — the "never half-load" property, checked on the
    // world rather than only on the return value.
    for (size_t n = 0; n < blob.size(); ++n) {
        auto plug2 = std::make_shared<bool>(true);
        WaterMobilizer m2(cavernFlood(), cavernTerrain(plug2));
        WaterCA c2(m2.makeSolidFn());
        CHECK(!WaterState::load(blob.data(), n, c2, m2));
        CHECK_EQ(c2.totalVolume(), uint64_t(0));
        CHECK_EQ(c2.storedBrickCount(), size_t(0));
        CHECK_EQ(m2.mobilizedBricks().size(), size_t(0));
    }

    // Trailing garbage: a blob that decodes fully but has bytes left over is a
    // corrupt append, not a valid save.
    std::vector<uint8_t> extra = blob;
    extra.push_back(0);
    CHECK(!WaterState::parse(extra.data(), extra.size()).has_value());

    auto tamper = [&blob](size_t offset, uint8_t xorMask) {
        std::vector<uint8_t> b = blob;
        b[offset] = static_cast<uint8_t>(b[offset] ^ xorMask);
        return !WaterState::parse(b.data(), b.size()).has_value();
    };
    CHECK(tamper(0, 0xff));  // magic
    CHECK(tamper(4, 0xff));  // container format version
    CHECK(tamper(8, 0xff));  // kWaterCAVersion: fill from other tick rules
    CHECK(tamper(12, 0xff)); // totalVolume: the integrity cross-check fires

    // Empty and garbage inputs are refused rather than crashing.
    CHECK(!WaterState::parse(nullptr, 0).has_value());
    const uint8_t junk[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    CHECK(!WaterState::parse(junk, sizeof junk).has_value());
}

// Loading into a world that already holds water is refused: it would leave
// stale cells the blob never mentions, which is a silent merge, not a load.
VXC_TEST(waterca_state_refuses_a_non_empty_target) {
    auto plugOpen = std::make_shared<bool>(false);
    WaterMobilizer mob(cavernFlood(), cavernTerrain(plugOpen));
    WaterCA ca(mob.makeSolidFn());
    drainCavern(ca, mob, plugOpen, -1);
    std::vector<uint8_t> blob;
    WaterState::serialize(ca, mob, blob);

    auto plug2 = std::make_shared<bool>(true);
    WaterMobilizer m2(cavernFlood(), cavernTerrain(plug2));
    WaterCA c2(m2.makeSolidFn());
    c2.addWater(12, 3, 5, 255); // pre-existing water in the sump
    const uint64_t before = c2.totalVolume();
    CHECK(before > 0);
    CHECK(!WaterState::load(blob.data(), blob.size(), c2, m2));
    CHECK_EQ(c2.totalVolume(), before); // refused without touching anything
    CHECK_EQ(m2.mobilizedBricks().size(), size_t(0));
}

// An untouched world persists to a tiny header and reloads to nothing — the
// implicit field's "an untouched lake costs zero storage" property survives
// into the save format.
VXC_TEST(waterca_state_empty_world_round_trips) {
    auto plugOpen = std::make_shared<bool>(false);
    WaterMobilizer mob(cavernFlood(), cavernTerrain(plugOpen));
    WaterCA ca(mob.makeSolidFn());
    std::vector<uint8_t> blob;
    WaterState::serialize(ca, mob, blob);
    CHECK_EQ(blob.size(), size_t(44)); // 3 versions + volume + three zero counts

    auto plug2 = std::make_shared<bool>(false);
    WaterMobilizer m2(cavernFlood(), cavernTerrain(plug2));
    WaterCA c2(m2.makeSolidFn());
    CHECK(WaterState::load(blob.data(), blob.size(), c2, m2));
    CHECK_EQ(c2.storedBrickCount(), size_t(0));
    CHECK_EQ(implicitVolume(m2), kLakeVolume); // still a full, free, implicit lake
}

// THE MEASUREMENT (waterca.h "PAYLOAD MODES"): does per-brick mode selection
// actually earn its place, or would dense-only be fine? Measured on three real
// scenarios rather than assumed, because the two non-dense modes win on
// DIFFERENT water and a single scenario would have answered only half of it:
//   * settled water (a drained lake, a pooled basin) is long stretches of 255
//     and 0, so kRle wins by a mile — one full brick is 3 bytes.
//   * THIN VERTICAL water — a drain shaft, a waterfall, a single wet column —
//     is the case that earns kSparse its place, and it is not the case I
//     first guessed. Water in motion turned out to still be SHEETS (see the
//     mid-drain numbers this test prints: kRle wins there too, and by more
//     than on the settled lake), so "moving water is scattered droplets" was
//     simply wrong. What actually defeats kRle is the CELL ORDER: cellIndex
//     is x-fastest, so a column of water one cell wide in x and y lands as
//     ISOLATED bytes 64 apart, and every wet cell costs kRle two runs (the
//     cell, plus the zeros after it) while kSparse pays a flat 3 bytes. A
//     player cutting a drain shaft to empty a cavern — the exact story
//     ADR-0005 opens with — produces precisely this shape.
// The per-scenario check also cross-checks the emitted blob against the size
// the mode rule predicts, so this measures the real encoder rather than a copy
// of it.
namespace {

struct ModeStats {
    size_t bricks = 0, dense = 0, sparse = 0, rle = 0;
    size_t chosenBytes = 0, denseBytes = 0, blobBytes = 0, denseBlobBytes = 0;
};

ModeStats measureBlob(const std::vector<uint8_t>& blob, const char* label) {
    ModeStats st;
    const std::optional<WaterState> parsed = WaterState::parse(blob.data(), blob.size());
    CHECK(parsed.has_value());
    if (!parsed) return st;

    for (const auto& entry : parsed->bricks) {
        const WaterState::BrickFill& f = entry.second;
        size_t nonZero = 0, runs = 0;
        for (size_t i = 0; i < f.size(); ++i) {
            if (f[i] != 0) ++nonZero;
            if (i == 0 || f[i] != f[i - 1]) ++runs;
        }
        const size_t dense = f.size(), sparse = 2 + 3 * nonZero, rle = 2 + 3 * runs;
        st.denseBytes += dense;
        if (dense <= sparse && dense <= rle) {
            st.chosenBytes += dense;
            ++st.dense;
        } else if (sparse <= rle) {
            st.chosenBytes += sparse;
            ++st.sparse;
        } else {
            st.chosenBytes += rle;
            ++st.rle;
        }
    }
    st.bricks = parsed->bricks.size();

    // Fixed overhead: 28-byte header, 13 bytes per brick (key + mode byte),
    // and a counted key list for the active and mobilized sets.
    const size_t overhead = 28 + 13 * st.bricks + 8 + 12 * parsed->active.size() + 8 +
                            12 * parsed->mobilized.size();
    CHECK_EQ(blob.size(), overhead + st.chosenBytes);
    st.blobBytes = blob.size();
    st.denseBlobBytes = overhead + st.denseBytes;

    std::printf("  [measure] %-22s %3zu bricks (%zu dense/%zu sparse/%zu rle), payload %6zu B vs"
                " %6zu B dense-only, blob %6zu B vs %6zu B (%zu%% of dense)\n",
                label, st.bricks, st.dense, st.sparse, st.rle, st.chosenBytes, st.denseBytes,
                st.blobBytes, st.denseBlobBytes,
                st.denseBlobBytes ? 100 * st.blobBytes / st.denseBlobBytes : 100);
    return st;
}

} // namespace

VXC_TEST(waterca_state_sparse_and_rle_encoding_beat_dense) {
    // (1) SETTLED, MOBILIZED: the drained cavern lake — contiguous water, the
    // case kRle exists for.
    auto plugOpen = std::make_shared<bool>(false);
    WaterMobilizer mob(cavernFlood(), cavernTerrain(plugOpen));
    WaterCA ca(mob.makeSolidFn());
    drainCavern(ca, mob, plugOpen, -1);
    std::vector<uint8_t> settledBlob;
    WaterState::serialize(ca, mob, settledBlob);
    const ModeStats settled = measureBlob(settledBlob, "drained cavern");

    // (2) IN MOTION: the same cavern three ticks into the drain, which is what
    // an autosave during play actually catches.
    auto plugOpenM = std::make_shared<bool>(false);
    WaterMobilizer mobM(cavernFlood(), cavernTerrain(plugOpenM));
    WaterCA caM(mobM.makeSolidFn());
    drainCavern(caM, mobM, plugOpenM, 3);
    std::vector<uint8_t> movingBlob;
    WaterState::serialize(caM, mobM, movingBlob);
    const ModeStats moving = measureBlob(movingBlob, "cavern mid-drain");

    // (3) A LARGE SETTLED POOL: the repo's own multi-brick pool fixture (the
    // 0x56BC18914355A205 golden scenario), i.e. deep water with genuinely full
    // interior bricks.
    WaterCA pool(basin(0, 15, 0, 15));
    CHECK_EQ(pool.addWater(7, 7, 30, 300000), uint32_t(300000));
    CHECK(runToSettleCheckingConservation(pool, 5000));
    WaterMobilizer noMob(cavernFlood(), cavernTerrain(plugOpen));
    std::vector<uint8_t> poolBlob;
    WaterState::serialize(pool, noMob, poolBlob);
    const ModeStats pooled = measureBlob(poolBlob, "16x16 settled pool");

    // (4) A THIN VERTICAL COLUMN: a one-cell-wide drain shaft. Contiguous in
    // SPACE but isolated in cellIndex ORDER, which is what kRle cannot exploit
    // and kSparse can.
    WaterCA shaft(shaftAt(0, 0));
    CHECK_EQ(shaft.addWater(0, 0, 20, 500), uint32_t(500));
    CHECK(runToSettleCheckingConservation(shaft, 200));
    WaterMobilizer noMob2(cavernFlood(), cavernTerrain(plugOpen));
    std::vector<uint8_t> shaftBlob;
    WaterState::serialize(shaft, noMob2, shaftBlob);
    const ModeStats shafted = measureBlob(shaftBlob, "vertical drain shaft");

    // THE VERDICT, as assertions rather than prose:
    // kRle earns its place on settled water, and by a wide margin.
    CHECK(settled.rle > 0);
    CHECK(pooled.rle > 0);
    CHECK(settled.chosenBytes * 4 < settled.denseBytes);
    CHECK(pooled.chosenBytes * 4 < pooled.denseBytes);
    // Water in motion is still sheets, so kRle wins there too — recorded as an
    // assertion so the surprising half of the measurement cannot rot silently.
    CHECK(moving.rle > 0);
    CHECK_EQ(moving.sparse, size_t(0));
    // kSparse earns its place on the drain shaft. THIS is the assertion that
    // would fail if the third mode were dead weight and should be deleted —
    // and it is the only one of the four scenarios that fails without it.
    CHECK(shafted.sparse > 0);
    CHECK(shafted.chosenBytes * 4 < shafted.denseBytes);
    // And no scenario is ever WORSE than dense-only, because the encoder picks
    // the minimum of the three per brick.
    CHECK(settled.chosenBytes <= settled.denseBytes);
    CHECK(moving.chosenBytes <= moving.denseBytes);
    CHECK(pooled.chosenBytes <= pooled.denseBytes);
    CHECK(shafted.chosenBytes <= shafted.denseBytes);
}
