// vxc_farwaterprobe -- "how many water BRICKS and QUADS are actually within
// 100 m / 500 m / 1 km of this camera, if water candidates came from the baked
// water plane instead of from a dense box sweep?"
//
// WHY THIS EXISTS. `RefreshImplicitWater` finds water by sweeping a dense box
// of brick columns -- 65 x 65 = 4,225 of them -- and asking each one whether it
// is wet. The box is +/-25.6 m, and beyond it rivers and lakes are drawn as
// flat quads, which the owner has rejected. Scaling the box does not work: the
// column count is quadratic in the radius, so 500 m is 1,563,001 columns (370x)
// and 1 km is 6,255,001 (1,480x).
//
// The premise this tool tests is that the sweep is dense only because it does
// not know where the water is -- and the baked water plane does. So the
// candidate set can be ENUMERATED from the plane (block-major, the way
// `riverRibbonFillWet` already does it) rather than SEARCHED for, at which
// point the cost stops scaling with swept area and starts scaling with water
// area.
//
// That premise being true does not make full-resolution water to 1 km
// affordable -- water area is small, but it is not zero, and a 45 m deep lake
// is a lot of bricks. So this tool measures BOTH:
//
//   * the candidate/brick/quad cost of full-resolution (0.1 m voxel) water at
//     each radius, against the ~4,500 bricks the 25.6 m box costs today, and
//   * the same counts at coarser water LODs, so the "reads as voxels, not a
//     flat plane" answer can be sized rather than guessed.
//
// WHAT IT COUNTS, and the distinctions matter:
//
//   wet fine px      water-plane pixels with depth >= 0, plus registered
//                    lake-basin extent cells (the bake writes the plane DRY
//                    inside a basin, so the two are disjoint and both are
//                    water -- CompositeWaterSampler unions them)
//   wet columns      water BRICK COLUMNS (0.8 m) whose datum stands above the
//                    amplified ground. This is the near field's own wet test.
//   water bricks     every brick between the ground and the datum. What a
//                    naive "offer everything wet" candidate rule costs.
//   surface bricks   the subset that emits at least one face, by the SAME
//                    proof the sweep's interior skip uses: a brick is interior
//                    iff its whole padded neighbourhood is full water. This is
//                    the number that actually costs a mesh and a draw.
//   quads            `meshBrick<8>` run for real over each surface brick, with
//                    the fill from `implicitWaterFill`. Pre-split: the client's
//                    EmitWaterQuads splits some of these again for the corner
//                    field, so this is a floor, not a ceiling.
//
// THE GROUND IS #3, THE AMPLIFIED ONE (`Amplifier::surfaceMm`), because that is
// what `FVoxelWaterImpl::ImplicitFillAtVoxel` passes to `implicitWaterFill`.
// The DATUM comes from `CompositeWaterSampler`, which measures from ground #2
// (`reconstructedGroundMm`) internally. Mixing those up is the conflation
// `tilestore.h` documents having been made three times across two languages.
//
// THE OCEAN IS EXCLUDED, matching `implicitWaterCeilingMm` and the sweep: an
// untouched sea is drawn by AVoxelOceanActor's plane and offering it here would
// be every brick at every shoreline.
//
// Usage:
//   vxc_farwaterprobe <tiledir> [--at Xm Ym] [--radii "100,500,1000"]
//                     [--scan] [--lods N] [--zstd PATH]
//
//   --at      camera world position in METRES. Default: the centre of the
//             wettest 480 m water block in the loaded set (i.e. --scan's pick).
//   --radii   comma-separated radii in metres (default 100,500,1000)
//   --scan    report the wettest water blocks in the loaded set and stop. Use
//             it to choose a camera site; a site off the river measures
//             nothing.
//   --lods    how many LOD levels to table (default 4: 0.1/0.2/0.4/0.8 m)
//   --zstd    explicit libzstd path

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
// See bankprobe.cpp: without NOMINMAX, windows.h's min/max macros break every
// std::max in this file under MSVC.
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "voxelcore/amplifier.h"
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

// A brick column's two heights. kNoWaterMm datum means "no implicit water in
// this column at any height", which is NOT the same as depth zero.
struct Col {
    int32_t groundMm = 0;
    int32_t datumMm = kNoWaterMm;
};

