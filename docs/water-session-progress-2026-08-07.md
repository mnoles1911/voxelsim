# Water session progress — night of 2026-08-06/07

Branch `claude/f6-interior-rim-injection`. Written for the owner waking up, and
for whoever picks this up next. Everything below is measured; where a number was
retracted it is left in with the retraction, not deleted.

## What shipped

**bake_ver 17, full wet block (6 tiles) baked and on disk** at namespace
`terrain-diffusion-unlabeled-80b9ca451a23eae4-b901833bd`. Water-only: wet
fraction 0.885%, unchanged from bv15, so this is the same amount of water placed
better rather than more water.

### 1. The consistency pass now runs AFTER the bridge, and that was the whole fix

bake_ver 16 added `enforce_neighbour_consistency` and it did essentially
nothing — 3% of violations removed in the shipped tile against 100% offline.
Cause: `bridge_to_face_contact` ran *after* it and is the exact inverse
operation (it RAISES a cell's surface to reach its neighbours' beds). The two
stages fought and the last one won. Reordered at bv17.

Tile (-4,-4), rises measured on the shipped tile:

| | rises | p90 | max | **over 1 m** |
|---|---|---|---|---|
| bv15 no pass | 136,268 | 1022 mm | 5382 mm | 14,133 |
| bv16 pass before bridge | 134,654 | 1017 mm | 5382 mm | 13,819 |
| **bv17 pass after bridge** | **132,647** | **860 mm** | **4620 mm** | **9,860** |

The headline count barely moves. The *tail* is what moved: rises over a metre
fell **30%** and the worst case dropped 762 mm. That tail is the owner's "a spot
or two where the magenta blocks seem to flow slightly up hill" — a 187 mm p50
rise is under two voxels and nobody can see it.

### 2. F3's acceptance test now has an answer: the bridge has become a no-op

F3 was built with a documented test: *"with slope in the depth law,
`bridge_to_face_contact` should become close to a no-op on steep reaches. If it
is still doing heavy lifting, the depth model is still wrong."* It was never
read, because the counters were never printed. They are now:

| tile | bridge raised | p50 raise | consistency lowered | drawn cells |
|---|---|---|---|---|
| (-5,-5) | 44,341 | 0.10 m | 478,211 | 872,533 |
| (-4,-5) | 29,482 | 0.19 m | 397,573 | 689,838 |
| (-3,-5) | 23,016 | 0.23 m | 361,837 | 601,330 |

The bridge touches ~4% of drawn cells at p50 0.10–0.23 m. That is the "close to
a no-op" the test asked for, so **the depth model with the slope term is
carrying its weight**. The consistency pass is now the stage doing the work,
lowering 40–55% of drawn cells.

## Two bugs found in my own instrumentation

**The counters read a wrong key.** They printed `bridge=0 lvl=0` on a tile where
both stages demonstrably do work, because `pipeline.py:4983` re-exports every
`width_stats` key with a `water_` prefix. A counter that silently reads zero is
worse than no counter — it reads as "this stage does nothing", which is exactly
the conclusion I was one step from drawing about the bridge.

**A water stage moved without a version bump.** Reordering changes baked bytes
but no *constant*, and `STAGE_ORDER` only covers the terrain stages B0–B5 —
water stage order is not identity-covered at all. The first bake after the
reorder was silently skipped as already-present. Worked around with
`BAKE_VERSION` 16→17. **Water stage order belongs in the identity payload**;
left for a deliberate change rather than widening the partition mid-session.

## Retracted the same night — do not build on these

**"62% of downhill wet steps rise."** Real number, wrong quantity. It recomputes
steepest descent on the shipped ground, where the drop is p50 4 mm — one
quantisation step. The direction is therefore chosen by rounding noise among
near-ties, so a surface varying ±270 mm rises about half the time by
construction. 61.66% is what noise produces. The >1 m tail still means
something; the headline does not.

**The downstream/upstream split of those rises** (80.6% vs 24.5%, which looked
like a physics inversion). It assumed the low 5 bits of `flow` are log2
discharge. They are not: `corr(q, ground) = +0.108` over channel cells, where a
discharge field must be negative (big rivers sit low), and depth is non-monotone
in q. Discarded.

**"The ground under the rivers is smooth, so the staircase is in the water."**
Retracted — see `measurements/water-surface-roughness-2026-08-07.txt`. I
reconstructed ground in Python as `base_offset_mm + elevation_cp * quant`. The
client logs `ground top z=1809.2 m` at a column whose fine tile reconstructs
that way to 2287.5–2312.5 m: the real ground is ~480 m below the entire range I
admitted. Independent tell — within-tile relief is 14–42 m while base offsets
run 991–2921 m, so tiles sharing an edge would jump hundreds of metres across
it. Third time this project has lost work to picking the wrong one of the three
grounds.

**What survives from that measurement**, because it never used ground: between
adjacent wet channel cells, `|Δdepth|` is p50 20 mm, p90 500 mm, and exceeds one
100 mm voxel for **32.0%** of pairs — improving bv15→bv17 (35.2% → 32.0%, p90
600 → 500 mm). Whether those depth jumps become *surface* jumps is now
**reopened and unmeasured**, because a deepening pool in a dropping bed is
smooth water over rough depth, which is what real rivers do.

## Open, in priority order

1. **Measure surface roughness with `vxc_riverribbonprobe`**, which reconstructs
   ground the client's way and already has a LONG PROFILE section. Never in
   Python again. This decides whether the staircase is real.
2. **The level field** (Wave 3 item 1) — ship `_fill_levels`' per-cell water
   level banded so the client resolves the waterline at 10 cm against the ground
   it actually draws. Needs a new tile section; `WATER_DRY_DEPTH = -1` blocks
   the cheap route. This is the fix for the owner's blocky waterline.
3. **Ponding before spilling** — the owner's "fill all the open air tiles to its
   left, right and centre before flowing down into the next area of least
   resistance". Likely via lowering basin registration thresholds.
4. **Water stage order into the bake identity payload.**
5. **The exponents** — plumbing landed and identity-covered, still blocked on
   the confluence `inBed` question. Raising the *width* exponent barely widens
   rivers under `lateral_fill`; **depth is the lever**.

## Guard rails that earned their keep tonight

`voxel.GPU 0` is still not a cvar — `-VoxelNoGpuMesh` is the real gate. The
capture asserts marker-installed and GPU-fork-off and both fired correctly. The
first bv17 capture attempt failed on a 420 s wall budget that the boot ate
before the 180 s settle timer fired; re-run at `-TimeoutSec 900`.
