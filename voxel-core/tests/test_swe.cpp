// Fixed-point shallow-water core (voxelcore/swe.h) and its CA coupling:
// the fixed-point traps, exact lake-at-rest, exact mass conservation,
// order independence/determinism, the derived settle deadband, the force
// field, and the CA<->SWE boundary's two-sided ledger and ownership
// partition. See voxelcore/swe.h for the design writeup and
// docs/adr/0004-swe-fixed-point-coupling.md for the decision these tests
// exist to make checkable.

#include "voxelcore/swe.h"

#include <algorithm>
#include <set>
#include <vector>

#include "voxelcore/waterca.h"
#include "vxctest.h"

using namespace vxc;

namespace {

// Deterministic test-local shuffle (no <random>, no float, no dependence on
// any library's generator).
uint64_t lcg(uint64_t& s) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return s >> 33;
}

std::vector<int32_t> identityOrder(const SweGrid& g) {
    std::vector<int32_t> o(static_cast<size_t>(g.sizeX()) * static_cast<size_t>(g.sizeY()));
    for (size_t i = 0; i < o.size(); ++i) o[i] = static_cast<int32_t>(i);
    return o;
}

std::vector<int32_t> shuffledOrder(const SweGrid& g, uint64_t seed) {
    std::vector<int32_t> o = identityOrder(g);
    for (size_t i = o.size(); i > 1; --i) {
        const size_t j = static_cast<size_t>(lcg(seed) % i);
        std::swap(o[i - 1], o[j]);
    }
    return o;
}

uint64_t gridDigest(const SweGrid& g) {
    Digest d;
    g.digest(d);
    return d.h;
}

// A walled rectangular basin with an editable set of removed voxels, so the
// coupling tests can dig a real hole under a real sheet.
class BasinTerrain {
public:
    BasinTerrain(int64_t x0, int64_t y0, int64_t x1, int64_t y1, int32_t floorZ)
        : x0_(x0), y0_(y0), x1_(x1), y1_(y1), floorZ_(floorZ) {}

    void dig(int64_t vx, int64_t vy, int64_t vz) { dug_.insert({vx, vy, vz}); }

    MaterialId operator()(int64_t vx, int64_t vy, int64_t vz) const {
        if (dug_.count({vx, vy, vz})) return MAT_AIR;
        if (vz <= floorZ_) return MAT_ROCK;                     // bed and everything under it
        if (vx < x0_ || vx > x1_ || vy < y0_ || vy > y1_) return MAT_ROCK; // walls
        return MAT_AIR;
    }

    WaterCA::SolidFn fn() const {
        return [this](int64_t vx, int64_t vy, int64_t vz) { return (*this)(vx, vy, vz); };
    }

private:
    struct Key {
        int64_t x, y, z;
        bool operator<(const Key& o) const {
            if (x != o.x) return x < o.x;
            if (y != o.y) return y < o.y;
            return z < o.z;
        }
    };
    int64_t x0_, y0_, x1_, y1_;
    int32_t floorZ_;
    std::set<Key> dug_;
};

} // namespace

// ===========================================================================
// Numerics: the fixed-point traps and the derived constants
// ===========================================================================

// swe.h §4's CFL analogue is CLAMPED by the constructor, never trusted, and
// the settle tolerance is derived rather than magic.
VXC_TEST(swe_stability_bound_is_enforced_and_tolerance_is_derived) {
    SweConfig def;
    CHECK(def.stableIn2D());
    CHECK_EQ(def.dampingQ8 + static_cast<int32_t>((int64_t{8} << 8) >> def.gainShift), 240);
    CHECK_EQ(sweSettleTolerance(def), 16); // 2^7 * (256-224)/256

    // A deliberately over-gained config is unstable...
    SweConfig hot;
    hot.gainShift = 2;
    CHECK(!hot.stableIn2D());
    // ...and the grid weakens the gain until it is not, rather than ringing.
    SweGrid g(0, 0, 4, 4, hot);
    CHECK(g.config().stableIn2D());
    CHECK(g.config().gainShift > hot.gainShift);
}

