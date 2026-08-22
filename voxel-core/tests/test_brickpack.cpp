// Canonical brick packing, CPU reference (voxelcore/brickpack.h). Phase 1-A of
// docs/ray-marching-plan-2026-08-19.md, against docs/brick-volume-format.md.
//
// WHAT THESE TESTS ARE FOR. The shipping producer is a compute kernel and the
// consumer is another one; the only way to look at either in situ is to launch
// the editor and judge terrain. Everything that can be silently wrong about the
// format is arithmetic — a palette order, a popcount rank, a bit offset — and
// all of it is here, where a wrong answer is a FAIL line rather than a hillside
// with every distant voxel the wrong colour.
//
// TWO OF THESE TESTS ARE THE REASON THE FILE EXISTS AT ALL:
//
//   brickpack_palette_order_independence — the format is canonical precisely so
//   that a GPU brick can be byte-compared against a CPU one. If content built
//   in a different order packs to different bytes, voxel.March.VerifyBricks can
//   never pass and there is no gate on the producer at all. It is tested with
//   its own premise checked: the insertion-ordered palette that vxc::Brick uses
//   is shown to DIFFER across the same orders, so the test cannot pass vacuously
//   by measuring a property nothing threatens.
//
//   brickpack_occupancy_compaction_sparse — material entries are indexed by the
//   popcount of occupancy bits BELOW a voxel, not by its cell index. The two
//   agree exactly on a fully solid brick, so a packer that confused them looks
//   perfect on dense terrain and mis-colours every sparse brick. The test reads
//   the payload slots WITHOUT going through solidRankBelow, because a decoder
//   that shares a broken rank function with the packer round-trips perfectly.
//
// EVERY TEST MUST BE DISTINGUISHABLE FROM "DID NOT RUN" (the standing rule after
// three absent-stat zeros produced false conclusions in one session). Beyond the
// harness's [PASS]/[FAIL], the data-driven cases assert a non-trivial COUNT of
// the thing they measured, so a predicate that silently matched nothing fails
// instead of passing vacuously.

#include "voxelcore/brickpack.h"

#include "voxelcore/brick.h"
#include "voxelcore/hash.h"
#include "vxctest.h"

#include <cstdio>
#include <vector>

using namespace vxc;

namespace {

constexpr int32_t kE = kMarchChunkEdgeVoxels; // 32

// --- byte comparison -------------------------------------------------------
//
// What voxel.March.VerifyBricks will do to a GPU readback, minus the rebase:
// descriptors, occupancy dwords and material dwords, exactly.
bool samePackedBytes(const ChunkBrickPack& a, const ChunkBrickPack& b) {
    if (a.descs != b.descs) return false;
    if (a.occ != b.occ) return false;
    if (a.mat != b.mat) return false;
    if (a.brickSolid != b.brickSolid) return false;
    return true;
}

// Materials for a chunk, from a flat 32^3 array. Lets a test build content in
// any order it likes and then hand the packer the finished volume — which is
// the whole point of the order-independence case.
struct ChunkVolume {
    std::vector<MaterialId> cells = std::vector<MaterialId>(kE * kE * kE, MAT_AIR);

