# The GPU job manager's own per-chunk cost — inventory, collapses, hooks

2026-08-23. Lane: `VoxelGpuMeshJobManager.cpp/.h` only. Authored, **NOT COMPILED
AND NOT RUN** — the single build/editor lane was held by `af2d2da997b15b523`.

---

## 1. What this lane was asked and what it found

The standing blocker: `dispatch=1,547 ms` per 5 s window on the GPU-primary arm
against a `211 ms` control, divided by `gpuForked=10,391` = **0.149 ms of game
thread per chunk routed to the GPU**, i.e. 7.4 s of game thread per second at
50,000 chunks/s.

**The division may be against the wrong denominator, and that is the first
finding.** `dispatch=` is `AccumDispatchMs`, and it brackets `T0..T1` plus
`T3..T3b` — which spans *both* the per-chunk pop/submit loop *and*
`GpuMeshJobs->Tick()` in its entirety (`VoxelWorldSubsystem.cpp:9371`, and the
manager tick is separately accumulated at 9384 into `AccumGpuManagerTickMs`).
Those two do not scale with the same thing:

    per 5 s window at 30-60 fps  ->  150-300 ticks
    1,547 ms / 200 ticks         =  7.7 ms PER TICK

which is the same order as this class's own recorded figure, "`Tick()` was
measured at ~18-19 ms per hitch frame" (`VoxelGpuMeshJobManager.h`, on
`GetAndResetTickStageMs`). `gpuMgrTickMs=` is already printed on the "Voxel
dispatch stages" line — **with no tick count beside it**, so nobody can divide
it. Section 4 has the one-line hook that fixes that.

---

## 2. Per-chunk cost inventory for this manager — counts, not adjectives

Everything the manager does on the game thread per chunk submitted, promoted,
dispatched and delivered. `I` = asset instances on the chunk, `C` =
`AssetColStarts.Num()`, `S` = `AssetSpans.Num()`, `R` = raster window pixels,
`Q` = queue depth, `N` = `InFlight.Num()` (bounded by `MaxInFlight` = 288 under
`-VoxelGpuPrimary`: `JobsInFlightPerCore` 8 x 36 logical cores).

### Submit() — once per chunk

| # | Site | Cost per chunk | Verdict |
|---|------|----------------|---------|
| 1 | `MakeShared<FJob>` | 1 malloc, `sizeof(FJob)` ~720 B + control block. `FJob` carries **two** whole `FVoxelGpuRegionRequest` (192 B each) | small, unavoidable |
| 2 | `Job->Region = MoveTemp(Region)` | 192 B scalar copy, arrays moved | free |
| 3 | **`MakeBrickRegion`: `OutReq = MeshReq`** | **`4*(2R)` raster + `44*I + 4*(C+S)` assets, malloc + memcpy, up to 5 heap allocations** | **COLLAPSED — see §3** |
| 4 | `MakeBrickRegion` anchor fixup | `I` iterations, 2 subs each | free |
| 5 | `ValidateRegionRequest(BrickRegion)` | O(I) + ~20 branches; builds no `FString` on the pass path | small |
| 6 | `Queued.Add` | amortised O(1) | free |

Term 3 is the one that matters and it is the direct successor to the owner's
original finding. **The owner's "FillRasterWindow samples ~46 KB per chunk" is
dead for the right reason and only for the raster half**: under
`-VoxelGpuRasterAtlas` the request carries no window at all
(`ValidateRegionRequest` *refuses* a request that is both atlas-armed and
window-filled), so `4*(2R)` is exactly 0. The **asset** half was never touched
by the atlas, and this copy duplicates a chunk's whole species column-prefix and
span tables. That is the same shape of per-chunk marshalling in the same
function, and nothing in the tree counted it.

**The same span tables are marshalled THREE times per chunk on the game thread:**

