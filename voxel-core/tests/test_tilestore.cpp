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

#include "voxelcore/detail_bedding.h" // kBeddingMaxAbsMm, for the translation test

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
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

// The golden tile carrying a SECTION_BASIN_TABLE (watershed plan P1,
// bake_ver 8), same provenance and the same skip-if-absent rule. Regenerate
// with `python terrain-service/tools/make_basin_fixture.py`, whose docstring
// lists what each of its five rows is there to break.
std::filesystem::path fineBasinFixturePath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_basins_512.vxtl";
}

// The golden tile carrying a BASIN TABLE v2 and a SECTION_HEADWATERS (water
// re-architecture Phase 1, bake_ver 24). A SECOND file rather than an edit to
// the one above, deliberately: v1 tiles are on disk in shipped namespaces, and
// the claim "this build still reads them" is worth nothing if the artefact
// that tests it follows the encoder's current default. Regenerate with
// `python terrain-service/tools/make_basin_v2_fixture.py`, whose docstring
// lists what each row and each head is there to break.
std::filesystem::path fineBasinV2FixturePath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_basins_v2_512.vxtl";
}

// The golden tiles carrying a SECTION_WATER_* plane (watershed plan P2,
// bake_ver 9), one per codec. Regenerate with
// `python terrain-service/tools/make_water_fixture.py`, whose docstring lists
// what each property of the plane is there to break. The ZSTD twin matters
// more here than elsewhere: 24 of the 38 resident production tiles are
// CODEC_ZSTD, so a decoder that only ever saw the RAW form would be untested
// against most of the world.
std::filesystem::path fineWaterFixturePath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_water_512.vxtl";
}
std::filesystem::path fineWaterZstdFixturePath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) /
           "vxtl_v2_golden_water_zstd_512.vxtl";
}

// The golden tiles carrying the SECTION_BATHY_* PAIR (bake_ver 27), one per
// codec. Regenerate with `python tools/make_bathy_fixture.py`, whose docstring
// lists what each property of the pair is there to break -- and which builds
// both planes by calling bake/basins.py's own `bathymetry_planes`, so the
// fixture pins the PRODUCER rather than a second copy of it.
std::filesystem::path fineBathyFixturePath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_bathy_512.vxtl";
}
std::filesystem::path fineBathyZstdFixturePath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_bathy_zstd_512.vxtl";
}

// The golden tile carrying the five SECTION_PLACE_* planes (bake_ver 28).
// Regenerate with `python terrain-service/tools/make_placement_fixture.py`,
// whose plane contents are FORMULAS the tests below recompute independently --
// the two halves share the format document and no code.
std::filesystem::path finePlacementFixturePath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_placement_512.vxtl";
}

// The CODEC_ZSTD twin of vxtl_v2_golden_512.vxtl: the SAME control lattice,
// block mode for block mode, with each block's payload wrapped in its own zstd
// frame. Regenerate both from the CODEC_RAW golden with
//   python terrain-service/tools/make_v2_zstd_fixture.py
// which also verifies the frames against the real zstandard decoder when it is
// installed. The point of the pair is that decoding it must reproduce the
// CODEC_RAW golden's whole-lattice digest exactly — same numbers, different
// codec — which no length or structural check can fake.
std::filesystem::path fineZstdFixturePath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_zstd_512.vxtl";
}

// The same lattice again, but compressed by a REAL libzstd at level 19, so its
// frames carry Compressed_Blocks and reading them needs the whole entropy
// stage (Huffman literals, FSE sequences) that a Raw_Block frame has none of.
// Regenerated by the same script, which refuses to write this one unless the
// output actually contains a Compressed_Block.
//
// Only the -DVXC_WITH_ZSTD=ON build can decode it; the default build uses it to
// prove the thing it cannot decode — see
// vxtl_v2_zstd_real_fixture_is_genuinely_entropy_coded, which is exactly the
// check CI can make without owning a compression library.
std::filesystem::path fineZstdRealFixturePath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_zstd_real_512.vxtl";
}

// Whole-lattice digest of the 512-edge golden, pinned by the CODEC_RAW golden
// test and then required of every codec claiming to carry the same numbers.
// One named constant rather than three literals: the equality between them IS
// the conformance claim, and it should not be possible to update one of them.
constexpr uint64_t kGolden512Digest = 0xeb1b757a71c59444ull;

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

// ---------------------------------------------------------------------------
// A zstd frame writer/reader restricted to Raw_Blocks (RFC 8878 §3.1.1), so
// this file can exercise CODEC_ZSTD end to end with NO compression library
// anywhere near voxel-core.
//
// That is not a workaround, it is the design being tested. §3 puts the
// decompressor at the host boundary because voxel-core has zero third-party
// dependencies and is linked into a UE binary that already ships zstd; a
// second static copy would be an ODR hazard, not just bloat. So the thing
// under test is the INJECTION and the validation either side of it, and the
// right decompressor for that is a real-but-minimal one this file owns.
//
// The frames written here are conformant zstd — the committed
// vxtl_v2_golden_zstd_512.vxtl fixture is built with the same subset by
// terrain-service/tools/vxtl_zstd_store.py, and that script verifies its
// output against the REAL zstandard decoder whenever it is installed. So a
// Raw_Block frame is not a private format that only these two halves agree on.
// ---------------------------------------------------------------------------

constexpr uint32_t kZstdMagic = 0xFD2FB528u;
// Frame_Content_Size_flag = 2 (4-byte FCS, value stored directly),
// Single_Segment_flag = 1 (no Window_Descriptor), no dict id, no checksum.
constexpr uint8_t kZstdDescriptor = 0xA0;
// Block_Maximum_Size is min(Window_Size, 128 KB). A 256x256 CODED/32 payload
// is 256 KB, so real fixture frames DO span several blocks.
constexpr size_t kZstdBlockMax = 128 * 1024;

std::vector<uint8_t> zstdStoreFrame(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    ByteWriter w(out);
    w.u32(kZstdMagic);
    w.u8(kZstdDescriptor);
    w.u32(static_cast<uint32_t>(payload.size()));
    size_t i = 0;
    for (;;) {
        const size_t take = std::min(kZstdBlockMax, payload.size() - i);
        const bool last = (i + take) >= payload.size();
        const uint32_t header =
            (last ? 1u : 0u) | (0u << 1) /* Raw_Block */ | (static_cast<uint32_t>(take) << 3);
        w.u8(static_cast<uint8_t>(header));
        w.u8(static_cast<uint8_t>(header >> 8));
        w.u8(static_cast<uint8_t>(header >> 16));
        out.insert(out.end(), payload.begin() + i, payload.begin() + i + take);
        i += take;
        if (last) break;
    }
    return out;
}

// Counters so a test can assert the PER-BLOCK property directly: one call, and
// the src window is exactly that block's frame — no neighbouring bytes, no
// state carried between calls (§4).
struct TestDecompressorState {
    uint64_t calls = 0;
    uint64_t rejects = 0;
    size_t lastSrcLen = 0;
    size_t lastDstLen = 0;
    // Refuse every frame, to exercise what the decoder does when the injected
    // decompressor says no — as a real zstd would on a corrupt frame.
    bool forceFail = false;
};

// The injected FineDecompressor. Strict on purpose: anything outside the
// Raw_Block subset, and any frame whose content is not EXACTLY dstLen bytes,
// is refused. Under CODEC_ZSTD comp_len is the compressed size and constrains
// nothing, so this exactness is the only length check there is.
bool testZstdInflate(void* user, const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    TestDecompressorState* st = static_cast<TestDecompressorState*>(user);
    if (st) {
        ++st->calls;
        st->lastSrcLen = srcLen;
        st->lastDstLen = dstLen;
    }
    const auto reject = [st]() {
        if (st) ++st->rejects;
        return false;
    };
    if (st && st->forceFail) return reject();
    if (srcLen < 9) return reject();
    ByteReader r(src, srcLen);
    uint32_t magic = 0, fcs = 0;
    uint8_t descriptor = 0;
    if (!r.u32(magic) || magic != kZstdMagic) return reject();
    if (!r.u8(descriptor) || descriptor != kZstdDescriptor) return reject();
    if (!r.u32(fcs) || fcs != dstLen) return reject();

    size_t off = 9, written = 0;
    for (;;) {
        if (off + 3 > srcLen) return reject();
        const uint32_t header = uint32_t(src[off]) | (uint32_t(src[off + 1]) << 8) |
                                (uint32_t(src[off + 2]) << 16);
        off += 3;
        const bool last = (header & 1u) != 0;
        const uint32_t blockType = (header >> 1) & 3u;
        const size_t size = header >> 3;
        if (blockType != 0) return reject(); // only Raw_Block
        if (off + size > srcLen) return reject();
        if (written + size > dstLen) return reject();
        for (size_t k = 0; k < size; ++k) dst[written + k] = src[off + k];
        off += size;
        written += size;
        if (last) break;
    }
    if (off != srcLen) return reject();
    if (written != dstLen) return reject();
    return true;
}

FineDecompressor testDecompressor(TestDecompressorState& st) {
    FineDecompressor d;
    d.fn = &testZstdInflate;
    d.user = &st;
    return d;
}

// How one block's payload becomes the bytes stored in the DATA section: identity
// under CODEC_RAW, a frame writer under CODEC_ZSTD. A parameter rather than a
// hardcoded call so the SAME hand-rolled encoder can emit Raw_Block frames (no
// compression library, the default everywhere) or genuinely entropy-coded ones
// (VXC_WITH_ZSTD, one test) without the two paths differing in anything else.
using FrameWriter = std::vector<uint8_t> (*)(const std::vector<uint8_t>&);

template <typename T>
struct PlanePlan {
    uint8_t mode = 0;       // 0 CONSTANT, 1 CODED, 2 RAW
    uint8_t residBits = 16; // 16 or 32
    int16_t constCp = 0;
    std::vector<T> v; // blockPixels samples, row-major x fastest (CODED/RAW)
};

