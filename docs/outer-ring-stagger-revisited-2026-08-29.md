# The outer-ring stagger, re-measured: the parked verdict no longer holds

2026-08-29. Four legs alternated on today's binary (STG2-{ctl,on}-{a,b}),
2560x1440 line flight from -61440,-61440, engagement proven both armed legs
(25 windows with deferred>0, maxStreak 2; control prints `stagger=off`).

    metric        ctl-a   ctl-b  |  on-a   on-b     change
    hitches >=33ms    4       6  |     0      0     ELIMINATED
    stutterPct     0.32    0.36  |  0.15   0.20     -49% (goal 0.10)
    worst frame   43.80   45.26  | 25.92  30.32     -37%
    p95           11.20   11.00  | 10.90  10.80     -0.25 ms
    p99           15.70   15.60  | 15.20  15.40     -0.35 ms
    p50            7.20    7.10  |  7.10   7.00     -0.10 ms
    tracked      79,889  79,997  | 79,809 79,809    -0.10 to -0.24%

## Why the same arm reads differently than on 2026-08-28

August's verdict was "buys a better worst frame, moves the stated gate by
NOTHING" -- p95 and p99 flat, stutterPct not credited. Two things changed
underneath it, neither of them this code:

1. **The atlas metronome hitch class is extinct** (AtlasCoveragePadChunks
   shipped today). In August the control's hitches were dominated by 33-43 ms
   atlas fills, which this arm cannot touch; removing those made the ring
   coincidence spikes the LARGEST remaining thing above the stutter bar.
2. **The frame is faster** (50% resolution): typical 7.1 ms against ~8.5.
   A fixed 20 ms stutter bar and a fixed 33.3 ms hitch bar catch a different
   population when the baseline moves, so the same spikes are now a bigger
   share of what crosses them.

The August arithmetic still stands and is not retracted: coincidence ticks are
~0.9% of frames, so they sit at and above p99 and CANNOT move the 99th
percentile much. The 0.35 ms p99 gain here is small and consistent with that;
the real movement is where it was always predicted to be -- hitches, worst
frame, and now stutterPct, which is the goal that is actually still open.

## The cost, re-measured and SMALLER than August

-0.10 to -0.24% tracked chunks (August: -0.37%), all in rings 512 m - 4 km,
each arriving one tick (~7 ms) later. `loaded` unchanged: the set lags, it
does not shrink.

## What is still not settled, and is now buildable

The open question is unchanged and remains the owner's: **is geometry between
512 m and 4 km arriving ~7 ms later visible at 20 m/s?** A parked capture
cannot show it (both arms converge during the 120 s settle) -- and this is the
second decision blocked on that gap, after the half-res rejection. A
distance-triggered MOVING capture (shutter fires at fixed metres along the
deterministic line flight, so both arms photograph the same ground at the same
pose) is being built for exactly this.

## Recommendation, stated plainly

**On the numbers this now ships.** It is the largest single move on the
stutter goal measured today -- 0.34% -> 0.175%, most of the remaining distance
to 0.10% -- it eliminates hitch frames outright, and it improves every
percentile slightly rather than trading them. Default stays 0 only until the
moving captures answer the far-field question, because a streaming-schedule
change is a claim about what the world looks like while you fly through it,
and this project does not take those claims on timing alone.

## The image question: ANSWERED BY ARITHMETIC, because the picture test could not

Moving captures were built for this decision (commit ead5f0b): a shutter that
fires on distance travelled so both arms photograph the same ground.
**The pose matching works** -- five matched distances, position gaps 2-6 cm,
shutter bracket 0.165 m, verified by tools/voxel-pair-moving-shots.py.

**And the comparison still cannot decide it.** A SAME-CONFIG rerun (stgCtl vs
stgCtl2, identical switches) differs from itself by:

    distance   floor (ctl vs ctl2)   arm (ctl vs on)
      512 m      70.72% / mean 38.54   68.70% / 36.93   below floor
     1024 m      18.33% /  5.35        30.25% /  8.86   above floor
     1536 m       8.10% /  3.11        10.27% /  3.67   below floor
     2048 m      59.00% / 22.01        20.79% /  5.64   below floor
     2560 m      16.76% /  5.24         6.43% /  2.75   below floor

Streaming arrival order is not deterministic under load, so a moving frame
catches a different in-progress world on every run. The cross-arm difference
is smaller than the same-arm difference at four of five distances. **A moving
pixel A/B cannot resolve an effect this small, and reruns will not fix it** --
recorded because the half-res question sits in the same trap, and a day could
be spent there.

So the cost is bounded in the unit that actually matters -- DISTANCE, not
pixels and not chunk counts:

    tick rate (armed leg)  353 ticks / 2 s window = 176 Hz -> 5.67 ms
    camera speed           23.4 m/s
    one tick               0.133 m of camera travel
    worst observed streak  2 ticks = 0.265 m
    against the boundary   512 m -> 0.05%    4096 m -> 0.007%

**The far ring arrives when the camera is about a foot further along.** That is
the whole trade, and it is why this ships: shipped default 1, revert with
`-VoxelAmortizeOuterScans=0`.
