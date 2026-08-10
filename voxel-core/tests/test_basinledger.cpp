// The basin volume ledger (water re-architecture Phase 2): the hypsometry a v1
// client reconstructs, the volume<->level pair, conservation under credit /
// debit / spill, persistence, and one standalone end-to-end run of the whole
// scalar path -- faucet -> routing graph -> basin -> spillway -> downstream --
// with every unit accounted for at both ends of every hand-off.
//
// See voxelcore/basinledger.h for the design writeup, and
// docs/water-rearchitecture-plan-2026-08-09.md §4/§5 for why the authority is a
// scalar at all.

#include "voxelcore/basinledger.h"

#include <map>
#include <vector>

#include "voxelcore/rivernet.h"
#include "vxctest.h"

using namespace vxc;

namespace {

constexpr int32_t kTestPixelMm = 1000; // 1 m pixels: every volume below is hand-checkable
constexpr uint32_t kTestTilePx = 64;

// ---------------------------------------------------------------------------
// A synthetic basin: a stepped square bowl, 16x16 pixels, in tile (0,0).
// ---------------------------------------------------------------------------
//
//   lattice (what the EXTENT is filled on): 0 mm over the 10x10 core
//     (pixels 3..12 on both axes), 10,000 mm outside it.
//   spline  (what the HYPSOMETRY is integrated on): 50 mm over the inner 4x4
//     (pixels 6..9), 250 mm over the rest of the core, 10,000 mm outside.
//
// THE TWO GROUNDS DISAGREE ON PURPOSE. basinledger.h's "WHICH GROUND" section
// says the extent must come from the control lattice (the plane the bake
// measured its components on) and the depth curve from the reconstructed
// spline (the surface the drawn waterline stands on). A fixture where both
// accessors answered the same thing could not tell a correct implementation
// from one that used the lattice for both -- which is the mistake that has been
// made three times in this codebase and that the split exists to prevent.
// `basin_hypsometry_floor_is_the_spline_not_the_lattice` is the assertion.
//
// Hand-computed reference, in ledger units (255 per 10 cm voxel, i.e. per
// 1e6 mm^3), with a 1 m^2 cell:
//   V(200 mm, the baked equilibrium) = 16 cells * 150 mm  =   2,400 mm*cells
//                                    = 2.4e9 mm^3         = 612,000 units
//   V(250 mm)                        = 16 cells * 200 mm  =   3,200 mm*cells
//                                                          = 816,000 units
//   V(500 mm, the sill)  = 16*450 + 84*250 =  28,200 mm*cells = 7,191,000 units
//   capacity to spill    = 7,191,000 - 612,000            = 6,579,000 units
class SteppedBowlTerrain final : public IBasinTerrain {
public:
    SteppedBowlTerrain() {
        row_.basinId = 0;
        row_.seedX = 8;
        row_.seedY = 8;
        row_.bboxX0 = 0;
        row_.bboxY0 = 0;
        row_.bboxX1 = 15;
        row_.bboxY1 = 15;
        row_.outletX = 15; // the saddle, on the bbox's east edge
        row_.outletY = 8;
        row_.spillMm = 500;
        row_.surfaceMm = 200;
        row_.kind = kBasinLakeTerminal;
    }

    BasinEntry& row() { return row_; }

    int32_t pixelSizeMm() const override { return kTestPixelMm; }
    const BasinEntry* basinRow(BasinId id) override {
        return (id == BasinId::fromTile(0, 0, 0)) ? &row_ : nullptr;
    }
    bool prewarmBasin(BasinId id) override { return basinRow(id) != nullptr; }

    int32_t latticeElevationMm(BasinId, int32_t lx, int32_t ly) override {
        return inCore(lx, ly) ? 0 : 10000;
    }
    int32_t groundMm(BasinId, int32_t lx, int32_t ly) override {
        if (!inCore(lx, ly)) return 10000;
        return inInner(lx, ly) ? 50 : 250;
    }

private:
    static bool inCore(int32_t lx, int32_t ly) {
        return lx >= 3 && lx <= 12 && ly >= 3 && ly <= 12;
    }
    static bool inInner(int32_t lx, int32_t ly) {
        return lx >= 6 && lx <= 9 && ly >= 6 && ly <= 9;
    }
    BasinEntry row_{};
};

BasinId theBasin() { return BasinId::fromTile(0, 0, 0); }

struct Fixture {
    SteppedBowlTerrain terrain;
    ClientHypsometryProvider capacity{terrain};
    BasinLedger ledger{capacity};

    Fixture() { capacity.setTilePixels(kTestTilePx); }
};

// ---------------------------------------------------------------------------
// A synthetic baked water + flow plane for the end-to-end run.
// ---------------------------------------------------------------------------
//
// Two straight channels along py == 8, both descending eastwards, with the
// basin's 16-pixel body between them:
//
//   FEEDER   px -14..-1, water surface 8000 mm down to 6700 mm. Its terminal
//            node is the graph's outlet on that side, and the test treats what
//            leaves there as delivered INTO the basin -- the plan's "basin
//            despawn sink -> ledger credit", with the despawn replaced by the
//            graph outlet since there are no particles yet.
//   SPILLWAY px 16..30, water surface 4900 mm down to 3500 mm, i.e. below the
//            sill at 500 mm... no: below the FEEDER, and physically downstream
//            of the saddle. Its terminal node is the sea.
//
// The basin's outlet pixel (15, 8) sits one pixel west of the spillway's head,
// so `nearestSegmentToVoxel` finds the spillway's first segment and the spill
// lands where a sill faucet would.
class TwoChannelWaterSource final : public IBakedWaterSource {
public:
    int32_t pixelSizeMm() const override { return kTestPixelMm; }

    bool waterAt(int64_t px, int64_t py, int32_t& outSurfaceMm, bool& outWet) override {
        if (!isChannel(px, py)) {
            outSurfaceMm = kNoWaterMm;
            outWet = false;
            return true;
        }
        outWet = true;
        outSurfaceMm = surfaceOf(px);
        return true;
    }

    bool flowAt(int64_t px, int64_t py, uint8_t& outFlow) override {
        if (!isChannel(px, py)) {
            outFlow = 0;
            return true;
        }
        outFlow = uint8_t(kFlowBitChannel | 10); // log2 catchment == 10 -> 1024 m^2
        return true;
    }

    static bool isFeeder(int64_t px, int64_t py) { return py == 8 && px >= -14 && px <= -1; }
    static bool isSpillway(int64_t px, int64_t py) { return py == 8 && px >= 16 && px <= 30; }

private:
    static bool isChannel(int64_t px, int64_t py) { return isFeeder(px, py) || isSpillway(px, py); }
    static int32_t surfaceOf(int64_t px) {
        // Strictly descending eastwards in both channels, so every D8 edge is a
        // strict decrease of the builder's total order and the two chains are
        // unambiguous.
        return px <= -1 ? int32_t(8000 - (px + 14) * 100) : int32_t(4900 - (px - 16) * 100);
    }
};

// ---------------------------------------------------------------------------
// BASIN TABLE v2 fixtures
// ---------------------------------------------------------------------------

// A PARABOLOID bowl, 40x40 pixels: ground = 5 mm per squared half-pixel from
// the centre, so the count of cells at or below h -- the wetted area A(h) --
// grows in proportion to (h - floor) and vanishes at the floor.
//
// That is EXACTLY the shape `BakedCapacityProvider` models, and this fixture is
// here to say what the baked curve costs when its assumption holds. The stepped
// bowl above is the opposite case (a flat bottom, so A(h) is a step) and says
// what it costs when the assumption does not. Both numbers are asserted below
// rather than described, because "close enough" is not a measurement.
//
// The two grounds AGREE here, unlike SteppedBowlTerrain, and deliberately: the
// lattice/spline split is already asserted by
// `basin_hypsometry_floor_is_the_spline_not_the_lattice`, and a fixture testing
// the CURVE should not also be varying the surface the curve is built on.
class ParaboloidBowlTerrain final : public IBasinTerrain {
public:
    ParaboloidBowlTerrain() {
        row_.basinId = 0;
        row_.seedX = 19;
        row_.seedY = 19;
        row_.bboxX0 = 0;
        row_.bboxY0 = 0;
        row_.bboxX1 = 39;
        row_.bboxY1 = 39;
        row_.outletX = 39;
        row_.outletY = 20;
        row_.spillMm = 4000;
        row_.surfaceMm = 1000;
        row_.kind = kBasinLakeTerminal;
    }

