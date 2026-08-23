// Voxel mip chain tests (plan Â§3.2): index mapping, threshold/majority/
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
    // GOLDEN(mips_chain) â€” kWorldGenVersion 6: the coarse-to-fine detail
    // rework. This chain is built from generated bricks, so it moves exactly
    // because amplifier_columns moves; the mip RULE itself (threshold,
    // majority, tie-break) is unchanged and its own tests still pass.
    // (was 0xE827A786195B8A73 at v3/v4/v5, 0xE4CF1B376622A38F at v2)
    CHECK_EQ(d.h, 0x216323B8FF725CC0ull);
}

// --- Backlog 0.0b: surface-preserving downsampling --------------------------
//
// The defect these pin: the majority vote discards thin surface layers. Snow
// and grass caps are 1-3 voxels over an unbounded rock body, so a surface
// group usually hands rock the vote outright, and a 4-4 tie STILL goes to
// rock because the tie-break is lowest id (MAT_ROCK 2 < MAT_SNOW 7 <
// MAT_GRASS 8). Compounded per level, and level is distance, the owner sees
// terrain that gets browner ring by ring. surfacePreserve = true carries the
// TOPMOST solid child instead; these tests pin the pick, the tie case it was
// built for, the fully-solid-group case that justifies topmost-ALWAYS, and
// the guarantee that solidity never moves.

VXC_TEST(mips_surface_preserve_topmost_wins) {
    // Parent cell (0,0,0), child 0, local block (0..1)^3: 4x rock on the
    // bottom layer, ONE snow voxel on top. Vote: rock 4-1. Preserve: snow.
    Brick<8> child;
    child.set(0, 0, 0, MAT_ROCK);
    child.set(1, 0, 0, MAT_ROCK);
    child.set(0, 1, 0, MAT_ROCK);
    child.set(1, 1, 0, MAT_ROCK);
    child.set(0, 0, 1, MAT_SNOW);
    const Brick<8>* children[8] = {};
    children[0] = &child;

    const Brick<8> voted = downsampleBricks<8>(children, 4, /*surfacePreserve=*/false);
    CHECK_EQ(voted.get(0, 0, 0), MAT_ROCK);
    const Brick<8> preserved = downsampleBricks<8>(children, 4, /*surfacePreserve=*/true);
    CHECK_EQ(preserved.get(0, 0, 0), MAT_SNOW);
    // Same occupancy either way -- the mode is colour, never shape.
    CHECK_EQ(voted.solidCount(), preserved.solidCount());
}

VXC_TEST(mips_surface_preserve_beats_tie_break) {
    // THE reported mechanism, exactly: 4 grass over 4 rock is a 4-4 tie, and
    // the deterministic tie-break hands it to rock (2 < 8). Preserve mode
    // must hand it to the grass on top.
    Brick<8> child;
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
            child.set(dx, dy, 0, MAT_ROCK);
            child.set(dx, dy, 1, MAT_GRASS);
        }
    const Brick<8>* children[8] = {};
    children[0] = &child;

    CHECK_EQ(downsampleBricks<8>(children, 4, false).get(0, 0, 0), MAT_ROCK);
    CHECK_EQ(downsampleBricks<8>(children, 4, true).get(0, 0, 0), MAT_GRASS);
}

VXC_TEST(mips_surface_preserve_full_group) {
    // Why topmost-ALWAYS rather than only-when-mixed: a 1-voxel cap whose
    // surface lands exactly on a group's top boundary makes the group FULLY
    // solid (the air starts in the group above), and a mixed-only rule would
    // still hand it to the vote -- losing the cap on roughly half of all
    // columns at every level. Verified here on the fully-solid group; the
    // deterministic within-layer pick (first (dy,dx) ascending at the top dz)
    // rides along via the two-material top layer.
    Brick<8> child;
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx)
            child.set(dx, dy, 0, MAT_ROCK);
    child.set(0, 0, 1, MAT_SNOW);   // (dy=0,dx=0): first in scan order -> the pick
    child.set(1, 0, 1, MAT_GRASS);
    child.set(0, 1, 1, MAT_GRASS);
    child.set(1, 1, 1, MAT_GRASS);
    const Brick<8>* children[8] = {};
    children[0] = &child;

    // Vote: rock 4, grass 3, snow 1 -> rock. Preserve: top layer, first in
    // (dy,dx) order -> snow.
    CHECK_EQ(downsampleBricks<8>(children, 4, false).get(0, 0, 0), MAT_ROCK);
    CHECK_EQ(downsampleBricks<8>(children, 4, true).get(0, 0, 0), MAT_SNOW);
}

VXC_TEST(mips_surface_preserve_solidity_untouched) {
    // 3 solid of 8 stays AIR at the default threshold with the mode on --
    // preserve changes which material a solid cell reports, never whether a
    // cell is solid. (The erosion the threshold causes is real and is a
    // separate, geometry-changing decision; see downsampleBricks' header.)
    Brick<8> child;
    child.set(0, 0, 0, MAT_ROCK);
    child.set(1, 0, 0, MAT_ROCK);
    child.set(0, 0, 1, MAT_SNOW);
    const Brick<8>* children[8] = {};
    children[0] = &child;

    const Brick<8> preserved = downsampleBricks<8>(children, 4, true);
    CHECK_EQ(preserved.get(0, 0, 0), MAT_AIR);
    CHECK(!preserved.occupied(0, 0, 0));
    // And at threshold 3 it becomes solid with the topmost material, exactly
    // as the same threshold change does in vote mode.
    CHECK_EQ(downsampleBricks<8>(children, 3, true).get(0, 0, 0), MAT_SNOW);
}
