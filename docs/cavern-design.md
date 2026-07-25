# Caverns, crevices and underground water — design (M4 cave pass v2)

Status: DESIGN (research + architecture; no worldgen code rides with this doc).
Author: architect session, 2026-07-21. Base: `main` post-PR-#54
(kWorldGenVersion 4, jittered-lattice tunnel network landed).

Matt's ask: *"can we have underground caverns and crevices like Minecraft has.
Do research for how to best generate these procedurally. Underground water
bodies as well."*

## 0. The tension this design resolves

The landed cave pass (`voxelcore/caves.h`) is fast because it is a
**per-column reduction**: all lattice/hash geometry runs once per column and
the per-voxel test is one multiply and one compare against two int32s
(`dz*dz < marginSq`). A large cavern is a genuinely 3D volume, which looks
like it breaks that reduction.

The claim this design defends: **it doesn't have to.** Any solid-or-air
field restricted to a single column is a set of z-intervals, and for the right
primitive — ellipsoidal blobs — the interval bound is *exactly the existing
per-voxel test*: an ellipsoid with vertical semi-axis `rz` intersected with a
column at horizontal distance-squared `dxy²` gives the interval
`dz² < rz²·(rxy² − dxy²)/rxy²`, i.e. one per-column division producing one
`marginSq`, then the same `dz*dz < marginSq` per voxel. Caverns therefore ride
the existing machinery; the design problem is **placement and anchoring** so
that (a) the common-case column pays almost nothing, (b) connectivity stays
structural rather than emergent, and (c) the integer-only CPU/GPU mirror
survives unchanged in shape.

## 1. Survey of approaches (prior art)

### 1.1 Minecraft 1.18+ noise caves — the modern reference

