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

} // namespace

void RiverNetwork::buildFromFlowAccumulation(ITileSampler& tiles, uint64_t seed,
                                             const RegionBounds& b, int64_t accumThreshold) {
    nodes_.clear();
    segments_.clear();
    outgoingSegmentOfNode_.clear();
    totalStorage_ = 0;
    totalInjected_ = 0;
    totalOutflowToOutlets_ = 0;
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

    std::vector<int64_t> accum(count, 0);
    for (size_t oi : order) {
        accum[oi] += precip[oi];
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
    int64_t outletDeltaThisTick = 0;
    for (size_t i = 0; i < n; ++i) {
        RiverSegment& s = segments_[i];
        const int32_t of = outflow[i];
        s.storage -= of;
        s.discharge = of;
        const uint32_t downSeg = outgoingSegmentOfNode_[s.toNode];
        if (downSeg != kNoSegment) {
            segments_[downSeg].storage += of;
        } else {
            outletDeltaThisTick += of;
        }
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
            // Documented stub -- see rivernet.h header comment "Hydrology
            // graph-diff": promoting sustained CA flux to a new segment
            // needs live CA data this graph-only layer doesn't have.
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
