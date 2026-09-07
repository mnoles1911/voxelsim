// F7 DETERMINISM PINS (docs/water-ocean-tides-plan-2026-09-04.md Phase F7:
// "a cross-process determinism test (two runs, same seed + epoch, compare
// tideOffsetMm/oceanconnect bit output) in voxel-core tests").
//
// WHAT THIS FILE ADDS over test_tide.cpp / test_oceanconnect.cpp, which
// already pin exact values at chosen points: a WHOLE-SWEEP byte contract.
// Every output the authority layer produces over a broad sweep of specs,
// epochs, quanta, grids and tide levels is serialised to one byte stream
// (explicit little-endian byte extraction, so the stream is the same bytes on
// any endianness) and compared two ways:
//
//   1. IN-PROCESS DOUBLE EVALUATION with independently-constructed state:
//      every spec, grid and buffer is built fresh inside the sweep function,
//      the function runs twice, and the two byte vectors must be EQUAL -- no
//      shared caches, no order dependence, no hidden state.
//   2. AGAINST A GOLDEN FNV-1a-64 HASH pinned as a literal. The literal is
//      the CROSS-PROCESS half of the claim, in exactly the sense the pinned
//      table values in test_tide.cpp are (46341 et al.): it was recorded from
//      one process and every subsequent run of this suite -- every machine,
//      every compiler CI throws at it -- is a different process agreeing with
//      that recording byte for byte. The harness has no golden-file
//      record/replay mode, so the pin IS the recording; this is stated
//      honestly rather than dressed up as something else.
//
// THE MUST-FAIL DISCIPLINE (test-must-be-able-to-fail): both pins were
// red-armed in scratch builds before the goldens landed --
//   - tide arm: tideOffsetMm rewritten over libm sin(double) instead of the
//     Q16 table; the sweep hash moved and the golden CHECK failed (test
//     _tide.cpp's cardinal pins also tripped, as designed). That is the exact
//     cross-machine drift class the LUT exists to exclude: libm sin is not
//     bit-specified, so a build that "simplifies" to it stops being pinnable.
//   - oceanconnect arm: the fill widened to 8-connectivity; the bit output
//     changed on the diagonal-pinch fixtures and the golden CHECK failed.
//     (Note what would NOT trip the byte pin: reordering the BFS neighbour
//     walk -- the CONNECTED SET is order-invariant, so the pin binds the
//     answer, not the visit order. The visit-order construction is doctrine
//     in oceanconnect.h; the answer bytes are what multiplayer needs equal.)
//
// A hash-literal change in a future diff is therefore a REVIEW EVENT: it
// means the authority layer's observable output moved, which is a
// cross-machine compatibility break, not a refactor detail.

#include <cstdint>
#include <vector>

#include "voxelcore/oceanconnect.h"
#include "voxelcore/tide.h"
#include "vxctest.h"

using namespace vxc;

