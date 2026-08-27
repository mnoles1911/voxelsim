# The +5.47 ms GPU term: what it can be, and the cheapest experiment that names it

Extends `docs/pipeline-waves-2026-08-27.md` **Wave 2**. Written 2026-08-27 from a
**read-only** survey — nothing was built, no leg was run, the build lane was busy
(`UnrealEditor-Cmd` pid 6684) for the whole of it. Every claim below is labelled
**VERIFIED** (I read that exact line), **INFERRED** (derived from VERIFIED lines),
or **CITED** (quoted from a comment, which on this project is *not* the same as
verified).

The target:

    term       FAST     p99     delta
    gpu        6.99    13.36    +5.47    <- unattributed
    chunks     18.7     80.1    +61.4    <- strongest correlate

---

## 0. The single most important thing, and it was free

**The new clock measures GPU BUSY time. Idle gaps are excluded by construction.**

`GPUProfiler.cpp:2135` pushes the platform's `MaxQueueBusyCycles` when the RHI
supplies one; otherwise `:2145` pushes `FTimestampStream::ComputeUnion`, whose
own first line reads (VERIFIED, `D:\UE_5.8\Engine\Source\Runtime\RHI\Private\GPUProfiler.cpp:943-944`):

    // The total number of cycles where at least one GPU pipe was busy.

The accumulator at `:972-976` only adds an interval **while `BusyPipes > 0`**.
Gaps where no pipe is busy are never counted.

Three consequences, and they close questions rather than open them:

1. **A CPU-side stall cannot inflate `gpu=`.** Shader/PSO compilation, a render-thread
   hitch, a fence wait — all of these *starve* the GPU. Starvation is idle. Idle is
   excluded. **The +5.47 ms is real extra GPU work executed, not the GPU waiting.**
   (INFERRED from the VERIFIED lines above. This retires falsifier candidate (a)
   in the brief before any leg is run — see §6.)
2. **Overlap is counted once.** The union charges concurrent pipes a single time. But
   this project has **zero async compute**: `grep -rc "AsyncCompute" ue-project/Source
   --include=*.cpp` returns **no matching files**, and `ERDGPassFlags::AsyncCompute`
   has **0 call sites** (VERIFIED). Everything runs on graphics queue 0, serialised.
   So streaming compute *cannot* hide behind the raster, and its cost adds fully.
3. **Pairing is approximate and the instrument says so itself.** `VoxelWorldSubsystem.cpp:6771-6776`
   (CITED): *"IT IS STILL NOT THIS FRAME'S GPU TIME ... gpu= correlates with the
   bucket rather than belonging to it exactly."* The drain loop at `:10597-10601`
   pops to the freshest sample and `LastGpuMs` (`:6778`) **carries forward when the
   queue is empty**, so a frame can be charged a repeat of the previous frame's
   number. That is tolerable for a bucket mean and is *not* a basis for naming a
   single frame.

---

## 1. What actually scales with chunks streamed

The default configuration decides this, and two of the defaults are not what the
comments say. Read the accessor, never the header comment.

| knob | file:line | comment/declared | **effective default** |
|---|---|---|---|
| `VoxelGpuPrimary` | `VoxelGpuMeshJobManager.cpp:358` | — | **1 (ON)** VERIFIED |
| `voxel.GPU.MeshBatchCap` | `VoxelGpuMeshJobManager.cpp:93` | 4 | **64** via `VoxelGpuMeshBatchCapEffective()` `:394` VERIFIED |
| `voxel.GPU.MeshHarvestCap` | `VoxelGpuMeshJobManager.cpp:147` | 8 | **0 = UNLIMITED** under primary, `:427` VERIFIED |
| `-VoxelGpuLeanBrickJobs` | `VoxelGpuMeshJobManager.cpp:454` | **"Default OFF"** (`:453`, CITED) | **ON under primary** — accessor `:466` returns `VoxelGpuPrimaryEnabled()` VERIFIED |
| `voxel.GPU.PoolAlloc` | `VoxelBrickPool.cpp:977` | — | **1 (ON)** VERIFIED |
| `voxel.GPU.BrickFlushBatch` | `VoxelBrickPool.cpp:748` | — | **0 (OFF)** VERIFIED — the fused arm is written and disabled |
| `voxel.March.IndexDeltaUpload` | `VoxelMarchChunkIndex.cpp:77` | **"default 0"** (`:2463`, CITED) | **1** VERIFIED |
| `-VoxelGpuResidency=` | `VoxelResidencyGpu.cpp:468-470` | — | **0 = the whole 12-pass module is inert** VERIFIED |
| `voxel.March` | `VoxelMarchRenderer.cpp:63` | help says *"0 ... IS THE DEFAULT"* (`:66-70`, CITED) | **1** VERIFIED |

**`-VoxelGpuLeanBrickJobs` and `voxel.March`'s help string are two more lying
names, on top of the five in the house rules.** `voxel.GPU.BrickFlushBatch`
defaulting OFF is the load-bearing one for everything below.

### The live per-chunk / per-tick split

