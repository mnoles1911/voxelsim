// Integer DDA raycast (voxelcore/raycast.h): exactness, face/placement
// reporting, negative-coordinate behavior, determinism.

#include "voxelcore/raycast.h"
#include "vxctest.h"

using namespace vxc;

namespace {

// Flat floor: everything with vz < 0 is solid rock.
MaterialId floorWorld(int64_t, int64_t, int64_t vz) {
    return vz < 0 ? MAT_ROCK : MAT_AIR;
}

// One solid voxel at (5, 0, 0).
MaterialId singleBlock(int64_t vx, int64_t vy, int64_t vz) {
    return (vx == 5 && vy == 0 && vz == 0) ? MAT_ROCK : MAT_AIR;
}

} // namespace

VXC_TEST(raycast_axis_aligned_hit_and_face) {
    // From (0.5, 0.5, 0.5)m voxel (5,5,5)... use origin voxel (0,0,0) centre,
    // shooting +x at the block at (5,0,0).
    const auto hit = raycastVoxels(singleBlock, 50, 50, 50, 1000, 0, 0);
    CHECK(hit.hit);
    CHECK_EQ(hit.vx, 5);
    CHECK_EQ(hit.vy, 0);
    CHECK_EQ(hit.vz, 0);
    CHECK_EQ(hit.faceAxis, 0);
    CHECK_EQ(hit.faceSign, 1); // entered moving +x, through the -x face
    CHECK_EQ(hit.px, 4);       // placement voxel in front of the face
    CHECK_EQ(hit.py, 0);
    CHECK_EQ(hit.pz, 0);
}

VXC_TEST(raycast_miss_when_segment_too_short) {
    const auto hit = raycastVoxels(singleBlock, 50, 50, 50, 400, 0, 0);
    CHECK(!hit.hit); // segment ends inside voxel 4, one short of the block
}

VXC_TEST(raycast_down_into_floor) {
    // Standing at 2m above the floor, looking straight down.
    const auto hit = raycastVoxels(floorWorld, 12345, -6789, 2000, 0, 0, -2500);
    CHECK(hit.hit);
    CHECK_EQ(hit.vz, -1);
    CHECK_EQ(hit.faceAxis, 2);
    CHECK_EQ(hit.faceSign, -1); // entered moving -z, through the +z face
    CHECK_EQ(hit.pz, 0);        // placement sits on top of the floor
    CHECK_EQ(hit.vx, floorDiv(12345, 100));
    CHECK_EQ(hit.vy, floorDiv(-6789, 100));
}

VXC_TEST(raycast_start_inside_solid) {
    const auto hit = raycastVoxels(floorWorld, 0, 0, -50, 500, 300, 700);
    CHECK(hit.hit);
    CHECK_EQ(hit.faceAxis, -1); // no face crossed
    CHECK_EQ(hit.vz, -1);
    CHECK_EQ(hit.px, hit.vx);
    CHECK_EQ(hit.pz, hit.vz);
}

VXC_TEST(raycast_diagonal_hits_floor_at_expected_column) {
    // 45° down-forward from (0,0,1m): dx=dz so it reaches z=0 after 1m of x.
    const auto hit = raycastVoxels(floorWorld, 0, 0, 1000, 3000, 0, -3000);
    CHECK(hit.hit);
    CHECK_EQ(hit.vz, -1);
    // At the moment z crosses below 0, x has advanced exactly 1000mm ⇒ x
    // voxel 10 (tie at the corner steps x before z, so the floor is entered
    // in column 10).
    CHECK_EQ(hit.vx, 10);
    CHECK_EQ(hit.faceAxis, 2);
}

VXC_TEST(raycast_zero_direction_is_a_miss) {
    const auto hit = raycastVoxels(singleBlock, 50, 50, 50, 0, 0, 0);
    CHECK(!hit.hit);
}

VXC_TEST(raycast_deterministic_and_symmetric_across_sign) {
    // The same geometric ray expressed from either end must hit the same
    // block faces deterministically run-to-run (not necessarily the same
    // voxel from both directions — but each direction must self-agree).
    for (int rep = 0; rep < 3; ++rep) {
        const auto a = raycastVoxels(singleBlock, 50, 20, 30, 900, -10, -20);
        const auto b = raycastVoxels(singleBlock, 50, 20, 30, 900, -10, -20);
        CHECK_EQ(a.hit, b.hit);
        CHECK_EQ(a.vx, b.vx);
        CHECK_EQ(a.vy, b.vy);
        CHECK_EQ(a.vz, b.vz);
        CHECK_EQ(a.faceAxis, b.faceAxis);
    }
    // And from the far side, the block is entered through its +x face.
    const auto back = raycastVoxels(singleBlock, 1050, 50, 50, -1000, 0, 0);
    CHECK(back.hit);
    CHECK_EQ(back.faceSign, -1);
    CHECK_EQ(back.px, 6);
}

VXC_TEST(raycast_negative_coordinates_boundary_exactness) {
    // Origin exactly on a voxel boundary moving negative: the crossing at
    // t=0 immediately enters the next voxel down.
    const auto hit = raycastVoxels(floorWorld, -200, 300, 0, 0, 0, -100);
    CHECK(hit.hit);
    CHECK_EQ(hit.vx, -2);
    CHECK_EQ(hit.vy, 3);
    CHECK_EQ(hit.vz, -1);
}
