# Retiring the clipmap: voxels as far as the eye can see

**Owner directive, 2026-08-30:** *"retire the clip map entirely, build more LODs
and do it in the most performant way possible and efficiently."*

## The cost driver is RING COUNT, not distance

This is the fact the whole design turns on, and it is measured, not assumed.
Marcher cost is ray-count linear with immovable per-ray cost
([[voxelsim-marcher-cost-is-ray-count]]), and what a ring adds is **segment
entries per ray** -- roughly 0.9 ms per ring at a sky-facing pose
([[voxelsim-segment-entry-is-the-cost]]). A ring's cost does NOT scale with how
wide it is. So covering 8 -> 65 km in three doubling rings costs three segment
entries on every ray, forever, while the clipmap it replaces costs ~zero.

**Therefore: use the FEWEST rings that keep voxels acceptably small.** Do not
simply append R8, R9, R10.

    ring    span            voxel     apparent size at the ring's INNER edge
    R8      8 -> 16 km      12.8 m    1.6 mrad  ~2.6 px
    R9     16 -> 32 km      25.6 m    1.6 mrad  ~2.6 px
    R10    32 -> 65 km      51.2 m    1.6 mrad  ~2.6 px

The doubling cascade holds apparent voxel size CONSTANT (~2.6 px) at every
ring's inner edge -- that is the cone rule working. So the question is not "is
R10 too coarse" but "how few rings can cover 65 km without a visible step".

**MEASURED 2026-08-30, AND IT CHANGES THIS SECTION.** R7 was measured at a
genuine sky-facing pose (`-VoxelPerfFlight=static -VoxelPerfPitch=25`,
engagement proven by `STATIC pose pinned at ... pitch=25.0` in the log):

    leg            p50     p95     p99
    4 km ctl-a    7.00    7.50    7.90
    4 km ctl-b    7.00    7.50    8.10
    8 km arm-a    7.00    7.50    8.30
    8 km arm-b    7.00    7.50    8.10

**Zero measurable cost at p50 and p95.** p99 is UNRESOLVED -- the arms average
+0.20 ms and the two controls differ by exactly that, so it is inside noise.

**So the ~0.9 ms/ring segment-entry figure does NOT describe the marginal cost
of an outer ring**, and the two-ring design it motivated is unnecessary. **Use
three doubling rings** (R8 16 km, R9 32 km, R10 65 km) and keep
`OuterUU(L) = R0 * 2^L`, the identity the marcher derives from ONE uniform.
Dropping the wide ring removes the plan's main design risk.

**The caveat that survives:** R7's cheapness may not extrapolate. Each ring
quadruples its annulus area, so R8-R10 each stream more chunks than R7 did, and
a STATIC pose admits nothing. The level-pose FLIGHT legs are where cost showed
(+0.10 ms p95, +0.07pp stutters, replicated twice). **Measure every new ring at
BOTH poses before adding the next.**

## Tier: levels 0-7 fine, R8+ COARSE-DERIVED

`VoxelRasterAtlas.h:18-27` states the constraint: **"PITCH IS A WORLD PROPERTY,
NOT AN LOD"** -- the two pitches select different octave ladders in the
amplifier, so the same ground has a different SHAPE depending on which tier
generated it, and a far chunk built from coarse pixels would diverge from the
CPU reference and the collision world.

**It is still viable, because the clipmap ALREADY reads coarse**
(`SampleTerrainHeightUU` -> `Impl->Tiles`, the 30 m sampler). The world already
has coarse-derived far field meeting fine-derived near field with a shape
discontinuity at the cascade edge. Making R8+ coarse-derived MOVES that
discontinuity; it does not create one.

Rules that keep it sound:

1. **Levels 0-7 stay fine. Only NEW levels are coarse-derived.** Nothing that
   exists today changes shape: no re-key, no worldgen version bump.
