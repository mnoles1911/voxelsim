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

### 3.1 The hole metric now exists, is certified, and measures the owner's complaint

> **SUPERSEDED 2026-08-23 by section 8.2 — "is certified" IS NO LONGER TRUE.**
> Both halves of the level+reason breakdown fail in steady state: `byLevel` is
> stuck at L0, and `byReason` collapses to `unattrib=100%` when the annotation
> writer freezes. Do not quote this instrument. See backlog 0.0i.

`voxel.March.HoleStats 1` (default off). `uncovered` = no hit anywhere + the ray crossed
an ABSENT chunk (not merely an empty one) + it points below the horizon. `substituted` =
a hit that came from a level coarser than the segment owning that ground, so it counts
HITS and the sky cannot contaminate it.

**Certified in both directions before being used** — a gate that cannot come out the other
way is worthless:

| condition | uncovered |
|---|---|
| settled, stationary | **0.0302% of rays** |
| `-VoxelMaxRingLevel=0` (ring 0 only) | **8.2479% of rays** |

273x separation. Then the number that matters:

| condition | uncovered | substituted |
|---|---|---|
| settled, stationary | 0.0302% | 0 |
| **flying at 30 m/s** | **3.9853%** | 0 |

**132x more holes in motion than at rest. That is the owner's complaint, in a number.**
The coverage verifier says `holes=0` in the same legs — it checks the DESIRED set is
resident, which is a different question from whether a ray found ground.

### 3.2 Phase 1 re-tested at the higher throughput — it substitutes, and still does not help

With the pipeline now 60% faster, hierarchical coverage plus fallthrough was re-run:

| | frames | p50 | p95 | brickPacks | uncovered | substituted |
|---|---|---|---|---|---|---|
| default | 11,949 | 21.55 ms | 32.69 ms | 752,707 | 3.9853% | 0 |
| + hierarchical + fallthrough 1 | 11,087 | 22.72 ms | 39.19 ms | 829,270 | **4.1363%** | **0.1129%** |

**Fallthrough is now demonstrably working** — 77,453 hits served by a coarser level, up
from exactly 0. And `uncovered` did not fall; it rose slightly. The substitution rate
(0.11% of hits) is two orders of magnitude too small to dent a 4% hole rate, because the
coarse levels are not resident at the leading edge either — the +49% residency demand is
still not met where it matters. **Stays off.** The next attempt should target residency at
the leading edge specifically, not total coverage.

---

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
3. **Drive `uncovered` down from 3.99% while flying** (§3.1). That is now the goal with a
   number on it. Phase 1 was the wrong lever (§3.2); the metric says the shortfall is
   residency at the LEADING EDGE, so target admission order and prefetch along the
   velocity vector rather than total coverage. `voxel.Stream.SpeculativeParkBricks 1`
   (§2.4) is the closest thing already in the tree and is worth re-measuring against
   `uncovered` specifically — it was judged on throughput, which was the wrong gate.
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

---

## 7. Where the pipeline bottlenecks now (measured after everything above)

Once the index copy was gone the ceiling moved twice more. Both were found with
instruments, not argument.

### 7.1 The in-flight "cap" was a per-tick BATCH QUOTA — and it was below the floor by construction

