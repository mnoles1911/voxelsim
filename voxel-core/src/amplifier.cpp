#include "voxelcore/amplifier.h"

#include "voxelcore/biome.h"
#include "voxelcore/carrier.h"
#include "voxelcore/density3.h"
#include "voxelcore/detail_bedding.h"
#include "voxelcore/detail_rill.h"

#include <atomic>

namespace vxc {
namespace {

// ---------------------------------------------------------------------------
// Tile-raster memo (performance only â€” provably cannot change any output).
//
// Measured on this box (clang -O2, seed 20260719, 409'600 columns): a column
// costs 0.946 us, of which the five ITileSampler reads (four elevationMm
// corners + one climate) are 0.574 us â€” 61%. Those reads are a function of the
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
// no VALUE.) Determinism is therefore untouched â€” a hit and a miss return
// bit-identical results, and no worldgen constant or iteration order moves.
//
// The tables are `thread_local`, so the memo needs no lock and adds no
// cross-thread contention on the shared Amplifier the UE job pool calls into.
// They are direct-mapped (no LRU bookkeeping on the hot path); a colliding
// probe simply misses and recomputes, which is always correct.
// ---------------------------------------------------------------------------

constexpr uint32_t kElevSlots = 256;   // ~6 KB/thread
constexpr uint32_t kClimateSlots = 64; // ~2 KB/thread

// --- opt-in memo instrumentation (VXC_MEMO_STATS) ---------------------------
//
// WHY COUNTS AND NOT A CLOCK. The claim the block memos exist to support is
// "16 per-column probes become 1", which is a COUNT. Counts are deterministic
// and completely immune to whatever else is running on the box; wall-clock is
// neither, and a contended run reads exactly like a slow configuration
// (docs/measurements/s1-close-2026-07-27.txt). Timing still decides whether the
// saving is worth anything, but the mechanism should be proved by counting it.
//
// Plain thread_local, not atomics: this is per-thread by construction, and an
// atomic in the hottest read in worldgen would be measuring the instrument.
// Compiled out entirely unless VXC_MEMO_STATS is defined, so the shipping path
// is untouched.
#ifdef VXC_MEMO_STATS
thread_local MemoStats tlMemoStats{};
#define VXC_MEMO_COUNT(field) (++tlMemoStats.field)
#else
#define VXC_MEMO_COUNT(field) ((void)0)
#endif

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

// Cheap index mix. Only distribution matters â€” collisions cost a recompute,
// never a wrong answer.
constexpr uint32_t slotIndex(uint64_t amp, int64_t px, int64_t py, uint32_t slots) {
    uint64_t h = static_cast<uint64_t>(px) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<uint64_t>(py) * 0xC2B2AE3D27D4EB4Full;
    h ^= amp * 0x165667B19E3779F9ull;
    return static_cast<uint32_t>((h >> 32) & (slots - 1));
}

int32_t cachedElevationMm(uint64_t amp, ITileSampler& tiles, int64_t px, int64_t py) {
    static thread_local ElevSlot slots[kElevSlots];
    VXC_MEMO_COUNT(elevProbes);
    ElevSlot& s = slots[slotIndex(amp, px, py, kElevSlots)];
    if (s.amp == amp && s.px == px && s.py == py) return s.value;
    VXC_MEMO_COUNT(elevMisses);
    const int32_t v = tiles.elevationMm(px, py);
    s.amp = amp;
    s.px = px;
    s.py = py;
    s.value = v;
    return v;
}

// The carrier's 4x4 control stencil, memoized as a BLOCK rather than as
// sixteen separate elevation probes.
//
// WHY THIS EXISTS, MEASURED. v9's cubic carrier reads 16 control points per
// column where v8's bilinear read 4. Going through cachedElevationMm sixteen
// times costs sixteen hash-and-compare probes per column, and the bench said
// exactly what that is worth: the amplify stage went 3321 ms -> 6616 ms at
// radius 128, a 2.0x regression, with total time up 1.53x. The reads
// themselves were nearly all memo HITS -- the cost was the probing, not the
// sampling.
//
// A whole level-0 chunk is 3.2 m across and a scale-1 tile pixel is 30 m, so
// every column in a chunk almost always falls in the SAME cell and wants the
// SAME sixteen control points. Caching the block keyed on the cell collapses
// those sixteen probes to one.
//
// Bit-identical by construction: it returns the same values cachedElevationMm
// would have, just fetched once. Eight slots because a chunk footprint touches
// at most 2x2 cells (4), with headroom for the bound's wider walk.
//
// v13: the slot now also carries the cell's RELIEF (carrier.h), which is a
// function of the same cell and costs four more reads, and the control points
// are PREFILTERED on a tier that ships samples. Both are per-cell, so both
// belong behind the same memo and neither adds a per-column read.
constexpr uint32_t kStencilSlots = 8;

struct StencilSlot {
    uint64_t amp = 0;
    int64_t px = 0, py = 0;
    int64_t cp[16] = {};
    int64_t reliefMm = 0;
};

// The 4x4 control stencil for cell (px, py) and the cell's landform relief.
//
// THE PREFILTER IS APPLIED HERE, ONCE PER CELL, and not inside evalCarrier:
// evalCarrier's contract is "given control points, evaluate the spline", and
// the question of whether a tier ships control points or samples belongs to the
// tile interface, not to the evaluator. carrier.h's carrierPrefiltersSamples is
// the single place that decides.
const StencilSlot& cachedStencilSlot(uint64_t amp, ITileSampler& tiles, int64_t px,
                                     int64_t py) {
    static thread_local StencilSlot slots[kStencilSlots];
    VXC_MEMO_COUNT(stencilProbes);
    StencilSlot& s = slots[slotIndex(amp, px, py, kStencilSlots)];
    if (s.amp == amp && s.px == px && s.py == py) return s;
    VXC_MEMO_COUNT(stencilMisses);

    const int64_t pxMm = tiles.pixelSizeMm();
    if (carrierPrefiltersSamples(pxMm)) {
        constexpr int64_t S = kCarrierPrefilterSpan;
        int64_t raw[S * S];
        for (int64_t b = 0; b < S; ++b)
            for (int64_t a = 0; a < S; ++a)
                raw[a + S * b] = cachedElevationMm(amp, tiles, px + kCarrierPrefilterLo + a,
                                                   py + kCarrierPrefilterLo + b);
        carrierPrefilterStencil(raw, s.cp);
    } else {
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
                s.cp[i + 4 * j] = cachedElevationMm(amp, tiles, px - 1 + i, py - 1 + j);
    }

    // Relief across a fixed 30 m PHYSICAL baseline, from the RAW raster rather
    // than from the prefiltered control points. The raster is the measurement;
    // the control points are a representation of it, and prefiltering amplifies
    // exactly the high frequencies the relief statistic is trying not to be
    // dominated by.
    const int64_t L = carrierReliefLagPx(pxMm);
    s.reliefMm = carrierReliefMm(cachedElevationMm(amp, tiles, px - L, py),
                                 cachedElevationMm(amp, tiles, px + L, py),
                                 cachedElevationMm(amp, tiles, px, py - L),
                                 cachedElevationMm(amp, tiles, px, py + L));
    s.amp = amp;
    s.px = px;
    s.py = py;
    return s;
}

// cachedStencil() lived here: a thin wrapper returning cachedStencilSlot().cp.
// v13 gave the slot a second field the callers need (reliefMm, computed from the
// RAW raster rather than the prefiltered control points), so every call site
// moved to cachedStencilSlot directly -- see the one at the column solve below.
// That left the .cp-only wrapper with no callers, and -Werror=unused-function
// turned it into a hard build failure on gcc and clang. Deleted rather than
// silenced: it is superseded, not merely unreferenced, and git holds it if the
// prefilter path ever wants a .cp-only accessor again.

// The climate 2x2 blend block, memoized as a block for the same reason and
// with the same bit-identity argument as cachedStencilSlot above: v9's faded
// bilinear reads four corners per column where v8's nearest-pixel read one.
struct ClimateQuadSlot {
    uint64_t amp = 0;
    int64_t px = 0, py = 0;
    ClimateSample c[4]{}; // (0,0) (1,0) (0,1) (1,1)
};

const ClimateSample* cachedClimateQuad(uint64_t amp, ITileSampler& tiles, int64_t px,
                                       int64_t py);

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

const ClimateSample* cachedClimateQuad(uint64_t amp, ITileSampler& tiles, int64_t px,
                                       int64_t py) {
    static thread_local ClimateQuadSlot slots[kStencilSlots];
    ClimateQuadSlot& s = slots[slotIndex(amp, px, py, kStencilSlots)];
    if (s.amp == amp && s.px == px && s.py == py) return s.c;
    s.c[0] = cachedClimate(amp, tiles, px, py);
    s.c[1] = cachedClimate(amp, tiles, px + 1, py);
    s.c[2] = cachedClimate(amp, tiles, px, py + 1);
    s.c[3] = cachedClimate(amp, tiles, px + 1, py + 1);
    s.amp = amp;
    s.px = px;
    s.py = py;
    return s.c;
}

// Faded-bilinear blend of four climate corner samples (v9).
//
// Per CHANNEL, in u8 space, with the same exact-integer bilinear form the
// carrier used before v9 -- fx/fy arrive already quintic-faded, so the blend is
// C1 in position and no gradient step survives at a pixel line.
//
// Truncating division on a non-negative numerator: every input is a u8 and both
// weights are non-negative, so the result cannot leave [0, 255] and needs no
// clamp. Mirrored by truncDiv in worldgen.ush.
ClimateSample blendClimate(const ClimateSample& c00, const ClimateSample& c10,
                           const ClimateSample& c01, const ClimateSample& c11, int64_t fx,
                           int64_t fy, int64_t pxMm) {
    const int64_t gx = pxMm - fx, gy = pxMm - fy;
    const int64_t den = pxMm * pxMm;
    auto mix = [&](uint8_t v00, uint8_t v10, uint8_t v01, uint8_t v11) {
        return static_cast<uint8_t>(
            ((int64_t(v00) * gx + int64_t(v10) * fx) * gy +
             (int64_t(v01) * gx + int64_t(v11) * fx) * fy) / den);
    };
    ClimateSample out;
    out.temperature = mix(c00.temperature, c10.temperature, c01.temperature, c11.temperature);
    out.seasonality = mix(c00.seasonality, c10.seasonality, c01.seasonality, c11.seasonality);
    out.precipitation =
        mix(c00.precipitation, c10.precipitation, c01.precipitation, c11.precipitation);
    out.precipVariability = mix(c00.precipVariability, c10.precipVariability,
                                c01.precipVariability, c11.precipVariability);
    return out;
}

// ---------------------------------------------------------------------------
// Cave-lattice memo (performance only â€” provably cannot change any output).
//
// With the tile-raster memo above in place, `caveColumnFor` became the largest
// single term in column(): ~0.227 us/col, ~68% of the remaining cost. Almost
// all of it is the 34-70 hashes of the 4x4 node block, the 18 candidate edges
// and the sinkhole candidate â€” and caves.h now exposes those separately
// (`CaveLattice` / `caveLatticeFor`) because they depend ONLY on the lattice
// CELL (ci, cj), never on where in the cell the column sits. One cell is
// 25.6 m square = 65'536 voxel columns, so a sweep recomputes the identical
// block tens of thousands of times.
//
// Same direct-mapped, thread_local, never-reused-id scheme as the tile memo:
// a hit and a miss return bit-identical values (caveLatticeFor is pure), so
// determinism is untouched and no worldgen constant or iteration order moves.
// The key is (seed, ci, cj) â€” the seed, not the Amplifier id, because the
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
// Cavern candidate-corner memo (performance only â€” same argument as above).
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
// Cavern SITE memo â€” the one that actually matters, and the one caverns.h's
// own cost model missed.
//
// A CavernSite is a function of (seed, fi, fj) and the terrain surface at the
// site's own anchor xy: a per-SITE value. But it was being decoded once per
// COLUMN, for every column inside the site's ~36 m reach disc â€” on the order
// of 400'000 columns â€” and each decode calls `surfaceAt`, which in production
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


// Detail octave table v2 (worldgen-versioned constant, docs/determinism.md).
// latticeMm chosen so octaves nest across brick sizes; amplitudes in mm before
// scaling. Ordered COARSE to FINE â€” the split into the two bands below is by
// index, so the ordering is load-bearing.
//
// ---------------------------------------------------------------------------
// WHY v2 EXISTS, AND WHY v1's PARAMETERS WERE NEVER RIGHT FOR REAL TILES.
//
// v1 was tuned entirely against SyntheticTileSampler, whose own octave ladder
// already runs down to a 2-pixel (60 m) lattice. Real terrain-diffusion tiles
// are a 30 m/px raster with nothing below Nyquist, so the amplifier was never
// once asked to actually CONTINUE a spectrum â€” and it does not.
//
// Measured with vxc_terrainprobe against the real tiles, using the DETRENDED
// roughness S2(d) = mean |h(x+d) - 2h(x) + h(x-d)| (the plain structure
// function is swamped by the mean slope on sloped ground and reports a
// meaningless H = 1.0):
//
//   * the coarse raster is self-affine with H ~ 0.7-0.9 from 960 m down to
//     30 m, which is what natural terrain looks like;
//   * the v1 amplified surface has H ~ 1.4-1.8 between 0.1 m and 0.4 m â€”
//     SMOOTHER THAN LINEAR. That is the signature of running out of octaves:
//     below the 400 mm finest lattice the surface is an analytically smooth
//     bilinear ramp, with roughness at 0.1 m lag of 0.05 voxels.
//
// A locally-planar surface voxelised at 10 cm produces coherent contour
// terraces, and that is the whole artifact. On 1.7% ground v1 gave terrace
// runs averaging 8.8 voxels with 27% of a transect inside dead-flat runs of
// 2 m or more; on a 98% slope it gave a perfect 1:1 staircase. Same defect,
// two ranges â€” the smooth vista and the corduroy ground are one bug.
//
// For calibration: terrain-diffusion's own Minecraft integration
// (github.com/xandergos/terrain-diffusion-mc, ported from
// terrain_diffusion/inference/minecraft_api.py) solves the identical
// coarse->fine problem with the identical architecture we already had â€”
// bilinear upsample plus slope-gated fBm, nothing learned â€” but applies up to
// ~100 m + ~70 m of it at native scale. v1's ceiling was 2.856 m. We were
// running at roughly 2.5% of the reference implementation's amplitude.
//
// (The checkpoint's decoder_model/ is NOT an option here: it is an EDMUnet2D
// consistency decoder, 4-channel latents at 240 m/px -> the 1-channel
// Laplacian residual at 30 m/px, a fixed 8x latent-to-pixel stage. 30 m is the
// end of that cascade and no finer model exists. Everything below 30 m is
// procedural and always will be.)
// ---------------------------------------------------------------------------
struct Octave {
    int32_t latticeMm;
    int32_t amplitudeMm;
};
constexpr Octave kDetailOctaves[] = {
    // --- LANDFORM band: scaled by slopeScaleQ10, so it vanishes on flats.
    // These are hillside-shaping octaves; on a plain there should be no
    // hillside, and damping them there is correct.
    {25600, 2600},
    {6400, 1100},
    // --- MICRORELIEF band: scaled by microScaleQ10, which has a FLOOR.
    // This band is why v2 is not just "v1 with bigger numbers". Real ground
    // has centimetre-to-metre roughness â€” clods, tussocks, stones, rills â€”
    // that does not disappear because the ground is level, and at 10 cm voxels
    // flat ground is precisely where the terrace runs are LONGEST and the
    // artifact is worst. Upstream's slope gate drives detail to exactly zero
    // on flats; that is harmless at 1 m blocks and actively wrong at 10 cm, so
    // this is the one place we deliberately diverge from the reference.
    // The 200 mm lattice is the new floor of the cascade: two voxels, which is
    // the finest scale at which value noise is still a shape rather than
    // per-voxel static.
    // Energy is weighted toward the METRE scale rather than the finest
    // lattice on purpose. An earlier cut put more into the 200 mm octave and
    // it read in-engine as per-2-voxel static â€” the eye takes dense
    // uncorrelated jitter as noise, not as ground. Larger, smoother lumps
    // with quieter fine detail on top read as terrain.
    // v11: 190 -> 60 and 60 -> 15. MEASURED, and the measurement is the whole
    // reason. Each octave contributes a local gradient of roughly
    // amplitude/lattice, and at 10 cm voxels the 400 mm octave was contributing
    // 190/400 = 0.475 and the 200 mm octave 0.30 -- so the finest two alone put
    // ~0.8 of gradient into a band 2 to 4 voxels wide. A surface that steep at
    // voxel scale cannot be anything but a field of one-voxel steps.
    //
    // terrainprobe's terrace metrics say it plainly. Shipped v10 measured a mean
    // terrace run of 2.62 voxels (median 2) and 12,679 constant-height plateau
    // components in a 51 m square: the surface changed voxel height every 2.6
    // columns, which is uncorrelated at voxel scale, i.e. static. With these two
    // amplitudes cut the mean run is 3.81 and the component count 1,883.
    //
    // WHY NOT A UNIFORM SCALE, which was tried first. Scaling every octave to
    // 0.5x reaches a similar fragmentation (1,962 components) but puts 82.7% of
    // the area into flat plateaus of 4 m or more, against 64.0% here -- it buys
    // the same reduction in static by flattening the metre scale, which is the
    // terracing artifact this band exists to break up. Cutting the two octaves
    // that are actually too steep is strictly the better trade.
    {1600, 500},
    {400, 90},
    {200, 25},
};
constexpr uint32_t kDetailOctaveCount = sizeof(kDetailOctaves) / sizeof(kDetailOctaves[0]);

// --- BAND OWNERSHIP: the fine tier changes who owns 30 m -> 3.75 m (v10) ------
//
// The table above assumes the tile raster stops at 30 m, so everything below it
// is the client's to invent. With the baked `.vxtl` v2 fine tier that assumption
// is false: at 1.875 m/px the raster carries MEASURED landform -- routed
// drainage, incised channels, talus at the angle of repose -- down to its own
// Nyquist. Synthesising 25.6 m and 6.4 m octaves on top of that does not add
// detail, it fights measured landforms with hash noise, and the hash noise wins
// wherever it is larger. So in fine-tier mode those two octaves are DELETED and
// the ladder is continued from 3.2 m instead.
//
// WHY IT IS SAFE TO BRANCH WORLDGEN ON A PROPERTY OF THE DATA. Normally this
// would be exactly the hazard the whole determinism doctrine exists to prevent:
// two clients with different tiles computing different collision is a desync,
// not a visual pop. It is safe here because `pixelSizeMm()` is not "what this
// client happens to have downloaded" -- it is a property of the WORLD, pinned by
// provider_id in the session handshake and identical for every participant. A
// world is a 30 m world or a 1.875 m world for its whole life. The streaming
// layer's residency gate then blocks voxelisation until the fine tile is present
// rather than substituting a coarse guess, so no client ever evaluates the
// coarse ladder on a fine world. If that gate is ever weakened to a fallback,
// THIS BRANCH BECOMES A DESYNC -- which is why the two facts live next to each
// other in a comment rather than in two files.
constexpr Octave kFineDetailOctaves[] = {
    // --- SHAPING band: slope- and curvature-gated, vanishes on flats.
    // One octave, not two: 3.2 m is the first wavelength the 1.875 m raster
    // cannot resolve. PROVISIONAL amplitude -- 500 mm at 1.6 m continued down a
    // lambda^0.8 ramp gives 500 * 2^0.8 ~= 870, rounded to 900. The plan requires
    // this be set by probe measurement against the fine tier's measured S2, and
    // that measurement does not exist yet. Do not tune it by eye.
    {3200, 900},
    // --- MICRORELIEF band: floored, because decimetre roughness is a property
    // of the material and not of the gradient. Unchanged from the coarse table:
    // the fine tier does not reach these wavelengths, so nothing here is
    // duplicating baked data. Keeping 1600 in the FLOORED band (rather than
    // promoting it to the shaping band with 3200) is deliberate -- the metre
    // scale carrying energy on flat ground is the fix that stopped flat terrain
    // reading as long terrace runs at 10 cm voxels, and gating it on slope would
    // undo exactly that.
    {1600, 500},
    {400, 90},
    {200, 25},
};
constexpr uint32_t kFineDetailOctaveCount =
    sizeof(kFineDetailOctaves) / sizeof(kFineDetailOctaves[0]);
constexpr uint32_t kFineLandformOctaveCount = 1;

// --- THE DETAIL AMPLITUDE SCALE (v10.1) -------------------------------------
//
// One q10 multiplier over EVERY octave in both tables. It exists because the
// first in-engine look at v10 showed something no instrument had: at 2560x1440,
// settled, the ground reads as a dense field of isolated one-voxel blocks and
// pits -- per-voxel static, not terrain. The amplifier's own octave-table
// comment records this exact failure from an earlier cut ("it read in-engine as
// per-2-voxel static -- the eye takes dense uncorrelated jitter as noise, not as
// ground"), so this is a recurrence, not a novelty.
//
// It is a SCALE rather than a re-tuned table on purpose. The relative weighting
// between octaves was chosen deliberately (energy toward the metre scale, quiet
// at the finest lattice) and there is no evidence that shape is wrong; the
// evidence is that the whole band is too loud. A scale moves the level and
// leaves the shape alone, which also makes the bracket a single variable.
//
// It is not a free knob: it changes worldgen, so it is a compile-time constant
// under kWorldGenVersion and NOT a cvar. A cvar here would let two clients
// disagree about the ground.
//
// CORROBORATION, before the picture: the S2 calibration solved the fine ladder
// at a 231-413 mm envelope against the shipped 1650 mm -- 4 to 7 times too loud
// -- and was set aside because its target is anchored on an int16-METRE raster
// whose 0.707 m quantisation floor sits above the 0.515 m it was extrapolating
// to. That objection is still right about the TARGET and was wrong as a reason
// to leave the level alone: the measurement and the photograph now agree on the
// direction, and only disagree about how far.
constexpr int64_t kDetailAmplitudeScaleQ10 = 1024;
constexpr int64_t scaledAmpMm(int64_t amplitudeMm) {
    return amplitudeMm * kDetailAmplitudeScaleQ10 / 1024;
}

// The octave-table tripwires below pin exact derived totals, so they necessarily
// move when the scale does. They are guarded on the shipped scale rather than
// deleted: their job is to stop the TABLE changing by accident, and a deliberate
// bracket of the LEVEL is not that. At 1024 every one of them is live.
// True only in the SHIPPED configuration. It covers the additive terms as well
// as the scale because an ablation of either one legitimately moves the derived
// totals, and a tripwire that fires during a deliberate experiment teaches the
// experimenter to edit tripwires -- which is how a real regression gets waved
// through later.
constexpr bool kAmpUnscaled = (kDetailAmplitudeScaleQ10 == 1024) &&
                              (kRillMaxAbsMm == 300) && (kBeddingMaxAbsMm == 120);

// A tile raster at or below this pitch carries baked sub-30 m landform. 3750 is
// scale 8 and 1875 is scale 16; only 16 is generated today, but the threshold is
// written against the BAND the raster resolves rather than against one blessed
// scale, so a future 3.75 m tier does not need this line changed.
constexpr int64_t kFineTierMaxPixelMm = 3750;
constexpr bool isFineTier(int64_t pxMm) { return pxMm <= kFineTierMaxPixelMm; }

// --- THE BAND SPLIT, THREE BANDS AT v13 ------------------------------------
//
// By index into the tables above, which are ordered COARSE TO FINE, so the
// ordering is load-bearing in three places now instead of two.
//
//   [0, kLandformOctaveCount)          SHAPING   relief gate x full curvature
//   [.., n - kMaterialOctaveCount)     METRE     relief gate x half curvature
//   [n - kMaterialOctaveCount, n)      MATERIAL  half curvature ONLY
//
// WHY THE THIRD BAND EXISTS. v12 had two, split by which GATE they took, and
// the microrelief gate's 0.75 floor was doing two incompatible jobs: keeping
// decimetre roughness alive on flat ground (right) and keeping METRE-scale
// roughness alive on flat ground (wrong -- that is landform, and a plain has
// none). Measured, the second job is most of why a finished plain read as six
// times more ridged than a finished mountain: the 1.6 m octave at 0.75x is
// 375 mm of relief at a wavelength a 1.875 m posting resolves, on ground whose
// real relief over the same distance is a few tens of mm.
//
// So the 1.6 m octave moves under the landform gate with the rest of the
// ladder, and only 0.4 m and 0.2 m -- clods, stones, the material itself --
// stay ungated. Those two are below the resolution of every reference DTM this
// was calibrated against, which is exactly why they must NOT be conditioned on
// a landform statistic: nothing in the calibration can see them, and a term
// nothing can see must not be allowed to move with something that can.
constexpr uint32_t kLandformOctaveCount = 2;
constexpr uint32_t kMaterialOctaveCount = 2;
static_assert(kLandformOctaveCount + kMaterialOctaveCount < kDetailOctaveCount,
              "all three bands must be non-empty; evalSurface and the bound both split here");

// The divisor evalSurface applies to each octave's raw noise before scaling it
// by the octave amplitude. Named because Amplifier::surfaceUpperBoundMm's
// detail allowance is derived through the same constant.
constexpr int64_t kDetailNoiseScale = 32768;

// --- THE GATE THAT REPLACED slopeScaleQ10 AND microScaleQ10 (v13) -----------
//
// Both are GONE. They are not retuned, not narrowed, not floored differently:
// the quantity they were conditioned on was the wrong one, and no setting of a
// gradient gate can express what they were being asked to express. carrier.h's
// reliefScaleQ10 is the replacement and its derivation lives there, beside the
// measurement it is conditioned on.
//
// THE MEASUREMENT THAT KILLED THEM, in one line:
// docs/terrain-validation-2026-07.md measured this detail pass adding +207% to
// a plain's 1.875 m mean slope and +3.3% to alpine's -- a factor of 63 -- and
// the finished plain coming out six times more RIDGED than the finished
// mountain. slopeScaleQ10 spans 1.9x between those two sites and microScaleQ10
// spans 1.06x. The landform they sit on spans 25x. A gate cannot correct for a
// quantity it does not measure.
//
// WHAT REPLACED THEM IS ONE GATE OVER BOTH BANDS, not two. The two-gate split
// existed because the shaping band was supposed to vanish on flats while the
// microrelief band was not; with relief as the argument that distinction is
// already expressed -- relief is exactly zero on a plane and the gate's own
// floor is what keeps decimetre roughness alive there. Two curves over one
// argument would just be two calibrations to get wrong.
//
// The curvature gate is unchanged and still splits the two bands (full
// excursion on shaping, half on microrelief); see evalSurface.

// Final clamp on a surface elevation, in mm above sea level.
constexpr int32_t kSurfaceClampMinMm = -8'000'000;
constexpr int32_t kSurfaceClampMaxMm = 9'000'000;

// ---------------------------------------------------------------------------
// THE CARRIER now lives in voxelcore/carrier.h â€” kCarrierT, the three B-spline
// bases, evalCarrier, carrierSlopeMmPerM, the analytic curvature added for
// docs/terrain-amplification-plan.md Â§3c, and the whole derivation. It was
// moved out of this file verbatim (a pure move; worldgen output unchanged) so
// that the Phase-3 curvature work and its tests can reach it.
//
// What stayed here: ceilDivPos, which only the bound uses, and couplings
// (4b)/(4c) below, which are properties the BOUND needs from the carrier rather
// than properties of the carrier itself.
// ---------------------------------------------------------------------------

// Ceiling division for non-negative numerator and positive denominator. Used
// only by the bound, where every rounding must go outward.
constexpr int64_t ceilDivPos(int64_t a, int64_t b) { return (a + b - 1) / b; }

// ---------------------------------------------------------------------------
// Compile-time couplings for Amplifier::surfaceUpperBoundMm.
//
// The bound is only sound while a handful of properties of the code above hold.
// Each one below is either DERIVED from the table (so a tweak carries into the
// bound automatically) or STATIC_ASSERTED (so a tweak that the derivation
// cannot follow breaks the build instead of shipping invisible holes in the
// terrain). This block is the whole reason the bound lives in this file rather
// than in the consumer that needs it.
// ---------------------------------------------------------------------------

// (1) valueNoise2's output range. Its result is a convex combination of four
// hashToSigned16 values divided exactly by latticeMm^2, so it cannot leave that
// primitive's range; pin both ends of the primitive. Widening it (say to 17
// bits) would raise every octave's reach and silently invalidate (2).
static_assert(hashToSigned16(0ull) == -32768,
              "hashToSigned16's range moved; Amplifier::surfaceUpperBoundMm's detail "
              "allowance is derived from it.");
static_assert(hashToSigned16(~0ull) == 32767,
              "hashToSigned16's range moved; Amplifier::surfaceUpperBoundMm's detail "
              "allowance is derived from it.");
constexpr int64_t kNoiseMax = 32767;

// (2) Every amplitude is positive. The next step takes each octave's maximum at
// noise == +kNoiseMax, which assumes the amplitude does not flip the sign.
constexpr bool detailAmplitudesArePositive() {
    for (uint32_t i = 0; i < kDetailOctaveCount; ++i)
        if (kDetailOctaves[i].amplitudeMm <= 0) return false;
    return true;
}
static_assert(detailAmplitudesArePositive(),
              "A non-positive detail amplitude puts that octave's maximum at noise == "
              "-32768, not +32767; revisit Amplifier::surfaceUpperBoundMm.");

// (3) The largest `detailMm` evalSurface's octave loop can produce, evaluated in
// the SAME integer form and over the SAME table that loop uses. DERIVED, not
// mirrored: adding, removing or re-tuning an octave moves the bound with it.
// PER BAND, because evalSurface now scales the two bands by different factors
// and the bound must do the same or it is not a bound. `landform` selects
// which half of the table to sum; both are DERIVED from the same table and the
// same kLandformOctaveCount split that evalSurface's loop uses.
// v13: `band` is 0 shaping / 1 metre / 2 material, matching the split above and
// evalSurface's own loop.
constexpr uint32_t bandOf(uint32_t i, uint32_t n, uint32_t nLandform) {
    if (i < nLandform) return 0;
    return i < n - kMaterialOctaveCount ? 1u : 2u;
}
constexpr int64_t detailMaxMm(const Octave* tab, uint32_t n, uint32_t nLandform, uint32_t band) {
    int64_t sum = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (bandOf(i, n, nLandform) != band) continue;
        sum += kNoiseMax * scaledAmpMm(tab[i].amplitudeMm) / kDetailNoiseScale;
    }
    return sum;
}
constexpr int64_t kLandformMaxMm =
    detailMaxMm(kDetailOctaves, kDetailOctaveCount, kLandformOctaveCount, 0);
constexpr int64_t kMetreMaxMm =
    detailMaxMm(kDetailOctaves, kDetailOctaveCount, kLandformOctaveCount, 1);
constexpr int64_t kMicroMaxMm =
    detailMaxMm(kDetailOctaves, kDetailOctaveCount, kLandformOctaveCount, 2);
// Same derivation over the fine-tier table. The bound is selected at RUNTIME
// from the sampler's pitch, not maxed over both tiers: taking the max would keep
// the coarse ladder's 3.7 m landform allowance on a world that never evaluates
// it, and the whole point of deleting those octaves is that the envelope
// tightens -- which streaming feels as more effective trims.
constexpr int64_t kFineLandformMaxMm =
    detailMaxMm(kFineDetailOctaves, kFineDetailOctaveCount, kFineLandformOctaveCount, 0);
constexpr int64_t kFineMetreMaxMm =
    detailMaxMm(kFineDetailOctaves, kFineDetailOctaveCount, kFineLandformOctaveCount, 1);
constexpr int64_t kFineMicroMaxMm =
    detailMaxMm(kFineDetailOctaves, kFineDetailOctaveCount, kFineLandformOctaveCount, 2);
// NB there is deliberately no combined kDetailMaxMm: the three bands are scaled
// by DIFFERENT factors, so a single summed ceiling has no caller and clang's
// -Wunused-const-variable rejects it under -Werror. Sum the named constants at
// the point of use instead.

// (4) Tripwire. (3) keeps the bound SOUND across an octave-table edit on its
// own; this makes such an edit impossible to do ACCIDENTALLY without reading
// the derivation. Any change here also moves worldgen output â€” bump
// kWorldGenVersion and regenerate goldens.
static_assert(!kAmpUnscaled || (kDetailOctaveCount == 5 && kLandformMaxMm == 3698 &&
                                kMetreMaxMm == 499 && kMicroMaxMm == 113),
              "kDetailOctaves changed. Amplifier::surfaceUpperBoundMm derives its "
              "detail allowance from the table, so the bound is still sound -- but "
              "re-read its derivation before updating these numbers, and remember an "
              "octave change is a worldgen change (bump kWorldGenVersion).");
static_assert(!kAmpUnscaled || (kFineDetailOctaveCount == 4 && kFineLandformMaxMm == 899 &&
                                kFineMetreMaxMm == 499 && kFineMicroMaxMm == 113),
              "kFineDetailOctaves changed; same obligation as the coarse table above.");
// The envelope TIGHTENS on a fine world -- because two synthesised landform
// octaves were replaced by one. This is the plan's claim that the bound gets
// better, checked rather than asserted: if an edit ever makes the fine ladder
// the LOOSER of the two, the gating in (6) below is sized for the wrong table
// and the claim is stale.
static_assert(kFineLandformMaxMm + kFineMetreMaxMm + kFineMicroMaxMm <
                  kLandformMaxMm + kMetreMaxMm + kMicroMaxMm,
              "the fine-tier ladder is supposed to be a TIGHTER envelope than the "
              "coarse one; if it is not, re-read the band-ownership comment.");

// (4b) THE CARRIER IS A CONVEX COMBINATION OF ITS CONTROL POINTS. This single
// property is what lets surfaceBoundsMm bound the base term by a plain min/max
// over the control stencil, with no evaluation and no slack. It needs the
// B-spline weights to be (a) never negative and (b) to sum EXACTLY to the
// denominator, at every q10 fraction the evaluator can produce. Both are swept
// exhaustively at compile time rather than argued from the algebra, so that any
// future re-derivation of the basis â€” a different knot convention, a tweaked
// denominator, a Catmull-Rom substitution â€” fails the build instead of quietly
// unbounding the terrain.
//
// The two-stage evaluator composes the property: each stage-1 row value is a
// convex combination of that row's control points, and the stage-2 value is a
// convex combination of the rows, so the result lies within the stencil's
// min/max. Truncation cannot escape it either â€” for integer m <= v <= M,
// trunc(v) is still in [m, M], because trunc moves v toward zero and both
// endpoints are integers on the same side of it.
constexpr bool carrierWeightsAreConvex() {
    for (int64_t tq = 0; tq <= kCarrierT; ++tq) {
        const CarrierW4 w = carrierValueW(tq);
        int64_t sum = 0;
        for (int i = 0; i < 4; ++i) {
            if (w.w[i] < 0) return false;
            sum += w.w[i];
        }
        if (sum != kCarrierValueDen) return false;

        const CarrierW3 q = carrierSlopeW(tq);
        int64_t qsum = 0;
        for (int i = 0; i < 3; ++i) {
            if (q.w[i] < 0) return false;
            qsum += q.w[i];
        }
        if (qsum != kCarrierSlopeDen) return false;
    }
    return true;
}
static_assert(carrierWeightsAreConvex(),
              "The cubic/quadratic B-spline weights must be non-negative and sum exactly "
              "to their denominators; Amplifier::surfaceBoundsMm bounds the carrier by "
              "min/max over the control stencil, which is only valid for a convex "
              "combination.");

// (4c) The stage-1 and stage-2 products must not overflow int64. Control points
// are int32 millimetres at worst (kSurfaceClampMinMm..MaxMm is well inside
// that), and the weight sums are the two denominators above, so the worst
// product is bounded by |cp| * den. Asserted rather than trusted because the
// natural "obvious" formulation of this evaluator â€” weights in fx rather than
// q10 â€” overflows by ten orders of magnitude, and the next person to simplify
// it will reach for exactly that.
static_assert(int64_t(kSurfaceClampMaxMm) * kCarrierValueDen < (int64_t(1) << 62),
              "carrier stage product can overflow int64; do not widen the q10 fraction "
              "or the elevation range without redoing this bound.");

// (5) THE DETAIL GATE'S CEILING (v13). The bound multiplies both bands by
// kReliefScaleMaxQ10, so soundness needs exactly one property of
// reliefScaleQ10: that it can NEVER exceed that ceiling, for any input. That is
// proved exhaustively in carrier.h next to the function itself (its clamp is
// its last operation and the sweep covers the whole range where the result
// varies plus a saturating tail), so this file only has to record WHY it takes
// the ceiling rather than the footprint's own value, which is a property of the
// bound and not of the gate:
//
//   the argument is a SECOND DIFFERENCE of the raster at a 30 m baseline, and
//   the Lipschitz machinery in surfaceBoundsMm bounds FIRST differences. There
//   is no cheap footprint-wide bound on a second difference that is tighter
//   than the clamp, and inventing one is a second gating argument to get wrong
//   in the one place an error is a hole in the world.
//
// This is the same trade the curvature gate already takes -- see (6). Taking a
// gate at its clamp is normally a LOOSENING, and it would have been one here at
// slopeScaleQ10's old 4.0x ceiling: v13 also moves the microrelief band and the
// two additive terms under this gate, so a 4.0x ceiling would have taken the
// coarse envelope from 27.3 m to 30.7 m. kReliefScaleMaxQ10 is 2.0x instead
// (carrier.h explains why that costs no terrain), which takes it to 15.4 m --
// so the bound is TIGHTER than v12's even though it can no longer use the
// footprint's own value. That is the trade, stated in the two numbers rather
// than asserted.
static_assert(kReliefScaleMaxQ10 == 2048,
              "the relief gate's ceiling is what the surface bound multiplies every detail "
              "term by, so it is a term in the world's surface envelope and not a tuning "
              "knob. Moving it moves the bound; re-derive kDetailMaxAtMaxSlopeMm below.");
// The floor is the entire point of the gate not being a plain proportionality;
// assert it here as well as in carrier.h, because THIS is the file whose octave
// table comment explains what it defends against.
static_assert(reliefScaleQ10(0) > 0,
              "the detail ladder must not vanish on perfectly flat ground -- that is exactly "
              "where the voxel terrace artifact is worst. See kDetailOctaves.");

// (6) The detail allowance at full scale, per band, and a check that it stays a
// small number of metres (i.e. that the bound has not quietly become useless).
// v10: the shaping band is multiplied by TWO gates now (slope and curvature), so
// the allowance takes both maxima; and two additive terms join, each with its own
// compile-time envelope proved in its own header. Written in the same order and
// the same integer form as evalSurface computes it, so the bound tracks the code
// rather than paraphrasing it.
// The microrelief band's half-excursion gate ceiling, DERIVED from the shaping
// band's rather than written down, so the two cannot drift apart. evalSurface
// computes `1024 + (cScale - 1024) / 2`, which is maximised at cScale's own
// maximum; the truncating divide only ever lowers it, so this is an upper bound
// at every input.
constexpr int64_t kCurvatureScaleMicroMaxQ10 =
    1024 + (kCurvatureScaleMaxQ10 - 1024) / 2;
static_assert(kCurvatureScaleMicroMaxQ10 == 1408, "micro curvature ceiling moved");

// v13: ONE gate over both bands, and the two additive terms now take it too --
// written in exactly the order and the integer form evalSurface uses them, so
// the bound tracks the code rather than paraphrasing it.
constexpr int64_t detailAllowanceMm(int64_t landformMax, int64_t metreMax, int64_t microMax) {
    return landformMax * kReliefScaleMaxQ10 / 1024 * kCurvatureScaleMaxQ10 / 1024 +
           metreMax * kReliefScaleMaxQ10 / 1024 * kCurvatureScaleMicroMaxQ10 / 1024 +
           microMax * kCurvatureScaleMicroMaxQ10 / 1024 +
           (kRillMaxAbsMm + kBeddingMaxAbsMm) * kReliefScaleMaxQ10 / 1024;
}
constexpr int64_t kDetailMaxAtMaxSlopeMm =
    detailAllowanceMm(kLandformMaxMm, kMetreMaxMm, kMicroMaxMm);
constexpr int64_t kFineDetailMaxAtMaxSlopeMm =
    detailAllowanceMm(kFineLandformMaxMm, kFineMetreMaxMm, kFineMicroMaxMm);
// v13: 27289 -> 15356 coarse and 7696 -> 5559 fine. Both SHRANK, which is the
// direction the streaming layer feels as more effective trims; see coupling (5)
// for why that is true even though the bound can no longer use the footprint's
// own gate value.
static_assert(!kAmpUnscaled || (kDetailMaxAtMaxSlopeMm == 15310), "detail allowance moved");
static_assert(!kAmpUnscaled || (kFineDetailMaxAtMaxSlopeMm == 5513), "fine-tier detail allowance moved");
// The fine tier still buys a materially tighter envelope, but the margin is
// 2.76x at v13 rather than 3.8x -- because the coarse side came down by 1.8x
// and the fine side only by 1.4x, the two tiers' worst cases converged. Written
// as 2x rather than quietly re-fitted to 2.76 so that the assert keeps meaning
// "materially", which is what it is for.
static_assert(kFineDetailMaxAtMaxSlopeMm * 2 < kDetailMaxAtMaxSlopeMm,
              "the fine tier is supposed to buy a materially tighter surface bound");

// --- THE DETAIL GRADIENT CAP (v14) ------------------------------------------
//
// docs/measurements/client-detail-drainage-2026-07-29.txt is the brief. Each
// octave contributes a local gradient of roughly amplitude/lattice (the v11
// comment above used exactly this arithmetic to cut two octaves), and the
// post-gate v13 ladder sums to ~2.3 of gradient at full gates on ground whose
// own gradient is 0.4 -- so the downhill direction reversed almost everywhere,
// and a reversed downhill is a closed basin. Measured: the client took the
// alpine exemplar's carrier from 0 interior sinks and a 224 m mean flow path
// to 1625 sinks and 29 m; 87.9% of a 40% slope could not drain. Worse, the
// relief gate scales detail UP with relief, so the ladder was loudest exactly
// where the physical argument says it must be quietest.
//
// THE FIX IS ONE JOINT NORMALISATION, not per-octave retuning: the relative
// weighting between octaves was chosen deliberately (energy toward the metre
// scale, quiet at the finest lattice) and nothing measured says that shape is
// wrong; what is wrong is the summed gradient. So the summed post-gate detail
// is scaled down, once, so that its NOMINAL gradient obeys
//
//     detailGradient <= max(kDetailGradFloorMmPerM,
//                           kDetailGradCapKQ10 * carrierGradient / 1024)
//
// The nominal gradient is the same amplitude/lattice estimate per band, put
// through the very gates evalSurface applies (relief x curvature), so the cap
// tightens exactly where the gates get loud. It is an ESTIMATE of the typical
// local extreme, not a proved bound on the noise field's derivative; k is
// calibrated against the drainage instrument (vxc_terrainprobe raw pit
// census), which is the only metric that turned out to see this defect at all.
//
// THE FLOOR IS NOT OPTIONAL. On flat ground the carrier's gradient is ~0.017
// and a cap proportional to it would delete the decimetre roughness the
// microrelief band exists to provide -- the fix that stopped flat terrain
// reading as long terrace runs at 10 cm voxels. The floor is what that band
// keeps regardless of how flat the carrier is.
//
// THE TWO ADDITIVE TERMS (rill, bedding) ride under the same scale factor --
// the cap is one factor over the whole summed detail -- but do NOT enter the
// nominal-gradient estimate: neither is an fBm octave with a single lattice,
// each already carries its own gradient condition and its own proved envelope
// (|rill| <= 300 mm, |bedding| <= 120 mm, both relief-gated), and k is
// calibrated on the composite that includes them, so their contribution is
// inside the measurement rather than inside the formula.
//
// BOUND OBLIGATIONS, stated so the coupling block above stays a proof. The cap
// multiplies the summed detail by scale <= 1.0, applied only when it would
// shrink, with one truncating divide toward zero. |trunc(d * a / g)| <= |d|
// whenever 0 < a <= g, so the capped detail lies inside every envelope the
// uncapped detail lies inside: kDetailMaxAtMaxSlopeMm, the AbsMax trio and the
// derived tripwires above keep their v13 values, surfaceBoundsMm needs no new
// term and no extra truncation millimetre (truncation toward zero can only
// shrink a magnitude already inside the envelope). The bound's WORST CASE is
// unchanged rather than tightened: at the bound's max-gate corner the carrier
// slope that would license that much detail is not cheaply boundable over a
// footprint, so the bound keeps taking the cap at its ceiling of 1.0 -- the
// same trade couplings (5) and (6) already take for the gates. The TYPICAL
// envelope of the actual surface tightens (that is the whole point); the
// bound simply does not claim the credit.
constexpr int64_t detailGradMmPerM(const Octave* tab, uint32_t n, uint32_t nLandform,
                                   uint32_t band) {
    int64_t sum = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (bandOf(i, n, nLandform) != band) continue;
        sum += scaledAmpMm(tab[i].amplitudeMm) * 1000 / tab[i].latticeMm;
    }
    return sum;
}
constexpr int64_t kLandformGradMmPerM =
    detailGradMmPerM(kDetailOctaves, kDetailOctaveCount, kLandformOctaveCount, 0);
