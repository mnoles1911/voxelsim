// vxc_stagedump — dump the CLIENT half of the amplification pipeline as plain
// float heightfields, one file per STAGE, over one world rectangle.
//
// WHY THIS EXISTS. We have measured the END of the pipeline repeatedly
// (vxc_terrainprobe) and can see that the rendered surface is wrong but not
// WHERE it went wrong. Every stage between "30 m diffusion tile on the wire"
// and "10 cm voxel the player stands on" is a separate transform, and until
// each one can be looked at on the same ground in the same units, a defect can
// only be attributed to the composition.
//
// The five stages, and who dumps them:
//
//   S0  raw 30 m diffusion tile, int16 whole metres      terrain-service tool
//   S1  bake output, 1.875 m/px (terrain_service.bake)   terrain-service tool
//   S2  CLIENT CARRIER ONLY — the C2 B-spline over the   >>> THIS TOOL <<<
//       tile raster, every detail octave and additive
//       term OFF. "What did the client inherit."
//   S3  CLIENT FULL CONTINUOUS SURFACE — Amplifier::     >>> THIS TOOL <<<
//       surfaceMm, before voxelisation.
//   S4  VOXELISED — S3 quantised to 10 cm exactly as     >>> THIS TOOL <<<
//       stratigraphyAt sees it, as a heightfield of the
//       topmost solid voxel.
//
// terrain-service/tools/dump_stage_heightfields.py writes S0 and S1 in the same
// .npy + JSON-sidecar shape over the same rectangle, so all five diff directly.
//
// THE TIER IS THE POINT, NOT A CONFIGURATION DETAIL. Amplifier behaves
// DIFFERENTLY depending on the sampler's pixelSizeMm(): on a fine world
// (1875 mm/px) the 25.6 m and 6.4 m landform octaves are deleted, and the
// curvature gate is tier-normalised. So S2/S3/S4 are dumped TWICE — once over
// the 30 m coarse sampler (TileGridSampler on s1 .vxtl v1 tiles) and once over
// a FineTileSampler reading a baked .vxtl v2 tile. That pair is the direct
// evidence for whether the client is double-counting structure the bake has
// already put into the raster.
//
// NOTHING HERE TOUCHES WORLDGEN. It reads amplifier.h/carrier.h and writes
// files. bench/ is outside the float ban (.github/workflows/ci.yml, job
// `float-ban`, which scans voxel-core/include and voxel-core/src only), so the
// heightfields can be plain float32 metres — which is what every consumer of a
// .npy expects and what makes these comparable with the Python-side stages.
//
// ---------------------------------------------------------------------------
// USAGE
//
//   vxc_stagedump --out DIR --seed N --coarse-dir DIR [--fine-dir DIR]
//                 --origin X Y --span M [--cell MM[:SPAN_M]]...
//                 [--tier coarse|fine|both] [--max-n N] [--no-s4]
//
//   --origin X Y   world coordinates in METRES of the rectangle's minimum
//                  corner. Snapped to nothing: give a multiple of 30 so the
//                  30 m, 1.875 m and 0.1 m lattices share their coarse nodes.
//   --span M       edge of the world rectangle, in metres.
//   --cell MM      lattice cell size in MILLIMETRES, repeatable. Defaults to
//                  `--cell 1875 --cell 100`. An optional `:SPAN_M` gives that
//                  lattice its own, CENTRED, sub-rectangle — because 960 m at
//                  0.1 m is 9601^2 samples (369 MB per stage per tier) and the
//                  voxel band is legible in a much smaller window. Every file's
//                  own rectangle is written into its sidecar; no consumer has
//                  to infer it.
//
// A worked invocation is in the tool's --help output.
//
// ---------------------------------------------------------------------------
// THE ONE HONEST COMPROMISE, STATED UP FRONT
//
// The amplifier is only DEFINED at integer voxel indices: surfaceMm(vx, vy)
// evaluates at world (vx * 100 mm, vy * 100 mm). The fine tier's posts are
// 1875 mm apart, which is 18.75 voxels — NOT an integer. So no voxel column
// sits exactly on a fine-tier post, and an S2/S3/S4 lattice at 1.875 m must
// snap each node to the nearest voxel column, up to 50 mm away.
//
// That is a property of the world, not of this tool: the client can only ever
// evaluate its surface on the 10 cm lattice, so 50 mm of horizontal
// misregistration against S1 is exactly what the renderer also has. Every
// sidecar records `snap_max_mm` and `snap_mean_mm` so a difference map is never
// read as a height error when it is a position error. At `--cell 100` the snap
// is identically zero.
// ---------------------------------------------------------------------------

