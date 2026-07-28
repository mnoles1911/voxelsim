# Speculative generation (T4-1): the re-sequenced path, and why this order

**Status:** written 2026-07-27. **S0 CLOSED**, **S1 CLOSED**, **S2 SHIPPED**
(except S2-1/S2-5 — see below), **S3 SHIPPED**, **S4 (T4-1) CLOSED — confirmed
by a 10-leg sweep, 2026-07-28.**

**This plan's goal is met and the programme has moved on.** The constraint is no
longer streaming: it is the draw path and the frame-time tail. Session summary,
including what did not ship and why:
`docs/measurements/session-summary-2026-07-28.txt`.

**Still open from this plan:** S2-1 (GPU hide pass) is built but UNVERIFIED and
ships gated off — its forced probe does not yet give a trustworthy verdict
(`docs/measurements/s21-gpu-hide-probe-2026-07-28.txt`). S2-5 (drop the CPU
shadow) is blocked on it, and is worth ~960 MB of system RAM at the new pool
capacity. Capacity step 2 was taken as **80M, not the planned 104M** — the 104M
figure was sized for a speculative parked population that measurement shows does
not exist (peak 172 chunks against a 4,000 cap). Data: `docs/measurements/s0-apply-census-2026-07-27.txt`,
`docs/measurements/s1-close-2026-07-27.txt`,
`docs/measurements/t41-first-result-2026-07-28.txt`.

    baseline                                    260.9 chunks/s   15,032 holes
    S1 (batch publish, budgets, capacity)     ~1,040.5                0
    S2-3 parking (line / circle)          1,064.9 / 906.5            0
    S4 T4-1 (lead 4.0s)                       1,069.5                0

**T4-1's result is not in that table, and that is the point.** Throughput is
unchanged — chunks/s spans 1061.5–1070.9 across all ten sweep legs *including
the controls*, a 0.9% total spread. What moved is the metric the table does not
carry — **median transient holes during the flight: ~822 → ~57, a 93%
reduction**, replicated on both passes at every lead value, against Wave S4's
close criterion of "toward the CPU arm's 55."

**The lead time is not a tuning knob.** 1 s captures the entire effect; 8 s is no
better. The plan sweeps lead because it assumed more lead buys more coverage —
it does not, and the cheapest setting is the best one. Size anything that scales
with lead for ~2 s. Recommended default if switched on: `VelocityLeadSec 2.0`.

Mechanism, from the fork census: GPU in-flight **19 → 63** (the idle capacity
this whole re-sequencing was aimed at), demand queue depth **234 → 188** because
speculation answers chunks before admission asks for them, and demand
submit-to-deliver latency **1864 ms → 1110 ms** as a consequence. Adding
non-demand work made the demand path faster.

**Read this before judging any future wave on chunks/s alone.** A throughput
metric scores the above as a tie. `tools/voxel-leg-summary.ps1` now reports the
flight-phase hole distribution as its own column so this cannot happen silently
again; `holes(final)` is 0 on both arms and can never decide a latency question.

**T4-1 is built.** Speculative enumeration ahead of the predicted anchor,
its own queue and sub-budget, submitted last, parked on arrival, and adopted by
the admission path that S2-3 already measured. Everything off behind
`voxel.Stream.VelocityLeadSec 0`.

**How much of S4 turned out to be free**, because S2-3 built it first: adoption
is *unchanged code*. `AddCandidate` finds a parked entry and unparks it, and it
does not care whether that entry got there by eviction or by speculation. The
whole of S4-3 is one branch in the completion handler that parks instead of
applying. Sequencing S2 before S4 was the single highest-leverage decision in
this plan. This supersedes the wave ORDER in
`docs/streaming-perf-implementation-plan.md` for the T4-1 path only — that
document's per-item content still stands and is referenced throughout.

## Wave S1 outcome, and what it did to the rest of this plan

    baseline                                260.9 chunks/s   15,032 holes
    + batch publication (S1-1)              581.3               966
    + pool 64M / table 81,920               631.6               257
    + unload budget 24 -> 256               797.0                 0

**§1a is discharged.** The apply loop went from never once reaching an empty
queue to `queueEmpty` dominant with a 0% stale fraction. The consumer is no
longer the wall — which was the entire premise of this re-sequencing. And
`holes = 0` at the adopted cascade for the first time.

**ADMISSION IS NOW THE LARGEST SHARE OF THE TICK — BUT THE TICK IS SMALL.**
*(Corrected 2026-07-27: the first version of this section said "66% of the tick"
and treated it as the new bottleneck. That number is a PEAK WINDOW's share of
the streaming tick, and the tick is only ~16% of wall time. Measured across a
full leg, `RecomputeDesiredSet` totals 10.2 s of ~285 s — **~3.6% of wall**.
Real, worth fixing, and nowhere near the constraint it was described as. This is
the same error that produced the original P0: a true ratio quoted against the
wrong denominator.)*

At the winning config `RecomputeDesiredSet` is the biggest single item inside
the streaming tick. The mechanism is real even though the magnitude was
overstated: `RecomputeDesiredSet` relaxes every `LevelAdmissionCutoffDistSq` to
`DBL_MAX` when `PendingJobNum() * 4 < Cap * 3` — whenever the pending queue is
short. S1 made the queue permanently short, so the cutoff is permanently
disabled: ~22,300 records/s admitted against ~800 chunks/s actually loading, and
`tracked` peaking near 96,000 against a 39,020 settle.