constexpr int64_t kMetreGradMmPerM =
    detailGradMmPerM(kDetailOctaves, kDetailOctaveCount, kLandformOctaveCount, 1);
constexpr int64_t kMicroGradMmPerM =
    detailGradMmPerM(kDetailOctaves, kDetailOctaveCount, kLandformOctaveCount, 2);
constexpr int64_t kFineLandformGradMmPerM =
    detailGradMmPerM(kFineDetailOctaves, kFineDetailOctaveCount, kFineLandformOctaveCount, 0);
constexpr int64_t kFineMetreGradMmPerM =
    detailGradMmPerM(kFineDetailOctaves, kFineDetailOctaveCount, kFineLandformOctaveCount, 1);
constexpr int64_t kFineMicroGradMmPerM =
    detailGradMmPerM(kFineDetailOctaves, kFineDetailOctaveCount, kFineLandformOctaveCount, 2);
// Tripwire, same doctrine as (4): DERIVED, so a table edit moves these with it;
// pinned, so a table edit cannot happen without reading this block. These are
// also the numbers hand-mirrored into worldgen.ush -- change them there too.
static_assert(!kAmpUnscaled || (kLandformGradMmPerM == 272 && kMetreGradMmPerM == 312 &&
                                kMicroGradMmPerM == 350),
              "coarse ladder nominal gradient moved; re-mirror worldgen.ush and re-run the "
              "drainage calibration (client-detail-drainage-2026-07-29.txt) before shipping");