// The coarse-LOD fill rule.
//
// AT LOD 0 IT IS `implicitWaterFill`, CALLED, not reimplemented -- the near
// field must not move by one unit, and the only way to guarantee that is to run
// the same function the client runs.
//
// ABOVE LOD 0 ONE BRANCH CHANGES, and measurement is what forced it. The fine
// rule rejects a cell whose BOTTOM is below the ground (`zMm < groundMm`),
// which is right when the cell is 100 mm: the ground is effectively flat across
// it, so a cell starting below the ground is inside rock. At 1.6 m it is not
// right, and the failure is total rather than gradual. The wet-country water
// measured here is p50 0.75 m deep, so at LOD 4 the whole water column fits
// inside one 1.6 m cell -- and if the ground happens to fall in that cell's
// interior, the cell below is rejected for starting under the ground while the
// cell above starts above the datum. Every cell reads dry. Measured before this
// fix: LOD 4 offered 138 surface bricks at 100 m and meshed **zero quads**. The
// river does not get coarser with distance, it DISAPPEARS.
//
// So above LOD 0 a cell is rejected only when it is ENTIRELY below the ground
// (`zBottomMm + cellMm <= groundMm`); a cell the ground passes through still
// takes its fill from the datum. That is the physically right reading at this
// scale -- the ground is inside the cell, the water sits on top of it -- and it
// is safe to draw because the surface's true height is carried by the 8-bit
// CORNER HEIGHTS (`BuildWaterCornerField` / `EmitWaterQuads`), not by which
// cell the quad lands in. A 1.6 m cell can still put its surface at the right
// millimetre.
uint8_t coarseWaterFill(int64_t zBottomMm, int32_t groundMm, int32_t surfaceMm, int64_t cellMm) {
    if (cellMm == int64_t(kVoxelSizeMm)) {
        return implicitWaterFill(zBottomMm / int64_t(kVoxelSizeMm), groundMm, surfaceMm, false);
    }
    if (surfaceMm == kNoWaterMm) return 0;
    if (zBottomMm + cellMm <= groundMm) return 0; // entirely inside rock
    const int64_t rem = int64_t(surfaceMm) - zBottomMm;
    if (rem <= 0) return 0;
    if (rem >= cellMm) return 255;
    return uint8_t((rem * 255 + cellMm / 2) / cellMm);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: vxc_farwaterprobe <tiledir> [--at Xm Ym] "
                             "[--radii \"100,500,1000\"] [--scan] [--lods N] [--zstd PATH]\n");
        return 2;
    }
    std::string fineDir = argv[1], zstdPath, radiiArg = "100,500,1000", basesArg = "100,128";
    double atXm = 0.0, atYm = 0.0;
    bool haveAt = false, scanOnly = false;
    int lods = 4;
    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--at") && i + 2 < argc) {
            atXm = std::strtod(argv[i + 1], nullptr);
            atYm = std::strtod(argv[i + 2], nullptr);
            haveAt = true;
            i += 2;
        } else if (!std::strcmp(a, "--radii") && i + 1 < argc) {
            radiiArg = argv[++i];
        } else if (!std::strcmp(a, "--bases") && i + 1 < argc) {
            basesArg = argv[++i];
        } else if (!std::strcmp(a, "--lods") && i + 1 < argc) {
            lods = std::atoi(argv[++i]);
        } else if (!std::strcmp(a, "--scan")) {
            scanOnly = true;
        } else if (!std::strcmp(a, "--zstd") && i + 1 < argc) {
            zstdPath = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a);
            return 2;
        }
    }
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
    std::vector<double> bases;
    {
        const char* s2 = basesArg.c_str();
        while (*s2) {
            char* end = nullptr;
            const double v = std::strtod(s2, &end);
            if (end == s2) break;
            bases.push_back(v);
            s2 = end;
            while (*s2 == ',' || *s2 == ' ') ++s2;
        }
    }
    std::sort(radii.begin(), radii.end());
    if (radii.empty()) {
        std::fprintf(stderr, "no radii\n");
        return 2;
    }
    if (lods < 1) lods = 1;

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

    FineTileSampler fine(seed, nullptr);
    fine.setDecompressor(dec);
    int refused = 0;
    std::vector<std::pair<int32_t, int32_t>> loaded;
    for (const auto& f : files) {
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
        std::printf("  tile (%d,%d) bakeVer=%u basins=%s(%zu) waterPlane=%s\n", t->tileX(), t->tileY(),
                    unsigned(t->header().bakeVer), t->hasBasins() ? "yes" : "NO", t->basins().size(),
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
    std::printf("seed %llu  tiles %zu (refused %d)  tileSize %lld px  pixel %lld mm\n",
                (unsigned long long)seed, fine.tileCount(), refused, (long long)tileSize,
                (long long)pxMm);

    LakeSampler lakes(fine);
    RiverSampler rivers(fine);
    CompositeWaterSampler both(lakes, rivers);
    Amplifier amp(seed, fine);

    // ---------------------------------------------------------------------
    // BLOCK CENSUS. Index-only where it can be: a CONSTANT water block owns no
    // data-section entry at all, so "is any of this 480 m square wet" is
    // answered for 72-87% of the plane without fetching a byte. Only the
    // non-constant blocks are decoded and scanned.
    // ---------------------------------------------------------------------
    struct WetBlock {
        int32_t tx, ty;
        uint32_t bx, by;
        uint64_t wet;
        int64_t cxMm, cyMm;
    };
    std::vector<WetBlock> wetBlocks;
    uint64_t blocksTotal = 0, blocksConstDry = 0, blocksConstWet = 0, blocksCoded = 0;
    uint64_t wetPxTotal = 0;
    {
        std::vector<int16_t> depth;
        for (const auto& tc : loaded) {
            const FineTile* t = fine.findTile(tc.first, tc.second);
            if (!t || !t->hasWater()) continue;
            const uint32_t perAxis = t->blocksPerAxis();
            const uint32_t dim = t->blockDim();
            const auto& idx = t->waterIndex();
            for (uint32_t by = 0; by < perAxis; ++by) {
                for (uint32_t bx = 0; bx < perAxis; ++bx) {
                    const size_t i = size_t(by) * perAxis + bx;
                    if (i >= idx.size()) continue;
                    ++blocksTotal;
                    uint64_t wet = 0;
                    if (idx[i].mode == kBlockConstant) {
                        // constCp < 0 is kWaterDryDepth: the whole 256x256 is
                        // dry and costs nothing to reject.
                        if (idx[i].constCp < 0) {
                            ++blocksConstDry;
                            continue;
                        }
                        ++blocksConstWet;
                        wet = uint64_t(dim) * dim;
                    } else {
                        ++blocksCoded;
                        if (!t->decodeWaterBlock(bx, by, depth)) continue;
                        if (depth.size() < size_t(dim) * dim) continue;
                        for (size_t k = 0; k < size_t(dim) * dim; ++k)
                            if (depth[k] >= 0) ++wet;
                        if (wet == 0) continue;
                    }
                    wetPxTotal += wet;
                    const int64_t opx = int64_t(tc.first) * tileSize + int64_t(bx) * dim;
                    const int64_t opy = int64_t(tc.second) * tileSize + int64_t(by) * dim;
                    wetBlocks.push_back(WetBlock{tc.first, tc.second, bx, by, wet,
                                                 (opx + dim / 2) * pxMm, (opy + dim / 2) * pxMm});
                }
            }
        }
    }
    std::sort(wetBlocks.begin(), wetBlocks.end(),
              [](const WetBlock& a, const WetBlock& b) { return a.wet > b.wet; });
    std::printf("\n=== WATER PLANE BLOCK CENSUS (480 m blocks) ===\n");
    std::printf("  blocks total            %llu\n", (unsigned long long)blocksTotal);
    std::printf("  CONSTANT dry            %llu  (%.1f%%, zero data bytes, index-only reject)\n",
                (unsigned long long)blocksConstDry,
                blocksTotal ? 100.0 * double(blocksConstDry) / double(blocksTotal) : 0.0);
    std::printf("  CONSTANT wet            %llu\n", (unsigned long long)blocksConstWet);
    std::printf("  coded/raw (may be wet)  %llu  (%.1f%%)\n", (unsigned long long)blocksCoded,
                blocksTotal ? 100.0 * double(blocksCoded) / double(blocksTotal) : 0.0);
    std::printf("  blocks with any wet px  %zu\n", wetBlocks.size());
    std::printf("  wet fine px total       %llu  (%.4f%% of the loaded plane)\n",
                (unsigned long long)wetPxTotal,
                blocksTotal ? 100.0 * double(wetPxTotal) / (double(blocksTotal) * 256.0 * 256.0) : 0.0);

    if (scanOnly || !haveAt) {
        std::printf("\n=== WETTEST WATER BLOCKS (candidate camera sites) ===\n");
        const size_t n = std::min<size_t>(10, wetBlocks.size());
        for (size_t i = 0; i < n; ++i) {
            const WetBlock& w = wetBlocks[i];
            std::printf("  tile (%d,%d) block (%2u,%2u)  wet px %6llu (%.1f%%)  centre %.1f %.1f m\n",
                        w.tx, w.ty, w.bx, w.by, (unsigned long long)w.wet,
                        100.0 * double(w.wet) / (256.0 * 256.0), double(w.cxMm) / 1000.0,
                        double(w.cyMm) / 1000.0);
        }
        if (scanOnly) return 0;
        if (wetBlocks.empty()) {
            std::fprintf(stderr, "no wet blocks; nothing to measure\n");
            return 1;
        }
        atXm = double(wetBlocks[0].cxMm) / 1000.0;
        atYm = double(wetBlocks[0].cyMm) / 1000.0;
        std::printf("  --at not given; using the wettest block's centre: %.1f %.1f m\n", atXm, atYm);
    }

    const int64_t camXmm = int64_t(atXm * 1000.0), camYmm = int64_t(atYm * 1000.0);
    const int64_t camVx = floorDiv(camXmm, int64_t(kVoxelSizeMm));
    const int64_t camVy = floorDiv(camYmm, int64_t(kVoxelSizeMm));
    const int64_t edge = int64_t(WaterBrick8::kEdge);
    const int64_t brickMm = edge * kVoxelSizeMm; // 800
    const int64_t camBx = floorDiv(camVx, edge), camBy = floorDiv(camVy, edge);
    std::printf("\ncamera at world (%.1f, %.1f) m -> brick column (%lld, %lld)\n", atXm, atYm,
                (long long)camBx, (long long)camBy);

    // ---------------------------------------------------------------------
    // TODAY'S BOX, reproduced, so every number below has its own baseline in
    // the same run rather than a quoted one. Same 65 x 65 x 33 sweep, same
    // ceiling rule, same interior skip -- minus the cavern term, which is not
    // what this tool is about and which the near field gets from a DIFFERENT
    // world anyway (see waterdatumprobe's header).
    // ---------------------------------------------------------------------
    {
        constexpr int32_t kR = 32, kRZ = 16;
        // The camera brick z: put it at the water surface under the camera,
        // which is where a player standing at a river is.
        const int32_t gMm = amp.surfaceMm(camVx, camVy);
        const int32_t dMm = both.waterSurfaceMmAtVoxel(camVx, camVy);
        const int64_t camZmm = dMm == kNoWaterMm ? int64_t(gMm) : int64_t(dMm);
        const int64_t camBz = floorDiv(floorDiv(camZmm, int64_t(kVoxelSizeMm)), edge);
        int64_t admitted = 0, candidates = 0, skippedInterior = 0, meshedNonEmpty = 0, quads = 0;
        // A candidate is not a draw. The sweep offers every brick from the box
        // FLOOR up to the flood level, with no lower bound from the ground, so
        // in shallow water most of what it offers is underground and meshes to
        // nothing (`ImplicitBricksEmpty`). Meshing them here is what makes the
        // comparison against the plane-driven numbers below an honest one --
        // those count only bricks that emit faces.
        std::vector<Quad> raw;
        for (int64_t by = camBy - kR; by <= camBy + kR; ++by) {
            for (int64_t bx = camBx - kR; bx <= camBx + kR; ++bx) {
                const int64_t vx = bx * edge, vy = by * edge;
                const int32_t lakeZMm = both.waterSurfaceMmAtVoxel(vx, vy);
                if (lakeZMm == kNoWaterMm) continue;
                ++admitted;
                const int64_t floodBrickZ = floorDiv(int64_t(lakeZMm) / int64_t(kVoxelSizeMm), edge);
                const int32_t groundMm = amp.surfaceMm(vx, vy);
                for (int64_t bz = camBz - kRZ; bz <= camBz + kRZ; ++bz) {
                    const int64_t padBottomMm = (bz * edge - 1) * kVoxelSizeMm;
                    const int64_t padTopMm = (bz * edge + edge) * kVoxelSizeMm;
                    if (padBottomMm >= groundMm && padTopMm + kVoxelSizeMm <= int64_t(lakeZMm)) {
                        ++skippedInterior;
                        continue;
                    }
                    if (bz > floodBrickZ) continue;
                    ++candidates;
                    raw.clear();
                    const int64_t oz = bz * edge;
                    meshBrick<WaterBrick8::kEdge>(
                        [&](int x, int y, int z) -> MaterialId {
                            const int64_t nvx = vx + (x < 0 ? -1 : (x >= int(edge) ? int64_t(edge) : x));
                            const int64_t nvy = vy + (y < 0 ? -1 : (y >= int(edge) ? int64_t(edge) : y));
                            const int32_t g = amp.surfaceMm(nvx, nvy);
                            const int32_t d = both.waterSurfaceMmAtVoxel(nvx, nvy);
                            const uint8_t fill = implicitWaterFill(oz + z, g, d, false);
                            return fill >= 8 ? MaterialId(1) : MaterialId(MAT_AIR);
                        },
                        raw);
                    if (!raw.empty()) {
                        ++meshedNonEmpty;
                        quads += raw.size();
                    }
                }
            }
        }
        std::printf("\n=== TODAY: the 25.6 m box (65 x 65 x 33 bricks) ===\n");
        std::printf("  columns swept           4225\n");
        std::printf("  columns admitted wet    %lld\n", (long long)admitted);
        std::printf("  proven-interior skipped %lld\n", (long long)skippedInterior);
        std::printf("  CANDIDATE BRICKS        %lld\n", (long long)candidates);
        std::printf("  of those, meshed        %lld  (%lld empty -- offered, meshed, discarded)\n",
                    (long long)meshedNonEmpty, (long long)(candidates - meshedNonEmpty));
        std::printf("  QUADS                   %lld\n", (long long)quads);
    }

    // ---------------------------------------------------------------------
    // PLANE-DRIVEN ENUMERATION at each radius.
    //
    // The grid is over BRICK COLUMNS and is padded by one on every side,
    // because the face test and the mesh both read a one-brick apron. Wet is
    // established from the water plane first (block-major, one decode per
    // block) and from the lake basin extents second; only the columns that
    // survive pay for an amplifier column and a datum.
    // ---------------------------------------------------------------------
    const double maxR = radii.back();
    const int64_t maxRb = int64_t(std::ceil(maxR * 1000.0 / double(brickMm)));
    const int64_t gw = 2 * (maxRb + 1) + 1; // padded by 1
    const int64_t gx0 = camBx - maxRb - 1, gy0 = camBy - maxRb - 1;
    std::printf("\ngrid: %lld x %lld brick columns (%.1f m across, padded by 1 brick)\n",
                (long long)gw, (long long)gw, double(gw * brickMm) / 1000.0);

    std::vector<Col> cols(size_t(gw) * size_t(gw));
    std::vector<uint8_t> wetMask(size_t(gw) * size_t(gw), 0);

    // -- wet from the water plane, block-major -----------------------------
    uint64_t planeWetCols = 0;
    {
        std::vector<int16_t> depth;
        const int64_t bpx0 = floorDiv(gx0 * brickMm, pxMm);
        const int64_t bpx1 = floorDiv((gx0 + gw) * brickMm, pxMm);
        const uint32_t dim0 = 256;
        (void)dim0;
        // Walk the fine-pixel window block by block; mark every brick column
        // whose centre voxel lands on a wet pixel.
        const FineTile* probe = nullptr;
        for (const auto& tc : loaded) {
            probe = fine.findTile(tc.first, tc.second);
            if (probe) break;
        }
        if (probe) {
            const uint32_t dim = probe->blockDim();
            const int64_t gb0x = floorDiv(bpx0, int64_t(dim)), gb1x = floorDiv(bpx1, int64_t(dim));
            const int64_t bpy0 = floorDiv(gy0 * brickMm, pxMm);
            const int64_t bpy1 = floorDiv((gy0 + gw) * brickMm, pxMm);
            const int64_t gb0y = floorDiv(bpy0, int64_t(dim)), gb1y = floorDiv(bpy1, int64_t(dim));
            for (int64_t gby = gb0y; gby <= gb1y; ++gby) {
                for (int64_t gbx = gb0x; gbx <= gb1x; ++gbx) {
                    const int64_t bpx = gbx * int64_t(dim), bpy = gby * int64_t(dim);
                    const int32_t tx = int32_t(floorDiv(bpx, tileSize));
                    const int32_t ty = int32_t(floorDiv(bpy, tileSize));
                    const FineTile* tile = fine.findTile(tx, ty);
                    if (!tile || !tile->hasWater()) continue;
                    const uint32_t lbx = uint32_t((bpx - int64_t(tx) * tileSize) >> tile->blockLog2());
                    const uint32_t lby = uint32_t((bpy - int64_t(ty) * tileSize) >> tile->blockLog2());
                    const size_t bi = size_t(lby) * tile->blocksPerAxis() + lbx;
                    const auto& idx = tile->waterIndex();
                    bool constWet = false;
                    if (bi < idx.size() && idx[bi].mode == kBlockConstant) {
                        if (idx[bi].constCp < 0) continue; // whole block dry, no decode
                        constWet = true;
                    } else {
                        if (!tile->decodeWaterBlock(lbx, lby, depth)) continue;
                        if (depth.size() < size_t(dim) * dim) continue;
                    }
                    // The brick columns whose centres fall in this block.
                    const int64_t mx0 = bpx * pxMm, mx1 = (bpx + int64_t(dim)) * pxMm;
                    const int64_t my0 = bpy * pxMm, my1 = (bpy + int64_t(dim)) * pxMm;
                    int64_t cbx0 = floorDiv(mx0, brickMm) - 1, cbx1 = floorDiv(mx1, brickMm) + 1;
                    int64_t cby0 = floorDiv(my0, brickMm) - 1, cby1 = floorDiv(my1, brickMm) + 1;
                    cbx0 = std::max(cbx0, gx0);
                    cby0 = std::max(cby0, gy0);
                    cbx1 = std::min(cbx1, gx0 + gw - 1);
                    cby1 = std::min(cby1, gy0 + gw - 1);
                    for (int64_t by = cby0; by <= cby1; ++by) {
                        for (int64_t bx = cbx0; bx <= cbx1; ++bx) {
                            const int64_t vx = bx * edge, vy = by * edge;
                            const int64_t px = floorDiv(vx * kVoxelSizeMm, pxMm);
                            const int64_t py = floorDiv(vy * kVoxelSizeMm, pxMm);
                            if (px < bpx || px >= bpx + int64_t(dim)) continue;
                            if (py < bpy || py >= bpy + int64_t(dim)) continue;
                            if (!constWet) {
                                const size_t di =
                                    size_t(py - bpy) * size_t(dim) + size_t(px - bpx);
                                if (depth[di] < 0) continue;
                            }
                            const size_t gi =
                                size_t(by - gy0) * size_t(gw) + size_t(bx - gx0);
                            if (!wetMask[gi]) {
                                wetMask[gi] = 1;
                                ++planeWetCols;
                            }
                        }
                    }
                }
            }
        }
    }

    // -- wet from registered lake basins -----------------------------------
    // The bake writes the water plane DRY inside a registered basin, so this is
    // the other half of what CompositeWaterSampler unions -- and in wet country
    // it is the deep half.
    uint64_t lakeWetCols = 0;
    {
        const int64_t tx0 = floorDiv(floorDiv(gx0 * brickMm, pxMm), tileSize);
        const int64_t tx1 = floorDiv(floorDiv((gx0 + gw) * brickMm, pxMm), tileSize);
        const int64_t ty0 = floorDiv(floorDiv(gy0 * brickMm, pxMm), tileSize);
        const int64_t ty1 = floorDiv(floorDiv((gy0 + gw) * brickMm, pxMm), tileSize);
        for (int64_t ty = ty0; ty <= ty1; ++ty) {
            for (int64_t tx = tx0; tx <= tx1; ++tx) {
                const std::vector<BasinEntry>* rows = lakes.basinsForTile(int32_t(tx), int32_t(ty));
                if (!rows) continue;
                for (const BasinEntry& b : *rows) {
                    if (!b.holdsWater()) continue;
                    const std::vector<uint8_t>* mask =
                        lakes.extentMaskFor(int32_t(tx), int32_t(ty), b.basinId);
                    if (!mask) continue;
                    const int64_t ox = tx * tileSize, oy = ty * tileSize;
                    const int64_t w = int64_t(b.bboxX1) - b.bboxX0 + 1;
                    const int64_t h = int64_t(b.bboxY1) - b.bboxY0 + 1;
                    if (int64_t(mask->size()) < w * h) continue;
                    for (int64_t ly = 0; ly < h; ++ly) {
                        for (int64_t lx = 0; lx < w; ++lx) {
                            if ((*mask)[size_t(ly * w + lx)] == 0) continue;
                            const int64_t px = ox + b.bboxX0 + lx, py = oy + b.bboxY0 + ly;
                            // Every brick column whose centre lands on this pixel.
                            const int64_t bx0 = floorDiv(px * pxMm, brickMm);
                            const int64_t bx1 = floorDiv((px + 1) * pxMm - 1, brickMm);
                            const int64_t by0 = floorDiv(py * pxMm, brickMm);
                            const int64_t by1 = floorDiv((py + 1) * pxMm - 1, brickMm);
                            for (int64_t by = by0; by <= by1; ++by) {
                                if (by < gy0 || by >= gy0 + gw) continue;
                                for (int64_t bx = bx0; bx <= bx1; ++bx) {
                                    if (bx < gx0 || bx >= gx0 + gw) continue;
                                    const size_t gi =
                                        size_t(by - gy0) * size_t(gw) + size_t(bx - gx0);
                                    if (!wetMask[gi]) {
                                        wetMask[gi] = 1;
                                        ++lakeWetCols;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    std::printf("wet brick columns in grid: %llu from the plane, +%llu from lake basins\n",
                (unsigned long long)planeWetCols, (unsigned long long)lakeWetCols);

    // -- resolve ground and datum for the wet columns only -----------------
    //
    // DEPTH IS REPORTED AS A DISTRIBUTION, NOT A MAXIMUM. `datum - amplified
    // ground` is the number that decides how many bricks a column costs, and
    // it is NOT the bake's own depth: the bake measures from ground #2
    // (`reconstructedGroundMm`) and this measures from ground #3, the surface
    // the renderer actually draws. Where the amplifier has cut below the
    // spline inside a lake extent the difference shows up here as a very deep
    // column, so a single max() would read as "there is 200 m of water here"
    // when what it means is "these two grounds disagree by 200 m on one
    // column". The percentiles are what to size against.
    uint64_t resolvedWet = 0;
    std::vector<int32_t> depths;
    for (int64_t by = gy0; by < gy0 + gw; ++by) {
        for (int64_t bx = gx0; bx < gx0 + gw; ++bx) {
            const size_t gi = size_t(by - gy0) * size_t(gw) + size_t(bx - gx0);
            if (!wetMask[gi]) continue;
            const int64_t vx = bx * edge, vy = by * edge;
            Col c;
            c.groundMm = amp.surfaceMm(vx, vy);
            c.datumMm = both.waterSurfaceMmAtVoxel(vx, vy);
            cols[gi] = c;
            if (c.datumMm != kNoWaterMm && int64_t(c.datumMm) > int64_t(c.groundMm)) {
                ++resolvedWet;
                depths.push_back(int32_t(int64_t(c.datumMm) - c.groundMm));
            }
        }
    }
    std::sort(depths.begin(), depths.end());
    auto pct = [&](double p) -> double {
        if (depths.empty()) return 0.0;
        size_t i = size_t(p * double(depths.size() - 1));
        return double(depths[i]) / 1000.0;
    };
    std::printf("columns with datum ABOVE the amplified ground: %llu\n",
                (unsigned long long)resolvedWet);
    std::printf("  depth (datum - amplified ground): p50 %.2f m  p90 %.2f m  p99 %.2f m  max %.2f m\n",
                pct(0.50), pct(0.90), pct(0.99), pct(1.0));

    auto colAt = [&](int64_t bx, int64_t by) -> const Col& {
        static const Col dry;
        if (bx < gx0 || bx >= gx0 + gw || by < gy0 || by >= gy0 + gw) return dry;
        return cols[size_t(by - gy0) * size_t(gw) + size_t(bx - gx0)];
    };

    // ---------------------------------------------------------------------
    // THE MEASUREMENT.
    //
    // `countBand` counts one ANNULUS at one LOD: coarse columns whose centre
    // falls in [rInner, rOuter) fine brick units of the camera. Everything
    // below is built from it -- the whole-disc table passes rInner = 0, the
    // cascade passes one band per level. One counter, so a cascade total and a
    // single-LOD total cannot be computed by two different rules.
    // ---------------------------------------------------------------------
    struct Counts {
        uint64_t wetCols = 0, waterBricks = 0, surfBricks = 0, quads = 0;
    };

    auto countBand = [&](int lod, int64_t rInnerB, int64_t rOuterB) -> Counts {
        Counts out;
        const int64_t step = int64_t(1) << lod;      // fine brick columns per coarse column
        const int64_t cellMm = int64_t(kVoxelSizeMm) << lod;
        const int64_t rIn2 = rInnerB * rInnerB, rOut2 = rOuterB * rOuterB;

        // A coarse column aggregates step x step fine brick columns. Wet iff at
        // least half of them are wet -- the same shape of rule as mips.h's
        // `solidThreshold = 4 of 8`, and for the same reason: a strict-any rule
        // grows water outwards at every LOD step (a 1-px river becomes a 1.6 m
        // wide one at LOD 4) and a strict-all erases every river narrower than
        // the cell. Ground and datum are MEANS over the wet children, which is
        // what keeps the coarse surface at the same height as the fine one
        // instead of at its extreme.
        auto coarseCol = [&](int64_t cx, int64_t cy, Col& out2) -> bool {
            int64_t n = 0, sumG = 0, sumD = 0;
            for (int64_t j = 0; j < step; ++j) {
                for (int64_t i = 0; i < step; ++i) {
                    const Col& c = colAt(cx * step + i, cy * step + j);
                    if (c.datumMm == kNoWaterMm) continue;
                    if (int64_t(c.datumMm) <= int64_t(c.groundMm)) continue;
                    ++n;
                    sumG += c.groundMm;
                    sumD += c.datumMm;
                }
            }
            if (n * 2 < step * step) return false;
            out2.groundMm = int32_t(sumG / n);
            out2.datumMm = int32_t(sumD / n);
            return true;
        };

        const int64_t cRb = (rOuterB + step - 1) / step;
        const int64_t ccamBx = floorDiv(camBx, step), ccamBy = floorDiv(camBy, step);
        const int64_t cw = 2 * cRb + 3, cx0 = ccamBx - cRb - 1, cy0 = ccamBy - cRb - 1;
        // Cached, because the face test and the mesh both read a one-column
        // apron and recomputing a step x step aggregate per read is a factor of
        // nine on the inner loop.
        std::vector<Col> cc(size_t(cw) * size_t(cw));
        for (int64_t cy = cy0; cy < cy0 + cw; ++cy)
            for (int64_t cx = cx0; cx < cx0 + cw; ++cx) {
                Col o;
                if (coarseCol(cx, cy, o)) cc[size_t(cy - cy0) * size_t(cw) + size_t(cx - cx0)] = o;
            }
        auto ccAt = [&](int64_t cx, int64_t cy) -> const Col& {
            static const Col dry;
            if (cx < cx0 || cx >= cx0 + cw || cy < cy0 || cy >= cy0 + cw) return dry;
            return cc[size_t(cy - cy0) * size_t(cw) + size_t(cx - cx0)];
        };

        std::vector<Quad> raw;
        for (int64_t cy = ccamBy - cRb; cy <= ccamBy + cRb; ++cy) {
            for (int64_t cx = ccamBx - cRb; cx <= ccamBx + cRb; ++cx) {
                // The distance test is in FINE brick units at every LOD, so a
                // band boundary is the same circle on the ground whichever
                // level is measuring it.
                const int64_t dx = cx * step - camBx, dy = cy * step - camBy;
                const int64_t d2 = dx * dx + dy * dy;
                if (d2 < rIn2 || d2 >= rOut2) continue;
                const Col& c = ccAt(cx, cy);
                if (c.datumMm == kNoWaterMm) continue;
                ++out.wetCols;
                const int64_t gz = floorDiv(int64_t(c.groundMm), cellMm);
                const int64_t dz = floorDiv(int64_t(c.datumMm) - 1, cellMm);
                const int64_t bz0 = floorDiv(gz, edge), bz1 = floorDiv(dz, edge);
                for (int64_t bz = bz0; bz <= bz1; ++bz) {
                    ++out.waterBricks;
                    // Interior test, the sweep's own proof generalised to the
                    // apron: a brick emits no face iff every padded cell is
                    // full water, i.e. every one of the nine columns it can
                    // read has ground at or below the pad bottom and datum a
                    // full cell above the pad top.
                    const int64_t padBottomMm = (bz * edge - 1) * cellMm;
                    const int64_t padTopMm = (bz * edge + edge) * cellMm;
                    bool interior = true;
                    for (int64_t j = -1; j <= 1 && interior; ++j) {
                        for (int64_t i = -1; i <= 1 && interior; ++i) {
                            const Col& n = ccAt(cx + i, cy + j);
                            if (n.datumMm == kNoWaterMm) {
                                interior = false;
                            } else if (!(int64_t(n.groundMm) <= padBottomMm &&
                                         padTopMm + cellMm <= int64_t(n.datumMm))) {
                                interior = false;
                            }
                        }
                    }
                    if (interior) continue;
                    ++out.surfBricks;
                    // Meshed for real, not estimated.
                    raw.clear();
                    const int64_t oz = bz * edge;
                    meshBrick<WaterBrick8::kEdge>(
                        [&](int x, int y, int z) -> MaterialId {
                            const Col& n = ccAt(cx + (x < 0 ? -1 : (x >= int(edge) ? 1 : 0)),
                                                cy + (y < 0 ? -1 : (y >= int(edge) ? 1 : 0)));
                            const uint8_t fill =
                                coarseWaterFill((oz + z) * cellMm, n.groundMm, n.datumMm, cellMm);
                            return fill >= 8 ? MaterialId(1) : MaterialId(MAT_AIR);
                        },
                        raw);
                    out.quads += raw.size();
                }
            }
        }
        return out;
    };

    // 4 verts x 32 B + 6 indices x 4 B is the shape the water chunk component
    // uploads; quoted as a scale, not a promise.
    auto megabytes = [](uint64_t quads) { return double(quads) * 152.0 / (1024.0 * 1024.0); };

    std::printf("\n=== PLANE-DRIVEN CANDIDATES, WHOLE DISC AT ONE LOD ===\n");
    std::printf("(no box, no vertical bound: the z range is the water column itself)\n");
    std::printf("%6s %5s %8s %10s %12s %12s %12s %10s\n", "radius", "lod", "voxel", "wet cols",
                "water brk", "surf brk", "quads", "MB");
    for (double R : radii) {
        const int64_t Rb = int64_t(std::ceil(R * 1000.0 / double(brickMm)));
        for (int lod = 0; lod < lods; ++lod) {
            const Counts c = countBand(lod, 0, Rb);
            std::printf("%5.0fm %5d %7.2fm %10llu %12llu %12llu %12llu %10.1f\n", R, lod,
                        double(int64_t(kVoxelSizeMm) << lod) / 1000.0, (unsigned long long)c.wetCols,
                        (unsigned long long)c.waterBricks, (unsigned long long)c.surfBricks,
                        (unsigned long long)c.quads, megabytes(c.quads));
        }
    }

    // ---------------------------------------------------------------------
    // THE CASCADE. LOD L covers [base << (L-1), base << L), i.e. a
    // distance-doubling ring set, which is what the terrain rings and the
    // clipmap already do (`AVoxelClipmapActor::SpacingUUForLevel` is
    // `Spacing0UU << LevelIndex`).
    //
    // `base` is chosen from SCREEN SIZE, not from cost: at 90 deg horizontal
    // FOV on a 1920 px frame one pixel subtends 8.18e-4 rad, so a 0.1 m voxel
    // is one pixel at 122 m, and each LOD doubling holds that same apparent
    // size for twice the distance. A cascade sized this way is not a quality
    // compromise at all beyond its first ring -- the coarser voxel is smaller
    // on screen than the pixel that would draw it.
    // ---------------------------------------------------------------------
    std::printf("\n=== DISTANCE CASCADE (LOD L over [base<<(L-1), base<<L)) ===\n");
    for (double baseM : bases) {
        std::printf("\nbase %.0f m -> rings", baseM);
        for (int l = 0; l < lods; ++l)
            std::printf(" %.0f", baseM * double(int64_t(1) << l));
        std::printf(" m\n");
        std::printf("%6s %8s %10s %10s %12s %12s %12s\n", "lod", "voxel", "from", "to", "surf brk",
                    "quads", "MB");
        Counts total;
        for (int lod = 0; lod < lods; ++lod) {
            const double rIn = lod == 0 ? 0.0 : baseM * double(int64_t(1) << (lod - 1));
            const double rOut = baseM * double(int64_t(1) << lod);
            const int64_t rInB = int64_t(rIn * 1000.0 / double(brickMm));
            const int64_t rOutB = int64_t(std::ceil(rOut * 1000.0 / double(brickMm)));
            const Counts c = countBand(lod, rInB, rOutB);
            total.wetCols += c.wetCols;
            total.waterBricks += c.waterBricks;
            total.surfBricks += c.surfBricks;
            total.quads += c.quads;
            std::printf("%6d %7.2fm %9.0fm %9.0fm %12llu %12llu %12.1f\n", lod,
                        double(int64_t(kVoxelSizeMm) << lod) / 1000.0, rIn, rOut,
                        (unsigned long long)c.surfBricks, (unsigned long long)c.quads,
                        megabytes(c.quads));
        }
        std::printf("%6s %7s  %8s %9.0fm %12llu %12llu %12.1f\n", "TOTAL", "", "0m",
                    baseM * double(int64_t(1) << (lods - 1)), (unsigned long long)total.surfBricks,
                    (unsigned long long)total.quads, megabytes(total.quads));
    }

    std::printf("\nNOTE: quads are pre-split. EmitWaterQuads splits some again for the\n"
                "corner field, so these are a floor. `surf brk` is the number that costs a\n"
                "mesh and a draw; `water brk` is what a naive offer-everything-wet rule costs.\n");
    return 0;
}
