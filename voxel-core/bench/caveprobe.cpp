// vxc_caveprobe -- IS AN INTERVAL BAND WORTH BUILDING? The falsification test.
//
// THE ONE QUESTION, AND WHY IT IS ASKED BEFORE ANY CODE IS WRITTEN.
//
// Underground streaming today admits chunks from a HALF-SPACE: `FFootprintBand`
// (ue-project/.../VoxelFootprintBand.h) carries one number per footprint,
// `SolidBelowVoxel`, meaning "everything strictly below this z is solid", and a
// chunk is skipped when its apron top falls under it. That half-space is what
// makes the underground affordable at all -- 76-79% of level-0 worker output
// meshes to zero quads today.
//
// The karst plan replaces the cave generator with a hydrology-routed conduit
// network that is PREVALENT and DEEP. A half-space cannot express "air at 30 m,
// solid rock from 40 to 110 m, air again at 120 m": the single bound collapses
// to the deepest air in the whole footprint, so the skip stops firing exactly
// where the new caves live. The proposed replacement is an INTERVAL BAND -- up
// to N z-spans per footprint instead of one -- and the entire residency plan
// rests on it saving enough work to be worth the complexity.
//
// Whether it does is a measurement, not an opinion, and TODAY'S caves can make
// it. They are sparse and shallow rather than prevalent and deep, so they are
// not the world the new plan builds -- but a footprint that already crosses a
// tunnel at 12 m and a cavern storey at 80 m has exactly the interval structure
// in question. If the mask cannot save its keep on this world, the pivot is
// wrong, and finding that out costs a day instead of a month.
//
// -- THE BAR, STATED BEFORE THE RUN ----------------------------------------
//
// The plan's claim is that interval-gated admission drops a full-depth column
// from ~63 level-0 chunks to ~8-14. Expressed as a ratio against today's
// half-space rule over the same window, the pivot is worth building at a
// >=50% reduction in admitted chunks and is dead below ~25%.
//
// -- THE ENGINE'S BINDING, NOT A SECOND ONE --------------------------------
//
// Air is decided by `Amplifier::materialAt(col, vz) == MAT_AIR` -- the exact
// call collision uses (`vxc::World::materialAt` -> `gen_.materialAt`) and the
// exact one `VoxelizeMain` mirrors. This probe does NOT re-derive intervals
// from `CaveColumn`/`CavernColumn` internals, which is how a probe ends up
// measuring a world the engine does not run; it walks the shipped predicate and
// records where it says air. Four separate nights have been lost in this repo
// to the other choice -- see the memory note
// `voxelsim-instrument-must-run-the-engine-binding`, and rebuild this binary
// before quoting it, because no engine build refreshes voxel-core's benches.
//
// -- WHAT IT DELIBERATELY DOES NOT MEASURE ----------------------------------
//
// Not quads, not VRAM, not frame time. Admitted CHUNK COUNT per footprint is
// the quantity the residency pivot changes; everything downstream of it belongs
// to `vxc_volumeprobe` and to an in-engine leg. Reporting a frame-time
// consequence from a headless chunk count would be exactly the inference this
// project has retracted before.
//
// Usage:
//   vxc_caveprobe --fine-dir DIR [--zstd PATH] [--center VX,VY]
//                 [--span N] [--depth-m M] [--stride S] [--caps 1,2,4,8]

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/core.h"
#include "voxelcore/tilestore.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace vxc;

