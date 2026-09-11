# Temperate slice performance plan

Written 2026-09-10. Target: **p95 frame time under 10 ms while moving at 20 m/s** (the Goal 3
bar from `docs/50k-budget-2026-08-23.md`). This plan is about the vertical slice as it actually
runs today, with voxel terrain plus roughly 123,000 instanced understory plants.

Every number below comes from captures already on disk, named at the point of use. Where I could
not attribute something, the plan says so instead of guessing, and the first phase exists to fix
exactly that.

## Where we actually are

| configuration | frames | frame p50 | frame p95 | frame p99 | game thread p50 | GPU p50 | render p50 |
|---|---|---|---|---|---|---|---|
| 48 m ring, default (route 10) | 6,028 | 22.88 | 46.48 | 350.90 | 22.83 | 13.16 | 4.00 |
| 256 m ring, size cull ON (walk 27) | 784 | 22.72 | 54.51 | 309.56 | 22.66 | 19.54 | 5.15 |
| 256 m ring, size cull OFF (walk 26) | 487 | 42.36 | 127.72 | 348.88 | 23.12 | 40.74 | 42.11 |

All milliseconds. Captures under `asset-forge/out/ecological-placement/`, each with a passing
fail-closed receipt.

**Not one frame in any of these captures reaches 100 FPS, and not one reaches 60 FPS.** Every
frame in all three captures exceeds 16.7 ms. At the default ring, 7.0% of frames exceed 33.3 ms;
with the 256 m ring unculled, 98.2% do.

Three facts shape everything that follows.

**1. Understory draw cost is a ring-area problem, and size culling already solves it.** Going from
the 256 m experiment ring to size culling on drops GPU from 40.74 to 19.54 ms and the render
thread from 42.11 to 5.15 ms. That is the largest single measured lever anywhere in this system,
and it is opt-in today.

**2. Once culled, ring radius stops mattering.** The 256 m culled frame p50 is 22.72 ms and the
48 m default is 22.88 ms. Both land on the same floor.

**3. That floor is the game thread, and it is not the understory.** At the default ring the
understory's entire game-thread cost at median is about 0.02 ms: detail tick 0.010, dispatch
0.005, unload 0.002, HISM rebuild 0.001, drain 0.000. The floor is 22.83 ms of something else.

**4. Added 2026-09-11 — that "something else" is 39% water simulation, at a site with no water.**
Twenty-four of the module's tick functions had no instrument at all, which is why fact 3 could only
say "something else". They have one now. `OceanTick` reads 4.16 ms and `RippleTick` 4.10 ms, at a
forest column the engine's own log puts at 70.8 m of ground over a 0.0 m sea level. A
74,384-triangle ocean mesh is followed by transform and asked "is the camera underwater?" every
frame, and a 512×512 ripple wave field is stepped at a fixed 60 Hz every frame, 75.8 m above the
nearest sea. `voxel.Water.Ripple.Enable` defaults to true with no config override, so this ships
that way. Neither actor has any gate on water being within reach. Both read the same 4.1 ms
standing still as walking — a flat tax that does not care what the player is doing.

Read together, facts 1 to 4 say the whole thing in one line each:

| configuration | frame p50 | fps | GPU p50 | game thread p50 |
|---|---|---|---|---|
| 48 m ring, ships today | 21.1 | 47 | 12.7 | 21.1 |
| 256 m ring | 41.7 | 24 | 40.5 | 21.8 |
| **256 m ring + size cull** | **22.7** | **44** | **19.5** | 22.7 |

**Size culling buys a 5.3× larger ring for nothing.** And at both the shipping ring and the culled
256 m ring the binding constraint is the same 21–23 ms game thread, of which 8.3 ms is water
simulation on dry land.


## Ordered work queue as of 2026-09-11

The measurement runs below all use the current binary and the current detail
cache. **Nothing in phase 2 may start until phase 1 finishes**, because
`VoxelDetailAssetSubsystem.cpp` and `M_VoxelDetailAsset` are both part of the
detail cache identity: editing either refuses every existing cache, and a
capture that starts after such an edit fails rather than silently measuring the
wrong geometry.

