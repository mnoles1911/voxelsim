# The P0 march spike: everything it measured, after the shader was deleted

**Status: RETIRED 2026-08-26.** `ue-project/Shaders/VoxelMarchSpike.usf` (1,456
lines), its four global shader classes (`FVoxelMarchSpikePS`,
`FVoxelMarchSpikeCountCS`, `FVoxelMarchSpikeMipL1CS`, `FVoxelMarchSpikeMipL2CS`),
its `AddMarchSpikePass` / `FVoxelMarchSpikeArm` / `FVoxelMarchSpikeCensus`
surface in `VoxelFluidRender.{h,cpp}`, its HUD readout in
`VoxelFluidSubsystem.cpp`, its five `voxel.Marcher.Spike*` cvars and its
compile harness `tools/voxel-check-march-shader.ps1` were all deleted together.

**Why it was deleted.** It was Phase 0 gate G1, superseded by the shipped
marcher. It was double-gated OFF: `voxel.Marcher.Spike` defaulted to 0 *and* it
hung off the fluid render extension, whose `voxel.Fluid.Enable` also defaults
to 0. It could not run in any shipping configuration, but it read as live code
and cost investigation time.

**Why this page exists.** The marcher still cites this shader by name in about
twenty-five comments across `VoxelMarchRenderer.{h,cpp}`,
`VoxelBrickTraverse.ush`, `VoxelMarch.usf` and `VoxelFluidCollision.ush` --
"the same volume VoxelMarchSpike.usf marched", "THE TIE RULE",
"THE REFERENCE NOISE FLOOR", "DEPTH SLACK", the no-fetch arm's argument.
**Those citations now resolve here.** Every note block from the deleted shader
is reproduced verbatim below, in file order, so that no finding was lost with
the code.

## The headline result the marcher still reasons from

Two mip levels is **not** straightforwardly better than one. Measured P0.5,
same pose, converged volume, budget 886, mean voxel advances per ray:

| arm | overall | hitMean | missMean | p95 |
|---|---|---|---|---|
| flat | 351.1 | 221.2 | 442.4 | 605 |
| skip=1 | 53.3 | 48.6 | 56.5 | 83 |
| skip=2 | 48.8 | **68.9** | **34.6** | 118 |

`skip=2` wins on MISSES (56.5 -> 34.6) and loses on HITS (48.6 -> 68.9), with a
materially worse p95 (83 -> 118). The mechanism is plain once seen: a ray that
terminates on a surface is by definition near occupied space, so every coarse
level it must descend through costs steps that advance it barely at all, while
a ray crossing open air collects the full benefit of the coarser stride. A
deeper fixed hierarchy is a straight trade of near-field cost for far-field
saving.

**The consequence for the real marcher:** the coarse level wants to be chosen
ADAPTIVELY -- coarse while the ray is far from anything occupied, fine once it
is close -- rather than by a fixed descent through every level on every ray.
That is a design finding, not a tuning note, and it came out of an experiment
aimed at something else entirely.

Corroboration to sanity-check against (the G2 brick census, independent of
this): 91-96% of the flat index measured skippable, 19.0 bricks per
brick-column at R0 falling to 2.0 at R5. A skip ratio wildly different from
that means one of the two instruments is wrong.

Caveat the shader stated and that still applies: **do not multiply the step
ratio by the flat arms' 5.9 us/step and call it a time.** Quote it as a step
ratio. A coarse step is not a fine step and the census charged them equally --
the coarse levels are 32 KB and 256 bytes against the fine volume's 16 MiB so
their loads are far cheaper, but coarse stepping used a reciprocal-multiply
cell exit rather than the fine walk's incremental compare-and-add, which is
more ALU per step. The two errors run in opposite directions and neither was
quantified.

---

## Also removed with it: `voxel.Marcher.SpikeVolume`

The spike's *substrate* cvar (`VoxelFluidSubsystem.cpp`) went too, since it had
no other consumer. What it did and why, for the record:

The occupancy volume holds TERRAIN bits, but it is allocated and placed by the
FLUID -- the origin latches on the first spawn, emit or armed faucet, and until
it does there is no volume at all. So a march spike that needed "terrain
occupancy around the viewer" could not get it without also creating water; the
first G1 leg ran its whole length against `occupancy=0/0` and measured nothing.

The obvious workaround was worse than it looked: spawning particles to force a
region into existence puts the SOLVER in the frame beside the march, turning
`simGpuMs` from a zero you can ignore into a confound you have to subtract.

`SpikeVolume 1` therefore latched and recentred the occupancy window on the
CAMERA with no water anywhere, and -- uniquely -- anchored **Z to the camera**
rather than to the ground. The fluid's own rule anchors Z to the ground (floor
~13 m under the surface) because water lives on terrain; a marcher wants the
camera at the centre of the box in all three axes or its 25.6 m of reach is
spent underground while the camera flies above it. The override was confined to
sessions where nothing had ever spawned, so it could never move the window out
from under live water.

**The related trap it was built to fix, which is still worth knowing:** the
first G1 leg printed `marchGpuMs=off` with `voxel.Marcher.Spike 886` applied.
The budget was published from *inside* the pass; the view extension declined
every frame for want of an occupancy volume, so the pass never ran and never
published -- and "could not run" was indistinguishable from "was not asked to".
The fix was to move the question off the path that failed (read the arm from
the cvar at the print site) rather than to rename the bucket. That is the
general rule: **a state that depends on a pass running can never be used to
describe whether it ran.**

# The full note record, verbatim from the deleted shader

