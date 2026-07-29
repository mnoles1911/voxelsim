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
#include <unordered_map>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/mesher.h"
#include "voxelcore/mips.h"
#include "voxelcore/world.h"

using namespace vxc;

namespace {

struct Options {
    int64_t radiusM = 32;
    uint64_t seed = 20260719;
    int brick = 0; // 0 = both
    bool digestOnly = false;
    bool goldens = false;
    bool mips = false; // --mips: per-level chunk build, fine mip path vs coarse path
    int reps = 5;      // --reps: min-of-N for the --mips mode
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
    std::printf("digest %016llx\n", static_cast<unsigned long long>(r.digest));
#ifdef VXC_MEMO_STATS
    // COUNTS, not clocks. These are deterministic and unaffected by anything
    // else running on the machine, which the ms figures above emphatically are
    // not. Per column is the number that matters: the block memos exist to turn
    // ~16 per-column tile probes into ~1.
    const vxc::MemoStats& m = vxc::memoStats();
    // stencilProbes IS the column count: evalSurface makes exactly one per
    // column. The counterfactual is exact rather than estimated -- before the
    // block memo, evalSurface called cachedElevationMm 16 times per column, by
    // construction.
    const double cols = m.stencilProbes ? double(m.stencilProbes) : 1.0;
    std::printf("memo: columns %llu | stencil hit %.3f%% | elev probes %llu = %.4f/col "
                "(was 16/col before the block memo, a %.0fx reduction)\n",
                (unsigned long long)m.stencilProbes,
                100.0 * (1.0 - double(m.stencilMisses) / cols),
                (unsigned long long)m.elevProbes, double(m.elevProbes) / cols,
                m.elevProbes ? 16.0 * cols / double(m.elevProbes) : 0.0);
#endif
    std::printf("\n");
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

// ---------------------------------------------------------------------------
// --mips: per-level LOD chunk generation cost, fine mip path (materialize +
// downsample every level-0 descendant, replicating the UE worker job:
// column-grid cache, level-0 source that always materializes, recursive
// downsampleBricks, [-1,B]^3 apron meshing of the 4x4x4 chunk bricks) vs the
// coarse path (GeneratedWorld::makeCoarseBrick at the level's own
// resolution). Cold caches per rep, min-of-N (--reps, default 5) per this
// box's measured ~15% run-to-run variance. Digests printed for both paths so
// runs are comparable; the coarse digest is additionally covered by
// test_coarsegen.cpp's pinned golden.
// ---------------------------------------------------------------------------

constexpr int kMipsBrickEdge = 8;   // matches the UE runtime (VoxelCoords::BrickEdgeVoxels)
constexpr int kMipsChunkBricks = 4; // 4x4x4 bricks per render chunk
constexpr int kMipsMaxLevel = 4;    // kNumLevels - 1

struct MipsPhases {
    double columnsMs = 0, fillMs = 0, downMs = 0, meshMs = 0, totalMs = 0;
    size_t l0Bricks = 0, mipBricks = 0;
    uint64_t digest = 0;
    size_t quads = 0;
};

// Fine path job state: FCachedMipBuilder-equivalent, cold, no shared cache.
struct MipsFineJob {
    static constexpr int B = kMipsBrickEdge;
    const GeneratedWorld<B>& gen;
    MipsPhases& ph;
    std::unordered_map<uint64_t, GeneratedWorld<B>::ColumnGrid> grids;
    std::unordered_map<BrickKey, Brick<B>, BrickKeyHash> l0;
    std::unordered_map<MipKey, Brick<B>, MipKeyHash> mips;

    MipsFineJob(const GeneratedWorld<B>& g, MipsPhases& p) : gen(g), ph(p) {}

