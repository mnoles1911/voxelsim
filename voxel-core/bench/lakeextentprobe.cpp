// vxc_lakeextentprobe -- do registered water-holding basins produce EMPTY
// extent masks, and what would testing the extent on the RECONSTRUCTED SPLINE
// instead of the control lattice change, over the real bv26 wet-block tiles?
//
// WHAT IT FOUND (2026-08-10, the reason it exists -- keep for reruns). The
// suspicion was that `LakeSampler::maskFor`'s lattice-based extent loses most
// of the wet block's 2,052 registered basins ("94 of the first 200 on tile
// (-4,-5) come out empty"). That number was an artifact of an offline replay
// decoding the lattice with LSB = the wire `quant` CODE (1) instead of the
// millimetres it encodes (QUANT_MM[1] = 100) -- relief compressed 100x, seeds
// stranded above their datums. Replayed here through the shipped decoder and
// the shipped fill: 2 of 2,049 water-holding basins are empty under the
// lattice rule (both true seed-cell strandings, since repaired by
// lakes.h:lakeExtentFillRescued), the lattice and spline extents agree to
// 0.1% of total area, and a PURE spline rule would lose 4 basins to spline-dry
// seeds -- strictly worse than the lattice it would replace. The live game's
// "207 basins drawn" against "2,052 registered" is scan geometry, not masks:
// four of the six wet tiles lie wholly outside the sheet actor's 10 km scan
// radius (1,627 rows), 218 more rows fail its per-basin bbox-vs-radius cull,
// and 1,627 + 218 + 207 = 2,052 exactly.
//
// WHAT THIS TOOL REPLAYS, per water-holding basin, per tile:
//
//   OLD     lakeExtentFill over the lattice (the primary shipped rule)
//   SPLINE  the same fill over reconstructedGroundMm, with the v2 floor cell
//           as a fallback seed when the wire seed itself reads dry
//   HYBRID  lattice cheap-accept, spline only on the rejects (the union rule),
//           same fallback seed -- what the shipped SEED RESCUE refills with,
//           here forced on every basin so its cost and area delta are visible
//   PROD    LakeSampler::extentMaskFor, i.e. what lakes.h actually ships in
//           this build (lattice rule + rescue on empties), verified against
//           its own replay
//
// and reports the empty-extent census, total lake area, the seed diagnosis
// (does the wire seed itself fail the lattice/spline test), and the cost of
// each rule in spline evaluations and wall time -- including the worst bboxes,
// because "4x4 gather per cell" is only a problem if the big basins make it
// one.
//
// Usage:
//   vxc_lakeextentprobe <tiledir> [--zstd PATH] [--max N] [--verbose]
//
//   --max N     replay only the first N water-holding basins per tile
//   --verbose   per-basin lines for every basin whose OLD extent is empty

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "voxelcore/lakes.h"
#include "voxelcore/tilestore.h"

using namespace vxc;

namespace {

// --- runtime zstd, bound the way the game binds it (see farwaterprobe) ------
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

std::optional<std::vector<uint8_t>> readBytes(const std::filesystem::path& p) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(p, ec);
    if (ec) return std::nullopt;
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f) return std::nullopt;
    const size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) return std::nullopt;
    return buf;
}