2. **The level -> tier rule is ONE function**, consulted by the GPU dispatch,
   the CPU reference AND the atlas. Two spellings is exactly the cross-arm
   divergence the atlas comment forbids -- and see the coarse-scale clamp in
   `docs/cascade-8km-r7-2026-08-30.md` for what a comment-synchronised constant
   across a module boundary actually costs.
3. **It is keyed on LEVEL, never on residency.** A residency-dependent fallback
   makes two clients disagree; a level rule is deterministic and reproducible.
   This is the distinction that separates it from the sea-level fabrication the
   fine-tier gate exists to refuse.

**Why this is also the enabler.** Voxels to 65 km on the fine tier would demand
~289 fine tiles against the 15 that exist, and today's bake refused to extend
that because the COARSE upstream is missing too (incomplete flow superblocks --
a truncated catchment routes differently depending on which neighbours happen to
exist). Coarse-derived outer rings collapse that demand to roughly today's.

**Second raster atlas at coarse pitch.** The atlas deliberately holds one pitch;
outer rings need a second at 30 m. Cheap: ~2.6 MiB against the fine atlas's
~610 MiB (both logged at init).

## What retiring the clipmap actually returns

* The seam, and the entire two-colour-authority bug class, stop being possible.
* `AVoxelClipmapActor`'s GAME-THREAD rebuild disappears -- 65x65 vertices per
  level, each now costing a climate sample and a `vxc::classifyBiome` call, and
  the p99 tail here is game-thread-owned.
* One representation to reason about.

## Measured, and what is still unknown

    R7 (the 8 km ring), matched alternated legs, 22.5 m/s, n~15,200:
        p50 +0.05 ms   p95 +0.10 ms   p99 +0.70 ms   chunks +4.6%
        (both controls agreed exactly at p99 16.00)

**Two inherited estimates were wrong and are now measured**: streaming was
predicted +14%/level and is +4.6%; frame cost was predicted ~+0.9 ms/level and
is +0.05 at p50. **Do not quote either old figure again.**

**STILL UNKNOWN, and it is the number that decides the whole programme: the
per-ring cost at a SKY-FACING pose.** R7 was measured at the flight's default
pitch only. If the 0.9 ms/ring segment-entry figure holds at sky, three rings is
~+2.7 ms against a 7.9 ms frame and the plan needs the two-ring shape or
rethinking. **Measure this before writing any traversal code.**

## STATUS 2026-08-30, and the consumer I under-scoped

**Landed, inert, and each proved by its own log line BEFORE anything depended on
it** -- which is why the R8 setback cost one constant instead of a revert:

* `VoxelTier::IsCoarseDerivedLevel` in VoxelEarthShaders, readable by both
  modules. Returns false for every streamed level today.
* The coarse raster atlas: `created at 30000 mm/px (fine atlas is 1875)`.
  Built only when the two pitches differ; ticked while inert so it is warm.
* Tier-aware raster routing. Atlas AND sampler both derive from ONE call to the
  rule, so they cannot disagree about pitch.
* R8's whole format sweep: preset, cap share, `kLevels` 9, cover level 9,
  grid-slot arrays to [10], hole-word to 9, census print to 9.

**R8 IS BUILT AND PARKED** at `kDefaultMaxRingLevel = 7`. An unstreamed level
costs "+8 MiB of buffer and zero extra ROUTINE traffic" (VoxelMarchChunkIndex.h)
-- flipping that one constant streams it.

### THE BLOCKER: streaming admission is a FOURTH consumer of the tier rule

This plan said "one rule, THREE consumers" -- GPU dispatch, CPU reference,
atlas. **Wrong.** `RecomputeDesiredSet -> EnumerateSurfaceFootprintCandidates ->
FootprintChunkZRangeCached -> ComputeFootprintChunkZRange` queries surface
elevation to decide WHICH CHUNKS EXIST, and it does so through
`Voxels.amplifier().column()`. **The amplifier is bound to ONE tier** (the
`isFineTier` branch VoxelRasterAtlas.h names), so admission asked the fine tier
for ground 16 km out and hit the gate.

