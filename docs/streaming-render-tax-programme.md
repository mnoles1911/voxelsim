# The streaming render tax — programme scoping

2026-08-27. A plan to find out, not a plan to build. Every claim is labelled
**VERIFIED** (I read the code or the log line), **INFERRED** (reasoning from
something verified), or **CITED** (external source, URL given).

---

## Summary — the five things a reader needs

1. **The scoping numbers are stale by 10x.** The +11.3 ms render tax is **+1.11 ms**
   on today's shipping default, and 0.054 ms/chunk is **0.0024 ms/chunk** (§0).
2. **Parked now meets >100 fps at every percentile including p99.** The whole
   remaining Goal-3 gap is the **moving tail**: +6.10 ms at p95, +10.10 ms at
   p99, but only +0.70 ms at the median (§1).
3. **The tail is not GPU backpressure and not the marcher** — it survives a 50%
   ray-count cut essentially unchanged, in an A/B that has already been run and
   could have come out the other way (§2.1).
4. **The tail is not per-chunk cost either** — the frames applying the most
   chunks are 5.30 ms *faster* than p95 (§1). It is bursty work, and
   `FVoxelGpuMeshJobManager::Tick` peaking at **96.0 ms on one frame** is the
   strongest single prior (§2.9, §6).
5. **Nothing in this tree describes a 15 ms frame.** Percentiles have no
   attribution, phase buckets are means, and the per-frame attribution block only
   fires above 33.3 ms. That gap is where the answer is hiding (§5).

**The programme is therefore "find what makes a bad frame bad", not "optimise
streaming".** Nine of the ten steps in §7 need no new code.


---

## 0. READ THIS FIRST — the numbers this programme was scoped on are stale by 10x

The brief for this document states the problem as:

> Parked already meets target at 8.85 ms; MOVING costs 20.19 ms. The +11.3 ms is
> RENDER THREAD. 163 chunks/frame costs +10.20 ms of frame, of which +8.87 ms
> (87%) is render thread — ~0.054 ms of render thread per chunk. RHI time nearly
> quadruples, 1.68 -> 6.35 ms.

**Not one of those figures survives on today's shipping default.** They describe
a binary superseded on 2026-08-25/26.

**VERIFIED** — `D:\voxelsim\Saved\TJDL-A-control.log`, run 2026-08-26 13:04-13:10.
Flag-free shipping default: the command line at line 491 carries no `voxel.*`
feature flag beyond `voxel.Stream.CoverageVerify 1` and the phase instrument.
`view=1552x873` read from the marcher's own line (TSR-upscaled to the owner's
2560x1440). `grep -c 'DOUBLE GRANT'` = **0**, so the leg is quotable.

Full-leg aggregate:

    seg                     n       appl/frame  frameMs  tickMs  renderMs  rhiMs
    SETTLED-PARKED-LEG    ~9,500      0.0        8.28     0.05     8.26     1.77
    SETTLED-MOVING-LEG    12,414     33.4        9.67     0.79     9.37     2.46

**The moving-vs-parked render-thread delta is +1.11 ms, not +11.3 ms.**
**RHI time rises 1.77 -> 2.46 ms (+39%); it does not quadruple.**

Bucketed **inside one 5-second window** on that leg, so leg-to-leg variance
cannot enter (window at 13:05:43, `-VoxelFramePhaseHeavy=64`):

    seg              n     appl/frame  frameMs  tickMs  renderMs  rhiMs
    SETTLED-PARKED   43       0.0       8.52     0.04     8.52     1.74
    LIGHT-APPLY     473       2.9       8.92     0.47     8.92     1.93
    HEAVY-APPLY      79     175.5       9.90     0.62     9.34     2.24

`HEAVY - LIGHT` = **+172.6 chunks/frame for +0.42 ms of render thread**
= **0.0024 ms of render thread per chunk**, against the recorded 0.054.
**A 22x reduction.** RHI moves +0.31 ms, not +4.67 ms.

**INFERRED** — the recovery is what shipped on 2026-08-25/26 and is recorded in
`docs/SCOREBOARD.md`: the GPU worklist claim chain defaulting ON (which made the
RDG pass term *flat in N* — precisely the mechanism that removes per-chunk RHI
translation cost), `MeshBatchCap` 64 -> 16, the raster page rescue, speculative
park, the pool double-grant fix, and the 7-ring cascade (~3.4x fewer resident
chunks). Moving p99 went 34.00 ms -> 20.07 ms across those.

> ### Standing instruction for this programme
> **Do not open a lane against the 0.054 ms/chunk, +8.87 ms, or 1.68 -> 6.35 ms
> figures.** They are retired. Re-derive any per-chunk render cost from a leg on
> the current binary before quoting it. This is the house rule *"a number taken
> on a different binary is not a control"* applied to the programme's own
> premise.

---

## 1. What the problem actually is now: **a tail, not a mean**

**VERIFIED** — same leg, `Voxel frame dist ... scope=total`, final window:

    seg              n        mean   p50           p95           p99          max
    SETTLED-PARKED  16,980    8.27   8.40 (119)    9.10 (110)    9.50 (105)   29.57
    SETTLED-MOVING  12,638    9.56   9.10 (110)   15.20  (66)   19.60  (51)   98.55

    gate=GOAL3-FAIL  gateP95=FAIL  gateSteady=FAIL  gateSpeed=PASS  meanSpeed=23.4 m/s
    stutters 108 (0.85%)   hitches 9

Two things follow, and both change the programme's shape.

**(a) PARKED MEETS THE >100 fps TARGET AT EVERY PERCENTILE, p99 INCLUDED.**
9.50 ms at p99 = 105 fps. The parked floor is no longer the problem.

**(b) MOVING MEETS IT AT THE MEDIAN AND MISSES IT ONLY IN THE TAIL.**

    percentile   moving   parked   delta
    p50           9.10     8.40    +0.70 ms
    p95          15.20     9.10    +6.10 ms
    p99          19.60     9.50   +10.10 ms

**Streaming costs ~0.7 ms on a typical frame and 6-10 ms on a bad one.** That is
a **variance** problem. It is not a throughput problem and it is not a
per-chunk-cost problem — a per-chunk cost shows up at the median first.

This also reconciles the two goals. Goal 3b (p99 >= 50 fps moving) is met at
51 fps *on this leg*, within noise. Goal 3a (>100 fps steady) is missed by
6.10 ms at p95 and 10.10 ms at p99. **The entire remaining gap to 100 fps is the
moving tail.**

### The tail is NOT the heavy-apply frames

**VERIFIED, and this is the first real refutation this document offers.** The
`HEAVY-APPLY` bucket — frames applying >= 64 chunks, mean 175.5 chunks/frame —
has a mean frame time of **9.90 ms**. p95 is **15.20 ms**. *The frames doing the
most streaming work are not the slow frames.*

    If the tail were per-chunk streaming cost, HEAVY-APPLY's mean would sit
    at or above p95. It sits 5.30 ms BELOW it.

**"Many chunks in one frame" costs about 1 ms and cannot account for a 6-10 ms
tail.** Whatever makes a p95 frame is something else — and no instrument in this
tree currently describes it (§5).

---

## 2. Part A — where does the cost go?

### 2.1 The backpressure question is ALREADY ANSWERED, by legs that exist

The brief asks for this first, and rightly: *if the delta is mostly GPU
backpressure the whole programme points at shaders instead.* **The measurement
that separates them has already been run.** The half-resolution study of
2026-08-26 *is* that experiment: halving the ray count halves the dominant GPU
cost while leaving every CPU-side streaming path untouched.

**VERIFIED** — four matched legs, one binary, same pose, same flight, same
`-VoxelFramePhase=3`, `DOUBLE GRANT: 0` on all four
(`Saved/TJDL-{A-control,B-halfstatic,C-halfjit1,D-halfjit2}.log`):

    arm            parked                  | moving                  | TAIL DELTA
                   p50    p95    p99       | p50    p95    p99       | p95     p99
    A full-res     8.40   9.10   9.50      | 9.10  15.20  19.60      | +6.10  +10.10
    B half-static  6.00   6.60   7.10      | 6.90  12.20  17.00      | +5.60   +9.90
    D half+jit8    6.00   6.60   7.10      | 6.90  12.10  17.20      | +5.50  +10.10

    renderBusy mean:  A parked 8.26 -> moving 9.37   (+1.11)
                      B parked 5.95 -> moving 7.11   (+1.16)
                      D parked 5.94 -> moving 7.10   (+1.16)

**Halving the ray count cut the parked frame by 2.50 ms at p95 (-27%) and cut the
moving tail delta by 0.50 ms (-8%). At p99 the delta did not move at all
(+10.10 -> +9.90 / +10.10). The mean render delta did not move either
(+1.11 -> +1.16).**

**The streaming rate is matched across arms**, so the comparison is fair.
`applPerFrame / frameMs` gives 3,454 chunks/s (A), 3,342 (B), 3,320 (D) — within
4%. `tickMs` is 0.79 / 0.79 / 0.78. `rhiMs` is 2.46 / 2.35 / 2.35 —
**RHI-thread time is resolution-independent**, which is what command translation
should be.