double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// The fill with an explicit seed, so the floor-cell fallback can be replayed
// here even against a lakes.h that does not have it yet. Same rule as
// lakeExtentFill: 8-connected component of {elev <= surfaceMm}, bbox-clipped.
template <class ElevFn>
size_t fillFrom(const BasinEntry& b, int32_t seedX, int32_t seedY, ElevFn&& elev,
                std::vector<uint8_t>& out) {
    BasinEntry s = b;
    s.seedX = uint16_t(seedX);
    s.seedY = uint16_t(seedY);
    if (seedX < b.bboxX0 || seedX > b.bboxX1 || seedY < b.bboxY0 || seedY > b.bboxY1) {
        out.assign(size_t(b.bboxX1 - b.bboxX0 + 1) * size_t(b.bboxY1 - b.bboxY0 + 1), 0);
        return 0;
    }
    return lakeExtentFill(s, elev, out);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: vxc_lakeextentprobe <tiledir> [--zstd PATH] [--max N] [--verbose]\n");
        return 2;
    }
    std::string fineDir = argv[1], zstdPath;
    size_t maxPerTile = SIZE_MAX;
    bool verbose = false;
    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--zstd") && i + 1 < argc) {
            zstdPath = argv[++i];
        } else if (!std::strcmp(a, "--max") && i + 1 < argc) {
            maxPerTile = size_t(std::strtoull(argv[++i], nullptr, 10));
        } else if (!std::strcmp(a, "--verbose")) {
            verbose = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a);
            return 2;
        }
    }

    bindZstd(zstdPath);

    std::vector<std::filesystem::path> files;
    for (auto& e : std::filesystem::directory_iterator(fineDir))
        if (e.path().extension() == ".vxtl") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::fprintf(stderr, "no .vxtl files in %s\n", fineDir.c_str());
        return 1;
    }

    FineDecompressor dec;
    dec.fn = &zstdInflate;
    dec.user = nullptr;

    uint64_t seed = 0;
    {
        FineError err = FineError::kNone;
        auto bytes = readBytes(files.front());
        if (!bytes) return 1;
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
    std::vector<std::pair<int32_t, int32_t>> loaded;
    for (const auto& f : files) {
        FineError err = FineError::kNone;
        auto bytes = readBytes(f);
        if (!bytes) continue;
        auto t = FineTile::parse(std::move(*bytes), dec, &err);
        if (!t) {
            std::printf("REFUSED %s (%s)\n", f.filename().string().c_str(), fineErrorName(err));
            continue;
        }
        std::printf("tile (%d,%d) bakeVer=%u basins=%zu\n", t->tileX(), t->tileY(),
                    unsigned(t->header().bakeVer), t->basins().size());
        loaded.push_back({t->tileX(), t->tileY()});
        fine.loadTile(std::move(*t));
    }
    const int64_t tileSize = int64_t(fine.tileSize());
    if (tileSize <= 0) {
        std::fprintf(stderr, "no tile loaded\n");
        return 1;
    }

    // The production sampler, so PROD rows report what lakes.h ships TODAY.
    LakeSampler prod(fine);

    struct Worst {
        double sec = 0;
        int32_t tx = 0, ty = 0;
        uint16_t id = 0;
        int64_t cells = 0;
    };

    struct Tally {
        size_t basins = 0;
        size_t emptyOld = 0, emptySpline = 0, emptyHybrid = 0, emptyProd = 0;
        uint64_t areaOld = 0, areaSpline = 0, areaHybrid = 0, areaProd = 0;
        // Seed diagnosis, over basins whose OLD extent came out empty.
        size_t seedFailsLattice = 0;      // the wire seed's own cp reads dry
        size_t bboxHadLatticeWet = 0;     // ...while other bbox cells read wet
        size_t seedFailsSpline = 0;       // the wire seed's SPLINE reads dry too
        size_t floorRescued = 0;          // v2 floor fallback found the pond
        // Cost.
        uint64_t bboxCells = 0;
        uint64_t splineEvalsPure = 0, splineEvalsHybrid = 0;
        double secOld = 0, secSpline = 0, secHybrid = 0;
        Worst worstSpline, worstHybrid;
    };
    Tally all;

    std::printf("\n%-9s %6s | %14s | %14s | %14s | %14s\n", "tile", "basins", "OLD empty/area",
                "SPLINE e/area", "HYBRID e/area", "PROD e/area");

    for (const auto& tc : loaded) {
        const int32_t tx = tc.first, ty = tc.second;
        const FineTile* t = fine.findTile(tx, ty);
        if (t == nullptr || !t->hasBasins()) continue;
        const int64_t ox = int64_t(tx) * tileSize, oy = int64_t(ty) * tileSize;
        prod.prewarmTile(tx, ty);

        // Which of the 8 neighbours are loaded, for the spline collar test.
        bool nb[3][3];
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                nb[dy + 1][dx + 1] = fine.findTile(tx + dx, ty + dy) != nullptr;

        Tally tt;
        size_t seen = 0;
        const std::vector<BasinEntry>& rows = t->basins();
        for (size_t bi = 0; bi < rows.size(); ++bi) {
            const BasinEntry& b = rows[bi];
            if (!b.holdsWater()) continue;
            if (seen >= maxPerTile) break;
            ++seen;
            ++tt.basins;

            const int64_t w = int64_t(b.bboxX1) - b.bboxX0 + 1;
            const int64_t h = int64_t(b.bboxY1) - b.bboxY0 + 1;
            tt.bboxCells += uint64_t(w) * uint64_t(h);

            // Decode the bbox plus the spline stencil halo (clamped to this
            // tile) once, so the timings below are fill cost, not decode cost.
            const int64_t hx0 = std::max<int64_t>(0, int64_t(b.bboxX0) - 1);
            const int64_t hy0 = std::max<int64_t>(0, int64_t(b.bboxY0) - 1);
            const int64_t hx1 = std::min<int64_t>(tileSize - 1, int64_t(b.bboxX1) + 2);
            const int64_t hy1 = std::min<int64_t>(tileSize - 1, int64_t(b.bboxY1) + 2);
            if (!fine.prewarm(ox + hx0, oy + hy0, ox + hx1, oy + hy1)) {
                std::printf("  UNRESOLVED basin id=%u (%d,%d)\n", unsigned(b.basinId), tx, ty);
                continue;
            }

            auto lattice = [&](int32_t lx, int32_t ly) {
                return fine.elevationMm(ox + lx, oy + ly);
            };
            // Spline ground, honest at the tile edge: where the 4x4 stencil
            // would read a tile that is not loaded, answer "very high" (never
            // wet) rather than mix missing-tile zeros into the carrier.
            uint64_t evals = 0;
            auto spline = [&](int32_t lx, int32_t ly) -> int32_t {
                const bool wLo = lx - 1 < 0, wHi = lx + 2 > tileSize - 1;
                const bool hLo = ly - 1 < 0, hHi = ly + 2 > tileSize - 1;
                const int dxs[2] = {wLo ? -1 : 0, wHi ? 1 : 0};
                const int dys[2] = {hLo ? -1 : 0, hHi ? 1 : 0};
                for (int a = 0; a < 2; ++a)
                    for (int c = 0; c < 2; ++c)
                        if (!nb[dys[a] + 1][dxs[c] + 1]) return INT32_MAX;
                ++evals;
                return reconstructedGroundMm(fine, ox + lx, oy + ly);
            };

            const int32_t fLx = b.hasV2() ? int32_t(int64_t(b.globalIdWorldX()) - ox) : -1;
            const int32_t fLy = b.hasV2() ? int32_t(int64_t(b.globalIdWorldY()) - oy) : -1;
            const bool floorInBbox = b.hasV2() && fLx >= b.bboxX0 && fLx <= b.bboxX1 &&
                                     fLy >= b.bboxY0 && fLy <= b.bboxY1;

            // --- OLD: the lattice rule, exactly what maskFor shipped --------
            std::vector<uint8_t> mOld;
            auto t0 = std::chrono::steady_clock::now();
            const size_t nOld = lakeExtentFill(b, lattice, mOld);
            tt.secOld += secondsSince(t0);
            tt.areaOld += nOld;
            if (nOld == 0) ++tt.emptyOld;

            // --- SPLINE: pure reconstructed-surface rule + floor fallback ---
            std::vector<uint8_t> mSpline;
            evals = 0;
            t0 = std::chrono::steady_clock::now();
            size_t nSpline = fillFrom(b, b.seedX, b.seedY, spline, mSpline);
            if (nSpline == 0 && floorInBbox)
                nSpline = fillFrom(b, fLx, fLy, spline, mSpline);
            const double sSpline = secondsSince(t0);
            tt.secSpline += sSpline;
            tt.splineEvalsPure += evals;
            tt.areaSpline += nSpline;
            if (nSpline == 0) ++tt.emptySpline;
            if (sSpline > tt.worstSpline.sec)
                tt.worstSpline = {sSpline, tx, ty, b.basinId, int64_t(w) * h};

            // --- HYBRID: lattice cheap-accept, spline on the rejects --------
            auto hybrid = [&](int32_t lx, int32_t ly) -> int32_t {
                const int32_t cp = fine.elevationMm(ox + lx, oy + ly);
                if (cp <= b.surfaceMm) return cp;
                return spline(lx, ly);  // spline() counts itself via `evals`
            };
            std::vector<uint8_t> mHyb;
            evals = 0;
            t0 = std::chrono::steady_clock::now();
            size_t nHyb = fillFrom(b, b.seedX, b.seedY, hybrid, mHyb);
            if (nHyb == 0 && floorInBbox) nHyb = fillFrom(b, fLx, fLy, hybrid, mHyb);
            const double sHyb = secondsSince(t0);
            tt.secHybrid += sHyb;
            tt.splineEvalsHybrid += evals;
            tt.areaHybrid += nHyb;
            if (nHyb == 0) ++tt.emptyHybrid;
            if (sHyb > tt.worstHybrid.sec)
                tt.worstHybrid = {sHyb, tx, ty, b.basinId, int64_t(w) * h};

            // --- PROD: what lakes.h ships in this build ---------------------
            const std::vector<uint8_t>* mp = prod.extentMaskFor(tx, ty, uint16_t(bi));
            size_t nProd = 0;
            if (mp != nullptr)
                for (uint8_t v : *mp) nProd += (v != 0);
            tt.areaProd += nProd;
            if (nProd == 0) ++tt.emptyProd;

            // --- seed diagnosis for the basins the OLD rule loses -----------
            if (nOld == 0) {
                const bool sFail = lattice(b.seedX, b.seedY) > b.surfaceMm;
                if (sFail) ++tt.seedFailsLattice;
                if (sFail) {
                    // Did the bbox hold ANY lattice-wet cell? If yes, the
                    // component existed and only the seed test lost it.
                    bool any = false;
                    for (int32_t ly = b.bboxY0; ly <= b.bboxY1 && !any; ++ly)
                        for (int32_t lx = b.bboxX0; lx <= b.bboxX1; ++lx)
                            if (lattice(lx, ly) <= b.surfaceMm) {
                                any = true;
                                break;
                            }
                    if (any) ++tt.bboxHadLatticeWet;
                }
                if (spline(b.seedX, b.seedY) > b.surfaceMm) {
                    ++tt.seedFailsSpline;
                    if (nSpline > 0) ++tt.floorRescued;
                }
                if (verbose) {
                    std::printf("  id=%-4u bbox %lldx%-6lld seed cp-surf=%+7lld mm "
                                "spline-surf=%+7lld mm  old=0 spline=%zu hybrid=%zu\n",
                                unsigned(b.basinId), (long long)w, (long long)h,
                                (long long)(lattice(b.seedX, b.seedY) - b.surfaceMm),
                                spline(b.seedX, b.seedY) == INT32_MAX
                                    ? (long long)999999
                                    : (long long)(spline(b.seedX, b.seedY) - b.surfaceMm),
                                nSpline, nHyb);
                }
            }
        }

        std::printf("(%3d,%3d) %6zu | %5zu %8.1fha | %5zu %8.1fha | %5zu %8.1fha | %5zu %8.1fha\n",
                    tx, ty, tt.basins, tt.emptyOld, double(tt.areaOld) * 1.875 * 1.875 / 10000.0,
                    tt.emptySpline, double(tt.areaSpline) * 1.875 * 1.875 / 10000.0, tt.emptyHybrid,
                    double(tt.areaHybrid) * 1.875 * 1.875 / 10000.0, tt.emptyProd,
                    double(tt.areaProd) * 1.875 * 1.875 / 10000.0);
        std::printf("          seed-diag: %zu/%zu old-empty seeds fail lattice "
                    "(%zu with wet cells elsewhere in bbox), %zu fail spline too, "
                    "%zu rescued by v2 floor\n",
                    tt.seedFailsLattice, tt.emptyOld, tt.bboxHadLatticeWet, tt.seedFailsSpline,
                    tt.floorRescued);
        std::printf("          cost: bboxCells=%" PRIu64 "  splineEvals pure=%" PRIu64
                    " hybrid=%" PRIu64 "  time old=%.3fs spline=%.3fs hybrid=%.3fs\n",
                    tt.bboxCells, tt.splineEvalsPure, tt.splineEvalsHybrid, tt.secOld, tt.secSpline,
                    tt.secHybrid);
        std::printf("          worst basin: spline id=%u %.3fs (%lld cells)   hybrid id=%u %.3fs "
                    "(%lld cells)\n",
                    unsigned(tt.worstSpline.id), tt.worstSpline.sec,
                    (long long)tt.worstSpline.cells, unsigned(tt.worstHybrid.id),
                    tt.worstHybrid.sec, (long long)tt.worstHybrid.cells);

        all.basins += tt.basins;
        all.emptyOld += tt.emptyOld;
        all.emptySpline += tt.emptySpline;
        all.emptyHybrid += tt.emptyHybrid;
        all.emptyProd += tt.emptyProd;
        all.areaOld += tt.areaOld;
        all.areaSpline += tt.areaSpline;
        all.areaHybrid += tt.areaHybrid;
        all.areaProd += tt.areaProd;
        all.seedFailsLattice += tt.seedFailsLattice;
        all.bboxHadLatticeWet += tt.bboxHadLatticeWet;
        all.seedFailsSpline += tt.seedFailsSpline;
        all.floorRescued += tt.floorRescued;
        all.bboxCells += tt.bboxCells;
        all.splineEvalsPure += tt.splineEvalsPure;
        all.splineEvalsHybrid += tt.splineEvalsHybrid;
        all.secOld += tt.secOld;
        all.secSpline += tt.secSpline;
        all.secHybrid += tt.secHybrid;
    }

    std::printf("\nTOTAL     %6zu | %5zu %8.1fha | %5zu %8.1fha | %5zu %8.1fha | %5zu %8.1fha\n",
                all.basins, all.emptyOld, double(all.areaOld) * 1.875 * 1.875 / 10000.0,
                all.emptySpline, double(all.areaSpline) * 1.875 * 1.875 / 10000.0, all.emptyHybrid,
                double(all.areaHybrid) * 1.875 * 1.875 / 10000.0, all.emptyProd,
                double(all.areaProd) * 1.875 * 1.875 / 10000.0);
    std::printf("TOTAL     seed-diag: %zu old-empty seeds fail lattice (%zu with wet cells "
                "elsewhere), %zu fail spline, %zu floor-rescued\n",
                all.seedFailsLattice, all.bboxHadLatticeWet, all.seedFailsSpline, all.floorRescued);
    std::printf("TOTAL     cost: bboxCells=%" PRIu64 "  splineEvals pure=%" PRIu64 " hybrid=%" PRIu64
                "  time old=%.3fs spline=%.3fs hybrid=%.3fs\n",
                all.bboxCells, all.splineEvalsPure, all.splineEvalsHybrid, all.secOld,
                all.secSpline, all.secHybrid);
    return 0;
}
