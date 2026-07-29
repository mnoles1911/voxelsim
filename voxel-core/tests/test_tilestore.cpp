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

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/bytes.h"
#include "voxelcore/core.h"
#include "vxctest.h"

using namespace vxc;

namespace {

std::filesystem::path fixturePath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "tile_s1_seed1_0_0.vxtl";
}

// The Python-encoded golden v2 tile of docs/vxtl-v2-format.md §9 item 2 —
// written by the ENCODER half of the contract, against the same frozen
// document, with no code shared with this side. Decoding it correctly is the
// interop proof. It is a deliberately small 512-edge tile (a 2x2 grid of the
// production 256x256 blocks) so it can be committed at 512 KB instead of
// 21 MB; production `size` is 8192. The test skips cleanly if it is absent.
std::filesystem::path fineFixturePath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_512.vxtl";
}

// The companion golden tile carrying a §6 flow plane, same provenance and the
// same skip-if-absent rule. Its elevation plane is deliberately uniform (all
// CODED/16) because mode diversity is already covered above; the FLOW plane is
// the one carrying one block of each mode.
std::filesystem::path fineFlowFixturePath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_flow_512.vxtl";
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

// ---------------------------------------------------------------------------
// A hand-rolled .vxtl v2 ENCODER, written straight from docs/vxtl-v2-format.md
// and deliberately NOT sharing a line of code with the decoder under test.
//
// That independence is the whole point. The real encoder is Python, built by
// another author against the same document, so the failure this file has to
// catch is "the C++ decoder disagrees with a plain reading of the spec" — and
// a test that reused the decoder's own constants and helpers could not catch
// it. Every offset, predictor rule and zigzag below is transcribed from the
// document, not from tilestore.cpp.
// ---------------------------------------------------------------------------

// §5, transcribed verbatim.
int32_t medPredictRef(int32_t W, int32_t N, int32_t NW) {
    const int32_t mx = W > N ? W : N;
    const int32_t mn = W < N ? W : N;
    if (NW >= mx) return mn;
    if (NW <= mn) return mx;
    return W + N - NW;
}
// §5: "(r << 1) ^ (r >> 31)".
uint32_t zigzagRef(int32_t r) {
    return (static_cast<uint32_t>(r) << 1) ^ static_cast<uint32_t>(r >> 31);
}

template <typename T>
struct PlanePlan {
    uint8_t mode = 0;       // 0 CONSTANT, 1 CODED, 2 RAW
    uint8_t residBits = 16; // 16 or 32
    int16_t constCp = 0;
    std::vector<T> v; // blockPixels samples, row-major x fastest (CODED/RAW)
};

// Encodes one plane's §4 index + data sections. dim is the block edge in
// pixels; plans is blocksPerAxis^2 entries, row-major with x fastest.
template <typename T>
void encodePlane(const std::vector<PlanePlan<T>>& plans, uint32_t dim,
                 std::vector<uint8_t>& index, std::vector<uint8_t>& data) {
    const uint32_t n = dim * dim;
    ByteWriter iw(index), dw(data);
    for (const PlanePlan<T>& p : plans) {
        uint64_t offset = 0;
        uint32_t compLen = 0;
        if (p.mode == 2) { // RAW: literal samples, no predictor, no zigzag
            offset = static_cast<uint64_t>(data.size());
            for (uint32_t i = 0; i < n; ++i) {
                if constexpr (sizeof(T) == 2) {
                    dw.u16(static_cast<uint16_t>(static_cast<int16_t>(p.v[i])));
                } else {
                    dw.u8(static_cast<uint8_t>(p.v[i]));
                }
            }
            compLen = static_cast<uint32_t>(n * sizeof(T));
        } else if (p.mode == 1) { // CODED: MED residuals, zigzag, LE
            offset = static_cast<uint64_t>(data.size());
            for (uint32_t y = 0; y < dim; ++y) {
                for (uint32_t x = 0; x < dim; ++x) {
                    int32_t pred;
                    if (x == 0 && y == 0) {
                        pred = 0;
                    } else if (y == 0) {
                        pred = static_cast<int32_t>(p.v[x - 1]);
                    } else if (x == 0) {
                        pred = static_cast<int32_t>(p.v[(y - 1) * dim]);
                    } else {
                        pred = medPredictRef(static_cast<int32_t>(p.v[y * dim + x - 1]),
                                             static_cast<int32_t>(p.v[(y - 1) * dim + x]),
                                             static_cast<int32_t>(p.v[(y - 1) * dim + x - 1]));
                    }
                    const uint32_t zz = zigzagRef(static_cast<int32_t>(p.v[y * dim + x]) - pred);
                    if (p.residBits == 16) dw.u16(static_cast<uint16_t>(zz));
                    else dw.u32(zz);
                }
            }
            compLen = n * (p.residBits / 8u);
        }
        iw.u64(offset);
        iw.u32(compLen);
        iw.u8(p.mode);
        iw.u16(static_cast<uint16_t>(p.constCp));
        // resid_bits is meaningful only for CODED; the Python encoder writes 0
        // for CONSTANT and RAW, and a decoder that validated it against
        // {16, 32} unconditionally would reject every real tile.
        iw.u8(p.mode == 1 ? p.residBits : uint8_t(0));
        for (int i = 0; i < 4; ++i) iw.u8(0); // pad
    }
}

struct V2Params {
    uint64_t seed = 1;
    int32_t x = 0, y = 0;
    uint8_t scale = 16;
    uint16_t size = 8192;
    uint8_t blockLog2 = 8;
    uint8_t predictor = 1;
    uint8_t quant = 1;
    uint8_t codec = 0;
    uint16_t bakeVer = 42;
    int32_t baseOffsetMm = 0;
    uint8_t parentScale = 0;
};

// Assembles the whole file: §3 header, section table, then ELEV_INDEX /
// ELEV_DATA (and FLOW_* when flowPlans is non-empty), each section immediately
// after the previous one.
std::vector<uint8_t> buildFineTile(const V2Params& p,
                                   const std::vector<PlanePlan<int16_t>>& elevPlans,
                                   const std::vector<PlanePlan<uint8_t>>& flowPlans = {}) {
    const uint32_t dim = 1u << p.blockLog2;
    std::vector<uint8_t> elevIndex, elevData, flowIndex, flowData;
    encodePlane<int16_t>(elevPlans, dim, elevIndex, elevData);
    const bool hasFlow = !flowPlans.empty();
    if (hasFlow) encodePlane<uint8_t>(flowPlans, dim, flowIndex, flowData);

    const uint16_t nSections = hasFlow ? uint16_t(4) : uint16_t(2);
    const uint64_t tableEnd = 43 + uint64_t(nSections) * 20;
    const uint64_t elevIndexOff = tableEnd;
    const uint64_t elevDataOff = elevIndexOff + elevIndex.size();
    const uint64_t flowIndexOff = elevDataOff + elevData.size();
    const uint64_t flowDataOff = flowIndexOff + flowIndex.size();

    std::vector<uint8_t> out;
    ByteWriter w(out);
    w.u8('V'); w.u8('X'); w.u8('T'); w.u8('L');
    w.u16(2);
    w.u64(p.seed);
    w.i32(p.x);
    w.i32(p.y);
    w.u8(p.scale);
    w.u16(p.size);
    w.u8(p.blockLog2);
    w.u8(p.predictor);
    w.u8(p.quant);
    w.u8(p.codec);
    w.u16(p.bakeVer);
    w.u16(hasFlow ? uint16_t(1) : uint16_t(0)); // flags bit0 = flow present
    w.i32(p.baseOffsetMm);
    w.u8(p.parentScale);
    w.u8(0); w.u8(0); w.u8(0); // reserved
    w.u16(nSections);
    // Section table: {u32 id, u64 offset, u64 length}, offsets from file start.
    w.u32(1); w.u64(elevIndexOff); w.u64(elevIndex.size());
    w.u32(2); w.u64(elevDataOff); w.u64(elevData.size());
    if (hasFlow) {
        w.u32(3); w.u64(flowIndexOff); w.u64(flowIndex.size());
        w.u32(4); w.u64(flowDataOff); w.u64(flowData.size());
    }
    out.insert(out.end(), elevIndex.begin(), elevIndex.end());
    out.insert(out.end(), elevData.begin(), elevData.end());
    if (hasFlow) {
        out.insert(out.end(), flowIndex.begin(), flowIndex.end());
        out.insert(out.end(), flowData.begin(), flowData.end());
    }
    return out;
}

