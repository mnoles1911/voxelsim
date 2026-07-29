"""Hillshade PNGs of the saved 1089^2 S1 windows: python hillshade.py CONFIG_TILE ..."""
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

HERE = Path(__file__).resolve().parent
WIN = HERE / "ridge_out" / "windows"
PNG = HERE / "ridge_out" / "png"
PNG.mkdir(parents=True, exist_ok=True)
CELL = 1.875


def shade(z, az_deg=315.0, alt_deg=45.0):
    gy, gx = np.gradient(z, CELL)
    az = np.radians(az_deg)
    alt = np.radians(alt_deg)
    sx, sy = np.sin(az), np.cos(az)
    denom = np.sqrt(1 + gx * gx + gy * gy)
    return np.clip((np.sin(alt) + np.cos(alt) * (gx * sx + gy * sy)) / denom, 0, 1)


for name in sys.argv[1:]:
    p = WIN / f"{name}_S1.npy"
    if not p.exists():
        p = WIN / f"{name}.npy"
    z = np.load(p).astype(np.float64)
    fig, ax = plt.subplots(figsize=(9, 9), dpi=160)
    ax.imshow(shade(z), cmap="gray", vmin=0, vmax=1)
    ax.set_title(f"{name}  ({z.shape[0]}px @ {CELL} m, 2.04 km)")
    ax.axis("off")
    fig.tight_layout()
    out = PNG / f"{name}.png"
    fig.savefig(out)
    plt.close(fig)
    print(out)
