# Is an interval band worth building? — the empirical answer is NULL, and why

**Date:** 2026-08-19 · **Tool:** `vxc_caveprobe` (`voxel-core/bench/caveprobe.cpp`, new)
**Data:** `karst-interval-band-2026-08-19.txt` · **Binary rebuilt immediately before the run.**
**Context:** the karst plan's Phase 1 residency pivot.

## The question

Underground admission today uses a **half-space**: `FFootprintBand` carries one number per
footprint, `SolidBelowVoxel` — "everything strictly below this z is solid". The karst plan
proposes replacing it with an **interval band**, ≤N z-spans per footprint, on the argument that a
multi-storey cave system leaves provably-solid rock *between* storeys that a half-space cannot
express. The plan's Phase 1 rests on that saving being real.

Measured against **today's** caves, over 15 baked fine tiles at bake_ver 28, 1,024 level-0
footprints per tile, a 200 m window below each column's own surface, 8×8 sampled columns per
3.2 m footprint:

| | value |
|---|---|
| footprints containing any subsurface air | **24.4%** |
| air spans per footprint, where there is air | **~1.1** |
| column provably solid | **99.4–100%** |
| admitted level-0 chunks, half-space (cap 1), where there is air | **2.30–2.97** |
| admitted level-0 chunks, exact interval set, where there is air | **2.30–2.96** |
| **saving from an interval band** | **0.2% mean, 1.2% best tile** |

## The finding

**On today's world an interval band saves nothing, and the reason is structural rather than
marginal: today's caves produce exactly ONE air span per footprint.** With one span, a half-space
bound from the surface to the bottom of that span is already tight — there is no second storey to
skip the rock in front of. The mechanism the pivot exists to exploit is not present in the world
being measured.

**This is a null result, not a refutation.** It neither clears the ≥50% bar nor falls below the
25% kill bar; it says the experiment cannot decide the question, because the structure under test
does not exist yet. The decision has to come from the karst design's own storey geometry.

### The arithmetic, labelled as arithmetic

Using the plan's storey depths (9–37 m, 40–110 m, 110–175 m) with 5×-scale passages ~15 m tall,
a footprint crossing one passage per storey:

| rule | admitted level-0 chunks |
|---|---|
| half-space, surface → deepest air (~147 m) | ~46 |
| interval set, 3 spans × ~15 m | ~15 |
| **saving** | **~67%** |

So the pivot pays **iff the world is multi-storey**, and by roughly the margin the plan claims.
That is a derivation from design intent, not a measurement of anything, and it must not be quoted
as one. **The honest sequence is: build the generator first, then re-run this probe against it.**
`vxc_caveprobe` is written to do exactly that with no changes — it reads whatever `materialAt`
produces.

## Three bugs found in this probe before the number was believed

Recorded because each one produced a *plausible* answer, and the first two would have shipped a
fabricated justification for building the pivot.

1. **Window top vs. column top.** The window starts at the footprint's *max* surface, so on
   sloping ground every column below that maximum contributed its open air as a "span". Tell:
   100% of footprints reported air while columns measured 99% solid. Fix: clip each column to its
   own surface.
2. **`floorDiv(surfaceMm, kVoxelSizeMm)` is still an air voxel.** `stratigraphyAt` tests the voxel
   *centre*, so the top voxel is air whenever the surface falls in the lower half of its cell —
   half of all columns. Tell: 99.8% of footprints still reported air after fix 1. Fix: derive the
   clip from the predicate, `floorDiv(surfaceMm - kVoxelSizeMm/2, kVoxelSizeMm)`.
3. **Double-counted boundary chunks.** `chunksTouched` summed per-span chunk counts, so two spans
   sharing a boundary chunk counted it twice. Tell: the *uncapped* set reported MORE admitted
   chunks than a capped one, which is impossible — merging can only add chunks.

With bugs 1 and 3 present the probe reported a **35.4% mean saving** and a per-tile range of
0–45.6%. That number is wrong and appears nowhere above. It was reading terrain relief, not caves.

## Method notes

* Air is `Amplifier::materialAt(col, vz) == MAT_AIR` — the binding collision uses and the one
  `worldgen.ush` mirrors. The probe does **not** re-derive intervals from `CaveColumn` /
  `CavernColumn` internals; that is how a probe ends up measuring a world the engine does not run.
* Capping merges the smallest gap first, which is conservative: the merged span covers everything
  both spans covered plus the rock between them. Over-admitting wastes work; under-admitting is a
  hole in the world.
* Tile `-15_-7` reports zero air everywhere — it is below `kCaveMinSurfaceMm` (12 m), so the cave
  pass declines it by definition. It is left in the table rather than dropped.
* The probe deliberately reports **admitted chunk count only** — not quads, VRAM or frame time.
  Those belong to `vxc_volumeprobe` and to an in-engine leg.

## What this changes in the plan

* Phase 1's interval band **must not be justified by a measurement of today's world**, and the
  plan should say the empirical test returned null.
* Building the band against today's caves is still the right sequencing — it is a strict
  refinement of an already-`static_assert`ed bound and can be proven correct there — but its
  *payoff* cannot be demonstrated until the karst generator produces storeys.
* Re-run this probe as the acceptance gate the first time the new generator carves anything.
