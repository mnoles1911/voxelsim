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

// --- surface upper bound (voxelcore/amplifier.h) ----------------------------
//
// Amplifier::surfaceUpperBoundMm is a SAFETY primitive: the sky-band trim skips
// streaming any chunk that sits entirely above it, so a bound that is ever too
// low is an invisible hole in the terrain — a chunk declared all-air that
// really contains rock. These tests are written to BREAK it, not to demonstrate
// it: randomised footprints, adversarial placement, and dense sampling inside
// each one.

namespace {

// Deterministic RNG for the sweep — splitmix64 so the whole test is reproducible
// byte-for-byte on every compiler (it is worldgen's own mixer, used here only as
// a test generator, never as worldgen randomness).
struct BoundRng {
    uint64_t s;
    explicit BoundRng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        s = splitmix64(s + 0x9E3779B97F4A7C15ull);
        return s;
    }
    // Inclusive.
    int64_t range(int64_t lo, int64_t hi) {
        return lo + static_cast<int64_t>(next() % static_cast<uint64_t>(hi - lo + 1));
    }
    bool chance(int64_t percent) { return static_cast<int64_t>(next() % 100u) < percent; }
};

struct BoundStats {
    int64_t footprints = 0;
    int64_t samples = 0;
    int64_t violations = 0;
    int64_t declined = 0;
    int64_t minSlackMm = 1LL << 60; // tightest (bound - max sampled surface)
    int64_t maxSlackMm = -(1LL << 60);
    int64_t slackSumMm = 0;
    int32_t minSurfaceMm = 2147483647;
    int32_t maxSurfaceMm = -2147483647;
};

// Check one footprint: bound it, then hammer it with dense samples.
void probeFootprint(const Amplifier& amp, int64_t vx0, int64_t vy0, int64_t vx1, int64_t vy1,
                    BoundRng& rng, BoundStats& st, int64_t sampleBudget) {
    const int64_t bound = amp.surfaceUpperBoundMm(vx0, vy0, vx1, vy1);
    if (bound == kSurfaceBoundDeclined) {
        ++st.declined;
        return;
    }
    ++st.footprints;

    const int64_t nx = vx1 - vx0 + 1, ny = vy1 - vy0 + 1;
    // Stride chosen so a big footprint still gets a dense-ish lattice, then
    // jittered per footprint so successive footprints probe DIFFERENT columns
    // rather than all landing on the same aligned sub-lattice (which would
    // systematically miss whatever falls between).
    int64_t stride = 1;
    while ((nx / stride + 1) * (ny / stride + 1) > sampleBudget) ++stride;
    const int64_t jx = rng.range(0, stride - 1), jy = rng.range(0, stride - 1);

    int64_t maxSeen = -(1LL << 60);
    auto sample = [&](int64_t vx, int64_t vy) {
        const int32_t s = amp.surfaceMm(vx, vy);
        ++st.samples;
        if (s > maxSeen) maxSeen = s;
        if (s < st.minSurfaceMm) st.minSurfaceMm = s;
        if (s > st.maxSurfaceMm) st.maxSurfaceMm = s;
        if (int64_t(s) > bound) {
            ++st.violations;
            if (st.violations <= 8)
                std::printf("    [BOUND VIOLATION] footprint [%lld,%lld]x[%lld,%lld] bound=%lld "
                            "but surfaceMm(%lld,%lld)=%d\n",
                            static_cast<long long>(vx0), static_cast<long long>(vx1),
                            static_cast<long long>(vy0), static_cast<long long>(vy1),
                            static_cast<long long>(bound), static_cast<long long>(vx),
                            static_cast<long long>(vy), s);
        }
    };

    for (int64_t y = vy0 + jy; y <= vy1; y += stride)
        for (int64_t x = vx0 + jx; x <= vx1; x += stride) sample(x, y);
    // The four corners always: a corner is where the bilinear patch attains its
    // maximum, so it is exactly what the strided lattice must not be allowed to
    // skip.
    sample(vx0, vy0);
    sample(vx1, vy0);
    sample(vx0, vy1);
    sample(vx1, vy1);
    // Plus uniformly random interior columns, which the lattice cannot cover.
    for (int i = 0; i < 48; ++i) sample(rng.range(vx0, vx1), rng.range(vy0, vy1));

    const int64_t slack = bound - maxSeen;
    if (slack < st.minSlackMm) st.minSlackMm = slack;
    if (slack > st.maxSlackMm) st.maxSlackMm = slack;
    st.slackSumMm += slack;
}

} // namespace

