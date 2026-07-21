#pragma once
// Cavern pass v1 (M4 cave pass v2, docs/cavern-design.md) — hash-gated
// ellipsoid-cluster caverns anchored on the existing tunnel lattice's
// backbone-crossing nodes, plus their per-site flood level.
//
// Header-only, integer-only (CI float-ban). This is C1 of the design's build
// plan (docs/cavern-design.md §7): new file, no other file touched, standalone
// and testable. C4 wires `cavernColumnFor`'s output into `ColumnSample` and
// `Amplifier::materialAt`, exactly the way `voxelcore/caves.h`'s `CaveColumn`
// is wired in today. Every expression here must eventually be mirrored
// bit-for-bit in `voxel-core/shaders/worldgen.hlsl` (C6) — same discipline as
// caves.h: integer math only, `floorDiv`/power-of-two masks instead of signed
// `%`, no unbounded allocation.
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
// Cavern sites live at the SAME class of lattice node caves.h's sinkhole
// shafts use: `(i & 3) == 0 && (j & 3) == 0` on the cave lattice, i.e. every
// 4th node in both axes, 102.4 m apart (`kCavernCoarseMm`). Every such node is
// a BACKBONE crossing — caves.h's backbone rule makes every 4th row's +x
// edges and every 4th column's +y edges unconditional, so a backbone-crossing
// node always has (interior-of-world) four incident tunnels — and the anchor
// point IS that exact node (same `caveNode()` call, same `CH_CAVE_NODE`
// channel, so bit-identical to the tunnel network's own node), at absolute
// height
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
// and future water tables are level. This is the one place the cavern pass
// needs a surface height at an xy other than the column being queried, hence
// the `surfaceAt` callback threaded through `cavernSiteFor`/`cavernColumnFor`
// below: the caller (Amplifier::column in C4) already owns the tile raster
// and can supply its own bilinear+octave surface function (or, for a
// standalone caller, anything satisfying the same contract — see the
// function comments). Callers must not widen a GPU column-cache struct to
// carry this; C6 recomputes it inside VoxelizeMain instead (design doc §3.5).
//
// ---------------------------------------------------------------------------
// WHY THE COMMON-CASE COLUMN IS CHEAP (design doc §3.4, §3.7)
// ---------------------------------------------------------------------------
// A site's maximum reach from its anchor (worst-case child offset + widest
// roughened radius, `kCavernMaxReachMm`) is safely under half the 102.4 m
// coarse spacing, so:
//   * the 2x2 block of coarse-lattice corners around the column's own coarse
//     cell is EXHAUSTIVE (nothing further away can possibly reach it) —
//     `static_assert` below;
//   * at most ONE of those 4 corners can ever be an open, in-reach site for
//     any given column (two adjacent corners' reach discs cannot both cover
//     the same point) — `static_assert` below, which is what lets
//     `cavernColumnFor` just accumulate into one `CavernColumn` with no
//     "which site wins" tie-break to define.
// Per corner, the FIRST thing tested is a single gate bit folded into the
// hash `caveNode()` already computes for node-jitter (`CH_CAVE_NODE`) — see
// `cavernSiteGateOpen` — so the common "gate closed" case (~68% of corners,
// theory (3/4)^1 per corner) costs nothing beyond a hash call the tunnel
// system's own node lookup would otherwise also need. Only once a corner
// passes gate + a cheap depth-safety window + a cheap xy-distance reject does
// `cavernSiteFor` run — the ONE place that reads the terrain raster
// (`surfaceAt`), decodes the K=4 child ellipsoids and the flood level. That
// full reduction is measured at ~4% of columns (docs/cavern-design.md §3.7).
// Columns with no site in reach get `CavernColumn{count=0, floodZMm=dry}` and
// the per-voxel test is one `count == 0` check.
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
// surprise skylight or a bedrock breach; the measured flood-fill in
// test_caverns.cpp is the arbiter of how much that costs connectivity, the
// same way it already is for tunnel pinching at cliffs.
//
// Separately, the SITE is only accepted if its own node depth falls inside a
// SAFE WINDOW (`kCavernNodeDepthSafeMinMm`..`kCavernNodeDepthSafeMaxMm`) that
// guarantees the full ellipsoid geometry (including the widest possible
// child offset+radius) cannot reach the roof or bedrock clamps even before
// the per-voxel guards are consulted — "sizing at the anchor column is
// structural" per the design doc, `static_assert`-pinned below.

