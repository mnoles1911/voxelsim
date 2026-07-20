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
//   t0 ElevationMm    -> binding 1   (ColumnMain)
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

// vxc::Amplifier::materialAt bit-for-bit.
uint materialAt(GpuColumnSample col, int64_t vz)
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

[numthreads(8, 8, 1)]
void VoxelizeMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= DispatchColumns.x || tid.y >= DispatchColumns.y)
        return;

    const uint bx = tid.x / 8u, by = tid.y / 8u;
    const uint lx = tid.x % 8u, ly = tid.y % 8u;
    const uint bricksX = DispatchColumns.x / 8u;
    const uint footprintIndex = bx + bricksX * by;

    const GpuColumnSample col = InColumns[tid.x + tid.y * DispatchColumns.x];

    for (uint bz = 0; bz < BricksZ; ++bz)
    {
        const uint brickIndex = footprintIndex * BricksZ + bz;
        const int64_t brickZ = (int64_t)BrickZMin + (int64_t)bz;
        [unroll]
        for (uint z = 0; z < 8u; ++z)
        {
            const int64_t vz = brickZ * (int64_t)8 + (int64_t)z;
            OutCells[brickIndex * 512u + cellIndexInBrick(lx, ly, z)] = materialAt(col, vz);
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
            uint w = 1u;
            while (i2 + w < 8u && mask[i2 + w + 8u * j2] == key) ++w;
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
                OutQuads[baseOffset + quadCount] = q;
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
    greedyMask(origin, axis, dir, slice, 1u, InQuadOffsets[tid.x]);
}
