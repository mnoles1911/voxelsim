#!/usr/bin/env python3
"""THE GUARDRAIL: prove a water-plane change moved no ground, on real tiles.

    python tools/verify_water_only_change.py \
        --before D:/tmp/frag-npz --after D:/tmp/riverfix-npz \
        --tiles="-11,-4 -11,-5 -12,-5 -11,-6" \
        --cache-dir D:/voxelsim/tile-cache --ns <shipped-namespace-dir>

    # or, when the new bake ran WITHOUT --diagnostic (and so wrote .vxtl
    # rather than npz), which is the only bake whose output is loadable:
    python tools/verify_water_only_change.py \
        --tiles="-11,-4 -11,-5 -12,-5 -11,-6" \
        --ns-before terrain-diffusion-...-ba9c62170 \
        --ns-after  terrain-diffusion-...-b10cf6d2c

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
    ap.add_argument("--before", default=None, help="npz dir of the old bake")
    ap.add_argument("--after", default=None, help="npz dir of the new bake")
    ap.add_argument("--tiles", required=True)
    ap.add_argument("--cache-dir", default="D:/voxelsim/tile-cache")
    ap.add_argument("--seed-hex", default="000000000135276f")
    ap.add_argument("--ns", default=None,
                    help="shipped fine namespace dir, to anchor the npz pair "
                         "to bytes that are in the world")
    ap.add_argument("--ns-before", default=None,
                    help="SHIPPED-vs-SHIPPED mode: the old namespace dir. Pair "
                         "with --ns-after. Use this when the new bake ran "
                         "WITHOUT --diagnostic, which is the only bake whose "
                         "output is loadable and therefore the only one worth "
                         "making the identity claim about.")
    ap.add_argument("--ns-after", default=None)
    args = ap.parse_args()

    tiles = [tuple(int(v) for v in p.split(",")) for p in args.tiles.split()]
    bad = 0

    # ---- SHIPPED vs SHIPPED. Two .vxtl namespaces, the codec's own planes,
    # exact equality. This is weaker than the npz comparison by exactly one
    # thing -- it reads the ground at the wire's quantisation rather than
    # unquantised -- and stronger by exactly one thing: both sides are bytes
    # that a client can load, so there is no dump in the argument at all. The
    # final bake must run without --diagnostic to be loadable, and a diagnostic
    # run writes no .vxtl, so a change cannot be verified both ways at once and
    # this is the mode that matches what ships.
    if args.ns_before or args.ns_after:
        if not (args.ns_before and args.ns_after):
            ap.error("--ns-before and --ns-after go together")
        for (x, y) in tiles:
            def _load(ns):
                p = (Path(args.cache_dir) / ns / args.seed_hex / "s16"
                     / f"{x}_{y}.vxtl")
                return tc.decode_v2(p.read_bytes()) if p.exists() else None
            tb, ta = _load(args.ns_before), _load(args.ns_after)
            if tb is None or ta is None:
                print(f"({x},{y}): MISSING .vxtl "
                      f"(before={tb is not None} after={ta is not None})")
                bad += 1
                continue
            # The DATUM has to match too, or "identical control points" would
            # be comparing two different origins and could pass while the
            # ground moved by exactly the datum difference.
            ok_d = (tb.base_offset_mm == ta.base_offset_mm
                    and tb.quant == ta.quant)
            ok_e = np.array_equal(np.asarray(tb.elevation_cp),
                                  np.asarray(ta.elevation_cp))
            ok_f = np.array_equal(np.asarray(tb.flow), np.asarray(ta.flow))
            wb = int((np.asarray(tb.water_cp) >= 0).sum()) if tb.water_cp is not None else 0
            wa = int((np.asarray(ta.water_cp) >= 0).sum()) if ta.water_cp is not None else 0
            print(f"({x},{y}): bv{tb.bake_ver} -> bv{ta.bake_ver}  "
                  f"datum {'SAME' if ok_d else 'DIFFERS'}  "
                  f"elevation_cp {'IDENTICAL' if ok_e else 'MOVED'}  "
                  f"flow {'IDENTICAL' if ok_f else 'MOVED'}  "
                  f"wet {wb:,} -> {wa:,} ({100.0 * (wa - wb) / max(wb, 1):+.2f}%)")
            if not (ok_d and ok_e and ok_f):
                bad += 1
        print(f"\n{'FAIL' if bad else 'PASS'}: {len(tiles)} tile(s), "
              f"{bad} problem(s)")
        return 1 if bad else 0

    if not (args.before and args.after):
        ap.error("give either --before/--after (npz) or --ns-before/--ns-after")
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
