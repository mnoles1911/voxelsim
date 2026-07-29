// Tile decoding + sampling (plan Â§3.1 step 2, Â§3.4 ITerrainSource): a
// cross-language fixture test. The fixture file is a byte-for-byte tile
// produced by terrain-service's Python synthetic provider + tile_codec.encode
// (seed=1, x=0, y=0, scale=1) â€” see voxel-core/tests/fixtures/README-less
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

#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

#include "voxelcore/amplifier.h"
#include "vxctest.h"

using namespace vxc;

namespace {

std::filesystem::path fixturePath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "tile_s1_seed1_0_0.vxtl";
}

// Builds an in-memory TileData with a constant elevation and constant
// climate planes, for tests that want to drive the amplifier from known
// hand-built tile data rather than the on-disk fixture.
TileData makeConstantTile(uint64_t seed, int32_t x, int32_t y, uint8_t scale,
                           int16_t elevationMetres, uint8_t temperature = 128,
                           uint8_t seasonality = 0, uint8_t precipitation = 128,
                           uint8_t precipVariability = 0) {
    TileData t;
    t.seed = seed;
    t.x = x;
    t.y = y;
    t.scale = scale;
    t.elevation.assign(TileData::kPixelCount, elevationMetres);
    t.climate[0].assign(TileData::kPixelCount, temperature);
    t.climate[1].assign(TileData::kPixelCount, seasonality);
    t.climate[2].assign(TileData::kPixelCount, precipitation);
    t.climate[3].assign(TileData::kPixelCount, precipVariability);
    return t;
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

    // Pinned pixel values (synthetic-v1, seed=1, tile (0,0), scale=1) â€” read
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
    CHECK_EQ(s.missingTileQueries.load(), uint64_t(0));

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
    CHECK_EQ(s.missingTileQueries.load(), uint64_t(0)); // still zero: all in-range

    // Out-of-range positive pixel: tile (1,0), not loaded -> deterministic
    // default + counter increments.
    CHECK_EQ(s.elevationMm(512, 0), int32_t(0));
    CHECK_EQ(s.missingTileQueries.load(), uint64_t(1));
    // Compared against ClimateSample{} rather than literals: the missing-tile
    // answer IS the default-constructed sample, and pinning the numbers here
    // twice only means updating them twice. What matters is that a miss is
    // deterministic and is the documented bland default, not what byte that
    // happens to be. (v8 moved it from 128/0/128/0 -- which decoded to a
    // freezing 6000 mm/yr rainforest -- to ~10 C / 800 mm/yr.)
    const ClimateSample kMissDefault{};
    ClimateSample cMiss = s.climate(512, 0);
    CHECK_EQ(int(cMiss.temperature), int(kMissDefault.temperature));
    CHECK_EQ(int(cMiss.seasonality), int(kMissDefault.seasonality));
    CHECK_EQ(int(cMiss.precipitation), int(kMissDefault.precipitation));
    CHECK_EQ(int(cMiss.precipVariability), int(kMissDefault.precipVariability));
    CHECK_EQ(s.missingTileQueries.load(), uint64_t(2));

    // Negative pixel coords: floorDiv(-1,512) == -1, a different (missing)
    // tile from tile (0,0) â€” must NOT alias to the loaded tile via truncation
    // toward zero.
    CHECK_EQ(s.elevationMm(-1, -1), int32_t(0));
    CHECK_EQ(s.missingTileQueries.load(), uint64_t(3));
    CHECK_EQ(s.elevationMm(-512, 0), int32_t(0)); // tile (-1, 0)
    CHECK_EQ(s.missingTileQueries.load(), uint64_t(4));

    // Exactly on the border: px=511 is the last local pixel of tile (0,0)
    // (in range, no counter bump); px=512 (checked above) is local pixel 0
    // of the neighboring, unloaded tile (1,0).
    const uint64_t before = s.missingTileQueries.load();
    CHECK_EQ(s.elevationMm(511, 0), int32_t(528) * 1000); // real tile data
    CHECK_EQ(s.missingTileQueries.load(), before);
}

VXC_TEST(amplifier_over_tilegridsampler_is_deterministic) {
    // Determinism through the real-data path (plan Â§2.3): the amplifier must
    // produce identical output across two independently loaded samplers over
    // the same fixture tile, with zero missing-tile fallbacks in the probed
    // range (proving the columns are actually driven by loaded tile data).
    TileGridSampler samplerA(1, 1), samplerB(1, 1);
    CHECK(samplerA.loadTileFile(fixturePath()));
    CHECK(samplerB.loadTileFile(fixturePath()));

    Amplifier ampA(1, samplerA), ampB(1, samplerB);

    // v9 STENCIL CONTRACT. The carrier is a cubic B-spline, so a column in
    // pixel cell px reads control points px-1 .. px+2 -- one further out on the
    // low side and two on the high side than v8's bilinear px..px+1. Starting
    // this sweep at vx = 0 therefore reads pixel -1, which is in the tile at
    // (-1,-1) and not loaded here, and the missing-tile assertions below would
    // fire on the amplifier doing exactly what it is now specified to do.
    // Start one cell in instead: pixels 20..500 of a 512-pixel tile, whose
    // stencils span 19..502 and stay inside.
    Digest digestA, digestB;
    for (int64_t vy = 6000; vy <= 150000; vy += 6000) {
        for (int64_t vx = 6000; vx <= 150000; vx += 6000) {
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
            // Every column here samples pixels 20..500 x 20..500, whose v9
            // control stencils span 19..502 — well inside the single loaded
            // 512x512 tile.
        }
    }
    CHECK_EQ(digestA.h, digestB.h);
    CHECK_EQ(samplerA.missingTileQueries.load(), uint64_t(0));
    CHECK_EQ(samplerB.missingTileQueries.load(), uint64_t(0));
}

