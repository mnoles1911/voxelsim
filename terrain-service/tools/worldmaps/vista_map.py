"""Where every vista screenshot was taken, plotted on the world.

A coordinate in a table is a thing you trust; a pin on a map is a thing you can
navigate by. This renders the same hillshade as 01-heightmap and drops a
numbered pin at each vista site, so a developer deciding where to walk around
can see the shot in its landform context -- that this desert is a coastal
basin, that this tundra is a 3.5 km massif -- before flying out to it.

Reads the JSON written by vista_sites.py so the pins cannot drift from the
labels. Run vista_sites.py first.

Usage:
    $PY vista_map.py <seed_dir> <vista-sites.json> <out.png>
"""
import json
import pathlib
import sys

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LightSource, LinearSegmentedColormap

from terrain_service import tile_codec

SEED_DIR = pathlib.Path(sys.argv[1])
SITES = json.load(open(sys.argv[2], encoding="utf-8"))
OUT = sys.argv[3]

DS = 4
PX_M = 30.0 * DS
TILE_M = 512 * 30.0

files = sorted(SEED_DIR.glob("*.vxtl"))
txs = sorted({int(f.stem.split("_")[0]) for f in files})
tys = sorted({int(f.stem.split("_")[1]) for f in files})
n = 512 // DS
W, H = len(txs) * n, len(tys) * n
elev = np.full((H, W), np.nan, np.float32)
for f in files:
    tx, ty = (int(v) for v in f.stem.split("_"))
    t = tile_codec.decode(f.read_bytes())
    elev[(ty - tys[0]) * n:(ty - tys[0] + 1) * n,
         (tx - txs[0]) * n:(tx - txs[0] + 1) * n] = t.elevation.astype(np.float32)[::DS, ::DS]

# world metres -> pixel in this image
X0, Y0 = txs[0] * TILE_M, tys[0] * TILE_M


def to_px(x_m, y_m):
    return (x_m - X0) / PX_M, (y_m - Y0) / PX_M


land = np.isfinite(elev) & (elev > 0)
ls = LightSource(azdeg=292.5, altdeg=45)
shade = ls.hillshade(np.nan_to_num(elev, nan=float(np.nanmin(elev))),
                     vert_exag=3.0, dx=PX_M, dy=PX_M)
cmap = LinearSegmentedColormap.from_list("hyps", [
    "#2e6b45", "#7fa25a", "#c8bd7a", "#a9793f", "#8a5a3a", "#b9b0aa", "#ffffff"])
hi = float(np.nanpercentile(elev[land], 99.5)) if land.any() else 1.0
rgb = cmap(np.clip(np.nan_to_num(elev) / max(hi, 1.0), 0, 1))[..., :3]
rgb[~land] = np.array([0.09, 0.15, 0.26])
rgb = rgb * (0.42 + 0.58 * shade[..., None])

fig, ax = plt.subplots(figsize=(15, 15.6), dpi=140)
ax.imshow(rgb, origin="upper", interpolation="nearest")
ax.set_axis_off()

labels = []
for i, s in enumerate(SITES, 1):
    px, py = to_px(s["x_m"], s["y_m"])
    ax.plot(px, py, marker="o", ms=15, mfc="none", mec="black", mew=3.0, zorder=5)
    ax.plot(px, py, marker="o", ms=15, mfc="none", mec="#ffe14d", mew=1.6, zorder=6)
    ax.text(px, py, str(i), color="#ffe14d", fontsize=9, fontweight="bold",
            ha="center", va="center", zorder=7,
            path_effects=None)
    labels.append(f"{i}.  {s['biome']} / {s['prov_top'].upper()}   "
                  f"tile ({s['tx']},{s['ty']})   -VoxelSpawnAt '{s['x_m']:.0f},{s['y_m']:.0f}'   "
                  f"{s['elev']:.0f} m")

ax.set_title("VISTA SCREENSHOT SITES   |   seed 20260719   |   "
             f"261 x 261 km @ {PX_M:.0f} m/px   |   all shots 1200 m AGL, pitch -20 deg",
             fontsize=13)
ax.text(0.005, -0.005, "\n".join(labels), transform=ax.transAxes, fontsize=9.5,
        va="top", ha="left", family="monospace",
        bbox=dict(facecolor="white", alpha=0.93, edgecolor="#999", pad=8))
fig.tight_layout()
fig.savefig(OUT, dpi=140, facecolor="white", bbox_inches="tight")
print("wrote", OUT)
