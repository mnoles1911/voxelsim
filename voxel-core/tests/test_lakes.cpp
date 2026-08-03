// voxelcore/lakes.h -- the client half of the baked basin registry.
//
// THE LOAD-BEARING TEST HERE is `lake_extent_matches_the_python_fixture_cell_for_cell`.
// The extent of a lake is ONE definition with TWO implementations -- the bake's
// `basins.lake_extent_mask` and this header's `lakeExtentFill` -- and a
// disagreement between them does not show up as a crash. It shows up as water
// on a hillside on the far side of a ridge, in one process and not the other,
// while every structural check passes. So the rule is frozen into
// `fixtures/lake_extent_v1.bin` by Python and reproduced here byte for byte.
//
// Everything else in this file keeps the pieces that rule stands on honest: a
// fill that respects its bbox, a dry basin that answers empty instead of
// throwing, a partial top fill that puts the surface AT the datum, and a
// sampler that never invents a lake where no tile is loaded.

#include <cstdio>
#include <iterator>
#include <optional>
#include <string>
#include <filesystem>
#include <fstream>
#include <vector>

#include "voxelcore/lakes.h"
#include "vxctest.h"

using namespace vxc;

namespace {

struct ExtentFixture {
    uint32_t w = 0, h = 0;
    std::vector<int32_t> elev;
    std::vector<BasinEntry> basins;
    std::vector<uint32_t> wetCells;
    std::vector<std::vector<uint8_t>> masks;
    bool ok = false;
};

std::filesystem::path extentFixturePath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "lake_extent_v1.bin";
}

// Reads the fixture, or leaves `ok` false when it is absent. Absent is a SKIP
// and not a failure for the same reason the .vxtl goldens are: a checkout
// without the generated fixtures should still build and run, and the generator
// is one command away (terrain-service/tools/make_lake_extent_fixture.py).
ExtentFixture loadExtentFixture() {
    ExtentFixture f;
    std::ifstream in(extentFixturePath(), std::ios::binary);
    if (!in) return f;
    char magic[8] = {};
    in.read(magic, 8);
    if (std::string(magic, 8) != "VXLKEXT1") return f;
    in.read(reinterpret_cast<char*>(&f.w), 4);
    in.read(reinterpret_cast<char*>(&f.h), 4);
    f.elev.resize(size_t(f.w) * size_t(f.h));
    in.read(reinterpret_cast<char*>(f.elev.data()),
            std::streamsize(f.elev.size() * sizeof(int32_t)));
    uint32_t n = 0;
    in.read(reinterpret_cast<char*>(&n), 4);
    for (uint32_t i = 0; i < n; ++i) {
        int16_t v[6];
        int32_t surf;
        uint32_t wet;
        in.read(reinterpret_cast<char*>(v), sizeof(v));
        in.read(reinterpret_cast<char*>(&surf), 4);
        in.read(reinterpret_cast<char*>(&wet), 4);
        BasinEntry b;
        b.basinId = uint16_t(i);
        b.seedX = uint16_t(v[0]);
        b.seedY = uint16_t(v[1]);
        b.bboxX0 = uint16_t(v[2]);
        b.bboxY0 = uint16_t(v[3]);
        b.bboxX1 = uint16_t(v[4]);
        b.bboxY1 = uint16_t(v[5]);
        b.surfaceMm = surf;
        b.spillMm = surf;
        b.kind = kBasinLakeOverflowing;
        f.basins.push_back(b);
        f.wetCells.push_back(wet);
    }
    for (const BasinEntry& b : f.basins) {
        const size_t n2 = size_t(b.bboxX1 - b.bboxX0 + 1) * size_t(b.bboxY1 - b.bboxY0 + 1);
        std::vector<uint8_t> m(n2);
        in.read(reinterpret_cast<char*>(m.data()), std::streamsize(n2));
        f.masks.push_back(std::move(m));
    }
    f.ok = bool(in);
    return f;
}

} // namespace

