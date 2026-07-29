#pragma once
// The river network <-> water CA coupling (plan §3.7 Layer R, W3-proper).
//
// rivernet.h built the COARSE hydrology graph and said so explicitly: it is
// "groundwork only", its kDivertChannel diff was "a documented no-op stub",
// and its own header named the missing piece -- "promoting a channel means
// synthesizing a brand-new node/segment from live CA flux data this
// graph-only layer does not have access to (that coupling is W3-proper,
// after waterca.h and this header are wired together)". THIS FILE IS THAT
// WIRING, and nothing else: it is the only place in voxel-core that includes
// both rivernet.h and waterca.h.
//
// STATUS: ships DISABLED. `RiverCoupleConfig::enabled` defaults to false and
// makes step() a total no-op that reads nothing and writes nothing (asserted,
// not assumed: rivercouple_is_a_total_no_op_when_disabled). No WaterCA tick
// rule is touched and `kWaterCAVersion` is NOT bumped -- both pinned water
// goldens are byte-identical, because everything here goes through the
// existing addWater()/fillAt()/activeBricks() surface and adds exactly zero
// work to WaterCA::step(). `kRiverNetVersion` DID go 2 -> 3, for reasons
// spelled out in rivernet.h; the rivernet golden did not move either.
//
// Engine-free and terrain-free by the same doctrine the rest of the water
// track follows: terrain arrives as a caller-supplied solidity callback and
// elevation as an ITileSampler, so this header includes no worldgen.
//
// =======================================================================
// 1. THE PARTITION: which accountant owns which water
// =======================================================================
// This is the whole correctness argument and it is deliberately much simpler
// than the CA<->SWE partition ADR-0004 had to build, because the two layers
// here are not two discretisations of the same field.
//
//   * The GRAPH holds water IN TRANSIT AT TILE SCALE. A segment's `storage`
//     is a routing state variable for a ~30 m reach. It is not voxels and it
//     is not rendered.
//   * The CA holds VISIBLE WATER AT VOXEL SCALE. It is the authority for
//     everything a player can see, swim in, or drown in.
//
// Water is born in the graph (precipitation/baseflow, via injectInflow),
// routes down the graph, and LEAVES the graph -- permanently -- either into
// the CA at an outfall or into the ocean at the coast. **There is no flow in
// the other direction.** The CA never gives water back to the graph.
//
// That one-directional rule is what makes this coupling structurally unable
// to reproduce the failure ADR-0004 spends a page excluding. The SWE coupler
// has to move volume both ways between two solvers that can both claim the
// same column, so it needs an ownership partition, hysteresis, a
// one-direction-per-tick rule and rate limits just to stop the two sides
// ping-ponging. Here, channel 2 (CA -> graph) moves ZERO UNITS: it is a
// TOPOLOGY edit, informed by the CA and crediting nothing. There is no second
// direction to get wrong, so there is nothing to oscillate.
//
// THE EXACT INVARIANT, after any sequence of ticks:
//
//     net.totalInjected() == net.totalStorage()
//                          + net.totalOutflowToOutlets()
//                          + net.totalWithdrawnToCoupler()
//
//     net.totalWithdrawnToCoupler() == graphUnitsToCA() + graphUnitsToOcean()
//
//     graphUnitsToCA() == fillDeliveredToCA() * config().graphUnitsPerFill
//
// and every unit counted in fillDeliveredToCA() is present in
// ca.totalVolume() (or was, until the CA's own rules moved or destroyed it --
// e.g. a player filling a wet voxel with rock, which destroys water in the
// CA today exactly as it always has). The three identities are asserted every
// tick by rivercouple_conservation_exact_across_the_boundary.
//
// UNITS. Graph storage is in the accumulation units rivernet.h routes (see
// its "buildFromFlowAccumulation" step 5); CA fill is 0..255 per voxel. The
// bridge is ONE integer, `graphUnitsPerFill`, and the coupler only ever
// withdraws WHOLE MULTIPLES of it -- so there is no remainder to lose, no
// fractional carry to persist, and no place for a unit to go missing in the
// conversion. Sub-multiple residue simply stays in the segment as storage,
// which is correct: it is water still in the reach.
//
// REFUSAL. `addWater` may place less than asked (a full cell, a solid cell).
// The unplaced remainder is REFUNDED to the segment it came from via
// refundFromCoupler, in the same call. That is exact, and it is also the
// right physics: a blocked outfall back-pressures its reach, storage rises,
// and that is the same mechanism rivernet.h already uses for "upstream stage
// rises" behind a dam. A coupler that could not refund would have to either
// destroy the units or invent a private holding tank whose contents nobody
// audits -- which is precisely the class of bug ADR-0004 was written about.
//
// =======================================================================
// 2. CHANNEL 1 (graph -> CA): discharge becomes water, by TOP-UP
// =======================================================================
// For each segment, once per coupler tick, the coupler looks at ONE column --
// the reach's downstream node -- and asks: how wet should the channel be
// here, given what this reach is carrying, and how wet is it actually?
//
//     target    = clamp(minTargetFill + discharge / dischargePerFillUnit,
//                       minTargetFill, maxTargetFill)
//     present   = sum of CA fill over the outfall window at that column
//     shortfall = max(0, target - present)
//
// and it hands over `min(shortfall, maxFillPerSegmentPerTick, storage/rate)`
// fill units. TOP-UP, NOT POUR, and that distinction is the entire design:
//
//   * A pour would drain the graph at a rate unrelated to anything, and the
//     graph would be dry in seconds no matter what the routing said.
//   * A top-up self-limits into a genuine FLUX MATCH. If the CA is carrying
//     water away from the outfall (a real flowing channel), the shortfall is
//     real every tick and the graph feeds it continuously. If the water is
//     going nowhere (a blocked or already-full channel), the shortfall is
//     zero, the coupler withdraws nothing, and the reach's storage rises --
//     which is exactly the reservoir behaviour Layer R already produces
//     behind a dam, now reachable from the CA side too.
//
// This is the same shape as the engine's existing reservoir top-up
// (UVoxelWaterSubsystem::StepFixed, which tops each registered reservoir cell
// back to 255 every fixed step), deliberately: there is one established
// injection idiom in this codebase and a new source should participate in it
// rather than invent a second.
//
// `discharge` is read, not `storage`, because discharge is the reach's FLOW
// and storage is its contents; rivernet.h documents the field as holding the
// build-time catchment estimate before the first step() and the routed
// outflow after it, and both readings are legitimately "how big is this
// river". The map from discharge to a visible depth is a PRESENTATION
// CALIBRATION (`dischargePerFillUnit`), not physics, and is named as one.
//
// WHERE the water lands: the outfall column is scanned DOWNWARD from a little
// above the node's tile elevation for the first solid voxel, and the water is
// placed on top of it. The scan matters because W3's sibling work carves an
// actual riverbed BELOW the tile surface; placing water at the tile surface
// would leave it sitting on the bank instead of in the channel. If the window
// contains no solid at all the water is placed at the window's floor and the
// CA's own gravity carries it down, which is the correct answer for a node
// hanging over a void.
//
// =======================================================================
// 3. THE OCEAN IS THE SINK
// =======================================================================
// Sea level is voxel z == 0 (core.h; AVoxelOceanActor is an implicit plane
// with ZERO voxel data behind it). So a river must not pool at the coast, and
// it must not be given CA fill there either -- fill written at or below z==0
// is water inside the ocean plane that nothing will ever drain, and it would
// grow without bound for as long as the river runs.
//
// Rule, in two halves:
//   * A segment whose downstream node is AT OR BELOW sea level is an OCEAN
//     OUTFALL. It receives no injection at all, and its entire storage is
//     withdrawn every tick into graphUnitsToOcean(). The sea is an infinite
//     sink with no storage of its own, so there is no cap and no back
//     pressure; what limits the rate is upstream routing, which is where the
//     rate belongs.
//   * A segment whose node is above sea level but whose resolved outfall
//     FLOOR is at or below it (a node on a cliff over the water) is treated
//     the same way, because the floor is where the fill would actually go.
//
// Either way the units are ledgered, not deleted: graphUnitsToOcean() is a
// term of the invariant in section 1, so "it went to the sea" is a checkable
// claim rather than a shrug.
//
// =======================================================================
// 4. CHANNEL 2 (CA -> graph): what promotes a channel, and why
// =======================================================================
// The plan says "Divert -> sustained CA flux promotes new channel to a
// segment". The brief for this work adds the design constraint that matters
// most: **a promotion criterion that fires on a puddle is worse than one that
// never fires.** A false promotion permanently corrupts routing (there is no
// un-promote diff) and permanently steals half the stem's discharge; a missed
// one costs nothing but a player having to dig a slightly better ditch.
//
// So promotion requires FIVE independent conjuncts, ALL of which must hold on
// every one of `sustainTicks` CONSECUTIVE coupler ticks. Any single failure
// drops the candidate outright (the dwell counter is not decremented, it is
// discarded), so the criterion cannot be satisfied by intermittent luck.
//
//   (1) IT LEAVES A RIVER. The course's take-off is an EXISTING river node,
//       and that node's own pixel is wet. This is the strongest single
//       anti-puddle guard and it is nearly free: rain pools, dug ponds,
//       bucket spills and flooded basements are not adjacent to a river node.
//   (2) IT REACHES A SINK. The course terminates at another existing river
//       node, or at or below sea level. A ditch that dead-ends in a pit is a
//       RESERVOIR, not a channel, and promoting it would tell the graph to
//       route water into a hole -- so it is rejected by construction rather
//       than by tuning.
//   (3) IT DESCENDS, STRICTLY, EVERY STEP. Segments are directed downhill by
//       rivernet.h's contract; a flat or uphill step would let step() route
//       water in a cycle. (This also rules out promoting the flat surface of
//       a large pool, which is the shape a puddle detector fires on.)
//   (4) IT IS A REACH, NOT A CELL. At least `minChannelPixels` tile pixels
//       long. At a 30 m pixel that is 60 m of continuous channel at the
//       default of 2.
//   (5) IT IS WET ALONG ITS WHOLE LENGTH, AND STILL MOVING. Every pixel of
//       the course carries at least `minWetFillPerPixel` of CA fill within
//       the sampled window, AND at least one sampled column of the course
//       lies in a brick that was ACTIVE on the CA's most recent step. That
//       last clause is the "flux" half of "sustained CA flux": WaterCA's
//       active set is exactly the set of bricks that CHANGED last tick
//       (waterca.h, "Activity / settling"), so a settled body drops out of it
//       within a few ticks and stops qualifying, while water genuinely
//       running downhill never does. It costs one std::set lookup.
//
// WHY THAT IS "SUSTAINED FLUX" AND NOT A PROXY FOR IT. The honest statement
// is that the CA does not expose a per-cell flux and this coupler REFUSES TO
// MAKE IT: adding flux accounting to the tick would touch the read/apply
// rules, move both water goldens, and undo part of the 4.7x the tick was just
// optimised by. What is measured instead is the observable signature of
// throughflow -- a long, strictly-descending, continuously-wet chain from a
// river to a sink, still changing, for `sustainTicks` in a row. A puddle
// fails (1), (2) and (3) independently; a rainstorm sheet fails (1) and dries
// out of (5); a player splashing in the shallows fails (2), (3) and (4).
//
// COST, AND WHY IT IS BUDGETED RATHER THAN SCALED. Nothing here touches the
// CA's tick path -- not one line runs inside WaterCA::step(), which is what
// keeps the 4.7x that tick was just optimised by intact and keeps both water
// goldens byte-identical. The pass's own cost is bounded per tick by
// (`maxTrackedCandidates` * `maxChannelPixels`) re-verification samples plus
// (`scansPerTick` * 8 * `maxChannelPixels`) discovery samples in the worst
// case -- at the defaults about 500 PIXEL samples, each of which is
// `sampleGrid`^2 * `sampleColumnVoxels` == 256 hashed fillAt() probes, so
// order 10^5 probes per pass at a ~1 Hz cadence. The budget is a hard cap and
// not a function of graph size on purpose: a continent-sized network costs
// exactly what a valley does per tick, it just takes more ticks to sweep.
// (That bound is a count, not a timing -- no wall-clock number is claimed for
// this pass because none has been measured.)
//
// WHAT PROMOTION DOES. It calls RiverNetwork::promoteChannel (which appends
// nodes/segments and gives the take-off node a SECOND outgoing segment -- see
// rivernet.h's "Bifurcation" note for why the resulting outflow split is
// even rather than conveyance-weighted) and emits a kDivertChannel
// RiverDiffRecord carrying the COURSE into pendingDiffs(). The diff is what
// makes the promotion persistent and replicable: a replay must not have to
// re-observe CA state that no longer exists, so the decision travels, not the
// evidence.
//
// =======================================================================
// 5. DETERMINISM
// =======================================================================
// Every choice in this file is a fixed rule over integers and coordinates:
// the compass order for candidate discovery is rivernet.cpp's own
// (N,NE,E,SE,S,SW,W,NW); the round-robin cursors advance by a fixed stride;
// candidates are held in a std::vector in discovery order; the node-by-pixel
// index is a std::map (ordered, not hashed). Nothing reads a clock, nothing
// iterates an unordered container, and nothing depends on the order in which
// WaterCA happens to store bricks -- only on `fillAt` values and on
// `activeBricks()` membership, both of which are already order-independent
// properties of the CA by its own contract.

