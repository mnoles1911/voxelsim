// vxc_waterrefreshprobe -- what a FLIGHT costs the near-field water disc, per
// second of flight rather than per rebuild.
//
// ---------------------------------------------------------------------------
// THE QUESTION
//
// `RefreshImplicitWater` keys its rebuild on the camera's BRICK CENTRE, so a
// 0.8 m step throws away the whole candidate list and re-sweeps 65 x 65 brick
// columns. The far-water plan flags this as the biggest unmeasured risk left
// and asks, before any resolution scheme is designed, whether the full-disc
// rebuild is in fact the bottleneck.
//
// "Per rebuild" cannot answer that, because the rebuild RATE is the other half
// of it: a rebuild is cheap if it happens twice a second and ruinous at forty.
// Everything here is therefore reported per SECOND OF SIMULATED FLIGHT, plus
// the worst single frame, which is what actually stutters.
//
// ---------------------------------------------------------------------------
// WHAT IT RUNS
//
// Two schemes over the SAME flight path, on the same real tiles:
//
//   FULL       today. Camera crosses a brick boundary -> clear the queue, clear
//              nothing else (there is no column cache in the client), re-sweep
//              all 4,225 columns, re-offer every candidate, re-mesh every one.
//
//   INCREMENT  voxelcore/waterwindow.h. Sweep only `Box(new) \ Box(old)`, keep
//              the survivors, evict the departed. The candidate predicate does
//              not mention the camera, so this is an IDENTITY and not an
//              approximation -- which is checked here rather than asserted:
//              every frame both schemes' live sets are hashed and compared, and
//              a single disagreement is a hard failure.
//
// Bricks are meshed with `meshBrick<8>` FOR REAL, at the drain budget the
// client uses (`kMaxImplicitMeshesPerTick = 192`), so "bricks re-meshed" and
// "quads regenerated" are counts of work done and not of work offered.
//
// ---------------------------------------------------------------------------
// READ THE CANDIDATE/MESHED DISTINCTION BEFORE QUOTING ANY NUMBER FROM HERE
//
// The sweep offers every brick from the box FLOOR up to the flood level with no
// lower bound from the ground, so in shallow water most of what it offers is
// underground and meshes to nothing. At the far-water sites the 25.6 m box
// offers 71,825 candidates of which ~8,400 mesh non-empty -- 88% underground.
// A candidate count is not a draw count. Both are printed, separately, and the
// comparison that matters is MESHED against MESHED.
//
// ---------------------------------------------------------------------------
// WHAT THE CAVERN TERM IS DOING HERE (nothing, deliberately)
//
// The client's ceiling is max(cavern flood, lake datum). The cavern half reads
// `FVoxelWaterImpl::Tiles`, a SyntheticTileSampler -- a DIFFERENT WORLD from
// the baked tiles on screen (see waterdatumprobe's header, which is where that
// was measured). Including it here would measure that bug rather than this one.
// The lake/river datum is the term that is real on a baked run, and it is the
// one farwaterprobe's baseline uses, so the two are comparable.
//
// usage:
//   vxc_waterrefreshprobe <tiledir> [--at Xm Ym] [--scan] [--seconds S]
//                         [--fps N] [--speed MPS] [--heading DEG] [--agl M]
//                         [--climb MPS] [--zstd PATH]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// Without NOMINMAX, windows.h's min/max macros break every std::max here under
// MSVC, and the clang/ninja build on this box does not catch it.
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "voxelcore/amplifier.h"
#include "voxelcore/brick.h"
#include "voxelcore/lakes.h"
#include "voxelcore/mesher.h"
#include "voxelcore/tiles.h"
#include "voxelcore/tilestore.h"
#include "voxelcore/waterca.h"
#include "voxelcore/waterwindow.h"

using namespace vxc;

