// vxc_bankprobe — the BANK-INTEGRITY probe for work item 7 of
// docs/watershed-system-plan.md.
//
// THE QUESTION IT ANSWERS, and nothing else. Along the channels the BAKE
// already carved into the .vxtl v2 fine tier, at VOXEL SCALE (100 mm) and
// under the v23 amplifier detail band the client actually evaluates: does the
// carved channel still hold a waterline, or does the detail band puncture its
// banks so water would run out sideways?
//
// It exists because the plan makes it the gate on the rest of item 7: the
// graded water plane (P2) and its BAKE_VERSION bump are worth building only if
// the carve holds. The risk is not hypothetical — tasks #47/#48 recorded the
// fine tier's detail band re-stranding ground at voxel scale, and v23 added a
// COMBINED gradient cap specifically to stop detail reversing the carrier's
// downhill. "Does detail break banks" has a mechanism behind it.
//
// -- WHAT IS MEASURED AGAINST WHAT ------------------------------------------
//
// Two arms over the SAME columns:
//
//   CARRIER   the baked carve on its own: carrier.h's production evalCarrier
//             over the fine tile's control lattice, with NO detail octaves,
//             NO rill, NO bedding and NO v16 horizontal warp. This is the
//             geometry the bake shipped.
//
//   AMPLIFIED Amplifier::surfaceMm over the same fine sampler: the whole v23
//             surface the client evaluates, warp and detail band included.
//
// The difference between the two arms is exactly "everything worldgen adds at
// voxel scale", which is the thing under suspicion. Nothing is re-derived: the
// carrier arm CALLS carrier.h's production evalCarrier (the fine tier does not
// prefilter — carrier.h static_asserts carrierPrefiltersSamples(1875) is
// false — so the control points go in as they come off the tile), and the
// amplified arm calls the production Amplifier. There is no lookalike here to
// drift.
//
// -- THE WATERLINE: A LADDER, BECAUSE NOTHING HAS BAKED A Q YET -------------
//
// No baked tile carries a runoff-weighted Q — that is what the REST of item 7
// builds — so any single waterline taken from a width/depth power law would be
// a PREDICTION dressed up as ground truth, and the plan explicitly warns off
// treating today's predicted widths as measured. This probe therefore never
// picks one waterline. It sweeps a LADDER of water depths above each section's
// own measured bed:
//
//     W(d) = bed + d,   d in {300, 1000, 3000, 10000} mm
//
// and reports the answer as a function of d. That is also the form item 7
// actually needs: not "it holds" but "it holds up to here", which is what
// sizing P2's water heads has to respect.
//
// A first cut of this probe DID derive one waterline geometrically, as
// bed + 3/4 x (first local crest - bed) after channel.h's 3/4-depth
// convention. It is recorded here because it failed in an instructive way:
// on real baked terrain the first turnover going outward from a stream is
// usually the VALLEY shoulder, tens of metres up, so the "channel depth" it
// measured came out at 14.5 m and the waterline it implied was a lake filling
// the valley, not a river. The statistic was satisfied by its own construct
// rather than by the terrain — exactly the failure mode this project has been
// bitten by twice already — so it was replaced by the ladder, which assumes
// nothing about where a bank is.
//
// -- THE THREE STATISTICS, AND WHAT EACH READS WHEN NOTHING IS WRONG --------
//
// Per section, per side (left/right of the bed), per depth d:
//
//   SHORE          the first column outward whose surface reaches W(d). Water
//                  at W stops here. Computed independently in each arm.
//
//   BREACH         the carrier has a shore inside the transect and the
//                  AMPLIFIED surface does NOT. Water that the baked carve
//                  contained now has a continuous sub-waterline path all the
//                  way out to the transect cap: it leaves. This is the
//                  decisive failure and it is what "the detail band punctures
//                  its banks" means physically.
//
//   RETREAT        shore_amplified - shore_carrier, in mm. How much further
//                  out the waterline reaches because detail lowered the near
//                  bank. Signed: detail raises ground as often as it lowers
//                  it, so a negative retreat is an ADVANCE and is expected.
//
//   PUNCTURES      channel.h's own local form, transcribed: wherever the
//                  carrier puts ground at or above the water line, the
//                  amplified ground must be there too. A puncture is a column
//                  where it is not. (tests/test_channel.cpp's
//                  channel_banks_contain_the_water_line; the 8107 -> 7763 -> 0
//                  history is docs/w3-channel-carving.md.) Counted over the
//                  whole transect, so it catches isolated pits a shore walk
//                  would step over.
//
// -- THE NOISE FLOOR, WHICH IS MEASURED AND NOT ASSERTED --------------------
//
// PUNCTURES cannot have a carrier-arm floor: the set it is counted over is
// defined by the carrier, so the carrier scores zero on it by construction.
// Saying "0 leaks with detail off" would be reporting the definition, not the
// ground. So the floor is measured a different way: the identical machinery
// runs on CONTROL SECTIONS placed on ground the flow plane does NOT flag as
// channel, chosen with the same margins and the same depth ladder. Whatever
// puncture and breach rate ordinary hillside produces is what the statistic
// reads when there is no bank to break, and the channel arm's margin over
// THAT is the only part of it that means anything.
//
// Reported alongside: |amplified - carrier| on control columns spread over the
// loaded footprint. If that read near zero the whole probe would be measuring
// nothing and every number below would be a vacuous pass.
//
// -- REACHES ARE CHOSEN BY MEASUREMENT ---------------------------------------
//
// Sections are drawn from the baked flow plane (docs/vxtl-v2-format.md §6:
// bit 5 = channel, bits 0-4 = log2 accumulation in m^2), stratified by
// accumulation bucket and spatially separated, so the run covers headwater
// trickles and trunk rivers rather than fifty samples of one reach. The
// accumulation is the BAKE's own.
//
// READ THE CENSUS BEFORE THE VERDICT. The channel bit is NOT a river mask:
// pipeline.flow_plane sets it on `a_eff >= channel_area_m2 OR incision >=
// channel_depth_m`, and on eroded terrain the second clause alone flags
// something like half of all pixels. Buckets below log2A ~ 17 (1e5 m^2, the
// bake's own channel-initiation area) are fluvially-incised GROUND, not
// streams, and the table separates them so a rate over "all channel pixels"
// can never be quoted as a rate over rivers.
//
// Usage:
//   vxc_bankprobe --fine-dir DIR [options]
//
//   --fine-dir DIR      directory of .vxtl v2 fine tiles (required)
//   --tiles x0,y0,x1,y1 load only tiles in this inclusive COARSE tile rect
//   --census-only       flow-plane census and reach inventory, no transects
//   --per-bucket N      sections per log2-accumulation bucket   (default 24)
//   --min-sep-px N      minimum separation between section centres, in fine
//                       pixels                                  (default 160)
//   --max-half-m N      transect half-length, metres            (default 120)
//   --bed-search-m N    half-width of the bed search, metres    (default  30)
//   --depths a,b,c      water depths above bed, mm  (default 300,1000,3000,10000)
//   --control N         control sections on non-channel ground  (default 300)
//   --excursion N       control columns for the detail-excursion self-check
//                                                               (default 40000)
//   --seed N            assert the tiles carry this seed (they supply it)
//   --zstd PATH         explicit libzstd to bind; otherwise searched
//   --dump-sections F   write one CSV row per section-side-depth to F

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/carrier.h"
#include "voxelcore/core.h"
#include "voxelcore/tilestore.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// windows.h defines min/max as macros, which then eat every std::max(a, b) in
// this file under MSVC (C2589, "'(': illegal token on right side of '::'").
// clang/ninja never saw it, so it shipped. Same guard as burialprobe.cpp and
// gpu_harness.cpp.
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace vxc;