### Phase 1 — measurement, current binary, current cache

1. **Size-cull screenshots.** Two route captures at the 256 m ring, size culling
   off and on, on the *detour* route. (The survey route aborts at its last
   waypoint — 6 of 7, 130.85 m of 135.47 — and does so at the 48 m ring too, so
   it is a bad route, not a ring problem. That is why the detour exists.)
   Produces 8 pose-matched checkpoint pairs plus a second timing confirmation.
2. **Water sub-scopes.** One walk capture reading `OceanFollowMs` /
   `OceanUnderwaterMs` and `RippleAutoWatchMs` / `RippleStepMs` /
   `RipplePublishMs` / `RippleHealthMs`, to split the 8.26 ms in two.
3. **Ripple falsifier.** One walk arm at
   `-dpcvars=voxel.Water.Ripple.Enable=0`. If ~4 ms does not come off the game
   thread, the scope is measuring something other than its name and the finding
   in fact 4 is wrong.
4. **Marcher resolution ladder.** Five walk arms at screen percentage
   100/80/65/45/33. Settles whether the marcher has a real fixed term — a
   two-point fit says 3.63 ms and the identical arithmetic was retracted once
   already.
5. **Height pyramid gates at this site.** `tools/voxel-heightpyramid-gates.ps1`,
   zero code. Does the 21.64% engagement transfer off the 2026-08-27 site, and
   does the 479-ray hole reproduce here? A null on the first kills the top
   marcher lever for the cost of two legs.

### Phase 2 — the re-bake cycle, batched because the toll is paid once

Both changes invalidate every detail cache, so they go together.

1. `create_detail_asset_material.py` already sets `used_with_nanite`; regenerate
   the asset headless with `-run=pythonscript`.
2. Add an opt-in flag to the understory component setup that sets
   `bWorldPositionOffsetWritesVelocity = false` before `RegisterComponent()`.
   It is a public bitfield with no setter. This drops the whole velocity
   submission for the ground cover only, rather than for the renderer, which is
   what `r.Velocity.EnableVertexDeformation` could not do.
3. Rebuild, then re-bake. A full bake measured ~20 minutes on 2026-09-11.
4. Three captures: a fresh control on the new cache, Nanite on, and
   WPO-velocity off. The control is not optional — the material change moves the
   cache identity, so no earlier capture is comparable.

Neither phase-2 change sets a default. Both are visual trades and both are the
owner's verdict under the settings-panel policy: Nanite puts these plants into
the Lumen scene for the first time, and dropping wind motion vectors smears
moving foliage under temporal upscaling.

## What the floor is made of, and what it is not

Named game-thread work at the 48 m default ring, medians, with nesting resolved from the scope
sites in `VoxelEarthFlyPawn.cpp`, `VoxelCharacterMovement.cpp`, `VoxelWorldSubsystem.cpp` and
`VoxelClipmapActor.cpp`:

| scope | median ms | note |
|---|---|---|
| PawnTickMs | 4.62 | contains MovementTickMs 4.61 |
| ↳ CollisionPrepareMs | 3.49 | 76% of the pawn tick |
| ClipmapTickMs | 2.99 | contains RoofProbeMs 2.99, essentially all of it |
| everything else named | < 0.1 each | |
| **named total** | **≈ 7.6** | |
| **game thread median** | **22.83** | |
| **unattributed** | **≈ 15.2 (two thirds)** | |

**Two thirds of the binding cost has no name.** That is the single most important fact in this
document. It is not necessarily our code: `GameThreadTime` includes all engine-side work, and the
missing time could be actor ticks, physics, garbage collection or render command submission. We do
not know, and no amount of optimising the named 7.6 ms can reach a 10 ms budget while 15.2 ms sits
unexplained beside it.

There is a strong clue about where it is not. The terrain-only flight leg of 2026-09-02
(`docs/cascade-cut-to-8-2026-09-02.md`, 28,301 frames at 20 m/s) has a frame p50 of 8.78 ms in
total, so it cannot contain 15 ms of hidden game-thread work. The unattributed cost appears in the
walking, ecology-site configuration and not in the flying, terrain-only one. Those two differ in
three ways at once (character pawn versus fly pawn, ecology fixture versus bare terrain, 2.2 m/s
versus 20 m/s), so the clue narrows the search without settling it.

