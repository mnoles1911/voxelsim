// River network segment-graph sim (plan §3.7 Layer R, W3 groundwork):
// flow-accumulation generation, Muskingum-class routing conservation, dam
// behavior, determinism, and graph-diff replay. See voxelcore/rivernet.h
// for the full design writeup.

#include "voxelcore/rivernet.h"

#include <vector>

#include "vxctest.h"

using namespace vxc;

namespace {

constexpr uint64_t kSeed = 20260719;

// A minimal, hand-verifiable synthetic slope: elevation strictly decreases
// eastward (px ascending) and is constant in y, so every pixel's unique
// D8 steepest-descent neighbor is due east -- one deterministic downhill
// chain the full width of any bounds box, with uniform per-pixel
// precipitation so accumulation (and therefore discharge) grows by exactly
// one constant increment per pixel along the chain. Deliberately simpler
// than vxc::SyntheticTileSampler so the flow/conservation/dam tests can
// assert exact structure by hand; the determinism test below separately
// exercises the real (noisier, branching) SyntheticTileSampler.
class LinearSlopeTileSampler final : public ITileSampler {
public:
    explicit LinearSlopeTileSampler(int32_t pixelSizeMm = 30000) : pixelSizeMm_(pixelSizeMm) {}

    int32_t pixelSizeMm() const override { return pixelSizeMm_; }

    int32_t elevationMm(int64_t px, int64_t /*py*/) override {
        return static_cast<int32_t>(100000 - px * 1000);
    }

    ClimateSample climate(int64_t /*px*/, int64_t /*py*/) override {
        ClimateSample c;
        c.precipitation = 100; // uniform baseflow generation per pixel
        return c;
    }

private:
    int32_t pixelSizeMm_;
};

RegionBounds slopeBounds() { return RegionBounds{0, 0, 19, 0}; }

// A hand-verifiable stand-in for the shipped water + flow planes: one channel
// running east along py == 4 from px 0 to px 9, its water surface descending 10
// mm per pixel, plus one CHANNEL-BIT-BUT-DRY cell at (5, 6) and one cell the
// source refuses to resolve at (7, 2).
//
// Those last two are the point of the fixture as much as the channel is:
// `requireWet` and the unresolved counter are the two ways buildFromBakedWater
// can quietly build the wrong graph, and neither is observable on a fixture
// where every cell is clean.
class SyntheticBakedWater final : public IBakedWaterSource {
public:
    int32_t pixelSizeMm() const override { return 1875; }

    bool waterAt(int64_t px, int64_t py, int32_t& outSurfaceMm, bool& outWet) override {
        if (px == 7 && py == 2) return false; // unresolvable: block not fetched
        if (py == 4 && px >= 0 && px <= 9) {
            outSurfaceMm = int32_t(1000 - px * 10);
            outWet = true;
            return true;
        }
        if (px == 5 && py == 6) {
            // A baked channel the bake left DRY -- a seasonal wash. It carries a
            // level band (a finite surface) and is still not water.
            outSurfaceMm = 900;
            outWet = false;
            return true;
        }
        // The dry answer. Spelled INT32_MIN rather than tilestore.h's
        // kNoWaterMm on purpose: rivernet.h depends on tiles.h and nothing
        // else, and a test that dragged the fine-tile decoder in to name one
        // sentinel would quietly make that untrue.
        outSurfaceMm = INT32_MIN;
        outWet = false;
        return true;
    }

    bool flowAt(int64_t px, int64_t py, uint8_t& outFlow) override {
        if (px == 7 && py == 2) return false;
        if ((py == 4 && px >= 0 && px <= 9) || (px == 5 && py == 6)) {
            // Accumulation grows downstream, as a real flow plane's does.
            const uint8_t log2 = uint8_t(4 + (px >= 0 && px <= 9 ? px / 2 : 0));
            outFlow = uint8_t(kFlowBitChannel | (log2 & kFlowLog2Mask));
            return true;
        }
        outFlow = 0; // no channel bit
        return true;
    }
};

} // namespace

