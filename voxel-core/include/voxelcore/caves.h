#pragma once
// Cave pass v1 (M4, plan §4 "cave pass") — deterministic integer carving of a
// CONNECTED tunnel network into the amplifier's stratigraphy.
//
// Header-only, integer-only (CI float-ban). Every expression here is mirrored
// bit-for-bit in voxel-core/shaders/worldgen.ush's `caveColumnFor` /
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
//      36.8 m, while bedrockDepthMm is >= 180 m everywhere since
//      kWorldGenVersion 5 (>= 40 m before it — amplifier.cpp), and
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

// Tube radius: [min, min+span). 1.2 m .. 2.8 m radius = 2.4 m .. 5.6 m wide
// passages — walkable by an agent, diggable by a player, and wide enough that
// the greedy mesher makes cheap quads out of the walls.
//
// v24 (docs/underground-system-plan.md W2): the radius is no longer constant
// along an edge. Three draws now bound one passage — the two END nodes' own
// radii and the per-edge draw, which becomes the MID value — and the radius is
// interpolated between them, so a passage swells and chokes along its length
// instead of being one extruded bore. The RANGE is deliberately unchanged: the
// plan's widening to [0.8, 4.0] m belongs with the field coupling of W5/W6 that
// decides where the wide end lives, and dropping the floor to 0.8 m would break
// the crevice containment static_assert below (crevices must stay thinner than
// the thinnest tunnel) — a coupling worth stating rather than discovering.
inline constexpr int64_t kCaveRadiusMinMm = 1200;
inline constexpr int64_t kCaveRadiusSpanMm = 1600;
inline constexpr int64_t kCaveRadiusMaxMm = kCaveRadiusMinMm + kCaveRadiusSpanMm;

// --- waypointed axes (v24, plan W2) -----------------------------------------
// Every edge gains ONE interior waypoint near its midpoint, hash-jittered
// sideways and dipped downward, turning a straight capsule into a two-segment
// polyline. Both endpoints are still the lattice nodes, so every connectivity
// argument in this file is untouched: the backbone is still the same graph and
// the shaft still bottoms out on a node that four tunnels pass through.
//
// WHAT THIS DOES AND DOES NOT FIX. It removes the straight-capsule tell (plan
// section 2.6 item 3) and it breaks the long unbroken SIGHTLINE down a backbone
// row. It does NOT flatten the network's routing: both ends of a run are still
// lattice nodes, so eight consecutive backbone edges still displace due east
// within a couple of degrees no matter how the axis wanders in between.
// vxc_caveprobe reports those as two separate numbers (SEGMENT lock and ROUTING
// lock) precisely so this distinction is measured rather than assumed; moving
// the routing lock needs off-axis edges, which is a different change.
inline constexpr int64_t kCaveWaypointLatMm = 5120; // +/- 5.12 m sideways
// DOWNWARD ONLY, and the asymmetry is forced rather than chosen: the roof
// static_assert below has only 200 mm of slack (min axis depth 9 m minus max
// radius 2.8 m against a 6 m clamp), so ANY upward excursion needs the depth
// band re-tuned first — which moves every cave in the world and is its own
// decision. Downward has 1200 mm of slack against the deliberately conservative
// 40 m bedrock assert, and 1.0 m of it is spent here.
inline constexpr int64_t kCaveWaypointDipMm = 1000;
inline constexpr int32_t kCaveEdgeSubSegs = 2; // a -> waypoint -> b

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
// sitting exactly on a junction can legitimately see 4 tunnels; since M4 cave
// pass v2 (docs/cavern-design.md §4) a crevice can ride the SAME edge as a
// tunnel, so a junction column can now see up to 4 tunnels + 4 crevices = 8;
// 12 leaves headroom for an unrelated tube brushing past. The cap is a
// fixed-size-storage bound, not a design limit — test_caves.cpp
// CaveSegmentCapHeadroom measures the real maximum over a large sample and
// fails if it ever reaches the cap, so the (deterministic, fixed-iteration-
// order) truncation below is never reached in practice on either CPU or GPU.
// v24 note, recorded because the plan expected the opposite: waypointing an
// edge into kCaveEdgeSubSegs independently-reduced sub-segments does NOT need
// a bigger cap. The theoretical worst case doubles, but the measured maximum
// over 800k columns is unchanged at 5-6 (test_caves.cpp
// cave_segment_cap_headroom) -- consecutive halves of one polyline are on
// opposite sides of the waypoint, so a column is normally inside only one of
// them. The cap stays at 12, which keeps ColumnSample the size it was and
// leaves the GPU struct's unrolled arrays alone. Crawlways (plan W7) are a
// second lattice and are the thing that will actually move this number.
inline constexpr int32_t kMaxCaveSegs = 12;

