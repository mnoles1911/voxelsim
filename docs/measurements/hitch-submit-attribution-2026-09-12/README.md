# The hitches are the streaming submit bracket, and the CSV hides it one row away

Desk analysis, 2026-09-12, no new capture: two frame CSVs already on disk, read
with `scratchpad/hitch.py`, `gtworst.py` and `wait.py` (method below). Phase 2 of
`docs/slice-performance-plan-2026-09-10.md` asked what blocks the game thread for
over a second. It is named now, to a scope, on the current binary.

## The reading rule that had to come first

**`FrameTime` lands one row AFTER the stall that caused it.** Read the top frames
by `FrameTime` and every thread timer looks innocent:

| row | FrameTime | GameThreadTime | RenderThreadTime | GPUTime |
|---|---|---|---|---|
| 1837 | **1650.71** | 43.2 | 12.4 | 19.0 |
| 1836 | 22.8 | **1651.24** | 3.6 | 13.0 |

The stall is on row 1836 and its consequence is stamped on 1837. The mirror that
proves it: on row 1836 `Exclusive/RenderThread/EventWait` reads **1642.6 ms** --
the render thread sat waiting for a game thread that was gone. Rank frames by
`GameThreadTime`, not by `FrameTime`, or the analysis measures the recovery frame
instead of the stall. My first pass did exactly that and read "no thread timer
accounts for the hitch", which was an artefact and not a finding.

168 frames in route-capture-10 carry an `EventWait` over 100 ms; 157 of them are
the render thread waiting on the game thread.

## The scope

`route-capture-10-revisit-retire-off` (2026-09-10, 6,028 frames, 48 m ring as the
harness then defaulted). Five worst rows by game thread:

| row | GameThreadTime | VoxelStream/TickMs | DispatchMs | **SubmitMs** |
|---|---|---|---|---|
| 1836 | 1651.24 | 1624.46 | 1557.32 | **1554.72** |
| 2522 | 808.26 | 785.01 | 772.77 | **768.62** |
| 2575 | 806.55 | 783.58 | 775.22 | **686.87** |
| 639 | 783.40 | 760.58 | 673.60 | **669.64** |
| 484 | 774.55 | 750.45 | 659.10 | **654.11** |

The nesting is `Exclusive/GameThread/Tickables` > `VoxelStream/TickMs` >
`DispatchMs` > `SubmitMs`, and each level is within a few ms of the one inside
it. **The hitch is the submit bracket and nothing else.**

Frame distribution on that capture: p50 22.88, p95 46.63, p99 355.37, max
1650.71; 6.95% of frames over 33.3 ms, 3.15% over 100 ms, 0.80% over 400 ms.

## It is still there on the current binary

`walk-capture-57-control` (2026-09-11, 986 frames, 256 m ring, size culling and
the frame shortlist both in). p99 187.21, max 412.75:

| row | GameThreadTime | DispatchMs | **SubmitMs** |
|---|---|---|---|
| 687 | 420.09 | 396.47 | **394.37** |
| 578 | 355.86 | 319.89 | **315.90** |
| 551 | 314.76 | 282.32 | **280.27** |

Same shape, smaller. Nothing landed since 2026-09-10 was aimed at this.

## What is NOT yet named, and why the obvious answer is wrong

On the route capture the submit sub-counters that exist as CSV stats do move --
`SubmitAssetAppearanceMs` 103-160 ms, `SubmitAssetMarshalMs` 36-105 ms,
`AppearanceCanonicalMs` 72-116 ms, with `AppearanceInputCandidates` up to 539,867
on the worst row. But they account for roughly 200-380 ms of a 650-1,550 ms
bracket, and **on walk-capture-57 they are zero on every stall row while SubmitMs
still reads 394 ms.** So the appearance marshal is a passenger on some stalls and
absent on others. It is not the cause.

`VoxelStream/ChunksApplied` reads exactly **64** on many stall rows and 0 on
quiet ones -- a per-tick cap being hit -- but not on all of them (row 551 has
none), so it is a correlate rather than the mechanism.

## The instrument that finishes this already exists and has never been run here

`SubmitMs` is subdivided six ways on the per-frame sample -- `SubReqHdrMs`,
`SubBandMs`, `SubRasterMs`, `SubAssetsMs`, `SubPoolMs`, `SubMgrMs`, plus
`SubTotalMs` (the function-side wall, so `loopMinusFn` is checkable). The report
prints as `Voxel frame attribution SUBMIT-SPLIT`, gated on
`voxel.Stream.FrameAttribution` and at least 200 samples
(`VoxelWorldSubsystem.cpp:14954`). Mode **2** restricts it to settled-moving
frames, which is the population the goal is judged on.

No log in `asset-forge/out/ecological-placement/` carries a SUBMIT-SPLIT line:
the switch has never been on at this site. **The next step is one walk capture
with `-dpcvars=voxel.Stream.FrameAttribution=2`, zero code.**

The standing hypothesis it tests: `SubRasterMs` carries it, because the
raster-atlas page fill is synchronous on the game thread and lands inside this
bracket (`VoxelWorldSubsystem.cpp:7731` says so in the field's own comment, and
the 2026-08-25 flight investigation measured per-page fills at 2.7 ms with
per-window game-thread costs peaking at 385 ms). If `SubRasterMs` is small and
`SubAssetsMs` or `SubReqHdrMs` is large, that hypothesis is dead and the fix is a
different one -- which is the point of running it before building anything.

## Method

    python scratchpad/hitch.py  <frames.csv> 100   # distribution + hitch-vs-quiet column deltas
    python scratchpad/gtworst.py <frames.csv> 5    # per-row breakdown ranked by GameThreadTime
    python scratchpad/wait.py   <frames.csv>       # which thread waited, and on which row

Quiet baseline is frames at or under 25 ms; deltas are medians, never means,
because every counter here is bimodal.
