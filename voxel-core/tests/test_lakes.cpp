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

// ---------------------------------------------------------------------------
// THE RIVER DATUM (watershed plan P2, §5.1)
// ---------------------------------------------------------------------------
//
// THIS IS THE GUARD FOR THE MISTAKE THIS CODEBASE HAS NOW MADE THREE TIMES --
// once in Python and twice in C++ -- and which nothing tested until this file.
//
// There are three "grounds" here (tilestore.h::reconstructedGroundMm lists
// them) and the stored water DEPTH is only meaningful against the middle one:
//
//   lattice    elevationMmFromCp(cp)        the prefiltered control point
//   spline     reconstructedGroundMm(...)   what the bake subtracted   <-- this
//   amplified  GetSurfaceHeightUU(...)      spline + rills + bedding + warp
//
// Every one of them is a plausible-looking int32 of millimetres, they agree to
// within a voxel on flat ground, and picking the wrong one throws NOTHING. It
// draws the river at the wrong height -- and |cp - surface| reaches 5.6 m on
// the production world, which is 56 voxels of water hanging in the air or
// buried in the hillside.
//
// So this asserts the datum by CONSTRUCTION rather than by inspection: the
// value RiverSampler returns must equal depth + spline, must NOT equal
// depth + lattice, and the reach must come out FLAT ACROSS ITS WIDTH -- which
// is the physical statement, and the one the fixture's deliberately rough bed
// under a deliberately smooth reach exists to make checkable.

namespace {

// The P2 water golden. Its reach is 5 px wide centred on x = 128, running the
// full height of tile (-2,-4); its bed carries sub-LSB roughness the water must
// not inherit. See tools/make_water_fixture.py.
std::filesystem::path waterFixturePath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_water_512.vxtl";
}

} // namespace