#include <algorithm>
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
#include "voxelcore/tilestore.h"

using namespace vxc;

namespace {

// ---------------------------------------------------------------------------
// S2 — the carrier, called rather than copied.
//
// evalCarrier() in voxelcore/carrier.h is the PRODUCTION carrier: amplifier.cpp
// evalSurface() calls exactly this function on exactly this stencil. Calling it
// is what makes S2 "the client carrier" rather than "a lookalike of the client
// carrier" — the distinction that decides whether a discrepancy between S2 and
// S3 is evidence about worldgen or evidence about this file.
//
// The only thing not shared with amplifier.cpp is the memo (cachedStencil),
// which is a cache and cannot change a value.
// ---------------------------------------------------------------------------
int64_t carrierHeightMm(ITileSampler& tiles, int64_t vx, int64_t vy) {
    const int64_t xMm = vx * kVoxelSizeMm, yMm = vy * kVoxelSizeMm;
    const int64_t pxMm = tiles.pixelSizeMm();
    const int64_t px = floorDiv(xMm, pxMm), py = floorDiv(yMm, pxMm);
    const int64_t fx = xMm - px * pxMm, fy = yMm - py * pxMm;
    int64_t cp[16];
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) cp[i + 4 * j] = tiles.elevationMm(px - 1 + i, py - 1 + j);
    return evalCarrier(cp, fx, fy, pxMm).heightMm;
}

// ---------------------------------------------------------------------------
// S4 — the voxelised surface.
//
// "Exactly as stratigraphyAt sees it" is load-bearing: since kWorldGenVersion
// 12 the solid test is `centre <= surfaceMm + D` with |D| <= 350 mm from
// voxelcore/density3.h, so the solid set is NOT the region under a graph and
// floor(surfaceMm / 100) is no longer the answer. This scans DOWN from a
// provable ceiling for the first non-air voxel, which is the definition.
//
// It uses stratigraphyAt, not materialAt, exactly as the brief asks: the cave
// and cavern passes carve voids that have nothing to do with the surface
// amplification chain, and a column whose top voxel happens to sit in a cave
// mouth would otherwise report a hole as a height.
//
// REPORTED VALUE: the TOP FACE of the topmost non-air voxel, (vz + 1) * 100 mm.
// With D == 0 that is S3 rounded to the nearest 100 mm (top face is within
// +/-49 mm of surfaceMm), so S4 - S3 is centred on zero and reads directly as
// quantisation error. Subtract 0.1 m for the voxel's bottom face if that is the
// convention a consumer wants; the sidecar says which one this is.
// ---------------------------------------------------------------------------
constexpr int64_t kNoSolid = INT64_MIN;

int64_t topSolidTopFaceMm(const ColumnSample& col, int64_t& outVz) {
    // |D| <= kDensity3MaxAbsMm, so nothing can be solid above this centre.
    const int64_t ceilMm = static_cast<int64_t>(col.surfaceMm) + 400; // 350 rounded up
    int64_t vz = floorDiv(ceilMm - kVoxelSizeMm / 2, kVoxelSizeMm);
    // 16 voxels is 1.6 m, comfortably more than the 3.5-voxel envelope either
    // side; a column that finds nothing in that band has no surface here at all
    // (it cannot: the envelope is a compile-time bound), so failing loudly is
    // right.
    for (int k = 0; k < 16; ++k, --vz) {
        if (Amplifier::stratigraphyAt(col, vz) != MAT_AIR) {
            outVz = vz;
            return (vz + 1) * kVoxelSizeMm;
        }
    }
    outVz = 0;
    return kNoSolid;
}

