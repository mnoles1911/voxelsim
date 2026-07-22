// Worldgen GPU kernel v1 — HLSL port of the CPU reference (hash.h +
// amplifier.cpp), per ADR-0001: single HLSL source compiled to DXIL for UE
// RDG and to SPIR-V (DXC -spirv) for the headless Vulkan determinism/perf
// harness.
//
// DETERMINISM CONTRACT (docs/determinism.md): every expression here must be
// a bit-exact mirror of the CPU reference. Integer-only; int64/uint64
// arithmetic requires Int64 shader ops (D3D12 Int64ShaderOps / Vulkan
// shaderInt64 — see ADR-0001). C++ and HLSL agree on two's-complement casts
// and arithmetic >> on signed values. They DO NOT reliably agree on signed
// `/` and `%` when the operands have mixed signs — that is undefined in HLSL
// and diverged between AMD and NVIDIA at the M0 gate; see the floorDiv /
// truncDiv comment below and use those helpers instead.
// Any change here that alters output is world-breaking: bump
// vxc::kWorldGenVersion and regenerate goldens, never patch silently.
//
// Kernels:
//   ColumnMain   — one thread per column, computes the full ColumnSample
//                  stratigraphy (mirrors vxc::Amplifier::column).
//   VoxelizeMain — one thread per column, chained after ColumnMain: reads
//                  that column's GpuColumnSample (NOT recomputed) and fills
//                  a vertical stack of brick-major cell buckets (mirrors
//                  vxc::Amplifier::materialAt / GeneratedWorld<8>::makeBrick).
// Greedy meshing kernels build on this in later ports.

// --- constants mirrored from voxel-core (compile checks in harness) --------

static const int kVoxelSizeMm = 100;

// HashChannel (hash.h) — append only, never renumber.
static const uint CH_DETAIL_OCTAVE_BASE = 0;
static const uint CH_TOPSOIL_JITTER = 16;
static const uint CH_BEDROCK_JITTER = 17;

// Material ids (core.h) — append only, never renumber.
static const uint MAT_AIR = 0;
static const uint MAT_BEDROCK = 1;
static const uint MAT_ROCK = 2;
static const uint MAT_GRAVEL = 3;
static const uint MAT_SAND = 4;
static const uint MAT_SUBSOIL = 5;
static const uint MAT_TOPSOIL = 6;
static const uint MAT_SNOW = 7;
static const uint MAT_GRASS = 8;
static const uint MAT_JUNGLE_SOIL = 9;
static const uint MAT_SAVANNA_GRASS = 10;
static const uint MAT_PODZOL = 11;
static const uint MAT_PERMAFROST = 12;
static const uint MAT_MUD = 13;
static const uint MAT_CLAY = 14;

// BiomeId (voxelcore/biome.h) — append only, never renumber.
static const uint BIOME_OCEAN = 0;
static const uint BIOME_BEACH = 1;
static const uint BIOME_GRASSLAND = 2;
static const uint BIOME_TEMPERATE_FOREST = 3;
static const uint BIOME_RAINFOREST = 4;
static const uint BIOME_DESERT = 5;
static const uint BIOME_SAVANNA = 6;
static const uint BIOME_TAIGA = 7;
static const uint BIOME_TUNDRA_ALPINE = 8;

// Biome gate/Whittaker thresholds (voxelcore/biome.h) — bit-exact mirror,
// worldgen contract.
static const int64_t kBiomeCliffSlopeMmPerPx = 6000;
static const int kBiomeBeachLowerMm = -3000;
static const int kBiomeBeachUpperMm = 4000;
static const int kBiomeTreelineBaseMm = 2600000;
static const int kBiomeTreelineMmPerTempUnit = 20000;
static const int kBiomeAlpineRockLineMm = 3200000;
static const int kBiomeTempColdU8 = 70;
static const int kBiomeTempWarmU8 = 140;
static const int kBiomeTempHotU8 = 170;
static const int kBiomePrecipAridU8 = 60;
static const int kBiomePrecipSemiU8 = 100;
static const int kBiomePrecipModU8 = 170;
static const int kBiomeSeasonalHighU8 = 128;

// Detail octave table v1 (amplifier.cpp kDetailOctaves).
static const int kOctaveCount = 4;
static const int kOctaveLatticeMm[kOctaveCount] = {25600, 6400, 1600, 400};
static const int kOctaveAmplitudeMm[kOctaveCount] = {1800, 700, 260, 100};

// --- primitives mirrored from core.h / hash.h ------------------------------

// VENDOR-DIVERGENCE FIX (M0 gate, 2026-07: AMD RX 7800 XT PASS / NVIDIA RTX
// 4090 FAIL on identical SPIR-V). Signed integer division and remainder with
// operands of MIXED SIGN are not portable in this stack:
//   * HLSL defines `%` only when both operands share a sign (D3D HLSL
//     operator reference); GLSL says the same for `%` and for `/`.
//   * DXC lowers them to OpSDiv/OpSRem, and the 64-bit integer emulation the
//     driver substitutes is where the vendors part ways. On AMD, OpSRem
//     returned a remainder with the sign of the DIVIDEND (matching x86/C++),
//     so the flooring correction `(r < 0) != (b < 0)` fired for negative
//     coordinates. On NVIDIA it did not fire, and floorDiv silently degraded
//     to TRUNCATING division for every column with a negative world
//     coordinate — wrong raster pixel, wrong noise lattice cell, metre-scale
//     surfaceMm/topsoilMm/subsoilMm divergence.
//
// Everything below therefore routes signed division through MAGNITUDE-ONLY
// unsigned division (OpUDiv), which has exactly one legal result on every
// implementation, and reconstructs the sign explicitly. No OpSRem, no
// sign-dependent rounding, anywhere in the worldgen math. Unsigned negation
// wraps by definition, so these are also correct at INT64_MIN.
// DO NOT reintroduce bare `/` or `%` on a possibly-negative signed value:
// use floorDiv (floor, world coord -> cell index) or truncDiv (toward zero,
// mirrors C++ `/`) so the CPU reference and the GPU agree by construction.

uint64_t absToU64(int64_t v)
{
    const uint64_t u = (uint64_t)v;
    // lint-shader-ub: allow UNSIGNED_UNDERFLOW - deliberate two's-complement negation of a magnitude; unsigned wraparound is well-defined and is exactly what makes this correct at INT64_MIN
    return (v < 0) ? ((uint64_t)0 - u) : u;
}

// Truncating division — bit-exact mirror of C++ `/` on int64_t.
int64_t truncDiv(int64_t a, int64_t b)
{
    const uint64_t uq = absToU64(a) / absToU64(b);
    // lint-shader-ub: allow UNSIGNED_UNDERFLOW - deliberate negation of an unsigned magnitude to rebuild the signed quotient; unsigned wraparound is well-defined
    return ((a < 0) != (b < 0)) ? (int64_t)((uint64_t)0 - uq) : (int64_t)uq;
}

// Floored division — bit-exact mirror of vxc::floorDiv (core.h).
int64_t floorDiv(int64_t a, int64_t b)
{
    const uint64_t ua = absToU64(a);
    const uint64_t ub = absToU64(b);
    const uint64_t uq = ua / ub;
    if ((a < 0) != (b < 0))
    {
        // Negative quotient: round away from zero unless the division was
        // exact. `uq * ub == ua` is the exactness test (no OpUMod needed).
        const uint64_t bump = (uq * ub == ua) ? (uint64_t)0 : (uint64_t)1;
        // lint-shader-ub: allow UNSIGNED_UNDERFLOW - deliberate negation of an unsigned magnitude, then a 0-or-1 rounding bump; unsigned wraparound is well-defined and is the intended two's-complement result
        return (int64_t)((uint64_t)0 - uq - bump);
    }
    return (int64_t)uq;
}

