#pragma once
// Cavern pass v1 (M4 cave pass v2, docs/cavern-design.md, folding in Matt's
// post-design decisions) — hash-gated, LARGE-and-RARE cavern sites anchored
// on the existing tunnel lattice's backbone-crossing nodes, each a coaxial
// CHAIN of ellipsoid "storeys" descending from the anchor, plus a per-site
// flood level.
//
// Header-only, integer-only (CI float-ban). This is C1 of the design's build
// plan (docs/cavern-design.md §7): new file, no other file touched, standalone
// and testable. C4 wires `cavernColumnFor`'s output into `ColumnSample` and
// `Amplifier::materialAt`, exactly the way `voxelcore/caves.h`'s `CaveColumn`
// is wired in today. Every expression here must eventually be mirrored
// bit-for-bit in `voxel-core/shaders/worldgen.ush` (C6) — same discipline as
// caves.h: integer math only, `floorDiv`/power-of-two masks instead of signed
// `%`, no unbounded allocation.
//
// ---------------------------------------------------------------------------
// POST-DESIGN CHANGES (Matt, folded in here rather than v1's spec)
// ---------------------------------------------------------------------------
// 1. Bedrock is moving from ~40-60 m to ~200 m depth (a later, amplifier.cpp
//    change this file does NOT own or assume the exact value of). This file
//    therefore makes NO compile-time assumption about how deep bedrock is:
//    the only place bedrock matters is the ordinary per-voxel runtime clamp
//    in `cavernCarveAt`, which takes `bedrockDepthMm` as a parameter exactly
//    like caves.h's `caveCarveAt` already does — already correctly
//    parameterised, already safe whether bedrock is 40 m or 200 m. There is
//    no "kCavernBedrockCeilingMm" constant anywhere below.
// 2. Caverns are LARGER and RARER than the original spec (12-28 m wide,
//    5-12 m tall, 1-in-4 gate at 102.4 m spacing): sites now sit 204.8 m
//    apart (every 8th lattice node, still a guaranteed backbone crossing —
//    8 is a multiple of caves.h's own backbone period of 4) and each open
//    site is a coaxial CHAIN of up to 4 rooms (24-56 m wide) descending from
//    the anchor, using the depth room the bedrock move frees up (see #3).
// 3. "Design for the vertical range": a single shallow ellipsoid centred on
//    the tunnel-depth anchor cannot get much taller without either poking
//    above the roof clamp (anchor depth tops out at 34 m, so a large upward
//    reach breaches daylight) or needing an assumed bedrock ceiling (#1). So
//    the EXTRA vertical range goes downward, as a CHAIN: child 0 is the
//    small "entrance chamber" at the anchor (tied to the tunnel depth, roof-
//    safe by the same static_assert style as before); children 1-3 each sit
//    directly below the PREVIOUS child (same xy — see "why coaxial" below),
//    independently sized, so the whole site can span up to ~40-100+ m of
//    depth. Whatever doesn't fit above the real bedrock at a given column is
//    truncated by the ordinary runtime clamp, same as it always was.
// 4. Underground water (design doc §5, approved as designed, implemented in
//    C7/C8) is unaffected: `CavernColumn.floodZMm` and `CH_CAVERN_FLOOD`
//    stay exactly as before, and every child still carries an own-center
//    flat floor, so C7 has a well-defined basin to flood.
//
// WHY COAXIAL (children 1-3 share the anchor's xy, not an independent offset)
// ---------------------------------------------------------------------------
// Two ellipsoids of revolution (rx == ry == rxy, semi-axis rz) centred a
// displacement (dxy, dz) apart do NOT have a simple exact overlap test in
// general — but if dxy == 0 (same xy, i.e. coaxial), the two ellipsoids'
// horizontal cross-sections at any given z are full circles on the SAME axis,
// so overlap reduces EXACTLY to the 1D interval test |dz| < rz0 + rz1. That
// is what makes the connectivity argument for a 4-room chain a clean,
// static_assert-able fact instead of an approximation: build the chain with
// zero horizontal offset between consecutive rooms, and the same worst-case
// static_assert style caves.h already uses (compare hashed MINIMUMS) proves
// every consecutive pair overlaps, hence (transitively) the whole chain's
// union is one connected blob for every possible hash draw. It also means
// distance-to-anchor (dxySq) is now computed ONCE PER COLUMN and reused for
// all 4 children instead of once per child — a real cost reduction, keeping
// faith with "the perf pass just landed 2.75x, do not give it back."
//
// ---------------------------------------------------------------------------
// WHY THIS RIDES THE EXISTING PER-COLUMN REDUCTION (design doc §0)
// ---------------------------------------------------------------------------
// A cavern is a genuinely 3D volume, but restricted to one column it is just a
// set of z-intervals, and for an ellipsoid that interval bound is exactly the
// tunnel test's shape: an ellipsoid with vertical semi-axis rz, intersected
// with a column at horizontal distance-squared dxy² from its center and
// horizontal semi-axis rxy, gives the interval
//
//     dz² < rz²·(rxy² − dxy²) / rxy²
//
// i.e. one per-column division producing one marginSq, then the same
// `dz*dz < marginSq` per voxel that caves.h already has. So caverns cost the
// per-voxel loop nothing new in shape — the entire new cost is PLACEMENT
// (deciding, once per column, which few columns are even near a cavern).
//
// ---------------------------------------------------------------------------
// PLACEMENT: HASH-GATED SITES AT BACKBONE-CROSSING NODES (design doc §3.1)
// ---------------------------------------------------------------------------
// Cavern sites live at every 8th lattice node in both axes (`(i & 7) == 0 &&
// (j & 7) == 0`, i.e. `kCavernCoarseLatticeRatio` cave-lattice cells, 204.8 m
// apart, `kCavernCoarseMm`) — a strict subset of the backbone-crossing nodes
// caves.h's sinkhole shafts use (every 4th), so every candidate is still
// guaranteed four incident backbone tunnels. The anchor point IS that exact
// node (same `caveNode()` call, same `CH_CAVE_NODE` channel, so bit-identical
// to the tunnel network's own node), at absolute height
//
//     anchorZ = surfaceMmAt(nx, ny) − node.depthMm
//
// using the SAME `node.depthMm` the tunnel axis uses at that point. That is
// what makes connectivity STRUCTURAL rather than a flood-fill statistic: the
// cavern's child-0 ellipsoid is centered exactly on the anchor with radius
// well above zero in every axis, so it always contains (nx, ny, anchorZ) —
// and that is exactly the point every incident backbone tunnel also carves,
// by the identical construction caves.h already proves connected. See
// test_caverns.cpp's connectivity test for the flood-filled verification.
//
// `surfaceMmAt` is evaluated at the SITE'S OWN (nx, ny) — not the querying
// column's position — because a lake or room floor that drapes with the
// terrain overhead is visibly wrong; caverns anchor at absolute z so floors
// and water tables are level. This is the one place the cavern pass needs a
// surface height at an xy other than the column being queried, hence the
// `surfaceAt` callback threaded through `cavernSiteFor`/`cavernColumnFor`
// below: the caller (Amplifier::column in C4) already owns the tile raster
// and can supply its own bilinear+octave surface function. Callers must not
// widen a GPU column-cache struct to carry this; C6 recomputes it inside
// VoxelizeMain instead (design doc §3.5).
//
// ---------------------------------------------------------------------------
// WHY THE COMMON-CASE COLUMN IS CHEAP (design doc §3.4, §3.7)
// ---------------------------------------------------------------------------
// A site's maximum reach from its anchor (widest possible roughened room
// radius, `kCavernMaxReachMm` — there is no offset term any more, see "why
// coaxial" above) is safely under half the 204.8 m coarse spacing, so:
//   * the 2x2 block of coarse-lattice corners around the column's own coarse
//     cell is EXHAUSTIVE (nothing further away can possibly reach it) —
//     `static_assert` below;
//   * at most ONE of those 4 corners can ever be an open, in-reach site for
//     any given column — `static_assert` below, which is what lets
//     `cavernColumnFor` just accumulate into one `CavernColumn` with no
//     "which site wins" tie-break to define.
// Per corner, the FIRST thing tested is a single gate bit folded into the
// hash `caveNode()` already computes for node-jitter (`CH_CAVE_NODE`) — see
// `cavernSiteGateOpen` — so the common "gate closed" case costs nothing
// beyond a hash call the tunnel system's own node lookup would otherwise
// also need. Only once a corner passes gate + a cheap depth-safety window +
// a cheap xy-distance reject does `cavernSiteFor` run — the ONE place that
// reads the terrain raster (`surfaceAt`), decodes the up-to-4-room chain and
// the flood level. The wider spacing (204.8 m vs the original 102.4 m) means
// this full-reduction tier fires on a QUARTER as many columns as before,
// purely from geometry, before the gate probability is even applied —
// exactly the "rarity helps the budget" property Matt asked to keep.
//
// ---------------------------------------------------------------------------
// SAFETY GUARDS ON SLOPES (design doc §3.6) — read this before retuning
// ---------------------------------------------------------------------------
// Tunnels carve in DEPTH SPACE (drape under the local surface), so their roof
// and bedrock clamps are backstops for geometry that already respects them
// everywhere (caves.h's static_asserts prove it). Caverns anchor at ABSOLUTE
// z, so a level room under sloping terrain cannot have that property: uphill
// columns see it deeper, downhill columns see it shallower (or breached).
// The per-voxel clamps in `cavernCarveAt` — roof, bedrock, sea level — are
// therefore LOAD-BEARING here, not just backstops, exactly the same clamps
// caves.h already applies, reused verbatim (`kCaveRoofMinMm`,
// `kCaveBedrockMarginMm`, `kCaveMinVoxelZ`, `kCaveMinSurfaceMm`). A cavern
// near a slope gets truncated with full cover rather than ever creating a
// surprise skylight, a floating floor, or a sea breach; test_caverns.cpp
// measures this on genuinely varied (not flat) terrain and reports the
// minimum roof cover actually observed, the same way it is already the
// arbiter for tunnel pinching at cliffs.
//
// Separately, ONLY child 0 (the entrance chamber, sitting exactly at the
// anchor) needs a compile-time safety window on the node's own depth — it is
// the one room whose center is tied to the shallow (9-34 m) tunnel band, so
// it is the only one that can threaten the ROOF (children 1-3 only ever sit
// deeper). `kCavernNodeDepthSafeMinMm` guarantees child 0's own widest
// possible radius cannot reach the roof clamp before any per-voxel guard is
// even consulted — "sizing at the anchor column is structural" per the
// design doc, `static_assert`-pinned below.

