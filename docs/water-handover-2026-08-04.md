# Water system — handover

> **STATUS 2026-08-09: [CURRENT].** Kept as the session record of how the
> baked-river-shape work got here, corrected in place where numbers moved
> (see its own 2026-08-05 banner below). `water-system-architecture.md`
> classifies this file as current history, not superseded, and that still
> holds. Read `docs/water-architecture.md` for the current direction.

> ## STILL CURRENT AS HISTORY — read the two live documents first (2026-08-05)
>
> This is **how the system got here**, and it is the best record of that. It is
> not the current state. Before acting on anything below, read:
>
> * **`docs/watershed-system-plan.md`** — how the water system works today: the
>   five stages, the constants and who owns them, the file map, the version
>   rules, and the exact probe commands.
> * **`docs/water-deep-dive-brief-2026-08-05.md`** — what is still wrong,
>   ranked, and the seventeen explanations already falsified.
>
> **What has moved since this was written.** `BAKE_VERSION` was 11 here; it is
> now **14** — `bake_ver` 12 added seam concentration and law-driven width,
> 13 replaced width with a **lateral fill** off the ground, and 14 makes the
> water **touch itself down a slope**. `TERRAIN_VERSION` is still 8 and no
> elevation byte has moved through any of it.
>
> **Phase 4 is done** (`docs/river-farfield-actor-2026-08-04.md`), and so is the
> hydrostatic cap. **Phase 5, the two-renderer tone seam, is still open.**
>
> **Every number below is from the four-tile arid corridor**, which sits at
> about the world's 80th percentile for runoff. The default test region is now
> the **wet alpine block**; see `docs/water-wet-country-2026-08-05.md`.
>
> Two numbers below have been corrected in place: the ELEV_DATA control-point
> difference (§1) and the 20,269 m river (§5, already caveated).

Written 2026-08-04, at the end of the session that built most of what is
described here. Read the first two sections before touching anything; they are
what stops you repeating a day's work.

The owner's standing requirement, in their words: **realistic flowing rivers
that start in high mountain valleys and reach all the way out to discharge into
the sea.**

**Not yet delivered, but much closer than it was.** The corridor's longest
continuous reach went from 1,113 m to **14,827 m** on the last day of this
session, and a basin-composed 15,332 m reach does run to the shoreline and out
onto the seafloor. What is still missing: the mountain heads (978 m, 1,326 m,
1,540 m) all stop inland, and rivers remain invisible beyond about 26 m because
nothing consumes the far-field producer. Phase 3 and Phase 4 in §5 are those
two gaps.

> **Phase 4 has since closed** — the ribbon actor ships. Beyond 25.6 m a river
> is now drawn, but as flat quads, which the owner has rejected on sight; the
> ring cascade is the agreed replacement and is half-built. Brief §4.3.

Everything else below is either progress toward that or something that had to
be fixed on the way.

---

## 1. Read this first: what is already settled

These are measured. Do not re-derive them, and be suspicious of any plan that
implicitly contradicts one.

**The bake writes real water.** Four corridor tiles carry 5.5k–9.3k wet pixels
each, depth **min 500 mm, p50 600–1050 mm, max 1080 mm**. There is a hard depth
floor at half a metre — wet-to-dry is a step, not a ramp, which rules out any
design that wants a gradient at the bank.

**The renderer draws it.** 92% of wet pixels emit a water voxel; the water sits
**p50 +723 mm** — about 7 voxels — above the drawn ground. Only 7.81% are
drawn dry.

**Near-field water only exists in a box around the camera.**
`kImplicitRadiusBricks = 32`, `kImplicitRadiusBricksZ = 16`, 8-voxel bricks →
**±25.6 m horizontally, ±12.8 m vertically**. At 10 m altitude and −20° pitch
with 90° FOV, the frame's bottom edge first touches ground **8.6–9.3 m ahead**,
so the usable window is roughly **9–25 m**. A capture at 90 m logs
`0 candidate brick(s)`. **"No water in the screenshot" is usually reach.**
Check whether water could have been in frame at all before diagnosing anything.

**Sheets are lakes-only.** The basin registry, `holdsWater()` and
`extentMaskFor` all assume a basin, and `CompositeWaterSampler` forwards the
sheet half to lakes deliberately. There is no far-field path for flowing water
in the shipping build.