```text
 VoxelMarchSpike.usf -- Phase 0 gate G1 of docs/ray-marching-plan-2026-08-19.md
 §3. This file exists to produce ONE NUMBER: the real per-pixel cost of
 marching real terrain, on this GPU, at this internal resolution. It is not a
 renderer, it is not a prototype of the renderer, and nothing in it is meant
 to survive into Phase 1.

 ---------------------------------------------------------------------------
 WHAT IS BEING MEASURED, AND WHAT IS DELIBERATELY NOT
 ---------------------------------------------------------------------------

 The plan's own statement of the limit, restated here because a number gets
 quoted long after the plan that produced it is closed: this walk is SINGLE
 LEVEL, DENSE, and takes NO palette fetch, NO ring transition, NO LOD and NO
 sparse indirection. It is a LOWER BOUND on the real marcher, not an estimate
 of it; the plan budgets the real thing at 1.4-1.6x whatever this reads.

 AND ONE LIMIT THE PLAN DOES NOT STATE, WHICH ANYONE READING THE PICTURE WILL
 HIT FIRST: the volume is 512 voxels = 51.2 m across, so THE SPIKE CAN SEE AT
 MOST ~25.6 m. This measures "what it costs to march a dense 512^3 volume",
 not "what it costs to march to the horizon" -- the traversal length is
 exactly what the plan's 886-step arm names and no more. A distant ridge is
 NOT in this volume and its absence from the overlay is correct.

 WHERE THE BOX SITS DEPENDS ON WHICH MODE PUT IT THERE, and the difference is
 worth 25 m of reach:
   voxel.Marcher.SpikeVolume 1 -- camera-centred in ALL THREE axes, so the
       eye is at the middle of the box and the reach is +/-25.6 m in every
       direction. THIS IS THE MODE THE SPIKE IS MEANT TO RUN IN.
   the fluid's own window (water present) -- XY on the camera, but Z anchored
       to the GROUND: floor ~13 m under the surface at the camera's column,
       ~38 m of air above. A camera flying at 80 m is then OUTSIDE the box
       entirely and the ray enters it from above; the clip below handles that
       correctly, but most of the budget is spent before the first voxel and
       the arms are not comparable with the centred ones.

 SO THE POSE MATTERS AS MUCH AS THE SITE. Centred on the eye, a camera 80 m up
 gets a box spanning 55-105 m of altitude with the ground entirely BELOW it,
 and every ray leaves the far side having touched nothing. That is not a
 broken pass -- an empty box is the LONGEST traversal it can produce, so the
 timing is real -- but it is the cost of marching AIR, and the gate is asking
 what it costs to march GROUND. Run the arms from a ground-level static pose,
 and confirm from the capture that the overlay actually covers terrain before
 quoting anything.

 One consequence for reading the timing: at a vista pose most pixels leave
 the box without ever hitting anything, which is the LONGEST traversal the
 box can produce. That is the expensive case and it is the right one to
 measure; it is not the pass failing to find terrain.

 The arms (voxel.Marcher.Spike is the budget, one value per arm):
   S1  budget 1      -- the pass overhead alone: full-screen quad, ray setup,
                        box clip, one DDA step. Everything that is NOT the
                        march.
   S4  budget 886    -- 512*sqrt(3), the full box diagonal: the walk can never
                        be the thing that stops the ray, so the geometry is.
   S5  budget 886 + voxel.Marcher.SpikeNoFetch 1 -- identical loop, ZERO
                        memory traffic.
   S4-S5 is the memory cost. S5-S1 is the ALU. Those two numbers, plus a
   looking-down arm against an at-the-horizon arm for divergence, decide the
   entire traversal design.

 ---------------------------------------------------------------------------
 WHERE THIS MUST BE RUN, AND WHY THE SITE IS PART OF THE MEASUREMENT
 ---------------------------------------------------------------------------

 ON A BAKED FINE TILE, AT (-61440, -61440) METRES. That is the four-tile
 junction of (-4,-4) (-4,-5) (-5,-4) (-5,-5) in the configured provider, it is
 covered on all quadrants, and it is the site every frame-time number in this
 gate was taken at -- so the march cost and the draw cost it is being weighed
 against describe the same ground.

 THE RULE THIS OBEYS. docs/measurements/ carries it from 2026-07-27: every
 perf number taken before that date came off the FLAT SYNTHETIC FALLBACK and
 had to be thrown out, because flat-fallback numbers do not transfer -- the
 GPU mesh fork's "38% cold-fill win" became ~5% on real tiles. A marcher is at
 least as sensitive: traversal length, hit distance and divergence are all
 properties of the terrain's shape, and flat ground has none of the shape this
 is meant to cost out.

 The engine already refuses rather than degrades here, which is why this file
 carries no gate of its own: an unattended run at an uncovered spawn is FATAL
 at VoxelFineTileStreamer.cpp:891, by design, precisely so a leg cannot
 quietly answer sea level and produce a number that looks real.

 ---------------------------------------------------------------------------
 THE MARCHER IS NOT WRITTEN HERE. IT IS ALREADY WRITTEN.
 ---------------------------------------------------------------------------

 VoxelFluidCollision.ush:265 `VoxelFluidWalkVoxelLine` is a budgeted
 Amanatides & Woo walk over the same 512^3 toroidal bit volume the fluid
 collides against; it mirrors voxelcore/raycast.h line for line and
 tests/test_fluidoccupancy.cpp cross-checks the two on the same geometry. A
 second DDA written for this spike would be a second thing to be wrong, and
 its being wrong would look exactly like the terrain being cheap or expensive.
 So this file is a RAY SETUP and a SHADE, and the loop between them belongs to
 somebody else.

 Two hooks in that file make the arms possible, both guarded and both default
 to today's behaviour (their reasoning lives next to them, not here):
   VOXEL_FLUID_MAX_COLLISION_STEPS -> a uniform, so the budget is a cvar
   VOXEL_FLUID_SOLID_NO_FETCH      -> a permutation, so no-fetch really is
                                      no fetch and not a predicated load

 ---------------------------------------------------------------------------
 THE FACE NORMAL IS FREE, AND THAT IS THE POINT (ADR-0008)
 ---------------------------------------------------------------------------

 The walk returns FaceAxis (0/1/2) and FaceSign (+1 == entered moving in
 +axis). That IS the exact surface normal of the voxel face the ray entered
 through -- no derivative, no interpolation, no vertex attribute, no
 smoothing, and no cost beyond the two integers the walk already had to know
 in order to step. ADR-0008 (flat per-voxel material colour, accepted) is
 built on precisely this, and the shading below is deliberately the crudest
 thing that shows it: flat, per-face, no filtering. Anything prettier would
 add cost that is not the march and would corrupt the number this file
 exists for.

 ---------------------------------------------------------------------------
 IT IS AN ADDITION, NEVER A SUBSTITUTION
 ---------------------------------------------------------------------------

 The terrain raster path is untouched and still draws every frame. This pass
 composites OVER it, translucently, and discards nothing that the raster
 path drew. That is what makes the A/B honest: with voxel.Marcher.Spike 0 the
 frame is byte-for-byte the shipped frame, and the delta at any other value is
 the march plus one full-screen pass and nothing else. A spike that REPLACED
 the terrain draw would measure (march - raster) and would answer a question
 nobody asked at this gate.

 The translucency is not decoration either: at 0.85 the rastered ground shows
 faintly through the marched surface, so a single screenshot says whether the
 marcher and the mesher agree about where the ground is. A registration error
 of one voxel is visible; an opaque overlay would hide it completely.
```

