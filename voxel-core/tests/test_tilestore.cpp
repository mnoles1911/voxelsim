// Tile decoding + sampling (plan §3.1 step 2, §3.4 ITerrainSource): a
// cross-language fixture test. The fixture file is a byte-for-byte tile
// produced by terrain-service's Python synthetic provider + tile_codec.encode
// (seed=1, x=0, y=0, scale=1) — see voxel-core/tests/fixtures/README-less
// provenance note below. Pinned pixel values were read once from the same
// Python invocation that generated the fixture and hardcoded here (golden
// pattern, like amplifier_golden_digest in test_amplifier.cpp).
//
// Regenerate the fixture with (from terrain-service/):
//   python -c "
//   from terrain_service.providers.synthetic import SyntheticProvider
//   from terrain_service import tile_codec
//   data = tile_codec.encode(SyntheticProvider().generate(1, 0, 0, 1))
//   open('../voxel-core/tests/fixtures/tile_s1_seed1_0_0.vxtl', 'wb').write(data)"

#include "voxelcore/tilestore.h"

#include <filesystem>

#include "voxelcore/amplifier.h"
#include "vxctest.h"

using namespace vxc;

namespace {

std::filesystem::path fixturePath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "tile_s1_seed1_0_0.vxtl";
}

} // namespace

VXC_TEST(tiledata_parses_fixture_header_and_pinned_pixels) {
    auto bytes = readFileBytes(fixturePath());
    CHECK(bytes.has_value());
    CHECK_EQ(bytes->size(), size_t(25 + 2 * 512 * 512 + 4 * 512 * 512)); // header + elevation + climate

    auto tile = TileData::parse(bytes->data(), bytes->size());
    CHECK(tile.has_value());
    CHECK_EQ(tile->seed, uint64_t(1));
    CHECK_EQ(tile->x, 0);
    CHECK_EQ(tile->y, 0);
    CHECK_EQ(int(tile->scale), 1);
    CHECK_EQ(tile->elevation.size(), size_t(TileData::kPixelCount));
    for (const auto& plane : tile->climate) CHECK_EQ(plane.size(), size_t(TileData::kPixelCount));

    // Pinned pixel values (synthetic-v1, seed=1, tile (0,0), scale=1) — read
    // once from the Python provider output that produced the fixture.
    CHECK_EQ(tile->elevationAt(0, 0), int16_t(1210));
    CHECK_EQ(tile->elevationAt(1, 0), int16_t(1204));
    CHECK_EQ(tile->elevationAt(0, 1), int16_t(1207));
    CHECK_EQ(tile->elevationAt(256, 256), int16_t(507));
    CHECK_EQ(tile->elevationAt(511, 511), int16_t(747));
    CHECK_EQ(tile->elevationAt(511, 0), int16_t(528));

    CHECK_EQ(int(tile->climateAt(0, 0, 0)), 220); // temperature
    CHECK_EQ(int(tile->climateAt(1, 0, 0)), 83);  // seasonality
    CHECK_EQ(int(tile->climateAt(2, 0, 0)), 123); // precipitation
    CHECK_EQ(int(tile->climateAt(3, 0, 0)), 134); // precip variability

    CHECK_EQ(int(tile->climateAt(0, 256, 256)), 184);
    CHECK_EQ(int(tile->climateAt(1, 256, 256)), 119);
    CHECK_EQ(int(tile->climateAt(2, 256, 256)), 98);
    CHECK_EQ(int(tile->climateAt(3, 256, 256)), 124);

    CHECK_EQ(int(tile->climateAt(0, 511, 511)), 150);
    CHECK_EQ(int(tile->climateAt(1, 511, 511)), 101);
    CHECK_EQ(int(tile->climateAt(2, 511, 511)), 97);
    CHECK_EQ(int(tile->climateAt(3, 511, 511)), 123);
}

VXC_TEST(tiledata_rejects_corrupt_input) {
    auto bytes = readFileBytes(fixturePath());
    CHECK(bytes.has_value());

    // Bad magic.
    auto badMagic = *bytes;
    badMagic[0] ^= 0xff;
    CHECK(!TileData::parse(badMagic.data(), badMagic.size()).has_value());

    // Bad version.
    auto badVersion = *bytes;
    badVersion[4] = 0x77;
    CHECK(!TileData::parse(badVersion.data(), badVersion.size()).has_value());

    // Bad declared size (offset 23..24 is the u16 size field: 4+2+8+4+4+1=23).
    auto badSize = *bytes;
    badSize[23] = 0x01;
    badSize[24] = 0x00;
    CHECK(!TileData::parse(badSize.data(), badSize.size()).has_value());

    // Truncated: drop the last byte, and truncate mid-header.
    CHECK(!TileData::parse(bytes->data(), bytes->size() - 1).has_value());
    CHECK(!TileData::parse(bytes->data(), size_t(10)).has_value());
    CHECK(!TileData::parse(bytes->data(), size_t(0)).has_value());

    // Trailing bytes.
    auto trailing = *bytes;
    trailing.push_back(0);
    CHECK(!TileData::parse(trailing.data(), trailing.size()).has_value());

    // Sanity: the untouched bytes still parse.
    CHECK(TileData::parse(bytes->data(), bytes->size()).has_value());
}