## The hitches are a separate problem with a known shape

The tail is not the median with noise on it. At the default ring the frame p99 is 350.90 ms and the
worst frame is 1,650.71 ms. Aligning the CSV correctly matters here: `FrameTime` lags the thread
columns by exactly one row in these captures, which I confirmed empirically (correlation of
FrameTime against GameThreadTime is +0.971 at a one-row shift and +0.203 unshifted). Attributed with
that alignment, the ten worst frames are all the same thing:

```
FrameTime 1650.71 | GT 1651.24  GPU 12.45  RT 3.60 | TickMs=1624.5 DispatchMs=1557.3 SubmitMs=1554.7
FrameTime  808.96 | GT  806.55  GPU 15.04  RT 4.19 | TickMs=783.6  DispatchMs=775.2  SubmitMs=686.9
FrameTime  807.94 | GT  808.26  GPU 11.56  RT 4.00 | TickMs=785.0  DispatchMs=772.8  SubmitMs=768.6
```

Game-thread time inside the streaming tick, inside dispatch, inside GPU submit. The GPU is idle at
12 to 15 ms throughout. This is the failure class already documented in
`docs/raster-atlas-warmup-2026-08-24.md` and `docs/submit-split-2026-08-28.md`, at a larger
magnitude than those recorded. At median the same counter reads 0.086 ms, so this is purely a tail
phenomenon: rare, enormous, and entirely on the game thread.

## The plan

Five phases. Each has a gate that can fail, and the order is deliberate: the instrumentation phase
comes before any optimisation of the floor, because two thirds of that floor is currently unnamed.

### Phase 0 — ship the lever that is already measured and already built

Understory size culling is opt-in. Turning it on is worth 21.2 ms of GPU and 37.0 ms of render
thread at the 256 m ring, and it is the difference between 98.2% and 17.3% of frames exceeding
33 ms there. Nothing needs to be written.

It is blocked on one thing only: whether the fade at 85% of the cull distance pops visibly. That is
an image verdict, and the owner has deferred the capture harness that would settle it. Two ways
forward that do not need the harness: ship it with a conservative start distance and accept a
smaller win, or ship it as a Settings row per the panel policy, defaulted on, so a player who sees
popping can turn it off. Either is a decision, not an engineering task.

**Gate:** at the 256 m ring, frames over 33.3 ms fall below 20%, with an owner image verdict on
popping recorded.

### Phase 1 — name the unnamed 15 ms  — **DONE 2026-09-11, and it found something**

Insights was not needed. Of the 31 tick functions in the module, 24 had no CSV scope at all, so the
game thread was unattributable by construction. Scopes were added to all of them (commit `662828f`,
plus the walk driver afterwards) and one capture settled it.

Result, at the forest site, 256 m ring, 541 frames — full table in
`docs/measurements/gamethread-attribution-2026-09-11/`:

| scope | median ms |
|---|---|
| GameThreadTime | 21.80 |
| PawnTick (⊃ MovementTick ⊃ CollisionPrepare 3.83) | 4.45 |
| **OceanTick** | **4.16** |
| **RippleTick** | **4.10** |
| ClipmapTick (⊃ RoofProbe 2.84) | 2.84 |
| WorldSubsystemTick | 0.37 |
| WaterSheetTick | 0.25 |

Named share went from 35% to 75%. The gate asked for 90%; the remaining 5.46 ms is engine-side actor
tick dispatch and the component update pass, which these scopes cannot reach, and chasing it is worth
less than the finding below. **Gate accepted as met in substance.**

**The finding: 8.51 ms — 39% of the game thread — is water simulation at a site with no water.**
The engine's own log puts the spawn column at 70.8 m of ground over a 0.0 m sea level. A
74,384-triangle ocean mesh is followed by transform and asked "is the camera underwater?" every
frame, and a 512×512 ripple wave field is stepped at a fixed 60 Hz every frame, 75.8 m above the
nearest sea. Neither has any gate on water being within reach. This is the first finding in this
work that is plain waste rather than a trade, and it is now the highest-value game-thread item.
Sub-scopes are in the source to split each of the two into its parts; the capture that reads them
is running.