static_assert(!kAmpUnscaled || (kFineLandformGradMmPerM == 281 && kFineMetreGradMmPerM == 312 &&
                                kFineMicroGradMmPerM == 350),
              "fine ladder nominal gradient moved; same obligation as the coarse trio");
// The measurement doc's headline mechanism, pinned: the coarse ladder at full
// gates carries ~2.3 of nominal gradient. If this stops being true the whole
// motivation for the cap should be re-read, not just the constant updated.
constexpr int64_t kDetailGradCeilMmPerM =
    kLandformGradMmPerM * kReliefScaleMaxQ10 / 1024 * kCurvatureScaleMaxQ10 / 1024 +
    kMetreGradMmPerM * kReliefScaleMaxQ10 / 1024 * kCurvatureScaleMicroMaxQ10 / 1024 +
    kMicroGradMmPerM * kCurvatureScaleMicroMaxQ10 / 1024;
static_assert(!kAmpUnscaled || kDetailGradCeilMmPerM == 2291,
              "the post-gate ladder's worst-case nominal gradient moved");

// k and the floors. Worldgen constants under kWorldGenVersion, NOT cvars -- a
// cvar here would let two clients disagree about the ground. All set by the
// calibration sweeps against the ten-site drainage ladder and the three baked
// fine exemplars; the sweep tables and the chosen arm's costs are in the v14
// measurement doc (docs/measurements/).
//
// k = 0.5: at 1.0 the capped ladder's nominal gradient EQUALS the carrier's
// and the downhill direction is only marginally protected -- measured, alpine
// read 63.8% stranded at an allowance equal to its own grade and 0.1% at half
// of it. Half also pays the estimate's known optimism (the fade curve's peak
// slope runs ~1.9x the amplitude/lattice mean this estimate sums).
//
// The floors sit at a measured KNEE, and the two tiers reach it for different
// reasons. COARSE (engaged ground only -- the ramp already exempts every flat
// class): the floor's only live effect is on engaged low-slope cells, i.e.
// mountain benches and hollow floors, where any allowance above the local
// grade ponds the very cells every hillside drains through. 100 -> 50 took
// the steepest exemplar from 40.7% to 17.5% stranded with no other row moving
// and the plains plateau untouched; 25 bought nothing further. A quiet hollow
// is also the physically right hollow -- colluvium is smooth; that is the
// curvature gate's own story. FINE (no exemption): the floor IS the flat-
// ground texture knob, and it is a straight trade -- the baked fine plains
// carrier alone measures 100% of its area in >= 4 m plateaus (mean terrace
// run 3.6 m: full corduroy; the bake's swales do not reach decimetre scale),
// v13's uncapped ladder gives 84.5% / 0.60 m but strands 97.3% of what the
// swales drain. At 50 the drainage residual is 1.3 points from the carrier's
// own (90.3% vs 89.0%) and lower floors erase the remaining texture to buy
// less than that. Not ~0, deliberately: the last step to zero costs the whole
// remaining anti-terrace band and recovers nothing measurable.
constexpr int64_t kDetailGradCapKQ10 = 512;     // 0.5 x carrier gradient
constexpr int64_t kDetailGradFloorMmPerM = 50;  // engaged minimum, coarse tier
constexpr int64_t kFineDetailGradFloorMmPerM = 50;
static_assert(kDetailGradCapKQ10 > 0 && kDetailGradCapKQ10 <= 1024,
              "k above 1.0 licenses detail steeper than the ground it decorates, which is "
              "the defect this cap exists to remove");