// --- crevices (M4 cave pass v2, docs/cavern-design.md §4) -------------------
// A crevice is NOT a new generator: it is a 1-in-8 gated decoration riding an
// EXISTING lattice edge (the tube is already there), a thin lens-tapered
// vertical slab centered on the tube's own axis. Because the slab always
// contains the axis point, connectivity is inherited for free from the tube
// it rides — no separate connectivity argument needed. It emits an ordinary
// CaveSeg, in depth space exactly like the tube, so there is zero new
// per-voxel mechanism: caveCarveAt does not change at all.
inline constexpr uint64_t kCrevGateMask = 7; // 1-in-8: top 3 bits of the hash == 0
inline constexpr int64_t kCrevHalfThickMinMm = 300;  // 0.3 m
inline constexpr int64_t kCrevHalfThickSpanMm = 500; // -> [0.3, 0.8) m
inline constexpr int64_t kCrevUpMinMm = 3000;   // 3 m above the tube axis
inline constexpr int64_t kCrevUpSpanMm = 7000;  // -> [3, 10) m
inline constexpr int64_t kCrevDownMinMm = 2000; // 2 m below the tube axis
inline constexpr int64_t kCrevDownSpanMm = 4000; // -> [2, 6) m

// Crevices must stay thinner than the thinnest tunnel: their xy accept
// corridor (radius t) is then always a strict subset of the tube's own
// (radius r), so a crevice can never exist somewhere its own tube doesn't —
// the containment the header comment's connectivity argument relies on.
static_assert(kCrevHalfThickMinMm + kCrevHalfThickSpanMm < kCaveRadiusMinMm,
              "crevices must stay thinner than the thinnest tunnel radius, or a "
              "crevice could exist outside its own tube's xy footprint");

// --- structural invariants, checked at compile time -------------------------

// The 3x3 candidate block is only exhaustive while a tube cannot reach more
// than one lattice cell sideways off its axis' bounding box. Since v24 the axis
// is a polyline whose waypoint may sit kCaveWaypointLatMm outside that box, so
// the waypoint excursion is part of the reach and part of this bound.
static_assert((kCaveRadiusMaxMm + kCaveWaypointLatMm) * 2 < kCaveLatticeMm,
              "cave tube reach (max radius plus the waypoint's lateral excursion) must "
              "stay well under one lattice cell, or the 3x3 candidate-node block in "
              "caveColumnFor stops being exhaustive");
// Roof: shallowest possible carved voxel is axis depth minus max radius. The
// waypoint dips DOWNWARD only, so the shallowest axis point is still a node and
// this bound is unchanged by v24 — see kCaveWaypointDipMm for why that
// asymmetry is forced rather than chosen.
static_assert(kCaveNodeDepthMinMm - kCaveRadiusMaxMm > kCaveRoofMinMm,
              "tube geometry must keep itself above the roof clamp — the clamp is a "
              "backstop, not the mechanism");
