# G4 — pooled-path parity with the component renderer

Rewritten 2026-07-25 (second pass). The first version of this document was
written from reading the CPU path and got the shape of the remaining work
substantially wrong in three ways. Corrections first, because they change what
should be built and in what order.

## Correction 1: the "material gate" was mostly not real

The previous version opened with: *"every remaining item needs a material graph
change ... no remaining parity item can be delivered by C++ alone."* That is
false for two of the four items, including the largest one.

The pooled vertex factory owns **both ends** of the vertex-colour pipe:
`VoxelQuadVertexFactory.ush` writes `Intermediates.Color` on the VS side and
`Result.VertexColor` in `GetMaterialPixelParameters` on the PS side. The
material computes `BaseColor = albedo * VertexColor.G * DebugTint`
(`Tools/create_voxel_material.py`), so **anything that can be expressed as a
vertex colour channel is already an interpolant the graph reads**, at either
vertex or pixel frequency, with `M_VoxelTerrain.uasset` untouched.

That covers:

- **Biome tint (vertex colour R)** — done, see below. No material change.
- **Voxel GI (vertex colour G)** — the volume-texture design samples in the
  factory and folds the result into `.g`. No material change.

It genuinely does apply to ring cross-fade (which drives OpacityMask through
named scalar parameters) and to debug tints (a named vector parameter). Both are
off by default; see below for why neither is urgent.

**Consequence: the risky step this programme was organised around — regenerating
a binary asset the shipping renderer draws with — is not on the critical path
for anything. Do not do it "while we're here".**

## Correction 2: the item that actually mattered was not on the list

Vertex colour **R** drives the biome tint, and the pooled factory computed it as
a bare `Axis == 2 && positive` sky-facing flag. That is neither half of what
`BuildChunkVertexData` does, and both differences were visible in ordinary play,
behind no cvar at all:

- **Side faces.** `VoxelClimate::BiomeTintForFace` returns **140/255, not 0**,
  for any non-Z face — "the riser of a 10 cm step reads as part turf, part
  exposed soil. A hard 0 stripes every hillside at voxel pitch." The factory
  returned 0, so every vertical wall in the world lost its partial biome blend.
  By area this was the larger of the two.
- **Cave floors.** A cave floor is a +Z face too. The CPU path gates the tint on
  the vertex being within 200 UU of the chunk's surface height; the factory had
  no gate, so underground +Z faces tinted as open sky. Its own comment predicted
  this and it was never followed up.

Measured, 30 s headless run at a fixed anchor, pooled against component:

| | pixels differing > 8/255 |
|---|---|
| before | **17.4%** |
| after | **4.3%** |
| same-path repeat-run noise floor | 1.1% |

**Fixed.** The surface height rides in a widened chunk table: `ChunkClimate`
(`float2`) became `ChunkParams` (`float4`) — `xy` climate, `z` surface height,
`w` spare. `z` is stored **relative to the chunk origin** so no float32
precision is spent on this world's 84 km offset (the same reasoning as the
rebase origin, `gpu-pool-rendering-notes.md` invariant 4).

The `w` channel is free and is exactly where per-chunk debug tints want to live,
so item 3 below no longer needs a table change either.

## Correction 3: ring cross-fade is off by default AND known-broken

The previous version called this "the only remaining item that affects how the
game looks in normal play" and put it first. It affects nothing in normal play:
`RingCrossFadeEnabled()` requires `-VoxelRingCrossFade` on the command line and
defaults to **off**, because the cross-dissolve was the dominant cause of the
see-through-ring bug — abutting annuli meant each ring faded to transparent over
ground only it covered.

Better still, the pooled path already matches it **by construction**: the C++
inert sentinels (`kInertLow*`/`kInertHigh*`) are the same values as the
material's own parameter defaults, so a pooled chunk with no material instance
gets bit-identical opacity behaviour to a component chunk with an inert one.

The ring-seam padding fix (`+e/sqrt(2)`, `status.md`) has since given the annuli
the overlap band the fade always needed, so re-testing the fade is now
*worthwhile* — but it is a rendering experiment, not a parity obligation, and it
is not a G5 blocker. **Nothing in G5 depends on it.**

## Correction 4: under `voxel.Stream.GPU`, the GI light field is empty

Not previously recorded anywhere. `UVoxelGISubsystem::NotifyChunkMeshUpdated` is
called from exactly one site — `UVoxelChunkComponent::SetChunkQuads` — and the
pooled streaming branch returns before any `UVoxelChunkComponent` exists. So
with GPU streaming on, no chunk is ever voxelized into the field, no brick is
ever solved, and `voxel.GI.Enabled 1` is a silent no-op rather than a visible
difference.