VXC_TEST(amplifier_surfaceMm_matches_column) {
    // surfaceUpperBoundMm bounds surfaceMm(); the trim's soundness argument is
    // about ColumnSample::surfaceMm. This is the equality that joins them, and
    // it must hold at EVERY column, not approximately.
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    for (int64_t x = -5000; x <= 5000; x += 137)
        for (int64_t y = -5000; y <= 5000; y += 191)
            CHECK_EQ(amp.surfaceMm(x, y), amp.column(x, y).surfaceMm);
}

VXC_TEST(amplifier_surface_bound_adversarial) {
    // Four environments: both real tile pixel sizes (30 m scale-1 and 11.25 m
    // scale-8, which give DIFFERENT corner-grid shapes for the same footprint)
    // crossed with different world seeds.
    struct Env {
        uint64_t seed;
        int32_t pixelMm;
    };
    const Env envs[] = {
        {kSeed, 30000}, {kSeed, 11250}, {0x5DEECE66Dull, 30000}, {1, 11250}};

    BoundStats st;
    int64_t hotspotFootprints = 0;

    for (const Env& env : envs) {
        SyntheticTileSampler tiles(env.seed, env.pixelMm);
        Amplifier amp(env.seed, tiles);
        BoundRng rng(env.seed * 31 + static_cast<uint64_t>(env.pixelMm));

        // Pre-scan for the STEEPEST neighbourhoods in this world and aim half
        // the footprints at them. A bound that only ever sees gentle ground is
        // not being tested: the places it can fail are the cliffs, where the
        // bilinear base swings hardest across a footprint and the slope-scale
        // term is largest.
        constexpr int32_t kScan = 96;
        constexpr int64_t kScanStride = 512; // 51.2 m
        int64_t hotX[16] = {}, hotY[16] = {};
        int64_t hotGrad[16] = {};
        for (int32_t iy = 0; iy < kScan; ++iy) {
            for (int32_t ix = 0; ix < kScan; ++ix) {
                const int64_t x = (ix - kScan / 2) * kScanStride;
                const int64_t y = (iy - kScan / 2) * kScanStride;
                const int64_t a = amp.surfaceMm(x, y);
                const int64_t gx = amp.surfaceMm(x + kScanStride, y) - a;
                const int64_t gy = amp.surfaceMm(x, y + kScanStride) - a;
                const int64_t g = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
                // Insertion into a tiny top-16 table.
                for (int32_t k = 0; k < 16; ++k) {
                    if (g > hotGrad[k]) {
                        for (int32_t m = 15; m > k; --m) {
                            hotGrad[m] = hotGrad[m - 1];
                            hotX[m] = hotX[m - 1];
                            hotY[m] = hotY[m - 1];
                        }
                        hotGrad[k] = g;
                        hotX[k] = x;
                        hotY[k] = y;
                        break;
                    }
                }
            }
        }

        for (int32_t trial = 0; trial < 220; ++trial) {
            // Size: half the real caller's shapes (a level-L chunk footprint is
            // 16 << L voxels on a side, L = 0..5), half arbitrary — including
            // degenerate 1-wide slivers and single columns, where the clipped
            // rectangle collapses to a segment or a point.
            int64_t w = 0, h = 0;
            if (rng.chance(50)) {
                const int64_t lvl = rng.range(0, 5);
                w = h = int64_t(16) << lvl;
            } else if (rng.chance(20)) {
                w = rng.range(1, 3);
                h = rng.range(1, 400);
            } else {
                w = rng.range(1, 500);
                h = rng.range(1, 500);
            }

            // Placement.
            int64_t x0 = 0, y0 = 0;
            if (rng.chance(50)) {
                // Aimed at a steep neighbourhood, with a jitter comparable to a
                // tile pixel so the footprint lands at every possible phase
                // relative to the pixel grid.
                const int64_t k = rng.range(0, 15);
                x0 = hotX[k] + rng.range(-600, 600);
                y0 = hotY[k] + rng.range(-600, 600);
                ++hotspotFootprints;
            } else {
                // Anywhere, including far negative coordinates — floorDiv's
                // sign handling is exactly where an off-by-one pixel index
                // would hide.
                x0 = rng.range(-2000000, 2000000);
                y0 = rng.range(-2000000, 2000000);
            }
            // A quarter of the time, snap to the chunk grid (what production
            // actually asks about); otherwise leave it at an arbitrary offset,
            // which is the harder case for the clip-to-pixel-cell logic.
            if (rng.chance(25)) {
                x0 -= ((x0 % w) + w) % w;
                y0 -= ((y0 % h) + h) % h;
            }
            probeFootprint(amp, x0, y0, x0 + w - 1, y0 + h - 1, rng, st, 2000);
        }
    }

    std::printf("    [amplifier] surface bound: %lld footprints (%lld aimed at the steepest "
                "ground, %lld declined), %lld dense samples, relief %d..%d mm (%lld m); "
                "slack min/mean/max = %lld / %lld / %lld mm; VIOLATIONS=%lld\n",
                static_cast<long long>(st.footprints), static_cast<long long>(hotspotFootprints),
                static_cast<long long>(st.declined), static_cast<long long>(st.samples),
                st.minSurfaceMm, st.maxSurfaceMm,
                static_cast<long long>((st.maxSurfaceMm - st.minSurfaceMm) / 1000),
                static_cast<long long>(st.minSlackMm),
                static_cast<long long>(st.footprints ? st.slackSumMm / st.footprints : 0),
                static_cast<long long>(st.maxSlackMm),
                static_cast<long long>(st.violations));

    // THE claim.
    CHECK_EQ(st.violations, 0);

    // The sweep must actually have swept: if any of these regress, the test
    // above has stopped proving anything.
    CHECK(st.footprints > 700);
    CHECK(st.samples > 400000);
    // The corpus really does contain mountains-to-ocean relief.
    CHECK(st.maxSurfaceMm - st.minSurfaceMm > 300000);
    // And the bound is not trivially large: somewhere in there it came within
    // the detail allowance of the true maximum.
    CHECK(st.minSlackMm >= 0);
    CHECK(st.minSlackMm < 12000);
}