---

```text
 ---------------------------------------------------------------------------
 The budget, declared BEFORE the walk is included
 ---------------------------------------------------------------------------

 The DDA step cap IS the measurement's independent variable, so it has to be a
 uniform: a permutation per budget cannot express an arbitrary cvar value, and
 re-launching the editor per arm is exactly the cost this gate was scoped to
 avoid. Host-side clamped to [1, 4096]; the walk's loop is [loop] already, so
 a uniform bound costs it nothing.
```

---

```text
 VOXEL_FLUID_SOLID_NO_FETCH is set by the shader permutation
 (FVoxelMarchSpikePS::FNoFetchDim) and needs no default here -- the collision
 header defaults it to 0 for every other consumer.
```

---

```text
 ---------------------------------------------------------------------------
 Bindings
 ---------------------------------------------------------------------------

 A parameter STRUCT, filled by SetShaderParameters, exactly like
 VoxelFluidRender.usf's passes -- not loose per-element FShaderParameters.
 docs/gpu-g2-draw-path.md records that the loose form measurably does not bind
 in this project and that ShaderBindings.Add on an unbound parameter is a
 silent no-op, which here would read as "the march is free".

 FluidOccupancyBits / FluidVolumeOriginVoxel / FluidVolumeDimVoxels /
 FluidVolumeWrapOffsetVoxel are NOT declared here: VoxelFluidCollision.ush
 declares them and it is the only file allowed to. The host pastes
 VOXEL_FLUID_OCCUPANCY_PARAMETERS() into the same struct so the names match.
```

---

```text
 Rotation of a view-space direction into world space. mul(float4(v, 0), M) --
 UE's row-vector convention, same as VoxelFluidRender.usf's splat. w = 0 so
 the translation row is ignored, which is why the TRANSLATED inverse view
 matrix is safe to narrow to float even at planet scale.
```

---

```text
 The camera, in the volume's own local UU frame (world UU minus the volume's
 min corner, i.e. the frame VoxelFluidCollision.ush calls LOCAL and the frame
 particle positions already live in). Differenced in DOUBLE on the host and
 narrowed after: both operands are tens of km, the difference is bounded by
 the volume's 5,120 UU, and doing that subtraction in float is how a marcher
 ends up hitting terrain a few metres from where it is drawn.
```

---

```text
 ---- the census (voxel.Marcher.SpikeCount, the counting pass only) ---------

 WHY A SECOND PASS AND NOT A UAV ON THE PIXEL SHADER. Two reasons, and the
 second is the one that decided it. First, the coordinator's own rule: the
 counters must not be able to move marchGpuMs, and the cleanest guarantee of
 that is that the timed pass does not contain them at all -- the counting
 dispatch is added AFTER the timing bracket closes. Second, and the reason
 this is not merely tidier: a UAV bound to a pixel shader inside an RDG raster
 pass is the one part of this design I cannot test without an editor, and a
 wrong guess there fails the whole counting arm. A compute dispatch with a
 structured-buffer UAV is unambiguous on every path this project already uses.

 The cost of that choice, stated: the counting run marches every ray TWICE
 (once for the picture, once for the census). That is why counting is its own
 cvar and its own run, and why no timing from a counting run should be quoted.

 The two marches are the SAME rays, not merely similar ones: both go through
 VoxelMarchSpikeTraceRay below, and the compute pass reconstructs exactly the
 SV_Position the rasteriser would have produced for that pixel.
```

---

```text
 Bins per outcome = the step budget + 1. Steps are provably <= the budget, so
 this histogram is EXACT and the mean and p95 read off it are exact too --
 no bucketing, no interpolation, nothing derived. The buffer is
 VOXEL_MARCH_OUTCOME_COUNT * MarchHistBins uints.
```

---

```text
 Dispatch extent = the view rect. Threads outside it write nothing, so the
 census total equals the pixel count exactly (see the conservation note on
 the outcome codes).
```

---

```text
 ---------------------------------------------------------------------------
 Constants
 ---------------------------------------------------------------------------
```

---

```text
 How far inside the box the clipped entry/exit points are pulled. The walk's
 first act is floor(pos / 10) + origin, and a point sitting exactly on the box
 face floors to voxel -1 or 512 -- which VoxelFluidSolidAtVoxel reports as
 SOLID (outside the volume is solid, by contract). The whole ray would then
 terminate on the box wall as "started inside", the screen would fill with one
 flat colour, and the march would measure nothing. 0.05 UU is ~100 float ULP
 at the box's far corner and 1/200 of a voxel.
```

---

```text
 Direction components below this are nudged away from zero before the
 reciprocal, so an axis-aligned ray produces +/-inf slab distances rather than
 0 * inf = NaN. The nudge can flip the sign of a component under 1e-6, which
 changes nothing: at that magnitude both signs put the slab crossings past the
 other two axes' and the min/max never selects them.
```

---

```text
 Depth-test slack, in UU, when deciding whether the marched surface is in
 front of what the raster path already drew.

 IT IS NOT AN EPSILON, IT IS THE WHOLE REASON THE OVERLAY IS LEGIBLE. The
 occupancy bits and the greedy-meshed quads are built from the SAME voxel
 lattice, so a correct march lands on the same face the raster drew, at the
 same depth, to within float error -- and a strict `<` then accepts roughly
 half the pixels and rejects the other half at random. The overlay would come
 out as noise and would read as a broken marcher rather than as two things
 agreeing. Two voxels of slack keeps a genuine occluder (a tree, an actor,
 the far side of a hill) rejecting cleanly while an exact agreement passes.
```

