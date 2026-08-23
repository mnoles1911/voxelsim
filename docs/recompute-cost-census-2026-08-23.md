# What recompute actually spends — the census

2026-08-23. Asked for: named buckets that reconcile to the printed
`recompute=` total, with the reconciliation delta printed, failing readings
stated both ways, and **no fixes in this pass**.

The first half of the answer needed no new code. The stage split **already
existed** and had never been aggregated.

---

## 1. The stage split, from logs already on disk

`VoxelWorldSubsystem.cpp:10821` prints `Voxel recompute (sum since last log):
totalMs= fineMs= exitScanMs= queueFilterMs= sortMs= | entryMs R0..R6`. Run
`python tools/recompute-census.py Saved/<leg>.log` over every **active** window
(a window with `totalMs = 0` is linger, not a measurement):

| leg | windows | ms/window | entryScan | exitScan | sort | fine | queueFilter | **RESIDUAL** |
|---|---|---|---|---|---|---|---|---|
| `ahead-on` | 73 | 709.2 | **61.1%** | **37.3%** | 1.3% | 0.2% | 0.1% | **0.06%** |
| `gp-ctl2` | 84 | 811.7 | **53.1%** | **45.2%** | 1.4% | 0.2% | 0.1% | **0.06%** |
| `pool-pri` | 82 | 965.7 | **58.7%** | **39.1%** | 1.9% | 0.2% | 0.1% | **0.05%** |

Per ring on `ahead-on`: R0 33.7%, R1 10.2%, R2 6.2%, R3 4.1%, R4 3.3%,
R5 3.7%.

**Three conclusions, and they close the question at this level.**

**(a) There is no hidden stage.** The existing buckets account for **99.94%**
of recompute. The residual is 31.7 ms out of 51,773 and its per-window range is
`-0.10 .. 4.80 ms`. Every plan that assumed unattributed cost inside recompute
was assuming something that is not there.

**(b) Two stages are the whole thing.** entry + exit = **90-99%**. `sort` is
1.3-1.9%, `fineResidency` 0.2%, `queueFilter` 0.1% — `queueFilter` is 29 ms
across an entire leg. Nothing outside entry and exit is worth a line of code.

**(c) The rise through the run is the EXIT half.** By quartile on `ahead-on`:

| | Q1 | Q2 | Q3 | Q4 | growth |
|---|---|---|---|---|---|
| total ms/window | 464.7 | 692.5 | 787.5 | 882.5 | 1.90x |
| entryScan | 354.5 | 405.0 | 457.4 | 513.1 | **1.45x** |
| exitScan | **100.8** | 279.2 | 318.8 | **354.9** | **3.52x** |

The exit walk is `O(tracked records)` and records climb monotonically through a
fill; the entry scan is `O(Span²)` per level and only grows as more rings come
active. So "recompute rises through the run" is, specifically, **the
`ChunkRecords` walk growing with world size** — a term that scales with neither
span nor throughput.

That matters for attribution of work already done. `-VoxelGpuResidency=2`'s
exit half **already deletes this stage**: measured `ms ev=0.02` per 5 s window
(`Saved/t42-live.log`) against 19-31 seconds of game thread per leg here. The
exit half of mode 2 is worth **37-45% of recompute and all of its growth**, and
it works today. It is the admit half that was 2.4x negative, which is what
`-VoxelResidencyAdmitBudget` addresses.

### Hook F — print the residual. One format token, one argument.

`VoxelWorldSubsystem.cpp:10821`. The comment above that line already says the
residual "is deliberately unbucketed… if the residual ever turns large, one of
those has stopped being cheap and deserves a bucket of its own" — but the line
never prints it, so nobody can see whether it has.

