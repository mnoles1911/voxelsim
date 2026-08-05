// Far-field voxel water (farwater.h): the LOD ring rule, the coarse fill, the
// majority aggregation, the water-column vertical bound and the interior proof.
//
// Every test here pins something a MEASUREMENT forced, not something a design
// preferred. The two that matter most:
//
//   * `coarse_fill_does_not_erase_shallow_water` -- the first cut of the coarse
//     fill reused `implicitWaterFill`'s "cell bottom below the ground is rock"
//     branch, and at a 1.6 m cell over p50-0.75 m water that made every cell
//     read dry. The probe meshed ZERO quads for 138 offered surface bricks. A
//     river that silently vanishes at distance is the exact defect this feature
//     exists to remove, so it gets a test.
//
//   * `lod0_is_exactly_the_near_field` -- the near field must not move by one
//     unit. Nothing here is allowed to be "equivalent to" implicitWaterFill.

#include <vector>

#include "voxelcore/farwater.h"
#include "vxctest.h"

using namespace vxc;

namespace {

// A tiny grid of columns, dry outside its bounds -- the shape every caller
// hands to farWaterBrickIsInterior.
struct Grid {
    int w = 0, h = 0;
    std::vector<FarWaterColumn> cells;
    FarWaterColumn at(int x, int y) const {
        if (x < 0 || x >= w || y < 0 || y >= h) return FarWaterColumn{};
        return cells[size_t(y) * size_t(w) + size_t(x)];
    }
};

Grid uniformGrid(int w, int h, int32_t groundMm, int32_t datumMm) {
    Grid g;
    g.w = w;
    g.h = h;
    g.cells.assign(size_t(w) * size_t(h), FarWaterColumn{groundMm, datumMm});
    return g;
}

} // namespace

// --- the ring rule ---------------------------------------------------------

VXC_TEST(farwater_lod_ring_boundaries_are_exact) {
    // base 40 bricks (32 m at 0.8 m bricks), 5 levels.
    constexpr int64_t base = 40;
    constexpr int maxLod = 5;
    // Inside the base radius is LOD 0, and the boundary belongs to the OUTER
    // ring (half-open [inner, outer)), so `base` itself is already LOD 1.
    CHECK_EQ(farWaterLodForDistance(0, base, maxLod), 0);
    CHECK_EQ(farWaterLodForDistance(base - 1, base, maxLod), 0);
    CHECK_EQ(farWaterLodForDistance(base, base, maxLod), 1);
    CHECK_EQ(farWaterLodForDistance(2 * base - 1, base, maxLod), 1);
    CHECK_EQ(farWaterLodForDistance(2 * base, base, maxLod), 2);
    CHECK_EQ(farWaterLodForDistance(4 * base, base, maxLod), 3);
    CHECK_EQ(farWaterLodForDistance(8 * base, base, maxLod), 4);
    CHECK_EQ(farWaterLodForDistance(16 * base, base, maxLod), 5);
    // Clamped, never past maxLod, however far out the column is.
    CHECK_EQ(farWaterLodForDistance(1'000'000, base, maxLod), maxLod);
    // A zero or negative base must not divide by zero or loop; it degenerates
    // to "everything is near field".
    CHECK_EQ(farWaterLodForDistance(1'000'000, 0, maxLod), 0);
}

VXC_TEST(farwater_cell_and_brick_sizes_double_per_level) {
    CHECK_EQ(farWaterCellMm(0), int64_t(kVoxelSizeMm));
    CHECK_EQ(farWaterCellMm(1), int64_t(200));
    CHECK_EQ(farWaterCellMm(5), int64_t(3200));
    // A brick stays 8 cells at every level -- that is what keeps meshBrick<8>
    // and the whole upload path untouched.
    CHECK_EQ(farWaterBrickMm(0), int64_t(800));
    CHECK_EQ(farWaterBrickMm(5), int64_t(25600));
    CHECK_EQ(farWaterStep(0), int64_t(1));
    CHECK_EQ(farWaterStep(4), int64_t(16));
}

