# Phase 4 — the far-field river actor

> **Current, with one caveat about its numbers (noted 2026-08-05).** The
> decision this document records — draw at the width the bake drew, no
> screen-width floor — still stands, and the widening measurement behind it is
> still the reason. But **every capture and width figure here is against
> `bake_ver` 10**, which is the most fragmented and narrowest river this project
> has produced; the bake is now at **14**, via law-driven width (12), lateral
> fill (13) and slope face contact (14). Nothing in the actor or the producer is
> pinned to a bake version, so the probe and the three captures can be re-run
> unchanged — and should be, before any of these numbers is quoted as current.
>
> How the system works today: `docs/watershed-system-plan.md`. What is still
> wrong with it: `docs/water-deep-dive-brief-2026-08-05.md`.
>
> The "52 m implicit disc" below is the **diameter** of the near-field box. Its
> reach from the camera is **±25.6 m horizontally and ±12.8 m vertically**, and
> the vertical half is what binds at altitude.

Written 2026-08-04. Closes the Phase 4 gap in `docs/water-handover-2026-08-04.md`
§5: `voxelcore/riverribbon.h` produced ordered centreline polylines and
**nothing consumed them**, so flowing water was invisible beyond the 52 m
implicit disc.

There is now a consumer. This document separates what was **measured** from what
was **decided**, because the decision rests entirely on the measurement and the
measurement is re-runnable.

---

## 1. What was built

| file | what it is |
|---|---|
| `ue-project/Source/VoxelEarth/VoxelRiverRibbonActor.h/.cpp` | the actor: a `UProceduralMeshComponent` sweeping the producer's polylines into quads at `M_WaterVoxel` |
| `UVoxelWaterSubsystem::{Begin,Fill,Finish,Abandon}RiverRibbonWindow` | four staged methods; the only thing the actor needs from the water tier |
| `IWaterSampler::ribbonTiles()` / `ribbonRivers()` | additive hook, defaulting to `nullptr`, mirroring the sheet half's "answers nothing here" doctrine |
| `VoxelEarthGameMode.cpp` | one `SpawnActor` line beside the lake sheet's |
| `vxc_riverribbonprobe --> LONGEST REACHES` | reach midpoints in world **metres**, so a capture can be aimed at a verified river |
| `tools/capture-pixdiff.py` | the control check, with the threshold ladder that makes it decidable |

The actor makes **no shape decision of its own**. Centreline selection,
tracing and the Douglas-Peucker simplification that removes the raster staircase
all stay in `riverribbon.h`, where they are unit-tested in `ctest` without an
engine. The actor addresses tiles, budgets the work, converts millimetres to
world units and sweeps a ribbon. That is deliberate: the no-staircase property
must remain a tested property of voxel-core, not an untested property of an
engine file.

**Staging.** `riverribbon.h` says the host must budget the fill across ticks, so:
one 256-pixel block row per tick for the wet-mask fill (it decodes water blocks
off disk), then thin/trace/simplify in one go, then one reach re-meshed per tick.
Measured in the editor at a 4 km radius: **fill 1,296.6 ms across 17 ticks
(~76 ms/tick), trace 114.3 ms**; at 8 km, 34 ticks and 437.8 ms.

---

## 2. MEASURED — the width policy

The one real design question was whether to widen the ribbon so it holds a
minimum screen width at range. **It was measured, and it fails.**

`vxc_riverribbonprobe` over corridor tile (-11,-5), bv10 cache, 315 samples,
water datum minus the **amplified drawn ground**:

| widening | drawn width | fraction of the edge BELOW drawn ground |
|---|---|---|
| none (centreline) | 2.65 m | **0.00%** |
| 1x (natural edge) | 2.65 m | 1.59% |
| 2x | 5.30 m | 7.94% |
| 3x | 7.94 m | 24.13% |
| 4x | 10.59 m | 27.94% |
| 6x | 15.89 m | 46.03% |
| 8x | 21.18 m | **58.41%** |

Headroom at the unwidened centreline: **p10 +673 mm, p50 +829 mm, p90 +1004 mm**.

Read both ends together, because that is the finding. **The unwidened ribbon is
never buried; all burial is manufactured by widening.** The banks beside a reach
are higher than the water, and every metre of widening pushes the edge out onto
them where the depth test eats it. A 58%-buried ribbon is not a wide river, it
is a dashed line — and a dashed line that re-dashes as the camera moves is
exactly the shimmer this work was told not to ship.