VXC_TEST(river_datum_is_the_spline_not_the_lattice) {
    const std::filesystem::path p = waterFixturePath();
    if (!std::filesystem::exists(p)) {
        std::printf("  (skip: %s absent)\n", p.string().c_str());
        return;
    }
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    FineError err = FineError::kNone;
    std::optional<FineTile> probe = FineTile::parse(bytes.data(), bytes.size(), {}, &err);
    CHECK(probe.has_value());
    if (!probe) return;
    CHECK(probe->hasWater());

    FineTileSampler tiles(probe->seed());
    CHECK(tiles.loadTile(bytes, &err));
    RiverSampler rivers(tiles);
    CHECK(rivers.prewarmTile(probe->tileX(), probe->tileY()));

    const uint32_t size = probe->size();
    const int64_t ox = int64_t(probe->tileX()) * size;
    const int64_t oy = int64_t(probe->tileY()) * size;
    const FineTile* t = tiles.findTile(probe->tileX(), probe->tileY());
    CHECK(t != nullptr);
    if (!t) return;
    const uint32_t dim = t->blockDim();

    // INTERIOR ONLY. The 4x4 carrier stencil reaches one pixel outside the
    // queried one, so a pixel on the tile edge gathers control points from a
    // tile that is not loaded -- FineTileSampler answers 0 mm there, by
    // documented policy. That is a streaming question, not a datum question,
    // and mixing the two would make this test fail for the wrong reason.
    const uint32_t LO = 2, HI = size - 3;

    size_t wet = 0, latticeDiffers = 0;
    int64_t worstLatticeErrMm = 0;
    for (uint32_t ly = LO; ly <= HI; ly += 7) {
        std::vector<int16_t> depth, elev;
        CHECK(t->decodeWaterBlock(0, ly / dim, depth));
        CHECK(t->decodeElevBlock(0, ly / dim, elev));
        for (uint32_t lx = LO; lx < dim && lx <= HI; ++lx) {
            const size_t i = size_t(ly % dim) * dim + size_t(lx % dim);
            const int16_t d = depth[i];
            const int32_t got = rivers.surfaceAtPixel(ox + lx, oy + ly);
            if (d < 0) {
                // Dry must stay dry, whatever the ground says.
                CHECK_EQ(int(got), int(kNoWaterMm));
                continue;
            }
            ++wet;
            // 1. THE DATUM IS THE SPLINE, exactly.
            const int32_t spline =
                FineTile::waterMmFromDepth(d, reconstructedGroundMm(tiles, ox + lx, oy + ly));
            CHECK_EQ(int(got), int(spline));

            // 2. AND IT IS NOT THE LATTICE. Counted rather than asserted per
            //    pixel: the two coincide wherever the prefilter happened to
            //    leave a control point on its own sample, and a test that
            //    demanded a difference at EVERY pixel would be asserting a
            //    property of the fixture's noise instead of the rule.
            const int32_t lattice = FineTile::waterMmFromDepth(d, t->elevationMmFromCp(elev[i]));
            if (lattice != got) {
                ++latticeDiffers;
                const int64_t e = int64_t(lattice) - int64_t(got);
                worstLatticeErrMm = std::max<int64_t>(worstLatticeErrMm, e < 0 ? -e : e);
            }
        }
    }

    CHECK(wet > 0);                     // a vacuous pass is the failure mode here
    // THE ANTI-VACUITY CLAUSE. If the lattice and the spline agreed everywhere
    // this test would pass no matter which one the sampler used, and would go
    // on passing after a regression. The fixture's rough bed guarantees they
    // do not; this is what says so out loud.
    CHECK(latticeDiffers > 0);
    std::printf("  river datum: %zu wet px, lattice differs on %zu (worst %lld mm)\n",
                wet, latticeDiffers, (long long)worstLatticeErrMm);

    // 3. THE PHYSICAL STATEMENT: a water surface is FLAT ACROSS THE REACH.
    //    The bed under it is not -- the fixture's `samples()` carries a
    //    high-frequency term precisely so that a datum taken from the wrong
    //    ground reproduces the roughness. Adding the depth to the LATTICE, or
    //    to an amplified surface, fails here; adding it to the spline does not,
    //    because the same ripple is in both the bed and the reconstruction and
    //    cancels.
    int64_t worstSpreadMm = 0, worstLatticeSpreadMm = 0;
    size_t rows = 0;
    for (uint32_t ly = LO; ly <= HI; ly += 13) {
        std::vector<int16_t> depth, elev;
        CHECK(t->decodeWaterBlock(0, ly / dim, depth));
        CHECK(t->decodeElevBlock(0, ly / dim, elev));
        int64_t lo = INT64_MAX, hi = INT64_MIN, llo = INT64_MAX, lhi = INT64_MIN;
        size_t n = 0;
        for (uint32_t lx = 126; lx <= 130; ++lx) {   // the 5 px reach
            const size_t i = size_t(ly % dim) * dim + size_t(lx % dim);
            if (depth[i] < 0) continue;
            ++n;
            const int64_t v = rivers.surfaceAtPixel(ox + lx, oy + ly);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            const int64_t l = FineTile::waterMmFromDepth(depth[i], t->elevationMmFromCp(elev[i]));
            llo = std::min(llo, l);
            lhi = std::max(lhi, l);
        }
        if (n < 3) continue;
        ++rows;
        worstSpreadMm = std::max(worstSpreadMm, hi - lo);
        worstLatticeSpreadMm = std::max(worstLatticeSpreadMm, lhi - llo);
    }
    CHECK(rows > 0);
    std::printf("  cross-section spread over %zu rows: spline %lld mm, lattice %lld mm\n",
                rows, (long long)worstSpreadMm, (long long)worstLatticeSpreadMm);
    // ONE VOXEL, and the bound is the format's own: the reach is flat to within
    // the spline's reconstruction residual, which make_water_fixture.py
    // measures at max 42 mm against a 100 mm voxel.
    CHECK(worstSpreadMm <= 100);
    // And the wrong datum is measurably worse, which is what makes the bound
    // above evidence rather than a tolerance that anything would pass.
    CHECK(worstLatticeSpreadMm > worstSpreadMm);
}

