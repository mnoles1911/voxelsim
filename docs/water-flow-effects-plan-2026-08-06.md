# Making rivers look like they are flowing

> **STATUS 2026-08-09: [SUPERSEDED by `docs/water-rearchitecture-plan-2026-08-09.md`
> — kept for measurements/history].** This entire plan is about faking flow on
> the *baked, near-field-voxel-drawn* river (ripple direction from the water
> gradient, ribbon orientation). Under the re-architecture, near-field flowing
> water is real PBF particle motion, not a directional shading trick, so the
> problem this plan solves goes away at the source. The gradient-direction
> negative result (37.94% usable) and the D8-vs-gradient measurement remain
> valid data. See `docs/water-architecture.md`.

**What this is.** A plan for the effect the owner asked for — *"realistic, flowing type effects rather than static cubic water voxels that just sit statically — some sort of flowing, directional effect through the watershed"* — and an answer to the specific question he raised about the Minecraft mod **Dynamic Waters**: whether we should be pathfinding water uphill from the ocean rather than downhill from the divide.

**Written 2026-08-06.** Against `main`, `TERRAIN_VERSION` 8, `BAKE_VERSION` 14, `kWorldGenVersion` 23, `kWaterCAVersion` 5. Every number below either says which file or measurement record it came from, or says that it is an estimate. `docs/water-system-architecture.md` is the durable design document; this plan sits under it and changes none of its settled conclusions.

**The three answers, up front.**

| question | answer |
|---|---|
| What does the mod do? | It generates *river geometry* uphill from sea level because it has no world-scale watershed solve. Its *flow effect* is Minecraft's own: a direction derived at runtime from the fluid-level gradient. |
| Uphill or downhill for us? | **Downhill. Do not change it.** Uphill is a connectivity device for a system with no global solve. We have a global solve, and uphill cannot produce discharge, which every consumer we have reads. |
| How do we get a flowing effect? | Ship the flow direction the bake already computes and throws away, and drive the water material's *existing* panning ripple and vertex-offset terms with it. The first move is a probe run, not a bake. |

> **STEP 1 HAS BEEN RUN, AND OPTION A FAILED. Updated 2026-08-06, same day.**
>
> The pre-registered bar was a gradient-derived direction on **≥ 90%** of centreline cells. Measured on the wet alpine block: **37.94%** (154,419 of 407,042). It fails *worse* on the centreline (37.9%) than on interior wet cells generally (75.7%) — and the centreline is the one place flow direction matters. Full record: `docs/measurements/water-surface-gradient-2026-08-06.txt`.
>
> **What replaced it, and it is better than the thing that failed:** `riverRibbonOrient` in `voxel-core/include/voxelcore/riverribbon.h`. A reach is an *ordered polyline*, so its tangent is already a direction; the only missing piece was its sign, and that comes from comparing the surface height at the two **ends** of the reach. That reads `graded_water_surface`'s invariant directly instead of estimating a derivative from it — two integers, no quantisation failure mode, exact rather than heuristic, and reaches that are level are reported as flat rather than assigned a direction they do not have.
>
> **The gradient's MAGNITUDE survives** as a free speed proxy (p50 9.4 m/km where it resolves). It is only the *direction* that is dead.
>
> One method correction, made before measuring: the plan below specifies a ±1-**brick** stencil (1.6 m), which is **sub-pixel** against an 1875 mm water plane — both taps land on the same stored control point and read exactly zero. That would have looked catastrophic and been an artefact. The measurement used ±1 **fine pixel** (3.75 m).

---

## 1. What Dynamic Waters actually does

Separated into what its own authors state, what is inferable from Minecraft's engine, and what is forum-level.

### 1.1 Verifiable — the mod's own description