static_assert(kDetailGradFloorMmPerM > 0 && kFineDetailGradFloorMmPerM > 0,
              "a zero floor deletes the ladder outright as slope tends to zero on engaged "
              "ground; at 10 cm voxels that is the terrace artifact, back");
static_assert(kDetailGradFloorMmPerM < kDetailGradCeilMmPerM &&
                  kFineDetailGradFloorMmPerM < kDetailGradCeilMmPerM,
              "a floor at or above the ladder's own ceiling makes the cap a no-op");

// THE ENGAGEMENT RAMP, and why a constant floor could not do the floor's job.
// The drainage ladder (docs/measurements/drainage-ladder-v13-2026-07-29.txt)
// splits the COARSE world by carrier grade: above ~25% the carrier is
// perfectly drained and client detail destroys it -- cap here; below ~12% the
// carrier cannot drain at 10 cm postings regardless, detail is the only thing
// helping (plains carrier 99.0% stranded at 10 cm -> 97.5% with detail), and
// the decimetre texture is the anti-terrace fix -- do not touch it.
//
// Those two jobs CANNOT be expressed as max(gradFloor, k * carrierGradient)
// with a constant floor, and this was measured rather than argued: the plains
// exemplar's post-gate ladder gradient is ~408 mm/m (material-band dominated),
// so keeping its texture needs an allowance >= ~410 at 17 mm/m of slope, while
// fixing the 20-25% rows needs an allowance <= ~200 at ten times the slope.
// The required allowance is NON-MONOTONE in slope; no (k, floor) pair
// produces that shape. The sweep that established it: floor 100 fixes the
// steep half of the ladder (alpine 87.9% -> 0.0% stranded) and takes the
// plains plateau fraction from 79.3% to 98.9% (the terrace artifact, back);
// floor 350+ keeps the plateau and leaves every row from 20% grade up broken.
// Exempting the material band instead (one obvious alternative) fails the
// other way: with the two gated bands capped hard and the material band left
// at 1.0x, alpine still strands 78.7% -- the decimetre band IS the pit field.
//
// THE RAMP ARGUMENT IS LANDFORM RELIEF, NOT SLOPE, and that too was measured
// rather than reasoned into. A slope ramp was tried first and it fixed every
// uniform class while leaving the two sites with incised valleys broken
// (steepest 77.3%, third-class 67.0% stranded, against 26.0%/45.7% under the
// unconditional cap): drainage is NON-LOCAL, every hillside's water exits
// through a valley floor, and a slope-local exemption grants the outlet
// itself full-amplitude detail -- a dam at the one place a dam strands the
// whole catchment. relief30 tells a true plain (665 mm at the plains
// exemplar; nothing within 30 m to drain into) from a valley floor (walls
// inside the 30 m baseline read thousands of mm), which is exactly the
// distinction the exemption needs, and it is the SAME per-cell slot.reliefMm
// the v13 amplitude gate already reads -- no new seam class is introduced
// (the gate itself shipped per-cell at v13, and the ramp is linear between
// its ends rather than stepped). For uniform grades relief30 ~= grade x 30 m,
// so the ramp ends below are the ladder's own 12% and 25% thresholds in
// relief currency: 3600 mm and 7500 mm.
//
// ON A FINE-TIER WORLD THE CAP ENGAGES EVERYWHERE, with no exemption. The
// flat exemption exists because the coarse carrier has NOTHING below 30 m and
// synthetic decimetre texture is the only thing breaking up a dead-flat
// plain. The baked fine tier carves sub-metre swales on exactly that ground
// (BAKE_VERSION 4, profile_regional_p = 2.0): its plains carrier drains at
// 19.1 m mean paths where the coarse one managed 4 cm, and the uncapped v13
// ladder DEGRADES that to 3.7 m (94-97% stranded on all three baked
// exemplars). Where the bake supplies structure at the client's own scales,
// the client's job is to stay out of its way -- the plan's "complement, do
// not add", showing up as a measurement. Tier-specific behaviour is house
// style, not a compromise: kFineDetailOctaves and carrierCurvatureTierNormQ10
// exist for the same reason.
constexpr int64_t kDetailCapReliefLoMm = 3600;
constexpr int64_t kDetailCapReliefHiMm = 7500;
static_assert(kDetailCapReliefLoMm > 0 && kDetailCapReliefLoMm < kDetailCapReliefHiMm,
              "the ramp must have positive width; its width divides");
static_assert(reliefScaleQ10(kDetailCapReliefLoMm) < kReliefScaleMaxQ10,
              "the ramp's low end should sit below the relief gate's saturation, or the "
              "exemption never varies where the gate does");

// ---------------------------------------------------------------------------
// Couplings for the MIRROR bounds: Amplifier::surfaceLowerBoundMm and
// Amplifier::solidBelowBoundMm. Same doctrine as (1)-(6) above -- derive from
// the real tables where possible, static_assert where the derivation cannot
// follow -- but the stakes are higher on this side. A too-high surface UPPER
// bound merely tracks a chunk that turned out to be empty; a too-high all-SOLID
// floor claims a chunk is solid when it is not, and the streaming layer then
// never tracks it at all. That is an unreachable cave, or a player digging into
// a chunk the world does not know exists.
// ---------------------------------------------------------------------------

// (7) The detail term's magnitude on the NEGATIVE side. (3) derives the maximum
// at noise == +kNoiseMax; the minimum is at noise == -32768, whose magnitude is
// one LSB larger, so the upper bound's allowance is NOT a valid magnitude bound
// for the lower side. Rather than mirror the asymmetry and invite an off-by-one
// in the one place an off-by-one is a hole in the world, take the trivially
// sound envelope: |noise| <= kDetailNoiseScale, so each octave's contribution
// is at most its own amplitude. DERIVED from the same table as (3).
constexpr int64_t detailAbsMaxMm(const Octave* tab, uint32_t n, uint32_t nLandform,
                                 uint32_t band) {
    int64_t sum = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (bandOf(i, n, nLandform) != band) continue;
        sum += scaledAmpMm(tab[i].amplitudeMm);
    }
    return sum;
}
constexpr int64_t kLandformAbsMaxMm =
    detailAbsMaxMm(kDetailOctaves, kDetailOctaveCount, kLandformOctaveCount, 0);
constexpr int64_t kMetreAbsMaxMm =
    detailAbsMaxMm(kDetailOctaves, kDetailOctaveCount, kLandformOctaveCount, 1);
constexpr int64_t kMicroAbsMaxMm =
    detailAbsMaxMm(kDetailOctaves, kDetailOctaveCount, kLandformOctaveCount, 2);
constexpr int64_t kFineLandformAbsMaxMm =
    detailAbsMaxMm(kFineDetailOctaves, kFineDetailOctaveCount, kFineLandformOctaveCount, 0);
constexpr int64_t kFineMetreAbsMaxMm =
    detailAbsMaxMm(kFineDetailOctaves, kFineDetailOctaveCount, kFineLandformOctaveCount, 1);
constexpr int64_t kFineMicroAbsMaxMm =
    detailAbsMaxMm(kFineDetailOctaves, kFineDetailOctaveCount, kFineLandformOctaveCount, 2);
static_assert(kLandformAbsMaxMm >= kLandformMaxMm && kMetreAbsMaxMm >= kMetreMaxMm &&
                  kMicroAbsMaxMm >= kMicroMaxMm,
              "the symmetric detail envelope must cover the positive-side maximum too, or "
              "surfaceLowerBoundMm is looser than surfaceUpperBoundMm in the wrong direction");
static_assert(!kAmpUnscaled || (kLandformAbsMaxMm == 3700 && kMetreAbsMaxMm == 500 &&
                                kMicroAbsMaxMm == 115),
              "detail amplitude sum moved; see (4)");
static_assert(kFineLandformAbsMaxMm >= kFineLandformMaxMm &&
                  kFineMetreAbsMaxMm >= kFineMetreMaxMm && kFineMicroAbsMaxMm >= kFineMicroMaxMm,
              "same obligation as the coarse pair, on the fine-tier table");
static_assert(!kAmpUnscaled || (kFineLandformAbsMaxMm == 900 && kFineMetreAbsMaxMm == 500 &&
                                kFineMicroAbsMaxMm == 115),
              "fine-tier detail amplitude sum moved; see (4)");

// (8) The cave family's carve depth, in the QUERYING COLUMN'S OWN depth space.
// caveCarveAt tests `depthMm < segs[s].depthMm + sqrt(marginSq)`, so the
// deepest reachable voxel is the deepest tube axis plus the widest radius; a
// crevice is emitted as an ordinary segment whose depth extends the axis by at
// most its downward reach. Sinkhole shafts bypass the roof clamp but are capped
// by the same caveNode depth ceiling, so they are covered by the axis term.
constexpr int64_t kMaxCaveCarveBelowSurfaceMm = kCaveNodeDepthMinMm + kCaveNodeDepthSpanMm +
                                                kCaveRadiusMaxMm + kCrevDownMinMm + kCrevDownSpanMm;
static_assert(kMaxCaveCarveBelowSurfaceMm == 42800,
              "cave/crevice depth envelope moved; Amplifier::solidBelowBoundMm derives its "
              "carve allowance from these constants.");

// (9) The cavern chain's carve depth, in the SITE'S OWN depth space. The chain
// is: anchor at caveNode's depth below the site surface, then (childCount - 1)
// downward steps, then the flat-floor clamp below the last room's centre.
//
// The FLOOR CLAMP is what bounds the bottom, not the room radius: cavernCarveAt
// refuses any voxel with `zAbs < sg.zFloorMm` before it ever evaluates the
// ellipsoid, and zFloorMm is `zMm - floorDropMm`. A 40 m vertical semi-axis
// therefore does NOT reach 40 m below the room centre -- it is cut off at
// floorDropMm. Getting this wrong in the safe direction would cost 25 m of
// bound for nothing; getting it wrong in the unsafe direction is a hole, which
// is why the adversarial test sweeps real cavern columns against it rather than
// trusting this comment.
constexpr int64_t kMaxCavernCarveBelowSiteSurfaceMm =
    kCaveNodeDepthMinMm + kCaveNodeDepthSpanMm +
    (kCavernChildCount - 1) * (kCavernStepDownMinMm + kCavernStepDownSpanMm) +
    kCavernFloorDropMinMm + kCavernFloorDropSpanMm;