int64_t clamp64(int64_t v, int64_t lo, int64_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

uint64_t splitmix64(uint64_t z)
{
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

uint64_t hash2(uint64_t seed, int64_t x, int64_t y, uint channel)
{
    return splitmix64(seed ^ splitmix64((uint64_t)x ^
                             splitmix64((uint64_t)y ^
                             splitmix64((uint64_t)channel))));
}

// vxc::hash3 (hash.h) bit-for-bit — one more splitmix64 round than hash2.
// Used by the cavern pass's per-room geometry channel (CH_CAVERN_SITE), which
// needs the room index as a third coordinate.
uint64_t hash3(uint64_t seed, int64_t x, int64_t y, int64_t z, uint channel)
{
    return splitmix64(seed ^ splitmix64((uint64_t)x ^
                             splitmix64((uint64_t)y ^
                             splitmix64((uint64_t)z ^
                             splitmix64((uint64_t)channel)))));
}

// Top 16 bits -> [-32768, 32767].
int hashToSigned16(uint64_t h)
{
    // lint-shader-ub: allow UNSIGNED_UNDERFLOW - recentring a full 16-bit unsigned range onto [-32768, 32767]; the subtraction happens in signed 32-bit after the (int) cast, so it cannot wrap, and a negative result is the whole point
    return (int)(uint)(h >> 48) - 32768;
}

int64_t valueNoise2(uint64_t seed, int64_t xMm, int64_t yMm, int64_t latticeMm, uint channel)
{
    const int64_t x0 = floorDiv(xMm, latticeMm);
    const int64_t y0 = floorDiv(yMm, latticeMm);
    const int64_t fx = xMm - x0 * latticeMm;
    const int64_t fy = yMm - y0 * latticeMm;
    const int64_t v00 = hashToSigned16(hash2(seed, x0, y0, channel));
    const int64_t v10 = hashToSigned16(hash2(seed, x0 + 1, y0, channel));
    const int64_t v01 = hashToSigned16(hash2(seed, x0, y0 + 1, channel));
    const int64_t v11 = hashToSigned16(hash2(seed, x0 + 1, y0 + 1, channel));
    const int64_t gx = latticeMm - fx;
    const int64_t gy = latticeMm - fy;
    // Numerator is routinely negative (v* in [-32768, 32767]) — truncDiv, not
    // `/`, per the floorDiv comment above.
    return truncDiv((v00 * gx + v10 * fx) * gy + (v01 * gx + v11 * fx) * fy,
                    latticeMm * latticeMm);
}

int64_t slopeScaleQ10(int64_t slopeMmPerPx)
{
    // slopeMmPerPx is a sum of absolute values (>= 0), so both roundings agree
    // today; truncDiv keeps that from becoming load-bearing.
    return clamp64(512 + truncDiv(slopeMmPerPx, 24), 256, 4096);
}

// --- biome classification (voxelcore/biome.h) -------------------------------
// Bit-exact mirror of vxc::classifyBiome / vxc::biomeTreelineMm /
// vxc::biomeSurfaceMaterial. See biome.h for the doc comments explaining the
// gate order and every threshold; DO NOT let this drift from that file.

int biomeTreelineMm(int tempU8)
{
    const int64_t adjusted = (int64_t)kBiomeTreelineBaseMm +
                              ((int64_t)tempU8 - 128) * (int64_t)kBiomeTreelineMmPerTempUnit;
    return (int)clamp64(adjusted, (int64_t)kBiomeBeachUpperMm, (int64_t)9000000);
}

uint classifyBiome(int tempU8, int precipU8, int seasonalityU8, int surfaceMm,
                    int64_t slopeMmPerPx)
{
    if (slopeMmPerPx > kBiomeCliffSlopeMmPerPx) return BIOME_TUNDRA_ALPINE;
    if (surfaceMm < kBiomeBeachLowerMm) return BIOME_OCEAN;
    if (surfaceMm <= kBiomeBeachUpperMm) return BIOME_BEACH;
    if (surfaceMm > biomeTreelineMm(tempU8)) return BIOME_TUNDRA_ALPINE;

    if (tempU8 < kBiomeTempColdU8) return BIOME_TAIGA;

    const bool seasonal = seasonalityU8 >= kBiomeSeasonalHighU8;
    const bool warm = tempU8 >= kBiomeTempWarmU8;
    const bool hot = tempU8 >= kBiomeTempHotU8;

    if (precipU8 < kBiomePrecipAridU8) return hot ? BIOME_DESERT : BIOME_GRASSLAND;
    if (precipU8 < kBiomePrecipSemiU8) return (warm && seasonal) ? BIOME_SAVANNA : BIOME_GRASSLAND;
    if (precipU8 < kBiomePrecipModU8)
        return (warm && seasonal) ? BIOME_SAVANNA : BIOME_TEMPERATE_FOREST;
    return warm ? BIOME_RAINFOREST : BIOME_TEMPERATE_FOREST; // wet band
}

uint biomeSurfaceMaterial(uint biome, int surfaceMm)
{
    if (biome == BIOME_OCEAN) return MAT_MUD;
    if (biome == BIOME_BEACH) return MAT_SAND;
    if (biome == BIOME_GRASSLAND) return MAT_GRASS;
    if (biome == BIOME_TEMPERATE_FOREST) return MAT_TOPSOIL;
    if (biome == BIOME_RAINFOREST) return MAT_JUNGLE_SOIL;
    if (biome == BIOME_DESERT) return MAT_SAND;
    if (biome == BIOME_SAVANNA) return MAT_SAVANNA_GRASS;
    if (biome == BIOME_TAIGA) return MAT_PODZOL;
    // BIOME_TUNDRA_ALPINE (default) — see biome.h's biomeSurfaceMaterial doc
    // comment for the elevation-based rock/permafrost split rationale.
    return surfaceMm > kBiomeAlpineRockLineMm ? MAT_ROCK : MAT_PERMAFROST;
}

// --- kernel I/O ------------------------------------------------------------

struct GpuColumnSample
{
    int surfaceMm;
    int topsoilMm;
    int subsoilMm;
    int bedrockDepthMm;
    uint surfaceMat;
};

// One cbuffer shared by both kernels (documented per-field below) rather
// than a second one: it stays a single binding, and the fields ColumnMain
// doesn't use (BrickZMin/BricksZ) or VoxelizeMain doesn't use (RasterOriginPx/
// RasterSize/PixelSizeMm) simply go unread by that kernel's entry point.
cbuffer WorldGenParams : register(b0)
{
    uint2 DispatchColumns;  // columns along x, y for this dispatch (both kernels)
    int2 RasterOriginPx;    // tile-pixel coordinate of raster window texel (0,0) (ColumnMain)
    uint2 RasterSize;       // raster window extent in pixels (ColumnMain)
    int PixelSizeMm;        // 30000 (scale 1) or 11250 (scale 8) (ColumnMain)
    uint SeedLo;            // uint64 seed split across two 32-bit values (ColumnMain)
    uint SeedHi;
    int OriginVx;           // world voxel coord of dispatch column (0,0);
    int OriginVy;           // int32 spans +/-214,000 km at 10cm — Earth fits
    int BrickZMin;          // (VoxelizeMain) lowest brick z-index of the stack:
                             // covers voxel z in [BrickZMin*8, (BrickZMin+BricksZ)*8)
    uint BricksZ;           // (VoxelizeMain) number of bricks in the vertical stack
    uint ScanCount;         // (Scan*Main) number of mask entries to scan
};

// Raster window (elevation mm per pixel; climate packed t|s<<8|p<<16|v<<24).
// The harness/RDG pass MUST size the window to cover every pixel the
// dispatch bilinearly samples ([floorDiv(min xMm, px) .. floorDiv(max xMm,
// px)+1]); reads clamp to the window edge deterministically as a defensive
// backstop, but a clamped read means the caller sized the window wrong.
StructuredBuffer<int> ElevationMm : register(t0);
StructuredBuffer<uint> ClimatePacked : register(t1);

RWStructuredBuffer<GpuColumnSample> OutColumns : register(u0);

// VoxelizeMain-only bindings. InColumns is the SAME buffer OutColumns was
// written to by the chained ColumnMain dispatch — columns are read back, not
// recomputed. Registers t3/u2 are deliberately non-contiguous with t0/t1/u0
// so that after tools/compile-shaders.ps1's DXC -fvk-*-shift mapping
// (b->+0, t->+1, u->+3) EVERY resource declared in this file lands on a
// distinct Vulkan (set=0, binding) slot, regardless of whether a given
// kernel's compiled entry point ends up referencing it:
//   b0 WorldGenParams -> binding 0   (both kernels)
//   t0 ElevationMm    -> binding 1   (ColumnMain AND, since the C6 cavern
//                                     mirror, VoxelizeMain — cavernSiteFor
//                                     needs the surface at the SITE's xy)
//   t1 ClimatePacked  -> binding 2   (ColumnMain)
//   u0 OutColumns     -> binding 3   (ColumnMain)
//   t3 InColumns      -> binding 4   (VoxelizeMain)
//   u2 OutCells       -> binding 5   (VoxelizeMain)
// voxel-core/bench/gpu_harness.cpp hardcodes this same layout for the
// VoxelizeMain pipeline's descriptor set.
StructuredBuffer<GpuColumnSample> InColumns : register(t3);

// One uint per cell, material id 0-255 in the low byte (packing optimization
// left for later). Layout — see the VoxelizeMain doc comment below for the
// precise brick/cell indexing this buffer is written with.
RWStructuredBuffer<uint> OutCells : register(u2);

// HARDENING (2026-07 cross-vendor UB pass, issue 5). `RasterSize` is host-
// controlled and unvalidated in-shader. When RasterSize.x == 0 the clamp
// upper bound `(int64_t)RasterSize.x - 1` is -1 while the lower bound is 0,
// so clamp64 returns -1 for any px past the origin (`v > hi` wins), and the
// `(uint)` cast turns that into ~4 billion — a wild out-of-bounds load whose
// result is vendor-defined (AMD's range-checked descriptors return 0,
// NVIDIA's may return adjacent memory). An empty raster window carries no
// data to interpolate, so return the zero element deterministically instead.
// This is a GUARD, not a behavior change: for RasterSize.{x,y} >= 1 — every
// window the harness/RDG pass ever binds — the guard is never taken and the
// clamp is unchanged.
int rasterElevationMm(int64_t px, int64_t py)
{
    if (RasterSize.x == 0u || RasterSize.y == 0u) return 0;
    // lint-shader-ub: allow UNSIGNED_UNDERFLOW - the zero-extent early-out directly above proves RasterSize.x >= 1, so this clamp bound is >= 0 and the range stays well-ordered
    const int64_t lx = clamp64(px - RasterOriginPx.x, 0, (int64_t)RasterSize.x - 1);
    // lint-shader-ub: allow UNSIGNED_UNDERFLOW - same zero-extent early-out proves RasterSize.y >= 1
    const int64_t ly = clamp64(py - RasterOriginPx.y, 0, (int64_t)RasterSize.y - 1);
    return ElevationMm[(uint)(lx + ly * RasterSize.x)];
}

uint rasterClimate(int64_t px, int64_t py)
{
    if (RasterSize.x == 0u || RasterSize.y == 0u) return 0u;
    // lint-shader-ub: allow UNSIGNED_UNDERFLOW - the zero-extent early-out directly above proves RasterSize.x >= 1, so this clamp bound is >= 0 and the range stays well-ordered
    const int64_t lx = clamp64(px - RasterOriginPx.x, 0, (int64_t)RasterSize.x - 1);
    // lint-shader-ub: allow UNSIGNED_UNDERFLOW - same zero-extent early-out proves RasterSize.y >= 1
    const int64_t ly = clamp64(py - RasterOriginPx.y, 0, (int64_t)RasterSize.y - 1);
    return ClimatePacked[(uint)(lx + ly * RasterSize.x)];
}

// --- evalSurface: bit-exact mirror of vxc::Amplifier::evalSurface ----------
// The surface half of column() — bilinear tile base plus the slope-scaled
// detail octaves, and the two by-products (tile pixel, tile slope) the rest of
// ColumnMain needs.
//
// C4 factored this out on the CPU for ONE reason, and it is the reason it
// exists here too: the cavern pass anchors rooms at ABSOLUTE z, so
// `cavernSiteFor` needs the terrain height at the SITE's own xy rather than at
// the querying column's, and that height must be produced by the very same
// function the querying column's own surfaceMm came from. VoxelizeMain
// therefore calls this directly (see cavernSiteFor below) instead of
// GpuColumnSample being widened to carry a site surface — which it could not
// do anyway, since the site is not a column in the dispatch.
//
// CALLER CONTRACT: this reads ElevationMm, so any kernel that calls it must
// have the raster bound and must have already refused PixelSizeMm == 0.
struct SurfaceEval
{
    int64_t px, py;          // tile pixel of the bilinear base tap
    int64_t slopeMmPerPx;    // tile-level slope, conditions detail + soil depth
    int surfaceMm;
};

SurfaceEval evalSurface(uint64_t seed, int64_t vx, int64_t vy)
{
    const int64_t xMm = vx * kVoxelSizeMm;
    const int64_t yMm = vy * kVoxelSizeMm;
    const int64_t pxMm = (int64_t)PixelSizeMm;

    // Bilinear base elevation (exact integer math).
    const int64_t px = floorDiv(xMm, pxMm);
    const int64_t py = floorDiv(yMm, pxMm);
    const int64_t fx = xMm - px * pxMm;
    const int64_t fy = yMm - py * pxMm;
    const int64_t e00 = (int64_t)rasterElevationMm(px, py);
    const int64_t e10 = (int64_t)rasterElevationMm(px + 1, py);
    const int64_t e01 = (int64_t)rasterElevationMm(px, py + 1);
    const int64_t e11 = (int64_t)rasterElevationMm(px + 1, py + 1);
    const int64_t gx = pxMm - fx;
    const int64_t gy = pxMm - fy;
    // Elevations go negative (oceans) — truncDiv, not `/`.
    const int64_t baseMm = truncDiv(
        (e00 * gx + e10 * fx) * gy + (e01 * gx + e11 * fx) * fy, pxMm * pxMm);

    const int64_t slopeMmPerPx =
        (e10 > e00 ? e10 - e00 : e00 - e10) + (e01 > e00 ? e01 - e00 : e00 - e01);

    const int64_t sScale = slopeScaleQ10(slopeMmPerPx);
    int64_t detailMm = 0;
    [unroll]
    for (int i = 0; i < kOctaveCount; ++i)
    {
        detailMm += truncDiv(valueNoise2(seed, xMm, yMm, (int64_t)kOctaveLatticeMm[i],
                                         CH_DETAIL_OCTAVE_BASE + (uint)i) *
                                 (int64_t)kOctaveAmplitudeMm[i],
                             32768);
    }
    detailMm = truncDiv(detailMm * sScale, 1024);

    SurfaceEval s;
    s.px = px;
    s.py = py;
    s.slopeMmPerPx = slopeMmPerPx;
    s.surfaceMm = (int)clamp64(baseMm + detailMm, -8000000, 9000000);
    return s;
}

// --- ColumnMain: bit-exact mirror of vxc::Amplifier::column ----------------

[numthreads(8, 8, 1)]
void ColumnMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= DispatchColumns.x || tid.y >= DispatchColumns.y)
        return;

    // HARDENING (2026-07 cross-vendor UB pass, issue 4). PixelSizeMm is a
    // host-supplied divisor that reaches floorDiv/truncDiv below (and
    // `pxMm * pxMm`). Zero would be an OpUDiv-by-zero: undefined in SPIR-V,
    // so the result is whatever the vendor's integer unit happens to produce
    // (and on some stacks it can fault). There is no meaningful column to
    // derive from a zero-size pixel, so refuse the dispatch deterministically.
    // Mirrored by a host-side assert in voxel-core/bench/gpu_harness.cpp.
    // GUARD ONLY: valid PixelSizeMm is 30000 (scale 1) or 11250 (scale 8) —
    // never 0 — so this branch is never taken on any real dispatch.
    if (PixelSizeMm == 0)
        return;

    const uint64_t seed = ((uint64_t)SeedHi << 32) | (uint64_t)SeedLo;
    const int64_t vx = (int64_t)OriginVx + (int64_t)tid.x;
    const int64_t vy = (int64_t)OriginVy + (int64_t)tid.y;

    const SurfaceEval se = evalSurface(seed, vx, vy);
    const int64_t px = se.px;
    const int64_t py = se.py;
    const int64_t slopeMmPerPx = se.slopeMmPerPx;

    const uint cl = rasterClimate(px, py);
    const uint clTemperature = cl & 0xff;
    const uint clSeasonality = (cl >> 8) & 0xff;
    const uint clPrecipitation = (cl >> 16) & 0xff;

    GpuColumnSample outCol;
    outCol.surfaceMm = se.surfaceMm;

    int64_t topsoil =
        clamp64(300 + (int64_t)clPrecipitation * 8 - truncDiv(slopeMmPerPx, 4), 0, 2500);
    const int64_t tj =
        (int64_t)hashToSigned16(hash2(seed, vx >> 4, vy >> 4, CH_TOPSOIL_JITTER));
    topsoil += truncDiv(topsoil * tj, 4 * 32768); // tj is signed — truncDiv.
    outCol.topsoilMm = (int)topsoil;

    outCol.subsoilMm = (int)clamp64(topsoil * 2 + 500, 0, 6000);

    // Bedrock top: a jittered band CENTRED ON 200 m. kWorldGenVersion 5 moved
    // this from 40-60 m to 180-220 m (amplifier.cpp — read its comment for
    // why 200 m is the band's MEAN, not its floor). Same deterministic shape
    // as before, one 16-bit hash field linearly mapped onto [base, base+span);
    // only the two constants moved.
    const uint64_t bj = hash2(seed, vx >> 6, vy >> 6, CH_BEDROCK_JITTER);
    outCol.bedrockDepthMm = (int)(180000 + ((bj >> 48) * 40000) / 65536);

    // Surface material from biome classification (M4) — morphology gates
    // then Whittaker climate lookup, bit-exact mirror of
    // vxc::Amplifier::column's classifyBiome/biomeSurfaceMaterial call.
    const uint biome = classifyBiome((int)clTemperature, (int)clPrecipitation,
                                     (int)clSeasonality, outCol.surfaceMm, slopeMmPerPx);
    outCol.surfaceMat = biomeSurfaceMaterial(biome, outCol.surfaceMm);

    OutColumns[tid.x + tid.y * DispatchColumns.x] = outCol;
}