Under the default arm (worklist all ON — `VoxelGpuMeshJobManager.cpp:599,651,686,720,790,823`,
VERIFIED), the bulk stages inside `AddRegionPasses` are **skipped per chunk** and
run once per tick as indirect dispatches (`VoxelGpuWorldGen.cpp:2199`, `:2245`,
`:2596`, `:2645` are the skip gates, VERIFIED). So there are two populations:

**Per TICK — constant pass count, thread count ∝ `Take`** (`Take = min(Pending, 384)`,
`VoxelGpuWorklist.cpp:465,480`, published as `LastFlush.Take` at `:627`):

| pass | AddPass | groups | threads per chunk-record |
|---|---|---|---|
| WorklistColumn | `VoxelGpuWorldGen.cpp:2955` | `Take×16` | 1,024 |
| **WorklistVoxelize** | `:3004` | `Take×16` | 1,024 threads × **32 z-cells = 32,768 density evals** |
| WorklistClassify | `:3045` | `Take×64` | 4,096 |
| WorklistPack | `:3136` | `Take×64` | 4,096 |
| **WorklistClaimWrite** | `:3796` | `Take×148` | **9,472 — worst-case sized, not actual** |
| + Args/Totals/Claim/Record/Consume/AssetStamp | `:2955`–`:3845`, `VoxelGpuWorklist.cpp:984,1350` | `Take×1` each | 64 each |

≈ **12 dispatches per tick, ~19,000 threads per chunk.** VERIFIED sizes; thread
arithmetic INFERRED from the group constants (`VoxelGpuWorklist.h:191,197,198,216,225`).

**Per CHUNK — this is the term with the tail shape** (`VoxelBrickPool.cpp:4322-4365`,
the GPU claim loop that `voxel.GPU.PoolAlloc 1` diverts CPU-fed chunks into at
`:2643-2665`, VERIFIED):

| # | AddPass | shader | size | note |
|---|---|---|---|---|
| B1 | `VoxelGpuWorldGen.cpp:3579` | `FVoxelBrickPoolClaimCS` | `(1,1,1)` | 1 group |
| B2 | `:3620` | `FVoxelBrickPoolAllocWordCopyCS` | `ceil(OccWords/64)` ≤ 16 | |
| B3 | `:3634` | `FVoxelBrickPoolAllocWordCopyCS` | `ceil(MatWords/64)` ≤ 132 | |
| B4 | `:3650` | `FVoxelBrickPoolAllocDescCS` | **1 group** | |
| B5 | `:3674` | `FVoxelBrickPoolAllocRecordCS` | `(1,1,1)` | 1 group |

plus **five transient `CreateStructuredBuffer` staging uploads per chunk**
(`VoxelBrickPool.cpp:4335,4338,4341,4345,4351`, VERIFIED): Totals 8 B, Mask 8 B,
Desc 512 B, Occ ≤4,096 B, Mat ≤33,792 B — **≤37.6 KiB resident + the same again
as staging traffic.**

**The structural point (INFERRED from VERIFIED lines):** all N chunks' passes
declare UAV access on the **same four pool buffers**, registered once at
`VoxelBrickPool.cpp:4306-4312`. RDG serialises same-resource writers in recording
order, so the graph contains roughly **5N dispatches and 5N UAV barriers** with
three of the five dispatches being a *single thread group*. At 80 chunks that is
~400 dispatches, ~400 barriers, ~400 transient buffer allocations and ~3 MB of
staging copies **in one frame**, to move ~3 MB of payload. The useful ALU is
negligible; the cost is launch and barrier latency.

### Cost model sanity check

Per-chunk marginal cost required to explain the spike:

    5.47 ms / 61.4 extra chunks  =  89 us per extra chunk
    5.47 ms / 80.1 total chunks  =  68 us per chunk

For the per-chunk claim loop that is **~18 µs per dispatch+barrier+staging-upload
triple**, or ~9 µs if you charge the barrier and the copy separately. For an RDNA3
part a UAV barrier that drains the pipeline costs single-digit microseconds and a
small transient buffer create + upload-heap copy + transition is comparable. **The
arithmetic lands inside the plausible band without needing anything unusual.**
(INFERRED. This is a magnitude argument, not a measurement — §3 is how you settle it.)

---

## 2. Ranked candidates

