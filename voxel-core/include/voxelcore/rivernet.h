#pragma once
// River network segment-graph sim (plan §3.7 Layer R / §3.7 Generation, W3
// groundwork): engine-free, integer/fixed-point CPU reference for the
// COARSE tile-scale hydrology graph that ticks ~1Hz ABOVE the per-voxel
// water pressure CA (waterca.h) -- this header has no dependency on
// waterca.h and is not itself the volume-conservation authority for voxel
// water; it is the sim layer the plan describes as producing dam/divert
// gameplay behavior (upstream stage rise, downstream discharge decay,
// burst-dam flood spikes) that a later W3-proper pass will read to drive
// CA/SWE and per-voxel channel carving. Terrain-free the same way
// waterca.h is terrain-free: the only external dependency is ITileSampler
// (tiles.h), queried for elevation + the precipitation climate channel.
//
// -----------------------------------------------------------------------
// Graph model
// -----------------------------------------------------------------------
// A RiverNode is a junction/point on the network (one qualifying tile
// pixel). A RiverSegment is a directed edge carrying flow from its
// (higher) fromNode to its (lower) toNode. buildFromFlowAccumulation
// gives each node AT MOST ONE outgoing segment (its single D8
// steepest-descent target), so the whole graph is a FOREST of chains/trees
// draining toward the low edge of the queried region (a node may still
// have any number of INCOMING segments, at a confluence). Node and
// segment ids are simply their index into nodes()/segments(), assigned in
// a fixed, position-ordered traversal (see buildFromFlowAccumulation) --
// fully deterministic given (tiles, regionBounds, accumThreshold), and
// independent of any hashing/iteration-order accident.
//
// -----------------------------------------------------------------------
// buildFromFlowAccumulation (v0 generation, plan §3.7 Generation)
// -----------------------------------------------------------------------
// Coarse, tile-pixel-scale D8 flow accumulation over an inclusive pixel
// rectangle (regionBounds), NOT a per-voxel operation:
//   1. Sample elevationMm + climate().precipitation for every pixel in
//      bounds.
//   2. D8 direction: each pixel's steepest-descent neighbor is simply the
//      lowest-elevation neighbor among the up-to-8 compass neighbors that
//      also lie inside regionBounds (out-of-bounds neighbors are never
//      sampled -- a pixel whose true downhill path would leave the region
//      just becomes a terminal/sink node with no outgoing segment, which
//      is exactly the desired "reaches the low edge of the queried
//      region" behavior with no special-casing). Ties broken by a FIXED
//      compass priority order (N,NE,E,SE,S,SW,W,NW) -- see rivernet.cpp.
//      This is a v0 simplification: true steepest descent normalizes drop
//      by distance (diagonal neighbors are ~1.41x farther); v0 instead
//      picks purely by lowest neighbor elevation, ignoring the
//      distance factor. Segment LENGTH still accounts for the diagonal
//      distance (integer sqrt(2) ~= 181/128 approximation, no floats).
//   3. Flow accumulation: pixels are visited in strictly-descending
//      elevation order (ties broken by position for full determinism).
//      Because every D8 edge points to a STRICTLY lower pixel, this order
//      guarantees every uphill contributor to a pixel is visited (and has
//      already routed its flow downhill) before the pixel itself is
//      visited -- the standard flow-accumulation topological-order trick,
//      no separate graph-traversal pass needed. Each pixel's accumulated
//      value = its own precipitation channel value + whatever was already
//      routed into it by uphill neighbors; it is then routed onward in
//      full to its own D8 target (if any).
//   4. Threshold: a pixel becomes a RiverNode iff its accumulation >=
//      accumThreshold. A RiverSegment connects two threshold-qualifying
//      pixels along a D8 edge; a qualifying pixel whose D8 target does not
//      itself qualify (or has none) is a terminal node -- exactly the
//      "coarse graph reaching the low edge / a basin sink" case.
//   5. discharge = f(accumulated upstream precip) = the accumulation value
//      itself (clamped to int32 range) at build time; step() below
//      repurposes the SAME field to mean "the routed outflow this tick"
//      once the network starts ticking (both are legitimately "current
//      flow through this segment", just from two different sources of
//      truth -- static estimate at generation time vs. live routed state).
//
// -----------------------------------------------------------------------
// step() -- Muskingum-class storage routing
// -----------------------------------------------------------------------
// Each segment's Muskingum K (the reach's travel-time constant) is fixed
// at build time from its length (travelMillis, see rivernet.cpp) --
// longer segments lag/store more, matching the physical role K plays in
// real Muskingum routing. v0 implements the X=0 special case (Muskingum
// reduces to a simple linear reservoir: outflow proportional to storage,
// no separate weighting between inflow/outflow history) since it is
// integer-exact, trivially conservative, and needs no subtraction that
// could go negative -- a documented, legitimate simplification, not the
// full 3-coefficient Muskingum equation.
//
// One step(dtMillis) call is a TWO-PHASE read-then-apply pass over every
// segment (mirrors waterca.h's two-phase contract, for the same reason:
// results must not depend on iteration order over segments even though a
// confluence node can have several segments simultaneously writing into
// the SAME downstream segment's storage this tick):
//   READ:  for every segment, compute this tick's outflow purely from
//     tick-START storage: outflow = clamp(storage * dtMillis / travelMillis
//     * conveyance / 255, 0, storage). setConveyance(seg, 0) forces this to
//     exactly 0 regardless of storage -- "full dam".
//   APPLY: for every segment, subtract its own computed outflow from its
//     own storage and add that SAME amount to the downstream segment
//     rooted at its toNode (outgoingSegment(toNode)) if one exists, else
//     it leaves the graph permanently and is recorded in
//     totalOutflowToOutlets(). Because every added amount was computed
//     against a snapshot taken before ANY apply write this tick, multiple
//     segments feeding one confluence never race or double-count, and the
//     whole pass is a pure function of (which segments exist, their
//     tick-start storage/conveyance) -- not of iteration order.
// This gives the dam behavior the plan describes for free, with no
// special-casing: force a segment's conveyance to 0 -> its own outflow is
// permanently 0 -> its storage only ever grows from upstream inflow
// (monotonically non-decreasing -- this IS "upstream stage rises", the
// segment immediately behind the dam doubling as the numeric proxy a
// later system would read to decide when to spawn a UE reservoir entity)
// -> the segment immediately downstream receives zero NEW inflow from the
// dammed segment, so its own storage (and therefore discharge) decays
// toward zero at its own routing rate, exactly "downstream discharge
// decays, beds dry over minutes" from plan §3.7.
//
// -----------------------------------------------------------------------
// Conservation
// -----------------------------------------------------------------------
// totalStorage() is an O(1) running ledger (like waterca's totalVolume()):
// injectInflow() increases it and totalInjected(); step() only ever moves
// storage between segments (net zero) except for whatever leaves via
// outlet segments, which is subtracted from totalStorage() and added to
// totalOutflowToOutlets() in the same step() call. The exact-integer
// invariant that always holds, after any sequence of injectInflow()/
// step() calls: totalStorage() + totalOutflowToOutlets() == totalInjected().
// recomputeTotalStorage() independently re-sums every segment's storage
// for cross-checking the ledger (tests only, not hot-path).
//
// -----------------------------------------------------------------------
// Hydrology graph-diff (persistent replicated log hook)
// -----------------------------------------------------------------------
// RiverDiffRecord is the minimal, real, documented hook for what plan
// §3.7 calls "hydrology graph = base (from seed) + persistent replicated
// graph-diff log". v0 implements exactly one REAL diff kind
// (kSetConveyance -- "a dam placed[, removed, or adjusted]"), which
// applyGraphDiff() replays through the exact same setConveyance() path a
// live caller would use, so replaying a log of these diffs against a
// freshly-built network (same seed/tiles/bounds) reproduces byte-identical
// state to whatever produced the diff live (see
// tests/test_rivernet.cpp's replay test). kDivertChannel ("a sustained CA
// flux promotes a new channel to a segment") is DOCUMENTED but
// intentionally a no-op stub today: promoting a channel means synthesizing
// a brand-new node/segment from live CA flux data this graph-only layer
// does not have access to (that coupling is W3-proper, after waterca.h and
// this header are wired together). Full network replication (sending
// these diffs over the wire) is M3-water integration, later still --
// this is only the local apply-side hook the plan calls for now.

