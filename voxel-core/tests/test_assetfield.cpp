// The asset term composed into the world function.
//
// WHAT THIS FILE IS ACTUALLY GUARDING. docs/asset-streaming-design.md said to
// wire the asset sample into the UE mesher's sampler. That is necessary and not
// sufficient, and the two defects it leaves are both silent:
//
//   1. An edit anywhere in an asset-bearing brick regenerates that brick from
//      GeneratedWorld::makeBrick (world.h:125-127) -- so if makeBrick has no
//      asset term, digging one voxel of a tree deletes the rest of the tree in
//      that 8^3 region, persistently and replicated.
//   2. World::materialAt feeds raycasts, digging, the region graph and
//      collapse. An asset present only in the mesher is a tree you can see and
//      walk through.
//
// Both are tested here, by doing the thing and looking at the result: the first
// by applying a real edit through World and re-reading the brick, the second by
// comparing the per-voxel path against the brick path voxel for voxel.

#include "voxelcore/assetfield.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "voxelcore/world.h"

#include "vxctest.h"

using namespace vxc;

namespace {

// A minimal VXA v3 blob, so the composition is tested against a grid with
// KNOWN voxels rather than against whatever a fixture happens to contain.
// Deliberately the same encoder shape test_assetgrid.cpp uses for its synthetic
// cases; the fixture tests there are what validate the reader itself.
void put32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 24));
}

// A solid nx*ny*nz block of `mat`, anchored so its base sits on the anchor
// voxel: origin (0,0,0) means local (0,0,0) is the anchor itself.
std::vector<uint8_t> solidBlock(int32_t nx, int32_t ny, int32_t nz, MaterialId mat,
                                uint32_t voxelSizeMm, int32_t ox = 0, int32_t oy = 0,
                                int32_t oz = 0) {
    std::vector<uint8_t> b;
    put32(b, kVxaMagic);
    put32(b, kVxaVersion);
    put32(b, uint32_t(ox));
    put32(b, uint32_t(oy));
    put32(b, uint32_t(oz));
    put32(b, uint32_t(nx));
    put32(b, uint32_t(ny));
    put32(b, uint32_t(nz));
    put32(b, voxelSizeMm);  // v2 -- BEFORE the run count, per forge/vxa.py
    put32(b, 1u);           // runCount
    put32(b, 0u);           // partRunCount (v3)
    put32(b, 0u);           // jointCount (v3)
    // one run covering the whole box
    b.push_back(uint8_t(mat));
    put32(b, uint32_t(nx * ny * nz));
    return b;
}

// A bank source over one grid, for every (bankId, seedIndex).
class OneGridBank : public IAssetBankSource {
public:
    explicit OneGridBank(const AssetGrid* g) : g_(g) {}
    const AssetGrid* bankGrid(uint16_t, uint16_t) const override { return g_; }

private:
    const AssetGrid* g_;
};

// A bank source that has nothing, to check the "failed to load" path leaves the
// world it was going to stand in intact.
class EmptyBank : public IAssetBankSource {
public:
    const AssetGrid* bankGrid(uint16_t, uint16_t) const override { return nullptr; }
};

AssetSpecies pillarSpecies(uint8_t layer, int32_t heightMm, uint32_t voxelSizeMm) {
    AssetSpecies s;
    s.bankId = 1;
    s.layer = layer;
    for (int b = 0; b < kBiomeCount; ++b) s.weightPerMille[b] = 1000;
    s.elevMinMm = -1'000'000;
    s.elevMaxMm = 9'000'000;
    s.slopeMaxMmPerM = 100000;
    s.heightMm = heightMm;
    s.voxelSizeMm = voxelSizeMm;
    return s;
}

AssetColumnFacts flatGround(int32_t surfaceMm) {
    AssetColumnFacts c;
    c.known = true;
    c.anchorSolid = true;
    c.biome = TEMPERATE_FOREST;
    c.surfaceMm = surfaceMm;
    c.slopeMmPerM = 0;
    return c;
}

// A layer with a site in every cell, so nothing in these tests turns on a hash
// draw. test_assetplacement.cpp records what a hash-dependent test costs.
AssetLayer everyCell(int32_t cellMm, int32_t heightMm, int32_t radiusMm, bool terrain = true) {
    AssetLayer L{};
    L.cellMm = cellMm;
    L.maxHeightMm = heightMm;
    L.maxDepthMm = 0;
    L.maxRadiusMm = radiusMm;
    L.densityPerMille = 1000;
    L.seedCount = 1;
    L.terrainLattice = terrain;
    return L;
}

constexpr uint64_t kSeed = 0xa55e7f1e1d0000abull;

} // namespace

