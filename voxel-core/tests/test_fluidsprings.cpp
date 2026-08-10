// Spring selection and window-edge river inflow (voxelcore/fluidsprings.h).
//
// The two tests that MUST exist here, because both failures are invisible on
// screen until the owner sees them:
//
//   * THE Q BAND IS MEASURED AGAINST THE TILE, NOT THE WINDOW. A 51 m window
//     holds about half a head, so a band derived from what the window can see
//     always selects that head -- which reproduces exactly the 229k-dots bug
//     this file exists to remove, while looking like it is filtering.
//   * A CROSSING IS INBOUND. Get the direction test backwards and every window
//     grows a faucet where its river LEAVES, pumping water back up the valley.

#include "voxelcore/fluidsprings.h"

#include <unordered_map>
#include <vector>

#include "vxctest.h"

using namespace vxc;

namespace {

SpringHead head(int64_t px, int64_t py, uint32_t q) {
    SpringHead h;
    h.px = px;
    h.py = py;
    h.qM3PerYear = q;
    return h;
}

// A hand-built water + flow plane. Every cell is dry non-channel until set;
// `refuse` makes a cell UNRESOLVABLE, which is the third state neither wet nor
// dry (see IBakedWaterSource).
class GridBakedWater final : public IBakedWaterSource {
public:
    int32_t pixelSizeMm() const override { return 1875; }

    void set(int64_t px, int64_t py, uint8_t accumLog2, int32_t surfaceMm, bool wet = true) {
        cells_[key(px, py)] = Cell{uint8_t(accumLog2 | kFlowBitChannel), surfaceMm, wet};
    }
    void refuse(int64_t px, int64_t py) { refused_.insert(key(px, py)); }

    bool waterAt(int64_t px, int64_t py, int32_t& outSurfaceMm, bool& outWet) override {
        if (refused_.count(key(px, py))) return false;
        auto it = cells_.find(key(px, py));
        if (it == cells_.end()) {
            outSurfaceMm = INT32_MIN;
            outWet = false;
            return true;
        }
        outSurfaceMm = it->second.surfaceMm;
        outWet = it->second.wet;
        return true;
    }

    bool flowAt(int64_t px, int64_t py, uint8_t& outFlow) override {
        if (refused_.count(key(px, py))) return false;
        auto it = cells_.find(key(px, py));
        outFlow = it == cells_.end() ? uint8_t(0) : it->second.flow;
        return true;
    }

private:
    struct Cell {
        uint8_t flow;
        int32_t surfaceMm;
        bool wet;
    };
    static uint64_t key(int64_t px, int64_t py) {
        return (uint64_t(uint32_t(int32_t(px))) << 32) | uint64_t(uint32_t(int32_t(py)));
    }
    std::unordered_map<uint64_t, Cell> cells_;
    struct Set {
        std::unordered_map<uint64_t, int> m;
        void insert(uint64_t k) { m[k] = 1; }
        size_t count(uint64_t k) const { return m.count(k); }
    } refused_;
};

} // namespace

// ---------------------------------------------------------------------------
// selectSprings
// ---------------------------------------------------------------------------

