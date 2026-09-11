# The marcher: what it costs, what is already built and switched off, and what to do

`GPU/VoxelMarch` reads **9.47 ms** of a 40.48 ms GPU frame at the temperate
forest site, 1280×720 at 65% screen percentage. At the shipping 48 m ring it is
9.14 ms of a 12.68 ms frame — **72% of the GPU**, and once size culling lands at
256 m it is 9.05 of 19.54, or 46%. Either way the marcher becomes the floor as
soon as the understory stops being one.

Up to ~0.3 ms of that 9.47 is the depth pre-emit, which sits inside the March
bracket and is bounded above by the separate emit timer. Read 9.47 as an upper
bound on the march proper.

## The shape of the cost

Two things are settled and should not be re-derived.

**The march is full-screen: one thread per view pixel, one ray per thread, every
tile, every frame.** There is no tile classifier and no ray compaction ahead of
it. `FVoxelMarchCompactCS` exists but runs *after* the march, building indirect
draw args for the emit raster pass so that the depth write's HTILE invalidation
is confined to tiles that actually hit terrain. It saves raster time and zero
marcher time. **Any idea that saves work on sky rays cannot be implemented by
not dispatching them.**

**The cost is segment entry, not iteration.** A ray runs 8 ring segments with a
fallthrough ladder up to 2 rungs deep — up to 16 walk entries — at about 0.9 ms
per ring. Air iterations are nearly free by comparison. Each entry re-zeroes a
~35-dword hit struct, re-initialises a 12-field chunk cache and a 7-field brick
cache, recomputes three reciprocals, and then takes one scattered 4-byte load
into a chunk index grid that is 64 MiB and 99.7% empty.

**It is pixel-dependent, but not proportionally.** `r.ScreenPercentage=33` gives
3.87× fewer rays (the marcher prints its own view size: 832×468 at 65, 423×238
at 33) and the marcher falls 9.47 → 5.14. Proportional would be 2.45. A
two-point split says 3.63 ms fixed plus 5.84 ms scaling — **and that arithmetic
is not trustworthy.** The identical fit earlier in this work claimed 3.71 ms
fixed and was retracted when a flight leg at 2.4× the pixels totalled 5.04 ms.
Two points cannot separate a fixed cost from a sublinear one. A five-point
ladder (screen percentage 100/80/65/45/33) is queued; it is the instrument.

## Four mechanisms are built, hole-argued, instrumented — and switched off

This is the most important finding in this file. None of these needs writing.

| mechanism | cvar | default | status |
|---|---|---|---|
| Height pyramid, march side | `voxel.March.HeightPyramid` | 0 | **dark** |
| Height pyramid, field build | `voxel.HeightPyramid.Build` | 0 | **dark, second switch** |
| BlockSkip, 4³-chunk occupancy | `voxel.March.BlockSkip` | 0 | **never compiled, never engaged in any log** |
| BlockSkySkip | `voxel.March.BlockSkySkip` | 0 | dark, depends on BlockSkip |

Setting only the march side of the height pyramid binds a one-float infinity
buffer and forces the arm inert **with no error**. That is this project's
signature failure and is exactly what happened on 2026-09-04: the falsifier read
VACUOUS because the pyramid was consulted zero times, the arm it rides never
having engaged.

## The height pyramid is the top lever, and it has a hole

It is a **max** pyramid over a provable upper bound on terrain height, so
skipping *above* it is sound — this is not the plain OR pyramid the project's
own research correctly called unsafe. Absence reads as positive infinity, so
unbuilt, declined and out-of-field cells are treated as possibly occupied, and
there is no sentinel to invert.

Run once at the horizon on 2026-08-27 at a different site, with a 100%-filled
field:

| measure | value |
|---|---|
| rays consulted | 36,582,192 |
| rays advanced | 21.64% |
| leaf cells skipped per ray | 17.6 (≈225 m at a 12.8 m leaf) |
| DDA steps added per ray | 19.5 |
| **rays MISSED at zero bias** | **479** |

A 225 m advance makes the zero-setup segment guard fire for ring segments 0, 1
and often 2 — roughly **6 of the ~16 walk entries per ray, before any setup at
all**. Naive ceiling ~35% of the marcher; realistic band 15–30%, or 1.4–2.8 ms,
net of the 19.5 loads the traversal adds. It is the only proposed mechanism that
reaches horizontal rays, which is where the frame is actually spent and which a
Z slab provably cannot help — the old Z cut skipped 0.00% of 3.3 billion
decisions at the horizon.

