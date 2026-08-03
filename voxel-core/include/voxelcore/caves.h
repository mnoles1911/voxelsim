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

// ---------------------------------------------------------------------------
// FIELD COUPLING (v27, docs/underground-system-plan.md W5)
// ---------------------------------------------------------------------------
// Until v26 every constant in this file was the same everywhere in the world.
// A desert plain and an alpine range got the same edge density, the same
// calibre range, the same crevice rate and the same entrance rate, because
// nothing here read anything about the place. That is the single complaint the
// owner's brief opens with -- caves "should be influenced by the biome and
// terrain type" -- and W5 is the wave that answers it.
//
// TWO FIELDS, BOTH ALREADY IN THE AMPLIFIER, NO NEW DATA:
//
//   * RELIEF -- carrier.h's relief30 (the rise across a 30 m physical
//     baseline), the same statistic the detail gate is conditioned on. It
//     spans 18x between the plains and alpine exemplars (872 mm vs 15,692 mm,
//     carrier.h's kReliefRefMm calibration), which is the property that makes
//     it usable as a landform discriminator at all.
//   * TEMPERATURE -- the climate raster's bio_1 byte (climate.h), read at the
//     node, used for ONE thing: frost shattering, which raises the fracture
//     (crevice) budget in cold ground.
//
// DELIBERATELY ABSENT: lithology and flow (both W6), and PRECIPITATION, which
// is excluded by the owner's own answer to Q4 -- caves stay dry until the
// watershed work lands, and a precip term is the first thing that would
// smuggle wetness back in.
//
// WHERE THE FIELD IS SAMPLED, AND WHY IT IS THE NODE AND NOT THE COLUMN.
// Every term below is a property of a lattice NODE or of an EDGE, never of the
// querying column. That is not a performance choice, it is a correctness one:
// an edge is seen by columns on both sides of it, and a gate keyed on the
// querying column's own field would make the same edge exist from one side and
// not from the other. Keying on the node makes each decision a pure function of
// (seed, i, j) and the terrain at ONE place, so both sides agree by
// construction. The sample point is the node's LATTICE ANCHOR (i * lattice,
// j * lattice) rather than its jittered position, so the field does not depend
// on the node hash and re-jittering nodes would not move it.
//
// CALIBRE STAYS CONTINUOUS ANYWAY. A per-node radius scale would be a visible
// step at a node if the radius were constant along an edge -- but since v24 it
// is INTERPOLATED between the two nodes' values and the edge's own mid draw, so
// scaling the control values leaves the passage C0 exactly as it was. The
// discrete gates (edge existence, crevice presence, entrance presence) are
// discrete decisions anyway; the field moves their RATE, and a rate has no
// seam.
//
// CONNECTIVITY IS NOT AT RISK, AND THAT IS THE WHOLE POINT OF THE BACKBONE.
// The density term only ever moves the 1-in-N gate on NON-backbone edges. The
// backbone is unconditional, so the "connected by construction, for every seed,
// with no thresholds to tune" argument at the top of this file survives a
// density field that goes to zero. A field coupling on a noise-threshold cave
// system would have to be tuned against disconnection; here it cannot be.
//
// THE ROOF GUARANTEE BECAME STRUCTURAL INSTEAD OF STATIC. v26 proved "no tunnel
// voxel is shallower than 6.2 m" from two constants (min axis depth minus max
// radius). Widening the calibre ceiling to 4.0 m breaks that arithmetic -- 9 m
// of minimum axis depth cannot cover a 4 m radius against a 6 m clamp. The
// replacement is a per-control-value CAP (caveScaledRadiusMm): a tube may be no
// wider at a control point than that point's own cover allows. Because depth
// and radius are interpolated along a sub-segment at the SAME parameter, a
// bound that holds at both endpoints holds everywhere between them by
// linearity -- so the guarantee is still structural, it is just proved per
// segment instead of per constant. See the static_asserts and
// cave_surface_integrity_roof_thickness.

#include "voxelcore/climate.h"
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
// instead of being one extruded bore.
//
// v27 (W5): this is the RAW draw. The field coupling multiplies it by
// kCaveCalibre*Q10 (0.875x on a plain, 1.428x in the mountains), which puts the
// realised range at [1.05, 2.45] m on flat ground and [1.71, 4.00] m on alpine
// ground. That is the plan's "[0.8, 4.0] m with the field coupling deciding
// where the wide end lives" — with the LOW end held at 1.05 m rather than
// 0.80 m, because a crevice must stay thinner than the thinnest tunnel or it
// can exist outside its own tube's footprint (static_assert below). The
// coupling was worth stating rather than discovering, and it is still the
// binding one.
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

