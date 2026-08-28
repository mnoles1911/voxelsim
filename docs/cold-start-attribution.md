# Cold start, attributed end to end (Wave 3)

Read on `Saved/TJDL-A-control.log` (2026-08-26, 13:03 UTC), the leg the Wave 3 brief was written
from. **Nothing here was measured by me** — I did not build, launch a leg, or open the editor.
Every number is either read out of that log or read out of the code that writes it. Claims are
labelled **VERIFIED** (I read the writing site, or did the arithmetic on the log), **CITED** (the
code records its own prior measurement), or **INFERRED**.

---

## THE HEADLINE

**The player's cold start is gated by a hardcoded 15.0 second minimum hold that no leg has ever
measured, and the 6.1 s settle everyone has been optimising is not the thing the `<5 s` target is
about.**

Five findings, in the order they change what to do:

1. **`LoadMinHoldSeconds = 15.0f`** (`VoxelFrontEndSwitches.h:65`). The loading curtain refuses to
   lift before 15 s regardless of whether the world is ready, then fades for 0.4 s more. **Even a
   0 s settle gives the player ≥ 15.4 s.** The `<5 s` target is missed by 3x by the curtain alone.
2. **Every settle measurement this project has ever taken was on a path with the curtain
   suppressed.** TJDL-A runs `-unattended`, and the log says
   `VoxelFrontEnd: suppressed (unattended run (-unattended))`. The 15 s hold has never appeared in
   a leg.
3. **`settleT=6.1 s` is not cold start.** It arms at "the subsystem first wanted a chunk" and
   deliberately excludes level bring-up and shader compilation. Process launch to a settled world
   on this leg is **23.6 s**; the streaming settle is **26%** of it.
4. **The atlas is not the largest game-thread term inside the settle.** `reqHdr` — four amplifier
   columns plus a climate sample per chunk submission — is **1,325.6 ms** against the atlas's
   1,463.9 ms, and it has not been named in a Wave 3 document.
5. **The 1.29-pages-per-call lead is refuted.** The cost is **per PAGE** (1.383 ms, 98% of it
   16,384 elevation samples), not per call. Batching 450 calls into one still fills 579 pages.

---

## 1. THE TWO CLOCKS, AND WHY THE TARGET IS WRITTEN AGAINST THE WRONG ONE

There are two independent notions of cold start in this tree and they measure disjoint intervals.

| | arms at | ends at | on this leg |
|---|---|---|---|
| **Streaming settle** (`LogVoxelPerf`) | first streaming tick with demand — `VoxelWorldSubsystem.cpp:10663-10682` | last streaming activity (+3.0 s hold, subtracted back out) | **6.1 s** |
| **Loading curtain** (`LogVoxelUI`) | NEW GAME → `BeginLoad`, +2 painted frames — `VoxelFrontEndSubsystem.cpp:499` | `LoadElapsed >= LoadMinHoldSeconds && probe ready`, or 60 s timeout | **never ran** |

### 1a. The settle clock excludes bring-up by design

**VERIFIED**, `VoxelWorldSubsystem.cpp:10660-10662`, the comment states the exclusion out loud:

> `// Arm at the first tick with streaming demand -- t=0 is "the subsystem first wanted a chunk",`
> `// not process start, so the number measures the streaming pipeline and not shader compilation`
> `// or level bring-up.`

`SettleT = LastActivity - ColdStartSeconds` (`:10718`), reported after a 3.0 s quiet hold that is
subtracted back out. `VoxelFramePhase::NoteSettled(SettleT)` at `:10731` stamps the
`BOUNDARY settleT=` line (`VoxelFramePhase.cpp:616`).

So `settleT` answers "how long did the streamer take once it started". It does not answer "how long
until the player sees a world", and `<5 s cold start` is written in the second language.

### 1b. The curtain's release condition

**VERIFIED**, `VoxelFrontEndSubsystem.cpp:623`:

```cpp
if ((LoadElapsedSeconds >= Switches.LoadMinHoldSeconds && bReady) || bTimedOut)
```

with `LoadMinHoldSeconds = 15.0f` (`VoxelFrontEndSwitches.h:65`, `-VoxelLoadMinHold=`) and a 0.4 s
`FadeDuration` (`VoxelUITheme.h:208`) applied at `VoxelFrontEndSubsystem.cpp:652` before
`State = Playing` at `:680`. The comment at `:616-620` gives the intent — "so a warm cache does not
flash the loading screen for a third of a second" — and that reasoning was sound when the world
took 21 s. **Against a 6.1 s world it is ~9 s of pure wait.**

`bReady` needs 3 consecutive good polls at 0.4 s intervals = **1.2 s of sustained quiet**
(`VoxelWorldReadyProbe.h:63-66`), with all 112 spatial probes hitting and rings R0..R3 idle
(`VoxelWorldReadyProbe.cpp:140-152`). **INFERRED:** on a 6.1 s settle `bReady` goes true around
t ≈ 7–8 s and then waits ~7 s for the clock.

**This is the single cheapest and largest change available in Wave 3, and it is a flag.**

---

