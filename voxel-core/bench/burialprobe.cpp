// vxc_burialprobe -- is the baked river BURIED inside the rendered terrain?
//
// THE ONE QUESTION. The bake stores water as a DEPTH ABOVE THE RECONSTRUCTED
// SURFACE (tile_codec.py's `water_depth_control_points` differences two
// per-pixel rasters of the smooth spline carrier). The client adds it back to
// the same datum -- `reconstructedGroundMm` -> `FineTile::waterMmFromDepth`,
// which is what `RiverSampler` does and is correct. But the terrain that is
// actually DRAWN is the AMPLIFIED surface: that same spline plus rills,
// bedding, the v16 horizontal warp and the coarse shaping octaves. Nothing in
// the bake ever saw those terms, so nothing guarantees the amplifier did not
// fill the channel back in.
//
// If it did, the symptom is exactly the observed one: `RiverSampler` returns a
// perfectly good water surface for thousands of columns, the water subsystem
// meshes bricks for them, and the player sees nothing, because
// `implicitWaterFill` refuses every voxel whose bottom is below the amplified
// ground and the datum sits under it.
//
// -- THE THREE GROUNDS, NAMED, BECAUSE CONFLATING THEM IS THE BUG CLASS ------
//
//   LATTICE      `FineTileSampler::elevationMm(px, py)`. A prefiltered B-spline
//                CONTROL POINT. Not a surface, not used here at all except
//                inside the spline evaluation below.
//   SPLINE       `reconstructedGroundMm(tiles, px, py)` -- tilestore.h's
//                production call, `evalCarrier` on the tier's own 4x4 stencil
//                at fx = fy = 0. THIS IS THE BAKE'S DATUM. Every
//                `water_surface_mm` in this file is SPLINE + depth.
//   AMPLIFIED    `Amplifier::surfaceMm(vx, vy)` -- documented bit-identical to
//                `column(vx, vy).surfaceMm`, which is what
//                `UVoxelWorldSubsystem::GetSurfaceHeightUU` returns and what
//                `VoxelWaterSubsystem.cpp`'s ImplicitFn passes to
//                `implicitWaterFill` as `groundMm`. THIS IS WHAT IS DRAWN.
//                Every `rendered_ground_mm` in this file is this.
//
// The probe never mixes them: it reports SPLINE + depth against AMPLIFIED, and
// separately reports AMPLIFIED - SPLINE, which is "everything worldgen adds
// inside the channel" and nothing else.
//
// -- THE COMPARISON IS AT VOXEL GRANULARITY, THE MESHER'S OWN --------------
//
// Rounding the wrong way here would fake the answer, so the test is not
// `water > ground` in millimetres. It is the shipped predicate, transcribed
// from voxelcore/lakes.h:
//
//     implicitWaterFill(vz, groundMm, waterSurfaceMm, false):
//         zMm = vz * kVoxelSizeMm            // the voxel BOTTOM, not its centre
//         if (zMm < groundMm) return 0;      // below the drawn ground: not open air
//         return waterFillUnits(zMm, waterSurfaceMm);   // > 0 iff surface > zMm
//
// so the LOWEST voxel that can ever carry water in a column is
//
//     vzLo = ceil(renderedGroundMm / 100)
//
// and that voxel is non-empty iff `waterSurfaceMm > vzLo * 100`. A column is
// therefore DRAWN-DRY -- water present in the bake, no water voxel emitted --
// iff
//
//     waterSurfaceMm <= 100 * ceil(renderedGroundMm / 100)
//
// which is the headline. Note this is STRICTLY HARSHER than the millimetre
// test `renderedGroundMm >= waterSurfaceMm`: a column can have water above the
// drawn ground in millimetres and still emit nothing, because the water and
// the ground land in the same 100 mm voxel. Both are reported, and the gap
// between them is the SAME-VOXEL BAND, counted separately.
//
// (The top SOLID voxel uses a different rule -- `Amplifier::stratigraphyAt`
// tests the voxel CENTRE, `vz*100+50 <= surfaceMm` -- so it is reported too,
// but it is not the burial test: the water fill is what decides whether a
// water brick has anything in it, and it is the bottom-face rule.)
//
// -- WHY EACH PIXEL IS SAMPLED AS A PATCH, NOT A POINT ----------------------
//
// A fine pixel is 1875 mm; a voxel is 100 mm. One pixel is 18.75 voxels
// across, so a 1-3 px river is 19-56 voxels wide and "too narrow to see" is
// not available as an explanation. It also means the water datum is piecewise
// constant over a patch of ~350 voxel columns while the AMPLIFIED ground
// varies freely inside it -- so a pixel is not buried or visible, it is buried
// over some FRACTION of its columns. The probe therefore evaluates an N x N
// grid of voxel columns per wet pixel (--sub, default 5) and reports both the
// centre column (the headline, unambiguous) and the per-pixel visible-column
// fraction (which is what decides whether a reach reads as a channel or as
// isolated blocks).
//
// -- THE CONTROL ------------------------------------------------------------
//
// "The amplifier adds 900 mm of relief inside the channel" means nothing
// without knowing what it adds everywhere else. So the same AMPLIFIED - SPLINE
// distribution is taken over DRY pixels near the channel: every dry pixel whose
// Chebyshev distance to the nearest wet pixel is in [--ctrl-near, --ctrl-far]
// (default 4..16 px = 7.5..30 m), found by a BFS over the tile's own wet mask.
// Near enough to be the same terrain, far enough not to be the channel.
//
// -- WHAT IS EXCLUDED, AND SAID OUT LOUD ------------------------------------
//
// The spline needs a 4x4 lattice stencil and the amplifier needs a wider
// footprint still (the v16 warp displaces horizontally). At the edge of the
// loaded tile set both read missing tiles, which `FineTileSampler` answers with
// elevation 0 rather than throwing. A pixel whose evaluation bumps
// `missingTileQueries` is DROPPED and counted, so an edge artifact can never be
// silently folded into the headline.
//
// -- THE --cam MODE ---------------------------------------------------------
//
// If the burial test comes back NEGATIVE the next question is where the water
// the client DID mesh actually went, so `--cam X,Y,Z` (camera position in UU =
// centimetres, straight off the capture log's `cam (...) UU`) replays
// `RefreshImplicitWater`'s candidate sweep at bench scale: the same brick
// centre, the same kImplicitRadiusBricks / kImplicitRadiusBricksZ box, the same
// `waterSurfaceMmAtVoxel` ceiling reject, and the same `implicitWaterFill` per
// voxel. It reports how many bricks hold water and, crucially, WHERE they are
// relative to the camera and to the ground -- which is the difference between
// "the water is buried" and "the water is drawn, in a box you are standing on
// top of".
//
// Usage:
//   vxc_burialprobe --fine-dir DIR [--tiles x,y[;x,y...]] [--sub N]
//                   [--ctrl-near N] [--ctrl-far N] [--zstd PATH]
//                   [--cam X,Y,Z]        (UU = cm, as the capture log prints)

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/carrier.h"
#include "voxelcore/core.h"
#include "voxelcore/lakes.h"
#include "voxelcore/tilestore.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
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