VXC_TEST(springs_q_band_admits_only_first_order_origins) {
    // Spread far enough apart that the spacing pass cannot be what filters.
    std::vector<SpringHead> heads = {
        head(0, 0, 500'000),      // the minimum
        head(1000, 0, 750'000),   // 1.5x -- inside the 2x band
        head(2000, 0, 1'000'000), // exactly 2x -- inside (the band is <=)
        head(3000, 0, 1'000'001), // a hair over -- out
        head(4000, 0, 1'900'000), // the tile median: a fragment break, not a spring
    };
    const SpringSelection s = selectSprings(heads, SpringParams{});
    CHECK(s.rated);
    CHECK_EQ(s.minQ, int64_t(500'000));
    CHECK_EQ(s.thresholdQ, int64_t(1'000'000));
    CHECK_EQ(s.candidates, 3u);
    CHECK_EQ(s.suppressed, 0u);
    CHECK_EQ(s.springs.size(), size_t(3));
    // Selection order is ascending Q, so a caller's cap keeps the smallest.
    CHECK_EQ(s.springs[0], 0u);
    CHECK_EQ(s.springs[1], 1u);
    CHECK_EQ(s.springs[2], 2u);
}

VXC_TEST(springs_reference_min_is_the_tile_not_the_window) {
    // THE BUG THIS PINS. These are the heads visible inside one 51 m window,
    // and every one of them is a mid-network fragment break: the tile's real
    // minimum is 552,537 (tile (-4,-4), bv24) and the smallest thing here is
    // 20 million. Derived from the window, the band would select the 20M one
    // and call it a spring.
    std::vector<SpringHead> heads = {
        head(10, 10, 20'000'000),
        head(14, 12, 24'000'000),
        head(19, 15, 41'000'000),
    };
    SpringParams windowDerived; // referenceMinQ 0 == derive
    const SpringSelection bad = selectSprings(heads, windowDerived);
    CHECK_EQ(bad.springs.size(), size_t(1)); // exactly the failure mode

    SpringParams tileAnchored;
    tileAnchored.referenceMinQ = 552'537;
    const SpringSelection good = selectSprings(heads, tileAnchored);
    CHECK_EQ(good.minQ, int64_t(552'537));
    CHECK_EQ(good.thresholdQ, int64_t(1'105'074));
    CHECK_EQ(good.candidates, 0u);
    CHECK(good.springs.empty()); // and the window is fed by its edges instead
}

VXC_TEST(springs_spacing_keeps_the_lowest_q_of_a_cluster) {
    // Five first-order heads inside 20 px (37 m) -- one gully top the bake
    // fragmented, not five springs.
    std::vector<SpringHead> heads = {
        head(100, 100, 900'000),
        head(104, 103, 600'000), // the lowest Q of the cluster
        head(110, 108, 700'000),
        head(118, 96, 800'000),
        head(95, 112, 1'000'000),
        head(400, 400, 650'000), // 300 px away: its own spring
    };
    SpringParams p;
    p.referenceMinQ = 500'000; // threshold 1,000,000: all six are candidates
    const SpringSelection s = selectSprings(heads, p);
    CHECK_EQ(s.candidates, 6u);
    CHECK_EQ(s.springs.size(), size_t(2));
    CHECK_EQ(s.suppressed, 4u);
    CHECK_EQ(s.springs[0], 1u); // 600k, the cluster's lowest
    CHECK_EQ(s.springs[1], 5u); // 650k, the far one

    // And the spacing is a real distance, not a bin membership: two heads 4 px
    // apart on opposite sides of a bin boundary must still collapse.
    std::vector<SpringHead> straddle = {head(79, 0, 900'000), head(82, 0, 800'000)};
    const SpringSelection t = selectSprings(straddle, p);
    CHECK_EQ(t.springs.size(), size_t(1));
    CHECK_EQ(t.springs[0], 1u);
}

VXC_TEST(springs_selection_is_deterministic_under_input_permutation) {
    std::vector<SpringHead> a = {
        head(100, 100, 900'000), head(104, 103, 600'000), head(110, 108, 700'000),
        head(400, 400, 650'000), head(404, 402, 620'000), head(900, 100, 550'000),
    };
    std::vector<SpringHead> b = {a[5], a[2], a[0], a[4], a[3], a[1]};
    SpringParams p;
    p.referenceMinQ = 500'000;
    const SpringSelection sa = selectSprings(a, p);
    const SpringSelection sb = selectSprings(b, p);
    CHECK_EQ(sa.springs.size(), sb.springs.size());
    CHECK_EQ(sa.candidates, sb.candidates);
    CHECK_EQ(sa.suppressed, sb.suppressed);
    // Compare the POINTS, since the indices are into different vectors.
    for (size_t i = 0; i < sa.springs.size(); ++i) {
        CHECK_EQ(a[sa.springs[i]].px, b[sb.springs[i]].px);
        CHECK_EQ(a[sa.springs[i]].py, b[sb.springs[i]].py);
        CHECK_EQ(int64_t(a[sa.springs[i]].qM3PerYear), int64_t(b[sb.springs[i]].qM3PerYear));
    }
    // Repeated calls agree with themselves too (no hash-order leak).
    const SpringSelection again = selectSprings(a, p);
    CHECK_EQ(again.springs.size(), sa.springs.size());
    for (size_t i = 0; i < sa.springs.size(); ++i) CHECK_EQ(again.springs[i], sa.springs[i]);
}

VXC_TEST(springs_unrated_set_skips_the_band_and_a_rated_set_drops_unrated_heads) {
    // Every head unrated (the pre-bv24 fallback): no band to measure, so the
    // spacing pass alone runs and the set stays usable.
    std::vector<SpringHead> unrated = {head(0, 0, 0), head(200, 0, 0), head(4, 4, 0)};
    const SpringSelection u = selectSprings(unrated, SpringParams{});
    CHECK(!u.rated);
    CHECK_EQ(u.candidates, 3u);
    CHECK_EQ(u.springs.size(), size_t(2)); // (0,0) and (200,0); (4,4) is 5.6 px away
    CHECK_EQ(u.suppressed, 1u);

    // One unrated head among rated ones must NOT win by default.
    std::vector<SpringHead> mixed = {head(0, 0, 0), head(500, 0, 600'000),
                                     head(1000, 0, 9'000'000)};
    const SpringSelection m = selectSprings(mixed, SpringParams{});
    CHECK(m.rated);
    CHECK_EQ(m.minQ, int64_t(600'000));
    CHECK_EQ(m.springs.size(), size_t(1));
    CHECK_EQ(m.springs[0], 1u);
}

VXC_TEST(springs_empty_and_degenerate_inputs) {
    const SpringSelection e = selectSprings({}, SpringParams{});
    CHECK(e.springs.empty());
    CHECK_EQ(e.candidates, 0u);
    CHECK(!e.rated);

    // Spacing off: every candidate survives.
    SpringParams p;
    p.spacingPx = 0;
    p.referenceMinQ = 500'000;
    std::vector<SpringHead> pile = {head(0, 0, 500'000), head(1, 0, 500'000),
                                    head(2, 0, 500'000)};
    const SpringSelection s = selectSprings(pile, p);
    CHECK_EQ(s.springs.size(), size_t(3));
    CHECK_EQ(s.suppressed, 0u);
}

// ---------------------------------------------------------------------------
// The discharge unit chain
// ---------------------------------------------------------------------------

VXC_TEST(springs_discharge_unit_chain) {
    // Bucket -> area: the GEOMETRIC midpoint, sqrt(2) * 2^L as 181/128.
    CHECK_EQ(springAreaM2(0), int64_t(1));                     // 1 * 1.414 -> 1 (integer)
    CHECK_EQ(springAreaM2(10), (int64_t(1024) * 181) / 128);   // 1448 m^2
    CHECK_EQ(springAreaM2(20), (int64_t(1048576) * 181) / 128);
    // Saturating, never wrapping: a wrapped negative would make a trunk read
    // as a headwater (rivernet.h flowAccumM2's own warning).
    CHECK(springAreaM2(31) > 0);
    CHECK(springAreaM2(31) >= int64_t(INT32_MAX));
    // The channel/bank/deposition bits must not leak into the bucket.
    CHECK_EQ(springAreaM2(uint8_t(10 | kFlowBitChannel | kFlowBitBank)), springAreaM2(10));

    // Area x runoff -> m^3/yr. 1 km^2 at 577 mm/yr is 577,000 m^3/yr.
    CHECK_EQ(springDischargeM3PerYear(1'000'000, 577), int64_t(577'000));
    CHECK_EQ(springDischargeM3PerYear(0, 577), int64_t(0));
    CHECK_EQ(springDischargeM3PerYear(1'000'000, 0), int64_t(0));
    // And the constant is the pipeline's measured catchment mean, not a guess.
    CHECK_EQ(kEdgeInflowRunoffMmPerYrDefault, int64_t(577));
}

VXC_TEST(springs_runoff_calibration_fits_the_median_and_says_when_it_did_not) {
    // Eight heads whose Q/area ratio is 4000 mm/yr, plus two exotic ones an
    // order of magnitude off. The median must ignore the outliers -- one
    // spring-fed reach crossing a dry catchment must not set the rate for the
    // whole box.
    GridBakedWater src;
    std::vector<SpringHead> heads;
    for (int64_t i = 0; i < 8; ++i) {
        src.set(i, 0, 10, 1000);
        // area(10) = 1448 m^2; q = area * 4000 / 1000 = 5792 m^3/yr
        heads.push_back(head(i, 0, uint32_t(springAreaM2(10) * 4000 / 1000)));
    }
    src.set(20, 0, 10, 1000);
    src.set(21, 0, 10, 1000);
    heads.push_back(head(20, 0, uint32_t(springAreaM2(10) * 90'000 / 1000)));
    heads.push_back(head(21, 0, uint32_t(springAreaM2(10) * 50 / 1000)));

    const RunoffCalibration c = calibrateRunoff(src, heads);
    CHECK(c.calibrated);
    CHECK_EQ(c.samples, 10u);
    CHECK_EQ(c.runoffMmPerYr, int64_t(4000));

    // Too few samples: the fallback, and it SAYS so rather than passing a fit
    // off as measured.
    std::vector<SpringHead> few(heads.begin(), heads.begin() + 3);
    const RunoffCalibration f = calibrateRunoff(src, few);
    CHECK(!f.calibrated);
    CHECK_EQ(f.samples, 3u);
    CHECK_EQ(f.runoffMmPerYr, kEdgeInflowRunoffMmPerYrDefault);

    // Unrated heads and heads over an unresolvable/absent flow byte are not
    // samples. (The heads here sit where GridBakedWater has no channel.)
    std::vector<SpringHead> offPlane = {head(500, 500, 1000), head(501, 500, 1000)};
    const RunoffCalibration o = calibrateRunoff(src, offPlane);
    CHECK_EQ(o.samples, 0u);
    CHECK(!o.calibrated);
}

// ---------------------------------------------------------------------------
// selectRiverCrossings
// ---------------------------------------------------------------------------

VXC_TEST(crossings_find_the_inbound_face_and_not_the_outbound_one) {
    // One river running west -> east along py 5, entering the box at px 0 and
    // leaving it at px 9. Accumulation grows downstream; the surface descends.
    GridBakedWater src;
    for (int64_t px = -1; px <= 10; ++px) {
        src.set(px, 5, uint8_t(10 + (px < 0 ? 0 : px / 3)), int32_t(1000 - px * 10));
    }
    RiverCrossingParams p;
    p.bounds = RegionBounds{0, 0, 9, 9};
    const RiverCrossingResult r = selectRiverCrossings(src, p);

    CHECK_EQ(r.crossings.size(), size_t(1));
    CHECK_EQ(r.crossings[0].px, int64_t(0));
    CHECK_EQ(r.crossings[0].py, int64_t(5));
    CHECK_EQ(int(r.crossings[0].dirX), 1); // pointing INTO the box, downstream
    CHECK_EQ(int(r.crossings[0].dirY), 0);
    CHECK_EQ(r.crossings[0].surfaceMm, int32_t(1000));
    CHECK_EQ(r.crossings[0].accumLog2, uint8_t(10));
    CHECK_EQ(r.crossings[0].areaM2, springAreaM2(10));
    CHECK_EQ(r.crossings[0].qM3PerYear,
             springDischargeM3PerYear(springAreaM2(10), kEdgeInflowRunoffMmPerYrDefault));

    // The ran-flags: the walk really did look at the whole ring.
    CHECK_EQ(r.edgePixelsScanned, uint64_t(4 * 10));
    CHECK_EQ(r.channelEdgePixels, uint64_t(2)); // (0,5) west and (9,5) east
    CHECK_EQ(r.unresolved, uint64_t(0));
}

VXC_TEST(crossings_a_wide_river_is_one_crossing_at_its_thalweg) {
    // A 5-px-wide reach crossing the west face, deepest (most accumulated) at
    // py 5. Twenty faucets across one river is the bug; one is the answer.
    GridBakedWater src;
    for (int64_t py = 3; py <= 7; ++py) {
        const uint8_t accum = (py == 5) ? uint8_t(14) : uint8_t(12);
        src.set(0, py, accum, 1000);
        src.set(1, py, accum, 1000);
        src.set(2, py, accum, 990);
    }
    RiverCrossingParams p;
    p.bounds = RegionBounds{0, 0, 9, 9};
    const RiverCrossingResult r = selectRiverCrossings(src, p);
    CHECK_EQ(r.crossings.size(), size_t(1));
    CHECK_EQ(r.crossings[0].py, int64_t(5));
    CHECK_EQ(r.crossings[0].accumLog2, uint8_t(14));
    CHECK_EQ(r.mergedRunPixels, uint64_t(4));
}

VXC_TEST(crossings_respect_wet_accum_and_unresolved) {
    // A DRY baked channel (a seasonal wash) crossing the west face, and a
    // resolvable-but-refused cell on the east face.
    GridBakedWater src;
    for (int64_t px = 0; px <= 3; ++px) src.set(px, 2, 12, 1000, /*wet*/ false);
    src.refuse(9, 7);

    RiverCrossingParams p;
    p.bounds = RegionBounds{0, 0, 9, 9};
    RiverCrossingResult r = selectRiverCrossings(src, p);
    CHECK(r.crossings.empty());
    CHECK_EQ(r.unresolved, uint64_t(1)); // the refused cell, counted not ignored

    // requireWet off admits the wash -- a legitimate thing to want, and a wrong
    // default (rivernet.h BakedWaterBuildParams).
    p.requireWet = false;
    r = selectRiverCrossings(src, p);
    CHECK_EQ(r.crossings.size(), size_t(1));
    CHECK_EQ(r.crossings[0].py, int64_t(2));

    // The accumulation band culls a rivulet the same way it culls a graph node.
    p.requireWet = true;
    GridBakedWater wetSrc;
    for (int64_t px = 0; px <= 3; ++px) wetSrc.set(px, 2, 12, 1000);
    p.minAccumLog2 = 13;
    r = selectRiverCrossings(wetSrc, p);
    CHECK(r.crossings.empty());
    CHECK_EQ(r.channelEdgePixels, uint64_t(1)); // seen, then rejected on the band
    p.minAccumLog2 = 12;
    r = selectRiverCrossings(wetSrc, p);
    CHECK_EQ(r.crossings.size(), size_t(1));
}

VXC_TEST(crossings_diagonal_inflow_and_empty_box) {
    // A reach entering the south-west corner on the diagonal. The inbound
    // neighbour is (1,1), so the emitted direction is the diagonal step.
    GridBakedWater src;
    src.set(0, 0, 12, 1000);
    src.set(1, 1, 13, 990);
    src.set(2, 2, 14, 980);
    RiverCrossingParams p;
    p.bounds = RegionBounds{0, 0, 9, 9};
    const RiverCrossingResult r = selectRiverCrossings(src, p);
    // (0,0) sits on both the west and the south face; it is ONE crossing.
    CHECK_EQ(r.crossings.size(), size_t(1));
    CHECK_EQ(r.crossings[0].px, int64_t(0));
    CHECK_EQ(r.crossings[0].py, int64_t(0));
    CHECK_EQ(int(r.crossings[0].dirX), 1);
    CHECK_EQ(int(r.crossings[0].dirY), 1);

    // A box with no channel in it reports nothing and says it looked.
    GridBakedWater dry;
    const RiverCrossingResult none = selectRiverCrossings(dry, p);
    CHECK(none.crossings.empty());
    CHECK_EQ(none.edgePixelsScanned, uint64_t(40));
    CHECK_EQ(none.channelEdgePixels, uint64_t(0));

    // A degenerate (empty) rectangle is refused rather than walked.
    RiverCrossingParams bad;
    bad.bounds = RegionBounds{5, 5, 4, 9};
    CHECK(selectRiverCrossings(src, bad).edgePixelsScanned == 0);
}