// Encodes one plane's §4 index + data sections. dim is the block edge in
// pixels; plans is blocksPerAxis^2 entries, row-major with x fastest. With
// `frame` set, each block's payload is wrapped in its OWN frame — one frame per
// block, no dictionary, nothing shared (§4).
template <typename T>
void encodePlane(const std::vector<PlanePlan<T>>& plans, uint32_t dim, FrameWriter frame,
                 std::vector<uint8_t>& index, std::vector<uint8_t>& data) {
    const uint32_t n = dim * dim;
    ByteWriter iw(index);
    for (const PlanePlan<T>& p : plans) {
        uint64_t offset = 0;
        uint32_t compLen = 0;
        std::vector<uint8_t> payload;
        ByteWriter dw(payload);
        if (p.mode == 2) { // RAW: literal samples, no predictor, no zigzag
            for (uint32_t i = 0; i < n; ++i) {
                if constexpr (sizeof(T) == 2) {
                    dw.u16(static_cast<uint16_t>(static_cast<int16_t>(p.v[i])));
                } else {
                    dw.u8(static_cast<uint8_t>(p.v[i]));
                }
            }
        } else if (p.mode == 1) { // CODED: MED residuals, zigzag, LE
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
        }
        if (p.mode != 0) {
            const std::vector<uint8_t> stored = frame ? frame(payload) : payload;
            offset = static_cast<uint64_t>(data.size());
            data.insert(data.end(), stored.begin(), stored.end());
            compLen = static_cast<uint32_t>(stored.size());
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
// `frameWriter` overrides how CODEC_ZSTD payloads are framed; nullptr means
// this file's Raw_Block writer, which is what every test that does not need a
// compression library uses. It is ignored under CODEC_RAW.
std::vector<uint8_t> buildFineTile(const V2Params& p,
                                   const std::vector<PlanePlan<int16_t>>& elevPlans,
                                   const std::vector<PlanePlan<uint8_t>>& flowPlans = {},
                                   FrameWriter frameWriter = nullptr) {
    const uint32_t dim = 1u << p.blockLog2;
    const FrameWriter frame =
        p.codec == 1 ? (frameWriter ? frameWriter : &zstdStoreFrame) : nullptr;
    std::vector<uint8_t> elevIndex, elevData, flowIndex, flowData;
    encodePlane<int16_t>(elevPlans, dim, frame, elevIndex, elevData);
    const bool hasFlow = !flowPlans.empty();
    if (hasFlow) encodePlane<uint8_t>(flowPlans, dim, frame, flowIndex, flowData);

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
    // CODEC_ZSTD is a valid part of the format, but no decompressor is injected
    // on this call, so the tile is refused whole rather than half-loaded. The
    // two refusals are distinguishable — see
    // vxtl_v2_zstd_without_a_decompressor_is_refused_loudly.
    rejects(28, 1);
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
    // 120 m .. 480 m, i.e. fine pixels 64..256 at 1875 mm.
    //
    // v13 MOVED THE WINDOW IN, from 40 m, and the reason is a real widening of
    // the read contract rather than a flaky test. The detail gate now reads the
    // raster's relief at a 30 m PHYSICAL baseline, which is +/-16 pixels on this
    // tier (carrier.h), and Amplifier::column ALSO evaluates the surface at a
    // cavern site's own xy up to kCavernMaxReachMm (~19 pixels) away -- so a
    // column at pixel 21 could reach pixel -14, off the single loaded tile, and
    // the missingTileQueries check below caught it. Starting at pixel 64 keeps
    // the whole read set inside the tile with room to spare, which is what this
    // test has always been about.
    Digest digestA, digestB;
    for (int64_t vy = 1200; vy <= 4800; vy += 400) {
        for (int64_t vx = 1200; vx <= 4800; vx += 400) {
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

            // The lattice really drives the carrier. Raising every control
            // point by 500 LSB = 50 m moves the carrier by exactly 50 m: the
            // gradient is still zero, so both detail bands are scaled
            // identically and their world-POSITION hashes are unchanged.
            //
            // v10 makes this an interval rather than an equality, and the
            // reason is a real property of the terrain rather than a slackening
            // of the test. The bedding term is a function of ABSOLUTE
            // ELEVATION -- bedding planes are geological, they sit at fixed
            // heights and the ground cuts through them -- so translating the
            // whole surface up 50 m genuinely slides it through the beds and
            // changes the outcrop pattern. That is the term working, not an
            // error, and a test demanding exact translation invariance would be
            // demanding that rock strata follow the terrain up and down.
            //
            // The residual is therefore bounded by the difference of two
            // bedding samples, i.e. 2 * kBeddingMaxAbsMm = 640 mm. Everything
            // else must still translate exactly, so this stays a sharp test of
            // the lattice: an interpolation error, unit slip or off-by-one
            // would blow straight through 640 mm.
            const ColumnSample ch = ampHigh.column(vx, vy);
            const int64_t shift = int64_t(ch.surfaceMm) - int64_t(ca.surfaceMm);
            CHECK(shift >= 50000 - 2 * kBeddingMaxAbsMm);
            CHECK(shift <= 50000 + 2 * kBeddingMaxAbsMm);
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
    CHECK_EQ(d.h, kGolden512Digest);

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

// ===========================================================================
// CODEC_ZSTD (docs/vxtl-v2-format.md §3): the decompressor is INJECTED at the
// host boundary, never linked into voxel-core.
//
// Everything below runs with NO compression library present. The injected
// decompressor is this file's own Raw_Block zstd reader (zstdStoreFrame /
// testZstdInflate above), which is exactly the shape the UE module uses when
// it hands over the engine's zstd instead.
// ===========================================================================

namespace {
// Offsets into a hand-built 8192-edge, blockLog2=8 tile, from §3's layout.
constexpr size_t kElevIndexOff = 43 + 2 * 20;                     // 83
constexpr size_t kElevDataOff = kElevIndexOff + kTestBlocks * 20; // 20563
constexpr size_t kEntryBytes = 20;
} // namespace

VXC_TEST(vxtl_v2_zstd_decodes_identically_to_codec_raw) {
    // THE equivalence that makes a codec a codec: the same lattice, encoded
    // once as CODEC_RAW and once as CODEC_ZSTD, must decode to identical
    // control points. Nothing about the values may depend on how they were
    // stored — and a whole-lattice digest is what proves it, because no
    // length, bounds or structural check can (§4: a literal int16 plane and a
    // resid_bits=16 residual plane are the same length under either codec).
    V2Params rawP;
    rawP.seed = 20260729;
    rawP.x = -3;
    rawP.y = 5;
    rawP.baseOffsetMm = 123'400;
    V2Params zstdP = rawP;
    zstdP.codec = 1;

    const std::vector<uint8_t> rawBytes = buildFineTile(rawP, makeMixedElevPlans());
    const std::vector<uint8_t> zstdBytes = buildFineTile(zstdP, makeMixedElevPlans());

    // The zstd file is BIGGER here, and that is expected: a Raw_Block frame
    // adds 9 bytes of frame header plus 3 per zstd block and compresses
    // nothing. Size is not what this test is about — real ratios are measured
    // with a real compressor on real tiles.
    CHECK(zstdBytes.size() > rawBytes.size());

    TestDecompressorState st;
    FineError rawErr = FineError::kBadHeader, zstdErr = FineError::kBadHeader;
    auto rawTile = FineTile::parse(rawBytes.data(), rawBytes.size(), {}, &rawErr);
    auto zstdTile =
        FineTile::parse(zstdBytes.data(), zstdBytes.size(), testDecompressor(st), &zstdErr);
    CHECK(rawTile.has_value());
    CHECK(zstdTile.has_value());
    CHECK(rawErr == FineError::kNone);
    CHECK(zstdErr == FineError::kNone);
    if (!rawTile || !zstdTile) return;
    CHECK_EQ(int(rawTile->codec()), int(kCodecRaw));
    CHECK_EQ(int(zstdTile->codec()), int(kCodecZstd));
    CHECK(!rawTile->decompressor().valid());
    CHECK(zstdTile->decompressor().valid());

    // Every block, both codecs, digest-compared — and the four interesting
    // blocks separately compared against what the PLANS say, so a bug that
    // corrupted both codecs identically could not pass.
    Digest dRaw, dZstd;
    uint64_t mismatches = 0;
    for (uint32_t by = 0; by < kTestPerAxis; ++by) {
        for (uint32_t bx = 0; bx < kTestPerAxis; ++bx) {
            std::vector<int16_t> a, b;
            if (!rawTile->decodeElevBlock(bx, by, a)) { ++mismatches; continue; }
            if (!zstdTile->decodeElevBlock(bx, by, b)) { ++mismatches; continue; }
            for (uint32_t i = 0; i < kTestBlockPixels; ++i) {
                dRaw.u16(static_cast<uint16_t>(a[i]));
                dZstd.u16(static_cast<uint16_t>(b[i]));
                if (a[i] != b[i]) ++mismatches;
            }
            if (by == 0 && bx < 4) {
                for (uint32_t ly = 0; ly < kTestDim; ++ly)
                    for (uint32_t lx = 0; lx < kTestDim; ++lx)
                        if (b[ly * kTestDim + lx] != expectedCp(bx, by, lx, ly)) ++mismatches;
            }
        }
    }
    CHECK_EQ(mismatches, uint64_t(0));
    CHECK_EQ(dRaw.h, dZstd.h);

    // Only the three non-CONSTANT blocks own a frame; CONSTANT blocks cost
    // zero data bytes, so the decompressor is never called for them (§4).
    CHECK_EQ(st.calls, uint64_t(3));
    CHECK_EQ(st.rejects, uint64_t(0));

    // §2's mm conversion is codec-independent too.
    int16_t cp = 0;
    CHECK(zstdTile->controlPointAt(2 * kTestDim + 4, 6, cp));
    CHECK_EQ(cp, codedWideCp(4, 6));
    CHECK_EQ(zstdTile->elevationMmFromCp(cp), 123'400 + int32_t(cp) * 100);
}

VXC_TEST(vxtl_v2_zstd_without_a_decompressor_is_refused_loudly) {
    // The requirement FineError exists for: with no decompressor registered, a
    // CODEC_ZSTD tile must be refused with a DISTINGUISHABLE error, never
    // loaded and left to answer zeros. Silently flat terrain under a client
    // that believes it has the fine tier is a desync, not a glitch — and
    // "kNoDecompressor" vs "kBadBlockIndex" is the difference between fixing
    // host wiring and chasing a corrupt bake.
    V2Params zstdP;
    zstdP.codec = 1;
    const std::vector<uint8_t> zstdBytes = buildFineTile(zstdP, makeMixedElevPlans());

    FineError err = FineError::kNone;
    CHECK(!FineTile::parse(zstdBytes.data(), zstdBytes.size(), {}, &err).has_value());
    CHECK(err == FineError::kNoDecompressor);
    // The default-argument form — every pre-existing caller — behaves the same.
    CHECK(!FineTile::parse(zstdBytes.data(), zstdBytes.size()).has_value());

    // A codec byte that is not part of the format at all is a DIFFERENT
    // failure: bytes this build does not understand, not a host that forgot to
    // wire something up.
    {
        std::vector<uint8_t> b = zstdBytes;
        b[28] = 9;
        err = FineError::kNone;
        TestDecompressorState st;
        CHECK(!FineTile::parse(b.data(), b.size(), testDecompressor(st), &err).has_value());
        CHECK(err == FineError::kUnknownCodec);
        CHECK_EQ(st.calls, uint64_t(0)); // refused before any block was touched
    }

    // CODEC_RAW never needs one, injected or otherwise — voxel-core must build
    // and pass its whole suite with no compression library anywhere.
    V2Params rawP;
    const std::vector<uint8_t> rawBytes = buildFineTile(rawP, makeMixedElevPlans());
    err = FineError::kBadHeader;
    CHECK(FineTile::parse(rawBytes.data(), rawBytes.size(), {}, &err).has_value());
    CHECK(err == FineError::kNone);

    // Same rule through the sampler, which is where a host actually wires this
    // up. setDecompressor is not retroactive, so the ordering is part of the
    // contract: register first, load second.
    FineTileSampler cold(1);
    err = FineError::kNone;
    CHECK(!cold.loadTile(zstdBytes, &err));
    CHECK(err == FineError::kNoDecompressor);
    CHECK_EQ(cold.tileCount(), size_t(0));
    CHECK_EQ(cold.elevationMm(0, 0), 0); // and that reads as a MISS, not a lattice of zeros
    CHECK_EQ(cold.missingTileQueries.load(), uint64_t(1));

    TestDecompressorState st;
    FineTileSampler warm(1);
    warm.setDecompressor(testDecompressor(st));
    CHECK(warm.decompressor().valid());
    err = FineError::kBadHeader;
    CHECK(warm.loadTile(zstdBytes, &err));
    CHECK(err == FineError::kNone);
    CHECK_EQ(warm.tileCount(), size_t(1));
    CHECK_EQ(warm.elevationMm(int64_t(kTestDim) + 5, 9), int32_t(codedSmallCp(5, 9)) * 100);
    CHECK_EQ(warm.missingTileQueries.load(), uint64_t(0));
    CHECK_EQ(warm.blockDecodeFailures.load(), uint64_t(0));

    // Reason codes have stable names for logs.
    CHECK(std::string(fineErrorName(FineError::kNoDecompressor)) == "no-decompressor");
    CHECK(std::string(fineErrorName(FineError::kUnknownCodec)) == "unknown-codec");
    CHECK(std::string(fineErrorName(FineError::kNone)) == "none");
}

VXC_TEST(vxtl_v2_zstd_rejects_corrupt_and_truncated_frames) {
    // Under CODEC_ZSTD comp_len is the COMPRESSED length, so it constrains
    // nothing about the contents: every length check that mattered under
    // CODEC_RAW is gone, and "the frame expands to exactly the size the header
    // implies" is all that is left. These cases are that check, from both
    // sides of it.
    V2Params p;
    p.codec = 1;
    const std::vector<uint8_t> good = buildFineTile(p, makeMixedElevPlans());
    TestDecompressorState st;
    auto ok = FineTile::parse(good.data(), good.size(), testDecompressor(st));
    CHECK(ok.has_value());
    if (!ok) return;

    // Block 1 (CODED/16) is the first block that owns a frame.
    const FineBlockEntry e1 = ok->elevIndex()[1];
    const size_t frame1 = kElevDataOff + static_cast<size_t>(e1.offset);
    CHECK_EQ(int(good[frame1 + 0]), 0x28); // zstd magic 0xFD2FB528, LE
    CHECK_EQ(int(good[frame1 + 1]), 0xB5);
    CHECK_EQ(int(good[frame1 + 2]), 0x2F);
    CHECK_EQ(int(good[frame1 + 3]), 0xFD);
    // comp_len is genuinely not the plain length any more.
    CHECK(e1.compLen != uint32_t(kTestBlockPixels) * 2);

    auto decodeFails = [&](const std::vector<uint8_t>& b, uint32_t bx, FineError want) {
        TestDecompressorState s;
        auto t = FineTile::parse(b.data(), b.size(), testDecompressor(s));
        CHECK(t.has_value()); // structurally fine: the failure is inside the frame
        if (!t) return;
        std::vector<int16_t> blk;
        FineError err = FineError::kNone;
        CHECK(!t->decodeElevBlock(bx, 0, blk, &err));
        CHECK(err == want);
    };

    { // Corrupt frame magic.
        std::vector<uint8_t> b = good;
        b[frame1] ^= 0xFF;
        decodeFails(b, 1, FineError::kDecompressFailed);
    }
    { // A frame declaring the WRONG decompressed size. This is the case that
      // matters most: the bytes are otherwise a well-formed frame, and only
      // the header-derived expectation catches it.
        std::vector<uint8_t> b = good;
        b[frame1 + 5] = 0x00; // Frame_Content_Size, 4 bytes LE at +5
        b[frame1 + 6] = 0x00;
        b[frame1 + 7] = 0x00;
        b[frame1 + 8] = 0x00;
        decodeFails(b, 1, FineError::kDecompressFailed);
    }
    { // Truncated frame: shrink comp_len so the last zstd block is cut short.
      // Still non-zero and still in bounds, so parse cannot see it.
        std::vector<uint8_t> b = good;
        const uint32_t shorter = e1.compLen - 4;
        for (int i = 0; i < 4; ++i)
            b[kElevIndexOff + 1 * kEntryBytes + 8 + i] = uint8_t(shorter >> (8 * i));
        decodeFails(b, 1, FineError::kDecompressFailed);
    }
    { // Garbage INSIDE the frame payload still inflates (Raw_Blocks are
      // literal) but must then reconstruct out of int16 — the residual path's
      // own guard, unchanged by the codec.
        std::vector<uint8_t> b = good;
        b[frame1 + 12] = 0xFE; // the block's first two residual words: +32767 twice
        b[frame1 + 13] = 0xFF;
        b[frame1 + 14] = 0xFE;
        b[frame1 + 15] = 0xFF;
        decodeFails(b, 1, FineError::kValueOutOfRange);
    }
    { // comp_len 0 on a block that must own a frame: rejected at PARSE, since
      // an empty frame cannot expand to anything and a block that owns no
      // bytes is CONSTANT by definition.
        std::vector<uint8_t> b = good;
        for (int i = 8; i < 12; ++i) b[kElevIndexOff + 1 * kEntryBytes + i] = 0;
        TestDecompressorState s;
        FineError err = FineError::kNone;
        CHECK(!FineTile::parse(b.data(), b.size(), testDecompressor(s), &err).has_value());
        CHECK(err == FineError::kBadBlockIndex);
    }
    { // A frame running past the end of ELEV_DATA is still caught at parse:
      // bounds are the one thing comp_len can still be checked against.
        std::vector<uint8_t> b = good;
        b[kElevIndexOff + 1 * kEntryBytes + 10] = 0xFF;
        TestDecompressorState s;
        FineError err = FineError::kNone;
        CHECK(!FineTile::parse(b.data(), b.size(), testDecompressor(s), &err).has_value());
        CHECK(err == FineError::kBadBlockIndex);
    }
    { // A decompressor that simply refuses — what a real zstd does on a frame
      // it dislikes — fails the block cleanly and bumps the sampler's decode
      // counter. It does NOT fall back to reading the frame as literal bytes.
        TestDecompressorState s;
        s.forceFail = true;
        FineTileSampler sampler(1);
        sampler.setDecompressor(testDecompressor(s));
        CHECK(sampler.loadTile(good));
        CHECK_EQ(sampler.elevationMm(int64_t(kTestDim) + 5, 9), 0);
        CHECK_EQ(sampler.blockDecodeFailures.load(), uint64_t(1));
        CHECK_EQ(sampler.missingTileQueries.load(), uint64_t(0));
        // A CONSTANT block owns no frame, so it still decodes: per-block
        // independence cuts both ways.
        CHECK_EQ(sampler.elevationMm(0, 0), int32_t(constCpForBlock(0, 0)) * 100);
    }

    // THE RECORDED TRAP, restated for CODEC_ZSTD. Relabel the RAW block
    // (bx=3) as CODED/16: its payload is 2 bytes/px either way, so the frame's
    // declared content size still matches and NOTHING structural can reject
    // it. The tile parses, the block "decodes", and the values are simply
    // wrong. That is why `mode` is authoritative and why the cross-language
    // digest test below is the check that matters — a length check is not a
    // correctness check.
    {
        std::vector<uint8_t> b = good;
        b[kElevIndexOff + 3 * kEntryBytes + 12] = 1;  // mode RAW -> CODED
        b[kElevIndexOff + 3 * kEntryBytes + 15] = 16; // resid_bits
        TestDecompressorState s;
        auto t = FineTile::parse(b.data(), b.size(), testDecompressor(s));
        CHECK(t.has_value());
        if (t) {
            std::vector<int16_t> blk;
            // It may or may not reconstruct in range. What must NOT happen is
            // it coming back as the correct literal lattice.
            if (t->decodeElevBlock(3, 0, blk)) CHECK(blk[7] != rawCp(7, 0));
        }
    }
}

VXC_TEST(vxtl_v2_zstd_blocks_are_independently_framed) {
    // §4's point, and the reason the fine tier is streamable at all: one frame
    // per block, no dictionary, no cross-block state. Decoding one block must
    // call the decompressor exactly once, with exactly that block's frame —
    // never the whole section, never a neighbour.
    V2Params p;
    p.codec = 1;
    const std::vector<uint8_t> bytes = buildFineTile(p, makeMixedElevPlans());
    TestDecompressorState st;
    auto tile = FineTile::parse(bytes.data(), bytes.size(), testDecompressor(st));
    CHECK(tile.has_value());
    if (!tile) return;

    const std::vector<FineBlockEntry> idx = tile->elevIndex();
    std::vector<int16_t> blk;

    CHECK(tile->decodeElevBlock(3, 0, blk)); // RAW: literal control points
    CHECK_EQ(st.calls, uint64_t(1));
    CHECK_EQ(st.lastSrcLen, size_t(idx[3].compLen));
    CHECK_EQ(st.lastDstLen, size_t(kTestBlockPixels) * 2);
    CHECK_EQ(blk[7], rawCp(7, 0));

    CHECK(tile->decodeElevBlock(0, 0, blk)); // CONSTANT: no frame at all
    CHECK_EQ(st.calls, uint64_t(1));
    CHECK_EQ(blk[0], constCpForBlock(0, 0));

    CHECK(tile->decodeElevBlock(2, 0, blk)); // CODED/32: 4 bytes/px
    CHECK_EQ(st.calls, uint64_t(2));
    CHECK_EQ(st.lastSrcLen, size_t(idx[2].compLen));
    CHECK_EQ(st.lastDstLen, size_t(kTestBlockPixels) * 4);
    CHECK_EQ(blk[6 * kTestDim + 4], codedWideCp(4, 6));

    // That 256 KB payload cannot fit one zstd block (Block_Maximum_Size is
    // 128 KB), so its frame necessarily carries several — the case a reader
    // that assumed one block per frame passes everything else on and fails.
    CHECK(idx[2].compLen > uint32_t(kTestBlockPixels) * 4 + 12);

    // Corrupting one block's frame leaves its neighbours decodable: no shared
    // state means no blast radius.
    {
        std::vector<uint8_t> b = bytes;
        b[kElevDataOff + static_cast<size_t>(idx[2].offset)] ^= 0xFF;
        TestDecompressorState s;
        auto t = FineTile::parse(b.data(), b.size(), testDecompressor(s));
        CHECK(t.has_value());
        if (!t) return;
        std::vector<int16_t> a;
        CHECK(!t->decodeElevBlock(2, 0, a));
        CHECK(t->decodeElevBlock(1, 0, a));
        CHECK_EQ(a[9 * kTestDim + 5], codedSmallCp(5, 9));
        CHECK(t->decodeElevBlock(3, 0, a));
        CHECK_EQ(a[7], rawCp(7, 0));
    }
}

VXC_TEST(vxtl_v2_zstd_flow_plane_round_trips) {
    // §6's u8 plane under CODEC_ZSTD. Its element width differs from the
    // elevation plane's, and so does the expected decompressed size, so a
    // decoder that hardcoded 2 bytes/px in the zstd path passes every
    // elevation test above and fails here.
    std::vector<PlanePlan<uint8_t>> flow(kTestBlocks);
    for (PlanePlan<uint8_t>& f : flow) {
        f.mode = 0;
        f.constCp = 0;
    }
    auto flowByte = [](uint32_t lx, uint32_t ly) {
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
    flow[kTestPerAxis].constCp = 0x25;

    V2Params rawP;
    V2Params zstdP;
    zstdP.codec = 1;
    const std::vector<uint8_t> rawBytes = buildFineTile(rawP, makeFlatElevPlans(120), flow);
    const std::vector<uint8_t> zstdBytes = buildFineTile(zstdP, makeFlatElevPlans(120), flow);

    TestDecompressorState st;
    auto rawTile = FineTile::parse(rawBytes.data(), rawBytes.size());
    auto zstdTile = FineTile::parse(zstdBytes.data(), zstdBytes.size(), testDecompressor(st));
    CHECK(rawTile.has_value());
    CHECK(zstdTile.has_value());
    if (!rawTile || !zstdTile) return;
    CHECK(zstdTile->hasFlow());

    // RAW on the flow plane is ONE byte per pixel, so the expected
    // decompressed size handed to the decompressor is blockPixels, not twice
    // that. Block (2,0) is the RAW flow block.
    std::vector<uint8_t> fb;
    CHECK(zstdTile->decodeFlowBlock(2, 0, fb));
    CHECK_EQ(st.lastDstLen, size_t(kTestBlockPixels));

    Digest dRaw, dZstd;
    uint64_t mismatches = 0;
    for (uint32_t by = 0; by < kTestPerAxis; ++by) {
        for (uint32_t bx = 0; bx < kTestPerAxis; ++bx) {
            std::vector<uint8_t> a, b;
            if (!rawTile->decodeFlowBlock(bx, by, a)) { ++mismatches; continue; }
            if (!zstdTile->decodeFlowBlock(bx, by, b)) { ++mismatches; continue; }
            for (uint32_t i = 0; i < kTestBlockPixels; ++i) {
                dRaw.u8(a[i]);
                dZstd.u8(b[i]);
                if (a[i] != b[i]) ++mismatches;
            }
        }
    }
    CHECK_EQ(mismatches, uint64_t(0));
    CHECK_EQ(dRaw.h, dZstd.h);

    // Spot-check against the plans, so a bug corrupting both codecs
    // identically could not pass.
    CHECK(zstdTile->decodeFlowBlock(1, 0, fb));
    CHECK_EQ(int(fb[3 * kTestDim + 5]), int(flowByte(5, 3)));
    CHECK(zstdTile->decodeFlowBlock(0, 1, fb));
    CHECK_EQ(int(fb[0]), 0x25);
}

VXC_TEST(vxtl_v2_zstd_golden_fixture_cross_language_digest) {
    // §9 item 2 for CODEC_ZSTD: a fixture written by PYTHON, decoded here with
    // an injected decompressor, digest-compared against the CODEC_RAW
    // golden's ALREADY PINNED whole-lattice digest. Same numbers, different
    // codec.
    //
    // That equality is the real proof and nothing weaker substitutes: under
    // CODEC_ZSTD comp_len says nothing, and even the decompressed length
    // cannot tell a literal int16 plane from a resid_bits=16 residual plane
    // (§4). Only the values can — against a value pinned before this codec
    // existed.
    const std::filesystem::path path = fineZstdFixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  SKIP vxtl_v2_zstd_golden_fixture_cross_language_digest: no %s "
                    "(regenerate: python terrain-service/tools/make_v2_zstd_fixture.py)\n",
                    path.filename().string().c_str());
        return;
    }

    auto bytes = readFileBytes(path);
    CHECK(bytes.has_value());
    if (!bytes) return;
    CHECK_EQ(vxtlVersion(bytes->data(), bytes->size()).value_or(0), uint16_t(2));

    // Without a decompressor this file is refused whole, so the fixture proves
    // the deployment failure mode too, not just the happy path.
    FineError err = FineError::kNone;
    CHECK(!FineTile::parse(bytes->data(), bytes->size(), {}, &err).has_value());
    CHECK(err == FineError::kNoDecompressor);

    TestDecompressorState st;
    auto tile = FineTile::parse(bytes->data(), bytes->size(), testDecompressor(st), &err);
    CHECK(tile.has_value());
    CHECK(err == FineError::kNone);
    if (!tile) return;

    // The same §3 header as the CODEC_RAW golden, except `codec`.
    CHECK_EQ(tile->seed(), uint64_t(20260729));
    CHECK_EQ(tile->tileX(), 100);
    CHECK_EQ(tile->tileY(), -42);
    CHECK_EQ(tile->size(), uint32_t(512));
    CHECK_EQ(int(tile->header().blockLog2), 8);
    CHECK_EQ(tile->quantMm(), 100);
    CHECK_EQ(int(tile->header().codec), int(kCodecZstd));
    CHECK_EQ(int(tile->header().bakeVer), 7);
    CHECK_EQ(tile->baseOffsetMm(), 500'000);
    CHECK(!tile->hasFlow());

    // The same four modes at the same four positions.
    const std::vector<FineBlockEntry> idx = tile->elevIndex();
    CHECK_EQ(idx.size(), size_t(4));
    CHECK_EQ(int(idx[0].mode), int(kBlockConstant));
    CHECK_EQ(idx[0].constCp, int16_t(-1000));
    CHECK_EQ(idx[0].compLen, uint32_t(0)); // CONSTANT owns no frame
    CHECK_EQ(int(idx[1].mode), int(kBlockCoded));
    CHECK_EQ(int(idx[1].residBits), 16);
    CHECK_EQ(int(idx[2].mode), int(kBlockCoded));
    CHECK_EQ(int(idx[2].residBits), 32);
    CHECK_EQ(int(idx[3].mode), int(kBlockRaw));
    CHECK_EQ(int(idx[3].residBits), 0);
    // comp_len is a FRAME length now, so none of these equal the plain sizes
    // the CODEC_RAW golden pins.
    CHECK(idx[1].compLen != uint32_t(256 * 256 * 2));
    CHECK(idx[2].compLen != uint32_t(256 * 256 * 4));
    CHECK(idx[3].compLen != uint32_t(256 * 256 * 2));

    std::vector<int16_t> b00, b10, b01, b11;
    CHECK(tile->decodeElevBlock(0, 0, b00));
    CHECK(tile->decodeElevBlock(1, 0, b10));
    CHECK(tile->decodeElevBlock(0, 1, b01));
    CHECK(tile->decodeElevBlock(1, 1, b11));
    CHECK_EQ(st.calls, uint64_t(3)); // one per non-CONSTANT block, no more
    CHECK_EQ(st.rejects, uint64_t(0));

    // The same pinned control points the CODEC_RAW golden test asserts.
    CHECK_EQ(b00[0], int16_t(-1000));
    CHECK_EQ(b00[65535], int16_t(-1000));
    CHECK_EQ(b10[0], int16_t(300));
    CHECK_EQ(b10[256 * 128 + 128], int16_t(124));
    CHECK_EQ(b01[128], int16_t(32767));
    CHECK_EQ(b01[129], int16_t(-32768));
    CHECK_EQ(b11[0], int16_t(-27010));
    CHECK_EQ(b11[65535], int16_t(-20423));

    // And the whole-lattice digest, pinned by the CODEC_RAW golden.
    Digest d;
    for (const std::vector<int16_t>* blk : {&b00, &b10, &b01, &b11})
        for (int16_t cp : *blk) d.u16(static_cast<uint16_t>(cp));
    CHECK_EQ(d.h, kGolden512Digest);

    // Value-for-value against the CODEC_RAW golden itself when it is present:
    // the strongest form of "the codec changed the bytes and nothing else".
    const std::filesystem::path rawPath = fineFixturePath();
    if (std::filesystem::exists(rawPath)) {
        auto rawBytes = readFileBytes(rawPath);
        CHECK(rawBytes.has_value());
        auto rawTile = FineTile::parse(rawBytes->data(), rawBytes->size());
        CHECK(rawTile.has_value());
        if (rawTile) {
            uint64_t mismatches = 0;
            std::vector<int16_t> a;
            const std::vector<int16_t>* zs[4] = {&b00, &b10, &b01, &b11};
            for (uint32_t i = 0; i < 4; ++i) {
                const uint32_t bx = i & 1u, by = i >> 1;
                if (!rawTile->decodeElevBlock(bx, by, a)) { ++mismatches; continue; }
                if (a.size() != zs[i]->size()) { ++mismatches; continue; }
                for (size_t k = 0; k < a.size(); ++k)
                    if (a[k] != (*zs[i])[k]) ++mismatches;
            }
            CHECK_EQ(mismatches, uint64_t(0));
        }
    }

    // Through the sampler, at the fixture's own 512 stride, exactly as the
    // CODEC_RAW golden does. Coarse tile (100, -42) owns fine pixels
    // [51200, 51712) x [-21504, -20992).
    TestDecompressorState st2;
    FineTileSampler s(20260729);
    s.setDecompressor(testDecompressor(st2));
    CHECK(s.loadTile(*bytes));
    CHECK_EQ(s.tileSize(), uint32_t(512));
    const int64_t ox = int64_t(100) * 512, oy = int64_t(-42) * 512;
    CHECK_EQ(s.elevationMm(ox, oy), 500'000 + (-1000) * 100);
    CHECK_EQ(s.elevationMm(ox + 256, oy), 500'000 + 300 * 100);
    CHECK_EQ(s.elevationMm(ox + 128, oy + 256), 500'000 + 32767 * 100);
    CHECK_EQ(s.elevationMm(ox + 256, oy + 256), 500'000 + (-27010) * 100);
    CHECK_EQ(s.missingTileQueries.load(), uint64_t(0));
    CHECK_EQ(s.blockDecodeFailures.load(), uint64_t(0));

    // Decode is a pure function of the bytes (§7): a second, independent parse
    // with a second decompressor instance gives the identical digest.
    TestDecompressorState st3;
    auto tile2 = FineTile::parse(bytes->data(), bytes->size(), testDecompressor(st3));
    CHECK(tile2.has_value());
    if (!tile2) return;
    Digest d2;
    std::vector<int16_t> block;
    for (uint32_t by = 0; by < 2; ++by) {
        for (uint32_t bx = 0; bx < 2; ++bx) {
            CHECK(tile2->decodeElevBlock(bx, by, block));
            for (int16_t cp : block) d2.u16(static_cast<uint16_t>(cp));
        }
    }
    CHECK_EQ(d.h, d2.h);
}

VXC_TEST(vxtl_v2_zstd_real_fixture_is_genuinely_entropy_coded) {
    // This test runs in the DEFAULT build, which has no compression library —
    // and that is the point. It cannot decode the real-zstd fixture, so it
    // asserts the one thing it can: that the fixture is genuinely
    // entropy-coded, i.e. that the -DVXC_WITH_ZSTD=ON test below is testing
    // something a Raw_Block frame does not.
    //
    // The evidence is that this file's own Raw_Block reader REFUSES two of the
    // three frames. A Raw_Block reader accepts every stored frame and only
    // stored frames, so "refused" means "not stored", which means the bytes
    // went through zstd's entropy stage. Size alone would not prove it (a
    // stored frame can be small if the payload is), and the codec byte proves
    // only what the header claims.
    const std::filesystem::path path = fineZstdRealFixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  SKIP vxtl_v2_zstd_real_fixture_is_genuinely_entropy_coded: no %s "
                    "(regenerate: python terrain-service/tools/make_v2_zstd_fixture.py, "
                    "which needs `zstandard`)\n",
                    path.filename().string().c_str());
        return;
    }
    auto bytes = readFileBytes(path);
    CHECK(bytes.has_value());
    if (!bytes) return;
    CHECK_EQ(vxtlVersion(bytes->data(), bytes->size()).value_or(0), uint16_t(2));

    // Real compression, unlike the Raw_Block twin which is very slightly LARGER
    // than the CODEC_RAW golden it mirrors.
    auto rawGolden = readFileBytes(fineFixturePath());
    if (rawGolden) CHECK(bytes->size() * 3 < rawGolden->size());

    // No decompressor: refused whole, and distinguishably so.
    FineError err = FineError::kNone;
    CHECK(!FineTile::parse(bytes->data(), bytes->size(), {}, &err).has_value());
    CHECK(err == FineError::kNoDecompressor);

    // Structure is codec-independent, so it parses fine with any decompressor.
    TestDecompressorState st;
    auto tile = FineTile::parse(bytes->data(), bytes->size(), testDecompressor(st), &err);
    CHECK(tile.has_value());
    CHECK(err == FineError::kNone);
    if (!tile) return;
    CHECK_EQ(int(tile->header().codec), int(kCodecZstd));
    CHECK_EQ(tile->size(), uint32_t(512));

    std::vector<int16_t> blk;
    // (1,0) CODED/16 and (0,1) CODED/32 both compressed, so both are refused.
    const uint32_t compressedBlocks[2][2] = {{1, 0}, {0, 1}};
    for (const auto& bc : compressedBlocks) {
        err = FineError::kNone;
        CHECK(!tile->decodeElevBlock(bc[0], bc[1], blk, &err));
        CHECK(err == FineError::kDecompressFailed);
    }
    CHECK_EQ(st.rejects, uint64_t(2));

    // (0,0) is CONSTANT: no frame at all, so per-block independence means it
    // still decodes with no decompression happening anywhere.
    const uint64_t callsBefore = st.calls;
    CHECK(tile->decodeElevBlock(0, 0, blk));
    CHECK_EQ(blk[0], int16_t(-1000));
    CHECK_EQ(st.calls, callsBefore);

    // (1,1) is the RAW block: pseudo-random int16 control points, which zstd
    // declines to compress and stores verbatim. So its frame IS a Raw_Block
    // frame and this reader decodes it correctly — worth asserting rather than
    // hiding, because it is the honest scope of what CI proves here: two of
    // the three frames are beyond this build, one is not.
    CHECK(tile->decodeElevBlock(1, 1, blk));
    CHECK_EQ(blk[0], int16_t(-27010));
    CHECK_EQ(blk[65535], int16_t(-20423));
}

// ===========================================================================
// REAL zstd. Everything below needs an actual libzstd and is compiled ONLY
// under -DVXC_WITH_ZSTD=ON (voxel-core/CMakeLists.txt), which CI never sets.
//
// Why the opt-in and not just "link zstd in the tests": voxel-core building,
// linking and passing with no compression library present is the property the
// injected-decompressor design exists to deliver (tilestore.h, and §3 of
// docs/vxtl-v2-format.md). A test suite that needed zstd to go green would
// have quietly repealed it. So the default build stays zstd-free and this is
// the developer's switch for the one thing that build genuinely cannot do.
//
// Note the library is linked into vxc_tests and NEVER into voxelcore: in this
// test's world vxc_tests plays the part the UE module plays in the shipped
// one — the HOST, which picks a zstd and hands voxel-core a callback over it.
// ===========================================================================

#ifdef VXC_WITH_ZSTD
#include <cstdlib>

#include <zstd.h>

namespace {

// The injected FineDecompressor, over a real libzstd. This is the whole of
// what a host has to write, and it is deliberately the same shape as the UE
// module's: one stateless call, no allocation, and the exact-length rule
// enforced here rather than trusted.
struct RealZstdState {
    uint64_t calls = 0;
    uint64_t rejects = 0;
    size_t lastSrcLen = 0;
    size_t lastDstLen = 0;
};

bool realZstdInflate(void* user, const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    RealZstdState* st = static_cast<RealZstdState*>(user);
    if (st) {
        ++st->calls;
        st->lastSrcLen = srcLen;
        st->lastDstLen = dstLen;
    }
    // ZSTD_decompress refuses to write past dstLen, so the only failure left to
    // catch by hand is a frame that expands to FEWER bytes than the header
    // implies — which is the case comp_len can no longer catch under
    // CODEC_ZSTD, and the one that turns a corrupt frame into plausible
    // terrain if it gets through.
    const size_t got = ZSTD_decompress(dst, dstLen, src, srcLen);
    if (ZSTD_isError(got) || got != dstLen) {
        if (st) ++st->rejects;
        return false;
    }
    return true;
}

FineDecompressor realDecompressor(RealZstdState& st) {
    FineDecompressor d;
    d.fn = &realZstdInflate;
    d.user = &st;
    return d;
}

// One block payload -> one real zstd frame. Level 19 to match every size
// number on record (terrain-service/tools/bake_real_tile.py measures at 19).
std::vector<uint8_t> realZstdFrame(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out(ZSTD_compressBound(payload.size()));
    const size_t n = ZSTD_compress(out.data(), out.size(), payload.data(), payload.size(), 19);
    if (ZSTD_isError(n)) return {};
    out.resize(n);
    return out;
}

} // namespace

VXC_TEST(vxtl_v2_zstd_real_frames_decode_identically_to_codec_raw) {
    // THE test this opt-in exists for: a lattice encoded twice, once CODEC_RAW
    // and once CODEC_ZSTD with frames a real libzstd actually entropy-coded,
    // must decode to identical control points. Same numbers, different bytes.
    //
    // Nothing weaker substitutes. comp_len is the compressed length under
    // CODEC_ZSTD and constrains nothing; the decompressed length cannot tell a
    // literal int16 plane from a resid_bits=16 residual plane (§4); and the
    // codec byte is a claim, not a check. Only a whole-lattice digest against
    // the CODEC_RAW encoding can fail when a real frame decodes to the wrong
    // numbers.
    V2Params rawP;
    rawP.seed = 20260729;
    rawP.x = -3;
    rawP.y = 5;
    rawP.baseOffsetMm = 123'400;
    V2Params zstdP = rawP;
    zstdP.codec = 1;

    const std::vector<uint8_t> rawBytes = buildFineTile(rawP, makeMixedElevPlans());
    const std::vector<uint8_t> zstdBytes =
        buildFineTile(zstdP, makeMixedElevPlans(), {}, &realZstdFrame);

    // Unlike the Raw_Block twin (which is BIGGER), this one is smaller: the
    // frames carry compressed data, which is the difference being tested.
    CHECK(zstdBytes.size() < rawBytes.size());

    RealZstdState st;
    FineError rawErr = FineError::kBadHeader, zstdErr = FineError::kBadHeader;
    auto rawTile = FineTile::parse(rawBytes.data(), rawBytes.size(), {}, &rawErr);
    auto zstdTile =
        FineTile::parse(zstdBytes.data(), zstdBytes.size(), realDecompressor(st), &zstdErr);
    CHECK(rawTile.has_value());
    CHECK(zstdTile.has_value());
    CHECK(rawErr == FineError::kNone);
    CHECK(zstdErr == FineError::kNone);
    if (!rawTile || !zstdTile) return;
    CHECK_EQ(int(zstdTile->codec()), int(kCodecZstd));

    // The frames are beyond a Raw_Block reader, which is what makes this
    // different from every other CODEC_ZSTD test in this file. Blocks 1 and 2
    // are the structured ones zstd compresses; block 3's control points are
    // pseudo-random, so zstd stores them and a Raw_Block reader can still read
    // that frame. Asserting both keeps the claim honest.
    {
        TestDecompressorState store;
        auto storeTile =
            FineTile::parse(zstdBytes.data(), zstdBytes.size(), testDecompressor(store));
        CHECK(storeTile.has_value());
        if (storeTile) {
            std::vector<int16_t> blk;
            FineError e = FineError::kNone;
            CHECK(!storeTile->decodeElevBlock(1, 0, blk, &e));
            CHECK(e == FineError::kDecompressFailed);
            CHECK(!storeTile->decodeElevBlock(2, 0, blk, &e));
            CHECK(e == FineError::kDecompressFailed);
        }
    }

    Digest dRaw, dZstd;
    uint64_t mismatches = 0;
    for (uint32_t by = 0; by < kTestPerAxis; ++by) {
        for (uint32_t bx = 0; bx < kTestPerAxis; ++bx) {
            std::vector<int16_t> a, b;
            if (!rawTile->decodeElevBlock(bx, by, a)) { ++mismatches; continue; }
            if (!zstdTile->decodeElevBlock(bx, by, b)) { ++mismatches; continue; }
            for (uint32_t i = 0; i < kTestBlockPixels; ++i) {
                dRaw.u16(static_cast<uint16_t>(a[i]));
                dZstd.u16(static_cast<uint16_t>(b[i]));
                if (a[i] != b[i]) ++mismatches;
            }
            // And against what the PLANS say, so a bug corrupting both codecs
            // identically could not pass.
            if (by == 0 && bx < 4) {
                for (uint32_t ly = 0; ly < kTestDim; ++ly)
                    for (uint32_t lx = 0; lx < kTestDim; ++lx)
                        if (b[ly * kTestDim + lx] != expectedCp(bx, by, lx, ly)) ++mismatches;
            }
        }
    }
    CHECK_EQ(mismatches, uint64_t(0));
    CHECK_EQ(dRaw.h, dZstd.h);

    // Three frames, three calls: CONSTANT blocks own no bytes (§4), so real
    // zstd is never invoked for them either.
    CHECK_EQ(st.calls, uint64_t(3));
    CHECK_EQ(st.rejects, uint64_t(0));
    CHECK_EQ(st.lastDstLen, size_t(kTestBlockPixels) * 2);
}

VXC_TEST(vxtl_v2_zstd_real_frames_reject_corruption) {
    // The exactness rule, enforced by a real zstd rather than by this file's
    // 30-line reader. Each case below is one that comp_len used to catch under
    // CODEC_RAW and cannot catch any more.
    V2Params p;
    p.codec = 1;
    const std::vector<uint8_t> good = buildFineTile(p, makeMixedElevPlans(), {}, &realZstdFrame);
    RealZstdState st;
    auto ok = FineTile::parse(good.data(), good.size(), realDecompressor(st));
    CHECK(ok.has_value());
    if (!ok) return;
    const FineBlockEntry e1 = ok->elevIndex()[1]; // CODED/16, the first framed block
    const size_t frame1 = kElevDataOff + static_cast<size_t>(e1.offset);
    CHECK_EQ(int(good[frame1 + 0]), 0x28); // still a zstd frame: magic 0xFD2FB528 LE
    CHECK_EQ(int(good[frame1 + 3]), 0xFD);
    CHECK(e1.compLen < uint32_t(kTestBlockPixels) * 2); // and it really compressed

    auto decodeFails = [&](const std::vector<uint8_t>& b, uint32_t bx, FineError want) {
        RealZstdState s;
        auto t = FineTile::parse(b.data(), b.size(), realDecompressor(s));
        CHECK(t.has_value()); // structurally fine: the failure is inside the frame
        if (!t) return;
        std::vector<int16_t> blk;
        FineError err = FineError::kNone;
        CHECK(!t->decodeElevBlock(bx, 0, blk, &err));
        CHECK(err == want);
        CHECK_EQ(s.rejects, uint64_t(1));
    };

    { // Corrupt magic: not a frame at all.
        std::vector<uint8_t> b = good;
        b[frame1] ^= 0xFF;
        decodeFails(b, 1, FineError::kDecompressFailed);
    }
    { // Corrupt the entropy-coded payload itself. There is no content checksum
      // in these frames, so this is caught by the bitstream being invalid or by
      // the output length — real zstd, either way, and not by us.
        std::vector<uint8_t> b = good;
        b[frame1 + 20] ^= 0xFF;
        b[frame1 + 21] ^= 0xFF;
        decodeFails(b, 1, FineError::kDecompressFailed);
    }
    { // Truncated frame, via a shortened comp_len that is still in bounds.
        std::vector<uint8_t> b = good;
        const uint32_t shorter = e1.compLen - 4;
        for (int i = 0; i < 4; ++i)
            b[kElevIndexOff + 1 * kEntryBytes + 8 + i] = uint8_t(shorter >> (8 * i));
        decodeFails(b, 1, FineError::kDecompressFailed);
    }
    { // THE case only the header-derived expectation catches: a perfectly valid
      // frame read with the WRONG expected size. Relabelling block 1 from
      // CODED/16 to CODED/32 doubles the size the header implies while leaving
      // the frame untouched and every bound intact, so nothing structural can
      // see it — and a decoder that trusted the frame's own content size, or
      // accepted a short read, would hand back half a block of garbage.
        std::vector<uint8_t> b = good;
        b[kElevIndexOff + 1 * kEntryBytes + 15] = 32; // resid_bits 16 -> 32
        decodeFails(b, 1, FineError::kDecompressFailed);
    }
}

VXC_TEST(vxtl_v2_zstd_real_golden_fixture_cross_language_digest) {
    // §9 item 2, end to end and for real: a fixture PYTHON compressed with
    // libzstd at level 19, decoded here by C++ over libzstd, digest-compared
    // against the whole-lattice digest the CODEC_RAW golden pinned before any
    // codec existed. Two languages, two encoders, one number.
    const std::filesystem::path path = fineZstdRealFixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  SKIP vxtl_v2_zstd_real_golden_fixture_cross_language_digest: no %s\n",
                    path.filename().string().c_str());
        return;
    }
    auto bytes = readFileBytes(path);
    CHECK(bytes.has_value());
    if (!bytes) return;

    RealZstdState st;
    FineError err = FineError::kBadHeader;
    auto tile = FineTile::parse(bytes->data(), bytes->size(), realDecompressor(st), &err);
    CHECK(tile.has_value());
    CHECK(err == FineError::kNone);
    if (!tile) return;
    CHECK_EQ(tile->seed(), uint64_t(20260729));
    CHECK_EQ(tile->tileX(), 100);
    CHECK_EQ(tile->tileY(), -42);
    CHECK_EQ(tile->size(), uint32_t(512));
    CHECK_EQ(int(tile->header().codec), int(kCodecZstd));
    CHECK_EQ(tile->baseOffsetMm(), 500'000);

    Digest d;
    std::vector<int16_t> blk;
    for (uint32_t by = 0; by < 2; ++by) {
        for (uint32_t bx = 0; bx < 2; ++bx) {
            CHECK(tile->decodeElevBlock(bx, by, blk));
            for (int16_t cp : blk) d.u16(static_cast<uint16_t>(cp));
        }
    }
    CHECK_EQ(d.h, kGolden512Digest);
    CHECK_EQ(st.calls, uint64_t(3));
    CHECK_EQ(st.rejects, uint64_t(0));

    // Value for value against the CODEC_RAW golden itself, which is the
    // strongest form of "the codec changed the bytes and nothing else".
    auto rawBytes = readFileBytes(fineFixturePath());
    if (rawBytes) {
        auto rawTile = FineTile::parse(rawBytes->data(), rawBytes->size());
        CHECK(rawTile.has_value());
        if (rawTile) {
            uint64_t mismatches = 0;
            std::vector<int16_t> a, b;
            for (uint32_t by = 0; by < 2; ++by) {
                for (uint32_t bx = 0; bx < 2; ++bx) {
                    if (!rawTile->decodeElevBlock(bx, by, a)) { ++mismatches; continue; }
                    if (!tile->decodeElevBlock(bx, by, b)) { ++mismatches; continue; }
                    if (a.size() != b.size()) { ++mismatches; continue; }
                    for (size_t k = 0; k < a.size(); ++k)
                        if (a[k] != b[k]) ++mismatches;
                }
            }
            CHECK_EQ(mismatches, uint64_t(0));
        }
    }

    // And the loop closed the other way: the RAW_BLOCK fixture CI decodes with
    // this repo's hand-rolled reader must also decode under a real zstd, to the
    // same digest. That is what stops "conformant zstd frame" from meaning
    // "conformant to our own reader" — the claim vxtl_zstd_store.py makes and
    // that nothing in C++ had ever checked.
    const std::filesystem::path storePath = fineZstdFixturePath();
    if (std::filesystem::exists(storePath)) {
        auto storeBytes = readFileBytes(storePath);
        CHECK(storeBytes.has_value());
        if (storeBytes) {
            RealZstdState st2;
            auto storeTile =
                FineTile::parse(storeBytes->data(), storeBytes->size(), realDecompressor(st2));
            CHECK(storeTile.has_value());
            if (storeTile) {
                Digest d2;
                for (uint32_t by = 0; by < 2; ++by) {
                    for (uint32_t bx = 0; bx < 2; ++bx) {
                        CHECK(storeTile->decodeElevBlock(bx, by, blk));
                        for (int16_t cp : blk) d2.u16(static_cast<uint16_t>(cp));
                    }
                }
                CHECK_EQ(d2.h, kGolden512Digest);
                CHECK_EQ(st2.rejects, uint64_t(0));
            }
        }
    }
}

VXC_TEST(vxtl_v2_zstd_real_production_tile_equivalence) {
    // The same equivalence at PRODUCTION SCALE, on a baked tile, because the
    // committed fixtures prove the format and not the size: a 512-edge tile is
    // 4 blocks and one of them is CONSTANT, while a real 8192^2 tile is 1024
    // blocks per plane, ~134 MB of lattice, and residual statistics an
    // entropy coder actually has to work at.
    //
    // Not committed — a real tile is ~20 MB compressed and ~140 MB raw — so it
    // is driven by two paths in the environment, and skips when they are
    // absent. Produce the pair with:
    // (written without shell line-continuations: a "//" comment whose last
    // character is a backslash is a multi-line comment to g++, and CI builds
    // with -Werror=comment)
    //   python terrain-service/tools/bake_real_tile.py --tiles-dir ...
    //     --tile -5 2 --out-dir DIR
    //   python terrain-service/tools/vxtl_recodec.py DIR/-5_2.vxtl
    //     --out DIR/-5_2.zstd.vxtl
    // then set VXC_FINE_TILE_RAW and VXC_FINE_TILE_ZSTD to the two files.
    // The recodec is a container operation, so the two files hold the same
    // payload bytes by construction and any difference here is this decoder's.
    const char* rawEnv = std::getenv("VXC_FINE_TILE_RAW");
    const char* zstdEnv = std::getenv("VXC_FINE_TILE_ZSTD");
    if (!rawEnv || !zstdEnv || !*rawEnv || !*zstdEnv) {
        std::printf("  SKIP vxtl_v2_zstd_real_production_tile_equivalence: set "
                    "VXC_FINE_TILE_RAW and VXC_FINE_TILE_ZSTD to a baked RAW/ZSTD pair\n");
        return;
    }
    const std::filesystem::path rawPath(rawEnv), zstdPath(zstdEnv);
    if (!std::filesystem::exists(rawPath) || !std::filesystem::exists(zstdPath)) {
        std::printf("  SKIP vxtl_v2_zstd_real_production_tile_equivalence: %s or %s missing\n",
                    rawEnv, zstdEnv);
        return;
    }

    auto rawBytes = readFileBytes(rawPath);
    auto zstdBytes = readFileBytes(zstdPath);
    CHECK(rawBytes.has_value());
    CHECK(zstdBytes.has_value());
    if (!rawBytes || !zstdBytes) return;
    std::printf("  real tile: CODEC_RAW %.2f MB, CODEC_ZSTD %.2f MB (%.2fx)\n",
                double(rawBytes->size()) / 1e6, double(zstdBytes->size()) / 1e6,
                double(rawBytes->size()) / double(zstdBytes->size()));

    FineError rawErr = FineError::kBadHeader, zErr = FineError::kBadHeader;
    RealZstdState st;
    auto rawTile = FineTile::parse(*rawBytes, {}, &rawErr);
    auto zTile = FineTile::parse(*zstdBytes, realDecompressor(st), &zErr);
    CHECK(rawTile.has_value());
    CHECK(zTile.has_value());
    CHECK(rawErr == FineError::kNone);
    CHECK(zErr == FineError::kNone);
    if (!rawTile || !zTile) return;

    // Everything the header says must survive a recodec except `codec` itself.
    CHECK_EQ(int(rawTile->codec()), int(kCodecRaw));
    CHECK_EQ(int(zTile->codec()), int(kCodecZstd));
    CHECK_EQ(rawTile->seed(), zTile->seed());
    CHECK_EQ(rawTile->tileX(), zTile->tileX());
    CHECK_EQ(rawTile->tileY(), zTile->tileY());
    CHECK_EQ(rawTile->size(), zTile->size());
    CHECK_EQ(rawTile->blockDim(), zTile->blockDim());
    CHECK_EQ(rawTile->quantMm(), zTile->quantMm());
    CHECK_EQ(rawTile->baseOffsetMm(), zTile->baseOffsetMm());
    CHECK_EQ(int(rawTile->hasFlow()), int(zTile->hasFlow()));
    CHECK_EQ(rawTile->elevIndex().size(), zTile->elevIndex().size());

    const uint32_t perAxis = rawTile->blocksPerAxis();
    Digest dRaw, dZstd;
    uint64_t mismatches = 0, framed = 0;
    std::vector<int16_t> a, b;
    for (uint32_t by = 0; by < perAxis; ++by) {
        for (uint32_t bx = 0; bx < perAxis; ++bx) {
            const FineBlockEntry& re = rawTile->elevIndex()[by * perAxis + bx];
            const FineBlockEntry& ze = zTile->elevIndex()[by * perAxis + bx];
            if (re.mode != ze.mode || re.residBits != ze.residBits ||
                re.constCp != ze.constCp) {
                ++mismatches;
            }
            if (ze.compLen) ++framed;
            if (!rawTile->decodeElevBlock(bx, by, a)) { ++mismatches; continue; }
            if (!zTile->decodeElevBlock(bx, by, b)) { ++mismatches; continue; }
            if (a.size() != b.size()) { ++mismatches; continue; }
            for (size_t i = 0; i < a.size(); ++i) {
                dRaw.u16(static_cast<uint16_t>(a[i]));
                dZstd.u16(static_cast<uint16_t>(b[i]));
                if (a[i] != b[i]) ++mismatches;
            }
        }
    }
    if (rawTile->hasFlow()) {
        std::vector<uint8_t> fa, fb;
        for (uint32_t by = 0; by < perAxis; ++by) {
            for (uint32_t bx = 0; bx < perAxis; ++bx) {
                if (!rawTile->decodeFlowBlock(bx, by, fa)) { ++mismatches; continue; }
                if (!zTile->decodeFlowBlock(bx, by, fb)) { ++mismatches; continue; }
                if (fa.size() != fb.size()) { ++mismatches; continue; }
                for (size_t i = 0; i < fa.size(); ++i) {
                    dRaw.u8(fa[i]);
                    dZstd.u8(fb[i]);
                    if (fa[i] != fb[i]) ++mismatches;
                }
            }
        }
    }
    CHECK_EQ(mismatches, uint64_t(0));
    CHECK_EQ(dRaw.h, dZstd.h);
    CHECK_EQ(st.rejects, uint64_t(0));
    CHECK(framed > 0); // a tile of nothing but CONSTANT blocks would prove nothing
    std::printf("  real tile: %ux%u blocks, %llu framed, whole-tile digest %016llx\n",
                perAxis, perAxis, static_cast<unsigned long long>(framed),
                static_cast<unsigned long long>(dZstd.h));
}

#else // !VXC_WITH_ZSTD

VXC_TEST(vxtl_v2_zstd_real_frames_decode_identically_to_codec_raw) {
    // Not a silent pass: the default build genuinely cannot run this, and it
    // must say so rather than look green. Configure with
    //   cmake -S voxel-core -B build-zstd -DVXC_WITH_ZSTD=ON
    // to link a real libzstd into vxc_tests (and only into vxc_tests).
    std::printf("  SKIP vxtl_v2_zstd_real_frames_decode_identically_to_codec_raw: "
                "built without a real zstd (configure -DVXC_WITH_ZSTD=ON)\n");
}

#endif // VXC_WITH_ZSTD


// ---------------------------------------------------------------------------
// SECTION_BASIN_TABLE (watershed plan P1). Cross-language: the table below was
// written by terrain_service/tile_codec.py against the same document, with no
// code shared with this decoder.
// ---------------------------------------------------------------------------

VXC_TEST(vxtl_v2_basin_table_matches_the_python_encoder) {
    const std::filesystem::path path = fineBasinFixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  (skipped: %s absent)\n", path.string().c_str());
        return;
    }
    std::optional<std::vector<uint8_t>> bytes = readFileBytes(path);
    CHECK(bytes.has_value());
    FineError err = FineError::kNone;
    std::optional<FineTile> tile = FineTile::parse(*bytes, {}, &err);
    CHECK(tile.has_value());
    CHECK_EQ(int(err), int(FineError::kNone));
    CHECK(tile->hasBasins());
    CHECK(tile->hasFlow()); // the table must coexist with another optional section
    CHECK_EQ(int(tile->basins().size()), 5);

    // Every kind appears. A decoder that reads `kind` as a bool passes an
    // all-lake table and fails here.
    uint32_t kindMask = 0;
    for (const BasinEntry& b : tile->basins()) kindMask |= (1u << b.kind);
    CHECK_EQ(int(kindMask), int((1u << kBasinKindCount) - 1u));

    // id 0: overflowing, surface EXACTLY at the spill.
    const BasinEntry& b0 = tile->basins()[0];
    CHECK_EQ(int(b0.basinId), 0);
    CHECK_EQ(int(b0.kind), int(kBasinLakeOverflowing));
    CHECK(b0.holdsWater());
    CHECK_EQ(int(b0.spillMm), 1234500);
    CHECK_EQ(int(b0.surfaceMm), 1234500);
    CHECK_EQ(int(b0.seedX), 40);
    CHECK_EQ(int(b0.seedY), 12);
    CHECK_EQ(int(b0.bboxX0), 30);
    CHECK_EQ(int(b0.bboxY1), 40);
    CHECK_EQ(int(b0.outletX), 29);

    // id 1: terminal, standing below its own outlet.
    CHECK_EQ(int(tile->basins()[1].kind), int(kBasinLakeTerminal));
    CHECK(tile->basins()[1].surfaceMm < tile->basins()[1].spillMm);
    CHECK(tile->basins()[1].holdsWater());

    // id 2: NEGATIVE elevations, and a one-pixel extent. The bake's registry
    // refuses a basin at or below sea level, so this row exists only to
    // exercise the i32 sign -- which would otherwise stay untested until the
    // first below-sea world.
    const BasinEntry& b2 = tile->basins()[2];
    CHECK_EQ(int(b2.spillMm), -2500);
    CHECK_EQ(int(b2.surfaceMm), -7300);
    CHECK_EQ(int(b2.bboxX0), int(b2.bboxX1));
    CHECK_EQ(int(b2.bboxY0), int(b2.bboxY1));

    // id 3 and 4 are dry: kind alone says so, and holdsWater() must agree.
    CHECK(!tile->basins()[3].holdsWater());
    CHECK(!tile->basins()[4].holdsWater());

    // id 4 reaches the LAST pixel on both axes -- an off-by-one in the bounds
    // check refuses the whole tile rather than clipping quietly.
    CHECK_EQ(int(tile->basins()[4].bboxX1), int(tile->size()) - 1);
    CHECK_EQ(int(tile->basins()[4].bboxY1), int(tile->size()) - 1);

    // Ids are 0..n-1 in order, which is what makes indexing by id meaningful.
    for (size_t i = 0; i < tile->basins().size(); ++i) {
        CHECK_EQ(int(tile->basins()[i].basinId), int(i));
    }
}

VXC_TEST(vxtl_v2_basin_table_is_refused_when_it_is_wrong) {
    const std::filesystem::path path = fineBasinFixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  (skipped: %s absent)\n", path.string().c_str());
        return;
    }
    std::optional<std::vector<uint8_t>> good = readFileBytes(path);
    CHECK(good.has_value());

    // Find the basin section's file offset from the section table, rather
    // than hardcoding it: the fixture's layout may change and a test that
    // corrupts the wrong bytes would pass for the wrong reason.
    uint16_t nSections = 0;
    std::memcpy(&nSections, good->data() + kFineHeaderBytes - 2, 2);
    uint64_t basinOff = 0, basinLen = 0;
    for (uint16_t i = 0; i < nSections; ++i) {
        const size_t e = kFineHeaderBytes + static_cast<size_t>(i) * kFineSectionEntryBytes;
        uint32_t id = 0;
        uint64_t off = 0, len = 0;
        std::memcpy(&id, good->data() + e, 4);
        std::memcpy(&off, good->data() + e + 4, 8);
        std::memcpy(&len, good->data() + e + 12, 8);
        if (id == kSectionBasinTable) { basinOff = off; basinLen = len; }
    }
    CHECK(basinLen == kBasinTableHeaderBytes + 5 * kBasinEntryBytes);

    auto refused = [&](const std::vector<uint8_t>& bad, FineError want) {
        FineError err = FineError::kNone;
        std::optional<FineTile> t = FineTile::parse(bad, {}, &err);
        CHECK(!t.has_value());
        CHECK_EQ(int(err), int(want));
    };

    // A row size this build does not know: refuse, never read 33-byte records
    // out of a 32-byte stream.
    {
        std::vector<uint8_t> bad = *good;
        bad[static_cast<size_t>(basinOff) + 2] = 33;
        refused(bad, FineError::kBadBasinTable);
    }
    // A count that disagrees with the section length.
    {
        std::vector<uint8_t> bad = *good;
        bad[static_cast<size_t>(basinOff) + 4] = 6;
        refused(bad, FineError::kBadBasinTable);
    }
    // A kind outside the enum.
    {
        std::vector<uint8_t> bad = *good;
        bad[static_cast<size_t>(basinOff) + kBasinTableHeaderBytes + 26] = 9;
        refused(bad, FineError::kBadBasinTable);
    }
    // Water standing ABOVE its own outlet -- the one field error that would
    // otherwise flood terrain rather than fail.
    {
        std::vector<uint8_t> bad = *good;
        const size_t surfAt = static_cast<size_t>(basinOff) + kBasinTableHeaderBytes + 22;
        const int32_t tooHigh = 2000000000;
        std::memcpy(bad.data() + surfAt, &tooHigh, 4);
        refused(bad, FineError::kBadBasinTable);
    }
    // Nonzero reserved bytes: this decoder does not know what they mean.
    {
        std::vector<uint8_t> bad = *good;
        bad[static_cast<size_t>(basinOff) + kBasinTableHeaderBytes + 27] = 1;
        refused(bad, FineError::kBadBasinTable);
    }
    // The flag without the section: refused, in both directions. flags sits at
    // byte 31 -- 25 v1-positional header bytes, then block_log2/predictor/
    // quant/codec and bake_ver.
    {
        std::vector<uint8_t> bad = *good;
        uint16_t flags = 0;
        std::memcpy(&flags, bad.data() + 31, 2);
        flags &= static_cast<uint16_t>(~kFineFlagBasinsPresent);
        std::memcpy(bad.data() + 31, &flags, 2);
        refused(bad, FineError::kBadSectionTable);
    }
    // An UNKNOWN flag bit is still refused whole -- the property that lets the
    // two halves of this format move in lockstep instead of drifting -- but as
    // kUnknownFeature, not kBadHeader: an undefined flag is what a tile NEWER
    // than its reader looks like, and it must not be reported as damage.
    {
        std::vector<uint8_t> bad = *good;
        uint16_t flags = 0;
        std::memcpy(&flags, bad.data() + 31, 2);
        flags |= 0x8000;
        std::memcpy(bad.data() + 31, &flags, 2);
        refused(bad, FineError::kUnknownFeature);
    }
}


// ---------------------------------------------------------------------------
// BASIN TABLE v2 + SECTION_HEADWATERS (water re-architecture Phase 1,
// bake_ver 24). Cross-language, same provenance and the same rule: the file
// below was written by terrain_service/tile_codec.py with no code shared with
// this decoder.
// ---------------------------------------------------------------------------

namespace {

// Finds one section's (offset, length) by walking the file's own section
// table, rather than hardcoding it: the fixture's layout may change and a test
// that corrupted the wrong bytes would pass for the wrong reason.
bool findSection(const std::vector<uint8_t>& bytes, uint32_t wantId, uint64_t& off,
                 uint64_t& len) {
    uint16_t nSections = 0;
    std::memcpy(&nSections, bytes.data() + kFineHeaderBytes - 2, 2);
    for (uint16_t i = 0; i < nSections; ++i) {
        const size_t e = kFineHeaderBytes + static_cast<size_t>(i) * kFineSectionEntryBytes;
        uint32_t id = 0;
        uint64_t o = 0, l = 0;
        std::memcpy(&id, bytes.data() + e, 4);
        std::memcpy(&o, bytes.data() + e + 4, 8);
        std::memcpy(&l, bytes.data() + e + 12, 8);
        if (id == wantId) {
            off = o;
            len = l;
            return true;
        }
    }
    return false;
}

} // namespace

VXC_TEST(vxtl_v2_basin_table_v2_matches_the_python_encoder) {
    const std::filesystem::path path = fineBasinV2FixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  (skipped: %s absent)\n", path.string().c_str());
        return;
    }
    std::optional<std::vector<uint8_t>> bytes = readFileBytes(path);
    CHECK(bytes.has_value());
    FineError err = FineError::kNone;
    std::optional<FineTile> tile = FineTile::parse(*bytes, {}, &err);
    CHECK(tile.has_value());
    CHECK_EQ(int(err), int(FineError::kNone));
    CHECK(tile->hasBasins());
    CHECK(tile->hasHeads());
    CHECK(tile->basinsResident());
    CHECK(tile->headsResident());
    CHECK_EQ(int(tile->basins().size()), 5);
    CHECK_EQ(int(tile->header().bakeVer), 24);

    // Where this tile's pixel (0,0) is in the world -- the transform every
    // absolute field below is checked against, and the one a client needs to
    // union rows across a seam.
    const int64_t wox = static_cast<int64_t>(tile->tileX()) * tile->size();
    const int64_t woy = static_cast<int64_t>(tile->tileY()) * tile->size();

    int spanning = 0, floorOutsideTile = 0;
    uint64_t maxCapacity = 0;
    bool sawZeroCapacity = false;
    for (const BasinEntry& b : tile->basins()) {
        CHECK(b.hasV2());
        CHECK_EQ(int(b.tableVersion), int(kBasinTableVersionV2));
        // The clipped view is the world view seen through this tile.
        CHECK(wox + b.bboxX0 >= b.worldX0);
        CHECK(wox + b.bboxX1 <= b.worldX1);
        CHECK(woy + b.bboxY0 >= b.worldY0);
        CHECK(woy + b.bboxY1 <= b.worldY1);
        // The identity anchor is a cell of this component, and it reads back
        // as a place: a packing, not a hash.
        CHECK(b.globalIdWorldX() >= b.worldX0 && b.globalIdWorldX() <= b.worldX1);
        CHECK(b.globalIdWorldY() >= b.worldY0 && b.globalIdWorldY() <= b.worldY1);
        // NEGATIVE world coordinates: this tile is (-2,-4), like the wet
        // alpine block, and the anchor packs through two's complement. A
        // decoder that read the halves as unsigned lands 4 billion px away.
        CHECK(b.globalIdWorldX() < 0);
        CHECK(b.globalIdWorldY() < 0);
        // basinledger.h's BasinId contract, on the bytes the runtime is
        // actually handed: bit 63 is its "tile-local v1 key" tag and 0 is its
        // "not a basin". An id breaking either would be dropped by the ledger
        // with no error anywhere -- no lake, no message. This is why the
        // packing biases 31-bit fields instead of using u32 halves.
        CHECK(b.globalId != 0);
        CHECK_EQ(int((b.globalId >> 63) & 1u), 0);
        CHECK(b.floorMm <= b.surfaceMm);
        CHECK(b.surfaceMm <= b.spillMm);
        if (b.crossesTile()) ++spanning;
        if (b.globalIdWorldX() < wox || b.globalIdWorldX() >= wox + tile->size()) {
            ++floorOutsideTile;
        }
        maxCapacity = std::max(maxCapacity, b.capacityLitres);
        if (b.capacityLitres == 0) sawZeroCapacity = true;
    }

    // THE ROW v2 EXISTS FOR: a tile-spanning basin whose DEEPEST CELL is in
    // the neighbour. A decoder that assumed the anchor is a local pixel, or
    // that seedPx is the floor, puts the lake in the wrong place. v1 could not
    // express this row at all, which is why it dropped such basins -- 123 of
    // them / 146 ha over the six wet-alpine tiles.
    CHECK_EQ(spanning, 2); // both directions, left edge and right edge
    CHECK(floorOutsideTile >= 1);

    // Capacity is u64 LITRES. A decoder reading 32 bits truncates the big pan.
    CHECK(maxCapacity > 0xFFFFFFFFull);
    CHECK_EQ(int(maxCapacity == 8000000000000ull), 1);
    // And exactly 0 for the overflowing lake: it is already at its spill, so
    // its headroom is nil. Not "missing".
    CHECK(sawZeroCapacity);
    CHECK_EQ(int(tile->basins()[0].kind), int(kBasinLakeOverflowing));
    CHECK_EQ(int(tile->basins()[0].capacityLitres), 0);

    // The heads: ordered by (y, x), one at u32 max (a decoder reading Q as
    // int32 gets a negative faucet rate) and one at exactly 0 (allowed, and
    // not the same thing as an absent row).
    CHECK_EQ(int(tile->heads().size()), 4);
    int64_t prev = -1;
    for (const HeadEntry& h : tile->heads()) {
        CHECK(h.px < tile->size() && h.py < tile->size());
        const int64_t key = (static_cast<int64_t>(h.py) << 17) | h.px;
        CHECK(key > prev);
        prev = key;
    }
    CHECK_EQ(int(tile->heads()[0].qM3PerYear), 0);
    CHECK_EQ(int(tile->heads()[3].qM3PerYear == 4294967295u), 1);
    // x DESCENDS between rows 0 and 1 while y ascends: a decoder that sorted
    // by (x, y) would have refused this file.
    CHECK(tile->heads()[1].px < tile->heads()[0].px);
    CHECK(tile->heads()[1].py > tile->heads()[0].py);
}

VXC_TEST(vxtl_v1_basin_rows_still_decode_and_report_no_identity) {
    // THE COMPATIBILITY CLAIM. Tiles written before bake_ver 24 are in shipped
    // namespaces; refusing them would strand every world baked so far. And a
    // v1 row's MISSING identity must read as missing -- if hasV2() lied, every
    // v1 basin would claim to be the lake whose floor is world pixel (0,0) and
    // a union by id would merge the entire world into one.
    const std::filesystem::path path = fineBasinFixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  (skipped: %s absent)\n", path.string().c_str());
        return;
    }
    std::optional<std::vector<uint8_t>> bytes = readFileBytes(path);
    CHECK(bytes.has_value());
    FineError err = FineError::kNone;
    std::optional<FineTile> tile = FineTile::parse(*bytes, {}, &err);
    CHECK(tile.has_value());
    CHECK_EQ(int(tile->basins().size()), 5);
    CHECK(!tile->hasHeads());
    CHECK(tile->heads().empty());
    for (const BasinEntry& b : tile->basins()) {
        CHECK(!b.hasV2());
        CHECK_EQ(int(b.tableVersion), int(kBasinTableVersionV1));
        CHECK_EQ(int(b.globalId == 0), 1);
        CHECK_EQ(int(b.capacityLitres == 0), 1);
        CHECK(!b.crossesTile());
    }
}

VXC_TEST(vxtl_v2_basin_table_v2_is_refused_when_it_is_wrong) {
    const std::filesystem::path path = fineBasinV2FixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  (skipped: %s absent)\n", path.string().c_str());
        return;
    }
    std::optional<std::vector<uint8_t>> good = readFileBytes(path);
    CHECK(good.has_value());
    uint64_t basinOff = 0, basinLen = 0;
    CHECK(findSection(*good, kSectionBasinTable, basinOff, basinLen));
    CHECK(basinLen == kBasinTableHeaderBytes + 5 * kBasinEntryBytesV2);

    auto refused = [&](const std::vector<uint8_t>& bad, FineError want) {
        FineError err = FineError::kNone;
        std::optional<FineTile> t = FineTile::parse(bad, {}, &err);
        CHECK(!t.has_value());
        CHECK_EQ(int(err), int(want));
    };
    // Byte offsets inside row 0: the v1 head is 32 bytes, then the v2 tail.
    const size_t row0 = static_cast<size_t>(basinOff) + kBasinTableHeaderBytes;
    const size_t tail0 = row0 + kBasinEntryBytes;

    // THE VERSION AND THE ROW SIZE ARE CHECKED AS A PAIR. A v1 label over
    // 80-byte rows is not "an old table relabelled" -- it is bytes neither
    // revision wrote, and reading it either way is a guess.
    {
        std::vector<uint8_t> bad = *good;
        bad[static_cast<size_t>(basinOff)] = 1; // version 1, rows still 80 B
        refused(bad, FineError::kBadBasinTable);
    }
    {
        std::vector<uint8_t> bad = *good;
        bad[static_cast<size_t>(basinOff) + 2] = 32; // entry_bytes 32 at v2
        refused(bad, FineError::kBadBasinTable);
    }
    {
        std::vector<uint8_t> bad = *good;
        bad[static_cast<size_t>(basinOff)] = 3; // a version nobody wrote
        refused(bad, FineError::kBadBasinTable);
    }
    // The identity anchor outside the basin's own extent: the id and the
    // extent would then describe two different basins, and the client's union
    // rule reads BOTH, so it would merge the wrong pair.
    {
        std::vector<uint8_t> bad = *good;
        uint64_t gid = 0;
        std::memcpy(&gid, bad.data() + tail0, 8);
        gid += (uint64_t(1) << 31) * 4000; // move the anchor 4000 px east
        std::memcpy(bad.data() + tail0, &gid, 8);
        refused(bad, FineError::kBadBasinTable);
    }
    // An id with the runtime's tile-local tag bit set, or a zero id: both are
    // values basinledger.h's BasinId refuses, so a tile carrying one would
    // lose that lake at the ledger with no error raised anywhere.
    {
        std::vector<uint8_t> bad = *good;
        uint64_t gid = 0;
        std::memcpy(&gid, bad.data() + tail0, 8);
        gid |= uint64_t(1) << 63;
        std::memcpy(bad.data() + tail0, &gid, 8);
        refused(bad, FineError::kBadBasinTable);
    }
    {
        std::vector<uint8_t> bad = *good;
        const uint64_t zero = 0;
        std::memcpy(bad.data() + tail0, &zero, 8);
        refused(bad, FineError::kBadBasinTable);
    }
    // A world bbox that no longer contains this tile's own clipped bbox --
    // the check that catches a row built with the wrong tile origin, which
    // would put a lake a whole tile away from where the client floods it.
    {
        std::vector<uint8_t> bad = *good;
        int32_t wx0 = 0;
        std::memcpy(&wx0, bad.data() + tail0 + 20, 4);
        const int32_t moved = wx0 + 4000;
        std::memcpy(bad.data() + tail0 + 20, &moved, 4);
        refused(bad, FineError::kBadBasinTable);
    }
    // An inside-out world bbox.
    {
        std::vector<uint8_t> bad = *good;
        int32_t wx0 = 0;
        std::memcpy(&wx0, bad.data() + tail0 + 20, 4);
        const int32_t past = wx0 - 1;
        std::memcpy(bad.data() + tail0 + 28, &past, 4); // worldX1 < worldX0
        refused(bad, FineError::kBadBasinTable);
    }
    // A floor above its own surface: the two came off different components,
    // and a client would compute a negative standing depth from the row.
    {
        std::vector<uint8_t> bad = *good;
        const int32_t tooHigh = 2000000000;
        std::memcpy(bad.data() + tail0 + 16, &tooHigh, 4); // floorMm
        refused(bad, FineError::kBadBasinTable);
    }
    // An unknown span-flag bit: same rule as the header flags -- bytes this
    // build does not implement are refused, not guessed at.
    {
        std::vector<uint8_t> bad = *good;
        bad[tail0 + 44] = 0x02;
        refused(bad, FineError::kBadBasinTable);
    }
    // Nonzero reserved bytes in the v2 tail.
    {
        std::vector<uint8_t> bad = *good;
        bad[tail0 + 47] = 1;
        refused(bad, FineError::kBadBasinTable);
    }
}

VXC_TEST(vxtl_v2_headwater_table_is_refused_when_it_is_wrong) {
    const std::filesystem::path path = fineBasinV2FixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  (skipped: %s absent)\n", path.string().c_str());
        return;
    }
    std::optional<std::vector<uint8_t>> good = readFileBytes(path);
    CHECK(good.has_value());
    uint64_t headOff = 0, headLen = 0;
    CHECK(findSection(*good, kSectionHeadwaters, headOff, headLen));
    CHECK(headLen == kHeadwaterTableHeaderBytes + 4 * kHeadwaterEntryBytes);

    auto refused = [&](const std::vector<uint8_t>& bad, FineError want) {
        FineError err = FineError::kNone;
        std::optional<FineTile> t = FineTile::parse(bad, {}, &err);
        CHECK(!t.has_value());
        CHECK_EQ(int(err), int(want));
    };
    const size_t row0 = static_cast<size_t>(headOff) + kHeadwaterTableHeaderBytes;

    // A row size this build does not know.
    {
        std::vector<uint8_t> bad = *good;
        bad[static_cast<size_t>(headOff) + 2] = 12;
        refused(bad, FineError::kBadHeadwaterTable);
    }
    // A version this build does not know.
    {
        std::vector<uint8_t> bad = *good;
        bad[static_cast<size_t>(headOff)] = 7;
        refused(bad, FineError::kBadHeadwaterTable);
    }
    // A count that disagrees with the section length.
    {
        std::vector<uint8_t> bad = *good;
        bad[static_cast<size_t>(headOff) + 4] = 5;
        refused(bad, FineError::kBadHeadwaterTable);
    }
    // A head outside the tile: the client would spawn a faucet off the plane.
    {
        std::vector<uint8_t> bad = *good;
        const uint16_t past = 4000;
        std::memcpy(bad.data() + row0, &past, 2);
        refused(bad, FineError::kBadHeadwaterTable);
    }
    // OUT OF ORDER -- which is how a DUPLICATE point gets in, and a duplicate
    // point is one faucet emitted twice, i.e. twice the water in one place.
    {
        std::vector<uint8_t> bad = *good;
        uint16_t y0 = 0;
        std::memcpy(&y0, bad.data() + row0 + 2, 2);
        const uint16_t later = static_cast<uint16_t>(y0 + 500);
        std::memcpy(bad.data() + row0 + 2, &later, 2);
        refused(bad, FineError::kBadHeadwaterTable);
    }
    // The section without its flag: those bytes would be silently ignored and
    // every faucet in the tile would go missing without a word. flags sit at
    // byte 31 (see the v1 basin refusal test for the layout).
    {
        std::vector<uint8_t> bad = *good;
        uint16_t flags = 0;
        std::memcpy(&flags, bad.data() + 31, 2);
        flags &= static_cast<uint16_t>(~kFineFlagHeadsPresent);
        std::memcpy(bad.data() + 31, &flags, 2);
        refused(bad, FineError::kBadSectionTable);
    }
}

VXC_TEST(the_reader_declares_the_features_it_learned_with_this_bake) {
    // The rule tilestore.h states for kFineMaxKnownBakeVer: bump it in the
    // same commit that teaches the parser a new flag bit or section id, never
    // on its own. bake_ver 24 taught it both and bake_ver 27 taught it both
    // again, so this is those bumps asserted rather than remembered -- and the
    // refusal message quotes this number, so a stale one sends whoever reads
    // the log to check the wrong thing.
    CHECK_EQ(int(kFineMaxKnownBakeVer), 28);
    CHECK_EQ(int(kFineFlagsKnown & kFineFlagHeadsPresent), int(kFineFlagHeadsPresent));
    CHECK_EQ(int(kFineFlagHeadsPresent), 0x8);
    CHECK_EQ(int(kSectionHeadwaters), 8);
    // bake_ver 27: the bathymetry pair. THE MASK IS THE LOAD-BEARING HALF -- a
    // bit missing from kFineFlagsKnown is not a missing feature, it is every
    // v27 tile refused whole as kUnknownFeature.
    CHECK_EQ(int(kFineFlagBathyPresent), 0x10);
    CHECK_EQ(int(kFineFlagsKnown & kFineFlagBathyPresent), int(kFineFlagBathyPresent));
    CHECK_EQ(int(kSectionBathyDepthIndex), 9);
    CHECK_EQ(int(kSectionBathyDepthData), 10);
    CHECK_EQ(int(kSectionBathyShoreIndex), 11);
    CHECK_EQ(int(kSectionBathyShoreData), 12);
    // The wire units, against terrain_service/bake/basins.py's own constants.
    CHECK_EQ(int(kBathyDepthLsbMm), 10);
    CHECK_EQ(int(kBathyShoreLsbMm), 100);
    CHECK_EQ(int(kBathyDryDepth), -1);
    CHECK_EQ(int(kBathyShoreClampUnits), 1000);
    CHECK_EQ(int(kBathyShoreClampMm), 100000);
    CHECK_EQ(int(kBasinEntryBytesV2), 80);
    // The v2 row is the v1 row plus a suffix -- the property that let the
    // layout grow without a second section id.
    CHECK(kBasinEntryBytesV2 > kBasinEntryBytes);
    CHECK(std::string(fineErrorName(FineError::kBadHeadwaterTable)) ==
          "bad-headwater-table");
    // bake_ver 28: the placement channel planes. Same load-bearing mask rule.
    CHECK_EQ(int(kFineFlagPlacementPresent), 0x20);
    CHECK_EQ(int(kFineFlagsKnown & kFineFlagPlacementPresent),
             int(kFineFlagPlacementPresent));
    CHECK_EQ(int(kSectionPlaceDistWaterIndex), 13);
    CHECK_EQ(int(kSectionPlaceDistWaterData), 14);
    CHECK_EQ(int(kSectionPlaceTwiIndex), 15);
    CHECK_EQ(int(kSectionPlaceHeatData), 22);
    // The wire units, against terrain_service/tile_codec.py's own constants.
    CHECK_EQ(int(kPlacementSubsample), 4);
    CHECK_EQ(int(kPlacementDistLsbMm), 2000);
    CHECK_EQ(int(kPlacementDistUnknown), 255);
    CHECK_EQ(placementDistanceMm(0), 0);
    CHECK_EQ(placementDistanceMm(10), 20000);
    CHECK_EQ(placementDistanceMm(kPlacementDistUnknown), INT32_MAX);
    CHECK_EQ(placementTwiMilli(24), 0);        // (24/8 - 3) == 0.0
    CHECK_EQ(placementTwiMilli(104), 10000);   // 10.0
    CHECK_EQ(placementTwiMilli(kPlacementTwiUnknown), INT32_MIN);
    // The derived block rule at both interesting sizes: production keeps the
    // header's blocks, a 512-px fixture collapses to one 128-px block.
    CHECK_EQ(int(finePlacementBlockLog2(8192, 8)), 8);
    CHECK_EQ(int(finePlacementBlockLog2(512, 8)), 7);
}


// ---------------------------------------------------------------------------
// SECTION_WATER_* (watershed plan P2, bake_ver 9). Cross-language, same rule
// as the basin table above: the plane was written by
// terrain_service/tile_codec.py against the same document, sharing no code
// with this decoder.
//
// The water plane's failure mode is SILENT, which is why these are exhaustive
// about the sentinel. A basin table that mis-parses raises; a water plane that
// mis-parses draws a river in the wrong place, or reads the dry sentinel as an
// ordinary control point and floods the tile with water 3.2 km underground.
// Nothing throws.
// ---------------------------------------------------------------------------

namespace {

void checkWaterFixture(const std::filesystem::path& path, const char* what,
                       FineDecompressor dec = {}) {
    if (!std::filesystem::exists(path)) {
        std::printf("  (skipped: %s absent)\n", path.string().c_str());
        return;
    }
    std::optional<std::vector<uint8_t>> bytes = readFileBytes(path);
    CHECK(bytes.has_value());
    FineError err = FineError::kNone;
    std::optional<FineTile> tile = FineTile::parse(*bytes, dec, &err);
    if (!tile.has_value() && err == FineError::kNoDecompressor) {
        std::printf("  (skipped %s: no zstd decompressor in this build)\n", what);
        return;
    }
    CHECK(tile.has_value());
    CHECK_EQ(int(err), int(FineError::kNone));
    CHECK(tile->hasWater());
    CHECK_EQ(int(tile->size()), 512);

    const uint32_t dim = tile->blockDim();
    std::vector<int16_t> blk;

    // 1. THE DRY HALF. Blocks (1,0) and (1,1) are entirely dry and encode as
    //    MODE_CONSTANT at the sentinel. A decoder that zero-fills a fresh
    //    block instead of sentinel-filling it puts water at the datum here.
    for (uint32_t by = 0; by < 2; ++by) {
        CHECK(tile->decodeWaterBlock(1, by, blk));
        CHECK_EQ(int(blk.size()), int(dim * dim));
        size_t wet = 0;
        for (int16_t cp : blk) {
            if (cp != kWaterDryDepth) ++wet;
            CHECK_EQ(int(tile->waterMmFromDepth(cp, 0)), int(kNoWaterMm));
        }
        CHECK_EQ(int(wet), 0);
    }

    // 2. THE WET HALF, and the sentinel exactly at the block seam.
    CHECK(tile->decodeWaterBlock(0, 0, blk));
    CHECK_EQ(int(blk[0 * dim + 255]), int(kWaterDryDepth));   // last px of block 0
    CHECK_EQ(int(tile->waterMmFromDepth(blk[0 * dim + 255], 0)), int(kNoWaterMm));

    // 3. The reach itself: 5 px wide, centred on x=128, and WET.
    size_t wetInRow = 0;
    for (uint32_t lx = 120; lx < 140; ++lx) {
        if (blk[10 * dim + lx] != kWaterDryDepth) ++wetInRow;
    }
    CHECK_EQ(int(wetInRow), 5);

    // 4. A stored DEPTH OF ZERO is one LSB off the -1 sentinel and MUST read
    //    as water, at exactly the bed. That is the boundary an
    //    `if (d <= 0) dry` bug gets wrong, and it is reachable: a reach's
    //    shallow edge quantises there.
    bool sawZeroDepth = false;
    for (uint32_t by = 0; by < 2; ++by) {
        CHECK(tile->decodeWaterBlock(0, by, blk));
        for (int16_t cp : blk) {
            if (cp < 0) continue;
            if (cp == 0) sawZeroDepth = true;
            // Every non-sentinel depth must produce a real elevation.
            CHECK(tile->waterMmFromDepth(cp, 0) != kNoWaterMm);
        }
    }
    CHECK(sawZeroDepth);

    // WHAT THIS TEST DELIBERATELY DOES NOT CHECK: the absolute water surface.
    // Reconstructing it needs the ground SPLINE evaluated on the control
    // lattice, which lives in the amplifier's carrier and not in this decoder,
    // and `waterMmFromDepth` takes that reconstructed ground as a parameter
    // precisely so this layer never has to guess at it. Evaluating the lattice
    // values directly here instead would be the exact confusion the signature
    // is shaped to prevent -- and it was: the first version of this test did
    // that and reported a reach rising by 232 mm.
    //
    // The invariant IS checked, on the Python side where the spline is
    // available, in tools/make_water_fixture.py: net fall 1.03 m over the
    // reach, worst upward step 7 mm against a 10 mm LSB.
}

} // namespace

VXC_TEST(vxtl_v2_water_plane_matches_the_python_encoder) {
    checkWaterFixture(fineWaterFixturePath(), "RAW water fixture");
}

// The ZSTD twin carries REAL zstd frames (the Python encoder used the
// zstandard library), so only the -DVXC_WITH_ZSTD=ON build can read it. The
// default build still gets a check out of it, and a sharp one: the tile must be
// REFUSED with kNoDecompressor rather than silently decoded as literal bytes.
// That refusal is the whole reason `fineCodecNeedsDecompressor` exists, and
// 24 of the 38 resident production tiles take this path.
VXC_TEST(vxtl_v2_water_plane_zstd_matches_the_python_encoder) {
#ifdef VXC_WITH_ZSTD
    RealZstdState st;
    checkWaterFixture(fineWaterZstdFixturePath(), "ZSTD water fixture",
                      realDecompressor(st));
#else
    const std::filesystem::path path = fineWaterZstdFixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  (skipped: %s absent)\n", path.string().c_str());
        return;
    }
    std::optional<std::vector<uint8_t>> bytes = readFileBytes(path);
    CHECK(bytes.has_value());
    FineError err = FineError::kNone;
    std::optional<FineTile> tile = FineTile::parse(*bytes, {}, &err);
    CHECK(!tile.has_value());
    CHECK_EQ(int(err), int(FineError::kNoDecompressor));
#endif
}

VXC_TEST(vxtl_v2_water_flag_and_sections_must_agree_in_both_directions) {
    const std::filesystem::path path = fineWaterFixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  (skipped: %s absent)\n", path.string().c_str());
        return;
    }
    std::optional<std::vector<uint8_t>> good = readFileBytes(path);
    CHECK(good.has_value());

    auto refused = [](const std::vector<uint8_t>& bytes, FineError want) {
        FineError err = FineError::kNone;
        std::optional<FineTile> t = FineTile::parse(bytes, {}, &err);
        CHECK(!t.has_value());
        CHECK_EQ(int(err), int(want));
    };

    // FLAG CLEAR, SECTIONS PRESENT. This is the direction that matters: those
    // bytes would be silently ignored and every river in the tile would vanish
    // with no error raised anywhere.
    {
        std::vector<uint8_t> bad = *good;
        uint16_t flags = 0;
        std::memcpy(&flags, bad.data() + 31, 2);
        flags &= static_cast<uint16_t>(~kFineFlagWaterPresent);
        std::memcpy(bad.data() + 31, &flags, 2);
        refused(bad, FineError::kBadSectionTable);
    }
}