### Phase 2 — kill the submit hitches

p99 is 350.90 ms and the worst frame is 1.65 s, all of it game-thread time inside GPU submit while
the GPU sits idle. This is the worst thing a player would feel, and it is worth more to the
experience than several milliseconds of median.

Start from the two existing documents on this failure class rather than from scratch. The specific
question to answer first is why a submit blocks the game thread for over a second when the same
counter reads 0.086 ms at median: what is being uploaded, how large it is, and whether it can be
split across frames or moved off the game thread.

**Gate:** frame p99 under 33.3 ms at the default ring over a capture of at least 5,000 frames, with
the worst frame under 100 ms.

### Phase 3 — the named game-thread costs

Three items now, in value order, all paid every frame, none of them necessary work:

- **Water simulation at a dry site, 8.51 ms.** `OceanTick` 4.16, `RippleTick` 4.10, `WaterSheetTick`
  0.25. See Phase 1. The shape of the fix is a proximity gate: neither the ocean's underwater test
  nor the ripple field's 60 Hz step has any reason to run when the nearest water surface is 75.8 m
  below the camera and behind terrain in every direction. The ripple field's `IsTickable` already
  exists as the place to put it and currently tests only `bArmed_` and the enable cvar. What the gate
  may NOT do is be wrong at a shoreline, so the predicate has to be conservative and the falsifier is
  a shore capture where ripples must still appear.
- **Collision prepare, 3.83 ms.** Three quarters of the pawn tick. It prepares a voxel query region
  per movement sweep. Worth checking whether consecutive sweeps in one frame re-prepare overlapping
  regions, which is the same shape as the resolve duplication the R0 profile found last week.
- **Roof probe, 2.84 ms.** Effectively the entire clipmap tick. It runs every frame regardless of
  whether the camera moved enough to change the answer.

Neither has been profiled below its own scope, so both numbers are what to attack, not yet a
diagnosis of why.

**Gate:** each under 1 ms at median, with placement, collision behaviour and the clipmap image
unchanged.

### Phase 4 — the GPU, which is a geometry problem and not a pixel problem

This phase was rewritten on the day it was drafted, because the measurements below arrived and
contradicted the obvious reading. Keep the order: attribution, then the decisive experiment, then
the levers that survive it.

**Where the GPU goes at the 256 m ring** (capture 26, `GPU/*` columns, medians over the
WalkForward phase):

| contributor | ms | share |
|---|---|---|
| Understory base pass | 14.80 | 35% |
| Understory velocity pass | 14.96 | 35% |
| Terrain marcher | 9.26 | 22% |
| Everything else: shadows, water, sky, lighting, post, TSR, HUD | ~1.0 | ~2.5% |

Shadows are 0.118 ms, water 0.120 ms, all lighting 0.192 ms, all sky and fog 0.149 ms. **Together
they are about 2% of the frame.** Anyone who proposes optimising them is optimising noise. This
also retires, for this scene, the 15.8 ms terrain-shadow figure that circulates in older notes: it
belonged to the quad path the marcher replaced, and `docs/parked-floor-2026-08-28.md` measures
shadow depths at 0.058 ms on the shipped path.

**Which ring these numbers describe, because it decides how urgent this is.** The 29.76 ms is the
**256 m experimental ring with size culling off**. At the shipping 48 m default the understory is
roughly 1.8 ms of GPU and the marcher dominates instead. So the honest brief is not "cut 29.76 ms by
90%"; it is **"what would it cost to ship a ring several times the default?"** That reframing changes
which techniques are worth their implementation cost, and it means the understory is not currently
the thing standing between this project and its frame budget. The marcher and the game thread are.