---

```text
 ---------------------------------------------------------------------------
 Helpers
 ---------------------------------------------------------------------------
```

---

```text
 Common.ush's ConvertFromDeviceZ, restated for the same reason
 VoxelFluidRender.usf restates it: this file includes only Platform.ush,
 because Common.ush drags in the view uniform buffer that this pass
 deliberately does not bind (every view quantity it needs is passed
 explicitly, which is also what keeps it offline-dxc-compilable).
```

---

```text
 ===========================================================================
 The empty-space skip pyramid (voxel.Marcher.SpikeSkip)
 ===========================================================================

 WHAT THIS EXPERIMENT IS FOR. The four flat arms reduced the whole project to
 one unknown: how many steps does a pyramid-accelerated march take? The cost
 model from those arms is march_ms ~= 0.13 + steps * 5.9 us at 1552x873, so
 200 steps is comfortable, 1,000 is marginal and 2,000 is dead -- and the flat
 spike needs 886 steps to cross 51.2 m with no skipping at all. "A pyramid
 will recover that" is exactly the kind of claim this project has falsified
 7 times out of 7, so it gets measured instead of argued.

 THE DELIVERABLE IS THE RATIO steps(skip=0) / steps(skip=2), on identical
 poses, NOT the time. The 51.2 m box is far too small for its timing to
 extrapolate; the step ratio is the multiplier the 4 km estimate needs.

 THREE CAVEATS, none of them small:

  1. THIS RATIO IS A LOWER BOUND on what a real cascade would achieve. The
     volume is single-level and DENSE at 10 cm throughout; a real cascade
     coarsens with distance, so its far field would skip in much bigger
     strides than anything measurable here. Six mip levels over a coarsening
     cascade beats two levels over a uniform 51.2 m box, and by an unknown
     margin.
  2. A COARSE STEP IS NOT A FINE STEP, and the census charges them equally.
     The coarse levels are 32 KB and 256 bytes against the fine volume's
     16 MiB, so they sit in cache and their loads are far cheaper -- but the
     coarse stepping below uses a reciprocal-multiply cell exit rather than
     the fine walk's incremental compare-and-add, which is more ALU per step.
     The two errors run in opposite directions and neither is quantified
     here. DO NOT multiply the step ratio by the flat arms' 5.9 us/step and
     call it a time; quote the ratio as a step ratio.
  3. THE SKIP WALK IS NEW CODE and the flat walk is unit-tested against
     voxelcore/raycast.h. So the census runs BOTH on the same ray whenever
     voxel.Marcher.SpikeSkipVerify is on and counts the disagreements. A
     non-zero mismatch count invalidates the ratio, loudly, on the same log
     line that reports it.

 ---------------------------------------------------------------------------
 A RESULT THIS EXPERIMENT PRODUCED THAT IT WAS NOT LOOKING FOR
 ---------------------------------------------------------------------------

 TWO MIP LEVELS IS NOT STRAIGHTFORWARDLY BETTER THAN ONE. Measured P0.5, same
 pose, converged volume, budget 886, mean voxel advances per ray:

     arm       overall   hitMean   missMean   p95
     flat        351.1     221.2      442.4   605
     skip=1       53.3      48.6       56.5    83
     skip=2       48.8      68.9       34.6   118

 skip=2 wins on MISSES (56.5 -> 34.6) and LOSES on HITS (48.6 -> 68.9), with a
 materially worse p95 (83 -> 118). The mechanism is plain once seen: a ray
 that terminates on a surface is by definition near occupied space, so every
 coarse level it must descend through costs steps that advance it barely at
 all, while a ray crossing open air collects the full benefit of the coarser
 stride. A deeper fixed hierarchy is a straight trade of near-field cost for
 far-field saving.

 THE CONSEQUENCE FOR THE REAL MARCHER: the coarse level wants to be chosen
 ADAPTIVELY -- coarse while the ray is far from anything occupied, fine once
 it is close -- rather than by a fixed descent through every level on every
 ray. That is a design finding, not a tuning note, and it is recorded here
 because it came out of an experiment aimed at something else entirely and
 would otherwise be lost with the arm that produced it.

 The corroboration to sanity-check against (the G2 brick census, independent
 of this): 91-96% of the flat index measured skippable, 19.0 bricks per
 brick-column at R0 falling to 2.0 at R5. A skip ratio wildly different from
 that means one of the two instruments is wrong.

 ---------------------------------------------------------------------------
 THE PYRAMID'S LAYOUT, AND WHY IT IS BUILT IN STORAGE SPACE
 ---------------------------------------------------------------------------

 L1: one bit per 8^3 block of voxels -> 64^3 bits = 8,192 words = 32 KB.
 L2: one bit per 8^3 block of L1 cells (64^3 voxels) -> 8^3 bits, one word
     per (y2,z2) row = 64 words = 256 bytes. Deliberately wasteful of bits
     and trivially indexable; at 256 bytes the waste is not worth a smarter
     packing.

 BOTH ARE BUILT OVER THE RAW BUFFER, i.e. in STORAGE coordinates, and the
 reader applies exactly the same local->storage wrap the fine reader applies
 before shifting down. That is only sound because the wrap offset is always a
 multiple of the 64-voxel recentre step (vxc::kFluidRecentreStepVoxels), so a
 toroidal seam can never fall inside an 8-voxel or 64-voxel block -- the mip
 cells slide with the window instead of being renamed by it, exactly as the
 fine bits do. If that quantum ever stops dividing the block size, this
 reduction silently starts describing blocks that straddle the seam.
```

---