// THE fixed-point trap swe.h calls out: a plain arithmetic >> rounds negatives
// toward -infinity, so the damping term would leave a stored flux of -1 stuck
// at -1 forever — a settled lake with a permanent one-unit-per-tick current in
// one direction only. shiftSym rounds both signs toward zero, so decay is
// symmetric and rest is exactly reachable.
VXC_TEST(swe_sign_symmetric_shift_lets_both_signs_decay_to_exactly_zero) {
    const int64_t damp = 224;

    int64_t plain = -1;
    for (int i = 0; i < 100; ++i) plain = (plain * damp) >> 8;
    CHECK_EQ(plain, -1); // the bug: never reaches rest

    for (int64_t start : {int64_t{-1}, int64_t{1}, int64_t{-5000}, int64_t{5000}}) {
        int64_t f = start;
        for (int i = 0; i < 200; ++i) f = shiftSym(f * damp, 8);
        CHECK_EQ(f, 0);
    }
}

// swe.h §1: lake at rest is EXACT over an arbitrarily uneven bed, because the
// bed enters only through head = bed*255 + depth and there is no source term
// to cancel against. This is the property a float Riemann-solver SWE only gets
// approximately, and the single strongest argument for the pipe formulation.
VXC_TEST(swe_lake_at_rest_is_exact_over_an_uneven_bed) {
    SweGrid g(0, 0, 9, 9);
    for (int32_t y = 0; y < 9; ++y) {
        for (int32_t x = 0; x < 9; ++x) {
            const int32_t bed = (x * 3 + y * 7) % 5; // deliberately jagged
            g.setBed(x, y, bed);
            g.addWater(x, y, (5 - bed) * 255 + 137); // equal head everywhere
        }
    }
    const uint64_t before = gridDigest(g);
    const int64_t vol = g.totalVolume();

    for (int i = 0; i < 500; ++i) g.step();

    CHECK_EQ(gridDigest(g), before); // not "settles back to" — never moves at all
    CHECK_EQ(g.totalVolume(), vol);
    for (int32_t y = 0; y < 9; ++y)
        for (int32_t x = 0; x < 9; ++x) {
            CHECK_EQ(g.faceFluxX(x, y), 0);
            CHECK_EQ(g.faceFluxY(x, y), 0);
            CHECK(g.velocityAt(x, y) == SweVelocity{});
        }
}

// swe.h §3 phase 4: conservation is a property of the apply loop. Nothing is
// created or destroyed by step() no matter how violent the transient.
VXC_TEST(swe_mass_conservation_is_exact_under_a_violent_transient) {
    SweGrid g(-4, -4, 12, 12);
    uint64_t s = 20260721ull;
    int64_t injected = 0;
    for (int32_t y = 0; y < 12; ++y)
        for (int32_t x = 0; x < 12; ++x) {
            g.setBed(-4 + x, -4 + y, static_cast<int32_t>(lcg(s) % 7));
            injected += g.addWater(-4 + x, -4 + y, static_cast<int32_t>(lcg(s) % 4000));
        }
    CHECK_EQ(g.totalVolume(), injected);

    for (int t = 0; t < 400; ++t) {
        g.step();
        CHECK_EQ(g.totalVolume(), injected);
        CHECK_EQ(g.recomputeVolume(), injected);
        // Depth is clamped by construction; it can never go negative even
        // mid-transient (swe.h §1 "it cannot blow up").
        for (int32_t y = 0; y < 12; ++y)
            for (int32_t x = 0; x < 12; ++x) CHECK(g.depthAt(-4 + x, -4 + y) >= 0);
    }
}

// swe.h §3: every phase either reads only tick-start state or touches each
// face from exactly one side, so the tick is a pure function of the column
// SET, never its enumeration order. The property a GPU port needs.
VXC_TEST(swe_tick_is_order_independent_and_deterministic) {
    auto build = []() {
        SweGrid g(0, 0, 10, 10);
        uint64_t s = 987654321ull;
        for (int32_t y = 0; y < 10; ++y)
            for (int32_t x = 0; x < 10; ++x) {
                g.setBed(x, y, static_cast<int32_t>(lcg(s) % 4));
                g.addWater(x, y, static_cast<int32_t>(lcg(s) % 3000));
            }
        return g;
    };

    SweGrid a = build(), b = build(), c = build(), d = build();
    std::vector<int32_t> rev = identityOrder(a);
    std::reverse(rev.begin(), rev.end());

    for (int t = 0; t < 120; ++t) {
        a.step();
        b.stepWithColumnOrder(identityOrder(b));
        c.stepWithColumnOrder(rev);
        // A different shuffle every tick, plus duplicates, which the tick
        // documents as ignored.
        std::vector<int32_t> sh = shuffledOrder(d, 0x5EEDu + static_cast<uint64_t>(t));
        sh.push_back(sh.front());
        sh.push_back(sh.back());
        d.stepWithColumnOrder(sh);

        CHECK_EQ(gridDigest(a), gridDigest(b));
        CHECK_EQ(gridDigest(a), gridDigest(c));
        CHECK_EQ(gridDigest(a), gridDigest(d));
    }
}