VXC_TEST(river_sampler_answers_no_water_with_nothing_loaded) {
    FineTileSampler tiles(7);
    RiverSampler rivers(tiles);
    CHECK_EQ(rivers.waterSurfaceMmAtVoxel(0, 0), kNoWaterMm);
    CHECK_EQ(rivers.surfaceAtPixel(3, 9), kNoWaterMm);
    CHECK(!rivers.prewarmTile(0, 0));
    CHECK_EQ(int(rivers.unresolvedBlocks()), 0);

    // A tile with NO water plane answers dry rather than borrowing the
    // elevation lattice as if it were a datum -- the bake_ver 8 case, which is
    // most of the cache.
    const std::filesystem::path p =
        std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_512.vxtl";
    if (!std::filesystem::exists(p)) return;
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    FineError err = FineError::kNone;
    std::optional<FineTile> probe = FineTile::parse(bytes.data(), bytes.size(), {}, &err);
    if (!probe.has_value()) return;
    CHECK(!probe->hasWater());
    FineTileSampler dry(probe->seed());
    CHECK(dry.loadTile(bytes, &err));
    RiverSampler r2(dry);
    CHECK(!r2.prewarmTile(probe->tileX(), probe->tileY()));
    const int64_t ox = int64_t(probe->tileX()) * probe->size();
    const int64_t oy = int64_t(probe->tileY()) * probe->size();
    CHECK_EQ(r2.surfaceAtPixel(ox + 100, oy + 100), kNoWaterMm);
}

// Lakes and rivers as one query. The composite must never LOWER a lake's datum
// -- taking the min where they overlap would drain a lake into the river that
// feeds it -- and must pass a dry answer through from either side.
VXC_TEST(composite_takes_the_higher_datum_and_forwards_the_sheet_half) {
    struct Fixed final : IWaterSampler {
        int32_t mm = kNoWaterMm;
        int32_t waterSurfaceMmAtVoxel(int64_t, int64_t) override { return mm; }
        uint32_t tilePixels() const override { return 512; }
        int32_t pixelSizeMm() const override { return 1875; }
    };
    Fixed lake, river;
    CompositeWaterSampler both(lake, river);

    lake.mm = kNoWaterMm; river.mm = kNoWaterMm;
    CHECK_EQ(both.waterSurfaceMmAtVoxel(0, 0), kNoWaterMm);

    lake.mm = 5000; river.mm = kNoWaterMm;
    CHECK_EQ(both.waterSurfaceMmAtVoxel(0, 0), 5000);

    lake.mm = kNoWaterMm; river.mm = 4200;
    CHECK_EQ(both.waterSurfaceMmAtVoxel(0, 0), 4200);

    // Overlap: the HIGHER wins, both ways round.
    lake.mm = 5000; river.mm = 4200;
    CHECK_EQ(both.waterSurfaceMmAtVoxel(0, 0), 5000);
    lake.mm = 4200; river.mm = 5000;
    CHECK_EQ(both.waterSurfaceMmAtVoxel(0, 0), 5000);

    // kNoWaterMm is INT32_MIN, so a NEGATIVE datum -- a reach below sea level,
    // which a tidal mouth genuinely is -- must not be mistaken for dry.
    lake.mm = kNoWaterMm; river.mm = -3000;
    CHECK_EQ(both.waterSurfaceMmAtVoxel(0, 0), -3000);
    lake.mm = -5000; river.mm = -3000;
    CHECK_EQ(both.waterSurfaceMmAtVoxel(0, 0), -3000);

    // The sheet half is the LAKES, unconditionally: a river reach is not a
    // flat disc and must never be handed to the sheet builder.
    CHECK(both.basinsForTile(0, 0) == nullptr);
    CHECK(both.extentMaskFor(0, 0, 0) == nullptr);
}

