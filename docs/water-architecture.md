# Water architecture — the current entry point

**What this is.** The single place to start for anything about water in this
game, the way `docs/world-generation-architecture.md` is for land. It
summarizes the approved direction, says what is shipped and stays, what is
being built and by which subsystem, the rules that survive the re-architecture
regardless of which solver is running, and indexes every other water document
in `docs/` by current status. It does not re-derive anything measured
elsewhere — it cites.

**Status: the direction is current; the roadmap in §3 is now history — phases
0-5 all shipped (reviewed 2026-08-19).** The authoritative design document
underneath this one is `docs/water-rearchitecture-plan-2026-08-09.md`, approved
by the owner. Read that for the full reasoning; this file is the map, not a
replacement.

---

## 1. The new architecture, in one paragraph

After weeks baking the *shape* of flowing water and drawing it as near-field
voxels (through `bake_ver` 23), the owner judged the result a regression and
supplied a blueprint, which was reconciled against the codebase and revised.
The owner then rejected the first draft's proposal to salvage the existing
SWE/CA solvers and reaffirmed **PBF (Position Based Fluids) as the single
flowing-water solver** — a decision the evidence backs independently: SWE is
structurally incapable of carrying a river below about 3% slope (an integer
dead zone where steady transport is exactly zero, against a world whose
median river gradient is 0.94%), and the CA's dam/drain failure modes are
documented and measured (`water-system-architecture.md` §6.6).

**The one-line architecture:** static baked basins (already largely shipped,
gaining volume/capacity/global identity) + **one GPU PBF solver** for all
active flowing water, fed by headwater and sill faucets and drained by
basin/boundary sinks — with the authoritative water state kept as **scalar
hydrology** (basin volumes, faucet rates, graph flows, all integers) so
multiplayer replicates integers, never particles. Rendering is a screen-space
surface pass over the existing polygon scene; unified terrain ray-marching is
deferred as a separable later project, not rejected.

**What the player sees, by range**, per the plan's own summary:

| range | what draws it |
|---|---|
| Near field (active simulation radius) | PBF particles, rendered as a screen-space fluid surface. Dam it, dig a channel, breach a lake — it flows, pools and levels by actual dynamics. No baked shape to be wrong. |
| Mid/far | River ribbons and lake sheets, **unchanged, both already shipped**. The baked water plane keeps shipping as data for them and for faucet placement. |
| Lakes, all ranges | Flat planes from the basin table; level = `surfaceMm + h(volume delta)`; rise, spill at the baked outlet, drain. |
| Existing CA | Frozen as-is (bucket pours, cave floods). Not extended. Retirable in a later pass once PBF handles pours — "one solver" is an end state reached by migration. |

Full section-by-section verdict on the owner's original blueprint (storage,
basin detection, macro-hydrology, PBF, the renderer) is in the re-architecture
plan §1–§7; not repeated here.

---

## 2. What ships today and stays

- **Static baked basins / lakes** — `voxel-core/include/voxelcore/lakes.h`,
  the basin registry from `terrain-service/terrain_service/bake/basins.py`.
  One scalar `surfaceMm` per basin, flat by construction, near-field implicit
  fill + far-field sheet rects. Gains a volume ledger and capacity in Phase 2;
  the shape does not change.
- **River ribbons** (mid/far) — `voxel-core/include/voxelcore/riverribbon.h`,
  `VoxelRiverRibbonActor`. Ordered centreline polylines traced from the water
  plane, shape decisions unit-tested in voxel-core without an engine. Unchanged.
