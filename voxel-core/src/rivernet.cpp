#include "voxelcore/rivernet.h"

#include <algorithm>
#include <cstdlib>

namespace vxc {
namespace {

constexpr int kD8Count = 8;
// Fixed compass tie-break order (N,NE,E,SE,S,SW,W,NW) -- see rivernet.h
// header comment. The FIRST neighbor achieving the minimum elevation wins,
// so this array's order is itself part of the determinism contract.
constexpr int64_t kD8Dx[kD8Count] = {0, 1, 1, 1, 0, -1, -1, -1};
constexpr int64_t kD8Dy[kD8Count] = {-1, -1, 0, 1, 1, 1, 0, -1};
constexpr bool kD8Diagonal[kD8Count] = {false, true, false, true, false, true, false, true};

// Integer sqrt(2) approximation (181/128 ~= 1.4141) for diagonal reach
// length vs. a cardinal step's pixelSizeMm -- no floats (docs/determinism.md).
constexpr int64_t kDiagNumerator = 181;
constexpr int64_t kCardNumerator = 128;

// Fixed flow-velocity constant (mm per millisecond) used to turn a
// segment's length into its Muskingum K travel-time constant at build
// time: travelMillis = lengthMm / kFlowVelocityMmPerMilli. 5 mm/ms == 5
// m/s, a plausible placeholder channel flow speed; a future pass can make
// this discharge- or slope-conditioned.
constexpr int64_t kFlowVelocityMmPerMilli = 5;

// Planar reach length between two promoted-channel points, in mm, from their
// world VOXEL coordinates. Generalizes buildFromFlowAccumulation's
// cardinal/diagonal split (which only ever sees a one-pixel D8 step) to an
// arbitrary offset using the SAME integer sqrt(2) approximation: for a step
// that is diagonal in the pixel grid dx == dy and this reduces to exactly
// major*181/128, and for an axial step it is exact. No floats, and never
// zero (travelMillis divides by it downstream).
int64_t channelStepLengthMm(const RiverNode& a, const RiverNode& b) {
    const int64_t dx = std::abs(b.vx - a.vx) * kVoxelSizeMm;
    const int64_t dy = std::abs(b.vy - a.vy) * kVoxelSizeMm;
    const int64_t major = std::max(dx, dy);
    const int64_t minor = std::min(dx, dy);
    return std::max<int64_t>(1, major + minor * (kDiagNumerator - kCardNumerator) / kCardNumerator);
}

} // namespace

void RiverNetwork::buildFromFlowAccumulation(ITileSampler& tiles, uint64_t seed,
                                             const RegionBounds& b, int64_t accumThreshold) {
    nodes_.clear();
    segments_.clear();
    outgoingSegmentOfNode_.clear();
    extraOutgoing_.clear();
    headwaterNodes_.clear();
    diffLog_.clear();
    totalStorage_ = 0;
    totalInjected_ = 0;
    totalOutflowToOutlets_ = 0;
    totalWithdrawn_ = 0;
    bakedScanned_ = 0;
    bakedUnresolved_ = 0;
    bakedChannelCells_ = 0;
    bakedRimHeadsDropped_ = 0;
    seed_ = seed;

    const int64_t w = b.px1 - b.px0 + 1;
    const int64_t h = b.py1 - b.py0 + 1;
    if (w <= 0 || h <= 0) return;

    const int64_t pixelSizeMm = tiles.pixelSizeMm();
    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    const auto idxOf = [&](int64_t px, int64_t py) -> size_t {
        return static_cast<size_t>((py - b.py0) * w + (px - b.px0));
    };

    // Sample elevation + precip once per pixel; iterating py-outer/px-inner
    // so index i already equals idxOf(px,py) for i = 0..count-1 in
    // ascending (py, px) order -- reused below as the canonical node-id
    // traversal order.
    std::vector<int32_t> elev(count);
    std::vector<uint8_t> precip(count);
    for (int64_t py = b.py0; py <= b.py1; ++py)
        for (int64_t px = b.px0; px <= b.px1; ++px) {
            const size_t i = idxOf(px, py);
            elev[i] = tiles.elevationMm(px, py);
            precip[i] = tiles.climate(px, py).precipitation;
        }

    // D8 steepest-descent direction: lowest-elevation in-bounds neighbor
    // (see header comment for the v0 no-distance-normalization note).
    // -1 == no downhill neighbor inside bounds (pit/sink/region-edge).
    std::vector<int8_t> dir(count, -1);
    for (int64_t py = b.py0; py <= b.py1; ++py)
        for (int64_t px = b.px0; px <= b.px1; ++px) {
            const size_t i = idxOf(px, py);
            int32_t best = elev[i];
            int8_t bestDir = -1;
            for (int k = 0; k < kD8Count; ++k) {
                const int64_t nx = px + kD8Dx[k], ny = py + kD8Dy[k];
                if (nx < b.px0 || nx > b.px1 || ny < b.py0 || ny > b.py1) continue;
                const int32_t ne = elev[idxOf(nx, ny)];
                if (ne < best) {
                    best = ne;
                    bestDir = static_cast<int8_t>(k);
                }
            }
            dir[i] = bestDir;
        }

    // Flow accumulation in strictly-descending elevation order (ties
    // broken by ascending array index, i.e. (py,px)) -- see header comment
    // "buildFromFlowAccumulation" step 3 for why this order is correct.
    std::vector<size_t> order(count);
    for (size_t i = 0; i < count; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t c) {
        if (elev[a] != elev[c]) return elev[a] > elev[c];
        return a < c;
    });

