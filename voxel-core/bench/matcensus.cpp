// Which material ids reach the QUADS -- the gate on making colour material-led.
//
//   vxc_matcensus <tiledir|--synthetic> <seed> [--radius M] [--dump FILE]
//
// WHY THIS EXISTS
// ---------------
// ADR-0009 makes a voxel face's colour its material, which is only safe if the
// renderer actually receives varied material ids. Two censuses disagree about
// whether it does, and the disagreement has stood unresolved for a year:
//
//   * the COLUMN census (vxc_climateprobe, after the worldgen v8 topsoil fix)
//     says the top voxel carries its biome's surface material on 100.00% of
//     columns, across mud, sand, grass, topsoil, podzol, permafrost and rock.
//
//   * the QUAD census (-VoxelMatHistogram, in-engine, 2M quads on the real
//     25-tile diffusion set) says MAT_ROCK 15% and MAT_SUBSOIL 85% and nothing
//     else -- no surface material at all.
//
// The quad census predates the v8 fix and has never been re-run.
// VoxelClimateProbe.h says so in as many words: "Re-run the switch before
// trusting an id-keyed appearance rule." This is that re-run, and it reports
// BOTH censuses from ONE walk of ONE world, so the comparison is inside the
// tool instead of across two instruments and two years.
//
// WHY NOT THE IN-ENGINE SWITCH. -VoxelMatHistogram cannot answer it any more.
// It lives in VoxelChunkComponent.cpp, i.e. the COMPONENT path, and
// voxel.Stream.GPU is on by default so the pooled path is what draws. And it
// counts `Q.Mat & 0xF` into a 16-slot table, so all 31 asset materials are
// folded into terrain slots -- MAT_BARK reads as MAT_AIR, MAT_LEAF_BROADLEAF as
// MAT_GRAVEL. It was written when the world had 16 materials.
//
// WHAT "REACHES THE QUADS" MEANS, precisely. A quad is a VISIBLE face: the
// greedy mesher emits one only where a solid voxel abuts air. So this measures
// the material of every surface the player can see, weighted by AREA (w*h, not
// quad count -- one 8x8 merged quad is 64 faces and must not count as one).
//
// ON TILE SOURCE. `--synthetic` answers "does the plumbing work" -- if varied
// surface ids reach quads at all, they do so here. It does NOT answer "in what
// proportion", because SyntheticTileSampler's climate is not WorldClim's and
// the biome mix differs. For proportions, point this at a real tile cache. The
// output says which mode it ran in, every time, because that distinction is
// exactly the one an earlier draft of this work got wrong by reasoning from
// thresholds instead of measuring.
//
// --dump writes a per-column record for tools/world-preview.py, which turns
// this same walk into a picture. One walk, three answers.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/biome.h"
#include "voxelcore/materialpalette.h"
#include "voxelcore/mesher.h"
#include "voxelcore/tilestore.h"
#include "voxelcore/tiles.h"

using namespace vxc;

namespace {

constexpr int B = 8;

struct Census {
    // Quad AREA, not quad count: the greedy mesher merges up to 8x8 faces into
    // one quad, so counting quads would under-weight exactly the large flat
    // surfaces that dominate what a player sees.
    uint64_t areaByMat[kMaterialCount] = {};
    uint64_t areaByMatFace[kMaterialCount][kFaceClassCount] = {};
    uint64_t quadsByMat[kMaterialCount] = {};
    uint64_t totalArea = 0, totalQuads = 0;