1. `VoxelWorldSubsystem.cpp:18147` — `Req.AssetColStarts.Append(S.ColStarts);
   Req.AssetSpans.Append(S.Spans);` (the lane holder's `assets=` bucket);
2. `MakeBrickRegion`'s `OutReq = MeshReq` — **this lane, now collapsed**;
3. `VoxelGpuMeshJobManager.cpp` DispatchBatch — `P.ColStarts =
   Reg.AssetColStarts; P.Spans = Reg.AssetSpans;` into the worklist payload,
   only when `-VoxelGpuWorklistAssetStamp` is armed. Not collapsible from this
   lane: `FVoxelWorklistAssetPayload` is `VoxelGpuWorklist.h`, another lane's
   file. **Left as a named target.**

### Tick() promote — once per chunk

| # | Site | Cost per chunk | Verdict |
|---|------|----------------|---------|
| 7 | `Source.RemoveAt(0, EAllowShrinking::No)` | `8*(Q-1)` bytes of memmove | **measured-out, do NOT fix.** At the observed `gpuDemandPending=240` that is 1.9 KB and ~0.1 µs — **0.07% of the 149 µs budget.** An O(1) head cursor would touch 8 call sites for nothing. |
| 8 | `ValidateRegionRequest(Job->Region)` | O(I) + ~20 branches | small |
| 9 | `ComputeRegionGraphSizes` | pure arithmetic, reads no array | free |
| 10 | `PromotedSeconds` stamp | 1 `FPlatformTime::Seconds()` | free |

### DispatchBatch — once per chunk

| # | Site | Cost per chunk | Verdict |
|---|------|----------------|---------|
| 11 | `Pool.AllocateGpuChunkShell` | 3 arena allocs + `Resident.Add` + `PendingGpuIndexAdds.Add` + a 64-entry ring write | required |
| 12 | **`Pool.DebugGetResidentChunk` (rule-2 revalidation)** | **1 `TMap` lookup on a 16-B key + 1 ~64-B `FResidentChunk` copy-out** | **COLLAPSED — see §3** |
| 13 | worklist record build | 1 `FVoxelGpuChunkWorkRecord` append + shading pack | required |
| 14 | worklist asset payload | `44*I + 4*(C+S)` again (see above) | other lane's struct |
| 15 | `TSet<FVoxelGpuBrickStack*> TalliedStacks` | 1 hash probe/chunk, only under `-VoxelGpuWorldGenBatch` (default off) | free on the leg |

### PollInFlight — per TICK, not per chunk

| # | Site | Cost per tick | Verdict |
|---|------|---------------|---------|
| 16 | walk 1 (`ToPoll` build) | `N` state loads + `N` atomic CAS on cold lines | ~30 µs/tick at N=288 |
| 17 | walk 2 (`TotalDone` scan) | `N` state loads + `N` atomic CAS | ~20 µs/tick |
| 18 | walk 3 (finish/timeout, reverse) | `N` state loads; `RemoveAt` shifts | ~15 µs/tick |
| 19 | 4 x `ENQUEUE_RENDER_COMMAND` | a `MoveTemp` and a few scalars **if the render thread takes it** | **UNKNOWN — now bracketed, see §3** |

**Add up 1-18 and this class cannot reach 0.149 ms/chunk however it is
counted.** 3 x 288 = 864 job visits per tick is tens of µs; every per-chunk term
is a few hundred bytes. **The only mechanism in these two files that CAN produce
that number is the game thread BLOCKING at a render-command handoff against a
saturated render thread** — and the GPU-primary arm is 66.3% GPU-busy against
the control's 3.7%, with `dispatched=7301 drained=126`. Nothing anywhere split
those four handoffs apart. Now they are.

---

## 3. What shipped

### `[gpu-jobcost]` — always on, ~5 s cadence, silent on a leg with no GPU jobs

Every figure ships with the count it is per, because that is precisely what the
0.149 ms reading did not have. Printed from `FVoxelGpuMeshJobManager::Tick`.

    [gpu-jobcost] submits=N subUs=X (hdr A + brick B + queue C, drift D)
      | copyPerChunk rasterB=.. assetB=..
      | ticks=T promoted=P batches=.. pollJobs/tick=..
      | chargedMs=.. = enqDisp .. + enqPoll .. + enqFetch .. + enqRel .. + deliver ..
      | perTickUs=.. perDeliverUs=.. delivered=..
      | jobLean=ON assetMove=.. assetCopy=.. bytesSaved=.. revalSkip=.. revalRan=..

The Submit buckets are **contiguous**, so `drift` must print ~0.000 ms; a
non-zero drift means someone broke the bracketing, not that time went missing.

**FAILING READINGS** (stated at the log site too):

* `subUs` near zero while `dispatch=` stays ~1,500 ms — **this manager's
  per-chunk half is not the cost.** Read `perTickUs` next.
* `perTickUs x ticks` accounting for the bulk — **the cost is PER TICK**, and
  every per-chunk optimisation in this file is beside the point.
* `enqDisp` / `enqPoll` dominating `chargedMs` — **render-thread backpressure**:
  the game thread is waiting at the handoff. Different file, different fix.
* `rasterB > 0` — the raster atlas is **declining** and the inline 46 KB window
  fill is live; every "the atlas fixed it" claim is void for that leg.
* `assetMove=0` with submits flowing and the switch armed — the collapse
  converted nothing; an explicit Warning line names the four preconditions.
* `bytesSaved=0` with `assetMove>0` — the moves ran and there was nothing to
  move. The leg has no asset traffic; **this collapse can be credited with
  nothing**, and must not be.
* `revalRan` growing on a leg reporting `evictions=0` — duplicate chunk keys are
  reaching one batch. A finding of its own.

### `-VoxelGpuJobLean=1` — latched, default OFF, byte-identical off

**Half one — the brick region's asset tables MOVE instead of being copied.**
New `VoxelGpuChunkRegion::MakeBrickRegionMoveAssets`. Removes `44*I + 4*(C+S)`
bytes of malloc + memcpy and up to 3 heap allocations per chunk.

Gate, mirroring DispatchBatch's lean gate term for term plus the atlas:
`-VoxelGpuJobLean` AND `-VoxelGpuLeanBrickJobs` AND `!bQuadMesh` AND
`BandEdge == 0` AND `bRasterAtlas`.

Why it is safe: under `-VoxelGpuLeanBrickJobs` a brick-only, band-free, packing
job **never calls `AddRegionPasses(GraphBuilder, Job->Region)` at all** — the
mesh region is built, copied, and read by nothing. What `Job->Region` loses:
`ValidateRegionRequest(Job->Region)` then skips its asset block, but that
coverage is **moved, not lost** — Submit validates the brick region, which now
holds those same instances, before latching `bBrickPack`.
`ComputeRegionGraphSizes` reads neither the instances nor the tables. The one
path that can still dispatch the mesh region afterwards — a job whose pool shell
is refused in DispatchBatch, clearing `bBrickPack` — was **already** generating
into a graph with no readback attached and discarding it.

The raster arrays are deliberately **not** moved even though they are empty
under the atlas: moving them would make `ValidateRegionRequest(Job->Region)`
*fail* on the inline-window control path, where they are 46 KB and real.

**Half two — skip DispatchBatch's shell revalidation when it is provably a
no-op.** Removes term 12.

A shell can only be stolen if something was **removed** from the pool's resident
map while the shells were being taken. There are two such removals and the
obvious guard catches only one: `EvictOne` (counted by `GetEvictions`) and
`AllocateForChunk`'s same-key replacement (counted by nothing — two queued jobs
for one chunk key, a state the Tier B.1 grouping comment already names as
reachable). Gating on evictions alone would be exactly the derived-instead-of-
checked join this project keeps paying for. So the gate is the identity

    (GetNumResidentChunks() delta) == (GetChunksAdded() delta)