static_assert(kMaxCavernCarveBelowSiteSurfaceMm == 91000,
              "cavern chain depth envelope moved; Amplifier::solidBelowBoundMm derives its "
              "carve allowance from these constants.");

// Every hashed field above is drawn from a half-open range, so Min + Span is a
// STRICT upper bound rather than an attainable value. Pinned at both the
// all-ones and all-zeros draw so a change to the field decode -- say to an
// inclusive range -- breaks the build rather than eating the one millimetre of
// slack these bounds rely on.
static_assert(cavernHashField10(~0ull, 0, kCavernStepDownSpanMm) < kCavernStepDownSpanMm,
              "cavern step-down field must stay half-open");
static_assert(cavernHashField10(~0ull, 0, kCavernFloorDropSpanMm) < kCavernFloorDropSpanMm,
              "cavern floor-drop field must stay half-open");
static_assert(cavernHashField10(0ull, 0, kCavernFloorDropSpanMm) == 0,
              "cavern floor-drop field must start at zero");
static_assert(caveNodeFromHash(~0ull, 0, 0).depthMm < kCaveNodeDepthMinMm + kCaveNodeDepthSpanMm,
              "caveNode depth field must stay half-open: both the tube envelope (8) and the "
              "cavern anchor envelope (9) are capped by it.");

// (10) The envelope solidBelowBoundMm actually subtracts: the deeper of the
// two, since both are measured below a surface that the dilated lower bound
// bounds from below.
constexpr int64_t kDeepestCarveBelowSurfaceMm =
    kMaxCaveCarveBelowSurfaceMm > kMaxCavernCarveBelowSiteSurfaceMm
        ? kMaxCaveCarveBelowSurfaceMm
        : kMaxCavernCarveBelowSiteSurfaceMm;
static_assert(kDeepestCarveBelowSurfaceMm == 91000, "carve envelope moved");

// (11) Independent backstop, and a genuine one rather than a restatement. Both
// carve passes refuse `depthMm + kCaveBedrockMarginMm >= bedrockDepthMm`, and
// bedrockDepthMm is drawn from [kBedrockDepthMinMm, +40 m]. So NOTHING can
// carve deeper than kBedrockDepthMinMm - kCaveBedrockMarginMm below its own
// column's surface, by a mechanism entirely separate from the chain geometry in
// (8) and (9). If the geometry-derived envelope ever exceeded this, one of the
// two derivations would be wrong -- and this catches it at compile time.
constexpr int64_t kBedrockDepthMinMm = 180000;
constexpr int64_t kBedrockDepthJitterMm = 40000;

// --- topsoil (worldgen v8) -------------------------------------------------
//
// The v6 formula was `clamp(300 + precipU8 * 8 - slopeMmPerPx / 4, 0, 2500)`
// and it had two independent faults, the second much worse than the first.
//
//   1. `precipU8 * 8` read the raw wire byte with no scale, so it meant
//      something different on synthetic tiles (byte centred on 128) than on
//      real ones (physical WorldClim quantization). Fixed by decoding to
//      mm/yr first, via climate.h.
//
//   2. `- slopeMmPerPx / 4` subtracted an ABSOLUTE depth, and at any realistic
//      30 m-pixel slope it simply swamped the base. This was the dominant
//      fault and it was NOT a real-tile problem -- measured with
//      vxc_climateprobe, topsoil came out ZERO on 85.5% of synthetic land and
//      91.3% of real land. The formula was broken on the very encoding it was
//      written for; the encoding mismatch only made it worse. Because
//      stratigraphyAt returns col.surfaceMat only while depth < topsoilMm,
//      zero topsoil means the biome material is never visible: the in-engine
//      -VoxelMatHistogram measured ROCK 15% / SUBSOIL 85% and not one surface
//      material across 2M quads.
//
// So erosion is now a FRACTION RETAINED, not a depth removed. It is
// dimensionless, so it cannot be swamped by a units mismatch again, and it
// scales the way soil loss actually does -- with how much is there.
//
// The retention floor is tied to kBiomeCliffSlopeMmPerPx so soil reaches its
// thinnest exactly where the cliff gate takes over and paints BARE_ROCK. One
// constant, two consumers: the "bare soil-less ground that is not classified
// as a cliff" state is unrepresentable by construction.
constexpr int64_t kTopsoilBaseMm = 200;
constexpr int64_t kTopsoilMmPerMetreOfRain = 200; // +200 mm of soil per m/yr of rain
constexpr int64_t kTopsoilSlopeRetentionMinQ10 = 128; // 12.5% retained at the cliff angle
// The FLOOR IS LOAD-BEARING, and it is applied after the jitter. The topmost
// voxel's depth below the surface is in [0, kVoxelSizeMm) by construction, so
// topsoilMm >= kVoxelSizeMm guarantees at least one visible biome-coloured
// voxel on every column, everywhere. Clamping before the jitter (the v6 order)
// would let a -25% draw take 100 mm to 75 mm and break that guarantee silently
// on the flattest ground, which is exactly where it is most visible.
constexpr int64_t kTopsoilMinMm = kVoxelSizeMm;
constexpr int64_t kTopsoilMaxMm = 2500;
static_assert(kTopsoilMinMm >= kVoxelSizeMm,
              "topsoil must be at least one voxel deep or the biome surface material can "
              "never appear in the world");
static_assert(kDeepestCarveBelowSurfaceMm < kBedrockDepthMinMm - kCaveBedrockMarginMm,
              "the geometric carve envelope must stay inside the bedrock clamp; if it does "
              "not, either (8)/(9) or the bedrock depth range is wrong.");

// (13) THE 3D DENSITY BAND'S ENVELOPE (voxelcore/density3.h, new at
// kWorldGenVersion 12). stratigraphyAt no longer tests `centre <= surfaceMm`;
// it tests `centre <= surfaceMm + D`, and D is what every surface-derived bound
// in this file now has to widen by.
//
// The whole widening is ONE constant, in both directions, and that is the
// point of the term having a compile-time envelope at all. density3.h proves
// |D| <= kDensity3MaxAbsMm by static_assert on the sum of its two components,
// so nothing here has to re-derive geometry the way couplings (8) and (9) do
// for the carve passes -- there is no hashed field to bound, no chain to walk,
// no worst-case draw to argue about. Both directions are ATTAINED (density3.h
// asserts equality at both extremes with the gates fully open), so this is the
// tight envelope and not a conservative one.
//
// It is asserted rather than used directly so that a change to the split
// between the bedding and pocket allocations -- which density3.h permits, as a
// redistribution -- cannot silently widen the world's bounds without the
// number in front of someone.
static_assert(kDensity3MaxAbsMm == 350,
              "the 3D density envelope moved; surfaceBoundsMm, GeneratedWorld's surface brick "
              "range and amplifier.h's air-reason enumeration are all widened by exactly this "
              "constant, and a bound that did not follow it is a hole in the world.");
// The envelope is 3.5 voxels, not a whole number of them, so every consumer
// that works in voxels rounds UP -- see density3BandVoxels, which is the single
// place that rounding lives so it cannot be done two different ways.
constexpr int64_t kDensity3EnvelopeVoxels =
    (kDensity3MaxAbsMm + kVoxelSizeMm - 1) / kVoxelSizeMm;
static_assert(kDensity3EnvelopeVoxels == 4,
              "3D density envelope is 3.5 voxels, covered by 4 either side");
static_assert(kDensity3EnvelopeVoxels * kVoxelSizeMm >= kDensity3MaxAbsMm,
              "the voxel envelope must cover the millimetre one");

// (14) The lithology gate's argument. density3RockGateQ is fed the depth of
// SOIL ABOVE ROCK, and stratigraphyAt's own layer model decides what that is:
// under a BARE_ROCK surface the subsoil band reads MAT_ROCK straight through
// (see stratigraphyAt below), so the only soil is the topsoil band; under
// anything else both bands are soil. Written as a function next to the layer
// model it is derived from rather than inlined at the call site, because the
// two must move together -- if stratigraphyAt ever stops reading rock through
// the subsoil band, this is wrong and nothing else would say so.
int64_t soilAboveRockMm(const ColumnSample& col) {
    return col.surfaceMat == MAT_ROCK ? int64_t(col.topsoilMm)
                                      : int64_t(col.topsoilMm) + int64_t(col.subsoilMm);
}

// (14b) WHAT THE LITHOLOGY GATE ACTUALLY SELECTS, pinned so it cannot drift.
//
// density3.h promotes the lithology gate to the whole displacement at v12, and
// its comment claims the continuous ramp resolves in practice to "bare rock
// only". That claim is a COUPLING between three constants in this file and two
// in density3.h, not a property of either, so it is asserted here where the
// three live rather than described there where it would rot.
//
// The thinnest soil a NON-rock column can carry is the topsoil floor plus the
// subsoil derived from it, and subsoil is `topsoil * 2 + 500`:
constexpr int64_t kThinnestNonRockSoilMm = kTopsoilMinMm * 2 + 500 + kTopsoilMinMm;
static_assert(kThinnestNonRockSoilMm >= kDensity3RockSoilZeroMm,
              "the 3D density band's lithology gate is supposed to be shut on every column "
              "that is not BARE_ROCK -- that is what keeps an undercut nose from cutting a "
              "face across a soil profile and exposing MAT_SUBSOIL where the biome material "
              "should be. It holds because the thinnest possible non-rock column already "
              "carries kDensity3RockSoilZeroMm of soil. If kTopsoilMinMm, the subsoil formula "
              "or that constant move, this stops being true SILENTLY: re-read density3.h's "
              "lithology gate section and decide deliberately, do not just widen the assert.");
// And the mirror-image half: on a rock cliff the gate must be OPEN, or the term
// is inert everywhere and the whole band is cost with no output. Slope
// retention pins topsoil at its floor on any column steep enough to pass the
// slope gate, and a MAT_ROCK column's soil is topsoil alone.
static_assert(kTopsoilMinMm <= kDensity3RockSoilFullMm,
              "a bare-rock cliff must open the lithology gate FULLY; if the topsoil floor "
              "rises past kDensity3RockSoilFullMm the band starts fading out on exactly the "
              "ground it exists for");

// (12) The dilation radius. A cavern site only reaches columns within
// kCavernMaxReachMm of its anchor (cavernColumnFromSites culls on exactly
// kCavernMaxReachSqMm), so bounding the surface over the footprint dilated by
// this radius bounds every site that can carve into the footprint. In voxels,
// rounded UP.
constexpr int64_t kCavernReachVoxels =
    (kCavernMaxReachMm + kVoxelSizeMm - 1) / kVoxelSizeMm;
static_assert(kCavernReachVoxels * kVoxelSizeMm >= kCavernMaxReachMm,
              "cavern reach must round UP in voxels, or the dilated footprint can miss the "
              "site whose surface the bound is derived from.");

} // namespace

uint64_t Amplifier::nextId() {
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1; // 0 means "empty slot"
}