// Deterministic integer-only pseudo-random lattice value in [-spread, spread].
int16_t testCp(uint32_t gx, uint32_t gy, int32_t spread) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t(gx) * 0x85EBCA6Bull);
    h *= 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
    h ^= uint64_t(gy) * 0x165667B19E3779F9ull;
    h *= 0x9E3779B185EBCA87ull;
    h ^= h >> 31;
    const int32_t span = 2 * spread + 1;
    return static_cast<int16_t>(static_cast<int32_t>(h % static_cast<uint64_t>(span)) - spread);
}

// The four block layouts §9 item 1 enumerates, at fixed positions so every
// test agrees on where they are. Everything else in the tile is CONSTANT with
// a per-block distinct value, which is also what pins §4's "row-major, x
// fastest" index ordering.
constexpr uint32_t kTestBlockLog2 = 8;
constexpr uint32_t kTestDim = 1u << kTestBlockLog2;      // 256
constexpr uint32_t kTestPerAxis = 8192u >> kTestBlockLog2; // 32
constexpr uint32_t kTestBlocks = kTestPerAxis * kTestPerAxis;
constexpr uint32_t kTestBlockPixels = kTestDim * kTestDim;

int16_t constCpForBlock(uint32_t bx, uint32_t by) {
    return static_cast<int16_t>(static_cast<int32_t>(by * kTestPerAxis + bx) - 512);
}

// Block (1,0): CODED/16. Small relief, so every residual fits int16.
int16_t codedSmallCp(uint32_t lx, uint32_t ly) { return testCp(lx, ly + 1000, 300); }
// Block (2,0): CODED/32. A checkerboard of +/-32000 makes every interior
// residual ~64000, which leaves [-32768, 32767] — §5's "resid_bits is
// REQUIRED, not an optimisation" case.
int16_t codedWideCp(uint32_t lx, uint32_t ly) {
    return static_cast<int16_t>(((lx + ly) & 1u) ? 32000 : -32000);
}
// Block (3,0): RAW, literal control points.
int16_t rawCp(uint32_t lx, uint32_t ly) { return testCp(lx + 7, ly + 9, 30000); }

std::vector<PlanePlan<int16_t>> makeMixedElevPlans() {
    std::vector<PlanePlan<int16_t>> plans(kTestBlocks);
    for (uint32_t by = 0; by < kTestPerAxis; ++by) {
        for (uint32_t bx = 0; bx < kTestPerAxis; ++bx) {
            PlanePlan<int16_t>& p = plans[by * kTestPerAxis + bx];
            p.mode = 0;
            p.constCp = constCpForBlock(bx, by);
        }
    }
    auto fill = [](PlanePlan<int16_t>& p, uint8_t mode, uint8_t bits,
                   int16_t (*gen)(uint32_t, uint32_t)) {
        p.mode = mode;
        p.residBits = bits;
        p.constCp = 0;
        p.v.resize(kTestBlockPixels);
        for (uint32_t ly = 0; ly < kTestDim; ++ly)
            for (uint32_t lx = 0; lx < kTestDim; ++lx) p.v[ly * kTestDim + lx] = gen(lx, ly);
    };
    fill(plans[1], 1, 16, &codedSmallCp);
    fill(plans[2], 1, 32, &codedWideCp);
    fill(plans[3], 2, 16, &rawCp);
    return plans;
}

// What block (bx,by)'s pixel (lx,ly) must decode to, independent of how it was
// encoded.
int16_t expectedCp(uint32_t bx, uint32_t by, uint32_t lx, uint32_t ly) {
    if (by == 0 && bx == 1) return codedSmallCp(lx, ly);
    if (by == 0 && bx == 2) return codedWideCp(lx, ly);
    if (by == 0 && bx == 3) return rawCp(lx, ly);
    return constCpForBlock(bx, by);
}

