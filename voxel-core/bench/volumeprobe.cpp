// vxc_volumeprobe -- the BRICK / VRAM CENSUS. Counts, not clocks.
//
// WHY THIS EXISTS. `docs/ray-marching-plan-2026-08-19.md` section 4 carries a
// number it cannot defend: three independent estimates of what a resident
// BRICK volume costs at 10 cm over the 4 km cascade came out ~150 MB, ~300 MB
// and ~657 MB -- a 4x spread -- and the whole spread lives in two unknowns
// that were ASSUMED rather than measured:
//
//   1. WHAT FRACTION OF BRICKS ARE MIXED. A brick that holds one material
//      collapses (`Brick<8>::tryCollapse`, brick.h) to an 8 B descriptor with
//      no payload at all. Every estimate assumed 20-30% mixed; nobody counted.
//      Interior solid rock collapses too, which is why the scheme does not
//      lose the case it looks like it should lose to greedy meshing.
//   2. HOW MANY DISTINCT MATERIALS A MIXED BRICK HOLDS. That -- and only that
//      -- picks bits-per-voxel (1 / 2 / 4 / 8), and cells are most of the byte
//      count. The 2-vs-4 bpp guess IS the 300-vs-657 MB gap.
//
// So this tool replaces an estimate with a walk. It generates the SAME bricks
// the engine generates, through the SAME functions, and reports what they
// actually contain.
//
// THE COMPARISON IS ONLY HONEST IF BOTH SIDES SEE THE SAME BRICKS. This is the
// `vxc_farwaterschemes` precedent (see that file's header): a scheme comparison
// assembled from two separate walks measures the difference between two walks.
// So the quad count here is not quoted from the HUD or from `vxc_bench` -- every
// brick this tool censuses is ALSO handed to `vxc::meshBrick<8>` in the same
// loop, and the quad column and the brick column are two readings of one field.
//
// THE ENGINE'S BINDING, NOT A SECOND ONE (the standing rule of 2026-08-17,
// `docs/asset-placement-audit.md` section: a probe pricing a world the engine
// does not run is not evidence). Every material this tool sees comes from:
//
//   * level 0 and every coarse ring:  `vxc::Amplifier::materialAt(col, vz)`
//     at the representative coordinate `GeneratedWorld::coarseRep(c, L)` --
//     which is literally what `makeCoarseBrick` computes, and what
//     `FCoarseChunkGridSampler` (VoxelWorldSubsystem.cpp) unfolds inline.
//     NOT `mips.h`: the mip chain is a majority vote and the rings are
//     nearest-neighbour; generator.h documents them as separate paths with
//     separate goldens, and the renderer runs the coarse one.
//   * the asset term at EVERY level, air-only and monotone, resolved once per
//     CHUNK over the chunk's dilated rep rect and sampled through per-column
//     shortlists -- the shape `FCoarseChunkGridSampler` uses, for its reason:
//     a level-5 chunk's rect holds thousands of instances and a per-voxel walk
//     of the full list is dead on arrival.
//   * placement channels through `assetColumnChannelsAt` (the canonical
//     binding). Without them riparians refuse everywhere and the standing-water
//     veto is inert -- the sentinel world, which is a different world.
//
// WHAT IT REPORTS, per ring level and in total:
//
//   1. BRICK CENSUS -- slots walked, all-air, all-solid (one material),
//      solid-but-multi-material, and MIXED. The partition is strict.
//   2. PALETTE-SIZE HISTOGRAM OVER MIXED BRICKS at 1 / 2 / 3-4 / 5-8 / 9-16 /
//      17+ distinct materials, with the bpp bucket each implies. This is the
//      single number the whole census rests on.
//   3. BYTES under a flat 1, 2, 4 and 8 bpp AND under ADAPTIVE (each brick at
//      the bpp its OWN palette needs), reported separately from occupancy
//      (64 B per mixed brick, `packBrickSolidBits`), descriptors (8 B per
//      brick) and the index -- because those three do not move with bpp and
//      lumping them in is how a 4x spread hides.
//   4. THE SAME WALK'S QUAD COUNT from `meshBrick<8>`, priced at 8 B packed +
//      4 B chunk id, against today's pool.
//   5. --exp: the measured SCALING EXPONENT of mixed-brick count against
//      resolution, over 100 / 50 / 25 mm. The plan predicts 2.0 (x4 per
//      halving) and prices the 5 cm row on it; at 2.5 that row is ~40% wrong.
//
// TWO REFUSALS, both learned the hard way on this project:
//
//   * A CASCADE WITH A ZERO-BRICK RING IS NOT REPORTED. An absent statistic
//     and a measured zero print the same "0" and mean opposite things
//     (`vxc_farwaterprobe` #226: a coarse level offered 138 surface bricks and
//     meshed zero quads, and the zero read as "cheap"). If any ring produces
//     no bricks this exits non-zero with the ring named.
//   * EVERY ROW PRINTS ITS OWN n AND ITS EXTRAPOLATION FACTOR. A full 2.5 cm
//     walk over 4 km is ~365M bricks and is not runnable, so --sample takes a
//     DETERMINISTIC STRATIFIED sample: the ring's chunk grid is partitioned
//     into s x s blocks and one chunk per block is chosen by splitmix64 of the
//     block index. A regular stride would alias against terrain periodicity; a
//     random draw would not reproduce. A row without its n is not a row.
//
// ONE HONEST LIMIT, stated where it is measured rather than in a footnote.
// `--voxel-mm 50` and `--voxel-mm 25` ask for a lattice FINER than
// `kVoxelSizeMm`, and the engine has no such rule -- the plan's own answer is
// that finer voxels mean adding ring levels BELOW L0. This tool extends the
// representative-sample rule downward, and the extension is exact in z and
// SATURATED in xy:
//
//   * z is exact. `Amplifier::materialAt` depends on z only through the voxel
//     centre `vz*100+50` (stratigraphy, and the water-marker test), so a cell
//     centre at any millimetre is obtained by shifting the COLUMN by the
//     difference and calling the engine's own function -- not by restating its
//     stratigraphy here. See `materialAtCentreMm`.
//   * xy is NOT refined, and cannot be. `Amplifier::column` is addressed in
//     INTEGER LEVEL-0 VOXELS; there is no sub-100 mm horizontal query in the
//     amplifier, so two 5 cm cells inside one 10 cm column read the same
//     ground. That is not this tool being lazy -- it is the measurement:
//     **5 cm voxels are a worldgen change, not a lattice change.** It is the
//     same conclusion plan section 4 reaches from the asset side ("the asset
//     migration and finer voxels are the same work item"), reached
//     independently from the terrain side.
//
//   The one approximation: the cave/cavern carve is evaluated at the true
//   containing LEVEL-0 voxel with a surface shifted by under one voxel
//   (< 100 mm) -- the cave lattice is metres-scale, so this is below its own
//   quantisation. Stratigraphy and the marker are exact.
//
// Usage:
//   vxc_volumeprobe --cascade [--at Xm Ym] [--voxel-mm 100|50|25] [--seed N]
//                   [--sample F] [--assets DIR] [--tiles DIR] [--coarse DIR]
//                   [--zstd PATH] [--exp]
//   vxc_volumeprobe --radius N  (one disc at the base voxel size, no cascade)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// Without NOMINMAX, windows.h's min/max macros break every std::max here.
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "voxelcore/amplifier.h"
#include "voxelcore/assetbank.h"
#include "voxelcore/assetchannels.h"
#include "voxelcore/assetfield.h"
#include "voxelcore/assetmanifest.h"
#include "voxelcore/brick.h"
#include "voxelcore/craftlattice.h"
#include "voxelcore/craftvolume.h"
#include "voxelcore/generator.h"
#include "voxelcore/hash.h"
#include "voxelcore/lakes.h"
#include "voxelcore/mesher.h"
#include "voxelcore/mips.h"
#include "voxelcore/tilestore.h"

using namespace vxc;

namespace {

constexpr int B = 8;                              // Brick<8>, the shipped edge
constexpr int kChunkBricks = 4;                   // 4 x 4 x 4 bricks per chunk
constexpr int kChunkCells = B * kChunkBricks;     // 32 cells per chunk axis
constexpr int kGridEdge = kChunkCells + 2;        // + the mesher's 1-cell apron

// THE COST MODEL, one constant per line so a reader can price a different
// scheme by editing exactly one of them.
constexpr double kBytesDescriptor = 8.0;   // per brick slot, homogeneous or not
constexpr double kBytesOccupancy = 64.0;   // 512 bits, mixed bricks only
constexpr double kBytesPaletteEntry = 1.0; // one MaterialId
constexpr double kBytesPalettePlan = 16.0; // plan section 4's "~16 B palette"
constexpr double kBytesIndexSlot = 4.0;    // one uint32 per slot of a FLAT index
constexpr double kBytesQuad = 12.0;        // 8 B packed quad + 4 B chunk id

// Today's saturated quad pool, from plan section 4. Quoted, not measured here:
// this tool has no editor. It is printed beside every total so the refund is
// never left as an exercise.
constexpr double kTodayPoolMiB = 2197.0;

double mib(double bytes) { return bytes / (1024.0 * 1024.0); }

int64_t ceilDiv(int64_t a, int64_t b) { return -floorDiv(-a, b); }

// Least-squares slope of log(y) against log(x). Copied in spirit from
// farwaterschemes.cpp -- same job, same guard against a zero row.
double logLogSlope(const std::vector<double>& xs, const std::vector<double>& ys) {
    size_t n = 0;
    double sx = 0.0, sy = 0.0;
    for (size_t i = 0; i < xs.size() && i < ys.size(); ++i) {
        if (xs[i] <= 0.0 || ys[i] <= 0.0) continue;
        sx += std::log(xs[i]);
        sy += std::log(ys[i]);
        ++n;
    }
    if (n < 2) return 0.0;
    const double mx = sx / double(n), my = sy / double(n);
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < xs.size() && i < ys.size(); ++i) {
        if (xs[i] <= 0.0 || ys[i] <= 0.0) continue;
        const double dx = std::log(xs[i]) - mx;
        num += dx * (std::log(ys[i]) - my);
        den += dx * dx;
    }
    return den == 0.0 ? 0.0 : num / den;
}

// --- runtime zstd, bound the way the game binds it --------------------------
// Verbatim in shape from farwaterschemes.cpp: the baked fine tiles are
// CODEC_ZSTD and a tool that cannot inflate them refuses every tile rather
// than silently censusing synthetic ground.
using ZstdDecompressFn = size_t (*)(void*, size_t, const void*, size_t);
using ZstdIsErrorFn = unsigned (*)(size_t);
ZstdDecompressFn gZstdDecompress = nullptr;
ZstdIsErrorFn gZstdIsError = nullptr;

bool zstdInflate(void*, const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    if (!src || !dst || srcLen == 0 || dstLen == 0) return false;
    if (!gZstdDecompress || !gZstdIsError) return false;
    const size_t produced = gZstdDecompress(dst, dstLen, src, srcLen);
    if (gZstdIsError(produced)) return false;
    return produced == dstLen;
}

void* openLib(const char* path) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryA(path));
#else
    return dlopen(path, RTLD_NOW);
#endif
}
void* symbol(void* h, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), name));
#else
    return dlsym(h, name);
#endif
}

bool bindZstd(const std::string& explicitPath) {
    std::vector<std::string> cands;
    if (!explicitPath.empty()) cands.push_back(explicitPath);
#if defined(_WIN32)
    cands.push_back("libzstd.dll");
    cands.push_back("zstd.dll");
#else
    cands.push_back("libzstd.so.1");
    cands.push_back("libzstd.so");
#endif
    for (const auto& c : cands) {
        void* h = openLib(c.c_str());
        if (!h) continue;
        auto d = reinterpret_cast<ZstdDecompressFn>(symbol(h, "ZSTD_decompress"));
        auto e = reinterpret_cast<ZstdIsErrorFn>(symbol(h, "ZSTD_isError"));
        if (!d || !e) continue;
        gZstdDecompress = d;
        gZstdIsError = e;
        std::printf("zstd: bound from '%s'\n", c.c_str());
        return true;
    }
    std::printf("zstd: NOT BOUND -- every CODEC_ZSTD tile will be refused\n");
    return false;
}

// ---------------------------------------------------------------------------
// THE LATTICE. One ring level, expressed as a cell size in millimetres, so the
// engine's levels (>= 100 mm) and the plan's proposed sub-levels (< 100 mm)
// are one object and every call site is written once.
// ---------------------------------------------------------------------------
struct Lattice {
    int64_t cellMm = kVoxelSizeMm;
    int64_t scale = 1;   // cellMm / 100, for cellMm >= 100
    bool sub = false;    // finer than kVoxelSizeMm: no engine rule, see header

    static Lattice forCell(int64_t cellMm) {
        Lattice L;
        L.cellMm = cellMm;
        L.sub = cellMm < int64_t(kVoxelSizeMm);
        L.scale = L.sub ? 1 : cellMm / int64_t(kVoxelSizeMm);
        return L;
    }

    // The REPRESENTATIVE level-0 voxel index for cell `c` on one axis. For the
    // engine's levels this is exactly GeneratedWorld<B>::coarseRep(c, L) --
    // deliberately spelled with the same integer division (scale/2 truncates,
    // which is what makes level 0 the identity).
    int64_t repVx(int64_t c) const {
        if (!sub) return c * scale + scale / 2;
        return floorDiv(c * cellMm + cellMm / 2, int64_t(kVoxelSizeMm));
    }

