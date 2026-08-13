"""How much bare wood does a tree actually show?

"The twigs look bare" is a statement about the SILHOUETTE, and counting leaf
voxels cannot answer it: a crown can gain half a million leaves and still show
the same branches if the new leaves landed inside the canopy. What you see from
outside is the first solid voxel down each line of sight, so that is what this
measures -- shoot four orthogonal views at the tree and ask, of everything
visible, what fraction is wood.

Read it as a comparison, not an absolute. A pine SHOULD show wood; a spruce
should not. The number is for telling a change from no change, which is the
thing a voxel count kept getting wrong.

    python tools/barecheck.py tundra-pine birch hero-sequoia
    python tools/barecheck.py --seeds 3 temperate-oak
"""
import argparse
from pathlib import Path

import numpy as np

import _path  # noqa: F401  (sys.path bootstrap)
from forge import materials, pipeline, spec as sm

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"


def _first_hit(data: np.ndarray, axis: int, flip: bool) -> np.ndarray:
    """The material of the nearest solid voxel along each line of sight."""
    d = np.flip(data, axis) if flip else data
    solid = d != 0
    seen = solid.any(axis=axis)
    # argmax on a boolean gives the first True, or 0 where there is none; the
    # `seen` mask is what separates "hit at index 0" from "never hit at all".
    idx = solid.argmax(axis=axis)
    hit = np.take_along_axis(d, np.expand_dims(idx, axis), axis)
    return np.squeeze(hit, axis)[seen]


def bareness(data: np.ndarray, band: tuple[float, float] = (0.0, 1.0)
             ) -> tuple[float, int]:
    """Fraction of the visible surface that is wood, over four side views.

    `band` clips to a height range first, because a bare BOLE and a bare TWIG
    are different findings and the whole-tree number cannot tell them apart. A
    sequoia is six metres through at the base and eighty-six tall, so its trunk
    alone is most of what a side view sees -- scored whole, it reported 61% bare
    and would have sent me tuning foliage to fix a trunk that is meant to show.
    """
    wood = {materials.resolve(n) for n in materials.WOOD_NAMES}
    nz = np.nonzero((data != 0).any(axis=(0, 1)))[0]
    if nz.size == 0:
        return 0.0, 0
    z0, z1 = int(nz[0]), int(nz[-1]) + 1
    lo = z0 + int(band[0] * (z1 - z0))
    hi = z0 + max(int(band[1] * (z1 - z0)), lo + 1)
    d = data[:, :, lo:hi]
    shown = np.concatenate([_first_hit(d, ax, fl)
                            for ax in (0, 1) for fl in (False, True)])
    if shown.size == 0:
        return 0.0, 0
    is_wood = np.isin(shown, list(wood))
    return float(is_wood.mean()), int(shown.size)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="+")
    ap.add_argument("--seeds", type=int, default=1)
    a = ap.parse_args()

    for n in a.names:
        s, _ = sm.load(SPECS / f"{n}.json")
        # Score inside the crown. The clear bole below it is meant to be wood,
        # and including it drowns the signal on anything tall.
        clear = float(sm.get(s, "trunk.clear_frac"))
        band = (min(clear + 0.05, 0.9), 1.0)
        rows = []
        for seed in range(1, a.seeds + 1):
            t = pipeline.build(s, seed)
            frac, px = bareness(t.grid.data, band)
            whole, _ = bareness(t.grid.data)
            rows.append(frac)
            print(f"  {n:18s} seed {seed}  crown {frac:6.1%} wood "
                  f"(whole tree {whole:5.1%})  {px:,} px  "
                  f"{t.stats['height_m']:.1f} m  {t.stats['clumps']} clumps  "
                  f"{t.stats['voxels']:,} vox")
        if a.seeds > 1:
            print(f"  {n:18s} mean {np.mean(rows):6.1%}  "
                  f"spread {min(rows):.1%}-{max(rows):.1%}")


if __name__ == "__main__":
    main()
