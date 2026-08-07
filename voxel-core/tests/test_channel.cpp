// River channel geometry (voxelcore/channel.h, W3 visible half): the
// discharge->width/depth power law, the graded bed and its strict descent,
// centreline continuity (a bed with a gap is a dam), reaching sea level,
// bank containment (a bank below the water line drains the reach
// sideways), and determinism. See channel.h for the design writeup.

#include "voxelcore/channel.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/rivernet.h"
#include "voxelcore/tiles.h"
#include "vxctest.h"

using namespace vxc;

namespace {

constexpr uint64_t kSeed = 20260719;

// A hand-verifiable convergent valley: elevation falls 1 m per pixel
// eastward and rises 0.1 m per pixel away from the y=0 axis. Every pixel's
// D8 steepest descent therefore runs east along py=0 and diagonally inward
// everywhere else, so the network is a main stem with tributaries joining
// it -- enough structure to exercise confluences and downstream discharge
// growth, while still being predictable by hand.
//
// The stem crosses sea level (z=0) at px=100 and keeps going, so a region
// wider than that contains a genuine coastline and a river mouth. That is
// what makes the "reaches the sea" test a real test rather than a tautology.
class ValleyTileSampler final : public ITileSampler {
public:
    int32_t pixelSizeMm() const override { return 30000; }

    int32_t elevationMm(int64_t px, int64_t py) override {
        return static_cast<int32_t>(100000 - px * 1000 + std::abs(py) * 100);
    }

    ClimateSample climate(int64_t, int64_t) override {
        ClimateSample c;
        c.precipitation = 100; // uniform, so accumulation tracks catchment area
        return c;
    }
};

RegionBounds valleyBounds() { return RegionBounds{0, -24, 179, 24}; }

// A built world. The channel bed is cut into the AMPLIFIED surface, which
// is what the game renders and collides against; referencing the raw tile
// elevation instead is the bug channel.h's header comment records, so every
// fixture here goes through the amplifier.
struct Built {
    RiverNetwork net;
    ChannelField field;
};

template <typename Tiles>
void buildOver(Tiles& tiles, const RegionBounds& bounds, Built& out) {
    out.net.buildFromFlowAccumulation(tiles, kSeed, bounds);
    Amplifier amp(kSeed, tiles);
    auto surface = channelSurfaceOf(amp);
    out.field.build(tiles, surface, out.net, bounds);
}

void buildValley(ValleyTileSampler& tiles, Built& out) {
    buildOver(tiles, valleyBounds(), out);
}

// Walks every reach centreline voxel by voxel.
//
//   gaps         columns where the channel has no opinion at all -- the bed
//                is interrupted, which is a dam.
//   aboveOwnBed  columns where the carved ground sits ABOVE that reach's
//                own graded bed, which is the same dam by a subtler route.
//
// A column influenced by a DIFFERENT reach than its own counts as neither:
// a tributary running into a trunk's valley is correctly subsumed by the
// trunk's deeper cut, and the carved ground there is BELOW the tributary
// bed, not above it. Checking carved height rather than a membership flag
// is what tells those two apart -- the first cut of this test used
// membership and reported 13760 false failures on exactly that case.
struct Continuity {
    int64_t columns = 0;
    int64_t gaps = 0;
    int64_t aboveOwnBed = 0;
};

Continuity walkCentrelines(const RiverNetwork& net, const ChannelField& field) {
    Continuity c;
    for (const RiverSegment& seg : net.segments()) {
        const RiverNode& a = net.nodes()[seg.fromNode];
        const RiverNode& b = net.nodes()[seg.toNode];
        const int64_t dx = b.vx - a.vx, dy = b.vy - a.vy;
        const int64_t steps = std::max(std::abs(dx), std::abs(dy));
        if (steps <= 0) continue;
        // Half-open [A, B): the junction voxel belongs to the next reach, so
        // it is counted exactly once across the whole network.
        for (int64_t s = 0; s < steps; ++s) {
            const int64_t vx = a.vx + (dx * s) / steps;
            const int64_t vy = a.vy + (dy * s) / steps;
            ++c.columns;
            const ChannelSample cs = field.sampleAt(vx, vy);
            if (!cs.influenced) {
                ++c.gaps;
                continue;
            }
            // Cut from arbitrarily high ground, so the result is purely the
            // channel's own target here and not an artefact of the fixture.
            const int64_t bedA = field.nodeBedMm()[seg.fromNode];
            const int64_t bedB = field.nodeBedMm()[seg.toNode];
            const int64_t ownBed = bedA + ((bedB - bedA) * s) / steps;
            if (field.surfaceMm(vx, vy, 1000000) > ownBed) ++c.aboveOwnBed;
        }
    }
    return c;
}

} // namespace

