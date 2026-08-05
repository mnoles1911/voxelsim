// vxc_farwaterenum -- does the far-field candidate source actually deliver the
// property the cascade is built on?
//
// `farwater.h` argues the cascade is affordable. `farwaterenum.h` is the
// enumeration that has to hold up for that argument to mean anything, and it
// rests on ONE claim: that most of the baked water plane can be rejected out of
// the block index, without fetching a byte. If that claim is false the cascade
// dies here, before any UE binding, which is the cheapest place for it to die.
//
// THIS PROBE MEASURES WET COUNTRY, NOT THE ARID CORRIDOR. Every water number in
// this project up to now came from four dry tiles, and a scheme sized on them
// is sized on the easy case. The six tiles this is meant to run on carry 0.560%
// wet fine pixels, a largest connected component spanning 14.63 km, 1,365
// reaches and 425 km of channel -- a far heavier load, and the one that decides
// whether the cascade ships.
//
// What it prints, in order:
//   1. the REALISED index-only rejection rate over the cascade's own window
//      (not the plane-wide census, which is the flattering number)
//   2. the disjointness count for the plane/basin union -- must be 0, or every
//      cost below is inflated by a double count
//   3. expensive resolves, per ring, against the estimate
//   4. bricks, quads and bytes for the cascade at each radius, against what
//      today's 25.6 m box draws at the same camera
//   5. the worst case over every site, because a scheme that halves the average
//      and keeps the spike has not fixed anything

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/farwater.h"
#include "voxelcore/farwaterenum.h"
#include "voxelcore/lakes.h"
#include "voxelcore/mesher.h"
#include "voxelcore/tilestore.h"

using namespace vxc;

namespace {

std::optional<std::vector<uint8_t>> readBytes(const std::filesystem::path& p) {
    std::FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f) return std::nullopt;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> b(size_t(n < 0 ? 0 : n));
    const size_t got = b.empty() ? 0 : std::fread(b.data(), 1, b.size(), f);
    std::fclose(f);
    if (got != b.size()) return std::nullopt;
    return b;
}

// 4 verts x 32 B + 6 indices x 4 B, the shape UWaterChunkComponent uploads.
// Quoted as a scale, not a promise -- the same constant vxc_farwaterprobe uses,
// so the two probes' MB columns are comparable.
double megabytes(uint64_t quads) { return double(quads) * 152.0 / (1024.0 * 1024.0); }

// THE CASCADE'S BASE, IN LOD-0 BRICKS, AND WHY IT IS NOT THE PROBE'S 32 m.
//
// 32 bricks is 25.6 m, exactly today's near-field box, so ring 1 starts where
// LOD 0 already ends and nothing about the near field moves. Taking the
// recommended 32 m instead needs kImplicitRadiusBricks to grow 32 -> 40, i.e.
// 81x81 columns against 65x65 = 1.55x the near-field window area -- and
// docs/near-water-refresh-findings.md measures the near field at 0.92x drain
// capacity WITH the ground floor, the only configuration in which the water
// disc ever finishes building. 1.55 x 0.92 = 1.43x is back over capacity.
constexpr int64_t kBaseBricks = 32;

// SQUARE (Chebyshev) rings, because the near-field window is a square and the
// two must nest without gap or overlap. A circular inner cut at radius 32
// double-covers the box's corners (two levels drawing the same water); a
// circular cut at the corner radius 32*sqrt(2) leaves a 10.6 m annulus on the
// axes that no level draws. Concentric squares have neither failure.
int64_t chebyshevBricks(int64_t bx, int64_t by, int64_t cx, int64_t cy) {
    const int64_t dx = bx > cx ? bx - cx : cx - bx;
    const int64_t dy = by > cy ? by - cy : cy - by;
    return dx > dy ? dx : dy;
}

