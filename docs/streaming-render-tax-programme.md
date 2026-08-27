# The streaming render tax: scoping

**Date:** 2026-08-26
**Status:** scoping. Nothing here is built. Every hypothesis carries the experiment
that could refute it.
**Predecessor:** the marcher programme, closed the same day — see
`docs/SCOREBOARD.md` and [[voxelsim-marcher-cost-is-ray-count]]. Eight approaches
refuted; ~0.2-0.4 ms of uncontested headroom left there. The remaining gap is here.

---

## 0. THE GAP, STATED HONESTLY

    goal                          status
    p99 >= 50 fps while moving    MET, at 50.8 fps -- one leg of three below the line
    steady-state >= 100 fps       NOT MET in any arm ever measured

`gate=` reads **GOAL3-FAIL** on every leg on disk. `VoxelFramePhase.h:126` says that
field is the AND of gateP95 and gateSteady and is **the only field that may be quoted
as the result**. Best p95 measured is 12.10 ms (83 fps, half-res); the control is
15.40 ms (65 fps). The gate wants p95 < 10.00 ms.

**Where the cost is** (all from matched legs, see SCOREBOARD for provenance):

    SETTLED-PARKED    gameBusy  1.69   renderBusy  9.23   108 fps   RENDERBOUND
    SETTLED-MOVING    gameBusy 12.20   renderBusy 18.60    54 fps   RENDERBOUND

**Parked already meets target. Moving costs +11.3 ms and it is RENDER THREAD.**

Bucketed within ONE leg so leg-to-leg variance cannot enter: applying 163 chunks/frame
costs **+10.20 ms of frame, of which +8.87 ms (87%) is render thread** and only
+0.95 ms is game thread. ~0.054 ms of render thread per chunk, independently
reproduced as 0.065 from a different route. **RHI time nearly quadruples,
1.68 -> 6.35 ms** — command-translation-shaped.

---

## 1. THREE TRAPS. Read these before proposing anything.

**1. `renderMs` IS NOT CPU TIME.** `RHICommandList.cpp:1940` `WaitOnRHIThreadFence`
wraps its block in `FScopeNonCriticalPath`, **not** `FScopeIdle`, and only `FScopeIdle`
adds to `Waits`. So **GPU backpressure lands in `GRenderThreadTime` as BUSY**. Turning
off ONE GPU compute pass whose CPU setup measures 0.048 ms dropped "render-thread busy"
by **5.40 ms**. `renderMs`, `renderBusyMs` and `setupOther` are all downstream of GPU
cost. When the frame says RENDERBOUND, **look in the shaders before the C++**.

**2. RDG PASS COUNT IS NOT THE TAIL.** Driving peak passes/tick **23x** higher
(25 -> 64 mean, 76 -> 1729 max) moved p99 by **0.6 ms**, inside run-to-run spread.
Any plan whose thesis is "build fewer RDG passes" is already refuted. Total prunable
dead passes across the whole render frame: **0.033 ms**.

**3. THE DEAD PATH.** `voxel.GPU.MeshDirectToPool` defaults **1**, so meshed chunks go
straight into the brick pool with no readback. They never become `PendingWrites`, never
carry a `CpuPack`, and never reach `UploadCpuWrites_RenderThread`. **Two fixes have
already been aimed at this dead code and measured nothing.** See §2.0 — a third nearly
was.

---

## 2. HYPOTHESES, RANKED

### 2.0 REFUTED BEFORE BUILDING: the 4x per-chunk LockBuffer path

A sweep flagged `VoxelBrickPool.cpp:3776-3872` — four independent
`LockBuffer`/`Memcpy`/`UnlockBuffer` cycles per chunk on the **immediate** command
list (`:3794` Occ, `:3812` Mat, `:3829` Desc, `:3860` ChunkTable), each doubled by
D3D12's unconditional `NeedsExtraTransitions` (`D3D12RHI.cpp:262`). At 163 chunks that
is ~652 lock pairs and ~1,304 `FRHITransitionInfo`s per frame. Compelling, and it is
the right SHAPE for a 1.68 -> 6.35 ms RHI move.

