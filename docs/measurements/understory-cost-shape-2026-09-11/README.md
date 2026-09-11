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

---

# Addendum: what the two passes are actually made of

Engine-source reading, 2026-09-11, against UE 5.8. Everything below is
`file:line` traceable; the measurements above are unchanged.

## Why `RHI/PrimitivesDrawn` cannot see the ground cover

Every GPU-instance-culled draw goes through the indirect indexed-primitive path,
which on D3D12 forwards to the multi-draw variant and increments the draw-call
counter only, with no primitive accumulation. Only the non-indirect statistics
path adds `primitives × instances`. So the ground cover contributes **exactly
zero** to that counter at any instance count, and the 267k-269k is the
non-instanced remainder of the frame. The retraction above is now explained from
source rather than merely inferred from the data.

## Why size culling works, and where it takes effect

Not on the GPU. A hierarchical instanced component **opts out** of GPU
per-instance draw-distance culling: `GetInstanceDrawDistanceMinMax` returns
false unconditionally, with the comment *"HISM already does distance culling
before submitting instances"*. The plain instanced component returns true.

So `SetCullDistances` is consumed entirely on the CPU, in the cluster-tree
traversal, where whole subtrees are dropped before any run is emitted.
**A cull-distance-rejected instance never becomes a load-balancer item, never
gets a compute thread, and never reaches the vertex shader.** That is why the
3.4x arrives despite the resident instance count barely moving, and why
`GPUSceneInstanceCount` is blind to it.

## The velocity pass is a full, independent second submission

One instance-culling context is built per view and mesh pass, and the merge that
follows is **concatenation, not deduplication** — the only duplicate guard is on
output-pointer identity. Each instance is therefore loaded from GPUScene and
culled twice, writes into two disjoint instance-id regions, and is
vertex-shaded, wind offset included, twice. Nothing from the depth or base pass
is reused. That is why the velocity half equals the base pass half to within
0.2 ms.

## Mobility is not the problem; World Position Offset is

The dynamic classification in scene culling is one line: a primitive is dynamic
if it was already dynamic or if its mobility is Movable. It is also **sticky** —
once dynamic, always dynamic for that primitive's life. World Position Offset
does not enter that predicate at all.

But the dynamic bucket is not what costs the 29.6 ms. What puts these primitives
in the velocity pass every frame is the wind: the proxy's
world-position-offset-velocity flag is set when the component supports WPO
velocity, vertex deformation outputs velocity, and any material has WPO. Without
it the velocity pass would reject this component every frame, because it is
positioned once and never moved, and the pass explicitly drops primitives whose
local-to-world equals their previous local-to-world.

### The one-line change that follows

`bWorldPositionOffsetWritesVelocity` on the static mesh component, default true,
is the **per-component** version of `r.Velocity.EnableVertexDeformation`.
Setting it false on the understory components clears the proxy flag and drops
the entire second geometry submission — **scoped to the ground cover instead of
to the whole renderer**, which the cvar arm could not be.

Worth −14.9 ms alone at the unculled 256 m ring, or roughly −4.4 ms stacked on
top of size culling, since the velocity half scales with the drawn set. The
trade is unchanged and is still the owner's: foliage wind stops producing motion
vectors, so moving leaves smear under temporal upscaling.

**It is not free to implement.** `VoxelDetailAssetSubsystem.cpp` is one of the
seven files hashed into the detail cache identity, so any edit to it refuses
every existing cache and forces a full re-bake. That is the same toll the Nanite
material flag pays, so the two should be batched into one re-bake cycle.

## Static mobility: rejected, with reasons, so it is not tried

It would buy nothing and cost several things.

**Nothing**, because a hierarchical instanced component forces dynamic relevance
true and static relevance false unconditionally in `GetViewRelevance`, and its
static-element draw is a no-op outside runtime virtual texturing. Cached mesh
draw commands are not mobility-gated in the first place; this component class
opts out of eligibility regardless.

**Cost**, four ways. Scene culling would switch from the incremental
per-instance branch to full remove-and-re-add — a roughly 89,000-instance
spatial-hash rebuild per frame at this capture's p95 churn. Distance fields
would move these into the persistent layer, where partial updates fall off a
cliff above 1,024 modified bounds, against about 22,000 adds per frame. Cached
shadow maps would invalidate. And static mobility sets `HasStaticLighting()`,
which with lightmap UV generation off forces the dynamic path back anyway.

There is also a hard assert forbidding per-instance previous transforms on a
static-mobility instanced component, and the instanced component class sets
Movable in its own constructor. Movable is the engine's intended mobility here.

## `WorldPositionOffsetDisableDistance`: real, reaches the GPU, but small

Unlike draw distance, this one is **not** overridden by the hierarchical
component, so it does reach the instance culling shader and clears the
evaluate-wind flag per instance beyond the chosen radius. The vertex shader then
short-circuits the wind.

It removes per-vertex wind arithmetic only. The instance still passes culling,
still occupies an indirect-argument slot, still gets full vertex-shader
invocations and full triangle setup — **in both passes**, because the velocity
decision is made at primitive level and does not consult the per-instance flag.
Expect low single-digit percent. Worth stacking, not worth leading with.

Its falsifier is useful in its own right: if setting a disable distance moves
almost nothing, that confirms the cost is geometry front-end work rather than
shader arithmetic.

## What is still not determined

The triangle count per plant mesh, per LOD. The `DetailMeshLOD key=...
L0=Nverts/Ntris@screen` line only fires on the fresh build path, and every
capture in the table above loaded from cache, so it appears zero times. Until
that number exists, "subpixel triangle setup" is a well-supported hypothesis for
the 14.69 ms base pass and not a proven attribution. One bake leg produces it.
