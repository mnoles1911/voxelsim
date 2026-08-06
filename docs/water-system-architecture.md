# Water in this world: how it works, end to end

**What this is.** The durable design document for the water system — what
`world-generation-architecture.md` is for the land. It is the single place that
says how water works, what is wrong with it, what has already been proved wrong,
and what the intended direction is. **It supersedes
`docs/watershed-system-plan.md` (this file's former name) and
`docs/water-deep-dive-brief-2026-08-05.md`, both of which are now redirect
stubs.**

**Consolidated 2026-08-05.** Against `main`, `TERRAIN_VERSION` 8, `BAKE_VERSION`
14, `kWorldGenVersion` 23, `kWaterCAVersion` 5. Every number was read from the
code or measured with a probe; where a number came from somewhere else it says
so.

**The three questions this file answers, in order:**

| | where |
|---|---|
| **How does it work?** | §0–§10. Stages, constants, files, version rules, how to measure any of it. |
| **What is wrong with it?** | §11 and §11a. Three mechanisms, all measured, all located. |
| **What is the plan?** | §11b. Design intent, and what is explicitly *not* being done. |

**Two rules for anyone editing this file.**

1. **`pipeline.HYDROLOGY_RESIDUALS` is part of this document.** It is ~200 lines
   of measured findings living in a Python string in
   `terrain-service/terrain_service/bake/pipeline.py`, and it is the most
   valuable water text in the repository. It is **referenced, never copied** —
   duplicating a fact is how "three incompatible currencies" happened. Read it
   before proposing anything about the pyramid.
2. **Measurement records in `docs/measurements/*.txt` are never deleted.** A
   negative result is the most expensive thing here to reproduce.

`docs/water-handover-2026-08-04.md` remains as the session record — archaeology
only; several of its numbers have since moved.

---

## 0. What this is supposed to produce

The owner's standing goal, in his words:

> "realistic, natural looking rivers that flow from origin source points out to
> the ocean or wherever they end. Lakes and basins filled. Water placement
> determined by bake, filling terrain features cut by erosion."

And the constraint that rules out most of the cheap answers:

> "I don't want to use lighting or shadow gimmicks. I want to physically change
> the geometry."

Two consequences follow from those, and they explain nearly every design choice
below. First, **the bake decides where water is and the client only draws it** —
there is no runtime "find the rivers" pass. Second, **water that is drawn has to
be made of the same voxels the ground is made of**, at least close to the
camera, because a flat quad standing in for a river is the gimmick that was
rejected.

---

## 1. The whole thing on one page

Water gets from the sky to the screen in five stages. Each stage hands the next
one a single, named product.

| # | stage | in → out | lives in |
|---|---|---|---|
| 1 | **Climate → runoff** | rainfall and temperature → millimetres of water per year that actually run off, rather than evaporating | `terrain-service/terrain_service/bake/water.py` |
| 2 | **Runoff → discharge** | runoff over an area → **Q**, the cubic metres per year passing each cell, accumulated downhill | `bake/water.py`, `bake/flow.py`, and the hydrology pyramid in `bake/pipeline.py` |
| 3 | **Discharge → channel size** | Q → how wide and how deep that watercourse is | `bake/water.py`, mirroring `voxel-core/include/voxelcore/channel.h` |
| 4 | **Channel size → painted cells** | width and depth → which fine pixels are wet, and what height the water surface stands at in each | `bake/water.py`, written into the tile as the water plane |
| 5 | **Painted cells → pixels on screen** | the water plane → real voxels near the camera, ribbons and sheets further out, a plane for the sea | `ue-project/Source/VoxelEarth/VoxelWaterSubsystem.cpp` and friends |

**Three of those five can each, on their own, produce a completely dry
screen** — stage 2 (no discharge reaches the coast), stage 4 (discharge is there
but no cell is marked wet), and stage 5 (cells are wet but nothing draws them at
this range). That is why "there is no water in the screenshot" is never a
diagnosis. It is a symptom with at least three causes, and §10 gives the probe
that separates them.

---

## 2. Stage 1 — climate to runoff

**What it does.** Turns the climate rasters into a single number per coarse
cell: runoff in millimetres per year. Runoff is rainfall *minus* what the
atmosphere can evaporate, which is the Budyko relation. `runoff_field_mm_yr` in
`bake/water.py` computes it; `basins.pet_mm_yr` and `basins.budyko_runoff_mm_yr`
do the arithmetic.

**Why it is not just rainfall, with a number.** Cold and wet beats warm and wet,
often decisively. Three tiles measured through the bake's own chain:

| tile | rainfall mm/yr | mean temp | evaporative demand | runoff mm/yr | share of rain that becomes river |
|---|---|---|---|---|---|
| (−4,−4) alpine | 1,449 | −4.4 °C | 189 | **1,262** | **87%** |
| (−15,−16) tropical | 2,724 | 22.0 °C | 1,391 | **1,515** | 56% |
| (−14,−6) arid | 306 | 25.1 °C | 1,729 | **8** | 2.5% |

The tropical tile gets nearly twice the rain of the alpine one and yields only
20% more river.

**It runs on the coarse grid, deliberately.** 576² floats, about 1.3 MB, gathered
to the fine grid at `y // scale`. Materialising it at fine resolution would be
340 MB of 16×16-replicated float32 inside the bake for no new information.

**What the world actually has.** Across all 289 coarse tiles: p25 3, p50 13,
p75 103, p90 364, p99 1,238, max 1,515 mm/yr. Put plainly: **the world is not
short of water. It was short of water we had looked at** — every water number
in this project before 2026-08-05 came from four tiles at 149–292 mm/yr, around
the world's 80th percentile. See §10.

---

## 3. Stage 2 — runoff to discharge

**What it does.** Adds up, for every cell, the water arriving from everything
uphill of it. The output is **Q**, in cubic metres per year. This is the only
currency stages 3 and 4 read.

**The change that made it real.** Until `bake_ver` 10 the bake did not carry Q up
the drainage pyramid — it reconstructed a proxy at each tile from local area
times *local* runoff. Any river that left the climate zone it was born in simply
evaporated on paper. Measured down one corridor, runoff falls 217.5 → 12.8 →
3.6 → 0.5 mm/yr, so the coast saw 1.27e6 m³/yr against a 3.15e6 threshold while
the coarse whole-world model said 5.87e7. Carrying real Q gives **2.50e7 at the
coast — 40% of the whole-world figure, up from 2.2%**.

**Single-receiver routing, and why.** Water leaves each cell to exactly one
neighbour (D8), not spread across several (multiple-flow-direction, "MFD"). Both
the fine tier (`water_flow_single_receiver`, `bake_ver` 11) and the pyramid's own
sweep (`water_pyramid_single_receiver`, `bake_ver` 12) do this. Spreading is
physically defensible but it fights the consumer: stage 4 applies a hard
threshold, and a fan of five half-sized streams clears no threshold that the one
combined stream would. Measured across one tile seam, the *same* 8.6e7 m³/yr
crossed under either rule — the ratio was 0.99 — but MFD's largest single
crossing carried 8.66e6 against D8's 2.02e7.

**There is also a numerical trap here worth knowing.** `_accumulate_mfd`
evaluates its weights in the surface's own dtype. On float32, a nearly-flat
surface's slope (~2.6e-4) raised to a high concentration exponent **underflows
to zero at about p = 11** — every weight zero, the cell reads as a pit, and its
entire accumulated discharge is dropped. This happens exactly on the flat
near-coast ground rivers must cross. At p = 32 more than half the world's water
budget is lost; D8 loses less than 1e-9. Pinned by
`test_accumulate_d8_beats_high_p_underflow`.

**One documented hazard.** `fill_depressions` fills *every* depression, including
ocean basins. A whole-world accumulation therefore reports enormous "rivers"
that are submarine sinks. The largest such figure, 82.5 m³/s, is a sink at
**−3,132 m below sea level** — see §13, because that claim is still sitting in a
tool's docstring.

---

## 4. Stage 3 — discharge to channel size

**What it does.** Turns Q into two lengths: how wide the channel is and how deep
the water in it is. These are the Leopold & Maddock downstream hydraulic
geometry laws — width and depth as power laws in discharge.

```
width_m        = 1.5 m  x (Q / Qp) ^ 0.3984          # CHANNEL_REF_WIDTH_M, CHANNEL_WIDTH_EXP
channel_depth  = 0.3 m  x (Q / Qp) ^ 0.3516          # CHANNEL_REF_DEPTH_M, CHANNEL_DEPTH_EXP
water_depth    = 3/4 x channel_depth                 # the other quarter is bank freeboard
Qp             = 315,576 m3/yr = 10 litres/second     # Q_PERENNIAL_M3_YR
```

**What the anchor means.** `Qp` is the discharge at which a watercourse is
*perennial* — wet every month rather than only after rain. Below it, a carved
gully is dry most of the year. The reference channel at that discharge is a
1.5 m wide, ankle-deep trickle, and every larger river is scaled off it.

**The freeboard is not decoration.** Water fills three quarters of the channel,
so the channel rim *is* the bank crest. Anything that raises the waterline to
the rim removes the bank.

**The laws are exact, and that is the problem.** Measured across a 6.3× change in
discharge between the arid corridor and the wet alpine block: width went up
2.07× against a predicted 6.3^0.398 = 2.07, and depth 1.91× against 6.3^0.352 =
1.92. Per Q decade on the wet block, observed depth against the law:

| Q m³/yr | cells | law says | observed p50 |
|---|---|---|---|
| 1e6–1e7 | 248,488 | 0.496 m | **0.499 m** |
| 1e7–1e8 | 103,495 | 1.024 m | **1.027 m** |
| 1e8–1e9 | 3,089 | 1.961 m | **1.961 m** |

**Nothing is clamping.** `CHANNEL_MAX_DEPTH_M` is 25 m and would need
Q = 9.18e10 m³/yr; the largest Q anywhere in this world is 1.68e8, which is
**0.18% of the cap**. The 400 m width cap needs 6.8e10 for even 200 m.

**What that means for the game.** Because the exponents are around 0.35–0.40, a
river that looks **10× bigger needs about 320× the discharge**, and this world's
entire trunk-discharge range spans about 6×. **No region of this world can
produce a dramatically bigger river under these laws.** The owner has pushed on
this twice — *"Why are rivers a maximum of 2 m deep?"* — and he is right. It is a
design decision that was inherited rather than made; the brief's §4.2 frames the
options.

---

## 5. Stage 4 — channel size to painted cells

**What it does.** Decides which fine pixels (1.875 m each) are wet, and what
height the water surface stands at in each one. The result is the **water
plane**, a section written into every fine tile: one 16-bit control point per
pixel, quantised at 100 mm, relative to the tile's base offset.

This is the stage that has changed most, and the changes are worth listing
because each one was a specific visible complaint.

| bake_ver | change | why |
|---|---|---|
| 9 | the water plane exists at all: discharge, water heads, a graded surface | — |
| 10 | discharge is carried up the pyramid instead of reconstructed locally | rivers vanished when they left their climate zone (§3) |
| 11 | drawable threshold drops about an octave (2.0 → 1.5 pixels); fine routing goes single-receiver | the corridor's water was 2,014 disconnected pieces, longest reach 1,113 m |
| 12 | the pyramid routes single-receiver too; extent comes from the width law | rivers ended at tile seams; the drawn river was a one-pixel line |
| 13 | **lateral fill** replaces the width law: a cell is wet when it stands below the surface of the water it drains into | owner, flying it: *"to the left and right of the water channel, there is empty air, not a river bank"* |
| 14 | **face contact**: raise each cell's surface to reach its neighbours' beds, and add the corner a diagonal step needs | owner: *"several cubes of water placed in a general direction but disconnected going down the slope"* |

**The threshold, in plain terms.** A channel at initiation is 1.5 m wide against
a 1.875 m pixel, so the smallest rivers are narrower than one pixel and cannot
be drawn at all. `q_drawable_m3_yr` *derives* the cut from the width law rather
than stating it separately, so it can never drift from the law. At 1.5 pixels
that is **1.53e6 m³/yr, or 0.048 m³/s — still 4.8× the perennial threshold.**
The gap between "perennially wet" and "drawable" is not hidden: `water_head_mask`
reports both counts, so the number of real streams this raster cannot draw stays
visible.

**Lateral fill (`fill_to_local_surface`), the big one.** Every earlier extent rule
read *discharge*. This one reads the ground and the water's own surface and
nothing else — no width, no radius, no relief term. So how wide a river is stops
being a property of Q and becomes a property of the valley, which is the point:
the same river should be wide on a floodplain and narrow in a gorge. It costs
about 3 CPU-seconds per tile (one topological sweep over the flow forest, no
heap, no search) and produced **7.15× the wet cells**. It leaves the centreline
untouched, so the long profile is the same array `bake_ver` 12 produced.

**Face contact (`bridge_to_face_contact`), and the geometry fact behind it.** The
client draws one flat slab per fine pixel, running from the ground up to that
pixel's surface. Two neighbouring pixels therefore only share a **face** when the
lower one's surface reaches the higher one's bed — and **two voxels touching
diagonally share an edge, not a face, which the client cannot draw at all.** On a
descending bed that means a river that is one connected thing on paper is a
chain of disconnected cubes on screen. Measured on the `bake_ver` 13 corridor:
the same wet cells formed 24–75 pieces in plan and **3,409–7,987 pieces as
drawn**, and on ground steeper than 0.15, up to **20.3% of wet cells touched
nothing at all**. The fix is exactly a no-op on standing water — every cell in a
pool is already below its neighbours' level — so the surface stays horizontal
where water stands and becomes bed-parallel where it runs, with no slope
threshold anywhere to leave a seam.

**A rule the whole stage rests on: the water surface never rises going
downstream.** `graded_water_surface` enforces it. Without it, water flows uphill
somewhere and the fill rules chase it.

### 5.1 Lakes and basins

*(This is the material that older code comments and `docs/vxtl-v2-format.md`
cite as "watershed-system-plan §4.2–§4.3". It lives here now.)*

**Keep the hole.** The bake fills every depression so that water has somewhere to
route to — but a filled depression is a lake bed packed with stone, and the
player would stand on it. So the fill depth is kept as a field,
`basin_depth = filled − carrier_plus_roughness`, and subtracted back out of the
shipped ground: `z_out = z − basin_depth`. Routing gets its pit-free surface;
the world gets its hole. Measured on two exemplar tiles, **1.7–2.2% of a tile is
lake bed**, up to 42–135 m deep — that is not a rounding error, it is a
noticeable landscape feature per tile.

**Whether a basin holds water is a water balance, not a dice roll.** For a basin
with inflow `Q_in`, precipitation `P` and evaporative demand `PET` over a surface
area `A(h)` that grows with water level `h`, solve

```
Q_in + P x A(h) = PET x A(h)              # inflow balances net evaporation
surface = min(spill_level, h*)            # it cannot rise above its own outlet
```

and classify what comes out. The five kinds, stored as one byte in the basin
registry:

| kind | meaning | what the player sees |
|---|---|---|
| 0 | dry playa | a bare basin floor |
| 1 | salt flat | a basin that fills and evaporates dry |
| 2 | seasonal | fills part of the year |
| 3 | lake, terminal | a permanent lake with no outlet |
| 4 | lake, overflowing | a permanent lake that spills into a river |

This is what makes an arid province and a wet province look different for a
reason rather than by a tuned constant: the same basin geometry gives a playa at
8 mm/yr of runoff and an overflowing lake at 1,262.

**The registry is cheap.** About 24–32 bytes per basin — id, seed pixel, bounding
box, spill height, surface height, kind, outlet — and tens of basins per tile, so
**under about 2 KB per tile** against a tile of tens of megabytes.

**Measured on the corridor:** 255 registered basins, **246 overflowing and 9
terminal, with zero dry playas, salt flats or seasonal basins** — so in that
climate the classifier removes nothing. Total lake area 8.305 km², area p50
6,676 m² and max 2.82 km², depth **p50 3.03 m, max 80.80 m**. Every one of the
255 satisfies `floor < surface ≤ spill`, and none has an empty extent.

**Lakes are held separately from rivers, on purpose.** Registered basins are
written **dry** in the river plane, and the client unions the two back together
(`CompositeWaterSampler`). The exclusion is exactly one copy of the fact rather
than two that can disagree. Verified on the corridor: all 2,362,237 cells inside
a lake extent are dry in the river plane.

**One consequence that surprised everyone.** In wet country the lakes are not a
garnish. On the longest reach of the wet alpine block, **42.3% of it is lake
sheet rather than river**, and the lakes are 22× deeper than the river is. The
lake path has had far less scrutiny than the river path and is what the player
mostly sees there. See §11.

---

## 6. Stage 5 — painted cells to pixels

Four different things draw water, at four ranges. This is the part of the system
with the most accumulated machinery and the least design.

### 6.1 Near field — real voxels

`RefreshImplicitWater` in `VoxelWaterSubsystem.cpp` meshes actual water voxels in
a box that follows the **camera**, not the water:

```
kImplicitRadiusBricks  = 32   ->  +/-25.6 m horizontally
kImplicitRadiusBricksZ = 16   ->  +/-12.8 m vertically
(8-voxel bricks of 10 cm voxels; 65 x 65 x 33 bricks = 52 x 52 x 26 m across)
budget: 192 bricks per tick
```

**The vertical limit binds first, and it is the single most common cause of "no
water in the shot".** A camera 90 m above a river is offered *no* wet column at
all — the measured capture logs `0 candidate brick(s)`. At 10 m altitude and
−20° pitch with a 90° field of view, the bottom of the frame first touches ground
8.6–9.3 m ahead, so the usable window is roughly **9 m to 25 m**.

**The mesher is already optimal here.** `meshBrick<8>` hits the theoretical 64:1
maximum on flat water tops. Any proposal that offers to "reduce water geometry"
is competing against a win already banked.

**There is exactly 8% headroom.** With the underground ground-floor fix, demand
against the 11,520 bricks/second drain sits at **0.92×**. Before it, 78×. This is
the number that constrains everything in §6.4: a 32 m ring base would need 1.55×
the window area, which is 1.43× the capacity, which is over.

### 6.2 Lakes at distance — sheets

`VoxelWaterSheetActor` + `lakeSheetRects` decompose a basin's wet mask into
axis-aligned rectangles, point-sampling at block centres. It is structurally
**lakes only** — the basin registry, `holdsWater()` and `extentMaskFor` all assume
a basin, and `CompositeWaterSampler` deliberately forwards the sheet half to
lakes. Cost measured at 2560×1440: about **0.12 ms at p95**, 0.7% of a 16.7 ms
budget, in a frame where the lake covers roughly 30% of the pixels.

The owner has already rejected this decomposition on a river: *"sharp,
rectangular, square edges where it meets land rather than a natural curving,
arching shoreline."* At roughly 15 m per rectangle against a river 1–3 pixels
wide, a river drawn this way is not a river with a bad edge — it is a chain of
disconnected 15 m squares.

### 6.3 Rivers at distance — ribbons

`riverribbon.h` turns the water plane into **ordered centreline polylines**, one
per reach, each carrying its own surface height and measured width;
`VoxelRiverRibbonActor` sweeps them into quads. All the shape decisions —
centreline selection, tracing, and the Douglas-Peucker simplification that
removes the raster staircase — stay in `voxel-core`, where they are unit-tested
without an engine. The actor budgets the work, converts millimetres to world
units and sweeps. That split is deliberate: "no staircase" must stay a tested
property of the library, not an untested property of an engine file.

The ribbon clips itself against the near-field box's exact footprint so the two
never draw the same water twice and z-fight.

**Widening the ribbon to keep it visible at range was measured and it fails.**
Widening in the ground plane pushes the edge onto banks that are higher than the
water, where the depth test eats it:

| widening | drawn width | fraction of the edge below drawn ground |
|---|---|---|
| none (centreline) | 2.65 m | **0.00%** |
| 2× | 5.30 m | 7.94% |
| 4× | 10.59 m | 27.94% |
| 8× | 21.18 m | **58.41%** |

A 58%-buried ribbon is not a wide river, it is a dashed line — and one that
re-dashes as the camera moves. So the policy is: **draw at the width the bake
drew.** `-VoxelRiverRibbonMinPx` exists to take the A/B and defaults to 0. The
honest conclusion, recorded so nobody re-derives it: a 2.65 m creek is genuinely
below the resolution of a 20 km vista, and the way to put a river on that
horizon is to make the river wider **in the bake**, not wider at draw time.

### 6.4 The sea, and the half-built cascade

The sea is a 40 km camera-following plane at z = 0 (`kSeaLevelMm = 0`), plus a
real ocean **datum** in the water sampler — the sea is water to the simulation,
not just to the renderer. Before that, a cove cut into the coast *drained*
instead of filling and never settled: volume 358k → 610k fill units and active
bricks 536 → 758 between ticks 100 and 1500, still climbing. A live CPU leak
behind every below-sea-level dig, hidden by the plane. With the datum the same
cove settles at tick 54.

**The gap everyone runs into.** Between 25.6 m and the horizon, flowing water is
ribbons and sheets — flat quads — which the owner rejected on sight: *"The flat
quads look terrible within 1 km. We need more real voxels out to 500 m or 1 km."*
The agreed answer is a **ring cascade**: concentric rings of progressively
coarser water voxels, where ring L covers `[base<<(L-1), base<<L)` metres with a
`(0.1<<L)` m voxel — each ring has 4× the area and ¼ the resolution, so the cost
per ring is flat. Priced at **16,872–31,059 bricks and 38,282–49,528 quads out to
1,024 m: 1.45×–2.62× today's box for 40× the radius.** It is half-built on branch
`claude/water-ring-cascade` and is not on `main`; see the brief §4.3 for what is
done, what is not, and the two constants that must move together.

### 6.5 How water is drawn, once it exists

The water surface has its own GPU pool (`voxel.Water.GPU`, default on), mirroring
the terrain pool: 8-voxel water bricks instead of 32-voxel terrain chunks, always
mip 0, translucent and two-sided with opacity varying 0.18–0.95 by depth and
foam. Because it is translucent it must be sorted, and it is sorted by **spatial
bucket** — 64 water bricks (51.2 m) of world space per bucket, typically 4–8
buckets live, capped at 12, each owning a 4 MB quad buffer. Details, including
the sorting argument that this replaced, are in `docs/gpu-water-pool-design.md`.

### 6.6 Player-disturbed water

Baked water is a **datum** — a height field the client fills up to. It is not a
flow. Two consequences, both measured:

* **Damming a baked river does nothing.** A wall across an undisturbed reach
  settles in 1 tick and the upstream level does not rise by a single voxel:
  every untouched part of the river is, to the simulation, a wall.
* **Draining one runs away.** A single 4-voxel shaft near the downstream end
  converts **100% of the reach** to simulated water, because the mobilisation
  front follows moving water and has no length bound.

The cellular automaton that handles disturbed water (`waterca.h`,
`kWaterCAVersion` 5) steps at a fixed 10 Hz. Water that has settled is demoted
back to the datum by an **exact, zero-tolerance** predicate — a tolerant one
recreates the double-occupancy bug that one-way mobilisation existed to prevent.
The test for "the water is finished" is `activeBrickCount() == 0 &&
!hydroGaveUp()`.

---

## 7. Two version numbers, and why that is the most valuable thing here

```
TERRAIN_VERSION = 8      decides the GROUND
BAKE_VERSION    = 14     decides the PRODUCTS (everything else in the tile)
```

Until `bake_ver` 9 these were one counter doing two unrelated jobs, and fusing
them made every additive change to the file format cost a whole new world.
Measured when the split was scoped: bumping the single counter from 8 to 9 moved
the roughness seed, so **every basin in the 256-tile lake survey, every channel
the bank probe walked, every vista site and every spawn coordinate the owner
holds would describe ground that simply stopped existing** — in order to add a
section that changes no elevation byte.

So: bump `TERRAIN_VERSION` and it is a new world, invalidating every
measurement, screenshot and site anyone holds. Bump `BAKE_VERSION` and tiles are
re-baked onto **identical ground**.

**This is the only reason water can be iterated at all.** Six water changes have
shipped under it without moving a single elevation byte. Two gates keep it
honest:

* `tests/test_bake_terrain_identity.py` — re-baking a resident fine tile must
  reproduce its elevation plane byte for byte.
* `test_constants_partition_is_exhaustive` — every bake constant must land in
  exactly one of the terrain payload or the product payload. A constant in
  neither would decide baked bytes while rolling no identity at all.

And `tools/verify_water_only_change.py` is the check to run on any water change,
because it compares through the codec's **own** `elevation_control_points`
operator. A hand-rolled quantiser has already produced one false "terrain moved"
alarm on this system.

**Isolation between world variants is by cache root, not by forging an
identity.** `D:\voxelsim\tile-cache` and `D:\vox-wet-cache` hold the same
content-addressed namespace names, because the name is derived from the bake
fingerprint and nothing about the bake changed. The tile directory alone decides
which world you are standing in.

**The cost that is not yet solved.** A water-only re-bake changes 0.15–0.62% of a
tile's bytes and re-keys **100%** of them, because the provider id hashes the
product half of the fingerprint. At 289 tiles × ~45 MB compressed — about 13 GB —
that means improving rivers would make every player re-download every tile they
have cached. The owner's decision (2026-08-04) is **per-section content
addressing**, which makes the same update about 150 KB per tile, ~43 MB for the
world. Nothing is built yet; see `docs/tile-slicing-2026-08-04.md`.

---

## 8. The constants, and which file owns each

Everything the water system decides comes from this list. `bake/water.py` is the
home for the laws; `channel.h` holds the fixed-point mirror and the two agree by
construction (`CHANNEL_WIDTH_EXP` is literally `102/256`, `channel.h`'s
`kChannelWidthExpQ8`).

| constant | value | owner |
|---|---|---|
| `Q_PERENNIAL_M3_YR` | 315,576 m³/yr = **10 L/s** | `bake/water.py` |
| `Q_RIVER_M3_YR` | 100 × perennial = 1 m³/s | `bake/water.py` |
| `Q_MAJOR_M3_YR` | 1000 × perennial = 10 m³/s | `bake/water.py` |
| `CHANNEL_REF_WIDTH_M` | 1.5 m at Qp | `bake/water.py` ↔ `channel.h` |
| `CHANNEL_REF_DEPTH_M` | 0.3 m at Qp | `bake/water.py` ↔ `channel.h` |
| `CHANNEL_WIDTH_EXP` | 102/256 = **0.3984** | `bake/water.py` ↔ `channel.h` |
| `CHANNEL_DEPTH_EXP` | 90/256 = **0.3516** | `bake/water.py` ↔ `channel.h` |
| `CHANNEL_MAX_WIDTH_M` | 400 m (never approached) | `bake/water.py` |
| `CHANNEL_MAX_DEPTH_M` | 25 m (never approached; matches `incise.py` cap) | `bake/water.py` |
| water depth / channel depth | **3/4** | `bake/water.py` ↔ `channel.h` |
| `WIDEN_MIN_DEPTH_M` | 0.1 m — ground must stand this far below a reach's surface | `bake/water.py` |
| drawable threshold | derived: `q_for_width_m(1.875 m × 1.5 px)` = **1.53e6 m³/yr** | `bake/water.py` |
| fine pixel | **1,875 mm** | tile format |
| coarse pixel | **30 m** | tile format |
| voxel | **`kVoxelSizeMm` = 100 mm** | `core.h` |
| sea level | **`kSeaLevelMm` = 0** | `core.h` |
| near-field water reach | `kImplicitRadiusBricks` 32, `…BricksZ` 16 → **±25.6 m / ±12.8 m** | `VoxelWaterSubsystem.cpp` |
| levelling path switch | `kMaxHydrostaticComponentCells` = 65,536 | `waterca.cpp` |
| `TERRAIN_VERSION` / `BAKE_VERSION` | 8 / 14 | `bake/pipeline.py` |
| `kWorldGenVersion` / `kWaterCAVersion` | 23 / 5 | `core.h`, `waterca.h` |

**Sizes, since one of these has been wrong in a plan and inverted a cost
argument.** `vxc::Quad` (`mesher.h`) is nine one-byte fields — axis, sign, slice,
u0, v0, w, h, ambient occlusion, material — and `MaterialId` is `uint8_t`, so it
is **9 bytes**, measured. The packed GPU quad is **8 bytes** (one `uint64`). It is
not 152 bytes, and it is not 12 bytes either; the deep-dive brief's §2 says 12 B,
which is the right order and the wrong number.

---

## 9. The file map

| concern | file |
|---|---|
| **Bake** | |
| Runoff, discharge, the laws, every constant, all four extent rules | `terrain-service/terrain_service/bake/water.py` |
| Flow accumulation and routing | `terrain-service/terrain_service/bake/flow.py` |
| Basin survey and the lake/playa water balance | `terrain-service/terrain_service/bake/basins.py` |
| Channel incision into the ground | `terrain-service/terrain_service/bake/incise.py` |
| Stage order, both version counters, the `bake_ver` history | `terrain-service/terrain_service/bake/pipeline.py` |
| Tile encode/decode, water plane section | `terrain-service/terrain_service/tile_codec.py` |
| **voxel-core** | |
| Lake and river sampling, ocean datum, the composite sampler | `voxel-core/include/voxelcore/lakes.h` |
| Far-field centreline producer (ribbons) | `voxel-core/include/voxelcore/riverribbon.h` |
| Channel geometry in fixed point (mirror of the laws) | `voxel-core/include/voxelcore/channel.h` |
| River network from flow accumulation | `voxel-core/include/voxelcore/rivernet.h` |
| Disturbed-water simulation, levelling, demotion | `voxel-core/include/voxelcore/waterca.h` |
| Tile decode, `reconstructedGroundMm` | `voxel-core/include/voxelcore/tilestore.h` |
| Greedy mesher, `vxc::Quad` | `voxel-core/include/voxelcore/mesher.h` |
| **Client** | |
| Near field, the implicit box, the CA, the GPU pool | `ue-project/Source/VoxelEarth/VoxelWaterSubsystem.cpp` |
| River ribbon actor | `ue-project/Source/VoxelEarth/VoxelRiverRibbonActor.cpp` |
| Lake sheet actor | `ue-project/Source/VoxelEarth/VoxelWaterSheetActor.cpp` |
| The 40 km sea plane | `ue-project/Source/VoxelEarth/VoxelOceanActor.cpp` |
| Water chunk component (also the cascade's drawing site) | `ue-project/Source/VoxelEarth/VoxelWaterChunkComponent.cpp` |
| Water material / world-position offset | `ue-project/Tools/create_water_voxel_material.py` |
| **Tools** | |
| Rank the whole world by runoff and trunk discharge | `terrain-service/tools/survey_world_water.py` |
| Prove a change moved no ground | `terrain-service/tools/verify_water_only_change.py` |
| Compose river ∪ lakes the way the client does | `terrain-service/tools/corridor_composed_reach.py` |
| Fragmentation, coast reach, long profile | `measure_corridor_fragmentation.py`, `corridor_coast_reach.py`, `river_long_profile.py` |
| **Probes** (`build/voxel-core-msvc/bench/Release`) | |
| The one that answers most questions | `vxc_riverribbonprobe` |
| Datum, banks, hydrology, burial | `vxc_waterdatumprobe`, `vxc_bankprobe`, `vxc_hydroprobe`, `vxc_burialprobe` |

**Three grounds, and they have been conflated repeatedly at a cost of days.**
Name which one you mean, every time:

1. the raw **sample field** — what the bake subtracts to get depth;
2. the **spline reconstruction** — `reconstructedGroundMm` in `tilestore.h`, what
   the water datum is measured from;
3. the **amplified surface** — `GroundMmAt` / `GetSurfaceHeightUU`, what is
   actually drawn.

**The water datum is reconstructed ground plus baked depth, and it never reads
the amplifier.** Both `tile_codec.py` and `tilestore.h` forbid it explicitly. The
amplifier was blamed for burying rivers; measured, it adds **+3 mm** at the
centreline (p50), and that mistake nearly rolled `kWorldGenVersion` for nothing.

---

## 10. Where to measure, and the exact commands

**Measure on wet country.** Every water number in this project until 2026-08-05
came from four dry tiles. The owner caught it from a screenshot: *"this still
looks like the arid desert, not an alpine, high wetness, more water location."*

| region | tiles | cache root | runoff mm/yr | what it is for |
|---|---|---|---|---|
| **wet alpine block** | `-5,-4 / -4,-4 / -3,-4 / -5,-5 / -4,-5 / -3,-5` | `D:\vox-wet-cache` | 445–1,262 (mean 751) | the default. 100% land, relief 4,906 m, world p88–p99 |
| arid corridor | `-11,-4 / -11,-5 / -12,-5 / -11,-6` | `D:\voxelsim\tile-cache` | 149–292 (mean 222) | the historical baseline, world p80. Only for like-for-like comparison |

Namespaces present as of 2026-08-05, confirmed by reading the `bake_ver` byte out
of the tile headers:

```
D:\vox-wet-cache\...-ba9c62170\000000000135276f\s16\   6 tiles, bake_ver 13
D:\vox-wet-cache\...-b10cf6d2c\000000000135276f\s16\   6 tiles, bake_ver 14
D:\voxelsim\tile-cache\...-b10cf6d2c\...\s16\          4 tiles, bake_ver 14  (corridor)
D:\voxelsim\tile-cache\...-b52995abb\...\s16\          4 tiles, bake_ver 12  (corridor)
D:\voxelsim\tile-cache\...-bd3d0ddc7\...\s16\         38 tiles, bake_ver 8, NO water plane
```

That last one is the newest directory **by date** and carries no water at all.
**Check `bake_ver`, not modification time.**

### The one probe worth running first

`vxc_riverribbonprobe <tiledir> [--origin PX PY] [--region PX]`. It reports, in
order: how much of the plane is wet, whether any water block failed to decode,
how many connected pieces the wet mask is in and how far the largest spans, a
cell-by-cell cross-check that the far-field fill and the near-field sampler see
the same water, centreline widths, reach lengths and midpoints in world metres
you can spawn at, the sub-pixel widening policy as numbers, and burial.

**`--origin` is the region's bottom-left corner, not its centre.** Getting this
wrong silently samples unbaked ground and reports a dry world. The command that
covers a fully-baked 2×2 block of the wet region, and reproduces every wet-block
number quoted here and in the brief:

```
cd D:\voxelsim\build\voxel-core-msvc\bench\Release
.\vxc_riverribbonprobe.exe ^
  "D:/vox-wet-cache/terrain-diffusion-unlabeled-80b9ca451a23eae4-ba9c62170/000000000135276f/s16" ^
  --origin -40960 -40960 --region 16384
```

A verified live playtest site on that block: `-VoxelSpawnAt=-64019,-69172`, water
surface 1,728.5 m, on a 3,865 m reach.

Reproducing the bake itself:

```
python tools/survey_world_water.py accumulate --coarse-dir <cache>/<ns>/000000000135276f/s1 --out D:/vox-wet-out/worldwater
python tools/survey_world_water.py blocks --dir D:/vox-wet-out/worldwater
python tools/bake_tiles_from_cache.py --seed 20260719 --cache-dir D:/vox-wet-cache ^
  --provider-id terrain-diffusion-unlabeled-80b9ca451a23eae4 ^
  --tiles="-5,-4 -4,-4 -3,-4 -5,-5 -4,-5 -3,-5" --npz-dir D:/vox-wet-npz
```

Bake cost is about **300 CPU-seconds per fine tile**, so the six wet tiles are
about 2,300 CPU-s and the whole 289-tile world would be roughly 24 CPU-hours.
A world-scale bake is deliberately **not** scheduled.

---

## 11. What the numbers say today

Measured 2026-08-05 on the wet alpine block at `bake_ver` 13, over the 30.72 km
square given above. This is the current state of the system, not a target.

| | value | how to read it |
|---|---|---|
| tiles loaded / refused / undecodable blocks | 6 / 0 / 0 | nothing is missing |
| far-field fill vs near-field sampler | **0 disagreements** in 5.48 M cells | the two paths agree exactly |
| wet fraction of the plane | **0.560%** | |
| connected pieces | 467, largest spanning **14.63 km** | 72.5% of wet pixels are in a piece spanning ≥2 km |
| reaches over 120 m | 1,365, totalling **424.9 km** of channel | |
| longest single reach | **3,865 m** | |
| centreline width | p50 **2.65 m**, p90 15.91 m, p99 66.28 m, max 169.69 m | |
| mean channel width | 4.01 pixels = **7.52 m** | |
| water surface heights | 297.3 m to 2,964.5 m | reconstructed ground + baked depth |
| burial at the centreline | **1.05%** of samples below drawn ground, p50 **+524 mm above** | the river itself sits above its bed |
| burial at the 5 km widened edge | **56.73%** | manufactured entirely by widening |
| burial at the 20 km capped edge | **81.70%** | |

**Three things in that table are open problems, and the brief ranks them:**

1. **The painted river does not obey its own law.** Along the block's longest
   composed reach, how strongly the *law's* width tracks distance downstream is
   +0.728 (Spearman rank correlation, where +1 is perfect); how strongly the
   *drawn* width tracks it is only **+0.401**. At kilometre 20 the law asks for
   18.2 m and the water plane holds **3.75 m — two pixels**. Something between
   stage 3 and stage 4 is losing most of the growth the law specifies. This is
   the single defect that most makes rivers look wrong.
2. **32,199 centreline runs hit the 60 m width cap**, on a plane where basins are
   supposed to be written dry and wide runs should therefore be rare. Either
   basins are not being written dry, or something that is not a basin is that
   wide. **Unresolved.**
3. **Lakes dominate wet country and nobody designed that.** On the longest reach,
   **42.3% is lake sheet, not river**; lake depth p50 4.82 m and max 45.4 m
   against a river-centreline max of **2.05 m**. The deepest water on this
   "river", by a factor of 22, is standing water. In the arid corridor the same
   measure was 0.6%. That is not obviously wrong — alpine country makes lakes —
   but it means the **lake** path, which has had far less scrutiny, is what the
   player mostly sees.

**One contamination to guard against in any wet-cell statistic.** River water
paints the seafloor and the lateral fill then amplifies it: **66% of wet pixels
in the trunk block** are seafloor, and in the arid corridor 2,156,457 of
2,891,487 wet cells — **74.6%** — were the single coastal tile. Quote wet-cell
counts inland-only, or say that you did not.

**Scale, for context.** The world's land area is **29,911 km²** — roughly
Belgium — not the ~57,600 km² a full tile-grid footprint suggests, because much
of the grid is sea. (Figure carried in from the deep-dive session 2026-08-05; it
is not derived anywhere in this repository.)

---

## 11a. What is actually wrong: three mechanisms, in series

Measured 2026-08-05. **All three are independent, and all three lose river.
Fixing any one alone leaves the others in place** — which is why partial
diagnoses here have repeatedly looked like failures.

Read them in flow order: F6 loses the water before the laws are evaluated, F3
gets the depth wrong, F2 gets the width wrong.

### F6 — the pyramid delivered water onto the tile's edge, not into it — **FIXED**

`pipeline.HYDROLOGY_RESIDUALS` #7, which the module itself had already flagged
as *"SEPARATE FROM CARRIED DISCHARGE (task #49), AND NOW THE BINDING ONE."*

`_edge_entries` found crossings against the **padded domain**, whose border sits
960 m outside the interior that ships. A stream could enter the padded domain,
run its whole downstream path through the apron, and leave again without ever
crossing the tile. Nothing was lost numerically — the water was delivered to the
wrong 960 m.

Measured on both afflicted tiles, each A/B in one process with the superblock
loaded once and shared, so the constant is the only difference:

| tile | interior max Q, before → after | padded ÷ interior | drawn cells |
|---|---|---|---|
| **-7,-5** | 1.30e6 → **3.91e8** m³/yr (0.041 → **12.40 m³/s**) | 275.1 → **1.004** | 45,099 → 118,701 |
| **-14,-5** | 2.22e6 → **1.38e7** m³/yr (0.070 → **0.438 m³/s**) | 5.97 → **1.013** | 120,980 → 139,542 |

12.40 m³/s against the 14.2 m³/s `survey_world_water.py trunk` calls the largest
accumulation on land: (-7,-5) now carries essentially the whole river.

**The OFF arm reproduces the repo's own independent measurement of (-14,-5)
exactly.** `HYDROLOGY_RESIDUALS` #7 records "the INTERIOR tops out at 2.22e6",
"a padded cell carries 1.32e7", and "a 6x drop"; this harness read 2.2185e6,
1.32495e7 and 5.97. That validates the harness, not only the fix.

**The inflow moves in both directions, and that is correct.** On (-7,-5) it
rises 0.2% — the predicted apron double count, estimated at 0.1% beforehand. On
(-14,-5) it *falls* 6.6%, because parent cells whose flow crosses the padded rim
but never reaches the interior are now correctly excluded: a stream that only
clips the apron no longer counts as having reached the tile.

The fix is `water_inject_at_interior_rim` — crossings taken against the
**interior** rectangle for the discharge currency only. **Water-only, asserted
not assumed:** elevation, accumulation and flow are bit-identical between arms,
0 of 67,108,864 cells. Two costs, both in the code: a bounded double count
(predicted 0.1% from the apron's own yield, measured 0.2%), and it breaks the
"one set of crossings, two currencies" invariant, so a cell can carry a
discharge whose matching upstream *area* never arrived.

Full record: `docs/measurements/f6-pyramid-delivers-to-apron-2026-08-05.txt`.

### F3 — the depth law has no slope in it — **open**

`water_depth_m(Q)` is Leopold–Maddock hydraulic geometry: depth depends on
discharge and nothing else. That is a fit to lowland rivers at roughly constant
slope, and the wet block's long profile runs **173 → 29 m/km**.

Normal-depth flow says depth goes as `(Q / √S)^(3/5)`, so the current law puts
too much water on steep upper reaches — where it then cannot stay connected —
and too little on flat lower ones. `bridge_to_face_contact` is a hand-built
correction for the missing slope term, which gives the fix a falsifiable
acceptance test: **with slope in the depth law, the bridge should become close
to a no-op on steep reaches.** If it is still doing heavy lifting, the depth
model is still wrong.

### F2 — the drawn width barely tracks discharge — **open**

Measured with the bake's own `lateral_extent_stats`, 20,000 transects per tile,
perpendicular taken from each cell's own D8 receiver:

| tile | drawn p50 | room p50 | law p50 | drawn/law | overshoot gate firing |
|---|---|---|---|---|---|
| -4,-4 | 18.8 m | 24.4 m | 1.5 m | 11.3× | 79.9% |
| -5,-5 | 16.9 m | 20.6 m | 1.5 m | 10.0× | 80.8% |
| -3,-4 | 28.1 m | 37.5 m | 1.5 m | 17.5× | 87.5% |

**The drawn river is 10–17× wider than the law at the median wet cell**, and the
bake's own `ratio_over_4_frac` overshoot gate fires on 80–87% of samples.

This is the *same defect* as §11's item 1, seen from the other end. §11 measures
the centreline at km 20 where Q is high (law 18.2 m, drawn 3.75 m — too narrow).
The table above measures the median wet cell, a sub-perennial headwater whose
law width is floored at 1.5 m — too wide by 10×.

**Drawn width is roughly 20 m everywhere.** `fill_to_local_surface` sets extent
from how much ground lies below the local water surface — a property of terrain
flatness, which is scale-free — and discharge enters only through the surface
height, which moves as `Q^0.352`, far too weakly to shape width. That is the
mechanism behind Spearman(drawn width, Q) = +0.457.

The rule already uses **75–82%** of the room available to it, so this is not a
case of the terrain withholding space.

### And one that is NOT wrong: the terrain

**No terrain change is required by any of the above, and Stage 0 measured why.**
Off-network the bake reproduces its own carrier to **2 cm**; the broad valley
form comes from the 30 m source and is preserved. The bake's whole net
contribution to channel geometry is a **3.4 m** median depression.

Incision *is* cap-bound over most of the network (p90 and p99 both exactly
25.00 m, `incise.py`'s `cap_m`) and B4/B4b/B5 put **12.6 m** back of the 20.9 m
it cut — so the shipped channel is the residue of two large opposing operations.
That is a real finding about the terrain bake, and it is **not** what makes
rivers wrong. Filed, not acted on.

Full record: `docs/measurements/valley-bed-vs-extent-2026-08-05.txt`, which also
carries the retraction of a wrong first answer and the two method errors that
produced it.

---

## 11b. Design intent

Where this is going, and — equally load-bearing — what is deliberately not being
done.

**One solve.** Rivers, lakes and the coastline should come out of a single
solver producing a single water-surface field, instead of two authorities
composed at runtime with nothing forcing them to agree at every junction (§5.1,
and §11's item 3 — 42.3% of the showcase reach is the seam between them).
Depressions yield spill heights; discharge routes through the **spill graph**
with lakes as nodes on the river rather than holes in it; each lake takes its
level from water balance capped at its spill.

**The ocean stays implicit, but its coastline joins the solve.** The depth plane
is int16 at 10 mm — max 327.67 m — so it cannot hold deep ocean, and the ocean
needs no data: `groundMm < kSeaLevelMm`, where sea level is the definition of
the coordinate system rather than a solved quantity. What *does* join is the
boundary: a flow path reaching a below-sea column terminates at exactly zero, and
a below-sea depression connected to the ocean is ocean. That retires the
`_sea_taper` hack.

**Solve on the shipped surface.** Today `graded_water_surface` grades along the
pre-B5 routing surface while the extent test and the stored depth use the
post-B5 shipped one. Closing that split makes a whole bug class unwritable.
Note this is *not* a complaint about the amplifier — see F1 under §13.

**A permanent visual instrument.** Water is invisible past ±25.6 m horizontally
and ±12.8 m vertically (§6.1), so every visual judgement about placement has
been made through a 25 m bubble. Solid marker voxels rendered through the
terrain path inherit full view distance, the LOD chain and the capture harness
for free, and cost nothing when switched off. This is an instrument, not a
feature: it separates "the model puts water in the wrong place" from "the
renderer cannot draw it".

**Explicitly not being done, each for a measured reason:**

- **No terrain re-bake, no lateral erosion stage, no shipped bed correction.**
  The valley form is the source's and the net channel slot is 3.4 m. See §11a.
- **No detail-band change as a water fix.** F1 is retracted (§13). The drainage
  defect in `client-detail-drainage-2026-07-29.txt` is real but is a *terrain*
  concern and must be judged on `drainage-ladder.ps1`, not on rivers.
- **No new source-point model.** `water_head_mask` already derives origins from
  real Budyko runoff over the real climate planes. It is correct and is reused
  unchanged.

**The one decision this document cannot make.** §11's exponent problem (see also
the brief's old §4.2): `width ∝ Q^0.398` means a 10× visibly bigger river needs
~320× the discharge, and this world's entire runoff range spans about 6× at the
trunk. The laws are *exact* against observation — nothing clamps — so this is a
design choice, not a bug. A bigger world or coarser cell so trunk Q can
accumulate; different exponents (the current ones are Earth's, and a game may not
want Earth's); or decoupling *visual* width from hydraulic width below some Q,
explicitly rather than by accident as in F2. **It should be chosen, not
inherited.**

---

## 12. The rest of the water documentation

**Current — read these.**

| file | what it holds |
|---|---|
| this file | the system as built, what is wrong with it, and the intent |
| `docs/water-handover-2026-08-04.md` | the session record of how it got here; corrected in place where numbers moved |
| `docs/water-wet-country-2026-08-05.md` | why the wet block was chosen and what it changed. Its numbers are `bake_ver` 13 |
| `docs/gpu-water-pool-design.md` | how the translucent water surface is drawn and sorted |
| `docs/w3-channel-carving.md` | `channel.h`, the fixed-point geometry, and what wiring it into the ground surface would cost |
| `docs/river-farfield-actor-2026-08-04.md` | the ribbon actor and the widening measurement. Its captures are `bake_ver` 10 |
| `docs/tile-slicing-2026-08-04.md` | fetching a tile in pieces — the fix for the re-download cost in §7 |
| `docs/lake-survey/lake-survey.md`, `docs/water-map/ocean-captures.md` | survey and capture records |

**Superseded — kept for history, banner at the top of each.**

| file | superseded by |
|---|---|
| `docs/water-deep-dive-brief-2026-08-05.md` | this file. Its Settled / Falsified / Traps content is folded into §12a, §13 and §14; its open cracks are superseded by §11a, which measured two of them to different conclusions |
| `docs/watershed-system-plan.md` | this file, under its former name |
| `docs/water-production-plan.md` (2026-08-02) | this file. Its branch triage in §10 is entirely spent |
| `docs/water-waves-plan-2026-08-04.md` | this file. Waves A and C0 landed; B2 closed with a negative result |

**Measurements — `docs/measurements/*.txt`. Never delete one.** A negative result
is the most expensive thing in this repository to reproduce, and this project has
re-run dead ends because a note was lost. The water-relevant ones, and what each
is still good for:

| file | still stands? |
|---|---|
| `carried-discharge-task49-2026-08-04.txt`, `carried-discharge-corridor-rebake-2026-08-04.txt` | yes. Includes the **negative** result: carrying real Q did *not* coalesce the wet mask (1,954 → 2,014 pieces) |
| `river-drawable-and-concentration-2026-08-04.txt` | yes. The apportionment behind `bake_ver` 11 |
| `river-fragmentation-diagnosis-2026-08-04.txt` | yes as data. Its **seam mechanism was later corrected** — the downstream tile was never starved, it was divided |
| `river-seam-and-width-2026-08-04.txt` | yes. Its "165 m position error" reading is superseded by the next row |
| `river-seam-position-scoping-2026-08-04.txt` | yes, and it is the **closing** answer: the residual is magnitude, not position |
| `river-long-profile-2026-08-04.txt` | yes, and it is the note that caught the span-vs-length error (§13) |
| `river-lateral-fill-2026-08-04.txt` | yes. `bake_ver` 13, and it carries the `quant == 1` correction |
| `river-slope-face-contact-2026-08-05.txt` | yes. `bake_ver` 14 |
| `hydrostatic-cap-2026-08-04.txt` | yes. The cap never bounded the flood; it only stopped *recording* |
| `valley-bed-vs-extent-2026-08-05.txt` | yes. Stage 0: the terrain is not the constraint. Carries a **retracted first answer** and the two method errors behind it |
| `f6-pyramid-delivers-to-apron-2026-08-05.txt` | yes. HYDROLOGY_RESIDUALS #7 measured at 276x, and the fix's A/B |
| `client-detail-drainage-2026-07-29.txt` | yes, but it is a **terrain** result. Do not cite it as a water defect — see F1 in §13 |

**Two things known to be unmeasured, rather than wrong:**

* **The two-renderer tone seam has never been quantified at a river mouth.**
  Known to exist; nobody has put a number on it where it matters most. Cheap to
  settle.
* **32,199 centreline runs hit the 60 m width cap** on a plane where basins are
  written dry and wide runs should be rare. Either basins are not being written
  dry, or something that is not a basin is that wide. **Unresolved.**

### Branch state as of 2026-08-05

| branch | state |
|---|---|
| `claude/f6-interior-rim-injection` | the §11a F6 fix, off by default. Tests pass; A/B measured on (-7,-5) |
| `claude/water-ring-cascade` (`D:ox-cascade`, `454c635`) | material offset + levelled component landed clean; **unpushed**. The plane enumerator is a committed sketch whose own commit message says it does not compile |
| `claude/underground-w5` | shelved cave work, kept whole. **Touches `amplifier.h` (+28) and `amplifier.cpp` (+159)** — load-bearing for surface terrain, not confined to caves |
| `claude/underground-build` | conflicted with `w5` inside unreviewed code and was left unmerged deliberately. Carries 308 lines never built and never reviewed |

Anything touching the amplifier should branch from `main` and stay clear of the
two shelved variants, or there will be a third conflicting state in surface
terrain code.

---

## 12a. Settled — do not re-derive

Each of these cost real time to establish and is periodically re-litigated.

* **The bake is the authority on where water is.** The client draws it; it does
  not decide it.
* **The amplified surface is not a water datum, and that is correct.** Three
  grounds get conflated repeatedly and it has cost days: the raw *sample field*,
  the *spline reconstruction* (`reconstructedGroundMm`), and the *amplified
  surface* (`GroundMmAt`). The datum is reconstructed ground plus baked depth,
  and it **never reads the amplifier**. Measurement backs the choice: 0.52 m of
  clearance at the centreline, 1.05% buried.
* **Ocean is a real datum**, not a special case bolted on.
* **The greedy mesher is already optimal on water tops.** `meshBrick<8>` hits the
  theoretical 64:1 maximum. Any proposal that "reduces water geometry" competes
  against a win already banked. `vxc::Quad` is **9 B** on the CPU, 8 B packed on
  the GPU.
* **Near-field mesh budget has ~8% headroom.** Demand against the 11,520
  bricks/s drain is **0.92×**. Any change that grows the near-field window
  breaks this: a 32 m ring base needs 1.55× the window area, i.e. 1.43×
  capacity — over. **Ring base is 25.6 m, not 32 m.**
* **Isolation between world variants is by cache root**, never by forging a
  provider identity. `D:oxelsim	ile-cache` and `D:ox-wet-cache` coexist;
  the tile dir alone decides which world you are standing in.
* **Two version numbers are the most valuable thing here** (§7). A water-only
  change leaves terrain bit-identical, proved by
  `tools/verify_water_only_change.py`. Preserve this; it is the only reason
  water can be iterated at all.

---

## 13. Claims that are wrong and are still somewhere in the tree

Each of these was believed, acted on, and then measured. They are listed so the
next reader recognises them rather than re-deriving them.

* **"A 20,269 m river."** That is a **euclidean span** — the widest separation of
  a connected blob, hull to hull. Along its own channel that river is
  **30,577 m**, sinuosity 1.51. Span was quoted as length several times before the
  longitudinal profile caught it. Whenever you see a river length, check which
  one it is.
* **"696 control points differ by 1 mm."** The real difference was **zero**. The
  comparison put a ZSTD-coded tile against a RAW one, which invents differences,
  and `quant == 1` is the **code for 100 mm**, not the value 1 mm — a 100×
  unit error. It reached a pull request. Corrected in place in
  `water-handover-2026-08-04.md` and `water-waves-plan-2026-08-04.md`.
* **"82.5 m³/s, the world's largest river."** It is a **submarine sink at
  −3,132 m**; `fill_depressions` fills ocean basins along with everything else.
  A previous version of this list said the claim was still live in
  `survey_world_water.py`; **it has since been corrected there**, and that
  docstring now carries the full argument plus the figure that replaces it — the
  largest accumulation *on land* is **14.2 m³/s** draining 1,960 km². Read
  `trunk` before quoting either number.
* **"`vxc::Quad` is 152 bytes."** It is **9 bytes** on the CPU and 8 packed on the
  GPU (§8). The 152 B figure inverted a cost comparison in a plan.
* **"The amplifier buries the river."** +3 mm at the centreline (p50); 0.52 m of
  clearance and 1.05% buried on the fuller measurement. **Re-proposed and
  re-retracted on 2026-08-05**, from arithmetic on `kFineDetailOctaves`
  amplitudes without applying `kDetailNoiseScale` or the microrelief gates. The
  octave table is not the delivered amplitude. If you find yourself computing a
  detail amplitude by hand, measure it instead.
* **"The channels are V-notches with no bed, so the river cannot widen."**
  Measured false. The terrain offers p50 24–37 m of submerged room against a law
  asking 1.5 m, and `fill_to_local_surface` already uses 75–82% of it. The first
  answer in this session said the opposite, from transects taken perpendicular to
  a hand-walked path built on the wrong surface; discharge along that path fell
  downstream on 13.6% of steps, which is impossible, and that is what caught it.
  Use `lateral_extent_stats`, which takes the perpendicular from each cell's own
  D8 receiver.
* **"`dump_stage_heightfields.py --stages` gives you the bake output."** Its
  `S1` is the **B3.relaxed** stage sink. B4 meso, B4b refill and B5
  reopen_basins all run after it and together move the channel **12.6 m**. Its
  docstring still calls S1 "= the bake", which was true when written. Worse, its
  `_coarse_fetch` returns elevation only and discards climate, so `--bake`
  silently produces **no water pass at all** — no runoff, no B6, `water_stats`
  simply absent rather than zero.
* **"Rivers die at tile seams."** 0.5–2.3% of pieces touched a tile edge against
  a 0.39% chance rate.
* **"A heightfield film is cheaper at distance."** It forfeits the mesher's
  banked 64:1 win at 23× the cascade's cost, and shows **0 pixels of vertical
  step** — the flat look already rejected.
* **"Rim instancing scales better than interior."** Inverted. The rim grows
  *faster* (R^1.46–2.80 against R^1.07–1.91). A river is a 1-D feature.
* **"Full-disc rebuild is the near-field bottleneck."** Ratio exactly 1.0. The
  real cause was **68.8% of offered bricks being underground**.
* **"Carrying real Q would coalesce the wet mask."** 1,954 → 2,014 pieces. It did
  not. (Q was still the right fix, for a different reason — see §3.)

---

## 14. Rules that have each cost hours

* **The bake is the authority on where water is.** If water is wrong, measure the
  bake before changing the client.
* **A live process is not a running editor.** This was reported off process state
  twice and was wrong twice. Confirm from **log progression**.
* **A blank capture is usually unloaded terrain**, not a rendering bug — and "no
  water in the shot" is usually the ±25.6 m / ±12.8 m reach (§6.1). Check whether
  water could have been in frame at all before diagnosing anything.
* **Confirm a capture settled, and the rule differs by altitude.**
  `jobsInFlight=0 pendingJobs=0 unloaded=0` always; plus
  `RefreshImplicitWater: DRAINED` near the ground, or the ribbon actor's
  `River ribbons: DRAINED build` at altitude. `RefreshImplicitWater: DRAINED`
  **cannot** appear in an altitude capture — the box is only 52×52×26 m — and
  reading its absence as a failed settle is wrong, but learning to ignore it lets
  a genuinely unsettled near-field frame through.
* **Pixel-diff every control against its pair and report the number.** A control
  once differed from its pair by 85% of pixels because exposure moved. And check
  the *signed* mean, not just the absolute one: a tonemap shift moves every pixel
  the same way, whereas render noise is symmetric. The tool now reports both.
* **A bake that does not write a loadable tile is not a bake.** `--diagnostic`
  write-protects the fine tier, so a run can look successful and produce nothing.
  Every capture for a stretch was taken against the narrowest river this project
  will ship, for exactly this reason.
* **Check the first tile before spending the rest.** A bake once wrote a tile
  with the water flag clear after 302 CPU-s and said nothing.
* **The owner judges screenshots, not you.** Deliver conditions and numbers, no
  verdict. Readings here have been wrong in both directions.
* **One UE editor per box.** Two capture agents once destroyed each other's
  frames for hours.
* **`ctest` needs `-C Release` on this generator**, or both suites report
  "Not Run" and look green.
* **Scan for junctions before any recursive delete.** `git worktree remove` once
  wiped the tile cache through one.
* **Comparing a ZSTD tile against a RAW tile invents diffs.** That produced a
  "696 control points differ by 1 mm" claim that was really **zero** — and
  `quant == 1` is the *code* for 100 mm, a 100× unit error. It reached a PR.
* **Bench targets carry `-Wall -Wextra -Werror` behind `if(NOT MSVC)`**, so this
  box warns about nothing and CI fails afterwards. Seven CI failures in three
  days from exactly this. Compile changed TUs with clang and those flags.
* **A stale prebuilt `voxelcore.lib`** produces link errors that look like
  missing code. Rebuild `build/voxel-core-msvc` first.
* **Live Coding blocks `Build.bat`** while the editor is open — not a build
  error. And **`Build.bat` exits 0 on `RulesError`**: gate on `Result: Succeeded`.
* **Zen Storage Server took 4,822 s cold** once and the editor never came up.
  Warm restart was 3.954 s. If it hangs, kill Zen and relaunch.
* **`vxc_riverribbonprobe --origin` is the region's low CORNER, not its centre.**
  Get it wrong and it samples unbaked ground and cheerfully reports a dry world
  — 0.09–0.13% wet instead of 0.560% — with no error, because as far as it knows
  there is simply no water there. Working command for the wet block:
  `--origin -40960 -40960 --region 16384`.
* **Reuse the bake's own instrument before writing a new one.** Three separate
  wrong answers in one session came from re-deriving something the bake already
  computes: flow direction, the perpendicular transect, and which surface the
  discharge was accumulated on.