```text
 0 = flat DDA (the control), 1 = L1 skipping, 2 = L2 then L1.

 A PERMUTATION, NOT A UNIFORM, and this was measured rather than chosen. With
 the skip level as a runtime `if`, the mip loads stay in the binary on the
 untaken path -- and the no-fetch arm, whose entire claim is "the identical
 loop with ZERO memory traffic", went from 0 to 3 dx.op.bufferLoad the moment
 the skip code landed. The offline disassembly caught it; nothing at runtime
 would have. The no-fetch number the plan already quotes (2.88 ms) came from a
 binary with no loads in it at all, and a uniform branch would have silently
 re-based it.
```

---

```text
 Cross-check the skip walk against the flat one, ray for ray, in the census
 pass only. See the diagnostics row on the histogram for what it records.
 A runtime uniform is fine here: it only ever appears in the census kernel,
 which is never the timed pass.
```

---

```text
 One thread per OUTPUT WORD of L1. Each thread owns 32 L1 cells along x =
 256 voxels = 8 input words per row, over an 8x8 block of rows -- so it reads
 512 words and writes one, and the pass as a whole touches the 16 MiB volume
 exactly once. A thread-per-cell mapping would have read every input word
 four times over.
```

---

```text
 LOCAL cell -> is anything solid in it. Mirrors VoxelFluidSolidAtVoxel's two
 rules exactly, at cell granularity: outside the volume reads as SOLID (so a
 skip can never carry a ray out through an unbuilt edge), and the local->
 storage wrap is applied before indexing.
```

---

```text
 The wrap is applied at VOXEL granularity and then shifted down, so this
 asks the same question the fine reader would ask of any voxel in the
 cell -- see the storage-space note above for why that is well defined.
```

---

```text
 Voxel advances a completed fine walk made over [SubEntry, SubExit]. Shared by
 the flat control and every skip level, so the two arms of the ratio count
 the same thing. See the flat path's own note for why each case is exact.
```

---

```text
 ---- the skip walk ---------------------------------------------------------

 Compiled out entirely in the control permutation, so the mip buffers are
 unreferenced there and DXC strips them -- which is what keeps the no-fetch
 arm's binary free of memory traffic (see VOXEL_MARCH_SKIP_LEVELS above).
```

---

```text
 ---------------------------------------------------------------------------
 WHY THIS IS THE SECOND VERSION, AND WHAT THE FIRST ONE GOT WRONG
 ---------------------------------------------------------------------------

 v1 advanced by a scalar epsilon in segment parameter and re-floored the cell
 index from the nudged point. P0.5 measured 52 rays out of 1,354,896
 disagreeing with the flat walk -- 0.0038%, and every one of them in the
 direction that makes the result look BETTER.

 THE CAUSE. Amanatides & Woo crosses exactly ONE plane per iteration, and
 raycast.h fixes the tie rule for simultaneous crossings: step one axis at a
 time, lowest axis first. A scalar t-nudge does not respect that. Where a ray
 passes within the nudge of a cell EDGE or CORNER, the nudge crosses two or
 three planes at once and lands in the diagonal neighbour -- silently omitting
 the intermediate cell that the flat walk visits. If the omitted cell held the
 surface, the skip walk misses it and marches on, which costs FEWER steps and
 still draws terrain. That is precisely the failure mode this spike was warned
 about, and it is why the verifier exists.

 It is NOT a defensible tie. A tie would be a ray touching an edge with zero
 measure; these rays pass through the intermediate cell for up to a nudge's
 width of real distance, and the flat walk is right about them.

 THE FIX, and why it needs no new DDA. Two changes:

   1. The coarse traversal is now a true Amanatides & Woo -- tMax/tDelta/step
      maintained incrementally, one plane per iteration, ties lowest-axis
      first. It is a line-for-line mirror of VoxelFluidWalkVoxelLine's own
      loop with the cell size substituted, so its tie behaviour matches the
      fine walk's BY CONSTRUCTION rather than by epsilon tuning. There are no
      epsilons left in the cell stepping at all.

   2. The fine walk is no longer seeded exactly on a cell boundary. Seeding it
      there is the other half of the same bug: floor() on the crossed axis is
      ambiguous at a plane, so a ray travelling in -x would be seeded in the
      wrong voxel. Instead the sub-segment BACKS OFF half a voxel into the
      PREVIOUS coarse cell -- which the mip has just told us is empty -- so
      the tested walk starts at a generic interior point and crosses the
      boundary itself, using its own tested tie rule. Nothing reconstructs a
      boundary crossing by hand any more.

 The backoff costs one or two extra fine steps per occupied cell, so it
 inflates the step count slightly. That is the conservative direction: it
 makes the measured skip ratio a little WORSE than the truth, never better.

 It also deletes v1's face-normal reconstruction. With the backoff, the fine
 walk crosses every boundary itself and reports a real face, so StartedInside
 can now only mean what it means in the control arm: the ray began inside
 solid.
```

---

```text
 Backoff distances, in UU along the ray, for seeding a walk inside the
 previous (known-empty) cell. Half of the cell the walk is about to step in,
 which is always strictly inside the previous cell and never far enough to
 reach the one before it.
```

---

```text
 Hang guards. Worst case is a ray crossing every cell along all three axes:
 64 per axis at L1, 8 at L2. These are guards, not budgets -- the step budget
 is what terminates a normal walk, and a guard firing is reported as budget
 exhaustion rather than as a clean miss.
```

---

```text
 Amanatides & Woo state over a grid of arbitrary cell size. A plain array for
 the per-axis members, not an int3/float3: the walk indexes them by the axis
 it chose each step, and dynamic component indexing of a VECTOR forces DXC to
 spill the whole vector to scratch on every write (the same note
 VoxelFluidCollision.ush carries on its own arrays).
```

---

```text
 Mirror of VoxelFluidWalkVoxelLine's setup block, with CellUU in place of the
 voxel size. Seeded from a point that is INSIDE the first cell, never on its
 boundary -- every caller guarantees that.
```

---

```text
 Seeded from a point strictly inside the first cell. The caller guarantees
 TFrom is either the segment start (an insetted box entry, generic) or a
 backed-off point inside a known-empty cell -- never a boundary.
```

---

