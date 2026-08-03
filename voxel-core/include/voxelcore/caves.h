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
//      ENTRANCE (see kCaveShaftNodeMask) — a sparse, explicitly chosen set of
//      hash-gated sites, not a leaky roof. Since v25 that exception is a
//      cavity rather than a bore, so it covers area rather than points, and
//      test_caves.cpp bounds BOTH its footprint and the much smaller part of
//      it that is actually open to the sky.
//   2. The network stays out of the bedrock floor: deepest possible carve is
//      36.8 m, while bedrockDepthMm is >= 180 m everywhere since
//      kWorldGenVersion 5 (>= 40 m before it — amplifier.cpp), and
//      caveCarveAt additionally refuses anything within kCaveBedrockMarginMm
//      of the column's own bedrock top AND Amplifier::materialAt refuses to
//      turn MAT_BEDROCK into air at all. Three independent guards.
//   3. Cave MOUTHS. This block used to claim that tunnels daylight sideways on
//      steep slopes "for free", because 6 m of cover overhead is no cover at
//      all when the nearest free face is sideways. THAT CLAIM WAS FALSE AND
//      WAS NEVER MEASURED. vxc_caveprobe measured it at the grassland site the
//      day W3 started: sideways-daylighting columns numbered exactly the
//      perforated shaft columns, i.e. NOT ONE mouth existed that was not just
//      a vertical hole seen from below. The arithmetic says why — a tunnel
//      axis is at least 9 m under its own column, so for ground a metre away
//      to lie below it the terrain has to fall 9 m in 1 m, which is a cliff,
//      not a hillside.
//
//      v25 produces mouths deliberately instead, and from the entrance
//      construct rather than from the tunnels: an entrance cavity has a LEVEL
//      floor and a roof clipped by the real ground, so on falling ground the
//      roof becomes the hillside and the chamber opens through it. See the
//      entrance block below.
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