// ---------------------------------------------------------------------------
// .npy writer (format 1.0). float32, C order, little-endian.
// ---------------------------------------------------------------------------
bool writeNpyF32(const std::filesystem::path& path, const std::vector<float>& v, int64_t ny,
                 int64_t nx) {
    char dict[256];
    const int dictLen =
        std::snprintf(dict, sizeof(dict),
                      "{'descr': '<f4', 'fortran_order': False, 'shape': (%lld, %lld), }",
                      (long long)ny, (long long)nx);
    if (dictLen <= 0) return false;
    // Header (10 bytes) + dict + padding + '\n' must be a multiple of 64.
    size_t total = 10 + static_cast<size_t>(dictLen) + 1;
    const size_t pad = (64 - (total % 64)) % 64;
    total += pad;
    const uint16_t headerLen = static_cast<uint16_t>(total - 10);

    FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) return false;
    // Every write's result is folded into `ok` rather than discarded: glibc
    // marks fwrite warn_unused_result under _FORTIFY_SOURCE, this target builds
    // with -Wall -Wextra -Werror on gcc and clang, and a silently short write
    // here is a truncated heightfield that looks like a terrain finding.
    bool ok = true;
    auto put = [&](const void* p, size_t elem, size_t n) {
        ok = ok && std::fwrite(p, elem, n, f) == n;
    };
    const unsigned char magic[8] = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0};
    put(magic, 1, 8);
    const unsigned char lenLE[2] = {static_cast<unsigned char>(headerLen & 0xFF),
                                    static_cast<unsigned char>((headerLen >> 8) & 0xFF)};
    put(lenLE, 1, 2);
    put(dict, 1, static_cast<size_t>(dictLen));
    const std::string padding(pad, ' ');
    put(padding.data(), 1, pad);
    put("\n", 1, 1);
    put(v.data(), sizeof(float), v.size());
    ok = ok && std::ferror(f) == 0;
    if (std::fclose(f) != 0) ok = false;
    return ok;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\' || c == '"') {
            o.push_back('\\');
            o.push_back(c);
        } else if (c == '\n') {
            o += "\\n";
        } else {
            o.push_back(c);
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// Lattices. Everything is INTEGER MILLIMETRES so the world rectangle a sidecar
// reports is exact rather than a float that nearly is.
// ---------------------------------------------------------------------------
struct Lattice {
    int64_t cellMm = 0;
    int64_t x0Mm = 0, y0Mm = 0; // world mm of node [0][0]
    int64_t n = 0;              // nodes per axis
};

double mm2m(int64_t mm) { return static_cast<double>(mm) / 1000.0; }

// Per-dump measurements a consumer must not have to guess at.
struct DumpStats {
    double minM = 0, maxM = 0, meanM = 0;
    int64_t snapMaxMm = 0;
    double snapMeanMm = 0;
    int64_t nInvalid = 0;   // S4 only: columns with no solid voxel in the band
    int64_t nDisplaced = 0; // S4 only: columns where D moved the top voxel
    uint64_t missingBefore = 0, missingAfter = 0;
};

void finishStats(DumpStats& st, const std::vector<float>& v) {
    if (v.empty()) return;
    double lo = v[0], hi = v[0];
    long double acc = 0;
    for (float f : v) {
        if (f < lo) lo = f;
        if (f > hi) hi = f;
        acc += f;
    }
    st.minM = lo;
    st.maxM = hi;
    st.meanM = static_cast<double>(acc / static_cast<long double>(v.size()));
}

// ---------------------------------------------------------------------------
// The sidecar. One schema for all five stages; the Python tool writes the same
// keys for S0/S1. `cell_size_mm` is the authority — nothing downstream is ever
// asked to infer a resolution.
// ---------------------------------------------------------------------------
struct Sidecar {
    std::string stage, name, tier, source;
    std::string coarseDir, fineDir;
    uint64_t seed = 0;
    int64_t pixelSizeMm = 0;
    Lattice lat;
    DumpStats st;
    std::string quantNote;
};

bool writeSidecar(const std::filesystem::path& path, const Sidecar& s, const std::string& npyName) {
    FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) return false;
    const int64_t n = s.lat.n;
    const int64_t spanMm = (n - 1) * s.lat.cellMm;
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"schema\": \"vxc.stagedump.v1\",\n");
    std::fprintf(f, "  \"stage\": \"%s\",\n", s.stage.c_str());
    std::fprintf(f, "  \"name\": \"%s\",\n", jsonEscape(s.name).c_str());
    std::fprintf(f, "  \"array\": \"%s\",\n", jsonEscape(npyName).c_str());
    std::fprintf(f, "  \"units\": \"metres\",\n");
    std::fprintf(f, "  \"dtype\": \"float32\",\n");
    std::fprintf(f, "  \"shape\": [%lld, %lld],\n", (long long)n, (long long)n);
    std::fprintf(f, "  \"axis_order\": \"row-major [y][x]; index 0 is the MINIMUM world "
                    "coordinate on that axis\",\n");
    std::fprintf(f, "  \"sample_convention\": \"node: value[j][i] is the surface at world "
                    "(origin_x + i*cell, origin_y + j*cell)\",\n");
    std::fprintf(f, "  \"cell_size_mm\": %lld,\n", (long long)s.lat.cellMm);
    std::fprintf(f, "  \"cell_size_m\": %.6f,\n", mm2m(s.lat.cellMm));
    std::fprintf(f, "  \"world_origin_m\": [%.4f, %.4f],\n", mm2m(s.lat.x0Mm), mm2m(s.lat.y0Mm));
    std::fprintf(f, "  \"world_origin_mm\": [%lld, %lld],\n", (long long)s.lat.x0Mm,
                 (long long)s.lat.y0Mm);
    std::fprintf(f, "  \"world_bounds_m\": [%.4f, %.4f, %.4f, %.4f],\n", mm2m(s.lat.x0Mm),
                 mm2m(s.lat.y0Mm), mm2m(s.lat.x0Mm + spanMm), mm2m(s.lat.y0Mm + spanMm));
    std::fprintf(f, "  \"world_span_m\": %.4f,\n", mm2m(spanMm));
    std::fprintf(f, "  \"seed\": %llu,\n", (unsigned long long)s.seed);
    std::fprintf(f, "  \"worldgen_version\": %u,\n", kWorldGenVersion);
    std::fprintf(f, "  \"voxel_size_mm\": %d,\n", kVoxelSizeMm);
    std::fprintf(f, "  \"tier\": \"%s\",\n", s.tier.c_str());
    std::fprintf(f, "  \"tier_pixel_size_mm\": %lld,\n", (long long)s.pixelSizeMm);
    std::fprintf(f, "  \"provenance\": {\n");
    std::fprintf(f, "    \"producer\": \"vxc_stagedump\",\n");
    std::fprintf(f, "    \"producer_source\": \"voxel-core/bench/stagedump.cpp\",\n");
    std::fprintf(f, "    \"stage_source\": \"%s\",\n", jsonEscape(s.source).c_str());
    std::fprintf(f, "    \"coarse_tile_dir\": \"%s\",\n", jsonEscape(s.coarseDir).c_str());
    std::fprintf(f, "    \"fine_tile_dir\": \"%s\"\n", jsonEscape(s.fineDir).c_str());
    std::fprintf(f, "  },\n");
    // THE SNAP. See the file header: the amplifier is defined only on the 10 cm
    // lattice, so a 1.875 m node is answered by the nearest voxel column.
    // Reported so a difference map against S1 is never read as a height error
    // when part of it is a position error.
    std::fprintf(f, "  \"voxel_snap\": {\n");
    std::fprintf(f, "    \"note\": \"the amplifier is defined only at integer voxel columns "
                    "(100 mm); each lattice node is answered by the NEAREST one\",\n");
    std::fprintf(f, "    \"max_mm\": %lld,\n", (long long)s.st.snapMaxMm);
    std::fprintf(f, "    \"mean_mm\": %.4f\n", s.st.snapMeanMm);
    std::fprintf(f, "  },\n");
    if (!s.quantNote.empty())
        std::fprintf(f, "  \"quantisation\": \"%s\",\n", jsonEscape(s.quantNote).c_str());
    std::fprintf(f, "  \"stats\": {\n");
    std::fprintf(f, "    \"min_m\": %.4f,\n", s.st.minM);
    std::fprintf(f, "    \"max_m\": %.4f,\n", s.st.maxM);
    std::fprintf(f, "    \"mean_m\": %.4f,\n", s.st.meanM);
    std::fprintf(f, "    \"columns_with_no_solid_voxel\": %lld,\n", (long long)s.st.nInvalid);
    std::fprintf(f, "    \"columns_displaced_by_density3\": %lld,\n", (long long)s.st.nDisplaced);
    std::fprintf(f, "    \"missing_tile_queries_before\": %llu,\n",
                 (unsigned long long)s.st.missingBefore);
    std::fprintf(f, "    \"missing_tile_queries_after\": %llu\n",
                 (unsigned long long)s.st.missingAfter);
    std::fprintf(f, "  }\n");
    std::fprintf(f, "}\n");
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