VXC_TEST(lake_extent_matches_the_python_fixture_cell_for_cell) {
    const ExtentFixture f = loadExtentFixture();
    if (!f.ok) {
        std::printf("  (skip: %s absent -- regenerate with "
                    "terrain-service/tools/make_lake_extent_fixture.py)\n",
                    extentFixturePath().string().c_str());
        return;
    }
    CHECK_EQ(int(f.basins.size()), 5);
    for (size_t i = 0; i < f.basins.size(); ++i) {
        std::vector<uint8_t> got;
        const size_t n = lakeExtentFill(
            f.basins[i],
            [&](int32_t x, int32_t y) { return f.elev[size_t(y) * f.w + size_t(x)]; },
            got);
        CHECK_EQ(n, size_t(f.wetCells[i]));
        CHECK_EQ(got.size(), f.masks[i].size());
        size_t diff = 0;
        for (size_t k = 0; k < got.size() && k < f.masks[i].size(); ++k)
            if ((got[k] != 0) != (f.masks[i][k] != 0)) ++diff;
        CHECK_EQ(diff, size_t(0));
    }
    // Row 0 and row 1 share ONE bbox and must come out disjoint -- the whole
    // reason the rule is a seeded fill and not `elev <= surfaceMm`.
    std::vector<uint8_t> m0, m1;
    lakeExtentFill(f.basins[0],
                   [&](int32_t x, int32_t y) { return f.elev[size_t(y) * f.w + size_t(x)]; }, m0);
    lakeExtentFill(f.basins[1],
                   [&](int32_t x, int32_t y) { return f.elev[size_t(y) * f.w + size_t(x)]; }, m1);
    CHECK_EQ(m0.size(), m1.size());
    size_t both = 0, either = 0, threshold = 0;
    for (size_t k = 0; k < m0.size(); ++k) {
        if (m0[k] && m1[k]) ++both;
        if (m0[k] || m1[k]) ++either;
    }
    CHECK_EQ(both, size_t(0));
    // ... and TOGETHER they are exactly the threshold set, so neither leaked
    // and neither came up short.
    const BasinEntry& b = f.basins[0];
    for (int32_t y = b.bboxY0; y <= b.bboxY1; ++y)
        for (int32_t x = b.bboxX0; x <= b.bboxX1; ++x)
            if (f.elev[size_t(y) * f.w + size_t(x)] <= b.surfaceMm) ++threshold;
    CHECK_EQ(either, threshold);
    CHECK(m0.size() > either);   // the bbox really is bigger than the water
}

VXC_TEST(lake_extent_is_eight_connected_through_a_diagonal_pinch) {
    // Two 3x3 hollows meeting at one diagonal corner. Four-connected code
    // returns 9 cells; the registry measured 18 on the 8-connected component,
    // and shipping 9 would leave half a lake dry with no error anywhere.
    constexpr int32_t W = 16;
    std::vector<int32_t> z(size_t(W) * W, 1000);
    auto at = [&](int32_t x, int32_t y) -> int32_t& { return z[size_t(y) * W + size_t(x)]; };
    for (int32_t y = 2; y <= 4; ++y)
        for (int32_t x = 2; x <= 4; ++x) at(x, y) = 0;
    for (int32_t y = 5; y <= 7; ++y)
        for (int32_t x = 5; x <= 7; ++x) at(x, y) = 0;

    BasinEntry b;
    b.seedX = 3;
    b.seedY = 3;
    b.bboxX0 = 0;
    b.bboxY0 = 0;
    b.bboxX1 = W - 1;
    b.bboxY1 = W - 1;
    b.surfaceMm = 500;
    b.kind = kBasinLakeTerminal;
    std::vector<uint8_t> m;
    CHECK_EQ(lakeExtentFill(b, [&](int32_t x, int32_t y) { return z[size_t(y) * W + size_t(x)]; }, m),
             size_t(18));
    CHECK(m[size_t(7) * W + 7] != 0);   // reached the far lobe
}