// --- entrances (v25, docs/underground-system-plan.md W3) ---------------------
//
// WHAT WAS HERE BEFORE, AND WHY IT WAS NOT A BUG. Depth-space tunnels never
// break the surface on their own (that is the roof guarantee), which on gentle
// terrain would leave the whole network sealed and reachable only by digging.
// So at a BACKBONE CROSSING node — one where (i & 3) == 0 AND (j & 3) == 0,
// hence guaranteed to have all four backbone tunnels incident on it — a 1-in-4
// hash gate opened a vertical shaft from that node's depth straight up to the
// surface. That construct carried THREE load-bearing guarantees at once and
// v25 keeps all three, by construction and not by tuning:
//
//   1. ENTRANCE RATE. The gate, the candidate node set and therefore the
//      number of entrances per km^2 are untouched. Same nodes, same 1-in-4.
//   2. STRUCTURAL CONNECTIVITY. The THROAT below is still a bore whose bottom
//      IS the node point, which lies on the axis of every tunnel meeting
//      there, and whose top is still the open surface. Everything else here is
//      shape added AROUND that bore.
//   3. ROOF INTEGRITY ELSEWHERE. The entrance is still the ONE enumerated
//      exception to the roof clamp in caveCarveAt, and it is still sparse. It
//      is no longer point-sized, so the exception's AREA is now a measured
//      quantity rather than a negligible one — test_caves.cpp
//      cave_entrance_exception_area_is_bounded is the gate on it.
//
// WHAT CHANGED, AND WHY. The owner's complaint was not that entrances exist;
// it was "really weird vertical shafts that shoot straight up to the game world
// surface", i.e. the SHAPE: a hash-placed, perfectly cylindrical, perfectly
// vertical bore with no surface expression — no bowl, no lip, no cause — at
// the same density on every landform. v25 keeps the bore and wraps it in an
// ENTRANCE CAVITY whose shape is decided by the ground:
//
//   * a LEVEL FLOOR at absolute z = (surface at the node) - kCaveEntranceFloor,
//     the same absolute-z anchoring caverns.h uses and for the same reason: a
//     floor that drapes with the terrain overhead is visibly wrong, and a level
//     one is somewhere a mob can stand (plan 5.6).
//   * a lens-shaped ROOF rising to `axisRise` over the node and tapering to the
//     floor at radius `reach`, so the cavity is a chamber that pinches out
//     rather than a hole with vertical walls;
//   * the roof is clipped by the real ground, `zTop = min(surface, floor + H)`.
//
// THAT ONE CLIP IS THE WHOLE PORTFOLIO. Nothing below branches on landform;
// the terrain does the branching, which is what makes this caused variety
// rather than placed variety (docs/landform-provinces-plan.md):
//
//   * FLAT GROUND. The surface is ~level with the node's, so the roof breaks
//     the ground only where H exceeds the floor depth — a bowl, open in the
//     middle, with a rock LIP overhanging the void all round it. A doline.
//   * A HILLSIDE. Downhill of the node the ground falls below floor + H, so
//     `zTop` becomes the ground itself and the cavity's roof IS the hill face:
//     the chamber daylights SIDEWAYS as a horizontal mouth with a level floor,
//     and stops entirely where the ground drops below the floor. This is the
//     "caves open horizontally in mountainsides" item, and it is free — no
//     second construct, no slope input, no new hash.
//   * A DRAINAGE LINE. Ground falling along one axis only stretches the mouth
//     along that axis: the swallow-hole/streambed-capture GEOMETRY, which is
//     what W3 can honestly build while water waits for the watershed work.
//
// The rim is warped by a value-noise field (kCaveEntranceRim*) so no two
// cavities share an outline and none of them is a circle.
//
// Density is unchanged: one candidate node per 4x4 lattice cells (102.4 m
// square), gated to 1 in 4, so roughly one entrance per 205 m square.
inline constexpr int64_t kCaveShaftNodeMask = 3;  // (i & mask) == 0 && (j & mask) == 0
inline constexpr uint64_t kCaveShaftGateMask = 3; // 1 in 4 of those open
// The THROAT: the surviving v24 bore, surface -> node. It is deliberately
// unchanged in size and role. It is what makes "every open site has an opening
// at the axis, and that opening reaches a backbone node" a structural fact
// instead of something the cavity's arithmetic has to be trusted to deliver.
inline constexpr int64_t kCaveShaftRadiusMinMm = 1000;
inline constexpr int64_t kCaveShaftRadiusSpanMm = 700;
inline constexpr int64_t kCaveShaftRadiusMaxMm = kCaveShaftRadiusMinMm + kCaveShaftRadiusSpanMm;

// Cavity floor depth below the NODE's own surface: [5, 9) m. Strictly less than
// kCaveNodeDepthMinMm so the floor is always above the node and the throat
// always has something to bore through (static_assert below).
inline constexpr int64_t kCaveEntranceFloorMinMm = 5000;
inline constexpr int64_t kCaveEntranceFloorSpanMm = 4000;
// How far the roof rises ABOVE the floor at the axis, over and above the floor
// depth itself: axisRise = floorDepth + over, with over in [1, 3) m. Because
// axisRise > floorDepth, the roof at the axis is above the node's own surface,
// which is what opens the bowl on flat ground; `over` is therefore the knob
// that decides how WIDE the daylight hole is, and hashing it is what stops
// every doline being the same size.
inline constexpr int64_t kCaveEntranceOverMinMm = 1000;
inline constexpr int64_t kCaveEntranceOverSpanMm = 2000;
// Footprint radius: [5, 13) m. This is the horizontal extent of the whole
// cavity, NOT of the daylight hole — on flat ground the hole is
// reach * sqrt(over / axisRise), roughly a third of it.
inline constexpr int64_t kCaveEntranceReachMinMm = 5000;
inline constexpr int64_t kCaveEntranceReachSpanMm = 8000;
inline constexpr int64_t kCaveEntranceReachMaxMm =
    kCaveEntranceReachMinMm + kCaveEntranceReachSpanMm;
