"""Temperature across the world, and where a snow system would actually matter.

TWO PANELS, because mean annual temperature does NOT answer the snow question.

  LEFT  -- mean annual temperature, the `temperature` channel (bio_1) as the
           tiles carry it. This is what the game reads today.
  RIGHT -- estimated COLDEST-MONTH temperature, derived from bio_1 and bio_4.

DERIVATION AND ITS ASSUMPTION, stated because it is an estimate, not data.
bio_4 is the standard deviation of the twelve monthly means, x100. For an
annual cycle that is roughly sinusoidal, the coldest month sits about 1.4
standard deviations below the annual mean, so

    T_coldest ~= bio_1 - 1.4 * (bio_4 / 100)

That is a WEATHER-GRADE approximation, not a climatology. It is good enough to
answer "does this place freeze in winter" and not good enough to schedule a
storm. A real seasonal cycle would need monthly fields the tiles do not carry.

The snow bands on the right panel are the design-relevant read:
  PERMANENT   mean annual below 0 C          -- snow/ice never fully leaves
  SEASONAL    coldest month below 0 C        -- a snow system is REQUIRED
  RARE        coldest month 0..3 C           -- occasional, cosmetic
  NONE        coldest month above 3 C        -- no snow system needed
"""
import pathlib
import sys

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LightSource, ListedColormap, LinearSegmentedColormap
from matplotlib.patches import Patch

from terrain_service import tile_codec

SEED_DIR, OUT = pathlib.Path(sys.argv[1]), sys.argv[2]
DS = 4
PX_M = 30.0 * DS

files = sorted(SEED_DIR.glob("*.vxtl"))
txs = sorted({int(f.stem.split("_")[0]) for f in files})
tys = sorted({int(f.stem.split("_")[1]) for f in files})
n = 512 // DS
W, H = len(txs) * n, len(tys) * n
elev = np.full((H, W), np.nan, np.float32)
tmean = np.zeros((H, W), np.float32)
tsd = np.zeros((H, W), np.float32)
for f in files:
    tx, ty = (int(v) for v in f.stem.split("_"))
    t = tile_codec.decode(f.read_bytes())
    sl = (slice((ty-tys[0])*n, (ty-tys[0]+1)*n), slice((tx-txs[0])*n, (tx-txs[0]+1)*n))
    elev[sl] = t.elevation.astype(np.float32)[::DS, ::DS]
    tmean[sl] = t.climate[0][::DS, ::DS] / 255.0 * 80.0 - 40.0
    tsd[sl] = t.climate[1][::DS, ::DS] / 255.0 * 3000.0 / 100.0   # bio_4 -> deg C

tcold = tmean - 1.4 * tsd
land = elev > 0

ls = LightSource(azdeg=292.5, altdeg=45)
shade = ls.hillshade(np.nan_to_num(elev, nan=float(np.nanmin(elev))),
                     vert_exag=3.0, dx=PX_M, dy=PX_M)

# --- left: mean annual temperature ---------------------------------------
tcmap = LinearSegmentedColormap.from_list("temp", [
    "#3b2f6e", "#2f6fb0", "#5fb0d0", "#9fd0a0", "#e8e07a", "#e09a4a", "#c2452d"])
lo, hi = -15.0, 30.0
norm = np.clip((tmean - lo) / (hi - lo), 0, 1)
rgb_t = tcmap(norm)[..., :3] * (0.45 + 0.55 * shade[..., None])
rgb_t[~land] = np.array([0.10, 0.13, 0.22]) * (0.5 + 0.5 * shade[~land, None])

# --- right: snow bands ----------------------------------------------------
BANDS = ["ocean", "PERMANENT", "SEASONAL", "RARE", "NONE"]
BCOL = ["#12203a", "#ffffff", "#8fc4e8", "#cfe3b8", "#c98f5a"]
band = np.zeros_like(tmean, np.int8)
band[land & (tcold > 3.0)] = 4
band[land & (tcold <= 3.0)] = 3
band[land & (tcold < 0.0)] = 2
band[land & (tmean < 0.0)] = 1
rgb_b = ListedColormap(BCOL)(np.clip(band, 0, 4))[..., :3] * (0.45 + 0.55 * shade[..., None])

nl = max(int(land.sum()), 1)
fig, axes = plt.subplots(1, 2, figsize=(21, 11.2), dpi=125)
axes[0].imshow(rgb_t, origin="upper", interpolation="nearest")
axes[0].set_title(f"MEAN ANNUAL TEMPERATURE  (land p5 {np.percentile(tmean[land],5):.1f} / "
                  f"p50 {np.percentile(tmean[land],50):.1f} / p95 {np.percentile(tmean[land],95):.1f} C)",
                  fontsize=12)
sm = plt.cm.ScalarMappable(cmap=tcmap, norm=plt.Normalize(lo, hi))
cb = fig.colorbar(sm, ax=axes[0], fraction=0.046, pad=0.02)
cb.set_label("deg C", fontsize=10)

axes[1].imshow(rgb_b, origin="upper", interpolation="nearest")
axes[1].set_title("WHERE A SNOW SYSTEM MATTERS  (est. coldest month = bio_1 - 1.4 x bio_4)",
                  fontsize=12)
axes[1].legend(handles=[Patch(facecolor=BCOL[i], edgecolor="k", lw=.4,
                              label=f"{BANDS[i]}  {100.0*(band==i).sum()/nl:.1f}% of land")
                        for i in (1, 2, 3, 4)], loc="lower left", fontsize=10, framealpha=.92)
for a in axes:
    a.set_axis_off()
fig.suptitle(f"seed 20260719   |   {len(files)} tiles   |   "
             f"{W*PX_M/1000:.0f} x {H*PX_M/1000:.0f} km @ {PX_M:.0f} m/px", fontsize=11)
fig.tight_layout()
fig.savefig(OUT, dpi=125, facecolor="white")
print("wrote", OUT)
print(f"\nmean annual, land: p5 {np.percentile(tmean[land],5):6.1f}  p50 {np.percentile(tmean[land],50):6.1f}  "
      f"p95 {np.percentile(tmean[land],95):6.1f}  min {tmean[land].min():6.1f}  max {tmean[land].max():6.1f}")
print(f"coldest month, land: p5 {np.percentile(tcold[land],5):6.1f}  p50 {np.percentile(tcold[land],50):6.1f}  "
      f"p95 {np.percentile(tcold[land],95):6.1f}  min {tcold[land].min():6.1f}")
for i in (1, 2, 3, 4):
    print(f"  {BANDS[i]:<10} {100.0*(band==i).sum()/nl:6.2f}% of land")
print(f"\nland above 2000 m that is SEASONAL or PERMANENT: "
      f"{100.0*((band[land & (elev>2000)] <= 2) & (band[land & (elev>2000)] >= 1)).mean():.1f}%")