**It is dead code at runtime.** The reasoning that flagged it read
`GVoxelBrickPackOnCpu = 1` (`:223`) and concluded the function does work. The real gate
is whether any entry in `Writes` carries a valid `CpuPack`, and under
`MeshDirectToPool=1` **none do** — the function early-returns at `NumCpuWrites == 0`.

**The proof, and it is free:** `brick-cpuupload` appears **6 times in the source and
0 times in any log on disk**, tonight's legs included.

    grep -c "brick-cpuupload" Saved/<leg>.log     # 0 on every leg

**THIS IS THE THIRD FIX AIMED AT THIS PATH.** The other two were a lock-coalescing fix
and `voxel.GPU.BrickFlushBatch` (measured `fused=0`, its `[brick-flushbatch]` line never
printed either). **Before proposing work anywhere in `UploadCpuWrites_RenderThread` or
`AddFlushPasses_RenderThread`, grep the logs for that path's own line.** A cvar default
is not proof a function runs.

---

### H1. UAV barriers between per-chunk pool-write passes — **STRONGEST LIVE CANDIDATE**

**Claim.** `VoxelGpuPoolComponent.cpp:3487` loops per chunk, and each iteration adds an
RDG compute pass writing the **same** destination UAVs (`VoxelGpuWorldGen.cpp:3179`
`DstQuads`, `:3180` `DstIds`). **`ERDGUnorderedAccessViewFlags::SkipBarrier` occurs
ZERO times in the entire Source tree** (verified count, 190 files). So RDG inserts a
full UAV barrier between every consecutive pair: **163 dispatches = 163 pipeline
drains.** The passes write **disjoint destination ranges** (each chunk owns its own
slice), so the barriers are almost certainly unnecessary.

`voxel.Terrain.RetireQuads` (default 1) fills the `Hides` list, which roughly doubles
this loop's pass count via the second loop at `:3515`.

**Why it survives trap 2.** Trap 2 refuted *pass CONSTRUCTION cost on the CPU*. This is
not that — it is **GPU serialisation**, and a drained pipeline shows up as backpressure,
which trap 1 says gets billed to the render thread as busy. The two traps together
predict exactly the symptom observed.

**Cheapest refutation.** There is no cvar; it needs a code change. But the *direction*
is testable for free first — see E0. If E0 says the +8.87 ms is CPU-busy rather than
backpressure, **H1 is refuted without writing any code**, because a barrier costs GPU
serialisation and nothing else.

**Null result looks like:** adding `SkipBarrier` to those two UAVs moves `VoxelMarch`-
adjacent GPU time by < 0.3 ms and p95 by less than run-to-run spread.

**Correctness precondition, non-negotiable:** `SkipBarrier` is only sound if the ranges
really are disjoint. Prove it from the allocator, not from the loop's shape.

---

### H2. Per-chunk RDG buffer creation on the live direct-to-pool path

**Claim.** `VoxelGpuMeshJobManager.cpp:4920` (`DispatchQuadCompact`) loops per payload
and does, per chunk: `RegisterExternalBuffer` (`:4927`), **`GraphBuilder.CreateBuffer`**
(`:4930`), a compact pass, and **`ConvertToExternalBuffer`** (`:4947`) — a pooled-buffer
extraction that can **grow the `RenderGraphResourcePool` during streaming**.

**This is the `MeshDirectToPool=1` default path** — unlike §2.0, it genuinely runs. And
unlike H4/H5 it is **not batch-capped**: the batch comes from delivered payloads, not
from `MeshBatchCap`.

**Cheapest refutation.** Instrument the pool's high-water mark across a moving leg
against a parked one. If the pool is not growing, the extraction is cheap and this is a
null. Existing hook: the render-frame tail sites (`-VoxelRenderFrame=2`) already carry
29 wired scopes; `meshJob` is one of the **UNWIRED** ones, so check `groupsUnwired=`
before reading `h=0` as evidence of anything.

---

### H3. Non-merged dirty runs — **has a free cvar, do this one first among the builds**