// Rim warp: bilinear value noise on a 6.4 m grid, applied as a Q10 multiplier
// on the squared radial parameter, so the rim wanders by roughly +/-12% of the
// reach. It multiplies t^2, so it is exactly ZERO at the axis — the daylight
// guarantee cannot be perturbed by it, which is why it is a multiplier and not
// an offset.
inline constexpr int64_t kCaveEntranceRimCellMm = 6400;
inline constexpr int64_t kCaveEntranceRimAmpQ10 = 256; // t^2 scaled by [0.75, 1.25)
inline constexpr int64_t kCaveEntranceRimMaxQ10 = 1024 + kCaveEntranceRimAmpQ10;

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

// --- entrance-cavity invariants (v25) ---------------------------------------

// The cavity floor must sit strictly ABOVE the shallowest node, or the throat
// would have nothing to bore and the union of throat and cavity would stop
// being a single depth interval (which is what lets CaveColumn carry an
// entrance in two int32s — see the reduction below).
static_assert(kCaveEntranceFloorMinMm + kCaveEntranceFloorSpanMm <= kCaveNodeDepthMinMm,
              "the entrance cavity's floor must stay above the shallowest lattice node, or "
              "the throat has nothing to bore and the entrance stops being one depth interval");
// The roof at the axis must clear the floor, unconditionally, for every draw.
// This is the DAYLIGHT GUARANTEE in arithmetic form: axisRise > floorDepth means
// the cavity roof at the axis is above the node's own surface.
static_assert(kCaveEntranceOverMinMm > 0,
              "the entrance cavity's axis rise must exceed its floor depth, or a site could "
              "generate a sealed chamber instead of an entrance");
// The throat must lie strictly inside the cavity for every combination of
// draws INCLUDING the worst rim warp, so the throat's own daylight bore is
// never left standing outside the chamber it is supposed to open into. The
// test is on t^2: (rThroatMax / reachMin)^2 * rimMax < 1.
static_assert(kCaveShaftRadiusMaxMm * kCaveShaftRadiusMaxMm * kCaveEntranceRimMaxQ10 <
                  kCaveEntranceReachMinMm * kCaveEntranceReachMinMm * 1024,
              "the throat must stay inside the entrance cavity's footprint under the worst "
              "rim warp, or a bore could daylight outside its own chamber");
// The reach bounds how far from the querying column the cave pass has to
// evaluate the terrain surface (see the surfaceAt contract below). The GPU
// raster window is sized from kCavernMaxReachMm, and the assert that the
// entrance tap fits inside it lives next to that constant (gpu_harness.cpp,
// VoxelGpuRegionBuild.h) where the margin is actually computed.
static_assert(kCaveEntranceReachMaxMm < kCaveLatticeMm,
              "an entrance cavity must not reach further than one lattice cell, or the 3x3 "
              "candidate block stops being exhaustive for entrances");

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
// v25 (plan W3). The entrance CAVITY's own draws get a channel of their own
// rather than spending more bits of CH_CAVE_SHAFT: that hash already supplies
// the gate (bits 48..49) and the throat radius (bits 0..19), and stacking three
// more 20-bit fields on it would leave nothing spare for a later entrance term.
// CH_CAVE_ENTRANCE_RIM is keyed on a WORLD-POSITION grid cell, not on a lattice
// node, so it must not share a channel with anything keyed on (i, j).
inline constexpr uint32_t CH_CAVE_ENTRANCE = 51;     // cavity floor / axis rise / reach
inline constexpr uint32_t CH_CAVE_ENTRANCE_RIM = 52; // rim value noise, keyed on world cells

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
    // The entrance over this column, if any (at most one can be in range —
    // entrance nodes are 102.4 m apart and a cavity reaches at most 13 m).
    // Zero marginSq means "no entrance here", which is the overwhelmingly
    // common case.
    //
    // v25 (plan W3) added `shaftDepthMinMm`, and it is the one struct change
    // the entrance portfolio needed. Until v24 an entrance was always open to
    // the sky over its whole footprint, so a single "carve from depth 0 down
    // to here" cutoff described it exactly. A cavity whose roof is clipped by
    // the real ground is NOT that: uphill of the node, and everywhere under the
    // overhanging lip of a doline, the void starts BELOW the surface. Two
    // int32s therefore describe the entrance as the closed depth interval
    // [min, max], and the per-voxel test gains exactly one compare.
    //
    // WHY AN INTERVAL IS ENOUGH — the union of the throat and the cavity is
    // provably one interval, never two. Both share the same bottom-anchored
    // structure: the cavity occupies [floorZ, zTop] and the throat, where it
    // exists, occupies [nodeZ, surface] with nodeZ <= floorZ (static_assert on
    // kCaveEntranceFloor*). So a throat column's union is [nodeZ, surface] and
    // a cavity-only column's is [floorZ, zTop]. There is no case that produces
    // a gap, which is what keeps this two fields rather than a segment list.
    int32_t shaftMarginSq = 0;   // reach^2 - xyDist^2 to the entrance axis, > 0 if present
    int32_t shaftDepthMinMm = 0; // ... shallowest carved depth (0 == open to the sky here)
    int32_t shaftDepthMaxMm = 0; // ... deepest carved depth
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
    int32_t shaftNodeSlot = -1;    // di + 4*dj of the entrance candidate, -1 = none
    int32_t shaftRadiusMm = 0;     // the throat bore's radius
    // v25 entrance cavity, all four decided at the SITE (see caveEntranceSite).
    int32_t entranceReachMm = 0;   // footprint radius
    int32_t entranceAxisRiseMm = 0; // roof height above the floor, at the axis
    int64_t entranceFloorZMm = 0;  // ABSOLUTE z of the level floor
    int64_t entranceNodeZMm = 0;   // ABSOLUTE z of the node = the throat's bottom
};