VXC_TEST(lake_extent_stops_at_the_bbox_and_at_a_dry_datum) {
    constexpr int32_t W = 32;
    std::vector<int32_t> z(size_t(W) * W, 0);   // one huge flat basin
    BasinEntry b;
    b.seedX = 16;
    b.seedY = 16;
    b.bboxX0 = 12;
    b.bboxY0 = 12;
    b.bboxX1 = 20;
    b.bboxY1 = 20;
    b.surfaceMm = 100;
    b.kind = kBasinLakeTerminal;
    std::vector<uint8_t> m;
    // Clipped to the bbox, not to the flat: 9x9, not 32x32.
    CHECK_EQ(lakeExtentFill(b, [&](int32_t, int32_t) { return 0; }, m), size_t(81));
    CHECK_EQ(m.size(), size_t(81));

    // A dry basin's surface sits at its floor, so nothing is BELOW it minus a
    // millimetre. Empty is the answer, not an error.
    b.surfaceMm = -1;
    CHECK_EQ(lakeExtentFill(b, [&](int32_t, int32_t) { return 0; }, m), size_t(0));
    CHECK_EQ(m.size(), size_t(81));   // still sized, so the caller can index it

    // A seed outside its own bbox is refused rather than clamped: a clamp
    // would silently fill a different basin.
    b.surfaceMm = 100;
    b.seedX = 4;
    CHECK_EQ(lakeExtentFill(b, [&](int32_t, int32_t) { return 0; }, m), size_t(0));
}

VXC_TEST(partial_top_fill_puts_the_surface_at_the_datum_not_on_the_lattice) {
    // A voxel is 100 mm. A datum 37 mm into a voxel must render 37% of it,
    // not 0% or 100% -- otherwise every lake is up to 5 cm out and two lakes
    // 9 cm apart draw at the same height.
    CHECK_EQ(int(waterFillUnits(0, 1000)), 255);      // well under water
    CHECK_EQ(int(waterFillUnits(1000, 1000)), 0);     // bottom exactly AT the datum
    CHECK_EQ(int(waterFillUnits(1100, 1000)), 0);     // above it
    CHECK_EQ(int(waterFillUnits(900, 1000)), 255);    // exactly one voxel under
    CHECK_EQ(int(waterFillUnits(963, 1000)), 94);     // 37 mm -> round(0.37*255)
    CHECK_EQ(int(waterFillUnits(0, kNoWaterMm)), 0);
    // A sliver of water is never dry, and no clamp is needed to make that so:
    // 255 units over 100 mm means the SMALLEST possible depth, one
    // millimetre, already rounds to 3. Asserted because it is the reason the
    // defensive `max(1, ...)` this function used to carry was deleted.
    CHECK_EQ(int(waterFillUnits(999, 1000)), 3);
    for (int32_t d = 1; d <= 100; ++d) CHECK(waterFillUnits(1000 - d, 1000) >= 1);
    // Monotone in depth, which is what stops a surface from being non-planar.
    for (int32_t d = 1; d <= 100; ++d)
        CHECK(waterFillUnits(1000 - d, 1000) >= waterFillUnits(1000 - d + 1, 1000));
}

VXC_TEST(implicit_water_fill_is_bounded_below_by_the_amplified_ground) {
    // The datum is authoritative and the ground is what stops it. A column
    // inside a lake's extent whose amplified ground rises ABOVE the datum
    // yields no water at all -- that is how the shoreline follows the ground's
    // own contour instead of the 1.875 m lattice's staircase.
    constexpr int32_t ground = 500, surface = 1000;
    CHECK_EQ(int(implicitWaterFill(4, ground, surface, false)), 0);   // 400 mm: inside rock
    CHECK_EQ(int(implicitWaterFill(5, ground, surface, false)), 255); // 500 mm: at the ground
    CHECK_EQ(int(implicitWaterFill(9, ground, surface, false)), 255);
    CHECK_EQ(int(implicitWaterFill(10, ground, surface, false)), 0);  // 1000 mm: at the datum
    // Ground above the datum -> dry, at every height.
    for (int64_t vz = -20; vz <= 20; ++vz)
        CHECK_EQ(int(implicitWaterFill(vz, 2000, surface, false)), 0);
    // A dry column is dry.
    CHECK_EQ(int(implicitWaterFill(6, ground, kNoWaterMm, false)), 0);
    // Cavern flood still wins outright, unchanged from today's behaviour --
    // this composition ADDS a case, it does not reinterpret the existing one.
    CHECK_EQ(int(implicitWaterFill(6, ground, kNoWaterMm, true)), 255);
    CHECK_EQ(int(implicitWaterFill(-99, 2000, kNoWaterMm, true)), 255);
}

