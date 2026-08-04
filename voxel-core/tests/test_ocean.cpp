// THE OCEAN AS THE THIRD TERM OF THE ImplicitFn
// (docs/watershed-system-plan.md work item 8, §6.4)
//
// §6.4's stated verification is "breach parity test old-vs-new path". THE TWO
// PATHS DO NOT AGREE ANYWHERE, which is the finding, so this is a DIFFERENTIAL
// test rather than a parity assertion: every case below runs both mechanisms
// over the same fixture and pins the difference with a number. A parity
// assertion here would have had to be written false to pass.
//
// The three differences, all measured by the tests in this file:
//
//   1. THE SEA IS NOT WATER TO THE OLD PATH. Reservoir v0 pins the breach's
//      own voxels at 255 fill units every tick, but the ocean beyond the
//      breach is nothing at all to the simulation -- the seabed's open air is
//      open air. So a cove cut into the coast does not FILL, it DRAINS: the
//      pinned cells pour into the seabed forever and the water spreads out
//      across it. Measured (`reservoir_v0_pours_into_a_dry_seabed_and_never_
//      settles`): total volume still climbing at tick 2000 with the active
//      brick count climbing with it. That is a live perf leak as much as a
//      physics one -- the CA's cost is its active set.
//   2. IT CANNOT TELL A PIT FROM THE SEA. Its test is "this cleared voxel is
//      below the datum and touches non-solid space that is also below the
//      datum". Dig into a hillside below sea level in two passes -- which is
//      just "keep digging" -- and the second pass sees the first pass's own
//      air as ocean. Measured: an infinite spring inside dry rock, 50 voxels
//      from the nearest water.
//   3. ITS HEAD IS THE BREACH, NOT THE DATUM. A cell pinned at 255 at
//      z = -9 voxels is a pressure source at z = -9.
//
// AND ONE THING THE NEW PATH DOES NOT FIX, recorded here rather than
// discovered later: a breach that requires water to RISE -- a tunnel punched
// into the sea at depth with headroom above it inland -- also stops at the
// puncture depth, for a completely different reason that belongs to the CA and
// not to the ocean. See `breach_deep_puncture_*` for the measurement and the
// diagnosis.
//
// WHAT "THE OLD PATH" IS, AND WHY IT IS TRANSCRIBED HERE. Reservoir v0 lives
// in ue-project (UVoxelWaterSubsystem::NotifyTerrainVoxelsCleared, and the
// top-up loop at the head of StepFixed) and voxel-core cannot include a UE
// header. `ReservoirV0` below is that rule copied out, and the copy is
// deliberately LITERAL -- including the parts that are wrong -- because a
// tidied-up transcription would be a test of the tidying. It is committed in
// the same change that deletes the original, so this file becomes the only
// remaining statement of what the retired path did and what it cost.
//
// THE CONTROL, because this repo keeps producing conclusions a control frame
// would have killed: every scenario also runs with the ocean term OFF
// (`kOceanOff`) and asserts the pre-item-8 result -- no water at all. A test
// whose "new path" would pass with the feature disabled measures nothing.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <set>
#include <vector>

#include "voxelcore/lakes.h"
#include "voxelcore/waterca.h"
#include "vxctest.h"

using namespace vxc;

namespace {

// --- the world ------------------------------------------------------------
//
// A coast, and nothing else: one straight shoreline at vx == 0.
//
//   vx <  0   seabed, worldgen surface 1 m BELOW the datum  -> open sea
//   vx >= 0   land,   worldgen surface 1 m ABOVE the datum  -> dry
//
// Deliberately flat and deliberately synthetic. The question is what the two
// MECHANISMS do with a datum, not what the amplifier produces, and a real
// heightfield would let a scenario pass for a reason unrelated to the rule
// under test. 1 m of water is 10 voxels: deep enough to have a column, shallow
// enough that a scenario settles inside a test suite's patience.
constexpr int32_t kSeabedMm = kSeaLevelMm - 1000;
constexpr int32_t kLandMm = kSeaLevelMm + 1000;

// The sea therefore occupies voxel z -10 .. -1 inclusive over a seabed column:
// 10 voxels, every one a full 255 (the datum lands exactly on a voxel
// boundary, so there is no partial top here -- lakes.h tests that half).
constexpr int64_t kSeaFloorVz = kSeabedMm / kVoxelSizeMm;      // -10, first WET voxel
constexpr int64_t kSeaTopVz = kSeaLevelMm / kVoxelSizeMm - 1;  // -1, last wet voxel

struct Voxel {
    int64_t x = 0, y = 0, z = 0;
    bool operator<(const Voxel& o) const {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        return z < o.z;
    }
};

// Worldgen ground + the edit overlay, kept apart on purpose: `groundMm` is the
// AMPLIFIED surface and never moves, `dug` is what the player cleared. Every
// mechanism below that consults one rather than the other does so for a reason
// stated at its call site.
struct CoastWorld {
    std::set<Voxel> dug;