#include <cstdint>
#include <vector>

#include "voxelcore/core.h"
#include "voxelcore/tiles.h"

namespace vxc {

// Bumped on any deliberate change to the generation (D8/accumulation) or
// routing (Muskingum-class step) math above -- invalidates saved graph
// diffs and golden digests, exactly like kWorldGenVersion/kWaterCAVersion.
inline constexpr uint32_t kRiverNetVersion = 1;

struct RiverNode {
    int64_t vx = 0, vy = 0; // world VOXEL coords (doctrine convention, core.h) of the source tile pixel's center
    int32_t elevationMm = 0;

    friend bool operator==(const RiverNode&, const RiverNode&) = default;
};

struct RiverSegment {
    uint32_t fromNode = 0, toNode = 0; // upstream -> downstream node ids
    int64_t lengthMm = 0;              // planar reach length (D8 cardinal/diagonal step, integer sqrt2 approx)
    int32_t discharge = 0;             // build-time: f(accumulated upstream precip); post-step(): this tick's routed outflow
    int32_t storage = 0;               // Muskingum storage state, integer units
    uint8_t conveyance = 255;          // 0..255; 0 = full dam (outflow forced to 0), 255 = free flow
    int64_t travelMillis = 1;          // Muskingum K as a travel-time constant (millis), fixed at build time from lengthMm

