# Handoff: streaming pipeline, day 2 — what shipped, what was disproved, what is next

**Date:** 2026-08-23
**Supersedes:** `docs/marcher-handoff-2026-08-22.md` (read it for the derivation of the
6,200 chunks/s floor and the owner's decisions; both still stand)
**Working dir:** `D:\voxelsim` (the single checkout)

Start at §1. Every number below came from a matched 30 m/s `line` leg, same spawn
(`-61440,-61440`), frozen sun, 1600x900, via `tools/voxel-run-flight-leg.ps1`.
Read leg logs with **`tools/leg-summary.sh <name>...`**, not with `grep | tail -1` — see §6.

---

## 1. The headline: two of yesterday's premises were wrong

**`jobGHz=0.04` — "worker threads are descheduled ~99% of their life" — was an instrument
artifact, and it is gone.** `Result.JobMs` meant task-body wall time on the CPU arm but
end-to-end latency including queue wait on the GPU arm, and the always-on level-0 census
accumulated both. It reported **1,226,578 ms of "worker time" in a 5-second window** —
30x more than the machine has worker threads to supply.

Fixed. The census now reads `cpuJobGHz=3.66` against `gridGHz=3.69` — two independently
derived numbers that now agree, where before they differed by 80x. **The workers were
running at full speed the whole time.** Level-0 worker cost is `p50 0.77 ms`.

**The other correction:** the real background worker count is **8**, not the ~12 assumed,
and the in-flight cap **never binds** — `cpuInFlight` sits at 0 while `gpuDemandPending`
pins at its own cap. So `voxel.Stream.JobsInFlightPerCore` 8 → 2 would have measured
nothing. That experiment is cancelled, not deferred.

---

## 2. What shipped, with its measurement

### 2.1 Phase 0.1 — the inner ring edge admitted and evicted the same band forever

Admission padded the inner edge inward; the exit scan still evicted at the raw radius, so
the band between them churned on every recompute. Both radii now come from
`VoxelStreamAdmission::InnerAdmitUU` / `InnerEvictUU`, and the three other sites that had
each hand-written the same test call them too.

Same binary, both arms (`-VoxelInnerHysteresisChunks=-0.7071` reproduces the old radii):

| | frames | p50 | p95 | unloads/s | brickPacks | tracked | holes |
|---|---|---|---|---|---|---|---|
| thrash | 6,602 | 27.42 ms | 75.34 ms | 1,291 | 444,038 | 115,917 | 0 |
| fixed | 8,245 | 22.23 ms | 67.48 ms | 1,653 | 511,668 | 124,039 | 0 |

**The handoff's stated gate could not fail.** "Stationary unloads/s 11,779 → 0" measures
0/s on the *pre-fix* binary too, because the desired set is only rescanned once the anchor
moves a quarter chunk edge. Stationary, the band is evicted once and never re-admitted.
The churn needs motion — which is the condition the owner flies in.

### 2.2 Shared column-grid cache — the biggest win of the day, now ON by default

The probe leg the last handoff asked for: **83.6% hit over 324,193 level-0 dispatches,
4.40x footprint reuse.** The 8–16 chunks stacked over one footprint were each rebuilding
the same 34×34 column grid.

| | cycPerColumn | brickPacks | frames | p95 | hitches |
|---|---|---|---|---|---|
| off | 3,056–3,211 | 475,145 | 7,626 | 68.94 ms | 3,316 |
| **on (1024)** | **902–1,068** | **623,804** | **10,321** | **42.87 ms** | **1,693** |

Coverage identical (holes=0 both arms). **1024 is the knee, measured:** 4096 raised hit%
63.0 → 69.4 and *lowered* throughput (packs 623,804 → 583,834, p95 42.87 → 51.53 ms) for
1,698 MB against 424 MB. Default on; disable with `-VoxelNoSharedGridCache`.

**It costs 424 MB of system RAM, not the ~208 MB the design computed from `sizeof(FGrid)`.**
Read the MB off the perf line; do not compute it.

### 2.3 Instruments that were lying, now split

- CPU service time vs GPU submit-to-deliver latency, globally and per level. The per-level
  GPU row reads `GpuSubmitToDeliverMs`, **not** `JobMs` — `JobMs` is 0 on that arm now, so
  feeding it would have produced a row of zeros that looked like a working instrument.
- The HUD's jobs-in-flight cap read a stale `2 × cores` (24) against a real `8 × cores`
  (96). One `MaxJobsInFlightCap()` now serves both the dispatcher and the readout.
- `cpuInFlight` / `gpuDemandPending` / `specPending` are published separately.

### 2.4 Speculation adopted nothing — the mechanism, found and fixed

Under `voxel.Terrain.RetireQuads`, **every** GPU delivery carries `NumQuads == 0` by
configuration. `ParkSpeculativeResult`'s emptiness test read that as "meshed empty", so
**100% of speculative results were discarded** — after being generated, made resident in
the brick pool, and rendered. Demand then re-dispatched a second full GPU job per chunk.

`voxel.Stream.SpeculativeParkBricks 1` takes adoption from **0% to 98%** (16,452
dispatched / 15,884 adopted). Holes stayed 0.

**Default OFF, deliberately.** The adoption counter is decisive about the mechanism; the
throughput delta is not. n=2 per arm overlaps: control frames 8,049 / 8,786, spec 10,180 /
8,612. Waste that is obviously waste has measured worse in this codebase before
(`RingOverlapChunks`), so this needs the owner's own session or more legs before it flips.

### 2.5 The index upload was the ceiling — 56 MiB per flush, now ~74 KiB

`MarkDirtyAndUpload` copied the whole 56 MiB cell grid per flush and handed it to
`QueueBufferUpload`, which copied it again and uploaded all of it. Now only changed cells
go up, as `[cell,value]` pairs scattered by a small compute pass into the persistent
buffer. **A flush averages 145 changed cells, not the ~9,500 estimated** — 70.6 MB moved
across a whole flight where the full path moved ~850 GB.

n=2 per arm, quiet box. **The arms do not overlap:**

| | frames | p50 | p95 | hitches | brickPacks | chunks/s mean / peak |
|---|---|---|---|---|---|---|
| full | 8,786 / 8,049 | 22.21 / 21.27 ms | 67.32 / 69.28 ms | 2,189 / 2,689 | 562,898 / 482,818 | 2,760 / 4,248 |
| **delta** | **12,323 / 12,381** | **20.71 / 20.73 ms** | **32.11 / 31.50 ms** | **562 / 536** | **768,010 / 768,784** | **4,392 / 7,501** |

Coverage identical, holes=0 of ~24,320 scanned in every leg. The delta arm reproduces to
within 0.5% on frames and 0.1% on packs — what removing a variable-cost bottleneck looks
like. **ON by default** (`voxel.March.IndexDeltaUpload 0` reverts).

**Against the owner's floor:** the handoff measured 968–2,435 chunks/s. This is **4,392
mean, 7,501 peak** — the peak clears the 6,200 floor; the mean is at 70% of it.

**Its verify gate crashes the D3D12 RHI** (`Fence->SyncPoints[GPUIndex] == nullptr` —
the single readback is re-enqueued while its previous fence is outstanding, and flushes
come 15,000+ per flight). **So the delta path is running unverified cell-by-cell.**
Closing that is the next correctness job.

### 2.6 GPU pass count is NOT the bottleneck — measured twice, both default-off

Two independent pieces batched GPU passes, both work mechanically, and **neither moved
throughput**:

| | passes | cross-check | frames vs its control | brickPacks |
|---|---|---|---|---|
| B.1 worldgen batching (`-VoxelGpuWorldGenBatch=1`) | 3,010 vs 10,335 (**3.4x**) | 207 ok / 0 FAIL | 8,429 vs 8,786 | 488,408 vs 562,898 |
| Pool-flush batching (`-VoxelGpuBrickFlushBatch=1`) | 698 vs ~1,758 (**2.5x**) | 263 ok / 0 FAIL | 12,237 vs 12,355 | 765,436 vs 768,799 |

Both stay off. **They are not failures — they are the measurement that located the real
ceiling**, which turned out to be §2.5. Reach for them again only if a future change makes
pass setup bind.

---

---

## 3. Phase 1 (the no-hole invariant) — built, measured, and NOT worth enabling yet

All three parts are in behind `-VoxelHierarchicalCoverage` and `voxel.March.Fallthrough`
(both default off; the control arm is byte-identical).

| | frames | p50 | tracked | holes | brickPacks |
|---|---|---|---|---|---|
| grid cache only | 10,321 | 21.89 ms | 126,320 | **0** | 623,804 |
| + hierarchical, fallthrough 0 | 5,957 | 39.94 ms | 187,676 | 2,368 | 546,536 |
| + hierarchical, fallthrough 1 | 6,490 | 41.64 ms | 196,073 | 2,356 | 577,829 |

**Why it loses, and it is not a bug in the implementation.** Full hierarchical coverage
costs **+49% resident chunks** (the design estimate was +24%). The pipeline cannot fill
that, so the coverage verifier starts reporting real holes — and, decisively, **there is
no coarse stand-in resident to fall through TO**, which is why turning fallthrough on
changes almost nothing (holes 2,368 → 2,356).

**Phase 1 is a throughput consumer, not a throughput fix. It needs Tier B first.** That
inverts the last handoff's ordering, and the table above is the reason.

---

## 4. Authored, merged, default-off, NOT yet measured

| Work | Switch | State |
|---|---|---|
| Asset resolve off the game thread (B.3) | `-VoxelAsyncAssetResolve` | compiles, unmeasured. **Inert without `-VoxelAssetDir`** — a leg without it measures an empty branch |
| Cold-band re-queue churn (~13,000 defers per 5 s) | `-VoxelColdBandDeferPark=1` | compiles, unmeasured |
| Marcher fallthrough depth 2 | `voxel.March.Fallthrough 2` | compiles, unmeasured |
| Batched pool flush (B.1's follow-up: the ~4 per-chunk flush passes fused per stack) | `voxel.GPU.BrickFlushBatch` / `-VoxelGpuBrickFlushBatch=1` | authored, **NOT yet compiled**, unmeasured. **Inert without `-VoxelGpuWorldGenBatch=1`** — without B.1's shared scratch every group is a singleton and falls back per chunk (counted). Read the `[brick-flushbatch]` window line; `xcheck ... FAIL` must stay 0 and `samples` must be non-zero |

---

## 5. Next, in the order the measurements now justify

1. **Close the index-delta verify gate** (§2.5). The delta path is the default and is
   unverified cell-by-cell. It needs the multi-slot readback ring the hole-stats and
   shadow-march paths already use, plus a skip when no slot is free.
2. **Find the new ceiling.** Pass count is out (§2.6) and the index copy is gone. Take a
   fresh `voxel.Stream.FrameAttribution` leg rather than assuming — every assumption about
   this pipeline that was not re-measured today turned out to be wrong.
3. **A hole metric that cannot count the sky** — authored, unmerged (`voxel.March.HoleStats`).
   The naive "ray exited with no hit" counts every sky ray. `substituted` (a hit from a
   level coarser than the segment owning that ground — counts HITS, so sky cannot
   contaminate it) and `uncovered` (no hit + crossed an ABSENT chunk + pointing below the
   horizon). Certify it can move BOTH ways before gating on it: `-VoxelMaxRingLevel=0`
   must make `uncovered` large, a settled stationary world must make it near zero.
4. **Re-test Phase 1** against the throughput it now has. It failed at 2,760 chunks/s
   because nothing coarse was resident to fall through to; at 4,392 that may change.
5. **Blue speckling** — a full diagnosis exists at
   `scratchpad/blue-speckling-diagnosis.md`. Its headline: no material or palette index is
   mip-averaged anywhere (the prime suspect is CLEARED), and in this renderer "a pixel that
   loses its sunlight" and "a pixel that turns blue" are the same event, so the geometry
   clues matter and the colour one does not. Cheapest discriminator: `voxel.March.ClimateStrength 4`
   stains every pixel the emit shaded and nothing else, halving the candidate space in one
   cvar. It also found, independently, that **GI never anchors under
   `voxel.Terrain.RetireQuads`** (`VoxelGI.cpp:1316-1332` returns on `NumQuads == 0`, so the
   volume never gets an origin) — almost certainly the answer to the standing "GI shows no
   visible difference" item.

## 6. Rules this session added (the earlier ones all still apply)

- **Never read a leg log with `grep ... | tail -1`.** The last window is the post-flight
  linger and is all zeros. It produced "the cache is enabled and doing nothing" and
  "dispatched=0" in the same session, both wrong, both retracted within minutes.
  **Use `tools/leg-summary.sh`.**
- **Do not run measurement legs while background agents are working.** Four agents running
  during a leg moved p50 by 6 ms and reversed the apparent sign of a comparison. Frame
  numbers taken under contention are not comparable to quiet ones.
- **Prefer a switch that reproduces the OLD behaviour over rebuilding the old binary.**
  `-VoxelInnerHysteresisChunks=-0.7071` put both arms of an A/B on one binary and removed
  the compile from the list of things a difference could be blamed on.
- **Run-to-run spread on this leg is large** — the same config measured 6,602 and 9,610
  frames. Treat n=1 as direction, never magnitude.
