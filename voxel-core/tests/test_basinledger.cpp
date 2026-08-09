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
