# Water surface pool (`voxel.Water.GPU`)

The water half of ADR-0006, `docs/gpu-g4-parity-plan.md` item 4. That item
called it "a separate, near-identical instance of the terrain pool". This
records how near-identical it actually turned out to be, since the answer was
the deliverable as much as the code was.

## The claim, checked

| | terrain pool | water pool | shared? |
|---|---|---|---|
| Material | `M_VoxelTerrain` | `M_WaterVoxel` | per-instance setter |
| Blend mode | opaque/masked | **translucent**, two-sided, opacity 0.55 | material only |
| Quad packing | `PackVoxelChunkQuad` | same | **yes, unchanged** |
| Decode | `VoxelQuadDecode.ush` | same | **yes, unchanged** |
| Vertex factory | `VoxelQuadVertexFactory.ush` | same | **yes, unchanged** |
| Vertex colour used | R biome tint, G AO, B/A climate | **G only** | inert, not wrong |
| Per-chunk material params | ring cross-fade, debug tint | **none** | n/a |
| Entry size | 32-voxel chunk | 8-voxel `vxc::WaterBrick8` | per-instance setter |
| Mip levels | 0..5 | always 0 | n/a |
| Shadows | `CastShadow` true | `CastShadow` true | same |
| Proxy relevance | — | `bTranslucentSelfShadow`, `CanBeOccluded` | added to both |

Two rows carry the whole result.

**The vertex factory needed no change**, which was not obvious in advance and
is the reason this is one class rather than two. `M_WaterVoxel`'s only input
from the geometry is `VertexColor.G` — it multiplies a constant blue by AO —
and the factory already writes AO into `.G`. The other three channels carry
terrain's sky-facing biome flag and its per-chunk climate; water never samples
them, so they are inert rather than wrong. Nothing in `.ush` had to learn what
water is.

**The quad packing already fits.** `PackVoxelChunkQuad`'s fields are each 8
bits and a water brick is 8 voxels on an edge, so brick-local coordinates fit
the encoding that carries terrain's 32-voxel chunk-local ones with 2 bits to
spare instead of 3. Water is in fact simpler: terrain bakes each brick's offset
within its render chunk into `Slice/U0/V0` at conversion time, whereas for
water the brick *is* the pool entry, so there is no rebase to bake.

So it is a second **instance**, parameterised by three setters — `SetPoolName`,
`SetChunkEdgeVoxels`, `SetChunkTableCapacity` — not a parallel copy. The
rendering notes warn against maintaining two immature pools and nothing here
justified it.

## Translucent sorting: the honest caveat

This is the one place where "near-identical" is genuinely false, and it is
worth being precise about *why it works anyway*, because the reason is narrow.

The renderer sorts translucent geometry **per primitive**. Per-brick, each
80 cm brick was its own sort key: water sorted correctly against the ocean
plane, against particles, and against other water at 80 cm granularity. Pooled,
every water surface in the world is ONE sort key, and within it triangles blend
in pool-buffer order — which is allocation order, i.e. arbitrary.

That does not produce a visible error **for this material specifically**:

- Constant base colour, constant 0.55 opacity, no refraction, no
  depth-dependent or scene-colour term. Two water fragments composed under the
  `over` operator therefore differ between orderings only by their lighting
  (AO and the specular response), not by their transmittance. The amount of
  background showing through a stack of N water surfaces is
  `(1-0.55)^N` regardless of the order they are drawn in.
- Water is still depth-tested against opaque terrain, so water behind rock is
  correctly hidden whether it is one primitive or three thousand.
- The per-brick path was already unsorted *within* a brick, so this widens an
  existing approximation rather than introducing a new class of error.

**What would break it**, and each is on the W5 polish list:

- Per-fill-fraction shading, foam, or caustics — anything that makes two water
  fragments differ in more than lighting.
- Refraction or any scene-colour read, which makes the composite order-dependent
  outright.
- A second translucent material intersecting the water volume, which would now
  sort against the whole pool as one unit rather than against the nearby bricks.

