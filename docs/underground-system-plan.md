# Underground system redesign — caves, tunnels, caverns

**Status:** PLAN (evidence + diagnosis + design; no worldgen code rides with
this doc). Written against `main` at v23 (`kWorldGenVersion = 23`, core.h:236),
after PR #199 (fine micro cap). The §1 evidence pack is **partial** — the box's
editor was ceded to higher-priority capture work mid-session; every un-shot row
is labelled NOT YET CAPTURED with its reproduction command. Author: agent
session 2026-08-03, from Matt's verbatim complaint below.

> "Right now, it looks terrible the caves and tunnels don't look
> believable/realistic. Caves look very computer made with procedural shapes.
> The tunnels are slightly better but really weird vertical shafts that shoot
> straight up to the game world surface to connect. This is not natural.
> Rethink the entire underground system for caves. Minecraft does a really good
> job of making caves feel natural and random as they're occurring in world. I
> think our caves, tunnels, and underground caverns should be influenced by the
> biome and terrain type they're occurring within. For example, I'd expect
> caves and crevices to be much more of a thing in mountainous regions than
> desert plains."

The judgement is Matt's and this doc does not re-litigate it. What it does:
show the current underground on camera, explain **mechanically** why it looks
the way it looks (every claim with file:line), and propose a redesign that
survives this codebase's real constraints — integer-only, CPU/GPU bit-exact,
deterministic, and render-thread-bound at 2K.

**The single most important finding, up front:** the vertical shafts Matt
flagged are not a bug and not lazy geometry — they are the system's
**entrance guarantee**, deliberately designed (caves.h:136-146), and they
carry three load-bearing properties at once: a guaranteed entrance *rate*
(the network is findable on a walk, even on flat ground), **structural
connectivity** (every shaft bottoms out on a backbone node of the provably
connected tunnel graph, which NPC pathing, digging and water routing all
assume), and **roof integrity everywhere else** (the ≥6 m cover guarantee
survives because the shaft is the one enumerated exception, not a leaky
threshold). Deleting the shafts without re-providing all three re-breaks
what they protect. The redesign therefore does not remove the entrance
mechanism — it re-shapes it into caused, place-dependent forms (§5.1 G5:
doline funnels, collapse mouths, widened slope mouths, stream sinks) that
keep the same anchors, the same sparse-rate discipline and the same
connectivity theorem. Details and quotes in §2.3.

---

## 1. Evidence: the current underground, on camera