// --- integer stats ----------------------------------------------------------

int64_t pct(std::vector<int64_t>& v, int p) {
    if (v.empty()) return 0;
    size_t i = (v.size() * static_cast<size_t>(p)) / 100;
    if (i >= v.size()) i = v.size() - 1;
    return v[i];
}

void reportDist(const char* label, std::vector<int64_t> v) {
    if (v.empty()) {
        std::printf("  %-28s (no samples)\n", label);
        return;
    }
    std::sort(v.begin(), v.end());
    double mean = 0.0;
    for (int64_t x : v) mean += static_cast<double>(x);
    mean /= static_cast<double>(v.size());
    std::printf("  %-28s n=%-8zu min=%-8" PRId64 " p10=%-8" PRId64 " p50=%-8" PRId64
                " p90=%-8" PRId64 " max=%-8" PRId64 " mean=%.0f\n",
                label, v.size(), v.front(), pct(v, 10), pct(v, 50), pct(v, 90), v.back(), mean);
}

// --- the burial predicate, transcribed from voxelcore/lakes.h --------------
//
// The lowest voxel `implicitWaterFill` can ever fill in a column whose drawn
// ground is `groundMm`: the first vz with vz*kVoxelSizeMm >= groundMm.
int64_t lowestFillableVz(int64_t groundMm) {
    // ceilDiv for possibly-negative groundMm, via floorDiv.
    return -floorDiv(-groundMm, kVoxelSizeMm);
}

// True iff the shipped predicate emits NO water voxel anywhere in this column.
// Uses implicitWaterFill itself rather than a restatement of it, so this probe
// cannot express the rule differently from the client.
bool drawnDry(int64_t groundMm, int32_t waterSurfaceMm) {
    if (waterSurfaceMm == kNoWaterMm) return true;
    const int64_t vzLo = lowestFillableVz(groundMm);
    return implicitWaterFill(vzLo, static_cast<int32_t>(groundMm), waterSurfaceMm, false) == 0;
}