// ---------------------------------------------------------------------------
// SECTION_BATHY_* (water appearance plan, bake_ver 27). Cross-language again,
// and the pair is written by bake/basins.py's `bathymetry_planes` -- the
// producer itself -- so what is pinned below is the real field, not a fixture
// author's idea of one.
//
// THE FAILURE MODE IS SILENT, twice over and in opposite directions:
//
//   * the DEPTH plane's -1 means "not inside any basin extent". Read as an
//     ordinary depth it is -10 mm of water, i.e. every dry cell in the world
//     becomes a lake one centimetre deep -- and the material would shade it.
//   * the SHORE plane has NO sentinel: every value is meaningful, and 0 means
//     "exactly on the waterline", the value at which every shore effect is at
//     full strength. A decoder that zero-fills anything here draws foam over
//     whatever it filled.
//
// Neither throws, which is why these tests read values rather than statuses.
// ---------------------------------------------------------------------------

namespace {

// Everything pinned here was printed by tools/make_bathy_fixture.py's own run
// (its "C++ test pins" block) rather than copied from an expectation.
constexpr uint32_t kBathyWetPixels = 8004;
constexpr int16_t kBathyDeepestUnits = 850;      // 8.50 m
constexpr int16_t kBathyCentreDepthUnits = 849;  // at local pixel (128, 200)
constexpr int16_t kBathyCentreShoreUnits = 919;  // 91.9 m inside the water
// The closest any cell gets to the waterline. One pixel, 1.875 m, 19 units --
// which is the measurement behind tilestore.h's "zero reads as land" note: the
// bake cannot emit a 0 here, so the boundary convention is never exercised in
// production and must not be relied on either way.
constexpr int16_t kBathyNearestShoreUnits = 19;

void checkBathyFixture(const std::filesystem::path& path, const char* what,
                       FineDecompressor dec = {}) {
    if (!std::filesystem::exists(path)) {
        std::printf("  (skipped: %s absent)\n", path.string().c_str());
        return;
    }
    std::optional<std::vector<uint8_t>> bytes = readFileBytes(path);
    CHECK(bytes.has_value());
    FineError err = FineError::kNone;
    std::optional<FineTile> tile = FineTile::parse(*bytes, dec, &err);
    if (!tile.has_value() && err == FineError::kNoDecompressor) {
        std::printf("  (skipped %s: no zstd decompressor in this build)\n", what);
        return;
    }
    CHECK(tile.has_value());
    CHECK_EQ(int(err), int(FineError::kNone));
    CHECK(tile->hasBathy());
    CHECK_EQ(int(tile->header().bakeVer), 27);
    CHECK_EQ(int(tile->size()), 512);
    // A v27 tile reaching this point AT ALL is the flags-mask half of the
    // change: without kFineFlagBathyPresent in kFineFlagsKnown the parse above
    // returns kUnknownFeature and every assertion below is unreachable.
    CHECK(tile->bathyDepthIndexResident());
    CHECK(tile->bathyShoreIndexResident());

    const uint32_t dim = tile->blockDim();
    std::vector<int16_t> depth, shore;

    // 1. THE DRY BLOCKS. (1,0) and (1,1) lie outside every extent and encode
    //    MODE_CONSTANT -- the encoding a production tile is almost entirely
    //    made of. The two planes hold DIFFERENT constants there, which is the
    //    point: a decoder that crossed the two indices would read one of them.
    for (uint32_t by = 0; by < 2; ++by) {
        CHECK(tile->bathyDepthBlockResident(1, by));
        CHECK(tile->decodeBathyDepthBlock(1, by, depth));
        CHECK_EQ(int(depth.size()), int(dim * dim));
        CHECK(tile->decodeBathyShoreBlock(1, by, shore));
        size_t wet = 0;
        for (size_t i = 0; i < depth.size(); ++i) {
            if (!bathyDepthIsDry(depth[i])) ++wet;
            CHECK_EQ(int(depth[i]), int(kBathyDryDepth));
            // The sentinel must NOT become a depth. -10 mm of water over the
            // whole dry world is what a missing sentinel test looks like.
            CHECK_EQ(int(bathyDepthMm(depth[i])), int(kNoBathyDepthMm));
            // Saturated on the LAND side, and land is what it must read as.
            CHECK_EQ(int(shore[i]), -int(kBathyShoreClampUnits));
            CHECK_EQ(int(bathyShoreMm(shore[i])), -int(kBathyShoreClampMm));
            CHECK(!bathyShoreIsWater(shore[i]));
        }
        CHECK_EQ(int(wet), 0);
    }
    // Both were CONSTANT, i.e. the branch above really was the constant path.
    CHECK_EQ(int(tile->bathyDepthIndex()[1].mode), int(kBlockConstant));
    CHECK_EQ(int(tile->bathyShoreIndex()[1].mode), int(kBlockConstant));
    // ...and block (0,0) was not: both planes carry a CODED block too, so this
    // fixture exercises both encodings of both planes. (MODE_RAW is not
    // reachable for these two -- the encoder never forces it on an int16 plane
    // where a MED residual always ties or beats a literal -- and it is covered
    // for the shared block decoder by the elevation and flow fixtures.)
    CHECK_EQ(int(tile->bathyDepthIndex()[0].mode), int(kBlockCoded));
    CHECK_EQ(int(tile->bathyShoreIndex()[0].mode), int(kBlockCoded));

    // 2. THE LAKE, in block (0,0).
    CHECK(tile->decodeBathyDepthBlock(0, 0, depth));
    CHECK(tile->decodeBathyShoreBlock(0, 0, shore));
    const size_t centre = size_t(200) * dim + 128;
    CHECK_EQ(int(depth[centre]), int(kBathyCentreDepthUnits));
    CHECK_EQ(int(bathyDepthMm(depth[centre])), int(kBathyCentreDepthUnits) * 10);
    CHECK_EQ(int(shore[centre]), int(kBathyCentreShoreUnits));
    CHECK_EQ(int(bathyShoreMm(shore[centre])), int(kBathyCentreShoreUnits) * 100);
    CHECK(bathyShoreIsWater(shore[centre]));
    CHECK(!bathyDepthIsDry(depth[centre]));

    // 3. A STORED DEPTH OF EXACTLY 0 -- one LSB off the sentinel, and WET at
    //    exactly the bed. The extent's outermost ring quantises there, so this
    //    is reachable rather than contrived, and `if (d <= 0) dry` gets it
    //    wrong.
    size_t wetPixels = 0, zeroDepth = 0;
    int16_t deepest = 0, nearestShore = INT16_MAX;
    for (size_t i = 0; i < depth.size(); ++i) {
        if (!bathyDepthIsDry(depth[i])) {
            ++wetPixels;
            if (depth[i] == 0) {
                ++zeroDepth;
                CHECK_EQ(int(bathyDepthMm(depth[i])), 0);   // water, at the bed
                CHECK(bathyDepthMm(depth[i]) != kNoBathyDepthMm);
            }
            if (depth[i] > deepest) deepest = depth[i];
        }
        const int16_t mag = static_cast<int16_t>(shore[i] < 0 ? -shore[i] : shore[i]);
        if (mag < nearestShore) nearestShore = mag;
        // 4. THE TWO PLANES AGREE, CELL FOR CELL, on which side of the
        //    shoreline they are describing. Neither plane can make this check
        //    on its own, and getting it wrong is how a lake ends up shaded as
        //    wet ground (or a bank as shallow water).
        CHECK_EQ(int(!bathyDepthIsDry(depth[i])), int(bathyShoreIsWater(shore[i])));
    }
    CHECK_EQ(int(wetPixels), int(kBathyWetPixels));
    CHECK(zeroDepth > 0);
    CHECK_EQ(int(deepest), int(kBathyDeepestUnits));
    // 5. NO CELL SITS ON THE WATERLINE, so the sign is always decisive.
    CHECK_EQ(int(nearestShore), int(kBathyNearestShoreUnits));

    // 6. The convenience accessor, which is the one path a cold caller should
    //    ever use -- and it hands back MILLIMETRES, so nothing downstream ever
    //    multiplies by 10 or 100 itself.
    int32_t depthMm = 0, shoreMm = 0;
    CHECK(tile->bathyAt(128, 200, depthMm, shoreMm));
    CHECK_EQ(int(depthMm), int(kBathyCentreDepthUnits) * 10);
    CHECK_EQ(int(shoreMm), int(kBathyCentreShoreUnits) * 100);
    // A dry pixel in the far corner: no depth, saturated distance, on land.
    CHECK(tile->bathyAt(511, 0, depthMm, shoreMm));
    CHECK_EQ(int(depthMm), int(kNoBathyDepthMm));
    CHECK_EQ(int(shoreMm), -int(kBathyShoreClampMm));
    // Off the plane is refused, not clamped.
    FineError perr = FineError::kNone;
    CHECK(!tile->bathyAt(512, 0, depthMm, shoreMm, &perr));
    CHECK_EQ(int(perr), int(FineError::kBadBlockCoords));
}

} // namespace

