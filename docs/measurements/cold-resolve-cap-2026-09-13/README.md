# Capping cold resolves per tick halves the worst frames' FREQUENCY, and costs the middle

Built tonight after the cold-resolve diagnosis
(`docs/measurements/submit-cold-resolve-2026-09-13/`) showed that halving cold
misses by warming ahead left frames over 300 ms untouched — a resolve first asked
for in the frame it is needed cannot be predicted, only bounded.

`voxel.Stream.ColdResolveCapPerTick`, **default 0 (off)**. At most N cold asset
resolves per streaming tick; the excess waits a tick, and only where a coarser
ancestor is resident and holds terrain. Same hold, same requeue, same hole-safety
argument as the cold-*shading* cap this is modelled on.

## It engages, and it never had to force one through

    Voxel cold resolve cap (window): cap=2 deferred=8993 exempt=0
    Voxel cold resolve cap (window): cap=2 deferred=2913 exempt=0

`exempt=0` across the capture: every deferral had a coarser ancestor covering the
ground, so nothing was pushed through against the hole rule. The cap is fully
effective and the guarantee never had to be spent.

## What it is worth, pooled over runs

| | defaults (3 runs) | cap=2 (2 runs) |
|---|---|---|
| frame p50 | 14.23 | 14.14 |
| frame p95 | 20.21 | **22.54** |
| frame p99 | 102.41 | 87.62 |
| frame max | 729.02 | **924.69** |
| over 33.3 ms | 2.47% | **2.92%** |
| over 100 ms | 1.08% | 0.89% |
| **over 300 ms** | 0.46% | **0.20%** |

**The honest reading is a trade.** Frames over 300 ms roughly halve, and that is
the band a player feels as a freeze. But the **worst single frame did not
improve** — the two armed runs read max 528.68 and 924.69 against the defaults'
729.02, so the claim "the cap removes the worst frame" is refuted by its own
repeat. And the middle band pays: p95 rises 2.3 ms and frames over 33 ms go from
2.47% to 2.92%, which is deferred work arriving later rather than never.

## THE VERDICT THAT COUNTS: a MOVING pair, and the owner sees no difference

The settled route pictures below were **not a real gate and should not be cited
as one**. The route harness stops the pawn at each stand and waits 2 seconds
before the shutter; deferred work catches up in a frame or two, so every arm had
converged on the same image long before the picture was taken. That test could
not have failed.

The gate is a capture taken **while moving**, which this project's own limitation
note said it could not take. `tools/voxel-moving-capture.ps1` does, and the pair
is: 20 m/s, forest spawn, no settle, same ground to **0.28 m**, same pose, one arm
cap off and one at cap 8.

**The owner's words, on the moving pair: "Both images look identical."**

So the trade this mechanism makes is invisible under the condition designed to
expose it, and the shipped default stands on evidence that could have failed.

## The owner's earlier verdict on the settled pictures (kept for the record)

Three route captures at stand 00 of the authored detour — the same cap-off
configuration **twice**, so the harness's own 17.65%-of-pixels noise was visible
rather than described, then the arm at cap 8. The owner's words: **"3 looks no
worse than 1 or 2"**.

`voxel.Stream.ColdResolveCapPerTick` therefore defaults to **8**. What the verdict
covers is exactly the configuration in those pictures; the millisecond-budget
variant defers more (4,908 against 3,125 deferrals in a window) and was shot
separately for its own verdict rather than assumed to be covered.

The section below is kept as written, because it is the reasoning that stood
before the pictures existed and it is what the verdict answered.

## Why it was not on by default

Two reasons, and neither is timing:

1. **It is a visual trade.** Deferring a submit means coarser ground is drawn for
   longer — a bounded mip-pop, by construction, but a visible one. Under the
   settings-panel policy that is the owner's verdict on pictures, not a number I
   can settle. The `substituted=` counter that would quantify it needs
   `voxel.Stream.CoverageVerify`, which the walk harness does not enable, so the
   artifact for that decision is a pose-matched route pair rather than a log.
2. **The benefit is real but modest**, and it is bought with a worse p95. A cap
   of 2 was the first value tried; 4 or 8 may hold the >300 ms win with less of
   the middle-band cost, and that sweep has not been run.

## The sweep, run after the above was written

Caps 4 and 8, one run each, against the same pooled baselines:

| | off | cap 2 | cap 4 | cap 8 |
|---|---|---|---|---|
| frame p50 | 14.23 | 14.14 | 14.08 | 14.17 |
| frame p95 | 20.21 | 22.54 | 20.66 | 21.18 |
| frame p99 | 102.41 | 87.62 | 115.73 | 118.35 |
| **frame max** | 729.02 | 924.69 | 486.75 | **381.87** |
| over 33.3 ms | 2.47% | 2.92% | 3.28% | 3.02% |
| over 100 ms | 1.08% | 0.89% | 1.07% | 1.14% |
| **over 300 ms** | 0.46% | 0.20% | **0.13%** | **0.13%** |