**Claim.** `VoxelGpuPoolComponent.cpp:1473` loops per dirty run doing 2-3
LockBuffer/Unlock pairs (`:1481` Quad, `:1489` ChunkId, `:1498` Corner). Merging is
**off by default**: `GVoxelPoolDirtyMergeGap = 0` (`:3170`), and the file's own comment
at `:3156` says "while not merging costs an extra LockBuffer/UnlockBuffer per run".
Under `RetireQuads` each retirement adds a **discontiguous** dirty range, so run count
tracks chunks touched. D3D12's transition doubling applies here too.

**Cheapest refutation — a cvar sweep, no code:**

    voxel.Pool.DirtyMergeGap 0 (control) / 64 / 1024

**Null:** p95 flat across the sweep. **Watch for:** merging over-writes untouched
ranges, so a large gap trades lock count for upload bytes. There is a knee; find it
rather than maximising.

---

### H4. `CreateUniformBufferImmediate` per range, per view, per frame

**Claim.** `VoxelGpuPoolComponent.cpp:1374`, three loops deep (`:1135` views,
`:1339` batches, `:1361` ranges). The file names its own number at `:1201`:
**"~8,000 `CreateUniformBufferImmediate` calls"**, and at `:1196`
**"13.49 ms of a 13.41 ms frame"**.

**This does not scale with chunks/frame — it scales with pool FRAGMENTATION**, which
`RetireQuads` drives up, multiplied by every shadow cascade. So it is a **steady-state**
term, not a burst term, and it may be a large part of why *parked* costs 9.23 ms of
render thread.

**Cheapest refutation.** `voxel.Stream.GPUCullDebugDrawNothing 3` zeroes submission
while **keeping** the render-thread cull cost; `7` drops both. `(control − 3)` bounds
the prize from the emit; `(3 − 7)` bounds the cull walk itself. Both already exist, and
the driver checks that `cameraGathers` HOLDS while `camQuads` goes to 0, so the zero is
a measured zero and not an absence.

---

### H5. It is not the CPU at all

**Claim.** Given trap 1, some unknown fraction of the +8.87 ms is the GPU being behind,
not the render thread being busy. The verified parked GPU split is `VoxelMarch.March`
**3.169 ms (54.2%)**, TSR ~1.60 (27%), ~40 other passes ~0.95 (16%) of 5.842 ms total.
Under streaming the pool-write and mesh passes are added to that.

**If this is dominant, the whole programme redirects at shaders and H1-H4 are noise.**

---

## 3. EXPERIMENT ORDER

### E0 — SETTLE BACKPRESSURE vs BUSY. Do this first; it reorders everything after it.

Nothing else should be built until this is answered. It decides whether the programme
is about CPU-side streaming work (H1-H4) or about GPU cost (H5).

Two matched legs, `line` flight, `-VoxelRenderFrame=2 -VoxelFramePhase=3`:

    A  control
    B  voxel.Stream.GPUMaxChunks 1     <- pool refuses new chunks; admission and
                                          dispatch still run, results discarded

`VoxelWorldSubsystem.cpp:23528-23531` — once `GetNumChunks() >= PoolCap`,
`ApplyMeshResult` returns false for every new chunk. **Read live, flippable mid-leg.**

Then a `ProfileGPU` capture on each (`voxel-run-gpu-arm.ps1` appends
`voxel.DeferExec <n> ProfileGPU`). **The question: does the GPU frame total move by
roughly the same amount `renderBusyMs` does?** If yes, it was backpressure and H5 wins.
If `renderBusyMs` falls and GPU total does not, it is genuinely CPU-side.

**This test can fail in both directions**, which is why it is first.

### E1 — H3's cvar sweep (free, no code)
### E2 — H4's draw-nothing bounds (free, no code)
### E3 — H1's `SkipBarrier`, only if E0 says backpressure and only after proving range disjointness
### E4 — H2's pool high-water instrumentation

---

## 4. WHAT IS ALREADY CLEAN — do not go looking here

All VERIFIED by exhaustive count, not sampling:

