// Amplifier v0: determinism, stratigraphy ordering, golden digest (plan §5
// task 3). The golden digest is the cross-compiler determinism proxy for the
// M0 NVIDIA-vs-AMD gate: gcc and clang CI builds must both match it.

#include "voxelcore/amplifier.h"
#include "voxelcore/generator.h"
#include "vxctest.h"

using namespace vxc;

namespace {
constexpr uint64_t kSeed = 20260719;
}

VXC_TEST(amplifier_is_deterministic) {
    SyntheticTileSampler tilesA(kSeed), tilesB(kSeed);
    Amplifier a(kSeed, tilesA), b(kSeed, tilesB);
    for (int64_t x = -300; x <= 300; x += 37)
        for (int64_t y = -300; y <= 300; y += 53) {
            const ColumnSample ca = a.column(x, y), cb = b.column(x, y);
            CHECK_EQ(ca.surfaceMm, cb.surfaceMm);
            CHECK_EQ(ca.topsoilMm, cb.topsoilMm);
            CHECK_EQ(ca.subsoilMm, cb.subsoilMm);
            CHECK_EQ(ca.bedrockDepthMm, cb.bedrockDepthMm);
            CHECK_EQ(ca.surfaceMat, cb.surfaceMat);
        }
}

VXC_TEST(amplifier_stratigraphy_ordering) {
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    for (int64_t x = -2000; x <= 2000; x += 419)
        for (int64_t y = -2000; y <= 2000; y += 611) {
            const ColumnSample col = amp.column(x, y);
            CHECK(col.topsoilMm >= 0);
            CHECK(col.subsoilMm >= 0);
            CHECK(col.bedrockDepthMm > col.topsoilMm + col.subsoilMm);
            CHECK(col.surfaceMat == MAT_TOPSOIL || col.surfaceMat == MAT_SAND ||
                  col.surfaceMat == MAT_SNOW);

            // Walking down the column: air above surface, then the layer
            // sequence, never air below the surface (implicit-solid doctrine).
            const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            CHECK_EQ(Amplifier::materialAt(col, topVz + 1), MAT_AIR);
            CHECK(Amplifier::materialAt(col, topVz) != MAT_AIR);
            bool leftTopLayer = false;
            for (int64_t vz = topVz; vz > topVz - 1200; vz -= 7) {
                const MaterialId m = Amplifier::materialAt(col, vz);
                CHECK(m != MAT_AIR);
                if (m != col.surfaceMat) leftTopLayer = true;
                // materialAt is depth-ordered: once below the top layer, the
                // surface material never reappears.
                if (leftTopLayer) CHECK(m != col.surfaceMat);
            }
            // 120m below any surface must be bedrock or rock.
            const MaterialId deep = Amplifier::materialAt(col, topVz - 1200);
            CHECK(deep == MAT_ROCK || deep == MAT_BEDROCK);
        }
}

VXC_TEST(amplifier_golden_digest) {
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    Digest d;
    for (int64_t y = -64; y < 64; y += 3)
        for (int64_t x = -64; x < 64; x += 3) {
            const ColumnSample col = amp.column(x, y);
            d.u32(static_cast<uint32_t>(col.surfaceMm));
            d.u32(static_cast<uint32_t>(col.topsoilMm));
            d.u32(static_cast<uint32_t>(col.subsoilMm));
            d.u32(static_cast<uint32_t>(col.bedrockDepthMm));
            d.u8(col.surfaceMat);
        }
    CHECK_EQ(d.h, 0xA7CFA118B16CE0DFull); // GOLDEN(amplifier_columns)
}

VXC_TEST(generated_brick_matches_pointwise_queries) {
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    GeneratedWorld<16> gen(amp);
    // A brick straddling the surface near the origin.
    const auto grid = gen.columns(0, 0);
    int32_t bzMin, bzMax;
    gen.surfaceBrickRange(grid, bzMin, bzMax);
    CHECK(bzMin <= bzMax);
    const BrickKey key{0, 0, bzMin};
    const Brick<16> brick = gen.makeBrick(key, grid);
    for (int z = 0; z < 16; ++z)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                CHECK_EQ(brick.get(x, y, z),
                         gen.materialAt(x, y, int64_t(key.z) * 16 + z));
    // The brick at the surface must be mixed (contains both air and solid)
    // for at least one of the range ends.
    const Brick<16> top = gen.makeBrick(BrickKey{0, 0, bzMax}, grid);
    CHECK(top.solidCount() < size_t(16 * 16 * 16));
    CHECK(brick.solidCount() > 0);
}