// --- entrance rim noise ------------------------------------------------------
// Bilinear value noise on a kCaveEntranceRimCellMm grid, in Q10, keyed on WORLD
// position rather than on the entrance node — so the warp is a property of the
// place, two neighbouring entrances in the same terrain share the same field,
// and nothing about it depends on which site is being drawn.
//
// Returned as a Q10 multiplier in [1024 - amp, 1024 + amp). Four hash2 calls,
// paid only by columns already inside an entrance footprint (~0.5% of the
// world), and integer-bilinear so it has no seams.
constexpr int64_t caveEntranceRimQ10(uint64_t seed, int64_t xMm, int64_t yMm) {
    const int64_t gx = floorDiv(xMm, kCaveEntranceRimCellMm);
    const int64_t gy = floorDiv(yMm, kCaveEntranceRimCellMm);
    // Fractions in Q10. floorDiv above makes these non-negative for negative
    // world coordinates too, which is the whole reason the grid index is not a
    // shift.
    const int64_t fx = floorDiv((xMm - gx * kCaveEntranceRimCellMm) * 1024, kCaveEntranceRimCellMm);
    const int64_t fy = floorDiv((yMm - gy * kCaveEntranceRimCellMm) * 1024, kCaveEntranceRimCellMm);
    // 10 bits of each corner hash, so every intermediate below stays far inside
    // int64 even after two Q10 multiplies.
    const int64_t v00 = static_cast<int64_t>(hash2(seed, gx, gy, CH_CAVE_ENTRANCE_RIM) >> 54);
    const int64_t v10 = static_cast<int64_t>(hash2(seed, gx + 1, gy, CH_CAVE_ENTRANCE_RIM) >> 54);
    const int64_t v01 = static_cast<int64_t>(hash2(seed, gx, gy + 1, CH_CAVE_ENTRANCE_RIM) >> 54);
    const int64_t v11 =
        static_cast<int64_t>(hash2(seed, gx + 1, gy + 1, CH_CAVE_ENTRANCE_RIM) >> 54);
    // floorDiv, not `>> 10`, on every one of these: the corner DIFFERENCES
    // are signed, and a right shift of a negative signed value is one of the
    // constructs this file routes through floorDiv everywhere else rather than
    // trusting two languages to agree about. tools/lint-shader-ub.py has no
    // rule for it, so the discipline has to come from here.
    const int64_t a = v00 + floorDiv((v10 - v00) * fx, 1024);
    const int64_t b = v01 + floorDiv((v11 - v01) * fx, 1024);
    const int64_t v = a + floorDiv((b - a) * fy, 1024); // [0, 1024)
    return 1024 - kCaveEntranceRimAmpQ10 + floorDiv(v * (2 * kCaveEntranceRimAmpQ10), 1024);
}