    // ground(x, y) == 5 * ((2x-39)^2 + (2y-39)^2). Doubling the coordinate is
    // what keeps a centre at (19.5, 19.5) exact in integers -- the bowl has to
    // be symmetric about the cell grid or its A(h) develops a staircase that
    // has nothing to do with the model under test.
    static int32_t groundAt(int32_t lx, int32_t ly) {
        const int64_t dx = 2 * int64_t(lx) - 39, dy = 2 * int64_t(ly) - 39;
        return int32_t(5 * (dx * dx + dy * dy));
    }

    BasinEntry& row() { return row_; }
    int32_t pixelSizeMm() const override { return kTestPixelMm; }
    const BasinEntry* basinRow(BasinId id) override {
        return (id == BasinId::fromTile(0, 0, 0)) ? &row_ : nullptr;
    }
    bool prewarmBasin(BasinId id) override { return basinRow(id) != nullptr; }
    int32_t latticeElevationMm(BasinId, int32_t lx, int32_t ly) override {
        return groundAt(lx, ly);
    }
    int32_t groundMm(BasinId, int32_t lx, int32_t ly) override { return groundAt(lx, ly); }

private:
    BasinEntry row_{};
};

constexpr uint32_t kV2TilePx = 64;

// `bake.basins.global_basin_id`'s packing, written out here rather than called,
// so a change to the C++ side that silently stopped agreeing with the bake
// would fail this file rather than pass it.
uint64_t packGlobalId(int32_t worldPx, int32_t worldPy) {
    return kBasinIdTag | (uint64_t(int64_t(worldPx) + kBasinIdAxisBias) << 31) |
           uint64_t(int64_t(worldPy) + kBasinIdAxisBias);
}

// A synthetic v2 table: tiles that are resident, tiles that are not, and a
// count of how often each was asked for.
class SyntheticV2Table final : public IBasinTableV2 {
public:
    int32_t pixelSizeMm() const override { return kTestPixelMm; }
    uint32_t tilePixels() const override { return kV2TilePx; }

    const std::vector<BasinEntry>* rowsForTile(int32_t tx, int32_t ty) override {
        ++lookups;
        const auto it = tiles_.find(key(tx, ty));
        return it == tiles_.end() ? nullptr : &it->second;
    }

    void addRow(int32_t tx, int32_t ty, const BasinEntry& r) {
        tiles_[key(tx, ty)].push_back(r);
    }
    void dropTile(int32_t tx, int32_t ty) { tiles_.erase(key(tx, ty)); }

    uint64_t lookups = 0;

private:
    static uint64_t key(int32_t tx, int32_t ty) {
        return (uint64_t(uint32_t(tx)) << 32) | uint64_t(uint32_t(ty));
    }
    std::map<uint64_t, std::vector<BasinEntry>> tiles_;
};

// One v2 row, with every v2 field filled: the whole point of the format is that
// a client never has to invent one of these, so a fixture that left some at
// zero would be testing a tile that cannot ship.
BasinEntry v2Row(uint16_t localId, int32_t floorPx, int32_t floorPy, int32_t wx0, int32_t wy0,
                 int32_t wx1, int32_t wy1, int32_t floorMm, int32_t surfaceMm, int32_t spillMm,
                 uint64_t capacityLitres, int32_t outWx, int32_t outWy, bool crosses) {
    BasinEntry r{};
    r.basinId = localId;
    r.seedX = uint16_t(floorPx & (kV2TilePx - 1));
    r.seedY = uint16_t(floorPy & (kV2TilePx - 1));
    r.bboxX0 = 0;
    r.bboxY0 = 0;
    r.bboxX1 = uint16_t(kV2TilePx - 1);
    r.bboxY1 = uint16_t(kV2TilePx - 1);
    // TILE-CLAMPED, and wrong on purpose for a spanning basin -- that is what
    // `worldOutletX/Y` exists to replace and what the spillway test asserts.
    r.outletX = uint16_t(clampi64(outWx, 0, int64_t(kV2TilePx) - 1));
    r.outletY = uint16_t(clampi64(outWy, 0, int64_t(kV2TilePx) - 1));
    r.spillMm = spillMm;
    r.surfaceMm = surfaceMm;
    r.kind = kBasinLakeTerminal;
    r.tableVersion = kBasinTableVersionV2;
    r.globalId = packGlobalId(floorPx, floorPy);
    r.capacityLitres = capacityLitres;
    r.floorMm = floorMm;
    r.worldX0 = wx0;
    r.worldY0 = wy0;
    r.worldX1 = wx1;
    r.worldY1 = wy1;
    r.worldOutletX = outWx;
    r.worldOutletY = outWy;
    r.spanFlags = crosses ? kBasinSpanCrossesTile : uint8_t(0);
    return r;
}

// The level a provider puts a basin at for a given delta -- the ledger's own
// `resolveLevel`, spelled out so the two providers can be asked the identical
// question with nothing in between.
bool levelForDelta(IBasinCapacityProvider& p, BasinId id, int64_t delta, int32_t& outMm) {
    int32_t floorMm = 0, eqMm = 0, spillMm = 0;
    int64_t atEq = 0;
    if (!p.levelsMm(id, floorMm, eqMm, spillMm)) return false;
    if (!p.volumeAtLevelUnits(id, eqMm, atEq)) return false;
    const int64_t target = atEq + delta;
    return p.levelAtVolumeUnits(id, target > 0 ? target : 0, outMm);
}

} // namespace

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

VXC_TEST(basinid_tile_local_round_trip_and_tag) {
    const BasinId a = BasinId::fromTile(0, 0, 0);
    CHECK(a.valid());
    CHECK(a.isTileLocal());
    CHECK_EQ(a.tileX(), 0);
    CHECK_EQ(a.tileY(), 0);
    CHECK_EQ(a.localId(), uint16_t(0));

    const BasinId b = BasinId::fromTile(-1234, 5678, 65535);
    CHECK(b.valid());
    CHECK(b.isTileLocal());
    CHECK_EQ(b.tileX(), -1234);
    CHECK_EQ(b.tileY(), 5678);
    CHECK_EQ(b.localId(), uint16_t(65535));

    // Distinct basins never alias -- the whole point of the packing.
    CHECK(!(BasinId::fromTile(1, 0, 0) == BasinId::fromTile(0, 1, 0)));
    CHECK(!(BasinId::fromTile(0, 0, 1) == BasinId::fromTile(0, 0, 2)));
}

VXC_TEST(basinid_out_of_range_tile_refuses_rather_than_folding) {
    // A coordinate past the 22-bit field must become kNoBasin, NOT wrap onto a
    // different tile's basin. Folding is the failure that looks like data.
    CHECK(!BasinId::fromTile(1 << 22, 0, 0).valid());
    CHECK(!BasinId::fromTile(0, -(1 << 22), 0).valid());
    CHECK_EQ(BasinId::fromTile(1 << 22, 0, 0).v, kNoBasin.v);
}