| # | candidate | file:line | required µs/chunk | verdict |
|---|---|---|---|---|
| ~~1~~ | ~~**Per-chunk pool claim loop**~~ | `VoxelBrickPool.cpp:4322-4365` | — | **RETIRED 2026-08-27, from logs already on disk. BOTH HALVES OF THIS ROW WERE WRONG — see §2.1.** |
| **2** | **Worklist voxelize + claim-write** — 32,768 density evals and 9,472 worst-case copy threads per chunk-record | `VoxelGpuWorldGen.cpp:3004`, `:3796` | 89 | **Strong, but wrong shape.** Constant pass count, thread count ∝ `Take`. It would move the *median* about as much as the tail. Biggest absolute GPU term; weakest tail explanation. |
| **3** | **Worst-case-sized word copies** — 9,472 threads/chunk that do not shrink for empty terrain, by explicit design (`VoxelGpuWorldGen.cpp:3603-3607`, CITED: *"the excess threads read two dwords and exit"*) | `:3620`, `:3634` | 89 | **Real but small.** ~76 KB of traffic per chunk ≈ 10 µs for the *whole frame* at 80 chunks. The waste is thread count, not time. Do not chase this first. |
| **4** | **Whole chunk-table rewrite per publish** — `LockBuffer` over the entire origin+params table, ~860 KB each, ~1.7 MB/tick, **growing with pool occupancy** | `VoxelGpuPoolComponent.cpp:1573-1584` | n/a (per tick) | **Distinctive signature: grows through a leg rather than tracking per-frame arrivals.** Mostly render-thread CPU; GPU side is the upload. Only live if a pool proxy is (water pool included). |
| **5** | **Raster-atlas upsert** — `NumUpserts` groups × 128² texels, 131 KB/page upload | `VoxelRasterAtlasGpu.cpp:223,257,268`; `kPagePx=128` at `VoxelRasterAtlas.h:84` | would need hundreds of pages/frame | **Fails magnitude.** Game-thread fill budget is ~2.0 ms at ~2.83 ms/page (`VoxelRasterAtlas.cpp:64-73`) → **under one page most frames.** BUT it is structurally **zero when parked**, so it is the right size to explain parked 6.79 → moving-FAST 6.99, i.e. ~0.2 ms. Not 5.47. |
| **6** | **CSM shadow depth of the quad pool** — pool sets `bShadowRelevance` (`VoxelGpuPoolComponent.cpp:916`), `r.ShadowQuality=5`, `r.Shadow.CSM.MaxCascades=10`, `MaxCSMResolution=2048` (`Config/DefaultEngine.ini:212-214`) | above | ~0.03 ms for 80 arrivals | **Fails magnitude as a DELTA term** — 80 arrivals against a ~39–54k-chunk pool is +0.2% geometry. **But it is very likely the largest single named term in the 6.99 ms BASE** (memory: terrain shadows 15.8 ms in the quad era; the marcher casts none). Measure it for the floor, not the tail. |
| **7** | **GI brick uploads / voxelize** | `VoxelGI.cpp:205` (cap **64**/frame), `:118` (cap **16**/frame) | 85 µs/unit | **Bounded by construction** — cannot supply 5.47 ms unless per-unit cost is absurd. Cheap to null via the caps (which do *not* perturb streaming); `voxel.GI.Enabled 0` **does** perturb it (`VoxelGI.cpp:76-77`, CITED) — do not use that arm. |
| **8** | **Chunk-index full upload** — 64 MiB, not the 56 MiB every comment claims (`kCells=16,777,216`, `VoxelMarchChunkIndex.h:263`) | `VoxelMarchChunkIndex.cpp:2472-2474` | one event = ~10 ms | **Fallback only** — routine flushes are delta (~9,500 cells). **But a single full upload would produce exactly a p99 spike.** Read `FullBecauseSeed/Lost/First/Pending/Large` (`:2074,2082,2086,2095,2147,2182`) before ruling it out. Zero cost to check — the counters already exist. |

### 2.1 RETIRED: the per-chunk pool claim loop (row 1). Zero cost, from disk.

**Checked on the owner's instruction -- "check if BrickFlushBatch actually engages
before flipping it" -- and the check killed the candidate before a leg was spent.**

**Both halves of row 1 were wrong.**

**(a) The loop is NOT gated by `voxel.GPU.BrickFlushBatch`.** That cvar is read at
exactly ONE site (`VoxelBrickPool.cpp:4248`) and consumed at two: the
`AddFlushPasses_RenderThread` arm select (`:4289`, branch `:3293`) and a log line
(`:4400`). `bBatchedFlush` does not appear anywhere between `:4290` and `:4399`,
which is where the claim loop lives. **Flipping the cvar cannot touch it.**

The two loops are fed by DISJOINT sets, decided at `AddChunkFromCpu`
(`:2644-2665`): armed (`voxel.GPU.PoolAlloc`, default 1 at `:977`) ->
`PendingGpuCpuWrites` -> the claim loop; unarmed -> `PendingWrites` -> the
flush-batch loop. The PoolAlloc help states the consequence itself at `:987-989`:
*"PendingWrites is structurally EMPTY and everything downstream of it ... is
off-path."* **So `BrickFlushBatch` is dead, AND it is not this loop's gate.**
Flipping it to 1 changes no bytes; its own comment at `:743-747` says so.

**(b) The 5-dispatches-per-chunk pattern was already flattened, two days ago.**
`-VoxelGpuWorklistClaim` defaults **1** since 2026-08-25
(`VoxelGpuMeshJobManager.cpp:823`); its comment at `:812-818` records the claim +
both word copies + desc/record collapsing to **three indirect dispatches per tick**,
"**5 -> 0**" brick passes per claim-fed chunk. Tonight's own logs confirm it ran:

    [gpu-worklist] wlclaim conv=483560 hostStaged=483560 gpuClaimed=483560 dupRefused=0
    [gpu-batch]    0 stacks / 0 chunks; stackSuppressed 16913
    [brick-gpualloc] shells 526081; claims 526081; doubleGrant 0 badFree 0

483,560 of 526,081 chunks took the flat path. The residual (42,521) matches
`cpuLaunched` (42,174) to ~1%, so the CPU claim loop's whole share is ~8% of chunks
= **~2-3 chunks and ~12 dispatches per frame**, not the ~80 chunks / ~400 dispatches
row 1 modelled.

