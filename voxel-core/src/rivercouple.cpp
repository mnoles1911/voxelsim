#include "voxelcore/rivercouple.h"

#include <algorithm>

namespace vxc {
namespace {

// THE SAME fixed compass tie-break order rivernet.cpp's D8 generation uses
// (N,NE,E,SE,S,SW,W,NW). Duplicated deliberately rather than exported from
// rivernet.cpp: the promotion search is a different algorithm that merely
// happens to want the same determinism contract, and coupling the two through
// a shared symbol would make a future change to one silently change the other.
constexpr int kD8Count = 8;
constexpr int64_t kD8Dx[kD8Count] = {0, 1, 1, 1, 0, -1, -1, -1};
constexpr int64_t kD8Dy[kD8Count] = {-1, -1, 0, 1, 1, 1, 0, -1};

} // namespace

RiverCaCoupler::RiverCaCoupler(RiverNetwork& net, WaterCA& ca, ITileSampler& tiles, SolidFn solid,
                               const RiverCoupleConfig& cfg)
    : net_(net), ca_(ca), tiles_(tiles), solid_(std::move(solid)), cfg_(cfg) {
    pixelSizeMm_ = std::max<int64_t>(1, tiles_.pixelSizeMm());
}

int64_t RiverCaCoupler::pixelOfVoxel(int64_t v) const {
    return floorDiv(v * kVoxelSizeMm, pixelSizeMm_);
}

int64_t RiverCaCoupler::voxelOfPixel(int64_t p) const {
    return floorDiv(p * pixelSizeMm_, kVoxelSizeMm);
}

void RiverCaCoupler::refreshNodeIndex() {
    if (nodeIndexSize_ == net_.nodes().size()) return;
    nodeOfPixel_.clear();
    const std::vector<RiverNode>& nodes = net_.nodes();
    for (size_t i = 0; i < nodes.size(); ++i)
        nodeOfPixel_[{pixelOfVoxel(nodes[i].vx), pixelOfVoxel(nodes[i].vy)}] =
            static_cast<uint32_t>(i);
    nodeIndexSize_ = nodes.size();
}

uint32_t RiverCaCoupler::nodeAtPixel(int64_t px, int64_t py) const {
    const auto it = nodeOfPixel_.find({px, py});
    return it == nodeOfPixel_.end() ? RiverNetwork::kNoNode : it->second;
}

std::vector<RiverDiffRecord> RiverCaCoupler::takePendingDiffs() {
    std::vector<RiverDiffRecord> out;
    out.swap(pendingDiffs_);
    return out;
}

int32_t RiverCaCoupler::bestCandidateDwell() const {
    int32_t best = 0;
    for (const Candidate& c : candidates_) best = std::max(best, c.dwell);
    return best;
}

void RiverCaCoupler::step() {
    if (!cfg_.enabled) return;
    outfallWrites_.clear();
    injectPass();
    if (cfg_.promotionEnabled) promotionPass();
}

// ===========================================================================
// Channel 1: graph -> CA (rivercouple.h section 2), and the ocean sink
// (section 3).
// ===========================================================================

void RiverCaCoupler::injectPass() {
    const size_t n = net_.segments().size();
    if (n == 0) return;
    // Round-robin with a bounded per-tick budget, so a continent-sized network
    // costs exactly what a valley does per tick and simply takes more ticks to
    // sweep. min() so a small network is not serviced twice in one tick, which
    // would double its effective hand-over rate.
    const size_t budget =
        std::min<size_t>(static_cast<size_t>(std::max(0, cfg_.maxOutfallsPerTick)), n);
    for (size_t i = 0; i < budget; ++i) {
        const uint32_t segId = static_cast<uint32_t>(injectCursor_ % n);
        injectCursor_ = (injectCursor_ + 1) % n;
        serviceSegment(segId);
    }
}

void RiverCaCoupler::serviceSegment(uint32_t segId) {
    // Copied out, not held by reference: withdrawToCoupler/refundFromCoupler
    // mutate the segment this reads.
    const RiverSegment seg = net_.segments()[segId];
    const RiverNode to = net_.nodes()[seg.toNode];

    const int64_t seaMm = static_cast<int64_t>(cfg_.seaLevelVz) * kVoxelSizeMm;

    // --- section 3, first half: this reach ends in the sea. -----------------
    // The sea has no storage and no voxel data (AVoxelOceanActor is an
    // implicit plane), so the reach's whole contents leave the world here.
    // Ledgered, not deleted: graphUnitsToOcean() is a term of the section-1
    // invariant. No cap -- what limits the rate to the sea is upstream
    // routing, which is where a rate limit belongs.
    if (to.elevationMm <= seaMm) {
        toOcean_ += net_.withdrawToCoupler(segId, seg.storage);
        return;
    }
    if (seg.storage <= 0) return;

    // --- resolve the outfall floor -----------------------------------------
    // Scan DOWN from a little above the node's tile elevation for the first
    // solid voxel and stand the water on top of it. The scan (rather than
    // "place at the tile surface") is what puts the water in the carved bed
    // instead of on the bank. No solid in the window means the node hangs over
    // a void: place at the window floor and let the CA's gravity do the rest.
    // TWO descents, not one, and the second is the reason: the window's top
    // can itself be solid (an overhang, a bridge, a roofed channel), and a
    // single "first solid from the top, stand on it" scan would then hand back
    // a z that is INSIDE the rock -- where addWater places exactly nothing and
    // the reach silently never delivers. So skip any overburden first, then
    // descend through the air to the real floor.
    const int64_t vx = to.vx, vy = to.vy;
    const int64_t topVz = floorDiv(to.elevationMm, kVoxelSizeMm) + cfg_.outfallAboveVoxels;
    const int64_t bottomVz = topVz - cfg_.outfallScanVoxels;
    int64_t z = topVz;
    while (z >= bottomVz && solidAt(vx, vy, z)) --z;
    if (z < bottomVz) return; // solid from top to bottom: nowhere to put water
    while (z >= bottomVz && !solidAt(vx, vy, z)) --z;
    const int64_t floorVz = (z >= bottomVz) ? z + 1 : bottomVz;

    // --- section 3, second half: the floor itself is at/below sea level. ----
    // A node above the waterline whose channel bed is not (a cliff outfall, a
    // river mouth cut below the tide) is still a river reaching the sea, and
    // writing CA fill at or below z==0 would build a pool inside the ocean
    // plane that nothing ever drains.
    if (floorVz <= cfg_.seaLevelVz) {
        toOcean_ += net_.withdrawToCoupler(segId, seg.storage);
        return;
    }

    // --- TOP-UP, not pour (section 2) --------------------------------------
    int32_t present = 0;
    for (int32_t k = 0; k < cfg_.outfallDepthVoxels; ++k)
        present += ca_.fillAt(vx, vy, floorVz + k);

    const int32_t target =
        clampi32(static_cast<int64_t>(cfg_.minTargetFill) +
                     static_cast<int64_t>(seg.discharge) / std::max(1, cfg_.dischargePerFillUnit),
                 cfg_.minTargetFill, cfg_.maxTargetFill);
    const int32_t shortfall = target - present;
    if (shortfall <= 0) return; // the channel is already as wet as this reach can make it

    const int32_t rate = std::max(1, cfg_.graphUnitsPerFill);
    int32_t wantFill = std::min(shortfall, std::max(0, cfg_.maxFillPerSegmentPerTick));
    // Only WHOLE multiples of `rate` are ever withdrawn, so the unit
    // conversion has no remainder to lose. Sub-multiple residue stays as
    // segment storage, which is correct: it is water still in the reach.
    wantFill = static_cast<int32_t>(
        std::min<int64_t>(wantFill, static_cast<int64_t>(seg.storage) / rate));
    if (wantFill <= 0) return;

    const int32_t took = net_.withdrawToCoupler(segId, wantFill * rate);
    const int32_t fillToPlace = took / rate;
    const uint32_t placed = ca_.addWater(vx, vy, floorVz, static_cast<uint32_t>(fillToPlace));

    // Whatever the CA refused goes straight back to the reach it came from --
    // exact, and physically the back-pressure that makes a blocked outfall
    // raise its own stage (rivercouple.h section 1, REFUSAL).
    const int32_t giveBack = took - static_cast<int32_t>(placed) * rate;
    if (giveBack > 0) refunded_ += net_.refundFromCoupler(segId, giveBack);

    toCa_ += static_cast<int64_t>(placed) * rate;
    fillDelivered_ += placed;
    if (placed > 0) outfallWrites_.push_back(RiverOutfallWrite{vx, vy, floorVz, placed});
}

// ===========================================================================
// Channel 2: CA -> graph (rivercouple.h section 4). Moves ZERO units.
// ===========================================================================

int64_t RiverCaCoupler::sampleWetFill(int64_t px, int64_t py, int32_t elevationMm,
                                      bool& outActive) const {
    // An evenly-spaced grid of sample columns spanning the pixel, rather than
    // the pixel's whole footprint: at a 30 m pixel and 10 cm voxels a footprint
    // is 300x300 columns, which is not a thing to scan per candidate per tick.
    const int64_t pixVox = std::max<int64_t>(1, pixelSizeMm_ / kVoxelSizeMm);
    const int32_t k = std::max(1, cfg_.sampleGrid);
    const int64_t stride = std::max<int64_t>(1, pixVox / (k + 1));
    const int64_t baseVx = voxelOfPixel(px), baseVy = voxelOfPixel(py);
    const int64_t topVz = floorDiv(elevationMm, kVoxelSizeMm) + cfg_.sampleAboveVoxels;
    const int32_t depth = std::max(1, cfg_.sampleColumnVoxels);

    int64_t sum = 0;
    BrickKey lastKey{0, 0, 0};
    bool haveLastKey = false;
    for (int32_t iy = 0; iy < k; ++iy) {
        for (int32_t ix = 0; ix < k; ++ix) {
            const int64_t sx = baseVx + (ix + 1) * stride;
            const int64_t sy = baseVy + (iy + 1) * stride;
            for (int32_t dz = 0; dz < depth; ++dz) {
                const int64_t sz = topVz - dz;
                const uint8_t f = ca_.fillAt(sx, sy, sz);
                if (f == 0) continue;
                sum += f;
                if (outActive) continue;
                // "Still moving": WaterCA's active set IS the set of bricks
                // that changed on the last step (waterca.h, "Activity /
                // settling"), so a settled body falls out of it and a running
                // one does not. Deduped against the previous key because a
                // descending z scan revisits the same brick 8 cells running.
                const BrickKey key = waterKeyForVoxel(sx, sy, sz);
                if (haveLastKey && key == lastKey) continue;
                lastKey = key;
                haveLastKey = true;
                if (ca_.activeBricks().count(key) != 0) outActive = true;
            }
        }
    }
    return sum;
}

bool RiverCaCoupler::pixelIsWet(int64_t px, int64_t py, int32_t elevationMm,
                                bool& outActive) const {
    return sampleWetFill(px, py, elevationMm, outActive) >= cfg_.minWetFillPerPixel;
}

bool RiverCaCoupler::verifyCandidate(Candidate& c) const {
    if (c.headNode >= net_.nodes().size() || c.course.empty()) return false;
    if (static_cast<int32_t>(c.course.size()) < cfg_.minChannelPixels) return false; // (4)

    const RiverNode head = net_.nodes()[c.headNode];
    bool anyActive = false;

    // (1) IT LEAVES A RIVER: the take-off is a real node and is itself wet.
    if (!pixelIsWet(pixelOfVoxel(head.vx), pixelOfVoxel(head.vy), head.elevationMm, anyActive))
        return false;

    int32_t prevElev = head.elevationMm;
    for (size_t i = 0; i < c.course.size(); ++i) {
        RiverChannelPoint& p = c.course[i];
        if (p.elevationMm >= prevElev) return false; // (3) strictly descending
        prevElev = p.elevationMm;

        const int64_t px = pixelOfVoxel(p.vx), py = pixelOfVoxel(p.vy);
        if (!pixelIsWet(px, py, p.elevationMm, anyActive)) return false; // (5) wet

        // Re-resolve against the CURRENT node set every tick: another
        // promotion may have turned one of these pixels into a node since
        // discovery, and re-using a stale kNoNode would create a duplicate
        // node on top of an existing one.
        const uint32_t at = nodeAtPixel(px, py);
        const bool isLast = (i + 1 == c.course.size());
        if (at != RiverNetwork::kNoNode && !isLast) return false;
        p.existingNode = isLast ? at : RiverNetwork::kNoNode;
    }

    // (2) IT REACHES A SINK: an existing node, or the sea.
    const RiverChannelPoint& tail = c.course.back();
    if (tail.existingNode == RiverNetwork::kNoNode &&
        tail.elevationMm > static_cast<int64_t>(cfg_.seaLevelVz) * kVoxelSizeMm)
        return false;

    // (5), the flux half: something along the course is still changing.
    return anyActive;
}

bool RiverCaCoupler::discoverFrom(uint32_t nodeId, Candidate& out) const {
    const RiverNode head = net_.nodes()[nodeId];
    const int64_t hpx = pixelOfVoxel(head.vx), hpy = pixelOfVoxel(head.vy);
    const int64_t seaMm = static_cast<int64_t>(cfg_.seaLevelVz) * kVoxelSizeMm;

    bool headActive = false;
    if (!pixelIsWet(hpx, hpy, head.elevationMm, headActive)) return false; // (1)

    for (int k = 0; k < kD8Count; ++k) {
        const int64_t qx = hpx + kD8Dx[k], qy = hpy + kD8Dy[k];
        // The first step must land on a pixel that is NOT already part of the
        // network. That single test excludes the main stem (whose target is a
        // node), any channel already promoted from this node, and any
        // neighbouring river -- so a "diversion" can never be the existing
        // course restated, and promoteChannel's own rejection of that case
        // becomes a backstop rather than the only guard.
        if (nodeAtPixel(qx, qy) != RiverNetwork::kNoNode) continue;
        const int32_t qe = tiles_.elevationMm(qx, qy);
        if (qe >= head.elevationMm) continue; // (3)
        bool anyActive = headActive;
        if (!pixelIsWet(qx, qy, qe, anyActive)) continue; // (5)

        std::vector<RiverChannelPoint> course;
        int64_t cx = qx, cy = qy;
        int32_t ce = qe;
        bool terminated = false;
        while (true) {
            RiverChannelPoint p;
            p.vx = voxelOfPixel(cx);
            p.vy = voxelOfPixel(cy);
            p.elevationMm = ce;
            p.existingNode = nodeAtPixel(cx, cy);
            course.push_back(p);
            if (p.existingNode != RiverNetwork::kNoNode) { // (2) rejoins the network
                terminated = true;
                break;
            }
            if (ce <= seaMm) { // (2) reaches the sea
                terminated = true;
                break;
            }
            if (static_cast<int32_t>(course.size()) >= cfg_.maxChannelPixels) break;

            bool found = false;
            for (int m = 0; m < kD8Count; ++m) {
                const int64_t nx = cx + kD8Dx[m], ny = cy + kD8Dy[m];
                if (nx == hpx && ny == hpy) continue;
                bool dup = false;
                for (const RiverChannelPoint& e : course)
                    if (e.vx == voxelOfPixel(nx) && e.vy == voxelOfPixel(ny)) {
                        dup = true;
                        break;
                    }
                if (dup) continue;
                const int32_t ne = tiles_.elevationMm(nx, ny);
                if (ne >= ce) continue; // (3): strict descent also forbids a cycle
                bool a = false;
                if (!pixelIsWet(nx, ny, ne, a)) continue; // (5)
                anyActive = anyActive || a;
                cx = nx;
                cy = ny;
                ce = ne;
                found = true;
                break;
            }
            if (!found) break; // the wet chain dead-ends: a reservoir, not a channel
        }

        if (!terminated) continue;                                          // (2)
        if (static_cast<int32_t>(course.size()) < cfg_.minChannelPixels) continue; // (4)
        if (!anyActive) continue;                                           // (5)

        out.headNode = nodeId;
        out.course = std::move(course);
        out.dwell = 0;
        return true;
    }
    return false;
}

void RiverCaCoupler::promotionPass() {
    refreshNodeIndex();

    // --- 1. re-verify every tracked candidate, from scratch ----------------
    // A single failing tick DISCARDS the candidate rather than decrementing
    // it, so the criterion cannot be met by intermittent luck (rivercouple.h
    // section 4).
    std::vector<Candidate> keep;
    keep.reserve(candidates_.size());
    for (Candidate& c : candidates_) {
        if (!verifyCandidate(c)) continue;
        ++c.dwell;
        if (c.dwell < cfg_.sustainTicks) {
            keep.push_back(std::move(c));
            continue;
        }
        const uint32_t newSeg = net_.promoteChannel(c.headNode, c.course);
        if (newSeg != RiverNetwork::kNoSegment) {
            ++promotions_;
            RiverDiffRecord d;
            d.kind = RiverDiffKind::kDivertChannel;
            d.headNode = c.headNode;
            d.course = c.course;
            pendingDiffs_.push_back(std::move(d));
            refreshNodeIndex(); // promotion appended nodes
        }
        // Promoted, or rejected outright by promoteChannel's validation --
        // either way this candidate is finished. Re-discovery will find it
        // again next sweep if it is still a real channel and was merely
        // rejected for a reason that has since cleared.
    }
    candidates_.swap(keep);

    // --- 2. discover new candidates, round-robin over nodes ----------------
    const size_t nn = net_.nodes().size();
    if (nn == 0) return;
    const int32_t maxTracked = std::max(0, cfg_.maxTrackedCandidates);
    for (int32_t s = 0; s < cfg_.scansPerTick; ++s) {
        if (static_cast<int32_t>(candidates_.size()) >= maxTracked) break;
        const uint32_t nodeId = static_cast<uint32_t>(scanCursor_ % nn);
        scanCursor_ = (scanCursor_ + 1) % nn;

        bool already = false;
        for (const Candidate& c : candidates_)
            if (c.headNode == nodeId) already = true;
        if (already) continue;

        Candidate c;
        if (!discoverFrom(nodeId, c)) continue;
        c.dwell = 1; // this tick's observation is the first of `sustainTicks`
        candidates_.push_back(std::move(c));
    }
}

} // namespace vxc