VXC_TEST(basinid_global_ids_are_disjoint_from_tile_local_ones) {
    const BasinId g = BasinId::fromGlobal(12345);
    CHECK(g.valid());
    CHECK(!g.isTileLocal());
    // 0 is kNoBasin, and the tag bit is reserved: both are refused so a v2 id
    // can never be mistaken for a tile-local key.
    CHECK(!BasinId::fromGlobal(0).valid());
    CHECK(!BasinId::fromGlobal(uint64_t(1) << 63).valid());
}

// ---------------------------------------------------------------------------
// The reconstructed hypsometry
// ---------------------------------------------------------------------------

VXC_TEST(basin_hypsometry_matches_the_hand_computed_bowl) {
    Fixture f;
    const BasinHypsometry* h = f.capacity.curveFor(theBasin());
    CHECK(h != nullptr);
    if (h == nullptr) return;

    CHECK_EQ(h->cellCount, int64_t(100)); // the 10x10 lattice core
    CHECK_EQ(h->equilibriumMm, 200);
    CHECK_EQ(h->spillMm, 500);
    CHECK_EQ(h->cellAreaMm2, int64_t(kTestPixelMm) * int64_t(kTestPixelMm));

    CHECK_EQ(h->unitsAtLevel(50), int64_t(0));        // at the floor
    CHECK_EQ(h->unitsAtLevel(0), int64_t(0));         // below it
    CHECK_EQ(h->unitsAtLevel(200), int64_t(612000));  // the baked equilibrium
    CHECK_EQ(h->unitsAtLevel(250), int64_t(816000));  // the inner shelf's rim
    CHECK_EQ(h->unitsAtLevel(500), int64_t(7191000)); // the sill
    CHECK_EQ(h->equilibriumUnits, int64_t(612000));
    CHECK_EQ(h->spillUnits, int64_t(7191000));

    // SATURATES at the sill rather than extrapolating: above the saddle the
    // basin is not a container, and a curve that kept climbing would let the
    // ledger hold water the spillway should already have taken.
    CHECK_EQ(h->unitsAtLevel(100000), int64_t(7191000));
}

VXC_TEST(basin_hypsometry_floor_is_the_spline_not_the_lattice) {
    // THE GROUND-TRUTH ASSERTION. The fixture's lattice bottoms out at 0 mm and
    // its spline at 50 mm; an implementation that integrated A(h) on the
    // lattice -- which is what `FineTileSampler::elevationMm` returns, and what
    // lakes.h:101's comment describes -- would report 0 here and would place a
    // credited lake 50 mm low on this fixture and up to 5.6 m low on the real
    // world (tilestore.h:1096-1098).
    Fixture f;
    const BasinHypsometry* h = f.capacity.curveFor(theBasin());
    CHECK(h != nullptr);
    if (h == nullptr) return;
    CHECK_EQ(h->floorMm, 50);

    // And the EXTENT still came from the lattice: the spline would admit the
    // same 100 cells here, but the count proves the fill ran at the SILL rather
    // than at the equilibrium surface -- a fill at 200 mm on the lattice admits
    // the same core, so the discriminating fact is the cell count against the
    // core's size, which is what this pins.
    CHECK_EQ(h->cellCount, int64_t(100));
}

VXC_TEST(basin_level_and_volume_are_exact_inverses) {
    Fixture f;
    const BasinHypsometry* h = f.capacity.curveFor(theBasin());
    CHECK(h != nullptr);
    if (h == nullptr) return;

    // The contract from IBasinCapacityProvider: levelAtVolumeUnits gives the
    // HIGHEST level whose volume is <= units, so round-tripping never drifts
    // however many times a caller does it.
    for (int64_t units : {int64_t(0), int64_t(1), int64_t(612000), int64_t(816000),
                          int64_t(3000000), int64_t(7190999), int64_t(7191000)}) {
        const int32_t level = h->levelAtUnits(units);
        CHECK(h->unitsAtLevel(level) <= units);
        if (level < h->spillMm) CHECK(h->unitsAtLevel(level + 1) > units);
        CHECK_EQ(h->levelAtUnits(h->unitsAtLevel(level)), level);
    }

    // The two hand-computed waypoints, exactly.
    CHECK_EQ(h->levelAtUnits(612000), 200);
    CHECK_EQ(h->levelAtUnits(816000), 250);
    CHECK_EQ(h->levelAtUnits(7191000), 500);
}

// ---------------------------------------------------------------------------
// Credit / debit / spill
// ---------------------------------------------------------------------------

VXC_TEST(basin_credit_is_monotone_and_conserves_under_pure_inflow) {
    Fixture f;
    const BasinId id = theBasin();

    int64_t prevDelta = 0, prevLevel = 0;
    prevLevel = f.ledger.levelMmFor(id, f.terrain.row().surfaceMm);
    CHECK_EQ(prevLevel, 200); // untouched basin stands at the baked equilibrium

    int64_t injected = 0;
    for (int i = 0; i < 20; ++i) {
        const int64_t accepted = f.ledger.credit(id, 100000);
        CHECK_EQ(accepted, int64_t(100000));
        injected += accepted;

        const int64_t delta = f.ledger.deltaUnits(id);
        CHECK(delta >= prevDelta); // monotone: pure inflow never lowers a lake
        prevDelta = delta;

        const int32_t level = f.ledger.levelMmFor(id, f.terrain.row().surfaceMm);
        CHECK(level >= prevLevel);
        prevLevel = level;

        CHECK(f.ledger.conserves());
        CHECK_EQ(f.ledger.sumOfDeltas(), f.ledger.recomputeSumOfDeltas());
    }

    // 2,000,000 units is well under the 6,579,000 headroom, so nothing spilled
    // and every unit is still in the lake.
    CHECK_EQ(f.ledger.totalCredited(), injected);
    CHECK_EQ(f.ledger.totalSpilled(), int64_t(0));
    CHECK_EQ(f.ledger.spillEvents(), uint64_t(0));
    CHECK_EQ(f.ledger.deltaUnits(id), injected);

    // And the level is exactly what the curve says for the ledgered volume --
    // "rises by exactly the ledgered volume", stated as the equality it is.
    const BasinHypsometry* h = f.capacity.curveFor(id);
    CHECK(h != nullptr);
    if (h != nullptr) {
        CHECK_EQ(f.ledger.levelMmFor(id, f.terrain.row().surfaceMm),
                 h->levelAtUnits(h->equilibriumUnits + injected));
    }
}

VXC_TEST(basin_spill_routes_exactly_the_excess_and_nothing_more) {
    Fixture f;
    const BasinId id = theBasin();

    int64_t headroom = 0;
    CHECK(f.ledger.capacityToSpillUnits(id, headroom));
    CHECK_EQ(headroom, int64_t(6579000));

    // One unit short of the sill: no spill, and the headroom is exactly 1.
    CHECK_EQ(f.ledger.credit(id, headroom - 1), headroom - 1);
    CHECK_EQ(f.ledger.spillEvents(), uint64_t(0));
    int64_t left = 0;
    CHECK(f.ledger.capacityToSpillUnits(id, left));
    CHECK_EQ(left, int64_t(1));

    // Now overfill by a known amount. Exactly the excess leaves; the basin
    // sits exactly at the sill; nothing is created or destroyed.
    CHECK_EQ(f.ledger.credit(id, 1 + 250000), int64_t(1 + 250000));
    CHECK_EQ(f.ledger.deltaUnits(id), headroom);
    CHECK_EQ(f.ledger.totalSpilled(), int64_t(250000));
    CHECK_EQ(f.ledger.spillEvents(), uint64_t(1));
    CHECK(f.ledger.conserves());
    CHECK_EQ(f.ledger.levelMmFor(id, f.terrain.row().surfaceMm), 500); // at the saddle

    const std::vector<BasinSpillEvent>& queued = f.ledger.pendingSpill();
    CHECK_EQ(queued.size(), size_t(1));
    if (!queued.empty()) {
        CHECK_EQ(queued[0].units, int64_t(250000));
        CHECK_EQ(queued[0].spillMm, 500);
        // outlet pixel (15,8) at 1 m/px, centred, in 10 cm voxels.
        CHECK_EQ(queued[0].outletVx, int64_t(155));
        CHECK_EQ(queued[0].outletVy, int64_t(85));
    }

    // A basin already at the sill spills EVERYTHING it is credited.
    CHECK_EQ(f.ledger.credit(id, 7777), int64_t(7777));
    CHECK_EQ(f.ledger.deltaUnits(id), headroom);
    CHECK_EQ(f.ledger.totalSpilled(), int64_t(257777));
    CHECK(f.ledger.conserves());
}

