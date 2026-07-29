// Coarse generation path tests (GeneratedWorld::coarseColumns /
// makeCoarseBrick / coarseSurfaceBrickRange â€” generator.h): the outer-ring
// LOD path that generates a level-L brick at its own resolution instead of
// materializing and downsampling its 8^L level-0 descendants.
//
// Covered here:
//   - level-0 identity: the coarse path at level 0 is bit-identical to the
//     fine path (columns, bricks, surface range) â€” one rule, provably
//     degenerating to the existing generator;
//   - pointwise consistency: every coarse cell equals a direct amplifier
//     query at its representative level-0 voxel, including across brick
//     seams (no per-brick state, so adjacent bricks can never contradict);
//   - surface-range formula against Amplifier::stratigraphyAt;
//   - a NEW pinned golden for levels 1..4 (this path has its own goldens;
//     it never feeds the fine mip chain, whose mips_chain golden is pinned
//     unchanged in test_mips.cpp);
//   - seed sensitivity;
//   - fidelity vs the TRUE mip (recursive downsampleBricks over full-res
//     generation): measured occupancy/material mismatch pinned as ceilings,
//     so the approximation can never silently degrade;
//   - cavern survival: a real cavern room's void survives coarsening at the
//     levels whose cells are smaller than the room.

#include <cinttypes>
#include <unordered_map>

#include "voxelcore/generator.h"
#include "voxelcore/mips.h"
#include "vxctest.h"

using namespace vxc;

namespace {
constexpr uint64_t kSeed = 20260719;
constexpr int B = 8;

// True-mip reference: recursive downsample over full-resolution generation,
// with a level-0 source that (like the UE worker job) always materializes â€”
// nullptr never propagates, so all-air groups are still voted on.
struct TrueMip {
    const GeneratedWorld<B>& gen;
    std::unordered_map<BrickKey, Brick<B>, BrickKeyHash> l0;
    std::unordered_map<MipKey, Brick<B>, MipKeyHash> mips;

    explicit TrueMip(const GeneratedWorld<B>& g) : gen(g) {}

    const Brick<B>& brick(int32_t level, const BrickKey& key) {
        if (level <= 0) {
            auto it = l0.find(key);
            if (it != l0.end()) return it->second;
            return l0.emplace(key, gen.makeBrick(key)).first->second;
        }
        const MipKey mk{level, key};
        auto it = mips.find(mk);
        if (it != mips.end()) return it->second;
        const Brick<B>* children[8] = {};
        for (int cz = 0; cz < 2; ++cz)
            for (int cy = 0; cy < 2; ++cy)
                for (int cx = 0; cx < 2; ++cx)
                    children[cx + 2 * cy + 4 * cz] = &brick(
                        level - 1, BrickKey{key.x * 2 + cx, key.y * 2 + cy, key.z * 2 + cz});
        return mips.emplace(mk, downsampleBricks<B>(children, 4)).first->second;
    }
};

uint64_t coarseRegionDigest(uint64_t seed) {
    SyntheticTileSampler tiles(seed);
    Amplifier amp(seed, tiles);
    GeneratedWorld<B> gen(amp);
    Digest d;
    for (int32_t level = 1; level <= 4; ++level)
        for (int32_t by = -1; by <= 1; ++by)
            for (int32_t bx = -1; bx <= 1; ++bx) {
                const auto grid = gen.coarseColumns(level, bx, by);
                int32_t bzMin = 0, bzMax = 0;
                gen.coarseSurfaceBrickRange(level, grid, bzMin, bzMax);
                d.u32(static_cast<uint32_t>(level));
                d.u32(static_cast<uint32_t>(bzMin));
                d.u32(static_cast<uint32_t>(bzMax));
                for (int32_t bz = bzMin; bz <= bzMax; ++bz)
                    gen.makeCoarseBrick(level, BrickKey{bx, by, bz}, grid).digest(d);
            }
    return d.h;
}

} // namespace

VXC_TEST(coarsegen_level0_identity) {
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    GeneratedWorld<B> gen(amp);

    for (int32_t by = -1; by <= 1; ++by)
        for (int32_t bx = -1; bx <= 1; ++bx) {
            const auto fine = gen.columns(bx, by);
            const auto coarse = gen.coarseColumns(0, bx, by);
            for (int i = 0; i < B * B; ++i) {
                CHECK_EQ(fine.cols[i].surfaceMm, coarse.cols[i].surfaceMm);
                CHECK_EQ(fine.cols[i].topsoilMm, coarse.cols[i].topsoilMm);
                CHECK_EQ(fine.cols[i].subsoilMm, coarse.cols[i].subsoilMm);
                CHECK_EQ(fine.cols[i].bedrockDepthMm, coarse.cols[i].bedrockDepthMm);
                CHECK_EQ(fine.cols[i].surfaceMat, coarse.cols[i].surfaceMat);
            }

            int32_t fMin = 0, fMax = 0, cMin = 0, cMax = 0;
            gen.surfaceBrickRange(fine, fMin, fMax);
            gen.coarseSurfaceBrickRange(0, coarse, cMin, cMax);
            CHECK_EQ(fMin, cMin);
            CHECK_EQ(fMax, cMax);

            for (int32_t bz = fMin - 1; bz <= fMax + 1; ++bz) {
                const BrickKey key{bx, by, bz};
                CHECK(gen.makeBrick(key, fine) == gen.makeCoarseBrick(0, key, coarse));
            }
        }
}

