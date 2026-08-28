# The buried-chunk skip costs more than it saves

2026-08-27. Four legs, alternated A,B,A,B, 2560x1440, line flight from
`-61440,-61440`, 120 s run after a 90 s preflight. Same binary on every arm --
no build ran between them.

## Result

    arm                    p50            p95             p99            max
    control (skip ON)   9.25 (108 fps)  13.71 (73.0)  17.60 (56.8 fps)  126.2
    -VoxelBuriedSkip=0  9.19 (109 fps)  13.78 (72.6)  16.41 (60.9 fps)  116.4

**p99 improves 1.19 ms, 56.8 -> 60.9 fps. p50 and p95 are unchanged.** The worst
single frame also falls, 126.2 -> 116.4 ms.

**It reproduces.** Within-arm spread is 0.04-0.08 ms against a 1.19 ms gap:

    control  p99 17.64 / 17.56       skip off  p99 16.39 / 16.43

## THE ARM I INTENDED DID NOT ENGAGE, AND THIS IS NOT IT

This sweep was built to remove the band -- +2.51 ms and 43% of the GPU's rise to
p95, in three terms that exist because mesh-region graphs are `kept because:
quads 0, band 31671, noPack 0`. **It did not.** Kept graphs went 31,623 -> 32,091:
UP, not to zero.

**The request and the consumer are gated separately, and only the consumer has a
switch.** `bComputeBand` (VoxelWorldSubsystem.cpp:21730) decides whether anything
will CONSULT the band and is what `-VoxelBuriedSkip` turns off. The REQUEST
predicate is `bWantBand` (:20647) and reads only `bBandSeedOnly`, `bBandColdOnly`,
`bBandCached` and `bBandReqInFlight` -- **it never asks whether a consumer
exists.** So with the skip off the band is still computed, still fenced, still
keeps the graph, and now nothing reads it.

**That means this measurement is the skip's own cost, with band cost held
constant on both arms.** The band A/B has not been run. The switch for it is
`-VoxelGpuBandColdOnly=1` (default off), which the `[gpu-lean]` counter's own
comment names.

## What the skip was actually buying: about 1%

    metric                control      skip off
    chunks loaded          47,382        47,936     +1.2%
    chunks tracked         79,281        79,246     -0.04%
    chunks applied /frame at p99  51.6      89.9     +74%

**The resident set at the end is the same world.** The skip avoids ~1% of loads
across a whole flight while every uncached footprint pays a band readback fence
to find out. The +74% applied-per-frame at p99 is flow, not content.

## The mechanism is NOT settled

The off arm meshes 74% more chunks per tail frame and its **GPU falls** --
13.73 -> 11.96 ms at p99, while game thread rises 13.24 -> 14.27 and render rises
9.58 -> 10.88. Two candidate readings, and they mean very different things:

1. **Denser residency, cheaper marching.** A ray that finds nothing marches
   further, so filling the gaps the skip leaves makes the marcher cheaper. This is
   the same hypothesis the GPU split raised independently (`GPU/VoxelMarch`
   correlates with coverage, r = 0.08 with chunk count).
2. **The control is missing content.** This project has two recorded routes where
   "provably air" is wrong -- a missing fine tile reads as sea level, and absence
   reads as air. If the skip drops chunks that should exist, the control is
   rendering less and the comparison is not a fair one.

Both make the off arm faster; only one makes it correct. **The near-identical
tracked count argues for (1)** -- the same chunks are resident at the end -- but
that is an instrument, not an image.

## WHAT THIS NEEDS BEFORE IT SHIPS: the image, and it is BLOCKED

`tools/voxel-capture.ps1` refuses to run: `voxelcore.lib` is 6 hours behind
`brickpack.h`, which belongs to another session's module. Rebuilding it is not
mine to do and would regenerate the world underneath tonight's entire measurement
series. **So there is no image A/B for this and it must not ship on timing alone**
-- this project has already shipped a "-7.6% win" that was the marcher deleting a
mountain, and the timing inverted to +3.1% once the image was honest.

Default stays 1. The next two steps, in order: matched captures once the lib
question is resolved, then `-VoxelGpuBandColdOnly=1` for the band itself.

---

# Part 2: with the band actually removed

The fix above (commit 17f0245) made the band request ask whether a consumer
exists. Re-run, four legs, alternated, same protocol, on the rebuilt binary.

## ENGAGEMENT, first, because the previous two sweeps failed here

    control      kept=31,650  (band 31,650)
    skip off     kept=0       (band 0)        <- every mesh-region graph now lean

Both off legs read exactly zero, from 31,650. **This is the first arm in three
sweeps that actually removed the band.**

## Result

    arm                    p50             p95             p99            max
    control            9.16 (109.2)   13.51 (74.0)   17.57 (56.9 fps)  139.8
    -VoxelBuriedSkip=0 8.49 (117.8)   11.96 (83.6)   14.28 (70.1 fps)  123.7

