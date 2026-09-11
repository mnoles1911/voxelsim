# The wave simulation was never the cost

`walk-capture-44-watersubscopes`, 532 frames, temperate forest at
`-154740,-81476`, 256 m ring. Quiet stretch only (frames 0–199, the settled
regime), so nothing here is hitch noise. Receipt passed.

Sub-scopes nested inside the two water ticks, so the parents read the same
totals and the children partition them.

| scope | median ms | p95 | max | share of parent |
|---|---|---|---|---|
| **OceanTick** | **4.077** | 4.548 | 5.717 | |
| ├ OceanFollow | 0.001 | 0.001 | 0.002 | 0% |
| └ **OceanUnderwater** | **4.075** | 4.547 | 5.715 | **100%** |
| **RippleTick** | **3.989** | 4.325 | 6.014 | |
| ├ **RippleAutoWatch** | **3.917** | 4.229 | 5.924 | **98%** |
| ├ RippleStep | 0.069 | 0.091 | 0.205 | 2% |
| ├ RipplePublish | 0.002 | 0.003 | 0.005 | 0% |
| └ RippleHealth | 0.000 | 0.000 | 0.001 | 0% |

Both parents reproduced against capture 43 on the previous binary: 4.118 →
4.077 and 4.133 → 3.989, inside and just outside the 2.8% game-thread
repeatability respectively. Game thread 21.549 → 21.096.

## What this overturns

The earlier record said a 512×512 wave field stepped at a fixed 60 Hz was
costing 4.1 ms at a site with no water. **The step measures 0.069 ms.** The
simulation is nearly free and was never the problem. The plan I had drafted —
skip the step while the field is provably zero — would have bought 0.069 ms and
is now dead.

98% of the ripple tick is `AutoWatch`, the watcher that polls the pawn and every
watched actor each frame to notice the moment something enters water.

## And both ticks are the same function

`AutoWatch` calls `UVoxelWaterSubsystem::IsUnderwaterAtWorld` for the pawn and
again for each watched actor. `AVoxelOceanActor::UpdateUnderwaterState` calls
the same function for the camera. The ocean's follow transform — a 74,384
triangle mesh moved under the camera every frame — costs **0.001 ms**, so the
mesh was never the problem either.

**Two calls a frame, 8.0 ms, 38% of a 21.1 ms game thread**, at a column the
engine's own log puts at 70.8 m of ground over a 0.0 m sea level.

## The fix was an identity, and it was still a null

**Superseded within the hour by
`docs/measurements/underwater-fastpath-null-2026-09-11/`.** The short circuit
below is correct reasoning and it removed the wrong half. It engaged on 12 of 12
calls a frame and moved the game thread 0.093 ms, because the sub-scopes shipped
with it showed `UnderwaterFillMs` at 8.088 ms against `UnderwaterSurfaceMs` at
**0.013 ms**. The worldgen ground sample was never the cost either. The arm is
retired; the split inside `GetWaterFillAtWorld` is the live question.

## What the fix was, kept for the record

`IsOpenSeaNowAtWorld` is exactly

    WorldZ < SeaZ && GroundZ < SeaZ && WorldZ >= GroundZ

When `WorldZ >= SeaZ` the first term is already false, so the conjunction is
false whatever the ground turns out to be. **The ground is not an input to the
answer in that half-space**, and declining to compute it cannot change the
answer. That is algebra, not a tolerance, so it defaults on and carries no
visual verdict. `voxel.Water.UnderwaterFastPath 0` restores the original call
order exactly and exists to be the control arm on one binary.

## The 200× spread that is not yet explained

The same function measures **0.020 ms** called from
`UVoxelCharacterMovementComponent::IsInWaterAt` and **4.075 ms** called from the
ocean actor, in the same frame, for positions in the same column. A 200×
difference between two callers of one function is not something to infer a cause
for, so sub-scopes went inside the function in the same build:
`UnderwaterFillMs` against `UnderwaterSurfaceMs`, plus a call count and a
fast-out count. Whichever half holds the time, the next capture names it — which
means a null on the fast path is still a result.

The leading candidates, unranked because the measurement is cheap and in flight:
the amplifier's thread-local column memo being warm for one caller and cold for
the other, or the implicit-field read inside `GetWaterFillAtWorld` depending on
the Z the caller passes.

## Why this matters now

Size culling took the GPU from 34.77 to 20.02 ms and left the game thread
untouched at 28.88 ms, so the slice is game-thread bound from here. This is the
largest single item on that thread and it is not a trade: nothing is lost by not
computing a number that cannot affect the answer.