// --- entrance site -----------------------------------------------------------
// The per-SITE half of the entrance: everything that depends on the node and on
// the terrain AT the node, and nothing that depends on the querying column.
//
// THE surfaceAt CONTRACT, identical in shape and in reason to caverns.h's: the
// surface is sampled at the NODE's own (xMm, yMm), not at the querying column's.
// A cavity floor that drapes with the ground overhead cannot produce a hillside
// mouth (the whole point of v25) and cannot give a mob anywhere level to stand.
// amplifier.cpp supplies `evalSurface(floorDiv(xMm, kVoxelSizeMm),
// floorDiv(yMm, kVoxelSizeMm)).surfaceMm`, mm -> voxel floorDiv included, and
// worldgen.ush mirrors that call exactly.
//
// `valid` is false when the node's own ground is below kCaveMinSurfaceMm — the
// beach/ocean guard applied to the SITE, mirroring cavernSiteFor. Without it a
// site whose node sits in shallow water could anchor a floor below sea level.
struct CaveEntranceSite {
    bool valid = false;
    int32_t throatRadiusMm = 0;
    int32_t reachMm = 0;
    int32_t axisRiseMm = 0;
    int64_t floorZMm = 0;
    int64_t nodeZMm = 0;
};

template <typename SurfaceFn>
constexpr CaveEntranceSite caveEntranceSite(uint64_t seed, int64_t i, int64_t j,
                                            const CaveNode& node, const SurfaceFn& surfaceAt) {
    CaveEntranceSite s;
    const uint64_t hs = hash2(seed, i, j, CH_CAVE_SHAFT);
    if (((hs >> 48) & kCaveShaftGateMask) != 0) return s; // not an entrance node
    const int32_t siteSurfaceMm = surfaceAt(node.xMm, node.yMm);
    if (siteSurfaceMm < kCaveMinSurfaceMm) return s; // beach/ocean guard on the SITE

    const uint64_t he = hash2(seed, i, j, CH_CAVE_ENTRANCE);
    const int64_t floorDepthMm =
        kCaveEntranceFloorMinMm +
        static_cast<int64_t>(((he & 0xFFFFFu) * static_cast<uint64_t>(kCaveEntranceFloorSpanMm)) >>
                             20);
    const int64_t overMm =
        kCaveEntranceOverMinMm +
        static_cast<int64_t>((((he >> 20) & 0xFFFFFu) *
                              static_cast<uint64_t>(kCaveEntranceOverSpanMm)) >> 20);
    const int64_t reachMm =
        kCaveEntranceReachMinMm +
        static_cast<int64_t>((((he >> 40) & 0xFFFFFu) *
                              static_cast<uint64_t>(kCaveEntranceReachSpanMm)) >> 20);

    s.valid = true;
    s.throatRadiusMm = static_cast<int32_t>(
        kCaveShaftRadiusMinMm +
        static_cast<int64_t>(((hs & 0xFFFFFu) * static_cast<uint64_t>(kCaveShaftRadiusSpanMm)) >>
                             20));
    s.reachMm = static_cast<int32_t>(reachMm);
    s.axisRiseMm = static_cast<int32_t>(floorDepthMm + overMm);
    s.floorZMm = static_cast<int64_t>(siteSurfaceMm) - floorDepthMm;
    s.nodeZMm = static_cast<int64_t>(siteSurfaceMm) - node.depthMm;
    return s;
}