namespace {

// --- runtime zstd, bound the way the game binds it -------------------------
// voxel-core links no zstd of its own (tilestore.h's injected
// FineDecompressor); without one bound every CODEC_ZSTD tile is REFUSED at
// load, so a refusal here is fatal rather than a smaller sample.
using ZstdDecompressFn = size_t (*)(void*, size_t, const void*, size_t);
using ZstdIsErrorFn = unsigned (*)(size_t);
ZstdDecompressFn gZstdDecompress = nullptr;
ZstdIsErrorFn gZstdIsError = nullptr;
std::string gZstdPath;

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

bool bindZstd(const std::vector<std::string>& candidates) {
    for (const std::string& c : candidates) {
        if (c.empty()) continue;
        void* h = openLib(c.c_str());
        if (!h) continue;
        auto* d = reinterpret_cast<ZstdDecompressFn>(symbol(h, "ZSTD_decompress"));
        auto* e = reinterpret_cast<ZstdIsErrorFn>(symbol(h, "ZSTD_isError"));
        if (!d || !e) continue;
        gZstdDecompress = d;
        gZstdIsError = e;
        gZstdPath = c;
        return true;
    }
    return false;
}

// --- intervals --------------------------------------------------------------

// The level-0 chunk edge. voxel-core deals in 8-voxel BRICKS and has no chunk
// constant; 32 voxels (3.2 m) is the engine's level-0 chunk
// (ue-project/Source/VoxelEarth/VoxelCoords.h: BrickEdgeVoxels *
// ChunkEdgeBricks). It is restated here because admission is what this probe
// measures and admission is denominated in chunks -- and restated WITH its
// source, because a silently duplicated constant is this repo's most-repeated
// bug (see hash_channel_registry.h's own header for the last one).
constexpr int64_t kL0ChunkEdgeVoxels = 32;

struct Span {
    int64_t lo = 0;  // inclusive voxel z
    int64_t hi = 0;  // inclusive voxel z
};

// Maximal runs of set bits in `air`, whose index i means voxel z = zTop - i.
// Returned in DESCENDING z, i.e. shallowest span first, which is the order a
// band would be walked from the surface down.
std::vector<Span> runsOf(const std::vector<uint8_t>& air, int64_t zTop) {
    std::vector<Span> out;
    size_t i = 0;
    while (i < air.size()) {
        if (!air[i]) { ++i; continue; }
        const size_t start = i;
        while (i < air.size() && air[i]) ++i;
        // i is one past the run's last index
        out.push_back(Span{zTop - static_cast<int64_t>(i - 1), zTop - static_cast<int64_t>(start)});
    }
    return out;
}

// Collapse `spans` to at most `cap` by repeatedly closing the SMALLEST gap.
// Closing a gap is conservative -- the merged span covers everything both
// spans covered plus solid rock between them -- which is the same asymmetry
// `VoxelFootprintBand.h` already states: over-admitting wastes work, under-
// admitting is a hole in the world. A cap of 1 reproduces today's half-space.
std::vector<Span> capSpans(std::vector<Span> spans, int cap) {
    if (cap < 1) cap = 1;
    while (static_cast<int>(spans.size()) > cap) {
        size_t best = 0;
        int64_t bestGap = INT64_MAX;
        for (size_t i = 0; i + 1 < spans.size(); ++i) {
            // spans are descending in z: spans[i].lo is above spans[i+1].hi
            const int64_t gap = spans[i].lo - spans[i + 1].hi;
            if (gap < bestGap) { bestGap = gap; best = i; }
        }
        spans[best].lo = spans[best + 1].lo;
        spans.erase(spans.begin() + static_cast<long>(best) + 1);
    }
    return spans;
}

// Distinct level-0 chunk z indices touched by any span. This is the quantity
// admission actually spends: one chunk admitted is one worker job, one
// residency record and one pool range.
// DISTINCT chunk indices, not the sum of per-span counts. Two spans separated
// by less than one chunk edge share a boundary chunk, and summing counts it
// twice -- which made the UNCAPPED set report MORE admitted chunks than a
// capped one, an impossibility (merging can only ever add chunks) and the tell
// that found this. Spans arrive descending in z and disjoint, so their chunk
// ranges are non-increasing and one comparison against the previous span's
// bottom chunk is enough; no set is needed.
int64_t chunksTouched(const std::vector<Span>& spans) {
    int64_t n = 0;
    bool first = true;
    int64_t prevBottom = 0;
    for (const Span& s : spans) {
        const int64_t bottom = floorDiv(s.lo, kL0ChunkEdgeVoxels);
        const int64_t top = floorDiv(s.hi, kL0ChunkEdgeVoxels);
        int64_t k = top - bottom + 1;
        if (!first && top == prevBottom) --k;   // shared boundary chunk
        n += k;
        prevBottom = bottom;
        first = false;
    }
    return n;
}

void reportDist(const char* label, std::vector<int64_t> v) {
    if (v.empty()) { std::printf("  %-30s (no samples)\n", label); return; }
    std::sort(v.begin(), v.end());
    double mean = 0.0;
    for (int64_t x : v) mean += static_cast<double>(x);
    mean /= static_cast<double>(v.size());
    const auto at = [&](int p) {
        size_t i = (v.size() * static_cast<size_t>(p)) / 100;
        if (i >= v.size()) i = v.size() - 1;
        return v[i];
    };
    std::printf("  %-30s mean %8.2f   p50 %6" PRId64 "   p90 %6" PRId64
                "   p99 %6" PRId64 "   max %6" PRId64 "\n",
                label, mean, at(50), at(90), at(99), v.back());
}

}  // namespace