#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "voxelcore/core.h"
#include "voxelcore/rivernet.h"
#include "voxelcore/tiles.h"
#include "voxelcore/waterca.h"

namespace vxc {

struct RiverCoupleConfig {
    // MASTER FLAG. Defaults to FALSE and makes step() a total no-op, exactly
    // like SweCoupleConfig::enabled: a new simulation layer does not get to
    // start changing a live world because it was linked in.
    bool enabled = false;

    // --- section 2, channel 1 (graph -> CA) -------------------------------

    // Graph storage units per one unit of CA fill. The coupler only withdraws
    // whole multiples of this, so the conversion is exact in both directions
    // and there is no fractional carry anywhere in the system.
    int32_t graphUnitsPerFill = 64;

    // Presentation calibration, NOT physics: how much visible depth a given
    // discharge should hold at an outfall. minTargetFill is one full voxel;
    // maxTargetFill is eight, i.e. 0.8 m of standing water at the node.
    int32_t dischargePerFillUnit = 1024;
    int32_t minTargetFill = 255;
    int32_t maxTargetFill = 2040;

    // Per-segment per-tick hand-over cap. Bounds how fast a single reach can
    // dump into the CA no matter how much storage it has accumulated (a burst
    // dam should surge, not teleport a lake).
    int32_t maxFillPerSegmentPerTick = 1024;