**THE NUMBER THAT SETTLES IT ALREADY EXISTED**, `Saved/RF-attrib.log` (2026-08-25,
shipping config, settled moving, 12,099 frames):

    seg=SETTLED-MOVING-LEG TAIL tailMs=1.60 | brickPoolMs=0.094(h=1.2)
    seg=SETTLED-PARKED-LEG                    brickPoolMs=0.001(h=0.0)

The whole flush render command -- `AddFlushPasses_RenderThread` AND the claim loop
-- sits inside `VOXEL_RENDER_FRAME_SCOPE_TAIL(TailBrickPool)` (`:4258`). **0.094
ms/frame is an upper bound on everything this row argued about: 5.9% of the tail.**

Caveat, stated rather than hidden: that is render-thread CPU, a MEAN over 12,099
frames, and it does not carry GPU execution of those dispatches or give a p99. It
is still decisive, because ~12 dispatches/frame cannot hide 8 ms.

**`[brick-flushbatch]` appears in exactly ONE log in all of `Saved/`** --
`flushbatch-on.log` (2026-08-22, before the 2026-08-24 PoolAlloc flip), where it
printed real work. It self-silences when all counters are zero (`:914-919`), so
absence means that loop did nothing -- **and that one log proves the line CAN
print, which makes its absence a test that can fail rather than a missing feature.**

**This would have been the FOURTH fix aimed at that dead path.** Do not aim a fifth.
Before proposing work anywhere downstream of `PendingWrites`, grep the logs for that
path's own line. A cvar default is not proof a function runs.

**Do NOT use `[brick-gpualloc] shells` to separate the two loops** --
`AllocateGpuChunkShell` is called from both `VoxelBrickPool.cpp:2647` (CPU) and
`VoxelGpuMeshJobManager.cpp:3832` (GPU), so it genuinely cannot distinguish them.
The isolating counter is `ChunksAddedFromCpu` (`++` at `:2651`, armed branch only,
printed by `voxel.Brick.Stats`); `cpuLaunched=` is the live proxy.

**WHERE THE TAIL ACTUALLY ISN'T, AND WHERE IT MIGHT BE.** On that same leg
`tailOtherMs=1.40` is **87% of the tail and unattributed** -- no bucket names it.
That is the only place in this instrument with room for a real cost.

Also found: `VoxelGpuMeshJobManager.cpp:469` banners `-VoxelGpuStackClaim, default
OFF` against an actual default of **1** at `:521`. A ninth lying label.

| — | **VoxelResidencyGpu** | `VoxelResidencyGpu.cpp:468-470` | — | **RULED OUT unless the leg passed `-VoxelGpuResidency=1|2`.** Confirm from the command line, then stop looking here. |

---

## 3. The measurement that settles it

The whole-frame clock cannot attribute within a frame. Four routes were evaluated
against two requirements: **cheap to arm**, and **able to catch a p99 frame rather
than an average**.

### The mechanism that makes one route win

`D:\UE_5.8\Engine\Source\Runtime\RHI\Private\GPUProfiler.cpp:1065-1067` (VERIFIED):

    uint64 TotalCycles = Exclusive.BusyCycles + Exclusive.WaitCycles;
    CsvProfiler->RecordEndOfPipeCustomStat(GPUStat.CsvStat->Name, GPUStat.CsvStat->CategoryIndex,
                                           FPlatformTime::ToMilliseconds64(TotalCycles), ECsvCustomStatOp::Set);

**Every `DECLARE_GPU_STAT_NAMED` stat's per-frame exclusive GPU milliseconds is
written into the CSV profiler, once per frame, end-of-pipe, into a category named
`GPU` (`:1059-1062`).** That is a per-frame time series of named GPU terms — from
which any percentile can be computed offline, and against which any other per-frame
column can be correlated.

Column name is `GPU/<StatName>` (`CsvProfiler.cpp:2456`), and the file carries **one
row per engine frame**, keyed by frame number (`CsvProfiler.cpp:2383`, `:3023-3034`,
`:3055-3132`). VERIFIED.

Enabling conditions, all VERIFIED:
- `HAS_GPU_STATS = ((STATS || CSV_PROFILER_STATS) && !UE_BUILD_SHIPPING)` — `RHIDefinitions.h:74`. On in Development.
- `CSV_PROFILER_STATS = CSV_PROFILER && !CSV_PROFILER_MINIMAL`, `CSV_PROFILER_MINIMAL 0` — `CsvProfilerConfig.h:25,35`. On.
- `bEmitToEngineStats = true` by default — `GPUProfiler.h:519`.
- Emission is gated on `Queue.Type == Graphics && Queue.Index == 0` (`GPUProfiler.cpp:1045`). **This project has no async compute, so every voxel dispatch qualifies.**
- Console command `CsvProfile START | STOP | FRAMES=N` — `CsvProfiler.cpp:1118`, handler `HandleCSVProfileCommand`.
- **Free of charge, no project code and not even the GPU-stats gate:** `DrawCalls` and `PrimitivesDrawn` are recorded per frame end-of-pipe at `GPUProfiler.cpp:2107-2108` through the **raw** `CsvProfiler` pointer — the `bCsvStatsEnabled` gate (`:2018`) is applied only where the pointer is handed to `EmitResults` (`:2051`, `:2064`). So a plain `CsvProfile` capture with no other flags already yields per-frame **`RHI/DrawCalls`** and **`RHI/PrimitivesDrawn`** columns. VERIFIED. These are the falsifier for candidate (c) in §6, at zero cost.