**CONCLUSION (VERIFIED): the moving tail is not GPU backpressure and it is not
the marcher.** It survives a 50% cut in GPU ray work essentially intact.
Deleting *half the marcher outright* leaves moving p99 at 17.00 ms = 59 fps,
nowhere near 100. **At most ~0.5 ms of the 10.10 ms tail is GPU.**

This test could have come out the other way: backpressure predicts the delta
falls ~27% with the GPU work. It fell 8% at p95 and 0% at p99.

**A third reading, and it is the interesting one.** The delta is flat against
*both* levers:

    if tail = GPU backpressure     -> delta falls ~27% with ray count.  It did not.
    if tail = per-chunk CPU cost   -> delta tracks chunks/frame.        HEAVY-APPLY refutes it (§1).
    if tail = periodic bursts      -> delta is flat in both.            OBSERVED.

**INFERRED:** the tail is work that fires on *some* frames for reasons unrelated
to how many chunks that frame applied and unrelated to GPU load. §4 ranks that
hypothesis space.

### 2.2 `renderMs` is not CPU time — the mechanism, verified deeper than the brief states

The brief's Trap 1 is right, but its cited mechanism is only half of it.

**VERIFIED** `D:\UE_5.8\...\SlateRHIRenderer\Private\SlateRHIRenderer.cpp:1356`

    GRenderThreadTime     = ThreadTime - RenderThreadIdle;   // Waits + GPUQuery + GPUPresent
    GRenderThreadWaitTime = RenderThreadIdle;

**VERIFIED** `Core\Public\Stats\ThreadIdleStats.h:54-64, 80-110` —
`FScopeNonCriticalPath` only decrements `IsCriticalPathCounter`; it **never
touches `Waits`**. Only `FScopeIdle` adds to `Waits`, and only when its `bIgnore`
flag is false.

**VERIFIED** `RHI\Private\RHICommandList.cpp:1940` — `WaitOnRHIThreadFence` opens
with `FScopeNonCriticalPath`, its comment saying so explicitly.

**And here is the part the brief does not have — the real leak, found by
following the wait down.** `WaitOnRHIThreadFence` ->
`FTaskGraphInterface::WaitUntilTaskCompletes(Fence, RenderThread_Local)` ->
`WaitOnNamedThreadForTasks` -> `ProcessTasksNamedThread(QueueIndex, bAllowStall)`.
`GetRenderThread_Local()` is the **local queue**, `QueueIndex == 1`.

**VERIFIED** `Core\Private\Async\TaskGraph.cpp:739-745`

    else if (ThreadId == ENamedThreads::GetRenderThread())
    {
        if (QueueIndex > 0)
        {
            StallStatId  = GET_STATID(STAT_TaskGraph_RenderStalls);
            bCountAsStall = true;
        }
        // else StatName = none, ...
    }

**VERIFIED** `TaskGraph.cpp:784`

    Queue(QueueIndex).StallRestartEvent->Wait(..., bCountAsStall);

and `FEvent::Wait`'s second parameter is **`bIgnoreThreadIdleStats`**
(**VERIFIED** `Core\Private\Windows\WindowsPlatformProcess.cpp:1940`,
`FScopeIdle Scope(bIgnoreThreadIdleStats)`).

**So `bCountAsStall = true` means the stall is NOT recorded as idle, and is
therefore booked as render-thread BUSY** — on the render thread's local queue,
which is exactly where every `WaitOnRHIThreadFence`, every parallel-translate
await and every dependent task wait lands.

**Two consequences, both load-bearing.**

1. **`renderBusyMs` counts render-thread dependent waits as busy** — not only GPU
   backpressure but *any* wait on the local queue. `renderMs`, `renderBusyMs` and
   `setupOther` are downstream of RHI-thread time, worker-task time and GPU time
   simultaneously.
2. **`rhiMs` does NOT have this defect and is the trustworthy number.**
   **VERIFIED** `TaskGraph.cpp:754`:
   `bCountAsStall = ThreadId != ENamedThreads::RHIThread;` — the RHI thread's
   stalls are explicitly excluded from this treatment and *do* register as idle.
   And **VERIFIED** `SlateRHIRenderer.cpp:1388`,
   `GRHIThreadTime = (Next - Last) - RHIThreadStats.Waits`. **`rhiMs` is genuine
   RHI-thread CPU busy time with idle removed.** Prefer it over `renderBusyMs`
   for any claim about command-translation cost.

**Caveat, stated rather than glossed.** The `bCountAsStall` assignments sit inside
`#if STATS`. `STATS` is 1 for Development builds (**VERIFIED**
`Core\Public\Misc\Build.h:258`), which is what the legs run. In a **Shipping**
build `bCountAsStall` stays `false` and the same waits *would* register as idle.
**The instrument behaves differently in Shipping than in the builds this project
measures, and nobody has checked what that does to these numbers.**

### 2.3 The GPU-time counter this project does not have

**VERIFIED:** `grep -rn "GGPUFrameTime\|RHIGetGPUFrameCycles" ue-project/Source/`
returns **zero call sites.** The project has never read a per-frame GPU time.
Every GPU claim in the tree comes from a one-off `ProfileGPU` or from the
marcher's own `RQT_AbsoluteTime` bracket, which measures one pass.

That is why Trap 1 keeps biting, and it is fixable in one line:

- **VERIFIED** `RHIGetGPUFrameCycles(uint32 GPUIndex = 0)` is public RHI API
  (`RHI\Public\DynamicRHI.h:1250`), readable every frame at zero cost, backed by
  `GGPUFrameTime` (`RHI\Private\GPUProfiler.cpp:2265-2284`).
- **VERIFIED** on D3D12 `GRHISupportsFrameCyclesBubblesRemoval = true`
  (`D3D12RHI.cpp:252`) — so it is **GPU busy time with CPU-caused bubbles
  removed**, exactly the quantity needed to settle busy-vs-backpressure *per
  frame* rather than per study.
- `VoxelFramePhase.cpp` already includes `RenderTimer.h` (line 8) and already
  prints `renderMs / renderWaitMs / rhiMs / gameWaitMs` on one line (`:321`,
  `:544`). Adding `gpuMs` is a one-line addition to an existing log line.

**This is the cheapest instrument improvement in the whole programme and it
retires an entire class of misattribution.** Listed as E0 in §4.

### 2.4 The LIVE per-chunk render-thread term — found, with file:line

The brief attributes the dead CPU upload path to `voxel.GPU.MeshDirectToPool`.
**That attribution is wrong, and the correction matters because it points at a
different file.**

**VERIFIED** — `voxel.GPU.PoolAlloc` is what kills the lock path.
`VoxelEarthShaders/Private/VoxelBrickPool.cpp:977` declares it `= 1`; its own
help text at `:979-992` states the consequence: while it is 1 the pool is armed,
`AddChunkFromGpu` refuses outright, `AddChunkFromCpu` diverts to
`PendingGpuCpuWrites`, **`PendingWrites` is structurally EMPTY**, and everything
downstream of it — `UploadCpuWrites_RenderThread`, `PendingClears`,
`voxel.GPU.BrickFlushBatch`, `voxel.Brick.UploadCoalesce` — is off-path.

**The same file contradicts itself, and the stale half is a trap.**
`VoxelBrickPool.cpp:247-283` carries a cost analysis of *exactly* the 163-chunk /
+8.87 ms leg, concluding "FOUR LockBuffer / Memcpy / UnlockBuffer cycles PER
CHUNK ... at 163 chunks that is ~650 lock/unlock pairs in one frame."
**That analysis describes the pre-2026-08-24 config.** Anyone reading `:247` and
tuning `voxel.Brick.UploadCoalesce` today would be optimising a dead path.

**What IS live and per-chunk (VERIFIED):**
`VoxelBrickPool.cpp:4322`, `for (const FPendingGpuCpuWrite& W : GpuCpuWrites)`,
inside `ENQUEUE_RENDER_COMMAND(VoxelBrickPoolFlush)` (`:4252`), inside one
`FRDGBuilder` (`:4283`). Per chunk:

| item | file:line | per chunk |
|---|---|---|
| `CreateStructuredBuffer` Totals (8 B) | `VoxelBrickPool.cpp:4335` | 1 |
| `CreateStructuredBuffer` Mask (8 B) | `:4338` | 1 |
| `CreateStructuredBuffer` Desc (512 B) | `:4341` | 1 |
| `CreateStructuredBuffer` Occ (<= 4 KB) | `:4346` | 1 |
| `CreateStructuredBuffer` Mat (<= 33.8 KB) | `:4351` | 1 |
| `AddBrickPoolClaimPass` — 1 buffer, 5 views, 1 `TShaderMapRef`, 1 compute pass | `VoxelGpuWorldGen.cpp:3542` | 1 pass |
| `AddBrickPoolAllocWritePasses` — 14 views, 4 `TShaderMapRef`, 4 compute passes | `VoxelGpuWorldGen.cpp:3585` | 4 passes |

**Per chunk: 6 RDG buffers, 5 uploads, 5 compute passes, ~19 RDG views, 5 global
shader-map lookups.** All five `CreateStructuredBuffer` calls pass the default
`ERDGInitialDataFlags::None`, documented at
`RenderCore\Public\RenderGraphDefinitions.h:277-280` as *"make a copy of the
initial data for replay when the graph is executed"* — so each chunk's ~38 KB is
memcpy'd on the render thread at graph-build time **and again** into the upload
heap at Execute. **VERIFIED.**