// v26 and earlier: non-backbone edges opened when the top 2 bits of their hash
// were zero (1 in 4). v27 replaced the fixed mask with a field-driven threshold
// (kCaveEdgeGate*Q20 + caveGateOpen), whose neutral setting tests the SAME two
// bits — so this constant is kept as the statement of what "neutral" means and
// as the thing the bit-identity static_assert below is written against.
inline constexpr uint64_t kCaveEdgeGateMask = 3;
static_assert(kCaveEdgeGateMask == 3, "the v26 gate this file's control reproduces");

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
// CANDIDATE density is unchanged and always has been: one candidate node per
// 4x4 lattice cells (102.4 m square). v25 and v26 gated those 1 in 4, giving
// roughly one entrance per 205 m square everywhere in the world. v27 (W5) makes
// the GATE the field's: 1 in 8 on a desert plain (~290 m spacing) up to 1 in 2
// in the mountains (~145 m). The candidate set, the backbone-crossing anchor
// and all three §2.3 guarantees are untouched — only how many candidates open.
inline constexpr int64_t kCaveShaftNodeMask = 3;  // (i & mask) == 0 && (j & mask) == 0
inline constexpr uint64_t kCaveShaftGateMask = 3; // v26's 1-in-4; the v27 neutral rate
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

inline constexpr int64_t kCrevHalfThickMaxMm = kCrevHalfThickMinMm + kCrevHalfThickSpanMm;

// ===========================================================================
// THE FIELD (v27, plan W5) — the two inputs, the two gates they drive, and the
// six numbers those gates hand to the rest of this file.
// ===========================================================================

// One sample of the coupling field, taken at a lattice node's ANCHOR. Both
// members come from data the amplifier already reads per column; nothing here
// is a new raster, a new channel or a new tile.
struct CaveField {
    // climate.h's bio_1 byte. The default is ClimateSample's own missing-tile
    // value (~10 C), so a world with no climate tiles gates as temperate rather
    // than as frozen — a missing tile must not manufacture fracture caves.
    int32_t tempU8 = climateTempU8FromDegC(10);
    // carrier.h's relief30: the rise across a 30 m physical baseline, in mm.
    // Exactly zero on dead-flat ground.
    int32_t reliefMm = 0;
};

// --- the relief ramp --------------------------------------------------------
// 0 at or below the low knee, 1024 at or above the high one, smoothstepped
// between. Both knees are stated INDEPENDENTLY of carrier.h's kReliefRefMm
// even though the high one carries the same value: kReliefRefMm calibrates the
// DETAIL gate, and a retune of surface roughness must not silently move every
// cave in the world.
//
// The numbers come from carrier.h's own measurement of the two exemplars on the
// pinned world: plains 872 mm, alpine 15,692 mm. The low knee sits above the
// plains figure so a plain lands on the floor of every gate below, and the high
// knee sits at the alpine figure so mountains saturate.
inline constexpr int64_t kCaveReliefLoMm = 2000;
inline constexpr int64_t kCaveReliefHiMm = 16000;

// Integer smoothstep 3t^2 - 2t^3 in Q10, clamped. Used by both ramps so the
// gates share one curve shape and one rounding.
constexpr int64_t caveSmoothQ10(int64_t tQ10) {
    const int64_t c = clampi64(tQ10, 0, 1024);
    return (c * c * (3 * 1024 - 2 * c)) / (1024 * 1024);
}
static_assert(caveSmoothQ10(0) == 0);
static_assert(caveSmoothQ10(1024) == 1024);
static_assert(caveSmoothQ10(512) == 512);
static_assert(caveSmoothQ10(-5000) == 0 && caveSmoothQ10(50000) == 1024,
              "the smoothstep must clamp, not wrap: the relief argument is raster-derived "
              "and nothing upstream bounds it");

constexpr int64_t caveReliefQ10(int64_t reliefMm) {
    if (reliefMm <= kCaveReliefLoMm) return 0;
    if (reliefMm >= kCaveReliefHiMm) return 1024;
    return caveSmoothQ10((reliefMm - kCaveReliefLoMm) * 1024 /
                         (kCaveReliefHiMm - kCaveReliefLoMm));
}

// --- the frost ramp ---------------------------------------------------------
// FULL frost weight at or below -2 C mean annual temperature, none at or above
// +12 C. Stated in physical units through climate.h's encoder for the reason
// that header exists: a bare u8 threshold is calibrated against whatever
// distribution its author happened to be looking at.
inline constexpr int32_t kCaveFrostColdU8 = climateTempU8FromDegC(-2);
inline constexpr int32_t kCaveFrostWarmU8 = climateTempU8FromDegC(12);
static_assert(kCaveFrostColdU8 < kCaveFrostWarmU8, "the frost ramp runs cold -> warm");

