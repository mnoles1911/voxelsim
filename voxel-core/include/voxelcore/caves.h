#pragma once
// Cave pass v1 (M4, plan §4 "cave pass") — deterministic integer carving of a
// CONNECTED tunnel network into the amplifier's stratigraphy.
//
// Header-only, integer-only (CI float-ban). Every expression here is mirrored
// bit-for-bit in voxel-core/shaders/worldgen.hlsl's `caveColumnFor` /
// `caveCarveAt` (docs/determinism.md CPU/GPU mirror contract). ANY change here
// must be made identically in both places, re-verified with vxc_gpu on real
// hardware, and paired with a kWorldGenVersion bump.
//
// ---------------------------------------------------------------------------
// WHY A LATTICE-GRAPH FORMULATION AND NOT 3D NOISE
// ---------------------------------------------------------------------------
// The obvious "carve where |noise3| < t" gives blobby, mostly-DISCONNECTED
// bubbles: pretty in a cross-section, useless for gameplay (M6 NPCs path
// through cave air, M5 digging opens into it, water drains into it — all of
// which want passages that go somewhere). Connectivity of a noise sublevel set
// is an emergent property you can only measure after the fact, and it is
// fragile under any threshold retune.
//
// So the network's connectivity is made STRUCTURAL instead of emergent. Caves
// are the union of capsules (round tubes) laid along the edges of a jittered
// 2D lattice graph:
//
//   * Nodes sit on a kCaveLatticeMm grid in x/y, each pushed to a hash-jittered
//     position anywhere inside its own cell, so the grid is invisible in the
//     result.
//   * Every node also carries a hash-jittered DEPTH BELOW THE LOCAL TERRAIN
//     SURFACE (not an absolute z — see "depth space" below).
//   * Edges run +x and +y between adjacent nodes. An edge exists if it is a
//     BACKBONE edge — every 4th row's +x edges, every 4th column's +y edges —
//     or if a hash gate (1 in 4) opens it.
//
// The backbone alone is provably connected: the +x backbone rows (j & 3) == 0
// are each fully connected along x, the +y backbone columns (i & 3) == 0 are
// each fully connected along y, and every backbone row meets every backbone
// column at the node where they cross. So the carved set is connected by
// construction over the whole plane, for every seed, with no thresholds to
// tune. The hash-gated extra edges only ADD passages (loops, side branches and
// dead ends off the backbone) — they can never disconnect anything. Nodes with
// no incident edge simply carry no tunnel.
//
// ---------------------------------------------------------------------------
// DEPTH SPACE (why tunnels follow the terrain)
// ---------------------------------------------------------------------------
// Node depth is measured DOWN FROM THE COLUMN'S OWN SURFACE, so the whole
// network drapes under the topography instead of living in an absolute z band.
// Three things fall out of that, and they are exactly the "respect the world's
// rules" requirements:
//
//   1. Roof thickness is guaranteed globally, not hoped for. The shallowest a
//      tunnel AXIS can run is kCaveNodeDepthMinMm and the fattest tube radius
//      is kCaveRadiusMinMm + kCaveRadiusSpanMm, so no TUNNEL voxel is ever
//      shallower than 6.2 m below its own surface; static_assert below pins
//      that against kCaveRoofMinMm. The single deliberate exception is the
//      sinkhole shaft (see kCaveShaftNodeMask) — a sparse, explicitly chosen
//      set of entrance holes, not a leaky roof.
//   2. The network stays out of the bedrock floor: deepest possible carve is
//      36.8 m, while bedrockDepthMm is >= 40 m everywhere (amplifier.cpp), and
//      caveCarveAt additionally refuses anything within kCaveBedrockMarginMm
//      of the column's own bedrock top AND Amplifier::materialAt refuses to
//      turn MAT_BEDROCK into air at all. Three independent guards.
//   3. Cave MOUTHS still happen, for free, exactly where you want them.
//      "6 m below the surface directly overhead" is only 6 m of rock on FLAT
//      ground; on a steep hillside the nearest free face is sideways, and the
//      tube daylights on the slope. No special entrance rule, no perforated
//      meadows.
//
// The cost of depth space is that the carved set is only connected up to the
// per-column shift z = surface - depth: a cliff that drops more than a tube
// diameter between two ADJACENT columns can pinch a tunnel in two. That is
// rare, and it is measured rather than assumed — see test_caves.cpp's
// flood-fill, which reports component count and the largest component's share
// of total cave volume.
//
// ---------------------------------------------------------------------------
// COLUMN/VOXEL SPLIT (and why this is cheap)
// ---------------------------------------------------------------------------
// A column's xy position determines, once, which tube axes pass near it and
// how far away they are in xy. Only the DEPTH comparison varies down the
// column. So caveColumnFor() does all the hashing and geometry once per column
// (34 hash2 calls, 4x4 nodes and 3x3x2 candidate edges) and reduces each
// nearby tube to two int32s:
//
//     marginSq  = r^2 - (xy distance from column to tube axis)^2   ( > 0 )
//     depthMm   = the tube axis's depth where it passes the column
//
// and then the per-voxel test is just `dz*dz < marginSq`. That is one multiply
// and one compare per candidate tube per voxel, which is why the cave pass can
// live inside ColumnSample and ride the existing column cache (UE
// VoxelWorldSubsystem, gpu_harness, GeneratedWorld::makeBrick) with no API
// change anywhere.
//
// The candidate set is EXACT, not approximate: a tube axis from node (i,j) is
// contained in the bounding box of cells (i,j)..(i+1,j+1), and the fattest
// radius is far below half a lattice cell (static_assert below), so only the
// 3x3 block of source nodes centred on the column's own cell can possibly
// reach it. Nothing is missed and nothing outside is consulted.