**p99 -3.29 ms, 56.9 -> 70.1 fps. p95 -1.55 ms. p50 -0.67 ms.** Every percentile
moves, which is what removing real work looks like. Worst frame 139.8 -> 123.7.

Per-leg, within-arm spread 0.11-0.19 ms against a 3.3 ms gap:

    control  p99 17.66 / 17.47      skip off  p99 14.33 / 14.22

**The band was worth ~2.1 ms of p99 on its own.** Part 1 (skip off, band still
computed) reached 16.41; this reaches 14.28.

## It is a TRADE, and the trade is now visible

    bucket        ctl gpu   off gpu      ctl game   off game    ctl chunks  off chunks
    TAIL            13.50      8.36         13.41      17.26          52.4        67.9

**GPU -5.14 ms, game thread +3.85 ms.** Removing the band takes a large block off
the GPU; losing the buried skip puts more chunks through the game thread. The net
is strongly positive because the GPU was the p95 driver -- but note what it does
to the shape: at the tail the game thread is now 17.26 ms against 8.36 ms of GPU.
**This arm does not just make the frame faster, it moves the bottleneck.** Any
further work should be planned against the new shape, not this table's control.

**The two halves are coupled by design** -- the band IS what the skip consumes --
so they cannot currently be taken separately. Whether the skip can be served by
something cheaper than a GPU reduce plus a readback fence is the open question,
and it is now worth asking: the skip's own value was measured at ~1% of loads.

## The image: still not verified, and the case for it is now stronger

    metric              control    skip off
    chunks loaded        47,623     61,314    +28.7%
    chunks tracked       79,246     79,281    +0.04%

**The resident set is the same world.** The off arm churns 28.7% more loads across
the flight and ends holding the same tracked chunks. That is a good argument and
it is still an instrument, not a picture.

`tools/voxel-capture.ps1` continues to refuse while `voxelcore.lib` is behind
another session's `brickpack.h`. **Default stays 1.** A +13 fps p99 is exactly the
size of result that this project has previously been wrong about -- the "-7.6%
win" that was the marcher deleting a mountain inverted to +3.1% once the image was
honest -- and the direction here (more chunks, not fewer) is the safe one but not
a proof.

---

# Part 3: the image, and SHIPPED

## The owner judged it: "they look the same"

Matched captures at a pinned pose -- column `-61440,-61440`, +60 m above the
surface, pitch -10, yaw 45, sun frozen 12:00 03-20, 2560x1440, 120 s settle.
Same ridgeline, same couloirs, same snow line.

    Saved/BANDIMG-ctl-stock.png        stock
    Saved/BANDIMG-off-bandremoved.png  -VoxelBuriedSkip=0
    Saved/BANDIMG-ctl2.png             stock again -- THE NOISE FLOOR
    Saved/BANDIMG-diff-x8.png          difference, amplified 8x

## AND THE NOISE FLOOR IS WHAT MAKES IT A MEASUREMENT

A cross-arm pixel difference means nothing without knowing what two runs of the
SAME arm produce. So a second control was captured:

    comparison                        any diff   >8/255     >32/255   max
    SAME CONFIG, two runs              48.604%   0.4855%    0.0106%    99
    stock vs band-off                  47.259%   0.5240%    0.0135%   163
    stock (2nd run) vs band-off        43.533%   0.3631%    0.0088%   161

**The cross-arm difference is indistinguishable from the same-arm noise floor,
and the second control is CLOSER to the band-off arm than to its own twin.** The
change produces no image difference above run-to-run variation.

The >32 outliers are diffuse speckle -- 73 cells of 943 at 64 px, at most 4
pixels each -- never the contiguous block a hole or a deleted ridge would make.

## SHIPPED: `BuriedSkipEnabled()` default 1 -> 0

Every gate this project asks for was cleared, in order:

    image first        owner verdict + a noise floor that can fail
    engagement proved  kept 31,650 -> 0, and TWO earlier sweeps that failed
                       this test had their timings discarded unread
    reproducible       within-arm spread 0.11-0.19 ms vs a 3.3 ms gap
    can fail           the noise floor was capable of showing the opposite

`-VoxelBuriedSkip=1` restores every build before this one.

## What is now true, and what it changes

    p50  9.16 -> 8.49 ms   109.2 -> 117.8 fps
    p95 13.51 -> 11.96      74.0 ->  83.6
    p99 17.57 -> 14.28      56.9 ->  70.1 fps
    max 139.8 -> 123.7

**The owner's 1% low >= 50 fps gate is met with 20 fps of margin, and p50 clears
the 100 fps steady target.** p95 and p99 do not.

**THE BOTTLENECK MOVED, and the next investigation must start from the new
shape.** At the tail, GPU -5.14 ms and game thread +3.85: the band leaves the
GPU, the unskipped chunks arrive on the game thread. The tail is now **17.26 ms
of game thread against 8.36 ms of GPU**. Everything in
`docs/p99-game-thread-split.md` that pointed at the game thread now points harder
-- and `submit`, already 86% of the dispatch rise, is the first place to look.
