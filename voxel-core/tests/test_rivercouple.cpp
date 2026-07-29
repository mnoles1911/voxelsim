// River network <-> water CA coupling (plan §3.7 Layer R, W3-proper).
//
// The two things worth testing hardest here are the two things the brief for
// this work named: (a) CONSERVATION across the graph<->CA boundary, which is
// where volume goes missing in couplings of this kind (see
// docs/adr/0004-swe-fixed-point-coupling.md for the same class of bug in the
// SWE coupler), and (b) the PROMOTION CRITERION, where a false positive is
// permanently destructive and a false negative costs nothing -- so the
// negative controls below matter more than the positive one.
//
// See voxelcore/rivercouple.h for the full design writeup.

#include "voxelcore/rivercouple.h"

#include <set>
#include <utility>
#include <vector>

#include "vxctest.h"

using namespace vxc;

namespace {

constexpr uint64_t kSeed = 20260728;

// A deliberately TINY tile pixel: 500 mm == 5 voxels per pixel, against the
// tile protocol's real 30 m. The coupling's arithmetic is entirely in terms
// of pixelSizeMm / kVoxelSizeMm, so shrinking the pixel changes nothing about
// what is under test and makes a whole multi-pixel channel fit in a handful
// of CA bricks -- which is what lets these tests fill and drain real water
// rather than assert against a mocked CA.
constexpr int32_t kPixMm = 500;
constexpr int64_t kPixVox = kPixMm / kVoxelSizeMm; // 5

// A programmable elevation grid. Out-of-grid pixels report an unreachable
// ridge, so every flow path -- generation's D8 and the coupler's promotion
// search alike -- stays inside the region with no special-casing.
class GridTiles final : public ITileSampler {
public:
    static constexpr int32_t kOutside = 1'000'000;

    GridTiles(int64_t w, int64_t h) : w_(w), h_(h), elev_(static_cast<size_t>(w * h), kOutside) {}

    int32_t pixelSizeMm() const override { return kPixMm; }

    int32_t elevationMm(int64_t px, int64_t py) override {
        if (px < 0 || py < 0 || px >= w_ || py >= h_) return kOutside;
        return elev_[static_cast<size_t>(py * w_ + px)];
    }

    // Rain falls only on the stem row, so no side row can ever accumulate
    // enough to become a node on its own -- which is exactly the situation a
    // promotion has to be able to change.
    ClimateSample climate(int64_t px, int64_t py) override {
        ClimateSample c;
        c.precipitation = (py == 0 && px >= 0 && px < w_) ? 100 : 0;
        return c;
    }

    void set(int64_t px, int64_t py, int32_t mm) { elev_[static_cast<size_t>(py * w_ + px)] = mm; }

private:
    int64_t w_, h_;
    std::vector<int32_t> elev_;
};

// Terrain solidity derived from the tile grid, plus an explicit carved bed on
// nominated pixels -- the same split the real world has, where the tile field
// is worldgen and the channel is a voxel edit. The promotion search reads the
// TILE field (a diverted channel must descend at tile scale, see
// rivercouple.h section 4); the CA and the outfall floor scan read this.
struct Terrain {
    GridTiles* tiles = nullptr;
    int64_t trenchVoxels = 0;
    std::set<std::pair<int64_t, int64_t>> carved;

    // SEALED mode, for the promotion tests only. Everything outside the carved
    // pixels becomes solid rock and each carved pixel is roofed at
    // trenchVoxels, so the only water in the world is water the test placed
    // and it cannot spill into a pixel the test did not nominate.
    //
    // This is not cosmetic: without it, water topped up in a carved pixel
    // pours over the (lower, uncarved) pixel beside it, wets IT, and the
    // coupler quite correctly promotes that channel too -- a cascade that is
    // right behaviour and useless as a controlled experiment. Sealing makes
    // "which pixels are wet" an input to the test rather than an emergent
    // property of it.
    bool sealed = false;

