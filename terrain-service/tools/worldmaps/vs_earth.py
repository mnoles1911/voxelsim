"""Is our world more varied than Earth, or more monotonous? Measured, not asserted.

Compares the stitched 289-tile world against real ETOPO on the two things
"varied" can mean:
  * HYPSOMETRY -- how much land is high, i.e. are there really mountains
  * LOCAL RELIEF -- how much height change per unit distance, i.e. is it rugged
    or is it a smooth ramp

Both are computed on the SAME window size in kilometres on both sides, because
relief is meaningless without a length scale: every landscape is flat if you
look at a small enough patch and rugged if you look at a big enough one.
"""
import pathlib
import sys

import numpy as np
import rasterio

from terrain_service import tile_codec

SEED_DIR = sys.argv[1]
DS = 4
PX_M = 30.0 * DS          # 120 m/px for ours

# ---- ours ---------------------------------------------------------------
p = pathlib.Path(SEED_DIR)
files = sorted(p.glob("*.vxtl"))
xs = sorted({int(f.stem.split("_")[0]) for f in files})
ys = sorted({int(f.stem.split("_")[1]) for f in files})
n = 512 // DS
W, H = (len(xs)) * n, (len(ys)) * n
ours = np.full((H, W), np.nan, dtype=np.float32)
for f in files:
    tx, ty = (int(v) for v in f.stem.split("_"))
    t = tile_codec.decode(f.read_bytes())
    ours[(ty - ys[0]) * n:(ty - ys[0] + 1) * n,
         (tx - xs[0]) * n:(tx - xs[0] + 1) * n] = t.elevation.astype(np.float32)[::DS, ::DS]

# ---- Earth --------------------------------------------------------------
src = rasterio.open("D:/terrain-diffusion/data/global/etopo_10m.tif")
earth = src.read(1).astype(np.float32)
earth[earth < -30000] = np.nan
h = earth.shape[0]
earth = earth[h // 6: h - h // 6, :]          # +/-60 deg, same crop the conditioning uses
# 10 arc-minute at the equator is ~18.5 km/px; degrade OURS to a comparable
# grid for the relief comparison rather than pretending the pitches match.
EARTH_PX_M = 18520.0


def hypso(a, label):
    land = a[np.isfinite(a) & (a > 0)]
    print(f"  {label:<12} land px {land.size:>9}  p50 {np.percentile(land,50):6.0f}  "
          f"p95 {np.percentile(land,95):6.0f}  max {land.max():6.0f} m")
    for thr in (500, 1000, 2000, 3000, 4000):
        print(f"      above {thr:5d} m: {100.0*(land>thr).mean():6.2f}%")


def relief(a, px_m, win_km, label):
    """p50/p95 of (max-min) within a square window of win_km on a side."""
    w = max(2, int(round(win_km * 1000.0 / px_m)))
    Hh, Ww = a.shape
    ny, nx = Hh // w, Ww // w
    if ny < 2 or nx < 2:
        print(f"      {label}: window {win_km} km too big for this raster")
        return
    blocks = a[:ny * w, :nx * w].reshape(ny, w, nx, w).swapaxes(1, 2).reshape(ny * nx, w * w)
    landish = np.nanmean(blocks > 0, axis=1) > 0.8      # mostly-land blocks only
    b = blocks[landish]
    if b.size == 0:
        print(f"      {label}: no mostly-land blocks")
        return
    r = np.nanmax(b, axis=1) - np.nanmin(b, axis=1)
    r = r[np.isfinite(r)]
    print(f"      {label:<8} {win_km:>5.0f} km window (n={r.size:>6}): "
          f"p50 {np.percentile(r,50):6.0f}  p95 {np.percentile(r,95):6.0f}  max {r.max():6.0f} m")


print("=== HYPSOMETRY: how much land is high ===")
hypso(ours, "OURS")
hypso(earth, "EARTH")

print("\n=== LOCAL RELIEF at matched window sizes ===")
print("  (Earth's raster is ~18.5 km/px, so only windows well above that are meaningful)")
for km in (60.0, 120.0):
    relief(ours, PX_M, km, "OURS")
    relief(earth, EARTH_PX_M, km, "EARTH")
    print()

print("=== OURS at fine windows Earth's raster cannot resolve ===")
for km in (2.0, 10.0, 30.0):
    relief(ours, PX_M, km, "OURS")