// ---------------------------------------------------------------------------
// THE NEAR-FIELD SWEEP OFFERS NOTHING OVER DRY GROUND
// ---------------------------------------------------------------------------
//
// THE TEST WHOSE ABSENCE LET A 52 m DISC OF WATER FLY AROUND FOLLOWING THE
// CAMERA.
//
// On 2026-08-04 the owner flew at 618.4 m over ground at 78 m and a disc of
// water appeared 527 m above the terrain. Every datum producer -- lake, river,
// composite, ocean -- read DRY at that column, against every bake of the seed
// on disk. The water was manufactured by `RefreshImplicitWater`'s candidate
// sweep, which takes its ceiling as max(cavern flood level, baked datum) and
// was reading the cavern half from an Amplifier over a DIFFERENT WORLD --
// surface 638.451 m where the renderer draws 77.6 m, so flood levels reaching
// 606.166 m. The camera-centred box's floor sits at
// 618.4 - 16 bricks * 8 voxels * 0.1 m = 605.6 m, so 606.166 m admitted exactly
// ONE brick per column: a flat slab at the box floor. 511 of them, which is
// verbatim what the session log reported.
//
// Sixteen captures and a dozen probes missed it because EVERY ONE OF THEM
// SAMPLED NEAR THE WATER. So the assertion this file was missing is the
// negative one: over dry ground, at altitude, the sweep offers ZERO. That is
// what the first test below is, and the brick arithmetic is spelled out rather
// than collapsed to a ceiling comparison because the slab formed BETWEEN the
// ceiling and the candidate list.
namespace {

// RefreshImplicitWater's sweep, exactly: bricks [C-R, C+R] on x/y and
// [C-Rz, C+Rz] on z, a column rejected outright when it can hold no water at
// any height, and every brick at or below the column ceiling offered.
//
// The three constants are restated from the UE translation unit (they live in
// VoxelWaterSubsystem.cpp, which has no test harness -- the other half of why
// this bug survived). vxc_waterdatumprobe restates the same three for the same
// reason and its comment says so.
constexpr int64_t kSweepRadiusBricks = 32;
constexpr int64_t kSweepRadiusBricksZ = 16;
constexpr int64_t kSweepBrickEdge = 8; // WaterBrick8::kEdge

struct SweepColumn {
    CavernColumn cavern;
    int32_t bakedMm = kNoWaterMm;
    int64_t groundMm = 0;
};

// `column(bx, by)` returns the SweepColumn at that BRICK's origin.
template <class ColumnFn>
int64_t sweepCandidateBricks(int64_t camVx, int64_t camVy, int64_t camVz, ColumnFn&& column) {
    const int64_t cx = floorDiv(camVx, kSweepBrickEdge);
    const int64_t cy = floorDiv(camVy, kSweepBrickEdge);
    const int64_t cz = floorDiv(camVz, kSweepBrickEdge);
    int64_t candidates = 0;
    for (int64_t by = cy - kSweepRadiusBricks; by <= cy + kSweepRadiusBricks; ++by)
        for (int64_t bx = cx - kSweepRadiusBricks; bx <= cx + kSweepRadiusBricks; ++bx) {
            const SweepColumn col = column(bx, by);
            const int64_t ceilingMm = implicitWaterCeilingMm(col.cavern, col.bakedMm, col.groundMm);
            if (ceilingMm == kNoImplicitWaterMm) continue;
            const int64_t topBrickZ = floorDiv(ceilingMm / int64_t(kVoxelSizeMm), kSweepBrickEdge);
            for (int64_t bz = cz - kSweepRadiusBricksZ; bz <= cz + kSweepRadiusBricksZ; ++bz)
                if (bz <= topBrickZ) ++candidates;
        }
    return candidates;
}

// A column carrying a cavern site's flood level. `segs` is the number of rooms
// actually reaching THIS column -- zero for the overwhelming majority of them,
// because `floodZMm` is a per-SITE value stamped onto every column inside the
// site's ~36 m reach disc.
SweepColumn cavernSiteColumn(int32_t floodZMm, int32_t segs, int64_t groundMm) {
    SweepColumn c;
    c.cavern.floodZMm = floodZMm;
    c.cavern.count = segs;
    for (int32_t i = 0; i < segs && i < kMaxCavernSegs; ++i) {
        c.cavern.segs[i].marginSq = 1;
        c.cavern.segs[i].zCenterMm = floodZMm;
        c.cavern.segs[i].zFloorMm = floodZMm - 1000;
    }
    c.groundMm = groundMm;
    return c;
}

} // namespace