// --- the integer log/exp the power law is built on -------------------------

VXC_TEST(channel_log2_exp2_are_monotone_and_round_trip) {
    // Monotone non-decreasing is the property that makes channelWidthMm /
    // channelDepthMm monotone in discharge; without it a slightly larger
    // catchment could produce a slightly narrower river.
    int64_t prev = log2Q8(1);
    for (int64_t v = 1; v < 200000; v = v + 1 + v / 64) {
        const int64_t l = log2Q8(v);
        CHECK(l >= prev);
        prev = l;
    }
    int64_t prevE = exp2Q8(0);
    for (int64_t l = 0; l < 24 * 256; ++l) {
        const int64_t e = exp2Q8(l);
        CHECK(e >= prevE);
        prevE = e;
    }

    // Exact on powers of two, and within 1% elsewhere -- the mantissa
    // tables are 16-entry with linear interpolation, so this bounds the
    // combined table + interpolation + flooring error.
    for (int32_t e = 0; e < 30; ++e) {
        const int64_t v = int64_t{1} << e;
        CHECK_EQ(log2Q8(v), int64_t{e} * 256);
        CHECK_EQ(exp2Q8(int64_t{e} * 256), v);
    }
    // Round-trip accuracy over the domain the channel law actually uses
    // (widths and depths, all >= kChannelRefDepthMm = 300). Measured worst
    // case is 1.09%. Below ~256 the error is dominated by integer
    // resolution rather than by the tables -- exp2Q8 floors -- which is why
    // the domain has a floor rather than a looser bound.
    for (int64_t v = 300; v < 5000000; v = v + 1 + v / 37) {
        const int64_t rt = exp2Q8(log2Q8(v));
        CHECK(rt > 0);
        CHECK(std::abs(rt - v) * 50 <= v); // <= 2%
    }
}

// --- goal 1: channel geometry from discharge -------------------------------

