#!/usr/bin/env python3
"""The corridor's reaches COMPOSED the way the client draws them.

    python tools/corridor_composed_reach.py \
        --ns terrain-diffusion-unlabeled-80b9ca451a23eae4-b52995abb \
        --npz-dir D:/tmp/seamwidth-npz \
        --tiles="-11,-4 -11,-5 -12,-5 -11,-6"

WHY THIS EXISTS. `measure_corridor_fragmentation.py` labels the P2 river plane
and `corridor_water_report.py` labels the npz's `water_surface_m`; both answer
"how long is a river reach". Neither answers the question the owner actually
looks at, because `CompositeWaterSampler` draws the river plane UNIONED with
the P1 basins' lake extents -- so a reach that runs through a lake is ONE
continuous piece on screen and TWO pieces in either existing tool. The 20,269 m
figure in docs/water-waves-plan-2026-08-04.md is this composition, and until
now it existed only in an ad-hoc script, which meant the headline claim about
this river was the one number nothing in the repo could reproduce.

Nothing here is re-implemented. The labeller, the pixel pitch and the exact
convex-hull span come from `measure_corridor_fragmentation`; the lake footprint
comes from `bake.basins.lake_extent_mask`, which is the single definition the
client's `IWaterSampler` also answers to. This tool only unions and reports.

WHY IT NEEDS THE NPZ AS WELL AS THE TILES. `lake_extent_mask` floods over the
RE-OPENED GROUND, and a `.vxtl` carries prefiltered B-spline CONTROL POINTS,
not samples -- flooding those would be a different surface and a different
shoreline. The npz's `elevation_m` is the bake's own `z_final`, which is what
the mask is defined on. Everything else (basin rows, the river plane) is read
from the shipped tile, so the composition is anchored to bytes in the world.

THE LAKE FRACTION IS PART OF THE ANSWER, not a footnote: a 13,030 m component
that is 94.4% lake sheet is a chain of lakes, and quoting it as a river has
already been a mistake once.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from terrain_service import tile_codec as tc  # noqa: E402
from terrain_service.bake import basins as bk  # noqa: E402
from measure_corridor_fragmentation import (  # noqa: E402
    FINE_PX,
    PIXEL_M,
    label8,
    max_pairwise_m,
)

KIND_NAME = {
    tc.BASIN_KIND_DRY_PLAYA: "dry_playa",
    tc.BASIN_KIND_SALT_FLAT: "salt_flat",
    tc.BASIN_KIND_SEASONAL: "seasonal",
    tc.BASIN_KIND_LAKE_TERMINAL: "lake_terminal",
    tc.BASIN_KIND_LAKE_OVERFLOWING: "lake_overflowing",
}


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache-dir", default="D:/voxelsim/tile-cache")
    ap.add_argument("--seed-hex", default="000000000135276f")
    ap.add_argument("--ns", required=True, help="fine namespace directory name")
    ap.add_argument("--npz-dir", required=True,
                    help="bake dump for the SAME tiles; supplies the re-opened "
                         "ground the lake extents flood over")
    ap.add_argument("--tiles", required=True)
    ap.add_argument("--top", type=int, default=6)
    args = ap.parse_args()

    tiles = [tuple(int(v) for v in p.split(",")) for p in args.tiles.split()]
    ns_dir = Path(args.cache_dir) / args.ns
    npz_dir = Path(args.npz_dir)

    xs = sorted({x for x, _ in tiles})
    ys = sorted({y for _, y in tiles})
    x0, y0 = xs[0], ys[0]
    H = (ys[-1] - y0 + 1) * FINE_PX
    W = (xs[-1] - x0 + 1) * FINE_PX

    Rs, Cs, Es, Ls = [], [], [], []
    kinds = {k: 0 for k in KIND_NAME}
    n_basins = river_tot = lake_tot = 0
    lake_area_m2 = 0.0

    for (x, y) in tiles:
        p = ns_dir / args.seed_hex / "s16" / f"{x}_{y}.vxtl"
        if not p.exists():
            print(f"({x},{y}): tile not found at {p}", file=sys.stderr)
            return 1
        t = tc.decode_v2(p.read_bytes())
        if t.water_cp is None:
            print(f"({x},{y}): bv{t.bake_ver} has NO water plane -- this "
                  "namespace predates P2", file=sys.stderr)
            return 1
        z = np.load(npz_dir / f"{x}_{y}.npz")["elevation_m"]

        river = np.asarray(t.water_cp) >= 0
        lake = np.zeros_like(river)
        for e in (t.basins or ()):
            kinds[e.kind] += 1
            n_basins += 1
            lake |= bk.lake_extent_mask(
                z_open=z, seed_px=e.seed_px,
                surface_m=e.surface_mm / 1000.0, bbox_px=e.bbox_px)
        comp = river | lake
        river_tot += int(river.sum())
        lake_tot += int(lake.sum())
        lake_area_m2 += float(lake.sum()) * PIXEL_M * PIXEL_M

        r, c = np.nonzero(comp)
        elev = (np.asarray(t.elevation_cp)[comp].astype(np.float64)
                * tc.QUANT_MM[t.quant] + t.base_offset_mm) / 1000.0
        Rs.append(r.astype(np.int64) + (y - y0) * FINE_PX)
        Cs.append(c.astype(np.int64) + (x - x0) * FINE_PX)
        Es.append(elev)
        Ls.append(lake[comp])
        print(f"({x},{y}): bv{t.bake_ver}  river {int(river.sum()):,}  "
              f"lake {int(lake.sum()):,}  composed {int(comp.sum()):,}  "
              f"basins {len(t.basins or ())}", flush=True)

    R = np.concatenate(Rs)
    C = np.concatenate(Cs)
    E = np.concatenate(Es)
    L = np.concatenate(Ls)

    print("\nbasins " + str(n_basins) + "  "
          + "  ".join(f"{KIND_NAME[k]}={v}" for k, v in sorted(kinds.items())))
    print(f"lake area total {lake_area_m2 / 1e6:.3f} km^2")
    print(f"river cells {river_tot:,}   lake cells {lake_tot:,}   "
          f"COMPOSED cells {R.size:,}")

    lab = label8(R, C, H, W)
    n = int(lab.max()) + 1 if lab.size else 0
    rows = []
    for k in range(n):
        m = lab == k
        rows.append((max_pairwise_m(R[m], C[m]), int(m.sum()),
                     float(E[m].max()), float(E[m].min()),
                     100.0 * float(L[m].mean())))
    rows.sort(reverse=True)
    print(f"components (8-conn): {n}")
    print("  span_m      cells     high_m     low_m   lake%")
    for s, sz, hi, lo, lp in rows[:args.top]:
        print(f"  {s:9.0f}  {sz:9,}  {hi:8.1f}  {lo:8.1f}  {lp:5.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