    int64_t surfaceVz(int64_t px, int64_t py) {
        return floorDiv(tiles->elevationMm(px, py), kVoxelSizeMm);
    }
    int64_t bedVz(int64_t px, int64_t py) {
        int64_t top = surfaceVz(px, py);
        if (carved.count({px, py})) top -= trenchVoxels;
        return top;
    }
    MaterialId at(int64_t vx, int64_t vy, int64_t vz) {
        const int64_t px = floorDiv(vx * kVoxelSizeMm, kPixMm);
        const int64_t py = floorDiv(vy * kVoxelSizeMm, kPixMm);
        const bool isCarved = carved.count({px, py}) != 0;
        if (sealed && !isCarved) return MAT_ROCK;
        const int64_t bed = bedVz(px, py);
        if (vz < bed) return MAT_ROCK;
        if (sealed && vz >= bed + trenchVoxels) return MAT_ROCK;
        return MAT_AIR;
    }
};

WaterCA::SolidFn solidFnFor(Terrain& t) {
    return [&t](int64_t vx, int64_t vy, int64_t vz) { return t.at(vx, vy, vz); };
}

// Sets every cell of `px,py`'s carved bed, `depth` voxels up, to `fill`.
// addWaterAt (the exact single-cell hook) rather than addWater so the test
// controls precisely which cells are wet, with no upward stacking.
void wetPixel(WaterCA& ca, Terrain& t, int64_t px, int64_t py, int64_t depth, uint8_t fill) {
    const int64_t bed = t.bedVz(px, py);
    for (int64_t dy = 0; dy < kPixVox; ++dy)
        for (int64_t dx = 0; dx < kPixVox; ++dx)
            for (int64_t dz = 0; dz < depth; ++dz) {
                const int64_t vx = px * kPixVox + dx, vy = py * kPixVox + dy, vz = bed + dz;
                const uint8_t cur = ca.fillAt(vx, vy, vz);
                if (cur < fill) ca.addWaterAt(vx, vy, vz, static_cast<uint32_t>(fill - cur));
            }
}

void dryPixel(WaterCA& ca, Terrain& t, int64_t px, int64_t py, int64_t depth) {
    const int64_t bed = t.bedVz(px, py);
    for (int64_t dy = 0; dy < kPixVox; ++dy)
        for (int64_t dx = 0; dx < kPixVox; ++dx)
            for (int64_t dz = 0; dz < depth; ++dz)
                ca.removeWaterAt(px * kPixVox + dx, py * kPixVox + dy, bed + dz, 255);
}

// --- the shared "stem plus one side pixel" scenario -------------------------
//
// Row 0 (px 0..9) is a river descending 1 m per pixel. Row 1 sits 300 mm
// ABOVE row 0 at every px -- high enough that D8 generation always keeps the
// stem in row 0, low enough that row 1 pixels are strictly downhill of the
// stem node one pixel upstream of them. Row 2 is an unreachable ridge.
//
// Nodes therefore come out as row-0 pixels px 1..9 (node k == pixel (k+1, 0)),
// with segments node k -> node k+1 and node 8 terminal at the region edge.
// The diversion under test leaves node 1 (pixel (2,0), 8000 mm), crosses the
// fresh pixel (3,1) at 7300 mm, and rejoins the network at node 2 (pixel
// (3,0), 7000 mm) -- a braided side channel, which is the shape a diversion
// actually takes at tile resolution beside a stem.
constexpr int64_t kGridW = 10, kGridH = 3;
constexpr int64_t kAccumThreshold = 9000;

struct Scenario {
    GridTiles tiles{kGridW, kGridH};
    Terrain terrain;
    RiverNetwork net;

    Scenario(int32_t stemTop, int32_t stemStep) {
        for (int64_t px = 0; px < kGridW; ++px) {
            const int32_t e0 = stemTop - static_cast<int32_t>(px) * stemStep;
            tiles.set(px, 0, e0);
            tiles.set(px, 1, e0 + 300);
            tiles.set(px, 2, 60000);
        }
        terrain.tiles = &tiles;
        net.buildFromFlowAccumulation(tiles, kSeed, RegionBounds{0, 0, kGridW - 1, kGridH - 1},
                                      kAccumThreshold);
    }
};

// The four-way ledger identity rivercouple.h section 1 states, checked in one
// place so every test below asserts exactly the same thing.
void checkLedger(const RiverNetwork& net, const RiverCaCoupler& c, const WaterCA& ca) {
    CHECK_EQ(net.totalStorage(), net.recomputeTotalStorage());
    CHECK_EQ(net.totalStorage() + net.totalOutflowToOutlets() + net.totalWithdrawnToCoupler(),
             net.totalInjected());
    CHECK_EQ(c.graphUnitsToCA() + c.graphUnitsToOcean(), net.totalWithdrawnToCoupler());
    CHECK_EQ(c.graphUnitsToCA(), c.fillDeliveredToCA() * c.config().graphUnitsPerFill);
    CHECK_EQ(ca.totalVolume(), ca.recomputeVolume());
}

// A config with channel 1 switched OFF by budget, for the promotion tests:
// with the coupler injecting nothing, every wet cell in the world is one the
// test placed, so "what promotes and what does not" is not entangled with
// "where did the coupler decide to pour".
RiverCoupleConfig promotionOnlyConfig() {
    RiverCoupleConfig cfg;
    cfg.enabled = true;
    cfg.maxOutfallsPerTick = 0;
    cfg.sustainTicks = 8; // shorter than the 30 s default; the dwell RULE is what is under test
    return cfg;
}

} // namespace