VXC_TEST(rivernet_flow_accumulation_reaches_low_edge_monotonic_discharge) {
    LinearSlopeTileSampler tiles;
    RiverNetwork net;
    net.buildFromFlowAccumulation(tiles, kSeed, slopeBounds(), 500);

    CHECK(net.segmentCount() > 0);
    CHECK(net.nodes().size() > net.segments().size()); // exactly one more node than segment: a single chain

    // Every segment is genuinely downhill.
    for (const RiverSegment& seg : net.segments())
        CHECK(net.nodes()[seg.fromNode].elevationMm > net.nodes()[seg.toNode].elevationMm);

    // Discharge (== f(accumulated upstream precip) at build time) strictly
    // increases along the single main stem; build order is px-ascending
    // here, so segments()[k] IS the k-th reach downstream.
    for (size_t i = 1; i < net.segments().size(); ++i)
        CHECK(net.segments()[i].discharge > net.segments()[i - 1].discharge);

    // The graph reaches the low (east) edge of the region: the
    // highest-px qualifying node (last assigned id) has no outgoing
    // segment.
    const uint32_t lastNode = static_cast<uint32_t>(net.nodes().size() - 1);
    CHECK_EQ(net.outgoingSegment(lastNode), RiverNetwork::kNoSegment);

    // Node 0 (first to cross the threshold, lowest px) is a true headwater:
    // it never appears as any segment's toNode.
    bool node0IsDownstreamOfSomething = false;
    for (const RiverSegment& seg : net.segments())
        if (seg.toNode == 0) node0IsDownstreamOfSomething = true;
    CHECK(!node0IsDownstreamOfSomething);
}

VXC_TEST(rivernet_routing_conservation_exact_ledger) {
    LinearSlopeTileSampler tiles;
    RiverNetwork net;
    net.buildFromFlowAccumulation(tiles, kSeed, slopeBounds(), 500);
    CHECK(net.segmentCount() > 0);

    const uint32_t headwater = 0;
    net.injectInflow(headwater, 10000);
    CHECK_EQ(net.totalStorage(), int64_t(10000));
    CHECK_EQ(net.totalInjected(), int64_t(10000));
    CHECK_EQ(net.totalOutflowToOutlets(), int64_t(0));

    for (int i = 0; i < 300; ++i) {
        net.step(1000);
        CHECK_EQ(net.totalStorage(), net.recomputeTotalStorage());
        CHECK_EQ(net.totalStorage() + net.totalOutflowToOutlets(), net.totalInjected());
    }
    // 300 ticks at this chain's travel rate is enough for most of the
    // 10000 units to have reached the outlet.
    CHECK(net.totalOutflowToOutlets() > 5000);
}

VXC_TEST(rivernet_dam_upstream_rises_downstream_decays_conserved) {
    LinearSlopeTileSampler tiles;
    RiverNetwork net;
    net.buildFromFlowAccumulation(tiles, kSeed, slopeBounds(), 500);
    CHECK(net.segmentCount() >= 4);

    const uint32_t headwater = 0;
    const uint32_t damSeg = net.segmentCount() / 2;
    const uint32_t downstreamSeg = damSeg + 1;
    CHECK(downstreamSeg < net.segmentCount());

    // Continuous headwater baseflow so the dammed segment keeps receiving
    // fresh inflow after damming (otherwise it would simply plateau once
    // upstream drains -- still monotonic, but a weaker test).
    for (int i = 0; i < 50; ++i) {
        net.injectInflow(headwater, 500);
        net.step(1000);
    }

    net.setConveyance(damSeg, 0);

    int32_t prevDamStorage = net.segments()[damSeg].storage;
    int32_t prevDownstreamDischarge = net.segments()[downstreamSeg].discharge;
    bool downstreamEverDecreased = false;
    for (int i = 0; i < 200; ++i) {
        net.injectInflow(headwater, 500);
        net.step(1000);

        CHECK_EQ(net.totalStorage(), net.recomputeTotalStorage());
        CHECK_EQ(net.totalStorage() + net.totalOutflowToOutlets(), net.totalInjected());

        CHECK(net.segments()[damSeg].storage >= prevDamStorage); // upstream stage: monotonic non-decreasing
        prevDamStorage = net.segments()[damSeg].storage;

        if (net.segments()[downstreamSeg].discharge < prevDownstreamDischarge)
            downstreamEverDecreased = true;
        prevDownstreamDischarge = net.segments()[downstreamSeg].discharge;
    }

    CHECK_EQ(net.segments()[damSeg].discharge, int32_t(0)); // conveyance 0 -> outflow forced to exactly 0
    CHECK(downstreamEverDecreased);
    CHECK(net.segments()[downstreamSeg].discharge < 50); // decayed toward zero over 200 ticks
    CHECK(net.segments()[damSeg].storage > 0); // reservoir behind the dam actually accumulated something
}