VXC_TEST(assetfield_stamps_a_species_at_the_voxel_the_anchor_names) {
    // A 1x1x30 pillar of bark, so "where did it land" has an unambiguous answer
    // at every voxel of its height.
    AssetGrid g;
    CHECK_EQ(int(g.parse(solidBlock(1, 1, 30, MAT_BARK, uint32_t(kVoxelSizeMm)))),
             int(AssetParseError::kOk));
    CHECK(g.onTerrainLattice());
    OneGridBank bank(&g);

    const AssetLayer layers[1] = {everyCell(3200, 3000, 0)};
    const AssetSpecies sp[1] = {pillarSpecies(0, 3000, uint32_t(kVoxelSizeMm))};

    AssetField field;
    field.setSeed(kSeed);
    field.setLayers(layers, 1);
    field.setSpecies(sp, 1);
    field.setBankSource(&bank);
    CHECK(!field.empty());

    const int32_t surfaceMm = 100'000;
    const std::vector<AssetInstance> insts = field.instancesForRect(
        AssetVoxelRect{0, 0, 31, 31}, [&](int64_t, int64_t) { return flatGround(surfaceMm); });
    CHECK(!insts.empty());

    const AssetInstance& inst = insts[0];
    const int64_t avx = floorDiv(inst.anchorXMm, int64_t(kVoxelSizeMm));
    const int64_t avy = floorDiv(inst.anchorYMm, int64_t(kVoxelSizeMm));
    CHECK_EQ(inst.anchorVz, topSolidVoxelZ(surfaceMm));

    // The pillar occupies exactly [anchorVz, anchorVz + 30) at its own column,
    // and nothing above, below or beside it.
    for (int64_t dz = -3; dz < 34; ++dz) {
        const MaterialId m = field.materialAt(insts, avx, avy, inst.anchorVz + dz);
        const bool inside = dz >= 0 && dz < 30;
        CHECK_EQ(int(m), inside ? int(MAT_BARK) : int(MAT_AIR));
    }
    CHECK_EQ(int(field.materialAt(insts, avx + 1, avy, inst.anchorVz)), int(MAT_AIR));
    CHECK_EQ(int(field.materialAt(insts, avx, avy + 1, inst.anchorVz)), int(MAT_AIR));
}