// The number of voxels the shipped predicate WOULD fill in this column (the
// partial top voxel counts as one). Zero iff drawnDry.
int64_t filledVoxelCount(int64_t groundMm, int32_t waterSurfaceMm) {
    if (waterSurfaceMm == kNoWaterMm) return 0;
    const int64_t vzLo = lowestFillableVz(groundMm);
    // Highest vz with fill > 0 is the last one whose BOTTOM is under the
    // surface: vz*100 < surface  =>  vz <= ceil(surface/100) - 1.
    const int64_t vzHi = -floorDiv(-static_cast<int64_t>(waterSurfaceMm), kVoxelSizeMm) - 1;
    return vzHi >= vzLo ? (vzHi - vzLo + 1) : 0;
}

// Top SOLID voxel: Amplifier::stratigraphyAt is a centre test,
// vz*100 + 50 <= surfaceMm. Reported for context; NOT the burial test.
int64_t topSolidVz(int64_t surfaceMm) {
    return floorDiv(surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
}

struct TileStats {
    int32_t tx = 0, ty = 0;
    int64_t wet = 0;          // wet pixels seen in the water plane
    int64_t evaluated = 0;    // ...of those, with a complete footprint
    int64_t dropped = 0;      // ...dropped: stencil/amplifier read a missing tile
    int64_t buriedMm = 0;     // rendered_ground_mm >= water_surface_mm
    int64_t buriedVoxel = 0;  // the shipped predicate emits no voxel at all
    int64_t sameVoxelBand = 0;// water above ground in mm, still no voxel emitted
    std::vector<int64_t> reliefWet;    // AMPLIFIED - SPLINE at wet pixels
    std::vector<int64_t> reliefDry;    // ...at the dry control
    std::vector<int64_t> depthMm;      // the bake's own depth, for cross-check
    std::vector<int64_t> headroomMm;   // water_surface_mm - rendered_ground_mm
    std::vector<int64_t> voxelsFilled; // per centre column
    std::vector<int64_t> visiblePctPerPixel; // % of sub-sampled columns not drawn-dry
    int64_t pixelsFullyInvisible = 0;  // 0% of sub-columns visible
    int64_t pixelsFullyVisible = 0;    // 100%
    int64_t subColumns = 0, subVisible = 0;
};

// Chebyshev distance to the nearest wet pixel, capped at `cap`, by BFS over
// the tile's own wet mask. `cap + 1` means "further than cap".
std::vector<uint8_t> wetDistance(const std::vector<uint8_t>& wet, int size, int cap) {
    std::vector<uint8_t> d(wet.size(), static_cast<uint8_t>(cap + 1));
    std::deque<int32_t> q;
    for (size_t i = 0; i < wet.size(); ++i) {
        if (wet[i]) {
            d[i] = 0;
            q.push_back(static_cast<int32_t>(i));
        }
    }
    while (!q.empty()) {
        const int32_t i = q.front();
        q.pop_front();
        const int nd = d[static_cast<size_t>(i)] + 1;
        if (nd > cap) continue;
        const int x = i % size, y = i / size;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const int nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= size || ny >= size) continue;
                const size_t j = static_cast<size_t>(ny) * static_cast<size_t>(size) +
                                 static_cast<size_t>(nx);
                if (d[j] > nd) {
                    d[j] = static_cast<uint8_t>(nd);
                    q.push_back(static_cast<int32_t>(j));
                }
            }
        }
    }
    return d;
}

} // namespace

// --- the --cam replay -------------------------------------------------------
//
// Transcribed from VoxelWaterSubsystem.cpp's RefreshImplicitWater. The
// constants are ITS constants and are duplicated here rather than included
// because voxel-core cannot see the UE module; if they change there, this
// prints a stale box and says so by disagreeing with the log's candidate count,
// which is why the candidate count is printed alongside.
constexpr int32_t kImplicitRadiusBricks = 32;  // xy, in 8-voxel bricks
constexpr int32_t kImplicitRadiusBricksZ = 16; // z
constexpr int64_t kBrickEdge = 8;              // vxc::WaterBrick8::kEdge