**It has never been timed, and it must not be, until the 479 reads zero.** The
red arm works: biasing every height bound down by 50 m produced 42 million
violations, so the falsifier can fire. 479 missed rays with a late-distribution
tail reaching 2,449 m is terrain the pyramid skipped, which is a hole, and a
hole outranks a speedup in this project by standing policy.

No diagnosis of the 479 exists anywhere in the repo. The leading hypothesis,
labelled as inference and not fact: the bound is an upper bound on the **fine**
surface, but coarse rings draw chunks voxelised at 0.1 × 2^L metres, so at level
7 a legitimately solid voxel is 12.8 m tall and can sit above the fine surface
the pyramid bounds. The late distribution's 10–100 m bucket is the right
magnitude for that.

**Next step, zero code:** re-run `tools/voxel-heightpyramid-gates.ps1` at the
current temperate forest site. It already sets both switches and runs its red
arm before its clean arm. Two questions, two legs: does the 21.64% engagement
transfer off the 2026-08-27 site, and does the hole reproduce here? A null on
the first kills the top marcher lever cheaply. That run is queued.

## The rest, ranked, with the dead ones named so they stop being re-proposed

**Make segment entry cheaper.** Every entry re-initialises ~54 dwords that did
not change between rungs, and recomputes the safe direction and its reciprocal
from a per-ray constant. Ceiling unknown and honestly so: if a third of the
~2,000-cycle entry is this prologue, it is ~1 ms; if those cycles are really the
first chunk-index cache miss and the loop-setup dependency chain, it is near
zero. The cheapest discriminator is a RenderDoc instruction-level capture of the
shipping permutation, looking for scratch traffic and reading the register count
— twenty minutes, no code, no leg — and it appears never to have been taken.

**Cut the fallthrough ladder's second rung.** The retry rung is ~47% of all walk
entries at sky and hits 0.054% of the time. Halving that population is ~20% of
the marcher, the largest single number available. It is ranked below the pyramid
because the two obvious routes are corpses. The rung probe was refuted at an
8.4% skip rate against a pre-registered 95%, and ran 30× slower; its own write-up
says not to build a second version with a cheaper probe, because the multiplier
is the problem. The sky ladder was retired for reading −7.6% by deleting a
mountain. The residency route is closed outright, because retiring the buried
skip made sky air resident with records. Hole risk here is extreme: this gate is
what fills the black arcs at LOD boundaries, and any predicate that retries
fewer rays can put sky where terrain belongs.

**Measure BlockSkip once and then delete it or ship it.** Built, permutation
wired, and its consult counter reads zero in every log. Its ceiling is low — it
removes air *iterations*, which the cost autopsy priced at +0.11 ms on 5.77 ms,
i.e. nothing — and it adds two buffer loads per block. It is on this list only
because leaving a built, undefaulted, never-once-engaged arm in the tree is
exactly the state this project has repeatedly been burned by. One leg closes it.

**Dropping a ring is not an optimisation.** The ring line is linear at ~0.9 ms
per ring, so cutting the outermost is worth ~0.9 ms — and halves visible terrain
radius from 8 km to 4 km. The cascade was deliberately grown to 8 rings. Ray
count and reach are owner product decisions, as the bound arm's retirement
already established. Recorded here so it is not presented as a free win.

**Pre-march tile classification is dead.** Every candidate predicate over an 8×8
tile is either the height pyramid, better applied per-ray, or residency, which
is closed. It also cannot help the horizon, where most tiles hit.

**Carrying the chunk cache across rungs is dead.** The obvious observation is
right — the cache is thrown away at every entry, so the first chunk of every
segment is a guaranteed scattered miss. The fix does not work: a chunk record is
validated against the record level, and a chunk coordinate names a different
place at each level. A cross-rung cache would need keying by level and
coordinate together, and its hit rate across a level change is structurally near
zero.

## What is not determined

- The cause of the 479 missed rays. No diagnosis exists; the coarse-LOD
  quantisation hypothesis above is inference. A per-level breakdown of
  violations on one gate leg settles it.
- Whether the 2026-08-27 engagement transfers to this site. Flat-terrain and
  different-site numbers do not transfer in this project, by hard experience.
- Whether the ~2,000-cycle entry is scratch, prologue arithmetic, or the index
  miss. A RenderDoc instruction-level capture settles it and has never been
  taken.