    // Flow weight is RAINFALL IN MM/YR, not the raw wire byte (worldgen v8,
    // kRiverNetVersion 2).
    //
    // The byte is a quantization of a physical range that the consumer had no
    // way to know (see voxelcore/climate.h), so weighting by it meant "rivers
    // form after N upstream pixels" depended on the encoding rather than on
    // rainfall: the same terrain gave a river every ~4 pixels under the old
    // synthetic tiles and every ~23 under real diffusion tiles, for no physical
    // reason. Decoding first makes accumThreshold mean a real quantity --
    // mm/yr of rain gathered upstream -- and makes it portable across any tile
    // source. kRiverAccumThresholdDefault is rescaled by the same factor so
    // river density is unchanged for the synthetic case it was tuned on.
    std::vector<int64_t> accum(count, 0);
    for (size_t oi : order) {
        accum[oi] += climatePrecipMmPerYrFromU8(precip[oi]);
        const int8_t d = dir[oi];
        if (d < 0) continue;
        const int64_t px = b.px0 + static_cast<int64_t>(oi % static_cast<size_t>(w));
        const int64_t py = b.py0 + static_cast<int64_t>(oi / static_cast<size_t>(w));
        const int64_t nx = px + kD8Dx[d], ny = py + kD8Dy[d];
        accum[idxOf(nx, ny)] += accum[oi];
    }

    // Node ids: assigned to every threshold-qualifying pixel in ascending
    // (py, px) order (== ascending i, per the sampling loop above) -- a
    // position-ordered traversal, deliberately independent of the
    // elevation-sorted `order` used for accumulation.
    std::vector<int32_t> nodeIdOfPixel(count, -1);
    for (size_t i = 0; i < count; ++i) {
        if (accum[i] < accumThreshold) continue;
        const int64_t px = b.px0 + static_cast<int64_t>(i % static_cast<size_t>(w));
        const int64_t py = b.py0 + static_cast<int64_t>(i / static_cast<size_t>(w));
        RiverNode n;
        n.vx = floorDiv(px * pixelSizeMm, static_cast<int64_t>(kVoxelSizeMm));
        n.vy = floorDiv(py * pixelSizeMm, static_cast<int64_t>(kVoxelSizeMm));
        n.elevationMm = elev[i];
        nodeIdOfPixel[i] = static_cast<int32_t>(nodes_.size());
        nodes_.push_back(n);
    }

    // Segments: one per qualifying pixel whose D8 target also qualifies,
    // visited in the same position order as node-id assignment above (so
    // segment ids are just as deterministic).
    outgoingSegmentOfNode_.assign(nodes_.size(), kNoSegment);
    for (size_t i = 0; i < count; ++i) {
        if (nodeIdOfPixel[i] < 0) continue;
        const int8_t d = dir[i];
        if (d < 0) continue;
        const int64_t px = b.px0 + static_cast<int64_t>(i % static_cast<size_t>(w));
        const int64_t py = b.py0 + static_cast<int64_t>(i / static_cast<size_t>(w));
        const int64_t nx = px + kD8Dx[d], ny = py + kD8Dy[d];
        const size_t ni = idxOf(nx, ny);
        if (nodeIdOfPixel[ni] < 0) continue; // downhill neighbor doesn't qualify -- this node is a terminal outlet

        RiverSegment seg;
        seg.fromNode = static_cast<uint32_t>(nodeIdOfPixel[i]);
        seg.toNode = static_cast<uint32_t>(nodeIdOfPixel[ni]);
        seg.lengthMm = pixelSizeMm * (kD8Diagonal[d] ? kDiagNumerator : kCardNumerator) / kCardNumerator;
        seg.discharge = static_cast<int32_t>(clampi64(accum[i], 0, 0x7FFFFFFF));
        seg.storage = 0;
        seg.conveyance = 255;
        seg.travelMillis = std::max<int64_t>(1, seg.lengthMm / kFlowVelocityMmPerMilli);

        const uint32_t segId = static_cast<uint32_t>(segments_.size());
        outgoingSegmentOfNode_[seg.fromNode] = segId;
        segments_.push_back(seg);
    }
}