    // Outfall column geometry, in voxels. `outfallScanVoxels` is how far BELOW
    // the node's tile elevation the coupler looks for the channel floor (W3's
    // sibling carving work cuts the bed below the tile surface, so this must
    // be deeper than the deepest carve); `outfallAboveVoxels` is how far above
    // it starts; `outfallDepthVoxels` is the window `present` is summed over.
    int32_t outfallScanVoxels = 24;
    int32_t outfallAboveVoxels = 2;
    int32_t outfallDepthVoxels = 10;

    // Segments serviced per tick (round-robin). Bounds the cost on a large
    // network; a network bigger than this is swept over several ticks.
    int32_t maxOutfallsPerTick = 256;

    // Sea level, in world voxel z (core.h: z == 0). Section 3.
    int32_t seaLevelVz = 0;

    // --- section 4, channel 2 (CA -> graph promotion) ---------------------

    bool promotionEnabled = true;

    // Conjunct (5)'s "sustained": consecutive coupler ticks all five
    // conjuncts must hold. At the intended ~1 Hz cadence this is 30 seconds
    // of continuously-flowing water. Chosen to be far longer than any
    // transient a single dig, splash or wave can produce, and short enough
    // that a player who cuts a real ditch sees it become a river while still
    // standing there.
    int32_t sustainTicks = 30;