// --- VoxelizeMain: bit-exact mirror of vxc::Amplifier::materialAt ---------
// (fed by ColumnMain's output; columns are never recomputed here).
//
// OutCells layout contract (mirrored exactly in gpu_harness.cpp):
//   - The dispatch footprint DispatchColumns (same meaning as ColumnMain:
//     one thread per voxel column) MUST be brick-aligned: DispatchColumns.x
//     and .y are exact multiples of 8. BricksX = DispatchColumns.x / 8,
//     BricksY = DispatchColumns.y / 8.
//   - Thread (tid.x, tid.y) belongs to brick footprint
//     (bx, by) = (tid.x / 8, tid.y / 8) at local column (lx, ly) = (tid.x % 8, tid.y % 8).
//   - The vertical stack for footprint (bx, by) holds BricksZ bricks; stack
//     index bz in [0, BricksZ) maps to actual brick z = BrickZMin + bz,
//     covering voxel z in [(BrickZMin+bz)*8, (BrickZMin+bz)*8 + 8) — i.e. the
//     whole stack spans voxel z in [BrickZMin*8, (BrickZMin+BricksZ)*8).
//   - Bricks are ordered z-major WITHIN a column's stack, and footprints are
//     ordered bx-major-x-then-y (row-major, x fastest):
//       footprintIndex = bx + BricksX * by
//       brickIndex     = footprintIndex * BricksZ + bz
//   - Cell index within a brick mirrors vxc::Brick<8>::cellIndex exactly:
//       cellIndexInBrick(x, y, z) = x + 8 * (y + 8 * z)
//   - Final index into OutCells: brickIndex * 512 + cellIndexInBrick(lx, ly, zLocal)
//
// InColumns is indexed exactly like ColumnMain's OutColumns:
//   idx = tid.x + tid.y * DispatchColumns.x

uint cellIndexInBrick(uint x, uint y, uint z) { return x + 8u * (y + 8u * z); }

// vxc::Amplifier::stratigraphyAt bit-for-bit (the layer model, cave pass NOT
// applied — materialAt below is the full function).
uint stratigraphyAt(GpuColumnSample col, int64_t vz)
{
    const int64_t centreMm = vz * (int64_t)kVoxelSizeMm + (int64_t)kVoxelSizeMm / 2;
    const int64_t depthMm = (int64_t)col.surfaceMm - centreMm;
    if (depthMm < 0) return MAT_AIR;
    if (depthMm < (int64_t)col.topsoilMm) return col.surfaceMat;
    if (depthMm < (int64_t)col.topsoilMm + (int64_t)col.subsoilMm)
        return col.surfaceMat == MAT_SAND ? MAT_GRAVEL : MAT_SUBSOIL;
    if (depthMm < (int64_t)col.bedrockDepthMm) return MAT_ROCK;
    return MAT_BEDROCK;
}

// --- M4 cave pass ----------------------------------------------------------
// Bit-exact mirror of voxelcore/caves.h. Read that header for WHY the network
// is a jittered lattice graph rather than 3D noise, why tube depth is measured
// from the column's own surface, and what the three safety clamps are for; the
// code below is deliberately a line-for-line transliteration of it so the two
// can be diffed by eye. DO NOT let them drift.
//
// Recomputed here per VoxelizeMain thread rather than carried through
// GpuColumnSample: it needs only (seed, vx, vy, surfaceMm) — no raster reads —
// so recomputing costs the same 34 hashes ColumnMain would have paid, keeps
// the column buffer layout (and the harness's column comparison) unchanged,
// and avoids widening a cross-kernel struct contract.

