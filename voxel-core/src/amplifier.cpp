#include "voxelcore/amplifier.h"

#include "voxelcore/biome.h"

#include <atomic>

namespace vxc {
namespace {

// ---------------------------------------------------------------------------
// Tile-raster memo (performance only — provably cannot change any output).
//
// Measured on this box (clang -O2, seed 20260719, 409'600 columns): a column
// costs 0.946 us, of which the five ITileSampler reads (four elevationMm
// corners + one climate) are 0.574 us — 61%. Those reads are a function of the
// TILE PIXEL, and a 30 m pixel covers 300x300 = 90'000 voxel columns, so the
// same handful of values was being recomputed tens of thousands of times: the
// whole 640x640-column benchmark region spans just 25 distinct tile pixels.
// SyntheticTileSampler in particular evaluates seven value-noise octaves (28
// hashes) per elevationMm call, and it is the sampler the UE runtime uses by
// default (only -VoxelTileDir switches to the raster-backed TileGridSampler).
//
// Memoising is safe because ITileSampler is a read-only view of canonical tile
// DATA: elevationMm/climate are pure functions of (sampler, px, py). (They are
// non-const only so an implementation may lazily page tiles in; that changes
// no VALUE.) Determinism is therefore untouched — a hit and a miss return
// bit-identical results, and no worldgen constant or iteration order moves.
//
// The tables are `thread_local`, so the memo needs no lock and adds no
// cross-thread contention on the shared Amplifier the UE job pool calls into.
// They are direct-mapped (no LRU bookkeeping on the hot path); a colliding
// probe simply misses and recomputes, which is always correct.
// ---------------------------------------------------------------------------

constexpr uint32_t kElevSlots = 256;   // ~6 KB/thread
constexpr uint32_t kClimateSlots = 64; // ~2 KB/thread

struct ElevSlot {
    uint64_t amp = 0; // owning Amplifier id; 0 == empty (ids start at 1)
    int64_t px = 0, py = 0;
    int32_t value = 0;
};

struct ClimateSlot {
    uint64_t amp = 0;
    int64_t px = 0, py = 0;
    ClimateSample value{};
};

// Cheap index mix. Only distribution matters — collisions cost a recompute,
// never a wrong answer.
constexpr uint32_t slotIndex(uint64_t amp, int64_t px, int64_t py, uint32_t slots) {
    uint64_t h = static_cast<uint64_t>(px) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<uint64_t>(py) * 0xC2B2AE3D27D4EB4Full;
    h ^= amp * 0x165667B19E3779F9ull;
    return static_cast<uint32_t>((h >> 32) & (slots - 1));
}

int32_t cachedElevationMm(uint64_t amp, ITileSampler& tiles, int64_t px, int64_t py) {
    static thread_local ElevSlot slots[kElevSlots];
    ElevSlot& s = slots[slotIndex(amp, px, py, kElevSlots)];
    if (s.amp == amp && s.px == px && s.py == py) return s.value;
    const int32_t v = tiles.elevationMm(px, py);
    s.amp = amp;
    s.px = px;
    s.py = py;
    s.value = v;
    return v;
}

ClimateSample cachedClimate(uint64_t amp, ITileSampler& tiles, int64_t px, int64_t py) {
    static thread_local ClimateSlot slots[kClimateSlots];
    ClimateSlot& s = slots[slotIndex(amp, px, py, kClimateSlots)];
    if (s.amp == amp && s.px == px && s.py == py) return s.value;
    const ClimateSample v = tiles.climate(px, py);
    s.amp = amp;
    s.px = px;
    s.py = py;
    s.value = v;
    return v;
}

// ---------------------------------------------------------------------------
// Cave-lattice memo (performance only — provably cannot change any output).
//
// With the tile-raster memo above in place, `caveColumnFor` became the largest
// single term in column(): ~0.227 us/col, ~68% of the remaining cost. Almost
// all of it is the 34-70 hashes of the 4x4 node block, the 18 candidate edges
// and the sinkhole candidate — and caves.h now exposes those separately
// (`CaveLattice` / `caveLatticeFor`) because they depend ONLY on the lattice
// CELL (ci, cj), never on where in the cell the column sits. One cell is
// 25.6 m square = 65'536 voxel columns, so a sweep recomputes the identical
// block tens of thousands of times.
//
// Same direct-mapped, thread_local, never-reused-id scheme as the tile memo:
// a hit and a miss return bit-identical values (caveLatticeFor is pure), so
// determinism is untouched and no worldgen constant or iteration order moves.
// The key is (seed, ci, cj) — the seed, not the Amplifier id, because the
// lattice is a function of the seed alone and two Amplifiers on one seed
// legitimately share it.
// ---------------------------------------------------------------------------

constexpr uint32_t kCaveLatticeSlots = 16; // ~11 KB/thread at ~700 B/slot

struct CaveLatticeSlot {
    bool valid = false;
    uint64_t seed = 0;
    int64_t ci = 0, cj = 0;
    CaveLattice value;
};

const CaveLattice& cachedCaveLattice(uint64_t seed, int64_t ci, int64_t cj) {
    static thread_local CaveLatticeSlot slots[kCaveLatticeSlots];
    CaveLatticeSlot& s = slots[slotIndex(seed, ci, cj, kCaveLatticeSlots)];
    if (s.valid && s.seed == seed && s.ci == ci && s.cj == cj) return s.value;
    s.value = caveLatticeFor(seed, ci, cj);
    s.valid = true;
    s.seed = seed;
    s.ci = ci;
    s.cj = cj;
    return s.value;
}

// caveColumnFor(seed, vx, vy, surfaceMm), memoised. Bit-identical by
// construction: it is caves.h's own composition with the cell-only half served
// from the table.
CaveColumn cachedCaveColumn(uint64_t seed, int64_t vx, int64_t vy, int32_t surfaceMm) {
    if (surfaceMm < kCaveMinSurfaceMm) return CaveColumn{};
    const int64_t ci = floorDiv(vx * kVoxelSizeMm, kCaveLatticeMm);
    const int64_t cj = floorDiv(vy * kVoxelSizeMm, kCaveLatticeMm);
    return caveColumnFromLattice(cachedCaveLattice(seed, ci, cj), vx, vy);
}

// ---------------------------------------------------------------------------
// Cavern candidate-corner memo (performance only — same argument as above).
//
// caverns.h's gate + depth-safety pass over the 2x2 coarse corners is a
// function of the coarse cell (si, sj) alone, and a coarse cell is 204.8 m
// square = 4.2 million voxel columns. Memoising it turns the cavern pass's
// unavoidable per-column cost into four predicted-taken compares.
// ---------------------------------------------------------------------------

constexpr uint32_t kCavernCornerSlots = 8; // ~2 KB/thread

struct CavernCornerSlot {
    bool valid = false;
    uint64_t seed = 0;
    int64_t si = 0, sj = 0;
    CavernCandidates value;
};

const CavernCandidates& cachedCavernCandidates(uint64_t seed, int64_t si, int64_t sj) {
    static thread_local CavernCornerSlot slots[kCavernCornerSlots];
    CavernCornerSlot& s = slots[slotIndex(seed, si, sj, kCavernCornerSlots)];
    if (s.valid && s.seed == seed && s.si == si && s.sj == sj) return s.value;
    s.value = cavernCandidatesFor(seed, si, sj);
    s.valid = true;
    s.seed = seed;
    s.si = si;
    s.sj = sj;
    return s.value;
}

// ---------------------------------------------------------------------------
// Cavern SITE memo — the one that actually matters, and the one caverns.h's
// own cost model missed.
//
// A CavernSite is a function of (seed, fi, fj) and the terrain surface at the
// site's own anchor xy: a per-SITE value. But it was being decoded once per
// COLUMN, for every column inside the site's ~36 m reach disc — on the order
// of 400'000 columns — and each decode calls `surfaceAt`, which in production
// is the amplifier's full bilinear-tile-base + four-detail-octave surface
// function, i.e. the single most expensive thing in column(). Measured on
// vxc_bench --radius 32 (a 64 m square region, so a large fraction of it sits
// inside one reach disc), that alone took the amplify stage from 97.6 ms to
// 180.2 ms. The standalone "~28 ns/col" figure for the cavern pass was
// measured against a constant surfaceAt and does not survive contact with the
// real column path.
//
// Memoising per (seed, fi, fj) restores it: the decode runs once per site
// instead of once per column, and `surfaceAt` with it.
// ---------------------------------------------------------------------------

constexpr uint32_t kCavernSiteSlots = 8; // ~1.5 KB/thread

struct CavernSiteSlot {
    bool valid = false;
    uint64_t seed = 0;
    int64_t fi = 0, fj = 0;
    CavernSite value;
};

template <typename SurfaceFn>
const CavernSite& cachedCavernSite(uint64_t seed, int64_t fi, int64_t fj, const CaveNode& node,
                                   const SurfaceFn& surfaceAt) {
    static thread_local CavernSiteSlot slots[kCavernSiteSlots];
    CavernSiteSlot& s = slots[slotIndex(seed, fi, fj, kCavernSiteSlots)];
    if (s.valid && s.seed == seed && s.fi == fi && s.fj == fj) return s.value;
    s.value = cavernSiteFor(seed, fi, fj, node, surfaceAt);
    s.valid = true;
    s.seed = seed;
    s.fi = fi;
    s.fj = fj;
    return s.value;
}

// cavernColumnFor(...), memoised. Bit-identical by construction: caverns.h's
// own composition with the two cell/site-only halves served from tables.
template <typename SurfaceFn>
CavernColumn cachedCavernColumn(uint64_t seed, int64_t vx, int64_t vy, int32_t surfaceMm,
                                const SurfaceFn& surfaceAt) {
    if (surfaceMm < kCaveMinSurfaceMm) return CavernColumn{};
    const int64_t si = floorDiv(vx * kVoxelSizeMm, kCavernCoarseMm);
    const int64_t sj = floorDiv(vy * kVoxelSizeMm, kCavernCoarseMm);
    return cavernColumnFromSites(
        seed, cachedCavernCandidates(seed, si, sj), vx, vy,
        [&](int64_t fi, int64_t fj, const CaveNode& node) -> const CavernSite& {
            return cachedCavernSite(seed, fi, fj, node, surfaceAt);
        });
}


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

uint64_t Amplifier::nextId() {
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1; // 0 means "empty slot"
}

// The surface half of column(): the bilinear tile base plus the slope-scaled
// detail octaves, and the two by-products (tile pixel, tile slope) the rest of
// column() needs. Factored out ONLY so the cavern pass's `surfaceAt` callback
// can be the very same function the querying column's own surfaceMm came from
// — caverns.h's contract for it — bit-identically, by construction rather than
// by a comment asking two copies to stay in step.
Amplifier::SurfaceEval Amplifier::evalSurface(int64_t vx, int64_t vy) const {
    const int64_t xMm = vx * kVoxelSizeMm;
    const int64_t yMm = vy * kVoxelSizeMm;
    const int64_t pxMm = tiles_->pixelSizeMm();

    // Bilinear base elevation from the tile raster (exact integer math).
    const int64_t px = floorDiv(xMm, pxMm), py = floorDiv(yMm, pxMm);
    const int64_t fx = xMm - px * pxMm, fy = yMm - py * pxMm;
    const int64_t e00 = cachedElevationMm(id_, *tiles_, px, py);
    const int64_t e10 = cachedElevationMm(id_, *tiles_, px + 1, py);
    const int64_t e01 = cachedElevationMm(id_, *tiles_, px, py + 1);
    const int64_t e11 = cachedElevationMm(id_, *tiles_, px + 1, py + 1);
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

    SurfaceEval s;
    s.px = px;
    s.py = py;
    s.slopeMmPerPx = slopeMmPerPx;
    s.surfaceMm = clampi32(baseMm + detailMm, -8'000'000, 9'000'000);
    return s;
}

ColumnSample Amplifier::column(int64_t vx, int64_t vy) const {
    const SurfaceEval s = evalSurface(vx, vy);
    const int64_t px = s.px, py = s.py;
    const int64_t slopeMmPerPx = s.slopeMmPerPx;

    const ClimateSample cl = cachedClimate(id_, *tiles_, px, py);

    ColumnSample col;
    col.surfaceMm = s.surfaceMm;

    // Topsoil deepens with precipitation, thins with slope; +/-25% hash jitter
    // breaks up contour-following layer boundaries.
    int64_t topsoil =
        clampi64(300 + static_cast<int64_t>(cl.precipitation) * 8 - slopeMmPerPx / 4, 0, 2500);
    const int64_t tj = hashToSigned16(hash2(seed_, vx >> 4, vy >> 4, CH_TOPSOIL_JITTER));
    topsoil += topsoil * tj / (4 * 32768);
    col.topsoilMm = static_cast<int32_t>(topsoil);

    col.subsoilMm = clampi32(topsoil * 2 + 500, 0, 6000);

    // Bedrock top: a jittered band CENTRED ON 200 m (Matt's decision; was
    // 40-60 m at kWorldGenVersion 4). Same deterministic shape as before — one
    // 16-bit field of a 6.4 m-lattice hash, linearly mapped onto [base, base +
    // span) — only the two constants move.
    //
    // Why 180-220 m rather than "the old 40 m + 50%-of-base shape scaled up"
    // (which would give 200-300 m, mean 250 m): 200 m is the number asked for,
    // so it is the band's MEAN, not its floor. The +/-20 m (10%) span is large
    // enough that the bedrock boundary does not read as a flat sheet draped
    // under the terrain — the only reason the jitter exists — while staying
    // comfortably clear of everything above it: the deepest a cavern chain
    // reaches is ~128 m (test_caverns.cpp measures 128'050 mm of depth at a
    // 200 m bedrock), so even the shallowest 180 m draw leaves ~52 m of
    // untouched rock above the floor against caves.h's 2 m margin.
    const uint64_t bj = hash2(seed_, vx >> 6, vy >> 6, CH_BEDROCK_JITTER);
    col.bedrockDepthMm = static_cast<int32_t>(180000 + ((bj >> 48) * 40000) / 65536);

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
    // Served from the per-thread cave-lattice memo above; bit-identical to
    // caveColumnFor(seed_, vx, vy, col.surfaceMm).
    col.cave = cachedCaveColumn(seed_, vx, vy, col.surfaceMm);

    // M4 cave pass v2 cavern pass (voxelcore/caverns.h), wired in exactly as
    // the cave pass above is: one reduction per column, carried in the
    // ColumnSample, consumed per voxel by materialAt.
    //
    // The one shape difference is the `surfaceAt` callback. Caverns anchor at
    // ABSOLUTE z (level floors and water tables, not draped ones), so the
    // reduction needs the terrain height at the SITE's own xy rather than at
    // the querying column's — see caverns.h's cavernSiteFor. Supplying
    // evalSurface() here makes that the identical function this column's own
    // surfaceMm came from, which is the contract that callback is written
    // against. It is invoked at most once per column and only for the <1% of
    // columns that pass the gate/depth/xy-reach rejects, and it hits the tile
    // memo above, so it costs nothing in the common case. C6 will recompute it
    // inside VoxelizeMain rather than widening GpuColumnSample.
    const auto surfaceAt = [this](int64_t xMm, int64_t yMm) -> int32_t {
        return evalSurface(floorDiv(xMm, int64_t(kVoxelSizeMm)),
                           floorDiv(yMm, int64_t(kVoxelSizeMm)))
            .surfaceMm;
    };
    col.cavern = cachedCavernColumn(seed_, vx, vy, col.surfaceMm, surfaceAt);
    return col;
}

// Per-thread memo of whole columns, for the voxel-walking callers described in
// amplifier.h. Same direct-mapped, lock-free, never-reused-id scheme as the
// tile memo above, and equally output-neutral: a hit returns the value a miss
// would have computed. 256 slots x 96 bytes = 24 KB/thread; a z-innermost walk
// (the common shape) needs only one live slot, and 256 additionally covers a
// row-major sweep across a brick footprint.
const ColumnSample& Amplifier::columnCached(int64_t vx, int64_t vy) const {
    constexpr uint32_t kColumnSlots = 256;
    struct ColumnSlot {
        uint64_t amp = 0; // 0 == empty (ids start at 1)
        int64_t vx = 0, vy = 0;
        ColumnSample value;
    };
    static thread_local ColumnSlot slots[kColumnSlots];

    ColumnSlot& s = slots[slotIndex(id_, vx, vy, kColumnSlots)];
    if (s.amp == id_ && s.vx == vx && s.vy == vy) return s.value;
    s.value = column(vx, vy);
    s.amp = id_;
    s.vx = vx;
    s.vy = vy;
    return s.value;
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
    if (caveCarveAt(col.cave, col.surfaceMm, col.bedrockDepthMm, vz))
        return static_cast<MaterialId>(MAT_AIR);
    // Caverns (voxelcore/caverns.h) carve the same way tunnels do and under
    // the same three independent bedrock guards — the MAT_BEDROCK refusal
    // above, cavernCarveAt's own margin clamp, and the geometry itself.
    // Ordered after caves purely because a cavern column is far rarer, so the
    // common case never reaches this call: cavernCarveAt's first test is
    // `count == 0`.
    if (cavernCarveAt(col.cavern, col.surfaceMm, col.bedrockDepthMm, vz))
        return static_cast<MaterialId>(MAT_AIR);
    return m;
}

} // namespace vxc
