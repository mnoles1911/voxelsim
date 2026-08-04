#!/usr/bin/env python3
"""THE GUARDRAIL: prove a water-plane change moved no ground, on real tiles.

    python tools/verify_water_only_change.py \
        --before D:/tmp/frag-npz --after D:/tmp/riverfix-npz \
        --tiles="-11,-4 -11,-5 -12,-5 -11,-6" \
        --cache-dir D:/voxelsim/tile-cache --ns <shipped-namespace-dir>

WHY NOT JUST TRUST ``tests/test_bake_terrain_identity.py``. That test bakes a
64^2 synthetic world with test constants. It is the right unit gate and it is
not evidence about the shipped world: the production bake runs 8192^2 with an
apron, a hydrology pyramid, real climate and B5 basins, and every one of those
is a path the synthetic test does not exercise. The claim being made is about
THESE tiles, so it is checked on these tiles.

WHAT IS COMPARED, and why each one is necessary rather than decorative:

  elevation_m   float32, EXACT equality, from the two npz dumps. Unquantised,
                so it catches a perturbation smaller than the 100 mm wire LSB
                that a control-point comparison would round away. This is the
                strongest of the three.
  flow          the flow plane, uint8, exact. It carries log2 catchment area
                and the channel flag, i.e. the AREA field -- which is the thing
                a routing change would move if it had leaked out of the water
                pass. A water-only change must leave it alone.
  vs shipped    the same two planes against the SHIPPED .vxtl, so the npz dumps
                are anchored to bytes that are actually in the world rather
                than to each other. Elevation is compared at the wire's own
                quantisation, since that is all the tile carries.

Any difference is reported and exits non-zero. There is no tolerance argument:
"the ground did not move" is an equality claim or it is nothing.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec as tc  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--before", required=True, help="npz dir of the old bake")
    ap.add_argument("--after", required=True, help="npz dir of the new bake")
    ap.add_argument("--tiles", required=True)
    ap.add_argument("--cache-dir", default="D:/voxelsim/tile-cache")
    ap.add_argument("--seed-hex", default="000000000135276f")
    ap.add_argument("--ns", default=None,
                    help="shipped fine namespace dir, to anchor the npz pair "
                         "to bytes that are in the world")
    args = ap.parse_args()

    tiles = [tuple(int(v) for v in p.split(",")) for p in args.tiles.split()]
    bad = 0
    for (x, y) in tiles:
        pb = Path(args.before) / f"{x}_{y}.npz"
        pa = Path(args.after) / f"{x}_{y}.npz"
        if not (pb.exists() and pa.exists()):
            print(f"({x},{y}): MISSING npz "
                  f"(before={pb.exists()} after={pa.exists()})")
            bad += 1
            continue
        b, a = np.load(pb), np.load(pa)
        ez = np.array_equal(b["elevation_m"], a["elevation_m"])
        fz = np.array_equal(b["flow"], a["flow"])
        az = np.array_equal(b["accumulation_m2"], a["accumulation_m2"])
        note = ""
        if not ez:
            d = np.abs(b["elevation_m"].astype(np.float64)
                       - a["elevation_m"].astype(np.float64))
            note = (f"  elevation differs on {int((d > 0).sum()):,} cells, "
                    f"max {d.max():.6g} m")
        line = (f"({x},{y}): elevation {'IDENTICAL' if ez else 'MOVED'}"
                f"  flow {'IDENTICAL' if fz else 'MOVED'}"
                f"  accumulation {'IDENTICAL' if az else 'MOVED'}")

        if args.ns:
            p = (Path(args.cache_dir) / args.ns / args.seed_hex / "s16"
                 / f"{x}_{y}.vxtl")
            if p.exists():
                t = tc.decode_v2(p.read_bytes())
                q = tc.QUANT_MM[t.quant]
                # The codec's OWN operator, on the shipped tile's own datum --
                # not a re-derived formula. `elevation_control_points` runs the
                # §2 prefilter before it quantises, and a hand-rolled
                # `round((z*1000 - base)/q)` silently skips it and then reports
                # a terrain move that did not happen. (It did, once, here.)
                cp, _, _ = tc.elevation_control_points(
                    a["elevation_m"], base_offset_mm=t.base_offset_mm,
                    quant=t.quant)
                shipped_cp = np.asarray(t.elevation_cp).astype(np.int64)
                ok_e = np.array_equal(np.asarray(cp).astype(np.int64), shipped_cp)
                ok_f = np.array_equal(np.asarray(t.flow), a["flow"])
                line += (f"\n    vs shipped .vxtl: elevation_cp "
                         f"{'IDENTICAL' if ok_e else 'DIFFERS'}"
                         f"  flow plane {'IDENTICAL' if ok_f else 'DIFFERS'}"
                         f"  (bv{t.bake_ver}, quant {q} mm)")
                if not (ok_e and ok_f):
                    bad += 1
            else:
                line += f"\n    vs shipped .vxtl: NOT FOUND at {p}"
        print(line + note)
        if not (ez and fz and az):
            bad += 1
    print(f"\n{'FAIL' if bad else 'PASS'}: {len(tiles)} tile(s), {bad} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