VXC_TEST(basin_spill_refused_downstream_is_refunded_not_lost) {
    Fixture f;
    const BasinId id = theBasin();
    int64_t headroom = 0;
    CHECK(f.ledger.capacityToSpillUnits(id, headroom));
    f.ledger.credit(id, headroom + 90000);
    CHECK_EQ(f.ledger.totalSpilled(), int64_t(90000));

    // A consumer that takes only part of what it is offered. The remainder
    // must come back to the basin -- a blocked outfall back-pressures the lake,
    // the same rule RiverNetwork::refundFromCoupler exists for.
    int64_t refunded = 0;
    const int64_t accepted = routeSpills(
        f.ledger,
        [](int64_t, int64_t, int64_t units, int32_t) { return units / 3; },
        &refunded);
    CHECK_EQ(accepted, int64_t(30000));
    CHECK_EQ(refunded, int64_t(60000));
    CHECK_EQ(f.ledger.totalSpilled(), int64_t(30000));
    CHECK(f.ledger.conserves());
    // The refund went back into the basin, which is now ABOVE the sill by the
    // refunded amount -- deliberate: the water is real and has nowhere to go.
    CHECK_EQ(f.ledger.deltaUnits(id), headroom + 60000);
    CHECK(f.ledger.pendingSpill().empty());
}

VXC_TEST(basin_debit_stops_at_empty_and_cannot_invent_units) {
    Fixture f;
    const BasinId id = theBasin();

    // The basin holds 612,000 units at equilibrium, so that is the most that
    // can ever be taken out of it before it is a dry hole.
    int64_t floorDelta = 0;
    CHECK(f.ledger.minDeltaUnits(id, floorDelta));
    CHECK_EQ(floorDelta, int64_t(-612000));

    CHECK_EQ(f.ledger.debit(id, 100000), int64_t(100000));
    CHECK_EQ(f.ledger.deltaUnits(id), int64_t(-100000));
    CHECK(f.ledger.levelMmFor(id, f.terrain.row().surfaceMm) < 200); // the lake dropped

    // Ask for far more than remains: only what is there comes out.
    CHECK_EQ(f.ledger.debit(id, 10000000), int64_t(512000));
    CHECK_EQ(f.ledger.deltaUnits(id), floorDelta);
    CHECK_EQ(f.ledger.levelMmFor(id, f.terrain.row().surfaceMm), 50); // the pool floor
    // An empty basin gives nothing more, however often it is asked.
    CHECK_EQ(f.ledger.debit(id, 1), int64_t(0));
    CHECK_EQ(f.ledger.debit(id, 999999), int64_t(0));
    CHECK(f.ledger.conserves());
    CHECK_EQ(f.ledger.totalDebited(), int64_t(612000));
}

VXC_TEST(basin_unresolvable_basin_is_refused_and_counted_not_guessed) {
    Fixture f;
    const BasinId ghost = BasinId::fromTile(9, 9, 3); // no such tile in the fixture

    CHECK_EQ(f.ledger.credit(ghost, 1000), int64_t(0));
    CHECK_EQ(f.ledger.debit(ghost, 1000), int64_t(0));
    CHECK_EQ(f.ledger.unresolvedCredits(), uint64_t(1));
    CHECK_EQ(f.ledger.unresolvedDebits(), uint64_t(1));
    CHECK_EQ(f.ledger.sumOfDeltas(), int64_t(0));
    CHECK(f.ledger.conserves());

    // THE RAN-FLAG. Both calls happened; both were refused. "asked twice and
    // refused twice" and "never asked" are different facts and the counters
    // keep them apart.
    CHECK_EQ(f.ledger.creditCalls(), uint64_t(1));
    CHECK_EQ(f.ledger.debitCalls(), uint64_t(1));
    CHECK_EQ(f.capacity.hypsometryBuilds(), uint64_t(0));
    CHECK(f.capacity.unresolvedBasins() > 0);

    // A resolvable basin in the same ledger still works and still counts.
    CHECK_EQ(f.ledger.credit(theBasin(), 1000), int64_t(1000));
    CHECK_EQ(f.capacity.hypsometryBuilds(), uint64_t(1));
    CHECK(f.capacity.hypsometryCells() >= uint64_t(100));
}

