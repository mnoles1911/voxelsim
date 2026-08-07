// vxc_riverribbonprobe -- what the far-field river producer actually emits over
// real baked tiles, and what it will be worth on screen.
//
// WHY THIS EXISTS. Task #57's defect is that a river is invisible from a vista,
// and the two ways to be wrong about a fix for it are both invisible in a
// screenshot:
//
//   1. THE GEOMETRY IS NOT THERE. A ribbon that emitted nothing, or emitted
//      four-metre scraps, looks exactly like a ribbon that is there and
//      occluded, and exactly like one that is there and sub-pixel. So the
//      counts come first: wet pixels, centreline pixels, paths, kilometres of
//      channel, and the width distribution the bake actually drew.
//
//   2. IT IS THERE AND TOO NARROW TO SEE. The measured reach width on these
//      tiles is 1-3 fine pixels. At 1440p a 2 m ribbon is sub-pixel within a
//      couple of kilometres, and "sub-pixel" is not a look, it is a shimmer. So
//      the screen-width table below is printed at the three ranges the brief
//      asks about, with the widening policy applied, so the failure mode at
//      each range is a number rather than an opinion.
//
// AND THE ONE THING THAT WOULD SILENTLY DEFEAT ALL OF IT. The ribbon is drawn
// at the water datum -- reconstructed ground plus baked depth -- and the
// terrain around it is drawn at the AMPLIFIED surface. If the amplified bank is
// higher than the datum, the depth test buries the ribbon and none of the
// counts matter. That is measured here at the centreline AND at the widened
// edge, because widening pushes the ribbon out over the bank and the edge is
// where it gets buried first.
//
// Compare vxc_riverprobe (is there a bed?) and vxc_burialprobe (does the near
// field offer water to the mesher?). Same shape of tool, same reason: a
// hand-derived number cannot be re-run after a constant changes.
//
// Usage:
//   vxc_riverribbonprobe <tiledir> [--origin PX PY] [--region PX]
//                        [--min-px F] [--cap N] [--zstd PATH]
//
//   --origin   region low corner in ABSOLUTE fine pixels. Defaults to the
//              centre of the loaded tile set.
//   --region   region edge in fine pixels (default 2048 = 3.84 km).
//   --min-px   minimum screen width in pixels (default 2.0; see the policy).
//   --cap      maximum widening factor (default 8).

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
#include "voxelcore/riverribbon.h"
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