All captures unattended via the `tools/voxel-capture.ps1` conventions: frozen
sun (12:00, 03-20, `-VoxelTimeScale=0`), one editor at a time, `.vxlog` cleared
before each run, settle counters grepped from the log and recorded next to each
file. Files in `D:\voxelsim\bake-out\caves-current\` with the full per-run log
excerpts in `captures-report.txt` there. Seed 20260719 (the project default,
`VoxelWorldSubsystem.cpp:13485`), fine tier, current tile cache.

One reading note on the settle line: `jobsInFlight` / `pendingJobs` are live
queue depths and must be 0 at the shutter. `unloaded=` in the same streaming
line is `TotalChunksUnloaded` — a **cumulative session counter**
(`VoxelWorldSubsystem.cpp:5101-5105`), monotone from process start. Every
underground fixture teleports the pawn tens of metres down, which forces the
surface streaming set to be torn down and the deep set built, so a non-zero
cumulative unload count is a property of the fixture, not of an unsettled
frame. The live unload queue is `pendingUnload`, reported verbatim below.

**Evidence status: the pack is PARTIAL and the gaps are labelled.** Midway
through this session the box's one-editor rule collided with another agent's
higher-priority capture work and this session stood down from the editor. One
capture completed and is delivered; every other row below is written out with
its exact reproduction command and marked **NOT YET CAPTURED** rather than
being dropped — the rows are the shot list for whoever gets the editor next.

| capture | file / status | settle at shutter | what it shows |
|---|---|---|---|
| Tunnel interior, savanna tile `2_-8`, spawn `38400,-115200`, `-VoxelCaveTest -VoxelCaveSettle=120` | `caves-tunnel-B-savanna-0.png` — **CAPTURED** | `jobsInFlight=0 pendingJobs=0 pendingUnload=0` (full line: `loaded=47083 unloaded=566 … underground=1 deepTracked=18923 deepWithGeometry=506`) | A pristine M4 tunnel from inside: 3.5 m-tall void, camera 15.0 m below surface, 1.5 m above floor; capture-time probes `+X=3.1m -X=0.5m +Y=8.0m -Y=2.7m +Z=1.9m -Z=1.0m`. The constant-calibre capsule shape of §2.1 is what is on screen. |
| Sinkhole shaft from below (the owner's vertical-shaft complaint, photographed up the bore), grassland tile `-1_1` | **NOT YET CAPTURED** — reproduce: `-VoxelSpawnAt=-7680,23040 -VoxelGICaveTest -VoxelGICaveSettle=120` + frozen-sun args | — | The 1.0–1.7 m radius perfectly vertical cylinder of §2.3, daylight mouth overhead. |
| Shaft mouth from the air (the same shaft seen from the surface) | **NOT YET CAPTURED** — parse `sinkhole shaft at (X,Y)` from the previous run's log, then `tools\voxel-capture.ps1 -SpawnAt 'X/100,Y/100' -SpawnAltM 45 -SpawnPitch -62 -SettleSec 240` | — | The clean uncased hole in undisturbed ground — no funnel, no debris, no cause. |
| Tunnel interior, grassland tile `-1_1` (second biome) | **NOT YET CAPTURED** — `-VoxelSpawnAt=-7680,23040 -VoxelCaveTest -VoxelCaveSettle=120` | — | Paired with the savanna interior: two tunnel interiors ~146 km apart in different biomes, expected indistinguishable (§2.4 — the generator takes no biome input, so this pair is the biome-insensitivity evidence). |
| Tunnel interior, tundra tile `-3_-3` (third biome) | **NOT YET CAPTURED** — `-VoxelSpawnAt=-38400,-38400 -VoxelCaveTest -VoxelCaveSettle=120` | — | Same pairing, colder climate. |
| Cavern interior, computed framing | **NOT YET CAPTURED** — `-VoxelSpawnAt=-7680,23040 -VoxelCavernShot=120 -VoxelCavernAt=-7680,23040` | — | The coaxial ellipsoid-stack room of §2.2. |

One caveat on the delivered frame, stated rather than argued away: batch 1
predates this session's overlap detection, and a foreign editor was first
observed on the box ~100 s after this run ended, so editor contention during
its settle window cannot be positively excluded. The queue depths at the
shutter were zero, and contention affects timing, not the generated geometry
the frame exists to show.

Fixtures used (all pre-existing; nothing was edited — a cave here is pristine
worldgen):

* `-VoxelCaveTest` — finds a real cave void near spawn and parks the camera in
  it (`VoxelEarthGameMode.cpp:2760-2994`).
* `-VoxelGICaveTest` — finds a **sinkhole shaft** near spawn and frames the
  daylight mouth from inside the cave (`VoxelEarthGameMode.cpp:603-830`). This
  is the owner's "vertical shaft that shoots straight up," photographed from
  the bottom.
* `-VoxelCavernShot` — measures a cavern room and frames it
  (`VoxelWorldSubsystem.cpp:13849-14290`).

Two capture-session facts worth recording: (a) the default capture spawn
(`-84480,53760`) is no longer inside the baked fine-tile set — the fine-tier
gate correctly refused it (`VoxelFineTileStreamer.cpp:455`), so all cave
capture sites are centres of currently-baked tiles (grassland `-1_1`, savanna
`2_-8`, tundra `-3_-3`); (b) the box was shared with another agent's
higher-priority capture work, which is why this session ultimately stood down
from the editor with the table below only partially shot.

---

## 2. Diagnosis: what generates the underground today

Two generators, both header-only integer worldgen, both carved per voxel in
`Amplifier::materialAt` (amplifier.cpp:2354-2377), mirrored bit-for-bit in
`worldgen.ush` (caves at :2039/:2203, caverns at :2398/:2492).

### 2.1 Tunnels: a jittered 2D lattice graph of capsules (`caves.h`)

* Nodes on a **25.6 m grid** (`kCaveLatticeMm`, caves.h:111), hash-jittered
  inside their cell, each carrying a hash depth **9–34 m below the local
  surface** (caves.h:114-115).
* Edges run **+x and +y between adjacent nodes only** (caves.h:277-281).
  Every 4th row's +x edges and every 4th column's +y edges always exist (the
  provably-connected "backbone", caves.h:34-41); other grid edges open on a
  1-in-4 hash gate. Each edge is a straight capsule of **constant radius
  1.2–2.8 m** (caves.h:121-123).
* Depth space: the whole network drapes under the topography. Roof cover of
  ≥6 m is guaranteed by construction (caves.h:51-57, static_assert :207);
  tunnels **daylight sideways on steep slopes for free** (caves.h:64-68) —
  the only "natural" entrances the system has.
* Crevices are not a separate generator: a 1-in-8 gated thin vertical slab
  (0.3–0.8 m half-thickness) riding an existing edge (caves.h:175-197).
* Hard exclusions: no caves below sea level (`kCaveMinVoxelZ`, caves.h:162),
  none under columns with surface below 12 m (caves.h:161), never within 2 m
  of bedrock (caves.h:160).

### 2.2 Caverns: coaxial ellipsoid chains at rare sites (`caverns.h`)

* Sites at every 8th lattice node in both axes (**204.8 m spacing**,
  caverns.h:177-178), gated 1-in-4 (caverns.h:185), anchored at **absolute z**
  so floors and water are level (caverns.h:104-113).
* Each open site: a chain of up to 4 ellipsoid "storeys" descending **on the
  same vertical axis** (`rx == ry`, "why coaxial", caverns.h:50-65): rooms
  24–56 m wide, deep rooms up to 80 m tall (caverns.h:202-217), each with a
  flat floor clamp and ±30% radial wall roughness (caverns.h:237-240).
* Per-site flood level: 60% of sites carry a level water table a little above
  the highest room floor (caverns.h:265-272, :417-428). The W2 mobilization
  half (dig in and it drains into the CA) shipped in the water subsystem.

### 2.3 The vertical shafts: what they are and *why they exist*

The shafts are **not an accident and not a bug** — they are the system's
entrance guarantee, and the design comment says so explicitly:

> "Depth-space tunnels never break the surface on their own (that is the roof
> guarantee), which on gentle terrain would leave the whole network sealed and
> reachable only by digging. Real cave systems open through a sparse set of
> potholes/sinkholes, and so does this one: at a BACKBONE CROSSING node …
> a 1-in-4 hash gate opens a vertical shaft from that node's depth straight up
> to the surface." — caves.h:136-146

And the carve exception:

> "Sinkhole shaft: the ONE construct allowed through the roof clamp, by design
> … Its bottom is a backbone crossing node, so it always lands on the main
> network; its top is the surface, so it is an entrance." — caves.h:559-564

Mechanically: candidate nodes every 102.4 m, 1-in-4 open → **about one shaft
per 205 m square**, each a **perfect vertical cylinder of radius 1.0–1.7 m**
(caves.h:148-156) running from the surface straight down to the node depth
(9–34 m). The perforated fraction of the surface is measured (well under 0.1%,
caves.h:150-152, test_caves.cpp).

So the shafts are carrying three load-bearing guarantees, and any redesign
must carry all three some other way:

1. **Entrance rate** — the network is findable on a walk without digging, on
   flat ground too (slope daylighting only covers steep terrain).
2. **Structural connectivity** — the shaft bottom IS a backbone node, so every
   entrance provably reaches the single connected component. NPC pathing (M6),
   digging, and water routing are all written against "caves go somewhere."
3. **Roof integrity elsewhere** — the ≥6 m cover guarantee stays intact
   because the exception is an explicitly enumerated, sparse construct rather
   than a leaky threshold.

What makes them read as wrong is purely their **shape**: a hash-placed,
perfectly cylindrical, perfectly vertical bore with no surface expression (no
funnel, no debris, no cause), at near-uniform density across every biome and
landform. Real sinkholes are karst features: they form where soluble rock and
water throughput exist, they flare into funnels, and they cluster.

### 2.4 What already varies with the world — and what doesn't

Varies today:

* **Terrain shape**, through depth-space draping and slope daylighting
  (caves.h:44-68) — a hillside gets mouths, a plain gets none.
* **Sea/beach guards** (caves.h:161-162, caverns.h:190).
* **Per-site flood level** (caverns.h:265-272).

Does **not** vary today — and this is the whole of Matt's third paragraph:

* `caveColumnFor(seed, vx, vy, surfaceMm)` (caves.h:527) and
  `cavernColumnFor(seed, vx, vy, surfaceMm, surfaceAt)` (caverns.h:590) take
  **no climate, no biome, no relief, no lithology input of any kind**. Tunnel
  density, radius, depth, crevice rate, cavern rate: identical constants from
  tundra to savanna. This is provable from the signatures alone; the §1
  grassland/tundra interior rows exist to let Matt see it, and are part of the
  labelled capture backlog.
* **Lithology does not exist at runtime.** The one lithology-adjacent gate the
  amplifier ever had belonged to the 3D density band (couplings (13)/(14)) and
  was removed with it at v20 (amplifier.cpp:1400-1416). Nothing in voxel-core
  knows what rock a column is made of beyond the fixed
  topsoil/subsoil/rock/bedrock stack.
* The climate the amplifier **already reads per column** for biome and topsoil
  — bio_1/bio_4/bio_12/bio_15, blended in amplifier.cpp:185-249 — is not
  consulted by either carve pass.

### 2.5 How caves interact with the bake and the fine tier

* Caves are **runtime worldgen only**. The terrain-service bake produces
  surface rasters (elevation control lattice, climate planes, flow plane —
  docs/vxtl-v2-format.md); it computes no underground data and knows nothing
  about caves. Conversely the cave pass reads nothing from the bake except
  `surfaceMm`.
* Because tunnels live in **depth space**, their absolute z is
  `surface − depth`: any change to the surface moves every cave under it.
  v23 (fine micro cap, PR #199) changed the fine-tier detail band, so v23
  moved cave geometry wherever the fine surface moved. Cave version churn is
  coupled to surface version churn by construction; that is worth keeping in
  mind when batching bumps.
* The bake's hydrology is **blind to cave entrances**: B4b depression-fills
  the whole domain (docs/worldgen-variety-plan.md:311-316), so the baked
  surface has no closed sinks — and then the cave pass perforates it with
  shafts the hydrology never saw. Today that is cosmetically survivable
  because shafts are ~1.4 m wide; any karst-scale entrance work has to face
  it head-on (§5, item W8).
* Streaming: the underground footprint is sized from the cave band — the
  38.4 m near band is set by the cave pass's maximum carve depth
  (docs/status.md "Underground streaming"). **Deepening the cave band is
  therefore a streaming-cost decision, not just a worldgen one.**

### 2.6 Why it reads "computer made" — the mechanical tells

Stated as geometry, not taste. Each of these is visible in the §1 captures:

1. **One characteristic scale per feature class.** Every tunnel is 2.4–5.6 m
   wide at 9–34 m depth on a 25.6 m lattice; every cavern room is an
   ellipsoid of revolution at one of 4 storeys of one chain. There is no
   small-into-large progression, no crawlway, no chamber you enter through a
   constriction.
2. **Direction lock.** Every tunnel edge connects a node to its +x or +y
   neighbour (caves.h:274-281). After jitter the individual segment is
   oblique, but over any distance the network runs grid-cardinal: backbone
   rows are unbroken west–east corridors every 102.4 m, columns north–south.
   (Same family of artifact as the D8 bake direction lock — a lattice
   admitting only a fixed set of headings.)
3. **Straight constant-radius capsules.** An edge's radius is one hash draw,
   constant along the whole 25.6 m (caves.h:283-287); the axis is a straight
   line in xy and a linear ramp in depth. Real passages meander, swell and
   choke.
4. **Rotational symmetry in rooms.** `rx == ry` (caverns.h:199-204) plus a
   coaxial stack means a cavern is a stack of round plates on one vertical
   axis; the ±30% roughness modulates the radius but cannot break the
   symmetry of the plan view.
5. **Cylindrical vertical entrances**, §2.3.
6. **Uniform density everywhere**, §2.4 — nothing about the place explains
   the caves in it. The standing principle in the provinces work is "caused
   variety, not placed variety" (docs/landform-provinces-plan.md); the
   underground currently has neither cause nor variety.

---

## 3. The bar: what Minecraft 1.18 actually does

Verified against the vanilla worldgen JSONs and the technical literature
(minecraft.wiki Cave/Noise-router/Aquifer pages; jacobsjo's aquifer analysis;
Kniberg's talks). Full detail in the research digest; the structural lessons:

* **Several generators with different characteristic shapes overlap, and none
  is the only thing you meet.** Cheese (large chambers: one low-frequency 3D
  noise past an off-centre threshold, pinched shut near the surface, broken
  into storeys by a squashed "cave_layer" noise, with a separate **pillar**
  field adding stone columns inside big rooms); spaghetti (long winding
  tunnels: the near-zero set of the max of two 3D noises — the |n|<t trick —
  with a **regional rarity noise modulating calibre**, plus a roughness field
  on the walls); noodle (1–5-block crawlways, same trick at 2.7× frequency,
  **regionally gated on/off** by a dedicated low-frequency noise); plus the
  legacy **carvers** (random-walk worms with sin-profile radius along the
  walk, and ravines: the same carver with yScale 3) still running on top.
* **Entrances are caused, not drilled.** A dedicated `entrances` density term
  exists **only near the surface** (a depth gradient kills it below ~y=30) and
  is min-ed with spaghetti so every surface funnel continues into a tunnel;
  near-surface terrain lets *only* that field carve, so noise caves cannot
  shred hilltops; and carvers plus plain field/terrain intersection supply
  hillside holes. Nothing in 1.18 bores a uniform vertical cylinder.
* **Water is local.** Aquifers assign quantized water levels on a 16×40×16
  cell grid with noisy stone barriers between disagreeing cells: perched
  pools, part-flooded rooms, dry caves under oceans, waterfalls — all
  emergent.
* **Biome changes decoration, not shape** — dripstone/lush/deep-dark are
  selected by the same climate noises layered on afterwards. The shape variety
  players attribute to "biomes" is mostly the regional rarity/gating noises
  above. Notably, Matt's ask goes **further than Minecraft** here: he wants
  shape coupled to place. We can do that honestly because our climate and
  relief are real per-column inputs, not post-hoc labels.
* What Minecraft does *not* provide: connectivity guarantees (a cheese room
  can be sealed; players dig), determinism constraints beyond seed purity, or
  an integer/GPU-mirror discipline. Our cavern design already rejected raw
  3D-noise carving for exactly those reasons (docs/cavern-design.md §1.2,
  §3.8) — the rejection stands. The redesign below reproduces Minecraft's
  *overlap-of-families* property inside the closed-form, per-column-reducible
  machinery we already trust.

---

## 4. Grounding: real cave genesis vs. the fields this world has

Matt's instinct (mountains yes, desert plains no) is directionally right, but
the true first-order driver is **lithology plus water**, with relief third:

| genesis | needs | do we have it? |
|---|---|---|
| **Karst dissolution** (most of Earth's great cave systems) | carbonate rock + water throughput | **No lithology field exists** (§2.4). Water throughput we have twice over: bio_12 precipitation per column, and the baked **flow plane** — log₂ flow accumulation + channel/bank bits on every production tile, `FineTile::decodeFlowBlock`, **currently zero consumers** (docs/w3-channel-carving.md:212-216). |
| **Fracture/talus caves** | relief + rock structure | Yes: carrier slope/relief per column (amplifier.cpp evalSurface), and a bedding **strike field** already exists (`CH_BEDDING_STRIKE`, hash.h registry id 27) for orientation coherence. |
| **Stream/spring caves** | concentrated drainage | Yes: the flow plane again. |
| **Sea caves** | a coast | Yes (implicit ocean at z<0) — but currently *deliberately* excluded (caves.h:161-162 makes "caves cannot breach or flood from the ocean" definitional). Reopening that is real work on the water side; deferred. |
| **Lava tubes** | volcanism | No. VOLCANIC province is designed-not-built (landform-provinces-plan Tier 3); no lava material or sim (cavern-design §8 Q5 parked it). Not justifiable now. |
| **Glacier caves** | ice bodies | No ice exists. Not justifiable. |

**The lithology question, answered plainly.** A karst tier needs a "soluble
rock here" field, and nothing can derive that from climate or elevation — the
provinces plan already reached this exact conclusion and prescribed the
answer: geological provinces are **hashed low-frequency overlay fields**,
because they are properties of rock, not weather, "and because they are hashed
they are also the ones whose spatial frequency we control"
(docs/landform-provinces-plan.md:40-44, :213-215). KARST was designed and
deliberately cut from Tier 1. So the proposal is to introduce a **runtime
lithology overlay**: a pure integer hash field over world position (~50–100 km
patches with smooth-blended margins), voxel-core-owned, no tile-format change,
no bake dependency. Cost: two new hash channels, a kWorldGenVersion bump it
would share with everything else here, and an honest admission that the field
is *placed*, with its **expression** caused — dissolution only where the
placed carbonate meets real water (precip × flow). Two recorded traps that
bound its scope:

* **The B2a/B4b trap.** Surface-level karst (closed depressions, disabled
  depression fill) gated at B2a is silently undone because **B4b re-fills the
  whole domain afterwards** (docs/worldgen-variety-plan.md:311-316); if
  bake-side karst is ever attempted the mask must come from the **superblock**
  raster so coarse and fine hydrology agree about where sinks are allowed.
  Everything in §5 except W8 therefore stays strictly **below** the baked
  surface, where the bake's hydrology guarantees are untouched.
* **The validation gap.** The Earth reference corpus has **no karst site**
  (terrain-service/tools/earth_reference.py:1755-1757, "deliberately omitted
  … worldgen has no karst term"). The corpus needs a doline-field reference
  DTM *before* karst tuning starts, or karst cannot be judged by the standing
  method (docs/worldgen-variety-plan.md:346).

**Province coupling at runtime.** The bake's province weights (FLUVIAL /
GLACIAL / ARID / LOWLAND,
`terrain-service/terrain_service/bake/province.py:province_fields`)
are float fields computed bake-side and are not shipped in tiles. Rather than
mirror that float classifier into integer voxel-core (a drift-prone second
implementation of a thing the plan says must never be reimplemented), the
runtime coupling below uses the **same inputs the weights are derived from** —
climate planes + relief, both already blended per column in the amplifier —
plus the new hashed lithology overlay. That keeps bake and runtime agreeing on
*causes* without sharing a classifier.

---

## 5. The redesign

### 5.0 Shape of the whole thing

Keep the load-bearing skeleton — lattice graph, structural connectivity,
depth-space roof guarantee, per-column reduction to `dz² < marginSq` segments,
absolute-z rooms and water — and change three things:

1. **Break the tells** (§2.6): waypointed winding passages, varying calibre,
   asymmetric rooms, no naked cylinders.
2. **Multiply the families** (§3): passages, chambers, crevice/fracture sets,
   crawlways and entrance forms as distinct overlapping generators with
   different characteristic shapes.
3. **Give every density a cause** (§4): relief, climate, flow and lithology
   fields modulate which families fire where, so a player can see *why* the
   underground changed.

All of it stays a pure integer function of (seed, position, tile rasters), all
of it per-column reducible, all of it mirrored in `worldgen.ush`.

### 5.1 Generator set

**G1 — Passages (rework of the tunnel lattice).**
Keep nodes, backbone and gated edges. Change the edge geometry:
* **Waypointed axes:** each edge gains 1–2 hash-jittered interior waypoints
  (lateral offset bounded to keep the 3×3 candidate block exhaustive — the
  bound is a static_assert like caves.h:203), turning one straight capsule
  into a 2–3-segment polyline. Per-column cost: the same closest-approach
  reduction over 2–3 sub-segments instead of 1; per-voxel cost unchanged.
  Because both endpoints stay the nodes, connectivity proofs are untouched.
  This also **kills the direction lock**: a waypointed +x edge wanders tens of
  degrees off-cardinal, and long sightlines down a backbone row vanish.
* **Calibre variation:** radius hashed **per node end** and interpolated along
  the edge (plus the existing per-edge draw as the mid value) — passages
  swell and choke. Range widens from [1.2, 2.8] to roughly [0.8, 4.0] m with
  the field coupling of §5.2 deciding where the wide end of the range lives.
* **Vertical undulation:** node depth stays the anchor, but the interpolated
  axis depth gains a low-amplitude hash sinuosity term (bounded so
  static_asserts on roof/bedrock still close). Passages rise and dip instead
  of ramping linearly.

**G2 — Chambers (rework of caverns).**
* **De-coaxial the chain:** children 1–3 regain a bounded horizontal offset
  (the original cavern design §3.2 had ±8 m before the coaxial simplification;
  the overlap proof reverts to "offset < sum of min radii", a static_assert,
  cavern-design.md:168-189). Rooms stagger down a slope instead of stacking on
  one axis.
* **Break plan-view symmetry:** rx ≠ ry plus a hashed orientation from the
  same 16-direction table detail_rill already uses (detail_rill.h) — elongated
  rooms without trig.
* **Pillars:** inside a room's reach disc, sparse hash-placed vertical stone
  columns (an *additive* per-column segment that re-solidifies, exactly the
  cheap inverse of a carve seg; Minecraft's pillars term is the model). Big
  rooms stop reading as empty ellipsoids.
* **Breakdown floors:** floor clamp keeps water well-defined; add hash rubble
  mounds (small positive segments above the floor) so floors are not planes.

**G3 — Fracture sets (rework of crevices).**
Crevices stop being a uniform 1-in-8 decoration: their gate and size couple to
**relief** (steep, high-relief columns are where fracture caves belong) and
their orientation aligns to the **bedding strike field** (CH_BEDDING_STRIKE)
rather than the host edge, so a region's fissures share a fabric direction the
player can read as geology. On high relief they may span multiple edges
(longer slabs, same per-column reduction).

**G4 — Crawlways (new, the "noodle" analogue).**
A second lattice at half the spacing (12.8 m) carrying thin (0.4–0.8 m radius)
waypointed tubes, **regionally gated** by a low-frequency hash noise so whole
areas have them and whole areas don't, and depth-banded to connect the passage
band down toward the chamber band. They are the "you can just barely fit"
texture Minecraft gets from noodle caves. Connectivity: crawlway nodes that
coincide with parent-lattice nodes (every 2nd) inherit the backbone argument;
isolated crawlway fragments are acceptable *only* because they are a strict
subset gated to regions that already have the connected network (and the
flood-fill share in test_caves stays the acceptance gate).

**G5 — Entrances (replaces the uniform cylinder shafts).**
The three §2.3 guarantees stay; the *shape and the cause* change. Same gated
backbone-crossing anchor nodes, same sparse rate discipline (measured
perforated fraction), but the construct drawn at an open node depends on the
place:

* **Doline funnel** (karst areas, gentle ground): an inverted cone from a
  surface bowl (2–6 m across) tapering to passage width at node depth, with a
  hash-tilted axis (up to ~15° off vertical). The surface expression is a
  visible depression, not a clean hole. The funnel is carved in depth space by
  the same shaft mechanism — one extra radius-vs-depth ramp — so it is still
  the ONE roof-clamp exception, still lands on a backbone node.
* **Collapse mouth** (over shallow chambers): where a G2 room's roof cover is
  under a threshold at an open entrance node, the entrance is a wider, rubble-
  rimmed crater into the room. Cause: the void below.
* **Slope mouths** (steep ground): already free from depth space; G1's
  calibre variation makes them irregular. On high-relief columns the entrance
  gate *prefers* to spend its budget here (a mouth-widening term rather than a
  new bore), which is exactly "caves and crevices are much more of a thing in
  mountainous regions."
* **Stream sinks** (karst + high flow accumulation): where the flow plane's
  channel bit is set near an open entrance node, the funnel snaps to the
  channel cell — a creek that visibly disappears underground. (Below-surface
  only; the baked hydrology is not modified — W8 is the version of this that
  would be.)

Entrance **rate** becomes field-driven (karst and mountains get more,
plains/arid get few), with a floor: the average "≥ 1 entrance per N m²
connected to the backbone" guarantee is kept per lithology class rather than
globally uniform, and the perforation stat in test_caves becomes per-class.

**G6 — Underground water (extend, don't replace).**
Per-site flood levels generalise to a **quantized regional water table**:
level hashed per cavern coarse cell (204.8 m grid, the cell structure already
exists) with the per-site value clamped toward its cell level — nearby sites
stop disagreeing arbitrarily, perched pools still occur, and deep storeys
flood more often than entrance chambers. Precipitation (bio_12) biases the
wet-site fraction: arid ground has drier caves. No CA change: mobilization is
already the shipped handoff, and `kWaterCAVersion` does not move. This item
must be coordinated with `docs/watershed-system-plan.md` (2026-08-03): once
surface streams are real, "where does a sinking creek's water go" stops being
rhetorical, and the honest v1 answer is *nowhere visible* — stream sinks are
entrance geometry only, with cave-to-watershed routing left to that plan's
machinery rather than invented here.

### 5.2 The field coupling (what pushes what, and why)

Inputs, all already per-column in the amplifier or pure hash: climate
(bio_1 temp, bio_12 precip, bio_15 precip seasonality — amplifier.cpp:185-249),
carrier relief/slope (evalSurface), flow plane (fine tier; §5.4 for the coarse
story), lithology overlay (new, hashed), plus surfaceMm as today.

| field | pushes | physical reading |
|---|---|---|
| lithology = CARBONATE × wetness (bio_12) × throughput (flow log₂) | G1 density + calibre up, G2 site rate up (multi-storey), G5 dolines + stream sinks | karst: dissolution needs soluble rock AND water moving through it |
| relief/slope high | G3 fracture rate + size up, G5 slope-mouth budget up, G1 vertical sinuosity up | mechanical fracture, cliff daylighting, steep hydraulic gradients |
| bio_12 low (arid) + lithology ≠ carbonate | everything sparser; G6 drier | no water, no dissolution — Matt's desert-plains intuition, made causal |
| cold (bio_1) + high relief | G3 up, G1 unchanged | frost shattering; (glacial caves stay out — no ice) |
| flow-plane channel bit + carbonate | G5 stream sinks | creeks sink where limestone crops out |
| low relief + non-carbonate + dry | near-barren underground | some places SHOULD be boring — that contrast is what makes karst legible |

The gates are smooth (Q10 multipliers on the existing hash-gate thresholds and
radius spans, smoothstepped on the blended fields — the same shape as the
bake's province fields), so there are no hard density seams; the lithology
overlay's margins are the one deliberate "geological contact" and get a
blend band of their own.

### 5.3 Determinism, mirror, versioning — the bill

* Everything above is a pure function of (seed, position) plus deterministic
  tile rasters — same contract as today. New hash channels append at the free
  ids (29, 50+; hash.h registry + hash_channel_registry.h).
* **One batched version event:** kWorldGenVersion 23 → 24. That means, in the
  same commit series: `worldgen.ush` re-mirrored expression-for-expression,
  SPIR-V respun (`tools/compile-shaders.ps1`), goldens regenerated
  (`tools/regen-goldens.ps1`), and `kExpectedCpuDigest` +
  `kExpectedCpuDigestWorldGenVersion` re-measured and re-pinned in
  `VoxelGpuVerify.cpp:74-95` — the static_assert makes forgetting this a build
  break, and the pin's own history (stale for thirteen versions) is the reason
  it is not optional. Saved edit logs invalidate on the bump (standing
  policy, editlog.h).
* The flow plane becomes a **GPU input** for the first time (a texture fetch,
  exactly the integration docs/w3-channel-carving.md:212-219 anticipated).
  That is a real shader-plumbing task (new SRV + FillRasterWindow work), and
  it is the main reason W6 is not cheap.
* Note the standing caveat from v22: the two GPU-verify fixture regions
  cannot exercise climate-gated paths (VoxelGpuVerify.cpp v22 note), so
  field-coupled cave changes need a fixture region that actually reaches the
  gates, or the digest proves less than it appears to.

### 5.4 Coarse tier and the flow plane

The flow plane exists on fine tiles. The coarse tier (30 m synthetic/tile
path) has no flow raster, and the two tiers must not disagree about where a
cave is. Resolution, in order of preference: (a) gate flow-driven *density*
terms on quantities that exist on both tiers (climate + relief) and use flow
only for *placement of entrances* (which are sparse and fine-tier-verified);
(b) if flow must drive density, thread the superblock flow raster (120 m/px)
through the coarse path the same way climate already is. (a) is assumed below;
(b) is the fallback with its own cost line. **Open question flagged rather
than hand-waved:** exact coarse/fine parity for flow-gated terms needs a
decision before W6 starts.

### 5.5 Perf budget

Ground truth today: `caveColumnFor` is the single largest term in `column()`
(~0.23 µs, 24% at measurement time; later ~68% of what remained —
docs/status.md perf table), entirely per-column, amortised by the lattice
block memo (caves.h:320-344, amplifier.cpp:256-301). The per-voxel loop is one
multiply-compare per segment and **none of the proposals change its shape** —
that is the invariant that keeps this affordable.

| change | per-column cost | mitigation |
|---|---|---|
| G1 waypoints | ×2–3 on the edge reduction (same hashes, more sub-segments) | lattice-block memo already amortises hashing; segment math is the cheap half |
| G1/G3 field gates | climate is already read per column; relief already computed; lithology = 1–2 hash2 + smoothstep per lattice cell (memoised) | ride the existing block memos |
| G2 offsets/pillars | +1 dxy per child (loses the coaxial saving), + pillar segs | caverns fire on ~4–5% of columns (cavern-design §3.7); stays localised |
| G4 crawlways | a second, smaller lattice: ~half-scale reduction where regionally gated open | the regional gate zeroes it for whole areas |
| G5 entrances | same per-node candidate logic as shafts today | sparse by construction |
| G6 water table | +1 hash per coarse cell | trivial |
| flow-plane reads | new tile decode per pixel block | same block-memo pattern as elevation/climate (amplifier.cpp:28) |

Segment cap: `kMaxCaveSegs` (12) will be exceeded by waypointed edges +
crawlways at junction columns; the cap grows (with the same measured-headroom
test, test_caves.cpp CaveSegmentCapHeadroom) — that is ColumnSample memory
(+8 bytes/seg) and a GPU struct change, called out now so it is not a
surprise.

**The render thread is the frame at 2K** (docs/backlog.md §0.1, measured
2026-07-28: render 13.49 ms against a 13.41 ms frame), and
cave work can only add visible quads. Two consequences baked into the plan:
(1) density increases are *field-gated*, so the global average cave volume
target stays near today's (1.18% of the 6–40 m band; 13.7% of columns —
docs/status.md M4), with karst regions above it and plains below; (2) every
work item below ships with the volume/perforation stats re-measured and a
resident-quad + leg comparison at a karst-dense site before it is accepted.
Deep streaming: anything that deepens the band moves the underground
streaming footprint (§2.5) and is priced separately (W9).

---

## 6. Work list (cheapest first, each independently testable)

Every item: vxc_tests green, connectivity flood-fill share ≥ today's, volume
and perforation stats printed, and — because the owner judges screenshots —
at least one same-ground A/B capture pair per item (the visual-bisection
method that settled the banding investigation). Items W2–W7 are each a
worldgen change and go out under the **single batched v24 bump** — the A/B
harness runs per-item on branch builds, the version event ships once.

* **W1 — Instruments first (no worldgen change).**
  (a) A plan-view + cross-section cave renderer over `caveColumnFor` /
  `cavernColumnFor` (the "topmost-cave-depth field" image from the M4 landing
  generalised to depth slices) so every later item has a cheap picture that
  doesn't need the editor. (b) A `-VoxelCaveTest`-family site-list mode
  (accept a coordinate list, produce the §1 pack in one run). (c) Per-class
  perforation/volume stats in test_caves. *Verify: images exist for today's
  system; stats match status.md's recorded numbers.*
* **W2 — G1 passages** (waypoints, calibre, undulation). The single highest
  visible-change-per-effort item: it fixes tells 2 and 3 everywhere at once.
  *Verify: A/B interior captures at one fixed cave; flood-fill share; a
  plan-view image showing the direction histogram flattening.*
* **W3 — G5 entrance portfolio, minimum version:** doline funnel replaces the
  naked cylinder (same nodes, same rate), slope-mouth widening term.
  *Verify: A/B of the §1 shaft capture — same node, funnel vs cylinder; the
  surface aerial showing a bowl instead of a hole; perforation stat
  unchanged.*
* **W4 — G2 chambers** (offsets, elongation, pillars, breakdown).
  *Verify: A/B CavernShot pair; plan-view symmetry visibly broken.*
* **W5 — Field coupling v1, no new data:** climate + relief gates on G1/G3/G5
  density, calibre and crevice rate (no lithology yet, no flow yet). This
  alone delivers "mountains have more and bigger caves than desert plains."
  *Verify: tunnel-interior + aerial pairs at tundra vs savanna vs a
  high-relief site; the W1 plan-view at both showing density difference; global
  volume within budget.*
* **W6 — Lithology overlay + karst tier:** the hashed carbonate field, G1/G2
  density multipliers, dolines and stream sinks (flow plane's first consumer,
  fine tier; coarse parity per §5.4). Prerequisite: an Earth karst reference
  site added to the corpus (§4). *Verify: karst-region capture set; walk from
  a stream sink into a multi-storey system; flow-plane SRV parity in
  vxc_gpu.*
* **W7 — G4 crawlways + G6 water table.**
  *Verify: crawlway interior captures; flooded-storey capture; waterca suite
  untouched.*
* **W8 — Bake-side karst surface (deliberately last, own decision point):**
  closed depressions via the B2a gate **with the mask derived from the
  superblock raster** (the recorded trap, worldgen-variety-plan.md:311-316),
  BAKE_VERSION roll, tiles re-baked. Only this item makes the *surface*
  karst-shaped; everything before it is subsurface-only and bake-safe.
* **W9 — Depth (own decision point):** bedrock sits at 180–220 m and the
  tunnel band stops at ~37 m; a second, deeper passage+chamber storey is the
  Minecraft-like "it keeps going down" move. Priced separately because it
  grows the underground streaming footprint (§2.5), not just worldgen.

### The first milestone Matt can walk into

**W2 + W3 at the grassland tile (`-1_1`), one build:** stand at a doline
funnel (not a cylinder), descend a winding passage whose width changes around
him, reach a junction, and — the A/B — the same walk on a v23 build for
contrast. Deliverable: the capture pack (funnel from surface, funnel from
below, two passage interiors, one junction) plus the live build. That is the
smallest thing that answers both "computer made" complaints and the shaft
complaint at once; the biome coupling (W5/W6) is the second visit.

---

## 7. Open questions for Matt — answers change the plan, so they are asked, not assumed

Each of these is a direction call, not something the code can answer. The plan
above picks a defensible default for every one; a different answer reshapes
the work list, so cheapest to answer them before W2 starts.

1. **Encounter rate, and its flip side.** Today the underground is uniform:
   one entrance per ~205 m square everywhere, caves under 13.7% of columns
   everywhere. The field-coupled design makes it *regional*: karst and
   mountain country dense (more than today), dry plains on plain rock nearly
   barren. That contrast is what makes the variety legible — but it means
   whole areas where a player finds almost nothing underground. How much of
   the world should be cave-rich, and is "boring underground here, on
   purpose" acceptable? (Default assumed: yes, with the global average held
   near today's.)
2. **What are caves FOR?** Traversal space (routes through mountains —
   passages/G1 matter most), resource space (something worth going down for —
   chambers/G2 and depth/W9 matter most), hazard space (crawlways, water,
   drops — G4/G6), or a mix? The generator set survives any answer but the
   ORDER of W4–W7 doesn't. (Default assumed: traversal + spectacle first,
   W2→W3→W4.)
3. **Depth.** Bedrock is at 180–220 m; tunnels stop at ~37 m, cavern chains
   at ~100 m. Should the world go deep (layered storeys, Minecraft-like
   "it keeps going down"), and should a player be able to reach a bottom?
   W9 prices this separately because it grows the underground **streaming**
   footprint, not just worldgen. (Default assumed: deferred decision; nothing
   above depends on it.)
4. **Flooded caves.** The per-site flood + dig-to-drain mobilization already
   ships; G6 upgrades it to regional water tables and couples it to
   precipitation, and the watershed plan makes surface water real. Wanted, or
   should caves stay mostly dry until the watershed work settles? (Default
   assumed: wanted, sequenced after W6 and coordinated with that plan.)
5. **Is a lithology field acceptable as new work?** Real cave genesis is
   rock-type-driven; karst dissolution caves — the most natural-reading cave
   systems on Earth — cannot exist without a "carbonate here" field, and §4
   argues a hashed overlay is the honest way to get one (the provinces plan
   reached the same conclusion and cut it from Tier 1 on scope). It is a
   substantial addition: new worldgen field, karst reference DTM for
   validation, flow-plane GPU plumbing. In or out? (The plan carries it as
   W6; without it, W5's climate+relief coupling still delivers the
   mountains-vs-plains contrast, and dolines lose their cause.)
6. **How much may the underground change the SURFACE?** Dolines, collapse
   craters and sinking streams are the visible half of karst and what makes
   it read as natural from above — but the surface is currently the bake's
   business, and bake-side karst has a recorded trap (B2a gate undone by
   B4b; superblock mask required — §4). W3's funnels perturb only the top few
   metres at sparse points; W8 is the real surface-karst decision with a
   BAKE_VERSION roll and re-baked tiles attached. Where is the line?
   (Default assumed: W3-level perturbation yes, W8 parked for its own
   decision.)

## 8. What this plan does NOT do

* No 3D-noise carving: the cavern design's rejection (connectivity,
  determinism, per-voxel cost, cavern-design.md §1.2/§3.8) is upheld;
  Minecraft's *effect* is reproduced by overlapping closed-form families.
* No float anywhere in voxel-core: every construct above is hash draws,
  integer polylines, Q10/Q16 multipliers and floorDiv — the same idiom as the
  existing passes.
* No promise that the current captures are "bad" — that call is Matt's, and
  §1 exists so he can make it (and later, unmake it) on evidence.
* No sea caves, lava tubes or glacier caves in this pass (§4 says exactly why
  each is out and what would have to exist first).