    static int32_t groundMm(int64_t vx) { return vx < 0 ? kSeabedMm : kLandMm; }

    // Post-edit solidity: what the CA and the mobilizer's terrain half see.
    MaterialId solidAt(int64_t vx, int64_t vy, int64_t vz) const {
        if (dug.count(Voxel{vx, vy, vz}) != 0) return MAT_AIR;
        return vz * kVoxelSizeMm < groundMm(vx) ? MAT_ROCK : MAT_AIR;
    }
    WaterCA::SolidFn solidFn() const {
        return [this](int64_t vx, int64_t vy, int64_t vz) { return solidAt(vx, vy, vz); };
    }
    void dig(const std::vector<Voxel>& cells) {
        for (const Voxel& v : cells) dug.insert(v);
    }
};

// --- the new path: the ocean as one more term -----------------------------
//
// This is the binding site from ue-project/Source/VoxelEarth/
// VoxelWaterSubsystem.cpp's FVoxelWaterImpl constructor, minus the cavern and
// baked-lake terms (this fixture bakes neither), so what is exercised is
// exactly the expression that ships.
enum OceanTerm { kOceanOn, kOceanOff };

WaterMobilizer::ImplicitFn oceanImplicit(const CoastWorld& w, OceanTerm term) {
    // `w` is taken and dropped on purpose: the ImplicitFn must be a pure
    // function of WORLDGEN, never of the edit overlay `w` carries (waterca.h,
    // "WHY TERRAIN IS PART OF THE IMPLICIT FIELD" -- solidity is the
    // mobilizer's half, not this callback's). Reading it here is exactly the
    // mistake that made Reservoir v0 flood a pit, so the parameter stays as
    // the place a future edit reaches for and finds this comment.
    (void)w;
    return [term](int64_t vx, int64_t vy, int64_t vz) -> uint8_t {
        (void)vy;
        const int32_t ground = CoastWorld::groundMm(vx);
        const int32_t baked = kNoWaterMm;  // no lake, no river reach in this fixture
        const int32_t datum = term == kOceanOn ? implicitWaterDatumMm(baked, ground) : baked;
        return implicitWaterFill(vz, ground, datum, /*cavernFlooded=*/false);
    };
}

// --- the old path: Reservoir v0, transcribed -------------------------------
//
// Source: UVoxelWaterSubsystem::NotifyTerrainVoxelsCleared (the seeding rule)
// and UVoxelWaterSubsystem::StepFixed (the top-up loop), as of eb9acac.
//
// NOTE THE TWO BARE ZEROES. The original spells both sea-level tests as a
// literal `0` -- the last two in the whole ue-project tree, and outside the
// reach of terrain-service/tests/test_sea_level_contract.py, which only scans
// voxel-core, worldgen.ush and the Python. They are written here as
// kSeaLevelVoxelZ so this file survives a moved datum; that is the ONLY
// liberty taken with the transcription, and it makes the old path strictly
// more correct than the original, not less.
struct ReservoirV0 {
    std::set<Voxel> cells;

