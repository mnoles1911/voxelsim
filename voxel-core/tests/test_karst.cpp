// Karst conduit carve (voxelcore/karst.h): the per-column reduction, the four
// independent safety guards, the segment cap being an ENFORCED contract rather
// than a hope, and the ellipse maths.
//
// WHAT THESE TESTS DELIBERATELY DO NOT COVER. Where the conduits come from.
// karst.h consumes a baked table and contains no generator, so every table here
// is built by hand -- which is the point: a carve tested against its own
// generator tests the pair, not the carve.

#include <cstdio>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/karst.h"
#include "vxctest.h"

using namespace vxc;

namespace {

//: A straight horizontal conduit through the origin at a given absolute z.
struct Fixture {
    std::vector<KarstNode> nodes;
    std::vector<KarstEdge> edges;

    KarstTable table() const {
        KarstTable t;
        t.nodes = nodes.data();
        t.edges = edges.data();
        t.nodeCount = static_cast<int32_t>(nodes.size());
        t.edgeCount = static_cast<int32_t>(edges.size());
        return t;
    }
};

Fixture straightTube(int32_t zMm, int32_t rHorizCm, int32_t rVertCm,
                     uint8_t kind = KARST_PHREATIC, int32_t yMm = 0) {
    Fixture f;
    KarstNode a;
    a.xMm = -50000; a.yMm = yMm; a.zMm = zMm;
    a.rHorizCm = static_cast<uint16_t>(rHorizCm);
    a.rVertCm = static_cast<uint16_t>(rVertCm);
    a.kind = kind;
    KarstNode b = a;
    b.xMm = 50000;
    f.nodes = {a, b};
    f.edges = {KarstEdge{0, 1}};
    return f;
}

//: Deep enough that the roof, bedrock and sea-level guards are all satisfied,
//: so a test about geometry is about geometry.
constexpr int32_t kSurfaceMm = 400000;   // 400 m
constexpr int32_t kBedrockDepthMm = 200000;
constexpr int32_t kAxisMm = 300000;      // 100 m below the surface

int carveCount(const KarstColumn& col, int64_t z0, int64_t z1) {
    int n = 0;
    for (int64_t vz = z0; vz <= z1; ++vz) {
        if (karstCarveAt(col, kSurfaceMm, kBedrockDepthMm, vz)) ++n;
    }
    return n;
}

}  // namespace

VXC_TEST(karst_empty_table_carves_nothing) {
    KarstTable t;
    const KarstColumn col = karstColumnFor(t, 0, 0);
    CHECK(col.count == 0);
    CHECK(!karstCarveAt(col, kSurfaceMm, kBedrockDepthMm, 3000));
}

VXC_TEST(karst_column_is_deterministic) {
    const Fixture f = straightTube(kAxisMm, 300, 300);
    const KarstColumn a = karstColumnFor(f.table(), 0, 0);
    const KarstColumn b = karstColumnFor(f.table(), 0, 0);
    CHECK(a.count == b.count && a.count == 1);
    CHECK(a.segs[0].marginSq == b.segs[0].marginSq);
    CHECK(a.segs[0].axisZMm == b.segs[0].axisZMm);
    CHECK(a.minZMm == b.minZMm && a.maxZMm == b.maxZMm);
}

VXC_TEST(karst_circular_tube_has_the_authored_height) {
    // A 3 m radius tube carved at 10 cm voxels should be about 60 voxels tall
    // through its axis. Exactly 60 would require the axis to land on a voxel
    // boundary, so the bar is +/- 2.
    const Fixture f = straightTube(kAxisMm, 300, 300);
    const KarstColumn col = karstColumnFor(f.table(), 0, 0);
    CHECK(col.count == 1);
    const int n = carveCount(col, 2500, 3500);
    CHECK(n >= 58 && n <= 62);
}