// All of caveColumnFor's hashing, for lattice cell (ci, cj). Iteration order
// matches the fused form exactly.
//
// v25: templated on the surface function, for the entrance site only (see
// caveEntranceSite's surfaceAt contract). At most ONE surface tap per lattice
// cell — a cell is 65'536 voxel columns, so the memo in amplifier.cpp amortises
// it to nothing. The shader mirror cannot memoise, so it defers the tap until
// AFTER the cheap xy reject; the value is the same either way, since surfaceAt
// is a pure function of the node's xy.
template <typename SurfaceFn>
constexpr CaveLattice caveLatticeFor(uint64_t seed, int64_t ci, int64_t cj,
                                     const SurfaceFn& surfaceAt) {
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

    // Entrance candidate. At most one node in the 3x3 block can be a backbone
    // crossing (three consecutive indices contain at most one multiple of 4 on
    // each axis), so the first hit in the fixed (dj, di) order is also the only
    // hit — recording it here is exactly what the fused loop's early-out found.
    for (int32_t dj = 0; dj < 3 && L.shaftNodeSlot < 0; ++dj)
        for (int32_t di = 0; di < 3 && L.shaftNodeSlot < 0; ++di) {
            const int64_t i = ci - 1 + di;
            const int64_t j = cj - 1 + dj;
            if ((i & kCaveShaftNodeMask) != 0 || (j & kCaveShaftNodeMask) != 0) continue;
            const int32_t slot = di + 4 * dj;
            const CaveEntranceSite site =
                caveEntranceSite(seed, i, j, L.nodes[slot], surfaceAt);
            if (!site.valid) continue;
            L.shaftNodeSlot = slot;
            L.shaftRadiusMm = site.throatRadiusMm;
            L.entranceReachMm = site.reachMm;
            L.entranceAxisRiseMm = site.axisRiseMm;
            L.entranceFloorZMm = site.floorZMm;
            L.entranceNodeZMm = site.nodeZMm;
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
constexpr CaveColumn caveColumnFromLattice(uint64_t seed, const CaveLattice& L, int64_t vx,
                                           int64_t vy, int32_t surfaceMm) {
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

    // --- entrance: the single candidate the lattice block already found ------
    //
    // Everything here is in ABSOLUTE z and converted to this column's depth
    // space exactly once, at the end. That is deliberate: the cavity's floor
    // and roof are level surfaces, and expressing a level surface in a
    // per-column depth frame is precisely the thing that produced a draped,
    // shape-less bore in v24.
    if (L.shaftNodeSlot >= 0) {
        const CaveNode& n = nodes[L.shaftNodeSlot];
        const int64_t ex = xMm - n.xMm;
        const int64_t ey = yMm - n.yMm;
        const int64_t distSq = ex * ex + ey * ey;
        const int64_t reach = L.entranceReachMm;
        const int64_t reachSq = reach * reach;
        if (distSq < reachSq) {
            // Radial parameter, squared, in Q16, warped by the rim noise. The
            // warp MULTIPLIES, so it vanishes at the axis: the daylight
            // guarantee below cannot be perturbed by terrain-keyed noise.
            //
            // distSq <= reachSq < 169e6 and the shift is by 16, so the
            // numerator peaks near 1.1e13 — three orders of magnitude inside
            // int64, and no denominator is ever squared (the trap the crevice
            // taper documents).
            const int64_t tSqQ16 =
                floorDiv(floorDiv(distSq * 65536, reachSq) * caveEntranceRimQ10(seed, xMm, yMm),
                         1024);
            // Roof height above the level floor: a lens that is axisRise tall
            // over the node and pinches to the floor at the (warped) rim.
            const int64_t hMm =
                tSqQ16 >= 65536 ? 0
                                : floorDiv(static_cast<int64_t>(L.entranceAxisRiseMm) *
                                               (65536 - tSqQ16),
                                           65536);
            // THE ONE CLIP THAT MAKES THE PORTFOLIO. On flat ground `zRoof`
            // wins in the middle and the ground wins further out: a bowl with
            // an overhanging lip. On a hillside the ground wins on the downhill
            // side and the cavity daylights sideways; where the ground falls
            // below the floor there is nothing left to carve at all.
            const int64_t zRoofMm = L.entranceFloorZMm + hMm;
            const int64_t zTopMm =
                (static_cast<int64_t>(surfaceMm) < zRoofMm) ? static_cast<int64_t>(surfaceMm)
                                                            : zRoofMm;
            if (zTopMm >= L.entranceFloorZMm) {
                out.shaftMarginSq = static_cast<int32_t>(reachSq - distSq);
                const int64_t dMinMm = static_cast<int64_t>(surfaceMm) - zTopMm;
                out.shaftDepthMinMm = static_cast<int32_t>(dMinMm > 0 ? dMinMm : 0);
                out.shaftDepthMaxMm =
                    static_cast<int32_t>(static_cast<int64_t>(surfaceMm) - L.entranceFloorZMm);
            }
        }
        // The THROAT, unchanged from v24 in size and role: a bore from this
        // column's own surface down to the node. It is a strict superset of the
        // cavity's interval here (the static_asserts prove the throat lies
        // inside the footprint and the node below the floor), so it simply
        // replaces the interval rather than having to be merged with it — and
        // it is what keeps "every open site has daylight over a backbone node"
        // a structural fact rather than an arithmetic hope.
        const int64_t rt = L.shaftRadiusMm;
        const int64_t throatDepthMm = static_cast<int64_t>(surfaceMm) - L.entranceNodeZMm;
        // `throatDepthMm >= 0` is a degeneracy guard, not a normal case: the
        // throat is at most 1.7 m from the node, so this column's ground can
        // only be BELOW the node's own depth where the terrain drops ~10 m
        // within 1.7 m -- a cliff face cutting through the site. Without it
        // that column would still be flagged as an entrance and would take
        // caveCarveAt's roof-clamp exception for an interval that is empty.
        if (distSq < rt * rt && throatDepthMm >= 0) {
            const int64_t throatMarginSq = rt * rt - distSq;
            if (throatMarginSq > static_cast<int64_t>(out.shaftMarginSq))
                out.shaftMarginSq = static_cast<int32_t>(throatMarginSq);
            out.shaftDepthMinMm = 0;
            if (throatDepthMm > static_cast<int64_t>(out.shaftDepthMaxMm))
                out.shaftDepthMaxMm = static_cast<int32_t>(throatDepthMm);
        }
    }
    return out;
}

// Fused form: the pure, self-contained `f(seed, vx, vy, surfaceMm)` the
// worldgen contract and the HLSL mirror are written against. Callers walking
// many columns should go through amplifier.cpp's memoised path instead, which
// produces bit-identical values.
template <typename SurfaceFn>
constexpr CaveColumn caveColumnFor(uint64_t seed, int64_t vx, int64_t vy, int32_t surfaceMm,
                                   const SurfaceFn& surfaceAt) {
    CaveColumn out;
    if (surfaceMm < kCaveMinSurfaceMm) return out;
    const int64_t ci = floorDiv(vx * kVoxelSizeMm, kCaveLatticeMm);
    const int64_t cj = floorDiv(vy * kVoxelSizeMm, kCaveLatticeMm);
    return caveColumnFromLattice(seed, caveLatticeFor(seed, ci, cj, surfaceAt), vx, vy, surfaceMm);
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
    // The entrance: the ONE construct allowed through the roof clamp, by design
    // (see kCaveShaftNodeMask). Its throat's bottom is a backbone crossing node,
    // so it always lands on the main network; its throat's top is the surface,
    // so it is an entrance. Bounded well above bedrock by kCaveNodeDepth*.
    //
    // v25 made this a closed depth INTERVAL rather than a cutoff — one extra
    // compare — because the cavity's roof is clipped by the real ground, so
    // under a doline's overhanging lip and everywhere uphill of a hillside
    // mouth the void legitimately starts below the surface. `shaftDepthMinMm`
    // is 0 wherever the entrance is open to the sky, which is the whole
    // throat and the middle of every bowl.
    if (cave.shaftMarginSq > 0 && depthMm >= static_cast<int64_t>(cave.shaftDepthMinMm) &&
        depthMm <= static_cast<int64_t>(cave.shaftDepthMaxMm))
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
