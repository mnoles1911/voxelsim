#!/usr/bin/env python3
"""How far does the corridor's longest wet component get toward the sea?

The fragmentation report says how long a piece is. It does not say WHERE it is,
and "a 4 km river ending 9 km inland" and "a 4 km river reaching the beach" are
different products. This measures, on the same stitched grid:

  * the corridor's sea cells (stitched elevation <= 0) and where they are;
  * for the longest components: the lowest wet pixel (the downstream end), its
    elevation, and the straight-line distance from it to the nearest sea cell.

Elevation is decimated 16x to the 30 m coarse pitch before the distance
transform -- the answer is quoted in kilometres and a 1.875 m grid over
16k x 24k px costs 1.6 GB to label for no extra significant figure.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec as tc  # noqa: E402

FINE_PX = tc.TILE_SIZE * tc.FINE_SCALE
PIXEL_M = 30.0 / tc.FINE_SCALE
DEC = tc.FINE_SCALE  # decimation to the 30 m pitch


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache-dir", default="D:/voxelsim/tile-cache")
    ap.add_argument("--seed-hex", default="000000000135276f")
    ap.add_argument("--ns", action="append", required=True)
    ap.add_argument("--tiles", required=True)
    ap.add_argument("--top", type=int, default=3)
    args = ap.parse_args()

    tiles = [tuple(int(v) for v in p.split(",")) for p in args.tiles.split()]
    xs = sorted({x for x, _ in tiles})
    ys = sorted({y for _, y in tiles})
    x0, y0 = xs[0], ys[0]
    nc, nr = xs[-1] - x0 + 1, ys[-1] - y0 + 1
    ch, cw = nr * FINE_PX // DEC, nc * FINE_PX // DEC

    from scipy import ndimage

    for spec in args.ns:
        label, ns = spec.split("=", 1)
        base = Path(args.cache_dir) / ns / args.seed_hex / "s16"
        coarse = np.full((ch, cw), np.nan, np.float32)
        wet_r: list[np.ndarray] = []
        wet_c: list[np.ndarray] = []
        wet_e: list[np.ndarray] = []
        for (x, y) in tiles:
            p = base / f"{x}_{y}.vxtl"
            if not p.exists():
                continue
            t = tc.decode_v2(p.read_bytes())
            e = (t.elevation_cp.astype(np.float64) * tc.QUANT_MM[t.quant]
                 + t.base_offset_mm) / 1000.0
            ro, co = (y - y0) * FINE_PX, (x - x0) * FINE_PX
            coarse[ro // DEC:(ro + FINE_PX) // DEC,
                   co // DEC:(co + FINE_PX) // DEC] = e[::DEC, ::DEC]
            if t.water_cp is not None:
                m = np.asarray(t.water_cp) >= 0
                r, c = np.nonzero(m)
                wet_r.append(r.astype(np.int64) + ro)
                wet_c.append(c.astype(np.int64) + co)
                wet_e.append(e[m])

        sea = np.nan_to_num(coarse, nan=1e9) <= 0.0
        print(f"--- {label} ---")
        print(f"  stitched grid {nr}x{nc} tiles, elevation "
              f"min {np.nanmin(coarse):.0f} max {np.nanmax(coarse):.0f} m")
        print(f"  sea cells (elev <= 0) at 30 m: {int(sea.sum()):,} "
              f"({100.0 * sea.mean():.2f}% of the corridor box)")
        if not sea.any():
            print("  NO SEA IN THE CORRIDOR BOX -- distances are to the box edge, "
                  "not to a coast; skipping.")
            continue
        dist_px = ndimage.distance_transform_edt(~sea)

        if not wet_r:
            print("  no wet pixels")
            continue
        R = np.concatenate(wet_r)
        C = np.concatenate(wet_c)
        E = np.concatenate(wet_e)
        # Reuse the labeller from the fragmentation tool.
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from measure_corridor_fragmentation import label8, max_pairwise_m
        lab = label8(R, C, nr * FINE_PX, nc * FINE_PX)
        comps = []
        for k in range(int(lab.max()) + 1):
            m = lab == k
            comps.append((max_pairwise_m(R[m], C[m]), k, int(m.sum())))
        comps.sort(reverse=True)

        # THE COAST QUESTION, ASKED THE OTHER WAY ROUND. "How long is the
        # longest piece" and "how close does the water get to the sea" are
        # different questions, and on this corridor the longest piece is the
        # answer to neither -- it sits 23 km inland. So: every wet pixel's
        # distance to the sea, and the nearest component regardless of size.
        d_all = dist_px[R // DEC, C // DEC] * 30.0 / 1000.0
        print(f"  wet pixel distance to sea, km:  min {d_all.min():.2f}  "
              f"p10 {np.percentile(d_all, 10):.2f}  p50 {np.percentile(d_all, 50):.2f}"
              f"  max {d_all.max():.2f}")
        for thr in (1.0, 2.0, 5.0, 10.0):
            print(f"    wet px within {thr:>4.0f} km of the sea: "
                  f"{int((d_all <= thr).sum()):>6,} "
                  f"({100.0 * float((d_all <= thr).mean()):5.2f}%)")
        near = []
        for span, k, size in comps:
            m = lab == k
            near.append((float(dist_px[R[m] // DEC, C[m] // DEC].min()),
                         span, size))
        # THE NUMBER THE OWNER ASKED FOR. Not "the longest component" and not
        # "the closest component" -- the LONGEST component that actually gets
        # to the coast. On this corridor those are three different pieces, and
        # the first two are misleading on their own.
        for reach_km in (1.0, 2.0, 5.0):
            cand = [(span, size, d) for d, span, size in near
                    if d * 0.03 <= reach_km]
            if not cand:
                print(f"  nothing within {reach_km:.0f} km of the sea")
                continue
            cand.sort(reverse=True)
            n_px = sum(s for _, s, _ in cand)
            print(f"  reaching within {reach_km:>4.0f} km of the sea: "
                  f"{len(cand):,} components, {n_px:,} px; "
                  f"longest span {cand[0][0]:.0f} m ({cand[0][1]:,} px, "
                  f"nearest point {cand[0][2] * 0.03:.2f} km out)")

        for span, k, size in comps[:args.top]:
            m = lab == k
            r, c, e = R[m], C[m], E[m]
            lo = int(np.argmin(e))
            d_km = float(dist_px[r[lo] // DEC, c[lo] // DEC]) * 30.0 / 1000.0
            hi = int(np.argmax(e))
            print(f"  span {span:8.0f} m  {size:>7,} px  "
                  f"elev {e.min():7.1f} -> {e.max():7.1f} m  "
                  f"low end at ({c[lo] * PIXEL_M / 1000:.2f}, "
                  f"{r[lo] * PIXEL_M / 1000:.2f}) km in-box, "
                  f"{d_km:.2f} km from the nearest sea cell "
                  f"(high end {float(dist_px[r[hi] // DEC, c[hi] // DEC]) * 0.03:.2f} km)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