constexpr int64_t caveFrostQ10(int64_t tempU8) {
    if (tempU8 <= kCaveFrostColdU8) return 1024;
    if (tempU8 >= kCaveFrostWarmU8) return 0;
    return caveSmoothQ10((kCaveFrostWarmU8 - tempU8) * 1024 /
                         (kCaveFrostWarmU8 - kCaveFrostColdU8));
}

// The FRACTURE budget: relief plus a weighted frost boost, clamped.
//
// THE PLAN'S TABLE SAYS "cold (bio_1) + high relief -> G3 up", i.e. a PRODUCT,
// and this is deliberately a weighted SUM instead. A product is zero wherever
// relief is zero, which would make the temperature term invisible on flat
// ground — and "tundra vs savanna" is exactly the pair W5 has to show a
// difference on. The sum keeps relief dominant (it alone can saturate the
// budget; frost alone can reach at most half of it) while letting a cold flat
// place out-fracture a hot flat one, which is the frost-shattering reading and
// is what the Verify line asks to see.
inline constexpr int64_t kCaveFrostBoostQ10 = 512;
constexpr int64_t caveFractureQ10(int64_t reliefQ10, int64_t frostQ10) {
    return clampi64(reliefQ10 + frostQ10 * kCaveFrostBoostQ10 / 1024, 0, 1024);
}

// --- the gate currency ------------------------------------------------------
// Every hash GATE below is a threshold on a 20-bit slice of an existing hash,
// so a rate is a plain integer in [0, 2^20] and lerps without a second unit.
inline constexpr int64_t kCaveGateOne = 1 << 20;

// G1 DENSITY (non-backbone edges): 1-in-8 on a plain, 1-in-2 in the mountains.
// The NEUTRAL value is exactly 1-in-4 — and because the 20-bit slice is chosen
// so that its top two bits are the two bits v26 tested, a neutral gate opens
// the BIT-IDENTICAL edge set v26 opened. That is what makes the W5 control an
// exact single-term difference rather than merely a rate match.
inline constexpr int64_t kCaveEdgeGateLoQ20 = kCaveGateOne / 8;
inline constexpr int64_t kCaveEdgeGateHiQ20 = kCaveGateOne / 2;
inline constexpr int64_t kCaveEdgeGateNeutralQ20 = kCaveGateOne / 4;

// G1 CALIBRE: the multiplier on every tube radius control value.
inline constexpr int64_t kCaveCalibreLoQ10 = 896;  // 0.875x — desert plains
inline constexpr int64_t kCaveCalibreHiQ10 = 1462; // 1.428x — 2.8 m draw -> 4.0 m
inline constexpr int64_t kCaveRadiusScaledMaxMm = kCaveRadiusMaxMm * kCaveCalibreHiQ10 / 1024;
inline constexpr int64_t kCaveRadiusScaledMinMm = kCaveRadiusMinMm * kCaveCalibreLoQ10 / 1024;
// The slack the depth-aware roof cap carries over kCaveRoofMinMm. One voxel,
// and it exists for a rounding reason rather than a design one: both the axis
// depth and the radius are realised with floorDiv along a sub-segment, and each
// can round down by up to 1 mm, so a cap written with zero slack could be
// violated by a millimetre at an interior point. 100 mm is two orders more than
// that needs and still nothing against the 6 m clamp.
inline constexpr int64_t kCaveCalibreRoofSlackMm = 100;

// G3 FRACTURE RATE: 1-in-16 where nothing fractures, up to 1-in-3 in cold,
// steep ground. Neutral is 1-in-8, v26's fixed rate.
inline constexpr int64_t kCrevGateLoQ20 = kCaveGateOne / 16;
inline constexpr int64_t kCrevGateHiQ20 = kCaveGateOne / 3;
inline constexpr int64_t kCrevGateNeutralQ20 = kCaveGateOne / 8;

// G3 FRACTURE SIZE: a multiplier on the slab's UPWARD reach only.
//
// Upward only, and the asymmetry is forced rather than chosen. The downward
// reach is what sets the cave pass's deepest possible carve (see
// kCaveDeepestCarveMm) — it has been the binding term there since crevices
// shipped — so scaling it would move the world's whole carve envelope for a
// cosmetic gain. The upward reach is already clamped per column against the
// roof, so scaling it up is free: a fissure reaches further toward daylight
// where the rock is fractured, and pinches where it is not.
inline constexpr int64_t kCrevUpLoQ10 = 768;  // 0.75x
inline constexpr int64_t kCrevUpHiQ10 = 1408; // 1.375x