// ===========================================================================
// Channel 1 + the ledger
// ===========================================================================

VXC_TEST(rivercouple_is_a_total_no_op_when_disabled) {
    Scenario s(10000, 1000);
    Terrain& t = s.terrain;
    WaterCA ca(solidFnFor(t));
    RiverCaCoupler coupler(s.net, ca, s.tiles, solidFnFor(t)); // enabled defaults to FALSE

    CHECK(s.net.segmentCount() > 0);
    Digest before;
    s.net.digest(before);

    for (int i = 0; i < 50; ++i) {
        s.net.injectInflow(0, 20000);
        s.net.step(1000);
        coupler.step();
    }

    CHECK_EQ(ca.totalVolume(), uint64_t(0));
    CHECK_EQ(coupler.graphUnitsToCA(), int64_t(0));
    CHECK_EQ(coupler.graphUnitsToOcean(), int64_t(0));
    CHECK_EQ(coupler.graphUnitsRefunded(), int64_t(0));
    CHECK_EQ(s.net.totalWithdrawnToCoupler(), int64_t(0));
    CHECK_EQ(coupler.trackedCandidateCount(), 0);
    // The graph still routes normally -- disabling the COUPLER must not
    // disable the network it is attached to.
    CHECK(s.net.totalOutflowToOutlets() > 0);
    Digest after;
    s.net.digest(after);
    CHECK(before.h != after.h);
}

VXC_TEST(rivercouple_conservation_exact_across_the_boundary) {
    Scenario s(10000, 1000);
    Terrain& t = s.terrain;
    // Carve the whole stem so the injected water has a bed to sit in rather
    // than sheeting off a plateau -- the situation W3's sibling carving pass
    // creates, and the one the outfall floor scan exists for.
    for (int64_t px = 0; px < kGridW; ++px) t.carved.insert({px, 0});
    t.trenchVoxels = 6;

    WaterCA ca(solidFnFor(t));
    RiverCoupleConfig cfg;
    cfg.enabled = true;
    cfg.promotionEnabled = false; // channel 1 in isolation
    RiverCaCoupler coupler(s.net, ca, s.tiles, solidFnFor(t), cfg);

    CHECK(s.net.segmentCount() >= 4);

    for (int i = 0; i < 200; ++i) {
        s.net.injectInflow(0, 20000);
        s.net.step(1000);
        coupler.step();
        ca.step();
        checkLedger(s.net, coupler, ca);
        // Nothing but this coupler ever adds water here, and the CA's own tick
        // conserves, so the CA's ledger must equal what the coupler handed it
        // -- the single strongest statement of "no volume goes missing at the
        // boundary" available.
        CHECK_EQ(ca.totalVolume(), uint64_t(coupler.fillDeliveredToCA()));
    }

    CHECK(coupler.fillDeliveredToCA() > 0);
    CHECK(ca.totalVolume() > 0);
    CHECK(ca.storedBrickCount() > 0);
}

VXC_TEST(rivercouple_refuses_gracefully_and_back_pressures_the_reach) {
    // A LIDDED world: exactly one air voxel per column, so every outfall can
    // hold at most 255 fill and the coupler's request is refused from the
    // second tick onward. The refund path is what keeps that from destroying
    // water, and the rising storage is the back-pressure it produces.
    Scenario s(10000, 1000);
    GridTiles& tiles = s.tiles;
    Terrain t;
    t.tiles = &tiles;
    WaterCA ca([&t](int64_t vx, int64_t vy, int64_t vz) {
        const int64_t px = floorDiv(vx * kVoxelSizeMm, kPixMm);
        const int64_t py = floorDiv(vy * kVoxelSizeMm, kPixMm);
        const int64_t bed = t.surfaceVz(px, py);
        return (vz == bed) ? MAT_AIR : MAT_ROCK; // one cell of headroom, then a lid
    });
    auto lid = [&t](int64_t vx, int64_t vy, int64_t vz) {
        const int64_t px = floorDiv(vx * kVoxelSizeMm, kPixMm);
        const int64_t py = floorDiv(vy * kVoxelSizeMm, kPixMm);
        return (vz == t.surfaceVz(px, py)) ? MAT_AIR : MAT_ROCK;
    };

    RiverCoupleConfig cfg;
    cfg.enabled = true;
    cfg.promotionEnabled = false;
    cfg.minTargetFill = 2040; // ask for far more than one cell can ever hold
    RiverCaCoupler coupler(s.net, ca, s.tiles, lid, cfg);

    int64_t storageAt40 = 0;
    for (int i = 0; i < 120; ++i) {
        s.net.injectInflow(0, 40000);
        s.net.step(1000);
        coupler.step();
        ca.step();
        checkLedger(s.net, coupler, ca);
        CHECK_EQ(ca.totalVolume(), uint64_t(coupler.fillDeliveredToCA()));
        if (i == 40) storageAt40 = s.net.totalStorage();
    }

    CHECK(coupler.graphUnitsRefunded() > 0);     // the CA really did refuse
    CHECK(coupler.fillDeliveredToCA() > 0);      // ...but not everything
    CHECK(s.net.totalStorage() > storageAt40);   // back-pressure: the reach stages up
}