Speculation *adds admission-side work* — S4-1 enumerates candidates the demand
path has not asked for — so this still shapes S4's design even at 3.6% of wall.
It is a design constraint on the enumerator, not a blocker to be cleared first.

  ⇒ T4-2 stays a **bet, not a prerequisite** — at 3.6% of wall the exit scan is
  not what stands between here and T4-1. What DOES matter for T4-1 is unchanged:
  speculation adds admission-side work, so the enumerator must be cheap and must
  not share `PendingJobKeysByLevel`. Re-check the share once speculation is
  actually adding load.

### Items measurement removed or demoted (do not rebuild without new evidence)

| item | was | measured |
|---|---|---|
| **T1-3** params cache | on T4-1's critical path | `params` is 0.002–0.004 ms — **0.2% of an apply**. STRUCK. |
| **S1-2** handle recycling | "3.72× reduction in the dominant term" | Works (`allocsEver` −60%) but throughput moved **inside noise**. `BuildChunkRuns` is dominated by `Runs.Sort()` over live runs, not the walk. Keep as an unboundedness fix; it is not a speed-up. |
| **T3-6** idle defrag | implied by 16,903 free runs / 72× largest-run collapse | The fragmentation was a SYMPTOM of residency ballooning on a too-small unload budget. Fixed by a cvar. **Not needed.** |
| **T1-2(b)**, **S1-4** | next levers after S1-1 | Real costs (`Runs.Sort` ~3.3 ms, `RebuildRunBounds` ~1.34 ms per publication), but the whole streaming tick is ~16% of wall and these are a slice of it. **Deprioritised, not cancelled.** |
| **MaxAppliesPerFrame** | "rejected at 192" | That leg was CONTENDED. Re-run clean: **1,033–1,048 chunks/s vs 794, holes 0** — a +30% WIN, not a regression. Default now 192. |

### Sizing, which T4-1's budget depends on

The pool went 44M → 64M quads and the table 49,152 → 81,920 to stop batching
refusing allocations. With the unload budget fixed, peak residency settles at
~50,900 chunks (~46M quads) — so the table raise is genuinely required (the peak
went through the old floor) but **64M may be more than needed**. That matters
here specifically: §2.6's memory arithmetic and T4-1's speculative reserve are
both sized from what is left over. Run a control at 48M before treating the
extra as necessary.

**What S0 changed, in one block** (details in the measurements file):

- **§1a confirmed.** The apply loop never exits on an empty queue while
  streaming (wallClock beats queueEmpty 5.5:1); `poolAdd` is 98–99% of per-apply
  cost and grows **0.275 → 2.108 ms within a single leg**. The published
  "results are not ARRIVING" reading is falsified — results arrive faster than
  the consumer applies them.
- **T1-3 is STRUCK from the critical path.** `params` measured 0.002–0.004 ms,
  0.2% of an apply. Multiplying 0.2% by a speculative lead factor is still 0.2%.
  One item removed before it was built, which is what the wave was for.
- **S1-2 handle recycling is RAISED.** §2.2's prediction held: `BuildChunkRuns`
  walks 68,416 allocations to emit 18,389 runs by the end of a flight — 3.7×
  waste, climbing, unbounded in session length. Batching does **not** fix it.
- **New: the result queue backs up and the backlog rots.** The producer runs ~3×
  the consumer under flight; deliver-to-apply reaches tens of seconds; a mean
  **40%** of drained results (peak 92.8% in a window) are discarded as stale.
  This makes T1-1 compound rather than linear — cutting apply cost shortens the
  queue, which cuts the stale fraction, which recovers throughput again.
- **Correction to the review:** it prices `RebuildRunBounds` at 2–4 ms per
  applied chunk; measured 0.247–0.714 ms. Still co-dominant with
  `BuildChunkRuns`; the number itself should not be requoted.
- **Harness:** `tools/voxel-run-leg.ps1` is a **cold-fill driver** and silently
  truncates flight legs at the end of preflight. Flight legs must launch the
  editor directly and let `UVoxelPerfRunSubsystem` exit on its own clock.

## Why this document exists

