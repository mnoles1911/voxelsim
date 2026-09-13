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

## Why it is not on by default

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

## What would settle it

A route capture pair — default against cap=2 — at the eight authored stands, plus
a cap sweep at 2/4/8 read against the >300 ms rate and p95 together. The
mechanism is built, instrumented on both sides, and off.