    // The millimetre height this cell's sample is taken at. For engine levels
    // that is the representative voxel's own centre -- the documented constant
    // +50 mm bias, below the coarse quantisation itself.
    int64_t centreMm(int64_t c) const {
        if (!sub) return repVx(c) * int64_t(kVoxelSizeMm) + int64_t(kVoxelSizeMm) / 2;
        return c * cellMm + cellMm / 2;
    }

    // Topmost SOLID cell of a column, under exactly the rule
    // GeneratedWorld::coarseSurfaceBrickRange applies: find the topmost level-0
    // voxel whose centre is at or below the surface, then ask which coarse cell
    // it falls in. Widening in level-0 units first and reducing afterwards is
    // load-bearing -- widening by whole COARSE cells over-covers by 2^L.
    int64_t topSolidCell(int32_t surfaceMm) const {
        if (!sub) {
            const int64_t top0 =
                floorDiv(int64_t(surfaceMm) - int64_t(kVoxelSizeMm) / 2, int64_t(kVoxelSizeMm));
            return floorDiv(top0 - scale / 2, scale);
        }
        return floorDiv(int64_t(surfaceMm) - cellMm / 2, cellMm);
    }

    // The cells on one axis whose representative voxel lands in [v0, v1].
    // Monotone in c, so this is an inversion rather than a search. Used for the
    // asset shortlists and for the asset z widening.
    void cellsForVoxelRange(int64_t v0, int64_t v1, int64_t& c0, int64_t& c1) const {
        if (!sub) {
            c0 = ceilDiv(v0 - scale / 2, scale);
            c1 = floorDiv(v1 - scale / 2, scale);
        } else {
            c0 = ceilDiv(v0 * int64_t(kVoxelSizeMm) - cellMm / 2, cellMm);
            c1 = floorDiv((v1 + 1) * int64_t(kVoxelSizeMm) - 1 - cellMm / 2, cellMm);
        }
    }
};

// THE ENGINE'S OWN materialAt, EVALUATED AT AN ARBITRARY MILLIMETRE CENTRE.
//
// Amplifier::materialAt(col, vz) reads z in exactly two places: the
// stratigraphy depth `col.surfaceMm - (vz*100+50)`, and the water-marker test
// `vz*100+50 <= col.waterSurfaceMm`. Both are differences against the column's
// own millimetre fields, so shifting BOTH fields by (nominal centre - wanted
// centre) makes the engine's function compute the wanted depth exactly. That
// is why this is nine lines and not a restatement of stratigraphyAt: the
// stratigraphy, the biome surface material, the topsoil/subsoil/bedrock
// ordering and the marker all stay in amplifier.cpp where their goldens are.
//
// The residual: caveCarveAt/cavernCarveAt take the level-0 `vz` (correct, it is
// the true containing voxel) and the SHIFTED surface, so the carve sees a
// surface displaced by under one level-0 voxel. The cave lattice is
// metres-scale; this is below its own quantisation and is the only
// approximation in the sub-lattice path.
MaterialId materialAtCentreMm(const ColumnSample& col, int64_t centreMm) {
    const int64_t half = int64_t(kVoxelSizeMm) / 2;
    const int64_t vz = floorDiv(centreMm - half, int64_t(kVoxelSizeMm));
    const int64_t shift = (vz * int64_t(kVoxelSizeMm) + half) - centreMm;
    ColumnSample c = col;
    c.surfaceMm = static_cast<int32_t>(int64_t(col.surfaceMm) + shift);
    if (c.waterSurfaceMm != kNoWaterMarkerMm)
        c.waterSurfaceMm = static_cast<int32_t>(int64_t(col.waterSurfaceMm) + shift);
    return Amplifier::materialAt(c, vz);
}

// One cell's terrain material on lattice `L`, from a column already sampled at
// that cell's representative xy.
inline MaterialId terrainAt(const Lattice& L, const ColumnSample& col, int64_t cz) {
    // ENGINE LEVELS TAKE THE ENGINE PATH VERBATIM. This is makeCoarseBrick's
    // body, and at level 0 it is makeBrick's, bit-identically.
    if (!L.sub) return Amplifier::materialAt(col, L.repVx(cz));
    return materialAtCentreMm(col, L.centreMm(cz));
}

// ---------------------------------------------------------------------------
// The channel source, bound to the canonical function. Same object the engine
// installs (FVoxelAssetChannelSource), minus the locking -- this tool is
// single-threaded on purpose, because a census that needs a thread pool is a
// census whose determinism nobody can check.
// ---------------------------------------------------------------------------
class ProbeChannelSource final : public IAssetChannelSource {
public:
    ProbeChannelSource(FineTileSampler* fine, IWaterSampler* water, ITileSampler* climate)
        : fine_(fine), water_(water), climate_(climate) {}
    AssetColumnChannels channelsAt(int64_t vx, int64_t vy) override {
        return assetColumnChannelsAt(fine_, water_, climate_, vx, vy);
    }

private:
    FineTileSampler* fine_ = nullptr;
    IWaterSampler* water_ = nullptr;
    ITileSampler* climate_ = nullptr;
};

// The asset term on disk, assembled or refused loudly. Same shape as
// vxc_bench's AssetsOnDisk; everything lives for the whole run because
// AssetField holds pointers.
struct AssetsOnDisk {
    AssetManifest manifest;
    std::vector<AssetSpecies> table;
    AssetBankLibrary banks;
    AssetField field;
    bool ok = false;

    bool load(const std::string& dir, uint64_t seed) {
        const auto blob = readFileBytes(std::filesystem::path(dir) / "species.vxm");
        if (!blob) {
            std::fprintf(stderr, "--assets: cannot read %s/species.vxm\n", dir.c_str());
            return false;
        }
        const AssetManifestError me = manifest.parse(*blob);
        if (me != AssetManifestError::kOk) {
            std::fprintf(stderr, "--assets: manifest refused: %s\n", assetManifestErrorText(me));
            return false;
        }
        const AssetTableBuildStats st = assetSpeciesTableFromManifest(manifest, table);
        if (st.kept == 0) {
            std::fprintf(stderr, "--assets: manifest folded to an EMPTY table\n");
            return false;
        }
        banks.configure(&manifest, (std::filesystem::path(dir) / "banks").string());
        field.setLayers(manifest.layers().data(), int(manifest.layers().size()));
        field.setSpecies(table.data(), int(table.size()));
        field.setBankSource(&banks);
        field.setSeed(seed);
        std::printf("assets: %d species installed (%d without banks), %zu layers\n", st.kept,
                    st.withoutBanks, manifest.layers().size());
        ok = true;
        return true;
    }
};

// ---------------------------------------------------------------------------
// THE CENSUS. Raw counts over whatever was actually walked; extrapolation
// happens once, at report time, and is printed as its own factor.
// ---------------------------------------------------------------------------
struct Census {
    // Sampling bookkeeping. `chunksCandidate` is every chunk in the annulus;
    // `chunksWalked` is how many were generated. Their ratio IS the row's
    // extrapolation factor and is printed on every row.
    int64_t chunksCandidate = 0, chunksWalked = 0;

    // The strict partition of brick slots walked.
    int64_t bricks = 0;      // == allAir + homogSolid + mixedFull + mixedSurf
    int64_t allAir = 0;      // homogeneous MAT_AIR -- 8 B descriptor, no payload
    int64_t homogSolid = 0;  // homogeneous non-air (all-solid, ONE material)
    int64_t mixedFull = 0;   // not homogeneous, but every cell solid
    int64_t mixedSurf = 0;   // not homogeneous, some air -- the surface shell

    // Palette-size histogram over MIXED bricks: 1, 2, 3-4, 5-8, 9-16, 17+.
    int64_t palHist[6] = {};
    int64_t paletteEntries = 0;  // sum of palette sizes over mixed bricks
    int64_t adaptiveCellBytes = 0; // sum of 64*bpp(own palette) over mixed bricks
    int64_t maxPalette = 0;

    // The shared walk's other half.
    int64_t quads = 0;
    int64_t solidCells = 0;
    int64_t instances = 0;   // --detail-cover: instances actually composed

    // The z envelope actually occupied, in bricks -- the denominator of a FLAT
    // index. Sentinels, not zeros, so "never written" cannot read as "zero".
    int64_t bzMin = INT64_MAX, bzMax = INT64_MIN;

    int64_t mixed() const { return mixedFull + mixedSurf; }

    void merge(const Census& o) {
        chunksCandidate += o.chunksCandidate;
        chunksWalked += o.chunksWalked;
        bricks += o.bricks;
        allAir += o.allAir;
        homogSolid += o.homogSolid;
        mixedFull += o.mixedFull;
        mixedSurf += o.mixedSurf;
        for (int i = 0; i < 6; ++i) palHist[i] += o.palHist[i];
        paletteEntries += o.paletteEntries;
        adaptiveCellBytes += o.adaptiveCellBytes;
        maxPalette = std::max(maxPalette, o.maxPalette);
        quads += o.quads;
        solidCells += o.solidCells;
        instances += o.instances;
        bzMin = std::min(bzMin, o.bzMin);
        bzMax = std::max(bzMax, o.bzMax);
    }
};

// Bits per voxel a palette of `n` distinct materials needs. This is the whole
// bpp question, in four lines, applied per brick by the ADAPTIVE column.
int bppFor(size_t n) {
    if (n <= 2) return 1;
    if (n <= 4) return 2;
    if (n <= 16) return 4;
    return 8;
}

int palBucket(size_t n) {
    if (n <= 1) return 0;
    if (n == 2) return 1;
    if (n <= 4) return 2;
    if (n <= 8) return 3;
    if (n <= 16) return 4;
    return 5;
}

const char* kPalBucketName[6] = {"1", "2", "3-4", "5-8", "9-16", "17+"};
const int kPalBucketBpp[6] = {1, 1, 2, 4, 4, 8};

// One ring of the cascade.
struct Ring {
    int level = 0;
    double innerM = 0.0, outerM = 0.0;
    Lattice lat;
    Census cen;
};

// ---------------------------------------------------------------------------
// The walk state: everything one configuration needs, built once.
// ---------------------------------------------------------------------------
struct Walk {
    const Amplifier* amp = nullptr;
    GeneratedWorld<B>* gen = nullptr;
    const AssetField* assets = nullptr;
    const AssetBankLibrary* banks = nullptr;
    uint64_t seed = 0;
    int64_t camXmm = 0, camYmm = 0;
    int64_t depthMm = 0; // --depth-m, see the note above censusChunk

    // Reused across chunks so a 24k-chunk cascade does not allocate 24k times.
    std::vector<ColumnSample> cols;
    std::vector<AssetField::ResolvedAssetInstance> resolved;
    std::vector<AssetField::ResolvedCoverInstance> coverResolved;
    std::vector<std::vector<uint16_t>> shortlist;
    std::vector<Quad> quads;

    Walk() {
        cols.resize(size_t(kGridEdge) * kGridEdge);
        shortlist.resize(size_t(kGridEdge) * kGridEdge);
    }
};