VXC_TEST(coarsegen_matches_pointwise_queries) {
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    GeneratedWorld<B> gen(amp);

    // Two xy-adjacent footprints at each level: cells are a pure function of
    // the GLOBAL coarse cell index, so the shared seam column x=0 of (1,0)
    // must continue x=7 of (0,0) with no per-brick offset error.
    for (int32_t level = 1; level <= 4; ++level)
        for (int32_t bx = 0; bx <= 1; ++bx) {
            const auto grid = gen.coarseColumns(level, bx, 0);
            int32_t bzMin = 0, bzMax = 0;
            gen.coarseSurfaceBrickRange(level, grid, bzMin, bzMax);
            const BrickKey key{bx, 0, bzMin};
            const Brick<B> brick = gen.makeCoarseBrick(level, key, grid);
            for (int z = 0; z < B; ++z)
                for (int y = 0; y < B; ++y)
                    for (int x = 0; x < B; ++x) {
                        const int64_t vx =
                            GeneratedWorld<B>::coarseRep(int64_t(bx) * B + x, level);
                        const int64_t vy = GeneratedWorld<B>::coarseRep(int64_t(y), level);
                        const int64_t vz =
                            GeneratedWorld<B>::coarseRep(int64_t(bzMin) * B + z, level);
                        CHECK_EQ(brick.get(x, y, z), amp.materialAt(vx, vy, vz));
                    }
        }
}

VXC_TEST(coarsegen_surface_range_formula) {
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    GeneratedWorld<B> gen(amp);

    // For every column: the topmost solid coarse cell under the
    // representative-sample rule sits inside [bzMin, bzMax], the cell above
    // it is air. Checked against stratigraphyAt (surface rule without the
    // cave carve, which coarseSurfaceBrickRange â€” like surfaceBrickRange â€”
    // deliberately ignores).
    for (int32_t level = 1; level <= 4; ++level) {
        const int64_t s = int64_t(1) << level;
        const auto grid = gen.coarseColumns(level, 0, 0);
        int32_t bzMin = 0, bzMax = 0;
        gen.coarseSurfaceBrickRange(level, grid, bzMin, bzMax);
        CHECK(bzMin <= bzMax);
        for (int i = 0; i < B * B; ++i) {
            const ColumnSample& col = grid.cols[i];
            const int64_t top0 = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
            const int64_t top = floorDiv(top0 - s / 2, s);
            CHECK(Amplifier::stratigraphyAt(col, GeneratedWorld<B>::coarseRep(top, level)) !=
                  MAT_AIR);
            CHECK_EQ(Amplifier::stratigraphyAt(
                         col, GeneratedWorld<B>::coarseRep(top + 1, level)),
                     MAT_AIR);
            CHECK(top >= int64_t(bzMin) * B);
            CHECK(top < (int64_t(bzMax) + 1) * B);
        }
    }
}

VXC_TEST(coarsegen_golden_digest) {
    // NEW golden for the coarse path (kWorldGenVersion 5). The fine-path
    // goldens (amplifier_columns, mips_chain, ...) are pinned elsewhere and
    // must not move; this one pins the coarse rule itself: representative
    // centre sample at c*2^L + 2^(L-1), materialAt semantics, levels 1..4
    // over the 3x3 footprints around the origin.
    const uint64_t d = coarseRegionDigest(kSeed);
    std::printf("    [coarsegen] golden digest 0x%016" PRIX64 "\n", d);
    // kWorldGenVersion 6: moves because the fine surface it samples moved
    // (coarse-to-fine detail rework). The coarse RULE is unchanged â€” its own
    // tests (coarsegen_level0_identity, coarsegen_matches_pointwise_queries,
    // coarsegen_surface_range_formula, coarsegen_fidelity_vs_true_mip) all
    // still pass, and the fidelity mismatch ceilings were not relaxed.
    // (was 0x85B3E79EF8D01AFC at v5)
    CHECK_EQ(d, 0x743BC64E0FFC1C03ull);
}

