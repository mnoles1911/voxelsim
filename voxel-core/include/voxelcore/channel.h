#pragma once
// River channel geometry (plan §3.1 "riverbed carving along flow lines",
// §3.7 "Generation" / Layer R, W3 visible half): turns the rivernet.h
// segment graph into a per-voxel RIVERBED -- a graded, continuous,
// water-tight trapezoidal channel whose width and depth come from upstream
// discharge, and which descends monotonically to the sea.
//
// This header is the TERRAIN half of W3. It does not create, move or
// conserve water; it produces the bed that waterca.h / the rivernet<->CA
// coupling fills. Read-only w.r.t. rivernet.h.
//
// =========================================================================
// WHY THIS IS A SEPARATE MODULE AND NOT A TERM IN evalSurface
// =========================================================================
// A channel's existence at (x,y) depends on square kilometres of upstream
// terrain (rivernet.h's D8 flow accumulation). It is therefore NOT a point
// function, and the amplifier's contract -- and the HLSL mirror in
// voxel-core/shaders/worldgen.ush -- is built entirely out of point
// functions of (seed, x, y) plus a bounded tile stencil. Putting a carve
// inside evalSurface would mean shipping the whole segment graph to the
// GPU as a new per-region buffer.
//
// So the split is: this module does the NON-LOCAL reasoning ONCE per
// region, at build(), and bakes the answer into a bounded spatial index.
// After that, sampleAt() is an O(1) point query over a handful of
// candidate segments -- the same shape as a tile fetch, and mirrorable as
// such when the wiring bump is taken. Until then nothing here changes
// worldgen's output, so kWorldGenVersion is UNTOUCHED, no golden moves and
// worldgen.ush needs no mirror. This follows the precedent set by
// carrier.h / detail_rill.h / detail_bedding.h, which all landed as
// standalone, tested, bounded functions before being wired in.
// docs/terrain-amplification-reconciliation.md "Collision B" is the
// argument for why the non-local part cannot be dissolved into a point
// function; this module is the answer to it, not a violation of it.
//
// =========================================================================
// GEOMETRY
// =========================================================================
// Hydraulic geometry, downstream form (Leopold & Maddock): channel width
// and depth are power laws in discharge. The server bake uses width ∝ A^0.4
// (docs/terrain-amplification-plan.md B2) and the design doc gives depth
// ∝ A^0.3..0.4 (docs/research/terrain-amplification-design-doc.md §6.2);
// this module uses the same 0.40 for width and the midpoint 0.35 for depth
// so that a future wiring pass agrees with the bake's own law rather than
// fighting it. See kChannelWidthExpQ8 / kChannelDepthExpQ8.
//
// The discharge currency is rivernet.h's: mm/yr of rainfall accumulated
// over the upstream catchment. kRiverAccumThresholdDefault is the smallest
// flow that counts as a river at all, so it is the natural reference point
// for the power law, and the reference width/depth are those of a channel
// at initiation -- a headwater trickle, not a trench.
//
// Cross-section, at perpendicular distance d from the reach centreline:
//
//        natural ground                              natural ground
//   ~~~~~~~~~~~~~~~\                                  /~~~~~~~~~~~~~~~
//                   \  <- bank plane keeps rising    /   (daylight:
//                    \    at kChannelBankRunPerRise /     the cut ends
//        rim = bed+d  \___                      ___/      where the plane
//                         \                    /          meets ground)
//                          \                  /
//                     bed   \________________/
//                           |<-- width -->|
//
//   d <= halfWidth                 -> target = bed                (flat bed)
//   d >  halfWidth                 -> target = bed + (d-halfWidth)/runPerRise
//
// CUT and FILL use different profiles past the rim, and that asymmetry is
// load-bearing:
//
//   cutTarget(d)  = target(d)                 -- keeps rising, so the cut
//                                                DAYLIGHTS into the hillside
//                                                instead of ending in a wall
//   fillTarget(d) = min(target(d), rim)       -- flat at rim height past the
//                                                rim: an embankment, not an
//                                                excavation backfilled
//
//   carved = natural > cutTarget  -> cutTarget                 (excavate)
//            natural < fillTarget -> min(fillTarget,
//                                        natural + maxFill)    (embank)
//            otherwise            -> natural                   (untouched)
//
// The fill half is not cosmetic and is the whole of "banks that hold
// water": a channel that only ever subtracts has a bank exactly as high as
// whatever terrain happened to be there, so anywhere the river runs across
// a side-slope one bank sits below the water line and the reach drains
// sideways.
//
// The fill has to run PAST the rim, which the first cut of this module got
// wrong. Stopping the fill at the rim leaves a knife edge: the rim is at
// bed+depth, but if the ground falls away immediately outside it, the water
// pours straight over. The bank test measured 7763 leaking sides from
// exactly that. Continuing the fill outward at rim height until natural
// ground rises above it -- an embankment that daylights -- is what closes
// the wetted perimeter, and kChannelMaxFillMm bounds it by HEIGHT, so on
// gently falling ground it reaches as far as it needs to while on a cliff
// edge it stops and is counted rather than building an aqueduct.
//
// =========================================================================
// THE GRADED BED, AND WHY IT REACHES THE SEA
// =========================================================================
// The bed is referenced to the AMPLIFIED surface, not to the tile-pixel
// elevation rivernet routed on. That distinction was measured, not
// assumed: on the 30 m coarse tier the amplifier's own detail swings the
// ground by tens of metres either side of the tile elevation, so a bed
// datum taken from the tile leaves a 300 mm headwater channel floating in
// mid-air or buried 20 m underground, and no bounded bank fill can rescue
// it. The first cut of this module did exactly that and the bank test
// reported 8107 leaking sides; referencing the surface took it to zero.
//
// So, per node, in a single downstream pass:
//
//     desired[n] = amplifiedSurface(n) - depth(discharge(n))
//     bed[n]     = min(desired[n], bed[upstream] - minDrop(reach))
//
// The second term is the whole of channel continuity. Without it the bed
// inherits every bump of the detail terms and a long reach becomes a chain
// of disconnected puddles separated by sills -- water in it would not
// flow, which is the one thing a river has to do. With it the bed
// descends STRICTLY, by construction rather than by luck, and a sill in
// the way is simply cut through. That is also what a real river does; the
// stats report how often it happened (sillsCut) and how deep the deepest
// resulting gorge is (maxCutBelowSurfaceMm), so an over-aggressive cut is
// visible as a number instead of as a canyon someone notices later.
//
// The pass needs a topological order and gets one free: rivernet's D8 edge
// always targets a STRICTLY lower tile pixel, so sorting nodes by
// descending tile elevation (ties by node id) visits every node after all
// of its contributors, with no graph traversal and no tie-break ambiguity.
//
// Two invariants follow and are asserted in test_channel.cpp:
//   * bed[n] <= amplifiedSurface(n) - depth(n)  -- the bed is always at
//     least a full channel depth below the ground it was cut from;
//   * bed strictly decreases along every reach -- bedIsStrictlyDescending().
//
// Reaching the sea then follows: sea level is voxel z=0 (core.h), ocean
// ground is below it, and D8 runs to the lowest neighbour, so a chain that
// reaches the coast continues into water. Because depth > 0 everywhere,
// the bed at the coastal node is strictly BELOW that node's surface, so
// the channel mouth is open below sea level and the ocean connects into it
// rather than being separated by a lip. A reach that ends on land instead
// is either a genuine endorheic basin or a D8 pit;
// ChannelFieldStats::outletsAboveSeaLevel counts them, so the failure is
// measured rather than assumed.
//
// =========================================================================
// DETERMINISM
// =========================================================================
// Integer/fixed-point only (CI job `float-ban`, .github/workflows/ci.yml).
// The power laws go through log2Q8/exp2Q8 below -- monotone integer
// log/exp with compile-time mantissa tables -- rather than any pow().
// Every traversal order is derived from rivernet's already-canonical node
// and segment ids, never from hash or container iteration order, so
// digest() is stable across compilers and vendors.

