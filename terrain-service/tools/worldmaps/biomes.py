"""Biome + province regions over the whole world, colour-coded on the hillshade.

Biomes come from world_map.classify(), which parses biome.h at run time so this
cannot drift from the client. Provinces come from the same discriminant the
bake uses, evaluated on the stitched coarse grid.
"""
import pathlib
import sys

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LightSource, ListedColormap
from matplotlib.patches import Patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from world_map import _read_constants, classify, BIOMES          # noqa: E402
from terrain_service import tile_codec                            # noqa: E402

SEED_DIR, OUT = sys.argv[1], sys.argv[2]
DS = 4
PX_M = 30.0 * DS
K = _read_constants()

p = pathlib.Path(SEED_DIR)
files = sorted(p.glob("*.vxtl"))
xs = sorted({int(f.stem.split("_")[0]) for f in files})
ys = sorted({int(f.stem.split("_")[1]) for f in files})
n = 512 // DS
W, H = len(xs) * n, len(ys) * n
elev = np.full((H, W), np.nan, np.float32)
temp = np.zeros((H, W), np.float32)
seas = np.zeros((H, W), np.float32)
prec = np.zeros((H, W), np.float32)
for f in files:
    tx, ty = (int(v) for v in f.stem.split("_"))
    t = tile_codec.decode(f.read_bytes())
    sl = (slice((ty-ys[0])*n, (ty-ys[0]+1)*n), slice((tx-xs[0])*n, (tx-xs[0]+1)*n))
    elev[sl] = t.elevation.astype(np.float32)[::DS, ::DS]
    temp[sl] = t.climate[0][::DS, ::DS] / 255.0 * 80.0 - 40.0
    seas[sl] = t.climate[1][::DS, ::DS] / 255.0 * 3000.0
    prec[sl] = t.climate[2][::DS, ::DS] / 255.0 * 12000.0

biome = classify(np.nan_to_num(elev), temp, prec, seas, K)

# --- province: same discriminant shape the bake uses -----------------------
# relief over a landform-scale window, plus temperature and aridity.
from scipy.ndimage import uniform_filter, maximum_filter, minimum_filter
win = max(3, int(round(5000.0 / PX_M)))          # ~5 km landform window
e0 = np.nan_to_num(elev)
relief = maximum_filter(e0, win) - minimum_filter(e0, win)
t_s = uniform_filter(temp, win)
p_s = uniform_filter(prec, win)
land = elev > 0
prov = np.zeros_like(biome)                       # 0 = ocean
prov[land & (t_s < 2.0) & (relief > 500)] = 1     # GLACIAL: cold + steep
prov[land & (prov == 0) & (p_s < 500)] = 2        # ARID
prov[land & (prov == 0) & (relief < 150)] = 3     # LOWLAND
prov[land & (prov == 0)] = 4                      # FLUVIAL
PROV_NAMES = ["OCEAN", "GLACIAL", "ARID", "LOWLAND", "FLUVIAL"]
PROV_COLS = ["#20344f", "#dbe9f2", "#d9b06a", "#9dc27a", "#4a7f57"]

ls = LightSource(azdeg=292.5, altdeg=45)
shade = ls.hillshade(np.nan_to_num(elev, nan=float(np.nanmin(elev))),
                     vert_exag=3.0, dx=PX_M, dy=PX_M)

fig, axes = plt.subplots(1, 2, figsize=(22, 11.6), dpi=130)
for ax, (lab, ids, names, cols) in zip(axes, [
        ("BIOMES", biome, [b[0] for b in BIOMES], [b[1] for b in BIOMES]),
        ("PROVINCES (bake erosion rules)", prov, PROV_NAMES, PROV_COLS)]):
    cmap = ListedColormap([c if isinstance(c, str) else c for c in cols])
    rgb = cmap(np.clip(ids, 0, len(cols)-1))[..., :3]
    rgb = rgb * (0.45 + 0.55 * shade[..., None])          # multiply by hillshade
    ax.imshow(rgb, origin="upper", interpolation="nearest")
    ax.set_title(f"{lab}   seed 20260719   261 x 261 km @ {PX_M:.0f} m/px", fontsize=13)
    ax.set_axis_off()
    tot = max(int((ids > 0).sum()), 1)
    handles = [Patch(facecolor=cols[i], edgecolor="k", lw=.4,
                     label=f"{names[i]}  {100.0*(ids==i).sum()/tot:.1f}%")
               for i in range(1, len(names))]
    ax.legend(handles=handles, loc="lower left", fontsize=9, framealpha=.9)
fig.tight_layout()
fig.savefig(OUT, dpi=130, facecolor="white")
print(f"wrote {OUT}")
for i, nm in enumerate(PROV_NAMES):
    if i:
        print(f"  province {nm:<8} {100.0*(prov==i).sum()/max((prov>0).sum(),1):6.2f}%")
