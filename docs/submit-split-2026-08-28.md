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

---

# What is inside `reqHdr`: a 100x-bimodal terrain sampler

`reqHdr` spans `SubT0..SubT1`, and everything in it is trivial arithmetic --
`SetChunkFootprint`, a seed copy, `CoarseLevel`, `RingSkirtMask` -- except one
call: `VoxelApplyFast::ShadingForDispatch`, four `GetSurfaceHeightUU` amplifier
columns plus a climate sample, per submission, on the game thread.

## The cache in front of it IS working

    busiest window   calls=45,125  avoided=40,025 (88.7%)  cacheHit=23,007  miss=5,100
    another          calls=27,974  avoided=25,172 (90.0%)  cacheHit=13,157  miss=2,802

~89% avoided, `cacheEvict` 173-208 against 131,072 slots, `mismatch=0`,
`offThread=0`. Eviction is not the problem; the misses are genuine first touches
on footprints the camera has never visited.

## The MISSES are 100x bimodal, and that is the tail

    per leg, 65 active windows      SUB-a            SUB-b
    us/sample   min                  5.7              6.3
                mean                30.8             35.9
                MAX                327.6            587.0
    ms/window   mean                38.3             39.8
                MAX                917.9           1000.1
    aggregate                     2,490 ms         2,587 ms   = 2% of game thread

**A single sample swings 100x, and the worst 2-second window spends ~918-1000 ms
of game thread -- about half of it -- inside this sampler.** In aggregate it is
2%. Negligible mean, dominant tail: the same shape as every other real finding on
this programme, and the reason a 1% low is a different problem from a mean.

The magnitude points at I/O. `ShadingImpl`'s own comment says
`GetSurfaceHeightUU`'s fine-tier prefetch "takes the sampler's lock exclusively
and can do disk I/O", which is also why the whole thing is game-thread-only
(`offThread` is a hard 0, checked rather than asserted).

## A COMMENT AT THE CALL SITE IS WRONG, and worth fixing

The site says the shading call "IS that bucket's body". That is right. But the
first reading of `Voxel apply fast` taken here showed `calls=0` and appeared to
refute it -- because it was read from the LAST windows of the leg, which fall in
the 60 s parked linger where nothing streams. **69 of 134 windows legitimately
read calls=0.** Sorting all windows instead shows 65 active ones peaking at
45,125 calls.

Third time tonight the same error class has appeared -- `Hitch frame` lines above
33.3 ms, a per-window TAIL bucket of three frames, and now a parked linger window.
**Check what population an instrument is describing before reading it**, and on
this harness "the last window" is never the flight.

## The lever

Not the cache -- it is at 89% with almost no eviction. The cost is cold
footprints being sampled inline at submit time, with a fine-tier fetch behind
them. The candidate is to warm those samples ahead of the camera rather than pay
them on the submitting frame; the streaming path already predicts where the
camera is going (`voxel.Stream.VelocityLeadSec`, and the admission scan knows the
footprint set a tick early). NOT yet attempted, and it must be measured with
`avoided` and `reqHdr` read TOGETHER -- the module's own instructions say
`avoided ~= calls` with a flat `reqHdr` means the diagnosis is wrong and the
change should be reverted rather than tuned.

---

# The lock is NOT the lever, and the disk-I/O guess was wrong

`-VoxelFineLockMeter=2`, one leg, the shipped default. The instrument was built
for exactly this question and had never been run.

    req calls=8,956  lockFree=0  shared=0  excl=8,956   (exclusive avoided 0.0%)
    EXCL   timed=8,969   wait=10.0ms  hold=1.1ms   us/timed  wait=1.11  hold=0.12
    SHARED timed=9,549   wait= 0.3ms  hold=0.4ms   us/timed  wait=0.033
    waitShare=87.8% of 11,745,787 ns in-lock
    acq[elev=1,019,320 climate=191,540 isResident=9,543 reqExcl=8,956 coldGame=0 ...]
    entered=1,229,378                         contended=YES

**It is armed and in the path** -- `entered` is 1.2 million, which is the reading
that separates "no contention" from "instrument outside the path". The header
insists on that distinction because twelve instruments on this project have read
green while never being in the path.

**And the verdict line says `contended=YES`, but the magnitudes refute the fix.**
Most time INSIDE the lock is waiting (87.8%), yet the entire lock accounts for
~11.7 ms per 2-second window while the sampler it guards costs up to 918-1000 ms
in that same window. **Per call: 1.11 us of wait against a 22-587 us sample.**
`-VoxelFineLockFast=1` -- the all-resident shared fast path, currently never taken
(`reqFast=0`) -- would buy about a microsecond a call. It is not worth a leg.

**`waitShare` is a RATIO AND ONLY A RATIO.** 87.8% of a small number is a small
number, and reading that field without its denominator would have sold a fix
worth ~1 us as though it were worth 500. The line prints `of 11,745,787 ns` for
exactly that reason; read both.

## And my own mechanism guess was wrong

I attributed the 587 us to the fine-tier prefetch doing disk I/O on the game
thread, reasoning from magnitude alone. **`coldGame=0`** -- `ResolveNonResidentPixel`
on the game thread never fired once. There is no blocking tile load in this path
on this leg.

So the 587 us is neither lock waiting nor I/O. **It is work** -- four
`vxc::Amplifier::column` evaluations plus a climate sample -- and it degrades 20x
under load, which is what worker-pool pressure on cache and memory bandwidth does
to a game-thread compute loop. That is a different problem with different fixes
from the one I proposed.

## What is left, honestly

Not the lock (measured, ~1 us). Not the cache (89% avoided, negligible eviction).
Not I/O (`coldGame=0`). The remaining shapes are: **sample fewer cold footprints**,
**make the column evaluation cheaper**, or **stop paying for it on the submitting
frame** -- and the last one is constrained by the sampler being game-thread-only
by design (the table is unsynchronised; `offThread` is a checked hard zero).
None of these is a flag, and none should be attempted on a magnitude argument
again.