#include <cstdint>
#include <vector>

#include "voxelcore/core.h"
#include "voxelcore/rivernet.h"
#include "voxelcore/tiles.h"

namespace vxc {

// Bumped on any deliberate change to the channel geometry math below.
// Independent of kWorldGenVersion while this module is unwired (nothing
// here reaches evalSurface); folded into it at the wiring bump.
inline constexpr uint32_t kChannelVersion = 1;

// --- integer log2 / exp2, Q8 ---------------------------------------------
//
// A power law is a straight line in log-log space, so the whole
// width/depth law is one multiply once we can take log2 and exp2 in fixed
// point. Both are MONOTONE NON-DECREASING by construction (monotone tables
// + linear interpolation + floor division), which is what lets
// channelWidthMm/channelDepthMm be monotone in discharge -- asserted
// exhaustively over a decade sweep in test_channel.cpp.

// 256 * log2(1 + i/16), i = 0..16, nearest integer.
inline constexpr int32_t kLog2MantissaQ8[17] = {
    0, 22, 44, 63, 82, 100, 118, 134, 150, 165, 179, 193, 207, 220, 232, 244, 256,
};

// 256 * 2^(i/16), i = 0..16, nearest integer.
inline constexpr int32_t kExp2MantissaQ8[17] = {
    256, 267, 279, 292, 304, 318, 332, 347, 362, 378, 395, 412, 431, 450, 470, 490, 512,
};

// log2(v) in Q8 fixed point. v <= 0 is clamped to 1 (log2 = 0) rather than
// being UB: callers feed it discharges, and a zero-discharge segment is a
// legitimate degenerate case (an all-desert catchment), not a programming
// error.
constexpr int64_t log2Q8(int64_t v) {
    if (v <= 1) return 0;
    // Highest set bit index.
    int32_t e = 0;
    for (int64_t t = v; t > 1; t >>= 1) ++e;
    // Normalise the mantissa into [256, 512).
    const int64_t m = (e >= 8) ? (v >> (e - 8)) : (v << (8 - e));
    const int64_t f = m - 256;      // [0, 256)
    const int64_t i = f >> 4;       // table index [0, 16)
    const int64_t r = f & 15;       // interpolation remainder [0, 16)
    const int64_t lo = kLog2MantissaQ8[i];
    const int64_t hi = kLog2MantissaQ8[i + 1];
    return static_cast<int64_t>(e) * 256 + lo + ((hi - lo) * r) / 16;
}

// 2^(lQ8 / 256), floored. Negative exponents return 0 rather than a
// fraction; callers keep their arguments non-negative by construction.
constexpr int64_t exp2Q8(int64_t lQ8) {
    if (lQ8 < 0) return 0;
    const int64_t e = lQ8 >> 8;     // integer part
    const int64_t f = lQ8 & 255;    // fractional part, [0, 256)
    const int64_t i = f >> 4;
    const int64_t r = f & 15;
    const int64_t lo = kExp2MantissaQ8[i];
    const int64_t hi = kExp2MantissaQ8[i + 1];
    const int64_t mant = lo + ((hi - lo) * r) / 16; // [256, 512)
    // mant is 256 * 2^(f/256), so the answer is mant << e >> 8.
    if (e >= 8) {
        if (e - 8 >= 48) return INT64_MAX / 4; // saturate long before overflow
        return mant << (e - 8);
    }
    return mant >> (8 - e);
}

// --- hydraulic geometry ---------------------------------------------------

// Reference channel, at the discharge where a river first exists
// (kRiverAccumThresholdDefault). A stream at channel initiation is roughly
// a metre or two across and ankle-to-knee deep; these are the numbers the
// power law is anchored on, so a headwater trickle can never come out as
// the same trench as a major river.
inline constexpr int64_t kChannelRefWidthMm = 1500; // 1.5 m
inline constexpr int64_t kChannelRefDepthMm = 300;  // 0.3 m

// Exponents in Q8: width ∝ Q^0.40 (matches the bake's width ∝ A^0.4),
// depth ∝ Q^0.35 (midpoint of the design doc's 0.3..0.4).
inline constexpr int64_t kChannelWidthExpQ8 = 102; // 0.3984
inline constexpr int64_t kChannelDepthExpQ8 = 90;  // 0.3516

// Caps. The depth cap matches the server bake's own incision cap
// (terrain_service/bake/incise.py cap_m = 25.0) so the two agree about how
// deep a channel can ever get; the width cap is a sanity rail well above
// any river a 30 m tile can resolve.
inline constexpr int64_t kChannelMaxDepthMm = 25'000;   // 25 m
inline constexpr int64_t kChannelMaxWidthMm = 400'000;  // 400 m

// Bank plane run:rise. 2:1 (~26.6 deg) is a stable, natural-looking bank in
// a voxel world and daylights into a hillside quickly.
inline constexpr int64_t kChannelBankRunPerRise = 2;

// How far past the rim the cut-only bank plane is allowed to keep rising
// before the channel simply stops influencing terrain, as a multiple of the
// bank run. Bounds the spatial index; without it the daylight run is
// unbounded on a steep valley wall.
inline constexpr int64_t kChannelDaylightRuns = 4;

// Maximum height of built bank (levee) at any one column. Fill exists to
// close the wetted perimeter where the river crosses a side-slope, not to
// build an aqueduct across a valley; past this the bank is left short and
// counted in ChannelFieldStats::fillClampedColumns.
inline constexpr int64_t kChannelMaxFillMm = 4'000; // 4 m

// Design water depth as a fraction of channel depth, and hence the water
// line the banks must contain. 3/4 leaves a quarter-depth freeboard, so
// the rim IS the bank crest and the cut and fill profiles are the same
// surface.
inline constexpr int64_t kChannelWaterDepthNum = 3;
inline constexpr int64_t kChannelWaterDepthDen = 4;

// Bed liner thickness: the top of the carved bed is alluvium (MAT_GRAVEL),
// not whatever stratum the cut happened to expose.
inline constexpr int64_t kChannelBedLinerMm = 300;
inline constexpr MaterialId kChannelBedMaterial = MAT_GRAVEL;
inline constexpr MaterialId kChannelBankMaterial = MAT_CLAY;

// Minimum bed grade enforced downstream, as a run per unit rise: 1000 means
// the bed must fall at least 1 mm per 1000 mm of reach (0.1%). Anything
// gentler is indistinguishable from flat at 100 mm voxels, and flat is what
// turns a reach into a chain of puddles.
inline constexpr int64_t kChannelMinGradeRun = 1000;

// The minimum drop the bed must take across a reach of the given planar
// length. At least one voxel, so the descent is visible at the world's own
// resolution and not just arithmetically true.
constexpr int64_t channelMinDropMm(int64_t reachLengthMm) {
    const int64_t d = reachLengthMm / kChannelMinGradeRun;
    return d < kVoxelSizeMm ? kVoxelSizeMm : d;
}

// --- the surface the channel is cut into ----------------------------------
//
// Deliberately an interface rather than a concrete Amplifier reference:
// channel.h stays free of amplifier.h (and of biome/caves/caverns behind
// it), and a caller that wants the bed referenced to something else -- a
// mip level, a bake raster, a test stub -- can say so.
class IChannelSurface {
public:
    virtual ~IChannelSurface() = default;
    virtual int32_t surfaceMm(int64_t vx, int64_t vy) = 0;
};

// Adapts anything with `int32_t surfaceMm(int64_t, int64_t) const` --
// vxc::Amplifier, notably -- without channel.h having to know the type.
template <typename T>
class ChannelSurfaceAdapter final : public IChannelSurface {
public:
    explicit ChannelSurfaceAdapter(const T& src) : src_(&src) {}
    int32_t surfaceMm(int64_t vx, int64_t vy) override { return src_->surfaceMm(vx, vy); }

private:
    const T* src_;
};

template <typename T>
ChannelSurfaceAdapter<T> channelSurfaceOf(const T& src) {
    return ChannelSurfaceAdapter<T>(src);
}

// The degenerate surface: the tile-pixel elevation itself, with no
// amplification. Correct ONLY where nothing adds detail on top (coarse
// previews, graph-only tests). Using it on a real world is the bug
// described in the header comment.
class TileElevationSurface final : public IChannelSurface {
public:
    explicit TileElevationSurface(ITileSampler& tiles) : tiles_(&tiles) {}
    int32_t surfaceMm(int64_t vx, int64_t vy) override {
        const int64_t pxMm = tiles_->pixelSizeMm();
        return tiles_->elevationMm(floorDiv(vx * kVoxelSizeMm, pxMm),
                                   floorDiv(vy * kVoxelSizeMm, pxMm));
    }

private:
    ITileSampler* tiles_;
};

// Channel width for a given rivernet discharge (mm/yr accumulated upstream
// rainfall). Monotone non-decreasing; exactly kChannelRefWidthMm at and
// below the river-formation threshold.
constexpr int64_t channelWidthMm(int64_t discharge) {
    const int64_t lq = log2Q8(discharge) - log2Q8(kRiverAccumThresholdDefault);
    if (lq <= 0) return kChannelRefWidthMm;
    const int64_t w = exp2Q8(log2Q8(kChannelRefWidthMm) + (lq * kChannelWidthExpQ8) / 256);
    return clampi64(w, kChannelRefWidthMm, kChannelMaxWidthMm);
}

// Channel depth for a given rivernet discharge. Monotone non-decreasing;
// exactly kChannelRefDepthMm at and below the threshold.
constexpr int64_t channelDepthMm(int64_t discharge) {
    const int64_t lq = log2Q8(discharge) - log2Q8(kRiverAccumThresholdDefault);
    if (lq <= 0) return kChannelRefDepthMm;
    const int64_t d = exp2Q8(log2Q8(kChannelRefDepthMm) + (lq * kChannelDepthExpQ8) / 256);
    return clampi64(d, kChannelRefDepthMm, kChannelMaxDepthMm);
}

// The bank run (horizontal distance from rim edge back down to bed level)
// for a channel of the given depth.
constexpr int64_t channelBankRunMm(int64_t depthMm) {
    return depthMm * kChannelBankRunPerRise;
}

// Total horizontal reach of the channel's influence either side of the
// centreline, including the daylighting cut-only plane. Sizes the index.
constexpr int64_t channelInfluenceMm(int64_t widthMm, int64_t depthMm) {
    return widthMm / 2 + channelBankRunMm(depthMm) * (1 + kChannelDaylightRuns);
}

// Target ground elevation of the channel cross-section at perpendicular
// distance dMm from the centreline, for a reach with the given bed
// elevation and depth. Monotone non-decreasing in dMm.
constexpr int64_t channelTargetMm(int64_t dMm, int64_t bedMm, int64_t depthMm, int64_t widthMm) {
    const int64_t halfW = widthMm / 2;
    if (dMm <= halfW) return bedMm;
    const int64_t run = channelBankRunMm(depthMm);
    if (run <= 0) return bedMm;
    return bedMm + ((dMm - halfW) * depthMm) / run;
}

// --- the field ------------------------------------------------------------

struct ChannelSample {
    // True where this column is inside the channel footprint at all (i.e.
    // the channel has an opinion about the ground height here).
    bool influenced = false;
    // True inside the flat bed (d <= halfWidth) -- the wetted bed proper.
    bool inBed = false;
    // True between the bed edge and the rim: the built/cut bank.
    bool inBank = false;