// The owner's frame, in the numbers the log and the overlay reported: camera
// 618.4 m, ground 78.0 m, every datum producer dry, and a cavern flood level of
// 606.166 m over the disc. ZERO bricks. Without the ground bound in
// `cavernWaterCeilingMm` case 2 returns 4225 and case 3 returns 4225 -- one per
// column, the slab; the owner's frame had a cavern site in reach of 511 of the
// 4225 and got 511.
VXC_TEST(implicit_sweep_offers_nothing_over_dry_ground_at_altitude) {
    const int64_t camVx = -1603970, camVy = -853968; // world (-160397.0, -85396.8) m
    const int64_t camVz = 6184;                      // 618.4 m
    const int64_t groundMm = 77997;                  // the overlay's 78.0 m

    // 1. Nothing in the column at all: no cavern site in reach, no baked datum.
    {
        const int64_t n = sweepCandidateBricks(camVx, camVy, camVz, [&](int64_t, int64_t) {
            SweepColumn c;
            c.groundMm = groundMm;
            return c;
        });
        CHECK_EQ(n, int64_t(0));
    }

    // 2. THE MEASURED CASE. A cavern site is in reach of the column and its
    //    flood level -- derived from the wrong world's 638 m surface -- stands
    //    at 606.166 m, just inside the box floor at 605.6 m. The datum is dry.
    //    The ground at 78 m is the only fact that can save this.
    {
        const int64_t n = sweepCandidateBricks(camVx, camVy, camVz, [&](int64_t, int64_t) {
            return cavernSiteColumn(606166, /*segs=*/0, groundMm);
        });
        CHECK_EQ(n, int64_t(0));
    }

    // 3. ...and still zero where the column DOES contain cavern air, which 4 of
    //    the owner's 511 did. "There is a real room here" is not a licence to
    //    put water 528 m above the ground that room is cut into.
    {
        const int64_t n = sweepCandidateBricks(camVx, camVy, camVz, [&](int64_t, int64_t) {
            return cavernSiteColumn(606166, /*segs=*/2, groundMm);
        });
        CHECK_EQ(n, int64_t(0));
    }

    // 4. The per-voxel rule agrees with the per-column ceiling that bounds it,
    //    at the box-floor voxel the disc was actually drawn on. These are one
    //    rule in two shapes, and a disagreement between them is either a hole
    //    in a lake or a slab in the sky.
    const CavernColumn wrongWorld = cavernSiteColumn(606166, 2, groundMm).cavern;
    const int64_t floorVz =
        (floorDiv(camVz, kSweepBrickEdge) - kSweepRadiusBricksZ) * kSweepBrickEdge;
    CHECK_EQ(floorVz, int64_t(6056));                     // 605.6 m, the disc's height
    CHECK(cavernFloodedAt(wrongWorld, floorVz));          // half the predicate says yes
    CHECK(!cavernWaterAt(wrongWorld, floorVz, groundMm)); // the whole rule says no
}