    MaterialId& at(int32_t x, int32_t y, int32_t z) {
        return cells[static_cast<size_t>(x + kE * (y + kE * z))];
    }
    MaterialId operator()(int32_t x, int32_t y, int32_t z) const {
        return cells[static_cast<size_t>(x + kE * (y + kE * z))];
    }
};

// Round-trip every one of the 32,768 voxels through the packed form. Returns
// the solid count so callers can assert the case was not silently empty.
int32_t checkRoundTrip(const ChunkVolume& vol, const ChunkBrickPack& pack) {
    int32_t solid = 0;
    for (int32_t z = 0; z < kE; ++z)
        for (int32_t y = 0; y < kE; ++y)
            for (int32_t x = 0; x < kE; ++x) {
                const MaterialId want = vol(x, y, z);
                CHECK_EQ(decodeChunkVoxelCanonical(pack, x, y, z), want);
                if (want != MAT_AIR) ++solid;
            }
    return solid;
}

// The insertion-ordered palette vxc::Brick would build for a given visitation
// order. Used only to prove that order-dependence is real, so that the
// canonical test is measuring something that could fail.
enum class ScanOrder { XFast, ZFast, Reverse };

template <typename Fn>
void visitBrick(ScanOrder order, const Fn& fn) {
    switch (order) {
        case ScanOrder::XFast:
            for (int32_t z = 0; z < 8; ++z)
                for (int32_t y = 0; y < 8; ++y)
                    for (int32_t x = 0; x < 8; ++x) fn(x, y, z);
            return;
        case ScanOrder::ZFast:
            for (int32_t x = 0; x < 8; ++x)
                for (int32_t y = 0; y < 8; ++y)
                    for (int32_t z = 0; z < 8; ++z) fn(x, y, z);
            return;
        case ScanOrder::Reverse:
            for (int32_t z = 7; z >= 0; --z)
                for (int32_t y = 7; y >= 0; --y)
                    for (int32_t x = 7; x >= 0; --x) fn(x, y, z);
            return;
    }
}

template <typename ContentFn>
std::vector<MaterialId> firstSeenPalette(ScanOrder order, const ContentFn& content) {
    std::vector<MaterialId> pal;
    visitBrick(order, [&](int32_t x, int32_t y, int32_t z) {
        const MaterialId m = content(x, y, z);
        for (MaterialId p : pal)
            if (p == m) return;
        pal.push_back(m);
    });
    return pal;
}

// --- content generators ----------------------------------------------------

// Every material in the enum, scattered. Drives the wide-palette paths (bpp 8,
// no local palette) that real terrain reaches only rarely.
MaterialId denseMix(int32_t x, int32_t y, int32_t z) {
    return static_cast<MaterialId>(hash3(11, x, y, z, 3) % kMaterialCount);
}

// Terrain-shaped: a surface with soil over rock over bedrock, so a chunk
// contains uniform-air bricks above, uniform-solid bricks below and mixed
// bricks at the surface — the mixture the 67.1% collapse rate is measured over.
MaterialId terrainLike(int32_t x, int32_t y, int32_t z) {
    const int32_t h = 16 + static_cast<int32_t>(hash3(5, x / 4, y / 4, 0, 1) % 5);
    if (z > h) return MAT_AIR;
    if (z == h) return MAT_GRASS;
    if (z > h - 4) return MAT_TOPSOIL;
    if (z > h - 9) return MAT_ROCK;
    return MAT_BEDROCK;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Round-trip identity
// ---------------------------------------------------------------------------

VXC_TEST(brickpack_roundtrip_dense_mix) {
    ChunkVolume vol;
    for (int32_t z = 0; z < kE; ++z)
        for (int32_t y = 0; y < kE; ++y)
            for (int32_t x = 0; x < kE; ++x) vol.at(x, y, z) = denseMix(x, y, z);

    const ChunkBrickPack pack = packChunkBricksCanonical(vol);
    const int32_t solid = checkRoundTrip(vol, pack);

    // ~46/47 of everything is solid here; anything near zero means the
    // generator, not the packer, is what got tested.
    CHECK(solid > 30000);
    // Every brick holds far more than 16 materials, so every one is mixed, at
    // 8 bpp, with no local palette. That is the branch this case is here for.
    int32_t bpp8 = 0;
    for (const BrickDesc& d : pack.descs) {
        CHECK_EQ(d.kind(), static_cast<uint32_t>(kBrickMixed));
        if (d.bppCode() == 8u && !d.hasLocalPalette()) ++bpp8;
    }
    CHECK_EQ(bpp8, kMarchChunkBricks);
}

VXC_TEST(brickpack_roundtrip_terrain_like) {
    ChunkVolume vol;
    for (int32_t z = 0; z < kE; ++z)
        for (int32_t y = 0; y < kE; ++y)
            for (int32_t x = 0; x < kE; ++x) vol.at(x, y, z) = terrainLike(x, y, z);

    const ChunkBrickPack pack = packChunkBricksCanonical(vol);
    const int32_t solid = checkRoundTrip(vol, pack);
    CHECK(solid > 4000);

    // The interesting property of terrain: all three kinds occur in one chunk.
    // If any count is zero this case has stopped exercising what it claims to.
    int32_t air = 0, uniform = 0, mixed = 0;
    for (const BrickDesc& d : pack.descs) {
        if (d.kind() == kBrickUniformAir) ++air;
        else if (d.kind() == kBrickUniformSolid) ++uniform;
        else ++mixed;
    }
    CHECK(air > 0);
    CHECK(uniform > 0);
    CHECK(mixed > 0);
    CHECK_EQ(air + uniform + mixed, kMarchChunkBricks);
    // Only mixed bricks occupy occupancy dwords, 16 apiece, and they are
    // appended in ascending brick index with no gaps.
    CHECK_EQ(static_cast<int32_t>(pack.occ.size()), mixed * kMarchBrickOccWords);
}

VXC_TEST(brickpack_roundtrip_watermark_is_solid) {
    // MAT_WATERMARK is where this file's solidity predicate deliberately parts
    // company with isSolidForFluid: the marker exists to be SEEN, so a marcher
    // that treated it as air would delete the instrument in the exact mode
    // somebody turned on to use it.
    CHECK(isSolidForRender(MAT_WATERMARK));
    CHECK(!isSolidForFluid(MAT_WATERMARK)); // the fluid rule, unchanged
    CHECK(!isSolidForRender(MAT_AIR));

    ChunkVolume vol;
    int32_t marks = 0;
    for (int32_t z = 0; z < kE; ++z)
        for (int32_t y = 0; y < kE; ++y)
            for (int32_t x = 0; x < kE; ++x)
                if ((x + y + z) % 3 == 0) {
                    vol.at(x, y, z) = MAT_WATERMARK;
                    ++marks;
                }

    const ChunkBrickPack pack = packChunkBricksCanonical(vol);
    CHECK(marks > 10000);
    CHECK_EQ(checkRoundTrip(vol, pack), marks);
    // One solid material and some air: the bppCode 0 shape, palette of one and
    // no payload at all.
    for (const BrickDesc& d : pack.descs) {
        CHECK_EQ(d.kind(), static_cast<uint32_t>(kBrickMixed));
        CHECK_EQ(d.bppCode(), 0u);
        CHECK(d.hasLocalPalette());
    }
}

// ---------------------------------------------------------------------------
// 2. Palette-order independence — the property the whole format exists for
// ---------------------------------------------------------------------------

VXC_TEST(brickpack_palette_order_independence) {
    // Content chosen so the first material seen genuinely differs by scan
    // order: the value depends on all three axes.
    const auto content = [](int32_t x, int32_t y, int32_t z) -> MaterialId {
        const uint64_t h = hash3(23, x, y, z, 9) % 6;
        if (h == 0) return MAT_AIR;
        return static_cast<MaterialId>(MAT_BEDROCK + h); // 2..6 distinct solids
    };

    // PREMISE CHECK. Insertion-ordered palettes — what vxc::Brick builds — are
    // different objects under different visitation orders. Without this the
    // test below could pass while measuring nothing.
    const std::vector<MaterialId> palA = firstSeenPalette(ScanOrder::XFast, content);
    const std::vector<MaterialId> palB = firstSeenPalette(ScanOrder::ZFast, content);
    const std::vector<MaterialId> palC = firstSeenPalette(ScanOrder::Reverse, content);
    CHECK(palA.size() >= 5); // air plus the solids
    CHECK(palA != palB);
    CHECK(palA != palC);

    // Now build the same brick through vxc::Brick three times, writing the
    // cells in those three orders, and pack each. The container's internal
    // palette differs every time; the packed bytes must not.
    ChunkBrickPack packs[3];
    const ScanOrder orders[3] = {ScanOrder::XFast, ScanOrder::ZFast, ScanOrder::Reverse};
    for (int i = 0; i < 3; ++i) {
        Brick<8> b;
        visitBrick(orders[i], [&](int32_t x, int32_t y, int32_t z) {
            b.set(x, y, z, content(x, y, z));
        });
        // Brick 0 of an otherwise empty chunk.
        packs[i] = packChunkBricksCanonical([&](int32_t x, int32_t y, int32_t z) -> MaterialId {
            if (x >= 8 || y >= 8 || z >= 8) return MAT_AIR;
            return b.get(x, y, z);
        });
    }
    CHECK(samePackedBytes(packs[0], packs[1]));
    CHECK(samePackedBytes(packs[0], packs[2]));

    // And identical to packing straight from the content function, i.e. the
    // packing depends on nothing about the container it was read out of.
    const ChunkBrickPack direct =
        packChunkBricksCanonical([&](int32_t x, int32_t y, int32_t z) -> MaterialId {
            if (x >= 8 || y >= 8 || z >= 8) return MAT_AIR;
            return content(x, y, z);
        });
    CHECK(samePackedBytes(packs[0], direct));

    // SECOND PREMISE CHECK. A CPU packer reads content in its own fixed scan,
    // so "first seen" is deterministic for IT — the reason first-seen is
    // unacceptable is that a GPU producer has no such scan to share. That makes
    // the ascending rule the part a CPU test can actually hold, and it is only
    // worth holding if the two orders differ here. They do: the first-seen
    // solid palette in the packer's own x-fastest scan is NOT ascending, so a
    // first-seen packer would emit different bytes and fail the check below.
    std::vector<MaterialId> firstSeenSolids;
    for (MaterialId m : palA)
        if (m != MAT_AIR) firstSeenSolids.push_back(m);
    bool firstSeenIsAscending = true;
    for (size_t i = 1; i < firstSeenSolids.size(); ++i)
        if (firstSeenSolids[i] < firstSeenSolids[i - 1]) firstSeenIsAscending = false;
    CHECK(!firstSeenIsAscending);

    // The local palette must be ASCENDING, which is the canonical rule stated
    // as bytes rather than as a property of the output.
    const BrickDesc& d = direct.descs[0];
    CHECK_EQ(d.kind(), static_cast<uint32_t>(kBrickMixed));
    CHECK(d.hasLocalPalette());
    const uint32_t* palWords = direct.mat.data() + d.matDwordOffset();
    int32_t entries = 0;
    MaterialId prev = 0;
    for (int32_t i = 0; i < kMarchLocalPaletteEntries; ++i) {
        const MaterialId m =
            static_cast<MaterialId>((palWords[i / 4] >> (8 * (i % 4))) & 0xFFu);
        if (m == MAT_AIR) break; // zero fill past the end; air is never a slot
        CHECK(m > prev);
        prev = m;
        ++entries;
    }
    CHECK_EQ(entries, 5);
}

// ---------------------------------------------------------------------------
// 3. Uniform collapse, both ways, and no payload
// ---------------------------------------------------------------------------

VXC_TEST(brickpack_uniform_air_collapses) {
    const ChunkBrickPack pack =
        packChunkBricksCanonical([](int32_t, int32_t, int32_t) { return MAT_AIR; });

    for (const BrickDesc& d : pack.descs) {
        CHECK_EQ(d.kind(), static_cast<uint32_t>(kBrickUniformAir));
        // An all-air descriptor is eight zero bytes, so a cleared pool reads as
        // empty world rather than as garbage.
        CHECK_EQ(d.OccWord, 0u);
        CHECK_EQ(d.MatWord, 0u);
        CHECK(!d.hasLocalPalette());
    }
    CHECK(pack.occ.empty());
    CHECK(pack.mat.empty());
    CHECK_EQ(pack.brickSolid, uint64_t(0));
    CHECK(!pack.anySolid);
    CHECK(!pack.allSolid);
    // 64 descriptors and nothing else: an over-admitted all-air chunk costs
    // 512 bytes, which is the whole reason vertical over-admission is routed
    // around rather than fixed (plan §9).
    CHECK_EQ(pack.residentBytes(), int64_t(512));
    for (int32_t i = 0; i < kMarchChunkBricks; ++i)
        CHECK_EQ(pack.brickCoarse[static_cast<size_t>(i)], uint64_t(0));
}

VXC_TEST(brickpack_uniform_solid_collapses) {
    const ChunkBrickPack pack =
        packChunkBricksCanonical([](int32_t, int32_t, int32_t) { return MAT_ROCK; });

    for (const BrickDesc& d : pack.descs) {
        CHECK_EQ(d.kind(), static_cast<uint32_t>(kBrickUniformSolid));
        CHECK_EQ(d.uniformMaterial(), static_cast<MaterialId>(MAT_ROCK));
        // No payload, no local palette, and the offset fields are unused —
        // MatWord carries the material and nothing else.
        CHECK(!d.hasLocalPalette());
        CHECK_EQ(d.occDwordOffset(), 0u);
        CHECK_EQ(d.MatWord, static_cast<uint32_t>(MAT_ROCK));
    }
    CHECK(pack.occ.empty());
    CHECK(pack.mat.empty());
    CHECK_EQ(pack.brickSolid, ~uint64_t(0));
    CHECK(pack.anySolid);
    CHECK(pack.allSolid);
    CHECK_EQ(pack.residentBytes(), int64_t(512));
    for (int32_t i = 0; i < kMarchChunkBricks; ++i)
        CHECK_EQ(pack.brickCoarse[static_cast<size_t>(i)], ~uint64_t(0));

    // Round-trip: a collapsed brick still answers for all 512 of its cells.
    int32_t checked = 0;
    for (int32_t z = 0; z < kE; ++z)
        for (int32_t y = 0; y < kE; ++y)
            for (int32_t x = 0; x < kE; ++x) {
                CHECK_EQ(decodeChunkVoxelCanonical(pack, x, y, z),
                         static_cast<MaterialId>(MAT_ROCK));
                ++checked;
            }
    CHECK_EQ(checked, 32768);
}

VXC_TEST(brickpack_all_solid_two_materials_does_not_collapse) {
    // The near miss: every cell solid, but not one material. tryCollapse's
    // predicate is "all cells hold ONE palette index", not "no air", and a
    // packer that tested only occupancy would silently paint the brick a single
    // colour. It must stay MIXED, with a full payload.
    const ChunkBrickPack pack =
        packChunkBricksCanonical([](int32_t x, int32_t, int32_t) -> MaterialId {
            return (x % 2 == 0) ? MAT_ROCK : MAT_BEDROCK;
        });

    for (const BrickDesc& d : pack.descs) {
        CHECK_EQ(d.kind(), static_cast<uint32_t>(kBrickMixed));
        CHECK_EQ(d.bppCode(), 1u); // two materials
        CHECK(d.hasLocalPalette());
    }
    CHECK(pack.allSolid); // every voxel non-air, so the chunk flag is still set
    CHECK_EQ(static_cast<int32_t>(pack.occ.size()),
             kMarchChunkBricks * kMarchBrickOccWords);
    int32_t checked = 0;
    for (int32_t z = 0; z < kE; ++z)
        for (int32_t y = 0; y < kE; ++y)
            for (int32_t x = 0; x < kE; ++x) {
                CHECK_EQ(decodeChunkVoxelCanonical(pack, x, y, z),
                         static_cast<MaterialId>((x % 2 == 0) ? MAT_ROCK : MAT_BEDROCK));
                ++checked;
            }
    CHECK_EQ(checked, 32768);
}

// ---------------------------------------------------------------------------
// 4. bpp boundaries
// ---------------------------------------------------------------------------

VXC_TEST(brickpack_bpp_ladder_thresholds) {
    // The pure function first, at every boundary that exists.
    CHECK_EQ(brickBppCodeFor(1), 0u);
    CHECK_EQ(brickBppCodeFor(2), 1u);
    CHECK_EQ(brickBppCodeFor(3), 2u);
    CHECK_EQ(brickBppCodeFor(4), 2u);
    CHECK_EQ(brickBppCodeFor(5), 4u);  // 2 -> 4
    CHECK_EQ(brickBppCodeFor(16), 4u); // the local palette holds exactly 16
    CHECK_EQ(brickBppCodeFor(17), 8u); // 4 -> 8, direct global ids
    CHECK_EQ(brickBppCodeFor(47), 8u);
}

VXC_TEST(brickpack_bpp_boundaries_end_to_end) {
    // A brick with exactly N distinct solid materials plus air, for every N the
    // ladder turns on. Checks the descriptor, the emitted byte count and the
    // decode, because the three can disagree independently.
    struct Case {
        int32_t materials;
        uint32_t bpp;
        bool localPalette;
    };
    const Case cases[] = {
        {1, 0u, true},  {2, 1u, true},  {4, 2u, true},
        {5, 4u, true},  {16, 4u, true}, {17, 8u, false},
    };

    int32_t ran = 0;
    for (const Case& c : cases) {
        ChunkVolume vol;
        int32_t solid = 0;
        std::vector<bool> present(static_cast<size_t>(c.materials), false);
        for (int32_t z = 0; z < 8; ++z)
            for (int32_t y = 0; y < 8; ++y)
                for (int32_t x = 0; x < 8; ++x) {
                    const int32_t i = Brick<8>::cellIndex(x, y, z);
                    // Air on every 9th cell, so the brick stays mixed. NINE and
                    // not eight: with an air stride of 8, slots 0 and 8 of a
                    // 16-material brick never get a cell (every i divisible by
                    // 16 is also divisible by 8), and the case would quietly
                    // test 14 materials while claiming 16.
                    if (i % 9 == 0) continue;
                    const int32_t slot = i % c.materials;
                    present[static_cast<size_t>(slot)] = true;
                    // Ids 1..N: MAT_BEDROCK upward, which at N=17 walks past
                    // MAT_WATERMARK and out into the asset materials.
                    vol.at(x, y, z) = static_cast<MaterialId>(1 + slot);
                    ++solid;
                }
        for (bool p : present) CHECK(p); // all N materials really landed
        CHECK_EQ(solid, 455);            // 512 - 57 air cells

        const ChunkBrickPack pack = packChunkBricksCanonical(vol);
        const BrickDesc& d = pack.descs[0];
        CHECK_EQ(d.kind(), static_cast<uint32_t>(kBrickMixed));
        CHECK_EQ(d.bppCode(), c.bpp);
        CHECK_EQ(d.hasLocalPalette(), c.localPalette);

        // Emitted material dwords: the fixed 4-dword local palette when there
        // is one, plus ceil(solid * bpp / 32) of payload.
        const int32_t wantWords =
            (c.localPalette ? kMarchLocalPaletteWords : 0) +
            brickMatPayloadWords(solid, c.bpp);
        CHECK_EQ(static_cast<int32_t>(pack.mat.size()), wantWords);
        CHECK_EQ(static_cast<int32_t>(pack.occ.size()), kMarchBrickOccWords);

        CHECK_EQ(checkRoundTrip(vol, pack), solid);
        ++ran;
    }
    CHECK_EQ(ran, 6);
}

// ---------------------------------------------------------------------------
// 5. Occupancy compaction
// ---------------------------------------------------------------------------

VXC_TEST(brickpack_occupancy_compaction_sparse) {
    // A deliberately SPARSE brick. Rank and cell index coincide on dense
    // content, so this is the only shape that can tell a correct packer from
    // one that indexed materials by cell.
    ChunkVolume vol;
    std::vector<MaterialId> inScanOrder; // the nth solid voxel's material
    int32_t rankDiffersFromCell = 0;
    for (int32_t z = 0; z < 8; ++z)
        for (int32_t y = 0; y < 8; ++y)
            for (int32_t x = 0; x < 8; ++x) {
                const int32_t i = Brick<8>::cellIndex(x, y, z);
                if ((x * 7 + y * 13 + z * 23) % 11 != 0) continue;
                const MaterialId m = static_cast<MaterialId>(MAT_BEDROCK + (i % 5));
                vol.at(x, y, z) = m;
                if (static_cast<int32_t>(inScanOrder.size()) != i) ++rankDiffersFromCell;
                inScanOrder.push_back(m);
            }
    const int32_t solid = static_cast<int32_t>(inScanOrder.size());

    // Non-vacuity, and it is the specific non-vacuity this case needs: the
    // brick must be sparse enough that rank and cell index disagree almost
    // everywhere. Otherwise a cell-indexed packer would pass.
    CHECK(solid > 20);
    CHECK(solid < 200);
    CHECK(rankDiffersFromCell > solid - 2);

    const ChunkBrickPack pack = packChunkBricksCanonical(vol);
    const BrickDesc& d = pack.descs[0];
    CHECK_EQ(d.kind(), static_cast<uint32_t>(kBrickMixed));
    CHECK_EQ(d.bppCode(), 4u); // five materials
    CHECK(d.hasLocalPalette());

    // Read the payload DIRECTLY, slot by slot, without touching
    // solidRankBelow. A decoder that shares a broken rank function with the
    // packer round-trips perfectly, so the round-trip alone proves nothing
    // here; this does.
    const uint32_t* matBlock = pack.mat.data() + d.matDwordOffset();
    const uint32_t* payload = matBlock + kMarchLocalPaletteWords;
    for (int32_t n = 0; n < solid; ++n) {
        const uint32_t slot = (payload[n / 8] >> (4 * (n % 8))) & 0xFu;
        const MaterialId got =
            static_cast<MaterialId>((matBlock[slot / 4] >> (8 * (slot % 4))) & 0xFFu);
        CHECK_EQ(got, inScanOrder[static_cast<size_t>(n)]);
    }
    // The payload is exactly as long as there are solid voxels — no slot is
    // spent on air.
    CHECK_EQ(static_cast<int32_t>(pack.mat.size()),
             kMarchLocalPaletteWords + brickMatPayloadWords(solid, 4u));

    CHECK_EQ(checkRoundTrip(vol, pack), solid);
}

VXC_TEST(brickpack_solid_rank_matches_a_linear_count) {
    // solidRankBelow against the obvious O(n) definition, over a pattern with
    // set bits in every word.
    uint32_t occ[kMarchBrickOccWords] = {};
    bool bits[512] = {};
    int32_t set = 0;
    for (int32_t i = 0; i < 512; ++i)
        if ((i * 37) % 7 < 3) {
            bits[i] = true;
            occ[fluidBrickWordOf(i)] |= 1u << fluidBrickShiftOf(i);
            ++set;
        }
    CHECK(set > 150);

    int32_t running = 0;
    for (int32_t i = 0; i < 512; ++i) {
        CHECK_EQ(solidRankBelow(occ, i), running);
        if (bits[i]) ++running;
    }
    CHECK_EQ(running, set);
}

// ---------------------------------------------------------------------------
// 6. Acceleration masks
// ---------------------------------------------------------------------------

VXC_TEST(brickpack_l1_brick_mask) {
    // One solid voxel in each of a handful of bricks, chosen so an index built
    // z-major instead of x-major would produce a different mask.
    const int32_t picks[][3] = {{0, 0, 0}, {1, 0, 0}, {0, 2, 0}, {0, 0, 3}, {3, 3, 3}};
    ChunkVolume vol;
    uint64_t want = 0;
    for (const auto& p : picks) {
        vol.at(p[0] * 8 + 1, p[1] * 8 + 2, p[2] * 8 + 3) = MAT_SAND;
        want |= uint64_t(1) << chunkBrickIndex(p[0], p[1], p[2]);
    }

    const ChunkBrickPack pack = packChunkBricksCanonical(vol);
    CHECK_EQ(pack.brickSolid, want);
    CHECK(pack.anySolid);
    CHECK(!pack.allSolid);
    CHECK_EQ(std::popcount(pack.brickSolid), 5);

    // Every set bit is a brick with a descriptor that is not uniform air, and
    // every clear bit is one that is. That equivalence is what lets the marcher
    // skip on the bit alone, without a fetch.
    for (int32_t i = 0; i < kMarchChunkBricks; ++i) {
        const bool bit = ((pack.brickSolid >> i) & 1u) != 0u;
        const bool empty = pack.descs[static_cast<size_t>(i)].kind() == kBrickUniformAir;
        CHECK_EQ(bit, !empty);
    }
}

VXC_TEST(brickpack_coarse4_skip_mask) {
    // Solid only in the 2x2x2 group at (cx,cy,cz) = (1,2,3) of brick 0.
    ChunkVolume vol;
    vol.at(2 * 1 + 1, 2 * 2, 2 * 3 + 1) = MAT_GRAVEL;
    const ChunkBrickPack pack = packChunkBricksCanonical(vol);
    CHECK_EQ(pack.brickCoarse[0], uint64_t(1) << (1 + 4 * (2 + 4 * 3)));
    for (int32_t i = 1; i < kMarchChunkBricks; ++i)
        CHECK_EQ(pack.brickCoarse[static_cast<size_t>(i)], uint64_t(0));

    // And against terrain, where the mask must agree cell for cell with the
    // occupancy it was reduced from.
    ChunkVolume terr;
    for (int32_t z = 0; z < kE; ++z)
        for (int32_t y = 0; y < kE; ++y)
            for (int32_t x = 0; x < kE; ++x) terr.at(x, y, z) = terrainLike(x, y, z);
    const ChunkBrickPack tp = packChunkBricksCanonical(terr);
    int32_t setGroups = 0, clearGroups = 0;
    for (int32_t b = 0; b < kMarchChunkBricks; ++b) {
        const int32_t bx = b % 4, by = (b / 4) % 4, bz = b / 16;
        for (int32_t cz = 0; cz < 4; ++cz)
            for (int32_t cy = 0; cy < 4; ++cy)
                for (int32_t cx = 0; cx < 4; ++cx) {
                    bool any = false;
                    for (int32_t dz = 0; dz < 2; ++dz)
                        for (int32_t dy = 0; dy < 2; ++dy)
                            for (int32_t dx = 0; dx < 2; ++dx)
                                any = any || terr(bx * 8 + cx * 2 + dx, by * 8 + cy * 2 + dy,
                                                  bz * 8 + cz * 2 + dz) != MAT_AIR;
                    const bool bit =
                        ((tp.brickCoarse[static_cast<size_t>(b)] >> (cx + 4 * (cy + 4 * cz))) &
                         1u) != 0u;
                    CHECK_EQ(bit, any);
                    if (any) ++setGroups; else ++clearGroups;
                }
    }
    // Both halves present, or the comparison above was trivially true.
    CHECK(setGroups > 100);
    CHECK(clearGroups > 100);
}

// ---------------------------------------------------------------------------
// 7. The descriptor itself, and the size the format is priced at
// ---------------------------------------------------------------------------

VXC_TEST(brickpack_desc_fields_do_not_collide) {
    // The offset field is 28 bits and the flags live above it. A pool large
    // enough to use the top of that range must not bleed into kind.
    BrickDesc d;
    d.OccWord = (kBrickMixed << kBrickKindShift) | kBrickOffsetMask |
                (1u << kBrickHasPaletteBit);
    d.MatWord = (8u << kBrickBppShift) | kBrickOffsetMask;
    CHECK_EQ(d.kind(), static_cast<uint32_t>(kBrickMixed));
    CHECK(d.hasLocalPalette());
    CHECK_EQ(d.occDwordOffset(), kBrickOffsetMask);
    CHECK_EQ(d.bppCode(), 8u);
    CHECK_EQ(d.matDwordOffset(), kBrickOffsetMask);
    // 2^28 dwords is 1 GiB per array, which is the headroom the 28-bit field
    // buys and the number a pool sizing decision has to be checked against.
    CHECK_EQ(kBrickOffsetMask + 1u, 268435456u);
}

VXC_TEST(brickpack_typical_brick_bytes) {
    // The census's typical mixed brick: three materials, about half the cells
    // solid. This is the number docs/brick-volume-format.md §4 prices the
    // format at, and it disagrees with the file — §4's own worked example says
    // 140 B (a 4 B palette), while its normative sentence says a fixed 16 B
    // local palette, which gives 152 B. This test pins what the code does.
    ChunkVolume vol;
    int32_t solid = 0;
    for (int32_t z = 0; z < 8; ++z)
        for (int32_t y = 0; y < 8; ++y)
            for (int32_t x = 0; x < 8; ++x) {
                if (z >= 4) continue; // top half air -> 256 solid
                vol.at(x, y, z) = static_cast<MaterialId>(MAT_BEDROCK + ((x + y) % 3));
                ++solid;
            }
    CHECK_EQ(solid, 256);

    const ChunkBrickPack pack = packChunkBricksCanonical(vol);
    const BrickDesc& d = pack.descs[0];
    CHECK_EQ(d.bppCode(), 2u);
    CHECK(d.hasLocalPalette());

    const int64_t descBytes = 8;
    const int64_t occBytes = int64_t(pack.occ.size()) * 4;
    const int64_t matBytes = int64_t(pack.mat.size()) * 4;
    const int64_t perBrick = descBytes + occBytes + matBytes;
    std::printf("  [info] typical 3-material mixed brick: %lld B desc + %lld B occ + "
                "%lld B mat (16 B palette + %lld B payload) = %lld B\n",
                static_cast<long long>(descBytes), static_cast<long long>(occBytes),
                static_cast<long long>(matBytes),
                static_cast<long long>(matBytes - kMarchLocalPaletteWords * 4),
                static_cast<long long>(perBrick));
    CHECK_EQ(occBytes, int64_t(64));
    CHECK_EQ(matBytes, int64_t(16 + 64)); // 16 B palette + 256 * 2 bits
    CHECK_EQ(perBrick, int64_t(152));
    // The whole chunk: 64 descriptors plus this one brick's payload.
    CHECK_EQ(pack.residentBytes(), int64_t(512 + 64 + 80));
    CHECK_EQ(checkRoundTrip(vol, pack), solid);
}