int64_t pct(std::vector<int64_t>& v, int p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = size_t((v.size() - 1) * size_t(p)) / 100;
    return v[i];
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: vxc_riverribbonprobe <tiledir> [--origin PX PY] "
                             "[--region PX] [--min-px F] [--cap N] [--zstd PATH]\n");
        return 2;
    }
    std::string fineDir = argv[1], zstdPath;
    int64_t regionPx = 2048, originPx = 0, originPy = 0;
    bool haveOrigin = false;
    double minScreenPx = 2.0;
    int32_t widenCap = 8;
    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--origin") && i + 2 < argc) {
            originPx = std::strtoll(argv[i + 1], nullptr, 10);
            originPy = std::strtoll(argv[i + 2], nullptr, 10);
            haveOrigin = true;
            i += 2;
        } else if (!std::strcmp(a, "--region") && i + 1 < argc) {
            regionPx = std::strtoll(argv[++i], nullptr, 10);
        } else if (!std::strcmp(a, "--min-px") && i + 1 < argc) {
            minScreenPx = std::strtod(argv[++i], nullptr);
        } else if (!std::strcmp(a, "--cap") && i + 1 < argc) {
            widenCap = int32_t(std::strtol(argv[++i], nullptr, 10));
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

    std::vector<std::filesystem::path> files;
    if (!std::filesystem::exists(fineDir)) {
        std::fprintf(stderr, "no such directory: %s\n", fineDir.c_str());
        return 1;
    }
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
    int loaded = 0, refused = 0, withWater = 0;
    int64_t minTx = INT64_MAX, maxTx = INT64_MIN, minTy = INT64_MAX, maxTy = INT64_MIN;
    for (const auto& f : files) {
        FineError err = FineError::kNone;
        auto bytes = probeReadBytes(f);
        if (!bytes) {
            ++refused;
            continue;
        }
        auto t = FineTile::parse(std::move(*bytes), dec, &err);
        if (!t) {
            std::fprintf(stderr, "REFUSED %s (%s)\n", f.string().c_str(), fineErrorName(err));
            ++refused;
            continue;
        }
        const int32_t tx = t->tileX(), ty = t->tileY();
        const bool water = t->hasWater();
        if (!fine.loadTile(std::move(*t))) {
            ++refused;
            continue;
        }
        if (water) ++withWater;
        minTx = std::min<int64_t>(minTx, tx);
        maxTx = std::max<int64_t>(maxTx, tx);
        minTy = std::min<int64_t>(minTy, ty);
        maxTy = std::max<int64_t>(maxTy, ty);
        ++loaded;
    }
    const int32_t pixelMm = fine.pixelSizeMm();
    const uint32_t tileSize = fine.tileSize();
    std::printf("seed: %" PRIu64 " (read from the tiles)\n", seed);
    std::printf("tiles: loaded=%d (with a water plane: %d) refused=%d  tileSize=%u px  pixel=%d mm\n",
                loaded, withWater, refused, tileSize, pixelMm);
    if (loaded == 0 || refused > 0) {
        std::fprintf(stderr, "nothing loaded, or a tile was refused -- refusing to report\n");
        return 1;
    }
    if (withWater == 0) {
        std::fprintf(stderr, "NO TILE HAS A WATER PLANE -- there is nothing for this producer to "
                             "draw, and that is a BAKE problem, not a renderer one\n");
        return 1;
    }

    if (!haveOrigin) {
        const int64_t cx = (minTx + maxTx + 1) * int64_t(tileSize) / 2;
        const int64_t cy = (minTy + maxTy + 1) * int64_t(tileSize) / 2;
        originPx = cx - regionPx / 2;
        originPy = cy - regionPx / 2;
    }
    std::printf("region: origin (%" PRId64 ", %" PRId64 ") px, %" PRId64 " px square = %.2f km\n",
                originPx, originPy, regionPx, double(regionPx * pixelMm) / 1e6);

    // --- the producer, end to end -------------------------------------------
    RiverSampler rivers(fine);
    RiverWetWindow win;
    win.resize(originPx, originPy, int32_t(regionPx), int32_t(regionPx));
    const uint64_t wet = riverRibbonFillWet(fine, win, 0, 0, win.w, win.h);
    const double wetFrac = 100.0 * double(wet) / double(int64_t(regionPx) * regionPx);
    std::printf("\n=== STAGE 1: the baked water plane ===\n");
    std::printf("  wet pixels        : %" PRIu64 " of %" PRId64 "  (%.3f%%)\n", wet,
                int64_t(regionPx) * regionPx, wetFrac);
    std::printf("  unresolved blocks : %" PRIu64 "  (non-zero means water is MISSING and looks "
                "exactly like no river)\n",
                rivers.unresolvedBlocks());
    if (wet == 0) {
        std::fprintf(stderr, "no wet pixels in this region -- point --origin at a river\n");
        return 1;
    }

    // --- IS THE BAKED NETWORK EVEN CONNECTED? -------------------------------
    //
    // The owner's goal is to watch one river run from a mountain valley to the
    // sea. If the water plane itself is a scatter of disconnected puddles then
    // NO renderer can draw that, and the defect is in the bake rather than in
    // the far-field path. This has to be attributed before any conclusion is
    // drawn from a reach count: 8-connected components of the WET MASK, before
    // thinning, before tracing, before the minimum-length drop.
    {
        std::vector<int32_t> comp(win.wet.size(), -1);
        std::vector<int32_t> stack;
        int32_t nComp = 0;
        std::vector<int64_t> compCells, compSpanMm;
        for (int32_t sy = 0; sy < win.h; ++sy) {
            for (int32_t sx = 0; sx < win.w; ++sx) {
                const size_t si = win.at(sx, sy);
                if (!win.wet[si] || comp[si] >= 0) continue;
                const int32_t id = nComp++;
                int64_t cells = 0;
                int32_t bx0 = sx, bx1 = sx, by0 = sy, by1 = sy;
                comp[si] = id;
                stack.push_back(int32_t(si));
                while (!stack.empty()) {
                    const int32_t ci = stack.back();
                    stack.pop_back();
                    ++cells;
                    const int32_t cx = ci % win.w, cy = ci / win.w;
                    bx0 = std::min(bx0, cx);
                    bx1 = std::max(bx1, cx);
                    by0 = std::min(by0, cy);
                    by1 = std::max(by1, cy);
                    for (int32_t k = 0; k < 8; ++k) {
                        const int32_t nx = cx + kRiverNeighDx[k], ny = cy + kRiverNeighDy[k];
                        if (!win.inBounds(nx, ny)) continue;
                        const size_t ni = win.at(nx, ny);
                        if (!win.wet[ni] || comp[ni] >= 0) continue;
                        comp[ni] = id;
                        stack.push_back(int32_t(ni));
                    }
                }
                compCells.push_back(cells);
                // Bounding-box diagonal: a lower bound on how far this blob
                // reaches, which is what "does it go anywhere" means.
                const int64_t dx = int64_t(bx1 - bx0) * pixelMm, dy = int64_t(by1 - by0) * pixelMm;
                int64_t d2 = dx * dx + dy * dy, d = 0;
                while ((d + 1) * (d + 1) <= d2) ++d;
                compSpanMm.push_back(d);
            }
        }
        std::vector<int64_t> spans = compSpanMm;
        std::printf("\n=== CONNECTIVITY OF THE BAKED WET MASK (before any of this producer) ===\n");
        std::printf("  8-connected components: %d\n", nComp);
        if (nComp > 0) {
            std::printf("  component span (bbox diagonal): p50 %.0f m  p90 %.0f m  MAX %.2f km\n",
                        double(pct(spans, 50)) / 1000.0, double(pct(spans, 90)) / 1000.0,
                        double(pct(spans, 100)) / 1e6);
            std::vector<int64_t> cellsv = compCells;
            std::printf("  component size: p50 %.0f px  max %.0f px\n", double(pct(cellsv, 50)),
                        double(pct(cellsv, 100)));
        }
        // How much of the drawn water belongs to something river-SHAPED, as
        // opposed to a puddle. A median component size tells you nothing here
        // because a scatter of 3-pixel specks drags it to 3 whatever the trunk
        // river is doing; what matters is the share of wet pixels living in a
        // blob long enough to read as a river at range.
        const int64_t thresholds[4] = {200000, 500000, 1000000, 2000000};
        std::printf("  components by span:");
        for (int t = 0; t < 4; ++t) {
            int32_t n = 0;
            int64_t cells = 0;
            for (size_t i = 0; i < compSpanMm.size(); ++i)
                if (compSpanMm[i] >= thresholds[t]) {
                    ++n;
                    cells += compCells[i];
                }
            int64_t total = 0;
            for (int64_t c : compCells) total += c;
            std::printf("  >=%.1fkm: %d (%.1f%% of wet px)", double(thresholds[t]) / 1e6, n,
                        total ? 100.0 * double(cells) / double(total) : 0.0);
        }
        std::printf("\n");
        std::printf("  READ THIS FIRST: if the largest component's span is a few hundred metres,\n"
                    "  the BAKE has no continuous river here and no far-field path can invent one.\n");
    }

    // CROSS-CHECK OF THE FAST FILL AGAINST THE REFERENCE PATH. riverRibbonFillWet
    // reads the raw depth block; RiverSampler::surfaceAtPixel is the shipped
    // per-pixel query the near field uses. They must agree cell for cell on
    // WETNESS or the far field is drawing a different river from the near field
    // -- which would look exactly like a correct river in a screenshot.
    {
        int64_t checked = 0, disagree = 0;
        const int32_t step = win.w > 4096 ? 7 : 1; // a coprime stride, so it is not a lattice
        for (int32_t y = 0; y < win.h; y += step) {
            for (int32_t x = 0; x < win.w; x += step) {
                const bool fast = win.wet[win.at(x, y)] != 0;
                const bool ref = rivers.surfaceAtPixel(win.x0 + x, win.y0 + y) != kNoWaterMm;
                ++checked;
                disagree += (fast != ref);
            }
        }
        std::printf("\n=== FAST FILL vs RiverSampler::surfaceAtPixel ===\n");
        std::printf("  checked %" PRId64 " cells (stride %d), disagreements: %" PRId64 "%s\n",
                    checked, step, disagree,
                    disagree == 0 ? "  -- the far field sees the same water as the near field"
                                  : "  *** THE TWO PATHS DISAGREE ***");
    }

    RiverThinField thin;
    riverRibbonThin(win, pixelMm, thin);
    riverRibbonResolveDatum(rivers, win, thin);
    std::printf("\n=== STAGE 2: centreline ===\n");
    std::printf("  centre pixels     : %" PRIu64 "  (%.2f%% of wet -- the thinning ratio is the "
                "mean channel width in pixels)\n",
                thin.centrePixels,
                thin.centrePixels ? 100.0 * double(thin.centrePixels) / double(wet) : 0.0);
    std::printf("  mean width        : %.2f px = %.2f m\n",
                thin.centrePixels ? double(wet) / double(thin.centrePixels) : 0.0,
                thin.centrePixels ? double(wet) / double(thin.centrePixels) * pixelMm / 1000.0 : 0.0);
    std::printf("  runs hitting cap  : %" PRIu64 "  (a reach wider than %d px = %.0f m; the bake "
                "writes basins DRY so these should be rare)\n",
                thin.wideRuns, kRiverRunScanCap, double(kRiverRunScanCap * pixelMm) / 1000.0);

    // =======================================================================
    // THE WATER-SURFACE GRADIENT, and whether it can carry a flow DIRECTION.
    // =======================================================================
    //
    // WHY THIS EXISTS. docs/water-flow-effects-plan-2026-08-06.md ranks the ways
    // to make a river look like it is flowing, and the whole ranking turns on one
    // unmeasured number. Option A -- derive a direction from the gradient of the
    // water surface, the way Minecraft's FlowingFluid::getFlow derives one from
    // neighbouring fluid levels -- is FREE: the near-field mesher already
    // computes that gradient for its normal, and ChunkParams.w is a documented
    // free per-brick float. Option B ships the bake's own D8 receiver, which is
    // correct everywhere but costs a BAKE_VERSION roll and, until per-section
    // content addressing lands, ~13 GB of re-download.
    //
    // The risk that decides between them: the depth plane is int16 at a 10 mm
    // LSB, and a large share of a river's apparent "gradient" is the
    // epsilon-fill floor rather than terrain. Where the drop across the stencil
    // quantises to zero the direction is undefined -- and flat reaches are
    // exactly where a real river's motion reads most strongly.
    //
    // THE STENCIL IS +/-1 FINE PIXEL, NOT +/-1 BRICK, and that is a correction to
    // the plan. A brick is 8 voxels = 0.8 m, so a +/-1-brick stencil spans 1.6 m
    // -- SUB-PIXEL against an 1875 mm water plane. It would sample the same
    // stored control point on both sides and read exactly zero almost
    // everywhere, which would have looked like a devastating result and been an
    // artefact of the measurement. +/-1 pixel spans 3.75 m.
    //
    // WHAT THIS DOES NOT MEASURE: angular agreement against the bake's D8
    // receiver. That needs `rec_w` dumped from a diagnostic bake (pipeline.py
    // computes it at :4729 and deletes it at :4784) and is not available to a
    // tile-reading probe. So this answers "is a direction DEFINED here", not "is
    // it the RIGHT direction". A pass here is necessary, not sufficient.
    {
        const double spanMm = 2.0 * double(pixelMm); // centred difference, +/-1 px
        int64_t centredOk = 0;    // all four neighbours wet -> a centred difference exists
        int64_t resolved = 0;     // ...and the drop clears the 10 mm quantisation
        int64_t flatQuantised = 0;// ...and it does not
        int64_t centreCells = 0, centreResolved = 0;
        std::vector<double> slopes; // m/km, for percentiles
        slopes.reserve(1 << 16);

        for (int32_t y = 1; y + 1 < win.h; ++y) {
            for (int32_t x = 1; x + 1 < win.w; ++x) {
                const size_t si = win.at(x, y);
                if (!win.wet[si]) continue;
                const int32_t sxp = rivers.surfaceAtPixel(win.x0 + x + 1, win.y0 + y);
                const int32_t sxm = rivers.surfaceAtPixel(win.x0 + x - 1, win.y0 + y);
                const int32_t syp = rivers.surfaceAtPixel(win.x0 + x, win.y0 + y + 1);
                const int32_t sym = rivers.surfaceAtPixel(win.x0 + x, win.y0 + y - 1);
                const bool isCentre = !thin.centre.empty() && thin.centre[si] != 0;
                if (isCentre) ++centreCells;
                if (sxp == kNoWaterMm || sxm == kNoWaterMm || syp == kNoWaterMm ||
                    sym == kNoWaterMm)
                    continue; // no centred difference: this is a bank cell
                ++centredOk;
                const double gx = double(sxp - sxm) / spanMm;
                const double gy = double(syp - sym) / spanMm;
                const double mag = std::sqrt(gx * gx + gy * gy);
                // The smallest drop the wire can express across this stencil.
                // Below it the direction is not weakly determined, it is absent.
                const double lsbSlope = 10.0 / spanMm;
                if (mag >= lsbSlope) {
                    ++resolved;
                    if (isCentre) ++centreResolved;
                    if (slopes.size() < slopes.capacity()) slopes.push_back(mag * 1000.0);
                } else {
                    ++flatQuantised;
                }
            }
        }

        std::printf("\n=== WATER-SURFACE GRADIENT: can it carry a flow direction? ===\n");
        std::printf("  stencil           : +/-1 fine pixel = %.2f m span (NOT +/-1 brick, which is "
                    "sub-pixel here and would read zero)\n", spanMm / 1000.0);
        std::printf("  wet cells with a centred difference : %" PRId64 " (%.1f%% of wet; the rest "
                    "are bank cells with a dry neighbour)\n",
                    centredOk, wet ? 100.0 * double(centredOk) / double(wet) : 0.0);
        std::printf("  ...of those, DIRECTION RESOLVES     : %" PRId64 " (%.2f%%)\n", resolved,
                    centredOk ? 100.0 * double(resolved) / double(centredOk) : 0.0);
        std::printf("  ...quantised to zero (no direction) : %" PRId64 " (%.2f%%)\n", flatQuantised,
                    centredOk ? 100.0 * double(flatQuantised) / double(centredOk) : 0.0);
        std::printf("  centreline cells                    : %" PRId64 ", resolving %" PRId64
                    " (%.2f%%)\n",
                    centreCells, centreResolved,
                    centreCells ? 100.0 * double(centreResolved) / double(centreCells) : 0.0);
        if (!slopes.empty()) {
            std::sort(slopes.begin(), slopes.end());
            auto pct = [&](double p) {
                size_t i = size_t(p * double(slopes.size() - 1));
                return slopes[i];
            };
            std::printf("  |grad| where it resolves, m/km      : p10 %.1f  p50 %.1f  p90 %.1f  "
                        "max %.1f\n",
                        pct(0.10), pct(0.50), pct(0.90), slopes.back());
        }
        std::printf("  READ THIS AS: the percentage on the 'DIRECTION RESOLVES' line is the share "
                    "of interior wet cells\n"
                    "  where a gradient-derived flow direction exists at all. Option A in the flow "
                    "plan is viable only\n"
                    "  if it is high; the magnitude is a usable SPEED proxy either way.\n");
    }

    std::vector<RiverRibbonPath> paths;
    RiverTraceParams tp;
    riverRibbonTrace(win, thin, pixelMm, tp, paths);

    int64_t rawPts = 0;
    std::vector<int64_t> widthsMm, lengthsMm;
    int64_t totalLenMm = 0;
    int32_t datumMin = INT32_MAX, datumMax = INT32_MIN;
    for (const RiverRibbonPath& p : paths) {
        rawPts += int64_t(p.pts.size());
        int64_t len = 0;
        for (size_t i = 0; i < p.pts.size(); ++i) {
            widthsMm.push_back(int64_t(p.pts[i].halfWidthMm) * 2);
            datumMin = std::min(datumMin, p.pts[i].surfaceMm);
            datumMax = std::max(datumMax, p.pts[i].surfaceMm);
            if (i == 0) continue;
            const int64_t dx = p.pts[i].px - p.pts[i - 1].px, dy = p.pts[i].py - p.pts[i - 1].py;
            const int64_t steps = std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
            int64_t seg = steps * int64_t(pixelMm);
            if (dx != 0 && dy != 0) seg = (seg * kSqrt2Num) / kSqrt2Den;
            len += seg;
        }
        lengthsMm.push_back(len);
        totalLenMm += len;
    }
    std::printf("\n=== STAGE 3: paths ===\n");
    std::printf("  reaches           : %zu   (dropped below %.0f m)\n", paths.size(),
                double(tp.minPathLengthMm) / 1000.0);
    std::printf("  total channel     : %.2f km\n", double(totalLenMm) / 1e6);
    if (!lengthsMm.empty()) {
        std::vector<int64_t> l = lengthsMm;
        std::printf("  reach length      : p50 %.0f m  p90 %.0f m  max %.0f m\n",
                    double(pct(l, 50)) / 1000.0, double(pct(l, 90)) / 1000.0,
                    double(pct(l, 100)) / 1000.0);
    }
    {
        std::vector<int64_t> w = widthsMm;
        std::printf("  WIDTH (the bake's own, at the centreline):\n");
        std::printf("    p50 %.2f m   p90 %.2f m   p95 %.2f m   p99 %.2f m   max %.2f m\n",
                    double(pct(w, 50)) / 1000.0, double(pct(w, 90)) / 1000.0,
                    double(pct(w, 95)) / 1000.0, double(pct(w, 99)) / 1000.0,
                    double(pct(w, 100)) / 1000.0);
    }
    std::printf("  datum span        : %.1f m .. %.1f m  (RECONSTRUCTED ground + baked depth; NOT "
                "the amplified surface)\n",
                datumMin / 1000.0, datumMax / 1000.0);

    // WHERE THE LONGEST REACHES ACTUALLY ARE, in world METRES, because a camera
    // has to be pointed at one. "Verify sites before shooting" is a standing
    // rule here and it has caught three wrong vista sites including a "beach"
    // in open water; a capture aimed by hand at a tile centre photographs
    // whatever happens to be there, and an empty valley and a missing feature
    // are the same picture.
    //
    // METRES, AND THE UNIT IS THE WHOLE POINT OF THIS COLUMN. It exists to be
    // pasted into `-VoxelSpawnAt`, which VoxelEarthGameMode.cpp:60 documents as
    // "(meters, world)". The first version of this printed UNREAL UNITS, which
    // are 100x smaller, and the capture it aimed died on the fine-tier
    // residency gate asking for tile (-1003,-492) -- a tile 1,000 tiles off the
    // baked set. The gate caught it; a coarse-tier run would have photographed
    // the wrong valley instead and said nothing.
    if (!paths.empty()) {
        std::vector<size_t> order(paths.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](size_t a, size_t b) { return lengthsMm[a] > lengthsMm[b]; });
        const size_t show = std::min<size_t>(order.size(), 8);
        std::printf("  LONGEST REACHES -- midpoint in world METRES, for -VoxelSpawnAt:\n");
        std::printf("    %-8s %-9s %-26s %-9s %s\n", "len m", "width m", "mid m (x,y)", "datum m",
                    "span m (x,y)");
        for (size_t k = 0; k < show; ++k) {
            const RiverRibbonPath& p = paths[order[k]];
            if (p.pts.empty()) continue;
            const RiverRibbonPoint& mid = p.pts[p.pts.size() / 2];
            int64_t x0 = mid.px, x1 = mid.px, y0 = mid.py, y1 = mid.py, wSum = 0;
            for (const RiverRibbonPoint& q : p.pts) {
                x0 = std::min(x0, q.px);
                x1 = std::max(x1, q.px);
                y0 = std::min(y0, q.py);
                y1 = std::max(y1, q.py);
                wSum += int64_t(q.halfWidthMm) * 2;
            }
            // A fine pixel is 1875 mm, i.e. 1.875 m. The +half puts the point at
            // the PIXEL CENTRE, which is what the ribbon actor anchors its
            // vertices on.
            const double mPerPx = double(pixelMm) / 1000.0;
            std::printf("    %-8.0f %-9.2f (%.0f, %.0f)%*s %-9.1f (%.0f, %.0f)\n",
                        double(lengthsMm[order[k]]) / 1000.0,
                        double(wSum) / double(p.pts.size()) / 1000.0,
                        (double(mid.px) + 0.5) * mPerPx, (double(mid.py) + 0.5) * mPerPx, 4, "",
                        mid.surfaceMm / 1000.0, double(x1 - x0 + 1) * mPerPx,
                        double(y1 - y0 + 1) * mPerPx);
        }
    }

    // =======================================================================
    // DOES THE WATER SURFACE EVER RISE GOING DOWNSTREAM?
    // =======================================================================
    //
    // The owner, flying bake_ver 15: "possible I see a spot or two where the
    // magenta blocks actually seem to flow slightly up hill when they should be
    // descending down path of least resistance in a mountain river valleys."
    //
    // It should be impossible. `graded_water_surface` runs `enforce_descent`
    // over the D8 receiver forest, so the stored surface is non-increasing
    // downstream BY CONSTRUCTION. This checks the claim on the shipped tiles,
    // ALONG THE REACH -- which is the only direction the question means
    // anything in. A river's neighbours across the channel are at the same
    // level and its neighbours over the bank belong to a different reach, so
    // "compare against the downhill neighbour" answers a different question and
    // answers it wrongly.
    //
    // ONE METHOD TRAP, AND IT ALREADY CAUGHT ME. Recomputing steepest descent
    // from the tile's ELEVATION CONTROL POINTS and comparing there reports 51%
    // of wet cells rising at bv14 -- an alarming number and a worthless one,
    // because the control lattice stands up to 5.6 m off the surface it
    // interpolates (tilestore.h's three-grounds note). Descent computed on the
    // lattice is not descent on the ground. The reaches below carry
    // `surfaceMm` resolved through `RiverSampler`, i.e. reconstructed ground
    // plus baked depth -- ground #2, the datum's own surface.
    //
    // Reaches are ORIENTED first, so "downstream" is pts[0] -> pts.back().
    {
        size_t flatReaches = 0;
        riverRibbonOrient(paths, &flatReaches);

        int64_t steps = 0, rises = 0, risesOverVoxel = 0;
        int32_t worstRiseMm = 0;
        std::vector<int32_t> riseMm;
        for (const RiverRibbonPath& p : paths) {
            for (size_t i = 0; i + 1 < p.pts.size(); ++i) {
                const int32_t a = p.pts[i].surfaceMm, b = p.pts[i + 1].surfaceMm;
                if (a == kNoWaterMm || b == kNoWaterMm) continue;
                ++steps;
                const int32_t d = b - a;   // > 0 == the surface went UP downstream
                if (d > 0) {
                    ++rises;
                    if (d > int32_t(kVoxelSizeMm)) ++risesOverVoxel;
                    if (d > worstRiseMm) worstRiseMm = d;
                    riseMm.push_back(d);
                }
            }
        }
        std::printf("\n=== DOES THE SURFACE RISE DOWNSTREAM? (along oriented reaches) ===\n");
        std::printf("  reaches oriented  : %zu   (%zu level end-to-end -- standing water, no "
                    "direction)\n", paths.size(), flatReaches);
        std::printf("  downstream steps  : %" PRId64 "\n", steps);
        std::printf("  ...that RISE      : %" PRId64 " (%.4f%%)   over one voxel: %" PRId64 "\n",
                    rises, steps ? 100.0 * double(rises) / double(steps) : 0.0, risesOverVoxel);
        if (!riseMm.empty()) {
            std::sort(riseMm.begin(), riseMm.end());
            std::printf("  rise mm           : p50 %d  p90 %d  max %d\n",
                        riseMm[riseMm.size() / 2], riseMm[(riseMm.size() * 9) / 10], worstRiseMm);
        }
        std::printf("  READ THIS CAREFULLY. enforce_descent guarantees non-increasing surface along\n"
                    "  the D8 RECEIVER FOREST. This walks the traced MEDIAL AXIS, which is a\n"
                    "  different path -- it can cross between adjacent reaches at a stitch, and a\n"
                    "  centreline that wanders laterally samples cells that inherited their level\n"
                    "  from different reaches. So a non-zero count here is NOT proof that the\n"
                    "  stored plane violates its own guarantee.\n"
                    "  What it IS: the surface along the channel a player actually sees. A rise\n"
                    "  under the 100 mm wire LSB cannot be seen; a metre-scale one is water\n"
                    "  visibly climbing a valley, which is what was reported from the air.\n"
                    "  To separate the two, split these by whether they fall at a reach JOIN or\n"
                    "  mid-reach -- mid-reach rises cannot be blamed on tracing.\n");
    }

    // --- simplification: the anti-staircase measurement ----------------------
    int64_t keptPts = 0;
    RiverSimplifyParams sp;
    for (RiverRibbonPath& p : paths) {
        riverRibbonSimplify(p, sp);
        keptPts += int64_t(p.pts.size());
    }
    std::printf("\n=== STAGE 4: simplification (the staircase removal) ===\n");
    std::printf("  points  %" PRId64 " -> %" PRId64 "   (%.1fx fewer; tolerance %d px = %.2f m "
                "perpendicular, %d mm vertical)\n",
                rawPts, keptPts, keptPts ? double(rawPts) / double(keptPts) : 0.0, sp.xyTolPx,
                double(sp.xyTolPx * pixelMm) / 1000.0, sp.elevTolMm);
    std::printf("  mean segment      : %.1f m  (the emitted quad length along the channel)\n",
                keptPts > int64_t(paths.size())
                    ? double(totalLenMm) / double(keptPts - int64_t(paths.size())) / 1000.0
                    : 0.0);

    // --- the sub-pixel policy, as numbers ------------------------------------
    //
    // 1440p, 90 degrees horizontal FOV: half the screen (1280 px) subtends
    // tan(45) = 1, so one pixel at screen centre subtends 1/1280 in tangent
    // units. A world width w at distance d covers w/d * 1280 pixels.
    std::vector<int64_t> wsorted = widthsMm;
    std::sort(wsorted.begin(), wsorted.end());
    const double wP50 = double(pct(wsorted, 50)) / 1000.0;
    const double wP95 = double(pct(wsorted, 95)) / 1000.0;
    std::printf("\n=== THE SUB-PIXEL POLICY, at 1440p / 90 deg horizontal FOV ===\n");
    std::printf("  minimum screen width %.2f px, widening capped at %dx natural\n", minScreenPx,
                widenCap);
    std::printf("  %-9s | %-28s | %-28s\n", "range", "p50 reach (natural width)",
                "p95 reach (natural width)");
    std::printf("  %-9s | %6s %7s %8s %5s | %6s %7s %8s %5s\n", "", "nat px", "world m", "drawn px",
                "cap?", "nat px", "world m", "drawn px", "cap?");
    const double ranges[3] = {1000.0, 5000.0, 20000.0};
    for (double d : ranges) {
        std::printf("  %6.1f km | ", d / 1000.0);
        for (int which = 0; which < 2; ++which) {
            const double w = which == 0 ? wP50 : wP95;
            const double natPx = w / d * 1280.0;
            double drawM = w;
            bool capped = false;
            if (natPx < minScreenPx) {
                const double want = minScreenPx / 1280.0 * d;
                const double maxW = w * double(widenCap);
                drawM = want;
                if (drawM > maxW) {
                    drawM = maxW;
                    capped = true;
                }
            }
            const double drawPx = drawM / d * 1280.0;
            std::printf("%6.2f %7.2f %8.2f %5s%s", natPx, drawM, drawPx, capped ? "CAP" : "-",
                        which == 0 ? " | " : "\n");
        }
    }

    // --- will it be buried? --------------------------------------------------
    //
    // The ribbon is drawn at the water datum; the ground around it is drawn at
    // the AMPLIFIED surface. Positive headroom means the datum stands above the
    // drawn ground and the ribbon is visible; negative means the bank is in
    // front of it and the depth test wins.
    //
    // Measured at the centreline AND at the widened edge, because widening
    // pushes the ribbon out over the bank, and the edge is buried long before
    // the centre is.
    Amplifier amp(seed, fine);
    std::vector<int64_t> headCentre, head5km, head20km;
    int64_t sampled = 0, centreBuried = 0, edge5Buried = 0, edge20Buried = 0;
    const double want5 = minScreenPx / 1280.0 * 5000.0 * 1000.0;   // mm
    const double want20 = minScreenPx / 1280.0 * 20000.0 * 1000.0; // mm
    for (const RiverRibbonPath& p : paths) {
        for (size_t i = 0; i + 1 < p.pts.size(); ++i) {
            const RiverRibbonPoint& a = p.pts[i];
            const RiverRibbonPoint& b = p.pts[i + 1];
            const int64_t tx = b.px - a.px, ty = b.py - a.py;
            if (tx == 0 && ty == 0) continue;
            // Unit-ish perpendicular, scaled: (-ty, tx) / |t|.
            const int64_t l2 = tx * tx + ty * ty;
            int64_t l = 1;
            while ((l + 1) * (l + 1) <= l2) ++l; // integer sqrt, tiny values
            if (l <= 0) continue;

            auto headroomAt = [&](int64_t offMm) {
                // offset in pixels along the perpendicular
                const int64_t offPx = offMm / pixelMm;
                const int64_t qx = a.px + (-ty * offPx) / l;
                const int64_t qy = a.py + (tx * offPx) / l;
                const int64_t vx = (qx * pixelMm) / kVoxelSizeMm;
                const int64_t vy = (qy * pixelMm) / kVoxelSizeMm;
                return int64_t(a.surfaceMm) - int64_t(amp.surfaceMm(vx, vy));
            };
            ++sampled;
            const int64_t h0 = headroomAt(0);
            const int64_t h5 = headroomAt(int64_t(want5 / 2));
            const int64_t h20 = headroomAt(int64_t(std::min(want20 / 2, double(a.halfWidthMm) * widenCap)));
            headCentre.push_back(h0);
            head5km.push_back(h5);
            head20km.push_back(h20);
            centreBuried += (h0 < 0);
            edge5Buried += (h5 < 0);
            edge20Buried += (h20 < 0);
        }
    }
    std::printf("\n=== BURIAL: water datum MINUS amplified drawn ground ===\n");
    std::printf("  (positive = the ribbon stands above the ground the renderer draws)\n");
    std::printf("  samples: %" PRId64 "\n", sampled);
    if (sampled > 0) {
        std::printf("  %-26s p10 %7.0f mm  p50 %7.0f mm  p90 %7.0f mm   BURIED %5.2f%%\n",
                    "at the centreline", double(pct(headCentre, 10)), double(pct(headCentre, 50)),
                    double(pct(headCentre, 90)), 100.0 * double(centreBuried) / double(sampled));
        std::printf("  %-26s p10 %7.0f mm  p50 %7.0f mm  p90 %7.0f mm   BURIED %5.2f%%\n",
                    "at the 5 km widened edge", double(pct(head5km, 10)), double(pct(head5km, 50)),
                    double(pct(head5km, 90)), 100.0 * double(edge5Buried) / double(sampled));
        std::printf("  %-26s p10 %7.0f mm  p50 %7.0f mm  p90 %7.0f mm   BURIED %5.2f%%\n",
                    "at the 20 km capped edge", double(pct(head20km, 10)), double(pct(head20km, 50)),
                    double(pct(head20km, 90)), 100.0 * double(edge20Buried) / double(sampled));
    }
    std::printf("\nNOTE: the amplified surface read here is the PRE-channel-carve one on this "
                "branch. A carve that deepens channels can only INCREASE headroom at the "
                "centreline; it does not change the datum, which is reconstructed ground + "
                "baked depth and never reads the amplifier.\n");
    return 0;
}