VXC_TEST(vxtl_v2_bathy_planes_match_the_python_encoder) {
    checkBathyFixture(fineBathyFixturePath(), "RAW bathy fixture");
}

// The ZSTD twin, same argument as the water plane's: production ships
// CODEC_ZSTD and its block payloads take a different path entirely. The
// default build still gets a sharp check out of it -- the tile must be REFUSED
// with kNoDecompressor rather than silently decoded as literal bytes.
VXC_TEST(vxtl_v2_bathy_planes_zstd_match_the_python_encoder) {
#ifdef VXC_WITH_ZSTD
    RealZstdState st;
    checkBathyFixture(fineBathyZstdFixturePath(), "ZSTD bathy fixture", realDecompressor(st));
#else
    const std::filesystem::path path = fineBathyZstdFixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  (skipped: %s absent)\n", path.string().c_str());
        return;
    }
    std::optional<std::vector<uint8_t>> bytes = readFileBytes(path);
    CHECK(bytes.has_value());
    FineError err = FineError::kNone;
    std::optional<FineTile> tile = FineTile::parse(*bytes, {}, &err);
    CHECK(!tile.has_value());
    CHECK_EQ(int(err), int(FineError::kNoDecompressor));
#endif
}

// ---------------------------------------------------------------------------
// sampleBathyRect -- the BULK read the material transport is built on.
//
// The thing these tests are actually protecting is the difference between "no
// water here" and "no data here". Everything downstream of this function is a
// shader that has to choose between the baked field and a screen-space
// fallback, and it can only choose if the hole is visible in the buffer. So
// every case below checks the SENTINEL and the per-reason counter, not just the
// values that came back.
// ---------------------------------------------------------------------------

