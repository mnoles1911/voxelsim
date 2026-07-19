// Worldgen GPU kernel v1 — HLSL port of the CPU reference (hash.h +
// amplifier.cpp), per ADR-0001: single HLSL source compiled to DXIL for UE
// RDG and to SPIR-V (DXC -spirv) for the headless Vulkan determinism/perf
// harness.
//
// DETERMINISM CONTRACT (docs/determinism.md): every expression here must be
// a bit-exact mirror of the CPU reference. Integer-only; int64/uint64
// arithmetic requires Int64 shader ops (D3D12 Int64ShaderOps / Vulkan
// shaderInt64 — see ADR-0001). C++ and HLSL agree on: two's-complement
// casts, truncation-toward-zero for / and %, arithmetic >> on signed.
// Any change here that alters output is world-breaking: bump
// vxc::kWorldGenVersion and regenerate goldens, never patch silently.
//
// Kernel: ColumnMain — one thread per column, computes the full
// ColumnSample stratigraphy (mirrors vxc::Amplifier::column). Voxelization
// and greedy meshing kernels build on this in later ports.

// --- constants mirrored from voxel-core (compile checks in harness) --------

static const int kVoxelSizeMm = 100;

// HashChannel (hash.h) — append only, never renumber.
static const uint CH_DETAIL_OCTAVE_BASE = 0;
static const uint CH_TOPSOIL_JITTER = 16;
static const uint CH_BEDROCK_JITTER = 17;

// Material ids (core.h).
static const uint MAT_AIR = 0;
static const uint MAT_BEDROCK = 1;
static const uint MAT_ROCK = 2;
static const uint MAT_GRAVEL = 3;
static const uint MAT_SAND = 4;
static const uint MAT_SUBSOIL = 5;
static const uint MAT_TOPSOIL = 6;
static const uint MAT_SNOW = 7;

// Detail octave table v1 (amplifier.cpp kDetailOctaves).
static const int kOctaveCount = 4;
static const int kOctaveLatticeMm[kOctaveCount] = {25600, 6400, 1600, 400};
static const int kOctaveAmplitudeMm[kOctaveCount] = {1800, 700, 260, 100};

// --- primitives mirrored from core.h / hash.h ------------------------------

int64_t floorDiv(int64_t a, int64_t b)
{
    const int64_t q = a / b;
    const int64_t r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
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

// Top 16 bits -> [-32768, 32767].
int hashToSigned16(uint64_t h)
{
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
    return ((v00 * gx + v10 * fx) * gy + (v01 * gx + v11 * fx) * fy) /
           (latticeMm * latticeMm);
}

int64_t slopeScaleQ10(int64_t slopeMmPerPx)
{
    return clamp64(512 + slopeMmPerPx / 24, 256, 4096);
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

cbuffer WorldGenParams : register(b0)
{
    uint2 DispatchColumns;  // columns along x, y for this dispatch
    int2 RasterOriginPx;    // tile-pixel coordinate of raster window texel (0,0)
    uint2 RasterSize;       // raster window extent in pixels
    int PixelSizeMm;        // 30000 (scale 1) or 11250 (scale 8)
    uint SeedLo;            // uint64 seed split across two 32-bit values
    uint SeedHi;
    int OriginVx;           // world voxel coord of dispatch column (0,0);
    int OriginVy;           // int32 spans +/-214,000 km at 10cm — Earth fits
    int Pad0;
};

// Raster window (elevation mm per pixel; climate packed t|s<<8|p<<16|v<<24).
// The harness/RDG pass MUST size the window to cover every pixel the
// dispatch bilinearly samples ([floorDiv(min xMm, px) .. floorDiv(max xMm,
// px)+1]); reads clamp to the window edge deterministically as a defensive
// backstop, but a clamped read means the caller sized the window wrong.
StructuredBuffer<int> ElevationMm : register(t0);
StructuredBuffer<uint> ClimatePacked : register(t1);

RWStructuredBuffer<GpuColumnSample> OutColumns : register(u0);

int rasterElevationMm(int64_t px, int64_t py)
{
    const int64_t lx = clamp64(px - RasterOriginPx.x, 0, (int64_t)RasterSize.x - 1);
    const int64_t ly = clamp64(py - RasterOriginPx.y, 0, (int64_t)RasterSize.y - 1);
    return ElevationMm[(uint)(lx + ly * RasterSize.x)];
}

uint rasterClimate(int64_t px, int64_t py)
{
    const int64_t lx = clamp64(px - RasterOriginPx.x, 0, (int64_t)RasterSize.x - 1);
    const int64_t ly = clamp64(py - RasterOriginPx.y, 0, (int64_t)RasterSize.y - 1);
    return ClimatePacked[(uint)(lx + ly * RasterSize.x)];
}

// --- ColumnMain: bit-exact mirror of vxc::Amplifier::column ----------------

[numthreads(8, 8, 1)]
void ColumnMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= DispatchColumns.x || tid.y >= DispatchColumns.y)
        return;

    const uint64_t seed = ((uint64_t)SeedHi << 32) | (uint64_t)SeedLo;
    const int64_t vx = (int64_t)OriginVx + (int64_t)tid.x;
    const int64_t vy = (int64_t)OriginVy + (int64_t)tid.y;
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
    const int64_t baseMm =
        ((e00 * gx + e10 * fx) * gy + (e01 * gx + e11 * fx) * fy) / (pxMm * pxMm);

    const int64_t slopeMmPerPx =
        (e10 > e00 ? e10 - e00 : e00 - e10) + (e01 > e00 ? e01 - e00 : e00 - e01);

    const int64_t sScale = slopeScaleQ10(slopeMmPerPx);
    int64_t detailMm = 0;
    [unroll]
    for (int i = 0; i < kOctaveCount; ++i)
    {
        detailMm += valueNoise2(seed, xMm, yMm, (int64_t)kOctaveLatticeMm[i],
                                CH_DETAIL_OCTAVE_BASE + (uint)i) *
                    (int64_t)kOctaveAmplitudeMm[i] / 32768;
    }
    detailMm = detailMm * sScale / 1024;

    const uint cl = rasterClimate(px, py);
    const uint clTemperature = cl & 0xff;
    const uint clPrecipitation = (cl >> 16) & 0xff;

    GpuColumnSample outCol;
    outCol.surfaceMm = (int)clamp64(baseMm + detailMm, -8000000, 9000000);

    int64_t topsoil =
        clamp64(300 + (int64_t)clPrecipitation * 8 - slopeMmPerPx / 4, 0, 2500);
    const int64_t tj =
        (int64_t)hashToSigned16(hash2(seed, vx >> 4, vy >> 4, CH_TOPSOIL_JITTER));
    topsoil += topsoil * tj / (4 * 32768);
    outCol.topsoilMm = (int)topsoil;

    outCol.subsoilMm = (int)clamp64(topsoil * 2 + 500, 0, 6000);

    const uint64_t bj = hash2(seed, vx >> 6, vy >> 6, CH_BEDROCK_JITTER);
    outCol.bedrockDepthMm = (int)(40000 + ((bj >> 48) * 20000) / 65536);

    if (clTemperature < 70 || outCol.surfaceMm > 2800000)
        outCol.surfaceMat = MAT_SNOW;
    else if ((clPrecipitation < 45 && clTemperature > 150) ||
             (outCol.surfaceMm > -2000 && outCol.surfaceMm < 3000))
        outCol.surfaceMat = MAT_SAND;
    else
        outCol.surfaceMat = MAT_TOPSOIL;

    OutColumns[tid.x + tid.y * DispatchColumns.x] = outCol;
}
