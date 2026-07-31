#!/usr/bin/env python3
"""Measure RISER HEIGHT: how many voxel levels the surface jumps at each step.

The owner's correction, and it matters: the visible bands are not one-voxel steps
in a smooth ramp, they are TERRACE RISERS -- the ground jumps straight up several
voxels at once, so each band is the side of a terrace. band_period.py measured the
horizontal run (the tread); this measures the vertical jump (the riser).

The distinction is diagnostic, not cosmetic:

  riser == 1 voxel everywhere  ->  quantisation of a smooth surface. Nothing in the
                                   data is terraced; the lattice made the steps.
  riser >> 1 voxel             ->  the CONTINUOUS surface contains near-vertical
                                   scarps. Something is putting them there, and no
                                   amount of roughness will remove what a generator
                                   term is deliberately drawing.

A riser of k voxels over one 10 cm cell is a local gradient of k -- e.g. 5 voxels
is a 500% grade, five times steeper than the steepest ground in the window. Such
segments cannot be an accident of sampling.
"""
import argparse

import numpy as np

VOXEL_MM = 100
CELL_M = 0.1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--n", type=int, default=512)
    a = ap.parse_args()

    h = np.fromfile(a.bin, dtype="<i4", count=a.n * a.n).reshape(a.n, a.n).astype(np.int64)
    lvl = np.floor_divide(h, VOXEL_MM)

    jumps = np.concatenate([np.abs(np.diff(lvl, axis=0)).ravel(),
                            np.abs(np.diff(lvl, axis=1)).ravel()])
    jumps = jumps[jumps > 0]
    n = len(jumps)
    print(f"{n} level transitions over a {a.n * CELL_M:.1f} m window\n")
    print("  riser height   share    (a k-voxel riser over one 10 cm cell is a grade of k00%)")
    for k in (1, 2, 3, 4, 5):
        share = (jumps == k).sum() / n
        print(f"  {k:2d} voxel{'s' if k > 1 else ' '}      {share * 100:5.1f}%")
    big = (jumps >= 6).sum() / n
    print(f"  6+ voxels      {big * 100:5.1f}%")
    print(f"\n  mean {jumps.mean():.2f} voxels, p99 {np.percentile(jumps, 99):.0f}, "
          f"max {jumps.max()}")

    # Where the tall risers sit, as a fraction of ground -- a few tall scarps
    # scattered about read as cliffs (good); tall risers lining up across a face
    # read as terracing (the artifact).
    tall = np.zeros_like(lvl, dtype=bool)
    tall[:-1, :] |= np.abs(np.diff(lvl, axis=0)) >= 3
    tall[:, :-1] |= np.abs(np.diff(lvl, axis=1)) >= 3
    print(f"  cells adjacent to a 3+ voxel riser: {tall.mean() * 100:.1f}% of the window")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
