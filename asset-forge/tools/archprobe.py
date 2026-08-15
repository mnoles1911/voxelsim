"""Does the arch carve actually open a hole, and if not, which guard stopped it?

`_arch` bails silently in four different places -- no scipy, an empty grid, a
radius under two voxels, and the three-attempt fit loop running out -- and every
one of them restores the stone and returns as if nothing happened. That is the
same failure shape as the weathering pass: the slider looks wired up, the build
succeeds, and the feature is simply absent. So the guards get instrumented
rather than reasoned about.

    python tools/archprobe.py hero-natural-arch
"""
import sys
from pathlib import Path

import numpy as np

import _path  # noqa: F401  (sys.path bootstrap)
from forge import pipeline, rock, spec as sm

ROOT = Path(__file__).resolve().parents[1]

TRACE = []
_real = rock._arch


def traced(grid, rng, amount, out=None):
    occ = grid.data != 0
    before = int(occ.sum())
    box = rock._occupied_box(occ)
    span = [box[i].stop - box[i].start for i in range(3)]
    TRACE.append(f"  called: amount={amount:.2f} span_vox={span} solid={before}")
    why = _real(grid, rng, amount, out=out)
    after = int((grid.data != 0).sum())
    TRACE.append(f"  result: {why}")
    TRACE.append(f"          removed {before - after} voxels "
                 f"({100.0 * (before - after) / max(before, 1):.1f}%)")


def main():
    name = sys.argv[1]
    # An arch that works on seed 1 and not on seed 2 is not a working arch, and
    # a single-seed probe cannot tell the difference. `--seeds N` walks 1..N and
    # prints the daylight for each, because that is the number the asset is
    # judged on. `--res` overrides the voxel size, which is how you get a 68 m
    # hero to build at all on a machine that cannot hold it at 10 cm.
    seeds = 1
    res = None
    if "--seeds" in sys.argv:
        seeds = int(sys.argv[sys.argv.index("--seeds") + 1])
    if "--res" in sys.argv:
        res = sys.argv[sys.argv.index("--res") + 1]
    s, _ = sm.load(ROOT / "specs" / f"{name}.json")
    rock._arch = traced

    if seeds > 1:
        from scipy import ndimage
        print(f"{name}: {seeds} seeds"
              f"{f' at {res} cm' if res else ''}")
        for seed in range(1, seeds + 1):
            TRACE.clear()
            try:
                a = pipeline.build(s, seed, resolution_cm=res)
            except MemoryError as e:
                print(f"  seed {seed}: DOES NOT BUILD -- {e}")
                continue
            occ = a.grid.data != 0
            best, opens = 0, 0
            for axis in (0, 1):
                sil = occ.any(axis=axis)
                holes = rock.daylight(sil)
                if int(holes.sum()) > best:
                    best = int(holes.sum())
                    opens = ndimage.label(holes)[1] if holes.any() else 0
            why = TRACE[1][10:] if len(TRACE) > 1 else "arch never ran"
            print(f"  seed {seed}: {a.stats['height_m']:5.1f} m  "
                  f"daylight {best:7,} px in {opens} opening(s)   {why}")
        return

    a = pipeline.build(s, 1, resolution_cm=res)
    print(f"{name}: {len(TRACE) // 3} arch call(s) across the fitting loop")
    for line in TRACE:
        print(line)

    # Cache the built grid. Every look at a 40 m arch costs three minutes of
    # rebuilding otherwise, and the questions worth asking about it come in
    # batches.
    np.savez_compressed(ROOT / "out" / f"{name}.npz",
                        data=a.grid.data, voxel_m=a.grid.voxel_m)

    # A hole you can see through is a hole in the SILHOUETTE, not a cavity.
    # Project along each horizontal axis and look for enclosed background, and
    # write the projections out -- the isometric preview looks down one fixed
    # corner and an opening can hide behind the stone's own thickness there,
    # so the count and the picture disagreeing is the interesting case.
    from scipy import ndimage
    from PIL import Image
    occ = a.grid.data != 0
    for axis, label in ((0, "x"), (1, "y")):
        sil = occ.any(axis=axis)
        holes = rock.daylight(sil)
        n = 0
        if holes.any():
            _, n = ndimage.label(holes)
        print(f"  looking along {label}: {n} enclosed opening(s), "
              f"{int(holes.sum())} px of {int(sil.sum())} px of stone")
        img = np.zeros(sil.shape + (3,), np.uint8)
        img[sil] = (210, 200, 180)
        img[holes] = (200, 60, 60)
        # Array is (across, z) with z increasing upward; flip so up is up.
        Image.fromarray(np.transpose(img, (1, 0, 2))[::-1]).save(
            ROOT / "out" / f"{name}-silhouette-{label}.png")


if __name__ == "__main__":
    main()
