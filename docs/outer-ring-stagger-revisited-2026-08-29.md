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