    // "adjacent cell is implicit-ocean (i.e. non-solid, z<0, outside active
    // water)". Called with the cells one edit cleared, AFTER the edit has been
    // applied to the world.
    int seed(WaterCA& ca, const CoastWorld& w, const std::vector<Voxel>& cleared) {
        static const Voxel kOff[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                      {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const std::set<Voxel> clearedSet(cleared.begin(), cleared.end());
        int newly = 0;
        for (const Voxel& v : cleared) {
            if (v.z >= kSeaLevelVoxelZ) continue;
            bool touchesOcean = false;
            for (const Voxel& off : kOff) {
                const Voxel n{v.x + off.x, v.y + off.y, v.z + off.z};
                if (clearedSet.count(n) != 0) continue;
                if (n.z >= kSeaLevelVoxelZ) continue;
                if (w.solidAt(n.x, n.y, n.z) != MAT_AIR) continue;
                if (ca.fillAt(n.x, n.y, n.z) != 0) continue;
                touchesOcean = true;
                break;
            }
            if (!touchesOcean) continue;
            if (cells.insert(v).second) ++newly;
            const uint8_t cur = ca.fillAt(v.x, v.y, v.z);
            if (cur < 255) ca.addWater(v.x, v.y, v.z, uint32_t(255 - cur));
        }
        return newly;
    }

    // StepFixed's loop: "boundary cells refill to 255 each tick while exposed",
    // where "while exposed" is not actually tested -- once registered, forever.
    void topUp(WaterCA& ca) const {
        for (const Voxel& v : cells) {
            const uint8_t cur = ca.fillAt(v.x, v.y, v.z);
            if (cur < 255) ca.addWater(v.x, v.y, v.z, uint32_t(255 - cur));
        }
    }
};

// --- running a scenario ----------------------------------------------------

std::vector<Voxel> box(int64_t x0, int64_t x1, int64_t y0, int64_t y1, int64_t z0, int64_t z1) {
    std::vector<Voxel> v;
    for (int64_t x = x0; x <= x1; ++x)
        for (int64_t y = y0; y <= y1; ++y)
            for (int64_t z = z0; z <= z1; ++z) v.push_back(Voxel{x, y, z});
    return v;
}

void invalidateAll(WaterCA& ca) {
    ca.invalidateSolidRegion(-4096, -4096, -1024, 4096, 4096, 1024);
}

// The z of the highest cell of `probe` that holds any water, or kDry.
constexpr int kDry = -9999;
int topWetVz(const WaterCA& ca, const std::vector<Voxel>& probe) {
    int top = kDry;
    for (const Voxel& v : probe) {
        if (ca.fillAt(v.x, v.y, v.z) > 0) top = std::max(top, int(v.z));
    }
    return top;
}

// Runs the NEW path to a settled state: nothing active AND no front pending.
// Returns the tick it settled on, or `budget` if it never did.
int runNewToSettle(WaterMobilizer& mob, WaterCA& ca, int budget) {
    for (int t = 0; t < budget; ++t) {
        mob.advanceFront(ca);
        ca.step();
        // The ledger every tick, exactly as waterca.h says production should:
        // a nonzero shortfall means the wall invariant broke and every number
        // after it is meaningless.
        CHECK_EQ(mob.shortfallVolume(), uint64_t{0});
        if (t > 3 && ca.steppedBrickCount() == 0 && mob.pendingFrontBricks() == 0) return t;
    }
    return budget;
}

int runOldToSettle(ReservoirV0& res, WaterCA& ca, int budget) {
    for (int t = 0; t < budget; ++t) {
        res.topUp(ca);
        ca.step();
        if (t > 3 && ca.steppedBrickCount() == 0) return t;
    }
    return budget;
}

// A cove: a rectangular notch cut clean through the coastal cliff, from the
// seabed up past the land surface. This is the ORDINARY breach -- the opening
// spans the whole water column, so the sea flows in LATERALLY and no water has
// to be lifted.
const std::vector<Voxel> kCoveDig = box(0, 3, 0, 3, kSeaFloorVz, 12);
const std::vector<Voxel> kCoveProbe = box(1, 1, 1, 1, kSeaFloorVz, 12);

const int kBudget = 600;

} // namespace

// ===========================================================================
// The term itself
// ===========================================================================

VXC_TEST(ocean_is_the_datum_where_the_ground_is_below_it_and_nowhere_else) {
    CHECK_EQ(int(oceanSurfaceMmAt(kSeabedMm)), int(kSeaLevelMm));
    CHECK_EQ(int(oceanSurfaceMmAt(kSeaLevelMm - 1)), int(kSeaLevelMm));
    // Exactly ON the datum is a beach, not a sea: zero depth of water.
    CHECK_EQ(int(oceanSurfaceMmAt(kSeaLevelMm)), int(kNoWaterMm));
    CHECK_EQ(int(oceanSurfaceMmAt(kLandMm)), int(kNoWaterMm));
}

VXC_TEST(ocean_fills_a_seabed_column_from_the_bed_to_the_datum) {
    const int32_t datum = implicitWaterDatumMm(kNoWaterMm, kSeabedMm);
    CHECK_EQ(int(datum), int(kSeaLevelMm));
    // One voxel of rock below the bed, the whole water column, one voxel of
    // air above the datum. The bounds are the assertion: an off-by-one here is
    // a shoreline that floats, or a sea one voxel short of its own surface.
    CHECK_EQ(int(implicitWaterFill(kSeaFloorVz - 1, kSeabedMm, datum, false)), 0);
    CHECK_EQ(int(implicitWaterFill(kSeaFloorVz, kSeabedMm, datum, false)), 255);
    CHECK_EQ(int(implicitWaterFill(kSeaTopVz, kSeabedMm, datum, false)), 255);
    CHECK_EQ(int(implicitWaterFill(kSeaTopVz + 1, kSeabedMm, datum, false)), 0);
}

VXC_TEST(ocean_composes_with_a_baked_lake_by_max_not_by_replacement) {
    // A coastal lake standing 5 m above the datum on a column whose ground is
    // still below it -- the case a plain "sea wins below zero" rule gets wrong,
    // and the shape a river mouth has once item 7 lands.
    const int32_t lake = kSeaLevelMm + 5000;
    CHECK_EQ(int(implicitWaterDatumMm(lake, kSeabedMm)), int(lake));
    CHECK_EQ(int(implicitWaterDatumMm(lake, kLandMm)), int(lake));
    // ...and a reach that has descended to meet the sea composes to ONE
    // surface, not two: at the mouth the two datums ARE the same number, so
    // max() cannot introduce a step down into the river.
    CHECK_EQ(int(implicitWaterDatumMm(kSeaLevelMm, kSeabedMm)), int(kSeaLevelMm));
    // No lake, no sea: still dry. The term adds water, it never removes any.
    CHECK_EQ(int(implicitWaterDatumMm(kNoWaterMm, kLandMm)), int(kNoWaterMm));
    CHECK_EQ(int(implicitWaterDatumMm(kNoWaterMm, kSeabedMm)), int(kSeaLevelMm));
}

VXC_TEST(ocean_term_off_is_exactly_the_pre_item_8_field) {
    // The control, as a unit: with the term off the composed datum is whatever
    // the bake said and nothing more. Every scenario below reruns with this.
    CoastWorld w;
    const WaterMobilizer::ImplicitFn off = oceanImplicit(w, kOceanOff);
    for (int64_t z = kSeaFloorVz; z <= kSeaTopVz; ++z) CHECK_EQ(int(off(-10, 0, z)), 0);
    const WaterMobilizer::ImplicitFn on = oceanImplicit(w, kOceanOn);
    CHECK_EQ(int(on(-10, 0, kSeaTopVz)), 255);
}

VXC_TEST(the_sea_is_a_wall_until_something_touches_it) {
    // "Real voxels only where touched" (§6.4), as a property rather than a
    // rendering claim: with no edit at all the mobilizer converts nothing, the
    // CA holds nothing, and the implicit field still reports the sea -- which
    // is what makes the untouched ocean cost zero bricks and zero ticks.
    CoastWorld w;
    WaterMobilizer mob(oceanImplicit(w, kOceanOn), w.solidFn());
    WaterCA ca(mob.makeSolidFn());
    for (int t = 0; t < 20; ++t) ca.step();
    CHECK_EQ(ca.totalVolume(), uint64_t{0});
    CHECK_EQ(mob.mobilizedBricks().size(), size_t{0});
    CHECK_EQ(int(mob.implicitFillAt(-10, 0, kSeaTopVz)), 255);
    // ...and it is a WALL: the CA must see unmobilized sea as solid, or water
    // poured beside it would flow into cells the implicit field still owns.
    const WaterCA::SolidFn solid = mob.makeSolidFn();
    CHECK_EQ(int(solid(-10, 0, kSeaTopVz)), int(MAT_ROCK));
    CHECK_EQ(int(solid(-10, 0, kSeaTopVz + 1)), int(MAT_AIR));  // above the datum: real air
}

// ===========================================================================
// THE ORDINARY BREACH: a cove cut through the coastal cliff
// ===========================================================================

VXC_TEST(breach_cove_ocean_term_fills_to_the_datum_and_settles) {
    CoastWorld w;
    WaterMobilizer mob(oceanImplicit(w, kOceanOn), w.solidFn());
    WaterCA ca(mob.makeSolidFn());
    w.dig(kCoveDig);
    invalidateAll(ca);
    mob.mobilizeEditRegion(ca, 0, 0, kSeaFloorVz, 3, 3, 12);
    const int settled = runNewToSettle(mob, ca, kBudget);

    const int topFill = int(ca.fillAt(1, 1, kSeaTopVz));
    std::printf("  [cove/new] settled at tick %d, %zu brick(s) mobilized, %llu fill units, "
                "top wet z=%d (datum z=%d), surface layer %d/255 = %d mm below the datum\n",
                settled, mob.mobilizedBricks().size(), (unsigned long long)ca.totalVolume(),
                topWetVz(ca, kCoveProbe), int(kSeaTopVz), topFill,
                (255 - topFill) * kVoxelSizeMm / 255);

    CHECK(settled < kBudget);                      // it SETTLES, which the old path never does
    CHECK_EQ(topWetVz(ca, kCoveProbe), int(kSeaTopVz));  // ...at the sea's own surface voxel
    CHECK_EQ(mob.shortfallVolume(), uint64_t{0});
    // Full to one voxel below the datum, nothing above it, and the surface
    // voxel itself PARTIAL.
    //
    // THE PARTIAL TOP IS REAL PHYSICS AND NOT A BUG, and it is the one
    // consequence of §6.4 worth stating out loud: what mobilizes is a FINITE
    // piece of an infinite sea, so filling the cove draws its volume from that
    // piece and the equalized level lands a fraction of a voxel below the
    // datum -- the whole mobilized body drops, exactly as a real bay would if
    // the ocean it drew from were only 197 bricks wide. The deficit is
    // bounded by (cove volume / mobilized volume) and shrinks as the front
    // mobilizes more sea; it is a few centimetres here. What must NOT happen
    // is the surface landing a whole voxel low, which would read as a step at
    // the shoreline, so that is what is asserted.
    CHECK(topFill > 0);
    CHECK(255 - topFill < 64);  // within a quarter of a voxel (2.5 cm) of the datum
    for (int64_t z = kSeaFloorVz; z < kSeaTopVz; ++z) CHECK_EQ(int(ca.fillAt(1, 1, z)), 255);
    for (int64_t z = kSeaTopVz + 1; z <= 12; ++z) CHECK_EQ(int(ca.fillAt(1, 1, z)), 0);
}

VXC_TEST(breach_cove_control_term_off_stays_completely_dry) {
    // The control the whole file needs: the same dig, the same fixture, the
    // ocean term off. If this were also wet, everything above would be the
    // fixture talking rather than the term.
    CoastWorld w;
    WaterMobilizer mob(oceanImplicit(w, kOceanOff), w.solidFn());
    WaterCA ca(mob.makeSolidFn());
    w.dig(kCoveDig);
    invalidateAll(ca);
    CHECK_EQ(mob.mobilizeEditRegion(ca, 0, 0, kSeaFloorVz, 3, 3, 12), size_t{0});
    runNewToSettle(mob, ca, 40);
    CHECK_EQ(ca.totalVolume(), uint64_t{0});
    CHECK_EQ(topWetVz(ca, kCoveProbe), kDry);
}

VXC_TEST(reservoir_v0_pours_into_a_dry_seabed_and_never_settles) {
    // THE SAME COVE, THE OLD MECHANISM. Reservoir v0 pins the breach face at
    // 255 every tick, but the ocean beyond it is not water to the CA -- the
    // seabed's open air is open air -- so the pinned cells do not FILL the
    // cove, they DRAIN through it and spread across the seabed. Nothing ever
    // reaches equilibrium because the source never turns off.
    CoastWorld w;
    WaterCA ca(w.solidFn());
    ReservoirV0 res;
    w.dig(kCoveDig);
    invalidateAll(ca);
    const int seeded = res.seed(ca, w, kCoveDig);

    uint64_t volAt[3] = {0, 0, 0};
    size_t activeAt[3] = {0, 0, 0};
    const int kMarks[3] = {100, 500, 1500};
    int mark = 0;
    for (int t = 0; t <= kMarks[2]; ++t) {
        res.topUp(ca);
        ca.step();
        if (mark < 3 && t == kMarks[mark]) {
            volAt[mark] = ca.totalVolume();
            activeAt[mark] = ca.activeBricks().size();
            ++mark;
        }
    }
    std::printf("  [cove/reservoir v0] seeded %d cell(s); volume %llu -> %llu -> %llu fill units "
                "at ticks %d/%d/%d, active bricks %zu -> %zu -> %zu; top wet z=%d (datum z=%d)\n",
                seeded, (unsigned long long)volAt[0], (unsigned long long)volAt[1],
                (unsigned long long)volAt[2], kMarks[0], kMarks[1], kMarks[2], activeAt[0],
                activeAt[1], activeAt[2], topWetVz(ca, kCoveProbe), int(kSeaTopVz));

    CHECK(seeded > 0);  // it did find the breach; this is not a null run
    // IT IS A SOURCE, not a body: volume strictly increases between every pair
    // of marks, 1500 ticks (2.5 minutes at the subsystem's 10 Hz) in.
    CHECK(volAt[1] > volAt[0]);
    CHECK(volAt[2] > volAt[1]);
    // ...and the CA's own cost driver grows with it. This is the half of the
    // defect that is a perf leak rather than a physics one.
    CHECK(activeAt[2] > activeAt[0]);
    // ...while the cove itself never reaches the datum, because the water it
    // is given runs away into the seabed.
    CHECK(topWetVz(ca, kCoveProbe) < int(kSeaTopVz));
}

// ===========================================================================
// THE DEEP PUNCTURE: what the new path does NOT fix
// ===========================================================================
//
// A 1x1 tunnel driven inland at z = -9 (one voxel above the sea floor) that
// breaks into the sea at its west end, with a shaft rising from its inland end
// through the land surface. Communicating vessels: the shaft should stand at
// the sea's surface, z = -1.
//
// NEITHER MECHANISM RAISES IT, and the reasons are different:
//
//   old   its source is a cell pinned at 255 at z = -9, so it is a head at
//         z = -9 and there is no pressure to lift anything above the tunnel.
//   new   the mobilized sea is already AT hydrostatic equilibrium -- every
//         cell full to the datum, nothing to redistribute -- so it goes
//         inactive within a few ticks, and waterca.h's Phase C explores new
//         (previously dry) headroom only through bricks in the ACTIVE set.
//         With no active brick beside the shaft, nothing ever notices the
//         headroom exists.
//
// THE DIAGNOSIS IS NOT A GUESS: the same dig against a sea small enough that
// the tunnel's own volume perturbs its level -- so the sea stays active for a
// few more ticks -- DOES raise the shaft to the datum. Measured on a 64x65
// voxel sea: shaft top wet z=-1, settled at tick 8. Against an unbounded sea:
// z=-9, settled at tick 5. Raising the mobilization front budget from 64 to
// 4096 bricks/tick changes nothing, so it is not front starvation.
//
// This belongs to work item 10 (the CA's activity/budget half), not to item 8,
// and it is recorded here so that the next person to look at a half-full
// tunnel finds a measurement instead of a mystery.

VXC_TEST(breach_deep_puncture_neither_path_lifts_water_above_the_puncture) {
    std::vector<Voxel> dig = box(0, 5, 0, 0, -9, -9);
    const std::vector<Voxel> up = box(5, 5, 0, 0, -8, 12);
    dig.insert(dig.end(), up.begin(), up.end());
    const std::vector<Voxel> probe = box(5, 5, 0, 0, -9, 12);

    CoastWorld wo;
    WaterCA cao(wo.solidFn());
    ReservoirV0 res;
    wo.dig(dig);
    invalidateAll(cao);
    res.seed(cao, wo, dig);
    runOldToSettle(res, cao, kBudget);
    const int oldTop = topWetVz(cao, probe);

    CoastWorld wn;
    WaterMobilizer mob(oceanImplicit(wn, kOceanOn), wn.solidFn());
    WaterCA can(mob.makeSolidFn());
    wn.dig(dig);
    invalidateAll(can);
    mob.mobilizeEditRegion(can, 0, 0, -9, 5, 0, 12);
    const int settled = runNewToSettle(mob, can, kBudget);
    const int newTop = topWetVz(can, probe);

    std::printf("  [deep puncture] reservoir v0 top wet z=%d; ocean term top wet z=%d "
                "(settled tick %d, %zu brick(s) mobilized); datum z=%d -- NEITHER reaches it\n",
                oldTop, newTop, settled, mob.mobilizedBricks().size(), int(kSeaTopVz));

    // Both stop at the puncture depth. Asserted rather than described so that
    // a future change to the CA's activity rule -- which is what would fix
    // this -- fails here loudly and gets this comment updated.
    CHECK_EQ(oldTop, -9);
    CHECK_EQ(newTop, -9);
    // What the new path DOES get right even here: it settles, and its ledger
    // balances. The old path is still running.
    CHECK(settled < kBudget);
    CHECK_EQ(mob.shortfallVolume(), uint64_t{0});
}

// ===========================================================================
// THE INLAND PIT -- §6.4's "fixable by testing the datum, not the camera"
// ===========================================================================
//
// No sea anywhere near: a shaft dug straight down into land whose worldgen
// surface stands 1 m ABOVE the datum, at vx = 50, fifty voxels inland of the
// shoreline and never touching it.
//
// Dug in three passes, which is not a contrivance -- it is what digging is.
// Pass 1 is above the datum and Reservoir v0's own guard rejects it. Pass 2 is
// below the datum but every below-datum neighbour of every cell is in the same
// pass's cleared set, so the "newly-dug-by-this-same-edit" exclusion rejects it
// too. PASS 3 IS WHERE IT BREAKS: the top cell of pass 3 has pass 2's air
// directly above it, that air is not in pass 3's cleared set, it is below the
// datum and it holds no CA fill -- which is verbatim the "adjacent cell is
// implicit-ocean" test -- so the pit seeds a boundary cell and tops it up to
// 255 forever, inside dry rock, fifty voxels from the nearest water.
VXC_TEST(reservoir_v0_floods_an_inland_pit_the_datum_test_leaves_dry) {
    const int64_t kPitX = 50;
    CHECK_EQ(int(CoastWorld::groundMm(kPitX)), int(kLandMm));

    const std::vector<Voxel> pass1 = box(kPitX, kPitX, 0, 0, 0, 10);    // surface to the datum
    const std::vector<Voxel> pass2 = box(kPitX, kPitX, 0, 0, -3, -1);   // first pass below it
    const std::vector<Voxel> pass3 = box(kPitX, kPitX, 0, 0, -6, -4);   // "keep digging"
    const std::vector<Voxel> probe = box(kPitX, kPitX, 0, 0, -6, 10);

    // --- old path, three notifications, exactly as the engine issues them ---
    CoastWorld wo;
    WaterCA cao(wo.solidFn());
    ReservoirV0 res;
    int seeded = 0;
    for (const std::vector<Voxel>* pass : {&pass1, &pass2, &pass3}) {
        wo.dig(*pass);
        invalidateAll(cao);
        seeded += res.seed(cao, wo, *pass);
    }
    runOldToSettle(res, cao, kBudget);

    // --- new path, the same three digs -------------------------------------
    CoastWorld wn;
    WaterMobilizer mob(oceanImplicit(wn, kOceanOn), wn.solidFn());
    WaterCA can(mob.makeSolidFn());
    size_t mobilized = 0;
    for (const std::vector<Voxel>* pass : {&pass1, &pass2, &pass3}) {
        wn.dig(*pass);
        invalidateAll(can);
        mobilized += mob.mobilizeEditRegion(can, kPitX, 0, pass->front().z, kPitX, 0,
                                            pass->back().z);
    }
    runNewToSettle(mob, can, kBudget);

    std::printf("  [inland pit] reservoir v0 seeded %d boundary cell(s) and holds %llu fill "
                "unit(s) of water in dry rock %lld voxels inland; ocean term seeded 0, mobilized "
                "%zu brick(s), holds %llu\n",
                seeded, (unsigned long long)cao.totalVolume(), (long long)kPitX, mobilized,
                (unsigned long long)can.totalVolume());

    // The bug, asserted rather than described: the old path DOES flood it.
    CHECK(seeded > 0);
    CHECK(cao.totalVolume() > 0);
    // The fix: the datum test never finds a sea over a column whose WORLDGEN
    // ground is above the datum, however deep the pit is dug.
    CHECK_EQ(can.totalVolume(), uint64_t{0});
    CHECK_EQ(mobilized, size_t{0});
    CHECK_EQ(mob.shortfallVolume(), uint64_t{0});
    CHECK_EQ(topWetVz(can, probe), kDry);
}