// THE OTHER DIRECTION, and it is what makes the test above a fix rather than a
// mute button. A skip that swallows real water is the exact defect this sweep
// has already been repaired for twice -- a baked lake that was offered no
// bricks at all, and a lake interior offered and meshed at full price. So: the
// same sweep, over water that is really there, must still offer it.
VXC_TEST(implicit_sweep_still_offers_real_cavern_and_lake_water) {
    // A flooded cavern 200 m down: ground 300 m, flood 100 m, camera in the
    // room at 99 m.
    {
        const int64_t camVz = 990;
        const int64_t n = sweepCandidateBricks(0, 0, camVz, [](int64_t, int64_t) {
            return cavernSiteColumn(100000, /*segs=*/2, 300000);
        });
        const int64_t cz = floorDiv(camVz, kSweepBrickEdge);
        const int64_t top = floorDiv(int64_t(100000) / int64_t(kVoxelSizeMm), kSweepBrickEdge);
        const int64_t perColumn = top - (cz - kSweepRadiusBricksZ) + 1;
        const int64_t columns = (2 * kSweepRadiusBricks + 1) * (2 * kSweepRadiusBricks + 1);
        CHECK(perColumn > 0);
        CHECK_EQ(n, perColumn * columns);
    }

    // A baked lake standing ABOVE its bed -- the ordinary wet case, and the one
    // that forbids clamping the baked term by the ground the way the cavern
    // term is clamped. Ground 20 m, datum 25 m, camera just over the surface.
    {
        const int64_t n = sweepCandidateBricks(0, 0, 260, [](int64_t, int64_t) {
            SweepColumn c;
            c.bakedMm = 25000;
            c.groundMm = 20000;
            return c;
        });
        CHECK(n > 0);
    }

    // ...and the ceiling itself, so the asymmetry is asserted rather than
    // implied by a brick count: the cavern term is bounded ABOVE by the ground,
    // the baked term is not bounded by it at all.
    const CavernColumn none;
    CHECK_EQ(implicitWaterCeilingMm(none, 25000, 20000), int64_t(25000));
    CHECK_EQ(implicitWaterCeilingMm(cavernSiteColumn(30000, 2, 20000).cavern, kNoWaterMm, 20000),
             int64_t(20000));
    CHECK_EQ(implicitWaterCeilingMm(cavernSiteColumn(15000, 2, 20000).cavern, kNoWaterMm, 20000),
             int64_t(15000));
    CHECK_EQ(implicitWaterCeilingMm(none, kNoWaterMm, 20000), kNoImplicitWaterMm);

    // kNoWaterMm is INT32_MIN and a ceiling is an int64: a legitimately
    // NEGATIVE datum (a tidal reach below sea level) must not read as dry, and
    // the dry sentinel must not read as "water at a very low elevation".
    CHECK_EQ(implicitWaterCeilingMm(none, -3000, -10000), int64_t(-3000));
    CHECK(implicitWaterCeilingMm(none, kNoWaterMm, -10000) < int64_t(-3000));
}

// THE GROUND BOUND CAN ONLY EVER REMOVE WATER, AND ONLY FROM OPEN SKY.
//
// The implicit field is what makes unmobilized water a WALL to the CA, so a
// change to it that ADDS water anywhere is a ledger shortfall rather than a
// crash -- water quietly appearing, blamed later on serialization or on
// replication. This is the exhaustive version of "it can only remove": every
// flood level, ground and z in a range that straddles both, checked as integer
// equality and implication, never as a tolerance.
//
// The third claim is the one that makes the bound safe to ship: where the flood
// level is at or below the ground -- which is every cavern in a world whose
// flood levels were derived from the surface being drawn -- the new predicate
// is BIT-IDENTICAL to the old one. The bound is a no-op on correct data and a
// hard stop on the disagreement between two worlds.
VXC_TEST(cavern_ground_bound_only_removes_and_the_ceiling_bounds_it) {
    const int32_t kFloods[] = {INT32_MIN, -5000, 0, 1, 12345, 77997, 606166};
    const int64_t kGrounds[] = {-8000, 0, 20000, 77997, 300000};
    for (int32_t flood : kFloods) {
        for (int64_t ground : kGrounds) {
            const int64_t ceiling = cavernWaterCeilingMm(flood, ground);
            bool identical = true;
            for (int64_t vz = -200; vz <= 7000; vz += 7) {
                const bool oldRule = cavernFloodedAtLevel(flood, vz);
                const bool newRule = cavernWaterAt(flood, vz, ground);
                // 1. NEVER ADDS: the new rule is a subset of the old one.
                CHECK(!newRule || oldRule);
                // 2. THE CEILING BOUNDS THE PER-VOXEL RULE. A voxel the rule
                //    fills must lie under the ceiling the sweep offers bricks
                //    up to -- otherwise the sweep withholds water that exists,
                //    which is the failure mode this sweep has already had twice.
                if (newRule) CHECK(vz * kVoxelSizeMm < ceiling);
                if (oldRule != newRule) identical = false;
            }
            // 3. A NO-OP ON CORRECT DATA: a flood level at or below the ground
            //    (every cavern whose site surface is the surface on screen)
            //    gives exactly the old answer at every z.
            if (flood == INT32_MIN || int64_t(flood) <= ground) CHECK(identical);
        }
    }

    // And the sentinel is not a very low elevation: a dry column has no ceiling
    // at all, which is a different thing from a ceiling below every voxel.
    CHECK_EQ(cavernWaterCeilingMm(INT32_MIN, 300000), INT64_MIN);
    CHECK(!cavernWaterAt(INT32_MIN, -100000, 300000));
}

