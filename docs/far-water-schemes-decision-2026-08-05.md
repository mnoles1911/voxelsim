# Distant water: the ring cascade against a heightfield strip and an instanced rim

Measured 2026-08-05 with `vxc_farwaterschemes` over the same three sites PR #226
used. Raw output: `docs/measurements/far-water-schemes-2026-08-05.txt`.

## B's scaling argument is false, and it is inverted

B rests on the claim that **rim length grows linearly with distance while
interior area grows quadratically**, so the interior is the expensive part and
instancing only the rim is nearly free. Measured on real river geometry at a
fixed 0.80 m cell, over a radius ladder from 50 m to 1 km, that is not what
happens. The exponents are fitted least-squares on log(count) vs log(radius):

| site | interior area | rim length | gap (needs +1.0) |
|---|---|---|---|
| A wet country, braided reach | R^1.07 | R^1.46 | **-0.39** |
| B wet country, lake reach | R^1.70 | R^1.84 | **-0.14** |
| C arid corridor, 100%-wet block | R^1.91 | R^2.80 | **-0.89** |

**The rim grows faster than the interior at every site.** The rim fraction
therefore *rises* with radius rather than falling — site A goes 0.7% of cells at
50 m to 2.2% at 1 km; site B rises from 4.8% at 150 m to 8.9% at 850 m.

The reason is geometric and it should have been obvious before it was measured.
A river is a **one-dimensional feature**: sweeping the radius outward adds
length, not area, so its interior grows linearly (site A's R^1.07) exactly as its
perimeter does. And distant water is not one body with one closed perimeter — it
is *many disconnected reaches and ponds*, and each new one that enters the disc
brings its whole perimeter with it. Quadratic interior growth only appears at
site C, where the camera sits in a 100%-wet block, and there the rim grows faster
still.

That kills B on its own terms. Everything below is why it would have lost anyway.

## The single table

Cumulative to each radius, cascade base 32 m, six LOD rings. Every scheme is
counted off **one shared water field** per ring, so a difference between two rows
is a difference between two schemes and never between two samplings of the world.
"MiB real" prices each scheme in the format it would actually ship in (see
*Bytes*, below); "reads as voxels" is the vertical step where water meets bank,
in pixels, at 90° HFOV on a 1920 px frame.

| site | scheme | dist | quads | instances | MiB real | reads as voxels |
|---|---|---|---:|---:|---:|---|
| **A** braided | today's 25.6 m box | 26 m | 22,539 | – | 0.26 | yes, 0.10 m step |
| | ring cascade | 100 m | 24,393 | – | 0.28 | yes, ≥3.8 px |
| | | 500 m | 32,317 | – | 0.37 | yes, ≥3.8 px |
| | | 1000 m | 32,741 | – | 0.37 | yes, ≥3.8 px |
| | A heightfield @cell | 1000 m | 759,626 | – | 110.11 | **no** |
| | A heightfield @column | 1000 m | 11,866 | – | 1.72 | **no** |
| | B surface + rim | 1000 m | 11,866 | 2,186 | 1.85 | yes, ≥3.8 px |
| **B** lake reach | today's 25.6 m box | 26 m | **0** | – | 0.00 | sees nothing |
| | ring cascade | 1000 m | 5,844 | – | 0.07 | yes, ≥3.8 px |
| | A heightfield @cell | 1000 m | 78,866 | – | 11.43 | **no** |
| | A heightfield @column | 1000 m | 1,225 | – | 0.18 | **no** |
| | B surface + rim | 1000 m | 1,225 | 2,788 | 0.35 | yes, ≥3.8 px |
| **C** 100%-wet | today's 25.6 m box | 26 m | 23,549 | – | 0.27 | yes, 0.10 m step |
| | ring cascade | 100 m | 37,940 | – | 0.43 | yes, ≥3.8 px |
| | | 1000 m | 61,614 | – | 0.71 | yes, ≥3.8 px |
| | A heightfield @cell | 1000 m | 1,422,538 | – | 206.21 | **no** |
| | A heightfield @column | 1000 m | 22,189 | – | 3.22 | **no** |
| | B surface + rim | 1000 m | 22,189 | 518 | 3.25 | **no within 400 m** |

## Why A loses: the greedy mesher already did A's job, perfectly

A's premise is that the cascade draws a volume when only the surface is visible.
Both halves of that are already handled, and the second one is the surprise:

* the interior proof drops every brick that emits no face, and
* **`meshBrick` is a GREEDY mesher.** It merges coplanar faces into maximal
  rectangles, so the flat top of a water column is *one quad*, not one per cell.

Measured, the merge factor on water tops is at the theoretical maximum of 64:1
(a brick face is 8×8 cells):

| site | top-face quads | top cells if fully split | merge |
|---|---:|---:|---:|
| A | 12,151 | 759,424 | **62.5×** |
| B | 1,248 | 78,400 | **62.8×** |
| C | 22,189 | 1,420,096 | **64.0×** |

So scheme A at cell resolution — the literal proposal, one quad per surface cell
— costs **exactly what the cascade's tops would cost if the mesher were switched
off**: 759,626 quads at site A against 759,424 top cells. That is **23× the
cascade's entire output** and 110 MiB against 0.37 MiB. At site C it is
1,422,538 quads and 206 MiB. A at cell resolution is not a saving, it is the
deliberate forfeit of a 64:1 win the renderer already banks.

A at *column* resolution is genuinely cheaper — 11,866 quads against 32,741 at
site A, 2.8×. What those 2.8× buy is precisely the thing being asked for:
**62.9% / 78.6% / 64.0% of the cascade's quads are SIDE faces**, and the side
faces are the stepped edge. Deleting them is deleting the voxel look. A film has
no thickness, so its vertical step at the bank is 0 px at every distance, at
every resolution, by construction — it is the flat quad the owner rejected, with
a displacement map on it.

The cascade's step, by contrast, holds at **≥3.8 px at every ring boundary out to
1 km** (≥5.1 px at 2560 px wide). That is the cascade's design property: the cell
doubles exactly as the ring radius doubles, so apparent step size is constant.

## Why B loses anyway

Beyond the falsified exponents:

* **Bytes go the wrong way.** #226 priced every quad at 152 B (4 verts × 32 B +
  6 indices × 4 B). That is right for a procedural heightfield — it is what
  `AVoxelRiverRibbonActor` really pushes — but **wrong for the voxel path by an
  order of magnitude**. A `vxc::Quad` is nine `uint8` fields, packed to **8 bytes**
  by `PackVoxelChunkQuad` and decoded in `VoxelQuadDecode.ush`, plus a 4-byte
  corner word on the water path: **12 B resident**. So B has 2.3× fewer
  primitives than the cascade and **5.0× / 5.0× / 4.6× more bytes**.