**And widening does not buy the visibility it costs that for.** At 1440p / 90°
horizontal FOV, one pixel subtends 1/1280 in tangent:

| range | 2.65 m reach | to reach 2 px needs | burial at that widening |
|---|---|---|---|
| 1 km | 3.39 px | — | — |
| 5 km | 0.68 px | 7.81 m (3x) | 24.1% |
| 20 km | 0.17 px | 31.2 m (12x) | >58% |

**There is no widening factor that is both visible at 20 km and not buried.**

### The decision

Draw at the width the bake drew. No screen-width floor.
`-VoxelRiverRibbonMinPx=F` exists so the A/B can be taken and **defaults to 0**
— draw everything, report the numbers, let the owner judge the frame.

The honest conclusion, recorded so nobody re-derives it: **a 2.65 m creek is
genuinely below the resolution of a 20 km vista.** The way to put a river on
that horizon is to make the river wider *in the bake* where discharge says it
should be — `channel_width_m(Q)` deciding extent the way `water_depth_m(Q)`
already decides depth, which the handover flags as open and the owner's call.
A 16 m trunk river is 1 px at 20 km with no widening at all.

---

## 3. MEASURED — captures

Provider `terrain-diffusion-unlabeled-80b9ca451a23eae4-b4d02b092` (**bake_ver
10**), tiles `D:\voxelsim\tile-cache`. Site chosen from the probe's own reach
table, not by hand: the **2,362 m reach** in tile (-13,-5) at
`(-196214, -71650)` m, datum 346.6 m, mean width 1.87 m. Tile (-13,-5) is the
most interior baked tile — (-12,-5) and (-14,-5) are baked either side.

Every capture: sun frozen 12:00 03-20, 2560x1440, yaw 45, and every one settled
`jobsInFlight=0 pendingJobs=0 unloaded=0`.

| capture | alt | pitch | reaches | quads | sub-pixel | fine-tier leaks |
|---|---|---|---|---|---|---|
| `rr-1km-on` | 1 km | -35° | 8 | 101 | 3 of 8 | 0 |
| `rr-5km-on` | 5 km | -55° | 8 | 101 | 8 of 8 | 0 |
| `rr-20km-on` | 20 km | -75° | 9 | 102 | 9 of 9 | 0 |

Each has a `-VoxelRiverRibbons=0` control at the byte-identical shutter pose.
Pixel diff (`tools/capture-pixdiff.py`), changed pixels by threshold:

| range | @8 | @32 | @64 | @128 | max Δ | background drift |
|---|---|---|---|---|---|---|
| 1 km | 53,155 (1.44%) | 8,752 | 6,760 | **1,824** | 186 | 0.91 |
| 5 km | 7,430 (0.20%) | 40 | 2 | 0 | 74 | 0.46 |
| 20 km | 1,955 (0.053%) | 3 | 0 | 0 | 43 | 0.38 |

**The controls are sound.** Background drift (mean |Δ| over unchanged pixels) is
under one level in all three pairs — this is not the 85%-of-pixels exposure
failure recorded in the handover. And the difference *contracts* as the
threshold rises: at 1 km the strong signal collapses into a bbox covering 8.1%
of the frame, which is a localised feature and not a global shift.

The falloff is monotone and matches the width table: at 1 km the ribbon is a
solid feature; at 5 km and 20 km it is a faint sub-pixel contribution that never
exceeds Δ74 and Δ43 respectively.

**No claim is made here about how any of these frames look.** Conditions,
numbers and paths only; the owner judges the screenshots.

### Under camera motion

A still frame cannot show crawl, so the 5 km pose was re-shot with the camera
stepped **25 m and 50 m** along +X — about 6.4 px of image motion per step —
with its own control at each pose.

**The geometry is bit-stable.** All three poses log
`8 reach(es), 101 quad(s)`, identical. Nothing is re-traced, re-decimated or
re-simplified as the camera moves; there is no LOD pop in this actor, because
the ribbon is rebuilt only on a 1 km regather hysteresis or a near-field hole
change.

**The screen contribution is not stable.** Same site, same geometry, centroid
within 65 px of itself in all three frames — so the ribbon is not entering or
leaving the frame — and yet:

| pose | ribbon energy (Σ\|Δ\|) | changed px @8 | px @32 | max Δ |
|---|---|---|---|---|
| 0 m | 78,561 | 7,430 | 40 | 74 |
| 25 m | 212,747 | 8,663 | 1,095 | 142 |
| 50 m | 261,869 | 15,917 | 924 | 142 |

