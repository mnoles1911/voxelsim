// VXA1 reader tests.
//
// THE POINT OF THE FIXTURES. Two of these tests read files that asset-forge
// (Python) actually wrote, not files this test encoded. That distinction is the
// whole value: a round trip against an encoder written in this same file proves
// two copies of my own understanding agree, which is exactly the closed loop
// docs/tree-asset-generator-research.md section 9 calls out when it says the
// only thing verifying the .vox output is "our own reader, which is a closed
// loop". The numbers asserted below (extent, origin, run count, solid count,
// material set, and a full-grid digest) were taken from the Python side with
// numpy, so a disagreement is a real cross-language disagreement about the wire
// format -- the packed-vs-aligned struct record being the one most likely to
// bite, since numpy pads structured dtypes only when asked and vxa.py does not
// ask.

#include "voxelcore/assetgrid.h"

#include <cstdio>
#include <string>
#include <vector>

#include "vxctest.h"

using namespace vxc;

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

// Minimal VXA encoder, for the synthetic cases only. Never used to validate
// the reader against itself -- the fixture tests do that job.
//
// `voxelMm` defaults to the terrain lattice because most synthetic cases do not
// care, but it is a PARAMETER rather than a constant so the malformed-blob test
// can hand it a zero and prove kBadVoxelSize fires.
std::vector<uint8_t> encode(int32_t ox, int32_t oy, int32_t oz, int32_t nx, int32_t ny, int32_t nz,
                            const std::vector<MaterialId>& dense,
                            uint32_t voxelMm = uint32_t(kVoxelSizeMm)) {
    std::vector<uint8_t> out;
    auto u32 = [&out](uint32_t v) {
        out.push_back(uint8_t(v & 0xffu));
        out.push_back(uint8_t((v >> 8) & 0xffu));
        out.push_back(uint8_t((v >> 16) & 0xffu));
        out.push_back(uint8_t((v >> 24) & 0xffu));
    };
    out.push_back('V'); out.push_back('X'); out.push_back('A'); out.push_back('1');
    u32(3u); // version
    u32(static_cast<uint32_t>(ox)); u32(static_cast<uint32_t>(oy)); u32(static_cast<uint32_t>(oz));
    u32(static_cast<uint32_t>(nx)); u32(static_cast<uint32_t>(ny)); u32(static_cast<uint32_t>(nz));
    u32(voxelMm);

    std::vector<MaterialId> mats;
    std::vector<uint32_t> lens;
    for (size_t i = 0; i < dense.size(); ++i) {
        if (!mats.empty() && dense[i] == mats.back()) {
            ++lens.back();
        } else {
            mats.push_back(dense[i]);
            lens.push_back(1u);
        }
    }
    u32(static_cast<uint32_t>(mats.size()));
    u32(0u); // part runs: the synthetic cases carry no rig
    u32(0u); // joints
    for (size_t i = 0; i < mats.size(); ++i) {
        out.push_back(mats[i]);
        u32(lens[i]);
    }
    return out;
}

// FNV-1a over the grid in the same C order the Python side digested, so the
// two digests are comparable by construction.
uint64_t digestGrid(const AssetGrid& g) {
    Digest d;
    for (int32_t x = 0; x < g.sizeX(); ++x)
        for (int32_t y = 0; y < g.sizeY(); ++y)
            for (int32_t z = 0; z < g.sizeZ(); ++z) d.u8(g.at(x, y, z));
    return d.h;
}

} // namespace