* **B has no rim where the water is widest.** At site C, rim cells are **zero out
  to 400 m** — the camera is inside a 100%-wet block, so there is no bank to
  instance. B degenerates to A exactly where the water fills the frame, and reads
  flat. Where B *does* have rim (site B, 2,788 instances against 1,225 quads),
  the rim is the *majority* of its cost. "The expensive part is the part nobody
  looks at" is backwards on both ends.

## The three checks

1. **Reads as voxels** — quantified above. Cascade and B: ≥3.8 px vertical step
   at every ring out to 1 km. A: 0 px at all distances, both resolutions.
2. **A shallow river must not vanish.** This world's water is p50 0.75–1.18 m
   deep and #226 caught a coarse level offering 138 bricks and meshing zero
   quads. `vxc_farwaterschemes` counts **vanished columns** — wet coarse columns
   that emit no primitive — per ring per site. With `farWaterFill` called from
   `farwater.h`, the result is **0 at every ring, at every site, at every
   radius**. The cascade is clean on the defect that killed the naive version.
3. **The handover.** The cascade is the only one of the three that does not
   invent a new seam class. `GetImplicitWaterDiscUU` publishes the near-field
   box; `AVoxelRiverRibbonActor::RebuildPath` already clips each reach against it
   with `ClipSegmentToRect` (Liang–Barsky) and emits the complement — up to two
   pieces, which is exactly the 101→102 quad observation — and
   `AVoxelWaterSheetActor::RebuildSheet` does the rect-minus-rect cut. The
   cascade changes **one constant**, the published radius, to
   `farWaterOuterBricks(base, maxLod)`; the 102-vs-101 check re-runs unchanged
   against the larger radius with no new code. A replaces a voxel volume with a
   zero-thickness film, so there is no rect to subtract: the film must terminate
   on the near field's outer voxel faces and match its corner-height field vertex
   for vertex or a crack opens along the whole disc. B inherits that seam and
   adds a second — rim instances must be suppressed where the near field already
   draws the same bank.

## Recommendation: ship the ring cascade. Drop A and B.

The number that justifies it: **to 1 km the cascade costs 32,741 quads / 0.37 MiB
at the braided reach and 61,614 quads / 0.71 MiB at the 100%-wet block — 1.45×
and 2.62× today's 25.6 m box for 40× the radius** — while keeping a ≥3.8 px
vertical step at every ring and zero vanished columns. At the lake reach it is
5,844 quads against a box that today draws *nothing at all*.

A costs 23–64× that to look worse. B costs 4.6–5.0× the bytes, has no rim to
instance exactly where the water is widest, and its central scaling claim is
contradicted by a measured gap of −0.14 to −0.89 where it needed +1.0.

The simple thing already wins.

## Caveats

* Sites A and B are **bv13** tiles and site C is **bv14**; this is the same tile
  set #226 measured, and no bv14 fine bake of the wet-country tiles exists on
  this box. The arid block census differs from #226 (2,906,407 vs 2,891,487 wet
  px) for that reason.
* Cross-check against #226: this tool aggregates coarse columns from **cells**
  where `vxc_farwaterprobe` aggregates them from **fine brick columns**. Surface
  bricks agree to 0.05% at site A (16,880 here against 16,872 in #226), so the
  two are seeing the same world.
* Quad counts are **pre-split**. `EmitWaterQuads` splits some again for the
  corner field, so every quad column is a floor. The split pushes the cascade's
  top faces *toward* the "top cells" column, i.e. toward A's cost — which makes
  A's case worse, not better.
