# Far-field voxel water — measurement, verdict, and the plan

> **STATUS 2026-08-09: [SUPERSEDED by `docs/water-rearchitecture-plan-2026-08-09.md`
> — kept for measurements/history].** This is the ring-cascade plan to extend
> *real voxel water* out to 500 m–1 km. The re-architecture does not extend
> voxel water outward at all: near field becomes PBF + screen-space fluid, and
> mid/far stays ribbons and sheets unchanged, so the gap this plan closes is
> closed a different way. The `farwater.h` LOD/fill primitives it shipped are
> bench-only and unwired into UE — see the deprecation audit,
> `docs/water-deprecation-audit-2026-08-09.md`. See `docs/water-architecture.md`.

Written 2026-08-05 on `claude/far-voxel-water`. The owner's ask: **real voxel
water out to 500 m – 1 km.** Today it stops at 25.6 m and everything beyond is
flat quads, which they described as looking terrible within 1 km.

**Read §1 before designing anything.** The headline is a negative result and it
kills the obvious plan.

---

## 1. The verdict, with the number

**Full-resolution (0.1 m voxel) water to 1 km is not affordable. It is not
close.** Measured, not estimated — `vxc_farwaterprobe` enumerates the candidate
set from the baked water plane and runs `meshBrick<8>` for real over every
brick that emits a face. Raw output in
`docs/measurements/far-voxel-water-2026-08-05.txt`.

Surface bricks (bricks that emit at least one face — the ones that cost a mesh
and a draw) and quads, against what today's 25.6 m box actually meshes at the
same camera:

| site | today, 25.6 m box | full-res 1 km | ratio |
|---|---|---|---|
| A — wet country, braided reach | 8,409 brk / 22,539 quads | **630,523 brk / 1,143,384 quads / 166 MB** | 75× / 51× |
| B — wet country, lake reach | **0 brk / 0 quads** | **346,185 brk / 881,711 quads / 128 MB** | ∞ |
| C — arid corridor, 100%-wet block | 8,450 brk / 23,549 quads | **7,232,575 brk / 10,858,375 quads / 1,574 MB** | 856× / 461× |

Site C is the worst case available in either cache and it is the one to size
against: **7.2 million surface bricks and 1.57 GB of quad data.** At the
existing `kMaxImplicitMeshesPerTick = 192` that is 37,670 ticks — **ten minutes**
to drain one rebuild.

Site B is worth reading twice for a different reason: **today that camera sees
zero voxel water**, because the nearest reach is past 26 m. That is the owner's
complaint expressed as a number rather than as a screenshot.

### At 500 m it is still not affordable

174,108 / 430,707 / 2,418,973 surface bricks at sites B / A / C. Site C alone is
287× today. **500 m does not rescue the plan; it is the same wall one step
closer.**

---

## 2. What DOES work, and it is measured too

### 2.1 The premise about enumeration was right

The sweep is dense only because it does not know where the water is, and the
baked plane does:

| | wet country (6 tiles) | arid corridor (4 tiles) |
|---|---|---|
| water blocks CONSTANT-dry | **68.7%** | **79.0%** |
| wet fine pixels | 0.53% | 1.08% |

A CONSTANT block owns no data-section entry at all, so two thirds to four fifths
of the plane is rejected **from the block index with zero bytes fetched and zero
decodes**. Candidate generation stops scaling with *swept area* and starts
scaling with *water area*.

**But that fixes the SEARCH cost, and §1 says the binding cost is the DRAW.**
This is the trap worth naming: the enumeration premise is true, it is a real
improvement, and on its own it does not deliver the feature. Shipping it alone
would have produced a beautifully cheap candidate list feeding a mesher that
takes ten minutes.

### 2.2 A distance-doubling LOD cascade is affordable

LOD `L` covers `[base << (L-1), base << L)` and uses a `(0.1 << L)` m voxel.
Each ring quadruples in area and quarters in linear resolution, so **the cost
per ring is flat**. That is the whole architectural argument, and it is
measured rather than asserted — surface bricks per ring, base 32 m:

| site | ring 0 | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|
| | 0–32 m | 32–64 | 64–128 | 128–256 | 256–512 | 512–1024 |
| C — arid 100%-wet | 10,026 | 7,536 | 3,768 | 3,760 | 3,628 | 2,341 |
| A — wet braided | 10,012 | 3,009 | 2,334 | 1,016 | 408 | 93 |

Totals out to **1,024 m**:

| site | surf bricks | quads | MB | vs today |
|---|---|---|---|---|
| A — wet braided | 16,872 | 38,282 | 5.5 | **2.0× bricks, 1.7× quads** |
| B — wet lake | 1,414 | 6,581 | 0.9 | (today: zero) |
| C — arid 100%-wet | 31,059 | 49,528 | 7.2 | **3.7× bricks, 2.1× quads** |

**Two to four times today's brick count for forty times the radius.** That is
the deliverable shape.

### 2.3 Which `base` to pick — this is a quality decision, and it is the owner's