VXC_TEST(rivernet_determinism_golden_digest) {
    SyntheticTileSampler tilesA(kSeed), tilesB(kSeed);
    RiverNetwork netA, netB;
    const RegionBounds bounds{-8, -8, 7, 7}; // 16x16 pixels: exercises real (branching) terrain
    netA.buildFromFlowAccumulation(tilesA, kSeed, bounds, 300);
    netB.buildFromFlowAccumulation(tilesB, kSeed, bounds, 300);

    CHECK_EQ(netA.segmentCount(), netB.segmentCount());
    CHECK(netA.segmentCount() > 0);

    // Identical stimulus on both: inject into every headwater segment (one
    // whose fromNode is never any segment's toNode), rather than a
    // hardcoded index, since the exact graph shape depends on the
    // synthetic terrain, not something this test should hardcode.
    std::vector<bool> isDownstreamOf(netA.nodes().size(), false);
    for (const RiverSegment& seg : netA.segments()) isDownstreamOf[seg.toNode] = true;
    for (uint32_t segId = 0; segId < netA.segmentCount(); ++segId) {
        if (isDownstreamOf[netA.segments()[segId].fromNode]) continue;
        netA.injectInflow(segId, 2000);
        netB.injectInflow(segId, 2000);
    }

    for (int i = 0; i < 50; ++i) {
        netA.step(1000);
        netB.step(1000);
    }

    Digest dA, dB;
    netA.digest(dA);
    netB.digest(dB);
    CHECK_EQ(dA.h, dB.h);
    CHECK_EQ(dA.h, 0xEC84E0B592821C38ull); // GOLDEN(rivernet_synthetic_slope)
    // Moved TWICE in the v8 wave, as predicted. First at 2a, when
    // SyntheticTileSampler began emitting the physical climate encoding and so
    // changed the precipitation byte this graph is weighted by (0xA4D30E5715339878
    // -> 0xB5EBA6B2D131223E). Then here, when the weight itself became mm/yr
    // rather than the raw byte (kRiverNetVersion 1 -> 2). Both are recorded
    // because a single re-pin would hide that two independent things moved. — kWorldGenVersion 3: synthetic-tile spectral-gap octaves (was 0xE4944F92B37F60FB at v2)
}