## 2. THE REAL TIMELINE (VERIFIED, timestamps from the log)

| from | to | wall | what |
|---|---|---|---|
| 13:03:51.2 | 13:04:06.726 | **15.52 s** | Engine init. The engine prints its own total: `LogLoad: (Engine Initialization) Total time: 15.52 seconds` |
| …13:03:54.112 | 13:03:57.868 | 3.76 s | …of which: Zen DDC service launch (`LogZenServiceInstance`) |
| …13:04:02.590 | 13:04:06.421 | 3.83 s | …of which: `LogLoad: Took 3.831396 seconds to LoadMap(/Engine/Maps/Entry)` |
| ……13:04:03.169 | 13:04:05.803 | **2.63 s** | ……of which: four fine tiles loaded on frame `[0]` |
| 13:04:06.726 | 13:04:08.744 | 2.02 s | First frame + voxel subsystem arm (sky/fog/MPC 08.360; atlas init 08.705; brick pool 952.0 MiB 08.725) |
| 13:04:08.744 | 13:04:14.84 | **6.10 s** | **`settleT`** — 50,052 chunks dispatched, 8,161/s mean |
| 13:04:14.84 | 13:04:17.874 | 3.00 s | Quiet hold, explicitly excluded from `settleT` |
| **total** | | **23.6 s** | process start → settled world |

Three structural notes on the 17.5 s pre-roll:

- **The voxel subsystem is initialised TWICE.** The full `Fine tier ENABLED` / `VoxelSky RESOLVED`
  / `BathyField armed` block prints at **13:04:02.238** and again at **13:04:03.152**, either side
  of `LogLoad: LoadMap: /Engine/Maps/Entry` and `BeginTearingDown for /Temp/Untitled_0`. The
  transient `Untitled` world is created, the subsystem arms on it, the world is torn down, and it
  arms again on `Entry`. Cost of the duplicate: **~0.91 s**. VERIFIED from the log; whether it is
  avoidable I cannot settle read-only.
- **The four fine tiles are 2.63 s of game thread on frame `[0]`, before any tick.** Per-tile
  lines: 409.4 / 385.8 / 319.9 / 306.3 MB read (of 496.2 / 473.0 / 408.0 / 394.5 MB files, in 10
  ranges each) + 134.2 MB decoded lattice each, "full decode" 300 / 281 / 275 / 266 ms. So
  **1,421.4 MB read** (≈1.5 s at ~950 MB/s) plus **1,122 ms of decode**. Loads are synchronous,
  exclusive-locked, whole-tile-decoded — `VoxelFineTileStreamer.cpp:601-628`, whose comment says a
  partially decoded tile "would let a worker query trigger a decode, which is the one thing the
  shared read lock cannot survive." This is the largest *project-owned* term in the pre-roll.
- **~950 MiB of GPU arenas are zero-filled on the render thread, untimed.**
  `FVoxelBrickPool::EnsureCreated_RenderThread` (`VoxelBrickPool.cpp:2100-2160`) makes four
  `CreateArenaBuffer` calls with `SetInitActionZeroData()` (`:951`). The pool announces
  `393216 chunks … — 952.0 MiB committed` at 13:04:08.725. Separately the GPU quad pool commits
  1,465 MB + 732 MB (`VoxelWorldSubsystem.cpp:22743`, `VoxelGpuPoolComponent.cpp:799-804`).
  **~2.5 GB of VRAM is committed inside the first streaming frames with no timer around any of
  it**, and that is a live candidate for the unattributed residual in §3d.

---

## 3. THE 6.1 s SETTLE, ATTRIBUTED

### 3a. The game-thread bill

Three `Voxel gpu submit split (window)` rows and three `[raster-atlas] window` rows span the settle
(±0.5 s at the edges). **VERIFIED** by summing the log fields.

| term | ms | % of the 6,100 ms wall | writing site |
|---|---|---|---|
| **`reqHdr`** (footprint + seed + **`ShadingForDispatch`** + skirt mask) | **1,325.6** | 21.7% | `VoxelWorldSubsystem.cpp:19814`–`:19876`, accumulated `:20320` |
| **atlas `Tick` sweep** (`GtFillCycles`) | **778.3** | 12.8% | `VoxelRasterAtlas.cpp:1590` |
| **atlas demand path** (`DemandCycles`) | **682.6** | 11.2% | `VoxelRasterAtlas.cpp:2074` |
| submit `band` + `assets` + `pool` + `mgrSubmit` | 31.0 | 0.5% | `:20321`–`:20326` |
| recompute (worst single window, `entryMs R5=210.68`) | ≥352 | ≥5.8% | `Voxel recompute (max since last log)` |
| **identified subtotal** | **~3,170** | **~52%** | |

Per-window detail:

```
13:04:11.214  submit total  130.2 = reqHdr   85.7 + band 1.0 + raster  27.2 + assets 0.4 + pool 2.9 + mgrSubmit 13.1  (17,719 calls,   7.3 us/call)
13:04:13.312  submit total 1032.4 = reqHdr 1008.2 + band 0.3 + raster  12.6 + assets 0.3 + pool 1.4 + mgrSubmit  9.6  (13,453 calls,  76.7 us/call)
13:04:15.303  submit total  882.2 = reqHdr  231.7 + band 0.1 + raster 648.4 + assets 0.1 + pool 0.3 + mgrSubmit  1.6  ( 3,638 calls, 242.5 us/call)
--- steady state, for scale ---
13:05:41.478  submit total   14.6 = reqHdr    8.8 + band 0.6 + raster   1.0 + assets 0.2 + pool 0.6 + mgrSubmit  3.3  ( 7,790 calls,   1.9 us/call)
```

**The atlas is 24% of the settle wall (1,463.9 ms of 6,100), exactly as the brief says. `reqHdr` is
22%, and it is bigger than either half of the atlas taken alone.** Together they are 46% of the
wall and 93% of the identified game-thread voxel work.

**`raster=` and `demandMs` are the same money, not two costs.** `FillWindowOnDemand` is called from
inside the `SubT2`→`SubT3` bracket (`VoxelWorldSubsystem.cpp:20016-20019`), so `raster=` 688.2 ms
and `demandMs` 682.6 ms are the same milliseconds counted twice. `GtFillCycles` (the `Tick` sweep)
*is* genuinely disjoint — `VoxelRasterAtlas.h:558-568` says so and `:1590` / `:2074` are separate
brackets. **Do not add `raster=` to `demandMs`.**

### 3b. What `reqHdr` actually is

**VERIFIED**, `VoxelWorldSubsystem.cpp:19869`: the bracket's body is
`VoxelApplyFast::ShadingForDispatch(...)`, and the comment above it at `:19844-19851` names the
cost:

> `// This call IS that bucket's body: four GetSurfaceHeightUU amplifier`
> `// columns plus a climate sample, per submission, on the game thread.`

Per-call cost across the settle: **4.8 → 75.0 → 63.7 µs**, against **1.1 µs in steady state**
(13:05:41). A **~68x cold-vs-warm ratio**, and the mechanism is one line away in the same log:

```
13:04:11.214  Voxel shared grid cache (window): cap=1024 hits=4364 misses=4662 (hit%=48.3) evictions=2644
13:04:13.312  Voxel shared grid cache (window): cap=1024 hits=1531 misses=2873 (hit%=34.8) evictions=1851
13:04:15.303  Voxel shared grid cache (window): cap=1024 hits=1048 misses= 764 (hit%=57.8) evictions= 682
```

**5,177 evictions against a 1,024-entry cap in 6 seconds, at a 34.8–57.8% hit rate.** Capacity is a
latched command-line switch, `-VoxelSharedGridCache=<entries>`, default 1024
(`VoxelWorldSubsystem.cpp:3310-3311`).

**Instrument warning on that same line:** the arming line says `cap=1024 entries (~208 MB)`, the
window line says `entries=1024 (424 MB)`. Two sizes for the same thing, 2.04x apart. Read the byte
accounting before sizing anything off either label.

### 3c. What the other ~48% is: neither thread you can see

The remaining ~2.9 s is not idle and not on the game thread. It is the two chunk producers running
in parallel.

**CPU workers — VERIFIED.** `cpuJobSec` is documented at `VoxelWorldSubsystem.cpp:8404-8414` as
"Sum of `Result.JobMs` over this window's drained CPU results"; `effConc` is that divided by the
window (`:11586-11587`).

```
13:04:11.214  passes=117 exitCap=94 cpuLaunched=9109 gpuForked=17719 cap=288 cpuInFlightExactNow= 92 cpuJobSec= 9.4 effConc=4.67
13:04:13.312  passes= 93 exitCap=46 cpuLaunched=4601 gpuForked=13453 cap=288 cpuInFlightExactNow=288 cpuJobSec=13.5 effConc=6.47
13:04:15.303  passes=100 exitCap=10 cpuLaunched=1532 gpuForked= 3638 cap=288 cpuInFlightExactNow=  0 cpuJobSec=11.6 effConc=5.78
```

**34.5 CPU-seconds** over 15,242 CPU chunks (2.26 ms/chunk) at a mean **5.6 thread-equivalents**, on
a 6-core / 12-thread Ryzen 5 5600X. `34.5 / 5.6 = 6.16 s`. The settle is **6.10 s**.

**GPU fork:** 34,810 chunks, `gpuMesh submit->deliver ms p50=67.9 → 156.6`, `gpuLatency p50`
33–98 ms per level. At ~1,308 in flight and 156 ms latency the ceiling is ~8,385 chunks/s; observed
8,161/s.

**INFERRED:** both producers land within a few percent of 8,161 chunks/s. They are almost certainly
coupled through the game thread's dispatch loop (`exitCap` dominates: 94 of 117 passes, then 46 of
93) rather than independently arriving at the same number.

### 3d. Serial vs overlapped

- **Serial** (nothing else runs): engine init 15.52 s; the four fine tiles 2.63 s (frame `[0]`, no
  tick yet); the double subsystem arm ~0.91 s; the loading curtain's 15.0 s hold, which by
  construction overlaps everything and then keeps going.