namespace {

// The fixture, loaded into a sampler at its own seed and tile coordinate. Every
// rect below is in GLOBAL fine-pixel space, so the tile's own origin has to come
// from the file rather than be assumed to be (0,0).
struct BathySamplerFixture {
    // A unique_ptr and not a value: FineTileSampler holds std::atomic counters,
    // so it is neither copyable nor movable and cannot be returned by value.
    std::unique_ptr<FineTileSampler> tiles;
    int32_t tx = 0, ty = 0;
    int64_t ox = 0, oy = 0; // global fine pixel of the tile's local (0,0)
    uint32_t dim = 0;
    uint64_t seed = 0;
    bool ok = false;
};

BathySamplerFixture loadBathySampler() {
    BathySamplerFixture f;
    const std::filesystem::path path = fineBathyFixturePath();
    if (!std::filesystem::exists(path)) return f;
    std::optional<std::vector<uint8_t>> bytes = readFileBytes(path);
    if (!bytes) return f;
    std::optional<FineTile> tile = FineTile::parse(*bytes, {}, nullptr);
    if (!tile) return f;
    f.tx = tile->tileX();
    f.ty = tile->tileY();
    f.ox = int64_t(f.tx) * int64_t(tile->size());
    f.oy = int64_t(f.ty) * int64_t(tile->size());
    f.dim = tile->blockDim();
    f.seed = tile->seed();
    f.tiles = std::make_unique<FineTileSampler>(f.seed);
    f.ok = f.tiles->loadTile(std::move(*tile));
    return f;
}

} // namespace