// swe.h §4: a settled surface is flat to the DERIVED deadband
// d_min = 2^gainShift * (256-dampingQ8)/256, not to some tuned number. This is
// the honest cost of fixed point and it is what ADR-0004 asks Matt to accept.
VXC_TEST(swe_pour_settles_flat_within_the_derived_deadband) {
    SweGrid g(0, 0, 9, 9);
    for (int32_t y = 0; y < 9; ++y)
        for (int32_t x = 0; x < 9; ++x) g.setBed(x, y, 0);
    const int64_t injected = g.addWater(4, 4, 100000);

    for (int t = 0; t < 20000; ++t) g.step();

    CHECK_EQ(g.totalVolume(), injected);
    const int32_t tol = sweSettleTolerance(g.config());
    int32_t worst = 0;
    for (int32_t y = 0; y < 9; ++y)
        for (int32_t x = 0; x < 9; ++x) {
            if (x + 1 < 9) worst = std::max<int32_t>(worst, static_cast<int32_t>(
                                       std::abs(g.headAt(x, y) - g.headAt(x + 1, y))));
            if (y + 1 < 9) worst = std::max<int32_t>(worst, static_cast<int32_t>(
                                       std::abs(g.headAt(x, y) - g.headAt(x, y + 1))));
        }
    CHECK(worst <= tol);
    // And it is genuinely at rest, not creeping: another 500 ticks change
    // nothing at all.
    const uint64_t settled = gridDigest(g);
    for (int t = 0; t < 500; ++t) g.step();
    CHECK_EQ(gridDigest(g), settled);
}

// swe.h §3 phase 1: the outer boundary is a hard wall. Volume can never leave
// the grid, which is what lets the coupler own the real perimeter.
VXC_TEST(swe_outer_boundary_is_a_closed_wall) {
    SweGrid g(0, 0, 5, 5);
    const int64_t injected = g.addWater(0, 0, 50000); // hard against the corner
    for (int t = 0; t < 2000; ++t) g.step();
    CHECK_EQ(g.totalVolume(), injected);
    CHECK_EQ(g.recomputeVolume(), injected);
}

// swe.h §6: the force field. Nonzero and correctly signed while water moves,
// exactly zero once it rests — the thing waterca.h's Phase C explicitly does
// not model and that a boat/foam/inrush-jet consumer needs.
VXC_TEST(swe_force_field_points_downhill_and_vanishes_at_rest) {
    SweGrid g(0, 0, 8, 1);
    for (int32_t x = 0; x < 8; ++x) g.setBed(x, 0, 0);
    g.addWater(0, 0, 40000); // all the water at -x end: flow must go +x

    bool sawPositive = false;
    for (int t = 0; t < 40; ++t) {
        g.step();
        const SweVelocity v = g.velocityAt(3, 0);
        CHECK(v.yMmPerSec == 0); // 1-wide grid: no y transport is possible
        if (v.xMmPerSec > 0) sawPositive = true;
        CHECK(v.xMmPerSec >= 0); // never flows uphill against a monotone head
    }
    CHECK(sawPositive);

    for (int t = 0; t < 20000; ++t) g.step();
    for (int32_t x = 0; x < 8; ++x) CHECK(g.velocityAt(x, 0) == SweVelocity{});
}

// Regression pin for the tick rules (swe.h §3). Moves only on a deliberate
// kSweVersion bump, exactly like the waterca goldens.
VXC_TEST(swe_pour_scenario_golden) {
    SweGrid g(0, 0, 8, 8);
    for (int32_t y = 0; y < 8; ++y)
        for (int32_t x = 0; x < 8; ++x) g.setBed(x, y, (x + y) % 3);
    g.addWater(2, 3, 60000);
    g.addWater(6, 1, 20000);
    for (int t = 0; t < 250; ++t) g.step();
    Digest d;
    g.digest(d);
    CHECK_EQ(d.h, 0x61523E585CF7B782ull); // GOLDEN(swe_pour_scenario)
}