VXC_TEST(karst_flat_tube_is_wider_than_it_is_tall) {
    // An epiphreatic passage: 6 m horizontal semi-axis, 1.5 m vertical. It must
    // carve a SHORT column at the axis and reach FURTHER sideways than a
    // circular tube of the same vertical extent would.
    const Fixture f = straightTube(kAxisMm, 600, 150, KARST_EPIPHREATIC);
    const KarstColumn on = karstColumnFor(f.table(), 0, 0);
    CHECK(on.count == 1);
    const int h = carveCount(on, 2900, 3100);
    CHECK(h >= 28 && h <= 32);          // 1.5 m semi-axis -> ~30 voxels

    // 5 m to the side: still inside the 6 m horizontal semi-axis.
    const KarstColumn side = karstColumnFor(f.table(), 0, 50);
    CHECK(side.count == 1);
    CHECK(carveCount(side, 2900, 3100) > 0);

    // 7 m to the side: outside it entirely.
    const KarstColumn out = karstColumnFor(f.table(), 0, 70);
    CHECK(out.count == 0);
}

VXC_TEST(karst_roof_guard_refuses_shallow_carves) {
    // Axis one metre below the surface: every voxel is inside the roof guard.
    const int32_t shallowAxis = kSurfaceMm - 1000;
    const Fixture f = straightTube(shallowAxis, 300, 300);
    const KarstColumn col = karstColumnFor(f.table(), 0, 0);
    CHECK(col.count == 1);
    CHECK(carveCount(col, 3900, 4100) == 0);
}

VXC_TEST(karst_entrance_kind_is_the_one_roof_exemption) {
    // The identical geometry, tagged ENTRANCE, MUST reach daylight -- that is
    // what an entrance is, and it is the only exemption in the file.
    const int32_t shallowAxis = kSurfaceMm - 1000;
    const Fixture f = straightTube(shallowAxis, 300, 300, KARST_ENTRANCE);
    const KarstColumn col = karstColumnFor(f.table(), 0, 0);
    CHECK(col.count == 1);
    CHECK(carveCount(col, 3900, 4100) > 0);
}

VXC_TEST(karst_bedrock_guard_refuses_deep_carves) {
    // Axis below the bedrock top: refused however good the geometry is.
    const int32_t deepAxis = kSurfaceMm - kBedrockDepthMm - 5000;
    const Fixture f = straightTube(deepAxis, 300, 300);
    const KarstColumn col = karstColumnFor(f.table(), 0, 0);
    CHECK(col.count == 1);
    CHECK(carveCount(col, deepAxis / 100 - 40, deepAxis / 100 + 40) == 0);
}

VXC_TEST(karst_never_carves_at_or_below_sea_level) {
    // Sea level with a surface just above it, so the roof and bedrock guards do
    // not do this test's work for it.
    const int32_t surface = static_cast<int32_t>(kKarstMinVoxelZ * kVoxelSizeMm) + 60000;
    const int32_t axis = static_cast<int32_t>(kKarstMinVoxelZ * kVoxelSizeMm);
    const Fixture f = straightTube(axis, 300, 300);
    const KarstColumn col = karstColumnFor(f.table(), 0, 0);
    for (int64_t vz = kKarstMinVoxelZ - 30; vz < kKarstMinVoxelZ; ++vz) {
        CHECK(!karstCarveAt(col, surface, 200000, vz));
    }
}

VXC_TEST(karst_band_early_out_matches_the_full_test) {
    // The two-compare band must never reject a voxel the segment loop would
    // have carved. Walk a wide range and check the band is a SUPERSET.
    const Fixture f = straightTube(kAxisMm, 400, 250);
    const KarstColumn col = karstColumnFor(f.table(), 0, 0);
    CHECK(col.count == 1);
    for (int64_t vz = 2000; vz <= 4000; ++vz) {
        const int64_t centre = vz * kVoxelSizeMm + kVoxelSizeMm / 2;
        const bool inBand = centre >= col.minZMm && centre <= col.maxZMm;
        if (karstCarveAt(col, kSurfaceMm, kBedrockDepthMm, vz)) {
            CHECK(inBand);
        }
    }
}