VXC_TEST(assetfield_refuses_a_detail_lattice_grid_in_the_world_grid) {
    // A 5 cm asset stamped into a 10 cm world comes out at twice its intended
    // size, and there are no boulder-shaped diagnostics (assetgrid.h:56-62). It
    // must answer air, not a doubled boulder -- checked here as the last line
    // of defence; assetSpeciesFits refuses it at load as the first.
    AssetGrid detailGrid;
    CHECK_EQ(int(detailGrid.parse(solidBlock(2, 2, 10, MAT_BARK, 50u))),
             int(AssetParseError::kOk));
    CHECK(!detailGrid.onTerrainLattice());
    OneGridBank bank(&detailGrid);

    // Filed on a TERRAIN layer, which is the mistake being guarded.
    const AssetLayer layers[1] = {everyCell(3200, 3000, 0, /*terrain=*/true)};
    const AssetSpecies sp[1] = {pillarSpecies(0, 3000, 50u)};

    AssetField field;
    field.setSeed(kSeed);
    field.setLayers(layers, 1);
    field.setSpecies(sp, 1);
    field.setBankSource(&bank);

    const std::vector<AssetInstance> insts = field.instancesForRect(
        AssetVoxelRect{0, 0, 31, 31}, [&](int64_t, int64_t) { return flatGround(100'000); });
    CHECK(!insts.empty()); // the policy still resolves it...
    for (const AssetInstance& i : insts) {
        const int64_t avx = floorDiv(i.anchorXMm, int64_t(kVoxelSizeMm));
        const int64_t avy = floorDiv(i.anchorYMm, int64_t(kVoxelSizeMm));
        // ...and it stamps nothing.
        CHECK_EQ(int(field.materialOfInstance(i, avx, avy, i.anchorVz)), int(MAT_AIR));
    }
    // And the load-time check names it, which is the half a player could act on.
    CHECK_EQ(int(assetSpeciesFits(sp[0], layers, 1, 3200)), int(AssetTableError::kWrongLattice));
}

VXC_TEST(assetfield_a_detail_lattice_LAYER_never_enters_the_world_grid) {
    // The other half of the same rule: ground cover has sites, and none of them
    // are world voxels. If this ever started stamping, ground cover would be in
    // the terrain grid where the bound never accounted for it -- solid above the
    // surface with no bound widening, which is a hole in the world.
    AssetGrid g;
    CHECK_EQ(int(g.parse(solidBlock(1, 1, 4, MAT_GRASS, 50u))), int(AssetParseError::kOk));
    OneGridBank bank(&g);

    const AssetLayer layers[1] = {everyCell(800, 500, 0, /*terrain=*/false)};
    const AssetSpecies sp[1] = {pillarSpecies(0, 400, 50u)};

    AssetField field;
    field.setSeed(kSeed);
    field.setLayers(layers, 1);
    field.setSpecies(sp, 1);
    field.setBankSource(&bank);

    const std::vector<AssetInstance> insts = field.instancesForRect(
        AssetVoxelRect{0, 0, 31, 31}, [&](int64_t, int64_t) { return flatGround(100'000); });
    CHECK(!insts.empty());
    for (const AssetInstance& i : insts)
        for (int64_t dz = 0; dz < 6; ++dz)
            CHECK_EQ(int(field.materialOfInstance(i, floorDiv(i.anchorXMm, int64_t(kVoxelSizeMm)),
                                                  floorDiv(i.anchorYMm, int64_t(kVoxelSizeMm)),
                                                  i.anchorVz + dz)),
                     int(MAT_AIR));
    // ...and the bound agrees it is not there.
    CHECK_EQ(assetTopAboveSurfaceMm(kSeed, layers, 1, AssetVoxelRect{0, 0, 31, 31}), 0);
}

VXC_TEST(assetfield_a_missing_bank_leaves_the_world_intact) {
    // A bank that failed to load must not take the terrain with it. Answering
    // air is what makes a load failure a missing tree rather than a missing
    // hillside.
    EmptyBank none;
    const AssetLayer layers[1] = {everyCell(3200, 3000, 0)};
    const AssetSpecies sp[1] = {pillarSpecies(0, 3000, uint32_t(kVoxelSizeMm))};
    AssetField field;
    field.setSeed(kSeed);
    field.setLayers(layers, 1);
    field.setSpecies(sp, 1);
    field.setBankSource(&none);
    const std::vector<AssetInstance> insts = field.instancesForRect(
        AssetVoxelRect{0, 0, 31, 31}, [&](int64_t, int64_t) { return flatGround(100'000); });
    for (const AssetInstance& i : insts)
        CHECK_EQ(int(field.materialOfInstance(i, 0, 0, i.anchorVz)), int(MAT_AIR));
}

VXC_TEST(assetfield_an_unresolvable_column_places_nothing) {
    // The anti-float guard, all the way through the field. A column the caller
    // could not evaluate produces no instances at all -- not instances at z = 0.
    AssetGrid g;
    CHECK_EQ(int(g.parse(solidBlock(1, 1, 20, MAT_BARK, uint32_t(kVoxelSizeMm)))),
             int(AssetParseError::kOk));
    OneGridBank bank(&g);
    const AssetLayer layers[1] = {everyCell(3200, 3000, 0)};
    const AssetSpecies sp[1] = {pillarSpecies(0, 2000, uint32_t(kVoxelSizeMm))};
    AssetField field;
    field.setSeed(kSeed);
    field.setLayers(layers, 1);
    field.setSpecies(sp, 1);
    field.setBankSource(&bank);

    CHECK(field.instancesForRect(AssetVoxelRect{0, 0, 31, 31}, [](int64_t, int64_t) {
                 return AssetColumnFacts{}; // known == false
             })
              .empty());

    // ... and a column whose anchor voxel is air (a cave mouth under the tree).
    CHECK(field.instancesForRect(AssetVoxelRect{0, 0, 31, 31}, [](int64_t, int64_t) {
                 AssetColumnFacts c = flatGround(100'000);
                 c.anchorSolid = false;
                 return c;
             })
              .empty());
}

// ---------------------------------------------------------------------------
// THE TWO DEFECTS THE UE-ONLY WIRING WOULD HAVE LEFT
// ---------------------------------------------------------------------------

namespace {

// A world over a synthetic tile source, with an asset field installed. Kept in
// one place so the two tests below differ only in what they ask of it.
struct AssetWorld {
    AssetGrid grid;
    OneGridBank bank{&grid};
    AssetLayer layers[1];
    AssetSpecies sp[1];
    AssetField field;

    // THE BRICK RANGE THE TESTS MUST SCAN, and it cannot be a constant.
    //
    // These tests run against a real Amplifier over a SyntheticTileSampler, so
    // the ground is wherever that sampler puts it -- which is not near z = 0 and
    // is not the same at every (x, y). A hard-coded band of bricks around the
    // origin finds bare sky, every assertion about asset voxels passes
    // vacuously, and the test reports success while proving nothing. That is
    // the precise failure this whole file exists to catch, so the range is
    // DERIVED from the surface instead.
    static void brickRangeForFootprint(const Amplifier& amp, int32_t bx, int32_t by,
                                       int32_t& bz0, int32_t& bz1) {
        GeneratedWorld<8> probe(amp); // no asset field: terrain only
        int32_t lo = 0, hi = 0;
        probe.surfaceBrickRange(probe.columns(bx, by), lo, hi);
        bz0 = lo - 1;      // a brick of clear ground below...
        bz1 = hi + 6;      // ...and enough sky above for a 4 m asset
    }

    AssetWorld() {
        // A wide, tall block so that whichever brick the tests look at is
        // inside it: 6 x 6 x 40 voxels of bark, centred on its anchor.
        grid.parse(solidBlock(6, 6, 40, MAT_BARK, uint32_t(kVoxelSizeMm), -3, -3, 0));
        // CELL PITCH AT MOST ONE BRICK, and this is the setup detail that made
        // the first cut of these tests measure nothing. A brick is 8 voxels
        // (0.8 m); at a 32-voxel cell there is one site per 3.2 m and its reach
        // is 0.3 m, so the overwhelming majority of bricks contain no asset at
        // all and every assertion below passes vacuously. The vacuous-truth
        // guards (assetVoxels > 0, added > 0) caught it, which is what they are
        // for.
        layers[0] = everyCell(800, 4000, 300);
        sp[0] = pillarSpecies(0, 4000, uint32_t(kVoxelSizeMm));
        field.setSeed(kSeed);
        field.setLayers(layers, 1);
        field.setSpecies(sp, 1);
        field.setBankSource(&bank);
    }
};

} // namespace

VXC_TEST(assetfield_the_per_voxel_path_and_the_brick_path_agree_voxel_for_voxel) {
    // DEFECT 2: "a tree that renders but is not there". The mesher reads bricks;
    // raycasts, digging, the region graph and collapse read materialAt. If the
    // two disagree, the tree is visible and intangible -- and nothing compares
    // them, so nothing would say so.
    AssetWorld aw;
    SyntheticTileSampler tiles(1234);
    Amplifier amp(1234, tiles);
    GeneratedWorld<8> gen(amp);
    gen.setAssetField(&aw.field);

    int bz0 = 0, bz1 = 0;
    AssetWorld::brickRangeForFootprint(amp, 0, 0, bz0, bz1);
    int assetVoxels = 0, compared = 0;
    for (int32_t bz = bz0; bz <= bz1; ++bz) {
        const BrickKey key{0, 0, bz};
        const Brick<8> brick = gen.makeBrick(key);
        for (int z = 0; z < 8; ++z)
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const int64_t vx = int64_t(key.x) * 8 + x, vy = int64_t(key.y) * 8 + y,
                                  vz = int64_t(key.z) * 8 + z;
                    const MaterialId fromBrick = brick.get(x, y, z);
                    const MaterialId fromVoxel = gen.materialAt(vx, vy, vz);
                    CHECK_EQ(int(fromBrick), int(fromVoxel));
                    if (fromBrick == MAT_BARK) ++assetVoxels;
                    ++compared;
                }
    }
    CHECK(compared > 3000);
    // The vacuous-truth guard: the sweep must actually have found asset voxels,
    // or "the two paths agree" is a statement about bare terrain.
    CHECK(assetVoxels > 0);
    std::printf("    brick/voxel agreement over %d voxels, %d of them asset\n", compared,
                assetVoxels);
}

VXC_TEST(assetfield_digging_one_voxel_does_not_delete_the_rest_of_the_brick_s_tree) {
    // DEFECT 1, and it is the reason the composition is in voxel-core rather
    // than in the UE sampler. World::applyToOverlay materialises the overlay
    // brick with gen_.makeBrick(key) and then applies the edit cells
    // (world.h:125-127). With the asset term only in the mesher, the first edit
    // ANYWHERE in an asset-bearing brick regenerates that 8^3 region as bare
    // terrain -- the tree's voxels inside it vanish, silently, into the
    // persisted edit log and out to every client.
    AssetWorld aw;
    SyntheticTileSampler tiles(1234);
    World<8> w(1234, tiles, "test");
    w.setAssetField(&aw.field);

    // Find a brick that genuinely contains asset voxels, and a voxel of the
    // tree inside it. A test that dug a hole in bare ground would pass with the
    // defect present.
    int64_t treeVx = 0, treeVy = 0, treeVz = 0;
    BrickKey key{0, 0, 0};
    int bz0 = 0, bz1 = 0;
    AssetWorld::brickRangeForFootprint(w.amplifier(), 0, 0, bz0, bz1);
    int found = 0;
    for (int32_t bz = bz0; bz <= bz1 && found < 2; ++bz) {
        const BrickKey k{0, 0, bz};
        const Brick<8> b = w.generated().makeBrick(k);
        for (int z = 0; z < 8 && found < 2; ++z)
            for (int y = 0; y < 8 && found < 2; ++y)
                for (int x = 0; x < 8 && found < 2; ++x)
                    if (b.get(x, y, z) == MAT_BARK) {
                        if (found == 0) {
                            key = k;
                            treeVx = int64_t(k.x) * 8 + x;
                            treeVy = int64_t(k.y) * 8 + y;
                            treeVz = int64_t(k.z) * 8 + z;
                        }
                        ++found;
                    }
    }
    CHECK(found >= 2); // a brick with at least two tree voxels to lose

    // Count the tree voxels in that brick before the edit.
    const Brick<8> before = w.generated().makeBrick(key);
    int barkBefore = 0;
    for (int z = 0; z < 8; ++z)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                if (before.get(x, y, z) == MAT_BARK) ++barkBefore;
    CHECK(barkBefore >= 2);

    // Dig exactly one voxel of it.
    w.setVoxel(treeVx, treeVy, treeVz, MAT_AIR);
    CHECK_EQ(int(w.materialAt(treeVx, treeVy, treeVz)), int(MAT_AIR));

    // Every OTHER tree voxel in that brick must still be there. With the asset
    // term absent from makeBrick this count would be zero.
    int barkAfter = 0;
    for (int z = 0; z < 8; ++z)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const int64_t vx = int64_t(key.x) * 8 + x, vy = int64_t(key.y) * 8 + y,
                              vz = int64_t(key.z) * 8 + z;
                if (w.materialAt(vx, vy, vz) == MAT_BARK) ++barkAfter;
            }
    std::printf("    bark voxels in the edited brick: %d before, %d after digging one\n",
                barkBefore, barkAfter);
    CHECK_EQ(barkAfter, barkBefore - 1);
}

VXC_TEST(assetfield_never_replaces_terrain_and_a_world_without_one_is_unchanged) {
    // MONOTONE COMPOSITION. assetplacement.h's bound assumes an asset is solid
    // ABOVE the surface; a composition that could also REMOVE solid would break
    // the all-solid floor bound while the sky bound went on looking correct. So
    // an asset writes only where the terrain answered air, and the evidence is
    // that turning the field on never turns a solid voxel into a different one.
    AssetWorld aw;
    SyntheticTileSampler tiles(1234);
    Amplifier amp(1234, tiles);
    GeneratedWorld<8> withField(amp), without(amp);
    withField.setAssetField(&aw.field);
    CHECK(without.assetField() == nullptr);

    int bz0 = 0, bz1 = 0;
    AssetWorld::brickRangeForFootprint(amp, 0, 0, bz0, bz1);
    int added = 0, same = 0;
    for (int32_t bz = bz0; bz <= bz1; ++bz) {
        const BrickKey key{0, 0, bz};
        const Brick<8> a = withField.makeBrick(key);
        const Brick<8> b = without.makeBrick(key);
        for (int z = 0; z < 8; ++z)
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const MaterialId ma = a.get(x, y, z), mb = b.get(x, y, z);
                    if (ma == mb) {
                        ++same;
                        continue;
                    }
                    // The ONLY legal difference: air became an asset material.
                    CHECK_EQ(int(mb), int(MAT_AIR));
                    CHECK(ma != MAT_AIR);
                    ++added;
                }
    }
    CHECK(added > 0);  // the field did something...
    CHECK(same > 0);   // ...and did not do it everywhere
    std::printf("    asset field added %d voxels and changed no existing one (%d unchanged)\n",
                added, same);
}

// ---------------------------------------------------------------------------
// The placement golden: real terrain, a real baked bank, a pinned digest
// ---------------------------------------------------------------------------

namespace {

std::vector<uint8_t> readFixtureBlob(const char* name) {
    std::string path = std::string(VXC_TEST_FIXTURE_DIR) + "/" + name;
    std::vector<uint8_t> bytes;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return bytes;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        bytes.resize(size_t(n));
        const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
        bytes.resize(got);
    }
    std::fclose(f);
    return bytes;
}

} // namespace

VXC_TEST(assetfield_installed_field_moves_the_world_digest_and_the_digest_is_pinned) {
    // THE RAN-FLAG, as a test (docs/asset-placement-architecture.md section 9):
    // installing an asset field with real species data MUST change the world
    // digest, and the changed digest is PINNED -- class 1-2 placement is part
    // of f(seed, x, y, z), so this golden is what a layer-table, manifest or
    // policy change has to consciously re-bless, exactly like the amplifier's
    // own goldens. The bank is the real baked tundra-pine fixture, so the
    // digest covers the whole chain: scatter, policy, anchor, yaw, rotated
    // origin and the run-length decode of a file asset-forge wrote.
    SyntheticTileSampler tiles(20260719);
    Amplifier amp(20260719, tiles);
    GeneratedWorld<8> gen(amp);

    AssetGrid pine;
    CHECK_EQ(int(pine.parse(readFixtureBlob("asset_tundra_pine_0002.vxa"))),
             int(AssetParseError::kOk));
    OneGridBank banks(&pine);

    // The production layer table (forge/manifest.py LAYERS, as carried by the
    // manifest fixture) and one species that tolerates everything, so the
    // golden does not depend on which biome the synthetic climate lands here.
    AssetLayer layers[kAssetLayerCount];
    layers[0] = {24'000, 60'000, 8'000, 15'000, 60, 4, true};
    layers[1] = {5'000, 34'000, 4'000, 12'000, 1000, 4, true};
    layers[2] = {2'200, 7'500, 2'000, 6'000, 1000, 4, true};
    layers[3] = {800, 30'000, 2'000, 2'500, 1000, 4, false};
    AssetSpecies pineRow;
    pineRow.bankId = 0;
    pineRow.layer = 1;
    for (int b = 0; b < kBiomeCount; ++b) pineRow.weightPerMille[b] = 900;
    pineRow.elevMinMm = -10'000;
    pineRow.elevMaxMm = 2'000'000;
    pineRow.slopeMaxMmPerM = 100'000;
    pineRow.clusterQ10 = 768;
    pineRow.heightMm = 8'000;
    pineRow.voxelSizeMm = uint32_t(kVoxelSizeMm);

    AssetField field;
    field.setLayers(layers, kAssetLayerCount);
    field.setSpecies(&pineRow, 1);
    field.setBankSource(&banks);
    field.setSeed(20260719);

    const auto digestRegion = [&](const AssetField* f) -> uint64_t {
        GeneratedWorld<8> g(amp);
        g.setAssetField(f);
        Digest d;
        // +/- 12 bricks = +/- 9.6 m: wide enough that the 5 m canopy lattice
        // has anchors INSIDE the region (a first cut at +/-3.2 m digested
        // only crowns reaching in from outside, and there were none -- the
        // pine's crown radius is 2.4 m).
        for (int32_t by = -12; by <= 12; ++by)
            for (int32_t bx = -12; bx <= 12; ++bx) {
                const auto grid = g.columns(bx, by);
                int32_t bzMin = 0, bzMax = 0;
                g.surfaceBrickRange(grid, bzMin, bzMax);
                // Pine height is 8 m = 80 voxels = 10 bricks of 8; walk the
                // shell plus that headroom so every crown voxel is digested.
                for (int32_t bz = bzMin; bz <= bzMax + 11; ++bz) {
                    const Brick<8> brick = g.makeBrick({bx, by, bz}, grid);
                    d.u32(uint32_t(bx));
                    d.u32(uint32_t(by));
                    d.u32(uint32_t(bz));
                    brick.digest(d);
                }
            }
        return d.h;
    };

    const uint64_t bare = digestRegion(nullptr);
    const uint64_t wooded = digestRegion(&field);
    std::printf("    bare %016llx, wooded %016llx\n", (unsigned long long)bare,
                (unsigned long long)wooded);
    // The ran-flag: an installed field that leaves the digest unchanged means
    // the field is not wired.
    CHECK(bare != wooded);
    // The golden. Moves ONLY on a deliberate worldgen change (kWorldGenVersion
    // bump): layer table, policy maths, scatter channels, or the fixture bake.
    CHECK_EQ((unsigned long long)wooded, 0xF41F8E6A14A3C5A9ull);
}

// --- COVER COMPOSE: the detail lattice, in a volume of its own --------------
//
// WHAT THESE GUARD, and it is a failure shape this project already knows. The
// obvious reference for a cover byte gate is materialAtResolved, and it returns
// MAT_AIR for every detail grid BY DESIGN (assetfield.h:354, "the last line of
// defence"). A gate built on it compares cover against air and PASSES IF THE
// PRODUCER ALSO EMITS NOTHING -- a check that cannot fail. The first test below
// is that check made able to fail: over ONE instance list it asserts the world
// path answers air AND the cover path answers the material. If cover compose is
// ever quietly reduced to a no-op, that test goes red rather than green.
//
// The tolerance here is EXACTLY ZERO and that is not an oversight. Both sides
// are integer voxels with no float anywhere in the path, so unlike a depth or
// timing gate there is no reference noise floor to calibrate against. Nobody
// should add an epsilon here out of habit.

namespace {

// A tuft-sized detail asset: 2x2x6 voxels at 50 mm = 10x10x30 cm.
bool makeTuft(AssetGrid& g) {
    return g.parse(solidBlock(2, 2, 6, MAT_LEAF_BROADLEAF, 50u)) == AssetParseError::kOk;
}

// A detail-lattice field over one grid, sited in every 800 mm cell -- the real
// L3 cell size (asset-forge/forge/manifest.py:591).
struct CoverFixture {
    AssetLayer layers[1];
    AssetSpecies sp[1];
    OneGridBank bank;
    AssetField field;

    explicit CoverFixture(const AssetGrid* g, uint32_t pitchMm = 50u) : bank(g) {
        layers[0] = everyCell(800, 300, 0, /*terrain*/ false);
        sp[0] = pillarSpecies(0, 300, pitchMm);
        field.setSeed(kSeed);
        field.setLayers(layers, 1);
        field.setSpecies(sp, 1);
        field.setBankSource(&bank);
    }
};

} // namespace

VXC_TEST(covercompose_composes_exactly_what_world_compose_refuses) {
    AssetGrid g;
    CHECK(makeTuft(g));
    CHECK(!g.onTerrainLattice());
    CoverFixture fx(&g);
    CHECK(!fx.field.empty());

    const std::vector<AssetInstance> insts = fx.field.instancesForRect(
        AssetVoxelRect{0, 0, 31, 31}, [&](int64_t, int64_t) { return flatGround(100'000); });
    CHECK(!insts.empty());

    // The WORLD path drops every one of them -- this is the "cannot fail"
    // reference, asserted rather than assumed.
    const std::vector<AssetField::ResolvedAssetInstance> world =
        fx.field.resolveForCompose(insts);
    CHECK_EQ(int(world.size()), 0);

    // The COVER path keeps them all.
    const std::vector<AssetField::ResolvedCoverInstance> cover =
        fx.field.resolveForCoverCompose(insts, 50u);
    CHECK_EQ(int(cover.size()), int(insts.size()));

    // And it puts the material somewhere. Sample the first instance's own base
    // voxel: local (0,0,0) of a grid whose origin is (0,0,0).
    const AssetField::ResolvedCoverInstance& r0 = cover[0];
    const int64_t bx = r0.anchorCx + r0.grid->rotatedOriginX(r0.yawQuarter);
    const int64_t by = r0.anchorCy + r0.grid->rotatedOriginY(r0.yawQuarter);
    const int64_t bz = r0.anchorCz + r0.grid->originZ();
    CHECK_EQ(int(AssetField::coverMaterialAtResolved(cover, bx, by, bz)),
             int(MAT_LEAF_BROADLEAF));

    // The same cover voxel through the world composer is air, because a detail
    // grid is not a world-lattice thing. Two composers, two lattices, and the
    // one that must answer does.
    CHECK_EQ(int(AssetField::materialAtResolved(world, bx, by, bz)), int(MAT_AIR));
}

VXC_TEST(covercompose_stands_cover_on_the_anchor_voxel_not_inside_it) {
    // The terrain convention shares the anchor voxel; cover must sit on its TOP
    // surface, or a 5 cm tuft is entirely buried in the 10 cm ground cube
    // (VoxelDetailAssetSubsystem.cpp:538-544).
    AssetGrid g;
    CHECK(makeTuft(g));
    CoverFixture fx(&g);
    const std::vector<AssetInstance> insts = fx.field.instancesForRect(
        AssetVoxelRect{0, 0, 31, 31}, [&](int64_t, int64_t) { return flatGround(100'000); });
    CHECK(!insts.empty());
    const std::vector<AssetField::ResolvedCoverInstance> cover =
        fx.field.resolveForCoverCompose(insts, 50u);
    CHECK(!cover.empty());

    // 100 mm world voxel / 50 mm cover voxel = 2 cover voxels per world voxel.
    for (size_t i = 0; i < cover.size(); ++i) {
        CHECK_EQ(int(cover[i].anchorCz), int((insts[i].anchorVz + 1) * 2));
        CHECK_EQ(int(cover[i].pitchMm), 50);
    }

    // The cover voxel directly BELOW the base is air: nothing was buried.
    const AssetField::ResolvedCoverInstance& r0 = cover[0];
    const int64_t bx = r0.anchorCx + r0.grid->rotatedOriginX(r0.yawQuarter);
    const int64_t by = r0.anchorCy + r0.grid->rotatedOriginY(r0.yawQuarter);
    const int64_t bz = r0.anchorCz + r0.grid->originZ();
    CHECK_EQ(int(AssetField::coverMaterialAtResolved(cover, bx, by, bz - 1)), int(MAT_AIR));
}

VXC_TEST(covercompose_floors_negative_anchors_rather_than_truncating) {
    // THE SIGN BUG, and the census ground is at (-39661, -57292) so this is the
    // common case, not the edge one. C++ floorDiv floors; HLSL '/' truncates
    // toward zero; they disagree on every negative non-multiple. A GPU mirror
    // must therefore receive anchors ALREADY DIVIDED by this function.
    AssetGrid g;
    CHECK(makeTuft(g));
    CoverFixture fx(&g);
    const std::vector<AssetInstance> insts = fx.field.instancesForRect(
        AssetVoxelRect{-400, -400, -369, -369},
        [&](int64_t, int64_t) { return flatGround(100'000); });
    CHECK(!insts.empty());
    const std::vector<AssetField::ResolvedCoverInstance> cover =
        fx.field.resolveForCoverCompose(insts, 50u);
    CHECK_EQ(int(cover.size()), int(insts.size()));

    // CALIBRATE THE TEST BEFORE TRUSTING IT: it only has teeth if at least one
    // instance actually lands where floor and truncation disagree.
    int discriminating = 0;
    for (size_t i = 0; i < cover.size(); ++i) {
        const int64_t mm = insts[i].anchorXMm;
        const int64_t trunc = mm / 50;           // toward zero, what HLSL does
        const int64_t floored = cover[i].anchorCx;
        // The floor property itself, on every instance.
        CHECK(floored * 50 <= mm);
        CHECK((floored + 1) * 50 > mm);
        if (mm < 0 && (mm % 50) != 0) {
            CHECK_EQ(int(floored), int(trunc - 1));
            ++discriminating;
        }
    }
    CHECK(discriminating > 0);
}

VXC_TEST(covercompose_refuses_a_grid_baked_at_another_pitch) {
    // Nothing in voxel-core resamples (asset-forge/README.md:38-41), so a 20 mm
    // bush in a 50 mm volume would come out 2.5x its size. It is dropped, and
    // the volume stays empty rather than wrong.
    AssetGrid g20;
    CHECK_EQ(int(g20.parse(solidBlock(2, 2, 6, MAT_LEAF_BROADLEAF, 20u))),
             int(AssetParseError::kOk));
    CoverFixture fx(&g20, /*species pitch*/ 20u);
    const std::vector<AssetInstance> insts = fx.field.instancesForRect(
        AssetVoxelRect{0, 0, 31, 31}, [&](int64_t, int64_t) { return flatGround(100'000); });
    CHECK(!insts.empty());

    // Its own pitch composes.
    CHECK_EQ(int(fx.field.resolveForCoverCompose(insts, 20u).size()), int(insts.size()));
    // A 50 mm volume refuses it entirely.
    CHECK_EQ(int(fx.field.resolveForCoverCompose(insts, 50u).size()), 0);
}

VXC_TEST(covercompose_refuses_a_pitch_that_does_not_tile_the_world_lattice) {
    // (anchorVz + 1) must land on a cover-voxel boundary. 100/30 does not
    // divide, so the whole volume is refused rather than composed half a voxel
    // low everywhere.
    AssetGrid g;
    CHECK(makeTuft(g));
    CoverFixture fx(&g);
    const std::vector<AssetInstance> insts = fx.field.instancesForRect(
        AssetVoxelRect{0, 0, 31, 31}, [&](int64_t, int64_t) { return flatGround(100'000); });
    CHECK(!insts.empty());
    CHECK_EQ(int(fx.field.resolveForCoverCompose(insts, 30u).size()), 0);
    CHECK_EQ(int(fx.field.resolveForCoverCompose(insts, 0u).size()), 0);
}

VXC_TEST(covercompose_never_admits_a_terrain_lattice_instance) {
    // Trees and rocks are already in the world volume through
    // VoxelAssetStamp.usf -> BrickPackMain (VoxelGpuWorldGen.cpp:1367/1389).
    // Composing them here as well would draw every tree twice.
    AssetGrid g;
    CHECK_EQ(int(g.parse(solidBlock(1, 1, 30, MAT_BARK, uint32_t(kVoxelSizeMm)))),
             int(AssetParseError::kOk));
    CHECK(g.onTerrainLattice());
    OneGridBank bank(&g);

    const AssetLayer layers[1] = {everyCell(3200, 3000, 0, /*terrain*/ true)};
    const AssetSpecies sp[1] = {pillarSpecies(0, 3000, uint32_t(kVoxelSizeMm))};
    AssetField field;
    field.setSeed(kSeed);
    field.setLayers(layers, 1);
    field.setSpecies(sp, 1);
    field.setBankSource(&bank);

    const std::vector<AssetInstance> insts = field.instancesForRect(
        AssetVoxelRect{0, 0, 31, 31}, [&](int64_t, int64_t) { return flatGround(100'000); });
    CHECK(!insts.empty());
    // The world composer takes them ...
    CHECK(!field.resolveForCompose(insts).empty());
    // ... and the cover composer takes none of them, at any pitch.
    CHECK_EQ(int(field.resolveForCoverCompose(insts, 50u).size()), 0);
    CHECK_EQ(int(field.resolveForCoverCompose(insts, uint32_t(kVoxelSizeMm)).size()), 0);
}
