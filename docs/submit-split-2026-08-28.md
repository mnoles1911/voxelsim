# What `submit` is, on the frames that matter

2026-08-28, two legs on the shipped default (buried skip off), 2560x1440 line
flight from `-61440,-61440`. First reading of the submit bracket's six parts on a
per-FRAME basis; until now the split existed only as window accumulators, which
cannot isolate the worst 1% of frames.

## The instrument checks itself before it says anything

`sDrift = subTotal - (the six)` reads **-0.000 on every bucket of both legs**. The
parts partition the whole. And `band` reads **0.013 ms** where it was the largest
GPU block a day ago -- an independent confirmation, from a counter that knows
nothing about the change, that the band really is gone.

## Result

    bucket   subTotal   reqHdr    band   raster   assets    pool     mgr   loopSubmit
    FAST        0.047    0.032   0.001    0.002    0.001   0.002   0.009      0.027
    SLOW        1.015    0.573   0.005    0.361    0.004   0.011   0.061      0.893
    TAIL        4.010    2.078   0.014    1.726    0.010   0.027   0.157      3.543

    tail - fast     reqHdr +2.005 / +2.086      raster +1.536 / +1.909
                    mgr    +0.154 / +0.143      everything else <= +0.026

**Submit at the tail is TWO things and nothing else: `reqHdr` (~52%) and `raster`
(~43%).** Together they are 3.6-4.0 ms of a 14.29 ms p99 frame.

## AND BOTH ARE BIMODAL -- the means understate them badly

    maxRaster   fast 0.10-0.15    slow/tail 32.74 / 42.60 ms
    maxReqHdr                     slow/tail 43.18 / 51.57 ms

Single frames spend **43-52 ms in reqHdr** and **33-43 ms in raster**. Those are
the freezes. A term whose mean is 1.7 ms and whose max is 42.6 ms is not
described by its mean, and `maxRaster` was printed for exactly this reason.

## This RECONCILES an earlier retraction rather than reversing it

The raster-atlas attribution was withdrawn earlier tonight because
`VoxelRasterAtlas.cpp` records the atlas at **943 ms of game thread across 130
post-settle windows** -- far too little to explain a large mean. That arithmetic
was right and so is this: ~2 tail frames per window at ~1.7 ms plus ~218 fast
frames at ~0.002 is ~3.8 ms per window, the same order as the recorded 7.25.

**A term can be negligible in aggregate and dominant in the tail at the same
time.** The mean was never the question; the gate is a 1% low.

## What each one is

**`reqHdr`** is the region request header -- footprint, seed, climate shading and
skirt -- built per call. Largest mean contributor at the tail and the largest
single-frame spike measured (51.57 ms). Least investigated of anything at this
level.

**`raster`** is the atlas: `PrepareRequest` plus the inline `FillRasterWindow`.
Mechanism already understood -- a page is 128 px x 1.875 m = 240 m, so at flight
speed a whole page COLUMN comes due about every 10 s, and the fills are
synchronous on the game thread. Both known levers are already spent: the demand
page cap is tuned (64 -> 256 measured, 1024 is worse), and **async fill is
REFUTED** -- it moves 85% of the work off the game thread and is worse overall
because it spreads the stall (hitch time 3957 -> 5834 ms, frames >=200 ms 1 -> 4).

## Where this sits against the target

p99 is now 14.29 ms (70.0 fps). The owner's 1% low >= 50 fps gate is met with 20
fps of margin. The older ">100 fps steady" target needs p99 at 10 ms, i.e. -4.3 ms
-- and `loopSubmit` alone is 3.54 ms of the tail frame. **Submit is the right size
to be the next target, and it is now two named terms rather than one opaque one.**
