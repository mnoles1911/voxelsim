// World = generated function + overlay: edits shadow generation, untouched
// bricks stay lazy (doctrine §2.1).

#include "voxelcore/world.h"
#include "vxctest.h"

using namespace vxc;

namespace {
constexpr uint64_t kSeed = 777;
}

VXC_TEST(world_dig_and_place) {
    SyntheticTileSampler tiles(kSeed);
    World<16> w(kSeed, tiles);
    const ColumnSample col = w.amplifier().column(10, 10);
    const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);

    // Dig: surface voxel becomes air.
    CHECK(w.materialAt(10, 10, topVz) != MAT_AIR);
    w.setVoxel(10, 10, topVz, MAT_AIR);
    CHECK_EQ(w.materialAt(10, 10, topVz), MAT_AIR);

    // Place: floating block above the surface.
    CHECK_EQ(w.materialAt(10, 10, topVz + 5), MAT_AIR);
    w.setVoxel(10, 10, topVz + 5, MAT_ROCK);
    CHECK_EQ(w.materialAt(10, 10, topVz + 5), MAT_ROCK);

    // Neighboring columns are untouched by the overlay.
    CHECK_EQ(w.materialAt(200, 200, topVz),
             w.generated().materialAt(200, 200, topVz));
    CHECK_EQ(w.log().size(), size_t(2));
}

VXC_TEST(world_brickAt_prefers_overlay) {
    SyntheticTileSampler tiles(kSeed);
    World<16> w(kSeed, tiles);
    const BrickKey key = ChunkMap<16>::keyForVoxel(5, 5, 0);
    const Brick<16> before = w.brickAt(key);
    w.setVoxel(5, 5, 0, MAT_SNOW);
    const Brick<16> after = w.brickAt(key);
    CHECK(!(before == after));
    CHECK_EQ(after.get(5, 5, 0), MAT_SNOW);
    CHECK_EQ(w.editedBricks().size(), size_t(1));
}

VXC_TEST(world_edit_then_revert_still_logged) {
    // Reverting a voxel to its generated value is still an edit (append-only
    // log never forgets); the overlay brick may collapse but stays present.
    SyntheticTileSampler tiles(kSeed);
    World<16> w(kSeed, tiles);
    const MaterialId orig = w.materialAt(3, 3, 3);
    w.setVoxel(3, 3, 3, MAT_SNOW);
    w.setVoxel(3, 3, 3, orig);
    CHECK_EQ(w.materialAt(3, 3, 3), orig);
    CHECK_EQ(w.log().size(), size_t(2));
    CHECK_EQ(w.editedBricks().size(), size_t(1));
}
