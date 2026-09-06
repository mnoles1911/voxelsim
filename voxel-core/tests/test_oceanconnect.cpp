// OCEAN CONNECTIVITY + THE ROCK-POOL DATUM, PINNED (tides plan Phase C gates:
// "oceanconnect + TidalDatumSource truth-table tests", each able to fail).
//
// THE MUST-FAIL DISCIPLINE (test-must-be-able-to-fail): the ridge-pool test
// carries its own mutation arm -- the SAME fixture with the ridge lowered
// flips to connected -- so "pool not connected" is proven to be a fact about
// the ridge rather than about a fill that never reaches anything. The truth
// table pins every branch of TidalDatumSource with exact integers; a rewrite
// of any comparison moves a pinned literal.

#include <cstdint>
#include <cstring>
#include <vector>

#include "voxelcore/oceanconnect.h"
#include "voxelcore/tidal.h"
#include "vxctest.h"

using namespace vxc;

namespace {

// A little world builder: n x n ground, everything defaulting to dry land
// well above the water, painted down by the tests.
struct Grid {
    int32_t n;
    std::vector<int32_t> ground;
    std::vector<uint8_t> bits;
    explicit Grid(int32_t inN, int32_t landMm = 5'000'000)
        : n(inN), ground(size_t(inN) * size_t(inN), landMm), bits(size_t(inN) * size_t(inN), 0) {}
    int32_t& at(int32_t x, int32_t y) { return ground[size_t(y) * size_t(n) + size_t(x)]; }
    uint8_t bit(int32_t x, int32_t y) const { return bits[size_t(y) * size_t(n) + size_t(x)]; }
    OceanConnectStats fill(int32_t tideNowMm, int32_t seedLevelMm) {
        return oceanConnectivityFill(ground.data(), n, tideNowMm, seedLevelMm, bits.data());
    }
};

// The numbers all the fixtures share, chosen to look like the shipped world:
// sea at 0 (a stand-in for kSeaLevelMm's role, not its value -- the fill takes
// absolutes and never reads the constant), tide range +-1050 (the default
// spec's), seed guard a full range plus margin below the sea.
constexpr int32_t kSea = 0;
constexpr int32_t kSeedLevel = kSea - 1050 - 1000; // sea - tideMax - margin

} // namespace

// --- the fill ----------------------------------------------------------------

// A bay: deep sea along the west edge, a shallow arm reaching inland. Every
// wet cell is reachable from the border seeds; the stats tie out exactly.
VXC_TEST(oceanconnect_bay_is_connected_and_the_stats_tie_out) {
    Grid g(8);
    for (int32_t y = 0; y < 8; ++y) g.at(0, y) = -10'000;         // deep sea, west edge
    for (int32_t x = 1; x <= 5; ++x) g.at(x, 3) = -500;           // the bay arm, shallow
    const OceanConnectStats s = g.fill(kSea, kSeedLevel);
    CHECK_EQ(int(s.seedCells), 8);           // the whole west edge, deep AND wet
    CHECK_EQ(int(s.wetCells), 13);           // 8 sea + 5 arm
    CHECK_EQ(int(s.connectedCells), 13);     // a bay IS the sea
    for (int32_t x = 1; x <= 5; ++x) CHECK_EQ(int(g.bit(x, 3)), 1);
    CHECK_EQ(int(g.bit(6, 3)), 0); // the arm ends where the ground rises
}

