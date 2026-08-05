// vxc_waterdatumprobe -- "which producer claims water at THIS world position,
// and at what height", asked of every producer separately, over real tiles.
//
// WHY THIS EXISTS. The owner reported a large disc of water floating ~527 m
// above the ground at world (-160398.2, -85408.4). Four things can draw water
// in the client -- the ocean plane, the lake SHEET, the river RIBBON and the
// near-field implicit brick sweep -- and three of them take their height from
// the baked tiles. "Is the wrong height in the DATA, or manufactured by the
// client?" is the first fork of any such diagnosis, and answering it by reading
// a basin table by hand is exactly the kind of hand-derived number that cannot
// be re-run after a constant changes.
//
// So this asks each producer, at one world position and over a square around
// it, in the producer's own words:
//
//   LakeSampler::surfaceAtPixel     the basin registry -- what a SHEET draws
//   RiverSampler::surfaceAtPixel    the water plane   -- what a RIBBON draws
//   CompositeWaterSampler           the near-field datum the ImplicitFn takes
//   oceanSurfaceMmAt                the sea term
//   reconstructedGroundMm           ground #2, the datum water is measured from
//   Amplifier::columnCached          ground #3, what the renderer actually draws
//
// AND IT REPRODUCES THE SHEET GATHER, because a sheet's height does not come
// from the column under the camera -- it comes from a basin ROW up to
// `--radius` away. AVoxelWaterSheetActor::Tick walks the fine tiles in a square
// around the camera and makes one sheet per `holdsWater()` basin whose world
// bbox meets it; the same walk is done here so "no basin anywhere in range sits
// at that height" is a measurement rather than an assumption.
//
// The three grounds are named separately on purpose -- see tilestore.h's
// `reconstructedGroundMm`, which exists because they have been conflated three
// times across two languages.
//
// Compare vxc_riverribbonprobe (what the ribbon producer emits, and whether it
// is buried) and vxc_burialprobe (does the near field offer water to the
// mesher?). Same shape of tool, same reason.
//
// Usage:
//   vxc_waterdatumprobe <tiledir> [--at Xm Ym] [--radius M] [--span M]
//                       [--band LOm HIm] [--zstd PATH]
//
//   --at       world position in METRES (default -160398.2 -85408.4)
//   --radius   sheet-gather radius in metres (default 10000, the actor's own
//              ScanRadiusUU)
//   --span     edge of the per-column sweep around --at, in metres (default
//              2000)
//   --band     report every resident basin whose surface falls in [LO, HI] m
//              (default 500 700)

#include <algorithm>
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
// See bankprobe.cpp: without NOMINMAX, windows.h's min/max macros break every
// std::max in this file under MSVC. clang/ninja does not catch it.
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "voxelcore/amplifier.h"
#include "voxelcore/lakes.h"
#include "voxelcore/tilestore.h"

using namespace vxc;