// ---------------------------------------------------------------------------
// buildFromBakedWater (see rivernet.h's header comment)
// ---------------------------------------------------------------------------
namespace {

// One scanned pixel of the baked planes.
struct BakedCell {
    int32_t surfaceMm = 0;
    uint8_t accumLog2 = 0;
    bool qualifies = false;
};

// The STRICT TOTAL ORDER downstream must decrease along. Water surface first
// (water runs down its own surface), then accumulation, then the canonical
// (py,px) pixel index. Total and strict, so no cycle can close -- which is the
// property that lets the builder skip the topological sort the D8 path needs.
bool bakedLess(const BakedCell& a, size_t ia, const BakedCell& b, size_t ib) {
    if (a.surfaceMm != b.surfaceMm) return a.surfaceMm < b.surfaceMm;
    if (a.accumLog2 != b.accumLog2) return a.accumLog2 > b.accumLog2;
    return ia < ib;
}

} // namespace

uint32_t RiverNetwork::buildFromBakedWater(IBakedWaterSource& source, uint64_t seed,
                                           const BakedWaterBuildParams& params) {
    nodes_.clear();
    segments_.clear();
    outgoingSegmentOfNode_.clear();
    extraOutgoing_.clear();
    headwaterNodes_.clear();
    diffLog_.clear();
    totalStorage_ = 0;
    totalInjected_ = 0;
    totalOutflowToOutlets_ = 0;
    totalWithdrawn_ = 0;
    bakedScanned_ = 0;
    bakedUnresolved_ = 0;
    bakedChannelCells_ = 0;
    bakedRimHeadsDropped_ = 0;
    seed_ = seed;

    const RegionBounds& b = params.bounds;
    const int64_t w = b.px1 - b.px0 + 1;
    const int64_t h = b.py1 - b.py0 + 1;
    if (w <= 0 || h <= 0) return 0;

    const int64_t pixelSizeMm = source.pixelSizeMm();
    if (pixelSizeMm <= 0) return 0;

    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    const auto idxOf = [&](int64_t px, int64_t py) -> size_t {
        return static_cast<size_t>((py - b.py0) * w + (px - b.px0));
    };

    // ONE SCAN, py-outer/px-inner, so index i is already the canonical
    // (py,px) traversal order the D8 builder assigns node ids in. Keeping the
    // two builders on ONE ordering convention is what lets a caller reason
    // about node ids without asking which builder made the graph.
    std::vector<BakedCell> cells(count);
    for (int64_t py = b.py0; py <= b.py1; ++py) {
        for (int64_t px = b.px0; px <= b.px1; ++px) {
            const size_t i = idxOf(px, py);
            ++bakedScanned_;

            uint8_t flow = 0;
            if (!source.flowAt(px, py, flow)) {
                ++bakedUnresolved_;
                continue;
            }
            if (!flowIsChannel(flow)) continue;
            ++bakedChannelCells_;
            const uint8_t accum = flowAccumLog2(flow);
            if (accum < params.minAccumLog2) continue;

            int32_t surfaceMm = 0;
            bool wet = false;
            if (!source.waterAt(px, py, surfaceMm, wet)) {
                ++bakedUnresolved_;
                if (!params.admitUnresolvedWater) continue;
                // The ordering fallback (see BakedWaterBuildParams): a surface
                // low enough that every resolved cell outranks it, so an
                // unresolved cell can only ever be a terminal.
                surfaceMm = INT32_MIN + 1;
                wet = true;
            } else if (params.requireWet && !wet) {
                continue;
            }

            cells[i].surfaceMm = surfaceMm;
            cells[i].accumLog2 = accum;
            cells[i].qualifies = true;
        }
    }

    // Node ids in the canonical order (== ascending i).
    std::vector<int32_t> nodeIdOfPixel(count, -1);
    for (size_t i = 0; i < count; ++i) {
        if (!cells[i].qualifies) continue;
        const int64_t px = b.px0 + static_cast<int64_t>(i % static_cast<size_t>(w));
        const int64_t py = b.py0 + static_cast<int64_t>(i / static_cast<size_t>(w));
        RiverNode n;
        // The pixel CENTRE, matching buildFromFlowAccumulation... except that
        // one takes the pixel's low corner. Both are one pixel of a 1.875 m
        // grid apart and neither is wrong, but they must not differ, so this
        // takes the corner too. The basin spillway compensates on its side
        // (basinledger.h's outletVoxel centres the outlet) because a saddle is
        // a point on the ground, not a cell of the graph.
        n.vx = floorDiv(px * pixelSizeMm, static_cast<int64_t>(kVoxelSizeMm));
        n.vy = floorDiv(py * pixelSizeMm, static_cast<int64_t>(kVoxelSizeMm));
        // THE WATER SURFACE, not the bed. A RiverNode's elevationMm is what
        // promoteChannel's strictly-descending check and rivercouple.h's
        // outfall both read, and for a graph whose geometry came from the water
        // plane the water surface is the consistent answer -- the bed under a
        // 3 m river is 3 m below the thing the graph is modelling.
        n.elevationMm = cells[i].surfaceMm;
        nodeIdOfPixel[i] = static_cast<int32_t>(nodes_.size());
        nodes_.push_back(n);
    }

    outgoingSegmentOfNode_.assign(nodes_.size(), kNoSegment);
    std::vector<uint8_t> hasIncoming(nodes_.size(), 0);

    for (size_t i = 0; i < count; ++i) {
        if (nodeIdOfPixel[i] < 0) continue;
        const int64_t px = b.px0 + static_cast<int64_t>(i % static_cast<size_t>(w));
        const int64_t py = b.py0 + static_cast<int64_t>(i / static_cast<size_t>(w));

        // Among the neighbours strictly BELOW this cell in the total order,
        // take the LOWEST one in that same order -- lowest water surface, then
        // greatest accumulation, then lowest pixel index. Using one order for
        // both the "is this downstream" test and the "which downstream" choice
        // is what makes the result a pure function of the planes: there is no
        // second rule that could disagree with the first, and no dependence on
        // the compass array's iteration order.
        int bestK = -1;
        size_t bestIdx = 0;
        for (int k = 0; k < kD8Count; ++k) {
            const int64_t nx = px + kD8Dx[k], ny = py + kD8Dy[k];
            if (nx < b.px0 || nx > b.px1 || ny < b.py0 || ny > b.py1) continue;
            const size_t ni = idxOf(nx, ny);
            if (nodeIdOfPixel[ni] < 0) continue;
            if (!bakedLess(cells[ni], ni, cells[i], i)) continue;
            if (bestK >= 0 && !bakedLess(cells[ni], ni, cells[bestIdx], bestIdx)) continue;
            bestK = k;
            bestIdx = ni;
        }
        if (bestK < 0) continue; // terminal: leaves the region, or is the graph's low point

        RiverSegment seg;
        seg.fromNode = static_cast<uint32_t>(nodeIdOfPixel[i]);
        seg.toNode = static_cast<uint32_t>(nodeIdOfPixel[bestIdx]);
        seg.lengthMm =
            pixelSizeMm * (kD8Diagonal[bestK] ? kDiagNumerator : kCardNumerator) / kCardNumerator;
        // BUILD-TIME DISCHARGE IS CATCHMENT AREA IN m^2 here -- see the header
        // comment's item 4 for why this is a third meaning and not a bug.
        seg.discharge = flowAccumM2(cells[i].accumLog2);
        seg.storage = 0;
        seg.conveyance = 255;
        seg.travelMillis = std::max<int64_t>(1, seg.lengthMm / kFlowVelocityMmPerMilli);

        const uint32_t segId = static_cast<uint32_t>(segments_.size());
        outgoingSegmentOfNode_[seg.fromNode] = segId;
        hasIncoming[seg.toNode] = 1;
        segments_.push_back(seg);
    }

    // Heads, in ascending node id. Walking PIXELS rather than node ids (the two
    // orders are identical -- node ids were assigned in ascending i above) so
    // the rim test has the pixel coordinate it needs without a second array.
    for (size_t i = 0; i < count; ++i) {
        const int32_t nid = nodeIdOfPixel[i];
        if (nid < 0) continue;
        if (hasIncoming[static_cast<size_t>(nid)]) continue;
        if (params.dropRimHeadwaters) {
            const int64_t px = b.px0 + static_cast<int64_t>(i % static_cast<size_t>(w));
            const int64_t py = b.py0 + static_cast<int64_t>(i / static_cast<size_t>(w));
            // ON the boundary ring: a reach entering the box was clipped here,
            // so "no in-edge" says nothing about whether this is a spring.
            if (px == b.px0 || px == b.px1 || py == b.py0 || py == b.py1) {
                ++bakedRimHeadsDropped_;
                continue;
            }
        }
        headwaterNodes_.push_back(static_cast<uint32_t>(nid));
    }
    return static_cast<uint32_t>(segments_.size());
}

