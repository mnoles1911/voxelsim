# The near-field water disc: what the rebuild actually costs

> **STATUS 2026-08-09: [CURRENT].** This is a shipped-infrastructure
> performance record (`waterwindow.h`, incremental refresh, the ground-floor
> fix). The re-architecture retires *river* meshing from the near-field
> implicit path at Phase 5, but lakes keep using implicit fill (Phase 2's
> "sheet actor + implicit fill" hooks), so this window/incremental machinery
> and its ground-floor fix remain load-bearing for whatever stays on that path.

Written 2026-08-05 on `claude/water-ring-invalidation`. Raw numbers in
`docs/measurements/water-refresh-2026-08-05.txt`, produced by
`vxc_waterrefreshprobe` over the bv14 tiles and `D:/vox-wet-cache`.

**Read §1 first. The headline is that the thing this task was scoped around is
not the bottleneck, and the real one is next to it.**

---

## 1. The verdict

The far-water plan (§4.2 item 3) names full-disc rebuild as "the single biggest
performance risk and it has not been measured". It is now measured, per second
of flight rather than per rebuild, and:

**Full-disc rebuild was NOT the bottleneck. The vertical candidate range was.**

At a low pass over the bv14 braided reach (2 m AGL, 15 m/s), per second:

| | rebuild (today) | incremental | incremental + ground floor |
|---|---|---|---|
| candidate columns swept | 56,193 | 2,125 | 2,125 |
| candidate bricks offered | 899,080 | 34,005 | **10,627** |
| bricks re-meshed | 11,520 | 11,520 | 9,284 |
| quads regenerated | 4,053 | 4,038 | **10,429** |
| cpu in sweep | 135 ms/s | 21 ms/s | **6 ms/s** |
| cpu in mesh | 459 ms/s | 461 ms/s | 383 ms/s |
| worst frame in flight | 24.3 ms | 13.3 ms | **12.5 ms** |

The row that decides it is **bricks re-meshed**: 11,520/s in both the rebuild
and the incremental scheme, a ratio of exactly 1.0. That is not "the change did
nothing". It is `kMaxImplicitMeshesPerTick` (192) times 60 fps — **the mesher is
pinned at its budget and has been the whole time**. Making the sweep 26x cheaper
cannot move a number that is set by the drain budget.

The demand tells the same story from the other side:

| scheme | demand | vs drain capacity | queue |
|---|---|---|---|
| full-disc rebuild | 899,080 /s | **78.0x** | never empties |
| incremental | 34,005 /s | **2.95x** | never empties |
| incremental + ground floor | 10,627 /s | **0.92x** | drains |

**Only the third scheme gets under capacity**, and it is the only one where the
water disc ever finishes building. That is the redirect: the resolution work
should be aimed at what the sweep OFFERS vertically, not at how often it runs.

The same three rows reproduce on the dense `vox-wet-cache` reach at 78.1x /
2.94x / 0.98x, so this is not one site's shape.

### Why the offered count was so far above the drawn count

The sweep offers every brick from the box FLOOR up to the flood level **with no
lower bound from the ground at all**. Measured at one camera on the braided
reach: of 67,600 bricks offered, **46,475 (68.8%) are provably underground**, and
only **12.5%** of what the drain actually meshed emitted a single quad. Seven
eighths of a saturated budget was being spent on rock.

`Amplifier::surfaceLowerBoundMm` already existed in voxel-core and is the exact
mirror of the `surfaceUpperBoundMm` the sweep already calls for its ceiling. It
is now called for the floor. Non-empty share of meshed bricks goes 12.5% -> 40%,
and **quads per second goes UP 2.6x** — the disc is drawing more water, sooner,
for less work, because the budget stopped going to buried bricks.

### The number that is NOT an improvement, and why it is still the point

Worst frame in steady flight goes 24.3 ms -> 12.5 ms, a 1.9x cut, and the sweep
half of it goes 15.8 ms -> 0.9 ms (17x). But the remaining 12.5 ms is nearly all
mesh, and it is mesh that is now *useful*. A scheme that halves the average and
keeps the spike has not fixed the problem; this one halves the spike, and what
survives is work that puts water on screen.

---

## 2. A second defect found on the way, and it is a leak