Chain, verified end to end: `DrainResults` -> `VoxelBrickCpuArm::Publish`
(`VoxelWorldSubsystem.cpp:24296`) -> `AddChunkFromCpu` (`VoxelBrickPool.cpp:2612`)
-> armed branch queues `PendingGpuCpuWrites` (`:2664`) -> `Flush()` (called once
per tick, `VoxelWorldSubsystem.cpp:10026`) -> the loop above.
`voxel.Stream.MaxAppliesPerFrame` = **192** (`VoxelDebug.cpp:344`).

**The crucial unknown, and it must be settled before any work here.** This loop
only runs for **CPU-packed** chunks. Under the GPU fork most chunks are not
CPU-packed, and today's HEAVY-vs-LIGHT render delta is only **+0.42 ms for 172.6
chunks** — which does not leave room for 6 buffers and 5 passes per chunk at that
rate. **Either the loop runs for very few chunks, or it is much cheaper than its
shape suggests.** Prove which before proposing anything (H5, §4).

### 2.5 What is NOT on the live path — measured absences, counted not inferred

**VERIFIED**, all from a full census rather than a truncated grep:

- **`FRHITransitionInfo` in this repo appears only at `VoxelBrickPool.cpp:456,
  458, 484, 498, 510, 511`**, all inside the `voxel.Brick.UploadCoalesce`
  batch-transitions arm — cvar default **0** at `:359`, and the whole path is
  off-path under `PoolAlloc=1` anyway. **DEAD.**
- **Zero `RHICmdList.Transition` / `AddUAVBarrier` calls on the live streaming
  path.** RDG owns every barrier.
- **Zero `RHICreateShaderResourceView`** anywhere; all views are RDG-managed. The
  "per-chunk descriptor churn" candidate has no call site to hang on.
- **No `RHICreateBuffer` per chunk.** RDG's pool aligns descriptors to a 64 KB
  page (`RenderGraphResourcePool.cpp:32-53`), so the variable-size Occ/Mat
  buffers collapse to one descriptor and pool cleanly.
- **No PSO-cache-miss risk while streaming.** Every `TShaderMapRef` on the live
  path hits the global shader map.
- **No stall on the live path.** Every `FlushRenderingCommands` /
  `SubmitAndBlockUntilGPUIdle` is in `VoxelGpuVerify.cpp`,
  `VoxelGpuMeshAsyncVerify.cpp`, the blocking `RunRegionBlocking` test entry, or
  teardown (`VoxelWorldSubsystem.cpp:10453`). The marcher's readbacks
  (`VoxelMarchRenderer.cpp:5621-6006`) are ring-buffered non-blocking
  `IsReady()` polls that do not scale with chunk count.
- **ENQUEUE census: 50 real call sites** in `ue-project/Source` (a bare `grep -c`
  returns 71; 21 are comments). **No site fires per chunk** — every one is
  per-tick or per-batch. `VoxelGpuMeshJobManager.cpp:1639-1648` records that the
  per-job shape was already collapsed into one batched command.
- **One RDG graph per dispatch batch**, not per chunk:
  `VoxelGpuMeshJobManager.cpp:4352`, `FRDGBuilder` at `:4356`, commented "ONE
  graph for the whole batch".
- **The 56 MiB chunk-index upload is not the default path.**
  `voxel.March.IndexDeltaUpload` defaults **1** (`VoxelMarchChunkIndex.cpp:77`),
  measured at ~145 changed cells per flush. The full upload at `:2196` is a
  fallback. Read `GetUploadStats()`'s `FullBecause*` counters before suspecting
  it.

**One INFERRED caveat worth recording, not acting on:** RDG's
`TryFindPooledBuffer` (`RenderGraphResourcePool.cpp:87`) is a linear scan over
all pooled buffers skipping any with refcount > 1. With ~900 live buffers in one
graph that is O(n^2) ~= 400k iterations/frame. Real, but probably sub-millisecond
and not worth a lane on its own.

### 2.6 The D3D12 transition tax — mechanism verified, application not established

**VERIFIED** `D3D12RHI.cpp:262` — `GRHIGlobals.NeedsExtraTransitions = true;` set
unconditionally in D3D12 init. Consumed at `RHICommandList.h:3199, 3240, 3309,
3341` (lock/unlock paths), `D3D12Buffer.cpp:845, 853, 881, 888`,
`D3D12Texture.cpp:2050-2677`, and — notably —
`RenderGraphBuilder.cpp:2717`, `if (RHICmdListUpload.NeedsExtraTransitions() && UploadedBuffers.Num() > 1)`,
so RDG's own batched-upload path pays it too. `D3D12LegacyBarriers.cpp:1409`
comments that duplicate transitions "happen most frequently with implicit ones
from NeedsExtraTransitions" — the engine authors expect them to be common.

**VERIFIED, and this is the lever nobody in this tree has noticed:**
`RHICommandList.h:1029-1040` gates it on a **per-command-list** flag —

    bool NeedsExtraTransitions() const { return GRHIGlobals.NeedsExtraTransitions && bAllowExtraTransitions; }
    bool SetAllowExtraTransitions(bool NewState);

and `RHICommandList.h:446-449` declares an engine-provided RAII scope,
**`FRHICommandListScopedAllowExtraTransitions`** (impl `RHICommandList.inl:155-160`),
default `true` (`:1187`). The implicit transitions can be switched off around a
known batch **without an engine patch**, provided that batch then transitions
explicitly and correctly.

**But the buffer lock/unlock call sites are off-path here (§2.5), and today's
`rhiMs` moves only +0.31 ms on a HEAVY frame.** There is no 4.67 ms tax looking
for an explanation. Recorded so the next person does not re-derive it; **not**
proposed as work.

### 2.7 The tail-attribution instrument is already built and has NEVER been run

**VERIFIED** — `ue-project/Source/VoxelEarthShaders/{Public/VoxelRenderFrame.h,
Private/VoxelRenderFrame.cpp}`, 79 references across 14 files:

- **Level 1** (`-VoxelRenderFrame=1`) splits `renderBusy` into `setupMs`
  (scene-renderer graph construction; anchors `SceneRendering.cpp:4299 / :4956`),
  `executeMs` (RDG execute; anchor `RenderGraphBuilder.cpp:2214`) and `tailMs`
  (everything else the render thread did) — each **busy, not wall**, with a
  **printed unclamped reconciliation residual** and `recon=INVALID` above 15%.
- **Level 2** (`-VoxelRenderFrame=2`) adds a scope on **29 verified
  `ENQUEUE_RENDER_COMMAND` call sites** outside the scene renderer, in six
  buckets (`VoxelRenderFrame.h:389-394`): `TailGpuMeshJob` (7), `TailBrickPool`
  (4), `TailChunkIndex` (2), `TailResidency` (3), `TailPoolComponent` (5),
  `TailGIVolume` (8) — plus **three** unclamped residuals `setupOther /
  executeOther / tailOther`, and **hit counters beside every ms**, so `h=0,
  ms=0` (dead scope) is distinguishable from `h>0, ms=0` (ran and was cheap).
- **Four mutation arms** (`VoxelRenderFrame.h:168`, `MutateHere` `:464`,
  `MutateArm` `:477`), each able to falsify the instrument. **Arm 3 blocks X ms
  and asserts busy must NOT move** — the only arm whose failure invalidates the
  whole file.
- **`VOXEL_RENDER_FRAME_SCOPE_TAIL(TailBrickPool)` at `VoxelBrickPool.cpp:4258`
  already wraps the per-chunk flush command of §2.4.** The instrument for that
  loop exists; no new probe is needed.

Two dead readings are named in the file in advance and are **not** evidence of
cheapness: `chunkIndex` reads `h=0` because `voxel.March.IndexGpuResident`
defaults off (its real cost is a game-thread `QueueBufferUpload` this bucket
cannot see); `poolComp` reads `h=0` because `UVoxelGpuPoolComponent` serves the
retired quad renderer.

**Terrain is not in the scene as primitives under the marcher** — records hold a
pool slot, not a component (`HoldsGeometry` / `HoldsTerrain`). Primitive
add/remove and GPU-scene churn from terrain are therefore **already excluded** as
the moving-vs-parked mechanism, without a leg.

### 2.8 Candidate weighing — verified vs not

| candidate | status | evidence |
|---|---|---|
| GPU backpressure / the marcher | **REFUTED (VERIFIED)** | half-res A/B §2.1 — tail delta flat across a 27% cut in GPU work |
| per-chunk render cost as the tail | **REFUTED (VERIFIED)** | HEAVY-APPLY mean 9.90 vs p95 15.20 (§1); 0.0024 ms/chunk (§0) |
| RHI command translation | **LARGELY RECOVERED (VERIFIED)** | `rhiMs` 1.77 -> 2.46 mean, +0.31 on HEAVY; not 1.68 -> 6.35 |
| RDG pass count | **REFUTED (project record)** | 23x peak passes moved p99 0.6 ms; prunable dead passes total 0.033 ms |
| terrain primitive / GPU-scene churn | **EXCLUDED (VERIFIED)** | marcher holds pool slots, not components |
| per-chunk descriptor / SRV creation | **EXCLUDED (VERIFIED)** | zero `RHICreateShaderResourceView` in the repo; all views RDG-managed |
| PSO cache misses while streaming | **EXCLUDED (VERIFIED)** | every live-path `TShaderMapRef` hits the global shader map |
| per-chunk buffer creation / resize | **EXCLUDED (VERIFIED)** | no `RHICreateBuffer` per chunk; RDG pool pages at 64 KB |
| readback / fence stalls on the live path | **EXCLUDED (VERIFIED)** | all blocking calls are in verify/test/teardown files |
| D3D12 implicit transitions on unlock | **MECHANISM VERIFIED, APPLICATION NOT** | call sites off-path under `PoolAlloc=1` (§2.5, §2.6) |
| the `VoxelBrickPoolFlush` per-chunk loop | **LIVE, COST UNKNOWN** | §2.4 — shape is expensive, measured delta is not; settle before acting |
| game-thread dispatch bursts | **LIVE at the hitch scale (VERIFIED)** | §2.9 |