- **`EImmediateFlushType` — 0 occurrences repo-wide.** No `ImmediateFlush` anywhere.
- **`SubmitAndBlockUntilGPUIdle` — 2, both dead on the streaming path**
  (`VoxelGpuWorldGen.cpp:4018`, `:4259`, both console-driven verification).
  `VoxelGpuMeshJobManager.cpp:4810` asserts the streaming path does not do it.
- **`FlushRenderingCommands` — 14, all dead on the streaming path** (verify helpers and
  debug probes).
- **No `SubmitCommandsHint`, no `RHIThreadFence`, no `BlockUntilGPUIdle`, no
  `RHIReadSurfaceData`** anywhere in Source.
- `VoxelResidencyGpu.cpp:1033` creates 6 readbacks per dispatch — **per-frame, not
  per-chunk**. Low priority.

**Free hoisting, unrelated to any hypothesis** (all verified in-loop, none hoisted):
`TShaderMapRef` + `GetGlobalShaderMap` at `VoxelGpuWorldGen.cpp:3157, 3182, 3204, 3232`
are re-looked-up per chunk, as are `CreateUAV` calls on **invariant** destination
buffers (`:3179, :3180, :3202`). Cheap individually; free to fix; not a hypothesis.

---

## 5. IS THIS A REGRESSION RATHER THAN AN OPTIMISATION?

**Open, and it changes the programme's framing.** There is a standing owner-flown report
that **marched terrain streams SLOWER than the quad path it replaced** (backlog §0.0),
which by the owner's own judgement outranks the marcher's 2.72x draw-path win.

If that regression and this +11.3 ms are the same phenomenon, the goal is **recovering
a known-good baseline**, not inventing new technique — and the quad path's numbers
become the target rather than a guess. **Settle this before committing to H1-H4.**

Not established: I did not get to read the backlog entry in this pass.

---

## 6. WHAT THIS DOCUMENT DOES NOT HAVE

**The open-source and published research half was never done.** The agents tasked with
it (voxel GPU-driven streaming; D3D12 barrier-cost guidance; Nanite/VSM per-instance CPU
scaling; RDG documentation) all terminated on an API rate limit before returning
findings. One got as far as reporting that **search engines are blocked from this box**
and that primary sources must be fetched directly by URL.

**So every claim in this document is from THIS repo and THIS engine's source.** Nothing
here is informed by how other engines solve it. The specific questions still worth
answering from outside:

- How do Nanite and Virtual Shadow Maps keep per-instance CPU cost flat as instance
  count grows? Closest shipped analogue to what this project needs.
- Documented cost of D3D12 UAV barriers between consecutive compute dispatches — H1
  rests on an assumption about their price that no measurement here supports yet.
- Whether `ExecuteIndirect` / multi-draw-indirect removes per-chunk render-thread work
  in practice or merely moves it.
- Sparse/tiled resources and persistent descriptor heaps for streaming without
  per-object render commands.

---

## 7. STANDING RULES FOR THIS PROGRAMME

- **Read the `++` site, never the counter's name.** Names here have lied and inverted
  conclusions (`promoteExit cap=` means `MaxInFlight`; `sveBlockedMs` is not
  SVE-specific; `FAIL:` is a section label, not a verdict).
- **A confirmation that cannot come out the other way is not a confirmation.**
- **Grep the logs for a path's own line before optimising it.** §2.0 is the third fix
  aimed at the same dead function.
- Before quoting ANY leg: `grep -c "DOUBLE GRANT"` must be 0, and read the render size
  from the marcher's own `view=` line, never the harness banner.
- `tools/leg-summary.sh <name>`, never `grep | tail -1` (lands in the linger window
  where everything reads zero).
- `-Cvars` separator is **COMMA**. UE splits `-ExecCmds` on comma only; a `|` is
  silently appended and the rest dropped. This voided a five-arm experiment on
  2026-08-26.
- Spawn `-84480,53760` is a **fatal** fine-tier gate leak. Use `-61440,-61440`.
- The box is shared. `Get-Process` (PowerShell), not `tasklist`.
