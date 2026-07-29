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
    totalStorage_ = 0;
    totalInjected_ = 0;
    totalOutflowToOutlets_ = 0;
    totalWithdrawn_ = 0;
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

void RiverNetwork::injectInflow(uint32_t segId, int32_t amount) {
    if (segId >= segments_.size() || amount <= 0) return;
    segments_[segId].storage += amount;
    totalStorage_ += amount;
    totalInjected_ += amount;
}

void RiverNetwork::setConveyance(uint32_t segId, uint8_t factor0to255) {
    if (segId >= segments_.size()) return;
    segments_[segId].conveyance = factor0to255;
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