- **Lake sheets** — `VoxelWaterSheetActor`, rectangle decomposition of a
  basin's wet mask. **As of 2026-08-11 these own lake basins at EVERY range,
  not just mid/far**, and the near-field voxel water is retired for lakes
  (owner's decision: "one type of rendered water, not two systems for
  basins"). Three things made that right: a lake surface is planar by
  construction, so the voxel volume added nothing visually; Single Layer Water
  is designed for a single surface and the near disc is a stacked volume; and
  the measured step across the boundary was +36.4/255 at matched depth and
  view angle. The near-field path stays for water that genuinely varies per
  cell — CA/mobilized water, cavern flood, ocean — and the implicit field
  keeps ANSWERING for swimming, collision and flood-on-dig; it merely stops
  being meshed.
- **The CA (`waterca.h`, `kWaterCAVersion` 5)** — frozen. Still handles bucket
  pours and cave floods. Its persistence design (ADR-0005, `WaterState`) stays
  load-bearing for that frozen scope.
- **The GPU water surface pool** (`voxel.Water.GPU`, `docs/gpu-water-pool-design.md`)
  — the translucent draw path, spatial sort buckets, chunk-table free list.
  Keeps drawing lake sheets, the voxel-binned PBF fallback/debug arm (§6 of
  the re-architecture plan), and whatever the frozen CA still meshes.
- **The water material's two documented bans** — but read the SCOPE note below
  before applying either, because both were narrower than they had been treated:
  1. **No scene-colour read.** Refraction ships as a normal-perturbed *depth*
     trick or not at all at v0 — a scene-colour read makes translucent
     compositing order-dependent outright (`gpu-water-pool-design.md`).
  2. **Reflections stay constant-sky Fresnel.** No dynamic reflection capture.

  **SCOPE, ESTABLISHED 2026-08-11.** Ban 1 exists because a *translucent
  material* grabbing scene colour breaks the pool's single sort key. It does
  NOT cover (a) a **post-process** material, where the colour buffer is the
  declared input — Epic's own underwater effect is one, which is what makes a
  proper submerged look available to us; nor (b) **Single Layer Water**, which
  is not sorted translucency at all: it renders in its own pass after deferred
  lighting, has no translucent sort key, and receives depth and velocity.
  Ban 2 likewise costs nothing under SLW — it never consumed planar
  reflections, and `r.Water.SingleLayer.Reflection 2` (captures + skylight
  only) is a first-class engine mode rather than a degraded fallback.
- **The bake's water-shape stages, demoted from "drawn shape" to "data".**
  Basin machinery, the water plane, the flow plane, discharge and heads all
  remain load-bearing — as faucet placement/rates, graph geometry, basin
  capacity, and the ribbon source — not as drawn near-field shape. See the
  deprecation audit (§5 below) for exactly which bake stages this covers and
  which ones lose their reason to exist.

---

## 3. The phased build — SHIPPED, and who owns each part

> **Reviewed 2026-08-19: phases 0 through 5 all landed, most of them on
> 2026-08-09.** Phase 0 spikes `bcabb76`; Phase 3 lifecycle `f0b4f31`; Phase 4
> screen-space fluid renderer `b795409`; Phase 5 (baked river plane retired from
> the near-field draw) `7925cb6`; the solver then took a soft cap at the measured
> knee — **102k particles at 2.4 ms** (`938da2a`). **What the table does not say:
> the far-field lake SHEET — a different draw path from the particle surface —
> was separately rebuilt onto Unreal's Single Layer Water shading model on
> 2026-08-11** (`MSM_SingleLayerWater`, `VoxelWaterSubsystem.cpp:147`), and wind
> waves and interactive ripples ride on it (`docs/water-wind-waves.md`,
> `docs/water-interactive-ripples.md`). Phase 6 is still open by design. The known
> live water defects are `docs/backlog.md` §9.

Kept below as written, because the phase names are what the code and the commit
messages refer to. From the re-architecture plan's phased roadmap
(§"Phased implementation"); consult it for full detail, acceptance gates and file
lists.

| phase | what | owning subsystem |
|---|---|---|
| **0 — feasibility spikes** | (a) compute-PBF prototype, ms/frame on the RX 7800 XT; (b) screen-space fluid pass (splat, bilateral smooth, Beer–Lambert); (c) GPU occupancy volume + particle collision, no tunneling at 10 cm walls. **Nothing else starts until these three numbers pass.** | new `ue-project/Source/VoxelEarthShaders/`, pattern from `VoxelGpuWorldGen.cpp` |
| **1 — bake: basin table v2 + headwaters** | Global basin ids, bake-side cross-tile merge; stop excluding spanning basins; ship per-basin capacity (∫A dh) and `floor_m`; ship headwater points + Q. One `BAKE_VERSION` roll, no terrain change. | `terrain-service/terrain_service/bake/basins.py`, `pipeline.py`, `tile_codec.py` |
| **2 — scalar hydrology authority** | `FBasinLedger` (persisted, replicated); lake level consulted by sheet actor + implicit fill; spillway logic (first consumer of `spillMm`/`outletX/Y`); routing graph rebuilt from baked water+flow planes and persisted. **Lakes visibly rise/spill/drain with zero particles — shippable alone.** | `VoxelWaterSubsystem.cpp` (ledger hooks), `voxel-core/include/voxelcore/rivernet.h` (`buildFromBakedWater`) |
| **3 — the PBF core** | Solver as custom compute (structured buffers, spatial hash grid, density-constraint projection); headwater + sill faucet emitters; basin/boundary despawn sinks; continuous conservation assertion. | new `ue-project/Source/VoxelEarth/VoxelFluidSubsystem.*` |
| **4 — fluid rendering** | Screen-space surface pass as the shipped look; voxel-binned fallback as the debug/ultra-low arm. Respects the two material bans (§2 above). | shaders + `VoxelWaterSubsystem.cpp` |
| **5 — retire the baked river plane from near-field draw** | Near-field river voxels stop meshing from the implicit term; rivers near the player are particles + surface only; ribbons/sheets continue outside. Bucket pours migrate CA → particle emission; CA demoted to legacy behind its flag. | `VoxelWaterSubsystem.cpp` |
| **6 — later/optional** | Dedicated-server scalar-only hardening; terrain ray-marching as its own project (kept open, independent); erosion/sediment coupling; audio/foam from particle statistics. | — |

**Key risks, ordered by kill-probability** (full detail in the plan): GPU
budget (p95 tail is already GPU-spike bound); collision tunneling through
10 cm walls; emitter throughput vs. look for multi-river scenes; cross-tile
basin merge correctness; presentation divergence in multiplayer (accepted by
design — volumes agree, splashes may not); scalar↔particle seam artifacts at
the active-radius handoff.

---

## 4. Standing rules that transcend the re-architecture

These hold regardless of which solver is running or which phase is live.
Sourced, not re-derived — read the cited section for the evidence.

- **The owner judges screenshots, not the implementer.** Deliver conditions
  and numbers, no verdict. Readings have been wrong in both directions.
  (`water-system-architecture.md` §14; re-architecture plan's own
  Verification section: "no verdict — owner judges screenshots.")
- **Every stage must write a ran-flag distinguishable from "found nothing."**
  Three absent-stat zeros produced false conclusions in one session (a
  counter silently reading a wrong key looked identical to "this stage does
  nothing" — `water-session-progress-2026-08-07.md`). Applies equally to PBF
  conservation counters (emitted/despawned/in-flight) going forward.
- **Never rebuild ground in Python.** Reconstructing ground as
  `base_offset_mm + elevation_cp * quant` by hand has produced a ~480 m error
  and a retracted headline finding, twice — and a third time on 2026-08-10,
  where the landmine was `quant` itself: **it is a CODE, not a multiplier**
  (`tile_codec.QUANT_MM = {1: 100, 2: 250}`; C++ `tilestore.h fineQuantMm`).
  Using it as a multiplier compressed relief 100×, floated every basin's
  surface hundreds of metres above its own bbox, saturated the extent fill,
  and drew a **solid rectangle for every lake** on the overlay the owner was
  asked to judge. Two findings were published from it and retracted
  ("1,429 of 2,052 basins produce no footprint"); the C++ census
  (`vxc_lakeextentprobe`) says **2 of 2,049**. Cheap tell: a 500 m basin whose
  entire bbox spans 0.7 m of relief is not terrain. Use the bake's own instrument
  (`vxc_riverribbonprobe`, which already reconstructs ground the client's way
  and carries a long-profile section) instead of re-deriving flow direction,
  perpendicular transects, or which surface discharge was accumulated on.
  (`water-session-progress-2026-08-07.md`; `water-system-architecture.md` §14.)
- **The three-grounds trap.** Name which ground you mean, every time: (1) the
  raw sample field, (2) the spline reconstruction (`reconstructedGroundMm`,
  what the water datum is measured from), (3) the amplified surface
  (`GroundMmAt`/`GetSurfaceHeightUU`, what is drawn). The water datum is
  reconstructed ground plus baked depth and **never reads the amplifier** —
  both `tile_codec.py` and `tilestore.h` forbid it explicitly.
  (`water-system-architecture.md` §9, §12a.)
- **The 2.7× streaming-regression guard protocol.** A 90-second same-pose
  comparison (settled chunk count vs. still-churning count) whenever anything
  touches the water sampler or the amplifier column path. Extending a
  sampling domain by searching neighbours at runtime once cost 8n locked
  queries per column and measured a 2.7× regression (51,063 settled vs.
  19,162 still churning, same pose, 90 s) — the fix was to move the work into
  the bake instead. (`water-waterline-at-voxel-scale-2026-08-06.md` §3;
  re-architecture plan's Verification section restates the guard for the PBF
  work.)
- **One UE editor per box.** Two capture agents once destroyed each other's
  frames for hours. (`water-system-architecture.md` §14.)
- **Water is rendered by two different renderers, and their physical constants
  live in exactly one file.** Above the surface it is a Single Layer Water
  material (`M_WaterVoxel`); from inside it is a post-process doing
  Beer–Lambert over the depth buffer (`M_Underwater`). Neither can reach the
  other's shading, so the only thing keeping them the same liquid is that both
  import `ue-project/Tools/water_optics.py` — absorption distance, absorption
  colour, scattering, phase g — and neither is allowed to type a coefficient as
  a literal. **This is not hypothetical: it shipped broken for one commit.**
  The surface was taken to a 3.5 m absorption distance with 5× the scattering
  (9d3bd0c) while submerging still applied a hard-coded
  `SceneColorTint(0.05, 0.30, 0.35)` and a fixed-density height fog, both tuned
  for the *ocean*, both applied to lakes at ~1650 m because the post-process
  component is `bUnbound`. Looking into a pond and swimming in it gave two
  different liquids. Same class of split as the near/far water seam the owner
  made us delete; the difference is that there is no path to retire here, so
  the fix is shared constants. Both generators print
  `water_optics.summary_lines()` into their log so "were these built from the
  same numbers?" is answerable without opening either asset, and
  `tools/voxel-underwater-regen.ps1` diffs the two blocks.
  - Deliberately **not** shared: how each renderer presents that water (foam,
    glints and ripple normals above; ambient level and the submerged
    readability scale below), and the underwater view's
    `UnderwaterExtinctionScale` (0.5) — a **deliberate readability cheat, not
    physics**. At the true 1.14/m red coefficient a swimmer could not see one
    metre. Photon and other shipped shaders do the same thing.
  - Deliberately **not** a Material Parameter Collection, which is the engine's
    own answer to "two materials, one set of numbers": `create_sky_material.py`
    deletes and recreates `MPC_VoxelSky` every run, and any material holding a
    binding to the old one compiles to UE's **default material** while the log
    reports success. A second collection would be a second instance of that
    hazard, for constants that never change at runtime. The cost of the Python
    import instead: the two materials expose these as separate parameters, so a
    material-instance override on one does not move the other. Instance
    overrides are for experiments; what ships comes from the file.
- **Decimating the extent rounds in OPPOSITE directions for drawing and for
  despawning, on purpose.** Drawing (`lakeSheetRects`) takes a block if ANY
  pixel in it is wet; the basin sink's grid (`basinExtentBits`) takes a cell
  only if its CENTRE is wet. The failure modes are not symmetric: an
  over-covered sheet runs into the bank, which is opaque ground above lake
  level and hides it, so the visible waterline becomes the terrain's own
  intersection with the lake plane — while an under-covered sheet leaves
  visible air between a lake and its shore (the owner's 2026-08-10
  screenshot). For despawning it inverts: reaching too far **deletes the
  river**, falling short leaks a few particles into sinks that already
  conserve to the ledger. `lakes.h` says this out loud so nobody "makes them
  consistent."
- **Grep every playtest log for `Failed to compile Material` before
  diagnosing anything visual.** `create_sky_material.py` deletes and recreates
  `MPC_VoxelSky` every run, which unbinds every dependent's
  `CollectionParameter`; on 2026-08-10 that left `M_WaterVoxel` failing to
  compile, so **UE drew all water with the DEFAULT MATERIAL** while the log
  reported `232 basin(s), 16678 rectangle(s), 0 unresolved`. "I can't see it"
  plus "the log says it was built" can mean the fallback material is drawing
  it. Regeneration order is mandatory: sky → dome → water.
- **Residence time is the first thing to compute when water misbehaves.**
  `alive ÷ emission rate`. 226 alive at 3,682/s is 61 ms — against a 150 UU/s
  emit speed that is 9 cm of travel, which says "dying at the mouth", not
  "flowing away", before any shader is opened.
- **The bake is the authority on where water is; the client only draws it.**
  Still true under the re-architecture — the bake now additionally decides
  faucet placement/rate and basin capacity, not just the drawn shape.
  (`water-system-architecture.md` §0, §12a.)
- **A blank capture is usually unloaded terrain, not a rendering bug**, and
  water specifically is invisible past whatever the active simulation radius
  ends up being — check whether water could have been in frame before
  diagnosing anything. (`water-system-architecture.md` §14.)
- **Measurement records in `docs/measurements/*.txt` are never deleted.** A
  negative result is the most expensive thing here to reproduce.
  (`water-system-architecture.md`, opening rules.)

---

## 5. Deprecation

See `docs/water-deprecation-audit-2026-08-09.md` for the itemized,
evidence-backed list of code the re-architecture makes obsolete (bake
water-shape stages, SWE, `rivercouple.h`, `widen_to_channel_width`, the
magenta water marker, `farwater.h`, `reservoir_v0`, density3 remnants) with
per-item recommended action. That document is a report; nothing in it has
been deleted or changed.

---

## 6. Every water document in `docs/`, by status

Every doc below carries its own `STATUS 2026-08-09` banner at the top with a
one-sentence reason; this table is the index, not a duplicate of the reasons.

| doc | status |
|---|---|
| `water-rearchitecture-plan-2026-08-09.md` | **CURRENT — authoritative.** The design doc this file summarizes. |
| `water-architecture.md` (this file) | **CURRENT.** New consolidated entry point. |
| `water-system-architecture.md` | **PARTIALLY CURRENT.** §6 draw-path facts, §12a settled claims, §14 rules-that-cost-hours load-bearing; river-shape bake/draw sections (§5, §6.1–6.4, §11, §11a, §11b) superseded as *drawn shape*, kept as *data* provenance. |
| `watershed-system-plan.md` | **CURRENT** (redirect stub to `water-system-architecture.md`, still accurate). |
| `water-deep-dive-brief-2026-08-05.md` | **CURRENT** (redirect stub, still accurate). |
| `water-production-plan.md` | **SUPERSEDED** — kept for history. Deterministic-content/mutable-state reasoning still sound in spirit; concrete work list superseded. |
| `water-waves-plan-2026-08-04.md` | **SUPERSEDED** — kept for history. Landed items (ribbon actor, hydrostatic cap) are shipped/current; open item (two-renderer tone seam on the near-field river) is moot post-Phase-5. |
| `water-flow-effects-plan-2026-08-06.md` | **SUPERSEDED** — kept for measurements/history. Fakes flow on the baked near-field river; PBF makes the fake unnecessary. |
| `water-waterline-at-voxel-scale-2026-08-06.md` | **SUPERSEDED** — kept for history. Fixes the near-field voxel waterline raster edge; retired with that draw path. |
| `far-voxel-water-plan.md` | **SUPERSEDED** — kept for measurements/history. Ring-cascade extension of *voxel* water to 1 km; the new plan doesn't extend voxel water outward at all. |
| `far-water-schemes-decision-2026-08-05.md` | **SUPERSEDED** — kept for measurements/history. Scheme comparison for the now-superseded ring cascade. |
| `near-water-refresh-findings.md` | **CURRENT.** Shipped near-field-refresh performance infra; stays load-bearing for the lake implicit-fill path even after rivers move to PBF. |
| `water-handover-2026-08-04.md` | **CURRENT** (session-history record, corrected in place; own doc classifies it this way). |
| `water-session-handoff-2026-08-06.md` | **PARTIALLY CURRENT.** F6 discharge-injection fix load-bearing for faucet Q; magenta-marker task and F2/F3 near-field shape tuning superseded. |
| `water-session-progress-2026-08-07.md` | **PARTIALLY CURRENT.** Bake data-quality measurements load-bearing; near-field-draw open items (banded level field, ponding-before-spilling) superseded. |
| `water-wet-country-2026-08-05.md` | **CURRENT.** Region-selection rationale, solver-independent. |
| `water-wet-country-bv14-2026-08-05.md` | **CURRENT.** Bake data-quality measurement, solver-independent. |
| `gpu-water-pool-design.md` | **CURRENT.** Shipped rendering infra; keeps serving lake sheets and the PBF debug/fallback arm. |
| `docs/adr/0003-hydrostatic-persistent-body.md` | **CURRENT.** Shipped CA perf/correctness; matches the "frozen CA" maintenance state. |
| `docs/adr/0004-swe-fixed-point-coupling.md` | **SUPERSEDED** — kept as the documented fallback if PBF's Phase 0 spikes fail. Not the active direction; do not enable the coupler without re-opening the decision. |
| `docs/adr/0005-water-persistence.md` | **CURRENT.** Shipped CA persistence; its "irreducible state" argument is the template for persisting the new scalar hydrology ledger. |
| `docs/adr/0007-swe-eligibility-depth-term.md` | **SUPERSEDED** — kept for history. Unimplemented proposal amending the now-shelved SWE track. |
| `docs/measurements/*.txt` (water-relevant subset) | **CURRENT as evidence**, per `water-system-architecture.md`'s own still-stands table. Never delete. |
| `docs/lake-survey/lake-survey.md`, `docs/water-map/ocean-captures.md` | **CURRENT.** Survey/capture records, solver-independent. |