#include "voxelcore/core.h"
#include "voxelcore/hash.h"

namespace vxc {

// --- worldgen contract constants (tune only on a kWorldGenVersion bump) -----

// Node grid spacing in x/y. 25.6 m — small enough that tunnels are a routine
// feature of any underground traverse, large enough that a 2 m-ish tube is a
// passage and not a sponge.
inline constexpr int64_t kCaveLatticeMm = 25600;

// Tube axis depth below the column's own surface: [min, min+span).
inline constexpr int64_t kCaveNodeDepthMinMm = 9000;
inline constexpr int64_t kCaveNodeDepthSpanMm = 25000;

// Tube radius, constant along one edge (varies edge to edge): [min, min+span).
// 1.2 m .. 2.8 m radius = 2.4 m .. 5.6 m wide passages — walkable by an agent,
// diggable by a player, and wide enough that the greedy mesher makes cheap
// quads out of the walls.
inline constexpr int64_t kCaveRadiusMinMm = 1200;
inline constexpr int64_t kCaveRadiusSpanMm = 1600;
inline constexpr int64_t kCaveRadiusMaxMm = kCaveRadiusMinMm + kCaveRadiusSpanMm;

// Backbone selector. `(j & kCaveBackboneMask) == 0` keeps every 4th row's +x
// edges and every 4th column's +y edges — the provably-connected skeleton.
// A power-of-two mask on purpose: two's-complement AND is the floored modulo
// for negative indices too, so no signed `%` is needed anywhere (the exact
// construct tools/lint-shader-ub.py exists to keep out of the shader).
inline constexpr int64_t kCaveBackboneMask = 3;

// Non-backbone edges open when the top 2 bits of their hash are zero (1 in 4).
inline constexpr uint64_t kCaveEdgeGateMask = 3;

// --- sinkhole shafts (the entrances) ----------------------------------------
// Depth-space tunnels never break the surface on their own (that is the roof
// guarantee), which on gentle terrain would leave the whole network sealed and
// reachable only by digging. Real cave systems open through a sparse set of
// potholes/sinkholes, and so does this one: at a BACKBONE CROSSING node — one
// where (i & 3) == 0 AND (j & 3) == 0, hence guaranteed to have all four
// backbone tunnels incident on it — a 1-in-4 hash gate opens a vertical shaft
// from that node's depth straight up to the surface.
//
// Connectivity is again structural, not hoped for: the shaft's bottom IS the
// node point, which lies on the axis of every tunnel meeting there, so the
// shaft is part of the main component by construction.
//
// Density: one candidate node per 4x4 lattice cells (102.4 m square), gated to
// 1 in 4, so roughly one entrance per 205 m square, each a ~1.0-1.7 m radius
// hole. That is findable on a walk without turning the ground into a colander
// — test_caves.cpp measures the perforated fraction of the surface (well under
// 0.1%) rather than leaving it to judgement.
inline constexpr int64_t kCaveShaftNodeMask = 3;  // (i & mask) == 0 && (j & mask) == 0
inline constexpr uint64_t kCaveShaftGateMask = 3; // 1 in 4 of those open
inline constexpr int64_t kCaveShaftRadiusMinMm = 1000;
inline constexpr int64_t kCaveShaftRadiusSpanMm = 700;

// Hard safety clamps, all applied per voxel in caveCarveAt.
inline constexpr int64_t kCaveRoofMinMm = 6000;       // never carve shallower than this
inline constexpr int64_t kCaveBedrockMarginMm = 2000; // stop this far above bedrock top
inline constexpr int32_t kCaveMinSurfaceMm = 12000;   // columns below this get no caves at all
inline constexpr int64_t kCaveMinVoxelZ = 0;          // never carve at or below sea level

// Max tube axes recorded per column. Four edges meet at a node, so a column
// sitting exactly on a junction can legitimately see 4; 6 leaves headroom for
// an unrelated tube brushing past. The cap is a fixed-size-storage bound, not
// a design limit — test_caves.cpp CaveSegmentCapHeadroom measures the real
// maximum over a large sample and fails if it ever reaches the cap, so the
// (deterministic, fixed-iteration-order) truncation below is never reached in
// practice on either CPU or GPU.
inline constexpr int32_t kMaxCaveSegs = 8;

// --- structural invariants, checked at compile time -------------------------

// The 3x3 candidate block is only exhaustive while a tube cannot reach more
// than one lattice cell sideways off its axis' bounding box.
static_assert(kCaveRadiusMaxMm * 2 < kCaveLatticeMm,
              "cave tube diameter must stay well under one lattice cell, or the 3x3 "
              "candidate-node block in caveColumnFor stops being exhaustive");
// Roof: shallowest possible carved voxel is axis depth minus max radius.
static_assert(kCaveNodeDepthMinMm - kCaveRadiusMaxMm > kCaveRoofMinMm,
              "tube geometry must keep itself above the roof clamp — the clamp is a "
              "backstop, not the mechanism");
// Floor: deepest possible carved voxel vs the shallowest bedrock top the
// amplifier can produce (40 m, amplifier.cpp bedrockDepthMm).
static_assert(kCaveNodeDepthMinMm + kCaveNodeDepthSpanMm + kCaveRadiusMaxMm +
                      kCaveBedrockMarginMm < 40000,
              "tube geometry must keep itself out of bedrock — the bedrock margin is a "
              "backstop, not the mechanism");

// --- hash channels ----------------------------------------------------------
// Extends hash.h's HashChannel registry (18..20; 32.. is reserved for
// synthetic tiles). Declared here rather than in hash.h to keep the cave pass
// self-contained; APPEND ONLY, never renumber — renumbering is world-breaking.
inline constexpr uint32_t CH_CAVE_NODE = 18;   // node jitter (position + depth)
inline constexpr uint32_t CH_CAVE_EDGE = 19;   // non-backbone edge gate
inline constexpr uint32_t CH_CAVE_RADIUS = 20; // per-edge tube radius
inline constexpr uint32_t CH_CAVE_SHAFT = 21;  // sinkhole gate + shaft radius

// --- node / edge primitives -------------------------------------------------

struct CaveNode {
    int64_t xMm = 0;
    int64_t yMm = 0;
    int64_t depthMm = 0; // below the terrain surface, not an absolute z
};

// Jittered position + depth of lattice node (i, j). One hash2 supplies all
// three: 20 bits each for x jitter, y jitter and depth, scaled by
// multiply-then-shift (never a division — the shift is by a literal, so it is
// portable in HLSL too).
constexpr CaveNode caveNode(uint64_t seed, int64_t i, int64_t j) {
    const uint64_t h = hash2(seed, i, j, CH_CAVE_NODE);
    CaveNode n;
    n.xMm = i * kCaveLatticeMm +
            static_cast<int64_t>(((h & 0xFFFFFu) * static_cast<uint64_t>(kCaveLatticeMm)) >> 20);
    n.yMm = j * kCaveLatticeMm +
            static_cast<int64_t>((((h >> 20) & 0xFFFFFu) * static_cast<uint64_t>(kCaveLatticeMm)) >> 20);
    n.depthMm =
        kCaveNodeDepthMinMm +
        static_cast<int64_t>((((h >> 40) & 0xFFFFFu) * static_cast<uint64_t>(kCaveNodeDepthSpanMm)) >> 20);
    return n;
}

// dir 0 = the +x edge out of (i, j), dir 1 = the +y edge. Backbone edges are
// unconditional (that is what makes the network connected for every seed);
// everything else is a 1-in-4 hash gate.
constexpr bool caveEdgeExists(uint64_t seed, int64_t i, int64_t j, int32_t dir) {
    if (dir == 0 && (j & kCaveBackboneMask) == 0) return true;
    if (dir == 1 && (i & kCaveBackboneMask) == 0) return true;
    return ((hash2(seed, i, j * 2 + dir, CH_CAVE_EDGE) >> 48) & kCaveEdgeGateMask) == 0;
}

constexpr int64_t caveEdgeRadiusMm(uint64_t seed, int64_t i, int64_t j, int32_t dir) {
    const uint64_t h = hash2(seed, i, j * 2 + dir, CH_CAVE_RADIUS);
    return kCaveRadiusMinMm +
           static_cast<int64_t>((((h >> 44) & 0xFFFFFu) * static_cast<uint64_t>(kCaveRadiusSpanMm)) >> 20);
}

// --- per-column reduction ---------------------------------------------------

// One tube axis passing near this column, reduced to the only two numbers the
// per-voxel test needs (see the header comment's COLUMN/VOXEL SPLIT).
struct CaveSeg {
    int32_t marginSq = 0; // r^2 - xyDist^2, always > 0 for a recorded segment
    int32_t depthMm = 0;  // axis depth below surface at the column's xy
};

struct CaveColumn {
    int32_t count = 0;
    CaveSeg segs[kMaxCaveSegs] = {};
    // Sinkhole shaft over this column, if any (at most one can be in range —
    // shaft nodes are 102.4 m apart and a shaft is ~1.4 m wide). Zero
    // marginSq means "no shaft here", which is the overwhelmingly common case.
    int32_t shaftMarginSq = 0;   // r^2 - xyDist^2 to the shaft axis, > 0 if present
    int32_t shaftDepthMaxMm = 0; // shaft runs from the surface down to this depth
};

// Every tube axis within reach of column (vx, vy). `surfaceMm` is the column's
// own terrain height: columns at or below kCaveMinSurfaceMm are excluded
// outright, which is the ocean/beach guard (see caveCarveAt).
//
// Iteration order (dj, then di, then dir) is part of the worldgen contract: it
// is what decides which segments survive if the kMaxCaveSegs cap were ever
// hit, and the shader mirrors it exactly.
constexpr CaveColumn caveColumnFor(uint64_t seed, int64_t vx, int64_t vy, int32_t surfaceMm) {
    CaveColumn out;
    if (surfaceMm < kCaveMinSurfaceMm) return out;

    const int64_t xMm = vx * kVoxelSizeMm;
    const int64_t yMm = vy * kVoxelSizeMm;
    const int64_t ci = floorDiv(xMm, kCaveLatticeMm);
    const int64_t cj = floorDiv(yMm, kCaveLatticeMm);

    // 4x4 node block: the 3x3 candidate SOURCE nodes plus the +x/+y endpoints
    // they need.
    CaveNode nodes[16] = {};
    for (int32_t dj = 0; dj < 4; ++dj)
        for (int32_t di = 0; di < 4; ++di)
            nodes[di + 4 * dj] = caveNode(seed, ci - 1 + di, cj - 1 + dj);

    for (int32_t dj = 0; dj < 3; ++dj) {
        for (int32_t di = 0; di < 3; ++di) {
            const int64_t i = ci - 1 + di;
            const int64_t j = cj - 1 + dj;
            for (int32_t dir = 0; dir < 2; ++dir) {
                if (!caveEdgeExists(seed, i, j, dir)) continue;
                const CaveNode a = nodes[di + 4 * dj];
                const CaveNode b = (dir == 0) ? nodes[di + 1 + 4 * dj] : nodes[di + 4 * (dj + 1)];

                const int64_t dx = b.xMm - a.xMm;
                const int64_t dy = b.yMm - a.yMm;
                const int64_t den = dx * dx + dy * dy;
                if (den == 0) continue; // unreachable (one axis always spans a full cell)

                // Closest approach in xy, parameterised on the axis and clamped
                // to the segment. Rational t = num/den kept exact; the single
                // rounding is the floorDiv when the closest point is realised.
                const int64_t wx = xMm - a.xMm;
                const int64_t wy = yMm - a.yMm;
                const int64_t num = clampi64(wx * dx + wy * dy, 0, den);

                const int64_t cx = a.xMm + floorDiv(dx * num, den);
                const int64_t cy = a.yMm + floorDiv(dy * num, den);
                const int64_t cd = a.depthMm + floorDiv((b.depthMm - a.depthMm) * num, den);

                const int64_t ex = xMm - cx;
                const int64_t ey = yMm - cy;
                const int64_t r = caveEdgeRadiusMm(seed, i, j, dir);
                const int64_t marginSq = r * r - (ex * ex + ey * ey);
                if (marginSq <= 0) continue;

                if (out.count < kMaxCaveSegs) {
                    out.segs[out.count].marginSq = static_cast<int32_t>(marginSq);
                    out.segs[out.count].depthMm = static_cast<int32_t>(cd);
                    ++out.count;
                }
            }
        }
    }

    // Sinkhole shaft. Same 3x3 source-node block; at most one node in it can
    // be a backbone crossing (three consecutive indices contain at most one
    // multiple of 4 on each axis), so the first hit in the fixed (dj, di)
    // order is also the only hit.
    for (int32_t dj = 0; dj < 3 && out.shaftMarginSq == 0; ++dj) {
        for (int32_t di = 0; di < 3 && out.shaftMarginSq == 0; ++di) {
            const int64_t i = ci - 1 + di;
            const int64_t j = cj - 1 + dj;
            if ((i & kCaveShaftNodeMask) != 0 || (j & kCaveShaftNodeMask) != 0) continue;
            const uint64_t h = hash2(seed, i, j, CH_CAVE_SHAFT);
            if (((h >> 48) & kCaveShaftGateMask) != 0) continue;
            const CaveNode n = nodes[di + 4 * dj];
            const int64_t r =
                kCaveShaftRadiusMinMm +
                static_cast<int64_t>(((h & 0xFFFFFu) * static_cast<uint64_t>(kCaveShaftRadiusSpanMm)) >> 20);
            const int64_t ex = xMm - n.xMm;
            const int64_t ey = yMm - n.yMm;
            const int64_t marginSq = r * r - (ex * ex + ey * ey);
            if (marginSq <= 0) continue;
            out.shaftMarginSq = static_cast<int32_t>(marginSq);
            out.shaftDepthMaxMm = static_cast<int32_t>(n.depthMm);
        }
    }
    return out;
}

// --- per-voxel carve test ---------------------------------------------------

// True if voxel (.., vz) of this column should become MAT_AIR. `surfaceMm` /
// `bedrockDepthMm` come from the column's own ColumnSample.
//
// Guard order is deliberate: cheapest and most restrictive first, and every
// guard is an independent reason to refuse — none of them is load-bearing for
// the others (the static_asserts above prove the tube geometry already
// satisfies the roof and bedrock rules, so those two clamps only ever matter
// if someone retunes the constants without re-reading them).
constexpr bool caveCarveAt(const CaveColumn& cave, int32_t surfaceMm, int32_t bedrockDepthMm,
                           int64_t vz) {
    if (cave.count == 0 && cave.shaftMarginSq == 0) return false;
    // Sea level. The implicit ocean (W1) owns everything below z=0 that is not
    // terrain; a void down there is water, not a cave, so the cave pass simply
    // never goes there. Together with kCaveMinSurfaceMm this makes "caves
    // cannot breach or flood from the ocean" a property of the definition
    // rather than something to test for.
    if (vz < kCaveMinVoxelZ) return false;
    if (surfaceMm < kCaveMinSurfaceMm) return false;

    const int64_t centreMm = vz * kVoxelSizeMm + kVoxelSizeMm / 2;
    const int64_t depthMm = static_cast<int64_t>(surfaceMm) - centreMm;
    if (depthMm < 0) return false; // above ground is the surface shell's business
    // Sinkhole shaft: the ONE construct allowed through the roof clamp, by
    // design (see kCaveShaftNodeMask). Its bottom is a backbone crossing node,
    // so it always lands on the main network; its top is the surface, so it is
    // an entrance. Bounded well above bedrock by kCaveNodeDepth*.
    if (cave.shaftMarginSq > 0 && depthMm <= static_cast<int64_t>(cave.shaftDepthMaxMm))
        return true;
    if (depthMm < kCaveRoofMinMm) return false;
    if (depthMm + kCaveBedrockMarginMm >= static_cast<int64_t>(bedrockDepthMm)) return false;

    for (int32_t s = 0; s < cave.count; ++s) {
        const int64_t dz = depthMm - static_cast<int64_t>(cave.segs[s].depthMm);
        if (dz * dz < static_cast<int64_t>(cave.segs[s].marginSq)) return true;
    }
    return false;
}

} // namespace vxc