VXC_TEST(rivernet_graph_diff_dam_replay_matches_live) {
    LinearSlopeTileSampler tiles;
    const RegionBounds bounds = slopeBounds();

    RiverNetwork live, replay;
    live.buildFromFlowAccumulation(tiles, kSeed, bounds, 500);
    replay.buildFromFlowAccumulation(tiles, kSeed, bounds, 500);
    CHECK(live.segmentCount() >= 4);

    const uint32_t headwater = 0;
    const uint32_t damSeg = live.segmentCount() / 2;

    // Identical baseline stimulus on both live and (soon-to-be) replayed
    // networks.
    for (int i = 0; i < 30; ++i) {
        live.injectInflow(headwater, 700);
        replay.injectInflow(headwater, 700);
        live.step(1000);
        replay.step(1000);
    }

    // `live` applies the dam directly; `replay` applies the exact same
    // change ONLY via the graph-diff replay hook.
    live.setConveyance(damSeg, 0);
    // Designated, not positional: RiverDiffRecord grew `headNode` and `course`
    // for the real kDivertChannel, and a positional brace-init that stops at
    // `value` is an error under -Werror=missing-field-initializers on gcc and
    // clang (MSVC does not warn, so the local build was green). Naming the
    // fields also says what a conveyance diff actually carries.
    replay.applyGraphDiff(RiverDiffRecord{
        .kind = RiverDiffKind::kSetConveyance, .segId = damSeg, .value = 0});

    for (int i = 0; i < 60; ++i) {
        live.injectInflow(headwater, 700);
        replay.injectInflow(headwater, 700);
        live.step(1000);
        replay.step(1000);
    }

    Digest dLive, dReplay;
    live.digest(dLive);
    replay.digest(dReplay);
    CHECK_EQ(dLive.h, dReplay.h);
    CHECK_EQ(live.segments()[damSeg].storage, replay.segments()[damSeg].storage);
    CHECK(live.segments()[damSeg].storage > 0);

    // A diverted-channel diff is a documented no-op today: applying it
    // must not perturb state (no new segments, no digest change).
    // An EMPTY course is a rejected (therefore inert) divert diff by
    // construction -- that is what still makes this a no-op now that
    // kDivertChannel is implemented, and leaving `course` unnamed here would
    // hide the very field the assertion depends on.
    replay.applyGraphDiff(RiverDiffRecord{
        .kind = RiverDiffKind::kDivertChannel, .segId = 0, .value = 0, .headNode = 0, .course = {}});
    Digest dReplayAfterStub;
    replay.digest(dReplayAfterStub);
    CHECK_EQ(dReplay.h, dReplayAfterStub.h);
}

// ---------------------------------------------------------------------------
// buildFromBakedWater (water re-architecture Phase 2)
// ---------------------------------------------------------------------------

VXC_TEST(rivernet_baked_builder_follows_the_water_plane_not_a_client_d8) {
    SyntheticBakedWater water;
    RiverNetwork net;
    BakedWaterBuildParams p;
    p.bounds = RegionBounds{0, 0, 9, 9};
    const uint32_t segs = net.buildFromBakedWater(water, kSeed, p);

    // Ten channel cells along py == 4 become ten nodes and nine reaches; the
    // dry channel cell at (5,6) is excluded by requireWet (the default), and the
    // one unresolvable cell never becomes anything.
    CHECK_EQ(net.nodes().size(), size_t(10));
    CHECK_EQ(segs, uint32_t(9));

    // Every reach runs downhill IN WATER SURFACE, which is what the builder
    // orders on (a RiverNode's elevationMm is the water surface for a graph
    // built this way -- see rivernet.cpp).
    for (const RiverSegment& s : net.segments())
        CHECK(net.nodes()[s.fromNode].elevationMm > net.nodes()[s.toNode].elevationMm);

    // Build-time discharge is the CATCHMENT AREA IN m^2 here, decoded from the
    // flow byte's log2 bucket -- the third meaning of that field, and the
    // reason this is a separate entry point.
    CHECK_EQ(net.segments()[0].discharge, int32_t(1) << 4); // px 0 -> log2 4
    CHECK_EQ(net.segments()[8].discharge, int32_t(1) << 8); // px 8 -> log2 8

    // Exactly one headwater (px 0) and one terminal (px 9).
    CHECK_EQ(net.headwaterNodes().size(), size_t(1));
    CHECK_EQ(net.headwaterNodes()[0], uint32_t(0));
    CHECK_EQ(net.headwaterSegments().size(), size_t(1));
    CHECK_EQ(net.headwaterSegments()[0], uint32_t(0));
    CHECK_EQ(net.outgoingSegment(uint32_t(net.nodes().size() - 1)), RiverNetwork::kNoSegment);

    // THE RAN-FLAGS. "scanned 100 cells, found 11 channel cells, could not
    // resolve 2 of them" is a different report from silence, and the unresolved
    // count is what tells a caller its graph is short a reach rather than that
    // the valley is dry.
    CHECK_EQ(net.bakedCellsScanned(), uint64_t(100));
    CHECK_EQ(net.bakedChannelCells(), uint64_t(11)); // 10 wet + 1 dry wash
    CHECK_EQ(net.bakedCellsUnresolved(), uint64_t(1));
}

