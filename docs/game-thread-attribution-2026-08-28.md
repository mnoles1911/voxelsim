# What `gameMs` is, and what is inside it

2026-08-28. One moving leg, shipping default, 2560x1440, CSV profiler with the
engine's own `Exclusive/GameThread/*` stats. 9,000 moving rows at 23.4 m/s.

## What the field actually is

`gameMs` is `GGameThreadTime` -- the engine's game-thread **BUSY** clock, and it
**EXCLUDES** `GGameThreadWaitTime`. They are two separate clocks, not a whole and
a part, so `gameMs - gameWait` is meaningless: a leg once printed
`gameBusy = -2.68` from that subtraction, which is how the distinction was found.
Read `gameMs` directly as busy, and `gameMs + gameWait` against `frame` to see how
much of the frame the game thread's own clocks account for.

## Where it goes -- and the limit on what can be claimed

    bucket   gameMs   voxel tick    not voxel
    FAST       2.60      0.60 (23%)      2.00
    TAIL      16.71     10.34 (62%)      6.37

The voxel half is fully broken down elsewhere (recompute 3.81, dispatch 4.46 of
which submit 3.50, apply 1.00, unload 0.27, brickFlush 0.07, unnamed 0.51).

**THE NON-VOXEL HALF CANNOT BE FULLY ACCOUNTED FOR, and this is stated rather
than papered over.** Summing every `Exclusive/GameThread/*` scope against frame
time on the same rows:

    bucket   frame   sum(GT busy)   sum(EventWait)   accounted
    FAST      8.55       2.847           5.629         99.1%
    TAIL     22.25       7.849           3.865         52.7%

**On fast frames the engine's own game-thread scopes sum to the frame almost
exactly. At the tail 10.5 ms is outside every named scope.** So the terms below
can be RANKED but do not constitute a complete decomposition of `gameMs`, and
presenting them as one would be inventing a total.

## The largest named non-voxel term

    Exclusive/GameThread/       FAST     TAIL     delta
    EndOfFrameUpdates          0.141    3.785    +3.643
    Tickables                  1.178    2.312    +1.134
    TickActors                 0.206    0.325    +0.119
    everything else            <=0.10   <=0.14   <=+0.04

`Tickables` contains the voxel tick and reconciles with it (voxel tick
0.880 -> 1.784 on these rows), so that term is not a second finding.

**`EndOfFrameUpdates` is the finding: +3.64 ms, an order of magnitude above every
other named non-voxel term.**

## AND IT IS NOT TERRAIN PUBLICATION -- the obvious explanation is dead

`EndOfFrameUpdates` processes components whose render state was marked dirty, and
chunk publication marks render state dirty, so the natural reading is that this is
streaming cost landing outside the voxel tick. **It is not.**

    corr(EndOfFrameUpdates, ChunksApplied) = 0.047     <- essentially zero
    corr(EndOfFrameUpdates, FrameTime)     = 0.325
    mean when chunks == 0:  0.201 ms  (n=7,106)
    mean when chunks  > 0:  0.308 ms  (n=1,894)

A 0.107 ms difference between chunk frames and chunk-free frames cannot produce a
3.79 ms tail. The cost is uncorrelated with chunk arrival.

**Its shape is BURSTY AND CLUSTERED, not periodic:** 375 of 9,000 frames exceed
2 ms, in runs of consecutive frames (gaps of 1-3), with the worst at frame
indices 5381/5383 and 5464/5465/5466. A scheduled batch would be evenly spaced;
this is something spawning or registering components across several frames.

## Candidates, none confirmed

`quads=0` on this leg, so terrain chunk components are not drawing.
`VoxelAgentSubsystem` calls `AgentISM->MarkRenderStateDirty()` at several sites
(which destroys and rebuilds the ISM proxy) and reports "swarm ISM ready", but no
agent population is logged, so it cannot be confirmed or excluded from this leg.
`VoxelDetailAssetSubsystem` places detail assets during flight and would cluster
this way. **Do not quote a cause.**

**The identifying step is cheap:** a counter on each `MarkRenderStateDirty` call
site, attributed by subsystem, read on a leg alongside this column. That says
whose components they are in one leg and settles it.