If any of those land, the pool needs either per-region sort keys (several
primitives, one per spatial bucket, giving back most of the win) or an
order-independent-transparency path. **Do not add them and assume the pool
still holds.**

## The pool bug water exposed

Chunk-table entries were never recycled. `AddChunk` appended a fresh entry
every call; `RemoveChunk` gave nothing back. So `ChunkOrigins` counted chunks
*ever added*, not chunks resident.

Terrain hides this — its churn is slow enough that the table stabilises near
the resident count. Water does not: the CA re-meshes at 10 Hz and its bricks
appear and vanish continuously, and the implicit pass sweeps a 65x65x33 brick
disc every time the camera crosses a brick boundary. Crossing the proxy's
`MaxChunks` headroom is **not a slow leak but a cliff**: `PushUpdatesToProxy`
falls back to `MarkRenderStateDirty`, which discards the proxy and re-uploads
the entire quad buffer — precisely the cost the incremental path exists to
avoid, now on a repeating timer.

Fixed with a free list. It is safe because `RemoveChunk` already repoints every
one of the chunk's quads at the hidden entry before returning, so no quad in
the pool still references the id being handed back.

One caller must NOT recycle: `UpdateChunk`'s realloc branch deliberately reuses
its chunk's entry so that an actively-dug chunk does not consume a fresh table
slot on every edit. Handing that id to the free list while quads still
reference it would let a later `AddChunk` issue it twice, and two chunks
sharing a table entry means one of them silently draws at the other's origin.
Hence `bRecycleChunkId`.

Visible in the log as `tableEntries=22 (4 free)`.

## Verification, and why the number is missing

### The anchor the task assumed does not work

`-VoxelSpawnAt=-84480,53760` has water in frame, but it is
`AVoxelOceanActor`'s plane — a different primitive with a different material
(`M_Ocean`) that `voxel.Water.GPU` does not touch. The implicit pass logs
`0 candidate brick(s)` there, and `UWaterChunkComponent` geometry exists only
where the CA has water (poured or breached) or where a cavern lake is in
range — i.e. underground. An A/B at that anchor compares two images containing
no water-brick geometry at all and passes either way.

Use **`-VoxelFloodTest=<delay>`** instead. It finds a real flooded cavern,
poses a fixed camera on the shore, and captures the static implicit lake and
then the drained lake, which exercises the implicit and CA halves of the pool
in one run.

### What was seen

Pooled, at the flood-test anchor:

- `voxel.Water.GPU: water pool up ... rebase (42000,20960,95840)`
- 2,231 implicit bricks / 28,862 quads in ONE primitive, 1 free run
  (unfragmented) throughout
- `VoxelWaterPool draw SUBMITTED`
- Translucent blue water at the correct waterline, cavern floor read through
  it, same alcove and same wall detail as the component path
- Drain shot showing the same drop, with the mobilization ledger identical to
  the component run: 655 bricks, debited == credited == 51,321,555, shortfall 0

Terrain under `voxel.Stream.GPU` re-checked after the chunk-id change: 4,182
chunks, 4,055,557 quads, `hidden=0 outOfRange=0`, one draw, renders normally.

### What was NOT established

A parity **number**. Same-path repeat runs differ by 20.4%–87.7% of pixels at
>8/255; pooled against component differs by 28.6%–79.2%. The ranges overlap
completely, in both directions, so this harness cannot separate the two paths.

The variance is **underground terrain-chunk residency**, not the water — the
same flakiness recorded as gotchas 3 and 4 of the C7/C8 pass in
`docs/status.md` ("underground chunk residency is tight and keyed to camera
height"; "from underground the world renders see-through"). It is two orders
of magnitude above the 1.1% floor the terrain A/B enjoys, because terrain fills
the frame from a stable surface anchor and a cavern does not.

**A tighter measurement needs a scene whose only variable is the water.** The
cheapest candidate is a surface pour at a flat anchor with the terrain fully
settled first, which removes the underground residency variable entirely;
`voxel.Water.SpawnIn` exists for that and aims at the ground rather than the
crosshair for the reason recorded in its own comment. It was not run to
conclusion here.