VXC_TEST(amplifier_over_manual_tiles_end_to_end) {
    // Same determinism proof as amplifier_over_tilegridsampler_is_deterministic,
    // but driving TileGridSampler from hand-built TileData (no fixture/codec
    // in the loop) â€” this is the shape UE runtime code will construct tiles
    // in once decoded from the terrain service, so it's worth pinning
    // independently of the fixture-based path.
    constexpr uint64_t seed = 20260721;

    TileData tile100 = makeConstantTile(seed, 0, 0, 1, 100);
    TileData tile500 = makeConstantTile(seed, 0, 0, 1, 500);

    TileGridSampler samplerA100(seed, 1), samplerB100(seed, 1), sampler500(seed, 1);
    CHECK(samplerA100.loadTile(tile100));
    CHECK(samplerB100.loadTile(tile100));
    CHECK(sampler500.loadTile(tile500));

    Amplifier ampA100(seed, samplerA100), ampB100(seed, samplerB100), amp500(seed, sampler500);

    // (i) Determinism: two independently loaded samplers over byte-identical
    // tile data must produce byte-identical ColumnSample output across a grid.
    Digest digestA, digestB;
    for (int64_t vy = 15000; vy <= 150000; vy += 15000) {
        for (int64_t vx = 15000; vx <= 150000; vx += 15000) {
            const ColumnSample ca = ampA100.column(vx, vy);
            const ColumnSample cb = ampB100.column(vx, vy);
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
        }
    }
    CHECK_EQ(digestA.h, digestB.h);
    CHECK_EQ(samplerA100.missingTileQueries.load(), uint64_t(0));
    CHECK_EQ(samplerB100.missingTileQueries.load(), uint64_t(0));

    // (ii) Tile data actually drives the amplifier: the 500m tile must yield
    // a strictly greater surfaceMm than the 100m tile at the same coords.
    // The 400m (400,000mm) gap between the two constant bases dwarfs the
    // bounded detail-octave contribution (see (iii) below), so this holds
    // everywhere in the probed range, not just at one lucky point.
    for (int64_t vy = 30000; vy <= 150000; vy += 30000) {
        for (int64_t vx = 30000; vx <= 150000; vx += 30000) {
            const int32_t s100 = ampA100.column(vx, vy).surfaceMm;
            const int32_t s500 = amp500.column(vx, vy).surfaceMm;
            CHECK(s500 > s100);
        }
    }

    // (iii) Sane band around 100,000 mm for the 100m tile. Justification
    // (mirrors amplifier.cpp's Amplifier::column): the carrier's weights sum
    // exactly to their denominator, so a B-spline over a perfectly CONSTANT
    // elevation field reproduces that constant exactly -- baseMm == 100000 mm
    // everywhere, with no interpolation error to argue about. For the same
    // reason every control-point first difference is zero, so the analytic
    // gradient is exactly zero and slopeMmPerM == 0, giving
    // sScale == slopeScaleQ10(0) == 512 (0.5x) and
    // mScale == microScaleQ10(0) == 768 (0.75x).
    //
    // Worst-case detail is then 0.5 * (2600 + 1100) + 0.75 * (500 + 190 + 60)
    // = 2412 mm, which is WIDER than the band below. That is deliberate and
    // was already true at v8: the bound is over the extreme of every octave's
    // hash simultaneously, which no real draw attains. The band is empirical,
    // and its job is to catch the tile data ceasing to drive the base at all
    // (a 100 km miss), not to be a proof.
    for (int64_t vy = 30000; vy <= 150000; vy += 30000) {
        for (int64_t vx = 30000; vx <= 150000; vx += 30000) {
            const int32_t surfaceMm = ampA100.column(vx, vy).surfaceMm;
            CHECK(surfaceMm >= 100000 - 2000);
            CHECK(surfaceMm <= 100000 + 2000);
        }
    }
}

