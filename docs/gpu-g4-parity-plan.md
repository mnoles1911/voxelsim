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
the overlap band the fade always needed, so it was worth re-testing. **It has
been re-tested and it is still broken** — see item 3 below. Nothing in G5 depends
on it, and it should not be built for the pooled path.

## Correction 4: under `voxel.Stream.GPU`, the GI light field is empty

Not previously recorded anywhere. `UVoxelGISubsystem::NotifyChunkMeshUpdated` is
called from exactly one site — `UVoxelChunkComponent::SetChunkQuads` — and the
pooled streaming branch returns before any `UVoxelChunkComponent` exists. So
with GPU streaming on, no chunk is ever voxelized into the field, no brick is
ever solved, and `voxel.GI.Enabled 1` is a silent no-op rather than a visible
difference.

This is step 0 of the GI work: without it, every subsequent GI verification
would compare an empty field against an empty field and pass.

**Fixed (PR #105).** The pooled branch of `ApplyMeshResult` now feeds the field
directly, moving the quads into a parallel voxelize queue that drains against the
same per-frame budget. Measured: component path and pooled path both settle at
**2212 bricks / 10.1 MB** — not the same order, the same number, since both
renderers consume the same CPU mesher output for the same level-0 chunk set and
the field is absolute-keyed. Before the fix the pooled run reported `bricks=0`
for its whole duration.

That change also had to bound the re-shade drain's *pops*, not just its
refreshes: a miss deliberately does not consume the refresh budget (that is what
makes the per-component dedupe free), and on the pooled path every pop misses, so
unbounded it would `RemoveAt(0)` through the whole ~2000-entry queue every frame
for no output at all.

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
free. This is the one remaining item where the material-asset question genuinely
returns — the tint is an RGB multiply, and all four vertex colour channels are
taken (R biome tint, G AO, B/A climate), so it cannot ride the pipe the other
items use.

The route is to widen the factory's `TexCoords` interpolant from `float2` to
`float4`, carry the tint packed into `.zw`, and have the material unpack it and
fold it into the same multiply `DebugTint` feeds today.

**The one design decision that makes this safe: encode identity as ZERO, not as
white.** The component path's vertex factory supplies only a `float2` texture
coordinate, so `TexCoord0.zw` arrives as zero there no matter what the graph
does. If the encoding is chosen so that zero means "no tint" — store the tint as
an offset from white, or reserve 0 as a sentinel the unpack maps to
`FLinearColor::White` — then **the same graph is correct on both paths with no
switch node and no permutation**, and the component path's rendering is
bit-identical to what it is today.

That is what makes this a genuinely small change rather than a risky one, and it
is why the asset edit should not be attempted without it: a naive unpack that
treats 0 as black would render every component-path chunk black the moment the
material is regenerated.

Regenerate with:

```
UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=Tools/create_voxel_material.py \
  -unattended -nop4 -nosplash
```

Verify by rendering the **component** path before and after and diffing the
screenshots numerically — not by reasoning about the graph. The bar is the
same-path repeat-run noise floor (0.02–1.1% of pixels differing by more than
8/255), and the CPU path must land inside it.

### 3. Ring cross-fade -- RE-TESTED, STILL BROKEN. Do not implement it.

`status.md` recorded that the ring-seam padding fix (`+e/sqrt(2)`) had finally
given the annuli the overlap band the cross-fade always needed, and that it was
"worth re-testing". It has been re-tested, and the answer is no.

Component path, same anchor, same 34 s settle, one flag differing:

| | pixels differing > 8/255 |
|---|---|
| `-VoxelRingCrossFade` vs control | 2.44% |
| same-path repeat-run noise floor | 1.83% |

The aggregate number is barely above the floor, but the difference is
**structured, not noise**: with the flag on, discrete blue see-through patches
appear in the mid-distance -- water and sky visible *through* terrain -- exactly
where the ring boundary falls. With it off the same region is solid. That is the
original see-through-ring signature, unchanged in kind.

So the overlap band was necessary but not sufficient. **Do not implement ring
cross-fade for the pooled path**, and do not spend the material-graph change on
it: it would be porting a defect. If it is ever revisited, the prerequisite is
understanding why overlapping annuli still dissolve to nothing -- most likely
that both rings fade *simultaneously* across the shared band rather than
crossing over, so their opacities sum to less than one in the middle -- and that
is a residency/fade-curve question, not a renderer one.

This closes the item. The G0 checklist listed it first; it turns out to be the
one thing on the list that should not be built.

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
