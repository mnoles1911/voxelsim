// M0 benchmark + determinism harness (plan §5 tasks 2-4, §4 M0 gates).
//
//   vxc_bench --radius <m> [--seed N] [--brick 8|16] [--digest] [--goldens]
//
// Measures amplify (column synthesis), voxelize (brick fill) and mesh
// (greedy) wall-clock over all surface-shell bricks within a horizontal
// radius, per brick size. --digest prints only the world+mesh FNV digest —
// CI runs gcc and clang builds and asserts they match (cross-compiler proxy
// for the NVIDIA-vs-AMD M0 gate until GPU runners exist). Timing uses
// std::chrono; floats appear ONLY in reporting, never in world math.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "voxelcore/mesher.h"
#include "voxelcore/world.h"

using namespace vxc;

namespace {

struct Options {
    int64_t radiusM = 32;
    uint64_t seed = 20260719;
    int brick = 0; // 0 = both
    bool digestOnly = false;
    bool goldens = false;
};

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct BenchResult {
    uint64_t digest = 0;
    size_t footprints = 0, bricks = 0, nonEmptyBricks = 0, homogeneousBricks = 0;
    size_t solidVoxels = 0, quads = 0;
    double amplifyMs = 0, voxelizeMs = 0, meshMs = 0;
};

template <int B>
BenchResult run(const Options& opt) {
    SyntheticTileSampler tiles(opt.seed);
    Amplifier amp(opt.seed, tiles);
    GeneratedWorld<B> gen(amp);
    BenchResult r;
    Digest digest;

    const int64_t radiusVox = opt.radiusM * 1000 / kVoxelSizeMm;
    const int32_t bMin = static_cast<int32_t>(floorDiv(-radiusVox, B));
    const int32_t bMax = static_cast<int32_t>(floorDiv(radiusVox - 1, B));

    // Extended column grid: brick footprint plus a 1-voxel apron so the
    // mesher can cull/AO across brick borders without materializing
    // neighbors (they are the same deterministic function).
    std::vector<ColumnSample> ext((B + 2) * (B + 2));
    std::vector<Quad> quads;

    for (int32_t by = bMin; by <= bMax; ++by) {
        for (int32_t bx = bMin; bx <= bMax; ++bx) {
            ++r.footprints;
            const auto tAmp = Clock::now();
            for (int y = -1; y <= B; ++y)
                for (int x = -1; x <= B; ++x)
                    ext[(x + 1) + (B + 2) * (y + 1)] =
                        amp.column(int64_t(bx) * B + x, int64_t(by) * B + y);
            r.amplifyMs += msSince(tAmp);

            // Surface shell: bricks containing any column's topmost voxel.
            int64_t vzMin = INT64_MAX, vzMax = INT64_MIN;
            for (int y = 0; y < B; ++y)
                for (int x = 0; x < B; ++x) {
                    const ColumnSample& c = ext[(x + 1) + (B + 2) * (y + 1)];
                    const int64_t top =
                        floorDiv(c.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
                    vzMin = top < vzMin ? top : vzMin;
                    vzMax = top > vzMax ? top : vzMax;
                }
            const int32_t bzMin = static_cast<int32_t>(floorDiv(vzMin, B));
            const int32_t bzMax = static_cast<int32_t>(floorDiv(vzMax, B));

            for (int32_t bz = bzMin; bz <= bzMax; ++bz) {
                ++r.bricks;
                const auto tVox = Clock::now();
                Brick<B> brick;
                for (int y = 0; y < B; ++y)
                    for (int x = 0; x < B; ++x) {
                        const ColumnSample& c = ext[(x + 1) + (B + 2) * (y + 1)];
                        for (int z = 0; z < B; ++z)
                            brick.set(x, y, z,
                                      Amplifier::materialAt(c, int64_t(bz) * B + z));
                    }
                brick.tryCollapse();
                r.voxelizeMs += msSince(tVox);
                r.solidVoxels += brick.solidCount();
                if (!brick.empty()) ++r.nonEmptyBricks;
                if (brick.isHomogeneous()) ++r.homogeneousBricks;

                const auto tMesh = Clock::now();
                quads.clear();
                const auto sampler = [&](int x, int y, int z) -> MaterialId {
                    const ColumnSample& c = ext[(x + 1) + (B + 2) * (y + 1)];
                    return Amplifier::materialAt(c, int64_t(bz) * B + z);
                };
                meshBrick<B>(sampler, quads);
                r.meshMs += msSince(tMesh);
                r.quads += quads.size();

                digest.u32(static_cast<uint32_t>(bx));
                digest.u32(static_cast<uint32_t>(by));
                digest.u32(static_cast<uint32_t>(bz));
                brick.digest(digest);
                digestQuads(quads, digest);
            }
        }
    }
    r.digest = digest.h;
    return r;
}

void report(const char* label, const BenchResult& r) {
    const double totalMs = r.amplifyMs + r.voxelizeMs + r.meshMs;
    std::printf("=== brick %s ===\n", label);
    std::printf("footprints %zu, surface bricks %zu (non-empty %zu, homogeneous %zu)\n",
                r.footprints, r.bricks, r.nonEmptyBricks, r.homogeneousBricks);
    std::printf("solid voxels %zu, quads %zu\n", r.solidVoxels, r.quads);
    std::printf("amplify %.1f ms | voxelize %.1f ms | mesh %.1f ms | total %.1f ms\n",
                r.amplifyMs, r.voxelizeMs, r.meshMs, totalMs);
    if (totalMs > 0)
        std::printf("throughput: %.0f bricks/s, %.0f Mvoxel/s (solid)\n",
                    r.bricks / totalMs * 1000.0, r.solidVoxels / totalMs / 1000.0);
    std::printf("digest %016llx\n\n", static_cast<unsigned long long>(r.digest));
}

void printGoldens(const Options& opt) {
    // Regeneration aid for the pinned test constants (kWorldGenVersion bumps
    // only). Keep in sync with test_hash.cpp / test_amplifier.cpp.
    std::printf("splitmix64(0)            = 0x%016llXull\n",
                (unsigned long long)splitmix64(0));
    std::printf("splitmix64(1)            = 0x%016llXull\n",
                (unsigned long long)splitmix64(1));
    std::printf("splitmix64(0xDEADBEEF)   = 0x%016llXull\n",
                (unsigned long long)splitmix64(0xDEADBEEFull));
    std::printf("hash2(1,0,0,0)           = 0x%016llXull\n",
                (unsigned long long)hash2(1, 0, 0, 0));
    std::printf("hash2(1,-3,7,TOPSOIL)    = 0x%016llXull\n",
                (unsigned long long)hash2(1, -3, 7, CH_TOPSOIL_JITTER));
    std::printf("hash3(42,100,-200,300,5) = 0x%016llXull\n",
                (unsigned long long)hash3(42, 100, -200, 300, 5));
    std::printf("signed16(hash2(1,2,3,4)) = %d\n", hashToSigned16(hash2(1, 2, 3, 4)));

    SyntheticTileSampler tiles(20260719);
    Amplifier amp(20260719, tiles);
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
    std::printf("GOLDEN(amplifier_columns) = 0x%016llXull\n", (unsigned long long)d.h);
    (void)opt;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--radius" && i + 1 < argc) opt.radiusM = std::atoll(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) opt.seed = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--brick" && i + 1 < argc) opt.brick = std::atoi(argv[++i]);
        else if (a == "--digest") opt.digestOnly = true;
        else if (a == "--goldens") opt.goldens = true;
        else {
            std::fprintf(stderr,
                         "usage: vxc_bench [--radius m] [--seed n] [--brick 8|16] "
                         "[--digest] [--goldens]\n");
            return 2;
        }
    }
    if (opt.goldens) {
        printGoldens(opt);
        return 0;
    }
    if (opt.digestOnly) {
        // Digest mode: fixed deterministic output for cross-build comparison.
        if (opt.brick == 8) {
            std::printf("%016llx\n", (unsigned long long)run<8>(opt).digest);
        } else {
            std::printf("%016llx\n", (unsigned long long)run<16>(opt).digest);
        }
        return 0;
    }
    std::printf("voxel-core bench: radius %lldm, seed %llu (worldgen v%u)\n\n",
                (long long)opt.radiusM, (unsigned long long)opt.seed, kWorldGenVersion);
    if (opt.brick == 0 || opt.brick == 8) report("8^3", run<8>(opt));
    if (opt.brick == 0 || opt.brick == 16) report("16^3", run<16>(opt));
    return 0;
}