std::vector<uint32_t> RiverNetwork::headwaterSegments() const {
    std::vector<uint32_t> out;
    out.reserve(headwaterNodes_.size());
    for (uint32_t n : headwaterNodes_) {
        const uint32_t seg = outgoingSegment(n);
        if (seg != kNoSegment) out.push_back(seg);
    }
    return out;
}

uint32_t RiverNetwork::nearestSegmentToVoxel(int64_t vx, int64_t vy, int64_t maxDistMm) const {
    uint32_t best = kNoSegment;
    int64_t bestDist = 0;
    for (size_t i = 0; i < segments_.size(); ++i) {
        const RiverNode& n = nodes_[segments_[i].fromNode];
        // The SAME integer metric channelStepLengthMm uses (major + minor *
        // (181-128)/128), so "nearest" here and "how long is that reach" there
        // cannot disagree about what distance means. No floats, no sqrt.
        const int64_t dx = std::abs(n.vx - vx) * kVoxelSizeMm;
        const int64_t dy = std::abs(n.vy - vy) * kVoxelSizeMm;
        const int64_t major = std::max(dx, dy), minor = std::min(dx, dy);
        const int64_t d = major + minor * (kDiagNumerator - kCardNumerator) / kCardNumerator;
        if (d > maxDistMm) continue;
        if (best == kNoSegment || d < bestDist) {
            best = static_cast<uint32_t>(i);
            bestDist = d;
        }
    }
    return best;
}