static const int64_t kCaveLatticeMm = 25600;
static const int64_t kCaveNodeDepthMinMm = 9000;
static const int64_t kCaveNodeDepthSpanMm = 25000;
static const int64_t kCaveRadiusMinMm = 1200;
static const int64_t kCaveRadiusSpanMm = 1600;
static const int64_t kCaveBackboneMask = 3;
static const uint64_t kCaveEdgeGateMask = 3;
static const int64_t kCaveShaftNodeMask = 3;
static const uint64_t kCaveShaftGateMask = 3;
static const int64_t kCaveShaftRadiusMinMm = 1000;
static const int64_t kCaveShaftRadiusSpanMm = 700;
static const int64_t kCaveRoofMinMm = 6000;
static const int64_t kCaveBedrockMarginMm = 2000;
static const int kCaveMinSurfaceMm = 12000;
static const int64_t kCaveMinVoxelZ = 0;
// Raised 8 -> 12 by M4 cave pass v2 (C2): a crevice can now ride the SAME
// lattice edge as a tunnel, so a junction column can see 4 tunnels + 4
// crevices; 12 leaves headroom. Mirrors caves.h's kMaxCaveSegs exactly — the
// cap is part of the worldgen contract because it decides, together with the
// (dj, di, dir) iteration order, which segments survive if it were ever hit.
static const int kMaxCaveSegs = 12;

// Crevices (M4 cave pass v2, docs/cavern-design.md §4): a 1-in-8 gated thin
// lens-tapered vertical slab riding an EXISTING tube's axis. Not a new
// generator and not a new per-voxel mechanism — it emits an ordinary CaveSeg
// in depth space, so caveCarveAt is untouched.
static const uint64_t kCrevGateMask = 7;
static const int64_t kCrevHalfThickMinMm = 300;
static const int64_t kCrevHalfThickSpanMm = 500;
static const int64_t kCrevUpMinMm = 3000;
static const int64_t kCrevUpSpanMm = 7000;
static const int64_t kCrevDownMinMm = 2000;
static const int64_t kCrevDownSpanMm = 4000;

// HashChannel ids from voxelcore/caves.h — append only, never renumber.
static const uint CH_CAVE_NODE = 18;
static const uint CH_CAVE_EDGE = 19;
static const uint CH_CAVE_RADIUS = 20;
static const uint CH_CAVE_SHAFT = 21;
// 22/23/25 belong to the cavern pass below (CH_CAVERN_SITE/_ROUGH/_FLOOD).
static const uint CH_CREVICE = 24;

struct GpuCaveColumn
{
    int segCount;
    int segMarginSq[kMaxCaveSegs];
    int segDepthMm[kMaxCaveSegs];
    int shaftMarginSq;
    int shaftDepthMaxMm;
};

// vxc::caveEdgeExists bit-for-bit.
bool caveEdgeExists(uint64_t seed, int64_t ei, int64_t ej, int edir)
{
    if (edir == 0 && (ej & kCaveBackboneMask) == 0) return true;
    if (edir == 1 && (ei & kCaveBackboneMask) == 0) return true;
    return ((hash2(seed, ei, ej * 2 + (int64_t)edir, CH_CAVE_EDGE) >> 48) & kCaveEdgeGateMask) == 0;
}

// vxc::caveEdgeRadiusMm bit-for-bit.
int64_t caveEdgeRadiusMm(uint64_t seed, int64_t ei, int64_t ej, int edir)
{
    const uint64_t hr = hash2(seed, ei, ej * 2 + (int64_t)edir, CH_CAVE_RADIUS);
    return kCaveRadiusMinMm +
           (int64_t)((((hr >> 44) & 0xFFFFFULL) * (uint64_t)kCaveRadiusSpanMm) >> 20);
}

// vxc::caveColumnFor bit-for-bit, including the (dj, di, dir) iteration order
// that decides which tubes survive the kMaxCaveSegs cap.
GpuCaveColumn caveColumnFor(uint64_t seed, int64_t vx, int64_t vy, int surfaceMm)
{
    GpuCaveColumn cc;
    cc.segCount = 0;
    cc.shaftMarginSq = 0;
    cc.shaftDepthMaxMm = 0;
    [unroll]
    for (int q = 0; q < kMaxCaveSegs; ++q)
    {
        cc.segMarginSq[q] = 0;
        cc.segDepthMm[q] = 0;
    }
    if (surfaceMm < kCaveMinSurfaceMm) return cc;

    const int64_t xMm = vx * (int64_t)kVoxelSizeMm;
    const int64_t yMm = vy * (int64_t)kVoxelSizeMm;
    const int64_t ci = floorDiv(xMm, kCaveLatticeMm);
    const int64_t cj = floorDiv(yMm, kCaveLatticeMm);

    int64_t nodeX[16];
    int64_t nodeY[16];
    int64_t nodeD[16];
    for (int nj = 0; nj < 4; ++nj)
    {
        for (int ni = 0; ni < 4; ++ni)
        {
            const int64_t gi = ci - 1 + (int64_t)ni;
            const int64_t gj = cj - 1 + (int64_t)nj;
            const uint64_t hn = hash2(seed, gi, gj, CH_CAVE_NODE);
            const int nidx = ni + 4 * nj;
            nodeX[nidx] = gi * kCaveLatticeMm +
                          (int64_t)(((hn & 0xFFFFFULL) * (uint64_t)kCaveLatticeMm) >> 20);
            nodeY[nidx] = gj * kCaveLatticeMm +
                          (int64_t)((((hn >> 20) & 0xFFFFFULL) * (uint64_t)kCaveLatticeMm) >> 20);
            nodeD[nidx] = kCaveNodeDepthMinMm +
                          (int64_t)((((hn >> 40) & 0xFFFFFULL) * (uint64_t)kCaveNodeDepthSpanMm) >> 20);
        }
    }

    for (int dj = 0; dj < 3; ++dj)
    {
        for (int di = 0; di < 3; ++di)
        {
            const int64_t ei = ci - 1 + (int64_t)di;
            const int64_t ej = cj - 1 + (int64_t)dj;
            for (int dir = 0; dir < 2; ++dir)
            {
                if (!caveEdgeExists(seed, ei, ej, dir)) continue;
                const int aIdx = di + 4 * dj;
                const int bIdx = (dir == 0) ? (di + 1 + 4 * dj) : (di + 4 * (dj + 1));
                const int64_t axMm = nodeX[aIdx];
                const int64_t ayMm = nodeY[aIdx];
                const int64_t adMm = nodeD[aIdx];
                const int64_t bxMm = nodeX[bIdx];
                const int64_t byMm = nodeY[bIdx];
                const int64_t bdMm = nodeD[bIdx];

                const int64_t sdx = bxMm - axMm;
                const int64_t sdy = byMm - ayMm;
                const int64_t den = sdx * sdx + sdy * sdy;
                if (den == 0) continue;

                const int64_t wx = xMm - axMm;
                const int64_t wy = yMm - ayMm;
                const int64_t num = clamp64(wx * sdx + wy * sdy, 0, den);

                const int64_t cxMm = axMm + floorDiv(sdx * num, den);
                const int64_t cyMm = ayMm + floorDiv(sdy * num, den);
                const int64_t cdMm = adMm + floorDiv((bdMm - adMm) * num, den);

                const int64_t ex = xMm - cxMm;
                const int64_t ey = yMm - cyMm;
                const int64_t rr = caveEdgeRadiusMm(seed, ei, ej, dir);
                const int64_t marginSq = rr * rr - (ex * ex + ey * ey);
                if (marginSq <= 0) continue;

                if (cc.segCount < kMaxCaveSegs)
                {
                    cc.segMarginSq[cc.segCount] = (int)marginSq;
                    cc.segDepthMm[cc.segCount] = (int)cdMm;
                    cc.segCount += 1;
                }

                // Crevice: a 1-in-8 gated thin vertical slab riding this same
                // edge. Only reachable here because marginSq > 0 already put
                // us within the (wider) tunnel radius of this axis, and
                // kCrevHalfThickMaxMm < kCaveRadiusMinMm, so a column outside
                // the tube is provably outside the crevice's narrower
                // corridor too — no separate reject was skipped.
                //
                // caves.h computes this hash for every existing edge (its
                // lattice block) and reads it only here; recomputing it lazily
                // is the same pure function of (seed, i, j, dir), so the value
                // is identical.
                const uint64_t crevH = hash2(seed, ei, ej * 2 + (int64_t)dir, CH_CREVICE);
                if (((crevH >> 61) & kCrevGateMask) == 0)
                {
                    const int64_t tMm =
                        kCrevHalfThickMinMm +
                        (int64_t)(((crevH & 0xFFFFFULL) * (uint64_t)kCrevHalfThickSpanMm) >> 20);
                    if (ex * ex + ey * ey <= tMm * tMm)
                    {
                        const int64_t hUpMm =
                            kCrevUpMinMm +
                            (int64_t)((((crevH >> 20) & 0xFFFFFULL) * (uint64_t)kCrevUpSpanMm) >> 20);
                        const int64_t hDownMm =
                            kCrevDownMinMm +
                            (int64_t)((((crevH >> 40) & 0xFFFFFULL) * (uint64_t)kCrevDownSpanMm) >> 20);
                        // Per-column clamp: never let the slab's top reach
                        // shallower than the roof clamp, so a crevice pinches
                        // out gracefully instead of getting a flat lid from
                        // caveCarveAt's own guard.
                        const int64_t hUpEffMm = clamp64(hUpMm, 0, cdMm - kCaveRoofMinMm);
                        const int64_t halfSpanMm = truncDiv(hUpEffMm + hDownMm, 2);
                        // Lens taper 4u(1-u), u = num/den, in Q16 FIXED POINT
                        // rather than the exact rational num*(den-num)/den^2:
                        // den = dx^2+dy^2 reaches ~5e9 mm^2 for a jittered
                        // edge, so den*den overflows int64. caves.h found that
                        // overflow and took the same Q16 route — this mirrors
                        // it exactly, including both floorDivs.
                        const int64_t uQ16 = floorDiv(num << 16, den);
                        const int64_t taperQ16 = (4 * uQ16 * ((int64_t)65536 - uQ16)) >> 16;
                        const int64_t halfSpanTaperedMm =
                            floorDiv(halfSpanMm * taperQ16, (int64_t)65536);
                        const int64_t crevMarginSq = halfSpanTaperedMm * halfSpanTaperedMm;
                        if (crevMarginSq > 0 && cc.segCount < kMaxCaveSegs)
                        {
                            cc.segMarginSq[cc.segCount] = (int)crevMarginSq;
                            cc.segDepthMm[cc.segCount] =
                                (int)(cdMm + floorDiv(hDownMm - hUpEffMm, 2));
                            cc.segCount += 1;
                        }
                    }
                }
            }
        }
    }

    // Sinkhole shaft — at most one backbone-crossing node is in reach.
    for (int sj = 0; sj < 3; ++sj)
    {
        for (int si = 0; si < 3; ++si)
        {
            if (cc.shaftMarginSq != 0) continue;
            const int64_t hi = ci - 1 + (int64_t)si;
            const int64_t hj = cj - 1 + (int64_t)sj;
            if ((hi & kCaveShaftNodeMask) != 0 || (hj & kCaveShaftNodeMask) != 0) continue;
            const uint64_t hs = hash2(seed, hi, hj, CH_CAVE_SHAFT);
            if (((hs >> 48) & kCaveShaftGateMask) != 0) continue;
            const int sIdx = si + 4 * sj;
            const int64_t sr = kCaveShaftRadiusMinMm +
                               (int64_t)(((hs & 0xFFFFFULL) * (uint64_t)kCaveShaftRadiusSpanMm) >> 20);
            const int64_t sex = xMm - nodeX[sIdx];
            const int64_t sey = yMm - nodeY[sIdx];
            const int64_t sMarginSq = sr * sr - (sex * sex + sey * sey);
            if (sMarginSq <= 0) continue;
            cc.shaftMarginSq = (int)sMarginSq;
            cc.shaftDepthMaxMm = (int)nodeD[sIdx];
        }
    }
    return cc;
}