namespace {

// --- byte serialisation, endian-proof by construction ------------------------

void put32(std::vector<uint8_t>& v, int32_t x) {
    const uint32_t u = uint32_t(x);
    v.push_back(uint8_t(u));
    v.push_back(uint8_t(u >> 8));
    v.push_back(uint8_t(u >> 16));
    v.push_back(uint8_t(u >> 24));
}

void put64(std::vector<uint8_t>& v, int64_t x) {
    const uint64_t u = uint64_t(x);
    for (int i = 0; i < 8; ++i) v.push_back(uint8_t(u >> (8 * i)));
}

uint64_t fnv1a64(const std::vector<uint8_t>& v) {
    uint64_t h = 14695981039346656037ull;
    for (uint8_t b : v) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

// --- the tide sweep ----------------------------------------------------------
//
// Everything constructed locally, per call: the specs are rebuilt from
// literals each time (same doctrine as test_tide.cpp's defaultSpec -- the
// tests' copy is the one under version control), so two calls share nothing.
//
// Spec coverage: the shipping default, a lone component, a full four-component
// spec with a negative amplitude and awkward phases/periods, and an
// inert-mixed spec (periodS <= 0 components interleaved with a live one).
// Epoch coverage: a fine sweep across the default spec's common period
// including negative epochs (floorMod territory), plus a coarse sweep out to
// +-4e15 ms (~127k years) -- the overflow headroom tide.h argues for is
// exercised, not just asserted.
std::vector<uint8_t> tideSweepBytes() {
    std::vector<uint8_t> out;

    // The table itself is part of the contract: 4096 Q16 entries, byte-pinned.
    for (int32_t k = 0; k < kTideLutSize; ++k) put32(out, kTideSin.v[k]);

    TideSpec specs[4];
    specs[0].comp[0] = {720, 800, 0}; // shipping default
    specs[0].comp[1] = {360, 250, 250};
    specs[0].count = 2;
    specs[1].comp[0] = {720, 800, 0}; // lone component
    specs[1].count = 1;
    specs[2].comp[0] = {720, 800, 125}; // full house, one amplitude negative
    specs[2].comp[1] = {360, -250, 250};
    specs[2].comp[2] = {43200, 3000, 777};
    specs[2].comp[3] = {97, 13, 999};
    specs[2].count = 4;
    specs[3].comp[0] = {0, 5000, 0}; // inert (period 0)
    specs[3].comp[1] = {-7, 123, 0}; // inert (negative period)
    specs[3].comp[2] = {113, 77, 500};
    specs[3].count = 3;

    const int32_t quanta[3] = {25, 7, 1000};
    for (const TideSpec& s : specs) {
        // Fine: two default-spec common periods, prime-ish stride for odd
        // phases, negative epochs included.
        for (int64_t e = -720000; e <= 720000; e += 7321) {
            const int32_t off = tideOffsetMm(s, e);
            put32(out, off);
            for (int32_t q : quanta) put32(out, quantiseTideMm(off, q));
            put64(out, tideVelocityUmPerS(s, e));
        }
        // Coarse and far: the session-clock-runs-for-years regime.
        for (int64_t e = -4'000'000'000'000'000; e <= 4'000'000'000'000'000;
             e += 137'000'000'137'000) {
            const int32_t off = tideOffsetMm(s, e);
            put32(out, off);
            put32(out, quantiseTideMm(off, 25));
            put64(out, tideVelocityUmPerS(s, e));
        }
    }
    return out;
}

// --- the connectivity sweep --------------------------------------------------
//
// Three fixture families, every buffer freshly allocated per call:
//   1. the 16x16 ragged coast from test_oceanconnect.cpp's byte-for-byte
//      test, filled at SIX tide levels spanning the default spec's range;
//   2. a 33x33 arithmetic archipelago (odd n, so the centre column exists):
//      deep west edge, a sinuous-ish channel, below-sea inland playas that
//      the deep-margin guard must keep refusing at every level;
//   3. degenerate n = 1 and n = 2 windows.
// The bit buffer AND the stats go into the stream after every fill: the
// stats are part of what a leg log compares across machines.
std::vector<uint8_t> oceanSweepBytes() {
    std::vector<uint8_t> out;
    const int32_t kSea = 0;
    const int32_t kSeedLevel = kSea - 1050 - 1000;
    const int32_t tides[6] = {kSea - 1050, kSea - 500, kSea, kSea + 275, kSea + 525,
                              kSea + 1050};

    auto runFill = [&](const std::vector<int32_t>& ground, int32_t n, int32_t tideNow) {
        std::vector<uint8_t> bits(size_t(n) * size_t(n), 0xAB); // poison: the fill must clear
        const OceanConnectStats s =
            oceanConnectivityFill(ground.data(), n, tideNow, kSeedLevel, bits.data());
        put32(out, n);
        put32(out, tideNow);
        out.insert(out.end(), bits.begin(), bits.end());
        put32(out, int32_t(s.seedCells));
        put32(out, int32_t(s.wetCells));
        put32(out, int32_t(s.connectedCells));
    };

    // 1. The ragged coast, verbatim from the in-file determinism test.
    {
        const int32_t n = 16;
        std::vector<int32_t> g(size_t(n) * size_t(n));
        for (int32_t y = 0; y < n; ++y)
            for (int32_t x = 0; x < n; ++x)
                g[size_t(y) * n + x] =
                    ((x * 7 + y * 13) % 5 == 0) ? -9'000 : ((x + y) % 3 == 0 ? -400 : 2'000);
        for (int32_t t : tides) runFill(g, n, t);
    }

    // 2. The archipelago: fixed by arithmetic, no RNG, same doctrine.
    {
        const int32_t n = 33;
        std::vector<int32_t> g(size_t(n) * size_t(n));
        for (int32_t y = 0; y < n; ++y) {
            for (int32_t x = 0; x < n; ++x) {
                int32_t h = ((x * x * 31 + y * 17 + x * y * 7) % 2600) - 1300;
                if (x < 2) h = -9'500;                    // deep sea, west edge
                if (y == 16 && x < 24) h = -2'200 - x * 50; // a deepening channel inland
                if (x >= 28 && y >= 28) h = -700;         // border playa: wet, shallow --
                g[size_t(y) * n + x] = h;                 // the guard must refuse it
            }
        }
        for (int32_t t : tides) runFill(g, n, t);
    }

    // 3. Degenerate windows stay total and stay pinned.
    runFill(std::vector<int32_t>{-10'000}, 1, kSea);
    runFill(std::vector<int32_t>{-10'000, 2'000, -400, -9'000}, 2, kSea + 275);

    return out;
}

// --- the goldens -------------------------------------------------------------
//
// Recorded 2026-09-05 from this exact sweep code (llvm-mingw x86_64, then
// re-verified by a rebuilt binary in a separate process -- see the F7
// scoreboard row). Constexpr integer arithmetic and the integer BFS admit no
// platform variance, so these are the numbers on EVERY machine; a change here
// is a cross-machine compatibility break to be reviewed, never rubber-stamped.
constexpr uint64_t kTideSweepGolden = 0x8ed5ba67136bb539ull;
constexpr uint64_t kOceanSweepGolden = 0xb327d409b40ae580ull;

} // namespace

VXC_TEST(f7_tide_sweep_double_evaluation_agrees_and_matches_golden) {
    const std::vector<uint8_t> a = tideSweepBytes();
    const std::vector<uint8_t> b = tideSweepBytes(); // independent state, same answer
    CHECK(a == b);
    CHECK(a.size() > size_t(kTideLutSize) * 4); // the sweep ran past the table
    CHECK_EQ(fnv1a64(a), kTideSweepGolden);
}

VXC_TEST(f7_oceanconnect_sweep_double_evaluation_agrees_and_matches_golden) {
    const std::vector<uint8_t> a = oceanSweepBytes();
    const std::vector<uint8_t> b = oceanSweepBytes();
    CHECK(a == b);
    CHECK_EQ(fnv1a64(a), kOceanSweepGolden);
}
