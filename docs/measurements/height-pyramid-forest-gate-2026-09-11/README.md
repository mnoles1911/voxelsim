# The height pyramid passes at the forest site, and the hole does not reproduce

`tools/voxel-heightpyramid-gates.ps1 -Prefix HPFOREST -SpawnAt -154740,-81476`,
run unchanged, zero code touched. Two static legs at the horizon pose (pitch 0,
yaw 0), 2560×1440 output at an internal 1664×936, sun frozen at 12:00 on 03-20.
Harness exited 0 and the clean arm's log carries no `Error` line.

This is the arm that has been **dark behind two console variables** since it was
written, and it has never been timed, because the one previous gate run fired
its falsifier.

## The two arms

| | red arm, bias 50 m | clean arm, bias 0 |
|---|---|---|
| rays consulted | 15,575,040 | 15,575,040 |
| rays advanced | 15,488,684 (99.45%) | **8,086,235 (51.92%)** |
| mid-ray re-entries | 879,932 | 2,452,289 |
| leaf cells skipped | 1,457,952,649 | 1,035,323,211 |
| traversal steps | 268,090,709 | 355,032,055 |
| **rays MISSED** | **714,221** | **0** |
| rays LATE (>0.1 m) | 6,673,697 | 282 |

**The red arm fired, which is what earns the right to believe the clean one.**
Lowering every height bound by 50 metres — deliberately claiming air where there
is ground — produced 714,221 missed rays. The test can fail.

**The clean arm lost nothing.** `0 MISSED over 15,575,040 consulted rays — no ray
lost its geometry outright.` The 479-ray hole that blocked this arm on 2026-08-27
does not reproduce here.

## Engagement more than doubled, at a site with a tenth the relief

| | 2026-08-27 site | this forest site |
|---|---|---|
| terrain relief in the field | 1,218.6 – 3,077.8 m | **19.0 – 304.6 m** |
| leaves filled | 100% | 100% |
| rays advanced | 21.64% | **51.92%** |
| leaf cells skipped per ray | 17.6 (≈225 m) | **66.5 (≈851 m)** |
| traversal steps per ray | 19.5 | 22.8 |

That is the opposite of what a flat site would suggest if the pyramid were only
skipping *vertical* air, and it is worth saying plainly: **the engagement did not
transfer, it improved.** At a 12.8 m leaf, 66.5 cells is roughly 851 m of ray
removed per ray, against ring outers of 64 / 128 / 256 / 512 m. The zero-setup
segment guard should therefore reject several whole ring segments per ray before
any entry work happens, and segment entry is where the marcher's cost lives.

The cost side rose too: 22.8 traversal steps per ray, each a buffer load into the
field. Whether the removed entries beat the added loads is precisely what has
never been measured.

## What is still open, and what must happen before any millisecond is quoted

**282 rays are still LATE**, hitting further than the control by more than 0.1 m,
with a maximum of 1,080.72 m. That is 0.0018% of rays, against 4,118 LATE on the
run that also missed 479. The harness treats MISSED as the hard failure and
passed this arm, but a long LATE tail is the same shape of defect one order
quieter, and it should be understood rather than waved through.

**The milliseconds on this leg are not quotable.** Hole statistics are a
permutation of the timed kernel — measured elsewhere at 4.448 ms against
4.548/4.463 — so an engagement leg's timings describe a different shader. Timing
has to be a separate pair with hole statistics off.

**The image gate has not been run.** This project shipped a −7.6% "win" that was
the marcher deleting a mountain, and the timing inverted to +3.1% once the image
was made honest. A renderer speedup is a claim about the image. Pinned-pose
captures with the pyramid on and off, against a control-versus-control noise
floor, come before any timing number is believed.

## Why this matters, and where it sits in the order

The marcher is 9.3 ms of a 20.0 ms GPU frame once size culling is on — 46% — and
the ceiling reasoned from the engagement above is 15–30% of that. But the slice
is game-thread bound after size culling (28.9 ms against 20.0), so this is the
lever that matters *after* the game-thread work, not before it. It is recorded
now because the gate that blocked it for a fortnight has just come back clean,
and because that fact perishes if nobody writes it down.
