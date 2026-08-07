# Deciding the waterline at 10 cm, against the ground we actually draw

**What this is.** The plan for the three problems the owner named on 2026-08-06,
after flying the magenta marker:

> 1. It runs at 1.875 m. The gravity solve picks a path on a grid 18× coarser
>    than a voxel, so at voxel scale the water can't follow a gully it never saw.
> 2. It solves on a different surface than we draw.
> 3. The consumers ignored slope.

**(3) is done** — `water_slope_in_depth` and `water_slope_in_extent` (commits
`44864ce`, `7bac156`), both off by default pending one bake decision. This
document is (1) and (2), which turn out to be the same change.

`docs/water-system-architecture.md` remains the durable design doc; this sits
under it.

---

## 1. One correction to the problem statement, because it changes the fix

Problem (2) as stated conflates **two different splits**, and only one of them
is a defect.

**The pre-B5 / post-B5 split is deliberate and already handled.**
`graded_water_surface` grades along the pre-B5 routing surface because that is
the one B4b's refill guarantees has zero sinks; the post-B5 shipped surface has
registered basins re-opened as holes, and running a descent chain through a
500 m deep re-opened basin would drag an entire river's water surface down into
it. Basin cells are passed as `exclude` and written dry, and the basin table
carries their surface separately. That split is load-bearing, not an oversight.

**The real split is baked ground versus drawn ground.** The bake reasons about
the *reconstructed* surface at 1.875 m. The client draws the *amplified*
surface at 10 cm — reconstructed plus detail the bake never saw. There are
three grounds in this codebase and conflating them has cost days before
(architecture §9), so name them:

| | what it is | who uses it |
|---|---|---|
| #1 sample field | the raw control lattice | the bake subtracts it to get depth |
| #2 reconstructed | `reconstructedGroundMm`, the B-spline of the lattice | **the water datum is measured from this** |
| #3 amplified | `GroundMmAt`, + 10 cm detail | **what the player sees and collides with** |

And one rule that must survive this work: **the water SURFACE must not read the
amplifier.** Both `tile_codec.py` and `tilestore.h` forbid it, and the reason is
good — a water surface is flat or graded by definition, and adding a depth to a
rippled surface puts metres of rill noise on it. The amplifier was once blamed
for burying rivers; measured, it adds **+3 mm** at the centreline (p50), and
that mistake nearly rolled `kWorldGenVersion` for nothing.

**So the split to close is not "solve the water surface on the amplified
ground". It is "decide the WATERLINE against the amplified ground".** The
surface stays a smooth baked datum; where that datum meets the terrain is a
per-voxel question and it is currently answered at 1.875 m.

---

## 2. What is already right, and it is more than it looks