VXC_TEST(rivercouple_ocean_is_the_sink_not_a_coastal_pool) {
    // The stem runs from +4 m down through sea level (voxel z == 0) to -5 m,
    // so the last several nodes are AT THE SEA.
    Scenario s(4000, 1000);
    Terrain& t = s.terrain;
    WaterCA ca(solidFnFor(t));
    RiverCoupleConfig cfg;
    cfg.enabled = true;
    cfg.promotionEnabled = false;
    RiverCaCoupler coupler(s.net, ca, s.tiles, solidFnFor(t), cfg);

    int oceanOutfalls = 0;
    for (uint32_t i = 0; i < s.net.segmentCount(); ++i)
        if (s.net.nodes()[s.net.segments()[i].toNode].elevationMm <= 0) ++oceanOutfalls;
    CHECK(oceanOutfalls > 0);

    int64_t storageAt50 = 0;
    for (int i = 0; i < 300; ++i) {
        s.net.injectInflow(0, 20000);
        s.net.step(1000);
        coupler.step();
        ca.step();
        checkLedger(s.net, coupler, ca);

        // Every reach that ends in the sea is emptied on the tick it is
        // serviced. The sea has no storage of its own, so nothing may pool.
        for (uint32_t k = 0; k < s.net.segmentCount(); ++k)
            if (s.net.nodes()[s.net.segments()[k].toNode].elevationMm <= 0)
                CHECK_EQ(s.net.segments()[k].storage, int32_t(0));

        if (i == 50) storageAt50 = s.net.totalStorage();
    }

    CHECK(coupler.graphUnitsToOcean() > 0);
    // Steady injection for 250 further ticks does NOT keep growing the
    // network's storage: the sink is doing its job. (This is the claim this
    // layer is responsible for -- the GRAPH does not pool at the coast. CA
    // water that flows downhill past the waterline afterwards is ordinary
    // gravity and belongs to the CA and the renderer, not to the coupler.)
    CHECK(s.net.totalStorage() <= storageAt50 * 2);
}

VXC_TEST(rivercouple_never_writes_ca_fill_at_or_below_sea_level) {
    // The placement half of section 3, isolated: a stem that is ENTIRELY at or
    // below sea level. Every reach is an ocean outfall, so if the rule holds,
    // the CA must end up completely empty while the graph's whole throughput
    // is accounted for in graphUnitsToOcean().
    Scenario s(-1000, 1000); // -1 m down to -10 m
    Terrain& t = s.terrain;
    WaterCA ca(solidFnFor(t));
    RiverCoupleConfig cfg;
    cfg.enabled = true;
    cfg.promotionEnabled = false;
    RiverCaCoupler coupler(s.net, ca, s.tiles, solidFnFor(t), cfg);

    CHECK(s.net.segmentCount() > 0);
    for (uint32_t i = 0; i < s.net.segmentCount(); ++i)
        CHECK(s.net.nodes()[s.net.segments()[i].toNode].elevationMm <= 0);

    for (int i = 0; i < 200; ++i) {
        s.net.injectInflow(0, 20000);
        s.net.step(1000);
        coupler.step();
        ca.step();
        checkLedger(s.net, coupler, ca);
    }

    CHECK_EQ(ca.totalVolume(), uint64_t(0));       // not one unit written into the sea
    CHECK_EQ(ca.storedBrickCount(), size_t(0));
    CHECK_EQ(coupler.fillDeliveredToCA(), int64_t(0));
    CHECK(coupler.graphUnitsToOcean() > 0);
    CHECK_EQ(coupler.graphUnitsToOcean(), s.net.totalWithdrawnToCoupler());
}

