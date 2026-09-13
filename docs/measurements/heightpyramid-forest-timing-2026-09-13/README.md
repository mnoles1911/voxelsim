# The height pyramid is timed at last, and at the forest site it costs 1.2%

Three walk captures, one binary, a cvar apart, frames 0:200, temperate forest at
`-154740,-81476`, 256 m ring, Nanite default, hole statistics OFF (they permute
the kernel being timed). `voxel.HeightPyramid.Build=1` on every arm, so the CPU
field, its tick cost and its streaming interaction are present in all three and
cannot be mistaken for the marcher's doing.

## The result, against its own noise floor

| pair | GPU/VoxelMarch | GPUTime |
|---|---|---|
| **off vs off** (the floor) | 8.890 → 8.901, **+0.1%** | 12.626 → 12.641, +0.1% |
| off vs **on** | 8.901 → 9.012, **+1.2%** | 12.641 → 12.752, +0.9% |

**The arm engaged** — `HeightPyramid=1` on the command line of the armed run and
`=0` on both controls, and the difference is twelve times the floor measured
between two identical runs.

**So the pyramid costs about 1.2% of marcher time here. It does not pay.**

## Why, and it was predictable from this project's own cost model

The engagement gate on 2026-09-11 read **51.92% of rays advanced** and **0 of
15,575,040 rays missed**, which is what licensed timing it at all. But rays
advancing is not the same as work removed. The marcher's cost is **segment
entry**, not iteration: a ray runs 8 ring segments with a fallthrough ladder up
to 2 rungs deep, each entry re-zeroing a ~35-dword hit struct, re-initialising a
12-field chunk cache and a 7-field brick cache, recomputing three reciprocals and
taking a scattered load into a 64 MiB index grid. Air iterations are nearly free
by comparison.

The pyramid skips **air**. It does not remove segment entries. So it removes the
cheap half and adds a per-ray pyramid traversal on top, and the sum comes out
slightly positive.

## What this does and does not retire

It retires the pyramid **as a lever at the forest site**, which is the site the
slice plan is about and where the marcher is 8.9 ms of a 12.6 ms GPU frame.

It does **not** retire it everywhere, and the reason is measured rather than
hopeful: engagement differs enormously by site — 21.64% of rays advanced at the
2026-08-27 site against 51.92% here — so the balance between skipped air and
added traversal is site-dependent. A vista or massif site, where relief is large
and rays travel far above distant ground, is a separate question with a separate
pair. The image gate has already passed at the massif
(`docs/measurements/heightpyramid-image-gate-2026-09-13/`), so that pair is
unblocked.

What this does close is the idea that the pyramid is the answer to the marcher
being 70% of the GPU frame. At this site it is not, and the next marcher idea
has to attack **segment entry** — the thing the cost model actually names —
rather than empty space.