```cpp
	       TEXT("Voxel recompute (sum since last log): totalMs=%.1f fineMs=%.1f exitScanMs=%.1f queueFilterMs=%.1f sortMs=%.1f residualMs=%.1f | ")
	       TEXT("entryMs %s"),
	       AccumRecomputeMs, AccumFineResidencyMs, AccumExitScanMs, AccumQueueFilterMs, AccumSortMs,
	       AccumRecomputeMs - AccumFineResidencyMs - AccumExitScanMs - AccumQueueFilterMs -
	           AccumSortMs - AccumLevelEntryMsTotal(),   // signed: negative means timers OVERLAP
	       *JoinPerLevel(...));
```

where `AccumLevelEntryMsTotal()` sums `AccumLevelEntryMs[0..kNumLevels-1]`.
**Keep it signed.** Positive means a stage is unbucketed; negative means two
timers overlap and every share is double-counted. An absolute value would hide
the second, which is the harder fault — `tools/test-voxel-ring-order.cpp` P2
asserts exactly this, and it **fails** when `ResidualMs()` is made absolute.

---

## 2. What is still unknown, and the instrument for it

Inside `entryMs[L]` there is **no split at all**. The cell body does geometry,
then a Z-range memo lookup, then a Z loop with two hash probes per Z cell, then
admission. The four candidate fixes each pay off against a different one of
those, and nobody knows the shares.

New file: `ue-project/Source/VoxelEarth/VoxelRecomputeProfile.h` (UE-free, so
its arithmetic is exercised by the standalone test).

### Why it counts instead of timing

The Z loop runs **~2.4 million times per 5 s window**. `FPlatformTime::Seconds`
is ~20-25 ns, so a pair around each Z cell adds **~110 ms to a 709 ms
measurement** — a 15% perturbation of the thing being measured. So the
loop-internal work is **counted** (one increment on an int64 already in cache,
free) and the per-operation costs are recovered by **least squares** over levels
and windows: six rings have very different mixes and a leg supplies ~500
`(entryMs, counts)` observations. `tools/recompute-census.py` pass 2 does the
fit.

**The fit's residual is the reconciliation delta and it is printed.** A model
that cannot explain `entryMs` from the counters shows up as a large residual
instead of producing plausible-looking shares. A **negative fitted cost** is
reported as a failure too — an operation cannot take negative time, so it means
the counters are collinear and the split is not identifiable from that leg. In
either case the tool says *do not read the shares*.

The **one** timer inside the sweep is `MemoFillMs`, and it is safe on its own
terms: a fill is a memoized amplifier column — rare once a level is warm, and
expensive. `memoHit`/`memoFill` print beside it, so "is it still rare" is
answerable; **`memoFill` approaching `memoHit` is the reading that says the
timer must come out.**

### Failing readings, both ways, at every site

Every stage carries a **call count** beside its milliseconds. `exitScan=0.00ms
n=0` (never ran) and `exitScan=0.00ms n=250` (ran and is genuinely free) are
different facts that print identically without it — three lanes in this
codebase have already been found inert exactly that way tonight.

| reading | means |
|---|---|
| residual > 5% of total | a stage is missing a bucket, or work is accumulated outside its timer |
| residual **negative** | timers **overlap**; every share is double-counted. Only visible because it is signed. |
| any stage `n=0` in a window where recompute ran | that stage never ran — nothing about it may be concluded from this leg |
| `cells=0` with `entryMs>0` | **the counters are outside the loop they describe.** The twelve-instruments-outside-the-path failure, exactly. |
| `zCells/cells` ≈ 1 | the Z range has stopped being a range; the per-cell arithmetic is about a different world than the leg |
| `memoFill` approaching `memoHit` | the memo is not memoizing; the fill timer is now itself a cost |
| fit residual > 10% of `entryMs`, or any negative cost | the model failed. The shares are not a breakdown. |

### Verified tonight, no build, no editor

`bash tools/run-voxel-ring-order-test.sh` — P1 reconciliation exact to the
millisecond, P2 the signed residual, P3 never-ran vs ran-and-free. **All three
fail when mutated**: making `ResidualMs()` absolute and making `StageNeverRan()`
test milliseconds instead of the call count produces

