"""Are the g10 ridge cells interfluves or painted on?

For each CONFIG_TILE with saved S1 + acc windows: classify geomorphons at the
10-cell lookout, take the channel network (acc >= A), and compare the
distance-to-nearest-channel distribution of ridge+peak cells against all cells.

  ratio = median(dist | ridge) / median(dist | all)

Interfluve ridges sit at local drainage-distance maxima -> ratio well above 1.
Ridges painted on by noise are placed independently of the network -> ratio ~1.

Usage: python ridge_drainage.py CONFIG_TILE [A_m2] ...
"""
import sys
from pathlib import Path

import numpy as np
from scipy.ndimage import distance_transform_edt

HERE = Path(__file__).resolve().parent
WT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(WT))
from terrain_service.geomorph.geomorphon import geomorphon_field  # noqa: E402

WIN = HERE / "ridge_out" / "windows"
CELL = 1.875

names = [a for a in sys.argv[1:] if not a.replace(".", "").isdigit()]
areas = [float(a) for a in sys.argv[1:] if a.replace(".", "").isdigit()] or [1e4]

print(f"{'case':24s} {'A_m2':>8s} {'chan%':>6s} {'ridge%':>7s} "
      f"{'d_med_all':>9s} {'d_med_ridge':>11s} {'ratio':>6s} {'d_med_vall':>10s}")
for name in names:
    z = np.load(WIN / f"{name}_S1.npy").astype(np.float64)
    acc = np.load(WIN / f"{name}_acc.npy").astype(np.float64)
    forms = geomorphon_field(z, CELL, search_m=10 * CELL)
    ridge = (forms == 2) | (forms == 3)
    valley = (forms == 9) | (forms == 10)
    ok = forms > 0
    for A in areas:
        chan = acc >= A
        if not chan.any():
            print(f"{name:24s} {A:8.0f}  no channels")
            continue
        d = distance_transform_edt(~chan) * CELL
        d_all = np.median(d[ok])
        d_r = np.median(d[ridge & ok]) if (ridge & ok).any() else float("nan")
        d_v = np.median(d[valley & ok]) if (valley & ok).any() else float("nan")
        print(f"{name:24s} {A:8.0f} {chan.mean()*100:5.1f}% {ridge[ok].mean()*100:6.2f}% "
              f"{d_all:9.1f} {d_r:11.1f} {d_r/d_all:6.2f} {d_v:10.1f}")