#include "voxelcore/caves.h"

namespace vxc {

// --- worldgen contract constants (tune only on a kWorldGenVersion bump) -----

// Cavern sites sit on the same lattice class as sinkhole shafts (every 4th
// cave-lattice node in both axes), 102.4 m apart.
inline constexpr int64_t kCavernCoarseMm = kCaveLatticeMm * 4;

// 1-in-4 site gate, decoded from bits the node-jitter hash otherwise leaves
// unused (caveNode() consumes bits 0..59 of hash2(seed,i,j,CH_CAVE_NODE) for
// x/y jitter + depth; bits 60/61 are free). This is the "fold gate bits into
// the node-jitter hash" optimization the design doc calls out (§3.7):
// testing it costs nothing beyond the hash caveNode() already computes, and
// removes what would otherwise be a second hash call in the ~63%-of-columns
// "gate open, later rejected on xy distance" tier.
inline constexpr uint64_t kCavernSiteGateMask = 3; // bits 60..61 == 0 -> open

// Beach/ocean guard on the SITE's own surface (not the querying column's) —
// stricter than caves.h's kCaveMinSurfaceMm because caverns are taller and a
// half-drowned cliffside room reads far worse than a half-drowned tunnel.
inline constexpr int32_t kCavernMinSurfaceMm = 20000;

// --- child ellipsoid geometry (design doc §3.2) -----------------------------

inline constexpr int32_t kCavernChildCount = 4;

// Children 1..3 offset from the anchor by up to this many mm on each axis
// independently (child 0 sits exactly on the anchor). ±8 m per the design.
inline constexpr int64_t kCavernChildOffsetXYMaxMm = 8000;
inline constexpr int64_t kCavernChildOffsetZMaxMm = 3000; // ±3 m

// Horizontal (rx == ry — an oblate spheroid, i.e. round in plan, flattened in
// z, matching "rooms not spheres") and vertical semi-axes, each independently
// hashed per child.
inline constexpr int64_t kCavernRxyMinMm = 6000;  // 6 m
inline constexpr int64_t kCavernRxySpanMm = 8000; // -> [6, 14) m
inline constexpr int64_t kCavernRxyMaxMm = kCavernRxyMinMm + kCavernRxySpanMm;
inline constexpr int64_t kCavernRzMinMm = 2500;   // 2.5 m
inline constexpr int64_t kCavernRzSpanMm = 3500;  // -> [2.5, 6) m
inline constexpr int64_t kCavernRzMaxMm = kCavernRzMinMm + kCavernRzSpanMm;

// Flat floor: each child's floor sits this far below the SITE's anchorZ (not
// the child's own center — every child shares the anchor as its floor
// reference, design doc §3.2), independently hashed per child.
inline constexpr int64_t kCavernFloorDropMinMm = 1000; // 1 m
inline constexpr int64_t kCavernFloorDropSpanMm = 3000; // -> [1, 4) m

// --- wall roughness (design doc §3.2) ---------------------------------------

// Per-column 2D value noise multiplies the reduced xy reach (rxy²) before the
// margin division, so walls/ceilings undulate at zero per-voxel cost — one
// sample shared by every child in the column, exactly like a tunnel radius is
// constant along an edge but the tube still looks organic because the whole
// lattice is jittered.
inline constexpr int64_t kCavernRoughLatticeMm = 3200; // 3.2 m
inline constexpr int64_t kCavernRoughAmpQ10 = 307;     // +/-0.3 of 1024
inline constexpr int64_t kCavernRoughMinQ10 = 1024 - kCavernRoughAmpQ10; // 717 (~0.70)
inline constexpr int64_t kCavernRoughMaxQ10 = 1024 + kCavernRoughAmpQ10; // 1331 (~1.30)

// --- site depth safety window (design doc §3.6) -----------------------------

// Worst-case vertical reach of any child ellipsoid from the anchor, in either
// direction: offset (up to +/-3 m) plus the widest possible radius (6 m).
inline constexpr int64_t kCavernMaxVerticalReachMm =
    kCavernChildOffsetZMaxMm + kCavernRzMaxMm; // 9000
// Roof/bedrock margins the SITE itself must clear before any per-voxel clamp
// is even consulted (2 m of extra headroom past the ordinary clamps, design
// doc §3.6).
inline constexpr int64_t kCavernRoofSafetyMarginMm = 2000;
inline constexpr int64_t kCavernBedrockSafetyMarginMm = 2000;
inline constexpr int64_t kCavernBedrockCeilingMm = 38000; // shallowest bedrock the amplifier can produce, minus nothing yet
// Only node depths in this window can host a full-size cavern without the
// geometry alone (before any per-voxel clamp) violating roof or bedrock.
inline constexpr int64_t kCavernNodeDepthSafeMinMm =
    kCaveRoofMinMm + kCavernRoofSafetyMarginMm + kCavernMaxVerticalReachMm; // 17000
inline constexpr int64_t kCavernNodeDepthSafeMaxMm =
    kCavernBedrockCeilingMm - kCavernBedrockSafetyMarginMm - kCavernMaxVerticalReachMm; // 27000

// --- candidate reach (design doc §3.4) --------------------------------------

// Safe (non-tight) bound on the worst-case 2D offset magnitude of a child
// from the anchor. The true worst case is 8000*sqrt(2) =~ 11314 mm (both
// axes hashed independently to +/-8000); 12000 mm is a round, easily-verified
// upper bound (12000^2 >= 2*8000^2) chosen to avoid a sqrt in constexpr
// integer code — being conservative here only widens the candidate net, it
// never risks missing a real site.
inline constexpr int64_t kCavernMaxOffsetMagnitudeMm = 12000;
// Safe bound on the widest a roughened child radius can reach: rxyMax scaled
// by the roughness ceiling (linear, not the tighter sqrt(roughness) the
// marginSq formula implies — again conservative on purpose).
inline constexpr int64_t kCavernMaxReachMm =
    kCavernMaxOffsetMagnitudeMm + (kCavernRxyMaxMm * kCavernRoughMaxQ10) / 1024;
inline constexpr int64_t kCavernMaxReachSqMm = kCavernMaxReachMm * kCavernMaxReachMm;

// --- flood level (design doc §5.1) ------------------------------------------

inline constexpr int64_t kCavernFloodMinMm = 800;  // 0.8 m above the highest child floor
inline constexpr int64_t kCavernFloodSpanMm = 2400; // -> [0.8, 3.2) m
// 40% of open sites are dry, rounded down from 0.4 * 2^32 in pure integer
// math (no floating literal, per the float-ban): a uniform top-32-bit hash
// value below this threshold means dry.
inline constexpr uint32_t kCavernFloodDryThreshold32 = static_cast<uint32_t>((4ull << 32) / 10);

// Max cavern segments recorded per column. At most one site can be in reach
// of any column (static_assert below), contributing at most kCavernChildCount
// (4) segments; 6 leaves headroom exactly like caves.h's kMaxCaveSegs does
// for tunnels, without changing the underlying "at most one site" argument.
inline constexpr int32_t kMaxCavernSegs = 6;

// --- structural invariants, checked at compile time -------------------------

// Every child overlaps child 0 (which sits on the anchor with radius >= the
// minimum), so the K-ellipsoid union is one connected blob. Compared as
// squares so no sqrt is needed: worst-case offset magnitude^2 vs (rxyMin +
// rxyMin)^2.
static_assert(2 * kCavernChildOffsetXYMaxMm * kCavernChildOffsetXYMaxMm <
                  4 * kCavernRxyMinMm * kCavernRxyMinMm,
              "every non-root child must overlap child 0, or the K-ellipsoid "
              "cluster is not guaranteed connected");

// The depth safety window must be non-empty and must sit inside the range
// caveNode() can actually produce (kCaveNodeDepthMinMm ..
// +kCaveNodeDepthSpanMm) -- otherwise no site could ever pass it, or the
// "safe" window could silently extend outside what caveNode() ever returns.
static_assert(kCavernNodeDepthSafeMinMm < kCavernNodeDepthSafeMaxMm,
              "cavern depth safety window must be non-empty");
static_assert(kCavernNodeDepthSafeMinMm >= kCaveNodeDepthMinMm,
              "cavern depth safety window must sit inside caveNode()'s range");
static_assert(kCavernNodeDepthSafeMaxMm <= kCaveNodeDepthMinMm + kCaveNodeDepthSpanMm,
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
inline constexpr uint32_t CH_CAVERN_SITE = 22;  // per-child geometry (offset/radii/floor)
inline constexpr uint32_t CH_CAVERN_ROUGH = 23; // per-column wall roughness
inline constexpr uint32_t CH_CAVERN_FLOOD = 25; // per-site flood level

// --- small helpers -----------------------------------------------------------

// Unsigned 10-bit field -> [0, spanMm), multiply-then-shift (never a
// division), matching caveNode()'s 20-bit fields but narrower since a site
// needs six independent fields out of one 64-bit hash.
constexpr int64_t cavernHashField10(uint64_t h, int32_t shift, int64_t spanMm) {
    return static_cast<int64_t>((((h >> shift) & 0x3FFu) * static_cast<uint64_t>(spanMm)) >> 10);
}

// --- site gate + candidacy ---------------------------------------------------

// True if the backbone-crossing node (fi, fj) is a cavern site candidate
// (folded into the node-jitter hash — see the constant comment above). Caller
// must already know (fi & 3) == 0 && (fj & 3) == 0; every caller here
// constructs fi/fj that way by construction, so no runtime mask check.
constexpr bool cavernSiteGateOpen(uint64_t seed, int64_t fi, int64_t fj) {
    return ((hash2(seed, fi, fj, CH_CAVE_NODE) >> 60) & kCavernSiteGateMask) == 0;
}

// True if a node at this depth can host a full-size cavern without the
// geometry alone (before any per-voxel clamp) violating roof or bedrock.
constexpr bool cavernDepthIsSafe(int64_t nodeDepthMm) {
    return nodeDepthMm >= kCavernNodeDepthSafeMinMm && nodeDepthMm <= kCavernNodeDepthSafeMaxMm;
}

// --- per-site geometry (the ~4% "full reduction" tier) ----------------------

struct CavernChild {
    int64_t xMm = 0, yMm = 0, zMm = 0; // absolute center
    int64_t rxyMm = 0, rzMm = 0;       // semi-axes (rx == ry: oblate spheroid)
    int64_t zFloorMm = 0;             // absolute flat-floor clamp
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
    for (int32_t c = 0; c < kCavernChildCount; ++c) {
        const uint64_t h = hash3(seed, fi, fj, c, CH_CAVERN_SITE);
        const bool isRoot = (c == 0);
        const int64_t offXMm =
            isRoot ? 0
                   : cavernHashField10(h, 0, 2 * kCavernChildOffsetXYMaxMm) - kCavernChildOffsetXYMaxMm;
        const int64_t offYMm =
            isRoot ? 0
                   : cavernHashField10(h, 10, 2 * kCavernChildOffsetXYMaxMm) - kCavernChildOffsetXYMaxMm;
        const int64_t offZMm =
            isRoot ? 0
                   : cavernHashField10(h, 20, 2 * kCavernChildOffsetZMaxMm) - kCavernChildOffsetZMaxMm;
        const int64_t rxyMm = kCavernRxyMinMm + cavernHashField10(h, 30, kCavernRxySpanMm);
        const int64_t rzMm = kCavernRzMinMm + cavernHashField10(h, 40, kCavernRzSpanMm);
        const int64_t floorDropMm =
            kCavernFloorDropMinMm + cavernHashField10(h, 50, kCavernFloorDropSpanMm);

        CavernChild& ch = site.children[c];
        ch.xMm = site.anchorXMm + offXMm;
        ch.yMm = site.anchorYMm + offYMm;
        ch.zMm = site.anchorZMm + offZMm;
        ch.rxyMm = rxyMm;
        ch.rzMm = rzMm;
        ch.zFloorMm = site.anchorZMm - floorDropMm;
        if (ch.zFloorMm > maxFloorZMm) maxFloorZMm = ch.zFloorMm;
    }

    // Flood level: 40% dry, else a level a bit above the highest child floor,
    // clamped below the anchor (air above the lake) and above zero (the
    // implicit ocean owns z<0; the min-surface gate keeps sites inland of it
    // anyway, this is just a defensive floor).
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
    int32_t zCenterMm = 0; // ABSOLUTE z of the child center
    int32_t zFloorMm = 0;  // ABSOLUTE floor clamp
};

struct CavernColumn {
    int32_t count = 0;
    CavernSeg segs[kMaxCavernSegs] = {};
    int32_t floodZMm = INT32_MIN; // INT32_MIN = dry (or no site in reach)
};

// Every cavern child within reach of column (vx, vy), reduced to the two
// int32s the per-voxel test needs plus the flat-floor clamp. `surfaceMm` is
// the QUERYING column's own terrain height (the ordinary ocean/beach guard,
// caves.h's kCaveMinSurfaceMm — deliberately the more permissive threshold;
// `kCavernMinSurfaceMm` is stricter and applies only to the SITE's own
// surface inside `cavernSiteFor`). `surfaceAt` is threaded through to
// `cavernSiteFor` — see its comment for the contract.
//
// Iteration order (dj, di, then child index) is part of the worldgen
// contract, mirrored bit-exactly in the eventual HLSL port.
template <typename SurfaceFn>
constexpr CavernColumn cavernColumnFor(uint64_t seed, int64_t vx, int64_t vy, int32_t surfaceMm,
                                        const SurfaceFn& surfaceAt) {
    CavernColumn out;
    if (surfaceMm < kCaveMinSurfaceMm) return out;

    const int64_t xMm = vx * kVoxelSizeMm;
    const int64_t yMm = vy * kVoxelSizeMm;
    const int64_t si = floorDiv(xMm, kCavernCoarseMm);
    const int64_t sj = floorDiv(yMm, kCavernCoarseMm);

    for (int32_t dj = 0; dj < 2; ++dj) {
        for (int32_t di = 0; di < 2; ++di) {
            const int64_t fi = (si + di) * 4;
            const int64_t fj = (sj + dj) * 4;

            // Cheapest reject first: the gate bit folded into the node-
            // jitter hash (no separate hash call, see kCavernSiteGateMask).
            if (!cavernSiteGateOpen(seed, fi, fj)) continue;

            const CaveNode node = caveNode(seed, fi, fj);
            if (!cavernDepthIsSafe(node.depthMm)) continue;

            const int64_t ex = xMm - node.xMm;
            const int64_t ey = yMm - node.yMm;
            if (ex * ex + ey * ey > kCavernMaxReachSqMm) continue;

            // Full reduction: the one place surfaceAt() (a terrain raster
            // read) is called. At most one corner per column ever reaches
            // here (static_assert above), so this never repeats work.
            const CavernSite site = cavernSiteFor(seed, fi, fj, node, surfaceAt);
            if (!site.valid) continue;

            // Wall roughness: one 2D value-noise sample for this column,
            // shared by every child of this site.
            const int64_t roughQ10 = clampi64(
                1024 + valueNoise2(seed, xMm, yMm, kCavernRoughLatticeMm, CH_CAVERN_ROUGH) *
                           kCavernRoughAmpQ10 / 32768,
                kCavernRoughMinQ10, kCavernRoughMaxQ10);

            for (int32_t c = 0; c < kCavernChildCount; ++c) {
                const CavernChild& ch = site.children[c];
                const int64_t cex = xMm - ch.xMm;
                const int64_t cey = yMm - ch.yMm;
                const int64_t dxySq = cex * cex + cey * cey;
                const int64_t rxySq = ch.rxyMm * ch.rxyMm;
                const int64_t rxySqRough = rxySq * roughQ10 / 1024;
                if (dxySq >= rxySqRough) continue; // this child doesn't reach here
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

// --- per-voxel carve test ---------------------------------------------------

// True if voxel (.., vz) of this column should become MAT_AIR. Same guard
// order/shape as caves.h's caveCarveAt, and — per the header comment above —
// these clamps are load-bearing here, not just backstops: `surfaceMm` /
// `bedrockDepthMm` are the QUERYING column's own values.
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

// --- underground water (design doc §5.1) ------------------------------------

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