Energy varies by **99.4% of its mean** over 50 m of camera travel, and the count
of strongly-changed pixels swings **27x** (40 → 1,095) at essentially the same
screen position.

Energy alone does not prove crawl, though: moving the camera legitimately brings
more river into frame, and water is view-angle dependent besides. So the same
series was shot at **1 km**, where the identical reaches project to 3.39 px and
are comfortably supra-pixel. Geometry identical there too (8 reaches, 101
quads, and the rebuilt binary reproduced the pre-refactor build exactly):

| regime | energy spread | px above Δ32 | **peak Δ across the three poses** |
|---|---|---|---|
| 1 km — 3.39 px, supra-pixel | 50.2% of mean | 8,752 → 16,388 (1.9x) | **186, 180, 180** |
| 5 km — 0.68 px, sub-pixel | 99.4% of mean | 40 → 1,095 (27.4x) | **74, 142, 142** |

**The discriminator is the peak, not the energy.** Parallax changes how *much*
river is on screen — which is why energy and pixel counts move at both ranges,
and legitimately so. Parallax does not change how *bright the brightest ribbon
pixel* is. At 1 km the peak is flat to within 3% across 50 m of travel. At 5 km
it nearly doubles, 74 → 142.

That is coverage aliasing, and it is the failure mode the brief names: below one
pixel the ribbon's intensity is a function of where the pixel grid happens to
fall, not of what is in the world. It is **not** a defect in this actor's
geometry — the geometry is bit-identical across all three poses — and no change
to this actor fixes it. It is the same fact the width table states from the
other direction: below about 1 px there is nothing honest to draw, and the
answer is a wider river in the bake, not a wider ribbon at draw time.

---

### The near-field handover

The riskiest range is the closest one: 9–25 m, where the ribbon must hand over
to the real water voxels `RefreshImplicitWater` meshes inside its 52 m disc. Two
coplanar translucent surfaces at the same datum would z-fight and blend twice,
so the ribbon clips its segments against the disc's exact footprint
(Liang-Barsky in doubles, not a per-segment drop — a segment averages 14 m here
and the whole handover window is 16 m wide).

Shot at **10 m altitude, -20° pitch**, on the same reach. The near field
engaged: `RefreshImplicitWater: DRAINED refresh #2 -- 740 candidate(s) in
4 tick(s)`, and the capture script reports `implicit DRAINED x1` rather than the
`x0` every altitude capture above reports.

**The clip demonstrably executed**: the ribbon emitted **102 quads** at this pose
against **101** at every altitude pose, from the identical 8 reaches. One segment
was split in two by the disc boundary — which is exactly, and only, what a
segment crossing the edge of the hole should produce.

**This pair also caught a bug in the control check itself, and the correction
matters more than the capture.** `tools/capture-pixdiff.py` first flagged it as
exposure drift: 405,283 changed pixels (10.99% of frame) with mean |Δ| **2.10**
over the supposedly *unchanged* pixels, and a bbox that stayed at 100% of the
frame all the way up the threshold ladder — textbook contamination. It was
re-shot with `-Cvars 'r.EyeAdaptationQuality 0'` and came back **the same**
(|Δ| 2.13). The cvar was not the problem, because exposure was never the
problem:

| pair | signed mean | \|mean\| | bias ratio |
|---|---|---|---|
| near-field 10 m | −0.055 | 2.129 | **0.026** |
| 1 km | +0.031 | 0.914 | 0.034 |
| 5 km | +0.007 | 0.459 | 0.015 |
| 20 km | −0.001 | 0.378 | 0.003 |

An exposure or tonemap shift is a **bias** — every pixel moves the same way, so
the *signed* mean is large. What is actually here is **symmetric** — signed mean
≈ 0 against an absolute mean of 2.1 — which is temporal render noise: TAA
jitter and stochastic reflection sampling off a large translucent water surface
that fills much more of the frame at 10 m than at altitude.

The original tool tested `mean |delta| > 1.0`, which cannot tell a bias from
noise and would have condemned every water-heavy A/B this project will ever
take. It now reports both and keys the warning on the **ratio**. By that test
**all four controls are sound** (bias ratio ≤ 0.034); the near-field pair is
merely noisier, which weakens a small effect rather than invalidating it.

---

## 4. Known limits, stated rather than discovered later

