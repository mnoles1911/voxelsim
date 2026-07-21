#include "voxelcore/amplifier.h"

#include "voxelcore/biome.h"

namespace vxc {
namespace {

// Detail octave table v1 (worldgen-versioned constant, docs/determinism.md).
// latticeMm chosen so octaves nest across brick sizes; amplitudes in mm before
// slope scaling.
struct Octave {
    int32_t latticeMm;
    int32_t amplitudeMm;
};
constexpr Octave kDetailOctaves[] = {
    {25600, 1800},
    {6400, 700},
    {1600, 260},
    {400, 100},
};

// Slope scale in q10 fixed point (1024 == 1.0): flat ground damps detail,
// steep ground amplifies it (scree/cliff roughness), clamped to [0.25, 4.0].
constexpr int64_t slopeScaleQ10(int64_t slopeMmPerPx) {
    return clampi64(512 + slopeMmPerPx / 24, 256, 4096);
}

} // namespace

ColumnSample Amplifier::column(int64_t vx, int64_t vy) const {
    const int64_t xMm = vx * kVoxelSizeMm;
    const int64_t yMm = vy * kVoxelSizeMm;
    const int64_t pxMm = tiles_->pixelSizeMm();

    // Bilinear base elevation from the tile raster (exact integer math).
    const int64_t px = floorDiv(xMm, pxMm), py = floorDiv(yMm, pxMm);
    const int64_t fx = xMm - px * pxMm, fy = yMm - py * pxMm;
    const int64_t e00 = tiles_->elevationMm(px, py);
    const int64_t e10 = tiles_->elevationMm(px + 1, py);
    const int64_t e01 = tiles_->elevationMm(px, py + 1);
    const int64_t e11 = tiles_->elevationMm(px + 1, py + 1);
    const int64_t gx = pxMm - fx, gy = pxMm - fy;
    const int64_t baseMm =
        ((e00 * gx + e10 * fx) * gy + (e01 * gx + e11 * fx) * fy) / (pxMm * pxMm);

    // Tile-level slope (mm of elevation change per pixel) conditions both
    // detail amplitude and soil depth.
    const int64_t slopeMmPerPx =
        (e10 > e00 ? e10 - e00 : e00 - e10) + (e01 > e00 ? e01 - e00 : e00 - e01);

    const int64_t sScale = slopeScaleQ10(slopeMmPerPx);
    int64_t detailMm = 0;
    for (uint32_t i = 0; i < sizeof(kDetailOctaves) / sizeof(kDetailOctaves[0]); ++i) {
        const Octave& o = kDetailOctaves[i];
        detailMm += valueNoise2(seed_, xMm, yMm, o.latticeMm, CH_DETAIL_OCTAVE_BASE + i) *
                    o.amplitudeMm / 32768;
    }
    detailMm = detailMm * sScale / 1024;

    const ClimateSample cl = tiles_->climate(px, py);

    ColumnSample col;
    col.surfaceMm = clampi32(baseMm + detailMm, -8'000'000, 9'000'000);

    // Topsoil deepens with precipitation, thins with slope; +/-25% hash jitter
    // breaks up contour-following layer boundaries.
    int64_t topsoil =
        clampi64(300 + static_cast<int64_t>(cl.precipitation) * 8 - slopeMmPerPx / 4, 0, 2500);
    const int64_t tj = hashToSigned16(hash2(seed_, vx >> 4, vy >> 4, CH_TOPSOIL_JITTER));
    topsoil += topsoil * tj / (4 * 32768);
    col.topsoilMm = static_cast<int32_t>(topsoil);

    col.subsoilMm = clampi32(topsoil * 2 + 500, 0, 6000);

    const uint64_t bj = hash2(seed_, vx >> 6, vy >> 6, CH_BEDROCK_JITTER);
    col.bedrockDepthMm = static_cast<int32_t>(40000 + ((bj >> 48) * 20000) / 65536);

    // Surface material from biome classification (M4): morphology gates
    // (slope, coastal band, temperature-adjusted treeline) run before the
    // Whittaker climate lookup — see voxelcore/biome.h, mirrored bit-exactly
    // in worldgen.hlsl's ColumnMain.
    const BiomeId biome = classifyBiome(cl.temperature, cl.precipitation, cl.seasonality,
                                         col.surfaceMm, slopeMmPerPx);
    col.surfaceMat = biomeSurfaceMaterial(biome, col.surfaceMm);

    // M4 cave pass (voxelcore/caves.h): reduce the jittered lattice tunnel
    // network to the tube axes that pass near this column. Depends only on
    // (seed, vx, vy, surfaceMm) — no raster reads — which is what lets
    // worldgen.hlsl recompute it inside VoxelizeMain rather than widening
    // GpuColumnSample. Mirrored bit-exactly there.
    col.cave = caveColumnFor(seed_, vx, vy, col.surfaceMm);
    return col;
}

MaterialId Amplifier::stratigraphyAt(const ColumnSample& col, int64_t vz) {
    const int64_t centreMm = vz * kVoxelSizeMm + kVoxelSizeMm / 2;
    const int64_t depthMm = static_cast<int64_t>(col.surfaceMm) - centreMm;
    if (depthMm < 0) return MAT_AIR;
    if (depthMm < col.topsoilMm) return col.surfaceMat;
    if (depthMm < col.topsoilMm + col.subsoilMm)
        return col.surfaceMat == MAT_SAND ? MAT_GRAVEL : MAT_SUBSOIL;
    if (depthMm < col.bedrockDepthMm) return MAT_ROCK;
    return MAT_BEDROCK;
}

MaterialId Amplifier::materialAt(const ColumnSample& col, int64_t vz) {
    const MaterialId m = stratigraphyAt(col, vz);
    // Already void, or the unbounded bedrock floor: the cave pass never
    // touches either. Refusing MAT_BEDROCK here is the third and last of the
    // independent bedrock guards (caves.h documents the other two) — even a
    // mis-tuned constant table cannot punch a hole in the world's floor.
    if (m == MAT_AIR || m == MAT_BEDROCK) return m;
    // MAT_AIR is an enumerator and `m` is a MaterialId variable, so a bare
    // `cond ? MAT_AIR : m` mixes an enumerated and a non-enumerated operand —
    // gcc's -Wextra rejects that (clang does not), so name the type explicitly.
    return caveCarveAt(col.cave, col.surfaceMm, col.bedrockDepthMm, vz)
               ? static_cast<MaterialId>(MAT_AIR)
               : m;
}

} // namespace vxc
