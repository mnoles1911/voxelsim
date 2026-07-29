// vxc_riverprobe -- diagnostic for the W3 visible half: does the carved
// riverbed actually exist, is it continuous, and does it reach the sea?
//
// WHY THIS EXISTS. "There is a riverbed for water to flow in" is four
// separate claims, and three of them are invisible in a screenshot until
// water is already flowing:
//
//   1. CHANNELS EXIST AND ARE SIZED BY DISCHARGE. A headwater trickle and a
//      major river must not be the same trench. Reported as the width/depth
//      span over the network and the headwater-to-mouth profile below.
//   2. THEY ARE CONTINUOUS. A bed with a gap is a dam. Walked voxel by
//      voxel along every reach centreline; any column where the channel has
//      no opinion is a gap.
//   3. THEY REACH THE SEA (voxel z=0). A channel that stops at a coastal
//      cliff is worse than none, and the failure is silent -- the terrain
//      still looks like terrain. Reported as outlets above sea level.
//   4. THE BANKS HOLD WATER. A bank one voxel below the water line drains
//      the reach sideways. Checked by walking perpendicular from each
//      centreline column out to the rim, on BOTH sides, against the carved
//      surface -- which is the same channelField.surfaceMm() the applicator
//      and the tests use, so the probe cannot be measuring a different
//      channel from the one that gets built.
//
// Compare vxc_terrainprobe (surface detail) and vxc_climateprobe (climate
// consumers): same shape of tool, same reason -- a hand-derived number
// cannot be re-run after a constant changes.
//
// Usage:
//   vxc_riverprobe <tiledir|--synthetic> <seed> [regionPx]
//                  [--origin PX PY] [--profile]
//
//   regionPx     region edge in TILE PIXELS (default 192; at 30 m/px that
//                is a 5.76 km square, comfortably bigger than the catchment
//                needed to grow a river past the initiation threshold).
//   --origin     region's low corner in tile pixels. THIS MATTERS: rivernet
//                routes only inside the region, so every chain terminates
//                at its edge. A window with no coastline in it reports
//                every outlet as stranded -- correctly, because inside that
//                window no river does reach the sea. Point it at a coast to
//                see a channel run from headwater to open water.
//   --profile    print the full headwater-to-mouth long profile of the
//                longest chain, one row per node.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/channel.h"
#include "voxelcore/rivernet.h"
#include "voxelcore/tilestore.h"

using namespace vxc;

namespace {

std::optional<std::vector<uint8_t>> probeReadFileBytes(const std::filesystem::path& p) {
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

// The chain this tool exists to show: the longest one that starts on dry
// land and ends below sea level -- a river running headwater to sea. Falls
// back to the longest chain of any kind if the region contains no such
// river (e.g. an inland window, where every chain terminates at the region
// edge because rivernet routes only inside its bounds).
//
// "Longest" is by reach count with ties broken on the lower headwater node
// id, so the same chain comes back on every run.
std::vector<uint32_t> showcaseChain(const RiverNetwork& net, const ChannelField& field,
                                    bool& reachesSea) {
    const size_t n = net.nodes().size();
    std::vector<uint8_t> hasIncoming(n, 0);
    for (const RiverSegment& s : net.segments())
        if (s.toNode < n) hasIncoming[s.toNode] = 1;

    std::vector<uint32_t> bestToSea, bestAny;
    for (size_t h = 0; h < n; ++h) {
        if (hasIncoming[h]) continue; // not a headwater
        std::vector<uint32_t> chain;
        uint32_t cur = static_cast<uint32_t>(h);
        // The graph is a forest with at most one outgoing edge per node, so
        // this terminates; the node bound is belt-and-braces.
        for (size_t guard = 0; guard <= n; ++guard) {
            chain.push_back(cur);
            const uint32_t seg = net.outgoingSegment(cur);
            if (seg == RiverNetwork::kNoSegment) break;
            cur = net.segments()[seg].toNode;
        }
        const bool sourceOnLand = field.nodeBedMm()[chain.front()] > 0;
        const bool mouthAtSea = field.nodeBedMm()[chain.back()] <= 0;
        if (sourceOnLand && mouthAtSea && chain.size() > bestToSea.size()) bestToSea = chain;
        if (chain.size() > bestAny.size()) bestAny = std::move(chain);
    }
    reachesSea = !bestToSea.empty();
    return reachesSea ? bestToSea : bestAny;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: vxc_riverprobe <tiledir|--synthetic> <seed> [regionPx] [--profile]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const uint64_t seed = std::strtoull(argv[2], nullptr, 10);
    int64_t regionPx = 192;
    bool wantProfile = false;
    bool haveOrigin = false;
    int64_t originPx = 0, originPy = 0;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--profile") {
            wantProfile = true;
        } else if (a == "--origin" && i + 2 < argc) {
            originPx = std::strtoll(argv[i + 1], nullptr, 10);
            originPy = std::strtoll(argv[i + 2], nullptr, 10);
            haveOrigin = true;
            i += 2;
        } else {
            regionPx = std::strtoll(a.c_str(), nullptr, 10);
        }
    }
    if (regionPx < 8) {
        std::fprintf(stderr, "regionPx must be >= 8\n");
        return 2;
    }