    // Conjunct (5)'s "wet": minimum summed CA fill within one pixel's sample
    // box (sampleGrid^2 columns x sampleColumnVoxels voxels, the window
    // starting sampleAboveVoxels above the pixel's tile elevation and running
    // down from there). At the defaults the box holds 4*4*16 = 256 cells (max
    // 65280 fill), so 3000 is about twelve completely full voxels' worth
    // spread through it -- water in a channel, not a film and not one wet
    // cell. The window has to reach WELL BELOW the tile surface because W3's
    // sibling carving work cuts the bed down there; 16 voxels is 1.6 m.
    int32_t minWetFillPerPixel = 3000;
    int32_t sampleGrid = 4;
    int32_t sampleColumnVoxels = 16;
    int32_t sampleAboveVoxels = 2;

    // Conjuncts (4) and the search bound.
    int32_t minChannelPixels = 2;
    int32_t maxChannelPixels = 12;

    // Per-tick budget (section 4, COST).
    int32_t maxTrackedCandidates = 8;
    int32_t scansPerTick = 4;
};

// One CA write channel 1 made this tick. The engine's re-mesh feed: water that
// appears at an outfall has to become geometry in the same frame or the river
// is invisible until something else happens to dirty the brick. Mirrors
// WaterMobilizer::takeRecentlyMobilized()'s role, and carries the amount so a
// caller can size the column span it dirties exactly as the reservoir top-up
// already does.
struct RiverOutfallWrite {
    int64_t vx = 0, vy = 0, vz = 0;
    uint32_t placed = 0;
};

// The river-graph <-> water-CA boundary. Holds references to both, owns only
// its candidate bookkeeping and its ledgers, and never mutates terrain.
class RiverCaCoupler {
public:
    using SolidFn = std::function<MaterialId(int64_t vx, int64_t vy, int64_t vz)>;

    RiverCaCoupler(RiverNetwork& net, WaterCA& ca, ITileSampler& tiles, SolidFn solid,
                   const RiverCoupleConfig& cfg = RiverCoupleConfig{});