// UNLOADING A TILE MUST TAKE ITS LAKE INDEX WITH IT.
//
// `LakeSampler::TileIndex::basins` is a pointer BORROWED from a resident
// FineTile; `FineTileSampler::unloadTile` destroys that tile. Before this test
// the index was built once and never revalidated, so the query after an unload
// read freed memory -- and what a freed BasinEntry draws is a lake sheet at an
// arbitrary Z. Not reachable in the shipping client only because
// FLakeWaterSampler owns a private sampler that never unloads; the streamer
// already calls unloadTile, so a byte budget on the lake tier is all it takes.
//
// The assertion is deliberately "answers DRY", not "does not crash": reading
// freed memory usually does not crash, which is the entire problem.
VXC_TEST(lake_sampler_index_does_not_outlive_its_tile) {
    const std::filesystem::path p =
        std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_basins_512.vxtl";
    if (!std::filesystem::exists(p)) {
        std::printf("  (skip: %s absent)\n", p.string().c_str());
        return;
    }
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    FineError err = FineError::kNone;
    std::optional<FineTile> probe = FineTile::parse(bytes.data(), bytes.size(), {}, &err);
    CHECK(probe.has_value());
    if (!probe) return;

    // A basin that actually holds water, so "wet before, dry after" is a real
    // transition rather than two dry answers.
    const BasinEntry* wet = nullptr;
    for (const BasinEntry& b : probe->basins()) {
        if (b.holdsWater()) {
            wet = &b;
            break;
        }
    }
    if (wet == nullptr) {
        std::printf("  (skip: fixture has no water-holding basin)\n");
        return;
    }
    const int32_t tx = probe->tileX(), ty = probe->tileY();
    const uint32_t size = probe->size();
    const int64_t sx = int64_t(tx) * size + wet->seedX;
    const int64_t sy = int64_t(ty) * size + wet->seedY;

    FineTileSampler s(probe->seed());
    CHECK(s.loadTile(bytes, &err));
    LakeSampler lakes(s);
    CHECK(lakes.prewarmTile(tx, ty));
    CHECK(lakes.surfaceAtPixel(sx, sy) != kNoWaterMm);
    CHECK(lakes.basinsForTile(tx, ty) != nullptr);

    CHECK(s.unloadTile(tx, ty));
    CHECK_EQ(lakes.surfaceAtPixel(sx, sy), kNoWaterMm);
    CHECK(lakes.basinsForTile(tx, ty) == nullptr);
    CHECK(lakes.extentMaskFor(tx, ty, 0) == nullptr);

    // Reloading the same bytes gives a DIFFERENT FineTile object, so the index
    // must be rebuilt against it rather than resurrected -- and the answer must
    // come back.
    CHECK(s.loadTile(bytes, &err));
    CHECK(lakes.surfaceAtPixel(sx, sy) != kNoWaterMm);
    CHECK(lakes.basinsForTile(tx, ty) != nullptr);
}