Three families, all sublevel/level sets of 3D Perlin-class noise, evaluated as
part of the density function (sources: minecraft.wiki World generation;
Alan Zucconi, "The World Generation of Minecraft"; jacobsjo's aquifer gist).

- **Cheese caves**: carve where a low-frequency 3D noise exceeds a
  "hollowness" threshold (the interior of the noise's "white regions").
  Produces *large open caverns* — multi-chunk rooms with noise pillars.
- **Spaghetti caves**: carve near the **zero level-set** of noise
  (`|n| < thickness`, in vanilla the min of two such fields) — the
  thin shell of a 3D isosurface, intersected down to winding 2D-ish sheets and
  tubes. Produces *long tunnels*.
- **Noodle caves**: same construction, tighter thickness and two intersected
  fields — *thin claustrophobic crawlways* (1–5 blocks).
- **Aquifers**: underground water WITHOUT flooding every cave below sea level.
  Space is cut into **16×40×16 fluid-level cells**, each hash-assigned a local
  water level (snapped to multiples of 3); a coarser 64×40×64 grid picks the
  fluid (water vs lava, lava only deep down); a "barrier" noise solidifies
  boundary blocks between adjacent cells of different level so lakes at
  different heights don't leak into each other. Water is placed at generation
  time as static source blocks — **it does not flow until disturbed**, at
  which point Minecraft's (non-conserving) fluid update takes over.
- **Legacy carvers** (pre-1.18, still present as a second mechanism):
  agent-based **Perlin worms** — random walks from per-chunk spawn points
  carving spheres/ellipsoids along the way. **Ravines/canyons** are the same
  carver with a tall, narrow cross-section — i.e. Minecraft's "crevices" are
  a *parameter variant of the tunnel carver*, not a separate system.

What 1.18 buys: the three noise families layer into a system where cheese
gives destination rooms, spaghetti connects them, noodle adds texture, and
aquifers make water local instead of global. What it costs: **per-voxel (or
per-interpolation-cell) 3D noise evaluation** — Mojang mitigates with
trilinear interpolation of a coarse 4×8×4 sample grid — and **emergent,
unguaranteed connectivity** (a cheese room can be sealed; Mojang doesn't care,
players dig; our M6 NPCs and water routing do care).

### 1.2 Other techniques

- **3D Worley/cellular noise** sublevel sets: rounder, chamber-like voids
  (distance-to-feature-point < t is literally a union of ball-ish regions).
  Same costs as cheese: per-voxel 3D evaluation, emergent connectivity.
- **Cellular-automata smoothing** (classic roguelike caves): iterate a
  birth/death rule over random seed fill. Inherently *regional and
  iterative* — the value at a voxel depends on a neighborhood relaxation, not
  a pure function of (seed, x, y, z). Breaks lazy/deterministic evaluation
  and the CPU/GPU mirror. Rejected outright for this codebase.
- **Agent/worm carvers**: what our lattice-tube system already replaces with
  a closed form. A worm's polyline must be discovered by scanning neighbor
  chunks for walk origins and re-simulating walks; our jittered lattice gives
  the same visual (winding tubes) as a pure O(1) per-column function with
  *provable* connectivity. Already landed; not revisited.
- **Sparse hash-placed SDF primitives** ("cavern sites"): deterministically
  gate cavern sites on a coarse lattice, each a union of a few ellipsoids;
  only columns near a site pay anything. This is the closed-form analogue of
  the cheese cave the same way lattice tubes are the closed-form analogue of
  the worm carver. Connectivity is free **if sites are anchored on the
  existing tunnel network's nodes**.

### 1.3 Which family produces what

| Want | Noise answer | Closed-form answer (ours) |
|---|---|---|
| tunnels | spaghetti (`\|n\|<t`) | lattice tubes — **landed** |
| large caverns | cheese (n > t) | **hash-gated ellipsoid-cluster sites on lattice nodes** (this design) |
| crevices | noodle / ravine carver | **tall thin slabs along existing lattice edges** (this design) |
| underground water | aquifer cells | **per-site flood level, aquifer-style, static until breached** (this design) |

## 2. Recommendation

**Extend the lattice-graph system with two new closed-form primitives rather
than introducing any 3D noise field:**

1. **Caverns** = hash-gated sites at *backbone-crossing nodes* of the existing
   lattice (102.4 m spacing, 1-in-4 gate ≈ one cavern per ~205 m square),
   each a union of 4 overlapping hash-jittered ellipsoids (rooms ~12–28 m
   across, ~5–12 m tall) with a flat-clamped floor and per-column
   noise-roughened walls. Anchored at **absolute z** derived from the surface
   at the site's own center (not per-column depth space) so floors and water
   surfaces are level. The anchor point *is* a lattice node that provably has
   four backbone tunnels through it → every cavern is connected to the global
   network **by construction**, same argument as sinkhole shafts.
2. **Crevices** = a 1-in-8 gated *decoration on existing edges*: a thin
   (0.6–1.6 m) vertical slab, lens-tapered along the edge, extending ~3–10 m
   up and ~2–6 m down from the tube axis, in depth space like the tubes.
   Because the slab contains the tube's own axis, connectivity is inherited.
   Crevices emit ordinary `CaveSeg`s — **zero new per-voxel mechanism**.

Why this and not per-brick 3D noise (the strongest competitor, §3.8): the
noise route costs an extra field evaluation for *every* underground brick,
gives emergent connectivity that our NPC/water/collapse systems can't rely
on, and forces a new evaluation site (per-brick) into `makeBrick`, the UE
column cache and both shader entry points. The site route keeps the
common-case column at "+4 hash gates and an xy reject" (measured §3.7), keeps
the per-voxel inner loop *byte-identical in shape*, keeps connectivity a
theorem instead of a statistic, and localizes 100% of the new cost to the
~4–5% of columns that are actually near a cavern.

**Underground water**: generated **static and implicit** (worldgen-owned,
deterministic, zero storage — a per-column `floodZMm`), converted to live W2
CA fill **lazily, brick-by-brick, when a breach or CA activity approaches**
("mobilize-on-approach"). This is exactly the plan §3.7 state machine
(implicit static ⇄ active sim voxels) applied underground, and it is the only
option that is simultaneously deterministic across clients, free until
touched, and drains properly when you dig into a lake.

## 3. Chosen architecture

### 3.1 Placement and anchoring

- Cavern candidate sites live at backbone-crossing nodes: lattice indices
  `(i & 3) == 0 && (j & 3) == 0` (the same class sinkhole shafts use —
  102.4 m apart). A new hash channel `CH_CAVERN_SITE = 22` gates 1-in-4 of
  them open and supplies all site geometry bits. Sinkholes gate on their own
  channel, so some caverns *also* have a sinkhole shaft dropping through
  their roof from daylight — desirable drama, free.
- The **anchor point** is the node's jittered position `(nx, ny)` (from the
  existing `caveNode()`/`CH_CAVE_NODE`, so it is bit-identical to the tunnel
  network's node) at absolute height
  `anchorZ = surfaceMmAt(nx, ny) − nodeDepthMm`.
  The node point lies on the axis of all four incident backbone tunnels, and
  the anchor is that exact point, so **the cavern volume contains a point of
  the connected backbone** for every seed. Connectivity is structural.
- `surfaceMmAt(nx, ny)` is the amplifier's own surface function (bilinear
  tile base + 4 detail octaves) evaluated at the site center — NOT the
  querying column's surface. This is the one genuinely new requirement: the
  cavern pass needs the surface at a *different* xy than the column being
  evaluated (§3.5 covers the GPU consequence). It is what makes cavern
  floors and water tables **level** instead of draping with terrain — a lake
  whose surface follows the hillside above it is instantly, visibly wrong.
  Tunnels keep their depth-space draping; rooms and water do not tolerate it.
- Site gate additionally requires `surfaceMmAt(site) ≥ kCavernMinSurfaceMm`
  (proposed 20 m): no caverns under beaches/ocean, mirroring
  `kCaveMinSurfaceMm` but stricter because caverns are taller.

### 3.2 Shape: ellipsoid cluster with flat floor and rough walls

Per open site, K = 4 child ellipsoids:

- Child 0 centered at the anchor; children 1–3 offset by hash within
  ±8 m horizontally, ±3 m vertically.
- Semi-axes hashed: `rx, ry ∈ [6, 14] m`, `rz ∈ [2.5, 6] m` (oblate — rooms,
  not spheres). Constants pinned by `static_assert`:
  `maxOffsetXY (8 m) < minRxy + minRxy (12 m)` → every child overlaps child 0
  → the union is connected; and `maxReach = maxOffsetXY + maxRxy + maxRough`
  stays under the candidate-block bound (§3.4).
- **Flat floor**: each child also carries `zFloorMm = anchorZ − hash·[1..4] m`;
  the per-voxel test gains one compare (`z ≥ zFloorMm`). Dome floors look like
  bubbles and make water depth ill-defined; flat floors read as rooms, are
  walkable by M6 agents, and give water bodies a well-defined basin. One
  int32 per seg, one compare per voxel *on cavern segs only*.
- **Rough walls**: per-column modulation of the reduced margin by 2D integer
  value noise (`valueNoise2`, new channel `CH_CAVERN_ROUGH = 23`, ~3.2 m
  lattice, q10 factor in `[0.7, 1.3]`). Because it multiplies the *per-column*
  margin, walls and ceilings undulate organically at zero per-voxel cost.
  (This is the same trick the tunnels get for free from node jitter.)

### 3.3 Per-column reduction (the fast-path argument)

New per-column struct (CPU: a member of `ColumnSample`, alongside `cave`):

```
struct CavernSeg  { int32 marginSq;  // rz²·(reachSq − dxy²)/reachSq, q-mm²
                    int32 zCenterMm; // ABSOLUTE z of child center
                    int32 zFloorMm;  // ABSOLUTE floor clamp
                  };
struct CavernColumn { int32 count; CavernSeg segs[kMaxCavernSegs /*6*/];
                      int32 floodZMm; /* INT32_MIN = dry (§5) */ };
```

Per-voxel test, appended to `caveCarveAt`'s loop (same guards run first):

```
zAbs = vz*100 + 50
for s in cavernSegs:  dz = zAbs − zCenterMm
                      carve if dz*dz < marginSq && zAbs ≥ zFloorMm
```

Identical shape to the tunnel test: one multiply, two compares. Columns not
near a cavern have `count == 0` and pay **nothing per voxel**.

Range check: semi-axes ≤ 14 m → `marginSq ≤ rz²` scaled ≤ (6000 mm)² =
3.6e7 ≪ INT32_MAX; `dz ≤ ~40 m` → `dz²` ≤ 1.6e9 < INT32_MAX but computed in
int64 as today. All existing habits hold.

### 3.4 Candidate enumeration (why the common case is ~free)

Site anchors sit inside the 25.6 m fine cell at each coarse (102.4 m) corner.
Maximum reach of a site from its anchor is
`maxOffsetXY + maxRxy + roughness ≈ 24 m`, and `24 m + 25.6 m (jitter cell)
< 102.4 m`, so **the 2×2 block of coarse corners around the column's coarse
cell is exhaustive** (`static_assert`, same style as the tunnels' 3×3
argument). Common-case per column:

- 4 gate hashes (`CH_CAVERN_SITE`), expected 1 open;
- for an open site: 1 `caveNode` hash + one xy distance-squared reject.

Only columns within ~24 m of an open site's anchor — an expected
`π·24² / (4·102.4²)` ≈ **4.3% of columns** — proceed to the full reduction:
one `surfaceMmAt(site)` evaluation (bilinear + 4 octaves ≈ 16–20 hashes,
identical code to the column's own surface), K=4 child hashes, and per child
a handful of int64 multiplies plus **one floorDiv** for the margin, plus one
`valueNoise2` roughness sample. Ballpark: ≈ 40–60 hash-equivalents on 4% of
columns, ≈ 5–6 hashes on the other 96%.

Against today's 34 cave hashes per column (which are themselves a fraction of
total column cost — raster bilinear, climate, biome, 4 detail octaves), the
expected added *column* cost is well under 10%, and the added *voxel* cost in
the common case is zero. §3.7 replaces these estimates with measurements.

### 3.5 CPU/GPU mirror

- CPU: `Amplifier::column()` computes `CavernColumn` (it owns the tile
  sampler, so `surfaceMmAt(site)` is available). `materialAt` consults it.
- GPU: mirror `cavernColumnFor` in `worldgen.ush`, evaluated in
  **`VoxelizeMain`** exactly like `caveColumnFor` is today (recompute, don't
  widen `GpuColumnSample` — carrying it would add ~80 B/column ≈ 64 MB at
  radius-64 harness scale for data that is pure hash+raster math).
  **New requirement**: `VoxelizeMain` needs the elevation raster bound
  (`t0`/ElevationMm — today ColumnMain-only) and the raster window padded by
  **+2 px margin** (site anchors can lie ~26 m ≈ 1 px outside the dispatch
  footprint at 30 m/px; +2 is the safe ceil). The surface-at-xy computation
  already exists in the shader (ColumnMain's body) — factor it into a shared
  function, byte-identical math.
  - Fallback if binding churn is unwelcome: evaluate caverns in ColumnMain
    and carry the reduced segs in a widened `GpuColumnSample`. Correct but
    pays the buffer bandwidth; recompute is the precedent and the
    recommendation.
- All-integer throughout; the one new division per child is `floorDiv`
  (lint-clean by construction). No wave ops, no shared memory.

### 3.6 Safety guards (roof / bedrock / sea level / clamps)

Absolute-z anchoring changes the guard story honestly: for tunnels the
geometry *provably* respects roof and bedrock in depth space and the clamps
are backstops. A level cavern under sloping terrain cannot have both
properties — uphill columns see it deeper (bedrock-ward), downhill columns
shallower (roof-ward). The design keeps every existing per-voxel clamp
**load-bearing at the slope extremes, by declared intent**:

- roof: `depthMm < kCaveRoofMinMm (6 m)` still refuses — a cavern near a
  slope gets truncated with ≥6 m of cover, never a surprise skylight;
- bedrock: the 2 m margin + `MAT_BEDROCK` refusal still refuse;
- sea level: `vz < 0` still refuses (implicit-ocean guard unchanged);
- sizing at the anchor column is structural: gate requires
  `nodeDepth − rzTop ≥ roofMin + 2 m` and
  `nodeDepth + rzBottom + 2 m ≤ 38 m` (inside the shallowest bedrock band
  minus margin), `static_assert`-pinned on the constant ranges.

Consequence: no natural cavern mouths in v1 (the roof clamp seals them);
access is via tunnels and sinkholes. Hash-gated hillside mouths are listed as
an open question (§8).

Truncation on slopes also means the *measured* connectivity test (flood fill,
`test_caves.cpp` style) remains the arbiter, exactly as it is for tunnel
pinching at cliffs today.

### 3.7 Measured cost (prototype)

Standalone integer prototype of §3.3–3.4 built against the repo's own
`hash.h`/`caves.h` (scratchpad `cavern-bench/bench.cpp`, sonnet measurement
agent; clang++ -O2, ~1.05 M columns, best of 3, this dev box):

| Measurement | Result |
|---|---|
| baseline `caveColumnFor` (landed tunnel reduction) | **191.3 ns/column** |
| baseline + cavern pass | **278.6 ns/column** (**+87.3 ns, +45.6%** of the cave-reduction share) |
| columns: all 4 gates closed / gate open but xy-rejected / full reduction | 32.8% / 63.0% / **4.2%** (theory: 31.6% / — / ~4.3%) |
| per-voxel test, 0 cavern segs (the 96% common case) | **+0.45 ns/voxel** (a single `count==0` check) |
| per-voxel test, 3 / 6 segs (near-cavern columns only) | 2.0 / 3.8 ns/voxel (~0.6 ns per seg) |

Reading the numbers:

- The +87 ns is **per column, once**, vs. the per-voxel work that dominates
  brick generation (a level-0 stack evaluates hundreds of voxels per
  column); relative to the *full* `column()` (raster bilinear + climate +
  biome + 4 detail octaves + stratigraphy, not just `caveColumnFor`) the
  regression is well under the headline 45.6%. C9's bench gate (<5% total
  column-path regression) is the binding check.
- Most of the 87 ns sits in the 63% "gate open, xy-rejected" tier (a
  `caveNode` hash + distance per open corner). Cheap fix folded into C1:
  derive the gate bits **from the same hash that supplies the node jitter**
  (one `hash2` per corner total, gate tested before the multiply-shift
  decodes) — removes a hash from the hot tier at zero design cost.
- The per-voxel fast path holds: cavern-free columns pay one compare per
  voxel, and even worst-case columns stay under 4 ns/voxel in the inner
  test.
- Prototype deviations (documented in bench.cpp): child radii were set
  smaller than spec (~4 m vs 6–14 m), which lowers the average seg count
  (0.27) but not the per-column op counts the timings measure; timing loops
  defeated dead-code elimination via a volatile sink; strided sampling
  decorrelated the region from the 102.4 m coarse lattice (a contiguous
  1024-column block is exactly one coarse cell wide — the real world does
  not have that alignment problem, but a naive bench does).

### 3.8 The road not taken: per-brick coarse 3D field

For the record, the strongest alternative: evaluate a coarse 3D noise at
brick corners (8 samples per 8³ brick), trilinearly interpolate per voxel in
fixed point, carve where interpolated density < t (a cheese cave). Honest
assessment — cost is actually tolerable (~1 hash-equivalent per voxel
amortized); what kills it is everything else: connectivity becomes emergent
and threshold-fragile (the exact failure mode caves.h §"WHY A LATTICE-GRAPH
FORMULATION" documents); evaluation moves from per-column to per-brick,
which touches `makeBrick`, the UE column cache contract, `gpu_harness` and
both shader entry points; LOD mips sample it at different granularities;
and every safety guarantee (roof/bedrock/ocean) becomes statistical. It buys
visual variety we can replicate with more/bigger ellipsoid children and
roughness noise at a fraction of the architectural churn.

## 4. Crevices

A gated decoration on **existing** lattice edges (tube already present):

- Gate: 1-in-8 per existing edge, `CH_CREVICE = 24`.
- Geometry: vertical slab centered on the tube axis: half-thickness
  `t ∈ [0.3, 0.8] m` (hash per edge), extending `hUp ∈ [3, 10] m` above and
  `hDown ∈ [2, 6] m` below the axis depth, **lens-tapered** along the edge by
  `4u(1−u)` (q10, `u` = along-edge parameter already computed as `num/den` in
  `caveColumnFor`) so fissures pinch out at nodes instead of ending in walls.
- Reduction: inside the existing edge loop — if the column's xy
  distance-squared to the axis `≤ t²`, emit an ordinary
  `CaveSeg{marginSq = halfSpan², depthMm = axisDepth + (hDown−hUp)/2}` in
  **depth space** (draping is fine, even good, for a narrow fissure).
  Per-column clamp shrinks `halfSpan` so the top stays below
  `kCaveRoofMinMm`.
- Per-voxel: **nothing new** — crevice segs are tunnel segs. Because the slab
  interval contains the axis depth, every crevice voxel-column overlaps its
  own tube → connected by construction.
- Cost: +1 hash per existing edge (gate), +1 hash + arithmetic on the ~1/8
  that open. A column sits near ≤ ~4 existing edges typically → ~4–6 hashes.
- Storage: `kMaxCaveSegs` 8 → **12** (a junction column can now see tube +
  crevice per edge). `CaveSegmentCapHeadroom` re-measures; the cap stays a
  storage bound, not a design limit.

Verdict on the research question: crevices are a **cheap special case of the
existing tube system** — they do not want their own generator. (Minecraft
agrees: its ravines are the tunnel carver with a tall thin cross-section.)
Surface-breaching ravines (canyons) would be a *different*, surface-visible
feature — deliberately out of scope pending Matt (§8).

## 5. Underground water

### 5.1 Generated state: static, implicit, worldgen-owned

Aquifer-style, adapted to sites (we don't need Minecraft's cell blending —
our water bodies live in discrete caverns with rock between them, so the
"barrier noise" problem never arises):

- Per open cavern site, `CH_CAVERN_FLOOD = 25` decides: **40% dry**, else a
  flood level `floodZMm = maxFloorZ + hash·[0.8 .. 3.2] m`, clamped below
  `anchorZ` (air above the lake, never a fully-drowned room) and to `> 0`
  (below z=0 the implicit ocean owns water; site gating keeps caverns inland
  anyway).
- Reduction: `CavernColumn.floodZMm` = the flood level if the column is
  within the site's reach disc (whether or not a cavern seg overlaps it —
  so a tunnel passing through the flooded zone floods too, no dry-tunnel-
  next-to-water seams inside the reach disc), else `INT32_MIN`.
- Semantics: a voxel is **implicit static water** iff it is cave air
  (`materialAt == MAT_AIR`, below-surface) and `zAbs < floodZMm`. This is a
  *derived predicate*, exactly like the W1 ocean's "air below z=0 offshore":
  terrain bricks still never store water (doctrine preserved), no CA fill
  exists, zero per-column storage beyond one int32, fully deterministic and
  covered by `kWorldGenVersion`.
- Rendering (UE): per-column `floodZMm` is available in the column cache; the
  terrain mesher emits a water-surface quad layer where cave air spans
  `floodZMm` (same material family as the ocean plane; underwater fog trigger
  extends to "below floodZ inside a flooded column"). No voxel water data.

Known accepted artifact: a long tunnel that dips below a neighboring site's
flood level *outside* the reach disc stays dry — a vertical water face can be
visible at the disc boundary in rare geometries. Minecraft's aquifers accept
the same class of artifact (visible level steps at cell boundaries with
barrier blocks). Tuning (modest flood depths, floors below tunnel-axis depth
band) makes it rare; mobilization (§5.2) makes it physically correct the
moment it matters.

### 5.2 Dynamic state: mobilize-on-approach into the W2 CA

The moment gameplay touches a lake, static water must become real:

- **Trigger**: the engine edit path already calls
  `wakeRegion`/`invalidateSolidRegion` per edit. Add: if the edit's halo (or
  any CA-active brick) intersects a brick containing implicit water, that
  brick is **mobilized**: every implicit-water cell in it becomes CA fill
  (255 below floodZ) via the existing accounting path, the brick is marked in
  a persistent `mobilizedBricks` set, and `wakeRegion` covers it. From then
  on the implicit predicate is masked off for that brick (render + queries
  consult the set).