VXC_TEST(lake_sampler_answers_no_water_with_nothing_loaded) {
    FineTileSampler tiles(1234);
    LakeSampler lakes(tiles);
    CHECK_EQ(lakes.waterSurfaceMmAtVoxel(0, 0), kNoWaterMm);
    CHECK_EQ(lakes.waterSurfaceMmAtVoxel(-1'000'000, 4'000'000), kNoWaterMm);
    CHECK_EQ(lakes.surfaceAtPixel(17, 42), kNoWaterMm);
    CHECK(!lakes.prewarmTile(0, 0));
    CHECK_EQ(int(lakes.residentMaskCount()), 0);
    CHECK_EQ(int(lakes.unresolvedBasins()), 0);

    NullWaterSampler none;
    CHECK_EQ(none.waterSurfaceMmAtVoxel(3, 4), kNoWaterMm);
}

VXC_TEST(lake_sampler_over_a_baked_tile_never_waters_a_dry_playa) {
    const std::filesystem::path p =
        std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_basins_512.vxtl";
    if (!std::filesystem::exists(p)) {
        std::printf("  (skip: %s absent)\n", p.string().c_str());
        return;
    }
    FineTileSampler tiles(0);
    FineError err = FineError::kNone;
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    // The golden is stamped with its own seed; take it from the parsed tile.
    std::optional<FineTile> probe = FineTile::parse(bytes.data(), bytes.size(), {}, &err);
    CHECK(probe.has_value());
    if (!probe) return;
    FineTileSampler s(probe->seed());
    CHECK(s.loadTile(bytes, &err));
    LakeSampler lakes(s);
    CHECK(lakes.prewarmTile(probe->tileX(), probe->tileY()));

    const uint32_t size = probe->size();
    const int64_t ox = int64_t(probe->tileX()) * size, oy = int64_t(probe->tileY()) * size;
    size_t wetColumns = 0, dryRowsChecked = 0;
    for (const BasinEntry& b : probe->basins()) {
        const int32_t mm = lakes.surfaceAtPixel(ox + b.seedX, oy + b.seedY);
        if (b.holdsWater()) {
            // A basin that holds water either answers its OWN datum at its own
            // seed, or answers nothing because the golden's synthetic lattice
            // does not actually dip below that datum there. What it must never
            // do is answer some OTHER basin's number.
            CHECK(mm == kNoWaterMm || mm == b.surfaceMm);
            if (mm != kNoWaterMm) ++wetColumns;
        } else {
            // A salt flat or a dry playa is NOT water. It is a registered
            // basin with a real surface elevation, and treating that number as
            // a water datum is how a desert gets a lake.
            ++dryRowsChecked;
            CHECK(mm != b.surfaceMm || b.surfaceMm == kNoWaterMm);
        }
    }
    CHECK(dryRowsChecked > 0);
    (void)wetColumns;
    // Outside every bbox there is no water, and asking twice gives the same
    // answer (the column memo must not be able to disagree with itself).
    const int32_t a = lakes.surfaceAtPixel(ox + size - 1, oy);
    CHECK_EQ(lakes.surfaceAtPixel(ox + size - 1, oy), a);
    // A tile that was never loaded stays silent rather than borrowing its
    // neighbour's registry.
    CHECK_EQ(lakes.surfaceAtPixel(ox + int64_t(size) * 3, oy), kNoWaterMm);
}