    const RiverCoupleConfig& config() const { return cfg_; }
    void setEnabled(bool on) { cfg_.enabled = on; }

    // One coupling tick, intended to be called immediately after each
    // RiverNetwork::step() (i.e. at the network's own ~1 Hz cadence, not the
    // CA's). Channel 1 then channel 2, in that order: a promotion decided this
    // tick starts receiving discharge on the NEXT one, which keeps a
    // promotion from being a same-tick discontinuity in the ledger.
    //
    // A total no-op when config().enabled is false -- it reads nothing and
    // writes nothing, so a caller who has not opted in pays one branch.
    void step();

    // --- ledgers (section 1) ----------------------------------------------
    int64_t graphUnitsToCA() const { return toCa_; }
    int64_t graphUnitsToOcean() const { return toOcean_; }
    int64_t graphUnitsRefunded() const { return refunded_; }
    int64_t fillDeliveredToCA() const { return fillDelivered_; }

    // Where channel 1 wrote CA fill on the most recent step(), for the caller's
    // re-mesh/replication bookkeeping. Cleared at the top of every step(), so
    // it describes exactly one tick and never accumulates.
    const std::vector<RiverOutfallWrite>& lastOutfallWrites() const { return outfallWrites_; }

    // --- diagnostics ------------------------------------------------------
    int32_t trackedCandidateCount() const { return static_cast<int32_t>(candidates_.size()); }
    int64_t promotionCount() const { return promotions_; }
    // Dwell of the closest candidate to promotion (0 if none) -- the number a
    // fixture prints to show the criterion converging rather than stuck.
    int32_t bestCandidateDwell() const;

    // Graph diffs this coupler has produced (kDivertChannel records) since the
    // last call, and clears the queue. The persistence/replication feed:
    // rivernet.h's graph-diff log is what makes a promotion survive a reload,
    // and applyGraphDiff replays these verbatim.
    std::vector<RiverDiffRecord> takePendingDiffs();

private:
    // One tracked promotion candidate: a course found by discovery, re-verified
    // from scratch every tick until it either promotes or fails once.
    struct Candidate {
        uint32_t headNode = RiverNetwork::kNoNode;
        std::vector<RiverChannelPoint> course;
        int32_t dwell = 0;
    };

    // Tile-pixel <-> world-voxel conversions (rivernet.cpp's own convention:
    // a node's vx is floorDiv(px * pixelSizeMm, kVoxelSizeMm), which inverts
    // exactly for the pixel sizes the tile protocol uses).
    int64_t pixelOfVoxel(int64_t v) const;
    int64_t voxelOfPixel(int64_t p) const;

    void refreshNodeIndex();
    uint32_t nodeAtPixel(int64_t px, int64_t py) const;

    // Channel 1.
    void injectPass();
    void serviceSegment(uint32_t segId);

    // Channel 2.
    void promotionPass();
    bool verifyCandidate(Candidate& c) const;
    bool discoverFrom(uint32_t nodeId, Candidate& out) const;
    // Summed CA fill in one pixel's sample box; also reports whether any of
    // the sampled columns sits in a brick the CA had active last step.
    int64_t sampleWetFill(int64_t px, int64_t py, int32_t elevationMm, bool& outActive) const;
    bool pixelIsWet(int64_t px, int64_t py, int32_t elevationMm, bool& outActive) const;

    bool solidAt(int64_t vx, int64_t vy, int64_t vz) const {
        return solid_(vx, vy, vz) != MAT_AIR;
    }

    RiverNetwork& net_;
    WaterCA& ca_;
    ITileSampler& tiles_;
    SolidFn solid_;
    RiverCoupleConfig cfg_;

    int64_t pixelSizeMm_ = 1;

    // Ordered (never hashed) so the index itself carries no iteration-order
    // dependence; rebuilt only when the node count changes, which is only at
    // build time and on promotion.
    std::map<std::pair<int64_t, int64_t>, uint32_t> nodeOfPixel_;
    size_t nodeIndexSize_ = static_cast<size_t>(-1);

    std::vector<Candidate> candidates_;
    std::vector<RiverDiffRecord> pendingDiffs_;
    std::vector<RiverOutfallWrite> outfallWrites_;

    uint64_t injectCursor_ = 0;
    uint64_t scanCursor_ = 0;

    int64_t toCa_ = 0, toOcean_ = 0, refunded_ = 0, fillDelivered_ = 0;
    int64_t promotions_ = 0;
};

} // namespace vxc
