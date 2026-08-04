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
// graph-diff log". Both diff kinds the plan names are now REAL and
// replayable through applyGraphDiff():
//   * kSetConveyance -- "a dam placed[, removed, or adjusted]" -- dispatches
//     to the exact same setConveyance() path a live caller would use.
//   * kDivertChannel -- "a sustained CA flux promotes a new channel to a
//     segment" -- dispatches to promoteChannel() (below). It carries the
//     take-off node id AND the promoted COURSE (world-voxel positions +
//     elevations), because the whole point of a diff log is that replay must
//     not have to re-derive the course from live CA state that no longer
//     exists. The course IS the decision; re-running the detector is not.
// Replaying a log of these against a freshly-built network (same
// seed/tiles/bounds) reproduces byte-identical state to whatever produced
// the diffs live -- see tests/test_rivernet.cpp's dam replay test and
// tests/test_rivercouple.cpp's promotion replay test. Full network
// replication (sending these diffs over the wire) is M3-water integration,
// later still -- this is only the local apply-side hook the plan calls for.
// The DETECTOR that decides when to emit a kDivertChannel lives in
// voxelcore/rivercouple.h (RiverCaCoupler), never here: this header stays
// waterca-free by doctrine, exactly as it stays terrain-free.
//
// -----------------------------------------------------------------------
// Bifurcation (a node with more than one outgoing segment)
// -----------------------------------------------------------------------
// buildFromFlowAccumulation still gives every node AT MOST ONE outgoing
// segment, so a freshly built graph is the forest described above.
// promoteChannel() is the ONLY thing that can ever give a node a second
// outgoing segment, and it does so because that is what a diversion IS: the
// main stem keeps running and a new channel leaves the same node.
//
// step()'s APPLY phase then splits a segment's outflow EVENLY across its
// downstream node's outgoing segments (integer division; the remainder goes
// one unit each to the first `outflow % n` targets in a fixed order --
// primary first, then extras in promotion order, so the sum is exactly the
// outflow and the split is order-independent). Evenly, NOT
// conveyance-weighted, and that choice is load-bearing rather than lazy: a
// dammed reach (conveyance 0) must keep RECEIVING inflow or the reservoir
// behind the dam never fills and the "upstream stage rises" behaviour
// documented above silently dies. Conveyance gates a reach's OUTflow only.
// A cross-section-weighted split is the physically right answer and is
// deferred until the coarse graph carries a channel width at all.
//
// The split is behind `extraOutgoing_.empty()`, so a graph that has never
// been promoted runs byte-identical arithmetic to the pre-bifurcation
// version -- which is why the pinned golden did NOT move at
// kRiverNetVersion 3.
//
// -----------------------------------------------------------------------
// The graph->CA hand-off (withdrawToCoupler / refundFromCoupler)
// -----------------------------------------------------------------------
// A river's DISCHARGE is a number; the water a player sees is WaterCA fill.
// Turning the first into the second means units leaving this ledger, and
// the only safe way to do that is to make the departure explicit rather
// than to let a caller reach in and decrement `storage`. So there is a
// third ledger term, totalWithdrawnToCoupler(), and the exact-integer
// invariant that holds after ANY sequence of injectInflow/step/withdraw/
// refund calls is
//     totalStorage() + totalOutflowToOutlets() + totalWithdrawnToCoupler()
//         == totalInjected()
// refundFromCoupler() is the exact inverse of withdrawToCoupler() and
// exists because the CA is allowed to REFUSE water (a full cell, a solid
// cell). A coupler that could not give units back would have to either
// destroy them or invent a private holding tank; giving them back to the
// segment they came from is both exact and physically right -- a blocked
// outfall back-pressures the reach, its storage rises, and that is the same
// "stage rises" mechanism the dam already uses. See rivercouple.h.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "voxelcore/core.h"
#include "voxelcore/tiles.h"

