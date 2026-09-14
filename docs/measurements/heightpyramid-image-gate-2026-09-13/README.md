# The height pyramid does not change the picture at the massif

`tools/voxel-heightpyramid-image-ab.ps1 -Prefix hpimg-massif`, run unchanged.
Five captures at 2560x1440, spawn `-61440,-61440` at +220 m, pitch 0, settle
150 s each, sun frozen 12:00 on 03-20. All five produced a PNG — no VOID
captures — and each was copied to a deterministic name at the moment it was
taken, never paired by modification time.

This is the gate that had never been run. The engagement gate passed at the
forest site on 2026-09-11 with 0 missed rays of 15.6 M, but this project shipped
a −7.6% "win" on this renderer that was the marcher deleting a mountain, and the
timing inverted to +3.1% once the image was made honest. A renderer speedup is a
claim about the image, so the image comes first.

## The result

| pair | % pixels over 8/255 | mean delta | concentration |
|---|---|---|---|
| **control vs control** (the noise floor) | **0.60%** | 1.27/255 | 0.22 |
| away yaw 225, control vs armed | **0.55%** | 1.22/255 | 0.24 |
| massif yaw 45, control vs armed | **0.41%** | 1.12/255 | 0.29 |

**Both armed pairs differ LESS than the renderer differs from itself.** Two
captures of the same arm at the same pose are not identical — TSR carries
temporal state, streaming order varies, the pawn settles to a slightly different
sub-voxel position — and that floor is 0.60%. The armed pairs sit at 0.55% and
0.41%, inside it.

**And the difference has no shape.** Concentration is 0.24 and 0.29 against 0.22
for the control pair, where 0.10 is perfectly diffuse and near 1.0 is one object.
The 90% box covers 71.5% and 70.2% of the frame. A deleted ridge, a missing
skyline or an arc would concentrate; this is spread across sky and rock alike,
which is the signature of temporal noise rather than of geometry that stopped
being drawn.

## Why the away yaw is the one that matters

The massif at `-57440,-57440` sits +x/+y of the spawn, so yaw 45 looks at it and
yaw 225 looks away, down the valley that falls to 1,485 m. The pyramid skips
hardest where local ground is far below the ray — looking away — so a too-low
bound deletes terrain there first. A clean shot toward the massif proves little
on its own; the away pair is the evidence, and it is the cleaner of the two.

## What this does and does not license

It licenses **timing**, which has never been measured for this arm: the gate that
blocked it is now clear at this site on the image as well as on the ray
statistics. Timing must be a separate pair with hole statistics OFF, because
those permute the kernel being timed (measured elsewhere at 4.448 ms against
4.548/4.463).

It does **not** transfer to the forest site. The same script at the forest spawn
is the next image gate, and the forest presents the marcher with a canopy —
high-variance occupancy with rays threading gaps — which is a different workload
from bare massif relief. The forest's engagement number is already known to be
different (51.92% of rays advanced there against 21.64% at the 2026-08-27 site),
so its image is a separate question and not a formality.
