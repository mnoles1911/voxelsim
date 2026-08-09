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

// -----------------------------------------------------------------------
// buildFromBakedWater (v1 generation, water re-architecture Phase 2)
// -----------------------------------------------------------------------
// buildFromFlowAccumulation above runs its OWN D8 over the elevation lattice,
// which means the graph's geometry is the client's opinion about where rivers
// are, while the rivers the PLAYER SEES are the bake's water plane. Those two
// disagree -- the bake's channels come from a full hydrology pass with carried
// discharge, monotone settling and bank carving, none of which a 16-line D8
// reproduces -- and once the graph is the AUTHORITY for how much water a reach
// carries (plan §4/§5), a graph that runs down a different valley than the
// drawn river is not an approximation, it is a different river.
//
// So this second builder constructs the same graph from the SHIPPED PLANES:
// the water plane says where water is and how high it stands, and the flow
// plane's channel bit + log2 accumulation say which of those cells is a
// channel and how much catchment is behind it. Both are already on the wire
// and already what the renderer draws.
//
//   1. A pixel QUALIFIES as a node iff its flow byte has the CHANNEL bit and
//      its log2 accumulation is >= minAccumLog2 (and, when requireWet, the
//      water plane calls it wet -- see BakedWaterBuildParams).
//   2. DOWNSTREAM IS THE WATER SURFACE, not the ground. Each qualifying pixel
//      routes to the LOWEST of its qualifying 8-neighbours under one strict
//      total order: water surface ascending, then accumulation DESCENDING,
//      then pixel index ascending. So the lowest surface wins, ties go to the
//      bigger river, and the remainder is broken canonically. Ordering on the
//      water
//      surface is both the physically right answer (water flows down its own
//      surface, not down the bed) and what makes the result ACYCLIC for free:
//      every edge strictly decreases a total order, so no cycle can close.
//      Flat reaches -- where the bake's surface is exactly equal across
//      several cells -- fall through to accumulation and then to pixel index,
//      which is deterministic but arbitrary. Documented rather than fixed:
//      exact routing over a flat pool is a basin question, not a channel one,
//      and the basin table already answers it.
//   3. HEADWATERS are the qualifying nodes with NO incoming segment, exposed
//      as headwaterNodes()/headwaterSegments() -- the emitters Phase 3 hangs
//      its faucets on. THE APPROXIMATION, stated plainly: a reach that enters
//      the region from outside its bounds has no in-edge inside the region and
//      is therefore reported as a head, so the region's upstream rim is full of
//      false heads. A caller placing faucets must either build over a region
//      wider than the one it emits in, or wait for the bake's own
//      `water_head_mask` (computed at water.py:480-525 and currently DISCARDED
//      at pipeline.py:5300), which Phase 1 ships and which is the exact answer.
//   4. `discharge` at build time means UPSTREAM CATCHMENT AREA IN m^2 here,
//      decoded from the flow byte's log2 bucket -- a THIRD meaning for that
//      field, and the reason the two builders are separate entry points rather
//      than one with a flag. buildFromFlowAccumulation's build-time discharge
//      is mm/yr of accumulated rainfall; after the first step() both mean the
//      routed outflow. A caller converting build-time discharge into a
//      baseflow rate must know which builder produced the graph.
//
// The old builder is KEPT and unchanged. Nothing that ships today moves.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "voxelcore/bytes.h"
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
//
// v4 (water re-architecture Phase 2): buildFromBakedWater adds a SECOND
// generation path, and RiverNetState persists a graph's diff log and routing
// state. No existing generation rule and no routing rule changed -- the pinned
// golden (tests/test_rivernet.cpp, 0xEC84E0B592821C38) is BYTE-IDENTICAL to v3
// and the D8 builder's arithmetic is untouched. The bump is for the same reason
// v3's was: a persisted graph-diff log now exists, and a log recorded against a
// build that had no buildFromBakedWater and no RiverNetState is not
// reproducible against one that does. Per docs/determinism.md that divergence
// is exactly what the constant signals.
inline constexpr uint32_t kRiverNetVersion = 4;