`docs/streaming-perf-implementation-plan.md` (PR #163) sequences the deep
review's 28 ideas into five waves aimed at the ~600 chunks/s pooled plateau.
T4-1 — speculative generation into the idle GPU — is filed last, as a "radical
bet."

**Owner decision (2026-07-27): T4-1 is the target.** It is the only item on the
board that changes the *category* of the problem rather than the number.
Terrain is built and parked in the pool before the camera flies into it, so the
LOD-boundary holes and the pop-in disappear rather than getting faster. Same
worldgen, same quality, same LOD distances — earlier.

So this plan builds only what T4-1 strictly needs, in dependency order, and
defers the rest. Nothing is cancelled; the deferred items keep their numbering.

**Owner decisions taken with it:** pool capacity target **104M quads**, reached
in two steps; **hard stop to re-decide after the plateau closes** (end of Wave
S1).

*Update after S1: only the first step (64M) was needed, and it was taken early
because batching — not parking — required it. Peak residency with the unload
budget fixed is ~50,900 chunks (~46M quads), so the second step to 104M is NOT
justified by anything measured yet. Re-derive it from T4-1's actual speculative
reserve rather than taking it as given.*

## How to use this document

Read `docs/deep-review-streaming-perf-2026-07-27.md` (the diagnosis) and
`docs/streaming-perf-implementation-plan.md` (the item detail) first. This plan
refers to their item numbers — T0-1, T1-1, T2-4 — without restating them, and
records only what *changes*: the order, and six code findings that alter what
gets built.

Ground rules are `docs/gpu-waves-plan.md:88-200`. All 14 apply unchanged.

---

## 1. What T4-1 actually needs

T4-1 = *pre-mesh the annulus in the velocity cone into parked pool ranges, so
admission finds geometry instead of commissioning it.* Five hard prerequisites.
Three are existing plan items; two are new.

| # | Need | Item |
|---|---|---|
| 1 | A consumer that can absorb extra applies — speculation *raises* apply volume, and against a per-apply O(N) tax it makes things strictly worse | **T1-1** batch publication |
| 2 | An allocation path that does not degrade as chunks churn | **NEW: handle recycling** (§2.2) |
| 3 | ~~A per-chunk param source that does not cost a full `Amplifier::column`~~ | ~~**T1-3(a)**~~ — **STRUCK by S0**: params is 0.2% of an apply |
| 4 | A place to put geometry nothing has asked for yet | **T2-4** parking (cheaper than the plan assumes — §2.3) |
| 5 | A direction to speculate in | **T2-1's velocity source only**, not its admission rewiring (§2.4) |

**Deferred, not cancelled:** T1-2(b), T1-5, T1-6, T2-2, T2-3, T2-6, T2-7, all of
T3, T4-2..T4-5. **T2-5 stays do-not-build.** **T1-7 is pulled onto the path** —
it is a correctness fix for pool-refusal stranding and this plan deliberately
raises pool pressure. **T1-4 is pulled back on** for a reason the plan does not
give (§2.6).

---

## 2. Findings that change the plan

Six things found by reading the code. Each changes what gets built. Symbols, not
line numbers, are the durable references (ground rule 12); line numbers here are
hints from 2026-07-27.

### 2.1 The 92,000 chunks/s headroom figure does not transfer to the live path

It comes from `voxel-core/bench/gpu_harness.cpp`'s `runGateMode()`
(`docs/gpu-g0-sizing.md` §1): raw Vulkan, level 0, tiles of 128×128 columns with
a **14×14-brick owned interior** and persistent descriptor sets across flights of
8 — no RDG, no `FRHIGPUBufferReadback`, no poll quantisation. The shipping path
dispatches 48×48 columns to mesh 4×4×4 interior bricks: a **3.4× halo waste per
chunk**, called out in `SubmitGpuMeshJob`'s own comment. The bench has that waste
amortised away.

**The live GPU dispatch ceiling is unmeasured.** What *is* measured and does
support the idea: the fork sits at **~11 jobs in flight against a cap of 256**
and delivers ~850/s under load. It is not depth-bound. There is real slack — its
size is simply unknown.

⇒ Wave S1 closes with a direct measurement of the live ceiling, and the
speculative lead budget is sized from that number, not from 92k.

### 2.2 `BuildChunkRuns()` is O(chunks ever added), not O(resident)

`Allocations` is append-only. `AddChunk` and `AddChunkFromGpu` both
`Allocations.Add(...)`; `Reset()` happens only in `InitPool`. Freed slots are
zeroed in place and never reused. `BuildChunkRuns()` does
`Runs.Reserve(Allocations.Num())` and then walks all of it — and it runs once per
publication, which today is once per applied chunk *and* once per unload.

Over the standard leg (90 s preflight + 120 s flight at ~600–1,000/s)
`Allocations.Num()` reaches ~150k–200k while live runs stay ~39k. So late in a
leg every single apply pays a ~2 MB reserve plus a 200k-entry walk plus a sort.

**Testable prediction, checked in S0-2: apply rate decays monotonically across a
leg.** Fix is a `FreeHandles` LIFO mirroring `FreeChunkIds` — cheap, safe, and
required before speculation churns allocations faster.

### 2.3 Parking is far cheaper than T2-4 assumes, and does not depend on T1-4

`VoxelQuadVertexFactory.ush` reads `ChunkOrigins[ChunkId].w` as the scale, so
**`w == 0` collapses every quad naming that id to a point regardless of what the
id buffer holds**. `ComputeRunBounds` returns `Hidden` on `Entry.W <= 0.f` and
`BuildCulledRanges` skips it.

So park = `ChunkOrigins[id].W = 0` + `bChunkTableDirty = true`. No quad traffic,
no `Pool.Free`, no id stamp — and the run is unchanged (`ChunkId`, `FirstQuad`,
`NumQuads` all identical), so **`bRunsDirty` need not even be set**. Unpark
restores the entry.

The id-stamp path T1-4 targets is *never entered* by parking, because parking
never recycles the id. The plan's "depends on T1-4 and T1-2(a)/(c); do not build
before both ship" is wrong on T1-4 and overstated on T1-2.

### 2.4 `Pawn->GetVelocity()` reads ~zero under the only sanctioned harness

T2-1 as written says "Read `Pawn->GetVelocity()` alongside the location."
`UVoxelPerfRunSubsystem` drives `-VoxelPerfFlight=line` with
`SetActorLocationAndRotation(..., ETeleportType::TeleportPhysics)` every tick,
bypassing `AddMovementInput` and `UFloatingPawnMovement` entirely.
`AActor::GetVelocity()` on a pawn returns the movement component's `Velocity`,
which that path never updates.

**T2-1 and T4-1 would both measure exactly nothing on the standard leg** —
lesson 7's "guards that can never fire," caught in advance this time.

⇒ Derive velocity by finite-differencing `LastAnchorLocation`, which
`TickStreaming` already stores, and EMA-smooth it. The first deliverable of Wave
S3 is a log line proving it is non-zero in flight.

### 2.5 Parked geometry cannot live on `FChunkRecord`

`DrainUnloads` does `ChunkRecords.Remove(Key)` unconditionally after releasing
geometry, destroying `PoolSlot`, `GenerationId` and `bMeshSettled`. A
speculative chunk has no record at all yet.

⇒ One registry, `TMap<FVoxelLevelChunkKey, FParkedGeometry>`, outside
`ChunkRecords`, serving both parked-on-evict and parked-on-speculation.

Corollary: `MarkChunkDirtyForRemesh` bumps `GenerationId` but **does not release
geometry**, so parked geometry would silently survive an edit unless the edit
paths evict from this registry explicitly.

### 2.6 The CPU shadow is what makes a 104M pool expensive, and T1-4 removes it

`InitPool` does `PooledQuads.SetNumZeroed(CapacityQuads)` (8 B/quad) and
`QuadChunkIds.SetNumZeroed(CapacityQuads)` (4 B/quad) — **12 B/quad of system
RAM** — and `CreateSceneProxy` copies both arrays *whole* on every proxy
creation. So 104M quads is not 832 MB:

| | 44M today | 104M target |
|---|---|---|
| GPU quad buffer (8 B) | 352 MB | 832 MB |
| GPU chunk-id buffer (4 B) | 176 MB | 416 MB |
| CPU shadow, `PooledQuads` + `QuadChunkIds` (12 B) | 528 MB | **1.25 GB** |
| transient copy on proxy creation | 528 MB | **1.25 GB** |

**~2.5 GB total, plus a 1.25 GB game-thread memcpy on any render-state rebuild.**

Under the shipped `voxel.GPU.MeshDirectToPool 1` default the shadow is *never
written* for GPU-meshed ranges by design; its only remaining consumer on the
GPU-only path is `RemoveChunkInternal`'s id stamp — exactly what **T1-4** moves
to the GPU. With T1-4 shipped the shadow can be sized to zero on the GPU-only
arm.

That is why T1-4 is back on the path, and why capacity is raised in **two steps**
rather than one: 64M when parking ships, 104M when speculation ships, so the
memory arrives with the feature that needs it.

Measured requirement is ~50–64M: 35.2M resident + ~7.2M for 8 s of lead + ~7.2M
parked churn, at the measured 902 quads/chunk mean (35,205,733 quads over 39,020
chunks). The remainder of 104M is fragmentation and growth headroom — real,
since no defrag exists (T3-6 is deferred).

---

## 3. Sequencing at a glance

| wave | contents | ships as | closed by |
|---|---|---|---|
| **S0** | instrumentation only | one PR | a written verdict on §1a |
| **S1** | T1-1, handle recycling, T1-3(a), conditional T1-2(a)+(c), ceiling measurement | one PR per item | pooled arm ≥ 1,082 chunks/s, holes 0 — **then HARD STOP** |
| **S2** | T1-4, T1-7, parking, capacity step 1, CPU-shadow drop | one PR per item | parking measured on its own |
| **S3** | velocity source only | one PR | non-zero smoothed velocity logged in flight |
| **S4** | T4-1 | one PR per sub-item | flight holes 854–907 → toward 55 |

**The hard stop after S1 is not a formality.** If the plateau has not closed,
the §1a diagnosis is wrong, and the correct next move is T3-5 (GPU-driven cull),
not Wave S2. Do not carry on through a failed S1 close.

---

## 4. The waves

### Wave S0 — Instrument (nothing built)

A hypothesis is cheaper to kill with instrumentation than to fix blind
(`docs/lessons-2026-07-27-gpu-sessions.md`, lesson 1).

- **S0-1 = T0-1** as written: `DrainResults` exit-reason counters
  (unconditional — four increments); per-apply stage timings in
  `ApplyMeshResult` (pack / params / pool-add / table-push); `FPoolPushStats`
  around the `OriginsCopy`/`ParamsCopy` assignment and the `BuildChunkRuns()`
  call; a `FThreadSafeCounter64` on `FVoxelGpuPoolBuffers` accumulating
  `RebuildRunBounds()` cycles — that holder exists precisely for cross-thread
  counters that outlive the component, and `DroppedWrites` is the precedent.
  Gate `voxel.Stream.ApplyStageStats` (default 0) on the
  `FPlatformTime::Seconds()` pairs only, so the instrument cannot be what is
  being measured.
- **S0-2 — new, tests §2.2.** Log `Allocations.Num()` beside `NumLiveChunks` in
  the 5 s pool line, and emit `avgChunks/s` per 5 s window rather than only as a
  leg mean. **Falsifies §2.2** if the ratio stays flat and the rate does not decay.
- **S0-3 = T0-2**, trimmed: `QueuedMs` + `ReadyToDeliverMs` per producer, **and
  the per-level quad-count distribution** — that distribution is the input the
  speculative reserve is sized from, and it is free here.
- **S0-4 = T0-3**, the `LodRetentionMs` 10,000-vs-20,000 comment fix.

**Closes when** one standard leg per arm has run with the counters on, plus one
stats-off leg proving the instrument is not the perturbation, and a written
verdict on §1a exists. **Pivot if** `QueueEmpty` is the dominant `DrainResults`
exit — then the consumer is not the wall and this whole re-sequencing is aimed at
the wrong half of the pipeline.

### Wave S1 — Make the consumer cheap ← **HARD STOP AFTER THIS WAVE**

**S1-1 — T1-1, batch pool publication.** Ships alone, its own PR. Rename the
current `PushUpdatesToProxy` body to `FlushUpdatesToProxy()`; make
`PushUpdatesToProxy()` a no-op while `BatchDepth > 0`; add a public RAII
`FScopedBatch` flushing on the outermost close. In `TickStreaming`, one scope
around the block containing `DrainResults`, `DrainGameThreadMesh` and
`DrainUnloads` — **not** around `DispatchJobs`, **not** across the whole tick.
`UpdateBounds()` / `MarkRenderTransformDirty()` move to the flush.
*Gate:* `voxel.Stream.PoolBatchPublish` (0).

The hazard is unchanged from the plan and is the only dangerous change in the
wave: batching creates a **free-then-reallocate-within-one-frame race**.
`VoxelGpuPoolAddWritePasses` runs first — that ordering is D1's whole correctness
argument — and the merged `DirtyQuadRanges` upload runs after, so a range freed
and re-issued to a GPU-meshed chunk in the same frame gets stale hidden ids
written over fresh geometry. The symptom is invisible terrain that reports as
loaded. Fix as the plan specifies: `UnmarkQuadsDirty(First, Count)`, the interval
*subtract* mirroring `MarkQuadsDirty`'s insert-and-coalesce, called from
`AddChunkFromGpu` right after a successful `Pool.Alloc`, counted as
`PoolDirtyOverlapsResolved`.

Also audit both bail-out branches (`LiveProxy == nullptr`,
`ChunkOrigins.Num() > GetMaxChunks()`) — they `Reset()` the dirty state and call
`FlushGpuWritesStandalone`, and must stay correct with a whole frame's mutations
pending. And decide **explicitly** whether the water pool (a second
`UVoxelGpuPoolComponent` instance, re-meshing at 10 Hz) is batched too. Do not
leave it accidental.

*Expectation setting:* this does **not** explain the 39–45 ms `renderMs` during
the pinned linger. `RebuildRunBounds` cannot run when nothing is applying. That
is §2e draw-path cost and belongs to T3-4/T3-5. Do not credit this wave with it.

**S1-2 — Handle recycling (new).** `TArray<int32> FreeHandles`;
`RemoveChunkInternal` pushes, `AddChunk`/`AddChunkFromGpu` pop and assign in
place. `AllocationChunkIds` stays parallel. Removes the cumulative term from
`BuildChunkRuns`. *Gate:* `voxel.Stream.PoolRecycleHandles` (0). *Measured:*
`Allocations.Num()` plateaus at ≈ live chunks; per-window `avgChunks/s` stops
decaying. *Falsified if* S0-2 showed no decay.

**S1-3 — ~~T1-3(a), column-keyed params cache~~. STRUCK BY S0. DO NOT BUILD.**
The argument for putting it here was that speculation multiplies the
`SampleChunkParamsForPool` call count. Measured across five legs, `params` is
**0.002–0.004 ms per apply — 0.2%**, at every point in fill and flight. A cache
would remove nothing, and 0.2% multiplied by a lead factor is still 0.2%.
`pack` (0.003–0.005 ms) is the same story. Recorded in the measurements file
under FALSIFIED so it is not re-proposed.

*Kept from the attempt:* the hoist of `SampleChunkParamsForPool` out of its two
call sites in `ApplyMeshResult`. It is behaviour-identical — still computed on
exactly the branches that computed it before, never on the `UpdateChunk` path —
and it is what makes the number readable at all.

**S1-4 — T1-2(a) + T1-2(c), conditional.** Build only if S0-1's render-thread
accumulator shows `RebuildRunBounds` still costing >1 ms/frame after S1-1 and
S1-2. Detail is in the implementation plan. **Skip T1-2(b)** — S1-2 already
removes the walk's growth term, and (b) is a parallel-array invariant maintained
on the render thread in the file that has already shipped a "perfectly correct
cull selects the wrong geometry" bug. The `voxel.Stream.PoolRunsVerify` harness
is mandatory, not optional.

**S1-5 — Measure the live GPU dispatch ceiling.** Not a code change. Drive
demand until `GpuJobsPending.Num()` approaches its 256 cap and read sustained
delivered chunks/s; cross-check against `voxel.GPU.VerifyAsyncMesh 64 8`'s own
throughput line at the shipped `MeshBatchCap 4` / `MeshHarvestCap 8`. Record as
`LiveGpuCeilingPerSec`. **That number minus the demand rate is the speculative
budget.**

**Closes when** the head-to-head is re-run — old stack (`voxel.Stream.GPU 0`,
`JobsInFlightPerCore=2`, CPU mesher) vs the full shipped stack, ×2 legs each, at
the adopted cascade — and the pooled arm is at or past **1,082 chunks/s** with
`holes(final) = 0`. Then **stop and report** either way; a negative result is a
deliverable.

### Wave S2 — RESHAPED BY S1's RESULT. Read this before the item list.

Three things changed under this wave while S1 ran:

**S2-4 already shipped, early and for a different reason.** The pool is at 64M
and the table at 81,920 — not to make room for parking, but because batching
outran both and started refusing allocations. Whether 64M is *more* than parking
needs is now an open control (§Sizing above).

**A NEW ITEM GOES FIRST, AND IT BLOCKS T4-1: fix the admission cutoff.**
`RecomputeDesiredSet` relaxes every `LevelAdmissionCutoffDistSq` to `DBL_MAX`
when `PendingJobNum() * 4 < Cap * 3`. That test means "the queue is short, so we
can afford to admit more" — which was true when the queue was short only because
the pipeline was starved. Now the queue is short because the consumer is *fast*,
so the cutoff is permanently off and admission floods: 22,300 records/s against
~800 chunks/s loading, `tracked` 92,875, and `RecomputeDesiredSet` at 66% of the
tick.

  This is the whole reason T4-1 cannot simply be built next. Speculation adds
  admission work by construction — S4-1 enumerates keys demand has not asked
  for. Stacking that on a flooding admission path repeats the exact mistake this
  re-sequencing exists to avoid.

  **S2-0 — ATTEMPTED AND IT DOES NOT BIND. See the correction below before
  re-attempting.** `voxel.Stream.AdmissionRecordCap` gates the relaxation at the
  TOP of `RecomputeDesiredSet`, but `TruncatePendingJobQueue` runs at the END of
  the same call and `DropFarthestOverCap` sets `OutCutoffDistSq = DBL_MAX`
  whenever a level's queue is not full — so the cutoff is re-relaxed immediately
  and the cap is a no-op. Measured: `peakTracked` 96,657 (off) vs 96,287 (cap
  52,000), `recordsAdded` unchanged. A real fix has to gate BOTH sites.

  **And it is low priority**: see the corrected share above. The cvar is left in
  place, default 0, documented as ineffective rather than quietly removed.

  ~~Make the relaxation condition mean what it was meant to mean.~~ The
  intent is "there is spare capacity downstream", and `PendingJobNum()` was a
  proxy for that which only worked while the consumer was the bottleneck. Candidates:
  gate on `tracked` against a residency target, on the apply loop's *exit reason*
  (S0-1's counters already distinguish queue-empty from budget-bound), or on
  admitted-vs-loaded rate. Measure `tracked`, `recordsAdded`, recompute ms and
  chunks/s; it must cut the flood without starving the leading edge.
  *Gate:* its own cvar, default = today's behaviour.

**The budget lesson generalises, and T4-1 will trip it again.**
`MaxUnloadsPerFrame=24` was correct for 260 chunks/s and became the binding
constraint at 631 — costing 77,290 refused allocations and looking exactly like
allocator fragmentation. Every per-frame budget in this subsystem was tuned
against a slower pipeline: `MaxAppliesPerFrame`, `MaxRemeshesPerFrame`,
`kMaxResultDrainsPerFrame`, `kMaxUnloadPopsPerFrame`, `GpuMeshInFlight`,
`MeshBatchCap`, `MeshHarvestCap`.

  **Any wave that changes throughput must re-sweep the budgets downstream of it,
  and a saturated budget can present as a bug in something else entirely.**
  T4-1 changes throughput by construction. Budget-sweep before diagnosing.

### Wave S2 — Parking, capacity, and the debt it creates

Ships and is measured **on its own**, before any speculation exists: re-admit
becomes O(1), so ring oscillation under motion should get cheaper by itself.

**S2-1 — T1-4, GPU hide pass.** Keep the id stamp (the review's form is unsound;
the repoint is what makes the id safe to recycle). Replace the per-quad shadow
loop with `FMemory::Memzero`, drop the `MarkQuadsDirty`, enqueue
`FPendingGpuHide {First, Count}` drained by the flush into a new
`AddQuadPoolHidePass` — a sibling of `AddQuadPoolWritePass`, which already writes
ids from a scalar `ChunkId`. Same graph, same command, before the table update.
*Gate:* `voxel.Stream.PoolGpuHide` (0). Needs a **forced probe** — allocate, free,
re-allocate a *smaller* range inside the freed block, read back the tail's ids
(ground rule 13).

**S2-2 — T1-7, un-strand pool-refusal chunks.** Four of `ApplyMeshResult`'s five
`return false` exits leave the record settled-less, not in flight, and not
re-queued; `AddCandidate`'s "already tracked" early return then never re-admits.
This plan deliberately raises pool pressure, so it must be fixed first. Retry
queue, **cap at 3**, count `PoolRefusalRetries` / `PoolRefusalGaveUp`. *Measured
by forcing it:* `-VoxelPoolCapacityQuads=20000000` with and without the flag.
Land it regardless of what the perf numbers say.

**S2-3 — Pool-range parking.** Component: `ParkChunk(Handle)` /
`UnparkChunk(Handle, Origin, Level, Params)`, both O(1) with no quad traffic
(§2.3). Subsystem: `TMap<FVoxelLevelChunkKey, FParkedGeometry>` +
LRU; `ReleaseChunkGeometry` parks under the cap; the entry pass consults the map
before dispatching. **Invalidation is the correctness surface** —
`MarkChunkDirtyForRemesh` and `PropagateEditToMips` must evict parked entries
(§2.5), and a generation/edit-epoch mismatch on unpark evicts rather than draws.
*Gate:* `voxel.Stream.PoolParkMax` (0). **Hard abort if** `GetFreeRunCount()`
climbs while `GetFreeQuads()` stays healthy, `poolPct` crosses ~90%, or
`allocFail` goes non-zero: `UpdateChunk`'s realloc path *deletes resident
terrain* on a full pool.

**S2-4 — Capacity step 1: 44M → 64M quads, table 49,152 → 81,920.**
`SetChunkTableCapacity` **must** be called before the proxy exists — `MaxChunks`
is frozen at proxy construction and crossing it takes the `MarkRenderStateDirty`
branch, which per D4-R1 invalidates every GPU-written range.

**S2-5 — Drop the CPU shadow on the GPU-only arm.** With S2-1 shipped nothing on
that path writes `PooledQuads` or `QuadChunkIds`. `-VoxelPoolCpuShadow=0`
(command line, not a cvar — it must be decided before streaming begins), under
which `InitPool` sizes both to zero and `AddChunk`/`UpdateChunk` refuse loudly.
The water pool keeps its shadow. `-VoxelNoGpuMesh` must force it back on. This is
what pays for §2.6.

### Wave S3 — Velocity (direction only)

Finite-difference `LastAnchorLocation` against the incoming `Anchor` over
`DeltaTime`, EMA-smooth over ~0.25 s, expose `SmoothedAnchorVelocity` and
`PredictedAnchor = Anchor + Clamp(SmoothedAnchorVelocity * LeadSec, MaxLeadUU)`.
**Not `Pawn->GetVelocity()`** (§2.4) — put that reason in the code comment, or
someone will "simplify" it back.

Deliberately **excludes** T2-1's admission rewiring: that is the risky half
(evicting against a forward-shifted centre deletes ground *behind* the camera
that is still on screen; a raw signal re-arms the entry-scan crossing gate and
reproduces the measured refill-churn pathology), and T4-1 does not need it.

*Gates:* `voxel.Stream.VelocityLeadSec` (0.0), `voxel.Stream.VelocityLeadMaxUU`
(3000). *Closes when* a `line` leg logs a sustained ~2,000 UU/s smoothed velocity
on the correct heading. **If it logs zero, stop** — nothing downstream is
measurable.

### Wave S4 — T4-1. What S1 changed about how to build it.

**The headroom argument is now measured in-engine, and it holds.** S0's latency
split: the GPU finishes a chunk in ~12 ms (`dispatchToReady` p50) and the job
waits ~128 ms to be picked up (`queued` p50), with the fork sitting at ~11 jobs
in flight against a cap of 256. The GPU is idle waiting for the pipeline, not
the reverse. That is the real basis for T4-1 — **not** the 92,000 chunks/s bench
figure, which does not transfer (§2.1).

**Four design constraints S1 produced, all of which change S4:**

1. **Enumeration must be cheap, because admission is the bottleneck.** S4-1 was
   specified as "reuse the existing footprint machinery" for correctness reasons.
   It is now also a *cost* requirement: `RecomputeDesiredSet` is 66% of the tick
   before speculation adds anything. If S2-0 does not fix the flood, T4-2 comes
   first — do not build a speculative enumerator onto a path that is already
   drowning.

2. **Parked + speculative geometry needs a hard cap AND SEPARATE caps.** Built
   that way: `SpeculativeMaxParked` is distinct from `PoolParkMax` because demand
   parking caches geometry that *was* wanted and speculation caches geometry that
   *might* be. A shared cap would let speculation starve the demand cache it
   depends on. Measured: residency expands to fill whatever capacity
   exists when the drain lags — 80,716 chunks against an 81,920 table floor, with
   refusals — and it looked like allocator fragmentation. Speculative geometry is
   *by definition* geometry nothing is asking for, so it has no natural back
   pressure at all. `SpeculativeMaxParked` is not a tuning knob, it is the thing
   standing between this feature and a pool that refuses demand allocations.

3. **`specHitRate` is NOT the deciding metric. This was wrong in the original
   plan and S2-3 proved it wrong.** Parking scored an **89% hit rate and read as
   a 33% throughput regression** — and the regression was an artefact of
   `chunksPerSec` not counting adopted chunks, so the true answer was "slightly
   better than baseline". A cache can be almost perfectly effective and still
   cost more than it saves, AND it can be cheap and still look expensive if the
   metric cannot see its output.

   **The decision metric is placed-chunks/s (meshed + adopted) and flight-phase
   holes.** Hit rate says whether the cone was aimed correctly, which is a
   genuinely useful diagnostic and a different question.

4. **Re-sweep the budgets after it lands** (see the Wave S2 note). T4-1 changes
   throughput; the last two things that did each moved the bottleneck somewhere
   nobody was looking.

**And the target is now flight-phase holes specifically.** Converged
`holes = 0` is already achieved without any speculation (S1 close). So T4-1 is
no longer competing for permanent coverage — it is aimed squarely at the
transient leading-edge gap under motion, which is what a player actually sees
and what the ~854–907 flight-phase median measured. Re-measure that median at
the S1 config first: it may already have moved, and T4-1's win has to be stated
against the *current* number, not the pre-S1 one.

**S4-1 — Enumeration.** New per-tick hook in `TickStreaming` between the
recompute gate and `DispatchJobs`; `RecomputeDesiredSet` runs only on a crossing
(3.2 m at L0) so speculation cannot hang off it. From `PredictedAnchor`,
enumerate keys not in `ChunkRecords`, not in the parked registry, admitted by the
entry pass's own annulus predicate at the predicted anchor but not at the true
one. **Reuse the existing footprint machinery** rather than writing a second
enumerator — this repo's recurring failure mode is two copies of one calibration
drifting apart. **Level 0 only to start**
(`voxel.Stream.SpeculativeMaxLevel`, 0). Own queue, **not**
`PendingJobKeysByLevel` — sharing it risks the admission-churn pathology already
measured at ~237,600 rejections/s.

**S4-2 — Submission that cannot starve demand.** Submit at the **end** of
`DispatchJobs`, after the demand loop has taken what it wants, against a separate
sub-budget carved out of the 256-slot `GpuJobsPending` cap and sized from S1-5.
Mark with a bit in `Submit`'s `UserTag` — currently always 0, round-tripped
untouched to `OnGpuMeshJobComplete`. **Zero new plumbing in the manager.**

Deliberately not doing: per-job cancellation and priority. No `Cancel(JobId)`
exists (only `CancelAll`), the promote loop is strict FIFO, and adding either
risks the exactly-one-outcome invariant the manager's header documents.
Submitting last plus a small sub-budget achieves the same end. A superseded
speculative job runs to completion and is parked or dropped — it cost one
dispatch out of idle capacity.

**S4-3 — Park on arrival, adopt on admission.** A result carrying the speculative
bit routes to `ParkSpeculativeResult`: `AddChunkFromGpu`, then **immediately**
`ParkChunk`, then register in the S2-3 map with the current
`GenerationId`/`EditEpoch`. **No `FChunkRecord` is created** — the record is what
admission owns. `AddCandidate` consults the registry before queueing; on a
matching hit it creates the record with `PoolSlot` set and `bMeshSettled = true`,
unparks, and does **not** queue a job. On a mismatch it evicts and falls through.
Preserve the two deliberate non-settle exits in `ApplyMeshResult`: a chunk whose
speculative allocation failed must not release a retained stand-in.

**S4-4 — Budget, and capacity step 2.** One **shared non-demand residency
budget** over parked + speculative, not two independent caps — they draw on the
same pool and the same chunk table, and overflowing either is severe. Eviction
priority: recently-parked-from-eviction > near speculative > far speculative.
Capacity step 2 lands here: **64M → 104M quads**, table **81,920 → 131,072**.

*Gates:* `voxel.Stream.SpeculativeLeadSec` (0.0 = the whole feature off),
`SpeculativeMaxInFlight`, `SpeculativeMaxParked`, `SpeculativeMaxLevel` (0).

*New counters:* `specDispatched`, `specParked`, `specAdopted`,
`specEvictedUnused`, `specStaleOnAdopt`, and
**`specHitRate = adopted / (adopted + evictedUnused)`**. A hit rate under ~50%
means the velocity cone is wrong and the work is waste heat. It is the single
number that says whether this idea is working.

**Closes when** flight-phase transient holes on the GPU arm fall from the
**854–907 median** toward the CPU arm's **55**, with `holes(final) = 0`,
`allocFail = 0`, `GetFreeRunCount()` stable, and demand-arm `avgChunks/s` no
worse than the S1 close. *Sweep* `SpeculativeLeadSec` 0/1/2/4/8, two legs each.

---

## 5. Verification

Per-item gates are above. End to end:

1. **Correctness, every wave touching geometry:** `voxel.GPU.VerifyPoolWrite 16 4`
   (direct **and** the `MeshDirectToPool 0` control), `voxel.GPU.VerifyAsyncMesh
   64 8`, `voxel.Stream.PoolClobberTest`, `voxel.Stream.CoverageVerify`, plus
   `ctest --test-dir build/voxel-core` and the `VoxelEarth.GpuPool` automation
   tests. **`voxel.GPU.VerifyRegion` against digest `6e893ab3679a8c81` is NOT
   needed** — nothing in S0–S4 touches `worldgen.ush` or the emit path. Say so in
   the PR rather than running it reflexively.
2. **Throughput and coverage:** the standard leg through
   `tools/voxel-run-leg.ps1`, ≥2 legs per arm, alternating:
   ```
   UnrealEditor-Cmd.exe <uproject> -game -nosplash -unattended -sm6 -dx12 \
     '-VoxelSpawnAt=-84480,53760' -VoxelPerfRun=120 -VoxelPerfFlight=line \
     -VoxelPerfPreflightSec=90 -VoxelPerfLingerSec=60 -VoxelPerfLogInterval=2 \
     -ExecCmds="voxel.Stream.CoverageVerify 1"
   ```
   The log must say `INSIDE the loaded tile coverage` or the leg ran on a flat
   fallback plane and is worthless. Clear `Saved/VoxelWorlds/*.vxlog` before any
   cold-fill leg. Treat `postWarmupMaxFrameMs` of exactly `400.000` as censored.
3. **Thresholds:** `tools/check-perf-run.py` against `Saved/PerfRuns/perf_*.json`.
4. **Screenshot diffs** where shading or geometry selection changes (S1-3, S1-4,
   S2-3), against a same-run floor — a pooled primitive fails silently
   (ground rule 4).

### Which gates apply where

| Change | VerifyPoolWrite | CoverageVerify | PoolClobberTest | forced probe | screenshot diff |
|---|---|---|---|---|---|
| S1-1 batch publish | **yes** (+control) | yes | yes | — | — |
| S1-2 handle recycling | yes | yes | yes | — | — |
| S1-3 params cache | — | — | — | — | **yes** (shading input) |
| S1-4 incremental table/bounds | yes | yes | yes | — | yes (cull can hide geometry) |
| S2-1 GPU hide pass | **yes** | yes | — | **yes**, partial-realloc | yes |
| S2-2 refusal retry | — | **yes, forced saturation** | — | **yes**, tiny pool | — |
| S2-3 parking | yes | yes | yes | — | yes |
| S4-3 speculative adopt | yes | **yes, flight + converged** | yes | — | yes |

Every wave's result — **including negative ones** — goes to `docs/measurements/`
in the existing format, and the falsified-lever list is extended.

**Do not re-propose the falsified levers:** fork caps 16/32, `GpuMeshInFlight`
1024, `DispatchAfterDrain` (default is **0**, and that is a measurement),
`AdmissionBandSkip`, ring cross-fade (closed do-not-build), slot-floor sweeps,
ring-major dispatch.
