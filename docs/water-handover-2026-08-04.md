# Water system — handover

Written 2026-08-04, at the end of the session that built most of what is
described here. Read the first two sections before touching anything; they are
what stops you repeating a day's work.

The owner's standing requirement, in their words: **realistic flowing rivers
that start in high mountain valleys and reach all the way out to discharge into
the sea.** That is not yet delivered. Everything below is either progress toward
it or a thing that had to be fixed on the way.

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
ground; `BAKE_VERSION` (now 10) decides everything else. A water-only change
re-bakes with **elevation and flow planes bit-identical**, verified by
rebuilding superblocks and comparing `filled` and `acc` byte for byte. Use it:
hydrology changes should cost no terrain re-key.

**Caveat on that:** bit-identical *planes* still produce a different
`ELEV_DATA` digest when the flow superblock is not retained across the re-bake
(no superblock is retained under a fine namespace, so the pyramid is rebuilt
and the difference rides through incision). Measured: **696 of 603,979,776
control points, 1 mm each**. Physically nothing moved; on the wire the tile is
a new identity and forces a client re-download. Unresolved.

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
| Rivers die at tile seams | **false** | 0.5–2.3% of components touch a tile edge vs a 0.39% chance baseline; 97.7–99.5% die mid-tile |
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

Eleven branches. `claude/water-integration` (`fa16d55`) merges six of them and
is **verified: clang build clean, ctest 2/2, 422 C++ tests 0 failures, pytest
534 passed / 2 skipped.** Nothing is on `main`.

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
| `claude/river-drawable-flow` | **in flight at handover** — lower the cut and concentrate flow |

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
`claude/water-integration` is verified and unmerged. Fold in the branches that
post-date it (`ocean-captures`, `carry-q-corridor-test`, `river-frag-diagnose`,
and `river-drawable-flow` when it reports), re-run the suites, and merge. One
reviewable diff beats eleven.

The only conflicts so far were two agents *adding beside each other*: both
`CMakeLists.txt` files and `lakes.h`. Keep both sides. **Taking one side of a
`CMakeLists.txt` conflict silently unregisters a whole test file and still
passes green** — check the test count, not just the exit code.

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
Read `claude/river-drawable-flow`'s result first. Then re-bake the corridor and
measure against these exact baselines, **stitched into one grid and labelled
once**, 8-connected:

| | bv10 baseline |
|---|---|
| components | 2,014 |
| longest span | 1,113 m |
| components ≥ 2 km | 0 |
| pieces meeting the coast | 235 |
| longest piece reaching the coast | 475 m |

Widening the network while thinning every river is not a win — report the
channel **width** distribution in metres too. Current width p50 **2.65 m**,
p90 5.62 m, p95 7.95 m, max 10.60 m.

### Phase 4 — the far-field river actor
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
- **#55 rising breach** — a breach needing water to *rise* stops at the puncture
  depth, because the mobilized sea is at equilibrium and therefore inactive, and
  Phase C explores dry headroom only through active bricks. Not a budget
  problem: 64× the front budget changed nothing.
- **A reservoir over 65,536 cells never levels** (`waterca.cpp:108`, tripped at
  `:1065`, acted on at `:1180`; the count **includes air**, so it bites sooner
  than 65.5 m³ suggests). The real blockers are *splits* (union-find merges but
  never deletes) and *the visited obligation* (per-body per-brick mask state).
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
- **Confirm a capture settled**: `jobsInFlight=0 pendingJobs=0 unloaded=0` and
  `RefreshImplicitWater: DRAINED`. A blank frame is usually unloaded terrain.
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
ctest    2/2, 422 C++ tests, 0 failures     (claude/water-integration)
pytest   534 passed / 2 skipped
bake     ~300 CPU-s per fine tile
tiles    D:\voxelsim\tile-cache\...-b196f6020\...\s16\   bv9
         ...-b4d02b092   bv10 (carried Q)
corridor (-11,-4) (-11,-5) (-12,-5) (-11,-6)   the one that carries water
         (-14,-4) … (-14,-7)                   dry; do not use as a test
```