// --- the fill --------------------------------------------------------------

VXC_TEST(lod0_is_exactly_the_near_field) {
    // Not "equivalent to" implicitWaterFill: the same value, every cell, over a
    // range that crosses the ground, the datum and the partial top.
    const int32_t ground = 100'000;
    const int32_t datum = 100'750;
    for (int64_t vz = 990; vz <= 1020; ++vz) {
        const uint8_t fine = implicitWaterFill(vz, ground, datum, false);
        const uint8_t far = farWaterFill(vz * int64_t(kVoxelSizeMm), ground, datum,
                                         farWaterCellMm(0));
        CHECK_EQ(int(far), int(fine));
    }
    // Dry stays dry at LOD 0.
    CHECK_EQ(int(farWaterFill(100'000, ground, kNoWaterMm, farWaterCellMm(0))), 0);
}

VXC_TEST(coarse_fill_does_not_erase_shallow_water) {
    // THE REGRESSION. Ground at 100.000 m, water 0.75 m deep -- the wet-country
    // p50. At LOD 4 the cell is 1.6 m, so the whole water column fits inside
    // one cell AND the ground falls in that cell's interior.
    const int32_t ground = 100'000;
    const int32_t datum = 100'750;
    const int64_t cell = farWaterCellMm(4); // 1600 mm
    CHECK_EQ(cell, int64_t(1600));

    // The cell the ground falls in starts below it. Under the fine rule this is
    // rejected as rock and the water is lost; here it must carry the water.
    const int64_t zBottom = floorDiv(int64_t(ground), cell) * cell; // 99'200
    CHECK(zBottom < ground);
    CHECK(zBottom + cell > datum);
    const uint8_t fill = farWaterFill(zBottom, ground, datum, cell);
    CHECK(fill > 0);
    // The fill is the datum's own remainder in the cell, so the surface height
    // the corner field will draw is the true one.
    CHECK_EQ(int(fill), int((int64_t(datum - zBottom) * 255 + cell / 2) / cell));

    // And at least one cell in the column is non-empty at EVERY level -- which
    // is the property "the river gets coarser, it does not disappear".
    for (int lod = 0; lod <= kFarWaterMaxLod; ++lod) {
        const int64_t c = farWaterCellMm(lod);
        bool anyFill = false;
        for (int64_t k = -2; k <= 2; ++k) {
            const int64_t zb = floorDiv(int64_t(ground), c) * c + k * c;
            if (farWaterFill(zb, ground, datum, c) > 0) anyFill = true;
        }
        CHECK(anyFill);
    }
}

VXC_TEST(coarse_fill_still_refuses_rock_and_air) {
    const int32_t ground = 100'000;
    const int32_t datum = 100'750;
    const int64_t cell = farWaterCellMm(3); // 800 mm
    // Entirely below the ground: rock, no water.
    CHECK_EQ(int(farWaterFill(ground - 5 * cell, ground, datum, cell)), 0);
    // Entirely above the datum: air.
    CHECK_EQ(int(farWaterFill(datum, ground, datum, cell)), 0);
    CHECK_EQ(int(farWaterFill(datum + cell, ground, datum, cell)), 0);
    // Dry column is dry at every level.
    for (int lod = 0; lod <= kFarWaterMaxLod; ++lod) {
        CHECK_EQ(int(farWaterFill(0, ground, kNoWaterMm, farWaterCellMm(lod))), 0);
    }
}

VXC_TEST(coarse_fill_saturates_in_deep_water) {
    // A 45 m lake: cells well below the surface and well above the bed must
    // read 255 at every level, or a deep lake draws internal faces.
    const int32_t ground = 0;
    const int32_t datum = 45'000;
    for (int lod = 0; lod <= kFarWaterMaxLod; ++lod) {
        const int64_t c = farWaterCellMm(lod);
        CHECK_EQ(int(farWaterFill(20'000, ground, datum, c)), 255);
    }
}

// --- the aggregation -------------------------------------------------------

VXC_TEST(majority_rule_keeps_half_and_drops_less) {
    // step 2 -> four children.
    FarWaterAccumulator half;
    half.add(FarWaterColumn{1000, 2000});
    half.add(FarWaterColumn{1000, 2000});
    half.add(FarWaterColumn{});
    half.add(FarWaterColumn{});
    FarWaterColumn out;
    CHECK(half.resolve(2, out)); // exactly half is WET
    CHECK_EQ(out.groundMm, 1000);
    CHECK_EQ(out.datumMm, 2000);

    FarWaterAccumulator minority;
    minority.add(FarWaterColumn{1000, 2000});
    minority.add(FarWaterColumn{});
    minority.add(FarWaterColumn{});
    minority.add(FarWaterColumn{});
    FarWaterColumn out2;
    CHECK(!minority.resolve(2, out2)); // one of four is DRY

    FarWaterAccumulator none;
    for (int i = 0; i < 4; ++i) none.add(FarWaterColumn{});
    FarWaterColumn out3;
    CHECK(!none.resolve(2, out3));
}

VXC_TEST(aggregation_averages_over_wet_children_only) {
    // A reach descending across the coarse cell: the coarse datum must be the
    // MEAN of the wet children, not the max, or every ring boundary is a step.
    FarWaterAccumulator a;
    a.add(FarWaterColumn{1000, 2000});
    a.add(FarWaterColumn{1000, 2400});
    a.add(FarWaterColumn{1000, 2200});
    a.add(FarWaterColumn{});          // dry, must not pull the mean down
    FarWaterColumn out;
    CHECK(a.resolve(2, out));
    CHECK_EQ(out.datumMm, 2200); // (2000+2400+2200)/3
    CHECK_EQ(out.groundMm, 1000);
    CHECK_EQ(a.wetChildren(), int64_t(3));
    CHECK_EQ(a.seenChildren(), int64_t(4));
}

VXC_TEST(aggregation_ignores_columns_whose_datum_is_below_ground) {
    // `wet()` is datum ABOVE ground, not merely "datum present". A column whose
    // datum has been buried by the amplifier is not water.
    FarWaterAccumulator a;
    a.add(FarWaterColumn{5000, 4000}); // datum below ground
    a.add(FarWaterColumn{5000, 5000}); // datum EQUAL to ground: not water
    a.add(FarWaterColumn{1000, 2000});
    a.add(FarWaterColumn{1000, 2000});
    FarWaterColumn out;
    CHECK(a.resolve(2, out)); // two of four wet
    CHECK_EQ(out.datumMm, 2000);
    CHECK_EQ(a.wetChildren(), int64_t(2));
}

// --- the vertical bound ----------------------------------------------------

VXC_TEST(brick_range_comes_from_the_water_column_not_a_box) {
    // THE POINT OF THIS TEST is that the range does not mention the camera.
    // kImplicitRadiusBricksZ made water stop existing 12.8 m above or below the
    // camera; a column 1 m deep here costs one or two bricks WHEREVER the
    // camera is, and a 45 m lake costs the bricks it needs.
    const int64_t cell = farWaterCellMm(0);
    const FarWaterColumn shallow{100'000, 100'750};
    const FarWaterBrickRange r = farWaterBrickRange(shallow, cell);
    CHECK(r.any());
    CHECK(r.count() <= 2);

    const FarWaterColumn deep{0, 45'000};
    const FarWaterBrickRange d = farWaterBrickRange(deep, cell);
    CHECK(d.any());
    // 45 m of water in 0.8 m bricks.
    CHECK_EQ(d.count(), int64_t(57));

    // Dry gives no bricks at all, and `any()` says so rather than returning a
    // degenerate range a caller might loop over.
    const FarWaterBrickRange dry = farWaterBrickRange(FarWaterColumn{}, cell);
    CHECK(!dry.any());
    CHECK_EQ(dry.count(), int64_t(0));

    // Coarser levels cost proportionally fewer bricks for the same lake.
    CHECK(farWaterBrickRange(deep, farWaterCellMm(3)).count() <
          farWaterBrickRange(deep, farWaterCellMm(0)).count());
}

VXC_TEST(brick_range_is_correct_below_sea_level) {
    // Negative absolute millimetres: floorDiv, not truncation. A seafloor reach
    // truncated toward zero puts its bricks one level too high.
    const FarWaterColumn c{-45'000, -44'000};
    const FarWaterBrickRange r = farWaterBrickRange(c, farWaterCellMm(0));
    CHECK(r.any());
    CHECK(r.z0 <= r.z1);
    CHECK_EQ(r.z0, floorDiv(floorDiv(int64_t(-45'000), int64_t(100)), int64_t(8)));
}

// --- the interior proof ----------------------------------------------------

VXC_TEST(interior_proof_refuses_at_a_shoreline) {
    // Deep uniform water: a brick in the middle of it is interior and emits no
    // face, which is what makes a large lake affordable.
    const int64_t cell = farWaterCellMm(0);
    Grid g = uniformGrid(3, 3, 0, 45'000);
    auto col = [&](int dx, int dy) { return g.at(1 + dx, 1 + dy); };
    // A brick well inside the body.
    CHECK(farWaterBrickIsInterior(20, cell, col));

    // One dry neighbour -- the shoreline -- and the proof must refuse. An
    // over-eager skip here punches a hole in a water surface.
    g.cells[0] = FarWaterColumn{};
    CHECK(!farWaterBrickIsInterior(20, cell, col));
}

VXC_TEST(interior_proof_refuses_at_the_surface_and_the_bed) {
    const int64_t cell = farWaterCellMm(0);
    const Grid g = uniformGrid(3, 3, 0, 45'000);
    auto col = [&](int dx, int dy) { return g.at(1 + dx, 1 + dy); };
    // The brick holding the datum has a top face.
    const FarWaterBrickRange r = farWaterBrickRange(FarWaterColumn{0, 45'000}, cell);
    CHECK(!farWaterBrickIsInterior(r.z1, cell, col));
    // The brick holding the bed has a bottom face.
    CHECK(!farWaterBrickIsInterior(r.z0, cell, col));
}

VXC_TEST(interior_proof_never_fires_on_shallow_water) {
    // p50 0.75 m water is thinner than one brick at every level, so no brick in
    // it can be interior. This is why the measured `surf brk` equals
    // `water brk` on a river: there is nothing to skip, and a rule that claimed
    // otherwise would be deleting the river.
    for (int lod = 0; lod <= kFarWaterMaxLod; ++lod) {
        const int64_t cell = farWaterCellMm(lod);
        const FarWaterColumn c{100'000, 100'750};
        const Grid g = uniformGrid(3, 3, c.groundMm, c.datumMm);
        auto col = [&](int dx, int dy) { return g.at(1 + dx, 1 + dy); };
        const FarWaterBrickRange r = farWaterBrickRange(c, cell);
        CHECK(r.any());
        for (int64_t bz = r.z0; bz <= r.z1; ++bz) {
            CHECK(!farWaterBrickIsInterior(bz, cell, col));
        }
    }
}

// --- the handover ----------------------------------------------------------

VXC_TEST(outer_radius_is_the_whole_cascade) {
    // The flat ribbon and sheet quads must be cut against the OUTER radius, not
    // against the 25.6 m implicit disc, or they are drawn underneath the voxel
    // water for the whole cascade.
    CHECK_EQ(farWaterOuterBricks(40, 5), int64_t(40 * 32)); // 32 m base -> 1024 m
    CHECK_EQ(farWaterOuterBricks(40, 0), int64_t(40));
    CHECK_EQ(farWaterOuterBricks(0, 5), int64_t(0));
    // And it agrees with the ring rule: the first distance NOT covered by the
    // cascade is exactly the outer radius.
    constexpr int64_t base = 40;
    CHECK_EQ(farWaterLodForDistance(farWaterOuterBricks(base, 5) - 1, base, 5), 5);
    CHECK_EQ(farWaterLodForDistance(farWaterOuterBricks(base, 5), base, 5), 5);
}