// The surface half of column(): the bilinear tile base plus the slope-scaled
// detail octaves, and the two by-products (tile pixel, tile slope) the rest of
// column() needs. Factored out ONLY so the cavern pass's `surfaceAt` callback
// can be the very same function the querying column's own surfaceMm came from
// â€” caverns.h's contract for it â€” bit-identically, by construction rather than
// by a comment asking two copies to stay in step.
Amplifier::SurfaceEval Amplifier::evalSurface(int64_t vx, int64_t vy) const {
    const int64_t xMm = vx * kVoxelSizeMm;
    const int64_t yMm = vy * kVoxelSizeMm;
    const int64_t pxMm = tiles_->pixelSizeMm();

    // C2 carrier over the tile raster (exact integer math). The control stencil
    // for the cell containing this column is px-1..px+2 on each axis â€” four
    // control points per axis rather than bilinear's two, which is what buys
    // gradient continuity across the cell boundary.
    const int64_t px = floorDiv(xMm, pxMm), py = floorDiv(yMm, pxMm);
    const int64_t fx = xMm - px * pxMm, fy = yMm - py * pxMm;
    // One per-cell read for all three things derived from the raster: the
    // (v13: prefiltered) control points, and the cell's landform relief.
    const StencilSlot& slot = cachedStencilSlot(id_, *tiles_, px, py);
    const CarrierEval carrier = evalCarrier(slot.cp, fx, fy, pxMm);
    const int64_t baseMm = carrier.heightMm;

    // The carrier's ANALYTIC gradient, in mm per metre, conditions both detail
    // amplitude and soil depth.
    //
    // v8 used tileSlopeMmPerPx: a forward difference of the cell's own corners,
    // and therefore CONSTANT over the whole cell. That fed slopeScaleQ10, whose
    // range is 0.25x..4.0x, so the same noise field was multiplied by a
    // step-discontinuous gain on each side of every pixel line â€” the ground
    // changed character at an invisible boundary even where height and slope
    // were continuous. Measured on real tiles at v8, the detail envelope stepped
    // by a median of 150-310 mm per boundary (p90 770-1075, max 2302): one to
    // three voxels of texture amplitude appearing along a dead-straight 30 m
    // line, as the TYPICAL case.
    //
    // The spline derivative is continuous, so that step is now structurally
    // impossible rather than merely small.
    //
    // v13: this NO LONGER CONDITIONS DETAIL AMPLITUDE -- see the gate block
    // above kDetailOctaves. It is still the slope classifyBiome, the topsoil
    // retention term and density3 take, all of which are genuinely questions
    // about the local GRADE (can soil stay on it, is it a cliff) rather than
    // about how much landform there is.
    const int64_t slopeMmPerM = carrierSlopeMmPerM(carrier, pxMm);

    // Two bands, accumulated separately because they take different scales
    // (see kDetailOctaves). Each band is summed at full precision and scaled
    // ONCE, rather than scaling per octave, so the truncation happens in one
    // place per band and the bound has exactly two terms to account for.
    // v10: the SHAPING band also takes a curvature gate. Convex crests roughen,
    // concave hollows smooth toward colluvial fill. This is the ridge-sharp /
    // valley-smooth asymmetry that makes ground read as SHAPED rather than
    // TEXTURED -- the v9 probe measured curvature-conditioned roughness at
    // 0.98-1.03, i.e. the 1.0 a stationary fBm gives, meaning v9's detail was
    // conditioned on nothing at all.
    //
    // It gates the shaping band only, NOT the microrelief band, for exactly the
    // reason microScaleQ10 has a floor: decimetre roughness is a property of the
    // material, not of the local geometry. A hollow collects colluvium and its
    // METRE-scale shape smooths; its clods and stones do not disappear.
    // Normalised to the 30 m reference tier before gating -- see the derivation
    // above kCurvatureKneeQ10. Without this the gate is a no-op at 30 m and
    // hard-clipped at 1.875 m, which is not two settings of one gate, it is two
    // different failures.
    const int64_t curveQ10 = carrierCurvatureTierNormQ10(
        carrierCurvatureMmPerM2Q10(evalCarrierCurvature(slot.cp, fx, fy, pxMm), pxMm), pxMm);
    const int64_t cScale = curvatureScaleQ10(curveQ10);

    const bool fine = isFineTier(pxMm);
    const Octave* tab = fine ? kFineDetailOctaves : kDetailOctaves;
    const uint32_t nOct = fine ? kFineDetailOctaveCount : kDetailOctaveCount;
    const uint32_t nLand = fine ? kFineLandformOctaveCount : kLandformOctaveCount;

    // THE LEVEL OF THE WHOLE LADDER, from the landform the raster measurably
    // carries rather than from the local gradient. See carrier.h's relief
    // block for why the argument changed and what it is measured against.
    const int64_t rScale = reliefScaleQ10(slot.reliefMm);
    int64_t landformMm = 0, metreMm = 0, microMm = 0;
    for (uint32_t i = 0; i < nOct; ++i) {
        const Octave& o = tab[i];
        // Faded, not raw: raw valueNoise2 puts a visible dead-straight crease
        // on every lattice line of every octave. See hash.h.
        //
        // The channel is indexed by the octave's position in ITS OWN table, so
        // the two tiers do not share noise fields octave-for-octave. That is
        // deliberate: they are different worlds (different provider_id), never
        // two renderings of one world, so there is nothing to keep consistent
        // and pretending otherwise would tie the fine ladder's channel choice to
        // the coarse table's length.
        const int64_t term =
            valueNoise2Fade(seed_, xMm, yMm, o.latticeMm, CH_DETAIL_OCTAVE_BASE + i) *
            scaledAmpMm(o.amplitudeMm) / kDetailNoiseScale;
        const uint32_t band = bandOf(i, nOct, nLand);
        if (band == 0)
            landformMm += term;
        else if (band == 1)
            metreMm += term;
        else
            microMm += term;
    }
    // The microrelief band takes HALF the curvature gate's excursion.
    //
    // The first cut gated only the shaping band, and the probe reported a
    // convex/concave detail ratio of 0.99 at every lag from 0.1 m to 1.6 m --
    // apparently no effect. The gate was in fact working perfectly; the METRIC
    // could not see it. The shaping band is 25.6 m and 6.4 m octaves, whose
    // second difference over a 0.1 m lag is ~1e-5 of their amplitude, so
    // everything the probe measures at those lags is microrelief. This is the
    // same trap recorded at the top of this file for the v8 gain step, which
    // also hid inside a band the estimator was not looking at.
    //
    // Fixing the metric alone would have been the wrong answer, because the
    // plan's intent -- ground that reads as SHAPED rather than TEXTURED -- is
    // about what the eye sees underfoot, which is exactly the metre-and-below
    // band. So the gate has to reach it.
    //
    // HALF, not all, and for a physical reason rather than as a hedge: a hollow
    // genuinely does smooth at decimetre scale, because it collects colluvium
    // and fine sediment, but the material's own texture -- clods, stones -- does
    // not disappear with the local geometry the way metre-scale shape does. Half
    // the excursion keeps the [0.75, 1.375] range comfortably above the floor
    // that microScaleQ10 exists to defend, so hollows cannot go quiet enough to
    // bring back the terrace runs that floor was added to break up.
    const int64_t cScaleMicro = 1024 + (cScale - 1024) / 2;
    // Three bands, three gates. The MATERIAL band takes no relief gate at all --
    // see the band-split block above kDetailOctaves: it is the ground's own
    // texture and does not scale with how much landform it is draped over.
    int64_t detailMm = landformMm * rScale / 1024 * cScale / 1024 +
                       metreMm * rScale / 1024 * cScaleMicro / 1024 +
                       microMm * cScaleMicro / 1024;

    // Two ADDITIVE structured terms, each with its own gate and its own proved
    // envelope. They are added after the band scaling rather than folded into a
    // band because neither is an fBm octave: the rill term is anisotropic and
    // conditioned on the gradient direction, and the bedding term is
    // quasi-periodic and conditioned on a regional structural field.
    //
    // v13: they DO take the relief gate, which they did not take the slope gate.
    // That is not an inconsistency, it is the same correction: the reason to
    // keep them out of the old gate was that each already had a GRADIENT
    // condition of its own and multiplying by a second gradient gate would
    // double-count it. Neither has a LANDFORM condition, and the measurement
    // that motivated this whole change -- 0.3-0.6 m of client roughness laid on
    // a plain that has 6.7 m of relief -- includes these two terms; leaving them
    // ungated would leave a 420 mm floor under a ladder whose whole point is
    // that it comes down to nothing on flat ground.
    const int64_t addMm = rillMm(seed_, xMm, yMm, carrier.sxMmPerPx * 1000 / pxMm,
                                 carrier.syMmPerPx * 1000 / pxMm) +
                          beddingMm(seed_, xMm, yMm, baseMm);
    detailMm += addMm * rScale / 1024;

    // v14: THE DETAIL GRADIENT CAP -- one scale-down over the whole summed
    // detail. See the derivation block above kDetailGradCapKQ10. The nominal
    // gradient is the per-band amplitude/lattice sum through the SAME gates,
    // in the SAME order and integer form, as the detail sum itself, so the
    // estimate and the thing it estimates cannot drift apart. Every quantity
    // here is non-negative except detailMm itself, whose divide truncates
    // toward zero -- truncDiv in the HLSL mirror, never floorDiv.
    const int64_t gradLand = fine ? kFineLandformGradMmPerM : kLandformGradMmPerM;
    const int64_t gradMetre = fine ? kFineMetreGradMmPerM : kMetreGradMmPerM;
    const int64_t gradMicro = fine ? kFineMicroGradMmPerM : kMicroGradMmPerM;
    const int64_t detailGradMmPerM = gradLand * rScale / 1024 * cScale / 1024 +
                                     gradMetre * rScale / 1024 * cScaleMicro / 1024 +
                                     gradMicro * cScaleMicro / 1024;
    // Engagement first: full on a fine world, ramped on the coarse one --
    // exactly zero below the ramp so flat coarse classes are bit-for-bit v13,
    // saturating to full above it. The branches keep every divide's numerator
    // non-negative.
    int64_t engageQ10 = 1024;
    if (!fine) {
        if (slot.reliefMm >= kDetailCapReliefHiMm) {
            engageQ10 = 1024;
        } else if (slot.reliefMm > kDetailCapReliefLoMm) {
            engageQ10 = (slot.reliefMm - kDetailCapReliefLoMm) * 1024 /
                        (kDetailCapReliefHiMm - kDetailCapReliefLoMm);
        } else {
            engageQ10 = 0;
        }
    }
    if (engageQ10 > 0) {
        const int64_t gradFloor = fine ? kFineDetailGradFloorMmPerM : kDetailGradFloorMmPerM;
        int64_t allowedGradMmPerM = kDetailGradCapKQ10 * slopeMmPerM / 1024;
        if (allowedGradMmPerM < gradFloor) allowedGradMmPerM = gradFloor;
        if (detailGradMmPerM > allowedGradMmPerM) {
            // Full-engagement scale, then blended toward 1.0 by the ramp. The
            // truncation in the blend rounds the scale UP (less capping), so
            // the result stays in (capQ10, 1024] -- never zero, never above
            // one -- and the divide below is the only signed one: truncDiv in
            // the HLSL mirror, never floorDiv.
            const int64_t capQ10 = allowedGradMmPerM * 1024 / detailGradMmPerM;
            const int64_t scaleQ10 = 1024 - engageQ10 * (1024 - capQ10) / 1024;
            detailMm = detailMm * scaleQ10 / 1024;
        }
    }

    SurfaceEval s;
    s.px = px;
    s.py = py;
    s.slopeMmPerM = slopeMmPerM;
    s.surfaceMm = clampi32(baseMm + detailMm, kSurfaceClampMinMm, kSurfaceClampMaxMm);
    return s;
}

int32_t Amplifier::surfaceMm(int64_t vx, int64_t vy) const { return evalSurface(vx, vy).surfaceMm; }