// ---------------------------------------------------------------------------
// THE FLOW BYTE (docs/vxtl-v2-format.md §6; terrain_service/tile_codec.py:378-383)
// ---------------------------------------------------------------------------
//
// bits 0-4: log2 of the upstream flow accumulation in m^2, clamped 0..31.
// bit 5: channel. bit 6: bank. bit 7: deposition.
//
// THE THIRD COPY OF THIS, and it is a copy on purpose rather than an accident:
// tile_codec.py owns the encoder's names, docs/vxtl-v2-format.md owns the
// prose, and until now the C++ side decoded the plane to raw bytes and named
// none of its bits -- FVoxelFineTileStreamer.cpp:223 says outright that nothing
// in ue-project reads it. These live HERE rather than in tilestore.h only
// because tilestore.h is being changed by the basin-table-v2 work concurrently;
// they belong beside the decoder and should move there when that lands.
inline constexpr uint8_t kFlowLog2Mask = 0x1F;
inline constexpr uint8_t kFlowBitChannel = 1u << 5;
inline constexpr uint8_t kFlowBitBank = 1u << 6;
inline constexpr uint8_t kFlowBitDeposition = 1u << 7;

constexpr uint8_t flowAccumLog2(uint8_t flowByte) { return uint8_t(flowByte & kFlowLog2Mask); }
constexpr bool flowIsChannel(uint8_t flowByte) { return (flowByte & kFlowBitChannel) != 0; }
// The log2 bucket back to an area in m^2, saturating rather than wrapping: the
// top bucket is 2^31 m^2 = 2,147 km^2 of catchment, which int32 cannot hold as
// a discharge, and a wrapped negative discharge would make a trunk reach look
// like a headwater.
constexpr int32_t flowAccumM2(uint8_t flowByte) {
    const int32_t log2 = int32_t(flowAccumLog2(flowByte));
    return log2 >= 31 ? INT32_MAX : int32_t(int64_t(1) << log2);
}

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

// ---------------------------------------------------------------------------
// THE BAKED WATER + FLOW PLANES, as buildFromBakedWater needs them
// ---------------------------------------------------------------------------
//
// AN INTERFACE RATHER THAN A FineTileSampler& and that is deliberate twice
// over. First, it keeps this header's documented dependency set intact --
// rivernet.h depends on tiles.h and nothing else, exactly as waterca.h is
// terrain-free, and reaching for tilestore.h here would put the whole fine-tile
// decoder behind every include of the routing graph. Second, it is what makes
// the builder testable at all: a synthetic source is twenty lines, where a
// fixture tile that carries a plausible river is a bake.
//
// The concrete adapter over the shipped fine tier is
// `vxc::FineTileBakedWaterSource` in voxelcore/basinledger.h, which is where
// the other baked-hydrology adapters live.
class IBakedWaterSource {
public:
    virtual ~IBakedWaterSource() = default;

    // Fine-tile pixel pitch in mm (1875 on the shipped tier).
    virtual int32_t pixelSizeMm() const = 0;

    // The water plane at one FINE pixel. `outSurfaceMm` is the absolute water
    // surface and `outWet` distinguishes a genuinely wet cell from a dry cell
    // that merely carries a level band (tilestore.h `waterCpIsWet`) -- the
    // distinction the level band exists for, and one a channel test must
    // respect or the graph grows a node in every dry collar cell.
    //
    // False means UNRESOLVABLE (tile absent, block not fetched, plane missing),
    // which is not "dry": the builder skips the pixel and counts it, so a graph
    // built over a half-streamed region is short a reach and says so, rather
    // than silently routing a river into a hole.
    virtual bool waterAt(int64_t px, int64_t py, int32_t& outSurfaceMm, bool& outWet) = 0;

    // The flow byte at one FINE pixel (channel bit + log2 accumulation; see
    // kFlowBitChannel above). Same false-means-unresolvable contract.
    virtual bool flowAt(int64_t px, int64_t py, uint8_t& outFlow) = 0;
};

// Knobs for buildFromBakedWater. Every default is the conservative one: take
// the bake at its word, require both the channel bit and standing water, and
// let the caller loosen it.
struct BakedWaterBuildParams {
    // Inclusive FINE-pixel rectangle to build over.
    RegionBounds bounds{};

    // Minimum log2(catchment m^2) for a channel cell to become a node. 0 admits
    // every channel cell the bake marked. This is the direct analogue of
    // buildFromFlowAccumulation's accumThreshold and it is in a DIFFERENT UNIT
    // (a log2 bucket of area, not mm/yr of rain), which is the same currency
    // warning the header comment gives for `discharge`.
    uint8_t minAccumLog2 = 0;

    // Require the water plane to call the cell WET, not merely banded. On is
    // correct for "the rivers the player sees"; off lets the graph follow a
    // baked channel through a reach the bake left dry (a seasonal wash), which
    // is a legitimate thing to want and a wrong default.
    bool requireWet = true;

