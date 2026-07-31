#!/usr/bin/env python3
"""Measure the PERIOD and the CAUSE of the parallel banding, from the continuous surface.

WHY THIS EXISTS. Twenty-plus attempts have been made to remove the banding, none of
which first measured the artifact's own spacing. Three candidate causes predict
different, non-overlapping spacings, so one measurement discriminates between them:

  quantisation  a smooth ramp cut into 100 mm voxels steps every  0.1/slope  metres,
                so the spacing is SLOPE-DEPENDENT and sub-metre on steep ground.
  carrier       the fine control lattice is 1.875 m. If the continuous surface itself
                carries power at 1.875 m (or 3.75 / 7.5 m) the bands are printed by the
                carrier and are INDEPENDENT of slope -- and every detail-amplitude fix
                to date was aimed at the wrong layer.
  LOD           coarsened voxels at distance step every 0.4-1.6 m; absent in the near
                field, and not visible in this dump at all (this is an L0 surface).

It also tests the structural claim that matters most, independent of period:

  MONOTONICITY. The v14/v15 drainage cap holds |grad detail| <= 0.5 |grad carrier|, so
  the surface cannot overturn its own fall direction. A strictly monotone surface
  crosses each contour level EXACTLY ONCE along every fall line, which is the
  definition of an unbroken contour band. Fragmenting a contour requires local
  counter-slope -- ground that genuinely rises against the slope. Real mountainsides
  have plenty (perched blocks, tussocks, small scarps); if ours has ~none, the cap is
  manufacturing the artifact by construction and no amount of added amplitude can help
  while it holds.

INPUT is the VXC_PROBE_DUMP raster: n*n int32 surface heights in mm, row-major, one
voxel (100 mm) apart, CONTINUOUS (pre-voxelisation) -- so quantisation effects can be
applied here deliberately rather than being baked into the input.

Usage:
  python tools/band_period.py --bin D:\\ue-cache\\band-g35.bin [--n 512]
"""
import argparse

import numpy as np

VOXEL_MM = 100.0
CELL_M = 0.1


def counter_slope(h_m: np.ndarray, carrier_win: int) -> tuple[float, float]:
    """Fraction of the surface that RISES along its own local descent direction.

    The descent direction is taken from a heavily smoothed copy (the landform fall
    line, not the local wiggle), and the derivative is taken at one cell. Returns
    (fraction rising, mean rise magnitude in mm per cell over the rising set).
    """
    k = np.ones(carrier_win) / carrier_win
    sm = np.apply_along_axis(lambda r: np.convolve(r, k, "same"), 0, h_m)
    sm = np.apply_along_axis(lambda r: np.convolve(r, k, "same"), 1, sm)
    gy_c, gx_c = np.gradient(sm, CELL_M)
    gy_r, gx_r = np.gradient(h_m, CELL_M)
    mag = np.hypot(gx_c, gy_c)
    ok = mag > 0.05  # only where there IS a fall line (>5% grade)
    ux, uy = gx_c / np.maximum(mag, 1e-9), gy_c / np.maximum(mag, 1e-9)
    # Directional derivative along DESCENT (-u). Positive => the ground rises as you
    # walk downhill: a counter-slope, the thing that breaks a contour.
    d = -(gx_r * ux + gy_r * uy)
    inner = (slice(2, -2), slice(2, -2))
    d, ok = d[inner], ok[inner]
    rising = (d > 0) & ok
    frac = rising.sum() / max(ok.sum(), 1)
    amp = (d[rising].mean() * CELL_M * 1000.0) if rising.any() else 0.0
    return float(frac), float(amp)


