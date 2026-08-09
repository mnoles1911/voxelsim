# Water re-architecture: reconciling the Hybrid Voxel-PBF blueprint against the game

## Context

After weeks of baking the *shape* of flowing water and drawing it (culminating in
bake_ver 23), the owner judged the result a regression and decided to
re-architect. He supplied a blueprint: 10 cm voxel terrain + dual-state water —
Position Based Fluids (PBF) particles for active flows near the player, static
flat "sleeping" basin planes for standing water — rendered by a unified
screen-space ray marcher, with an offline basin registry (flood-fill, spillway
heights), runtime faucet/sink lifecycle (headwater faucets → streams → sleeping
basins → spillway overflow), and cross-chunk basin stitching via union-find.

This plan reconciles that blueprint against the actual codebase, verified by
three exploration passes over the bake (terrain-service), the runtime water
machinery (voxel-core + ue-project), and the renderer, plus a design pass on the
dynamic core. The owner reviewed the first draft (which proposed salvaging the
existing SWE/CA solvers) and **rejected that salvage: he is not pleased with SWE
or CA, and reaffirmed PBF as the single flowing-water solver.** The evidence
supports him — the design pass independently proved the SWE scheme structurally
cannot carry a river (integer dead zone below ~3 % slope where steady transport
is exactly zero), and the CA's dam/drain failure modes are documented and
measured. The one-line verdict of this revision:

**Static baked basins (already largely shipped, needing volume/capacity/global
identity) + ONE GPU PBF solver for all active flowing water, fed by headwater
and sill faucets and drained by basin/boundary sinks — with the authoritative
water state kept as SCALAR hydrology (basin volumes, faucet rates, graph flows)
so multiplayer replicates integers, never particles. Fluid rendering is a
screen-space surface pass over the existing polygon scene now; unified terrain
ray marching is deferred as a separable later project, not rejected.**

---

## Section-by-section verdict on the blueprint

### §1–2 Storage (`ChunkColumn {uint16 height, uint8 waterDepth}`, 32×32×256 chunks, 3D atlas) — REJECT, already better served

- Chunks are **32³ cubes** (3.2 m), not 32×32×256 columns (`VoxelCoords.h:25-28`);
  vertical residency is a surface-following shell + depth skirt, not fixed columns.
- The runtime world is a **3D material function** — column stratigraphy minus
  analytic caves and caverns carved per voxel (`worldgen.ush:2523-2530`). A 2.5D
  column struct cannot represent it.
- `uint8 waterDepth` (max 25.5 m) under-ranges measured basin depths (spills to
  ~46 m on the wet block alone).
- The 2.5D-with-depth representation the blueprint wants **already ships** as the
  baked tile format: per-column elevation + int16 water depth at 10 mm LSB +
  basin table, at 1.875 m/px (`tilestore.h:164-226`). It is the bake/wire format,
  which is where it belongs. No new storage.
- There is no GPU voxel atlas; geometry lives as packed quads in a pooled
  structured buffer. The only Texture3Ds are the GI light volumes
  (`VoxelGIVolume.h:40-58`), CPU-uploaded, invisible to materials.

### §3 Basin detection (runtime per-chunk flood fill + union-find stitching) — ALREADY EXISTS, better; adopt the *missing fields*, not the code

The bake already does this **once, offline, at 1.875 m/px over 15.36 km tiles**
(`basins.py:252-273`, invoked `pipeline.py:4809`): depression components on the
re-opened surface, spill elevation (`spill_m`), **spillway location**
(`outlet_px` — "the saddle the basin spills over"), climate-driven equilibrium
surface, per-basin hypsometry A(h). `spill_mm` and `outletX/Y` are **on the wire
today and read by nothing** (`tile_codec.py:354-356`, `tilestore.h:217-222`,
only consumer is a validation check).

The blueprint's runtime per-chunk detector would be strictly worse: its "chunk"
is 3.2 m, so a 1 km lake spans ~100,000 chunks and the union-find graph becomes
the hot path; its BFS is order-dependent in admitting cells (spillway is
discovered *during* the fill that depends on it) and its
`(chunkUid<<32 | ++staticCounter)` id scheme collides. Discard the code.

What it gets right is exactly what the bake is missing:

| Blueprint element | Status in repo | Action |
|---|---|---|
| Spillway height + location | On the wire, unread | **Consume it** (new runtime basin registry) |
| Per-basin volume / capacity | Does not exist; hypsometry computed then discarded (`pipeline.py:4819 keep_hypsometry=False`) | **Integrate ∫A dh in-bake, ship capacity** |
| Cross-tile basin identity | Does not exist; spanning basins **dropped** (hole ships filled — measured ~9 % of lake area on the wet block, `spanning-basin-loss-2026-08-07.txt`) | **Basin table v2: global ids, stop excluding spanning** (u16 tile-local ids are structurally incapable — codec enforces 0..n-1, `tile_codec.py:1096-1108`) |
| Headwater locations | Computed in-bake (`water_head_mask`, `water.py:480-525`) then **discarded** (`pipeline.py:5300`) | **Ship them** (points + Q) |
| Union-find | None in shipping code (ADR-0003:255); two offline analysis tools demonstrate the cross-tile pattern | Merge basins **bake-side at superblock level**, not runtime |