VXC_TEST(assetgrid_reads_a_real_asset_forge_file) {
    const std::vector<uint8_t> blob = readFixture("asset_tundra_pine_0002.vxa");
    CHECK(!blob.empty());
    AssetGrid g;
    const AssetParseError err = g.parse(blob);
    CHECK_EQ(int(err), int(AssetParseError::kOk));
    CHECK(g.valid());

    // Every number below read off the Python decode of this same file.
    //
    // The fixture was re-baked when VXA went to version 2, and the numbers
    // moved for TWO independent reasons that are worth keeping apart: the
    // format gained a voxel-size field (4 more header bytes), and the species
    // itself moved from the 5 cm asset lattice to the 10 cm terrain lattice,
    // because a tree joins the world grid and is destructible as terrain is.
    // So this is a coarser pine, not a different one.
    CHECK_EQ(g.sizeX(), 49);
    CHECK_EQ(g.sizeY(), 49);
    CHECK_EQ(g.sizeZ(), 80);
    CHECK_EQ(g.originX(), -24);
    CHECK_EQ(g.originY(), -24);
    CHECK_EQ(g.originZ(), 0);
    CHECK_EQ(int(g.runCount()), 13856);
    CHECK_EQ(int(g.solidCount()), 14314);

    // A TREE IS ON THE TERRAIN LATTICE. This is the assertion that would have
    // caught the whole class of bug version 2 exists for: before it, a file
    // could not say what scale it was and this reader had to assume.
    CHECK_EQ(int(g.voxelSizeMm()), int(kVoxelSizeMm));
    CHECK(g.onTerrainLattice());

    // Probe cells, chosen on the Python side before this test existed. The
    // trunk cell in particular is the one that catches a record-stride error:
    // a reader assuming 8-byte aligned records walks off into the wrong run
    // almost immediately and every probe past the first disagrees.
    CHECK_EQ(int(g.at(24, 24, 0)), 16);  // bark, at the base of the trunk
    CHECK_EQ(int(g.at(0, 0, 0)), 0);
    CHECK_EQ(int(g.at(24, 24, 40)), 16); // bark, halfway up the same trunk
    CHECK_EQ(int(g.at(48, 48, 79)), 0);
    CHECK_EQ(int(g.at(15, 15, 28)), 0);
}

VXC_TEST(assetgrid_reads_a_second_real_file_with_a_different_material_set) {
    const std::vector<uint8_t> blob = readFixture("asset_meadow_daisy_0001.vxa");
    CHECK(!blob.empty());
    AssetGrid g;
    CHECK_EQ(int(g.parse(blob)), int(AssetParseError::kOk));
    CHECK_EQ(g.sizeX(), 9);
    CHECK_EQ(g.sizeY(), 8);
    CHECK_EQ(g.sizeZ(), 9);
    CHECK_EQ(g.originX(), 4);
    CHECK_EQ(g.originY(), 4);
    CHECK_EQ(g.originZ(), 0);
    CHECK_EQ(int(g.runCount()), 91);
    CHECK_EQ(int(g.solidCount()), 106);
    CHECK_EQ(int(g.at(4, 4, 0)), 8); // MAT_GRASS

    // A FLOWER IS A DETAIL ASSET AND IS NOT ON THE TERRAIN LATTICE. Deliberately
    // paired with the pine above: one fixture of each class, so a reader that
    // ignored the new field and assumed terrain lattice fails here and only
    // here. 50 mm against the world's 100.
    CHECK_EQ(int(g.voxelSizeMm()), 50);
    CHECK(!g.onTerrainLattice());
}

// Every baked asset must be built from materials this engine actually defines.
//
// This test was originally written the other way up, asserting that they were
// NOT -- because at the time none of them were, and a test that merely noted
// the gap would have let it be forgotten. It was built to fail on the day the
// enum append landed and point at itself. It did, and this is that edit.
//
// Keeping it, inverted, is the point: the ids in a .vxa are baked by
// asset-forge and the ids in vxc::Material are declared here, and nothing but
// this test makes the two agree. If they drift, a tree loads and renders as
// SOMETHING -- MaterialId is a uint8_t, so an unknown id indexes past the end
// of every material-keyed array instead of faulting -- which is the failure
// mode that hid the original gap for as long as it did.
VXC_TEST(assetgrid_real_assets_use_materials_the_engine_defines) {
    AssetGrid pine, daisy;
    CHECK_EQ(int(pine.parse(readFixture("asset_tundra_pine_0002.vxa"))), int(AssetParseError::kOk));
    CHECK_EQ(int(daisy.parse(readFixture("asset_meadow_daisy_0001.vxa"))), int(AssetParseError::kOk));

    CHECK_EQ(int(pine.maxMaterialId()), 20);  // MAT_LEAF_NEEDLE
    CHECK_EQ(int(daisy.maxMaterialId()), 24); // MAT_LEAF_BLOSSOM
    CHECK_EQ(int(kMaterialCount), 47);        // through MAT_BEAK_HORN = 46
    CHECK(pine.materialsWithinEngine());
    CHECK(daisy.materialsWithinEngine());
}

