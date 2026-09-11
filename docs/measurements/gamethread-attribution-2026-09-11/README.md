# Where the game thread actually goes at the forest site

Capture: `asset-forge/out/ecological-placement/walk-capture-43-full256-tickscopes`
Binary: commit `662828f`, 23 newly-added tick scopes plus a GPU bracket on the ZTight reduce.
Site: temperate forest, spawn `-154740,-81476`, 256 m detail ring, cache
`temperate-authored-lods-full-13`, 1280x720 at `r.ScreenPercentage=65`.
541 frames. Receipt passed. No foreign GPU load (start guard and the during-run
guard both clean).

## The headline

**Half the named game thread is water simulation, at a site with no water in it.**

| scope | median ms | p95 ms | max ms |
|---|---|---|---|
| GameThreadTime | 21.80 | 74.95 | 400.77 |
| PawnTick | 4.45 | 11.32 | 64.45 |
| ├ MovementTick | 4.44 | 11.31 | 64.43 |
| └ └ CollisionPrepare | 3.83 | 7.24 | 17.34 |
| **OceanTick** | **4.16** | 6.96 | 15.23 |
| **RippleTick** | **4.10** | 6.32 | 16.07 |
| ClipmapTick | 2.84 | 4.96 | 12.72 |
| └ RoofProbe | 2.84 | 4.96 | 12.72 |
| WorldSubsystemTick | 0.37 | 19.45 | 371.06 |
| WaterSheetTick | 0.25 | 0.67 | 1.16 |
| DetailTick | 0.13 | 2.76 | 101.29 |
| RasterAtlasTick | 0.05 | 2.17 | 4.04 |

Adding the root scopes (not the nested ones) gives 16.34 ms of a 21.80 ms
median. Before this capture only 7.6 ms was named — 35%. It is now 75%.

`OceanTick` + `RippleTick` + `WaterSheetTick` = **8.51 ms, 39% of the game
thread**, at a forest column the engine's own log puts at **70.8 m of ground
over a 0.0 m sea level**. The sea is 75.8 m below the camera and behind solid
terrain in every direction.

Neither actor has any gate on water being near. From this capture's own log:

    Ocean: ring grid built -- 13 section(s), 40017 vertices, 74384 triangles,
    finest cell 1.50 m over +-96 m, reach +-135.2 km. The camera is followed
    by transform.

    RippleField: armed. 512x512 texels at 0.10 m -> a 51.2 m window (+/-25.6 m).
    Fixed dt 0.0167 s, speed 0.50 m/s ...
    RippleField: first simulation step ran. ... mask on, publishing to MPC yes.

A 74,384-triangle ocean mesh is being followed by transform and asked "is the
camera underwater?" every frame, and a 512x512 wave simulation is being stepped
at a fixed 60 Hz every frame, 75.8 m above the only sea within reach.

This is the largest single finding of the performance work so far, and it is
the first one that is a plain waste rather than a trade.

## The other quarter

5.46 ms of the 21.80 ms median is still unnamed. Two tick functions in the
module had no scope at all until this capture's follow-up
(`VoxelWalkTestSubsystem`, now scoped; `VoxelObjectRegistry` has no `Tick`).
The rest is engine-side actor tick dispatch, physics and the component update
pass, which these scopes cannot reach. Chasing it is lower value than the 8.51
ms above.

## ZTight: priced, and dead

`GPU/VoxelMarchZTight` — the reduce that dispatches one thread per chunk slot
(393,216 by default) every frame to produce 28 words:

| | ms |
|---|---|
| ZTight median | 0.050 |
| ZTight p95 | 0.066 |
| ZTight max | 0.101 |
| VoxelMarch median | 9.472 |

0.5% of the marcher timer. The kill threshold was written into the code before
the measurement was taken: *"If it reads below ~0.15 ms at the forest site,
that idea is dead and should not be built."* It reads 0.050 ms.

**Gating the ZTight dispatch on the index generation is dead.** It would have
added a correctness surface to the one pass in `VoxelMarchRenderer.cpp` that
can produce a hole, to buy at most 0.05 ms. The bracket stays in the source as
the record of why.

## A counter that lies, recorded so nobody else trusts it

While reading this capture I took `RHI/PrimitivesDrawn` = 268,092 as the
understory's triangle count and reasoned from it. **It is not.** Across 39
captures at this site that counter sits between 267,000 and 269,000 in every
single one — including captures with 4,386 resident understory instances and
captures with 126,979. It does not track the instanced ground cover at all.
Any "triangles per instance" or "nanoseconds per instance" figure derived from
it is void.

`GPUSceneInstanceCount` does not track the cost either, which is the more
useful half of the correction. The size-cull pair:

| capture | instances | base pass + velocity |
|---|---|---|
| 24, size cull OFF | 126,870 | 30.22 ms |
| 25, size cull ON | 125,428 | 8.86 ms |
| 26, size cull OFF | 126,455 | 29.80 ms |
| 27, size cull ON | 125,350 | 8.78 ms |

1% fewer instances, 3.4x less cost. `SetCullDistances` does not remove
instances from the component or from GPUScene; it changes which ones are
*drawn*. So the cost follows the drawn set, not the resident set.

## Method notes

`FrameTime` lags the thread columns by one CSV row (correlation +0.971 when
shifted, +0.203 unshifted), so thread medians are read from their own columns
and never inferred from `FrameTime`. GPU counter repeatability at this site is
0.7%; game thread is 2.8%. A 0.05 ms reading is therefore well inside the
instrument's resolution as a *bound*, which is all the kill decision needs.