### 2.9 The extreme tail is a different animal from the p95 tail

**VERIFIED** — the same leg's 102 `Hitch frame` lines (bar 33.3 ms; only 9 fall
inside `SETTLED-MOVING`, the rest are cold fill). Two distinct shapes:

**Shape 1 — game-thread dispatch bursts.** `dispatchMs` of
117.18 / 87.55 / 76.19 / 70.73 / 69.12 / 58.93 / 39.59 / 37.76 / 27.36 / 23.88 ms
on individual frames, with `applyMs` under 1 ms and `renderMs` around 10. Inside
those, the `other=` residual ~= `gpuMgrTickMs`, i.e.
`FVoxelGpuMeshJobManager::Tick`.

**Shape 2 — the render thread waiting, not working.** For example:

    frameMs=92.59 | subsystemTickMs=7.27 elsewhereMs=85.32
                  | renderMs=14.23 renderWaitMs=74.26 rhiMs=7.56 gameWaitMs=2.97
                  | dispatchMs=0.42 applyMs=0.08 | componentsApplied=192

74 ms of **registered** render-thread idle, with both threads' busy times normal.
That is a stall — GPU present, a driver event, an allocation, or the OS — not
busy work.

**INFERRED:** these two shapes and the p95 tail are probably three different
problems. The house rule about averaging a 20.1 ms frame with a 293 ms one
applies. **Do not open a lane that treats "the tail" as one thing.**

---

## 3. Part B — open-source and published prior art

Every item is labelled **DOCUMENTED AS SHIPPED** (official docs, published course
notes, or engine source I read), **TALK/BLOG CLAIM**, or **INFERENCE**. URLs at
the end of the section.

### 3.1 The single most adoptable thing in the engine: `FScatterUploadBuffer`

**DOCUMENTED AS SHIPPED (engine source, read directly).** UE's own answer to
"many small per-object GPU writes without N buffer locks" is
`FScatterUploadBuffer`, in
`D:\UE_5.8\Engine\Source\Runtime\RenderCore\Public\UnifiedBuffer.h` — **public,
`RENDERCORE_API`**, in a module this project already depends on. (Note: it is
*not* in a file called `ScatterUploadBuffer.h`; that name does not exist.)

Mechanism, verified line by line:

- Two GPU buffers: a `ScatterBuffer` of uint32 destination indices and an
  `UploadBuffer` of payload.
- `Init()` locks **both, once** (`UnifiedBuffer.cpp:1590-1591`), resizing only on
  power-of-two hysteresis (`:1561`, `:1576`).
- `Add_GetRef(Index, Num)` (`UnifiedBuffer.h:111-127`) writes one uint32 and
  returns a raw pointer into the payload. **No RHI call, no lock, no barrier, per
  element.**
- `ResourceUploadTo()` (`UnifiedBuffer.cpp:1609`) unlocks both (`:1635-1636`) and
  issues **exactly one** `FComputeShaderUtils::Dispatch` of `FScatterCopyCS`
  (`:1699`), bracketed by `BeginUAVOverlap` / `EndUAVOverlap`.
- The GPU side is ~15 lines (`Engine\Shaders\Private\ByteBuffer.usf:174-205`).

**Net cost: 2 locks + 2 unlocks + 1 dispatch for N scattered updates, regardless
of N.** Per-element CPU cost collapses to one uint32 store plus a memcpy.

There is a one-call RDG front end:
`FRDGScatterUploadBuilder::Process(GraphBuilder, UploadBuffer, DstResource, NumElements, NumBytesPerElement, Name, Function)`
(`UnifiedBuffer.h:406-425`). `Execute()` (`UnifiedBuffer.cpp:1066-1107`) folds
every uploader's lock/fill/unlock into **one** `AddCommandListSetupTask`, moving
it to an async task above 32 KB.

**And this is what GPUScene itself runs on.** `Renderer\Private\GPUScene.cpp`
uses **four** uploaders for the entire scene — `PrimitiveUploadBuffer`,
`InstancePayloadUploadBuffer`, `InstanceSceneUploadBuffer`, `LightmapUploadBuffer`
(`:1218-1232`), locked at `:1241-1244`, filled in parallel (`:1234`, `:1236`),
unlocked at `:1400-1403`. **Four compute dispatches per frame for the whole
scene's primitive and instance updates, no matter how many primitives changed.**
That is the concrete answer to "what replaces per-object render commands".

**Relevance here (INFERENCE, and deliberately hedged).** This is the exact shape
of the per-chunk loop at `VoxelBrickPool.cpp:4322` (§2.4): 5 `CreateStructuredBuffer`
calls and 5 compute passes per chunk could become one upload buffer and one
dispatch. **But today's measured cost of that loop is at most +0.42 ms for 172
chunks (§0), so the prize is small, and H5 must prove the loop is even hot before
anyone rewrites it.** Recorded as the *right* technique to reach for **if** H5
confirms — not as a lane.

### 3.2 Nanite — how per-instance CPU cost is kept flat

**DOCUMENTED AS SHIPPED** (SIGGRAPH 2021 Advances course notes, Karis / Stubbe /
Wihlidal). Verbatim:

> "So now we can draw all opaque geometry **with a single draw call**. Completely
> GPU driven. … **CPU cost is independent from number of objects in the scene or
> in view.** Materials are a draw per shader but those are far fewer than
> objects."

> "Renderer now retained mode / GPU scene representation persists across frames /
> **Sparsely updated where things change** / All vertex/index data in single large
> resource … **If only drawing depth the entire scene can draw with 1
> DrawIndirect**"

Published GPU timings (avg ~2496x1404 upsampled to 4K): InstanceCull 108 us,
ClusterCull 406 us, Rasterize 1148 us, BuildHZB 99 us, post-pass 125 / 102 /
183 us, `Nanite::BasePass` 2084 us. Their summary: geometry culling and
rasterization total ~2.5 ms with "**negligible CPU cost**"; the material pass
adds ~2 ms with "**a small CPU cost with 1 draw per material**".

The one CPU cost that does *not* vanish, stated by the authors: material draw
calls are issued regardless of whether any pixels use them — "unfortunate side
effect of GPU driven".

**Persistent-thread culling, quantified by Epic:** "Single dispatch … no need to
repeatedly drain the GPU / **10-60% saving (typically ~25%)**". They also warn it
depends on forward-progress guarantees "not defined by D3D or HLSL … An
optimization. Not a requirement for Nanite."
**SOURCE-VERIFIED:** `PersistentNodeAndClusterCull<...>` at
`Engine\Shaders\Private\Nanite\NaniteClusterCulling.usf:976`, dispatched from
`Renderer\Private\Nanite\NaniteCullRaster.cpp:1023`; genuinely indirect raster at
`NaniteCullRaster.cpp:3663 / :3667 / :3852`.

### 3.3 Nanite STREAMING — the closest published analogue to this project's problem