> **THE GATE THAT WOULD HAVE MADE THIS A SILENT NO-OP.** `r.GPUCsvStatsEnabled`
> **defaults to 0** (`GPUProfiler.cpp:118-121`, VERIFIED). It is consumed at `:2018`
> and the profiler pointer is passed as `bCsvStatsEnabled ? CsvProfiler : nullptr`
> at `:2051`/`:2064` — so with it off, `EmitResults` receives `nullptr` and **the
> `GPU/` columns are simply absent**, with no error. The command-line shortcut is
> **`-csvGpuStats`** (`CsvProfiler.cpp:5098-5105`, sets the cvar to 1).
> **Proof of engagement for this arm: the CSV must contain at least one `GPU/`
> column. If it does not, the leg measured nothing and must not be interpreted.**

### The routes, ranked

| route | cost to arm | catches a p99 frame? | verdict |
|---|---|---|---|
| **`RDG_EVENT_SCOPE_STAT` + `DECLARE_GPU_STAT_NAMED`, read out via `-csvGpuStats` + `CsvProfile`** | **2 lines per file.** Five working precedents in-tree: `VoxelMarchRenderer.cpp:47,48,7285,8715`; `VoxelShadowMarch.cpp:43,751`; `VoxelFluidSim.cpp:31,569`; `VoxelFluidRender.cpp:40,547` | **YES — full per-frame series, offline percentiles, offline correlation against chunks/frame** | **DO THIS.** |
| `FGPUStat::OnTimingResults` override — same scopes, results delivered straight to C++ | ~40–60 lines: `DECLARE_GPU_STAT_NAME_TYPE` (`RealtimeGPUProfiler.h:109`) + a hand-rolled subclass of `TGPUStat<>` + a ring buffer + thread marshalling | **Yes** — `GPUProfiler.h:535`, called at `GPUProfiler.cpp:1071` **once per frame per queue, raw inclusive Busy/Idle/Wait ms, unsmoothed, and NOT gated on `bEmitToEngineStats` or `r.GPUCsvStatsEnabled`** | **The strongest fallback.** Two frictions: it fires on the **RHI submission thread** (`D3D12Submission.cpp:1318`), not game or render; and **no frame number is passed** (`FFrameState` carries `StatsFrame` only under `#if STATS` and does not forward it), so you must count invocations or correlate via an always-present stat. |
| `stat GPU` | 0 (same scopes) | **No — structurally.** The `Avg` column is a **60-frame rolling mean**: history window `StatsCommand.cpp:192` (`maxhistoryframes=60`), merged `:1431-1437`, divided `:1468`, rendered as Avg/Max/Min by `StatsRender2.cpp:817-831`. A 60-frame mean cannot show a p99. Its `Max` column is a rolling max over the same window, on-screen only. Nothing in `tools/` captures or parses it (`grep` for `stat gpu` across all `.ps1/.sh/.py` = **0 matches**, VERIFIED) | Same scopes, structurally worse readout. Take the CSV. |
| `ProfileGPU` on a hitching frame | 0 — already scripted, `tools/voxel-run-gpu-arm.ps1:218-220` composes `voxel.DeferExec $CaptureAt ProfileGPU`, and `tools/march-direction-summary.sh:178` already parses pass rows out of the log | **No, and it cannot be made to.** `GPUProfiler.cpp:2197-2206`: the command bumps a bare refcount consumed by `ShouldProfileNextFrame()` (`:2217-2228`) — **next frame only, no frame targeting, and it ignores its own `Args`.** There is **no hitch-triggered auto-profile anywhere in the engine** (VERIFIED by count: `ProfileGPU.*Hitch`, `Hitch.*ProfileGPU`, `AutoProfileGPU` = **0 matches** across `Runtime/`, `Developer/`, `Programs/`, `Config/`). `voxel-run-gpu-arm.ps1:4-9` says the same thing in its own words (CITED): *"says nothing about the distribution, and its own frame may be atypical"* | Useful for a **pass inventory** — which is worth having once, to confirm the §1 dispatch map against reality. Cannot produce a tail. |
| Per-dispatch `RQT_AbsoluteTime` timestamps → new `FFrameSample` fields | ~40 lines per site plus readback plumbing plus new percentile arrays. Four working precedents (`VoxelMarchRenderer.cpp:6678-6703`, `VoxelShadowMarch.cpp:446-469`, `VoxelFluidSim.cpp:725-996`, `VoxelFluidRender.cpp:567-733`) | Yes | **The fallback, not the first move.** Real trap already paid for: `VoxelFluidSim.cpp:694-701` (CITED) — naive begin/end query passes *"tripped RDG's breadcrumb sentinel assert"*; they must be untracked `NeverCull` passes. |

### Where the scopes are missing

**Not one of the seven streaming files has a GPU stat** (VERIFIED counts):