VXC_TEST(basin_datum_source_moves_the_lake_and_leaves_untouched_ones_alone) {
    Fixture f;
    BasinLedgerDatumSource source(f.ledger);
    BasinEntry& row = f.terrain.row();

    // With no delta, the hook must hand back the wire's own field byte for
    // byte -- which is what keeps every shipped lake and every pinned test
    // exactly where they were.
    CHECK_EQ(source.basinDatumMm(0, 0, row), row.surfaceMm);

    f.ledger.credit(theBasin(), 204000); // 612,000 + 204,000 == V(250 mm)
    CHECK_EQ(source.basinDatumMm(0, 0, row), 250);

    // A basin in another tile shares the ledger and is untouched by this one.
    BasinEntry other = row;
    other.basinId = 1;
    CHECK_EQ(source.basinDatumMm(7, 7, other), other.surfaceMm);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

VXC_TEST(basin_ledger_blob_round_trips_and_reserialises_byte_identical) {
    Fixture f;
    f.ledger.credit(BasinId::fromTile(0, 0, 0), 250000);
    f.ledger.restoreDelta(BasinId::fromTile(3, -4, 7), -98765);
    f.ledger.restoreDelta(BasinId::fromTile(-2, 11, 1), 4242);

    std::vector<uint8_t> blob;
    BasinLedgerState::serialize(f.ledger, blob);
    CHECK(blob.size() > 20);

    Fixture g;
    CHECK(BasinLedgerState::load(blob.data(), blob.size(), g.ledger));
    CHECK_EQ(g.ledger.basinCount(), size_t(3));
    CHECK_EQ(g.ledger.deltaUnits(BasinId::fromTile(0, 0, 0)), int64_t(250000));
    CHECK_EQ(g.ledger.deltaUnits(BasinId::fromTile(3, -4, 7)), int64_t(-98765));
    CHECK_EQ(g.ledger.deltaUnits(BasinId::fromTile(-2, 11, 1)), int64_t(4242));
    CHECK(g.ledger.conserves());

    Digest a, b;
    f.ledger.digest(a);
    g.ledger.digest(b);
    CHECK_EQ(a.h, b.h);

    // Key-sorted by construction, so a re-serialised load is byte-identical
    // and there is nothing to sort at save time.
    std::vector<uint8_t> again;
    BasinLedgerState::serialize(g.ledger, again);
    CHECK_EQ(again.size(), blob.size());
    CHECK(again == blob);

    // The restored level agrees with the live one -- the whole reason the
    // authority is a VOLUME and not a level.
    CHECK_EQ(g.ledger.levelMmFor(theBasin(), f.terrain.row().surfaceMm),
             f.ledger.levelMmFor(theBasin(), f.terrain.row().surfaceMm));
}

VXC_TEST(basin_ledger_blob_refuses_every_way_it_can_be_wrong) {
    Fixture f;
    f.ledger.credit(theBasin(), 123456);
    std::vector<uint8_t> good;
    BasinLedgerState::serialize(f.ledger, good);

    {
        Fixture g;
        std::vector<uint8_t> bad = good;
        bad[0] ^= 0xFF; // magic
        CHECK(!BasinLedgerState::load(bad.data(), bad.size(), g.ledger));
        CHECK_EQ(g.ledger.basinCount(), size_t(0));
    }
    {
        Fixture g;
        std::vector<uint8_t> bad = good;
        bad[4] = uint8_t(kBasinLedgerVersion + 1); // version
        CHECK(!BasinLedgerState::load(bad.data(), bad.size(), g.ledger));
    }
    {
        Fixture g;
        std::vector<uint8_t> bad = good;
        bad.pop_back(); // truncated
        CHECK(!BasinLedgerState::load(bad.data(), bad.size(), g.ledger));
    }
    {
        Fixture g;
        std::vector<uint8_t> bad = good;
        bad.push_back(0); // trailing bytes
        CHECK(!BasinLedgerState::load(bad.data(), bad.size(), g.ledger));
    }
    {
        Fixture g;
        std::vector<uint8_t> bad = good;
        bad[bad.size() - 8] ^= 0x01; // the integrity cross-check
        CHECK(!BasinLedgerState::load(bad.data(), bad.size(), g.ledger));
    }
    {
        // An empty ledger is a VALID blob (a world where no lake has moved),
        // and must not be confused with a corrupt one.
        Fixture empty, g;
        std::vector<uint8_t> blob;
        BasinLedgerState::serialize(empty.ledger, blob);
        CHECK(BasinLedgerState::load(blob.data(), blob.size(), g.ledger));
        CHECK_EQ(g.ledger.basinCount(), size_t(0));
    }
}

// ---------------------------------------------------------------------------
// THE END-TO-END SCALAR PATH
// ---------------------------------------------------------------------------

VXC_TEST(scalar_hydrology_end_to_end_faucet_graph_basin_spillway_conserves) {
    // Plan Phase 2's shippable claim, as one standalone voxel-core test: a
    // headwater faucet feeds the routing graph, the graph's outlet fills a
    // basin, the basin rises by exactly the volume it was credited, and once it
    // reaches its baked sill the excess appears downstream -- with every unit
    // accounted for at both ends of every hand-off and no engine, no tile file
    // and no particle anywhere in it.
    TwoChannelWaterSource water;
    RiverNetwork net;
    BakedWaterBuildParams params;
    params.bounds = RegionBounds{-20, 0, 34, 16};
    const uint32_t segs = net.buildFromBakedWater(water, /*seed*/ 20260809, params);
    CHECK(segs > 0);
    CHECK_EQ(net.bakedCellsUnresolved(), uint64_t(0));
    CHECK(net.bakedChannelCells() > 0);

    // Two chains of 14 and 15 nodes -> 13 + 14 segments, two heads, two
    // terminals. Asserted rather than assumed, because everything below indexes
    // into them.
    CHECK_EQ(net.nodes().size(), size_t(29));
    CHECK_EQ(segs, uint32_t(27));
    CHECK_EQ(net.headwaterNodes().size(), size_t(2));

    // Locate the two chains by their nodes' world position rather than by index
    // arithmetic, so a change to build order fails loudly instead of quietly
    // testing the wrong reach.
    uint32_t feederHeadSeg = RiverNetwork::kNoSegment;
    uint32_t feederLastSeg = RiverNetwork::kNoSegment;
    uint32_t spillFirstSeg = RiverNetwork::kNoSegment;
    uint32_t spillLastSeg = RiverNetwork::kNoSegment;
    for (uint32_t s = 0; s < segs; ++s) {
        const RiverNode& from = net.nodes()[net.segments()[s].fromNode];
        const int64_t px = floorDiv(from.vx * kVoxelSizeMm, int64_t(kTestPixelMm));
        const bool terminal = net.outgoingSegment(net.segments()[s].toNode) ==
                              RiverNetwork::kNoSegment;
        if (px == -14) feederHeadSeg = s;
        if (px == -2 && terminal) feederLastSeg = s;
        if (px == 16) spillFirstSeg = s;
        if (px == 29 && terminal) spillLastSeg = s;
    }
    CHECK(feederHeadSeg != RiverNetwork::kNoSegment);
    CHECK(feederLastSeg != RiverNetwork::kNoSegment);
    CHECK(spillFirstSeg != RiverNetwork::kNoSegment);
    CHECK(spillLastSeg != RiverNetwork::kNoSegment);
    if (feederHeadSeg == RiverNetwork::kNoSegment || feederLastSeg == RiverNetwork::kNoSegment ||
        spillFirstSeg == RiverNetwork::kNoSegment || spillLastSeg == RiverNetwork::kNoSegment) {
        return;
    }

    Fixture f;
    const BasinId id = theBasin();
    int64_t headroom = 0;
    CHECK(f.ledger.capacityToSpillUnits(id, headroom));

    constexpr int32_t kFaucetPerTick = 500000;
    constexpr int kTicks = 60;
    int64_t faucetTotal = 0, deliveredToBasin = 0, toSea = 0, reinjected = 0;

    for (int t = 0; t < kTicks; ++t) {
        net.injectInflow(feederHeadSeg, kFaucetPerTick);
        faucetTotal += kFaucetPerTick;

        const int64_t outletsBefore = net.totalOutflowToOutlets();
        net.step(1000);

        // The two sinks. A terminal segment's post-step `discharge` IS what
        // left the graph there this tick (rivernet.h step(), APPLY phase), and
        // the two must add up to the ledger's own outlet delta -- which is the
        // cross-check that these are the only two exits.
        const int64_t intoBasin = net.segments()[feederLastSeg].discharge;
        const int64_t intoSea = net.segments()[spillLastSeg].discharge;
        CHECK_EQ(net.totalOutflowToOutlets() - outletsBefore, intoBasin + intoSea);
        deliveredToBasin += intoBasin;
        toSea += intoSea;

        // BASIN SINK: what left the graph enters the lake, ledgered on both
        // sides. The graph already removed it from totalStorage(); credit()
        // returning less than we hand it would be a leak, so it is asserted.
        if (intoBasin > 0) {
            const int64_t deltaBefore = f.ledger.deltaUnits(id);
            const int64_t accepted = f.ledger.credit(id, intoBasin);
            CHECK_EQ(accepted, intoBasin);
            // "Rises by exactly the ledgered volume": what the lake gained plus
            // what it spilled is exactly what it was handed.
            const int64_t gained = f.ledger.deltaUnits(id) - deltaBefore;
            CHECK(gained >= 0);
            CHECK(gained <= intoBasin);
        }

        // SILL FAUCET: the excess goes into the reach nearest the baked outlet.
        // 2 m of reach: the outlet pixel and the spillway head are adjacent.
        int64_t refunded = 0;
        reinjected += routeSpills(
            f.ledger,
            [&](int64_t vx, int64_t vy, int64_t units, int32_t) -> int64_t {
                const uint32_t seg = net.nearestSegmentToVoxel(vx, vy, 2000);
                if (seg == RiverNetwork::kNoSegment) return 0;
                const int32_t amount = int32_t(clampi64(units, 0, INT32_MAX));
                net.injectInflow(seg, amount);
                return amount;
            },
            &refunded);
        CHECK_EQ(refunded, int64_t(0)); // the spillway is right there; nothing may bounce

        // Both ledgers hold every tick, not just at the end.
        CHECK_EQ(net.totalStorage(), net.recomputeTotalStorage());
        CHECK_EQ(net.totalStorage() + net.totalOutflowToOutlets(), net.totalInjected());
        CHECK(f.ledger.conserves());
    }

    // --- the basin actually filled, and then spilled ------------------------
    CHECK(deliveredToBasin > headroom); // the run was long enough to overtop it
    CHECK_EQ(f.ledger.deltaUnits(id), headroom);
    CHECK_EQ(f.ledger.levelMmFor(id, f.terrain.row().surfaceMm), 500); // standing at the sill
    CHECK(f.ledger.spillEvents() > 0);

    // --- the spill appeared DOWNSTREAM, not in the feeder --------------------
    CHECK(reinjected > 0);
    CHECK(toSea > 0);
    // The spillway carried the water: its head segment saw storage, and every
    // unit that reached the sea came from the spill, since the feeder's only
    // exit is the basin.
    CHECK(toSea <= reinjected);

    // --- ONE conservation statement over the WHOLE path ---------------------
    //
    // Everything the faucet ever made is now in exactly one of three places:
    // still routing in the reaches, standing in the lake, or gone to sea.
    // Nothing else may hold a unit.
    CHECK_EQ(faucetTotal, net.totalStorage() + f.ledger.sumOfDeltas() + toSea);
    // Stated the other way, at each seam: what the graph delivered is what the
    // lake kept plus what it passed on.
    CHECK_EQ(deliveredToBasin, f.ledger.sumOfDeltas() + reinjected);
    CHECK_EQ(net.totalInjected(), faucetTotal + reinjected);
    CHECK_EQ(f.ledger.totalCredited() - f.ledger.totalDebited(),
             f.ledger.sumOfDeltas() + f.ledger.totalSpilled());
}

// ---------------------------------------------------------------------------
// Basin table v2: the baked capacity path
// ---------------------------------------------------------------------------

VXC_TEST(basin_v2_baked_curve_tracks_the_client_hypsometry_on_a_paraboloid) {
    // The BAKE is simulated honestly: the row ships the three levels and the
    // integral of the client's own measured curve between surface and spill,
    // in litres, which is what `capacity_m3` computes and `capacity_l` carries.
    // Nothing else about the shape crosses over -- that is the whole question.
    ParaboloidBowlTerrain terrain;
    ClientHypsometryProvider client{terrain};
    client.setTilePixels(kV2TilePx);
    const BasinId v1id = BasinId::fromTile(0, 0, 0);
    const BasinHypsometry* h = client.curveFor(v1id);
    CHECK(h != nullptr);
    if (h == nullptr) return;

    const int64_t measuredCapacity = h->spillUnits - h->equilibriumUnits;
    const uint64_t capacityLitres = uint64_t(measuredCapacity / kBasinUnitsPerLitre);

    SyntheticV2Table src;
    src.addRow(0, 0,
               v2Row(0, 19, 19, 0, 0, 39, 39, h->floorMm, h->equilibriumMm, h->spillMm,
                     capacityLitres, 39, 20, false));
    BakedBasinTable table{src};
    BakedCapacityProvider baked{table};
    const BasinId v2id = BasinId::fromGlobal(packGlobalId(19, 19));
    CHECK(v2id.valid());
    CHECK(!v2id.isTileLocal());

    // --- the three levels are the wire's own, so they are IDENTICAL ---------
    int32_t bf = 0, be = 0, bs = 0;
    CHECK(baked.levelsMm(v2id, bf, be, bs));
    CHECK_EQ(bf, h->floorMm);
    CHECK_EQ(be, h->equilibriumMm);
    CHECK_EQ(bs, h->spillMm);

    // --- the headroom is BAKED TRUTH, not a modelled number -----------------
    //
    // V(spill) - V(surface) == capacity_l exactly, because both sides are
    // floors of quantities differing by that integer. This is the number that
    // decides WHEN a lake spills, so it is the one that may not be a model.
    int64_t atSpill = 0, atEq = 0;
    CHECK(baked.volumeAtLevelUnits(v2id, bs, atSpill));
    CHECK(baked.volumeAtLevelUnits(v2id, be, atEq));
    CHECK_EQ(atSpill - atEq, int64_t(capacityLitres) * kBasinUnitsPerLitre);
    // And the two curves genuinely DISAGREE about the absolute volume held at
    // equilibrium while agreeing about the level -- what sets a stage is the
    // curve's slope there, not its integral from the floor. Asserted, because
    // a reader who did not know this would file the next loop as a bug.
    CHECK(atEq != h->equilibriumUnits);

    // --- the stage tracks the measured curve over the WHOLE headroom --------
    int32_t worstMm = 0;
    for (int i = 0; i <= 100; ++i) {
        const int64_t delta = measuredCapacity * i / 100;
        int32_t clientMm = 0, bakedMm = 0;
        CHECK(levelForDelta(client, v1id, delta, clientMm));
        CHECK(levelForDelta(baked, v2id, delta, bakedMm));
        const int32_t gap = clientMm > bakedMm ? clientMm - bakedMm : bakedMm - clientMm;
        if (gap > worstMm) worstMm = gap;
    }
    // 13 mm over a 3,990 mm stage range: 0.33%. Exact integer arithmetic on
    // both sides, so this is a VALUE and not a tolerance -- if it moves, the
    // curve moved.
    CHECK_EQ(worstMm, 13);
    // Both ends are pinned, so they must agree to the millimetre.
    int32_t endMm = 0;
    CHECK(levelForDelta(baked, v2id, 0, endMm));
    CHECK_EQ(endMm, h->equilibriumMm);
    CHECK(levelForDelta(baked, v2id, measuredCapacity, endMm));
    CHECK_EQ(endMm, h->spillMm);
}

VXC_TEST(basin_v2_curve_cost_on_a_flat_bottomed_basin_is_measured_not_assumed) {
    // The same comparison on the STEPPED bowl, whose A(h) is a step and is the
    // worst case for a linear-A model. The point of the test is that the cost
    // is a number in the repository rather than a caveat in a comment: if the
    // model is ever changed, this says by how much it moved and on which shape.
    SteppedBowlTerrain terrain;
    ClientHypsometryProvider client{terrain};
    client.setTilePixels(kV2TilePx);
    const BasinId v1id = BasinId::fromTile(0, 0, 0);
    const BasinHypsometry* h = client.curveFor(v1id);
    CHECK(h != nullptr);
    if (h == nullptr) return;

    const int64_t measuredCapacity = h->spillUnits - h->equilibriumUnits;
    CHECK_EQ(measuredCapacity, int64_t(6579000)); // the hand-computed headroom

    SyntheticV2Table src;
    src.addRow(0, 0,
               v2Row(0, 8, 8, 0, 0, 15, 15, h->floorMm, h->equilibriumMm, h->spillMm,
                     uint64_t(measuredCapacity / kBasinUnitsPerLitre), 15, 8, false));
    BakedBasinTable table{src};
    BakedCapacityProvider baked{table};
    const BasinId v2id = BasinId::fromGlobal(packGlobalId(8, 8));

    int32_t worstMm = 0;
    for (int i = 0; i <= 100; ++i) {
        const int64_t delta = measuredCapacity * i / 100;
        int32_t clientMm = 0, bakedMm = 0;
        CHECK(levelForDelta(client, v1id, delta, clientMm));
        CHECK(levelForDelta(baked, v2id, delta, bakedMm));
        const int32_t gap = clientMm > bakedMm ? clientMm - bakedMm : bakedMm - clientMm;
        if (gap > worstMm) worstMm = gap;
    }
    // 31 mm over a 450 mm range: 6.9%, against the paraboloid's 0.33%. That is
    // the price of shipping an integral instead of a curve, on the shape that
    // costs the most.
    CHECK_EQ(worstMm, 31);
}

VXC_TEST(basin_v2_spanning_lake_is_one_ledger_entry_and_capacity_is_not_summed) {
    // ONE lake straddling the seam between tile (0,0) and tile (1,0). Both
    // tiles register it, both write the SAME global id (the floor cell, which
    // lives in tile 0), and -- the fact this test exists for -- both report the
    // WHOLE component's capacity, because each tile's padded window saw the
    // whole basin.
    const int64_t capacityLitres = 400000;
    SyntheticV2Table src;
    src.addRow(0, 0,
               v2Row(0, 40, 20, 40, 10, 90, 30, 1000, 3000, 5000, capacityLitres, 95, 20, true));
    src.addRow(1, 0,
               v2Row(0, 40, 20, 40, 10, 90, 30, 1000, 3000, 5000, capacityLitres, 95, 20, true));

    BakedBasinTable table{src};
    BakedCapacityProvider baked{table};
    const BasinId id = BasinId::fromGlobal(packGlobalId(40, 20));

    const BakedBasin* comp = table.resolve(id);
    CHECK(comp != nullptr);
    if (comp == nullptr) return;
    CHECK_EQ(comp->memberRows, uint32_t(2)); // both rows folded in
    CHECK(comp->spanning);
    CHECK_EQ(table.componentCount(), size_t(1));
    CHECK_EQ(table.spanningComponents(), uint64_t(1));
    // The ids matched, so the bbox+spill fallback never had to fire.
    CHECK_EQ(table.fallbackUnions(), uint64_t(0));
    CHECK_EQ(table.capacityDisagreements(), uint64_t(0));

    // CAPACITY IS NOT SUMMED. One lake's headroom, not two.
    CHECK_EQ(comp->capacityUnits, capacityLitres * kBasinUnitsPerLitre);

    // Both tiles' rows key to the same account, which is the whole point: a
    // per-tile key would give this lake two half-accounts drawn at two heights.
    BasinLedger ledger{baked};
    BasinLedgerDatumSource datum{ledger, &table};
    const std::vector<BasinEntry>* rowsA = src.rowsForTile(0, 0);
    const std::vector<BasinEntry>* rowsB = src.rowsForTile(1, 0);
    CHECK(rowsA != nullptr && rowsB != nullptr);
    if (rowsA == nullptr || rowsB == nullptr) return;
    CHECK(basinKeyFor(0, 0, (*rowsA)[0], &table) == id);
    CHECK(basinKeyFor(1, 0, (*rowsB)[0], &table) == id);

    // The headroom the ledger will accept is the baked number, to the unit.
    int64_t headroom = 0;
    CHECK(ledger.capacityToSpillUnits(id, headroom));
    CHECK_EQ(headroom, capacityLitres * kBasinUnitsPerLitre);

    const int64_t credit = headroom / 4;
    CHECK_EQ(ledger.credit(id, credit), credit);
    CHECK_EQ(ledger.basinCount(), size_t(1)); // ONE entry, not one per tile
    CHECK_EQ(ledger.sumOfDeltas(), credit);
    CHECK(ledger.conserves());

    // Both halves of the lake are drawn at the one new level.
    const int32_t levelA = datum.basinDatumMm(0, 0, (*rowsA)[0]);
    const int32_t levelB = datum.basinDatumMm(1, 0, (*rowsB)[0]);
    CHECK_EQ(levelA, levelB);
    CHECK(levelA > 3000); // it rose off the baked equilibrium
    CHECK(levelA < 5000); // and is nowhere near spilling on a quarter charge
}

VXC_TEST(basin_v2_union_falls_back_to_world_bbox_and_spill_when_the_ids_disagree) {
    // The apron blind spot: two tiles measured slightly different padded
    // surfaces over the overlap, so they disagree about which cell is the
    // deepest and write DIFFERENT global ids for one lake. `tile_codec.py`'s
    // documented fallback is what has to catch it -- world bboxes overlap and
    // the spills agree to within the field's own 1 mm LSB.
    const int64_t capacityLitres = 250000;
    SyntheticV2Table src;
    // Tile 0's row puts the floor at world (40,20); tile 1's at (70,21), with a
    // sill one millimetre lower. Same lake.
    src.addRow(0, 0,
               v2Row(0, 40, 20, 40, 10, 90, 30, 1000, 3000, 5000, capacityLitres, 95, 20, true));
    src.addRow(1, 0,
               v2Row(0, 70, 21, 45, 10, 95, 30, 1000, 3000, 4999, capacityLitres, 95, 20, true));

    BakedBasinTable table{src};
    BakedCapacityProvider baked{table};
    const BasinId a = BasinId::fromGlobal(packGlobalId(40, 20));
    const BasinId b = BasinId::fromGlobal(packGlobalId(70, 21));
    CHECK(!(a == b));

    const BakedBasin* comp = table.resolve(a);
    CHECK(comp != nullptr);
    if (comp == nullptr) return;
    CHECK_EQ(table.fallbackUnions(), uint64_t(1));
    CHECK_EQ(comp->memberRows, uint32_t(2));
    CHECK_EQ(table.componentCount(), size_t(1));
    // NOT SUMMED on the fallback path either.
    CHECK_EQ(comp->capacityUnits, capacityLitres * kBasinUnitsPerLitre);
    // The canonical key is the smaller of the two ids, so which tile streamed
    // in first cannot change what the lake is called.
    CHECK(comp->id == (a.v < b.v ? a : b));
    // Both ids now resolve to that one component.
    CHECK(table.resolve(b) == comp);

    const std::vector<BasinEntry>* rowsA = src.rowsForTile(0, 0);
    const std::vector<BasinEntry>* rowsB = src.rowsForTile(1, 0);
    CHECK(rowsA != nullptr && rowsB != nullptr);
    if (rowsA == nullptr || rowsB == nullptr) return;
    CHECK(basinKeyFor(0, 0, (*rowsA)[0], &table) == comp->id);
    CHECK(basinKeyFor(1, 0, (*rowsB)[0], &table) == comp->id);

    BasinLedger ledger{baked};
    CHECK_EQ(ledger.credit(comp->id, 1000), int64_t(1000));
    CHECK_EQ(ledger.basinCount(), size_t(1));
}

VXC_TEST(basin_v2_late_neighbour_merges_the_account_instead_of_stranding_it) {
    // The streaming order that would otherwise lose water: tile 1 is resident
    // and tile 0 is not, so its half of the lake is credited under its OWN id;
    // tile 0 then arrives, the union rule fires, and the account has to follow
    // the lake rather than sit under a key nothing asks about again.
    const int64_t capacityLitres = 250000;
    const BasinEntry rowA =
        v2Row(0, 40, 20, 40, 10, 90, 30, 1000, 3000, 5000, capacityLitres, 95, 20, true);
    const BasinEntry rowB =
        v2Row(0, 70, 21, 45, 10, 95, 30, 1000, 3000, 4999, capacityLitres, 95, 20, true);

    SyntheticV2Table src;
    src.addRow(1, 0, rowB); // only the far half is resident
    BakedBasinTable table{src};
    BakedCapacityProvider baked{table};
    BasinLedger ledger{baked};

    const BasinId b = BasinId::fromGlobal(packGlobalId(70, 21));
    // Tile 1's row names a floor cell in tile 1, so it resolves alone.
    CHECK_EQ(table.resolveKeyFor(1, 0, rowB).v, b.v);
    CHECK_EQ(ledger.credit(b, 5000), int64_t(5000));
    CHECK_EQ(ledger.basinCount(), size_t(1));
    CHECK_EQ(ledger.deltaUnits(b), int64_t(5000));

    // Tile 0 streams in. Ingesting it discovers that the two rows are one lake.
    src.addRow(0, 0, rowA);
    table.ingestTile(0, 0);
    const std::vector<BakedBasinTable::Merge> merges = table.drainMerges();
    CHECK(!merges.empty());
    for (const BakedBasinTable::Merge& m : merges) {
        ledger.mergeAccount(m.from, m.into);
    }

    const BasinId a = BasinId::fromGlobal(packGlobalId(40, 20));
    const BasinId canonical = a.v < b.v ? a : b;
    CHECK_EQ(ledger.basinCount(), size_t(1));              // still one lake
    CHECK_EQ(ledger.deltaUnits(canonical), int64_t(5000)); // and it kept its water
    CHECK_EQ(ledger.sumOfDeltas(), int64_t(5000));         // nothing was created
    CHECK(ledger.conserves());                             // and nothing was lost
    CHECK_EQ(ledger.accountMerges(), uint64_t(1));
}

VXC_TEST(basin_v2_spillway_uses_the_world_outlet_not_the_tile_clamped_one) {
    // `outletX/Y` is clamped into the row's own tile, so for a basin whose
    // saddle is in a NEIGHBOUR it names a cell on the seam. Routing a spill
    // there feeds the wrong valley, which is the one spill failure that looks
    // like data rather than like a bug.
    const int64_t capacityLitres = 100;
    SyntheticV2Table src;
    // Floor in tile (0,0); the true saddle is at world pixel (200, 20), which
    // is tile 3. The u16 outlet is clamped to 63 -- still inside tile 0.
    src.addRow(0, 0,
               v2Row(0, 40, 20, 40, 10, 210, 30, 1000, 3000, 5000, capacityLitres, 200, 20, true));
    BakedBasinTable table{src};
    BakedCapacityProvider baked{table};
    const BasinId id = BasinId::fromGlobal(packGlobalId(40, 20));

    const std::vector<BasinEntry>* rows = src.rowsForTile(0, 0);
    CHECK(rows != nullptr);
    if (rows == nullptr) return;
    CHECK_EQ((*rows)[0].outletX, uint16_t(kV2TilePx - 1)); // the clamp, i.e. the lie

    int64_t vx = 0, vy = 0;
    CHECK(baked.outletVoxel(id, vx, vy));
    // World pixel 200 at 1 m per pixel, taken at the pixel CENTRE (matching
    // buildFromFlowAccumulation's node placement), in 10 cm voxels.
    CHECK_EQ(vx, int64_t((200 * kTestPixelMm + kTestPixelMm / 2) / kVoxelSizeMm));
    CHECK_EQ(vy, int64_t((20 * kTestPixelMm + kTestPixelMm / 2) / kVoxelSizeMm));
    // Not the clamped cell, which is over 1.3 km short of the real saddle.
    CHECK(vx != int64_t((63 * kTestPixelMm + kTestPixelMm / 2) / kVoxelSizeMm));

    // And a real spill event carries it, since that is what the router reads.
    BasinLedger ledger{baked};
    int64_t headroom = 0;
    CHECK(ledger.capacityToSpillUnits(id, headroom));
    CHECK_EQ(ledger.credit(id, headroom + 777), headroom + 777);
    const std::vector<BasinSpillEvent> spills = ledger.drainSpillEvents();
    CHECK_EQ(spills.size(), size_t(1));
    if (spills.empty()) return;
    CHECK_EQ(spills[0].units, int64_t(777));
    CHECK_EQ(spills[0].outletVx, vx);
    CHECK_EQ(spills[0].outletVy, vy);
    CHECK_EQ(spills[0].spillMm, 5000);
}

VXC_TEST(basin_v2_router_sends_each_key_to_the_provider_that_owns_it) {
    // v1 and v2 tiles coexist in one session (a partly re-baked cache is the
    // normal state), so the choice of provider must be per KEY and not a mode.
    SteppedBowlTerrain terrain;
    ClientHypsometryProvider client{terrain};
    client.setTilePixels(kV2TilePx);

    SyntheticV2Table src;
    src.addRow(0, 0, v2Row(0, 40, 20, 40, 10, 90, 30, 1000, 3000, 5000, 1000, 95, 20, false));
    BakedBasinTable table{src};
    BakedCapacityProvider baked{table};
    BasinCapacityRouter router{client, baked};

    const BasinId v1 = BasinId::fromTile(0, 0, 0);
    const BasinId v2 = BasinId::fromGlobal(packGlobalId(40, 20));

    int32_t f = 0, e = 0, s = 0;
    CHECK(router.levelsMm(v1, f, e, s));
    CHECK_EQ(s, 500); // the stepped bowl's sill: the client answered
    CHECK(router.levelsMm(v2, f, e, s));
    CHECK_EQ(s, 5000); // the baked row's sill: the v2 provider answered

    // A v2 key the table cannot resolve is REFUSED, not answered by the client
    // provider on a fallback that would silently invent a curve for it.
    const BasinId ghost = BasinId::fromGlobal(packGlobalId(9000, 9000));
    CHECK(!router.levelsMm(ghost, f, e, s));
    CHECK(router.unresolvedBasins() > 0);
}

VXC_TEST(basin_v2_refuses_impossible_geometry_rather_than_clamping_it) {
    SyntheticV2Table src;
    // A basin claiming 2,000 m of stage: past kBasinMaxSpanMm, which is where
    // the curve's overflow argument stops holding.
    src.addRow(0, 0, v2Row(0, 40, 20, 40, 10, 90, 30, 0, 0, 2000000, 1000, 95, 20, false));
    BakedBasinTable table{src};
    BakedCapacityProvider baked{table};
    const BasinId id = BasinId::fromGlobal(packGlobalId(40, 20));

    int32_t f = 0, e = 0, s = 0;
    CHECK(!baked.levelsMm(id, f, e, s)); // refused
    CHECK_EQ(table.refusedRows(), uint64_t(1));
    // And the ledger's own refusal contract holds through it: the credit is
    // lost and COUNTED, never guessed into a basin at an invented level.
    BasinLedger ledger{baked};
    CHECK_EQ(ledger.credit(id, 10000), int64_t(0));
    CHECK_EQ(ledger.unresolvedCredits(), uint64_t(1));
    CHECK_EQ(ledger.sumOfDeltas(), int64_t(0));
}

VXC_TEST(basin_v2_capacity_units_convert_from_litres_with_no_rounding) {
    // A litre is 1e6 mm^3 is one 10 cm voxel is kBasinLedgerUnitsPerVoxel
    // units, so this conversion is exact in both directions -- the only volume
    // conversion in the file that is exact by construction rather than by a
    // chosen rounding rule.
    CHECK_EQ(basinUnitsFromLitres(0), int64_t(0));
    CHECK_EQ(basinUnitsFromLitres(1), int64_t(255));
    CHECK_EQ(basinUnitsFromLitres(2600000000ull), int64_t(2600000000ll) * 255);
    // The encoder's own absurd upper bound (1.96e15 litres) still fits.
    CHECK_EQ(basinUnitsFromLitres(1960000000000000ull), int64_t(1960000000000000ll) * 255);
    // Past int64 it SATURATES rather than wrapping: a negative capacity would
    // let the spillway route backwards.
    CHECK(basinUnitsFromLitres(~uint64_t(0)) > 0);
}
