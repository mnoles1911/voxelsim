// Voxel mip chain tests (plan §3.2): index mapping, threshold/majority/
// tie-break semantics, homogeneous fast paths, and a determinism golden over
// a 2-level chain built from the amplifier (mirrors test_amplifier.cpp's
// surface-brick lookup pattern).

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "voxelcore/generator.h"
#include "voxelcore/mips.h"
#include "vxctest.h"

using namespace vxc;

namespace {
constexpr uint64_t kSeed = 20260719;

// Verify that setting exactly one child cell solid produces exactly one
// solid parent cell, at the coordinate the index-mapping formula predicts,
// with the expected material. `childIdx`/`lx,ly,lz` are hand-computed (not
// derived from the implementation) for the given parent cell.
void checkIndexMapping(int px, int py, int pz, int childIdx, int lx, int ly, int lz,
                       MaterialId mat) {
    Brick<8> child;
    child.set(lx, ly, lz, mat);

    const Brick<8>* children[8] = {};
    children[childIdx] = &child;

    const Brick<8> parent = downsampleBricks<8>(children, /*solidThreshold=*/1);
    CHECK_EQ(parent.get(px, py, pz), mat);
    CHECK_EQ(parent.solidCount(), size_t(1));
}

} // namespace

VXC_TEST(mips_index_mapping) {
    // Low-half cell, child 0.
    checkIndexMapping(0, 0, 0, /*childIdx=*/0, /*lx,ly,lz=*/0, 0, 0, MAT_ROCK);
    // High-half on all three axes, child 3 = 1 + 2*1 + 4*0.
    checkIndexMapping(7, 5, 3, /*childIdx=*/3, /*lx,ly,lz=*/6, 2, 6, MAT_SAND);
    // Exactly at the half boundary (x=y=z=B/2), child 7 = 1+2+4.
    checkIndexMapping(4, 4, 4, /*childIdx=*/7, /*lx,ly,lz=*/0, 0, 0, MAT_GRAVEL);
    // Just below the half boundary, child 0, local (6,6,6).
    checkIndexMapping(3, 3, 3, /*childIdx=*/0, /*lx,ly,lz=*/6, 6, 6, MAT_SNOW);
}

VXC_TEST(mips_threshold_semantics) {
    // Parent cell (0,0,0) draws from child 0, local block (0..1,0..1,0..1).
    // Exactly 3 of the 8 local cells solid.
    Brick<8> child;
    child.set(0, 0, 0, MAT_ROCK);
    child.set(1, 0, 0, MAT_ROCK);
    child.set(0, 1, 0, MAT_ROCK);
    // Remaining 5 cells in the block stay air.

    const Brick<8>* children[8] = {};
    children[0] = &child;

    const Brick<8> atFour = downsampleBricks<8>(children, /*solidThreshold=*/4);
    CHECK_EQ(atFour.get(0, 0, 0), MAT_AIR);
    CHECK(!atFour.occupied(0, 0, 0));

    const Brick<8> atThree = downsampleBricks<8>(children, /*solidThreshold=*/3);
    CHECK_EQ(atThree.get(0, 0, 0), MAT_ROCK);
    CHECK(atThree.occupied(0, 0, 0));
}

VXC_TEST(mips_majority_tie_break) {
    // 2x MAT_ROCK + 2x MAT_SAND solid (4 of 8) -> tie, lower id (ROCK) wins.
    {
        Brick<8> child;
        child.set(0, 0, 0, MAT_ROCK);
        child.set(1, 0, 0, MAT_ROCK);
        child.set(0, 1, 0, MAT_SAND);
        child.set(1, 1, 0, MAT_SAND);
        const Brick<8>* children[8] = {};
        children[0] = &child;
        const Brick<8> parent = downsampleBricks<8>(children); // default threshold 4
        CHECK_EQ(parent.get(0, 0, 0), MAT_ROCK);
    }
    // 3x MAT_SAND + 2x MAT_ROCK solid (5 of 8) -> clear majority, SAND wins.
    {
        Brick<8> child;
        child.set(0, 0, 0, MAT_SAND);
        child.set(1, 0, 0, MAT_SAND);
        child.set(0, 1, 0, MAT_SAND);
        child.set(1, 1, 0, MAT_ROCK);
        child.set(0, 0, 1, MAT_ROCK);
        const Brick<8>* children[8] = {};
        children[0] = &child;
        const Brick<8> parent = downsampleBricks<8>(children); // default threshold 4
        CHECK_EQ(parent.get(0, 0, 0), MAT_SAND);
    }
}