// TWO READINGS OF "RESIDENT SET", AND CONFLATING THEM IS WHERE THE 4x LIVES.
//
// `--depth-m 0` (the default) censuses the SURFACE SHELL: every brick holding
// some column's topmost solid cell, plus whatever the asset term reaches. That
// is exactly the set the streamer materialises and meshes today, so it is the
// only apples-to-apples comparison against the quad pool -- and it is ~100%
// mixed BY CONSTRUCTION, because a brick is in the set precisely because the
// surface crosses it.
//
// `--depth-m D` extends every brick column D metres DOWNWARD. That is the set a
// marchable/diggable VOLUME needs, and it is the reading plan section 4's
// estimates were actually made under: it is where interior solid bricks appear,
// where `tryCollapse` earns its "load-bearing", and the only reading in which a
// 20-30% mixed fraction is even possible. The two readings differ by an order
// of magnitude in brick count and by a factor of ten in mixed fraction, so a
// census that reports one number without naming which is a census that has
// recreated the spread it was built to close.
//
// Generate and census ONE chunk (4 x 4 x 4 bricks) of lattice `L` whose cell
// origin is (baseCx, baseCy) on the chunk grid. Mirrors the engine's chunk job:
// one aproned column grid, one asset resolve, then the bricks.
void censusChunk(Walk& w, const Lattice& L, int64_t chunkX, int64_t chunkY, Census& out) {
    const int64_t baseCx = chunkX * kChunkCells, baseCy = chunkY * kChunkCells;

    // --- the aproned column grid, at REPRESENTATIVE xy ---------------------
    for (int ly = 0; ly < kGridEdge; ++ly) {
        const int64_t vy = L.repVx(baseCy + ly - 1);
        for (int lx = 0; lx < kGridEdge; ++lx)
            w.cols[size_t(lx) + size_t(kGridEdge) * size_t(ly)] =
                w.amp->column(L.repVx(baseCx + lx - 1), vy);
    }

    // --- the asset term, resolved ONCE for the whole chunk ------------------
    //
    // Per CHUNK, not per brick, and that is not an optimisation: a level-5
    // chunk's dilated rect holds thousands of sites, each costing an amplifier
    // column, so resolving per 8-cell brick footprint would pay ~16x the
    // columns for identical answers. It is also exactly what the engine does.
    w.resolved.clear();
    for (auto& s : w.shortlist) s.clear();
    bool haveAssets = false;
    if (w.assets != nullptr && !w.assets->empty()) {
        const AssetVoxelRect rect{L.repVx(baseCx - 1), L.repVx(baseCy - 1),
                                  L.repVx(baseCx + kChunkCells), L.repVx(baseCy + kChunkCells)};
        GeneratedWorld<B>* gen = w.gen;
        const Amplifier* amp = w.amp;
        const std::vector<AssetInstance> insts = w.assets->instancesForRect(
            rect,
            [gen, amp](int64_t avx, int64_t avy) {
                // THROUGH THE CHANNEL SOURCE, never the channel-less overload:
                // that overload is the sentinel world in which riparians refuse
                // everywhere and the standing-water veto is inert.
                return assetColumnFactsFromSample(amp->column(avx, avy),
                                                  gen->assetChannelsAt(avx, avy));
            },
            /*terrainOnly*/ true);
        w.resolved = w.assets->resolveForCompose(insts);
        haveAssets = !w.resolved.empty();

        // PER-COLUMN SHORTLISTS, built instance-outward, exactly as
        // FCoarseChunkGridSampler builds them: each instance appends its index
        // to the few grid columns whose representative coordinate its rotated
        // xy box covers, leaving the per-cell lookup a 0-to-few walk.
        for (size_t i = 0; i < w.resolved.size(); ++i) {
            const auto& r = w.resolved[i];
            const int64_t minVx = r.anchorVx + r.grid->rotatedOriginX(r.yawQuarter);
            const int64_t minVy = r.anchorVy + r.grid->rotatedOriginY(r.yawQuarter);
            const int64_t maxVx = minVx + r.grid->rotatedSizeX(r.yawQuarter) - 1;
            const int64_t maxVy = minVy + r.grid->rotatedSizeY(r.yawQuarter) - 1;
            int64_t c0x = 0, c1x = 0, c0y = 0, c1y = 0;
            L.cellsForVoxelRange(minVx, maxVx, c0x, c1x);
            L.cellsForVoxelRange(minVy, maxVy, c0y, c1y);
            c0x = std::max(c0x, baseCx - 1);
            c1x = std::min(c1x, baseCx + kChunkCells);
            c0y = std::max(c0y, baseCy - 1);
            c1y = std::min(c1y, baseCy + kChunkCells);
            for (int64_t cy = c0y; cy <= c1y; ++cy)
                for (int64_t cx = c0x; cx <= c1x; ++cx)
                    w.shortlist[size_t(cx - baseCx + 1) +
                                size_t(kGridEdge) * size_t(cy - baseCy + 1)]
                        .push_back(static_cast<uint16_t>(i));
        }
    }

    // The cell sampler: terrain first, then the asset term, AIR ONLY. The
    // monotone rule (an asset never replaces terrain) is what keeps a
    // part-buried rock buried and the all-solid floor bound true.
    const auto cellAt = [&](int64_t cx, int64_t cy, int64_t cz) -> MaterialId {
        const size_t gi =
            size_t(cx - baseCx + 1) + size_t(kGridEdge) * size_t(cy - baseCy + 1);
        const MaterialId m = terrainAt(L, w.cols[gi], cz);
        if (m != MAT_AIR || !haveAssets) return m;
        const std::vector<uint16_t>& sl = w.shortlist[gi];
        if (sl.empty()) return m;
        const int64_t rx = L.repVx(cx), ry = L.repVx(cy), rz = L.repVx(cz);
        for (const uint16_t i : sl) {
            const auto& r = w.resolved[i];
            const int64_t ax = rx - r.anchorVx - int64_t(r.grid->rotatedOriginX(r.yawQuarter));
            const int64_t ay = ry - r.anchorVy - int64_t(r.grid->rotatedOriginY(r.yawQuarter));
            const int64_t az = rz - r.anchorVz - int64_t(r.grid->originZ());
            if (ax < INT32_MIN || ax > INT32_MAX || ay < INT32_MIN || ay > INT32_MAX ||
                az < INT32_MIN || az > INT32_MAX)
                continue;
            const MaterialId am = r.grid->atYaw(int32_t(ax), int32_t(ay), int32_t(az),
                                                r.yawQuarter);
            if (am != MAT_AIR) return am;
        }
        return m;
    };

    // --- the bricks --------------------------------------------------------
    for (int byi = 0; byi < kChunkBricks; ++byi) {
        for (int bxi = 0; bxi < kChunkBricks; ++bxi) {
            const int64_t fx0 = baseCx + int64_t(bxi) * B, fy0 = baseCy + int64_t(byi) * B;

            // Surface shell for this brick footprint: every brick holding some
            // column's topmost solid cell. Same rule as
            // GeneratedWorld::coarseSurfaceBrickRange, restated against this
            // aproned grid rather than a ColumnGrid -- if the two ever
            // disagree, this stops censusing the set the streamer materialises.
            int64_t czMin = INT64_MAX, czMax = INT64_MIN;
            for (int y = 0; y < B; ++y)
                for (int x = 0; x < B; ++x) {
                    const size_t gi = size_t(fx0 + x - baseCx + 1) +
                                      size_t(kGridEdge) * size_t(fy0 + y - baseCy + 1);
                    const int64_t top = L.topSolidCell(w.cols[gi].surfaceMm);
                    czMin = std::min(czMin, top);
                    czMax = std::max(czMax, top);
                }

            // ASSET WIDENING, per BRICK COLUMN and by each instance's OWN baked
            // extents -- not by the layer bound. The layer bound is a price
            // list (vxc_assetprobe measures it); using it here would census the
            // BOUND instead of the content, which is a different number and the
            // one that made "18x streaming multiplier" a fake result.
            if (haveAssets) {
                for (int y = 0; y < B; ++y)
                    for (int x = 0; x < B; ++x) {
                        const size_t gi = size_t(fx0 + x - baseCx + 1) +
                                          size_t(kGridEdge) * size_t(fy0 + y - baseCy + 1);
                        for (const uint16_t i : w.shortlist[gi]) {
                            const auto& r = w.resolved[i];
                            const int64_t lo = r.anchorVz + int64_t(r.grid->originZ());
                            const int64_t hi = lo + int64_t(r.grid->sizeZ()) - 1;
                            int64_t c0 = 0, c1 = 0;
                            L.cellsForVoxelRange(lo, hi, c0, c1);
                            if (c1 < c0) continue;
                            czMin = std::min(czMin, c0);
                            czMax = std::max(czMax, c1);
                        }
                    }
            }

            // The VOLUME reading: D metres of ground below the shallowest
            // column of this footprint. In CELLS, so a coarse ring pays coarse
            // cells for the same metres -- a fixed cell depth would make the
            // outer rings a hundred times deeper in metres than the inner ones
            // and the cascade's byte profile would be an artefact of the unit.
            if (w.depthMm > 0) czMin -= std::max<int64_t>(1, w.depthMm / L.cellMm);

            const int64_t bz0 = floorDiv(czMin, B), bz1 = floorDiv(czMax, B);
            out.bzMin = std::min(out.bzMin, bz0);
            out.bzMax = std::max(out.bzMax, bz1);

            for (int64_t bz = bz0; bz <= bz1; ++bz) {
                Brick<B> brick;
                const int64_t fz0 = bz * B;
                for (int y = 0; y < B; ++y)
                    for (int x = 0; x < B; ++x)
                        for (int z = 0; z < B; ++z)
                            brick.set(x, y, z, cellAt(fx0 + x, fy0 + y, fz0 + z));
                // LOAD-BEARING. Without this every interior rock brick counts
                // as mixed and the census answers the wrong question.
                brick.tryCollapse();

                ++out.bricks;
                out.solidCells += int64_t(brick.solidCount());
                if (brick.isHomogeneous()) {
                    if (brick.homogeneousMaterial() == MAT_AIR) ++out.allAir;
                    else ++out.homogSolid;
                } else {
                    const size_t pal = brick.paletteSize();
                    if (brick.solidCount() == size_t(Brick<B>::kCells)) ++out.mixedFull;
                    else ++out.mixedSurf;
                    ++out.palHist[palBucket(pal)];
                    out.paletteEntries += int64_t(pal);
                    out.maxPalette = std::max(out.maxPalette, int64_t(pal));
                    out.adaptiveCellBytes += int64_t(Brick<B>::kCells) * bppFor(pal) / 8;
                }

                // THE OTHER HALF OF THE SHARED FIELD. Same brick, same walk --
                // this is the whole reason quads and bricks can be compared at
                // all. The apron is exactly the mesher's documented [-1, B]
                // domain, served by the same cellAt.
                w.quads.clear();
                meshBrick<B>(
                    [&](int x, int y, int z) -> MaterialId {
                        return cellAt(fx0 + x, fy0 + y, fz0 + z);
                    },
                    w.quads);
                out.quads += int64_t(w.quads.size());
            }
        }
    }
}

// DETERMINISTIC STRATIFIED SELECTION. The ring's chunk grid is partitioned into
// stride x stride blocks and exactly one chunk per block is walked, chosen by
// splitmix64 of the block coordinates. Deterministic (same answer on every
// machine and every re-run), spatially uniform (one per block, so no region is
// unrepresented), and NOT a regular stride -- a fixed phase would alias against
// the terrain's own periodicity, which is precisely the failure mode a census
// of a procedurally periodic world must avoid.
bool selectedChunk(uint64_t seed, int64_t cx, int64_t cy, int64_t stride) {
    if (stride <= 1) return true;
    const int64_t bx = floorDiv(cx, stride), by = floorDiv(cy, stride);
    uint64_t h = splitmix64(seed ^ 0x9E3779B97F4A7C15ull);
    h = splitmix64(h ^ static_cast<uint64_t>(static_cast<uint32_t>(bx)));
    h = splitmix64(h ^ (static_cast<uint64_t>(static_cast<uint32_t>(by)) << 32));
    const int64_t pick = int64_t(h % uint64_t(stride * stride));
    const int64_t lx = cx - bx * stride, ly = cy - by * stride;
    return (ly * stride + lx) == pick;
}

// Walk one annulus at one lattice.
void censusRing(Walk& w, Ring& R, double sampleFraction, bool verbose) {
    const int64_t chunkMm = R.lat.cellMm * kChunkCells;
    const int64_t rIn = int64_t(R.innerM * 1000.0), rOut = int64_t(R.outerM * 1000.0);
    const int64_t rIn2 = rIn * rIn, rOut2 = rOut * rOut;
    const int64_t reach = rOut + chunkMm;
    const int64_t cx0 = floorDiv(w.camXmm - reach, chunkMm);
    const int64_t cx1 = floorDiv(w.camXmm + reach, chunkMm);
    const int64_t cy0 = floorDiv(w.camYmm - reach, chunkMm);
    const int64_t cy1 = floorDiv(w.camYmm + reach, chunkMm);

    int64_t stride = 1;
    if (sampleFraction > 0.0 && sampleFraction < 1.0)
        stride = std::max<int64_t>(1, int64_t(std::llround(std::sqrt(1.0 / sampleFraction))));

    for (int64_t cy = cy0; cy <= cy1; ++cy) {
        for (int64_t cx = cx0; cx <= cx1; ++cx) {
            // Membership by chunk CENTRE, one rule, so a chunk belongs to
            // exactly one ring and the cascade neither double-counts nor gaps.
            const int64_t wx = cx * chunkMm + chunkMm / 2 - w.camXmm;
            const int64_t wy = cy * chunkMm + chunkMm / 2 - w.camYmm;
            const int64_t d2 = wx * wx + wy * wy;
            if (d2 < rIn2 || d2 >= rOut2) continue;
            ++R.cen.chunksCandidate;
            if (!selectedChunk(w.seed, cx, cy, stride)) continue;
            ++R.cen.chunksWalked;
            censusChunk(w, R.lat, cx, cy, R.cen);
        }
        if (verbose && ((cy - cy0) % 32 == 0))
            std::fprintf(stderr, "  ring L%d row %lld/%lld walked %lld chunks\r", R.level,
                         (long long)(cy - cy0), (long long)(cy1 - cy0),
                         (long long)R.cen.chunksWalked);
    }
    if (verbose) std::fprintf(stderr, "                                              \r");
}