namespace {

// --- runtime zstd, bound the way the game binds it --------------------------
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

std::optional<std::vector<uint8_t>> probeReadBytes(const std::filesystem::path& p) {
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

const char* basinKindName(uint8_t k) {
    switch (k) {
    case kBasinDryPlaya: return "dry-playa";
    case kBasinSaltFlat: return "salt-flat";
    case kBasinSeasonal: return "seasonal";
    case kBasinLakeTerminal: return "lake-terminal";
    case kBasinLakeOverflowing: return "lake-overflowing";
    default: return "UNKNOWN";
    }
}

// kNoWaterMm prints as a WORD, never as a number: INT32_MIN formatted as metres
// is -2147483.648, which reads as a plausible (very deep) elevation and is
// exactly the confusion this whole probe exists to prevent.
void printMm(const char* label, int32_t mm) {
    if (mm == kNoWaterMm) {
        std::printf("  %-32s DRY\n", label);
    } else {
        std::printf("  %-32s %.3f m\n", label, double(mm) / 1000.0);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: vxc_waterdatumprobe <tiledir> [--at Xm Ym] [--radius M] "
                             "[--span M] [--band LOm HIm] [--zstd PATH]\n");
        return 2;
    }
    std::string fineDir = argv[1], zstdPath;
    double atXm = -160398.2, atYm = -85408.4;
    double radiusM = 10000.0, spanM = 2000.0;
    double bandLoM = 500.0, bandHiM = 700.0;
    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--at") && i + 2 < argc) {
            atXm = std::strtod(argv[i + 1], nullptr);
            atYm = std::strtod(argv[i + 2], nullptr);
            i += 2;
        } else if (!std::strcmp(a, "--radius") && i + 1 < argc) {
            radiusM = std::strtod(argv[++i], nullptr);
        } else if (!std::strcmp(a, "--span") && i + 1 < argc) {
            spanM = std::strtod(argv[++i], nullptr);
        } else if (!std::strcmp(a, "--band") && i + 2 < argc) {
            bandLoM = std::strtod(argv[i + 1], nullptr);
            bandHiM = std::strtod(argv[i + 2], nullptr);
            i += 2;
        } else if (!std::strcmp(a, "--zstd") && i + 1 < argc) {
            zstdPath = argv[++i];
        }
    }

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
        if (bindZstd(cands))
            std::printf("zstd: bound from '%s'\n", gZstdPath.c_str());
        else
            std::printf("zstd: NOT BOUND -- every CODEC_ZSTD tile will be refused\n");
    }

    if (!std::filesystem::exists(fineDir)) {
        std::fprintf(stderr, "no such directory: %s\n", fineDir.c_str());
        return 1;
    }
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

    // The seed comes off the tiles, never a flag: the detail octaves are
    // seeded, so a wrong seed produces plausible detail over the right terrain.
    uint64_t seed = 0;
    {
        FineError err = FineError::kNone;
        auto bytes = probeReadBytes(files.front());
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
    struct LoadedTile {
        int32_t tx, ty;
    };
    std::vector<LoadedTile> loaded;
    int refused = 0;
    for (const auto& f : files) {
        FineError err = FineError::kNone;
        auto bytes = probeReadBytes(f);
        if (!bytes) {
            ++refused;
            continue;
        }
        auto t = FineTile::parse(std::move(*bytes), dec, &err);
        if (!t) {
            std::printf("REFUSED %s (%s)\n", f.filename().string().c_str(), fineErrorName(err));
            ++refused;
            continue;
        }
        // The header fields go in the banner because a units bug is read from
        // them: `quant` is a CODE, not millimetres, and `baseOffsetMm` is the
        // datum a control point is relative to. Printing the DECODED quantMm()
        // is the point -- the raw code has already cost this project a 100x.
        std::printf("  tile (%d,%d) baseOffsetMm=%d quantMm=%d basins=%s(%zu) waterPlane=%s\n",
                    t->tileX(), t->tileY(), t->baseOffsetMm(), t->quantMm(),
                    t->hasBasins() ? "yes" : "NO", t->basins().size(),
                    t->hasWater() ? "yes" : "NO");
        loaded.push_back({t->tileX(), t->tileY()});
        fine.loadTile(std::move(*t));
    }

    const int64_t pxMm = fine.pixelSizeMm();
    const int64_t tileSize = int64_t(fine.tileSize());
    if (pxMm <= 0 || tileSize <= 0) {
        std::fprintf(stderr, "no tile loaded (%d refused)\n", refused);
        return 1;
    }
    std::printf("seed %llu  tiles %zu (refused %d)  tileSize %lld px  pixel %lld mm  tile span %.3f m\n",
                (unsigned long long)seed, fine.tileCount(), refused, (long long)tileSize,
                (long long)pxMm, double(tileSize * pxMm) / 1000.0);

    LakeSampler lakes(fine);
    RiverSampler rivers(fine);
    CompositeWaterSampler both(lakes, rivers);
    Amplifier amp(seed, fine);

    const int64_t atXmm = int64_t(atXm * 1000.0), atYmm = int64_t(atYm * 1000.0);
    const int64_t px = floorDiv(atXmm, pxMm), py = floorDiv(atYmm, pxMm);
    const int64_t vx = floorDiv(atXmm, int64_t(kVoxelSizeMm));
    const int64_t vy = floorDiv(atYmm, int64_t(kVoxelSizeMm));
    std::printf("\nat world (%.1f, %.1f) m -> fine px (%lld, %lld) -> tile (%lld, %lld)\n", atXm, atYm,
                (long long)px, (long long)py, (long long)floorDiv(px, tileSize),
                (long long)floorDiv(py, tileSize));

    std::printf("\n=== EVERY PRODUCER AT THIS COLUMN ===\n");
    const int32_t groundMm = amp.columnCached(vx, vy).surfaceMm;
    printMm("LakeSampler (sheet datum)", lakes.surfaceAtPixel(px, py));
    printMm("RiverSampler (ribbon datum)", rivers.surfaceAtPixel(px, py));
    printMm("Composite (near-field datum)", both.waterSurfaceMmAtVoxel(vx, vy));
    printMm("oceanSurfaceMmAt (the sea)", oceanSurfaceMmAt(groundMm));
    std::printf("  %-32s %.3f m   (ground 2: what water is measured FROM)\n",
                "reconstructedGroundMm", double(reconstructedGroundMm(fine, px, py)) / 1000.0);
    std::printf("  %-32s %.3f m   (NOT a surface -- the control lattice)\n", "elevationMm (ground 1)",
                double(fine.elevationMm(px, py)) / 1000.0);
    std::printf("  %-32s %.3f m   (ground 3: what the renderer DRAWS)\n", "Amplifier surfaceMm",
                double(groundMm) / 1000.0);

    // ---- The per-column sweep ----------------------------------------------
    //
    // The column above answers "is the camera standing in water". It does NOT
    // answer "is there water NEAR here at a wrong height", which is the actual
    // question when something is floating in the middle distance.
    std::printf("\n=== HIGHEST WATER SURFACE WITHIN %.0f m (per-column sweep) ===\n", spanM * 0.5);
    const int64_t halfPx = int64_t(spanM * 1000.0 / double(pxMm)) / 2;
    const int64_t stride = 4; // 7.5 m at the fine tier: finer than any sheet step
    int32_t bestLake = kNoWaterMm, bestRiver = kNoWaterMm;
    int64_t bestLakeX = 0, bestLakeY = 0, bestRiverX = 0, bestRiverY = 0;
    int64_t wetLake = 0, wetRiver = 0, columns = 0;
    for (int64_t dy = -halfPx; dy <= halfPx; dy += stride) {
        for (int64_t dx = -halfPx; dx <= halfPx; dx += stride) {
            ++columns;
            const int32_t l = lakes.surfaceAtPixel(px + dx, py + dy);
            const int32_t r = rivers.surfaceAtPixel(px + dx, py + dy);
            if (l != kNoWaterMm) {
                ++wetLake;
                if (bestLake == kNoWaterMm || l > bestLake) {
                    bestLake = l;
                    bestLakeX = px + dx;
                    bestLakeY = py + dy;
                }
            }
            if (r != kNoWaterMm) {
                ++wetRiver;
                if (bestRiver == kNoWaterMm || r > bestRiver) {
                    bestRiver = r;
                    bestRiverX = px + dx;
                    bestRiverY = py + dy;
                }
            }
        }
    }
    std::printf("  %lld columns sampled at %lld px stride (%.1f m)\n", (long long)columns,
                (long long)stride, double(stride * pxMm) / 1000.0);
    if (bestLake == kNoWaterMm) {
        std::printf("  lake  : %lld wet, NO LAKE WATER AT ALL in this square\n", (long long)wetLake);
    } else {
        std::printf("  lake  : %lld wet, highest %.3f m at world (%.0f, %.0f) m\n", (long long)wetLake,
                    double(bestLake) / 1000.0, double(bestLakeX * pxMm) / 1000.0,
                    double(bestLakeY * pxMm) / 1000.0);
    }
    if (bestRiver == kNoWaterMm) {
        std::printf("  river : %lld wet, NO RIVER WATER AT ALL in this square\n", (long long)wetRiver);
    } else {
        std::printf("  river : %lld wet, highest %.3f m at world (%.0f, %.0f) m\n",
                    (long long)wetRiver, double(bestRiver) / 1000.0,
                    double(bestRiverX * pxMm) / 1000.0, double(bestRiverY * pxMm) / 1000.0);
    }

    // ---- The sheet gather, exactly as AVoxelWaterSheetActor::Tick does it ---
    std::printf("\n=== LAKE SHEET GATHER (%.0f m square, as the actor walks it) ===\n", radiusM);
    const int64_t tx0 = floorDiv(floorDiv(int64_t((atXm - radiusM) * 1000.0), pxMm), tileSize);
    const int64_t tx1 = floorDiv(floorDiv(int64_t((atXm + radiusM) * 1000.0), pxMm), tileSize);
    const int64_t ty0 = floorDiv(floorDiv(int64_t((atYm - radiusM) * 1000.0), pxMm), tileSize);
    const int64_t ty1 = floorDiv(floorDiv(int64_t((atYm + radiusM) * 1000.0), pxMm), tileSize);
    int sheets = 0, missing = 0;
    int32_t highestSheetMm = kNoWaterMm;
    double highestSheetX = 0.0, highestSheetY = 0.0;
    // The widest sheet in range, because "a 2 km disc" is a claim about EXTENT
    // and the tallest basin is usually a pond.
    double widestSpan = 0.0;
    std::string widestWhat;
    for (int64_t ty = ty0; ty <= ty1; ++ty) {
        for (int64_t tx = tx0; tx <= tx1; ++tx) {
            const std::vector<BasinEntry>* bs = lakes.basinsForTile(int32_t(tx), int32_t(ty));
            if (bs == nullptr) {
                // NOT "no lakes here": a tile that is absent draws no sheet, and
                // that looks exactly like a dry basin. Counted, per house rule.
                ++missing;
                continue;
            }
            const int64_t ox = tx * tileSize, oy = ty * tileSize;
            int inRange = 0;
            for (size_t i = 0; i < bs->size(); ++i) {
                const BasinEntry& b = (*bs)[i];
                if (!b.holdsWater()) continue; // a dry playa has no sheet
                const double minX = double((ox + b.bboxX0) * pxMm) / 1000.0;
                const double minY = double((oy + b.bboxY0) * pxMm) / 1000.0;
                const double maxX = double((ox + b.bboxX1 + 1) * pxMm) / 1000.0;
                const double maxY = double((oy + b.bboxY1 + 1) * pxMm) / 1000.0;
                if (maxX < atXm - radiusM || minX > atXm + radiusM || maxY < atYm - radiusM ||
                    minY > atYm + radiusM)
                    continue;
                ++inRange;
                ++sheets;
                if (highestSheetMm == kNoWaterMm || b.surfaceMm > highestSheetMm) {
                    highestSheetMm = b.surfaceMm;
                    highestSheetX = 0.5 * (minX + maxX);
                    highestSheetY = 0.5 * (minY + maxY);
                }
                const double span = std::max(maxX - minX, maxY - minY);
                if (span > widestSpan) {
                    widestSpan = span;
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                                  "tile(%lld,%lld) basin %zu %s, surface %.1f m, %.0f x %.0f m, "
                                  "centre (%.0f, %.0f) m",
                                  (long long)tx, (long long)ty, i, basinKindName(b.kind),
                                  double(b.surfaceMm) / 1000.0, maxX - minX, maxY - minY,
                                  0.5 * (minX + maxX), 0.5 * (minY + maxY));
                    widestWhat = buf;
                }
            }
            std::printf("  tile (%lld,%lld): %d water-holding basin(s) in range, of %zu row(s)\n",
                        (long long)tx, (long long)ty, inRange, bs->size());
        }
    }
    std::printf("  %d sheet(s) would be created; %d tile(s) in the square are NOT RESIDENT "
                "(their water is ABSENT, which looks exactly like dry ground)\n",
                sheets, missing);
    if (highestSheetMm != kNoWaterMm) {
        std::printf("  HIGHEST sheet datum in range: %.1f m, centred (%.0f, %.0f) m\n",
                    double(highestSheetMm) / 1000.0, highestSheetX, highestSheetY);
    }
    if (!widestWhat.empty()) {
        std::printf("  WIDEST sheet in range: %s\n", widestWhat.c_str());
    }

    // ---- Every resident basin inside the reported height band ---------------
    std::printf("\n=== EVERY RESIDENT BASIN WITH SURFACE IN %.0f..%.0f m ===\n", bandLoM, bandHiM);
    int found = 0;
    for (const LoadedTile& t : loaded) {
        const std::vector<BasinEntry>* bs = lakes.basinsForTile(t.tx, t.ty);
        if (bs == nullptr) continue;
        const int64_t ox = int64_t(t.tx) * tileSize, oy = int64_t(t.ty) * tileSize;
        for (size_t i = 0; i < bs->size(); ++i) {
            const BasinEntry& b = (*bs)[i];
            const double s = double(b.surfaceMm) / 1000.0;
            if (s < bandLoM || s > bandHiM) continue;
            ++found;
            const double minX = double((ox + b.bboxX0) * pxMm) / 1000.0;
            const double minY = double((oy + b.bboxY0) * pxMm) / 1000.0;
            const double maxX = double((ox + b.bboxX1 + 1) * pxMm) / 1000.0;
            const double maxY = double((oy + b.bboxY1 + 1) * pxMm) / 1000.0;
            const double dx = 0.5 * (minX + maxX) - atXm, dy = 0.5 * (minY + maxY) - atYm;
            std::printf("  tile(%d,%d) basin %zu %s surface %.1f m  %.0f x %.0f m  %.2f km away\n",
                        t.tx, t.ty, i, basinKindName(b.kind), s, maxX - minX, maxY - minY,
                        std::sqrt(dx * dx + dy * dy) / 1000.0);
        }
    }
    if (found == 0) {
        std::printf("  NONE. A sheet at that height cannot be coming from this tile set.\n");
    }
    return 0;
}