void RiverNetwork::injectInflow(uint32_t segId, int32_t amount) {
    if (segId >= segments_.size() || amount <= 0) return;
    segments_[segId].storage += amount;
    totalStorage_ += amount;
    totalInjected_ += amount;
}

void RiverNetwork::setConveyance(uint32_t segId, uint8_t factor0to255) {
    if (segId >= segments_.size()) return;
    segments_[segId].conveyance = factor0to255;
    // Recorded AFTER the range check, so a log never carries a diff that replay
    // would reject. Off by default -- see rivernet.h's "the diff log, as a
    // RECORDING".
    if (recordDiffs_) {
        diffLog_.push_back(RiverDiffRecord{.kind = RiverDiffKind::kSetConveyance,
                                           .segId = segId,
                                           .value = factor0to255,
                                           .headNode = 0,
                                           .course = {}});
    }
}

int32_t RiverNetwork::withdrawToCoupler(uint32_t segId, int32_t amount) {
    if (segId >= segments_.size() || amount <= 0) return 0;
    const int32_t take = std::min(amount, segments_[segId].storage);
    if (take <= 0) return 0;
    segments_[segId].storage -= take;
    totalStorage_ -= take;
    totalWithdrawn_ += take;
    return take;
}

int32_t RiverNetwork::refundFromCoupler(uint32_t segId, int32_t amount) {
    if (segId >= segments_.size() || amount <= 0) return 0;
    // Clamped by what has actually been withdrawn: a refund is a REVERSAL,
    // and letting one exceed the withdrawals would make this a second (and
    // unaudited) injection path straight past totalInjected().
    const int32_t give = static_cast<int32_t>(std::min<int64_t>(amount, totalWithdrawn_));
    if (give <= 0) return 0;
    segments_[segId].storage += give;
    totalStorage_ += give;
    totalWithdrawn_ -= give;
    return give;
}

uint32_t RiverNetwork::outgoingSegmentCount(uint32_t nodeId) const {
    if (nodeId >= outgoingSegmentOfNode_.size()) return 0;
    if (outgoingSegmentOfNode_[nodeId] == kNoSegment) return 0;
    const auto it = extraOutgoing_.find(nodeId);
    return 1u + (it == extraOutgoing_.end() ? 0u : static_cast<uint32_t>(it->second.size()));
}

