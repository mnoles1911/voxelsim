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

### Phase 1 — name the unnamed 15 ms

No optimisation work on the floor should start before this. Run Unreal Insights against the slice,
which the repo already uses (`-trace=cpu,frame,log`, per `docs/tile-loading-async-2026-09-07.md`),
in two configurations: the walking ecology capture, and a flight leg at the same site. The pair
separates "character pawn and physics" from "ecology site" as the source of the missing time.

**Gate:** at least 90% of the game-thread median attributed to named scopes, with the result
written up as a measurement record. If the missing time turns out to be stock engine work we do not
control, that is a finding that changes the whole plan, and it is better to learn it in week one
than in week six.

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

Two items, both paid every frame, both looking like query costs rather than necessary work:

- **Collision prepare, 3.49 ms.** Three quarters of the pawn tick. It prepares a voxel query region
  per movement sweep. Worth checking whether consecutive sweeps in one frame re-prepare overlapping
  regions, which is the same shape as the resolve duplication the R0 profile found last week.
- **Roof probe, 2.99 ms.** Effectively the entire clipmap tick. It runs every frame regardless of
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

**Lever 4a — the velocity pass, about 15 ms, half the understory cost.** `GPU/RenderVelocities`
(14.96 ms, 179 draw calls) is a near-exact duplicate of the base pass (14.80 ms, 173 draw calls). It
exists because the wind animation moves vertices in the shader, and Unreal puts anything with
vertex-deforming materials into the velocity pass every frame whether or not it actually moved.
`r.VelocityOutputPass` is not set anywhere in the project, so this is the engine default. The
question to answer first is whether distant or small plants need per-vertex wind at all, since
anything that stops deforming leaves the pass. This is the single largest identified saving in the
frame and nothing has been tried against it.

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

**The marcher, and an honest problem with the target.** The marcher does scale with pixels, but not
purely: fitting the two resolution points gives a fixed 3.71 ms plus 14.25 ns per pixel. It is also
roughly twice as expensive here as on the terrain-only flight leg, 9.26 ms against 5.04 ms. A
plausible mechanism exists, since marcher cost tracks how far rays travel through empty air and a
walking camera looks at the horizon while a flying one looks down, but nothing has measured that and
it should not be assumed.

The problem for the target is arithmetic. At the default 48 m ring the marcher is about 9.9 ms of a
13.2 ms GPU frame, and the whole-frame budget for 100 FPS is 10 ms. **Terrain alone spends the
entire budget before a single plant is drawn.** Marcher cost tracks ray count, ray count tracks
pixels, and reducing rays has been rejected twice by the owner, with half-resolution marked rejected
and an explicit instruction not to re-propose ray-count reduction. Those two facts cannot both hold
at the current resolution. This plan does not try to resolve that; it names it, because a plan that
quietly aims at an unreachable number is worse than one that says where the wall is.

**Gate:** understory GPU under 10 ms at the 256 m ring, and a decision recorded on the marcher
arithmetic above.

### Phase 5 — measure at the speed the target is written for

Every slice number in this document is from walking at 2.2 m/s. The target is written for 20 m/s.
Streaming cost scales with how fast new ground arrives, so the slice has never been measured under
the condition it is supposed to pass. This needs a flight or sprint leg through the ecology fixture.

**Gate:** a capture at 20 m/s through the fixture, with the same receipt discipline, so the plan's
own target is measured rather than assumed.

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