**There is no bake_ver 11 tile cache on disk.** Phase 3 reports 173 components
and a 14,827 m longest reach, but the newest namespace carrying a water plane is
`-b4d02b092` at **bake_ver 10** — the fragmented one. Every number above is
therefore against bv10, whose longest corridor reach is 2,362 m and whose
components are 612 in one tile. Two newer namespaces were being written during this session and **neither had
produced a single `.vxtl` by the end of it** — `-b7817e0df` (06:25) and
`-b52995abb` (11:05), both flow tiles only. Re-check before assuming bv10 is
still the newest; when a bv11 fine cache exists, re-run the probe and the three
captures unchanged, since nothing in the actor or the producer is pinned to a
bake version.

Two consequences worth naming. First, the reach counts here (8–9 reaches inside
a 4 km window, longest 2,362 m) are bv10's fragmentation, not this actor's
reach. Second — and this is the one that matters for the milestone — **bv10's
p50 width of 1.87 m is the fragmented raster's centreline width**, so the
sub-pixel finding above is measured against the *narrowest* river this project
will ship. A width restoration moves every number in §2 and §3 in the same
direction at once.

**Water coverage is nine tiles.** Only the corridor is baked with a water plane;
`-bd3d0ddc7` has 38 tiles but is bake_ver 8 with **no water at all**. The 5 km
and 20 km captures were taken with `-VoxelFineTileGateFatal=0`, though in the
event **the gate never leaked** at any of the three altitudes — the fine tier
stayed resident. The switch was a precaution, and the logs record 0 leaks.

**The near/far tone problem is untouched.** The ribbon uses `M_WaterVoxel`, the
same material as the near-field voxels and the lake sheets, so the owner-tuned
constants apply to all three by construction. That fixes divergence, not tone;
Phase 5 stands.

**The unity-build fix was not optional.** `claude/water-integration` did **not**
build `VoxelEarthEditor` before this branch: `VoxelOceanCaptureFixture.cpp`
declared `FOceanRun`/`FRunRef` in an *anonymous* namespace, which under this
module's unity build collides with `VoxelSkyLadderFixture.cpp`'s identically
named symbols and produced a dozen "is not a member of `FOceanRun`" errors
pointing at the *other* file. Verified pre-existing by building the branch with
this actor removed. The fix is the one `VoxelSkyLadderFixture.cpp:892` already
documents having made for itself — a named namespace plus a hoisting
`using namespace`.

---

## 5. Capture inventory

All in `docs/water-map/captures/`. Every one settled
`jobsInFlight=0 pendingJobs=0 unloaded=0`; sun frozen 12:00 03-20; 2560x1440;
yaw 45; site `-VoxelSpawnAt=-196214,-71650` (the 2,362 m reach in tile
(-13,-5)); provider `...-80b9ca451a23eae4-b4d02b092`, **bake_ver 10**.

| file | alt | pitch | note |
|---|---|---|---|
| `rr-1km-{on,off,diff}` | 1 km | -35° | 3 of 8 reaches sub-pixel |
| `rr-5km-{on,off,diff}` | 5 km | -55° | 8 of 8 sub-pixel |
| `rr-20km-{on,off,diff}` | 20 km | -75° | 9 of 9 sub-pixel; `-VoxelRiverRibbonRangeM=8000` |
| `rr-1km-m{25,50}-{on,off}` | 1 km | -35° | motion series, +25/+50 m on X |
| `rr-5km-m{25,50}-{on,off}` | 5 km | -55° | motion series, +25/+50 m on X |
| `rr-nearfield-{on,off}` | 10 m | -20° | near field engaged, 740 candidate bricks |
| `rr-nearfield-fixedexp-{on,off,diff}` | 10 m | -20° | same, `r.EyeAdaptationQuality 0` |

`-off` is always `-VoxelRiverRibbons=0` at the byte-identical shutter pose, and
logs `River ribbons: DISABLED (-VoxelRiverRibbons=0)` — so a
suppressed-by-switch control is distinguishable from a
suppressed-by-absence one from the log alone.

The 5 km and 20 km captures carried `-VoxelFineTileGateFatal=0` as a precaution
against the corridor's partial coverage. **It was never needed: 0 gate leaks at
every altitude.** The terrain in all of them is real baked terrain.

---

## 6. Verification

```
vxc_tests   422 tests, 0 failures
ctest -C Release   2/2 passed (vxc_tests 113.81 s, vxc_editlog_selftest 0.06 s)
voxelcore.lib      MSVC (Visual Studio 18 2026) Release, clean
VoxelEarthEditor Win64 Development   Result: Succeeded
```
