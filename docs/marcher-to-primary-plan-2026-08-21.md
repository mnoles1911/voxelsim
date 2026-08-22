# Ray marching to primary renderer — what is left, in waves

Date: 2026-08-21. Written after the marcher first rendered terrain into the real
frame (see `docs/measurements/armA-drawpath-ceiling-2026-08-19.txt`, entries of
this date). Supersedes the sequencing in `docs/phase5-flip-decision-memo.md`
§11, whose three named legs have now all been run.

---

## 0. Two things to correct before planning anything

### 0.1 "Delete the old method" cannot mean delete the quad renderer

**Water runs its own independent instances of the entire quad stack, one per
world-space bucket**, and terrain retirement was explicitly scoped never to
touch it (`VoxelBrickPool.cpp:134-137`; `VoxelWaterSubsystem.cpp:1997`, `:2026`,
`:2081`). There are up to five `UVoxelGpuPoolComponent`s in a live world and the
census is written down at `VoxelGI.cpp:440-446`.

So the deletable surface is **terrain's USE of the quad path**, not the path:

| Deletable when this is done | Must survive |
|---|---|
| terrain's `GetOrCreateGpuPool` and its mesh/scan/emit publication | `UVoxelGpuPoolComponent` |
| the terrain quad shadow path and `voxel.Stream.GPUShadowMaxLevel` | `FVoxelQuadVertexFactory`, `VoxelQuadDecode.ush` |
| terrain's cull walk / range emit and their instrumentation | the geometry suballocator |
| `VoxelQuadVertexFactory`'s terrain-only climate/biome/GI terms — **only after Wave 2** | water's per-bucket pools |

That is still the whole prize: the cull walk and range emit are ~4.2 ms of the
render thread and the mesh/scan/emit is the streaming bottleneck.

### 0.2 The state today, measured not assumed

- **It renders.** 58,154 colours / 9.23 MB at the acceptance pose against the
  quad path's 31,728 / 8.89 MB, sky and fog on.
- **The quad path is provably idle in that arm**: `retired=1`,
  `terrainPoolUsedQuads=0`, `quads=0`, no `VoxelTerrainPool cull` line at all,
  and ProfileGPU shows **PrePass 0 primitives / BasePass 960** against the quad
  path's historical 17,689,546 per pass. Negative control: marcher off + quads
  retired renders an **empty frame**.
- **p50 35.71 → 14.72 ms (2.43x). p95 50.03 → 43.42 ms (1.15x)**, two legs per
  arm.
- **Everything is off by default.** `voxel.March` 0, `voxel.GPU.BrickPack` 0,
  `voxel.Terrain.RetireQuads` 0, and `Config/*.ini` mentions none of them.

---

## Wave 1 — Correctness blockers. Nothing can ship until these land.

Each of these fails **silently**, which is why they go together: three silent
failures arriving at once is indistinguishable from the renderer being broken.

| # | Item | Evidence it is broken | Shape of the fix |
|---|---|---|---|
| 1.1 | **The marcher declines every frame without a fluid occupancy volume**, even under `Source 1` where it never reads that volume | `VoxelMarchRenderer.cpp:3592-3597` and `:3694-3699` — `if (!Volume.IsValid()) { DeclinedNoVolume++; return; }`, taken before any source check | gate the volume requirement on `Arm.Source == 0`. Small. **Until this lands, terrain rendering requires the water sim to be running with 40,000 particles** — which is why every leg in the archive carries `voxel.Fluid.Spawn 40000` |
| 1.2 | **The pawn waits for terrain forever under retirement** | `HoldsGeometry()` is `Component.IsValid() \|\| PoolSlot != INDEX_NONE` (`VoxelWorldSubsystem.cpp:1319`) — quad-valued. Under retirement every chunk reports no geometry and `VoxelCharacterMovement` never clears `bWaitingForTerrain` | a terrain-aware predicate that also answers true for brick residency. **Static-flight legs cannot catch this** — they pin the pose, which is why every leg so far has looked fine |
| 1.3 | **Nothing ever frees a terrain chunk from the brick pool** | `VoxelBrickPool.h:49-53`: *"nothing in the streaming path knows to free from it… hooking those four sites belongs with the phase that makes the marcher the draw path"*, and `:917` *"nothing in the streaming path calls RemoveChunk"* | hook eviction, park, unpark and unload. The pool is already at **94,096 / 131,072 chunks (72%)** at the standard pose, so this is not theoretical — it ends in eviction of near chunks, which reads as holes |
| 1.4 | **Edits reach the brick pool only when the marcher is already on** | the publish exists and is correct (`VoxelWorldSubsystem.cpp:14253` `VoxelBrickCpuArm::Publish`), but is gated on `voxel.GPU.BrickPack` (default 0) and `voxel.GPU.BrickPackResident` (`:1893`) | falls out of the default flip in Wave 5; listed so nobody re-derives it as missing. **Editing is NOT a gap** — I assumed it was and was wrong |