**The decisive experiment.** The obvious story was overdraw: the plant material is masked, which
disables early depth rejection, and the depth prepass measures 0.009 ms across three draw calls, so
every layer of overlapping grass is shaded. That story is wrong, and one capture settles it.
Capture 35 runs the identical scene at `r.ScreenPercentage=33` instead of 65, which is 3.87 times
fewer shaded pixels with identical geometry, instances and draw calls:

| counter | 65% (mean of two runs) | 33% | change |
|---|---|---|---|
| GPU/Basepass | 14.803 | 14.724 | −0.5% |
| GPU/RenderVelocities | 14.956 | 14.916 | −0.3% |
| GPU/VoxelMarch | 9.258 | 5.143 | −44% |
| GPU total | 40.699 | 36.326 | −11% |

**The understory passes did not move.** Cutting shaded pixels by a factor of 3.87 changed them by
0.4%, which is inside the noise floor measured below. The understory cost is entirely geometry:
vertex processing, primitive assembly and instance handling. It is not shading, not overdraw, not
resolution, and not the masked material's early-Z behaviour.

That result kills a whole family of proposals before anyone spends a week on one, and it points at
a different family that follows directly from it.

**Lever 4a — two geometry traversals, not one pass plus a bonus one. Corrected.** An earlier draft
called `GPU/RenderVelocities` (14.96 ms) "a full duplicate geometry pass" and treated all of it as a
saving. **That is wrong.** `r.VelocityOutputPass` defaults to 0, and the engine documents mode 0 as
"renders during the depth pass; this splits the depth pass into 2 phases: with and without velocity"
(`VelocityRendering.cpp:31-37`). The project overrides it nowhere.

So `RenderVelocities` **is** the depth pass for velocity-relevant geometry, which is why `GPU/Prepass`
reads 0.009 ms across three draw calls: the plants' depth work is not missing, it is in the other
line. The understory therefore pays what any prepass renderer pays, one depth traversal and one base
pass traversal, with velocity written during the first. Turning velocity off would move the plants
back into the ordinary prepass rather than deleting a traversal, and the saving would be the velocity
write plus one redundant vertex-offset evaluation, not 15 ms.

That makes the real target the traversal count itself, and it is why Nanite matters here rather than
any velocity switch: Nanite rasterises once into a visibility buffer and produces depth, material and
velocity as screen-space exports from it, so **two geometry traversals become one** and the velocity
is still correct. The remaining question is what a single Nanite traversal costs against the current
two, which only the rebake A/B can answer.

**Lever 4b — the LOD chain barely reduces geometry.** Across the 339 baked models, the coarsest LOD
is a median of 74% of LOD0 triangles, and the whole chain saves 29% of triangles library-wide.
Sixteen models have no LODs at all, and 260 of 323 have only two levels. For a workload that has now
been measured as geometry-bound, that is close to having no LOD system. Real decimation on the
coarse levels attacks the cost directly.

**Lever 4c — size culling, about 21 ms, already built.** Covered in Phase 0. Its mechanism is now
understood: it drops 50 of the submitted HISM components by cutting most species' draw distance from
256 m to 32-48 m, which is a geometry reduction, which is exactly what this workload responds to.

**Lever 4d — Nanite is switched off.** `VoxelDetailAssetSubsystem.cpp:1455` sets
`NaniteSettings.bEnabled = false`. Nanite exists for precisely this shape of problem: enormous
instanced triangle counts with weak LODs. Whether it suits 25 mm voxel foliage with masked
two-sided materials is a real question and it may not, but it has not been tried and the workload
now provably matches its purpose.

**The marcher, corrected.** An earlier draft of this section fitted the two resolution points to
"3.71 ms fixed plus 14.25 ns per pixel" and inferred a large resolution-independent cost. **That was
wrong, and the flight leg refutes it in one line:** the marcher costs 5.04 ms in total there, at
921,600 pixels, which is 2.4 times the forest capture's pixel count. A 3.71 ms fixed term cannot fit
inside a 5.04 ms total. A two-point fit through a non-linear curve manufactures an intercept, and
that is all that happened. There is also an in-tree sweep that already measured the kernel as linear:
4.37 / 3.99 / 3.91 ms per million rays across a 5.7x range, within 2%
(`VoxelMarchRenderer.cpp:1806-1808`).