```text
 BACK OFF INTO THE PREVIOUS CELL before handing the span to the
 tested walk, so it crosses this cell's entry face itself rather
 than being seeded on it. Clamped to TFrom so the first cell of a
 span never reaches behind its own start.
```

---

```text
 Same backoff argument one level up: start the L1 walk inside the
 previous L2 cell, which the mip has just said is empty, so its
 own A&W crosses this cell's face.
```

---

```text
 ---------------------------------------------------------------------------
 The trace, shared by the picture and the census
 ---------------------------------------------------------------------------
```

---

```text
 EVERY RAY ENDS IN EXACTLY ONE OF THESE, and the census counts all five.

 THE CONSERVATION RULE, which is the instrument's own ran-flag: the five
 outcome totals MUST sum to the dispatched pixel count. They cannot disagree
 unless the histogram is being written wrong, so the census line prints the
 sum and the pixel count together rather than trusting either. This project
 has been burned three times by counters that could not prove themselves.

 EXHAUSTED IS NOT A MISS, which is why there are five and not three. A ray
 that ran out of budget with segment left is the population a brick pyramid
 or a mip chain would rescue; folding it into "missed" hides exactly the
 thing the next design decision turns on.
```

---

```text
 ONE EXTRA HISTOGRAM ROW, above the five outcomes, that is NOT an outcome and
 is deliberately excluded from the conservation sum: it holds the skip-vs-flat
 verification tallies rather than a distribution over steps.
   bin 0  rays where the skip walk and the flat walk DISAGREED
   bin 1  rays where the two were comparable at all (see the census kernel)
 A non-zero bin 0 invalidates the step ratio the skip arms exist to produce,
 which is why it rides the same buffer and prints on the same line instead of
 living somewhere it could be forgotten.
```

---

```text
 AND A SAMPLE TAIL, past the histogram rows entirely: a claim counter plus a
 handful of fixed-width records describing individual disagreeing rays. A
 count with no handle to debug from is a count nobody can act on -- the first
 mismatch report was exactly that, and finding the cause needed a hypothesis
 rather than a coordinate.

 Layout, at MarchHistBins * VOXEL_MARCH_HIST_ROWS:
   [0]                    rays claimed (may exceed the records kept)
   [1 + i*10 .. +9]       record i: pixel x, pixel y, flags,
                          flat voxel xyz, skip voxel xyz, skip steps
 flags bit 0 = the flat walk found a surface, bit 1 = the skip walk did.
```

---

```text
 Header words ahead of the records: [0] rays that claimed a record slot,
 [1] rays excused as a shared-face tie (see THE TIE RULE below).

 The tie counter lives HERE and not in a histogram bin on purpose: the diag
 row is only MarchHistBins wide, and the budget-1 arm has Bins == 2, so bin 2
 does not exist there. A counter that silently vanishes on one arm is exactly
 the class of defect this whole instrument was built to refuse.
```

---

```text
 ---------------------------------------------------------------------------
 THE TIE RULE, decided 2026-08-19 on the P0.6 evidence
 ---------------------------------------------------------------------------

 After the diagonal-skip fix, every remaining skip-vs-flat disagreement was an
 ADJACENT voxel differing on EXACTLY ONE axis, both solid, at the same
 distance along the ray. 53 rays in 1,354,896. The question this answers is
 which of the two is "the" hit.

 NEITHER, AND raycast.h DOES NOT ARBITRATE IT. Its tie rule
 (raycast.h:74-80) governs which AXIS to step when two tMax values are equal
 -- the ORDER of traversal. It says nothing about which voxel a point lying
 exactly ON a shared face belongs to. That is a different question and it has
 no canonical answer: the geometric predicate "which side of this plane is the
 ray on" is undefined for a ray that is on the plane.

 AND THE SKIP WALK CANNOT BE PICKING A NON-CANONICAL ANSWER, which is the
 decisive argument. Both arms call the SAME VoxelFluidWalkVoxelLine. The skip
 walk does not implement a rule of its own that could disagree with
 raycast.h's; it applies the identical rule from a start point half a voxel
 further back. What differs is float rounding in `floor(pos / 10)` for a
 coordinate that sits exactly on a boundary -- the two walks reach that
 coordinate by different arithmetic and land on opposite sides of an exact
 integer. Both then report the same surface, at the same distance, through a
 face that is INTERNAL to the pair and therefore invisible.

 So it is excused, and excused NARROWLY -- all three of:
   * the two hit voxels differ by exactly 1 on exactly one axis (L1 == 1)
   * both voxels are solid
   * the two hits are within VOXEL_MARCH_TIE_MAX_SEPARATION_UU along the ray
 and counted in their own column rather than folded into agreement, so the
 class stays visible and a change in its RATE is still a signal.

 WHAT THIS DELIBERATELY DOES NOT DO IS LOOSEN A THRESHOLD. A real
 skipped-voxel defect puts the two hits a whole voxel apart ALONG THE RAY --
 10 UU or more. The separation bound below is 0.5 UU, twenty times smaller
 than the smallest real error and four orders of magnitude above the float
 noise it exists to absorb. Widening it toward a voxel would start excusing
 genuine bugs, which is the one way this instrument could stop earning its
 keep.
```

---