From the project pages on [Modrinth](https://modrinth.com/project/wL4lvEci) and [CurseForge](https://www.curseforge.com/minecraft/mc-mods/dynamic-waters) (172k+ downloads, actively updated to 11.1.2 on 2026-07-16, with a 12.0 test build 2026-08-01):

* **Uphill generation, in their own words.** Mountain streams "are generated using an advanced algorithm that works backwards. They begin at parent river junctions (at sea level, Y=62) and intelligently pathfind their way *uphill* by sampling the terrain's heightmap." They call it a **drain-to-source** algorithm. The stated payoff is shape: "flat water terraces on gentle slopes" and "dramatic vertical waterfalls on steep cliffs."
* **The main network is not uphill.** Main rivers are a *fractal* network that "dynamically split into 1st, 2nd, and 3rd degree branches depending on their width." Only the small mountain streams are traced uphill. This distinction is easy to lose and it matters: the uphill pass is a *tributary attachment* step, not the river model.
* **Deltas.** Large rivers approaching the ocean "split into multiple distributary channels."
* **Generation architecture.** "A completely custom, heavily optimized region-based generation system"; river layouts are computed "asynchronously on a dedicated thread pool during the earliest stages of world generation (the NOISE stage)" and cached, so "by the time the game actually needs to carve the terrain, the river data is already calculated." A "strict regional grid system" prevents chunk cascading.
* **The flow effect is configurable and separable from generation.** There is an option to **disable water flow animations** (stated as compatibility with fluid-physics mods), a per-entity flow strength where "entities set to 0.0 are no longer pushed or affected by water flow," a configurable **ambient river sound**, and config for **river particle** frequency.
* **Changelog evidence about what the flow actually is.** Version 9.0.1 fixed "squids no longer move backwards or glitch in rivers"; later versions fixed water from a bucket "not flowing correctly" in ocean biomes, mountain rivers "losing their water flow after a server restart — the flow state is now properly saved," and flow animations misbehaving in modded biomes whose names contain "Sea" or "Deep".
* **Limitations they state.** Requires a new world. Conflicts with any mod that "restores, replaces, or heavily modifies vanilla-style rivers."
* **It is closed source.** No public repository was found. Everything above is the authors' prose, not code.

### 1.2 Verifiable — the engine underneath

The mod describes itself as providing "Streams-style rivers." **Streams** (delvr) is the open ancestor and is [on GitHub](https://github.com/delvr/Streams) (Scala, requires the Farseek library, **archived 2026-04-26**, superseded by Farseek-Mods). Its README states: rivers are made of **custom non-decaying flowing blocks**, they "originate in multiple sources and flow down the terrain through slopes and waterfalls, joining together into wider rivers until they reach the sea," and they carry "a true current." The author's own note is worth quoting: *"most of it will be replaced as part of an upcoming major rewrite."*

The mechanism both mods sit on is vanilla. Minecraft's `FlowingFluid` exposes `getFlow(BlockGetter, BlockPos, FluidState) → Vec3` ([Forge javadoc](https://nekoyue.github.io/ForgeJavaDocs-NG/javadoc/1.19.3/net/minecraft/world/level/material/FlowingFluid.html)). **The flow vector is not stored. It is derived, every time it is asked for, from the fluid levels of the neighbouring blocks.** One vector then feeds three consumers at once: the rotation of the animated water texture, the push applied to entities, and the mod's particles and sound.

That is the single most transferable idea in the whole investigation, and §4 is built on it.

### 1.3 Not verified — say so

* **How Dynamic Waters renders motion.** Neither project page describes it. It is *consistent with* the changelog ("flow state is now properly saved", vanilla-style flow-direction bugs in named biomes) that it places vanilla flowing fluid states and lets vanilla derive the vector — but no source or documentation confirms it. **Treat as inference, not fact.**
* **Any performance number.** "No lag," "lightning fast," "minimally slower chunk generation" are the only claims. There is not one measurement, on either page, in any changelog, or in any review found. **We have no cost data on this mod at all.**
* **Whether the uphill pass improves anything measurable.** The claimed benefits (terraces, waterfalls, better terrain adaptation) are aesthetic and are asserted, never measured.
* **Where it breaks down.** No public defect list beyond the changelog. The visible failure classes are compatibility (rivers, biomes, structures) and the "requires a new world" rule, which is the same class of problem our `TERRAIN_VERSION` exists to manage.

**Forum hearsay, excluded:** claims about relative performance against Streams, about whether it "actually simulates" water, and about entity-push tuning. None reproduced from a primary source; none used below.

---

## 2. Uphill or downhill? Downhill. Do not change it.

**The short version.** Uphill-from-the-ocean is what you do when you cannot afford to solve the whole watershed. Its product is a *connected path*. Our product is *discharge*, and you cannot get discharge by walking upstream.

### 2.1 What uphill buys them

Dynamic Waters generates a Minecraft world region by region, on a thread pool, during the noise stage, with an explicit rule against chunk cascading — that is, a region is not allowed to depend on its neighbours' generation. Under that constraint a downhill trace is unusable: to know where a stream goes you would have to generate the terrain it flows into, which is the cascade the architecture forbids.

Starting at a known outlet at sea level and walking uphill inverts the dependency. The path is guaranteed to reach the sea because it *started* there. It terminates when the heightmap stops rising. It costs O(path length) samples of a heightmap function that can be evaluated anywhere, cheaply, without generating anything. **Uphill is a connectivity and termination guarantee bought without a global view.** It is a good answer to their problem.

### 2.2 Why it is the wrong answer to ours

Five reasons, in order of how hard they bite.

**1. Uphill cannot produce discharge, and discharge is the only currency our consumers read.** Q is a sum over everything upslope of a cell (`bake/water.py`, §3 of the architecture doc). Walking upstream from an outlet, you cannot know a cell's Q until you have visited its entire upslope subtree — at which point you have done the downhill sweep, in a worse order. Every consumer downstream of routing reads Q: the width law, the depth law, `q_drawable_m3_yr`, `water_head_mask`. An uphill pass hands us a path with no number attached to it.

**2. We already paid for the global solve, and it was the change that made rivers real.** Carrying real Q up the drainage pyramid (`bake_ver` 10) moved the discharge reaching the coast from 1.27e6 to **2.50e7 m³/yr — 2.2% to 40% of the whole-world model** (architecture §3). We have the world-scale view their architecture forbids. Adopting a technique whose purpose is to avoid needing it is going backwards.

**3. The connectivity guarantee we would be buying, we already have.** `fill_depressions` uses the epsilon variant of priority-flood (`bake/flow.py`), so **every cell except a domain-border cell has a strictly lower neighbour** and every drop of water reaches the border. Termination at the sea is structural, not searched for. The measured cost of *not* having this was severe — a plain fill stranded **69% of the domain's area** and left 341,368 inland dead-ends (`flow.py` module docstring) — which is exactly why the epsilon fill is there.

**4. Changing the routing rolls the world.** The water pass and the incision share one D8 receiver forest — `pipeline.py` says so at the water sweep: *"It is also B2b's own forest, so the discharge follows the same centrelines the incision's slope term was taken along."* `profile_incision` solves the graded long profile along that tree. Change how water is routed and you change the carve, which changes elevation bytes, which rolls `TERRAIN_VERSION`, which invalidates **every measurement, screenshot, vista site and spawn coordinate anyone holds** (architecture §7) and costs ~24 CPU-hours to re-bake the world (§10). For a *rendering* complaint. That trade is not close.

**5. The settled-decision rule applies.** Single-receiver routing is in §12a with a measured reason: `_accumulate_mfd` evaluates its weights in the surface's own dtype, and on a nearly-flat epsilon-filled surface (slope ~2.6e-4 in float32) the weights **underflow to zero at about p = 11** — every weight zero, the cell reads as a pit, its whole accumulated discharge is dropped, on exactly the flat near-coast ground rivers must cross. At p = 32 more than half the world's water budget is lost; D8 loses under 1e-9. Pinned by `test_accumulate_d8_beats_high_p_underflow`. **Nothing in this plan re-opens that.**

### 2.3 The asymmetry the owner suspected is real, and it is the whole story

| | Dynamic Waters | us |
|---|---|---|
| terrain | a runtime heightmap function, evaluable at any coordinate for the price of a call | a **baked pyramid**; the fine tier exists only where a tile has been baked |
| generation scope | one region, forbidden to depend on neighbours | one world, with discharge carried across tile seams up a pyramid |
| what the router produces | a connected path | **Q in m³/yr per cell** |
| what an uphill walk costs | O(path), no dependencies | leaving the tile means sampling ground that does not exist |

An uphill walk in our world hits an unbaked tile and has nothing to read. Their uphill walk never can. **Their asymmetry is the reason the technique works for them and the reason it does not transfer.**

### 2.4 The one idea from uphill worth keeping, and it is not a routing change

Their uphill pass produces a network that is **connected by construction** rather than by clearing a threshold. Our drawn network is not: on the wet alpine block the wet mask is **467 connected pieces** (architecture §11). That fragmentation is a *threshold* artefact, not a routing artefact — `q_drawable` cuts, and a reach that dips under the cut and comes back breaks in two.

The transferable version costs no re-route at all: **walk *up* the existing D8 receiver forest from every coastal outlet, taking the highest-Q parent at each junction, and mark the trunk.** That is one topological sweep over a forest the bake already has, in the direction it already stores (`rec[c]` points downstream; the inverse walk is a gather). It gives an ordered, connected, sea-anchored trunk set — which is exactly the input a flowing effect wants, and exactly what `riverribbon.h` is already half-doing when it traces reaches. It is listed as an optional step in §6 and it does not touch the ground.

**What this section is not saying.** It is not saying uphill is a bad idea in general, and it is not saying the mod is wrong. It is saying uphill solves a problem we solved differently and more expensively, and adopting it would cost us the solve we paid for.

---

## 3. The envelope any proposal has to live inside

These are measured and each one has killed a proposal before.

| constraint | number | source |
|---|---|---|
| Near-field real water exists only here | **±25.6 m horizontally, ±12.8 m vertically**; 65×65×33 bricks = 52×52×26 m | architecture §6.1, `VoxelWaterSubsystem.cpp` |
| The mesher is already at its theoretical optimum | `meshBrick<8>` hits **64:1** on flat water tops | architecture §6.1, §12a |
| Mesh budget headroom | drain **11,520 bricks/s**, demand **10,627/s** → **0.92×**, i.e. **~893 bricks/s spare** | `docs/near-water-refresh-findings.md` §1, `docs/measurements/water-refresh-2026-08-05.txt` |
| Every CA-active brick re-meshes at 10 Hz | stated as water's hot path | `VoxelGpuPoolComponent.h:357` |
| The bake decides where water is; the client only draws it | no runtime "find the rivers" pass | architecture §0, §12a |
| Flat quads standing in for rivers were rejected | *"I want to physically change the geometry"* | architecture §0 |
| A water-only re-bake re-keys 100% of tile bytes | **~13 GB re-download** today; **~43 MB** after per-section content addressing | architecture §7, `docs/tile-slicing-2026-08-04.md` |

**Four facts about the plumbing that decide everything in §4. Each was read out of the code for this plan.**

1. **The near-field mesher already computes the water-surface gradient and throws it into a normal.** `VoxelWaterChunkComponent.cpp:184-191` computes `dHdU` and `dHdV` from the quad's four corner heights and builds `FVector3f(-dHdU, -dHdV, 1)`. A flow direction is `-(dHdU, dHdV)` normalised, and it is *already in a register*.
2. **There is exactly one free per-brick channel, and it is documented as free.** Water publishes `ChunkParams` as `FVector4f(0.5f, Activity, kNoSurfaceGate, 0.0f)` (`VoxelWaterSubsystem.cpp:1942`), and the pool header describes the layout as "climate in xy, surface height in z, **one spare in w**" (`VoxelGpuPoolComponent.h:231`). Foam activity already rides `.y` all the way to the material. **`.w` is a free float per water brick (0.8 m), on a path that is already built, tested and published.**
3. **There is no free per-vertex channel.** Vertex colour is fully allocated on water: R = per-corner surface height, G = AO, B = top-boundary flag, A = foam activity (`VoxelWaterChunkComponent.cpp:217-243`). The per-quad side channel `QuadCornerHeights` is a `uint32` holding four 8-bit heights and is also full (`VoxelGpuPoolComponent.h:235-239`).
4. **The packed GPU quad has five unused bits.** `word0 = axis | positive<<4 | slice<<8 | u0<<16 | v0<<24` (`VoxelQuadDecode.ush:16`), where `axis` takes values 0–2 and `positive` 0–1 in nibbles (`VoxelMeshTypes.h:44-45, 77-78`). A 3-bit D8 direction fits with two bits left over and **no change to the 8-byte quad**. The catch is in §4.

**And one hard rule from the material, which is the trap in this whole area.** `create_water_voxel_material.py` already contains two motion terms: a two-scale panning ripple on **Normal** (pixel-shader only, keyed on the world-planar UV wrapped to a 32 m period) and a continuous vertical bob on **World Position Offset**, keyed on **absolute world XY**. The file states why the keys differ, and it is load-bearing:

> "this is a POSITION output, and it MUST agree bit-for-bit between two adjacent bricks at a vertex they share, or the bricks tear apart into a visible crack every time the ripple phase moves."

**Therefore: a per-brick constant may drive the Normal safely, and may not drive WPO.** Any WPO term must be a function of absolute world position that both bricks sharing a vertex evaluate identically. This kills the naive version of the most attractive option and §4 is shaped around it.

---

## 4. The options, ranked

Ranked by effect delivered per unit of risk. Every one of them is a *renderer* change except B, which is the only one that touches the bake.

### Option A — derive direction from the water-surface gradient, drive the ripple that already exists

**The effect.** The surface texture of a river visibly travels downstream, faster on steep reaches, and a lake stands still.

**How.** In `RefreshImplicitWater`, sample the water datum at the brick's corners over a ±1-brick stencil, take the gradient, pack `atan2` and magnitude into the free `ChunkParams.w`, and use it in the material to steer the existing panning ripple's direction and rate instead of its current fixed pan. This is vanilla Minecraft's trick — a direction derived from the level gradient — applied to a field we already reconstruct.

**Cost.** One float per water brick on a published path. No new wire bytes, no bake change, no `BAKE_VERSION` bump, **no re-download**. No change to greedy merging: the value is constant across the brick, so `meshBrick<8>`'s 64:1 is untouched. Material-graph work plus a small mesher change.

**Why it is only *ranked* first and not *recommended* outright — the risk that decides the whole plan.** The stored depth plane is int16 at a 10 mm LSB (`tile_codec.py:225`). On genuine along-flow gradients that resolves fine: at the world's p50 of **47.1 m/km** over a 1.6 m stencil the drop is 75 mm = **7.5 LSB**, and the wet block's long profile runs **173 → 29 m/km** (architecture §11a F3), so the worst genuine case is still ~4.6 LSB. **But the same measurement note records that 58% of river cells' "gradient" is the epsilon-fill floor and is not terrain at all** (`tile_codec.py:199-201`). On those cells the drop is ~2 ULP of the surface's magnitude, quantises to zero, and the derived direction is undefined. Flat reaches are also where a real river's motion is *most* visible.

**So the honest statement is: gradient magnitude is a good speed proxy and is free; gradient direction may be degenerate on the majority of wet cells.** §6 makes measuring that fraction the first action, because it decides whether we can avoid Option B entirely.

**What A cannot do.** It steers the *Normal*, which is a shading cue. Per §3's hard rule it may **not** be used to steer WPO without tearing the mesh. So A on its own is closer to the "lighting gimmick" the owner rejected than to "physically change the geometry" — it is the cheap half. A′ below is the other half.

### Option A′ — a directional travelling wave on WPO, keyed on absolute world position

**The effect.** The water *surface geometry* travels downstream — real vertex displacement, not shading.

**How.** Keep the existing absolute-world-XY key (so seams stay exact) and make the phase advance along the flow direction. The flow direction must therefore also be a function of absolute world position that both bricks agree on. Two ways:
* **A′-cheap:** advance the phase along a *smoothed, reach-scale* direction that varies slowly enough to be sampled per vertex from world position without a per-vertex channel — e.g. a low-frequency direction field reconstructed from the same datum in the material. Risk: any per-brick quantisation of that field reintroduces the tear.
* **A′-correct:** add a per-quad `QuadFlow` array parallel to `QuadCornerHeights` — the pattern already exists and already has an `AddChunk`/`UpdateChunk` overload shape. Cost is **+4 bytes per water quad on top of the 8-byte packed quad, a 50% growth of the water quad buffer** (buckets are 4 MB each, typically 4–8 live, capped at 12 — architecture §6.5), so up to ~+2 MB per live bucket. And it does **not** fit in the packed quad's five spare bits after all, because quads are greedily merged: a merged quad spans many cells with different directions, so putting direction in the quad forces the merge to break on direction change, which is a direct tax on the banked 64:1 win. **Do not put flow in the packed quad.**

**Recommendation within A′:** try A′-cheap only after A has proved the direction field is usable at all; treat A′-correct as a costed fallback with the quad-buffer growth stated up front.

### Option B — ship the flow direction the bake already computes and deletes

**The effect.** A correct direction everywhere the bake has one, including the flat reaches where A is degenerate.

**How.** `pipeline.py:4729` computes `rec_w, _ = kernels.d8_receivers(z_route, geom.fine_pixel_m)` — the D8 receiver on the water routing surface, which *is* the flow direction — and `pipeline.py:4784` does `del rec_w`. The direction exists, at fine resolution, and is discarded. Encode it as 3 bits per wet pixel in a new tile section (the existing flow byte has **all eight bits allocated**: bits 0–4 log2 accumulation, bit 5 channel, bit 6 bank, bit 7 deposition — `tile_codec.py:279-284` — so there is no room to borrow).

**Cost, with the right comparison.** The measured comparable is the flow plane at 8192²: **5.745 MB compressed on alpine tile (-5,2)**, of which bits 0–4 alone are 5.015 MB and bits 5–7 alone are **0.920 MB**; gated, the shipped plane is 1.518 MB (`pipeline.FLOW_PLANE_SIZE`). A direction plane written only where the water plane is wet covers **0.560% of pixels** (architecture §11), so it should compress well below the flags-only 0.920 MB. **Estimate 0.1–0.5 MB per tile — and measure it before believing it, because this exact spec was out by 1000× once already** (the format predicted "~5–10 KB" for the flow plane).

**The cost that actually decides it.** This is a product-only change: `BAKE_VERSION` 14 → 15, ground bit-identical, provable by `tools/verify_water_only_change.py`. But a water-only re-bake changes 0.15–0.62% of a tile's bytes and **re-keys 100% of them** — **~13 GB of re-download across 289 tiles** (architecture §7). After per-section content addressing lands (`docs/tile-slicing-2026-08-04.md`, decided 2026-08-04, nothing built) the same update is ~150 KB per tile, ~43 MB for the world.

**Therefore: B is the right answer and the wrong time.** Do not bump `BAKE_VERSION` for a flow direction alone. Bundle it with tile slicing, or with the next water bake that has to happen anyway.

### Option C — orient the far-field ribbon and scroll it

**The effect.** The river keeps flowing past 25.6 m instead of freezing at the near-field boundary.

**How, and it is nearly free.** `riverribbon.h:126-127` says: *"One reach, ordered head to foot along the channel (or foot to head — the direction is not meaningful here and nothing downstream depends on it; a ribbon is symmetric)."* Each `RiverRibbonPoint` carries its own `surfaceMm` (`riverribbon.h:120-124`), and `graded_water_surface` guarantees **the water surface never rises going downstream** (architecture §5). So orienting a reach is one comparison of the surface height at its two ends, and it is *exact*, not heuristic. Then scroll the ribbon's UV along its own arclength.

**Cost.** A few lines in `voxel-core`, unit-testable without an engine — which is the established split for this file (architecture §6.3) — plus one material parameter. No new data, no bake change.

**The honest caveat.** A ribbon is a flat quad, and scrolling a texture on a flat quad is precisely the class the owner rejected. **C is not a substitute for A/A′; it is the thing that stops A/A′ from creating a visible seam** where a river flows inside 25.6 m and stops flowing outside it. If A ships, C ships with it, or we have manufactured a motion seam on top of the tone seam that architecture §12 already records as known-but-unmeasured.

### Option D — actually move the water. **Not proposed.**

Priced so nobody re-derives it. Making baked water move means promoting it from a datum to CA-simulated water, and every CA-active brick re-meshes at 10 Hz (`VoxelGpuPoolComponent.h:357`). Spare mesh capacity is **~893 bricks/s** (11,520 drain − 10,627 demand, measured). So this is affordable only if fewer than about **89 water bricks** are resident in the near field. A 52×52 m box with a river of the world's mean channel width (7.52 m, architecture §11) through it holds roughly 390 m² of water surface, which at 0.8 m bricks is **~610 surface bricks** — *this is a geometric estimate, not a measurement* — giving ~6,100 bricks/s, about **7× over budget**.

And the budget is the smaller problem. Architecture §6.6 is explicit: baked water is a datum, and the two measured consequences are that **damming a baked river does nothing** (a wall settles in 1 tick, the upstream level does not rise by one voxel, because every untouched part of the river is a wall to the simulation) and **draining one runs away** (a single 4-voxel shaft converts **100% of the reach** to simulated water, because the mobilisation front follows moving water and has no length bound). Turning the whole river into simulated water is that runaway, deliberately, everywhere.

**"Make it flow" is not a rendering change if taken literally. It is a decision to simulate every river in view, and the CA has a documented runaway on exactly that case.** Not proposed.

### The ranking, on one line each

| # | option | effect | wire cost | mesh cost | re-download | risk |
|---|---|---|---|---|---|---|
| 1 | **A** — gradient → `ChunkParams.w` → existing ripple | shading motion, directional | **0** | **0** | **0** | direction may be degenerate on ~58% of cells |
| 2 | **C** — orient + scroll the ribbon | motion survives past 25.6 m | 0 | 0 | 0 | it is a flat quad, and that was rejected |
| 3 | **A′** — directional travelling wave on WPO | **real vertex displacement** | 0 or +4 B/quad | 0 (per-brick) or merge tax | 0 | **mesh tearing** unless seam-exact |
| 4 | **B** — ship the bake's D8 direction | correct direction everywhere | est. 0.1–0.5 MB/tile | 0 | **13 GB today** | wait for tile slicing |
| — | **D** — simulate the river | true motion | — | **~7× over budget** | — | CA runaway; **not proposed** |

---

## 5. How any of this interacts with `waterca.h`

This is the part that is easiest to get wrong and most obvious when it is wrong.

**The rule: a flow effect is a property of the DATUM path only.** The moment a brick is mobilised into the CA, its water is a real simulation and its apparent motion must come from the simulation, not from the baked gradient. Otherwise a player who dams a river watches the dammed pool continue to scroll downstream — which reads worse than water that never scrolled at all, because it advertises that the motion is a lie.

**The gate is already built and already published.** `ChunkParams.y` carries per-brick foam activity, and it is non-zero exactly while `vxc::WaterCA` is still working on that brick (`create_water_voxel_material.py`, "SIGNAL 2, ACTIVITY. VertexColor.A, 1 while vxc::WaterCA still calls the brick active"). So:

* `activity == 0` → baked datum → use the flow direction (A, A′, or B).
* `activity > 0` → CA-simulated → suppress the baked flow scroll. Whether to substitute the CA's own net flow is a later question; suppressing is correct and costs nothing.

Note that activity reaches the material by **two different mechanisms** — vertex colour A on the CPU path and `ChunkParams.y` on the pooled path — and the code already flags this as "the channel to check first if the two paths ever foam differently" (`VoxelWaterChunkComponent.cpp:225-231`). A flow gate riding the same signal inherits the same hazard. **Check both paths, or the gate will work in one renderer and not the other.**

**One thing this does *not* fix, and should not pretend to.** None of A, A′, B or C makes damming a river work. Baked water remains a datum. A flowing *appearance* over a non-flowing *model* is a deliberate, bounded lie, and it is the same lie every river in every game of this kind tells. It should be written down as such rather than discovered later.

---

## 6. What to do first, and the test that would tell us it worked

**Do not write a material node until step 1 has a number.** The entire ranking above turns on one unmeasured quantity: how often the water-surface gradient has a usable direction. If it is usable, Option A is free and we are done for the price of a material edit. If it is not, Option B is the only correct answer and we should not spend a day discovering that in a shader.

### Step 1 — the probe. Measure the gradient field before building anything on it.

Extend `vxc_riverribbonprobe` (architecture §9 calls it "the one that answers most questions") to report, over a window of wet cells:

1. the fraction of wet cells where `|∇(water surface)|` over a ±1-brick (1.6 m) stencil exceeds the 10 mm LSB — i.e. where a direction exists at all;
2. the angular agreement between that gradient direction and the bake's own D8 receiver, dumped once from a diagnostic bake as ground truth;
3. the same two numbers split by lake cells and river cells, because standing water should read exactly zero and that is a *feature*, not a failure.

Run it on the wet alpine block, at the command the architecture doc pins:

```
.\vxc_riverribbonprobe.exe "D:/vox-wet-cache/...-b10cf6d2c/000000000135276f/s16" ^
  --origin -40960 -40960 --region 16384
```

`--origin` is the region's **low corner**, not its centre — getting it wrong reports a dry world with no error (architecture §14).

### The falsifiable acceptance test

> **On wet cells that lie on a river centreline, the gradient-derived direction agrees with the bake's D8 receiver to within 45° (one D8 step) on ≥ 90% of cells; and on registered-basin cells the gradient magnitude is exactly zero.**

* **Pass** → Option A is viable. Proceed to step 2. We get a directional river with zero wire bytes and zero re-download.
* **Fail** → Option A is dead as a *direction* source (keep the magnitude as a speed proxy, it is still free). Option B becomes the plan, and it waits for tile slicing. **We will have learned this for the price of one probe run instead of a bake, a `BAKE_VERSION` bump and a 13 GB re-download.**

Either outcome is worth having, which is what makes this the right first step. The 90% figure is a judgement, not a derivation — set it before running, so it cannot be moved afterwards.

### Step 2 (only on a pass) — the smallest visible thing

Pack direction and speed into `ChunkParams.w`, steer the existing panning ripple with it, and go to the pinned playtest site: `-VoxelSpawnAt=-64019,-69172`, water surface 1,728.5 m, on a 3,865 m reach (architecture §10). **Confirm the capture settled** — `jobsInFlight=0 pendingJobs=0 unloaded=0` plus `RefreshImplicitWater: DRAINED` near the ground (architecture §14).

**The owner judges the screenshot. Deliver conditions and numbers, no verdict** (architecture §14).

### Step 3 — Option C, in the same change or immediately after

Orient the ribbon by end-point surface height and scroll it, so the motion does not stop dead at 25.6 m. Cheap, unit-testable in `voxel-core`, and it is the difference between "the river flows" and "the river flows in a 26 m bubble."

### Optional, and independent of all of the above

The uphill trunk walk from §2.4 — gather up the existing D8 forest from coastal outlets, take the highest-Q parent, mark the trunk. One sweep, no re-route, no ground change. It is the one genuinely good idea in the mod's uphill pass, and it is available to us without any of the architecture that made them need it.

---

## 7. What is explicitly not being proposed, and why

* **No change to routing direction.** Uphill cannot produce discharge, our consumers all read discharge, and the routing forest is shared with the incision so changing it rolls `TERRAIN_VERSION`. §2.
* **No re-litigation of D8 vs MFD.** Settled with a measured reason (float32 weight underflow at p ≈ 11 losing more than half the world's water budget on exactly the flat coastal ground rivers cross), pinned by test. Architecture §3, §12a. **Noted for completeness and not acted on:** `docs/measurements/d8-direction-lock-2026-07-31.txt` establishes that D8 exposure causes a real 45° direction lock in the *terrain* at 50–112× the noise floor, and that the fix is D-infinity on the carve. That is a terrain finding with its own plan; it is not a water-rendering matter and this document does not touch it.
* **No simulation of baked rivers.** ~7× over the mesh budget by estimate, and it is the CA's documented runaway case. §4 Option D.
* **No `BAKE_VERSION` bump for flow direction alone.** 13 GB of re-download for a renderer feature. Bundle it with tile slicing or with a bake that has to happen anyway. §4 Option B.
* **No flow data in the packed GPU quad.** Five bits are free, but quads are greedily merged and a merged quad spans cells with different directions, so it would force merge breaks and tax the banked 64:1 win. §4 Option A′.
* **No widening of the near-field box to show more moving water.** Demand is already at 0.92× of drain; a larger window is over. Architecture §12a.
* **No new source-point or head model.** `water_head_mask` is correct and is reused unchanged. Architecture §11b.

---

## 8. What could not be verified

Stated plainly so it is not mistaken for established.

1. **How Dynamic Waters renders motion.** Closed source; not described on either project page. That it uses vanilla's derived flow vector is an *inference* from its changelog and from its self-description as "Streams-style," not a fact.
2. **Any cost figure for the mod.** There are none, anywhere. "No lag" is the entirety of the performance evidence.
3. **Whether their uphill pass measurably improves anything.** All claimed benefits are aesthetic and asserted.
4. **The compressed size of a direction plane in our tiles.** Estimated 0.1–0.5 MB/tile from the flow plane's measured flags-only 0.920 MB scaled by the 0.560% wet fraction. **Unmeasured.** The same class of estimate was wrong by 1000× once in this format's own spec.
5. **The count of resident water bricks in the near field.** Option D's ~610 is derived from box geometry and the mean channel width, not measured. `vxc_waterrefreshprobe` reports throughput, not residency; a residency count needs a small addition to it.
6. **Whether the ±1-brick gradient stencil is the right one.** Chosen because 1.6 m against the world's p50 along-flow gradient gives 7.5 LSB of signal. Larger stencils buy signal and lose spatial detail. Step 1 should sweep the stencil width rather than assume 1.
7. **Whether a smoothed direction field can drive WPO without tearing (A′-cheap).** This is a claim about seam-exactness across brick boundaries and it is untested. Treat A′-cheap as a hypothesis until a `-VoxelWindingCheck`-style geometric proof exists for it, the way winding got one.

---

## Sources

Mod research:
- [Dynamic Waters: Realistic Flowing Rivers — Modrinth](https://modrinth.com/project/wL4lvEci)
- [Dynamic Waters: Realistic Flowing Rivers — CurseForge](https://www.curseforge.com/minecraft/mc-mods/dynamic-waters)
- [Dynamic Waters 9.0.1 changelog — Modrinth](https://modrinth.com/mod/dynamic-waters-realistic-flowing-rivers/version/9.0.1)
- [Dynamic Waters files list — CurseForge](https://www.curseforge.com/minecraft/mc-mods/dynamic-waters/files/all)
- [delvr/Streams — GitHub (archived 2026-04-26)](https://github.com/delvr/Streams)
- [Streams — CurseForge](https://www.curseforge.com/minecraft/mc-mods/streams)
- [`FlowingFluid` javadoc (Forge 1.19.3)](https://nekoyue.github.io/ForgeJavaDocs-NG/javadoc/1.19.3/net/minecraft/world/level/material/FlowingFluid.html)

In-repo, every number above: `docs/water-system-architecture.md` (§0, §3, §5, §6.1, §6.3, §6.5, §6.6, §7, §9, §10, §11, §11a, §11b, §12a, §14), `docs/near-water-refresh-findings.md`, `docs/measurements/water-refresh-2026-08-05.txt`, `docs/measurements/d8-direction-lock-2026-07-31.txt`, `docs/tile-slicing-2026-08-04.md`.