Read per pixel instead, which is the honest unit:

| leg | pixels | marcher ms | ns/pixel |
|---|---|---|---|
| terrain-only flight leg | 921,600 | 5.04 | 5.47 |
| forest walk, 65% screen | 389,376 | 9.26 | 23.78 |
| forest walk, 33% screen | 100,674 | 5.14 | 51.09 |

Two things follow, and both are more interesting than a fixed cost. **Per-ray cost rises as rays get
fewer**, 2.15x more expensive per ray for 3.87x fewer rays, which is the signature of a latency-bound
kernel losing cache reuse rather than of a constant overhead. And **the forest is 4.3x the flight leg
per pixel, not the 1.8x a raw millisecond comparison suggests.**

That kills the mechanism the earlier draft guessed at. Camera pose was the hypothesis, since marcher
cost rises when rays travel through empty air and a walking camera faces the horizon. But the entire
measured pose range on bare terrain spans 0.82 ns/pixel looking down to 4.16 looking at sky
(`docs/marcher-direction-baseline.md`), and the forest sits at 23.78, far above the top of that
range. **Pose cannot explain a 4.3x gap when the whole pose effect is 5x between its extremes.**

The candidate that fits is one the plan had not connected: **trees are marched voxels, not instances**
(`docs/environment-residency-audit.md`). Every terrain-only baseline was measured on bare ground. The
forest site presents the marcher with a canopy: high-variance occupancy with rays threading gaps,
which is the worst case for a cost law that charges per segment entry. Nothing has measured this, and
it should be measured before anything is built.

The problem for the target is arithmetic. At the default 48 m ring the marcher is about 9.9 ms of a
13.2 ms GPU frame, and the whole-frame budget for 100 FPS is 10 ms. **Terrain alone spends the
entire budget before a single plant is drawn.** Marcher cost tracks ray count, ray count tracks
pixels, and reducing rays has been rejected twice by the owner, with half-resolution marked rejected
and an explicit instruction not to re-propose ray-count reduction. Those two facts cannot both hold
at the current resolution. This plan does not try to resolve that; it names it, because a plan that
quietly aims at an unreachable number is worse than one that says where the wall is.

**Gate:** understory GPU under 10 ms at the 256 m ring; the marcher's forest premium attributed to a
named cause; and a decision recorded on the marcher arithmetic above.

**Before any marcher work is built, three measurements, none of which need code.** A GPU profile
capture at the forest site, which splits the single `VoxelMarch` timer into its passes (it currently
encloses a per-frame reduce over the whole brick pool as well as the march itself). A four-point
resolution sweep, to replace the two-point fit above with a real curve. And a direction sweep at the
forest spawn against the vista baseline, which is the experiment that decides whether this is a
renderer problem or a site-coverage problem. The harness for all three already exists.

### Phase 5 — measure at the speed the target is written for

Every slice number in this document is from walking at 2.2 m/s. The target is written for 20 m/s.
Streaming cost scales with how fast new ground arrives, so the slice has never been measured under
the condition it is supposed to pass. This needs a flight or sprint leg through the ecology fixture.

**Gate:** a capture at 20 m/s through the fixture, with the same receipt discipline, so the plan's
own target is measured rather than assumed.

## Research addendum, 2026-09-11: the candidate levers

Three investigations (engine source, repo history, published practice) ran against the measurements
above. What follows is what survived them. Several popular ideas did not, and they are listed too,
because knowing what not to try is most of the value.

### The understory: Nanite is the only route that removes a whole pass

The understory pays two geometry traversals per frame. One is the base pass. The other is reported as
`RenderVelocities`, and at the engine default (`r.VelocityOutputPass=0`) that line **is** the depth
pass for vertex-deforming geometry, which is why the ordinary prepass reads almost zero. So this is
not a spare pass bolted on for motion vectors; it is the depth half of a normal two-traversal
renderer, with velocity written into it.