VXC_TEST(assetgrid_random_access_agrees_with_a_full_sequential_decode) {
    // The column index is the only clever thing in the reader, and this is the
    // property it must have: at(x,y,z) for every cell must equal what a plain
    // sequential expansion of the runs produces. Run over a real file so the
    // run structure is a real one (16,430 runs, many columns whose first cell
    // lands mid-run) rather than something convenient.
    const std::vector<uint8_t> blob = readFixture("asset_tundra_pine_0002.vxa");
    AssetGrid g;
    CHECK_EQ(int(g.parse(blob)), int(AssetParseError::kOk));

    // Sequential expansion, straight from the wire, independent of the reader.
    std::vector<MaterialId> flat;
    flat.reserve(size_t(g.sizeX()) * size_t(g.sizeY()) * size_t(g.sizeZ()));
    {
        const uint8_t* rec = blob.data() + kVxaHeaderBytes;
        for (size_t i = 0; i < g.runCount(); ++i, rec += kVxaRunBytes) {
            const uint32_t len = uint32_t(rec[1]) | (uint32_t(rec[2]) << 8) |
                                 (uint32_t(rec[3]) << 16) | (uint32_t(rec[4]) << 24);
            for (uint32_t k = 0; k < len; ++k) flat.push_back(static_cast<MaterialId>(rec[0]));
        }
    }
    CHECK_EQ(int(flat.size()), g.sizeX() * g.sizeY() * g.sizeZ());

    size_t idx = 0;
    int mismatches = 0;
    for (int32_t x = 0; x < g.sizeX(); ++x)
        for (int32_t y = 0; y < g.sizeY(); ++y)
            for (int32_t z = 0; z < g.sizeZ(); ++z, ++idx)
                if (g.at(x, y, z) != flat[idx]) ++mismatches;
    CHECK_EQ(mismatches, 0);
}

VXC_TEST(assetgrid_reads_the_rig_of_a_baked_animal) {
    // The one fixture with parts. A tree and a flower have nothing that moves
    // relative to anything else, so they answer hasParts() false -- which is
    // not a degraded case and is asserted alongside, so a reader that lost the
    // parts entirely cannot pass by looking like a rock.
    AssetGrid g;
    CHECK_EQ(int(g.parse(readFixture("asset_common_raven_0007.vxa"))),
             int(AssetParseError::kOk));
    CHECK(g.hasParts());
    CHECK_EQ(int(g.voxelSizeMm()), 10);
    CHECK(!g.onTerrainLattice());
    CHECK_EQ(int(g.joints().size()), 8);

    // Every solid voxel belongs to a part, and no empty voxel does. This is the
    // assertion that catches a part table one run out of step with the material
    // table -- they tile the same box, and nothing else checks that they agree.
    int solid = 0, taggedSolid = 0, taggedAir = 0;
    for (int x = 0; x < g.sizeX(); ++x) {
        for (int y = 0; y < g.sizeY(); ++y) {
            for (int z = 0; z < g.sizeZ(); ++z) {
                const bool s = g.at(x, y, z) != MAT_AIR;
                const bool p = g.partAt(x, y, z) != 0;
                if (s) { ++solid; if (p) ++taggedSolid; }
                else if (p) ++taggedAir;
            }
        }
    }
    CHECK_EQ(solid, 5989);
    CHECK_EQ(taggedSolid, 5989);
    CHECK_EQ(taggedAir, 0);

    // A tree carries none of this, and says so.
    AssetGrid pine;
    CHECK_EQ(int(pine.parse(readFixture("asset_tundra_pine_0002.vxa"))),
             int(AssetParseError::kOk));
    CHECK(!pine.hasParts());
    CHECK_EQ(int(pine.joints().size()), 0);
    CHECK_EQ(int(pine.partAt(24, 24, 0)), 0);
}