    // When the water plane cannot resolve a cell but the flow plane can, admit
    // it using the elevation-free fallback surface of INT32_MIN+1 so ordering
    // still works. OFF by default: a river routed through unresolved cells is
    // exactly the "silently routing into a hole" case waterAt's contract warns
    // about. Left as a knob only because a caller doing a coarse survey over a
    // partially streamed region may genuinely prefer a rough graph to none.
    bool admitUnresolvedWater = false;
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

    // v1 generation from the SHIPPED water + flow planes (see the header
    // comment "buildFromBakedWater"). Clears any previously built graph,
    // ledgers, diff log and headwater set, exactly as the D8 builder does.
    // Returns the number of segments built; 0 means the region carries no
    // baked channel that met `minAccumLog2` (check bakedCellsUnresolved()
    // before concluding it is dry).
    uint32_t buildFromBakedWater(IBakedWaterSource& source, uint64_t seed,
                                 const BakedWaterBuildParams& params);

    // Nodes with NO incoming segment -- the headwaters, in ascending node id.
    // Empty for a graph built by buildFromFlowAccumulation (which does not
    // compute them) even though that graph has heads; ask buildFromBakedWater
    // for them, or derive them the way test_rivernet.cpp's determinism test
    // does. See the header comment for what makes these APPROXIMATE.
    const std::vector<uint32_t>& headwaterNodes() const { return headwaterNodes_; }
    // The outgoing segment of each headwater node, i.e. exactly the segment ids
    // an emitter should injectInflow into. Same order as headwaterNodes(); a
    // head with no outgoing segment (an isolated one-cell channel) is omitted,
    // so this can be shorter.
    std::vector<uint32_t> headwaterSegments() const;

    // Cells buildFromBakedWater asked about and could not resolve (see
    // IBakedWaterSource). NON-ZERO MEANS THE GRAPH IS SHORT REACHES, which
    // looks exactly like a dry valley -- the ran-flag half of the pair with
    // bakedCellsScanned().
    uint64_t bakedCellsUnresolved() const { return bakedUnresolved_; }
    uint64_t bakedCellsScanned() const { return bakedScanned_; }
    uint64_t bakedChannelCells() const { return bakedChannelCells_; }

    const std::vector<RiverNode>& nodes() const { return nodes_; }
    const std::vector<RiverSegment>& segments() const { return segments_; }
    uint32_t segmentCount() const { return static_cast<uint32_t>(segments_.size()); }

    // The segment whose FROM node is nearest (Chebyshev-with-diagonal-weighting
    // in world mm, the same integer metric channelStepLengthMm uses) to a world
    // VOXEL position, or kNoSegment when the graph is empty or nothing lies
    // within `maxDistMm`. O(segments); intended for the once-per-spill lookup
    // the basin spillway does, not for a per-tick sweep.
    //
    // THE SPILLWAY'S ENTRY POINT. `BasinEntry::outletX/Y` names the saddle a
    // lake overflows at; this turns that into the reach the overflow joins.
    // Refusing beyond maxDistMm is what keeps a spill from teleporting into an
    // unrelated valley when the graph does not happen to cover the outlet --
    // the caller then refunds the units to the basin (see
    // basinledger.h `routeSpills`) and the lake back-pressures instead.
    uint32_t nearestSegmentToVoxel(int64_t vx, int64_t vy, int64_t maxDistMm) const;

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

    // --- the diff log, as a RECORDING (persistence, Phase 2) ----------------
    //
    // The header's "Hydrology graph-diff" section already establishes that a
    // log of RiverDiffRecords replayed against a freshly built graph reproduces
    // the same topology and conveyance. What did not exist is anything that
    // KEEPS the log, so a session's dams and diversions died with the process.
    //
    // Recording is OFF by default and, when off, costs one bool test in
    // setConveyance and one in promoteChannel -- so an unarmed graph runs the
    // arithmetic it always ran and the pinned golden cannot move.
    //
    // applyGraphDiff dispatches THROUGH setConveyance/promoteChannel, so
    // replaying a log into a recording graph rebuilds an identical log. That is
    // the property that makes save -> load -> save byte-identical, and
    // test_rivernet's round-trip asserts it rather than assuming it.
    //
    // ONLY SUCCESSFUL CHANGES ARE RECORDED: promoteChannel rejects
    // conservatively and a rejected promotion mutates nothing, so logging it
    // would put a record in the log that replay would reject again -- harmless
    // but a lie about what happened.
    void setDiffRecording(bool on) { recordDiffs_ = on; }
    bool diffRecording() const { return recordDiffs_; }
    const std::vector<RiverDiffRecord>& diffLog() const { return diffLog_; }
    void clearDiffLog() { diffLog_.clear(); }

