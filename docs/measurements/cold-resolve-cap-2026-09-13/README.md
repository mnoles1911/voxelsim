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

## What would settle it

A route capture pair — default against cap=2 — at the eight authored stands, plus
a cap sweep at 2/4/8 read against the >300 ms rate and p95 together. The
mechanism is built, instrumented on both sides, and off.