VXC_TEST(amplifier_surface_bound_single_column_is_base_exact) {
    // A 1x1 footprint clips to a POINT inside one pixel cell, so the bilinear
    // base term is bounded exactly and the only slack left is the detail
    // allowance: at most 2 * kDetailMaxMm * maxSlopeScale / 1024 = 22'848 mm,
    // attained when the true detail sits at its minimum. Asserting that ceiling
    // is how this test detects the base term silently becoming conservative
    // (e.g. someone reverting to "largest pixel corner elevation").
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    int64_t worst = 0, checked = 0;
    for (int64_t x = -30000; x <= 30000; x += 1013)
        for (int64_t y = -30000; y <= 30000; y += 1279) {
            const int64_t b = amp.surfaceUpperBoundMm(x, y, x, y);
            CHECK(b != kSurfaceBoundDeclined);
            const int64_t s = amp.surfaceMm(x, y);
            CHECK(b >= s);
            if (b - s > worst) worst = b - s;
            ++checked;
        }
    std::printf("    [amplifier] single-column bound slack: worst %lld mm over %lld columns\n",
                static_cast<long long>(worst), static_cast<long long>(checked));
    CHECK(checked > 2000);
    CHECK(worst <= 22848);
}

VXC_TEST(amplifier_surface_bound_declines_rather_than_guesses) {
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    // Empty footprints.
    CHECK_EQ(amp.surfaceUpperBoundMm(10, 0, 9, 0), kSurfaceBoundDeclined);
    CHECK_EQ(amp.surfaceUpperBoundMm(0, 10, 0, 9), kSurfaceBoundDeclined);
    // A footprint far too large for the corner budget must DECLINE (the safe
    // answer), never return a number it cannot justify. 16 corners per axis
    // against a 30 m pixel is 450 m; ask for 100 km.
    CHECK_EQ(amp.surfaceUpperBoundMm(0, 0, 1000000, 1000000), kSurfaceBoundDeclined);
    // ...and the largest footprint it does accept is still bounded correctly.
    const int64_t big = amp.surfaceUpperBoundMm(0, 0, 4400, 4400);
    CHECK(big != kSurfaceBoundDeclined);
    for (int64_t x = 0; x <= 4400; x += 97)
        for (int64_t y = 0; y <= 4400; y += 89) CHECK(int64_t(amp.surfaceMm(x, y)) <= big);
}

VXC_TEST(amplifier_surface_bound_golden_digest) {
    // GOLDEN(amplifier_surface_bound) — NOT worldgen output (the bound is a
    // derived query; kWorldGenVersion does not cover it and must not move for
    // it). Pinned so that a change to the bound's arithmetic — including one
    // that only makes it LOOSER, which the adversarial test above would happily
    // accept — shows up as a deliberate decision rather than a silent drift in
    // how many chunks the sky-band trim skips.
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    Digest d;
    for (int64_t lvl = 0; lvl <= 4; ++lvl) {
        const int64_t span = int64_t(16) << lvl;
        for (int64_t cy = -6; cy < 6; ++cy)
            for (int64_t cx = -6; cx < 6; ++cx)
                d.i64(amp.surfaceUpperBoundMm(cx * span, cy * span, cx * span + span - 1,
                                              cy * span + span - 1));
    }
    CHECK_EQ(d.h, 0x5588EBCD842ECE3Dull);
}