- **Overlapped**: everything inside the 6.1 s settle. `reqHdr`, both atlas halves and recompute are
  all on the game thread and serialise *against each other*; the CPU workers and the GPU fork run
  beside them.
- **Unattributed:** ~48% of the settle wall sits on no voxel clock. §7 R1 is the only thing that
  answers it.

Consequence: **the atlas's 1,464 ms cannot buy 1,464 ms of settle.** The project has already
measured the conversion — §5.

---

## 4. WHY THE FILL IS SYNCHRONOUS ON THE GAME THREAD

**It is a real data dependency, and it has a name.**

**VERIFIED**, `VoxelWorldSubsystem.cpp:20016-20019`:

```cpp
if (!RasterAtlas.IsValid() ||
    (!RasterAtlas->PrepareRequest(Req) &&
     !(RasterAtlas->FillWindowOnDemand(ActiveTiles(), Req) && RasterAtlas->PrepareRequest(Req))))
{
    VoxelGpuRegionBuild::FillRasterWindow(Req, ActiveTiles());
}
```

The dependency, **CITED** from the comment at `VoxelWorldSubsystem.cpp:20072-20077` and **VERIFIED**
at the consuming site:

> `// AND IT COSTS A SECOND TIME ON THE GPU. The worklist claim chain refuses a`
> `// job whose region is not atlas-armed (VoxelGpuMeshJobManager.cpp's`
> `// 'if (!Job->BrickRegion.bRasterAtlas) { ++WorklistSkipNoAtlas; continue; }')`

Consuming site confirmed: `Source/VoxelEarthShaders/Private/VoxelGpuMeshJobManager.cpp:4000`.

The chain is: **a chunk cannot be dispatched to the GPU mesher without elevation for its window
already being on the GPU.** It can get there two ways — resident atlas pages (referenced for
~0.1 µs) or an inline ~46 KB per-chunk window sampled on the game thread — and both are produced
synchronously in `SubmitGpuMeshJob` before the job is handed over. The third option, "no
elevation", is not a state the worklist accepts; a declined chunk pays ~19 extra RDG passes on the
classic path instead.

**So this is a genuine dependency, not a writing style.** What is negotiable is *when* the data is
produced — earlier, or on another thread — never *whether*. That is what limits §5 and §6 to the
shapes they take.

---

## 5. THE 1.29-PAGES-PER-CALL LEAD IS REFUTED

The brief's strongest lead is that 450 calls averaging 1.29 pages is a batching problem. **It is
not.** The atlas's own per-page instrument settles it.

**VERIFIED by arithmetic** across all three settle windows — `perPage × fills` reproduces
`fillMs + demandMs` to within 1.7 ms:

| window | `fills=` | `perPage` | product | `fillMs` + `demandMs` |
|---|---|---|---|---|
| 13:04:10.753 | 202 | 1.094 ms | 221.0 ms | 197.2 + 24.1 = **221.3** |
| 13:04:12.754 | 166 | 2.364 ms | 392.4 ms | 383.4 + 9.5 = **392.9** |
| 13:04:14.757 | 690 | 1.225 ms | 845.3 ms | 197.7 + 649.0 = **846.7** |
| **total** | **1058** | | | **1,463.9 ms** |

`fills=` is **VERIFIED** to count both paths: `++PagesFilled` lives in `CommitStagedPage`
(`VoxelRasterAtlas.cpp:1018`), which every `FillPage` reaches — from the `Tick` sweep and from
`FillWindowOnDemand` alike. The 1,058 total also equals the reported `resident=1058/1681`.

So the whole settle atlas bill is **1,058 page fills at 1.383 ms each**, and per the `fill:` line
**98% of a page is `elev`**:

```
perPage 1.225 ms = elev 1.205 (98%) + climate 0.014 (1%) + stage 0.006 (0%) + resid 0.000 (0%)
callsPerPage elev=16384 climate=64 of 16384 px (dedup on, 256x)
```

**VERIFIED** at `VoxelRasterAtlas.cpp:883`: `Row[Lx] = Tiles.elevationMm(Px0 + Lx, Py)` — one
sampler call per pixel, 16,384 per page, and the comment at `:870-874` calls it "IRREDUCIBLE IN
CALL COUNT through this interface". Climate already has a 256x run-dedup (that is fill mode 2);
elevation has none.

**What batching would and would not buy.** Collapsing the 450 demand calls into one "collect the
whole window's missing set once" call removes 449 executions of `CollectMissingWindowPages` — a
walk over ~9 pages of the window grid, mirror lookups only. Microseconds. The 579 pages still get
filled, still at ~1.2 ms each. **Nothing stops the batch today; it just would not pay.**

**The lever that is left, and it is the biggest one in the atlas.** The 256x that mode 2 got on
climate has never been attempted on elevation. The atlas page pitch is 1,875 mm/px
(`[raster-atlas] init: pitch=1875 mm/px`) and the resident fine tile pitch is also 1,875 mm/px
(`Fine tile (-5,-5) resident: 8192 px edge`). **INFERRED, and this is the single highest-value
thing to check in code:** if the page's pixel lattice is coincident with the decoded fine-tile
lattice, a page fill over a resident fine tile is a strided copy, not 16,384 virtual sampler calls.
If it is coincident, that is the 98% term *deleted* rather than moved — no new thread, no async
hazard. If it is not (offset, filtered, or amplified), the lever is dead and should be recorded as
dead. **I could not settle this read-only** without tracing `FVoxelFineTileSamplerProxy::elevationMm`
to its lattice indexing; that trace is step 1 of any atlas work. See §7 R5.