namespace {

// --- runtime zstd, bound the way the game binds it --------------------------
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

// --- the client's own constants, restated because they live in a UE TU ------
constexpr int32_t kRadiusBricks = 32;
constexpr int32_t kRadiusBricksZ = 16;
constexpr int32_t kMaxImplicitMeshesPerTick = 192;
constexpr int32_t kMinVisibleFillProbe = 8;

// --- brick keys -------------------------------------------------------------
// int32 to match VoxelCoords::FVoxelCoord, so nothing here can represent a
// brick the client could not.
struct BrickKey3 {
    int32_t x, y, z;
    bool operator==(const BrickKey3& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct BrickKey3Hash {
    size_t operator()(const BrickKey3& k) const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](int32_t v) {
            h ^= uint64_t(uint32_t(v));
            h *= 1099511628211ull;
        };
        mix(k.x);
        mix(k.y);
        mix(k.z);
        return size_t(h);
    }
};
struct ColKey2 {
    int32_t x, y;
    bool operator==(const ColKey2& o) const { return x == o.x && y == o.y; }
};
struct ColKey2Hash {
    size_t operator()(const ColKey2& k) const {
        return size_t((uint64_t(uint32_t(k.x)) << 32) ^ uint64_t(uint32_t(k.y)) * 2654435761ull);
    }
};

// An order-independent signature of a live set: summed per-key hashes. Two sets
// agree iff every member agrees, and it costs one add per insert/erase instead
// of a sort.
inline uint64_t keySignature(const BrickKey3& k) {
    uint64_t h = uint64_t(uint32_t(k.x)) * 0x9E3779B97F4A7C15ull;
    h ^= uint64_t(uint32_t(k.y)) * 0xC2B2AE3D27D4EB4Full;
    h ^= uint64_t(uint32_t(k.z)) * 0x165667B19E3779F9ull;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return h;
}

// --- the per-column half of the sweep, which is the expensive half ----------
// One datum resolve and one amplified-ground evaluation per brick COLUMN. Both
// are functions of (bx, by) ONLY -- no camera term anywhere -- which is the
// property the incremental scheme rests on.
struct ColData {
    bool wet = false;
    int32_t lakeZMm = kNoWaterMm;
    int32_t groundMm = 0;
    int64_t floodBrickZ = 0;
    // A PROVABLE lower bound on the amplified ground over the brick's padded
    // footprint, or kSurfaceLowerBoundDeclined. Only resolved in `grounded`
    // mode, because it is a second amplifier query per column and charging the
    // other schemes for it would flatter them.
    int64_t groundFloorMm = kSurfaceLowerBoundDeclined;
};

struct World {
    Amplifier* amp;
    CompositeWaterSampler* water;
};

ColData evaluateColumn(World& w, int64_t bx, int64_t by, bool grounded) {
    const int64_t edge = int64_t(WaterBrick8::kEdge);
    const int64_t vx = bx * edge, vy = by * edge;
    ColData c;
    c.lakeZMm = w.water->waterSurfaceMmAtVoxel(vx, vy);
    if (c.lakeZMm == kNoWaterMm) return c; // dry column: whole vertical run skipped
    c.wet = true;
    c.groundMm = w.amp->surfaceMm(vx, vy);
    c.floodBrickZ = floorDiv(int64_t(c.lakeZMm) / int64_t(kVoxelSizeMm), edge);
    if (grounded) {
        // The same padded footprint the interior proof already uses, and the
        // exact mirror of the client's GetSurfaceUpperBoundMm call.
        c.groundFloorMm = w.amp->surfaceLowerBoundMm(vx - 1, vy - 1, vx + edge, vy + edge);
    }
    return c;
}

// Is (bx, by, bz) a candidate? THE CAMERA IS NOT AN ARGUMENT, and that is the
// whole point -- see waterwindow.h. The box clips this predicate; it does not
// participate in it.
//
// `grounded` adds the FLOOR the sweep has never had. Today the vertical run
// starts at the box floor with no lower bound from the ground at all, so in
// shallow water most of what it offers is buried rock that meshes to nothing.
// The bound is a PROOF, not a margin: `surfaceLowerBoundMm` is a guaranteed
// lower bound on the ground over the padded footprint, so a brick whose padded
// TOP is at or below it has every padded cell inside rock, fill 0 everywhere,
// and therefore no face anywhere. Declining to bound (kSurfaceLowerBoundDeclined)
// fails OPEN, which is the same direction the ceiling term fails.
bool isCandidate(const ColData& c, int64_t bz, bool grounded) {
    if (!c.wet) return false;
    const int64_t edge = int64_t(WaterBrick8::kEdge);
    const int64_t padBottomMm = (bz * edge - 1) * kVoxelSizeMm;
    const int64_t padTopMm = (bz * edge + edge) * kVoxelSizeMm;
    // Proven interior: every padded cell full water, so no face anywhere.
    if (padBottomMm >= c.groundMm && padTopMm + kVoxelSizeMm <= int64_t(c.lakeZMm)) return false;
    if (bz > c.floodBrickZ) return false;
    if (grounded && c.groundFloorMm != kSurfaceLowerBoundDeclined && padTopMm <= c.groundFloorMm) {
        return false; // provably buried
    }
    return true;
}

// --- the mesh, run for real -------------------------------------------------
// The padded footprint is gathered ONCE per brick (10 x 10 columns, the only
// distinct values the clamped sampler can ask for) instead of once per padded
// cell. That is the same memo the client's BuildWaterFillPad is, and without it
// this probe spends all its time in the amplifier rather than in the thing
// being measured.
struct PadCols {
    int32_t g[10][10];
    int32_t d[10][10];
};

void gatherPad(World& w, int64_t vx, int64_t vy, PadCols& out) {
    const int64_t edge = int64_t(WaterBrick8::kEdge);
    for (int j = 0; j < 10; ++j) {
        const int64_t ny = vy + (j == 0 ? -1 : (j == 9 ? edge : int64_t(j - 1)));
        for (int i = 0; i < 10; ++i) {
            const int64_t nx = vx + (i == 0 ? -1 : (i == 9 ? edge : int64_t(i - 1)));
            out.g[i][j] = w.amp->surfaceMm(nx, ny);
            out.d[i][j] = w.water->waterSurfaceMmAtVoxel(nx, ny);
        }
    }
}

size_t meshOneBrick(World& w, const BrickKey3& k, std::vector<Quad>& raw) {
    const int64_t edge = int64_t(WaterBrick8::kEdge);
    const int64_t vx = int64_t(k.x) * edge, vy = int64_t(k.y) * edge, oz = int64_t(k.z) * edge;
    PadCols pad;
    gatherPad(w, vx, vy, pad);
    raw.clear();
    meshBrick<WaterBrick8::kEdge>(
        [&pad, oz](int x, int y, int z) -> MaterialId {
            const int i = x < 0 ? 0 : (x >= int(WaterBrick8::kEdge) ? 9 : x + 1);
            const int j = y < 0 ? 0 : (y >= int(WaterBrick8::kEdge) ? 9 : y + 1);
            const uint8_t fill = implicitWaterFill(oz + z, pad.g[i][j], pad.d[i][j], false);
            return fill >= kMinVisibleFillProbe ? MaterialId(1) : MaterialId(MAT_AIR);
        },
        raw);
    return raw.size();
}

// --- the flight -------------------------------------------------------------
struct FlightConfig {
    double x0M = 0, y0M = 0;
    double headingDeg = 45.0;
    double speedMps = 30.0;
    double aglM = 40.0;
    double climbMps = 0.0;
    double seconds = 20.0;
    int fps = 60;
};

struct RunTotals {
    int64_t frames = 0;
    int64_t rebuildFrames = 0;
    int64_t colsSwept = 0;    // columns whose datum+ground were actually evaluated
    int64_t bricksOffered = 0; // candidates enqueued
    int64_t bricksMeshed = 0;  // meshBrick<8> calls actually executed
    int64_t bricksNonEmpty = 0;
    int64_t quads = 0;
    int64_t evictions = 0;
    int64_t discarded = 0; // queued but thrown away by a later rebuild
    double sweepMs = 0, meshMs = 0;
    // worst single frame
    double worstFrameMs = 0;
    double worstSweepMs = 0;
    int64_t worstFrameCols = 0;
    int64_t worstFrameOffered = 0;
    // ...and the same excluding the FIRST window build, which is a full sweep
    // in both schemes by construction (there is nothing to shift from) and
    // happens once at spawn. Steady-state is what stutters while flying, so
    // quoting the spawn frame as "the spike" would flatter the incremental
    // scheme's before-number and its after-number equally, and hide the
    // difference that matters.
    double steadyWorstFrameMs = 0;
    double steadyWorstSweepMs = 0;
    int64_t steadyWorstCols = 0;
    int64_t maxQueue = 0;
    int64_t framesQueueNonEmpty = 0;
    std::vector<uint64_t> liveSig; // per frame, for cross-scheme equivalence
};

void runScheme(bool incremental, bool grounded, const FlightConfig& fc, uint64_t seed,
               const std::string& fineDirCopyUnused, FineTileSampler& fine, RunTotals& out) {
    (void)fineDirCopyUnused;
    // Each scheme gets its OWN Amplifier on its OWN thread. amplifier.cpp
    // memoises cavern sites in a `static thread_local` slot table, so a warm
    // table from the first scheme would make the second look faster for free.
    LakeSampler lakes(fine);
    RiverSampler rivers(fine);
    CompositeWaterSampler water(lakes, rivers);
    Amplifier amp(seed, fine);
    World w{&amp, &water};

    const int64_t edge = int64_t(WaterBrick8::kEdge);
    std::unordered_map<ColKey2, ColData, ColKey2Hash> cols;
    std::unordered_set<BrickKey3, BrickKey3Hash> live;
    std::vector<BrickKey3> pending;
    std::vector<Quad> raw;
    WaterWindow win; // empty == nothing built yet
    uint64_t sig = 0;

    const int frames = int(fc.seconds * double(fc.fps));
    const double dt = 1.0 / double(fc.fps);
    const double hx = std::cos(fc.headingDeg * 3.14159265358979323846 / 180.0);
    const double hy = std::sin(fc.headingDeg * 3.14159265358979323846 / 180.0);

    out.liveSig.reserve(size_t(frames));

    for (int f = 0; f < frames; ++f) {
        const double t = double(f) * dt;
        const double camXm = fc.x0M + hx * fc.speedMps * t;
        const double camYm = fc.y0M + hy * fc.speedMps * t;
        const int64_t camVx = floorDiv(int64_t(camXm * 1000.0), int64_t(kVoxelSizeMm));
        const int64_t camVy = floorDiv(int64_t(camYm * 1000.0), int64_t(kVoxelSizeMm));
        // Terrain-following at a fixed height above ground, which is what makes
        // the vertical centre move on its own -- a level flight over a valley
        // crosses z brick boundaries without the pilot doing anything.
        const double groundM = double(amp.surfaceMm(camVx, camVy)) / 1000.0;
        const double camZm = groundM + fc.aglM + fc.climbMps * t;
        const int64_t camVz = floorDiv(int64_t(camZm * 1000.0), int64_t(kVoxelSizeMm));

        const int64_t cx = floorDiv(camVx, edge);
        const int64_t cy = floorDiv(camVy, edge);
        const int64_t cz = floorDiv(camVz, edge);
        const WaterWindow nw = waterWindowAt(cx, cy, cz, kRadiusBricks, kRadiusBricksZ);

        int64_t frameCols = 0, frameOffered = 0;
        const auto tSweep0 = std::chrono::steady_clock::now();

        if (nw != win) {
            ++out.rebuildFrames;
            if (!incremental) {
                // TODAY. The queue is Reset and the whole box re-swept. Anything
                // still queued from the last rebuild is thrown away unmeshed --
                // which at flight speed is most of it.
                out.discarded += int64_t(pending.size());
                pending.clear();
                live.clear();
                sig = 0;
                // There is no column cache in the client: every rebuild
                // re-resolves every column's datum and ground.
                cols.clear();
                for (int64_t by = nw.y0; by <= nw.y1; ++by) {
                    for (int64_t bx = nw.x0; bx <= nw.x1; ++bx) {
                        const ColData c = evaluateColumn(w, bx, by, grounded);
                        ++frameCols;
                        if (!c.wet) continue;
                        for (int64_t bz = nw.z0; bz <= nw.z1; ++bz) {
                            if (!isCandidate(c, bz, grounded)) continue;
                            const BrickKey3 k{int32_t(bx), int32_t(by), int32_t(bz)};
                            live.insert(k);
                            sig += keySignature(k);
                            pending.push_back(k);
                            ++frameOffered;
                        }
                    }
                }
            } else {
                // SHIFT AND FILL. Sweep only what entered; keep the survivors;
                // evict what left.
                WaterWindow gone[kWaterWindowMaxRegions];
                const int nGone = waterWindowDifference(win, nw, gone);
                for (int r = 0; r < nGone; ++r) {
                    const WaterWindow& g = gone[r];
                    for (int64_t by = g.y0; by <= g.y1; ++by)
                        for (int64_t bx = g.x0; bx <= g.x1; ++bx)
                            for (int64_t bz = g.z0; bz <= g.z1; ++bz) {
                                const BrickKey3 k{int32_t(bx), int32_t(by), int32_t(bz)};
                                if (live.erase(k)) {
                                    sig -= keySignature(k);
                                    ++out.evictions;
                                }
                            }
                }
                // Columns that left the footprint entirely stop being cached.
                WaterWindow goneCols[kWaterWindowMaxColumnRegions];
                const int nGoneCols = waterWindowColumnDifference(win, nw, goneCols);
                for (int r = 0; r < nGoneCols; ++r) {
                    const WaterWindow& g = goneCols[r];
                    for (int64_t by = g.y0; by <= g.y1; ++by)
                        for (int64_t bx = g.x0; bx <= g.x1; ++bx)
                            cols.erase(ColKey2{int32_t(bx), int32_t(by)});
                }
                // A pending brick that left the window must not be meshed.
                pending.erase(std::remove_if(pending.begin(), pending.end(),
                                             [&live](const BrickKey3& k) {
                                                 return live.find(k) == live.end();
                                             }),
                              pending.end());

                WaterWindow fresh[kWaterWindowMaxRegions];
                const int nFresh = waterWindowDifference(nw, win, fresh);
                for (int r = 0; r < nFresh; ++r) {
                    const WaterWindow& g = fresh[r];
                    for (int64_t by = g.y0; by <= g.y1; ++by) {
                        for (int64_t bx = g.x0; bx <= g.x1; ++bx) {
                            const ColKey2 ck{int32_t(bx), int32_t(by)};
                            auto it = cols.find(ck);
                            if (it == cols.end()) {
                                // Only a column the window has never held costs
                                // a datum resolve. A pure-altitude step finds
                                // every column already here and sweeps none.
                                it = cols.emplace(ck, evaluateColumn(w, bx, by, grounded)).first;
                                ++frameCols;
                            }
                            const ColData& c = it->second;
                            if (!c.wet) continue;
                            for (int64_t bz = g.z0; bz <= g.z1; ++bz) {
                                if (!isCandidate(c, bz, grounded)) continue;
                                const BrickKey3 k{int32_t(bx), int32_t(by), int32_t(bz)};
                                if (!live.insert(k).second) continue;
                                sig += keySignature(k);
                                pending.push_back(k);
                                ++frameOffered;
                            }
                        }
                    }
                }
            }
            win = nw;
        }

        const auto tSweep1 = std::chrono::steady_clock::now();
        const double sweepMs =
            std::chrono::duration<double, std::milli>(tSweep1 - tSweep0).count();

        // --- the drain, at the client's own budget --------------------------
        int64_t meshedThisFrame = 0;
        const auto tMesh0 = std::chrono::steady_clock::now();
        while (!pending.empty() && meshedThisFrame < kMaxImplicitMeshesPerTick) {
            const BrickKey3 k = pending.back();
            pending.pop_back();
            ++meshedThisFrame;
            const size_t n = meshOneBrick(w, k, raw);
            if (n > 0) {
                ++out.bricksNonEmpty;
                out.quads += int64_t(n);
            }
        }
        const auto tMesh1 = std::chrono::steady_clock::now();
        const double meshMs = std::chrono::duration<double, std::milli>(tMesh1 - tMesh0).count();

        out.bricksMeshed += meshedThisFrame;
        out.colsSwept += frameCols;
        out.bricksOffered += frameOffered;
        out.sweepMs += sweepMs;
        out.meshMs += meshMs;
        out.worstFrameMs = std::max(out.worstFrameMs, sweepMs + meshMs);
        out.worstSweepMs = std::max(out.worstSweepMs, sweepMs);
        out.worstFrameCols = std::max(out.worstFrameCols, frameCols);
        out.worstFrameOffered = std::max(out.worstFrameOffered, frameOffered);
        if (f > 0) {
            out.steadyWorstFrameMs = std::max(out.steadyWorstFrameMs, sweepMs + meshMs);
            out.steadyWorstSweepMs = std::max(out.steadyWorstSweepMs, sweepMs);
            out.steadyWorstCols = std::max(out.steadyWorstCols, frameCols);
        }
        out.maxQueue = std::max(out.maxQueue, int64_t(pending.size()));
        if (!pending.empty()) ++out.framesQueueNonEmpty;
        ++out.frames;
        out.liveSig.push_back(sig);
    }
}

// THE REGRESSION GATE FOR THE GROUND FLOOR, and it is a proof check rather
// than a spot check: at one camera, every brick the floor REJECTS but today's
// sweep offers is meshed for real, and a single non-empty one is a failure.
//
// "Water must still be absent where the datum says dry" cost a day once. The
// symmetric error -- water absent where the datum says WET -- is what an
// over-eager skip does, and it punches a hole in a water surface. This is the
// only kind of check that can tell them apart.
int64_t verifyGroundFloor(uint64_t seed, FineTileSampler& fine, const FlightConfig& fc,
                          int64_t& rejectedOut, int64_t& offeredOut) {
    LakeSampler lakes(fine);
    RiverSampler rivers(fine);
    CompositeWaterSampler water(lakes, rivers);
    Amplifier amp(seed, fine);
    World w{&amp, &water};

    const int64_t edge = int64_t(WaterBrick8::kEdge);
    const int64_t camVx = floorDiv(int64_t(fc.x0M * 1000.0), int64_t(kVoxelSizeMm));
    const int64_t camVy = floorDiv(int64_t(fc.y0M * 1000.0), int64_t(kVoxelSizeMm));
    const double groundM = double(amp.surfaceMm(camVx, camVy)) / 1000.0;
    const int64_t camVz =
        floorDiv(int64_t((groundM + fc.aglM) * 1000.0), int64_t(kVoxelSizeMm));
    const WaterWindow win = waterWindowAt(floorDiv(camVx, edge), floorDiv(camVy, edge),
                                          floorDiv(camVz, edge), kRadiusBricks, kRadiusBricksZ);

    int64_t violations = 0, rejected = 0, offered = 0;
    std::vector<Quad> raw;
    for (int64_t by = win.y0; by <= win.y1; ++by) {
        for (int64_t bx = win.x0; bx <= win.x1; ++bx) {
            const ColData c = evaluateColumn(w, bx, by, /*grounded=*/true);
            if (!c.wet) continue;
            for (int64_t bz = win.z0; bz <= win.z1; ++bz) {
                if (!isCandidate(c, bz, /*grounded=*/false)) continue;
                ++offered;
                if (isCandidate(c, bz, /*grounded=*/true)) continue;
                ++rejected;
                const BrickKey3 k{int32_t(bx), int32_t(by), int32_t(bz)};
                if (meshOneBrick(w, k, raw) > 0) ++violations;
            }
        }
    }
    rejectedOut = rejected;
    offeredOut = offered;
    return violations;
}

void report(const char* name, const RunTotals& r, double seconds) {
    std::printf("\n=== %s ===\n", name);
    std::printf("  frames                       %lld  (%lld with a moved window)\n",
                (long long)r.frames, (long long)r.rebuildFrames);
    std::printf("  PER SECOND OF FLIGHT\n");
    std::printf("    window rebuilds            %8.1f /s\n", double(r.rebuildFrames) / seconds);
    std::printf("    candidate columns swept    %8.1f /s\n", double(r.colsSwept) / seconds);
    std::printf("    candidate bricks offered   %8.1f /s\n", double(r.bricksOffered) / seconds);
    std::printf("    BRICKS RE-MESHED           %8.1f /s\n", double(r.bricksMeshed) / seconds);
    std::printf("      of those, non-empty      %8.1f /s\n", double(r.bricksNonEmpty) / seconds);
    std::printf("    QUADS REGENERATED          %8.1f /s\n", double(r.quads) / seconds);
    std::printf("    bricks evicted             %8.1f /s\n", double(r.evictions) / seconds);
    std::printf("    queued then DISCARDED      %8.1f /s  (swept, never meshed)\n",
                double(r.discarded) / seconds);
    std::printf("    cpu in sweep               %8.2f ms/s\n", r.sweepMs / seconds);
    std::printf("    cpu in mesh                %8.2f ms/s\n", r.meshMs / seconds);
    std::printf("  WORST SINGLE FRAME\n");
    std::printf("    sweep                      %8.3f ms   (%lld columns, %lld offered)\n",
                r.worstSweepMs, (long long)r.worstFrameCols, (long long)r.worstFrameOffered);
    std::printf("    sweep + mesh               %8.3f ms\n", r.worstFrameMs);
    std::printf("  WORST FRAME IN STEADY FLIGHT (excludes the spawn build)\n");
    std::printf("    sweep                      %8.3f ms   (%lld columns)\n", r.steadyWorstSweepMs,
                (long long)r.steadyWorstCols);
    std::printf("    sweep + mesh               %8.3f ms\n", r.steadyWorstFrameMs);
    std::printf("  QUEUE\n");
    std::printf("    longest                    %lld brick(s)\n", (long long)r.maxQueue);
    std::printf("    frames with work pending   %lld of %lld (%.1f%%)\n",
                (long long)r.framesQueueNonEmpty, (long long)r.frames,
                r.frames ? 100.0 * double(r.framesQueueNonEmpty) / double(r.frames) : 0.0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: vxc_waterrefreshprobe <tiledir> [--at Xm Ym] [--scan] "
                     "[--seconds S] [--fps N] [--speed MPS] [--heading DEG] [--agl M] "
                     "[--climb MPS] [--zstd PATH]\n");
        return 2;
    }
    std::string fineDir = argv[1], zstdPath;
    FlightConfig fc;
    bool haveAt = false, scan = false;
    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--at") && i + 2 < argc) {
            fc.x0M = std::strtod(argv[i + 1], nullptr);
            fc.y0M = std::strtod(argv[i + 2], nullptr);
            haveAt = true;
            i += 2;
        } else if (!std::strcmp(a, "--scan")) {
            scan = true;
        } else if (!std::strcmp(a, "--seconds") && i + 1 < argc) {
            fc.seconds = std::strtod(argv[++i], nullptr);
        } else if (!std::strcmp(a, "--fps") && i + 1 < argc) {
            fc.fps = int(std::strtol(argv[++i], nullptr, 10));
        } else if (!std::strcmp(a, "--speed") && i + 1 < argc) {
            fc.speedMps = std::strtod(argv[++i], nullptr);
        } else if (!std::strcmp(a, "--heading") && i + 1 < argc) {
            fc.headingDeg = std::strtod(argv[++i], nullptr);
        } else if (!std::strcmp(a, "--agl") && i + 1 < argc) {
            fc.aglM = std::strtod(argv[++i], nullptr);
        } else if (!std::strcmp(a, "--climb") && i + 1 < argc) {
            fc.climbMps = std::strtod(argv[++i], nullptr);
        } else if (!std::strcmp(a, "--zstd") && i + 1 < argc) {
            zstdPath = argv[++i];
        }
    }
    if (fc.fps <= 0) fc.fps = 60;

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

    // The seed comes off the tiles, never a flag: the detail octaves are seeded,
    // so a wrong seed produces plausible detail over the right terrain.
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
    int refused = 0;
    std::vector<std::pair<int32_t, int32_t>> loaded;
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
        loaded.push_back({t->tileX(), t->tileY()});
        fine.loadTile(std::move(*t));
    }
    const int64_t pxMm = fine.pixelSizeMm();
    const int64_t tileSize = int64_t(fine.tileSize());
    if (pxMm <= 0 || tileSize <= 0) {
        std::fprintf(stderr, "no tile loaded (%d refused)\n", refused);
        return 1;
    }
    std::printf("seed %llu  tiles %zu (refused %d)  pixel %lld mm  tile span %.1f m\n",
                (unsigned long long)seed, fine.tileCount(), refused, (long long)pxMm,
                double(tileSize * pxMm) / 1000.0);

    // --- pick a start, and REFUSE to measure dry ground ---------------------
    // Three of nine vista sites were once wrong, including a "beach" in open
    // water. A flight that never passes water measures an empty sweep and
    // reports a beautiful improvement over nothing at all.
    {
        LakeSampler lakes(fine);
        RiverSampler rivers(fine);
        CompositeWaterSampler water(lakes, rivers);
        if (scan || !haveAt) {
            // Coarse census over the loaded tiles: which 200 m cell holds the
            // most wet columns.
            const double tileM = double(tileSize * pxMm) / 1000.0;
            double bestX = 0, bestY = 0;
            int bestWet = -1;
            std::printf("\n=== SCAN: wettest 200 m cells in the loaded set ===\n");
            std::vector<std::tuple<int, double, double>> hits;
            for (const auto& tl : loaded) {
                const double ox = double(tl.first) * tileM, oy = double(tl.second) * tileM;
                for (int cyi = 0; cyi < int(tileM / 200.0); ++cyi) {
                    for (int cxi = 0; cxi < int(tileM / 200.0); ++cxi) {
                        const double sx = ox + double(cxi) * 200.0;
                        const double sy = oy + double(cyi) * 200.0;
                        int wet = 0;
                        for (int s = 0; s < 100; ++s) {
                            const double px = sx + double(s % 10) * 20.0;
                            const double py = sy + double(s / 10) * 20.0;
                            const int64_t vx =
                                floorDiv(int64_t(px * 1000.0), int64_t(kVoxelSizeMm));
                            const int64_t vy =
                                floorDiv(int64_t(py * 1000.0), int64_t(kVoxelSizeMm));
                            if (water.waterSurfaceMmAtVoxel(vx, vy) != kNoWaterMm) ++wet;
                        }
                        if (wet > 0) hits.push_back({wet, sx, sy});
                        if (wet > bestWet) {
                            bestWet = wet;
                            bestX = sx;
                            bestY = sy;
                        }
                    }
                }
            }
            std::sort(hits.begin(), hits.end(),
                      [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
            for (size_t i = 0; i < hits.size() && i < 8; ++i) {
                std::printf("  %3d/100 wet at (%.0f, %.0f) m\n", std::get<0>(hits[i]),
                            std::get<1>(hits[i]), std::get<2>(hits[i]));
            }
            if (bestWet <= 0) {
                std::fprintf(stderr, "\nNO WATER anywhere in the loaded tiles -- refusing to "
                                     "measure a dry flight.\n");
                return 1;
            }
            if (!haveAt) {
                fc.x0M = bestX;
                fc.y0M = bestY;
                std::printf("  -> starting the flight at the wettest cell (%.0f, %.0f) m\n", bestX,
                            bestY);
            }
            if (scan) return 0;
        }
    }

    const double seconds = fc.seconds;
    std::printf("\nflight: start (%.0f, %.0f) m, heading %.0f deg, %.0f m/s, %.0f m AGL, "
                "climb %.1f m/s, %.0f s at %d fps  -> %.0f m travelled\n",
                fc.x0M, fc.y0M, fc.headingDeg, fc.speedMps, fc.aglM, fc.climbMps, seconds, fc.fps,
                fc.speedMps * seconds);

    int64_t gfRejected = 0, gfOffered = 0, gfViolations = 0;
    std::thread([&] {
        gfViolations = verifyGroundFloor(seed, fine, fc, gfRejected, gfOffered);
    }).join();
    std::printf("\n=== GROUND-FLOOR PROOF CHECK at the start camera ===\n");
    std::printf("  today's sweep offers      %lld brick(s)\n", (long long)gfOffered);
    std::printf("  the ground floor rejects  %lld (%.1f%%)\n", (long long)gfRejected,
                gfOffered ? 100.0 * double(gfRejected) / double(gfOffered) : 0.0);
    std::printf("  of those, NON-EMPTY       %lld   <- must be 0\n", (long long)gfViolations);
    if (gfViolations != 0) {
        std::fprintf(stderr, "\nGROUND FLOOR IS NOT A PROOF -- it deleted real water. Refusing.\n");
        return 1;
    }

    RunTotals full, incr, grounded;
    // Separate threads: amplifier.cpp's cavern-site memo is `static
    // thread_local`, so sharing one would hand the later schemes a warm table.
    std::thread([&] { runScheme(false, false, fc, seed, fineDir, fine, full); }).join();
    std::thread([&] { runScheme(true, false, fc, seed, fineDir, fine, incr); }).join();
    std::thread([&] { runScheme(true, true, fc, seed, fineDir, fine, grounded); }).join();

    report("FULL-DISC REBUILD (today)", full, seconds);
    report("INCREMENTAL SHIFT-AND-FILL (waterwindow.h)", incr, seconds);
    report("INCREMENTAL + PROVEN GROUND FLOOR (surfaceLowerBoundMm)", grounded, seconds);

    // --- the correctness gate ----------------------------------------------
    // The two schemes must hold the SAME live set on EVERY frame. This is the
    // claim that the incremental result is an identity rather than an
    // approximation, and it is the regression gate for "water must still be
    // absent where the datum says dry".
    std::printf("\n=== EQUIVALENCE ===\n");
    if (full.liveSig.size() != incr.liveSig.size()) {
        std::printf("  FAIL: frame counts differ (%zu vs %zu)\n", full.liveSig.size(),
                    incr.liveSig.size());
        return 1;
    }
    int64_t bad = 0;
    int64_t firstBad = -1;
    for (size_t i = 0; i < full.liveSig.size(); ++i) {
        if (full.liveSig[i] != incr.liveSig[i]) {
            ++bad;
            if (firstBad < 0) firstBad = int64_t(i);
        }
    }
    if (bad == 0) {
        std::printf("  OK: identical live brick set on all %zu frames.\n", full.liveSig.size());
    } else {
        std::printf("  FAIL: %lld of %zu frames disagree (first at frame %lld)\n", (long long)bad,
                    full.liveSig.size(), (long long)firstBad);
    }

    // --- the ratios, which are the deliverable ------------------------------
    auto ratio = [](double a, double b) { return b > 0 ? a / b : 0.0; };
    std::printf("\n=== INCREMENTAL vs FULL, per second of flight ===\n");
    std::printf("  columns swept    %10.1f -> %10.1f   (%.1fx less)\n",
                double(full.colsSwept) / seconds, double(incr.colsSwept) / seconds,
                ratio(double(full.colsSwept), double(incr.colsSwept)));
    std::printf("  bricks offered   %10.1f -> %10.1f   (%.1fx less)\n",
                double(full.bricksOffered) / seconds, double(incr.bricksOffered) / seconds,
                ratio(double(full.bricksOffered), double(incr.bricksOffered)));
    std::printf("  bricks re-meshed %10.1f -> %10.1f   (%.1fx less)\n",
                double(full.bricksMeshed) / seconds, double(incr.bricksMeshed) / seconds,
                ratio(double(full.bricksMeshed), double(incr.bricksMeshed)));
    std::printf("  quads            %10.1f -> %10.1f   (%.1fx less)\n",
                double(full.quads) / seconds, double(incr.quads) / seconds,
                ratio(double(full.quads), double(incr.quads)));
    std::printf("  WORST FRAME      %10.3f ms -> %7.3f ms   (%.1fx less)\n", full.worstFrameMs,
                incr.worstFrameMs, ratio(full.worstFrameMs, incr.worstFrameMs));
    std::printf("  worst sweep      %10.3f ms -> %7.3f ms   (%.1fx less)\n", full.worstSweepMs,
                incr.worstSweepMs, ratio(full.worstSweepMs, incr.worstSweepMs));
    std::printf("  WORST IN FLIGHT  %10.3f ms -> %7.3f ms   (%.1fx less)  <- the stutter\n",
                full.steadyWorstFrameMs, incr.steadyWorstFrameMs,
                ratio(full.steadyWorstFrameMs, incr.steadyWorstFrameMs));

    // --- what the drain budget actually permits -----------------------------
    // The comparison above is only half the story if the mesher is saturated:
    // `bricks re-meshed` is then pinned at kMaxImplicitMeshesPerTick * fps in
    // EVERY scheme and shows 1.0x however much cheaper the sweep gets. Say so
    // in the output rather than leaving it to be inferred from two equal
    // numbers, because "no improvement" and "already at the ceiling" look
    // identical here and mean opposite things.
    const double drainCapacity = double(kMaxImplicitMeshesPerTick) * double(fc.fps);
    std::printf("\n=== IS THE MESHER SATURATED? (budget %d/tick at %d fps = %.0f bricks/s) ===\n",
                kMaxImplicitMeshesPerTick, fc.fps, drainCapacity);
    auto demand = [&](const char* n, const RunTotals& r) {
        const double off = double(r.bricksOffered) / seconds;
        std::printf("  %-34s demand %9.1f /s  = %5.2fx capacity   queue peak %lld%s\n", n, off,
                    off / drainCapacity, (long long)r.maxQueue,
                    r.framesQueueNonEmpty == r.frames ? "   NEVER EMPTY" : "");
    };
    demand("FULL-DISC REBUILD", full);
    demand("INCREMENTAL", incr);
    demand("INCREMENTAL + GROUND FLOOR", grounded);
    std::printf("  non-empty share of what was meshed: full %.1f%%, incr %.1f%%, grounded %.1f%%\n",
                full.bricksMeshed ? 100.0 * double(full.bricksNonEmpty) / double(full.bricksMeshed)
                                  : 0.0,
                incr.bricksMeshed ? 100.0 * double(incr.bricksNonEmpty) / double(incr.bricksMeshed)
                                  : 0.0,
                grounded.bricksMeshed
                    ? 100.0 * double(grounded.bricksNonEmpty) / double(grounded.bricksMeshed)
                    : 0.0);

    // The trap this probe exists to avoid reporting past: a flight that never
    // puts water inside the box measures an empty sweep and reports a
    // magnificent improvement over nothing at all.
    if (full.bricksOffered == 0) {
        std::printf("\n  NOTE: the box offered ZERO bricks over this whole flight. The sweep ran\n");
        std::printf("  %lld times and produced nothing, because kImplicitRadiusBricksZ = 16 puts\n",
                    (long long)full.rebuildFrames);
        std::printf("  the box floor 12.8 m under the camera and the water is further down than\n");
        std::printf("  that. This is not a measurement of meshing cost -- it is a measurement of\n");
        std::printf("  the sweep running for nothing. Re-run at a lower --agl for the other case.\n");
    }
    return bad == 0 ? 0 : 1;
}