// ===========================================================================
// Channel 2: promotion. The negative controls carry more weight than the
// positive one -- see the file header.
// ===========================================================================

namespace {

// Drives the shared scenario with a caller-chosen wetting policy and reports
// how many promotions happened. `wet(tick)` is called immediately before each
// coupler tick, which is when the criterion is evaluated.
template <typename WetFn>
int64_t runPromotion(Scenario& s, WaterCA& ca, RiverCaCoupler& coupler, int ticks, WetFn wet) {
    for (int i = 0; i < ticks; ++i) {
        wet(i);
        s.net.step(1000);
        coupler.step();
        ca.step();
    }
    return coupler.promotionCount();
}

void carveDiversionScenario(Terrain& t) {
    t.trenchVoxels = 6;
    t.sealed = true;
    t.carved.insert({2, 0}); // the take-off node's pixel
    t.carved.insert({3, 1}); // the fresh ditch pixel
    t.carved.insert({3, 0}); // the node it rejoins
}

// A SOURCE at the take-off and a SINK at the outlet, driven once per tick.
// The drain is the load-bearing half: conjunct (5) requires the course to be
// STILL MOVING (a brick the CA had active last step), and a sealed channel
// that is merely topped up to full settles out of the active set within a
// tick or two and correctly stops qualifying. Water has to actually be going
// somewhere, which is the whole point of the criterion.
void driveDiversion(WaterCA& ca, Terrain& t, uint8_t fill, int64_t depth) {
    dryPixel(ca, t, 3, 0, 2); // the sink: water leaves the channel downstream
    wetPixel(ca, t, 2, 0, depth, fill);
    wetPixel(ca, t, 3, 1, depth, fill);
    wetPixel(ca, t, 3, 0, depth, fill);
}

} // namespace

VXC_TEST(rivercouple_promotes_a_sustained_diverted_channel) {
    Scenario s(10000, 1000);
    Terrain& t = s.terrain;
    carveDiversionScenario(t);
    WaterCA ca(solidFnFor(t));
    RiverCaCoupler coupler(s.net, ca, s.tiles, solidFnFor(t), promotionOnlyConfig());

    const size_t nodesBefore = s.net.nodes().size();
    const uint32_t segsBefore = s.net.segmentCount();
    CHECK(nodesBefore >= 9);

    const int64_t promoted =
        runPromotion(s, ca, coupler, 40, [&](int) { driveDiversion(ca, t, 255, 4); });

    CHECK_EQ(promoted, int64_t(1));
    if (s.net.segmentCount() != segsBefore + 2) return; // don't index past the end below
    // One fresh node (the ditch pixel) and two segments (take-off -> ditch,
    // ditch -> the rejoin node).
    CHECK_EQ(s.net.nodes().size(), nodesBefore + 1);
    CHECK_EQ(s.net.segmentCount(), segsBefore + 2);

    // The take-off is node 1 == pixel (2,0), and it now has TWO outflows: the
    // main stem it always had, plus the promoted channel. That is the
    // bifurcation, and it is the whole point of a diversion.
    CHECK_EQ(s.net.outgoingSegmentCount(1), uint32_t(2));
    const RiverNode& fresh = s.net.nodes()[nodesBefore];
    CHECK_EQ(fresh.vx, int64_t(3 * kPixVox));
    CHECK_EQ(fresh.vy, int64_t(1 * kPixVox));
    CHECK_EQ(fresh.elevationMm, int32_t(7300));

    // The promoted channel rejoins the network rather than dead-ending.
    const RiverSegment& tail = s.net.segments()[segsBefore + 1];
    CHECK_EQ(tail.fromNode, uint32_t(nodesBefore));
    CHECK_EQ(tail.toNode, uint32_t(2)); // node 2 == pixel (3,0)

    // And a diff was emitted carrying the DECISION, not the evidence.
    const std::vector<RiverDiffRecord> diffs = coupler.takePendingDiffs();
    CHECK_EQ(diffs.size(), size_t(1));
    CHECK(diffs[0].kind == RiverDiffKind::kDivertChannel);
    CHECK_EQ(diffs[0].headNode, uint32_t(1));
    CHECK_EQ(diffs[0].course.size(), size_t(2));
    CHECK(coupler.takePendingDiffs().empty()); // taking drains the queue
}