    // --- routing state, for persistence -------------------------------------
    //
    // The diff log carries the DECISIONS; this carries the WATER. They are
    // different facts and both have to survive a reload: replaying the log
    // rebuilds the dams, but not the reservoir that had piled up behind one.
    struct RoutingSnapshot {
        struct Seg {
            int32_t discharge = 0;
            int32_t storage = 0;
            uint8_t conveyance = 255;

            friend bool operator==(const Seg&, const Seg&) = default;
        };
        std::vector<Seg> segs;
        int64_t totalStorage = 0;
        int64_t totalInjected = 0;
        int64_t totalOutflowToOutlets = 0;
        int64_t totalWithdrawn = 0;

        friend bool operator==(const RoutingSnapshot&, const RoutingSnapshot&) = default;
    };

    RoutingSnapshot captureRoutingState() const;

    // Overwrites every segment's storage/discharge/conveyance and all four
    // ledger totals. ALL-OR-NOTHING: returns false and mutates nothing if the
    // snapshot's segment count differs from this graph's (a blob for a
    // different region, or one whose diff log did not replay), if the totals
    // fail the header's exact-integer identity
    //     totalStorage + totalOutflowToOutlets + totalWithdrawn == totalInjected,
    // or if the per-segment storages do not sum to totalStorage. A partially
    // applied snapshot is a conservation ledger that lies for the rest of the
    // session, and the whole point of the ledger is that it does not.
    bool restoreRoutingState(const RoutingSnapshot& snap);

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

    // buildFromBakedWater's outputs and its ran-flags.
    std::vector<uint32_t> headwaterNodes_;
    uint64_t bakedScanned_ = 0;
    uint64_t bakedUnresolved_ = 0;
    uint64_t bakedChannelCells_ = 0;

    bool recordDiffs_ = false;
    std::vector<RiverDiffRecord> diffLog_;
};

// ---------------------------------------------------------------------------
// PERSISTENCE (water re-architecture Phase 2)
// ---------------------------------------------------------------------------
//
// WHAT IS SAVED AND WHY IT IS TWO THINGS. A river graph after a session is
// (base topology from the region) + (the decisions a player made) + (the water
// currently in the reaches). The base is rebuilt by running the same builder
// over the same region, so it is not in the blob. The other two are, and they
// are separate sections because they fail differently: a diff log that will not
// replay is a corrupt save, while routing state that does not match is merely a
// graph built over different bounds -- and the second must not be able to make
// the first unreadable.
//
// Little-endian, integer only, bytes.h primitives -- the same shape
// WaterState and EditLog use.
//
//   u32 magic, u32 kRiverNetVersion
//   u32 diffCount, then per diff:
//       u8 kind, u32 segId, u8 value, u32 headNode,
//       u32 courseLen, then per point: i64 vx, i64 vy, i32 elevationMm,
//                                      u32 existingNode
//   u32 segCount, then per segment: i32 discharge, i32 storage, u8 conveyance
//   i64 totalStorage, i64 totalInjected, i64 totalOutflowToOutlets,
//   i64 totalWithdrawn
//
// VERSIONED ON kRiverNetVersion, not on a private counter, and that is the
// whole compatibility story: the routing math IS the meaning of `storage`, so a
// blob written by different math must be refused rather than reinterpreted --
// the identical argument WaterState makes for kWaterCAVersion.
struct RiverNetState {
    static constexpr uint32_t kMagic = 0x54534E52; // "RNST" little-endian

    // Appends the graph's diff log and routing state to `out`; does not clear.
    static void serialize(const RiverNetwork& net, std::vector<uint8_t>& out);

    // Decodes, validates, then applies to an ALREADY BUILT `net` -- diffs first
    // (which may add segments, so the routing section's count is checked
    // against the POST-replay graph), then the routing snapshot.
    //
    // False on bad magic, a version mismatch, truncation, trailing bytes, an
    // implausible count, a diff that fails to replay into a topology matching
    // the snapshot's segment count, or a snapshot that fails
    // restoreRoutingState's ledger identity. On a false the graph may have had
    // diffs applied -- which is why the caller's correct response is to discard
    // the graph and rebuild it, exactly as LoadWaterState discards and reverts
    // to the implicit field.
    static bool load(const uint8_t* data, size_t size, RiverNetwork& net);
};

} // namespace vxc
