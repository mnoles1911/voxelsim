# Underground system redesign — caves, tunnels, caverns

**Status:** PLAN, revised same-day against **Matt's answers to §7** (all six
answered 2026-08-03; three changed this plan's assumed defaults — the deltas
are recorded in §7). Written against `main` at v23 (`kWorldGenVersion = 23`,
core.h:236). The §1 evidence pack is being re-shot from the labelled backlog
now that the box's editor is free again. Author: agent session 2026-08-03,
from Matt's verbatim complaint below.

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
connectivity theorem. Details and quotes in §2.3. **Matt has since named the
entrance portfolio himself, almost item for item (§7 Q6), so it is this
plan's headline deliverable, not a sub-item.**

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
| Sinkhole shaft from below (the owner's vertical-shaft complaint, photographed up the bore), grassland tile `-1_1`, `-VoxelSpawnAt=-7680,23040 -VoxelGICaveTest -VoxelGICaveSettle=120` | `caves-shaft-updaylight-grassland-0.png` — **CAPTURED** (re-shot with the box exclusively free) | `jobsInFlight=0 pendingJobs=0 pendingUnload=0` (`loaded=60071 … deepTracked=22258`) | Shaft at `(-7670,23150)` m, cave floor 21.9 m below daylight; camera in the cave looking 55° up the bore at the lit mouth. The §2.3 vertical cylinder, from the bottom. |
| Shaft mouth from the air (the same shaft seen from the surface) | **NOT YET CAPTURED** — parse `sinkhole shaft at (X,Y)` from the previous run's log, then `tools\voxel-capture.ps1 -SpawnAt 'X/100,Y/100' -SpawnAltM 45 -SpawnPitch -62 -SettleSec 240` | — | The clean uncased hole in undisturbed ground — no funnel, no debris, no cause. |
| Tunnel interior, grassland tile `-1_1` (second biome), `-VoxelSpawnAt=-7680,23040 -VoxelCaveTest -VoxelCaveSettle=120` | `caves-tunnel-A-grassland-0.png` — **CAPTURED** | `jobsInFlight=0 pendingJobs=0 pendingUnload=0` (`loaded=47709 … deepTracked=19019`) | 2.5 m-tall void, 21.8 m below surface. Paired with the savanna interior ~146 km away: same generator, no biome input (§2.4) — the biome-insensitivity evidence, now on film. |
| Tunnel interior, tundra tile `-3_-3` (third biome), `-VoxelSpawnAt=-38400,-38400 -VoxelCaveTest -VoxelCaveSettle=120` | `caves-tunnel-C-tundra-0.png` — **CAPTURED** | `jobsInFlight=0 pendingJobs=0 pendingUnload=0` (`loaded=56843 … deepTracked=19039`) | 4.5 m-tall void, 21.0 m below surface, third biome, same character. |
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
  (`VoxelWorldSubsystem.cpp:13849-14290`). **Fixture bug found while
  shooting:** its `-VoxelCavernAt=<X>,<Y>` origin never survives parsing —
  `FParse::Value` for an FString stops at the comma by default, unlike
  `-VoxelSpawnAt`, which passes `bShouldStopOnSeparator=false`
  (VoxelEarthGameMode.cpp:76). Unquoted, the origin silently falls back to
  (0,0); on this tile cache that is off the baked set and the fine-tier gate
  kills the run. Workaround: quote the value on the command line
  (`-VoxelCavernAt="-7680,23040"`); fix: pass
  `bShouldStopOnSeparator=false` like the spawn parser does.

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
  it head-on (§5, item W9).
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
  Everything in §5 except W9 therefore stays strictly **below** the baked
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

**G5 — The entrance portfolio (THE HEADLINE — listed first in spirit even
though numbered fifth; the other generators exist so that these entrances
lead somewhere worth entering).**

A reading that must be stated, not silently resolved: Matt's Q5 answer says
"no sinkhole entrances" and his Q6 answer lists "sinkholes" among the
entrance kinds he wants. This plan reads Q5 as rejecting the **current
construct** — the hash-placed, perfectly vertical, uncased cylinder with no
funnel, no debris and no cause — and Q6 as naming the landforms that should
replace it. If that reading is wrong, one line from Matt corrects it and the
doline/sinkhole items below come out; nothing else in the plan depends on
them alone.

The three §2.3 guarantees stay; the *shape and the cause* change. Same gated
backbone-crossing anchor nodes, same sparse rate discipline (measured
perforated fraction), but the construct drawn at an open node depends on the
place. The four kinds, exactly the four Matt named in Q6:

* **Horizontal mouths in mountainsides** (steep ground): already free from
  depth space (caves.h:64-68); G1's calibre variation makes them irregular,
  and on high-relief columns the entrance gate *spends its budget here* — a
  mouth-widening term (flared aperture, sometimes into a G2 entrance chamber)
  rather than a new bore. This is "caves are much more of a thing in
  mountainous regions" made mechanical.
* **Dolines and collapse craters** (gentle ground): an inverted cone from a
  surface bowl (2–6 m across) tapering to passage width at node depth, axis
  hash-tilted up to ~15° off vertical — a visible depression with a cause,
  not a clean hole. Carved by the same shaft mechanism (one extra
  radius-vs-depth ramp), so still the ONE roof-clamp exception, still landing
  on a backbone node. Collapse variant: where a G2 room's roof cover is under
  a threshold at an open node, a wider rubble-rimmed crater into the room —
  the void below is the visible cause. Dense in karst; rare and subdued
  (soil-pipe scale) outside it.
* **Sinkholes** in the landform sense are the doline/collapse items above —
  bowls and craters, never the current naked cylinder, per the Q5/Q6 reading.
* **Stream sinks / swallets** (the most causally-motivated of the four): where
  the flow plane's channel bit is set near an open entrance node, the funnel
  snaps to the channel cell — a streambed that visibly disappears underground
  into the system. The flow plane already ships on every tile and has zero
  consumers (docs/w3-channel-carving.md:212-216), so this entrance is driven
  by data that already exists, and it is the entrance a player *finds by
  following terrain* rather than by luck. Below-surface carve only in v1; the
  baked hydrology is not modified (W9 is the version that would), and where
  the sunk water goes is deliberately left to the watershed plan (§7 Q4).

**Flat ground, answered plainly rather than waved at.** Horizontal mouths do
not exist on flat terrain — that is what the cylinders were for. Under the
new scheme: flat **karst** ground gets dolines and swallets in numbers (that
is where dolines belong on Earth); flat **non-karst** ground becomes
**entrance-poor on purpose** — a low, non-zero floor of subdued collapse
entrances keeps the per-region "findable on a walk, connected to the
backbone" guarantee alive at a much longer walk, and the barren-underground
contrast is the approved Q1 trade. The per-class perforation stat in
test_caves is the acceptance gate that the floor actually holds.

**G6 — Underground water: PARKED (Matt, Q4: "stay dry until watershed work
is done").**
The designed extension — quantized regional water tables per cavern coarse
cell, precipitation-biased wet fractions — is recorded here and deliberately
NOT scheduled. No new water coupling of any kind ships with this redesign;
coordination happens against `docs/watershed-system-plan.md` (being built now
on `claude/watershed-build`, BAKE_VERSION 7→8 landed there) once that work
settles. One explicit decision rides with parking it, flagged rather than
made silently: **v23 already floods 60% of cavern sites**
(caverns.h:265-272). "Stay dry until watershed" can mean (a) leave the
shipped per-site floods as they are, or (b) raise the dry threshold to ~100%
during the redesign (a one-constant change inside the same v24 bump). This
plan assumes (a) — do not touch shipped behaviour without being asked — and
Matt can flip it in one line.

### 5.2 The field coupling (what pushes what, and why)

Inputs, all already per-column in the amplifier or pure hash: carrier
relief/slope (evalSurface), flow plane (fine tier; §5.4 for the coarse
story), temperature (bio_1, amplifier.cpp:185-249), lithology overlay (new,
hashed), plus surfaceMm as today. **Precipitation coupling is deliberately
absent**: it was this plan's original wetness term, and it is parked with the
rest of the water story (Q4, "do not couple to precipitation yet") —
water *throughput* comes from the flow plane, which is terrain-derived and
already shipped, not from climate. When the watershed work settles,
re-introducing a precip bias is a tuning pass, not a redesign.

| field | pushes | physical reading |
|---|---|---|
| lithology = CARBONATE × throughput (flow-plane log₂ accumulation) | G1 density + calibre up, G2 site rate up (multi-storey), G5 dolines + swallets | karst: dissolution needs soluble rock AND water moving through it — **this pair is what delivers Matt's Q6 "caves in both karst and non-karst areas, the latter rarer and smaller": two different KINDS of system, not one system at two densities** |
| relief/slope high | G3 fracture rate + size up, G5 horizontal-mouth budget up, G1 vertical sinuosity up | mechanical fracture, cliff daylighting, steep hydraulic gradients |
| cold (bio_1) + high relief | G3 up | frost shattering; (glacial caves stay out — no ice) |
| flow-plane channel bit + carbonate | G5 stream sinks | creeks sink where limestone crops out |
| low relief + non-carbonate | near-barren underground (low entrance floor, small fracture caves only) | some places SHOULD be boring — that contrast is what makes karst legible, and Matt approved it (Q1) |

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
streaming footprint (§2.5) and is gated by W8's streaming acceptance measurement (§5.7).

---

### 5.6 What the generator set owes exploration — and the mobs that are coming (Q2)

Matt's answer to "what are caves for": *"Traversal, spectacle, exploration,
eventually fighting mobs."* Not primarily a resource space. Exploration and
mobs each place obligations on the generators, and they are cheaper stated
now than retrofitted:

**Exploration (legibility, landmarks, a sense of going somewhere):**
* **Distinct place-shapes** are the landmark budget: a junction node, a
  chamber with pillars, a crevice slot, a swallet shaft of light — each
  family reads differently, so "I've been here" and "this leads somewhere
  new" are visually decidable. This is an argument for the overlap-of-
  families design over any single tuned generator, independent of realism.
* **Gradient toward reward:** passages widen toward chambers (G1 calibre
  interpolation makes that free: bias the node-end radius up near open
  cavern sites), so following the growing passage is a learnable strategy —
  the underground equivalent of "caused variety."
* **Orientation:** daylight shafts of the doline/collapse entrances and the
  strike-aligned fracture fabric (G3) give directional cues; the old
  everywhere-the-same lattice gave none.

**Mobs (not a generator, but a client of every generator):**
* **Navigable floor and headroom guarantees.** Tunnels are already 2.4–5.6 m
  bores and cavern floors are already flat-clamped (caverns.h:222-223) —
  keep both as *invariants* under G1/G2 rework: minimum walkable calibre on
  backbone edges, flat-floor area preserved in chambers. `pathfind.h` prices
  air cheap, so the connected component IS the mob navmesh; the flood-fill
  share test is therefore a gameplay guarantee, not just a worldgen stat.
* **Spawn surfaces.** Chamber floor area and crawlway mouths are the natural
  spawn sets; the W1 instruments should print per-family floor-area stats so
  a future spawn system has a measured surface budget instead of a guess.
* **Sightlines and cover.** Waypointed passages (G1) create corners; pillars
  (G2) create cover. Both are combat affordances the current straight
  capsules and empty ellipsoids lack — the same changes that fix the look
  fix the fight.
* **No involuntary pits on main routes.** Storey-to-storey connections on
  the backbone must be traversable both ways (inclined passages, collapse
  ramps), never sheer drops; drops are allowed off-backbone as deliberate
  hazard/spectacle. This constrains §5.7's connector geometry.

### 5.7 Depth: layered storeys and a rare reachable bottom (Q3 — now IN)

Matt: *"Yes keep going down — can reach bottom in rare instances."* Bedrock
sits at 180–220 m (amplifier.cpp bedrock band); today's tunnels stop at
~37 m and cavern chains at ~100 m, so more than half the rock column is
empty. The design:

* **Storey 1 (9–37 m):** the reworked G1/G3 band, unchanged in depth.
* **Storey 2 (~40–110 m):** the G2 chamber band plus a second, sparser
  passage lattice at a coarser spacing (51.2 m) linking chamber sites —
  same lattice mathematics, different constants, its own hash channels.
* **Storey 3 (~110–175 m, karst-weighted):** rare, large galleries; mostly
  reached through storey-2 chains. Outside karst, storey 3 barely exists.
* **The bottom, rare by construction:** a 1-in-N gate (tuned to "rare
  instances", e.g. one per several km²) on deep-storey sites drops a final
  chamber whose floor is the bedrock margin itself — the player stands on
  the world's basement rock. Reachability rides the same structural-
  connectivity argument as everything else: the chain anchors on a backbone
  node. All three bedrock guards stay untouched (caves.h's two + the
  MAT_BEDROCK refusal in materialAt, amplifier.cpp:2356-2361) — the floor
  is *reached*, never *breached*.
* **Connectors:** inclined ramp passages and collapse chimneys between
  storeys, traversable per §5.6's no-involuntary-pit rule on backbone
  routes.

**The explicit streaming price (this is why depth was priced separately).**
The underground streaming footprint is derived from the carve envelope: the
38.4 m near band exists because the deepest cave voxel is ~36.8 m down
(docs/status.md "Underground streaming"). Deepening the envelope to ~175 m
multiplies the potential deep chunk volume by ~4.5×; the mitigations are that
the deep set is only tracked when the anchor is underground and deep
(`deepTracked` already exists — the savanna capture ran at deepTracked=18923
for a 15 m cave), that storeys 2–3 are sparse by construction so most deep
chunks mesh to zero quads, and that the deep sight radius
(`voxel.Stream.UndergroundSightM`) bounds the active volume. The acceptance
gate for W8 is therefore a **leg + capture pair at a deep karst site**
showing frame time and resident-quad counts against the standing perf
records, before any constant ships. If the measured cost is unacceptable,
the fallback is fewer, larger deep voids (cheaper per visible volume than
many small ones — greedy-mesher quads scale with surface, not volume).

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
  perforation/volume stats in test_caves, plus per-family **floor-area**
  stats (§5.6 — the future spawn system's surface budget). *Verify: images
  exist for today's system; stats match status.md's recorded numbers.*
* **W2 — G1 passages** (waypoints, calibre, undulation). The single highest
  visible-change-per-effort item: it fixes tells 2 and 3 everywhere at once.
  *Verify: A/B interior captures at one fixed cave; flood-fill share; a
  plan-view image showing the direction histogram flattening.*
* **W3 — G5 entrance portfolio, minimum version (the headline lands here):**
  doline funnel replaces the naked cylinder (same nodes, same rate) and the
  horizontal-mouth widening term ships for steep ground. Swallets wait for
  W6 only because they need the flow-plane plumbing.
  *Verify: A/B of the §1 shaft capture — same node, funnel vs cylinder; the
  surface aerial showing a bowl instead of a hole; a mountainside mouth
  capture; perforation stat unchanged.*

  **LANDED at kWorldGenVersion 25, with two plan assumptions contradicted by
  measurement.** One construct, not four: an entrance CAVITY with a level floor
  at absolute z and a lens roof CLIPPED BY THE REAL GROUND. Nothing in it
  branches on landform — the terrain does the branching, which is why it is
  caused variety and not placed variety. On flat ground the roof breaks the
  surface only in the middle and the result is a bowl with an overhanging rock
  lip (a doline); where the ground falls away the roof becomes the hillside and
  the chamber opens SIDEWAYS with a level floor (the mountainside mouth); ground
  falling along one axis stretches that opening into the streambed-capture
  geometry, which is the swallet's shape without a drop of water. The v24 bore
  survives inside it as the THROAT, which is what keeps the three guarantees
  structural rather than arithmetic.

  Contradicted:
  1. **W3 did NOT need a CaveColumn/GPU struct change of the kind expected.**
     It needed exactly one int32 — `shaftDepthMinMm` — because a roof clipped
     by the ground makes the entrance a closed depth INTERVAL instead of a
     cutoff. The union of throat and cavity is provably one interval, so no
     segment list was needed, and `GpuCaveColumn` is kernel-local so no
     cross-kernel layout moved. Per-voxel cost: one extra compare.
  2. **"Tunnels daylight sideways on steep slopes for free" (§2.1, caves.h's own
     header) was FALSE.** vxc_caveprobe measured it at the grassland site:
     sideways-daylighting columns numbered exactly the perforated shaft columns,
     i.e. zero mouths existed that were not a vertical hole seen from below. A
     tunnel axis is ≥ 9 m under its own column, so ground a metre away has to
     fall 9 m in 1 m for the claim to hold — a cliff, not a hillside. The
     mouths in this item are therefore NEW capability, not a tuning of an
     existing one, and caves.h now records the falsification in place of the
     claim.

  Measured, seed 20260719, grassland tile `-1_1`, 409.6 m box, v24 → v25:
  entrance sites and gate UNCHANGED by construction (same nodes, same 1-in-4;
  1047 of 4225 candidates open over a 13 km grid, and all 1047 are daylit at the
  axis AND carved continuously from daylight to their backbone node);
  carved volume 1.709% → 1.734% of the band (+1.5%); perforated surface
  0.0092% → 0.084%, still an order under the 0.5% ctest gate; sideways
  daylighting mouths 24 → 477 (v24's 24 were exactly its 24 perforated
  columns, i.e. none); entrance footprint 0.31% of the surface, of which 74% is
  ROOFED and 55% carries less cover than the 6 m roof clamp — that last number
  is the horizontal-mouth signal, and it was structurally 0 at v24. Standable
  floor inside entrances 0 → 613 spots. Connectivity at a real entrance did not
  regress: largest component 81.76% → 82.98%, sky-reachable share 81.76% →
  82.98%, portal samples 50 → 782.

  **And a third fixture-coverage trap, sprung and caught the same day.**
  `vxc_gpu`'s cave-band fixture asserted in a comment that a gated-open sinkhole
  node was inside it. It was not, and never had been: entrance candidates are
  102.4 m apart and that dispatch is 6.4 m across. v25 moved every entrance in
  the world and the harness digest did not change one bit. Closed by an
  entrance-bearing fixture placed on a site found with `vxc_caveprobe
  --synthetic` (15,335 entrance columns of 16,384; CPU/GPU bit-exact on an AMD
  RX 7800 XT over 10.1M cells) and, so it cannot recur silently, by a per-region
  "exercised: N cave / N entrance / N cavern" line printed at every run. The
  same line shows the CAVERN pass is still exercised by NO vxc_gpu fixture at
  all — that one is left for W4, stated rather than closed.

  Still open from this item: the UE-side `kExpectedCpuDigest` fixtures contain
  no entrance either, so the pin is a statement about surface worldgen only; it
  was shown differentially not to move (the ten pre-v25 harness fixtures
  reproduce v24's digest byte for byte) and the version pin was rolled to 25
  with the module recompiled. **No rendered frame exists for any of this** —
  the box's editor was owned by another agent for the whole session, so every
  *Verify* capture above is still outstanding.
* **W4 — G2 chambers** (offsets, elongation, pillars, breakdown).
  *Verify: A/B CavernShot pair; plan-view symmetry visibly broken.*

  **LANDED at kWorldGenVersion 26.** All four terms shipped, and the wave's
  real content is the second half of the Verify line rather than the first:
  "plan-view symmetry visibly broken" is a claim about a NUMBER, so W4 built
  the number and then built the control that makes it mean anything.

  What changed. The v25 chamber was, provably, one roughened DISC in plan:
  every room shared the anchor's xy (coaxial), rx == ry so every room was
  round, and the only asymmetric term — the wall-roughness noise — was a
  boundary wobble shared by all four rooms. Four rooms drew four concentric
  circles. v26: children 1-3 step SIDEWAYS as well as down; every room is an
  ellipse with its own hashed heading (one of 8, as an integer Q12 cos/sin
  pair) and elongation ratio (1.17-2.15 : 1); a world-space field of pairwise
  disjoint PILLARS is left standing inside the rooms; and a per-column rubble
  rise turns the flat floor into a breakdown surface. `cavernCarveAt` is
  byte-for-byte the v25 per-voxel predicate — the whole change lives in the
  per-site and per-column tiers, and vxc_bench measures no amplify-stage cost
  (429.6 vs 429.0 ms at 8^3, 328.9 vs 339.4 ms at 16^3, i.e. inside noise).

  **THE COAXIAL PROOF HAD TO BE REBUILT, and one constant is load-bearing
  because of it.** v25's connectivity argument was that dxy == 0 makes two
  ellipsoids' overlap EXACTLY the 1D test |dz| < rz0 + rz1. An offset kills
  that and there is no simple exact overlap test for two ellipsoids at a
  general displacement. The replacement is a WITNESS COLUMN — the child's own
  axis, where the child contributes its full rz and the parent contributes
  h = rz_parent·sqrt(1 - dEff²/R²) — which is again a 1D test and again
  static_assert-able against hashed minimums. Two things exist only to make it
  hold: the sideways step is taken ALONG THE PARENT'S LONG AXIS (so the
  elongation ratio never amplifies dEff, and dEff is just the step length),
  and `kCavernRzDeepMinMm` rose 12 m -> 16 m so a deep room's rz exceeds the
  largest possible step by a 2 m overlap floor. That floor is not decoration:
  at 0 the chain would be "provably connected" through a hairline a 100 mm
  voxel grid can round away. Measured on 60 real sites, the tightest observed
  room-to-room overlap at the child axis is 7.96 m against the 2.00 m floor.
  Pillars get two independent guards, both asserted: discs are pairwise
  DISJOINT (so the field can never tile the plane), and pillars are suppressed
  within 4 m of every room axis — wider than the worst-case 3.03 m throat, so
  no draw can put rock in the chain's neck or on the anchor point the tunnel
  network meets. The chain's DEPTH envelope does not move (the flat-floor
  clamp bounds the bottom, not rz, and breakdown only ever raises floors).

  **THE STATISTIC, AND WHY IT IS A DIFFERENCE.** A one-arm asymmetry number
  would have repeated W3's mouth-metric error exactly: a v25 cavern footprint
  ALREADY reads asymmetric wherever the roof clamp truncates it against a
  slope, the bedrock clamp cuts its bottom off, or the sampled region clips it.
  So `cave_families.h` gained `cavernSiteWithoutChamberShape` — v25's coaxial,
  round, pillar-free, flat-floored chamber out of the v26 world, built the same
  way the W3 entrance control is (neutralise fields of the shipping struct; no
  worldgen edited, nothing mirrored, no digest can move) — and every statistic
  is reduced twice on the same columns, terrain, seed and shipping predicate.

  Measured, seed 20260719, synthetic sampler, at the cavern site on lattice
  (24,-24), world (625.6, -588.8) m (v26 shipping vs the v25-shape control):
  plan anisotropy **1.47:1 vs 1.01:1**; room-to-room stack drift **14.2 m vs
  1.0 m**; enclosed rock islands in plan — which is what a pillar IS, measured
  topologically rather than by asking the pillar field whether it fired —
  **11 vs 0**; half-turn defect 213/1000 vs 35/1000; floor step within a room
  **47 mm vs 0 mm**. At the second site, lattice (-16,-16): 1.83:1 vs 1.09:1,
  15.3 m vs 1.1 m, 0 vs 0 islands, 25 mm vs 0 mm. The ctest gate
  (`cavern_plan_symmetry_...`) runs the same pair over 12 flat sites and reads
  2.00:1 vs 1.09:1, 10.3 m vs 1.8 m, 51 vs 1 islands, 53 mm vs 0 mm.

  **Two of the four control bounds are NOT zero, and saying so is the point.**
  The floor step really is exactly 0 on the control, because a v25 floor is a
  plane. Anisotropy and stack drift are not, because the wall-roughness noise
  v25 already had wobbles a disc's boundary by up to 15% of its radius: that
  moves a slice centroid by over a metre and can occasionally pinch off a
  one-cell rock island. Those two bounds are therefore stated as the
  statistic's measured NOISE FLOOR and the gate requires the real arm to beat
  the floor by a margin, not merely to be above zero. Asserting the control was
  zero would have made the gate assert something false and read the real arm's
  margin as larger than it is.

  Also fixed, and worth recording as the same class of error one level down:
  the per-room floor statistic was first written as the column's LOWEST floor,
  and the control then read HIGHER than the shipping arm (1200 mm vs 534 mm) —
  because that number is dominated by the step where one room's footprint ends
  and the next room's deeper floor takes over, which is chain geometry and not
  rubble at all. Asked per room it is 0 on the control by construction.

  **THE CAVERN PASS IS NOW EXERCISED BY vxc_gpu, and it never was before.**
  The per-region "exercised:" line W3 added had been reporting "0 cavern
  column(s)" for every fixture since the day it was added — so
  `cavernColumnFor`, `cavernSiteFor` and `cavernCarveAt`, roughly 200 lines of
  the worldgen.ush mirror including a per-column division and a four-room
  reduction, had never been compared between CPU and GPU at all. W4 rewrote all
  of it, which is exactly the wave where that would have shipped a real
  divergence. Closed by a `cavern` fixture at voxel (6456, -6049), 96x96
  columns over the chain's own 91 m depth envelope, **found by search rather
  than by eye**: `vxc_caveprobe` gained a per-site line that scans blocks
  around each open cavern site and scores them on how close to a HALF-AND-HALF
  chamber/rock split they are, rejecting any block containing no pillar column.
  The chosen block reports 49% chamber and pillar rock inside it, and the
  harness now prints `exercised: 614 cave / 0 entrance / 4536 cavern
  column(s) of 9216`. **The site mattered as much as the block**: a third of
  cavern sites hash to no pillars at all, the first site the search was run
  against was one of them, and a fixture there would have exercised the pillar
  mirror by not containing it — the same shape of hole this whole paragraph is
  about. CPU/GPU bit-exact on an AMD RX 7800 XT over 59,520 columns and
  18.8M cells.

  The UE-side `kExpectedCpuDigest` pin did not move, and this time it was
  established by BUILDING BOTH: the harness was run at v26 with the new cavern
  fixture disabled, and again with caverns.h / worldgen.ush / core.h / the
  prebuilt SPIR-V stashed back to v25. Both print the identical digest
  `2921d08fd179c25c` over all eleven pre-existing regions, 50,304 columns and
  10,143,744 cells. Version pin rolled to 26 and the UE module recompiled
  clean.

  **No rendered frame exists for any of this.** The box's one editor slot was
  held by the watershed work for the whole session, so the *Verify* line's A/B
  CavernShot pair is outstanding. The capture sites are picked and verified
  rather than guessed: `docs/measurements/w4-cavern-site-shapes.txt` lists
  every open cavern site in a 1.6 km box with its four rooms' axes, radii,
  headings and elongations, plus whether the site has pillars at all — which
  is the thing a capture must not be wrong about.

  Still open from this item: the plan-view *half-turn defect* is the weakest of
  the four statistics (213 vs 35 per mille) because the roughness wobble
  dominates it; it is reported as a diagnostic and NOT gated on. And the
  breakdown amplitude is capped at 1.5 m by the throat argument
  (`kCavernBreakdownMaxMm < kCavernFloorDropMinMm`), which reads as 53 mm of
  floor change per metre — a gentle rubble surface, and a number the owner's
  eye should be the arbiter of rather than this note.
* **W5 — Field coupling v1, no new data:** temperature + relief gates on
  G1/G3/G5 density, calibre and crevice rate (no lithology yet, no flow yet,
  and — per Q4 — no precipitation). This alone delivers "mountains have more
  and bigger caves than desert plains."
  *Verify: tunnel-interior + aerial pairs at tundra vs savanna vs a
  high-relief site; the W1 plan-view at both showing density difference; global
  volume within budget.*

  **LANDED at kWorldGenVersion 27, PARTIALLY VERIFIED, and the underground
  work is being shelved after it** — the owner rejected the v25 entrance and
  v26 chamber SHAPES ("incredibly unnatural, stamped geometric shapes … Same
  with doline"), and `docs/underground-entrance-rework-plan.md` carries that
  diagnosis. **W5 is orthogonal to it and survives**: field coupling is about
  WHERE caves are dense and wide, not about what shape they are, and that doc
  lists it as still wanted. The numbers below outlive the shapes.

  Six gates ship, all keyed on the lattice NODE (never on the querying column
  — an edge is seen from both sides and both sides must agree), sampled at the
  node's lattice ANCHOR: G1 density 1-in-8 … 1-in-2, G1 calibre 0.875× … 1.428×
  (realised radius [1.05, 2.45] m on a plain, [1.71, 4.00] m alpine), G3
  crevice rate 1-in-16 … 1-in-3 and G3 upward reach 0.75× … 1.375× (both on
  relief PLUS a weighted frost boost), G5 entrance rate 1-in-8 … 1-in-3.

  **Measured, one world, gates off vs on** (`vxc_caveprobe --field-off`, seed
  20260719, synthetic sampler, 819.2 m box —
  `docs/measurements/w5-field-coupling-2026-08-03.txt`): carved volume
  **1.440% → 2.727%** of the 6–45 m band; tunnel **1.117% → 2.300%**; crevice
  **0.049% → 0.160%**; tunnel axis length per km² 44,473 → 53,496 m (that is
  the density term) against a 2.06× volume rise (so the rest is calibre);
  tunnel floor area 133,002 → 213,732 m²/km². Global volume is **within
  budget** — 2.727% against the 15% ctest ceiling, 5.5× of headroom — and the
  carve DEPTH envelope does not move at all.

  **Three things that did NOT move, reported because they are the honest
  half.** The thinnest non-entrance roof is 6.0 m in both arms against a 6.0 m
  clamp — that is the measurement that the new *depth-aware calibre cap* holds,
  which had to replace v26's two-constant roof static_assert (9 m of cover
  cannot hold a 4 m tube above a 6 m clamp by arithmetic). Perforated surface
  moved 5%, i.e. the ground is no more of a colander than at v26. And
  **entrance density is not demonstrated**: 20 open sites → 19, because this
  fixture's relief sits near the middle of the ramp where the gate is close to
  v26's 1-in-4. The SET changed, the COUNT did not, and no claim is made for
  that half of G5 without a real-tile run.

  **A term was tried, measured and DROPPED**: a relief multiplier on the
  entrance cavity's footprint. At 1.25× (1.5625× of area) it took the two
  roof-integrity bounds shipped since v25 from comfortable to failing —
  footprint over a cavity 3.96% → 6.18% against a 5% gate, open-to-sky 1.17% →
  1.83% against a 2% gate. The W5 line asks for G5 *density*, not size, so
  buying size by loosening a roof bound in the wave that multiplies what the
  bound guards was the wrong trade. caves.h records it where the constant would
  have been.

  **AND THE OLD BEDROCK ASSERT WAS WRONG, quietly, for three versions.** It
  bounded the deepest carve by the TUBE alone and omitted the crevice's 6 m
  downward reach, so the real envelope has been 41.0 m against an assert
  claiming 40 m ever since crevices shipped. Harmless (bedrock has been ≥ 180 m
  since v5) and never checked. v27 states the envelope over every construct
  that can carve and asserts that instead — and the widened calibre does not
  move it, because the crevice term still dominates.

  CPU/GPU **bit-exact at v27** on an AMD RX 7800 XT (59,520 columns, 18.8M
  cells, digest `ec45911f93ad4c47`); VoxelizeMain now binds ClimatePacked as
  well as ElevationMm and its dispatch cost went 11.32 → 14.16 ms over the same
  cells. `vxc_tests` 368 pass / 0 fail. UE module recompiled clean.

  **Still open, and it is the usual list plus one instrument gap.** No rendered
  frame exists (the editor was held all session), and the captures are now
  cancelled with the rework. No real-tile run, so the tundra-vs-savanna
  TEMPERATURE contrast — the one thing the frost term exists for — is
  unmeasured. The UE-side `kExpectedCpuDigest` was NOT re-measured and is
  expected to be wrong (it needs the editor; a loud block at the constant says
  so). And `--field-off` is not wired through vxc_caveprobe's connectivity
  flood fill, which reduces columns through the shipping Amplifier directly —
  so that block prints byte-identical numbers in both arms, which is
  UNMEASURED and not a null result. The structural connectivity argument is
  untouched either way: the density gate only ever moves the rate on
  NON-backbone edges.
* **W6 — Lithology overlay + karst tier:** the hashed carbonate field, G1/G2
  density multipliers, dolines and stream sinks (flow plane's first consumer,
  fine tier; coarse parity per §5.4). Prerequisite: an Earth karst reference
  site added to the corpus (§4). *Verify: karst-region capture set; walk from
  a stream sink into a multi-storey system; flow-plane SRV parity in
  vxc_gpu.*
* **W7 — G4 crawlways.**
  *Verify: crawlway interior captures; per-class volume stats within budget.*
* **W8 — Depth: storeys 2–3, connectors, and the rare bottom (§5.7 —
  ordered now at Matt's direction, no longer parked).** Ships in two halves:
  (a) worldgen — the deeper lattices and the 1-in-N bottom gate, verified by
  the W1 cross-section imager and connectivity stats; (b) the streaming
  acceptance gate — a leg + capture pair at a deep karst site against the
  standing perf records **before** the constants ship. The two halves land
  together or (a) waits.
* **W9 — Bake-side karst surface (deliberately last, own decision point):**
  closed depressions via the B2a gate **with the mask derived from the
  superblock raster** (the recorded trap, worldgen-variety-plan.md:311-316),
  BAKE_VERSION roll, tiles re-baked, coordinated with the watershed work
  which owns the bake's hydrology invariants now. Only this item makes the
  *surface* karst-shaped; everything before it is subsurface-only and
  bake-safe.
* **PARKED, explicitly:** G6 regional water tables and any precipitation
  coupling (Q4 — until the watershed system lands); sea caves, lava tubes,
  glacier caves (§4, unchanged).

### The first milestone Matt can walk into

**W2 + W3 at the grassland tile (`-1_1`), one build:** stand at a doline
funnel (not a cylinder), descend a winding passage whose width changes around
him, reach a junction, and — the A/B — the same walk on a v23 build for
contrast. Deliverable: the capture pack (funnel from surface, funnel from
below, two passage interiors, one junction) plus the live build. That is the
smallest thing that answers both "computer made" complaints and the shaft
complaint at once; the biome coupling (W5/W6) is the second visit.

---

## 7. Matt's answers (2026-08-03) and what they changed

The six questions below were asked with a stated default each; Matt answered
all six the same day. Answers are quoted verbatim; **three changed the plan**
and the deltas are marked. The original question texts are in this doc's git
history (first commit on this branch).

1. **Encounter rate.** *"Yes."* — Default held. Regional contrast approved,
   including deliberately barren underground (folded into §5.2's coupling
   table and the flat-ground paragraph in G5).
2. **What are caves for.** *"Traversal, spectacle, exploration, eventually
   fighting mobs."* — Default (traversal + spectacle first) survives, with
   two additions: **exploration is a first-class goal** and **mobs are
   coming**. New §5.6 records what the generator set owes both — landmark
   variety, reward gradients, floor/headroom/sightline invariants, spawn
   surfaces, and the no-involuntary-pits rule — so none of it is retrofitted
   later. Not primarily a resource space.
3. **Depth.** *"Yes keep going down — can reach bottom in rare instances."*
   — **CHANGED from the deferred default.** Depth is in: layered storeys and
   a rare reachable bottom, designed in §5.7, ordered as W8 with an explicit
   streaming acceptance gate (the pricing that was the reason for deferral
   is now the gate instead).
4. **Flooded caves.** *"Stay dry until water shed work is done."* —
   **CHANGED from "wanted, sequenced after W6."** G6 and all precipitation
   coupling are PARKED (§5.1 G6, §5.2); coordination target is
   `docs/watershed-system-plan.md` (`claude/watershed-build`, BAKE_VERSION
   7→8 landed there). One flagged sub-decision: v23 already floods 60% of
   cavern sites; this plan leaves that shipped behaviour untouched unless
   Matt says otherwise.
5. **Lithology.** *"Open to this but no sinkhole entrances. We need cave
   entrances to occur in very natural looking ways on our terrain surface.
   Defer to you if lithology is the right approach."* — Decision taken (by
   the coordinating session, with the deferral explicit): **the field is
   IN**, because Q6's own requirement — karst and non-karst cave systems,
   the latter rarer and smaller — is precisely what a lithology field
   expresses and is awkward to fake with climate and relief alone: those can
   modulate *density*, not produce two different *kinds* of system. The
   entrance half of this answer is handled by the Q5/Q6 reading stated at
   the top of G5.
6. **Surface expression.** *"Dolines and collapse craters are allowed. But
   there should also be sideways cave mouth entrances in which caves open
   horizontally in mountain side, sinkholes, and stream beds that disappear
   underground and flow down into cave systems. Obviously we want caves in
   both karst and non karst areas. The latter being rarer and smaller cave
   systems."* — This is the G5 portfolio, near item-for-item, and it
   promoted entrances to the plan's headline. The apparent Q5/Q6
   contradiction on "sinkholes" is resolved by a stated reading (G5, first
   paragraph), not silently. Bake-side surface karst remains its own later
   decision (W9).

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