### §4 Macro-hydrology (faucets → streams → sleeping basins → spillway overflow) — ADOPT; it is the organizing architecture, and most parts exist dormant

Mapping onto the tree:

- **Sleeping basins** = the shipped lake model: one scalar `surfaceMm` per basin,
  flat by construction, near-field implicit fill + far-field sheet rects
  (`lakes.h:292-334`, `:516-548`). Zero per-tick cost, exactly the blueprint's
  "sleeping" semantics. Missing only the runtime *volume scalar* and capacity.
- **Active streams** = the blueprint's **PBF solver** (see §5 — adopted, new
  build). The dormant SWE was evaluated as an alternative and disqualified by
  its own arithmetic: stability bound caps steady transport below d/8 fill units
  per face per tick with a settle dead zone ≥ ~3 % water-surface slope
  (`swe.h:199-210`, `:406-416`) against a world whose p50 river gradient is
  0.94 % — steady through-flow is exactly zero. The CA's documented failures
  (damming a baked river does nothing; draining converts 100 % of the reach —
  `water-system-architecture.md` §6.6) close the case. The owner's judgment that
  the existing solvers are not good enough is confirmed by measurement.
- **The off-screen routing graph** ("boundary stream sink … routes the bulk flow
  vector mathematically through background memory") = **`RiverNetwork`**
  (`rivernet.h`): per-segment storage, Muskingum-class routing, `injectInflow`,
  conservation ledger — live in UE behind `voxel.Water.Rivers=false`. Reused as
  the scalar authority fabric; graph rebuilt from baked data instead of its own
  client D8, and persisted.
- **Basin despawn sink** = particle enters a basin's extent → despawn, credit the
  basin's volume ledger. **Boundary sink** = particle crosses the active-region
  edge → despawn, credit the local graph segment. Both are integer ledger
  credits, which is what makes the scalar-authority split work.
- **Spillway faucets** = when a basin's volume exceeds capacity, emit particles
  at its baked `outlet_px` (first consumer of that wire field).

### §5 PBF particles — ADOPTED as the single flowing-water solver, with an authority/presentation split

Owner's decision, reaffirmed after review, and the evidence sides with it (see
§4: both existing solvers disqualified by their own numbers). The concerns that
drove the earlier rejection are real engineering constraints, and each has a
concrete answer that becomes part of the architecture:

- **Determinism / float ban.** The project's integer-determinism rules apply to
  *authoritative* state. The split: **authority = scalar hydrology** — per-basin
  volume ledgers, faucet rates, graph segment storage — all integers, all tiny,
  all persisted. **PBF = presentation of flow**, driven by those scalars. No
  particle position is ever authoritative; gameplay consequences (level rose,
  basin spilled, channel carries flow) read the ledger.
- **Multiplayer.** Nothing particle-shaped crosses the wire. Scalars replicate
  (a per-basin int64 and sparse graph diffs — the graph diff log is already
  replay-exact, `rivernet.h:127-149`). Each client runs its own local PBF from
  the same replicated sources; visual micro-divergence between clients is
  acceptable because volumes agree. A headless dedicated server runs
  scalar-only — rivers exist as levels and flows, not particles. A listen-server
  host with a GPU may additionally validate outcomes.
- **Zero particle infrastructure** (verified: no Niagara dependency, no
  emitters, docs price it "from scratch"). True, and it is the main cost of this
  plan: the solver is a new build — custom compute (structured buffers + spatial
  hash grid), NOT Niagara, so it stays inside the project's RDG/global-shader
  infrastructure (`VoxelGpuWorldGen.cpp` establishes the pattern) and its
  determinism/testing culture as far as a float sim can.
- **Terrain collision**: a GPU occupancy volume over the active region
  (512³ bits ≈ 16 MB for the 51.2 m cube), built incrementally from dirty
  bricks; particles project out along voxel face normals — the blueprint's
  collision model, and the voxel grid makes it cheap. New but tractable.
- **Budget reality to hold**: frame is render-thread (CPU) bound at p50 with
  GPU spikes at p95 (`frame-attribution-2026-07-28.txt`) — PBF spends GPU, so
  the p95 tail is the risk to measure, not the p50. Particle population math:
  sustained population ≈ residence time × spawn rate; a 1 m³/s river with ~100 s
  of visible residence ≈ 100 k particles at 10 cm spacing. 100-300 k particles
  is established territory for compute PBF on an RX 7800 XT; multiple large
  rivers need importance culling (despawn-to-scalar outside view).