// Floor: deepest possible carved voxel vs a bedrock top of 40 m. That is the
// PRE-v5 amplifier minimum, kept deliberately after the v5 move to a
// 180-220 m band (amplifier.cpp bedrockDepthMm): asserting against the old,
// much shallower floor is a strictly stronger statement, and it keeps tunnel
// geometry provably independent of wherever the bedrock band happens to sit.
// The waypoint dip is added because it is the one construct that can put the
// axis below its own endpoints.
static_assert(kCaveNodeDepthMinMm + kCaveNodeDepthSpanMm + kCaveWaypointDipMm +
                      kCaveRadiusMaxMm + kCaveBedrockMarginMm < 40000,
              "tube geometry must keep itself out of bedrock — the bedrock margin is a "
              "backstop, not the mechanism");

// --- hash channels ----------------------------------------------------------
// Extends hash.h's HashChannel registry — see the authoritative allocation
// table at the top of hash.h (and its machine-checked twin,
// voxelcore/hash_channel_registry.h) before adding or renumbering anything
// here. Declared here rather than in hash.h to keep the cave pass
// self-contained; APPEND ONLY, never renumber a shipped id without a
// kWorldGenVersion bump.
//
// CH_CAVE_NODE/CH_CAVE_EDGE live at 30/31, not 18/19: they originally reused
// 18/19, which collided with hash.h's own CH_ECOTONE_TEMP/CH_ECOTONE_PRECIP
// (same two ids, two different features, a real double allocation — not
// merely a name clash). Moved to the free ids hash.h's table identifies.
inline constexpr uint32_t CH_CAVE_NODE = 30;   // node jitter (position + depth)
inline constexpr uint32_t CH_CAVE_EDGE = 31;   // non-backbone edge gate
inline constexpr uint32_t CH_CAVE_RADIUS = 20; // per-edge tube radius (the MID value since v24)
inline constexpr uint32_t CH_CAVE_SHAFT = 21;  // sinkhole gate + shaft radius
// 22/23/25 belong to voxelcore/caverns.h (CH_CAVERN_SITE/_ROUGH/_FLOOD).
inline constexpr uint32_t CH_CREVICE = 24;     // crevice gate + slab geometry
// v24 (plan W2). Two new ids, both taken from hash.h's declared-free list.
// 29 is the id density3.h's deleted CH_POCKET used to hold; hash.h keeps it
// listed as free rather than silently reusable, so claiming it is registered
// in hash_channel_registry.h like everything else.
//
// The node radius needs its OWN channel rather than reusing CH_CAVE_RADIUS
// with an (i, j) key: the edge draw keys on (i, j*2+dir), and (i, 5) as a node
// key collides exactly with edge (i, j=2, dir=1). That would make a node's
// calibre equal to one particular neighbouring edge's, which is a correlation
// artifact, not a coincidence to leave alone.
inline constexpr uint32_t CH_CAVE_WAYPOINT = 29;    // edge waypoint offset + dip
inline constexpr uint32_t CH_CAVE_NODE_RADIUS = 50; // per-node tube radius

// --- node / edge primitives -------------------------------------------------

struct CaveNode {
    int64_t xMm = 0;
    int64_t yMm = 0;
    int64_t depthMm = 0; // below the terrain surface, not an absolute z
};

// One edge's interior waypoint (v24): the midpoint of the two nodes, pushed
// sideways and dipped. `radiusMm` is the edge's own draw, i.e. the calibre at
// the waypoint.
struct CaveWaypoint {
    int64_t xMm = 0;
    int64_t yMm = 0;
    int64_t depthMm = 0;
};