VXC_TEST(sample_bathy_rect_matches_the_per_pixel_accessor) {
    BathySamplerFixture f = loadBathySampler();
    if (!f.ok) {
        std::printf("  (skipped: bathy fixture absent)\n");
        return;
    }
    // A 7x5 window straddling the pinned centre pixel (local 128, 200), read
    // into a buffer whose stride is WIDER than the window -- the sub-window case
    // the texture filler uses, and the one an off-by-one in the row indexing
    // shows up in immediately.
    constexpr int64_t kW = 7, kH = 5, kStride = 11;
    const int64_t px0 = f.ox + 125, py0 = f.oy + 198;
    std::vector<int16_t> depth(size_t(kStride * kH), 12345);
    std::vector<int16_t> shore(size_t(kStride * kH), 12345);

    BathyRectStats st = sampleBathyRect(*f.tiles, px0, py0, px0 + kW - 1, py0 + kH - 1,
                                        depth.data(), shore.data(), kStride);
    CHECK_EQ(int(st.cells), int(kW * kH));
    CHECK_EQ(int(st.filled), int(kW * kH));
    CHECK(st.complete());
    CHECK_EQ(int(st.missingTiles + st.noPlane + st.notResident + st.decodeFailed), 0);

    // Cell for cell against FineTile::bathyAt, which is a completely different
    // code path (decode-per-pixel) reaching the same bytes. The two disagreeing
    // is the only way the block/sub-rect arithmetic can be wrong and still look
    // plausible.
    const FineTile* tile = f.tiles->findTile(f.tx, f.ty);
    CHECK(tile != nullptr);
    for (int64_t y = 0; y < kH; ++y) {
        for (int64_t x = 0; x < kW; ++x) {
            int32_t wantDepthMm = 0, wantShoreMm = 0;
            CHECK(tile->bathyAt(uint32_t(px0 + x - f.ox), uint32_t(py0 + y - f.oy),
                                wantDepthMm, wantShoreMm));
            const size_t i = size_t(y * kStride + x);
            CHECK_EQ(int(bathyDepthMm(depth[i])), int(wantDepthMm));
            CHECK_EQ(int(bathyShoreMm(shore[i])), int(wantShoreMm));
        }
    }
    // The centre pin itself, so this test fails on a shifted window and not only
    // on a mismatched one.
    CHECK_EQ(int(depth[size_t(2 * kStride + 3)]), int(kBathyCentreDepthUnits));
    CHECK_EQ(int(shore[size_t(2 * kStride + 3)]), int(kBathyCentreShoreUnits));
    // PADDING UNTOUCHED. Columns kW..kStride-1 of every row must still hold the
    // poison value: a filler that wrote its own width instead of the caller's
    // stride would have marched across them.
    for (int64_t y = 0; y < kH; ++y) {
        for (int64_t x = kW; x < kStride; ++x) {
            CHECK_EQ(int(depth[size_t(y * kStride + x)]), 12345);
            CHECK_EQ(int(shore[size_t(y * kStride + x)]), 12345);
        }
    }
}