    int32_t bedMm = 0;         // graded bed elevation of the nearest reach
    int32_t targetMm = 0;      // cut profile: cross-section elevation, rising past the rim
    int32_t fillTargetMm = 0;  // fill profile: min(targetMm, rimMm) -- flat past the rim
    int32_t rimMm = 0;         // bed + depth: bank crest / top of channel
    int32_t waterLineMm = 0;   // bed + design water depth; banks must contain this
    int32_t depthMm = 0;       // reach depth
    int32_t widthMm = 0;       // reach width
    int32_t dischargeAt = 0;   // reach discharge (mm/yr upstream rainfall)
    uint32_t segId = 0;        // nearest reach, RiverNetwork::kNoSegment if none
};

struct ChannelFieldStats {
    uint32_t segments = 0;
    uint32_t nodes = 0;
    // Terminal (outlet) nodes whose graded bed is still above sea level:
    // rivers that do not reach the sea inside the built region. Either a
    // genuine endorheic basin or a D8 pit; either way, measured.
    uint32_t outletsAboveSeaLevel = 0;
    uint32_t outlets = 0;
    // Reaches whose bed is at or below sea level: the tidal/marine part of
    // the network.
    uint32_t segmentsBelowSeaLevel = 0;
    // Nodes where the minimum-grade constraint, not the local surface, set
    // the bed -- i.e. the channel cut through a sill instead of climbing
    // over it. Some of this is correct and necessary; a lot of it means the
    // routing tier and the surface tier disagree about where downhill is.
    uint32_t sillsCut = 0;
    // Deepest cut below the local amplified surface, over all nodes. The
    // number that would show up in game as a gorge.
    int64_t maxCutBelowSurfaceMm = 0;
    int64_t minBedMm = 0;
    int64_t maxBedMm = 0;
    int64_t maxWidthMm = 0;
    int64_t maxDepthMm = 0;
};

class ChannelField {
public:
    // Builds the channel geometry for `net` (already built over `bounds`
    // from the same `tiles`). `surface` is the ground the channel is cut
    // into -- pass channelSurfaceOf(amplifier) for a real world; see the
    // header comment for why the tile elevation is NOT good enough. Clears
    // any previous build. One surface query per node plus O(segments); no
    // per-voxel work happens here.
    void build(ITileSampler& tiles, IChannelSurface& surface, const RiverNetwork& net,
               const RegionBounds& bounds);