#include "voxelcore/caves.h"

namespace vxc {

// --- worldgen contract constants (tune only on a kWorldGenVersion bump) -----

// Cavern sites sit on a strict subset of the sinkhole-shaft lattice class
// (every Nth cave-lattice node in both axes, N = kCavernCoarseLatticeRatio),
// 204.8 m apart -- 2x caves.h's own 102.4 m shaft spacing, i.e. a QUARTER as
// many candidate corners per unit area, the "rarer" half of "large but rare."
inline constexpr int64_t kCavernCoarseLatticeRatio = 8;
inline constexpr int64_t kCavernCoarseMm = kCaveLatticeMm * kCavernCoarseLatticeRatio;

// 1-in-4 site gate, decoded from bits the node-jitter hash otherwise leaves
// unused (caveNode() consumes bits 0..59 of hash2(seed,i,j,CH_CAVE_NODE) for
// x/y jitter + depth; bits 60/61 are free). This is the "fold gate bits into
// the node-jitter hash" optimization the design doc calls out (§3.7):
// testing it costs nothing beyond the hash caveNode() already computes.
inline constexpr uint64_t kCavernSiteGateMask = 3; // bits 60..61 == 0 -> open

// Beach/ocean guard on the SITE's own surface (not the querying column's) —
// stricter than caves.h's kCaveMinSurfaceMm because caverns are taller and a
// half-drowned cliffside room reads far worse than a half-drowned tunnel.
inline constexpr int32_t kCavernMinSurfaceMm = 20000;

// --- chain geometry (design doc §3.2, widened for "large but rare") --------

// Four rooms per open site: child 0 is the entrance chamber at the anchor
// (tied to the shallow tunnel depth); children 1..3 chain downward from the
// previous room, same xy each time (see "why coaxial" above).
inline constexpr int32_t kCavernChildCount = 4;

// Horizontal semi-axis, shared by every room in the chain (rx == ry — an
// oblate spheroid, round in plan). 24-56 m diameter -- 2x the original
// spec's 12-28 m wide, satisfying "bigger."
inline constexpr int64_t kCavernRxyMinMm = 12000;  // 12 m
inline constexpr int64_t kCavernRxySpanMm = 16000; // -> [12, 28) m
inline constexpr int64_t kCavernRxyMaxMm = kCavernRxyMinMm + kCavernRxySpanMm;

// Child 0's vertical semi-axis: kept modest so it stays roof-safe across
// most of the tunnel node depth range (see the safety-window constants
// below) -- it is the "doorway," not the main room.
inline constexpr int64_t kCavernRz0MinMm = 4000;  // 4 m
inline constexpr int64_t kCavernRz0SpanMm = 6000; // -> [4, 10) m
inline constexpr int64_t kCavernRz0MaxMm = kCavernRz0MinMm + kCavernRz0SpanMm;

// Children 1..3's vertical semi-axis: the actual "use the new depth" room,
// independently hashed per room, up to 40 m tall (80 m full height) each.
inline constexpr int64_t kCavernRzDeepMinMm = 12000;  // 12 m
inline constexpr int64_t kCavernRzDeepSpanMm = 28000; // -> [12, 40) m
inline constexpr int64_t kCavernRzDeepMaxMm = kCavernRzDeepMinMm + kCavernRzDeepSpanMm;

// Flat floor, own-center-relative for every room (a chain of 40-80 m tall
// rooms sharing one floor reference stopped making sense once the rooms are
// no longer clustered around one point).
inline constexpr int64_t kCavernFloorDropMinMm = 3000;  // 3 m
inline constexpr int64_t kCavernFloorDropSpanMm = 12000; // -> [3, 15) m

// Downward chain step: child[c]'s center sits this far below child[c-1]'s
// (child 0 has none -- it IS the anchor). Bounded (see static_asserts below)
// so consecutive rooms always overlap regardless of which radii got hashed.
inline constexpr int64_t kCavernStepDownMinMm = 2000;  // 2 m
inline constexpr int64_t kCavernStepDownSpanMm = 12000; // -> [2, 14) m

// --- wall roughness (design doc §3.2) ---------------------------------------

// Per-column 2D value noise multiplies the reduced xy reach (rxy²) before the
// margin division, so walls/ceilings undulate at zero per-voxel cost — one
// sample shared by every room in the column. Lattice widened vs the original
// spec (3.2 m) to stay visually proportional to the much bigger rooms.
inline constexpr int64_t kCavernRoughLatticeMm = 6400; // 6.4 m
inline constexpr int64_t kCavernRoughAmpQ10 = 307;     // +/-0.3 of 1024
inline constexpr int64_t kCavernRoughMinQ10 = 1024 - kCavernRoughAmpQ10; // 717 (~0.70)
inline constexpr int64_t kCavernRoughMaxQ10 = 1024 + kCavernRoughAmpQ10; // 1331 (~1.30)

// --- child-0 depth safety window (design doc §3.6) --------------------------
// Only child 0 needs this: it is the only room tied to the shallow (9-34 m)
// tunnel-node depth, so it is the only one that can threaten the roof.
// Deliberately NOT bounded above by any assumed bedrock depth (see the
// header comment's post-design-change note #1) -- the natural ceiling is
// simply caveNode()'s own depth range; going deeper than that never happens
// because node.depthMm cannot produce it.
inline constexpr int64_t kCavernRoofSafetyMarginMm = 2000;
inline constexpr int64_t kCavernNodeDepthSafeMinMm =
    kCaveRoofMinMm + kCavernRoofSafetyMarginMm + kCavernRz0MaxMm; // 6000+2000+10000 = 18000
inline constexpr int64_t kCavernNodeDepthSafeMaxMm =
    kCaveNodeDepthMinMm + kCaveNodeDepthSpanMm; // 34000, caveNode()'s own ceiling

// --- candidate reach (design doc §3.4) --------------------------------------

// Safe bound on the widest a roughened room radius can reach from the anchor
// (there is no per-child xy offset any more -- every room shares the
// anchor's xy, see "why coaxial" above -- so this is purely the widest
// radius scaled by the roughness ceiling; linear, not the tighter
// sqrt(roughness) the marginSq formula implies -- conservative on purpose).
inline constexpr int64_t kCavernMaxReachMm = (kCavernRxyMaxMm * kCavernRoughMaxQ10) / 1024;
inline constexpr int64_t kCavernMaxReachSqMm = kCavernMaxReachMm * kCavernMaxReachMm;

// --- flood level (design doc §5.1, approved as designed) --------------------

inline constexpr int64_t kCavernFloodMinMm = 800;  // 0.8 m above the highest room floor
inline constexpr int64_t kCavernFloodSpanMm = 2400; // -> [0.8, 3.2) m
// 40% of open sites are dry, rounded down from 0.4 * 2^32 in pure integer
// math (no floating literal, per the float-ban): a uniform top-32-bit hash
// value below this threshold means dry.
inline constexpr uint32_t kCavernFloodDryThreshold32 = static_cast<uint32_t>((4ull << 32) / 10);

// Max cavern segments recorded per column. At most one site can be in reach
// of any column (static_assert below), contributing at most kCavernChildCount
// (4) segments; 6 leaves headroom exactly like caves.h's kMaxCaveSegs does
// for tunnels, without changing the underlying "at most one site" argument.
inline constexpr int32_t kMaxCavernSegs = 4; // tight == kCavernChildCount: the provable max (see comment above + static_assert below); verified empirically (max 4 over 16M+ columns, test_caverns.cpp's cavern_segment_cap_headroom).

// --- structural invariants, checked at compile time -------------------------

// Chain overlap, step 0->1: child 1 must overlap child 0 for EVERY possible
// hash draw. Coaxial (dxy=0), so overlap is the exact 1D test
// stepDown < rz0 + rz1; checked against the worst case (both radii and the
// step at their least/most overlap-hostile values).
static_assert(kCavernStepDownMinMm + kCavernStepDownSpanMm < kCavernRz0MinMm + kCavernRzDeepMinMm,
              "child 1 must overlap child 0 (coaxial 1D interval test) for every "
              "possible hashed radius/step, or the chain is not guaranteed connected");
// Chain overlap, steps 1->2 and 2->3: both ends use the "deep room" range.
static_assert(kCavernStepDownMinMm + kCavernStepDownSpanMm < 2 * kCavernRzDeepMinMm,
              "consecutive deep rooms must overlap for every possible hashed radius/"
              "step, or the chain is not guaranteed connected");

// The depth safety window must be non-empty and must sit inside the range
// caveNode() can actually produce (kCaveNodeDepthMinMm..+kCaveNodeDepthSpanMm)
// -- otherwise no site could ever pass it.
static_assert(kCavernNodeDepthSafeMinMm < kCavernNodeDepthSafeMaxMm,
              "cavern depth safety window must be non-empty");
static_assert(kCavernNodeDepthSafeMinMm >= kCaveNodeDepthMinMm,
              "cavern depth safety window must sit inside caveNode()'s range");

// The 2x2 corner block is only exhaustive while nothing further away can
// reach the column: max reach from an anchor, plus the anchor's own worst-
// case jitter within its lattice cell, must stay under one coarse cell.
static_assert(kCavernMaxReachMm + kCaveLatticeMm < kCavernCoarseMm,
              "cavern reach must stay well under one coarse cell, or the 2x2 "
              "candidate-corner block in cavernColumnFor stops being exhaustive");
// At most one open, in-reach site can ever cover a given column: two
// adjacent corners are at least (coarse spacing - one jitter cell) apart,
// which must exceed twice the max reach.
static_assert(2 * kCavernMaxReachMm < kCavernCoarseMm - kCaveLatticeMm,
              "two adjacent cavern sites' reach discs must never both cover the "
              "same column, or cavernColumnFor's single-site accumulation is wrong");

// --- hash channels -----------------------------------------------------------
// Extends caves.h's registry (18..21, 24 reserved for CH_CREVICE in
// caves.h). APPEND ONLY, never renumber.
inline constexpr uint32_t CH_CAVERN_SITE = 22;  // per-room geometry (radii/floor/step)
inline constexpr uint32_t CH_CAVERN_ROUGH = 23; // per-column wall roughness
inline constexpr uint32_t CH_CAVERN_FLOOD = 25; // per-site flood level

// --- small helpers -----------------------------------------------------------

// Unsigned 10-bit field -> [0, spanMm), multiply-then-shift (never a
// division), matching caveNode()'s 20-bit fields but narrower since a room
// needs at most four independent fields out of one 64-bit hash.
constexpr int64_t cavernHashField10(uint64_t h, int32_t shift, int64_t spanMm) {
    return static_cast<int64_t>((((h >> shift) & 0x3FFu) * static_cast<uint64_t>(spanMm)) >> 10);
}

// --- site gate + candidacy ---------------------------------------------------

// True if the backbone-crossing node (fi, fj) is a cavern site candidate
// (folded into the node-jitter hash — see the constant comment above). Caller
// must already know (fi & 7) == 0 && (fj & 7) == 0; every caller here
// constructs fi/fj that way by construction, so no runtime mask check.
constexpr bool cavernSiteGateOpen(uint64_t seed, int64_t fi, int64_t fj) {
    return ((hash2(seed, fi, fj, CH_CAVE_NODE) >> 60) & kCavernSiteGateMask) == 0;
}

// True if a node at this depth can host child 0 (the entrance chamber, tied
// to this exact depth) without its geometry alone -- before any per-voxel
// clamp -- violating the roof. See the safety-window constant comments for
// why there is no bedrock-side bound here.
constexpr bool cavernDepthIsSafe(int64_t nodeDepthMm) {
    return nodeDepthMm >= kCavernNodeDepthSafeMinMm && nodeDepthMm <= kCavernNodeDepthSafeMaxMm;
}

// --- per-site geometry (the rare "full reduction" tier) ---------------------

struct CavernChild {
    int64_t zMm = 0;     // absolute center (xy is always the site's anchor xy)
    int64_t rxyMm = 0;   // horizontal semi-axis (rx == ry: oblate spheroid)
    int64_t rzMm = 0;    // vertical semi-axis
    int64_t zFloorMm = 0; // absolute flat-floor clamp, own-center-relative
};

struct CavernSite {
    bool valid = false;
    int64_t anchorXMm = 0, anchorYMm = 0, anchorZMm = 0;
    CavernChild children[kCavernChildCount] = {};
    int32_t floodZMm = INT32_MIN; // INT32_MIN = dry (or invalid)
};

// Full geometry for the cavern site anchored at backbone-crossing node
// (fi, fj), given its already-computed CaveNode `node` (caller has it from
// the xy-distance reject already — recomputed here would be a second,
// redundant hash call). `surfaceAt(xMm, yMm)` must return the same surface
// height the querying column's own `surfaceMm` came from (Amplifier's
// bilinear-tile-base + detail-octave function in production; C4 supplies it,
// e.g. as a lambda over `Amplifier::column(...).surfaceMm` or a factored-out
// raw surface function). Called at most once per column (see the
// "at most one site in reach" static_assert above) and only after the cheap
// gate/depth/xy-reach checks in `cavernColumnFor` already passed — this is
// the one place in the whole cavern pass that reads the terrain raster.
//
// Every room after child 0 chains directly below the PREVIOUS room (same
// xy — "why coaxial" in the header comment), so the whole site's footprint
// is exactly the anchor's xy for every room; only the per-room z, radii and
// floor vary.
template <typename SurfaceFn>
constexpr CavernSite cavernSiteFor(uint64_t seed, int64_t fi, int64_t fj, const CaveNode& node,
                                    const SurfaceFn& surfaceAt) {
    CavernSite site;
    const int32_t siteSurfaceMm = surfaceAt(node.xMm, node.yMm);
    if (siteSurfaceMm < kCavernMinSurfaceMm) return site; // beach/ocean guard on the SITE

    site.anchorXMm = node.xMm;
    site.anchorYMm = node.yMm;
    site.anchorZMm = static_cast<int64_t>(siteSurfaceMm) - node.depthMm;

    int64_t maxFloorZMm = INT64_MIN;
    int64_t prevZMm = site.anchorZMm;
    for (int32_t c = 0; c < kCavernChildCount; ++c) {
        const uint64_t h = hash3(seed, fi, fj, c, CH_CAVERN_SITE);
        const bool isRoot = (c == 0);

        const int64_t rxyMm = kCavernRxyMinMm + cavernHashField10(h, 0, kCavernRxySpanMm);
        const int64_t rzMm = isRoot ? kCavernRz0MinMm + cavernHashField10(h, 10, kCavernRz0SpanMm)
                                     : kCavernRzDeepMinMm + cavernHashField10(h, 10, kCavernRzDeepSpanMm);
        const int64_t floorDropMm =
            kCavernFloorDropMinMm + cavernHashField10(h, 20, kCavernFloorDropSpanMm);
        const int64_t stepDownMm =
            isRoot ? 0 : kCavernStepDownMinMm + cavernHashField10(h, 30, kCavernStepDownSpanMm);

        const int64_t zMm = isRoot ? site.anchorZMm : prevZMm - stepDownMm;
        prevZMm = zMm;

        CavernChild& ch = site.children[c];
        ch.zMm = zMm;
        ch.rxyMm = rxyMm;
        ch.rzMm = rzMm;
        ch.zFloorMm = zMm - floorDropMm;
        if (ch.zFloorMm > maxFloorZMm) maxFloorZMm = ch.zFloorMm;
    }

    // Flood level: 40% dry, else a level a bit above the highest room floor
    // (typically child 0's, the shallowest room -- so a wet site reads as a
    // flooded shaft, submerged from the deepest room up to just under the
    // entrance), clamped below the anchor (air above the lake) and above
    // zero (the implicit ocean owns z<0; the min-surface gate keeps sites
    // inland of it anyway, this is just a defensive floor).
    const uint64_t floodHash = hash2(seed, fi, fj, CH_CAVERN_FLOOD);
    if (static_cast<uint32_t>(floodHash >> 32) >= kCavernFloodDryThreshold32) {
        const int64_t floodMm =
            maxFloorZMm + kCavernFloodMinMm + cavernHashField10(floodHash, 0, kCavernFloodSpanMm);
        site.floodZMm = static_cast<int32_t>(clampi64(floodMm, 1, site.anchorZMm - 1));
    }

    site.valid = true;
    return site;
}

// --- per-column reduction (design doc §3.3) ---------------------------------

struct CavernSeg {
    int32_t marginSq = 0;  // rz^2 * (roughened rxy^2 - xyDist^2) / roughened rxy^2, > 0
    int32_t zCenterMm = 0; // ABSOLUTE z of the room center
    int32_t zFloorMm = 0;  // ABSOLUTE floor clamp
};

struct CavernColumn {
    int32_t count = 0;
    CavernSeg segs[kMaxCavernSegs] = {};
    int32_t floodZMm = INT32_MIN; // INT32_MIN = dry (or no site in reach)
};

// ---------------------------------------------------------------------------
// CANDIDATE CORNERS (performance only — provably cannot change any output)
// ---------------------------------------------------------------------------
// The gate + depth-safety pass over the 2x2 corner block is a function of the
// column's COARSE CELL (si, sj) alone, never of where inside the 204.8 m cell
// the column sits — that cell is 2048x2048 = 4.2 MILLION voxel columns. So the
// four hashes are split out here exactly the way caves.h splits out its
// lattice block, and amplifier.cpp memoises them per (seed, si, sj). The fused
// `cavernColumnFor` below is unchanged in value and stays the contract/HLSL-
// mirror form.
//
// This also fuses what used to be two hashes per corner into one:
// `cavernSiteGateOpen` computes hash2(seed, fi, fj, CH_CAVE_NODE) for the gate
// bits and `caveNode` then recomputed the very same hash for the jitter. One
// call now feeds both, via caves.h's `caveNodeFromHash`. Same bits, same node.
struct CavernCandidate {
    bool open = false; // gate open AND node depth inside the child-0 safety window
    int64_t fi = 0, fj = 0;
    CaveNode node;
};

struct CavernCandidates {
    // Index dj * 2 + di — the (dj, di) iteration order that is part of the
    // worldgen contract.
    CavernCandidate corners[4] = {};
};

constexpr CavernCandidates cavernCandidatesFor(uint64_t seed, int64_t si, int64_t sj) {
    CavernCandidates out;
    for (int32_t dj = 0; dj < 2; ++dj)
        for (int32_t di = 0; di < 2; ++di) {
            const int64_t fi = (si + di) * kCavernCoarseLatticeRatio;
            const int64_t fj = (sj + dj) * kCavernCoarseLatticeRatio;
            CavernCandidate& c = out.corners[dj * 2 + di];
            c.fi = fi;
            c.fj = fj;

            // Cheapest reject first: the gate bit folded into the node-jitter
            // hash (see kCavernSiteGateMask), then the node it also encodes.
            const uint64_t h = hash2(seed, fi, fj, CH_CAVE_NODE);
            if (((h >> 60) & kCavernSiteGateMask) != 0) continue;
            c.node = caveNodeFromHash(h, fi, fj);
            if (!cavernDepthIsSafe(c.node.depthMm)) continue;
            c.open = true;
        }
    return out;
}

// Every cavern room within reach of column (vx, vy), reduced to the two
// int32s the per-voxel test needs plus the flat-floor clamp. `surfaceMm` is
// the QUERYING column's own terrain height (the ordinary ocean/beach guard,
// caves.h's kCaveMinSurfaceMm — deliberately the more permissive threshold;
// `kCavernMinSurfaceMm` is stricter and applies only to the SITE's own
// surface inside `cavernSiteFor`). `surfaceAt` is threaded through to
// `cavernSiteFor` — see its comment for the contract.
//
// Iteration order (dj, di, then room index) is part of the worldgen
// contract, mirrored bit-exactly in the eventual HLSL port.
//
// `siteFor(fi, fj, node) -> CavernSite` is how the site geometry is obtained.
// It is a parameter rather than a direct `cavernSiteFor` call because a site
// is a per-SITE value, not a per-column one, and the original comment here —
// "at most one corner per column ever reaches this, so this never repeats
// work" — was only true WITHIN a column. Across columns it repeats for every
// one of the ~400'000 columns inside the site's ~36 m reach disc, and each
// repeat re-runs `surfaceAt`, which in production is the amplifier's full
// bilinear-tile-base + detail-octave surface function. Measured on
// vxc_bench --radius 32, that alone roughly DOUBLED the whole amplify stage.
// Hoisting it behind a callable lets amplifier.cpp memoise per (seed, fi, fj)
// while the pure form below stays exactly what the contract and the HLSL
// mirror are written against.
template <typename SiteFn>
constexpr CavernColumn cavernColumnFromSites(uint64_t seed, const CavernCandidates& cands,
                                              int64_t vx, int64_t vy, const SiteFn& siteFor) {
    CavernColumn out;
    const int64_t xMm = vx * kVoxelSizeMm;
    const int64_t yMm = vy * kVoxelSizeMm;

    for (int32_t dj = 0; dj < 2; ++dj) {
        for (int32_t di = 0; di < 2; ++di) {
            const CavernCandidate& cand = cands.corners[dj * 2 + di];
            if (!cand.open) continue;
            const int64_t fi = cand.fi;
            const int64_t fj = cand.fj;
            const CaveNode& node = cand.node;

            // Every room in the chain shares this xy (see "why coaxial"), so
            // this distance is computed ONCE per column, not once per room.
            const int64_t ex = xMm - node.xMm;
            const int64_t ey = yMm - node.yMm;
            const int64_t dxySq = ex * ex + ey * ey;
            if (dxySq > kCavernMaxReachSqMm) continue;

            // Full reduction: the one place the terrain raster is read. At
            // most one corner per column ever reaches here (static_assert
            // above), so no column does this twice.
            const CavernSite& site = siteFor(fi, fj, node);
            if (!site.valid) continue;

            // Wall roughness: one 2D value-noise sample for this column,
            // shared by every room of this site.
            const int64_t roughQ10 = clampi64(
                1024 + valueNoise2(seed, xMm, yMm, kCavernRoughLatticeMm, CH_CAVERN_ROUGH) *
                           kCavernRoughAmpQ10 / 32768,
                kCavernRoughMinQ10, kCavernRoughMaxQ10);

            for (int32_t c = 0; c < kCavernChildCount; ++c) {
                const CavernChild& ch = site.children[c];
                const int64_t rxySq = ch.rxyMm * ch.rxyMm;
                const int64_t rxySqRough = rxySq * roughQ10 / 1024;
                if (dxySq >= rxySqRough) continue; // this room doesn't reach here
                const int64_t marginSq = ch.rzMm * ch.rzMm * (rxySqRough - dxySq) / rxySqRough;
                if (marginSq <= 0) continue;
                if (out.count < kMaxCavernSegs) {
                    out.segs[out.count].marginSq = static_cast<int32_t>(marginSq);
                    out.segs[out.count].zCenterMm = static_cast<int32_t>(ch.zMm);
                    out.segs[out.count].zFloorMm = static_cast<int32_t>(ch.zFloorMm);
                    ++out.count;
                }
            }
            out.floodZMm = site.floodZMm;
        }
    }
    return out;
}

// As above, with sites computed straight from `surfaceAt` (no memo).
template <typename SurfaceFn>
constexpr CavernColumn cavernColumnFromCandidates(uint64_t seed, const CavernCandidates& cands,
                                                   int64_t vx, int64_t vy,
                                                   const SurfaceFn& surfaceAt) {
    return cavernColumnFromSites(
        seed, cands, vx, vy, [&](int64_t fi, int64_t fj, const CaveNode& node) {
            return cavernSiteFor(seed, fi, fj, node, surfaceAt);
        });
}

// Fused form: the pure, self-contained `f(seed, vx, vy, surfaceMm, surfaceAt)`
// the worldgen contract and the HLSL mirror are written against. Callers
// walking many columns should go through amplifier.cpp's memoised path
// instead, which produces bit-identical values.
template <typename SurfaceFn>
constexpr CavernColumn cavernColumnFor(uint64_t seed, int64_t vx, int64_t vy, int32_t surfaceMm,
                                        const SurfaceFn& surfaceAt) {
    CavernColumn out;
    if (surfaceMm < kCaveMinSurfaceMm) return out;
    const int64_t si = floorDiv(vx * kVoxelSizeMm, kCavernCoarseMm);
    const int64_t sj = floorDiv(vy * kVoxelSizeMm, kCavernCoarseMm);
    return cavernColumnFromCandidates(seed, cavernCandidatesFor(seed, si, sj), vx, vy, surfaceAt);
}

// --- per-voxel carve test ---------------------------------------------------

// True if voxel (.., vz) of this column should become MAT_AIR. Same guard
// order/shape as caves.h's caveCarveAt, and — per the header comment above —
// these clamps are load-bearing here, not just backstops: `surfaceMm` /
// `bedrockDepthMm` are the QUERYING column's own values, and `bedrockDepthMm`
// is a runtime parameter with no compile-time assumption baked in anywhere
// in this file (see the post-design-change note #1 at the top).
constexpr bool cavernCarveAt(const CavernColumn& col, int32_t surfaceMm, int32_t bedrockDepthMm,
                              int64_t vz) {
    if (col.count == 0) return false;
    if (vz < kCaveMinVoxelZ) return false;
    if (surfaceMm < kCaveMinSurfaceMm) return false;

    const int64_t zAbs = vz * kVoxelSizeMm + kVoxelSizeMm / 2;
    const int64_t depthMm = static_cast<int64_t>(surfaceMm) - zAbs;
    if (depthMm < 0) return false; // above ground
    if (depthMm < kCaveRoofMinMm) return false;
    if (depthMm + kCaveBedrockMarginMm >= static_cast<int64_t>(bedrockDepthMm)) return false;

    for (int32_t s = 0; s < col.count; ++s) {
        const CavernSeg& sg = col.segs[s];
        if (zAbs < static_cast<int64_t>(sg.zFloorMm)) continue; // flat floor clamp
        const int64_t dz = zAbs - static_cast<int64_t>(sg.zCenterMm);
        if (dz * dz < static_cast<int64_t>(sg.marginSq)) return true;
    }
    return false;
}

// --- underground water (design doc §5.1, approved as designed) -------------

// Half of the "implicit static water" predicate: true if voxel (.., vz) sits
// below this column's flood level. The other half — that the voxel is cave
// air, i.e. `Amplifier::materialAt(col, vz) == MAT_AIR` — is the caller's
// job, since that needs the full ColumnSample this header doesn't have.
// Callers: `cavernFloodedAt(col.cavern, vz) && materialAt(col, vz) == MAT_AIR`.
constexpr bool cavernFloodedAt(const CavernColumn& col, int64_t vz) {
    if (col.floodZMm == INT32_MIN) return false;
    const int64_t zAbs = vz * kVoxelSizeMm + kVoxelSizeMm / 2;
    return zAbs < static_cast<int64_t>(col.floodZMm);
}

} // namespace vxc