---

## 6. WHY GAME-THREAD MILLISECONDS DO NOT BECOME SETTLE SECONDS

Two measurements already in-tree cut against optimising the atlas for settle time.

**CITED**, `VoxelRasterAtlas.cpp:586-591` — the fill-mode ladder, matched arms, one switch each:

```
mode  perPage    elev            climate         fill ms GT  settle
  0   1.093 ms   0.642 (59%)     0.445 (41%)       4,363 ms   21.4 s
  1   0.971      0.563           0.397             3,071      20.9
  2   0.587      0.572 (97%)     0.006 ( 1%)       1,877      20.9   <- shipping default
  3   0.902      0.894           0.008               330      20.3
```

**CITED**, `VoxelRasterAtlas.cpp:645-652`, the project's own summary of that table:

> `// AND THE HONEST NUMBER, WHICH CUTS AGAINST ALL OF THAT: removing`
> `// 4,033 ms of game thread bought 1.1 s of settle -- 27%, not 1:1.`

**At the measured 27% conversion, deleting the atlas's entire 1,464 ms buys ~0.40 s of the 6.1 s
settle — 6.5%.** That is the number to hold any atlas proposal against.

That ladder settled at ~21 s, not 6.1 s; it is a different binary and its absolute settle figures do
not transfer (this project's own rule). The *ratio* is what carries.

**The async refutation**, **CITED** verbatim, `VoxelRasterAtlas.cpp:151-157`:

> `// WHY NOT ASYNC (-VoxelGpuRasterAtlasFill=3) INSTEAD: measured, and it is`
> `// WORSE despite moving 85% of the fill off the game thread (489 -> 79 ms`
> `// per 2 s window). It SPREADS the stall rather than removing it. Total time`
> `// in hitch frames over one flight: baseline 4795 ms / 35 hitches, this fix`
> `// 3957 ms / 58, async 5834 ms / 77. Severity, frames >= 200 ms: baseline 10,`
> `// this fix 1, async 4. The game-thread number alone would have sold async;`
> `// summing the hitch time is what refuted it.`

**I am not proposing async, and I have not found a version of it that differs.** The refutation was
taken on total hitch time over a *flight* — the moving-tail question. The settle question it does
answer is the ladder above: mode 3 removed 1,547 ms of game thread relative to mode 2 and moved
settle by 0.6 s. Same ~27% conversion, from the other end.

Mode 3 also still carries the fatal-gate hazard, and **on this configuration it would report a
result it did not measure**: the safety arm admits a page to a worker only when every fine tile it
can touch is inside the streamer's pinned ring, and this leg logs `ringRadius=0` on the
`Fine tier ENABLED` line, with the atlas printing `asyncSafety=… RING-0-ADMITS-NOTHING`. A mode-3
arm here needs `-VoxelFineTileRingRadius=1` or it is **measuring mode 2 and printing mode 3**.

---

## 7. IS PREFETCH VIABLE? NOT FOR THIS TARGET.

**No, and the reason is structural, not a tuning question.**

**VERIFIED**, `VoxelWorldSubsystem.cpp:23065-23069`: `EnumerateSpeculativeCandidates` returns early
when `Lead.SizeSquared() < ChunkEdgeUU * ChunkEdgeUU` — the predicted displacement must exceed one
level-0 chunk edge (3.2 m).

**VERIFIED from this log:** during the whole fill segment the anchor is stationary. The
`seg=FILL scope=total` row reports `meanSpeedMps=0.0 maxSpeedMps=0.0` over all 536 frames; the leg
holds the spawn pose for a 90 s preflight before the flight clock starts.

**Velocity-lead prefetch is provably inert during cold start.** Lead = 0 → early-out → nothing
enumerated. That is not a tuning failure; there is no future position to predict when the anchor has
not moved. Answering the brief's question directly: **it does not address the actual target.**

Two further facts bound any prefetch design, worth writing down so nobody re-opens this:

- **The atlas has no velocity input at all.** `RasterAtlas->Tick(...)` is handed the *true* anchor
  (`VoxelWorldSubsystem.cpp:9960`), and every fill-order and coverage decision runs off
  `LastAnchorPxX/Y`. There are **zero** case-sensitive matches for `Prefetch|Speculat|Predict` in
  `VoxelRasterAtlas.cpp`; the ten case-insensitive hits are all comments.
- **The lead clamp is under half a page.** This leg logs
  `Speculative lead budget: 24000 UU (240.0 m) … slack above the 6000 UU VelocityLeadMaxUU clamp
  -- the cvar decides`, so the effective clamp is **60 m**. One atlas page is **240 m** of ground.
  A prefetch on today's clamp would rarely reach the next page column — which is exactly the 10.2 s
  page-crossing metronome it would be trying to pre-empt. It would need its own clamp, not
  `MaxLeadUU`.

Prefetch is a **Wave 2 lever aimed at the flight metronome.** It is not a Wave 3 lever.

---

## 8. THE FLOOR

### 8a. If "cold start" means what the player waits

**≥ 15.4 s, set by a constant.** `LoadMinHoldSeconds = 15.0f` + 0.4 s fade, measured from NEW GAME,
independent of how fast the world loads. **`<5 s` is not reachable at any streaming speed while
that constant stands, and it becomes reachable the moment it is lowered** — subject to the ready
probe, which on a 6.1 s settle is satisfied at t ≈ 7–8 s.

So the honest floor for the player-visible number is:

```
max( LoadMinHoldSeconds , settle + 1.2 s probe confirmation ) + 0.4 s fade
```

At today's settle that is `max(15.0, 7.3) + 0.4` = **15.4 s**. With the hold lowered to 5 s it is
`max(5.0, 7.3) + 0.4` = **7.7 s**, and the streaming settle becomes the binding term for the first
time. **That is the point at which Wave 3's existing work starts to matter, and not before.**

### 8b. If "cold start" means `settleT`

The binding term is CPU worldgen: **34.5 CPU-seconds** of worker work (§3c).

| worker concurrency | settle floor |
|---|---|
| observed, 5.6 thread-equivalents | **6.16 s** (observed: 6.10 s) |
| 6 physical cores, perfect | 5.75 s |
| **`<5 s` requires** | **≥ 6.9 thread-equivalents** |
| 12 logical cores, perfect SMT | 2.88 s |

**`<5 s` on `settleT` is arithmetically reachable but only above the physical core count** — it
needs SMT to actually help this workload, on a 6-core CPU, while the game thread is simultaneously
~52% busy competing for the same cores. Not impossible, not comfortable, and dependent on a machine
property nobody has measured.

**Caveat that can invert this.** `cpuJobSec` is wall time *inside* the job lambda, and the comment at
`VoxelWorldSubsystem.cpp:8409-8414` warns that in-lambda lock waits inflate it ("contention wearing
a concurrency costume"). There is a candidate on this very leg — §9 R2. If a meaningful share of the
34.5 CPU-seconds is lock wait rather than compute, the *compute* floor is lower and the settle is
contention-bound, not core-bound. That is a one-flag leg and it is worth running before anyone
accepts 6.16 s as a floor.

### 8c. If "cold start" means process launch to a settled world

**23.6 s today, floor ~17.2 s on this build even if streaming were instantaneous** (15.52 s engine
init + ~1.5 s of unavoidable tile I/O).

But that number is measured on the **wrong binary for the question**. The commandline is
`VoxelEarth.uproject -game` running the **editor** modules (`UnrealEditor-*.dll`, Development,
non-monolithic). Three of the largest pre-roll terms are editor-build artefacts:

| term | s | present in a shipping build? |
|---|---|---|
| Zen DDC service launch | 3.76 | no |
| `AssetRegistryGather … Wall time 7.5420s` + 118.6 MiB registry cache read | overlapped | no |
| dozens of `InternalLoadLibrary: UnrealEditor-*.dll` | unmeasured | no (monolithic) |
| four fine tiles, synchronous on frame `[0]` | 2.63 | **yes** |
| duplicate subsystem arm across the `Untitled`→`Entry` map load | 0.91 | probably |

**Nobody has ever measured this project's cold start on a shipping build, and the target is written
against a number only a shipping build can produce.** A packaged Shipping build with a stopwatch to
first playable frame is smaller work than any fix below, and it decides whether the target is a
target or a fiction.

---

## 9. RANKED FIXES

Every one uses an **existing** command-line switch, cvar, or a code read. None needs new code. Each
has the cheapest experiment that could refute it and what a null looks like. One leg each; all
serialise on the build lane.

### R0 — Lower `LoadMinHoldSeconds`, and measure the player's number

**The largest single term in the player's cold start is a constant, and it has a flag.**

- **Run:** an interactive (not `-unattended`) leg, stopwatch from NEW GAME to `State = Playing`,
  control vs `-VoxelLoadMinHold=5`.
- **Read:** `VoxelFrontEnd: closing the curtain after %.2fs (%s)` — the parenthesis says
  `world ready` or `timed out`, which is the whole answer.
- **Predicts:** control closes at ~15.0 s with `(world ready)`; the arm closes at ~7.3 s, still
  `(world ready)`. **A ~7.7 s saving on the number the target is about.**
- **Null / failure:** the arm closes with `(timed out)`, or closes at 5 s and the player sees holes.
  Then the ready probe is not sufficient at that hold and the 15 s was load-bearing after all —
  which is itself the finding, and points at `-VoxelLoadGateMaxRing`.
- **Watch:** the loading bar is time-floored against a 60 s denominator
  (`VoxelWorldReadyProbe.cpp:22-31`), so at a 5 s release it will snap from ~8% to 100%. Cosmetic,
  but it will look broken and someone will file it as a bug.

### R1 — Run cold start on the instrument that can see it *(not a fix; do it first)*

`voxel.Stream.FrameAttribution=1` samples **every frame from process start**, carries per-thread
means for FAST vs SLOW buckets, **and has a live GPU clock**: `FRHIGPUFrameTimeHistory` is wired at
`VoxelWorldSubsystem.cpp:6731` / `:10272`, with a self-check at `:12327` printing
`ARMED` / `*** DEAD ***`.

**This corrects a standing claim in `docs/pipeline-waves-2026-08-27.md`: there IS a per-frame GPU
timer in this project now.** It has never been run against cold start —
`grep -c "Voxel frame attribution" TJDL-A-control.log` = **0**, because the cvar defaults to 0
(`VoxelDebug.cpp:970-972`). Mode 1 is marked legacy *because* it pools cold fill with flight, which
is exactly what Wave 3 wants.

- **Run:** control leg + `-Cvars=voxel.Stream.FrameAttribution=1`.
- **Settles:** the ~48% of the settle on no voxel clock — split into game / render / rhi / gameWait
  / **GPU**. The ~950 MiB render-thread arena zero-fill (§2) would land here.
- **Null:** `GPU-CLOCK samples=0 *** DEAD ***`, or FAST `gpu=` far below ~5.8 ms → discard every
  `gpu=` figure; the residual stays unnamed.
- **Red arm:** the same leg at mode 3 (SETTLED-PARKED) must produce a FAST `gpu=` near 5.8 ms. If
  mode 3 also reads near zero, the clock is not measuring and mode 1's rows mean nothing either.
- **Pair with:** `-VoxelRenderFrame=2` for `seg=FILL TAIL`, whose `brickPoolMs` bucket is where a
  first-touch zero-fill would show.

### R2 — Arm the fine-tier lock fast path *(the contention hypothesis for §8b)*

Every leg logs `Fine lock: meter=off fast=0 | req calls=… excl=… (exclusive avoided 0.0%)`. By
settle end: **61,105 cumulative `RequestFootprint` calls, 61,105 exclusive, 0% avoided.** The arming
line spells out the cause: "RequestFootprint takes Lock_ EXCLUSIVELY on every call". CPU workers call
this inside the job lambda — so this is the named candidate for inflated `cpuJobSec`.

- **Run:** `-VoxelFineLockMeter=1` first (free, byte-identical on the lock paths), then a matched
  pair with `-VoxelFineLockFast=1`. The safety argument is at `VoxelFineTileStreamer.cpp:751`;
  **read it before arming 2, which "TRUSTS" a mirror answer (`:969`)**.
- **Predicts:** `exclusive avoided` rises above 0; `effConc` rises; `settleT` falls.
- **Null:** `exclusive avoided 0.0%` still → the switch did not latch and the leg is NOT MEASURED (a
  control wearing the flag). Or: avoided rises, `effConc` does not → the lock was not the
  contention and §8b's core-bound reading stands.

### R3 — Raise worker priority or bypass the scheduler *(attacks the §8b floor directly)*

Workers run at `BackgroundNormal`, the **lowest** priority (`WorkerTaskPriority()` default 0,
`VoxelWorldSubsystem.cpp:4244-4254`), while the game thread is ~52% busy on the same cores. Two
switches exist with their failing readings already written at `:4235-4241` and `:4268-4288`.

- **Run:** `-VoxelWorkerTaskPri=2` (foreground worker set), then `-VoxelWorkerPool=10` (dedicated
  `FQueuedThreadPool`, bypasses the scheduler so thread COUNT is the one variable).
- **Predicts:** `effConc` rises above 6.47; `settleT` falls; jobs/s rises from 8,161.
- **Null, and the in-tree note says to expect it:** "`effConc` NOT rising with the switch = priority
  was not the limiter, the background pool's thread count is". If `-VoxelWorkerPool=10` *also* pins
  `effConc` near 6 with cores idle, the limiter is OS thread priority or memory bandwidth and
  §8b's floor is real.
- **Watch:** `seg=FILL p95` and settled-moving p95 — foreground workers compete with render-side
  task work, which is presumably why Background was chosen, though no comment records a measurement.

### R4 — Raise the shared column-grid cache *(attacks `reqHdr`, the largest named term)*

5,177 evictions against a 1,024-entry cap at a 34.8% low hit rate, while `reqHdr` runs at 68x its
steady-state per-call cost.

- **Run:** `-VoxelSharedGridCache=4096` (`VoxelWorldSubsystem.cpp:3310-3311`).
- **Predicts:** window `hit%` rises, `evictions` falls, `reqHdr` per-call falls from ~75 µs toward
  the 1.1 µs steady state.
- **Null — and this is quite likely:** hit% flat → the misses are compulsory (first touch of ground
  never seen), not capacity, and no cache size fixes a cold start. A cheap, useful retirement.
- **Check first:** the size labels disagree by 2.04x (§3b). At the pessimistic 424 MB/1024 reading,
  4096 entries is ~1.7 GB. Confirm the byte accounting before running; watch RSS and
  `poolEvictions`.

### R5 — Check whether the atlas page lattice is coincident with the fine-tile lattice *(code read)*

The biggest possible win in the atlas, and it costs nothing to check. §5 has the argument. If
coincident, `SamplePage`'s 16,384 per-pixel `elevationMm` calls become a strided copy and 98% of
1,464 ms goes away with no new thread and no async hazard.

- **Read:** `FVoxelFineTileSamplerProxy::elevationMm` → its lattice indexing, against
  `VoxelRasterAtlas.cpp:876-890`'s `Px0 + Lx, Py` derivation.
- **Null:** offsets, filtering or amplification between them → dead. **Write it down as dead** and
  stop proposing atlas fill optimisations.
- **Even on success:** at the measured 27% conversion this is ~0.39 s of the settle. It matters for
  the flight metronome more than for cold start.

### R6 — Measure a packaged Shipping build's process-to-playable time

§8c. The pre-roll target is written in a language no instrument in this repo speaks. One packaged
build and a stopwatch decides whether Wave 3 is chasing 6.1 s of 23.6 s or 6.1 s of 8 s.

### R7 — Move the four fine tiles off the synchronous boot path *(2.63 s, deferred deliberately)*

The largest project-owned pre-roll term. 1,122 ms is decode, ~1.5 s is read. **Not proposed as
actionable yet**, because it changes what the world *is* at t=0 and the fine-tier gate policy on
this leg is `STOP THE RUN on the first leak` — a tile arriving late is a leak and the run dies. That
coupling has to be designed around, not switched. Revisit after R0 establishes what the real target
is.

---

## 10. INSTRUMENT WARNINGS FOR WHOEVER PICKS THIS UP

- **`elsewhereMs` on the `Hitch frame:` line cannot be read.** **VERIFIED**,
  `VoxelWorldSubsystem.cpp:10310-10312`: `ElsewhereMs = FrameMs - TickMsSoFar`, where `FrameMs` is
  the frame's DeltaTime (describing the *previous* frame) and `TickMsSoFar` is the *current* tick,
  partial. The `FFrameSample` path four lines earlier deliberately avoids exactly this by using
  `PrevTickMs` (`:10261`, `:10279-10280`); the log line does not. Proof in the log:
  `frameMs=38.02 … subsystemTickMs=124.40` at 13:04:14.156 — a part larger than its whole. Frames
  reporting 142–158 ms of "elsewhere" during the settle are **unattributed, not attributed to
  elsewhere.** R1 is the fix.
- **`raster=` and `demandMs` are the same milliseconds.** Adding them double-counts ~683 ms (§3a).
- **`fills=` counts both paths**; `demand pages=` is a subset. `++PagesFilled` is in
  `CommitStagedPage` (`VoxelRasterAtlas.cpp:1018`), not in either caller.
- **`perPage` is `(GtFillCycles + DemandCycles) / fills`,** not `fillMs / fills`. Verified against
  three windows to 1.7 ms.
- **The shared-grid-cache size is reported two ways, 2.04x apart** (208 MB vs 424 MB for the same
  1,024 entries). Read the byte accounting, not either label.
- **`capHit=0 (lifetime 0)` is confirmed dead here too** — 1,058 pages over 6.1 s, per-call max
  ~3.2 pages against a 256/tick cap. `-VoxelGpuRasterAtlasDemandPagesPerTick` cannot bind. The
  in-tree sweep record at `VoxelRasterAtlas.cpp:159-198` already calls the lever "exhausted in both
  directions"; this leg agrees.
- **A mode-3 arm on this configuration measures mode 2.** `ringRadius=0` on the `Fine tier ENABLED`
  line, and `asyncSafety=… RING-0-ADMITS-NOTHING`. Needs `-VoxelFineTileRingRadius=1` to test
  anything.
- **`-unattended` suppresses the front end**, so no `-unattended` leg has ever exercised the
  loading gate. `VoxelFrontEnd: suppressed (unattended run (-unattended))` at 13:04:02.245.
- **`VoxelPerfRun post-warmup (t>=10s)` is the instrument the settle probe exists to correct** — its
  window opens at a fixed 10 s regardless of when the world settled
  (`VoxelPerfRunSubsystem.h:284`). On a 6.1 s settle it pools ~4 s of settled play with nothing.
- **`-Cvars` separator is COMMA.** A `|` silently drops everything after it.

---

## 11. WHAT I DID NOT ESTABLISH

- Whether the atlas page lattice is coincident with the fine-tile lattice (R5). The one open
  question that could change the atlas picture, and I could not close it read-only.
- What the 15.52 s of engine init costs on a shipping build. Nobody has.
- Whether the duplicate voxel subsystem arm across the `Untitled`→`Entry` map load is avoidable.
- Whether the ~48% of the settle that is on no voxel game-thread clock is GPU, render thread (the
  ~950 MiB arena zero-fill is the leading candidate), or a real CPU stall. R1 answers this and only
  R1.
- Whether the ready probe is actually satisfied at ~7.3 s on a 6.1 s settle. Inferred from the
  probe's own cadence; R0 measures it.
- Any timing of my own. I ran no leg, opened no editor, built nothing, and committed nothing.