VXC_TEST(channel_width_and_depth_scale_with_discharge) {
    const int64_t qRef = kRiverAccumThresholdDefault;

    // At and below the river-formation threshold the channel is exactly the
    // reference trickle -- not zero, not a trench.
    CHECK_EQ(channelWidthMm(0), kChannelRefWidthMm);
    CHECK_EQ(channelDepthMm(0), kChannelRefDepthMm);
    CHECK_EQ(channelWidthMm(qRef), kChannelRefWidthMm);
    CHECK_EQ(channelDepthMm(qRef), kChannelRefDepthMm);

    // Monotone non-decreasing across four decades of discharge.
    int64_t pw = 0, pd = 0;
    for (int64_t q = 1; q < qRef * 10000; q = q + 1 + q / 48) {
        const int64_t w = channelWidthMm(q), d = channelDepthMm(q);
        CHECK(w >= pw);
        CHECK(d >= pd);
        CHECK(w <= kChannelMaxWidthMm);
        CHECK(d <= kChannelMaxDepthMm);
        pw = w;
        pd = d;
    }

    // A HEADWATER TRICKLE AND A MAJOR RIVER ARE NOT THE SAME TRENCH. A
    // catchment 10,000x the threshold is a major river; width ~ Q^0.40 puts
    // it around 10000^0.4 = 40x the reference width, depth ~ Q^0.35 around
    // 25x. Bounded generously either side so this tests the shape of the
    // law, not the exact rounding of the mantissa tables.
    // MEASURED AT THIS WORLD'S OWN LARGEST RIVER, not at an arbitrary decade.
    // The biggest discharge anywhere on land is 3.91e8 m3/yr against a
    // perennial anchor of 3.16e5, i.e. about 1240x -- so that is the number
    // that decides whether a trunk reads as a trunk. 10000x used to be the
    // probe here and is now past the width cap under the exaggerated
    // exponents, which would have tested the CLAMP rather than the law.
    const int64_t bigQ = qRef * 1240;
    const int64_t bigW = channelWidthMm(bigQ), bigD = channelDepthMm(bigQ);
    // A major river: well over 100 m wide and deep enough to drown in, which
    // is the whole point of moving off Earth's exponents.
    CHECK(bigW >= 100'000 && bigW <= 200'000);
    CHECK(bigD >= 8'000 && bigD <= 14'000);
    // AND STILL OFF BOTH CAPS. If the world's largest river clamps, every
    // large river clamps to the SAME size and the exaggeration has recreated
    // the flatness it was meant to cure, one decade higher up.
    CHECK(bigW < kChannelMaxWidthMm);
    CHECK(bigD < kChannelMaxDepthMm);

    // Depth grows more slowly than width, as hydraulic geometry requires --
    // rivers get wide faster than they get deep.
    CHECK(bigW * kChannelRefDepthMm > bigD * kChannelRefWidthMm);

    // Checked against the ideal power law, decade by decade, and the TOLERANCE
    // is the test rather than the numbers: the integer law must track
    // 1500*m^(166/256) and 300*m^(128/256) to better than 1.6%, which is what
    // says the log2/exp2 pair is really computing a power law and not merely
    // something monotone.
    //
    // THE EXACT INTEGER PINS ARE GONE ON PURPOSE. They encoded the OLD
    // exponents and nothing else -- changing 0.3984 to 0.6484 failed six of
    // them while every property this test exists to defend still held. A pin
    // that only restates the current constant does not defend a behaviour, it
    // just has to be rewritten whenever the constant is a decision rather than
    // a mistake. Determinism is still covered: the ideal columns are computed
    // from the SAME Q8 constants the law uses, so a mantissa-table regression
    // moves one and not the other.
    //
    // Capped rows are skipped: above the cap the law is flat by design and
    // comparing it to an uncapped ideal would fail for the right reason.
    struct Row {
        int64_t mult;
        double idealW, idealD;
    };
    const Row rows[] = {
        {1, 1500.0, 300.0},
        {10, 6676.0, 949.0},
        {100, 29714.0, 3000.0},
        {1000, 132252.0, 9487.0},
    };
    for (const Row& r : rows) {
        const int64_t w = channelWidthMm(qRef * r.mult);
        const int64_t d = channelDepthMm(qRef * r.mult);
        if (w < kChannelMaxWidthMm) {
            CHECK(std::abs(double(w) - r.idealW) * 63.0 <= r.idealW); // < 1.6%
        }
        if (d < kChannelMaxDepthMm) {
            CHECK(std::abs(double(d) - r.idealD) * 63.0 <= r.idealD);
        }
    }
}

VXC_TEST(channel_cross_section_is_a_trapezoid) {
    const int64_t bed = -5000, depth = 2000, width = 8000;
    const int64_t halfW = width / 2;
    const int64_t run = channelBankRunMm(depth);

    // Flat bed out to the half-width.
    CHECK_EQ(channelTargetMm(0, bed, depth, width), bed);
    CHECK_EQ(channelTargetMm(halfW, bed, depth, width), bed);

    // Then a straight bank plane rising to exactly the rim at the bank run.
    CHECK_EQ(channelTargetMm(halfW + run, bed, depth, width), bed + depth);
    CHECK_EQ(channelTargetMm(halfW + run / 2, bed, depth, width), bed + depth / 2);

    // Monotone non-decreasing with distance, and it keeps rising past the
    // rim so the cut daylights into the hillside instead of ending in a
    // vertical wall.
    int64_t prev = bed - 1;
    for (int64_t d = 0; d <= halfW + run * 6; d += 37) {
        const int64_t t = channelTargetMm(d, bed, depth, width);
        CHECK(t >= prev);
        prev = t;
    }
    CHECK(channelTargetMm(halfW + run * 3, bed, depth, width) > bed + depth);
}

// --- goal 2 + the bed's central invariant ----------------------------------