// G5 ENTRANCE RATE: 1-in-8 on a plain, 1-in-3 in the mountains — the "more
// mouths in a mountainside" half of the headline. Neutral is 1-in-4, and the
// slice is again chosen so neutral reproduces v26's open-site set exactly.
inline constexpr int64_t kCaveShaftGateLoQ20 = kCaveGateOne / 8;
inline constexpr int64_t kCaveShaftGateHiQ20 = kCaveGateOne / 3;
inline constexpr int64_t kCaveShaftGateNeutralQ20 = kCaveGateOne / 4;

// G5 MOUTH BUDGET — TRIED, MEASURED, AND DROPPED. Recorded rather than
// silently absent, because the plan's coupling table lists it.
//
// A relief multiplier on the entrance cavity's footprint RADIUS was written and
// measured at 1.25x on saturated relief. Radius scales area by its square, so
// 1.25x is 1.5625x of ground over a cavity, and the two roof-integrity bounds
// cave_surface_integrity_roof_thickness has enforced since v25 went from
// comfortable to failing/near-failing on the same 102.4 m box centred on a real
// entrance:
//
//     footprint over a cavity   3.96%  ->  6.18%   (bound 5%   — FAILS)
//     open to the sky           1.17%  ->  1.83%   (bound 2%   — 9% of margin left)
//
// Two things follow, and both argue the same way. First, the W5 line asks for
// G5 *density*, not G5 size; the plan's "horizontal-mouth budget" is delivered
// by the rate plus the terrain clip that already decides mouth SHAPE (v25), so
// nothing in the brief is lost. Second, buying a size term by loosening a
// roof-integrity bound in the same wave that multiplies what the bound guards
// is the wrong trade — the bound is the only thing standing between "more
// entrances in mountains" and "the mountains are a colander".
//
// If a mouth-size term is wanted later it should come with a re-derived bound
// and its own measurement, not by widening this one. The rate ceiling above is
// 1-in-3 rather than 1-in-2 for the same reason, one step down: at 1-in-2 the
// footprint reaches 4.7% against the same 5% bound, which is margin too thin to
// ship on a statistic this noisy.

// Everything the field decides, in one struct — so there is exactly one place
// to read what the coupling does, and exactly one thing for the A/B control to
// neutralise (cave_families.h). Nothing else in this file reads CaveField.
struct CaveGates {
    int32_t edgeGateQ20 = static_cast<int32_t>(kCaveEdgeGateNeutralQ20);
    int32_t calibreQ10 = 1024;
    int32_t crevGateQ20 = static_cast<int32_t>(kCrevGateNeutralQ20);
    int32_t crevUpQ10 = 1024;
    int32_t entranceGateQ20 = static_cast<int32_t>(kCaveShaftGateNeutralQ20);
};

// THE CONTROL VALUE, and it is a worldgen constant rather than a test fixture:
// a default-constructed CaveGates reproduces v26's rates, and for the edge and
// entrance gates v26's exact draws. cave_families.h's A/B feeds this in.
inline constexpr CaveGates kCaveGatesNeutral{};

constexpr int64_t caveLerpQ10(int64_t lo, int64_t hi, int64_t tQ10) {
    return lo + (hi - lo) * tQ10 / 1024;
}

constexpr CaveGates caveGatesFromField(const CaveField& f) {
    const int64_t rQ = caveReliefQ10(f.reliefMm);
    const int64_t fracQ = caveFractureQ10(rQ, caveFrostQ10(f.tempU8));
    CaveGates g;
    g.edgeGateQ20 =
        static_cast<int32_t>(caveLerpQ10(kCaveEdgeGateLoQ20, kCaveEdgeGateHiQ20, rQ));
    g.calibreQ10 = static_cast<int32_t>(caveLerpQ10(kCaveCalibreLoQ10, kCaveCalibreHiQ10, rQ));
    g.crevGateQ20 = static_cast<int32_t>(caveLerpQ10(kCrevGateLoQ20, kCrevGateHiQ20, fracQ));
    g.crevUpQ10 = static_cast<int32_t>(caveLerpQ10(kCrevUpLoQ10, kCrevUpHiQ10, fracQ));
    g.entranceGateQ20 =
        static_cast<int32_t>(caveLerpQ10(kCaveShaftGateLoQ20, kCaveShaftGateHiQ20, rQ));
    return g;
}

