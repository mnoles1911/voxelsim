# G2 — how the GPU-resident geometry actually gets drawn

The architecture decision for ADR-0006 G2, made 2026-07-25 after reading the
engine source rather than reasoning from the docs. **This corrects a G0 finding
that was wrong in a way that would have cost real time.**

## The correction: Landscape is not the precedent we thought

`docs/gpu-g0-sizing.md` §6 and the handoff both said, roughly, *"build on the
Landscape pattern — it is the shipping precedent for ADR-0006 invariant 2."*

That is only half right, and the wrong half is the load-bearing one.

**Landscape registers one `FPrimitiveSceneProxy` per `ULandscapeComponent`** —
i.e. one primitive per terrain section. Streaming a section in or out *does* go
through `AddPrimitive`/`RemovePrimitive`. It is a many-proxies system.

What Landscape genuinely is a precedent for is narrower but still valuable:
**swapping indirect draw arguments per view on an already-registered primitive**,
via `bViewDependentArguments` + `ApplyViewDependentMeshArguments`
(`LandscapeRender.cpp:3030`). That is a real answer to "how do I change what
gets drawn without touching `FScene`" — it is just not an answer to "how do I
hold N chunks under one primitive."

Taking the original note at face value would have meant modelling the pool on a
system that solves a different problem.

**The closer structural templates:**

| System | What it actually does | Relevance |
|---|---|---|
| `FGeometryCollectionSceneProxy` | One proxy, one shared geometry pool, many pieces appearing/disappearing by rewriting buffer contents | **Closest structural match** |
| Niagara GPU mesh/sprite | One proxy per emitter, arbitrary streaming instance count, rebuilds its `FMeshBatch` every frame with fresh indirect args | **Closest behavioural match** |
| Nanite page pool | Persistent GPU pool, streaming touches only pool contents, visibility resolved GPU-side per frame | Validates the architecture; not reusable code (editor-only builder) |
| Landscape | One primitive per section; per-view indirect arg swap within a section | Precedent for the arg swap only |

## The decision

**Start on dynamic relevance (the Niagara shape), not static relevance.**

One proxy. `GetDynamicMeshElements` builds one `FMeshBatch` per view per frame
with `NumPrimitives = 0` and an `IndirectArgsBuffer` pointing at that frame's
args. Streaming a chunk in or out writes into the pool and the chunk-metadata
buffer, and nothing else.

Why not static relevance first, given ADR-0006 is a frame-time programme?

Because **there is no supported way to change how much geometry a cached static
batch draws.** `FMeshBatchElement::NumPrimitives` is baked into the cached
`FMeshDrawCommand`; changing it requires `RequestStaticMeshUpdate()`, which
drops and re-caches that primitive's draw commands
(`RendererScene.cpp:6555`). Far cheaper than `AddPrimitive` — it never touches
the octree, bounds arrays, or light interactions — but not free, and doing it
every frame for a streaming pool gives back exactly what static relevance was
supposed to buy.

Dynamic relevance sidesteps the entire question: nothing is cached, so nothing
needs invalidating, and a per-frame changing draw count is the *expected* case
rather than a fight with the caching layer. Niagara ships this shape for
particle counts that change every single frame.

**The upgrade path is real and does not require rework.** If profiling later
shows the per-frame `AddMeshBatch` cost matters (many mesh passes × many
views), move to static relevance + `bViewDependentArguments` +
`ApplyViewDependentMeshArguments`, which gets the command cached exactly once
while the GPU buffer contents still change freely. The pool, the metadata
buffer, the culling pass, and the vertex factory are all unchanged by that
switch — only the relevance flags and where the `FMeshBatch` is built move.

So: take the simpler thing that is known to work, and keep the faster thing
available. Do not pay for static-relevance complexity before measuring that the
dynamic path is the bottleneck.

## Confirmed constraints (verified in 5.8 source, do not re-derive)

1. **`NumPrimitives == 0` is the documented signal** to use `IndirectArgsBuffer`
   — `MeshBatch.h:272` says so outright, and `SubmitDrawIndirectEnd`
   (`MeshPassProcessor.cpp:1408`) branches on exactly that.
2. **One indirect draw per mesh draw command, confirmed.**
   `RHIMultiDrawIndexedPrimitiveIndirect` exists but the renderer never calls
   it with `MaxDrawArguments > 1`; D3D12 implements the single-draw entry point
   on top of it with `MaxDrawArguments = 1` (`D3D12Commands.cpp:1322`).
   **Consequence, unchanged from G0: the pool must compact into ONE contiguous
   draw range per frame per pass, not one draw per chunk.** This is the single
   most important constraint on the pool layout and the culling pass.
3. **Static-mesh command caching requires exactly one `FMeshBatchElement`**
   (`PrimitiveSceneProxy.cpp:164`) and excludes any batch with
   `bViewDependentArguments` set — relevant only if we take the upgrade path.