// ---------------------------------------------------------------------------
// The sky-band trim's proof obligation. DERIVATION â€” read this before touching
// evalSurface above.
//
// evalSurface computes, for a column at (xMm, yMm) = (vx, vy) * kVoxelSizeMm:
//
//     surfaceMm = clampi32(base(x,y) + detail(x,y), MIN, MAX)
//
// so an upper bound on surfaceMm over a footprint is the clamp applied to
// (an upper bound on base) + (an upper bound on detail) over that footprint.
// Both sub-bounds and the composition rest on one property used repeatedly:
//
//     C++ integer division by a POSITIVE divisor is truncation toward zero,
//     and truncation toward zero is a NONDECREASING function of the real
//     quotient.  (x <= y  =>  trunc(x) <= trunc(y): both signs, and mixed
//     signs via trunc(x) <= 0 <= trunc(y).)
//
// That is what lets every step below bound the exact rational quantity and then
// truncate, rather than having to carry rounding slack around. It is also why
// applying clampi32/clampi64 to a bound yields a bound: clamping is monotone.
//
// (a) BASE â€” bounded EXACTLY, not conservatively.
//     base is the bilinear interpolation of the four elevations of the tile
//     pixel cell the column falls in. Restricted to one cell it is a bilinear
//     patch, which is LINEAR along each axis with the other held fixed, so its
//     maximum over any axis-aligned sub-rectangle is attained at a CORNER of
//     that sub-rectangle. We therefore clip the footprint to each pixel cell it
//     touches and evaluate that cell's own bilinear form at the four clipped
//     corners, via the very function evalSurface uses. Taking the largest of
//     those is the exact maximum of base over the footprint's bounding
//     rectangle â€” a superset of the discrete columns in it, hence still a
//     bound, with no sampling-density argument anywhere.
//
//     This matters: taking the largest pixel CORNER elevation instead would
//     also be valid but badly loose whenever the footprint is small relative to
//     a 30 m pixel (the level-0..3 chunks), because the nearest corner can be
//     tens of metres of relief away from any ground the footprint contains.
//
// (b) DETAIL â€” bounded by the amplitude sum times the footprint's OWN slope
//     scale. detail = (sum over octaves of trunc(noise_i * amp_i / 32768))
//     scaled by trunc(* sScale / 1024). noise_i is a valueNoise2, whose range
//     is pinned at compile time to [-32768, 32767] via hashToSigned16 â€” see
//     the numbered block near kDetailOctaves. Each octave's term is therefore
//     at most trunc(32767 * amp_i / 32768) (amplitudes are asserted positive),
//     and kLandformMaxMm / kMicroMaxMm are exactly those sums per band,
//     computed from the same table evalSurface loops over.
//
//     sScale is slopeScaleQ10 of the slope at the pixel cell the column falls
//     in; every such cell is in the corner grid we just read, so the footprint's
//     maximum slope is available EXACTLY, and slopeScaleQ10 is asserted
//     nondecreasing, so the max-slope cell gives the max scale. Using the
//     footprint's own slope rather than the global worst case takes the
//     allowance from 11.42 m to typically 1-3 m, which is the difference
//     between the trim binding at level 4 and not.
//
//     Note the bound does NOT evaluate a single hash: it is cheaper than one
//     column, which is what makes it usable per chunk on the streaming path.
//
// (c) The two sub-bounds are summed and clamped. Sound because each is an upper
//     bound on its term at EVERY column of the footprint simultaneously (they
//     need not be attained at the same column) and clamping is monotone.
//
// (d) THE 3D DENSITY BAND (kWorldGenVersion 12). What the callers of these
//     bounds actually need is not a bound on surfaceMm any more -- it is a
//     bound on the surface stratigraphyAt tests against, which is surfaceMm + D
//     with |D| <= kDensity3MaxAbsMm. So the constant is added to the upper
//     bound and subtracted from the lower one, AFTER the clamp (see the code).
//     That is the entire re-derivation: unlike the detail terms there is no
//     gate to take at its ceiling, no table to sum and no tier to select,
//     because density3.h's envelope is proved at compile time and is
//     independent of position, slope, curvature and pixel size alike.
//
//     It is worth being explicit that this is the LOOSEST possible statement of
//     the term and is still cheap: D is zero on ~93% of columns and inside the
//     +/-700 mm band on the rest, so on nearly every footprint the true
//     excursion is zero and the bound gives away 700 mm anyway. Making it
//     footprint-dependent would mean evaluating the slope gate over the
//     footprint, which the Lipschitz machinery above could do -- and it would
//     buy 0.7 m against a measured 8.5 m of existing slack at level 0. Not
//     worth a second gating argument to get wrong.
//
// It declines (kSurfaceBoundDeclined) rather than guess whenever it has no
// information: no tile raster, a degenerate pixel size, an empty footprint, or
// a footprint so large it would need an unbounded number of corner reads.
// ---------------------------------------------------------------------------
// Shared body of surfaceUpperBoundMm and surfaceLowerBoundMm. ONE traversal
// producing BOTH bounds, so the two can never disagree about which cells the
// footprint touches, which corners it reads, or what the maximum slope is --
// the failure mode a second copy of this loop would eventually have.
//
// Returns false when it declines; the two public wrappers turn that into their
// own (deliberately different) sentinels.
bool Amplifier::surfaceBoundsMm(int64_t vx0, int64_t vy0, int64_t vx1, int64_t vy1,
                                int64_t& outLowerMm, int64_t& outUpperMm) const {
    if (vx1 < vx0 || vy1 < vy0) return false; // empty footprint
    const int64_t pxMm = tiles_ ? int64_t(tiles_->pixelSizeMm()) : 0;
    if (pxMm <= 0) return false;

    // Footprint bounding rectangle in mm. Inclusive of both end columns: an
    // amplifier column at index vx sits exactly at vx * kVoxelSizeMm.
    const int64_t x0Mm = vx0 * kVoxelSizeMm, x1Mm = vx1 * kVoxelSizeMm;
    const int64_t y0Mm = vy0 * kVoxelSizeMm, y1Mm = vy1 * kVoxelSizeMm;

    // THE CONTROL GRID. The cells the columns fall in are cx0..cx1; a cubic
    // B-spline on cell c reads control points c-1..c+2, so the grid the bound
    // must see is dilated by one on the low side and two on the high side. That
    // dilation is why kSurfaceBoundMaxCornersPerAxis had to grow in v9; the
    // PIXEL SIZE is why it had to grow again for the 1.875 m fine tier. Both
    // derivations live on the constant in amplifier.h -- note that nx/ny below
    // are bounded by the footprint, not by the cap, so the cap's value costs
    // nothing until it is the thing that declines.
    const int64_t cx0 = floorDiv(x0Mm, pxMm), cx1 = floorDiv(x1Mm, pxMm);
    const int64_t cy0 = floorDiv(y0Mm, pxMm), cy1 = floorDiv(y1Mm, pxMm);
    const int64_t px0 = cx0 - 1, py0 = cy0 - 1;
    const int64_t nx = (cx1 - cx0 + 1) + 3, ny = (cy1 - cy0 + 1) + 3;
    if (nx > kSurfaceBoundMaxCornersPerAxis || ny > kSurfaceBoundMaxCornersPerAxis) return false;

    // v13: WHAT THIS GRID HOLDS IS CONTROL POINTS, WHICH ON A SAMPLE TIER ARE
    // NO LONGER THE RASTER. Everything below -- the first-difference scan that
    // gives the Lipschitz constant and the centre carrier evaluation -- is a
    // statement about the spline's own control lattice, so on a tier that gets
    // prefiltered the grid has to be prefiltered too. Reading raw elevations
    // here and bounding a prefiltered carrier with them is not conservative in
    // either direction: the prefilter is a SHARPENING filter, so it can grow a
    // first difference by up to 2.98x per axis, and a bound derived from the
    // unsharpened lattice would be too small. That is a hole in the world.
    //
    // The alternative -- keep the raw grid and multiply the Lipschitz constant
    // by the filter's gain -- is sound but 8.9x looser on every footprint, which
    // would take the sky-band trim out entirely. Prefiltering the grid costs one
    // more pass over it and no extra sampler reads beyond the halo.
    const bool prefilter = carrierPrefiltersSamples(pxMm);
    const int64_t halo = prefilter ? kCarrierPrefilterRadius : 0;
    const int64_t rnx = nx + 2 * halo, rny = ny + 2 * halo;
    if (rnx > kSurfaceBoundMaxCornersPerAxis || rny > kSurfaceBoundMaxCornersPerAxis)
        return false;

    // Read the raster once (through the same per-thread tile memo column()
    // uses); everything below is derived from this grid.
    int64_t elev[kSurfaceBoundMaxCornersPerAxis * kSurfaceBoundMaxCornersPerAxis];
    {
        // The haloed raw grid is a thread_local scratch rather than a second
        // 32 KB stack array, which is exactly the escape hatch
        // kSurfaceBoundMaxCornersPerAxis's own comment nominates for growing
        // past ~128 KB of frame. It is written before it is read on every path
        // that reads it, so it carries no state between calls.
        static thread_local int64_t raw[kSurfaceBoundMaxCornersPerAxis *
                                        kSurfaceBoundMaxCornersPerAxis];
        int64_t* src = prefilter ? raw : elev;
        for (int64_t iy = 0; iy < rny; ++iy)
            for (int64_t ix = 0; ix < rnx; ++ix)
                src[ix + rnx * iy] =
                    cachedElevationMm(id_, *tiles_, px0 - halo + ix, py0 - halo + iy);
        if (prefilter) {
            // Separable, in the same two stages and the same integer form
            // carrierPrefilterStencil uses, so the grid holds exactly the
            // control points evalSurface's stencil holds. Stage 1 collapses y
            // into a (rnx x ny) intermediate written back over `raw`'s own rows,
            // which is safe because row iy reads only rows iy..iy+2R and writes
            // row iy -- strictly behind the read window.
            for (int64_t iy = 0; iy < ny; ++iy)
                for (int64_t ix = 0; ix < rnx; ++ix) {
                    int64_t acc = kCarrierPrefilterW[0] * raw[ix + rnx * (iy + halo)];
                    for (int64_t n = 1; n <= halo; ++n)
                        acc += kCarrierPrefilterW[n] * (raw[ix + rnx * (iy + halo + n)] +
                                                        raw[ix + rnx * (iy + halo - n)]);
                    raw[ix + rnx * iy] = acc / kCarrierPrefilterDen;
                }
            for (int64_t iy = 0; iy < ny; ++iy)
                for (int64_t ix = 0; ix < nx; ++ix) {
                    int64_t acc = kCarrierPrefilterW[0] * raw[(ix + halo) + rnx * iy];
                    for (int64_t m = 1; m <= halo; ++m)
                        acc += kCarrierPrefilterW[m] * (raw[(ix + halo + m) + rnx * iy] +
                                                        raw[(ix + halo - m) + rnx * iy]);
                    // The SAME clamp carrierPrefilterStencil applies, or the
                    // grid is not the lattice the carrier evaluates.
                    elev[ix + nx * iy] = clampi64(acc / kCarrierPrefilterDen,
                                                  -kCarrierControlClampMm, kCarrierControlClampMm);
                }
        }
    }

    // A LIPSCHITZ BOUND AROUND ONE CENTRE EVALUATION, rather than a hull.
    //
    // The obvious v9 bound is "min/max over the control stencil", which the
    // convex-combination property (coupling (4b)) makes trivially sound. It is
    // also far too loose to ship: a 3.2 m level-0 chunk sits inside ONE 30 m
    // cell, and the hull of that cell's 4x4 stencil spans 90 m of terrain, so
    // in mountains the bound would sit tens of metres off the actual surface
    // and the sky-band trim would stop paying. v8's bilinear bound did not have
    // this problem because a bilinear patch is planar, so evaluating the
    // clipped footprint corners was exact.
    //
    // Instead, use the gradient the carrier already provides. The spline's
    // partial derivatives are convex combinations of control-point first
    // differences (coupling (4b) again, for the quadratic basis), so
    //
    //     |dS/dx| <= maxAbsDx / pxMm      |dS/dy| <= maxAbsDy / pxMm
    //
    // over the whole grid, and therefore for any point p in the footprint and
    // the footprint's centre c:
    //
    //     |S(p) - S(c)| <= |dS/dx| * |px - cx| + |dS/dy| * |py - cy|
    //                   <= maxAbsDx/pxMm * W/2 + maxAbsDy/pxMm * H/2
    //
    // One carrier evaluation plus one scan of first differences. Tighter than
    // the hull by roughly the ratio of footprint size to cell size, O(1) in the
    // footprint area, and it degrades gracefully: on genuinely steep ground the
    // slack grows, which is the safe direction.
    int64_t maxAbsDx = 0, maxAbsDy = 0;
    for (int64_t iy = 0; iy < ny; ++iy)
        for (int64_t ix = 0; ix + 1 < nx; ++ix) {
            const int64_t d = elev[(ix + 1) + nx * iy] - elev[ix + nx * iy];
            const int64_t a = d < 0 ? -d : d;
            if (a > maxAbsDx) maxAbsDx = a;
        }
    for (int64_t iy = 0; iy + 1 < ny; ++iy)
        for (int64_t ix = 0; ix < nx; ++ix) {
            const int64_t d = elev[ix + nx * (iy + 1)] - elev[ix + nx * iy];
            const int64_t a = d < 0 ? -d : d;
            if (a > maxAbsDy) maxAbsDy = a;
        }

    // +1 per axis for the truncation in each of the evaluator's two divisions,
    // which rounds toward zero and so could otherwise shave the magnitude of a
    // derivative the bound is claiming to dominate.
    const int64_t lipDx = maxAbsDx + 1, lipDy = maxAbsDy + 1;

    // Carrier at the footprint centre. Rounded down; the half-voxel that
    // rounding can move the centre is absorbed by taking the half-extents
    // rounded UP below.
    const int64_t cxMm = floorDiv(x0Mm + x1Mm, 2), cyMm = floorDiv(y0Mm + y1Mm, 2);
    const int64_t cpx = floorDiv(cxMm, pxMm), cpy = floorDiv(cyMm, pxMm);
    // Through the SAME per-cell stencil evalSurface uses, so the centre value
    // this bound is built around is bit-identically the value a column at the
    // centre would compute -- including the v13 prefilter. Building a second
    // 4x4 read here is what would let the two drift.
    const int64_t centreMm =
        evalCarrier(cachedStencilSlot(id_, *tiles_, cpx, cpy).cp, cxMm - cpx * pxMm,
                    cyMm - cpy * pxMm, pxMm)
            .heightMm;

    // Half-extents, rounded up, plus one mm each for the centre rounding above.
    const int64_t halfW = (x1Mm - x0Mm + 1) / 2 + 1, halfH = (y1Mm - y0Mm + 1) / 2 + 1;
    const int64_t carrierSlackMm =
        ceilDivPos(lipDx * halfW, pxMm) + ceilDivPos(lipDy * halfH, pxMm);

    const int64_t maxBaseMm = centreMm + carrierSlackMm;
    const int64_t minBaseMm = centreMm - carrierSlackMm;

    // v13: THE DETAIL GATE IS TAKEN AT ITS CEILING, NOT AT THE FOOTPRINT'S OWN
    // VALUE. The gate's argument is now the raster's SECOND difference at a
    // 30 m baseline (coupling (5)), and the Lipschitz machinery above bounds
    // first differences; there is no cheaper sound bound on the argument than
    // its clamp. This is the same trade the curvature gate has always taken and
    // it is now the loosest step in the derivation for BOTH bands.
    //
    // It does not widen the world's envelope: kReliefScaleMaxQ10 is exactly the
    // 4096 slopeScaleQ10 used to clamp to, so kDetailMaxAtMaxSlopeMm is a
    // WORST-CASE number that has not grown. What it costs is the typical case,
    // where v12 could feed a gentle footprint's own small slope and get a
    // tighter allowance. The adversarial sweep on real tiles is what says
    // whether that matters, and it is reported rather than assumed.
    //
    // (a) The tier still selects which octave table's maxima apply -- at
    // runtime, from the sampler's pitch, exactly as evalSurface selects it.
    // (b) The two additive terms now take the gate as well, because evalSurface
    // does; their own envelopes are still constants.
    const bool fineTier = isFineTier(pxMm);
    const int64_t landMax = fineTier ? kFineLandformMaxMm : kLandformMaxMm;
    const int64_t metrMax = fineTier ? kFineMetreMaxMm : kMetreMaxMm;
    const int64_t micrMax = fineTier ? kFineMicroMaxMm : kMicroMaxMm;
    const int64_t landAbs = fineTier ? kFineLandformAbsMaxMm : kLandformAbsMaxMm;
    const int64_t metrAbs = fineTier ? kFineMetreAbsMaxMm : kMetreAbsMaxMm;
    const int64_t micrAbs = fineTier ? kFineMicroAbsMaxMm : kMicroAbsMaxMm;
    const int64_t maxDetailMm = detailAllowanceMm(landMax, metrMax, micrMax);
    // The negative side uses the symmetric envelope (coupling (7)), and takes
    // one further millimetre PER TRUNCATION in the q10 divides, which round
    // toward zero and so would otherwise shave the magnitude. v13 counts six:
    // two on the shaping band, two on the metre band, one on the material band,
    // one on the additive pair's own gate divide. One millimetre each is the
    // right price for not having to argue about it.
    const int64_t minDetailMm = detailAllowanceMm(landAbs, metrAbs, micrAbs) + 6;

    // THE 3D DENSITY BAND, ADDED AFTER THE CLAMP AND NOT BEFORE IT.
    //
    // evalSurface clamps its own output to [kSurfaceClampMinMm,
    // kSurfaceClampMaxMm], so `clampi64(base +/- detail, MIN, MAX)` bounds
    // surfaceMm exactly as it did before v12. What stratigraphyAt now tests
    // against is surfaceMm + D, and D is applied AFTER that clamp -- the
    // displaced surface genuinely can sit 700 mm outside the clamped range, so
    // folding the envelope in before clamping would bound the wrong quantity
    // and would be too tight by up to 700 mm at exactly the two elevations
    // where the clamp bites. Sound because clamping is monotone and D is
    // bounded independently of position: for every column and every z,
    //     surfaceMm + D <= clampi64(...) + kDensity3MaxAbsMm.
    //
    // See coupling (13). No slope, curvature or tier argument is needed: unlike
    // every other term in this sum the envelope is a constant, and the gates
    // can only ever shrink it toward zero.
    outUpperMm =
        clampi64(maxBaseMm + maxDetailMm, kSurfaceClampMinMm, kSurfaceClampMaxMm) +
        kDensity3MaxAbsMm;
    outLowerMm =
        clampi64(minBaseMm - minDetailMm, kSurfaceClampMinMm, kSurfaceClampMaxMm) -
        kDensity3MaxAbsMm;
    return true;
}

int64_t Amplifier::surfaceUpperBoundMm(int64_t vx0, int64_t vy0, int64_t vx1,
                                       int64_t vy1) const {
    int64_t lo = 0, hi = 0;
    if (!surfaceBoundsMm(vx0, vy0, vx1, vy1, lo, hi)) return kSurfaceBoundDeclined;
    return hi;
}

int64_t Amplifier::surfaceLowerBoundMm(int64_t vx0, int64_t vy0, int64_t vx1,
                                       int64_t vy1) const {
    int64_t lo = 0, hi = 0;
    if (!surfaceBoundsMm(vx0, vy0, vx1, vy1, lo, hi)) return kSurfaceLowerBoundDeclined;
    return lo;
}

// See the contract and the full derivation on the declaration in amplifier.h.
// The whole proof is two lines here because every hard part is a compile-time
// constant in the coupling block above, which is exactly where it belongs.
// True if ANY cavern site that ANY column in the rect could possibly see has
// its anchor within carving reach of the rect. Conservative: it returns true
// whenever it is not certain of the answer, and true is the safe direction (it
// forces the caller onto the deeper, more pessimistic carve envelope).
//
// Exactness argument. cavernColumnFromSites reduces column (vx, vy) against
// exactly the four candidates of that column's OWN coarse cell, and culls each
// on `dxySq > kCavernMaxReachSqMm` measured from the column to the anchor. So
// enumerating the candidates of every coarse cell the rect's columns fall in is
// a superset of every site any column in the rect can see, and testing each
// anchor against the rect's nearest point is a lower bound on that per-column
// distance. If the nearest point is out of reach, no column in the rect is.
//
// Cost: four hashes per coarse cell, memoised per (seed, si, sj) in the same
// table the cave pass uses. No surface read, no site decode -- the gate bit and
// the node jitter come out of one hash, and the depth safety window is a
// comparison. A cell is 204.8 m, so a level-0 chunk footprint touches one.
bool Amplifier::cavernMayReachFootprint(int64_t vx0, int64_t vy0, int64_t vx1, int64_t vy1) const {
    const int64_t x0Mm = vx0 * kVoxelSizeMm, x1Mm = vx1 * kVoxelSizeMm;
    const int64_t y0Mm = vy0 * kVoxelSizeMm, y1Mm = vy1 * kVoxelSizeMm;
    const int64_t si0 = floorDiv(x0Mm, kCavernCoarseMm), si1 = floorDiv(x1Mm, kCavernCoarseMm);
    const int64_t sj0 = floorDiv(y0Mm, kCavernCoarseMm), sj1 = floorDiv(y1Mm, kCavernCoarseMm);

    // A rect spanning more coarse cells than this is not worth enumerating;
    // say "yes" and let the caller take the pessimistic envelope. A level-4
    // chunk (51.2 m) against a 204.8 m cell can straddle at most 2 per axis.
    constexpr int64_t kMaxCoarseCellsPerAxis = 4;
    if (si1 - si0 + 1 > kMaxCoarseCellsPerAxis || sj1 - sj0 + 1 > kMaxCoarseCellsPerAxis)
        return true;

    for (int64_t sj = sj0; sj <= sj1; ++sj) {
        for (int64_t si = si0; si <= si1; ++si) {
            const CavernCandidates& cands = cachedCavernCandidates(seed_, si, sj);
            for (int32_t k = 0; k < 4; ++k) {
                const CavernCandidate& c = cands.corners[k];
                if (!c.open) continue;
                // Nearest point of the rect to this anchor, per axis.
                const int64_t ex = c.node.xMm < x0Mm   ? x0Mm - c.node.xMm
                                   : c.node.xMm > x1Mm ? c.node.xMm - x1Mm
                                                       : 0;
                const int64_t ey = c.node.yMm < y0Mm   ? y0Mm - c.node.yMm
                                   : c.node.yMm > y1Mm ? c.node.yMm - y1Mm
                                                       : 0;
                if (ex * ex + ey * ey <= kCavernMaxReachSqMm) return true;
            }
        }
    }
    return false;
}