namespace vxc {

// Bumped on any deliberate change to the generation (D8/accumulation) or
// routing (Muskingum-class step) math above -- invalidates saved graph
// diffs and golden digests, exactly like kWorldGenVersion/kWaterCAVersion.
// v2: flow accumulation weights by rainfall in MM/YR rather than the raw
// climate wire byte, so a river's catchment threshold means a physical
// quantity instead of an artifact of the u8 encoding. See rivernet.cpp.
//
// v3 (W3-proper coupling): promoteChannel() can now add nodes/segments to a
// live graph and give a node a SECOND outgoing segment, and
// withdrawToCoupler()/refundFromCoupler() move units out of and back into
// the storage ledger. NO GENERATION RULE AND NO ROUTING RULE CHANGED for a
// graph that is never promoted and never withdrawn from: the pinned golden
// (tests/test_rivernet.cpp, 0xEC84E0B592821C38) is BYTE-IDENTICAL to v2, and
// the split arithmetic is behind an `extraOutgoing_.empty()` test that is
// true for every graph buildFromFlowAccumulation can produce. This is a
// version bump for the same reason kWaterCAVersion went to 4 with no tick
// rule changed (see waterca.h): a LIVE world now evolves differently -- its
// topology can grow -- so a persisted graph-diff log or a recorded session
// from before this is no longer reproducible against it. Per
// docs/determinism.md that divergence is exactly what the constant signals.
inline constexpr uint32_t kRiverNetVersion = 3;

// Default minimum accumulated rainfall, in mm/yr summed over the upstream
// catchment, for a pixel to become a river node.
//
// DERIVED from the v1 value so river density is preserved for a given
// precipitation field: v1 accumulated the raw u8 byte with a threshold of 500,
// and one u8 step is climatePrecipMmPerYrFromU8(1) worth of rain, so the same
// catchment now sums 12000/255 times as much. 500 * 12000 / 255 = 23529.
//
// Note this preserves density with respect to the SAME precipitation field.
// The synthetic sampler's field also moved at worldgen v8 (it now emits the
// physical encoding rather than a byte centred on 128), so synthetic river
// density does change -- correctly, since the old synthetic precip decoded to
// a median of 5223 mm/yr, which is not a climate that exists.
inline constexpr int64_t kRiverAccumThresholdDefault =
    500 * (kClimatePrecipMaxMmPerYr - kClimatePrecipMinMmPerYr) / 255;

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

// Sea level for the graph. A node at or below this elevation is AT THE SEA,
// and the coupler treats it as the network's terminal sink (rivercouple.h).
//
// This used to be a second, independent `= 0`. It had ZERO references
// anywhere in the tree -- including channel.cpp, in this same subsystem,
// which tested against a bare `0` twice. That is the whole argument for
// core.h's kSeaLevelMm in one alias: a private copy of a shared datum does
// not get used, it gets re-typed. Kept as a name because "is this node in
// the ocean" is a graph question and the graph reads better for saying so.
inline constexpr int32_t kRiverSeaLevelMm = kSeaLevelMm;

// One point on a promoted channel's course: where it is, how high it is, and
// (for the LAST point only) which existing node it lands on. World VOXEL
// coords, matching RiverNode -- the detector works in voxels because that is
// where the CA lives, and the graph must be able to replay the decision
// without re-deriving it from a tile sampler.
struct RiverChannelPoint {
    int64_t vx = 0, vy = 0;
    int32_t elevationMm = 0;
    uint32_t existingNode = 0xFFFFFFFFu; // kNoNode unless this point IS an existing node

    friend bool operator==(const RiverChannelPoint&, const RiverChannelPoint&) = default;
};

// Hydrology graph-diff kinds (see header comment "Hydrology graph-diff").
enum class RiverDiffKind : uint8_t {
    kSetConveyance = 0, // dam placed/removed/adjusted: segId + new conveyance value.
    kDivertChannel = 1, // channel diversion: headNode + course. REAL since kRiverNetVersion 3.
};

struct RiverDiffRecord {
    RiverDiffKind kind = RiverDiffKind::kSetConveyance;
    uint32_t segId = 0;
    uint8_t value = 255; // kSetConveyance: the new conveyance factor

    // kDivertChannel only. `course` is empty for every kSetConveyance record,
    // so the common case costs one empty vector -- the same trade EditLog
    // makes for its variable-length payloads. An EMPTY course is a rejected
    // (and therefore inert) divert diff by construction, which is what keeps
    // a default-constructed kDivertChannel record a no-op.
    uint32_t headNode = 0;
    // The `{}` is load-bearing, not decoration. Every other member here has a
    // default initializer; without one, gcc and clang raise
    // -Werror=missing-field-initializers at EVERY brace-init that stops short
    // of `course` -- including designated ones -- while MSVC stays silent, so
    // the failure only ever appears in CI. Defaulting it here fixes all call
    // sites at once instead of making each one name a field it does not care
    // about.
    std::vector<RiverChannelPoint> course{};

    friend bool operator==(const RiverDiffRecord&, const RiverDiffRecord&) = default;
};

class RiverNetwork {
public:
    static constexpr uint32_t kNoSegment = 0xFFFFFFFFu;
    static constexpr uint32_t kNoNode = 0xFFFFFFFFu;

