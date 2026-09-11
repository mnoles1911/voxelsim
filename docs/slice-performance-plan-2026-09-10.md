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

### Phase 4 — the GPU floor

GPU p50 is 13.16 ms at the default ring against a 10 ms whole-frame budget, so the GPU alone
currently exceeds the target. For reference the terrain-only leg measured 7.75 ms steady GPU
(marcher 5.04, TSR 1.26), so the slice's ecology content is adding roughly 5.4 ms, with the caveat
that the two captures are at different sites.

This phase depends on Phase 0 landing first, because size culling changes what the GPU is doing,
and on a proper GPU attribution of the slice scene, which does not exist yet in the same form as
the terrain-only one.

**Gate:** GPU p50 under 8 ms at the default ring with the ecology fixture loaded.

### Phase 5 — measure at the speed the target is written for

Every slice number in this document is from walking at 2.2 m/s. The target is written for 20 m/s.
Streaming cost scales with how fast new ground arrives, so the slice has never been measured under
the condition it is supposed to pass. This needs a flight or sprint leg through the ecology fixture.

**Gate:** a capture at 20 m/s through the fixture, with the same receipt discipline, so the plan's
own target is measured rather than assumed.

## What this plan does not claim

It does not claim the target is reachable. Terrain alone reached p50 8.78 ms and was 1.7 ms from
the p95 bar; the slice adds content and a character pawn on top of that, and 15 ms of the gap is
currently unexplained. Whether 100 FPS with this much ground cover is achievable is a question
Phase 1 answers, not this document.

It also does not propose new rendering architecture. Every phase above is either shipping something
already built, measuring something currently unmeasured, or attacking a specific named cost. If
Phase 1 finds the missing time is structural, the architectural conversation starts there, with
evidence.