VXC_TEST(karst_segment_cap_is_reported_not_silently_dropped) {
    // More overlapping conduits than the cap. caves.h drops the excess in
    // silence; this must SAY so, because the bake's contract is that it never
    // happens and a violated contract nobody reports is a bug that ships.
    Fixture f;
    for (int i = 0; i < kMaxKarstSegs + 3; ++i) {
        KarstNode a;
        a.xMm = -50000; a.yMm = 0; a.zMm = kAxisMm + i * 100;
        a.rHorizCm = 300; a.rVertCm = 300;
        KarstNode b = a;
        b.xMm = 50000;
        f.nodes.push_back(a);
        f.nodes.push_back(b);
        f.edges.push_back(KarstEdge{static_cast<uint32_t>(2 * i),
                                    static_cast<uint32_t>(2 * i + 1)});
    }
    const KarstColumn col = karstColumnFor(f.table(), 0, 0);
    CHECK(col.count == kMaxKarstSegs);
    CHECK(col.overflow);
    CHECK(!karstColumnFits(col));
}

VXC_TEST(karst_column_fits_when_under_the_cap) {
    const Fixture f = straightTube(kAxisMm, 300, 300);
    const KarstColumn col = karstColumnFor(f.table(), 0, 0);
    CHECK(col.count == 1);
    CHECK(!col.overflow);
    CHECK(karstColumnFits(col));
}

VXC_TEST(karst_keyhole_cuts_below_the_tube_floor) {
    // A keyhole must carve DEEPER than the plain tube of identical radii; that
    // notch is the whole difference between the two sections.
    const Fixture tube = straightTube(kAxisMm, 300, 300, KARST_PHREATIC);
    const Fixture key = straightTube(kAxisMm, 300, 300, KARST_KEYHOLE);
    const KarstColumn ct = karstColumnFor(tube.table(), 0, 0);
    const KarstColumn ck = karstColumnFor(key.table(), 0, 0);
    CHECK(ct.count == 1 && ck.count == 1);
    CHECK(ck.segs[0].floorDropMm > 0);
    CHECK(ct.segs[0].floorDropMm == 0);
    CHECK(ck.minZMm < ct.minZMm);
    CHECK(carveCount(ck, 2500, 3500) > carveCount(ct, 2500, 3500));
}

VXC_TEST(karst_deepest_air_is_conservative) {
    // The streaming bound must never UNDER-report: a hole in the world is what
    // that costs, while over-reporting only costs a chunk nobody sees.
    const Fixture f = straightTube(kAxisMm, 300, 300);
    const KarstColumn col = karstColumnFor(f.table(), 0, 0);
    const int64_t bound = karstDeepestAirMm(col, kSurfaceMm);
    for (int64_t vz = 2000; vz <= 4000; ++vz) {
        if (!karstCarveAt(col, kSurfaceMm, kBedrockDepthMm, vz)) continue;
        const int64_t depth = int64_t(kSurfaceMm) - (vz * kVoxelSizeMm + kVoxelSizeMm / 2);
        CHECK(depth <= bound);
    }
}

VXC_TEST(karst_far_column_sees_no_segments) {
    // A column well outside the conduit's horizontal reach must reduce to
    // nothing, so the common case costs one compare in carveAt.
    const Fixture f = straightTube(kAxisMm, 300, 300);
    const KarstColumn col = karstColumnFor(f.table(), 0, 10000);
    CHECK(col.count == 0);
    CHECK(!karstCarveAt(col, kSurfaceMm, kBedrockDepthMm, 3000));
}

// --- MIRROR PARITY GROUNDWORK ---------------------------------------------
// The HLSL mirror is voxel-core/shaders/karst.ush, compiled on its own against
// both ADR-0001 targets by KarstMirrorTest.usf. Proving the two agree BIT-EXACTLY
// needs both running over the same inputs, which is vxc_gpu's job once the carve
// is wired into worldgen.ush. These two tests are what that run will be measured
// against, and they pin the hazards the transliteration can plausibly get wrong.

