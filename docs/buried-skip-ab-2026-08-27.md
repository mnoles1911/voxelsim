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