// vxc::caveCarveAt bit-for-bit.
bool caveCarveAt(GpuCaveColumn cc, int surfaceMm, int bedrockDepthMm, int64_t vz)
{
    if (cc.segCount == 0 && cc.shaftMarginSq == 0) return false;
    if (vz < kCaveMinVoxelZ) return false;
    if (surfaceMm < kCaveMinSurfaceMm) return false;

    const int64_t centreMm = vz * (int64_t)kVoxelSizeMm + (int64_t)kVoxelSizeMm / 2;
    const int64_t depthMm = (int64_t)surfaceMm - centreMm;
    if (depthMm < 0) return false;
    if (cc.shaftMarginSq > 0 && depthMm <= (int64_t)cc.shaftDepthMaxMm) return true;
    if (depthMm < kCaveRoofMinMm) return false;
    if (depthMm + kCaveBedrockMarginMm >= (int64_t)bedrockDepthMm) return false;

    for (int s = 0; s < cc.segCount; ++s)
    {
        const int64_t dz = depthMm - (int64_t)cc.segDepthMm[s];
        if (dz * dz < (int64_t)cc.segMarginSq[s]) return true;
    }
    return false;
}

// --- M4 cave pass v2 cavern pass -------------------------------------------
// Bit-exact mirror of voxelcore/caverns.h. Read that header for WHY sites are
// hash-gated backbone-crossing nodes 204.8 m apart, why each open site is a
// COAXIAL chain of four ellipsoid rooms descending from the anchor, and why
// the per-voxel roof/bedrock/sea clamps are LOAD-BEARING here rather than
// backstops (caverns anchor at absolute z, so a level room under sloping
// terrain has no draped-depth guarantee to fall back on). The code below is a
// line-for-line transliteration so the two can be diffed by eye.
//
// The CPU splits this into cavernCandidatesFor (per coarse cell) +
// cavernSiteFor (per site) + cavernColumnFromSites (per column) purely so
// amplifier.cpp can memoise the first two — each is a pure function of the
// coarse cell / site respectively. The fused form below is what caverns.h
// calls "the pure, self-contained f(...) the HLSL mirror is written against",
// and is bit-identical by construction: same arithmetic, same (dj, di, room)
// iteration order.

static const int64_t kCavernCoarseLatticeRatio = 8;
static const int64_t kCavernCoarseMm = kCaveLatticeMm * kCavernCoarseLatticeRatio; // 204800
static const uint64_t kCavernSiteGateMask = 3; // bits 60..61 of CH_CAVE_NODE == 0 -> open
static const int kCavernMinSurfaceMm = 20000;  // stricter than kCaveMinSurfaceMm, SITE only
static const int kCavernChildCount = 4;
static const int64_t kCavernRxyMinMm = 12000;
static const int64_t kCavernRxySpanMm = 16000;
static const int64_t kCavernRxyMaxMm = kCavernRxyMinMm + kCavernRxySpanMm;
static const int64_t kCavernRz0MinMm = 4000;
static const int64_t kCavernRz0SpanMm = 6000;
static const int64_t kCavernRzDeepMinMm = 12000;
static const int64_t kCavernRzDeepSpanMm = 28000;
static const int64_t kCavernFloorDropMinMm = 3000;
static const int64_t kCavernFloorDropSpanMm = 12000;
static const int64_t kCavernStepDownMinMm = 2000;
static const int64_t kCavernStepDownSpanMm = 12000;
static const int64_t kCavernRoughLatticeMm = 6400;
static const int64_t kCavernRoughAmpQ10 = 307;
static const int64_t kCavernRoughMinQ10 = 717;  // 1024 - kCavernRoughAmpQ10
static const int64_t kCavernRoughMaxQ10 = 1331; // 1024 + kCavernRoughAmpQ10
// kCaveRoofMinMm + kCavernRoofSafetyMarginMm + kCavernRz0MaxMm, and
// caveNode()'s own depth ceiling (kCaveNodeDepthMinMm + kCaveNodeDepthSpanMm).
static const int64_t kCavernNodeDepthSafeMinMm = 18000;
static const int64_t kCavernNodeDepthSafeMaxMm = 34000;
// (kCavernRxyMaxMm * kCavernRoughMaxQ10) / 1024 = (28000 * 1331) / 1024, i.e.
// the widest a roughened room radius can reach from the anchor. Written as a
// literal rather than the expression so no `/` appears at file scope.
static const int64_t kCavernMaxReachMm = 36394;
static const int64_t kCavernMaxReachSqMm = kCavernMaxReachMm * kCavernMaxReachMm;
static const int64_t kCavernFloodMinMm = 800;
static const int64_t kCavernFloodSpanMm = 2400;
static const uint kCavernFloodDryThreshold32 = 1717986918u; // (4 << 32) / 10
static const int kMaxCavernSegs = 6;
// vxc::CavernSite/CavernColumn's INT32_MIN "dry" sentinel. Spelled as a hex
// bit pattern because HLSL parses -2147483648 as a negation of a literal that
// does not fit in int.
static const int kCavernDryZMm = (int)0x80000000u;

static const uint CH_CAVERN_SITE = 22;  // per-room geometry (radii/floor/step)
static const uint CH_CAVERN_ROUGH = 23; // per-column wall roughness
static const uint CH_CAVERN_FLOOD = 25; // per-site flood level

// vxc::cavernHashField10 — unsigned 10-bit field -> [0, spanMm), multiply-
// then-shift, never a division. The CPU takes a `shift` parameter; here the
// caller pre-shifts instead, so every shift distance in this file stays an
// integer LITERAL (a variable shift distance is undefined past the operand
// width and is one of tools/lint-shader-ub.py's rules). Same value.
int64_t cavernField10(uint64_t hShifted, int64_t spanMm)
{
    return (int64_t)(((hShifted & 0x3FFULL) * (uint64_t)spanMm) >> 10);
}

struct GpuCaveNode
{
    int64_t xMm, yMm, depthMm;
};

// vxc::CavernSite. xy is always the anchor's (every room is coaxial), so only
// the per-room z, radii and floor are carried.
struct GpuCavernSite
{
    bool valid;
    int64_t anchorZMm;
    int64_t childZMm[kCavernChildCount];
    int64_t childRxyMm[kCavernChildCount];
    int64_t childRzMm[kCavernChildCount];
    int64_t childFloorZMm[kCavernChildCount];
    int floodZMm;
};

// vxc::CavernColumn.
struct GpuCavernColumn
{
    int count;
    int marginSq[kMaxCavernSegs];
    int zCenterMm[kMaxCavernSegs];
    int zFloorMm[kMaxCavernSegs];
    int floodZMm;
};