`base` sets how big a water voxel is on screen. At 90° horizontal FOV on a
1920 px frame one pixel subtends 8.18e-4 rad, so a voxel at its own ring's outer
edge is always `0.1 / (base/1 m)` rad ≈ a constant pixel size:

| base | voxel at ring's far edge | reach at LOD 5 | cost, site C |
|---|---|---|---|
| 32 m | **3.8 px** | 1,024 m | 31,059 brk / 49,528 quads |
| 64 m | **1.9 px** | 2,048 m | 48,812+ brk (LOD 0 alone is 40,138) |
| 128 m | **0.95 px** | 4,096 m | LOD 0 alone is 107,620 brk — too expensive |

`base = 32 m` is the recommendation: it keeps LOD 0 at roughly today's near
field (so the near field is unchanged by construction) and reaches 1,024 m for
2–4× today's cost. 3.8 px is visibly chunky at a ring's far edge, but **that is
the owner's call, not mine** — the number is here so it can be made without
another measurement pass.

**This reframes the goal, and the reframing is honest:** the owner's complaint
is about the *flat quads*, not about voxel size. "Reads as voxels, not a flat
plane, all the way out" is deliverable. "Full resolution everywhere" is not.

---

## 3. What is built and landed on this branch

### `voxel-core/include/voxelcore/farwater.h` — the rules, integer-only

`farWaterCellMm` / `farWaterBrickMm` / `farWaterStep`, `farWaterLodForDistance`,
`farWaterFill`, `FarWaterColumn`, `FarWaterAccumulator`, `farWaterBrickRange`,
`farWaterBrickIsInterior`, `farWaterOuterBricks`.

They live in voxel-core for the reason `implicitWaterFill` does: so the client's
binding site and the tests cannot express them differently.

Three of these are worth knowing about before touching them.

**A brick stays 8×8×8 at every level.** Only the scale it is drawn at changes.
That keeps `meshBrick<8>`, `WaterBrick8`, `BuildWaterFillPad`,
`BuildWaterCornerField`, `EmitWaterQuads` and the whole upload path untouched —
the same trick the terrain rings already use.

**`farWaterFill` at LOD 0 CALLS `implicitWaterFill`.** It does not reimplement
it and it is not "equivalent to" it. `lod0_is_exactly_the_near_field` pins the
same value for every cell across the ground, the datum and the partial top.
This is the no-near-field-regression guarantee, and it is structural rather
than tested-into-existence.

**Above LOD 0 one branch of the fill changes, and MEASUREMENT forced it.** The
fine rule rejects a cell whose *bottom* is below the ground — correct at 100 mm,
where the ground is flat across the cell. It is catastrophically wrong at 1.6 m.
This world's water is **p50 0.75 m deep** (site A) to **p50 1.18 m** (site B),
so at LOD 4 the entire water column fits inside one cell; if the ground falls in
that cell's interior, the cell below is rejected for starting under the ground
while the cell above starts above the datum, and **every cell reads dry**.

> Measured before the fix: LOD 4 offered 138 surface bricks at 100 m and meshed
> **zero quads**. The river did not get coarser with distance — it *disappeared*,
> silently, in exactly the direction ("there is no water out there") this whole
> feature exists to fix.

Above LOD 0 a cell is now rejected only when it lies *entirely* below the ground.
That is safe to draw because the surface's true height is carried by the 8-bit
**corner heights**, not by which cell the quad lands in — a 3.2 m cell still puts
its surface at the right millimetre. Pinned by
`coarse_fill_does_not_erase_shallow_water`.

**The vertical bound is the water column, not a box.** `farWaterBrickRange` runs
ground→datum and **never mentions the camera**. `kImplicitRadiusBricksZ = 16` is
why water stops existing 12.8 m above the camera, why a capture at 90 m logs
`0 candidate brick(s)`, and why flying loses the river. A 1 m deep column now
costs the one or two bricks it needs instead of 33, and a camera at 200 m or
5 km still sees every reach below it. Pinned by
`brick_range_comes_from_the_water_column_not_a_box`.

### `voxel-core/bench/farwaterprobe.cpp` — `vxc_farwaterprobe`

Reproduces today's 25.6 m box *in the same run* so every number has its own
baseline rather than a quoted one, then enumerates plane-driven candidates and
meshes them for real. `--scan` picks a camera site from the block census; a site
off the river measures nothing, which is how three of nine vista sites were once
wrong.

### 15 tests in `voxel-core/tests/test_farwater.cpp`

Every one pins something a measurement forced, not something a design preferred.

### Gates

```
vxc_tests   478, 0 failures   (463 on this origin/main + 15 new)
ctest       2/2  -C Release
UE module   Result: Succeeded (Build.bat VoxelEarthEditor Win64 Development)
```

`TERRAIN_VERSION` untouched; no bake change; nothing in `terrain-service` touched.

---

## 4. What is NOT built, and the one thing that blocks it

The UE binding site is **not** wired. This is deliberate, and there is a hard
dependency behind it that the next session must handle first.

### 4.1 The blocker: `M_WaterVoxel` hardcodes one voxel of WPO