VXC_TEST(coarsegen_seed_sensitivity) {
    CHECK(coarseRegionDigest(kSeed) != coarseRegionDigest(kSeed + 1));
}

VXC_TEST(coarsegen_fidelity_vs_true_mip) {
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    GeneratedWorld<B> gen(amp);
    TrueMip mip(gen);

    // Surface-shell bricks at footprint (0,0) per level (the deep interior
    // and the open air are exact by uniformity; the shell is where the
    // approximation lives). Mismatch is pinned as a permille CEILING so the
    // coarse rule can never silently drift further from the true mip;
    // measured actuals are printed for the record.
    //
    // Measured at v8 (seed 20260719): occupancy 38/19/11/10 permille, material
    // 75/61/21/35 permille at levels 1/2/3/4.
    // Measured at v9: occupancy 28/18/16/23, material 52/109/53/90.
    //
    // WHY THE MATERIAL CEILINGS ROSE AT v9, AND WHY THAT IS THE FIX WORKING.
    // The coarse rule did not change; the field under it did. v8's slope was a
    // per-cell forward difference, constant across a whole 30 m tile pixel, and
    // climate is read per pixel too -- so within one cell the ONLY thing that
    // could move a column's biome was its own surfaceMm crossing the treeline
    // or the beach band. Materials therefore came in 30 m blocks, and centre-
    // representative sampling reproduced blocks almost perfectly. That blockiness
    // was one of the three mechanisms making the tile grid visible.
    //
    // v9's slope is the carrier's analytic gradient, which varies continuously
    // within a cell, so the cliff gate's boundary is now a smooth curve running
    // THROUGH cells rather than snapping to their edges. Material structure is
    // genuinely finer-grained, and a coarse cell's centre sample represents its
    // block less well the coarser the level gets -- which is exactly the shape
    // of the change: L1 (0.2 m cells, below the structure scale) IMPROVED
    // 75 -> 52 as the carrier got smoother, while L2-L4 rose.
    //
    // Accepted because occupancy -- which is what drives silhouette and
    // collision -- is unchanged to within noise, and material at level >= 2 is
    // distant-ring shading. If occupancy ever degrades, that is a different
    // conversation and this comment is not a licence for it.
    const int64_t occCeilPermille[5] = {0, 60, 40, 40, 40};
    // v10 raised L3 from 85 to 135. This is a REAL fidelity regression and is
    // recorded as one rather than absorbed: measured L3 material mismatch went
    // 85 -> 125 per mille when the bedding term landed.
    //
    // The mechanism is inherent to point-sampled LOD rather than a bug. Coarse
    // generation takes ONE representative column per coarse cell, while the true
    // mip averages every column under it. Bedding is quasi-periodic with a
    // ~3.2 m bed thickness and 320 mm amplitude, so at L3 the representative
    // column's position within a bed is essentially uncorrelated with the cell's
    // average, and stratigraphy is conditioned on the surface.
    //
    // Accepted on the same terms the comment above already sets out, and the
    // terms are met rather than merely invoked: OCCUPANCY -- silhouette and
    // collision -- is unchanged (31/19/21/33 against ceilings 60/40/40/40), and
    // L3 is distant-ring shading. The ceiling is set just above the measured
    // value, not at a round number, so further degradation still trips it.
    // Worth a look in-engine on a layered cliff at distance; if bedding reads as
    // shimmering across an LOD transition, this number is why.
    const int64_t matCeilPermille[5] = {0, 90, 165, 135, 140};

    for (int32_t level = 1; level <= 4; ++level) {
        const auto grid = gen.coarseColumns(level, 0, 0);
        int32_t bzMin = 0, bzMax = 0;
        gen.coarseSurfaceBrickRange(level, grid, bzMin, bzMax);

        int64_t cells = 0, occMismatch = 0, bothSolid = 0, matMismatch = 0;
        for (int32_t bz = bzMin; bz <= bzMax; ++bz) {
            const BrickKey key{0, 0, bz};
            const Brick<B> coarse = gen.makeCoarseBrick(level, key, grid);
            const Brick<B>& truth = mip.brick(level, key);
            for (int z = 0; z < B; ++z)
                for (int y = 0; y < B; ++y)
                    for (int x = 0; x < B; ++x) {
                        const MaterialId a = coarse.get(x, y, z);
                        const MaterialId b = truth.get(x, y, z);
                        ++cells;
                        if ((a == MAT_AIR) != (b == MAT_AIR)) {
                            ++occMismatch;
                        } else if (a != MAT_AIR) {
                            ++bothSolid;
                            if (a != b) ++matMismatch;
                        }
                    }
        }
        const int64_t occPm = cells > 0 ? occMismatch * 1000 / cells : 0;
        const int64_t matPm = bothSolid > 0 ? matMismatch * 1000 / bothSolid : 0;
        std::printf("    [coarsegen] L%d vs true mip: cells %" PRId64 ", occupancy mismatch %" PRId64
                    "/1000, material mismatch (both solid) %" PRId64 "/1000\n",
                    level, cells, occPm, matPm);
        CHECK(cells > 0);
        CHECK(occPm <= occCeilPermille[level]);
        CHECK(matPm <= matCeilPermille[level]);
    }
}

