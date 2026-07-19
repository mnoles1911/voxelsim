// Property tests for brick storage (plan §5 task 2).

#include "voxelcore/brick.h"
#include "voxelcore/chunkmap.h"
#include "voxelcore/hash.h"
#include "vxctest.h"

using namespace vxc;

namespace {

template <int B>
void roundtripImpl() {
    Brick<B> b;
    // Deterministic pseudo-random write/read-back over every cell.
    for (int z = 0; z < B; ++z)
        for (int y = 0; y < B; ++y)
            for (int x = 0; x < B; ++x) {
                const MaterialId m =
                    static_cast<MaterialId>(hash3(7, x, y, z, 999) % kMaterialCount);
                b.set(x, y, z, m);
            }
    size_t solid = 0;
    for (int z = 0; z < B; ++z)
        for (int y = 0; y < B; ++y)
            for (int x = 0; x < B; ++x) {
                const MaterialId want =
                    static_cast<MaterialId>(hash3(7, x, y, z, 999) % kMaterialCount);
                CHECK_EQ(b.get(x, y, z), want);
                CHECK_EQ(b.occupied(x, y, z), want != MAT_AIR);
                if (want != MAT_AIR) ++solid;
            }
    CHECK_EQ(b.solidCount(), solid);
    CHECK(b.paletteSize() <= kMaterialCount);
    CHECK(!b.tryCollapse()); // mixed content must not collapse
}

} // namespace

VXC_TEST(brick_set_get_roundtrip_8) { roundtripImpl<8>(); }
VXC_TEST(brick_set_get_roundtrip_16) { roundtripImpl<16>(); }

VXC_TEST(brick_default_is_homogeneous_air) {
    Brick<16> b;
    CHECK(b.isHomogeneous());
    CHECK_EQ(b.homogeneousMaterial(), MAT_AIR);
    CHECK(b.empty());
    CHECK_EQ(b.get(3, 4, 5), MAT_AIR);
    // Writing air into an air brick must not expand storage.
    b.set(1, 1, 1, MAT_AIR);
    CHECK(b.isHomogeneous());
}

VXC_TEST(brick_homogeneous_fill_and_collapse) {
    Brick<8> b(MAT_ROCK);
    CHECK(b.isHomogeneous());
    CHECK_EQ(b.solidCount(), size_t(8 * 8 * 8));
    // Punch one voxel out, then restore it: collapses back to homogeneous.
    b.set(0, 0, 0, MAT_AIR);
    CHECK(!b.isHomogeneous());
    CHECK_EQ(b.get(0, 0, 0), MAT_AIR);
    CHECK_EQ(b.solidCount(), size_t(8 * 8 * 8 - 1));
    b.set(0, 0, 0, MAT_ROCK);
    CHECK(b.tryCollapse());
    CHECK(b.isHomogeneous());
    CHECK_EQ(b.homogeneousMaterial(), MAT_ROCK);
    CHECK_EQ(b.solidCount(), size_t(8 * 8 * 8));
}

VXC_TEST(brick_equality_is_representation_independent) {
    Brick<8> collapsed(MAT_SAND);
    Brick<8> dense;
    for (int z = 0; z < 8; ++z)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) dense.set(x, y, z, MAT_SAND);
    CHECK(collapsed == dense);
    dense.set(7, 7, 7, MAT_ROCK);
    CHECK(!(collapsed == dense));
}

VXC_TEST(chunkmap_voxel_to_brick_key) {
    using CM = ChunkMap<16>;
    const BrickKey a = CM::keyForVoxel(0, 0, 0);
    CHECK_EQ(a.x, 0);
    const BrickKey b = CM::keyForVoxel(-1, 16, 31);
    CHECK_EQ(b.x, -1);
    CHECK_EQ(b.y, 1);
    CHECK_EQ(b.z, 1);
    const BrickKey c = CM::keyForVoxel(-16, -17, -1);
    CHECK_EQ(c.x, -1);
    CHECK_EQ(c.y, -2);
    CHECK_EQ(c.z, -1);
}

VXC_TEST(chunkmap_insert_find_erase) {
    ChunkMap<8> m;
    const BrickKey k{1, -2, 3};
    CHECK(m.find(k) == nullptr);
    m.getOrCreate(k).set(0, 0, 0, MAT_ROCK);
    CHECK(m.find(k) != nullptr);
    CHECK_EQ(m.find(k)->get(0, 0, 0), MAT_ROCK);
    CHECK_EQ(m.size(), size_t(1));
    CHECK(m.erase(k));
    CHECK_EQ(m.size(), size_t(0));
}

VXC_TEST(floor_div_mod) {
    CHECK_EQ(floorDiv(7, 8), 0);
    CHECK_EQ(floorDiv(-1, 8), -1);
    CHECK_EQ(floorDiv(-8, 8), -1);
    CHECK_EQ(floorDiv(-9, 8), -2);
    CHECK_EQ(floorMod(-1, 8), 7);
    CHECK_EQ(floorMod(-8, 8), 0);
    CHECK_EQ(floorMod(7, 8), 7);
}