```text
 ---------------------------------------------------------------------------
 THE REFERENCE NOISE FLOOR, and why the verifier measures it every frame
 ---------------------------------------------------------------------------

 After the tie exemption, P0.6 left 20 disagreements in 1,354,896 that were
 NOT ties: two axes apart, both solid, hits ~1.6 voxels apart along the ray.
 Traced offline in a float32 mirror of both walks, every one has the same
 shape:

     [flat] axis=0 v=(50,49,32) t=0.236133948
     [flat] axis=2 v=(50,49,31) t=0.236133978     <- 3e-8 apart, ~2 float ULP

 The ray crosses an x plane and a z plane SIMULTANEOUSLY to within two float
 ULP. The flat walk reaches that point with a tMax accumulated over thirty
 `tMax += tDelta` steps; the sub-walk reaches it with a tMax computed fresh
 from its own start. The two orderings differ, the two walks enter different
 voxels, and both of those voxels happen to be solid. Neither is wrong: at
 two ULP the geometry does not say which plane is crossed first.

 THIS IS NOT A PROPERTY OF SKIPPING. Measured on 8,000,000 rays in the
 offline mirror:

     flat vs the SAME flat walk, entry nudged 1e-6 UU along the same ray
                                                   157 / 8M   = 2.0e-5
     flat vs the skip walk                          12 / 8M   = 1.5e-6

 The reference disagrees with a negligibly perturbed COPY OF ITSELF thirteen
 times more often than it disagrees with the skip walk. Any two DDA instances
 with different arithmetic histories diverge at this rate; a restart-based
 skip walk necessarily has a different history, and no amount of care in the
 skip logic can go below the floor the comparison itself sits on.

 SO THE VERIFIER MEASURES THE FLOOR, on the same rays, in the same frame,
 rather than having a number written into it. The census runs the flat walk a
 SECOND time over a segment scaled by 1 - 1e-7 -- geometrically the same ray,
 one part in ten million of arithmetic difference, no new DDA -- and counts
 how often the reference disagrees with itself. `mismatch` is then read
 against `refNoise` instead of against zero.

 THIS IS CALIBRATION, NOT A LOOSENED THRESHOLD. The floor is measured from
 the same population every frame; a real defect pushes mismatch WELL above it
 (the pre-fix diagonal bug ran at 52 against a floor of this order), and
 nothing about the control can be tuned to hide one. Its one contamination:
 the scaled segment is shorter by ~1e-3 UU at the far end, so a reference hit
 inside that last sliver flips to a miss. That inflates the floor slightly,
 which is the conservative direction -- it can only make the verifier stricter
 about what counts as noise, never more permissive.
```

---

```text
 VOXEL ADVANCES the walk actually made -- not loop iterations. On a clean
 miss the loop runs one extra non-advancing iteration to notice the
 segment is spent; that one is deliberately not counted, because "how far
 did this ray get" is the question the cost model is asking.
```

---

```text
 ONE ray, from a pixel's SV_Position. The pixel shader is handed this by the
 rasteriser; the census pass reconstructs it as ViewRectMin + thread + 0.5,
 which is exactly what the rasteriser produces for that pixel -- so the two
 passes trace the same population and not merely a similar one.
```

---

```text
 UE view space: X right, Y up, Z forward. z = 1 makes t along this
 UNNORMALISED direction equal to view-space Z, which is the conversion
 used at the depth test in the pixel shader.
```

---

```text
 ---- clip to the 512^3 box --------------------------------------------

 THE WALK HAS NO WORLD BOUND OF ITS OWN in the no-fetch arm: with the
 solidity stub returning false, "outside the volume is solid" stops
 stopping anything, and an unclipped ray would walk until the budget ran
 out at every pixel regardless of geometry. Clipping here is what keeps
 the two arms running the SAME loop over the SAME segment -- it is part of
 the measurement, not a tidiness.

 float3(bool3) rather than letting the comparison promote itself: the
 implicit form compiles but the explicit one is what DXC's HLSL-2021 rules
 are guaranteed to accept without a truncation diagnostic.
```

---

```text
 Under voxel.Marcher.SpikeVolume the camera is at the box centre, so this
 rejects nothing; it earns its place in the other cases -- the frames
 after a teleport and before the first recentre, and a ground-anchored
 window the camera has flown above (see WHERE THE BOX SITS, at the top).
```

---

```text
 ---- THE MARCH ---------------------------------------------------------

 The control arm and the skip arms differ ONLY here. Everything above --
 the ray, the box clip, the inset -- and everything below -- the outcome
 classification, the step accounting, the shading -- is shared, so a
 steps(skip=0)/steps(skip=2) ratio cannot be contaminated by the two arms
 setting up different rays.
```

---

```text
 ---- outcome -----------------------------------------------------------

 STEPS ARE DERIVED FROM THE WALK'S OWN OUTPUT, not counted inside it (see
 VoxelMarchSpikeWalkSteps), and that is deliberate: it keeps
 VoxelFluidCollision.ush -- which mirrors voxelcore/raycast.h line for
 line and is unit-tested against it -- free of a field that exists only
 for a measurement.
```

---

```text
 ---------------------------------------------------------------------------
 1. the picture (and the TIMED pass)
 ---------------------------------------------------------------------------

 Vertex shader: FluidRenderScreenVS, reused verbatim out of
 VoxelFluidRender.usf (no vertex factory, no vertex buffer, one full-screen
 triangle from SV_VertexID). The host points a second global-shader class at
 that same entry point; there is no second copy of it.
```

---

```text
 ---- THE ANCHOR THAT KEEPS THE MARCH ALIVE -----------------------------

 READ THIS BEFORE SIMPLIFYING THE SHADE BELOW. In the no-fetch arm
 T.Hit.Hit is provably false at COMPILE time (the solidity stub is a
 permutation, not a branch). Every value guarded by `if (Hit)` is
 therefore dead code, and if the pixel's output depended ONLY on such
 values -- the natural shape, `if (!Hit) discard;` -- DXC would eliminate
 the entire walk and the arm would report the cost of an empty full-screen
 pass. That is not a hypothetical: the measurement would look like a
 spectacular result rather than a bug.

 So the shader's output ALWAYS carries values the loop wrote, on every
 path. THE LOAD-BEARING TERMS ARE TEnter AND BudgetExhausted: those two
 are written by the loop on the paths that survive in BOTH arms, so
 neither arm can fold the walk away. Voxel and Steps are folded in for
 diagnosis and because they are loop-written in the fetch arm; do not rely
 on them alone.

 And there is NO `discard` anywhere in this shader for the same reason.
 Occluded and missing pixels leave with alpha 0; the blend unit drops
 them, the loop still ran, and the cost is still paid and still measured.

 CHECKED, NOT ASSUMED. Both permutations were compiled offline with the
 project's own dxc (tools/dxc, -T ps_6_0 -HV 2021) and disassembled: the
 no-fetch binary contains ZERO dx.op.bufferLoad and still carries the
 walk's loop with its trip bound read from the MarchStepBudget cbuffer
 slot, while the fetch binary carries the same loop plus the three
 occupancy loads. That is the arm doing what it claims. If this shade is
 ever restructured, re-run that check -- it is the only thing standing
 between this file and a very convincing wrong answer.
```

