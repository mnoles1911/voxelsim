// vxc_farwaterschemes -- FOUR ways to draw distant water, measured against each
// other on the same tiles, at the same sites, from ONE shared water field.
//
// WHY THIS EXISTS. PR #226 (`vxc_farwaterprobe`) established that
// full-resolution voxel water to 1 km is not affordable (630k-7.2M surface
// bricks) and that a distance-doubling RING CASCADE is (16.9k-31.1k bricks,
// 38.3k-49.5k quads to 1,024 m). Two alternatives were then proposed, and the
// owner asked for them to be MEASURED before anything is committed:
//
//   (A) DRAW THE SURFACE, NOT THE VOLUME. Water is a film; a river 1 m deep and
//       100 m wide is ~99% interior that is never visible. Represent distant
//       water as a heightfield strip -- one quad per surface cell displaced to
//       the water surface -- instead of a stack of bricks.
//
//   (B) SURFACE PLUS INSTANCED RIM. Draw the interior as (A) and the RIM as
//       instanced boxes. The claim to test: rim length grows LINEARLY with
//       distance while interior area grows QUADRATICALLY, so the expensive part
//       is the part nobody looks at.
//
// THE COMPARISON IS ONLY HONEST IF ALL FOUR SEE THE SAME WATER. So this tool
// builds ONE cell-resolution field per ring by sampling `Amplifier::surfaceMm`
// (ground #3, what the renderer draws) and `CompositeWaterSampler` (the datum,
// which already unions the water plane with registered lake basins), and then
// counts all four schemes off that single field. `vxc_farwaterprobe` remains the
// cross-check: its cascade rows and this tool's cascade rows are computed by
// different aggregations of the same world and should agree to within the
// difference between "majority of fine BRICK COLUMNS" and "majority of CELLS".
//
// WHAT EACH SCHEME COSTS, precisely:
//
//   today's box     the 25.6 m sweep, reproduced verbatim from #226 so the
//                   baseline is re-measured here rather than quoted.
//   cascade         per ring, coarse columns -> brick z range -> the interior
//                   proof -> `meshBrick<8>` RUN FOR REAL. Quads counted, not
//                   estimated.
//   A (heightfield) one quad per wet surface CELL (the literal proposal), and
//                   also one quad per wet coarse COLUMN -- which is the
//                   LIKE-FOR-LIKE comparison against the cascade, because the
//                   cascade's water field is uniform across the 8x8 cells of a
//                   brick footprint and so its horizontal water resolution IS
//                   the coarse column, not the cell.
//   B (surface+rim) the column-resolution surface of A, plus ONE INSTANCED BOX
//                   PER RIM CELL, where a rim cell is a wet cell with at least
//                   one dry 4-neighbour.
//
// AND THREE THINGS THAT MUST BE CHECKED RATHER THAN ASSUMED:
//
//   1. SCALING. `--exp` walks a radius ladder at a FIXED cell size and fits
//      log(count) vs log(radius) for wet cells and for rim cells separately.
//      Fixing the cell size is the whole point: it separates how the GEOMETRY
//      grows from how the cascade's own coarsening shrinks it. If both
//      exponents come out near 1, B's asymptotic argument is dead, because a
//      river is a ONE-DIMENSIONAL feature and its area grows linearly with
//      radius exactly as its rim does.
//   2. SILENT DISAPPEARANCE. #226 found a coarse level that offered 138 surface
//      bricks and meshed ZERO QUADS: the whole water column fit inside one cell
//      and every cell read dry. This world's water is p50 0.75-1.18 m deep, so
//      every scheme reports VANISHED COLUMNS -- wet coarse columns that emit no
//      primitive at all -- per ring, and a nonzero count is a defect.
//   3. READS AS VOXELS. The owner's complaint was flat quads, and the silhouette
//      is the thing. What makes water read as voxels is the VERTICAL STEP where
//      the column meets the bank, so this reports the step height each scheme
//      leaves and how many pixels it subtends at the ring's own distance.
//
// Usage:
//   vxc_farwaterschemes <tiledir> --at Xm Ym [--radii "100,500,1000"]
//                       [--base 32] [--lods 6] [--exp] [--expcell 800]
//                       [--fov 90] [--width 1920] [--zstd PATH]

#include <algorithm>
#include <cinttypes>
#include <cmath>
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
// Without NOMINMAX, windows.h's min/max macros break every std::max here.
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "voxelcore/amplifier.h"
#include "voxelcore/farwater.h"
#include "voxelcore/lakes.h"
#include "voxelcore/mesher.h"
#include "voxelcore/tiles.h"
#include "voxelcore/tilestore.h"
#include "voxelcore/waterca.h"

using namespace vxc;

namespace {

// --- runtime zstd, bound the way the game binds it --------------------------
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

// One column of water: the amplified ground and the composed datum.
// `datumMm == kNoWaterMm` is DRY, and that is not the same as depth zero.
struct Col {
    int32_t groundMm = 0;
    int32_t datumMm = kNoWaterMm;
    bool wet() const { return datumMm != kNoWaterMm && int64_t(datumMm) > int64_t(groundMm); }
};

// A square grid of water columns at ONE cell size, in absolute cell coordinates.
// This is the single shared field every scheme is counted from.
struct CellField {
    int lod = 0;
    int64_t cellMm = 100;
    int64_t x0 = 0, y0 = 0, w = 0;
    std::vector<Col> cells;