// v12 note: the 3D density band needs NO term of its own here. Both tiers
// derive from surfaceLowerBoundMm, which is now a bound on the DISPLACED
// surface (it already has kDensity3MaxAbsMm subtracted), and the carve
// envelopes below are measured in each column's own depth space from that same
// surface. So the floor drops by exactly 700 mm and the enumeration in
// amplifier.h stays closed -- reason 1 already covers the band, which is why it
// is written there as 1b rather than as a fourth subtracted constant.
int64_t Amplifier::solidBelowBoundMm(int64_t vx0, int64_t vy0, int64_t vx1, int64_t vy1) const {
    if (vx1 < vx0 || vy1 < vy0) return kSurfaceLowerBoundDeclined;

    // TIER 1, the common case by a wide margin. No cavern site is in reach, so
    // the only carvers left are tunnels, crevices and shafts -- all bounded in
    // the querying column's OWN depth space. That means no dilation is needed
    // either, and the tight lower bound over the footprint itself is valid.
    //
    // This tier is the whole reason the bound is useful at playable depths. The
    // cavern envelope is 91 m against the cave family's 42.8 m, and caverns are
    // rare: on the test corpus, 756 cavern columns against ~100k sampled. A
    // single flat 91 m envelope would only ever skip chunks more than 91 m
    // down, which at a 60 m anchor depth is a small cap at the bottom of the
    // sight sphere; 42.8 m clears most of it.
    if (!cavernMayReachFootprint(vx0, vy0, vx1, vy1)) {
        const int64_t tightMm = surfaceLowerBoundMm(vx0, vy0, vx1, vy1);
        if (tightMm == kSurfaceLowerBoundDeclined) return kSurfaceLowerBoundDeclined;
        return tightMm - kMaxCaveCarveBelowSurfaceMm;
    }

    // TIER 2. A cavern site is in reach, so dilate by that reach BEFORE
    // bounding: the chain is anchored at absolute z derived from the surface at
    // its OWN anchor, which is a different column from any in the footprint.
    //
    // No min() against tier 1's value is needed: the dilated lower bound is
    // never above the tight one and the cavern envelope is never shallower than
    // the cave one, so this branch is unconditionally the more conservative of
    // the two.
    const int64_t lowerMm = surfaceLowerBoundMm(vx0 - kCavernReachVoxels, vy0 - kCavernReachVoxels,
                                                vx1 + kCavernReachVoxels, vy1 + kCavernReachVoxels);
    if (lowerMm == kSurfaceLowerBoundDeclined) return kSurfaceLowerBoundDeclined;

    return lowerMm - kDeepestCarveBelowSurfaceMm;
}

ColumnSample Amplifier::column(int64_t vx, int64_t vy) const {
    const SurfaceEval s = evalSurface(vx, vy);
    const int64_t px = s.px, py = s.py;
    const int64_t slopeMmPerM = s.slopeMmPerM;
    const int64_t pxMmC = tiles_->pixelSizeMm();
    const int64_t xMmC = vx * kVoxelSizeMm, yMmC = vy * kVoxelSizeMm;

    // CLIMATE, FADED-BILINEAR RATHER THAN NEAREST-PIXEL (v9).
    //
    // v8 read the single pixel the column fell in. classifyBiome's inputs were
    // therefore piecewise constant on the 30 m grid, so surfaceMat and topsoil
    // depth changed in hard 30 m squares -- the third of the three mechanisms
    // that made the tile grid visible, and the one that shows up as blocky
    // material patches rather than as a lighting crease.
    //
    // Interpolating the CHANNELS (not the classification) is the right level to
    // fix it: the gates stay hard, which is correct -- a real treeline or a real
    // shoreline is a sharp boundary -- but they now sit on smooth iso-curves of
    // the climate field instead of on pixel edges.
    //
    // Quintic-faded weights, not raw bilinear: the same argument as the detail
    // octaves (hash.h). Raw bilinear leaves a gradient step in the interpolated
    // field at every pixel line, which would put a faint straight edge back into
    // exactly the boundaries this is meant to curve.
    const int64_t cfx = fadeFractionMm(xMmC - px * pxMmC, pxMmC);
    const int64_t cfy = fadeFractionMm(yMmC - py * pxMmC, pxMmC);
    const ClimateSample* cq = cachedClimateQuad(id_, *tiles_, px, py);
    const ClimateSample cl = blendClimate(cq[0], cq[1], cq[2], cq[3], cfx, cfy, pxMmC);

    // ECOTONE DITHER. Interpolation alone turns 30 m stair-steps into smooth
    // curves, but a smooth curve through a hard threshold is still a clean line
    // -- and a biome boundary in the world is a ragged few-metre transition, not
    // a contour. A small hash perturbation on the two decision channels breaks
    // it up at the metre scale without moving the boundary's average position.
    //
    // +/-2 u8 units is deliberately tiny: on the temperature channel that is
    // ~0.6 C, well inside the model's own uncertainty, and it costs two hashes
    // on a 1.6 m lattice.
    const int32_t ecoT = static_cast<int32_t>(
        hashToSigned16(hash2(seed_, vx >> 4, vy >> 4, CH_ECOTONE_TEMP)) * 2 / 32768);
    const int32_t ecoP = static_cast<int32_t>(
        hashToSigned16(hash2(seed_, vx >> 4, vy >> 4, CH_ECOTONE_PRECIP)) * 2 / 32768);
    const int32_t clTempDithered = clampi32(int64_t(cl.temperature) + ecoT, 0, 255);
    const int32_t clPrecipDithered = clampi32(int64_t(cl.precipitation) + ecoP, 0, 255);

    ColumnSample col;
    col.surfaceMm = s.surfaceMm;

    // Topsoil deepens with rainfall and thins with slope; +/-25% hash jitter
    // breaks up contour-following layer boundaries. See the constant block for
    // why the slope term is a retained fraction rather than a subtracted depth.
    const int64_t rainMmPerYr = climatePrecipMmPerYrFromU8(cl.precipitation);
    const int64_t topsoilBaseMm =
        kTopsoilBaseMm + rainMmPerYr * kTopsoilMmPerMetreOfRain / 1000;
    const int64_t retainQ10 =
        clampi64(1024 - slopeMmPerM * 1024 / kBiomeCliffSlopeMmPerM,
                 kTopsoilSlopeRetentionMinQ10, 1024);
    int64_t topsoil = topsoilBaseMm * retainQ10 / 1024;
    const int64_t tj = hashToSigned16(hash2(seed_, vx >> 4, vy >> 4, CH_TOPSOIL_JITTER));
    topsoil += topsoil * tj / (4 * 32768);
    // Clamp AFTER the jitter -- see kTopsoilMinMm.
    col.topsoilMm = static_cast<int32_t>(clampi64(topsoil, kTopsoilMinMm, kTopsoilMaxMm));

    // From the CLAMPED topsoil, not the raw one -- otherwise a column whose
    // topsoil was floored still gets the unfloored value's subsoil, and the
    // two layers disagree about the same column.
    col.subsoilMm = clampi32(int64_t(col.topsoilMm) * 2 + 500, 0, 6000);

    // Bedrock top: a jittered band CENTRED ON 200 m (Matt's decision; was
    // 40-60 m at kWorldGenVersion 4). Same deterministic shape as before â€” one
    // 16-bit field of a 6.4 m-lattice hash, linearly mapped onto [base, base +
    // span) â€” only the two constants move.
    //
    // Why 180-220 m rather than "the old 40 m + 50%-of-base shape scaled up"
    // (which would give 200-300 m, mean 250 m): 200 m is the number asked for,
    // so it is the band's MEAN, not its floor. The +/-20 m (10%) span is large
    // enough that the bedrock boundary does not read as a flat sheet draped
    // under the terrain â€” the only reason the jitter exists â€” while staying
    // comfortably clear of everything above it: the deepest a cavern chain
    // reaches is ~128 m (test_caverns.cpp measures 128'050 mm of depth at a
    // 200 m bedrock), so even the shallowest 180 m draw leaves ~52 m of
    // untouched rock above the floor against caves.h's 2 m margin.
    const uint64_t bj = hash2(seed_, vx >> 6, vy >> 6, CH_BEDROCK_JITTER);
    // Named constants rather than literals so coupling (11) in the block above
    // -- the independent bedrock backstop on the all-solid carve envelope --
    // is checked against the range this line ACTUALLY produces. Same values, so
    // worldgen output is bit-identical.
    col.bedrockDepthMm =
        static_cast<int32_t>(kBedrockDepthMinMm + ((bj >> 48) * kBedrockDepthJitterMm) / 65536);

    // Surface material from biome classification (M4): morphology gates
    // (slope, coastal band, temperature-adjusted treeline) run before the
    // Whittaker climate lookup â€” see voxelcore/biome.h, mirrored bit-exactly
    // in worldgen.ush's ColumnMain.
    // Dithered temperature/precipitation, undithered seasonality: the dither
    // exists to ragged the two gates that draw long boundaries across the world
    // (treeline and the Whittaker precipitation bands). Topsoil depth above
    // deliberately uses the UNDITHERED rainfall -- it is a depth model, not a
    // boundary, and dithering it would just add noise to a smooth field.
    const BiomeId biome = classifyBiome(clTempDithered, clPrecipDithered, cl.seasonality,
                                         col.surfaceMm, slopeMmPerM);
    col.surfaceMat = biomeSurfaceMaterial(biome, col.surfaceMm);

    // Phase 4 bounded 3D density (voxelcore/density3.h), reduced once per column
    // exactly as the cave and cavern passes are, and for the same reason: the
    // per-voxel test is a static function of (ColumnSample, vz).
    //
    // ORDERED AFTER surfaceMat BECAUSE IT READS IT. The lithology gate's
    // argument is the depth of soil above rock, and which of the two soil bands
    // count is decided by the surface material -- see coupling (14).
    //
    // COST ON A COLUMN THAT DOES NOT QUALIFY: two compares and two divides, and
    // density3ColumnFor returns a zeroed struct having computed no hash at all.
    // On the 25 real diffusion tiles that is 93.5% of columns -- 88% rejected by
    // the slope gate and the rest by the lithology gate. The qualifying 6.5% pay
    // exactly one hash2, the structural domain. The slope fed in is the
    // CARRIER's analytic gradient magnitude, the same scalar slopeScaleQ10 and
    // classifyBiome already take -- density3.h's "primary form" exists precisely
    // so this needs no gradient vector and SurfaceEval needs no new field.
    col.d3 = density3ColumnFor(seed_, xMmC, yMmC, slopeMmPerM, soilAboveRockMm(col));

    // M4 cave pass (voxelcore/caves.h): reduce the jittered lattice tunnel
    // network to the tube axes that pass near this column. Depends only on
    // (seed, vx, vy, surfaceMm) â€” no raster reads â€” which is what lets
    // worldgen.ush recompute it inside VoxelizeMain rather than widening
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
    // the querying column's â€” see caverns.h's cavernSiteFor. Supplying
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
    // THE ONE LINE THAT MAKES THIS NOT A HEIGHTFIELD (kWorldGenVersion 12).
    // The test was `centre <= surfaceMm`; it is now `centre <= surfaceMm + D`,
    // with |D| <= kDensity3MaxAbsMm from voxelcore/density3.h. Because D is a
    // function of z, the solid set is no longer the region under a graph, and a
    // column can read air-then-solid going up -- an overhang.
    //
    // D DISPLACES THE WHOLE PROFILE, not just the air test. depthMm is measured
    // from the displaced surface, so topsoil, subsoil and the rock/bedrock
    // boundary all hang below the displaced surface rather than below a ghost
    // one 700 mm away. The alternative -- move the air boundary and leave the
    // layers where they were -- would put an undercut nose made of BEDROCK on a
    // cliff whose bedrock top is 200 m down, and would break stratigraphy's own
    // depth ordering (the surface material reappearing below the top layer),
    // which the MAT_ROCK case below already exists to protect.
    //
    // COST: on a column whose slope gate is closed (~93% of them, and the whole
    // reason this is affordable) density3ColumnDisplacementMm is one compare
    // against a zeroed field. Inside a qualifying column it is one more compare
    // per voxel outside the +/-700 mm band, and only inside the band does it
    // hash. Nothing here is approximate -- see density3.h section 0 for why both
    // skips return the identical answer rather than a close one.
    const int64_t displacementMm =
        density3ColumnDisplacementMm(col.d3, centreMm, col.surfaceMm);
    const int64_t depthMm = static_cast<int64_t>(col.surfaceMm) + displacementMm - centreMm;
    if (depthMm < 0) return MAT_AIR;
    if (depthMm < col.topsoilMm) return col.surfaceMat;
    if (depthMm < col.topsoilMm + col.subsoilMm) {
        // Coarse subsoil under sandy surfaces; and under a BARE_ROCK surface
        // (worldgen v8) there is no soil profile at all -- it is rock all the
        // way down. Without this a cliff column read rock, then SUBSOIL, then
        // rock again: a soil layer sandwiched under a rock skin, which is not
        // a thing, and which also broke stratigraphy's depth-ordering
        // invariant (the surface material reappearing below the top layer).
        if (col.surfaceMat == MAT_SAND) return MAT_GRAVEL;
        if (col.surfaceMat == MAT_ROCK) return MAT_ROCK;
        return MAT_SUBSOIL;
    }
    if (depthMm < col.bedrockDepthMm) return MAT_ROCK;
    return MAT_BEDROCK;
}

MaterialId Amplifier::materialAt(const ColumnSample& col, int64_t vz) {
    const MaterialId m = stratigraphyAt(col, vz);
    // Already void, or the unbounded bedrock floor: the cave pass never
    // touches either. Refusing MAT_BEDROCK here is the third and last of the
    // independent bedrock guards (caves.h documents the other two) â€” even a
    // mis-tuned constant table cannot punch a hole in the world's floor.
    if (m == MAT_AIR || m == MAT_BEDROCK) return m;
    // MAT_AIR is an enumerator and `m` is a MaterialId variable, so a bare
    // `cond ? MAT_AIR : m` mixes an enumerated and a non-enumerated operand â€”
    // gcc's -Wextra rejects that (clang does not), so name the type explicitly.
    if (caveCarveAt(col.cave, col.surfaceMm, col.bedrockDepthMm, vz))
        return static_cast<MaterialId>(MAT_AIR);
    // Caverns (voxelcore/caverns.h) carve the same way tunnels do and under
    // the same three independent bedrock guards â€” the MAT_BEDROCK refusal
    // above, cavernCarveAt's own margin clamp, and the geometry itself.
    // Ordered after caves purely because a cavern column is far rarer, so the
    // common case never reaches this call: cavernCarveAt's first test is
    // `count == 0`.
    if (cavernCarveAt(col.cavern, col.surfaceMm, col.bedrockDepthMm, vz))
        return static_cast<MaterialId>(MAT_AIR);
    return m;
}

#ifdef VXC_MEMO_STATS
MemoStats& memoStats() { return tlMemoStats; }
void resetMemoStats() { tlMemoStats = MemoStats{}; }
#endif

} // namespace vxc