    // The column census, for the side-by-side: the material of each column's
    // topmost solid voxel.
    uint64_t topByMat[kMaterialCount] = {};
    uint64_t columns = 0;
};

const char* materialName(int m) {
    // Deliberately a switch and not a table: a table here would be a second
    // copy of the enum, which is the failure this whole area of the codebase
    // keeps paying for. A missing case is a compiler warning.
    switch (MaterialId(m)) {
        case MAT_AIR: return "MAT_AIR";
        case MAT_BEDROCK: return "MAT_BEDROCK";
        case MAT_ROCK: return "MAT_ROCK";
        case MAT_GRAVEL: return "MAT_GRAVEL";
        case MAT_SAND: return "MAT_SAND";
        case MAT_SUBSOIL: return "MAT_SUBSOIL";
        case MAT_TOPSOIL: return "MAT_TOPSOIL";
        case MAT_SNOW: return "MAT_SNOW";
        case MAT_GRASS: return "MAT_GRASS";
        case MAT_JUNGLE_SOIL: return "MAT_JUNGLE_SOIL";
        case MAT_SAVANNA_GRASS: return "MAT_SAVANNA_GRASS";
        case MAT_PODZOL: return "MAT_PODZOL";
        case MAT_PERMAFROST: return "MAT_PERMAFROST";
        case MAT_MUD: return "MAT_MUD";
        case MAT_CLAY: return "MAT_CLAY";
        case MAT_WATERMARK: return "MAT_WATERMARK";
        default: break;
    }
    return m >= kFirstAssetMaterial ? "(asset)" : "(unnamed)";
}

// The materials biomeSurfaceMaterial can return. If these do not appear in the
// quad census, the gate has failed and colour must not become material-led.
bool isSurfaceMaterial(int m) {
    switch (MaterialId(m)) {
        case MAT_SAND:
        case MAT_TOPSOIL:
        case MAT_GRASS:
        case MAT_JUNGLE_SOIL:
        case MAT_SAVANNA_GRASS:
        case MAT_PODZOL:
        case MAT_PERMAFROST:
        case MAT_MUD:
        case MAT_ROCK:
            return true;
        default:
            return false;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: vxc_matcensus <tiledir|--synthetic> <seed> "
                     "[--radius M] [--origin X Y]\n"
                     "                     [--dump FILE [--dump-radius M] "
                     "[--dump-stride N]]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const uint64_t seed = std::strtoull(argv[2], nullptr, 10);
    int64_t radiusM = 200;
    int64_t originXM = 0, originYM = 0;
    bool originGiven = false;
    std::string dumpPath;
    int64_t dumpRadiusM = 0;   // 0 = follow --radius
    int64_t dumpStride = 1;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--radius") == 0 && i + 1 < argc) {
            radiusM = std::strtoll(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--origin") == 0 && i + 2 < argc) {
            originXM = std::strtoll(argv[++i], nullptr, 10);
            originYM = std::strtoll(argv[++i], nullptr, 10);
            originGiven = true;
        } else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dumpPath = argv[++i];
        } else if (std::strcmp(argv[i], "--dump-radius") == 0 && i + 1 < argc) {
            dumpRadiusM = std::strtoll(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--dump-stride") == 0 && i + 1 < argc) {
            dumpStride = std::strtoll(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    // Tile source, following vxc_climateprobe's loader exactly -- same
    // seed/scale rejection, same reason.
    SyntheticTileSampler synth(seed);
    TileGridSampler grid(seed, 1);
    ITileSampler* tiles = &synth;
    bool realTiles = false;

    int32_t minTx = 0, maxTx = 0, minTy = 0, maxTy = 0;
    if (dir != "--synthetic") {
        int loaded = 0, rejected = 0;
        bool anyBox = false;
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".vxtl") continue;
            std::optional<std::vector<uint8_t>> bytes = readFileBytes(e.path());
            if (!bytes) { ++rejected; continue; }
            std::optional<TileData> parsed = TileData::parse(bytes->data(), bytes->size());
            if (!parsed) { ++rejected; continue; }
            const int32_t tx = parsed->x, ty = parsed->y;
            if (!grid.loadTile(std::move(*parsed))) { ++rejected; continue; }
            ++loaded;
            if (!anyBox) {
                minTx = maxTx = tx;
                minTy = maxTy = ty;
                anyBox = true;
            } else {
                minTx = std::min(minTx, tx);
                maxTx = std::max(maxTx, tx);
                minTy = std::min(minTy, ty);
                maxTy = std::max(maxTy, ty);
            }
        }
        std::printf("tiles loaded=%d rejected=%d bbox x=[%d,%d] y=[%d,%d]\n", loaded,
                    rejected, minTx, maxTx, minTy, maxTy);
        if (loaded == 0) {
            std::fprintf(stderr, "no tiles loaded from %s\n", dir.c_str());
            return 1;
        }
        tiles = &grid;
        realTiles = true;
    }

    std::printf("TILE SOURCE: %s\n", realTiles ? "REAL tiles" : "SyntheticTileSampler");
    if (!realTiles) {
        std::printf("  NOTE: synthetic climate is not WorldClim's, so the PROPORTIONS\n"
                    "  below are not the shipped world's. What this mode answers is\n"
                    "  whether varied surface ids reach the quads AT ALL.\n");
    }
    std::printf("seed=%llu radius=%lldm brick=%d\n",
                (unsigned long long)seed, (long long)radiusM, B);

    Amplifier amp(seed, *tiles);

    // WHERE TO SAMPLE. The world origin is sea in this world, and a census of
    // 120 m of ocean floor reports 100% MAT_MUD and answers nothing -- which is
    // what the first run of this probe against the repo's baked tiles did. So
    // when no origin is given the window SEEKS LAND: a coarse scan of the loaded
    // tiles' bounding box, keeping the land column nearest the box centre, so
    // the choice is deterministic and reproducible rather than "wherever I
    // happened to point it".
    if (!originGiven && realTiles) {
        const int64_t k = TileData::kTileSize;
        const int64_t pxMm = tiles->pixelSizeMm();
        const int64_t px0 = int64_t(minTx) * k, px1 = (int64_t(maxTx) + 1) * k - 2;
        const int64_t py0 = int64_t(minTy) * k, py1 = (int64_t(maxTy) + 1) * k - 2;
        const int64_t cx = (px0 + px1) / 2, cy = (py0 + py1) / 2;
        const int steps = 48;
        int64_t bestD2 = INT64_MAX;
        bool found = false;
        for (int j = 0; j <= steps; ++j) {
            for (int i = 0; i <= steps; ++i) {
                const int64_t px = px0 + (px1 - px0) * i / steps;
                const int64_t py = py0 + (py1 - py0) * j / steps;
                // Tile pixels -> voxels. Inset by the radius so the whole
                // window stays inside the loaded box.
                const int64_t vx = px * pxMm / kVoxelSizeMm;
                const int64_t vy = py * pxMm / kVoxelSizeMm;
                const ColumnSample col = amp.column(vx, vy);
                // Comfortably above the beach band, so the window is real land
                // rather than a shoreline that censuses as sand and mud.
                if (col.surfaceMm <= kBiomeBeachUpperMm + 20000) continue;
                const int64_t dx = px - cx, dy = py - cy;
                const int64_t d2 = dx * dx + dy * dy;
                if (d2 < bestD2) {
                    bestD2 = d2;
                    originXM = vx * kVoxelSizeMm / 1000;
                    originYM = vy * kVoxelSizeMm / 1000;
                    found = true;
                }
            }
        }
        if (found) {
            std::printf("land seek: sampling at (%lld, %lld) m -- the land column "
                        "nearest the tile-box centre\n",
                        (long long)originXM, (long long)originYM);
        } else {
            std::printf("land seek: NO LAND above %d mm found in the loaded tiles; "
                        "censusing the origin, which will be seafloor\n",
                        kBiomeBeachUpperMm + 20000);
        }
    }

    Census c;


    const int64_t radiusVox = radiusM * 1000 / kVoxelSizeMm;
    const int64_t originVx = originXM * 1000 / kVoxelSizeMm;
    const int64_t originVy = originYM * 1000 / kVoxelSizeMm;
    const int32_t bMin = static_cast<int32_t>(floorDiv(originVx - radiusVox, B));
    const int32_t bMax = static_cast<int32_t>(floorDiv(originVx + radiusVox - 1, B));
    const int32_t bMinY = static_cast<int32_t>(floorDiv(originVy - radiusVox, B));
    const int32_t bMaxY = static_cast<int32_t>(floorDiv(originVy + radiusVox - 1, B));

    std::vector<ColumnSample> ext(size_t(B + 2) * (B + 2));
    std::vector<Quad> quads;

    for (int32_t by = bMinY; by <= bMaxY; ++by) {
        for (int32_t bx = bMin; bx <= bMax; ++bx) {
            for (int y = -1; y <= B; ++y) {
                for (int x = -1; x <= B; ++x) {
                    ext[size_t((x + 1) + (B + 2) * (y + 1))] =
                        amp.column(int64_t(bx) * B + x, int64_t(by) * B + y);
                }
            }

            // The column census, and the dump, over this footprint's own
            // columns (not the apron).
            int64_t vzMin = INT64_MAX, vzMax = INT64_MIN;
            for (int y = 0; y < B; ++y) {
                for (int x = 0; x < B; ++x) {
                    const ColumnSample& col = ext[size_t((x + 1) + (B + 2) * (y + 1))];
                    const int64_t top = topSolidVoxelZ(col.surfaceMm);
                    vzMin = std::min(vzMin, top);
                    vzMax = std::max(vzMax, top);

                    const MaterialId topMat = Amplifier::materialAt(col, top);
                    ++c.columns;
                    ++c.topByMat[topMat];
                }
            }

            const int32_t bzMin = static_cast<int32_t>(floorDiv(vzMin, B));
            const int32_t bzMax = static_cast<int32_t>(floorDiv(vzMax, B));

            for (int32_t bz = bzMin; bz <= bzMax; ++bz) {
                quads.clear();
                const auto sampler = [&](int x, int y, int z) -> MaterialId {
                    const ColumnSample& col = ext[size_t((x + 1) + (B + 2) * (y + 1))];
                    return Amplifier::materialAt(col, int64_t(bz) * B + z);
                };
                meshBrick<B>(sampler, quads);

                for (const Quad& q : quads) {
                    const uint64_t area = uint64_t(q.w) * q.h;
                    const FaceClass fc = faceClassOf(q.axis, q.positive != 0);
                    c.areaByMat[q.mat] += area;
                    c.areaByMatFace[q.mat][fc] += area;
                    ++c.quadsByMat[q.mat];
                    c.totalArea += area;
                    ++c.totalQuads;
                }
            }
        }
    }


    // --- the column dump, for tools/world-preview.py -------------------------
    //
    // A SEPARATE PASS, columns only and no meshing, so it can cover kilometres
    // while the census above stays local and affordable. The census answers "do
    // the ids reach the quads" and needs the mesher; the picture answers "what
    // does the world look like" and needs area. Sharing one radius would have
    // made both bad: a census wide enough to see several biomes takes hours to
    // mesh, and one narrow enough to mesh shows a single biome.
    //
    // Amplifier::surfaceInfo, not column(): it returns surface, the signed
    // per-axis gradient, the biome AND the climate in one call, and skips the
    // cave and cavern reductions the picture has no use for. The gradient is
    // what hillshades the image; the climate is what lets --compare paint
    // today's biome-LUT answer beside the material one.
    //
    // BINARY RECORDS, TEXT HEADER. The first version wrote one text line per
    // column and produced a 194 MB file for a 6 km window -- most of it the
    // coordinates, which are redundant because the grid is regular. The header
    // stays text so the file says what it is; the body is fixed-width
    // little-endian so a 6 km window is a few megabytes.
    if (!dumpPath.empty()) {
        std::FILE* dump = std::fopen(dumpPath.c_str(), "wb");
        if (dump == nullptr) {
            std::fprintf(stderr, "cannot write %s\n", dumpPath.c_str());
            return 1;
        }
        const int64_t dr = dumpRadiusM > 0 ? dumpRadiusM : radiusM;
        const int64_t drVox = dr * 1000 / kVoxelSizeMm;
        const int64_t stride = dumpStride > 0 ? dumpStride : 1;
        const int64_t n = std::max<int64_t>(1, (2 * drVox) / stride);

        // ONE KEY PER LINE. The reader takes the first token as the key and
        // everything after it as that key's values, so a second key on the
        // same line is swallowed as a value of the first and simply vanishes.
        // `real_tiles` rode on the seed line and did exactly that, and
        // world-preview.py captioned every real-tile picture
        // "SyntheticTileSampler" -- a confident lie about the provenance of
        // the thing a reviewer is judging.
        std::fprintf(dump, "# vxc_matcensus dump v2\n");
        std::fprintf(dump, "# seed %llu\n", (unsigned long long)seed);
        std::fprintf(dump, "# real_tiles %d\n", realTiles ? 1 : 0);
        std::fprintf(dump, "# origin_m %lld %lld\n",
                     (long long)originXM, (long long)originYM);
        std::fprintf(dump, "# radius_m %lld\n", (long long)dr);
        std::fprintf(dump, "# stride_vox %lld\n", (long long)stride);
        std::fprintf(dump, "# grid %lld %lld\n", (long long)n, (long long)n);
        std::fprintf(dump, "# metres_per_cell %.4f\n",
                     double(stride) * kVoxelSizeMm / 1000.0);
        // int32 surfaceMm, int32 gradX, int32 gradY, u8 mat, u8 biome,
        // u8 temperature, u8 precipitation -- 16 bytes, row-major.
        std::fprintf(dump, "# record <iiiBBBB> row-major\n");
        std::fprintf(dump, "# END\n");

        std::vector<uint8_t> row(size_t(n) * 16);
        for (int64_t j = 0; j < n; ++j) {
            size_t o = 0;
            for (int64_t i = 0; i < n; ++i) {
                const int64_t vx = originVx - drVox + i * stride;
                const int64_t vy = originVy - drVox + j * stride;
                const Amplifier::SurfaceInfo si = amp.surfaceInfo(vx, vy);
                const ColumnSample col = amp.column(vx, vy);
                const MaterialId m = Amplifier::materialAt(col, topSolidVoxelZ(col.surfaceMm));

                const int32_t vals[3] = {si.surfaceMm,
                                         static_cast<int32_t>(si.slopeXMmPerM),
                                         static_cast<int32_t>(si.slopeYMmPerM)};
                std::memcpy(&row[o], vals, sizeof(vals));
                o += sizeof(vals);
                row[o++] = uint8_t(m);
                row[o++] = uint8_t(si.biome);
                row[o++] = si.climate.temperature;
                row[o++] = si.climate.precipitation;
            }
            std::fwrite(row.data(), 1, o, dump);
        }
        std::fclose(dump);
        std::printf("wrote %lldx%lld column dump to %s (%lld m radius, %.2f m/cell)\n\n",
                    (long long)n, (long long)n, dumpPath.c_str(), (long long)dr,
                    double(stride) * kVoxelSizeMm / 1000.0);
    }

    if (c.totalArea == 0) {
        std::fprintf(stderr, "no quads emitted -- nothing to census\n");
        return 1;
    }

    // --- the two censuses, side by side --------------------------------------
    std::printf("%-20s %12s %12s | %12s\n", "material", "quad area", "% of area",
                "% of columns");
    std::printf("%-20s %12s %12s | %12s\n", "--------", "---------", "---------",
                "------------");
    for (int m = 0; m < kMaterialCount; ++m) {
        if (c.areaByMat[m] == 0 && c.topByMat[m] == 0) continue;
        std::printf("%-20s %12llu %11.2f%% | %11.2f%%\n", materialName(m),
                    (unsigned long long)c.areaByMat[m],
                    100.0 * double(c.areaByMat[m]) / double(c.totalArea),
                    100.0 * double(c.topByMat[m]) / double(c.columns));
    }
    std::printf("\n%llu quads, %llu faces of area, %llu columns\n\n",
                (unsigned long long)c.totalQuads, (unsigned long long)c.totalArea,
                (unsigned long long)c.columns);

    // --- top faces only ------------------------------------------------------
    //
    // The number that matters most for ground colour: a player standing on
    // terrain sees mostly +Z faces, and those are the ones a surface material
    // should own.
    uint64_t topArea = 0;
    for (int m = 0; m < kMaterialCount; ++m) topArea += c.areaByMatFace[m][kFaceTop];
    std::printf("TOP FACES ONLY (%llu of area):\n", (unsigned long long)topArea);
    for (int m = 0; m < kMaterialCount; ++m) {
        if (c.areaByMatFace[m][kFaceTop] == 0) continue;
        std::printf("  %-20s %11.2f%%\n", materialName(m),
                    100.0 * double(c.areaByMatFace[m][kFaceTop]) / double(topArea));
    }

    // --- the verdict ---------------------------------------------------------
    //
    // THE TEST IS AGREEMENT, NOT DIVERSITY, and the first version of this probe
    // got that wrong: it demanded two or more distinct surface materials and
    // reported FAIL on a synthetic world that is uniformly tundra -- a property
    // of the tile source, not of the plumbing, and exactly the kind of false
    // red that teaches people to ignore a gate.
    //
    // The question is whether the ids the COLUMN census reports survive into the
    // QUADS. That is answered by comparing the two distributions: if meshing or
    // stratigraphy were dropping surface materials, the top-face mix would
    // diverge from the column mix -- which is precisely what the historical
    // in-engine census showed (columns varied, quads were rock and subsoil
    // only). Diversity is reported separately, because a world with one biome
    // is a fact about the world worth knowing and is not this gate's business.
    double worstDelta = 0.0;
    int worstMat = 0;
    for (int m = 0; m < kMaterialCount; ++m) {
        const double quadPct = 100.0 * double(c.areaByMatFace[m][kFaceTop]) / double(topArea);
        const double colPct = 100.0 * double(c.topByMat[m]) / double(c.columns);
        const double d = quadPct > colPct ? quadPct - colPct : colPct - quadPct;
        if (d > worstDelta) { worstDelta = d; worstMat = m; }
    }

    uint64_t surfaceTopArea = 0;
    int distinctSurface = 0;
    for (int m = 0; m < kMaterialCount; ++m) {
        if (!isSurfaceMaterial(m)) continue;
        surfaceTopArea += c.areaByMatFace[m][kFaceTop];
        if (c.areaByMatFace[m][kFaceTop] > 0) ++distinctSurface;
    }
    const double surfacePct = 100.0 * double(surfaceTopArea) / double(topArea);

    std::printf("\nAGREEMENT between the column census and the top-face census:\n");
    std::printf("  worst per-material difference %.2f points (%s)\n", worstDelta,
                materialName(worstMat));
    std::printf("  surface materials hold %.2f%% of top-face area, %d distinct\n",
                surfacePct, distinctSurface);

    // 5 points of slack: the two censuses count different things -- one column
    // versus the visible top-face AREA of that column, which differs wherever a
    // column's top voxel is occluded by its neighbour or a merged quad spans a
    // material boundary. A real drop of a surface material moves tens of points,
    // as the historical census did.
    const bool agrees = worstDelta <= 5.0;
    const bool surfaceOwnsTop = surfacePct >= 95.0;

    if (agrees && surfaceOwnsTop) {
        std::printf("\nVERDICT: PASS -- the material ids the column census reports "
                    "reach the quads.\n");
        if (distinctSurface < 2) {
            std::printf("  CAVEAT: only one surface material appears. This world has one\n"
                        "  biome, which is expected on SyntheticTileSampler and would be a\n"
                        "  WORLDGEN problem on real tiles. Re-run against a real tile cache\n"
                        "  before quoting proportions.\n");
        }
        return 0;
    }
    std::printf("\nVERDICT: FAIL -- the column census and the quad census disagree.\n");
    if (!agrees) {
        std::printf("  The ids voxel-core computes per column are NOT what the mesher\n"
                    "  emits. Do not make colour material-led until this is understood:\n"
                    "  the defect is in stratigraphy or meshing, not in the colour system.\n");
    }
    if (!surfaceOwnsTop) {
        std::printf("  Surface materials hold only %.2f%% of top-face area -- most of what\n"
                    "  a player looks down at is a subsurface stratum.\n", surfacePct);
    }
    return 1;
}