// vxc::cavernSiteFor bit-for-bit.
//
// THE surfaceAt CONTRACT (caverns.h, and the whole reason evalSurface exists
// as a separate function on both sides): the surface is sampled at the SITE's
// OWN (node.xMm, node.yMm), NOT at the querying column's xy. A lake or room
// floor that drapes with the terrain overhead is visibly wrong; caverns anchor
// at absolute z so floors and water tables stay level. amplifier.cpp supplies
// `evalSurface(floorDiv(xMm, kVoxelSizeMm), floorDiv(yMm, kVoxelSizeMm))
// .surfaceMm` — including the mm -> voxel floorDiv, mirrored exactly below.
GpuCavernSite cavernSiteFor(uint64_t seed, int64_t fi, int64_t fj, GpuCaveNode node)
{
    GpuCavernSite site;
    site.valid = false;
    site.anchorZMm = 0;
    site.floodZMm = kCavernDryZMm;
    [unroll]
    for (int q = 0; q < kCavernChildCount; ++q)
    {
        site.childZMm[q] = 0;
        site.childRxyMm[q] = 0;
        site.childRzMm[q] = 0;
        site.childFloorZMm[q] = 0;
    }

    const int siteSurfaceMm = evalSurface(seed, floorDiv(node.xMm, (int64_t)kVoxelSizeMm),
                                          floorDiv(node.yMm, (int64_t)kVoxelSizeMm))
                                  .surfaceMm;
    if (siteSurfaceMm < kCavernMinSurfaceMm) return site; // beach/ocean guard on the SITE

    site.anchorZMm = (int64_t)siteSurfaceMm - node.depthMm;

    int64_t maxFloorZMm = (int64_t)0x8000000000000000ULL; // INT64_MIN
    int64_t prevZMm = site.anchorZMm;
    for (int c = 0; c < kCavernChildCount; ++c)
    {
        const uint64_t h = hash3(seed, fi, fj, (int64_t)c, CH_CAVERN_SITE);
        const bool isRoot = (c == 0);

        const int64_t rxyMm = kCavernRxyMinMm + cavernField10(h, kCavernRxySpanMm);
        const int64_t rzMm = isRoot ? kCavernRz0MinMm + cavernField10(h >> 10, kCavernRz0SpanMm)
                                    : kCavernRzDeepMinMm + cavernField10(h >> 10, kCavernRzDeepSpanMm);
        const int64_t floorDropMm =
            kCavernFloorDropMinMm + cavernField10(h >> 20, kCavernFloorDropSpanMm);
        const int64_t stepDownMm =
            isRoot ? 0 : kCavernStepDownMinMm + cavernField10(h >> 30, kCavernStepDownSpanMm);

        const int64_t zMm = isRoot ? site.anchorZMm : prevZMm - stepDownMm;
        prevZMm = zMm;

        site.childZMm[c] = zMm;
        site.childRxyMm[c] = rxyMm;
        site.childRzMm[c] = rzMm;
        site.childFloorZMm[c] = zMm - floorDropMm;
        if (site.childFloorZMm[c] > maxFloorZMm) maxFloorZMm = site.childFloorZMm[c];
    }

    // Flood level: 40% of open sites dry, else a level just above the highest
    // room floor, clamped below the anchor and above zero.
    const uint64_t floodHash = hash2(seed, fi, fj, CH_CAVERN_FLOOD);
    if ((uint)(floodHash >> 32) >= kCavernFloodDryThreshold32)
    {
        const int64_t floodMm =
            maxFloorZMm + kCavernFloodMinMm + cavernField10(floodHash, kCavernFloodSpanMm);
        site.floodZMm = (int)clamp64(floodMm, 1, site.anchorZMm - 1);
    }

    site.valid = true;
    return site;
}

// vxc::cavernColumnFor bit-for-bit — the fused candidates + per-column
// reduction, including the (dj, di) corner order and the per-room order that
// together decide which segments survive the kMaxCavernSegs cap.
GpuCavernColumn cavernColumnFor(uint64_t seed, int64_t vx, int64_t vy, int surfaceMm)
{
    GpuCavernColumn cv;
    cv.count = 0;
    cv.floodZMm = kCavernDryZMm;
    [unroll]
    for (int q = 0; q < kMaxCavernSegs; ++q)
    {
        cv.marginSq[q] = 0;
        cv.zCenterMm[q] = 0;
        cv.zFloorMm[q] = 0;
    }
    // The QUERYING column's ordinary ocean/beach guard — deliberately the more
    // permissive kCaveMinSurfaceMm; kCavernMinSurfaceMm is stricter and
    // applies only to the SITE's own surface, inside cavernSiteFor.
    if (surfaceMm < kCaveMinSurfaceMm) return cv;

    const int64_t xMm = vx * (int64_t)kVoxelSizeMm;
    const int64_t yMm = vy * (int64_t)kVoxelSizeMm;
    const int64_t si = floorDiv(xMm, kCavernCoarseMm);
    const int64_t sj = floorDiv(yMm, kCavernCoarseMm);

    for (int dj = 0; dj < 2; ++dj)
    {
        for (int di = 0; di < 2; ++di)
        {
            const int64_t fi = (si + (int64_t)di) * kCavernCoarseLatticeRatio;
            const int64_t fj = (sj + (int64_t)dj) * kCavernCoarseLatticeRatio;

            // vxc::cavernCandidatesFor: cheapest reject first — the site gate
            // folded into bits 60/61 of the node-jitter hash caveNode() would
            // compute anyway, then the node those same bits also encode
            // (vxc::caveNodeFromHash), then child 0's depth safety window.
            const uint64_t hn = hash2(seed, fi, fj, CH_CAVE_NODE);
            if (((hn >> 60) & kCavernSiteGateMask) != 0) continue;
            GpuCaveNode node;
            node.xMm = fi * kCaveLatticeMm +
                       (int64_t)(((hn & 0xFFFFFULL) * (uint64_t)kCaveLatticeMm) >> 20);
            node.yMm = fj * kCaveLatticeMm +
                       (int64_t)((((hn >> 20) & 0xFFFFFULL) * (uint64_t)kCaveLatticeMm) >> 20);
            node.depthMm =
                kCaveNodeDepthMinMm +
                (int64_t)((((hn >> 40) & 0xFFFFFULL) * (uint64_t)kCaveNodeDepthSpanMm) >> 20);
            if (node.depthMm < kCavernNodeDepthSafeMinMm ||
                node.depthMm > kCavernNodeDepthSafeMaxMm)
                continue;

            // Every room shares the anchor's xy (coaxial chain), so this
            // distance is computed ONCE per column, not once per room.
            const int64_t ex = xMm - node.xMm;
            const int64_t ey = yMm - node.yMm;
            const int64_t dxySq = ex * ex + ey * ey;
            if (dxySq > kCavernMaxReachSqMm) continue;

            // Full reduction: the one place the terrain raster is read. At
            // most one corner per column ever reaches here (caverns.h
            // static_assert), so no column does this twice.
            const GpuCavernSite site = cavernSiteFor(seed, fi, fj, node);
            if (!site.valid) continue;

            // Wall roughness: one 2D value-noise sample for this column,
            // shared by every room of this site.
            const int64_t roughQ10 =
                clamp64(1024 + truncDiv(valueNoise2(seed, xMm, yMm, kCavernRoughLatticeMm,
                                                    CH_CAVERN_ROUGH) *
                                            kCavernRoughAmpQ10,
                                        32768),
                        kCavernRoughMinQ10, kCavernRoughMaxQ10);

            for (int c = 0; c < kCavernChildCount; ++c)
            {
                const int64_t rxyMm = site.childRxyMm[c];
                const int64_t rzMm = site.childRzMm[c];
                const int64_t rxySq = rxyMm * rxyMm;
                const int64_t rxySqRough = truncDiv(rxySq * roughQ10, 1024);
                if (dxySq >= rxySqRough) continue; // this room doesn't reach here
                const int64_t marginSq = truncDiv(rzMm * rzMm * (rxySqRough - dxySq), rxySqRough);
                if (marginSq <= 0) continue;
                if (cv.count < kMaxCavernSegs)
                {
                    cv.marginSq[cv.count] = (int)marginSq;
                    cv.zCenterMm[cv.count] = (int)site.childZMm[c];
                    cv.zFloorMm[cv.count] = (int)site.childFloorZMm[c];
                    cv.count += 1;
                }
            }
            cv.floodZMm = site.floodZMm;
        }
    }
    return cv;
}

// vxc::cavernCarveAt bit-for-bit. Same guard order/shape as caveCarveAt, minus
// the sinkhole exception (caverns have no construct allowed through the roof).
bool cavernCarveAt(GpuCavernColumn cv, int surfaceMm, int bedrockDepthMm, int64_t vz)
{
    if (cv.count == 0) return false;
    if (vz < kCaveMinVoxelZ) return false;
    if (surfaceMm < kCaveMinSurfaceMm) return false;

    const int64_t zAbs = vz * (int64_t)kVoxelSizeMm + (int64_t)kVoxelSizeMm / 2;
    const int64_t depthMm = (int64_t)surfaceMm - zAbs;
    if (depthMm < 0) return false; // above ground
    if (depthMm < kCaveRoofMinMm) return false;
    if (depthMm + kCaveBedrockMarginMm >= (int64_t)bedrockDepthMm) return false;

    for (int s = 0; s < cv.count; ++s)
    {
        if (zAbs < (int64_t)cv.zFloorMm[s]) continue; // flat floor clamp
        const int64_t dz = zAbs - (int64_t)cv.zCenterMm[s];
        if (dz * dz < (int64_t)cv.marginSq[s]) return true;
    }
    return false;
}

// vxc::Amplifier::materialAt bit-for-bit (stratigraphy, then caves, then
// caverns — caverns ordered last purely because a cavern column is far rarer,
// so the common case never reaches that call: cavernCarveAt's first test is
// `count == 0`).
uint materialAt(GpuColumnSample col, GpuCaveColumn cc, GpuCavernColumn cv, int64_t vz)
{
    const uint m = stratigraphyAt(col, vz);
    if (m == MAT_AIR || m == MAT_BEDROCK) return m;
    if (caveCarveAt(cc, col.surfaceMm, col.bedrockDepthMm, vz)) return MAT_AIR;
    if (cavernCarveAt(cv, col.surfaceMm, col.bedrockDepthMm, vz)) return MAT_AIR;
    return m;
}