VXC_TEST(tilegridsampler_loads_and_rejects_seed_scale_mismatch) {
    TileGridSampler wrongSeed(2, 1);
    CHECK(!wrongSeed.loadTileFile(fixturePath()));
    CHECK_EQ(wrongSeed.tileCount(), size_t(0));

    TileGridSampler wrongScale(1, 8);
    CHECK(!wrongScale.loadTileFile(fixturePath()));
    CHECK_EQ(wrongScale.tileCount(), size_t(0));

    TileGridSampler ok(1, 1);
    CHECK(ok.loadTileFile(fixturePath()));
    CHECK_EQ(ok.tileCount(), size_t(1));
    CHECK_EQ(ok.pixelSizeMm(), 30000);

    // Loading garbage bytes fails cleanly too.
    TileGridSampler garbage(1, 1);
    std::vector<uint8_t> junk = {'N', 'O', 'P', 'E', 0, 0, 0, 0};
    CHECK(!garbage.loadTile(junk));
    CHECK_EQ(garbage.tileCount(), size_t(0));
}

VXC_TEST(tilegridsampler_pixel_routing_and_missing_tile_counter) {
    TileGridSampler s(1, 1);
    CHECK(s.loadTileFile(fixturePath()));
    CHECK_EQ(s.missingTileQueries, uint64_t(0));

    // In-range pixels: routed to tile (0,0), values match the fixture.
    CHECK_EQ(s.elevationMm(0, 0), int32_t(1210) * 1000);
    CHECK_EQ(s.elevationMm(1, 0), int32_t(1204) * 1000);
    CHECK_EQ(s.elevationMm(0, 1), int32_t(1207) * 1000);
    CHECK_EQ(s.elevationMm(511, 511), int32_t(747) * 1000);
    CHECK_EQ(s.elevationMm(256, 256), int32_t(507) * 1000);
    ClimateSample c00 = s.climate(0, 0);
    CHECK_EQ(int(c00.temperature), 220);
    CHECK_EQ(int(c00.seasonality), 83);
    CHECK_EQ(int(c00.precipitation), 123);
    CHECK_EQ(int(c00.precipVariability), 134);
    CHECK_EQ(s.missingTileQueries, uint64_t(0)); // still zero: all in-range

    // Out-of-range positive pixel: tile (1,0), not loaded -> deterministic
    // default + counter increments.
    CHECK_EQ(s.elevationMm(512, 0), int32_t(0));
    CHECK_EQ(s.missingTileQueries, uint64_t(1));
    ClimateSample cMiss = s.climate(512, 0);
    CHECK_EQ(int(cMiss.temperature), 128);
    CHECK_EQ(int(cMiss.seasonality), 0);
    CHECK_EQ(int(cMiss.precipitation), 128);
    CHECK_EQ(int(cMiss.precipVariability), 0);
    CHECK_EQ(s.missingTileQueries, uint64_t(2));

    // Negative pixel coords: floorDiv(-1,512) == -1, a different (missing)
    // tile from tile (0,0) — must NOT alias to the loaded tile via truncation
    // toward zero.
    CHECK_EQ(s.elevationMm(-1, -1), int32_t(0));
    CHECK_EQ(s.missingTileQueries, uint64_t(3));
    CHECK_EQ(s.elevationMm(-512, 0), int32_t(0)); // tile (-1, 0)
    CHECK_EQ(s.missingTileQueries, uint64_t(4));

    // Exactly on the border: px=511 is the last local pixel of tile (0,0)
    // (in range, no counter bump); px=512 (checked above) is local pixel 0
    // of the neighboring, unloaded tile (1,0).
    const uint64_t before = s.missingTileQueries;
    CHECK_EQ(s.elevationMm(511, 0), int32_t(528) * 1000); // real tile data
    CHECK_EQ(s.missingTileQueries, before);
}

VXC_TEST(amplifier_over_tilegridsampler_is_deterministic) {
    // Determinism through the real-data path (plan §2.3): the amplifier must
    // produce identical output across two independently loaded samplers over
    // the same fixture tile, with zero missing-tile fallbacks in the probed
    // range (proving the columns are actually driven by loaded tile data).
    TileGridSampler samplerA(1, 1), samplerB(1, 1);
    CHECK(samplerA.loadTileFile(fixturePath()));
    CHECK(samplerB.loadTileFile(fixturePath()));

    Amplifier ampA(1, samplerA), ampB(1, samplerB);

    Digest digestA, digestB;
    for (int64_t vy = 0; vy <= 150000; vy += 6000) {
        for (int64_t vx = 0; vx <= 150000; vx += 6000) {
            const ColumnSample ca = ampA.column(vx, vy);
            const ColumnSample cb = ampB.column(vx, vy);
            digestA.u32(static_cast<uint32_t>(ca.surfaceMm));
            digestA.u32(static_cast<uint32_t>(ca.topsoilMm));
            digestA.u32(static_cast<uint32_t>(ca.subsoilMm));
            digestA.u32(static_cast<uint32_t>(ca.bedrockDepthMm));
            digestA.u8(ca.surfaceMat);
            digestB.u32(static_cast<uint32_t>(cb.surfaceMm));
            digestB.u32(static_cast<uint32_t>(cb.topsoilMm));
            digestB.u32(static_cast<uint32_t>(cb.subsoilMm));
            digestB.u32(static_cast<uint32_t>(cb.bedrockDepthMm));
            digestB.u8(cb.surfaceMat);
            // Every column here samples pixels 0..500 x 0..500 — well inside
            // the single loaded 512x512 tile, plus its +1 bilinear neighbor.
        }
    }
    CHECK_EQ(digestA.h, digestB.h);
    CHECK_EQ(samplerA.missingTileQueries, uint64_t(0));
    CHECK_EQ(samplerB.missingTileQueries, uint64_t(0));
}
