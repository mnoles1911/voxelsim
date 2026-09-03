// The craft lattice: 25 mm player-authored detail inside promoted 80 cm
// terrain bricks.
//
// The load-bearing invariant is promote_is_projection_identity. Promotion
// expands a terrain brick into 32,768 craft cells and the projection folds them
// back with mips.h's existing 2x rule, twice. If that round trip is not
// BYTE-IDENTICAL then promotion alone changes the world, and every downstream
// claim -- collision, rendering, replication, the save file -- is built on a
// world that moved when nobody edited it.
//
// The refusal tests use CraftLattice's fault injector on purpose. No production
// path removes a craft brick from a promoted terrain brick, so without it the
// missing-brick refusals would be checks that cannot fail.

#include <vector>

#include "voxelcore/craftlattice.h"
#include "voxelcore/craftvolume.h"
#include "vxctest.h"

using namespace vxc;

namespace {

using Lattice = CraftLattice<kMarchBrickEdge>;
using TerrainBrick = Brick<kMarchBrickEdge>;

// A terrain brick with real structure in it: a solid lower half of rock, a
// sand cap one voxel thick, and a single clay voxel so no 2x2x2 group is
// uniform everywhere. Deliberately NOT homogeneous -- a homogeneous brick would
// pass the identity test through the fast path alone and prove nothing about
// the general one.
TerrainBrick mixedSource() {
    TerrainBrick b(MAT_AIR);
    for (int z = 0; z < 4; ++z)
        for (int y = 0; y < kMarchBrickEdge; ++y)
            for (int x = 0; x < kMarchBrickEdge; ++x) b.set(x, y, z, MAT_ROCK);
    for (int y = 0; y < kMarchBrickEdge; ++y)
        for (int x = 0; x < kMarchBrickEdge; ++x) b.set(x, y, 4, MAT_SAND);
    b.set(3, 5, 2, MAT_CLAY);
    b.set(6, 1, 4, MAT_TOPSOIL);
    return b;
}

// Min-corner craft cell of a terrain brick, per axis. A craft chunk key IS the
// terrain brick key, so this is the chunk edge and nothing subtler -- but the
// tests must say it once rather than each open-coding key.x * 32, which is
// exactly where a sign or a factor goes wrong unnoticed.
int64_t craftChunkBaseCell(int32_t brickCoord) {
    return static_cast<int64_t>(brickCoord) * kCraftChunkEdgeCells;
}

bool bricksIdentical(const TerrainBrick& a, const TerrainBrick& b) {
    for (int z = 0; z < kMarchBrickEdge; ++z)
        for (int y = 0; y < kMarchBrickEdge; ++y)
            for (int x = 0; x < kMarchBrickEdge; ++x)
                if (a.get(x, y, z) != b.get(x, y, z)) return false;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Coordinates
// ---------------------------------------------------------------------------

VXC_TEST(craft_chunk_key_is_exactly_the_terrain_brick_key) {
    // The single property the whole design rests on: a craft chunk (32 cells x
    // 25 mm = 80 cm) is one terrain brick (8 voxels x 10 cm = 80 cm). Tested at
    // negative coordinates too, because worlds sit there and floorDiv vs '/'
    // is the classic way this silently stops being true.
    const int64_t probes[] = {0, 1, 7, 31, 32, 33, -1, -7, -31, -32, -33, -1000001};
    for (int64_t c : probes) {
        const int64_t v = voxelOfCraftCell(c);
        const BrickKey viaTerrain = ChunkMap<kMarchBrickEdge>::keyForVoxel(v, v, v);
        const BrickKey viaCraft = craftChunkKeyOfCell(c, c, c);
        CHECK(viaTerrain.x == viaCraft.x);
        CHECK(viaTerrain.y == viaCraft.y);
        CHECK(viaTerrain.z == viaCraft.z);
    }
}

VXC_TEST(craft_cell_of_voxel_round_trips) {
    for (int64_t v = -40; v <= 40; ++v) {
        const int64_t c0 = craftCellOfVoxelMin(v);
        for (int64_t d = 0; d < kCraftCellsPerVoxel; ++d) CHECK(voxelOfCraftCell(c0 + d) == v);
    }
}

// ---------------------------------------------------------------------------
// THE load-bearing test
// ---------------------------------------------------------------------------

VXC_TEST(promote_is_projection_identity) {
    const TerrainBrick source = mixedSource();
    // Negative key on purpose: promotion, the craft-brick base and both folds
    // all do coordinate arithmetic that truncation would break.
    const BrickKey key{-13, 7, -2};

    Lattice lat;
    CHECK(lat.promote(key, source));
    CHECK(lat.isPromoted(key));
    CHECK(lat.promotedCount() == 1);
    CHECK(lat.craftBrickCount() == size_t(kCraftBricksPerChunk));

    TerrainBrick projected;
    CHECK(lat.project(key, projected));
    CHECK(bricksIdentical(source, projected));
    CHECK(lat.counters.projectRefusedMissingBrick.load() == 0);
}

VXC_TEST(promote_is_projection_identity_for_a_homogeneous_brick) {
    // Both sub-cases of the fast path, since Brick(fill) sets occupancy only
    // when fill != MAT_AIR and the two go down different lines.
    for (MaterialId fill : {MaterialId(MAT_AIR), MaterialId(MAT_ROCK)}) {
        Lattice lat;
        const TerrainBrick source(fill);
        const BrickKey key{2, -5, 9};
        CHECK(lat.promote(key, source));
        TerrainBrick projected;
        CHECK(lat.project(key, projected));
        CHECK(bricksIdentical(source, projected));
    }
}

// ---------------------------------------------------------------------------
// The projection actually moves -- i.e. the identity test above is not passing
// because nothing is connected
// ---------------------------------------------------------------------------

VXC_TEST(carving_a_whole_voxel_of_craft_cells_projects_that_voxel_to_air) {
    Lattice lat;
    const BrickKey key{0, 0, 0};
    CHECK(lat.promote(key, TerrainBrick(MAT_ROCK)));

    // Terrain voxel (2,3,1) of this brick -> its 4^3 craft cells.
    const int64_t vx = 2, vy = 3, vz = 1;
    const int64_t c0x = craftCellOfVoxelMin(vx), c0y = craftCellOfVoxelMin(vy),
                  c0z = craftCellOfVoxelMin(vz);
    for (int64_t dz = 0; dz < kCraftCellsPerVoxel; ++dz)
        for (int64_t dy = 0; dy < kCraftCellsPerVoxel; ++dy)
            for (int64_t dx = 0; dx < kCraftCellsPerVoxel; ++dx)
                CHECK(lat.setCell(c0x + dx, c0y + dy, c0z + dz, MAT_AIR));

    TerrainBrick projected;
    CHECK(lat.project(key, projected));
    CHECK(projected.get(int(vx), int(vy), int(vz)) == MAT_AIR);
    // ...and nothing else moved.
    for (int z = 0; z < kMarchBrickEdge; ++z)
        for (int y = 0; y < kMarchBrickEdge; ++y)
            for (int x = 0; x < kMarchBrickEdge; ++x)
                if (!(x == vx && y == vy && z == vz)) CHECK(projected.get(x, y, z) == MAT_ROCK);
}

VXC_TEST(a_sub_voxel_carve_is_invisible_to_the_projection_and_that_is_the_point) {
    // Removing one 25 mm cell out of 64 leaves the 10 cm voxel solid: solidity
    // is a >= 4-of-8 majority at each fold. This is what lets everything that
    // reads the coarse world -- pathfinding, water, standing on the ground --
    // keep working while the marcher draws the notch.
    Lattice lat;
    const BrickKey key{0, 0, 0};
    CHECK(lat.promote(key, TerrainBrick(MAT_ROCK)));
    CHECK(lat.setCell(craftCellOfVoxelMin(2), craftCellOfVoxelMin(3), craftCellOfVoxelMin(1),
                      MAT_AIR));

    TerrainBrick projected;
    CHECK(lat.project(key, projected));
    CHECK(projected.get(2, 3, 1) == MAT_ROCK);
    // But the craft lattice really did change -- otherwise this test proves
    // only that setCell did nothing.
    CHECK(lat.materialAt(craftCellOfVoxelMin(2), craftCellOfVoxelMin(3),
                         craftCellOfVoxelMin(1)) == MAT_AIR);
    CHECK(lat.counters.cellsWritten.load() == 1);
}

// ---------------------------------------------------------------------------
// Refusals -- each one proved able to fire
// ---------------------------------------------------------------------------

VXC_TEST(promote_is_idempotent_and_says_so) {
    Lattice lat;
    const BrickKey key{4, 4, 4};
    const int64_t bx = craftChunkBaseCell(key.x), by = craftChunkBaseCell(key.y),
                  bz = craftChunkBaseCell(key.z);
    CHECK(lat.promote(key, TerrainBrick(MAT_ROCK)));
    // A second promote must NOT re-flatten a carved brick.
    CHECK(lat.setCell(bx, by, bz, MAT_AIR));
    CHECK(!lat.promote(key, TerrainBrick(MAT_ROCK)));
    CHECK(lat.counters.promoteRejectedAlready.load() == 1);
    CHECK(lat.materialAt(bx, by, bz) == MAT_AIR);
}

VXC_TEST(setCell_refuses_outside_a_promoted_brick) {
    Lattice lat;
    CHECK(!lat.setCell(5, 5, 5, MAT_ROCK));
    CHECK(lat.counters.cellsRejectedNotPromoted.load() == 1);
    CHECK(lat.counters.cellsWritten.load() == 0);
}

VXC_TEST(project_refuses_an_unpromoted_brick) {
    Lattice lat;
    TerrainBrick out;
    CHECK(!lat.project(BrickKey{1, 2, 3}, out));
    CHECK(lat.counters.projectRefusedNotPromoted.load() == 1);
}

VXC_TEST(project_refuses_a_missing_craft_brick_rather_than_reading_it_as_air) {
    // THE FAULT-INJECTION ARM. Absence reading as air is how this project has
    // deleted terrain before, so the refusal must be shown to fire rather than
    // assumed. Without the injector this check could never go red.
    Lattice lat;
    const BrickKey key{0, 0, 0};
    CHECK(lat.promote(key, TerrainBrick(MAT_ROCK)));

    TerrainBrick before;
    CHECK(lat.project(key, before));

    const BrickKey base = craftBrickBaseOfTerrainBrick(key);
    CHECK(lat.forceEraseCraftBrickForFaultInjection(BrickKey{base.x + 1, base.y + 2, base.z + 3}));

    TerrainBrick after = before;
    CHECK(!lat.project(key, after));
    CHECK(lat.counters.projectRefusedMissingBrick.load() == 1);
    // And it did not write a partial answer into the out param.
    CHECK(bricksIdentical(after, before));
}

// ---------------------------------------------------------------------------
// Digest
// ---------------------------------------------------------------------------

VXC_TEST(digest_moves_on_a_sub_voxel_carve_the_projection_cannot_see) {
    // The projection deliberately hides a 25 mm notch (see above). The digest
    // must NOT, or two peers could disagree about carved geometry while their
    // world digests agree -- a desync that renders and never reports.
    Lattice a, b;
    const BrickKey key{0, 0, 0};
    CHECK(a.promote(key, TerrainBrick(MAT_ROCK)));
    CHECK(b.promote(key, TerrainBrick(MAT_ROCK)));
    CHECK(a.digest() == b.digest());

    CHECK(b.setCell(0, 0, 0, MAT_AIR));
    CHECK(a.digest() != b.digest());
}

VXC_TEST(digest_is_independent_of_promotion_order) {
    const BrickKey k1{-3, 0, 5}, k2{9, -9, 0};
    Lattice a, b;
    CHECK(a.promote(k1, TerrainBrick(MAT_ROCK)));
    CHECK(a.promote(k2, TerrainBrick(MAT_SAND)));
    CHECK(b.promote(k2, TerrainBrick(MAT_SAND)));
    CHECK(b.promote(k1, TerrainBrick(MAT_ROCK)));
    CHECK(a.digest() == b.digest());
}

// ---------------------------------------------------------------------------
// The producer
// ---------------------------------------------------------------------------

VXC_TEST(produced_pack_decodes_back_to_every_craft_cell) {
    // The strong gate: not "it produced something" but "all 32,768 cells come
    // back out of the canonical format exactly as they went in".
    Lattice lat;
    const BrickKey key{-2, 3, -7};
    CHECK(lat.promote(key, mixedSource()));
    const int64_t baseX = craftChunkBaseCell(key.x);
    const int64_t baseY = craftChunkBaseCell(key.y);
    const int64_t baseZ = craftChunkBaseCell(key.z);

    // Carve a recognisable shape so the pack is genuinely mixed rather than a
    // field of collapsed uniform bricks.
    for (int64_t i = 0; i < 20; ++i)
        CHECK(lat.setCell(baseX + i, baseY + 5, baseZ + 3, MAT_AIR));

    CraftProducerCounters counters;
    const CraftChunkResult r = produceCraftChunk(lat, key, counters);
    CHECK(r.produced);
    CHECK(counters.chunksAttempted.load() == 1);
    CHECK(counters.chunksProduced.load() == 1);

    int mismatches = 0;
    for (int32_t z = 0; z < kCraftChunkEdgeCells; ++z)
        for (int32_t y = 0; y < kCraftChunkEdgeCells; ++y)
            for (int32_t x = 0; x < kCraftChunkEdgeCells; ++x) {
                const MaterialId want = lat.materialAt(baseX + x, baseY + y, baseZ + z);
                const MaterialId got = decodeChunkVoxelCanonical(r.pack, x, y, z);
                if (want != got) ++mismatches;
            }
    CHECK(mismatches == 0);
}

VXC_TEST(a_hollow_promoted_brick_is_still_PRODUCED_and_the_counters_say_which) {
    // The divergence from cover's C1, and the reason it exists: an all-air
    // craft chunk must still be stored, because the marcher's supersede bit
    // means "a craft chunk is resident here". Drop it and the carved-away rock
    // comes back.
    Lattice lat;
    const BrickKey key{0, 0, 0};
    CHECK(lat.promote(key, TerrainBrick(MAT_ROCK)));
    for (int64_t z = 0; z < kCraftChunkEdgeCells; ++z)
        for (int64_t y = 0; y < kCraftChunkEdgeCells; ++y)
            for (int64_t x = 0; x < kCraftChunkEdgeCells; ++x) CHECK(lat.setCell(x, y, z, MAT_AIR));

    CraftProducerCounters counters;
    const CraftChunkResult r = produceCraftChunk(lat, key, counters);
    CHECK(r.produced);
    CHECK(!r.anySolid);
    CHECK(counters.chunksProduced.load() == 1);
    // "produced but hollow" and "did not run" must not be the same reading.
    CHECK(counters.chunksWithSolid.load() == 0);
    CHECK(counters.ran());
}

VXC_TEST(producer_counters_separate_did_not_run_from_found_nothing) {
    CraftProducerCounters counters;
    CHECK(!counters.ran());

    Lattice lat;
    const CraftChunkResult r = produceCraftChunk(lat, BrickKey{1, 1, 1}, counters);
    CHECK(!r.produced);
    CHECK(counters.ran()); // it DID run; it refused.
    CHECK(counters.refusedNotPromoted.load() == 1);
    CHECK(counters.chunksProduced.load() == 0);
}

VXC_TEST(producer_refuses_a_missing_craft_brick_rather_than_packing_air) {
    // The second fault-injection arm. This is the refusal that would otherwise
    // render as deleted terrain with every counter healthy.
    Lattice lat;
    const BrickKey key{0, 0, 0};
    CHECK(lat.promote(key, TerrainBrick(MAT_ROCK)));
    const BrickKey base = craftBrickBaseOfTerrainBrick(key);
    CHECK(lat.forceEraseCraftBrickForFaultInjection(BrickKey{base.x, base.y, base.z + 1}));

    CraftProducerCounters counters;
    const CraftChunkResult r = produceCraftChunk(lat, key, counters);
    CHECK(!r.produced);
    CHECK(counters.refusedMissingBrick.load() == 1);
    CHECK(counters.chunksProduced.load() == 0);
}

VXC_TEST(an_untouched_promoted_brick_costs_only_descriptors) {
    // The plan's cost claim, as an assertion rather than as arithmetic in a
    // document: a promoted-but-uncarved brick collapses to 64 uniform-SOLID
    // descriptors and carries no occupancy or material payload at all.
    Lattice lat;
    const BrickKey key{0, 0, 0};
    CHECK(lat.promote(key, TerrainBrick(MAT_ROCK)));

    CraftProducerCounters counters;
    const CraftChunkResult r = produceCraftChunk(lat, key, counters);
    CHECK(r.produced);
    CHECK(r.pack.occ.empty());
    CHECK(r.pack.mat.empty());
    CHECK(r.residentBytes() == int64_t(kMarchChunkBricks) * 8);
    CHECK(r.pack.allSolid);
}
