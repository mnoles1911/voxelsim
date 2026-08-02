"""Verify the orographic rain shadow: seam-free, terrain-caused, and widening.

WHY THIS IS A TOOL AND NOT A TEST. terrain-service's pytest environment is
deliberately torch-free (see test_diffusion.py::test_world_shape_defaults_are_pinned),
so ``terrain_diffusion`` cannot be imported there. This follows the precedent
set by ``bake_seam_check.py``: a standalone checker with a real exit code, run
against the terrain-diffusion venv.

    cd D:/terrain-diffusion
    PYTHONPATH=D:/terrain-diffusion ./.venv/Scripts/python.exe \
        D:/voxelsim/terrain-service/tools/orographic_check.py

(CWD must be the terrain-diffusion repo: ``STATS_CACHE_PATH`` and the WorldClim
rasters are opened at paths relative to the process CWD.)

THE PROPERTY THAT MATTERS MOST IS THE FIRST ONE. ``finalize_synthetic_map``
runs per 64x64 coarse-cell window with NO HALO (world_pipeline.py:924), so any
neighbourhood operation on the window's elevation array yields a different
answer near an edge than in the interior, and tile borders print into the
climate. The implementation avoids this by re-sampling the elevation noise at
absolute upwind world coordinates rather than marching across the array. That
is a claim about the code's structure; this checks it as a fact.
"""
import argparse
import sys

import numpy as np

try:
    from terrain_diffusion.inference.synthetic_map import make_synthetic_map_factory
except ImportError:  # pragma: no cover - environment guard
    sys.exit(
        "terrain_diffusion is not importable. It is a namespace package with no "
        "__file__ and resolves only from its own repo root -- run with "
        "PYTHONPATH=<terrain-diffusion> and CWD=<terrain-diffusion>."
    )

FREQ = [1.5, 3.0, 3.0, 3.0, 3.0]
DROP = 0.5


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seed", type=int, default=20260719)
    ap.add_argument("--window", type=int, default=256, help="analysis window in coarse cells")
    ap.add_argument("--min-corr", type=float, default=-0.5,
                    help="fail if corr(barrier, multiplier) is not at least this negative")
    args = ap.parse_args()

    off = make_synthetic_map_factory(frequency_mult=FREQ, seed=args.seed,
                                     drop_water_pct=DROP, orographic=None)
    on = make_synthetic_map_factory(frequency_mult=FREQ, seed=args.seed,
                                    drop_water_pct=DROP, orographic={})
    failures = []

    # --- 1. seam-freeness ------------------------------------------------
    # The same 64x64 region, asked for standalone and as an inset of a larger
    # window. Any halo-dependent term differs at the sub-window's edges.
    print("=== 1. SEAM TEST ===")
    for label, (big_box, sub_box) in {
        "origin": ((0, 0, 256, 256), (64, 64, 128, 128)),
        "negative coords": ((-192, -192, 64, 64), (-128, -128, -64, -64)),
    }.items():
        big = on(*big_box).numpy()
        sub = on(*sub_box).numpy()
        i0 = sub_box[0] - big_box[0]
        j0 = sub_box[1] - big_box[1]
        n = sub_box[2] - sub_box[0]
        err = float(np.abs(big[:, i0:i0 + n, j0:j0 + n] - sub).max())
        ok = err == 0.0
        print(f"  {label:<16} max|diff| = {err:.6e}   {'PASS' if ok else 'FAIL'}")
        if not ok:
            failures.append(f"seam ({label}): max|diff| = {err:.6e}, must be exactly 0")

    # --- 2. is the shadow caused by terrain? -----------------------------
    W = args.window
    raw = on.sample_raw(0, 0, W, W)
    elev = np.asarray(raw[0], dtype=np.float64)
    p_off = off.finalize(raw.copy())[3].astype(np.float64)
    p_on = on.finalize(raw.copy(), window=(0, 0, W, W))[3].astype(np.float64)
    land = elev > 0
    ratio = np.where(p_off > 1e-6, p_on / np.maximum(p_off, 1e-6), 1.0)

    # Independent reconstruction of the upwind barrier, so this checks the
    # implementation rather than restating it. Default wind_from_deg=270 gives
    # u_i=0, u_j=-1, so the probe shifts j by -d. MEASURED, not assumed:
    # shifting j moves array AXIS 0 (shifting i moves axis 1), because
    # sample_synthetic_map pairs meshgrid(x, y) with reshape(i2-i1, j2-j1) and
    # silently transposes for square windows.
    L = 1.0 / (0.05 * FREQ[0])
    fracs = (0.15, 0.30, 0.60, 1.20)
    barrier = np.zeros_like(elev)
    for frac in fracs:
        barrier = np.maximum(barrier, np.roll(elev, int(round(frac * L)), axis=0) - elev)
    valid = np.zeros_like(elev, dtype=bool)
    valid[int(round(max(fracs) * L)):, :] = True   # drop the roll-wrapped margin
    m = land & valid

    print("\n=== 2. CAUSED BY TERRAIN? ===")
    print(f"  land cells analysed: {int(m.sum())} of {elev.size}")
    if m.sum() < 100:
        failures.append("too few land cells to judge causality; raise --window")
    else:
        r = float(np.corrcoef(barrier[m], ratio[m])[0, 1])
        ok = r <= args.min_corr
        print(f"  corr(upwind barrier, precip multiplier) = {r:+.3f}   "
              f"{'PASS' if ok else 'FAIL'}  (need <= {args.min_corr:+.2f})")
        if not ok:
            failures.append(f"corr(barrier, multiplier) = {r:+.3f}, need <= {args.min_corr:+.2f}")
        hi, lo = barrier[m] > 600, barrier[m] < 50
        if hi.any() and lo.any():
            print(f"  mean multiplier behind a >600 m barrier : {ratio[m][hi].mean():.3f}  (n={int(hi.sum())})")
            print(f"  mean multiplier with a  <50 m barrier   : {ratio[m][lo].mean():.3f}  (n={int(lo.sum())})")

    # --- 3. does the spread widen? ---------------------------------------
    # NOTE: these are SKETCH statistics, measured before the coarse model runs.
    # They are NOT comparable to the "precipitation spans 7.8% of encodable
    # range" figure, which was measured on DELIVERED TILES -- the model
    # compresses. Comparing the two would be the same error class as reading a
    # heightfield probe and calling it a placed-voxel result.
    print("\n=== 3. PRECIPITATION SPREAD OVER LAND (sketch, pre-model) ===")
    for label, p in (("oro OFF", p_off), ("oro ON", p_on)):
        q = np.percentile(p[land], [5, 25, 50, 75, 95])
        print(f"  {label:<8} p5 {q[0]:7.1f}  p25 {q[1]:7.1f}  p50 {q[2]:7.1f}  "
              f"p75 {q[3]:7.1f}  p95 {q[4]:7.1f}   IQR {q[3]-q[1]:7.1f}")
    print(f"  land under  400 mm/yr:  OFF {100*(p_off[land] < 400).mean():5.2f}%   "
          f"ON {100*(p_on[land] < 400).mean():5.2f}%")
    print(f"  land over  2000 mm/yr:  OFF {100*(p_off[land] > 2000).mean():5.2f}%   "
          f"ON {100*(p_on[land] > 2000).mean():5.2f}%")

    print()
    if failures:
        for f in failures:
            print(f"FAIL: {f}")
        return 1
    print("All orographic checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