VXC_TEST(mips_homogeneous_fast_paths) {
    // 8 identical homogeneous solid children -> homogeneous parent, same material.
    {
        const Brick<8> homRock(MAT_ROCK);
        const Brick<8>* children[8] = {&homRock, &homRock, &homRock, &homRock,
                                        &homRock, &homRock, &homRock, &homRock};
        const Brick<8> parent = downsampleBricks<8>(children);
        CHECK(parent.isHomogeneous());
        CHECK_EQ(parent.homogeneousMaterial(), MAT_ROCK);
    }
    // All-null children -> homogeneous air.
    {
        const Brick<8>* children[8] = {};
        const Brick<8> parent = downsampleBricks<8>(children);
        CHECK(parent.isHomogeneous());
        CHECK_EQ(parent.homogeneousMaterial(), MAT_AIR);
    }
}

VXC_TEST(mips_chain_determinism_golden) {
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    GeneratedWorld<8> gen(amp);

    // A small fixed brick region straddling the surface near the origin
    // (same lookup pattern as test_amplifier.cpp's
    // generated_brick_matches_pointwise_queries).
    const auto grid = gen.columns(0, 0);
    int32_t bzMin, bzMax;
    gen.surfaceBrickRange(grid, bzMin, bzMax);

    // Level-0 bricks are generated lazily and cached by the test harness
    // (playing the role of a ChunkMap/World overlay); the mip chain only
    // ever sees them through the source callback.
    std::unordered_map<BrickKey, Brick<8>, BrickKeyHash> level0Cache;
    auto source = [&](const BrickKey& k) -> const Brick<8>* {
        auto it = level0Cache.find(k);
        if (it == level0Cache.end()) it = level0Cache.emplace(k, gen.makeBrick(k)).first;
        return &it->second;
    };
    MipChain<8> chain(source);

    // 2x2x2 block of level-2 keys around the surface straddle -> their 8
    // level-1 children each -> a modest, fully deterministic neighborhood.
    const int32_t z2 = static_cast<int32_t>(floorDiv(bzMin, 4));
    std::vector<BrickKey> level2Keys;
    for (int32_t dz = 0; dz < 2; ++dz)
        for (int32_t dy = 0; dy < 2; ++dy)
            for (int32_t dx = 0; dx < 2; ++dx)
                level2Keys.push_back(BrickKey{dx, dy, z2 + dz});

    std::vector<BrickKey> level1Keys;
    for (const BrickKey& k2 : level2Keys)
        for (int32_t dz = 0; dz < 2; ++dz)
            for (int32_t dy = 0; dy < 2; ++dy)
                for (int32_t dx = 0; dx < 2; ++dx)
                    level1Keys.push_back(
                        BrickKey{k2.x * 2 + dx, k2.y * 2 + dy, k2.z * 2 + dz});

    std::sort(level1Keys.begin(), level1Keys.end(), BrickKeyLess{});
    std::sort(level2Keys.begin(), level2Keys.end(), BrickKeyLess{});

    Digest d;
    for (const BrickKey& k : level1Keys) {
        const Brick<8>* b = chain.brick(1, k);
        CHECK(b != nullptr);
        d.u64(static_cast<uint32_t>(k.x));
        d.u64(static_cast<uint32_t>(k.y));
        d.u64(static_cast<uint32_t>(k.z));
        b->digest(d);
    }
    for (const BrickKey& k : level2Keys) {
        const Brick<8>* b = chain.brick(2, k);
        CHECK(b != nullptr);
        d.u64(static_cast<uint32_t>(k.x));
        d.u64(static_cast<uint32_t>(k.y));
        d.u64(static_cast<uint32_t>(k.z));
        b->digest(d);
    }
    CHECK_EQ(d.h, 0xACC109F9B1A5AD25ull); // GOLDEN(mips_chain)
}