VXC_TEST(sample_bathy_rect_crosses_block_and_tile_edges) {
    BathySamplerFixture f = loadBathySampler();
    if (!f.ok) {
        std::printf("  (skipped: bathy fixture absent)\n");
        return;
    }
    CHECK_EQ(int(f.dim), 256);

    // 1. ACROSS A BLOCK EDGE, from the CODED lake block (0,0) into the CONSTANT
    //    dry block (1,0). One rect, two block modes, two decodes -- and the two
    //    planes hold different constants on the dry side, so a filler that
    //    decoded one index and read the other lands here.
    constexpr int64_t kW = 6, kH = 2;
    const int64_t px0 = f.ox + 253, py0 = f.oy + 100;
    std::vector<int16_t> depth(size_t(kW * kH), 0), shore(size_t(kW * kH), 0);
    BathyRectStats st = sampleBathyRect(*f.tiles, px0, py0, px0 + kW - 1, py0 + kH - 1,
                                        depth.data(), shore.data(), kW);
    CHECK(st.complete());
    CHECK_EQ(int(st.cells), int(kW * kH));
    for (size_t i = 0; i < depth.size(); ++i) {
        // Row 100 of this fixture is dry on both sides of the edge; what the
        // test is pinning is that BOTH blocks answered, with their own plane's
        // constant, and neither cell came back as the missing sentinel.
        CHECK(depth[i] != kBathyMissing);
        CHECK(shore[i] != kBathyMissing);
        CHECK_EQ(int(depth[i]), int(kBathyDryDepth));
        CHECK_EQ(int(shore[i]), -int(kBathyShoreClampUnits));
    }

    // 2. OFF THE LOADED TILE. The right half of this rect belongs to the
    //    neighbour tile, which was never loaded: those cells must come back as
    //    kBathyMissing and be charged to missingTiles, while the left half is
    //    still filled normally. A partial answer is the normal steady state at
    //    the edge of the streamed set, so it must not poison the whole call.
    const int64_t ex0 = f.ox + 508;
    std::vector<int16_t> ed(size_t(kW * kH), 0), es(size_t(kW * kH), 0);
    BathyRectStats est = sampleBathyRect(*f.tiles, ex0, py0, ex0 + kW - 1, py0 + kH - 1,
                                         ed.data(), es.data(), kW);
    CHECK_EQ(int(est.cells), int(kW * kH));
    CHECK_EQ(int(est.filled), int(4 * kH));         // local x 508..511
    CHECK_EQ(int(est.missingTiles), int(2 * kH));   // local x 512..513 -> next tile
    CHECK(!est.complete());
    for (int64_t y = 0; y < kH; ++y) {
        for (int64_t x = 0; x < kW; ++x) {
            const size_t i = size_t(y * kW + x);
            if (x < 4) {
                CHECK(ed[i] != kBathyMissing);
                CHECK(es[i] != kBathyMissing);
            } else {
                CHECK_EQ(int(ed[i]), int(kBathyMissing));
                CHECK_EQ(int(es[i]), int(kBathyMissing));
            }
        }
    }
    // AND THE SENTINEL IS NOT A DEPTH. This is the assertion the whole sentinel
    // choice exists for: run the hole through the ordinary converters and it
    // must not read as shallow water sitting on the waterline.
    CHECK(bathyDepthIsDry(kBathyMissing));
    CHECK(!bathyShoreIsWater(kBathyMissing));
}