    // v0 coarse generation (see header comment). Clears any previously
    // built graph and ledgers. accumThreshold is the minimum accumulated
    // (precip-weighted upstream) value for a pixel to become a RiverNode.
    void buildFromFlowAccumulation(ITileSampler& tiles, uint64_t seed,
                                   const RegionBounds& bounds,
                                   int64_t accumThreshold = kRiverAccumThresholdDefault);

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

    // --- The graph->CA hand-off (see header comment) ----------------------
    //
    // Removes up to `amount` units from `segId`'s storage and moves them OUT
    // of this ledger into totalWithdrawnToCoupler(). Returns the amount
    // ACTUALLY removed, which is less than `amount` exactly when the segment
    // held less -- a caller keeping a two-sided ledger must use the return
    // value, never the request (same contract as WaterCA::addWater).
    int32_t withdrawToCoupler(uint32_t segId, int32_t amount);

    // The exact inverse: puts `amount` units back into `segId`'s storage and
    // out of totalWithdrawnToCoupler(). Clamped to what has actually been
    // withdrawn, so no sequence of calls can invent units. Returns the amount
    // actually refunded.
    int32_t refundFromCoupler(uint32_t segId, int32_t amount);

    // --- Channel promotion (kDivertChannel; see header comment) -----------
    //
    // Adds a new channel leaving EXISTING node `headNode` and running through
    // `course` (strictly descending, downstream order). Every course point
    // except the last must be a fresh position (existingNode == kNoNode); the
    // last may name an existing node, which is how a diversion REJOINS the
    // network. A course whose last point is fresh becomes a terminal node --
    // an outlet, exactly like a basin sink, which is what a channel running
    // to the sea is.
    //
    // Returns the id of the FIRST new segment (the take-off's new outflow),
    // or kNoSegment if the request was rejected -- in which case NOTHING is
    // mutated. Rejections, all of them conservative on purpose (a wrong
    // promotion corrupts routing permanently; a missed one costs nothing):
    //   * headNode out of range, or an empty course;
    //   * a course point that is not STRICTLY lower than its predecessor
    //     (segments are directed downhill by contract, and a flat or uphill
    //     edge would let step() route water in a cycle);
    //   * a non-final point naming an existing node, or any point naming an
    //     out-of-range node;
    //   * a duplicated position within the course, or a first point that
    //     duplicates the head's EXISTING downstream node -- i.e. a "diversion"
    //     that is really just the main stem again.
    // Replayable via applyGraphDiff(kDivertChannel).
    uint32_t promoteChannel(uint32_t headNode, const std::vector<RiverChannelPoint>& course);

    // How many outgoing segments node `nodeId` has (1 for an ordinary node,
    // 0 for an outlet, >1 only where promoteChannel made a bifurcation).
    uint32_t outgoingSegmentCount(uint32_t nodeId) const;

    // One Muskingum-class routing tick over every segment (see header
    // comment "step() -- Muskingum-class storage routing"). dtMillis <= 0
    // is a no-op (every outflow computes to 0).
    void step(int64_t dtMillis);

    // Replays a hydrology graph-diff (see header comment). kSetConveyance
    // dispatches to setConveyance; kDivertChannel dispatches to
    // promoteChannel (and is inert for the default-constructed record, whose
    // course is empty and therefore rejected).
    void applyGraphDiff(const RiverDiffRecord& diff);

    // --- Conservation ledger (see header comment) ---
    int64_t totalStorage() const { return totalStorage_; }
    int64_t totalInjected() const { return totalInjected_; }
    int64_t totalOutflowToOutlets() const { return totalOutflowToOutlets_; }
    int64_t totalWithdrawnToCoupler() const { return totalWithdrawn_; }
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
    std::vector<uint32_t> outgoingSegmentOfNode_; // nodeId -> PRIMARY segId or kNoSegment

    // Outgoing segments BEYOND the primary, for the bifurcations
    // promoteChannel creates. EMPTY for every graph buildFromFlowAccumulation
    // can produce, which is what keeps step()'s hot loop (and the golden)
    // exactly what it was -- see the header's "Bifurcation" note. Looked up
    // by key only, never iterated, so the unordered_map's own bucket order is
    // not observable; each value vector is in promotion order.
    std::unordered_map<uint32_t, std::vector<uint32_t>> extraOutgoing_;

    uint64_t seed_ = 0; // stored for provenance / future hash-based generation refinements; the D8+accumulation math itself is purely elevation/precip-driven and needs no hashing today

    int64_t totalStorage_ = 0;
    int64_t totalInjected_ = 0;
    int64_t totalOutflowToOutlets_ = 0;
    int64_t totalWithdrawn_ = 0;
};

} // namespace vxc