    // O(1) point query: the channel's opinion about world voxel column
    // (vx, vy). influenced == false means "no river here, leave the
    // amplified surface alone".
    ChannelSample sampleAt(int64_t vx, int64_t vy) const;

    // The ground surface after the channel is applied to a natural
    // (amplified) surface height. This is the single definition of the
    // carve that the applicator, the tests and any future wiring pass all
    // share, so they cannot disagree:
    //   - outside the footprint     -> naturalSurfaceMm unchanged
    //   - above the cut profile     -> excavated down to it
    //   - below the fill profile    -> embanked up to it, bounded by
    //                                  kChannelMaxFillMm of built height
    //   - between the two           -> unchanged
    int32_t surfaceMm(int64_t vx, int64_t vy, int32_t naturalSurfaceMm) const;

    // Same, but reporting whether the fill was clamped by
    // kChannelMaxFillMm -- i.e. this column's bank is SHORT and the reach
    // can leak here. Drives ChannelFieldStats and the bank test.
    int32_t surfaceMm(int64_t vx, int64_t vy, int32_t naturalSurfaceMm, bool& fillClamped) const;

    // Per-node graded bed elevation, indexed by rivernet node id.
    const std::vector<int32_t>& nodeBedMm() const { return nodeBedMm_; }
    // Per-node discharge (mm/yr upstream rainfall), indexed by node id.
    const std::vector<int32_t>& nodeDischarge() const { return nodeDischarge_; }