**Exit test for Wave 1:** a *non-static* leg — walk the pawn, dig, place, walk
away until the chunk unloads, walk back — with `voxel.March 1` and quads
retired, and no fluid spawn. Pawn moves, edits appear, pool count comes back
down.

---

## Wave 2 — Visual parity. This is what the owner will call "it looks wrong".

The marcher currently shades from a flat palette plus corner AO. The quad path
does considerably more, and none of it is ported.

| # | Missing term | Where the quad path does it | Consequence |
|---|---|---|---|
| 2.1 | **Biome tint and climate** | `VoxelQuadVertexFactory.ush:276-406` packs per-chunk climate and biome tint into vertex colour; `:276` is a mirror of `VoxelClimate::BiomeTintForFace` | marched terrain has **no biome colour at all**. Likely the largest visible delta in the A/B pair already sent |
| 2.2 | **Surface-gradient shading** | `VoxelQuadVertexFactory.ush:316`, a transcription of `VoxelClimate::UnpackSurfaceGradients` | flat-looking slopes |
| 2.3 | **GI is not applied** | quad path samples `VoxelGIVol.VolumePos/Neg` (`:723-737`) and local lights (`:814-817`); the marcher writes `EncodeIndirectIrradiance(1.0f)` — the neutral value (`VoxelMarch.usf:976`) | no indirect light on terrain. Note the GI **producer** already marches bricks (`VoxelGIMarch.usf`); only the consumer is missing |
| 2.4 | **GI volume anchor is a stand-in** | the volume's origin is *defined* as the terrain pool component's world location (`VoxelGI.cpp:533-547`), so retirement keeps a deliberately **empty** quad pool alive purely to be found (`VoxelWorldSubsystem.cpp:13344-13348`) | a quad pool that exists only as a coordinate. Must be replaced by a real origin before terrain's pool can be deleted |
| 2.5 | **Depth placement** | depth gate at the cascade, this pose: interior 27.98% disagreement vs a 9.88% control, max **1.276 voxels** | sub-2-voxel, so placement/LOD rather than coverage — but it is a FAIL and it is unexplained. **Must be re-run now the renderer draws**, and it can NEVER be run after the flip: the gate's reference is the raster path itself |

**Order note:** 2.5 before the flip, always. Everything else in this wave is a
look question and goes to the owner as A/B pairs, not as a verdict.

---

## Wave 3 — Shadows. Phase 4 of the original plan, never started.

| # | Item | State |
|---|---|---|
| 3.1 | **Marched terrain cannot cast into CSM** | structural: the emit runs at `PostRenderBasePassDeferred`, *after* shadow map rendering (`VoxelMarchRenderer.h:32`). It receives lighting; it cannot cast |
| 3.2 | **The marcher's own sun-shadow pass is measurement-only** | `voxel.Shadow.March` default 0; mode 1 = *"march to a scratch mask + stats. NOTHING VISIBLE CHANGES"*; mode 2 *"NOT BUILT"* (`VoxelShadowMarch.cpp:97-107`). Reach 64 m, level-0 only |
| 3.3 | **Non-voxel receivers** | characters, debris, agents, water and ribbons still need terrain shadows once terrain leaves the CSM — a shared shadow-march callable from their materials |

**A caveat that changes the urgency:** the quad path's shadow machinery may be
delivering nothing today. `VoxelGpuPoolComponent.cpp:927-944` records
`shadowGathers=0`, and `VoxelSkySubsystem.cpp:2171-2183` names the cause —
`DynamicShadowDistanceMovableLight` at 0 emits no ShadowDepths pass at all
unless `-VoxelShadowDistanceM=` is passed. **Measure whether terrain shadows
exist today before budgeting to replace them.** If the shipped game has never
had terrain shadows, this wave is a feature, not a regression to repair.

---

## Wave 4 — Assets in the volume (Phase 6)