int main(int argc, char** argv) {
    std::string fineDir, zstdPath, centerSel, capsSel = "1,2,4,8";
    int span = 24;       // footprints per side of the sampled square
    int depthM = 200;    // how far below the surface to look
    int stride = 1;      // voxel columns skipped inside a footprint

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if (!std::strcmp(a, "--fine-dir")) fineDir = next(a);
        else if (!std::strcmp(a, "--zstd")) zstdPath = next(a);
        else if (!std::strcmp(a, "--center")) centerSel = next(a);
        else if (!std::strcmp(a, "--span")) span = std::atoi(next(a));
        else if (!std::strcmp(a, "--depth-m")) depthM = std::atoi(next(a));
        else if (!std::strcmp(a, "--stride")) stride = std::atoi(next(a));
        else if (!std::strcmp(a, "--caps")) capsSel = next(a);
        else { std::fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    if (fineDir.empty()) {
        std::fprintf(stderr,
                     "usage: vxc_caveprobe --fine-dir DIR [--zstd PATH] [--center VX,VY]\n"
                     "                     [--span N] [--depth-m M] [--stride S] [--caps 1,2,4,8]\n");
        return 2;
    }
    if (span < 1) span = 1;
    if (depthM < 1) depthM = 1;
    if (stride < 1) stride = 1;

    std::vector<int> caps;
    for (const char* p = capsSel.c_str(); *p;) {
        caps.push_back(std::atoi(p));
        while (*p && *p != ',') ++p;
        if (*p == ',') ++p;
    }
    if (caps.empty()) caps.push_back(1);
    std::sort(caps.begin(), caps.end());

    {
        std::vector<std::string> cands;
        if (!zstdPath.empty()) cands.push_back(zstdPath);
#if defined(_WIN32)
        cands.push_back("libzstd.dll");
        cands.push_back("zstd.dll");
#else
        cands.push_back("libzstd.so.1");
        cands.push_back("libzstd.so");
#endif
        if (bindZstd(cands)) std::printf("zstd: bound from '%s'\n", gZstdPath.c_str());
        else std::printf("zstd: NOT BOUND -- every CODEC_ZSTD tile will be refused\n");
    }

    FineDecompressor dec;
    dec.fn = &zstdInflate;
    dec.user = nullptr;

    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(fineDir)) {
        if (e.is_regular_file() && e.path().extension() == ".vxtl") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) { std::fprintf(stderr, "no .vxtl under %s\n", fineDir.c_str()); return 2; }

    uint64_t seed = 0;
    {
        auto bytes = readFileBytes(files.front());
        if (!bytes) { std::fprintf(stderr, "cannot read %s\n", files.front().string().c_str()); return 1; }
        FineError err = FineError::kNone;
        auto t = FineTile::parse(std::move(*bytes), dec, &err);
        if (!t) {
            std::fprintf(stderr, "cannot parse %s (%s)\n", files.front().string().c_str(),
                         fineErrorName(err));
            return 1;
        }
        seed = t->seed();
    }

    FineTileSampler fine(seed, nullptr);
    fine.setDecompressor(dec);
    int loaded = 0, refused = 0;
    int64_t anyTileVx = 0, anyTileVy = 0;
    for (const auto& f : files) {
        FineError err = FineError::kNone;
        auto bytes = readFileBytes(f);
        if (!bytes) { ++refused; continue; }
        auto t = FineTile::parse(std::move(*bytes), dec, &err);
        if (!t) { std::fprintf(stderr, "REFUSED %s (%s)\n", f.string().c_str(), fineErrorName(err)); ++refused; continue; }
        const int32_t tx = t->tileX(), ty = t->tileY();
        const uint32_t sz = t->size();
        if (!fine.loadTile(std::move(*t))) { ++refused; continue; }
        if (loaded == 0) {
            // The sampler owns the pixel size (FineTileSampler::pixelSizeMm,
            // ITileSampler's contract); FineTile does not expose one. Asking
            // the sampler is also what the engine does.
            const int64_t pxMm = fine.pixelSizeMm();
            // Centre of the first tile, in voxels -- the default sample site,
            // so a run with no --center still lands on baked ground.
            anyTileVx = (static_cast<int64_t>(tx) * sz + sz / 2) * pxMm / kVoxelSizeMm;
            anyTileVy = (static_cast<int64_t>(ty) * sz + sz / 2) * pxMm / kVoxelSizeMm;
        }
        ++loaded;
    }
    std::printf("tiles: loaded=%d refused=%d  seed=%" PRIu64 "\n", loaded, refused, seed);
    if (loaded == 0) { std::fprintf(stderr, "nothing loaded -- refusing to report\n"); return 1; }

    int64_t cx = anyTileVx, cy = anyTileVy;
    if (!centerSel.empty()) {
        long long a = 0, b = 0;
        if (std::sscanf(centerSel.c_str(), "%lld,%lld", &a, &b) == 2) { cx = a; cy = b; }
    }

    Amplifier amp(seed, fine);

    const int64_t edge = kL0ChunkEdgeVoxels;
    const int64_t depthVox = static_cast<int64_t>(depthM) * 1000 / kVoxelSizeMm;

    std::printf("\nfootprints: %dx%d level-0 (%" PRId64 " voxels = %.1f m each), "
                "centre (%" PRId64 ",%" PRId64 ")\n",
                span, span, edge, double(edge * kVoxelSizeMm) / 1000.0, cx, cy);
    std::printf("window: %d m below each footprint's own max surface (%" PRId64 " voxels); "
                "column stride %d\n", depthM, depthVox, stride);
    std::printf("air predicate: Amplifier::materialAt(col, vz) == MAT_AIR "
                "(the collision/mirror binding)\n\n");

    std::vector<int64_t> nSpans, solidPctV, chunkAll;
    std::vector<std::vector<int64_t>> chunkCap(caps.size());
    int64_t footprintsWithAir = 0, footprints = 0;

    const int64_t fx0 = floorDiv(cx, edge) - span / 2;
    const int64_t fy0 = floorDiv(cy, edge) - span / 2;

    std::vector<uint8_t> air(static_cast<size_t>(depthVox), 0);
    std::vector<ColumnSample> cols;

    for (int64_t fy = 0; fy < span; ++fy) {
        for (int64_t fx = 0; fx < span; ++fx) {
            const int64_t vx0 = (fx0 + fx) * edge;
            const int64_t vy0 = (fy0 + fy) * edge;

            // Sample the footprint's columns ONCE. Amplifier::column is the
            // expensive call here (caveColumnFor alone was measured at 24% of
            // it, later 68% of what remained), and the window's top is the MAX
            // surface over the footprint -- which is what makes the window
            // cover every column's subsurface, not just the deepest column's --
            // so the columns have to be held rather than recomputed.
            cols.clear();
            int64_t zTop = INT64_MIN;
            for (int64_t dy = 0; dy < edge; dy += stride) {
                for (int64_t dx = 0; dx < edge; dx += stride) {
                    cols.push_back(amp.column(vx0 + dx, vy0 + dy));
                    const int64_t s =
                        floorDiv(static_cast<int64_t>(cols.back().surfaceMm), int64_t(kVoxelSizeMm));
                    if (s > zTop) zTop = s;
                }
            }
            if (zTop == INT64_MIN) continue;

            // Union of SUBSURFACE air over the footprint.
            //
            // THE WINDOW TOP IS THE FOOTPRINT'S MAX SURFACE, SO EACH COLUMN
            // MUST BE CLIPPED TO ITS OWN. Without this clip every column whose
            // ground sits below the footprint maximum contributes the open air
            // above it, and on any sloping ground that is nearly every column:
            // the probe then reports a near-surface "air span" in ~100% of
            // footprints and the whole measurement becomes a reading of relief
            // rather than of caves. That is exactly what the first run of this
            // probe did, and the tell was 100% of footprints having air while
            // the columns measured 99% solid.
            std::fill(air.begin(), air.end(), uint8_t(0));
            for (const ColumnSample& col : cols) {
                // The topmost SOLID voxel, derived from stratigraphyAt's own
                // test rather than guessed: it takes the voxel CENTRE
                // (vz*kVoxelSizeMm + kVoxelSizeMm/2) and calls anything above
                // the surface air. floorDiv(surfaceMm, kVoxelSizeMm) is
                // therefore still an AIR voxel whenever the surface falls in
                // the lower half of its cell -- which is half of all columns,
                // and which made the probe report a near-surface air span in
                // 99.8% of footprints on its second run.
                const int64_t colTop = floorDiv(
                    static_cast<int64_t>(col.surfaceMm) - int64_t(kVoxelSizeMm) / 2,
                    int64_t(kVoxelSizeMm));
                for (int64_t i = 0; i < depthVox; ++i) {
                    const int64_t vz = zTop - i;
                    if (vz > colTop) continue;   // above THIS column's ground: not a cave
                    if (air[static_cast<size_t>(i)]) continue;
                    if (amp.materialAt(col, vz) == MAT_AIR)
                        air[static_cast<size_t>(i)] = 1;
                }
            }

            ++footprints;
            const std::vector<Span> spans = runsOf(air, zTop);
            if (spans.empty()) {
                // Provably solid for the whole window: every rule admits zero.
                nSpans.push_back(0);
                solidPctV.push_back(100);
                chunkAll.push_back(0);
                for (size_t k = 0; k < caps.size(); ++k) chunkCap[k].push_back(0);
                continue;
            }
            ++footprintsWithAir;

            int64_t airVox = 0;
            for (const Span& s : spans) airVox += (s.hi - s.lo + 1);
            nSpans.push_back(static_cast<int64_t>(spans.size()));
            solidPctV.push_back(100 - (airVox * 100) / depthVox);
            chunkAll.push_back(chunksTouched(spans));
            for (size_t k = 0; k < caps.size(); ++k)
                chunkCap[k].push_back(chunksTouched(capSpans(spans, caps[k])));
        }
    }

    std::printf("footprints sampled: %" PRId64 "  (with any subsurface air: %" PRId64
                ", %.1f%%)\n\n", footprints, footprintsWithAir,
                footprints ? 100.0 * double(footprintsWithAir) / double(footprints) : 0.0);

    std::printf("PER-FOOTPRINT STRUCTURE\n");
    reportDist("air spans", nSpans);
    reportDist("column provably solid (%)", solidPctV);
    std::printf("\nADMITTED LEVEL-0 CHUNKS PER FOOTPRINT, by band capacity\n");
    std::printf("  (cap 1 IS today's half-space rule; 'exact' is the unbounded interval set)\n");
    for (size_t k = 0; k < caps.size(); ++k) {
        char lbl[64];
        std::snprintf(lbl, sizeof lbl, "cap %d span(s)", caps[k]);
        reportDist(lbl, chunkCap[k]);
    }
    reportDist("exact (uncapped)", chunkAll);

    // --- the verdict, against the bar stated in the header -----------------
    //
    // TWO POPULATIONS, REPORTED SEPARATELY, AND THIS IS THE WHOLE READING.
    // A footprint with no subsurface air admits one chunk under every rule, so
    // it contributes a 0% saving while telling you nothing about the mechanism.
    // Today's caves are sparse, so those footprints DOMINATE the sample and
    // drag the mean toward zero -- which reads as "the interval band does not
    // help" when what it means is "there was nothing here to skip". The karst
    // plan's world is the opposite: nearly every footprint crosses something.
    // So the number that transfers is the CONDITIONAL one, over footprints
    // that actually contain subsurface air, and the unconditional mean is
    // printed beside it rather than instead of it.
    const auto meanOf = [](const std::vector<int64_t>& v) {
        if (v.empty()) return 0.0;
        double m = 0.0;
        for (int64_t x : v) m += double(x);
        return m / double(v.size());
    };
    // "Has air" is nSpans > 0, which keeps both arms over the same footprints.
    const auto meanWhereAir = [&](const std::vector<int64_t>& v) {
        double m = 0.0;
        int64_t k = 0;
        for (size_t i = 0; i < v.size() && i < nSpans.size(); ++i) {
            if (nSpans[i] <= 0) continue;
            m += double(v[i]);
            ++k;
        }
        return k ? m / double(k) : 0.0;
    };

    const double base = meanOf(chunkCap[0]);   // caps is sorted: [0] is the smallest
    const double baseAir = meanWhereAir(chunkCap[0]);
    std::printf("\nVERDICT -- reduction against cap %d\n", caps[0]);
    std::printf("  %-8s %20s %20s\n", "", "ALL footprints", "footprints WITH air");
    for (size_t k = 1; k < caps.size(); ++k) {
        const double m = meanOf(chunkCap[k]);
        const double ma = meanWhereAir(chunkCap[k]);
        std::printf("  cap %-4d %9.2f %7.1f%% %9.2f %7.1f%%\n", caps[k],
                    m, base > 0.0 ? 100.0 * (base - m) / base : 0.0,
                    ma, baseAir > 0.0 ? 100.0 * (baseAir - ma) / baseAir : 0.0);
    }
    const double exact = meanOf(chunkAll);
    const double exactAir = meanWhereAir(chunkAll);
    std::printf("  exact    %9.2f %7.1f%% %9.2f %7.1f%%\n",
                exact, base > 0.0 ? 100.0 * (base - exact) / base : 0.0,
                exactAir, baseAir > 0.0 ? 100.0 * (baseAir - exactAir) / baseAir : 0.0);
    std::printf("  cap %d baseline: %.2f chunks over all, %.2f where there is air\n",
                caps[0], base, baseAir);
    std::printf("\n  Bar: >=50%% is worth building, <=25%% kills the pivot.\n");
    std::printf("  NOTE: measured on TODAY'S sparse shallow caves. The karst plan's\n");
    std::printf("  world has more storeys, which can only widen this gap -- but that\n");
    std::printf("  is an argument, not a measurement, and it is not evidence.\n");
    return 0;
}
