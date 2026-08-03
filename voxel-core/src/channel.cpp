#include "voxelcore/channel.h"

#include <algorithm>

namespace vxc {
namespace {

// Integer sqrt (Newton, seeded from the bit length). Deterministic and
// float-free -- docs/determinism.md forbids std::sqrt in world derivation.
constexpr int64_t isqrt64(int64_t v) {
    if (v <= 0) return 0;
    if (v < 4) return 1;
    int64_t x = v;
    int64_t y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return x;
}

// Linear interpolation of a..b at the rational t = num/den, floored.
constexpr int64_t lerpRat(int64_t a, int64_t b, int64_t num, int64_t den) {
    if (den <= 0) return a;
    return a + floorDiv((b - a) * num, den);
}

} // namespace

void ChannelField::build(ITileSampler& tiles, IChannelSurface& surface, const RiverNetwork& net,
                         const RegionBounds& bounds) {
    segs_.clear();
    cellStart_.clear();
    cellSegs_.clear();
    nodeBedMm_.clear();
    nodeDischarge_.clear();
    stats_ = ChannelFieldStats{};

    bounds_ = bounds;
    pixelSizeMm_ = tiles.pixelSizeMm();
    gw_ = bounds.px1 - bounds.px0 + 1;
    gh_ = bounds.py1 - bounds.py0 + 1;
    if (gw_ <= 0 || gh_ <= 0 || pixelSizeMm_ <= 0) {
        gw_ = gh_ = 0;
        return;
    }

    const std::vector<RiverNode>& nodes = net.nodes();
    const std::vector<RiverSegment>& segments = net.segments();
    const size_t nodeCount = nodes.size();
    stats_.nodes = static_cast<uint32_t>(nodeCount);
    stats_.segments = static_cast<uint32_t>(segments.size());
    if (nodeCount == 0) return;

    // --- per-node discharge -------------------------------------------------
    // A node's outgoing segment carries exactly that node's accumulation
    // (rivernet.cpp assigns seg.discharge = accum[fromNode]), so for every
    // non-terminal node the discharge is exact. A terminal node -- a river
    // mouth or basin sink -- has no outgoing segment, so its throughput is
    // the sum of what flows into it; that is a lower bound on its true
    // accumulation (it omits the node's own rainfall), which keeps
    // discharge non-decreasing downstream and therefore keeps the bed
    // monotone. An isolated node that qualified on its own rainfall alone
    // falls back to the threshold that made it qualify.
    nodeDischarge_.assign(nodeCount, 0);
    std::vector<uint8_t> hasIncoming(nodeCount, 0);
    for (const RiverSegment& seg : segments) {
        if (seg.toNode < nodeCount) {
            hasIncoming[seg.toNode] = 1;
            nodeDischarge_[seg.toNode] =
                static_cast<int32_t>(clampi64(static_cast<int64_t>(nodeDischarge_[seg.toNode]) +
                                                  static_cast<int64_t>(seg.discharge),
                                              0, 0x7FFFFFFF));
        }
    }
    for (size_t n = 0; n < nodeCount; ++n) {
        const uint32_t out = net.outgoingSegment(static_cast<uint32_t>(n));
        if (out != RiverNetwork::kNoSegment && out < segments.size()) {
            nodeDischarge_[n] = segments[out].discharge; // exact accumulation
        } else if (nodeDischarge_[n] == 0) {
            nodeDischarge_[n] = static_cast<int32_t>(
                clampi64(kRiverAccumThresholdDefault, 0, 0x7FFFFFFF));
        }
    }

    // --- graded bed ---------------------------------------------------------
    // One downstream pass:
    //     bed[n] = min(surface(n) - depth(n), bed[upstream] - minDrop)
    // See the header comment for why the datum is the AMPLIFIED surface and
    // why the second term is the whole of channel continuity.
    //
    // Topological order for free: rivernet's D8 edge always targets a
    // strictly lower TILE pixel, so descending tile elevation (ties by node
    // id, which is itself position-ordered and canonical) visits every node
    // after all of its contributors.
    std::vector<uint32_t> order(nodeCount);
    for (size_t i = 0; i < nodeCount; ++i) order[i] = static_cast<uint32_t>(i);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        if (nodes[a].elevationMm != nodes[b].elevationMm)
            return nodes[a].elevationMm > nodes[b].elevationMm;
        return a < b;
    });

    nodeBedMm_.assign(nodeCount, 0);
    std::vector<int64_t> cap(nodeCount, INT64_MAX);
    for (uint32_t n : order) {
        const int64_t depth = channelDepthMm(nodeDischarge_[n]);
        const int64_t ground = surface.surfaceMm(nodes[n].vx, nodes[n].vy);
        const int64_t desired = ground - depth;
        int64_t bed = desired;
        if (cap[n] < bed) {
            bed = cap[n];
            ++stats_.sillsCut;
        }
        nodeBedMm_[n] = clampi32(bed, INT32_MIN, INT32_MAX);
        stats_.maxCutBelowSurfaceMm = std::max(stats_.maxCutBelowSurfaceMm, ground - bed);

        const uint32_t out = net.outgoingSegment(n);
        if (out == RiverNetwork::kNoSegment || out >= segments.size()) continue;
        const uint32_t t = segments[out].toNode;
        if (t >= nodeCount) continue;
        cap[t] = std::min(cap[t], bed - channelMinDropMm(segments[out].lengthMm));
    }

    // --- reaches ------------------------------------------------------------
    segs_.reserve(segments.size());
    bool first = true;
    for (const RiverSegment& seg : segments) {
        if (seg.fromNode >= nodeCount || seg.toNode >= nodeCount) continue;
        const RiverNode& a = nodes[seg.fromNode];
        const RiverNode& b = nodes[seg.toNode];

        Reach r;
        r.ax = a.vx;
        r.ay = a.vy;
        r.bx = b.vx;
        r.by = b.vy;
        r.bedAMm = nodeBedMm_[seg.fromNode];
        r.bedBMm = nodeBedMm_[seg.toNode];
        r.depthAMm = static_cast<int32_t>(channelDepthMm(nodeDischarge_[seg.fromNode]));
        r.depthBMm = static_cast<int32_t>(channelDepthMm(nodeDischarge_[seg.toNode]));
        r.widthAMm = static_cast<int32_t>(channelWidthMm(nodeDischarge_[seg.fromNode]));
        r.widthBMm = static_cast<int32_t>(channelWidthMm(nodeDischarge_[seg.toNode]));
        r.discharge = seg.discharge;
        const int64_t dx = r.bx - r.ax, dy = r.by - r.ay;
        r.lenSq = dx * dx + dy * dy;

        if (r.bedAMm <= kRiverSeaLevelMm && r.bedBMm <= kRiverSeaLevelMm) ++stats_.segmentsBelowSeaLevel;
        stats_.maxWidthMm = std::max(stats_.maxWidthMm,
                                     static_cast<int64_t>(std::max(r.widthAMm, r.widthBMm)));
        stats_.maxDepthMm = std::max(stats_.maxDepthMm,
                                     static_cast<int64_t>(std::max(r.depthAMm, r.depthBMm)));
        const int64_t lo = std::min(r.bedAMm, r.bedBMm), hi = std::max(r.bedAMm, r.bedBMm);
        if (first) {
            stats_.minBedMm = lo;
            stats_.maxBedMm = hi;
            first = false;
        } else {
            stats_.minBedMm = std::min(stats_.minBedMm, lo);
            stats_.maxBedMm = std::max(stats_.maxBedMm, hi);
        }
        segs_.push_back(r);
    }

    // --- outlets ------------------------------------------------------------
    // Only nodes that something actually flows INTO count as river mouths;
    // a lone qualifying pixel with no upstream is not a river that failed
    // to reach the sea.
    for (size_t n = 0; n < nodeCount; ++n) {
        if (net.outgoingSegment(static_cast<uint32_t>(n)) != RiverNetwork::kNoSegment) continue;
        if (!hasIncoming[n]) continue;
        ++stats_.outlets;
        if (nodeBedMm_[n] > kRiverSeaLevelMm) ++stats_.outletsAboveSeaLevel;
    }

    // --- spatial index ------------------------------------------------------
    // One cell per tile pixel. Each reach is inserted into every cell its
    // influence AABB touches; a query then only has to test that cell's
    // (short) candidate list. Counting sort, so the layout is a pure
    // function of segment id order -- no hash or allocation-order
    // dependence anywhere in the digest.
    const size_t cellCount = static_cast<size_t>(gw_) * static_cast<size_t>(gh_);
    cellStart_.assign(cellCount + 1, 0);

    const auto reachCells = [&](const Reach& r, int64_t& cx0, int64_t& cy0, int64_t& cx1,
                                int64_t& cy1) {
        const int64_t infMm = channelInfluenceMm(std::max(r.widthAMm, r.widthBMm),
                                                 std::max(r.depthAMm, r.depthBMm));
        const int64_t infVox = infMm / kVoxelSizeMm + 1;
        const int64_t vx0 = std::min(r.ax, r.bx) - infVox;
        const int64_t vx1 = std::max(r.ax, r.bx) + infVox;
        const int64_t vy0 = std::min(r.ay, r.by) - infVox;
        const int64_t vy1 = std::max(r.ay, r.by) + infVox;
        cx0 = clampi64(floorDiv(vx0 * kVoxelSizeMm, pixelSizeMm_), bounds_.px0, bounds_.px1);
        cx1 = clampi64(floorDiv(vx1 * kVoxelSizeMm, pixelSizeMm_), bounds_.px0, bounds_.px1);
        cy0 = clampi64(floorDiv(vy0 * kVoxelSizeMm, pixelSizeMm_), bounds_.py0, bounds_.py1);
        cy1 = clampi64(floorDiv(vy1 * kVoxelSizeMm, pixelSizeMm_), bounds_.py0, bounds_.py1);
    };

    for (const Reach& r : segs_) {
        int64_t cx0, cy0, cx1, cy1;
        reachCells(r, cx0, cy0, cx1, cy1);
        for (int64_t cy = cy0; cy <= cy1; ++cy)
            for (int64_t cx = cx0; cx <= cx1; ++cx) {
                const size_t i = static_cast<size_t>((cy - bounds_.py0) * gw_ + (cx - bounds_.px0));
                ++cellStart_[i + 1];
            }
    }
    for (size_t i = 0; i < cellCount; ++i) cellStart_[i + 1] += cellStart_[i];
    cellSegs_.assign(cellStart_[cellCount], 0);

    std::vector<uint32_t> cursor(cellStart_.begin(), cellStart_.end() - 1);
    for (size_t s = 0; s < segs_.size(); ++s) {
        int64_t cx0, cy0, cx1, cy1;
        reachCells(segs_[s], cx0, cy0, cx1, cy1);
        for (int64_t cy = cy0; cy <= cy1; ++cy)
            for (int64_t cx = cx0; cx <= cx1; ++cx) {
                const size_t i = static_cast<size_t>((cy - bounds_.py0) * gw_ + (cx - bounds_.px0));
                cellSegs_[cursor[i]++] = static_cast<uint32_t>(s);
            }
    }
}