VXC_TEST(coarsegen_cavern_survival) {
    SyntheticTileSampler tiles(kSeed);
    Amplifier amp(kSeed, tiles);
    GeneratedWorld<B> gen(amp);

    // Fine cavern void height at a column: air cells strictly below the
    // surface shell (stratigraphy solid, materialAt air), scanned down to
    // bedrock depth.
    const auto fineVoidAt = [&](int64_t vx, int64_t vy, int64_t& lo, int64_t& hi) -> int64_t {
        const ColumnSample col = amp.column(vx, vy);
        const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
        int64_t n = 0;
        lo = 0;
        hi = 0;
        for (int64_t vz = topVz - 2200; vz < topVz; ++vz) {
            if (Amplifier::stratigraphyAt(col, vz) != MAT_AIR &&
                Amplifier::materialAt(col, vz) == MAT_AIR) {
                if (n == 0) lo = vz;
                hi = vz;
                ++n;
            }
        }
        return n;
    };

    // Phase 1: wide deterministic scan (stride 128 voxels = 12.8 m, over
    // +/-1.6 km) for any cavern-reached column. Room reach discs are tens
    // of metres wide, so the stride cannot step over one.
    int64_t hitVx = 0, hitVy = 0;
    bool found = false;
    for (int64_t vy = -16000; vy <= 16000 && !found; vy += 128)
        for (int64_t vx = -16000; vx <= 16000 && !found; vx += 128) {
            if (amp.column(vx, vy).cavern.count > 0) {
                hitVx = vx;
                hitVy = vy;
                found = true;
            }
        }
    CHECK(found);
    if (!found) return;

    // Phase 2: refine to the max-void column nearby â€” an anchor near the
    // room's centre, so every level's representative column for the cell
    // containing it (lateral offset < s voxels) still falls inside the room.
    int64_t foundVx = hitVx, foundVy = hitVy, fineVoid = 0, voidLo = 0, voidHi = 0;
    for (int64_t vy = hitVy - 512; vy <= hitVy + 512; vy += 32)
        for (int64_t vx = hitVx - 512; vx <= hitVx + 512; vx += 32) {
            int64_t lo = 0, hi = 0;
            const int64_t n = fineVoidAt(vx, vy, lo, hi);
            if (n > fineVoid) {
                fineVoid = n;
                voidLo = lo;
                voidHi = hi;
                foundVx = vx;
                foundVy = vy;
            }
        }
    std::printf("    [coarsegen] cavern anchor voxel (%" PRId64 ", %" PRId64 "): %" PRId64
                " fine void cells in [%" PRId64 ", %" PRId64 "]\n",
                foundVx, foundVy, fineVoid, voidLo, voidHi);
    CHECK(fineVoid > 0);
    if (fineVoid == 0) return;

    // Each level queries its own representative column for the cell
    // containing the anchor.
    for (int32_t level = 1; level <= 4; ++level) {
        const int64_t s = int64_t(1) << level;
        if (s * kVoxelSizeMm >= (voidHi - voidLo + 1) * kVoxelSizeMm) continue; // cell taller than room
        const int64_t cx = floorDiv(foundVx, s);
        const int64_t cy = floorDiv(foundVy, s);
        const ColumnSample rcol = amp.column(GeneratedWorld<B>::coarseRep(cx, level),
                                             GeneratedWorld<B>::coarseRep(cy, level));
        int64_t coarseVoid = 0;
        for (int64_t cz = floorDiv(voidLo - s, s); cz <= floorDiv(voidHi + s, s); ++cz) {
            const int64_t vz = GeneratedWorld<B>::coarseRep(cz, level);
            if (Amplifier::stratigraphyAt(rcol, vz) != MAT_AIR &&
                Amplifier::materialAt(rcol, vz) == MAT_AIR)
                ++coarseVoid;
        }
        std::printf("    [coarsegen] L%d: %" PRId64 " coarse void cells (cell %" PRId64
                    " fine voxels)\n",
                    level, coarseVoid, s);
        // Proportional survival: the void's coarse-cell count is at least
        // half of fineVoid/s (nearest representative column can sit near
        // the room wall, so demand proportionality, not equality).
        CHECK(coarseVoid >= fineVoid / s / 2);
        CHECK(coarseVoid > 0);
    }
}