The near field **already resolves the waterline per voxel at 10 cm against the
amplified surface**. `VoxelWaterSubsystem.cpp` reads `GroundMmAt` (ground #3)
and calls `implicitWaterFill`, which even carries the topmost voxel's sub-voxel
remainder so the surface sits *at* the datum instead of snapping to the 10 cm
lattice.

The fill rule was never the problem. **The DOMAIN is.**
`waterSurfaceMmAtVoxel` answers `kNoWaterMm` outside the baked wet mask, and
that mask is one decision per fine pixel — 1,875 mm against a 100 mm voxel.
So the waterline is computed at 10 cm inside cells the bake called wet and **not
computed at all one cell over**, and the edge snaps to a 1.875 m boundary. That
is the blocky edge in every screenshot.

---

## 3. The fix: ship the field the bake already computes and deletes

`fill_to_local_surface` computes, via `_fill_levels`, a **per-cell water surface
for every cell that drains to a drawn channel** — one topological sweep up the
receiver forest. It uses that field to decide wet/dry at 1.875 m, writes the
answer, and **throws the field away**.

That field is exactly what the client needs.

```
today   tile stores:  wet mask + depth, per 1.875 m cell
        client asks:  "is this cell wet?"        -> 1.875 m answer
        result:       waterline quantised to the raster

after   tile stores:  water SURFACE LEVEL over a band, per 1.875 m cell
        client asks:  "is amplifiedGround(vx,vy,vz) below the level here?"
        result:       waterline at 10 cm, following the drawn gully
```

**Why this solves (1) as well as (2).** The gravity solve still runs at 1.875 m
— and it should, because it needs the global watershed view a client standing in
one tile cannot compute. What changes is that its *output* stops being a
yes/no per raster cell and becomes a height the client can intersect with the
terrain it actually draws. The gully the coarse solve never saw is then
followed by the waterline, because the waterline is decided where the water
surface crosses the 10 cm ground.

**It is one sampler query, not a search.** This is the difference between this
plan and what was tried on 2026-08-06 and reverted: extending the domain at
*runtime* by searching neighbours cost 8n locked sampler queries per column on
every column in the world, and measured a **2.7× streaming regression**
(51,063 chunks settled against 19,162 still churning, same pose, 90 s). Baking
the level field moves that work to the bake, where it is already being done.

---

## 4. What it costs, and the number that decides it

**Unmeasured, and this is the first task.** The wet mask covers **0.560%** of a
tile (architecture §11). The level field is defined on every cell that drains to
a drawn channel, which is a much larger set — potentially most of a valley
floor. Storing it unbounded would be a large fraction of a tile.

So the field must be **banded**: store the level only where the ground is within
`H` metres of it, since a cell 40 m above the water can never be wet no matter
how the amplifier ripples. `H` is the whole cost/benefit knob:

* `H` must exceed the amplifier's own swing, or the band clips exactly the
  detail this change exists to resolve;
* the amplifier adds **+3 mm** at the centreline (p50) but tens of metres on
  open hillside, so the band is not obviously small.

**MEASURE FIRST, on the wet alpine block:**

1. the fraction of cells with a defined level, unbanded;
2. the same at `H` = 1, 2, 5, 10 m;
3. the compressed size of the resulting plane, against the water plane's
   current 51–174 KB and the flow plane's measured 0.920 MB for its flag bits.

The format has form here: `docs/tile-slicing-2026-08-04.md` records that the
water plane is **72–87% CONSTANT blocks**, which is why it is small. A level
field over a band may be far less constant, and the honest expectation is that
this costs more than the plane it augments. The spec for this format was once
out by **1000×** on exactly this kind of estimate.

---

## 5. Sequencing, and what it rides with

This is a `BAKE_VERSION` roll: the tile gains a section. Two other decided-but-
unflipped changes roll it too — `water_inject_at_interior_rim` (F6, worth 300×
the discharge on one measured tile) and the slope terms from (3). **All of them
should roll once.** A water-only re-bake re-keys 100% of every tile, which is
~13 GB today, or ~43 MB after per-section content addressing.

So:

1. **Measure §4.** One probe run. It decides whether the band is affordable and
   at what `H`.
2. **Bake the level plane** behind an off-by-default constant, and prove
   water-only with `tools/verify_water_only_change.py` (elevation bit-identical).
3. **Client reads it**: `waterSurfaceMmAtVoxel` returns the banded level instead
   of only the wet cells' surface. `implicitWaterFill` is unchanged — it already
   does the right thing once the domain is right.
4. **Then flip everything together** and pay the re-key once: F6, both slope
   terms, the level plane, and the exponent change if its confluence failure is
   explained by then.

---

## 6. What this does NOT fix

* **Depth.** The stored depth is still per 1.875 m cell. This changes where the
  water *ends*, not how deep it is between. F3's slope term is the depth half.
* **The staircase on steep reaches, entirely.** A finer waterline will soften it,
  but the surface is still piecewise-constant per cell, and interpolating it is a
  separate change (already prototyped in the marker).
* **Simulation.** Nothing here steps water over time, and nothing should: every
  CA-active brick re-meshes at 10 Hz, spare capacity is ~893 bricks/s against a
  river's ~6,100 in the near-field box — **7× over** — and the CA has a
  documented runaway where one 4-voxel shaft converts 100% of a reach. The
  gravity solve already exists in the bake; this plan is about resolving its
  answer at the resolution we draw, not about re-solving it at runtime.