// Crevices must stay thinner than the thinnest tunnel: their xy accept
// corridor (radius t) is then always a strict subset of the tube's own
// (radius r), so a crevice can never exist somewhere its own tube doesn't —
// the containment the header comment's connectivity argument relies on.
//
// v27 restates it against the SCALED minimum, because the calibre gate can now
// shrink a plain's tubes below the raw draw. 800 mm against 1050 mm: the gate's
// low knee cannot go below 683/1024 without breaking this, which is the real
// reason the plan's 0.8 m floor is not reachable here.
static_assert(kCrevHalfThickMaxMm < kCaveRadiusScaledMinMm,
              "crevices must stay thinner than the thinnest SCALED tunnel radius, or a "
              "crevice could exist outside its own tube's xy footprint");
// The other way a tube can get thin is the depth-aware roof cap. The shallowest
// possible axis is a node at kCaveNodeDepthMinMm, so the cap can never demand
// less than this — and it must still clear the crevice.
static_assert(kCaveNodeDepthMinMm - kCaveRoofMinMm - kCaveCalibreRoofSlackMm >
                  kCrevHalfThickMaxMm,
              "the depth-aware calibre cap must never squeeze a tube below its own "
              "crevice's half-thickness");

// --- structural invariants, checked at compile time -------------------------

// The 3x3 candidate block is only exhaustive while a tube cannot reach more
// than one lattice cell sideways off its axis' bounding box. Since v24 the axis
// is a polyline whose waypoint may sit kCaveWaypointLatMm outside that box, so
// the waypoint excursion is part of the reach and part of this bound.
static_assert((kCaveRadiusScaledMaxMm + kCaveWaypointLatMm) * 2 < kCaveLatticeMm,
              "cave tube reach (max SCALED radius plus the waypoint's lateral excursion) "
              "must stay well under one lattice cell, or the 3x3 candidate-node block in "
              "caveColumnFor stops being exhaustive");
// ROOF. Until v26 this was a two-constant subtraction: min axis depth minus max
// radius. v27's 4.0 m calibre ceiling makes that arithmetic false (9 m of cover
// cannot hold a 4 m tube above a 6 m clamp), so the guarantee moved into
// caveScaledRadiusMm's per-control-value cap and the linearity argument in the
// header comment. What is still static is the WORST CASE THE CAP ITSELF CAN
// PRODUCE: at the shallowest legal axis the cap allows exactly this much
// radius, and that must still clear the clamp.
static_assert(kCaveNodeDepthMinMm - (kCaveNodeDepthMinMm - kCaveRoofMinMm -
                                     kCaveCalibreRoofSlackMm) > kCaveRoofMinMm,
              "the depth-aware calibre cap must leave the roof clamp unreachable — the "
              "clamp is a backstop, not the mechanism");
// FLOOR — and the old form of this assert was WRONG, quietly, since M4 cave
// pass v2 shipped crevices.
//
// It bounded the deepest carve by the TUBE alone (axis + dip + radius), and
// asserted that against a 40 m bedrock top: the PRE-v5 amplifier minimum, kept
// deliberately after the v5 move to a 180-220 m band because asserting against
// the shallower figure is a strictly stronger statement. But a crevice reaches
// kCrevDownMinMm + kCrevDownSpanMm = 6 m below its own tube axis, more than
// twice the widest tube radius, so the real deepest carve has been 41.0 m — one
// metre PAST the figure the assert claimed — for three worldgen versions. It
// was harmless (bedrock has been >= 180 m since v5) and it was never checked,
// which is the part worth recording.
//
// v27 states the envelope explicitly, over every construct that can carve, and
// asserts THAT. The tunnel term grows (3.998 m of scaled radius against 2.8 m)
// and the crevice term does not move at all, so the envelope is unchanged at
// 41.0 m and the widened calibre costs the world nothing in depth.
inline constexpr int64_t kCaveDeepestAxisMm =
    kCaveNodeDepthMinMm + kCaveNodeDepthSpanMm + kCaveWaypointDipMm;
inline constexpr int64_t kCaveDeepestTunnelMm = kCaveDeepestAxisMm + kCaveRadiusScaledMaxMm;
inline constexpr int64_t kCaveDeepestCreviceMm =
    kCaveDeepestAxisMm + kCrevDownMinMm + kCrevDownSpanMm;
inline constexpr int64_t kCaveDeepestCarveMm =
    kCaveDeepestTunnelMm > kCaveDeepestCreviceMm ? kCaveDeepestTunnelMm : kCaveDeepestCreviceMm;