VXC_TEST(rivernet_baked_builder_requireWet_and_threshold_are_real_gates) {
    SyntheticBakedWater water;

    // requireWet off admits the dry wash: an eleventh node, still no eleventh
    // segment (it has no qualifying neighbour).
    {
        RiverNetwork net;
        BakedWaterBuildParams p;
        p.bounds = RegionBounds{0, 0, 9, 9};
        p.requireWet = false;
        net.buildFromBakedWater(water, kSeed, p);
        CHECK_EQ(net.nodes().size(), size_t(11));
        CHECK_EQ(net.segmentCount(), uint32_t(9));
        CHECK_EQ(net.headwaterNodes().size(), size_t(2)); // the wash is its own head
    }

    // A threshold above the headwater's bucket clips the top of the chain
    // rather than the whole thing -- the direct analogue of accumThreshold.
    {
        RiverNetwork net;
        BakedWaterBuildParams p;
        p.bounds = RegionBounds{0, 0, 9, 9};
        p.minAccumLog2 = 7; // px >= 6
        net.buildFromBakedWater(water, kSeed, p);
        CHECK_EQ(net.nodes().size(), size_t(4)); // px 6..9
        CHECK_EQ(net.segmentCount(), uint32_t(3));
    }

    // And a threshold nothing meets gives an EMPTY graph with a non-zero
    // channel-cell count, which is exactly how a caller tells "no river here"
    // from "the threshold is wrong".
    {
        RiverNetwork net;
        BakedWaterBuildParams p;
        p.bounds = RegionBounds{0, 0, 9, 9};
        p.minAccumLog2 = 31;
        CHECK_EQ(net.buildFromBakedWater(water, kSeed, p), uint32_t(0));
        CHECK(net.bakedChannelCells() > 0);
    }
}

VXC_TEST(rivernet_baked_builder_graph_is_acyclic_and_routes_conservatively) {
    // The strict-total-order rule's purpose: no cycle can close, so water
    // injected anywhere reaches an outlet and the ledger stays exact.
    SyntheticBakedWater water;
    RiverNetwork net;
    BakedWaterBuildParams p;
    p.bounds = RegionBounds{0, 0, 9, 9};
    net.buildFromBakedWater(water, kSeed, p);
    CHECK(net.segmentCount() > 0);

    net.injectInflow(net.headwaterSegments()[0], 100000);
    for (int i = 0; i < 200; ++i) {
        net.step(1000);
        CHECK_EQ(net.totalStorage(), net.recomputeTotalStorage());
        CHECK_EQ(net.totalStorage() + net.totalOutflowToOutlets(), net.totalInjected());
    }
    // A cyclic graph would recirculate forever and never drain.
    CHECK_EQ(net.totalOutflowToOutlets(), int64_t(100000));
    CHECK_EQ(net.totalStorage(), int64_t(0));
}

VXC_TEST(rivernet_nearest_segment_finds_the_outlet_reach_and_refuses_far_ones) {
    SyntheticBakedWater water;
    RiverNetwork net;
    BakedWaterBuildParams p;
    p.bounds = RegionBounds{0, 0, 9, 9};
    net.buildFromBakedWater(water, kSeed, p);

    // Node k sits at voxel floorDiv(k*1875, 100) == k*18 (+ rounding), py == 4
    // -> vy == 75. Ask right on top of node 3.
    const RiverNode& n3 = net.nodes()[3];
    CHECK_EQ(net.nearestSegmentToVoxel(n3.vx, n3.vy, 10000), uint32_t(3));

    // Far away with a generous budget still finds SOMETHING (the graph is one
    // chain); far away with a tight budget must refuse rather than reach, which
    // is what stops a spill teleporting into an unrelated valley.
    CHECK(net.nearestSegmentToVoxel(100000, 100000, 1LL << 40) != RiverNetwork::kNoSegment);
    CHECK_EQ(net.nearestSegmentToVoxel(100000, 100000, 1000), RiverNetwork::kNoSegment);

    RiverNetwork empty;
    CHECK_EQ(empty.nearestSegmentToVoxel(0, 0, 1LL << 40), RiverNetwork::kNoSegment);
}