// A rock pool behind a ridge: wet, below sea even, and NOT connected -- with
// the mutation arm that proves the verdict comes from the ridge. This is the
// gate's own falsifier: a fill that answered "connected" for everything wet
// (the pre-Phase-C IsOpenSea error class) fails the first half, and a fill
// that answered "disconnected" for everything fails the second.
VXC_TEST(oceanconnect_ridge_locked_pool_is_not_connected_until_the_ridge_goes) {
    Grid g(8);
    for (int32_t y = 0; y < 8; ++y) g.at(0, y) = -10'000; // sea
    g.at(2, 4) = -300;                                    // the pool: wet at kSea
    g.at(3, 4) = -300;
    // Ridge cells between pool and sea stay at the default +5000 m land.
    const OceanConnectStats s = g.fill(kSea, kSeedLevel);
    CHECK_EQ(int(s.wetCells), 10);
    CHECK_EQ(int(s.connectedCells), 8); // the sea alone; the pool holds its own
    CHECK_EQ(int(g.bit(2, 4)), 0);
    CHECK_EQ(int(g.bit(3, 4)), 0);
    // THE MUTATION ARM: breach the ridge one cell wide, below the waterline.
    g.at(1, 4) = -200;
    const OceanConnectStats s2 = g.fill(kSea, kSeedLevel);
    CHECK_EQ(int(s2.connectedCells), 11); // sea + breach + both pool cells
    CHECK_EQ(int(g.bit(2, 4)), 1);
    CHECK_EQ(int(g.bit(3, 4)), 1);
}

// The tide decides: the same pool over a submerged sill is part of the sea at
// high water and its own body one quantum after the tide drops below the
// sill. This is the rock-pool cycle at grid level.
VXC_TEST(oceanconnect_pool_flips_across_a_tide_step) {
    Grid g(8);
    for (int32_t y = 0; y < 8; ++y) g.at(0, y) = -10'000; // sea
    g.at(1, 4) = 500;                                     // the sill: above kSea
    g.at(2, 4) = -300;                                    // the pool
    // High water, sill submerged (tide 525 > sill 500):
    g.fill(kSea + 525, kSeedLevel);
    CHECK_EQ(int(g.bit(2, 4)), 1);
    CHECK_EQ(int(g.bit(1, 4)), 1);
    // One 25 mm quantum lower the sill stands proud (500 >= 500 is DRY --
    // strictly-below is the same rule oceanSurfaceMmAt draws):
    g.fill(kSea + 500, kSeedLevel);
    CHECK_EQ(int(g.bit(1, 4)), 0);
    CHECK_EQ(int(g.bit(2, 4)), 0); // wet, but cut off: the pool holds
}

// FOUR-connected on purpose: a diagonal pinch between two dry cells is not a
// channel. lakeExtentFill is 8-connected (pinned to the bake); this rule is
// runtime-only and takes the physics. If someone "fixes" the disagreement in
// either direction, one of the two suites trips.
VXC_TEST(oceanconnect_does_not_flow_through_a_diagonal_pinch) {
    Grid g(5);
    g.at(0, 0) = -10'000; // sea corner (border, deep)
    g.at(1, 1) = -300;    // diagonal neighbour, wet
    const OceanConnectStats s = g.fill(kSea, kSeedLevel);
    CHECK_EQ(int(s.seedCells), 1);
    CHECK_EQ(int(g.bit(1, 1)), 0); // reachable only diagonally: NOT connected
    CHECK_EQ(int(s.connectedCells), 1);
}

// THE DEEP-MARGIN GUARD: an inland below-sea playa touching the window edge is
// wet at the border but SHALLOW, so it must not seed itself into being ocean.
// Deepen it past the guard and it does -- the guard, too, can fail.
VXC_TEST(oceanconnect_shallow_border_playa_cannot_self_seed) {
    Grid g(6);
    g.at(5, 2) = -800; // border cell, wet at kSea, but above kSeedLevel
    g.at(4, 2) = -800;
    const OceanConnectStats s = g.fill(kSea, kSeedLevel);
    CHECK_EQ(int(s.seedCells), 0);
    CHECK_EQ(int(s.wetCells), 2);
    CHECK_EQ(int(s.connectedCells), 0); // wet water that is NOT the sea
    // Mutation: sink the border cell below the guard and the playa becomes a
    // (correctly detected) deep channel off the window edge.
    g.at(5, 2) = kSeedLevel - 1;
    const OceanConnectStats s2 = g.fill(kSea, kSeedLevel);
    CHECK_EQ(int(s2.seedCells), 1);
    CHECK_EQ(int(s2.connectedCells), 2);
}