[numthreads(8, 8, 1)]
void VoxelizeMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= DispatchColumns.x || tid.y >= DispatchColumns.y)
        return;

    // Same guard as ColumnMain, and now load-bearing for the same reason: the
    // cavern pass calls evalSurface (for the SITE's surface height), which
    // divides by PixelSizeMm. GUARD ONLY — valid values are 30000 or 11250.
    if (PixelSizeMm == 0)
        return;

    const uint bx = tid.x / 8u, by = tid.y / 8u;
    const uint lx = tid.x % 8u, ly = tid.y % 8u;
    const uint bricksX = DispatchColumns.x / 8u;
    const uint footprintIndex = bx + bricksX * by;

    const GpuColumnSample col = InColumns[tid.x + tid.y * DispatchColumns.x];

    // Cave pass: one reduction per column, reused down the whole brick stack —
    // the same split vxc::ColumnSample::cave makes on the CPU.
    const uint64_t caveSeed = ((uint64_t)SeedHi << 32) | (uint64_t)SeedLo;
    const int64_t colVx = (int64_t)OriginVx + (int64_t)tid.x;
    const int64_t colVy = (int64_t)OriginVy + (int64_t)tid.y;
    const GpuCaveColumn cave = caveColumnFor(caveSeed, colVx, colVy, col.surfaceMm);

    // Cavern pass, wired in exactly as the cave pass above: one reduction per
    // column, reused down the whole brick stack. Unlike caves it DOES read the
    // raster — cavernSiteFor evaluates the terrain height at the site's own xy
    // (see its comment) — so VoxelizeMain now binds ElevationMm (t3 -> Vulkan
    // binding 1). caverns.h's design doc §3.5 chose this over widening
    // GpuColumnSample: the site is generally not a column in the dispatch, so
    // there is no column entry to carry its surface on.
    const GpuCavernColumn cavern = cavernColumnFor(caveSeed, colVx, colVy, col.surfaceMm);

    for (uint bz = 0; bz < BricksZ; ++bz)
    {
        const uint brickIndex = footprintIndex * BricksZ + bz;
        const int64_t brickZ = (int64_t)BrickZMin + (int64_t)bz;
        [unroll]
        for (uint z = 0; z < 8u; ++z)
        {
            const int64_t vz = brickZ * (int64_t)8 + (int64_t)z;
            // lint-shader-ub: allow UNGUARDED_WRITE - index is bounded by construction: the entry guard caps tid below DispatchColumns, so footprintIndex < bricksX*bricksY and brickIndex < bricksX*bricksY*BricksZ, while lx/ly/z are all < 8; the host sizes OutCells to exactly bricksX*bricksY*BricksZ*512 (contract mirrored in gpu_harness.cpp, asserted there)
            OutCells[brickIndex * 512u + cellIndexInBrick(lx, ly, z)] =
                materialAt(col, cave, cavern, vz);
        }
    }
}

// --- Greedy mesher kernels (docs/gpu-mesher-design.md) ---------------------
// Bit-exact port of vxc::meshBrick<8> (voxelcore/mesher.h). Determinism
// strategy: count -> host exclusive scan -> emit at scanned offsets. NO
// atomics anywhere in the output path, so quad order is identical to the CPU
// stream by construction:
//   per interior brick: axis {0,1,2} x dir {neg,pos} x slice {0..7} masks,
//   quads within a mask in the CPU row-major greedy emission order.
//
// One thread per face-mask. Mask decode (mirrors the CPU loop nesting in
// meshBrick: axis outer, then dir, then slice):
//   maskIndex = meshBrickIndex * 48 + axis * 16 + dir * 8 + slice
// Interior bricks only (the 1-brick halo supplies apron reads):
//   interior dims: mbx = BricksX-2, mby = BricksY-2, mbz = BricksZ-2
//   meshBrickIndex = ix + mbx * (iy + mby * iz)   (x fastest, z slowest)
//   actual brick   = (ix+1, iy+1, iz+1) in region brick coords
//
// Bindings (register -> -fvk-shift Vulkan binding; all distinct, see the
// table above InColumns): u4 OutQuadCounts -> 7, t5 InQuadOffsets -> 6,
// u5 OutQuads -> 8.
//
// Packed quad (2x uint32, unpacked and byte-compared against vxc::Quad in
// the harness):
//   word0 = axis | positive<<4 | slice<<8 | u0<<16 | v0<<24
//   word1 = w | h<<8 | ao<<16 | mat<<24

RWStructuredBuffer<uint> OutQuadCounts : register(u4);
StructuredBuffer<uint> InQuadOffsets : register(t5);
RWStructuredBuffer<uint2> OutQuads : register(u5);

// Region-voxel material read. rv is region-local voxel coords (region brick
// (0,0,0) cell (0,0,0) is rv = (0,0,0)); callers only pass coords inside the
// voxelized region (interior brick +/-1 cell stays inside because of the
// halo brick).
uint regionCellMat(int3 rv)
{
    const uint bx = (uint)(rv.x >> 3), by = (uint)(rv.y >> 3), bz = (uint)(rv.z >> 3);
    const uint lx = (uint)(rv.x & 7), ly = (uint)(rv.y & 7), lz = (uint)(rv.z & 7);
    const uint bricksX = DispatchColumns.x / 8u;
    const uint footprintIndex = bx + bricksX * by;
    const uint brickIndex = footprintIndex * BricksZ + bz;
    return OutCells[brickIndex * 512u + cellIndexInBrick(lx, ly, lz)] & 0xffu;
}

// vxc::detail::aoCorner bit-for-bit.
uint aoCorner(bool s1, bool s2, bool c)
{
    // lint-shader-ub: allow UNSIGNED_UNDERFLOW - the subtrahend is a sum of three 0-or-1 terms, so it is at most 3 and the result stays in [0, 3]; cannot go below zero
    return (s1 && s2) ? 0u : (3u - ((s1 ? 1u : 0u) + (s2 ? 1u : 0u) + (c ? 1u : 0u)));
}

// Builds the 8x8 face mask for (brickOrigin, axis, dir, slice) and runs the
// CPU greedy algorithm. When emit != 0, writes packed quads at
// OutQuads[baseOffset + n]; always returns the quad count. Mirrors
// vxc::meshBrick<8> exactly: mask key = mat | ao<<8 | 1<<16.
uint greedyMask(int3 brickOriginRv, uint axis, uint dir, uint slice, uint emit, uint baseOffset)
{
    const uint u = (axis + 1u) % 3u;
    const uint v = (axis + 2u) % 3u;
    const int nOff = (dir != 0u) ? 1 : -1;

    uint mask[64];
    for (uint j = 0; j < 8u; ++j)
    {
        for (uint i = 0; i < 8u; ++i)
        {
            int3 c = int3(0, 0, 0);
            c[axis] = (int)slice;
            c[u] = (int)i;
            c[v] = (int)j;
            const uint m = regionCellMat(brickOriginRv + c);
            uint key = 0u;
            if (m != MAT_AIR)
            {
                int3 n = c;
                n[axis] += nOff;
                if (regionCellMat(brickOriginRv + n) == MAT_AIR)
                {
                    // 4-corner AO from the 8 cells ringing the face on the
                    // neighbor plane (mesher.h order: (0,0),(1,0),(0,1),(1,1)).
                    bool uNeg, uPos, vNeg, vPos, dNN, dPN, dNP, dPP;
                    {
                        int3 p;
                        p = n; p[u] -= 1;            uNeg = regionCellMat(brickOriginRv + p) != MAT_AIR;
                        p = n; p[u] += 1;            uPos = regionCellMat(brickOriginRv + p) != MAT_AIR;
                        p = n; p[v] -= 1;            vNeg = regionCellMat(brickOriginRv + p) != MAT_AIR;
                        p = n; p[v] += 1;            vPos = regionCellMat(brickOriginRv + p) != MAT_AIR;
                        p = n; p[u] -= 1; p[v] -= 1; dNN = regionCellMat(brickOriginRv + p) != MAT_AIR;
                        p = n; p[u] += 1; p[v] -= 1; dPN = regionCellMat(brickOriginRv + p) != MAT_AIR;
                        p = n; p[u] -= 1; p[v] += 1; dNP = regionCellMat(brickOriginRv + p) != MAT_AIR;
                        p = n; p[u] += 1; p[v] += 1; dPP = regionCellMat(brickOriginRv + p) != MAT_AIR;
                    }
                    uint ao = 0u;
                    ao |= aoCorner(uNeg, vNeg, dNN);
                    ao |= aoCorner(uPos, vNeg, dPN) << 2;
                    ao |= aoCorner(uNeg, vPos, dNP) << 4;
                    ao |= aoCorner(uPos, vPos, dPP) << 6;
                    key = m | (ao << 8) | (1u << 16);
                }
            }
            mask[i + 8u * j] = key;
        }
    }

    // Greedy rectangle merge - identical control flow to mesher.h.
    uint quadCount = 0u;
    for (uint j2 = 0; j2 < 8u; ++j2)
    {
        for (uint i2 = 0; i2 < 8u;)
        {
            const uint key = mask[i2 + 8u * j2];
            if (key == 0u)
            {
                ++i2;
                continue;
            }
            // HARDENING (2026-07 cross-vendor UB pass, issue 2). This was
            //   while (i2 + w < 8u && mask[i2 + w + 8u * j2] == key) ++w;
            // which reads mask[64] — one past `uint mask[64]` — on the
            // terminating iteration of the last row (i2 + w == 8, j2 == 7).
            // It is only safe if `&&` short-circuits, and HLSL has
            // historically declined to guarantee short-circuit evaluation for
            // scalar operands (the spec permits evaluating both sides; DXC
            // does short-circuit today, but that is a compiler property, not
            // a contract). Hoisting the bound into its own `if` makes the
            // in-range test structural. Identical iteration count and
            // identical `w` for every input — pure control-flow rewrite.
            uint w = 1u;
            while (i2 + w < 8u)
            {
                if (mask[i2 + w + 8u * j2] != key) break;
                ++w;
            }
            uint h = 1u;
            for (; j2 + h < 8u; ++h)
            {
                bool rowOk = true;
                for (uint k = 0; k < w; ++k)
                {
                    if (mask[i2 + k + 8u * (j2 + h)] != key)
                    {
                        rowOk = false;
                        break;
                    }
                }
                if (!rowOk) break;
            }
            for (uint dj = 0; dj < h; ++dj)
                for (uint di = 0; di < w; ++di)
                    mask[i2 + di + 8u * (j2 + dj)] = 0u;

            if (emit != 0u)
            {
                const uint ao8 = (key >> 8) & 0xffu;
                const uint mat = key & 0xffu;
                uint2 q;
                q.x = axis | (dir << 4) | (slice << 8) | (i2 << 16) | (j2 << 24);
                q.y = w | (h << 8) | (ao8 << 16) | (mat << 24);
                // HARDENING (2026-07 cross-vendor UB pass, issue 3).
                // `baseOffset` comes from InQuadOffsets, i.e. from whatever
                // the scan chain wrote; the write was previously unclamped,
                // so a short/stale ScanCount (offsets never scanned) or a
                // corrupt offset would scribble arbitrarily far outside
                // OutQuads. Bound it against the buffer's OWN length rather
                // than a new cbuffer field: GetDimensions lowers to
                // OpArrayLength over the bound descriptor range, so it is the
                // real allocation, needs no host/shader contract to stay in
                // sync, and cannot itself go stale. `quadIndex < baseOffset`
                // catches uint wraparound. GUARD ONLY: the harness sizes
                // OutQuads to the 32-quads-per-mask upper bound
                // (docs/gpu-mesher-design.md), so on every valid dispatch
                // quadIndex < quadCap holds and every quad is still written.
                uint quadCap, quadStride;
                OutQuads.GetDimensions(quadCap, quadStride);
                const uint quadIndex = baseOffset + quadCount;
                if (quadIndex >= baseOffset && quadIndex < quadCap)
                    OutQuads[quadIndex] = q;
            }
            ++quadCount;
            i2 += w;
        }
    }
    return quadCount;
}