---

```text
 A fixed key direction, not the ephemeris sun: this pass must not acquire
 a second binding, a second cross-thread setting, or a reason to differ
 between arms. Ambient 0.25 so the faces pointing away are still legible.
```

---

```text
 Per-axis tint so the face the walk reported is readable straight off the
 screenshot -- x, y and z faces are three visibly different colours, which
 is the cheapest possible check that the normal is real and not a constant.
```

---

```text
 The camera is inside solid rock (or inside a region the volume has
 not built yet, which reads as solid by contract). No face was crossed
 so there is no normal to shade; reported in its own colour rather
 than shaded as if it were a surface.
```

---

```text
 THE BUDGET WAS THE BINDING CONSTRAINT ON THIS PIXEL. Magenta, and
 loud, because a budget too small for the site produces a picture that
 is otherwise indistinguishable from "there is no terrain here" -- and
 the arm would then be timing a march that stopped early everywhere
 while reading as a legitimate result. The census counts this same
 population exactly; the colour is how you notice you need to look.
```

---

```text
 ---------------------------------------------------------------------------
 2. the census (voxel.Marcher.SpikeCount 1)
 ---------------------------------------------------------------------------

 One thread per pixel of the view rect, one atomic per ray, into an EXACT
 histogram: MarchHistBins bins per outcome, bin index = voxel advances. The
 CPU reads the whole thing back and computes counts, mean and p95 from the
 distribution itself. Nothing is bucketed, interpolated, or inferred from
 totals -- which is the whole reason this pass exists rather than four timing
 points with a survival curve fitted to them.

 ONE atomic per ray, not two: an outcome's ray count is the SUM of that
 outcome's bins, so counting it separately would be a second way to say the
 same thing and a second thing to be inconsistent with.
```

---

```text
 The SV_Position the rasteriser would hand the pixel shader for this
 pixel: rect origin + integer pixel + the half-pixel centre offset. Same
 ray, not a similar one.
```

---

```text
 Steps are provably in [0, MarchStepBudget] and the bin count is the
 budget + 1, so the clamp never fires. It is here because a histogram that
 can be written out of bounds corrupts a neighbouring outcome's bin and
 then reports it as a perfectly plausible number.
```

---

```text
 ---- the skip walk proving itself -------------------------------------

 THE SKIP WALK IS THE ONLY PART OF THIS SPIKE THAT IS NOT BACKED BY A
 UNIT TEST. VoxelFluidWalkVoxelLine mirrors voxelcore/raycast.h and
 tests/test_fluidoccupancy.cpp cross-checks the two on real geometry; the
 pyramid traversal above has nothing of the kind, and it is the thing
 whose output -- a step ratio -- the whole 4 km extrapolation would rest
 on. A hierarchical walk that skips one cell too many produces a LOWER
 step count and a picture that still looks broadly like terrain, which is
 the most dangerous possible failure here: it would read as a better
 result rather than as a bug.

 So the census runs both on the same ray and counts disagreements. This
 runs in the census pass only, never in the timed one.
```

---

```text
 NOT COMPARABLE when either walk ran out of budget: skipping is
 supposed to reach further on the same budget, so the two legitimately
 disagree there and counting that as a mismatch would bury the real
 ones. Excluded from the denominator too, so the reported rate is a
 rate over rays that could actually be compared.
```

---

```text
 Agreement means: both found a surface or neither did, and when
 both did, it is the SAME voxel. The face is deliberately not
 compared -- the skip walk recovers it from the cell boundary it
 crossed, which is the same face by construction but is computed
 a different way, and a float-level disagreement there would be a
 separate finding rather than a wrong surface.
```

---

```text
 THE SHARED-FACE TIE (see THE TIE RULE at the top of this file).
 All three conditions, never any two: adjacent on exactly one
 axis, both solid, and the same point on the ray to within a
 twentieth of a voxel. A real skipped-voxel defect separates the
 two hits by a whole voxel ALONG THE RAY and fails the third test
 by a factor of twenty, so this excuses the degenerate case
 without widening the net that would catch a bug.
```

---

```text
 THE RESTART-ONLY CONTROL -- the direct one, and the one the
 verdict uses.

 The SAME skip walk, over the SAME ray, with the mip forced solid:
 identical code path, identical restart structure, and provably
 ZERO skipping, because no cell is ever reported empty. Any
 disagreement it has with the flat walk is therefore restart-
 induced by construction -- it cannot be a skipped cell, because
 nothing was skipped.

 This replaces the segment-scale probe as the discriminator
 because that probe measures a DIFFERENT quantity: how sensitive
 the reference is to its own rounding, which is not the same as
 how much a restart-based walk must diverge from it. The offline
 model made that gap visible -- it put the segment-scale probe
 ABOVE the skip walk's disagreement rate on both synthetic worlds,
 while the engine measured it three times BELOW. A control that
 disagrees with the thing it is controlling for is not a control.

 ITS DIRECTION IS PERMISSIVE, AND THAT IS STATED ON THE LINE: it
 restarts at EVERY cell, whereas the skip walk restarts only at
 occupied ones, so it sees more divergence opportunities and is an
 UPPER bound on restart noise. mismatch ABOVE it is therefore a
 hard result -- a real defect. mismatch below it is consistent
 with noise but does not prove it.
```

---

```text
 THE REFERENCE'S OWN INSTABILITY, measured on this same ray (see
 THE REFERENCE NOISE FLOOR above). The flat walk again, over a
 segment scaled by one part in ten million: same geometry,
 different rounding. Counted whether or not the skip walk agreed,
 so the floor is measured over the whole compared population.
```

---

```text
 Counted in its own column, NOT folded into agreement: the
 class is understood, but a change in its RATE would still be
 telling us something and must stay visible.
```

---

```text
 Claim a record slot. The counter keeps counting past the
 last slot on purpose, so the log can say how many rays the
 samples were drawn FROM -- a sample set that silently stops
 representing the population is worse than no samples.
```