// Determinism is a byte-for-byte claim, checked as one: same input, two runs,
// identical output buffers and identical stats. (The scan order and FIFO are
// the construction; this is the regression tripwire for anyone who "improves"
// the walk with an unordered container.)
VXC_TEST(oceanconnect_is_deterministic_byte_for_byte) {
    Grid g(16);
    // A ragged coast: pseudo-random-ish but fixed by arithmetic, no RNG.
    for (int32_t y = 0; y < 16; ++y)
        for (int32_t x = 0; x < 16; ++x)
            g.at(x, y) = ((x * 7 + y * 13) % 5 == 0) ? -9'000 : ((x + y) % 3 == 0 ? -400 : 2'000);
    const OceanConnectStats a = g.fill(kSea + 275, kSeedLevel);
    std::vector<uint8_t> first = g.bits;
    const OceanConnectStats b = g.fill(kSea + 275, kSeedLevel);
    CHECK(first == g.bits);
    CHECK_EQ(a.seedCells, b.seedCells);
    CHECK_EQ(a.wetCells, b.wetCells);
    CHECK_EQ(a.connectedCells, b.connectedCells);
    // And the stats are internally consistent everywhere, not just here:
    CHECK(a.connectedCells <= a.wetCells);
    CHECK(a.seedCells <= a.connectedCells || a.seedCells == 0);
}