`MaxJobsInFlight = JobsInFlightPerCore x cores = 96`. With jobs now retiring in ~0.5 ms
inside a ~20.7 ms tick, "96 in flight" degenerates into "**96 dispatched per tick, then
the workers idle until the next tick tops them up**". That is `96 x 48.3 ticks/s =
4,637 chunks/s` — **below the owner's 6,200 floor as a matter of arithmetic**, no matter
how fast the workers get. Measured 4,341/s against that predicted 4,637/s ceiling.

The dispatch-loop counter now says it outright: at cap 96, `exitCap` dominates.

| config | chunks/s mean | peak | frames | p95 | hitches | holes |
|---|---|---|---|---|---|---|
| default (fork on, cap 8/core) | 4,341 | 7,516 | 12,355 | 31.74 ms | 543 | 0 |
| fork off, cap 8/core | 4,498 | 7,644 | 14,138 | 25.48 ms | **54** | 0 |
| **fork off, cap 24/core** | **6,763** | **9,811** | 12,103 | 31.99 ms | 511 | 6 |
| fork off, cap 48/core | 6,673 | 9,164 | 12,044 | 31.95 ms | 501 | 6 |

**6,763/s clears the floor.** 48/core is not better than 24 — that is the knee.

**Why nobody saw this:** the audit concluded the cap "never binds", correctly, *while the
GPU fork was on* — the fork's jobs inflated the blended counter so `CpuJobsOutstanding()`
read ~0 and the cap looked irrelevant. It was masked, not absent. The planned
`JobsInFlightPerCore` 8 -> 2 experiment would have made things worse; the right direction
was up.

### 7.2 The GPU fork is a net negative right now

It packs **6.5%** of chunks (`brickFromGpu=49,665` of 768,799) at `submit->deliver p50
2,281-2,348 ms`, holds per-ring floor slots for those seconds, and accounts for **90% of
all hitches** (543 -> 54 with it off). Its FIFO drains at `MeshBatchCap = 4` promotions
per tick, so Little's law caps the whole fork at ~111-144 chunks/s — about **2% of the
floor**. It cannot carry this pipeline in its current shape.

Do NOT raise `MeshBatchCap` — the measured hitch sweep behind it still stands. The fix is
either the queue-depth gate (`-VoxelGpuMeshQueueDepth=16`, in the tree, default off) or a
fork that costs less per job.

### 7.3 The current limiter: the queue runs dry

At cap 24/core the loop's exits invert — **`exitCap` 40%, `exitEmpty` 60%** across 11,744
passes. The dispatcher now outruns the producer of work. `pendingJobs` falls from ~1,630 to
~470 in the same window. **Admission, not dispatch, is next.**

Also confirmed and NOT fixed by the obvious lever: coarse rings still starve
(`R5 pending=245 dispCpu=1 floor=1`), and `voxel.Stream.RingFloorCpuOnly 1` does not move
throughput (6,589 -> 6,624/s, starved-ring lines 9 -> 12). Leave it off.

### 7.4 The finding that matters most, and it is not a throughput finding

Throughput rose **52%** and the hole rate did not fall:

| | chunks/s | uncovered (flying) |
|---|---|---|
| default | 4,341 | 3.99% of rays |
| cap 24, fork off | 6,589 | **4.97% of rays** |

**More chunks per second is not buying fewer holes.** That points away from raw rate and at
*which* chunks get streamed: admission ORDER and the leading edge, not admission VOLUME.
The next work should be priority along the velocity vector, not another throughput lever —
and `uncovered`, not `brickPacks/s`, is the gate it should be judged on.

### 7.5 The GPU fork: what it is, why it is off, and what is still unexplained

**What it is.** `-VoxelNoGpuMesh` disables it. It moves chunk MESHING off the CPU worker
pool onto `FVoxelGpuMeshJobManager`: the chunk is queued, promoted into a render-graph
dispatch, generated by shaders whose `worldgen.ush:ColumnMain` is a bit-exact mirror of
`Amplifier::column`, and delivered back. It changes only who PRODUCES geometry, never who
draws it. On by default since 2026-07-27, when it measured 32-38% faster than the CPU path.
**Tier B's whole premise is this path carrying everything**, so turning it off today is a
statement about its current shape, not about GPU meshing.

**Where its 2.3 seconds went — measured, and the stages sum exactly (residual 0.00 ms):**

| stage | p50 |
|---|---|
| **queued** (sitting in the FIFO) | **1,602-1,718 ms** |
| promoteToDispatch | 1.1 ms |
| dispatchToReady (the actual GPU work) | 27.6-29.1 ms |
| readyToDeliver | 54.9-59.7 ms |

**The GPU is not slow; its queue is.** ~28 ms of work behind ~1.6 s of waiting, because the
FIFO promotes 4 per tick (`MeshBatchCap`, whose raise is separately measured to hitch).

**The latency IS fixable, and fixing it did not save the fork.** `-VoxelGpuMeshQueueDepth=16`
routes overflow to the CPU arm; it cut submit->deliver 1,068 -> 147 ms (7x) and rerouted
18,847 chunks. All three arms, cap 24/core:

| | frames | p95 | hitches | brickPacks |
|---|---|---|---|---|
| **fork off** | **11,745** | **32.00 ms** | **474** | **1,009,737** |
| fork on | 10,832 | 41.46 ms | 1,257 | 913,197 |
| fork on + queue 16 | 10,716 | 42.42 ms | 1,283 | 919,117 |

**Off wins on every axis, and latency was not the reason.** Off also costs nothing: the CPU
arm absorbed 100% of the fork's work with the same total packs and the same coverage.

**UNEXPLAINED, and left that way rather than guessed at.** Why the fork still costs ~9% of
throughput and nearly triples hitches once its queue wait is gone. Candidates not yet
separated: per-chunk render-thread work on delivery, ring-floor slots still held through
the accounting, burst delivery hitching the apply step, or contention with the marcher for
the device. A tempting check -- comparing `renderMs` between arms -- **does not work**: that
field is sampled only on hitch frames (n equals the hitch count exactly), so its population
differs between arms by construction. Whoever picks this up needs a per-frame render-thread
sample, not the hitch line.

**This matters beyond the fork.** Tier B assumes moving MORE work onto the GPU is the way to
20,000-50,000 chunks/s. The only measurement anyone has of that direction says the opposite
at today's scale. Settle §7.5's unexplained cost BEFORE building more of Tier B on it.

---

## 8. Phase 2 landed (added later this session): GPU-written residency, and the verify gate un-broken

Per docs/gpu-streaming-architecture.md P2. Two deliverables, both **default off /
behaviour-preserving**; a control leg is byte-identical.

### 8.1 The publish path — `voxel.March.IndexGpuResident` (default 0)

A flushed chunk's index cell is written by a GPU kernel (`VoxelMarchIndexPublishMain`,
second entry point of VoxelMarchIndexScatter.usf) in a render command enqueued directly
behind the pool's brick writes, instead of the game thread snapshotting `[cell,value]`
pairs for the next marcher graph. The kernel derives the CELL with **the marcher's own
wrap function** — factored into `Shaders/VoxelMarchIndexCell.ush`, included by both the
read side (VoxelBrickTraverse.ush) and the write side — composes the value, and executes
the removal guard (clear only a cell still naming the retired slot) against the live GPU
buffer. Removals dispatch before additions; RDG's UAV barrier is the Removed-before-Added
rule.

**What the CPU still supplies, plainly:** the chunk coordinate and level (request data —
the CPU decides what to generate), the level→grid-slot mapping and cover-band admission
(index policy, kept where its counters live), and — until P1's suballocator lands — the
**pool slot**, which is `AllocateForChunk`'s answer carried in the entry. Entry dword 4 is
the P1 seam: it stops being uploaded and starts being read from the allocator's output.

The CPU shadow (`Cells`) is still maintained — it is the counters, the full-upload
fallback base, and the verify reference — but it is **off the residency path**: the
marcher finds the chunk resident without `Register()` staging anything.

Counters (log line "Voxel march index GPU publish"): publishes, cellsWritten,
evictionsCleared, fellbackPendingCpu (once per mid-flight ON-flip is normal),
lostNoBuffer (must stay 0; a full staging heals it, counted as `fullBecause lost`).

### 8.2 The verify gate — fixed, and it can FAIL

`voxel.March.IndexDeltaVerify` no longer crashes the D3D12 RHI. The single readback was
re-enqueued while its previous fence was outstanding because `IsReady()` is meaningless
between `AddEnqueueCopyPass` and graph execution (the fence is only re-armed at execution,
and `Register()` runs up to three times a frame). Now: a 2-slot readback ring with
per-slot expected hashes, an ArmedFrame gate (never poll a slot in the frame that armed
it), and a counted skip when the ring is full — the hole-stats / shadow-march pattern.

The gate is the Phase 2 correctness proof: after a sampled publish (or CPU delta scatter),
the whole GPU buffer is read back, FNV-hashed in shadow order, and compared to the hash of
the CPU shadow state it was patched to equal. **It fails if the GPU-derived cells disagree
with what the CPU would have written** — shared-wrap drift, a lost entry, a guard
mismatch — and a wrong cell is persistent divergence, so sampling suffices. Run it on any
leg that flips 8.1 on.

### 8.3 Not done / to measure (no build was run from this worktree)

- Compile + a PIE leg: `voxel.March.IndexGpuResident 1` + `voxel.March.IndexDeltaVerify 1`,
  read `verify pass/FAIL/skip` and the publish line. **FAIL>0 outranks everything.**
- The publish path still builds its entries on the game thread from the flush delta; the
  game-thread win is that staging/merge/Register-consume drop out, but the real payoff is
  architectural — P1's allocator plugs into entry dword 4 and the apply loop can then
  shrink to shadow bookkeeping.
- Holes gate: `uncovered` must not move with the switch on (admission order is untouched,
  so it should not — verify, don't assume).


---

## 8. Measured 2026-08-23, late session

### 8.1 The recompute split — the number the whole plan was gated on

Phase 1 landed (timers split, sums added beside the maxima, per-level radii
hoisted out of the per-record and per-cell inner loops) and the leg
`Saved/phase1-split.log` was run against it: flight `line`, spawn
`-61440,-61440`, one binary.

Steady-state windows, 5 s each:

| stage | ms/window | share |
| --- | --- | --- |
| **admission** (`entryMs` summed over rings) | 116-186 | **~65%** |
| **eviction walk** (`exitScanMs`, now isolated) | 45-78 | **~30%** |
| sort | 2-8 | ~2% |
| fine-tier residency tick | 1-8 | ~1% |
| queue filter | **0.0** | 0% |

**The parts sum to `totalMs` exactly.** e.g. 68.1 + 2.1 + 4.1 + 116.1 = 190.4,
and it closes like that in every window. The residual bucket -- prologue, cutoff
relaxation, `PruneFootprintZRangeCache`, `FlushAbsentMarks` -- is effectively
zero, so nothing is hiding outside the named stages.

**Two earlier claims in this document are corrected by this.**
  1. "The bottleneck is the eviction pass walking 233,838 records" was
     premature: the exit scan is **a third**, not the bulk. Admission is roughly
     **twice** it.
  2. The `queueFilterMs` that used to be bundled inside `exitScanMs` costs
     **literally nothing**. It was part of what made the old combined number
     unreadable.

**Consequence for the plan:** Phase 2 (incremental admission) is correctly
prioritised, and Phase 3 (bucketed eviction) remains worth doing at ~30% rather
than being noise.

### 8.2 The uncovered level+reason instrument fails both halves

Three headless captures at 120 / 400 / 1200 m, read with `tools/read-holes.sh`.
`byLevel` reads `L0=<everything> L1..L5=0` in every window of every rung.
`byReason` starts honest (`never=100%`) and then collapses to `unattrib=100%`
for the whole steady state -- and `annotWrites pending` **freezes** at 290,332 at
exactly that moment. The annotation writer stops; the reason bits go stale.

The capture side is sound: `attributed == uncovered` and
`sum(byLevel) == sum(byReason)` hold in every window including the bad ones. The
fault is isolated to annotation. Full entry: backlog 0.0i.

### 8.3 Admission is rejecting candidates by the hundred thousand

Same leg, `job flow` across the last six windows -- `candidatesRejected` against
`dispatched`:

```
    6,438 / 16,227      61,933 / 13,420     268,484 /  9,883
  471,884 /  9,845     595,763 /  9,512    748,677 /  9,646