4. **Persistent buffers are `TRefCountPtr<FRDGPooledBuffer>` members**,
   created once with `ConvertToExternalBuffer`, brought into each frame's graph
   with `RegisterExternalBuffer`, resized with `ResizeStructuredBufferIfNeeded`.
   This is what `FGPUScene` itself does for its primitive/instance buffers
   (`GPUScene.cpp:963`), so it is the authoritative pattern.
5. **RDG inserts the compute→draw barrier automatically** as long as both the
   writing pass and the draw declare the same `FRDGBufferRef` in their pass
   parameters. Hand-rolled transitions are only needed for buffers touched
   outside RDG.
6. **`GetTypeHash()` must be a static local per proxy CLASS**, not per instance
   — getting it wrong silently corrupts state-bucket/PSO caching.

## What is already built toward this

- **Pool suballocator** — `FVoxelGpuGeometryPool`, first-fit over a coalesced
  free list, contiguous allocations so a chunk is one draw range. Tested
  headlessly (`VoxelEarth.GpuPool.*`), including a 20k-step soak that asserts
  pairwise disjointness of every live allocation.
  - **Measured:** ~10% of allocations refused by fragmentation at 64k-quad
    capacity with realistic 600–2400 quad chunks. The live pool is ~9.4M quads
    so the ratio should fall, but **G3 needs a compaction or eviction policy**
    and this is the evidence. Watch `GetLargestFreeRun()` vs `GetFreeQuads()`.
- **Quad decode** — `VoxelQuadDecode.ush`, verified against a CPU reference over
  real quad streams (39,996 vertices, exact, no epsilon). This is the function
  the vertex factory will call.
- **The kernels themselves** — G2a, bit-exact in-engine, digest
  `f3c48a4df3e20e9a` matching the standalone bench.

## The vertex factory recipe (researched 2026-07-25, do not re-derive)

**Template: `FLidarPointCloudVertexFactory`**
(`Engine/Plugins/Enterprise/LidarPointCloud/.../LidarPointCloudRenderBuffers.h/.cpp`
+ `Shaders/Private/LidarPointCloudVertexFactory.ush`). It reads points from an
SRV keyed purely by `SV_VertexID` and expands each into a camera-facing quad in
the vertex shader — structurally the same thing we need, and much smaller than
Water/Landscape/Nanite.

- **`SV_VertexID` arrives as a member of `FVertexFactoryInput`**:
  `uint VertexId : SV_VertexID;`. Then `QuadIndex = VertexId / 6`,
  `Corner = VertexId % 6`.
- **Do NOT use `MANUAL_VERTEX_FETCH` or `SupportsManualVertexFetch`.** That is a
  narrower `FLocalVertexFactory` mechanism for fetching *classic* static-mesh
  attributes from SRVs. Lidar sets neither; it just indexes its own bound
  buffer. Same for us.
- **An empty vertex declaration is legal and idiomatic.** Call
  `InitDeclaration(FVertexDeclarationElementList())` — `InitDeclaration` forwards
  to `GetOrCreateVertexDeclaration` with no minimum-element assert, the same
  object `GEmptyVertexDeclaration` is. Nanite's own voxel raster does this
  (`Nanite/Voxel.cpp:371`). No dummy stream needed.
- **Drawing with no index buffer:** `FMeshBatchElement::IndexBuffer = nullptr`,
  `FirstIndex` = starting vertex, `NumPrimitives = NumQuads * 2`,
  `NumInstances = 1`. `SubmitDrawEnd` (`MeshPassProcessor.cpp:1346`) branches on
  the null index buffer straight to `RHICmdList.DrawPrimitive`.
- **Minimal flags — everything else costs an extra `.ush` hook:**
  ```cpp
  IMPLEMENT_VERTEX_FACTORY_TYPE(FVoxelQuadVertexFactory,
      "/VoxelEarth/VoxelQuadVertexFactory.ush",
        EVertexFactoryFlags::UsedWithMaterials
      | EVertexFactoryFlags::SupportsDynamicLighting);
  ```
  Omit `SupportsStaticLighting` (procedural geometry is never lightmapped),
  `SupportsPrimitiveIdStream` (no GPUScene — use the per-draw primitive uniform
  buffer via `FMeshBatchElement::PrimitiveUniformBuffer`), and
  `SupportsPositionOnly` unless we also write the position-only input structs.
- **Bind the quad SRV through a uniform buffer**, built once in the factory's
  `InitRHI` with `SHADER_PARAMETER_SRV`, and added in `GetElementShaderBindings`.
  Preferred over a loose `FShaderResourceParameter` because a stable
  `FRHIUniformBuffer*` stays hashable for draw-command caching.
- **Every `.ush` function the compiler demands** is enumerated in the Lidar
  file; the mandatory set is `FVertexFactoryInput` (with `VertexId` and the
  stereo/multiview blocks), `FVertexFactoryIntermediates` (must contain
  `FSceneDataIntermediates SceneData`), `FVertexFactoryInterpolantsVSToPS`,
  `GetVertexFactoryIntermediates`, `VertexFactoryGetInterpolantsVSToPS`,
  `VertexFactoryGetTangentToLocal`, `VertexFactoryGetWorldPosition`,
  `VertexFactoryGetInstanceSpacePosition`,
  `VertexFactoryGetRasterizedWorldPosition`,
  `VertexFactoryGetPositionForVertexLighting`, `VertexFactoryGetWorldNormal`,
  `VertexFactoryGetPreviousWorldPosition`,
  `VertexFactoryGetPreviousInstanceSpacePosition`,
  `GetMaterialVertexParameters`, `GetMaterialPixelParameters`,
  `VertexFactoryGetTranslatedPrimitiveVolumeBounds`,
  `VertexFactoryGetPrimitiveId`, and finally
  `#include "/Engine/Private/VertexFactoryDefaultInterface.ush"` **last**.