struct RingCost {
    uint64_t coarseCols = 0;
    uint64_t waterBricks = 0;
    uint64_t surfBricks = 0;
    uint64_t quads = 0;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: vxc_farwaterenum <tiledir> [--at Xm Ym] [--radii \"100,500,1000\"] "
                     "[--sites N] [--maxlod N]\n");
        return 2;
    }
    std::string fineDir = argv[1], radiiArg = "100,500,1000";
    double atXm = 0.0, atYm = 0.0;
    bool haveAt = false;
    int sites = 1, maxLod = kFarWaterMaxLod;
    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--at") && i + 2 < argc) {
            atXm = std::strtod(argv[i + 1], nullptr);
            atYm = std::strtod(argv[i + 2], nullptr);
            haveAt = true;
            i += 2;
        } else if (!std::strcmp(a, "--radii") && i + 1 < argc) {
            radiiArg = argv[++i];
        } else if (!std::strcmp(a, "--sites") && i + 1 < argc) {
            sites = std::atoi(argv[++i]);
        } else if (!std::strcmp(a, "--maxlod") && i + 1 < argc) {
            maxLod = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a);
            return 2;
        }
    }
    if (maxLod < 0) maxLod = 0;
    if (maxLod > kFarWaterMaxLod) maxLod = kFarWaterMaxLod;
    if (sites < 1) sites = 1;

    std::vector<double> radii;
    for (const char* s = radiiArg.c_str(); *s;) {
        char* end = nullptr;
        const double v = std::strtod(s, &end);
        if (end == s) break;
        radii.push_back(v);
        s = end;
        while (*s == ',' || *s == ' ') ++s;
    }
    std::sort(radii.begin(), radii.end());
    if (radii.empty()) {
        std::fprintf(stderr, "no radii\n");
        return 2;
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

    const int64_t edge = int64_t(WaterBrick8::kEdge);
    const int64_t brickMm = edge * int64_t(kVoxelSizeMm);

    // ---------------------------------------------------------------------
    // CAMERA SITES. The wettest water blocks, so the measurement lands on the
    // load rather than beside it -- a camera in a dry corner would report a
    // cascade that costs nothing and prove nothing.
    // ---------------------------------------------------------------------
    struct Site {
        int64_t cxMm, cyMm;
        uint64_t wet;
    };
    std::vector<Site> siteList;
    if (haveAt) {
        siteList.push_back(Site{int64_t(atXm * 1000.0), int64_t(atYm * 1000.0), 0});
    } else {
        std::vector<int16_t> depth;
        for (const auto& tc : loaded) {
            const FineTile* t = fine.findTile(tc.first, tc.second);
            if (!t || !t->hasWater()) continue;
            const uint32_t perAxis = t->blocksPerAxis();
            const int64_t dim = int64_t(t->blockDim());
            const std::vector<FineBlockEntry>& idx = t->waterIndex();
            for (uint32_t by = 0; by < perAxis; ++by) {
                for (uint32_t bx = 0; bx < perAxis; ++bx) {
                    const size_t i = size_t(by) * perAxis + bx;
                    if (i >= idx.size()) continue;
                    uint64_t wet = 0;
                    if (idx[i].mode == kBlockConstant) {
                        if (idx[i].constCp < 0) continue;
                        wet = uint64_t(dim) * uint64_t(dim);
                    } else {
                        if (!t->decodeWaterBlock(bx, by, depth)) continue;
                        if (depth.size() < size_t(dim) * size_t(dim)) continue;
                        for (size_t k = 0; k < size_t(dim) * size_t(dim); ++k)
                            if (depth[k] >= 0) ++wet;
                        if (wet == 0) continue;
                    }
                    const int64_t opx = int64_t(tc.first) * tileSize + int64_t(bx) * dim;
                    const int64_t opy = int64_t(tc.second) * tileSize + int64_t(by) * dim;
                    siteList.push_back(Site{(opx + dim / 2) * pxMm, (opy + dim / 2) * pxMm, wet});
                }
            }
        }
        std::sort(siteList.begin(), siteList.end(),
                  [](const Site& a, const Site& b) { return a.wet > b.wet; });
        if (siteList.empty()) {
            std::fprintf(stderr, "no wet blocks; nothing to measure\n");
            return 1;
        }
        if (int(siteList.size()) > sites) siteList.resize(size_t(sites));
    }

    const double maxR = radii.back();
    // The cascade's own outer radius, which is what the enumeration window has
    // to cover regardless of what the caller asked to report at.
    const double cascadeOuterM = double(farWaterOuterBricks(kBaseBricks, maxLod) * brickMm) / 1000.0;
    std::printf("\ncascade: base %.1f m, maxLod %d -> outer %.1f m on the axes (%.1f m on the diagonal)\n",
                double(kBaseBricks * brickMm) / 1000.0, maxLod, cascadeOuterM, cascadeOuterM * 1.41421356);

    // Worst case across sites, for every reported radius.
    struct Worst {
        uint64_t cascadeBricks = 0, cascadeQuads = 0, todayBricks = 0, todayQuads = 0;
        double atX = 0, atY = 0;
    };
    std::vector<Worst> worst(radii.size());
    FarWaterEnumStats totalStats;

    for (size_t si = 0; si < siteList.size(); ++si) {
        const Site& s = siteList[si];
        const int64_t camBx = floorDiv(s.cxMm, brickMm), camBy = floorDiv(s.cyMm, brickMm);
        std::printf("\n=====================================================================\n");
        std::printf("SITE %zu of %zu -- camera %.1f, %.1f m (brick %lld,%lld)\n", si + 1,
                    siteList.size(), double(s.cxMm) / 1000.0, double(s.cyMm) / 1000.0,
                    (long long)camBx, (long long)camBy);

        // The grid must cover the larger of the cascade's outer radius and the
        // largest radius asked for, plus an apron: the interior test reads a
        // one-COARSE-column ring, which at maxLod is 2^maxLod LOD-0 columns.
        const int64_t needB =
            std::max<int64_t>(farWaterOuterBricks(kBaseBricks, maxLod),
                              int64_t(std::ceil(maxR * 1000.0 / double(brickMm))));
        const int64_t apron = (int64_t(1) << maxLod) * 2;
        const int64_t rb = needB + apron;
        const int64_t gw = 2 * rb + 1;

        FarWaterColumnGrid grid;
        grid.resize(camBx - rb, camBy - rb, int32_t(gw), int32_t(gw));
        FarWaterEnumStats st;

        farWaterEnumPlane(fine, grid, st);
        farWaterEnumBasins(both, grid, st);
        farWaterResolveWet(
            grid, [&](int64_t vx, int64_t vy) { return amp.surfaceMm(vx, vy); },
            [&](int64_t vx, int64_t vy) { return both.waterSurfaceMmAtVoxel(vx, vy); }, st);

        std::printf("\n--- ENUMERATION, REALISED over a %lld x %lld brick-column window (%.0f m across) ---\n",
                    (long long)gw, (long long)gw, double(gw * brickMm) / 1000.0);
        std::printf("  water blocks visited        %llu\n", (unsigned long long)st.blocksVisited);
        std::printf("    no tile / no water plane  %llu\n", (unsigned long long)st.blocksNoTile);
        std::printf("    CONSTANT dry (index-only) %llu\n", (unsigned long long)st.blocksConstDry);
        std::printf("    CONSTANT wet (index-only) %llu\n", (unsigned long long)st.blocksConstWet);
        std::printf("    DECODED                   %llu  (%llu failed)\n",
                    (unsigned long long)st.blocksDecoded, (unsigned long long)st.blocksDecodeFailed);
        std::printf("  INDEX-ONLY REJECT RATE      %.1f%%  <-- the property the cascade rests on\n",
                    100.0 * st.indexOnlyRejectRate());
        std::printf("\n  wet brick columns from the PLANE   %llu\n", (unsigned long long)st.planeWetCols);
        std::printf("  wet brick columns from BASINS      %llu  (%llu basins walked, %llu with no mask)\n",
                    (unsigned long long)st.lakeWetCols, (unsigned long long)st.basinsWalked,
                    (unsigned long long)st.basinsNoMask);
        std::printf("  COLUMNS CLAIMED BY BOTH            %llu  <-- must be 0 (disjointness)\n",
                    (unsigned long long)st.bothWetCols);
        std::printf("  union                              %llu\n", (unsigned long long)st.unionWetCols());
        std::printf("\n  EXPENSIVE RESOLVES (amplifier column + datum)  %llu\n",
                    (unsigned long long)st.colsResolved);
        std::printf("  of those, datum above the amplified ground    %llu (%.1f%%)\n",
                    (unsigned long long)st.colsWetAfterResolve,
                    st.colsResolved ? 100.0 * double(st.colsWetAfterResolve) / double(st.colsResolved)
                                    : 0.0);

        totalStats.blocksVisited += st.blocksVisited;
        totalStats.blocksNoTile += st.blocksNoTile;
        totalStats.blocksConstDry += st.blocksConstDry;
        totalStats.blocksConstWet += st.blocksConstWet;
        totalStats.blocksDecoded += st.blocksDecoded;
        totalStats.planeWetCols += st.planeWetCols;
        totalStats.lakeWetCols += st.lakeWetCols;
        totalStats.bothWetCols += st.bothWetCols;
        totalStats.colsResolved += st.colsResolved;
        totalStats.colsWetAfterResolve += st.colsWetAfterResolve;

        // -- resolves attributed to the ring that would have to pay for them --
        std::printf("\n--- RESOLVES PER RING (the cold-fill cost, by ring) ---\n");
        std::printf("  %3s %8s %10s %12s\n", "lod", "voxel", "to (m)", "wet cols");
        std::vector<uint64_t> perRing(size_t(maxLod) + 1, 0);
        for (int64_t by = grid.by0; by < grid.by0 + grid.h; ++by) {
            for (int64_t bx = grid.bx0; bx < grid.bx0 + grid.w; ++bx) {
                const size_t i = grid.at(bx, by);
                if (grid.src[i] == 0) continue;
                const int64_t d = chebyshevBricks(bx, by, camBx, camBy);
                if (d >= farWaterOuterBricks(kBaseBricks, maxLod)) continue;
                const int lod = farWaterLodForDistance(d, kBaseBricks, maxLod);
                perRing[size_t(lod)] += 1;
            }
        }
        uint64_t ringTotal = 0;
        for (int l = 0; l <= maxLod; ++l) {
            ringTotal += perRing[size_t(l)];
            std::printf("  %3d %7.2fm %10.1f %12llu\n", l,
                        double(farWaterCellMm(l)) / 1000.0,
                        double((kBaseBricks << l) * brickMm) / 1000.0,
                        (unsigned long long)perRing[size_t(l)]);
        }
        std::printf("  %3s %8s %10s %12llu  <-- THE CASCADE'S COLD-FILL RESOLVE COST\n", "all", "",
                    "", (unsigned long long)ringTotal);
        // THE TWO RESOLVE NUMBERS ARE DIFFERENT ON PURPOSE AND THE DIFFERENCE IS
        // NOT A LEAK. The grid is sized to the larger of the cascade's outer
        // radius and the largest radius asked for on the command line, plus the
        // interior test's apron, so `colsResolved` covers ground the cascade
        // itself would never draw. The per-ring total above is the number to
        // size the client on; this one is what THIS RUN paid.
        std::printf("  (grid-wide resolves %llu -- the grid also covers the reporting radius %.0f m "
                    "and a %lld-column apron)\n",
                    (unsigned long long)st.colsResolved, maxR, (long long)apron);

        // -----------------------------------------------------------------
        // THE COST. One counter for the cascade and one for today's box, so a
        // ratio cannot come from two different rules.
        // -----------------------------------------------------------------
        //
        // Coarse columns are aggregated with FarWaterAccumulator -- the header's
        // own majority rule and its MEAN ground and datum -- rather than a local
        // approximation, and the fill comes from farWaterFill for the same
        // reason: #228 confirmed that function has 0 vanished columns at every
        // ring, and this world's water is p50 0.75-1.18 m deep, so a coarse cell
        // that swallows the whole column is exactly the case a reimplementation
        // gets wrong and reads as "the river is not there".
        auto coarseColAt = [&](int lod, int64_t cx, int64_t cy, FarWaterColumn& out) -> bool {
            const int64_t step = farWaterStep(lod);
            FarWaterAccumulator acc;
            for (int64_t j = 0; j < step; ++j)
                for (int64_t i = 0; i < step; ++i)
                    acc.add(grid.colAt(cx * step + i, cy * step + j));
            return acc.resolve(step, out);
        };

        auto costForBand = [&](int lod, int64_t rInner, int64_t rOuter) -> RingCost {
            RingCost out;
            const int64_t step = farWaterStep(lod);
            const int64_t cellMm = farWaterCellMm(lod);
            const int64_t cInner = rInner / step, cOuter = (rOuter + step - 1) / step;
            const int64_t ccamx = floorDiv(camBx, step), ccamy = floorDiv(camBy, step);
            // Cached: the interior test and the mesh both read a one-column
            // apron, and recomputing a step x step aggregate per read is a
            // factor of nine on the inner loop.
            const int64_t cw = 2 * cOuter + 3, cx0 = ccamx - cOuter - 1, cy0 = ccamy - cOuter - 1;
            std::vector<FarWaterColumn> cc(size_t(cw) * size_t(cw));
            for (int64_t cy = cy0; cy < cy0 + cw; ++cy)
                for (int64_t cx = cx0; cx < cx0 + cw; ++cx) {
                    FarWaterColumn o;
                    if (coarseColAt(lod, cx, cy, o))
                        cc[size_t(cy - cy0) * size_t(cw) + size_t(cx - cx0)] = o;
                }
            auto ccAt = [&](int64_t cx, int64_t cy) -> const FarWaterColumn& {
                static const FarWaterColumn dry;
                if (cx < cx0 || cx >= cx0 + cw || cy < cy0 || cy >= cy0 + cw) return dry;
                return cc[size_t(cy - cy0) * size_t(cw) + size_t(cx - cx0)];
            };

            std::vector<Quad> raw;
            for (int64_t cy = ccamy - cOuter; cy <= ccamy + cOuter; ++cy) {
                for (int64_t cx = ccamx - cOuter; cx <= ccamx + cOuter; ++cx) {
                    // The band test is in LOD-0 brick units at every level, so a
                    // ring boundary is the same square on the ground whichever
                    // level is measuring it.
                    const int64_t d = chebyshevBricks(cx * step, cy * step, camBx, camBy);
                    if (d < rInner || d >= rOuter) continue;
                    (void)cInner;
                    const FarWaterColumn& c = ccAt(cx, cy);
                    if (!c.wet()) continue;
                    ++out.coarseCols;
                    const FarWaterBrickRange r = farWaterBrickRange(c, cellMm);
                    for (int64_t bz = r.z0; bz <= r.z1; ++bz) {
                        ++out.waterBricks;
                        if (farWaterBrickIsInterior(bz, cellMm, [&](int dx, int dy) {
                                return ccAt(cx + dx, cy + dy);
                            })) {
                            continue;
                        }
                        ++out.surfBricks;
                        raw.clear();
                        const int64_t oz = bz * edge;
                        meshBrick<WaterBrick8::kEdge>(
                            [&](int x, int y, int z) -> MaterialId {
                                const FarWaterColumn& n =
                                    ccAt(cx + (x < 0 ? -1 : (x >= int(edge) ? 1 : 0)),
                                         cy + (y < 0 ? -1 : (y >= int(edge) ? 1 : 0)));
                                const uint8_t fill = farWaterFill((oz + z) * cellMm, n.groundMm,
                                                                  n.datumMm, cellMm);
                                return fill >= 8 ? MaterialId(1) : MaterialId(MAT_AIR);
                            },
                            raw);
                        out.quads += raw.size();
                    }
                }
            }
            return out;
        };

        // TODAY'S BOX, computed by the same counter at LOD 0 -- the square
        // 32-brick window AND the +/-16-brick z clip, which is literally what
        // RefreshImplicitWater sweeps. The z clip is the part that is easy to
        // forget and it is why today's box loses a lake the moment the camera
        // climbs 13 m.
        const int64_t camVz = grid.inBounds(camBx, camBy)
                                  ? floorDiv(int64_t(grid.cols[grid.at(camBx, camBy)].groundMm),
                                             int64_t(kVoxelSizeMm))
                                  : 0;
        const int64_t camBz = floorDiv(camVz, edge);
        auto todayBox = [&]() -> RingCost {
            RingCost out;
            std::vector<Quad> raw;
            for (int64_t by = camBy - kBaseBricks; by <= camBy + kBaseBricks; ++by) {
                for (int64_t bx = camBx - kBaseBricks; bx <= camBx + kBaseBricks; ++bx) {
                    const FarWaterColumn& c = grid.colAt(bx, by);
                    if (!c.wet()) continue;
                    ++out.coarseCols;
                    const FarWaterBrickRange r = farWaterBrickRange(c, int64_t(kVoxelSizeMm));
                    for (int64_t bz = std::max(r.z0, camBz - 16); bz <= std::min(r.z1, camBz + 16);
                         ++bz) {
                        ++out.waterBricks;
                        if (farWaterBrickIsInterior(bz, int64_t(kVoxelSizeMm),
                                                    [&](int dx, int dy) {
                                                        return grid.colAt(bx + dx, by + dy);
                                                    })) {
                            continue;
                        }
                        ++out.surfBricks;
                        raw.clear();
                        const int64_t oz = bz * edge;
                        meshBrick<WaterBrick8::kEdge>(
                            [&](int x, int y, int z) -> MaterialId {
                                const FarWaterColumn& n =
                                    grid.colAt(bx + (x < 0 ? -1 : (x >= int(edge) ? 1 : 0)),
                                               by + (y < 0 ? -1 : (y >= int(edge) ? 1 : 0)));
                                const uint8_t fill = farWaterFill(int64_t(oz + z) * kVoxelSizeMm,
                                                                  n.groundMm, n.datumMm,
                                                                  int64_t(kVoxelSizeMm));
                                return fill >= 8 ? MaterialId(1) : MaterialId(MAT_AIR);
                            },
                            raw);
                        out.quads += raw.size();
                    }
                }
            }
            return out;
        };
        const RingCost today = todayBox();

        std::printf("\n--- COST: CASCADE vs TODAY'S 25.6 m BOX (same camera, same counter) ---\n");
        std::printf("  today's box: %llu surface bricks, %llu quads, %.1f MB\n",
                    (unsigned long long)today.surfBricks, (unsigned long long)today.quads,
                    megabytes(today.quads));
        std::printf("  %8s %10s %12s %12s %10s %10s\n", "radius", "surf brk", "quads", "MB",
                    "vs today", "(quads)");
        for (size_t ri = 0; ri < radii.size(); ++ri) {
            const int64_t Rb = int64_t(std::ceil(radii[ri] * 1000.0 / double(brickMm)));
            RingCost tot;
            for (int l = 0; l <= maxLod; ++l) {
                const int64_t inner = l == 0 ? 0 : (kBaseBricks << (l - 1));
                const int64_t outer = kBaseBricks << l;
                if (inner >= Rb) break;
                const RingCost c = costForBand(l, inner, std::min(outer, Rb));
                tot.coarseCols += c.coarseCols;
                tot.waterBricks += c.waterBricks;
                tot.surfBricks += c.surfBricks;
                tot.quads += c.quads;
            }
            std::printf("  %7.0fm %10llu %12llu %10.1f %9.2fx %9.2fx\n", radii[ri],
                        (unsigned long long)tot.surfBricks, (unsigned long long)tot.quads,
                        megabytes(tot.quads),
                        today.surfBricks ? double(tot.surfBricks) / double(today.surfBricks) : 0.0,
                        today.quads ? double(tot.quads) / double(today.quads) : 0.0);
            if (tot.surfBricks > worst[ri].cascadeBricks) {
                worst[ri].cascadeBricks = tot.surfBricks;
                worst[ri].cascadeQuads = tot.quads;
                worst[ri].todayBricks = today.surfBricks;
                worst[ri].todayQuads = today.quads;
                worst[ri].atX = double(s.cxMm) / 1000.0;
                worst[ri].atY = double(s.cyMm) / 1000.0;
            }
        }

        // Per-ring breakdown at the cascade's own outer radius -- the flatness
        // claim is the whole argument and it should be visible, not inferred.
        std::printf("\n--- PER-RING at the full cascade (%.1f m) ---\n", cascadeOuterM);
        std::printf("  %3s %8s %10s %10s %12s %12s %10s\n", "lod", "voxel", "from", "to",
                    "surf brk", "quads", "MB");
        for (int l = 0; l <= maxLod; ++l) {
            const int64_t inner = l == 0 ? 0 : (kBaseBricks << (l - 1));
            const int64_t outer = kBaseBricks << l;
            const RingCost c = costForBand(l, inner, outer);
            std::printf("  %3d %7.2fm %9.1fm %9.1fm %12llu %12llu %10.1f\n", l,
                        double(farWaterCellMm(l)) / 1000.0, double(inner * brickMm) / 1000.0,
                        double(outer * brickMm) / 1000.0, (unsigned long long)c.surfBricks,
                        (unsigned long long)c.quads, megabytes(c.quads));
        }
    }

    std::printf("\n=====================================================================\n");
    std::printf("=== WORST CASE ACROSS %zu SITE(S) ===\n", siteList.size());
    std::printf("  %8s %12s %12s %12s %10s\n", "radius", "surf brk", "quads", "MB", "vs today");
    for (size_t ri = 0; ri < radii.size(); ++ri) {
        std::printf("  %7.0fm %12llu %12llu %10.1f %9.2fx   (at %.0f, %.0f m; today %llu brk)\n",
                    radii[ri], (unsigned long long)worst[ri].cascadeBricks,
                    (unsigned long long)worst[ri].cascadeQuads, megabytes(worst[ri].cascadeQuads),
                    worst[ri].todayBricks
                        ? double(worst[ri].cascadeBricks) / double(worst[ri].todayBricks)
                        : 0.0,
                    worst[ri].atX, worst[ri].atY, (unsigned long long)worst[ri].todayBricks);
    }
    std::printf("\n=== ENUMERATION TOTALS ACROSS ALL SITES ===\n");
    std::printf("  index-only reject rate %.1f%%   columns claimed by both sources %llu\n",
                100.0 * totalStats.indexOnlyRejectRate(),
                (unsigned long long)totalStats.bothWetCols);
    std::printf("  expensive resolves %llu over %llu blocks decoded\n",
                (unsigned long long)totalStats.colsResolved,
                (unsigned long long)totalStats.blocksDecoded);
    return 0;
}
