// Amplifier v0: determinism, stratigraphy ordering, golden digest (plan §5
// task 3). The golden digest is the cross-compiler determinism proxy for the
// M0 NVIDIA-vs-AMD gate: gcc and clang CI builds must both match it.

#include "voxelcore/amplifier.h"
#include "voxelcore/biome.h"
#include "voxelcore/generator.h"
#include <cstdio>


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
            // Biome surface materials (M4, voxelcore/biome.h); MAT_SNOW is a
            // retired v0 id no longer produced by classifyBiome.
            CHECK(col.surfaceMat == MAT_TOPSOIL || col.surfaceMat == MAT_SAND ||
                  col.surfaceMat == MAT_GRASS || col.surfaceMat == MAT_JUNGLE_SOIL ||
                  col.surfaceMat == MAT_SAVANNA_GRASS || col.surfaceMat == MAT_PODZOL ||
                  col.surfaceMat == MAT_PERMAFROST || col.surfaceMat == MAT_MUD ||
                  col.surfaceMat == MAT_ROCK);

            // Walking down the column: air above surface, then the layer
            // sequence, never air below the surface. This is the STRATIGRAPHY
            // (Amplifier::stratigraphyAt) — since the M4 cave pass,
            // materialAt() also carves tunnels out of it, so "no air below the
            // surface" is no longer true of the full material function and
            // holds only of the layer model this test is about. The cave
            // pass's own invariants live in test_caves.cpp.
            const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            CHECK_EQ(Amplifier::stratigraphyAt(col, topVz + 1), MAT_AIR);
            CHECK(Amplifier::stratigraphyAt(col, topVz) != MAT_AIR);
            // The surface shell itself is never carved (roof clamp).
            CHECK(Amplifier::materialAt(col, topVz) != MAT_AIR);
            bool leftTopLayer = false;
            for (int64_t vz = topVz; vz > topVz - 1200; vz -= 7) {
                const MaterialId m = Amplifier::stratigraphyAt(col, vz);
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
    // GOLDEN(amplifier_columns) — kWorldGenVersion 5: bedrock top moved from a
    // 40-60 m band to a 180-220 m one (200 m mean, Matt's decision). This
    // digest covers surfaceMm/topsoilMm/subsoilMm/bedrockDepthMm/surfaceMat, of
    // which ONLY bedrockDepthMm moved — the cave and cavern passes are not part
    // of it, so the v5 cavern fold-in does not touch this value.
    // (was 0x81785278E4DFCF67 at v3/v4, 0x73B43CAE621CA286 at v2)
    CHECK_EQ(d.h, 0xA29A7A767DC1543Bull);
}

// --- C4: the cavern pass is actually wired into the amplifier ---------------

// Every pre-existing golden (mips_chain, both bench digests) only ever
// voxelises SURFACE-SHELL bricks — bricks containing some column's topmost
// voxel — and a cavern never comes within 12 m of the surface. So none of them
// moves when caverns are folded in, and none of them would notice if the
// fold-in were silently dropped. These two tests are the ones that do.