| file | GPU stat | event scope | bare pass names |
|---|---|---|---|
| `VoxelGpuWorldGen.cpp` | 0 | 1 (`:2484`, covers BrickPack only) | 52 |
| `VoxelGpuPoolComponent.cpp` | 0 | **0** | 0 |
| `VoxelBrickPool.cpp` | 0 | 4 (`:2904,2990,3201,4304`) | 0 |
| `VoxelMarchChunkIndex.cpp` | 0 | 0 | 4 |
| `VoxelGpuWorklist.cpp` | 0 | 0 | 2 |
| `VoxelResidencyGpu.cpp` | 0 | 0 | 6 |
| `VoxelRasterAtlasGpu.cpp` | 0 | 0 | 3 |

### The trap: three spellings that compile and measure nothing

In 5.8 the legacy GPU profiler was **removed**, not deprecated —
`Runtime\RenderCore\Private\ProfilingDebugging\` does not exist; only the macro
header survives. `RHIDefinitions.h:68-71` now `#define`s `RHI_NEW_GPU_PROFILER` to
a `UE_DEPRECATED_MACRO(5.8, "The legacy GPU profiler has been removed...") 1`. All
VERIFIED.

| spelling | file:line | status |
|---|---|---|
| `SCOPED_GPU_STAT` | `RealtimeGPUProfiler.h:154` | **DEAD — `UE_DEPRECATED_MACRO`, expands to nothing** |
| `RDG_GPU_STAT_SCOPE` | `RenderGraphEvent.h:520` | **DEAD — expands to nothing** |
| `RDG_RHI_GPU_STAT_SCOPE` | `RenderGraphEvent.h:535` | **DEAD** |
| `DECLARE_GPU_STAT` / `_NAMED` / `DEFINE_GPU_STAT` | `RealtimeGPUProfiler.h:123,128,133` | **LIVE, still required** |
| `RDG_EVENT_SCOPE_STAT` | `RenderGraphEvent.h:480` | **LIVE — use this** |
| `RHI_BREADCRUMB_EVENT_STAT` | `RHIBreadcrumbs.h:1302` | **LIVE** |

The project already recorded half of this at `VoxelFluidSim.cpp:24-30` (CITED).
**Anyone reaching for the familiar `SCOPED_GPU_STAT` will arm a scope that reports
nothing and looks armed — this project's signature failure.** `RDG_EVENT_SCOPE_STAT`
feeds ProfileGPU *and* `stat GPU` *and* the CSV column from one scope.

### The minimum armed set (5 scopes, one per suspect)

    VoxelStreamPoolClaim   around VoxelBrickPool.cpp:4322-4365   <- candidate 1
    VoxelStreamWorklist    around the per-tick flush graph        <- candidate 2
    VoxelStreamPoolUpload  around VoxelGpuPoolComponent.cpp:1559+ <- candidate 4
    VoxelStreamAtlas       around VoxelRasterAtlasGpu.cpp:220-270 <- candidate 5
    VoxelStreamChunkIndex  around VoxelMarchChunkIndex.cpp:2540+  <- candidate 8

Plus **one** `CSV_CUSTOM_STAT` for chunks-applied-this-frame, so the correlation is
computable inside the same CSV rather than inferred across two instruments.

**Keep these five scopes SIBLINGS, never nested.** The CSV value is
`Exclusive.BusyCycles + Exclusive.WaitCycles` (`GPUProfiler.cpp:1066`) while the
`stat GPU` display and the `OnTimingResults` callback report **inclusive** cycles
(`:1038-1040`, `:1071`). Sibling scopes make the two readings agree and make the
columns sum toward the frame total; nested ones make "which number am I reading"
a live question, which on this project is how a term gets quoted wrong.

### Note on the existing tail machine

`FFrameSample` (`VoxelWorldSubsystem.cpp:6706-6743`) is a retained per-frame array
(`kMaxFrameSamples = 40000`), sorted at report time and bucketed FAST/SLOW(≥p95)/
TAIL(≥p99) at `:12578-12626`. **Any float added to it gets bucketed means and maxes
for free** — that is the fallback route's payoff. Two properties to know before
relying on it: `FrameSamples` is **never cleared** (`grep -c "FrameSamples.Reset\|
FrameSamples.Empty"` = **0**, VERIFIED), so every window's percentiles are
cumulative-to-date, not per-window; and **individual samples are never printed**, so
no offline tool can compute a percentile of a new term from the log. The CSV route
does not have either limitation.

---

## 4. The `GPUCullDebugDrawNothing` bound — yes, and it is better than a single bound

**It is stream-neutral, un-guarded, and it separates camera raster from shadow
raster.** Declaration `VoxelGpuPoolComponent.cpp:451-458`; the *one and only* read
site is `:1185` (`grep -c "GDebugDrawNothing"` = **3 total occurrences**: decl, cvar
binding, read — VERIFIED, counted before concluding).

The branch it actually gates (VERIFIED, `:1184-1191`, `:1238-1242`):

    const bool bA3Shadow  = Views[ViewIndex]->GetDynamicMeshElementsShadowCullFrustum() != nullptr;
    const bool bA3Suppress = (A3Mask & (bA3Shadow ? 2 : 1)) != 0;
    if (bA3Suppress && (A3Mask & 4) != 0) { RecordGather(...0,0); continue; }
    ...
    if (bA3Suppress)                      { RecordGather(...0, VisibleQuadsThisGather); continue; }