// ---------------------------------------------------------------------------
// Persistence (RiverNetState)
// ---------------------------------------------------------------------------

VXC_TEST(rivernet_state_blob_round_trip_is_digest_exact_including_diffs) {
    // THE CLAIM rivernet.h:127-149 makes -- that a diff log replayed against a
    // freshly built graph reproduces the live state -- VERIFIED here for the
    // save path, and with the routing state on top, because the log alone
    // reproduces the dam and not the reservoir piled up behind it.
    LinearSlopeTileSampler tiles;
    RiverNetwork live;
    live.buildFromFlowAccumulation(tiles, kSeed, slopeBounds(), 500);
    CHECK(live.segmentCount() >= 4);
    live.setDiffRecording(true);

    const uint32_t headwater = 0;
    const uint32_t damSeg = live.segmentCount() / 2;
    for (int i = 0; i < 20; ++i) {
        live.injectInflow(headwater, 700);
        live.step(1000);
    }
    live.setConveyance(damSeg, 0);          // recorded
    live.setConveyance(damSeg + 1, 128);    // recorded
    for (int i = 0; i < 40; ++i) {
        live.injectInflow(headwater, 700);
        live.step(1000);
    }
    CHECK_EQ(live.diffLog().size(), size_t(2));
    CHECK(live.segments()[damSeg].storage > 0);

    std::vector<uint8_t> blob;
    RiverNetState::serialize(live, blob);
    CHECK(blob.size() > 16);

    // The load path is: rebuild the base graph the same way, then apply the
    // blob. That is what the UE host does after a restart.
    RiverNetwork loaded;
    loaded.buildFromFlowAccumulation(tiles, kSeed, slopeBounds(), 500);
    loaded.setDiffRecording(true);
    CHECK(RiverNetState::load(blob.data(), blob.size(), loaded));

    Digest dLive, dLoaded;
    live.digest(dLive);
    loaded.digest(dLoaded);
    CHECK_EQ(dLive.h, dLoaded.h);
    CHECK_EQ(loaded.totalStorage(), live.totalStorage());
    CHECK_EQ(loaded.totalInjected(), live.totalInjected());
    CHECK_EQ(loaded.totalOutflowToOutlets(), live.totalOutflowToOutlets());
    CHECK_EQ(loaded.totalStorage(), loaded.recomputeTotalStorage());
    CHECK_EQ(loaded.totalStorage() + loaded.totalOutflowToOutlets(), loaded.totalInjected());

    // applyGraphDiff dispatches through setConveyance, so a recording graph
    // rebuilds an identical log -- which is what makes save -> load -> save
    // byte-identical rather than merely equivalent.
    CHECK_EQ(loaded.diffLog().size(), size_t(2));
    std::vector<uint8_t> again;
    RiverNetState::serialize(loaded, again);
    CHECK(again == blob);

    // And the two graphs keep agreeing once they are ticking again, which is
    // the property a save is actually for.
    for (int i = 0; i < 30; ++i) {
        live.injectInflow(headwater, 700);
        loaded.injectInflow(headwater, 700);
        live.step(1000);
        loaded.step(1000);
    }
    Digest eLive, eLoaded;
    live.digest(eLive);
    loaded.digest(eLoaded);
    CHECK_EQ(eLive.h, eLoaded.h);
}