    SyntheticTileSampler synth(seed);
    TileGridSampler grid(seed, 1);
    ITileSampler* tiles = &synth;
    int64_t px0 = -regionPx / 2, py0 = -regionPx / 2;

    if (dir != "--synthetic") {
        int loaded = 0, rejected = 0;
        bool anyBox = false;
        int32_t minTx = 0, minTy = 0;
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".vxtl") continue;
            std::optional<std::vector<uint8_t>> bytes = probeReadFileBytes(e.path());
            if (!bytes) { ++rejected; continue; }
            std::optional<TileData> parsed = TileData::parse(bytes->data(), bytes->size());
            if (!parsed) { ++rejected; continue; }
            const int32_t tx = parsed->x, ty = parsed->y;
            if (!grid.loadTile(std::move(*parsed))) { ++rejected; continue; }
            ++loaded;
            if (!anyBox) { minTx = tx; minTy = ty; anyBox = true; }
            else { minTx = std::min(minTx, tx); minTy = std::min(minTy, ty); }
        }
        std::printf("tiles loaded=%d rejected=%d scale=%d pixelSizeMm=%d\n", loaded, rejected,
                    (int)grid.scale(), grid.pixelSizeMm());
        if (loaded == 0) {
            std::fprintf(stderr, "no tiles loaded from %s\n", dir.c_str());
            return 1;
        }
        tiles = &grid;
        px0 = static_cast<int64_t>(minTx) * 512;
        py0 = static_cast<int64_t>(minTy) * 512;
    }
    if (haveOrigin) {
        px0 = originPx;
        py0 = originPy;
    }

    const RegionBounds bounds{px0, py0, px0 + regionPx - 1, py0 + regionPx - 1};
    const int64_t pxMm = tiles->pixelSizeMm();
    std::printf("region px=[%lld..%lld]x[%lld..%lld] (%lld px, %.2f km/side) pixelSizeMm=%lld\n",
                (long long)bounds.px0, (long long)bounds.px1, (long long)bounds.py0,
                (long long)bounds.py1, (long long)regionPx,
                (double)(regionPx * pxMm) / 1e6, (long long)pxMm);

    RiverNetwork net;
    net.buildFromFlowAccumulation(*tiles, seed, bounds);
    // The bed is cut into the AMPLIFIED surface, not the tile elevation --
    // see channel.h. Probing against anything else would measure a channel
    // nobody builds.
    Amplifier amp(seed, *tiles);
    auto surface = channelSurfaceOf(amp);
    ChannelField field;
    field.build(*tiles, surface, net, bounds);
    const ChannelFieldStats& st = field.stats();

    std::printf("\n--- network ---\n");
    std::printf("nodes=%u segments=%u outlets=%u\n", st.nodes, st.segments, st.outlets);
    if (st.segments == 0) {
        std::printf("NO RIVERS in this region -- nothing to probe.\n");
        return 0;
    }
    std::printf("width  min=%lld mm  max=%lld mm\n",
                (long long)channelWidthMm(0), (long long)st.maxWidthMm);
    std::printf("depth  min=%lld mm  max=%lld mm\n",
                (long long)channelDepthMm(0), (long long)st.maxDepthMm);
    std::printf("bed    min=%lld mm  max=%lld mm\n", (long long)st.minBedMm,
                (long long)st.maxBedMm);
    std::printf("bed strictly descending downstream: %s\n",
                field.bedIsStrictlyDescending() ? "YES" : "NO  <-- INVARIANT BROKEN");

    std::printf("\n--- reaching the sea (sea level = voxel z=0) ---\n");
    std::printf("reaches with bed at/below sea level: %u / %u\n", st.segmentsBelowSeaLevel,
                st.segments);
    std::printf("outlets above sea level (stranded):  %u / %u\n", st.outletsAboveSeaLevel,
                st.outlets);

    // --- continuity: every reach centreline, voxel by voxel ---------------
    //
    // A gap is a column the channel has no opinion about at all. A column
    // left ABOVE its own reach's graded bed is the same dam by a subtler
    // route. A column carved BELOW its own bed is fine and expected: that
    // is a tributary correctly subsumed by a deeper trunk it runs into.
    int64_t centrelineColumns = 0, gaps = 0, aboveOwnBed = 0;
    for (const RiverSegment& seg : net.segments()) {
        const RiverNode& a = net.nodes()[seg.fromNode];
        const RiverNode& b = net.nodes()[seg.toNode];
        const int64_t dx = b.vx - a.vx, dy = b.vy - a.vy;
        const int64_t steps = std::max(std::abs(dx), std::abs(dy));
        if (steps <= 0) continue;
        for (int64_t s = 0; s < steps; ++s) { // [A, B) so junctions count once
            const int64_t vx = a.vx + (dx * s) / steps;
            const int64_t vy = a.vy + (dy * s) / steps;
            ++centrelineColumns;
            if (!field.sampleAt(vx, vy).influenced) {
                ++gaps;
                continue;
            }
            const int64_t bedA = field.nodeBedMm()[seg.fromNode];
            const int64_t bedB = field.nodeBedMm()[seg.toNode];
            if (field.surfaceMm(vx, vy, 1000000) > bedA + ((bedB - bedA) * s) / steps)
                ++aboveOwnBed;
        }
    }

    std::printf("\n--- continuity (every reach centreline, voxel by voxel) ---\n");
    std::printf("centreline columns=%lld  gaps=%lld  above-own-bed=%lld\n",
                (long long)centrelineColumns, (long long)gaps, (long long)aboveOwnBed);

    // --- banks holding water ------------------------------------------------
    //
    // Stated LOCALLY, the same way test_channel.cpp states it: wherever the
    // design profile puts ground at or above the local water line, the
    // CARVED ground has to be there too. Each column carries its own
    // nearest-reach water line, so unlike a ray cast out from a centreline
    // this cannot compare a column against the water line of a different,
    // higher part of the channel and invent leaks.
    int64_t bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    bool firstNode = true;
    for (const RiverNode& n : net.nodes()) {
        if (firstNode) { bx0 = bx1 = n.vx; by0 = by1 = n.vy; firstNode = false; continue; }
        bx0 = std::min(bx0, n.vx); bx1 = std::max(bx1, n.vx);
        by0 = std::min(by0, n.vy); by1 = std::max(by1, n.vy);
    }
    const int64_t margin = (channelInfluenceMm(st.maxWidthMm, st.maxDepthMm) / kVoxelSizeMm) + 4;
    int64_t bankColumns = 0, bankLeaks = 0, fillClampedCols = 0;
    for (int64_t vy = by0 - margin; vy <= by1 + margin; vy += 3) {
        for (int64_t vx = bx0 - margin; vx <= bx1 + margin; vx += 3) {
            const ChannelSample cs = field.sampleAt(vx, vy);
            if (!cs.influenced) continue;
            // The submerged inner bank is legitimately below the water line.
            if (cs.fillTargetMm < cs.waterLineMm) continue;
            ++bankColumns;
            bool clamped = false;
            const int32_t carved = field.surfaceMm(vx, vy, amp.surfaceMm(vx, vy), clamped);
            if (clamped) ++fillClampedCols;
            if (carved < cs.waterLineMm) ++bankLeaks;
        }
    }

    std::printf("\n--- banks holding water (3-voxel sweep of the network bbox) ---\n");
    std::printf("bank columns=%lld  below the water line=%lld  fill-clamped=%lld\n",
                (long long)bankColumns, (long long)bankLeaks, (long long)fillClampedCols);

    // --- long profile -------------------------------------------------------
    bool reachesSea = false;
    const std::vector<uint32_t> chain = showcaseChain(net, field, reachesSea);
    std::printf("\n--- %s: %zu nodes ---\n",
                reachesSea ? "longest HEADWATER-TO-SEA river"
                           : "longest chain (NONE reaches the sea in this window)",
                chain.size());
    if (!chain.empty()) {
        const uint32_t head = chain.front(), mouth = chain.back();
        std::printf("headwater node %u at vx=%lld vy=%lld elev=%d mm bed=%d mm Q=%d\n", head,
                    (long long)net.nodes()[head].vx, (long long)net.nodes()[head].vy,
                    net.nodes()[head].elevationMm, field.nodeBedMm()[head],
                    field.nodeDischarge()[head]);
        std::printf("mouth     node %u at vx=%lld vy=%lld elev=%d mm bed=%d mm Q=%d  %s\n", mouth,
                    (long long)net.nodes()[mouth].vx, (long long)net.nodes()[mouth].vy,
                    net.nodes()[mouth].elevationMm, field.nodeBedMm()[mouth],
                    field.nodeDischarge()[mouth],
                    field.nodeBedMm()[mouth] <= 0 ? "(BELOW SEA LEVEL -- reaches the sea)"
                                                  : "(above sea level -- stranded)");
        const int32_t hw = static_cast<int32_t>(channelWidthMm(field.nodeDischarge()[head]));
        const int32_t mw = static_cast<int32_t>(channelWidthMm(field.nodeDischarge()[mouth]));
        const int32_t hd = static_cast<int32_t>(channelDepthMm(field.nodeDischarge()[head]));
        const int32_t md = static_cast<int32_t>(channelDepthMm(field.nodeDischarge()[mouth]));
        std::printf("headwater channel %d x %d mm  ->  mouth channel %d x %d mm  (%.1fx wider, "
                    "%.1fx deeper)\n",
                    hw, hd, mw, md, mw ? (double)mw / (double)hw : 0.0,
                    md ? (double)md / (double)hd : 0.0);

        if (wantProfile) {
            std::printf("\n%-6s %-12s %-12s %-12s %-10s %-8s %-8s\n", "i", "vx", "vy", "bed_mm",
                        "Q", "w_mm", "d_mm");
            for (size_t i = 0; i < chain.size(); ++i) {
                const uint32_t nd = chain[i];
                std::printf("%-6zu %-12lld %-12lld %-12d %-10d %-8lld %-8lld\n", i,
                            (long long)net.nodes()[nd].vx, (long long)net.nodes()[nd].vy,
                            field.nodeBedMm()[nd], field.nodeDischarge()[nd],
                            (long long)channelWidthMm(field.nodeDischarge()[nd]),
                            (long long)channelDepthMm(field.nodeDischarge()[nd]));
            }
        }
    }

    Digest d;
    field.digest(d);
    std::printf("\nchannel digest=%016llx (kChannelVersion=%u)\n", (unsigned long long)d.h,
                kChannelVersion);
    return 0;
}
