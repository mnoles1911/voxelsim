// Worldgen hash v1 goldens (docs/determinism.md). These values are the
// determinism contract: if any of them changes, every saved world breaks.
// Regenerate ONLY on a deliberate kWorldGenVersion bump (build with
// -DVXC_PRINT_GOLDENS in the bench target to reprint).

#include "voxelcore/hash.h"
#include "voxelcore/hash_channel_registry.h"
#include "vxctest.h"

using namespace vxc;

VXC_TEST(splitmix64_goldens) {
    CHECK_EQ(splitmix64(0), 0xE220A8397B1DCDAFull);
    CHECK_EQ(splitmix64(1), 0x910A2DEC89025CC1ull);
    CHECK_EQ(splitmix64(0xDEADBEEFull), 0x4ADFB90F68C9EB9Bull);
}

VXC_TEST(hash2_hash3_goldens) {
    CHECK_EQ(hash2(1, 0, 0, 0), 0x2A4F111B3BE57715ull);
    CHECK_EQ(hash2(1, -3, 7, CH_TOPSOIL_JITTER), 0x52B06D9E06B210C0ull);
    CHECK_EQ(hash3(42, 100, -200, 300, 5), 0x418B86146DE7DB86ull);
}

VXC_TEST(hash_to_signed16_range_and_goldens) {
    CHECK_EQ(hashToSigned16(0), -32768);
    CHECK_EQ(hashToSigned16(~0ull), 32767);
    CHECK_EQ(hashToSigned16(hash2(1, 2, 3, 4)), -14356);
}

VXC_TEST(value_noise_matches_lattice_at_lattice_points) {
    const uint64_t seed = 99;
    // At exact lattice points the bilinear reduces to the corner hash.
    for (int64_t lx = -3; lx <= 3; ++lx)
        for (int64_t ly = -3; ly <= 3; ++ly) {
            const int64_t n = valueNoise2(seed, lx * 400, ly * 400, 400, 7);
            CHECK_EQ(n, int64_t(hashToSigned16(hash2(seed, lx, ly, 7))));
        }
}

VXC_TEST(value_noise_bounded_and_deterministic) {
    const uint64_t seed = 1234;
    for (int64_t x = -5000; x <= 5000; x += 137)
        for (int64_t y = -5000; y <= 5000; y += 251) {
            const int64_t a = valueNoise2(seed, x, y, 1600, 3);
            const int64_t b = valueNoise2(seed, x, y, 1600, 3);
            CHECK_EQ(a, b);
            CHECK(a >= -32768 && a <= 32767);
        }
}

// Guards the fix for the CH_CAVE_NODE/CH_CAVE_EDGE vs CH_ECOTONE_TEMP/
// CH_ECOTONE_PRECIP double allocation (both pairs used to be 18/19). The
// static_assert in hash_channel_registry.h already fails the BUILD on any
// such collision; this test additionally exercises the same check at run
// time (so a coverage/mutation tool sees it exercised) and pins the specific
// ids the fix landed on, so an accidental revert back to 18/19 is caught
// here even before someone rebuilds far enough to hit the static_assert.
VXC_TEST(hash_channel_registry_has_no_collisions) {
    CHECK(vxc::channel_registry_detail::firstCollisionOrNegativeOne() == -1);
    CHECK_EQ(static_cast<uint32_t>(CH_CAVE_NODE), 30u);
    CHECK_EQ(static_cast<uint32_t>(CH_CAVE_EDGE), 31u);
    CHECK(CH_CAVE_NODE != CH_ECOTONE_TEMP);
    CHECK(CH_CAVE_EDGE != CH_ECOTONE_PRECIP);
}

VXC_TEST(value_noise_continuity_across_lattice_cells) {
    // Adjacent samples must not jump more than the max lattice slope
    // (2*65535/L per mm step) — catches floor/rounding seams at cell borders.
    const uint64_t seed = 55;
    const int64_t L = 400;
    int64_t prev = valueNoise2(seed, -L * 2, 123, L, 1);
    for (int64_t x = -L * 2 + 1; x <= L * 2; ++x) {
        const int64_t cur = valueNoise2(seed, x, 123, L, 1);
        const int64_t jump = cur > prev ? cur - prev : prev - cur;
        CHECK(jump <= 2 * 65535 / L + 2);
        prev = cur;
    }
}