VXC_TEST(sample_bathy_rect_degenerate_inputs) {
    BathySamplerFixture f = loadBathySampler();
    if (!f.ok) {
        std::printf("  (skipped: bathy fixture absent)\n");
        return;
    }
    // An inverted rect fills nothing and is not an error.
    std::vector<int16_t> buf(4, 7);
    BathyRectStats inv = sampleBathyRect(*f.tiles, 10, 10, 9, 10, buf.data(), nullptr, 2);
    CHECK_EQ(int(inv.cells), 0);
    CHECK_EQ(int(buf[0]), 7);

    // One plane only: the other pointer is null and must not be dereferenced,
    // and the requested plane is still filled and still counted.
    std::vector<int16_t> shoreOnly(4, 7);
    BathyRectStats one = sampleBathyRect(*f.tiles, f.ox + 128, f.oy + 200,
                                         f.ox + 129, f.oy + 201, nullptr, shoreOnly.data(), 2);
    CHECK_EQ(int(one.cells), 4);
    CHECK_EQ(int(one.filled), 4);
    CHECK_EQ(int(shoreOnly[0]), int(kBathyCentreShoreUnits));

    // A sampler with nothing loaded has no grid stride at all -- every cell is a
    // missing tile, and nothing is left holding the caller's poison value.
    FineTileSampler empty(f.seed);
    std::vector<int16_t> d(4, 7), s(4, 7);
    BathyRectStats none = sampleBathyRect(empty, 0, 0, 1, 1, d.data(), s.data(), 2);
    CHECK_EQ(int(none.cells), 4);
    CHECK_EQ(int(none.filled), 0);
    CHECK_EQ(int(none.missingTiles), 4);
    for (size_t i = 0; i < 4; ++i) {
        CHECK_EQ(int(d[i]), int(kBathyMissing));
        CHECK_EQ(int(s[i]), int(kBathyMissing));
    }
}

VXC_TEST(vxtl_v2_bathy_flag_and_sections_must_agree_in_both_directions) {
    const std::filesystem::path path = fineBathyFixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  (skipped: %s absent)\n", path.string().c_str());
        return;
    }
    std::optional<std::vector<uint8_t>> good = readFileBytes(path);
    CHECK(good.has_value());

    auto refused = [](const std::vector<uint8_t>& bytes, FineError want) {
        FineError err = FineError::kNone;
        std::optional<FineTile> t = FineTile::parse(bytes, {}, &err);
        CHECK(!t.has_value());
        CHECK_EQ(int(err), int(want));
    };
    // Rewrites one section-table entry's ID, which is how a section is made to
    // "go missing" without moving a byte of payload: an id this parser does not
    // know is ignored (bounded, but not routed anywhere).
    auto renameSection = [&good](uint32_t from, uint32_t to) {
        std::vector<uint8_t> bad = *good;
        uint16_t nSections = 0;
        std::memcpy(&nSections, bad.data() + kFineHeaderBytes - 2, 2);
        for (uint16_t i = 0; i < nSections; ++i) {
            const size_t e = kFineHeaderBytes + size_t(i) * kFineSectionEntryBytes;
            uint32_t id = 0;
            std::memcpy(&id, bad.data() + e, 4);
            if (id == from) std::memcpy(bad.data() + e, &to, 4);
        }
        return bad;
    };

    // FLAG CLEAR, SECTIONS PRESENT. The direction that matters most: four
    // sections' worth of bytes would be silently ignored and every lake in the
    // tile would render flat and unshaded, with no error raised anywhere.
    {
        std::vector<uint8_t> bad = *good;
        uint16_t flags = 0;
        std::memcpy(&flags, bad.data() + 31, 2);
        flags &= static_cast<uint16_t>(~kFineFlagBathyPresent);
        std::memcpy(bad.data() + 31, &flags, 2);
        refused(bad, FineError::kBadSectionTable);
    }

    // FLAG SET, A SECTION MISSING -- and it is refused for EACH of the four
    // independently, because one flag bit covers both planes. A tile carrying
    // depth without shore distance is not a partial feature to be tolerated:
    // it is a file that disagrees with the producer that wrote it.
    const uint32_t kUnknownSectionId = 4242;
    for (uint32_t id : {uint32_t(kSectionBathyDepthIndex), uint32_t(kSectionBathyDepthData),
                        uint32_t(kSectionBathyShoreIndex), uint32_t(kSectionBathyShoreData)}) {
        refused(renameSection(id, kUnknownSectionId), FineError::kBadSectionTable);
    }
}

VXC_TEST(a_reader_without_the_bathy_flag_bit_refuses_a_v27_tile) {
    // THE BIT IS GENUINELY NEW, and this is the assertion that says so: the
    // mask a reader had at bake_ver 24 does not contain it. Spelled out from
    // the four bits that existed then rather than derived from kFineFlagsKnown,
    // so that adding a bit to the mask cannot quietly make this test agree.
    constexpr uint16_t kFlagsKnownAtBakeVer24 = kFineFlagFlowPresent | kFineFlagBasinsPresent |
                                                kFineFlagWaterPresent | kFineFlagHeadsPresent;
    CHECK_EQ(int(kFlagsKnownAtBakeVer24 & kFineFlagBathyPresent), 0);
    CHECK_EQ(int(kFineFlagsKnown & kFineFlagBathyPresent), int(kFineFlagBathyPresent));

    const std::filesystem::path path = fineBathyFixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  (skipped: %s absent)\n", path.string().c_str());
        return;
    }
    std::optional<std::vector<uint8_t>> bytes = readFileBytes(path);
    CHECK(bytes.has_value());

    // What the OLD reader would have computed on this very file. This is the
    // whole of the refusal test in tilestore.cpp -- `(flags & ~known) != 0` --
    // evaluated against the mask of the day, so it is the old build's verdict
    // and not a paraphrase of it.
    FineHeaderFacts facts;
    CHECK(fineReadHeaderFacts(bytes->data(), bytes->size(), facts));
    CHECK(facts.v2Fields);
    CHECK_EQ(int(facts.bakeVer), 27);
    CHECK((facts.flags & kFineFlagBathyPresent) != 0);
    CHECK((facts.flags & static_cast<uint16_t>(~kFlagsKnownAtBakeVer24)) != 0);
    // And THIS build reads the same file, which is the other half of the claim:
    // the refusal above is a property of the old mask, not of the bytes.
    CHECK_EQ(int(facts.unknownFlagBits()), 0);
    FineError err = FineError::kNone;
    CHECK(FineTile::parse(*bytes, {}, &err).has_value());
    CHECK_EQ(int(err), int(FineError::kNone));

    // The mechanism still fires for a bit NOBODY implements (bit6 -- bit5
    // became the bake_ver 28 placement planes, which is exactly why this test
    // must always use a bit past the mask's edge), which is what a reader
    // older than its tiles will meet next time, and it must say "the reader
    // is old" rather than "the file is broken".
    {
        std::vector<uint8_t> bad = *bytes;
        uint16_t flags = 0;
        std::memcpy(&flags, bad.data() + 31, 2);
        flags |= 0x40;
        std::memcpy(bad.data() + 31, &flags, 2);
        FineError e = FineError::kNone;
        CHECK(!FineTile::parse(bad, {}, &e).has_value());
        CHECK_EQ(int(e), int(FineError::kUnknownFeature));
        FineHeaderFacts badFacts;
        CHECK(fineReadHeaderFacts(bad.data(), bad.size(), badFacts));
        CHECK_EQ(int(badFacts.unknownFlagBits()), 0x40);
        const std::string why = fineDescribeRejection(FineError::kUnknownFeature, badFacts);
        CHECK(why.find("bake_ver up to " + std::to_string(kFineMaxKnownBakeVer)) !=
              std::string::npos);
    }
    // Bit5 SET but the ten placement sections missing is not "unknown", it is
    // a file that disagrees with itself -- the same section-agreement refusal
    // every other flag gets.
    {
        std::vector<uint8_t> bad = *bytes;
        uint16_t flags = 0;
        std::memcpy(&flags, bad.data() + 31, 2);
        flags |= kFineFlagPlacementPresent;
        std::memcpy(bad.data() + 31, &flags, 2);
        FineError e = FineError::kNone;
        CHECK(!FineTile::parse(bad, {}, &e).has_value());
        CHECK_EQ(int(e), int(FineError::kBadSectionTable));
    }
}

// ---------------------------------------------------------------------------
// SECTION_PLACE_* (bake_ver 28). Cross-language, same rule as every plane
// above: the fixture was written by terrain_service/tile_codec.py against the
// same format document, sharing no code with this decoder, and the expected
// values below are the generator's FORMULAS recomputed, not its output copied.
// ---------------------------------------------------------------------------

namespace {

// make_placement_fixture.py's plane formulas, one spelling per language.
uint8_t placementFixtureExpected(FinePlacementPlane plane, uint32_t sx, uint32_t sy) {
    switch (plane) {
        case kPlacementDistWater: {
            const uint32_t d = (sx > 16 ? sx - 16 : 0) * 3;
            return static_cast<uint8_t>(d > 255 ? 255 : d);
        }
        case kPlacementTwi: {
            if (sx >= 112 && sy >= 112) return 255;
            const uint32_t t = (sx + sy) / 2;
            return static_cast<uint8_t>(t > 254 ? 254 : t);
        }
        case kPlacementTalus: return 0;
        case kPlacementCurv: {
            const int32_t m = ((int32_t(sx) - int32_t(sy)) % 64 + 64) % 64;
            const int32_t v = 128 + m - 32;
            return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
        }
        case kPlacementHeat: return static_cast<uint8_t>((sx * 2 + sy) % 255);
        default: return 0;
    }
}

} // namespace

VXC_TEST(placement_fixture_parses_and_every_plane_reads_back_by_formula) {
    const std::filesystem::path path = finePlacementFixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  (skipped: %s absent)\n", path.string().c_str());
        return;
    }
    std::optional<std::vector<uint8_t>> bytes = readFileBytes(path);
    CHECK(bytes.has_value());
    FineError err = FineError::kNone;
    std::optional<FineTile> tile = FineTile::parse(*bytes, {}, &err);
    CHECK(tile.has_value());
    CHECK(tile->hasPlacement());
    CHECK_EQ(int(tile->header().bakeVer), 28);
    CHECK_EQ(int(tile->placementEdge()), 128);
    CHECK_EQ(int(tile->placementBlockLog2()), 7); // derived: one 128-px block
    CHECK_EQ(int(tile->placementBlockCount()), 1);

    // Every subsampled pixel of every plane, through decodePlacementBlock.
    std::vector<uint8_t> block;
    for (uint32_t p = 0; p < kPlacementPlaneCount; ++p) {
        const FinePlacementPlane plane = static_cast<FinePlacementPlane>(p);
        CHECK(tile->placementBlockResident(plane, 0, 0));
        CHECK(tile->decodePlacementBlock(plane, 0, 0, block, &err));
        for (uint32_t sy = 0; sy < 128; ++sy)
            for (uint32_t sx = 0; sx < 128; ++sx) {
                if (block[sy * 128 + sx] != placementFixtureExpected(plane, sx, sy)) {
                    CHECK_EQ(int(block[sy * 128 + sx]),
                             int(placementFixtureExpected(plane, sx, sy)));
                    return; // one mismatch prints; 81,920 would not help
                }
            }
    }

    // The cold-path per-pixel read folds the 4x subsample itself: fine pixel
    // (67, 250) is subsampled pixel (16, 62) -- the shoreline column, where
    // the distance is exactly 0 (wet).
    uint8_t v[kPlacementPlaneCount];
    CHECK(tile->placementAt(67, 250, v, &err));
    CHECK_EQ(int(v[kPlacementDistWater]), 0);
    CHECK_EQ(int(v[kPlacementTwi]), int(placementFixtureExpected(kPlacementTwi, 16, 62)));
    CHECK_EQ(int(v[kPlacementTalus]), 0);

    // And the conversions on real wire values: a stored 30 is 60 m.
    CHECK_EQ(placementDistanceMm(30), 60'000);

    // The sampler's voxel-addressed read, through its block cache. The tile
    // is at coarse (-7, 3): fine pixel (-7*512 + 20, 3*512 + 40), and
    // 18.75 voxels per fine pixel puts a voxel squarely inside it.
    FineTileSampler sampler(20260719);
    CHECK(sampler.loadTile(std::move(*tile)));
    const int64_t px = -7 * 512 + 20, py = 3 * 512 + 40;
    const int64_t vx = px * 1875 / kVoxelSizeMm, vy = py * 1875 / kVoxelSizeMm;
    FineTileSampler::FinePlacementSample s = sampler.placementAtVoxel(vx, vy);
    CHECK(s.valid);
    CHECK_EQ(int(s.distWater), int(placementFixtureExpected(kPlacementDistWater, 20 / 4, 40 / 4)));
    CHECK_EQ(int(s.twi), int(placementFixtureExpected(kPlacementTwi, 5, 10)));
    CHECK_EQ(int(s.curv), int(placementFixtureExpected(kPlacementCurv, 5, 10)));
    CHECK_EQ(int(s.heat), int(placementFixtureExpected(kPlacementHeat, 5, 10)));
    // Warm now: the same query is a pure read.
    CHECK(sampler.placementAtVoxel(vx, vy).valid);
}

VXC_TEST(placement_planes_absent_serve_sentinels_not_zeros) {
    // A pre-28 tile (the basin fixture) has no planes: hasPlacement() is
    // false, the per-pixel read refuses, and the SAMPLER answers an invalid
    // sample whose fields are the fail-closed sentinels -- never zeros, which
    // would read as "wet, at the shoreline, maximum talus" and invert every
    // gate that consumes them.
    const std::filesystem::path path = fineBasinFixturePath();
    if (!std::filesystem::exists(path)) {
        std::printf("  (skipped: %s absent)\n", path.string().c_str());
        return;
    }
    std::optional<std::vector<uint8_t>> bytes = readFileBytes(path);
    CHECK(bytes.has_value());
    std::optional<FineTile> tile = FineTile::parse(*bytes, {});
    CHECK(tile.has_value());
    CHECK(!tile->hasPlacement());
    uint8_t v[kPlacementPlaneCount];
    FineError err = FineError::kNone;
    CHECK(!tile->placementAt(10, 10, v, &err));
    CHECK(!tile->placementBlockResident(kPlacementDistWater, 0, 0));

    FineTileSampler sampler(tile->seed());
    const int32_t tx = tile->tileX(), ty = tile->tileY();
    const uint32_t size = tile->size();
    CHECK(sampler.loadTile(std::move(*tile)));
    const int64_t px = int64_t(tx) * size + 8, py = int64_t(ty) * size + 8;
    FineTileSampler::FinePlacementSample s =
        sampler.placementAtVoxel(px * 1875 / kVoxelSizeMm, py * 1875 / kVoxelSizeMm);
    CHECK(!s.valid);
    CHECK_EQ(int(s.distWater), int(kPlacementDistUnknown));
    CHECK_EQ(int(s.twi), int(kPlacementTwiUnknown));
    // == assetpolicy.h's kAssetNoWaterDistanceMm; assetchannels.h pins them.
    CHECK_EQ(placementDistanceMm(s.distWater), INT32_MAX);
}