    friend bool operator==(const RiverSegment&, const RiverSegment&) = default;
};

// Inclusive tile-pixel rectangle (same pixel space as ITileSampler).
struct RegionBounds {
    int64_t px0 = 0, py0 = 0, px1 = 0, py1 = 0;
};

// Hydrology graph-diff kinds (see header comment "Hydrology graph-diff").
enum class RiverDiffKind : uint8_t {
    kSetConveyance = 0, // dam placed/removed/adjusted: segId + new conveyance value. REAL, replayable.
    kDivertChannel = 1, // channel diversion promoting CA flux to a new segment. Documented STUB (no-op) today.
};

struct RiverDiffRecord {
    RiverDiffKind kind = RiverDiffKind::kSetConveyance;
    uint32_t segId = 0;
    uint8_t value = 255; // kSetConveyance: the new conveyance factor

    friend bool operator==(const RiverDiffRecord&, const RiverDiffRecord&) = default;
};

class RiverNetwork {
public:
    static constexpr uint32_t kNoSegment = 0xFFFFFFFFu;

    // v0 coarse generation (see header comment). Clears any previously
    // built graph and ledgers. accumThreshold is the minimum accumulated
    // (precip-weighted upstream) value for a pixel to become a RiverNode.
    void buildFromFlowAccumulation(ITileSampler& tiles, uint64_t seed,
                                   const RegionBounds& bounds,
                                   int64_t accumThreshold = 500);

    const std::vector<RiverNode>& nodes() const { return nodes_; }
    const std::vector<RiverSegment>& segments() const { return segments_; }
    uint32_t segmentCount() const { return static_cast<uint32_t>(segments_.size()); }

    // The segment rooted at `nodeId` (i.e. fromNode == nodeId), or
    // kNoSegment if that node has no outgoing segment (a terminal/outlet
    // node -- the graph's low edge or a basin sink).
    uint32_t outgoingSegment(uint32_t nodeId) const {
        return nodeId < outgoingSegmentOfNode_.size() ? outgoingSegmentOfNode_[nodeId] : kNoSegment;
    }

    // Adds `amount` storage units directly to segment `segId` (headwater
    // baseflow/precip stimulus, or any scripted test/gameplay inflow) --
    // the rivernet analogue of WaterCA::addWater. Ledger-tracked exactly:
    // increases both totalStorage() and totalInjected() by `amount`.
    // No-op for an out-of-range segId or a non-positive amount.
    void injectInflow(uint32_t segId, int32_t amount);

    // Sets segment `segId`'s conveyance factor (0 = full dam, 255 = free
    // flow). No-op for an out-of-range segId. Real, replayable via
    // applyGraphDiff(kSetConveyance).
    void setConveyance(uint32_t segId, uint8_t factor0to255);

    // One Muskingum-class routing tick over every segment (see header
    // comment "step() -- Muskingum-class storage routing"). dtMillis <= 0
    // is a no-op (every outflow computes to 0).
    void step(int64_t dtMillis);

    // Replays a hydrology graph-diff (see header comment). kSetConveyance
    // is real (dispatches to setConveyance); kDivertChannel is a
    // documented no-op stub today.
    void applyGraphDiff(const RiverDiffRecord& diff);

    // --- Conservation ledger (see header comment) ---
    int64_t totalStorage() const { return totalStorage_; }
    int64_t totalInjected() const { return totalInjected_; }
    int64_t totalOutflowToOutlets() const { return totalOutflowToOutlets_; }
    // Independent re-sum of every segment's storage, for cross-checking
    // totalStorage() (tests only; O(segments)).
    int64_t recomputeTotalStorage() const;

    // Deterministic digest over every segment, in id (== build/vector)
    // order -- already canonical, no separate sort needed (see header
    // comment "Graph model").
    void digest(Digest& d) const;

private:
    std::vector<RiverNode> nodes_;
    std::vector<RiverSegment> segments_;
    std::vector<uint32_t> outgoingSegmentOfNode_; // nodeId -> segId or kNoSegment
    uint64_t seed_ = 0; // stored for provenance / future hash-based generation refinements; the D8+accumulation math itself is purely elevation/precip-driven and needs no hashing today

    int64_t totalStorage_ = 0;
    int64_t totalInjected_ = 0;
    int64_t totalOutflowToOutlets_ = 0;
};

} // namespace vxc