// Jittered position + depth of lattice node (i, j). One hash2 supplies all
// three: 20 bits each for x jitter, y jitter and depth, scaled by
// multiply-then-shift (never a division — the shift is by a literal, so it is
// portable in HLSL too).
//
// Split into a "from an already-computed hash" form purely so callers that
// ALSO need other bits of the same `hash2(seed, i, j, CH_CAVE_NODE)` value can
// pay for it once instead of twice — voxelcore/caverns.h's site gate lives in
// bits 60/61 of exactly this hash, and used to recompute it. No value changes:
// caveNode() is still bit-for-bit what it was, and the shader mirror only ever
// needs the fused form below.
constexpr CaveNode caveNodeFromHash(uint64_t h, int64_t i, int64_t j) {
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

constexpr CaveNode caveNode(uint64_t seed, int64_t i, int64_t j) {
    return caveNodeFromHash(hash2(seed, i, j, CH_CAVE_NODE), i, j);
}

// dir 0 = the +x edge out of (i, j), dir 1 = the +y edge. Backbone edges are
// unconditional (that is what makes the network connected for every seed);
// everything else is a 1-in-4 hash gate.
constexpr bool caveEdgeExists(uint64_t seed, int64_t i, int64_t j, int32_t dir) {
    if (dir == 0 && (j & kCaveBackboneMask) == 0) return true;
    if (dir == 1 && (i & kCaveBackboneMask) == 0) return true;
    return ((hash2(seed, i, j * 2 + dir, CH_CAVE_EDGE) >> 48) & kCaveEdgeGateMask) == 0;
}

// The MID calibre of an edge since v24: the radius the passage has at its
// waypoint. Unchanged arithmetic — what changed is that it is one of three
// control values instead of the whole edge.
constexpr int64_t caveEdgeRadiusMm(uint64_t seed, int64_t i, int64_t j, int32_t dir) {
    const uint64_t h = hash2(seed, i, j * 2 + dir, CH_CAVE_RADIUS);
    return kCaveRadiusMinMm +
           static_cast<int64_t>((((h >> 44) & 0xFFFFFu) * static_cast<uint64_t>(kCaveRadiusSpanMm)) >> 20);
}

// The calibre a passage has AT a node (v24). Per NODE, not per edge, so all
// four passages meeting at a junction agree on how wide the junction is —
// which is what makes a junction read as one place rather than as four tubes
// that happen to touch.
constexpr int64_t caveNodeRadiusMm(uint64_t seed, int64_t i, int64_t j) {
    const uint64_t h = hash2(seed, i, j, CH_CAVE_NODE_RADIUS);
    return kCaveRadiusMinMm +
           static_cast<int64_t>((((h >> 44) & 0xFFFFFu) * static_cast<uint64_t>(kCaveRadiusSpanMm)) >> 20);
}

// One hash2 call per edge supplies the crevice's gate bit AND all of its
// geometry (half-thickness, up/down reach) — a single channel, unlike the
// tunnel's split gate/radius channels, since the design doc doesn't call for
// folding anything here (there is no separate cheap tier to protect: a
// crevice is only ever evaluated on an edge that already exists).
constexpr uint64_t caveCreviceHash(uint64_t seed, int64_t i, int64_t j, int32_t dir) {
    return hash2(seed, i, j * 2 + dir, CH_CREVICE);
}

// The interior waypoint of edge (i, j, dir), given its two endpoint nodes.
// One hash2 supplies all three fields: 20 bits of x offset, 20 of y offset,
// 20 of downward dip, each decoded by the same multiply-then-shift the node
// jitter uses (never a division). The base point is the exact integer midpoint
// of the two nodes — truncating toward zero via a shift would be direction-
// dependent for negative coordinates, so it goes through floorDiv like every
// other division in this file.
constexpr CaveWaypoint caveWaypoint(uint64_t seed, int64_t i, int64_t j, int32_t dir,
                                    const CaveNode& a, const CaveNode& b) {
    const uint64_t h = hash2(seed, i, j * 2 + dir, CH_CAVE_WAYPOINT);
    CaveWaypoint w;
    // The lateral base is the midpoint shifted a full excursion NEGATIVE, and
    // the decoded [0, 2*lat) draw is then added. Writing it as
    // "midpoint + draw - lat" is the same int64 value but subtracts from an
    // expression whose right operand came out of unsigned arithmetic, which is
    // the shape tools/lint-shader-ub.py rejects in the mirror -- and it is
    // right to: that shape is one editing slip away from wrapping. Both sides
    // are kept in this order so the CPU and the shader read identically.
    w.xMm = floorDiv(a.xMm + b.xMm, 2) - kCaveWaypointLatMm +
            static_cast<int64_t>(((h & 0xFFFFFu) *
                                  static_cast<uint64_t>(2 * kCaveWaypointLatMm)) >> 20);
    w.yMm = floorDiv(a.yMm + b.yMm, 2) - kCaveWaypointLatMm +
            static_cast<int64_t>((((h >> 20) & 0xFFFFFu) *
                                  static_cast<uint64_t>(2 * kCaveWaypointLatMm)) >> 20);
    w.depthMm =
        floorDiv(a.depthMm + b.depthMm, 2) +
        static_cast<int64_t>((((h >> 40) & 0xFFFFFu) *
                              static_cast<uint64_t>(kCaveWaypointDipMm)) >> 20);
    return w;
}
constexpr bool caveCreviceGateOpen(uint64_t h) {
    return ((h >> 61) & kCrevGateMask) == 0;
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

// ---------------------------------------------------------------------------
// LATTICE BLOCK (performance only — provably cannot change any output)
// ---------------------------------------------------------------------------
// Everything caveColumnFor hashes is a function of the column's LATTICE CELL
// (ci, cj), never of where inside that cell the column sits: the 4x4 node
// block, the 18 candidate edges' existence/radius/crevice draws, and the
// single sinkhole candidate. A lattice cell is 25.6 m square = 256x256 =
// 65'536 voxel columns, so on any realistic sweep those ~34-70 hashes were
// being recomputed tens of thousands of times over.
//
// Splitting them out into a plain value (`caveLatticeFor`) leaves the pure
// `caveColumnFor` composition below bit-identical — it is the same arithmetic
// in the same order — while letting a caller that walks many columns memoise
// the block. amplifier.cpp does exactly that with a thread_local direct-mapped
// table, the same output-neutral scheme it already uses for tile reads. The
// shader mirror can keep fusing the two halves; nothing about the contract,
// the iteration order or any constant moves.
//
// Note the block computes the crevice hash for every EXISTING edge, whereas
// the fused form only reached it for edges the column actually falls inside.
// That is strictly more work per lattice cell and strictly less per column
// (amortised over 65'536 of them), and the VALUE is unchanged because
// caveCreviceHash is a pure function of (seed, i, j, dir).
// radiusMm == 0 is the "no edge here" sentinel; a real tube can never hash to
// it, so no parallel existence flag (and no extra byte per edge) is needed.
static_assert(kCaveRadiusMinMm > 0,
              "CaveLatticeEdge uses radiusMm == 0 as its 'edge does not exist' sentinel");

struct CaveLatticeEdge {
    int32_t radiusMm = 0;  // MID calibre; 0 == this edge does not exist
    uint64_t crevHash = 0; // caveCreviceHash for it (meaningless if radiusMm == 0)
    CaveWaypoint way;      // v24 interior waypoint (meaningless if radiusMm == 0)
};

struct CaveLattice {
    CaveNode nodes[16] = {};        // di + 4*dj over the 4x4 block
    int32_t nodeRadiusMm[16] = {};  // v24 per-node calibre, same slot indexing
    CaveLatticeEdge edges[18] = {}; // (di + 3*dj) * 2 + dir over the 3x3 sources
    int32_t shaftNodeSlot = -1;    // di + 4*dj of the sinkhole candidate, -1 = none
    int32_t shaftRadiusMm = 0;
};

// All of caveColumnFor's hashing, for lattice cell (ci, cj). Iteration order
// matches the fused form exactly.
constexpr CaveLattice caveLatticeFor(uint64_t seed, int64_t ci, int64_t cj) {
    CaveLattice L;

    // 4x4 node block: the 3x3 candidate SOURCE nodes plus the +x/+y endpoints
    // they need.
    for (int32_t dj = 0; dj < 4; ++dj)
        for (int32_t di = 0; di < 4; ++di) {
            const int32_t slot = di + 4 * dj;
            L.nodes[slot] = caveNode(seed, ci - 1 + di, cj - 1 + dj);
            L.nodeRadiusMm[slot] =
                static_cast<int32_t>(caveNodeRadiusMm(seed, ci - 1 + di, cj - 1 + dj));
        }

    for (int32_t dj = 0; dj < 3; ++dj)
        for (int32_t di = 0; di < 3; ++di) {
            const int64_t i = ci - 1 + di;
            const int64_t j = cj - 1 + dj;
            for (int32_t dir = 0; dir < 2; ++dir) {
                if (!caveEdgeExists(seed, i, j, dir)) continue;
                CaveLatticeEdge& e = L.edges[(di + 3 * dj) * 2 + dir];
                e.radiusMm = static_cast<int32_t>(caveEdgeRadiusMm(seed, i, j, dir));
                e.crevHash = caveCreviceHash(seed, i, j, dir);
                const CaveNode& a = L.nodes[di + 4 * dj];
                const CaveNode& b = (dir == 0) ? L.nodes[di + 1 + 4 * dj]
                                               : L.nodes[di + 4 * (dj + 1)];
                e.way = caveWaypoint(seed, i, j, dir, a, b);
            }
        }

    // Sinkhole shaft candidate. At most one node in the 3x3 block can be a
    // backbone crossing (three consecutive indices contain at most one
    // multiple of 4 on each axis), so the first hit in the fixed (dj, di)
    // order is also the only hit — recording it here is exactly what the
    // fused loop's early-out found.
    for (int32_t dj = 0; dj < 3 && L.shaftNodeSlot < 0; ++dj)
        for (int32_t di = 0; di < 3 && L.shaftNodeSlot < 0; ++di) {
            const int64_t i = ci - 1 + di;
            const int64_t j = cj - 1 + dj;
            if ((i & kCaveShaftNodeMask) != 0 || (j & kCaveShaftNodeMask) != 0) continue;
            const uint64_t h = hash2(seed, i, j, CH_CAVE_SHAFT);
            if (((h >> 48) & kCaveShaftGateMask) != 0) continue;
            L.shaftNodeSlot = di + 4 * dj;
            L.shaftRadiusMm = static_cast<int32_t>(
                kCaveShaftRadiusMinMm +
                static_cast<int64_t>(((h & 0xFFFFFu) * static_cast<uint64_t>(kCaveShaftRadiusSpanMm)) >> 20));
        }
    return L;
}

// Every tube axis within reach of column (vx, vy), given its lattice cell's
// already-hashed block. `surfaceMm` is the column's own terrain height:
// columns at or below kCaveMinSurfaceMm are excluded outright by the caller,
// which is the ocean/beach guard (see caveCarveAt).
//
// Iteration order (dj, then di, then dir) is part of the worldgen contract: it
// is what decides which segments survive if the kMaxCaveSegs cap were ever
// hit, and the shader mirrors it exactly.
constexpr CaveColumn caveColumnFromLattice(const CaveLattice& L, int64_t vx, int64_t vy) {
    CaveColumn out;
    const int64_t xMm = vx * kVoxelSizeMm;
    const int64_t yMm = vy * kVoxelSizeMm;
    const CaveNode* nodes = L.nodes;

    for (int32_t dj = 0; dj < 3; ++dj) {
        for (int32_t di = 0; di < 3; ++di) {
            for (int32_t dir = 0; dir < 2; ++dir) {
                const CaveLatticeEdge& edge = L.edges[(di + 3 * dj) * 2 + dir];
                if (edge.radiusMm == 0) continue; // edge does not exist
                const int32_t aSlot = di + 4 * dj;
                const int32_t bSlot = (dir == 0) ? di + 1 + 4 * dj : di + 4 * (dj + 1);
                const CaveNode a = nodes[aSlot];
                const CaveNode b = nodes[bSlot];

                // v24: the axis is a two-segment polyline a -> waypoint -> b,
                // and each sub-segment is reduced independently. Reducing only
                // the global closest approach would be cheaper but wrong: a
                // waypointed edge can pass the SAME column twice at two
                // different depths, and that second passage is exactly the
                // vertical richness the waypoints exist to create.
                //
                // The iteration order (dj, di, dir, sub) is part of the
                // worldgen contract — it decides which segments survive if the
                // kMaxCaveSegs cap were ever hit — and the shader mirrors it.
                for (int32_t sub = 0; sub < kCaveEdgeSubSegs; ++sub) {
                    const int64_t pxMm = (sub == 0) ? a.xMm : edge.way.xMm;
                    const int64_t pyMm = (sub == 0) ? a.yMm : edge.way.yMm;
                    const int64_t pdMm = (sub == 0) ? a.depthMm : edge.way.depthMm;
                    const int64_t qxMm = (sub == 0) ? edge.way.xMm : b.xMm;
                    const int64_t qyMm = (sub == 0) ? edge.way.yMm : b.yMm;
                    const int64_t qdMm = (sub == 0) ? edge.way.depthMm : b.depthMm;
                    // Calibre control values: node radius at the outer end,
                    // the edge's own draw at the waypoint.
                    const int64_t prMm =
                        (sub == 0) ? L.nodeRadiusMm[aSlot] : static_cast<int64_t>(edge.radiusMm);
                    const int64_t qrMm =
                        (sub == 0) ? static_cast<int64_t>(edge.radiusMm) : L.nodeRadiusMm[bSlot];

                    const int64_t dx = qxMm - pxMm;
                    const int64_t dy = qyMm - pyMm;
                    const int64_t den = dx * dx + dy * dy;
                    if (den == 0) continue; // a waypoint landing exactly on its node

                    // Closest approach in xy, parameterised on the sub-segment
                    // and clamped to it. Rational t = num/den kept exact; the
                    // single rounding is the floorDiv when the closest point is
                    // realised.
                    const int64_t wx = xMm - pxMm;
                    const int64_t wy = yMm - pyMm;
                    const int64_t num = clampi64(wx * dx + wy * dy, 0, den);

                    const int64_t cx = pxMm + floorDiv(dx * num, den);
                    const int64_t cy = pyMm + floorDiv(dy * num, den);
                    const int64_t cd = pdMm + floorDiv((qdMm - pdMm) * num, den);

                    const int64_t ex = xMm - cx;
                    const int64_t ey = yMm - cy;
                    // Calibre interpolated along the sub-segment at the same
                    // parameter as the axis point, so the passage swells and
                    // chokes continuously and is C0 across the waypoint and
                    // across every node.
                    const int64_t r = prMm + floorDiv((qrMm - prMm) * num, den);
                    const int64_t marginSq = r * r - (ex * ex + ey * ey);
                    if (marginSq <= 0) continue;

                    if (out.count < kMaxCaveSegs) {
                        out.segs[out.count].marginSq = static_cast<int32_t>(marginSq);
                        out.segs[out.count].depthMm = static_cast<int32_t>(cd);
                        ++out.count;
                    }

                // Crevice: a 1-in-8 gated thin vertical slab riding this same
                // edge (docs/cavern-design.md §4). Only reachable here because
                // marginSq > 0 already put us within the (wider) tunnel radius
                // r of this axis; kCrevHalfThickMaxMm < kCaveRadiusMinMm
                // (static_assert above) means a column outside r is provably
                // outside the crevice's own, narrower corridor too, so no
                // separate reject was skipped.
                const uint64_t crevH = edge.crevHash;
                if (caveCreviceGateOpen(crevH)) {
                    const int64_t tMm =
                        kCrevHalfThickMinMm +
                        static_cast<int64_t>(((crevH & 0xFFFFFu) * static_cast<uint64_t>(kCrevHalfThickSpanMm)) >> 20);
                    if (ex * ex + ey * ey <= tMm * tMm) {
                        const int64_t hUpMm =
                            kCrevUpMinMm + static_cast<int64_t>((((crevH >> 20) & 0xFFFFFu) *
                                                                  static_cast<uint64_t>(kCrevUpSpanMm)) >> 20);
                        const int64_t hDownMm =
                            kCrevDownMinMm + static_cast<int64_t>((((crevH >> 40) & 0xFFFFFu) *
                                                                    static_cast<uint64_t>(kCrevDownSpanMm)) >> 20);
                        // Per-column clamp: never let the slab's top reach
                        // shallower than the roof clamp, so a crevice pinches
                        // out gracefully near the surface instead of getting
                        // an abrupt flat lid from caveCarveAt's own guard.
                        const int64_t hUpEffMm = clampi64(hUpMm, 0, cd - kCaveRoofMinMm);
                        const int64_t halfSpanMm = (hUpEffMm + hDownMm) / 2;
                        // Lens taper: 4u(1-u), where u is the closest-approach
                        // parameter along the WHOLE edge, not along this
                        // sub-segment -- 0 at either node, 1 at the waypoint,
                        // so the fissure still pinches to nothing before it
                        // would otherwise end in a flat wall at the node.
                        // Since v24 the edge is two sub-segments, so
                        // u = (sub + num/den) / kCaveEdgeSubSegs; computing it
                        // per sub-segment instead would put a full-width slab
                        // at both nodes, which is the exact artifact the taper
                        // exists to remove. Done in Q16 fixed point
                        // rather than the exact rational (num*(den-num)/den^2)
                        // on purpose: den = dx^2+dy^2 for a jittered lattice
                        // edge can reach ~3e9 mm^2 (two node jitters can each
                        // span most of a lattice cell), so den*den overflows
                        // int64 -- this is exactly the kind of construct
                        // tools/lint-shader-ub.py exists to keep out of the
                        // shader, and the reason every division elsewhere in
                        // this file goes through floorDiv on pre-scaled
                        // quantities instead of squaring a large denominator.
                        const int64_t uQ16 =
                            floorDiv((static_cast<int64_t>(sub) << 16) + floorDiv(num << 16, den),
                                     kCaveEdgeSubSegs);
                        const int64_t taperQ16 = (4 * uQ16 * ((1 << 16) - uQ16)) >> 16;
                        const int64_t halfSpanTaperedMm = floorDiv(halfSpanMm * taperQ16, 1 << 16);
                        const int64_t crevMarginSq = halfSpanTaperedMm * halfSpanTaperedMm;
                        if (crevMarginSq > 0 && out.count < kMaxCaveSegs) {
                            out.segs[out.count].marginSq = static_cast<int32_t>(crevMarginSq);
                            out.segs[out.count].depthMm =
                                static_cast<int32_t>(cd + floorDiv(hDownMm - hUpEffMm, 2));
                            ++out.count;
                        }
                    }
                }
                } // sub
            }
        }
    }

    // Sinkhole shaft: the single candidate the lattice block already found.
    if (L.shaftNodeSlot >= 0) {
        const CaveNode& n = nodes[L.shaftNodeSlot];
        const int64_t r = L.shaftRadiusMm;
        const int64_t ex = xMm - n.xMm;
        const int64_t ey = yMm - n.yMm;
        const int64_t marginSq = r * r - (ex * ex + ey * ey);
        if (marginSq > 0) {
            out.shaftMarginSq = static_cast<int32_t>(marginSq);
            out.shaftDepthMaxMm = static_cast<int32_t>(n.depthMm);
        }
    }
    return out;
}

// Fused form: the pure, self-contained `f(seed, vx, vy, surfaceMm)` the
// worldgen contract and the HLSL mirror are written against. Callers walking
// many columns should go through amplifier.cpp's memoised path instead, which
// produces bit-identical values.
constexpr CaveColumn caveColumnFor(uint64_t seed, int64_t vx, int64_t vy, int32_t surfaceMm) {
    CaveColumn out;
    if (surfaceMm < kCaveMinSurfaceMm) return out;
    const int64_t ci = floorDiv(vx * kVoxelSizeMm, kCaveLatticeMm);
    const int64_t cj = floorDiv(vy * kVoxelSizeMm, kCaveLatticeMm);
    return caveColumnFromLattice(caveLatticeFor(seed, ci, cj), vx, vy);
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
