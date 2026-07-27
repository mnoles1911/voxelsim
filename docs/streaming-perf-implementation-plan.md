# Streaming-perf implementation plan — executing the 2026-07-27 deep review

**Status:** written 2026-07-27, not yet started. Execution brief for a fresh
session.

## How to use this document

Read `docs/deep-review-streaming-perf-2026-07-27.md` first — this plan executes
it and refers to its section numbers (§1a, §2c, T0-1 … T4-5) throughout without
restating them. Then read the two measurement files it rests on:
`docs/measurements/gpu-throughput-wave-2026-07-27.txt` (the P0 and the falsified
levers) and `docs/measurements/ring-gap-2026-07-27.txt` (the standard leg and the
settle rules). Ground rules are `docs/gpu-waves-plan.md:88-200`.

Work top to bottom. **Wave 0 and Wave 1 are the committed work**; stop and
re-plan after Wave 1 closes, because its result decides whether Waves 2–4 are the
right shape at all. Each item states what changes, how it is gated, what measures
it, and what would falsify it — the falsifier is not decoration, it is the exit
condition that stops a dead lever eating a session.

### Critical files

| File | What lives there |
|---|---|
| `ue-project/Source/VoxelEarthShaders/Private/VoxelGpuPoolComponent.cpp` | `PushUpdatesToProxy`, `BuildChunkRuns`, `MarkQuadsDirty`, `AddChunk`/`AddChunkFromGpu`/`RemoveChunkInternal`/`UpdateChunk`, and the proxy's `UpdateChunkTable_RenderThread` / `RebuildRunBounds` / `ComputeRunBounds` / `BuildCulledRanges` / `GetDynamicMeshElements`. T1-1, T1-2, T1-4, T2-4, T3-4, T3-5 all land here. |
| `ue-project/Source/VoxelEarthShaders/Public/VoxelGpuPoolComponent.h` | `FChunkRun`, `FPendingGpuWrite`, `FDirtyRange`, `PooledQuads`/`QuadChunkIds`/`ChunkOrigins`/`ChunkParams`/`FreeChunkIds` — and the doc comments encoding the id-recycling and write-ordering invariants T1-1 and T1-4 must not break. Read those comments before touching either. |
| `ue-project/Source/VoxelEarth/VoxelWorldSubsystem.cpp` (~12,600 lines) | `TickStreaming`, `DrainResults`, `DrainUnloads`, `ApplyMeshResult`, `SampleChunkParamsForPool`, `RecomputeDesiredSet`, `SortPendingQueues`, `DispatchJobs` (cold-band throttle), `ReplacementCovered`, `MaybeLogCounters`. T0-1, T0-2, T1-3, T1-7, T2-1, T2-2, T2-3, T2-6. |
| `ue-project/Source/VoxelEarth/VoxelDebug.cpp` / `.h` | Every `voxel.*` CVar and its measurement history. All new gates register here. |
| `ue-project/Source/VoxelEarthShaders/Private/VoxelGpuMeshJobManager.cpp` / `Public/…h` | `Tick`, `DispatchBatch`, `PollInFlight`, `Deliver`, `FVoxelGpuMeshJobResult` timing fields. T0-2, T1-5, T1-6, T3-1, T3-2. |
| `tools/voxel-run-leg.ps1` | The only sanctioned leg driver. Every closing measurement goes through it. |

---

## Context