namespace {

// --- the .vxtl v2 flow plane, docs/vxtl-v2-format.md §6 ---------------------
// One uint8 per fine pixel. Written by terrain_service.bake.pipeline.flow_plane.
constexpr uint8_t kFlowLogMask = 0x1F; // bits 0-4: log2(accumulation m^2), 0..31
constexpr uint8_t kFlowChannel = 0x20; // bit 5
constexpr uint8_t kFlowBank = 0x40;    // bit 6
// pipeline.BakeConstants.channel_area_m2 = 1e5. log2(1e5) = 16.6, so bucket 17
// is the first whose whole range clears it: at and above this the channel flag
// is AREA-qualified, below it the flag came from the incision clause alone.
constexpr int kAreaQualifiedBucket = 17;

// Directions tried when looking for the cross-section. Odd, so the mask's own
// perpendicular is always one of them.
constexpr int kDirFan = 9;

// --- runtime zstd, bound the way the game binds it -------------------------
//
// 24 of the 38 resident tiles of the current world are CODEC_ZSTD. voxel-core
// deliberately links no zstd of its own (tilestore.h's injected
// FineDecompressor; ue-project/Source/VoxelEarth/VoxelTileCodec.cpp is what
// this mirrors), so without one bound those tiles are REFUSED at load with
// kNoDecompressor. A probe that quietly measured only the 14 CODEC_RAW tiles
// would undercount the terrain by 5x and never say so, so the codec census is
// printed unconditionally and any refusal is fatal.
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
    return produced == dstLen; // the only length check CODEC_ZSTD admits
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

// Bind BOTH exports or neither: a library with ZSTD_decompress but no
// ZSTD_isError is not a zstd this code understands, and half-binding turns a
// deployment mistake into wrong terrain.
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

// --- the CARRIER arm --------------------------------------------------------
//
// The baked surface with every detail term off. It calls carrier.h's
// PRODUCTION evalCarrier over the tile's own control points rather than
// re-deriving the spline (terrainprobe.cpp keeps a private copy because it
// also needs the 30 m tier's prefilter path; this probe does not).
//
// It deliberately does NOT apply the v16 horizontal warp. The warp is a
// worldgen term, not something the bake wrote, so it belongs on the AMPLIFIED
// side of the comparison along with the octaves; the carrier arm is the carve
// as it was baked.
struct CarrierArm {
    FineTileSampler* tiles = nullptr;