VXC_TEST(channel_bed_descends_strictly_downstream) {
    ValleyTileSampler tiles;
    Built b;
    buildValley(tiles, b);

    CHECK(b.field.stats().segments > 0);
    // The invariant from channel.h: the downstream pass takes
    // min(surface - depth, upstreamBed - minDrop), so the bed strictly
    // descends no matter what the surface does. Cutting through a sill is
    // allowed and expected; climbing over one is not.
    CHECK(b.field.bedIsStrictlyDescending());

    // Checked directly as well, so a bug in bedIsStrictlyDescending itself
    // cannot make this test vacuous, and with the minimum drop actually
    // enforced rather than merely "not increasing".
    for (const RiverSegment& seg : b.net.segments()) {
        const int64_t up = b.field.nodeBedMm()[seg.fromNode];
        const int64_t down = b.field.nodeBedMm()[seg.toNode];
        CHECK(down < up);
        CHECK(up - down >= channelMinDropMm(seg.lengthMm));
        // Discharge never decreases downstream -- the property the width and
        // depth laws rest on.
        CHECK(b.field.nodeDischarge()[seg.toNode] >= b.field.nodeDischarge()[seg.fromNode]);
    }

    // The bed is genuinely below the AMPLIFIED ground it was cut from -- at
    // least a full channel depth, everywhere. That is the invariant the
    // tile-elevation datum could not provide: measured against the tile
    // instead, the amplifier's own detail put the bed above ground on this
    // very fixture.
    Amplifier amp(kSeed, tiles);
    for (size_t n = 0; n < b.net.nodes().size(); ++n) {
        const RiverNode& nd = b.net.nodes()[n];
        const int64_t ground = amp.surfaceMm(nd.vx, nd.vy);
        const int64_t depth = channelDepthMm(b.field.nodeDischarge()[n]);
        CHECK(b.field.nodeBedMm()[n] <= ground - depth);
    }

    // The cut stays a river valley rather than becoming a canyon: no node's
    // bed is more than a few channel depths below its own ground.
    CHECK(b.field.stats().maxCutBelowSurfaceMm > 0);
    CHECK(b.field.stats().maxCutBelowSurfaceMm < 60'000); // < 60 m
}

VXC_TEST(channel_bed_descends_strictly_on_synthetic_terrain) {
    // The same invariant on real (noisy, branching, pit-ridden) terrain
    // rather than the hand-built valley.
    SyntheticTileSampler tiles(kSeed);
    Built b;
    buildOver(tiles, RegionBounds{-96, -96, 95, 95}, b);

    CHECK(b.field.stats().segments > 0);
    CHECK(b.field.bedIsStrictlyDescending());
    for (const RiverSegment& seg : b.net.segments())
        CHECK(b.field.nodeBedMm()[seg.toNode] < b.field.nodeBedMm()[seg.fromNode]);
}

// --- goal: they must not have gaps -----------------------------------------

VXC_TEST(channel_has_no_gaps_along_any_reach) {
    ValleyTileSampler tiles;
    Built b;
    buildValley(tiles, b);

    const Continuity c = walkCentrelines(b.net, b.field);
    CHECK(c.columns > 1000); // the walk is actually covering ground
    // A GAP IS A DAM. Every voxel column on every reach centreline, from
    // every headwater to every outlet, is carved, and none is left standing
    // above its own reach's graded bed.
    CHECK_EQ(c.gaps, int64_t{0});
    CHECK_EQ(c.aboveOwnBed, int64_t{0});
}

VXC_TEST(channel_has_no_gaps_on_synthetic_terrain) {
    SyntheticTileSampler tiles(kSeed);
    Built b;
    buildOver(tiles, RegionBounds{-96, -96, 95, 95}, b);

    const Continuity c = walkCentrelines(b.net, b.field);
    CHECK(c.columns > 1000);
    CHECK_EQ(c.gaps, int64_t{0});
    CHECK_EQ(c.aboveOwnBed, int64_t{0});
}

VXC_TEST(channel_is_continuous_across_confluences) {
    ValleyTileSampler tiles;
    Built b;
    buildValley(tiles, b);

    // Find a node with more than one incoming reach -- a confluence -- and
    // check the bed is single-valued and continuous there: sampling the
    // junction voxel gives one bed, and it is at most the bed of every
    // reach feeding it (the trunk swallows the tributary, no step up).
    std::vector<int> incoming(b.net.nodes().size(), 0);
    for (const RiverSegment& s : b.net.segments()) ++incoming[s.toNode];

    int confluences = 0;
    for (size_t n = 0; n < b.net.nodes().size(); ++n) {
        if (incoming[n] < 2) continue;
        ++confluences;
        const ChannelSample cs =
            b.field.sampleAt(b.net.nodes()[n].vx, b.net.nodes()[n].vy);
        CHECK(cs.influenced);
        CHECK(cs.inBed);
        for (const RiverSegment& s : b.net.segments())
            if (s.toNode == n) CHECK(cs.bedMm <= b.field.nodeBedMm()[s.fromNode]);
    }
    CHECK(confluences > 0); // the fixture really does branch
}

// --- goal 3: they must reach the sea ---------------------------------------

VXC_TEST(channel_reaches_the_sea) {
    ValleyTileSampler tiles;
    Built b;
    buildValley(tiles, b);

    // The fixture's stem crosses z=0 at px=100 and the region runs to
    // px=179, so a correct network has a real coastline in it.
    CHECK(b.field.stats().segmentsBelowSeaLevel > 0);
    CHECK(b.field.stats().minBedMm < 0);

    // Every outlet that anything flows into is at or below sea level: no
    // river in this region stops on dry land.
    CHECK(b.field.stats().outlets > 0);
    CHECK_EQ(b.field.stats().outletsAboveSeaLevel, uint32_t{0});

    // And the mouth is strictly BELOW sea level, not merely touching it --
    // that is what leaves the channel open to the ocean instead of
    // separated from it by a lip.
    for (size_t n = 0; n < b.net.nodes().size(); ++n) {
        if (b.net.outgoingSegment(static_cast<uint32_t>(n)) != RiverNetwork::kNoSegment) continue;
        bool fed = false;
        for (const RiverSegment& s : b.net.segments())
            if (s.toNode == n) fed = true;
        if (!fed) continue;
        CHECK(b.field.nodeBedMm()[n] < 0);
    }
}

VXC_TEST(channel_mouth_is_open_below_sea_level_for_the_whole_tidal_reach) {
    ValleyTileSampler tiles;
    Built b;
    buildValley(tiles, b);

    // Walk the stem downstream from the last node above sea level. Once the
    // bed goes below zero it must STAY below zero all the way out -- a
    // channel that dips under sea level and comes back up has trapped the
    // river behind a sill.
    for (const RiverSegment& seg : b.net.segments()) {
        if (b.field.nodeBedMm()[seg.fromNode] > 0) continue;
        CHECK(b.field.nodeBedMm()[seg.toNode] < 0);
    }
}

// --- goal 4: banks that hold water -----------------------------------------

VXC_TEST(channel_banks_contain_the_water_line) {
    ValleyTileSampler tiles;
    Built b;
    buildValley(tiles, b);
    Amplifier amp(kSeed, tiles);

    // The containment property, stated LOCALLY: at every column the channel
    // has an opinion about, if the design profile puts ground at or above
    // the local water line, the CARVED ground must be there too. Anywhere
    // that fails is a hole in the wetted perimeter at exactly the elevation
    // the water reaches, which is what draining sideways looks like.
    //
    // Stated per column rather than by casting rays out from the centreline
    // on purpose: a ray 40 m from a descending reach is nearer to a
    // different, lower part of the channel than the column it started from,
    // so it compares against the wrong water line and reports leaks that
    // are not there. Each column carries its own nearest-reach water line,
    // so the local form has no such coupling.
    //
    // Sweep the bounding box of the network at 3-voxel stride: dense enough
    // that a hole narrower than the sweep cannot also be wide enough to
    // drain a reach, cheap enough to run in a unit test.
    int64_t vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
    bool first = true;
    for (const RiverNode& n : b.net.nodes()) {
        if (first) { vx0 = vx1 = n.vx; vy0 = vy1 = n.vy; first = false; continue; }
        vx0 = std::min(vx0, n.vx); vx1 = std::max(vx1, n.vx);
        vy0 = std::min(vy0, n.vy); vy1 = std::max(vy1, n.vy);
    }
    CHECK(!first);
    const int64_t margin = 600; // 60 m, past the widest bank in this fixture

    int64_t checked = 0, leaks = 0, clamped = 0;
    for (int64_t vy = vy0 - margin; vy <= vy1 + margin; vy += 3) {
        for (int64_t vx = vx0 - margin; vx <= vx1 + margin; vx += 3) {
            const ChannelSample cs = b.field.sampleAt(vx, vy);
            if (!cs.influenced) continue;
            // Only where the design says there should be containing ground:
            // the submerged inner bank is legitimately below the water line.
            if (cs.fillTargetMm < cs.waterLineMm) continue;
            ++checked;
            bool wasClamped = false;
            const int32_t carved =
                b.field.surfaceMm(vx, vy, amp.surfaceMm(vx, vy), wasClamped);
            if (wasClamped) ++clamped;
            if (carved < cs.waterLineMm) ++leaks;
        }
    }
    CHECK(checked > 1000);
    // Not one column of bank sits below the water it is meant to hold.
    CHECK_EQ(leaks, int64_t{0});
    // ...and it was achieved without the fill cap ever biting, so the banks
    // are real ground rather than a levee clamped short.
    CHECK_EQ(clamped, int64_t{0});
}

// The same containment property on real (noisy, branching) terrain rather
// than the hand-built valley, and on a window that contains a genuine
// coastline. Here it is a RATE rather than zero: where the ground falls
// away by more than kChannelMaxFillMm inside the embankment footprint the
// bank is deliberately left short rather than growing into an aqueduct, so
// a small residue is by design. The bound exists to catch a regression that
// makes it large, and the measured figure at the time of writing is
// 10783/5467427 = 0.20% over the full 192-px window (vxc_riverprobe
// --synthetic 20260719 192 --origin -800 -96).
VXC_TEST(channel_banks_contain_the_water_line_on_coastal_terrain) {
    SyntheticTileSampler tiles(kSeed);
    Built b;
    const RegionBounds bounds{-800, -96, -705, -1}; // coastal: land and open sea
    buildOver(tiles, bounds, b);
    Amplifier amp(kSeed, tiles);

    CHECK(b.field.stats().segments > 0);
    CHECK(b.field.bedIsStrictlyDescending());

    int64_t vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
    bool first = true;
    for (const RiverNode& n : b.net.nodes()) {
        if (first) { vx0 = vx1 = n.vx; vy0 = vy1 = n.vy; first = false; continue; }
        vx0 = std::min(vx0, n.vx); vx1 = std::max(vx1, n.vx);
        vy0 = std::min(vy0, n.vy); vy1 = std::max(vy1, n.vy);
    }
    CHECK(!first);
    const int64_t margin =
        channelInfluenceMm(b.field.stats().maxWidthMm, b.field.stats().maxDepthMm) /
            kVoxelSizeMm + 4;

    int64_t checked = 0, leaks = 0;
    for (int64_t vy = vy0 - margin; vy <= vy1 + margin; vy += 5) {
        for (int64_t vx = vx0 - margin; vx <= vx1 + margin; vx += 5) {
            const ChannelSample cs = b.field.sampleAt(vx, vy);
            if (!cs.influenced || cs.fillTargetMm < cs.waterLineMm) continue;
            ++checked;
            if (b.field.surfaceMm(vx, vy, amp.surfaceMm(vx, vy)) < cs.waterLineMm) ++leaks;
        }
    }
    CHECK(checked > 10000);
    CHECK(leaks * 100 < checked); // under 1% of bank columns leak
}

// Reaching the sea, on real terrain with a real coast. rivernet routes only
// inside its region, so an inland window legitimately has every chain
// terminating at the region edge -- this fixture deliberately straddles the
// coastline so "reaches the sea" means something.
VXC_TEST(channel_reaches_the_sea_on_coastal_terrain) {
    SyntheticTileSampler tiles(kSeed);
    Built b;
    buildOver(tiles, RegionBounds{-800, -96, -609, 95}, b);

    // The window really does contain both dry land and open sea.
    CHECK(b.field.stats().maxBedMm > 0);
    CHECK(b.field.stats().minBedMm < 0);
    // A substantial part of the network is at or below sea level, so rivers
    // are running into the ocean rather than stopping at the shoreline.
    CHECK(b.field.stats().segmentsBelowSeaLevel * 2 > b.field.stats().segments);

    // There is at least one river that starts on dry land and ends below
    // sea level, and its bed descends monotonically the whole way: the
    // headwater-to-sea case, which is the entire point of the exercise.
    std::vector<uint8_t> hasIncoming(b.net.nodes().size(), 0);
    for (const RiverSegment& s : b.net.segments()) hasIncoming[s.toNode] = 1;

    int64_t bestLen = 0;
    for (size_t h = 0; h < b.net.nodes().size(); ++h) {
        if (hasIncoming[h] || b.field.nodeBedMm()[h] <= 0) continue; // want a dry-land source
        int64_t len = 1;
        int32_t prevBed = b.field.nodeBedMm()[h];
        uint32_t cur = static_cast<uint32_t>(h);
        for (;;) {
            const uint32_t seg = b.net.outgoingSegment(cur);
            if (seg == RiverNetwork::kNoSegment) break;
            cur = b.net.segments()[seg].toNode;
            const int32_t bed = b.field.nodeBedMm()[cur];
            CHECK(bed < prevBed); // strictly downhill, every step of the way
            prevBed = bed;
            ++len;
        }
        if (b.field.nodeBedMm()[cur] <= 0 && len > bestLen) bestLen = len;
    }
    // A real river, not a two-pixel stub that happens to start above the
    // waterline. The measured longest is 21 nodes (~600 m of stem).
    CHECK(bestLen >= 10);
}

VXC_TEST(channel_carve_cuts_high_ground_and_fills_low_ground) {
    ValleyTileSampler tiles;
    Built b;
    buildValley(tiles, b);
    CHECK(b.net.segments().size() > 0);

    const RiverNode& n = b.net.nodes()[b.net.segments()[0].fromNode];
    const ChannelSample cs = b.field.sampleAt(n.vx, n.vy);
    CHECK(cs.inBed);

    // Cut: ground far above the bed comes down to exactly the bed.
    CHECK_EQ(b.field.surfaceMm(n.vx, n.vy, cs.bedMm + 50'000), cs.bedMm);
    // Fill: a hole in the bed is closed back up to the bed, so the reach is
    // water-tight rather than draining through the floor.
    bool clamped = false;
    CHECK_EQ(b.field.surfaceMm(n.vx, n.vy, cs.bedMm - 1000, clamped), cs.bedMm);
    CHECK(!clamped);
    // ...but only up to the bounded levee height: this is a riverbank, not
    // an aqueduct.
    b.field.surfaceMm(n.vx, n.vy, static_cast<int32_t>(cs.bedMm - kChannelMaxFillMm - 5000),
                      clamped);
    CHECK(clamped);

    // Far outside the footprint the channel has no opinion at all.
    const int32_t away = 12345;
    CHECK_EQ(b.field.surfaceMm(n.vx + 100000, n.vy + 100000, away), away);
}

// --- determinism -----------------------------------------------------------

VXC_TEST(channel_build_is_deterministic) {
    ValleyTileSampler t1, t2;
    Built b1, b2;
    buildValley(t1, b1);
    buildValley(t2, b2);

    Digest d1, d2;
    b1.field.digest(d1);
    b2.field.digest(d2);
    CHECK_EQ(d1.h, d2.h);
    CHECK(d1.h != 0);

    // Rebuilding into the SAME object clears state properly rather than
    // accumulating it.
    Amplifier amp1(kSeed, t1);
    auto surface1 = channelSurfaceOf(amp1);
    b1.field.build(t1, surface1, b1.net, valleyBounds());
    Digest d3;
    b1.field.digest(d3);
    CHECK_EQ(d3.h, d1.h);
    CHECK_EQ(b1.field.stats().segments, b2.field.stats().segments);

    // Point queries agree with the digest's view.
    for (const RiverNode& n : b1.net.nodes()) {
        const ChannelSample s1 = b1.field.sampleAt(n.vx, n.vy);
        const ChannelSample s2 = b2.field.sampleAt(n.vx, n.vy);
        CHECK_EQ(s1.bedMm, s2.bedMm);
        CHECK_EQ(s1.targetMm, s2.targetMm);
        CHECK_EQ(s1.widthMm, s2.widthMm);
    }
}

VXC_TEST(channel_empty_region_is_inert) {
    ValleyTileSampler tiles;
    RiverNetwork net;
    ChannelField field;
    // A region too small to accumulate past the river threshold.
    const RegionBounds tiny{0, 0, 1, 1};
    net.buildFromFlowAccumulation(tiles, kSeed, tiny);
    TileElevationSurface surface(tiles);
    field.build(tiles, surface, net, tiny);

    CHECK(field.empty());
    CHECK_EQ(field.sampleAt(0, 0).influenced, false);
    CHECK_EQ(field.surfaceMm(0, 0, 4242), 4242);
    CHECK(field.bedIsStrictlyDescending()); // vacuously true, must not crash
}