uint32_t RiverNetwork::promoteChannel(uint32_t headNode, const std::vector<RiverChannelPoint>& course) {
    // --- VALIDATE EVERYTHING BEFORE MUTATING ANYTHING. A half-applied
    // promotion is unrecoverable: there is no un-promote diff, and a segment
    // pointing at a node that should not exist corrupts routing forever.
    if (headNode >= nodes_.size() || course.empty()) return kNoSegment;

    int32_t prevElev = nodes_[headNode].elevationMm;
    int64_t prevVx = nodes_[headNode].vx, prevVy = nodes_[headNode].vy;
    for (size_t i = 0; i < course.size(); ++i) {
        const RiverChannelPoint& p = course[i];
        if (p.elevationMm >= prevElev) return kNoSegment; // must strictly descend
        if (p.vx == prevVx && p.vy == prevVy) return kNoSegment;
        const bool isLast = (i + 1 == course.size());
        if (p.existingNode != kNoNode) {
            if (!isLast) return kNoSegment;            // only the tail may rejoin the network
            if (p.existingNode >= nodes_.size()) return kNoSegment;
            if (p.existingNode == headNode) return kNoSegment; // a self-loop is not a channel
        }
        // No repeated position anywhere in the course (an O(n^2) scan over a
        // course that is bounded to a handful of points by the detector).
        for (size_t j = 0; j < i; ++j)
            if (course[j].vx == p.vx && course[j].vy == p.vy) return kNoSegment;
        prevElev = p.elevationMm;
        prevVx = p.vx;
        prevVy = p.vy;
    }

    // A "diversion" whose first step lands on the node the head already
    // drains into is just the main stem restated -- reject rather than
    // silently double the stem's conveyance via the even split.
    const uint32_t headPrimary = outgoingSegmentOfNode_[headNode];
    if (headPrimary != kNoSegment) {
        const RiverNode& stemTo = nodes_[segments_[headPrimary].toNode];
        if (stemTo.vx == course[0].vx && stemTo.vy == course[0].vy) return kNoSegment;
    }

    // --- APPLY. Nodes first (so every segment below can name a real id).
    std::vector<uint32_t> nodeIds(course.size(), kNoNode);
    for (size_t i = 0; i < course.size(); ++i) {
        if (course[i].existingNode != kNoNode) {
            nodeIds[i] = course[i].existingNode;
            continue;
        }
        RiverNode n;
        n.vx = course[i].vx;
        n.vy = course[i].vy;
        n.elevationMm = course[i].elevationMm;
        nodeIds[i] = static_cast<uint32_t>(nodes_.size());
        nodes_.push_back(n);
        outgoingSegmentOfNode_.push_back(kNoSegment);
    }

    const uint32_t firstNewSeg = static_cast<uint32_t>(segments_.size());
    uint32_t fromNode = headNode;
    for (size_t i = 0; i < course.size(); ++i) {
        const uint32_t toNode = nodeIds[i];
        RiverSegment seg;
        seg.fromNode = fromNode;
        seg.toNode = toNode;
        seg.lengthMm = channelStepLengthMm(nodes_[fromNode], nodes_[toNode]);
        seg.discharge = 0; // a brand-new channel has routed nothing yet
        seg.storage = 0;
        seg.conveyance = 255;
        seg.travelMillis = std::max<int64_t>(1, seg.lengthMm / kFlowVelocityMmPerMilli);

        const uint32_t segId = static_cast<uint32_t>(segments_.size());
        if (outgoingSegmentOfNode_[fromNode] == kNoSegment)
            outgoingSegmentOfNode_[fromNode] = segId;
        else
            extraOutgoing_[fromNode].push_back(segId); // THE bifurcation, see header
        segments_.push_back(seg);
        fromNode = toNode;
    }
    // ONLY ON SUCCESS. Every rejection above returns kNoSegment having mutated
    // nothing, so a log that recorded them would carry records replay must
    // reject again -- harmless, but a lie about what happened.
    if (recordDiffs_) {
        diffLog_.push_back(RiverDiffRecord{.kind = RiverDiffKind::kDivertChannel,
                                           .segId = firstNewSeg,
                                           .value = 255,
                                           .headNode = headNode,
                                           .course = course});
    }
    return firstNewSeg;
}

void RiverNetwork::step(int64_t dtMillis) {
    const size_t n = segments_.size();
    if (n == 0) return;

    // READ phase: every outflow is computed from tick-START storage only
    // (see header comment "step()") -- never from anything the APPLY phase
    // below writes, which is what makes the pass order-independent over
    // segments even at a multi-in-edge confluence.
    std::vector<int32_t> outflow(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const RiverSegment& s = segments_[i];
        if (s.storage <= 0 || s.conveyance == 0 || dtMillis <= 0) continue;
        const int64_t raw = (static_cast<int64_t>(s.storage) * dtMillis) / s.travelMillis;
        const int64_t scaled = (raw * s.conveyance) / 255;
        outflow[i] = static_cast<int32_t>(clampi64(scaled, 0, s.storage));
    }

    // APPLY phase: subtract from source, add to the downstream segment (or
    // the outlet ledger). Internal moves net to zero; only outlet-bound
    // flow changes totalStorage_.
    // A graph that has never been promoted has no bifurcations, so this is
    // false for every graph buildFromFlowAccumulation can produce and the
    // loop below runs the identical arithmetic it did before kRiverNetVersion
    // 3 -- one bool test, no hash probe, golden unmoved.
    const bool anyBifurcation = !extraOutgoing_.empty();

    int64_t outletDeltaThisTick = 0;
    for (size_t i = 0; i < n; ++i) {
        RiverSegment& s = segments_[i];
        const int32_t of = outflow[i];
        s.storage -= of;
        s.discharge = of;
        const uint32_t downSeg = outgoingSegmentOfNode_[s.toNode];
        if (downSeg == kNoSegment) {
            // Outlet: no primary means no extras either (promoteChannel fills
            // the primary slot first), so this is the whole story.
            outletDeltaThisTick += of;
            continue;
        }
        if (!anyBifurcation) {
            segments_[downSeg].storage += of;
            continue;
        }
        const auto it = extraOutgoing_.find(s.toNode);
        if (it == extraOutgoing_.end()) {
            segments_[downSeg].storage += of;
            continue;
        }
        // EVEN SPLIT across primary + extras (header comment "Bifurcation").
        // The remainder is handed out one unit at a time in the fixed order
        // primary-then-extras, so the shares sum to EXACTLY `of` -- the split
        // moves water, it never rounds any away.
        const int32_t targets = 1 + static_cast<int32_t>(it->second.size());
        const int32_t share = of / targets;
        int32_t rem = of % targets;
        const auto take = [&]() {
            int32_t give = share;
            if (rem > 0) {
                ++give;
                --rem;
            }
            return give;
        };
        segments_[downSeg].storage += take();
        for (uint32_t extra : it->second) segments_[extra].storage += take();
    }
    totalOutflowToOutlets_ += outletDeltaThisTick;
    totalStorage_ -= outletDeltaThisTick;
}