### §6 Unified ray-marching renderer — SPLIT: fluid half adopted now, terrain half deferred (not rejected)

- **Terrain marching is deferred, not dead.** It replaces the entire draw path
  (pooled quads, vertex factory, GI integration, CDLOD clipmap — months), and
  nothing in the water system depends on how terrain is drawn. It stays open as
  a separable later project once water is proven; the water work neither blocks
  it nor is blocked by it.
- **The fluid half lands now** as a screen-space fluid pass: splat particle
  depths, bilateral smooth, derive normals, shade with Beer–Lambert (the exact
  law the water material already implements via scene depth,
  `create_voxel_material.py:184-244`), composite against the opaque depth buffer
  (written and consumed today). This has a decisive side benefit: **particles
  never touch the voxel mesher**, and the mesher is the measured bottleneck
  (~893 bricks/s spare vs ~6,100/s a churning river would demand). Two flags to
  respect from the material's documented bans: no scene-colour read (sort-key
  hazard) — refraction ships as a normal-perturbed *depth* trick or not at all
  at v0; reflections stay constant-sky Fresnel.
- **Fallback/debug view**: bin particles into voxel fill and draw through the
  existing water pool — native blocky aesthetic, zero new renderer, useful for
  A/B and for ultra-low settings, but pays the mesher cost, so it is the debug
  arm, not the product.

### §7 Roadmap — Phase 1 largely exists in the bake; Phase 2's scalar layer mostly exists dormant; Phase 3's solver+renderer are the genuinely new build

---

## The reconciled architecture (what we build)

**One sentence: static baked lakes stay as shipped and gain volume + spillway
overflow; ALL active flowing water near the player is one GPU PBF solver fed by
headwater faucets and sill faucets and drained by basin/boundary despawn sinks;
the authoritative state is scalar hydrology (integer ledgers + the routing
graph), so multiplayer replicates integers, never particles; rendering is a
screen-space fluid surface composited over the existing polygon scene; the baked
river plane is demoted from "the water you draw" to "data" (faucet rates, graph
geometry, far-field ribbons).**

What the player sees, by range:
- **Near field (the active simulation radius):** rivers, cascades and any
  disturbed water as PBF particles rendered as a smooth fluid surface. Dam it,
  dig a channel, breach a lake — it flows, pools and levels by actual dynamics.
  Staircase impossible: there is no baked shape to be wrong.
- **Mid/far:** river ribbons and lake sheets, unchanged (both shipped); the
  baked water plane keeps shipping as data for them and for faucet placement.
  Distant rivers are mostly sub-pixel anyway (mean width ~3 m; 35 of 157 reaches
  under one pixel in the probe), so the particle→ribbon handoff seam is gentle.
- **Lakes at all ranges:** flat planes from the basin table, level =
  `surfaceMm + h(volume delta)` — they rise, spill at the baked outlet, drain.
- **Existing CA:** frozen as-is (it ships and handles bucket pours/cave floods);
  not extended. Once PBF handles pours, the CA can be retired in a later pass —
  "one solver" is the end state, reached by migration, not by day-one deletion.

---

## Phased implementation

**Phase 0 — three feasibility spikes (isolated, no bake, ~days each).**
(a) Compute-PBF prototype in an isolated UE scene via the project's existing
RDG/global-shader pattern: 100-300 k particles, spatial hash grid, measure ms/
frame on the RX 7800 XT against the p95 GPU-spike headroom. (b) Screen-space
fluid pass: splat those particles' depths, bilateral smooth, normals,
Beer–Lambert shade, composite against the opaque depth buffer in-project.
(c) GPU occupancy volume built incrementally from dirty bricks + particle
collision against it (10 cm walls must not tunnel at expected velocities).
**These three numbers decide the whole plan; nothing else starts until they
pass.**

