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
    replay.applyGraphDiff(RiverDiffRecord{RiverDiffKind::kSetConveyance, damSeg, 0});

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
    replay.applyGraphDiff(RiverDiffRecord{RiverDiffKind::kDivertChannel, 0, 0});
    Digest dReplayAfterStub;
    replay.digest(dReplayAfterStub);
    CHECK_EQ(dReplay.h, dReplayAfterStub.h);
}