void RiverNetwork::applyGraphDiff(const RiverDiffRecord& diff) {
    switch (diff.kind) {
        case RiverDiffKind::kSetConveyance:
            setConveyance(diff.segId, diff.value);
            break;
        case RiverDiffKind::kDivertChannel:
            // Real since kRiverNetVersion 3. The DECISION (which course) was
            // made by the detector in rivercouple.h against live CA state;
            // this replays it verbatim, so a log replayed against a freshly
            // built graph reproduces the promoted topology exactly. A record
            // with an empty course is rejected by promoteChannel and is
            // therefore inert, which is what keeps a default-constructed
            // kDivertChannel diff a no-op.
            promoteChannel(diff.headNode, diff.course);
            break;
    }
}

int64_t RiverNetwork::recomputeTotalStorage() const {
    int64_t sum = 0;
    for (const RiverSegment& s : segments_) sum += s.storage;
    return sum;
}

RiverNetwork::RoutingSnapshot RiverNetwork::captureRoutingState() const {
    RoutingSnapshot snap;
    snap.segs.reserve(segments_.size());
    for (const RiverSegment& s : segments_) {
        snap.segs.push_back(RoutingSnapshot::Seg{s.discharge, s.storage, s.conveyance});
    }
    snap.totalStorage = totalStorage_;
    snap.totalInjected = totalInjected_;
    snap.totalOutflowToOutlets = totalOutflowToOutlets_;
    snap.totalWithdrawn = totalWithdrawn_;
    return snap;
}

bool RiverNetwork::restoreRoutingState(const RoutingSnapshot& snap) {
    // VALIDATE EVERYTHING BEFORE MUTATING ANYTHING, the same rule
    // promoteChannel follows and for a stronger reason: a half-restored ledger
    // is a conservation invariant that fails for the rest of the session, and
    // every conservation assertion downstream would then be measuring the load
    // rather than the sim.
    if (snap.segs.size() != segments_.size()) return false;
    int64_t sum = 0;
    for (const RoutingSnapshot::Seg& s : snap.segs) {
        if (s.storage < 0) return false;
        sum += s.storage;
    }
    if (sum != snap.totalStorage) return false;
    if (snap.totalStorage < 0 || snap.totalInjected < 0 || snap.totalOutflowToOutlets < 0 ||
        snap.totalWithdrawn < 0) {
        return false;
    }
    if (snap.totalStorage + snap.totalOutflowToOutlets + snap.totalWithdrawn !=
        snap.totalInjected) {
        return false;
    }

    for (size_t i = 0; i < segments_.size(); ++i) {
        segments_[i].discharge = snap.segs[i].discharge;
        segments_[i].storage = snap.segs[i].storage;
        segments_[i].conveyance = snap.segs[i].conveyance;
    }
    totalStorage_ = snap.totalStorage;
    totalInjected_ = snap.totalInjected;
    totalOutflowToOutlets_ = snap.totalOutflowToOutlets;
    totalWithdrawn_ = snap.totalWithdrawn;
    return true;
}

// ---------------------------------------------------------------------------
// RiverNetState -- see rivernet.h's PERSISTENCE section for the byte layout
// ---------------------------------------------------------------------------
namespace {

// ByteWriter carries no i64 (nothing needed one until now), and adding one to
// bytes.h would touch a header five other wire formats share. Casting through
// u64 here is exact -- two's complement round-trips bit for bit -- and keeps
// the change inside this file.
void writeI64(ByteWriter& w, int64_t v) { w.u64(static_cast<uint64_t>(v)); }
bool readI64(ByteReader& r, int64_t& v) {
    uint64_t u = 0;
    if (!r.u64(u)) return false;
    v = static_cast<int64_t>(u);
    return true;
}

} // namespace

