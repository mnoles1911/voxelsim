// Greedy mesher properties (plan §5 task 4): face-count conservation, correct
// merging, AO behavior, neighbor-aware culling at brick borders.

#include <map>

#include "voxelcore/mesher.h"
#include "voxelcore/hash.h"
#include "vxctest.h"

using namespace vxc;

namespace {

constexpr int B = 8;

// Dense B^3 test fixture with an explicit apron; sampler domain [-1, B].
struct Fixture {
    MaterialId cells[(B + 2) * (B + 2) * (B + 2)] = {};

    MaterialId& at(int x, int y, int z) {
        return cells[(x + 1) + (B + 2) * ((y + 1) + (B + 2) * (z + 1))];
    }
    MaterialId operator()(int x, int y, int z) const {
        return cells[(x + 1) + (B + 2) * ((y + 1) + (B + 2) * (z + 1))];
    }
};

// Brute-force count of visible faces per (axis, dir) for interior cells.
std::map<std::pair<int, int>, int> visibleFaces(const Fixture& f) {
    std::map<std::pair<int, int>, int> counts;
    const int offs[3][2][3] = {{{-1, 0, 0}, {1, 0, 0}},
                               {{0, -1, 0}, {0, 1, 0}},
                               {{0, 0, -1}, {0, 0, 1}}};
    for (int z = 0; z < B; ++z)
        for (int y = 0; y < B; ++y)
            for (int x = 0; x < B; ++x) {
                if (f(x, y, z) == MAT_AIR) continue;
                for (int axis = 0; axis < 3; ++axis)
                    for (int dir = 0; dir < 2; ++dir) {
                        const int* o = offs[axis][dir];
                        if (f(x + o[0], y + o[1], z + o[2]) == MAT_AIR)
                            counts[{axis, dir}]++;
                    }
            }
    return counts;
}

std::vector<Quad> mesh(const Fixture& f) {
    std::vector<Quad> quads;
    meshBrick<B>(f, quads);
    return quads;
}

int totalArea(const std::vector<Quad>& quads) {
    int area = 0;
    for (const Quad& q : quads) area += int(q.w) * q.h;
    return area;
}

} // namespace

VXC_TEST(mesher_empty_brick_emits_nothing) {
    Fixture f;
    CHECK_EQ(mesh(f).size(), size_t(0));
}

VXC_TEST(mesher_single_voxel_six_unit_quads) {
    Fixture f;
    f.at(3, 4, 5) = MAT_ROCK;
    const auto quads = mesh(f);
    CHECK_EQ(quads.size(), size_t(6));
    for (const Quad& q : quads) {
        CHECK_EQ(int(q.w), 1);
        CHECK_EQ(int(q.h), 1);
        CHECK_EQ(q.mat, MAT_ROCK);
        CHECK_EQ(int(q.ao), 0xff); // isolated voxel: all corners open
    }
}

VXC_TEST(mesher_two_adjacent_voxels_merge) {
    Fixture f;
    f.at(2, 2, 2) = MAT_ROCK;
    f.at(3, 2, 2) = MAT_ROCK;
    const auto quads = mesh(f);
    // 6 quads: the shared face pair is culled, the four side pairs merge 2x1.
    CHECK_EQ(quads.size(), size_t(6));
    CHECK_EQ(totalArea(quads), 10);
}

VXC_TEST(mesher_different_materials_do_not_merge) {
    Fixture f;
    f.at(2, 2, 2) = MAT_ROCK;
    f.at(3, 2, 2) = MAT_SAND;
    const auto quads = mesh(f);
    CHECK_EQ(quads.size(), size_t(10));
    for (const Quad& q : quads) {
        CHECK_EQ(int(q.w) * q.h, 1);
    }
}

VXC_TEST(mesher_full_brick_open_apron_six_full_quads) {
    Fixture f;
    for (int z = 0; z < B; ++z)
        for (int y = 0; y < B; ++y)
            for (int x = 0; x < B; ++x) f.at(x, y, z) = MAT_ROCK;
    const auto quads = mesh(f);
    CHECK_EQ(quads.size(), size_t(6));
    for (const Quad& q : quads) {
        CHECK_EQ(int(q.w), B);
        CHECK_EQ(int(q.h), B);
    }
}

VXC_TEST(mesher_solid_apron_culls_border_faces) {
    // Brick fully solid AND surrounded by solid neighbors: nothing visible.
    Fixture f;
    for (int z = -1; z <= B; ++z)
        for (int y = -1; y <= B; ++y)
            for (int x = -1; x <= B; ++x) f.at(x, y, z) = MAT_ROCK;
    CHECK_EQ(mesh(f).size(), size_t(0));
}

VXC_TEST(mesher_area_conservation_random_brick) {
    // Total merged quad area per (axis,dir) must equal the brute-force count
    // of visible faces — greedy merging may never create or drop faces.
    Fixture f;
    for (int z = -1; z <= B; ++z)
        for (int y = -1; y <= B; ++y)
            for (int x = -1; x <= B; ++x) {
                const uint64_t h = hash3(321, x, y, z, 77);
                f.at(x, y, z) = (h % 100 < 45)
                                    ? static_cast<MaterialId>(1 + h / 100 % (kMaterialCount - 1))
                                    : static_cast<MaterialId>(MAT_AIR);
            }
    const auto quads = mesh(f);
    const auto expected = visibleFaces(f);
    std::map<std::pair<int, int>, int> got;
    for (const Quad& q : quads) got[{q.axis, q.positive}] += int(q.w) * q.h;
    for (int axis = 0; axis < 3; ++axis)
        for (int dir = 0; dir < 2; ++dir) {
            const auto k = std::make_pair(axis, dir);
            const int e = expected.count(k) ? expected.at(k) : 0;
            const int g = got.count(k) ? got.at(k) : 0;
            CHECK_EQ(g, e);
        }
}

VXC_TEST(mesher_ao_darkens_inside_corner) {
    // An L: floor slab + wall. Floor cells adjacent to the wall must have
    // darkened AO on the wall-side corners and must NOT merge with open floor
    // cells that have different AO.
    Fixture f;
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y) f.at(x, y, 0) = MAT_ROCK;
    for (int y = 0; y < 4; ++y) f.at(0, y, 1) = MAT_ROCK; // wall along x=0
    const auto quads = mesh(f);
    bool sawDarkened = false, sawOpen = false;
    for (const Quad& q : quads) {
        if (q.axis != 2 || !q.positive || q.slice != 0) continue; // +z floor faces
        if (q.ao != 0xff) sawDarkened = true;
        else sawOpen = true;
    }
    CHECK(sawDarkened);
    CHECK(sawOpen);
}

VXC_TEST(mesher_deterministic_output_order) {
    Fixture f;
    for (int z = -1; z <= B; ++z)
        for (int y = -1; y <= B; ++y)
            for (int x = -1; x <= B; ++x)
                f.at(x, y, z) = (hash3(9, x, y, z, 3) % 3 == 0) ? MAT_TOPSOIL : MAT_AIR;
    const auto a = mesh(f);
    const auto b = mesh(f);
    CHECK(a == b);
    Digest da, db;
    digestQuads(a, da);
    digestQuads(b, db);
    CHECK_EQ(da.h, db.h);
}