`docs/deep-review-streaming-perf-2026-07-27.md` (PR #162) is a four-track code
review of everything that meshes, renders, culls, loads and unloads the voxel
cascade. It exists to answer the open P0 at the end of
`docs/measurements/gpu-throughput-wave-2026-07-27.txt`:

> At the adopted 128 m / 4 km cascade the pooled-renderer arms plateau at
> ~600 chunks/s regardless of mesher (fork on 602, fork off 593), apply budget
> only ~8.5% saturated, while the old component-renderer arm posts ~1,080/s and
> converges holes to 0. The pooled arms end the standard leg with ~15,000 holes.

Every producer-side lever swept so far has measured dead — fork batch caps,
pipeline depth, dispatch cadence, mesher choice. The review's contribution is a
code-read explanation of *why* they were all dead, plus 28 ranked ideas. This
plan turns those 28 into buildable, measurable work.

**Independently confirmed before writing this plan** (two separate reads, so the
first wave is not resting on the review's word alone): `AddChunk`,
`AddChunkFromGpu` and `RemoveChunkInternal` each end with their own
`PushUpdatesToProxy()` call. That function copies `ChunkOrigins` and `ChunkParams`
whole, calls `BuildChunkRuns()` (O(N) walk plus a sort), and enqueues a render
command whose `UpdateChunkTable_RenderThread` re-uploads the whole table and then
runs `RebuildRunBounds()` — an `FBox::TransformBy` per run over every resident
chunk. At ~39,020 resident chunks that is an O(N) tax on *both* threads for *every
single chunk applied*, and it is paid again on every unload. Producer-side knobs
cannot move a consumer-side per-item O(N) cost, which is exactly why the whole
sweep came back dead.

**Intended outcome:** the pooled arm reaches or passes the component arm's
~1,080 chunks/s with converged `holes=0` at the adopted cascade. At that point the
component renderer stops being the benchmark the GPU stack loses to, and the
already-designed throughput multipliers (tile batching) become worth building.

## Working rules that apply to all 28 items

These are not new; they are the existing project discipline, restated because
every item below inherits them.

- **Gate everything.** Each behaviour change gets a `voxel.*` CVar or `-Voxel*`
  switch registered in `VoxelEarth/VoxelDebug.cpp`/`.h`, **defaulting to today's
  behaviour** until a leg justifies the flip. The flip is a separate, one-line
  commit whose message carries the measurement.
- **The standard leg:**

  ```
  UnrealEditor-Cmd.exe <uproject> -game -nosplash -unattended -sm6 -dx12 \
    '-VoxelSpawnAt=-84480,53760' -VoxelPerfRun=120 -VoxelPerfFlight=line \
    -VoxelPerfPreflightSec=90 -VoxelPerfLingerSec=60 -VoxelPerfLogInterval=2 \
    -ExecCmds="voxel.Stream.CoverageVerify 1"
  ```

  The spawn must be `-84480,53760` and the log must say `INSIDE the loaded tile
  coverage`, or the leg ran on a flat fallback plane and is worthless. `line`
  flight is the only mode that enters virgin terrain. `-VoxelNoGpuMesh` selects
  the CPU control arm. For any P0 claim also run the **component-renderer arm**
  (`voxel.Stream.GPU 0`, `JobsInFlightPerCore=2`) — that is the 1,082/s number the
  plateau is measured against.
- **Drive legs through `tools/voxel-run-leg.ps1`**, not a hand-written wait loop.
  Its settle rule (`loaded` unchanged for 3 consecutive samples *and*
  `jobsInFlight=0` *and* `pendingJobs=0`) is what stops a mid-fill lull being
  reported as a finish; bypassing it has produced two retracted results already.
  Clear `Saved/VoxelWorlds/*.vxlog` before any cold-fill leg.
- **Two legs minimum per arm**, alternated; the within-config spread is the noise
  floor. Discard the first run after a build (cold PSO). Frame times are clamped
  at 400 ms — read `postWarmupMaxFrameMs` and treat exactly `400.000` as censored
  data. Full list: `docs/gpu-waves-plan.md:88-200` (14 ground rules).
- **Correctness gates that already exist**, re-run by any item that can corrupt
  geometry: `voxel.GPU.VerifyPoolWrite K N`, `voxel.GPU.VerifyAsyncMesh K N`,
  `voxel.GPU.VerifyRegion` (determinism digest pinned at `6e893ab3679a8c81`),
  `voxel.Stream.CoverageVerify`, `voxel.Stream.PoolClobberTest`. The digest only
  needs re-running for waves that touch `worldgen.ush` or the emit path — T3-1 and
  T3-2 and nothing in T0/T1/T2. Say so in the PR rather than running it reflexively.
- **Do not re-propose the falsified levers**: fork caps 16/32, `GpuMeshInFlight`
  1024, `DispatchAfterDrain`, `AdmissionBandSkip`, ring cross-fade (closed
  do-not-build), slot-floor sweeps, ring-major dispatch.
- **Cite symbols, not line numbers**, in code comments and PR bodies (ground rule 12).

## Where this plan departs from the review

A second code read agreed with §1a but found six items that would cost a wave if
taken at face value. Each is argued at the item itself; collected here so they are
not missed.

| Item | Review says | This plan does |
|---|---|---|
| T3-3 | D6 band-reduce is "built, gated — turn it on" | It is already **on unconditionally**; no gate exists. Struck. |
| T2-5 | Build a RAM mesh cache | **Do not build** — quads never reach the CPU under the shipped `MeshDirectToPool` default. |
| T1-4 | Drop the per-quad id stamp, zero the table entry | **Unsound** — breaks id recycling on a partial realloc. Keep the stamp, move it to a GPU hide pass. |
| T1-2 | O(N) per chunk → O(log N) | Search is O(log N); the array memmove is still O(N). ~40× cheaper, not asymptotically free. |
| T2-2 | Pad the inner admission edge | Pad **both** edges with hysteresis, or it churns. |
| T4-5 | Decide with `-VoxelL0GridCacheProbe` | Wrong cache. The verdict is available by reading — just delete it. |

Two more worth stating plainly: the "8.5% saturated" figure is even weaker
evidence than the review credits (it is published as a mean of three ratios
against three different ceilings, so with re-meshes near zero it reads roughly a
third of the true apply ratio); and **T1-1 must not be credited with the static
40 ms frames** — those happen during a pinned linger with nothing streaming, so
they are §2e's draw-path cost and belong to T3-4/T3-5.

## Sequencing at a glance

| wave | items | ships as | closed by |
|---|---|---|---|
| 0 | T0-1..3 | one PR, instrumentation only | a written verdict on §1a |
| 1a | **T1-1 alone** | its own PR | GPU arm ≥ ~900 chunks/s, `allocFail`/dropped writes 0 |
| 1b | T1-2, T1-3 | one PR, three sub-gates | table-push cost ≈ 0, chunks/s not worse |
| 1c | T1-4, T1-7, §2c hazards | one PR | forced-probe gates green, unload bytes down |
| 2 | T2-1..7 | one PR per sub-theme, not one big one | flight-phase transient holes → toward 55 |
| 3 | T3-1..6 | separate PRs, docs' own order | each on its own measured claim |
| 4 | T4-1..5 | design note each, before code | re-decided after Wave 1/2 land |

**T1-1 ships alone, before T1-2** — a deliberate departure from the review's
sequencing. It is the largest single lever, it carries the only new corruption
hazard in the tier, and if it alone closes the 600→1,080 gap then T1-2's
incremental-runs work (a parallel-array invariant maintained on the render thread,
in the file that has already shipped a "perfectly correct cull selects the wrong
geometry" bug) may not be worth its risk. That information costs one leg set.

Waves 0 and 1 are the committed work. **Stop and re-plan after Wave 1 closes** —
if the plateau shuts, T4-2 may be unnecessary and T4-1 becomes the highest-value
item on the board; if it does not, the diagnosis is wrong and Waves 2–4 are all
premature. Do not pre-commit to the back half.

---

## Wave 0 — Instrument first (T0, hours, one leg)

Nothing is built in this wave. Its whole purpose is to confirm or kill §1 of the
review cheaply, because the ring-gap night's first lesson is that a hypothesis is
cheaper to kill with instrumentation than to fix blind.

**T0-1 — Split `DrainResults` exit reasons and per-apply stage timings.**
In `FVoxelWorldImpl::DrainResults` count which exit fired — queue-empty,
`ApplyBudgetMs` wall clock, `MaxAppliesPerFrame` count cap, drain cap — as four
counters beside the existing `ResultsDrainedSinceLog` / `StaleDiscardsSinceLog`.
In `ApplyMeshResult`, accumulate per-stage wall clock into four buckets next to
the existing `AccumApplyMs` family: quad pack, `SampleChunkParamsForPool`,
`Pool->AddChunk`/`AddChunkFromGpu`, and the table push. Report as one new
`Voxel apply stages (5s window):` line in `MaybeLogCounters`, same shape as the
existing retention line.
On the pool side add `FPoolPushStats { Pushes; RunsBuilt; TableCopyMs; BuildRunsMs; }`
and `GetAndResetPushStats()`, instrumented around the `OriginsCopy`/`ParamsCopy`
assignment and the `BuildChunkRuns()` call. For the render-thread half, accumulate
cycles around `RebuildRunBounds()` into a `FThreadSafeCounter64` on
`FVoxelGpuPoolBuffers` — that holder exists precisely for cross-thread counters
that outlive the component (`DroppedWrites` is the precedent) — and drain it from
the game thread.
*Gate:* `voxel.Stream.ApplyStageStats` (default 0). The exit-reason counters are
unconditional (four increments); only the `FPlatformTime::Seconds()` pairs are
gated, so the instrument cannot be what is being measured.
*Measured:* standard leg ×2 GPU, ×2 CPU, adopted cascade, stats on — plus one
stats-off leg to prove the instrument is not the perturbation (`avgChunks/s`
within noise).
*Falsifies §1a — any one of:* `QueueEmpty` is the dominant exit (results genuinely
are not arriving, and the whole T1 wave is aimed at the wrong half of the
pipeline — pivot to the producer); table-push plus `RebuildRunBounds` is under
30% of per-apply cost; or `PoolAddMs` per apply does not scale with
`ChunkRecords.Num()` across the 90 s preflight.

**T0-2 — Submit→apply latency histogram per producer, per stage.**
`FVoxelGpuMeshJobResult` already carries `DispatchToReadyMs` and
`SubmitToDeliverMs`. Add `QueuedMs` (Submit → promoted out of `Queued` in `Tick`)
and `ReadyToDeliverMs` (first `IsReady` → `Deliver`); both stamps already exist
(`FJob::ReadySeconds`, the promote loop). On the streaming side stamp a delivery
time in `OnGpuMeshJobComplete` and compute `DeliverToApplyMs` at the apply; mirror
for the CPU worker arm. Store in the existing rolling-window idiom
(`WorkerJobMsWindow[256]`), one window per stage per producer, reporting p50/p95/max.
**Also log the quad-count distribution per level** — free here, and it is the input
T1-6's long form needs before it can be costed at all.
*Gate:* `voxel.Stream.LatencyStats` (default 0). *Expected:* `QueuedMs` dominates
(Little's law, §1d). If `DispatchToReadyMs` dominates instead, the GPU is the
limiter and T3-1/T3-2 move up the queue.

**T0-3 — Resolve the `LodRetentionMs` 10,000-vs-20,000 contradiction.**
`VoxelDebug.cpp`'s CVar documents the full 5000→20000→10000 retune and is correct;
the retention-census comment in `MaybeLogCounters` still says "its 20000 default".
Fix the comment to name 10,000 **and** to state that the zero-`capRel` result was
taken at 20,000 and is therefore not a prediction for the shipped default.
Comment-only; no gate, no leg. It matters because T2-6's acceptance bar is
flight-phase `capRel`, currently anchored to a number measured at a retention
value that no longer ships.

**Closes when:** one standard leg per arm with the counters on, and a written
verdict on whether §1a holds.

---

## Wave 1 — Close the plateau (T1)

### T1-1 — Batch pool publication once per frame ← *the wave*

Accumulate adds and removes across `DrainResults` and `DrainUnloads` and flush a
single publication at the end of the streaming tick instead of one per chunk. The
dirty-range machinery already accumulates correctly across multiple mutations
(`DirtyQuadRanges`, `MarkQuadsDirty`, `bChunkTableDirty`/`bRunsDirty`); only the
eager flush at the bottom of `AddChunk`/`AddChunkFromGpu`/`RemoveChunkInternal`
forces the per-chunk cost.

Rename the current body to `FlushUpdatesToProxy()`; make `PushUpdatesToProxy()` a
no-op while `BatchDepth > 0` (setting `bFlushPending`). Add a public RAII
`FScopedBatch` that flushes on the outermost close. In `TickStreaming`, open one
scope around the block containing `DrainResults`, `DrainGameThreadMesh` and
`DrainUnloads` — **not** around `DispatchJobs` (nothing there touches the pool)
and **not** across the whole tick (edit paths call `ApplyMeshResult` from
elsewhere and must keep their existing flush semantics). `UpdateBounds()` /
`MarkRenderTransformDirty()` move from per-add to the flush.

*Gate:* `voxel.Stream.PoolBatchPublish` (default 0 = byte-identical to today).

**The hazard, and it is the only genuinely dangerous change in the tier.**
Batching introduces a **free-then-reallocate-within-one-frame race**. Today each
add and remove flushes its own render command, so a free's hidden-id upload always
lands before a later add's GPU write. Batched into one command,
`VoxelGpuPoolAddWritePasses` runs **first** — that ordering is D1's whole
correctness argument — and the merged `DirtyQuadRanges` upload runs after, so a
range freed and re-issued to a GPU-meshed chunk in the same frame gets stale
hidden ids written *over* the freshly written geometry. The symptom is invisible
terrain that reports as loaded.
Design against it: add `UnmarkQuadsDirty(First, Count)` — the interval *subtract*
mirroring `MarkQuadsDirty`'s insert-and-coalesce — and call it from
`AddChunkFromGpu` immediately after a successful `Pool.Alloc`, over the allocated
range. For a GPU-written range the authoritative content is the pending compute
write; any dirty interval left over it is stale shadow content. Count the
subtractions in `PoolDirtyOverlapsResolved` and log it: a non-zero value on the
first leg proves the hazard is real and handled, and a zero value across a full
flight means either it cannot occur or the call is not wired — both worth knowing.

*Also audit:* the `LiveProxy == nullptr` and `GetMaxChunks()` overflow branches
both `Reset()` the dirty state and must stay correct with several chunks' worth
pending; and decide explicitly whether the water pool (same component, re-meshes
at 10 Hz) is batched too or left alone — do not leave it accidental.

*Measured:* correctness first and not optionally —
`voxel.GPU.VerifyPoolWrite 16 4` **with the flag on**, plus the
`MeshDirectToPool 0` control on the same binary; then
`voxel.Stream.PoolClobberTest`; then a `CoverageVerify` converged read at the
pre-change value. Then the standard leg ×2 GPU, ×2 CPU, plus the component arm.
*Deciding metric:* GPU-arm `avgChunks/s`, target ≥ 900. Secondary: `holes(final)`
falling from ~15.3k, hitches not worse.
*Falsified if* `avgChunks/s` moves < 15% **and** T0-1 said table-push was dominant
— that combination means the batching did not actually reduce flush count, so check
`Pushes` fell from ≈ applies+unloads to ≈ ticks. **Revert immediately** if
`GetGpuDirectWritesDropped()` or `allocFail` goes non-zero, or `holes(final)` rises.

*Expectation setting:* T1-1 does **not** explain the B3 baseline's 39–45 ms
`renderMs` during the pinned linger — `RebuildRunBounds` cannot run when nothing is
applying. That static-scene cost is §2e and belongs to T3-4/T3-5. Do not credit
this wave with it, or it will read as under-delivering when it has not.

### T1-2 — Incremental table / runs / bounds

Three independently gateable pieces; land them in this order in one PR, each with
its own CVar, so a leg can attribute them.

**(a) Sub-range chunk-table upload.** `ChunkOrigins`/`ChunkParams` are copied
whole and uploaded with a whole-buffer `LockBuffer`. Track touched entries in a
`TArray<FDirtyRange> DirtyChunkEntries` using the *same* insert-and-coalesce
helper as `MarkQuadsDirty` — factor it into a file-local
`InsertAndCoalesce(TArray<FDirtyRange>&, First, Last, Gap)` and have both call it;
do not write it twice. Chunk ids recycle LIFO so a frame's touched ids are
scattered — cap the run count (`kMaxEntryUploadRuns = 32`) and fall back to the
whole-table upload above it. *Gate:* `voxel.Stream.PoolTableIncremental` (default 0).

**(b) Incremental `Runs`.** Keep a persistent offset-sorted `TArray<FChunkRun>`
maintained by `Algo::LowerBound` on `FirstQuad` (unique per live allocation) plus
`Insert`/`RemoveAt`. Keep `BuildChunkRuns()` as the reference implementation for
the verifier and `DebugGetChunkRuns()`.
*Cost honesty:* only the search is O(log N) — these are flat `TArray`s, so each
edit is a binary search plus an O(N) memmove (~470 KB at 39k runs, ~25–50 µs).
Still ~40× cheaper than walk-plus-sort plus a full bounds rebuild, but "removes
the residency-scaling term entirely" is false as stated. If T0-1 shows the memmove
still binds, the fallback is a tombstoned array (leave `NumQuads == 0` entries in
place, compact past 25%) — do **not** build that speculatively.
*Gate:* `voxel.Stream.PoolRunsIncremental` (default 0).

**(c) Delta transport and incremental `RunBounds`.** Change the render-command
payload from `TArray<FChunkRun>` to `TArray<FRunDelta>` (`Insert`/`Remove` + run);
apply deltas to the proxy's `Runs` **and** the parallel `RunBounds`, calling
`ComputeRunBounds` once per insert. Three invariants, each `check()`ed:
1. `RunBounds.Num() == Runs.Num()` after every apply — that parallelism becomes
   load-bearing rather than incidental.
2. The delta apply is a private method called only from the same two sites as
   `RebuildRunBounds` (`CreateRenderThreadResources`, `UpdateChunkTable_RenderThread`),
   preserving the written-only-outside-a-gather contract. `RunBounds` is read by
   `GetDynamicMeshElements`, which runs **concurrently** across the camera and each
   shadow cascade; this is the one place in the wave where a mistake is a race.
3. **Full rebuild on transform change.** Compare `GetLocalToWorld()` against
   `RunBoundsLocalToWorld` and fall back to a full rebuild when they differ. The
   pool is re-based after `RegisterComponent`, and `ApplyWorldOffset` is a second
   path; either would otherwise leave every incrementally inserted box describing
   where chunks used to be.

**Verification harness — mandatory, not optional.** `voxel.Stream.PoolRunsVerify`
(default 0): after each delta apply, rebuild `Runs`/`RunBounds` from scratch and
byte-compare, logging the first divergence with op, index and both runs, then
latch. This is the `-VoxelCoarseGridVerify` pattern, and it converts "pure
refactor" from an argument into a count. Run a full clean leg before any flip.

*Measured:* on top of shipped T1-1, a four-point matrix `(0,0,0) / (1,0,0) /
(1,1,0) / (1,1,1)`, standard leg ×2 per arm. `TablePushGameMs` and the
render-thread `RebuildRunBounds` accumulator must both fall to ≈0 and
`avgChunks/s` must not fall. *Falsified if* it does not improve on top of T1-1 —
ship (a) only, the cheap low-risk third, and stop; batching already amortised the
residency-scaling term. Do not ship without a green `PoolRunsVerify` leg and a
`CoverageVerify` converged read at parity: a stale run does not merely fail to
draw its chunk, it points the cull at somebody else's quads.

### T1-3 — Kill the per-apply `Amplifier::column`

Two layers; build the cheap universal one first.

**(a) Column-keyed params cache.** `SampleChunkParamsForPool` is a pure function
of `(Level, Key.X, Key.Y)` — `Key.Z` never enters it, since `SurfaceZRelUU` is
just `SurfaceZUU − ChunkWorldOrigin.Z`, one subtraction off a cached absolute. So
cache `{ Temp, Precip, SurfaceZAbsUU }` keyed by level+XY and derive the relative
Z per chunk: one `GetSurfaceHeightUU` per **column** instead of per **chunk**, a
4–16× reduction from Z-stack depth alone, with no new plumbing. Prune it in
`PruneFootprintZRangeCache` alongside the footprint caches (same 2× unload-ring
rule). Hoist the per-call `GetSubsystem<UVoxelWorldSubsystem>()` out — pass the
impl in, it is `this`. Apply the same cache to `BuildChunkVertexData`'s
`ChunkSurfaceZUU` on the component path.
*Gate:* `voxel.Stream.ParamsColumnCache` (default 0).

**(b) Carry it in the result** — only if T0-1 says residual `ParamsMs` still
matters after (a). CPU worker arm: the job already has the 34×34 grid, so add
`SurfaceTopUU` plus two climate bytes to `FJobResult`. GPU fork arm: widen the
existing band readback from 2 ints to 3, carrying the **chunk-centre** column top,
**not** the band max — the band is a max over 34×34 and would change shading. The
all-or-none harvest covers the extra word for free. Level-0 only; L1–5 keep (a).
This is the "two copies of one calibration drifting apart" shape from the backlog
appendix, so add a 1-in-N debug compare against `SampleChunkParamsForPool`.
*Gate:* `voxel.Stream.CarrySurfaceInResult` (default 0).

*Measured:* `ParamsMs` must fall ≥80% at (a). Correctness is a **settled
pinned-pose screenshot diff** against a same-run floor, not a count — this changes
shading inputs. *Falsified if* the diff exceeds the same-run floor, or if
`ParamsMs` was already under 10% of per-apply (then skip (b) entirely).

### T1-4 — Move the unload's id stamp to the GPU

The review calls this "table-entry-only unload"; **its form as written is unsound.**
`RemoveChunkInternal` stamps `kHiddenChunkId` across the freed range in the CPU
shadow — O(quads) — and marks that range dirty, re-uploading ~12 B/quad. The review
proposes dropping the stamp and zeroing only the table entry. That breaks the
id-recycling invariant: the repoint is *precisely why* the chunk's own id becomes
unreferenced and safe to recycle through `FreeChunkIds`, and a **partially** reused
free block would leave a tail of quads naming a recycled id, which then draw at a
different chunk's origin.

**Keep the stamp; move it off the upload path.** Replace the per-quad shadow loop
with `FMemory::Memzero` (`kHiddenChunkId == 0`, so this is a real memset and keeps
the shadow correct), drop the `MarkQuadsDirty` call, and enqueue an
`FPendingGpuHide { First, Count }` drained by the flush into a new
`AddQuadPoolHidePass` — a trivial sibling of the existing `AddQuadPoolWritePass`,
recorded in the same graph, in the same command, before the table update, under the
ordering rule `FPendingGpuWrite` already documents. Where a hide and a same-frame
GPU write cover the same range, `AddChunkFromGpu` also subtracts from
`PendingGpuHides`; assert the two lists are disjoint afterwards.

*Gate:* `voxel.Stream.PoolGpuHide` (default 0).
*Measured:* `voxel.GPU.VerifyPoolWrite 16 4`, `PoolClobberTest`, a converged-`holes`
leg — **and a forced probe**, because the failure mode above will not occur
naturally: allocate, free, re-allocate a *smaller* range inside the freed block,
and read back the tail's ids, which must be `kHiddenChunkId`. Ground rule 13 — a
path with no recorded executions is untested. Deciding metric: `unloadMs` in the
hitch-attribution line and `PoolUploadStats` bytes/update.

### T1-5 — Decouple pacing from frame rate

Every stage is paced per tick, so throughput is proportional to frame rate and a
load storm that drops fps drops chunks/s everywhere at once. Add a small
`FRateBudget { double Accum; int32 Take(double PerSec, float Dt, int32 Ceiling); }`
(fractional carry, ceiling-clamped) and use it at `PollInFlight`'s `HarvestCap`,
`DrainUnloads`' `MaxUnloads`, `DrainGameThreadMesh`'s `MaxRemeshes`, and
`DrainResults`' `kMinAppliesPerFrame` floor.
**Explicitly not** `MeshBatchCap` (falsified: 602.3 vs 602.0) and **not**
`ApplyBudgetMs` (already wall-clock). Scope it to the four sites above and rank it
last in T1.
*Gate:* `voxel.Stream.RateBudgets` (default 0).
*Measured:* the discriminator is a **capped-fps leg** (`t.MaxFPS 30`) — the only
configuration where the coupling is visible as a difference. *Falsified if*
chunks/s at 30 fps does not improve relative to the uncapped ratio.
*This is not `DispatchAfterDrain`*, which was falsified: that added a second
same-frame dispatch, this makes existing caps time-based. Say so in the commit.

### T1-6 — Fences instead of polling — **defer out of Wave 1**

Completion is detected by a per-tick poll of `FRHIGPUBufferReadback::IsReady` and
delivered on the *next* game tick — a floor of ~2 poll quanta, ~25–50 ms. §1d's own
argument is that latency falls out of fixing throughput, and the long form
(pre-reserving pool space against `MaxQuads`) is not yet designable: the true bound
is 98,304 quads per chunk, which at 256 in flight is 25 M quads of reservation
against a 44 M pool. It becomes designable once T0-2 has logged the *actual*
per-level quad distribution — reserve p99, true up.
If a post-Wave-1 leg still shows two poll quanta, the cheap interim is a second
`PollInFlight` from an `FCoreDelegates::OnEndFrame` hook, gated
`voxel.GPU.MeshPollTwicePerFrame` (default 0) — worth at most half a frame; read
`ReadyToDeliverMs` from T0-2 to decide.

### T1-7 — Un-strand pool-refusal chunks (correctness)

`ApplyMeshResult` has four `return false` exits that leave the record settled-less,
not in flight and **not re-queued**: the `PoolCap` early return, the
`Rec.PoolSlot == INDEX_NONE` allocation failure, and two Error-logged collision
returns. The entry scan's "already tracked" early-return then never re-admits, so a
transient pool-pressure spike during fast flight becomes a standing hole until
eviction — the same family as the scan-before-park race fixed in `508a2fa`.
Add `PendingPoolRetry` plus a per-key retry count on `FChunkRecord`; push on
refusal, drain at the top of `DispatchJobs` back into `PendingJobKeysByLevel[Level]`
preserving `DistSq`. **Cap retries (3)** and count `PoolRefusalRetries` /
`PoolRefusalGaveUp` — an uncapped retry on a genuinely full pool is a livelock.
*Gate:* `voxel.Stream.PoolRefusalRetry` (default 0).
*Measured:* invisible on a healthy leg (`allocFail = 0`), so **force it** —
`-VoxelPoolCapacityQuads=20000000` with and without the flag, comparing converged
`holes`. *Falsified if* forced-saturation `holes` does not fall, or if
`PoolRefusalRetries` free-runs.
Land it regardless of what the perf numbers say; it is a correctness fix.

### Hazards from §2c with no tier of their own — fold into Wave 1

- **Chunk-table floor headroom is thin.** `MaxChunks` is 49,152 against a measured
  39,020 settle — ~26%, and motion retention's double-residency pushes it up.
  Crossing it takes the `ChunkOrigins.Num() > GetMaxChunks()` branch, which calls
  `MarkRenderStateDirty()` — a full render-state rebuild that (per D4-R1)
  invalidates every GPU-written range. Raise the floor, or make the rebuild
  re-dispatch GPU-written chunks. T1-1 changes that branch's surroundings anyway.
- **Resurrection writes `RetainUntilSeconds = 0.0` where the documented sentinel is
  −1000.** Behaviourally equal today, a trap later. One line.
- **Freed pool ranges keep drawing as degenerate points until reused** — real vertex
  cost, invisible in a picture. Subsumed by T1-4 and T2-4; no separate work.

### Wave 1 closes when

The head-to-head is re-run — old stack (`voxel.Stream.GPU 0`,
`JobsInFlightPerCore=2`, CPU mesher) vs the full shipped stack, ×2 legs each, at
the adopted 128 m / 4 km cascade — and the pooled arm is at or past **1,082
chunks/s** with `holes(final) = 0`. That is the P0 close. If it is not met, T3-5
moves ahead of Wave 2. Record the result in `docs/measurements/` in the existing
format whichever way it goes: a negative result is a deliverable, because it means
the plateau has a second mechanism nobody has found yet.

---

## Wave 2 — Kill the transient LOD-boundary holes (T2)

Wave 1 raises throughput; this wave attacks the symptom a player sees. Several
items are only affordable once the apply path is cheap.

### T2-1 — Velocity-aware anchor

Nothing in streaming reads velocity, so work is prioritised for where the camera
*was* by the time results land ~225 ms later — 4.5 m of error per pipeline transit
at 20 m/s. Read `Pawn->GetVelocity()` alongside the location, keep an EMA
(~0.25 s — raw jitter would re-arm the entry-scan crossing gate and reproduce the
measured refill-churn pathology), and compute
`PredictedAnchor = Anchor + Clamp(SmoothedVelocity * LeadSec, MaxLeadUU)`.

**Where the predicted anchor is used, and where it must not be** — this asymmetry
is not in the review and it decides whether the idea helps or opens holes behind
the player:

| Site | Anchor | Why |
|---|---|---|
| `SortPendingQueues` | **Predicted** | Pure priority; admits and evicts nothing. |
| `RecomputeDesiredSet` **entry** pass (annulus test, `AddCandidate`, `LevelAdmissionCutoffDistSq`, `DropFarthestOverCap`) | **Predicted** | This is the point of the idea. |
| Entry-scan crossing gate (`LastAnchorChunkPerLevel`) | **Predicted**, still quantised | Quantisation is what keeps it from free-running. |
| `RecomputeDesiredSet` **exit** pass (`bBeyondOuter`, `bInsideInner`, `bBeyondVertical`) | **True anchor** | Evicting against a forward-shifted centre deletes ground *behind* the camera that is still on screen. |
| `ReplacementCovered`'s `XYDesired` | **Predicted** | Mirrors the entry predicate; if they disagree, an absent replacement blocks a stand-in forever. |
| `ComputeRingSkirtMask`, `ComputeRetainReplacementZMask` | **True anchor** | Both describe geometry currently drawn. |

*Gate:* `voxel.Stream.VelocityLeadSec` (default 0.0 ⇒ byte-identical to today) and
`voxel.Stream.VelocityLeadMaxUU` (default 3000). Sweep 0 / 0.25 / 0.5 / 0.75 / 1.0.
*Measured:* flight-phase `holes` on the GPU arm (854–907 median at baseline)
against the CPU arm's 55. Secondary: `capRel` should fall; `recordsAdded` /
`candidatesRejected` churn must **not** rise — that is the pathology this can
reintroduce. *Falsified if* flight holes do not fall ≥30% at any lead value, or if
churn rises >20% at the value that helps.

### T2-2 — Speed-scaled inward pre-admission band

The outer ring edge has a 1.25× exit band and an e/√2 seam pad; the inner boundary
crossing — the exact reported symptom — has nothing ahead of it. Mirror it inward:
`InwardPadUU = Clamp(Speed * InwardPadSec, 0, MaxInwardPadUU)`, zero when
stationary.
**Pad both sides.** The entry pass's inner skip and the exit pass's `bInsideInner`
read the *same* raw `InnerUU`; padding entry alone creates an admit→evict→admit
churn loop of exactly the shape the 2026-07-27 refill-oscillation fix chased. The
exit pad must be strictly wider — one chunk half-diagonal — and
`ReplacementCovered`'s predicate has to agree with both.
*Gate:* `voxel.Stream.InwardPadSec` (default 0.0), `voxel.Stream.InwardPadMaxUU`
(default 3000).
*This is not the closed ring cross-fade* — overlap, not fade. Note it in the commit.
*Measured:* same flight legs as T2-1 (measure T2-1 alone first, then the pair).
Cost: `tracked`, `residentQuads` at settle, `poolPct`. The outer pad cost ~9%
residency. *Falsified if* this costs >15% residency without a matching flight-hole
drop, or if `allocFail` goes non-zero — the 44 M pool sizing includes only +13% for
motion stand-ins and this adds a second overlap band, so consider raising
`kPoolCapacityQuads` in the same PR and saying so.

### T2-3 — Cold-band prefetch along velocity

The cold-band throttle serialises the leading edge: a cold L0 footprint dispatches
one blind seed and defers its column-mates until that seed's result *drains* — a
≥28 ms GPU round trip plus poll quanta per new column, with a 5 s age-out backstop
equal to 100 m of travel at 20 m/s if a mark strands.

**(a) K > 1 blind seeds while moving.** The throttle gates on
`FootprintBlindJobInFlight.Find(Footprint)` being present at all; make the map's
value a count and allow up to K concurrent seeds when `Speed > 0`. Nearly a
one-liner, fully reversible. *Gate:* `voxel.Stream.ColdBandSeeds` (default 1 = today).

**(b) Band-only prefetch along velocity.** Bands are pure functions of (X, Y), so
`FootprintBandCache` could be seeded for the leading-edge annulus. The kernel path
exists, but `FVoxelGpuMeshJobManager::Tick` **rejects** `!Job->Region.bMeshChain`
("this manager exists to produce quads"), so this needs either a relaxation plus a
band-only job state or a second lightweight submit path — real work that competes
with the fork for in-flight slots. **Stage behind (a)'s leg.**

*Measured:* `ColdBandDefersSinceLog`, `ColdBandHeldThisFrame` and
`FootprintBandCache.Num()` are already in the 5 s line. *Falsified if* (a) at
K=2/4 does not reduce cold-band defers per flight, or reduces them without moving
holes — in which case leading-edge serialisation was not the constraint and (b)
should not be built.

### T2-4 — Pool-range parking instead of freeing

On evict, keep the pool range and table entry, hidden, on an LRU; re-admit becomes
flipping the entry back. Under-motion ring oscillation and inward/outward crossings
become O(1) instead of a full re-mesh round trip.

Component side: `ParkChunk(Handle)` sets `ChunkOrigins[id].W = 0` (already what
`RemoveChunkInternal` does for neutralisation), marks the entry dirty, and **keeps**
the allocation, handle and id; `UnparkChunk(...)` restores the entry and re-inserts
the run. Both are O(1) game-thread plus one entry delta given T1-2(a)/(c), with no
quad traffic in either direction. Subsystem side: a
`TMap<FVoxelLevelChunkKey, FParkedGeometry>` (`PoolHandle`, `GenerationId`,
`ParkedAtSeconds`, `QuadCount`) plus an LRU; `ReleaseChunkGeometry` parks under a
cap, and the entry pass consults the map before dispatching.

**Invalidation is the correctness surface.** `MarkChunkDirtyForRemesh` and
`PropagateEditToMips` must evict parked entries for their keys, and a
`GenerationId` mismatch on unpark must evict rather than draw. The record is gone
from `ChunkRecords` by then, which is why this index is a separate map keyed by
`(level, key)` rather than living on the record.
**Chunk-table pressure:** parking keeps ids live, so `ChunkOrigins.Num()` climbs
toward parked+resident. Raise the 49,152 floor in the same PR, sized from the park
cap — crossing it forces a whole-pool re-upload that invalidates every GPU-written
range.

*Gate:* `voxel.Stream.PoolParkMax` (default 0 = off). *Depends on* T1-4 and
T1-2(a)/(c); do not build before both ship.
*Measured:* flight leg. `avgChunks/s` (re-admits become O(1)), flight holes, and
`dispatched` per flight (should fall — that is work not done). *Falsified if*
`poolPct` crosses ~90% (fragmentation on a first-fit allocator, and `UpdateChunk`'s
realloc path *deletes resident terrain* on a full pool) or `allocFail` goes non-zero.

### T2-5 — Mesh LRU cache in RAM: **do not build**

The review costs it at 8 B/quad, ~280 MB for the cascade — but under the shipped
`voxel.GPU.MeshDirectToPool 1` default **the quads never reach the CPU**;
`PooledQuads` holds zeros for every GPU-meshed range by design. A RAM cache could
only be filled by reinstating the readback D1 deleted, on every ring crossing. T2-4
already covers the hot loop on the GPU side where the bytes live. If a warm-loop
cache is still wanted afterwards, the right shape is a hidden overflow region
*inside* the pool, and that is a design note rather than a wave item. (The same
objection does **not** sink T4-3, where the readback is paid once per chunk per
world version rather than per crossing.)

### T2-6 — Release stand-ins on settle, cap as backstop

`ReplacementCovered` re-runs per held stand-in per frame (~8k map probes/frame at
~1,000 held), and the 10 s cap is itself the hole-producing release when the
pipeline is slow. Two changes, and the **pair** is recommended over the review's
pure event-driven form:

**(a) Event-driven release.** When `ApplyMeshResult` sets `bMeshSettled` — and at
the two pre-dispatch skip sites that also settle, the buried-skip and sky-band
`continue`s, which the ring-gap wave already had to remember once — look up the
settled key's parent and children and decrement a new `RetainWaitCount` on any
retained neighbour, initialised in `RecomputeDesiredSet` where `RetainReplaceDir`
and `RetainChildZMask` are already stamped. Zero ⇒ release.
**(b) Keep an amortised poll for the absence case.** Event-driven has no event for
"an absent replacement became desired" — exactly the state `ReplacementCovered`
blocks on since the 2026-07-27 fix. Keep the poll but round-robin 1/N of held
stand-ins per frame (N = 8): an 8× cut with no new correctness surface.

*Gate:* `voxel.Stream.RetentionEventRelease` (default 0),
`voxel.Stream.RetentionPollFraction` (default 1 = today).
*Measured:* the three release counters (`covRelSettled` / `covRelAbsent` /
`capRel`) must be statistically identical to the polling build over a full leg — a
systematic shortfall in `covRelSettled` means releases are being missed, showing up
as `held` climbing and residency rising. Secondary: `unloadMs`. *Falsified if*
`held` rises, or flight-phase `capRel` rises above its T0-3-corrected baseline.
*Depends on T0-3.*

### T2-7 — Ring-5 ↔ clipmap seam

Four independent sub-items, each with its own CVar; do not land them as one change
because they will not all be wins. **None is verifiable by a counter** —
`CoverageVerify` only enumerates ring annuli and is blind to the clipmap — so these
need pinned-pose screenshot diffs against a same-session floor. Note in the PR that
the *motion* half of §2d (the ±128 m leading-edge lag) cannot be shown by a still
at all and needs a video capture or an instrumented hole-mask-vs-cascade-edge
distance log; offer that rather than a misleading screenshot.

- **(a) Inscribe the square hole in the circular cascade.** The hole is a square of
  half-extent 4,096 m, the cascade a circle of radius 4,096 m, so the corners out to
  5,793 m belong to neither. Shrink the half-extent to `Outer/√2`. Cheapest
  correctness win in T2, but it creates a clipmap/voxel overlap in the corners —
  ship with (d). *Gate:* `voxel.Clipmap.HoleInscribed` (default 0).
- **(b) Overlap by one coarse cell** rather than abutting.
  *Gate:* `voxel.Clipmap.HoleOverlapCells` (default 0).
- **(c) Move the hole mask per tick, independent of the 256 m vertex snap.** The
  mask is a material parameter; the snap exists for the vertex grid. Decouple.
  *Gate:* `voxel.Clipmap.HoleFollowsAnchor` (default 0).
- **(d) Bias the clipmap's inner rings down** by the detail-band envelope
  (`kLandformAbsMaxMm + kMicroAbsMaxMm`). This is what makes (a) and (b)'s overlap
  read as "voxels sit on the clipmap" rather than z-fighting.
  *Gate:* `voxel.Clipmap.InnerBiasMm` (default 0).
- **(e) Outward skirt on ring 5's outermost faces.** `ComputeRingSkirtMask` only
  skirts toward *finer* neighbours; add the mirror case toward the clipmap.
  *Gate:* `voxel.Stream.RingSkirtOutermost` (default 0). This changes emitted
  geometry ⇒ **`voxel.GPU.VerifyAsyncMesh` and `voxel.GPU.VerifyRingSkirt` must both
  be re-run**, and it is the one T2 item that can touch the digest if the skirt
  logic lives in `worldgen.ush`.

**Wave 2 closes when:** the ring-gap flight is re-run and flight-phase transient
holes drop from the 854–907 median toward the CPU arm's 55, with admission churn no
worse than +20% and residency cost under +15%.

---

## Wave 3 — Throughput multipliers (T3)

Designed or half-built already. Sequence strictly after Wave 1: a producer
multiplier is worthless while the consumer is the wall, which is the docs' own
sequencing and what the plateau vindicated.

| Idea | Already built | Remains | Gate | Decided by |
|---|---|---|---|---|
| **T3-1 Tile batching** (4×4 lattice, ≤16 chunks/dispatch) | Design complete; **occupancy census shipping** (`Voxel tile census` line — mean 2.4–6 cold, ~2–3 under motion). D2 chunk-local emit and the D5.3 skirt-in-kernel decision make it composable. | A multi-chunk request shape in `SetChunkFootprint`; an N-chunk `AddRegionPasses`; N pool allocations from one source buffer (`FPendingGpuWrite` already carries `SrcOffset`, so the pool side needs no change). | `voxel.GPU.MeshTileBatch` (0) | `avgChunks/s` and cold-fill time. Touches emit ⇒ **digest `6e893ab3679a8c81` must be re-run.** |
| **T3-2 Uber-batch the RDG graph** | `DispatchBatch` already builds one graph per batch, not per job. T1-1 already delivers the render-side merge of per-apply pool writes. | One *dispatch* per kernel across N chunks with per-chunk constants in a structured buffer (`QuadWriteBase` anticipates this). | `voxel.GPU.MeshUberBatch` (0) | `dispatchMs`/`renderMs`; amortises the 3.4× halo waste for adjacent chunks. |
| **T3-3 D6 band-reduce** | **Done and on** — `SubmitGpuMeshJob` sets the band request for every L0 fork dispatch with no CVar in the path; gate green in `wave-d6-band-coverage.txt`. | **Struck.** No gate remains to flip. What genuinely remains is a *coarse-level* band, blocked on the units problem `SubmitGpuMeshJob`'s own comment states. | — | — |
| **T3-4 Vertex-factory indirection** | `FVoxelQuadRangeParameters` has exactly one member (`BaseQuad`), which is what makes this tractable. | Replace the per-range `CreateUniformBufferImmediate` in `GetDynamicMeshElements` with one structured buffer of ranges plus a per-element integer in `FMeshBatchElement::UserData`. | `voxel.Stream.GPUCullIndirectRanges` (0) | Kills ~5–6k single-frame uniform buffers/frame and the 181–421 ms churn transients. **Prerequisite for T3-5.** `.ush` edit ⇒ ground rule 9 (work in a worktree). |
| **T3-5 GPU-driven cull → indirect draw → compaction** | **G0 landed** — the gather-split census (`RecordGather`/`CountSubmittedQuads`) produced the 92.6%-shadow figure. | G1 (indirect draw reproducing the current full draw bit-for-bit) → G2 (`VoxelQuadCompact.usf` vs a CPU reference) → G3 (`FSceneViewExtension` + external-access conversion) → G4 (the `.ush` branch + A/B). Staged plan is in `docs/gpu-g-compaction.md` §6 — do not re-plan it. | staged per that doc | Removes ~195k CPU frustum tests/frame, the K=1024 over-draw floor, and gives shadow gathers a scheduling story. **The named unowned debt, and the fix for the static-scene 40 ms.** |
| **T3-6 Idle-time pool defrag** | `GetLargestFreeRun`/`GetFreeRunCount`/`GetFreeQuads` already instrument the diagnosis. | A background `QuadPoolWriteMain`-shaped move pass, N quads/frame, updating allocation, run and table entry atomically per move. | `voxel.Stream.PoolDefragQuadsPerFrame` (0) | `GetFreeRunCount()` (1 = unfragmented) and `allocFail`. **Composes badly with T2-4** — parked ranges are immovable unless the park index updates too, so sequence after it. |

---

## Wave 4 — Radical bets (T4)

Each wants its own design note before any code. Sequenced behind Wave 1/2 results
because two may prove unnecessary. **What each one actually buys, in practical
terms, is the first line of each entry** — the rest is how.

**T4-1 — Speculative generation into the idle GPU.**
**Upside: streaming stops being something the player can see.** Terrain is already
built and sitting in the pool before you fly into it, so the LOD-boundary holes and
pop-in disappear rather than getting faster. This is the only item on the whole
board that changes the *category* of the problem instead of the number. The
headroom is real and large: the bench measures **92,000 chunks/s** of GPU
generation at r=128 against a pipeline currently consuming ~600/s, so after Wave 1
the GPU is idle roughly 99% of the time from streaming's point of view. Spend that
idle time pre-meshing the annulus in the velocity cone (T2-1's predicted anchor)
into parked pool ranges (T2-4's mechanism), so admission *finds* geometry instead
of commissioning it. Same worldgen, same quality, same LOD distances — just
earlier. Composes entirely from things Waves 1–2 already build, which is why it is
the highest-value T4 item despite being filed as a bet.

**T4-2 — GPU-resident residency.**
**Upside: buys back roughly a third of the frame budget at cascade scale, and stops
the world's size from taxing the CPU at all.** The per-frame bookkeeping that
decides which chunks *should* be loaded costs ~2.4–2.7 ms at 16k records and
**~6–8 ms at 40k** — against a 22 ms frame, a large slice spent on housekeeping
rather than drawing, and it grows as the world grows. Moving annulus enumeration
and desired-set diffing to a compute pass (the cascade is pure math over the
anchor) leaves the CPU consuming only a compact admit/evict delta for records,
collision and edits. It also removes the entry-scan quantisation that T2-1 and T2-2
only work around. **Decide after Wave 1's leg, not before** — if the plateau closes
and the exit scans stay under ~2 ms, this is a large project solving a problem that
no longer hurts.

**T4-3 — On-disk deterministic chunk cache.**
**Upside: the second visit to anywhere is free.** Worldgen is a versioned pure
function, so packed quads keyed `(seed, worldgenVersion, level, key)` can be written
to disk and read back instead of regenerated; edits are already vetoed by
`NeedsOverlayAwarePath`. Cold fill (~47 s at 4 km) and every revisit become
disk-bound rather than compute-bound — practically, faster loads and no re-cost for
ground you have already seen. Also the natural transport for a future CDN vista
layer.
*Caveat that decides the design:* under the shipped `MeshDirectToPool` default the
quads never come back to the CPU, so populating the cache means reading them back.
That is defensible **here** — paid once per chunk per world version and amortised
across every future session — where the same cost kills T2-5, which would pay it on
every ring crossing.

**T4-4 — Async-compute meshing queue.**
**Upside: smoother frames under load, not more chunks.** Moving the mesh and
compaction chain to the async queue lets it overlap rasterisation instead of
competing with it, which shows up as fewer hitches during heavy streaming rather
than a higher chunks/s number. Only meaningful once T1-5 and T1-6 have made the
pipeline GPU-bound rather than tick-bound — until then there is nothing to overlap.
T0-2's `DispatchToReadyMs` distribution is the tell.

**T4-5 — Delete `FSharedMipCache`.**
**Upside: 512 MB of RAM back, and one less dead subsystem to reason about.** No
speed win — a memory and clarity item. The cache is **unreachable at shipped
defaults**: it serves only `MakeLevelSampler`, and `kDefaultCoarseMinLevel = 1`
routes every L≥1 job through the coarse path that bypasses it. That verdict is
available by reading the code and needs no leg. Remove it along with
`voxel.MipCacheBudgetMB` and the `MipCache*` fields in `FVoxelPerfSnapshot`.
**Do not use `-VoxelL0GridCacheProbe` to decide this** — the probe measures reuse of
the level-0 34×34 column grid, an entirely different and genuinely live cache. That
column-grid cache deserves its own item; the probe exists to answer *that* question
and answers it well.

---

## Verification

Per-wave gates are stated at each item. End to end:

1. **Correctness, for every wave that touches geometry:**
   `voxel.GPU.VerifyPoolWrite 16 4` (direct and the `MeshDirectToPool 0` control),
   `voxel.GPU.VerifyAsyncMesh 64 8`, `voxel.GPU.VerifyRegion` against the pinned
   digest `6e893ab3679a8c81`, and `voxel.Stream.PoolClobberTest` for anything
   touching pool publication. Plus `ctest --test-dir build/voxel-core` and the
   `VoxelEarth.GpuPool` automation tests for the suballocator.
2. **Throughput and coverage:** the standard leg through `tools/voxel-run-leg.ps1`,
   ≥2 legs per arm, alternating GPU and CPU arms, at the adopted cascade. Decisive
   metrics: `avgChunks/s`, `holes` (converged linger for permanent, flight-phase for
   transient), hitches, p50, p95.
3. **Thresholds:** `tools/check-perf-run.py` against the emitted
   `Saved/PerfRuns/perf_*.json`, so pass/fail is an exit code rather than a reading.
4. **The head-to-head that closes the programme:** pooled vs component at the
   adopted cascade, appended to
   `docs/measurements/gpu-throughput-wave-2026-07-27.txt` in its existing format.

### Which gates apply where

| Change | VerifyPoolWrite | VerifyAsyncMesh | VerifyRegion | CoverageVerify | PoolClobberTest | digest | screenshot diff |
|---|---|---|---|---|---|---|---|
| T1-1 batch publish | **yes** (+control) | — | — | yes | yes | — | — |
| T1-2 incremental table/runs | yes | — | — | yes | yes | — | yes (cull can hide geometry) |
| T1-3 params source | — | — | — | — | — | — | **yes** (shading input) |
| T1-4 GPU hide pass | **yes** + forced partial-realloc probe | — | — | yes | — | — | yes |
| T1-7 refusal retry | — | — | — | **yes, forced saturation** | — | — | — |
| T2-1/2/3 anchor & admission | — | — | — | **yes, flight + converged** | — | — | — |
| T2-4 parking | yes | — | — | yes | yes | — | yes |
| T2-7(e) outermost skirt | — | **yes** + VerifyRingSkirt | yes | — | — | **if `worldgen.ush` moves** | yes |
| T3-1 / T3-2 batching | yes | yes | yes | yes | — | **yes** | — |
| T3-4 / T3-5 draw path | — | — | — | yes | — | — | **yes, pixel identity first** |

Every wave's result — including negative ones — gets written to
`docs/measurements/` and the falsified-lever list extended. That record is what
stopped this review re-proposing six dead levers.