VXC_TEST(assetgrid_out_of_range_reads_answer_air_on_every_face) {
    // The mesher's 1-voxel apron reads one cell outside every brick, so a brick
    // flush with the asset's bounding box reads outside it on every load. This
    // must be air, not a wrap, not a clamp: a clamp would smear the asset's
    // boundary column outward and weld the tree to whatever is next to it.
    const std::vector<MaterialId> dense(2 * 3 * 4, MaterialId(MAT_ROCK));
    AssetGrid g;
    CHECK_EQ(int(g.parse(encode(0, 0, 0, 2, 3, 4, dense))), int(AssetParseError::kOk));
    CHECK_EQ(int(g.at(1, 2, 3)), int(MAT_ROCK));
    CHECK_EQ(int(g.at(-1, 0, 0)), int(MAT_AIR));
    CHECK_EQ(int(g.at(0, -1, 0)), int(MAT_AIR));
    CHECK_EQ(int(g.at(0, 0, -1)), int(MAT_AIR));
    CHECK_EQ(int(g.at(2, 0, 0)), int(MAT_AIR));
    CHECK_EQ(int(g.at(0, 3, 0)), int(MAT_AIR));
    CHECK_EQ(int(g.at(0, 0, 4)), int(MAT_AIR));
}

VXC_TEST(assetgrid_rejects_malformed_blobs_by_reason) {
    const std::vector<MaterialId> dense(2 * 2 * 2, MaterialId(MAT_ROCK));
    const std::vector<uint8_t> good = encode(0, 0, 0, 2, 2, 2, dense);

    AssetGrid g;
    CHECK_EQ(int(g.parse(nullptr, 0)), int(AssetParseError::kTooSmall));
    CHECK_EQ(int(g.parse(good.data(), 12)), int(AssetParseError::kTooSmall));

    std::vector<uint8_t> badMagic = good;
    badMagic[1] = 'Y';
    CHECK_EQ(int(g.parse(badMagic)), int(AssetParseError::kBadMagic));

    std::vector<uint8_t> badVersion = good;
    badVersion[4] = 9;
    CHECK_EQ(int(g.parse(badVersion)), int(AssetParseError::kBadVersion));

    // VERSION 1 IS REFUSED, NOT ASSUMED. It is the one bad version that will
    // actually turn up, because every asset baked before the voxel-size field
    // existed is one. Reading it as terrain lattice was the tempting
    // compromise and it is wrong for the whole corpus that existed when this
    // changed -- granite-boulder and tundra-pine were both v1 at 5 cm. The
    // offsets alone make it unsafe: at v2 positions, a v1 file's run count is
    // read as its voxel size.
    std::vector<uint8_t> v1 = good;
    v1[4] = 1;
    CHECK_EQ(int(g.parse(v1)), int(AssetParseError::kBadVersion));

    // v2 likewise: it records a scale but no rig, and a rig cannot be inferred
    // from voxels -- which voxels were a wing is knowledge only the generator
    // had. tools/vxa_upgrade.py converts one for assets that need no rig.
    std::vector<uint8_t> v2 = good;
    v2[4] = 2;
    CHECK_EQ(int(g.parse(v2)), int(AssetParseError::kBadVersion));

    // A voxel has to have a size. Zero is what an uninitialised or truncated
    // writer produces, and it is the value that would divide by zero in any
    // caller converting local voxels to metres.
    CHECK_EQ(int(g.parse(encode(0, 0, 0, 2, 2, 2, dense, 0u))),
             int(AssetParseError::kBadVoxelSize));
    CHECK_EQ(int(g.parse(encode(0, 0, 0, 2, 2, 2, dense, 100000u))),
             int(AssetParseError::kBadVoxelSize));

    // ... and a legal non-terrain one is accepted and reported as itself. A
    // 10 mm bird is not malformed, it is a detail entity.
    CHECK_EQ(int(g.parse(encode(0, 0, 0, 2, 2, 2, dense, 10u))), int(AssetParseError::kOk));
    CHECK_EQ(int(g.voxelSizeMm()), 10);
    CHECK(!g.onTerrainLattice());

    std::vector<uint8_t> zeroDim = good;
    zeroDim[20] = zeroDim[21] = zeroDim[22] = zeroDim[23] = 0; // nx = 0
    CHECK_EQ(int(g.parse(zeroDim)), int(AssetParseError::kBadDimensions));

    // Header promises its runs but the body is cut short.
    CHECK_EQ(int(g.parse(good.data(), good.size() - 1)), int(AssetParseError::kTruncatedBody));

    // Runs that do not tile the box. Shrinking the only run's length leaves the
    // sum below the cell count; this is the check that catches a writer whose
    // idea of the extent disagrees with its idea of the body.
    std::vector<uint8_t> shortRuns = good;
    shortRuns[kVxaHeaderBytes + 1] = 3; // run length 8 -> 3
    CHECK_EQ(int(g.parse(shortRuns)), int(AssetParseError::kRunLengthSum));

    // And a failed parse must read as ALL AIR, not as half an asset.
    CHECK(!g.valid());
    CHECK_EQ(int(g.at(0, 0, 0)), int(MAT_AIR));
}

