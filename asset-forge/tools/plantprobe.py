"""Measure the dimensions a forester measures, off our own built voxel grid.

`docs/plant-proportion-research.md` fits tree specs to the Tallo database and to
FIA field measurements. Both of those describe a REAL tree with four numbers --
total height, crown width, crown depth, stem diameter at breast height -- so
this tool produces the same four numbers from a built asset, and it produces
them the way the field crew would, not the way the spec author intended.

That distinction is the whole point. A spec says `crown.radius_m 6.0`; the
generator treats that as a seed cloud for a colonisation algorithm and the
finished tree can be wider or narrower (`forge/skeleton.py:248` allows 1.7x for
exactly this reason). A spec says `height_m 25.0`; under `growth.model
colonize` nothing enforces it -- the top of the tree is wherever the crown
envelope ended up, which is `center_frac + height_frac*squash/2` times the
authored height and NOT the authored height. So comparing an authored slider to
a field measurement compares two different quantities. This measures the built
asset.

What it reports, per seed:

* `height_m`     -- bbox, ground to the highest solid voxel.
* `crown_w_m`    -- the widest horizontal spread of the crown, as the mean of
                    the X and Y extents at the level where that mean peaks.
                    A field crew measures two perpendicular crown diameters and
                    averages them, so this is the same convention.
* `crown_base_m` -- the lowest LEAF voxel. Height to live crown base.
* `crown_depth_m`/`crown_ratio` -- height minus crown base, and that over height.
* `dbh_cm`       -- diameter of the trunk at 1.3 m, as the area-equivalent
                    diameter (2*sqrt(A/pi)) of the wood component the trunk axis
                    passes through. Area-equivalent rather than a caliper width
                    because a fluted or buttressed bole is not round and a
                    caliper across it reports the flare, not the stem.
* `dbase_cm`     -- the same at 0.1 m, i.e. above the ground plane but inside
                    the root flare.
* `taper`        -- `dbh_cm / dbase_cm`. 1.0 is a post; a real stem is 0.7-0.9.

Leafless species (a dead snag, a cactus) have no leaf voxels, so `crown_base_m`
falls back to the lowest level whose horizontal spread exceeds 2.5x the trunk
width there -- i.e. where the thing stops being a pole. That fallback is marked
`*` in the output so it is never mistaken for a measured crown base.

    python tools/plantprobe.py american-beech scots-pine
    python tools/plantprobe.py --kind tree --seeds 1 2 3 --json out/probe.json
    python tools/plantprobe.py --kind grass --seeds 1 2 3
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np
from scipy import ndimage

import _path  # noqa: F401  (sys.path bootstrap)
from forge import materials, pipeline, spec as sm

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"

WOOD = {materials.resolve(n) for n in materials.WOOD_NAMES}
LEAF = {materials.resolve(n) for n in materials.LEAF_NAMES}


def _extent(mask2d: np.ndarray) -> tuple[float, float]:
    """(X extent, Y extent) in voxels of a 2-D occupancy slice."""
    if not mask2d.any():
        return 0.0, 0.0
    xs = np.nonzero(mask2d.any(axis=1))[0]
    ys = np.nonzero(mask2d.any(axis=0))[0]
    return float(xs[-1] - xs[0] + 1), float(ys[-1] - ys[0] + 1)


def _axis_component(slice2d: np.ndarray, seed_xy: tuple[int, int]) -> int:
    """Voxel count of the connected blob nearest `seed_xy` in a 2-D mask.

    The trunk leans and wanders, so the axis is not at a fixed (x, y). Label the
    slice, then take the component whose centroid is closest to where the bole
    was one slice down -- which is how the caller walks it up the stem.
    """
    lab, n = ndimage.label(slice2d, structure=np.ones((3, 3)))
    if n == 0:
        return 0
    cents = ndimage.center_of_mass(slice2d, lab, range(1, n + 1))
    sizes = ndimage.sum(slice2d, lab, range(1, n + 1))
    best = min(range(n), key=lambda i: (cents[i][0] - seed_xy[0]) ** 2
               + (cents[i][1] - seed_xy[1]) ** 2)
    return int(sizes[best]), cents[best]


def _stem_diameter(data: np.ndarray, voxel_m: float, z_m: float
                   ) -> tuple[float, tuple[float, float] | None]:
    """Area-equivalent diameter, in metres, of the bole at `z_m` above ground.

    Walks the trunk up from the ground slice so a leaning or wandering stem is
    still followed. Returns (diameter_m, centroid) or (0.0, None).
    """
    nz = data.shape[2]
    k_end = int(round(z_m / voxel_m))
    if k_end >= nz:
        return 0.0, None
    wood = np.isin(data, list(WOOD))
    if not wood[:, :, 0].any():
        # Nothing woody on the ground plane (a tuft, a cactus with no bark
        # material). Fall back to all solid voxels.
        wood = data != 0
    if not wood[:, :, 0].any():
        return 0.0, None
    ys, xs = np.nonzero(wood[:, :, 0])
    cur = (float(ys.mean()), float(xs.mean()))
    area = 0
    for k in range(0, k_end + 1):
        sl = wood[:, :, k]
        if not sl.any():
            return 0.0, None
        area, cur = _axis_component(sl, cur)
        if area == 0:
            return 0.0, None
    d_vox = 2.0 * math.sqrt(area / math.pi)
    return d_vox * voxel_m, cur


def measure(data: np.ndarray, voxel_m: float) -> dict:
    solid = data != 0
    nzz = np.nonzero(solid.any(axis=(0, 1)))[0]
    if nzz.size == 0:
        return {}
    height_m = float(nzz[-1] + 1) * voxel_m

    leaf = np.isin(data, list(LEAF))
    has_leaf = bool(leaf.any())

    # Crown width: the level whose mean of X and Y extent is largest. Measured
    # over ALL solid voxels, because a crown is what you see, wood included.
    per_z = []
    for k in range(int(nzz[0]), int(nzz[-1]) + 1):
        ex, ey = _extent(solid[:, :, k])
        per_z.append(((ex + ey) * 0.5, k, ex, ey))
    crown_w_vox, k_widest, ex, ey = max(per_z)
    crown_w_m = crown_w_vox * voxel_m

    if has_leaf:
        lz = np.nonzero(leaf.any(axis=(0, 1)))[0]
        crown_base_m = float(lz[0]) * voxel_m
        crown_top_m = float(lz[-1] + 1) * voxel_m
        base_est = False
    else:
        # No foliage. Find where the thing stops being a pole: the lowest level
        # whose spread is more than 2.5x the spread at 0.3 m.
        k0 = min(int(round(0.3 / voxel_m)), int(nzz[-1]))
        ref = max((_extent(solid[:, :, k0])[0] + _extent(solid[:, :, k0])[1]) * 0.5, 1.0)
        crown_base_m = height_m * 0.5
        for w, k, _, _ in per_z:
            if k > k0 and w > 2.5 * ref:
                crown_base_m = float(k) * voxel_m
                break
        crown_top_m = height_m
        base_est = True

    crown_depth_m = max(crown_top_m - crown_base_m, 0.0)
    dbh_m, _ = _stem_diameter(data, voxel_m, 1.3)
    dbase_m, _ = _stem_diameter(data, voxel_m, 0.1)

    # MEAN crown radius, via crown projected area. This is the number a forest
    # inventory records and the number Tallo stores, and it is NOT half the
    # bbox width: a lopsided crown with one long branch has a bbox far wider
    # than its mean radius, and comparing that bbox to a mean would report a
    # crown-width error that is really a definition difference. Projected area
    # is the union of occupied columns above the crown base, looking down.
    # Holes are filled first: a field crew reads the crown edge off the drip
    # line and a gap between two foliage clumps is inside the crown, not
    # outside it. Unfilled, a loosely-foliaged pine reports a crown radius that
    # is really a porosity measurement.
    k0 = int(round(crown_base_m / voxel_m))
    proj = ndimage.binary_fill_holes(solid[:, :, k0:].any(axis=2))
    cpa_m2 = float(proj.sum()) * voxel_m * voxel_m
    crown_r_mean_m = math.sqrt(cpa_m2 / math.pi) if cpa_m2 > 0 else 0.0

    return dict(
        height_m=round(height_m, 3),
        crown_w_m=round(crown_w_m, 3),
        crown_r_mean_m=round(crown_r_mean_m, 3),
        crown_proj_m2=round(cpa_m2, 2),
        cwmean_over_h=round(2 * crown_r_mean_m / height_m, 4) if height_m else None,
        crown_w_at_m=round(k_widest * voxel_m, 2),
        crown_base_m=round(crown_base_m, 3),
        crown_base_estimated=base_est,
        crown_depth_m=round(crown_depth_m, 3),
        crown_ratio=round(crown_depth_m / height_m, 4) if height_m else None,
        cw_over_h=round(crown_w_m / height_m, 4) if height_m else None,
        dbh_cm=round(dbh_m * 100, 2),
        dbase_cm=round(dbase_m * 100, 2),
        taper=round(dbh_m / dbase_m, 4) if dbase_m > 0 else None,
        dbh_over_h=round(dbh_m / height_m, 5) if height_m else None,
        voxels=int(solid.sum()),
    )


def probe(name: str, seeds, res=None) -> list[dict]:
    s, _ = sm.load(SPECS / f"{name}.json")
    out = []
    for seed in seeds:
        t = pipeline.build(s, seed, resolution_cm=res) if res else pipeline.build(s, seed)
        m = measure(t.grid.data, t.stats["voxel_cm"] / 100.0)
        m.update(name=name, seed=seed, kind=sm.get(s, "kind"),
                 spec_height_m=float(sm.get(s, "height_m")))
        out.append(m)
    return out


def _mean(rows, key):
    v = [r[key] for r in rows if r.get(key) is not None]
    return float(np.mean(v)) if v else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="*")
    ap.add_argument("--kind", action="append", default=None)
    ap.add_argument("--seeds", type=int, nargs="+", default=[1])
    ap.add_argument("--res", type=float, default=None)
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    names = list(a.names)
    if a.kind:
        for p in sorted(SPECS.glob("*.json")):
            d = json.loads(p.read_text(encoding="utf-8"))
            if d.get("kind") in a.kind:
                names.append(p.stem)
    if not names:
        ap.error("give spec names or --kind")

    allrows = []
    print(f"{'spec':24s} {'H_m':>6s} {'specH':>6s} {'CW_m':>6s} {'CWmn':>6s} "
          f"{'CW/H':>6s} {'CR':>6s} {'DBH_cm':>7s} {'taper':>6s} {'DBH/H':>7s}")
    for n in sorted(set(names)):
        try:
            rows = probe(n, a.seeds, a.res)
        except Exception as exc:  # a broken spec must not stop the sweep
            print(f"{n:24s} BUILD FAILED: {exc}")
            continue
        allrows += rows
        print(f"{n:24s} {_mean(rows,'height_m'):6.2f} "
              f"{rows[0]['spec_height_m']:6.2f} {_mean(rows,'crown_w_m'):6.2f} "
              f"{2*_mean(rows,'crown_r_mean_m'):6.2f} "
              f"{_mean(rows,'cwmean_over_h'):6.3f} {_mean(rows,'crown_ratio'):6.3f} "
              f"{_mean(rows,'dbh_cm'):7.1f} {_mean(rows,'taper'):6.3f} "
              f"{_mean(rows,'dbh_over_h'):7.4f}"
              + ("  *" if rows[0].get("crown_base_estimated") else ""))
    if a.json:
        Path(a.json).parent.mkdir(parents=True, exist_ok=True)
        Path(a.json).write_text(json.dumps(allrows, indent=1), encoding="utf-8")
        print(f"\nwrote {a.json}  ({len(allrows)} rows)")


if __name__ == "__main__":
    main()