// ---------------------------------------------------------------------------
// --detail-cover: THE COVER VOLUME, censused on its own lattice.
// ---------------------------------------------------------------------------
//
// WHAT THIS MEASURES AND WHY IT IS A DIFFERENT WALK, not a flag on the one
// above. docs/detail-assets-in-the-volume-2026-08-19.md section 3.1 disputes
// ray-marching-plan-2026-08-19.md:795-797, which says putting the detail plants
// in a volume and taking R0 to 5 cm are the same work item. The dispute is
// settled by a number, and section 5.1 names the falsifier: if this walk
// reports ~500 MiB or more, the plan is right and the doc is wrong.
//
// A COVER VOLUME HOLDS COVER AND NOTHING ELSE. That is the whole economy and
// it is why this cannot be `--voxel-mm 50` on the terrain walk:
//
//   * The terrain walk's brick z-range starts from every column's topmost solid
//     cell, so EVERY brick column in the ring produces bricks. Here, a brick
//     column that no instance reaches produces ZERO bricks -- not an all-air
//     shell, not a descriptor, nothing. Cover is sparse in xy and thin in z,
//     and a walk that materialised air around it would price a volume nobody
//     proposed.
//   * There is no terrain term in `coverAt` at all. Terrain lives in the 10 cm
//     world volume where it already is; composing it here too would double it.
//
// THE TRANSFORM IS NOT RESTATED HERE. Every cell comes through
// AssetField::coverMaterialOfResolved -- the same function
// coverMaterialAtResolved walks -- because a probe that re-inlines the
// arithmetic measures a world the reference does not describe. That is the
// standing rule of 2026-08-17 and the reason this file quotes the engine's
// binding everywhere else.
//
// THE FLAT INDEX IS REPORTED BUT IS THE WRONG STRUCTURE HERE, and the row says
// so rather than burying it in a total. Cover is a thin shell spread over the
// terrain's full relief, which is the worst case a dense 3D brick index can be
// handed: the occupied fraction is tiny and the z envelope is the landscape's.
// Read the payload columns for what a cover volume costs and the index column
// for what it would cost to address it densely -- they are different questions
// and section 4's 2.5 cm finding is the precedent for keeping them apart.
void censusCoverChunk(Walk& w, const Lattice& L, int64_t chunkX, int64_t chunkY, Census& out) {
    const int64_t baseCx = chunkX * kChunkCells, baseCy = chunkY * kChunkCells;

    w.coverResolved.clear();
    for (auto& s : w.shortlist) s.clear();
    if (w.assets == nullptr || w.assets->empty()) return;

    // The chunk's own rect, in LEVEL-0 voxels: assetSitesForRect already
    // dilates by each layer's maxRadiusMm and tests every site's reach exactly,
    // so pre-dilating here would enumerate twice (assetfield.h's own note).
    const int64_t mm0x = baseCx * L.cellMm, mm1x = (baseCx + kChunkCells) * L.cellMm - 1;
    const int64_t mm0y = baseCy * L.cellMm, mm1y = (baseCy + kChunkCells) * L.cellMm - 1;
    const AssetVoxelRect rect{floorDiv(mm0x, int64_t(kVoxelSizeMm)),
                              floorDiv(mm0y, int64_t(kVoxelSizeMm)),
                              floorDiv(mm1x, int64_t(kVoxelSizeMm)),
                              floorDiv(mm1y, int64_t(kVoxelSizeMm))};

    GeneratedWorld<B>* gen = w.gen;
    const Amplifier* amp = w.amp;
    // terrainOnly FALSE -- the whole point. The terrain walk passes true and
    // that is what makes these 85% of instances invisible to every existing
    // consumer.
    const std::vector<AssetInstance> insts = w.assets->instancesForRect(
        rect,
        [gen, amp](int64_t avx, int64_t avy) {
            return assetColumnFactsFromSample(amp->column(avx, avy),
                                              gen->assetChannelsAt(avx, avy));
        },
        /*terrainOnly*/ false);
    w.coverResolved = w.assets->resolveForCoverCompose(insts, uint32_t(L.cellMm));
    if (w.coverResolved.empty()) return;

    // ANCHOR OWNERSHIP IS THE DEDUP, and without it this counter lies. Reach
    // dilation legitimately returns one instance to every chunk it overlaps
    // (assetSitesForRect tests reach, not containment), so summing list sizes
    // counts a reed once per chunk it leans into. The bricks are unaffected --
    // each chunk censuses its own cells and an instance reaching in is real --
    // but the per-hectare figure would read several times high, which is
    // exactly the direction that would flatter this measurement. Same rule
    // VoxelDetailAssetSubsystem applies for the same reason.
    for (const auto& r : w.coverResolved)
        if (r.anchorCx >= baseCx && r.anchorCx < baseCx + kChunkCells &&
            r.anchorCy >= baseCy && r.anchorCy < baseCy + kChunkCells)
            ++out.instances;

    // Per-column shortlists over the aproned grid, instance-outward. Cover
    // boxes are already in COVER cells (anchorC* is), so this is a clamp rather
    // than a lattice conversion -- the level-0 <-> coarse inversion the terrain
    // path needs has no analogue here.
    for (size_t i = 0; i < w.coverResolved.size(); ++i) {
        const auto& r = w.coverResolved[i];
        const int64_t minCx = r.anchorCx + r.grid->rotatedOriginX(r.yawQuarter);
        const int64_t minCy = r.anchorCy + r.grid->rotatedOriginY(r.yawQuarter);
        const int64_t maxCx = minCx + r.grid->rotatedSizeX(r.yawQuarter) - 1;
        const int64_t maxCy = minCy + r.grid->rotatedSizeY(r.yawQuarter) - 1;
        const int64_t c0x = std::max(minCx, baseCx - 1), c1x = std::min(maxCx, baseCx + kChunkCells);
        const int64_t c0y = std::max(minCy, baseCy - 1), c1y = std::min(maxCy, baseCy + kChunkCells);
        for (int64_t cy = c0y; cy <= c1y; ++cy)
            for (int64_t cx = c0x; cx <= c1x; ++cx)
                w.shortlist[size_t(cx - baseCx + 1) + size_t(kGridEdge) * size_t(cy - baseCy + 1)]
                    .push_back(static_cast<uint16_t>(i));
    }

    const auto coverAt = [&](int64_t cx, int64_t cy, int64_t cz) -> MaterialId {
        const size_t gi = size_t(cx - baseCx + 1) + size_t(kGridEdge) * size_t(cy - baseCy + 1);
        const std::vector<uint16_t>& sl = w.shortlist[gi];
        for (const uint16_t i : sl) {
            // ONE definition of the transform; see the header.
            const MaterialId m =
                AssetField::coverMaterialOfResolved(w.coverResolved[i], cx, cy, cz);
            if (m != MAT_AIR) return m;
        }
        return MAT_AIR;   // no terrain term: cover volumes hold cover
    };

    for (int byi = 0; byi < kChunkBricks; ++byi) {
        for (int bxi = 0; bxi < kChunkBricks; ++bxi) {
            const int64_t fx0 = baseCx + int64_t(bxi) * B, fy0 = baseCy + int64_t(byi) * B;

            // The z envelope of this brick footprint, from the instances' OWN
            // baked extents -- never from a layer bound, which is a price list
            // and not content (the "18x streaming multiplier" fake result).
            int64_t czMin = INT64_MAX, czMax = INT64_MIN;
            for (int y = 0; y < B; ++y)
                for (int x = 0; x < B; ++x) {
                    const size_t gi = size_t(fx0 + x - baseCx + 1) +
                                      size_t(kGridEdge) * size_t(fy0 + y - baseCy + 1);
                    for (const uint16_t i : w.shortlist[gi]) {
                        const auto& r = w.coverResolved[i];
                        const int64_t lo = r.anchorCz + int64_t(r.grid->originZ());
                        const int64_t hi = lo + int64_t(r.grid->sizeZ()) - 1;
                        czMin = std::min(czMin, lo);
                        czMax = std::max(czMax, hi);
                    }
                }
            // NOTHING REACHES THIS BRICK COLUMN -> NO BRICKS. Not an all-air
            // brick, not a descriptor. This one line is most of the difference
            // between a cover volume and R0 at 5 cm.
            if (czMax < czMin) continue;

            const int64_t bz0 = floorDiv(czMin, B), bz1 = floorDiv(czMax, B);
            out.bzMin = std::min(out.bzMin, bz0);
            out.bzMax = std::max(out.bzMax, bz1);

            for (int64_t bz = bz0; bz <= bz1; ++bz) {
                Brick<B> brick;
                const int64_t fz0 = bz * B;
                for (int y = 0; y < B; ++y)
                    for (int x = 0; x < B; ++x)
                        for (int z = 0; z < B; ++z)
                            brick.set(x, y, z, coverAt(fx0 + x, fy0 + y, fz0 + z));
                brick.tryCollapse();

                ++out.bricks;
                out.solidCells += int64_t(brick.solidCount());
                if (brick.isHomogeneous()) {
                    if (brick.homogeneousMaterial() == MAT_AIR) ++out.allAir;
                    else ++out.homogSolid;
                } else {
                    const size_t pal = brick.paletteSize();
                    if (brick.solidCount() == size_t(Brick<B>::kCells)) ++out.mixedFull;
                    else ++out.mixedSurf;
                    ++out.palHist[palBucket(pal)];
                    out.paletteEntries += int64_t(pal);
                    out.maxPalette = std::max(out.maxPalette, int64_t(pal));
                    out.adaptiveCellBytes += int64_t(Brick<B>::kCells) * bppFor(pal) / 8;
                }

                // The same shared-field discipline as the terrain walk: the
                // quad column and the brick column are two readings of one
                // field, so a disagreement is visible rather than assumed.
                w.quads.clear();
                meshBrick<B>(
                    [&](int x, int y, int z) -> MaterialId {
                        return coverAt(fx0 + x, fy0 + y, fz0 + z);
                    },
                    w.quads);
                out.quads += int64_t(w.quads.size());
            }
        }
    }
}

// The cover ring is a DISC, not an annulus: cover has one lattice and one
// reach (VoxelDetailAssetSubsystem's -VoxelDetailRingMeters, default 112 m),
// so there is no cascade to partition.
void censusCoverRing(Walk& w, Ring& R, double sampleFraction, bool verbose) {
    const int64_t chunkMm = R.lat.cellMm * kChunkCells;
    const int64_t rOut = int64_t(R.outerM * 1000.0);
    const int64_t rOut2 = rOut * rOut;
    const int64_t reach = rOut + chunkMm;
    const int64_t cx0 = floorDiv(w.camXmm - reach, chunkMm);
    const int64_t cx1 = floorDiv(w.camXmm + reach, chunkMm);
    const int64_t cy0 = floorDiv(w.camYmm - reach, chunkMm);
    const int64_t cy1 = floorDiv(w.camYmm + reach, chunkMm);

    int64_t stride = 1;
    if (sampleFraction > 0.0 && sampleFraction < 1.0)
        stride = std::max<int64_t>(1, int64_t(std::llround(std::sqrt(1.0 / sampleFraction))));

    for (int64_t cy = cy0; cy <= cy1; ++cy) {
        for (int64_t cx = cx0; cx <= cx1; ++cx) {
            const int64_t wx = cx * chunkMm + chunkMm / 2 - w.camXmm;
            const int64_t wy = cy * chunkMm + chunkMm / 2 - w.camYmm;
            if (wx * wx + wy * wy >= rOut2) continue;
            ++R.cen.chunksCandidate;
            if (!selectedChunk(w.seed, cx, cy, stride)) continue;
            ++R.cen.chunksWalked;
            censusCoverChunk(w, R.lat, cx, cy, R.cen);
        }
        if (verbose && ((cy - cy0) % 16 == 0))
            std::fprintf(stderr, "  cover row %lld/%lld walked %lld chunks\r",
                         (long long)(cy - cy0), (long long)(cy1 - cy0),
                         (long long)R.cen.chunksWalked);
    }
    if (verbose) std::fprintf(stderr, "                                              \r");
}

// ---------------------------------------------------------------------------
// Reporting.
// ---------------------------------------------------------------------------

// Every byte column of one row, already extrapolated. Kept as one struct so
// the per-ring rows and the TOTAL row cannot drift apart.
// ---------------------------------------------------------------------------
// --craft: the settlement model
// ---------------------------------------------------------------------------
//
// THIS HALF IS A MODEL AND THE OTHER HALF IS NOT, AND THE DIFFERENCE IS THE
// WHOLE DESIGN. --detail-cover walks REAL ground and asks the resolver what
// grows there; its number is a measurement. There is no procedural source for
// "what a player builds", so nothing here can be. Every number this mode prints
// is a consequence of a building recipe I wrote, and it is labelled as such on
// every row. The assumption-free half of the craft census lives in
// tests/test_craftcost.cpp, where per-pattern byte costs are PINNED against the
// format contract; that is the half that can falsify something.
//
// WHY ALIGNMENT IS A SWEPT PARAMETER AND NOT A DETAIL. Measured in
// test_craftcost.cpp: a 16x32x16 hole whose faces land on the 8-cell brick grid
// leaves every brick uniform and costs the 512 B floor. THE SAME HOLE MOVED ONE
// CELL costs 3,072 B, because it straddles 32 bricks. Cost tracks carved surface
// measured in BRICKS, not carved volume. A settlement model that fixed alignment
// would report whichever answer its author happened to build in, so this sweeps
// it and prints both.
//
// GROUND INDEPENDENCE. A building is authored, not generated, so these craft
// numbers do not depend on the terrain under them and are unaffected by the
// synthetic-ground fallback. The TERRAIN CONTROL does depend on it, so the
// control is refused rather than printed on synthetic ground -- see the report.

struct CraftBuilding {
    int64_t w = 0, d = 0, h = 0; // craft cells
    int64_t wallT = 0;           // wall/floor/roof thickness, craft cells
    int64_t offset = 0;          // 0 = faces on the brick grid, 1 = one cell off
    int intensity = 0;           // 0 shell, 1 openings, 2 openings + detailing
    int materials = 1;           // distinct solid materials in the fabric
};