// ===========================================================================
// The WaterCA coupling hook (waterca.h addWaterAt/removeWaterAt)
// ===========================================================================

VXC_TEST(waterca_single_cell_add_remove_are_accounted_saturating_and_wake) {
    BasinTerrain terrain(0, 0, 7, 7, -1);
    WaterCA ca(terrain.fn());

    CHECK_EQ(ca.addWaterAt(3, 3, 0, 200), 200u);
    CHECK_EQ(ca.totalVolume(), 200u);
    CHECK_EQ(ca.recomputeVolume(), 200u);
    CHECK_EQ(ca.fillAt(3, 3, 0), 200);
    CHECK(ca.activeBrickCount() > 0); // woke the brick, unlike setReplicatedFill

    CHECK_EQ(ca.addWaterAt(3, 3, 0, 500), 55u); // saturates at 255, never overflows
    CHECK_EQ(ca.fillAt(3, 3, 0), 255);
    CHECK_EQ(ca.addWaterAt(3, 3, -1, 10), 0u);  // solid: refuses

    CHECK_EQ(ca.removeWaterAt(3, 3, 0, 1000), 255u); // caps at what is present
    CHECK_EQ(ca.totalVolume(), 0u);
    CHECK_EQ(ca.recomputeVolume(), 0u);
    CHECK_EQ(ca.storedBrickCount(), 0u); // homogeneous-empty collapse still applies
}

// ===========================================================================
// CA <-> SWE coupling (swe.h §5)
// ===========================================================================

// ADR-0004 is PENDING, so the coupler ships inert. Not "cheap when off" —
// provably a total no-op.
VXC_TEST(swe_coupler_is_a_total_no_op_when_disabled) {
    BasinTerrain terrain(0, 0, 7, 7, -1);
    WaterCA ca(terrain.fn());
    SweGrid g(0, 0, 8, 8);
    for (int32_t y = 0; y < 8; ++y)
        for (int32_t x = 0; x < 8; ++x) g.setBed(x, y, -1);
    g.addWater(4, 4, 5000);
    ca.addWaterAt(2, 2, 0, 200);

    SweCaCoupler cp(g, ca, terrain.fn());
    CHECK(!cp.config().enabled); // the shipped default

    Digest gd0, cd0;
    g.digest(gd0);
    ca.digest(cd0);
    for (int t = 0; t < 50; ++t) cp.step();
    Digest gd1, cd1;
    g.digest(gd1);
    ca.digest(cd1);

    CHECK_EQ(gd1.h, gd0.h);
    CHECK_EQ(cd1.h, cd0.h);
    CHECK_EQ(cp.transferredToCA(), 0);
    CHECK_EQ(cp.transferredToSWE(), 0);
    CHECK_EQ(cp.sweColumnCount(), 0);
}

// THE headline coupling invariant (swe.h §5): every unit that crosses the
// boundary leaves one ledger and enters the other in the same statement, so
// the coupled system conserves exactly across an arbitrary sequence of ticks —
// including promotion, a dug puncture, a metered inrush, and demotion.
VXC_TEST(swe_coupler_conserves_volume_across_the_boundary) {
    BasinTerrain terrain(0, 0, 7, 7, -1);
    WaterCA ca(terrain.fn());
    SweGrid g(0, 0, 8, 8);
    for (int32_t y = 0; y < 8; ++y)
        for (int32_t x = 0; x < 8; ++x) g.setBed(x, y, -1);

    SweCoupleConfig cfg;
    cfg.enabled = true;
    SweCaCoupler cp(g, ca, terrain.fn(), cfg);

    int64_t injected = 0;
    for (int32_t y = 1; y < 7; ++y)
        for (int32_t x = 1; x < 7; ++x) {
            cp.forcePromote(x, y);
            injected += g.addWater(x, y, 1200);
        }
    injected += static_cast<int64_t>(ca.addWaterAt(0, 0, 0, 200)); // a CA-owned corner
    CHECK_EQ(ca.totalVolume() + static_cast<uint64_t>(g.totalVolume()),
             static_cast<uint64_t>(injected));

    // Dig a shaft straight down under the middle of the sheet.
    for (int64_t z = -1; z >= -6; --z) terrain.dig(4, 4, z);
    ca.invalidateSolidCache();
    ca.wakeRegion(4, 4, -6, 4, 4, 0);

    for (int t = 0; t < 300; ++t) {
        cp.step();
        g.step();
        ca.step();
        CHECK_EQ(ca.totalVolume() + static_cast<uint64_t>(g.totalVolume()),
                 static_cast<uint64_t>(injected));
    }
    CHECK_EQ(ca.recomputeVolume() + static_cast<uint64_t>(g.recomputeVolume()),
             static_cast<uint64_t>(injected));
    CHECK(cp.transferredToCA() > 0); // the puncture really did drain
}