static_assert(kCaveDeepestCarveMm + kCaveBedrockMarginMm < 45000,
              "cave-pass geometry (tube AND crevice) must keep itself out of bedrock — the "
              "bedrock margin is a backstop, not the mechanism. 45 m is four times clear of "
              "the 180 m the amplifier's band has bottomed out at since v5.");

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
//
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
// v27 (plan W5). The crevice GATE needs its own channel, and this is the one
// place W5 could not reuse a bit slice of an existing hash.
//
// The edge and entrance gates could: v26 tested two named bits of CH_CAVE_EDGE
// / CH_CAVE_SHAFT, and a 20-bit window POSITIONED so those two bits are its top
// two makes "threshold == 1/4" bit-identical to v26 while every other threshold
// is a smooth widening of the same comparison. CH_CREVICE has no room for that
// trick: its 64 bits are already spent on three 20-bit geometry fields plus the
// 3-bit gate at 61..63, so any 20-bit window wide enough for a rate would
// overlap the down-reach field and correlate crevice PRESENCE with crevice
// SIZE. That is a real artifact, not a coincidence to leave alone (see
// CH_CAVE_NODE_RADIUS for the same call made the same way), so the gate gets a
// clean channel and the W5 control is rate-identical to v26 here rather than
// bit-identical. Stated rather than glossed.
inline constexpr uint32_t CH_CAVE_CREV_GATE = 56;

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

// The 20-bit slice a field-driven gate is compared against. The SHIFT is the
// load-bearing part: `(h >> 30) & 0xFFFFF` is bits 30..49, whose top two bits
// are bits 48 and 49 — the exact pair v26's `((h >> 48) & 3) == 0` tested. So
// `caveGateOpen(h, kCaveGateOne / 4)` is bit-for-bit v26's 1-in-4 gate, and
// every other threshold widens or narrows the same comparison continuously.
inline constexpr int32_t kCaveGateShift = 30;
constexpr bool caveGateOpen(uint64_t h, int32_t shift, int64_t thresholdQ20) {
    return static_cast<int64_t>((h >> shift) & 0xFFFFFu) < thresholdQ20;
}
static_assert(kCaveEdgeGateNeutralQ20 == kCaveGateOne / 4 &&
                  kCaveShaftGateNeutralQ20 == kCaveGateOne / 4,
              "the neutral edge/entrance thresholds are what make the W5 control reproduce "
              "v26's draws exactly; they are 1/4 of the gate currency because v26 tested two "
              "bits, and moving either breaks the bit-identity claim in cave_families.h");

