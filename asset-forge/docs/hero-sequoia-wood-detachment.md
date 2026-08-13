# `hero-sequoia` shed wood on most of its individuals

**Status: FIXED, 2026-08-13, in `forge/rasterize.py::_flute_trunk` (not by me).
`python tools/buildcheck.py` passes: 77 builds, 0 problems.** Kept because the
route to the fix is worth more than the fix, and because the last section is
still open.

Found 2026-08-12 while adding the fish kind. It was not a fish defect and it
was not new; the fish work only changed which individual gets built.

**The cause was not what I guessed.** I had it as a branch whose join to the
trunk landed on the wrong side of a rounding decision, and offered two leads,
both about the skeleton. It was neither: the FLUTING cut removed a band under
the trunk's outer relief and left a rind attached only at its corners. My
diagnosis that it was pre-existing and unrelated to the fish work was right;
my diagnosis of the mechanism was wrong, and the difference between those two
is the difference between a measurement and a hypothesis. Everything above the
"Where to look" heading was measured and held up. Everything under it was a
guess and did not.

    hero-sequoia seed 1: broken: 3235 wood voxels are not joined to the trunk

## What is actually wrong

Measured at 20 cm on seed 6, so it is cheap to reproduce:

    wood voxels                                 348,274
      face-connected components                      33  (largest 348,201)
      26-connected components                         1  (largest 348,274)
      wood not FACE-joined to the trunk               73
        ...of which corner-joined to it after all     73
        ...genuinely separate wood                     0
      whole asset 26-connected pieces                  1

**Nothing floats.** The tree is one piece and every wood voxel touches the
trunk. What fails is that 73 of them touch it only at a CORNER rather than face
to face — 0.02% of the wood.

The `wood connected` check is face-only on purpose, and the reason is written
down in `grid.component_fraction`: *"a branch joined to the trunk only at a
corner is a branch that falls off"*. So the check is right and the geometry is
wrong; this is a real defect, just a very small one.

## Why it surfaced now

`spec_hash` is mixed into the seed (`pipeline.rng_for`), so **adding any row to
`spec.PARAMS` moves every species in the library onto a different individual.**
Adding the `fish` and `detail` groups did exactly that.

Under the spec hash this species had BEFORE those rows existed:

    old-hash seed 1:   354,029 vox   wood_detached  0
    old-hash seed 2:   706,280 vox   wood_detached  8
    old-hash seed 3:   491,177 vox   wood_detached  2
    old-hash seed 4:   344,326 vox   wood_detached 44
    old-hash seed 5:   448,699 vox   wood_detached 38
    old-hash seed 6:   784,257 vox   wood_detached  2
    old-hash seed 7:   528,025 vox   wood_detached  0
    old-hash seed 8:   723,332 vox   wood_detached  0

**Five of eight individuals were already broken.** Seed 1 was one of the three
clean ones, and seed 1 is the only individual `buildcheck` ever builds.

## The bigger finding

The library has been green on **one seed per species, which is a sample of one.**
That is the actual lesson here and it is worth more than the tree.

`python tools/buildcheck.py --skip-heavy --seeds 1 2` is 146 builds and passes,
so the ordinary library is sound at two. The four heavy heroes have never had a
multi-seed pass because they are slow — `hero-arch-colossal` alone is 13 minutes
at its authored size — and this is what was hiding in there.

## Reproduce

    python -c "from forge import pipeline, spec as sm; s,_=sm.load('specs/hero-sequoia.json'); \
      [print(i, pipeline.build(s,i,resolution_cm=20).stats['wood_detached']) for i in range(1,7)]"

    seed 1: 29   seed 2: 1   seed 3: 60   seed 4: 0   seed 5: 0   seed 6: 73

## Where to look

Not investigated further — this was found in a fish session and fixing the tree
rasterizer from one is how unrelated things get broken. Two leads, in order of
how plausible they look:

1. **`grid._traverse` can stop short.** It breaks on `tmax[i] > 1.0`, and
   consecutive skeleton segments each start at `floor(p0)` of their own start
   point. If a segment's run ends one voxel before `floor(p1)` while the next
   segment starts there, the two are corner-adjacent rather than face-adjacent.
   That would be resolution-dependent and rare, which matches: 73 voxels in
   348,000, on some seeds and not others.
2. **`rasterize.foliage` overwriting wood.** `grid.blob` defaults to
   `only_air=True` so it should not, but the frond and strand paths are worth
   checking against that.

Whatever the cause, the fix belongs with a multi-seed run of all four heroes,
not with a single green seed.