- **bit 1 = camera gathers, bit 2 = shadow gathers.** So the mask is a *decomposition*, not one switch. Nobody has used it that way.
- **bit 4 is not independent** — `:1191` requires `bA3Suppress` first, so mask `4` alone is a complete no-op. (VERIFIED; this is a trap worth knowing before someone runs a `4` arm and reports "no effect".)
- **There is no GPU compute cull.** The cull is render-thread CPU (`BuildCulledRanges`, `:1209`). At mask 3 the walk and the uniform-buffer emit still run; only `Collector.AddMesh` is removed. So **mask 3 = keep 100% of the CPU cull, remove 100% of the pool's GPU raster.**
- **It does NOT touch pool writes or worldgen.** Those live in `UpdateQuadRange_RenderThread` (`:1459-1509`) and `UpdateChunkTable_RenderThread` (`:1512-1585`), neither of which reads the mask; GPU worldgen never references it. **So mask 7 does not bound the streaming-GPU term — it bounds the DRAW term, which is exactly what makes it a clean subtraction.** (VERIFIED. This answers the brief's question directly: *no*, it does not isolate streaming-side GPU work; it isolates the draw path, which lets you subtract the draw path *out* of the residual.)
- **No editor/packaged guard.** `grep -c "^#if" VoxelGpuPoolComponent.cpp` = **0**; flags are `ECVF_RenderThreadSafe`, not `ECVF_Cheat`. Settable in `-game`.
- **Stream-neutral.** The only side effect on the suppressed path is `RecordGather` (`:2433-2500`), which I read end-to-end: pure atomics and a `UE_LOG`. Nothing feeds admission, desired-set or dispatch. **chunks/frame is unaffected, so the A/B is valid** — VERIFIED, and this is the property that makes it worth running first.

**Caveat, INFERRED:** mask 3 removes the pool from the depth prepass and shadow-depth
passes, so anything reading scene depth (VSM invalidation, TSR, SSAO) changes too, and
`control − mask3` slightly over-attributes to the pool. Mitigated here because the
marcher writes SceneDepth independently (`voxel.March` default **1**, and it explicitly
does *not* suppress the quad path — `VoxelMarchRenderer.cpp:75-76`, CITED).

### Free companion reading, zero code

`voxel.Stream.GPUCullStatsPeriod` defaults **0 = census off** (`VoxelGpuPoolComponent.cpp:569`)
— which is why no log in `Saved/` contains a census line (VERIFIED: 0 matches across
the six most recent). Set it to 60 and the existing census prints **camera vs shadow
submitted quads per gather**, windowed, with an explicit *"shadowGathers=0 MEASURED
(not absent)"* line for the zero case (`:2494-2500`). That sizes candidate 6 without
touching a single line of code.

---

## 5. Run order — cheapest decisive first

Every arm below is **zero code** except step 4.

1. **Read the counters that already exist**, from a leg you have. No new run needed:
   `LastFlush.Take` (`VoxelGpuWorklist.cpp:627`) vs `ChunksAddedFromCpu` /
   `CpuUploadBytes` (`VoxelBrickPool.cpp:2650`, `:4235`) — this says whether arriving
   chunks go through the **per-tick worklist** (candidate 2) or the **per-chunk claim
   loop** (candidate 1), and those have different fixes. Also read
   `FullBecause{Seed,Lost,First,Pending,Large}` to kill or promote candidate 8, and
   `[gpu-lean] mesh-region graphs skipped=` (`VoxelGpuMeshJobManager.cpp:975`) to
   confirm lean is actually on — it decides whether each chunk pays one region graph
   or two, a 2–3× swing in the §1 cost model.
2. **`voxel.Stream.GPUCullStatsPeriod 60`** on the next moving leg — sizes the shadow
   term (candidate 6) for free.
3. **The decomposition sweep**, four matched moving legs, stream-neutral:
   `GPUCullDebugDrawNothing` = **0** (control) / **1** (camera raster gone) /
   **2** (shadow raster gone) / **3** (both gone, CPU cull kept).
   `(0−1)` is the camera-pass GPU cost, `(0−2)` the shadow-depth GPU cost, `(0−3)` the
   whole draw path. **If the +5.47 ms survives arm 3, the draw path is not the tail
   and candidates 1/2 own it.** Remember `-Cvars` separator is **COMMA**.
4. **Then, and only then, arm the five GPU stats + one CSV chunk counter** (§3) and
   run one moving leg with **`-csvGpuStats`** on the command line plus
   `voxel.DeferExec <sec> CsvProfile FRAMES=1800` in `-ExecCmds`. That is the
   measurement that names the term. Everything before it is subtraction; this one is
   attribution.

   **Pre-registered arm check, because this arm has two silent-no-op modes:** the CSV
   must contain a `GPU/` column (else `r.GPUCsvStatsEnabled` never took), and the log
   must contain `voxel.DeferExec: running now` (`VoxelDebug.cpp:945`) with a timestamp
   **inside** the flight window — `tools/march-direction-summary.sh:191-213` already
   does exactly this lexicographic check against the pose-pin and `VoxelPerfRun
   complete` lines, and it is the pattern to copy. A leg failing either check is void,
   not "a null result".

   **And a red arm before the green one:** the marcher already has a working
   `RDG_EVENT_SCOPE_STAT` (`VoxelMarchRenderer.cpp:7285`). Its `GPU/VoxelMarch` column
   must appear in the same CSV and must land near the known ~3.169 ms parked marcher
   cost. If a scope known to be live reads zero or absent, the readout is broken and
   no streaming column may be interpreted.

---

## 6. Falsifiers

**Retired before any leg runs:**

- **(a) Shader/PSO compilation stalls.** A compile stall starves the GPU; starvation is
  idle; idle is excluded from the union (§0). **A CPU stall cannot raise `gpu=`.**
  Independently, terrain-material PSOs are precached at BeginPlay
  (`VoxelWorldSubsystem.cpp:27805`, switchable off with `-VoxelNoPSOPrecache`).
  *The reading that would have distinguished it:* `rhiMs` (the clean CPU clock, per the
  house rule) and `renderMs` rising while `gpu` stayed flat. `gpu` rose. Dead.
- **(d) GPU backpressure / the GPU merely waiting.** Same argument. Dead.

**Still live, and each has a distinguishing reading:**

- **(b) VRAM pressure / eviction as the pool grows.** A GPU stalled on a page fault or
  reading over PCIe is **busy**, not idle — it *does* inflate the union. **Distinguishing
  reading: correlate `gpu` against CUMULATIVE pool residency versus INSTANTANEOUS
  chunks/frame.** Memory pressure tracks the cumulative curve and rises monotonically
  through a leg; dispatch cost tracks the per-frame spike and is flat in leg-time.
  The CSV route gives both columns in one file. **This test can come out either way,
  which is the point.**
- **(c) More geometry to raster.** **Free reading, no project code and not even the
  GPU-stats flag:** `RHI/DrawCalls` and `RHI/PrimitivesDrawn` are written per frame by
  any CSV capture (`GPUProfiler.cpp:2107-2108`, outside the `bCsvStatsEnabled` gate).
  If p99 frames show `PrimitivesDrawn` ≈ FAST frames, the raster hypothesis is dead;
  if `DrawCalls` spikes instead, it is the cull emitting more ranges, which is a
  different fix in a different file (`VoxelGpuPoolComponent.cpp:1332-1391`).

**And the confound nobody has raised, which could invalidate the whole correlation:**

- **Reverse causation: a longer frame HARVESTS more chunks, purely because it is longer.**
  `voxel.GPU.MeshHarvestCap` resolves to **0 = unlimited** under primary
  (`VoxelGpuMeshJobManager.cpp:427`, VERIFIED), so one poll takes every job that has
  become ready. A 22 ms frame gives async GPU jobs ~2.4× the wall time a 9 ms frame
  does before the next harvest. **chunks/frame would therefore correlate with frame
  time even with zero causal contribution to it.** `-VoxelTickBudgetMs` defaults to 0
  (`VoxelTickBudget.h:195-208`), so that route is closed, but the harvest route is open.
  *The reading that distinguishes:* whether the chunk spike **leads or lags** the GPU
  spike in the per-frame time series. Leading = chunks cause the cost. Lagging = the
  cost caused the chunks. **The current aggregate report cannot answer this** — it
  prints bucket means, never the series (`:12572-12574` notes samples are kept in
  capture order and nothing prints them). **The CSV route answers it directly**, which
  is a second, independent reason to prefer it.

---

## 7. Things found on the way that are bugs, not findings

- **`-VoxelGpuLeanBrickJobs` header says "Default OFF"** (`VoxelGpuMeshJobManager.cpp:453`)
  while the accessor returns `VoxelGpuPrimaryEnabled()` (`:466`). Under the default
  `-VoxelGpuPrimary` it is **ON**. Sixth lying name.
- **`voxel.March`'s help says mode 0 "IS THE DEFAULT"** (`VoxelMarchRenderer.cpp:66-70`);
  the declaration on `:63` is **1**. Seventh.
- **`voxel.March.IndexDeltaUpload` comment says "default 0"** (`VoxelMarchChunkIndex.cpp:2463`);
  declaration `:77` is **1**. Eighth.
- **The chunk index is 64 MiB, not the 56 MiB every comment in the file claims**
  (`:80`, `:164`, `:2501`) — `kCells = 16,777,216` at `VoxelMarchChunkIndex.h:263`.
  The comments date from `kGridSlots = 7`; it is now 8 (`:262`).
- **`VoxelBrickPool.h:258` says "a 32 B record always"**; `kChunkRecordBytes = 16*4 = **64** `
  (`VoxelBrickPool.cpp:46-47`). Same claim repeated at `VoxelBrickPool.cpp:3669`.
- **`VoxelResidencyGpu.cpp:887`: a `FIntVector(1,1,1)` dispatch with a per-op serial
  loop inside the kernel.** Its *shape* says constant; its *cost* scales with feedback
  records. A name/shape lie waiting to happen. Module is off by default, so it is
  harmless today.
- **`GDebugDrawNothing` mask value `4` alone is a silent no-op** (`:1191` requires
  `bA3Suppress` first). Anyone running a `4` arm will report "no effect" correctly and
  conclude the wrong thing.