**There are three grounds and they have been conflated three times, across two
languages.** Name which one you mean, every time:
1. the raw **sample field** — what the bake subtracts to get depth;
2. the **spline reconstruction** — `reconstructedGroundMm` in `tilestore.h`,
   what the water datum is measured from;
3. the **amplified surface** — `GroundMmAt` / `GetSurfaceHeightUU`, what is
   drawn. Explicitly forbidden as a water datum by both `tile_codec.py` and
   `tilestore.h`.

**The version split works and is proven.** `TERRAIN_VERSION = 8` decides the
ground; `BAKE_VERSION` (11 when this was written; **14** today) decides
everything else. A water-only change
re-bakes with **elevation and flow planes bit-identical**, verified by
rebuilding superblocks and comparing `filled` and `acc` byte for byte. Use it:
hydrology changes should cost no terrain re-key.

**Caveat on that:** bit-identical *planes* still produce a different
`ELEV_DATA` digest when the flow superblock is not retained across the re-bake
(no superblock is retained under a fine namespace, so the pyramid is rebuilt).
Physically nothing moves; on the wire the tile is a new identity and forces a
client re-download. Unresolved.

> **CORRECTED 2026-08-05.** This paragraph used to claim the re-bake left
> **696 of 603,979,776 control points different by 1 mm each**, and that the
> difference "rides through incision". **Both halves are wrong. The real
> difference is zero.** The comparison put a ZSTD-coded tile against a RAW one,
> which invents differences, and `quant == 1` was read as "1 mm" when it is the
> **code for 100 mm** — a 100× unit error. It reached a PR before it was caught.
> Re-verified through the codec's own `elevation_control_points` operator:
> elevation IDENTICAL and flow plane IDENTICAL on all four corridor tiles and on
> all six wet-block tiles. See `docs/measurements/river-lateral-fill-2026-08-04.txt`
> §6. **Always compare through the codec's own operator, never a hand-rolled
> quantiser** — that has produced a false "terrain moved" alarm here before.