void usage() {
    std::fprintf(stderr,
                 "vxc_stagedump — dump S2/S3/S4 of the amplification pipeline as .npy + JSON\n"
                 "\n"
                 "usage: vxc_stagedump --out DIR --seed N --coarse-dir DIR [--fine-dir DIR]\n"
                 "                     --origin X Y --span M [--cell MM[:SPAN_M]]...\n"
                 "                     [--tier coarse|fine|both] [--max-n N] [--no-s4]\n"
                 "\n"
                 "  --out DIR         output directory (created). Dumps are large: use a\n"
                 "                    gitignored path.\n"
                 "  --seed N          world seed. Must match the tiles' own header seed.\n"
                 "  --coarse-dir DIR  directory of s1 .vxtl v1 tiles (30 m/px). REQUIRED even\n"
                 "                    for a fine-tier run: the fine tier carries no climate,\n"
                 "                    and column() classifies a biome.\n"
                 "  --fine-dir DIR    directory of s16 .vxtl v2 tiles (1.875 m/px). Enables\n"
                 "                    the fine tier. Refuses to fall back silently.\n"
                 "  --origin X Y      world metres of the rectangle's MINIMUM corner.\n"
                 "  --span M          rectangle edge, metres.\n"
                 "  --cell MM[:SPAN]  lattice cell in MILLIMETRES, repeatable, with an\n"
                 "                    optional own centred sub-span in metres.\n"
                 "                    Default: --cell 1875 --cell 100\n"
                 "  --tier T          coarse | fine | both   (default: both if --fine-dir)\n"
                 "  --max-n N         refuse a lattice wider than N nodes (default 4096)\n"
                 "  --no-s4           skip the voxelised stage (S4 costs a full column())\n"
                 "\n"
                 "example:\n"
                 "  vxc_stagedump --out stage-dumps/alpine --seed 20260719 \\\n"
                 "      --coarse-dir tile-cache/.../s1 --fine-dir tile-cache/.../s16 \\\n"
                 "      --origin -69120 38400 --span 960 --cell 1875 --cell 100:120\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string outDir, coarseDir, fineDir, tierArg;
    uint64_t seed = 0;
    bool haveSeed = false, haveOrigin = false, haveSpan = false, doS4 = true;
    int64_t originXm = 0, originYm = 0, spanM = 0, maxN = 4096;
    struct CellSpec {
        int64_t cellMm;
        int64_t spanM; // 0 => use the global span
    };
    std::vector<CellSpec> cells;

    auto needArg = [&](int& i, const char* name) -> const char* {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "%s needs a value\n", name);
            std::exit(2);
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
            usage();
            return 0;
        } else if (!std::strcmp(a, "--out")) {
            outDir = needArg(i, a);
        } else if (!std::strcmp(a, "--coarse-dir")) {
            coarseDir = needArg(i, a);
        } else if (!std::strcmp(a, "--fine-dir")) {
            fineDir = needArg(i, a);
        } else if (!std::strcmp(a, "--tier")) {
            tierArg = needArg(i, a);
        } else if (!std::strcmp(a, "--seed")) {
            seed = std::strtoull(needArg(i, a), nullptr, 0);
            haveSeed = true;
        } else if (!std::strcmp(a, "--origin")) {
            originXm = std::strtoll(needArg(i, a), nullptr, 10);
            originYm = std::strtoll(needArg(i, a), nullptr, 10);
            haveOrigin = true;
        } else if (!std::strcmp(a, "--span")) {
            spanM = std::strtoll(needArg(i, a), nullptr, 10);
            haveSpan = true;
        } else if (!std::strcmp(a, "--max-n")) {
            maxN = std::strtoll(needArg(i, a), nullptr, 10);
        } else if (!std::strcmp(a, "--no-s4")) {
            doS4 = false;
        } else if (!std::strcmp(a, "--cell")) {
            const char* v = needArg(i, a);
            char* endp = nullptr;
            CellSpec cs{std::strtoll(v, &endp, 10), 0};
            if (endp && *endp == ':') cs.spanM = std::strtoll(endp + 1, nullptr, 10);
            if (cs.cellMm < kVoxelSizeMm) {
                std::fprintf(stderr, "--cell %s: cell must be at least one voxel (%d mm)\n", v,
                             kVoxelSizeMm);
                return 2;
            }
            cells.push_back(cs);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a);
            usage();
            return 2;
        }
    }

    if (outDir.empty() || coarseDir.empty() || !haveSeed || !haveOrigin || !haveSpan) {
        std::fprintf(stderr, "--out, --seed, --coarse-dir, --origin and --span are required\n\n");
        usage();
        return 2;
    }
    if (spanM <= 0) {
        std::fprintf(stderr, "--span must be positive\n");
        return 2;
    }
    if (cells.empty()) cells.push_back(CellSpec{1875, 0});
    if (cells.size() == 1 && cells[0].cellMm == 1875 && cells[0].spanM == 0)
        cells.push_back(CellSpec{kVoxelSizeMm, 0});

    const bool wantFine = !fineDir.empty();
    bool doCoarse = true, doFine = wantFine;
    if (!tierArg.empty()) {
        if (tierArg == "coarse") {
            doFine = false;
        } else if (tierArg == "fine") {
            doCoarse = false;
            doFine = true;
        } else if (tierArg != "both") {
            std::fprintf(stderr, "--tier must be coarse, fine or both\n");
            return 2;
        }
    }
    if (doFine && !wantFine) {
        // Refuse rather than fall back: a run whose sidecars said "fine" while
        // it read the 30 m raster is exactly the mislabelled measurement this
        // whole exercise exists to prevent.
        std::fprintf(stderr, "--tier fine needs --fine-dir\n");
        return 2;
    }

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    // --- coarse tier ------------------------------------------------------
    TileGridSampler coarse(seed, 1);
    {
        int loaded = 0, rejected = 0;
        if (!std::filesystem::exists(coarseDir)) {
            std::fprintf(stderr, "--coarse-dir %s does not exist\n", coarseDir.c_str());
            return 1;
        }
        for (auto& e : std::filesystem::directory_iterator(coarseDir)) {
            if (e.path().extension() != ".vxtl") continue;
            if (coarse.loadTileFile(e.path()))
                ++loaded;
            else
                ++rejected;
        }
        std::printf("coarse tier: dir=%s loaded=%d rejected=%d pixelSizeMm=%d\n",
                    coarseDir.c_str(), loaded, rejected, coarse.pixelSizeMm());
        if (loaded == 0) {
            std::fprintf(stderr,
                         "no v1 tiles loaded from %s (a seed mismatch rejects every tile)\n",
                         coarseDir.c_str());
            return 1;
        }
    }

    // --- fine tier --------------------------------------------------------
    // climateSource = the coarse sampler: the fine tier carries elevation and
    // (optionally) flow, never climate, and column() classifies a biome.
    FineTileSampler fine(seed, &coarse);
    if (wantFine) {
        int loaded = 0, rejected = 0;
        if (std::filesystem::exists(fineDir)) {
            for (auto& e : std::filesystem::directory_iterator(fineDir)) {
                if (e.path().extension() != ".vxtl") continue;
                FineError err = FineError::kNone;
                if (fine.loadTileFile(e.path(), &err)) {
                    ++loaded;
                } else {
                    ++rejected;
                    std::fprintf(stderr, "  rejected %s: %s\n",
                                 e.path().filename().string().c_str(), fineErrorName(err));
                }
            }
        }
        std::printf("fine tier:   dir=%s loaded=%d rejected=%d pixelSizeMm=%d\n", fineDir.c_str(),
                    loaded, rejected, fine.pixelSizeMm());
        if (loaded == 0) {
            std::fprintf(stderr, "no v2 fine tiles loaded from %s; refusing to silently fall "
                                 "back to the coarse tier\n",
                         fineDir.c_str());
            return 1;
        }
    }

    Amplifier ampCoarse(seed, coarse);
    Amplifier ampFine(seed, fine);

    const int64_t ox = originXm * 1000, oy = originYm * 1000, spanMm = spanM * 1000;

    // Prewarm the fine sampler over the whole rectangle (plus the carrier's
    // 4x4 stencil dilation). FineTileSampler decodes blocks lazily and is NOT
    // safe to query concurrently until every touched block is resident; more
    // usefully here, prewarming makes a missing tile a loud failure at startup
    // instead of a silent plane of zeros in the middle of a dump.
    if (doFine) {
        const int64_t p = fine.pixelSizeMm();
        const bool ok = fine.prewarm(floorDiv(ox, p) - 2, floorDiv(oy, p) - 2,
                                     floorDiv(ox + spanMm, p) + 3, floorDiv(oy + spanMm, p) + 3);
        std::printf("fine tier:   prewarm %s, %zu blocks resident, %llu missing-tile queries\n",
                    ok ? "ok" : "INCOMPLETE", fine.residentBlockCount(),
                    (unsigned long long)fine.missingTileQueries.load());
        if (!ok)
            std::fprintf(stderr, "  WARNING: the rectangle is not fully covered by the loaded "
                                 "v2 tiles; the uncovered part reads 0 mm\n");
    }

    struct Written {
        std::string file, stage, tier;
        int64_t cellMm, n;
    };
    std::vector<Written> written;

    // --- the dump loop ----------------------------------------------------
    for (const CellSpec& cs : cells) {
        const int64_t latSpanMm = (cs.spanM > 0 ? cs.spanM * 1000 : spanMm);
        if (latSpanMm > spanMm) {
            std::fprintf(stderr, "--cell %lld:%lld — sub-span exceeds --span\n",
                         (long long)cs.cellMm, (long long)cs.spanM);
            return 2;
        }
        const int64_t n = latSpanMm / cs.cellMm + 1;
        if (n > maxN) {
            std::fprintf(stderr,
                         "--cell %lld over %lld m is %lld nodes per axis, above --max-n %lld. "
                         "Give the lattice its own sub-span (--cell %lld:SPAN_M) or raise "
                         "--max-n.\n",
                         (long long)cs.cellMm, (long long)(latSpanMm / 1000), (long long)n,
                         (long long)maxN, (long long)cs.cellMm);
            return 2;
        }
        // Centre a sub-span inside the footprint, on a whole number of cells so
        // the sub-lattice's nodes remain a subset of the full one's.
        const int64_t inset = ((spanMm - (n - 1) * cs.cellMm) / 2 / cs.cellMm) * cs.cellMm;
        Lattice lat{cs.cellMm, ox + inset, oy + inset, n};

        for (int tierIdx = 0; tierIdx < 2; ++tierIdx) {
            const bool isFine = tierIdx == 1;
            if (isFine && !doFine) continue;
            if (!isFine && !doCoarse) continue;
            ITileSampler& tiles = isFine ? static_cast<ITileSampler&>(fine)
                                         : static_cast<ITileSampler&>(coarse);
            Amplifier& amp = isFine ? ampFine : ampCoarse;
            const char* tierName = isFine ? "fine" : "coarse";

            const size_t cnt = static_cast<size_t>(n * n);
            std::vector<float> s2(cnt), s3(cnt), s4;
            if (doS4) s4.resize(cnt);
            DumpStats st2, st3, st4;
            st2.missingBefore = st3.missingBefore = st4.missingBefore =
                isFine ? fine.missingTileQueries.load() : coarse.missingTileQueries.load();

            int64_t snapMax = 0;
            long double snapAcc = 0;
            int64_t noSolid = 0, displaced = 0;

            std::printf("  %-6s cell %5lld mm  %lld^2 nodes  span %.1f m ...", tierName,
                        (long long)cs.cellMm, (long long)n, mm2m((n - 1) * cs.cellMm));
            std::fflush(stdout);

            for (int64_t j = 0; j < n; ++j) {
                const int64_t yMm = lat.y0Mm + j * lat.cellMm;
                const int64_t vy = floorDiv(yMm + kVoxelSizeMm / 2, kVoxelSizeMm);
                const int64_t dy = std::abs(yMm - vy * kVoxelSizeMm);
                for (int64_t i = 0; i < n; ++i) {
                    const int64_t xMm = lat.x0Mm + i * lat.cellMm;
                    const int64_t vx = floorDiv(xMm + kVoxelSizeMm / 2, kVoxelSizeMm);
                    const int64_t dx = std::abs(xMm - vx * kVoxelSizeMm);
                    const int64_t d = std::max(dx, dy);
                    if (d > snapMax) snapMax = d;
                    snapAcc += static_cast<long double>(d);

                    const size_t k = static_cast<size_t>(j * n + i);
                    s2[k] = static_cast<float>(mm2m(carrierHeightMm(tiles, vx, vy)));
                    if (doS4) {
                        const ColumnSample col = amp.column(vx, vy);
                        s3[k] = static_cast<float>(mm2m(col.surfaceMm));
                        int64_t vz = 0;
                        const int64_t topMm = topSolidTopFaceMm(col, vz);
                        if (topMm == kNoSolid) {
                            ++noSolid;
                            s4[k] = s3[k];
                        } else {
                            s4[k] = static_cast<float>(mm2m(topMm));
                            // Without the density band the topmost solid voxel
                            // is floorDiv(surfaceMm - 50, 100). Counting the
                            // columns where it is NOT says how much of S4 is a
                            // heightfield and how much is the 3D band.
                            const int64_t naive =
                                floorDiv(static_cast<int64_t>(col.surfaceMm) - kVoxelSizeMm / 2,
                                         kVoxelSizeMm);
                            if (vz != naive) ++displaced;
                        }
                    } else {
                        s3[k] = static_cast<float>(mm2m(amp.surfaceMm(vx, vy)));
                    }
                }
            }

            const uint64_t missAfter =
                isFine ? fine.missingTileQueries.load() : coarse.missingTileQueries.load();
            const double snapMean =
                static_cast<double>(snapAcc / static_cast<long double>(cnt));
            for (DumpStats* st : {&st2, &st3, &st4}) {
                st->snapMaxMm = snapMax;
                st->snapMeanMm = snapMean;
                st->missingAfter = missAfter;
            }
            st4.nInvalid = noSolid;
            st4.nDisplaced = displaced;
            finishStats(st2, s2);
            finishStats(st3, s3);
            if (doS4) finishStats(st4, s4);
            std::printf(" done (S3 %.1f..%.1f m)\n", st3.minM, st3.maxM);

            struct Job {
                const char* stage;
                const char* name;
                const char* src;
                const char* quant;
                const std::vector<float>* data;
                const DumpStats* st;
                bool on;
            };
            const Job jobs[3] = {
                {"S2", "client carrier only (C2 B-spline over the tile raster; every detail "
                       "octave and additive term OFF)",
                 "voxelcore/carrier.h evalCarrier() over ITileSampler::elevationMm — the same "
                 "function and stencil amplifier.cpp evalSurface() calls",
                 "", &s2, &st2, true},
                {"S3", "client full continuous surface, before voxelisation",
                 "Amplifier::surfaceMm (via Amplifier::column, bit-identical)", "", &s3, &st3,
                 true},
                {"S4", "voxelised surface: topmost solid voxel as Amplifier::stratigraphyAt "
                       "sees it",
                 "Amplifier::stratigraphyAt scanned down from surfaceMm + the density3 "
                 "envelope; cave/cavern carving deliberately NOT applied",
                 "TOP FACE of the topmost non-air voxel, (vz+1)*100 mm; subtract 0.1 for the "
                 "bottom face",
                 &s4, &st4, doS4},
            };

            for (const Job& jb : jobs) {
                if (!jb.on) continue;
                char base[128];
                std::snprintf(base, sizeof(base), "%s_%s_%lldmm", jb.stage, tierName,
                              (long long)cs.cellMm);
                const std::filesystem::path npy =
                    std::filesystem::path(outDir) / (std::string(base) + ".npy");
                const std::filesystem::path js =
                    std::filesystem::path(outDir) / (std::string(base) + ".json");
                if (!writeNpyF32(npy, *jb.data, n, n)) {
                    std::fprintf(stderr, "failed to write %s\n", npy.string().c_str());
                    return 1;
                }
                Sidecar sc;
                sc.stage = jb.stage;
                sc.name = jb.name;
                sc.tier = tierName;
                sc.source = jb.src;
                sc.coarseDir = coarseDir;
                sc.fineDir = isFine ? fineDir : std::string();
                sc.seed = seed;
                sc.pixelSizeMm = tiles.pixelSizeMm();
                sc.lat = lat;
                sc.st = *jb.st;
                sc.quantNote = jb.quant;
                if (!writeSidecar(js, sc, std::string(base) + ".npy")) {
                    std::fprintf(stderr, "failed to write %s\n", js.string().c_str());
                    return 1;
                }
                written.push_back(Written{std::string(base), jb.stage, tierName, cs.cellMm, n});
            }
        }
    }

    // A manifest so a consumer can enumerate a run without globbing, and so the
    // footprint every file shares is written down exactly once.
    {
        const std::filesystem::path mp = std::filesystem::path(outDir) / "manifest_client.json";
        FILE* f = std::fopen(mp.string().c_str(), "wb");
        if (f) {
            std::fprintf(f, "{\n  \"schema\": \"vxc.stagedump.manifest.v1\",\n");
            std::fprintf(f, "  \"producer\": \"vxc_stagedump\",\n");
            std::fprintf(f, "  \"seed\": %llu,\n", (unsigned long long)seed);
            std::fprintf(f, "  \"worldgen_version\": %u,\n", kWorldGenVersion);
            std::fprintf(f, "  \"footprint_origin_m\": [%lld, %lld],\n", (long long)originXm,
                         (long long)originYm);
            std::fprintf(f, "  \"footprint_span_m\": %lld,\n", (long long)spanM);
            std::fprintf(f, "  \"coarse_tile_dir\": \"%s\",\n", jsonEscape(coarseDir).c_str());
            std::fprintf(f, "  \"fine_tile_dir\": \"%s\",\n", jsonEscape(fineDir).c_str());
            std::fprintf(f, "  \"files\": [\n");
            for (size_t i = 0; i < written.size(); ++i) {
                const Written& w = written[i];
                std::fprintf(f,
                             "    {\"base\": \"%s\", \"stage\": \"%s\", \"tier\": \"%s\", "
                             "\"cell_size_mm\": %lld, \"n\": %lld}%s\n",
                             w.file.c_str(), w.stage.c_str(), w.tier.c_str(), (long long)w.cellMm,
                             (long long)w.n, i + 1 < written.size() ? "," : "");
            }
            std::fprintf(f, "  ]\n}\n");
            std::fclose(f);
        }
    }

    std::printf("\nwrote %zu heightfield(s) + sidecars to %s\n", written.size(), outDir.c_str());
    return 0;
}