**A coarse-bound amplifier is the remaining piece, and it is bigger than the
second atlas** -- the amplifier is the world generator, not a cache. Options not
yet costed: a second amplifier instance bound to the coarse sampler, or a tier
argument threaded through `column()`.

**The stack dump in `ReportGateLeak_Locked` named this caller in ONE leg.**
Before it existed, the equivalent hunt took three builds of eliminating
subsystems by measurement. Reach for it first.

## Order of work

1. **Sky-pose per-ring cost for R7.** One matched leg pair. Everything downstream
   depends on it.
2. **The level -> tier function**, with its own test. One spelling, three
   consumers.
3. **The coarse raster atlas**, sized and logged like the fine one.
4. **R8 only**, measured at sky AND down before adding more.
5. Decide two-wide-rings vs three-doubling from (1) and (4).
6. Retire `AVoxelClipmapActor` LAST, once voxels demonstrably cover its range --
   it is the fallback while the cascade is incomplete.

**Do not delete the clipmap first.** It is what draws the world if any of the
above stalls.


---

## OUTCOME 2026-08-30/31 -- THE CASCADE SHIPPED; TWO DEFECTS REMAIN

**DONE. Voxels reach 65,536 m in 11 rings and the clipmap is RETIRED by default**
(`-VoxelClipmap` restores it; the actor, material and generator are all intact).
R10's outer edge equals the clipmap's own former outer half-extent, so the
ordering rule at the top of this plan -- retire it LAST, once voxels demonstrably
cover its range -- is satisfied rather than assumed.

**Measured, not estimated:**

    cascade      chunks    vs prev     p50     p95     p99
    4 km (R6)    52,786      --
    8 km (R7)    55,205     +4.6%     +0.05   +0.10   +0.70
    16 km (R8)   57,829     +4.7%
    65 km (R10)  59,723     +3.3%     flat    flat    unresolved

**16x the view distance for ~13% more resident chunks and no measurable p50/p95
cost.** The R8/R9/R10 ladder (six alternated legs, one binary) is flat to within
0.1 ms at p50 and p95; p99 is non-monotonic with a within-rung spread larger than
any between-rung difference, so it is UNRESOLVED, not zero.

**The ~0.9 ms/ring segment-entry figure does not describe an outer ring's
marginal cost.** Two inherited estimates were wrong by ~3x (streaming +14%
predicted vs +4.6% actual; 0.9 ms/ring vs +0.05 at p50). Measure, do not quote.

### FIVE consumers sized themselves off the cascade edge

Introducing a tier boundary means auditing everything that derives a DISTANCE
from `GetMaxRingLevel()`. Each was correct while one tier served every level:

1. streaming admission (`ComputeFootprintChunkZRange` -> the amplifier's tier)
2. on-demand raster fill
3. atlas prefetch coverage
4. the admit-centre reach
5. **the ocean plane** -- a hard-coded 40 km, +-20 km from the camera, which
   became a visible straight edge across open water the moment the clipmap
   stopped covering it. Now derived from `VoxelCoords::kNumLevels`.

Grep for constants in METRES or UU before assuming a ring-level sweep is done.

### STILL OPEN

* **A world edge remains over ocean**, but it is NOT the ocean plane (now 164 km).
  At (-53760,-61440) the coarse tile set ends ~69 km east, which is also about
  where the cascade ends. It is the edge of the world DATA. Not chased.
* **R9/R10 have no sky-pose timing leg.** The ladder was a line flight.
* **The demonstration is geography-limited**: the fine-baked block is coastal, so
  no heading from it looks over 60 km of land. The 3,067 m peak at
  (-56940,-56610) is the best inland viewpoint -- headings 0/30/60 are 100% land
  to 60 km per the coarse elevation data.
