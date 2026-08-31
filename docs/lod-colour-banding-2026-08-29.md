# LOD colour banding: diagnosed, and the fix that was already in the tree

**Owner-reported, restated 2026-08-29:** *"LOD0 in the alpine area is almost pure
white ... that decreases as you look further and further out at distant LODs.
More specifically, there is more and more brown on the side voxel faces at more
distant. The colour and shading scheme should be consistent regardless of the
LOD."*

This is backlog **0.0b**, first reported 2026-08-23. The mechanism was diagnosed
then and a fix was written on all three generation paths -- and then never
validated, so it shipped switched off and stayed off for six days.

## The mechanism, and why the effect is so sharp

Surface colour comes from a stratigraphy layer whose thickness is clamped at
`kTopsoilMinMm = 100` (`voxel-core/shaders/worldgen.ush:275`). The comment there
states the intent: *"guarantees at least one visible biome-coloured voxel on
every column."* **One voxel -- and only at level 0, where a voxel is 100 mm.**

Thickness is scaled down by slope (`retainQ10`), so **steep alpine ground sits at
that 100 mm floor**, which is why the defect is worst exactly where the owner
saw it.

Coarse levels take ONE representative sample per coarse cell, and that sample
lands up to a whole coarse voxel BELOW the true surface:

    ring  voxel    sample lands below surface   100 mm cap survives?
      0   0.1 m    0                            yes  -- pure white
      1   0.2 m    up to 0.1 m                  ~half the columns
      2   0.4 m    up to 0.3 m                  rarely
      3+  0.8 m+   up to 0.7 m+                 no -- MAT_ROCK / MAT_SUBSOIL

**So the distant terrain is not shaded differently. It is made of different
material.** That is also why it reads worst on SIDE faces: a coarse riser is a
0.8-6 m wall of body rock, where the same wall at level 0 is a stack of
snow-topped 10 cm steps.

A second path compounds it for mip'd/edited ground: `downsampleBricks` takes a
majority vote of 8 children and **breaks ties to the LOWEST material id**, and
`MAT_ROCK = 2` sits below `MAT_SNOW = 7` and `MAT_GRASS = 8` (`mips.h:22`).

## What the fix does

`-VoxelSurfaceMip=1` makes the ONE coarse cell per column whose representative
span contains the level-0 surface voxel resample AT that voxel. Wired through
all three producers from a single accessor (`VoxelGpuWorldGen::SurfaceMipEnabled`):
the CPU mip builder, the CPU coarse generator, and the GPU worldgen kernel.

**Occupancy is byte-identical either way** -- the resample is adopted only when
non-air, so every quad, pack and streaming decision is unchanged. It is a COLOUR
change. No re-bake, no world re-key, no worldgen version bump.

**It is dead at level 0 by construction** (`coarseScale > 1`), which supplies a
falsifier the measurement below uses.

## The gap that was closed first

The switch printed NOTHING in either position. An A/B that came back at the
noise floor could not be told apart from a switch that never armed -- the house
failure mode. `SurfaceMipEnabled()` now logs which way it latched, once, from
the single derivation.

    LogVoxelGpu: Surface-preserving coarse materials (-VoxelSurfaceMip): ON. ...

## The measurement

Three matched captures, alpine at `-54233,-68221`, 80 m above the surface,
pitch -18, yaw 45, sun pinned 12:00 03-20, 2560x1440, settled 120 s. Two
controls give the noise floor; the third differs in the flag alone. Engagement
proved by the log line above in each leg.

    pixel diff (thresh 8/255)     changed
    control-b vs control-a          0.99%     <- noise floor
    armed    vs control-a          17.26%     <- 17x the floor
    at thresh 96:  floor 0.00%, armed 4.45%

Per-band, over terrain pixels only (sky excluded by a blue-dominance test, not a
row cutoff). `brown` is `(R-B)/(R+G+B)` -- a HUE measure, so it does not move with
exposure or sun angle. Screen row is a monotonic proxy for ground distance.

    band  rows        floor%  effect%   brown OFF  brown ON   delta
      2   288-432       1.72    59.88    +0.0277   -0.0038   -0.0315
      3   432-576       2.16    58.78    +0.0668   +0.0122   -0.0546
      4   576-720       1.91    49.07    +0.0653   +0.0091   -0.0562
      5   720-864       1.28    24.48    +0.0393   +0.0125   -0.0268
      6   864-1008      0.67     8.26    +0.0109   +0.0043   -0.0066
      7  1008-1152      0.40     6.78    +0.0193   +0.0116   -0.0077
      8  1152-1296      0.23     3.24    +0.0217   +0.0173   -0.0044
      9  1296-1440      0.22     0.51    +0.0180   +0.0175   -0.0004