across the shell loop: every successful allocation increments `ChunksAdded` and
grows `Resident` by one **unless it removed something first**. Deltas agreeing
means nothing was removed by any mechanism, named or not.

---

## 4. Hooks for the lane holder (`VoxelWorldSubsystem.cpp`) — NOT edited here

**None are required.** `[gpu-jobcost]` prints itself from the manager's own
`Tick`, and the switch is a command-line latch. The manager side is complete and
measurable with zero changes to your file.

**One hook is worth taking**, and it is the smallest one on the table.

### H1 — give `gpuMgrTickMs` its denominator (1 line changed, 1 added)

`gpuMgrTickMs=` is the whole reason the 0.149 ms figure cannot be checked: it is
printed as a bare window total beside a per-chunk `perDispatch`, so a per-frame
cost reads as a per-chunk one. `AccumTicks` is already in scope in the same
function.

Function: the periodic perf log containing `TEXT("Voxel dispatch stages (5s
window): ...")`. At time of writing that is `VoxelWorldSubsystem.cpp:10849-10858`.

Replace the format fragment on **line 10851**:

```cpp
	       TEXT("+ pick=%.1f + overlay=%.1f + other=%.1f | gpuMgrTickMs=%.1f | dispatched=%lld ")
```

with:

```cpp
	       TEXT("+ pick=%.1f + overlay=%.1f + other=%.1f | gpuMgrTickMs=%.1f over %d ticks ")
	       TEXT("(%.3fms/tick) | dispatched=%lld ")
```

