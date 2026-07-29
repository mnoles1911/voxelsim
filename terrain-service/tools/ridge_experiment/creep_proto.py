"""Post-hoc prototype of channel-gated hillslope diffusion (soil creep).

Applies n explicit diffusion steps to a saved S1 window, with per-cell
diffusivity gated OFF where the flow accumulation says "channel":

    D(x) = D0 * (1 - A^q / (A^q + a_crit^q))      (the bake's own soft gate)

Then re-measures the g10 panel, slope ladder, and ridge placement.

Usage: python creep_proto.py CONFIG_TILE a_crit sigma_m [sigma_m ...]
"""
import sys
from pathlib import Path

import numpy as np
from scipy.ndimage import distance_transform_edt

HERE = Path(__file__).resolve().parent
WT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(WT))
from terrain_service.geomorph.geomorphon import geomorphon_field, geomorphon_histogram  # noqa: E402
from terrain_service.geomorph.slope import slope_statistics  # noqa: E402

WIN = HERE / "ridge_out" / "windows"
CELL = 1.875
REF_LADDER = [2.134, 1.943, 1.728, 1.523, 1.301]


def block_mean(z, f):
    if f == 1:
        return z
    h = (z.shape[0] // f) * f
    w = (z.shape[1] // f) * f
    return z[:h, :w].reshape(h // f, f, w // f, f).mean(axis=(1, 3))


def gated_diffuse(z, acc, a_crit, sigma_m, q=2.0):
    """Explicit gated diffusion to an effective smoothing length sigma_m."""
    z = z.astype(np.float64).copy()
    aq = np.power(np.clip(acc, 0, None), q)
    gate = 1.0 - aq / (aq + a_crit ** q)          # 1 on hillslopes, 0 in channels
    # sigma^2 = 2*D*t ; with unit D*dt per step alpha (in cell^2), n steps:
    # sigma_cells^2 = 2 * alpha * n. alpha = 0.2 for stability.
    alpha = 0.2
    n = int(np.ceil((sigma_m / CELL) ** 2 / (2 * alpha)))
    for _ in range(n):
        lap = (np.roll(z, 1, 0) + np.roll(z, -1, 0) + np.roll(z, 1, 1)
               + np.roll(z, -1, 1) - 4 * z)
        lap[0, :] = lap[-1, :] = lap[:, 0] = lap[:, -1] = 0.0
        z += alpha * gate * lap
    return z, n


def panel(z, acc, a_crit, tile):
    g = geomorphon_histogram(z, CELL, search_m=10 * CELL)
    forms = geomorphon_field(z, CELL, search_m=10 * CELL)
    ridge = (forms == 2) | (forms == 3)
    ok = forms > 0
    chan = acc >= a_crit
    d = distance_transform_edt(~chan) * CELL
    ratio = (np.median(d[ridge & ok]) / np.median(d[ok])
             if (ridge & ok).any() else float("nan"))
    lad = [slope_statistics(block_mean(z, f), CELL * f).to_dict()["mean_deg"]
           for f in (1, 2, 4, 8, 16)]
    dev = (max(abs(v / r - 1) for v, r in zip(lad, REF_LADDER))
           if tile == "plains" else float("nan"))
    return (g.frac("ridge", "peak"), g.frac("valley", "pit"), lad[0], ratio, dev,
            lad)


def main():
    name = sys.argv[1]
    a_crit = float(sys.argv[2])
    sigmas = [float(s) for s in sys.argv[3:]]
    tile = name.rsplit("_", 1)[1]
    z0 = np.load(WIN / f"{name}_S1.npy").astype(np.float64)
    acc = np.load(WIN / f"{name}_acc.npy").astype(np.float64)
    print(f"{'sigma':>6s} {'steps':>5s} {'g10 r+p':>8s} {'g10 v+p':>8s} "
          f"{'mean_deg':>8s} {'place':>6s} {'ladder_dev':>10s}")
    r = panel(z0, acc, a_crit, tile)
    print(f"{'0':>6s} {'-':>5s} {r[0]:8.4f} {r[1]:8.4f} {r[2]:8.3f} {r[3]:6.2f} "
          f"{'' if np.isnan(r[4]) else format(r[4]*100, '.1f') + '%':>10s}")
    for s in sigmas:
        z, n = gated_diffuse(z0, acc, a_crit, s)
        r = panel(z, acc, a_crit, tile)
        print(f"{s:6.1f} {n:5d} {r[0]:8.4f} {r[1]:8.4f} {r[2]:8.3f} {r[3]:6.2f} "
              f"{'' if np.isnan(r[4]) else format(r[4]*100, '.1f') + '%':>10s}")
        print(f"       ladder {[round(v, 3) for v in r[5]]}")


if __name__ == "__main__":
    main()