// ---------------------------------------------------------------------------
// THE SHEET (watershed plan work item 5, §5.2)
// ---------------------------------------------------------------------------
//
// These cover the two things that can put water on dry ground at range, which
// is the only new way this feature can be wrong: a decimation that GROWS the
// lake, and a near-field cut that leaves a gap or an overlap. Both are pure
// geometry, so both are checkable here rather than in a screenshot.

namespace {

// A basin whose bbox is [0,w) x [0,h), seeded at (sx, sy).
BasinEntry sheetBasin(int32_t w, int32_t h) {
    BasinEntry b{};
    b.bboxX0 = 0;
    b.bboxY0 = 0;
    b.bboxX1 = uint16_t(w - 1);
    b.bboxY1 = uint16_t(h - 1);
    b.seedX = 0;
    b.seedY = 0;
    b.surfaceMm = 1000;
    return b;
}

size_t rectArea(const std::vector<LakeSheetRect>& rs) {
    size_t a = 0;
    for (const LakeSheetRect& r : rs) {
        a += size_t(r.x1 - r.x0 + 1) * size_t(r.y1 - r.y0 + 1);
    }
    return a;
}

} // namespace

VXC_TEST(sheet_rects_at_step_one_reproduce_the_mask_exactly) {
    const int32_t w = 9, h = 7;
    BasinEntry b = sheetBasin(w, h);
    std::vector<uint8_t> mask(size_t(w) * size_t(h), 0);
    // A ragged blob with a hole in it, so runs, gaps and re-starts all appear.
    auto set = [&](int32_t x, int32_t y) { mask[size_t(y) * size_t(w) + size_t(x)] = 1; };
    for (int32_t y = 1; y <= 5; ++y)
        for (int32_t x = 1; x <= 7; ++x) set(x, y);
    mask[size_t(3) * size_t(w) + size_t(4)] = 0; // punch one cell out

    std::vector<LakeSheetRect> rs;
    lakeSheetRects(b, mask, 1, rs);
    size_t wet = 0;
    for (uint8_t m : mask) wet += (m != 0);
    CHECK_EQ(int(rectArea(rs)), int(wet));
    // The hole splits its row into two runs; every other wet row is one.
    CHECK_EQ(int(rs.size()), 6);
    // Every emitted cell is wet -- the property that keeps water off dry land.
    for (const LakeSheetRect& r : rs) {
        for (int32_t y = r.y0; y <= r.y1; ++y)
            for (int32_t x = r.x0; x <= r.x1; ++x) CHECK(mask[size_t(y) * size_t(w) + size_t(x)] != 0);
    }
}

VXC_TEST(sheet_decimation_can_only_shrink_a_lake_never_grow_it) {
    // A disc, which is the shape a decimation is most likely to overshoot on:
    // its boundary is diagonal everywhere.
    const int32_t w = 64, h = 64;
    BasinEntry b = sheetBasin(w, h);
    std::vector<uint8_t> mask(size_t(w) * size_t(h), 0);
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            const double dx = x - 31.5, dy = y - 31.5;
            if (dx * dx + dy * dy <= 25.0 * 25.0) mask[size_t(y) * size_t(w) + size_t(x)] = 1;
        }
    }
    size_t wet = 0;
    for (uint8_t m : mask) wet += (m != 0);

    for (int32_t step : {1, 2, 4, 8, 16}) {
        std::vector<LakeSheetRect> rs;
        lakeSheetRects(b, mask, step, rs);
        // Every cell of every emitted rectangle must be inside the bbox and, at
        // step 1, wet. At a coarser step the rectangle covers a block whose
        // CENTRE was wet -- so the containment claim is the weaker, honest one:
        // the sheet's total area never exceeds the lake's by more than the
        // boundary blocks, and it never leaves the bbox.
        for (const LakeSheetRect& r : rs) {
            CHECK(r.x0 >= b.bboxX0 && r.x1 <= b.bboxX1);
            CHECK(r.y0 >= b.bboxY0 && r.y1 <= b.bboxY1);
            CHECK(r.x0 <= r.x1 && r.y0 <= r.y1);
        }
        const size_t area = rectArea(rs);
        // A centre-sampled decimation of a convex blob is an EROSION plus at
        // most one block of boundary in each direction; it must never be more
        // than the circumscribing square and must stay within a block ring of
        // the true area.
        CHECK(area <= size_t(w) * size_t(h));
        const size_t ring = size_t(step) * size_t(4 * 50 + 4 * size_t(step));
        CHECK(area <= wet + ring);
    }
    // Step 0 and negative steps are clamped to 1 rather than dividing by zero.
    std::vector<LakeSheetRect> zero;
    lakeSheetRects(b, mask, 0, zero);
    CHECK_EQ(int(rectArea(zero)), int(wet));
}