void replayDisc(FineTileSampler& fine, Amplifier& amp, int64_t camXcm, int64_t camYcm,
                int64_t camZcm) {
    RiverSampler rivers(fine);
    // UU is centimetres and a voxel is 100 mm = 10 cm, so voxel = floor(cm/10).
    const int64_t camVx = floorDiv(camXcm, 10);
    const int64_t camVy = floorDiv(camYcm, 10);
    const int64_t camVz = floorDiv(camZcm, 10);
    const int64_t cbx = floorDiv(camVx, kBrickEdge);
    const int64_t cby = floorDiv(camVy, kBrickEdge);
    const int64_t cbz = floorDiv(camVz, kBrickEdge);

    const int32_t camGroundMm = amp.surfaceMm(camVx, camVy);
    const int32_t camWaterMm = rivers.waterSurfaceMmAtVoxel(camVx, camVy);

    std::printf("\n=== --cam replay of RefreshImplicitWater's disc ===\n");
    std::printf("  camera            : (%" PRId64 ", %" PRId64 ", %" PRId64 ") UU"
                "  = voxel (%" PRId64 ", %" PRId64 ", %" PRId64 ")  = brick (%" PRId64
                ", %" PRId64 ", %" PRId64 ")\n",
                camXcm, camYcm, camZcm, camVx, camVy, camVz, cbx, cby, cbz);
    std::printf("  under the camera  : AMPLIFIED ground %.2f m, water surface %s, camera "
                "%.2f m above ground\n",
                camGroundMm / 1000.0,
                camWaterMm == kNoWaterMm ? "DRY"
                                         : (std::to_string(camWaterMm / 1000.0) + " m").c_str(),
                (camZcm * 10 - camGroundMm) / 1000.0);
    std::printf("  disc box (the ONLY place a water voxel can be drawn near-field): "
                "+/-%.1f m in xy, +/-%.1f m in z about the camera BRICK\n",
                (kImplicitRadiusBricks * kBrickEdge * kVoxelSizeMm) / 1000.0,
                (kImplicitRadiusBricksZ * kBrickEdge * kVoxelSizeMm) / 1000.0);

    int64_t wetColumns = 0, candidates = 0, bricksWithWater = 0, surfaceBricks = 0;
    int64_t filledVoxels = 0;
    int64_t minZmm = INT64_MAX, maxZmm = INT64_MIN;
    int64_t minDx = INT64_MAX, maxDx = INT64_MIN, minDy = INT64_MAX, maxDy = INT64_MIN;
    std::vector<int64_t> waterAboveGroundMm; // per wet brick column, water - ground

    for (int32_t by = -kImplicitRadiusBricks; by <= kImplicitRadiusBricks; ++by) {
        for (int32_t bx = -kImplicitRadiusBricks; bx <= kImplicitRadiusBricks; ++bx) {
            const int64_t vx = (cbx + bx) * kBrickEdge;
            const int64_t vy = (cby + by) * kBrickEdge;
            const int32_t lakeZMm = rivers.waterSurfaceMmAtVoxel(vx, vy);
            if (lakeZMm == kNoWaterMm) continue; // (no cavern arm here: bench has no site table)
            ++wetColumns;
            const int64_t floodBrickZ = floorDiv(int64_t(lakeZMm) / kVoxelSizeMm, kBrickEdge);
            for (int32_t bz = -kImplicitRadiusBricksZ; bz <= kImplicitRadiusBricksZ; ++bz) {
                const int64_t bzAbs = cbz + bz;
                if (bzAbs > floodBrickZ) continue;
                ++candidates;
                int64_t filled = 0, empty = 0;
                for (int64_t oy = 0; oy < kBrickEdge; ++oy) {
                    for (int64_t ox = 0; ox < kBrickEdge; ++ox) {
                        const int32_t g = amp.surfaceMm(vx + ox, vy + oy);
                        const int32_t w = rivers.waterSurfaceMmAtVoxel(vx + ox, vy + oy);
                        for (int64_t oz = 0; oz < kBrickEdge; ++oz) {
                            const int64_t vz = bzAbs * kBrickEdge + oz;
                            if (implicitWaterFill(vz, g, w, false) > 0) {
                                ++filled;
                                const int64_t zmm = vz * kVoxelSizeMm;
                                if (zmm < minZmm) minZmm = zmm;
                                if (zmm > maxZmm) maxZmm = zmm;
                            } else {
                                ++empty;
                            }
                        }
                    }
                }
                if (filled > 0) {
                    ++bricksWithWater;
                    filledVoxels += filled;
                    if (empty > 0) ++surfaceBricks; // a brick that can emit faces
                    const int64_t dx = (vx - camVx) * kVoxelSizeMm;
                    const int64_t dy = (vy - camVy) * kVoxelSizeMm;
                    if (dx < minDx) minDx = dx;
                    if (dx > maxDx) maxDx = dx;
                    if (dy < minDy) minDy = dy;
                    if (dy > maxDy) maxDy = dy;
                }
            }
            const int32_t g = amp.surfaceMm(vx, vy);
            waterAboveGroundMm.push_back(int64_t(lakeZMm) - int64_t(g));
        }
    }

    std::printf("  wet brick columns in the disc: %" PRId64 " of %d;  candidate bricks: %" PRId64
                "\n",
                wetColumns, (2 * kImplicitRadiusBricks + 1) * (2 * kImplicitRadiusBricks + 1),
                candidates);
    std::printf("  bricks holding >=1 water voxel: %" PRId64 "  (of which %" PRId64
                " also hold air, i.e. can emit faces);  water voxels: %" PRId64 "\n",
                bricksWithWater, surfaceBricks, filledVoxels);
    if (bricksWithWater > 0) {
        std::printf("  WHERE THEY ARE, relative to the camera:  x %+.1f .. %+.1f m,"
                    "  y %+.1f .. %+.1f m,  z %+.2f .. %+.2f m\n",
                    minDx / 1000.0, maxDx / 1000.0, minDy / 1000.0, maxDy / 1000.0,
                    (minZmm - camZcm * 10) / 1000.0, (maxZmm - camZcm * 10) / 1000.0);
        std::printf("  absolute z of the water slab: %.2f .. %.2f m "
                    "(AMPLIFIED ground under the camera: %.2f m)\n",
                    minZmm / 1000.0, maxZmm / 1000.0, camGroundMm / 1000.0);
    }
    reportDist("water - AMPLIFIED @ wet cols", waterAboveGroundMm);
}