**The worst frame falls as the cap RISES** — 729 off, 487 at 4, 382 at 8 — which
is the opposite of the obvious reading and worth stating plainly. A tight cap
defers so much that the deferred work piles into a later tick; a looser one bleeds
the burst off without building a queue behind it. Cap 8 gives the lowest worst
frame measured anywhere tonight.

**And p99 goes the other way**: 102 off against 115–118 at caps 4 and 8. The dial
is not "better or worse", it is **worst-frame against how often a frame lands in
the 33–100 ms band**, and the two ends want opposite settings.

`exempt=` is 0 at cap 2 but 1,949 and 1,980 at caps 4 and 8 — at the looser caps
some deferrals had no coarser ancestor and were pushed through, which is the hole
guarantee spending itself as designed and being counted where it happens.

Caps 4 and 8 are single runs; the max column especially should be repeated before
anyone quotes it, since cap 2's own two runs disagreed by 400 ms on exactly that
statistic.

## The pictures, and why the pixel statistic cannot judge them

Three route captures at the eight authored stands: cap off, cap off AGAIN, and
cap 8. The second control is the whole point — without it the armed numbers mean
nothing.

| stand | floor (off vs off) | arm (off vs cap 8) |
|---|---|---|
| 00 | 17.65% | 60.18% |
| 01 | 16.70% | 47.28% |
| 02 | 19.13% | 31.27% |
| 03 | 42.57% | 72.03% |
| 04 | 45.36% | 61.67% |
| 05 | 40.68% | 54.32% |
| 06 | 43.11% | 49.45% |
| 07 | 29.84% | 45.15% |

**The floor is 17–45% of pixels between two runs of the SAME configuration.** A
route capture stops at a waypoint after a settle, and arrival position, streaming
order and temporal accumulation all differ between runs; this is the large floor
that moving captures on this project are known to have. The armed pair is higher
at every stand, but at stand 06 it is 49.45% against a 43.11% floor, which is not
a difference anyone can read as a defect.

**So the pixel statistic detects that something differs and cannot say what.**
The artifact for this decision is the pictures themselves, judged by the owner,
with the two controls shown alongside so the floor is visible rather than
described. Sent 2026-09-13.

## At speed, which is the condition the target names, the trade is better

Same authored route at tier 7 (9.5 m/s sustained), cap off against cap 8:

| | cap off | cap 8 |
|---|---|---|
| frame p50 | 20.61 | **18.33** |
| frame p95 | 168.73 | 160.67 |
| frame p99 | 392.42 | **284.69** |
| frame max | 1655.40 | 1626.57 |
| over 33.3 ms | 20.95% | 20.31% |
| over 100 ms | 7.98% | **10.33%** |
| **over 300 ms** | 2.12% | **0.67%** |

`deferred=31366 exempt=1794` — 5.4% of deferrals had no coarser ancestor and were
pushed through, the hole guarantee spending itself.

**The same shape as at a walk, but larger in both directions.** Frames over 300 ms
fall by 68% and p99 by 27%, and even the median improves 11%; frames over 100 ms
rise from 7.98% to 10.33%. The faster the player moves the more cold resolves
arrive per second, so both sides of the trade grow — and the side that grows more
is the one being bought.

That matters for the decision: this cap is worth most exactly where the slice is
furthest from its target, and worth least standing still.

## A count is the wrong bound, and a millisecond budget is better but not sufficient

With the count cap at 8 and the pawn at 9.5 m/s, the 100–300 ms GAME-THREAD band
still reads `SubAssetsMs` **87.12 ms** of a 144.23 ms game thread. That is the
count cap failing to bind: a cold coarse resolve is 28.91 ms and a level-0 one
3.77, so "8 per tick" is anything between 30 and 231 ms.

`voxel.Stream.ColdResolveBudgetMsPerTick` bounds the same thing in milliseconds.
At 9.5 m/s, all three arms on the same route:

| | cap off | count cap 8 | **budget 20 ms** |
|---|---|---|---|
| frame p50 | 20.61 | 18.33 | 20.22 |
| frame p95 | 168.73 | 160.67 | **148.82** |
| frame p99 | 392.42 | 284.69 | **261.00** |
| frame max | 1655.40 | 1626.57 | 1742.37 |
| over 33.3 ms | 20.95% | 20.31% | **24.35%** |
| over 100 ms | 7.98% | **10.33%** | 8.24% |
| over 300 ms | 2.12% | **0.67%** | 0.98% |
| SubAssetsMs in the 100–300 ms band | 89.21 | 87.12 | **79.94** |

**The budget wins p95, p99 and the over-100 ms rate; the count cap wins over-300
ms.** They bound different things, which is what the sweep already suggested.

**And neither bounds the band to its nominal figure.** A 20 ms budget should give
roughly "budget + one resolve" — about 49 ms — and the band still reads 79.94.