**The one open decision — stopping material compilation from exploding.**
`ShouldCompilePermutation` normally gates on an engine-defined per-material
usage flag (`bIsUsedWithWater`, `bIsUsedWithLidarPointCloud`, …) so only
materials the artist ticked compile against the factory. `FLocalVertexFactory`
has no such gate, which is exactly why it is compiled against nearly every
material in a project. Those flags are a fixed engine enum, so the choices are:

1. Gate on `MaterialDomain == MD_Surface || bIsSpecialEngineMaterial` and rely
   on only a handful of materials ever being assigned to voxel components.
   Bounded in practice, honest, zero engine changes. **Start here.**
2. Add a real `bUsedWithVoxelTerrain` usage flag in engine source (we have the
   tree at `D:\UE_5.8`). Clean and mirrors what Epic did for Water/LiDAR, but
   it is an engine modification with all that implies for upgrades.

Always OR in `bIsSpecialEngineMaterial`, or the default-material fallback fails
to compile against the factory.

## BLOCKER FOR POOLING: `GetElementShaderBindings` is never invoked

Found 2026-07-25, empirically, and it constrains how the pool can work.

The factory renders correctly — but its
`FVertexFactoryShaderParameters::GetElementShaderBindings` **is never called**.
Proven twice over:

1. A one-shot `UE_LOG` at the top of it never fires, in any run.
2. Moving the per-chunk framing (`ChunkOriginUU`, `LevelScale`) out of the
   uniform buffer and into loose parameters bound there made the chunk vanish
   — `LevelScale` stayed 0, collapsing every vertex to a point. Moving it back
   into the uniform buffer restored rendering immediately.

What *does* work is the factory's uniform buffer
(`IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT` + `SHADER_PARAMETER_SRV`), which is
bound without that hook.

**Why this matters:** per-element bindings are the natural way to give one
vertex factory many chunks — each chunk's quads are contiguous in the pool, so
each draw needs only its own origin and mip scale from
`FMeshBatchElement::UserData`. With that hook dead, one factory currently
serves exactly one chunk, which means one primitive per chunk — i.e. **the
`AddPrimitive` funnel ADR-0006 exists to remove is still there.**

Likely cause, per the engine trace: `FMeshMaterialShader`'s
`VertexFactoryParameters` is null, so `ShaderBaseClasses.cpp:494-502` skips the
call with no `else`, no `ensure`, and no log. It is created by
`VertexFactoryType->CreateShaderParameters(frequency, ParameterMap)`, which
returns null unless `TVertexFactoryParameterTraits` is specialised for that
`(type, frequency)` pair — i.e. unless `IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE`
took effect for the frequency being drawn. Ours declares `SF_Vertex` only.

**TRIED AND DID NOT FIX IT (2026-07-25):** registering `SF_Pixel` alongside
`SF_Vertex` via a second `IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE`. The hook
still never fires. The registration is kept because it is more correct
regardless, but the cause lies elsewhere -- so start the next attempt by
confirming the runtime `VertexFactory->GetType()` is the same type the macro
was applied to, and by breakpointing `CreateShaderParameters`.

**Original next step (superseded):** register `SF_Pixel` as well (and confirm against
`FLidarPointCloudVertexFactoryBase`, which declares `SF_Vertex` plus
compute/ray-hit-group), or confirm the runtime `VertexFactory->GetType()`
matches the type the macro was applied to. Until it resolves, the pool cannot
have per-chunk framing and there is no point building it.

**Three ways round it if the hook stays dead**, cheapest first:
1. A `StructuredBuffer` of per-chunk origins plus a per-quad chunk id
   (+4 bytes/quad on top of 8 — G0 established VRAM is not the constraint).
2. Bake the chunk offset into the quad coordinates at upload, as the brick
   re-basing already does — limited by the uint8 fields to ~25 m of range.
3. One uniform buffer per chunk, allocated per frame via
   `Collector.AllocateOneFrameResource` — still needs a working bind hook, so
   only viable if the cause turns out to be frequency registration.

## What is left for G2

1. A custom vertex factory doing manual vertex fetch from the quad pool via
   `SV_VertexID` (6 vertices per quad, no index buffer) — recipe above.
2. The proxy + component, dynamic relevance, one `FMeshBatch` per view.
3. A culling/compaction compute pass writing `FRHIDrawIndexedIndirectParameters`.
4. **The gate: a single chunk renders identically to the CPU-meshed component**
   — a visual A/B, and the one part of this that genuinely needs eyes.