VXC_TEST(missing_tile_fallback_is_deterministic_through_amplifier) {
    // Querying far outside any loaded tile must fall back to the documented
    // deterministic default (elevation 0, default ClimateSample), bump the
    // counter, and â€” since the amplifier is a pure function of what the
    // sampler returns â€” still produce byte-identical Amplifier output across
    // two independently constructed (empty) samplers.
    constexpr uint64_t seed = 555;
    TileGridSampler samplerA(seed, 1), samplerB(seed, 1); // no tiles loaded at all

    for (TileGridSampler* s : {&samplerA, &samplerB}) {
        CHECK_EQ(s->elevationMm(1'000'000, 1'000'000), int32_t(0));
        const ClimateSample kDefault{};
        const ClimateSample c = s->climate(1'000'000, 1'000'000);
        CHECK_EQ(int(c.temperature), int(kDefault.temperature));
        CHECK_EQ(int(c.seasonality), int(kDefault.seasonality));
        CHECK_EQ(int(c.precipitation), int(kDefault.precipitation));
        CHECK_EQ(int(c.precipVariability), int(kDefault.precipVariability));
    }
    CHECK_EQ(samplerA.missingTileQueries.load(), uint64_t(2));
    CHECK_EQ(samplerB.missingTileQueries.load(), uint64_t(2));

    Amplifier ampA(seed, samplerA), ampB(seed, samplerB);
    Digest digestA, digestB;
    for (int64_t vy = 500000; vy <= 590000; vy += 15000) {
        for (int64_t vx = 500000; vx <= 590000; vx += 15000) {
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
        }
    }
    CHECK_EQ(digestA.h, digestB.h);
    // Every query in this loop misses (nothing is loaded), and both samplers
    // ran the exact same query sequence, so their tallies must match exactly
    // â€” not merely both be nonzero.
    CHECK_EQ(samplerA.missingTileQueries.load(), samplerB.missingTileQueries.load());
    CHECK(samplerA.missingTileQueries.load() > uint64_t(2));
}

VXC_TEST(concurrent_queries_are_thread_safe_and_counter_is_exact) {
    // Meshing-worker-thread smoke test: several threads hammer
    // elevationMm()/climate() concurrently, spanning both the one loaded
    // tile and an adjacent never-loaded tile. tiles_ is populated before any
    // thread starts (doctrine: all loadTile* calls happen at init), so the
    // only concurrent mutation is the atomic counter increment. The query
    // pattern is arranged so the exact miss count is computable up front,
    // independent of thread interleaving.
    constexpr uint64_t seed = 777;
    TileGridSampler s(seed, 1);
    CHECK(s.loadTile(makeConstantTile(seed, 0, 0, 1, 250, /*temperature=*/200,
                                       /*seasonality=*/10, /*precipitation=*/90,
                                       /*precipVariability=*/5)));

    constexpr int kThreads = 4;
    constexpr int64_t kItersPerThread = 4096; // multiple of 1024 -> exact miss accounting below
    std::atomic<uint64_t> mismatches{0};

    auto worker = [&](int64_t py) {
        for (int64_t i = 0; i < kItersPerThread; ++i) {
            // px cycles 0..1023: [0,511] is loaded tile (0,0); [512,1023] is
            // never-loaded tile (1,0). py in [0, kThreads) keeps ty == 0
            // throughout, so tile (0,0) is the only one ever loaded here.
            const int64_t px = i % 1024;
            const int32_t elev = s.elevationMm(px, py);
            const ClimateSample c = s.climate(px, py);
            if (px < 512) {
                if (elev != int32_t(250) * 1000) mismatches.fetch_add(1, std::memory_order_relaxed);
                if (c.temperature != 200 || c.seasonality != 10 || c.precipitation != 90 ||
                    c.precipVariability != 5)
                    mismatches.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (elev != 0) mismatches.fetch_add(1, std::memory_order_relaxed);
                const ClimateSample kDefault{};
                if (c.temperature != kDefault.temperature ||
                    c.seasonality != kDefault.seasonality ||
                    c.precipitation != kDefault.precipitation ||
                    c.precipVariability != kDefault.precipVariability)
                    mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, int64_t(t));
    for (std::thread& th : threads) th.join();

    // Correctness must not be reported via CHECK/CHECK_EQ from inside worker
    // threads: vxctest's failure counter is a plain int, not atomic, so
    // concurrent CHECK failures there would themselves be a data race. All
    // per-query verdicts are folded into the atomic `mismatches` counter
    // instead, and asserted here after every thread has joined.
    CHECK_EQ(mismatches.load(), uint64_t(0));

    // Exact expected miss count: each thread's px cycles 0..1023 exactly
    // kItersPerThread/1024 times; 512 of the 1024 px values per cycle land in
    // the unloaded tile and miss on BOTH elevationMm and climate (2 misses
    // each), independent of scheduling/interleaving.
    const uint64_t cyclesPerThread = uint64_t(kItersPerThread) / 1024;
    const uint64_t missesPerThread = cyclesPerThread * 512 * 2;
    const uint64_t expectedTotal = missesPerThread * uint64_t(kThreads);
    CHECK_EQ(s.missingTileQueries.load(), expectedTotal);
}