ChannelSample ChannelField::evaluate(int64_t vx, int64_t vy) const {
    ChannelSample out;
    out.segId = RiverNetwork::kNoSegment;
    if (segs_.empty() || gw_ <= 0 || gh_ <= 0) return out;

    const int64_t cx = floorDiv(vx * kVoxelSizeMm, pixelSizeMm_);
    const int64_t cy = floorDiv(vy * kVoxelSizeMm, pixelSizeMm_);
    if (cx < bounds_.px0 || cx > bounds_.px1 || cy < bounds_.py0 || cy > bounds_.py1) return out;

    const size_t cell = static_cast<size_t>((cy - bounds_.py0) * gw_ + (cx - bounds_.px0));
    const uint32_t lo = cellStart_[cell], hi = cellStart_[cell + 1];

    int64_t bestTarget = 0;
    bool found = false;

    for (uint32_t k = lo; k < hi; ++k) {
        const Reach& r = segs_[cellSegs_[k]];

        // Closest point on the reach segment, as a clamped rational
        // parameter -- no division until the interpolations below.
        const int64_t abx = r.bx - r.ax, aby = r.by - r.ay;
        const int64_t apx = vx - r.ax, apy = vy - r.ay;
        int64_t tNum = 0, tDen = r.lenSq;
        if (tDen > 0) {
            tNum = clampi64(apx * abx + apy * aby, 0, tDen);
        } else {
            tDen = 1;
        }
        const int64_t ccx = r.ax + floorDiv(abx * tNum, tDen);
        const int64_t ccy = r.ay + floorDiv(aby * tNum, tDen);
        const int64_t ddx = (vx - ccx) * kVoxelSizeMm, ddy = (vy - ccy) * kVoxelSizeMm;
        const int64_t dMm = isqrt64(ddx * ddx + ddy * ddy);

        const int64_t bed = lerpRat(r.bedAMm, r.bedBMm, tNum, tDen);
        const int64_t depth = lerpRat(r.depthAMm, r.depthBMm, tNum, tDen);
        const int64_t width = lerpRat(r.widthAMm, r.widthBMm, tNum, tDen);
        if (dMm > channelInfluenceMm(width, depth)) continue;

        const int64_t target = channelTargetMm(dMm, bed, depth, width);
        // Deepest cut wins: at a confluence the trunk's lower bed must
        // swallow the tributary's, or the join is a step.
        if (found && target >= bestTarget) continue;

        found = true;
        bestTarget = target;
        const int64_t halfW = width / 2;
        const int64_t run = channelBankRunMm(depth);
        out.influenced = true;
        out.inBed = dMm <= halfW;
        out.inBank = !out.inBed && dMm <= halfW + run;
        out.bedMm = clampi32(bed, INT32_MIN, INT32_MAX);
        out.targetMm = clampi32(target, INT32_MIN, INT32_MAX);
        // Fill stops climbing at the rim: past it the bank becomes a flat
        // embankment crest that daylights outward, not a backfilled
        // excavation. See channel.h "CUT and FILL use different profiles".
        out.fillTargetMm = clampi32(std::min(target, bed + depth), INT32_MIN, INT32_MAX);
        out.rimMm = clampi32(bed + depth, INT32_MIN, INT32_MAX);
        out.waterLineMm = clampi32(
            bed + (depth * kChannelWaterDepthNum) / kChannelWaterDepthDen, INT32_MIN, INT32_MAX);
        out.depthMm = static_cast<int32_t>(depth);
        out.widthMm = static_cast<int32_t>(width);
        out.dischargeAt = r.discharge;
        out.segId = cellSegs_[k];
    }
    return out;
}