    int64_t surfaceMm(int64_t vx, int64_t vy) const {
        const int64_t xMm = vx * kVoxelSizeMm, yMm = vy * kVoxelSizeMm;
        const int64_t pxMm = tiles->pixelSizeMm();
        const int64_t px = floorDiv(xMm, pxMm), py = floorDiv(yMm, pxMm);
        const int64_t fx = xMm - px * pxMm, fy = yMm - py * pxMm;
        int64_t cp[16];
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
                cp[i + 4 * j] = tiles->elevationMm(px - 1 + i, py - 1 + j);
        return evalCarrier(cp, fx, fy, pxMm).heightMm;
    }
};

struct Candidate {
    int64_t px = 0, py = 0; // global FINE pixel coords
    uint8_t logA = 0;
    bool control = false;
};

// A private mixer for SAMPLING ONLY -- which sites to visit and, on control
// ground, which way to face. Deliberately not voxelcore/hash.h's hash2: that
// one is worldgen's, its channel argument is registered in
// hash_channel_registry.h, and a probe borrowing a channel to shuffle a list
// would put a diagnostic's sampling order into the same namespace as the
// terrain's own noise fields. Nothing measured depends on this function.
uint64_t sampleMix(int64_t x, int64_t y) {
    uint64_t h = static_cast<uint64_t>(x) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<uint64_t>(y) + 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 29;
    return h;
}

int64_t pct(std::vector<int64_t>& v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(q * static_cast<double>(v.size() - 1) + 0.5);
    if (i >= v.size()) i = v.size() - 1;
    return v[i];
}

// Per (arm, depth) tallies, summed over one population of sections.
struct Tally {
    int64_t sections = 0;    // sections that produced at least one usable side
    int64_t sidesTested = 0; // sides where the CARRIER had a shore inside the transect
    int64_t sidesOpen = 0;   // sides the carrier itself could not contain: excluded
    int64_t breaches = 0;    // carrier contained, v23 did not
    int64_t checked = 0;     // columns with carrier >= W
    int64_t punctures = 0;   // ...of those, columns with amplified < W
    std::vector<int64_t> retreatMm;
    std::vector<int64_t> punctureDepthMm;
};

} // namespace

