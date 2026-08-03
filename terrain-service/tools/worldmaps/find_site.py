"""Find a spawn point that actually IS the biome you want to photograph.

WHY. The first vista sweep picked its nine sites off a downsampled biome map by
eye, and two of them were wrong in ways no one could see until the labels were
recomputed at the spawn column: the "beach" shot spawned at -142 m with ZERO
land inside 6 km -- it is a picture of open water -- and the "fluvial" shot
landed in rainforest already covered by another shot.

A vista site has to satisfy two conditions that a biome map does not show:

  1. the SPAWN COLUMN classifies as the biome you are naming the file for, and
  2. the SURROUNDING FRAME contains the thing worth photographing -- for a
     coastline that means a land fraction near half, not near 0 or 1.

So this scores candidates on both, and additionally prefers relief (a vista of
a flat plain is the shot that started this whole problem).

Usage:
    $PY find_site.py <seed_dir> BEACH [--land-lo 0.3 --land-hi 0.7]
"""
import argparse
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from world_map import _read_constants, classify, BIOMES        # noqa: E402
from terrain_service import tile_codec                          # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("seed_dir")
ap.add_argument("biome")
ap.add_argument("--land-lo", type=float, default=0.30)
ap.add_argument("--land-hi", type=float, default=0.70)
ap.add_argument("--radius-m", type=float, default=6000.0)
ap.add_argument("--top", type=int, default=8)
# A VISTA SEES PAST ITS OWN TILE. voxel-capture.ps1's header: off the generated
# tiles you photograph the flat fallback plane, "which looks like a bug and is
# not". A 1200 m camera at -20 deg has a horizon far beyond --radius-m, so a
# site near the world edge fills its far field with fallback. Keep vista sites
# well inside; the default is ~2 tiles.
ap.add_argument("--min-edge-km", type=float, default=0.0)
args = ap.parse_args()

PX_M, TILE = 30.0, 512
TILE_M = TILE * PX_M
K = _read_constants()
WANT = [i for i, b in enumerate(BIOMES) if b[0] == args.biome.upper()]
if not WANT:
    sys.exit(f"unknown biome {args.biome}; have {[b[0] for b in BIOMES]}")
WANT = WANT[0]

seed = pathlib.Path(args.seed_dir)
files = sorted(seed.glob("*.vxtl"))
txs = sorted({int(f.stem.split("_")[0]) for f in files})
tys = sorted({int(f.stem.split("_")[1]) for f in files})
W, H = len(txs) * TILE, len(tys) * TILE
print(f"stitching {len(files)} tiles -> {W}x{H} at 30 m/px ...", flush=True)

elev = np.full((H, W), np.nan, np.float32)
temp = np.zeros((H, W), np.float32)
seas = np.zeros((H, W), np.float32)
prec = np.zeros((H, W), np.float32)
for f in files:
    tx, ty = (int(v) for v in f.stem.split("_"))
    t = tile_codec.decode(f.read_bytes())
    sl = (slice((ty - tys[0]) * TILE, (ty - tys[0] + 1) * TILE),
          slice((tx - txs[0]) * TILE, (tx - txs[0] + 1) * TILE))
    elev[sl] = t.elevation.astype(np.float32)
    temp[sl] = t.climate[0] / 255.0 * 80.0 - 40.0
    seas[sl] = t.climate[1] / 255.0 * 3000.0
    prec[sl] = t.climate[2] / 255.0 * 12000.0

biome = classify(np.nan_to_num(elev), temp, prec, seas, K)
land = np.isfinite(elev) & (elev > 0)
rad = int(round(args.radius_m / PX_M))

# candidate grid every ~1.9 km, away from the world edge by one view radius
step = 64
cands = []
margin = max(rad, int(round(args.min_edge_km * 1000.0 / PX_M)))
for r in range(margin, H - margin, step):
    for c in range(margin, W - margin, step):
        if biome[r, c] != WANT or not land[r, c]:
            continue
        win = (slice(r - rad, r + rad), slice(c - rad, c + rad))
        lf = float(land[win].mean())
        if not (args.land_lo <= lf <= args.land_hi):
            continue
        e = elev[win]
        el = e[land[win]]
        if el.size < 100:
            continue
        relief = float(np.nanpercentile(el, 99) - np.nanpercentile(el, 1))
        share = float((biome[win] == WANT).mean())
        # want: the named biome well represented, coastline in frame, some relief
        score = share * 2.0 + (1.0 - abs(lf - 0.5) * 2.0) + min(relief / 300.0, 1.0)
        x_m = (txs[0] * TILE + c) * PX_M
        y_m = (tys[0] * TILE + r) * PX_M
        cands.append((score, x_m, y_m, lf, share, relief, float(elev[r, c]),
                      float(temp[r, c]), float(prec[r, c])))

cands.sort(reverse=True)
print(f"\n{len(cands)} candidates for {args.biome.upper()} "
      f"(land {args.land_lo:.0%}-{args.land_hi:.0%} within {args.radius_m/1000:.0f} km)\n")
print(f"{'score':>6} {'-VoxelSpawnAt':>22} {'land%':>6} {'biome%':>7} {'relief':>7} "
      f"{'elev':>6} {'T':>6} {'P':>7}")
seen = []
for c in cands:
    # keep them apart: no two picks within 10 km
    if any(abs(c[1] - s[1]) < 10000 and abs(c[2] - s[2]) < 10000 for s in seen):
        continue
    seen.append(c)
    print(f"{c[0]:6.2f} {f'{c[1]:.0f},{c[2]:.0f}':>22} {100*c[3]:6.1f} {100*c[4]:7.1f} "
          f"{c[5]:7.0f} {c[6]:6.0f} {c[7]:6.1f} {c[8]:7.0f}")
    if len(seen) >= args.top:
        break