// swe.h §5(a): a punctured column is a METERED source, not an instant dump,
// and the hand-over to the CA completes on its own via the eligibility
// predicate + demote dwell — no special case.
VXC_TEST(swe_coupler_puncture_meters_the_inrush_then_hands_the_column_over) {
    BasinTerrain terrain(0, 0, 7, 7, -1);
    WaterCA ca(terrain.fn());
    SweGrid g(0, 0, 8, 8);
    for (int32_t y = 0; y < 8; ++y)
        for (int32_t x = 0; x < 8; ++x) g.setBed(x, y, -1);

    SweCoupleConfig cfg;
    cfg.enabled = true;
    SweCaCoupler cp(g, ca, terrain.fn(), cfg);
    for (int32_t y = 1; y < 7; ++y)
        for (int32_t x = 1; x < 7; ++x) cp.forcePromote(x, y);
    g.addWater(4, 4, 2000);
    CHECK(cp.isSweColumn(4, 4));

    for (int64_t z = -1; z >= -8; --z) terrain.dig(4, 4, z);
    ca.invalidateSolidCache();

    // Metered: the first tick moves at most drainPerTick, never the lot.
    const int32_t before = g.depthAt(4, 4);
    cp.step();
    const int32_t movedFirstTick = before - g.depthAt(4, 4);
    CHECK(movedFirstTick > 0);
    CHECK(movedFirstTick <= cfg.drainPerTick);
    CHECK(cp.lastPuncturedCount() >= 1);
    CHECK(cp.isSweColumn(4, 4)); // still SWE-owned: the dwell window has not elapsed

    for (int t = 0; t < 200; ++t) {
        cp.step();
        g.step();
        ca.step();
    }
    // Hand-over completed: the column is CA-owned and holds no sheet depth.
    CHECK(!cp.isSweColumn(4, 4));
    CHECK_EQ(g.depthAt(4, 4), 0);
    CHECK(cp.transferredToCA() > 0);
}

// swe.h §5 mechanism 1: a column flickering in and out of eligibility EVERY
// tick must never flip owner. Membership chatter is the oscillation mode the
// plan flags as W4's risk, and it is excluded by the dwell windows, not tuned
// away.
VXC_TEST(swe_coupler_hysteresis_prevents_membership_chatter) {
    BasinTerrain terrain(0, 0, 7, 7, -1);
    WaterCA ca(terrain.fn());
    SweGrid g(0, 0, 8, 8);
    for (int32_t y = 0; y < 8; ++y)
        for (int32_t x = 0; x < 8; ++x) g.setBed(x, y, -1);

    SweCoupleConfig cfg;
    cfg.enabled = true;
    // A lid that appears and vanishes on alternate ticks, driven by a
    // caller-controlled flag the solidity callback reads.
    bool lid = false;
    auto solid = [&terrain, &lid](int64_t vx, int64_t vy, int64_t vz) -> MaterialId {
        if (lid && vx == 3 && vy == 3 && vz == 2) return MAT_ROCK;
        return terrain(vx, vy, vz);
    };
    SweCaCoupler cp(g, ca, solid, cfg);

    int32_t flips = 0;
    bool prev = cp.isSweColumn(3, 3);
    for (int t = 0; t < 200; ++t) {
        lid = (t % 2) == 0; // eligibility for (3,3) alternates every single tick
        cp.step();
        const bool now = cp.isSweColumn(3, 3);
        if (now != prev) ++flips;
        prev = now;
    }
    // Neither dwell window can ever be satisfied by an alternating signal, so
    // the column never changes owner at all.
    CHECK_EQ(flips, 0);
    CHECK(!cp.isSweColumn(3, 3));
}