My first explanation was that the budget resets per streaming TICK while the band
is per FRAME, so a frame with several ticks pays it several times. **That is
refuted by the counters**: the tick-budget window reads `ticks=121` where the same
window holds roughly 250 frames, so the streaming tick runs about once every two
frames. No frame pays the budget twice.

**The real reason is the "+ one resolve" term, and it is unbounded.** The budget
can only defer a resolve it has not done yet; the first cold resolve of a tick
always goes through, and a single coarse resolve was measured at up to 526 ms.
20 ms of budget plus one large resolve is exactly the ~80 ms the band shows.

So bounding the tail properly needs the cost charged **before** the work, from
something knowable in advance. The level is a usable proxy — coarse resolves
measured 28.91 ms against level 0's 3.77 — so a submit whose ESTIMATED cost
exceeds the remaining budget can be deferred without first paying it. That is the
next version of this mechanism, and it is the one that could actually bound a
frame.

## Charging the estimate up front, which is the version that works

`voxel.Stream.ColdResolveChargeEstimate` (default true, only active with a budget
set) charges a cold resolve's estimated cost against the budget **before** paying
it, using the level as the proxy — the measured 3.77 ms for level 0 and 28.91 for
coarse. A wrong estimate costs a deferral, never a hole: the coverage test is
untouched.

All four arms, same route at 9.5 m/s:

| | off | count 8 | budget, measured | **budget, estimate** |
|---|---|---|---|---|
| frame p50 | 20.61 | 18.33 | 20.22 | **17.29** |
| frame p95 | 168.73 | 160.67 | 148.82 | **125.97** |
| frame p99 | 392.42 | 284.69 | **261.00** | 294.11 |
| over 33.3 ms | 20.95% | 20.31% | 24.35% | **19.45%** |
| over 100 ms | 7.98% | 10.33% | 8.24% | **6.16%** |
| over 300 ms | 2.12% | **0.67%** | 0.98% | 0.96% |

**It wins the median, p95, and both of the rates that had been getting worse.**
p50 −16%, p95 −25%, frames over 100 ms −23%, all against the unbounded arm, and
unlike every earlier variant it does not pay for the tail with the middle band —
over-33 ms is better than doing nothing at all.

The count cap still holds the best over-300 ms figure, and the measured budget the
best p99. So the ordering depends on which statistic the owner is buying, and all
three are one cvar apart.

**What is still not bounded:** the 100–300 ms band's `SubAssetsMs` is 78.53 ms,
barely moved from 79.94. The estimate charges 28.91 ms for every coarse level, but
a level-7 footprint is vastly wider than a level-1 one, so the estimate is too low
where it matters most. A per-level estimate — the measurement already splits misses
by level — is the obvious next refinement.

## The resolve cost per level, measured — and why the accurate model is worse

`assets` is the streaming cost at EVERY speed, not just at a walk. The 20 m/s
flight leg's TAIL bucket reads tick 1805 ms → dispatch 1662 → submit 1657 →
**assets 1594 (96%)**, with raster 20.3. It is not admission, not tile IO, not the
mesher.

Per-level cost, 3,975 misses at 9.5 m/s with the cap off — "coarse" had been one
bucket averaging levels 1–7, which is a number about the mix rather than a cost:

| level | misses | total ms | ms each |
|---|---|---|---|
| 0 | 428 | 2,616 | 6.11 |
| 1 | 1,045 | 8,866 | 8.48 |
| 2 | 382 | 2,395 | 6.27 |
| 3 | 430 | 3,110 | 7.23 |
| 4 | 97 | 1,038 | 10.70 |
| 5 | 70 | 1,122 | 16.03 |
| **6** | 651 | 28,101 | **43.17** |
| **7** | 872 | 94,367 | **108.22** |

**Levels 6 and 7 are 38% of the misses and 86.5% of the milliseconds.** A single
level-7 resolve is 108 ms — more than any sane tick budget on its own.

**And putting that table into the estimate made the result WORSE:**

| | flat 28.91 | per-level table |
|---|---|---|
| p50 | **17.29** | 20.14 |
| p95 | **125.97** | 158.42 |
| p99 | 294.11 | **274.69** |
| over 100 ms | **6.16%** | 7.72% |
| over 300 ms | 0.96% | **0.83%** |

**The budget's job is to bound a tick, not to be accurate.** A flat 28.91
over-charges levels 0–3 by about 4x, and that over-charge is a throughput limiter
that keeps the tick short. Charge the true 6–8 ms and many more fine resolves fit
in one tick — which is the middle band getting worse. The accurate model helps
only the extreme tail, where its 43 and 108 ms figures defer the genuinely huge
resolves.

The flat figure is kept, and it is also the one the owner saw pictures of.

## What would settle it

The owner's verdict on those stands, and a repeat of the cap 4/8 runs — the max
column especially, since cap 2's two runs disagreed by 400 ms on it. The
mechanism is built, instrumented on both sides, swept, and off.
