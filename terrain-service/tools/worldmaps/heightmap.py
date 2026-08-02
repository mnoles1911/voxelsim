"""Stitch every coarse tile into ONE elevation map: flat or mountainous?

Renders the raw 30 m elevation plane from the .vxtl tiles -- no client detail
terms, no voxelisation, no LOD, no biome colouring. What you see is what the
diffusion model produced.

Hypsometric tint + hillshade, because either alone lies: a tint with no relief
shading looks flat wherever the palette is smooth, and a hillshade with no tint
cannot tell a 200 m hill from a 6 km massif.
"""
import pathlib
import sys

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LightSource, LinearSegmentedColormap

from terrain_service import tile_codec

SEED_DIR = sys.argv[1]
OUT = sys.argv[2]
DS = int(sys.argv[3]) if len(sys.argv) > 3 else 4      # downsample factor
PX_M = 30.0 * DS

p = pathlib.Path(SEED_DIR)
files = sorted(p.glob("*.vxtl"))
xs = sorted({int(f.stem.split("_")[0]) for f in files})
ys = sorted({int(f.stem.split("_")[1]) for f in files})
x0, x1, y0, y1 = xs[0], xs[-1], ys[0], ys[-1]
n = 512 // DS
W, H = (x1 - x0 + 1) * n, (y1 - y0 + 1) * n
grid = np.full((H, W), np.nan, dtype=np.float32)

for f in files:
    tx, ty = (int(v) for v in f.stem.split("_"))
    t = tile_codec.decode(f.read_bytes())
    e = t.elevation.astype(np.float32)[::DS, ::DS]
    grid[(ty - y0) * n:(ty - y0 + 1) * n, (tx - x0) * n:(tx - x0 + 1) * n] = e

land = grid[grid > 0]
print(f"{len(files)} tiles -> {W}x{H} px @ {PX_M:.0f} m/px = {W*PX_M/1000:.0f} km across")
print(f"elevation  min {np.nanmin(grid):.0f}  max {np.nanmax(grid):.0f} m")
print(f"land only  p50 {np.percentile(land,50):.0f}  p95 {np.percentile(land,95):.0f}  max {land.max():.0f} m")
for thr in (1000, 2000, 3000, 4000, 5000):
    print(f"  land above {thr:5d} m: {100.0*(land>thr).mean():6.2f}%")

# Hypsometric palette: ocean blues below 0, then green -> tan -> brown -> white.
vmin, vmax = float(np.nanmin(grid)), float(np.nanmax(grid))
span = vmax - vmin
def stop(v):  # elevation -> 0..1 position in the colormap
    return (v - vmin) / span
cmap = LinearSegmentedColormap.from_list("hypso", [
    (0.0,            "#08214a"),
    (stop(-2000),    "#12508c"),
    (stop(-50),      "#3f8fc4"),
    (stop(0.1),      "#2f6b34"),
    (stop(400),      "#6f9a45"),
    (stop(1000),     "#b9a765"),
    (stop(2000),     "#9c7248"),
    (stop(3500),     "#7a5a48"),
    (stop(5000),     "#cfc6bf"),
    (1.0,            "#ffffff"),
])

filled = np.where(np.isnan(grid), vmin, grid)
ls = LightSource(azdeg=292.5, altdeg=45)
rgb = ls.shade(filled, cmap=cmap, vmin=vmin, vmax=vmax, blend_mode="soft",
               vert_exag=3.0, dx=PX_M, dy=PX_M)
rgb[np.isnan(grid)] = 1.0   # absent tiles -> white

fig_w = 13.0
fig = plt.figure(figsize=(fig_w, fig_w * H / W + 0.9), dpi=150)
ax = fig.add_axes([0.0, 0.06, 1.0, 0.94])
ax.imshow(rgb, origin="upper", interpolation="nearest")
ax.set_axis_off()

km = W * PX_M / 1000.0
bar_km = 50.0
bar_px = bar_km * 1000.0 / PX_M
ax.plot([W*0.03, W*0.03 + bar_px], [H*0.96, H*0.96], color="k", lw=3, solid_capstyle="butt")
ax.text(W*0.03 + bar_px/2, H*0.945, f"{bar_km:.0f} km", ha="center", va="bottom",
        fontsize=11, color="k", weight="bold")
fig.text(0.5, 0.015,
         f"seed 20260719  |  {len(files)} coarse tiles  |  {km:.0f} x {H*PX_M/1000:.0f} km  "
         f"@ {PX_M:.0f} m/px  |  elevation {vmin:.0f} to {vmax:.0f} m  |  "
         f"land p50 {np.percentile(land,50):.0f} m, p95 {np.percentile(land,95):.0f} m  |  "
         f"hillshade az 292.5 alt 45, vert exag 3x",
         ha="center", fontsize=9)
fig.savefig(OUT, dpi=150, facecolor="white")
print(f"wrote {OUT}")
