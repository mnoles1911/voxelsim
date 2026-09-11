# What the understory's 29.6 ms is actually made of

Thirty-nine walk captures at the same temperate forest site, read together.
All at spawn `-154740,-81476`, 1280x720 offscreen, the same species set
(`full-forest-low-cover-1`). "base+vel" is `GPU/Basepass` + `GPU/RenderVelocities`,
the two passes that draw the ground cover.

| capture | ring m | instances | base+vel ms | marcher ms | GPU ms |
|---|---|---|---|---|---|
| walk-capture-2 | 48 | 4,386 | 2.15 | 7.61 | 11.50 |
| walk-capture-9 | 48 | 6,773 | 3.40 | 9.29 | 14.50 |
| walk-capture-21 | 48 | 7,208 | 1.80 | 9.14 | 12.68 |
| **walk-capture-24, size cull OFF** | 256 | 126,870 | **30.22** | 9.48 | 41.42 |
| **walk-capture-25, size cull ON** | 256 | 125,428 | **8.86** | 9.21 | 19.94 |
| **walk-capture-26, size cull OFF** | 256 | 126,455 | **29.80** | 9.24 | 40.74 |
| **walk-capture-27, size cull ON** | 256 | 125,350 | **8.78** | 9.05 | 19.54 |
| walk-capture-35, `r.ScreenPercentage=33` | 256 | 126,107 | 29.64 | **5.14** | 36.33 |
| walk-capture-42, velocity WPO off | 256 | 126,215 | **14.65** | 9.06 | 35.32 |
| walk-capture-43, current head | 256 | 126,979 | 29.58 | 9.47 | 40.48 |

Four things fall straight out of that table, and one of them is the whole game.

## 1. Size culling is worth 21 ms of GPU, and it is the largest lever in the project

Two matched pairs, taken a day apart, agree to within 0.5 ms: 30.22 → 8.86 and
29.80 → 8.78. **A 71% cut to the understory, and a 52% cut to the whole GPU
frame** (41.4 → 19.9 and 40.7 → 19.5).

Nothing else measured in this work is close. Nanite was +0.73 ms. The velocity
trade is −5.19 ms. Predictive prewarm is a streaming fix, not a frame-rate one.

**It costs nothing to turn on and it is already built.** `-VoxelDetailSizeCull`
sets per-instance draw distances from each plant's own size: a plant is drawn
out to `max(height, half its width) × 128 m per metre`, rounded up to a 16 m
bucket and clamped between 32 m and the configured ring. A 2 m shrub keeps the
full 256 m. A 30 cm tuft of grass drops to 48 m.

The only thing standing between the project and 21 ms is whether the owner
finds the result acceptable to look at. **That is a picture question, and the
pictures have never been shot.** Two pose-matched route captures are running
now to produce them.

## 2. It is not pixel-bound, and it is not the resident instance count either

Dropping `r.ScreenPercentage` to 33 gives 3.87x fewer shaded pixels and moves
the two passes by 0.2% (29.64 vs 29.58). The marcher, in the same capture,
falls from 9.47 to 5.14 — so the instrument works and the marcher *is*
pixel-bound. The understory is not.

Nor does it track `GPUSceneInstanceCount`. The size-cull pairs differ by about
1% in resident instances and by 3.4x in cost. `SetCullDistances` does not
remove instances from the component; it changes which ones get **drawn**. The
cost follows the drawn set.

Nor is it the ring's residency: 48 m holds ~6,800 instances and costs 3.40 ms,
256 m holds ~126,000 and costs 29.8 ms. That is 18.8x the instances for 8.8x
the cost — sublinear, because distant plants sit at cheaper LODs.

**Warning about one counter.** `RHI/PrimitivesDrawn` reads 267,000–269,000 in
every one of these 39 captures, from the 4,386-instance ones to the
126,979-instance ones. It does not see the instanced ground cover. Do not
reason from it.

## 3. The velocity pass is an exact duplicate of the base pass

`r.Velocity.EnableVertexDeformation=0` takes base+vel from 29.58 to 14.65 —
almost precisely half. The velocity half costs what the base pass half costs,
and it exists only to give TSR motion vectors for wind-blown foliage.

This is a visual trade and not a free win: without those vectors, moving
foliage smears under temporal upscaling. Per the settings-panel policy it is a
Settings row and the owner's verdict sets the default. The screenshots to judge
it have not been shot either.

**Size culling and the velocity trade do not overlap.** One removes drawn
instances; the other removes a whole pass over whatever remains. Taken
together the arithmetic suggests the understory would fall from 29.6 ms to
roughly 4.4 ms, though that product has not been measured and must not be
quoted as if it had.

## 4. The authored LOD chain already paid, and it is already on

At the default 48 m ring, captures 2 through 17 cost 3.38–3.41 ms for ~6,800
instances; captures 18 through 23 cost 1.80–2.22 ms for slightly *more*
instances — a 47% cut. The manifests differ by exactly one argument:
`-VoxelDetailMeshLOD`.

So the authored LOD chain is worth roughly half the understory cost, it has
been proven, and **it is already enabled in every 256 m capture in the table
above**, capture 43 included. The 29.6 ms is what remains *after* it. There is
no second helping of that win waiting to be collected.