    const Col& at(int64_t cx, int64_t cy) const {
        static const Col dry;
        if (cx < x0 || cx >= x0 + w || cy < y0 || cy >= y0 + w) return dry;
        return cells[size_t(cy - y0) * size_t(w) + size_t(cx - x0)];
    }
};

// BYTES, AND THE TWO CONVENTIONS ARE NOT INTERCHANGEABLE.
//
// #226 priced every quad at 152 B (4 verts x 32 B + 6 indices x 4 B), which is
// what an UNPACKED procedural mesh costs. That is right for the flat ribbon and
// sheet actors -- `AVoxelRiverRibbonActor::RebuildPath` really does push 4
// FVector verts and 6 indices per quad through a UProceduralMeshComponent -- and
// it is right for a displaced heightfield, which is the same kind of geometry.
//
// It is WRONG for the voxel path, and wrong by an order of magnitude. A voxel
// quad is `vxc::Quad`: nine uint8 fields, packed to EIGHT BYTES by
// `PackVoxelChunkQuad` and decoded in the shader (`VoxelQuadDecode.ush`). The
// water path carries one extra uint32 per quad for the packed corner heights.
// So a cascade quad is 12 B resident and a heightfield quad is 152 B -- a
// 12.7x per-primitive advantage to the voxel scheme that #226's uniform 152 B
// convention hid. Both columns are reported below; the ratios that matter do
// not depend on which is used, but the absolute megabytes very much do.
constexpr double kBytesPerProcQuad = 152.0;
constexpr double kBytesPerPackedQuad = 12.0; // 8 B packed quad + 4 B corner word
// A hardware-instanced box costs a per-instance transform. UE's instanced static
// mesh carries 4 x float4 per instance; the box mesh itself is shared once and
// is noise at these counts.
constexpr double kBytesPerInstance = 64.0;

double mib(double bytes) { return bytes / (1024.0 * 1024.0); }

// Least-squares slope of log(y) against log(x): the scaling exponent.
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

// What one scheme costs over one band.
struct SchemeCounts {
    uint64_t quads = 0;      // surface / mesh quads
    uint64_t instances = 0;  // B's rim boxes
    uint64_t surfBricks = 0; // cascade only
    uint64_t wetCells = 0;
    uint64_t rimCells = 0;
    uint64_t wetCols = 0;
    uint64_t vanishedCols = 0; // wet columns that emitted NOTHING -- a defect
    // THE DECOMPOSITION THAT DECIDES A AND B. `meshBrick` is a GREEDY mesher --
    // it merges coplanar faces into maximal rectangles -- so the cascade is NOT
    // paying one quad per surface cell for the water top. Splitting its output
    // into the +Z TOP faces (which is all a heightfield would draw) and the SIDE
    // faces (which is the stepped edge, i.e. the entire reason it reads as
    // voxels) says exactly what scheme A removes and what it costs.
    //
    // `topCells` is the sum of w*h over top faces: the count those faces would
    // become if every one of them were split back to 1x1. That is not
    // hypothetical -- `EmitWaterQuads` already splits +Z faces both ways into
    // unit quads whenever the corner-height field is non-planar over the merge,
    // which is the normal case on a sloping river. So `topCells` is the
    // cascade's post-split top-face ceiling, and it is directly comparable with
    // scheme A at cell resolution.
    uint64_t topQuads = 0, topCells = 0, sideQuads = 0;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: vxc_farwaterschemes <tiledir> --at Xm Ym [--radii \"100,500,1000\"] "
                     "[--base 32] [--lods 6] [--exp] [--expcell 800] [--fov 90] [--width 1920] "
                     "[--zstd PATH]\n");
        return 2;
    }
    std::string fineDir = argv[1], zstdPath, radiiArg = "100,500,1000";
    double atXm = 0.0, atYm = 0.0, baseM = 32.0, fovDeg = 90.0;
    bool haveAt = false, doExp = false;
    int lods = 6, screenW = 1920;
    int64_t expCellMm = 800;
    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--at") && i + 2 < argc) {
            atXm = std::strtod(argv[i + 1], nullptr);
            atYm = std::strtod(argv[i + 2], nullptr);
            haveAt = true;
            i += 2;
        } else if (!std::strcmp(a, "--radii") && i + 1 < argc) {
            radiiArg = argv[++i];
        } else if (!std::strcmp(a, "--base") && i + 1 < argc) {
            baseM = std::strtod(argv[++i], nullptr);
        } else if (!std::strcmp(a, "--lods") && i + 1 < argc) {
            lods = std::atoi(argv[++i]);
        } else if (!std::strcmp(a, "--exp")) {
            doExp = true;
        } else if (!std::strcmp(a, "--expcell") && i + 1 < argc) {
            expCellMm = std::strtoll(argv[++i], nullptr, 10);
        } else if (!std::strcmp(a, "--fov") && i + 1 < argc) {
            fovDeg = std::strtod(argv[++i], nullptr);
        } else if (!std::strcmp(a, "--width") && i + 1 < argc) {
            screenW = std::atoi(argv[++i]);
        } else if (!std::strcmp(a, "--zstd") && i + 1 < argc) {
            zstdPath = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a);
            return 2;
        }
    }
    if (!haveAt) {
        std::fprintf(stderr, "--at is required (use vxc_farwaterprobe --scan to pick a site)\n");
        return 2;
    }
    if (lods < 1) lods = 1;
    if (screenW < 1) screenW = 1920;
    if (expCellMm < int64_t(kVoxelSizeMm)) expCellMm = int64_t(kVoxelSizeMm);

    std::vector<double> radii;
    {
        const char* s = radiiArg.c_str();
        while (*s) {
            char* end = nullptr;
            const double v = std::strtod(s, &end);
            if (end == s) break;
            radii.push_back(v);
            s = end;
            while (*s == ',' || *s == ' ') ++s;
        }
    }
    std::sort(radii.begin(), radii.end());
    if (radii.empty()) {
        std::fprintf(stderr, "no radii\n");
        return 2;
    }

    bindZstd(zstdPath);

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

    // The seed comes off the tiles, never a flag.
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

    // Only the tiles the camera can actually reach are loaded: the whole wet
    // cache is 295 tiles and the widest radius here is 1 km, which is a fifth of
    // one tile. Loading the rest would be minutes of decode for nothing.
    FineTileSampler fine(seed, nullptr);
    fine.setDecompressor(dec);
    int refused = 0, skipped = 0;
    std::vector<std::pair<int32_t, int32_t>> loaded;
    {
        // Tile pitch is tileSize * pixelSize; read it from the first tile.
        FineError err0 = FineError::kNone;
        auto b0 = readBytes(files.front());
        if (!b0) return 1;
        auto t0 = FineTile::parse(std::move(*b0), dec, &err0);
        if (!t0) return 1;
        const int64_t tilePitchMm = int64_t(t0->blocksPerAxis()) * int64_t(t0->blockDim()) *
                                    int64_t(tilePixelSizeMm(kFineTileScale));
        const int64_t camXmm0 = int64_t(atXm * 1000.0), camYmm0 = int64_t(atYm * 1000.0);
        const int64_t reachMm = int64_t(radii.back() * 1000.0) + tilePitchMm;
        const int64_t tx0 = floorDiv(camXmm0 - reachMm, tilePitchMm);
        const int64_t tx1 = floorDiv(camXmm0 + reachMm, tilePitchMm);
        const int64_t ty0 = floorDiv(camYmm0 - reachMm, tilePitchMm);
        const int64_t ty1 = floorDiv(camYmm0 + reachMm, tilePitchMm);
        for (const auto& f : files) {
            // Filename is "<tx>_<ty>.vxtl"; parse it so untouched tiles are never
            // read from disk at all.
            const std::string stem = f.stem().string();
            const size_t us = stem.find('_');
            if (us != std::string::npos) {
                const long long ftx = std::strtoll(stem.c_str(), nullptr, 10);
                const long long fty = std::strtoll(stem.c_str() + us + 1, nullptr, 10);
                if (ftx < tx0 || ftx > tx1 || fty < ty0 || fty > ty1) {
                    ++skipped;
                    continue;
                }
            }
            FineError err = FineError::kNone;
            auto bytes = readBytes(f);
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
            std::printf("  tile (%d,%d) bakeVer=%u basins=%s(%zu) waterPlane=%s\n", t->tileX(),
                        t->tileY(), unsigned(t->header().bakeVer), t->hasBasins() ? "yes" : "NO",
                        t->basins().size(), t->hasWater() ? "yes" : "NO");
            loaded.push_back({t->tileX(), t->tileY()});
            fine.loadTile(std::move(*t));
        }
    }
    const int64_t pxMm = fine.pixelSizeMm();
    if (pxMm <= 0 || fine.tileSize() <= 0) {
        std::fprintf(stderr, "no tile loaded (%d refused, %d out of reach)\n", refused, skipped);
        return 1;
    }
    std::printf("seed %llu  tiles %zu loaded (%d refused, %d out of reach)  pixel %lld mm\n",
                (unsigned long long)seed, fine.tileCount(), refused, skipped, (long long)pxMm);

    LakeSampler lakes(fine);
    RiverSampler rivers(fine);
    CompositeWaterSampler both(lakes, rivers);
    Amplifier amp(seed, fine);

    const int64_t camXmm = int64_t(atXm * 1000.0), camYmm = int64_t(atYm * 1000.0);
    const int64_t camVx = floorDiv(camXmm, int64_t(kVoxelSizeMm));
    const int64_t camVy = floorDiv(camYmm, int64_t(kVoxelSizeMm));
    const int64_t edge = int64_t(WaterBrick8::kEdge);
    const int64_t camBx = floorDiv(camVx, edge), camBy = floorDiv(camVy, edge);
    std::printf("\ncamera at world (%.1f, %.1f) m -> brick column (%lld, %lld)\n", atXm, atYm,
                (long long)camBx, (long long)camBy);
    std::printf("cascade base %.0f m, %d LOD levels, outer radius %.0f m\n", baseM, lods,
                baseM * double(int64_t(1) << (lods - 1)));

    // Angular size of one screen pixel, the same small-angle convention #226
    // used (FOV / width): at 90 deg on 1920 px that is 8.18e-4 rad.
    const double radPerPixel = (fovDeg * 3.14159265358979323846 / 180.0) / double(screenW);
    std::printf("screen: %d px wide at %.0f deg HFOV -> %.3e rad/px\n", screenW, fovDeg,
                radPerPixel);

    // -----------------------------------------------------------------------
    // TODAY'S BOX, reproduced verbatim from #226 so the baseline is measured in
    // this run rather than quoted from another one.
    // -----------------------------------------------------------------------
    uint64_t boxMeshed = 0, boxQuads = 0;
    {
        constexpr int32_t kR = 32, kRZ = 16;
        const int32_t gMm = amp.surfaceMm(camVx, camVy);
        const int32_t dMm = both.waterSurfaceMmAtVoxel(camVx, camVy);
        const int64_t camZmm = dMm == kNoWaterMm ? int64_t(gMm) : int64_t(dMm);
        const int64_t camBz = floorDiv(floorDiv(camZmm, int64_t(kVoxelSizeMm)), edge);
        int64_t admitted = 0, candidates = 0;
        std::vector<Quad> raw;
        for (int64_t by = camBy - kR; by <= camBy + kR; ++by) {
            for (int64_t bx = camBx - kR; bx <= camBx + kR; ++bx) {
                const int64_t vx = bx * edge, vy = by * edge;
                const int32_t lakeZMm = both.waterSurfaceMmAtVoxel(vx, vy);
                if (lakeZMm == kNoWaterMm) continue;
                ++admitted;
                const int64_t floodBrickZ =
                    floorDiv(int64_t(lakeZMm) / int64_t(kVoxelSizeMm), edge);
                const int32_t groundMm = amp.surfaceMm(vx, vy);
                for (int64_t bz = camBz - kRZ; bz <= camBz + kRZ; ++bz) {
                    const int64_t padBottomMm = (bz * edge - 1) * kVoxelSizeMm;
                    const int64_t padTopMm = (bz * edge + edge) * kVoxelSizeMm;
                    if (padBottomMm >= groundMm && padTopMm + kVoxelSizeMm <= int64_t(lakeZMm))
                        continue;
                    if (bz > floodBrickZ) continue;
                    ++candidates;
                    raw.clear();
                    const int64_t oz = bz * edge;
                    meshBrick<WaterBrick8::kEdge>(
                        [&](int x, int y, int z) -> MaterialId {
                            const int64_t nvx =
                                vx + (x < 0 ? -1 : (x >= int(edge) ? int64_t(edge) : x));
                            const int64_t nvy =
                                vy + (y < 0 ? -1 : (y >= int(edge) ? int64_t(edge) : y));
                            const int32_t g = amp.surfaceMm(nvx, nvy);
                            const int32_t d = both.waterSurfaceMmAtVoxel(nvx, nvy);
                            const uint8_t fill = implicitWaterFill(oz + z, g, d, false);
                            return fill >= 8 ? MaterialId(1) : MaterialId(MAT_AIR);
                        },
                        raw);
                    if (!raw.empty()) {
                        ++boxMeshed;
                        boxQuads += raw.size();
                    }
                }
            }
        }
        std::printf("\n=== TODAY: the 25.6 m box (65 x 65 x 33 bricks) ===\n");
        std::printf("  columns admitted wet %lld,  candidate bricks %lld\n", (long long)admitted,
                    (long long)candidates);
        std::printf("  MESHED BRICKS %llu   QUADS %llu   %.2f MiB packed\n",
                    (unsigned long long)boxMeshed, (unsigned long long)boxQuads,
                    mib(double(boxQuads) * kBytesPerPackedQuad));
    }

    // -----------------------------------------------------------------------
    // THE SHARED FIELD. One cell grid per LOD, sampled directly from the
    // amplifier and the composite water sampler. Every scheme below reads THIS
    // and nothing else, so a difference between two rows is a difference between
    // two SCHEMES and never between two samplings of the world.
    // -----------------------------------------------------------------------
    auto buildField = [&](int lod, double outerM) -> CellField {
        CellField f;
        f.lod = lod;
        f.cellMm = int64_t(kVoxelSizeMm) << lod;
        const int64_t rCells = int64_t(std::ceil(outerM * 1000.0 / double(f.cellMm)));
        const int64_t camCx = floorDiv(camXmm, f.cellMm), camCy = floorDiv(camYmm, f.cellMm);
        f.w = 2 * (rCells + 2) + 1;
        f.x0 = camCx - rCells - 2;
        f.y0 = camCy - rCells - 2;
        f.cells.assign(size_t(f.w) * size_t(f.w), Col{});
        for (int64_t cy = 0; cy < f.w; ++cy) {
            for (int64_t cx = 0; cx < f.w; ++cx) {
                // Sample at the cell CENTRE, converted to a voxel coordinate --
                // the samplers are voxel-addressed and integer-only.
                const int64_t wx = (f.x0 + cx) * f.cellMm + f.cellMm / 2;
                const int64_t wy = (f.y0 + cy) * f.cellMm + f.cellMm / 2;
                const int64_t vx = floorDiv(wx, int64_t(kVoxelSizeMm));
                const int64_t vy = floorDiv(wy, int64_t(kVoxelSizeMm));
                const int32_t d = both.waterSurfaceMmAtVoxel(vx, vy);
                if (d == kNoWaterMm) continue; // dry: never pay for the ground
                Col c;
                c.datumMm = d;
                c.groundMm = amp.surfaceMm(vx, vy);
                f.cells[size_t(cy) * size_t(f.w) + size_t(cx)] = c;
            }
        }
        return f;
    };

    // A coarse COLUMN is the 8 x 8 cell footprint of one brick. Wet iff at least
    // half its cells are wet -- the same shape of rule as mips.h's
    // `solidThreshold = 4 of 8`, and for the reason farwater.h documents: a
    // strict-any rule grows every river outward one cell per level, a strict-all
    // erases every river narrower than the cell. Ground and datum are MEANS over
    // the wet children, which is what holds the coarse surface at the same
    // height as the fine one and keeps the ring boundary invisible.
    auto coarseCol = [&](const CellField& f, int64_t colX, int64_t colY, Col& out) -> bool {
        int64_t n = 0, sumG = 0, sumD = 0;
        for (int64_t j = 0; j < edge; ++j) {
            for (int64_t i = 0; i < edge; ++i) {
                const Col& c = f.at(colX * edge + i, colY * edge + j);
                if (!c.wet()) continue;
                ++n;
                sumG += c.groundMm;
                sumD += c.datumMm;
            }
        }
        if (n * 2 < edge * edge) return false;
        out.groundMm = int32_t(sumG / n);
        out.datumMm = int32_t(sumD / n);
        return true;
    };

    // -----------------------------------------------------------------------
    // COUNT ONE BAND, ALL FOUR SCHEMES, off one field.
    // -----------------------------------------------------------------------
    struct BandResult {
        SchemeCounts casc, aCell, aCol, b;
        int64_t cellMm = 100;
    };

    auto countBand = [&](const CellField& f, double rInnerM, double rOuterM) -> BandResult {
        BandResult R;
        R.cellMm = f.cellMm;
        const int64_t rIn = int64_t(rInnerM * 1000.0), rOut = int64_t(rOuterM * 1000.0);
        const int64_t rIn2 = rIn * rIn, rOut2 = rOut * rOut;

        auto inBand = [&](int64_t wx, int64_t wy) -> bool {
            const int64_t dx = wx - camXmm, dy = wy - camYmm;
            const int64_t d2 = dx * dx + dy * dy;
            return d2 >= rIn2 && d2 < rOut2;
        };

        // --- cell-resolution pass: scheme A's literal form, and the rim -----
        for (int64_t cy = f.y0; cy < f.y0 + f.w; ++cy) {
            for (int64_t cx = f.x0; cx < f.x0 + f.w; ++cx) {
                const Col& c = f.at(cx, cy);
                if (!c.wet()) continue;
                const int64_t wx = cx * f.cellMm + f.cellMm / 2;
                const int64_t wy = cy * f.cellMm + f.cellMm / 2;
                if (!inBand(wx, wy)) continue;
                ++R.aCell.wetCells;
                // RIM = a wet cell with at least one dry 4-neighbour. This is
                // the water/bank boundary, which is the silhouette the owner is
                // actually looking at.
                const bool rim = !f.at(cx - 1, cy).wet() || !f.at(cx + 1, cy).wet() ||
                                 !f.at(cx, cy - 1).wet() || !f.at(cx, cy + 1).wet();
                if (rim) ++R.aCell.rimCells;
            }
        }
        R.aCell.quads = R.aCell.wetCells; // one displaced quad per surface cell

        // --- coarse-column pass: the cascade, A at column resolution, and B --
        const int64_t col0x = floorDiv(f.x0, edge) + 1, col1x = floorDiv(f.x0 + f.w - 1, edge) - 1;
        const int64_t col0y = floorDiv(f.y0, edge) + 1, col1y = floorDiv(f.y0 + f.w - 1, edge) - 1;
        const int64_t cw = col1x - col0x + 1, ch = col1y - col0y + 1;
        if (cw <= 0 || ch <= 0) return R;
        // Cached: the interior proof and the mesh both read a one-column apron,
        // and recomputing an 8x8 aggregate per read is a factor of nine on the
        // inner loop.
        std::vector<Col> cc(size_t(cw) * size_t(ch));
        for (int64_t cy = col0y; cy <= col1y; ++cy)
            for (int64_t cx = col0x; cx <= col1x; ++cx) {
                Col o;
                if (coarseCol(f, cx, cy, o))
                    cc[size_t(cy - col0y) * size_t(cw) + size_t(cx - col0x)] = o;
            }
        auto ccAt = [&](int64_t cx, int64_t cy) -> const Col& {
            static const Col dry;
            if (cx < col0x || cx > col1x || cy < col0y || cy > col1y) return dry;
            return cc[size_t(cy - col0y) * size_t(cw) + size_t(cx - col0x)];
        };

        const int64_t colMm = f.cellMm * edge;
        std::vector<Quad> raw;
        for (int64_t cy = col0y; cy <= col1y; ++cy) {
            for (int64_t cx = col0x; cx <= col1x; ++cx) {
                const Col& c = ccAt(cx, cy);
                if (!c.wet()) continue;
                const int64_t wx = cx * colMm + colMm / 2, wy = cy * colMm + colMm / 2;
                if (!inBand(wx, wy)) continue;
                ++R.casc.wetCols;

                // ---- scheme A at column resolution: ONE quad, displaced -----
                ++R.aCol.wetCols;
                ++R.aCol.quads;

                // ---- the cascade: brick z range, interior proof, real mesh --
                const uint64_t quadsBefore = R.casc.quads;
                const int64_t gz = floorDiv(int64_t(c.groundMm), f.cellMm);
                const int64_t dz = floorDiv(int64_t(c.datumMm) - 1, f.cellMm);
                for (int64_t bz = floorDiv(gz, edge); bz <= floorDiv(dz, edge); ++bz) {
                    const int64_t padBottomMm = (bz * edge - 1) * f.cellMm;
                    const int64_t padTopMm = (bz * edge + edge) * f.cellMm;
                    bool interior = true;
                    for (int64_t j = -1; j <= 1 && interior; ++j)
                        for (int64_t i = -1; i <= 1 && interior; ++i) {
                            const Col& n = ccAt(cx + i, cy + j);
                            if (!n.wet() || int64_t(n.groundMm) > padBottomMm ||
                                padTopMm + f.cellMm > int64_t(n.datumMm))
                                interior = false;
                        }
                    if (interior) continue;
                    ++R.casc.surfBricks;
                    raw.clear();
                    const int64_t oz = bz * edge;
                    meshBrick<WaterBrick8::kEdge>(
                        [&](int x, int y, int z) -> MaterialId {
                            const Col& n = ccAt(cx + (x < 0 ? -1 : (x >= int(edge) ? 1 : 0)),
                                                cy + (y < 0 ? -1 : (y >= int(edge) ? 1 : 0)));
                            // farWaterFill, CALLED -- not reimplemented. At LOD 0
                            // it reduces to implicitWaterFill, which is what the
                            // near field runs, so the two cannot drift.
                            const uint8_t fill = farWaterFill((oz + z) * f.cellMm, n.groundMm,
                                                              n.datumMm, f.cellMm);
                            return fill >= 8 ? MaterialId(1) : MaterialId(MAT_AIR);
                        },
                        raw);
                    R.casc.quads += raw.size();
                    // Split the greedy output by face direction. axis 2 positive
                    // is the water TOP; everything else is the stepped edge.
                    for (const Quad& q : raw) {
                        if (q.axis == 2 && q.positive) {
                            ++R.casc.topQuads;
                            R.casc.topCells += uint64_t(q.w) * uint64_t(q.h);
                        } else {
                            ++R.casc.sideQuads;
                        }
                    }
                }
                // SILENT DISAPPEARANCE CHECK. A wet column that emitted no quad
                // is the #226 failure: the water did not get coarser, it ceased
                // to exist. Nonzero here is a defect, not a cost.
                if (R.casc.quads == quadsBefore) ++R.casc.vanishedCols;

                // ---- scheme B: A's surface, plus one box per RIM CELL -------
                ++R.b.quads;
                for (int64_t j = 0; j < edge; ++j)
                    for (int64_t i = 0; i < edge; ++i) {
                        const int64_t sx = cx * edge + i, sy = cy * edge + j;
                        if (!f.at(sx, sy).wet()) continue;
                        if (!f.at(sx - 1, sy).wet() || !f.at(sx + 1, sy).wet() ||
                            !f.at(sx, sy - 1).wet() || !f.at(sx, sy + 1).wet())
                            ++R.b.instances;
                    }
            }
        }
        R.b.wetCols = R.aCol.wetCols;
        return R;
    };

    // -----------------------------------------------------------------------
    // THE CASCADE'S RING STRUCTURE, and every scheme measured over it.
    //
    // "Scheme A at 1 km" means A drawn over the SAME rings the cascade uses --
    // that is the decision in front of the owner. Nobody is choosing between the
    // cascade and full-resolution A at a kilometre; #226 already priced that at
    // 630k-7.2M bricks and it is not on the table.
    // -----------------------------------------------------------------------
    struct Cumulative {
        uint64_t cascQuads = 0, cascBricks = 0, cascVanished = 0;
        uint64_t cascTopQuads = 0, cascTopCells = 0, cascSideQuads = 0;
        uint64_t aCellQuads = 0, aColQuads = 0;
        uint64_t bQuads = 0, bInstances = 0;
        uint64_t wetCells = 0, rimCells = 0;
        double worstStepPx = 0.0;
    };
    std::vector<Cumulative> perRadius(radii.size());

    std::printf("\n=== PER-RING, EVERY SCHEME OFF ONE FIELD (base %.0f m) ===\n", baseM);
    std::printf("%4s %8s %8s %8s | %9s %9s %7s | %9s %9s | %9s %9s | %8s %8s %7s\n", "ring",
                "voxel", "from", "to", "casc brk", "casc qd", "vanish", "A cell qd", "A col qd",
                "B qd", "B inst", "wetcell", "rimcell", "rim%");

    for (int lod = 0; lod < lods; ++lod) {
        const double rIn = lod == 0 ? 0.0 : baseM * double(int64_t(1) << (lod - 1));
        const double rOut = baseM * double(int64_t(1) << lod);
        if (rIn >= radii.back()) break;
        const CellField f = buildField(lod, rOut);
        // Clip the ring to each requested radius so the cumulative totals are
        // true totals at exactly 100 / 500 / 1000 m rather than at ring edges.
        for (size_t ri = 0; ri < radii.size(); ++ri) {
            const double lim = radii[ri];
            if (rIn >= lim) continue;
            const BandResult br = countBand(f, rIn, std::min(rOut, lim));
            Cumulative& cu = perRadius[ri];
            cu.cascQuads += br.casc.quads;
            cu.cascBricks += br.casc.surfBricks;
            cu.cascVanished += br.casc.vanishedCols;
            cu.cascTopQuads += br.casc.topQuads;
            cu.cascTopCells += br.casc.topCells;
            cu.cascSideQuads += br.casc.sideQuads;
            cu.aCellQuads += br.aCell.quads;
            cu.aColQuads += br.aCol.quads;
            cu.bQuads += br.b.quads;
            cu.bInstances += br.b.instances;
            cu.wetCells += br.aCell.wetCells;
            cu.rimCells += br.aCell.rimCells;
        }
        // The un-clipped ring, for the per-ring table and the flatness argument.
        const BandResult br = countBand(f, rIn, rOut);
        std::printf("%4d %7.2fm %7.0fm %7.0fm | %9llu %9llu %7llu | %9llu %9llu | %9llu %9llu | "
                    "%8llu %8llu %6.1f%%\n",
                    lod, double(f.cellMm) / 1000.0, rIn, rOut,
                    (unsigned long long)br.casc.surfBricks, (unsigned long long)br.casc.quads,
                    (unsigned long long)br.casc.vanishedCols, (unsigned long long)br.aCell.quads,
                    (unsigned long long)br.aCol.quads, (unsigned long long)br.b.quads,
                    (unsigned long long)br.b.instances, (unsigned long long)br.aCell.wetCells,
                    (unsigned long long)br.aCell.rimCells,
                    br.aCell.wetCells ? 100.0 * double(br.aCell.rimCells) /
                                            double(br.aCell.wetCells)
                                      : 0.0);
        // READS AS VOXELS. The step that matters is the VERTICAL one where the
        // water column meets the bank; its height is the cell, and it is drawn
        // at the ring's own distance. A ring whose step falls under a pixel has
        // stopped reading as voxels no matter what it costs.
        const double stepPx = (double(f.cellMm) / 1000.0) / rOut / radPerPixel;
        for (size_t ri = 0; ri < radii.size(); ++ri)
            if (rIn < radii[ri]) perRadius[ri].worstStepPx =
                perRadius[ri].worstStepPx == 0.0 ? stepPx : std::min(perRadius[ri].worstStepPx, stepPx);
    }

    // -----------------------------------------------------------------------
    // THE SINGLE TABLE.
    // -----------------------------------------------------------------------
    // WHAT THE GREEDY MESHER ALREADY DOES. Scheme A's premise is that the
    // cascade is drawing a VOLUME and that only the surface is visible. Both
    // halves of that are already handled: the interior proof drops bricks that
    // emit no face, and `meshBrick` is GREEDY, so the flat top of a water column
    // is merged into a maximal rectangle rather than one quad per cell. This
    // table says how much of the cascade's cost is the surface (what A would
    // keep) and how much is the stepped edge (what A would delete).
    std::printf("\n=== WHAT THE CASCADE IS ACTUALLY DRAWING ===\n");
    std::printf("%7s %12s %12s %12s %12s %10s\n", "dist", "casc quads", "top faces", "side faces",
                "top cells", "side %");
    for (size_t ri = 0; ri < radii.size(); ++ri) {
        const Cumulative& cu = perRadius[ri];
        std::printf("%6.0fm %12llu %12llu %12llu %12llu %9.1f%%\n", radii[ri],
                    (unsigned long long)cu.cascQuads, (unsigned long long)cu.cascTopQuads,
                    (unsigned long long)cu.cascSideQuads, (unsigned long long)cu.cascTopCells,
                    cu.cascQuads ? 100.0 * double(cu.cascSideQuads) / double(cu.cascQuads) : 0.0);
    }
    std::printf("  'top cells' is what the top faces become if EmitWaterQuads splits every one\n"
                "  of them back to 1x1 -- the cascade's worst case, and scheme A's best case.\n");

    std::printf("\n=== SCHEME x DISTANCE ===\n");
    std::printf("%-28s %7s %12s %12s %10s %10s  %s\n", "scheme", "dist", "quads", "instances",
                "MiB real", "MiB @152B", "reads as voxels?");
    std::printf("%-28s %6.0fm %12llu %12s %10.2f %10.2f  %s\n", "today: 25.6 m box", 25.6,
                (unsigned long long)boxQuads, "-", mib(double(boxQuads) * kBytesPerPackedQuad),
                mib(double(boxQuads) * kBytesPerProcQuad), "YES 0.10 m step, full voxel column");
    for (size_t ri = 0; ri < radii.size(); ++ri) {
        const Cumulative& cu = perRadius[ri];
        const double R = radii[ri];
        char note[160];
        std::snprintf(note, sizeof(note), "YES %.1f px min vertical step across rings",
                      cu.worstStepPx);
        std::printf("%-28s %6.0fm %12llu %12s %10.2f %10.2f  %s\n", "ring cascade", R,
                    (unsigned long long)cu.cascQuads, "-",
                    mib(double(cu.cascQuads) * kBytesPerPackedQuad),
                    mib(double(cu.cascQuads) * kBytesPerProcQuad), note);
        std::printf("%-28s %6.0fm %12llu %12s %10.2f %10.2f  %s\n", "A: heightfield @cell", R,
                    (unsigned long long)cu.aCellQuads, "-",
                    mib(double(cu.aCellQuads) * kBytesPerProcQuad),
                    mib(double(cu.aCellQuads) * kBytesPerProcQuad),
                    "NO  a film has no vertical step");
        std::printf("%-28s %6.0fm %12llu %12s %10.2f %10.2f  %s\n", "A: heightfield @column", R,
                    (unsigned long long)cu.aColQuads, "-",
                    mib(double(cu.aColQuads) * kBytesPerProcQuad),
                    mib(double(cu.aColQuads) * kBytesPerProcQuad),
                    "NO  a film has no vertical step");
        std::printf("%-28s %6.0fm %12llu %12llu %10.2f %10.2f  %s\n", "B: surface + instanced rim",
                    R, (unsigned long long)cu.bQuads, (unsigned long long)cu.bInstances,
                    mib(double(cu.bQuads) * kBytesPerProcQuad +
                        double(cu.bInstances) * kBytesPerInstance),
                    mib(double(cu.bQuads) * kBytesPerProcQuad +
                        double(cu.bInstances) * kBytesPerInstance),
                    note);
        std::printf("%-28s %6.0fm  cascade vanished columns %llu   wet cells %llu   rim cells %llu "
                    "(%.1f%%)\n",
                    "  [checks]", R, (unsigned long long)cu.cascVanished,
                    (unsigned long long)cu.wetCells, (unsigned long long)cu.rimCells,
                    cu.wetCells ? 100.0 * double(cu.rimCells) / double(cu.wetCells) : 0.0);
    }

    // -----------------------------------------------------------------------
    // THE SCALING EXPONENT, at a FIXED cell size.
    //
    // THIS IS THE TEST THAT DECIDES B. The claim is that rim length grows
    // LINEARLY with radius while interior area grows QUADRATICALLY, so the
    // interior is the expensive part and instancing only the rim is nearly free.
    // Fixing the cell size is essential: measured through the cascade's own
    // coarsening, every count looks flat and tells you nothing about the
    // geometry. If both exponents land near 1, the claim is false here -- a
    // river is a ONE-DIMENSIONAL feature and sweeping a radius over it adds
    // length, not area.
    // -----------------------------------------------------------------------
    if (doExp) {
        std::printf("\n=== SCALING AT A FIXED %.2f m CELL ===\n", double(expCellMm) / 1000.0);
        int expLod = 0;
        while ((int64_t(kVoxelSizeMm) << expLod) < expCellMm && expLod < 20) ++expLod;
        const CellField f = buildField(expLod, radii.back());
        std::printf("(cell %.2f m; grid %lld x %lld)\n", double(f.cellMm) / 1000.0, (long long)f.w,
                    (long long)f.w);
        std::vector<double> ladder = {50, 100, 150, 200, 300, 400, 500, 700, 850, 1000};
        while (!ladder.empty() && ladder.back() > radii.back()) ladder.pop_back();
        std::vector<double> xs, wetYs, rimYs;
        std::printf("%8s %12s %12s %12s %10s\n", "radius", "wet cells", "rim cells", "interior",
                    "rim %");
        for (double R : ladder) {
            const BandResult br = countBand(f, 0.0, R);
            xs.push_back(R);
            wetYs.push_back(double(br.aCell.wetCells));
            rimYs.push_back(double(br.aCell.rimCells));
            std::printf("%7.0fm %12llu %12llu %12llu %9.1f%%\n", R,
                        (unsigned long long)br.aCell.wetCells,
                        (unsigned long long)br.aCell.rimCells,
                        (unsigned long long)(br.aCell.wetCells - br.aCell.rimCells),
                        br.aCell.wetCells
                            ? 100.0 * double(br.aCell.rimCells) / double(br.aCell.wetCells)
                            : 0.0);
        }
        const double wetExp = logLogSlope(xs, wetYs);
        const double rimExp = logLogSlope(xs, rimYs);
        std::printf("\n  EXPONENT  wet/interior area  R^%.2f\n", wetExp);
        std::printf("  EXPONENT  rim length         R^%.2f\n", rimExp);
        std::printf("  B's premise needs interior ~R^2 and rim ~R^1, i.e. a gap near 1.0.\n");
        std::printf("  MEASURED GAP: %.2f\n", wetExp - rimExp);
    }

    std::printf("\nNOTE: quads are pre-split; EmitWaterQuads splits some again for the corner\n"
                "field, so every quad column is a floor and the ratios are what to read.\n");
    return 0;
}