- **Propagating front**: mobilization also fires when an *active CA brick*
  becomes adjacent to a still-implicit water brick (checked during the
  engine's existing per-tick dirty-brick sweep, not inside voxel-core's CA).
  Digging into a lake therefore converts the nearby water; as it drains, CA
  activity reaches deeper into the body and converts the next shell of
  bricks, so the drain front advances **brick-by-brick, budget-bounded per
  tick** — no 9500-brick hitch on first pickaxe swing, and the static
  remainder behind the front simply holds the old level until reached, which
  reads as a plausible drain front.
- **Conservation**: exact by construction — each mobilized cell adds exactly
  its implicit fill to the CA's ledger once (`mobilizedBricks` guards
  re-entry); the CA conserves from there. No global "cavern volume" number is
  ever needed; the hypsometric-reservoir machinery of plan §3.7 remains a
  later optimization for re-staticizing settled bodies, not a dependency.
- **Determinism story** (the important boundary): the *flood field* is
  worldgen (pure `f(seed, x, y)`, versioned by `kWorldGenVersion`). The
  *mobilized set and CA fill* are **simulation state** — server-authoritative,
  replicated like all W2 water, persisted alongside CA state in the save.
  The edit log stays terrain-only and untouched. A replay of the same edit
  stream over the same seed reproduces the same mobilization sequence
  (the trigger is a deterministic function of edits + CA activity), but
  mobilization is *not* re-derived from the edit log on load — it is loaded,
  like water. `kWaterCAVersion` does **not** bump: no tick rule changes; the
  new bulk-seed entry point is additive engine plumbing in the same class as
  `wakeRegion`'s original introduction (a version note in the header comment
  is warranted either way).