VXC_TEST(karst_is_translation_symmetric_across_the_origin) {
    // HAZARD A from karst.ush's header: integer division direction. Every
    // division here is either by a positive denominator with the numerator's
    // sign preserved, or wholly non-negative -- so truncation is correct AND
    // identical on both sides. If someone "fixes" one of them to floorDiv, or
    // uses plain division where the lattice needs floor, the two sides diverge
    // for NEGATIVE coordinates only, which is the half of the world nobody
    // tests. So: the same conduit at +X and at -X must carve the same shape.
    const int64_t kOffset = 1000000;   // 100 km, comfortably negative on the left
    Fixture pos = straightTube(kAxisMm, 300, 250);
    Fixture neg = pos;
    for (auto& n : pos.nodes) n.xMm += static_cast<int32_t>(kOffset);
    for (auto& n : neg.nodes) n.xMm -= static_cast<int32_t>(kOffset);

    for (int64_t dy = -40; dy <= 40; dy += 8) {
        const KarstColumn a = karstColumnFor(pos.table(), kOffset / kVoxelSizeMm, dy);
        const KarstColumn b = karstColumnFor(neg.table(), -kOffset / kVoxelSizeMm, dy);
        CHECK_EQ(a.count, b.count);
        for (int32_t i = 0; i < a.count; ++i) {
            CHECK_EQ(a.segs[i].marginSq, b.segs[i].marginSq);
            CHECK_EQ(a.segs[i].rVertMm, b.segs[i].rVertMm);
            CHECK_EQ(a.segs[i].floorDropMm, b.segs[i].floorDropMm);
        }
        CHECK_EQ(a.maxZMm - a.minZMm, b.maxZMm - b.minZMm);
    }
}

VXC_TEST(karst_golden_digest) {
    // THE TARGET THE GPU PARITY RUN IS MEASURED AGAINST. A branching table with
    // varied kinds and radii, swept over a region, folded into one FNV-1a value.
    // When karst.ush runs for real, it must reproduce this exact number; until
    // then it pins the CPU side against accidental change.
    //
    // If this moves, kWorldGenVersion moves with it. That is the whole point of
    // a golden: it is not a regression test, it is a version tripwire.
    Fixture f;
    const int32_t kinds[4] = {KARST_PHREATIC, KARST_VADOSE, KARST_KEYHOLE,
                              KARST_EPIPHREATIC};
    for (int i = 0; i < 4; ++i) {
        KarstNode a;
        a.xMm = -40000 + i * 7000;
        a.yMm = -12000 + i * 9000;
        a.zMm = kAxisMm + i * 2500;
        a.rHorizCm = static_cast<uint16_t>(180 + i * 140);
        a.rVertCm = static_cast<uint16_t>(120 + i * 90);
        a.kind = static_cast<uint8_t>(kinds[i]);
        KarstNode b = a;
        b.xMm = 40000 - i * 5000;
        b.yMm = 14000 - i * 6000;
        b.zMm = kAxisMm - i * 1800;
        b.rHorizCm = static_cast<uint16_t>(200 + i * 110);
        b.rVertCm = static_cast<uint16_t>(150 + i * 70);
        b.kind = a.kind;
        f.nodes.push_back(a);
        f.nodes.push_back(b);
        f.edges.push_back(KarstEdge{static_cast<uint32_t>(2 * i),
                                    static_cast<uint32_t>(2 * i + 1)});
    }

    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](uint64_t v) {
        for (int b = 0; b < 8; ++b) {
            h ^= (v >> (b * 8)) & 0xFF;
            h *= 1099511628211ull;
        }
    };
    for (int64_t vx = -60; vx <= 60; vx += 4) {
        for (int64_t vy = -60; vy <= 60; vy += 4) {
            const KarstColumn col = karstColumnFor(f.table(), vx, vy);
            mix(static_cast<uint64_t>(col.count));
            mix(static_cast<uint64_t>(static_cast<uint32_t>(col.minZMm)));
            mix(static_cast<uint64_t>(static_cast<uint32_t>(col.maxZMm)));
            for (int64_t vz = 2900; vz <= 3100; vz += 3) {
                mix(karstCarveAt(col, kSurfaceMm, kBedrockDepthMm, vz) ? 1u : 0u);
            }
        }
    }
    std::printf("      karst_layer golden: 0x%016llX\n",
                static_cast<unsigned long long>(h));
    CHECK_EQ(h, 0xA500EED45E8333B5ull);
}