VXC_TEST(assetgrid_yaw_is_a_bijection_that_preserves_content) {
    // Four quarter turns must return the asset to itself, and each turn must
    // move solid mass rather than lose it -- a rotation that silently drops
    // cells outside the rotated box would thin every rotated instance, which
    // reads as "some trees are sparser than others" and never as a bug.
    std::vector<MaterialId> dense(3 * 5 * 2, MaterialId(MAT_AIR));
    // An L shape, so no symmetry can hide a wrong rotation.
    auto put = [&](int x, int y, int z, MaterialId m) { dense[size_t((x * 5 + y) * 2 + z)] = m; };
    put(0, 0, 0, MAT_ROCK);
    put(1, 0, 0, MAT_ROCK);
    put(2, 0, 0, MAT_ROCK);
    put(0, 1, 0, MAT_SAND);
    put(0, 2, 0, MAT_SAND);

    AssetGrid g;
    CHECK_EQ(int(g.parse(encode(0, 0, 0, 3, 5, 2, dense))), int(AssetParseError::kOk));

    for (uint8_t yaw = 0; yaw < 4; ++yaw) {
        const int32_t w = g.rotatedSizeX(yaw), h = g.rotatedSizeY(yaw);
        CHECK_EQ(w * h, g.sizeX() * g.sizeY());
        uint64_t solid = 0;
        for (int32_t x = 0; x < w; ++x)
            for (int32_t y = 0; y < h; ++y)
                for (int32_t z = 0; z < g.sizeZ(); ++z)
                    if (g.atYaw(x, y, z, yaw) != MAT_AIR) ++solid;
        CHECK_EQ(int(solid), int(g.solidCount()));
    }

    // yaw 0 is the identity, and yaw 2 is its own inverse.
    for (int32_t x = 0; x < g.sizeX(); ++x)
        for (int32_t y = 0; y < g.sizeY(); ++y)
            for (int32_t z = 0; z < g.sizeZ(); ++z) {
                CHECK_EQ(int(g.atYaw(x, y, z, 0)), int(g.at(x, y, z)));
                CHECK_EQ(int(g.atYaw(g.sizeX() - 1 - x, g.sizeY() - 1 - y, z, 2)),
                         int(g.at(x, y, z)));
            }
}

VXC_TEST(assetgrid_digest_is_stable_across_a_reparse) {
    // Cheap determinism guard: the reader must be a pure function of the bytes.
    const std::vector<uint8_t> blob = readFixture("asset_meadow_daisy_0001.vxa");
    AssetGrid a, b;
    CHECK_EQ(int(a.parse(blob)), int(AssetParseError::kOk));
    CHECK_EQ(int(b.parse(blob)), int(AssetParseError::kOk));
    CHECK_EQ(int(digestGrid(a) == digestGrid(b)), 1);
    CHECK(digestGrid(a) != 0);
}