VXC_TEST(rivernet_state_blob_carries_a_promoted_channel_through_a_reload) {
    LinearSlopeTileSampler tiles;
    RiverNetwork live;
    live.buildFromFlowAccumulation(tiles, kSeed, slopeBounds(), 500);
    live.setDiffRecording(true);
    CHECK(live.nodes().size() >= 6);

    // A diversion leaving node 2 and running to a fresh terminal. Recorded only
    // because it SUCCEEDS -- a rejected promotion mutates nothing and must not
    // enter the log.
    const RiverNode& head = live.nodes()[2];
    std::vector<RiverChannelPoint> course{
        RiverChannelPoint{head.vx + 7, head.vy + 7, head.elevationMm - 500,
                          RiverNetwork::kNoNode},
        RiverChannelPoint{head.vx + 14, head.vy + 14, head.elevationMm - 900,
                          RiverNetwork::kNoNode}};
    CHECK(live.promoteChannel(2, course) != RiverNetwork::kNoSegment);
    CHECK_EQ(live.diffLog().size(), size_t(1));
    // A rejected one (uphill course) leaves the log alone.
    CHECK_EQ(live.promoteChannel(2, {RiverChannelPoint{head.vx + 1, head.vy, head.elevationMm + 1,
                                                       RiverNetwork::kNoNode}}),
             RiverNetwork::kNoSegment);
    CHECK_EQ(live.diffLog().size(), size_t(1));

    for (int i = 0; i < 25; ++i) {
        live.injectInflow(0, 900);
        live.step(1000);
    }

    std::vector<uint8_t> blob;
    RiverNetState::serialize(live, blob);

    RiverNetwork loaded;
    loaded.buildFromFlowAccumulation(tiles, kSeed, slopeBounds(), 500);
    CHECK(RiverNetState::load(blob.data(), blob.size(), loaded));
    CHECK_EQ(loaded.segmentCount(), live.segmentCount()); // the diversion came back
    CHECK_EQ(loaded.outgoingSegmentCount(2), live.outgoingSegmentCount(2));
    CHECK_EQ(loaded.outgoingSegmentCount(2), uint32_t(2)); // the bifurcation itself

    Digest dLive, dLoaded;
    live.digest(dLive);
    loaded.digest(dLoaded);
    CHECK_EQ(dLive.h, dLoaded.h);
}

VXC_TEST(rivernet_state_blob_refuses_every_way_it_can_be_wrong) {
    LinearSlopeTileSampler tiles;
    RiverNetwork live;
    live.buildFromFlowAccumulation(tiles, kSeed, slopeBounds(), 500);
    live.setDiffRecording(true);
    live.setConveyance(1, 200);
    live.injectInflow(0, 5000);
    live.step(1000);

    std::vector<uint8_t> good;
    RiverNetState::serialize(live, good);

    const auto freshGraph = [&]() {
        RiverNetwork n;
        n.buildFromFlowAccumulation(tiles, kSeed, slopeBounds(), 500);
        return n;
    };

    {
        RiverNetwork n = freshGraph();
        std::vector<uint8_t> bad = good;
        bad[0] ^= 0xFF;
        CHECK(!RiverNetState::load(bad.data(), bad.size(), n));
    }
    {
        RiverNetwork n = freshGraph();
        std::vector<uint8_t> bad = good;
        bad[4] = uint8_t(kRiverNetVersion + 1);
        CHECK(!RiverNetState::load(bad.data(), bad.size(), n));
    }
    {
        RiverNetwork n = freshGraph();
        std::vector<uint8_t> bad = good;
        bad.pop_back();
        CHECK(!RiverNetState::load(bad.data(), bad.size(), n));
    }
    {
        RiverNetwork n = freshGraph();
        std::vector<uint8_t> bad = good;
        bad.push_back(0); // trailing bytes
        CHECK(!RiverNetState::load(bad.data(), bad.size(), n));
    }
    {
        // A blob whose ledger totals do not satisfy the header's exact-integer
        // identity is refused WITHOUT partially applying, which is the whole
        // reason restoreRoutingState validates before it writes.
        RiverNetwork n = freshGraph();
        std::vector<uint8_t> bad = good;
        bad[bad.size() - 24] ^= 0x02; // totalInjected
        CHECK(!RiverNetState::load(bad.data(), bad.size(), n));
        CHECK_EQ(n.totalInjected(), int64_t(0));
        CHECK_EQ(n.totalStorage(), int64_t(0));
    }
    {
        // A blob for a graph of a different size: same failure, and the reason
        // the segment count is on the wire at all.
        RiverNetwork small;
        small.buildFromFlowAccumulation(tiles, kSeed, RegionBounds{0, 0, 5, 0}, 500);
        CHECK(!RiverNetState::load(good.data(), good.size(), small));
    }
}