**Read row 9 first.** It is the nearest ground, level 0, where the switch is
dead -- and it moves 0.51% against a 0.22% floor. **The falsifier passes: the arm
could have changed the near field and did not.**

**Read the OFF column next.** Brownness peaks at the mid rings (+0.067) and is
LOWEST on the near ground (+0.018). That gradient IS the owner's report,
measured.

**Then the delta.** The mid rings lose 82-86% of their brown excess and land at
+0.009..+0.013 against a near-field +0.0175. **The distant terrain now matches
the near terrain's hue, which is the stated requirement.**

### The aerial pair the owner asked for

Same column, four more matched captures -- 250 m at pitch -45 and 600 m at
pitch -40, each arm, poses identical to the centimetre. Engagement proved by the
latch line in all four legs.

    250 m / -45 deg      changed%   brown OFF   brown ON     delta
      far (top of frame)   75.14     +0.0571     -0.0113    -0.0684
      mid                  64.69     +0.0835     +0.0069    -0.0766
      mid                  51.97     +0.0865     +0.0138    -0.0727
      near (bottom)         5.61     +0.0214     +0.0146    -0.0067

    600 m / -40 deg      changed%   brown OFF   brown ON     delta
      mid                  85.24     +0.0815     +0.0003    -0.0812
      mid                  78.57     +0.0978     +0.0109    -0.0869
      near-ish (bottom)    59.05     +0.0899     +0.0130    -0.0769

**The 600 m pose has no level-0 ground in frame at all** -- altitude sets a floor
on distance, so every pixel is a coarse ring and 51-85% of them move. That is
the honest picture of what the defect costs from the air. It also means **this
pose carries no built-in falsifier**; the 250 m pose does (its bottom band is
near ground and moves 5.61% against a far field moving 75%), and the 80 m vista
above carries the strongest one (0.51% against a 0.22% floor).

Under the fix every band across both aerial poses lands in +0.000..+0.015,
against an OFF spread of +0.021..+0.104. **The banding is not reduced, it is
flat.**

## What this does NOT cover

A second, independent LOD-dependent colour term is still live and unmeasured:
the surface-proximity gate uses a **fixed 200 UU band**
(`kSurfaceBandUU`, `VoxelMarch.usf:2716` and `VoxelQuadVertexFactory.ush:323`)
against a plane fitted across a whole chunk. At level 5 a chunk spans ~205 m, so
the fit residual dwarfs the band -- and the band is smaller than ONE voxel
(200 UU vs 640 UU). Top faces that fail it are repainted with the SIDE palette
entry.

**It is a weaker suspect and should not be touched on suspicion**: for MAT_SNOW,
TOP and SIDE differ by ~5%, and for MAT_GRASS the SIDE entry is a darker green,
not a brown. Pick it up only if the owner still sees banding with the flag on.

The third effect named in backlog 0.0b -- `solidThreshold = 4` eroding a sloped
surface by up to half a voxel per level -- is GEOMETRY, deliberately untouched
here, and is a separate decision with worldgen-version consequences.

## Status: SHIPPED DEFAULT 1, 2026-08-29

**Owner's verdict on the matched captures: "Bottom half with fix looks great."**
`SurfaceMipEnabled()` now defaults to 1
(`VoxelEarthShaders/Private/VoxelGpuWorldGen.cpp`).

**The revert is one word on the command line: `-VoxelSurfaceMip=0`.** Nothing
else in the project depends on it, occupancy is byte-identical in both
positions, and neither direction needs a re-bake, a re-key or a worldgen
version bump.

The accessor's log line now reads `ON (default)` / `OFF -- reverted by
-VoxelSurfaceMip=0`, so a leg's own log says which arm it is without anyone
having to reconstruct the command line.

**What is NOT measured: the cost.** The arm adds one extra `materialAt` call for
the single cell per column that straddles the surface, on coarse levels only. It
is expected to be lost in noise and no timing leg has been run. Do not quote it
as free.
