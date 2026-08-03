# Cave entrances and chambers still read as stamped shapes — the rework

**Status: documented, deferred.** Owner's call on 2026-08-03: water (ocean,
rivers, lakes) is the priority for the day; this is picked up afterwards.

This supersedes nothing. W3 (v25 entrances) and W4 (v26 G2 chambers) are
committed, measured and correct against the goals they were given. The owner
looked at their output and rejected it anyway, in these words:

> Cave cross sections and entrances still read as incredibly unnatural, stamped
> geometric shapes that have been cookie cutter stamped into the otherwise
> natural looking surface terrain. Same with doline.

So the waves did what they promised and the promise was insufficient. This
document records *why*, while the evidence is fresh, so the next attempt does
not rediscover it.

---

## 1. The diagnosis: a primitive clipped by terrain is still a primitive

W3's own design note (`caves.h:206-240`) states the intent clearly and honestly:
one construct, and *"nothing below branches on landform; the terrain does the
branching"*. Flat ground gives a doline, falling ground gives a mountainside
mouth, a one-axis fall gives streambed-capture geometry.

That is true, and it is not enough. **"The terrain does the branching" means the
terrain CLIPS.** A lens is authored in its own parameter space and then cut by
the ground surface. Clipping a primitive with terrain yields a primitive with a
terrain-shaped bite out of it. It does not yield a feature that the terrain
looks like it produced.

The same pattern produced the same complaint about water in the same session —
the far-field lake sheet is axis-aligned rectangles intersected with the wet
mask, and the owner called it *"sharp, rectangular, square edges where it meets
land rather than a natural curving, arching shoreline"*. One root cause, two
subsystems. **In both, the tell is a straight line or a flat plane that no
natural process would produce.**

This is also the shape of the two fixes that have actually worked on this
project: the banding was solved when the mechanism became physical
(`voxelsim-banding-3d-density-blindspot`), and the terracing was solved the same
way. Neither was solved by making a shape prettier.

## 2. The specific tells, measured

1. **A perfectly level floor.** `kCaveEntranceFloorMinMm` puts the floor at an
   absolute z, `[5, 9) m` below the node's own surface — a horizontal plane in a
   world where nothing else is flat. The justification on record is gameplay (a
   mob can stand on it, plan §5.6), not naturalness. W4's rubble adds **47 mm**
   of relief to a 5-9 m deep bowl: **0.5%**. It is still a plane.

2. **A smooth analytic roof.** The lens rises to `axisRise` over the node and
   tapers to the floor at `reach`. A quadric surface.

3. **Only the RIM is noised.** `kCaveEntranceRim*` warps the plan outline. The
   roof and floor surfaces get nothing. The silhouette wobbles while the shape
   stays a lens, which is why it reads as a blob rather than as a cave.

4. **No detail band on the cavity, while the terrain around it has one.** The
   surrounding surface carries v23 amplifier detail down to 10 cm; the cavity
   boundary is a clean analytic surface at the same 10 cm. **Smooth geometry
   embedded in rough geometry is the literal definition of "stamped in."** This
   is probably the single strongest contributor and the cheapest to test.

5. **Uniform size on a lattice.** Floor depth spans `[5, 9) m` and density is one
   candidate per 4x4 lattice cells gated 1-in-4 — about one entrance per 205 m
   square. Real collapse and karst features have heavy-tailed size distributions
   and cluster along joints, faults and drainage lines. Evenly spaced,
   same-sized features read as placed however good each one is.

A fairness note that should travel with the cross-section renders: a 2D slice is
the harshest possible view of an analytic surface — it shows the lens as a
perfect arc. But the owner independently rejected the **in-engine** doline
(`w3-25-doline-close.png`, `w3-25-doline-aerial.png`), so the cross-sections are
not inventing the problem.

## 3. The direction

**An entrance should not be a shape. It should be where a process broke
through.** The two mechanisms that produce real cave openings:

* **Roof failure.** A passage runs close enough to the surface that its roof
  cannot hold, and collapses. The opening's outline is then the *passage's* plan
  shape plus the failure's, its depth is the passage's depth, and its floor is
  breakdown — the fallen roof — not a level plane. Sizes inherit the passage's
  own size distribution, which is already heavy-tailed, and clustering comes free
  because passages are not uniformly distributed.
* **Drainage capture.** A surface drainage line reaches a void and goes
  underground. The outline is the *channel's*, so it is elongated along flow by
  construction rather than by a special case, and its floor grades downhill.

Both derive outline, depth, floor and size from something that already exists
in the world, rather than from `reach` and `floorDepth`. That is the difference
between "we clipped a lens" and "the ground collapsed here".

**Cheap experiments to run first**, because one of them may clear the bar
without the redesign, and they are hours rather than days:

* Give the cavity surface the same detail band the terrain has (tell 4).
* Replace the level floor with breakdown relief at metres, not centimetres
  (tell 1) — W4's 47 mm is two orders of magnitude short.
* Widen the size distribution and let density follow a structural field rather
  than a uniform lattice (tell 5).

If those close it, the redesign is unnecessary. **Measure before choosing** —
that is what W3 and W4 both got right.

## 4. What to keep

None of the following should be re-litigated. All of it was expensive and all of
it is sound:

* **The entrance guarantee.** Every open site is daylit and reaches a backbone
  node. The v24 bore survives inside the cavity as the *throat* precisely so this
  stays structural rather than arithmetic. Any redesign must preserve it or
  replace it with something equally structural.
* **The single-term control.** W3's `--entrance-off` and W4's
  `cavernSiteWithoutChamberShape` compare one term inside one world, on identical
  columns, terrain, seed and predicate. Every credible claim in both waves came
  from this. Two-build comparisons do not count.
* **The instruments.** `vxc_caveprobe` (with `--scale`/`--coarse-dir`, since it
  used to measure the 30 m coarse raster while the game draws the fine tier and
  node surfaces move up to 27 m), the ctest census gates, and the per-region
  `exercised: N cave / N entrance / N cavern` line in `vxc_gpu`.
* **The statistics discipline.** Both waves caught themselves reporting a number
  satisfied by their own construct rather than by the terrain — W3's "55% thin
  roof" measured its lens pinching to nothing near the rim, W4's first floor
  statistic measured chain geometry rather than rubble. Ask what a statistic
  reads on flat featureless ground before gating on it, and state the control's
  measured noise floor rather than implying a clean zero.
* **W5's field coupling** (temperature and relief gating density and calibre) is
  orthogonal to shape and remains wanted.

## 5. Sequencing

Deferred behind water. When it resumes: run the three cheap experiments in §3,
measure with the existing controls, and only then decide whether the
process-driven redesign is needed. The owner judges the frames; do not ship a
verdict on appearance.