    const Brick<B>* brick(int32_t level, const BrickKey& key) {
        if (level <= 0) {
            auto it = l0.find(key);
            if (it != l0.end()) return &it->second;
            const uint64_t gk =
                (uint64_t(uint32_t(key.x)) << 32) | uint64_t(uint32_t(key.y));
            auto git = grids.find(gk);
            if (git == grids.end()) {
                const auto t0 = Clock::now();
                git = grids.emplace(gk, gen.columns(key.x, key.y)).first;
                ph.columnsMs += msSince(t0);
            }
            const auto t1 = Clock::now();
            auto [it2, ins] = l0.emplace(key, gen.makeBrick(key, git->second));
            ph.fillMs += msSince(t1);
            ++ph.l0Bricks;
            return &it2->second;
        }
        const MipKey mk{level, key};
        auto it = mips.find(mk);
        if (it != mips.end()) return &it->second;
        const Brick<B>* children[8] = {};
        for (int cz = 0; cz < 2; ++cz)
            for (int cy = 0; cy < 2; ++cy)
                for (int cx = 0; cx < 2; ++cx)
                    children[cx + 2 * cy + 4 * cz] = brick(
                        level - 1, BrickKey{key.x * 2 + cx, key.y * 2 + cy, key.z * 2 + cz});
        const auto t0 = Clock::now();
        Brick<B> built = downsampleBricks<B>(children, 4);
        ph.downMs += msSince(t0);
        ++ph.mipBricks;
        return &mips.emplace(mk, std::move(built)).first->second;
    }
};

// Coarse path job state: same shape, but bricks come straight from
// makeCoarseBrick at the target level — no level-0 bricks, no downsampling.
struct MipsCoarseJob {
    static constexpr int B = kMipsBrickEdge;
    const GeneratedWorld<B>& gen;
    MipsPhases& ph;
    std::unordered_map<uint64_t, GeneratedWorld<B>::ColumnGrid> grids;
    std::unordered_map<BrickKey, Brick<B>, BrickKeyHash> bricks;

    MipsCoarseJob(const GeneratedWorld<B>& g, MipsPhases& p) : gen(g), ph(p) {}