VXC_TEST(rivercouple_does_not_promote_a_puddle_or_a_flicker_or_a_dead_end) {
    const int kTicks = 120; // 15x the shortened dwell window

    // (5) NOT WET ENOUGH. A film of 20 fill per cell over the same geometry
    // that promotes at 255. This is the puddle case in its purest form: the
    // shape is right, the quantity is not.
    {
        Scenario s(10000, 1000);
        Terrain& t = s.terrain;
        carveDiversionScenario(t);
        WaterCA ca(solidFnFor(t));
        RiverCaCoupler coupler(s.net, ca, s.tiles, solidFnFor(t), promotionOnlyConfig());
        const int64_t n =
            runPromotion(s, ca, coupler, kTicks, [&](int) { driveDiversion(ca, t, 20, 1); });
        CHECK_EQ(n, int64_t(0));
        CHECK(ca.totalVolume() > 0); // there really was water; it just was not a channel
    }

    // (5) NOT SUSTAINED. Fully wet on even ticks, drained on odd ones. A
    // failing tick DISCARDS the candidate rather than decrementing it, so an
    // intermittent flow can never accumulate its way to a promotion.
    {
        Scenario s(10000, 1000);
        Terrain& t = s.terrain;
        carveDiversionScenario(t);
        WaterCA ca(solidFnFor(t));
        RiverCaCoupler coupler(s.net, ca, s.tiles, solidFnFor(t), promotionOnlyConfig());
        const int64_t n = runPromotion(s, ca, coupler, kTicks, [&](int tick) {
            if (tick % 2 == 0)
                driveDiversion(ca, t, 255, 4);
            else
                dryPixel(ca, t, 3, 1, 6);
        });
        CHECK_EQ(n, int64_t(0));
    }

    // (2) DEAD END. The ditch pixel is soaked, but the pixel it would drain
    // into is dry, and every other lower neighbour is dry too. That is a
    // RESERVOIR, and promoting it would tell the graph to route water into a
    // hole. Rejected structurally, not by threshold.
    {
        Scenario s(10000, 1000);
        Terrain& t = s.terrain;
        carveDiversionScenario(t);
        WaterCA ca(solidFnFor(t));
        RiverCaCoupler coupler(s.net, ca, s.tiles, solidFnFor(t), promotionOnlyConfig());
        const int64_t n = runPromotion(s, ca, coupler, kTicks, [&](int) {
            wetPixel(ca, t, 2, 0, 4, 255);
            wetPixel(ca, t, 3, 1, 4, 255);
            dryPixel(ca, t, 3, 0, 8); // kept dry against the spill from (3,1)
        });
        (void)0;
        CHECK_EQ(n, int64_t(0));
        CHECK_EQ(coupler.trackedCandidateCount(), 0); // never even became a candidate
    }

    // (4) TOO SHORT. The exact geometry that promotes above, with
    // minChannelPixels raised by one.
    {
        Scenario s(10000, 1000);
        Terrain& t = s.terrain;
        carveDiversionScenario(t);
        WaterCA ca(solidFnFor(t));
        RiverCoupleConfig cfg = promotionOnlyConfig();
        cfg.minChannelPixels = 3;
        RiverCaCoupler coupler(s.net, ca, s.tiles, solidFnFor(t), cfg);
        const int64_t n =
            runPromotion(s, ca, coupler, kTicks, [&](int) { driveDiversion(ca, t, 255, 4); });
        CHECK_EQ(n, int64_t(0));
    }

    // (1) NOT LEAVING A RIVER. The ditch and its outlet are soaked but the
    // take-off node itself is dry: whatever this water is, it did not come
    // out of the river, so it is not a diversion.
    {
        Scenario s(10000, 1000);
        Terrain& t = s.terrain;
        carveDiversionScenario(t);
        WaterCA ca(solidFnFor(t));
        RiverCaCoupler coupler(s.net, ca, s.tiles, solidFnFor(t), promotionOnlyConfig());
        const int64_t n = runPromotion(s, ca, coupler, kTicks, [&](int) {
            dryPixel(ca, t, 3, 0, 2);
            wetPixel(ca, t, 3, 1, 4, 255);
            wetPixel(ca, t, 3, 0, 4, 255);
            dryPixel(ca, t, 2, 0, 6); // the take-off itself is dry
        });
        CHECK_EQ(n, int64_t(0));
    }
}