```
FAIL  P2 overlapping timers produce a NEGATIVE residual
FAIL  P2 the overlap is the residual magnitude
FAIL  P3 a free stage is not a never-ran stage
```

and restoring returns PASS.

`python tools/recompute-census.py Saved/ahead-on.log` runs on today's logs and
produced §1 above. Pass 2 on a leg without the switch prints *"the leg ran
without `-VoxelRecomputeCensus`, so the per-operation split is unavailable —
that is the 'switch was off' reading, not 'the costs are zero'."*

### Hook G — `-VoxelRecomputeCensus`, off = byte-identical

Include `"VoxelRecomputeProfile.h"`; add
`VoxelRecomputeProfile::FWindow RecomputeProfile;` beside `AccumLevelEntryMs`
(`:7649`) and a `VoxelRecomputeProfile::FEntryCounters LevelCensus[kNumLevels];`
scratch. All increments guard on
`VoxelStreamAdmission::RecomputeCensusEnabled()` (command-line-latched, for
`-VoxelPendingJobCap`'s reason). Register `VoxelRecomputeCensus` in
`tools/frontend-switch-classification.txt`.

| # | site | insert |
|---|---|---|
| G1 | the cell loop, `:16347` (`for (int32 Cy = AnchorChunk.Y - ScanSpan; …)`) | `++C.CellsVisited` at the top of the body; `++C.IncrSkipped` on the incremental-skip `continue`; `++C.GeoRejected` on each geometric `continue` (inner pad, outer edge, seam-parent) |
| G2 | `FootprintChunkZRangeCached`, `:13611` | `++C.MemoHit` on the memo-hit return; `++C.MemoFill` plus an `FPlatformTime::Seconds` pair around the compute path into `C.MemoFillMs` |
| G3 | `EnumerateSurfaceFootprintCandidates`, `:6756` | `++C.ZCells` per Z iteration; `++C.RecordProbes` / `++C.ParkProbes` at the two `Find` calls; `++C.Admits` on commit |
| G4 | the exit walk (`ThisFrameExitScanMs` bracket) | `++E.RecordsWalked` per record; `++E.VerticalTests` on the deep test; `++E.EvictsQueued` per queued unload |
| G5 | `MaybeLogCounters`, beside the line at `:10821` | the census line below, then `RecomputeProfile.Reset()` |

G5's line — the field order is what `tools/recompute-census.py`'s `CENSUS`
regex parses, so **change them together or the tool silently matches nothing**
(which it reports as "switch was off", the one wrong answer it can give):

```
Voxel recompute entry census (5s window): R0[ms=%.1f cells=%lld incrSkip=%lld geoRej=%lld
memoHit=%lld memoFill=%lld memoFillMs=%.1f zCells=%lld recProbe=%lld parkProbe=%lld
admit=%lld defer=%lld] | R1[...] ... | exit[records=%lld vert=%lld evict=%lld]
| stages %s | residualMs=%.1f
```

`ms=` per level is the existing `AccumLevelEntryMs[L]`, so the fit is against
the timer that already exists rather than a new one.

**Do not fix anything from this pass until the fit has run once.** §1 is
settled from logs; §2 is an instrument, and its own failing readings are the
first thing to check on the leg that carries it.

---

## 3. Where this leaves the picture

| stage | share | growth over a run | status |
|---|---|---|---|
| entry scan | 53-61% | 1.45x | `-VoxelRingOrderScan` (11.3-19.1x fewer cells) + `-VoxelResidencyAdmitBudget`; **split inside it still unknown — hook G** |
| exit walk | 37-45% | **3.52x** | mode 2 already deletes it (`ms ev=0.02`); needs the admit half fixed to be net-positive |
| sort + fine + queueFilter | 1.6% | — | not worth touching |
| residual | **0.06%** | — | nothing there |

Recompute was never what capped chunks/s — the apply cap
(`MaxAppliesPerFrame = 192`, ending 65-76% of ticks) is. But recompute is the
largest game-thread item post-fix and it grows, and this says exactly which
half grows and why.