- **CA sees static water as absent** (it isn't fill) — harmless: rock walls
  separate bodies, and any path by which CA water could reach an implicit
  body passes through the adjacency trigger first, converting it before
  contact. That invariant ("no CA water may touch an implicit-water cell")
  is the property test for this feature.

### 5.3 Why not the alternatives

- **Static-only (no CA handoff)**: digging into a lake leaves a hole in a
  wall of frozen water. Matt's ask explicitly includes draining; W2 exists
  and now reacts to digs. Rejected.
- **Fully dynamic from generation** (seed CA fill for every lake at chunk
  materialization): couples the CA ledger to streaming order, makes worldgen
  determinism depend on simulation state, costs memory and ticks for water
  nobody has ever seen, and re-runs settling on every load. Rejected — this
  is precisely what the plan's implicit⇄active state machine exists to avoid.

## 6. Integration with existing systems

- **Connectivity / NPCs (M6)**: caverns contain a backbone node; crevices
  contain their tube's axis → the "one big component" property strengthens.
  `pathfind.h` prices air cheap, so caverns become natural NPC routes — and
  flooded caverns are *still air to the pathfinder* (water isn't in terrain
  bricks). NPCs will happily stroll through lakes; already true of the ocean,
  parked as a water-track follow-up (water-aware traversal cost), noted here
  so nobody is surprised.
- **Collapse (M5)**: generation does not run stability analysis, so generated
  caverns stand until touched (correct — real caves stand). First player dig
  into a wide cavern roof runs the ordinary edit-driven collapse check over a
  span the system has not seen before (up to ~28 m). Needs one calibration
  pass (subtask C6) so a single dig doesn't drop a whole ceiling — or does,
  if that's the fun — Matt's call on tuning, mechanism unchanged.
- **GI (M4)**: the light field flood fill already handles enclosed volumes;
  caverns are simply dark until opened or lit. No change.
- **Streaming**: deepest cavern voxel by construction ≤ 38 m below surface —
  inside the 38.4 m near-band skirt that the cave band already sized. No
  streaming change; the constraint is now load-bearing from two features, so
  the static_assert should mention it.
- **LOD/mips**: cave air already propagates up the mip chain; bigger voids
  just show up in R1+ like any air. Heightmap/vista bands unaffected
  (underground). No change.
- **Ocean/W1**: unchanged guards (`vz ≥ 0`, min-surface gates). Flood levels
  are strictly above z=0, so implicit-ocean and implicit-cavern water are
  disjoint predicates.

## 7. Build plan (subagent-executable subtasks)

Ordering respects the standing constraint that another agent may be working
in `amplifier.cpp` / `worldgen.ush`: all early subtasks live in **new files
or caves.h only**; the two shared-file fold-ins are deliberately last and
tiny.

- **C1 — `voxelcore/include/voxelcore/caverns.h`** (new, header-only,
  integer-only): constants + static_asserts (§3.1–3.4, §3.6), hash channels
  22/23/25, `cavernSiteFor`, `cavernColumnFor` (needs a
  `surfaceAtFn`-style callback or a small `SurfaceEval` helper shared with
  the amplifier — keep raster access injected, doctrine-clean),
  `cavernCarveAt`, flood-level function. Owner: caverns.h only. No other
  file touched. ~Independent.
- **C2 — crevices in `voxelcore/include/voxelcore/caves.h`**: `kMaxCaveSegs`
  8→12, `CH_CREVICE = 24`, slab reduction inside the edge loop, updated
  header comment + static_asserts. Owner: caves.h + `test_caves.cpp`
  headroom/golden updates. Independent of C1.
- **C3 — tests `voxel-core/tests/test_caverns.cpp`** (new): connectivity
  flood fill including caverns+crevices (component count / largest share, as
  test_caves.cpp), roof/bedrock/sea-level guard sweeps, slope-truncation
  measurement, volume fraction budget, cavern-seg cap headroom, flood-level
  properties (level surface, >0, below anchor, disc-consistency), pinned
  goldens. Owner: tests + CMake list. After C1+C2.
- **C4 — amplifier fold-in**: `ColumnSample` gains `CavernColumn`;
  `column()` computes it; `materialAt` consults `cavernCarveAt`. ~15 lines
  across `amplifier.h`/`amplifier.cpp`. **Coordinate with any perf agent in
  those files; land last of the CPU wave.** Owner: amplifier.h/.cpp.
- **C5 — version + goldens**: `kWorldGenVersion` 4→5 with changelog comment;
  re-pin per §9's predicted table; `vxc_tests` green. After C4.
- **C6 — GPU mirror in `voxel-core/shaders/worldgen.ush` + harness**:
  factor surface-at-xy into a shared function; bind ElevationMm into the
  VoxelizeMain pipeline (+2 px raster margin in `gpu_harness` and the UE
  dispatch path); mirror caverns + crevices bit-exactly; lint
  (`tools/lint-shader-ub.py`) clean; full-region CPU/GPU compare on the AMD
  box (and the NVIDIA runner when it cycles). **Coordinate — shared file.**
  After C4/C5.
- **C7 — UE static water rendering**: mesher emits flood-surface quads from
  per-column `floodZMm` + underwater fog trigger underground; screenshot a
  flooded cavern once underground streaming shows it. Owner:
  `ue-project/Source/VoxelEarth` (mesher/water subsystem render side).
  Parallel with C6.
- **C8 — mobilize-on-approach**: engine edit-path hook + active-adjacency
  check, brick conversion (implicit fill enumeration helper in voxel-core —
  new small header, no CA rule change), persistent + replicated
  `mobilizedBricks`, per-tick conversion budget; tests: conservation ledger
  exactness, no-CA-touches-implicit invariant, dig-drains-lake scenario.
  Owner: `VoxelWaterSubsystem.*` + new voxelcore helper header. After C7.
- **C9 — perf gate**: bench column throughput before/after (existing bench),
  assert common-case column cost regression < 5% and zero per-voxel
  regression for cavern-free columns; record in docs/status.md. After C5
  (CPU) and again after C6 (GPU).

Suggested order: C1 ∥ C2 → C3 → C4 → C5 → C9a → C6 ∥ C7 → C8 → C9b.

## 8. Open questions for Matt

1. **Density/scale**: proposal = ~1 cavern per 205 m square, rooms 12–28 m
   wide / 5–12 m tall, ~60% flooded. More or fewer? Bigger rooms need the
   coarse spacing and the streaming skirt revisited together.
2. **World depth**: bedrock at 40–60 m caps the whole underground at roughly
   one cavern "storey". Minecraft's post-1.18 world is ~3× deeper and layers
   caves. Deepening is a separate, deliberate worldgen+streaming project —
   want it on the roadmap?
3. **Natural cavern mouths** (hillside openings where a cavern meets a
   slope): v1 seals them via the roof clamp; a gated exception would look
   spectacular but perforates the "6 m cover" guarantee. Want it?
4. **Surface ravines/canyons** (crevices that breach daylight,
   Minecraft-style): visible terrain change, new feature class — in or out?
5. **Lava**: aquifer prior art fills deep cells with lava; we have no lava
   material or sim. Park entirely, or reserve the hash bits now (cheap)?
6. **Collapse tuning on first cavern-roof dig** (§6): drama vs. safety.
7. Static-vs-dynamic water default needs no decision — recommended on merit
   (§5); flagging only per the task brief. Confirm and it ships as designed.

## 9. Version / golden impact

- `kWorldGenVersion` **4 → 5** (one bump for caverns + crevices + flood
  field together; land C1–C5 as one version event, not three).
- `kWaterCAVersion`: **no bump** (no tick-rule change; mobilization is
  engine plumbing + additive seeding API). Header note added.
- Hash channels: append 22 (`CH_CAVERN_SITE`), 23 (`CH_CAVERN_ROUGH`),
  24 (`CH_CREVICE`), 25 (`CH_CAVERN_FLOOD`). Append-only rule respected;
  32+ (synthetic tiles) untouched.
- Predicted golden movement (to be asserted, not discovered):
  - MOVE: `cave_layer` (materialAt now carves more), `amplifier_golden_digest`
    (ColumnSample layout/content grows), gpu_harness `--radius 64`/`--radius
    128` digests.
  - MUST NOT MOVE: gpu_harness default 2-region digest (surface shell only,
    entirely above the 6 m roof — same argument that held for v4),
    `mips_chain` (±3 m of surface), `biome_map`, all `test_hash`,
    `connectivity`/`pathfind`/`regiongraph`/`collapse`/`waterca`/`tilestore`
    (fixture-driven, amplifier-free).
  - NEW: `cavern_layer`, `crevice_layer`, `flood_field` goldens in C3.
- Edit-log compatibility: none preserved across the version bump, as always
  (`editlog.h` refuses mismatched `kWorldGenVersion`) — this is the standing
  policy, stated here so it's predicted.

## Sources

- minecraft.wiki — [World generation](https://minecraft.wiki/w/World_generation)
- jacobsjo — [Minecraft Aquifer Explanation](https://gist.github.com/jacobsjo/0ce1f9d02e5c3e490e228ac5ad810482)
- Alan Zucconi — [The World Generation of Minecraft](https://www.alanzucconi.com/2022/06/05/minecraft-world-generation/)
- `voxelcore/caves.h` header comment (lattice-graph rationale), PR #54;
  `docs/status.md` M4 cave pass + W2 sections; plan §2/§3.7/§4.