VXC_TEST(rivercouple_promotion_moves_no_water_and_keeps_the_ledger_exact) {
    // Channel 2 is a TOPOLOGY edit that credits nothing (rivercouple.h
    // section 1). Proving that is a one-liner and it is the reason this
    // coupling cannot leak the way a two-way one can.
    Scenario s(10000, 1000);
    Terrain& t = s.terrain;
    carveDiversionScenario(t);
    WaterCA ca(solidFnFor(t));
    RiverCaCoupler coupler(s.net, ca, s.tiles, solidFnFor(t), promotionOnlyConfig());

    const int64_t promoted =
        runPromotion(s, ca, coupler, 40, [&](int) { driveDiversion(ca, t, 255, 4); });
    CHECK_EQ(promoted, int64_t(1));

    CHECK_EQ(s.net.totalWithdrawnToCoupler(), int64_t(0));
    CHECK_EQ(coupler.graphUnitsToCA(), int64_t(0));
    CHECK_EQ(coupler.graphUnitsToOcean(), int64_t(0));
    CHECK_EQ(ca.totalVolume(), ca.recomputeVolume());

    // Now turn channel 1 back on over the PROMOTED graph and prove the ledger
    // still closes with a bifurcation in it -- the split is a move, not a
    // rounding.
    RiverCoupleConfig live;
    live.enabled = true;
    live.promotionEnabled = false;
    RiverCaCoupler feeder(s.net, ca, s.tiles, solidFnFor(t), live);
    const uint64_t caBefore = ca.totalVolume();
    for (int i = 0; i < 100; ++i) {
        s.net.injectInflow(0, 20000);
        s.net.step(1000);
        feeder.step();
        ca.step();
        CHECK_EQ(s.net.totalStorage(), s.net.recomputeTotalStorage());
        CHECK_EQ(s.net.totalStorage() + s.net.totalOutflowToOutlets() +
                     s.net.totalWithdrawnToCoupler(),
                 s.net.totalInjected());
        CHECK_EQ(feeder.graphUnitsToCA() + feeder.graphUnitsToOcean(),
                 s.net.totalWithdrawnToCoupler());
    }
    CHECK_EQ(ca.totalVolume() - caBefore, uint64_t(feeder.fillDeliveredToCA()));
}

// ===========================================================================
// promoteChannel / bifurcation, as graph operations in their own right
// ===========================================================================

VXC_TEST(rivernet_promote_channel_rejects_every_malformed_course) {
    Scenario s(10000, 1000);
    RiverNetwork& net = s.net;
    CHECK(net.nodes().size() >= 4);

    const size_t nodes0 = net.nodes().size();
    const uint32_t segs0 = net.segmentCount();
    const auto unchanged = [&]() {
        CHECK_EQ(net.nodes().size(), nodes0);
        CHECK_EQ(net.segmentCount(), segs0);
    };

    const int32_t headElev = net.nodes()[1].elevationMm;
    const auto pt = [&](int64_t vx, int64_t vy, int32_t e) {
        RiverChannelPoint p;
        p.vx = vx;
        p.vy = vy;
        p.elevationMm = e;
        return p;
    };

    CHECK_EQ(net.promoteChannel(1, {}), RiverNetwork::kNoSegment); // empty course
    unchanged();
    CHECK_EQ(net.promoteChannel(9999, {pt(100, 100, headElev - 10)}), RiverNetwork::kNoSegment);
    unchanged();
    // Not strictly descending.
    CHECK_EQ(net.promoteChannel(1, {pt(100, 100, headElev)}), RiverNetwork::kNoSegment);
    unchanged();
    CHECK_EQ(net.promoteChannel(1, {pt(100, 100, headElev - 10), pt(101, 100, headElev - 10)}),
             RiverNetwork::kNoSegment);
    unchanged();
    // Duplicate position within the course.
    CHECK_EQ(net.promoteChannel(1, {pt(100, 100, headElev - 10), pt(100, 100, headElev - 20)}),
             RiverNetwork::kNoSegment);
    unchanged();
    // A non-final point naming an existing node.
    {
        RiverChannelPoint a = pt(100, 100, headElev - 10);
        a.existingNode = 3;
        CHECK_EQ(net.promoteChannel(1, {a, pt(101, 100, headElev - 20)}), RiverNetwork::kNoSegment);
        unchanged();
    }
    // A final point naming an out-of-range node.
    {
        RiverChannelPoint a = pt(100, 100, headElev - 10);
        a.existingNode = 9999;
        CHECK_EQ(net.promoteChannel(1, {a}), RiverNetwork::kNoSegment);
        unchanged();
    }
    // The head's EXISTING downstream node restated as a "diversion".
    {
        const RiverNode& stemTo = net.nodes()[net.segments()[net.outgoingSegment(1)].toNode];
        CHECK_EQ(net.promoteChannel(1, {pt(stemTo.vx, stemTo.vy, stemTo.elevationMm)}),
                 RiverNetwork::kNoSegment);
        unchanged();
    }
}