// --- AMPLIFIER INTEGRATION -------------------------------------------------
// These two must be read together. The first proves the carve is INERT with no
// table installed, which is what lets it land without moving the world digest.
// The second proves it is inert because the table is empty and NOT because the
// wiring is broken -- a disconnected wire passes the first test perfectly, and
// "it changed nothing" is exactly what a feature that was never called looks
// like. This repo has shipped that failure before (the standing-water veto read
// an empty debug field and was inert for a whole programme).

VXC_TEST(karst_is_inert_in_the_amplifier_with_no_table) {
    SyntheticTileSampler tiles(20260819ull);
    Amplifier amp(20260819ull, tiles);
    CHECK(amp.karstTable().empty());
    for (int64_t vx = -200; vx <= 200; vx += 50) {
        const ColumnSample col = amp.column(vx, 37);
        CHECK(col.karst.count == 0);
        for (int64_t vz = -50; vz <= 200; vz += 7) {
            // Whatever the terrain says, the karst pass must not be the reason.
            CHECK(!karstCarveAt(col.karst, col.surfaceMm, col.bedrockDepthMm, vz));
        }
    }
}

VXC_TEST(karst_table_installed_actually_carves_through_materialAt) {
    SyntheticTileSampler tiles(20260819ull);
    Amplifier amp(20260819ull, tiles);

    // Put a fat conduit squarely inside solid rock under a real column: read the
    // surface first, then place the axis 40 m below it, so the roof and bedrock
    // guards are both satisfied by construction rather than by luck.
    const ColumnSample probe = amp.column(0, 0);
    const int32_t axisMm = probe.surfaceMm - 40000;

    std::vector<KarstNode> nodes(2);
    for (int i = 0; i < 2; ++i) {
        nodes[i].xMm = (i == 0) ? -60000 : 60000;
        nodes[i].yMm = 0;
        nodes[i].zMm = axisMm;
        nodes[i].rHorizCm = 500;   // 5 m
        nodes[i].rVertCm = 500;
        nodes[i].kind = KARST_PHREATIC;
    }
    std::vector<KarstEdge> edges{KarstEdge{0, 1}};
    KarstTable t;
    t.nodes = nodes.data();
    t.edges = edges.data();
    t.nodeCount = 2;
    t.edgeCount = 1;
    amp.setKarstTable(t);

    const ColumnSample col = amp.column(0, 0);
    CHECK(col.karst.count == 1);

    // The axis voxel must now read as AIR through the SHIPPED path -- not
    // through karstCarveAt directly, which would test the carve against itself.
    const int64_t axisVz = axisMm / kVoxelSizeMm;
    CHECK(Amplifier::materialAt(col, axisVz) == MAT_AIR);

    // And a column well outside the conduit must be untouched.
    const ColumnSample far = amp.column(0, 4000);
    CHECK(far.karst.count == 0);

    // Removing the table restores the no-op exactly.
    amp.setKarstTable(KarstTable{});
    const ColumnSample after = amp.column(0, 0);
    CHECK(after.karst.count == 0);
    CHECK(Amplifier::materialAt(after, axisVz) != MAT_AIR);
}