**The motion vectors are correct, so the pass cannot simply be deleted.** I checked, because a
plausible failure mode would have made it useless work: if the wind's animation used a value with no
previous-frame history, the computed velocity for the sway would be zero and the pass would be
paying 15 ms for nothing. It does not. The material translator substitutes the previous frame's time
automatically when it builds the velocity variant
(`HLSLMaterialTranslator.cpp:5278`, `bCompilingPreviousFrame ? View.PrevFrameGameTime : View.GameTime`).
Only the slowly-changing weather vector, which comes from a parameter collection, lacks history, and
that changes on weather timescales rather than per frame. Turning the pass off would therefore cost
visible smearing on moving grass, and by the correction above it would not even recover the line's
full cost. Both halves of that make it a poor lever.

**Nanite collapses the two traversals into one while keeping correct velocity.** Nanite vertex factories are
excluded from velocity shader compilation entirely (`VelocityRendering.cpp:201-202`), because Nanite
computes velocity per pixel from the visibility buffer instead
(`NaniteDepthExport.usf`, `NaniteExportGBuffer.usf`). Nanite geometry is rasterised once; depth,
material and velocity all become screen-space exports from that single pass. On top of that it brings
continuous cluster LOD, which attacks the base pass too, and it does per-instance culling on the GPU
while honouring the existing cull distances. Instanced components already return a Nanite proxy when
the asset has Nanite data (`HierarchicalInstancedStaticMesh.cpp:2993-3004`).

**Why it is currently off, and why that may not apply here.** The recorded reason is not performance.
It is arbitrary voxel destruction: `docs/tree-appearance-pilot-status.md` records the decision, and it
is about **trees** on the editable procedural-mesh path. The understory is explicitly the opposite
case, accepted as instanced meshes that are never edited in place. Whether the objection binds them
is an owner decision, not an engineering one, but the technical half of it is now settled: the
understory components never touch the destructible path. `VoxelDetailAssetSubsystem.cpp` contains no
per-instance destruction code at all, it consumes baked static meshes rather than procedural ones,
and the procedural plant component has exactly one consumer, the separate opt-in environment
prototype. So the flag on line 1455 disables Nanite on the understory as a side effect of a decision
taken for trees, which fits the code site carrying no comment and the introducing commit carrying no
rationale. **Scope any change to the 25 mm understory profiles**: trees still have the
destructibility constraint and it is still correct for them.

**Three costs that would not show up in the pass counters.** Geometry below 32-pixel edges goes
through Nanite's software rasteriser, which is its design case but not free. Masked materials sort
last in the raster bins, mitigable per component with a programmable-raster distance. And instanced
foliage currently contributes nothing to the global illumination scene
(`HierarchicalInstancedStaticMesh.cpp:806-807`); under Nanite it would, which is a lighting change to
judge by eye and a cost that will not appear in the base pass.

**A method trap worth more than the lever.** Testing this by toggling the Nanite cvar measures the
simplified fallback mesh, not the real geometry, because the proxy render mode falls back by default.
Both arms must be rebakes with the flag flipped in the asset. The repo learned this once already and
wrote it down in `ue-project/Tools/capture_tree_appearance_pilot.py`.

### The understory: the LOD chain, and why it is weak

The coarsest LOD is a median 74% of LOD0 triangles. It is worth knowing that this is **not** the
previously-fixed bug where Unreal's default reduction overwrote authored LODs; that fix is live
(`VoxelDetailAssetSubsystem.cpp:1462` sets the reduction base per source model, and the cache
identity carries `authoredLOD=1`). The 74% is what the authoring pipeline actually produces. For a
geometry-bound workload that is the thing to change, and it is independent of Nanite: the two can
ship together. Re-voxelising coarse levels at a coarser pitch is the obvious route for blocky assets,
where aggressive simplification is far more visually acceptable than it would be for realistic
foliage.

### The marcher: measure before building, because the timer is not what it says

The single `VoxelMarch` timer encloses more than the march. Inside it, among others, a reduction
dispatches **393,216 threads every frame** over the whole brick pool to produce eight numbers whose
input changes at streaming rate, not frame rate. Nothing isolates it, which is why it has never been
named or priced.