int main(int argc, char** argv) {
    std::string fineDir, zstdPath, dumpPath;
    int perBucket = 24, minSepPx = 160, controlSections = 300, excursionN = 40000;
    int64_t maxHalfM = 120, bedSearchM = 30;
    std::vector<int64_t> depths = {300, 1000, 3000, 10000};
    bool censusOnly = false;
    long long wantSeed = -1;
    bool haveRect = false;
    int rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;

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
        else if (!std::strcmp(a, "--dump-sections")) dumpPath = next(a);
        else if (!std::strcmp(a, "--per-bucket")) perBucket = std::atoi(next(a));
        else if (!std::strcmp(a, "--min-sep-px")) minSepPx = std::atoi(next(a));
        else if (!std::strcmp(a, "--control")) controlSections = std::atoi(next(a));
        else if (!std::strcmp(a, "--excursion")) excursionN = std::atoi(next(a));
        else if (!std::strcmp(a, "--max-half-m")) maxHalfM = std::atoll(next(a));
        else if (!std::strcmp(a, "--bed-search-m")) bedSearchM = std::atoll(next(a));
        else if (!std::strcmp(a, "--census-only")) censusOnly = true;
        else if (!std::strcmp(a, "--seed")) wantSeed = std::atoll(next(a));
        else if (!std::strcmp(a, "--depths")) {
            depths.clear();
            const char* s = next(a);
            while (*s) {
                depths.push_back(std::atoll(s));
                while (*s && *s != ',') ++s;
                if (*s == ',') ++s;
            }
        } else if (!std::strcmp(a, "--tiles")) {
            if (std::sscanf(next(a), "%d,%d,%d,%d", &rx0, &ry0, &rx1, &ry1) != 4) {
                std::fprintf(stderr, "--tiles wants x0,y0,x1,y1\n");
                return 2;
            }
            haveRect = true;
        } else {
            std::fprintf(stderr, "unknown option %s\n", a);
            return 2;
        }
    }
    if (fineDir.empty()) {
        std::fprintf(stderr, "usage: vxc_bankprobe --fine-dir DIR [options]\n");
        return 2;
    }
    if (depths.empty()) {
        std::fprintf(stderr, "--depths left the ladder empty\n");
        return 2;
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

    // Read the seed off the tiles rather than trust a flag. The detail octaves
    // are seeded; running the wrong seed produces plausible-looking detail over
    // the right terrain, a failure no statistic in this file could detect.
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
    if (wantSeed >= 0 && static_cast<uint64_t>(wantSeed) != seed) {
        std::fprintf(stderr, "seed mismatch: tiles carry %" PRIu64 ", --seed said %lld\n", seed,
                     wantSeed);
        return 1;
    }
    std::printf("seed: %" PRIu64 " (read from the tiles)\n", seed);

    FineTileSampler fine(seed, nullptr);
    fine.setDecompressor(dec);

    int loaded = 0, refused = 0, skipped = 0, zstdTiles = 0, rawTiles = 0;
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
        if (haveRect &&
            (t->tileX() < rx0 || t->tileX() > rx1 || t->tileY() < ry0 || t->tileY() > ry1)) {
            ++skipped;
            continue;
        }
        if (t->codec() == kCodecZstd) ++zstdTiles; else ++rawTiles;
        const int32_t tx = t->tileX(), ty = t->tileY();
        if (!fine.loadTile(std::move(*t))) {
            std::fprintf(stderr, "REJECTED %s by the sampler (seed or size mismatch)\n",
                         f.string().c_str());
            ++refused;
            continue;
        }
        coords.emplace_back(tx, ty);
        ++loaded;
    }
    std::printf("tiles: loaded=%d (CODEC_ZSTD=%d CODEC_RAW=%d) refused=%d skipped-by-rect=%d  "
                "tileSize=%u px  pixel=%d mm\n",
                loaded, zstdTiles, rawTiles, refused, skipped, fine.tileSize(),
                fine.pixelSizeMm());
    if (loaded == 0) {
        std::fprintf(stderr, "nothing loaded\n");
        return 1;
    }
    if (refused > 0) {
        std::fprintf(stderr,
                     "%d tile(s) refused -- refusing to report a verdict over a partial world\n",
                     refused);
        return 1;
    }

    const int64_t tileSz = fine.tileSize();
    const auto tileLoaded = [&](int64_t px, int64_t py) {
        return fine.findTile(static_cast<int32_t>(floorDiv(px, tileSz)),
                             static_cast<int32_t>(floorDiv(py, tileSz))) != nullptr;
    };
    // Every column a section touches must come from a loaded tile: the
    // transect, the carrier's 4x4 stencil, the warp's reach and the channel-mask
    // neighbourhood. A missing tile reads elevation 0, which would show up as a
    // spectacular fake leak rather than as an error.
    const int64_t marginPx = (maxHalfM * 1000 + kCarrierWarpMaxMm) / fine.pixelSizeMm() + 8;
    const auto regionLoaded = [&](int64_t px, int64_t py) {
        for (int64_t dy = -marginPx; dy <= marginPx; dy += marginPx)
            for (int64_t dx = -marginPx; dx <= marginPx; dx += marginPx)
                if (!tileLoaded(px + dx, py + dy)) return false;
        return true;
    };

    // --- FLOW-PLANE CENSUS --------------------------------------------------
    std::vector<int64_t> chanByLog(32, 0);
    int64_t totalPixels = 0, channelPixels = 0, bankPixels = 0, areaQualified = 0;
    int64_t tilesWithFlow = 0;
    std::vector<Candidate> cands, controls;
    const int64_t keepEvery = 11;
    {
        std::vector<uint8_t> block;
        for (auto [tx, ty] : coords) {
            const FineTile* t = fine.findTile(tx, ty);
            if (!t) continue;
            if (!t->hasFlow()) {
                std::printf("  tile (%d,%d): NO FLOW PLANE\n", tx, ty);
                continue;
            }
            ++tilesWithFlow;
            const uint32_t bpa = t->blocksPerAxis(), bd = t->blockDim();
            const int64_t baseX = static_cast<int64_t>(tx) * t->size();
            const int64_t baseY = static_cast<int64_t>(ty) * t->size();
            int64_t seenC = 0, seenN = 0;
            for (uint32_t by = 0; by < bpa; ++by) {
                for (uint32_t bx = 0; bx < bpa; ++bx) {
                    if (!t->decodeFlowBlock(bx, by, block)) continue;
                    for (uint32_t k = 0; k < block.size(); ++k) {
                        const uint8_t v = block[k];
                        ++totalPixels;
                        const bool ch = (v & kFlowChannel) != 0;
                        if ((v & kFlowBank) != 0) ++bankPixels;
                        const int64_t gx = baseX + bx * bd + (k % bd);
                        const int64_t gy = baseY + by * bd + (k / bd);
                        if (!ch) {
                            // Control ground: not channel, not bank, and not
                            // next to either.
                            if ((v & kFlowBank) == 0 && (seenN++ % 9973) == 0 &&
                                regionLoaded(gx, gy)) {
                                Candidate c;
                                c.px = gx;
                                c.py = gy;
                                c.logA = 0;
                                c.control = true;
                                controls.push_back(c);
                            }
                            continue;
                        }
                        ++channelPixels;
                        const uint8_t lg = static_cast<uint8_t>(v & kFlowLogMask);
                        chanByLog[lg]++;
                        if (lg >= kAreaQualifiedBucket) ++areaQualified;
                        if ((seenC++ % keepEvery) != 0) continue;
                        if (!regionLoaded(gx, gy)) continue;
                        Candidate c;
                        c.px = gx;
                        c.py = gy;
                        c.logA = lg;
                        cands.push_back(c);
                    }
                }
            }
        }
    }
    std::printf("\nFLOW-PLANE CENSUS over %d tiles (%" PRId64 " carry a flow plane)\n", loaded,
                tilesWithFlow);
    std::printf("  fine pixels scanned          %14" PRId64 "\n", totalPixels);
    std::printf("  channel-flagged              %14" PRId64 "  (%.2f%%)\n", channelPixels,
                totalPixels ? 100.0 * double(channelPixels) / double(totalPixels) : 0.0);
    std::printf("    of those, AREA-qualified   %14" PRId64 "  (%.2f%% of all pixels) "
                "-- log2A >= %d, i.e. the bake's own 1e5 m^2 channel area\n",
                areaQualified, totalPixels ? 100.0 * double(areaQualified) / double(totalPixels) : 0.0,
                kAreaQualifiedBucket);
    std::printf("    the rest are INCISION-flagged ground, not streams\n");
    std::printf("  bank-flagged                 %14" PRId64 "  (%.2f%%)\n", bankPixels,
                totalPixels ? 100.0 * double(bankPixels) / double(totalPixels) : 0.0);
    std::printf("\n  channel pixels by baked log2 accumulation\n  %6s %14s %16s %s\n", "log2A",
                "pixels", "area (m^2)", "kind");
    for (int i = 0; i < 32; ++i) {
        if (chanByLog[i] == 0) continue;
        std::printf("  %6d %14" PRId64 " %16.0f %s\n", i, chanByLog[i], std::pow(2.0, i),
                    i >= kAreaQualifiedBucket ? "stream (area-qualified)" : "incised ground");
    }
    if (channelPixels == 0) {
        std::fprintf(stderr, "\nNO CHANNEL PIXELS -- nothing to probe.\n");
        return 1;
    }

    // --- SECTION SELECTION --------------------------------------------------
    //
    // Stratified by bucket, then spatially separated. Ordered by a hash of the
    // coordinate rather than by (y, x): the first cut sorted by row and the
    // greedy separation pass then took everything from the northern edge of one
    // tile, which is a sample of one hillside, not of the world.
    const auto shuffleKey = [](const Candidate& c) {
        return sampleMix(c.px, c.py);
    };
    const auto spread = [&](std::vector<Candidate> v, int want) {
        std::sort(v.begin(), v.end(), [&](const Candidate& a, const Candidate& b) {
            return shuffleKey(a) < shuffleKey(b);
        });
        std::vector<Candidate> out;
        for (const Candidate& c : v) {
            bool tooClose = false;
            for (const Candidate& p : out) {
                const int64_t dx = p.px - c.px, dy = p.py - c.py;
                if (dx * dx + dy * dy < int64_t{minSepPx} * minSepPx) { tooClose = true; break; }
            }
            if (tooClose) continue;
            out.push_back(c);
            if (static_cast<int>(out.size()) >= want) break;
        }
        return out;
    };

    std::vector<Candidate> chosen;
    {
        std::vector<std::vector<Candidate>> byBucket(32);
        for (const Candidate& c : cands) byBucket[c.logA].push_back(c);
        for (int b = 31; b >= 0; --b) {
            if (byBucket[b].empty()) continue;
            for (const Candidate& c : spread(byBucket[b], perBucket)) chosen.push_back(c);
        }
    }
    const std::vector<Candidate> chosenControl = spread(controls, controlSections);
    std::printf("\nsections: %zu on channel pixels (<= %d per bucket, >= %d fine px apart) + "
                "%zu control sections on non-channel ground\n",
                chosen.size(), perBucket, minSepPx, chosenControl.size());
    if (censusOnly) return 0;

    Amplifier amp(seed, fine);
    CarrierArm carrier{&fine};

    // --- SELF-CHECK: is the detail band doing anything on THIS terrain? -----
    {
        std::vector<int64_t> exc;
        const int side = std::max(1, static_cast<int>(std::sqrt(double(excursionN))));
        // Walk the loaded tiles themselves, inset by the margin, so no column
        // can straddle a tile that is not resident.
        for (auto [tx, ty] : coords) {
            const int64_t x0 = static_cast<int64_t>(tx) * tileSz + marginPx;
            const int64_t y0 = static_cast<int64_t>(ty) * tileSz + marginPx;
            const int64_t span = tileSz - 2 * marginPx;
            if (span <= 0) continue;
            const int per = std::max(1, side / std::max<int>(1, static_cast<int>(
                                                                  std::sqrt(double(loaded)))));
            for (int j = 0; j < per; ++j) {
                for (int i = 0; i < per; ++i) {
                    const int64_t px = x0 + span * i / per, py = y0 + span * j / per;
                    const int64_t vx = px * fine.pixelSizeMm() / kVoxelSizeMm;
                    const int64_t vy = py * fine.pixelSizeMm() / kVoxelSizeMm;
                    exc.push_back(std::llabs(amp.surfaceMm(vx, vy) - carrier.surfaceMm(vx, vy)));
                }
            }
        }
        long double sum = 0;
        for (int64_t v : exc) sum += static_cast<long double>(v);
        std::printf("\nSELF-CHECK -- |amplified - carrier| on %zu columns inside loaded tiles\n",
                    exc.size());
        std::printf("  mean %.0f  p50 %" PRId64 "  p90 %" PRId64 "  p99 %" PRId64 "  max %" PRId64
                    " mm\n",
                    exc.empty() ? 0.0 : double(sum / (long double)exc.size()), pct(exc, 0.50),
                    pct(exc, 0.90), pct(exc, 0.99), pct(exc, 1.0));
        if (pct(exc, 0.90) < 10) {
            std::fprintf(stderr, "  the detail band is doing essentially nothing here; every "
                                 "number below would be a vacuous pass\n");
            return 1;
        }
    }

    // --- TRANSECTS ----------------------------------------------------------
    const int64_t pxMm = fine.pixelSizeMm();
    const int64_t N = maxHalfM * 1000 / kVoxelSizeMm;
    const int64_t bedN = std::min(N, bedSearchM * 1000 / kVoxelSizeMm);
    const int dirR = 8; // fine-pixel radius for the channel-axis estimate

    std::vector<uint8_t> fblock;
    int32_t cacheTx = 0, cacheTy = 0;
    uint32_t cacheBx = 0xffffffffu, cacheBy = 0xffffffffu;
    const auto flowAt = [&](int64_t gx, int64_t gy, uint8_t& out) -> bool {
        const int32_t tx = static_cast<int32_t>(floorDiv(gx, tileSz));
        const int32_t ty = static_cast<int32_t>(floorDiv(gy, tileSz));
        const FineTile* t = fine.findTile(tx, ty);
        if (!t || !t->hasFlow()) return false;
        const int64_t lx = gx - static_cast<int64_t>(tx) * tileSz;
        const int64_t ly = gy - static_cast<int64_t>(ty) * tileSz;
        const uint32_t bd = t->blockDim();
        const uint32_t bx = static_cast<uint32_t>(lx / bd), by = static_cast<uint32_t>(ly / bd);
        if (bx != cacheBx || by != cacheBy || tx != cacheTx || ty != cacheTy) {
            if (!t->decodeFlowBlock(bx, by, fblock)) return false;
            cacheBx = bx; cacheBy = by; cacheTx = tx; cacheTy = ty;
        }
        out = fblock[static_cast<size_t>((ly % bd) * bd + (lx % bd))];
        return true;
    };

    // tallies[population][bucket][depth]; population 0 = channel, 1 = control.
    std::vector<std::vector<std::vector<Tally>>> tal(
        2, std::vector<std::vector<Tally>>(32, std::vector<Tally>(depths.size())));
    int64_t rejAxis = 0, rejFlat = 0;
    std::vector<int64_t> bedOffsetMm, bankReliefMm;
    std::FILE* dumpF = dumpPath.empty() ? nullptr : std::fopen(dumpPath.c_str(), "w");
    if (dumpF)
        std::fprintf(dumpF, "pop,px,py,log2A,side,depth_mm,bed_mm,shore_car_mm,shore_amp_mm,"
                            "retreat_mm,breach,checked,punctures\n");

    std::vector<int64_t> cArr(static_cast<size_t>(2 * N + 1)), aArr(static_cast<size_t>(2 * N + 1));
    std::vector<Candidate> all = chosen;
    all.insert(all.end(), chosenControl.begin(), chosenControl.end());

    for (const Candidate& c : all) {
        const int pop = c.control ? 1 : 0;

        // --- across-section direction ---------------------------------------
        //
        // A transect only measures a bank if it CROSSES the channel. The first
        // cut took the perpendicular of the second moment of the local channel
        // mask and stopped there; on this world that failed badly, and both
        // failures were visible in the output rather than guessed at:
        //
        //   * the channel bit covers ~48% of pixels (the incision clause), so
        //     a 13x13 mask neighbourhood is usually a blob with no axis. 75 of
        //     92 sections were thrown away as "ill-conditioned".
        //   * where an axis WAS returned it was often the blob's, not the
        //     stream's, so the "perpendicular" ran along the valley: the bed
        //     search then found its minimum at the far edge of its window
        //     (p50 offset 29.3 m of a 30 m window), i.e. the transect was
        //     running downhill, not across anything.
        //
        // So the mask is filtered to the pixels carrying THIS pixel's own
        // order of accumulation, which is the stream thread rather than the
        // incised ground around it, and the result is then CHECKED against the
        // terrain: of a fan of directions near the mask perpendicular, take the
        // one that actually gives a cross-section, scored by the smaller of the
        // two rises out of the bed. A section whose best direction still has no
        // rise on one side is not a cross-section and is dropped.
        double nx = 0, ny = 0;
        double baseTh = 0;
        double fanRad = 0;
        if (!c.control) {
            double sxx = 0, sxy = 0, syy = 0;
            int n = 0;
            for (int dy = -dirR; dy <= dirR; ++dy)
                for (int dx = -dirR; dx <= dirR; ++dx) {
                    uint8_t v = 0;
                    if (!flowAt(c.px + dx, c.py + dy, v)) continue;
                    if ((v & kFlowChannel) == 0) continue;
                    // The thread, not the blob: same order of accumulation as
                    // the centre. Upstream and downstream neighbours share it;
                    // the incised hillside beside it does not.
                    if (static_cast<int>(v & kFlowLogMask) + 1 < static_cast<int>(c.logA)) continue;
                    sxx += double(dx) * dx;
                    sxy += double(dx) * dy;
                    syy += double(dy) * dy;
                    ++n;
                }
            if (n < 5) { ++rejAxis; continue; }
            sxx /= n; sxy /= n; syy /= n;
            const double tr = sxx + syy, det = sxx * syy - sxy * sxy;
            const double disc = std::sqrt(std::max(0.0, tr * tr / 4 - det));
            const double l1 = tr / 2 + disc;
            if (l1 <= 1e-9) { ++rejAxis; continue; }
            double ax = sxy, ay = l1 - sxx;
            if (std::fabs(ax) + std::fabs(ay) < 1e-9) { ax = 1; ay = 0; }
            const double an = std::sqrt(ax * ax + ay * ay);
            baseTh = std::atan2(ax / an, -ay / an); // the mask perpendicular
            const double l2 = tr / 2 - disc;
            // A well-formed thread constrains the direction tightly; a blob
            // does not, and the fan widens to a full half-turn rather than
            // trusting it.
            fanRad = (l2 / l1 < 0.5) ? 0.6 : 1.5708;
        } else {
            // Control ground has no channel to be perpendicular to, so it gets
            // the same DIRECTION SEARCH over a full half-turn. That matters:
            // the control population must be scored by the same estimator, or
            // it would be comparing a cross-section against an arbitrary
            // traverse and the noise floor would be a different statistic.
            baseTh = double(sampleMix(c.px, c.py) & 0xffffu) * (3.14159265358979 / 65536.0);
            fanRad = 1.5708;
        }

        const double cxMm = (double(c.px) + 0.5) * double(pxMm);
        const double cyMm = (double(c.py) + 0.5) * double(pxMm);
        const auto fillCarrier = [&](double ux, double uy) {
            for (int64_t i = -N; i <= N; ++i) {
                const double xm = cxMm + ux * double(i) * double(kVoxelSizeMm);
                const double ym = cyMm + uy * double(i) * double(kVoxelSizeMm);
                cArr[static_cast<size_t>(i + N)] =
                    carrier.surfaceMm(std::llround(xm / kVoxelSizeMm),
                                      std::llround(ym / kVoxelSizeMm));
            }
        };

        // Pick the direction on the CARRIER alone -- the baked carve chooses
        // where its own cross-section is, and letting the amplified surface
        // vote would let detail pick the transect that flatters it.
        {
            double bestScore = -1;
            for (int k = 0; k < kDirFan; ++k) {
                const double th =
                    baseTh + fanRad * (2.0 * k / double(kDirFan - 1) - 1.0);
                const double ux = std::cos(th), uy = std::sin(th);
                fillCarrier(ux, uy);
                int64_t bMm = cArr[static_cast<size_t>(N)], bIdx = 0;
                for (int64_t i = -bedN; i <= bedN; ++i)
                    if (cArr[static_cast<size_t>(i + N)] < bMm) {
                        bMm = cArr[static_cast<size_t>(i + N)];
                        bIdx = i;
                    }
                int64_t riseL = 0, riseR = 0;
                for (int64_t i = -N; i <= bIdx; ++i)
                    riseL = std::max(riseL, cArr[static_cast<size_t>(i + N)] - bMm);
                for (int64_t i = bIdx; i <= N; ++i)
                    riseR = std::max(riseR, cArr[static_cast<size_t>(i + N)] - bMm);
                // A cross-section is one that rises on BOTH sides, and the
                // bed must be inside the search window rather than pinned to
                // its edge (which is what "the transect runs downhill" looks
                // like).
                if (std::llabs(bIdx) >= bedN - 2) continue;
                const double score = double(std::min(riseL, riseR));
                if (score > bestScore) { bestScore = score; nx = ux; ny = uy; }
            }
            if (bestScore < 300) { // no direction here gives banks worth the name
                ++rejFlat;
                continue;
            }
        }

        for (int64_t i = -N; i <= N; ++i) {
            const double xm = cxMm + nx * double(i) * double(kVoxelSizeMm);
            const double ym = cyMm + ny * double(i) * double(kVoxelSizeMm);
            const int64_t vx = std::llround(xm / kVoxelSizeMm);
            const int64_t vy = std::llround(ym / kVoxelSizeMm);
            cArr[static_cast<size_t>(i + N)] = carrier.surfaceMm(vx, vy);
            aArr[static_cast<size_t>(i + N)] = amp.surfaceMm(vx, vy);
        }
        const auto C = [&](int64_t i) { return cArr[static_cast<size_t>(i + N)]; };
        const auto A = [&](int64_t i) { return aArr[static_cast<size_t>(i + N)]; };

        int64_t bedIdx = 0, bedMm = C(0);
        for (int64_t i = -bedN; i <= bedN; ++i)
            if (C(i) < bedMm) { bedMm = C(i); bedIdx = i; }
        bedOffsetMm.push_back(std::llabs(bedIdx) * kVoxelSizeMm);
        {
            int64_t riseL = 0, riseR = 0;
            for (int64_t i = -N; i <= bedIdx; ++i) riseL = std::max(riseL, C(i) - bedMm);
            for (int64_t i = bedIdx; i <= N; ++i) riseR = std::max(riseR, C(i) - bedMm);
            bankReliefMm.push_back(std::min(riseL, riseR));
        }

        for (size_t di = 0; di < depths.size(); ++di) {
            const int64_t W = bedMm + depths[di];
            Tally& t = tal[pop][c.logA][di];
            bool anySide = false;

            for (int s = 0; s < 2; ++s) {
                const int64_t step = s == 0 ? -1 : +1;
                int64_t shoreC = 0, shoreA = 0;
                bool haveC = false, haveA = false;
                for (int64_t i = bedIdx; i >= -N && i <= N; i += step) {
                    if (!haveC && C(i) >= W) { shoreC = i; haveC = true; }
                    if (!haveA && A(i) >= W) { shoreA = i; haveA = true; }
                    if (haveC && haveA) break;
                }
                if (!haveC) { ++t.sidesOpen; continue; }
                ++t.sidesTested;
                anySide = true;
                const int64_t retreat =
                    (haveA ? std::llabs(shoreA - bedIdx) - std::llabs(shoreC - bedIdx) : N) *
                    kVoxelSizeMm;
                // A breached side has no shore to retreat TO, so it is counted
                // as a breach and kept OUT of the retreat distribution. Folding
                // its sentinel in would put a 120 m "retreat" (the transect
                // cap) into a percentile table about decimetres.
                if (!haveA)
                    ++t.breaches;
                else
                    t.retreatMm.push_back(retreat);
                if (dumpF)
                    std::fprintf(dumpF,
                                 "%s,%" PRId64 ",%" PRId64 ",%u,%d,%" PRId64 ",%" PRId64
                                 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%d,,\n",
                                 c.control ? "control" : "channel", c.px, c.py, c.logA, s,
                                 // PRId64 is %ld where int64_t is long (Linux) and %lld where
                                 // it is long long (Windows); std::llabs is long long on both,
                                 // so the cast is what makes one format string correct on each.
                                 depths[di], bedMm,
                                 static_cast<int64_t>(std::llabs(shoreC - bedIdx)) * kVoxelSizeMm,
                                 haveA ? static_cast<int64_t>(std::llabs(shoreA - bedIdx)) * kVoxelSizeMm
                                       : int64_t{-1},
                                 retreat,
                                 haveA ? 0 : 1);
            }

            // channel.h's local containment form over the whole transect.
            // The DEPTH of each puncture is kept as well as its existence: a
            // column 5 mm under the waterline and a column 800 mm under it are
            // not the same finding, and a count alone cannot tell them apart.
            for (int64_t i = -N; i <= N; ++i) {
                if (C(i) < W) continue;
                ++t.checked;
                if (A(i) < W) {
                    ++t.punctures;
                    t.punctureDepthMm.push_back(W - A(i));
                }
            }
            if (anySide) ++t.sections;
        }
    }
    if (dumpF) std::fclose(dumpF);

    // --- REPORT -------------------------------------------------------------
    std::printf("\nsections dropped: axis unusable %" PRId64 ", dead-flat cross-section %" PRId64
                "\n",
                rejAxis, rejFlat);
    std::printf("bed offset from the section centre: p50 %" PRId64 " p95 %" PRId64 " mm "
                "(search half-width %lld m)\n",
                pct(bedOffsetMm, 0.5), pct(bedOffsetMm, 0.95), (long long)bedSearchM);
    std::printf("bank relief, carrier (the SMALLER of the two rises out of the bed -- these are "
                "the cross-sections everything below is about): p05 %" PRId64 " p50 %" PRId64
                " p95 %" PRId64 " mm\n",
                pct(bankReliefMm, 0.05), pct(bankReliefMm, 0.50), pct(bankReliefMm, 0.95));

    const char* popName[2] = {"CHANNEL", "CONTROL (non-channel ground)"};
    for (int pop = 0; pop < 2; ++pop) {
        std::printf("\n=== %s ===============================================\n", popName[pop]);
        std::printf("%6s %8s %7s %7s %7s %9s %9s %11s %10s %10s\n", "log2A", "depth_mm", "sects",
                    "sides", "open", "BREACH", "breach%", "bankcols", "PUNCTURE", "punct%");
        // Aggregate rows: all buckets, then the area-qualified stream subset.
        for (int mode = 0; mode < (pop == 0 ? 3 : 1); ++mode) {
            for (size_t di = 0; di < depths.size(); ++di) {
                if (mode == 2) {
                    // per-bucket detail, streams only
                    for (int b = 31; b >= kAreaQualifiedBucket; --b) {
                        const Tally& t = tal[pop][b][di];
                        if (t.sidesTested == 0) continue;
                        std::vector<int64_t> r = t.retreatMm;
                        std::printf("%6d %8" PRId64 " %7" PRId64 " %7" PRId64 " %7" PRId64
                                    " %9" PRId64 " %8.3f%% %11" PRId64 " %10" PRId64 " %8.4f%%\n",
                                    b, depths[di], t.sections, t.sidesTested, t.sidesOpen,
                                    t.breaches, 100.0 * double(t.breaches) / double(t.sidesTested),
                                    t.checked, t.punctures,
                                    t.checked ? 100.0 * double(t.punctures) / double(t.checked)
                                              : 0.0);
                    }
                    continue;
                }
                Tally a;
                for (int b = 0; b < 32; ++b) {
                    if (mode == 1 && b < kAreaQualifiedBucket) continue;
                    const Tally& t = tal[pop][b][di];
                    a.sections += t.sections;
                    a.sidesTested += t.sidesTested;
                    a.sidesOpen += t.sidesOpen;
                    a.breaches += t.breaches;
                    a.checked += t.checked;
                    a.punctures += t.punctures;
                    a.retreatMm.insert(a.retreatMm.end(), t.retreatMm.begin(), t.retreatMm.end());
                    a.punctureDepthMm.insert(a.punctureDepthMm.end(), t.punctureDepthMm.begin(),
                                             t.punctureDepthMm.end());
                }
                if (a.sidesTested == 0) continue;
                std::printf("%6s %8" PRId64 " %7" PRId64 " %7" PRId64 " %7" PRId64 " %9" PRId64
                            " %8.3f%% %11" PRId64 " %10" PRId64 " %8.4f%%   retreat p50 %" PRId64
                            " p95 %" PRId64 " mm\n",
                            mode == 0 ? "ALL" : ">=17", depths[di], a.sections, a.sidesTested,
                            a.sidesOpen, a.breaches,
                            100.0 * double(a.breaches) / double(a.sidesTested), a.checked,
                            a.punctures,
                            a.checked ? 100.0 * double(a.punctures) / double(a.checked) : 0.0,
                            pct(a.retreatMm, 0.50), pct(a.retreatMm, 0.95));
                // How many punctures are deeper than ONE VOXEL. Below that the
                // hole is finer than the grid the world is built on: the client
                // voxelises at floor(h / 100 mm), so a 40 mm dip in a
                // continuous heightfield need not survive into a single missing
                // voxel of bank. It is reported rather than filtered out --
                // silently discarding sub-voxel punctures would be choosing the
                // answer.
                int64_t overVoxel = 0;
                for (int64_t v : a.punctureDepthMm)
                    if (v > kVoxelSizeMm) ++overVoxel;
                std::printf("%6s %8s   retreat max %" PRId64 " mm | puncture depth p50 %" PRId64
                            " p95 %" PRId64 " max %" PRId64 " mm | deeper than one voxel "
                            "%" PRId64 " (%.1f%% of punctures, %.5f%% of bank columns)\n",
                            "", "", pct(a.retreatMm, 1.0), pct(a.punctureDepthMm, 0.50),
                            pct(a.punctureDepthMm, 0.95), pct(a.punctureDepthMm, 1.0), overVoxel,
                            a.punctures ? 100.0 * double(overVoxel) / double(a.punctures) : 0.0,
                            a.checked ? 100.0 * double(overVoxel) / double(a.checked) : 0.0);
            }
            if (mode == 0 && pop == 0)
                std::printf("  -- the same, restricted to AREA-QUALIFIED streams (log2A >= %d) --\n",
                            kAreaQualifiedBucket);
            if (mode == 1 && pop == 0) std::printf("  -- per stream bucket --\n");
        }
    }
    if (!dumpPath.empty()) std::printf("\nper-side rows dumped to %s\n", dumpPath.c_str());
    return 0;
}
