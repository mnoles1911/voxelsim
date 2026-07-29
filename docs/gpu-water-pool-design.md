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
| Vertex factory | `VoxelQuadVertexFactory.ush` | same + a water branch | **CORRECTED — see below** |
| Vertex colour used | R biome tint, G AO, B/A climate | **R fill, G AO, B top-boundary** | **CORRECTED — see below** |
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

> **CORRECTED. This held exactly as long as water's material was constant, and
> stopped holding the moment fill drove anything.** The claim was true, and its
> reasoning was sound, but its premise was load-bearing in a way this document
> did not flag: "water never samples R/B" is a statement about *this* material,
> not about water.
>
> Stepped fill-fraction surfaces need two per-vertex quantities the geometry
> already has and the factory was discarding:
>
> - **R — the CA fill fraction.** `Decoded.MaterialId` was decoded and then
>   thrown away, because terrain deliberately computes R as a *binary*
>   sky-facing biome flag from face direction and surface height rather than
>   from the material id (`:198` — thresholding a categorical id through the
>   `FColor`→shader transform proved unreliable). Water's mesher puts fill in
>   the `mat` byte precisely so it rides the existing 8-bit encoding, so the
>   factory has to stop discarding it.
> - **B — a top-boundary flag.** Whether this *vertex* sits on the +Z boundary
>   of its own voxel. Terrain uses B for per-chunk climate.
>
> Both are switched on `FVoxelQuadVertexFactoryParameters::WaterMode`, a uniform
> buffer member (not a loose `FShaderParameter` — those are measured not to bind
> in this project, and `ShaderBindings.Add()` on an unbound one is a *silent*
> no-op, `gpu-g2-draw-path.md`).
>
> **What survives the correction is the part that mattered.** The quad packing,
> the decode, and the pool machinery are still shared unchanged; this is one
> branch on one channel pair, not a second decode path, so the pool is still one
> class parameterised rather than two maintained. But the general claim — "the
> renderer never had to learn what water is" — is now false, and the honest
> version is narrower: *the geometry encoding never had to.*
>
> The per-vertex-ness of B is the non-obvious half and was found by reasoning
> through the geometry, not by a test. Gating the offset on the face normal
> instead — the obvious implementation — lowers only `+Z` faces and leaves a
> partially-filled cell's side walls at full height, ringing every pool with a
> one-voxel bathtub rim standing proud of its own surface. A side face has two
> top vertices and two bottom ones; moving only the top pair makes it a
> trapezoid that meets the lowered surface.

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

> **Still true after stepped fill-fraction surfaces landed, and worth being
> explicit about because it is easy to muddle.** That change makes fill drive
> **geometry** — where a surface vertex sits — and nothing else. Base colour and
> opacity are still constant, there is still no refraction and no scene-colour
> read, so two water fragments *still* differ between orderings only by their
> lighting, and a stack of N surfaces still transmits `(1-0.55)^N` in any order.
> Moving a vertex does not change transmittance. **The single sort key survives
> this change; it does not survive the next one.**

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

> **That anchor now exists: `-VoxelWaterParityTest[=<delaySeconds>]`**
> (`VoxelEarthGameMode.cpp`). It poses on the surface above the spawn column,
> **waits for terrain streaming to go quiet before any water exists**, pours a
> fixed 30,000 units, waits for the CA to reach zero active bricks, re-asserts
> the identical pose and captures `VoxelWaterParity`. The settle wait is the
> instrument, not a convenience — a capture taken mid-stream measures streaming,
> and the fixture logs a loud warning if it hits its poll cap without going
> quiet, so a contaminated run cannot be mistaken for a clean one.
>
> Protocol: run it twice at one `voxel.Water.GPU` value to get the noise floor,
> then twice at the other. If the same-config pair straddles the cross-config
> difference, there is no difference to report — say so rather than reporting
> the difference.
>
> **The frame-cost half still cannot be automated, and the reason is recorded in
> `manual-verification-checklist.md` 4a**: `-VoxelPerfRun` samples the world
> delta, which the engine clamps at `MaxUndilatedFrameTime` (400 ms), and these
> anchors run below that on the full cascade — so every automated sample reads
> exactly 400.00 and two configurations come back identical. A human reading
> `stat unit` is the instrument. **Draw is the number to watch, not Frame.**

## CA pooling vs SWE spreading (play-test, 2026-07-29)

With the ADR-0004 renderer union wired, the same 30,000-unit pour draws two
visibly different bodies depending on `voxel.Water.SWE`:

- **OFF (CA only):** Phase C's hydrostatic pass settles the pour into compact
  pools sitting in terrain depressions. 519 water quads.
- **ON (sheet + CA union):** the sheet spreads the same water laterally as a
  thin film across many more columns. 1,048 quads — more *surface*, not more
  water. `sheetVolume 29780 + caVolume 220 == 30000`, zero conservation
  failures, so nothing is double-drawn or lost.

**Matt's verdict, first hands-on pass: CA pooling into basins reads as more
correct.** Recorded as a preference, not a proof — and explicitly flagged as
something he may revisit with more testing.

**What was NOT established, and matters before anyone tunes this.** A gentle
pour onto near-flat ground cannot distinguish "shallow water correctly
spreading as a thin film" from "bed heights seated slightly wrong, so water
sits where it should have drained". Both look like a film. The discriminating
test is a **breach or a real slope**, where correct SWE produces a visible
surge and a directed front, and wrong beds produce water sitting on a hillside.
Tuning damping/absorption toward pooling before running that test would bury a
possible bed-seating bug rather than fix it, and the symptom afterwards would
be far harder to attribute.

So: the preference is recorded, the tuning is deliberately NOT applied yet, and
the breach test is the prerequisite. `voxel.Water.SWE` stays default-0.