    const ChannelFieldStats& stats() const { return stats_; }
    bool empty() const { return segs_.empty(); }

    // Checks the theorem in the header comment: every segment's downstream
    // bed is strictly below its upstream bed. True on any correctly built
    // field; a false here means rivernet's D8 or accumulation invariants
    // changed under us.
    bool bedIsStrictlyDescending() const;

    // Deterministic digest over every reach in rivernet's canonical
    // segment-id order.
    void digest(Digest& d) const;

private:
    struct Reach {
        int64_t ax = 0, ay = 0;    // upstream end, world voxel coords
        int64_t bx = 0, by = 0;    // downstream end, world voxel coords
        int32_t bedAMm = 0, bedBMm = 0;
        int32_t depthAMm = 0, depthBMm = 0;
        int32_t widthAMm = 0, widthBMm = 0;
        int32_t discharge = 0;
        int64_t lenSq = 0;         // |AB|^2 in voxels^2, 0 for a degenerate reach
    };

    // Uniform grid over the region, one cell per tile pixel.
    int64_t pixelSizeMm_ = 0;
    RegionBounds bounds_{};
    int64_t gw_ = 0, gh_ = 0;
    std::vector<uint32_t> cellStart_;  // gw_*gh_ + 1 prefix offsets into cellSegs_
    std::vector<uint32_t> cellSegs_;

    std::vector<Reach> segs_;
    std::vector<int32_t> nodeBedMm_;
    std::vector<int32_t> nodeDischarge_;
    ChannelFieldStats stats_{};

    // Nearest-reach evaluation shared by sampleAt/surfaceMm.
    ChannelSample evaluate(int64_t vx, int64_t vy) const;
};

} // namespace vxc