`ue-project/Tools/create_water_voxel_material.py:786` builds the
partial-fill offset from a literal:

```python
down_one_voxel.set_editor_property("constant", unreal.LinearColor(0.0, 0.0, -10.0, 1.0))
```

`-10.0` is `VoxelCoords::VoxelSizeUU`. World-position offset is applied in world
space, so **scaling the component does not scale this** — at LOD `L` the water
surface would sit up to `(1 << L)` voxels too high, i.e. up to 3.2 m out at
LOD 5. That is a visible step at every ring boundary, on the exact seam this
feature has to sell.

**The fix is small but it is an editor task:** replace the constant with a scalar
parameter (`VoxelSizeUU`) and create one material instance per LOD level at
startup — five MIDs total, shared by every brick at that level, so the per-brick
cost is nil. It needs the editor to run the material script, and the editor was
in use for this whole session.

**Do this before writing any of §4.2**, because it decides whether the far field
can use `UWaterChunkComponent` at all or needs its own proxy.

### 4.2 Then, in `VoxelWaterSubsystem.cpp`

1. `RefreshFarWater`, beside `RefreshImplicitWater` and **not replacing it** —
   ring 0 stays exactly the code that runs today, which is what makes "no
   near-field regression" free rather than something to re-verify.
2. Candidates from `riverRibbonFillWet` (the plane half, block-major, already
   tested at 0 disagreements against `surfaceAtPixel`) unioned with
   `LakeSampler::extentMaskFor` (the basin half — the bake writes the plane DRY
   inside a registered basin, so the two are disjoint and `CompositeWaterSampler`
   is what unions them). `vxc_farwaterprobe` does exactly this and is the
   reference.
3. Per-ring rebuild triggers. **Today the whole disc rebuilds when the camera
   crosses ONE 0.8 m brick boundary.** A far ring must not: rebuild ring `L`
   only when the camera has moved a fraction of that ring's own cell, or the far
   field re-meshes continuously while walking. This is the single biggest
   performance risk in §4.2 and it has not been measured.
4. Budget the drain per ring, farthest-first within a ring, near rings first.
5. `SetVoxelScale` on `UWaterChunkComponent` (or the pooled equivalent), plus
   the per-LOD MID from §4.1.

### 4.3 The handover, which must move

`GetImplicitWaterDiscUU` (`VoxelWaterSubsystem.cpp:5766`) publishes the 25.6 m
disc, and both far-field actors cut their flat quads against it —
`AVoxelRiverRibbonActor::RebuildPath` (Liang-Barsky in doubles) and
`AVoxelWaterSheetActor::HoleForDatum`. **Both must be cut against
`farWaterOuterBricks` instead**, or flat quads are drawn underneath the voxel
water for the whole cascade and the two-renderer tone problem (Phase 5,
+44.5 vs +76.8 blueness) shows up over a kilometre instead of over 52 m.

The existing handover was verified at 102 quads vs 101 from the identical 8
reaches, one segment split at the disc boundary. **Re-run that check against the
new radius**; it is the cheapest possible proof the cut still lands exactly.

Note the z gate too: `HoleForDatum` refuses when the datum is outside
`[MinZ, MaxZ)`, which comes from `kImplicitRadiusBricksZ`. The far field has no
vertical bound, so that gate must be dropped for the far radius rather than
widened.

---

## 5. Things that are settled, so nobody re-derives them

- **The 4,500-bricks figure is a candidate count, not a draw count.** Reproduced
  here: at sites A and C the 25.6 m box offers **71,825 candidates** of which
  only **8,409 / 8,450 mesh non-empty**. The sweep offers every brick from the
  box *floor* up to the flood level with no lower bound from the ground, so in
  shallow water ~88% of what it offers is underground and meshes to nothing.
  Compare *meshed* against *meshed*, or the far field looks 8× better than it is.
- **`surf bricks == water bricks` on a river is correct, not a bug.** p50 0.75 m
  water is thinner than one brick at every level, so no brick in it can be
  interior. A rule that claimed otherwise would be deleting the river. Pinned by
  `interior_proof_never_fires_on_shallow_water`.
- **Depth must be read as a distribution.** Site A reports p50 0.75 m, p90 0.87,
  p99 1.49 — and **max 207.26 m** on one column. That maximum is not 207 m of
  water: it is `datum − amplified ground` where the amplifier has cut below the
  spline inside a lake extent, i.e. the two grounds disagreeing on one column.
  Size against the percentiles.
- **The majority aggregation rule (`≥ half the children wet`) is not arbitrary.**
  Strict-ANY grows every river outward one coarse cell per level, so a 1.9 m
  ribbon is 3.2 m wide at LOD 5 and climbs its own banks. Strict-ALL erases every
  river narrower than the cell, which at LOD 5 is most of them. Same shape as
  `mips.h`'s `solidThreshold = 4 of 8`, for the same reason.
- **Ground and datum aggregate as MEANS over the wet children, not extremes.** A
  max datum lifts the coarse surface to the highest point of a descending reach
  and makes every ring boundary a step; a min ground digs the water into the
  bank.
