"""Freeze the grid as `_arch` receives it, so the carve can be worked on alone.

A 90 m arch is five minutes a look, and almost all of that is the mass,
faceting and weathering passes that the carve does not depend on. The fitting
loop happens to run the same stone at three sizes, and the small one fails the
same way the big one does -- so the small one is the one to keep.

    python tools/archrepro.py hero-arch-colossal      # writes out/archin-*.npz
"""
import sys
from pathlib import Path

import numpy as np

import _path  # noqa: F401  (sys.path bootstrap)
from forge import pipeline, rock, spec as sm

ROOT = Path(__file__).resolve().parents[1]
_real = rock._arch
SAVED = []


def capture(grid, rng, amount):
    n = len(SAVED)
    p = ROOT / "out" / f"archin-{n}.npz"
    # Nothing here may touch `rng`: `_arch` draws from it to place the opening,
    # so a stray draw moves the very thing being reproduced.
    np.savez_compressed(p, data=grid.data, voxel_m=grid.voxel_m, amount=amount)
    SAVED.append(p)
    print(f"  saved {p.name}  shape={grid.data.shape}  solid={int((grid.data != 0).sum())}")
    return _real(grid, rng, amount)


def main():
    name = sys.argv[1]
    s, _ = sm.load(ROOT / "specs" / f"{name}.json")
    rock._arch = capture
    pipeline.build(s, 1)


if __name__ == "__main__":
    main()
