# voxelsim performance scoreboard

**One page. Every number here is read from a named log line by a named tool.**
If a figure cannot be traced to a line below, it does not belong on this page.

Updated 2026-08-24. Baseline column = what the OWNER experiences (stock editor,
no extra flags). "Armed" = our leg config, which he does not run.

> ## THE TWO COLUMNS BELOW SWAPPED MEANING AT c52b2d2 — READ THIS FIRST
>
> **The owner decided to arm the GPU-primary set as shipping defaults**, so as
> of that commit the "armed" column IS what a stock editor with no extra flags
> runs. The "baseline (stock)" column is now HISTORICAL — it describes a
> configuration reachable only by passing `-VoxelGpuPrimary=0` explicitly.
> **Numbers in the table below have NOT yet been re-measured on the new
> default**; the re-measurement is the next leg. Do not quote the baseline
> column as "what the owner experiences" until this note is removed.
>
> **One flag, eleven behaviours.** `-VoxelGpuPrimary` is not one switch: it
> implies the GPU mesh fork ON, and re-points `GpuMeshInFlight` 256 -> 1024,
> `DispatchAheadCap` 0 -> 4096, `MeshBatchCap` **4 -> 64**, `MeshHarvestCap`
> 8 -> 0, and `LeanBrickJobs` OFF -> ON. The four sibling flags
> (`PoolAlloc`, `WorldGenBatch`, `StackClaim`, `RasterAtlas`) are armed
> alongside it. **Any sweep of one of those constants must now start from the
> primary-implied value, not the cvar default** — `MeshBatchCap`'s stock is
> **64**, not 4. This is the "three caps in series make a throughput number
> unattributable" hazard at a scale of eleven, and it is the price of shipping
> the block the measurements were taken on.
>
> **Atlas fill mode 3 (async) is NOT part of the set and stays OFF.** Arming
> the atlas master switch must never imply mode 3; if it ever does, that is a
> defect, not a tuning decision.
>
> **UNRESOLVED, AND IT OUTRANKS THE HEADLINE.** The matched leg behind the
> armed column (recorded in the body of `VoxelGpuPrimaryEnabled()`) reports
> `holes` **0 -> 10** and `R0` **6.4 s -> 16.4 s** under arming. Those are the
> two OWNER-VISIBLE DEFECTS below. The owner accepted this trade having been
> told the set *fixes* the arcs; on that leg it worsens them and de-prioritises
> the near ring he named first. Settling this is the first job of the next leg,
> and its failing readings are written into the code both ways so the leg can
> come out either way.

---

## THE THREE GOALS

| # | metric | baseline (stock) | armed | TARGET | status |
|---|---|---|---|---|---|
| 1 | cold start to settle | **45.5 s** | 18.1 s | **a few seconds (<= 5 s)** | FAIL |
| 2 | streaming throughput | **3,618/s** | 9,125/s | **50,000/s** | FAIL |
| 3a | frame p95 while MOVING >= 20 m/s | **44.00 ms (23 fps)** | 37.00 ms (27 fps) | **< 10.00 ms (>100 fps)** | FAIL |
| 3b | steadiness while MOVING | **31.4% stutters** | 51-58% | **<= 0.10% stutters** | FAIL |

**Goal 3 has two parts and both must pass.** A stutter is a frame over 20 ms —
a dropped frame at 100 fps. `hitches` (>= 33.3 ms) is the legacy 30-fps bar and
is kept only for comparability with historical legs; **it is not the gate.**

## OWNER-VISIBLE DEFECTS (judged by eye, not by counter)

| defect | state | resolution |
|---|---|---|
| black arcs / holes at LOD boundaries flying forward | PRESENT | capture pair, stock vs armed; owner judges |
| chunks load left-to-right, not toward facing | PRESENT | `NearestAdmit` + `ViewBias` are OFF by default |

---

## WHERE EVERY NUMBER COMES FROM

| metric | log line | how to read it |
|---|---|---|
| cold start | `Voxel cold settle: SETTLED t=..s` | `tools/leg-summary.sh`. **Read `peak:`, never `last:`** |
| throughput | same line, `mean=../s` | ditto |
| per-ring settle | same line, `R0..R5 t=..s` | R0 is the ring the player stands in |
| frame p95 / stutters | `SETTLED-MOVING scope=total ... p95Ms= stutterPct=` | **`SETTLED-MOVING` only.** Parked and fill are not the gate |
| the gate verdict | `gate=GOAL3-PASS/FAIL` | the AND of `gateP95` + `gateSteady` + `gateSpeed` |
| did the leg test the gate | `gateSpeed=` / `meanSpeedMps=` | below 20 m/s the leg is INVALID, not passing |
| thread ownership | `PIPELINE ... bound=` | void if `residualPct > 25` |
| game vs render | `PIPELINE gameBusyMs= renderBusyMs=` | the longer stage sets `floorFps` |
| holes | `voxel.Stream.CoverageVerify` | **defaults 0** — armed by harness only |

## THE CEILING THAT DECIDES GOAL 3

    seg                   gameBusy   renderBusy   floorFps   bound
    SETTLED-PARKED           1.69       9.23        108      RENDERBOUND
    SETTLED-MOVING          12.20      18.60         54      RENDERBOUND
    MOVING, after the
    admission fix            6.54      20.05         50      RENDERBOUND

**The render thread is the longer stage at every speed, parked and moving.**
Deleting 100% of remaining game-thread work cannot reach 100 fps. Goal 3 is a
rendering problem. **Parked is already 9.23 ms — a 108 fps ceiling with zero
terrain streaming.**

## WHERE I WANT TO END

    cold start   45.5 s  ->  <= 5 s
    throughput   3,618/s ->  50,000/s
    p95 moving   44 ms   ->  < 10 ms      (both at >= 20 m/s)
    stutters     31.4%   ->  <= 0.10%
    render frame 18.6 ms ->  < 10 ms      (this is the binding one)
    arcs         present ->  absent, owner-judged
    load order   L-to-R  ->  nearest-first, owner-judged

**Every arm reports its `SETTLED-MOVING gate=` alongside its headline. An arm
that improves cold start or throughput and worsens the moving p95 is not a win.**

## STANDING READING RULES (each earned by a retracted finding)
- Read `peak:`, never `last:` — the linger window reads zeros. **Ten wrong
  findings tonight came from the wrong window.**
- A percentage without its absolute is not a measurement.
- Wait time spread across N threads is not wall time.
- Game-thread milliseconds are not wall milliseconds (~0.25x on this pipeline).
- A bucket that cannot go negative is not a measurement.
- A residual that cannot stay large is not a residual.
- A check that has never failed is not yet known to be a check. **Six mutation
  arms exist in this codebase and none has ever been run.**