// Every block of the tile set to one CONSTANT value — a perfectly flat control
// lattice.
std::vector<PlanePlan<int16_t>> makeFlatElevPlans(int16_t cp) {
    std::vector<PlanePlan<int16_t>> plans(kTestBlocks);
    for (PlanePlan<int16_t>& p : plans) {
        p.mode = 0;
        p.constCp = cp;
    }
    return plans;
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
    // tile from tile (0,0) — must NOT alias to the loaded tile via truncation
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
    // Determinism through the real-data path (plan §2.3): the amplifier must
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
    // in the loop) — this is the shape UE runtime code will construct tiles
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
    // counter, and — since the amplifier is a pure function of what the
    // sampler returns — still produce byte-identical Amplifier output across
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
    // — not merely both be nonzero.
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

// ===========================================================================
// .vxtl v2 — the baked fine tier (docs/vxtl-v2-format.md).
// ===========================================================================

VXC_TEST(vxtl_v2_pixel_size_and_scale_constants) {
    // §1: scale 16 is 30 m / 16 = 1.875 m/px, and 8192 of them span the same
    // 15.36 km as one 512 px s1 tile at 30 m. MIRROR: tile_codec.py:43.
    CHECK_EQ(tilePixelSizeMm(16), 1875);
    CHECK_EQ(tilePixelSizeMm(1), 30000);  // unchanged
    CHECK_EQ(tilePixelSizeMm(8), 3750);   // unchanged
    CHECK_EQ(tilePixelSizeMm(2), 0);      // still unsupported
    CHECK_EQ(int64_t(kFineTileSize) * tilePixelSizeMm(16),
             int64_t(TileData::kTileSize) * tilePixelSizeMm(1));

    // §2's quantisation ladder.
    CHECK_EQ(fineQuantMm(1), 100);
    CHECK_EQ(fineQuantMm(2), 250);
    CHECK_EQ(fineQuantMm(0), 0);
    CHECK_EQ(fineQuantMm(3), 0);
}

VXC_TEST(vxtl_v2_decodes_constant_coded16_coded32_and_raw_blocks) {
    // §9 item 1's coverage, over hand-built bytes: CONSTANT, CODED/16,
    // CODED/32 and RAW blocks in one tile.
    V2Params p;
    p.seed = 20260729;
    p.x = -3;
    p.y = 5;
    p.baseOffsetMm = 123'400;
    p.quant = 1; // 100 mm/LSB
    const std::vector<uint8_t> bytes = buildFineTile(p, makeMixedElevPlans());

    // The layout the builder produced, pinned so the corruption test below can
    // address individual fields by offset rather than by search.
    CHECK_EQ(bytes.size(),
             size_t(43 + 2 * 20 + kTestBlocks * 20 +
                    /*CODED/16*/ kTestBlockPixels * 2 + /*CODED/32*/ kTestBlockPixels * 4 +
                    /*RAW*/ kTestBlockPixels * 2));

    auto tile = FineTile::parse(bytes.data(), bytes.size());
    CHECK(tile.has_value());
    CHECK_EQ(tile->seed(), uint64_t(20260729));
    CHECK_EQ(tile->tileX(), -3);
    CHECK_EQ(tile->tileY(), 5);
    CHECK_EQ(tile->size(), kFineTileSize);
    CHECK_EQ(tile->blockDim(), kTestDim);
    CHECK_EQ(tile->blocksPerAxis(), kTestPerAxis);
    CHECK_EQ(tile->blockCount(), kTestBlocks);
    CHECK_EQ(tile->quantMm(), 100);
    CHECK_EQ(tile->baseOffsetMm(), 123'400);
    CHECK_EQ(int(tile->header().bakeVer), 42);
    CHECK(!tile->hasFlow());

    // The four interesting blocks, every pixel.
    uint64_t mismatches = 0;
    for (uint32_t bx = 0; bx < 4; ++bx) {
        std::vector<int16_t> block;
        CHECK(tile->decodeElevBlock(bx, 0, block));
        CHECK_EQ(block.size(), size_t(kTestBlockPixels));
        for (uint32_t ly = 0; ly < kTestDim; ++ly)
            for (uint32_t lx = 0; lx < kTestDim; ++lx)
                if (block[ly * kTestDim + lx] != expectedCp(bx, 0, lx, ly)) ++mismatches;
    }
    CHECK_EQ(mismatches, uint64_t(0));

    // The index really is row-major with x fastest (§4): every CONSTANT block
    // carries a distinct value derived from by*32+bx, so a transposed index
    // would show up immediately.
    uint64_t constMismatches = 0;
    for (uint32_t by = 0; by < kTestPerAxis; ++by) {
        for (uint32_t bx = 0; bx < kTestPerAxis; ++bx) {
            if (by == 0 && bx < 4) continue;
            std::vector<int16_t> block;
            if (!tile->decodeElevBlock(bx, by, block)) { ++constMismatches; continue; }
            if (block.front() != constCpForBlock(bx, by)) ++constMismatches;
            if (block.back() != constCpForBlock(bx, by)) ++constMismatches;
        }
    }
    CHECK_EQ(constMismatches, uint64_t(0));

    // Out-of-range block coords are rejected, not clamped.
    std::vector<int16_t> scratch;
    CHECK(!tile->decodeElevBlock(kTestPerAxis, 0, scratch));
    CHECK(!tile->decodeElevBlock(0, kTestPerAxis, scratch));
    // No flow plane -> no flow blocks.
    std::vector<uint8_t> flowScratch;
    CHECK(!tile->decodeFlowBlock(0, 0, flowScratch));

    // §2's elevation formula, on a control point from each block mode.
    int16_t cp = 0;
    CHECK(tile->controlPointAt(0, 0, cp)); // CONSTANT block (0,0)
    CHECK_EQ(cp, constCpForBlock(0, 0));
    CHECK_EQ(tile->elevationMmFromCp(cp), 123'400 + int32_t(cp) * 100);
    CHECK(tile->controlPointAt(kTestDim + 5, 9, cp)); // CODED/16 block (1,0)
    CHECK_EQ(cp, codedSmallCp(5, 9));
    CHECK(tile->controlPointAt(2 * kTestDim + 4, 6, cp)); // CODED/32 block (2,0)
    CHECK_EQ(cp, codedWideCp(4, 6));
    CHECK(tile->controlPointAt(3 * kTestDim + 200, 201, cp)); // RAW block (3,0)
    CHECK_EQ(cp, rawCp(200, 201));
    CHECK(!tile->controlPointAt(kFineTileSize, 0, cp)); // outside the lattice
}

VXC_TEST(vxtl_v2_quant_250_scales_elevation) {
    // §2/§3: `quant` selects the LSB so a high-relief tile can fall back to
    // 250 mm without a format change. Same lattice, both quant values: the
    // control points must decode identically and only the mm conversion moves.
    V2Params p100, p250;
    p100.quant = 1;
    p100.baseOffsetMm = -70'000;
    p250 = p100;
    p250.quant = 2;

    const std::vector<uint8_t> b100 = buildFineTile(p100, makeMixedElevPlans());
    const std::vector<uint8_t> b250 = buildFineTile(p250, makeMixedElevPlans());
    // Only the `quant` byte at offset 27 differs.
    CHECK_EQ(b100.size(), b250.size());
    size_t differing = 0;
    for (size_t i = 0; i < b100.size(); ++i)
        if (b100[i] != b250[i]) ++differing;
    CHECK_EQ(differing, size_t(1));

    auto t100 = FineTile::parse(b100.data(), b100.size());
    auto t250 = FineTile::parse(b250.data(), b250.size());
    CHECK(t100.has_value());
    CHECK(t250.has_value());
    CHECK_EQ(t100->quantMm(), 100);
    CHECK_EQ(t250->quantMm(), 250);

    uint64_t cpMismatches = 0, mmMismatches = 0;
    for (uint32_t bx = 0; bx < 4; ++bx) {
        std::vector<int16_t> a, b;
        CHECK(t100->decodeElevBlock(bx, 0, a));
        CHECK(t250->decodeElevBlock(bx, 0, b));
        for (uint32_t i = 0; i < kTestBlockPixels; ++i) {
            if (a[i] != b[i]) ++cpMismatches;
            if (t100->elevationMmFromCp(a[i]) != -70'000 + int32_t(a[i]) * 100) ++mmMismatches;
            if (t250->elevationMmFromCp(b[i]) != -70'000 + int32_t(b[i]) * 250) ++mmMismatches;
        }
    }
    CHECK_EQ(cpMismatches, uint64_t(0));
    CHECK_EQ(mmMismatches, uint64_t(0));
}

VXC_TEST(vxtl_v2_rejects_truncated_corrupt_and_trailing_input) {
    // §9 item 4: the same all-or-nothing rejection TileData::parse gives v1.
    // Field offsets are read straight off §3's table: magic 0, version 4,
    // seed 6, x 14, y 18, scale 22, size 23, block_log2 25, predictor 26,
    // quant 27, codec 28, bake_ver 29, flags 31, base_offset_mm 33,
    // parent_scale 37, reserved 38..40, n_sections 41, section table 43.
    const V2Params p;
    const std::vector<uint8_t> good = buildFineTile(p, makeMixedElevPlans());
    CHECK(FineTile::parse(good.data(), good.size()).has_value());

    auto rejects = [&](size_t offset, uint8_t value) {
        std::vector<uint8_t> b = good;
        b[offset] = value;
        CHECK(!FineTile::parse(b.data(), b.size()).has_value());
    };

    rejects(0, 'X');   // magic
    rejects(4, 1);     // version 1 in a v2 parser
    rejects(4, 3);     // version 3
    rejects(22, 8);    // scale 8, not the fine tier
    rejects(24, 0);    // size 8192 is 0x2000: zeroing the high byte gives 0
    rejects(23, 1);    // ...and nudging the low byte gives 8193
    rejects(26, 0);    // predictor 0: PRED_MED is the only one defined
    rejects(26, 2);    // predictor 2
    rejects(27, 0);    // quant 0
    rejects(27, 3);    // quant 3
    rejects(28, 1);    // CODEC_ZSTD: valid format, not decodable in this build
    rejects(28, 9);    // codec 9: not a codec at all
    rejects(32, 0x01); // flags bit8 — an undefined flag
    rejects(31, 0x02); // flags bit1 — an undefined flag
    rejects(37, 1);    // parent_scale != 0 (reserved for a residual ladder)
    rejects(38, 1);    // reserved must be 0
    rejects(39, 1);
    rejects(40, 1);

    // block_log2 must tile the grid exactly and stay in range.
    rejects(25, 0);
    rejects(25, 14);
    rejects(25, 7); // 8192/128 = 64 blocks per axis, but the index is sized for 32

    // Truncation, at the end and mid-header.
    CHECK(!FineTile::parse(good.data(), good.size() - 1).has_value());
    CHECK(!FineTile::parse(good.data(), size_t(43)).has_value());
    CHECK(!FineTile::parse(good.data(), size_t(10)).has_value());
    CHECK(!FineTile::parse(good.data(), size_t(0)).has_value());

    // Trailing bytes past the last section.
    {
        std::vector<uint8_t> b = good;
        b.push_back(0);
        CHECK(!FineTile::parse(b.data(), b.size()).has_value());
    }

    // Section table: a section that runs past EOF, and one that overlaps the
    // header/table region. The first entry is {u32 id, u64 off, u64 len} at 43.
    {
        std::vector<uint8_t> b = good;
        b[43 + 4] = 0xff; // ELEV_INDEX offset -> absurd
        b[43 + 5] = 0xff;
        b[43 + 6] = 0xff;
        b[43 + 7] = 0xff;
        CHECK(!FineTile::parse(b.data(), b.size()).has_value());
    }
    {
        std::vector<uint8_t> b = good;
        b[43 + 4] = 0; // ELEV_INDEX offset -> 0, i.e. on top of the header
        CHECK(!FineTile::parse(b.data(), b.size()).has_value());
    }
    {
        std::vector<uint8_t> b = good;
        b[43] = 2; // both sections now claim id ELEV_DATA: duplicate id, and
                   // ELEV_INDEX is missing
        CHECK(!FineTile::parse(b.data(), b.size()).has_value());
    }

    // Block index entries. ELEV_INDEX starts at 43 + 2*20 = 83; each entry is
    // {u64 offset, u32 comp_len, u8 mode, i16 const_cp, u8 resid_bits, u8[4]}.
    constexpr size_t kIdx = 43 + 2 * 20;
    constexpr size_t kEntry = 20;
    CHECK_EQ(kIdx, size_t(83));
    {
        std::vector<uint8_t> b = good; // pad must be 0 (§4)
        b[kIdx + 16] = 1;
        CHECK(!FineTile::parse(b.data(), b.size()).has_value());
    }
    {
        std::vector<uint8_t> b = good; // CONSTANT block with comp_len != 0
        b[kIdx + 8] = 4;
        CHECK(!FineTile::parse(b.data(), b.size()).has_value());
    }
    {
        std::vector<uint8_t> b = good; // mode 3 is not a mode
        b[kIdx + 12] = 3;
        CHECK(!FineTile::parse(b.data(), b.size()).has_value());
    }
    {
        std::vector<uint8_t> b = good; // CODED block with resid_bits 8
        b[kIdx + kEntry + 15] = 8;
        CHECK(!FineTile::parse(b.data(), b.size()).has_value());
    }
    {
        // CODED/16 relabelled CODED/32: under CODEC_RAW the residual stream
        // length is fully determined, so the declared comp_len no longer
        // matches and the tile is rejected rather than half-decoded.
        std::vector<uint8_t> b = good;
        b[kIdx + kEntry + 15] = 32;
        CHECK(!FineTile::parse(b.data(), b.size()).has_value());
    }
    {
        std::vector<uint8_t> b = good; // block payload runs past ELEV_DATA
        b[kIdx + kEntry + 4] = 0xff;
        b[kIdx + kEntry + 5] = 0xff;
        CHECK(!FineTile::parse(b.data(), b.size()).has_value());
    }

    // A payload that passes every structural check but reconstructs outside
    // int16. ELEV_DATA begins at 83 + 1024*20 = 20563; block (1,0) is the
    // first non-CONSTANT block, so its residuals start there. Two consecutive
    // maximum-positive residuals drive cp to 65534.
    {
        constexpr size_t kData = kIdx + kTestBlocks * kEntry;
        CHECK_EQ(kData, size_t(20563));
        std::vector<uint8_t> b = good;
        b[kData + 0] = 0xfe; // zigzag 65534 -> +32767, pred 0 -> cp 32767
        b[kData + 1] = 0xff;
        b[kData + 2] = 0xfe; // zigzag 65534 -> +32767, pred 32767 -> 65534: reject
        b[kData + 3] = 0xff;
        auto t = FineTile::parse(b.data(), b.size());
        CHECK(t.has_value()); // structurally fine — the failure is in decode
        std::vector<int16_t> block;
        CHECK(!t->decodeElevBlock(1, 0, block));
    }

    // Sanity: the untouched bytes still parse.
    CHECK(FineTile::parse(good.data(), good.size()).has_value());
}

VXC_TEST(vxtl_v1_and_v2_parsers_reject_each_other) {
    // §3: the first 25 bytes are positionally identical to v1 precisely so a
    // v1 parser fails on `version` rather than on garbage. v1's golden
    // behaviour is unchanged by any of this.
    const V2Params p;
    const std::vector<uint8_t> v2 = buildFineTile(p, makeFlatElevPlans(7));
    CHECK(!TileData::parse(v2.data(), v2.size()).has_value());

    auto v1 = readFileBytes(fixturePath());
    CHECK(v1.has_value());
    CHECK(!FineTile::parse(v1->data(), v1->size()).has_value());
    CHECK(TileData::parse(v1->data(), v1->size()).has_value());

    // vxtlVersion routes bytes without try-both.
    CHECK_EQ(vxtlVersion(v2.data(), v2.size()).value_or(0), uint16_t(2));
    CHECK_EQ(vxtlVersion(v1->data(), v1->size()).value_or(0), uint16_t(1));
    std::vector<uint8_t> junk = {'N', 'O', 'P', 'E', 1, 0};
    CHECK(!vxtlVersion(junk.data(), junk.size()).has_value());
    CHECK(!vxtlVersion(v2.data(), size_t(5)).has_value());
}

VXC_TEST(vxtl_v2_flow_plane_round_trips) {
    // §6: one uint8 per fine pixel, same block structure, same predictor.
    // Mostly zeros in practice, so CONSTANT-0 blocks plus one CODED and one
    // RAW block is the shape a real tile has.
    std::vector<PlanePlan<uint8_t>> flow(kTestBlocks);
    for (PlanePlan<uint8_t>& f : flow) {
        f.mode = 0;
        f.constCp = 0;
    }
    auto flowByte = [](uint32_t lx, uint32_t ly) {
        // bits 0-4 log2(flow accumulation), bit5 channel, bit6 bank, bit7 deposition
        const uint32_t acc = (lx * 7u + ly * 3u) & 31u;
        uint32_t v = acc;
        if (((lx + ly) & 3u) == 0u) v |= 0x20u;
        if ((lx & 7u) == 1u) v |= 0x40u;
        if ((ly & 15u) == 2u) v |= 0x80u;
        return static_cast<uint8_t>(v);
    };
    for (uint32_t i : {1u, 2u}) {
        flow[i].mode = (i == 1u) ? uint8_t(1) : uint8_t(2); // CODED, then RAW
        flow[i].residBits = 16;
        flow[i].v.resize(kTestBlockPixels);
        for (uint32_t ly = 0; ly < kTestDim; ++ly)
            for (uint32_t lx = 0; lx < kTestDim; ++lx)
                flow[i].v[ly * kTestDim + lx] = flowByte(lx, ly);
    }
    // One CONSTANT block carrying a non-zero value, to prove const_cp is read
    // as the flow byte and not ignored.
    flow[kTestPerAxis].constCp = 0x25;

    const V2Params p;
    const std::vector<uint8_t> bytes = buildFineTile(p, makeFlatElevPlans(120), flow);
    auto tile = FineTile::parse(bytes.data(), bytes.size());
    CHECK(tile.has_value());
    CHECK(tile->hasFlow());

    uint64_t mismatches = 0;
    for (uint32_t bx : {1u, 2u}) {
        std::vector<uint8_t> block;
        CHECK(tile->decodeFlowBlock(bx, 0, block));
        CHECK_EQ(block.size(), size_t(kTestBlockPixels));
        for (uint32_t ly = 0; ly < kTestDim; ++ly)
            for (uint32_t lx = 0; lx < kTestDim; ++lx)
                if (block[ly * kTestDim + lx] != flowByte(lx, ly)) ++mismatches;
    }
    CHECK_EQ(mismatches, uint64_t(0));

    std::vector<uint8_t> block;
    CHECK(tile->decodeFlowBlock(0, 1, block)); // the non-zero CONSTANT block
    CHECK_EQ(int(block[0]), 0x25);
    CHECK_EQ(int(block.back()), 0x25);
    CHECK(tile->decodeFlowBlock(5, 5, block));
    CHECK_EQ(int(block[0]), 0);

    // The elevation plane is untouched by the flow plane's presence.
    std::vector<int16_t> elev;
    CHECK(tile->decodeElevBlock(9, 9, elev));
    CHECK_EQ(elev[0], int16_t(120));

    // flags bit0 and the FLOW_* sections must agree: clearing the bit while
    // the sections are still there is a corrupt header, not a flow-less tile.
    {
        std::vector<uint8_t> b = bytes;
        b[31] = 0;
        CHECK(!FineTile::parse(b.data(), b.size()).has_value());
    }
    // ...and so is claiming flow with no FLOW_* sections.
    {
        const std::vector<uint8_t> noFlow = buildFineTile(p, makeFlatElevPlans(120));
        std::vector<uint8_t> b = noFlow;
        b[31] = 1;
        CHECK(!FineTile::parse(b.data(), b.size()).has_value());
    }
}

VXC_TEST(finetilesampler_exposes_the_control_lattice) {
    V2Params p;
    p.seed = 4242;
    p.x = 0;
    p.y = 0;
    p.baseOffsetMm = 50'000;
    const std::vector<uint8_t> bytes = buildFineTile(p, makeMixedElevPlans());

    FineTileSampler s(4242);
    CHECK_EQ(s.pixelSizeMm(), 1875);
    CHECK(s.loadTile(bytes));
    CHECK_EQ(s.tileCount(), size_t(1));
    CHECK_EQ(s.residentBlockCount(), size_t(0)); // §4: nothing decoded until asked

    // Seed mismatch is rejected with no state change, as TileGridSampler does.
    FineTileSampler wrongSeed(1);
    CHECK(!wrongSeed.loadTile(bytes));
    CHECK_EQ(wrongSeed.tileCount(), size_t(0));
    std::vector<uint8_t> junk = {'N', 'O', 'P', 'E', 2, 0, 0, 0};
    CHECK(!wrongSeed.loadTile(junk));

    // elevationMm() returns the CONTROL POINT in mm — §2's formula — not a
    // spline sample.
    auto expectMm = [&](uint32_t bx, uint32_t by, uint32_t lx, uint32_t ly) {
        return 50'000 + int32_t(expectedCp(bx, by, lx, ly)) * 100;
    };
    CHECK_EQ(s.elevationMm(0, 0), expectMm(0, 0, 0, 0));
    CHECK_EQ(s.elevationMm(int64_t(kTestDim) + 5, 9), expectMm(1, 0, 5, 9));
    CHECK_EQ(s.elevationMm(int64_t(2 * kTestDim) + 4, 6), expectMm(2, 0, 4, 6));
    CHECK_EQ(s.elevationMm(int64_t(3 * kTestDim) + 200, 201), expectMm(3, 0, 200, 201));
    CHECK_EQ(s.elevationMm(int64_t(5 * kTestDim) + 7, int64_t(3 * kTestDim) + 11),
             expectMm(5, 3, 7, 11));
    CHECK_EQ(s.elevationMm(int64_t(kFineTileSize) - 1, int64_t(kFineTileSize) - 1),
             expectMm(kTestPerAxis - 1, kTestPerAxis - 1, kTestDim - 1, kTestDim - 1));
    CHECK_EQ(s.missingTileQueries.load(), uint64_t(0));
    CHECK_EQ(s.blockDecodeFailures.load(), uint64_t(0));
    CHECK_EQ(s.residentBlockCount(), size_t(6)); // exactly the blocks touched

    int16_t cp = 0;
    CHECK(s.controlPointAt(int64_t(kTestDim) + 5, 9, cp));
    CHECK_EQ(cp, codedSmallCp(5, 9));

    // Missing tiles: the fine grid of coarse tile (x, y) is fine pixels
    // [x*8192, (x+1)*8192), so 8192 is tile (1, 0) and -1 is tile (-1, -1) —
    // floorDiv routing, never truncation toward zero.
    CHECK_EQ(s.elevationMm(int64_t(kFineTileSize), 0), 0);
    CHECK_EQ(s.missingTileQueries.load(), uint64_t(1));
    CHECK_EQ(s.elevationMm(-1, -1), 0);
    CHECK_EQ(s.missingTileQueries.load(), uint64_t(2));
    CHECK(!s.controlPointAt(-1, -1, cp));
    CHECK_EQ(s.missingTileQueries.load(), uint64_t(3));
    CHECK(s.findTile(0, 0) != nullptr);
    CHECK(s.findTile(1, 0) == nullptr);

    // No climate source wired up -> the documented bland default, never an
    // accidental biome.
    const ClimateSample kDefault{};
    const ClimateSample c = s.climate(0, 0);
    CHECK_EQ(int(c.temperature), int(kDefault.temperature));
    CHECK_EQ(int(c.precipitation), int(kDefault.precipitation));

    // With a coarse sampler supplied, climate delegates in ITS pixel units.
    TileGridSampler coarse(4242, 1);
    CHECK(coarse.loadTile(makeConstantTile(4242, 0, 0, 1, 100, 201, 11, 91, 6)));
    FineTileSampler withClimate(4242, &coarse);
    CHECK(withClimate.loadTile(bytes));
    const ClimateSample cd = withClimate.climate(16 * 3 + 4, 16 * 5 + 1); // coarse px (3,5)
    CHECK_EQ(int(cd.temperature), 201);
    CHECK_EQ(int(cd.seasonality), 11);
    CHECK_EQ(int(cd.precipitation), 91);
    CHECK_EQ(int(cd.precipVariability), 6);
    CHECK_EQ(coarse.missingTileQueries.load(), uint64_t(0));

    // prewarm() makes a region resident so later queries are pure reads.
    FineTileSampler warm(4242);
    CHECK(warm.loadTile(bytes));
    CHECK(warm.prewarm(0, 0, int64_t(2 * kTestDim) - 1, int64_t(kTestDim) - 1));
    CHECK_EQ(warm.residentBlockCount(), size_t(2)); // blocks (0,0) and (1,0)
    CHECK_EQ(warm.elevationMm(int64_t(kTestDim) + 5, 9), expectMm(1, 0, 5, 9));
    CHECK_EQ(warm.residentBlockCount(), size_t(2)); // no new decode
    CHECK(!warm.prewarm(int64_t(kFineTileSize), 0, int64_t(kFineTileSize), 0)); // missing tile
    CHECK(!warm.prewarm(10, 10, 0, 0)); // inverted rect
}

VXC_TEST(amplifier_over_fine_control_lattice_is_deterministic) {
    // The point of FineTileSampler being an ITileSampler: the v9 carrier in
    // amplifier.cpp — which IS docs/vxtl-v2-format.md §8's normative spline,
    // same q10 fraction, same 6*1024^3 weights, same two-stage separable
    // evaluation with truncating division — evaluates directly on this
    // lattice. There is deliberately no second C++ implementation of that
    // spline for the two to drift apart.
    constexpr uint64_t seed = 20260729;
    V2Params p;
    p.seed = seed;
    p.baseOffsetMm = 0;
    p.quant = 1;
    // A perfectly flat control lattice at 1000 * 100 mm = 100 m.
    const std::vector<uint8_t> bytes = buildFineTile(p, makeFlatElevPlans(1000));

    // A second flat lattice exactly 50 m higher, for the exact-offset check
    // below.
    V2Params pHigh = p;
    const std::vector<uint8_t> bytesHigh = buildFineTile(pHigh, makeFlatElevPlans(1500));

    FineTileSampler samplerA(seed), samplerB(seed), samplerHigh(seed);
    CHECK(samplerA.loadTile(bytes));
    CHECK(samplerB.loadTile(bytes));
    CHECK(samplerHigh.loadTile(bytesHigh));
    CHECK_EQ(samplerA.pixelSizeMm(), 1875);

    Amplifier ampA(seed, samplerA), ampB(seed, samplerB), ampHigh(seed, samplerHigh);

    // Amplifier::column takes VOXEL coordinates (100 mm each), so these span
    // 40 m .. 400 m, i.e. fine pixels 21..213 at 1875 mm — stencils 20..215,
    // well inside the one loaded 8192-pixel tile, the same margin the v1 test
    // keeps for the v9 stencil contract.
    Digest digestA, digestB;
    for (int64_t vy = 400; vy <= 4000; vy += 400) {
        for (int64_t vx = 400; vx <= 4000; vx += 400) {
            const ColumnSample ca = ampA.column(vx, vy);
            const ColumnSample cb = ampB.column(vx, vy);
            digestA.u32(static_cast<uint32_t>(ca.surfaceMm));
            digestA.u8(ca.surfaceMat);
            digestB.u32(static_cast<uint32_t>(cb.surfaceMm));
            digestB.u8(cb.surfaceMat);

            // A B-spline's weights sum exactly to their denominator, so over a
            // perfectly CONSTANT control lattice the carrier reproduces that
            // constant exactly: baseMm == 100000 with no interpolation error
            // and an exactly zero analytic gradient, leaving only the detail
            // octaves on top. With slope 0 those are scaled by
            // slopeScaleQ10(0) = 512 and microScaleQ10(0) = 768, so the bound
            // is 3698*512/1024 + 747*768/1024 = 2409 mm. This is the DERIVED
            // bound, not the v1 test's empirical +/-2000: the detail octaves
            // are hashed on world position, so a 1875 mm pixel grid draws
            // different values than a 30 m one and the empirical band does not
            // carry over.
            CHECK(ca.surfaceMm >= 100000 - 2409);
            CHECK(ca.surfaceMm <= 100000 + 2409);

            // The lattice really drives the carrier, and exactly. Raising
            // every control point by 500 LSB = 50 m moves the carrier by 50 m
            // and nothing else: the gradient is still zero, so both detail
            // bands are scaled identically and their world-position hashes are
            // unchanged. Any interpolation error, unit slip or off-by-one in
            // the lattice would show up here as a non-50000 difference.
            const ColumnSample ch = ampHigh.column(vx, vy);
            CHECK_EQ(ch.surfaceMm - ca.surfaceMm, 50000);
        }
    }
    CHECK_EQ(digestA.h, digestB.h);
    CHECK_EQ(samplerA.missingTileQueries.load(), uint64_t(0));
    CHECK_EQ(samplerB.missingTileQueries.load(), uint64_t(0));
    CHECK_EQ(samplerA.blockDecodeFailures.load(), uint64_t(0));
    CHECK_EQ(samplerHigh.missingTileQueries.load(), uint64_t(0));
}

VXC_TEST(vxtl_v2_resid_bits_is_ignored_outside_coded_blocks) {
    // Settled interop rule: the encoder writes resid_bits = 0 for CONSTANT and
    // RAW blocks, where §5 gives it no meaning. A decoder that validated it
    // against {16, 32} unconditionally would reject every real tile, so the
    // field must be ignored in those modes — including a nonsense value.
    const V2Params p;
    const std::vector<uint8_t> good = buildFineTile(p, makeMixedElevPlans());
    constexpr size_t kIdx = 43 + 2 * 20;
    constexpr size_t kEntry = 20;

    // As emitted: 0 for the CONSTANT block 0 and the RAW block 3, 16/32 for
    // the two CODED blocks.
    CHECK_EQ(int(good[kIdx + 15]), 0);
    CHECK_EQ(int(good[kIdx + 1 * kEntry + 15]), 16);
    CHECK_EQ(int(good[kIdx + 2 * kEntry + 15]), 32);
    CHECK_EQ(int(good[kIdx + 3 * kEntry + 15]), 0);

    std::vector<uint8_t> b = good;
    b[kIdx + 15] = 99;              // CONSTANT
    b[kIdx + 3 * kEntry + 15] = 99; // RAW
    auto tile = FineTile::parse(b.data(), b.size());
    CHECK(tile.has_value());
    if (!tile) return;
    std::vector<int16_t> block;
    CHECK(tile->decodeElevBlock(0, 0, block));
    CHECK_EQ(block[0], constCpForBlock(0, 0));
    CHECK(tile->decodeElevBlock(3, 0, block));
    CHECK_EQ(block[7], rawCp(7, 0));
}

VXC_TEST(vxtl_v2_golden_fixture_cross_language_digest) {
    // §9 item 2: a golden fine tile encoded by PYTHON, decoded here,
    // digest-compared. Nothing in this test shares code with the encoder; it
    // is the only check that catches the two halves reading the frozen spec
    // differently. It skips (rather than fails) when the fixture is absent so
    // the decoder can land independently.
    //
    // DIGEST RECIPE, so the Python side can reproduce it byte for byte:
    // FNV-1a 64 (offset basis 0xcbf29ce484222325, prime 0x100000001b3) over
    // the decoded control points as little-endian uint16 — two byte-feeds,
    // low byte first — visiting blocks in index order (by outer, bx inner:
    // §4's row-major, x fastest) and each block in raster order.
    const std::filesystem::path path = fineFixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  SKIP vxtl_v2_golden_fixture_cross_language_digest: no %s "
                    "(Python encoder fixture, docs/vxtl-v2-format.md §9 item 2)\n",
                    path.filename().string().c_str());
        return;
    }

    auto bytes = readFileBytes(path);
    CHECK(bytes.has_value());
    CHECK_EQ(bytes->size(), size_t(524'451));
    CHECK_EQ(vxtlVersion(bytes->data(), bytes->size()).value_or(0), uint16_t(2));

    auto tile = FineTile::parse(bytes->data(), bytes->size());
    CHECK(tile.has_value());
    if (!tile) return;

    // §3 header, every field the encoder pinned.
    CHECK_EQ(tile->seed(), uint64_t(20260729));
    CHECK_EQ(tile->tileX(), 100);
    CHECK_EQ(tile->tileY(), -42);
    CHECK_EQ(int(tile->header().scale), 16);
    CHECK_EQ(tile->size(), uint32_t(512));
    CHECK_EQ(int(tile->header().blockLog2), 8);
    CHECK_EQ(int(tile->header().predictor), int(kPredMed));
    CHECK_EQ(tile->quantMm(), 100);
    CHECK_EQ(int(tile->header().codec), int(kCodecRaw));
    CHECK_EQ(int(tile->header().bakeVer), 7);
    CHECK_EQ(tile->baseOffsetMm(), 500'000);
    CHECK_EQ(int(tile->header().parentScale), 0);
    CHECK(!tile->hasFlow());
    CHECK_EQ(tile->blocksPerAxis(), uint32_t(2));
    CHECK_EQ(tile->blockDim(), uint32_t(256));

    // One block of each mode, in §4 index order (bx fastest).
    const std::vector<FineBlockEntry>& idx = tile->elevIndex();
    CHECK_EQ(idx.size(), size_t(4));
    CHECK_EQ(int(idx[0].mode), int(kBlockConstant)); // (0,0)
    CHECK_EQ(idx[0].constCp, int16_t(-1000));
    CHECK_EQ(idx[0].compLen, uint32_t(0));
    CHECK_EQ(int(idx[1].mode), int(kBlockCoded)); // (1,0), smooth sinusoid
    CHECK_EQ(int(idx[1].residBits), 16);
    CHECK_EQ(int(idx[2].mode), int(kBlockCoded)); // (0,1), the cliff
    CHECK_EQ(int(idx[2].residBits), 32);
    CHECK_EQ(int(idx[3].mode), int(kBlockRaw)); // (1,1)
    CHECK_EQ(idx[3].compLen, uint32_t(256 * 256 * 2));
    // resid_bits carries no meaning outside CODED and is written as 0.
    CHECK_EQ(int(idx[0].residBits), 0);
    CHECK_EQ(int(idx[3].residBits), 0);

    std::vector<int16_t> b00, b10, b01, b11;
    CHECK(tile->decodeElevBlock(0, 0, b00));
    CHECK(tile->decodeElevBlock(1, 0, b10));
    CHECK(tile->decodeElevBlock(0, 1, b01));
    CHECK(tile->decodeElevBlock(1, 1, b11));

    // CONSTANT: §2's formula over the whole block.
    CHECK_EQ(b00[0], int16_t(-1000));
    CHECK_EQ(b00[65535], int16_t(-1000));
    CHECK_EQ(tile->elevationMmFromCp(b00[0]), 500'000 + (-1000) * 100); // 400 m

    // CODED/16: pinned control points read off the Python decode.
    CHECK_EQ(b10[0], int16_t(300));
    CHECK_EQ(b10[5], int16_t(520));
    CHECK_EQ(b10[255], int16_t(786));
    CHECK_EQ(b10[256], int16_t(297));
    CHECK_EQ(b10[256 * 128 + 128], int16_t(124));
    CHECK_EQ(b10[65535], int16_t(192));

    // CODED/32: THE case resid_bits exists for. Row 0 is flat except a single
    // one-pixel cliff at x=128 that swings the full int16 range and back, so
    // the residual at x=129 is -65535 — outside int16. A decoder that assumed
    // 16-bit residuals reads garbage from here on and never errors.
    CHECK_EQ(b01[127], int16_t(0));
    CHECK_EQ(b01[128], int16_t(32767));
    CHECK_EQ(b01[129], int16_t(-32768));
    CHECK_EQ(b01[130], int16_t(0));
    CHECK_EQ(b01[0], int16_t(0));
    CHECK_EQ(b01[65535], int16_t(0));
    // Re-derive the residuals from the decoded lattice and confirm at least
    // one genuinely leaves int16 — i.e. resid_bits = 32 was not decorative.
    uint64_t wideResiduals = 0;
    for (uint32_t y = 0; y < 256; ++y) {
        for (uint32_t x = 0; x < 256; ++x) {
            int32_t pred;
            if (x == 0 && y == 0) pred = 0;
            else if (y == 0) pred = b01[x - 1];
            else if (x == 0) pred = b01[(y - 1) * 256];
            else pred = medPredictRef(b01[y * 256 + x - 1], b01[(y - 1) * 256 + x],
                                      b01[(y - 1) * 256 + x - 1]);
            const int32_t r = int32_t(b01[y * 256 + x]) - pred;
            if (r < -32768 || r > 32767) ++wideResiduals;
        }
    }
    CHECK(wideResiduals > 0);

    // RAW: literal control points, no predictor, no zigzag. (Read as MED
    // residuals instead, this block reconstructs to ±4.19 M — far outside
    // int16 — so the two readings are not merely different, one of them
    // cannot decode this block at all.)
    CHECK_EQ(b11[0], int16_t(-27010));
    CHECK_EQ(b11[1], int16_t(-22561));
    CHECK_EQ(b11[255], int16_t(26425));
    CHECK_EQ(b11[256], int16_t(-32386));
    CHECK_EQ(b11[65535], int16_t(-20423));

    // The whole-lattice digest, computed by the recipe above. Pinned from the
    // Python reference decode of these same bytes.
    Digest d;
    for (const std::vector<int16_t>* blk : {&b00, &b10, &b01, &b11})
        for (int16_t cp : *blk) d.u16(static_cast<uint16_t>(cp));
    CHECK_EQ(d.h, uint64_t(0xeb1b757a71c59444ull));

    // Decode is a pure function of the bytes (§7): an independent parse of the
    // same file gives the identical digest.
    auto tile2 = FineTile::parse(bytes->data(), bytes->size());
    CHECK(tile2.has_value());
    Digest d2;
    std::vector<int16_t> block;
    for (uint32_t by = 0; by < 2; ++by) {
        for (uint32_t bx = 0; bx < 2; ++bx) {
            CHECK(tile2->decodeElevBlock(bx, by, block));
            for (int16_t cp : block) d2.u16(static_cast<uint16_t>(cp));
        }
    }
    CHECK_EQ(d.h, d2.h);

    // And it goes through the sampler, at the fixture's own 512 stride: coarse
    // tile (100, -42) owns fine pixels [51200, 51712) x [-21504, -20992).
    FineTileSampler s(20260729);
    CHECK(s.loadTile(*bytes));
    CHECK_EQ(s.tileSize(), uint32_t(512));
    CHECK_EQ(s.pixelSizeMm(), 1875);
    const int64_t ox = int64_t(100) * 512, oy = int64_t(-42) * 512;
    CHECK_EQ(s.elevationMm(ox, oy), 500'000 + (-1000) * 100);
    CHECK_EQ(s.elevationMm(ox + 256, oy), 500'000 + 300 * 100);        // block (1,0)
    CHECK_EQ(s.elevationMm(ox + 128, oy + 256), 500'000 + 32767 * 100); // the cliff
    CHECK_EQ(s.elevationMm(ox + 256, oy + 256), 500'000 + (-27010) * 100); // RAW
    CHECK_EQ(s.missingTileQueries.load(), uint64_t(0));
    CHECK_EQ(s.blockDecodeFailures.load(), uint64_t(0));
    CHECK_EQ(s.elevationMm(ox - 1, oy), 0); // coarse tile (99, -42): not loaded
    CHECK_EQ(s.missingTileQueries.load(), uint64_t(1));

    // A sampler must not mix grid strides: the 8192-edge hand-built tile and
    // this 512-edge one address pixels differently.
    V2Params p;
    p.seed = 20260729;
    p.x = 0;
    p.y = 0;
    CHECK(!s.loadTile(buildFineTile(p, makeFlatElevPlans(1))));
    CHECK_EQ(s.tileCount(), size_t(1));
}

VXC_TEST(vxtl_v2_golden_flow_fixture_cross_language) {
    // §6 against the Python encoder, closing the last untested corner of the
    // contract. The flow plane carries one block of each mode with the flag
    // bits deliberately scattered at DIFFERENT positions than the log2 field
    // varies, so a decoder that conflates the two — or shifts by one — fails
    // loudly instead of passing on a plane of zeros.
    const std::filesystem::path path = fineFlowFixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  SKIP vxtl_v2_golden_flow_fixture_cross_language: no %s "
                    "(Python encoder fixture, docs/vxtl-v2-format.md §6)\n",
                    path.filename().string().c_str());
        return;
    }

    auto bytes = readFileBytes(path);
    CHECK(bytes.has_value());
    CHECK_EQ(bytes->size(), size_t(852'251));

    auto tile = FineTile::parse(bytes->data(), bytes->size());
    CHECK(tile.has_value());
    if (!tile) return;

    CHECK_EQ(tile->seed(), uint64_t(20260729));
    CHECK_EQ(tile->tileX(), 101);
    CHECK_EQ(tile->tileY(), -42);
    CHECK_EQ(tile->size(), uint32_t(512));
    CHECK_EQ(int(tile->header().blockLog2), 8);
    CHECK_EQ(tile->quantMm(), 100);
    CHECK_EQ(int(tile->header().codec), int(kCodecRaw));
    CHECK_EQ(int(tile->header().bakeVer), 7);
    CHECK_EQ(tile->baseOffsetMm(), 500'000);
    CHECK_EQ(tile->header().flags, kFineFlagFlowPresent);
    CHECK(tile->hasFlow());

    // Elevation: one smooth field, all CODED/16.
    const std::vector<FineBlockEntry>& ei = tile->elevIndex();
    CHECK_EQ(ei.size(), size_t(4));
    for (const FineBlockEntry& e : ei) {
        CHECK_EQ(int(e.mode), int(kBlockCoded));
        CHECK_EQ(int(e.residBits), 16);
        CHECK_EQ(e.compLen, uint32_t(256 * 256 * 2));
    }

    // Flow: one block of each mode, in §4 index order (bx fastest).
    const std::vector<FineBlockEntry>& fi = tile->flowIndex();
    CHECK_EQ(fi.size(), size_t(4));
    CHECK_EQ(int(fi[0].mode), int(kBlockConstant)); // (0,0)
    CHECK_EQ(fi[0].compLen, uint32_t(0));
    // const_cp is i16 on the wire but holds an UNSIGNED flow byte: 0xFF is
    // 255 (log2 = 31 with channel, bank and deposition all set), never -1.
    CHECK_EQ(fi[0].constCp, int16_t(255));
    CHECK_EQ(int(fi[1].mode), int(kBlockCoded)); // (1,0)
    CHECK_EQ(int(fi[1].residBits), 16);
    CHECK_EQ(int(fi[2].mode), int(kBlockRaw)); // (0,1)
    // RAW on the flow plane is ONE byte per pixel, not two — the encoder is
    // parameterised by element dtype, and this length is the check.
    CHECK_EQ(fi[2].compLen, uint32_t(256 * 256 * 1));
    CHECK_EQ(int(fi[2].residBits), 0);
    CHECK_EQ(int(fi[3].mode), int(kBlockCoded)); // (1,1)

    std::vector<int16_t> e00, e10, e01, e11;
    CHECK(tile->decodeElevBlock(0, 0, e00));
    CHECK(tile->decodeElevBlock(1, 0, e10));
    CHECK(tile->decodeElevBlock(0, 1, e01));
    CHECK(tile->decodeElevBlock(1, 1, e11));
    CHECK_EQ(e00[0], int16_t(300));
    CHECK_EQ(e00[1], int16_t(346));
    CHECK_EQ(e00[255], int16_t(786));
    CHECK_EQ(e00[256], int16_t(297));
    CHECK_EQ(e00[65535], int16_t(192));
    CHECK_EQ(e10[0], int16_t(744));
    CHECK_EQ(e10[65535], int16_t(819));
    CHECK_EQ(e01[0], int16_t(-312));
    CHECK_EQ(e01[65535], int16_t(-546));
    CHECK_EQ(e11[0], int16_t(131));
    CHECK_EQ(e11[65535], int16_t(79));

    std::vector<uint8_t> f00, f10, f01, f11;
    CHECK(tile->decodeFlowBlock(0, 0, f00));
    CHECK(tile->decodeFlowBlock(1, 0, f10));
    CHECK(tile->decodeFlowBlock(0, 1, f01));
    CHECK(tile->decodeFlowBlock(1, 1, f11));
    CHECK_EQ(f00.size(), size_t(65536));

    // CONSTANT: 255 everywhere, and 255 is the unsigned reading. A decoder
    // that sign-extended const_cp would land on -1 and be rejected outright by
    // the 0..255 range check, so this pin cannot silently pass either way.
    CHECK_EQ(int(f00[0]), 255);
    CHECK_EQ(int(f00[65535]), 255);

    // CODED flow block: same MED predictor as the elevation plane, over u8.
    CHECK_EQ(int(f10[0]), 224);
    CHECK_EQ(int(f10[1]), 65);
    CHECK_EQ(int(f10[2]), 66);
    CHECK_EQ(int(f10[3]), 67);
    CHECK_EQ(int(f10[255]), 111);
    CHECK_EQ(int(f10[256]), 33);
    CHECK_EQ(int(f10[257]), 2);
    CHECK_EQ(int(f10[65535]), 38);

    // RAW flow block: literal bytes, plus the three explicit edge pixels the
    // encoder planted at (10,10), (11,10), (12,10).
    CHECK_EQ(int(f01[0]), 0x8a);
    CHECK_EQ(int(f01[1]), 0x46);
    CHECK_EQ(int(f01[2]), 0xd8);
    CHECK_EQ(int(f01[255]), 0x80);
    CHECK_EQ(int(f01[256]), 26);
    CHECK_EQ(int(f01[65535]), 0x5e);
    CHECK_EQ(int(f01[10 * 256 + 10]), 0xff); // log2 31 + channel + bank + deposition
    CHECK_EQ(int(f01[10 * 256 + 11]), 0x60); // channel | bank only, log2 0
    CHECK_EQ(int(f01[10 * 256 + 12]), 0x80); // deposition only, log2 0

    CHECK_EQ(int(f11[0]), 160);
    CHECK_EQ(int(f11[1]), 35);
    CHECK_EQ(int(f11[255]), 41);
    CHECK_EQ(int(f11[256]), 37);
    CHECK_EQ(int(f11[65535]), 143);

    // §6's field split, counted rather than spot-checked: bits 0-4 are the
    // log2 accumulation and bits 5/6/7 are channel/bank/deposition. These
    // tallies pin that the flag bits and the log2 field are independent — an
    // off-by-one shift anywhere in the byte moves every one of them, and
    // f11's zero bank count is a value a masking bug cannot reproduce.
    struct FlowStats {
        uint64_t channel = 0, bank = 0, deposition = 0;
        int32_t log2Min = 255, log2Max = -1;
    };
    auto stats = [](const std::vector<uint8_t>& b) {
        FlowStats s;
        for (uint8_t v : b) {
            if (v & 0x20u) ++s.channel;
            if (v & 0x40u) ++s.bank;
            if (v & 0x80u) ++s.deposition;
            const int32_t l = static_cast<int32_t>(v & 31u);
            if (l < s.log2Min) s.log2Min = l;
            if (l > s.log2Max) s.log2Max = l;
        }
        return s;
    };
    const FlowStats s10 = stats(f10), s01 = stats(f01), s11 = stats(f11);
    CHECK_EQ(s10.channel, uint64_t(4096));
    CHECK_EQ(s10.bank, uint64_t(3072));
    CHECK_EQ(s10.deposition, uint64_t(2259));
    CHECK_EQ(s10.log2Min, 0);
    CHECK_EQ(s10.log2Max, 23);
    CHECK_EQ(s01.channel, uint64_t(32213));
    CHECK_EQ(s01.bank, uint64_t(32204));
    CHECK_EQ(s01.deposition, uint64_t(32215));
    CHECK_EQ(s01.log2Max, 31); // §6 clamps to 0..31; the RAW block reaches it
    CHECK_EQ(s11.channel, uint64_t(6972));
    CHECK_EQ(s11.bank, uint64_t(0));
    CHECK_EQ(s11.deposition, uint64_t(2120));
    CHECK_EQ(s11.log2Max, 26);

    // Whole-plane digests, same recipe as the elevation fixture (FNV-1a 64,
    // block index order then raster order; the flow plane feeds one byte per
    // pixel rather than a little-endian uint16).
    Digest de, df;
    for (const std::vector<int16_t>* blk : {&e00, &e10, &e01, &e11})
        for (int16_t cp : *blk) de.u16(static_cast<uint16_t>(cp));
    for (const std::vector<uint8_t>* blk : {&f00, &f10, &f01, &f11})
        for (uint8_t v : *blk) df.u8(v);
    CHECK_EQ(de.h, uint64_t(0x76798ab9a9eace98ull));
    CHECK_EQ(df.h, uint64_t(0xf49b2aaf18234d19ull));

    // The elevation plane still goes through the sampler, unaffected by the
    // flow plane's presence. Coarse tile (101, -42) owns fine pixels
    // [51712, 52224) x [-21504, -20992).
    FineTileSampler s(20260729);
    CHECK(s.loadTile(*bytes));
    const int64_t ox = int64_t(101) * 512, oy = int64_t(-42) * 512;
    CHECK_EQ(s.elevationMm(ox, oy), 500'000 + 300 * 100);
    CHECK_EQ(s.elevationMm(ox + 256, oy + 256), 500'000 + 131 * 100);
    CHECK_EQ(s.missingTileQueries.load(), uint64_t(0));

    // A corrupt file claiming const_cp = -1 for a CONSTANT flow block must be
    // REJECTED, not wrapped into a plausible 255. FLOW_INDEX is at 524491 and
    // entry 0's const_cp is at +13 (u64 offset + u32 comp_len + u8 mode).
    constexpr size_t kFlowConstCp = 524491 + 13;
    CHECK_EQ(int((*bytes)[kFlowConstCp]), 0xff);
    CHECK_EQ(int((*bytes)[kFlowConstCp + 1]), 0x00);
    {
        std::vector<uint8_t> b = *bytes;
        b[kFlowConstCp + 1] = 0xff; // 0x00ff -> 0xffff == -1
        CHECK(!FineTile::parse(b.data(), b.size()).has_value());
    }
    {
        std::vector<uint8_t> b = *bytes;
        b[kFlowConstCp + 1] = 0x01; // 0x01ff == 511: past a flow byte
        CHECK(!FineTile::parse(b.data(), b.size()).has_value());
    }
    // The same value on an ELEVATION CONSTANT block is perfectly legal, so the
    // check must be per-plane and not a blanket rejection of negatives.
    {
        std::vector<PlanePlan<int16_t>> plans = makeFlatElevPlans(-1);
        V2Params ep;
        const std::vector<uint8_t> eb = buildFineTile(ep, plans);
        auto t = FineTile::parse(eb.data(), eb.size());
        CHECK(t.has_value());
        std::vector<int16_t> blk;
        CHECK(t->decodeElevBlock(0, 0, blk));
        CHECK_EQ(blk[0], int16_t(-1));
    }

    // RAW on the flow plane is one byte per pixel: relabelling that block's
    // length as if it were two must fail, which is what makes the dtype
    // parameterisation an enforced contract rather than a convention.
    {
        std::vector<uint8_t> b = *bytes;
        b[524491 + 2 * 20 + 8] = 0x00; // comp_len 65536 -> 131072
        b[524491 + 2 * 20 + 9] = 0x00;
        b[524491 + 2 * 20 + 10] = 0x02;
        CHECK(!FineTile::parse(b.data(), b.size()).has_value());
    }
}