**DOCUMENTED AS SHIPPED**, both in the slides ("Resident pages stored in **one big
GPU page buffer**"; "Pages can reference data from parent pages, **without
requiring a CPU copy**") and in engine source read directly:

`Engine\Source\Runtime\Engine\Private\Nanite\NaniteStreamingPageUploader.cpp`

- `:163` — the upload buffer is locked **once for the whole batch**:
  `LockBuffer(PageUploadBuffer->GetRHI(), 0, MaxPageBytes, RLM_WriteOnly)`
- `:166-193` — `Add_GetRef()` per page is a **pointer bump plus bookkeeping, zero
  RHI calls**: `uint8* ResultPtr = PageDataPtr + NextPageByteOffset; NextPageByteOffset += PageSize;`
- `:206` — unlocked **once**
- `:329-368` — then **1** independent transcode dispatch plus K parent-dependent
  dispatches, K = dependency depth, ordered by "a naive multi-pass topology sort,
  but with **a low number of passes in practice**" (`:239`)

**Per-page CPU cost = a pointer bump and a memcpy. Zero locks, zero barriers,
zero draw calls per page.**

**And Nanite budgets itself in exactly the units this project measures.**
`Engine\Private\Rendering\NaniteStreamingManager.cpp`:
`r.Nanite.Streaming.StreamingPoolSize` default **512 MB** (`:57`) = 4096 resident
pages (`:647`); `MaxPendingPages` 128 (`:82`);
**`r.Nanite.Streaming.MaxPageInstallsPerFrame` = 128** (`:90`). Page size is
128 KB (`NaniteDefinitions.h:57-58`).

**This is the most useful single number in Part B.** Epic's shipped answer to
"how many streaming installs per frame" is **128, capped deliberately.** This
project runs `voxel.Stream.MaxAppliesPerFrame = 192` (`VoxelDebug.cpp:344`) and
measures 175.5 on a HEAVY frame. **The project is already the same order of
magnitude as Nanite's shipped budget** — evidence *against* "we are doing
something structurally excessive per frame", and consistent with the measured
+0.42 ms.

Also worth stealing: `FOrderedScatterUpdater`
(`Engine\Private\Nanite\NaniteOrderedScatterUpdater.h:22-40`, `.cpp:115-126`)
accumulates arbitrarily many 4-byte `(op|offset, value)` writes into a plain CPU
array and flushes **all of them in one pass**.

### 3.4 Virtual Shadow Maps — per-light cost flat via multiview

**DOCUMENTED AS SHIPPED** (same course notes):

> "Instead we added multiview support to Nanite. … It can render all shadow maps
> for every light in the scene, to all of their virtualized mipmaps at once. **In
> extreme cases we've seen a 100x speedup compared to individual calls.**"

> "**Shadow cost scales with resolution! Not scene complexity** * NumLights per
> pixel."

Caching: "the only regions of the shadow maps that are updated each frame are
those where objects are moving or edges of the frustum as the camera moves."

**SOURCE-VERIFIED:** 128x128 pages and a 128x128 page table
(`Engine\Shaders\Shared\VirtualShadowMapDefinitions.h:13-20`, giving 16384 virtual
resolution); `r.Shadow.Virtual.MaxPhysicalPages` default **2048**
(`VirtualShadowMapArray.cpp:173-182`). **Per-object CPU work on invalidation is
one array append** (`FShadowInvalidatingInstancesImplementation::AddInstanceRange`,
`VirtualShadowMapCacheManager.cpp:432-436`) and **all invalidations for the frame
flush in one compute dispatch** (`FInvalidateInstancePagesLoadBalancerCS`,
`:1960-1989`).

**The transferable pattern is the load balancer**, not the shadow maps:
`FInstanceCullingLoadBalancer` / `FGPUWorkGroupLoadBalancer`
(`Renderer\Private\InstanceCulling\InstanceCullingLoadBalancer.h:19-45`,
`Renderer\Private\GPUWorkGroupLoadBalancer.h:15-60`) pack variable-length
per-object ranges into fixed 64-thread workgroups. Directly applicable to
variable per-chunk work.

### 3.5 D3D12 barriers and the `NeedsExtraTransitions` tax — with the numbers vendors actually publish

**Microsoft, DOCUMENTED:**

> "**Tip: You should batch multiple transitions into one API call wherever
> possible.**"
> "**Resource barriers can be expensive. They are designed to force cache
> flushes, memory layout changes and other synchronization** that may not be
> necessary for resources already in the common state."
> "Excessive transitions to the common state can **dramatically slow down GPU
> performance**."

And, crucially for the transition tax: **all buffer resources are implicitly
promoted from `COMMON` to the relevant state on first GPU access, and the
promotion is "free"; buffers decay back after `ExecuteCommandLists`, and "the
decay is free".** Microsoft's own advice: *"try to rely on common state promotion
and decay whenever its semantics let you get away without issuing ResourceBarrier
calls."*

**NVIDIA, DOCUMENTED:** "Minimize the use of barriers and fences"; "**Group
barriers in one call to `ResourceBarrier`**"; "**Use split barriers when
possible**". The only concrete count they publish is a preference for one
NULL-to-NULL aliasing barrier over "many (for example, **200+**) resource-to-NULL
barriers".

**AMD GPUOpen RDNA Performance Guide, DOCUMENTED** (relevant — the target is an
RX 7800 XT / RDNA3): "**Barriers can drain the GPU of work.**" "**Batch groups of
barriers into a single call.**" "Don't issue read to read barriers." On uploads:
"Use the copy queue to move memory over PCIe", write to upload heaps "only using
memcpy or sequentially", and batching larger transfers outperforms numerous small
copies.

**Honest gap, stated rather than filled: no vendor publishes a "N barriers per
frame is too many" threshold.** NVIDIA's "200+" is about one specific aliasing
case, not a general budget. **Anyone quoting a general threshold is inventing
it.** The practical instrument on this GPU is Radeon GPU Profiler, which
visualises and flags barriers per frame.

**What UE's `NeedsExtraTransitions` costs per buffer (SOURCE-VERIFIED):**
`D3D12Buffer.cpp:845 / :853` gives 2 transitions on a read-only lock,
`:881 / :888` gives 2 on unlock. Each lands in
`FRHICommandListBase::TransitionInternal` (`RHICommandList.cpp:2516-2541`), which
**forces `ERHITransitionCreateFlags::NoSplit` at `:2519`** — so these implicit
transitions can **never** be the split barriers both Microsoft and NVIDIA
recommend. The engine's own comment at `D3D12LegacyBarriers.cpp:1409` admits they
are mostly redundant ("Skip duplicate transitions. This happens most frequently
with implicit ones from NeedsExtraTransitions") — but the skip happens *after*
the per-resource state lookup, so **the CPU work is spent regardless**; only the
D3D12 barrier itself is elided.

Cross-referencing Microsoft: the `Unknown -> CopyDest -> Unknown` pair UE issues
around every buffer unlock is precisely the class MS says "can introduce a lot of
unneeded overhead", because buffers promote and decay for free anyway.

**Epic ships the escape hatch and uses it in exactly this situation.**
`RenderGraphBuilder.cpp:2717`:

    if ( (RHICmdListUpload.NeedsExtraTransitions()) && UploadedBuffers.Num() > 1)
    {
        // This is here because we are explicitly batching a series of transition for all the buffers
        // and we don't [want] the individual extra transitions in Lock/Unlock
        FRHICommandListScopedAllowExtraTransitions ScopedExtraTransitions(RHICmdListUpload, false);

It then locks **all** buffers, fills, issues **one** batched transition (`:2782`),
unlocks all, and issues **one** batched revert (`:2795`). Second shipped
precedent: `Renderer\Private\VT\AdaptiveVirtualTexture.cpp:872`.

**The caveat that matters most for this project (SOURCE-VERIFIED):** that batched
path engages **only when `UploadedBuffers.Num() > 1` in the same builder.**
Per-chunk `FRDGBuilder::QueueBufferUpload` calls made one at a time across
separate builders fall into the `else` branch at
`RenderGraphBuilder.cpp:2801-2833` and pay the full per-lock transition cost.
**INFERENCE:** batching uploads into one builder therefore needs no custom
transition code — the engine already does the right thing once N > 1.

One more per-unlock item worth knowing, SOURCE-VERIFIED
(`D3D12Buffer.cpp:800-830`): each unlocked buffer also does
`ConditionalClearShaderResource`, a real `FlushResourceBarriers()`, two
`UpdateResidency` calls (residency tracking is on — `ENABLE_RESIDENCY_MANAGEMENT 1`,
`D3D12RHI.h:29`), a `CopyBufferRegionChecked`, then `ConditionalSplitCommandList()`
— which closes the command list once `NumCommands > D3D12.MaxCommandsPerCommandList`
(**default 10000**, `D3D12CommandContext.cpp:15-21`), each split costing an extra
`ExecuteCommandLists` batch.

### 3.6 RDG's own guarantees

**DOCUMENTED** (Epic RDG docs): RDG performs "transitioning of sub-resources using
**split-barriers** to hide latency", transitions subresources "**across the
graph**", merges render passes, schedules async-compute fences, culls passes, and
records command lists in parallel. **Epic publishes no per-pass CPU cost number.**
**SOURCE-VERIFIED:** `FRDGBarrierBatchBegin::CreateTransition` creates **one**
`FRHITransition` for an **array** of `FRHITransitionInfo`
(`RenderGraphPass.cpp:267-276`), with Begin and End submitted at different passes
(`RenderGraphBuilder.cpp:3489-3502`) — genuine split barriers.

**This independently corroborates this project's own Trap 2** (RDG pass count is
not the tail): RDG is designed so that pass count is cheap and barriers are
amortised across the graph.

### 3.7 Epic ships an experimental Nanite VOXEL renderer in 5.8

**DOCUMENTED AS SHIPPED (engine source; no public documentation exists).**
`D:\UE_5.8\Engine\Source\Runtime\Renderer\Private\Nanite\Voxel.cpp` — 566 lines,
gated `!UE_BUILD_SHIPPING`, cvars `r.Voxel` (default 0), `r.Voxel.Method`,
`r.Voxel.Level2`, `r.Voxel.TileSize` (8). `DrawVisibleBricks` renders the whole
thing in **10 RDG passes**, fully indirect, from a screen-area-sized hash table of
visible bricks; shaders at `Engine\Shaders\Private\Nanite\Voxel\`
(`AutoVoxel.usf`, `TileBricks.usf`, `ScatterBricks.usf`, `RasterizeBricks.usf`).
**Zero per-brick CPU work.** Worth reading before designing anything new here —
it is Epic's own take on this exact problem shape, in the engine this project
already builds against.

### 3.8 What Part B did NOT establish

**The voxel / open-source half of the brief is not covered here.** Teardown,
Dreams, No Man's Sky, Sodium/Embeddium's arena allocator and multi-draw-indirect,
godot_voxel, NanoVDB / GVDB, sparse and tiled resources (`UpdateTileMappings`),
and the Aaltonen/Haar and Wihlidal GPU-driven talks were commissioned as a
separate investigation which **did not complete** — the session's WebSearch
budget was exhausted. **Stated as a gap rather than filled from recollection.**

That material is worth a follow-up, but note that §3.3's Nanite streaming result
already gives the canonical form of the answer those sources converge on — one
persistent buffer, sub-allocate by pointer bump, one dispatch, capped installs
per frame — so the marginal value is confirmation rather than new direction.

### Sources

- [A Deep Dive into Nanite Virtualized Geometry — Karis, Stubbe, Wihlidal, SIGGRAPH 2021 Advances](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf)
- [Using Resource Barriers to Synchronize Resource States in Direct3D 12 — Microsoft](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12)
- [Advanced API Performance: Barriers — NVIDIA](https://developer.nvidia.com/blog/advanced-api-performance-barriers/)
- [RDNA Performance Guide — AMD GPUOpen](https://gpuopen.com/learn/rdna-performance-guide/)
- [Render Dependency Graph in Unreal Engine — Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine)
- [Efficient Virtual Shadow Maps for Many Lights — Olsson et al.](https://efficientshading.com/2014/01/01/efficient-virtual-shadow-maps-for-many-lights/) (cited by the Nanite deck; not independently verified)
- UE 5.8 engine source at `D:\UE_5.8`, file:line throughout, read-only

---

## 4. Part C — ranked hypotheses

Ranked by expected value = (size of the prize) x (probability the experiment
settles it) / cost. Each names the existing switch or counter it uses. **Each can
come out the other way**, and the null result is written down.

### E0 — prerequisite, not a hypothesis: put a GPU clock on the frame line

Not a hypothesis; a precondition for trusting anything below. One line:
`RHIGetGPUFrameCycles()` into the existing `Voxel frame phase` line (§2.3). Until
it exists, every "RENDERBOUND" verdict in this project is a verdict about a
number that books dependent waits as busy (§2.2).

**Cost:** one line in `VoxelFramePhase.cpp`, which already includes
`RenderTimer.h`.
**Falsifier for the instrument itself:** `gpuMs` must read ~5.8 ms parked at full
res, matching the verified `ProfileGPU` split (`VoxelMarch.March` 3.169 of
5.842 total). If it reads 0.00, or reads the frame time, it is not measuring the
GPU and nothing below may quote it.

---

### H1 — The moving tail is periodic bursts of streaming-adjacent work, not per-chunk cost

**RANK 1.** Largest prize — it is the whole remaining Goal-3 gap — and the
evidence already narrows it hard.

**Claim.** The 6.10 ms (p95) / 10.10 ms (p99) moving-vs-parked gap is caused by
work firing on a *minority* of frames on a schedule or a trigger — ring
recompute, eviction sweeps, pool allocation windows, atlas fills, chunk-index
rebuilds — rather than by the marginal cost of the chunks each frame applies.

**Evidence for (all VERIFIED).**
- The delta is flat against GPU work: a 27% ray-count cut moved the tail delta
  -8% at p95 and 0% at p99 (§2.1).
- The delta is flat against per-frame chunk count: HEAVY-APPLY at 175.5
  chunks/frame has mean 9.90 ms, **5.30 ms below p95** (§1).
- The median cost of streaming is +0.70 ms and the mean render delta +1.11 ms. A
  tail 9x the mean is bimodal by construction.
- `docs/tick-budget-2026-08-24.md` §1 reached the same conclusion from the other
  side: the tail is bimodal, and mass left the 20-33 ms band in *both* directions
  as speed rose — which terrain difficulty cannot produce.

**Cheapest experiment that could REFUTE it. No new code.** One flag-free leg with
`-VoxelFramePhase=3` and **`-VoxelFramePhaseHeavy` swept 16 / 64 / 256**. The
instrument buckets frames by applies-per-frame and prints `frameMs`, `renderMs`
and `rhiMs` per bucket *within a single window*.
- **REFUTED** if `frameMs` rises steeply and monotonically with the threshold —
  i.e. frames applying more chunks *are* the slow frames after all. Concretely:
  if the HEAVY bucket's mean at threshold 256 reaches or exceeds the leg's p95
  (15.20 ms).
- **NULL RESULT looks like:** HEAVY mean flat at ~9.5-10.5 ms across all three
  thresholds while p95 stays ~15 ms. That is confirmation, and it says the slow
  frames must be found *by frame time*, not by chunk count.
- **This arm can fail its own validity check:** at threshold 256 the HEAVY
  population may be empty, in which case the instrument prints its own
  "THIS POPULATION IS EMPTY" line and the arm is **void, not passing**.

**Follow-on if confirmed** (a one-constant change, not a lane): lower the
`Hitch frame` bar from 33.3 ms to **12 ms** —
`VoxelDebug.h:460`, `inline constexpr float kHitchThresholdMs = 33.3f;`. That
block already prints `dispatchMs / applyMs / remeshMs / unloadMs /
componentsApplied / poolReuses` plus the per-thread timers, and its sub-lines
already split dispatch into `airProof/band/submit/pick/overlay/other` and
recompute into `recompute/fine/exitScan/queueFilter/sort` with per-ring entry
times. **Nothing in this tree currently describes a 15 ms frame** (§5); this makes
it describe one, with an instrument that already exists.

---

### H2 — The tail is game-thread dispatch bursts inside `FVoxelGpuMeshJobManager::Tick`

**RANK 2.** Directly evidenced at the hitch scale; unproven at the p95 scale.

**Claim.** The frames that miss are frames where the streaming tick's *dispatch*
stage runs long, and the render thread is merely downstream of it.

**Evidence for (VERIFIED).** In today's hitch population `dispatchMs` reaches
23-117 ms on individual frames while `applyMs` stays under 1 ms and `renderMs`
stays ~10 ms; the `other=` residual tracks `gpuMgrTickMs` (§2.9). Moving-leg mean
`tickMs` is only 0.79 ms, so this is purely a tail phenomenon. It is also the
stage the §0.0 backlog entry names (§6).

**Evidence against (VERIFIED).** `gameWaitMs` is 6.29 ms of a 9.67 ms moving
frame — **the game thread is idle 65% of a typical moving frame.** A cost that is
idle two-thirds of the time cannot set the median. It can still set the tail.

**Cheapest experiment that could REFUTE it. No new code.** Sweep `MeshBatchCap`
over **8 / 16 / 32 / 64** on matched flag-free legs (shipped default is **16**;
`-VoxelGpuPrimary` implies 64). Read `p95Ms`/`p99Ms` off the
`Voxel frame dist seg=SETTLED-MOVING scope=total` line and `dispatchMs` off the
hitch lines.
- **REFUTED** if p95/p99 are flat across the sweep while `dispatchMs` peaks track
  the cap. Then dispatch burst size is not what the tail is made of.
- **NULL RESULT looks like:** p95/p99 within run-to-run spread — this project's
  own spread on that metric is ~0.3-0.6 ms across three repeats of `ZZ-final` —
  at every cap.
- **This test has already half-run, and the prior weakens the hypothesis.** The
  recorded sweep gave `MeshBatchCap` 64/32/16/8 -> moving p99 35.0 / 22.0 / 21.3
  / 22.0 ms: a **knee at 16, no further gain at 8**. **INFERRED:** if shrinking
  the burst further does nothing, burst size is not the remaining lever. Re-run
  only to confirm the knee still sits at 16 on today's binary.

---

### H3 — The moving render delta lives in `tailMs`, attributable to one of six named subsystems

**RANK 3.** Modest prize — the mean render delta is only 1.11 ms — but the
experiment is **already built, has never been run, and can falsify itself.**

**Claim.** The moving-vs-parked render delta lives in `tailMs` (the render
commands enqueued by `VoxelGpuMeshJobManager`, `VoxelBrickPool`,
`VoxelMarchChunkIndex`, `VoxelResidencyGpu`, `VoxelGpuPoolComponent`, `VoxelGI`)
rather than in scene-renderer setup or RDG execute.

**Evidence for.** **INFERRED** from §2.7: all 29 sites scale with streaming, and
streaming is what motion causes. **VERIFIED** that terrain is not in the scene as
primitives, which removes the main alternative.

**Evidence against (VERIFIED).** The whole quantity being attributed is +1.11 ms
mean / +6.10 ms p95, and the instrument reports **means per segment** — so it may
resolve the 1.11 and never see the 6.10.

**Cheapest experiment that could REFUTE it. No new code.** Three legs matched to
the flag-free default:

1. **`-VoxelRenderFrameMutate=3 -VoxelRenderFrameMutateMs=2` FIRST.** This arm
   *blocks* 2 ms and asserts `mBase` busy **unchanged**, `sveBlocked` +2 ms,
   `renderBusy` **unchanged**. If busy rises by 2 ms, every bucket in the file is
   wall time wearing a busy label and nothing else may be quoted.
   **Given §2.2 — render-thread local-queue stalls are booked as busy by
   `bCountAsStall` — I expect this arm to be at material risk of failing, and I
   am writing that down before it runs.** A failure is a finding, not a wasted
   leg: it would mean the project's central "RENDERBOUND" claim rests on a
   contaminated counter.
2. `-VoxelRenderFrame=1` — the D1-D4 split.
3. `-VoxelRenderFrame=2` — the six tail buckets. **Not a control for leg 2:** the
   file prints `l2OverheadMs` and its share of `tailMs`, and the two `tailMs`
   figures may not be compared without subtracting it.

- **REFUTED** if `d(tailMs)` between MOVING and PARKED is **< 50%** of
  `d(renderBusy)` — the file's own pre-registered D4 criterion.
- **NULL RESULT looks like:** every bucket equal between MOVING and PARKED. The
  file's own rule says report that as *"the render frame does not respond to
  motion anywhere this instrument can see"* and **do not resolve it by picking
  the largest bucket.**
- **Pre-registered voiding readings:** no `Voxel render frame` line;
  `frames=0` in a segment; `recon=INVALID`; `families/frame > 1.01`;
  `renderBusyMs=0.00`; `dropped > 0`; `camSpeedMS ~ 0` on a MOVING line.
  `shadowMs=0.000` is the **correct** reading and is not evidence shadows are
  free.

---

### H4 — The >= 33 ms freezes are a separate defect from the p95 tail and must not be pooled with it

**RANK 4.** Small share of frames, large share of *felt* quality — and pooling it
with H1 would corrupt H1's measurement.

**Claim.** `maxMs=98.55` (full-res) / `112.90` (half-res) and the 8-9 hitches per
moving leg have their own cause — a stall, not busy work — and averaging them
into "the tail" produces a mechanism that is not there.

**Evidence for (VERIFIED).** The 92.59 ms frame in §2.9 has `renderWaitMs=74.26`
against `renderMs=14.23` and `rhiMs=7.56`: **registered idle**, on the thread this
project calls the bottleneck. Several other hitch frames show `elsewhereMs` of
65-158 ms with both threads' busy times normal. That is not a scaled-up p95
frame.

**Cheapest experiment that could REFUTE it. No new code, no new leg** — partition
the existing leg log's `Hitch frame` lines by whether `renderWaitMs > renderMs`.
- **REFUTED** if the hitch population is *not* bimodal — if hitches look like
  scaled-up p95 frames (busy-dominated, `renderWaitMs` small), one mechanism
  explains both and they should be pooled.
- **NULL RESULT looks like:** a single cluster. Today's 34 quoted lines look
  bimodal by eye (one cluster with `dispatchMs` 23-117 ms, one with
  `renderWaitMs` 25-134 ms and `dispatchMs` < 2 ms), but **I have not counted the
  whole population and will not claim the split until someone does.**
- **In-tree prior art arguing the same way:** the raster page rescue took
  `>= 200 ms` freezes from 10 to 1 and hitch time -47% **without moving p50** — a
  lever that touched only the tail.

---

### H5 — The `VoxelBrickPoolFlush` per-chunk loop is a live render-thread cost worth removing

**RANK 5, and last on purpose.** The code shape is alarming; the measured cost
leaves no room for it. **This is a "prove it executes" hypothesis, not an
optimisation.**

**Claim.** The per-chunk loop at `VoxelBrickPool.cpp:4322` — 6 RDG buffers, 5
uploads, 5 compute passes, ~19 views, and a double memcpy of ~38 KB per chunk
(§2.4) — is a material share of the render thread while streaming.

**Evidence for (VERIFIED).** The loop is live under today's defaults, it is
per-chunk, and `ERDGInitialDataFlags::None` genuinely copies twice.

**Evidence against (VERIFIED).** HEAVY-vs-LIGHT is **+0.42 ms of render thread
for +172.6 chunks**. Six buffers and five passes per chunk at 172 chunks cannot
cost 0.42 ms. **Either the loop runs for very few chunks (most chunks come from
the GPU fork and are never CPU-packed), or it is far cheaper than its shape.**

**Cheapest experiment that could REFUTE it. No new code.** Two readings:
1. `-VoxelRenderFrame=2` and read **`TailBrickPool`'s `h=` hit count** while
   streaming — the scope at `VoxelBrickPool.cpp:4258` already wraps this exact
   command.
2. The clean A/B: **`voxel.Brick.PackOnCpu 0`** (`VoxelBrickPool.cpp:223`,
   default 1) empties `GpuCpuWrites` and removes the loop entirely while leaving
   the GPU fork intact.

- **REFUTED** if `TailBrickPool` reads `h=0` while streaming (the command never
  fires — path dead, lane closes), **or** if `PackOnCpu 0` moves p95/p99 by less
  than run-to-run spread.
- **NULL RESULT looks like:** `h > 0, ms = 0.000` — it runs and costs nothing.
  Per that file's own rule, `h=0` with `ms=0` and `h>0` with `ms=0` are different
  findings and only the second means "cheap".
- **Do not propose work on any upload path without first proving the path
  executes.** Two past fixes aimed at `UploadCpuWrites_RenderThread` measured
  nothing, and `VoxelBrickPool.cpp:247-283` still contains a detailed, confident,
  **stale** cost analysis of that dead path (§2.4). It is the most inviting trap
  in this file.

---

### Two documentation defects found while scoping (report, do not fix here)

1. **`VoxelBrickPool.cpp:247-283`** analyses the CPU lock/unlock path as though it
   were live. It has been off-path since `PoolAlloc` shipped at 1 on 2026-08-24.
2. **`voxel.Terrain.RetireQuads`** is initialised `= 1` at
   `VoxelBrickPool.cpp:636`, but its own help text at `:641` says "default 0" and
   `VoxelBrickPool.h:1485` says "Off by default". **The code is right; both
   comments are wrong.** This is the project's own recorded failure mode —
   *counter names lie; read the site that writes the value.*

---

## 5. What no existing instrument can see

Stated plainly rather than assumed away.

1. **Nothing in this tree describes a 15 ms frame.** `Voxel frame dist` gives
   percentiles without attribution. `Voxel frame phase` gives attribution but
   only as **means per segment**. The `Hitch frame` block gives per-frame
   attribution but only above **33.3 ms** (`VoxelDebug.h:460`). The p95 band —
   which *is* the Goal-3 gap — falls in the hole between them.
2. **No per-frame GPU time exists** (§2.3): zero call sites for
   `RHIGetGPUFrameCycles`.
3. **`renderBusyMs` books dependent waits as busy** in Development builds (§2.2)
   and behaves differently in Shipping. Nobody has measured the difference.
4. **The `VoxelRenderFrame` mutation arms have never been run**, so the
   render-frame split is an instrument whose checks have never failed — and by
   this project's own rule, a check that has never failed is not yet known to be
   a check.
5. **A moving capture does not exist as a capability**, so no tail hypothesis can
   be judged visually while flying.

---

## 6. Part D — backlog §0.0, the marcher streaming regression

**Located:** `D:\voxelsim\docs\backlog.md:853`, *"NEW P0 (2026-08-22) — MAJOR
REGRESSION: marcher streaming is slower than the quad mesher it replaced"*,
running to `:967`. **It has not been updated since 2026-08-22.** No later entry
closes it, and no note anywhere in `docs/` supersedes it.

### What it actually claims, and what was actually measured

**VERIFIED** — the entry is owner-reported from the editor, with three symptoms
held to be one cause: (1) streaming slower than the pre-marcher quad system,
(2) chunks missing at LOD boundaries on initial load, (3) flying at normal speed
outpaces the streamer, leaving holes that fill in behind the camera.

Its evidence base is `Saved/Logs/VoxelEarth.log`, 2026-08-22 16:30-16:32, **two
PIE sessions back to back in one process**:

                              session 1                 session 2
    index entries @ 5 s         9,675                     1,531
    index entries @ 25 s       65,522                    26,106
    sustained fill rate    ~2,000-2,500 chunks/s   1,979 -> 880 -> 300/s
    streaming tickMs       0.269 then 0.007        0.267-0.281 sustained

and a second pass naming **two limiters with different owners**:

- **Cold fill — `recompute` dominates, and it is explicitly NOT marcher work.**
  Tick reaches 90% of wall; `recomputeMs` 2,815.7 ms per window decaying to
  85.2 ms; essentially all in the level-0 entry loop at ~14 us per footprint.
  Root cause named: `FootprintChunkZRangeCached`
  (`VoxelWorldSubsystem.cpp:8654`) **refuses to memoise while the fine tier has
  not finished decoding the tile** — correct behaviour, landed in `1e5207b`
  (2026-08-17) as a fine-tier correctness fix, and **it costs the quad path
  exactly the same.** The entry calls this "the leading explanation for chunks
  missing at LOD boundaries on initial load".
- **Steady state — `dispatch` dominates, and this one IS the marcher's.** Tick
  reaches **95.4% of wall**, `dispatchMs` **4,365.0 ms** per 5 s window, packs/s
  falling 2,388 -> 1,866 -> 1,383 as dispatch grows. The stage brackets put
  nearly all the unattributed remainder in one place: **`other` ~= `gpuMgrTickMs`
  in every sample** (4.19/4.73/4.95 against 4.10/4.66/4.90), i.e.
  **`FVoxelGpuMeshJobManager::Tick`**.

It also carries a separate, still-open sub-problem **(b): a second-PIE-session
throughput collapse** — session 2 fills at about a seventh of session 1's rate
and its streaming tick never drops. The teardown bug fixed on 2026-08-22 (brick
pool and chunk index never released with the `UWorld`) is verified working and
**did not address this**.

### Is it the same problem as the +11.3 ms render tax?

**No — they were different problems, and they have since diverged further. But
one half of §0.0 is the direct ancestor of this programme's leading tail
hypothesis.** Taking the three parts separately:

**(a) The throughput half is LARGELY RECOVERED. VERIFIED.**

    §0.0, 2026-08-22          today, 2026-08-26 (TJDL-A-control, flag-free default)
    ~2,000-2,500 chunks/s     8,161 chunks/s cold settle
    decaying to 300/s         no decay; all 7 rings settle by 6.1 s
                              (R0 1.9 s, R1 2.2, R2 2.7, R3 3.4, R4 4.4, R5 5.9, R6 6.1)
    tick 95.4% of wall        tickMs 0.79 ms = 8% of a 9.67 ms moving frame
    dispatch 4,365 ms/window  dispatch mean well under 1 ms

**Different units, different thread, different question.** §0.0 measures
**chunks per second and world completeness on the game thread**. The +11.3 ms
tax measured **milliseconds per frame on the render thread**. They were never the
same quantity, and §0.0's own closing line says so: *"Do not read frame time as
progress on this item. The thing being fixed is chunks per second and holes at
boundaries."*

**(b) The dispatch half is STILL ALIVE, and it is now a TAIL phenomenon — this is
the real link. VERIFIED.** §0.0's signature is `other ~= gpuMgrTickMs` inside
`dispatchMs`. Today's leg shows exactly that signature, but only on hitch frames:
`dispatchMs` of 117.18 / 87.55 / 76.19 / 70.73 / 69.12 / 58.93 ms with the `other`
residual tracking `gpuMgrTickMs`, and **`gpuMgrTickMs` peaking at 96.0 ms on a
single frame** (§2.9, Shape 1). The mean is now under 1 ms; the maximum has not
moved much.

**So §0.0's dispatch mechanism changed shape rather than going away: it stopped
being a sustained throughput ceiling and became a burst.** That is precisely
**H2** in §4, and §0.0's own unfinished action item — *"Not yet checked, and
cheap: whether `gpuMgrTick` is doing work proportional to the job queue or
re-scanning something per tick. That is one read of
`VoxelGpuMeshJobManager::Tick` and it is where this should resume"* — **is still
the cheapest next step and should be adopted verbatim into this programme.**

**(c) The second-PIE-session collapse is UNTESTED and structurally invisible to
this programme's instruments.** Every headless leg is a fresh process, so no leg
run since — including all four TJDL legs — exercises a second session in one
process. **Nothing in §1-§5 of this document is evidence about (b) in either
direction.** It needs an editor session, not a leg.

### What this changes about the programme's framing

The brief asks whether this reframes the programme from *"optimise streaming"* to
*"recover a regression"*. **Neither, on today's evidence.**

- The throughput regression **has already been recovered** (a), so there is no
  regression left to recover on that axis.
- The render tax **was never the same problem**, and has itself shrunk 10x (§0).
- What survives is **one mechanism, `FVoxelGpuMeshJobManager::Tick`, which used
  to cap throughput and now spikes the tail.** That makes the programme
  *"find what makes a bad frame bad"*, with §0.0's dispatch finding as the single
  strongest prior.

**Two things §0.0 asks for that have still never been done, and both are cheap:**

1. **The quad-vs-marcher producer A/B has never been run.** `voxel.March 0` +
   `voxel.Terrain.RetireQuads 0`, same flight, reading chunks/s and LOD-boundary
   behaviour. Its prediction is already written down and falsifiable: *turning the
   marcher off should remove most of the dispatch cost and leave most of the
   recompute cost in place; if both drop the z-range memo attribution is wrong;
   if neither drops both attributions are wrong.* **Note the trap the entry
   records: the producer switches are command-line / ini only —
   `-ExecCmds` lands after streaming has begun and will silently not apply
   (`VoxelBrickPool.cpp:286-304`).** A leg that sets them via `-ExecCmds` will
   look like a null result and will not be one.
2. **`FVoxelGpuMeshJobManager::Tick` has never been read for whether its work is
   proportional to the job queue.** One file read.

### Recommendation on the entry itself

**§0.0 should be split and partly closed, not carried forward whole.** Carrying a
2026-08-22 P0 whose headline claim ("streaming is slower than the quad mesher")
is contradicted by today's 8,161 chunks/s is how a stale number gets quoted back
as a requirement — the same failure the SCOREBOARD records for the invented
"<= 0.10% stutters" gate. Proposed split:

- **(a) throughput** — mark RECOVERED with today's leg cited, and note that the
  original claim was never quantified against a pre-marcher build, so "slower
  than the quad mesher" was **never actually measured** and cannot now be.
- **(b) second-PIE collapse** — keep OPEN as its own item, tagged
  *editor-only, no leg can see it.*
- **(c) dispatch / `gpuMgrTick`** — fold into this programme as **H2**, keeping
  the original prediction and the `-ExecCmds` trap.
- **(d) the `FootprintChunkZRangeCached` cold-fill cost** — keep OPEN as its own
  item, tagged *not marcher work; costs the quad path equally.*

---

## 7. Run order, and what would close the programme

Every step below uses an existing switch or an existing log line. Only E0 and
H1's follow-on touch code, and both are one line.

    step  what                                    cost        what it settles
    ----  --------------------------------------  ----------  ---------------------------------
    E0    gpuMs on the frame-phase line           1 line      retires the busy-vs-backpressure
                                                              ambiguity permanently (§2.3)
    H3.1  -VoxelRenderFrameMutate=3               1 leg       whether renderBusyMs means anything
                                                              at all. RUN THIS BEFORE H3.2/3.3
    H1    -VoxelFramePhaseHeavy sweep 16/64/256   1 leg       is the tail per-chunk or bursty
    H4    re-read the existing leg log            0 legs      are the >=33 ms freezes a separate
                                                              defect from the p95 tail
    D2    read FVoxelGpuMeshJobManager::Tick      0 legs      is gpuMgrTick's work proportional
                                                              to the job queue (backlog §0.0's
                                                              own unfinished action item)
    H1'   kHitchThresholdMs 33.3 -> 12.0          1 const     makes the existing per-frame
                                                              attribution block describe a p95
                                                              frame for the first time
    H2    MeshBatchCap sweep 8/16/32/64           4 legs      is the tail dispatch burst size
                                                              (prior says no; confirm the knee)
    H5    TailBrickPool h= , then PackOnCpu 0     2 legs      is the per-chunk flush loop hot
    D1    voxel.March 0 + RetireQuads 0 A/B       2 legs      backlog §0.0's never-run producer
          (command line / ini ONLY, never                     comparison
           -ExecCmds -- see §6)

**The programme closes when one of these is true:**

- a named mechanism accounts for >= 4 ms of the 6.10 ms p95 gap, with a switch
  that turns it off and a leg that shows p95 move; **or**
- all of H1-H5 come back null, in which case the honest report is *"the moving
  tail is not visible to any instrument this project has"* and the next
  investment is instrumentation (§5), not optimisation.

**Both outcomes must be reportable.** By this project's own rule, a programme
that can only conclude "we found it" is not a programme.

### Three things this document deliberately does NOT propose

1. **Anything aimed at the marcher.** It is 54% of the GPU frame and ~0.5 ms of
   a 10.10 ms tail (§2.1). The marcher programme's remaining ~0.2-0.4 ms of
   uncontested headroom is real and is worth taking on its own merits, but it is
   not this problem and must not be scored against this gate.
2. **Anything aimed at streaming throughput.** 8,161 chunks/s cold, seven rings
   settled in 6.1 s, `tickMs` 0.79 of a 9.67 ms frame, game thread idle 65%.
   Deleting 100% of remaining game-thread streaming work does not reach 100 fps,
   and §0.0's throughput half is recovered (§6).
3. **Any rewrite of the CPU upload path** before H5 proves the path executes.
   Two past fixes there measured nothing, and `VoxelBrickPool.cpp:247-283` still
   contains a confident, detailed, **stale** cost analysis of a dead path (§2.4).

### Provenance

Written 2026-08-27 from: `D:\voxelsim\Saved\TJDL-{A-control,B-halfstatic,C-halfjit1,D-halfjit2}.log`
(2026-08-26, flag-free shipping default, `view=1552x873`, `DOUBLE GRANT: 0` on
all four); `D:\voxelsim\docs\{SCOREBOARD.md,backlog.md,render-frame-split-2026-08-24.md,
frame-phase-2026-08-24.md,tick-budget-2026-08-24.md,apply-fast-path-2026-08-23.md}`;
the voxel plugin sources under `D:\voxelsim\ue-project\Source`; and UE 5.8 engine
source at `D:\UE_5.8`. No file in the repo was modified except this document.

**One numeric coincidence, flagged so nobody trips on it.** `docs/apply-fast-path-2026-08-23.md`
prices apply at **54.5 us of GAME thread per chunk** — numerically identical to the
retired **0.054 ms of RENDER thread per chunk**. They are different threads,
different call sites and different dates. **Do not quote one as the other.**
