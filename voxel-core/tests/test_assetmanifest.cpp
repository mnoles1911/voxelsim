// VXM1 manifest reader tests.
//
// Two sources, on the test_assetgrid.cpp model. The synthetic blobs come from
// tests/asset_manifest_testutil.h -- an independent hand spelling of the
// layout -- and drive every named refusal by corrupting known offsets. The
// fixture (tests/fixtures/asset_species_v1.vxm) is a file asset-forge's
// tools/export_manifest.py ACTUALLY WROTE over the real 828 specs, and the
// numbers asserted against it are read from specs/tundra-pine.json by eye:
// a disagreement is a real cross-language disagreement about the wire.

#include "voxelcore/assetmanifest.h"

#include <cstdio>
#include <string>
#include <vector>

#include "asset_manifest_testutil.h"
#include "voxelcore/assetfield.h"
#include "vxctest.h"

using namespace vxc;
using vxmtest::VxmSpecies;
using vxmtest::buildVxm;

namespace {

std::vector<uint8_t> readFixture(const char* name) {
    std::string path = std::string(VXC_TEST_FIXTURE_DIR) + "/" + name;
    std::vector<uint8_t> bytes;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return bytes;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        bytes.resize(static_cast<size_t>(n));
        const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
        bytes.resize(got);
    }
    std::fclose(f);
    return bytes;
}

VxmSpecies taigaTree(const std::string& name) {
    VxmSpecies s;
    s.name = name;
    s.weights[TAIGA] = 1000;
    s.weights[TEMPERATE_FOREST] = 200;
    return s;
}

} // namespace