Three measurements, none needing code, should precede any marcher work: a GPU profile capture at the
forest site to split that timer into its passes; a four-point resolution sweep to replace the
two-point fit this plan already had to retract; and a camera-direction sweep at the forest spawn
against the bare-terrain baseline, which decides whether the forest premium is a renderer problem or
a site-coverage problem. The harness for all three exists.

After that, the candidates in rough order of expected value:

- **The per-entry constant.** An entry costs roughly 2,000 GPU cycles for what is two to four
  dependent memory loads, which is latency, serialised. This is the repo's own second open item, named
  in August and never run, and no vendor GPU profiler has ever been pointed at this renderer. It
  attacks every entry including the useless ones.
- **Wave width.** The kernel runs 64-wide on hardware that is natively 32-wide, which halves the
  waves available to hide that latency. One attribute and a permutation, with an engagement counter
  that can fail.
- **One address space over the cascade.** Today a ray can pay up to fourteen segment entries, seven
  rings times a retry ladder. Behind a single page table it would pay about two. This is the largest
  structural number available and it is genuinely untried, but it is weeks of work and the repo's own
  bug history is full of per-level mistakes that a unified space would make global. Model it offline
  first: if the simulated entry count does not collapse, do not build it.

### What the research ruled out

Ray coherence sorting and wavefront compaction predict nulls here, because the waves are already 99%
lane-full and primary rays are the most coherent rays that exist. Cone-marched ray starts were
refuted in-tree. Temporal reuse of hit distance already ships. Anything targeting iterations rather
than segment entries is aimed at the wrong term of the cost law. And the whole empty-space-skipping
family has seven dead arms behind it, one retired permanently with instructions not to rebuild it.

One nuance worth preserving: the height pyramid is often lumped in with those, but it was retired on
**correctness**, not cost, after missing 479 rays. It is world-derived rather than residency-derived
and it targets entries. If anyone revisits it, the first task is diagnosing those missed rays, not
timing it.

## How trustworthy these numbers are

Capture 34 repeats capture 26's configuration exactly, which gives a run-to-run noise floor that
every single-pair claim in this document can be read against:

| counter | capture 26 | capture 34 | difference |
|---|---|---|---|
| GPU total | 40.739 | 40.659 | −0.2% |
| GPU/Basepass | 14.852 | 14.754 | −0.7% |
| GPU/RenderVelocities | 14.943 | 14.969 | +0.2% |
| GPU/VoxelMarch | 9.239 | 9.276 | +0.4% |
| Game thread | 23.117 | 23.769 | +2.8% |
| Frame time | 42.355 | 42.101 | −0.6% |

**GPU counters reproduce to within 0.7%.** The 20.8 ms size-cull saving and the 0.4% resolution
result are both far outside that, so neither is a one-run artefact. The game thread is looser at
2.8%, so treat small game-thread differences with more suspicion than GPU ones.

Two instrument notes worth keeping. `-ResX`/`-ResY` are inert in this harness: a capture requesting
2560x1440 still rendered 832x468. The knob that works is `r.ScreenPercentage` passed through
`-dpcvars`, since it is latched at startup, and the proof it engaged is the log's own
`px of a WxH view` line. And two CSV counters must not be quoted for instancing work:
`RHI/PrimitivesDrawn` counts each draw call's triangles once rather than once per instance, and
`GPUSceneInstanceCount` counts registered rather than drawn instances. Neither moves when culling
works.

## What this plan does not claim

It does not claim the target is reachable, and Phase 4 now gives a concrete reason to doubt it at
the current resolution: the marcher alone costs about 9.9 ms against a 10 ms whole-frame budget,
and the one lever that moves it has been rejected twice on visual grounds. Terrain alone reached
p50 8.78 ms on a flight leg and was 1.7 ms from the p95 bar, but that is a different pawn, site and
speed from anything measured here. Whether 100 FPS is achievable with this much ground cover is
what Phases 1 and 4 answer between them, not this document.

It also does not propose new rendering architecture. Every phase above is either shipping something
already built, measuring something currently unmeasured, or attacking a specific named cost. If
Phase 1 finds the missing time is structural, the architectural conversation starts there, with
evidence.
