# What the worst 1% of frames is actually made of

2026-08-27. Pooled from **13 legs that carry the per-frame GPU clock**, n-weighted over every
reporting window: **4,456,807 fast frames and 89,650 tail (>=p99) frames.** No new leg was run to
produce this -- it is the existing archive read on the right population.

## The headline: the tail has TWO regimes, and they are not the same problem

    bucket        n      frame   gameMs      gpu   render   gameWait
    FAST    4,456,807     8.03     2.75     6.97     8.23     5.39
    SLOW      446,156    17.03     6.93    12.82    10.70     6.61
    TAIL       89,650    22.94    13.78    13.35     9.92     6.08

`gameMs` and `gpu` run concurrently, so they do not sum to `frame`. Read the steps separately:

    FAST -> SLOW   gpu +5.85   game +4.18     <- GPU-LED
    SLOW -> TAIL   gpu +0.53   game +6.85     <- GAME THREAD ONLY. The GPU has stopped rising.

**The GPU saturates at ~13 ms and then stops.** Everything that separates a p95 frame from a p99
frame is on the game thread. Since the goal is stated as a 1% low, **the p99 step is the goal**,
and the GPU work is aimed at the p95 step.

This is the first time the two could be told apart, because until this week the project had no
per-frame GPU clock at all. The clock passes its own falsifier here: the FAST bucket reads
`gpu=6.97` against a ~5.8 ms parked GPU frame, which is the right side of it for a moving leg.

## Inside the +11.01 ms game-thread rise

    bucket    gameMs      tick   dispatch    submit    apply   chunks/frame
    FAST        2.75     0.543      0.212     0.033    0.021       21.0
    SLOW        6.93     3.030      1.299     0.852    0.186       76.4
    TAIL       13.76     8.963      4.125     3.276    0.469       49.3

`submit` is a component of `dispatch` (`FFrameSample::SubmitMs`, and the dispatch line decomposes
as `airProof + band + submit + pick + overlay + other`), so they do not add.

    voxel subsystem tick        +8.42   76.5% of the game-thread rise
      of which dispatch         +3.91     46% of the tick rise
        of which submit         +3.24     83% of the dispatch rise
      of which apply            +0.45
      UNATTRIBUTED within tick  +4.06   <- the largest single term, and it has no name
    everything else on the game thread  +2.59

## Two targets, and neither is the one this programme has been aiming at

**1. `submitMs`, +3.24 ms.** The largest NAMED item, and **what is inside it is NOT established.**

The field comment says "raster-atlas fills land here", but the atlas is very unlikely to be this
term. `VoxelRasterAtlas.cpp:160-180` records a measured post-settle figure: **943 ms of game thread
in total across 130 windows** (fill 734.3 + demand 209.0) on the shipping default. That is ~3.6 ms
per second of wall clock. It cannot supply a +3.24 ms mean across 89,650 tail frames.

The submit bracket also covers **the GPU fork's region request**, which samples per-chunk shading.
So `submitMs` is at least two different costs sharing one counter, and the split is unknown.

**The decomposition already exists and cannot be read on this population.** `Hitch frame dispatch`
prints `dispatchMs = airProof + band + submit + pick + overlay + other` -- but only above 33.3 ms,
and the tail sits under that bar. **That is the third time this exact bar has blocked this exact
question.** Carrying that split onto the frame sample is the instrument that ends it.

**What IS corrected:** `submitMs` was recorded as "dead three times". Two of those readings were
taken off hitch lines (wrong population). The third was at p95, where it is genuinely small
(+0.82 ms). At p99 it is +3.24 ms. The refutations do not cover the population that matters --
but neither does anything yet explain what the 3.24 ms IS.

**2. The +4.06 ms inside the tick that nothing names.** Bigger than submit. `FFrameSample` carries
frame, tick, render, renderWait, rhi, gameWait, game, gpu, dispatch, submit, apply and the chunk
count -- and **not** remesh or unload. So this residual cannot be split at frame granularity today.
Adding those two floats to the sample is the next instrument, and it is small.

## Robustness: it is not an artefact of pooling swept arms with controls

The 13 legs include batch-cap, harvest-cap and apply-budget variants. Split apart, the two regimes
survive in both halves independently:

    CONTROLS ONLY (5 legs, 1,530,015 FAST / 30,794 TAIL)
      FAST->SLOW  gpu +5.48  game +4.07      SLOW->TAIL  gpu +0.91  game +5.41
    SWEPT ARMS ONLY (8 legs, 3,000,652 FAST / 60,344 TAIL)
      FAST->SLOW  gpu +6.05  game +4.23      SLOW->TAIL  gpu +0.34  game +7.56

The controls-only table, which is the one to quote:

    bucket        n      frame   gameMs      gpu     tick   dispatch   submit
    FAST    1,530,015     8.05     2.77     6.98     0.55      0.21     0.03
    SLOW      153,164    16.50     6.84    12.45     2.93      1.18     0.72
    TAIL       30,794    21.78    12.25    13.37     7.84      3.30     2.50

FAST -> TAIL on controls: frame +13.73, game +9.48, gpu +6.39, tick +7.29, dispatch +3.09,
submit +2.47. The voxel tick is **76.9%** of the game-thread rise, against 76.5% on the full pool.
The swept arms have a heavier tail than the controls, which is expected -- several of those arms
were deliberately detuned -- and they do not change the shape.

## What this re-confirms, on much larger n

**It is not volume.** Tail frames apply **fewer** chunks than slow frames -- 49.3 against 76.4 --
and are 5.9 ms slower. The frames doing the most streaming work are still not the slow frames.
Every lever that regulated volume (harvest cap, batch cap, apply budget, halving publication)
failed for this reason, and this table says why on 89,650 tail frames rather than a few hundred.

## The care that this reading needed

The first read of this was **wrong in the opposite direction** and looked convincing. Taking the
last attribution window of one leg gave `TAIL gameMs=2.25, gpu=18.58` -- a clean "the tail is the
GPU, the game-thread theory is dead" story. That window's tail bucket held **three frames.**
Pooling every window across every leg reverses it. **A per-window tail bucket is n=3 to n=12;
it is not a population.**