**The `.vxtl` format is already sliceable.** Every block is compressed on its
own with no state across boundaries, and the index carries
`(offset u64, comp_len u32, mode, const_cp, resid_bits)` — 20 B × 1024 blocks =
20.5 KB per plane. Verified across 76 tiles: blocks contiguous, in index order,
exactly covering the data section. ZSTD median block **34,008 B** against a
32–56 MB tile. Slicing is a fetch problem, not a format change (task #52).

**`FLOW_BIT_CHANNEL` is useless as a river signal.** It is set on 14–66% of a
tile, so its "network" is a blob whose span is just the tile diagonal. Every
wet cell is on a channel; almost no channel cell is wet.

---

## 2. Read this second: wrong turns, already taken

Five confident explanations died this session. Two nearly became code.

| claim | verdict | the number that killed it |
|---|---|---|
| The amplifier buries the river | **false** | AMPLIFIED − SPLINE inside the channel is p50 **+3 mm**, tails *smaller* than on dry banks 10–30 m away |
| Rivers die at tile seams | **false at the time, now superseded** | 0.5–2.3% of components touched a tile edge vs a 0.39% chance baseline. That was true while threshold height and MFD dispersion dominated. With both fixed, **35 of 36 raw components now end within one pixel of a tile edge** — see Phase 3 |
| Tile (-14,-7) is the river mouth | **false** | 96.2% ocean, median −115 m, all 395 drawable cells below sea level. The coastline is (-14,-6) |
| Carrying Q up the pyramid will coalesce the wet mask | **false** | 1,954 → **2,014** components, longest span identical to the pixel |
| The area/wet gap proves a defect | **half false** | I cut the two networks an octave apart. Level-matched, wet share goes 35.1% → **66.4%** |

The amplifier one nearly rolled `kWorldGenVersion` and re-pinned the GPU digest
for a problem that did not exist. **Measure before building.**

Two controls were also worthless, from different causes, on the same day:
- a capture control that differed from its pair by **85% of pixels** because
  exposure moved — it proved nothing and a wrong conclusion was drawn from it;
- `voxel-capture.ps1` cleared the edit log but **not** the `.vxwater` blob, so a
  run restored **886,179,570 fill units / 17,235 mobilized bricks** before its
  own dig. Fixed on `claude/ocean-captures`.

**Pixel-diff every control against its pair and report the number.** If the
difference is large and diffuse rather than localised to the one variable under
test, the control is broken.

---

## 3. What was built

**All eleven branches below are merged into `claude/water-integration` and
verified together there** — build clean, `vxc_tests` 422 with 0 failures,
pytest 539 passed / 2 skipped. That branch is what lands on `main`, as one PR.
Once it is merged, start from `main`; the branches are kept only for
archaeology and you should not need to check any of them out.

| branch | what it holds |
|---|---|
| `claude/watershed-build` `c09ef43` | P2 water in the bake; `reconstructedGroundMm`; `RiverSampler` wired through `CompositeWaterSampler` |
| `claude/water-edit-response` `6779857` | §6.3 the edit-response layer, §6.5 the return path |
| `claude/water-return` `d88ac8a` | 9a front gate, 9b mobilized ceiling, 9c exact demotion |
| `claude/water-ocean` `676bf99` | the ocean as the third datum term; Reservoir v0 deleted |
| `claude/carry-q-up-pyramid` `082615e` | discharge carried up the pyramid |
| `claude/river-farfield` `2ad2961` | `riverribbon.h`, the far-field centreline producer |
| `claude/watershed-burial-probe` `83a375b` | `vxc_burialprobe`, with a `--cam` mode that replays the candidate sweep from a capture log |
| `claude/carry-q-corridor-test` `d1603d3` | the bv10 corridor re-bake and its negative result |
| `claude/river-frag-diagnose` `0e0e95b` | the fragmentation diagnosis; `river_break_probe.py`; `--npz-dir` / `--diagnostic` |
| `claude/ocean-captures` `d69db03` | three ocean captures, `VoxelOceanCaptureFixture`, the `.vxwater` fix |
| `claude/river-drawable-flow` `67af930` | lower the cut and concentrate flow — 1,113 m → 14,827 m |

### Highlights worth knowing about

**Damming a baked river does nothing.** A wall across an undisturbed reach
settles in **1 tick** and the upstream stage does not rise by a voxel: a baked
river is a *datum*, not a flow, and every untouched part of it is a wall to the
CA. Every shipped test that seemed to prove otherwise pours or breaches; none
dams a datum.

**Draining one runs away.** A single 4-voxel shaft near the downstream end
converts **100% of the reach** — `advanceFront` follows moving water and has no
length bound. It is drainage, not disturbance, that is unbounded.

**`mobilized_` was insert-only** — two insert sites, zero erase sites — so
settled water stopped costing ticks but never stopped costing memory, savegame
and snapshot. 9a/9b/9c add the return path, with an **exact, zero-tolerance**
demotion predicate; a tolerant one recreates the double-occupancy bug that
one-way mobilization existed to prevent.

**The sea was never water to the simulation.** Only pinned breach cells were.
A cove cut into the coast **drained** instead of filling and never settled —
volume 358k → 610k, active bricks 536 → 758 between ticks 100 and 1500, still
climbing. A live CPU leak behind every below-sea-level dig, hidden by the 40 km
plane. The datum path settles the same cove at tick 54.

**Discharge was faked from local runoff.** Any river leaving its own climate
zone vanished: down one corridor runoff falls 217.5 → 12.8 → 3.6 → 0.5 mm/yr,
so the coast saw 1.27e6 m³/yr against a 3.15e6 threshold while the coarse world
said 5.87e7. Carried Q gives **2.50e7 — 40% of the coarse world, up from 2.2%**.

---

## 4. Why there is still no river, and what to do about it

The corridor's longest continuous reach is **1,113 m**. Water meets the coast
as **235 separate pieces**, the longest of which reaches it for **475 m**.

Diagnosed and apportioned:

- **Threshold height — 49%.** `q_drawable` = 3.1467e6 m³/yr derives from a
  **2-pixel minimum channel width**. Against the corridor's median implied
  runoff of 0.1883 m/yr that is an area cut at **2^23.99** — an octave above
  where the design record assumed rivers start. **90.29% of dry cells on the
  area network sit within a factor of two of the threshold**, which is why the
  height dominates.
- **Runoff weighting / MFD dispersion — 51%.** 25–33% of network cells have no
  strictly-lower neighbour holding as much as they do: their whole accumulation
  was split. Under any single-receiver rule that count is 0. Controlled A/B on
  one refilled surface: **D8 gives 1–3 components spanning 5.9–10.5 km where
  MFD p=1.1 gives 111–619 spanning 0.5–3.7 km** (internal to a reconstruction,
  so direction not magnitude).
- **The log2 bin — 0%, ruled out provably.** `floor(log2 x) ≥ 23` *is*
  `x ≥ 2^23`; networks identical to the cell on three tiles.
- **B5 basins — 0%.** 6.2% of cells, **0 m** of span; the 17.4%-basin tile
  loses the second least.

One reach walked, 2,001 steps: `105 wet, 14 dry, 129 wet, 13 dry, 57 wet,
2 dry, 166 wet…` — fifty wet-dry-wet excursions on a single channel.

**The owner's decision, taken 2026-08-04: do both — lower the cut by about an
octave AND concentrate flow for the water-drawing pass.** That work is in
flight on `claude/river-drawable-flow`; check its result before planning past
Phase 2.

---

## 5. Phases of work, in order

### Phase 1 — land the integration
One PR from `claude/water-integration` into `main`. It is verified as a whole,
not just per branch: build clean, `vxc_tests` 422 with 0 failures, pytest 539
passed / 2 skipped. After it merges, start from `main`.

One lesson worth keeping. Every conflict across eleven branches was two agents
*adding beside each other* — both `CMakeLists.txt` files and `lakes.h`. Keep
both sides. **Taking one side of a `CMakeLists.txt` conflict silently
unregisters a whole test file and still passes green**, so check the test
count, not just the exit code. That is why the counts are recorded in §7.

### Phase 2 — wire the return path
**9a and 9b exist and are never called.** `setFrontGate` and
`setMobilizedCeiling` have no caller in `UVoxelWaterSubsystem`, and the cost is
already visible: a breach into the open sea spreads without bound —
activeBricks 19,636 → 41,613, volume 501M → 884M, water tick 2.0 → 2.5 s, the
engine itself logging `runaway spread`. Also unwired: nothing calls
`demoteBudgeted` from an authority tick, and `takeRecentlyDemoted()` has no
sender on the water-diff channel.

This is the highest value-per-hour item on the list. The mechanism is built and
tested; only the engine-side call sites are missing.

### Phase 3 — finish the river

**Done, and it worked.** `claude/river-drawable-flow`, merged. `BAKE_VERSION`
10 → 11, terrain bit-identical, fingerprint unchanged at `fe0275e105cbf77c`.

| | bv10 | bv11 |
|---|---|---|
| components | 2,014 | **173** |
| longest span | 1,113 m | **14,827 m** |
| components ≥ 2 km | 0 | **36** |
| longest piece at the coast | 475 m | **9,881 m** |
| wet pixels | 27,347 | 158,458 |

Apportioned by re-thresholding the Q already on disk, all arms like for like:

| arm | comps | longest | ≥2 km |
|---|---|---|---|
| MFD @ 2.0 px (bv10) | 2,019 | 1,113 m | 0 |
| MFD @ 1.5 px — threshold only | 4,930 | 4,216 m | 1 |
| D8 @ 2.0 px — concentration only | 22 | 11,792 m | 12 |
| D8 @ 1.5 px — both | 36 | 16,341 m | 25 |

**The threshold half alone makes fragmentation worse** — the cells it admits
are exactly the ones MFD then splits. Concentration is the larger effect on
both axes; together they buy +4.5 km of longest reach over concentration alone.

**Why D8 and not a higher `mfd_p`**, and this is a latent bug worth knowing:
`_accumulate_mfd` evaluates weights in the surface's own dtype, and on float32
an epsilon-filled flat's slope (~2.6e-4) **underflows at p ≈ 11** — every
weight zero, `tot` zero, the cell reads as a pit and its entire accumulation is
dropped. On exactly the flat near-coast ground rivers must cross. p=32 loses
>50% of the budget; D8 loses <1e-9. Pinned by
`test_accumulate_d8_beats_high_p_underflow`.

**Three things still qualify the win.**

1. **It is not yet a mountain river.** The 14,827 m reach heads at 978.1 m and
   stops 6.66 km short of the sea, on the (-11,-5)/(-11,-6) seam. The 978 m,
   1,326 m and 1,540 m heads all stop inland. With registered basins composed
   back in — what the client actually draws, since the basin table supplies them
   and `CompositeWaterSampler` unions the two — a **15,332 m** reach does run
   from 240.4 m to the shoreline and out onto the seafloor.
2. **The raster now draws a centreline.** Width p90 5.62 → **1.88 m**, widest
   10.61 → 3.75 m, and **99.21% of wet pixels are a single 1.875 m pixel**
   against a law width of 3.53 m at p50. The plane is wet where a cell's *own*
   Q clears the cut, so MFD's fan was what made the ribbon wide — bv10's
   agreement with the width law was an accident. **You cannot get both from the
   mask alone.** The fix is to let `channel_width_m(Q)` decide extent the way
   `water_depth_m(Q)` already decides depth. That is a third change and a
   decision about what the world looks like — **it is the owner's call and it is
   open.**
3. **What fragments it now is neither fixed cause.** 137 of the 173 raster
   components are the B5 basin exclusion, by design, composed client-side. Of
   the 36 raw components, **35 end within one pixel of a tile edge** — 24 at the
   measurement window, **11 at an interior seam**. ~~The fine Q does not cross a
   fine tile boundary: each tile restarts from the 30 m superblock injection.~~
   **That explanation is WRONG — see bake_ver 12 below. The water crosses; it
   arrives divided.** Read the correction before acting on this paragraph.

#### bake_ver 12 — items 2 and 3 addressed, one of them only half

Added 2026-08-04 on `claude/river-seam-width`. Full numbers and method in
`docs/measurements/river-seam-and-width-2026-08-04.txt`; terrain verified
bit-identical on all four corridor tiles, including through the codec's own
`elevation_control_points` against the shipped tiles.

**Item 2 is done.** `water_width_from_law` lets `channel_width_m(Q)` decide the
drawn extent, clamped to ground standing at least 100 mm below that reach's own
water surface. Width p90 **1.88 → 7.50 m**, max 3.75 → 15.91 m. On the
centreline pixels — the only population the law is a function of — the drawn
ribbon now agrees with the law to within one raster pixel on **93.1%** of the
network against 59.8% before, and the share drawn at under half its own law
width falls **40.1% → 1.7%**. It does not over-draw (0.1% exceeds twice the
law) and **0 wet pixels stand above the drawn ground**. Which bound shapes it
was measured first: the terrain allows p50 11–28 m of lateral extent and 90–93%
of wet cells have room for the whole law width, so the *law* does the shaping
and the clamp bites on the 7–10% that would otherwise be drawn uphill.

**Item 3 is half done, and the handover's mechanism for it was wrong.** The
downstream tile was never *starved*: the same 8.6e7 m³/yr crossed the seam
either way (D8/MFD = **0.99** on the total). It was **divided** — MFD's largest
single crossing held 8.66e6 against D8's 2.02e7, and a hard threshold cannot use
a fan. `water_pyramid_single_receiver` routes the pyramid's discharge sweep D8
to match the fine tier. A/B'd as its own arm: it buys **9,881 → 15,028 m** on
the piece that reaches the coast and **1 → 3** reaches over 10 km, while merging
almost nothing (173 → 172 components — the merging in the combined result is the
*widening's* doing).

**What is left is a POSITION error, not a magnitude one, and it is the new
binding constraint.** At the seam the coarse D8 trunk sits 165 m from the fine
trunk; the injection lands at the lowest fine cell in a 30 m footprint on flat
near-coast plain (194.5–199.8 m over 375 m, no incised channel, local Q below
the cut), i.e. on the wrong side of a divide a few metres high, and the water
ends up 1.3 km east. A 960 m apron cannot heal what has no valley to fall into.
**10 of 35 multi-km reaches still end at an interior seam**, including the
978.1 m one. The fix that follows is to place the entry cell by the *fine*
surface's own thalweg rather than by its lowest elevation — but `_edge_entries`
deliberately scatters BOTH currencies through one set of crossings, so moving it
moves the area injection, hence incision, hence the ground. That is a design
decision, not a patch.

**Item 1, composed the way the client draws it: yes, from a foothill.** River
plane ∪ the 255 lake sheets, labelled once: the longest composed reach runs
**20,269 m unbroken from 389.0 m down to −476.0 m**, out onto the seafloor, and

> **Read that 20,269 m correctly: it is a EUCLIDEAN SPAN, hull to hull, not a
> length.** `max_pairwise_m` measures the widest separation of the component,
> not the distance water travels. **Along its own channel the river is
> 30,577 m, sinuosity 1.51**, of which 24,046 m is above sea level. Quoting the
> span as river length was an error repeated several times before the
> longitudinal profile caught it — see
> `docs/measurements/river-long-profile-2026-08-04.txt`.

only **5.0%** of it is lake sheet — a river, not a chain of lakes. (bv11's was
16,341 m and 40.2% lake.) A second runs 17,909 m from 269.1 m. The 978 m,
1,326 m and 1,540 m heads still stop inland; the 1,540 m one *does* appear in a
13,030 m composed component but it is **94.4% lake sheet** and must not be
quoted as a river.

**Lakes and basins fill — verified, not rebuilt.** 255 registered basins over
the corridor, **246 overflowing / 9 terminal, zero dry playas, salt flats or
seasonal**, so `holdsWater()` removes nothing here. Total **8.305 km²**, area
p50 6,676 m² and max 2.82 km², depth **p50 3.03 m, max 80.80 m**, 0 empty
extents, and `floor < surface ≤ spill` holds for all 255. All 2,362,237 cells
inside a lake extent are dry in the river plane — the exclusion is exactly one
copy of the fact, unioned back by `CompositeWaterSampler`.

### Phase 4 — the far-field river actor

> **DONE, 2026-08-04.** `VoxelRiverRibbonActor` consumes the producer and the
> widening question below was settled by measurement — the policy is to draw at
> the width the bake drew, with no screen-width floor. Full record:
> `docs/river-farfield-actor-2026-08-04.md`. The paragraph below describes the
> gap as it stood before that landed.

`riverribbon.h` produces ordered centreline polylines (not rectangles —
deliberately, because `lakeSheetRects` point-samples at block centres with
`TargetCellsPerSide = 128` and gives a ~15 m axis-aligned staircase the owner
has already rejected). **There is no UE actor consuming it.** Until there is,
rivers remain invisible beyond ~26 m.

Known negative result to design around: a minimum-screen-width policy fails.
Widening in the ground plane pushes the ribbon onto ground higher than the
water — at 20 km, **58.2% of the widened edge is below drawn ground**.

### Phase 5 — the two-renderer tone problem
The sea is drawn by a 40 km plane and is deliberately not meshed; lakes and
river reaches **are** meshed. Every boundary between them is a shell meeting a
surface. Measured at the lake seam: **+44.5 vs +76.8** blueness. Measured at a
breach, using the control's own plane as reference: where the shell draws, the
difference runs **−14.1 to +9.0, mean −1.3 — not a constant offset, and the
sign flips with viewing angle.** A fixed tint will not reconcile them.

Still unmeasured: the **river/sea** join. It could not be taken because every
tile in that cache is `bake_ver 7` with no water plane.

### Phase 6 — deferred, with owners' notes already written
- **#53 caves**, shelved at v27. `docs/underground-entrance-rework-plan.md`
  records the diagnosis (a primitive clipped by terrain is still a primitive)
  and what must not be re-litigated.
- **#52 slicing**, scoped above. The only open unknown is whether the transport
  supports byte ranges.
- **#55 rising breach — CLOSED, and the recorded diagnosis was wrong.** It was
  never the CA activity rule. Sea + tunnel + shaft is one component far over the
  hydrostatic cap, so Phase C declined to level it at all — which is also why
  the body went inactive. The "small sea does reach the datum" control varied
  two things at once: a small sea is a small *component*, i.e. one under the
  cap. The note that 64× the front budget changed nothing was correct and
  consistent. At `kWaterCAVersion` 5 the unbounded sea reaches the datum
  (z −9 → −1). See `docs/measurements/hydrostatic-cap-2026-08-04.txt` §7.
- **A reservoir over 65,536 cells never levels — FIXED at `kWaterCAVersion` 5.**
  The cap is now a path selector, not a give-up threshold: over it, the same
  bottom-up allocation is computed from a per-brick membership mask and a z
  histogram, with no cell list and no sort, and refused outright unless the sum
  of its targets equals `totalVol` as exact integers. Two things in the old
  note are worth correcting for anyone who reads it in an archive. **The cap
  never bounded the flood** — it stopped *recording* at the cap and kept
  walking, so 99% of flood pops on a 4×-over breach were spent on discarded
  answers. And **the two "real blockers" (splits, the visited obligation) gate
  ADR-0003's option B, not correctness**: there is no union-find in the
  codebase, the pass re-floods every tick, and the per-brick visited masks
  already exist — so it already held the exact cell set and volume at the moment
  it discarded them. Observability shipped with it and is always compiled:
  `hydroGaveUp()`, `worstDeferral()`, both persisted in the water blob. **The
  test for "the water is done" is now
  `activeBrickCount() == 0 && !hydroGaveUp()`.** One consequence to design
  around: correct levelling of an *unbounded* implicit sea does not terminate
  (300 → 1500 ticks takes the cove from 8,908 to 31,168 mobilized bricks), so
  the mobilized ceiling (9b) is no longer optional — the deferral was bounding
  that runaway by accident.
- **#58 MSVC** — `NOMINMAX` added to `bankprobe.cpp` and `riverribbonprobe.cpp`,
  **not verified**: there is no Visual Studio on this box. Three of five bench
  tools include `windows.h` and only the two written after the first breakage
  guard against it; make it a shared header or a CMake definition rather than a
  thing each author must remember.

---

## 6. Working rules that were learned the hard way

- **One UE editor per box.** Two capture agents once destroyed each other's
  frames for hours. Serialise anything editor-bound; everything else can run in
  parallel worktrees.
- **The owner judges screenshots, not you.** Deliver conditions and numbers, no
  verdict. Readings here have been wrong in both directions.
- **Say what a capture depicts.** Two ocean frames were served without noting
  they were synthetic fixture digs; the owner reasonably judged the shape of a
  `CarveSphere` stack as if it were terrain.
- **Confirm a capture settled**, and **the rule differs by altitude**:
  `jobsInFlight=0 pendingJobs=0 unloaded=0` always, plus
  * **near field (~10 m)** — `RefreshImplicitWater: DRAINED`;
  * **altitude** — the ribbon actor's `River ribbons: DRAINED build`, with its
    reach and quad counts.

  `RefreshImplicitWater: DRAINED` **cannot appear in an altitude capture**: the
  implicit disc is only 52×52×26 m around the camera, so at 1 km there is no
  implicit water in it and the line is structurally absent. Reading that as a
  failed settle is wrong — and worse, learning to ignore it lets a genuinely
  unsettled *near-field* frame through. A blank frame is usually unloaded
  terrain.
- **The altitude ladder is 10 m, 200 m, 1 km, 5 km** (owner, 2026-08-04; 20 km
  was judged excessive). 200 m is the one that matters most: the near-field box
  reaches only ~26 m, so 200 m is the lowest altitude where the far-field ribbon
  carries the river alone, and it is where a bad handover between the two would
  show.
- **Verify sites before shooting.** Three of nine vista sites were once wrong,
  including a "beach" in open water. One candidate shore this session was
  rejected on survey: max **+0.19 m** over 80×80 m — a shoal.
- **Check the first tile before spending the rest.** A bake once wrote a tile
  with the water flag clear after 302 CPU-s and said nothing. `_encode_fine` now
  refuses to write a product the encoder does not accept and logs `water=N%wet`.
- **`ctest` needs `-C Release` on this generator**, or both suites report
  "Not Run" and look green.
- **Scan for junctions before any recursive delete.** `git worktree remove` once
  wiped the tile cache through one.

## 7. Baselines

```
ctest       2/2, 383 cases -- needs -C Release, or both suites report
            "Not Run" and look green
vxc_tests   422 C++ tests, 0 failures        (claude/water-integration)
pytest      546 passed / 2 skipped   (was 539; #209 added 7)
bake        ~300 CPU-s per fine tile
tiles    D:\voxelsim\tile-cache\...-b196f6020\...\s16\   bv9
         ...-b4d02b092   bv10 (carried Q) -- CODEC_RAW, ~210 MB/tile
         ...-b52995abb   bv12 (seam crossing + width, PR #209) <- LOAD THIS
         ...-bd3d0ddc7   bv8, NO water plane. 38 tiles, and the newest
                         directory by date -- do not mistake it for the
                         newest WATER cache. Check bake_ver, not mtime.
```

**Until task C0 (PR #214) there was NO bv11 or bv12 cache at all**, because
every bake after PR #209 ran with `--diagnostic`, which write-protects the fine
tier. Every capture taken before that showed width p90 **1.88 m** where bv12
gives **7.50 m** -- the narrowest river this project will ship. That is fixed,
and the lesson stands: **a bake that does not write a loadable tile is not a
bake**, and `--diagnostic` is the flag that makes it look like one.

```
corridor (-11,-4) (-11,-5) (-12,-5) (-11,-6)   the one that carries water
         (-14,-4) … (-14,-7)                   dry; do not use as a test
```

A note on the pytest number: a terrain-diffusion venv on this box reads
531 with 1 failure because torch is installed there. Environment artefact —
every number here is from system Python 3.12.