```

Three-quarters of a million candidates rejected per 5 s window while dispatch
falls ~40%. Unexplained as of this writing and under investigation; it is
plausibly the same mechanism behind the owner's loading-order complaint (8.4).

Note `chunksPerSec` reads 0.0 on this leg and is **not** a throughput figure
here: `zeroQuad == drained`, i.e. every chunk meshes to zero quads by
configuration in the marcher build. Use `dispatched` per window.

### 8.4 Owner-reported: chunks load left-to-right, and far before near

> "the chunks always load from left to right in relation to the player character
> camera. and it does not always prioritize the nearest chunks to player camera.
> rather sometimes far chunk areas load left to right in the far distance rather
> than the clearly higher priority... chunk areas closer to player at LOD0."

The same leg corroborates it. `entryMs` marches ring by ring, one level at a
time, in level order:

```
R0=185.0 R1=45.7  R2=45.0  R3=89.5  R4=232.7 R5=661.4   (cold)
R0=339.2 R1=56.9  R2=6.0   R3=0     R4=0     R5=0
R0=0     R1=154.0 R2=37.9  R3=6.2   R4=0     R5=0
R0=0     R1=0     R2=159.9 R3=16.1  R4=5.8   R5=0
R0=0     R1=0     R2=0     R3=169.0 R4=14.4  R5=0
R0=0     R1=0     R2=0     R3=0     R4=127.3 R5=0
```

That is **level-primary ordering** (the "far before near" half); a row-major
grid sweep within a level is the "left to right" half. **This is a hypothesis
fitted to a plot and must be confirmed against the code before being acted on.**

Owner also wants the cascade extended from 4 km to **4-8 km**.

**THE CONSTRAINT ANY 8 km PROPOSAL MUST CLEAR.** The marcher chunk index is a
toroidal grid, `kDimXY = kDimZ = 128` per level, wrapping with `& (kDim-1)` and
carrying **no origin**. Its correctness is a compile-time proof that a level's
resident span is `< 128` chunks. At L5 today: chunk edge 102.4 m, span
2 x 4096 = 8192 m, so **80 chunks**. Widening ring 5 to an 8 km outer radius
gives 16384 / 102.4 = **160 chunks > 128 and the index ALIASES** -- the file's
own words for the symptom are "one chunk silently shadowing another -- turned
into a hole by the marcher's record validation, but a hole nobody ordered".
That trap has already been live and silent in this file once, because the assert
meant to catch it was a tautology.

The clean route to 8 km is **an added ring level 6** (chunk edge 204.8 m ->
16384 / 204.8 = 80 chunks, the same safe number), not a wider ring 5. Knock-on
constraints to check: `kNumLevels`, the index's `kLevels`/`kRingGrids`, and
`kCoverLevel`, which must stay `< 8` to fit the VisBuffer's three-bit level
field and the record's four-bit `LevelAndFlags`.

### 8.5 A build guard that earned its keep

`tools/voxel-capture.ps1` refused to launch: `voxelcore.lib` was stale against
`mips.h`. **UBT does not track voxel-core's sources**, so `Build.bat` reported
"Result: Succeeded" while linking the previous day's library. Without that
refusal the coarse-mip A/B pair would have been shot against an out-of-date
generator, shown no difference, and been reported as an ineffective fix -- with
a green build and clean logs throughout.

The full sequence after any voxel-core header change is:

```
cmake --build build/voxel-core-msvc --config Release
# then FORCE A RELINK -- rebuilding the lib alone does not trigger one:
touch a file under ue-project/Source/VoxelEarth && Build.bat ...
```