VXC_TEST(amplifier_folds_caverns_into_materialAt) {
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);

    int64_t columnsWithCavern = 0, cavernAirVoxels = 0, cavernOnlyAirVoxels = 0;
    int64_t bedrockVoxelsChecked = 0;
    int64_t thinnestCavernRoofMm = 1LL << 60;
    int64_t deepestCavernVoxelMm = 0;

    for (int64_t x = -4000; x < 4000; x += 23) {
        for (int64_t y = -4000; y < 4000; y += 29) {
            const ColumnSample col = amp.column(x, y);
            if (col.cavern.count == 0) continue;
            ++columnsWithCavern;

            // Walk 260 m down: past the caverns, through MAT_ROCK, and into
            // the 180-220 m bedrock floor.
            const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            for (int64_t vz = topVz; vz > topVz - 2600; --vz) {
                const MaterialId strat = Amplifier::stratigraphyAt(col, vz);
                const MaterialId m = Amplifier::materialAt(col, vz);
                if (strat == MAT_BEDROCK) {
                    // The world's floor is never turned into air by any pass.
                    CHECK_EQ(m, MAT_BEDROCK);
                    ++bedrockVoxelsChecked;
                    continue;
                }
                if (strat == MAT_AIR || m != MAT_AIR) continue;

                // materialAt turned solid stratigraphy into air: exactly one of
                // the two passes must own that decision.
                const bool byCave =
                    caveCarveAt(col.cave, col.surfaceMm, col.bedrockDepthMm, vz);
                const bool byCavern =
                    cavernCarveAt(col.cavern, col.surfaceMm, col.bedrockDepthMm, vz);
                CHECK(byCave || byCavern);
                if (!byCavern) continue;
                ++cavernAirVoxels;
                const int64_t depthMm =
                    int64_t(col.surfaceMm) - (vz * kVoxelSizeMm + kVoxelSizeMm / 2);
                if (depthMm < thinnestCavernRoofMm) thinnestCavernRoofMm = depthMm;
                if (depthMm > deepestCavernVoxelMm) deepestCavernVoxelMm = depthMm;
                // The fold-in claim proper: without the cavern pass this voxel
                // would still be solid, so caverns are genuinely ADDING void.
                if (!byCave) ++cavernOnlyAirVoxels;
            }
        }
    }

    std::printf("    [amplifier] cavern fold-in: %lld columns over a cavern, %lld voxels "
                "carved by the cavern pass (%lld of them cavern-ONLY), roof cover "
                "%lld..%lld mm; %lld bedrock voxels all refused\n",
                static_cast<long long>(columnsWithCavern),
                static_cast<long long>(cavernAirVoxels),
                static_cast<long long>(cavernOnlyAirVoxels),
                static_cast<long long>(thinnestCavernRoofMm),
                static_cast<long long>(deepestCavernVoxelMm),
                static_cast<long long>(bedrockVoxelsChecked));

    CHECK(columnsWithCavern > 0);
    CHECK(cavernAirVoxels > 0);
    CHECK(cavernOnlyAirVoxels > 0); // caverns add void the tunnel pass does not
    CHECK(bedrockVoxelsChecked > 0);
    CHECK(thinnestCavernRoofMm >= kCaveRoofMinMm);
    // Multi-storey depth is the whole point of the 200 m bedrock move: the
    // chain must reach far past where the old 40-60 m floor would have cut it.
    CHECK(deepestCavernVoxelMm > 60000);
}

VXC_TEST(amplifier_deep_column_golden_digest) {
    // GOLDEN(amplifier_deep_materials) — new at kWorldGenVersion 5. Digests
    // Amplifier::materialAt down 260 m over a 800 m-wide sparse grid, so unlike
    // every other worldgen golden it actually covers the cave pass, the cavern
    // pass and the bedrock boundary as the amplifier composes them, rather than
    // just the surface shell.
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    Digest d;
    int64_t cavernColumns = 0;
    for (int64_t y = -4000; y < 4000; y += 211) {
        for (int64_t x = -4000; x < 4000; x += 211) {
            const ColumnSample col = amp.column(x, y);
            if (col.cavern.count > 0) ++cavernColumns;
            d.u32(static_cast<uint32_t>(col.cavern.count));
            d.u32(static_cast<uint32_t>(col.cavern.floodZMm));
            const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            for (int64_t vz = topVz; vz > topVz - 2600; vz -= 7)
                d.u8(Amplifier::materialAt(col, vz));
        }
    }
    // The golden is only worth pinning if the sampled set genuinely reaches
    // caverns; assert that rather than hope for it.
    CHECK(cavernColumns > 0);
    std::printf("    [amplifier] deep golden covers %lld cavern columns\n",
                static_cast<long long>(cavernColumns));
    CHECK_EQ(d.h, 0xF88B88DB9D9341AAull);
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