// swe.h S5 ownership. TWO DIFFERENT STRENGTHS of guarantee, asserted
// separately because they genuinely are different and the header now says so:
//   (1) the SWE side is ABSOLUTE — a CA-owned column is a hard wall in the
//       numerics (setColumnActive) and can never receive sheet water at all;
//   (2) the CA side is RATE-LIMITED — CA fill appearing inside a sheet range
//       is evacuated at up to absorbPerTick per column per tick, so it is
//       fully cleared exactly when the source stays within that rate.
VXC_TEST(swe_coupler_ownership_partition_walls_off_and_evacuates) {
    BasinTerrain terrain(0, 0, 7, 7, -1);
    WaterCA ca(terrain.fn());
    SweGrid g(0, 0, 8, 8);
    for (int32_t y = 0; y < 8; ++y)
        for (int32_t x = 0; x < 8; ++x) g.setBed(x, y, -1);

    SweCoupleConfig cfg;
    cfg.enabled = true;
    SweCaCoupler cp(g, ca, terrain.fn(), cfg);
    for (int32_t y = 1; y < 7; ++y)
        for (int32_t x = 1; x < 7; ++x) {
            cp.forcePromote(x, y);
            g.addWater(x, y, 900);
        }

    // (1) Enabling the coupler seated the perimeter: nothing is active except
    // what was promoted.
    for (int32_t k = 0; k < 8; ++k) {
        CHECK(!g.columnActive(0, k));
        CHECK(!g.columnActive(7, k));
    }
    // The four basin CORNERS have only 2 open lateral neighbours, so they fail
    // the eligibility predicate forever and stay CA-owned for the whole run.
    // (The wall-adjacent EDGES have 3 and do legitimately promote — that is the
    // predicate working, not a leak, and the conservation test covers them.)
    const int32_t cornerX[4] = {0, 7, 0, 7};
    const int32_t cornerY[4] = {0, 0, 7, 7};
    const int64_t sheetVol = g.totalVolume();

    // (2) Shove CA water up into the middle of the sheet every tick, at
    // strictly under the absorb rate.
    for (int t = 0; t < 120; ++t) {
        ca.addWaterAt(3, 3, 0, 100); // < cfg.absorbPerTick
        cp.step();
        g.step();
        ca.step();
        for (int32_t k = 0; k < 4; ++k) {
            CHECK(!g.columnActive(cornerX[k], cornerY[k])); // never receives sheet water
            CHECK_EQ(g.depthAt(cornerX[k], cornerY[k]), 0);
            CHECK(!cp.isSweColumn(cornerX[k], cornerY[k]));
        }
        CHECK_EQ(ca.totalVolume(), 0u); // fully evacuated every single tick
    }
    CHECK(cp.transferredToSWE() > 0);
    CHECK(g.totalVolume() > sheetVol); // and it really landed in the sheet
}

// swe.h §5 eligibility: the predicate is precisely "where the depth-averaged
// assumption is legitimate" — a lidded (flooded-tunnel) column and a
// one-voxel-wide pipe both stay with the CA no matter how long they dwell.
VXC_TEST(swe_coupler_eligibility_rejects_lidded_and_confined_columns) {
    BasinTerrain terrain(0, 0, 7, 7, -1);
    WaterCA ca(terrain.fn());
    SweGrid g(0, 0, 8, 8);
    for (int32_t y = 0; y < 8; ++y)
        for (int32_t x = 0; x < 8; ++x) g.setBed(x, y, -1);

    SweCoupleConfig cfg;
    cfg.enabled = true;
    auto solid = [&terrain](int64_t vx, int64_t vy, int64_t vz) -> MaterialId {
        if (vx == 2 && vy == 2 && vz == 1) return MAT_ROCK; // a lid over (2,2)
        return terrain(vx, vy, vz);
    };
    SweCaCoupler cp(g, ca, solid, cfg);

    for (int t = 0; t < 100; ++t) cp.step();

    CHECK(!cp.isSweColumn(2, 2)); // lidded: not a free surface
    CHECK(!cp.isSweColumn(0, 0)); // basin corner: only 2 open sides, too confined
    CHECK(cp.isSweColumn(4, 4));  // open interior: promoted
    CHECK(cp.sweColumnCount() > 0);
}