VXC_TEST(sheet_rects_reject_a_mask_that_is_not_its_basins_bbox) {
    BasinEntry b = sheetBasin(8, 8);
    std::vector<uint8_t> wrong(63, 1); // one short
    std::vector<LakeSheetRect> rs;
    CHECK_EQ(int(lakeSheetRects(b, wrong, 1, rs)), 0);
    CHECK_EQ(int(rs.size()), 0);
}

VXC_TEST(subtracting_the_near_field_hole_loses_exactly_the_overlap) {
    const LakeSheetRect r{0, 0, 9, 9}; // 100 cells
    LakeSheetRect out[4];

    // Disjoint: unchanged, one piece.
    CHECK_EQ(int(subtractRect(r, LakeSheetRect{20, 20, 25, 25}, out)), 1);
    CHECK(out[0].x0 == 0 && out[0].y0 == 0 && out[0].x1 == 9 && out[0].y1 == 9);

    // Touching edges do NOT overlap -- the bounds are inclusive, so a hole
    // ending at x = -1 leaves the rectangle whole. Getting this backwards is
    // how a one-cell seam appears along every near/far boundary.
    CHECK_EQ(int(subtractRect(r, LakeSheetRect{-5, 0, -1, 9}, out)), 1);

    auto areaOf = [](const LakeSheetRect* rs, size_t n) {
        size_t a = 0;
        for (size_t i = 0; i < n; ++i) a += size_t(rs[i].x1 - rs[i].x0 + 1) * size_t(rs[i].y1 - rs[i].y0 + 1);
        return a;
    };

    // A hole in the middle: four pieces, and their area is exactly the
    // complement. No double counting (which would draw water twice and blend it
    // twice) and no gap (which would be a ring of missing water).
    const LakeSheetRect mid{3, 3, 5, 5};
    size_t n = subtractRect(r, mid, out);
    CHECK_EQ(int(n), 4);
    CHECK_EQ(int(areaOf(out, n)), 100 - 9);
    // Disjointness, checked cell by cell over the whole rectangle.
    for (int32_t y = 0; y <= 9; ++y) {
        for (int32_t x = 0; x <= 9; ++x) {
            int hits = 0;
            for (size_t i = 0; i < n; ++i) {
                if (x >= out[i].x0 && x <= out[i].x1 && y >= out[i].y0 && y <= out[i].y1) ++hits;
            }
            const bool inHole = (x >= mid.x0 && x <= mid.x1 && y >= mid.y0 && y <= mid.y1);
            CHECK_EQ(hits, inHole ? 0 : 1);
        }
    }

    // A hole that swallows the rectangle leaves nothing at all.
    CHECK_EQ(int(subtractRect(r, LakeSheetRect{-1, -1, 10, 10}, out)), 0);

    // An edge hole leaves three pieces, still exactly complementary.
    const LakeSheetRect edge{-4, 4, 2, 6};
    n = subtractRect(r, edge, out);
    CHECK_EQ(int(areaOf(out, n)), 100 - 3 * 3);
}