    const Brick<B>* brick(int32_t level, const BrickKey& key) {
        auto it = bricks.find(key);
        if (it != bricks.end()) return &it->second;
        const uint64_t gk = (uint64_t(uint32_t(key.x)) << 32) | uint64_t(uint32_t(key.y));
        auto git = grids.find(gk);
        if (git == grids.end()) {
            const auto t0 = Clock::now();
            git = grids.emplace(gk, gen.coarseColumns(level, key.x, key.y)).first;
            ph.columnsMs += msSince(t0);
        }
        const auto t1 = Clock::now();
        auto [it2, ins] = bricks.emplace(key, gen.makeCoarseBrick(level, key, git->second));
        ph.fillMs += msSince(t1);
        ++ph.mipBricks;
        return &it2->second;
    }
};

// One cold level-L chunk build + mesh through `Job`; phases accumulated into
// the returned MipsPhases. Chunk (0, 0, ckz) where ckz holds the surface at
// the origin, matching where the real streaming jobs do their heavy work.
template <typename Job>
MipsPhases runMipsChunk(const GeneratedWorld<kMipsBrickEdge>& gen, int32_t level, int32_t ckz) {
    constexpr int B = kMipsBrickEdge;
    MipsPhases ph;
    Job job(gen, ph);
    Digest digest;
    std::vector<Quad> quads;
    const auto tAll = Clock::now();
    for (int dz = 0; dz < kMipsChunkBricks; ++dz)
        for (int dy = 0; dy < kMipsChunkBricks; ++dy)
            for (int dx = 0; dx < kMipsChunkBricks; ++dx) {
                const int64_t obx = dx, oby = dy;
                const int64_t obz = int64_t(ckz) * kMipsChunkBricks + dz;
                // Materialize the mesh's whole 27-brick neighborhood first so
                // generation cost lands in the generation phases, then time
                // the mesh separately over warm bricks.
                for (int nz = -1; nz <= 1; ++nz)
                    for (int ny = -1; ny <= 1; ++ny)
                        for (int nx = -1; nx <= 1; ++nx)
                            (void)job.brick(level,
                                            BrickKey{int32_t(obx + nx), int32_t(oby + ny),
                                                     int32_t(obz + nz)});
                const int64_t ovx = obx * B, ovy = oby * B, ovz = obz * B;
                const auto sampler = [&](int x, int y, int z) -> MaterialId {
                    const int64_t X = ovx + x, Y = ovy + y, Z = ovz + z;
                    const Brick<B>* b = job.brick(
                        level, BrickKey{int32_t(floorDiv(X, B)), int32_t(floorDiv(Y, B)),
                                        int32_t(floorDiv(Z, B))});
                    if (!b) return MAT_AIR;
                    return b->get(int(floorMod(X, B)), int(floorMod(Y, B)),
                                  int(floorMod(Z, B)));
                };
                const auto tM = Clock::now();
                quads.clear();
                meshBrick<B>(sampler, quads);
                ph.meshMs += msSince(tM);
                ph.quads += quads.size();
                digest.u32(static_cast<uint32_t>(obx));
                digest.u32(static_cast<uint32_t>(oby));
                digest.u32(static_cast<uint32_t>(obz));
                job.brick(level, BrickKey{int32_t(obx), int32_t(oby), int32_t(obz)})
                    ->digest(digest);
                digestQuads(quads, digest);
            }
    ph.totalMs = msSince(tAll);
    ph.digest = digest.h;
    return ph;
}

void runMipsBench(const Options& opt) {
    constexpr int B = kMipsBrickEdge;
    SyntheticTileSampler tiles(opt.seed);
    Amplifier amp(opt.seed, tiles);
    GeneratedWorld<B> gen(amp);
    const ColumnSample c0 = amp.column(0, 0);
    std::printf("mips bench: seed %llu (worldgen v%u), surface at origin %d mm, min of %d\n\n",
                (unsigned long long)opt.seed, kWorldGenVersion, c0.surfaceMm, opt.reps);
    std::printf("level |     fine total (columns |   l0fill | downsample | mesh) |"
                " coarse total (columns |  fill | mesh) | speedup\n");

    for (int32_t level = 0; level <= kMipsMaxLevel; ++level) {
        const int64_t chunkVox0 = int64_t(kMipsChunkBricks) * B << level;
        const int32_t ckz = static_cast<int32_t>(
            floorDiv(floorDiv(c0.surfaceMm, kVoxelSizeMm), chunkVox0));

        MipsPhases fine, coarse;
        fine.totalMs = 1e30;
        coarse.totalMs = 1e30;
        for (int r = 0; r < opt.reps; ++r) {
            const MipsPhases f = runMipsChunk<MipsFineJob>(gen, level, ckz);
            if (f.totalMs < fine.totalMs) fine = f;
            const MipsPhases c = runMipsChunk<MipsCoarseJob>(gen, level, ckz);
            if (c.totalMs < coarse.totalMs) coarse = c;
        }
        std::printf("  %d   | %11.1f ms (%7.1f | %8.1f | %10.1f | %4.1f) |"
                    " %9.1f ms (%7.2f | %5.2f | %4.1f) | %6.1fx\n",
                    level, fine.totalMs, fine.columnsMs, fine.fillMs, fine.downMs, fine.meshMs,
                    coarse.totalMs, coarse.columnsMs, coarse.fillMs, coarse.meshMs,
                    coarse.totalMs > 0 ? fine.totalMs / coarse.totalMs : 0.0);
        std::printf("      | fine: l0 bricks %zu, mip bricks %zu, quads %zu, digest %016llx\n",
                    fine.l0Bricks, fine.mipBricks, fine.quads,
                    (unsigned long long)fine.digest);
        std::printf("      | coarse: bricks %zu, quads %zu, digest %016llx\n",
                    coarse.mipBricks, coarse.quads, (unsigned long long)coarse.digest);
    }
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
        else if (a == "--mips") opt.mips = true;
        else if (a == "--reps" && i + 1 < argc) opt.reps = std::atoi(argv[++i]);
        else {
            std::fprintf(stderr,
                         "usage: vxc_bench [--radius m] [--seed n] [--brick 8|16] "
                         "[--digest] [--goldens] [--mips] [--reps n]\n");
            return 2;
        }
    }
    if (opt.goldens) {
        printGoldens(opt);
        return 0;
    }
    if (opt.mips) {
        runMipsBench(opt);
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
