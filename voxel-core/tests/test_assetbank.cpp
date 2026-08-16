// AssetBankLibrary tests: the production IAssetBankSource over a real
// directory layout, driven through a synthetic manifest and synthetic VXA
// blobs written to a scratch directory -- plus one test over the fixture file
// asset-forge actually baked.
//
// WHAT IS UNDER TEST is the residency and refusal model, not the VXA decode
// (test_assetgrid.cpp owns that): banks load per species in sorted filename
// order, seed indices reduce modulo what survived validation, every refusal
// is a NAMED count, and a missing bank composes as air AND as a number --
// because "the table loaded and nothing renders" must be a one-line diagnosis,
// not an afternoon.

#include "voxelcore/assetbank.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "asset_manifest_testutil.h"
#include "vxctest.h"

using namespace vxc;
using vxmtest::VxmSpecies;
using vxmtest::buildVxm;

namespace {

namespace fs = std::filesystem;

// Minimal VXA v3 encoder (the same independent spelling test_assetgrid.cpp
// carries; duplicated deliberately -- see its comment on closed loops).
std::vector<uint8_t> encodeVxa(int32_t ox, int32_t oy, int32_t oz, int32_t nx, int32_t ny,
                               int32_t nz, const std::vector<MaterialId>& dense,
                               uint32_t voxelMm = uint32_t(kVoxelSizeMm)) {
    std::vector<uint8_t> out;
    auto u32 = [&out](uint32_t v) {
        out.push_back(uint8_t(v & 0xffu));
        out.push_back(uint8_t((v >> 8) & 0xffu));
        out.push_back(uint8_t((v >> 16) & 0xffu));
        out.push_back(uint8_t((v >> 24) & 0xffu));
    };
    out.push_back('V'); out.push_back('X'); out.push_back('A'); out.push_back('1');
    u32(3u);
    u32(uint32_t(ox)); u32(uint32_t(oy)); u32(uint32_t(oz));
    u32(uint32_t(nx)); u32(uint32_t(ny)); u32(uint32_t(nz));
    u32(voxelMm);
    std::vector<MaterialId> mats;
    std::vector<uint32_t> lens;
    for (size_t i = 0; i < dense.size(); ++i) {
        if (!mats.empty() && dense[i] == mats.back()) ++lens.back();
        else { mats.push_back(dense[i]); lens.push_back(1u); }
    }
    u32(uint32_t(mats.size()));
    u32(0u);
    u32(0u);
    for (size_t i = 0; i < mats.size(); ++i) { out.push_back(mats[i]); u32(lens[i]); }
    return out;
}

// A 2x2x3 solid block of one material, anchored base-at-origin.
std::vector<uint8_t> tinyTree(MaterialId mat, uint32_t voxelMm = uint32_t(kVoxelSizeMm),
                              int32_t nz = 3) {
    return encodeVxa(-1, -1, 0, 2, 2, nz, std::vector<MaterialId>(size_t(4 * nz), mat),
                     voxelMm);
}

void writeBlob(const fs::path& p, const std::vector<uint8_t>& bytes) {
    fs::create_directories(p.parent_path());
    FILE* f = std::fopen(p.string().c_str(), "wb");
    CHECK(f != nullptr);
    if (f) {
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
}

// One scratch root per test, wiped before use so a rerun is a rerun.
fs::path scratch(const char* name) {
    fs::path root = fs::temp_directory_path() / "vxc_assetbank_tests" / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    return root;
}

std::vector<uint8_t> readFixture(const char* name) {
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

VxmSpecies oakRow(const char* name, uint16_t seedsBaked = 3) {
    VxmSpecies s;
    s.name = name;
    s.weights[TEMPERATE_FOREST] = 800;
    s.seedsBaked = seedsBaked;
    return s;
}

} // namespace

VXC_TEST(assetbank_loads_sorted_and_reduces_the_seed_index) {
    const fs::path root = scratch("sorted");
    // Three seeds, one material each, written OUT OF ORDER so only the sort
    // can make seed 0 mean the same grid twice.
    writeBlob(root / "test-oak" / "test-oak-0003.vxa", tinyTree(MaterialId(18)));
    writeBlob(root / "test-oak" / "test-oak-0001.vxa", tinyTree(MaterialId(16)));
    writeBlob(root / "test-oak" / "test-oak-0002.vxa", tinyTree(MaterialId(17)));

    AssetManifest m;
    CHECK_EQ(int(m.parse(buildVxm({oakRow("test-oak")}))), int(AssetManifestError::kOk));
    AssetBankLibrary lib;
    lib.configure(&m, root.string());

    const AssetGrid* g0 = lib.bankGrid(0, 0);
    const AssetGrid* g1 = lib.bankGrid(0, 1);
    const AssetGrid* g2 = lib.bankGrid(0, 2);
    CHECK(g0 != nullptr && g1 != nullptr && g2 != nullptr);
    if (g0 && g1 && g2) {
        CHECK_EQ(int(g0->at(0, 0, 0)), 16);
        CHECK_EQ(int(g1->at(0, 0, 0)), 17);
        CHECK_EQ(int(g2->at(0, 0, 0)), 18);
        // seedIndex reduces modulo the valid count: a site drawn against a
        // layer seedCount of 4 still lands on a real grid in a 3-seed bank.
        CHECK_EQ(lib.bankGrid(0, 3), g0);
        CHECK_EQ(lib.bankGrid(0, 7), g1);
        // Pointers are stable: the same query is the same grid, not a reload.
        CHECK_EQ(lib.bankGrid(0, 0), g0);
    }
    const AssetBankLibrary::Stats st = lib.stats();
    CHECK_EQ(int(st.filesLoaded), 3);
    CHECK_EQ(int(st.filesRefused), 0);
    CHECK_EQ(int(st.seedCountMismatch), 0);
    CHECK(st.bytesResident > 0);
}

VXC_TEST(assetbank_refuses_by_name_and_still_serves_the_rest) {
    const fs::path root = scratch("refusals");
    // Seed 1 good; seed 2 taller than L1's 34 m cap (341 voxels of trunk);
    // seed 3 baked at 50 mm (wrong lattice for a terrain layer -- the boulder
    // at twice its size); seed 4 carries a material the engine does not have.
    writeBlob(root / "test-oak" / "test-oak-0001.vxa", tinyTree(MaterialId(16)));
    writeBlob(root / "test-oak" / "test-oak-0002.vxa", tinyTree(MaterialId(16), 100, 341));
    writeBlob(root / "test-oak" / "test-oak-0003.vxa", tinyTree(MaterialId(16), 50));
    writeBlob(root / "test-oak" / "test-oak-0004.vxa",
              tinyTree(MaterialId(kMaterialCount)));

    AssetManifest m;
    CHECK_EQ(int(m.parse(buildVxm({oakRow("test-oak", 4)}))), int(AssetManifestError::kOk));
    AssetBankLibrary lib;
    lib.configure(&m, root.string());

    // Force the load, then read the census.
    CHECK_EQ(lib.loadSpecies(0), 1);
    const AssetBankLibrary::Stats st = lib.stats();
    CHECK_EQ(int(st.filesLoaded), 1);
    CHECK_EQ(int(st.filesRefused), 3);
    CHECK_EQ(int(st.refusedBy[size_t(AssetBankError::kTooTall)]), 1);
    // The 50 mm file fails the manifest's own voxel size before the lattice
    // check ever runs -- the manifest said this species bakes at 100 mm.
    CHECK_EQ(int(st.refusedBy[size_t(AssetBankError::kWrongVoxelSize)]), 1);
    CHECK_EQ(int(st.refusedBy[size_t(AssetBankError::kMaterialsBeyond)]), 1);
    // Disk disagrees with the manifest's seedsBaked = 4: counted, because the
    // world moving under the manifest is a fact someone has to see.
    CHECK_EQ(int(st.seedCountMismatch), 1);

    // The surviving seed serves every index.
    const AssetGrid* g = lib.bankGrid(0, 5);
    CHECK(g != nullptr);
    if (g) CHECK_EQ(int(g->at(0, 0, 0)), 16);

    // Every refusal reason names itself.
    for (int e = 0; e <= int(AssetBankError::kTooWide); ++e)
        CHECK(assetBankErrorText(AssetBankError(e))[0] != '\0');
}

VXC_TEST(assetbank_misses_compose_as_air_and_are_counted) {
    const fs::path root = scratch("misses");
    AssetManifest m;
    CHECK_EQ(int(m.parse(buildVxm({oakRow("test-absent", 0)}))),
             int(AssetManifestError::kOk));
    AssetBankLibrary lib;
    lib.configure(&m, root.string());

    // A species the manifest knows whose bank directory does not exist.
    CHECK(lib.bankGrid(0, 0) == nullptr);
    // A bankId past the manifest.
    CHECK(lib.bankGrid(500, 0) == nullptr);

    const AssetBankLibrary::Stats st = lib.stats();
    CHECK_EQ(int(st.requests), 2);
    CHECK_EQ(int(st.missNoSeeds), 1);
    CHECK_EQ(int(st.missNoSpecies), 1);
    CHECK_EQ(int(st.servedGrids), 0);
    // seedsBaked was 0 and disk agrees: NOT a mismatch. "Not baked yet" and
    // "lost the files" are different facts.
    CHECK_EQ(int(st.seedCountMismatch), 0);
}

VXC_TEST(assetbank_serves_the_file_asset_forge_actually_baked) {
    // The tundra-pine fixture through the whole production path: manifest row,
    // directory layout, load-time validation against the canopy layer, and a
    // material read at the anchor -- so "the loader works" is a statement
    // about a real baked tree, not about tinyTree().
    const std::vector<uint8_t> pine = readFixture("asset_tundra_pine_0002.vxa");
    CHECK(!pine.empty());
    const fs::path root = scratch("fixture");
    writeBlob(root / "tundra-pine" / "tundra-pine-0002.vxa", pine);

    VxmSpecies row = oakRow("tundra-pine", 1);
    row.heightMm = 9000;
    AssetManifest m;
    CHECK_EQ(int(m.parse(buildVxm({row}))), int(AssetManifestError::kOk));
    AssetBankLibrary lib;
    lib.configure(&m, root.string());

    const AssetGrid* g = lib.bankGrid(0, 0);
    CHECK(g != nullptr);
    if (g) {
        CHECK(g->valid());
        CHECK(g->onTerrainLattice());
        CHECK(g->materialsWithinEngine());
        CHECK(g->solidCount() > 1000); // a 9 m conifer is not a shrub
    }
    const AssetBankLibrary::Stats st = lib.stats();
    CHECK_EQ(int(st.filesLoaded), 1);
    CHECK_EQ(int(st.filesRefused), 0);
}
