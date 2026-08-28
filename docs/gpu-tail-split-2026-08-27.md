# The +5.8 ms GPU step, named. Answer to `docs/gpu-tail-attribution.md`.

2026-08-27, after the survey. **Nothing in the GPU frame is unattributed any more.**
Two matched moving legs, 9,000 CSV frames each, `1552x873` (60.6% screen percentage
that TSR upscales to the owner's 1440p), spawn `-61440,-61440`, `line` flight,
frozen 12:00 03-20, `DOUBLE GRANT = 0` on both.

## 0. The result in one table

FAST (<=p50 frame) -> SLOW (p95..p99 frame). Two independent legs; every term
reproduces within ~0.1 ms, which is the reproducibility claim rather than an
assertion that it should.

| GPU term | what it is | dSLOW C | dSLOW B1 | share |
|---|---|---:|---:|---:|
| `GPU/VoxelStreamRgBand` | **BandReduceMain**, one pass per band-carrying job | **+1.463** | **+1.349** | **25%** |
| `GPU/VoxelMarch` | the marcher | +1.240 | +1.050 | 20% |
| `GPU/VoxelStreamWlVoxelize` | worklist VoxelizeWorklistMain (per tick, indirect) | +0.899 | +0.804 | 15% |
| `GPU/VoxelStreamWlColumn` | worklist ColumnWorklistMain (per tick, indirect) | +0.694 | +0.635 | 12% |
| `GPU/VoxelStreamRgColumn` | classic ColumnMain, in the kept mesh-region graphs | +0.636 | +0.607 | 11% |
| `GPU/VoxelStreamRgVoxelize` | classic VoxelizeMain, same graphs | +0.411 | +0.413 | 7% |
| `GPU/Unaccounted` | queue time inside no stat scope at all | +0.259 | +0.349 | 5% |
| `GPU/VoxelStreamWlClassify` / `WlPack` / `MeshJob` / `WlStamp` / `WlClaim` / `PoolWrite` / `Pool` | the rest of streaming | +0.28 | +0.26 | 5% |
| everything else (Basepass, ShadowDepths, TSR, Postprocessing, water, fog, sky, lights, MarchEmit) | the whole draw path | **-0.11** | **-0.10** | -2% |
| **SUM = graphics queue busy** | | **7.116 -> 12.881 (+5.797)** | **7.200 -> 12.610 (+5.442)** | |

**Streaming is +4.2 ms of the +5.8, i.e. 73%. The marcher is +1.15 (20%). Unnamed is
+0.30 (5%). The draw path is NEGATIVE.**

The sum is the check, not a claim: `GPU/Unaccounted` is the engine's own residual
(`GPUProfiler.cpp:800`, accumulated at `:1556` only while the stat stack is empty),
so the columns sum to the queue's busy cycles by construction and anything genuinely
unnamed has nowhere to hide.

And it lands on the number everyone has been quoting. The engine's own attribution
line on the same leg reads `FAST gpu=7.11 -> SLOW gpu=12.68`; the CSV sum reads
`7.116 -> 12.881`. Same quantity, ~0.2 ms apart.

## 1. What was armed, and how it was proved armed BEFORE any number was read

`-csvGpuStats` + `CsvProfile FRAMES=9000` deferred into the flight, plus
`DECLARE_GPU_STAT_NAMED` scopes on every standalone streaming `FRDGBuilder`.

Three arm checks, all pre-registered, all passed:

1. **`GPU/` columns exist.** 56 of them. `r.GPUCsvStatsEnabled` defaults 0 and with
   it off the columns are simply ABSENT with no error; a capture without them is
   void, not null.
2. **RED ARM: `GPU/VoxelMarch` p50 = 4.15 ms** on a leg where the marcher is
   independently known to cost ~3-4 ms. A scope known to be live reads live, so a
   streaming scope reading zero means zero rather than broken plumbing.
3. **The instrumented binary still measures the same game.** `FAST gpu=7.11` against
   the uninstrumented control leg's `6.97`; `TAIL gpu=13.29` against `13.29`;
   p95 frame 15.50 against 15.30. Inside the ~1.0 ms run-to-run noise floor.

### The trap that cost the first leg

`RDG_EVENT_SCOPE_STAT` at a top-level `FRDGBuilder` **crashes**:

    Assertion failed: LocalCurrentBreadcrumb == FRHIBreadcrumbNode::Sentinel
    RenderGraphBuilder.cpp:1770 ... VoxelRasterAtlasGpu.cpp:162

These builders call `Execute()` *inside* the scope, not after it. `FRDGBuilder::Execute`
checks its breadcrumb is back at Sentinel. Fix: **`RHI_BREADCRUMB_EVENT_STAT` on the
RHI command list, declared before the builder** so it outlives the graph. Same stat,
same CSV column, no assert. The RDG form is correct and is used for the *nested*
sub-terms, which close when their function returns.

(The survey's warning about the three dead spellings -- `SCOPED_GPU_STAT`,
`RDG_GPU_STAT_SCOPE`, `RDG_RHI_GPU_STAT_SCOPE` -- was right and was avoided.)

## 2. TEN scopes produced NO column at all: measured absence

A scope that opens and costs nothing still creates its column -- `GPU/VoxelStreamAtlas`
proves it, at 0.0014 ms. So an absent column means the code never ran ONCE in 9,000
post-settle flight frames. These never ran:

| absent scope | what that retires |
|---|---|
| `VoxelStreamChunkIndex` | **survey candidate 8.** Corroborated by the counters: `full=3 delta=13,813` for the WHOLE run, and all three fulls in preflight (`first=0 seed=1 pending=2 large=0 lost=0`). Three frames cannot move a 360-frame p95 bucket. |
| `VoxelStreamPoolUpload` | **survey candidate 4** (the ~860 KB chunk-table rewrite). Expected under `voxel.March 1` -- the quad pool is not in use -- but now measured rather than assumed. |
| `VoxelStreamRgMesh` | the quad/mesh chain inside `AddRegionPasses` (MeshCount, 3 scans, MeshEmit, QuadTotal) **never executes**. |
| `VoxelStreamRgBrickPack` | the brick-pack chain inside `AddRegionPasses` never executes (the worklist owns it). |
| `VoxelStreamRgAsset` | AssetStampMain, the "one dispatch per instance" block, never executes in the region graph. |
| `VoxelStreamQuadIO`, `VoxelStreamBrickStack`, `VoxelStreamResidency`, `VoxelStreamGIMarch`, `VoxelStreamRegionBlocking` | never execute. Residency was already known inert; the other four were not. |

`GPU/VoxelStreamAtlas` DOES exist and is **0.0014 ms with dSLOW -0.0001**. Survey
candidate 5 is not merely too small to be the tail -- it is too small to be the floor.

## 3. Lead or lag: same frame, and the reverse-causation confound does not apply here

Two alignments were measured, because they answer different questions and using one
for the other silently changes the answer (it moves the FAST->SLOW GPU step from
+5.8 ms to +2.1 ms):

- **-3 rows, from the GPU-CLOCK IDENTITY** (r = 0.965-0.969 against
  `VoxelStream/GpuFrameMs`, which is the same physical quantity). This is the pairing
  the engine's own `gpu=` already uses, so the table in §0 decomposes exactly the
  number the FAST/SLOW/TAIL rows report. **Used for the split.**
- **-1 row, consensus of 11 streaming terms against `ChunksApplied`.** The streaming
  dispatches ARE the arriving chunks' passes, so that pairing is same-frame by
  construction. The 2-row difference is the pipeline depth. **Used for lead/lag only.**

Net of that, every streaming term peaks at **net +0 -- the same frame -- at
r = 0.81-0.83**, and the marcher peaks at r = 0.08, i.e. nothing.

**The reverse-causation confound does not reach this result, and the reason is
mechanism rather than lag.** The worry was that an unlimited harvest cap lets a
22 ms frame collect 2.4x the chunks a 9 ms frame does, so chunks/frame would
correlate with frame time with zero causal contribution. That argument works on
*frame time*. It does not work on **GPU busy cycles executed inside the very
dispatches those chunks produce**: there is no route by which a long CPU frame makes
`BandReduceMain` or `ColumnWorklistMain` consume more GPU cycles other than by
containing more chunk records. Idle is excluded from the clock by construction
(`GPUProfiler.cpp:943-944`), so the extra time is work executed.

## 4. What the biggest term is, and the fix that is NOT available

**`BandReduceMain` is the largest single named GPU term in the step: +1.41 ms, 25%.**
One pass per band-carrying job, gated on `Request.BandEdge > 0`.

Two switches for it already exist and both default OFF: `-VoxelGpuBandColdOnly` and
`-VoxelGpuBandSeedOnly` (`VoxelWorldSubsystem.cpp:20567`, `:20617`). `-VoxelGpuBandSeedOnly=1`
promises at most one band-carrying job per footprint, ever.

**It was run (BAND-B1) and it is a NULL, and the counters say why rather than leaving
it as "no effect":**

    leg          seed     dup   redundant     free   |  [gpu-lean] kept because band
    GPUSPLIT-C  31,671      0           0  207,069   |  31,671
    BAND-B1     31,609      0           0  207,005   |  31,609

`dup=0` and `redundant=0` on **both** arms. The legacy path was already emitting
exactly the seed-only population -- one request per virgin footprint, nothing cached
to be redundant against and nothing in flight to dedupe. **There is nothing for
seed-only to remove on a `line` flight.** The GPU term confirms it at a far sharper
resolution than frame time could: `RgBand` dSLOW 1.463 (control) vs 1.349 (arm),
and frame p95 15.50 vs 15.80 -- both inside noise.

So the lever is not "request fewer bands". It is either the cost of `BandReduceMain`
itself, or whether the band is needed at all under the marcher. **Not answered here,
and not guessed at.**

Same for `RgColumn` + `RgVoxelize`, +1.05 ms combined: `wlcols conv=483,014 fb=387`
says 99.9% of worklist RECORDS are converted, but those 387 fallbacks cannot cost
1 ms. The population differs -- `[gpu-lean] kept=31,636` mesh-region graphs are
called as `AddRegionPasses(GraphBuilder, Job->Region)` with **no column feed**
(`VoxelGpuMeshJobManager.cpp:~4745` passes `bFeedColumns ? &ColumnFeed : nullptr`
only for the BRICK region), so those 6.5% of jobs run the classic Column and
Voxelize kernels. A counter honest about its own population, read as if it covered
another one.

## 5. The marcher's +1.15 ms

It rises, and it is **not** the cause: halving ray count moved the moving p99 delta
0%. Its correlation with chunks is r = 0.08. A hypothesis with a falsifier, not a
claim: streaming-heavy frames are frames with terrain not yet resident, and a ray
that finds nothing marches further. That would make the marcher's rise a
*second-order streaming cost*. **Falsifier:** it predicts `GPU/VoxelMarch` correlates
with an unresolved-coverage counter and not with chunk count -- which is the shape
already seen. It has not been tested and must not be quoted as a finding.

## 6. Where the p99 is NOT

Framed against SLOW deliberately. Pooled over 13 legs the GPU rises +5.85 ms to p95
and only +0.53 ms more to p99 -- **it saturates at ~13 ms**. Both legs here reproduce
that: SUM SLOW 12.881 / TAIL 12.867 (C), 12.610 / 12.978 (B1). **A p99-framed GPU
conclusion would be attributing a step the GPU does not take.** The p99 is a
game-thread step and is a different investigation.

## 7. Instrument changes shipped with this (uncommitted)

- `RHI_BREADCRUMB_EVENT_STAT` on 21 standalone streaming `FRDGBuilder` sites;
  `RDG_EVENT_SCOPE_STAT` on 13 nested sub-terms.
- `CSV_DEFINE_CATEGORY(VoxelStream)` and 16 per-frame game-thread columns, so the
  correlation is computable inside one file instead of inferred across two
  instruments.
- The GPU-history drain moved out of the attribution gate and made unconditional.
  Not a behaviour change: the drain takes the FRESHEST timing and discards the
  backlog, so a qualifying frame reads the same value either way.
- `FFrameSample` gained `RemeshMs`, `UnloadMs`, `BrickFlushMs` and the six dispatch
  sub-terms, and two new report lines print them over FAST/SLOW/TAIL --
  the population the `Hitch frame dispatch` line's 33.3 ms bar could never reach.
  First readings, same leg:

      TICK-STAGES  FAST tick=0.55 dispatch=0.20 apply=0.02 remesh=0.00 unload=0.03 brickFlush=0.00 other=0.29
                   SLOW tick=3.24 dispatch=1.62 apply=0.23 remesh=0.00 unload=0.08 brickFlush=0.03 other=1.29
                   TAIL tick=10.44 dispatch=5.96 apply=0.59 remesh=0.00 unload=0.19 brickFlush=0.06 other=3.64

      DISPATCH     TAIL disp=5.96 airProof=0.03 band=0.01 submit=4.98 pick=0.02 overlay=0.01 dOther=0.91 gpuMgr=0.44

  **remesh is 0.00 everywhere and unload is +0.15 at p99: neither is the within-tick
  residual.** `submit` is +4.95 of dispatch's +5.76 -- 86% of it. What is left is
  `other` = +3.35, tick time outside all five brackets, which is now a measured gap
  with a name rather than an absence.
- `tools/csv-gpu-attrib.py`: the offline reader. Refuses a capture with no `GPU/`
  column or a dead red arm; measures both alignments and says which it used;
  buckets on FRAME time (bucketing on GPU total selects each term into its own tail)
  and reports SLOW before TAIL.

## 8. Reproduce

    tools\voxel-run-flight-leg.ps1 -LogName <name> -Width 2560 -Height 1440 `
      -SpawnAt '-61440,-61440' -Flight line -PreflightSec 90 -RunSec 120 -LingerSec 60 `
      -Cvars "voxel.Stream.CoverageVerify 1, voxel.Stream.FrameAttribution 2, csv.CompressionMode 0, voxel.DeferExec 112 CsvProfile FRAMES=9000" `
      -ExtraArgs @('-csvGpuStats')
    python tools\csv-gpu-attrib.py ue-project\Saved\Profiling\CSV\Profile(*).csv

Legs on disk: `Saved/GPUSPLIT-C.log` + `Saved/csv-GPUSPLIT-C.csv` (control),
`Saved/BAND-B1.log` + `Saved/csv-BAND-B1.csv` (`-VoxelGpuBandSeedOnly=1`).