// Total for degenerate input, like every other arithmetic layer here.
VXC_TEST(oceanconnect_is_total_for_degenerate_input) {
    OceanConnectStats s = oceanConnectivityFill(nullptr, 4, 0, -1000, nullptr);
    CHECK_EQ(int(s.seedCells), 0);
    CHECK_EQ(int(s.wetCells), 0);
    CHECK_EQ(int(s.connectedCells), 0);
    Grid g(1, -10'000);
    s = g.fill(kSea, kSeedLevel);
    CHECK_EQ(int(s.seedCells), 1); // a 1x1 window IS all border
    CHECK_EQ(int(s.connectedCells), 1);
    s = oceanConnectivityFill(g.ground.data(), 0, 0, 0, g.bits.data());
    CHECK_EQ(int(s.wetCells), 0);
}

// --- TidalDatumSource: the full truth table ---------------------------------

namespace {

struct FixtureInner final : IBasinDatumSource {
    int32_t answer = 0;
    int32_t calls = 0;
    int32_t basinDatumMm(int32_t, int32_t, const BasinEntry&) override {
        ++calls;
        return answer;
    }
};

struct FixtureOracle final : ITidalBasinOracle {
    bool tidal = false;
    bool isTidal(int32_t, int32_t, const BasinEntry&) override { return tidal; }
};

BasinEntry poolRow(int32_t spillMm, int32_t floorMm, int32_t surfaceMm) {
    BasinEntry b{};
    b.spillMm = spillMm;
    b.floorMm = floorMm;
    b.surfaceMm = surfaceMm;
    return b;
}

} // namespace

// NON-TIDAL PASSES THROUGH BIT-EXACT -- the inland gate's foundation. Every
// inner answer, sentinel included, comes back untouched at every tide.
VXC_TEST(tidal_datum_non_tidal_passthrough_is_bit_exact) {
    FixtureInner inner;
    FixtureOracle oracle; // tidal = false
    TidalDatumSource tidal(inner, oracle);
    const BasinEntry b = poolRow(400, -900, 250);
    const int32_t weird[] = {kNoWaterMm, INT32_MIN + 1, -1, 0, 250, INT32_MAX};
    for (int32_t tide : {-1050, 0, 401, 1050}) {
        tidal.setTideNowMm(tide);
        for (int32_t w : weird) {
            inner.answer = w;
            CHECK_EQ(tidal.basinDatumMm(0, 0, b), w);
        }
    }
    CHECK(inner.calls > 0); // the inner source was genuinely consulted
}

// CONNECTED RIDES THE TIDE: tide at or over the sill puts the pool's surface
// AT the tide -- the pool is one body with the sea.
VXC_TEST(tidal_datum_connected_pool_rides_the_tide) {
    FixtureInner inner;
    FixtureOracle oracle;
    oracle.tidal = true;
    TidalDatumSource tidal(inner, oracle);
    const BasinEntry b = poolRow(400, -900, 250);
    inner.answer = 250; // the baked equilibrium, below everything tidal here
    tidal.setTideNowMm(400); // exactly at the sill: equality is CONNECTED
    CHECK_EQ(tidal.basinDatumMm(0, 0, b), 400);
    tidal.setTideNowMm(1025); // high water
    CHECK_EQ(tidal.basinDatumMm(0, 0, b), 1025);
}

// DISCONNECTED HOLDS AT THE SILL, however far the tide falls: the pool was
// filled to its brim when the sea left, and nothing drains it.
VXC_TEST(tidal_datum_disconnected_pool_holds_full_at_its_sill) {
    FixtureInner inner;
    FixtureOracle oracle;
    oracle.tidal = true;
    TidalDatumSource tidal(inner, oracle);
    const BasinEntry b = poolRow(400, -900, 250);
    inner.answer = 250;
    tidal.setTideNowMm(399); // one mm under the sill: cut off
    CHECK_EQ(tidal.basinDatumMm(0, 0, b), 400);
    tidal.setTideNowMm(-1050); // dead low water
    CHECK_EQ(tidal.basinDatumMm(0, 0, b), 400);
}

// THE LEDGER WINS WHEN HIGHER (risk-register row): a rain-credited pool
// standing above the tide's answer keeps the ledger's level in BOTH tidal
// states. The tide is an overlay, never a drain.
VXC_TEST(tidal_datum_ledger_wins_when_higher) {
    FixtureInner inner;
    FixtureOracle oracle;
    oracle.tidal = true;
    TidalDatumSource tidal(inner, oracle);
    const BasinEntry b = poolRow(400, -900, 250);
    inner.answer = 700; // credited well above the sill
    tidal.setTideNowMm(-1050); // disconnected: tidal answer would be 400
    CHECK_EQ(tidal.basinDatumMm(0, 0, b), 700);
    tidal.setTideNowMm(500); // connected: tidal answer would be 500
    CHECK_EQ(tidal.basinDatumMm(0, 0, b), 700);
    tidal.setTideNowMm(1025); // ...until the tide genuinely stands higher
    CHECK_EQ(tidal.basinDatumMm(0, 0, b), 1025);
}

// A DRY tidal row (surfaceMm == kNoWaterMm sentinel through the inner source)
// still answers the tidal datum: the sentinel is INT32_MIN and can never win
// the max, which is exactly how a dry playa the tide floods gets a surface
// without a ledger row. (This is the arithmetic behind blocker 2's admit-on-
// demand -- the admission itself lives in LakeSampler.)
VXC_TEST(tidal_datum_dry_row_sentinel_never_wins_the_max) {
    FixtureInner inner;
    FixtureOracle oracle;
    oracle.tidal = true;
    TidalDatumSource tidal(inner, oracle);
    const BasinEntry b = poolRow(400, -900, kNoWaterMm);
    inner.answer = kNoWaterMm;
    tidal.setTideNowMm(-1050);
    CHECK_EQ(tidal.basinDatumMm(0, 0, b), 400); // held at the sill, not "no water"
    tidal.setTideNowMm(600);
    CHECK_EQ(tidal.basinDatumMm(0, 0, b), 600);
}