| # | Item | State |
|---|---|---|
| 4.1 | **The asset stall is the blocker** | `vxc::GeneratedWorld::materialAt` (`generator.h:173-184`) at **288 µs/call** with assets against 0.16 without (~1,800x), no `terrainOnly` fast path. Assets alone cost one 78 s frame; assets **plus** fluid spawn stalls permanently. The identical defect was already fixed once in the level-0 mesher and never carried over |
| 4.2 | **Cover already publishes into the brick pool** | `VoxelDetailAssetSubsystem.cpp:1484` `AddChunkFromCpu` at level 7, and `:1624` `RemoveChunk` — the detail path is the **only** caller that frees, which is worth copying for 1.3 |
| 4.3 | HISM detail path retires once cover is proven in the volume | it uses no quad pool, so it is independent of the flip |

---

## Wave 5 — The flip, then the deletion

**5.1 Attribute the tail first.** p95 moved only 1.15x and max frames (~350 ms)
are unchanged in both arms. Hitch frames read `renderMs=4.7`,
`subsystemTickMs=0.01`, `dispatchMs/applyMs/remeshMs=0` and ~42 ms in
game-thread *"elsewhere"* — so the tail is neither the render thread nor any
voxel subsystem. This is now the largest unattributed frame-time item in the
project and it is not a rendering problem.

**5.2 Flip the defaults in ONE change:** `voxel.March 1`, `voxel.March.Source 1`,
`voxel.March.Rings 1`, `voxel.March.SkipLevels 2`, `voxel.GPU.BrickPack 1`,
`voxel.GPU.BrickPackResident 1`, `voxel.Terrain.RetireQuads 1` — together with
items 1.1, 1.2, 1.3 and 2.4, because each of those fails silently.

**5.3 Then delete**, in this order, each behind its own leg:
1. terrain's mesh/scan/emit dispatch in the streaming path
2. terrain's `GetOrCreateGpuPool` and the empty-pool GI stand-in (needs 2.4)
3. the terrain cull walk / range emit and `voxel.Stream.GPUShadowMaxLevel`
4. terrain-only terms in `VoxelQuadVertexFactory.ush` (needs Wave 2 ported)

**Never delete:** `UVoxelGpuPoolComponent`, `FVoxelQuadVertexFactory`,
`VoxelQuadDecode.ush`, the suballocator. Water owns them.

---

## Known debts to accept or schedule explicitly

- **HZB**: built before the marcher's depth lands, so Lumen screen traces and
  SSR accelerate against an HZB with no terrain and overshoot
  (`VoxelMarchRenderer.h:169-175`). The seam's one genuine quality debt.
- **Decals**: structurally unreachable at this hook — `DBufferA/B/C` live in
  `Renderer/Private` and `ESceneTextureSetupMode` has no DBuffer flag
  (`VoxelMarch.usf:113-128`). **Mitigated by fact**: this project places no
  decals at all, and the only decal calls in the tree are
  `SetReceivesDecals(false)`. Schedule only if decals are ever wanted.
- **Collision is a non-issue.** It is voxel-query based against
  `IsSolidAtVoxel` and the quad pool has had `NoCollision` since construction
  (`VoxelGpuPoolComponent.cpp:2578`, `VoxelCharacterMovement.h:19-23`).
- **Two instruments are broken**: `voxel.March.VerifySource`'s readback never
  lands, and `voxel.Cover.VerifyStore` crashes on a standalone `FRDGBuilder`.
  Both are diagnostics, neither blocks, but the VerifySource one is the tool
  that would answer per-ring questions.

---

## The honest shape of the remaining work

**Wave 1 is small and mandatory** — four contained changes, one non-static leg.
**Wave 2 is the real work** and it is shading, not rendering: biome tint,
climate, surface gradients and GI all have to move from a vertex factory into a
pixel shader that has strictly better information to do it with (the exact
voxel, face and material, per ADR-0008). **Wave 3 may be smaller than it looks**
if terrain shadows are not currently being delivered at all — measure before
budgeting. **Wave 4 is gated on one ~1,800x function**, already diagnosed and
already fixed once elsewhere.

The renderer is no longer the risk. The remaining risk is everything that was
attached to the renderer.

---

## Wave 6 — water off the quad pool (added 2026-08-21, owner's question)

**Verdict: yes, and water is arguably a BETTER fit for marching than terrain was
— but it cannot use the same pass, and that distinction is the whole design.**