int main(int argc, char** argv) {
    std::string fineDir, zstdPath, tileSel, camSel;
    int sub = 5, ctrlNear = 4, ctrlFar = 16;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(a, "--fine-dir")) fineDir = next(a);
        else if (!std::strcmp(a, "--zstd")) zstdPath = next(a);
        else if (!std::strcmp(a, "--tiles")) tileSel = next(a);
        else if (!std::strcmp(a, "--cam")) camSel = next(a);
        else if (!std::strcmp(a, "--sub")) sub = std::atoi(next(a));
        else if (!std::strcmp(a, "--ctrl-near")) ctrlNear = std::atoi(next(a));
        else if (!std::strcmp(a, "--ctrl-far")) ctrlFar = std::atoi(next(a));
        else {
            std::fprintf(stderr, "unknown option %s\n", a);
            return 2;
        }
    }
    if (fineDir.empty()) {
        std::fprintf(stderr, "usage: vxc_burialprobe --fine-dir DIR [--tiles x,y;x,y] "
                             "[--sub N] [--ctrl-near N] [--ctrl-far N]\n");
        return 2;
    }
    if (sub < 1) sub = 1;
    if (ctrlFar < ctrlNear) ctrlFar = ctrlNear;
    if (ctrlFar > 200) ctrlFar = 200;

    // Which tiles to MEASURE. Every tile in the directory is still LOADED --
    // the spline stencil and the amplifier footprint reach across tile edges,
    // and a neighbour that is present is one fewer dropped pixel.
    std::vector<std::pair<int32_t, int32_t>> want;
    if (!tileSel.empty()) {
        const char* s = tileSel.c_str();
        while (*s) {
            int tx = 0, ty = 0;
            if (std::sscanf(s, "%d,%d", &tx, &ty) != 2) {
                std::fprintf(stderr, "--tiles wants x,y[;x,y...]\n");
                return 2;
            }
            want.emplace_back(tx, ty);
            while (*s && *s != ';') ++s;
            if (*s == ';') ++s;
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
    // seeded, so a wrong seed produces plausible detail over the right terrain
    // and no statistic here could tell.
    uint64_t seed = 0;
    {
        FineError err = FineError::kNone;
        auto bytes = readFileBytes(files.front());
        if (!bytes) {
            std::fprintf(stderr, "cannot read %s\n", files.front().string().c_str());
            return 1;
        }
        auto t = FineTile::parse(std::move(*bytes), dec, &err);
        if (!t) {
            std::fprintf(stderr, "cannot parse %s (%s)\n", files.front().string().c_str(),
                         fineErrorName(err));
            return 1;
        }
        seed = t->seed();
    }
    std::printf("seed: %" PRIu64 " (read from the tiles)\n", seed);

    FineTileSampler fine(seed, nullptr);
    fine.setDecompressor(dec);

    int loaded = 0, refused = 0, withWater = 0;
    std::vector<std::pair<int32_t, int32_t>> coords;
    for (const auto& f : files) {
        FineError err = FineError::kNone;
        auto bytes = readFileBytes(f);
        if (!bytes) {
            std::fprintf(stderr, "REFUSED %s (unreadable)\n", f.string().c_str());
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
            std::fprintf(stderr, "REJECTED %s by the sampler\n", f.string().c_str());
            ++refused;
            continue;
        }
        if (water) ++withWater;
        coords.emplace_back(tx, ty);
        ++loaded;
    }
    std::printf("tiles: loaded=%d (with a water plane: %d) refused=%d  tileSize=%u px  "
                "pixel=%d mm  voxel=%d mm\n",
                loaded, withWater, refused, fine.tileSize(), fine.pixelSizeMm(), kVoxelSizeMm);
    if (loaded == 0 || refused > 0) {
        std::fprintf(stderr, "nothing loaded, or a tile was refused -- refusing to report\n");
        return 1;
    }

    if (want.empty()) want = coords;

    Amplifier amp(seed, fine);
    const int64_t pxMm = fine.pixelSizeMm();
    const int size = static_cast<int>(fine.tileSize());

    std::printf("\nGROUNDS: water_surface = SPLINE(reconstructedGroundMm) + depth;  "
                "rendered = AMPLIFIED(Amplifier::surfaceMm)\n");
    std::printf("BURIAL RULE (voxelcore/lakes.h implicitWaterFill, voxel BOTTOM face): "
                "drawn-dry iff water_surface <= %d * ceil(rendered / %d)\n",
                kVoxelSizeMm, kVoxelSizeMm);
    std::printf("sub-sampling: %dx%d voxel columns per %" PRId64 " mm pixel; "
                "dry control at Chebyshev %d..%d px from wet\n",
                sub, sub, pxMm, ctrlNear, ctrlFar);

    if (!camSel.empty()) {
        long long cx = 0, cy = 0, cz = 0;
        if (std::sscanf(camSel.c_str(), "%lld,%lld,%lld", &cx, &cy, &cz) != 3) {
            std::fprintf(stderr, "--cam wants X,Y,Z in UU (centimetres)\n");
            return 2;
        }
        replayDisc(fine, amp, cx, cy, cz);
        std::printf("\nsampler counters: missingTileQueries=%" PRIu64
                    " blockDecodeFailures=%" PRIu64 "\n",
                    fine.missingTileQueries.load(), fine.blockDecodeFailures.load());
        return 0;
    }

    std::vector<TileStats> all;
    TileStats pooled;
    pooled.tx = 0;
    pooled.ty = 0;

    for (const auto& tc : want) {
        const FineTile* tile = fine.findTile(tc.first, tc.second);
        if (!tile) {
            std::fprintf(stderr, "tile %d,%d not loaded -- skipped\n", tc.first, tc.second);
            continue;
        }
        TileStats st;
        st.tx = tc.first;
        st.ty = tc.second;

        const int64_t basePx = static_cast<int64_t>(tc.first) * size;
        const int64_t basePy = static_cast<int64_t>(tc.second) * size;

        // Decode the whole water plane into a wet mask + depth raster first;
        // the BFS control needs the mask before any per-pixel work.
        std::vector<uint8_t> wet(static_cast<size_t>(size) * static_cast<size_t>(size), 0);
        std::vector<int16_t> depthCp(wet.size(), kWaterDryDepth);
        if (tile->hasWater()) {
            const uint32_t per = tile->blocksPerAxis();
            const uint32_t dim = tile->blockDim();
            std::vector<int16_t> blk;
            for (uint32_t by = 0; by < per; ++by) {
                for (uint32_t bx = 0; bx < per; ++bx) {
                    if (!tile->decodeWaterBlock(bx, by, blk)) continue;
                    for (uint32_t ly = 0; ly < dim; ++ly) {
                        for (uint32_t lx = 0; lx < dim; ++lx) {
                            const int16_t d = blk[static_cast<size_t>(ly) * dim + lx];
                            const size_t o = static_cast<size_t>(by * dim + ly) *
                                                 static_cast<size_t>(size) +
                                             static_cast<size_t>(bx * dim + lx);
                            depthCp[o] = d;
                            if (d >= 0) {
                                wet[o] = 1;
                                ++st.wet;
                            }
                        }
                    }
                }
            }
        }
        if (st.wet == 0) {
            std::printf("\ntile %d,%d: no wet pixels\n", st.tx, st.ty);
            all.push_back(std::move(st));
            continue;
        }

        const std::vector<uint8_t> dist = wetDistance(wet, size, ctrlFar);

        // Prewarm the elevation blocks over the tile plus a one-pixel skirt so
        // the per-pixel missingTileQueries delta reports only genuinely absent
        // TILES, not a cold cache.
        fine.prewarm(basePx - 2, basePy - 2, basePx + size + 1, basePy + size + 1);

        for (int ly = 0; ly < size; ++ly) {
            for (int lx = 0; lx < size; ++lx) {
                const size_t o = static_cast<size_t>(ly) * static_cast<size_t>(size) +
                                 static_cast<size_t>(lx);
                const bool isWet = wet[o] != 0;
                const bool isCtrl = !isWet && dist[o] >= ctrlNear && dist[o] <= ctrlFar;
                if (!isWet && !isCtrl) continue;

                const int64_t px = basePx + lx, py = basePy + ly;
                const uint64_t missBefore = fine.missingTileQueries.load();

                // SPLINE. tilestore.h's production reconstruction, fx = fy = 0,
                // which is the datum the bake differenced against.
                const int32_t splineMm = reconstructedGroundMm(fine, px, py);

                // The pixel's own voxel-column patch. The pixel spans world x
                // in [px*pxMm, (px+1)*pxMm); the centre column is the voxel
                // containing px*pxMm + pxMm/2.
                const int64_t x0Mm = px * pxMm, y0Mm = py * pxMm;
                const int64_t cxVx = floorDiv(x0Mm + pxMm / 2, kVoxelSizeMm);
                const int64_t cyVx = floorDiv(y0Mm + pxMm / 2, kVoxelSizeMm);
                const int32_t centreAmpMm = amp.surfaceMm(cxVx, cyVx);

                if (fine.missingTileQueries.load() != missBefore) {
                    if (isWet) ++st.dropped;
                    continue;
                }

                const int64_t relief = static_cast<int64_t>(centreAmpMm) -
                                       static_cast<int64_t>(splineMm);
                if (isCtrl) {
                    st.reliefDry.push_back(relief);
                    continue;
                }

                const int16_t d = depthCp[o];
                const int32_t waterMm = FineTile::waterMmFromDepth(d, splineMm);
                ++st.evaluated;
                st.reliefWet.push_back(relief);
                st.depthMm.push_back(static_cast<int64_t>(d) * kWaterDepthLsbMm);
                st.headroomMm.push_back(static_cast<int64_t>(waterMm) -
                                        static_cast<int64_t>(centreAmpMm));

                const bool mmBuried = centreAmpMm >= waterMm;
                const bool voxBuried = drawnDry(centreAmpMm, waterMm);
                if (mmBuried) ++st.buriedMm;
                if (voxBuried) ++st.buriedVoxel;
                if (voxBuried && !mmBuried) ++st.sameVoxelBand;
                st.voxelsFilled.push_back(filledVoxelCount(centreAmpMm, waterMm));

                // The patch. Columns are spread evenly across the pixel; the
                // centre one is included when sub is odd.
                int64_t vis = 0, tot = 0;
                for (int sy = 0; sy < sub; ++sy) {
                    for (int sx = 0; sx < sub; ++sx) {
                        const int64_t oxMm = x0Mm + (pxMm * (2 * sx + 1)) / (2 * sub);
                        const int64_t oyMm = y0Mm + (pxMm * (2 * sy + 1)) / (2 * sub);
                        const int64_t vx = floorDiv(oxMm, kVoxelSizeMm);
                        const int64_t vy = floorDiv(oyMm, kVoxelSizeMm);
                        const int32_t g = amp.surfaceMm(vx, vy);
                        ++tot;
                        if (!drawnDry(g, waterMm)) ++vis;
                    }
                }
                st.subColumns += tot;
                st.subVisible += vis;
                const int64_t visPct = tot ? (vis * 100) / tot : 0;
                st.visiblePctPerPixel.push_back(visPct);
                if (vis == 0) ++st.pixelsFullyInvisible;
                if (vis == tot) ++st.pixelsFullyVisible;
            }
        }
        all.push_back(std::move(st));
    }

    // --- report --------------------------------------------------------------
    const auto emit = [&](const TileStats& st, const char* name) {
        std::printf("\n=== %s ===\n", name);
        std::printf("  wet pixels in plane   : %" PRId64 "\n", st.wet);
        std::printf("  evaluated             : %" PRId64 "   dropped (incomplete "
                    "footprint): %" PRId64 "\n",
                    st.evaluated, st.dropped);
        if (st.evaluated == 0) return;
        const double n = static_cast<double>(st.evaluated);
        std::printf("  HEADLINE  drawn-dry at voxel granularity : %" PRId64 " / %" PRId64
                    "  = %.2f%%\n",
                    st.buriedVoxel, st.evaluated, 100.0 * static_cast<double>(st.buriedVoxel) / n);
        std::printf("            rendered >= water (millimetres): %" PRId64 " / %" PRId64
                    "  = %.2f%%\n",
                    st.buriedMm, st.evaluated, 100.0 * static_cast<double>(st.buriedMm) / n);
        std::printf("            same-voxel band (mm-visible, voxel-buried): %" PRId64
                    "  = %.2f%%\n",
                    st.sameVoxelBand, 100.0 * static_cast<double>(st.sameVoxelBand) / n);
        std::printf("  patch: %" PRId64 " / %" PRId64 " sub-columns emit water = %.2f%%;  "
                    "pixels 0%% visible: %" PRId64 " (%.2f%%), 100%% visible: %" PRId64
                    " (%.2f%%)\n",
                    st.subVisible, st.subColumns,
                    st.subColumns ? 100.0 * static_cast<double>(st.subVisible) /
                                        static_cast<double>(st.subColumns)
                                  : 0.0,
                    st.pixelsFullyInvisible,
                    100.0 * static_cast<double>(st.pixelsFullyInvisible) / n,
                    st.pixelsFullyVisible,
                    100.0 * static_cast<double>(st.pixelsFullyVisible) / n);
        std::printf("  distributions (mm unless noted):\n");
        reportDist("baked depth (SPLINE datum)", st.depthMm);
        reportDist("AMPLIFIED - SPLINE @ wet", st.reliefWet);
        reportDist("AMPLIFIED - SPLINE @ dry", st.reliefDry);
        reportDist("water - AMPLIFIED (headroom)", st.headroomMm);
        reportDist("water voxels in column", st.voxelsFilled);
        reportDist("visible sub-columns [%]", st.visiblePctPerPixel);
    };

    for (const TileStats& st : all) {
        char name[64];
        std::snprintf(name, sizeof(name), "tile %d,%d", st.tx, st.ty);
        emit(st, name);
        pooled.wet += st.wet;
        pooled.evaluated += st.evaluated;
        pooled.dropped += st.dropped;
        pooled.buriedMm += st.buriedMm;
        pooled.buriedVoxel += st.buriedVoxel;
        pooled.sameVoxelBand += st.sameVoxelBand;
        pooled.subColumns += st.subColumns;
        pooled.subVisible += st.subVisible;
        pooled.pixelsFullyInvisible += st.pixelsFullyInvisible;
        pooled.pixelsFullyVisible += st.pixelsFullyVisible;
        const auto app = [](std::vector<int64_t>& dst, const std::vector<int64_t>& src) {
            dst.insert(dst.end(), src.begin(), src.end());
        };
        app(pooled.reliefWet, st.reliefWet);
        app(pooled.reliefDry, st.reliefDry);
        app(pooled.depthMm, st.depthMm);
        app(pooled.headroomMm, st.headroomMm);
        app(pooled.voxelsFilled, st.voxelsFilled);
        app(pooled.visiblePctPerPixel, st.visiblePctPerPixel);
    }
    emit(pooled, "POOLED");

    std::printf("\ncross-check: top SOLID voxel uses the CENTRE rule "
                "(vz*%d+%d <= AMPLIFIED); the water fill above uses the BOTTOM rule. "
                "They are different tests and only the second decides visibility.\n",
                kVoxelSizeMm, kVoxelSizeMm / 2);
    std::printf("sampler counters: missingTileQueries=%" PRIu64 " blockDecodeFailures=%" PRIu64
                "  (dropped pixels above are the ones these fired on)\n",
                fine.missingTileQueries.load(), fine.blockDecodeFailures.load());
    (void)topSolidVz;
    return 0;
}