VXC_TEST(rivernet_bifurcation_splits_evenly_and_conserves_exactly) {
    Scenario s(10000, 1000);
    RiverNetwork& net = s.net;
    const uint32_t head = 1;
    const int32_t headElev = net.nodes()[head].elevationMm;
    const uint32_t stemSeg = net.outgoingSegment(head);
    CHECK(stemSeg != RiverNetwork::kNoSegment);

    // A three-point course ending in a FRESH terminal node -- i.e. a channel
    // that leaves the network and does not come back, which is what a river
    // running to the sea is.
    RiverChannelPoint a, b, c;
    a.vx = 500;  a.vy = 500;  a.elevationMm = headElev - 100;
    b.vx = 505;  b.vy = 505;  b.elevationMm = headElev - 200;
    c.vx = 510;  c.vy = 510;  c.elevationMm = headElev - 300;
    const uint32_t first = net.promoteChannel(head, {a, b, c});
    CHECK(first != RiverNetwork::kNoSegment);
    CHECK_EQ(net.outgoingSegmentCount(head), uint32_t(2));
    CHECK_EQ(net.segmentCount(), first + 3); // three new reaches, one per course point

    // Every new segment is a real downhill reach with a nonzero travel time.
    for (uint32_t k = first; k < net.segmentCount(); ++k) {
        CHECK(net.nodes()[net.segments()[k].fromNode].elevationMm >
              net.nodes()[net.segments()[k].toNode].elevationMm);
        CHECK(net.segments()[k].lengthMm > 0);
        CHECK(net.segments()[k].travelMillis >= 1);
    }

    // Route real water through the bifurcation. The ledger must close every
    // tick and BOTH branches must carry flow.
    for (int i = 0; i < 400; ++i) {
        net.injectInflow(0, 30000);
        net.step(1000);
        CHECK_EQ(net.totalStorage(), net.recomputeTotalStorage());
        CHECK_EQ(net.totalStorage() + net.totalOutflowToOutlets() + net.totalWithdrawnToCoupler(),
                 net.totalInjected());
    }
    CHECK(net.segments()[stemSeg].discharge > 0); // the main stem still runs
    CHECK(net.segments()[first].discharge > 0);   // ...and so does the diversion

    // A dammed reach must keep RECEIVING inflow (rivernet.h: this is why the
    // split is even rather than conveyance-weighted). Dam the stem and watch
    // its reservoir keep filling.
    net.setConveyance(stemSeg, 0);
    const int32_t stemStorageBefore = net.segments()[stemSeg].storage;
    for (int i = 0; i < 100; ++i) {
        net.injectInflow(0, 30000);
        net.step(1000);
    }
    CHECK_EQ(net.segments()[stemSeg].discharge, int32_t(0));
    CHECK(net.segments()[stemSeg].storage > stemStorageBefore);
    CHECK(net.segments()[first].discharge > 0); // the diversion is unaffected
    CHECK_EQ(net.totalStorage() + net.totalOutflowToOutlets() + net.totalWithdrawnToCoupler(),
             net.totalInjected());
}

VXC_TEST(rivernet_divert_channel_diff_replays_identically) {
    // The graph-diff log's whole promise: replaying the DECISION against a
    // freshly built graph reproduces byte-identical state.
    Scenario live(10000, 1000), replay(10000, 1000);
    CHECK_EQ(live.net.segmentCount(), replay.net.segmentCount());

    const int32_t headElev = live.net.nodes()[1].elevationMm;
    RiverChannelPoint a, b;
    a.vx = 500;  a.vy = 500;  a.elevationMm = headElev - 100;
    b.vx = 505;  b.vy = 505;  b.elevationMm = headElev - 200;

    CHECK(live.net.promoteChannel(1, {a, b}) != RiverNetwork::kNoSegment);

    RiverDiffRecord d;
    d.kind = RiverDiffKind::kDivertChannel;
    d.headNode = 1;
    d.course = {a, b};
    replay.net.applyGraphDiff(d);

    for (int i = 0; i < 120; ++i) {
        live.net.injectInflow(0, 25000);
        replay.net.injectInflow(0, 25000);
        live.net.step(1000);
        replay.net.step(1000);
    }
    Digest dLive, dReplay;
    live.net.digest(dLive);
    replay.net.digest(dReplay);
    CHECK_EQ(dLive.h, dReplay.h);

    // A kDivertChannel record with an EMPTY course stays inert, which is what
    // keeps the default-constructed record a no-op.
    Digest before;
    replay.net.digest(before);
    replay.net.applyGraphDiff(RiverDiffRecord{.kind = RiverDiffKind::kDivertChannel, .segId = 0, .value = 0});
    Digest after;
    replay.net.digest(after);
    CHECK_EQ(before.h, after.h);
}