ChannelSample ChannelField::sampleAt(int64_t vx, int64_t vy) const { return evaluate(vx, vy); }

int32_t ChannelField::surfaceMm(int64_t vx, int64_t vy, int32_t naturalSurfaceMm) const {
    bool ignored = false;
    return surfaceMm(vx, vy, naturalSurfaceMm, ignored);
}

int32_t ChannelField::surfaceMm(int64_t vx, int64_t vy, int32_t naturalSurfaceMm,
                                bool& fillClamped) const {
    fillClamped = false;
    const ChannelSample s = evaluate(vx, vy);
    if (!s.influenced) return naturalSurfaceMm;

    // Cut: ground above the cross-section is excavated, everywhere in the
    // footprint including the bank plane past the rim, which is what lets
    // the cut daylight into the hillside instead of ending in a wall.
    if (naturalSurfaceMm > s.targetMm) return s.targetMm;

    // Fill: ground below the embankment profile is built up. Past the rim
    // the profile is flat at rim height, so the embankment runs outward
    // until natural ground overtakes it -- that is what closes the wetted
    // perimeter where the reach crosses a side-slope. Bounded by BUILT
    // HEIGHT, so it reaches as far as it needs to on gentle ground and
    // gives up (counted) on a cliff edge.
    if (naturalSurfaceMm < s.fillTargetMm) {
        const int64_t capped = static_cast<int64_t>(naturalSurfaceMm) + kChannelMaxFillMm;
        if (capped < s.fillTargetMm) {
            fillClamped = true;
            return clampi32(capped, INT32_MIN, INT32_MAX);
        }
        return s.fillTargetMm;
    }
    return naturalSurfaceMm;
}

bool ChannelField::bedIsStrictlyDescending() const {
    for (const Reach& r : segs_)
        if (r.bedBMm >= r.bedAMm) return false;
    return true;
}

void ChannelField::digest(Digest& d) const {
    d.u32(kChannelVersion);
    for (const Reach& r : segs_) {
        d.i64(r.ax);
        d.i64(r.ay);
        d.i64(r.bx);
        d.i64(r.by);
        d.u32(static_cast<uint32_t>(r.bedAMm));
        d.u32(static_cast<uint32_t>(r.bedBMm));
        d.u32(static_cast<uint32_t>(r.depthAMm));
        d.u32(static_cast<uint32_t>(r.depthBMm));
        d.u32(static_cast<uint32_t>(r.widthAMm));
        d.u32(static_cast<uint32_t>(r.widthBMm));
        d.u32(static_cast<uint32_t>(r.discharge));
    }
}

} // namespace vxc