This is step 0 of the GI work: without it, every subsequent GI verification
would compare an empty field against an empty field and pass.

## Where parity stands now

Fixed this pass:

- Vertex colour R: side-face tint + surface-proximity gate (above).
- `bVelocityRelevance` — the pooled proxy never set it, so pooled terrain
  contributed nothing to the velocity pass and TSR reprojected it from depth
  alone. The factory already implemented `VertexFactoryGetPreviousWorldPosition`;
  only the flag asking for it was missing.
- `bUsesLightingChannels`, `bRenderCustomDepth` — never set on the pooled proxy.
- Editor **Wireframe view mode** — pooled terrain was the one thing in the level
  that stayed solid in wireframe.

Known and remaining, in the order they should be done:

### 1. GI light field as a GPU volume texture

The largest remaining piece, and worth more than parity: it decouples lighting
from re-meshing, which a dig currently forces. Full design in
`docs/gpu-gi-volume-design.md` — sizing, upload path, precision, staging, and
the traps. Headlines:

- No material change (correction 1).
- Sample **per pixel**, not per vertex. The CPU path compensates for per-vertex
  GI by subdividing greedy quads; the pooled path structurally cannot do that
  (6 vertices per quad from `SV_VertexID`), so per-vertex on the pool is
  strictly worse than per-vertex on the component path. Per-pixel is both
  simpler here and better than what it replaces.
- Compute the volume coordinate in **pool-primitive space** — never reconstruct
  a world position in the shader.
- Step 0 is correction 4 above.

### 2. Per-chunk debug tints

Debug-only, and the storage question is already answered: `ChunkParams.w` is
free. Still needs the material graph to read an interpolant instead of the
`DebugTint` vector parameter, so this is the one item where the asset question
genuinely returns — and it is the one item whose absence costs nothing in play.

### 3. Ring cross-fade

See correction 3. Re-test it as a rendering experiment now that the rings
overlap. If it is kept, it needs the same material treatment as item 2.

### 4. Water surface pool

**Built, behind `voxel.Water.GPU` (default off). Full write-up in
`docs/gpu-water-pool-design.md`.**

"Near-identical" held, and by more than expected: the vertex factory, the quad
packing and the decode are shared **unchanged**, because `M_WaterVoxel`'s only
geometry input is `VertexColor.G` (AO) and the factory already writes it there.
So this is a second *instance* of `UVoxelGpuPoolComponent` parameterised by
three setters, not a parallel copy.

Two corrections to the one-line framing above:

- **Translucent sorting is a real regression, currently invisible.** One
  primitive means one sort key for all water. It does not show *for this
  material* — constant colour, constant opacity, no refraction, so a stack of N
  water surfaces transmits `(1-0.55)^N` whatever order they blend in. Fill-
  fraction shading, foam, caustics or refraction (all W5) would each break that.
  Do not add them and assume the pool still holds.
- **It exposed a bug in the shared pool.** Chunk-table entries were never
  recycled, so the table counted chunks ever added rather than chunks resident.
  Slow churn hid it on terrain; water's 10 Hz re-mesh crosses the proxy's
  headroom and turns every crossing into a full render-state rebuild plus a
  whole-buffer re-upload. Fixed for both paths.

Not established: a parity *number*. The cavern-lake harness's same-path
repeat-run noise floor is 20–88% of pixels, swamping any path difference — see
the design doc for why and what a tighter measurement would need.

## What G5 needs

`voxel.Stream.GPU` on by default needs **none of the four items above**. The
visual gap that did exist in default configuration is closed and measured.

Retiring the component path entirely is a different question, and it is not the
same decision as flipping the default. These still only work on the component
path, and each would become a silent no-op:

- Voxel GI (`voxel.GI.*`) — item 1.
- Chunk-state and ring debug tints — item 2.
- Ring cross-fade (`-VoxelRingCrossFade`) — item 3.
- `voxel.Render.CastShadow` A/B (pool hardcodes `true`).
- Static-relevance A/B (`-VoxelStaticRelevance`).
- The mesh-time diagnostics `-VoxelMatHistogram`, `-VoxelGIColorCheck`,
  `-VoxelWindingCheck`, `voxel.GI.DebugVis`.
- Per-quad (rather than per-chunk) climate sampling — the ~3% residual in the
  table above, deliberately accepted.
- `voxel.Stream.GPUMaxLevel` / `GPUMaxChunks` presuppose the component path as
  the fallback for non-pooled levels, and those are the two sharpest debugging
  tools this renderer has.

So: **flip the default, keep the path.** The cvar is the revert, and the
bisection tools that depend on both renderers coexisting are worth more than the
code deleted by retiring one.