void RiverNetState::serialize(const RiverNetwork& net, std::vector<uint8_t>& out) {
    ByteWriter w(out);
    w.u32(kMagic);
    w.u32(kRiverNetVersion);

    const std::vector<RiverDiffRecord>& diffs = net.diffLog();
    w.u32(static_cast<uint32_t>(diffs.size()));
    for (const RiverDiffRecord& d : diffs) {
        w.u8(static_cast<uint8_t>(d.kind));
        w.u32(d.segId);
        w.u8(d.value);
        w.u32(d.headNode);
        w.u32(static_cast<uint32_t>(d.course.size()));
        for (const RiverChannelPoint& p : d.course) {
            writeI64(w, p.vx);
            writeI64(w, p.vy);
            w.i32(p.elevationMm);
            w.u32(p.existingNode);
        }
    }

    const RiverNetwork::RoutingSnapshot snap = net.captureRoutingState();
    w.u32(static_cast<uint32_t>(snap.segs.size()));
    for (const RiverNetwork::RoutingSnapshot::Seg& s : snap.segs) {
        w.i32(s.discharge);
        w.i32(s.storage);
        w.u8(s.conveyance);
    }
    writeI64(w, snap.totalStorage);
    writeI64(w, snap.totalInjected);
    writeI64(w, snap.totalOutflowToOutlets);
    writeI64(w, snap.totalWithdrawn);
}

bool RiverNetState::load(const uint8_t* data, size_t size, RiverNetwork& net) {
    ByteReader r(data, size);
    uint32_t magic = 0, version = 0, diffCount = 0;
    if (!r.u32(magic) || magic != kMagic) return false;
    if (!r.u32(version) || version != kRiverNetVersion) return false;
    if (!r.u32(diffCount)) return false;
    // A diff is at least 14 bytes on the wire, so a count that could not
    // possibly be backed by the remaining bytes fails here rather than after
    // reserving memory for it.
    if (static_cast<size_t>(diffCount) > size / 14 + 1) return false;

    std::vector<RiverDiffRecord> diffs;
    diffs.reserve(diffCount);
    for (uint32_t i = 0; i < diffCount; ++i) {
        RiverDiffRecord d;
        uint8_t kind = 0;
        uint32_t courseLen = 0;
        if (!r.u8(kind) || !r.u32(d.segId) || !r.u8(d.value) || !r.u32(d.headNode) ||
            !r.u32(courseLen)) {
            return false;
        }
        if (kind > static_cast<uint8_t>(RiverDiffKind::kDivertChannel)) return false;
        d.kind = static_cast<RiverDiffKind>(kind);
        if (static_cast<size_t>(courseLen) > size / 24 + 1) return false;
        d.course.resize(courseLen);
        for (uint32_t c = 0; c < courseLen; ++c) {
            if (!readI64(r, d.course[c].vx) || !readI64(r, d.course[c].vy) ||
                !r.i32(d.course[c].elevationMm) || !r.u32(d.course[c].existingNode)) {
                return false;
            }
        }
        diffs.push_back(std::move(d));
    }

    uint32_t segCount = 0;
    if (!r.u32(segCount)) return false;
    if (static_cast<size_t>(segCount) > size / 9 + 1) return false;
    RiverNetwork::RoutingSnapshot snap;
    snap.segs.resize(segCount);
    for (uint32_t i = 0; i < segCount; ++i) {
        if (!r.i32(snap.segs[i].discharge) || !r.i32(snap.segs[i].storage) ||
            !r.u8(snap.segs[i].conveyance)) {
            return false;
        }
    }
    if (!readI64(r, snap.totalStorage) || !readI64(r, snap.totalInjected) ||
        !readI64(r, snap.totalOutflowToOutlets) || !readI64(r, snap.totalWithdrawn)) {
        return false;
    }
    if (!r.atEnd()) return false;

    // Diffs FIRST: kDivertChannel adds segments, so the snapshot's count is a
    // statement about the POST-replay graph. Replayed through applyGraphDiff so
    // the load path and the live path are the same code -- which is what makes
    // the round trip byte-exact rather than merely plausible.
    for (const RiverDiffRecord& d : diffs) net.applyGraphDiff(d);
    return net.restoreRoutingState(snap);
}

void RiverNetwork::digest(Digest& d) const {
    for (const RiverSegment& s : segments_) {
        d.u32(s.fromNode);
        d.u32(s.toNode);
        d.i64(s.lengthMm);
        d.u32(static_cast<uint32_t>(s.discharge));
        d.i64(s.storage);
        d.u8(s.conveyance);
        d.i64(s.travelMillis);
    }
}

} // namespace vxc