and replace **line 10857**:

```cpp
	       AccumGpuManagerTickMs, (long long)JobsDispatchedSinceLog,
```

with:

```cpp
	       AccumGpuManagerTickMs, AccumTicks,
	       AccumTicks > 0 ? AccumGpuManagerTickMs / double(AccumTicks) : 0.0,
	       (long long)JobsDispatchedSinceLog,
```

Reading: if `ms/tick` times `ticks` accounts for the bulk of `dispatch=`, the
blocker is **per tick** and the whole "0.149 ms per chunk" framing is a division
artefact. If it does not, the per-chunk framing stands and your six
`AccumGpuSubmit*` buckets name which phase.

### H2 (optional) — a per-chunk denominator on the brick flush

`Voxel brick flush (5s window, both call sites): prepMs/enqueueMs/sinkMs` has
the same problem. Beside it, `AccumBrickFlushMs` and the pack rate already exist.
No code offered; noted so it is not missed.

---

## 5. The leg that measures this

Two matched cold-start legs on the sanctioned harness (memory:
`voxelsim-headless-leg-harness`), read with `tools/leg-summary.sh`, **never**
`grep | tail -1`:

    ARM A (control, instrument only — proves the line and names the mechanism)
      ...the standing -VoxelGpuPrimary cold-start flags, unchanged...

    ARM B (collapse armed)
      ...the same flags... -VoxelGpuJobLean=1

Read in this order:

1. **`[gpu-jobcost]` on ARM A.** `drift` ~0.000. Then `subUs` vs `perTickUs x
   ticks`: which of the two is the mass? Then `enqDisp`/`enqPoll` inside
   `chargedMs`: is the game thread waiting? **This decides the whole
   investigation, and ARM A alone answers it.**
2. **`copyPerChunk assetB=`** on ARM A — the actual byte count per chunk this
   lane's collapse removes. If it is ~0 there are no assets on the leg and
   half one of the switch is credited with nothing.
3. **ARM B `assetMove=` and `revalSkip=`** — did the work MOVE? Non-zero, or the
   switch is inert and arm B measured nothing. Only then is any timing
   difference between the arms interpretable.
4. **`dispatched=`/`drained=` together** (handoff rule): a diverged pair
   invalidates any cap reading on either arm.

**Do not read a throughput difference between the arms before step 3.** Eleven
features in this project have read healthy while doing nothing; `assetMove` and
`revalSkip` are the two counters that make that state unrepresentable here.