This is a RENDERING change only. It does not reopen the water architecture:
scalar field stays the authority, particles stay the presentation
(`docs/water-rearchitecture-plan-2026-08-09.md`). Nothing here proposes SWE or
CA for rivers.

### 6.1 Half of water is already off the quad path

There are two water renderers, and only one of them is the target:

| | what it draws | how |
|---|---|---|
| `VoxelWaterSubsystem` | the scalar/baked water surface (lakes, rivers, the implicit disc) | **translucent quads**, `M_WaterVoxel`, through per-bucket `UVoxelGpuPoolComponent`s |
| `VoxelFluidRender` | the PBF particles | **already a screen-space GPU pass**, composited post-opaque at `PrePostProcessPass_RenderThread` — no quad pool, no material, no sort key |

So the project has already built and shipped the exact shape this proposal
needs, for the particle half. `VoxelFluidRender.h:6-26` is the precedent and it
already argues the case: the alternative it rejected was *"a translucent
material on a screen quad … would re-enter the very material/sort-key machinery
whose hazards the water material documents."*

### 6.2 The strongest argument, and it is not performance

**Marching makes the sorting problem disappear by construction.**

The per-bucket split exists solely because UE sorts translucent draws by
`PrimitiveBounds[...].BoxSphereBounds.Origin` — one value per primitive — so
*"a single water primitive genuinely cannot sort its own contents against itself
… Splitting the primitive is the only lever the renderer offers short of an OIT
path"* (`VoxelWaterSubsystem.cpp:1890-1909`). The bucket size (64 bricks) is
reverse-engineered from the largest water body the system produces.

A ray gives hits in **exact front-to-back order along the ray**, per pixel. There
is nothing to sort. The buckets, the sort-key hazard, the 4-8 primitives and the
64-brick tuning all cease to exist — that IS the OIT path the comment says is
the alternative.

### 6.3 Why it cannot be the same pass as terrain

The terrain marcher is an **opaque** architecture: it writes SceneDepth in a
pre-base-pass depth pass and a GBuffer at `PostRenderBasePassDeferred`, and the
engine's deferred lighting shades it. Water must do none of that — writing
opaque depth would occlude everything behind the surface, which is exactly what
you can see through.

Water marches as its **own pass at the post-opaque hook**, where
`VoxelFluidRender` already lives:

- reads the terrain's final opaque depth (which the marcher now writes) as the
  ray's far bound — the same `t_max` seeding the terrain march already does
- marches the water volume front-to-back, accumulating absorption/thickness
- composites into SceneColor with refraction, reading what is behind it

**Shared: the volume, the brick traversal, the index, the tile classification.
Not shared: the pass, the hook, the output.** Water becomes a third source
alongside terrain and cover, exactly as ground cover already is a second one at
level 7.

### 6.4 What it unlocks

- `UVoxelGpuPoolComponent`, `FVoxelQuadVertexFactory`, `VoxelQuadDecode.ush` and
  the suballocator become **genuinely deletable** — §0.1's "must survive" column
  empties. That is the difference between retiring the quad path and deleting it.
- The two water renderers can then **merge**: a marched scalar surface and a
  screen-space particle surface are both post-opaque screen-space passes over
  the same authority. That is the real streamlining prize, larger than the pool
  removal.
- Water inherits empty-space skipping and the ring cascade for free.

### 6.5 What I cannot tell you yet — the cost

**There is no measured frame cost for the water pool anywhere in the repo.** The
plan's §0.1b assigns it a share of the surviving ~9.5 ms render thread but never
isolates it. So "less cost" is plausible and unproven, and the honest reasons to
expect a win are structural rather than measured: 4-8 fewer translucent
primitives, no sort key, no second cull/emit, and water covers a small fraction
of screen so a tile-classified march is cheap where a full-screen terrain march
is 3.5 ms.

**The cheap measurement, before any code:** one leg at a water-heavy pose with
the water pool's draw suppressed, against the same pose unsuppressed — the same
arm-B trick Phase 0 used on terrain (`voxel.Stream.PoolDrawEnable`). That gives
the ceiling on the prize in one leg and costs nothing to write.

### 6.6 Where it sits

**After Waves 1 and 2, not instead of them.** Water on the quad path is not
blocking terrain, and terrain's shading gaps (biome tint, climate, GI) are the
thing the owner can currently see. Do the measurement in 6.5 opportunistically —
it is one leg and it decides whether Wave 6 is a performance item or purely an
architectural one.