// dir 0 = the +x edge out of (i, j), dir 1 = the +y edge. Backbone edges are
// unconditional (that is what makes the network connected for every seed);
// everything else is a hash gate whose RATE is the field's (v27) — 1-in-8 on a
// plain, 1-in-4 at the neutral control, 1-in-2 in the mountains.
//
// `gateQ20` belongs to the edge's SOURCE node (i, j), never to the querying
// column: an edge is looked at from both sides and both sides must agree.
constexpr bool caveEdgeExists(uint64_t seed, int64_t i, int64_t j, int32_t dir,
                              int64_t gateQ20) {
    if (dir == 0 && (j & kCaveBackboneMask) == 0) return true;
    if (dir == 1 && (i & kCaveBackboneMask) == 0) return true;
    return caveGateOpen(hash2(seed, i, j * 2 + dir, CH_CAVE_EDGE), kCaveGateShift, gateQ20);
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

// THE CALIBRE GATE AND THE ROOF CAP, in one place because they are one rule.
//
// `rawMm` is the hashed draw, `calibreQ10` the field's multiplier, and
// `axisDepthMm` the depth of the control point this radius belongs to. The cap
// is what replaced v26's two-constant roof static_assert: a control point may
// be no wider than its own cover allows, so the passage cannot reach the roof
// clamp no matter how generous the field is.
//
// WHY IT IS SOUND ALONG THE WHOLE SEGMENT, not just at the control points:
// caveColumnFromLattice interpolates the axis depth and the radius at the SAME
// parameter num/den, so both are affine in that parameter and
// `depth - radius >= kCaveRoofMinMm + slack` at both endpoints implies it
// everywhere between. The slack absorbs the two floorDivs.
constexpr int64_t caveScaledRadiusMm(int64_t rawMm, int64_t calibreQ10, int64_t axisDepthMm) {
    const int64_t scaled = rawMm * calibreQ10 / 1024;
    const int64_t cap = axisDepthMm - kCaveRoofMinMm - kCaveCalibreRoofSlackMm;
    return scaled < cap ? scaled : cap;
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
// v26 and earlier: the crevice gate was the top 3 bits of caveCreviceHash.
// v27 moved it to its own channel and its own field-driven threshold (see
// CH_CAVE_CREV_GATE); this is kept as the statement of the neutral rate the
// control reproduces, and is what kCrevGateNeutralQ20 is derived against.
constexpr bool caveCreviceGateOpenV26(uint64_t h) {
    return ((h >> 61) & kCrevGateMask) == 0;
}
static_assert(kCrevGateNeutralQ20 == kCaveGateOne / 8,
              "the neutral crevice threshold must reproduce v26's 1-in-8 RATE (it cannot "
              "reproduce its draws — the gate moved channel; see CH_CAVE_CREV_GATE)");

// The entrance gate on its own, for instruments that enumerate open SITES
// without reducing a column (test_caves, vxc_caveprobe). It exists so those do
// not hand-roll the predicate: three of them hand-rolled v26's two-bit test,
// and a hand-rolled copy of a gate that has just become field-driven is exactly
// how an instrument ends up counting a world nobody is building.
constexpr bool caveEntranceGateOpen(uint64_t seed, int64_t i, int64_t j,
                                    const CaveGates& gates) {
    return caveGateOpen(hash2(seed, i, j, CH_CAVE_SHAFT), kCaveGateShift, gates.entranceGateQ20);
}

constexpr bool caveCreviceGateOpen(uint64_t seed, int64_t i, int64_t j, int32_t dir,
                                   int64_t gateQ20) {
    return caveGateOpen(hash2(seed, i, j * 2 + dir, CH_CAVE_CREV_GATE), 44, gateQ20);
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
    int32_t radiusMm = 0;  // MID calibre, SCALED and roof-capped; 0 == no edge here
    uint64_t crevHash = 0; // caveCreviceHash for it (meaningless if radiusMm == 0)
    CaveWaypoint way;      // v24 interior waypoint (meaningless if radiusMm == 0)
    // v27: the crevice's GATE and SIZE now come from the source node's field,
    // so both are decided here rather than per column. The gate moved out of
    // `crevHash` entirely (it has its own channel — see CH_CAVE_CREV_GATE);
    // crevHash still supplies all three geometry draws exactly as it did.
    bool crevOpen = false;
    int32_t crevUpQ10 = 1024;
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
                                            const CaveNode& node, const SurfaceFn& surfaceAt,
                                            const CaveGates& gates) {
    CaveEntranceSite s;
    const uint64_t hs = hash2(seed, i, j, CH_CAVE_SHAFT);
    // v27: a field-driven threshold on bits 30..49 replaces v26's two-bit test
    // on bits 48..49. At the neutral rate the two are the SAME predicate (see
    // caveGateOpen), and bits 30..47 were unused, so widening the window
    // correlates the gate with nothing — in particular not with the throat
    // radius, which lives in bits 0..19 of this same hash.
    if (!caveGateOpen(hs, kCaveGateShift, gates.entranceGateQ20)) return s;
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
    // v27 note: the cavity's FOOTPRINT is deliberately NOT field-scaled. See
    // the "G5 MOUTH BUDGET" block above for the measurement that dropped it.
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
// v27: `gatesAt(i, j) -> CaveGates` supplies the field coupling for lattice node
// (i, j). Both callers of this form are in-tree and deliberately so: the
// SHIPPING path is caveLatticeFor below, which derives the gates from the field
// at the node's anchor; the CONTROL is cave_families.h, which passes
// kCaveGatesNeutral and gets v26's density and calibre out of the v27 world.
// Splitting at the gates rather than at the field is what makes that control an
// exact single-term difference — see the header comment there.
template <typename SurfaceFn, typename GatesFn>
constexpr CaveLattice caveLatticeForGates(uint64_t seed, int64_t ci, int64_t cj,
                                          const SurfaceFn& surfaceAt, const GatesFn& gatesAt) {
    CaveLattice L;

    // 4x4 node block: the 3x3 candidate SOURCE nodes plus the +x/+y endpoints
    // they need. Each node's own gates are computed once here and used for
    // everything that node owns — its calibre, its two outgoing edges, its
    // crevices and (if it is a backbone crossing) its entrance.
    CaveGates gates[16];
    for (int32_t dj = 0; dj < 4; ++dj)
        for (int32_t di = 0; di < 4; ++di) {
            const int32_t slot = di + 4 * dj;
            const int64_t i = ci - 1 + di;
            const int64_t j = cj - 1 + dj;
            L.nodes[slot] = caveNode(seed, i, j);
            gates[slot] = gatesAt(i, j);
            // The node's calibre is capped against the NODE's own depth: this
            // control point is where the passage is shallowest, so it is where
            // the cap actually binds.
            L.nodeRadiusMm[slot] = static_cast<int32_t>(caveScaledRadiusMm(
                caveNodeRadiusMm(seed, i, j), gates[slot].calibreQ10, L.nodes[slot].depthMm));
        }

    for (int32_t dj = 0; dj < 3; ++dj)
        for (int32_t di = 0; di < 3; ++di) {
            const int64_t i = ci - 1 + di;
            const int64_t j = cj - 1 + dj;
            const CaveGates& g = gates[di + 4 * dj];
            for (int32_t dir = 0; dir < 2; ++dir) {
                if (!caveEdgeExists(seed, i, j, dir, g.edgeGateQ20)) continue;
                CaveLatticeEdge& e = L.edges[(di + 3 * dj) * 2 + dir];
                e.crevHash = caveCreviceHash(seed, i, j, dir);
                e.crevOpen = caveCreviceGateOpen(seed, i, j, dir, g.crevGateQ20);
                e.crevUpQ10 = g.crevUpQ10;
                const CaveNode& a = L.nodes[di + 4 * dj];
                const CaveNode& b = (dir == 0) ? L.nodes[di + 1 + 4 * dj]
                                               : L.nodes[di + 4 * (dj + 1)];
                e.way = caveWaypoint(seed, i, j, dir, a, b);
                // The MID calibre is capped against the WAYPOINT's depth, which
                // is the control point it belongs to. radiusMm must be written
                // LAST of the three geometry fields only in the sense that it is
                // the existence sentinel; the waypoint it depends on is computed
                // above.
                e.radiusMm = static_cast<int32_t>(caveScaledRadiusMm(
                    caveEdgeRadiusMm(seed, i, j, dir), g.calibreQ10, e.way.depthMm));
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
                caveEntranceSite(seed, i, j, L.nodes[slot], surfaceAt, gates[slot]);
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

// The lattice ANCHOR of node (i, j) — where the coupling field is sampled. Not
// the jittered node position: see the header comment.
constexpr int64_t caveNodeAnchorMm(int64_t i) { return i * kCaveLatticeMm; }

// THE SHIPPING COMPOSITION. `fieldAt(xMm, yMm) -> CaveField` is the second
// terrain callback the cave pass takes, alongside surfaceAt, and it is written
// against exactly the same contract: it must come from the very functions this
// column's own climate and relief came from. amplifier.cpp supplies
// Amplifier::caveFieldAt and worldgen.ush mirrors that call.
//
// SIXTEEN taps per lattice CELL — one per node of the 4x4 block, and a cell is
// 65'536 voxel columns, so the CPU's lattice memo amortises them to nothing.
// The shader cannot memoise and pays them per column; that is the same trade
// the entrance's surface tap already makes, and the reason the field is keyed
// on the anchor (a pure function of (i, j)) rather than on the jittered node is
// that it makes the tap independent of every hash in this file.
template <typename SurfaceFn, typename FieldFn>
constexpr CaveLattice caveLatticeFor(uint64_t seed, int64_t ci, int64_t cj,
                                     const SurfaceFn& surfaceAt, const FieldFn& fieldAt) {
    return caveLatticeForGates(seed, ci, cj, surfaceAt, [&](int64_t i, int64_t j) {
        return caveGatesFromField(fieldAt(caveNodeAnchorMm(i), caveNodeAnchorMm(j)));
    });
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
                if (edge.crevOpen) {
                    const int64_t tMm =
                        kCrevHalfThickMinMm +
                        static_cast<int64_t>(((crevH & 0xFFFFFu) * static_cast<uint64_t>(kCrevHalfThickSpanMm)) >> 20);
                    if (ex * ex + ey * ey <= tMm * tMm) {
                        // v27: the UPWARD reach is the field-scaled one — a
                        // fracture propagates further toward daylight where the
                        // rock is fractured. The downward reach is deliberately
                        // NOT scaled (see kCrevUpLoQ10): it is the term that
                        // sets kCaveDeepestCarveMm.
                        const int64_t hUpMm =
                            (kCrevUpMinMm + static_cast<int64_t>((((crevH >> 20) & 0xFFFFFu) *
                                                                  static_cast<uint64_t>(kCrevUpSpanMm)) >> 20)) *
                            edge.crevUpQ10 / 1024;
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
template <typename SurfaceFn, typename FieldFn>
constexpr CaveColumn caveColumnFor(uint64_t seed, int64_t vx, int64_t vy, int32_t surfaceMm,
                                   const SurfaceFn& surfaceAt, const FieldFn& fieldAt) {
    CaveColumn out;
    if (surfaceMm < kCaveMinSurfaceMm) return out;
    const int64_t ci = floorDiv(vx * kVoxelSizeMm, kCaveLatticeMm);
    const int64_t cj = floorDiv(vy * kVoxelSizeMm, kCaveLatticeMm);
    return caveColumnFromLattice(seed, caveLatticeFor(seed, ci, cj, surfaceAt, fieldAt), vx, vy,
                                 surfaceMm);
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