**Phase 1 — bake: basin table v2 + headwaters (one BAKE_VERSION roll, no terrain
change).**
Global basin ids with bake-side cross-tile merge at superblock level; stop
excluding spanning basins; ship per-basin capacity (∫A dh from the hypsometry
already computed then discarded) and floor_m; ship headwater points + Q
(`water_head_mask` output, currently `del`'d at `pipeline.py:5300`).
Water-only: `verify_water_only_change` must PASS; spanning-basin census → ~0.

**Phase 2 — scalar hydrology authority (visible win before any particle).**
`FBasinLedger` (per-basin int64 vs baked equilibrium; persisted in the water
save blob; replicated); lake level consulted by sheet actor + implicit fill (two
hooks); spillway logic (delta > capacity routes excess to the outlet's graph
segment — first consumer of `spillMm`/`outletX/Y`); routing graph rebuilt from
baked water+flow planes (`RiverNetwork::buildFromBakedWater`) and persisted.
Lakes visibly rise/spill/drain with zero particles — shippable on its own.

**Phase 3 — the PBF core (product of spike a+c).**
Solver as custom compute (structured buffers, hash grid, density constraint
projection); emitters: headwater faucets within the active radius (rate = baked
Q), sill faucets (from Phase 2 spill events); sinks: basin extent despawn →
ledger credit, boundary despawn → graph segment credit; conservation test:
emitted − despawned == in-flight, asserted continuously. Standalone
presentation first; scalars were already MP-safe in Phase 2.

**Phase 4 — fluid rendering (product of spike b).**
The screen-space surface pass as the shipped look; voxel-binned fallback as the
debug/ultra-low arm. Respect the two documented material bans (no scene-colour
read; constant-sky Fresnel).

**Phase 5 — retire the baked river plane from the near-field draw.**
Near-field river voxels stop meshing from the implicit term (same masking
pattern the mobilizer already uses); rivers near the player exist only as
particles + surface; ribbons/sheets continue outside. Bucket pours migrate from
CA to particle emission; CA demoted to legacy behind its flag.

**Phase 6 — later/optional:** dedicated-server scalar-only mode hardening;
terrain ray marching as its own project (kept open, independent); erosion/
sediment coupling; audio/foam driven by particle statistics.

## Verification

- Phase 0 spike numbers are the gates, measured on the pinned playtest scene.
- Conservation at every boundary: emitted − despawned == in-flight;
  ledger credits == despawn counts; graph storage+outlets == injected (existing
  rivernet ledger); basin volume monotone under pure inflow.
- Every stage writes a ran-flag distinguishable from "found nothing" (three
  absent-stat zeros produced false conclusions this session; standing rule).
- Captures at the pinned pose per change, conditions stated, no verdict — owner
  judges screenshots (standing rule).
- Scenario tests: dam-and-leave/return; breach a lake into a valley; dig a
  channel from a river; headwater-to-basin fill rate matches faucet Q.
- Streaming guard: 90 s same-pose comparison whenever anything touches the
  water sampler or amplifier column path (the 2.7× regression protocol).

## Key risks (ordered by kill-probability)

1. **GPU budget** — the frame's p95 tail is already GPU spikes; PBF + SSF spend
   GPU. Spike (a)+(b) measure it first. Mitigations: particle cap with
   importance despawn, half-rate sim tick, resolution-scaled SSF.
2. **Collision tunneling / leaking through 10 cm walls** — spike (c). PBF's
   projection model is the mitigation (the blueprint's own argument vs SPH).
3. **Emitter throughput vs look** — a real 1 m³/s river ≈ 100 k sustained
   particles; multiple rivers in radius need culling policy. Tunable, but the
   look under culling must be judged by the owner early.
4. **Cross-tile basin merge correctness** (Phase 1 bake code) — census + fixture
   tests; client extent fill must agree with the merged table.
5. **Presentation divergence in MP** (clients see slightly different splashes)
   — accepted by design; volumes agree. Flag to owner explicitly.
6. **Scalar↔particle seam artifacts** (river appears/disappears at the active
   radius) — mitigated by ribbon handoff at the same radius the disc already
   uses; judged visually.

## Critical files

- NEW `ue-project/Source/VoxelEarthShaders/` PBF compute + SSF passes (pattern:
  `VoxelGpuWorldGen.cpp` RDG global shaders)
- NEW `ue-project/Source/VoxelEarth/VoxelFluidSubsystem.*` — solver host,
  emitters/sinks, occupancy volume
- `ue-project/Source/VoxelEarth/VoxelWaterSubsystem.cpp` — basin ledger hooks
  (implicit fill :741, save :3080); CA freeze; pour migration later
- `voxel-core/include/voxelcore/rivernet.h` — `buildFromBakedWater`,
  persistence (scalar authority fabric)
- `terrain-service/terrain_service/bake/basins.py` + `pipeline.py` +
  `tile_codec.py` — basin table v2, capacity, floor, headwaters
- `voxel-core/include/voxelcore/tilestore.h` + `lakes.h` — BasinEntry v2 decode,
  ledger-adjusted lake datum

## What this means for the existing work

The bake's basin machinery, water plane, flow plane, discharge and heads all
remain load-bearing — as *data* (faucet placement and rates, graph geometry,
basin capacity, ribbon source), not as drawn shape. The bv18–bv23 settling
stages stop mattering visually once Phase 5 lands. The four bug fixes (render
overlap, see-through gate, ceiling, monotone rule) all stand. SWE and the
conveyor design are shelved with their analysis on record — if PBF's Phase 0
numbers fail, that analysis is the documented fallback, not a restart.