`RefreshImplicitWater` never destroyed the `UWaterChunkComponent` of a brick that
left the box. The rebuild `Reset()` the pending list and re-swept the new box, so
a departed brick was simply never revisited; nothing anywhere pruned by distance.
Flying therefore accumulated implicit water components without bound — a
one-brick-wide shell left behind every 0.8 m step, all still registered and still
drawing.

The eviction pass the incremental scheme needs anyway is the fix. This was not
visible in any per-rebuild number, which is why it survived: the candidate count
looked identical whether or not the components behind the camera still existed.

---

## 3. What shipped

**`voxel-core/include/voxelcore/waterwindow.h`** — the window rules, integer-only.
`waterWindowCentreBrick`, `WaterWindow`, `waterWindowAt`, `waterWindowIntersect`,
`waterWindowDifference`, `waterWindowColumnDifference`.

The load-bearing observation is that **the candidate predicate never mentions the
camera**. A column is wet or not; a brick is under its flood level or not; a
brick is proven-interior or not. The camera contributes only the box that clips
it. So `candidates(camera) = P ∩ Box(camera)`, and a camera step is a box
difference — the overlap is provably unchanged and already meshed. The
incremental result is therefore **bit-identical** to the rebuild it replaces:
there is no staleness to trade, no hysteresis to tune. `vxc_waterrefreshprobe`
checks it every frame and fails on one disagreement.

**Per-ring invalidation falls out of the same rule** rather than being a separate
throttle. Quantise a ring's window to its OWN brick size (`(8 << lod)` voxels) and
a camera step smaller than one of that ring's bricks does not move the window at
all. Ring L then refreshes exactly 2^L times less often than ring 0 — pinned by
`each_ring_refreshes_half_as_often_as_the_one_below`. When the far cascade lands,
it needs no rebuild-rate logic of its own.

**`voxel-core/bench/waterrefreshprobe.cpp`** — `vxc_waterrefreshprobe`. Runs the
three schemes over one flight path on real tiles, meshes with `meshBrick<8>` at
the client's own 192/tick budget, and carries two gates: the frame-by-frame
equivalence check above, and a proof check that meshes every brick the ground
floor rejects and fails on a single non-empty one.

**14 tests** in `voxel-core/tests/test_waterwindow.cpp`.

**`UVoxelWorldSubsystem::GetSurfaceLowerBoundMm`** — the mirror of the existing
upper-bound accessor.

**`RefreshImplicitWater`** — window difference instead of rebuild, a per-column
cache, an eviction pass, the ground floor, and a containment test at drain-pop so
a queued brick cannot outlive the window that queued it.

### Gates

```
vxc_tests   478, 0 failures   (464 on this origin/main + 14 new)
ctest       2/2  -C Release
UE module   Result: Succeeded (Build.bat VoxelEarthEditor Win64 Development)
clang       -Wall -Wextra -Werror clean on both changed voxel-core TUs
```

`TERRAIN_VERSION` untouched; no bake change; nothing in `terrain-service` touched.

---

## 4. Things that are settled, so nobody re-derives them

- **A candidate count is not a draw count, and the gap is vertical.** 71,825
  offered against ~8,400 meshing non-empty was already on record; this run says
  where the 88% goes — it is *below the ground*, not spread thinly. That is why
  a floor fixes it and a smaller radius would not.
- **`kImplicitRadiusBricksZ = 16` makes the disc empty at flight altitude.** Case
  2 in the measurements file: the same camera at 40 m AGL over the same water
  offers **zero bricks** across the entire flight, while still re-sweeping 4,225
  columns 26.6 times a second. The sweep runs for nothing. `farwater.h`'s
  `farWaterBrickRange` (ground->datum, camera-free) is the standing fix and this
  is a second, independent measurement of why it is needed.
- **Per-second is the only honest unit here.** Per-rebuild numbers cannot
  distinguish a cheap sweep that runs 40 times a second from an expensive one
  that runs twice, and the rebuild rate is set by speed, which is the variable
  the complaint is about.
- **"No improvement" and "already at the ceiling" look identical** in a
  bricks-re-meshed row. The probe prints demand against drain capacity for that
  reason; without it the incremental scheme reads as a null result.
- **The ms figures are softer than the counts.** This box was shared with a bake
  for part of the run (`tools/voxel-measure-guard.ps1` exists for exactly that).
  The counts — columns, bricks, quads, demand-vs-capacity — are deterministic and
  carry the argument; the millisecond columns are indicative.