def spectrum_peaks(h_m: np.ndarray, hp_m: float = 12.0) -> list[tuple[float, float]]:
    """Dominant wavelengths of the high-passed continuous surface, in metres.

    High-pass first: without it the landform ramp dominates every bin and the peak
    search just returns the window size (a trap this project has hit three times).
    """
    w = int(round(hp_m / CELL_M))
    k = np.ones(w) / w
    lo = np.apply_along_axis(lambda r: np.convolve(r, k, "same"), 0, h_m)
    lo = np.apply_along_axis(lambda r: np.convolve(r, k, "same"), 1, lo)
    hp = h_m - lo
    inner = hp[w:-w, w:-w]
    n = inner.shape[1]
    win = np.hanning(n)
    p = np.zeros(n // 2)
    for row in inner:
        p += np.abs(np.fft.rfft((row - row.mean()) * win))[: n // 2] ** 2
    for col in inner.T:
        p += np.abs(np.fft.rfft((col - col.mean()) * win))[: n // 2] ** 2
    freqs = np.fft.rfftfreq(n, d=CELL_M)[: n // 2]
    lam = np.divide(1.0, freqs, out=np.full_like(freqs, np.inf), where=freqs > 0)
    # Only wavelengths the high-pass actually passes, and above 2 cells (Nyquist).
    # Stop WELL short of hp_m: the filter's own shoulder is the loudest thing in the
    # band and a naive argmax just returns it (this project has hit that trap three
    # times, and this function hit it once more before the limit went to hp_m/3).
    band = (lam > 0.2) & (lam < hp_m / 3.0)
    idx = np.argsort(p[band])[::-1][:5]
    return [(float(lam[band][i]), float(p[band][i] / p[band].max())) for i in idx]


def level_runs(h_mm: np.ndarray, slope: np.ndarray) -> dict:
    """Observed band spacing vs the spacing pure quantisation predicts.

    Along each column, a 'run' is a maximal stretch at one voxel level; its length IS
    the visible band width. Pure quantisation of a smooth ramp gives 0.1/slope.
    """
    lvl = np.floor_divide(h_mm, VOXEL_MM).astype(np.int64)
    obs, pred = [], []
    for i in range(0, lvl.shape[1], 4):
        col, s = lvl[:, i], slope[:, i]
        edges = np.flatnonzero(np.diff(col) != 0)
        if len(edges) < 3:
            continue
        runs = np.diff(edges) * CELL_M
        mids = ((edges[:-1] + edges[1:]) // 2).astype(int)
        sm = s[mids]
        good = (sm > 0.05) & (runs > 0)
        obs.extend(runs[good])
        pred.extend(CELL_M / sm[good])
    obs, pred = np.asarray(obs), np.asarray(pred)
    return {
        "n": len(obs),
        "obs_median_m": float(np.median(obs)) if len(obs) else float("nan"),
        "pred_median_m": float(np.median(pred)) if len(pred) else float("nan"),
        "ratio_median": float(np.median(obs / pred)) if len(obs) else float("nan"),
        "obs_p90_m": float(np.percentile(obs, 90)) if len(obs) else float("nan"),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--n", type=int, default=512)
    a = ap.parse_args()

    h_mm = np.fromfile(a.bin, dtype="<i4", count=a.n * a.n).reshape(a.n, a.n).astype(np.float64)
    h_m = h_mm / 1000.0
    gy, gx = np.gradient(h_m, CELL_M)
    slope = np.hypot(gx, gy)

    print(f"window {a.n * CELL_M:.1f} m square at {CELL_M * 100:.0f} cm, "
          f"relief {(h_m.max() - h_m.min()):.1f} m, median grade {np.median(slope) * 100:.0f}%")

    print("\n--- TEST 1: band spacing vs the pure-quantisation prediction ---")
    r = level_runs(h_mm, slope)
    print(f"  runs measured        {r['n']}")
    print(f"  observed  median     {r['obs_median_m']:.2f} m   (p90 {r['obs_p90_m']:.2f} m)")
    print(f"  quantisation predicts{r['pred_median_m']:8.2f} m")
    print(f"  ratio observed/pred  {r['ratio_median']:.2f}"
          f"   (~1 = plain quantisation of a smooth surface; >>1 = real terraces)")

    print("\n--- TEST 2: periodic structure in the CONTINUOUS surface ---")
    for lam, rel in spectrum_peaks(h_m):
        tag = ""
        for name, L in (("fine lattice 1.875 m", 1.875), ("half-lattice 0.9375 m", 0.9375),
                        ("3.75 m", 3.75)):
            if abs(lam - L) / L < 0.12:
                tag = f"  <== matches {name}"
        print(f"  lambda {lam:6.2f} m   relative power {rel:.2f}{tag}")

    print("\n--- TEST 2b: band spacing BY SLOPE (the 0.1/slope law, and where it bites) ---")
    lvl = np.floor_divide(h_mm, VOXEL_MM).astype(np.int64)
    for lo, hi in ((0.03, 0.08), (0.08, 0.15), (0.15, 0.30), (0.30, 0.60), (0.60, 1.20)):
        runs = []
        for i in range(0, lvl.shape[1], 2):
            col, s = lvl[:, i], slope[:, i]
            e = np.flatnonzero(np.diff(col) != 0)
            if len(e) < 3:
                continue
            r, mids = np.diff(e) * CELL_M, ((e[:-1] + e[1:]) // 2).astype(int)
            sel = (s[mids] >= lo) & (s[mids] < hi)
            runs.extend(r[sel])
        if len(runs) < 50:
            print(f"  grade {lo * 100:3.0f}-{hi * 100:3.0f}%  too few samples here")
            continue
        print(f"  grade {lo * 100:3.0f}-{hi * 100:3.0f}%   observed band width "
              f"{np.median(runs):5.2f} m   law predicts {CELL_M / ((lo + hi) / 2):5.2f} m   "
              f"n={len(runs)}")
    print("  NB shallow ground gives WIDE bands: at 10% grade one voxel of rise takes a")
    print("  metre of run, which is exactly the metre-scale banding seen in game.")

    print("\n--- TEST 3: counter-slope (can a contour break at all?) ---")
    for win_m in (2.0, 5.0, 15.0):
        f, amp = counter_slope(h_m, int(round(win_m / CELL_M)))
        print(f"  fall line from a {win_m:4.1f} m carrier: {f * 100:5.1f}% of ground rises "
              f"going downhill, mean rise {amp:5.1f} mm/cell")
    print("  real hillslopes run tens of percent; near zero means the drainage cap has")
    print("  made the surface monotone, and monotone surfaces cannot break a contour.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