// Material of one BUILDING-LOCAL craft cell. MAT_AIR means "not structure".
MaterialId craftBuildingCell(const CraftBuilding& b, int64_t x, int64_t y, int64_t z) {
    if (x < 0 || y < 0 || z < 0 || x >= b.w || y >= b.d || z >= b.h) return MAT_AIR;

    const bool floorSlab = z < b.wallT;
    const bool roofSlab = z >= b.h - b.wallT;
    const bool wall =
        x < b.wallT || x >= b.w - b.wallT || y < b.wallT || y >= b.d - b.wallT;
    if (!floorSlab && !roofSlab && !wall) return MAT_AIR; // the room inside

    if (b.intensity >= 1) {
        // A door through the -y wall, and two windows per long wall. Deliberately
        // NOT on multiples of eight -- an opening that happens to land on the
        // brick grid is the free case and would flatter the model.
        const int64_t doorW = 26, doorH = 78; // ~65 cm x ~195 cm
        if (y < b.wallT && z >= b.wallT && z < b.wallT + doorH &&
            x >= b.w / 2 - doorW / 2 && x < b.w / 2 + doorW / 2) {
            return MAT_AIR;
        }
        const int64_t winW = 34, winH = 34, winZ = b.wallT + 45;
        if (z >= winZ && z < winZ + winH) {
            for (int64_t i = 1; i <= 2; ++i) {
                const int64_t cx = (b.w * i) / 3;
                if (x >= cx - winW / 2 && x < cx + winW / 2 &&
                    (y < b.wallT || y >= b.d - b.wallT)) {
                    return MAT_AIR;
                }
            }
        }
    }

    if (b.intensity >= 2) {
        // Detailing: a chamfer along the top outer edge, and a pierced band
        // under the eaves. This is what 2.5 cm is FOR, and it is the expensive
        // case because both straddle bricks everywhere they go.
        const int64_t fromTop = b.h - 1 - z;
        const int64_t fromEdgeX = std::min(x, b.w - 1 - x);
        const int64_t fromEdgeY = std::min(y, b.d - 1 - y);
        if (fromTop < 6 && std::min(fromEdgeX, fromEdgeY) < 6 - fromTop) return MAT_AIR;

        const int64_t bandZ = b.h - b.wallT - 20;
        if (z >= bandZ && z < bandZ + 14 && wall) {
            if (((x + y) % 6) < 3) return MAT_AIR;
        }
    }

    // THE MATERIAL COUNT MOVES THE BPP LADDER, and a single-material model
    // silently prices every mixed brick at 1 bpp. Real building is stone AND
    // plaster AND timber AND glazing: four materials is 2 bpp plus a 16 B local
    // palette, measured at +36% per mixed brick in test_craftcost.cpp. Found by
    // a mutation exercise noticing that no pinned pattern exercised that rung --
    // so neither did this model, and it was under-pricing every building.
    if (b.materials <= 1) return MAT_ROCK;
    switch (((z / 5) + (x / 11)) % b.materials) {
        case 0: return MAT_ROCK;
        case 1: return MAT_CLAY;
        case 2: return MAT_SAND;
        case 3: return MAT_TOPSOIL;
        case 4: return MAT_GRAVEL;
        case 5: return MAT_SUBSOIL;
        default: return MAT_BEDROCK;
    }
}

struct CraftResult {
    Census cen;                  // priced through bytesFor, comparable with cover
    int64_t packBytes = 0;       // summed ChunkBrickPack::residentBytes()
    int64_t bricksPromoted = 0;  // terrain bricks the structure touches
    int64_t bricksProduced = 0;
    int64_t bricksWithSolid = 0;
    int64_t solidCells = 0;
    int64_t structureCells = 0;  // craft cells the recipe called structure
    bool ran = false;
};

// One building, promoted and carved and packed. ONE BUILDING AT A TIME, and
// then multiplied: buildings in a settlement share no bricks, so the total is
// linear by construction and holding fifty of them in memory at once would buy
// nothing but a peak of a few hundred MB.
CraftResult censusCraftBuilding(const CraftBuilding& b) {
    CraftResult out;
    const int64_t E = int64_t(kCraftChunkEdgeCells);

    // Terrain bricks the bounding box spans, with the alignment offset applied.
    const int64_t x0 = b.offset, y0 = b.offset, z0 = b.offset;
    const int64_t bx0 = floorDiv(x0, E), bx1 = floorDiv(x0 + b.w - 1, E);
    const int64_t by0 = floorDiv(y0, E), by1 = floorDiv(y0 + b.d - 1, E);
    const int64_t bz0 = floorDiv(z0, E), bz1 = floorDiv(z0 + b.h - 1, E);

    CraftLattice<B> lat;
    CraftProducerCounters counters;

    for (int64_t bz = bz0; bz <= bz1; ++bz)
        for (int64_t by = by0; by <= by1; ++by)
            for (int64_t bx = bx0; bx <= bx1; ++bx) {
                const BrickKey tk{int32_t(bx), int32_t(by), int32_t(bz)};

                // PROMOTE ONLY WHAT THE STRUCTURE TOUCHES. A player places wall
                // blocks and chisels them; nobody fills a room with rock to
                // hollow it out. Promoting the whole bounding box would charge
                // the 512 B floor for every cubic metre of empty room and make
                // the model say more about my loop bounds than about building.
                bool touched = false;
                for (int64_t cz = 0; cz < E && !touched; ++cz)
                    for (int64_t cy = 0; cy < E && !touched; ++cy)
                        for (int64_t cx = 0; cx < E && !touched; ++cx) {
                            if (craftBuildingCell(b, bx * E + cx - x0, by * E + cy - y0,
                                                  bz * E + cz - z0) != MAT_AIR) {
                                touched = true;
                            }
                        }
                if (!touched) continue;

                lat.promote(tk, Brick<B>(MAT_ROCK));
                ++out.bricksPromoted;

                for (int64_t cz = 0; cz < E; ++cz)
                    for (int64_t cy = 0; cy < E; ++cy)
                        for (int64_t cx = 0; cx < E; ++cx) {
                            const MaterialId m = craftBuildingCell(
                                b, bx * E + cx - x0, by * E + cy - y0, bz * E + cz - z0);
                            if (m != MAT_ROCK) {
                                lat.setCell(bx * E + cx, by * E + cy, bz * E + cz, m);
                            } else {
                                ++out.structureCells;
                            }
                        }
            }

    if (out.bricksPromoted == 0) return out; // ran==false: the caller refuses

    // Pack every promoted brick, and classify its craft bricks into the SAME
    // Census the terrain and cover arms fill -- two readings of one field, so
    // the two byte models below are not two different walks.
    for (const BrickKey& tk : lat.promotedSorted()) {
        const CraftChunkResult r = produceCraftChunk(lat, tk, counters);
        if (!r.produced) continue;
        ++out.bricksProduced;
        if (r.anySolid) ++out.bricksWithSolid;
        out.packBytes += r.residentBytes();

        ++out.cen.chunksCandidate;
        ++out.cen.chunksWalked;
        const BrickKey base = craftBrickBaseOfTerrainBrick(tk);
        for (int32_t cbz = 0; cbz < kCraftBricksPerAxis; ++cbz)
            for (int32_t cby = 0; cby < kCraftBricksPerAxis; ++cby)
                for (int32_t cbx = 0; cbx < kCraftBricksPerAxis; ++cbx) {
                    const Brick<B>* cb = lat.craftBricks().find(
                        BrickKey{base.x + cbx, base.y + cby, base.z + cbz});
                    if (cb == nullptr) continue; // refused above; cannot happen here
                    ++out.cen.bricks;
                    const size_t solid = cb->solidCount();
                    out.cen.solidCells += int64_t(solid);
                    if (solid == 0) {
                        ++out.cen.allAir;
                        continue;
                    }
                    if (cb->isHomogeneous()) {
                        ++out.cen.homogSolid;
                        continue;
                    }
                    const size_t pal = cb->paletteSize();
                    if (solid == size_t(Brick<B>::kCells)) {
                        ++out.cen.mixedFull;
                    } else {
                        ++out.cen.mixedSurf;
                    }
                    ++out.cen.palHist[palBucket(pal)];
                    out.cen.paletteEntries += int64_t(pal);
                    out.cen.maxPalette = std::max(out.cen.maxPalette, int64_t(pal));
                    out.cen.adaptiveCellBytes +=
                        int64_t(Brick<B>::kCells) * bppFor(pal) / 8;
                }
        out.cen.bzMin = std::min(out.cen.bzMin, int64_t(tk.z));
        out.cen.bzMax = std::max(out.cen.bzMax, int64_t(tk.z));
    }

    out.solidCells = int64_t(counters.solidCellsPacked.load());
    out.ran = counters.chunksAttempted.load() != 0;
    return out;
}

struct Bytes {
    double descriptors = 0, occupancy = 0, palette = 0, palettePlan = 0, index = 0;
    double cells[4] = {}; // 1, 2, 4, 8 bpp
    double adaptive = 0;
    double quads = 0;
    double total(int bppIdx) const {
        return descriptors + occupancy + palette + index +
               (bppIdx < 0 ? adaptive : cells[bppIdx]);
    }
};

Bytes bytesFor(const Census& c, double factor) {
    Bytes b;
    const double bricks = double(c.bricks) * factor;
    const double mixed = double(c.mixed()) * factor;
    b.descriptors = bricks * kBytesDescriptor;
    b.occupancy = mixed * kBytesOccupancy;
    b.palette = double(c.paletteEntries) * factor * kBytesPaletteEntry;
    b.palettePlan = mixed * kBytesPalettePlan;
    for (int i = 0; i < 4; ++i) {
        const int bpp = 1 << i;
        b.cells[i] = mixed * double(Brick<B>::kCells) * double(bpp) / 8.0;
    }
    b.adaptive = double(c.adaptiveCellBytes) * factor;
    b.quads = double(c.quads) * factor * kBytesQuad;
    return b;
}

// The FLAT index: one uint32 per slot of a dense 3D brick array over the ring's
// own footprint and its MEASURED z envelope. Reported apart from the payload
// because it is the term plan section 4 says forces a sparse hash or an octree
// at 2.5 cm -- a design consequence, not a tuning one -- and a total that hides
// it inside "bytes" cannot make that argument.
double flatIndexBytes(const Census& c, double factor) {
    if (c.bzMax < c.bzMin) return 0.0;
    const double zSpan = double(c.bzMax - c.bzMin + 1);
    const double brickColumns = double(c.chunksCandidate) * double(kChunkBricks * kChunkBricks);
    (void)factor; // chunksCandidate is already the FULL ring, never sampled
    return brickColumns * zSpan * kBytesIndexSlot;
}

void printRing(const Ring& R, const char* label) {
    const Census& c = R.cen;
    const double factor = c.chunksWalked > 0
                              ? double(c.chunksCandidate) / double(c.chunksWalked)
                              : 0.0;
    const double mixedPct = c.bricks > 0 ? 100.0 * double(c.mixed()) / double(c.bricks) : 0.0;
    std::printf("\n--- %s   cell %.3f m   chunk %.1f m   %.0f-%.0f m ---\n", label,
                double(R.lat.cellMm) / 1000.0, double(R.lat.cellMm * kChunkCells) / 1000.0,
                R.innerM, R.outerM);
    // THE n AND THE FACTOR, ON EVERY ROW. A row without them is not a row.
    std::printf("  n: chunks walked %lld of %lld candidates  ->  extrapolation x%.3f"
                "   (bricks walked %lld)\n",
                (long long)c.chunksWalked, (long long)c.chunksCandidate, factor,
                (long long)c.bricks);
    if (c.bricks == 0) {
        std::printf("  *** ZERO BRICKS ***\n");
        return;
    }
    std::printf("  brick slots %11.0f | all-air %10.0f (%.1f%%) | all-solid 1-mat %10.0f (%.1f%%)"
                " | solid multi-mat %9.0f (%.1f%%) | MIXED %10.0f (%.1f%%)\n",
                double(c.bricks) * factor, double(c.allAir) * factor,
                100.0 * double(c.allAir) / double(c.bricks), double(c.homogSolid) * factor,
                100.0 * double(c.homogSolid) / double(c.bricks), double(c.mixedFull) * factor,
                100.0 * double(c.mixedFull) / double(c.bricks), double(c.mixed()) * factor,
                mixedPct);
    std::printf("  palette over MIXED:");
    const double mx = double(c.mixed());
    for (int i = 0; i < 6; ++i)
        std::printf("  %s:%.0f(%.1f%%,%dbpp)", kPalBucketName[i], double(c.palHist[i]) * factor,
                    mx > 0 ? 100.0 * double(c.palHist[i]) / mx : 0.0, kPalBucketBpp[i]);
    std::printf("\n  palette mean %.2f, max %lld\n",
                c.mixed() > 0 ? double(c.paletteEntries) / mx : 0.0, (long long)c.maxPalette);

    const Bytes b = bytesFor(c, factor);
    const double idx = flatIndexBytes(c, factor);
    std::printf("  bytes MiB: desc %.1f  occ %.1f  pal %.2f (plan-16B %.1f)  flat-index %.1f\n",
                mib(b.descriptors), mib(b.occupancy), mib(b.palette), mib(b.palettePlan),
                mib(idx));
    std::printf("             cells 1bpp %.1f | 2bpp %.1f | 4bpp %.1f | 8bpp %.1f | "
                "ADAPTIVE %.1f (%.2f bpp effective)\n",
                mib(b.cells[0]), mib(b.cells[1]), mib(b.cells[2]), mib(b.cells[3]),
                mib(b.adaptive),
                c.mixed() > 0 ? 8.0 * double(c.adaptiveCellBytes) /
                                    (double(c.mixed()) * double(Brick<B>::kCells))
                              : 0.0);
    std::printf("             TOTAL adaptive %.1f MiB   (2bpp %.1f, 4bpp %.1f)\n",
                mib(b.total(-1) + idx), mib(b.total(1) + idx), mib(b.total(2) + idx));
    std::printf("  quads (same walk) %.0f -> %.1f MiB at %.0f B/quad\n", double(c.quads) * factor,
                mib(b.quads), kBytesQuad);

    // MARCH STEP HEADROOM. Not a march measurement -- that is G1, a GPU pass,
    // and it belongs to the spike, not to a CPU census. But these two numbers
    // fall out of this walk for free and they are the denominators the spike's
    // cost-per-pixel needs: how DEEP a brick column is (the steps a downward
    // ray must take before it hits), and how much of the flat index is empty
    // (the empty-space-skipping headroom a DDA over the brick grid can win).
    // Reported here because the march is O(pixels) x O(steps), and only the
    // second factor is a property of the volume rather than of the frame.
    const double columns = double(c.chunksCandidate) * double(kChunkBricks * kChunkBricks);
    const double zSpan = c.bzMax >= c.bzMin ? double(c.bzMax - c.bzMin + 1) : 0.0;
    if (columns > 0.0 && zSpan > 0.0) {
        const double occupied = double(c.bricks) * factor;
        std::printf("  march headroom: %.2f bricks per brick-column, z envelope %.0f bricks -> "
                    "flat index %.2f%% occupied (%.2f%% skippable)\n",
                    occupied / columns, zSpan, 100.0 * occupied / (columns * zSpan),
                    100.0 * (1.0 - occupied / (columns * zSpan)));
    }
}

} // namespace

