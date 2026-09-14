# Half the quiet game thread was two queries that had already been answered

Three walk captures, ONE binary, a cvar apart, frames 0:200 of each, temperate
forest at `-154740,-81476`, 256 m ring, Nanite default. Receipts passed.

| arm | cvars |
|---|---|
| control | `RoofProbeShortlist=0, ShortlistAcrossFrames=0` |
| roof probe only | `ShortlistAcrossFrames=0` |
| both (the default) | none |

## The ladder

| counter (median) | control | roof only | both | control → both |
|---|---|---|---|---|
| **GameThreadTime** | 13.029 | 9.877 | **6.168** | **−6.86 ms, −52.7%** |
| RoofProbeMs | 3.097 | 0.003 | 0.004 | −99.9% |
| ClipmapTickMs | 3.099 | 0.004 | 0.006 | −99.8% |
| CollisionPrepareMs | 3.663 | 3.855 | 0.001 | −100% |
| CollisionPreparations | 1.000 | 1.000 | 0.000 | |
| PawnTickMs | 3.920 | 4.103 | 0.364 | −90.7% |
| GPUTime | 12.622 | 12.591 | 12.592 | −0.2% |
| FrameTime | 13.816 | 13.843 | 13.954 | +1.0% |

**The roof probe is worth 3.15 ms and the retained shortlist another 3.71 ms.**
Neither is a visual trade: the roof probe computes the identical boolean from the
identical samples, and the retained shortlist returns the identical instance list
while the asset field's `configurationRevision` is unchanged.

**The prediction held.** Written before the run: `CollisionPreparations` stays at
1 per frame, because the sweep rect is about ±4 voxels around the pawn's XY while
the probe's covered rect is ±32 voxels around the same column, so whichever ticks
first the other's request is contained. It reads 1.000 on the roof-probe arm —
the change did not buy its saving by forcing a second prepare. With retention on
it reads 0.000, which is the shortlist surviving the frame.

## The honest half: this buys no frame time today

`FrameTime` does not move — 13.816 → 13.954, inside run-to-run spread. With
Nanite in, the frame is bound by the render path at ~12.6 ms of GPU, and the game
thread at 13.0 ms was only just above it. Taking the game thread to 6.2 ms moves
it from "the joint constraint" to "not the constraint", and the frame stays where
the GPU leaves it.

That is worth having anyway, and not because of the median:

- **The tail is game-thread work.** The p99 hitch is a cold asset resolve on this
  same thread (`docs/measurements/submit-cold-resolve-2026-09-13/`), and 6.8 ms
  of headroom per frame is 6.8 ms that a hitch no longer adds to.
- **The target is written for 20 m/s**, where streaming does far more per frame
  than it does at a walk. Every number here is from a walking pawn.
- **It removes two of the four items** that the 2026-09-11 attribution found were
  "expensive queries rebuilt every frame for answers that barely change".

`GPU/VoxelMarch` is unmoved across all three arms, which is the control that says
this is game-thread only.