VXC_TEST(assetmanifest_round_trips_a_synthetic_table) {
    VxmSpecies tree = taigaTree("test-pine");
    tree.abundanceQ10 = 922; // 0.9
    tree.clusterQ10 = 768;   // 0.75
    tree.spacingMm = 5000;
    tree.heightMm = 9000;

    VxmSpecies fish;
    fish.name = "test-trout";
    fish.kind = 6; // fish
    fish.layer = kAssetLayerNotScattered;
    fish.flags = 0;
    fish.waterKind = 2;  // river
    fish.waterMask = 0x06; // fresh | brackish
    fish.voxelSizeMm = 20;
    fish.weights[TEMPERATE_FOREST] = 500;
    fish.groupMin = 3;
    fish.groupMax = 12;
    fish.groupRadiusMm = 2500;

    const std::vector<uint8_t> blob = buildVxm({tree, fish});
    AssetManifest m;
    CHECK_EQ(int(m.parse(blob)), int(AssetManifestError::kOk));
    CHECK(m.valid());
    CHECK_EQ(int(m.layers().size()), kAssetLayerCount);
    CHECK_EQ(m.layers()[1].maxHeightMm, 34'000);
    CHECK_EQ(int(m.layers()[3].terrainLattice), 0);
    CHECK_EQ(int(m.species().size()), 2);

    const AssetManifestSpecies& t = m.species()[0];
    CHECK(t.name == "test-pine");
    CHECK_EQ(int(t.kind), int(AssetKind::kTree));
    CHECK_EQ(int(t.layer), 1);
    CHECK_EQ(int(t.terrainLattice), 1);
    CHECK_EQ(int(t.biomeWeightPerMille[TAIGA]), 1000);
    CHECK_EQ(int(t.biomeWeightPerMille[TEMPERATE_FOREST]), 200);
    CHECK_EQ(int(t.biomeWeightPerMille[DESERT]), 0);
    CHECK_EQ(int(t.abundanceQ10), 922);
    CHECK_EQ(int(t.clusterQ10), 768);
    CHECK_EQ(t.spacingMm, 5000);
    CHECK_EQ(t.heightMm, 9000);
    CHECK_EQ(int(t.seedsBaked), 4);

    const AssetManifestSpecies& f = m.species()[1];
    CHECK(f.name == "test-trout");
    CHECK_EQ(int(f.kind), int(AssetKind::kFish));
    CHECK_EQ(int(f.layer), int(kAssetLayerNotScattered));
    CHECK_EQ(int(f.waterKind), int(AssetWaterKind::kRiver));
    CHECK_EQ(int(f.waterMask), 0x06);
    CHECK_EQ(int(f.groupMin), 3);
    CHECK_EQ(int(f.groupMax), 12);
    CHECK_EQ(f.groupRadiusMm, 2500);
    CHECK_EQ(f.voxelSizeMm, 20u);
}

VXC_TEST(assetmanifest_refuses_with_named_reasons) {
    const std::vector<uint8_t> good = buildVxm({taigaTree("test-pine")});
    AssetManifest m;

    // Offsets are the layout constants asset_manifest_testutil.h documents.
    std::vector<uint8_t> b = good;
    b[0] = 'X';
    CHECK_EQ(int(m.parse(b)), int(AssetManifestError::kBadMagic));

    b = good;
    b[4] = 99;
    CHECK_EQ(int(m.parse(b)), int(AssetManifestError::kBadVersion));

    b = good;
    b[8] = uint8_t(kBiomeCount + 1);
    CHECK_EQ(int(m.parse(b)), int(AssetManifestError::kBadBiomeCount));

    b = good;
    b[12] = uint8_t(kAssetLayerCount + 1);
    CHECK_EQ(int(m.parse(b)), int(AssetManifestError::kBadLayerCount));

    b = good;
    b[20] = 151;
    CHECK_EQ(int(m.parse(b)), int(AssetManifestError::kBadRecordSize));

    b = good;
    b.resize(b.size() - 1);
    CHECK_EQ(int(m.parse(b)), int(AssetManifestError::kTruncated));

    CHECK_EQ(int(m.parse(good.data(), 16)), int(AssetManifestError::kTooSmall));

    // THE REORDER GUARD, which is the check this format exists for: swap two
    // biome names and every weight in the library silently changes biome.
    // Biome names start at offset 32; "ocean" and "beach" are rows 0 and 1.
    b = good;
    b[32 + 0] = 'b';
    b[32 + 1] = 'e';
    b[32 + 2] = 'a';
    b[32 + 3] = 'c';
    b[32 + 4] = 'h';
    CHECK_EQ(int(m.parse(b)), int(AssetManifestError::kBiomeOrderMismatch));

    // Species record fields: species block starts at 288.
    b = good;
    b[288 + 64] = 200; // kind
    CHECK_EQ(int(m.parse(b)), int(AssetManifestError::kBadKind));

    b = good;
    b[288 + 65] = uint8_t(kAssetLayerCount); // layer index one past the table
    CHECK_EQ(int(m.parse(b)), int(AssetManifestError::kBadLayer));

    b = good;
    b[288] = 0; // empty name
    CHECK_EQ(int(m.parse(b)), int(AssetManifestError::kBadName));

    b = good;
    b[288 + 2] = uint8_t(' '); // a path component with a space is refused
    CHECK_EQ(int(m.parse(b)), int(AssetManifestError::kBadName));

    // A species taller than its own layer's cap: the exporter checks this at
    // bake, so a file that carries it is a file the exporter did not write.
    VxmSpecies tall = taigaTree("test-too-tall");
    tall.heightMm = 40'000; // L1 cap is 34 m
    b = buildVxm({tall});
    CHECK_EQ(int(m.parse(b)), int(AssetManifestError::kSpeciesMisfiled));
    CHECK(m.misfiledName() == "test-too-tall");
    CHECK_EQ(int(m.misfiledWhy()), int(AssetTableError::kTooTall));

    // A 5 cm species filed on a terrain layer: stamped at twice its size.
    VxmSpecies wrongLattice = taigaTree("test-wrong-lattice");
    wrongLattice.voxelSizeMm = 50;
    b = buildVxm({wrongLattice});
    CHECK_EQ(int(m.parse(b)), int(AssetManifestError::kSpeciesMisfiled));
    CHECK_EQ(int(m.misfiledWhy()), int(AssetTableError::kWrongLattice));

    // Every reason says something: "the table did not load" is not a diagnosis.
    for (int e = 0; e <= int(AssetManifestError::kSpeciesMisfiled); ++e)
        CHECK(assetManifestErrorText(AssetManifestError(e))[0] != '\0');

    // And a refused parse leaves the manifest EMPTY, not half-filled.
    CHECK(!m.valid());
    CHECK_EQ(int(m.species().size()), 0);
}

VXC_TEST(assetmanifest_fold_is_weight_x_abundance_x_spacing_residual) {
    // Species A: full weight, abundance 0.5, spacing at the cell pitch.
    VxmSpecies a = taigaTree("test-a");
    a.abundanceQ10 = 512;
    a.spacingMm = 5000; // == L1 cell
    // Species B: same, but authored at 4x the cell -- the residual is 1/16.
    VxmSpecies b = taigaTree("test-b");
    b.abundanceQ10 = 512;
    b.spacingMm = 20'000;
    // Species C: spacing TIGHTER than the cell folds at 1, not above it: the
    // lattice cannot place closer than its own pitch, so the fold must not
    // reward asking.
    VxmSpecies c = taigaTree("test-c");
    c.abundanceQ10 = 1024;
    c.spacingMm = 2500;

    AssetManifest m;
    CHECK_EQ(int(m.parse(buildVxm({a, b, c}))), int(AssetManifestError::kOk));
    std::vector<AssetSpecies> table;
    const AssetTableBuildStats st = assetSpeciesTableFromManifest(m, table);
    CHECK_EQ(st.kept, 3);

    // A: 1000 * 512/1024 * 1 = 500.
    CHECK_EQ(int(table[0].weightPerMille[TAIGA]), 500);
    // B: 1000 * 0.5 / 16 = 31.25 -> 31 (round to nearest).
    CHECK_EQ(int(table[1].weightPerMille[TAIGA]), 31);
    // C: clamped residual: 1000 * 1 * 1 = 1000.
    CHECK_EQ(int(table[2].weightPerMille[TAIGA]), 1000);
    // The secondary biome folds with the same factors.
    CHECK_EQ(int(table[0].weightPerMille[TEMPERATE_FOREST]), 100);

    // bankId is the manifest ROW INDEX -- the bank library's key.
    CHECK_EQ(int(table[0].bankId), 0);
    CHECK_EQ(int(table[2].bankId), 2);
    // And the folded rows still satisfy the load-time filing check.
    for (const AssetSpecies& s : table)
        CHECK_EQ(int(assetSpeciesFits(s, m.layers().data(), int(m.layers().size()), 0)),
                 int(AssetTableError::kOk));
}

VXC_TEST(assetmanifest_fold_counts_what_it_drops) {
    // A detail entity (not this table's business), a species too rare for
    // per-mille, a species with no biome anywhere, a kept species without
    // banks. Every dropped row is a NUMBER, because a fold that quietly
    // shipped 500 of 828 species is this project's signature failure.
    VxmSpecies fish;
    fish.name = "test-fish";
    fish.kind = 6;
    fish.layer = kAssetLayerNotScattered;
    fish.flags = 0;
    fish.weights[OCEAN] = 500;

    VxmSpecies rare = taigaTree("test-landmark");
    rare.layer = 0;          // 24 m lattice
    rare.spacingMm = 3'000'000; // 3 km: (24/3000)^2 * anything << 1 per-mille
    rare.heightMm = 60'000;

    VxmSpecies dark = taigaTree("test-dark");
    for (uint32_t i = 0; i < kBiomeCount; ++i) dark.weights[i] = 0;

    VxmSpecies unbaked = taigaTree("test-unbaked");
    unbaked.seedsBaked = 0;

    AssetManifest m;
    CHECK_EQ(int(m.parse(buildVxm({fish, rare, dark, unbaked}))),
             int(AssetManifestError::kOk));
    std::vector<AssetSpecies> table;
    const AssetTableBuildStats st = assetSpeciesTableFromManifest(m, table);
    CHECK_EQ(st.kept, 1);
    CHECK_EQ(st.detailEntities, 1);
    CHECK_EQ(st.tooRare, 1);
    CHECK_EQ(st.noBiome, 1);
    CHECK_EQ(st.withoutBanks, 1);
    CHECK_EQ(int(table.size()), 1);
    CHECK_EQ(int(table[0].bankId), 3); // test-unbaked's manifest row
}

VXC_TEST(assetmanifest_reads_the_file_asset_forge_actually_wrote) {
    const std::vector<uint8_t> blob = readFixture("asset_species_v1.vxm");
    CHECK(!blob.empty());
    AssetManifest m;
    CHECK_EQ(int(m.parse(blob)), int(AssetManifestError::kOk));

    // 828 specs, minus the two whose height exceeds every layer
    // (hero-sequoia, hero-arch-colossal -- the exporter refuses and names
    // them). If the library gains or loses species this constant moves WITH
    // the fixture, deliberately: re-export, re-copy, re-read the report.
    CHECK_EQ(int(m.species().size()), 826);
    CHECK_EQ(int(m.layers().size()), kAssetLayerCount);
    // The layer table the manifest was built against -- the price list.
    CHECK_EQ(m.layers()[0].cellMm, 24'000);
    CHECK_EQ(m.layers()[0].densityPerMille, uint16_t(60));
    CHECK_EQ(m.layers()[1].maxHeightMm, 34'000);
    CHECK_EQ(int(m.layers()[3].terrainLattice), 0);

    // tundra-pine, numbers read from specs/tundra-pine.json by eye:
    // biomes.taiga 1.0, temperate_forest 0.2, tundra_alpine 0.35; abundance
    // 0.9; cluster 0.75; spacing 4.5 m; elev 0..2200 m; slope_max 70%
    // (55 -> 70 in the scree-slope-band pass, 20248e2, which never refreshed
    // this fixture; caught at the 2026-08-18 re-bless);
    // height 9 m; kind tree; 4 baked bank seeds on disk.
    const AssetManifestSpecies* pine = nullptr;
    for (const AssetManifestSpecies& s : m.species())
        if (s.name == "tundra-pine") pine = &s;
    CHECK(pine != nullptr);
    if (pine != nullptr) {
        CHECK_EQ(int(pine->kind), int(AssetKind::kTree));
        CHECK_EQ(int(pine->layer), 1); // 9 m: canopy
        CHECK_EQ(int(pine->terrainLattice), 1);
        CHECK_EQ(int(pine->biomeWeightPerMille[TAIGA]), 1000);
        CHECK_EQ(int(pine->biomeWeightPerMille[TEMPERATE_FOREST]), 200);
        CHECK_EQ(int(pine->biomeWeightPerMille[TUNDRA_ALPINE]), 350);
        CHECK_EQ(int(pine->biomeWeightPerMille[DESERT]), 0);
        CHECK_EQ(int(pine->abundanceQ10), 922); // 0.9 * 1024, rounded
        CHECK_EQ(int(pine->clusterQ10), 768);   // 0.75 * 1024
        CHECK_EQ(pine->spacingMm, 4500);
        CHECK_EQ(pine->elevMinMm, 0);
        CHECK_EQ(pine->elevMaxMm, 2'200'000);
        CHECK_EQ(pine->slopeMaxMmPerM, 700);
        CHECK_EQ(pine->heightMm, 9000);
        CHECK_EQ(pine->voxelSizeMm, 100u);
        CHECK_EQ(int(pine->seedsBaked), 4);
    }

    // The fold over the whole real library, with its counts printed so a
    // future re-export shows its numbers in the test log.
    std::vector<AssetSpecies> table;
    const AssetTableBuildStats st = assetSpeciesTableFromManifest(m, table);
    std::printf(
        "    fixture manifest: %d kept, %d detail entities, %d too rare, %d no-biome, "
        "%d without banks\n",
        st.kept, st.detailEntities, st.tooRare, st.noBiome, st.withoutBanks);
    // 826 = kept + detail entities (382 fish/bird/quadruped/cetacean) +
    // tooRare (the landmark heroes) + noBiome. The exact split is pinned
    // loosely -- the invariants, not the census:
    CHECK_EQ(st.kept + st.detailEntities + st.tooRare + st.noBiome,
             int(m.species().size()));
    CHECK(st.kept > 400);          // the plant/rock library is most of the file
    CHECK(st.detailEntities > 300); // 131+127+106+18 animals
    CHECK(st.tooRare >= 4 && st.tooRare <= 12); // the hero landmarks
    // Terrain-kind banks were baked (4 seeds each); ground cover has none yet.
    CHECK(st.withoutBanks > 0);
}

VXC_TEST(assetmanifest_imported_table_realises_a_subset_of_what_the_bound_accounted_for) {
    // THE VETO-ONLY CONTRACT, extended from test_assetpolicy.cpp's pinned
    // property to the REAL imported table: for every chunk in a sweep, every
    // instance the policy produces from the fixture manifest corresponds to a
    // site the bound already saw, on the site's own layer, no taller than
    // that layer's declared maximum. If this fails, assetTopAboveSurfaceMm
    // has stopped being an upper bound over the production data and the
    // failure mode is a hole in the world.
    const std::vector<uint8_t> blob = readFixture("asset_species_v1.vxm");
    AssetManifest m;
    CHECK_EQ(int(m.parse(blob)), int(AssetManifestError::kOk));
    std::vector<AssetSpecies> table;
    assetSpeciesTableFromManifest(m, table);
    CHECK(!table.empty());

    const uint64_t seed = 20260719;
    const AssetLayer* layers = m.layers().data();
    const int layerCount = int(m.layers().size());

    AssetColumnFacts taigaGround;
    taigaGround.known = true;
    taigaGround.biome = TAIGA;
    taigaGround.surfaceMm = 400'000; // 400 m: inside every conifer's band
    taigaGround.slopeMmPerM = 120;
    taigaGround.anchorSolid = true;
    taigaGround.distanceToWaterMm = kAssetNoWaterDistanceMm;

    int checked = 0;
    for (int64_t cy = -3; cy <= 3; ++cy)
        for (int64_t cx = -3; cx <= 3; ++cx) {
            // A level-0 render chunk footprint: 32 voxels.
            const AssetVoxelRect rect{cx * 32, cy * 32, cx * 32 + 31, cy * 32 + 31};
            const int32_t bound = assetTopAboveSurfaceMm(seed, layers, layerCount, rect);
            for (const AssetSite& s : assetSitesForRect(seed, layers, layerCount, rect)) {
                AssetInstance inst;
                if (!assetResolveSite(seed, layers, layerCount, table.data(),
                                      int(table.size()), s, taigaGround, inst))
                    continue;
                CHECK_EQ(int(inst.layer), s.layer);
                CHECK(table[inst.speciesIndex].heightMm <= layers[inst.layer].maxHeightMm);
                // The bound accounts for TERRAIN layers only: a detail-lattice
                // instance (ground cover) puts no voxel in the world grid, is
                // deliberately excluded from the bound, and materialOfInstance
                // refuses to stamp it -- so the subset property it must satisfy
                // is its layer's own cap, asserted above, not the bound.
                if (layers[inst.layer].terrainLattice)
                    CHECK(layers[inst.layer].maxHeightMm <= bound);
                ++checked;
            }
        }
    // Vacuous-truth guard: a taiga column at 400 m that places nothing from
    // the real library means the import is broken, not that the test passed.
    CHECK(checked > 50);
}