int main(int argc, char** argv) {
    bool cascade = false, doExp = false, verbose = true, allowSynthetic = false;
    bool detailCover = false;
    bool craft = false;
    int64_t craftBuildings = 50;   // a village, the scale the falsifier names
    double craftWm = 8.0, craftDm = 6.0, craftHm = 3.0, craftWallM = 0.20;
    int64_t craftMaterials = 4;    // stone, plaster, timber, glazing
    double coverRingM = 112.0;   // VoxelDetailAssetSubsystem's -VoxelDetailRingMeters default
    int64_t coverPitchMm = 50;   // the pitch 223 of the 230 detail plants are baked at
    double radiusM = 0.0, atXm = 0.0, atYm = 0.0, sample = 1.0, depthM = 0.0;
    int64_t voxelMm = kVoxelSizeMm;
    uint64_t seed = 20260719;
    std::string assetsDir, fineDir, coarseDir, zstdPath;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--cascade")) cascade = true;
        else if (!std::strcmp(a, "--detail-cover")) detailCover = true;
        else if (!std::strcmp(a, "--craft")) craft = true;
        else if (!std::strcmp(a, "--craft-buildings") && i + 1 < argc) craftBuildings = std::strtoll(argv[++i], nullptr, 10);
        else if (!std::strcmp(a, "--craft-wall-m") && i + 1 < argc) craftWallM = std::strtod(argv[++i], nullptr);
        else if (!std::strcmp(a, "--craft-materials") && i + 1 < argc) craftMaterials = std::strtoll(argv[++i], nullptr, 10);
        else if (!std::strcmp(a, "--craft-size") && i + 3 < argc) {
            craftWm = std::strtod(argv[i + 1], nullptr);
            craftDm = std::strtod(argv[i + 2], nullptr);
            craftHm = std::strtod(argv[i + 3], nullptr);
            i += 3;
        }
        else if (!std::strcmp(a, "--cover-ring-m") && i + 1 < argc) coverRingM = std::strtod(argv[++i], nullptr);
        else if (!std::strcmp(a, "--cover-pitch-mm") && i + 1 < argc) coverPitchMm = std::strtoll(argv[++i], nullptr, 10);
        else if (!std::strcmp(a, "--radius") && i + 1 < argc) radiusM = std::strtod(argv[++i], nullptr);
        else if (!std::strcmp(a, "--at") && i + 2 < argc) {
            atXm = std::strtod(argv[i + 1], nullptr);
            atYm = std::strtod(argv[i + 2], nullptr);
            i += 2;
        } else if (!std::strcmp(a, "--voxel-mm") && i + 1 < argc) voxelMm = std::strtoll(argv[++i], nullptr, 10);
        else if (!std::strcmp(a, "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(a, "--sample") && i + 1 < argc) sample = std::strtod(argv[++i], nullptr);
        else if (!std::strcmp(a, "--depth-m") && i + 1 < argc) depthM = std::strtod(argv[++i], nullptr);
        else if (!std::strcmp(a, "--assets") && i + 1 < argc) assetsDir = argv[++i];
        else if (!std::strcmp(a, "--tiles") && i + 1 < argc) fineDir = argv[++i];
        else if (!std::strcmp(a, "--coarse") && i + 1 < argc) coarseDir = argv[++i];
        else if (!std::strcmp(a, "--zstd") && i + 1 < argc) zstdPath = argv[++i];
        else if (!std::strcmp(a, "--exp")) doExp = true;
        else if (!std::strcmp(a, "--allow-synthetic")) allowSynthetic = true;
        else if (!std::strcmp(a, "--quiet")) verbose = false;
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a);
            return 2;
        }
    }
    if (!cascade && !detailCover && !craft && radiusM <= 0.0) {
        std::fprintf(stderr,
                     "usage: vxc_volumeprobe --cascade | --radius N | --detail-cover | --craft\n"
                     "       --craft [--craft-buildings 50] [--craft-size W D H] "
                     "[--craft-wall-m 0.20] [--craft-materials 4]\n"
                     "       --detail-cover [--cover-ring-m 112] [--cover-pitch-mm 50] --assets DIR\n"
                     "       [--at Xm Ym] [--voxel-mm 100|50|25] [--seed N] [--sample F]\n"
                     "       [--assets DIR] [--tiles DIR] [--coarse DIR] [--zstd PATH] [--exp]\n"
                     "       [--allow-synthetic]  (required to report a synthetic-ground walk)\n");
        return 2;
    }
    if (voxelMm <= 0 || (voxelMm > int64_t(kVoxelSizeMm) &&
                         voxelMm % int64_t(kVoxelSizeMm) != 0)) {
        std::fprintf(stderr, "--voxel-mm must be a positive divisor/multiple of %d\n",
                     int(kVoxelSizeMm));
        return 2;
    }
    if (sample <= 0.0 || sample > 1.0) sample = 1.0;

    bindZstd(zstdPath);

    // --- the world under census --------------------------------------------
    SyntheticTileSampler synth(seed);
    TileGridSampler coarse(seed, /*scale*/ 1);
    ITileSampler* climate = &synth;
    bool realClimate = false;
    if (!coarseDir.empty()) {
        int loaded = 0, rejected = 0;
        for (auto& e : std::filesystem::directory_iterator(coarseDir)) {
            if (e.path().extension() != ".vxtl") continue;
            if (coarse.loadTileFile(e.path())) ++loaded;
            else ++rejected;
        }
        std::printf("coarse tiles: loaded %d, rejected %d from %s\n", loaded, rejected,
                    coarseDir.c_str());
        if (loaded == 0) {
            std::fprintf(stderr, "no coarse tiles loaded; refusing to silently census synthetic "
                                 "climate under a --coarse flag\n");
            return 1;
        }
        climate = &coarse;
        realClimate = true;
    }

    FineTileSampler fine(seed, climate);
    FineDecompressor dec;
    dec.fn = &zstdInflate;
    dec.user = nullptr;
    fine.setDecompressor(dec);
    bool realGround = false;
    if (!fineDir.empty()) {
        int loaded = 0, rejected = 0;
        for (auto& e : std::filesystem::directory_iterator(fineDir)) {
            if (e.path().extension() != ".vxtl") continue;
            if (fine.loadTileFile(e.path())) ++loaded;
            else ++rejected;
        }
        std::printf("fine tiles: loaded %d, rejected %d from %s\n", loaded, rejected,
                    fineDir.c_str());
        if (loaded == 0) {
            std::fprintf(stderr, "no fine tiles loaded; refusing to silently census synthetic "
                                 "ground under a --tiles flag\n");
            return 1;
        }
        realGround = true;
    }

    Amplifier amp(seed, realGround ? static_cast<ITileSampler&>(fine)
                                   : (realClimate ? static_cast<ITileSampler&>(coarse)
                                                  : static_cast<ITileSampler&>(synth)));
    GeneratedWorld<B> gen(amp);

    LakeSampler lakeWater(fine);
    RiverSampler riverWater(fine);
    CompositeWaterSampler bakedWater(lakeWater, riverWater);
    IWaterSampler* water = realGround ? static_cast<IWaterSampler*>(&bakedWater) : nullptr;
    ProbeChannelSource channels(realGround ? &fine : nullptr, water, climate);
    gen.setAssetChannelSource(&channels);

    AssetsOnDisk assets;
    if (!assetsDir.empty()) {
        if (!assets.load(assetsDir, seed)) return 1;
        gen.setAssetField(&assets.field);
    }

    std::printf("\nvxc_volumeprobe  seed %llu  worldgen v%u  at (%.1f, %.1f) m\n",
                (unsigned long long)seed, unsigned(kWorldGenVersion), atXm, atYm);
    std::printf("terrain: %s elevation, %s climate, water datum %s, assets %s\n",
                realGround ? "REAL fine-tile" : "SYNTHETIC",
                realClimate ? "REAL coarse-tile" : "SYNTHETIC",
                water != nullptr ? "BAKED lake+river" : "NONE (dry world)",
                assets.ok ? "INSTALLED (composed at every level)" : "NONE");
    std::printf("brick %d^3, chunk %d^3 cells (%dx%dx%d bricks); sample fraction %.4f\n", B,
                kChunkCells, kChunkBricks, kChunkBricks, kChunkBricks, sample);
    std::printf("resident set: %s\n",
                depthM > 0.0
                    ? "VOLUME -- the surface shell PLUS ground below it (--depth-m). This is the "
                      "reading plan section 4's estimates were made under."
                    : "SURFACE SHELL only (--depth-m 0) -- the set the streamer meshes today. "
                      "~100% mixed BY CONSTRUCTION; see the note above censusChunk.");
    if (depthM > 0.0)
        std::printf("              depth %.1f m below the shallowest column of each brick "
                    "footprint, counted in the ring's OWN cells\n", depthM);

    // REFUSAL 2: A SYNTHETIC WALK IS NOT A CENSUS.
    //
    // The synthetic tile sampler is the FLAT FALLBACK, and on this project
    // flat-fallback numbers are known not to transfer -- the rule was earned on
    // 2026-07-27, when the GPU mesher's "38% cold-fill win" became ~5% on real
    // tiles and every number taken before it had to be discarded. It bites this
    // tool harder than most: the two deliverables ARE the mixed-brick fraction
    // and the palette histogram, and both are functions of relief and of the
    // biome surface material -- exactly what the fallback does not have.
    // Measured here, same site, 16 m disc: synthetic reads 19.9% mixed and
    // palette mean 2.82 where the real bake reads 14.9% and 3.23.
    //
    // Same shape as the zero-brick refusal and for the same reason: a synthetic
    // run and a real run must not be able to look alike in the output.
    // --allow-synthetic is the deliberate door, and it prints a banner so a
    // pasted excerpt still carries the caveat.
    // --craft is exempt: a building is AUTHORED, so its byte cost does not
    // depend on the ground under it and the fallback cannot bias it. The one
    // number that does depend on real ground is the terrain control, and the
    // craft report refuses to print that rather than printing a synthetic one.
    if ((!realGround || !realClimate) && !craft) {
        const char* what = !realGround ? (!realClimate ? "Elevation AND climate" : "Elevation")
                                       : "Climate";
        if (!allowSynthetic) {
            std::fprintf(stderr,
                         "\n*** REFUSED ***\n"
                         "%s is SYNTHETIC -- the flat fallback. The two numbers this tool exists\n"
                         "to produce, the mixed-brick fraction and the palette histogram, are\n"
                         "precisely the ones the fallback gets wrong (2026-07-27: every perf\n"
                         "number taken on it had to be discarded). Pass --tiles <s16 dir> and\n"
                         "--coarse <s1 dir> for a site the bake covers, or --allow-synthetic if\n"
                         "you really mean to census the fallback.\n",
                         what);
            return 4;
        }
        std::printf("\n!!! SYNTHETIC WALK (--allow-synthetic). %s is the FLAT FALLBACK; these\n"
                    "!!! numbers do NOT transfer to the real bake -- see 2026-07-27.\n",
                    what);
    }

    // --- --craft: the settlement model, reported and returned ---------------
    if (craft) {
        const double cellM = double(kCraftPitchMm) / 1000.0;
        auto cells = [&](double m) { return int64_t(std::llround(m / cellM)); };

        std::printf("\n=== CRAFT SETTLEMENT MODEL -- 25 mm lattice ===\n");
        std::printf("!!! THIS IS A MODEL, NOT A MEASUREMENT. There is no procedural source for\n"
                    "!!! what a player builds, so every number below follows from a building\n"
                    "!!! recipe written into this tool. The assumption-free half of the craft\n"
                    "!!! census is tests/test_craftcost.cpp, where per-pattern byte costs are\n"
                    "!!! PINNED against the format contract.\n");
        // THE MATERIAL COUNT BELONGS ON THIS LINE. It moves the answer more than
        // any other parameter here -- 1 material against 4 is 5.7 MB against
        // 43.0 MB for the same fifty buildings -- and this banner exists so that
        // a pasted excerpt still carries the recipe that produced it.
        std::printf("\n  building  %.2f x %.2f x %.2f m, wall %.2f m, %lld material(s)"
                    "  (%lld x %lld x %lld craft cells)\n",
                    craftWm, craftDm, craftHm, craftWallM, (long long)craftMaterials,
                    (long long)cells(craftWm), (long long)cells(craftDm), (long long)cells(craftHm));

        static const char* kIntensityName[3] = {"shell", "openings", "detailed"};
        struct Row { int64_t offset; int intensity; CraftResult r; };
        std::vector<Row> rows;

        for (int64_t off : {int64_t(0), int64_t(1)})
            for (int it = 0; it < 3; ++it) {
                CraftBuilding b;
                b.w = cells(craftWm); b.d = cells(craftDm); b.h = cells(craftHm);
                b.wallT = std::max<int64_t>(1, cells(craftWallM));
                b.offset = off;
                b.intensity = it;
                b.materials = int(craftMaterials);
                rows.push_back(Row{off, it, censusCraftBuilding(b)});
            }

        // THE PRODUCER MUST HAVE RUN. "Nothing was built" and "the model did not
        // run" are different answers and a byte total cannot tell them apart --
        // craftvolume.h's funnel exists for exactly this.
        bool anyRan = false;
        for (const Row& r : rows) anyRan = anyRan || r.r.ran;
        if (!anyRan) {
            std::fprintf(stderr,
                         "\n*** THE PRODUCER DID NOT RUN -- refusing to report a craft volume.\n"
                         "    No terrain brick was promoted, so this is not a result about\n"
                         "    building; it is a result about this tool.\n");
            return 1;
        }

        std::printf("\n  ONE BUILDING\n");
        std::printf("  %-6s %-9s %8s %8s %10s %12s %12s %9s\n", "align", "carve", "bricks",
                    "mixed", "solid cells", "pack KiB", "probe KiB", "floor%");
        for (const Row& row : rows) {
            const CraftResult& r = row.r;
            if (!r.ran) continue;
            const Bytes b = bytesFor(r.cen, 1.0);
            const double probe = b.descriptors + b.occupancy + b.palettePlan + b.adaptive;
            const double floorShare =
                probe > 0.0 ? 100.0 * (double(r.bricksProduced) * 64.0 * kBytesDescriptor) / probe
                            : 0.0;
            std::printf("  %-6s %-9s %8lld %8lld %10lld %12.1f %12.1f %8.1f\n",
                        row.offset == 0 ? "grid" : "off-1", kIntensityName[row.intensity],
                        (long long)r.bricksProduced, (long long)r.cen.mixed(),
                        (long long)r.solidCells, double(r.packBytes) / 1024.0, probe / 1024.0,
                        floorShare);
        }
        std::printf("  pack  = summed ChunkBrickPack::residentBytes() -- descriptors+occ+mat,\n"
                    "          the real pack, which the cover census never had.\n"
                    "  probe = desc+occ+palette-plan+adaptive, the SAME model --detail-cover\n"
                    "          prints, so these are comparable with its 25.4 / 131.6 MiB.\n"
                    "  floor%% = share that is the promotion floor rather than carved content.\n"
                    "          100%% means every byte IS the floor -- which is the ALIGNED\n"
                    "          case, and its absolute cost is the smallest on the table.\n");

        // The settlement. LINEAR BY CONSTRUCTION and said to be: buildings in a
        // settlement share no bricks, so N of them is N times one. Measuring
        // fifty separately would cost memory and buy no information.
        std::printf("\n  SETTLEMENT (linear -- buildings share no bricks)\n");
        std::printf("  %-6s %-9s", "align", "carve");
        static const int64_t kCounts[] = {1, 10, 50, 100};
        for (int64_t n : kCounts) std::printf(" %10lld", (long long)n);
        std::printf("   (MiB, pack)\n");
        for (const Row& row : rows) {
            if (!row.r.ran) continue;
            std::printf("  %-6s %-9s", row.offset == 0 ? "grid" : "off-1",
                        kIntensityName[row.intensity]);
            for (int64_t n : kCounts) {
                std::printf(" %10.1f", double(row.r.packBytes) * double(n) / (1024.0 * 1024.0));
            }
            std::printf("\n");
        }

        // The band ceiling, from the pinned floor rather than from arithmetic
        // written in a document.
        const int64_t bandChunks = int64_t(80) * 80 * 80;
        std::printf("\n  BAND: +/-40 chunks x %.2f m = +/-%.1f m, %lld brick slots\n",
                    double(kCraftChunkEdgeCells) * cellM,
                    40.0 * double(kCraftChunkEdgeCells) * cellM, (long long)bandChunks);

        // --- the falsifiers, evaluated HERE and not left to the reader -------
        const CraftResult* worst = nullptr;
        for (const Row& row : rows) {
            if (!row.r.ran) continue;
            if (worst == nullptr || row.r.packBytes > worst->packBytes) worst = &row.r;
        }
        const double settlementMB =
            double(worst->packBytes) * double(craftBuildings) / 1.0e6;
        std::printf("\n  VERDICT -- pre-registered 2026-08-26: a settlement-scale craft volume\n"
                    "  inside the band exceeding 150 MB falsifies the per-brick promotion unit.\n"
                    "      worst configuration x %lld buildings = %.1f MB\n",
                    (long long)craftBuildings, settlementMB);
        std::printf("      %s\n", settlementMB > 150.0
                                      ? ">>> FALSIFIED -- the promotion unit is too coarse <<<"
                                      : ">>> STANDS <<<");

        // --- the 2026-08-27 falsifier: craft against terrain, per m^2 -------
        //
        // COMPARED AGAINST A PUBLISHED REAL-GROUND NUMBER, NOT ONE COMPUTED HERE.
        // The control this falsifier names is "terrain on the same footprint",
        // and a settlement footprint is a different shape from any ring this
        // tool walks -- so the honest comparison normalises both to bytes per
        // square metre and uses the measurement that was already taken on REAL
        // fine tiles: docs/measurements/cover-volume-census-2026-08-19.txt,
        // grassland terrain control, 126.7 MiB over the 256 m shipping ring.
        // Recomputing it on the synthetic fallback would be worse than not
        // having it (2026-07-27: flat-fallback numbers do not transfer).
        const double kControlMiB = 126.7;
        const double kControlRingM = 256.0;
        const double kControlAreaM2 = 3.14159265358979 * kControlRingM * kControlRingM;
        const double terrainBytesPerM2 = kControlMiB * 1024.0 * 1024.0 / kControlAreaM2;

        const double footprintM2 = craftWm * craftDm * double(craftBuildings);
        const double terrainOnFootprint = terrainBytesPerM2 * footprintM2;

        std::printf("\n  TERRAIN CONTROL (published, real fine tiles -- "
                    "cover-volume-census-2026-08-19)\n");
        std::printf("      grassland terrain %.1f MiB over a %.0f m ring = %.0f B/m^2\n",
                    kControlMiB, kControlRingM, terrainBytesPerM2);
        std::printf("      settlement footprint %.0f m^2 (%lld x %.1f x %.1f m)"
                    " -> terrain there = %.2f MB\n",
                    footprintM2, (long long)craftBuildings, craftWm, craftDm,
                    terrainOnFootprint / 1.0e6);

        std::printf("\n  VERDICT -- pre-registered 2026-08-27: if a settlement a player could\n"
                    "  plausibly build costs MORE than the terrain control on the same\n"
                    "  footprint, craft is a doubling of the world budget rather than a\n"
                    "  decoration, and P3 must carry an eviction policy rather than defer one.\n");
        for (const Row& row : rows) {
            if (!row.r.ran) continue;
            const double mb = double(row.r.packBytes) * double(craftBuildings) / 1.0e6;
            std::printf("      %-6s %-9s %7.1f MB  = %5.1fx terrain   %s\n",
                        row.offset == 0 ? "grid" : "off-1", kIntensityName[row.intensity], mb,
                        mb / (terrainOnFootprint / 1.0e6),
                        mb > terrainOnFootprint / 1.0e6 ? "FIRES" : "stands");
        }
        std::printf("\n      READ BOTH NUMBERS. The ratio fires because a 3 m building is more\n"
                    "      geometry than the ground it stands on -- that is expected, not a\n"
                    "      defect. What the ratio does NOT say is that craft is unaffordable:\n"
                    "      the largest row above is %.1f MB against a brick pool sized in\n"
                    "      hundreds of MB, and against cover's own 131.6 MiB at its shipping\n"
                    "      ring. The falsifier is reported as written; the absolute column is\n"
                    "      what should decide whether P3 needs eviction.\n",
                    double(worst->packBytes) * double(craftBuildings) / 1.0e6);

        if (!realGround) {
            std::printf("\n  (Ground here is the synthetic fallback. It does not affect any craft\n"
                        "   row -- a building is authored, not generated -- and the control above\n"
                        "   is a published real-tile measurement, so neither number is synthetic.)\n");
        }
        return 0;
    }

    // --- --detail-cover: the arm, reported and returned ---------------------
    if (detailCover) {
        // A COVER CENSUS WITHOUT BANKS IS A MEASURED ZERO THAT MEANS NOTHING --
        // the same refusal this file applies to a zero-brick ring, for the same
        // reason (vxc_farwaterprobe #226). Every detail species would compose as
        // air and the answer would read as "cover is free".
        if (!assets.ok) {
            std::fprintf(stderr,
                         "--detail-cover requires --assets DIR: without banks every detail "
                         "species composes as air and the census reports a free volume.\n");
            return 1;
        }

        // WHICH CONTENT THIS VOLUME CANNOT HOLD, BY NAME, BEFORE THE NUMBER.
        // A pitch admits exactly one bake (nothing in voxel-core resamples), so
        // a species baked elsewhere is dropped per instance deep in the resolve
        // loop where it no longer has a name. Said once, here, at load.
        const std::vector<AssetCoverPitchRefusal> refused =
            assetCoverPitchRefusals(assets.manifest, uint32_t(coverPitchMm));
        std::printf("\n===========================================================\n");
        std::printf("COVER VOLUME   pitch %lld mm   ring %.0f m   seed %llu   at (%.0f, %.0f) m\n",
                    (long long)coverPitchMm, coverRingM, (unsigned long long)seed, atXm, atYm);
        std::printf("===========================================================\n");
        std::printf("refused at this pitch: %d species", int(refused.size()));
        if (!refused.empty()) {
            std::printf(" -> ");
            for (size_t i = 0; i < refused.size() && i < 12; ++i)
                std::printf("%s%s(%u mm)", i ? ", " : "", refused[i].name.c_str(),
                            unsigned(refused[i].voxelSizeMm));
            if (refused.size() > 12) std::printf(", +%d more", int(refused.size() - 12));
        }
        std::printf("\n");

        Ring R;
        R.level = 0;
        R.innerM = 0.0;
        R.outerM = coverRingM;
        R.lat = Lattice::forCell(coverPitchMm);

        Walk w;
        w.amp = &amp;
        w.gen = &gen;
        w.assets = &assets.field;
        w.banks = &assets.banks;
        w.seed = seed;
        w.camXmm = int64_t(atXm * 1000.0);
        w.camYmm = int64_t(atYm * 1000.0);
        w.depthMm = 0;   // a cover volume has no subsurface reading

        if (verbose)
            std::fprintf(stderr, "walking cover disc (%.0f m, cell %.3f m)...\n", coverRingM,
                         double(R.lat.cellMm) / 1000.0);
        censusCoverRing(w, R, sample, verbose);

        const Census& c = R.cen;
        const double factor =
            c.chunksWalked > 0 ? double(c.chunksCandidate) / double(c.chunksWalked) : 0.0;
        std::printf("\n  n: chunks walked %lld of %lld candidates  ->  extrapolation x%.3f\n",
                    (long long)c.chunksWalked, (long long)c.chunksCandidate, factor);
        if (c.bricks == 0) {
            std::printf("  *** ZERO BRICKS -- refusing to report a cover volume of nothing. "
                        "Check --assets and the site. ***\n");
            return 1;
        }

        const double occupied = double(c.mixed() + c.homogSolid) * factor;
        const double areaHa = 3.14159265358979 * coverRingM * coverRingM / 10000.0;
        std::printf("  instances composed %12.0f   (%.1f per hectare over %.2f ha)\n",
                    double(c.instances) * factor,
                    areaHa > 0 ? double(c.instances) * factor / areaHa : 0.0, areaHa);
        std::printf("  brick slots %12.0f | all-air %10.0f (%.1f%%) | all-solid %8.0f "
                    "| MIXED %10.0f (%.1f%%)\n",
                    double(c.bricks) * factor, double(c.allAir) * factor,
                    100.0 * double(c.allAir) / double(c.bricks), double(c.homogSolid) * factor,
                    double(c.mixed()) * factor, 100.0 * double(c.mixed()) / double(c.bricks));
        std::printf("  OCCUPIED BRICKS %11.0f   <- the modelled term in the doc's section 5.1\n",
                    occupied);
        std::printf("  solid cover voxels %10.0f   (%.1f per occupied brick of %d)\n",
                    double(c.solidCells) * factor,
                    occupied > 0 ? double(c.solidCells) * factor / occupied : 0.0,
                    int(Brick<B>::kCells));
        std::printf("  palette over MIXED:");
        const double mx = double(c.mixed());
        for (int i = 0; i < 6; ++i)
            std::printf("  %s:%.0f(%.1f%%)", kPalBucketName[i], double(c.palHist[i]) * factor,
                        mx > 0 ? 100.0 * double(c.palHist[i]) / mx : 0.0);
        std::printf("\n  palette mean %.2f, max %lld\n",
                    mx > 0 ? double(c.paletteEntries) / mx : 0.0, (long long)c.maxPalette);

        const Bytes b = bytesFor(c, factor);
        const double payload = b.descriptors + b.occupancy + b.palettePlan + b.adaptive;
        const double idx = flatIndexBytes(c, factor);
        std::printf("\n  bytes MiB: desc %.2f  occ %.2f  pal(16B) %.2f  ADAPTIVE cells %.2f "
                    "(%.2f bpp effective)\n",
                    mib(b.descriptors), mib(b.occupancy), mib(b.palettePlan), mib(b.adaptive),
                    c.mixed() > 0 ? 8.0 * double(c.adaptiveCellBytes) /
                                        (double(c.mixed()) * double(Brick<B>::kCells))
                                  : 0.0);
        std::printf("  quads (shared walk, cross-check only) %.0f\n", double(c.quads) * factor);
        std::printf("\n  >>> COVER VOLUME PAYLOAD: %.1f MiB  <<<\n", mib(payload));
        std::printf("      (desc + occ + 16 B palette + adaptive cells; the number section 5.1 "
                    "predicts at 16-35 MiB)\n");
        std::printf("  flat 3D brick index over the same footprint: %.1f MiB -- REPORTED APART "
                    "AND IT IS THE WRONG STRUCTURE HERE.\n", mib(idx));
        std::printf("      Cover is a thin shell over the landscape's full relief, the worst case "
                    "a dense index can be handed;\n"
                    "      %.3f%% of its slots are occupied. Read the payload for what a cover "
                    "volume costs and this for\n"
                    "      what addressing it densely would cost -- section 4's 2.5 cm finding is "
                    "the precedent for keeping them apart.\n",
                    idx > 0 ? 100.0 * occupied * kBytesIndexSlot / idx : 0.0);

        // THE FALSIFIER, EVALUATED RATHER THAN LEFT TO THE READER.
        std::printf("\n  VERDICT against docs/detail-assets-in-the-volume-2026-08-19.md 5.1:\n");
        if (mib(payload) >= 500.0)
            std::printf("      FALSIFIED. Payload %.1f MiB >= 500 MiB: the doc is wrong and\n"
                        "      ray-marching-plan-2026-08-19.md:795-797 is right -- the coupling "
                        "holds.\n", mib(payload));
        else
            std::printf("      STANDS. Payload %.1f MiB < 500 MiB, against 1,110 MiB measured for "
                        "R0-at-5cm.\n", mib(payload));
        return 0;
    }

    // --- the resolutions to walk -------------------------------------------
    std::vector<int64_t> baseSizes;
    if (doExp) baseSizes = {100, 50, 25};
    else baseSizes = {voxelMm};

    struct RunResult {
        int64_t baseMm = 100;
        std::vector<Ring> rings;
        Census total;
        bool zeroRing = false;
        int zeroLevel = -1;
    };
    std::vector<RunResult> runs;

    for (const int64_t baseMm : baseSizes) {
        RunResult run;
        run.baseMm = baseMm;

        // THE CASCADE, mirrored: {0,128} {128,256} ... {2048,4096} metres, one
        // level per ring, chunk footprint doubling with the level. `--radius`
        // collapses it to a single disc at the base size, which is a different
        // question (10 cm everywhere) and is labelled as one.
        if (cascade) {
            static const double kRingM[6][2] = {{0, 128},    {128, 256},   {256, 512},
                                                {512, 1024}, {1024, 2048}, {2048, 4096}};
            for (int L = 0; L < 6; ++L) {
                Ring R;
                R.level = L;
                R.innerM = kRingM[L][0];
                R.outerM = kRingM[L][1];
                R.lat = Lattice::forCell(baseMm << L);
                run.rings.push_back(R);
            }
        } else {
            Ring R;
            R.level = 0;
            R.innerM = 0.0;
            R.outerM = radiusM;
            R.lat = Lattice::forCell(baseMm);
            run.rings.push_back(R);
        }

        Walk w;
        w.amp = &amp;
        w.gen = &gen;
        w.assets = assets.ok ? &assets.field : nullptr;
        w.banks = assets.ok ? &assets.banks : nullptr;
        w.seed = seed;
        w.camXmm = int64_t(atXm * 1000.0);
        w.camYmm = int64_t(atYm * 1000.0);
        w.depthMm = int64_t(depthM * 1000.0);

        std::printf("\n===========================================================\n");
        std::printf("BASE VOXEL %lld mm%s\n", (long long)baseMm,
                    baseMm < int64_t(kVoxelSizeMm)
                        ? "   [SUB-LATTICE: z exact, xy SATURATED at 100 mm -- see header]"
                        : "");
        std::printf("===========================================================\n");

        for (Ring& R : run.rings) {
            if (verbose)
                std::fprintf(stderr, "walking ring L%d (%.0f-%.0f m, cell %.3f m)...\n", R.level,
                             R.innerM, R.outerM, double(R.lat.cellMm) / 1000.0);
            censusRing(w, R, sample, verbose);
            char label[64];
            std::snprintf(label, sizeof(label), "RING L%d", R.level);
            printRing(R, label);
            if (R.cen.bricks == 0) {
                run.zeroRing = true;
                if (run.zeroLevel < 0) run.zeroLevel = R.level;
            }
            run.total.merge(R.cen);
        }
        runs.push_back(std::move(run));
    }

    // --- REFUSAL: a zero-brick ring is not a cheap ring ---------------------
    for (const RunResult& run : runs) {
        if (!run.zeroRing) continue;
        std::printf("\n");
        std::fprintf(stderr,
                     "\n*** REFUSED ***\n"
                     "Ring L%d produced ZERO BRICKS at base %lld mm. An absent statistic and a\n"
                     "measured zero print the same character and mean opposite things, so this\n"
                     "cascade is NOT reported as a result. Check that the site (--at %.0f %.0f m)\n"
                     "is covered by the tiles in --tiles, and that the ring's annulus is not\n"
                     "empty at this chunk size.\n",
                     run.zeroLevel, (long long)run.baseMm, atXm, atYm);
        return 3;
    }

    // --- totals -------------------------------------------------------------
    for (const RunResult& run : runs) {
        // Per-ring extrapolation factors differ, so the TOTAL is summed from
        // already-extrapolated rings rather than from raw counts with one mean
        // factor -- averaging the factor is how a sampled census quietly
        // reweights its own rings.
        Bytes tb;
        double idx = 0.0, bricks = 0.0, mixed = 0.0, allAir = 0.0, homog = 0.0, mixedFull = 0.0;
        double quads = 0.0, palHist[6] = {}, palEntries = 0.0;
        for (const Ring& R : run.rings) {
            const Census& c = R.cen;
            if (c.chunksWalked == 0) continue;
            const double f = double(c.chunksCandidate) / double(c.chunksWalked);
            const Bytes b = bytesFor(c, f);
            tb.descriptors += b.descriptors;
            tb.occupancy += b.occupancy;
            tb.palette += b.palette;
            tb.palettePlan += b.palettePlan;
            for (int i = 0; i < 4; ++i) tb.cells[i] += b.cells[i];
            tb.adaptive += b.adaptive;
            tb.quads += b.quads;
            idx += flatIndexBytes(c, f);
            bricks += double(c.bricks) * f;
            mixed += double(c.mixed()) * f;
            allAir += double(c.allAir) * f;
            homog += double(c.homogSolid) * f;
            mixedFull += double(c.mixedFull) * f;
            quads += double(c.quads) * f;
            palEntries += double(c.paletteEntries) * f;
            for (int i = 0; i < 6; ++i) palHist[i] += double(c.palHist[i]) * f;
        }
        std::printf("\n=== TOTAL, base %lld mm, %s ===\n", (long long)run.baseMm,
                    cascade ? "six-ring cascade to 4 km" : "single disc");
        std::printf("  brick slots %.0f | all-air %.0f (%.1f%%) | all-solid 1-mat %.0f (%.1f%%) | "
                    "solid multi-mat %.0f (%.1f%%) | MIXED %.0f (%.1f%%)\n",
                    bricks, allAir, bricks > 0 ? 100.0 * allAir / bricks : 0.0, homog,
                    bricks > 0 ? 100.0 * homog / bricks : 0.0, mixedFull,
                    bricks > 0 ? 100.0 * mixedFull / bricks : 0.0, mixed,
                    bricks > 0 ? 100.0 * mixed / bricks : 0.0);
        std::printf("  palette over MIXED:");
        for (int i = 0; i < 6; ++i)
            std::printf("  %s:%.0f(%.1f%%)", kPalBucketName[i], palHist[i],
                        mixed > 0 ? 100.0 * palHist[i] / mixed : 0.0);
        std::printf("\n  palette mean %.2f\n", mixed > 0 ? palEntries / mixed : 0.0);
        std::printf("  MiB: desc %.1f  occ %.1f  pal %.2f (plan-16B %.1f)  flat-index %.1f\n",
                    mib(tb.descriptors), mib(tb.occupancy), mib(tb.palette), mib(tb.palettePlan),
                    mib(idx));
        std::printf("       cells 1bpp %.1f | 2bpp %.1f | 4bpp %.1f | 8bpp %.1f | ADAPTIVE %.1f\n",
                    mib(tb.cells[0]), mib(tb.cells[1]), mib(tb.cells[2]), mib(tb.cells[3]),
                    mib(tb.adaptive));
        std::printf("\n  RESIDENT SET (desc + occ + pal + flat-index + cells):\n");
        const char* names[5] = {"1 bpp", "2 bpp", "4 bpp", "8 bpp", "ADAPTIVE"};
        for (int i = 0; i < 5; ++i) {
            const double cells = i < 4 ? tb.cells[i] : tb.adaptive;
            const double tot = tb.descriptors + tb.occupancy + tb.palette + idx + cells;
            std::printf("    %-9s %8.1f MiB   (%.2fx today's %.0f MiB quad pool)\n", names[i],
                        mib(tot), mib(tot) / kTodayPoolMiB, kTodayPoolMiB);
        }
        std::printf("\n  SAME WALK, QUADS: %.0f quads -> %.1f MiB at %.0f B/quad "
                    "(%.2fx today's %.0f MiB)\n",
                    quads, mib(tb.quads), kBytesQuad, mib(tb.quads) / kTodayPoolMiB,
                    kTodayPoolMiB);
    }

    // --- the scaling exponent ----------------------------------------------
    if (doExp && runs.size() >= 2) {
        std::printf("\n=== SCALING: mixed-brick count against resolution ===\n");
        std::printf("  The plan predicts exponent 2.0 (x4 mixed bricks per halving of the\n"
                    "  voxel) and prices the 5 cm row on it. At 2.5 that row is ~40%% wrong.\n");
        std::vector<double> invCell, mixedN;
        for (const RunResult& run : runs) {
            double mixed = 0.0, bricks = 0.0;
            int64_t walked = 0, cand = 0;
            for (const Ring& R : run.rings) {
                if (R.cen.chunksWalked == 0) continue;
                const double f = double(R.cen.chunksCandidate) / double(R.cen.chunksWalked);
                mixed += double(R.cen.mixed()) * f;
                bricks += double(R.cen.bricks) * f;
                walked += R.cen.chunksWalked;
                cand += R.cen.chunksCandidate;
            }
            invCell.push_back(100.0 / double(run.baseMm));
            mixedN.push_back(mixed);
            std::printf("  base %3lld mm: MIXED %14.0f  (of %.0f slots, %.1f%%)   "
                        "n = %lld/%lld chunks\n",
                        (long long)run.baseMm, mixed, bricks,
                        bricks > 0 ? 100.0 * mixed / bricks : 0.0, (long long)walked,
                        (long long)cand);
        }
        for (size_t i = 1; i < mixedN.size(); ++i)
            if (mixedN[i - 1] > 0)
                std::printf("  %lld mm -> %lld mm: x%.3f  (exponent %.3f)\n",
                            (long long)runs[i - 1].baseMm, (long long)runs[i].baseMm,
                            mixedN[i] / mixedN[i - 1],
                            std::log(mixedN[i] / mixedN[i - 1]) / std::log(2.0));
        std::printf("  FITTED EXPONENT (log mixed vs log 1/cell): %.3f\n",
                    logLogSlope(invCell, mixedN));
    }

    return 0;
}