// Decodes a flat mask index into (brick origin, axis, dir, slice); returns
// false when the index is out of range.
bool decodeMask(uint maskIndex, out int3 brickOriginRv, out uint axis, out uint dir, out uint slice)
{
    const uint bricksX = DispatchColumns.x / 8u;
    const uint bricksY = DispatchColumns.y / 8u;
    // HARDENING (2026-07 cross-vendor UB pass, issue 1). The interior-brick
    // dims below subtract the 1-brick halo from each axis. On UNSIGNED
    // values, a region with fewer than 3 bricks on any axis wraps: e.g.
    // bricksX == 1 gives mbx == 0xFFFFFFFF, and the product
    // mbx*mby*mbz*48 then wraps to some large-but-arbitrary maskCount that
    // the `maskIndex >= maskCount` range check happily passes. decodeMask
    // would return true with a nonsense brick origin, and regionCellMat
    // would read OutCells far out of bounds — vendor-defined behavior
    // exactly like the bug this pass exists to prevent (AMD's range-checked
    // descriptors return 0; NVIDIA's may return adjacent memory). A region
    // that thin has NO interior bricks, so the correct answer is "no mask".
    // GUARD ONLY: gpu_harness.cpp already refuses to run the mesh chain
    // unless bricksX/Y/Z are all >= 3, so this branch is never taken on any
    // dispatch that actually reaches the GPU today.
    if (bricksX < 3u || bricksY < 3u || BricksZ < 3u)
    {
        brickOriginRv = int3(0, 0, 0);
        axis = 0u;
        dir = 0u;
        slice = 0u;
        return false;
    }
    // lint-shader-ub: allow UNSIGNED_UNDERFLOW - the >= 3 early-out directly above proves every axis has at least 3 bricks, so each halo subtraction leaves >= 1; this is the exact underflow that guard was added to prevent
    const uint mbx = bricksX - 2u, mby = bricksY - 2u, mbz = BricksZ - 2u;
    const uint maskCount = mbx * mby * mbz * 48u;
    if (maskIndex >= maskCount)
    {
        brickOriginRv = int3(0, 0, 0);
        axis = 0u;
        dir = 0u;
        slice = 0u;
        return false;
    }
    const uint meshBrickIndex = maskIndex / 48u;
    const uint sub = maskIndex % 48u;
    axis = sub / 16u;
    dir = (sub % 16u) / 8u;
    slice = sub % 8u;
    const uint ix = meshBrickIndex % mbx;
    const uint iy = (meshBrickIndex / mbx) % mby;
    const uint iz = meshBrickIndex / (mbx * mby);
    brickOriginRv = int3((int)((ix + 1u) * 8u), (int)((iy + 1u) * 8u), (int)((iz + 1u) * 8u));
    return true;
}

[numthreads(64, 1, 1)]
void MeshCountMain(uint3 tid : SV_DispatchThreadID)
{
    int3 origin;
    uint axis, dir, slice;
    if (!decodeMask(tid.x, origin, axis, dir, slice))
        return;
    OutQuadCounts[tid.x] = greedyMask(origin, axis, dir, slice, 0u, 0u);
}

[numthreads(64, 1, 1)]
void MeshEmitMain(uint3 tid : SV_DispatchThreadID)
{
    int3 origin;
    uint axis, dir, slice;
    if (!decodeMask(tid.x, origin, axis, dir, slice))
        return;
    // HARDENING (2026-07 cross-vendor UB pass, issue 3). InQuadOffsets[i] is
    // only meaningful for i < ScanCount — those are the entries the
    // ScanBlocks/Sums/Add chain actually wrote. Emitting from an offset
    // beyond ScanCount would read never-written device memory, whose
    // contents differ by driver and allocation history, and then use it as a
    // write base. GUARD ONLY: the harness always sets ScanCount == maskCount
    // (both call sites), and decodeMask has already rejected
    // tid.x >= maskCount above, so this is unreachable on a valid dispatch.
    if (tid.x >= ScanCount)
        return;
    greedyMask(origin, axis, dir, slice, 1u, InQuadOffsets[tid.x]);
}

// --- GPU exclusive scan over per-mask quad counts ---------------------------
// Replaces the host scan between count and emit (the 128m gate's dominant
// cost — see docs/status.md). Determinism: fixed-order shared-memory
// Hillis-Steele over integers, identical result to the host scan by
// definition of exclusive scan. Three dispatches chained in ONE command
// buffer with barriers:
//   ScanBlocksMain: per-256-block exclusive scan of OutQuadCounts into
//                   OutQuadOffsets + per-block totals into OutBlockSums.
//   ScanSumsMain:   ONE workgroup exclusive-scans OutBlockSums in place
//                   (max 256 blocks => maskCount <= 65,536 per dispatch —
//                   the harness asserts this per tile).
//   ScanAddMain:    adds the scanned block base to every element.
// The emit pass then reads InQuadOffsets — the SAME buffer bound read-only.
// ScanCount (cbuffer append) = number of masks in this dispatch.

RWStructuredBuffer<uint> OutQuadOffsets : register(u6);
RWStructuredBuffer<uint> OutBlockSums : register(u7);

groupshared uint gsScan[256];

[numthreads(256, 1, 1)]
void ScanBlocksMain(uint3 tid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID,
                    uint3 gid : SV_GroupID)
{
    const uint n = ScanCount;
    const uint v = (tid.x < n) ? OutQuadCounts[tid.x] : 0u;
    gsScan[gtid.x] = v;
    GroupMemoryBarrierWithGroupSync();
    // Hillis-Steele inclusive scan (fixed iteration order — deterministic).
    [unroll]
    for (uint offset = 1u; offset < 256u; offset <<= 1u)
    {
        uint add = 0u;
        if (gtid.x >= offset)
            // lint-shader-ub: allow UNSIGNED_UNDERFLOW - the enclosing `gtid.x >= offset` test is precisely the no-underflow precondition for this subtraction
            add = gsScan[gtid.x - offset];
        GroupMemoryBarrierWithGroupSync();
        gsScan[gtid.x] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    // Exclusive = inclusive shifted right by one.
    // lint-shader-ub: allow UNSIGNED_UNDERFLOW - the ternary takes the subtracting branch only when gtid.x != 0, i.e. gtid.x >= 1, so this cannot go below zero
    const uint exclusive = (gtid.x == 0u) ? 0u : gsScan[gtid.x - 1u];
    if (tid.x < n)
        OutQuadOffsets[tid.x] = exclusive;
    if (gtid.x == 255u)
        // lint-shader-ub: allow UNGUARDED_WRITE - one store per workgroup, so gid.x < the dispatched group count, and the host sizes OutBlockSums to exactly ceil(ScanCount/256) = that group count (gpu_harness.cpp blockSumsBuf)
        OutBlockSums[gid.x] = gsScan[255u]; // block total (inclusive of padding zeros)
}

[numthreads(256, 1, 1)]
void ScanSumsMain(uint3 gtid : SV_GroupThreadID)
{
    const uint numBlocks = (ScanCount + 255u) / 256u;
    const uint v = (gtid.x < numBlocks) ? OutBlockSums[gtid.x] : 0u;
    gsScan[gtid.x] = v;
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint offset = 1u; offset < 256u; offset <<= 1u)
    {
        uint add = 0u;
        if (gtid.x >= offset)
            // lint-shader-ub: allow UNSIGNED_UNDERFLOW - the enclosing `gtid.x >= offset` test is precisely the no-underflow precondition for this subtraction
            add = gsScan[gtid.x - offset];
        GroupMemoryBarrierWithGroupSync();
        gsScan[gtid.x] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    // lint-shader-ub: allow UNSIGNED_UNDERFLOW - the ternary takes the subtracting branch only when gtid.x != 0, i.e. gtid.x >= 1, so this cannot go below zero
    const uint exclusive = (gtid.x == 0u) ? 0u : gsScan[gtid.x - 1u];
    if (gtid.x < numBlocks)
        OutBlockSums[gtid.x] = exclusive;
}

[numthreads(256, 1, 1)]
void ScanAddMain(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    if (tid.x < ScanCount)
        OutQuadOffsets[tid.x] += OutBlockSums[gid.x];
}
